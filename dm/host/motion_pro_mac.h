// Workaround: macOS's GCMotion of these pads, and the sticks of some Switch Pro-compatible pads, are zero-only, but the HID reports its driver
// receives carry the IMU and the sticks: read them. Main run loop only; one buffer / decoder per retained IOHIDDevice.
#pragma once
#import <IOKit/hid/IOHIDManager.h>
#import <GameController/GCSyntheticDeviceKeys.h>
#include "motion_pro.h"
#include "pad_pro.h"
#include "dm_input.h"
#include <stdlib.h>
#include <string.h>

// One row per model; each model pairs on its own, so one pad of each pairs with another model connected
typedef int (*pro_decoder)(pro_motion *, const uint8_t *, size_t, uint64_t, pro_motion_sample[3]);
typedef struct { const char *name, *category; int vendor, product[2]; uint8_t report; size_t min_len; pro_decoder decode; } pro_model;
enum { PRO_MODEL_PRO, PRO_MODEL_DS4, PRO_MODELS };
enum { PRO_STICK_TRIG = DM_STICK_MAX / 2 };   // half a stick in report 0x30 while GameController reads zero...
#define PRO_STICK_HOLD_NS 150000000ull        // ...for this long latches the stick onto the HID reports
static const pro_model g_pro_models[PRO_MODELS] = {
    {"Pro Controller", "Switch Pro Controller", 0x057E, {0x2009}, 0x30, 49, pro_motion_decode},
    // a DualShock 4 over USB sends 0x01, never 0x11: GameController's motion works there
    {"DualShock 4", "DualShock 4", 0x054C, {0x05C4, 0x09CC}, 0x11, 78, ds4_motion_decode},
};
static int pro_model_of(int vendor, int product) {
    for (int m = 0; m < PRO_MODELS; m++)
        for (int i = 0; i < 2; i++)
            if (vendor == g_pro_models[m].vendor && product && product == g_pro_models[m].product[i]) return m;
    return -1;
}
static int pro_model_of_category(const char *category) {
    for (int m = 0; m < PRO_MODELS; m++) if (category && !strcmp(category, g_pro_models[m].category)) return m;
    return -1;
}

typedef struct pro_device {
    IOHIDDeviceRef hid; uint8_t report[512]; pro_motion motion; uint64_t heard; int model;   // heard: its last complete report
    // A Pro Controller's sticks per side: stick_live, GameController moved it (it owns the stick); stick_hid, latched onto report 0x30;
    // stick_since, when the deflection began while GameController read zero; stick_sent / _mask, what was last pushed to front_mac.m
    pad_pro sticks; int32_t stick[4]; int stick_live[2], stick_hid[2]; uint64_t stick_since[2];
    int32_t stick_sent[4]; int stick_sent_mask;
    struct pro_device *next;
} pro_device;
typedef struct { pro_device *bound; int player, controllers, slot, seen_devices, seen_controllers; } pro_pairing;
static IOHIDManagerRef g_pro_manager;
static pro_device *g_pro_devices;
static pro_pairing g_pro_pair[PRO_MODELS] = {[0 ... PRO_MODELS - 1] = {NULL, -1, 0, -1, -1, -1}};
static int g_pro_active = 1, g_pro_gyro = 1, g_pro_sticks = 1, g_pro_stick_timer;
static void (*g_pro_changed)(void);
static int (*g_pro_gc_sticks)(int player, int nonzero[2]);   // front_mac.m: 1 if the player's Pro Controller is GameController's; which of its sticks are non-zero
static void (*g_pro_stick_changed)(int player);              // front_mac.m: read that controller again (a latched stick moved or let go)

