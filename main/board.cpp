// Board layer: ST7789 LCD (LovyanGFX, same panel settings as the gta-minimap-demo), backlight,
// BOOT button, QMI8658 accelerometer (auto-rotation), CST3530 touch and (for H.264 dumps) microSD.
// One display task owns the LCD; everything else hands it frames or asks for a status redraw.
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <math.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "driver/gpio.h"
#include "app.h"
#include "ui_fonts.h"
#if CONFIG_AAM_DUMP_H264
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#endif

static const char *TAG = "board";

// ---- pins (Waveshare ESP32-S3-Touch-LCD-2.8 demo code) ----------------------
#define PIN_LCD_SCLK 40
#define PIN_LCD_MOSI 45
#define PIN_LCD_CS 42
#define PIN_LCD_DC 41
#define PIN_LCD_RST 39
#define PIN_LCD_BL 5
#define PIN_BOOT 0
#define PIN_IMU_SDA 11
#define PIN_IMU_SCL 10
#define PIN_SD_CLK 14
#define PIN_SD_CMD 17
#define PIN_SD_D0 16
#define PIN_SD_D3 21   // held high so the card stays in SD (not SPI) mode
#define IMU_I2C_PORT 1
#define PIN_TP_SDA 1
#define PIN_TP_SCL 3
#define PIN_TP_INT 4
#define PIN_TP_RST 2
#define TP_I2C_PORT 0
#define TP_ADDR 0x58      // CST3530
#define IMU_ROT_OFFSET 3  // quarter turns between accelerometer axes and panel (verified in the demo)

class LGFX : public lgfx::LGFX_Device {
    lgfx::Panel_ST7789 _panel;
    lgfx::Bus_SPI _bus;
    lgfx::Light_PWM _light;

public:
    LGFX() {
        auto b = _bus.config();
        b.spi_host = SPI3_HOST;
        b.spi_mode = 0;
        b.freq_write = 80000000;
        b.freq_read = 16000000;
        b.spi_3wire = true;
        b.use_lock = true;
        b.dma_channel = SPI_DMA_CH_AUTO;
        b.pin_sclk = PIN_LCD_SCLK;
        b.pin_mosi = PIN_LCD_MOSI;
        b.pin_miso = -1;
        b.pin_dc = PIN_LCD_DC;
        _bus.config(b);
        _panel.setBus(&_bus);

        auto p = _panel.config();
        p.pin_cs = PIN_LCD_CS;
        p.pin_rst = PIN_LCD_RST;
        p.pin_busy = -1;
        p.panel_width = 240;
        p.panel_height = 320;
        p.offset_x = 0;
        p.offset_y = 0;
        p.readable = false;
        p.invert = true;
        p.rgb_order = false;  // BGR panel
        p.dlen_16bit = false;
        p.bus_shared = false;
        _panel.config(p);

        auto l = _light.config();
        l.pin_bl = PIN_LCD_BL;
        l.invert = false;
        l.freq = 44100;
        l.pwm_channel = 7;
        _light.config(l);
        _panel.setLight(&_light);
        setPanel(&_panel);
    }
};

static LGFX lcd;

// ---- frame hand-off ---------------------------------------------------------
// Four PSRAM frames: on the wire, pending, being filled, and the last one shown (kept intact so
// it can be re-pushed with or without the banner on a still map). The newest frame wins.
#define NFB 4
#define STRIP_LINES 16
static uint16_t *s_fb[NFB];
static uint16_t *s_strip[2];          // internal DMA-capable bounce buffers
static volatile int s_pending = -1;   // index waiting for the display task
static volatile int s_showing = -1;   // index being pushed
static volatile int s_last = -1;      // last frame on screen: kept intact so it can be re-pushed
static portMUX_TYPE s_fb_lock = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_display_task;

uint16_t *display_acquire_frame(void) {
    portENTER_CRITICAL(&s_fb_lock);
    int idx = 0;
    for (int i = 0; i < NFB; i++)
        if (i != s_pending && i != s_showing && i != s_last) { idx = i; break; }
    portEXIT_CRITICAL(&s_fb_lock);
    return s_fb[idx];
}

void display_submit_frame(uint16_t *fb) {
    portENTER_CRITICAL(&s_fb_lock);
    for (int i = 0; i < NFB; i++)
        if (s_fb[i] == fb) s_pending = i;
    portEXIT_CRITICAL(&s_fb_lock);
    xTaskNotifyGive(s_display_task);
}

