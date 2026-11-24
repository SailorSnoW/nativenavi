// Joy-Con (Switch 1) report 0x30: buttons 3..5, sticks 6..11, three 12-byte IMU samples at 13 (accel then gyro XYZ, int16 LE, 5 ms apart, newest last).
// macOS streams it but zeroes the IMU until EnableIMU (0x40), which pad_jc1_mac.h sends. Pure (tests/t_jc1.c).
// Chip axes: +X toward the top, +Y the short axis, +Z out of the face (left) or the back (right). Switch frame: x short (right), y long (top), z out of the face:
// (-Y, +X, +Z) left, (+Y, +X, -Z) right, gyro too. Acceleration negated: the host wants gravity, not specific force.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>
#include "pad_pro.h"
#include "hle_haptics.h"

enum {
    JC1_REPORT_LEN = 49,
    JC1_IMU_PERIOD_NS = 5000000,
    JC1_IMU_SAMPLES = 3,
    JC1_SUB_DEVICE_INFO = 0x02,
    JC1_SUB_INPUT_MODE = 0x03,
    JC1_SUB_SPI_READ = 0x10,
    JC1_SUB_LEDS = 0x30,
    JC1_SUB_ENABLE_IMU = 0x40,
    JC1_SUB_VIBRATION = 0x48,
    JC1_INPUT_MODE_FULL = 0x30,
    JC1_SPI_IMU_CAL = 0x6020,
    JC1_IMU_CAL_LEN = 0x18,
};
// keeps the actuators quiet while the host talks
static const uint8_t jc1_neutral_rumble[8] = {0x00, 0x01, 0x40, 0x40, 0x00, 0x01, 0x40, 0x40};

// id, counter, 8 rumble bytes, subcommand, argument (at most 38 bytes)
static inline void jc1_output_report(uint8_t out[JC1_REPORT_LEN], uint8_t counter, uint8_t sub, const uint8_t *arg, size_t n) {
    memset(out, 0, JC1_REPORT_LEN);
    out[0] = 0x01;
    out[1] = (uint8_t)(counter & 0x0f);
    memcpy(out + 2, jc1_neutral_rumble, sizeof jc1_neutral_rumble);
    out[10] = sub;
    if (arg && n) memcpy(out + 11, arg, n > 38 ? 38 : n);
}
// The console's handshake, one subcommand a step 60 ms apart (one too soon is dropped); report mode before the sensor. 0 past the last step.
enum { JC1_HANDSHAKE_SUBS = 7 };
static inline uint8_t jc1_handshake_report(int step, uint8_t counter, uint8_t out[JC1_REPORT_LEN]) {
    static const uint8_t sub[JC1_HANDSHAKE_SUBS] = {JC1_SUB_DEVICE_INFO, JC1_SUB_INPUT_MODE, JC1_SUB_ENABLE_IMU, JC1_SUB_VIBRATION, JC1_SUB_SPI_READ, JC1_SUB_SPI_READ, JC1_SUB_SPI_READ};
    static const uint32_t spi_at[3] = {JC1_SPI_IMU_CAL, PAD_PRO_SPI_FACTORY, PAD_PRO_SPI_USER};
    static const uint8_t spi_len[3] = {JC1_IMU_CAL_LEN, PAD_PRO_SPI_FACTORY_LEN, PAD_PRO_SPI_USER_LEN};
    if (step < 0 || step >= JC1_HANDSHAKE_SUBS) return 0;
    uint8_t arg[5] = {0}; size_t n = 0;
    if (step == 1) { arg[0] = JC1_INPUT_MODE_FULL; n = 1; }
    else if (step == 2 || step == 3) { arg[0] = 1; n = 1; }
    else if (step >= 4) {
        uint32_t a = spi_at[step - 4];
        arg[0] = a & 0xff; arg[1] = a >> 8 & 0xff; arg[2] = a >> 16 & 0xff; arg[3] = a >> 24 & 0xff;
        arg[4] = spi_len[step - 4];
        n = 5;
    }
    jc1_output_report(out, counter, sub[step], arg, n);
    return sub[step];
}
// ack bit at 13, subcommand at 14; -1: not a reply
static inline int jc1_reply_sub(const uint8_t *r, size_t len) { return len >= 15 && r[0] == 0x21 && (r[13] & 0x80) ? r[14] : -1; }

