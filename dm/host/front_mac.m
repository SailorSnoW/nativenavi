// Mac front end: with DM_WINDOW=1 open a window (CAMetalLayer, for the graphics backend to present frames) + keyboard and controllers → dm_input; the guest runs on a large-stack thread instead and the main thread belongs to AppKit.
// Without DM_WINDOW: run the guest on the current thread with the default application task role (front_role.h).
// The window is fullscreen-capable (the green button and ⌃⌘F); DM_FULLSCREEN=1 starts it that way. ⌘Q quits, and so does a controller's Home button held for 2 s and
// confirmed with A (home_quit.h).
// Key map (Nintendo layout): WASD left stick, arrow keys D-pad (with Shift held: the right stick, for the camera), J=A K=B U=X I=Y, Q=L E=R, Z=ZL C=ZR, F=press left stick G=press right stick, Return=+ Tab=−, Space=shake the controller.
// Controllers: face buttons by position (right A, bottom B, top X, left Y) or on a Nintendo pad by its labels (pad_apple.h), shoulders L R, triggers ZL ZR, Menu + Options −, both sticks and their presses.
// Each controller is a player of its own (local co-op): the first one connected is player 1, together with the keyboard, the next player 2, and so on up to 8 (pad_slots.h);
// DM_PAD_MERGE=1 merges every controller into player 1 instead, DM_KBD_PLAYER=own makes the keyboard player 1 on its own, with the controllers from player 2 on.
#import <Cocoa/Cocoa.h>
#import <QuartzCore/CAMetalLayer.h>
#import <Metal/Metal.h>
#import <GameController/GameController.h>
#include "dm.h"
#include "front_role.h"
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
#include "pad_switch2_apple.h"
static int s2pad_player(s2_pad *d);   // the Switch 2 pads' player slots (below, beside the raw HID pads')
static void s2pad_read(s2_pad *d);
static void (*g_merge_sink)(const dm_haptic_state *);   // DM_PAD_MERGE: where player 1's rumble goes, the pads last used (merge_rumble_to): NULL for GameController's
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
static int g_rl, g_ru, g_rr, g_rd;   // arrow keys pressed with Shift held: the right stick (each goes back up as it went down, whatever Shift does meanwhile)
static int g_input_on = 1;   // main thread: the game window is key, so its keyboard, mouse and controllers reach the game (input_release / input_resume)