void display_request_status(void) {
    g_app.status_dirty = true;
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

// ---- look: Nunito (anti-aliased VLW), soft sky / navy palette, rounded shapes -----------------
struct palette {
    uint32_t bg0, bg1, card, text, muted, accent, ok, err, pin, track;
};
static const palette PAL_DAY = { 0xF4F7FB, 0xE2EBF7, 0xFFFFFF, 0x23304A, 0x8592A8, 0x4A7FE0, 0x3DBE8B, 0xE5736A, 0xF06E64, 0xDCE4F0 };
static const palette PAL_NIGHT = { 0x10192B, 0x1B2740, 0x1F2D47, 0xE6EBF5, 0x8C9AB5, 0x7AA7FF, 0x6FD3A5, 0xF28B82, 0xFF8A80, 0x344463 };
static const palette &pal(void) { return g_app.night ? PAL_NIGHT : PAL_DAY; }
static uint32_t c888(uint32_t v) { return lgfx::color888(v >> 16, (v >> 8) & 255, v & 255); }

static float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }
static float ease_out_cubic(float t) { t = 1 - t; return 1 - t * t * t; }
static float ease_out_back(float t) { const float c = 1.70158f; t -= 1; return 1 + (c + 1) * t * t * t + c * t * t; }

static uint32_t mix888(uint32_t a, uint32_t b, float t) {  // a -> b
    int r = ((a >> 16) & 255) + (int)((((int)(b >> 16) & 255) - ((int)(a >> 16) & 255)) * t);
    int g = ((a >> 8) & 255) + (int)((((int)(b >> 8) & 255) - ((int)(a >> 8) & 255)) * t);
    int bl = (a & 255) + (int)((((int)b & 255) - ((int)a & 255)) * t);
    return (r << 16) | (g << 8) | bl;
}

static inline uint16_t pack565s(int r, int g, int b) {  // byte-swapped RGB565, as frames are stored
    uint16_t v = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (v >> 8) | (v << 8);
}

// Vertical gradient with a 4x4 ordered dither, so RGB565 shows no bands.
static void fill_gradient(uint16_t *buf, uint32_t top, uint32_t bottom) {
    static const uint8_t bayer[4][4] = { { 0, 8, 2, 10 }, { 12, 4, 14, 6 }, { 3, 11, 1, 9 }, { 15, 7, 13, 5 } };
    for (int y = 0; y < LCD_H; y++) {
        uint32_t c = mix888(top, bottom, y / (float)(LCD_H - 1));
        int r = (c >> 16) & 255, g = (c >> 8) & 255, b = c & 255;
        uint16_t *row = buf + y * LCD_W;
        for (int x = 0; x < LCD_W; x++) {
            int d = bayer[y & 3][x & 3];
            int rr = r + (d >> 1), gg = g + (d >> 2), bb = b + (d >> 1);  // ~1 LSB of 5/6/5-bit noise
            row[x] = pack565s(rr > 255 ? 255 : rr, gg > 255 ? 255 : gg, bb > 255 ? 255 : bb);
        }
    }
}

// Anti-aliased arc of thickness th (round ends) from angle a0 to a1, degrees, 0 = up, clockwise.
static void smooth_arc(LGFX_Sprite &s, int cx, int cy, float r, float th, float a0, float a1, uint32_t color) {
    int n = (int)((a1 - a0) / 5) + 1;
    float px = 0, py = 0;
    for (int i = 0; i <= n; i++) {
        float a = (a0 + (a1 - a0) * i / n) * (float)M_PI / 180;
        float x = cx + r * sinf(a), y = cy - r * cosf(a);
        if (i) s.drawWideLine(px, py, x, y, th / 2, c888(color));
        px = x; py = y;
    }
}

// ---- status screen (rendered off-screen, pushed whole: no visible redraw) ------------------
static LGFX_Sprite s_ui(&lcd);
static uint16_t *s_ui_buf;

static void text_mininav(LGFX_Sprite &s, const uint8_t *font, int x, int y, uint32_t mini, uint32_t nav, bool center) {
    s.loadFont(font);
    s.setTextDatum(lgfx::baseline_left);
    int wm = s.textWidth("Mini"), wn = s.textWidth("Nav");
    if (center) x -= (wm + wn) / 2;
    s.setTextColor(c888(mini));
    s.drawString("Mini", x, y);
    s.setTextColor(c888(nav));
    s.drawString("Nav", x + wm, y);
}

struct chip { int x, y, w, h; };
static const chip CHIPS[3] = { { 12, 178, 92, 50 }, { 114, 178, 92, 50 }, { 216, 178, 92, 50 } };

// A tap on the status screen (LCD coordinates).
static void status_tap(int x, int y) {
    for (int i = 0; i < 3; i++) {
        const chip &k = CHIPS[i];
        if (x < k.x - 4 || x >= k.x + k.w + 4 || y < k.y - 6 || y >= k.y + k.h + 6) continue;
        if (i == 0) app_toggle_touch("screen");
        else if (i == 1) app_toggle_full("screen");
        else app_toggle_night("screen");
    }
}

