// Switch 2 controllers over Bluetooth LE: the protocol and a pad's state, pure C for tests (docs/reference/services.md#switch-2-controllers-over-bluetooth-le).
// A Joy-Con 2's stick sits in its side's slot (left first, right second), its calibration in the first slot (0x130A8).
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "dm_input.h"

#define S2_COMPANY_ID 0x0553
#define S2_VENDOR_ID  0x057E
enum { S2_PID_JOYCON_R = 0x2066, S2_PID_JOYCON_L = 0x2067, S2_PID_PRO = 0x2069 };

// every write is Write Without Response
#define S2_UUID_INPUT   "AB7DE9BE-89FE-49AD-828F-118F09DF7FD2"   // notifications: 63-byte input reports
#define S2_UUID_CMD_WR  "649D4AC9-8EB7-4E6C-AF44-1EA54FE5F005"
#define S2_UUID_CMD_RSP "C765A961-D9D8-4D36-A20A-5315B111836A"   // command replies (notifications)

enum { S2_CMD_MEMORY = 0x02, S2_CMD_LEDS = 0x09, S2_CMD_FEATURE = 0x0C, S2_CMD_PAIR = 0x15 };
enum { S2_SUB_MEMORY_READ = 0x04, S2_SUB_LEDS_SET = 0x07, S2_SUB_FEATURE_SET = 0x02, S2_SUB_FEATURE_ENABLE = 0x04,
       S2_SUB_PAIR_HOST = 0x01, S2_SUB_PAIR_KEY1 = 0x04, S2_SUB_PAIR_KEY2 = 0x02, S2_SUB_PAIR_DONE = 0x03 };
// the console's mask; all bits make a Joy-Con 2 report phantom ZL / ZR
#define S2_FEATURES (0x01 | 0x02 | 0x04 | 0x20 | 0x80)
enum { S2_ADDR_INFO = 0x00013000u, S2_ADDR_STICK1 = 0x00013080u, S2_ADDR_STICK2 = 0x000130C0u, S2_ADDR_USER_STICK1 = 0x001FC042u, S2_ADDR_USER_STICK2 = 0x001FC062u };
// parameters (S2_STICK_PARAMS bytes), then the calibration (9 bytes: 0x130A8, 0x130E8)
enum { S2_STICK_PARAMS = 0x28, S2_STICK_BLOCK = S2_STICK_PARAMS + 9 };

// 32-bit LE at report offset 4, top three bits always set; rail buttons: right 0x10 / 0x20, left 0x100000 / 0x200000 (SR before SL)
enum {
    S2_Y = 0x1, S2_X = 0x2, S2_B = 0x4, S2_A = 0x8, S2_SR_R = 0x10, S2_SL_R = 0x20, S2_R = 0x40, S2_ZR = 0x80, S2_MINUS = 0x100, S2_PLUS = 0x200,
    S2_RSTICK = 0x400, S2_LSTICK = 0x800, S2_HOME = 0x1000, S2_CAPTURE = 0x2000, S2_C = 0x4000,
    S2_DOWN = 0x10000, S2_UP = 0x20000, S2_RIGHT = 0x40000, S2_LEFT = 0x80000, S2_SR_L = 0x100000, S2_SL_L = 0x200000, S2_L = 0x400000, S2_ZL = 0x800000,
    S2_GR = 0x1000000, S2_GL = 0x2000000,
};

static inline uint16_t s2_u16(const uint8_t *p) { return (uint16_t)(p[0] | (uint16_t)p[1] << 8); }
static inline uint32_t s2_u24(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16; }   // a packed 12-bit pair: x low, y high
static inline uint32_t s2_u32(const uint8_t *p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24; }
static inline int16_t s2_s16(const uint8_t *p) { return (int16_t)s2_u16(p); }

typedef struct { uint32_t clock_ms, buttons; uint16_t lx, ly, rx, ry; int16_t raw[6]; uint16_t battery_mv; } s2_report;   // clock_ms: the controller's clock; raw: accel xyz, gyro xyz in counts

// longer packets on the same characteristic are audio
static inline int s2_report_parse(const uint8_t *d, size_t n, s2_report *r) {
    if (r) memset(r, 0, sizeof *r);
    if (!d || !r || n < 0x3C || n > 0x50) return 0;
    r->clock_ms = s2_u32(d); r->buttons = s2_u32(d + 4);
    uint32_t a = s2_u24(d + 10), b = s2_u24(d + 13);
    r->lx = (uint16_t)(a & 0xFFF); r->ly = (uint16_t)(a >> 12); r->rx = (uint16_t)(b & 0xFFF); r->ry = (uint16_t)(b >> 12);
    r->battery_mv = s2_u16(d + 0x1F);
    for (int i = 0; i < 6; i++) r->raw[i] = s2_s16(d + 0x30 + 2 * i);
    return 1;
}

