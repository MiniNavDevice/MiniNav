// Video pipeline: access units -> esp_h264 software decoder (dual task) -> crop the 640x480 UI
// area out of the 800x480 frame -> 2:1 downscale + BT.601 YUV->RGB565 -> display task (DMA).
//
// Flow control: the phone may have CONFIG_AAM_MAX_UNACKED frames in flight. A frame is acked
// when the decoder takes it, so the next one crosses the WiFi while this one decodes and the
// phone paces itself to our decode speed.
#include <string.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_attr.h"
#include "esp_h264_dec_sw.h"
#include "esp_h264_dec_param.h"
#include "app.h"

static const char *TAG = "video";

#define AU_SLOTS 4
#define AU_CAP (512 * 1024)

typedef struct {
    uint8_t *data;
    int len;
    bool from_phone;
} au_slot;

static au_slot s_slots[AU_SLOTS];
static QueueHandle_t s_free_q, s_ready_q;
static volatile bool s_need_idr = true;     // decoder waits for an IDR before it accepts P-frames
static volatile bool s_reset_req;
static uint8_t s_codec_cfg[1024];
static volatile int s_codec_cfg_len;
static volatile bool s_codec_cfg_new;
static portMUX_TYPE s_cfg_lock = portMUX_INITIALIZER_UNLOCKED;
static DRAM_ATTR uint8_t s_clamp[1024];     // s_clamp[512 + v] = clamp(v, 0, 255)

// stats (read by app_main)
uint32_t g_vid_frames, g_vid_dropped, g_vid_decode_us, g_vid_scale_us, g_vid_errors;  // stats only

static bool has_idr(const uint8_t *p, int n) {
    for (int i = 0; i + 4 < n; i++) {
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && p[i + 3] == 1))) {
            int k = p[i + 2] == 1 ? i + 3 : i + 4;
            int type = p[k] & 0x1f;
            if (type == 5) return true;
            if (type == 1) return false;  // first slice is a P/B slice
            i = k;
        }
    }
    return false;
}

static bool is_annexb(const uint8_t *p, int n) {
    return n >= 4 && p[0] == 0 && p[1] == 0 && (p[2] == 1 || (p[2] == 0 && p[3] == 1));
}

// AVCC (4-byte big-endian lengths) to Annex-B in place. Android Auto already sends Annex-B;
// this only runs if a phone ever does not.
static void avcc_to_annexb(uint8_t *p, int n) {
    int i = 0;
    while (i + 4 <= n) {
        uint32_t len = ((uint32_t)p[i] << 24) | (p[i + 1] << 16) | (p[i + 2] << 8) | p[i + 3];
        p[i] = 0; p[i + 1] = 0; p[i + 2] = 0; p[i + 3] = 1;
        if (len == 0 || i + 4 + (int)len > n) break;
        i += 4 + len;
    }
}

bool video_push_au(const uint8_t *data, int len, bool from_phone) {
    if (from_phone && g_app.demo) app_toggle_demo("phone video arrived");
    if (!from_phone && !g_app.demo) return true;
    if (len <= 0 || len > AU_CAP) { g_vid_dropped++; return false; }
    bool idr = has_idr(data, len);
    if (s_need_idr && !idr) {
        // Waiting for a keyframe: a P-frame now would decode to garbage.
        g_vid_dropped++;
        if (from_phone) aap_request_keyframe();
        return false;
    }
    int idx;
    // Phones can send past the window we announce (Open Headunit saw it too). Waiting a little
    // pushes back through TCP instead of dropping a P-frame and forcing a keyframe cycle.
    if (xQueueReceive(s_free_q, &idx, from_phone ? pdMS_TO_TICKS(1500) : portMAX_DELAY) != pdTRUE) {
        // The phone sent more than the window allows: drop and resync on the next IDR.
        g_vid_dropped++;
        s_need_idr = true;
        ESP_LOGW(TAG, "decoder queue full, frame dropped; waiting for a keyframe");
        aap_request_keyframe();
        return false;
    }
    au_slot *s = &s_slots[idx];
    memcpy(s->data, data, len);
    if (!is_annexb(s->data, len)) avcc_to_annexb(s->data, len);
    s->len = len;
    s->from_phone = from_phone;
    if (idr) s_need_idr = false;
    xQueueSend(s_ready_q, &idx, portMAX_DELAY);
    return true;
}