static void render_status(void) {
    const palette &p = pal();
    LGFX_Sprite &s = s_ui;
    fill_gradient(s_ui_buf, p.bg0, p.bg1);
    text_mininav(s, FONT_HEAD, 18, 28, p.text, p.accent, false);

    s.fillSmoothRoundRect(12, 38, 296, 130, 18, c888(p.card));

    // what is happening, in plain words
    int stage = g_app.stage < STAGE_AP ? STAGE_AP : g_app.stage > STAGE_DISCOVERY ? STAGE_DISCOVERY : g_app.stage;
    char ip_line[40];
    snprintf(ip_line, sizeof ip_line, "Phone %s", g_app.phone_ip);
    char l1[48] = "", l2[48] = "";
    const char *title = "";
    switch (stage) {
    case STAGE_AP:
        title = "Waiting for phone";
        snprintf(l1, sizeof l1, "Join WiFi %s", CONFIG_AAM_AP_SSID);
        snprintf(l2, sizeof l2, "or hotspot %s", CONFIG_AAM_HOTSPOT_SSID);
        break;
    case STAGE_PHONE:
        title = "Phone found";
        snprintf(l1, sizeof l1, "%s", ip_line);
        snprintf(l2, sizeof l2, "Start the headunit server");
        break;
    case STAGE_TCP: title = "Connecting"; snprintf(l1, sizeof l1, "%s", ip_line); break;
    case STAGE_VERSION: title = "Checking version"; snprintf(l1, sizeof l1, "%s", ip_line); break;
    case STAGE_TLS: title = "Securing link"; snprintf(l1, sizeof l1, "%s", ip_line); break;
    default: title = "Starting Auto"; snprintf(l1, sizeof l1, "%s", g_app.phone_name[0] ? g_app.phone_name : ip_line); break;
    }

    // progress ring: one sixth per step
    const int cx = 68, cy = 92;
    const float r = 31.5f, th = 9;
    smooth_arc(s, cx, cy, r, th, 0, 360, p.track);
    smooth_arc(s, cx, cy, r, th, 0, 360.0f * stage / 6, p.accent);
    s.loadFont(FONT_TITLE);
    s.setTextDatum(lgfx::middle_center);
    s.setTextColor(c888(p.text));
    char num[8];
    snprintf(num, sizeof num, "%d/6", stage);
    s.drawString(num, cx, cy + 1);

    s.setTextDatum(lgfx::baseline_left);
    s.drawString(title, 118, 82);
    s.loadFont(FONT_BODY);
    s.setTextColor(c888(p.muted));
    s.drawString(l1, 118, 104);
    if (!g_app.error[0]) s.drawString(l2, 118, 122);

    if (g_app.error[0]) {  // soft pill along the bottom of the card
        s.loadFont(FONT_SMALL);
        s.fillSmoothRoundRect(24, 138, 272, 20, 10, c888(p.err));
        s.setTextDatum(lgfx::middle_center);
        s.setTextColor(c888(0xFFFFFF));
        s.drawString(g_app.error, LCD_W / 2, 148);
    }

    // settings: three tappable pills (the screen is always touchable here)
    for (int i = 0; i < 3; i++) {
        const chip &k = CHIPS[i];
        bool on = i == 0 ? g_app.touch_enabled : i == 1 ? (g_app.full_screen && !g_app.touch_enabled) : g_app.night;
        bool locked = i == 1 && g_app.touch_enabled;
        const char *label = i == 0 ? "Touch" : i == 1 ? "Full screen" : "Theme";
        const char *value = i == 2 ? (g_app.night ? "Dark" : "Light") : locked ? "Needs touch off" : on ? "On" : "Off";
        bool filled = on && i != 2;
        s.fillSmoothRoundRect(k.x, k.y, k.w, k.h, 16, c888(filled ? p.accent : p.card));
        s.setTextDatum(lgfx::baseline_center);
        s.loadFont(FONT_SMALL);
        s.setTextColor(c888(filled ? mix888(p.accent, 0xFFFFFF, 0.8f) : p.muted));
        s.drawString(label, k.x + k.w / 2, k.y + 18);
        s.loadFont(locked ? FONT_SMALL : FONT_BODY);
        s.setTextColor(c888(filled ? 0xFFFFFF : locked ? p.muted : i == 2 ? p.accent : p.text));
        s.drawString(value, k.x + k.w / 2, k.y + 37);
    }
    s.unloadFont();
}

// ---- toast: a soft pill blended into every frame on its way to the LCD ---------------------
// (drawing it over the frame afterwards flickered). Colour sprite + an analytic alpha mask, so the
// rounded edges blend into whatever is underneath without fringes.
#define TOAST_W 200
#define TOAST_H 36
#define TOAST_X ((LCD_W - TOAST_W) / 2)
#define TOAST_Y 12
static LGFX_Sprite s_toast_spr(&lcd);
static uint8_t s_toast_alpha[TOAST_W * TOAST_H];
static bool s_toast_on;   // blend the banner into pushed frames
static char s_toast[48];
static volatile uint32_t s_toast_until;   // millis() deadline, 0 = none
static bool s_toast_shown;

static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