// cmd, 0x91, transport 0x01, subcommand, 0, payload length, 0, 0, payload; 0 when it does not fit
static inline size_t s2_command(uint8_t *out, size_t cap, uint8_t cmd, uint8_t sub, const uint8_t *payload, size_t n) {
    if (!out || n > 0xFF || cap < 8 + n) return 0;
    out[0] = cmd; out[1] = 0x91; out[2] = 0x01; out[3] = sub; out[4] = 0; out[5] = (uint8_t)n; out[6] = out[7] = 0;
    if (n) memcpy(out + 8, payload, n);
    return 8 + n;
}
// length, 0x7E, 0, 0, address (LE); the reply: 8-byte header, these 8 echoed, the data
static inline void s2_memory_payload(uint8_t out[8], uint8_t len, uint32_t addr) {
    out[0] = len; out[1] = 0x7E; out[2] = out[3] = 0;
    out[4] = (uint8_t)addr; out[5] = (uint8_t)(addr >> 8); out[6] = (uint8_t)(addr >> 16); out[7] = (uint8_t)(addr >> 24);
}
#define S2_REPLY_DATA 16
// byte 1: 1 done, 2 status
static inline int s2_reply_is(const uint8_t *d, size_t n, uint8_t cmd) { return d && n >= 8 && d[0] == cmd && (d[1] == 1 || d[1] == 2); }

// manufacturer data after the company id: vid at 3, pid at 5, the wake host at 10 (6 bytes LE), all zero in pairing mode
typedef struct { uint16_t vid, pid; int pairing; uint8_t host[6]; } s2_advert;
static inline int s2_advert_parse(const uint8_t *d, size_t n, s2_advert *a) {
    if (a) memset(a, 0, sizeof *a);
    if (!d || !a || n < 16) return 0;
    a->vid = s2_u16(d + 3); a->pid = s2_u16(d + 5);
    a->pairing = 1; for (int i = 10; i < 16; i++) { a->host[i - 10] = d[i]; if (d[i]) a->pairing = 0; }
    return a->vid == S2_VENDOR_ID && (a->pid == S2_PID_PRO || a->pid == S2_PID_JOYCON_L || a->pid == S2_PID_JOYCON_R);
}
// host NULL: unknown (used without bonding, S2_ADV_TAKE). A wake toward another host (a nearby Switch 2) is never written to.
enum { S2_ADV_IGNORE, S2_ADV_WAKE, S2_ADV_PAIR, S2_ADV_TAKE };
static inline int s2_advert_use(const s2_advert *a, const uint8_t *host) {
    if (a->pairing) return host ? S2_ADV_PAIR : S2_ADV_TAKE;
    return host && !memcmp(a->host, host, 6) ? S2_ADV_WAKE : S2_ADV_IGNORE;
}

// Bonding (0x15): the host's address (0x00, 0x02, the address twice LE), two 16-byte keys after 0x00, then DONE with 0x00. Fresh random
// keys work; the link is not encrypted.
#define S2_PAIR_HOST_LEN 14
#define S2_PAIR_KEY_LEN 17
static inline void s2_pair_host_payload(uint8_t out[S2_PAIR_HOST_LEN], const uint8_t host[6]) { out[0] = 0x00; out[1] = 0x02; memcpy(out + 2, host, 6); memcpy(out + 8, host, 6); }
static inline void s2_pair_key_payload(uint8_t out[S2_PAIR_KEY_LEN], const uint8_t key[16]) { out[0] = 0x00; memcpy(out + 1, key, 16); }

// 0x40 bytes: serial at 2 (14 characters), vid at 18, pid at 20
static inline int s2_info_parse(const uint8_t *d, size_t n, uint16_t *vid, uint16_t *pid, char serial[15]) {
    if (!d || n < 0x16) return 0;
    *vid = s2_u16(d + 18); *pid = s2_u16(d + 20);
    memcpy(serial, d + 2, 14); serial[14] = 0;
    for (int i = 0; i < 14; i++) if (serial[i] < 0x20 || serial[i] > 0x7e) serial[i] = '?';
    return 1;
}