// SPI 0x6020: accel origin, coeff, gyro origin, coeff (3 × int16 LE). The gyro is 816 or 936 / (coeff - origin) deg/s per count and units store either:
// each axis takes the constant nearest the part's 0.0607 deg/s per count.
typedef struct { float accel_scale[3], gyro_scale[3], gyro_origin[3]; int have; } jc1_cal;
static inline int16_t jc1_s16(const uint8_t *b) { return (int16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8); }
static inline void jc1_cal_default(jc1_cal *c) {
    memset(c, 0, sizeof *c);
    for (int i = 0; i < 3; i++) { c->accel_scale[i] = 1.f / 4096.f; c->gyro_scale[i] = 0.061f; }
}
// SPI read reply: 0x90 0x10 <address LE> <length> <data>. 1: the IMU block
static inline int jc1_parse_cal(jc1_cal *c, const uint8_t *r, size_t len) {
    if (len < 20 + JC1_IMU_CAL_LEN || r[0] != 0x21 || r[13] != 0x90 || r[14] != JC1_SUB_SPI_READ) return 0;
    uint32_t address = (uint32_t)r[15] | (uint32_t)r[16] << 8 | (uint32_t)r[17] << 16 | (uint32_t)r[18] << 24;
    if (address != JC1_SPI_IMU_CAL || r[19] < JC1_IMU_CAL_LEN) return 0;
    const uint8_t *d = r + 20;
    for (int i = 0; i < 3; i++) {
        float accel_origin = (float)jc1_s16(d + 2 * i), accel_coeff = (float)jc1_s16(d + 6 + 2 * i);
        float gyro_origin = (float)jc1_s16(d + 12 + 2 * i), gyro_coeff = (float)jc1_s16(d + 18 + 2 * i);
        if (fabsf(accel_coeff - accel_origin) > 1.f) c->accel_scale[i] = 4.f / (accel_coeff - accel_origin);
        float denom = gyro_coeff - gyro_origin;
        if (fabsf(denom) > 1.f) {
            float accurate = 816.f / denom, saturation_free = 936.f / denom;
            c->gyro_scale[i] = fabsf(accurate - 0.061f) <= fabsf(saturation_free - 0.061f) ? accurate : saturation_free;
        }
        c->gyro_origin[i] = gyro_origin;
    }
    c->have = 1;
    return 1;
}

// The factory origin leaves a drift: the mean of a still second corrects it (blended). A mean past 60 counts (3.7 deg/s) is a slow turn. As s2_rest_update.
#define JC1_REST_N 200   // about a second, 5 ms apart
typedef struct { int n, have; int32_t sum[3]; int16_t gmin[3], gmax[3], amin[3], amax[3]; float bias[3]; } jc1_rest;
static inline void jc1_rest_update(jc1_rest *r, const int16_t res[6]) {   // accel counts, then gyro counts against the factory's origin
    if (!r->n)
        for (int i = 0; i < 3; i++) {
            r->sum[i] = 0;
            r->gmin[i] = r->gmax[i] = res[3 + i];
            r->amin[i] = r->amax[i] = res[i];
        }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += res[3 + i];
        if (res[3 + i] < r->gmin[i]) r->gmin[i] = res[3 + i];
        if (res[3 + i] > r->gmax[i]) r->gmax[i] = res[3 + i];
        if (res[i] < r->amin[i]) r->amin[i] = res[i];
        if (res[i] > r->amax[i]) r->amax[i] = res[i];
        if (r->gmax[i] - r->gmin[i] > 100 || r->amax[i] - r->amin[i] > 300) { r->n = 0; return; }
    }
    if (++r->n < JC1_REST_N) return;
    r->n = 0;
    float m[3];
    for (int i = 0; i < 3; i++) {
        m[i] = (float)r->sum[i] / JC1_REST_N;
        if (fabsf(m[i]) > 60.f) return;
    }
    for (int i = 0; i < 3; i++) r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i];
    r->have = 1;
}