void display_toast(const char *text) {
    snprintf(s_toast, sizeof s_toast, "%s", text);
    s_toast_until = now_ms() + 1500;
    s_toast_shown = false;   // re-render even if a banner is already up
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

static void render_toast(void) {
    if (!s_toast_spr.getBuffer()) {
        s_toast_spr.setColorDepth(16);
        s_toast_spr.setPsram(true);   // internal RAM is tight
        s_toast_spr.createSprite(TOAST_W, TOAST_H);
        // pill coverage: distance to a stadium shape, 1 px soft edge
        const float rad = TOAST_H / 2.0f;
        for (int y = 0; y < TOAST_H; y++)
            for (int x = 0; x < TOAST_W; x++) {
                float px = x + 0.5f, py = y + 0.5f;
                float qx = px < rad ? rad - px : px > TOAST_W - rad ? px - (TOAST_W - rad) : 0;
                float qy = py - rad;
                float d = sqrtf(qx * qx + qy * qy) - rad;   // < 0 inside
                s_toast_alpha[y * TOAST_W + x] = (uint8_t)(clamp01(0.5f - d) * 235);  // a touch see-through
            }
    }
    s_toast_spr.fillScreen(c888(0x1E2940));
    s_toast_spr.loadFont(FONT_TITLE);
    s_toast_spr.setTextDatum(lgfx::middle_center);
    s_toast_spr.setTextColor(c888(0xFFFFFF));
    s_toast_spr.drawString(s_toast, TOAST_W / 2, TOAST_H / 2 + 1);
    s_toast_spr.unloadFont();
}

static inline uint16_t blend565s(uint16_t dst, uint16_t src, int a) {  // both byte-swapped
    dst = (dst >> 8) | (dst << 8);
    src = (src >> 8) | (src << 8);
    int r = ((dst >> 11) * (255 - a) + (src >> 11) * a) / 255;
    int g = (((dst >> 5) & 63) * (255 - a) + ((src >> 5) & 63) * a) / 255;
    int b = ((dst & 31) * (255 - a) + (src & 31) * a) / 255;
    uint16_t v = (r << 11) | (g << 5) | b;
    return (v >> 8) | (v << 8);
}

static void overlay_toast(uint16_t *strip, int y0, int lines) {
    const uint16_t *src = (const uint16_t *)s_toast_spr.getBuffer();
    if (!src) return;
    for (int l = 0; l < lines; l++) {
        int ty = y0 + l - TOAST_Y;
        if (ty < 0 || ty >= TOAST_H) continue;
        const uint16_t *sp = src + ty * TOAST_W;
        const uint8_t *ap = s_toast_alpha + ty * TOAST_W;
        uint16_t *dp = strip + l * LCD_W + TOAST_X;
        for (int x = 0; x < TOAST_W; x++)
            if (ap[x]) dp[x] = blend565s(dp[x], sp[x], ap[x]);
    }
}

// Waits for the SPI DMA by sleeping a tick at a time instead of spinning: the display task
// outranks the decoder on core 1, and spinning through a 15 ms frame push stole that time from it.
static inline void dma_idle(bool yield) {
    if (yield) while (lcd.dmaBusy()) vTaskDelay(1);
    else lcd.waitDMA();
}

static void push_frame(const uint16_t *fb, bool yield = true) {
    // Copy PSRAM -> internal strip, DMA it out while the next strip is copied.
    int which = 0;
    for (int y = 0; y < LCD_H; y += STRIP_LINES) {
        int lines = LCD_H - y < STRIP_LINES ? LCD_H - y : STRIP_LINES;
        memcpy(s_strip[which], fb + y * LCD_W, lines * LCD_W * 2);  // its last DMA finished before the previous push
        if (s_toast_on) overlay_toast(s_strip[which], y, lines);
        dma_idle(yield);
        lcd.pushImageDMA(0, y, LCD_W, lines, (lgfx::swap565_t *)s_strip[which]);
        which ^= 1;
    }
    dma_idle(yield);
}

// ---- boot animation -------------------------------------------------------------------------
// ~2.7 s while WiFi and the phone link come up on the other core, so it adds no boot time:
// a route glides in, a map pin drops onto its end with a soft ripple, "MiniNav" fades in, then
// everything crossfades into the status screen. Drawn into spare PSRAM frames.
static void blend_frames(uint16_t *dst, const uint16_t *a, const uint16_t *b, int alpha) {
    for (int i = 0; i < LCD_W * LCD_H; i++) dst[i] = blend565s(a[i], b[i], alpha);
}

static void boot_animation(void) {
    // `base` holds what only grows (background + route so far), `frm` is base + this frame's pin,
    // ripple and title. Only new route segments are drawn per frame.
    static LGFX_Sprite base(&lcd), frm(&lcd);
    uint16_t *base_buf = s_fb[NFB - 2], *frm_buf = s_fb[NFB - 1];
    s_last = NFB - 1;  // no video at boot, but keep the frame pool away from these anyway
    base.setColorDepth(16);
    base.setBuffer(base_buf, LCD_W, LCD_H, 16);
    frm.setColorDepth(16);
    frm.setBuffer(frm_buf, LCD_W, LCD_H, 16);

    const palette &p = pal();
    fill_gradient(base_buf, p.bg0, p.bg1);
    frm.loadFont(FONT_HERO);

    // route: cubic Bezier from the bottom-left corner to the pin tip
    const float P[4][2] = { { -10, 235 }, { 40, 150 }, { 110, 100 }, { 160, 100 } };
    auto bez = [&](float t, float &x, float &y) {
        float u = 1 - t;
        x = u * u * u * P[0][0] + 3 * u * u * t * P[1][0] + 3 * u * t * t * P[2][0] + t * t * t * P[3][0];
        y = u * u * u * P[0][1] + 3 * u * u * t * P[1][1] + 3 * u * t * t * P[2][1] + t * t * t * P[3][1];
    };
    const int N = 60, PIN_X = 160, PIN_TIP = 100, PIN_R = 17;
    int drawn = 0;  // route segments already in `base`

    int64_t t0 = esp_timer_get_time();
    int frames = 0;
    for (;;) {
        float ms = (esp_timer_get_time() - t0) / 1000.0f;
        if (ms > 2400) break;

        // 1) route glides in (0-800 ms)
        int want = (int)(ease_out_cubic(clamp01(ms / 800)) * N + 0.5f);
        for (; drawn < want; drawn++) {
            float x0, y0, x1, y1;
            bez((float)drawn / N, x0, y0);
            bez((float)(drawn + 1) / N, x1, y1);
            base.drawWideLine(x0, y0, x1, y1, 3.5f, c888(p.accent));
        }
        memcpy(frm_buf, base_buf, LCD_W * LCD_H * 2);

        // 2) soft ripple where the pin lands (1000-1900 ms)
        float rp = clamp01((ms - 1000) / 900);
        if (rp > 0 && rp < 1) {
            float rx = 10 + 42 * ease_out_cubic(rp), ry = rx * 0.35f;
            uint32_t col = mix888(p.accent, mix888(p.bg0, p.bg1, 0.4f), rp);
            float px = 0, py = 0;
            for (int i = 0; i <= 36; i++) {
                float a = i * 2 * (float)M_PI / 36;
                float x = PIN_X + rx * cosf(a), y = PIN_TIP + ry * sinf(a);
                if (i) frm.drawWideLine(px, py, x, y, 1.0f, c888(col));
                px = x; py = y;
            }
        }

        // 3) pin drops onto the end of the route (600-1150 ms): a smooth teardrop
        float pt = clamp01((ms - 600) / 550);
        if (pt > 0) {
            float lift = (1 - ease_out_back(pt)) * 70;
            int tip = PIN_TIP - (int)lift, cy = tip - 28;
            frm.drawWedgeLine(PIN_X, tip - 1, PIN_X, cy, 1.0f, PIN_R * 0.82f, c888(p.pin));
            frm.fillSmoothCircle(PIN_X, cy, PIN_R, c888(p.pin));
            frm.fillSmoothCircle(PIN_X, cy, 6, c888(0xFFFFFF));
        }

        // 4) "MiniNav" fades and rises in (1000-1700 ms)
        float tt = ease_out_cubic(clamp01((ms - 1000) / 700));
        if (tt > 0) {
            uint32_t under = mix888(p.bg0, p.bg1, 0.7f);
            frm.setTextDatum(lgfx::baseline_left);
            int wm = frm.textWidth("Mini"), wn = frm.textWidth("Nav");
            int x = (LCD_W - wm - wn) / 2, y = 166 + (int)((1 - tt) * 10);
            frm.setTextColor(c888(mix888(under, p.text, tt)));
            frm.drawString("Mini", x, y);
            frm.setTextColor(c888(mix888(under, p.accent, tt)));
            frm.drawString("Nav", x + wm, y);
        }

        push_frame(frm_buf, false);  // nothing else needs core 1 yet: spin on the DMA for speed
        frames++;
    }
    frm.unloadFont();

    // 5) crossfade into the status screen (300 ms)
    render_status();
    memcpy(base_buf, frm_buf, LCD_W * LCD_H * 2);
    int64_t f0 = esp_timer_get_time();
    for (;;) {
        float k = (esp_timer_get_time() - f0) / 300000.0f;
        if (k >= 1) break;
        blend_frames(frm_buf, base_buf, s_ui_buf, (int)(ease_out_cubic(k) * 255));
        push_frame(frm_buf, false);
        frames++;
    }
    push_frame(s_ui_buf, false);
    base.deleteSprite();  // detach; the buffers belong to the frame pool
    frm.deleteSprite();
    g_app.status_dirty = false;
    ESP_LOGI(TAG, "boot animation: %d frames", frames);
}

// Console "shot": prints what is on screen (the status canvas, or the last video frame) as hex
// rows, for tools/screenshot.py. Lets the UI be checked from the Mac without a camera.
void display_dump(void) {
    const uint16_t *fb = (g_app.stage >= STAGE_VIDEO || g_app.demo) && s_last >= 0 ? s_fb[s_last] : s_ui_buf;
    static char line[LCD_W * 4 + 16];
    for (int y = 0; y < LCD_H; y++) {
        int n = sprintf(line, "SHOT %03d ", y);
        for (int x = 0; x < LCD_W; x++) n += sprintf(line + n, "%04X", fb[y * LCD_W + x]);
        puts(line);
        if ((y & 15) == 15) vTaskDelay(1);
    }
}

static void show_status(void) {
    render_status();
    s_toast_on = s_toast_until != 0;
    push_frame(s_ui_buf);
}

static void display_task(void *) {
    lcd.startWrite();
    boot_animation();
    int last_rot = g_app.rotation;
    for (;;) {
        // Sleep until there is work; wake every 100 ms only while a banner has to time out.
        ulTaskNotifyTake(pdTRUE, s_toast_until ? pdMS_TO_TICKS(100) : portMAX_DELAY);
        if (g_app.rotation != last_rot) {
            last_rot = g_app.rotation;
            lcd.endWrite();
            lcd.setRotation(last_rot);
            lcd.startWrite();
            g_app.status_dirty = true;
        }
        int idx;
        portENTER_CRITICAL(&s_fb_lock);
        idx = s_pending;
        if (idx >= 0) { s_showing = idx; s_pending = -1; }
        portEXIT_CRITICAL(&s_fb_lock);
        bool video = g_app.stage >= STAGE_VIDEO || g_app.demo;
        if (idx >= 0 && video) {
            push_frame(s_fb[idx]);
            s_last = idx;
        }
        if (idx >= 0) {
            portENTER_CRITICAL(&s_fb_lock);
            s_showing = -1;
            portEXIT_CRITICAL(&s_fb_lock);
        }
        if (g_app.status_dirty && !video) {
            g_app.status_dirty = false;
            show_status();
        }
        if (s_toast_until && !s_toast_shown) {   // new banner: show it now, not at the next frame
            s_toast_shown = true;
            render_toast();
            s_toast_on = true;
            if (video && s_last >= 0) push_frame(s_fb[s_last]);
            else if (!video) push_frame(s_ui_buf);
        }
        if (s_toast_until && (int32_t)(now_ms() - s_toast_until) >= 0) {
            s_toast_until = 0;
            s_toast_shown = false;
            s_toast_on = false;
            if (video && s_last >= 0) push_frame(s_fb[s_last]);
            else if (!video) push_frame(s_ui_buf);
        }
    }
}

// ---- IMU: auto-rotation --------------------------------------
static uint8_t imu_addr;
static float gx, gy, gz;

static bool imu_write(uint8_t reg, uint8_t val) {
    uint8_t b[2] = { reg, val };
    return lgfx::i2c::transactionWrite(IMU_I2C_PORT, imu_addr, b, 2, 400000).has_value();
}

static bool imu_read(uint8_t reg, uint8_t *buf, size_t n) {
    return lgfx::i2c::transactionWriteRead(IMU_I2C_PORT, imu_addr, &reg, 1, buf, n, 400000).has_value();
}

static void imu_init(void) {
    lgfx::i2c::init(IMU_I2C_PORT, PIN_IMU_SDA, PIN_IMU_SCL);
    uint8_t who = 0;
    for (uint8_t a : { 0x6B, 0x6A }) {
        imu_addr = a;
        if (imu_read(0x00, &who, 1) && who == 0x05) break;
        imu_addr = 0;
    }
    if (!imu_addr) { ESP_LOGW(TAG, "QMI8658 not found, rotation fixed"); return; }
    imu_write(0x02, 0x60);  // CTRL1: auto-increment, little endian
    imu_write(0x03, 0x16);  // CTRL2: accel +-4g, ~125 Hz (only gravity is needed now)
    imu_write(0x08, 0x01);  // CTRL7: accel on
    ESP_LOGI(TAG, "QMI8658 at 0x%02X", imu_addr);
}

static uint32_t millis(void) { return (uint32_t)(esp_timer_get_time() / 1000); }

// Landscape rotation (1 or 3) for the current gravity, or -1 while flat, portrait or ambiguous.
// The Android Auto picture is landscape, so portrait holds keep the last landscape rotation.
static int rotation_from_gravity(void) {
    float ax = fabsf(gx), ay = fabsf(gy);
    if (fmaxf(ax, ay) < 0.5f || fabsf(ax - ay) < 0.25f) return -1;
    int r = ay > ax ? (gy > 0 ? 0 : 2) : (gx > 0 ? 3 : 1);
    r = (r + IMU_ROT_OFFSET) & 3;
    return (r & 1) ? r : -1;
}

// Road bumps are kept out of the gravity estimate: a sample that jumps more than this from the
// previous one is skipped. (A double knock used to toggle day/night; bumps triggered it.)
static constexpr float JOLT_G = 0.30f;

// 20 Hz is plenty for auto-rotation.
static void imu_poll(void) {
    static uint32_t t_poll, t_cand;
    static int cand = -1;
    static float px, py, pz;
    if (!imu_addr) return;
    uint32_t now = millis();
    if (now - t_poll < 50) return;
    t_poll = now;
    uint8_t b[6];
    if (!imu_read(0x35, b, 6)) return;
    const float k = 1.0f / 8192;
    float x = (int16_t)(b[0] | b[1] << 8) * k, y = (int16_t)(b[2] | b[3] << 8) * k, z = (int16_t)(b[4] | b[5] << 8) * k;
    float jerk = sqrtf((x - px) * (x - px) + (y - py) * (y - py) + (z - pz) * (z - pz));
    px = x; py = y; pz = z;
    if (jerk > JOLT_G) return;
    gx += (x - gx) * 0.3f; gy += (y - gy) * 0.3f; gz += (z - gz) * 0.3f;
    int r = rotation_from_gravity();
    if (r < 0 || r == g_app.rotation) { cand = -1; return; }
    if (r != cand) { cand = r; t_cand = now; }
    if (now - t_cand < 400) return;
    cand = -1;
    g_app.rotation = r;
    if (s_display_task) xTaskNotifyGive(s_display_task);
}

// ---- CST3530 touch -> Android Auto taps and drags --------------------------------------
// This board's touch chip is a Hynitron CST3530 at I2C 0x58 with 32-bit register addresses (not
// the CST328 at 0x1A that Waveshare's code expects). Protocol from the CST353X Linux driver in
// gl-inet/gl-openwrt (package/kernel/cst353x): write D0070000, read 9 bytes ([0..1] checksum =
// 0x55 + bytes 4..8; byte 8 high nibble 1 = down, 2 = held, 0 = up; x/y 12-bit in 4, 5, 7), then
// write D00002AB to end the frame.
static bool s_tp_ok;

static bool cst3530_frame(uint8_t *raw, bool *acked = nullptr) {
    static const uint8_t reg[4] = { 0xD0, 0x07, 0x00, 0x00 };
    static const uint8_t end[4] = { 0xD0, 0x00, 0x02, 0xAB };
    bool ok = lgfx::i2c::transactionWriteRead(TP_I2C_PORT, TP_ADDR, reg, 4, raw, 9, 400000).has_value();
    lgfx::i2c::transactionWrite(TP_I2C_PORT, TP_ADDR, end, 4, 400000);
    if (acked) *acked = ok;
    if (!ok) return false;
    uint16_t sum = 0x55 + raw[4] + raw[5] + raw[6] + raw[7] + raw[8];
    return (raw[0] | (raw[1] << 8)) == sum;
}

static void touch_init(void) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_TP_INT;
    io.mode = GPIO_MODE_INPUT;
    gpio_config(&io);
    io.pin_bit_mask = 1ULL << PIN_TP_RST;
    io.mode = GPIO_MODE_OUTPUT;
    gpio_config(&io);
    lgfx::i2c::init(TP_I2C_PORT, PIN_TP_SDA, PIN_TP_SCL);
    gpio_set_level((gpio_num_t)PIN_TP_RST, 0); vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)PIN_TP_RST, 1); vTaskDelay(pdMS_TO_TICKS(100));
    // With no finger the chip answers with an all-zero frame (bad checksum): an ACK is presence.
    uint8_t raw[9];
    for (int i = 0; i < 5 && !s_tp_ok; i++) {
        cst3530_frame(raw, &s_tp_ok);
        if (!s_tp_ok) vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (s_tp_ok) ESP_LOGI(TAG, "touch CST3530 found");
    else ESP_LOGW(TAG, "touch controller not found");
}