static int pro_sticks_fresh(const pro_device *d, uint64_t now) { return d && d->sticks.seen && now >= d->heard && now - d->heard < PRO_MOTION_STALE_NS; }
static void pro_stick_reset(pro_device *d) {
    memset(d->stick_live, 0, sizeof d->stick_live); memset(d->stick_hid, 0, sizeof d->stick_hid); memset(d->stick_since, 0, sizeof d->stick_since);
    memset(d->stick_sent, 0, sizeof d->stick_sent); d->stick_sent_mask = 0;
}
static int pro_stick_nonzero_sent(const pro_device *d) {
    return d && d->stick_sent_mask && (d->stick_sent[0] || d->stick_sent[1] || d->stick_sent[2] || d->stick_sent[3]);
}
static void pro_stick_arm_expire(uint64_t delay_ns);
// A latched tilted stick lets go when report 0x30 stops coming without a removal
static void pro_stick_expire(void) {
    pro_pairing *g = &g_pro_pair[PRO_MODEL_PRO];
    pro_device *d = g->bound;
    if (!g_pro_sticks || !g_pro_active || g->player < 0 || !pro_stick_nonzero_sent(d)) return;
    uint64_t now = now_ns();
    if (pro_sticks_fresh(d, now)) { pro_stick_arm_expire(PRO_MOTION_STALE_NS - (now - d->heard)); return; }
    memset(d->stick_sent, 0, sizeof d->stick_sent); d->stick_sent_mask = 0;
    if (g_pro_stick_changed) g_pro_stick_changed(g->player);
}
static void pro_stick_arm_expire(uint64_t delay_ns) {
    if (!g_pro_manager || g_pro_stick_timer) return;   // the unit tests have no manager and call pro_stick_expire themselves
    g_pro_stick_timer = 1;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)delay_ns), dispatch_get_main_queue(), ^{
        g_pro_stick_timer = 0;
        pro_stick_expire();
    });
}
// front_mac.m's pad_read, before it publishes a Switch Pro Controller: a non-zero GameController stick owns that stick again; a latched one
// replaces GameController's zero. Returns the sticks replaced (bit 0 left, bit 1 right).
static int dm_pro_sticks(int player, const int gc_nonzero[2], int32_t st[4]) {
    pro_pairing *g = &g_pro_pair[PRO_MODEL_PRO];
    pro_device *d = g->bound;
    if (!g_pro_sticks || !g_pro_active || player != g->player || !d) return 0;
    if (gc_nonzero) for (int s = 0; s < 2; s++) if (gc_nonzero[s]) {
        d->stick_live[s] = 1; d->stick_since[s] = 0;
        if (d->stick_hid[s]) {
            d->stick_hid[s] = 0; d->stick_sent_mask &= ~(1 << s); d->stick_sent[2 * s] = d->stick_sent[2 * s + 1] = 0;
            LOG("[hid] Pro Controller %s stick: GameController active again; returning to GameController", s ? "right" : "left");
        }
    }
    if (!pro_sticks_fresh(d, now_ns())) return 0;
    int mask = (d->stick_hid[0] ? 1 : 0) | (d->stick_hid[1] ? 2 : 0);
    if (!mask) return 0;
    for (int a = 0; a < 4; a++) if (mask & (1 << (a / 2))) st[a] = d->stick[a];
    return mask;
}
// At each report 0x30: latch a stick after PRO_STICK_HOLD_NS of deflection past PRO_STICK_TRIG while GameController (mapped and unmapped)
// reads exact zero; tell front_mac.m only when a latched stick's value or the mask changes
static void pro_stick_update(pro_device *d, int player, uint64_t now) {
    if (!g_pro_gc_sticks) return;
    int nz[2] = {0, 0};
    if (!g_pro_gc_sticks(player, nz)) return;
    for (int s = 0; s < 2; s++) {
        if (nz[s]) {
            d->stick_live[s] = 1; d->stick_since[s] = 0;
            if (d->stick_hid[s]) {
                d->stick_hid[s] = 0;
                LOG("[hid] Pro Controller %s stick: GameController active again; returning to GameController", s ? "right" : "left");
            }
        } else if (!d->stick_live[s] && !d->stick_hid[s]) {
            int moved = abs(d->stick[2 * s]) >= PRO_STICK_TRIG || abs(d->stick[2 * s + 1]) >= PRO_STICK_TRIG;
            if (!moved) d->stick_since[s] = 0;
            else if (!d->stick_since[s]) d->stick_since[s] = now;
            else if (now >= d->stick_since[s] && now - d->stick_since[s] >= PRO_STICK_HOLD_NS) {
                d->stick_hid[s] = 1;
                LOG("[hid] Pro Controller %s stick: GameController reads zero while HID moves; reading from report 0x30 (DM_PRO_STICKS=0 keeps GameController)", s ? "right" : "left");
            }
        }
    }
    int mask = (d->stick_hid[0] ? 1 : 0) | (d->stick_hid[1] ? 2 : 0);
    int32_t cur[4] = {0};
    for (int a = 0; a < 4; a++) if (mask & (1 << (a / 2))) cur[a] = d->stick[a];
    if (mask == d->stick_sent_mask && !memcmp(cur, d->stick_sent, sizeof cur)) return;
    d->stick_sent_mask = mask; memcpy(d->stick_sent, cur, sizeof cur);
    if (pro_stick_nonzero_sent(d)) pro_stick_arm_expire(PRO_MOTION_STALE_NS);
    if (g_pro_stick_changed) g_pro_stick_changed(player);
}