// 0x0B bytes: centre, max and min travel, each a packed 12-bit pair
typedef struct { float cx, cy, maxx, maxy, minx, miny; int valid; float dead; } s2_cal;   // dead: raw counts, 0 when unknown
static inline void s2_cal_default(s2_cal *c) { c->cx = c->cy = 2048; c->maxx = c->maxy = c->minx = c->miny = 1200; c->valid = 0; c->dead = 0; }
static inline int s2_cal_blank(const uint8_t *d, size_t n) { return !d || n < 3 || (d[0] == 0xFF && d[1] == 0xFF && d[2] == 0xFF); }
static inline int s2_cal_parse(const uint8_t *d, size_t n, s2_cal *c) {
    if (!d || !c || n < 9 || s2_cal_blank(d, n)) return 0;
    uint32_t p = s2_u24(d), q = s2_u24(d + 3), r = s2_u24(d + 6);
    c->cx = (float)(p & 0xFFF); c->cy = (float)(p >> 12);
    c->maxx = (float)(q & 0xFFF); c->maxy = (float)(q >> 12);   // travel above and below the centre, not positions
    c->minx = (float)(r & 0xFFF); c->miny = (float)(r >> 12);
    c->valid = c->maxx > 100 && c->maxy > 100 && c->minx > 100 && c->miny > 100;
    return c->valid;
}
// A format byte, then 13 packed 12-bit pairs; the third pair is taken as the dead zone (inferred, trusted only within S2_DEAD_MIN..S2_DEAD_MAX:
// docs/reference/services.md#a-switch-2-controllers-stick-at-rest-reads-as-in-use). 0 for a blank or odd block.
static inline float s2_params_dead(const uint8_t *d, size_t n) {
    if (!d || n < S2_STICK_PARAMS || d[0] == 0xFF) return 0;
    const uint8_t *t = d + 1 + 3 * 2;
    uint32_t p = s2_u24(t), x = p & 0xFFF, y = p >> 12;
    if (x != y || x == 0 || x == 0xFFF) return 0;
    return (float)x;
}

// the user's calibration (blank unless calibrated on a console) over the factory's; 2 user, 1 factory, 0 none (s2_cal_default kept)
static inline int s2_cal_from_flash(const uint8_t *block, size_t n, const uint8_t *user, size_t un, s2_cal *c) {
    s2_cal_default(c); s2_cal t;
    if (block && n >= S2_STICK_PARAMS) c->dead = s2_params_dead(block, n);
    float dead = c->dead;
    if (user && s2_cal_parse(user, un, &t)) { *c = t; c->dead = dead; return 2; }
    if (block && n >= S2_STICK_BLOCK && s2_cal_parse(block + S2_STICK_PARAMS, n - S2_STICK_PARAMS, &t)) { *c = t; c->dead = dead; return 1; }
    return 0;
}

// the Pro Controller 2's share
#define S2_DEAD_SHARE 0.10f
#define S2_DEAD_MIN 0.03f
#define S2_DEAD_MAX 0.25f
// 0 when it has none to trust
static inline float s2_cal_dead_own(const s2_cal *c) {
    float travel = (c->maxx + c->minx + c->maxy + c->miny) / 4, share = travel > 0 ? c->dead / travel : 0;
    return share >= S2_DEAD_MIN && share <= S2_DEAD_MAX ? share : 0;
}
static inline float s2_cal_dead_share(const s2_cal *c) { float own = s2_cal_dead_own(c); return own > 0 ? own : S2_DEAD_SHARE; }
// -1..1 (x right, y up): a round dead zone, the rest rescaled from 0 (as pad_hid_stick), each axis clamped after
static inline void s2_cal_apply(const s2_cal *c, uint16_t rawx, uint16_t rawy, float *x, float *y) {
    s2_cal d; if (!c || !c->valid) { s2_cal_default(&d); if (c) d.dead = c->dead; c = &d; }
    float sx = (float)rawx - c->cx, sy = (float)rawy - c->cy;
    float tx = sx >= 0 ? c->maxx : c->minx, ty = sy >= 0 ? c->maxy : c->miny;
    float nx = tx > 0 ? sx / tx : 0, ny = ty > 0 ? sy / ty : 0, r = sqrtf(nx * nx + ny * ny), dead = s2_cal_dead_share(c);
    if (r <= dead) { *x = *y = 0; return; }
    float k = (r - dead) / (1 - dead) / r; nx *= k; ny *= k;
    *x = fminf(fmaxf(nx, -1.f), 1.f); *y = fminf(fmaxf(ny, -1.f), 1.f);
}

static const uint8_t s2_led_pattern[8] = {0x01, 0x03, 0x07, 0x0F, 0x09, 0x05, 0x0D, 0x06};

typedef struct { int have; uint16_t pid; s2_report r; s2_cal cal[2]; } s2_dev;   // cal[0]: the first slot (a Joy-Con's only stick), cal[1]: the Pro Controller 2's right stick
typedef struct { s2_dev pro, left, right; } s2_pad_state;

