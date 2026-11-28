// Mac front end. DM_WINDOW=1: a window (CAMetalLayer) with keyboard and controller input, the guest on a large-stack thread,
// the main thread AppKit's; otherwise the guest runs on the current thread.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "dm.h"
#include "front_role.h"
#include "front_mac_present.h"
#include "dm_input.h"
#include "front.h"
#include "hle.h"
#include "pad_slots.h"
#include "haptics_apple.h"
#include "motion_apple.h"
#include "motion_pro_mac.h"
#include "pad_jc1_mac.h"
#include "pad_apple.h"
#include "pad_hid_mac.h"
#include "pad_gip_mac.h"
#include "pad_switch2_apple.h"
static int s2pad_player(s2_pad *d);
static void s2pad_read(s2_pad *d);
static void (*g_merge_sink)(const dm_haptic_state *);   // DM_PAD_MERGE: the pads player 1's rumble goes to; NULL = GameController's
static void merge_rumble_to(void (*sink)(const dm_haptic_state *));
#include "home_quit.h"
#include "home_quit_mac.h"
#include "ctrl_prompt.h"
#include <signal.h>
#include <math.h>
#include <sys/stat.h>

static CAMetalLayer *g_layer;
static uint64_t g_btn;
static int g_w, g_a, g_s, g_d;
static int g_rl, g_ru, g_rr, g_rd;   // Shift+arrow: the right stick; each key goes up as it went down, whatever Shift does
static int g_input_on = 1;   // main thread

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
static _Atomic int g_screen_fps;
int front_screen_max_fps(void) { return g_screen_fps; }
static _Atomic int g_screen_variable_refresh;
int front_screen_variable_refresh(void) { return g_screen_variable_refresh; }
// Main thread: the screen this host switched and its own mode (NULL = none switched)
static CGDirectDisplayID g_match_id;
static CGDisplayModeRef g_match_orig;
static int g_match_orig_vrr;
// valid while g_match_orig != NULL
static int32_t g_match_pick;
static int g_match_pick_vrr;
static void screen_refresh(NSScreen *screen) {
    if (!screen) return;
    g_screen_fps = (int)screen.maximumFramesPerSecond;
    g_screen_variable_refresh = screen.minimumRefreshInterval > 0 && screen.maximumRefreshInterval > screen.minimumRefreshInterval;
    // NSScreen can keep the old VRR range or rate for a while after a switch: keep the picked mode's
    if (g_match_orig && [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == g_match_id) {
        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(g_match_id);
        if (cur && CGDisplayModeGetIODisplayModeID(cur) == g_match_pick) {
            g_screen_variable_refresh = g_match_pick_vrr;
            double hz = CGDisplayModeGetRefreshRate(cur);
            if (hz > 0) g_screen_fps = (int)round(hz);
        }
        if (cur) CGDisplayModeRelease(cur);
    }
}
static _Atomic int g_fullscreen;
int front_fullscreen(void) { return g_fullscreen; }
static _Atomic int g_window_covered;   // from the window's occlusion state, on the main thread
int front_window_covered(void) { return g_window_covered; }
// The display link waits while the screen settles (fullscreen not entered, a match queued, a switch < 1 s ago):
// docs/reference/frame-rate.md#the-display-link-started-before-fullscreens-mode-switch
static _Atomic int g_fs_starting, g_match_pending;
static _Atomic double g_fs_asked, g_mode_switched;
int front_screen_settling(void) {
    double now = CACurrentMediaTime();
    if (g_fs_starting && !g_fullscreen && now - g_fs_asked < 5.0) return 1;
    if (g_match_pending > 0 && g_fullscreen) return 1;
    return g_mode_switched > 0 && now - g_mode_switched < 1.0;
}
// Main thread: the layer is opaque (Direct-to-Display) except in fullscreen on a mirrored display, a workaround (front_mac_present.h)
static void layer_opaque_sync(NSScreen *screen, int fullscreen) {
    if (!g_layer) return;
    static int mode = -2, applied = -1;
    static CGDirectDisplayID last;   // the window's last screen, for a window on none
    if (mode == -2) {
        const char *e = getenv("DM_MAC_COMPOSITED");
        mode = front_mac_composited_mode(e);
        if (e && *e && mode == FRONT_MAC_COMP_AUTO && strcmp(e, "auto")) LOG("[RED] DM_MAC_COMPOSITED=%s: 1, 0 or auto; using auto", e);
    }
    if (screen) last = [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue];
    CGDirectDisplayID id = last ? last : CGMainDisplayID();
    int mirrored = id != kCGNullDirectDisplay && CGDisplayIsInMirrorSet(id) != 0;
    int op = front_mac_layer_opaque(mode, fullscreen, mirrored), first = applied < 0;
    if (op == applied) return;
    applied = op;
    g_layer.opaque = op ? YES : NO;
    const char *why = front_mac_layer_opaque_why(mode, fullscreen, mirrored);
    if (!op) LOG("front end: Metal layer non-opaque (%s%s)", why, mode == FRONT_MAC_COMP_AUTO ? "; DM_MAC_COMPOSITED=0 keeps it opaque" : "");
    else if (!first || mode == FRONT_MAC_COMP_OFF) LOG("front end: Metal layer opaque (%s)", why);
}
static int win_fullscreen(NSWindow *win) { return g_fullscreen || (win.styleMask & NSWindowStyleMaskFullScreen) != 0; }
// Fullscreen switches the screen, for this app only, to a mode whose rate the game's divides (DM_SCREEN_MATCH=0: off), VRR screens
// too (their 60 Hz link misses): docs/reference/frame-rate.md#fullscreen-switches-the-screen-to-a-rate-the-frame-rate-divides
static NSWindow *g_win;
static _Atomic int g_match_want;   // present thread only; 0 = not known
// A switch the screens undo (window off the screen, or back at its own mode after MATCH_SETTLE_S) and redone soon is a bounce;
// after MATCH_BOUNCES in a row the screen keeps its rate. Main thread.
#define MATCH_SETTLE_S 5.0
#define MATCH_BOUNCES 2
static double g_match_switched, g_match_undone;   // CACurrentMediaTime, 0 = never
static int g_match_bounces, g_match_bounced;
static int g_match_recheck;
static double mode_hz(CGDisplayModeRef m) { double hz = CGDisplayModeGetRefreshRate(m); return hz > 0 ? hz : 0; }   // 0 = not known
static int hz_multiple(double hz, int fps) { double k = round(hz / fps); return k >= 1 && fabs(hz / fps - k) < 0.01; }   // 59.94 Hz counts as 60
static CGError screen_set_mode(CGDirectDisplayID id, CGDisplayModeRef m) {
    CGDisplayConfigRef cfg;
    CGError r = CGBeginDisplayConfiguration(&cfg);
    if (r != kCGErrorSuccess) return r;
    r = CGConfigureDisplayWithDisplayMode(cfg, id, m, NULL);
    if (r != kCGErrorSuccess) { CGCancelDisplayConfiguration(cfg); return r; }
    r = CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
    if (r == kCGErrorSuccess) g_mode_switched = CACurrentMediaTime();
    return r;
}
static void screen_match_forget(void) {
    CGDisplayModeRelease(g_match_orig);
    g_match_orig = NULL;
    g_match_pick = 0;
    g_match_pick_vrr = 0;
}
// undone: the screens took the switch back, not the player leaving fullscreen
static void screen_match_restore(int on_it, int undone) {
    double hz = mode_hz(g_match_orig);
    CGError r = screen_set_mode(g_match_id, g_match_orig);
    LOG("front end: screen %s %.0f Hz", r == kCGErrorSuccess ? "back at" : "could not be switched back to", hz);
    if (r == kCGErrorSuccess && on_it) {   // the screen observer may not show the mode yet
        if (hz > 0) g_screen_fps = (int)round(hz);
        g_screen_variable_refresh = g_match_orig_vrr;
    }
    screen_match_forget();
    if (undone) g_match_undone = CACurrentMediaTime();
}
static void screen_match_apply(void);
static void screen_match_once(void) {   // never entered twice at once
    static int off = -1;
    if (off < 0) {
        const char *e = getenv("DM_SCREEN_MATCH");
        off = e && !strcmp(e, "0");
        if (off) LOG("front end: the screen keeps its refresh rate in fullscreen too (DM_SCREEN_MATCH=0)");
    }
    NSScreen *scr = g_win.screen;
    int want = g_match_want, full = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    CGDirectDisplayID id = scr ? [scr.deviceDescription[@"NSScreenNumber"] unsignedIntValue] : kCGNullDirectDisplay;
    if (g_match_orig && (!full || id != g_match_id)) screen_match_restore(id == g_match_id, full);
    static int last_want;
    if (!full || want != last_want) {
        if (g_match_bounced && full) LOG("front end: the screen's rate is matched again for %d fps", want);
        last_want = want;
        g_match_undone = 0;
        g_match_bounces = 0;
        g_match_bounced = 0;
    }
    if (off || g_match_bounced || !full || want <= 0 || !scr) return;
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    if (!cur) return;
    // read now when not switched: a fixed mode the player chose must not inherit a stale VRR flag
    if (!g_match_orig) screen_refresh(scr);
    int32_t cur_id = CGDisplayModeGetIODisplayModeID(cur);
    if (g_match_orig && cur_id == CGDisplayModeGetIODisplayModeID(g_match_orig)) {
        double since = CACurrentMediaTime() - g_match_switched;
        if (since < MATCH_SETTLE_S) {   // not shown yet: check again later in case no notification comes
            CGDisplayModeRelease(cur);
            if (!g_match_recheck) {
                g_match_recheck = 1;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((MATCH_SETTLE_S - since) * NSEC_PER_SEC) + 10000000LL), dispatch_get_main_queue(), ^{ g_match_recheck = 0; screen_match_apply(); });
            }
            return;
        }
        // back at its own mode without a switch back from this host: undone
        double own = mode_hz(cur);
        LOG("front end: screen at its own %.0f Hz again %.1f s after this host switched it", own, since);
        if (own > 0) g_screen_fps = (int)round(own);
        g_screen_variable_refresh = g_match_orig_vrr;
        screen_match_forget();
        g_match_undone = CACurrentMediaTime();
    }
    double hz = mode_hz(cur);
    // a mode this host chose, as it chose it (the observer may lag): a fixed mode is never left for the VRR one at a rate it matches
    int vrr = !g_match_orig ? g_screen_variable_refresh : cur_id == g_match_pick ? g_match_pick_vrr : cur_id == CGDisplayModeGetIODisplayModeID(g_match_orig) ? g_match_orig_vrr : 0;
    if (!vrr && (hz <= 0 || hz_multiple(hz, want))) {
        if (g_match_orig) {   // keep its fixed-mode state even if the observer lags
            if (hz > 0) g_screen_fps = (int)round(hz);
            g_screen_variable_refresh = 0;
        }
        CGDisplayModeRelease(cur);
        return;
    }
    size_t w = CGDisplayModeGetWidth(cur), h = CGDisplayModeGetHeight(cur), pw = CGDisplayModeGetPixelWidth(cur), ph = CGDisplayModeGetPixelHeight(cur);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)@{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES});
    CGDisplayModeRef pick = NULL;
    double pick_hz = 0;
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i);
        double r = mode_hz(m);
        if (CGDisplayModeGetWidth(m) != w || CGDisplayModeGetHeight(m) != h || CGDisplayModeGetPixelWidth(m) != pw || CGDisplayModeGetPixelHeight(m) != ph ||
            !CGDisplayModeIsUsableForDesktopGUI(m))
            continue;
        // VRR screen: a fixed-rate mode only
        if (vrr && (CGDisplayModeGetIODisplayModeID(m) == cur_id || (hz > 0 && fabs(r - hz) < 0.5))) continue;
        if (hz_multiple(r, want) && r > pick_hz) {
            pick = m;
            pick_hz = r;
        }
    }
    if (!pick && vrr && (hz <= 0 || hz_multiple(hz, want))) {
        LOG("front end: variable-refresh screen at %.0f Hz has no fixed-rate mode of this size at a multiple of %d Hz: it keeps its rate", hz, want);
    } else if (!pick) {
        LOG("front end: screen at %.0f Hz has no mode of this size at a multiple of %d Hz: frames are paced on the nearest refreshes", hz, want);
    } else {
        g_match_bounces = g_match_undone > 0 && CACurrentMediaTime() - g_match_undone < MATCH_SETTLE_S ? g_match_bounces + 1 : 0;
        if (g_match_bounces >= MATCH_BOUNCES) {
            g_match_bounced = 1;
            LOG("front end: screen at %.0f Hz keeps its rate until fullscreen is left or the frame rate changes: the screens took the switch back %d times in a row, and switching it again moves the screens about (DM_SCREEN_MATCH=0 keeps it from the start)", hz, g_match_bounces);
        } else {
            CGError r = screen_set_mode(id, pick);
            LOG("front end: %sscreen at %.0f Hz %s %s%.0f Hz for %d fps in fullscreen (DM_SCREEN_MATCH=0 keeps it)", vrr ? "variable-refresh " : "", hz, r == kCGErrorSuccess ? "switched to" : "could not be switched to", vrr ? "fixed " : "", pick_hz, want);
            if (r == kCGErrorSuccess) {
                g_match_switched = CACurrentMediaTime();
                if (!g_match_orig) {
                    g_match_orig = CGDisplayModeRetain(cur);
                    g_match_id = id;
                    g_match_orig_vrr = vrr;
                }
                g_match_pick = CGDisplayModeGetIODisplayModeID(pick);
                g_match_pick_vrr = g_match_pick == CGDisplayModeGetIODisplayModeID(g_match_orig) ? g_match_orig_vrr : 0;
                g_screen_fps = (int)round(pick_hz);   // the observer may not show the new mode yet
                g_screen_variable_refresh = g_match_pick_vrr;
            }
        }
    }
    if (modes) CFRelease(modes);
    CGDisplayModeRelease(cur);
}
// CGCompleteDisplayConfiguration runs the main run loop, where the screen notification can re-enter: a nested call runs once more after, from the main queue.
static void screen_match_apply(void) {
    static int busy, again;
    if (busy) {
        again = 1;
        return;
    }
    busy = 1;
    screen_match_once();
    busy = 0;
    if (again) {
        again = 0;
        g_match_pending++;
        dispatch_async(dispatch_get_main_queue(), ^{ screen_match_apply(); g_match_pending--; });
    }
}
void front_screen_match(int fps) {
    if (!g_layer || g_match_want == fps) return;
    g_match_want = fps;
    g_match_pending++;
    dispatch_async(dispatch_get_main_queue(), ^{ screen_match_apply(); g_match_pending--; });
}
static void cursor_hide_for_input(void) {
    if (!g_win.isKeyWindow || !NSApp.isActive) return;
    NSView *v = g_win.contentView;
    if (NSPointInRect([v convertPoint:g_win.mouseLocationOutsideOfEventStream fromView:nil], v.bounds)) [NSCursor setHiddenUntilMouseMoves:YES];
}
static void push(void) {
    int32_t lx = (g_d - g_a) * 32767, ly = (g_w - g_s) * 32767;
    int32_t rx = (g_rr - g_rl) * 32767, ry = (g_ru - g_rd) * 32767;
    dm_input_set(g_btn, lx, ly, rx, ry);
}
static void kbd_release(void) {
    g_btn = 0;
    g_w = g_a = g_s = g_d = 0;
    g_rl = g_ru = g_rr = g_rd = 0;
    push();
}
static int key_bit(unsigned short code) {
    switch (code) {
        case 38: return 0;  case 40: return 1;  case 32: return 2;  case 34: return 3;    // J K U I → A B X Y
        case 3: return 4;   case 5: return 5;                                            // F G → press stick
        case 12: return 6;  case 14: return 7;  case 6: return 8;   case 8: return 9;    // Q E Z C → L R ZL ZR
        case 36: return 10; case 48: return 11;                                          // Return Tab → + −
        case 123: return 12; case 126: return 13; case 124: return 14; case 125: return 15; // ← ↑ → ↓
    }
    return -1;
}
// the arrow and function keys carry the fn flag too: only a letter makes a Globe shortcut
static int fn_shortcut(NSEvent *e) {
    NSString *s = e.charactersIgnoringModifiers;
    return (e.modifierFlags & NSEventModifierFlagFunction) && s.length == 1 && [[NSCharacterSet letterCharacterSet] characterIsMember:[s characterAtIndex:0]];
}
static ctrl_prompt g_cp;
static int g_cp_confirm, g_cp_cancel;
static int cp_poll(void);
static void on_key(NSEvent *e, int down) {
    if (!g_input_on) return;
    unsigned short c = e.keyCode;
    if (cp_blocks_input(g_cp.phase)) {
        if (down && g_cp.phase == CP_OPEN) {
            if (c == 53 || c == 40) g_cp_cancel = 1;
            else if (c == 36 || c == 38) g_cp_confirm = 1;
            cp_poll();
        }
        return;
    }
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    int *rs = c == 123 ? &g_rl : c == 126 ? &g_ru : c == 124 ? &g_rr : c == 125 ? &g_rd : NULL;
    if (rs && (down ? (e.modifierFlags & NSEventModifierFlagShift) != 0 : *rs)) { *rs = down; push(); return; }
    int b = key_bit(c);
    uint64_t m = b >= 0 ? 1ull << b : c == 49 ? DM_BTN_SHAKE : 0;   // Space: shake
    g_btn = down ? (g_btn | m) : (g_btn & ~m);
    if (down && (m || c == 13 || c == 0 || c == 1 || c == 2)) cursor_hide_for_input();   // WASD
    push();
}