// Raw touch is in the panel's portrait frame (240x320); map it to the current rotation.
static void touch_to_screen(int rx, int ry, int rot, int &x, int &y) {
    const int w0 = 240, h0 = 320;
    switch (rot & 3) {
    case 0: x = rx; y = ry; break;
    case 1: x = ry; y = w0 - 1 - rx; break;
    case 2: x = w0 - 1 - rx; y = h0 - 1 - ry; break;
    default: x = h0 - 1 - ry; y = rx; break;
    }
}

// Reads only while the chip pulls INT low, as the Linux driver does: polling and acknowledging the
// frame register restarts the chip's contact tracking and turns a drag into a string of taps.
// The status screen is always touchable (its setting buttons); Android Auto gets touches only while
// video is up and the Touch setting is on.
static void touch_poll(void) {
    static uint32_t t_move, t_last;
    static bool down, ui_down;
    static int ax, ay;
    if (!s_tp_ok) return;
    bool video = g_app.stage >= STAGE_VIDEO || g_app.demo;
    bool to_aa = video && g_app.touch_enabled;
    if (!to_aa && down) { down = false; aap_send_touch(1, ax, ay); }
    if (video && !to_aa) return;
    uint32_t now = millis();
    if (gpio_get_level((gpio_num_t)PIN_TP_INT) != 0) {
        if (down && now - t_last > 300) { down = false; aap_send_touch(1, ax, ay); }  // lost the lift
        if (now - t_last > 300) ui_down = false;
        return;
    }
    uint8_t raw[9];
    if (!cst3530_frame(raw)) return;
    int ev = raw[8] >> 4;
    t_last = now;
    int lx = 0, ly = 0;
    bool pressed = ev == 1 || ev == 2;
    if (pressed) {
        int rx = raw[4] + ((raw[7] & 0x0F) << 8);
        int ry = raw[5] + ((raw[7] & 0xF0) << 4);
        if (rx >= 240 || ry >= 320) return;
        touch_to_screen(rx, ry, g_app.rotation, lx, ly);
    }
    if (!video) {  // status screen: one action per finger press
        if (pressed && !ui_down) { ui_down = true; status_tap(lx, ly); }
        if (!pressed) ui_down = false;
        return;
    }
    if (pressed) {
        ax = lx * 2 + (g_video_crop_x - g_video_margin_w / 2);  // Android Auto UI coordinates (no margin; past a hidden left rail)
        ay = ly * 2;
        if (!down) {
            down = true; t_move = now;
            aap_send_touch(0, ax, ay);
        } else if (now - t_move >= 33) {
            t_move = now;
            aap_send_touch(2, ax, ay);
        }
    } else if (down) {
        down = false;
        aap_send_touch(1, ax, ay);
    }
}