// a Joy-Con alone is held sideways: its rail buttons are the shoulders, its stick the left
static inline uint64_t s2_buttons(uint32_t b, int joycon, int alone) {
    uint64_t m = 0;
    if (b & S2_A) m |= DM_BTN_A; if (b & S2_B) m |= DM_BTN_B; if (b & S2_X) m |= DM_BTN_X; if (b & S2_Y) m |= DM_BTN_Y;
    if (b & S2_L) m |= DM_BTN_L; if (b & S2_R) m |= DM_BTN_R; if (b & S2_ZL) m |= DM_BTN_ZL; if (b & S2_ZR) m |= DM_BTN_ZR;
    if (b & S2_PLUS) m |= DM_BTN_PLUS; if (b & S2_MINUS) m |= DM_BTN_MINUS; if (b & S2_LSTICK) m |= DM_BTN_LS; if (b & S2_RSTICK) m |= DM_BTN_RS;
    if (b & S2_LEFT) m |= DM_BTN_LEFT; if (b & S2_UP) m |= DM_BTN_UP; if (b & S2_RIGHT) m |= DM_BTN_RIGHT; if (b & S2_DOWN) m |= DM_BTN_DOWN;
    if (joycon && alone) {
        if (b & (S2_SL_L | S2_SL_R)) m |= DM_BTN_L;
        if (b & (S2_SR_L | S2_SR_R)) m |= DM_BTN_R;
    }
    return m;
}

// -32767..32767, y up; a Joy-Con pair is a Pro Controller, one alone is sideways; 0 when empty
static inline int s2_pad_input(const s2_pad_state *p, uint64_t *buttons, int32_t st[4]) {
    float x, y; uint64_t m = 0; st[0] = st[1] = st[2] = st[3] = 0;
#define STICK(i, dev, c, rx_, ry_) do { s2_cal_apply(&(dev)->cal[c], rx_, ry_, &x, &y); st[i] = (int32_t)lrintf(x * DM_STICK_MAX); st[(i) + 1] = (int32_t)lrintf(y * DM_STICK_MAX); } while (0)
    if (p->pro.have) {
        m = s2_buttons(p->pro.r.buttons, 0, 0);
        STICK(0, &p->pro, 0, p->pro.r.lx, p->pro.r.ly); STICK(2, &p->pro, 1, p->pro.r.rx, p->pro.r.ry);
    } else if (p->left.have && p->right.have) {
        m = s2_buttons(p->left.r.buttons, 1, 0) | s2_buttons(p->right.r.buttons, 1, 0);
        STICK(0, &p->left, 0, p->left.r.lx, p->left.r.ly); STICK(2, &p->right, 0, p->right.r.rx, p->right.r.ry);
    } else if (p->left.have) {
        m = s2_buttons(p->left.r.buttons, 1, 1); STICK(0, &p->left, 0, p->left.r.lx, p->left.r.ly);
    } else if (p->right.have) {
        m = s2_buttons(p->right.r.buttons, 1, 1); STICK(0, &p->right, 0, p->right.r.rx, p->right.r.ry);
    } else return 0;
#undef STICK
    *buttons = m;
    return 1;
}

static inline int s2_pad_home(const s2_pad_state *p) { return ((p->pro.have ? p->pro.r.buttons : 0) | (p->left.have ? p->left.r.buttons : 0) | (p->right.have ? p->right.r.buttons : 0)) & S2_HOME ? 1 : 0; }
static inline void s2_pad_ab(const s2_pad_state *p, int *a, int *b) {
    uint32_t m = (p->pro.have ? p->pro.r.buttons : 0) | (p->right.have ? p->right.r.buttons : 0);   // a left Joy-Con alone has no A or B: the prompt also takes the keyboard
    *a |= (m & S2_A) != 0; *b |= (m & S2_B) != 0;
}

