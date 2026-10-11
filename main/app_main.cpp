// MiniNav: Waveshare ESP32-S3 2.8" board as a WiFi Android Auto head unit that shows the
// Google Maps projection. See README.md for the phone setup.
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "app.h"

static const char *TAG = "main";
aam_state_t g_app;

extern uint32_t g_aap_rx_bytes, g_aap_decrypt_us, g_aap_frames;
extern uint32_t g_vid_frames, g_vid_dropped, g_vid_decode_us, g_vid_scale_us, g_vid_errors;

static const char *stage_name(int s) {
    static const char *names[] = { "boot", "ap", "phone", "tcp", "version", "tls", "discovery", "video" };
    return s >= 0 && s <= STAGE_VIDEO ? names[s] : "?";
}

void app_set_stage(int stage) {
    if (g_app.stage != stage) ESP_LOGI(TAG, "stage %s -> %s", stage_name(g_app.stage), stage_name(stage));
    g_app.stage = stage;
    display_request_status();
}

void app_set_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_app.error, sizeof g_app.error, fmt, ap);
    va_end(ap);
    display_request_status();
}

static void save_prefs(void) {
    nvs_handle_t h;
    if (nvs_open("mininav", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "night", g_app.night);
    nvs_set_u8(h, "touch", g_app.touch_enabled);
    nvs_set_u8(h, "full", g_app.full_screen);
    nvs_commit(h);
    nvs_close(h);
}

void app_toggle_night(const char *why) {
    g_app.night = !g_app.night;
    ESP_LOGI(TAG, "%s -> %s", why, g_app.night ? "night" : "day");
    aap_send_night(g_app.night);
    save_prefs();
    display_request_status();
}

// Touch for Android Auto needs the app rail on screen, so touch on means full screen off. Both
// change the layout Android Auto is told at connection time, so a live session reconnects.
void app_toggle_touch(const char *why) {
    g_app.touch_enabled = !g_app.touch_enabled;
    ESP_LOGI(TAG, "%s -> touch %s", why, g_app.touch_enabled ? "on" : "off");
    save_prefs();
    if (g_app.stage >= STAGE_VIDEO) display_toast(g_app.touch_enabled ? "Touch ON" : "Touch OFF");
    if (CONFIG_AAM_HIDE_RAIL && g_app.full_screen) aap_restart_session();
    display_request_status();
}

void app_toggle_full(const char *why) {
    if (g_app.touch_enabled) return;  // the rail stays while touch is on
    g_app.full_screen = !g_app.full_screen;
    ESP_LOGI(TAG, "%s -> full screen %s", why, g_app.full_screen ? "on" : "off");
    save_prefs();
    if (g_app.stage >= STAGE_VIDEO) display_toast(g_app.full_screen ? "Full screen" : "App bar shown");
    if (CONFIG_AAM_HIDE_RAIL) aap_restart_session();
    display_request_status();
}

void app_toggle_demo(const char *why) {
    g_app.demo = !g_app.demo;
    ESP_LOGI(TAG, "%s -> demo %s", why, g_app.demo ? "on" : "off");
    video_reset();
    display_request_status();
}

static void console_poll(void) {
    static char line[32];
    static int len;
    uint8_t c;
    while (usb_serial_jtag_read_bytes(&c, 1, 0) == 1) {
        if (c == '\r' || c == '\n') {
            line[len] = 0;
            if (!strcmp(line, "demo")) app_toggle_demo("console");
            else if (!strcmp(line, "night")) app_toggle_night("console");
            else if (!strcmp(line, "touch")) app_toggle_touch("console");
            else if (!strcmp(line, "full")) app_toggle_full("console");
            else if (!strcmp(line, "shot")) display_dump();
            else if (!strcmp(line, "status"))
                ESP_LOGI(TAG, "stage %s, phone %s, error \"%s\"", stage_name(g_app.stage), g_app.phone_ip, g_app.error);
            else if (len) ESP_LOGI(TAG, "commands: demo, night, touch, full, shot, status");
            len = 0;
        } else if (len < (int)sizeof line - 1) {
            line[len++] = c;
        }
    }
}

static void stats(void) {
    static int64_t last;
    static uint32_t f0, d0, dec0, sc0, rx0, dc0, fr0;
    int64_t now = esp_timer_get_time();
    if (now - last < 10000000) return;  // every 10 s
    float sec = (now - last) / 1e6f;
    last = now;
    uint32_t f = g_vid_frames - f0, dec = g_vid_decode_us - dec0, sc = g_vid_scale_us - sc0;
    uint32_t rx = g_aap_rx_bytes - rx0, dc = g_aap_decrypt_us - dc0, fr = g_aap_frames - fr0, dr = g_vid_dropped - d0;
    f0 = g_vid_frames; d0 = g_vid_dropped; dec0 = g_vid_decode_us; sc0 = g_vid_scale_us;
    rx0 = g_aap_rx_bytes; dc0 = g_aap_decrypt_us; fr0 = g_aap_frames;
    if (f)  // only while video is playing
        ESP_LOGI(TAG, "[perf] %s fps=%.1f decode=%.1fms scale=%.1fms | rx=%.0fKB/s frames=%u decrypt=%.2fms/frame | dropped=%u errors=%u | psram %uKB internal %uKB",
                 g_app.demo ? "demo" : stage_name(g_app.stage), f / sec, f ? dec / 1000.0f / f : 0, f ? sc / 1000.0f / f : 0,
                 rx / 1024.0f / sec, (unsigned)fr, fr ? dc / 1000.0f / fr : 0, (unsigned)dr, (unsigned)g_vid_errors,
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10));
}

extern "C" void app_main(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    g_app.full_screen = true;   // defaults; saved settings override
    nvs_handle_t h;
    if (nvs_open("mininav", NVS_READONLY, &h) == ESP_OK) {
        uint8_t v = 0;
        if (nvs_get_u8(h, "night", &v) == ESP_OK) g_app.night = v;
        if (nvs_get_u8(h, "touch", &v) == ESP_OK) g_app.touch_enabled = v;
        if (nvs_get_u8(h, "full", &v) == ESP_OK) g_app.full_screen = v;
        nvs_close(h);
    }
    g_app.rotation = 1;
    ESP_LOGI(TAG, "MiniNav starting, free PSRAM %u KB", (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

    usb_serial_jtag_driver_config_t uj = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
#if CONFIG_AAM_SERIAL_TRANSPORT
    uj.tx_buffer_size = 16384;   // whole test packets must fit
    uj.rx_buffer_size = 16384;
#else
    uj.tx_buffer_size = 2048;    // logs only; internal RAM is tight
    uj.rx_buffer_size = 256;     // console commands
#endif
    usb_serial_jtag_driver_install(&uj);
    usb_serial_jtag_vfs_use_driver();  // ordered logs; with no USB host attached they are dropped, never block

    // Library chatter off; our own tags stay at INFO.
    static const char *quiet[] = { "wifi", "wifi_init", "phy_init", "pp", "net80211", "esp_netif_lwip", "H264_DEC.SW" };
    for (const char *t : quiet) esp_log_level_set(t, ESP_LOG_WARN);

    board_init();
    video_start();
    demo_start();
    aap_start();
    net_start();
    ESP_LOGI(TAG, "up: internal RAM %u KB free, PSRAM %u KB free",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) >> 10),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) >> 10));

    for (;;) {
        board_poll_inputs();
#if !CONFIG_AAM_SERIAL_TRANSPORT
        console_poll();
#endif
        stats();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
