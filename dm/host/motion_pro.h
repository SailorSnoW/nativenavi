// Passive Pro Controller IMU reports (macOS enables report 0x30). The LSM6DS3 at ±2000 dps is 70 mdps/count, not 4000/65536.
// Samples in byte order, 5 ms apart, as Linux hid-nintendo (open: not confirmed on hardware).
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <math.h>

enum { PRO_MOTION_PERIOD_NS = 5000000, PRO_MOTION_STALE_NS = 300000000 };
typedef struct { int live, seen; uint8_t counter; uint64_t received; } pro_motion;
typedef struct { float acc[3], rot[3]; uint64_t ns; } pro_motion_sample;
// Pads stream even at rest: an entry silent this long is a dead IOKit twin.
enum { PRO_SILENT_NS = 1000000000 };
static inline int pro_live(uint64_t heard, uint64_t now) { return heard && now >= heard && now - heard < PRO_SILENT_NS; }
static inline int pro_motion_player(int devices, int controllers, int player) {
    // GameController has no public per-controller HID identity: pair only one pad with one controller of its model.
    return devices == 1 && controllers == 1 ? player : -1;
}
static inline int pro_motion_decode(pro_motion *s, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3]) {
    if (len < 49 || r[0] != 0x30 || now < 2 * PRO_MOTION_PERIOD_NS) return 0;
    if (s->seen && (now < s->received || now - s->received > PRO_MOTION_STALE_NS)) *s = (pro_motion){0};
    if (s->seen && s->counter == r[1]) return 0;
    int16_t v[3][6];
    int data = 0;
    for (int k = 0; k < 3; k++) for (int i = 0; i < 6; i++) {
        const uint8_t *b = r + 13 + 12 * k + 2 * i;
        v[k][i] = (int16_t)((uint16_t)b[0] | (uint16_t)b[1] << 8);
        data |= v[k][i] != 0;
    }
    s->seen = 1;
    s->counter = r[1];
    s->received = now;
    if (!s->live && !data) return 0;   // empty stream: no motion source
    s->live = 1;
    const float g = 1.f / 4096.f, rad = 0.070f * (float)M_PI / 180.f;
    for (int k = 0; k < 3; k++) {
        // Raw: x toward shoulders, y left, z out of face; specific force. Switch: x right, y forward, gravity down.
        out[k] = (pro_motion_sample){{v[k][1] * g, -v[k][0] * g, -v[k][2] * g},
                                    {-v[k][4] * rad, v[k][3] * rad, v[k][5] * rad}, now - (uint64_t)(2 - k) * PRO_MOTION_PERIOD_NS};
    }
    return 3;
}
// DualShock 4 over Bluetooth (GCMotion sends zeroes): report 0x11, six int16 from byte 15, gyro then accel; 8192 counts/G, 2000 deg/s per 32767.
// Raw: x right, y out of face, z toward the player; signs measured on a pad.
static inline int ds4_motion_decode(pro_motion *s, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3]) {
    if (len < 78 || r[0] != 0x11) return 0;
    if (s->seen && (now < s->received || now - s->received > PRO_MOTION_STALE_NS)) *s = (pro_motion){0};
    int16_t v[6];
    int data = 0;
    for (int i = 0; i < 6; i++) {
        v[i] = (int16_t)((uint16_t)r[15 + 2 * i] | (uint16_t)r[16 + 2 * i] << 8);
        data |= v[i] != 0;
    }
    s->seen = 1;
    s->received = now;
    if (!s->live && !data) return 0;   // empty stream: no motion source
    s->live = 1;
    const float g = 1.f / 8192.f, rad = 2000.f / 32767.f * (float)M_PI / 180.f;
    out[0] = (pro_motion_sample){{-v[3] * g, v[5] * g, -v[4] * g}, {v[0] * rad, -v[2] * rad, v[1] * rad}, now};
    return 1;
}
// 8BitDo Ultimate 2 Wireless (2DC8:6012) over Bluetooth (no GCMotion): firmware 1.03+'s 34-byte report 0x01, six int16 from byte 15, accel then gyro;
// 4096 counts/G, 2000 deg/s per 32767 (SDL's SDL_hidapi_8bitdo.c). Raw axes as the Pro Controller's. Firmware 1.02's 12-byte reports carry no sensor.
static inline int abitdo_motion_decode(pro_motion *s, const uint8_t *r, size_t len, uint64_t now, pro_motion_sample out[3]) {
    if (len < 34 || r[0] != 0x01) return 0;
    if (s->seen && (now < s->received || now - s->received > PRO_MOTION_STALE_NS)) *s = (pro_motion){0};
    int16_t v[6];
    int data = 0;
    for (int i = 0; i < 6; i++) {
        v[i] = (int16_t)((uint16_t)r[15 + 2 * i] | (uint16_t)r[16 + 2 * i] << 8);
        data |= v[i] != 0;
    }
    s->seen = 1;
    s->received = now;
    if (!s->live && !data) return 0;   // empty stream: no motion source
    s->live = 1;
    const float g = 1.f / 4096.f, rad = 2000.f / 32767.f * (float)M_PI / 180.f;
    out[0] = (pro_motion_sample){{v[1] * g, -v[0] * g, -v[2] * g}, {-v[4] * rad, v[3] * rad, v[5] * rad}, now};
    return 1;
}
