// Shared state between the network, protocol, video and display tasks.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "sdkconfig.h"

#define LCD_W 320
#define LCD_H 240

// Android Auto's app rail (launcher, mic, phone, clock) is 80 dp wide on the driver's side. With
// "hide rail", while touch is off, the board asks for a UI area that much wider than the 640 px it
// shows and crops the rail off. Both values are set at service discovery (aap.cpp).
#ifndef CONFIG_AAM_HIDE_RAIL
#define CONFIG_AAM_HIDE_RAIL 0
#endif
#define RAIL_PX (80 * CONFIG_AAM_VIDEO_DENSITY / 160)   // 100 px at 200 dpi
extern volatile int g_video_margin_w;  // margin_width announced to the phone (both sides together)
extern volatile int g_video_crop_x;    // first column of the 800x480 frame that is shown

// Connection stages, in order. The status screen shows a tick for every stage reached.
enum aam_stage {
    STAGE_BOOT = 0,
    STAGE_AP,          // access point up, waiting for the phone to join
    STAGE_PHONE,       // phone joined and has an IP
    STAGE_TCP,         // connected to the headunit server
    STAGE_VERSION,     // version exchange OK
    STAGE_TLS,         // TLS handshake + auth complete
    STAGE_DISCOVERY,   // service discovery answered
    STAGE_VIDEO,       // video frames on screen
};

typedef struct {
    volatile int stage;
    volatile bool night;            // dark theme (Android Auto night mode)
    volatile bool demo;             // playing the built-in demo clip instead of the phone
    volatile bool touch_enabled;    // touch goes to Android Auto (setting, saved)
    volatile bool full_screen;      // hide Android Auto's app rail (setting, saved; needs touch off)
    volatile bool status_dirty;     // status screen needs a redraw
    volatile int rotation;          // LovyanGFX rotation, 1 or 3 (landscape)
    char phone_ip[16];
    char phone_name[32];
    char error[64];                 // last failure, shown on the status screen
} aam_state_t;

extern aam_state_t g_app;

#ifdef __cplusplus
extern "C" {
#endif

void app_set_stage(int stage);
void app_set_error(const char *fmt, ...);
void app_toggle_night(const char *why);
void app_toggle_demo(const char *why);
void app_toggle_touch(const char *why);   // also shows/hides the rail: reconnects if connected
void app_toggle_full(const char *why);    // only while touch is off

// board.cpp
void board_init(void);
void board_poll_inputs(void);          // BOOT button + IMU, call every ~10 ms
uint16_t *display_acquire_frame(void); // 320x240 byte-swapped RGB565 buffer to fill
void display_submit_frame(uint16_t *fb);
void display_request_status(void);
void display_toast(const char *text);  // short banner over the screen for ~1.5 s
void display_dump(void);               // console "shot": screen contents as hex rows
bool sd_ready(void);

// net.cpp
void net_start(void);
bool net_phone_ip(char *out, int len);  // latest phone IP, false if none

// aap.cpp
void aap_start(void);
void aap_send_night(bool night);
void aap_send_video_ack(void);
void aap_queue_video_ack(void);  // same, sent from a helper task (used by the decoder)
void aap_request_keyframe(void);
void aap_restart_session(void);  // polite bye-bye, then reconnect (to apply a new layout)
void aap_send_touch(int action, int x, int y);  // action: 0 down, 1 up, 2 move; x,y in 800x480

// video.cpp
void video_start(void);
// Hands a complete access unit (Annex-B) to the decoder. Returns false when the decoder queue is
// full, in which case the caller still acks and the decoder resyncs on the next IDR.
bool video_push_au(const uint8_t *data, int len, bool from_phone);
void video_set_codec_config(const uint8_t *data, int len);
void video_reset(void);

// demo.cpp
void demo_start(void);

#ifdef __cplusplus
}
#endif