void *front_metal_layer(void) { return (__bridge void *)g_layer; }
static _Atomic int g_screen_fps;   // the window's screen's maximumFramesPerSecond, kept up to date when the window moves to another screen
int front_screen_max_fps(void) { return g_screen_fps; }
static _Atomic int g_screen_variable_refresh;
int front_screen_variable_refresh(void) { return g_screen_variable_refresh; }
static CGDirectDisplayID g_match_id; static CGDisplayModeRef g_match_orig; static int g_match_orig_vrr;   // main thread: the screen this host switched, its own mode (NULL = none switched) and whether that mode is variable-refresh
static int32_t g_match_pick; static int g_match_pick_vrr;   // main thread: the IO mode ID this host last switched to (valid while g_match_orig != NULL) and whether it is variable-refresh
static void screen_refresh(NSScreen *screen) {   // main thread: refresh capabilities of the current screen and mode, including a mode change without moving the window
    if (!screen) return;
    g_screen_fps = (int)screen.maximumFramesPerSecond;
    g_screen_variable_refresh = screen.minimumRefreshInterval > 0 && screen.maximumRefreshInterval > screen.minimumRefreshInterval;
    // A screen this host switched (screen_match_apply): NSScreen can lag and keep the old variable-refresh range or rate after the switch, which pushed the display link to
    // frame latency 2 on a fixed-rate mode (DELL S2725QC, Ocarina of Time). While the screen is in the mode this host picked, keep that mode's rate and VRR flag
    if (g_match_orig && [screen.deviceDescription[@"NSScreenNumber"] unsignedIntValue] == g_match_id) {
        CGDisplayModeRef cur = CGDisplayCopyDisplayMode(g_match_id);
        if (cur && CGDisplayModeGetIODisplayModeID(cur) == g_match_pick) {
            g_screen_variable_refresh = g_match_pick_vrr;
            double hz = CGDisplayModeGetRefreshRate(cur); if (hz > 0) g_screen_fps = (int)round(hz);
        }
        if (cur) CGDisplayModeRelease(cur);
    }
}
static _Atomic int g_fullscreen;   // the window is in fullscreen (set when it has entered and when it has left)
int front_fullscreen(void) { return g_fullscreen; }   // nvn_mtl.m link_latency: frame latency 2 in variable-refresh fullscreen
// Whether the screen is still settling at start, so the display link waits before starting (nvn_mtl.m link_sync): fullscreen was asked for at launch (DM_FULLSCREEN) and has not been
// entered yet (at most 5 s), a fullscreen match request is still queued on the main thread, or this host switched a screen's mode under 1 s ago. Started before, the link ran into
// the mode switch fullscreen brings (silences up to 500 ms, a rescue, and a minute at 20-30 fps on a ROG XG27AQNGV). The present thread shows frames meanwhile
static _Atomic int g_fs_starting, g_match_pending; static _Atomic double g_fs_asked, g_mode_switched;
int front_screen_settling(void) {
    double now = CACurrentMediaTime();
    if (g_fs_starting && !g_fullscreen && now - g_fs_asked < 5.0) return 1;
    if (g_match_pending > 0 && g_fullscreen) return 1;
    return g_mode_switched > 0 && now - g_mode_switched < 1.0;
}
// The screen's refresh rate matched to the game's frame rate in fullscreen (front_screen_match, from PR #79; DM_SCREEN_MATCH=0 turns it off). 60 fps on a 144 Hz screen cannot be paced
// evenly: the display link counts a tick on the refresh nearest each 1/60 s slot (nvn_mtl.m link_range), so frames stay up 2 and 3 refreshes in turn (13.9 / 20.8 ms) and Apple's HUD shows
// the frame interval jumping between them. In fullscreen, a screen whose rate is not a multiple of the game's switches to the mode of the same size with the highest rate that is (144 → 120 Hz
// for 60 and 30 fps), for this app only: macOS switches it back when the host quits, crashes included, and leaving fullscreen or that screen switches it back at once. A window keeps the
// screen's rate, because the switch changes the whole screen for every app and most external screens go black for a second or more. A screen with no such mode keeps the virtual ticks.
// A variable-refresh screen (ProMotion) in fullscreen switches to a fixed-rate mode of the same size at a multiple of the game's rate (120 → 60 Hz for 60 and 30 fps), though its own
// rate is a multiple: there the display link at 60 Hz missed its targets (every present a 120 Hz refresh late and every other tick skipped, 40 fps on an M5 Pro at frame latency 2;
// 1 → 2 → 3 vsync steps at latency 1, issue #347), and the fixed 60 Hz mode held 60.0 fps at latency 1 (docs/reference/frame-rate.md). A screen with no such mode keeps its rate, and
// the 120 Hz link paces it (nvn_mtl.m link_vrr_120)
static NSWindow *g_win; static _Atomic int g_match_want;   // the game's window; the frame rate to match (0 = not known yet), written by the present thread only
// A switch reconfigures every screen, and with several screens (a DisplayLink one among them) that moved the fullscreen window off the switched screen and back: each move
// switched the screen back and then again, 18 times in a minute, until two of three screens lost their signal and the Dock and Finder stopped answering (LNPANDY/NX2Apple#1085).
// A switch is undone when the screens take it back, not the player or the game: the fullscreen window left the switched screen, or the screen is at its own mode again
// MATCH_SETTLE_S after the switch with no switch back from this host. A switch due less than MATCH_SETTLE_S after an undone one is a bounce, and at MATCH_BOUNCES bounces in
// a row the screen keeps its rate. Leaving fullscreen and a new frame rate start over: a switch back made for them is not an undone one. A switch that has not shown on the
// screen yet is not made again until MATCH_SETTLE_S elapses. Main thread
#define MATCH_SETTLE_S 5.0
#define MATCH_BOUNCES 2
static double g_match_switched, g_match_undone;   // when this host last switched a screen, when the screens last undid a switch (CACurrentMediaTime, 0 = never)
static int g_match_bounces, g_match_bounced;   // switches in a row due soon after an undone one; the rate is left alone until fullscreen is left or the frame rate changes
static int g_match_recheck;   // a delayed settle check is already scheduled on the main queue
static double mode_hz(CGDisplayModeRef m) { double hz = CGDisplayModeGetRefreshRate(m); return hz > 0 ? hz : 0; }   // 0 = not known (older built-in screens report none)
static int hz_multiple(double hz, int fps) { double k = round(hz / fps); return k >= 1 && fabs(hz / fps - k) < 0.01; }   // 59.94 Hz counts as 60
static CGError screen_set_mode(CGDirectDisplayID id, CGDisplayModeRef m) {   // for this app only (kCGConfigureForAppOnly)
    CGDisplayConfigRef cfg; CGError r = CGBeginDisplayConfiguration(&cfg);
    if (r != kCGErrorSuccess) return r;
    r = CGConfigureDisplayWithDisplayMode(cfg, id, m, NULL);
    if (r != kCGErrorSuccess) { CGCancelDisplayConfiguration(cfg); return r; }
    r = CGCompleteDisplayConfiguration(cfg, kCGConfigureForAppOnly);
    if (r == kCGErrorSuccess) g_mode_switched = CACurrentMediaTime();   // front_screen_settling
    return r;
}
static void screen_match_restore(int on_it, int undone) {   // main thread: the screen this host switched gets its own mode again; on_it: the window is on that screen; undone: the
    // fullscreen window left that screen (the screens took the switch back), not the player leaving fullscreen
    double hz = mode_hz(g_match_orig); CGError r = screen_set_mode(g_match_id, g_match_orig);
    LOG("front end: screen %s %.0f Hz", r == kCGErrorSuccess ? "back at" : "could not be switched back to", hz);
    if (r == kCGErrorSuccess && on_it) { if (hz > 0) g_screen_fps = (int)round(hz); g_screen_variable_refresh = g_match_orig_vrr; }   // the screen observer may not show the mode yet
    CGDisplayModeRelease(g_match_orig); g_match_orig = NULL; g_match_pick = 0; g_match_pick_vrr = 0;
    if (undone) g_match_undone = CACurrentMediaTime();
}
static void screen_match_apply(void);
static void screen_match_once(void) {   // screen_match_apply's work, never entered twice at once
    static int off = -1; if (off < 0) { const char *e = getenv("DM_SCREEN_MATCH"); off = e && !strcmp(e, "0"); if (off) LOG("front end: the screen keeps its refresh rate in fullscreen too (DM_SCREEN_MATCH=0)"); }
    NSScreen *scr = g_win.screen; int want = g_match_want, full = (g_win.styleMask & NSWindowStyleMaskFullScreen) != 0;
    CGDirectDisplayID id = scr ? [scr.deviceDescription[@"NSScreenNumber"] unsignedIntValue] : kCGNullDirectDisplay;
    if (g_match_orig && (!full || id != g_match_id)) screen_match_restore(id == g_match_id, full);
    static int last_want;
    if (!full || want != last_want) {   // left fullscreen, or a new frame rate: the switches before do not count
        if (g_match_bounced && full) LOG("front end: the screen's rate is matched again for %d fps", want);
        last_want = want; g_match_undone = 0; g_match_bounces = 0; g_match_bounced = 0;
    }
    if (off || g_match_bounced || !full || want <= 0 || !scr) return;
    CGDisplayModeRef cur = CGDisplayCopyDisplayMode(id);
    if (!cur) return;
    // Whether the screen is variable-refresh, read now when this host has not switched it: a player's own fixed mode (chosen in System Settings after leaving fullscreen)
    // must not be misread from a leftover variable-refresh flag
    if (!g_match_orig) screen_refresh(scr);
    int32_t cur_id = CGDisplayModeGetIODisplayModeID(cur);
    if (g_match_orig && cur_id == CGDisplayModeGetIODisplayModeID(g_match_orig)) {
        double since = CACurrentMediaTime() - g_match_switched;
        if (since < MATCH_SETTLE_S) {   // this host's switch has not shown yet: look again once it should have, in case no notification comes
            CGDisplayModeRelease(cur);
            if (!g_match_recheck) {
                g_match_recheck = 1;
                dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)((MATCH_SETTLE_S - since) * NSEC_PER_SEC) + 10000000LL), dispatch_get_main_queue(), ^{ g_match_recheck = 0; screen_match_apply(); });
            }
            return;
        }
        // the screen is at its own mode again with no switch back from this host (macOS or the screen took the switch back): its rate as that mode has it, and the switch is undone
        double own = mode_hz(cur);
        LOG("front end: screen at its own %.0f Hz again %.1f s after this host switched it", own, since);
        if (own > 0) g_screen_fps = (int)round(own);
        g_screen_variable_refresh = g_match_orig_vrr;
        CGDisplayModeRelease(g_match_orig); g_match_orig = NULL; g_match_pick = 0; g_match_pick_vrr = 0;
        g_match_undone = CACurrentMediaTime();
    }
    double hz = mode_hz(cur);
    // the current mode is variable-refresh: the screen's own mode as screen_refresh saw it; one this host switched to as it chose it (the screen observer may lag), so a
    // fixed mode is never left for the variable one at a rate it already matches
    int vrr = !g_match_orig ? g_screen_variable_refresh : cur_id == g_match_pick ? g_match_pick_vrr : cur_id == CGDisplayModeGetIODisplayModeID(g_match_orig) ? g_match_orig_vrr : 0;
    if (!vrr && (hz <= 0 || hz_multiple(hz, want))) {   // already a multiple, or not known
        if (g_match_orig) { if (hz > 0) g_screen_fps = (int)round(hz); g_screen_variable_refresh = 0; }   // the screen this host switched: keep its fixed-mode state even if the screen observer lags
        CGDisplayModeRelease(cur); return;
    }
    size_t w = CGDisplayModeGetWidth(cur), h = CGDisplayModeGetHeight(cur), pw = CGDisplayModeGetPixelWidth(cur), ph = CGDisplayModeGetPixelHeight(cur);
    CFArrayRef modes = CGDisplayCopyAllDisplayModes(id, (__bridge CFDictionaryRef)@{(__bridge NSString *)kCGDisplayShowDuplicateLowResolutionModes: @YES});
    CGDisplayModeRef pick = NULL; double pick_hz = 0;
    for (CFIndex i = 0; modes && i < CFArrayGetCount(modes); i++) {
        CGDisplayModeRef m = (CGDisplayModeRef)CFArrayGetValueAtIndex(modes, i); double r = mode_hz(m);
        if (CGDisplayModeGetWidth(m) != w || CGDisplayModeGetHeight(m) != h || CGDisplayModeGetPixelWidth(m) != pw || CGDisplayModeGetPixelHeight(m) != ph || !CGDisplayModeIsUsableForDesktopGUI(m)) continue;
        // on a variable-refresh screen only a fixed-rate mode: not the current one, nor one at its rate (the variable mode at the screen's top rate)
        if (vrr && (CGDisplayModeGetIODisplayModeID(m) == cur_id || (hz > 0 && fabs(r - hz) < 0.5))) continue;
        if (hz_multiple(r, want) && r > pick_hz) { pick = m; pick_hz = r; }
    }
    if (!pick && vrr && (hz <= 0 || hz_multiple(hz, want))) LOG("front end: variable-refresh screen at %.0f Hz has no fixed-rate mode of this size at a multiple of %d Hz: it keeps its rate", hz, want);
    else if (!pick) LOG("front end: screen at %.0f Hz has no mode of this size at a multiple of %d Hz: frames are paced on the nearest refreshes", hz, want);
    else if ((g_match_bounces = g_match_undone > 0 && CACurrentMediaTime() - g_match_undone < MATCH_SETTLE_S ? g_match_bounces + 1 : 0) >= MATCH_BOUNCES) {
        g_match_bounced = 1;
        LOG("front end: screen at %.0f Hz keeps its rate until fullscreen is left or the frame rate changes: the screens took the switch back %d times in a row, and switching it again moves the screens about (DM_SCREEN_MATCH=0 keeps it from the start)", hz, g_match_bounces);
    } else {
        CGError r = screen_set_mode(id, pick);
        LOG("front end: %sscreen at %.0f Hz %s %s%.0f Hz for %d fps in fullscreen (DM_SCREEN_MATCH=0 keeps it)", vrr ? "variable-refresh " : "", hz, r == kCGErrorSuccess ? "switched to" : "could not be switched to", vrr ? "fixed " : "", pick_hz, want);
        if (r == kCGErrorSuccess) {
            g_match_switched = CACurrentMediaTime();
            if (!g_match_orig) { g_match_orig = CGDisplayModeRetain(cur); g_match_id = id; g_match_orig_vrr = vrr; }
            g_match_pick = CGDisplayModeGetIODisplayModeID(pick);
            g_match_pick_vrr = g_match_pick == CGDisplayModeGetIODisplayModeID(g_match_orig) ? g_match_orig_vrr : 0;
            g_screen_fps = (int)round(pick_hz);   // the screen observer may not show the new mode yet: its rate, and whether it is variable (back to the screen's own mode: as that was)
            g_screen_variable_refresh = g_match_pick_vrr;
        }
    }
    if (modes) CFRelease(modes);
    CGDisplayModeRelease(cur);
}
// main thread: on a new frame rate, on entering and leaving fullscreen and on a move to another screen. CGCompleteDisplayConfiguration runs the main run loop until the screens
// are configured, and the window's screen notification can come inside it (before g_match_orig is saved or released): a switch or switch back made inside another ran twice
// (LNPANDY/NX2Apple#1085) or oscillated between modes (PR #3157). A call inside one runs once more after it, from the main queue
static void screen_match_apply(void) {
    static int busy, again;
    if (busy) { again = 1; return; }
    busy = 1; screen_match_once(); busy = 0;
    if (again) {
        again = 0; g_match_pending++;
        dispatch_async(dispatch_get_main_queue(), ^{ screen_match_apply(); g_match_pending--; });
    }
}
void front_screen_match(int fps) {
    if (!g_layer || g_match_want == fps) return;   // no window (a windowless run's main thread runs the guest), or the rate as before
    g_match_want = fps; g_match_pending++;
    dispatch_async(dispatch_get_main_queue(), ^{ screen_match_apply(); g_match_pending--; });
}
// The pointer hides on game input while it is over the picture of the active game window (issue #675): AppKit shows it again when the mouse moves, input_release when
// the window loses focus. The title bar, other windows and other apps keep theirs
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
// fn held with a letter: the arrow and function keys carry the same modifier flag, so the key must be a letter for this to be a Globe shortcut
static int fn_shortcut(NSEvent *e) {
    NSString *s = e.charactersIgnoringModifiers;
    return (e.modifierFlags & NSEventModifierFlagFunction) && s.length == 1 && [[NSCharacterSet letterCharacterSet] characterIsMember:[s characterAtIndex:0]];
}
static ctrl_prompt g_cp; static int g_cp_confirm, g_cp_cancel;   // the controller applet's prompt (front_ctrl_support) and a key pressed in it since its last step
static int cp_poll(void);
static void on_key(NSEvent *e, int down) {
    if (!g_input_on) return;
    unsigned short c = e.keyCode;
    if (cp_blocks_input(g_cp.phase)) {   // the prompt has the keyboard: Return or J (A) is done, Escape or K (B) closes it, no key reaches the game
        if (down && g_cp.phase == CP_OPEN) { if (c == 53 || c == 40) g_cp_cancel = 1; else if (c == 36 || c == 38) g_cp_confirm = 1; cp_poll(); }
        return;
    }
    switch (c) { case 13: g_w = down; break; case 0: g_a = down; break; case 1: g_s = down; break; case 2: g_d = down; break; }
    // An arrow key pressed with Shift held is the right stick until it is released; without Shift, the D-pad. The keyboard had no right stick: a 3D game's camera
    // could be turned only with a controller (Donkey Kong Bananza)
    int *rs = c == 123 ? &g_rl : c == 126 ? &g_ru : c == 124 ? &g_rr : c == 125 ? &g_rd : NULL;
    if (rs && (down ? (e.modifierFlags & NSEventModifierFlagShift) != 0 : *rs)) { *rs = down; push(); return; }
    int b = key_bit(c);
    uint64_t m = b >= 0 ? 1ull << b : c == 49 ? DM_BTN_SHAKE : 0;   // Space: shaken while held
    g_btn = down ? (g_btn | m) : (g_btn & ~m);
    if (down && (m || c == 13 || c == 0 || c == 1 || c == 2)) cursor_hide_for_input();   // a key the game takes (WASD: the left stick)
    push();
}