// clock: the newest sample's time, 15 ms a report slewed toward arrival (stamped from arrival, an early report's samples would be dropped). data_ns: last sensor data.
typedef struct { int seen; uint8_t counter; uint64_t received, clock, emitted, data_ns; } jc1_stream;
typedef struct { float acc[3], rot[3]; uint64_t ns; } jc1_sample;
enum { JC1_STALE_NS = 300000000 };
typedef struct { jc1_cal cal; jc1_rest rest; jc1_stream stream; } jc1_unit;
static inline void jc1_unit_init(jc1_unit *u) { memset(u, 0, sizeof *u); jc1_cal_default(&u->cal); }
// A rest learned against another origin starts again; the same calibration read again keeps it.
static inline int jc1_unit_cal(jc1_unit *u, const uint8_t *r, size_t len) {
    jc1_cal c = u->cal;
    if (!jc1_parse_cal(&c, r, len)) return 0;
    for (int i = 0; i < 3; i++) if (c.gyro_origin[i] != u->cal.gyro_origin[i]) { memset(&u->rest, 0, sizeof u->rest); break; }
    u->cal = c;
    return 1;
}
static inline int jc1_live(const jc1_stream *s, uint64_t now) { return s->data_ns && now >= s->data_ns && now - s->data_ns < JC1_STALE_NS; }
// no sensor data for wait (silent, or reports with the sensor off after a wake)
static inline int jc1_quiet(const jc1_stream *s, uint64_t asked, uint64_t wait, uint64_t now) {
    uint64_t since = s->data_ns > asked ? s->data_ns : asked;
    return now >= since && now - since >= wait;
}

// 0 for short, foreign, repeated or all-zero sensor reports (sensor off: a real gyro is never 0 three times). acc: gravity in G; rot: rad/s; Switch frame.
static inline int jc1_input(jc1_unit *u, const uint8_t *r, size_t len, int left, uint64_t now, jc1_sample out[JC1_IMU_SAMPLES]) {
    jc1_stream *s = &u->stream;
    if (len < JC1_REPORT_LEN || r[0] != 0x30 || now < 2 * (uint64_t)JC1_IMU_PERIOD_NS) return 0;
    if (s->seen && (now < s->received || now - s->received > JC1_STALE_NS)) *s = (jc1_stream){0};
    if (s->seen && s->counter == r[1]) return 0;
    int16_t v[JC1_IMU_SAMPLES][6];
    int data = 0;
    for (int k = 0; k < JC1_IMU_SAMPLES; k++)
        for (int i = 0; i < 6; i++) {
            v[k][i] = jc1_s16(r + 13 + 12 * k + 2 * i);
            data |= v[k][i] != 0;
        }
    s->seen = 1;
    s->counter = r[1];
    s->received = now;
    if (!data) return 0;
    s->data_ns = now;
    for (int k = 0; k < JC1_IMU_SAMPLES; k++) {
        int16_t res[6];
        for (int i = 0; i < 3; i++) { res[i] = v[k][i]; res[3 + i] = (int16_t)(v[k][3 + i] - (int)u->cal.gyro_origin[i]); }
        jc1_rest_update(&u->rest, res);
    }
    float offset[3];
    for (int i = 0; i < 3; i++) offset[i] = u->cal.gyro_origin[i] + (u->rest.have ? u->rest.bias[i] : 0.f);
    int64_t drift = (int64_t)now - (int64_t)s->clock;
    if (!s->clock || drift > 100000000 || drift < -100000000) s->clock = now;   // first report, or after a stall
    else s->clock = (uint64_t)((int64_t)s->clock + drift / 4);                   // pulled a quarter toward the arrival
    // Samples end at the clock, never after arrival, none repeated: an early report gives fewer than three.
    uint64_t newest = s->clock > now ? now : s->clock;
    s->clock += (uint64_t)JC1_IMU_SAMPLES * JC1_IMU_PERIOD_NS;
    const float deg = (float)M_PI / 180.f;
    const jc1_cal *cal = &u->cal;
    int n = 0;
    for (int k = 0; k < JC1_IMU_SAMPLES; k++) {
        uint64_t t = newest - (uint64_t)(2 - k) * JC1_IMU_PERIOD_NS;
        if (s->emitted && t <= s->emitted) continue;
        float ax = v[k][0] * cal->accel_scale[0], ay = v[k][1] * cal->accel_scale[1], az = v[k][2] * cal->accel_scale[2];
        float gx = (v[k][3] - offset[0]) * cal->gyro_scale[0];
        float gy = (v[k][4] - offset[1]) * cal->gyro_scale[1];
        float gz = (v[k][5] - offset[2]) * cal->gyro_scale[2];
        out[n].acc[0] = -(left ? -ay : ay);   // gravity, not specific force
        out[n].acc[1] = -ax;
        out[n].acc[2] = -(left ? az : -az);
        out[n].rot[0] = (left ? -gy : gy) * deg;
        out[n].rot[1] = gx * deg;
        out[n].rot[2] = (left ? gz : -gz) * deg;
        out[n].ns = t;
        s->emitted = t;
        n++;
    }
    return n;
}