// Controllers hold player slots (pad_slots.h); player 1's merges with the keyboard. DM_PAD_MERGE=1: all are player 1, the last moved wins.
static pad_slots g_slots;
static GCController *g_held[DM_PLAYERS];   // kept alive while it holds the slot (the table keys on its pointer)
static int g_merge, g_kbd_own;
// a resting stick's drift (under 15%) does not hide the pointer; nor does deflection a DM_PAD_DEADZONE swallows
static void pad_set(int p, uint64_t m, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    const int32_t dead = (int32_t)(fmaxf(0.15f, dm_pad_deadzone()) * DM_STICK_MAX);
    if (m || abs(lx) > dead || abs(ly) > dead || abs(rx) > dead || abs(ry) > dead) cursor_hide_for_input();
    dm_pad_set_player(p, m, lx, ly, rx, ry);
}
static void hq_poll(void);
static home_quit g_hq;
static int g_press_to_join;
static int pad_join(const void *owner, const char *name);
static const char *pad_name(GCController *c);
static void pads_motion_changed(GCController *gone);
// read from their HID reports instead (pad_hid_mac.h, pad_jc1_mac.h)
static int pad_gc_unread(GCController *c) { return hidpad_gc_unread(c) || jc1_gc_unread(c); }
// The Switch Pro stick fallback (motion_pro_mac.h): a stick counts as live in GameController when its mapped or its unmapped axis moves
static GCController *g_pro_gc, *g_pro_gc_prev, *g_pro_gc_gone;
static int pro_gc_stick_nonzero(GCController *c, int right) {
    GCExtendedGamepad *g = c.extendedGamepad;
    GCControllerDirectionPad *d = right ? g.rightThumbstick : g.leftThumbstick;
    if (d.xAxis.value != 0.f || d.yAxis.value != 0.f) return 1;
    id<GCDirectionPadElement> u = c.input.unmappedInput.dpads[right ? GCInputRightThumbstick : GCInputLeftThumbstick];
    return u && (u.xAxis.value != 0.f || u.yAxis.value != 0.f);
}
static void pad_read(GCExtendedGamepad *g) {
    if (!g_input_on) return;
    hq_poll();
    if (hq_blocks_input(g_hq.phase)) return;
    int p = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)g.controller);
    if (p < 0 && (!g_press_to_join || pad_gc_unread(g.controller))) return;
    uint64_t m = dm_pad_face(g);   // by position; a Nintendo pad by its labels
    #define K(btn, bit) if (btn.isPressed) m |= bit
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R);
    // Workaround: triggers that rest above zero report isPressed (docs/reference/services.md#an-analog-trigger-at-rest-read-as-zl-held)
    if (g.leftTrigger.value > 0.25f) m |= DM_BTN_ZL;
    if (g.rightTrigger.value > 0.25f) m |= DM_BTN_ZR;
    K(g.buttonMenu, DM_BTN_PLUS); K(g.buttonOptions, DM_BTN_MINUS); K(g.leftThumbstickButton, DM_BTN_LS); K(g.rightThumbstickButton, DM_BTN_RS);   // nil on pads without them
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
    #undef K
    if (p < 0) {
        GCController *c = g.controller;
        if (!m) return;
        p = pad_join((__bridge const void *)c, pad_name(c));
        if (p < 0) return;
        g_held[p] = c;
        dm_motion_attach(c, p);
        pads_motion_changed(nil);
    }
    if (cp_poll()) return;
    if (m) merge_rumble_to(NULL);
    dm_haptics_apple_controller(p, g.controller);
    // GameController's sticks carry no dead zone of their own: DM_PAD_DEADZONE sets one (dm_input.h), else they pass through raw
    const float gc_dead = dm_pad_deadzone();
    int32_t st[4];
    dm_stick_apply(gc_dead, g.leftThumbstick.xAxis.value, g.leftThumbstick.yAxis.value, &st[0], &st[1]);
    dm_stick_apply(gc_dead, g.rightThumbstick.xAxis.value, g.rightThumbstick.yAxis.value, &st[2], &st[3]);
    if (g_pro_sticks && [g.controller.productCategory isEqualToString:@"Switch Pro Controller"] && !pad_gc_unread(g.controller)) {
        int nz[2] = {g.leftThumbstick.xAxis.value != 0.f || g.leftThumbstick.yAxis.value != 0.f,
                     g.rightThumbstick.xAxis.value != 0.f || g.rightThumbstick.yAxis.value != 0.f};
        dm_pro_sticks(p, nz, st);
    }
    pad_set(p, m, st[0], st[1], st[2], st[3]);
}
// Without the claim macOS takes Home and Capture as system gestures (Home opens the Games app)
static void pad_keep_system_buttons(GCController *c) {
    for (NSString *b in @[GCInputButtonHome, GCInputButtonShare]) c.physicalInputProfile.buttons[b].preferredSystemGestureState = GCSystemGestureStateDisabled;
    c.extendedGamepad.buttonHome.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL down) { (void)b; (void)v; (void)down; if (g_input_on) hq_poll(); };
}
static const char *pad_name(GCController *c) { return c.vendorName.UTF8String ?: "?"; }
static void npad_name(int p, char *out, size_t cap) {
    uint32_t id = dm_player_npad(p);
    if (id == 0x20) snprintf(out, cap, "handheld");
    else snprintf(out, cap, "%u", id);
}
// The raw HID motion and the Pro stick fallback follow the controllers (gone: one just removed, still listed), each model counted on its own
static int pro_gc_player(GCController *c) { return g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)c); }
static int pro_gc_sticks(int player, int nz[2]) {
    GCController *c = g_pro_gc;
    if (!c || !c.extendedGamepad || pad_gc_unread(c) || pro_gc_player(c) != player) return 0;
    nz[0] = pro_gc_stick_nonzero(c, 0); nz[1] = pro_gc_stick_nonzero(c, 1);
    return 1;
}
static void pro_stick_changed(int player) {
    GCController *c = g_pro_gc;
    if (!c || pro_gc_player(c) != player) c = g_pro_gc_prev;
    if (!c || c == g_pro_gc_gone || !c.extendedGamepad || pad_gc_unread(c) || pro_gc_player(c) != player) return;
    if (![GCController.controllers containsObject:c]) return;
    pad_read(c.extendedGamepad);
}
static void pads_motion_changed(GCController *gone) {
    int count[PRO_MODELS] = {0}, player[PRO_MODELS];
    for (int m = 0; m < PRO_MODELS; m++) player[m] = -1;
    GCController *pro = nil;
    for (GCController *c in GCController.controllers) {
        int m = c != gone && c.extendedGamepad && !pad_gc_unread(c) ? pro_model_of_controller(c.productCategory.UTF8String, c.vendorName.UTF8String) : -1;
        if (m >= 0) {
            count[m]++;
            player[m] = pro_gc_player(c);
            if (m == PRO_MODEL_PRO) pro = c;
        }
    }
    g_pro_gc_prev = g_pro_gc; g_pro_gc = count[PRO_MODEL_PRO] == 1 ? pro : nil; g_pro_gc_gone = gone;
    dm_motion_pro_players(count, player);
    g_pro_gc_prev = nil; g_pro_gc_gone = nil;
}
static void pro_devices_changed(void) { pads_motion_changed(nil); }
// DM_PAD_PRESS_TO_JOIN=1: a controller becomes the lowest free player at its first button; not with DM_PAD_MERGE
static int pad_join(const void *owner, const char *name) {
    int p = g_press_to_join ? pad_slot_take_lowest(&g_slots, owner, name) : pad_slot_take(&g_slots, owner, name);
    if (p < 0) {
        LOG("controller '%s': a button pressed, but all %d players are taken", name, DM_PLAYERS);
        return -1;
    }
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s': a button pressed: player %d (npad %s)", name, p + 1, nb);
    dm_pad_players(pad_slot_mask(&g_slots));
    return p;
}
static int merge_pads_left(GCController *gone) {
    if (g_hidpads || g_gippads || g_jc1_pads || dm_switch2_pads()) return 1;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) return 1;
    return 0;
}
static void pad_listen(GCController *c) { c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); }; }
// gone: the controller just removed, nil on a connect
static void pads_changed(GCController *gone) {
    if (g_merge) {
        int any = 0;
        for (GCController *c in GCController.controllers) if (jc1_gc_unread(c)) pad_keep_system_buttons(c);
        for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) {
            if (jc1_gc_unread(c)) {
                c.extendedGamepad.valueChangedHandler = nil;
                continue;
            }
            any = 1;
            pad_keep_system_buttons(c);
            pad_listen(c);
            dm_motion_attach(c, 0);
            pad_read(c.extendedGamepad);
        }
        if (gone) dm_motion_detach(gone, 0);
        if (!any) {
            if (!g_hidpads && !g_gippads && !g_jc1_pads && !dm_switch2_pads()) dm_pad_set(0, 0, 0, 0, 0);
            dm_haptics_apple_controller(0, nil);
        }
        pads_motion_changed(gone);
        return;
    }
    hidpad_twins_eval(gone);
    hidpad_twins_release();
    NSArray<GCController *> *now = GCController.controllers;
    char nb[16];
    for (GCController *c in now) if (c != gone && jc1_gc_unread(c)) pad_keep_system_buttons(c);
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p] && (g_held[p] == gone || ![now containsObject:g_held[p]] || pad_gc_unread(g_held[p]))) {
        npad_name(p, nb, sizeof nb);
        LOG("controller '%s' disconnected: player %d (npad %s) is free", pad_name(g_held[p]), p + 1, nb);
        g_held[p].extendedGamepad.valueChangedHandler = nil;
        dm_motion_detach(g_held[p], p);
        dm_haptics_apple_controller(p, nil);
        pad_slot_free(&g_slots, (__bridge const void *)g_held[p]);
        g_held[p] = nil;
        dm_pad_set_player(p, 0, 0, 0, 0, 0);
    }
    for (GCController *c in now) if (c.extendedGamepad && c != gone && pad_slot_of(&g_slots, (__bridge const void *)c) < 0) {
        if (pad_gc_unread(c)) {
            pad_keep_system_buttons(c);
            c.extendedGamepad.valueChangedHandler = nil;
            continue;
        }
        if (g_press_to_join) {
            pad_keep_system_buttons(c);
            if (!c.extendedGamepad.valueChangedHandler) LOG("controller '%s' connected: press a button to play (DM_PAD_PRESS_TO_JOIN)", pad_name(c));
            pad_listen(c);
            continue;
        }
        int p = pad_slot_take(&g_slots, (__bridge const void *)c, pad_name(c));
        if (p < 0) {
            LOG("controller '%s' connected: all %d players are taken, not used", pad_name(c), DM_PLAYERS);
            continue;
        }
        g_held[p] = c;
        pad_keep_system_buttons(c);
        pad_listen(c);
        dm_motion_attach(c, p);
        npad_name(p, nb, sizeof nb);
        LOG("controller '%s' connected: player %d (npad %s)%s", pad_name(c), p + 1, nb, c.motion.hasRotationRate ? ", with motion (gyro)" : "");
        pad_read(c.extendedGamepad);
    }
    hidpad_twins_take(gone);
    dm_pad_players(pad_slot_mask(&g_slots));
    pads_motion_changed(gone);
    cp_poll();
}
// macOS counts no controller input as user activity: keep the display awake while the game window is key
static id g_display_activity;
static void display_awake(int on) {
    if (on && !g_display_activity)
        g_display_activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityIdleDisplaySleepDisabled reason:@"NativeNavi active game window"];
    else if (!on && g_display_activity) {
        [NSProcessInfo.processInfo endActivity:g_display_activity];
        g_display_activity = nil;
    }
}
// A window that is no longer key gets no key-up: release what the front end pressed; back in focus, read each controller as it is.
static void pad_release_with_joys(int p) {
    if (p < 0) return;
    dm_pad_set_player(p, 0, 0, 0, 0, 0);
    dm_joy_set(p, 0, 0, 0, 0);
    dm_joy_set(p, 1, 0, 0, 0);
}
static void pads_release(void) {
    if (g_merge) {
        if (merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p]) dm_pad_set_player(p, 0, 0, 0, 0, 0);
    for (hid_pad *d = g_hidpads; d; d = d->next) {
        int p = pad_slot_of(&g_slots, d);
        if (p >= 0) dm_pad_set_player(p, 0, 0, 0, 0, 0);
    }
    for (gip_pad *d = g_gippads; d; d = d->next) {
        int p = pad_slot_of(&g_slots, d);
        if (p >= 0) dm_pad_set_player(p, 0, 0, 0, 0, 0);
    }
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) pad_release_with_joys(s2pad_player(d));
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) pad_release_with_joys(jc1pad_player(d));
}
static void hidpad_motion_clear(void) {
    for (hid_pad *d = g_hidpads; d; d = d->next) {
        int p = hidpad_full(d) ? pad_slot_of(&g_slots, d) : -1;
        if (p >= 0) dm_motion_clear_hid(p);
    }
}
static void pads_reread(void) {
    if (g_merge) {
        for (GCController *c in GCController.controllers) if (c.extendedGamepad) pad_read(c.extendedGamepad);
    } else {
        for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p].extendedGamepad) pad_read(g_held[p].extendedGamepad);
    }
    for (hid_pad *d = g_hidpads; d; d = d->next) hidpad_read(d);
    for (gip_pad *d = g_gippads; d; d = d->next) gippad_read(d);
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) s2pad_read(d);
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) jc1pad_read(d);
}
static void hq_close(void);
static void input_release(void) {
    hq_close();
    [NSCursor setHiddenUntilMouseMoves:NO];
    display_awake(0);
    g_input_on = 0;
    dm_motion_pro_active(0);
    hidpad_motion_clear();
    dm_switch2_active(0);
    dm_jc1_active(0);
    kbd_release();
    pads_release();
}
static void input_resume(void) {
    display_awake(1);
    g_input_on = 1;
    dm_motion_pro_active(1);
    dm_switch2_active(1);
    dm_jc1_active(1);
    pads_reread();
}
// A HID gamepad collection does not prove a controller (keyboard receivers declare one): a raw HID pad joins at its first press
// (docs/reference/services.md#a-keyboard-receiver-that-declares-a-gamepad-takes-player-1); a twin GameController lists joins at connection.
static int hidpad_first_press(const hid_pad *d) { return !g_merge && (g_press_to_join || !d->twin); }
static int hidpad_player(const hid_pad *d) { return g_merge ? 0 : pad_slot_of(&g_slots, d); }
static void hidpad_show_player(hid_pad *d, int p, uint64_t m) {
    hidpad_lights(d, p);
    if (!hidpad_rumbles(d)) return;
    if (g_merge) {
        if (m) merge_rumble_to(dm_hidpad_rumble);
        return;
    }
    if (d->sink != p + 1) {
        d->sink = p + 1;
        dm_haptics_apple_set_sink(p, dm_hidpad_rumble);
    }
}
static void hidpad_read(hid_pad *d) {
    if (!g_input_on || hq_blocks_input(g_hq.phase)) return;
    int p = hidpad_player(d), full = hidpad_full(d);
    uint64_t m = full ? d->pro_m : pad_hid_buttons(&d->st);
    if (p < 0) {
        if (!hidpad_first_press(d) || !m) return;
        p = pad_join(d, d->name);
        if (p < 0) return;
    }
    hidpad_show_player(d, p, m);
    if (cp_poll()) return;
    if (full) {
        pad_set(p, m, d->pro_s[0], d->pro_s[1], d->pro_s[2], d->pro_s[3]);
        return;
    }
    int32_t s[4];
    pad_hid_sticks(&d->st, s);
    pad_set(p, m, s[0], s[1], s[2], s[3]);
}
static void hidpad_attach(hid_pad *d) {
    if (hidpad_first_press(d)) {
        LOG("controller '%s' (HID) connected: press a button to play%s", d->name, g_press_to_join ? " (DM_PAD_PRESS_TO_JOIN)" : "");
        hidpad_read(d);
        return;
    }
    int p = g_merge ? 0 : pad_slot_take(&g_slots, d, d->name);
    if (p < 0) {
        LOG("controller '%s' (HID) connected: all %d players are taken, not used", d->name, DM_PLAYERS);
        return;
    }
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s' (HID) connected: player %d (npad %s)", d->name, p + 1, nb);
    if (!g_merge) dm_pad_players(pad_slot_mask(&g_slots));
    hidpad_read(d);
}
static void hidpad_motion(hid_pad *d, const float acc[3], const float rot[3], uint64_t ns) {
    int p = g_input_on && !g_merge ? pad_slot_of(&g_slots, d) : -1;
    if (p < 0) return;
    dm_motion_set_hid(p, acc, rot, ns);
}
static void hidpad_models_changed(void) { pads_changed(nil); }
static void hidpad_detach(hid_pad *d) {   // d is already off g_hidpads
    if (g_merge) {
        LOG("controller '%s' (HID) disconnected", d->name);
        int rumbles = 0;
        for (hid_pad *o = g_hidpads; o; o = o->next) rumbles |= hidpad_rumbles(o);
        if (!rumbles && g_merge_sink == dm_hidpad_rumble) merge_rumble_to(NULL);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d);
    if (p < 0) return;
    if (hidpad_full(d)) dm_motion_clear_hid(p);
    if (d->sink) {
        d->sink = 0;
        dm_haptics_apple_set_sink(p, NULL);
    }
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s' (HID) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);
    dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();
}
// Xbox One (GIP) and Xbox 360 (XUSB) pads over USB with no macOS driver (pad_gip_mac.h): a player from its connection, as a GameController controller,
// or at its first press with DM_PAD_PRESS_TO_JOIN; an Xbox One pad's Xbox button is Home (hq_buttons)
static void gippad_read(gip_pad *d) {
    if (!g_input_on) return;
    hq_poll();
    if (hq_blocks_input(g_hq.phase)) return;
    int p = g_merge ? 0 : pad_slot_of(&g_slots, d);
    if (p < 0 && (!g_press_to_join || !d->st.buttons || (p = pad_join(d, d->name)) < 0)) return;
    if (cp_poll()) return;
    pad_set(p, d->st.buttons, d->st.s[0], d->st.s[1], d->st.s[2], d->st.s[3]);
}
static void gippad_attach(gip_pad *d) {
    if (g_press_to_join && !g_merge) {
        LOG("controller '%s' (USB) connected: press a button to play (DM_PAD_PRESS_TO_JOIN)", d->name);
        cp_poll();
        return;
    }
    int p = g_merge ? 0 : pad_slot_take(&g_slots, d, d->name);
    if (p < 0) {
        LOG("controller '%s' (USB) connected: all %d players are taken, not used", d->name, DM_PLAYERS);
        return;
    }
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s' (USB) connected: player %d (npad %s)", d->name, p + 1, nb);
    if (!g_merge) dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();
}
static void gippad_detach(gip_pad *d) {   // d is already off g_gippads
    if (g_merge) {
        LOG("controller '%s' (USB) disconnected", d->name);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d);
    if (p < 0) { cp_poll(); return; }   // one that had not joined: one fewer waiting
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s' (USB) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);
    dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();
}
static int s2pad_player(s2_pad *d) { return g_merge ? 0 : pad_slot_of(&g_slots, d); }
static void merge_rumble_to(void (*sink)(const dm_haptic_state *)) {
    if (!g_merge || g_merge_sink == sink) return;
    g_merge_sink = sink;
    dm_haptics_apple_set_sink(0, sink);
}
static int s2pad_blocked(void) { return !g_input_on || hq_blocks_input(g_hq.phase) || cp_blocks_input(g_cp.phase); }
static void s2pad_read(s2_pad *d) {
    hq_poll();
    if (!g_input_on || hq_blocks_input(g_hq.phase)) return;
    int p = s2pad_player(d);
    uint64_t m;
    int32_t s[4];
    if (!s2_pad_input(&d->st, &m, s)) return;
    if (p < 0) {
        if (!g_press_to_join || !m) return;
        p = pad_join(d, d->name);
        if (p < 0) return;
        dm_haptics_apple_set_sink(p, dm_switch2_rumble);
    }
    if (cp_poll()) return;
    if (m) merge_rumble_to(dm_switch2_rumble);
    pad_set(p, m, s[0], s[1], s[2], s[3]);
}
static int s2pad_attach(s2_pad *d) {   // 1: kept; 0: no free player
    if (g_press_to_join && !g_merge) {
        LOG("controller '%s' (Bluetooth) connected: press a button to play (DM_PAD_PRESS_TO_JOIN)", d->name);
        cp_poll();
        return 1;
    }
    int p = g_merge ? 0 : pad_slot_take(&g_slots, d, d->name);
    if (p < 0) {
        LOG("controller '%s' (Bluetooth) connected: all %d players are taken, not used", d->name, DM_PLAYERS);
        return 0;
    }
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s' (Bluetooth) connected: player %d (npad %s)", d->name, p + 1, nb);
    if (!g_merge) {
        dm_pad_players(pad_slot_mask(&g_slots));
        dm_haptics_apple_set_sink(p, dm_switch2_rumble);
    }
    cp_poll();
    return 1;
}
static void s2pad_detach(s2_pad *d) {   // d is already off g_s2pads
    if (g_merge) {
        LOG("controller '%s' (Bluetooth) disconnected", d->name);
        if (!dm_switch2_pads() && g_merge_sink == dm_switch2_rumble) merge_rumble_to(NULL);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d);
    if (p < 0) {
        cp_poll();
        return;
    }
    dm_haptics_apple_set_sink(p, NULL);
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s' (Bluetooth) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);
    dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();
}
static int jc1pad_player(jc1_pad *d) { return g_merge ? 0 : pad_slot_of(&g_slots, d); }
static int jc1pad_blocked(void) { return s2pad_blocked(); }
static void jc1pad_read(jc1_pad *d) {
    hq_poll();
    if (!g_input_on || hq_blocks_input(g_hq.phase)) return;
    int p = jc1pad_player(d);
    uint64_t m;
    int32_t s[4];
    if (!jc1_pad_state(d, &m, s)) return;
    if (p < 0) {
        if (!m) return;
        p = pad_join(d, d->name);
        if (p < 0) return;
        dm_haptics_apple_set_sink(p, dm_jc1_rumble);
    }
    if (cp_poll()) return;
    if (m) merge_rumble_to(dm_jc1_rumble);
    pad_set(p, m, s[0], s[1], s[2], s[3]);
}
static void jc1pad_attach(jc1_pad *d) {
    if (g_merge) LOG("controller '%s' (HID) connected: player 1 (DM_PAD_MERGE)", d->name);
    else {
        LOG("controller '%s' (HID) connected: press a button to play", d->name);
        cp_poll();
    }
    jc1pad_read(d);
}
static void jc1pad_detach(jc1_pad *d) {   // d is already off g_jc1_pads
    if (g_merge) {
        LOG("controller '%s' (HID) disconnected", d->name);
        if (!g_jc1_pads && g_merge_sink == dm_jc1_rumble) merge_rumble_to(NULL);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d);
    if (p < 0) {
        cp_poll();
        return;
    }
    dm_haptics_apple_set_sink(p, NULL);
    char nb[16];
    npad_name(p, nb, sizeof nb);
    LOG("controller '%s' (HID) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);
    dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();
}
static void jc1_models_changed(void) { pads_changed(nil); }
// Home held 2 s opens the quit prompt: A quits as ⌘Q does, B or Home closes; meanwhile the game is told it lost focus. DM_HOME_QUIT=0: off
static int g_hq_on;
static NSTimer *g_hq_timer;
static NSView *g_hq_hold, *g_hq_prompt;
static CAShapeLayer *g_hq_ring;
static char g_hq_text[HQ_TEXTS][256];
static void hq_start(void) {
    g_hq_on = hq_enabled(getenv("DM_HOME_QUIT"));
    char title[256];
    snprintf(title, sizeof title, "Quit %s?", DM_GAME_NAME);
    const char *def[HQ_TEXTS] = {"Keep holding to quit", title, "Unsaved progress will be lost.", "Quit", "Cancel"};
    const char *text = getenv("DM_HOME_QUIT_TEXT");
    for (int i = 0; i < HQ_TEXTS; i++) hq_text_line(text, i, def[i], g_hq_text[i], sizeof g_hq_text[i]);
    LOG("front end: hold the Home button to quit %s", g_hq_on ? "is on" : "is off (DM_HOME_QUIT=0)");
}
// macOS 27's Home setting, readable only while a controller is connected: "system" keeps Home whatever the host asks
static void hq_log_setting(void) {
#if defined(__MAC_27_0)
    if (@available(macOS 27.0, *)) {
        static int done;
        if (done) return;
        int any = 0;
        for (GCController *c in GCController.controllers) if (c.extendedGamepad) any = 1;
        if (!any) return;
        done = 1;
        GCControllerHomeButtonSettingCustomizationStatus by = GCControllerHomeButtonSettingCustomizationDefault;
        NSError *err = nil;
        GCControllerHomeButtonSettingInAppAction v = [[GCControllerHomeButtonSettingsManager new] readControllerHomeButtonInAppAction:&by withError:&err];
        LOG("front end: Home button in apps: %s (%s)%s%s", hq_in_app_name((long)v), by == GCControllerHomeButtonSettingCustomizationUser ? "set by the player" : "the system's default",
            err ? ", " : "", err ? err.localizedDescription.UTF8String : "");
    }
#endif
}
static void hq_buttons(int *home, int *a, int *b, GCController **holder) {
    *home = *a = *b = 0;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *g = c.extendedGamepad;
        if (!g) continue;
        if (g.buttonHome.isPressed) {
            *home = 1;
            if (!*holder) *holder = c;
        }
        *a |= g.buttonA.isPressed;
        *b |= g.buttonB.isPressed;
    }
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) {
        if (s2_pad_home(&d->st)) *home = 1;
        s2_pad_ab(&d->st, a, b);
    }
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) {
        if (jc1_pad_home(d)) *home = 1;
        jc1_pad_ab(d, a, b);
    }
    for (gip_pad *d = g_gippads; d; d = d->next) {
        *home |= d->st.guide;
        *a |= !!(d->st.buttons & DM_BTN_A);
        *b |= !!(d->st.buttons & DM_BTN_B);
    }
}
static void hq_fade(NSView *v, int in) {
    if (!v) return;
    if (in) v.alphaValue = 0;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) { ctx.duration = 0.18; v.animator.alphaValue = in ? 1 : 0; }
                        completionHandler:^{ if (!in) [v removeFromSuperview]; }];
}
static void hq_hide_hold(void) {
    [g_hq_timer invalidate];
    g_hq_timer = nil;
    [g_hq_hold removeFromSuperview];
    g_hq_hold = nil;
    g_hq_ring = nil;
}
static void hq_hide_prompt(void) { hq_fade(g_hq_prompt, 0); g_hq_prompt = nil; }
static void hq_changed(hq_phase was, hq_phase now, GCController *holder) {
    if (was == HQ_HOLDING) hq_hide_hold();
    NSView *cv = g_win.contentView;
    switch (now) {
    case HQ_HOLDING:
        g_hq_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; hq_poll(); }];
        break;
    case HQ_PROMPT: {
        pads_release();
        dm_focus_set(2);
        GCExtendedGamepad *g = holder.extendedGamepad;
        g_hq_prompt = hq_prompt_view(cv.bounds, @(g_hq_text[HQ_TEXT_TITLE]), @(g_hq_text[HQ_TEXT_NOTE]), @(g_hq_text[HQ_TEXT_QUIT]), @(g_hq_text[HQ_TEXT_CANCEL]),
                                     g.buttonA.sfSymbolsName, g.buttonB.sfSymbolsName);
        [cv addSubview:g_hq_prompt];
        hq_fade(g_hq_prompt, 1);
        LOG("front end: Home held, quit prompt open");
        break;
    }
    case HQ_SETTLE:
        hq_hide_prompt();
        dm_focus_set(1);
        LOG("front end: quit prompt closed, the game goes on");
        break;
    case HQ_IDLE:
        if (was == HQ_SETTLE) pads_reread();
        break;
    case HQ_QUIT:
        LOG("front end: quit from the Home button prompt");
        [NSApp terminate:nil];
        break;
    }
}
static void hq_poll(void) {
    static int busy;
    if (!g_hq_on || !g_win || busy) return;
    busy = 1;
    int home, a, b;
    GCController *holder = nil;
    hq_buttons(&home, &a, &b, &holder);
    double now = CACurrentMediaTime();
    hq_phase was = g_hq.phase, ph = hq_step(&g_hq, now, home, a, b);
    if (ph != was) hq_changed(was, ph, holder);
    double fill = hq_ring(&g_hq, now);
    if (fill >= 0) {
        if (!g_hq_hold) {
            NSView *cv = g_win.contentView;
            CAShapeLayer *ring = nil;
            g_hq_hold = hq_hold_view(cv.bounds, @(g_hq_text[HQ_TEXT_HOLD]), &ring);
            g_hq_ring = ring;
            [cv addSubview:g_hq_hold];
            hq_fade(g_hq_hold, 1);
        }
        hq_hold_set(g_hq_ring, fill);
    }
    busy = 0;
}
static void hq_close(void) {
    if (g_hq.phase == HQ_IDLE || g_hq.phase == HQ_QUIT) return;
    if (g_hq.phase == HQ_PROMPT) LOG("front end: quit prompt closed, the window lost the focus");
    hq_cancel(&g_hq);
    hq_hide_hold();
    hq_hide_prompt();
}
// The controller applet's prompt (ctrl_prompt.h): A and B on any controller, Return / J done, Escape / K cancels
static NSView *g_cp_view;
static dispatch_semaphore_t g_cp_done;
static int g_cp_ok, g_cp_shown_joined = -1, g_cp_shown_on = -1;
static void cp_buttons(int *a, int *b) {
    *a = *b = 0;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *g = c.extendedGamepad;
        if (!g) continue;
        *a |= g.buttonA.isPressed;
        *b |= g.buttonB.isPressed;
    }
    for (hid_pad *d = g_hidpads; d; d = d->next) {
        uint64_t m = d->twin && hidpad_is_pro(d) ? d->pro_m : pad_hid_buttons(&d->st);
        *a |= !!(m & DM_BTN_A);
        *b |= !!(m & DM_BTN_B);
    }
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) s2_pad_ab(&d->st, a, b);
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) jc1_pad_ab(d, a, b);
    for (gip_pad *d = g_gippads; d; d = d->next) {
        *a |= !!(d->st.buttons & DM_BTN_A);
        *b |= !!(d->st.buttons & DM_BTN_B);
    }
}
static int cp_waiting(void) {
    if (g_merge) return 0;
    int n = 0;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad && !pad_gc_unread(c) && pad_slot_of(&g_slots, (__bridge const void *)c) < 0) n++;
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) if (pad_slot_of(&g_slots, d) < 0) n++;
    for (hid_pad *d = g_hidpads; d; d = d->next) if (pad_slot_of(&g_slots, d) < 0) n++;
    for (gip_pad *d = g_gippads; d; d = d->next) if (pad_slot_of(&g_slots, d) < 0) n++;
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) if (pad_slot_of(&g_slots, d) < 0) n++;
    return n;
}
static void cp_show(void) {
    int on = cp_can_confirm(&g_cp);
    if (g_cp_view && g_cp_shown_joined == g_cp.joined && g_cp_shown_on == on) return;
    const char *env = getenv("DM_CTRL_PROMPT_TEXT");
    char t[CP_TEXTS][256];
    for (int i = 0; i < CP_TEXTS; i++) cp_text(env, i, &g_cp, t[i], sizeof t[i]);
    GCExtendedGamepad *g = nil;
    for (GCController *c in GCController.controllers) if ((g = c.extendedGamepad)) break;
    NSView *cv = g_win.contentView, *v = hq_card_view(cv.bounds, @(t[CP_TEXT_TITLE]), [NSString stringWithFormat:@"%s\n%s", t[CP_TEXT_HINT], t[CP_TEXT_COUNT]], @(t[CP_TEXT_DONE]),
                                                   @(t[CP_TEXT_CANCEL]), g.buttonA.sfSymbolsName, g.buttonB.sfSymbolsName, on);
    if (g_cp_view) [g_cp_view removeFromSuperview];
    [cv addSubview:v];
    if (!g_cp_view) hq_fade(v, 1);
    g_cp_view = v;
    g_cp_shown_joined = g_cp.joined;
    g_cp_shown_on = on;
}
// returns whether it keeps the game's input
static int cp_poll(void) {
    if (g_cp.phase == CP_IDLE) return 0;
    int a, b, joined = dm_players_reported();
    cp_buttons(&a, &b);
    int confirm = g_cp_confirm, cancel = g_cp_cancel;
    g_cp_confirm = g_cp_cancel = 0;
    cp_phase was = g_cp.phase, ph = cp_step(&g_cp, joined, cp_waiting(), a, b, confirm, cancel);
    if (was == CP_OPEN && ph == CP_OPEN) cp_show();
    if (was == CP_OPEN && ph != CP_OPEN) {
        hq_fade(g_cp_view, 0);
        g_cp_view = nil;
        LOG("front end: controller prompt closed: %d players (%s)", joined, g_cp.ok ? "done" : "canceled");
        g_cp_ok = g_cp.ok;
        dispatch_semaphore_signal(g_cp_done);
        ph = cp_step(&g_cp, joined, 0, a, b, 0, 0);
    }
    if (ph == CP_IDLE) {
        pads_reread();
        return 0;
    }
    return 1;
}
int front_ctrl_support(int min, int max) {
    if (!g_layer) return -1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        kbd_release();
        pads_release();
        int a, b;
        cp_buttons(&a, &b);
        g_cp_done = done;
        cp_open(&g_cp, min, max, dm_players_reported(), a, b);
        LOG("front end: controller prompt open: %d to %d players wanted, %d in, %d controllers waiting to join", min, max, g_cp.joined, cp_waiting());
        cp_show();
        cp_poll();
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return g_cp_ok;
}
static void pads_start(void) {
    const char *e = getenv("DM_PAD_MERGE"); g_merge = e && atoi(e);
    e = getenv("DM_KBD_PLAYER"); g_kbd_own = !g_merge && e && !strcmp(e, "own");
    e = getenv("DM_PAD_PRESS_TO_JOIN"); g_press_to_join = !g_merge && e && atoi(e);
    static const char kbd = 0;
    if (g_kbd_own) pad_slot_take(&g_slots, &kbd, "keyboard");
    LOG("front end: %s", g_merge ? "every controller is player 1 (DM_PAD_MERGE)" : g_kbd_own ? "the keyboard is player 1, each controller a player of its own from player 2 on (DM_KBD_PLAYER=own)"
                                                                           : "each controller is a player of its own, the keyboard plays with player 1");
    if (g_press_to_join) LOG("front end: a controller becomes a player at its first button press, the lowest player free (DM_PAD_PRESS_TO_JOIN)");
    hq_start();
    pads_changed(nil);
    hq_log_setting();
    dm_motion_pro_start(pro_devices_changed, pro_gc_sticks, pro_stick_changed);
    dm_jc1_start(g_merge);
    hidpad_start();
    gippad_start();
    static const dm_switch2_front s2front = {s2pad_attach, s2pad_detach, s2pad_read, s2pad_player, s2pad_blocked};
    dm_switch2_start(&s2front);
}