// Physical controllers (GameController) go to dm_pad_set_player (pad_read); player 1's is ORed with the keyboard, a stick takes
// whichever is deflected more (hle_hid.c). DM_KEYS uses player 1's path, so player 1's controller touched during a script takes it over.
// Players: every controller with an extended gamepad profile holds a slot (pad_slots.h: player 1 first, a reconnected controller gets its player back), and hle_hid.c reports
// player n on its own npad while the slot is held (dm_pad_players). A one-player game that supports only No1 / Handheld still sees every controller as player 1 (hle_hid.c).
// With DM_PAD_MERGE=1 every controller is player 1, as before (the last one moved wins; when the last one goes away its buttons and sticks are released)
static pad_slots g_slots;
static GCController *g_held[DM_PLAYERS];   // the controller of each slot, kept alive while it holds it (the slot table keys on its pointer)
static int g_merge, g_kbd_own;
static int32_t pad_axis(float v) { return (int32_t)(fmaxf(-1.f, fminf(1.f, v)) * DM_STICK_MAX); }
// A controller's state for player p, GameController or raw HID: a pressed button or a stick past 15% hides the pointer (cursor_hide_for_input), a resting stick's drift does not
static void pad_set(int p, uint64_t m, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    const int32_t dead = DM_STICK_MAX * 15 / 100;
    if (m || abs(lx) > dead || abs(ly) > dead || abs(rx) > dead || abs(ry) > dead) cursor_hide_for_input();
    dm_pad_set_player(p, m, lx, ly, rx, ry);
}
static void hq_poll(void);
static home_quit g_hq;   // the Home button's hold and quit prompt (home_quit.h): while the prompt is open the controllers' input goes to it, not to the game
static int g_press_to_join;   // DM_PAD_PRESS_TO_JOIN=1, below pad_read
static int pad_join(const void *owner, const char *name);
static const char *pad_name(GCController *c);
static void pads_motion_changed(GCController *gone);
// GameController's controllers that the host reads from their HID reports instead, so they stay unread here: identical pads (pad_hid_mac.h) and original Joy-Cons (pad_jc1_mac.h)
static int pad_gc_unread(GCController *c) { return hidpad_gc_unread(c) || jc1_gc_unread(c); }
static void pad_read(GCExtendedGamepad *g) {
    if (!g_input_on) return;
    hq_poll(); if (hq_blocks_input(g_hq.phase)) return;
    int p = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)g.controller);
    if (p < 0 && (!g_press_to_join || pad_gc_unread(g.controller))) return;   // a controller read from its HID reports: GameController's controller of it stays unread
    uint64_t m = dm_pad_face(g);   // by position, a Nintendo pad by its labels
    #define K(btn, bit) if (btn.isPressed) m |= bit
    K(g.leftShoulder, DM_BTN_L); K(g.rightShoulder, DM_BTN_R);
    // Work around nonzero resting values (Xbox LT about 0.06 with isPressed true): digital Switch triggers actuate above 25% (PR #2504).
    if (g.leftTrigger.value > 0.25f) m |= DM_BTN_ZL;
    if (g.rightTrigger.value > 0.25f) m |= DM_BTN_ZR;
    K(g.buttonMenu, DM_BTN_PLUS); K(g.buttonOptions, DM_BTN_MINUS); K(g.leftThumbstickButton, DM_BTN_LS); K(g.rightThumbstickButton, DM_BTN_RS);   // nil on pads without them: not pressed
    K(g.dpad.left, DM_BTN_LEFT); K(g.dpad.up, DM_BTN_UP); K(g.dpad.right, DM_BTN_RIGHT); K(g.dpad.down, DM_BTN_DOWN);
    #undef K
    if (p < 0) {   // DM_PAD_PRESS_TO_JOIN=1: it joins at its first button
        GCController *c = g.controller; if (!m || (p = pad_join((__bridge const void *)c, pad_name(c))) < 0) return;
        g_held[p] = c; dm_motion_attach(c, p); pads_motion_changed(nil);
    }
    if (cp_poll()) return;   // the controller applet's prompt has the controllers (front_ctrl_support)
    if (m) merge_rumble_to(NULL);   // DM_PAD_MERGE: a button on this controller takes player 1's rumble back from a Switch 2 pad or Joy-Cons read from HID
    dm_haptics_apple_controller(p, g.controller);
    pad_set(p, m, pad_axis(g.leftThumbstick.xAxis.value), pad_axis(g.leftThumbstick.yAxis.value), pad_axis(g.rightThumbstick.xAxis.value), pad_axis(g.rightThumbstick.yAxis.value));
}
// Home and Capture: without this macOS takes them as system gestures (Home opens the Games app). Capture has no function in the host, Home quits when it is held (hq_poll).
// macOS 27 respects the claim when the Home button setting for apps lets the app decide (hq_log_setting names it in the log)
static void pad_keep_system_buttons(GCController *c) {
    for (NSString *b in @[GCInputButtonHome, GCInputButtonShare]) c.physicalInputProfile.buttons[b].preferredSystemGestureState = GCSystemGestureStateDisabled;
    c.extendedGamepad.buttonHome.pressedChangedHandler = ^(GCControllerButtonInput *b, float v, BOOL down) { (void)b; (void)v; (void)down; if (g_input_on) hq_poll(); };
}
static const char *pad_name(GCController *c) { return c.vendorName.UTF8String ?: "?"; }
static void npad_name(int p, char *out, size_t cap) { uint32_t id = dm_player_npad(p); if (id == 0x20) snprintf(out, cap, "handheld"); else snprintf(out, cap, "%u", id); }
// The players' raw HID motion source follows the controllers (gone: one just removed, still listed): a Pro Controller's or a DualShock 4's, each model counted on its own
// (motion_pro_mac.h)
static void pads_motion_changed(GCController *gone) {
    int count[PRO_MODELS] = {0}, player[PRO_MODELS];
    for (int m = 0; m < PRO_MODELS; m++) player[m] = -1;
    for (GCController *c in GCController.controllers) {
        int m = c != gone && c.extendedGamepad ? pro_model_of_category(c.productCategory.UTF8String) : -1;
        if (m >= 0) { count[m]++; player[m] = g_merge ? 0 : pad_slot_of(&g_slots, (__bridge const void *)c); }
    }
    dm_motion_pro_players(count, player);
}
static void pro_devices_changed(void) { pads_motion_changed(nil); }
// DM_PAD_PRESS_TO_JOIN=1: a controller is no player until one of its buttons is pressed, then it is the lowest player free, as a console's "press a button" screen.
// Whatever the port, the order of connection or the link (Bluetooth has no port), and for identical pads too: so the players pick their own order. Not with DM_PAD_MERGE
// A raw HID pad joins this way without the setting too (hidpad_first_press), and then takes its slot as a connecting controller does (pad_slot_take)
static int pad_join(const void *owner, const char *name) {
    int p = g_press_to_join ? pad_slot_take_lowest(&g_slots, owner, name) : pad_slot_take(&g_slots, owner, name); char nb[16];
    if (p < 0) { LOG("controller '%s': a button pressed, but all %d players are taken", name, DM_PLAYERS); return -1; }
    npad_name(p, nb, sizeof nb); LOG("controller '%s': a button pressed: player %d (npad %s)", name, p + 1, nb);
    dm_pad_players(pad_slot_mask(&g_slots));
    return p;
}
// DM_PAD_MERGE: whether a controller still plays player 1 (gone: one just removed, not counted), a GameController one or a raw HID pad (pad_hid_mac.h)
static int merge_pads_left(GCController *gone) {
    if (g_hidpads || g_jc1_pads || dm_switch2_pads()) return 1;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) return 1;
    return 0;
}
// Called at start and on every connect / disconnect (gone: the controller that was just removed, nil on a connect)
static void pads_changed(GCController *gone) {
    if (g_merge) {
        int any = 0;
        for (GCController *c in GCController.controllers) if (jc1_gc_unread(c)) pad_keep_system_buttons(c);   // read from HID (pad_jc1_mac.h), a lone Joy-Con's too (no extended gamepad profile)
        for (GCController *c in GCController.controllers) if (c.extendedGamepad && c != gone) {
            if (jc1_gc_unread(c)) { c.extendedGamepad.valueChangedHandler = nil; continue; }
            any = 1; pad_keep_system_buttons(c); c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
            dm_motion_attach(c, 0);   // every controller is player 1: the last one moved drives its six-axis sensor
            pad_read(c.extendedGamepad);
        }
        if (gone) dm_motion_detach(gone, 0);
        if (!any) { if (!g_hidpads && !g_jc1_pads && !dm_switch2_pads()) dm_pad_set(0, 0, 0, 0, 0); dm_haptics_apple_controller(0, nil); }   // a raw HID pad still plays player 1 (pad_hid_mac.h)
        pads_motion_changed(gone);
        return;
    }
    hidpad_twins_eval(gone); hidpad_twins_release();   // identical pads (pad_hid_mac.h): those GameController lists them all of again go back to it first
    NSArray<GCController *> *now = GCController.controllers; char nb[16];
    for (GCController *c in now) if (c != gone && jc1_gc_unread(c)) pad_keep_system_buttons(c);   // read from HID (pad_jc1_mac.h), a lone Joy-Con's too (no extended gamepad profile)
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p] && (g_held[p] == gone || ![now containsObject:g_held[p]] || pad_gc_unread(g_held[p]))) {
        npad_name(p, nb, sizeof nb); LOG("controller '%s' disconnected: player %d (npad %s) is free", pad_name(g_held[p]), p + 1, nb);
        g_held[p].extendedGamepad.valueChangedHandler = nil;
        dm_motion_detach(g_held[p], p);
        dm_haptics_apple_controller(p, nil);
        pad_slot_free(&g_slots, (__bridge const void *)g_held[p]); g_held[p] = nil;
        dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
    }
    for (GCController *c in now) if (c.extendedGamepad && c != gone && pad_slot_of(&g_slots, (__bridge const void *)c) < 0) {
        if (pad_gc_unread(c)) { pad_keep_system_buttons(c); c.extendedGamepad.valueChangedHandler = nil; continue; }   // read from IOKit; Home and Capture stay out of macOS's hands
        if (g_press_to_join) {   // a player at its first button (pad_read)
            pad_keep_system_buttons(c);
            if (!c.extendedGamepad.valueChangedHandler) LOG("controller '%s' connected: press a button to play (DM_PAD_PRESS_TO_JOIN)", pad_name(c));
            c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
            continue;
        }
        int p = pad_slot_take(&g_slots, (__bridge const void *)c, pad_name(c));
        if (p < 0) { LOG("controller '%s' connected: all %d players are taken, not used", pad_name(c), DM_PLAYERS); continue; }
        g_held[p] = c; pad_keep_system_buttons(c);
        c.extendedGamepad.valueChangedHandler = ^(GCExtendedGamepad *g, GCControllerElement *el) { (void)el; pad_read(g); };
        dm_motion_attach(c, p);
        npad_name(p, nb, sizeof nb); LOG("controller '%s' connected: player %d (npad %s)%s", pad_name(c), p + 1, nb, c.motion.hasRotationRate ? ", with motion (gyro)" : "");
        pad_read(c.extendedGamepad);
    }
    hidpad_twins_take(gone);   // the pads of a model GameController lists fewer of, a player each
    dm_pad_players(pad_slot_mask(&g_slots));
    pads_motion_changed(gone);
    cp_poll();   // a controller came or went while the controller applet's prompt is up
}
// Display sleep: macOS counts keyboard and mouse input as user activity, not a controller's, so in controller-only play the display dims and locks; the latency
// activity in front_run keeps the system awake, not the display. While the game window is key the display stays awake (NSActivityIdleDisplaySleepDisabled);
// losing focus or being minimized lets it sleep again. An explicit lock or sleep is not held off, and no input is synthesized
static id g_display_activity;
static void display_awake(int on) {
    if (on && !g_display_activity)
        g_display_activity = [NSProcessInfo.processInfo beginActivityWithOptions:NSActivityIdleDisplaySleepDisabled reason:@"NativeNavi active game window"];
    else if (!on && g_display_activity) { [NSProcessInfo.processInfo endActivity:g_display_activity]; g_display_activity = nil; }
}
// Focus: a window that is no longer key sees no key-up or controller release, so whatever is held when it loses focus (another app, minimized) would
// stay pressed in the game. The front end releases only what it pressed itself: scripted buttons and sticks have their own path (hle_hid.c).
// Out of focus a controller's changes are ignored, so back in focus each controller is read as it is now (a button still held, a stick still deflected)
// Nothing the controllers pressed stays pressed (focus lost, the Home quit prompt open)
static void pads_release(void) {
    if (g_merge) { if (merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0); return; }
    for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p]) dm_pad_set_player(p, 0, 0, 0, 0, 0);
    for (hid_pad *d = g_hidpads; d; d = d->next) { int p = pad_slot_of(&g_slots, d); if (p >= 0) dm_pad_set_player(p, 0, 0, 0, 0, 0); }
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) { int p = s2pad_player(d); if (p >= 0) { dm_pad_set_player(p, 0, 0, 0, 0, 0); dm_joy_set(p, 0, 0, 0, 0); dm_joy_set(p, 1, 0, 0, 0); } }
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) { int p = jc1pad_player(d); if (p >= 0) { dm_pad_set_player(p, 0, 0, 0, 0, 0); dm_joy_set(p, 0, 0, 0, 0); dm_joy_set(p, 1, 0, 0, 0); } }
}
static void hidpad_motion_clear(void) {   // the gyro of identical Pro Controllers read from IOKit (pad_hid_mac.h)
    for (hid_pad *d = g_hidpads; d; d = d->next) { int p = hidpad_full(d) ? pad_slot_of(&g_slots, d) : -1; if (p >= 0) dm_motion_clear_hid(p); }
}
// Each controller read as it is now (focus back, the Home quit prompt closed)
static void pads_reread(void) {
    if (g_merge) { for (GCController *c in GCController.controllers) if (c.extendedGamepad) pad_read(c.extendedGamepad); }
    else for (int p = 0; p < DM_PLAYERS; p++) if (g_held[p].extendedGamepad) pad_read(g_held[p].extendedGamepad);
    for (hid_pad *d = g_hidpads; d; d = d->next) hidpad_read(d);
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) s2pad_read(d);
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) jc1pad_read(d);
}
static void hq_close(void);
static void input_release(void) {
    hq_close();   // the prompt or the hold indicator, if one is up: the Games app the system opens for Home takes the focus too
    [NSCursor setHiddenUntilMouseMoves:NO];
    display_awake(0);
    g_input_on = 0;
    dm_motion_pro_active(0); hidpad_motion_clear();
    dm_switch2_active(0); dm_jc1_active(0);
    g_btn = 0; g_w = g_a = g_s = g_d = 0; g_rl = g_ru = g_rr = g_rd = 0; push();
    pads_release();
}
static void input_resume(void) {
    display_awake(1);
    g_input_on = 1;
    dm_motion_pro_active(1);
    dm_switch2_active(1); dm_jc1_active(1);
    pads_reread();
}
// Raw HID gamepads (pad_hid_mac.h): a player slot each, as a GameController controller, named by the device's product name. Identical pads that GameController lists
// fewer of are read the same way, an identical Pro Controller from its report 0x30.
// A HID device that declares a GamePad or Joystick collection only claims to be a controller: a keyboard and mouse receiver can declare one and never send a report through it
// (a Keychron Link, 3434:d030, GitHub issue #1657), and nothing in its descriptor tells it from a pad at rest. So a raw HID pad is a player from its first button press, not
// from its connection, with or without DM_PAD_PRESS_TO_JOIN: a device that never presses takes no player from a real controller. An identical pad (twin) is a model
// GameController lists, so it is a player at its connection unless the setting is on
static int hidpad_first_press(const hid_pad *d) { return !g_merge && (g_press_to_join || !d->twin); }
static int hidpad_player(const hid_pad *d) { return g_merge ? 0 : pad_slot_of(&g_slots, d); }
// A pad of Nintendo's protocol (pad_hid_mac.h) shows its player on its lights, and its player's rumble goes to it once it answers
static void hidpad_show_player(hid_pad *d, int p, uint64_t m) {
    hidpad_lights(d, p);
    if (!hidpad_rumbles(d)) return;
    if (g_merge) { if (m) merge_rumble_to(dm_hidpad_rumble); return; }
    if (d->sink != p + 1) { d->sink = p + 1; dm_haptics_apple_set_sink(p, dm_hidpad_rumble); }   // its rumble goes over HID, not to a Core Haptics engine
}
static void hidpad_read(hid_pad *d) {
    if (!g_input_on || hq_blocks_input(g_hq.phase)) return;
    int p = hidpad_player(d), full = hidpad_full(d);
    uint64_t m = full ? d->pro_m : pad_hid_buttons(&d->st);
    if (p < 0 && (!hidpad_first_press(d) || !m || (p = pad_join(d, d->name)) < 0)) return;   // joins at its first button
    hidpad_show_player(d, p, m);
    if (cp_poll()) return;   // the controller applet's prompt has the controllers
    if (full) { pad_set(p, m, d->pro_s[0], d->pro_s[1], d->pro_s[2], d->pro_s[3]); return; }
    int32_t s[4]; pad_hid_sticks(&d->st, s);
    pad_set(p, m, s[0], s[1], s[2], s[3]);
}
static void hidpad_attach(hid_pad *d) {
    if (hidpad_first_press(d)) { LOG("controller '%s' (HID) connected: press a button to play%s", d->name, g_press_to_join ? " (DM_PAD_PRESS_TO_JOIN)" : ""); hidpad_read(d); return; }
    int p = g_merge ? 0 : pad_slot_take(&g_slots, d, d->name); char nb[16];
    if (p < 0) { LOG("controller '%s' (HID) connected: all %d players are taken, not used", d->name, DM_PLAYERS); return; }
    npad_name(p, nb, sizeof nb); LOG("controller '%s' (HID) connected: player %d (npad %s)", d->name, p + 1, nb);
    if (!g_merge) dm_pad_players(pad_slot_mask(&g_slots));   // merged, every controller is player 1, as pads_changed has it
    hidpad_read(d);
}
static void hidpad_motion(hid_pad *d, const float acc[3], const float rot[3], uint64_t ns) {
    int p = g_input_on && !g_merge ? pad_slot_of(&g_slots, d) : -1; if (p < 0) return;
    dm_motion_set_hid(p, acc, rot, ns);
}
static void hidpad_models_changed(void) { pads_changed(nil); }
static void hidpad_detach(hid_pad *d) {   // d is already off g_hidpads
    if (g_merge) {   // as pads_changed: released when the last controller goes away
        LOG("controller '%s' (HID) disconnected", d->name);
        int rumbles = 0; for (hid_pad *o = g_hidpads; o; o = o->next) rumbles |= hidpad_rumbles(o);
        if (!rumbles && g_merge_sink == dm_hidpad_rumble) merge_rumble_to(NULL);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d); char nb[16]; if (p < 0) return;
    if (hidpad_full(d)) dm_motion_clear_hid(p);
    if (d->sink) { d->sink = 0; dm_haptics_apple_set_sink(p, NULL); }
    npad_name(p, nb, sizeof nb); LOG("controller '%s' (HID) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);   // nothing stays pressed
    dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();   // one controller fewer to wait for, or one player fewer
}
// Switch 2 controllers over Bluetooth LE (pad_switch2_apple.h): a pad is a player's controller, as a GameController one or a raw HID pad
static int s2pad_player(s2_pad *d) { return g_merge ? 0 : pad_slot_of(&g_slots, d); }
// DM_PAD_MERGE: player 1's rumble follows the controller last used, as pad_read hands it to a GameController one
static void merge_rumble_to(void (*sink)(const dm_haptic_state *)) {
    if (!g_merge || g_merge_sink == sink) return;
    g_merge_sink = sink; dm_haptics_apple_set_sink(0, sink);
}
static int s2pad_blocked(void) { return !g_input_on || hq_blocks_input(g_hq.phase) || cp_blocks_input(g_cp.phase); }   // no input to the game (focus lost, the Home or controller prompt up): its Joy-Con states are not sent (pad_switch2_apple.m)
static void s2pad_read(s2_pad *d) {
    hq_poll();   // Home and the prompt's A and B come from these pads too
    if (!g_input_on || hq_blocks_input(g_hq.phase)) return;
    int p = s2pad_player(d);
    uint64_t m; int32_t s[4]; if (!s2_pad_input(&d->st, &m, s)) return;
    if (p < 0) {   // DM_PAD_PRESS_TO_JOIN=1: joins at its first button
        if (!g_press_to_join || !m || (p = pad_join(d, d->name)) < 0) return;
        dm_haptics_apple_set_sink(p, dm_switch2_rumble);   // its rumble goes over Bluetooth, not to a Core Haptics engine
    }
    if (cp_poll()) return;   // the controller applet's prompt has the controllers
    if (m) merge_rumble_to(dm_switch2_rumble);
    pad_set(p, m, s[0], s[1], s[2], s[3]);
}
static int s2pad_attach(s2_pad *d) {   // 1: kept (a player, or waiting for its first button press); 0: no free player (pad_switch2_apple.h)
    if (g_press_to_join && !g_merge) { LOG("controller '%s' (Bluetooth) connected: press a button to play (DM_PAD_PRESS_TO_JOIN)", d->name); cp_poll(); return 1; }
    int p = g_merge ? 0 : pad_slot_take(&g_slots, d, d->name); char nb[16];
    if (p < 0) { LOG("controller '%s' (Bluetooth) connected: all %d players are taken, not used", d->name, DM_PLAYERS); return 0; }
    npad_name(p, nb, sizeof nb); LOG("controller '%s' (Bluetooth) connected: player %d (npad %s)", d->name, p + 1, nb);
    if (!g_merge) { dm_pad_players(pad_slot_mask(&g_slots)); dm_haptics_apple_set_sink(p, dm_switch2_rumble); }   // its rumble goes over Bluetooth, not to a Core Haptics engine
    cp_poll();   // one controller fewer to wait for
    return 1;
}
static void s2pad_detach(s2_pad *d) {   // d is already off g_s2pads
    if (g_merge) {
        LOG("controller '%s' (Bluetooth) disconnected", d->name);
        if (!dm_switch2_pads() && g_merge_sink == dm_switch2_rumble) merge_rumble_to(NULL);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d); char nb[16]; if (p < 0) { cp_poll(); return; }   // one that had not joined: one fewer waiting
    dm_haptics_apple_set_sink(p, NULL);
    npad_name(p, nb, sizeof nb); LOG("controller '%s' (Bluetooth) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);
    dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();   // one player fewer
}
// Original Joy-Cons read from their HID reports (pad_jc1_mac.h): a pad is one Joy-Con or a left and a right one, a player from its first button press as a raw HID pad
// (hidpad_first_press: with or without DM_PAD_PRESS_TO_JOIN), whose rumble goes to the Joy-Cons themselves
static int jc1pad_player(jc1_pad *d) { return g_merge ? 0 : pad_slot_of(&g_slots, d); }
static int jc1pad_blocked(void) { return s2pad_blocked(); }
static void jc1pad_read(jc1_pad *d) {
    hq_poll();   // Home and the prompt's A and B come from these pads too
    if (!g_input_on || hq_blocks_input(g_hq.phase)) return;
    int p = jc1pad_player(d);
    uint64_t m; int32_t s[4]; if (!jc1_pad_state(d, &m, s)) return;
    if (p < 0) {   // joins at its first button
        if (!m || (p = pad_join(d, d->name)) < 0) return;
        dm_haptics_apple_set_sink(p, dm_jc1_rumble);
    }
    if (cp_poll()) return;   // the controller applet's prompt has the controllers
    if (m) merge_rumble_to(dm_jc1_rumble);
    pad_set(p, m, s[0], s[1], s[2], s[3]);
}
static void jc1pad_attach(jc1_pad *d) {
    if (g_merge) LOG("controller '%s' (HID) connected: player 1 (DM_PAD_MERGE)", d->name);
    else { LOG("controller '%s' (HID) connected: press a button to play", d->name); cp_poll(); }   // one more controller waiting to join
    jc1pad_read(d);
}
static void jc1pad_detach(jc1_pad *d) {   // d is already off g_jc1_pads
    if (g_merge) {
        LOG("controller '%s' (HID) disconnected", d->name);
        if (!g_jc1_pads && g_merge_sink == dm_jc1_rumble) merge_rumble_to(NULL);
        if (!merge_pads_left(nil)) dm_pad_set(0, 0, 0, 0, 0);
        return;
    }
    int p = pad_slot_free(&g_slots, d); char nb[16]; if (p < 0) { cp_poll(); return; }   // one that had not joined: one fewer waiting
    dm_haptics_apple_set_sink(p, NULL);
    npad_name(p, nb, sizeof nb); LOG("controller '%s' (HID) disconnected: player %d (npad %s) is free", d->name, p + 1, nb);
    dm_pad_set_player(p, 0, 0, 0, 0, 0);
    dm_pad_players(pad_slot_mask(&g_slots));
    cp_poll();   // one player fewer
}
static void jc1_models_changed(void) { pads_changed(nil); }
// Quit with the Home button (home_quit.h, LNPANDY/NX2Apple#336): held for 2 s it opens the prompt (home_quit_mac.h) over the game's view, A quits through the same path as ⌘Q
// (NSApp terminate, so the save writer and the other exit handlers run as there), B or Home closes it. While the prompt is open the game is told it lost the focus, as the
// Switch's HOME menu does, and gets no controller input; the keyboard and ⌘Q work as always. DM_HOME_QUIT=0 turns it off
static int g_hq_on;
static NSTimer *g_hq_timer;   // while Home is held: the time runs on without a button change
static NSView *g_hq_hold, *g_hq_prompt; static CAShapeLayer *g_hq_ring;
static char g_hq_text[HQ_TEXTS][256];
static void hq_start(void) {
    g_hq_on = hq_enabled(getenv("DM_HOME_QUIT"));
    char title[256]; snprintf(title, sizeof title, "Quit %s?", DM_GAME_NAME);
    const char *def[HQ_TEXTS] = {"Keep holding to quit", title, "Unsaved progress will be lost.", "Quit", "Cancel"};
    const char *text = getenv("DM_HOME_QUIT_TEXT");   // a front end's, in the player's language: one line per text, in home_quit.h's order
    for (int i = 0; i < HQ_TEXTS; i++) hq_text_line(text, i, def[i], g_hq_text[i], sizeof g_hq_text[i]);
    LOG("front end: hold the Home button to quit %s", g_hq_on ? "is on" : "is off (DM_HOME_QUIT=0)");
}
// The Home button setting for apps (macOS 27), once, when the first controller is there (GameController reads it only while one is connected): "system" in the log
// explains a hold that does nothing, since the system keeps Home whatever the host asks. A front end reads the setting itself (GCControllerHomeButtonSettingsManager)
static void hq_log_setting(void) {
#if defined(__MAC_27_0)
    if (@available(macOS 27.0, *)) {
        static int done; if (done) return;
        int any = 0; for (GCController *c in GCController.controllers) if (c.extendedGamepad) any = 1;
        if (!any) return;
        done = 1;
        GCControllerHomeButtonSettingCustomizationStatus by = GCControllerHomeButtonSettingCustomizationDefault; NSError *err = nil;
        GCControllerHomeButtonSettingInAppAction v = [[GCControllerHomeButtonSettingsManager new] readControllerHomeButtonInAppAction:&by withError:&err];
        LOG("front end: Home button in apps: %s (%s)%s%s", hq_in_app_name((long)v), by == GCControllerHomeButtonSettingCustomizationUser ? "set by the player" : "the system's default",
            err ? ", " : "", err ? err.localizedDescription.UTF8String : "");
    }
#endif
}
static void hq_buttons(int *home, int *a, int *b, GCController **holder) {
    *home = *a = *b = 0;
    for (GCController *c in GCController.controllers) {
        GCExtendedGamepad *g = c.extendedGamepad; if (!g) continue;
        if (g.buttonHome.isPressed) { *home = 1; if (!*holder) *holder = c; }
        *a |= g.buttonA.isPressed; *b |= g.buttonB.isPressed;
    }
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) { if (s2_pad_home(&d->st)) *home = 1; s2_pad_ab(&d->st, a, b); }   // Switch 2 controllers: no GCController, so no holder (the prompt shows the default A / B symbols)
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) { if (jc1_pad_home(d)) *home = 1; jc1_pad_ab(d, a, b); }   // original Joy-Cons read from HID (pad_jc1_mac.h)
}
static void hq_fade(NSView *v, int in) {
    if (!v) return;
    if (in) v.alphaValue = 0;
    [NSAnimationContext runAnimationGroup:^(NSAnimationContext *ctx) { ctx.duration = 0.18; v.animator.alphaValue = in ? 1 : 0; }
                        completionHandler:^{ if (!in) [v removeFromSuperview]; }];
}
static void hq_hide_hold(void) { [g_hq_timer invalidate]; g_hq_timer = nil; [g_hq_hold removeFromSuperview]; g_hq_hold = nil; g_hq_ring = nil; }
static void hq_hide_prompt(void) { hq_fade(g_hq_prompt, 0); g_hq_prompt = nil; }
static void hq_changed(hq_phase was, hq_phase now, GCController *holder) {
    if (was == HQ_HOLDING) hq_hide_hold();
    NSView *cv = g_win.contentView;
    switch (now) {
    case HQ_HOLDING:
        g_hq_timer = [NSTimer scheduledTimerWithTimeInterval:1.0 / 60 repeats:YES block:^(NSTimer *t) { (void)t; hq_poll(); }];
        break;
    case HQ_PROMPT: {
        pads_release();   // nothing the game held stays pressed while the prompt has the controllers
        dm_focus_set(2);
        GCExtendedGamepad *g = holder.extendedGamepad;
        g_hq_prompt = hq_prompt_view(cv.bounds, @(g_hq_text[HQ_TEXT_TITLE]), @(g_hq_text[HQ_TEXT_NOTE]), @(g_hq_text[HQ_TEXT_QUIT]), @(g_hq_text[HQ_TEXT_CANCEL]),
                                     g.buttonA.sfSymbolsName, g.buttonB.sfSymbolsName);
        [cv addSubview:g_hq_prompt]; hq_fade(g_hq_prompt, 1);
        LOG("front end: Home held, quit prompt open");
        break;
    }
    case HQ_SETTLE:
        hq_hide_prompt(); dm_focus_set(1);
        LOG("front end: quit prompt closed, the game goes on");
        break;
    case HQ_IDLE:
        if (was == HQ_SETTLE) pads_reread();   // the game reads the controllers again, as they are now
        break;
    case HQ_QUIT:
        LOG("front end: quit from the Home button prompt");
        [NSApp terminate:nil];   // as ⌘Q
        break;
    }
}
static void hq_poll(void) {
    static int busy; if (!g_hq_on || !g_win || busy) return;
    busy = 1;
    int home, a, b; GCController *holder = nil; hq_buttons(&home, &a, &b, &holder);
    double now = CACurrentMediaTime();
    hq_phase was = g_hq.phase, ph = hq_step(&g_hq, now, home, a, b);
    if (ph != was) hq_changed(was, ph, holder);
    double fill = hq_ring(&g_hq, now);
    if (fill >= 0) {
        if (!g_hq_hold) { NSView *cv = g_win.contentView; CAShapeLayer *ring = nil; g_hq_hold = hq_hold_view(cv.bounds, @(g_hq_text[HQ_TEXT_HOLD]), &ring); g_hq_ring = ring; [cv addSubview:g_hq_hold]; hq_fade(g_hq_hold, 1); }
        hq_hold_set(g_hq_ring, fill);
    }
    busy = 0;
}
static void hq_close(void) {
    if (g_hq.phase == HQ_IDLE || g_hq.phase == HQ_QUIT) return;
    if (g_hq.phase == HQ_PROMPT) LOG("front end: quit prompt closed, the window lost the focus");
    hq_cancel(&g_hq); hq_hide_hold(); hq_hide_prompt();
}
// The controller applet's prompt (front.h front_ctrl_support, ctrl_prompt.h; GitHub issue #1640): the game asked for more players than it sees, and the game's thread waits while
// the prompt is up over the game, as the Switch's player screen. A controller that joins meanwhile is a player at once (its first button with DM_PAD_PRESS_TO_JOIN, its
// connection without), and the count on the card follows. A and B are GameController's buttonA and buttonB on any controller, as the Home prompt's (a raw HID pad's A and B,
// pad_hid_mac.h); the press that makes a controller join does not count. Return or J is done, Escape or K closes it. The game gets no controller, keyboard or mouse input
// while it is up, nor until the A or B that closed it is let go. A front end passes the texts in the player's language as DM_CTRL_PROMPT_TEXT (cp_text)
static NSView *g_cp_view; static dispatch_semaphore_t g_cp_done; static int g_cp_ok, g_cp_shown_joined = -1, g_cp_shown_on = -1;
static void cp_buttons(int *a, int *b) {
    *a = *b = 0;
    for (GCController *c in GCController.controllers) { GCExtendedGamepad *g = c.extendedGamepad; if (g) { *a |= g.buttonA.isPressed; *b |= g.buttonB.isPressed; } }
    for (hid_pad *d = g_hidpads; d; d = d->next) { uint64_t m = d->twin && hidpad_is_pro(d) ? d->pro_m : pad_hid_buttons(&d->st); *a |= !!(m & DM_BTN_A); *b |= !!(m & DM_BTN_B); }
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) s2_pad_ab(&d->st, a, b);   // Switch 2 controllers (pad_switch2_apple.h)
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) jc1_pad_ab(d, a, b);             // original Joy-Cons read from HID (pad_jc1_mac.h)
}
static int cp_waiting(void) {   // connected controllers that are no player yet
    if (g_merge) return 0;
    int n = 0;
    for (GCController *c in GCController.controllers) if (c.extendedGamepad && !pad_gc_unread(c) && pad_slot_of(&g_slots, (__bridge const void *)c) < 0) n++;
    for (jc1_pad *d = g_jc1_pads; d; d = d->next) if (pad_slot_of(&g_slots, d) < 0) n++;
    for (hid_pad *d = g_hidpads; d; d = d->next) if (pad_slot_of(&g_slots, d) < 0) n++;
    for (s2_pad *d = dm_switch2_pads(); d; d = d->next) if (pad_slot_of(&g_slots, d) < 0) n++;
    return n;
}
static void cp_show(void) {   // the card, built again when its count or its done action changes
    int on = cp_can_confirm(&g_cp);
    if (g_cp_view && g_cp_shown_joined == g_cp.joined && g_cp_shown_on == on) return;
    const char *env = getenv("DM_CTRL_PROMPT_TEXT"); char t[CP_TEXTS][256];
    for (int i = 0; i < CP_TEXTS; i++) cp_text(env, i, &g_cp, t[i], sizeof t[i]);
    GCExtendedGamepad *g = nil; for (GCController *c in GCController.controllers) if ((g = c.extendedGamepad)) break;
    NSView *cv = g_win.contentView, *v = hq_card_view(cv.bounds, @(t[CP_TEXT_TITLE]), [NSString stringWithFormat:@"%s\n%s", t[CP_TEXT_HINT], t[CP_TEXT_COUNT]], @(t[CP_TEXT_DONE]),
                                                   @(t[CP_TEXT_CANCEL]), g.buttonA.sfSymbolsName, g.buttonB.sfSymbolsName, on);
    if (g_cp_view) [g_cp_view removeFromSuperview];
    [cv addSubview:v]; if (!g_cp_view) hq_fade(v, 1);
    g_cp_view = v; g_cp_shown_joined = g_cp.joined; g_cp_shown_on = on;
}
// One step of the prompt (a button, a key, a controller joining, coming or going); returns whether it keeps the game's input
static int cp_poll(void) {
    if (g_cp.phase == CP_IDLE) return 0;
    int a, b, joined = dm_players_reported(); cp_buttons(&a, &b);
    int confirm = g_cp_confirm, cancel = g_cp_cancel; g_cp_confirm = g_cp_cancel = 0;
    cp_phase was = g_cp.phase, ph = cp_step(&g_cp, joined, cp_waiting(), a, b, confirm, cancel);
    if (was == CP_OPEN && ph == CP_OPEN) cp_show();
    if (was == CP_OPEN && ph != CP_OPEN) {
        hq_fade(g_cp_view, 0); g_cp_view = nil;
        LOG("front end: controller prompt closed: %d players (%s)", joined, g_cp.ok ? "done" : "canceled");
        g_cp_ok = g_cp.ok; dispatch_semaphore_signal(g_cp_done);
        ph = cp_step(&g_cp, joined, 0, a, b, 0, 0);   // nothing held: the game has its input at once
    }
    if (ph == CP_IDLE) { pads_reread(); return 0; }   // each controller read as it is now
    return 1;
}
int front_ctrl_support(int min, int max) {
    if (!g_layer) return -1;
    dispatch_semaphore_t done = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_main_queue(), ^{
        g_btn = 0; g_w = g_a = g_s = g_d = 0; g_rl = g_ru = g_rr = g_rd = 0; push();   // nothing the game held stays pressed while the prompt has the input
        pads_release();
        int a, b; cp_buttons(&a, &b);
        g_cp_done = done; cp_open(&g_cp, min, max, dm_players_reported(), a, b);
        LOG("front end: controller prompt open: %d to %d players wanted, %d in, %d controllers waiting to join", min, max, g_cp.joined, cp_waiting());
        cp_show(); cp_poll();
    });
    dispatch_semaphore_wait(done, DISPATCH_TIME_FOREVER);
    return g_cp_ok;
}
static void pads_start(void) {
    const char *e = getenv("DM_PAD_MERGE"); g_merge = e && atoi(e);
    e = getenv("DM_KBD_PLAYER"); g_kbd_own = !g_merge && e && !strcmp(e, "own");
    e = getenv("DM_PAD_PRESS_TO_JOIN"); g_press_to_join = !g_merge && e && atoi(e);
    static const char kbd = 0;
    if (g_kbd_own) pad_slot_take(&g_slots, &kbd, "keyboard");   // holds player 1 for good
    LOG("front end: %s", g_merge ? "every controller is player 1 (DM_PAD_MERGE)" : g_kbd_own ? "the keyboard is player 1, each controller a player of its own from player 2 on (DM_KBD_PLAYER=own)"
                                                                           : "each controller is a player of its own, the keyboard plays with player 1");
    if (g_press_to_join) LOG("front end: a controller becomes a player at its first button press, the lowest player free (DM_PAD_PRESS_TO_JOIN)");
    hq_start();
    pads_changed(nil);
    hq_log_setting();
    dm_motion_pro_start(pro_devices_changed);
    dm_jc1_start(g_merge);
    hidpad_start();
    static const dm_switch2_front s2front = {s2pad_attach, s2pad_detach, s2pad_read, s2pad_player, s2pad_blocked};
    dm_switch2_start(&s2front);
}

