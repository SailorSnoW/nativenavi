// Host front end (front_mac.m).
#pragma once
#include <stdlib.h>
// Metal reads HUD switches when the first device is made. Called before host preload and again after the launcher applies its settings; explicit environment values win.
static inline void front_hud_env(void) {
    const char *e = getenv("DM_METAL_HUD");
    if (!e || !atoi(e)) return;
    setenv("MTL_HUD_ENABLED", "1", 0);
    // Spatial scaling has no temporal exposure: showing that HUD metric makes the MetalFX rows flicker.
    setenv("MTL_HUD_HIDE_METALFX_METRICS", "1", 0);
}
// Run the guest: with a window the guest runs on its own thread and the main thread belongs to the system UI; without a window it runs directly on the current thread.
void front_run(void (*guest)(void));
// The CAMetalLayer the graphics backend presents frames to (NULL when there is no window)
void *front_metal_layer(void);
// The highest refresh rate of the screen the game is shown on in Hz, 0 = not known (front_mac.m: the window's screen; front_ios.m: the game view's screen, 60 in Low Power Mode). Any thread
int front_screen_max_fps(void);
// Whether the window's current screen mode supports variable refresh (Mac front_mac.m; none on iOS). Any thread
int front_screen_variable_refresh(void);
// The frame rate the display link paces the game at (60, 30, 120; nvn_mtl.m link_sync): in fullscreen, a screen whose refresh rate is not a multiple of it is switched, while the game runs,
// to a mode of the same size whose rate is, and back when the window leaves fullscreen or that screen (front_mac.m; none on iOS). Called from the present thread
void front_screen_match(int fps);
// The shape of the picture the game presents (nvn_mtl.m present_now, present_shape: the layer's, not the crop's, w x h), called when its width / height changes: the front end
// lays the picture out in that shape instead of 16:9. Before the first call the picture is 16:9. Called from the present thread; the front end switches to its main thread and does not block
void front_picture_shape(int w, int h);
// Progress screen of the first-start prepare step (nvn_mtl.m warm_prepare: "Preparing shaders" while the pipelines of the prewarm lists are built before the game's first frame):
// title and done of total shown over the game's view (a dark screen, the title, a bar and a count), updated on each call; title NULL hides it. Any thread; the front end switches to its main
// thread and does not block. Nothing is shown without a window
void front_progress(const char *title, long done, long total);
// An app that hosts the game view and has a look of its own (a launcher holding several games) can draw that progress itself: set before the game starts, with a window the hook gets each
// front_progress call's values on the front end's main thread (title NULL when the step ends; the title is valid during the call) and the front end shows no screen of its own.
// NULL (the default): the front end's screen above
extern void (*front_progress_hook)(const char *title, long done, long total);
#include <stdint.h>
#include <stddef.h>
// Controller applet prompt (hle_hid.c ctrl_support, ctrl_prompt.h; GitHub issue #1640): the game wants min to max players (max <= 0: no maximum) and sees fewer. The main thread shows a
// prompt over the game while controllers join and the calling thread blocks until it closes. 1: done (A or Return with the minimum in, the maximum in, or nobody left to join);
// 0: closed (B, Escape); -1 with no window: nothing shown, the caller answers as the player closing it
int front_ctrl_support(int min, int max);

// Save not written (called by the host's hle_fs.c; the iOS front end provides a strong implementation of the same name that shows the native prompt "Not enough storage. Progress was not saved"; the default weak implementation only logs).
// Timing: when a save write fails (disk full etc.) within one of the game's save transactions (between two Commits), so that this Commit is refused and the previously committed save is unchanged, it is called once per refused transaction;
// the next transaction that succeeds commits as usual, and the front end need not do any recovery. err = the first failing errno (ENOSPC = 28 means out of space). Called on the game thread; the front end switches to the main thread itself and must not block
void front_save_failed(int err);

// GPU lost (called by the graphics backend's nvn_mtl.m gpu_lost_check, once): the first command buffer came back MTL4CommandQueueErrorAccessRevoked, so macOS runs none of the process's GPU work
// any more and nothing is drawn again. By then presents have stopped and the game's waits for the GPU are released, so the game runs on with nothing on screen until the front end ends it.
// The Mac front end leaves full screen, says why in an alert and quits with exit status 70; without a window it ends the run as the default weak implementation (nvn_mtl.m) does, which the iOS
// front end keeps: a FATAL line and an abort. Called on Metal's feedback queue; the front end switches to the main thread itself and must not block
void front_gpu_lost(void);

