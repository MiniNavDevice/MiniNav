// Minimal protobuf (proto2 wire format) writer and reader for the few Android Auto messages
// this head unit sends and reads. Field numbers come from Open Headunit's .proto files.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    bool overflow;
} pb_w;

static inline void pb_init(pb_w *w, uint8_t *buf, size_t cap) {
    w->buf = buf; w->len = 0; w->cap = cap; w->overflow = false;
}

static inline void pb_raw_varint(pb_w *w, uint64_t v) {
    do {
        uint8_t b = v & 0x7f;
        v >>= 7;
        if (v) b |= 0x80;
        if (w->len < w->cap) w->buf[w->len++] = b; else w->overflow = true;
    } while (v);
}

static inline void pb_varint(pb_w *w, uint32_t field, uint64_t v) {
    pb_raw_varint(w, (uint64_t)field << 3);
    pb_raw_varint(w, v);
}

// int32/enum fields: negative values are sign-extended to 64 bits on the wire.
static inline void pb_int(pb_w *w, uint32_t field, int64_t v) { pb_varint(w, field, (uint64_t)v); }

static inline void pb_bool(pb_w *w, uint32_t field, bool v) { pb_varint(w, field, v ? 1 : 0); }

static inline void pb_bytes(pb_w *w, uint32_t field, const void *data, size_t n) {
    pb_raw_varint(w, ((uint64_t)field << 3) | 2);
    pb_raw_varint(w, n);
    if (w->len + n <= w->cap) { memcpy(w->buf + w->len, data, n); w->len += n; }
    else w->overflow = true;
}

static inline void pb_string(pb_w *w, uint32_t field, const char *s) { pb_bytes(w, field, s, strlen(s)); }

// Embedded message: build it in its own pb_w, then append.
static inline void pb_msg(pb_w *w, uint32_t field, const pb_w *sub) {
    if (sub->overflow) w->overflow = true;
    pb_bytes(w, field, sub->buf, sub->len);
}

// ---- reader ----
typedef struct {
    const uint8_t *p;
    const uint8_t *end;
} pb_r;

typedef struct {
    uint32_t field;
    uint32_t wire;
    uint64_t v;          // varint / fixed value
    const uint8_t *data; // length-delimited payload
    size_t len;
} pb_field;

static inline bool pb_read_varint(pb_r *r, uint64_t *out) {
    uint64_t v = 0;
    for (int shift = 0; shift < 64 && r->p < r->end; shift += 7) {
        uint8_t b = *r->p++;
        v |= (uint64_t)(b & 0x7f) << shift;
        if (!(b & 0x80)) { *out = v; return true; }
    }
    return false;
}

// Returns false at the end of the buffer or on malformed input.
static inline bool pb_next(pb_r *r, pb_field *f) {
    if (r->p >= r->end) return false;
    uint64_t key;
    if (!pb_read_varint(r, &key)) return false;
    f->field = (uint32_t)(key >> 3);
    f->wire = key & 7;
    f->data = NULL; f->len = 0; f->v = 0;
    switch (f->wire) {
    case 0: return pb_read_varint(r, &f->v);
    case 1: if (r->end - r->p < 8) return false; memcpy(&f->v, r->p, 8); r->p += 8; return true;
    case 5: if (r->end - r->p < 4) return false; { uint32_t x; memcpy(&x, r->p, 4); f->v = x; } r->p += 4; return true;
    case 2: {
        uint64_t n;
        if (!pb_read_varint(r, &n) || (uint64_t)(r->end - r->p) < n) return false;
        f->data = r->p; f->len = (size_t)n; r->p += n; return true;
    }
    default: return false;
    }
}

// First varint value of `field` in a message, or `dflt`.
static inline int64_t pb_get_int(const uint8_t *data, size_t len, uint32_t field, int64_t dflt) {
    pb_r r = { data, data + len };
    pb_field f;
    while (pb_next(&r, &f)) if (f.field == field && f.wire == 0) return (int64_t)f.v;
    return dflt;
}

#ifdef __cplusplus
}
#endif