// The window's content view, which holds the Metal layer
@interface DmGameView : NSView @end
@implementation DmGameView
// The drawable follows the view in pixels: a layer assigned to a layer-backed view kept the drawable at the first window's 1280x720 in fullscreen too, so the game picture was
// scaled down to it and up again by the compositor
- (void)fitDrawable {
    CAMetalLayer *l = (CAMetalLayer *)self.layer; if (![l isKindOfClass:CAMetalLayer.class] || !self.window) return;
    l.contentsScale = self.window.backingScaleFactor;
    NSSize px = [self convertSizeToBacking:self.bounds.size]; if (px.width < 1 || px.height < 1 || CGSizeEqualToSize(l.drawableSize, px)) return;
    l.drawableSize = px; dm_fstep_settle(); LOG("front end: drawable %.0fx%.0f", px.width, px.height);
}
- (void)setFrameSize:(NSSize)s { [super setFrameSize:s]; [self fitDrawable]; }
- (void)viewDidChangeBackingProperties { [super viewDidChangeBackingProperties]; [self fitDrawable]; }
@end

// AppKit reports a refused fullscreen request only to the window delegate (there is no notification for it)
static void (^g_fs_failed)(void);
@interface DmWinDelegate : NSObject <NSWindowDelegate> @end
@implementation DmWinDelegate
- (void)windowDidFailToEnterFullScreen:(NSWindow *)w { (void)w; if (g_fs_failed) g_fs_failed(); }
@end

