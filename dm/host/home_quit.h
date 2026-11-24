// Quit with the Home button: held HQ_HOLD_S it opens a "Quit?" prompt, A quits as ⌘Q, B or Home closes. A / B are GameController's
// buttonA / buttonB by label, not the game's Switch buttons. Pure, for tests.
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

#define HQ_HOLD_S 2.0
#define HQ_RING_S 0.3    // a short press shows nothing

typedef enum {
    HQ_IDLE,
    HQ_HOLDING,
    HQ_PROMPT,
    HQ_SETTLE,    // input resumes once Home, A and B are up: the closing B never reaches the game
    HQ_QUIT,      // final
} hq_phase;

typedef struct {
    hq_phase phase;
    double since;     // s, any monotonic clock
    int home;         // a press is a change from up to down
    int a_up, b_up;   // up since the prompt opened: a held button does not count
} home_quit;

// env: DM_HOME_QUIT's value, NULL when unset
static inline int hq_enabled(const char *env) { return !(env && !strcmp(env, "0")); }

// on every button change and, while Home is held, from a timer
static inline hq_phase hq_step(home_quit *q, double now, int home, int a, int b) {
    int pressed = home && !q->home;
    q->home = home;
    switch (q->phase) {
    case HQ_IDLE:
        if (pressed) { q->phase = HQ_HOLDING; q->since = now; }
        break;
    case HQ_HOLDING:
        if (!home) q->phase = HQ_IDLE;
        else if (now - q->since >= HQ_HOLD_S) { q->phase = HQ_PROMPT; q->a_up = !a; q->b_up = !b; }
        break;
    case HQ_PROMPT:
        if (!a) q->a_up = 1;
        if (!b) q->b_up = 1;
        if ((b && q->b_up) || pressed) q->phase = HQ_SETTLE;   // A and B together: cancel wins
        else if (a && q->a_up) q->phase = HQ_QUIT;
        break;
    case HQ_SETTLE:
        if (!home && !a && !b) q->phase = HQ_IDLE;
        break;
    case HQ_QUIT:
        break;
    }
    return q->phase;
}

// a Home still held needs a new press
static inline void hq_cancel(home_quit *q) {
    if (q->phase == HQ_QUIT) return;
    q->phase = HQ_IDLE;
    q->home = 1;
}

static inline int hq_blocks_input(hq_phase p) { return p == HQ_PROMPT || p == HQ_SETTLE || p == HQ_QUIT; }

// -1 while not shown
static inline double hq_ring(const home_quit *q, double now) {
    if (q->phase != HQ_HOLDING) return -1;
    double t = (now - q->since - HQ_RING_S) / (HQ_HOLD_S - HQ_RING_S);
    return t < 0 ? -1 : t > 1 ? 1 : t;
}

// one per line of DM_HOME_QUIT_TEXT
enum { HQ_TEXT_HOLD, HQ_TEXT_TITLE, HQ_TEXT_NOTE, HQ_TEXT_QUIT, HQ_TEXT_CANCEL, HQ_TEXTS };
// an empty or missing line gives def
static inline void hq_text_line(const char *text, int i, const char *def, char *out, size_t cap) {
    const char *s = text;
    for (int k = 0; s && k < i; k++) {
        s = strchr(s, '\n');
        if (s) s++;
    }
    size_t n = 0;
    if (s) {
        const char *e = strchr(s, '\n');
        n = e ? (size_t)(e - s) : strlen(s);
    }
    if (!n) {
        snprintf(out, cap, "%s", def);
        return;
    }
    if (n >= cap) n = cap - 1;
    memcpy(out, s, n);
    out[n] = 0;
}

// macOS 27's GCControllerHomeButtonSettingInAppAction: "system" keeps Home, "app" decides, "off" ignores it, "unavailable"
static inline const char *hq_in_app_name(long v) {
    return v == 0 ? "system" : v == 1 ? "app" : v > 1 ? "off" : "unavailable";
}