// ---- BOOT button -----------------------------------------------------------------------------
// Single press = day/night, quick double press = touch on/off, hold 1 s = demo.
// A single press waits 350 ms for a possible second press, so the two never both fire.
static void button_poll(void) {
    static bool was_down, held, pending;
    static uint32_t t_down, t_release;
    bool down = gpio_get_level((gpio_num_t)PIN_BOOT) == 0;
    uint32_t now = millis();
    if (down && !was_down) { t_down = now; held = false; }
    if (down && !held && now - t_down > 1000) { held = true; pending = false; app_toggle_demo("BOOT hold"); }
    if (!down && was_down && !held && now - t_down > 30) {
        if (pending && now - t_release < 350) {
            pending = false;
            app_toggle_touch("BOOT double press");
        } else {
            pending = true;
            t_release = now;
        }
    }
    if (pending && !down && now - t_release >= 350) {
        pending = false;
        app_toggle_night("BOOT");
    }
    was_down = down;
}

void board_poll_inputs(void) {
    button_poll();
    imu_poll();
    touch_poll();
}

// ---- microSD (1-bit SDMMC), only built for H.264 dumps ------------------------------------
#if CONFIG_AAM_DUMP_H264
static bool s_sd;
bool sd_ready(void) { return s_sd; }

static void sd_init(void) {
    gpio_set_direction((gpio_num_t)PIN_SD_D3, GPIO_MODE_OUTPUT);
    gpio_set_level((gpio_num_t)PIN_SD_D3, 1);
    esp_vfs_fat_sdmmc_mount_config_t mount = {};
    mount.format_if_mount_failed = false;
    mount.max_files = 4;
    mount.allocation_unit_size = 16 * 1024;
    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 1;
    slot.clk = (gpio_num_t)PIN_SD_CLK;
    slot.cmd = (gpio_num_t)PIN_SD_CMD;
    slot.d0 = (gpio_num_t)PIN_SD_D0;
    slot.d1 = GPIO_NUM_NC;
    slot.d2 = GPIO_NUM_NC;
    slot.d3 = GPIO_NUM_NC;
    slot.flags |= SDMMC_SLOT_FLAG_INTERNAL_PULLUP;
    sdmmc_card_t *card;
    esp_err_t err = esp_vfs_fat_sdmmc_mount("/sdcard", &host, &slot, &mount, &card);
    s_sd = err == ESP_OK;
    if (s_sd) ESP_LOGI(TAG, "microSD mounted for dumps");
    else ESP_LOGW(TAG, "no microSD for dumps (%s)", esp_err_to_name(err));
}
#else
bool sd_ready(void) { return false; }
static void sd_init(void) {}
#endif