// Only devices that send motion reports count: an entry with no pad behind it would leave one unpaired. Under DM_PAD_MERGE the first live model keeps a shared player.
static void pro_route(void) {
    uint64_t now = now_ns();
    unsigned taken = 0;
    for (int m = 0; m < PRO_MODELS; m++) {
        pro_pairing *g = &g_pro_pair[m];
        int n = 0;
        pro_device *live = NULL;
        for (pro_device *d = g_pro_devices; d; d = d->next) if (d->model == m && pro_live(d->heard, now)) { n++; live = d; }
        int p = pro_motion_player(n, g->controllers, g->slot);
        if (g_pro_gyro && p >= 0 && p < 32 && taken >> p & 1) p = -1;
        pro_device *d = p >= 0 ? live : NULL;
        if (p < 0 && n && g->controllers && (n != g->seen_devices || g->controllers != g->seen_controllers)) {
            if (g_pro_gyro) LOG("[hid] %s raw motion: %d HID devices sending reports / %d controllers; cannot pair them uniquely, keeping GameController motion", g_pro_models[m].name, n, g->controllers);
            else if (m == PRO_MODEL_PRO && g_pro_sticks) LOG("[hid] Pro Controller HID sticks: %d HID devices sending reports / %d controllers; cannot pair them uniquely, keeping GameController sticks", n, g->controllers);
        }
        g->seen_devices = n;
        g->seen_controllers = g->controllers;
        if (g_pro_gyro && p >= 0 && p < 32) taken |= 1u << p;
        if (d == g->bound && p == g->player) continue;
        int old_p = g->player, notify = m == PRO_MODEL_PRO && g->bound && g->bound->stick_sent_mask;
        if (g->bound && m == PRO_MODEL_PRO) pro_stick_reset(g->bound);
        if (g_pro_gyro && g->player >= 0) dm_motion_clear_hid(g->player);
        g->bound = d;
        g->player = p;
        if (d) {
            d->motion = (pro_motion){0};
            if (m == PRO_MODEL_PRO) pro_stick_reset(d);
            if (g_pro_gyro) LOG("[hid] %s raw motion: player %d (passive HID 0x%02x)", g_pro_models[m].name, p + 1, g_pro_models[m].report);
        }
        if (notify && old_p >= 0 && g_pro_stick_changed) g_pro_stick_changed(old_p);
    }
}
static void dm_motion_pro_players(const int controllers[PRO_MODELS], const int slots[PRO_MODELS]) {
    for (int m = 0; m < PRO_MODELS; m++) {
        g_pro_pair[m].controllers = controllers[m];
        g_pro_pair[m].slot = slots[m];
    }
    pro_route();
}
static void dm_motion_pro_active(int active) {
    g_pro_active = active;
    for (int m = 0; m < PRO_MODELS; m++) {
        pro_pairing *g = &g_pro_pair[m];
        if (g->bound) {
            g->bound->motion = (pro_motion){0};
            if (m == PRO_MODEL_PRO) {
                memset(g->bound->stick_since, 0, sizeof g->bound->stick_since);
                memset(g->bound->stick_sent, 0, sizeof g->bound->stick_sent);
                g->bound->stick_sent_mask = 0;
            }
        }
        if (!active && g_pro_gyro && g->player >= 0) dm_motion_clear_hid(g->player);
    }
}
static void pro_report(void *ctx, IOReturn result, void *sender, IOHIDReportType type, uint32_t report_id, uint8_t *r, CFIndex len) {
    (void)sender;
    pro_device *d = ctx;
    const pro_model *m = &g_pro_models[d->model];
    pro_pairing *g = &g_pro_pair[d->model];
    if (result != kIOReturnSuccess || type != kIOHIDReportTypeInput || report_id != m->report || len < (CFIndex)m->min_len || r[0] != m->report) return;
    uint64_t now = now_ns();
    int was = pro_live(d->heard, now);
    if (d->model == PRO_MODEL_PRO && g_pro_sticks) {
        if (!was) d->sticks = (pad_pro){0};
        uint64_t buttons;
        pad_pro_decode(&d->sticks, r, (size_t)len, &buttons, d->stick);
    }
    d->heard = now;
    // a continuous stream must also release a device that fell silent
    if (!was && g_pro_changed) g_pro_changed(); else pro_route();
    if (!g_pro_active || d != g->bound) return;
    if (d->model == PRO_MODEL_PRO && g_pro_sticks) pro_stick_update(d, g->player, now);
    if (!g_pro_gyro) return;
    pro_motion_sample samples[3];
    int n = m->decode(&d->motion, r, (size_t)len, now, samples);
    for (int i = 0; i < n; i++) dm_motion_set_hid(g->player, samples[i].acc, samples[i].rot, samples[i].ns);
}
static int pro_property(IOHIDDeviceRef hid, CFStringRef key) {
    CFTypeRef v = IOHIDDeviceGetProperty(hid, key);
    int n = 0;
    if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
    return n;
}
static void pro_found(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)sender;
    if (result != kIOReturnSuccess || IOHIDDeviceGetProperty(hid, CFSTR(kIOHIDGCSyntheticDeviceKey)) == kCFBooleanTrue) return;
    for (pro_device *d = g_pro_devices; d; d = d->next) if (d->hid == hid) return;
    int model = pro_model_of(pro_property(hid, CFSTR(kIOHIDVendorIDKey)), pro_property(hid, CFSTR(kIOHIDProductIDKey)));
    if (model < 0 || (model != PRO_MODEL_PRO && !g_pro_gyro)) return;
    pro_device *d = calloc(1, sizeof *d);
    if (!d) {
        LOG("[RED] %s raw HID: cannot allocate device state", g_pro_models[model].name);
        return;
    }
    d->model = model;
    d->hid = (IOHIDDeviceRef)CFRetain(hid);
    d->next = g_pro_devices;
    g_pro_devices = d;
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, pro_report, d);
    if (g_pro_changed) g_pro_changed();
}
static void pro_removed(void *ctx, IOReturn result, void *sender, IOHIDDeviceRef hid) {
    (void)ctx; (void)result; (void)sender;
    pro_device **link = &g_pro_devices;
    while (*link && (*link)->hid != hid) link = &(*link)->next;
    if (!*link) return;
    pro_device *d = *link;
    *link = d->next;
    // the main run loop serializes removal with report callbacks
    IOHIDDeviceRegisterInputReportCallback(hid, d->report, sizeof d->report, NULL, NULL);
    if (g_pro_changed) g_pro_changed(); else pro_route();
    CFRelease(d->hid);
    free(d);
}
// DM_MOTION=0 or DM_MOTION_HID=0: no raw motion; DM_PRO_STICKS=0: no stick fallback; both off: no HID reading at all
static void dm_motion_pro_start(void (*changed)(void), int (*gc_sticks)(int, int[2]), void (*stick_changed)(int)) {
    const char *e = getenv("DM_MOTION"), *h = getenv("DM_MOTION_HID"), *st = getenv("DM_PRO_STICKS");
    g_pro_gyro = !(e && !atoi(e)) && !(h && !atoi(h));
    g_pro_sticks = !(st && !atoi(st));
    if (!g_pro_gyro && !g_pro_sticks) return;
    g_pro_changed = changed; g_pro_gc_sticks = gc_sticks; g_pro_stick_changed = stick_changed;
    g_pro_manager = IOHIDManagerCreate(kCFAllocatorDefault, kIOHIDOptionsTypeNone);
    if (!g_pro_manager) return;
    NSMutableArray *match = [NSMutableArray array];
    for (int m = 0; m < PRO_MODELS; m++) {
        if (m != PRO_MODEL_PRO && !g_pro_gyro) continue;
        for (int i = 0; i < 2; i++) if (g_pro_models[m].product[i])
            [match addObject:@{@kIOHIDVendorIDKey: @(g_pro_models[m].vendor), @kIOHIDProductIDKey: @(g_pro_models[m].product[i])}];
    }
    IOHIDManagerSetDeviceMatchingMultiple(g_pro_manager, (__bridge CFArrayRef)match);
    IOHIDManagerRegisterDeviceMatchingCallback(g_pro_manager, pro_found, NULL);
    IOHIDManagerRegisterDeviceRemovalCallback(g_pro_manager, pro_removed, NULL);
    IOHIDManagerScheduleWithRunLoop(g_pro_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
    IOReturn r = IOHIDManagerOpen(g_pro_manager, kIOHIDOptionsTypeNone);   // nonexclusive
    if (r != kIOReturnSuccess) {
        LOG("[RED] Controller passive HID: cannot open HID (0x%x); keeping GameController input", r);
        IOHIDManagerUnscheduleFromRunLoop(g_pro_manager, CFRunLoopGetMain(), kCFRunLoopCommonModes);
        CFRelease(g_pro_manager);
        g_pro_manager = NULL;
    }
}