@interface DmGameView : NSView @end
@implementation DmGameView
// A layer-backed view's layer keeps the first 1280x720 drawable in fullscreen: follow the view in pixels
- (void)fitDrawable {
    CAMetalLayer *l = (CAMetalLayer *)self.layer;
    if (![l isKindOfClass:CAMetalLayer.class] || !self.window) return;
    l.contentsScale = self.window.backingScaleFactor;
    NSSize px = [self convertSizeToBacking:self.bounds.size];
    if (px.width < 1 || px.height < 1 || CGSizeEqualToSize(l.drawableSize, px)) return;
    l.drawableSize = px;
    dm_fstep_settle();
    LOG("front end: drawable %.0fx%.0f", px.width, px.height);
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self fitDrawable]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self fitDrawable]; }
@end

// AppKit reports a refused fullscreen request only to the window delegate
static void (^g_fs_failed)(void);
@interface DmWinDelegate : NSObject <NSWindowDelegate> @end
@implementation DmWinDelegate
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { (void)w; if (g_fs_failed) g_fs_failed(); }
@end

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];   // per thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt};
    sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn();
    LOG("guest thread finished");
    exit(0);
}

// Metal reads MTL_HUD_ENABLED at the first device, possibly before the window
__attribute__((constructor)) static void hud_env(void) { front_hud_env(); }