typedef struct { void (*fn)(void); } guest_arg;
static void *guest_tramp(void *p) {
    static char alt[65536];                       // The signal alternate stack is per thread; crash printing must also work on the guest thread
    stack_t ss = {.ss_sp = alt, .ss_size = sizeof alt}; sigaltstack(&ss, 0);
    ((guest_arg *)p)->fn(); LOG("guest thread finished"); exit(0);
}

// Metal reads MTL_HUD_ENABLED when the process makes its first device, which can be before the window: set from DM_METAL_HUD before main (set in front_run, the HUD did not show)
__attribute__((constructor)) static void hud_env(void) { front_hud_env(); }

// The Mods menu (hle_mods.c), rebuilt on every open
@interface DmModsMenuDelegate : NSObject <NSMenuDelegate>
@end
@implementation DmModsMenuDelegate
- (void)menuNeedsUpdate:(NSMenu *)menu {
    [menu removeAllItems];
    int n = dm_mods_count();
    NSString *pack = nil;
    for (int i = 0; i < n; i++) {
        NSArray<NSString *> *parts = [@(dm_mods_name(i)) componentsSeparatedByString:@"/"];
        if (parts.count > 1 && ![parts[0] isEqualToString:pack]) [menu addItemWithTitle:parts[0] action:nil keyEquivalent:@""].enabled = NO;
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
    (void)it; NSString *dir = @(dm_mods_dir_path());
    [[NSFileManager defaultManager] createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil];
    [[NSWorkspace sharedWorkspace] openURL:[NSURL fileURLWithPath:dir isDirectory:YES]];
}
@end
static DmModsMenuDelegate *g_mods_menu;
// The Amiibo menu (hle_sdk16.c), rebuilt on every open
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
    (void)it; NSOpenPanel *panel = [NSOpenPanel openPanel];
    panel.message = @"Choose an amiibo dump (.bin)"; panel.allowsMultipleSelection = NO; panel.canChooseDirectories = NO;
    [panel beginWithCompletionHandler:^(NSModalResponse r) {
        if (r != NSModalResponseOK || !panel.URL) return;
        if (!dm_amiibo_scan(panel.URL.fileSystemRepresentation)) {
            NSAlert *a = [NSAlert new]; a.messageText = @"Not an amiibo dump";
            a.informativeText = @"The file is not a raw amiibo image (a .bin of 520 to 572 bytes)."; [a runModal];
        }
    }];
}
- (void)removeAmiibo:(NSMenuItem *)it { (void)it; dm_amiibo_remove(); }
@end
static DmAmiiboMenuDelegate *g_amiibo_menu;
// Game > Reset Game (⌘R): the game's reset, as Home > close the software > start it again (hle_reset.c), asked off the main thread (the request takes the audio renderers' locks)
@interface DmResetTarget : NSObject @end
@implementation DmResetTarget
- (void)resetGame:(NSMenuItem *)it { (void)it; dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ dm_reset_request("Reset Game (⌘R)"); }); }
@end
static DmResetTarget *g_reset_target;
void front_run(void (*guest)(void)) {
    // A game: tell macOS its work is latency-critical, so the system does not trade its frame time for power while the frame rate is capped
    static id activity; activity = [[NSProcessInfo processInfo] beginActivityWithOptions:NSActivityUserInitiated | NSActivityLatencyCritical reason:@"game"];
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
        win.title = @(DM_GAME_NAME);   // the game profile's name (game name label)
        // Fullscreen: without FullScreenPrimary the green button only zooms the window. The present path already scales
        // the game picture to fit the drawable with black bars around it, so fullscreen keeps the game's aspect ratio.
        win.collectionBehavior = NSWindowCollectionBehaviorFullScreenPrimary;
        NSView *v = win.contentView = [[DmGameView alloc] initWithFrame:r];
        v.wantsLayer = YES;
        g_layer = [CAMetalLayer layer];
        g_layer.device = MTLCreateSystemDefaultDevice();
        g_layer.pixelFormat = MTLPixelFormatBGRA8Unorm;
        g_layer.contentsScale = win.backingScaleFactor;
        // Apple's HUD over the game (frame rate, frame and GPU time, memory): front_hud_env turns it on before the first device. The layer's developerHUDProperties
        // stay unset: mode=default forces this layer's HUD on, so the Metal HUD menu's Hide HUD left it on screen
        if (getenv("DM_METAL_HUD") && atoi(getenv("DM_METAL_HUD"))) LOG("front end: Metal performance HUD on (DM_METAL_HUD)");
        v.layer = g_layer; [(DmGameView *)v fitDrawable];
        NSScreen *home = nil; const char *ws = getenv("DM_WINDOW_SCREEN");
        if (ws && *ws) {
            NSArray<NSScreen *> *all = NSScreen.screens;
            if (!strcmp(ws, "builtin")) { for (NSScreen *sc in all) if (CGDisplayIsBuiltin([sc.deviceDescription[@"NSScreenNumber"] unsignedIntValue])) { home = sc; break; } }
            else if (atoi(ws) >= 0 && (NSUInteger)atoi(ws) < all.count) home = all[atoi(ws)];
            if (home) LOG("front end: window on screen %s (DM_WINDOW_SCREEN=%s)", home.localizedName.UTF8String, ws); else LOG("[RED] front end: no screen for DM_WINDOW_SCREEN=%s", ws);
        }
        if (home) { NSRect vf = home.visibleFrame; [win setFrameOrigin:NSMakePoint(NSMidX(vf) - win.frame.size.width / 2, NSMidY(vf) - win.frame.size.height / 2)]; }
        else [win center];
        [win makeKeyAndOrderFront:nil];
        screen_refresh(win.screen ?: NSScreen.mainScreen);
        g_win = win;
        [NSApp activateIgnoringOtherApps:YES];
        // A host another process starts (scripts/regress.py) is not activated (its focus state stays 2), so its window opened wherever it landed in the window order, and behind
        // another app's window macOS fires its display link about once a second: Breath of the Wild stepped up to 3 vsyncs at boot and never came back (20 fps runs).
        // DM_WINDOW_TOP=1 orders it in front without activation and keeps it above other apps' windows; the log names every change of the window's visibility either way
        if (getenv("DM_WINDOW_TOP") && atoi(getenv("DM_WINDOW_TOP"))) { win.level = NSFloatingWindowLevel; [win orderFrontRegardless]; LOG("front end: window kept in front (DM_WINDOW_TOP)"); }
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowDidChangeOcclusionStateNotification object:win queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
            (void)n; static int vis = -1; int v = (win.occlusionState & NSWindowOcclusionStateVisible) != 0; if (v == vis) return; vis = v;
            if (v) { dm_fstep_settle(); LOG("front end: window visible"); } else LOG("front end: window covered (its display link fires about once a second: frame times measure nothing)"); }];
        // Main menu: AppKit runs ⌘ shortcuts only through menu items, so ⌘Q quits, ⌘R resets the game and ⌃⌘F toggles fullscreen from here (AppKit shows the item as Globe-F and keeps ⌃⌘F as a hidden twin)
        NSMenu *bar = [NSMenu new], *app = [NSMenu new], *wm = [[NSMenu alloc] initWithTitle:@"Window"];
        [[bar addItemWithTitle:@"" action:nil keyEquivalent:@""] setSubmenu:app];
        [app addItemWithTitle:[@"Quit " stringByAppendingString:@(DM_GAME_NAME)] action:@selector(terminate:) keyEquivalent:@"q"];
        g_reset_target = [DmResetTarget new];
        NSMenu *gm = [[NSMenu alloc] initWithTitle:@"Game"];
        NSMenuItem *rs = [gm addItemWithTitle:@"Reset Game" action:@selector(resetGame:) keyEquivalent:@"r"]; rs.target = g_reset_target;
        [[bar addItemWithTitle:@"Game" action:nil keyEquivalent:@""] setSubmenu:gm];
        g_mods_menu = [DmModsMenuDelegate new];
        NSMenu *mm = [[NSMenu alloc] initWithTitle:@"Mods"];
        mm.delegate = g_mods_menu; mm.autoenablesItems = NO;
        [[bar addItemWithTitle:@"Mods" action:nil keyEquivalent:@""] setSubmenu:mm];
        if (dm_game_imports("_ZN2nn3nfp")) {
            g_amiibo_menu = [DmAmiiboMenuDelegate new];
            NSMenu *am = [[NSMenu alloc] initWithTitle:@"Amiibo"];
            am.delegate = g_amiibo_menu; am.autoenablesItems = NO;
            [g_amiibo_menu menuNeedsUpdate:am];   // so that the key equivalent exists before the menu is first opened
            [[bar addItemWithTitle:@"Amiibo" action:nil keyEquivalent:@""] setSubmenu:am];
        }
        [[bar addItemWithTitle:@"Window" action:nil keyEquivalent:@""] setSubmenu:wm];
        [wm addItemWithTitle:@"Enter Full Screen" action:@selector(toggleFullScreen:) keyEquivalent:@"f"].keyEquivalentModifierMask = NSEventModifierFlagControl | NSEventModifierFlagCommand;
        NSApp.mainMenu = bar;
        const char *fs = getenv("DM_FULLSCREEN");   // 1 (or any value but 0): start in fullscreen; 0, the settings window's Window, or unset: a window
        if (fs && strcmp(fs, "0")) {
            g_fs_asked = CACurrentMediaTime(); g_fs_starting = 1;   // front_screen_settling: the display link waits for fullscreen
            // A game opened from a fullscreen app starts while macOS leaves that Space. Wait for activation and for the window to reach the active Space before asking for fullscreen
            // (whichever happens last), and repeat a refused startup request once. Only a request made here owns a retry; a later player request must never restart startup handling
            static DmWinDelegate *wd; wd = [DmWinDelegate new]; win.delegate = wd;
            __block int want = 1, retried = 0, entering = 0;
            void (^enter)(void) = ^{
                if (!want || (win.styleMask & NSWindowStyleMaskFullScreen) || !NSApp.isActive || !win.isOnActiveSpace) return;
                want = 0; entering = 1; [win toggleFullScreen:nil]; };
            g_fs_failed = ^{
                if (!entering) return;
                entering = 0;
                LOG("front end: fullscreen was refused (app active %d, window on the active Space %d)%s", NSApp.isActive, win.isOnActiveSpace, retried ? "" : ", asking once more");
                if (retried) { g_fs_starting = 0; return; }
                retried = 1; want = 1; dispatch_async(dispatch_get_main_queue(), enter); };
            [NSNotificationCenter.defaultCenter addObserverForName:NSApplicationDidBecomeActiveNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; enter(); }];
            [NSWorkspace.sharedWorkspace.notificationCenter addObserverForName:NSWorkspaceActiveSpaceDidChangeNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; enter(); }];
            for (NSNotificationName nm in @[NSWindowWillEnterFullScreenNotification, NSWindowWillExitFullScreenNotification])
                [NSNotificationCenter.defaultCenter addObserverForName:nm object:win queue:nil usingBlock:^(NSNotification *n) {
                    (void)n; want = 0;
                    if (!entering) g_fs_failed = nil; }];   // the player took over while startup was waiting; our own transition has entering set
            [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) {
                (void)n; want = 0; entering = 0; g_fs_failed = nil; }];   // startup succeeded: later manual refusals have no startup request to retry
            dispatch_async(dispatch_get_main_queue(), enter);
        }
        for (NSNotificationName nm in @[GCControllerDidConnectNotification, GCControllerDidDisconnectNotification])
            [NSNotificationCenter.defaultCenter addObserverForName:nm object:nil queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) {
                GCController *c = n.object; int conn = n.name == GCControllerDidConnectNotification;
                if (!c.extendedGamepad || g_merge) LOG("controller %s: %s%s", conn ? "connected" : "disconnected", pad_name(c), c.extendedGamepad ? "" : " (no extended gamepad profile: not used)");
                if (conn) hidpad_gc_connected(c);   // a HID pad of its name that only waited for it (pad_hid_mac.h)
                pads_changed(conn ? nil : c);
                if (conn) hq_log_setting(); }];
        dm_haptics_apple_init();
        pads_start();
        [NSEvent addLocalMonitorForEventsMatchingMask:NSEventMaskKeyDown | NSEventMaskKeyUp handler:^NSEvent *(NSEvent *e) {
            if (e.window != win) return e;   // Only swallow keys for the game window; another window of the host's (an alert) receives its keys as usual
            if (e.type == NSEventTypeKeyDown && (e.modifierFlags & NSEventModifierFlagCommand)) return e;   // ⌘ shortcuts go to the menu; a key released while ⌘ is held still reaches the game, or it would stay pressed
            if (e.type == NSEventTypeKeyDown && fn_shortcut(e)) return e;   // fn (Globe) + a letter is the system's (fn+F full screen, GitHub issue #873), not the game's F
            if (!e.isARepeat) on_key(e, e.type == NSEventTypeKeyDown);
            return nil;   // Swallow it to avoid the system beep
        }];
        [[NSNotificationCenter defaultCenter] addObserverForName:NSWindowWillCloseNotification object:win queue:nil
                                                      usingBlock:^(NSNotification *n) { (void)n; exit(0); }];
        // Lifecycle (hle.h / dm.h): focus lost → 2, focus regained → 1; minimized → 3 + audio paused + GPU suspended, restored → resume (followed by focus regained → 1).
        // Input is released when the window stops being key (and when it is minimized) and taken up again when it is key again
        NSNotificationCenter *nc = NSNotificationCenter.defaultCenter;
        [nc addObserverForName:NSWindowDidChangeScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; screen_refresh(win.screen); if (win.screen) g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); dm_fstep_settle(); }];
        [nc addObserverForName:NSApplicationDidChangeScreenParametersNotification object:nil queue:nil usingBlock:^(NSNotification *n) { (void)n; screen_refresh(win.screen); }];
        // Entering and leaving fullscreen: the layer is layer-backed, so its frame (and with it the drawable) follows the
        // content view by itself; only the scale factor has to be carried over to the screen the window ended up on. The screen's rate is matched in fullscreen only (screen_match_apply)
        for (NSNotificationName nm in @[NSWindowWillEnterFullScreenNotification, NSWindowWillExitFullScreenNotification])
            [nc addObserverForName:nm object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_fstep_settle(); }];
        [nc addObserverForName:NSWindowDidEnterFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; screen_refresh(win.screen); g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); g_fullscreen = 1; g_fs_starting = 0; dm_fstep_settle(); }];   // the switch first: front_screen_settling holds the link until it settles
        [nc addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; g_fullscreen = 0; screen_refresh(win.screen); g_layer.contentsScale = win.backingScaleFactor; screen_match_apply(); dm_fstep_settle(); }];
        // DM_FOCUS_KEEP=1 (scripts/regress.py): the game stays in focus while its window is up, whichever app is active. Tears of the Kingdom stops presenting out of focus, so
        // another app taking the key window during an unattended run read as a hang (no frame after the last focus change)
        static int keep = -1; if (keep < 0) keep = getenv("DM_FOCUS_KEEP") && atoi(getenv("DM_FOCUS_KEEP"));
        [nc addObserverForName:NSWindowDidResignKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); if (!win.miniaturized && !keep) dm_focus_set(2); }];
        [nc addObserverForName:NSWindowDidBecomeKeyNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_resume(); dm_focus_set(1); }];
        [nc addObserverForName:NSWindowDidMiniaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; input_release(); dm_focus_set(3); dm_audio_suspend(1); dm_gfx_suspend(1); }];
        [nc addObserverForName:NSWindowDidDeminiaturizeNotification object:win queue:nil usingBlock:^(NSNotification *n) { (void)n; dm_gfx_suspend(0); dm_audio_suspend(0); dm_focus_set(2); }];
        display_awake(win.isKeyWindow);   // the window may have become key (makeKeyAndOrderFront above) before these observers were installed
        // Guest thread: 64 MiB stack (the Switch main thread stack is specified by npdm; the host gives plenty)
        static guest_arg ga; ga.fn = guest;
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 64u << 20);
        pthread_t th; if (pthread_create(&th, &a, guest_tramp, &ga)) die("pthread_create guest");
        [NSApp run];
    }
}

