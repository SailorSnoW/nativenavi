// Wired Xbox 360 (XUSB) pads over USB that macOS's Xbox driver (Microsoft's product IDs only) leaves alone. Pure: tests/t_pad_xusb.c checks it; pad_gip_mac.h
// reads the devices. Input report 0x00, length 0x14: byte 2 the d-pad (bits 0-3), Start, Back and the stick clicks, byte 3 LB, RB, Guide (bit 2, not read yet)
// and A B X Y (4-7), bytes 4 / 5 the triggers (0-255), 6-13 the sticks (int16, y up); the state messages at connection are skipped.
// Buttons by position, as front_mac.m maps GameController and pad_gip.h Xbox One: Xbox A is Switch B, B is A, X is Y, Y is X; triggers press past 25%.
#pragma once
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include "dm_input.h"
#include "pad_hid.h"

#define XUSB_DEV_CLASS 0xFF
#define XUSB_DEV_SUBCLASS 0xFF
#define XUSB_DEV_PROTOCOL 0xFF
#define XUSB_IF_CLASS 0xFF
#define XUSB_IF_SUBCLASS 0x5D
#define XUSB_IF_PROTOCOL 0x01
#define XUSB_REPORT_LEN 20
#define XUSB_TRIGGER_PRESS 64

typedef struct {
    uint64_t buttons;   // Switch buttons (dm_input.h)
    int32_t s[4];       // lx, ly, rx, ry
    int heard;
} pad_xusb;

// A pad is an ff/ff/ff device with an ff/5d/01 interface; ff/5d/81 is the wireless receiver's, ff/5d/02 an audio interface's
static inline int pad_xusb_device_ok(int dev_class, int dev_subclass, int dev_protocol) {
    return dev_class == XUSB_DEV_CLASS && dev_subclass == XUSB_DEV_SUBCLASS && dev_protocol == XUSB_DEV_PROTOCOL;
}
static inline int pad_xusb_interface_ok(int cls, int sub, int pro) {
    return cls == XUSB_IF_CLASS && sub == XUSB_IF_SUBCLASS && pro == XUSB_IF_PROTOCOL;
}
static inline void pad_xusb_reset(pad_xusb *x) { memset(x, 0, sizeof *x); }
static inline float pad_xusb_axis(const uint8_t *p) { int16_t v = (int16_t)(p[0] | p[1] << 8); return v < 0 ? v / 32768.0f : v / 32767.0f; }

// 1: an input report that changed the state
static int pad_xusb_decode(pad_xusb *x, const uint8_t *r, size_t n) {
    if (n < XUSB_REPORT_LEN || r[0] != 0x00 || r[1] != XUSB_REPORT_LEN) return 0;
    static const struct { uint8_t byte, bit; uint64_t to; } map[] = {
        {2, 0, DM_BTN_UP}, {2, 1, DM_BTN_DOWN}, {2, 2, DM_BTN_LEFT}, {2, 3, DM_BTN_RIGHT},
        {2, 4, DM_BTN_PLUS}, {2, 5, DM_BTN_MINUS}, {2, 6, DM_BTN_LS}, {2, 7, DM_BTN_RS},
        {3, 0, DM_BTN_L}, {3, 1, DM_BTN_R},
        {3, 4, DM_BTN_B}, {3, 5, DM_BTN_A}, {3, 6, DM_BTN_Y}, {3, 7, DM_BTN_X},
    };
    uint64_t m = 0;
    for (size_t i = 0; i < sizeof map / sizeof map[0]; i++) if (r[map[i].byte] >> map[i].bit & 1) m |= map[i].to;
    if (r[4] >= XUSB_TRIGGER_PRESS) m |= DM_BTN_ZL;
    if (r[5] >= XUSB_TRIGGER_PRESS) m |= DM_BTN_ZR;
    int32_t s[4];
    pad_hid_stick(pad_xusb_axis(r + 6), pad_xusb_axis(r + 8), &s[0], &s[1]);
    pad_hid_stick(pad_xusb_axis(r + 10), pad_xusb_axis(r + 12), &s[2], &s[3]);
    int changed = !x->heard || m != x->buttons || memcmp(s, x->s, sizeof s);
    x->buttons = m; memcpy(x->s, s, sizeof s); x->heard = 1;
    return changed;
}