void video_set_codec_config(const uint8_t *data, int len) {
    if (len > (int)sizeof s_codec_cfg) return;
    portENTER_CRITICAL(&s_cfg_lock);
    memcpy(s_codec_cfg, data, len);
    s_codec_cfg_len = len;
    s_codec_cfg_new = true;
    portEXIT_CRITICAL(&s_cfg_lock);
}

void video_reset(void) {
    s_reset_req = true;
    s_need_idr = true;
}

// ---- scaler ---------------------------------------------------------------------------------
static inline uint16_t IRAM_ATTR yuv_to_565_swapped(int y, int u, int v) {
    int c = (y - 16) * 298 + 128;
    int d = u - 128, e = v - 128;
    int r = s_clamp[512 + ((c + 409 * e) >> 8)];
    int g = s_clamp[512 + ((c - 100 * d - 208 * e) >> 8)];
    int b = s_clamp[512 + ((c + 516 * d) >> 8)];
    uint16_t px = ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3);
    return (px >> 8) | (px << 8);
}

// 640x480 crop at (cx, cy) -> 320x240 rows [oy0, oy1), averaging 2x2 luma; 4:2:0 chroma lines
// up 1:1 with output pixels. Works two output pixels at a time with 32-bit loads and stores.
static void IRAM_ATTR scale_2to1(const uint8_t *yuv, int w, int h, int cx, int cy, uint16_t *out, int oy0, int oy1) {
    const uint8_t *Y = yuv, *U = yuv + w * h, *V = U + (w / 2) * (h / 2);
    for (int oy = oy0; oy < oy1; oy++) {
        const uint32_t *y0 = (const uint32_t *)(Y + (cy + 2 * oy) * w + cx);
        const uint32_t *y1 = (const uint32_t *)((const uint8_t *)y0 + w);
        const uint16_t *u = (const uint16_t *)(U + (cy / 2 + oy) * (w / 2) + cx / 2);
        const uint16_t *v = (const uint16_t *)(V + (cy / 2 + oy) * (w / 2) + cx / 2);
        uint32_t *o = (uint32_t *)(out + oy * LCD_W);
        for (int i = 0; i < LCD_W / 2; i++) {
            uint32_t a = y0[i], b = y1[i];
            uint32_t uu = u[i], vv = v[i];
            int ya = ((a & 0xff) + ((a >> 8) & 0xff) + (b & 0xff) + ((b >> 8) & 0xff) + 2) >> 2;
            int yb = (((a >> 16) & 0xff) + (a >> 24) + ((b >> 16) & 0xff) + (b >> 24) + 2) >> 2;
            uint32_t pa = yuv_to_565_swapped(ya, uu & 0xff, vv & 0xff);
            uint32_t pb = yuv_to_565_swapped(yb, uu >> 8, vv >> 8);
            o[i] = pa | (pb << 16);
        }
    }
}

// Same, for a crop that starts on an odd 2-byte boundary (the rail-hidden layout: x = 30):
// byte loads, since the 32/16-bit loads above must be aligned.
static void IRAM_ATTR scale_2to1_unaligned(const uint8_t *yuv, int w, int h, int cx, int cy, uint16_t *out, int oy0, int oy1) {
    const uint8_t *Y = yuv, *U = yuv + w * h, *V = U + (w / 2) * (h / 2);
    for (int oy = oy0; oy < oy1; oy++) {
        const uint8_t *y0 = Y + (cy + 2 * oy) * w + cx, *y1 = y0 + w;
        const uint8_t *u = U + (cy / 2 + oy) * (w / 2) + cx / 2;
        const uint8_t *v = V + (cy / 2 + oy) * (w / 2) + cx / 2;
        uint16_t *o = out + oy * LCD_W;
        for (int i = 0; i < LCD_W; i++) {
            int yy = (y0[2 * i] + y0[2 * i + 1] + y1[2 * i] + y1[2 * i + 1] + 2) >> 2;
            o[i] = yuv_to_565_swapped(yy, u[i], v[i]);
        }
    }
}

static inline void scale_rows(const uint8_t *yuv, int w, int h, int cx, int cy, uint16_t *out, int oy0, int oy1) {
    if (cx & 3) scale_2to1_unaligned(yuv, w, h, cx, cy, out, oy0, oy1);
    else scale_2to1(yuv, w, h, cx, cy, out, oy0, oy1);
}