// Progress screen of the prepare step (front.h front_progress): a dark view over the game's, the title, a bar and a count, centered; AppKit views, so it draws while the game's thread waits
static NSView *g_prog; static NSTextField *g_prog_title, *g_prog_count; static NSProgressIndicator *g_prog_bar;
static NSTextField *prog_label(NSRect r, CGFloat size, NSFontWeight w, NSColor *c) {
    NSTextField *t = [NSTextField labelWithString:@""]; t.frame = r; t.alignment = NSTextAlignmentCenter; t.textColor = c;
    t.font = [NSFont monospacedDigitSystemFontOfSize:size weight:w]; return t;
}
void (*front_progress_hook)(const char *title, long done, long total);
void front_progress(const char *title, long done, long total) {
    if (!g_layer) return;
    NSString *tt = title ? @(title) : nil;
    void (*hook)(const char *, long, long) = front_progress_hook;
    if (hook) { dispatch_async(dispatch_get_main_queue(), ^{ hook(tt.UTF8String, done, total); }); return; }   // the host app draws it (front.h)
    dispatch_async(dispatch_get_main_queue(), ^{
        if (!tt) { [g_prog removeFromSuperview]; g_prog = nil; return; }
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
            [box addSubview:g_prog_title]; [box addSubview:g_prog_bar]; [box addSubview:g_prog_count]; [g_prog addSubview:box];
            [cv addSubview:g_prog];
        }
        g_prog_title.stringValue = tt; g_prog_bar.maxValue = total > 0 ? (double)total : 1; g_prog_bar.doubleValue = (double)done;
        g_prog_count.stringValue = [NSString stringWithFormat:@"%@ of %@", [NSNumberFormatter localizedStringFromNumber:@(done) numberStyle:NSNumberFormatterDecimalStyle],
                                    [NSNumberFormatter localizedStringFromNumber:@(total) numberStyle:NSNumberFormatterDecimalStyle]];
    });
}

