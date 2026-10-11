// Built-in demo: loops an embedded 800x480 H.264 clip (Google Maps style drive, made by
// tools/make_demo_clip.py) through the same decoder, scaler and LCD path as the phone's video.
// Hold BOOT for 1 s (or type "demo" on the serial console) to start or stop it. It also shows
// the decode speed on real hardware without a phone.
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "app.h"

static const char *TAG = "demo";
extern const uint8_t demo_start_bin[] asm("_binary_demo_h264_start");
extern const uint8_t demo_end_bin[] asm("_binary_demo_h264_end");

static int next_start_code(const uint8_t *p, int n, int from) {
    for (int i = from; i + 3 < n; i++)
        if (p[i] == 0 && p[i + 1] == 0 && (p[i + 2] == 1 || (p[i + 2] == 0 && i + 3 < n && p[i + 3] == 1))) return i;
    return n;
}

static int nal_type_at(const uint8_t *p, int n, int sc) {
    int k = p[sc + 2] == 1 ? sc + 3 : sc + 4;
    return k < n ? p[k] & 0x1f : -1;
}

static void demo_task(void *) {
    const uint8_t *clip = demo_start_bin;
    const int n = demo_end_bin - demo_start_bin;
    ESP_LOGI(TAG, "demo clip %d KB", n >> 10);
    for (;;) {
        if (!g_app.demo) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        // One access unit = any parameter sets plus the next slice (the clip has one slice per frame).
        int pos = next_start_code(clip, n, 0);
        while (pos < n && g_app.demo) {
            int au_start = pos, end = pos;
            for (;;) {
                int type = nal_type_at(clip, n, end);
                end = next_start_code(clip, n, end + 3);
                if (type == 1 || type == 5 || end >= n) break;
            }
            video_push_au(clip + au_start, end - au_start, false);
            pos = end;
        }
    }
}

void demo_start(void) {
    xTaskCreatePinnedToCore(demo_task, "demo", 4096, NULL, 4, NULL, 0);
}