// Rumble: a zero byte, then a 16-byte block per actuator: 0x40 | samples (1..3) << 4 | sequence, then five-byte samples of two bands (10-bit
// log frequency code, 96 steps an octave, 333 = 110 Hz; 10-bit linear amplitude). The Pro Controller 2's channel is untested.
#define S2_UUID_VIB_JOYCON_R "FA19B0FB-CD1F-46A7-84A1-BBB09E00C149"
#define S2_UUID_VIB_JOYCON_L "289326CB-A471-485D-A8F4-240C14F18241"
#define S2_UUID_VIB_PRO      "CC483F51-9258-427D-A939-630C31F72B05"
typedef struct { uint16_t lf_hz, lf_amp, hf_hz, hf_amp; } s2_haptic;   // *_hz: frequency codes; *_amp: 0..1023
static inline void s2_haptic_pack(const s2_haptic *v, uint8_t out[5]) {
    uint64_t x = (uint64_t)(v->lf_hz & 0x3FF) | (uint64_t)(v->lf_amp & 0x3FF) << 10 | (uint64_t)(v->hf_hz & 0x3FF) << 20 | (uint64_t)(v->hf_amp & 0x3FF) << 30;
    for (int i = 0; i < 5; i++) out[i] = (uint8_t)(x >> (8 * i));
}
static inline float s2_freq_hz(uint16_t code) { return 110.f * exp2f(((float)code - 333.f) / 96.f); }
// rounded as the console's encoder: clamped to 9.935..16034 Hz, truncated, then the nearer of it and the next
static inline uint16_t s2_freq_code(float hz) {
    if (!(hz >= 9.935f)) hz = 9.935f; if (hz > 16034.f) hz = 16034.f;
    int c = (int)(96.f * log2f(hz) - 318.0105285644531f);
    if (c > 0x3FE) return (uint16_t)(c > 0x3FF ? 0x3FF : c);
    if (c < 0) c = 0;
    return (uint16_t)(fabsf(s2_freq_hz((uint16_t)(c + 1)) - hz) < fabsf(s2_freq_hz((uint16_t)c) - hz) ? c + 1 : c);
}
// truncated, then the nearer of it and the next
static inline uint16_t s2_amp_code(float a) {
    if (!(a > 0.f)) a = 0.f; if (a > 1.f) a = 1.f;
    int c = (int)(a * 1023.f); if (c > 0x3FE) return 0x3FF;
    return (uint16_t)(fabsf((float)(c + 1) / 1023.f - a) < fabsf((float)c / 1023.f - a) ? c + 1 : c);
}
static inline void s2_rumble_cap(float *low, float *high) { float sum = *low + *high, k = sum > 1.f ? 0.98f / sum : 0.98f; *low *= k; *high *= k; }
static inline s2_haptic s2_haptic_from(float amp_low, float freq_low, float amp_high, float freq_high) {
    s2_haptic v = {s2_freq_code(freq_low), s2_amp_code(amp_low), s2_freq_code(freq_high), s2_amp_code(amp_high)};
    return v;
}
static inline int s2_haptic_on(const s2_haptic *v) { return v->lf_amp || v->hf_amp; }
// three samples of the game's 5 ms values per block
static inline size_t s2_rumble_packet3(uint8_t out[33], const s2_haptic first[3], const s2_haptic second[3], uint8_t seq) {
    size_t o = 0;
    out[o++] = 0x00;
    for (int blk = 0; blk < 2; blk++) {
        out[o++] = (uint8_t)(0x40 | 3 << 4 | (seq & 0x0F));
        for (int i = 0; i < 3; i++) {
            s2_haptic_pack(blk ? &second[i] : &first[i], out + o);
            o += 5;
        }
    }
    return o;
}
// a Pro Controller 2 plays both motors (one per grip), a paired Joy-Con its side's, one alone the stronger of each band
static inline void s2_rumble_pick(const s2_pad_state *p, const s2_dev *dev, const s2_haptic m[2], s2_haptic *a, s2_haptic *b) {
    if (dev == &p->pro) { *a = m[0]; *b = m[1]; return; }
    int right = dev == &p->right;
    if ((right ? p->left.have : p->right.have)) { *a = *b = m[right]; return; }
    s2_haptic v = m[0];
    if (m[1].lf_amp > v.lf_amp) { v.lf_amp = m[1].lf_amp; v.lf_hz = m[1].lf_hz; }
    if (m[1].hf_amp > v.hf_amp) { v.hf_amp = m[1].hf_amp; v.hf_hz = m[1].hf_hz; }
    *a = *b = v;
}
// bit 0 left, bit 1 right
static inline unsigned s2_rumble_motors(const s2_pad_state *p, const s2_dev *dev) {
    if (dev == &p->pro) return 3;
    int right = dev == &p->right;
    return (right ? p->left.have : p->right.have) ? 1u << right : 3;
}