// Any other size: nearest neighbour from a centred 4:3 crop.
static void IRAM_ATTR scale_any(const uint8_t *yuv, int w, int h, int cx, int cy, int cw, int ch, uint16_t *out) {
    const uint8_t *Y = yuv, *U = yuv + w * h, *V = U + (w / 2) * (h / 2);
    uint32_t sx = (cw << 16) / LCD_W, sy = (ch << 16) / LCD_H;
    for (int oy = 0; oy < LCD_H; oy++) {
        int yy = cy + ((oy * sy) >> 16);
        const uint8_t *yr = Y + yy * w;
        const uint8_t *ur = U + (yy / 2) * (w / 2);
        const uint8_t *vr = V + (yy / 2) * (w / 2);
        uint16_t *o = out + oy * LCD_W;
        uint32_t fx = 0;
        for (int ox = 0; ox < LCD_W; ox++, fx += sx) {
            int xx = cx + (fx >> 16);
            o[ox] = yuv_to_565_swapped(yr[xx], ur[xx / 2], vr[xx / 2]);
        }
    }
}

// The 2:1 scale runs on both cores: core 1 (this task) does the top half while a helper on
// core 0 does the bottom half. The decoder is idle between frames, so both cores are free.
static struct { const uint8_t *yuv; int w, h, cx, cy; uint16_t *out; } s_job;
static SemaphoreHandle_t s_job_go, s_job_done;

static void scale_helper_task(void *) {
    for (;;) {
        xSemaphoreTake(s_job_go, portMAX_DELAY);
        scale_rows(s_job.yuv, s_job.w, s_job.h, s_job.cx, s_job.cy, s_job.out, LCD_H / 2, LCD_H);
        xSemaphoreGive(s_job_done);
    }
}

static void scale_split(const uint8_t *yuv, int w, int h, int cx, int cy, uint16_t *out) {
    s_job.yuv = yuv; s_job.w = w; s_job.h = h; s_job.cx = cx; s_job.cy = cy; s_job.out = out;
    xSemaphoreGive(s_job_go);
    scale_rows(yuv, w, h, cx, cy, out, 0, LCD_H / 2);
    xSemaphoreTake(s_job_done, portMAX_DELAY);
}

static void show_frame(const uint8_t *yuv, int w, int h) {
    int64_t t0 = esp_timer_get_time();
    uint16_t *fb = display_acquire_frame();
    int cw, ch, cx, cy;
    if (w == 800 && h == 480 && CONFIG_AAM_VIDEO_MARGIN_H == 0) {
        cw = 640; ch = 480; cx = g_video_crop_x; cy = 0;   // the asked-for UI area, minus the rail if hidden
    } else {
        // anything else: a centred 4:3 crop
        cw = w; ch = w * 3 / 4;
        if (ch > h) { ch = h; cw = h * 4 / 3; }
        cx = ((w - cw) / 2) & ~3; cy = ((h - ch) / 2) & ~1;
    }
    if (cw == 640 && ch == 480) scale_split(yuv, w, h, cx, cy, fb);
    else scale_any(yuv, w, h, cx, cy, cw, ch, fb);
    g_vid_scale_us += esp_timer_get_time() - t0;
    display_submit_frame(fb);
}

// ---- decoder task ---------------------------------------------------------------------------
static esp_h264_dec_handle_t s_dec;
static esp_h264_dec_param_sw_handle_t s_dec_param;

static bool dec_open(void) {
    esp_h264_dec_cfg_sw_t cfg = {};
    cfg.pic_type = ESP_H264_RAW_FMT_I420;
    if (esp_h264_dec_sw_new(&cfg, &s_dec) != ESP_H264_ERR_OK) { ESP_LOGE(TAG, "decoder new failed"); return false; }
    if (esp_h264_dec_open(s_dec) != ESP_H264_ERR_OK) { ESP_LOGE(TAG, "decoder open failed"); return false; }
    esp_h264_dec_sw_get_param_hd(s_dec, &s_dec_param);
    return true;
}

static void dec_close(void) {
    if (!s_dec) return;
    esp_h264_dec_close(s_dec);
    esp_h264_dec_del(s_dec);
    s_dec = NULL;
}

