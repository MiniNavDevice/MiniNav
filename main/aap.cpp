// Android Auto Protocol (AAP) head-unit client over TCP to the phone's headunit server.
//
// Ground truth: Open Headunit (github.com/andreknieriem/open-headunit), a working WiFi
// "headunit server" client. Message ids, field numbers, flags and the handshake order below
// follow its Kotlin sources and .proto files; aasdk agrees on the framing.
//
// Frame: [channel][flags][len_hi][len_lo] (+ 4-byte total length on a FIRST-but-not-LAST frame),
// then the payload. flags: bit0 FIRST, bit1 LAST, bit2 control-type message on a non-control
// channel, bit3 ENCRYPTED. A message starts with a 2-byte big-endian id. After the TLS handshake
// every frame payload is encrypted on its own (one or more TLS records).
#include <string.h>
#include <stdarg.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/select.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_task_wdt.h"
#include "mbedtls/ssl.h"
#include "mbedtls/ctr_drbg.h"
#include "mbedtls/entropy.h"
#include "mbedtls/x509_crt.h"
#include "mbedtls/pk.h"
#include "mbedtls/error.h"
#include "app.h"
#include "pb.h"
#include "aap_cert.h"

static const char *TAG = "aap";
#ifndef CONFIG_AAM_SPEECH_AUDIO
#define CONFIG_AAM_SPEECH_AUDIO 0
#endif
#ifndef CONFIG_AAM_MOTORCYCLE
#define CONFIG_AAM_MOTORCYCLE 0
#endif
#ifndef CONFIG_AAM_LOG_MESSAGES
#define CONFIG_AAM_LOG_MESSAGES 0
#endif

// ---- constants (Open Headunit protocol/*.kt, proto/*.proto) ---------------------------------
enum {
    CH_CONTROL = 0, CH_SENSOR = 1, CH_VIDEO = 2, CH_INPUT = 3,
    CH_SPEECH = 4, CH_SYSTEM_AUDIO = 5, CH_MEDIA_AUDIO = 6,
    CH_MIC = 7, CH_MEDIA_PLAYBACK = 9, CH_NAVIGATION = 10,
};
enum {
    F_FIRST = 0x01, F_LAST = 0x02, F_CONTROL = 0x04, F_ENCRYPTED = 0x08,
};
enum {
    MSG_VERSION_REQUEST = 1, MSG_VERSION_RESPONSE = 2, MSG_SSL_HANDSHAKE = 3, MSG_AUTH_COMPLETE = 4,
    MSG_SERVICE_DISCOVERY_REQUEST = 5, MSG_SERVICE_DISCOVERY_RESPONSE = 6,
    MSG_CHANNEL_OPEN_REQUEST = 7, MSG_CHANNEL_OPEN_RESPONSE = 8, MSG_CHANNEL_CLOSE = 9,
    MSG_PING_REQUEST = 11, MSG_PING_RESPONSE = 12, MSG_NAV_FOCUS_REQUEST = 13, MSG_NAV_FOCUS_NOTIFICATION = 14,
    MSG_BYEBYE_REQUEST = 15, MSG_BYEBYE_RESPONSE = 16, MSG_VOICE_SESSION = 17,
    MSG_AUDIO_FOCUS_REQUEST = 18, MSG_AUDIO_FOCUS_NOTIFICATION = 19,
    // media channels
    MEDIA_DATA = 0x0000, MEDIA_CODEC_CONFIG = 0x0001, MEDIA_SETUP = 0x8000, MEDIA_START = 0x8001,
    MEDIA_STOP = 0x8002, MEDIA_CONFIG = 0x8003, MEDIA_ACK = 0x8004,
    MEDIA_MIC_REQUEST = 0x8005, MEDIA_MIC_RESPONSE = 0x8006, MEDIA_VIDEO_FOCUS_REQUEST = 0x8007, MEDIA_VIDEO_FOCUS_NOTIFICATION = 0x8008,
    // sensor channel
    SENSOR_START_REQUEST = 0x8001, SENSOR_START_RESPONSE = 0x8002, SENSOR_EVENT = 0x8003,
    // input channel
    INPUT_BINDING_REQUEST = 0x8002, INPUT_BINDING_RESPONSE = 0x8003,
};
enum { SENSOR_NIGHT = 10, SENSOR_DRIVING_STATUS = 13 };
enum { VIDEO_FOCUS_PROJECTED = 1, VIDEO_FOCUS_NATIVE = 2 };

#define RX_FRAME_CAP (20 * 1024)
#define VIDEO_MSG_CAP (1024 * 1024)
#define OTHER_MSG_CAP (64 * 1024)
#define BIO_CAP (32 * 1024)
#define NCH 16

// ---- session state --------------------------------------------------------------------------
static int s_sock = -1;
static volatile bool s_encrypted;
static volatile bool s_alive;            // session up and allowed to send
static volatile bool s_sensor_open;
static volatile int32_t s_video_session = 0;
static int32_t s_session_ids[NCH];
static SemaphoreHandle_t s_tx_lock;      // one TLS context: every encrypt and decrypt holds this
static uint8_t *s_rx_frame;
static uint8_t *s_tx_buf;
static TaskHandle_t s_ack_task;          // sends video ACKs so the decoder never waits on the TLS lock

struct assembly { uint8_t *buf; size_t cap, len; bool active, overflow; };
static uint8_t *s_scratch;  // plaintext of messages too big for their channel buffer (e.g. album art), dropped
static assembly s_asm[NCH];

static mbedtls_ssl_context s_ssl;
static mbedtls_ssl_config s_conf;
static mbedtls_x509_crt s_crt;
static mbedtls_pk_context s_key;
static mbedtls_entropy_context s_entropy;
static mbedtls_ctr_drbg_context s_drbg;

static uint8_t *s_bio_in; static size_t s_bio_in_len, s_bio_in_pos;
static uint8_t *s_bio_out; static size_t s_bio_out_len;

// per-stage timing, read by the stats line in app_main
uint32_t g_aap_rx_bytes, g_aap_decrypt_us, g_aap_frames;  // stats only
volatile int g_video_margin_w = CONFIG_AAM_VIDEO_MARGIN_W, g_video_crop_x = CONFIG_AAM_VIDEO_MARGIN_W / 2;

// ---- TLS memory BIO -------------------------------------------------------------------------
static int bio_send(void *, const unsigned char *buf, size_t len) {
    if (s_bio_out_len + len > BIO_CAP) return MBEDTLS_ERR_SSL_ALLOC_FAILED;
    memcpy(s_bio_out + s_bio_out_len, buf, len);
    s_bio_out_len += len;
    return (int)len;
}

