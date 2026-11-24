// The controller applet's player prompt (ShowControllerSupport): the game's thread waits until enough players join or
// the player cancels; no timeout and no close on focus loss, as on a Switch. Pure, for tests.
#pragma once
#include <stddef.h>
#include <stdio.h>
#include <string.h>

typedef enum {
    CP_IDLE,
    CP_OPEN,
    CP_SETTLE,   // input resumes once A and B are up: the closing press never reaches the game
} cp_phase;

typedef struct {
    cp_phase phase;
    int min, max;     // max <= 0: none
    int joined;
    int a_up, b_up;   // up since open or the last join: a held button does not count
    int ok;           // 1 done, 0 canceled
} ctrl_prompt;

static inline void cp_open(ctrl_prompt *q, int min, int max, int joined, int a, int b) {
    *q = (ctrl_prompt){.phase = CP_OPEN, .min = min, .max = max, .joined = joined, .a_up = !a, .b_up = !b};
}

// a press that makes a controller join changes joined, so it does not count as A
static inline cp_phase cp_step(ctrl_prompt *q, int joined, int waiting, int a, int b, int confirm, int cancel) {
    switch (q->phase) {
    case CP_IDLE:
        break;
    case CP_OPEN:
        if (joined != q->joined) {
            q->joined = joined;
            q->a_up = !a;
            q->b_up = !b;
        }
        if (!a) q->a_up = 1;
        if (!b) q->b_up = 1;
        if ((b && q->b_up) || cancel) {   // A and B together: cancel wins
            q->phase = CP_SETTLE;
            q->ok = 0;
        } else if (joined >= q->min && ((a && q->a_up) || confirm || (q->max > 0 && joined >= q->max) || !waiting)) {
            q->phase = CP_SETTLE;
            q->ok = 1;
        }
        break;
    case CP_SETTLE:
        if (!a && !b) q->phase = CP_IDLE;
        break;
    }
    return q->phase;
}

static inline int cp_blocks_input(cp_phase p) { return p != CP_IDLE; }
static inline int cp_can_confirm(const ctrl_prompt *q) { return q->joined >= q->min; }

// One per line of DM_CTRL_PROMPT_TEXT, {min} {max} {joined} replaced; an empty or missing line keeps the English one
enum { CP_TEXT_TITLE, CP_TEXT_HINT, CP_TEXT_COUNT, CP_TEXT_DONE, CP_TEXT_CANCEL, CP_TEXTS };
static const char *const cp_text_default[CP_TEXTS] = {"{min} players wanted", "Press a button on each controller to join.", "Players: {joined} of {min}", "Done", "Cancel"};
static inline void cp_text(const char *text, int i, const ctrl_prompt *q, char *out, size_t cap) {
    const char *s = text;
    size_t n = 0;
    for (int k = 0; s && k < i; k++) {
        s = strchr(s, '\n');
        if (s) s++;
    }
    if (s) {
        const char *e = strchr(s, '\n');
        n = e ? (size_t)(e - s) : strlen(s);
    }
    if (!n) {
        s = cp_text_default[i];
        n = strlen(s);
    }
    if (!cap) return;
    static const char *const key[3] = {"{min}", "{max}", "{joined}"};
    const int val[3] = {q->min, q->max, q->joined};
    size_t o = 0;
    for (size_t k = 0; k < n && o + 1 < cap;) {
        size_t kl = 0;
        int v = 0;
        for (int j = 0; j < 3; j++) {
            size_t l = strlen(key[j]);
            if (k + l <= n && !memcmp(s + k, key[j], l)) {
                kl = l;
                v = val[j];
                break;
            }
        }
        if (!kl) {
            out[o++] = s[k++];
            continue;
        }
        int w = snprintf(out + o, cap - o, "%d", v);
        if (w > 0) o = o + (size_t)w < cap ? o + (size_t)w : cap - 1;
        k += kl;
    }
    out[o] = 0;
}
