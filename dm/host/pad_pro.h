// Pro Controller report 0x30 for a pad GameController does not list (dekuNukem bluetooth_hid_notes.md); y grows upwards.
// Sticks: SPI calibration (user's at 0x8010, else factory's at 0x603D); until it answers, each axis measures its rest and reach.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include "dm_input.h"

enum { PAD_PRO_MIDDLE = 2048, PAD_PRO_REST_MAX = 500, PAD_PRO_REACH_MIN = 1200, PAD_PRO_DEAD_PCT = 7 };
typedef struct { int seen, cal[2], rest[4], reach[4][2]; } pad_pro;   // cal: 1 factory, 2 user; reach: below, above the rest
enum { PAD_PRO_SPI_USER = 0x8010, PAD_PRO_SPI_USER_LEN = 22, PAD_PRO_SPI_FACTORY = 0x603D, PAD_PRO_SPI_FACTORY_LEN = 18 };

static inline size_t pad_pro_spi_read(uint8_t out[16], uint8_t counter, uint32_t addr, uint8_t len) {
    static const uint8_t rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};
    out[0] = 0x01; out[1] = counter & 0x0f;
    for (int i = 0; i < 8; i++) out[2 + i] = rumble[i];
    out[10] = 0x10; out[11] = addr & 0xff; out[12] = addr >> 8 & 0xff; out[13] = addr >> 16 & 0xff; out[14] = addr >> 24 & 0xff; out[15] = len;
    return 16;
}
// 12-bit x, y: left stick above, center, below; right stick center, below, above
static inline void pad_pro_cal_decode(const uint8_t *d, int right, int c[6]) {   // c: x and y rest, below, above
    int v[6];
    for (int i = 0; i < 3; i++) { v[2 * i] = (d[3 * i + 1] << 8 & 0xf00) | d[3 * i]; v[2 * i + 1] = d[3 * i + 2] << 4 | d[3 * i + 1] >> 4; }
    static const int left[3] = {1, 2, 0}, rightorder[3] = {0, 1, 2};
    const int *o = right ? rightorder : left;
    for (int k = 0; k < 3; k++) { c[2 * k] = v[2 * o[k]]; c[2 * k + 1] = v[2 * o[k] + 1]; }
}
static inline int pad_pro_cal_sane(const int c[6]) {
    for (int a = 0; a < 2; a++) {
        if (abs(c[a] - PAD_PRO_MIDDLE) > PAD_PRO_REST_MAX) return 0;
        for (int k = 1; k < 3; k++) if (c[2 * k + a] < 400 || c[2 * k + a] > PAD_PRO_MIDDLE) return 0;
    }
    return 1;
}
// Report 0x21, data from byte 20; the user's calibration wins. Returns the sticks set (1 left, 2 right).
static inline int pad_pro_spi_reply(pad_pro *s, const uint8_t *r, size_t len) {
    if (len < 20 || r[0] != 0x21 || !(r[13] & 0x80) || r[14] != 0x10) return 0;
    uint32_t addr = r[15] | r[16] << 8 | (uint32_t)r[17] << 16 | (uint32_t)r[18] << 24;
    size_t n = r[19];
    const uint8_t *d = r + 20;
    if (len < 20 + n) return 0;
    int set = 0;
    for (int stick = 0; stick < 2; stick++) {
        const uint8_t *at = NULL; int kind = 0;
        if (addr == PAD_PRO_SPI_USER && n >= PAD_PRO_SPI_USER_LEN && d[11 * stick] == 0xb2 && d[11 * stick + 1] == 0xa1) { at = d + 11 * stick + 2; kind = 2; }
        if (addr == PAD_PRO_SPI_FACTORY && n >= PAD_PRO_SPI_FACTORY_LEN && s->cal[stick] < 2) { at = d + 9 * stick; kind = 1; }
        if (!at) continue;
        int c[6];
        pad_pro_cal_decode(at, stick, c);
        if (!pad_pro_cal_sane(c)) continue;
        for (int a = 0; a < 2; a++) { s->rest[2 * stick + a] = c[a]; s->reach[2 * stick + a][0] = c[2 + a]; s->reach[2 * stick + a][1] = c[4 + a]; }
        s->cal[stick] = kind; set |= 1 << stick;
    }
    if (set) s->seen = 1;
    return set;
}

static inline uint64_t pad_pro_buttons(const uint8_t *r) {
    static const struct { uint8_t at, bit; uint32_t btn; } map[] = {
        {3, 0x01, DM_BTN_Y}, {3, 0x02, DM_BTN_X}, {3, 0x04, DM_BTN_B}, {3, 0x08, DM_BTN_A}, {3, 0x40, DM_BTN_R}, {3, 0x80, DM_BTN_ZR},
        {4, 0x01, DM_BTN_MINUS}, {4, 0x02, DM_BTN_PLUS}, {4, 0x04, DM_BTN_RS}, {4, 0x08, DM_BTN_LS},
        {5, 0x01, DM_BTN_DOWN}, {5, 0x02, DM_BTN_UP}, {5, 0x04, DM_BTN_RIGHT}, {5, 0x08, DM_BTN_LEFT}, {5, 0x40, DM_BTN_L}, {5, 0x80, DM_BTN_ZL},
    };
    uint64_t m = 0;
    for (size_t i = 0; i < sizeof map / sizeof *map; i++) if (r[map[i].at] & map[i].bit) m |= map[i].btn;
    return m;
}
static inline int32_t pad_pro_axis(pad_pro *s, int a, int raw) {
    int v = raw - s->rest[a], mag = abs(v), *reach = &s->reach[a][v > 0];
    if (mag > *reach && !s->cal[a / 2]) *reach = mag;   // a calibrated reach stays; past it reads full
    int dead = *reach * PAD_PRO_DEAD_PCT / 100;
    if (mag <= dead) return 0;
    int64_t out = (int64_t)(mag - dead) * DM_STICK_MAX / (*reach - dead);
    if (out > DM_STICK_MAX) out = DM_STICK_MAX;
    return v < 0 ? -(int32_t)out : (int32_t)out;
}
// st: left x, y, right x, y; 0 for another report
static inline int pad_pro_decode(pad_pro *s, const uint8_t *r, size_t len, uint64_t *m, int32_t st[4]) {
    if (len < 12 || r[0] != 0x30) return 0;
    const int raw[4] = {r[6] | (r[7] & 0x0f) << 8, r[7] >> 4 | r[8] << 4, r[9] | (r[10] & 0x0f) << 8, r[10] >> 4 | r[11] << 4};
    if (!s->seen) {
        s->seen = 1;
        for (int a = 0; a < 4; a++) { s->rest[a] = abs(raw[a] - PAD_PRO_MIDDLE) <= PAD_PRO_REST_MAX ? raw[a] : PAD_PRO_MIDDLE; s->reach[a][0] = s->reach[a][1] = PAD_PRO_REACH_MIN; }
    }
    *m = pad_pro_buttons(r);
    for (int a = 0; a < 4; a++) st[a] = pad_pro_axis(s, a, raw[a]);
    return 1;
}