// Rumble timing: each value the game sets (about every 5 ms) plays once, in the first 5 ms slot ending after it was set, three slots a
// packet, else short pulses last longer. A backlogged value waits at most S2_RUMBLE_BACKLOG slots.
#define S2_RUMBLE_SLOT_NS 5000000ull
#define S2_RUMBLE_BACKLOG 5
#define S2_RUMBLE_LOG 64
typedef struct { uint64_t t[S2_RUMBLE_LOG]; s2_haptic m[S2_RUMBLE_LOG][2]; uint64_t n; } s2_rumble_log;   // oldest first
typedef struct { uint64_t next, end; s2_haptic now[2]; } s2_rumble_cursor;   // the first value not yet played, the end of the last slot, what plays
static inline void s2_rumble_log_add(s2_rumble_log *l, uint64_t t, const s2_haptic m[2]) {
    uint64_t i = l->n % S2_RUMBLE_LOG; l->t[i] = t; l->m[i][0] = m[0]; l->m[i][1] = m[1]; l->n++;
}
static inline int s2_haptic_same(const s2_haptic *a, const s2_haptic *b) { return a->lf_hz == b->lf_hz && a->lf_amp == b->lf_amp && a->hf_hz == b->hf_hz && a->hf_amp == b->hf_amp; }
// a value that changes none of this controller's motors takes no slot
static inline void s2_rumble_slot(const s2_rumble_log *l, s2_rumble_cursor *c, unsigned motors) {
    uint64_t end = c->end += S2_RUMBLE_SLOT_NS, late = S2_RUMBLE_BACKLOG * S2_RUMBLE_SLOT_NS;
    if (c->next > l->n) c->next = l->n;   // another player's log (the pad changed player)
    else if (l->n - c->next > S2_RUMBLE_LOG) c->next = l->n - S2_RUMBLE_LOG;   // overwritten: the oldest kept
    for (; c->next < l->n; c->next++) {
        uint64_t i = c->next % S2_RUMBLE_LOG;
        if (l->t[i] >= end) return;
        if (c->next + 1 < l->n && l->t[(c->next + 1) % S2_RUMBLE_LOG] + late < end) continue;   // a newer one is overdue too: pass this one over
        int changes = 0;
        for (int k = 0; k < 2; k++) if (motors >> k & 1 && !s2_haptic_same(&l->m[i][k], &c->now[k])) changes = 1;
        if (!changes) continue;
        c->now[0] = l->m[i][0]; c->now[1] = l->m[i][1]; c->next++;
        return;
    }
}
// a packet's last slot ends about lag before now, so every value in a slot is known; a cursor a packet off starts again
static inline void s2_rumble_resync(s2_rumble_cursor *c, uint64_t now, uint64_t lag) {
    uint64_t want = now > lag + 3 * S2_RUMBLE_SLOT_NS ? now - lag - 3 * S2_RUMBLE_SLOT_NS : 0;
    if (c->end + 3 * S2_RUMBLE_SLOT_NS < want || c->end > want + 3 * S2_RUMBLE_SLOT_NS) c->end = want;
}

// rail buttons included; stick -32767..32767
static inline uint64_t s2_joy_buttons(uint32_t b, int right) {
    uint64_t m = 0;
    if (!right) {
        if (b & S2_LEFT) m |= DM_BTN_LEFT; if (b & S2_UP) m |= DM_BTN_UP; if (b & S2_RIGHT) m |= DM_BTN_RIGHT; if (b & S2_DOWN) m |= DM_BTN_DOWN;
        if (b & S2_L) m |= DM_BTN_L; if (b & S2_ZL) m |= DM_BTN_ZL; if (b & S2_MINUS) m |= DM_BTN_MINUS; if (b & S2_LSTICK) m |= DM_BTN_LS;
        if (b & S2_SL_L) m |= DM_BTN_LSL; if (b & S2_SR_L) m |= DM_BTN_LSR;
    } else {
        if (b & S2_A) m |= DM_BTN_A; if (b & S2_B) m |= DM_BTN_B; if (b & S2_X) m |= DM_BTN_X; if (b & S2_Y) m |= DM_BTN_Y;
        if (b & S2_R) m |= DM_BTN_R; if (b & S2_ZR) m |= DM_BTN_ZR; if (b & S2_PLUS) m |= DM_BTN_PLUS; if (b & S2_RSTICK) m |= DM_BTN_RS;
        if (b & S2_SL_R) m |= DM_BTN_RSL; if (b & S2_SR_R) m |= DM_BTN_RSR;
    }
    return m;
}
static inline void s2_joy_stick(const s2_dev *d, int right, int32_t out[2]) {
    float x, y; s2_cal_apply(&d->cal[0], right ? d->r.rx : d->r.lx, right ? d->r.ry : d->r.ly, &x, &y);
    out[0] = (int32_t)lrintf(x * DM_STICK_MAX); out[1] = (int32_t)lrintf(y * DM_STICK_MAX);
}

