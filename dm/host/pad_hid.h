// Raw HID gamepads GameController does not list: buttons, hat and axes to Switch buttons and sticks. Pure (tests); pad_hid_mac.h reads the
// devices. Default map: 8BitDo's D-input numbering; DM_HID_MAP=A=2,B=1,... replaces entries, DM_HID_LOG=1 logs each usage pressed.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "dm_input.h"

#define PAD_HID_DEFAULT_VENDOR 0x2DC8   // 8BitDo, whose numbering the default map is
enum { PH_X, PH_Y, PH_Z, PH_RX, PH_RY, PH_RZ, PH_BRAKE, PH_ACCEL, PH_AXES };
enum pad_hid_layout { PAD_HID_DEFAULT, PAD_HID_NINTENDO, PAD_HID_X360, PAD_HID_SWITCH_WIRED };
typedef struct {
    uint32_t buttons;          // bit u - 1: usage u held
    int hat;                   // -1 centered, 0 up, clockwise to 7 up-left
    float axis[PH_AXES];       // -1..1 (a trigger rests at -1)
    uint8_t has[PH_AXES];
    uint8_t map[16];           // Switch button bit -> HID button usage, 0 none
    enum pad_hid_layout layout;
    uint8_t rest_seen[PH_AXES];
    float rest[PH_AXES], reach[PH_AXES][2];   // reach below and above the rest
} pad_hid;

// Nintendo's protocol (dekuNukem/Nintendo_Switch_Reverse_Engineering): a pad declares it with all four of 0x3F, 0x21, 0x30 and 0x01; rumble 0x10 is apart
enum { PAD_NIN_SIMPLE = 1, PAD_NIN_REPLY = 2, PAD_NIN_FULL = 4, PAD_NIN_SUBCMD = 8, PAD_NIN_RUMBLE = 16, PAD_NIN_PROTOCOL = 15 };
#define PAD_HID_NIN_REPORT 0x3F   // 16 buttons, a hat, four 16-bit axes
static inline int pad_hid_nin_bit(int output, uint32_t id, uint32_t page) {
    if (output) return id == 0x01 ? PAD_NIN_SUBCMD : id == 0x10 ? PAD_NIN_RUMBLE : 0;
    return id == PAD_HID_NIN_REPORT && page == 9 ? PAD_NIN_SIMPLE : id == 0x21 ? PAD_NIN_REPLY : id == 0x30 ? PAD_NIN_FULL : 0;
}
static inline int pad_hid_nin_protocol(int bits) { return (bits & PAD_NIN_PROTOCOL) == PAD_NIN_PROTOCOL; }
// player lights need only the subcommand channel; some clones implement the lights alone
static inline int pad_hid_nin_lights(int bits) {
    int need = PAD_NIN_SIMPLE | PAD_NIN_REPLY | PAD_NIN_SUBCMD;
    return (bits & need) == need;
}

static const char *const pad_hid_names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "PLUS", "MINUS", "LEFT", "UP", "RIGHT", "DOWN"};
static const uint8_t pad_hid_nin_map[16] = {2, 1, 4, 3, 11, 12, 5, 6, 7, 8, 10, 9};
// Steam Link's virtual Xbox 360 pad's usages as macOS delivers them (measured, not the descriptor's order); the triggers are axes
static const uint8_t pad_hid_x360_map[16] = {2, 1, 4, 3, 7, 8, 5, 6, 0, 0, 9, 10, 14, 12, 15, 13};
// Switch wired HID usages: 1 Y, 2 B, 3 A, 4 X, 5 / 6 L / R, 7 / 8 ZL / ZR, 9 minus, 10 plus, 11 / 12 stick clicks, 13 Home, 14 Capture (report 0x3F's start 1 B, 2 A)
static const uint8_t pad_hid_switch_wired_map[16] = {3, 2, 4, 1, 11, 12, 5, 6, 7, 8, 10, 9};

// a virtual pad can reuse the Xbox ID with Brake / Accelerator usages
static enum pad_hid_layout pad_hid_layout_for(int nin, int vid, int pid, const uint8_t has[PH_AXES]) {
    if (nin & PAD_NIN_SIMPLE) return PAD_HID_NINTENDO;
    if (vid == 0x045e && pid == 0x028e && has[PH_X] && has[PH_Y] && has[PH_RX] && has[PH_RY] &&
        has[PH_Z] && has[PH_RZ] && !has[PH_BRAKE] && !has[PH_ACCEL]) return PAD_HID_X360;
    if (vid == 0x0e6f && pid == 0x0185) return PAD_HID_SWITCH_WIRED;   // PDP Wired Fight Pad Pro for Switch, by its ID: PDP's other pads number theirs otherwise
    return PAD_HID_DEFAULT;
}

// 1 read every pad of the model as a player, 0 not, -1 not yet: a pad just connected may not be listed yet, so start only after the wait
static inline int pad_dupes_want(int n, int listed, int read, int due) { return n < 2 || n <= listed ? 0 : read || due ? 1 : -1; }