@interface DmModsMenuDelegate : NSObject <NSMenuDelegate>
@end
@implementation DmModsMenuDelegate
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    int n = dm_mods_count();
    NSString *pack = nil;
    for (int i = 0; i < n; i++) {
        NSArray<NSString *> *parts = [@(dm_mods_name(i)) componentsSeparatedByString:@"/"];
        if (parts.count > 1 && (!pack || ![parts[0] isEqualToString:pack])) [menu addItemWithTitle:parts[0] action:nil keyEquivalent:@""].enabled = NO;
        pack = parts.count > 1 ? parts[0] : nil;
        NSMenuItem *it = [menu addItemWithTitle:parts.lastObject action:@selector(toggleMod:) keyEquivalent:@""];
        if (dm_mods_exclusive(i)) it.toolTip = @"Replaces the same files as another mod of this pack: turning it on turns that one off";
        it.target = self; it.tag = i; it.state = dm_mods_on(i) ? NSControlStateValueOn : NSControlStateValueOff; it.indentationLevel = pack ? 1 : 0;
    }
    if (!n) [menu addItemWithTitle:@"No mods in the mods folder" action:nil keyEquivalent:@""].enabled = NO;
    if (dm_mods_edited()) [menu addItemWithTitle:@"Code patches change at the next start" action:nil keyEquivalent:@""].enabled = NO;
    [menu addItem:[NSMenuItem separatorItem]];
    const char *dir = dm_mods_dir_path();
    NSMenuItem *op = [menu addItemWithTitle:@"Open Mods Folder" action:*dir ? @selector(openModsFolder:) : nil keyEquivalent:@""];
    op.target = self; op.enabled = *dir != 0;
}
- (void)toggleMod:(NSMenuItem *)it { dm_mods_set((int)it.tag, !dm_mods_on((int)it.tag)); }
- (void)openModsFolder:(NSMenuItem *)it {
    (void)it;
    NSString *dir = @(dm_mods_dir_path());
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
}
@end
static DmModsMenuDelegate *g_mods_menu;
@interface DmAmiiboMenuDelegate : NSObject <NSMenuDelegate>
@end
@implementation DmAmiiboMenuDelegate
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    NSMenuItem *scan = [menu addItemWithTitle:@"Scan Amiibo…" action:@selector(scanAmiibo:) keyEquivalent:@"a"];
    scan.target = self; scan.keyEquivalentModifierMask = NSEventModifierFlagCommand | NSEventModifierFlagShift;
    const char *name = dm_amiibo_name();
    NSMenuItem *rm = [menu addItemWithTitle:name ? [NSString stringWithFormat:@"Remove %@", @(name)] : @"No amiibo on the reader" action:name ? @selector(removeAmiibo:) : nil keyEquivalent:@""];
    rm.target = self; rm.enabled = name != NULL;
}
- (void)scanAmiibo:(NSMenuItem *)it {
    (void)it;
    NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Choose an amiibo dump (.bin)"; panel.allowsMultipleSelection = NO; panel.canChooseDirectories = NO;
    [panel beginWithCompletionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !panel.URL) return;
        if (!dm_amiibo_scan(panel.URL.fileSystemRepresentation)) {
            NSAlert *a = [NSAlert new]; a.messageText = @"Not an amiibo dump";
            a.informativeText = @"The file is not a raw amiibo image (a .bin of 520 to 572 bytes).";
            [a runModal];
        }
    }];
}
- (void)removeAmiibo:(NSMenuItem *)it { (void)it; dm_amiibo_remove(); }
@end
static DmAmiiboMenuDelegate *g_amiibo_menu;
// requested off the main thread: it takes the audio renderers' locks
@interface DmResetTarget : NSObject @end
@implementation DmResetTarget
- (void)resetGame:(NSMenuItem *)it { (void)it; dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ dm_reset_request("Reset Game (⌘R)"); }); }
@end
static DmResetTarget *g_reset_target;
void front_run(void (*guest)(void)) {
    // latency-critical: macOS does not trade frame time for power while the frame rate is capped
    static id activity __attribute__((unused));
    activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"game"];
    if (!getenv("DM_WINDOW")) {
        kern_return_t kr;
        int role_status = front_windowless_role(getenv("DM_WINDOWLESS_ROLE"), &kr);
        if (role_status > 0) LOG("front end: applied the default application task role for windowless play");
        else if (role_status < 0) LOG("front end: could not apply the default application task role (Mach error %d)", kr);
        else LOG("front end: kept the existing task role (DM_WINDOWLESS_ROLE=0)");
        guest();
        return;
    }
    @autoreleasepool {
        [NSApplication sharedApplication];
        [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
        NSRect r = NSMakeRect(0, 0, 1280, 720);
        NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                    styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
                                                      backing:NSBackingStoreBuffered defer:NO];
        win.title = @(DM_GAME_NAME);
        win.backgroundColor = NSColor.blackColor;   // behind a non-opaque layer
        // without FullScreenPrimary the green button only zooms
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        NSView *v = win.contentView = [[DmGameView alloc] initWithFrame:r];
        v.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        const char *fs = getenv("DM_FULLSCREEN");   // any value but 0: start in fullscreen
        int want_fs = fs && strcmp(fs, "0");
        layer_opaque_sync(win.screen ?: NSScreen.mainScreen, want_fs);   // before the first drawable
        // developerHUDProperties stay unset: mode=default kept the HUD on after the menu's Hide HUD
        if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) LOG("front end: Metal performance HUD on (DM_METAL_HUD)");
        v.layer = g_layer;
        [(DmGameView *)v fitDrawable];
        NSScreen *home = nil;
        const char *ws = getenv("DM_WINDOW_SCREEN");
        if (ws && *ws) {
            NSArray<NSScreen *> *all = NSScreen.screens;
            if (!strcmp(ws, "builtin")) {
                for (NSScreen *sc in all) {
                    if (CGDisplayIsBuiltin([sc.deviceDescription[@"NSScreenNumber"] unsignedIntValue])) {
                        home = sc;
                        break;
                    }
                }
            } else if (atoi(ws) >= 0 && (NSUInteger)atoi(ws) < all.count) {
                home = all[atoi(ws)];
            }
            if (home) LOG("front end: window on screen %s (DM_WINDOW_SCREEN=%s)", home.localizedName.UTF8String, ws);
            else LOG("[RED] front end: no screen for DM_WINDOW_SCREEN=%s", ws);
        }
        if (home) {
            NSRect vf = home.visibleFrame;
            [win setFrameOrigin:NSMakePoint(NSMidX(vf) - win.frame.size.width / 2, NSMidY(vf) - win.frame.size.height / 2)];
        } else {
            [win center];
        }
        [win makeKeyAndOrderFront:nil];
        screen_refresh(win.screen ?: NSScreen.mainScreen);
        layer_opaque_sync(win.screen ?: NSScreen.mainScreen, want_fs);
        g_win = win;
        [NSApp activateIgnoringOtherApps:YES];
        // A host another process starts is not activated and can open behind other windows:
        // docs/reference/frame-rate.md#a-game-window-another-process-starts-opens-behind-other-windows
        if (getenv("DM_WINDOW_TOP") && atoi(getenv("DM_WINDOW_TOP"))) {
            win.level = NSFloatingWindowLevel;
            [win orderFrontRegardless];
            LOG("front end: window kept in front (DM_WINDOW_TOP)");
        }
        // Covered, presents follow the host's clock (nvn_display.inc link_sync). Read again a second after opening: a window that opens behind another and stays
        // there may send no change.
        void (^occl)(void) = ^{
            static int vis = -1;
            int v = (win.occlusionState & NSWindowOcclusionStateVisible) != 0;
            if (v == vis) return;
            vis = v;
            g_window_covered = !v;
            if (v) {
                dm_fstep_settle();
                LOG("front end: window visible");
            } else {
                LOG("front end: window covered (its display link fires about once a second: presents follow the host's clock until it is visible)");
            }
        };
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeOcclusionStateNotification object:win queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; occl(); }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, NSEC_PER_SEC), dispatch_get_main_queue(), occl);
        // AppKit runs ⌘ shortcuts only through menu items
        NSMenu *bar = [NSMenu new], *app = [NSMenu new], *wm = [[NSMenu alloc] initWithTitle:@"Window"];
        [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
        [app addItemWithTitle:[@"Quit " stringByAppendingString:@(DM_GAME_NAME)] action:@selector(terminate:) keyEquivalent:@"q"];
        g_reset_target = [DmResetTarget new];
        NSMenu *gm = [[NSMenu alloc] initWithTitle:@"Game"];
        NSMenuItem *rs = [gm addItemWithTitle:@"Reset Game" action:@selector(resetGame:) keyEquivalent:@"r"];
        rs.target = g_reset_target;
        [[bar addItemWithTitle:@"Game" action:nil keyEquivalent:@""] setSubmenu:gm];
        g_mods_menu = [DmModsMenuDelegate new];
        NSMenu *mm = [[NSMenu alloc] initWithTitle:@"Mods"];
        mm.delegate = g_mods_menu; mm.autoenablesItems = NO;
        [[bar addItemWithTitle:@"Mods" action:nil keyEquivalent:@""] setSubmenu:mm];
        if (dm_game_imports("_ZN2nn3nfp")) {
            g_amiibo_menu = [DmAmiiboMenuDelegate new];
            NSMenu *am = [[NSMenu alloc] initWithTitle:@"Amiibo"];
            am.delegate = g_amiibo_menu; am.autoenablesItems = NO;
            [g_amiibo_menu menuNeedsUpdate:am];   // the key equivalent must exist before the menu first opens
            [[bar addItemWithTitle:@"Amiibo" action:nil keyEquivalent:@""] setSubmenu:am];
        }
        [[bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""] setSubmenu:wm];
        [wm addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        NSApp.mainMenu = bar;
        if (want_fs) {
            g_fs_asked = CACurrentMediaTime();
            g_fs_starting = 1;
            // Opened from a fullscreen app, the game starts while macOS leaves that Space: ask for fullscreen after activation and the Space
            // change; only a refused startup request is retried, once.
            static DmWinDelegate *wd;
            wd = [DmWinDelegate new];
            win.delegate = wd;
            __block int want = 1, retried = 0, entering = 0;
            void (^enter)(void) = ^{
                if (!want || (win.styleMask & NSWindowStyleMaskFullScreen) || !NSApp.isActive || !win.isOnActiveSpace) return;
                want = 0;
                entering = 1;
                [win toggleFullScreen:nil];
            };
            g_fs_failed = ^{
                if (!entering) return;
                entering = 0;
                LOG("front end: fullscreen was refused (app active %d, window on the active Space %d)%s", NSApp.isActive, win.isOnActiveSpace, retried ? "" : ", asking once more");
                if (retried) {
                    g_fs_starting = 0;
                    layer_opaque_sync(win.screen, 0);
                    return;
                }
                retried = 1;
                want = 1;
                dispatch_async(dispatch_get_main_queue(), enter);
            };
            [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; enter(); }];
            [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; enter(); }];
            for (NSNotificationName nm in @[NSWindowWillEnterFullScreenNotification, NSWindowWillExitFullScreenNotification])
                [NSNotificationCenter.defaultCenter addObserverForName:nm object:win queue:nil usingBlock:^(NSNotification *n) {
                    (void)n; want = 0;
                    if (!entering) g_fs_failed = nil; }];
            [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) {
                (void)n; want = 0; entering = 0; g_fs_failed = nil; }];
            dispatch_async(dispatch_get_main_queue(), enter);
        }
        for (NSNotificationName nm in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
            [NSNotificationCenter.defaultCenter addObserverForName:nm object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                GCController *c = n.object;
                int conn = n.name == GCControllerDidConnectNotification;
                if (!c.extendedGamepad || g_merge) LOG("controller %s: %s%s", conn ? "connected" : "disconnected", pad_name(c), c.extendedGamepad ? "" : " (no extended gamepad profile: not used)");
                if (conn) hidpad_gc_connected(c);
                pads_changed(conn ? nil : c);
                if (conn) hq_log_setting(); }];
        dm_haptics_apple_init();
        pads_start();
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // another window of the host's (an alert) gets its keys
            if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand)) return e;   // a key-up with ⌘ held still reaches the game, or the key would stay pressed
            if (e.type == NSEventTypeKeyDown && fn_shortcut(e)) return e;   // fn+letter is the system's
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // swallowed, or the system beeps
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; exit(0); }];
        // focus lost → 2, regained → 1; minimized → 3 with audio paused and the GPU suspended
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidChangeScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            screen_refresh(win.screen);
            if (win.screen) g_layer.contentsScale = win.backingScaleFactor;
            layer_opaque_sync(win.screen, win_fullscreen(win) || g_fs_starting);
            screen_match_apply();
            dm_fstep_settle();
        }];
        [nc addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            screen_refresh(win.screen);
            layer_opaque_sync(win.screen, win_fullscreen(win) || g_fs_starting);   // mirroring turned on or off
        }];
        // the layer follows the view; only the scale factor follows the screen
        [nc addObserverForName:NSWindowWillEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; layer_opaque_sync(win.screen, 1); dm_fstep_settle(); }];
        [nc addObserverForName:NSWindowWillExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_fstep_settle(); }];
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            screen_refresh(win.screen);
            g_layer.contentsScale = win.backingScaleFactor;
            layer_opaque_sync(win.screen, 1);
            screen_match_apply();
            g_fullscreen = 1;
            g_fs_starting = 0;
            dm_fstep_settle();
        }];
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) {
            (void)n;
            g_fullscreen = 0;
            screen_refresh(win.screen);
            g_layer.contentsScale = win.backingScaleFactor;
            layer_opaque_sync(win.screen, 0);
            screen_match_apply();
            dm_fstep_settle();
        }];
        // DM_FOCUS_KEEP=1: the game keeps focus whatever app is active (docs/reference/frame-rate.md#a-regress-run-stops-when-another-app-takes-the-key-window)
        static int keep = -1;
        if (keep < 0) keep = getenv("DM_FOCUS_KEEP") && atoi(getenv("DM_FOCUS_KEEP"));
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); if (!win.miniaturized && !keep) dm_focus_set(2); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_resume(); dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        display_awake(win.isKeyWindow);   // the window may have become key before these observers
        static guest_arg ga;
        ga.fn = guest;
        pthread_attr_t a;
        pthread_attr_init(&a);
        pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th;
        if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// AppKit views, so it draws while the game's thread waits