// Held S2_GESTURE_NS: SL + SR on a paired Joy-Con splits it (sideways); L and R held on two lone Joy-Cons joins them.
#define S2_GESTURE_NS 3000000000ull
enum { S2_GESTURE_NONE, S2_GESTURE_SPLIT, S2_GESTURE_JOIN };
static inline int s2_gesture(uint32_t b, int right, int paired) {
    uint32_t rails = right ? (S2_SL_R | S2_SR_R) : (S2_SL_L | S2_SR_L);
    if (paired) return (b & rails) == rails ? S2_GESTURE_SPLIT : S2_GESTURE_NONE;
    return (b & (right ? S2_R : S2_L)) && !(b & rails) ? S2_GESTURE_JOIN : S2_GESTURE_NONE;
}
typedef struct { int kind, done; uint64_t since; } s2_hold;
static inline int s2_hold_update(s2_hold *h, int kind, uint64_t now) {
    if (kind != h->kind) { h->kind = kind; h->since = now; h->done = 0; }
    return kind != S2_GESTURE_NONE && now - h->since >= S2_GESTURE_NS;
}

// the Pro Controller 2; of two Joy-Cons want_right's side; of one, that one
static inline const s2_dev *s2_pad_motion_dev(const s2_pad_state *p, int want_right) {
    if (p->pro.have) return &p->pro;
    if (p->left.have && p->right.have) return want_right ? &p->right : &p->left;
    return p->left.have ? &p->left : p->right.have ? &p->right : NULL;
}

// map: six signed channel numbers (1..6 = accel xyz, gyro xyz; negative flips) to the Switch frame (x right, y forward, z up; face up 0 0 -1).
// Gyro: 0.0607 dps per count, measured on Joy-Con 2s (docs/reference/services.md#switch-2-controllers-over-bluetooth-le).
#define S2_ACCEL_G (1.f / 4096.f)
#define S2_GYRO_DPS 0.0607f
#define S2_GYRO_RAD (S2_GYRO_DPS * 3.14159265358979323846f / 180.f)
// An upright Joy-Con 2 reads the Switch frame; one alone is turned a quarter turn about up. The Pro Controller 2's frame is the pair's
// (docs/reference/services.md#a-pro-controller-2s-motion-turned-a-quarter-turn).
static const int8_t s2_map_pair[6] = {-1, -2, -3, 4, 5, 6};
static const int8_t s2_map_right_alone[6] = {2, -1, -3, -5, 4, 6};
static const int8_t s2_map_left_alone[6] = {-2, 1, -3, 5, -4, 6};
static const int8_t s2_map_pro[6] = {-1, -2, -3, 4, 5, 6};
static inline const int8_t *s2_pad_map(const s2_pad_state *p, const s2_dev *dev) {
    if (p->pro.have) return s2_map_pro;
    if (p->left.have && p->right.have) return s2_map_pair;
    return dev == &p->left ? s2_map_left_alone : s2_map_right_alone;
}
// A Joy-Con 2 at rest reads about 1.3 dps off: a second of near-constant samples gives the offset; past 4 dps it is a slow turn.
#define S2_REST_N 33
typedef struct { int n, have; int32_t sum[3]; int16_t gmin[3], gmax[3], amin[3], amax[3]; float bias[3]; } s2_rest;
static inline void s2_rest_update(s2_rest *r, const int16_t raw[6]) {
    if (!r->n) for (int i = 0; i < 3; i++) { r->sum[i] = 0; r->gmin[i] = r->gmax[i] = raw[3 + i]; r->amin[i] = r->amax[i] = raw[i]; }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += raw[3 + i];
        if (raw[3 + i] < r->gmin[i]) r->gmin[i] = raw[3 + i]; if (raw[3 + i] > r->gmax[i]) r->gmax[i] = raw[3 + i];
        if (raw[i] < r->amin[i]) r->amin[i] = raw[i]; if (raw[i] > r->amax[i]) r->amax[i] = raw[i];
        if (r->gmax[i] - r->gmin[i] > 100 || r->amax[i] - r->amin[i] > 300) { r->n = 0; return; }   // moving: start the second again
    }
    if (++r->n < S2_REST_N) return;
    r->n = 0;
    float m[3]; for (int i = 0; i < 3; i++) { m[i] = (float)r->sum[i] / S2_REST_N; if (fabsf(m[i]) > 60.f) return; }
    for (int i = 0; i < 3; i++) r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i];
    r->have = 1;
}
// Four LE floats: temperature (°C), then the x y z offset in rad/s; 0.070 dps per count matches a right Joy-Con 2's rest offset.
// 0 for a short block, a non-finite value or past 5 dps.
#define S2_ADDR_GYRO_CAL 0x00013040u
#define S2_GYRO_CAL_DPS 0.070f
static inline int s2_gyro_cal_parse(const uint8_t *d, size_t n, float off[3]) {
    if (!d || n < 16) return 0;
    float v[4]; memcpy(v, d, sizeof v);
    for (int i = 0; i < 4; i++) if (!isfinite(v[i])) return 0;
    for (int i = 0; i < 3; i++) { float dps = v[1 + i] * 57.29577951308232f; if (fabsf(dps) > 5.f) return 0; off[i] = dps / S2_GYRO_CAL_DPS; }
    return 1;
}
// the left Joy-Con 2's reads about the negative; the Pro Controller 2's is not measured
static inline int s2_gyro_cal_usable(uint16_t pid) { return pid == S2_PID_JOYCON_R; }