void board_init(void) {
    gpio_config_t io = {};
    io.pin_bit_mask = 1ULL << PIN_BOOT;
    io.mode = GPIO_MODE_INPUT;
    io.pull_up_en = GPIO_PULLUP_ENABLE;
    gpio_config(&io);

    lcd.init();
    if (g_app.rotation != 1 && g_app.rotation != 3) g_app.rotation = 1;
    lcd.setRotation(g_app.rotation);
    lcd.setBrightness(255);

    for (int i = 0; i < NFB; i++) {
        s_fb[i] = (uint16_t *)heap_caps_calloc(LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);
        assert(s_fb[i]);
    }
    s_ui_buf = (uint16_t *)heap_caps_calloc(LCD_W * LCD_H, 2, MALLOC_CAP_SPIRAM);
    assert(s_ui_buf);
    s_ui.setColorDepth(16);
    s_ui.setBuffer(s_ui_buf, LCD_W, LCD_H, 16);
    for (int i = 0; i < 2; i++) {
        s_strip[i] = (uint16_t *)heap_caps_malloc(LCD_W * STRIP_LINES * 2, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
        assert(s_strip[i]);
    }
    imu_init();
    touch_init();
    sd_init();
    g_app.status_dirty = true;
    xTaskCreatePinnedToCore(display_task, "display", 6144, NULL, 6, &s_display_task, 1);
}