static int bio_recv(void *, unsigned char *buf, size_t len) {
    size_t avail = s_bio_in_len - s_bio_in_pos;
    if (!avail) return MBEDTLS_ERR_SSL_WANT_READ;
    if (len > avail) len = avail;
    memcpy(buf, s_bio_in + s_bio_in_pos, len);
    s_bio_in_pos += len;
    return (int)len;
}

static void tls_err(const char *what, int ret) {
    char msg[96];
    mbedtls_strerror(ret, msg, sizeof msg);
    ESP_LOGE(TAG, "%s: -0x%04X %s", what, -ret, msg);
}

static bool tls_init(void) {
    mbedtls_entropy_init(&s_entropy);
    mbedtls_ctr_drbg_init(&s_drbg);
    int ret = mbedtls_ctr_drbg_seed(&s_drbg, mbedtls_entropy_func, &s_entropy, (const unsigned char *)"mininav", 7);
    if (ret) { tls_err("drbg seed", ret); return false; }
    mbedtls_ssl_config_init(&s_conf);
    mbedtls_x509_crt_init(&s_crt);
    mbedtls_pk_init(&s_key);
    if ((ret = mbedtls_x509_crt_parse(&s_crt, (const unsigned char *)AAP_CERT_PEM, sizeof AAP_CERT_PEM))) {
        tls_err("cert", ret); return false;
    }
    if ((ret = mbedtls_pk_parse_key(&s_key, (const unsigned char *)AAP_KEY_PEM, sizeof AAP_KEY_PEM, NULL, 0,
                                    mbedtls_ctr_drbg_random, &s_drbg))) {
        tls_err("key", ret); return false;
    }
    mbedtls_ssl_config_defaults(&s_conf, MBEDTLS_SSL_IS_CLIENT, MBEDTLS_SSL_TRANSPORT_STREAM, MBEDTLS_SSL_PRESET_DEFAULT);
    mbedtls_ssl_conf_authmode(&s_conf, MBEDTLS_SSL_VERIFY_NONE);   // the phone's certificate is not checked
    mbedtls_ssl_conf_rng(&s_conf, mbedtls_ctr_drbg_random, &s_drbg);
    mbedtls_ssl_conf_min_tls_version(&s_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    mbedtls_ssl_conf_max_tls_version(&s_conf, MBEDTLS_SSL_VERSION_TLS1_2);
    if ((ret = mbedtls_ssl_conf_own_cert(&s_conf, &s_crt, &s_key))) { tls_err("own cert", ret); return false; }
    mbedtls_ssl_init(&s_ssl);
    return true;
}

// ---- socket helpers ---------------------------------------------------------------------------
static bool tcp_send_all(const uint8_t *p, size_t n) {
    while (n) {
        int r = send(s_sock, p, n, 0);
        if (r < 0) {
            if (errno == EAGAIN || errno == EWOULDBLOCK) { vTaskDelay(1); continue; }
            ESP_LOGW(TAG, "send failed: errno %d", errno);
            return false;
        }
        p += r; n -= r;
    }
    return true;
}

// Reads exactly n bytes. `idle_ms` bounds the wait for the first byte, `body_ms` the rest.
static bool tcp_recv_all(uint8_t *p, size_t n, int idle_ms, int body_ms) {
    int64_t start = esp_timer_get_time();
    size_t got = 0;
    while (got < n) {
        int r = recv(s_sock, p + got, n - got, 0);
        if (r > 0) { got += r; g_aap_rx_bytes += r; continue; }
        if (r == 0) { ESP_LOGW(TAG, "phone closed the connection"); return false; }
        if (errno != EAGAIN && errno != EWOULDBLOCK) { ESP_LOGW(TAG, "recv failed: errno %d", errno); return false; }
        esp_task_wdt_reset();
        int limit = got ? body_ms : idle_ms;
        if ((esp_timer_get_time() - start) / 1000 > limit) {
            ESP_LOGW(TAG, "recv timeout (%u of %u bytes)", (unsigned)got, (unsigned)n);
            return false;
        }
        if (!s_alive && s_encrypted) return false;
    }
    return true;
}

// ---- test transport: AAP over the USB serial port ------------------------------------------
// Built only with CONFIG_AAM_SERIAL_TRANSPORT, for tools/fake_phone.py: the Mac plays the phone
// over USB, so the whole stack (framing, TLS, discovery, ACK pacing, decode) runs without WiFi.
// Packets are A5 5A len_hi len_lo data; log text (ASCII) passes around them.
#if CONFIG_AAM_SERIAL_TRANSPORT
#include "freertos/stream_buffer.h"
#include "driver/usb_serial_jtag.h"
static StreamBufferHandle_t s_ser_rx;
static volatile bool s_ser_hello;

static void ser_reader_task(void *) {
    uint8_t buf[512];
    int state = 0, need = 0;
    for (;;) {
        int n = usb_serial_jtag_read_bytes(buf, sizeof buf, pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            uint8_t c = buf[i];
            switch (state) {
            case 0: state = c == 0xA5 ? 1 : 0; break;
            case 1: state = c == 0x5A ? 2 : 0; break;
            case 2: need = c << 8; state = 3; break;
            case 3:
                need |= c;
                if (need == 0) { s_ser_hello = true; state = 0; }
                else state = 4;
                break;
            case 4: {
                int k = n - i < need ? n - i : need;
                xStreamBufferSend(s_ser_rx, buf + i, k, portMAX_DELAY);
                i += k - 1;
                need -= k;
                if (!need) state = 0;
                break;
            }
            }
        }
    }
}

static bool ser_send_all(const uint8_t *p, size_t n) {
    static uint8_t *pkt = (uint8_t *)heap_caps_malloc(4100, MALLOC_CAP_INTERNAL);
    while (n) {
        size_t k = n > 4096 ? 4096 : n;
        pkt[0] = 0xA5; pkt[1] = 0x5A; pkt[2] = k >> 8; pkt[3] = k & 0xff;
        memcpy(pkt + 4, p, k);
        if (usb_serial_jtag_write_bytes(pkt, k + 4, pdMS_TO_TICKS(2000)) != (int)(k + 4)) return false;
        p += k; n -= k;
    }
    return true;
}

static bool ser_recv_all(uint8_t *p, size_t n, int idle_ms, int body_ms) {
    int64_t start = esp_timer_get_time();
    size_t got = 0;
    while (got < n) {
        size_t r = xStreamBufferReceive(s_ser_rx, p + got, n - got, pdMS_TO_TICKS(200));
        if (r) { got += r; g_aap_rx_bytes += r; continue; }
        esp_task_wdt_reset();
        if ((esp_timer_get_time() - start) / 1000 > (got ? body_ms : idle_ms)) return false;
    }
    return true;
}
#endif

static bool send_all(const uint8_t *p, size_t n) {
#if CONFIG_AAM_SERIAL_TRANSPORT
    return ser_send_all(p, n);
#else
    return tcp_send_all(p, n);
#endif
}

static bool recv_all(uint8_t *p, size_t n, int idle_ms, int body_ms) {
#if CONFIG_AAM_SERIAL_TRANSPORT
    return ser_recv_all(p, n, idle_ms, body_ms);
#else
    return tcp_recv_all(p, n, idle_ms, body_ms);
#endif
}

// ---- sending ----------------------------------------------------------------------------------
static bool is_control_type(uint16_t id) { return id >= 1 && id <= 26; }

static bool send_message(uint8_t ch, uint16_t id, const uint8_t *payload, size_t len) {
    if (s_sock < 0) return false;
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    uint8_t flags = F_FIRST | F_LAST;
    if (ch != CH_CONTROL && is_control_type(id)) flags |= F_CONTROL;
    uint8_t *plain = s_tx_buf + 4;
    plain[0] = id >> 8; plain[1] = id & 0xff;
    memcpy(plain + 2, payload, len);
    size_t plen = len + 2;
    const uint8_t *body = plain;
    size_t blen = plen;
    bool ok = true;
    if (s_encrypted) {
        s_bio_out_len = 0;
        size_t off = 0;
        while (off < plen) {
            int r = mbedtls_ssl_write(&s_ssl, plain + off, plen - off);
            if (r <= 0) { tls_err("encrypt", r); ok = false; break; }
            off += r;
        }
        body = s_bio_out; blen = s_bio_out_len;
        flags |= F_ENCRYPTED;
    }
    if (ok && blen > 0xffff) { ESP_LOGE(TAG, "message too big (%u)", (unsigned)blen); ok = false; }
    if (ok) {
        uint8_t hdr[4] = { ch, flags, (uint8_t)(blen >> 8), (uint8_t)blen };
        ok = send_all(hdr, 4) && send_all(body, blen);
    }
#if CONFIG_AAM_LOG_MESSAGES
    ESP_LOGI(TAG, "TX ch %d id 0x%04X len %u", ch, id, (unsigned)len);
#endif
    xSemaphoreGive(s_tx_lock);
    return ok;
}

static bool send_pb(uint8_t ch, uint16_t id, const pb_w *w) {
    if (w->overflow) { ESP_LOGE(TAG, "protobuf overflow for 0x%04X", id); return false; }
    return send_message(ch, id, w->buf, w->len);
}

// ---- outgoing messages --------------------------------------------------------------------
static void send_video_focus(int mode, bool unsolicited) {
    uint8_t b[8]; pb_w w; pb_init(&w, b, sizeof b);
    pb_int(&w, 1, mode);
    pb_bool(&w, 2, unsolicited);
    send_pb(CH_VIDEO, MEDIA_VIDEO_FOCUS_NOTIFICATION, &w);
}

static void send_driving_status(void) {
    uint8_t s[4], b[8]; pb_w sub, w;
    pb_init(&sub, s, sizeof s); pb_int(&sub, 1, 0);  // UNRESTRICTED
    pb_init(&w, b, sizeof b); pb_msg(&w, 13, &sub);
    send_pb(CH_SENSOR, SENSOR_EVENT, &w);
}

static void send_night_event(bool night) {
    uint8_t s[4], b[8]; pb_w sub, w;
    pb_init(&sub, s, sizeof s); pb_bool(&sub, 1, night);
    pb_init(&w, b, sizeof b); pb_msg(&w, 10, &sub);
    send_pb(CH_SENSOR, SENSOR_EVENT, &w);
}

void aap_send_night(bool night) {
    if (s_alive && s_sensor_open) {
        send_night_event(night);
    }
}

void aap_send_touch(int action, int x, int y) {
    if (!s_alive || g_app.stage < STAGE_VIDEO) return;
    uint8_t pb_[16], tb[32], b[48]; pb_w ptr, te, w;
    pb_init(&ptr, pb_, sizeof pb_);
    pb_varint(&ptr, 1, x); pb_varint(&ptr, 2, y); pb_varint(&ptr, 3, 0);
    pb_init(&te, tb, sizeof tb);
    pb_msg(&te, 1, &ptr);
    pb_varint(&te, 2, 0);
    pb_varint(&te, 3, action);
    pb_init(&w, b, sizeof b);
    pb_varint(&w, 1, (uint64_t)esp_timer_get_time() * 1000);  // nanoseconds, as Open Headunit sends (Maps gestures use it)
    pb_msg(&w, 3, &te);
    send_pb(CH_INPUT, 0x8001, &w);
}

void aap_send_video_ack(void) {
    if (!s_alive) return;
    uint8_t b[16]; pb_w w; pb_init(&w, b, sizeof b);
    pb_int(&w, 1, s_video_session);
    pb_varint(&w, 2, 1);
    send_pb(CH_VIDEO, MEDIA_ACK, &w);
}

// The video task only queues ACKs: sending one means waiting for the TLS lock, which the
// network task holds while it decrypts (up to a few ms per 16 KB frame).
static void ack_task(void *) {
    for (;;) {
        ulTaskNotifyTake(pdFALSE, portMAX_DELAY);  // one ACK per queued frame
        aap_send_video_ack();
    }
}

void aap_queue_video_ack(void) {
    if (s_ack_task) xTaskNotifyGive(s_ack_task);
}

static void send_media_ack(uint8_t ch) {
    uint8_t b[16]; pb_w w; pb_init(&w, b, sizeof b);
    pb_int(&w, 1, s_session_ids[ch]);
    pb_varint(&w, 2, 1);
    send_pb(ch, MEDIA_ACK, &w);
}

// Keyframe request: release video focus, then take it back 400 ms later (Open Headunit's
// focus cycle). The phone answers a regained focus with a fresh IDR.
static esp_timer_handle_t s_focus_timer;
static int64_t s_last_kf_request;
static void focus_regain_cb(void *) {
    if (s_alive) send_video_focus(VIDEO_FOCUS_PROJECTED, true);
}
void aap_request_keyframe(void) {
    int64_t now = esp_timer_get_time();
    if (!s_alive || now - s_last_kf_request < 2000000) return;
    s_last_kf_request = now;
    ESP_LOGW(TAG, "requesting a keyframe (video focus cycle)");
    send_video_focus(VIDEO_FOCUS_NATIVE, false);
    esp_timer_stop(s_focus_timer);
    esp_timer_start_once(s_focus_timer, 400000);
}

static void add_audio_sink(pb_w *w, int ch, int stream_type, int rate, int channels) {
    uint8_t cfgb[16], sinkb[48], svcb[64];
    pb_w cfg, sink, svc;
    pb_init(&cfg, cfgb, sizeof cfgb);
    pb_varint(&cfg, 1, rate); pb_varint(&cfg, 2, 16); pb_varint(&cfg, 3, channels);
    pb_init(&sink, sinkb, sizeof sinkb);
    pb_varint(&sink, 1, 1);             // MEDIA_CODEC_AUDIO_PCM
    pb_varint(&sink, 2, stream_type);   // SPEECH = 1, SYSTEM = 2, MEDIA = 3
    pb_msg(&sink, 3, &cfg);
    pb_init(&svc, svcb, sizeof svcb);
    pb_varint(&svc, 1, ch);
    pb_msg(&svc, 3, &sink);
    pb_msg(w, 1, &svc);
}

// Android Auto's newer car service aborts setup right after discovery when a head unit without
// a microphone claims to be anything but a motorcycle (Open Headunit, VehicleTypePolicy.kt), and a
// motorcycle only gets two-wheeler directions. So the board announces a microphone and claims a
// car; mic requests are declined (no mic on this board, the phone's own mic is not used).
// The phone keeps the type it first stored for a vehicle id, hence a fresh id per type.
#if CONFIG_AAM_MOTORCYCLE
#define VEHICLE_ID "MININAV0002-moto"
#define VEHICLE_TYPE 3
#else
#define VEHICLE_ID "MININAV0003"
#define VEHICLE_TYPE 1
#endif

static void send_service_discovery_response(void) {
    static uint8_t *out = (uint8_t *)heap_caps_malloc(1024, MALLOC_CAP_SPIRAM);
    pb_w w; pb_init(&w, out, 1024);
    uint8_t a[96], b[96], c[160];
    pb_w x, y, z;

    // sensors: DRIVING_STATUS + NIGHT
    pb_init(&x, a, sizeof a);
    pb_init(&y, b, sizeof b); pb_varint(&y, 1, SENSOR_DRIVING_STATUS); pb_msg(&x, 1, &y);
    pb_init(&y, b, sizeof b); pb_varint(&y, 1, SENSOR_NIGHT); pb_msg(&x, 1, &y);
    pb_init(&z, c, sizeof c); pb_varint(&z, 1, CH_SENSOR); pb_msg(&z, 2, &x);
    pb_msg(&w, 1, &z);

    // video sink: H.264 BP 800x480 @30, UI area in the middle
    pb_init(&y, b, sizeof b);
    pb_varint(&y, 1, 1);    // _800x480
    pb_varint(&y, 2, 2);    // _30 (the enum has _60 = 1, _30 = 2)
    // Layout: hide the rail while touch is off (it is unreachable then anyway).
    bool hide_rail = CONFIG_AAM_HIDE_RAIL && g_app.full_screen && !g_app.touch_enabled;
    int margin = hide_rail ? 160 - RAIL_PX : CONFIG_AAM_VIDEO_MARGIN_W;
    g_video_margin_w = margin;
    g_video_crop_x = margin / 2 + (hide_rail && !CONFIG_AAM_RIGHT_HAND_DRIVE ? RAIL_PX : 0);
    pb_varint(&y, 3, margin);
    pb_varint(&y, 4, CONFIG_AAM_VIDEO_MARGIN_H);
    pb_varint(&y, 5, CONFIG_AAM_VIDEO_DENSITY);
    pb_varint(&y, 8, 10000);  // square pixels
    pb_varint(&y, 10, 3);     // MEDIA_CODEC_VIDEO_H264_BP
    pb_init(&x, a, sizeof a);
    pb_varint(&x, 1, 3);      // available_type H264_BP
    pb_varint(&x, 2, 0);      // audio_type NONE
    pb_msg(&x, 4, &y);
    pb_bool(&x, 5, true);     // available while in call
    pb_init(&z, c, sizeof c); pb_varint(&z, 1, CH_VIDEO); pb_msg(&z, 3, &x);
    pb_msg(&w, 1, &z);

    // input: capacitive touchscreen (CST3530) + a few keys. Field layout from aasdk's newer
    // InputSourceService.proto: keycodes packed, TouchScreen{width, height, type}.
    pb_init(&y, b, sizeof b); pb_varint(&y, 1, 800); pb_varint(&y, 2, 480); pb_varint(&y, 3, 1);  // CAPACITIVE
    pb_init(&x, a, sizeof a);
    static const uint8_t keys[] = { 4, 19, 20, 21, 22, 23, 84, 85, 87, 88 };
    pb_bytes(&x, 1, keys, sizeof keys);  // packed varints (all < 128)
    pb_msg(&x, 2, &y);
    pb_init(&z, c, sizeof c); pb_varint(&z, 1, CH_INPUT); pb_msg(&z, 4, &x);
    pb_msg(&w, 1, &z);

    // audio: system sounds always (Open Headunit announces it even with audio off)
    // 16 kHz mono is the only config the phone accepts on the SYSTEM sink.
    add_audio_sink(&w, CH_SYSTEM_AUDIO, 2, 16000, 1);
    if (CONFIG_AAM_SPEECH_AUDIO) {
        // Speech + media sinks too, like Open Headunit with audio on. The audio is acked and dropped.
        add_audio_sink(&w, CH_SPEECH, 1, 16000, 1);
        add_audio_sink(&w, CH_MEDIA_AUDIO, 3, 48000, 2);
    }

    // microphone source (16 kHz mono PCM); requests to open it are declined
    if (VEHICLE_TYPE != 3) {
        pb_init(&y, b, sizeof b); pb_varint(&y, 1, 16000); pb_varint(&y, 2, 16); pb_varint(&y, 3, 1);
        pb_init(&x, a, sizeof a); pb_varint(&x, 1, 1); pb_msg(&x, 2, &y);
        pb_init(&z, c, sizeof c); pb_varint(&z, 1, CH_MIC); pb_msg(&z, 5, &x);
        pb_msg(&w, 1, &z);
    }

    // media playback status (empty) and navigation status (turn codes only)
    pb_init(&x, a, sizeof a);
    pb_init(&z, c, sizeof c); pb_varint(&z, 1, CH_MEDIA_PLAYBACK); pb_msg(&z, 9, &x);
    pb_msg(&w, 1, &z);
    pb_init(&x, a, sizeof a); pb_varint(&x, 1, 1000); pb_varint(&x, 2, 2);
    pb_init(&z, c, sizeof c); pb_varint(&z, 1, CH_NAVIGATION); pb_msg(&z, 8, &x);
    pb_msg(&w, 1, &z);

    pb_string(&w, 2, "MiniNav");          // make
    pb_string(&w, 3, "ESP32-S3");         // model
    pb_string(&w, 4, "2026");             // year
    pb_string(&w, 5, VEHICLE_ID);
    pb_varint(&w, 6, CONFIG_AAM_RIGHT_HAND_DRIVE ? 1 : 0);
    pb_string(&w, 7, "DIY");              // head unit make
    pb_string(&w, 8, "MiniNav 2.8");      // head unit model
    pb_string(&w, 9, "1");                // sw build
    pb_string(&w, 10, "1.0");             // sw version
    pb_bool(&w, 11, false);               // can play native media during VR
    pb_bool(&w, 12, false);               // keep the projected clock
    pb_varint(&w, 13, 0x06);              // hide phone signal + battery on the small screen
    pb_string(&w, 14, "MiniNav");
    pb_init(&x, a, sizeof a);
    pb_string(&x, 1, "MiniNav"); pb_string(&x, 2, "ESP32-S3"); pb_string(&x, 3, "2026");
    pb_string(&x, 4, VEHICLE_ID); pb_string(&x, 5, "DIY"); pb_string(&x, 6, "MiniNav 2.8");
    pb_string(&x, 7, "1"); pb_string(&x, 8, "1.0");
    pb_varint(&x, 9, VEHICLE_TYPE);  // see VEHICLE_ID
    pb_msg(&w, 17, &x);

    ESP_LOGI(TAG, "service discovery answered (%u bytes)", (unsigned)w.len);
    send_pb(CH_CONTROL, MSG_SERVICE_DISCOVERY_RESPONSE, &w);
}

// ---- receiving ------------------------------------------------------------------------------
typedef struct { uint8_t ch, flags; uint16_t len; uint32_t total; } frame_hdr;

static bool read_frame(frame_hdr *h, int idle_ms) {
    uint8_t hdr[4];
    if (!recv_all(hdr, 4, idle_ms, 5000)) return false;
    h->ch = hdr[0]; h->flags = hdr[1]; h->len = (hdr[2] << 8) | hdr[3]; h->total = 0;
    if ((h->flags & (F_FIRST | F_LAST)) == F_FIRST) {
        uint8_t t[4];
        if (!recv_all(t, 4, 5000, 5000)) return false;
        h->total = ((uint32_t)t[0] << 24) | (t[1] << 16) | (t[2] << 8) | t[3];
    }
    if (h->len > RX_FRAME_CAP) { ESP_LOGE(TAG, "frame too big: %u", h->len); return false; }
    return recv_all(s_rx_frame, h->len, 5000, 5000);
}

// Decrypts the frame in s_rx_frame and appends the plaintext to `dst`.
static int decrypt_into(const frame_hdr *h, uint8_t *dst, size_t cap) {
    int64_t t0 = esp_timer_get_time();
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_bio_in = s_rx_frame; s_bio_in_len = h->len; s_bio_in_pos = 0;
    size_t out = 0;
    int ret = 0;
    for (;;) {
        if (out >= cap) { ret = -1; ESP_LOGE(TAG, "message buffer full"); break; }
        int r = mbedtls_ssl_read(&s_ssl, dst + out, cap - out);
        if (r > 0) { out += r; continue; }
        if (r == MBEDTLS_ERR_SSL_WANT_READ) {
            if (s_bio_in_pos != s_bio_in_len)
                ESP_LOGW(TAG, "ch %d: %u bytes of a partial TLS record dropped", h->ch, (unsigned)(s_bio_in_len - s_bio_in_pos));
            break;
        }
        tls_err("decrypt", r); ret = -1; break;
    }
    s_bio_in_len = s_bio_in_pos = 0;
    xSemaphoreGive(s_tx_lock);
    g_aap_decrypt_us += esp_timer_get_time() - t0;
    return ret < 0 ? -1 : (int)out;
}

static inline void log_message(uint8_t ch, uint16_t id, const uint8_t *p, size_t n) {
#if CONFIG_AAM_LOG_MESSAGES
    char hex[3 * 64 + 1];
    size_t k = n < 64 ? n : 64;
    for (size_t i = 0; i < k; i++) sprintf(hex + 3 * i, "%02X ", p[i]);
    hex[3 * k] = 0;
    ESP_LOGI(TAG, "RX ch %d id 0x%04X len %u: %s", ch, id, (unsigned)n, hex);
#endif
}

static bool s_bye;
static volatile bool s_restart;   // board asked for a new session (layout change)

void aap_restart_session(void) {
    if (!s_alive || g_app.stage < STAGE_DISCOVERY) return;  // not connected: the next session uses the new layout anyway
    s_restart = true;
    uint8_t b[4]; pb_w w; pb_init(&w, b, sizeof b); pb_varint(&w, 1, 1);  // USER_SELECTION
    send_pb(CH_CONTROL, MSG_BYEBYE_REQUEST, &w);
    ESP_LOGI(TAG, "bye-bye sent to switch the layout");
}

static void handle_message(uint8_t ch, uint8_t flags, const uint8_t *msg, size_t n) {
    if (n < 2) return;
    uint16_t id = (msg[0] << 8) | msg[1];
    const uint8_t *p = msg + 2;
    size_t len = n - 2;
    log_message(ch, id, p, len);

    // Channel open requests arrive on the channel being opened, flagged control-type.
    if (id == MSG_CHANNEL_OPEN_REQUEST && (ch != CH_CONTROL || (flags & F_CONTROL))) {
        uint8_t b[4]; pb_w w; pb_init(&w, b, sizeof b); pb_int(&w, 1, 0);
        send_pb(ch, MSG_CHANNEL_OPEN_RESPONSE, &w);
        ESP_LOGD(TAG, "channel %d open", ch);
        if (ch == CH_SENSOR) { s_sensor_open = true; send_driving_status(); }
        return;
    }
    if (id == MSG_CHANNEL_CLOSE && ch != CH_CONTROL) { ESP_LOGI(TAG, "channel %d closed", ch); return; }

    if (ch == CH_CONTROL) {
        switch (id) {
        case MSG_SERVICE_DISCOVERY_REQUEST: {
            pb_r r = { p, p + len }; pb_field f;
            char name[32] = "", brand[32] = "";
            while (pb_next(&r, &f)) {
                if (f.field == 4 && f.data) snprintf(name, sizeof name, "%.*s", (int)f.len, f.data);
                if (f.field == 5 && f.data) snprintf(brand, sizeof brand, "%.*s", (int)f.len, f.data);
            }
            ESP_LOGI(TAG, "service discovery request from \"%s\" (%s)", name, brand);
            snprintf(g_app.phone_name, sizeof g_app.phone_name, "%s", name[0] ? name : brand);
            send_service_discovery_response();
            app_set_stage(STAGE_DISCOVERY);
            return;
        }
        case MSG_PING_REQUEST: {
            int64_t ts = pb_get_int(p, len, 1, 0);
            uint8_t b[16]; pb_w w; pb_init(&w, b, sizeof b); pb_int(&w, 1, ts);
            send_pb(CH_CONTROL, MSG_PING_RESPONSE, &w);
            return;
        }
        case MSG_PING_RESPONSE: return;
        case MSG_NAV_FOCUS_REQUEST: {
            uint8_t b[4]; pb_w w; pb_init(&w, b, sizeof b); pb_varint(&w, 1, 2);  // NAV_FOCUS_PROJECTED
            send_pb(CH_CONTROL, MSG_NAV_FOCUS_NOTIFICATION, &w);
            return;
        }
        case MSG_AUDIO_FOCUS_REQUEST: {
            int req = (int)pb_get_int(p, len, 1, 1);
            int state = req == 1 ? 1 : req == 2 ? 2 : req == 3 ? 7 : 3;  // GAIN, GAIN_TRANSIENT, GUIDANCE_ONLY, LOSS
            uint8_t b[8]; pb_w w; pb_init(&w, b, sizeof b); pb_varint(&w, 1, state);
            send_pb(CH_CONTROL, MSG_AUDIO_FOCUS_NOTIFICATION, &w);
            return;
        }
        case MSG_BYEBYE_REQUEST: {
            ESP_LOGW(TAG, "phone said bye-bye (reason %d)", (int)pb_get_int(p, len, 1, 0));
            uint8_t b[1]; pb_w w; pb_init(&w, b, sizeof b);
            send_pb(CH_CONTROL, MSG_BYEBYE_RESPONSE, &w);
            s_bye = true;
            return;
        }
        case MSG_BYEBYE_RESPONSE: s_bye = true; return;
        case MSG_VOICE_SESSION: return;
        default:
            ESP_LOGD(TAG, "control message 0x%04X (%u bytes) ignored", id, (unsigned)len);
            return;
        }
    }

    if (ch == CH_SENSOR) {
        if (id == SENSOR_START_REQUEST) {
            int type = (int)pb_get_int(p, len, 1, 0);
            uint8_t b[4]; pb_w w; pb_init(&w, b, sizeof b); pb_int(&w, 1, 0);
            send_pb(CH_SENSOR, SENSOR_START_RESPONSE, &w);
            ESP_LOGD(TAG, "sensor %d started", type);
            if (type == SENSOR_NIGHT) send_night_event(g_app.night);
            if (type == SENSOR_DRIVING_STATUS) send_driving_status();
        }
        return;
    }

    if (ch == CH_INPUT) {
        if (id == INPUT_BINDING_REQUEST) {
            uint8_t b[4]; pb_w w; pb_init(&w, b, sizeof b); pb_int(&w, 1, 0);
            send_pb(CH_INPUT, INPUT_BINDING_RESPONSE, &w);
        }
        return;
    }

    if (ch == CH_MIC) {
        if (id == MEDIA_MIC_REQUEST) {
            bool open = pb_get_int(p, len, 1, 0) != 0;
            static int32_t mic_session;
            if (open) mic_session++;
            ESP_LOGD(TAG, "phone %s the microphone", open ? "asked for (declined)" : "closed");
            uint8_t b[24]; pb_w w; pb_init(&w, b, sizeof b);
            pb_int(&w, 1, open ? -1 : 0);
            pb_int(&w, 2, mic_session);
            send_pb(CH_MIC, MEDIA_MIC_RESPONSE, &w);
        }
        return;
    }

    bool video = ch == CH_VIDEO;
    bool audio = ch == CH_SYSTEM_AUDIO || ch == CH_SPEECH || ch == CH_MEDIA_AUDIO;
    if (!video && !audio) return;  // media playback / navigation status: not used

    switch (id) {
    case MEDIA_SETUP: {
        uint8_t b[16]; pb_w w; pb_init(&w, b, sizeof b);
        pb_varint(&w, 1, 2);  // STATUS_READY
        pb_varint(&w, 2, video ? CONFIG_AAM_MAX_UNACKED : 4);
        pb_varint(&w, 3, 0);  // configuration index 0
        send_pb(ch, MEDIA_CONFIG, &w);
        ESP_LOGD(TAG, "%s setup", video ? "video" : "audio");
        if (video) {
            send_video_focus(VIDEO_FOCUS_PROJECTED, true);
        } else {
            uint8_t c[8]; pb_w f; pb_init(&f, c, sizeof c); pb_varint(&f, 1, 1); pb_bool(&f, 2, true);
            send_pb(CH_CONTROL, MSG_AUDIO_FOCUS_NOTIFICATION, &f);
        }
        return;
    }
    case MEDIA_START: {
        int32_t sid = (int32_t)pb_get_int(p, len, 1, 0);
        s_session_ids[ch] = sid;
        if (video) { s_video_session = sid; ESP_LOGI(TAG, "video start, session %d", (int)sid); }
        return;
    }
    case MEDIA_STOP:
        if (video) ESP_LOGI(TAG, "video stop");
        return;
    case MEDIA_CODEC_CONFIG:
        if (video) {
            ESP_LOGD(TAG, "codec config (SPS/PPS) %u bytes", (unsigned)len);
            video_set_codec_config(p, len);
            aap_send_video_ack();
        } else {
            send_media_ack(ch);
        }
        return;
    case MEDIA_DATA:
        if (video) {
            if (len <= 8) { aap_send_video_ack(); return; }
            if (g_app.stage < STAGE_VIDEO) app_set_stage(STAGE_VIDEO);
            // 8-byte timestamp, then the access unit. The video task acks when it takes it.
            if (!video_push_au(p + 8, len - 8, true)) aap_send_video_ack();
        } else {
            send_media_ack(ch);
        }
        return;
    case MEDIA_VIDEO_FOCUS_REQUEST: {
        int mode = (int)pb_get_int(p, len, 2, 1);
        ESP_LOGD(TAG, "phone asked for video focus mode %d, keeping the map projected", mode);
        send_video_focus(VIDEO_FOCUS_PROJECTED, false);
        return;
    }
    default:
        return;
    }
}

// ---- session ----------------------------------------------------------------------------------
static bool version_exchange(void) {
    static const uint8_t ver[4] = { 0, 1, 0, 1 };  // major 1, minor 1 (aasdk)
    if (!send_message(CH_CONTROL, MSG_VERSION_REQUEST, ver, 4)) return false;
    frame_hdr h;
    if (!read_frame(&h, 8000)) { app_set_error("No version answer from phone"); return false; }
    if (h.len < 8 || ((s_rx_frame[0] << 8) | s_rx_frame[1]) != MSG_VERSION_RESPONSE) {
        app_set_error("Unexpected first message from phone");
        return false;
    }
    int major = (s_rx_frame[2] << 8) | s_rx_frame[3];
    int minor = (s_rx_frame[4] << 8) | s_rx_frame[5];
    int16_t status = (int16_t)((s_rx_frame[6] << 8) | s_rx_frame[7]);
    ESP_LOGI(TAG, "version response %d.%d status %d", major, minor, status);
    if (status != 0) { app_set_error("Phone rejected version (%d)", status); return false; }
    app_set_stage(STAGE_VERSION);
    return true;
}

static bool tls_handshake(void) {
    int64_t t0 = esp_timer_get_time();
    mbedtls_ssl_free(&s_ssl);
    mbedtls_ssl_init(&s_ssl);
    int ret = mbedtls_ssl_setup(&s_ssl, &s_conf);
    if (ret) { tls_err("ssl setup", ret); return false; }
    mbedtls_ssl_set_bio(&s_ssl, NULL, bio_send, bio_recv, NULL);
    s_bio_in_len = s_bio_in_pos = 0;
    s_bio_out_len = 0;
    for (;;) {
        ret = mbedtls_ssl_handshake(&s_ssl);
        if (ret != 0 && ret != MBEDTLS_ERR_SSL_WANT_READ && ret != MBEDTLS_ERR_SSL_WANT_WRITE) {
            tls_err("handshake", ret);
            app_set_error("TLS handshake failed (-0x%04X)", -ret);
            return false;
        }
        if (s_bio_out_len) {
            // Our whole flight goes out as one SSL_HANDSHAKE message.
            static uint8_t *flight = (uint8_t *)heap_caps_malloc(BIO_CAP, MALLOC_CAP_SPIRAM);
            size_t n = s_bio_out_len;
            memcpy(flight, s_bio_out, n);
            s_bio_out_len = 0;
            if (!send_message(CH_CONTROL, MSG_SSL_HANDSHAKE, flight, n)) return false;
        }
        if (ret == 0) break;
        if (ret == MBEDTLS_ERR_SSL_WANT_WRITE) continue;
        // Need the phone's next flight. Skip late version responses (Open Headunit does too).
        frame_hdr h;
        do {
            if (!read_frame(&h, 10000)) { app_set_error("TLS: phone stopped answering"); return false; }
        } while (h.len >= 2 && ((s_rx_frame[0] << 8) | s_rx_frame[1]) == MSG_VERSION_RESPONSE);
        if (h.len < 2 || ((s_rx_frame[0] << 8) | s_rx_frame[1]) != MSG_SSL_HANDSHAKE) {
            ESP_LOGE(TAG, "expected SSL handshake data, got 0x%02X%02X", s_rx_frame[0], s_rx_frame[1]);
            return false;
        }
        static uint8_t *in = (uint8_t *)heap_caps_malloc(RX_FRAME_CAP, MALLOC_CAP_SPIRAM);
        memcpy(in, s_rx_frame + 2, h.len - 2);
        s_bio_in = in; s_bio_in_len = h.len - 2; s_bio_in_pos = 0;
    }
    ESP_LOGI(TAG, "TLS handshake done in %d ms (%s)", (int)((esp_timer_get_time() - t0) / 1000),
             mbedtls_ssl_get_ciphersuite(&s_ssl));
    static const uint8_t ok[2] = { 0x08, 0x00 };  // AuthComplete { status = 0 }
    if (!send_message(CH_CONTROL, MSG_AUTH_COMPLETE, ok, 2)) return false;
    s_encrypted = true;
    app_set_stage(STAGE_TLS);
    return true;
}

// When the board ends a session (timeout, decrypt error, ...), tell the phone first. A socket that
// just disappears leaves Android Auto's headunit server holding the dead session, and the next
// connection then sticks at "version" until the server is restarted on the phone.
static void say_goodbye(void) {
    uint8_t b[4]; pb_w w; pb_init(&w, b, sizeof b); pb_varint(&w, 1, 1);  // ByeByeRequest { USER_SELECTION }
    if (!send_pb(CH_CONTROL, MSG_BYEBYE_REQUEST, &w)) return;  // link already gone
    // Give the phone up to 1 s to answer; anything else that arrives meanwhile is decrypted
    // (to keep the TLS stream in step) and dropped.
    int64_t until = esp_timer_get_time() + 1000000;
    while (esp_timer_get_time() < until) {
        frame_hdr h;
        if (!read_frame(&h, 300)) break;
        if (!(h.flags & F_ENCRYPTED)) continue;
        int n = decrypt_into(&h, s_scratch, RX_FRAME_CAP);
        if (n < 0) break;
        if (h.ch == CH_CONTROL && (h.flags & F_FIRST) && n >= 2 &&
            ((s_scratch[0] << 8) | s_scratch[1]) == MSG_BYEBYE_RESPONSE) {
            ESP_LOGI(TAG, "phone acknowledged bye-bye");
            return;
        }
    }
    ESP_LOGW(TAG, "bye-bye sent, no answer");
}

static void run_session(void) {
    s_encrypted = false;
    s_alive = true;
    s_sensor_open = false;
    s_bye = false;
    s_video_session = 0;
    memset(s_session_ids, 0, sizeof s_session_ids);
    for (auto &a : s_asm) a.active = false;
    ulTaskNotifyValueClear(s_ack_task, UINT32_MAX);  // no stale ACKs from the last session

    if (!version_exchange() || !tls_handshake()) return;
    g_app.error[0] = 0;

    while (!s_bye) {
        frame_hdr h;
        if (!read_frame(&h, 20000)) {
            if (g_app.stage < STAGE_VIDEO) app_set_error("Phone went quiet");
            else app_set_error("Connection lost");
            break;
        }
        esp_task_wdt_reset();
        g_aap_frames++;
        if (h.ch >= NCH) { ESP_LOGW(TAG, "frame on channel %d ignored", h.ch); continue; }
        assembly &a = s_asm[h.ch];
        if (h.flags & F_FIRST) {
            if (a.active) ESP_LOGW(TAG, "ch %d: unfinished message dropped", h.ch);
            a.len = 0;
            a.active = true;
            a.overflow = false;
        } else if (!a.active) {
            ESP_LOGW(TAG, "ch %d: continuation without a start, dropped", h.ch);
            continue;
        }
        if (h.flags & F_ENCRYPTED) {
            // Every frame must still be decrypted to keep the TLS record sequence in step.
            if (!a.overflow && a.cap - a.len < h.len) a.overflow = true;
            int n = a.overflow ? decrypt_into(&h, s_scratch, RX_FRAME_CAP) : decrypt_into(&h, a.buf + a.len, a.cap - a.len);
            if (n < 0) { app_set_error("Decrypt failed"); break; }
            if (!a.overflow) a.len += n;
        } else {
            if (a.len + h.len > a.cap) { a.active = false; continue; }
            memcpy(a.buf + a.len, s_rx_frame, h.len);
            a.len += h.len;
        }
        if (h.flags & F_LAST) {
            a.active = false;
            if (a.overflow) {
                ESP_LOGW(TAG, "ch %d: message bigger than %u KB dropped", h.ch, (unsigned)(a.cap >> 10));
                if (h.ch == CH_VIDEO) aap_send_video_ack();
                continue;
            }
            handle_message(h.ch, h.flags, a.buf, a.len);
        }
    }
    if (s_restart) app_set_error("Switching layout...");  // the phone may just close instead of answering
    else if (s_bye) app_set_error("Phone ended Android Auto");
    else if (s_encrypted) say_goodbye();
}

static void close_socket(void) {
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    s_alive = false;
    s_encrypted = false;
    if (s_sock >= 0) { shutdown(s_sock, SHUT_RDWR); close(s_sock); s_sock = -1; }
    xSemaphoreGive(s_tx_lock);
}

static bool connect_phone(const char *ip) {
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (s < 0) return false;
    struct sockaddr_in addr = {};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(CONFIG_AAM_PHONE_PORT);
    inet_pton(AF_INET, ip, &addr.sin_addr);
    struct timeval tv = { 3, 0 };
    setsockopt(s, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    tv = { 1, 0 };
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    int one = 1;
    setsockopt(s, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof one);
    // Non-blocking connect with a 4 s limit: a phone that dozes on WiFi makes a blocking
    // connect hang ~18 s (ARP retries), long enough to trip the task watchdog.
    int fl = fcntl(s, F_GETFL, 0);
    fcntl(s, F_SETFL, fl | O_NONBLOCK);
    int rc = connect(s, (struct sockaddr *)&addr, sizeof addr);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wf; FD_ZERO(&wf); FD_SET(s, &wf);
        struct timeval ct = { 4, 0 };
        rc = select(s + 1, NULL, &wf, NULL, &ct) == 1 ? 0 : -1;
        if (rc == 0) {
            int err = 0; socklen_t el = sizeof err;
            getsockopt(s, SOL_SOCKET, SO_ERROR, &err, &el);
            if (err) { errno = err; rc = -1; }
        } else {
            errno = EHOSTUNREACH;
        }
    }
    fcntl(s, F_SETFL, fl);
    if (rc != 0) {
        int e = errno;
        close(s);
        ESP_LOGW(TAG, "connect %s:%d failed (errno %d%s)", ip, CONFIG_AAM_PHONE_PORT, e,
                 e == ECONNREFUSED ? ", refused: is the headunit server started?" : "");
        app_set_error(e == ECONNREFUSED ? "Port %d refused: start headunit server" : "Phone not reachable (%d)",
                      e == ECONNREFUSED ? CONFIG_AAM_PHONE_PORT : e);
        return false;
    }
    s_sock = s;
    return true;
}

static void aap_task(void *) {
    esp_task_wdt_add(NULL);
    char ip[16];
    int silent = 0;
#if CONFIG_AAM_SERIAL_TRANSPORT
    for (;;) {
        esp_task_wdt_reset();
        if (!s_ser_hello) { vTaskDelay(pdMS_TO_TICKS(100)); continue; }
        s_ser_hello = false;
        xStreamBufferReset(s_ser_rx);
        strcpy(g_app.phone_ip, "USB test");
        s_sock = 0;  // marks the link as open for send_message
        ESP_LOGI(TAG, "fake phone on USB serial");
        app_set_stage(STAGE_TCP);
        run_session();
        s_alive = false; s_encrypted = false; s_sock = -1;
        video_reset();
        ESP_LOGW(TAG, "serial session ended: %s", g_app.error);
        app_set_stage(STAGE_PHONE);
    }
#endif
    for (;;) {
        esp_task_wdt_reset();
        if (!net_phone_ip(ip, sizeof ip)) {
            if (g_app.stage > STAGE_AP) app_set_stage(STAGE_AP);
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        if (g_app.stage < STAGE_PHONE || g_app.stage > STAGE_PHONE) app_set_stage(STAGE_PHONE);
        ESP_LOGI(TAG, "connecting to %s:%d", ip, CONFIG_AAM_PHONE_PORT);
        if (connect_phone(ip)) {
            int64_t t0 = esp_timer_get_time();
            ESP_LOGI(TAG, "TCP connected (go/no-go passed)");
            app_set_stage(STAGE_TCP);
            run_session();
            close_socket();
            video_reset();
            ESP_LOGW(TAG, "session ended after %d s: %s", (int)((esp_timer_get_time() - t0) / 1000000), g_app.error);
            // Android Auto's headunit server is a serial proxy: once one connection is parked
            // inside the phone, later ones are accepted and never answered (Open Headunit,
            // UnresponsivePeerPolicy.kt). Retrying fast only strands more sockets, so back off
            // and tell the user to restart the server.
            silent = g_app.stage == STAGE_TCP ? silent + 1 : 0;
            app_set_stage(STAGE_PHONE);
        }
        int wait_ms = s_restart ? 300 : 3000;
        s_restart = false;
        if (silent >= 3) {
            app_set_error("Phone stuck: Stop + Start headunit server");
            if (silent == 3) ESP_LOGW(TAG, "phone accepts but never answers: restart its headunit server; retrying every 20 s");
            wait_ms = 20000;
        }
        char ip2[16];
        for (int t = 0; t < wait_ms; t += 500) {
            esp_task_wdt_reset();
            vTaskDelay(pdMS_TO_TICKS(500));
            if (!net_phone_ip(ip2, sizeof ip2) || strcmp(ip, ip2)) { silent = 0; break; }  // phone rejoined
        }
    }
}

void aap_start(void) {
    s_tx_lock = xSemaphoreCreateMutex();
    s_rx_frame = (uint8_t *)heap_caps_malloc(RX_FRAME_CAP, MALLOC_CAP_SPIRAM);
    s_tx_buf = (uint8_t *)heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    s_bio_out = (uint8_t *)heap_caps_malloc(BIO_CAP, MALLOC_CAP_SPIRAM);
    s_scratch = (uint8_t *)heap_caps_malloc(RX_FRAME_CAP, MALLOC_CAP_SPIRAM);
    for (int i = 0; i < NCH; i++) {
        s_asm[i].cap = i == CH_VIDEO ? VIDEO_MSG_CAP : OTHER_MSG_CAP;
        s_asm[i].buf = (uint8_t *)heap_caps_malloc(s_asm[i].cap, MALLOC_CAP_SPIRAM);
        assert(s_asm[i].buf);
    }
    assert(s_rx_frame && s_tx_buf && s_bio_out);
    esp_timer_create_args_t ta = {};
    ta.callback = focus_regain_cb;
    ta.name = "focus";
    esp_timer_create(&ta, &s_focus_timer);
    if (!tls_init()) { app_set_error("TLS init failed"); return; }
#if CONFIG_AAM_SERIAL_TRANSPORT
    s_ser_rx = xStreamBufferCreateWithCaps(64 * 1024, 1, MALLOC_CAP_SPIRAM);
    xTaskCreatePinnedToCore(ser_reader_task, "ser_rx", 4096, NULL, 9, NULL, 0);
#endif
    xTaskCreatePinnedToCore(ack_task, "ack", 4096, NULL, 9, &s_ack_task, 0);
    xTaskCreatePinnedToCore(aap_task, "aap", 12288, NULL, 8, NULL, 0);
}