// GPU lost (front.h): macOS runs none of the game's GPU work any more and nothing is drawn again. In full screen the frozen game held the whole screen (#1326: WindowServer froze for 40 s
// and its watchdog killed it), so the window leaves full screen first and the alert comes once it has, or after 3 s if it has not. Then the app quits with exit status 70
// (EX_SOFTWARE), so a launcher or script that runs it sees a failed run: exit() runs the same atexit handlers as ⌘Q's terminate (the one will-terminate
// observer, haptics_apple.m's, is an atexit handler too), whose exit status is 0
void front_gpu_lost(void) {
    if (!g_layer) die("GPU access revoked: macOS runs none of this process's GPU work any more");
    LOG("front end: the GPU no longer runs the game's work; telling the player and quitting");
    dispatch_async(dispatch_get_main_queue(), ^{
        __block int told = 0;
        void (^tell)(void) = ^{
            if (told++) return;
            NSAlert *a = [NSAlert new];
            a.alertStyle = NSAlertStyleCritical;
            a.messageText = [@"The GPU stopped running " stringByAppendingString:@(DM_GAME_NAME)];   // @(): the name is UTF-8, which %s would not read as such
            a.informativeText = @"After a GPU error, macOS stopped running the game's GPU work, so nothing more can be drawn and the game quits. The log has the details.";
            [a addButtonWithTitle:@"Quit"];
            // An app opened in the background would leave the alert behind other apps' windows, and DM_WINDOW_TOP's floating game window would cover it
            [NSApp activateIgnoringOtherApps:YES];
            [a layout]; NSWindow *game = g_win; if (game && a.window.level <= game.level) a.window.level = game.level + 1;
            [a runModal];
            exit(70);
        };
        NSWindow *win = g_win;
        if (!(win.styleMask & NSWindowStyleMaskFullScreen)) { tell(); return; }
        [NSNotificationCenter.defaultCenter addObserverForName:NSWindowDidExitFullScreenNotification object:win queue:NSOperationQueue.mainQueue usingBlock:^(NSNotification *n) { (void)n; tell(); }];
        dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC), dispatch_get_main_queue(), tell);
        [win toggleFullScreen:nil];
    });
}