// Byte 3 right (Y X B A SR SL R ZR from bit 0), 4 shared (Minus Plus RStick LStick Home Capture; bit 7 is the grip), 5 left (Down Up Right Left SR SL L ZL).
typedef struct { uint8_t at, bit; uint32_t btn; } jc1_bit;
static inline uint64_t jc1_joy_buttons(const uint8_t *r, int right) {
    static const jc1_bit left[] = {
        {5, 0x01, DM_BTN_DOWN}, {5, 0x02, DM_BTN_UP}, {5, 0x04, DM_BTN_RIGHT}, {5, 0x08, DM_BTN_LEFT}, {5, 0x10, DM_BTN_LSR}, {5, 0x20, DM_BTN_LSL},
        {5, 0x40, DM_BTN_L}, {5, 0x80, DM_BTN_ZL}, {4, 0x01, DM_BTN_MINUS}, {4, 0x08, DM_BTN_LS},
    }, rightmap[] = {
        {3, 0x01, DM_BTN_Y}, {3, 0x02, DM_BTN_X}, {3, 0x04, DM_BTN_B}, {3, 0x08, DM_BTN_A}, {3, 0x10, DM_BTN_RSR}, {3, 0x20, DM_BTN_RSL},
        {3, 0x40, DM_BTN_R}, {3, 0x80, DM_BTN_ZR}, {4, 0x02, DM_BTN_PLUS}, {4, 0x04, DM_BTN_RS},
    };
    uint64_t m = 0;
    for (size_t i = 0; i < sizeof left / sizeof *left; i++) {
        const jc1_bit *e = right ? &rightmap[i] : &left[i];
        if (r[e->at] & e->bit) m |= e->btn;
    }
    return m;
}
static inline int jc1_home(const uint8_t *r) { return (r[4] & 0x10) != 0; }   // byte 4 bit 4
// -32767..32767, y up; pad_pro.h decodes and calibrates (SPI 0x603D / 0x8010)
static inline int jc1_joy_stick(pad_pro *s, const uint8_t *r, size_t len, int right, int32_t out[2]) {
    uint64_t m; int32_t st[4];
    if (!pad_pro_decode(s, r, len, &m, st)) return 0;
    out[0] = st[right ? 2 : 0]; out[1] = st[right ? 3 : 1];
    return 1;
}