// Sample times from the controller's ms clock plus an anchor near the smallest latency seen: arrivals come in bursts and a lost report shows
// only in the clock. Never later than the arrival nor S2_CLOCK_LAG_NS before it; *lost: the reports the step skipped.
#define S2_CLOCK_LAG_NS 60000000ull
#define S2_CLOCK_REPORT_MS 30u
#define S2_CLOCK_DECAY 0.001   // clocks up to 1000 ppm apart
#define S2_CLOCK_LOW_RUN 2
typedef struct { int have; uint32_t ms; uint64_t ns, dev; int64_t anchor, low; unsigned low_n; } s2_clock;   // dev: controller time unwrapped (ns); anchor: arrival - dev; low: the run's latest offset
static inline uint64_t s2_clock_sample(s2_clock *c, uint32_t ms, uint64_t arrival, unsigned *lost) {
    uint32_t step = ms - c->ms; uint64_t t;
    if (lost) *lost = 0;
    if (!c->have || step > 1000u) { c->dev = 0; c->anchor = (int64_t)arrival; c->low_n = 0; t = arrival; }   // first report, a second of silence, or the clock went back
    else {
        uint64_t step_ns = (uint64_t)step * 1000000ull; c->dev += step_ns;
        int64_t off = (int64_t)(arrival - c->dev), rise = c->anchor + (int64_t)((double)step_ns * S2_CLOCK_DECAY);
        if (off < c->anchor) {
            int64_t later = c->low_n && c->low > off ? c->low : off;
            c->low = off;
            if (++c->low_n >= S2_CLOCK_LOW_RUN) { c->anchor = later; c->low_n = 0; }
        } else { c->low_n = 0; c->anchor = off < rise ? off : rise; }
        if ((int64_t)arrival - (c->anchor + (int64_t)c->dev) > (int64_t)S2_CLOCK_LAG_NS) c->anchor = off - (int64_t)S2_CLOCK_LAG_NS;
        t = (uint64_t)(c->anchor + (int64_t)c->dev);
        if (t > arrival) t = arrival;
        uint64_t floor = c->ns + step_ns / 2;   // half a step after the last: samples microseconds apart lose integrated rotation
        if (t < floor) t = floor < arrival ? floor : arrival;
        if (t <= c->ns) t = c->ns + 1000;
        if (lost && step > S2_CLOCK_REPORT_MS + S2_CLOCK_REPORT_MS / 2) *lost = (step + S2_CLOCK_REPORT_MS / 2) / S2_CLOCK_REPORT_MS - 1;
    }
    c->have = 1; c->ms = ms; c->ns = t;
    return t;
}
// bias: gyro offset in counts, NULL none
static inline void s2_motion(const int16_t raw[6], const int8_t map[6], const float *bias, float gyro_scale, float acc[3], float rot[3]) {
    for (int i = 0; i < 6; i++) {
        int c = map[i] < 0 ? -map[i] : map[i]; float s = map[i] < 0 ? -1.f : 1.f; if (c < 1 || c > 6) { c = 1; s = 0; }
        float v = s * ((float)raw[c - 1] - (c > 3 && bias ? bias[c - 4] : 0.f));
        if (i < 3) acc[i] = v * S2_ACCEL_G; else rot[i - 3] = v * gyro_scale;
    }
}
// "2,-1,-3,-5,4,6" -> map; 0 and map untouched when not six signed numbers of 1..6
static inline int s2_map_parse(const char *s, int8_t map[6]) {
    int8_t m[6]; if (!s) return 0;
    for (int i = 0; i < 6; i++) {
        char *e; long v = strtol(s, &e, 10); if (e == s || v == 0 || v < -6 || v > 6) return 0;
        m[i] = (int8_t)v; s = e; if (i < 5) { if (*s != ',') return 0; s++; }
    }
    if (*s) return 0;
    memcpy(map, m, 6);
    return 1;
}