static NSView *g_prog;
static NSTextField *g_prog_title, *g_prog_count;
static NSProgressIndicator *g_prog_bar;
static NSTextField *prog_label(NSRect r, CGFloat size, NSFontWeight w, NSColor *c) {
    NSTextField *t = [NSTextField labelWithString:@""];
    t.frame = r;
    t.alignment = NSTextAlignmentCenter;
    t.textColor = c;
    t.font = [NSFont monospacedDigitSystemFontOfSize:size weight:w];
    return t;
}
void (*front_progress_hook)(const char *title, long done, long total);
void front_progress(const char *title, long done, long total) {
    if (!g_layer) return;
    NSString *tt = title ? @(title) : nil;
    void (*hook)(const char *, long, long) = front_progress_hook;
    if (hook) {
        dispatch_async(dispatch_get_main_queue(), ^{ hook(tt.UTF8String, done, total); });
        return;
    }
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!tt) {
            [g_prog removeFromSuperview];
            g_prog = nil;
            return;
        }
        if (!g_prog) {
            NSView *cv = g_win.contentView;
            g_prog = [[NSView alloc] initWithFrame:cv.bounds]; g_prog.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
            g_prog.wantsLayer = YES; g_prog.layer.backgroundColor = [NSColor colorWithWhite:0.06 alpha:0.94].CGColor;
            NSView *box = [[NSView alloc] initWithFrame:NSMakeRect(floor((cv.bounds.size.width - 400) / 2), floor((cv.bounds.size.height - 96) / 2), 400, 96)];
            box.autoresizingMask = NSViewMinXMargin | NSViewMaxXMargin | NSViewMinYMargin | NSViewMaxYMargin;
            g_prog_title = prog_label(NSMakeRect(0, 62, 400, 30), 20, NSFontWeightSemibold, NSColor.whiteColor);
            g_prog_bar = [[NSProgressIndicator alloc] initWithFrame:NSMakeRect(20, 36, 360, 20)];
            g_prog_bar.style = NSProgressIndicatorStyleBar; g_prog_bar.indeterminate = NO; g_prog_bar.minValue = 0;
            g_prog_count = prog_label(NSMakeRect(0, 4, 400, 22), 13, NSFontWeightRegular, [NSColor colorWithWhite:0.72 alpha:1]);
            [box addSubview:g_prog_title];
            [box addSubview:g_prog_bar];
            [box addSubview:g_prog_count];
            [g_prog addSubview:box];
            [cv addSubview:g_prog];
        }
        g_prog_title.stringValue = tt;
        g_prog_bar.maxValue = total > 0 ? (double)total : 1;
        g_prog_bar.doubleValue = (double)done;
        g_prog_count.stringValue = [NSString stringWithFormat:@"%@ of %@", [NSNumberFormatter localizedStringFromNumber:@(done) numberStyle:NSNumberFormatterDecimalStyle],
                                    [NSNumberFormatter localizedStringFromNumber:@(total) numberStyle:NSNumberFormatterDecimalStyle]];
    });
}