// Sideways: the left one turned a quarter counterclockwise, the right one clockwise. Upright (x right, y top, z out) becomes (-y, x, z) left, (y, -x, z) right.
static inline void jc1_sideways_vec(const float in[3], int right, float out[3]) {
    float x = in[0], y = in[1];
    out[0] = right ? y : -y; out[1] = right ? -x : x; out[2] = in[2];
}
static inline void jc1_sideways_stick(const int32_t in[2], int right, int32_t out[2]) {
    int32_t x = in[0], y = in[1];
    out[0] = right ? y : -y; out[1] = right ? -x : x;
}
// face buttons by place, rail buttons as L and R, stick button as the left stick's
static inline uint64_t jc1_sideways_buttons(uint64_t b, int right) {
    static const uint64_t from_l[4] = {DM_BTN_DOWN, DM_BTN_LEFT, DM_BTN_RIGHT, DM_BTN_UP}, from_r[4] = {DM_BTN_X, DM_BTN_A, DM_BTN_Y, DM_BTN_B};   // what becomes A, B, X, Y
    static const uint64_t to[4] = {DM_BTN_A, DM_BTN_B, DM_BTN_X, DM_BTN_Y};
    const uint64_t *from = right ? from_r : from_l;
    uint64_t m = b & (DM_BTN_L | DM_BTN_R | DM_BTN_ZL | DM_BTN_ZR | DM_BTN_PLUS | DM_BTN_MINUS);
    for (int i = 0; i < 4; i++) if (b & from[i]) m |= to[i];
    if (b & (DM_BTN_LSL | DM_BTN_RSL)) m |= DM_BTN_L;
    if (b & (DM_BTN_LSR | DM_BTN_RSR)) m |= DM_BTN_R;
    if (b & (DM_BTN_LS | DM_BTN_RS)) m |= DM_BTN_LS;
    return m;
}
// For a game with no Joy-Con style: a pair is a Pro Controller, one alone is held sideways. 0: the pad holds none
static inline int jc1_pad_input(const int have[2], const uint64_t b[2], const int32_t st[2][2], uint64_t *m, int32_t out[4]) {
    out[0] = out[1] = out[2] = out[3] = 0; *m = 0;
    if (have[0] && have[1]) {
        *m = (b[0] | b[1]) & 0xffffu;
        out[0] = st[0][0]; out[1] = st[0][1]; out[2] = st[1][0]; out[3] = st[1][1];
        return 1;
    }
    for (int s = 0; s < 2; s++) if (have[s]) {
        *m = jc1_sideways_buttons(b[s], s);
        jc1_sideways_stick(st[s], s, out);
        return 1;
    }
    return 0;
}
// the right one of a pair (prefer 0: left), unless only the left streams; -1 none
static inline int jc1_merged_side(const int have[2], const int live[2], int prefer) {
    if (have[0] && have[1]) return live[prefer] || !live[!prefer] ? prefer : !prefer;
    return have[1] ? 1 : have[0] ? 0 : -1;
}

// A Joy-Con joins the first pad holding the other side alone, else makes its own. sides[i]: bit 0 left, bit 1 right. -1: a new pad
static inline int jc1_pair_pick(const uint32_t *sides, int n, int right) {
    uint32_t other = right ? 1u : 2u;
    for (int i = 0; i < n; i++) if (sides[i] == other) return i;
    return -1;
}
// Gestures as switch2_proto.h s2_gesture: SL + SR for 3 s splits, L alone + R alone for 3 s pairs.
#define JC1_GESTURE_NS 3000000000ull
enum { JC1_GESTURE_NONE, JC1_GESTURE_SPLIT, JC1_GESTURE_JOIN };
static inline int jc1_gesture(uint64_t b, int right, int paired) {
    uint64_t rails = right ? (DM_BTN_RSL | DM_BTN_RSR) : (DM_BTN_LSL | DM_BTN_LSR);
    if (paired) return (b & rails) == rails ? JC1_GESTURE_SPLIT : JC1_GESTURE_NONE;
    return (b & (right ? DM_BTN_R : DM_BTN_L)) && !(b & rails) ? JC1_GESTURE_JOIN : JC1_GESTURE_NONE;
}
typedef struct { int kind, done; uint64_t since; } jc1_hold;
static inline int jc1_hold_update(jc1_hold *h, int kind, uint64_t now) {
    if (kind != h->kind) { h->kind = kind; h->since = now; h->done = 0; }
    return kind != JC1_GESTURE_NONE && now - h->since >= JC1_GESTURE_NS;
}

// Subcommand 0x30: low nibble lit, high nibble flashing; no player yet flashes all four, as the console does.
static inline uint8_t jc1_led_pattern(int player) {
    static const uint8_t p[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};
    return player >= 0 && player < 8 ? p[player] : 0xF0;
}