// Report 0x3F usages: 1 B, 2 A, 3 Y, 4 X, 5 / 6 L / R, 7 / 8 ZL / ZR, 9 minus, 10 plus, 11 / 12 stick clicks, 13 Home, 14 Capture
static void pad_hid_init(pad_hid *h, const char *map, enum pad_hid_layout layout) {
    static const uint8_t def[16] = {2, 1, 5, 4, 14, 15, 7, 8, 9, 10, 12, 11};
    const uint8_t *button_map = def;
    switch (layout) {
    case PAD_HID_NINTENDO: button_map = pad_hid_nin_map; break;
    case PAD_HID_X360: button_map = pad_hid_x360_map; break;
    case PAD_HID_SWITCH_WIRED: button_map = pad_hid_switch_wired_map; break;
    case PAD_HID_DEFAULT: break;
    }
    memset(h, 0, sizeof *h);
    h->hat = -1;
    h->layout = layout;
    memcpy(h->map, button_map, sizeof h->map);
    for (int a = 0; a < PH_AXES; a++) h->axis[a] = a >= PH_BRAKE ? -1 : 0;
    for (const char *s = map; s && *s; s = strchr(s, ',') ? strchr(s, ',') + 1 : "") {
        const char *eq = strchr(s, '='), *end = strchr(s, ',');
        if (!eq || (end && eq > end)) continue;
        int u = atoi(eq + 1);
        for (int b = 0; b < 16; b++)
            if ((size_t)(eq - s) == strlen(pad_hid_names[b]) && !strncasecmp(s, pad_hid_names[b], eq - s) && u >= 0 && u <= 32) h->map[b] = (uint8_t)u;
    }
}
static int pad_hid_axis_of(uint32_t page, uint32_t usage) {   // -1: not a gamepad axis
    if (page == 1 && usage >= 0x30 && usage <= 0x35) return PH_X + (int)(usage - 0x30);
    if (page == 2 && usage == 0xC5) return PH_BRAKE;
    if (page == 2 && usage == 0xC4) return PH_ACCEL;
    return -1;
}
// the Xbox layout's right stick is Rx / Ry: Z / Rz are triggers
static int pad_hid_axis_for(enum pad_hid_layout layout, int axis) {
    if (layout == PAD_HID_X360 && axis == PH_Z) return PH_BRAKE;
    if (layout == PAD_HID_X360 && axis == PH_RZ) return PH_ACCEL;
    return axis;
}
static float pad_hid_norm(long v, long min, long max) { return max > min ? (float)(2.0 * (v - min) / (max - min) - 1) : 0; }
// Report 0x3F's sticks are raw (off centre, short of range): measured from the first value as rest; the reach only grows. Report 0x30 is calibrated.
#define PAD_HID_RAW_REACH 0.7f
static float pad_hid_raw_axis(pad_hid *h, int a, float v) {
    if (!h->rest_seen[a]) {
        h->rest_seen[a] = 1;
        h->rest[a] = fabsf(v) < 0.25f ? v : 0;
        h->reach[a][0] = h->reach[a][1] = PAD_HID_RAW_REACH;
    }
    float d = v - h->rest[a];
    int side = d > 0;
    if (fabsf(d) > h->reach[a][side]) h->reach[a][side] = fabsf(d);
    return d / h->reach[a][side];
}
static int pad_hid_hat(long v, long min, long max) { return v >= min && v <= max && v - min < 8 ? (int)(v - min) : -1; }   // past the 8 directions: centered
// a round dead zone, the rest rescaled from 0; each axis clamped after, so square gates reach their corners
#define PAD_HID_DEAD 0.12f
static void pad_hid_stick(float x, float y, int32_t *ox, int32_t *oy) {
    float r = sqrtf(x * x + y * y);
    if (r <= PAD_HID_DEAD) { *ox = *oy = 0; return; }
    float k = (r - PAD_HID_DEAD) / (1 - PAD_HID_DEAD) / r;
    x *= k;
    y *= k;
    *ox = (int32_t)((x < -1 ? -1 : x > 1 ? 1 : x) * DM_STICK_MAX);
    *oy = (int32_t)((y < -1 ? -1 : y > 1 ? 1 : y) * DM_STICK_MAX);
}

static uint64_t pad_hid_map_buttons(const pad_hid *h, uint32_t buttons) {
    uint64_t m = 0;
    for (int b = 0; b < 16; b++) if (h->map[b] && buttons >> (h->map[b] - 1) & 1) m |= 1ull << b;
    return m;
}
// system is report byte 4 (Home, Capture); stale simple-report axes and hat never enter the full report's state
static uint64_t pad_hid_nin_buttons(const pad_hid *h, uint64_t buttons, uint8_t system) {
    uint32_t usages = 0;
    for (int b = 0; b < 12; b++) if (buttons >> b & 1) usages |= 1u << (pad_hid_nin_map[b] - 1);
    if (system & 0x10) usages |= 1u << 12;
    if (system & 0x20) usages |= 1u << 13;
    return pad_hid_map_buttons(h, usages) | (buttons & (DM_BTN_LEFT | DM_BTN_UP | DM_BTN_RIGHT | DM_BTN_DOWN));
}
static uint64_t pad_hid_buttons(const pad_hid *h) {
    static const uint64_t hat[8] = {DM_BTN_UP, DM_BTN_UP | DM_BTN_RIGHT, DM_BTN_RIGHT, DM_BTN_RIGHT | DM_BTN_DOWN, DM_BTN_DOWN, DM_BTN_DOWN | DM_BTN_LEFT, DM_BTN_LEFT, DM_BTN_LEFT | DM_BTN_UP};
    uint64_t m = pad_hid_map_buttons(h, h->buttons);
    if (h->hat >= 0 && h->hat < 8) m |= hat[h->hat];
    if (h->has[PH_BRAKE] && h->axis[PH_BRAKE] > 0) m |= DM_BTN_ZL;
    if (h->has[PH_ACCEL] && h->axis[PH_ACCEL] > 0) m |= DM_BTN_ZR;
    return m;
}
static void pad_hid_sticks(const pad_hid *h, int32_t s[4]) {   // HID's Y grows downward, the Switch's upward
    int z = h->has[PH_Z];
    pad_hid_stick(h->axis[PH_X], -h->axis[PH_Y], &s[0], &s[1]);
    pad_hid_stick(h->axis[z ? PH_Z : PH_RX], -h->axis[z ? PH_RZ : PH_RY], &s[2], &s[3]);
}
