// Host front end (front_mac.m).
#pragma once
#include <stdlib.h>
// Metal reads these at its first device; explicit environment values win.
static inline void front_hud_env(void) {
    const char *e = getenv("DM_METAL_HUD");
    if (!e || !atoi(e)) return;
    setenv("MTL_HUD_ENABLED", "1", 0);
    // MetalFX spatial has no temporal exposure: this HUD metric would flicker.
    setenv("MTL_HUD_HIDE_METALFX_METRICS", "1", 0);
}
// With a window the guest runs on its own thread (the main thread is the UI's).
void front_run(void (*guest)(void));
void *front_metal_layer(void);
// Hz, 0 = unknown. Any thread.
int front_screen_max_fps(void);
// Any thread.
int front_screen_variable_refresh(void);
// Fullscreen: switches to a same-size mode whose rate is a multiple of fps. Present thread.
void front_screen_match(int fps);
// Any thread.
int front_screen_settling(void);
// No part of the window is on screen (other windows over it, another Space). Any thread.
int front_window_covered(void);
// title NULL hides it. Any thread; does not block.
void front_progress(const char *title, long done, long total);
// Set before start: gets front_progress's values on the main thread in place of the built-in screen.
extern void (*front_progress_hook)(const char *title, long done, long total);
#include <stdint.h>
#include <stddef.h>
// Blocks until closed: 1 done, 0 closed (B / Escape), -1 no window.
int front_ctrl_support(int min, int max);

// A save write failed (err = first errno), so that transaction's Commit is refused. Game thread: do not block.
void front_save_failed(int err);

// MTL4CommandQueueErrorAccessRevoked: no GPU work runs any more. Metal's feedback queue: do not block.
void front_gpu_lost(void);