// Feeds a buffer through the decoder; shows any picture it completes. False on a decode error.
static bool decode(uint8_t *data, int len, bool show) {
    esp_h264_dec_in_frame_t in = {};
    in.raw_data.buffer = data;
    in.raw_data.len = len;
    while (in.raw_data.len > 0) {
        esp_h264_dec_out_frame_t out = {};
        int64_t t0 = esp_timer_get_time();
        esp_h264_err_t err = esp_h264_dec_process(s_dec, &in, &out);
        g_vid_decode_us += esp_timer_get_time() - t0;
        if (err != ESP_H264_ERR_OK) { ESP_LOGW(TAG, "decode error %d", err); return false; }
        if (in.consume == 0) break;
        in.raw_data.buffer += in.consume;
        in.raw_data.len -= in.consume;
        if (out.out_size > 0 && show) {
            esp_h264_resolution_t res = {};
            esp_h264_dec_get_resolution(s_dec_param, &res);
            static int lw, lh;
            if (res.width != lw || res.height != lh) {
                lw = res.width; lh = res.height;
                ESP_LOGI(TAG, "stream %dx%d", lw, lh);
            }
            g_vid_frames++;
            show_frame(out.outbuf, res.width, res.height);
        }
    }
    return true;
}

static void video_task(void *) {
    dec_open();
    FILE *dump = NULL;
    for (;;) {
        int idx;
        if (xQueueReceive(s_ready_q, &idx, pdMS_TO_TICKS(500)) != pdTRUE) {
            if (s_reset_req) { s_reset_req = false; dec_close(); dec_open(); }
            continue;
        }
        au_slot *s = &s_slots[idx];
        if (s->from_phone) aap_queue_video_ack();  // pipelining: the next frame travels while this one decodes
        if (!s->from_phone && !g_app.demo) { xQueueSend(s_free_q, &idx, portMAX_DELAY); continue; }
        if (s_reset_req) { s_reset_req = false; dec_close(); dec_open(); s_codec_cfg_new = true; }

        static uint8_t *cfg = (uint8_t *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
        int cfg_len = 0;
        portENTER_CRITICAL(&s_cfg_lock);
        if (s_codec_cfg_new && s->from_phone) { memcpy(cfg, s_codec_cfg, s_codec_cfg_len); cfg_len = s_codec_cfg_len; s_codec_cfg_new = false; }
        portEXIT_CRITICAL(&s_cfg_lock);
        if (cfg_len) decode(cfg, cfg_len, false);

#if CONFIG_AAM_DUMP_H264
        if (s->from_phone && sd_ready()) {
            if (!dump) dump = fopen("/sdcard/dump.h264", "ab");
            if (dump) {
                if (cfg_len) fwrite(cfg, 1, cfg_len, dump);
                fwrite(s->data, 1, s->len, dump);
                static int n;
                if (++n % 30 == 0) fflush(dump);
            }
        }
#endif
        (void)dump;
        if (!decode(s->data, s->len, true)) {
            g_vid_errors++;
            dec_close();
            dec_open();
            s_need_idr = true;
            s_codec_cfg_new = true;
            if (s->from_phone) aap_request_keyframe();
        }
        // No delay here: this task blocks every frame (scaler hand-off, queue wait), so the
        // idle task still runs and feeds the watchdog.
        xQueueSend(s_free_q, &idx, portMAX_DELAY);
    }
}

void video_start(void) {
    for (int i = 0; i < 1024; i++) {
        int v = i - 512;
        s_clamp[i] = v < 0 ? 0 : v > 255 ? 255 : v;
    }
    s_free_q = xQueueCreate(AU_SLOTS, sizeof(int));
    s_ready_q = xQueueCreate(AU_SLOTS, sizeof(int));
    for (int i = 0; i < AU_SLOTS; i++) {
        s_slots[i].data = (uint8_t *)heap_caps_malloc(AU_CAP, MALLOC_CAP_SPIRAM);
        assert(s_slots[i].data);
        xQueueSend(s_free_q, &i, 0);
    }
    s_job_go = xSemaphoreCreateBinary();
    s_job_done = xSemaphoreCreateBinary();
    xTaskCreatePinnedToCore(scale_helper_task, "scale", 3072, NULL, 5, NULL, 0);
    xTaskCreatePinnedToCore(video_task, "video", 8192, NULL, 5, NULL, 1);
}