// HD rumble (dekuNukem rumble_data_table.md): report 0x10 = id, counter, 4 bytes left, 4 right. Frequency code round(32 log2(f / 10 Hz)): high band 0x61..0xDF
// stored (code - 0x60) * 4 in 9 bits, low 0x41..0xBF as code - 0x40 in 7. Amplitude e 0..100: high stores 2e, low (e >> 1) + 0x40 with e's low bit in bit 15.
static inline int jc1_freq_code(float hz, int lo, int hi) {
    if (!(hz > 0.f)) hz = 160.f;
    long c = lrintf(32.f * log2f(hz / 10.f));
    return c < lo ? lo : c > hi ? hi : (int)c;
}
static inline float jc1_amp_of(int e) { return e <= 0 ? 0.f : e < 16 ? exp2f(e / 4.f) / 120.f : e < 32 ? exp2f(e / 16.f) / 17.f : exp2f(e / 32.f) / 8.7f; }
// nearest on the log scale; none below half of code 1's
static inline int jc1_amp_code(float a) {
    if (!(a >= jc1_amp_of(1) * 0.70710678f)) return 0;
    int lo = 1, hi = 100;
    while (lo < hi) {
        int mid = (lo + hi + 1) / 2;
        if (jc1_amp_of(mid) <= a) lo = mid;
        else hi = mid - 1;
    }
    return lo < 100 && a * a > jc1_amp_of(lo) * jc1_amp_of(lo + 1) ? lo + 1 : lo;
}
static inline void jc1_rumble_encode(const dm_vibration *v, uint8_t out[4]) {
    int hf = (jc1_freq_code(v->frequency_high, 0x61, 0xDF) - 0x60) * 4, lf = jc1_freq_code(v->frequency_low, 0x41, 0xBF) - 0x40;
    int eh = jc1_amp_code(v->amplitude_high), el = jc1_amp_code(v->amplitude_low);
    out[0] = (uint8_t)(hf & 0xff);
    out[1] = (uint8_t)(2 * eh + (hf >> 8));
    out[2] = (uint8_t)(lf | (el & 1) << 7);
    out[3] = (uint8_t)((el >> 1) + 0x40);
}
static inline int jc1_vibration_on(const dm_vibration *v) { return jc1_amp_code(v->amplitude_low) || jc1_amp_code(v->amplitude_high); }
enum { JC1_RUMBLE_LEN = 10 };
// a Joy-Con plays its own half; a Pro Controller both
static inline void jc1_rumble_report(uint8_t out[JC1_RUMBLE_LEN], uint8_t counter, const uint8_t left[4], const uint8_t right[4]) {
    out[0] = 0x10; out[1] = (uint8_t)(counter & 0x0f);
    memcpy(out + 2, left, 4); memcpy(out + 6, right, 4);
}
// its side's in a pair; alone, the stronger of each band (as s2_rumble_pick)
static inline dm_vibration jc1_rumble_pick(const dm_vibration m[2], int right, int paired) {
    if (paired) return m[right];
    dm_vibration v = m[0];
    if (m[1].amplitude_low > v.amplitude_low) { v.amplitude_low = m[1].amplitude_low; v.frequency_low = m[1].frequency_low; }
    if (m[1].amplitude_high > v.amplitude_high) { v.amplitude_high = m[1].amplitude_high; v.frequency_high = m[1].frequency_high; }
    return v;
}
// Reports go every 15 ms, games set vibration every 5 ms: each report plays the strongest value since the last, so short pulses still play.
typedef struct { dm_vibration cur[2], peak[2]; } jc1_rumble_hold;
static inline float jc1_vib_strength(const dm_vibration *v) { return v->amplitude_low + v->amplitude_high; }
static inline void jc1_rumble_set(jc1_rumble_hold *h, const dm_vibration m[2]) {
    for (int k = 0; k < 2; k++) { h->cur[k] = m[k]; if (jc1_vib_strength(&m[k]) >= jc1_vib_strength(&h->peak[k])) h->peak[k] = m[k]; }
}
static inline void jc1_rumble_take(jc1_rumble_hold *h, dm_vibration out[2]) {
    for (int k = 0; k < 2; k++) { out[k] = h->peak[k]; h->peak[k] = h->cur[k]; }
}