// GPU lost: leave fullscreen first (a frozen fullscreen game held the whole screen), then alert and exit(70) so a launcher sees a failure
void front_gpu_lost(void) {
    if (!g_layer) die("GPU access revoked: macOS runs none of this process's GPU work any more");
    LOG("front end: the GPU no longer runs the game's work; telling the player and quitting");
    dispatch_async(dispatch_get_main_queue(), ^{
        __block int told = 0;
        void (^tell)(void) = ^{
            if (told++) return;
            NSAlert *a = [NSAlert new];
            a.alertStyle = NSAlertStyleCritical;
            a.messageText = [@"The GPU stopped running " stringByAppendingString:@(DM_GAME_NAME)];   // %s would not read UTF-8
            a.informativeText = @"After a GPU error, macOS stopped running the game's GPU work, so nothing more can be drawn and the game quits. The log has the details.";
            [a addButtonWithTitle:@"Quit"];
            // in the background the alert would sit behind other windows
            [NSApp activateIgnoringOtherApps:YES];
            [a layout];
            NSWindow *game = g_win;
            if (game && a.window.level <= game.level) a.window.level = game.level + 1;
            [a runModal];
            exit(70);
        };
        NSWindow *win = g_win;
        if (!(win.styleMask & NSWindowStyleMaskFullScreen)) {
            tell();
            return;
        }
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; tell(); }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), tell);
        [win toggleFullScreen:nil];
    });
}
