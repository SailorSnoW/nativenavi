// Display-link late latch: the callback that woke an acquire waits a bounded time for its present, only into an empty queue
// (docs/reference/frame-rate.md#the-callback-took-the-frame-queued-after-the-previous-tick). Pure logic, tested by tests/t_link_latch.c.
#pragma once
#include <stdint.h>

#define LINK_LATCH_GIVE_UP 8   // timed-out latches in a row before it stops waiting on every tick
#define LINK_LATCH_PROBE 64    // woken callbacks between tries once stopped
#define LINK_LATCH_SKIP_GAP 1.35   // a callback this many periods after a latched present follows a skipped tick (above the 1.25 of 144 Hz virtual ticks)
#define LINK_LATCH_SKIP_MAX 0.25   // seconds: a longer gap is a pause or a stall, not a skip

// CACurrentMediaTime seconds; 0 = take the queued frame at once. The wait ends an eighth of a period before targetTimestamp, by when the frame's GPU work must end
// and an eighth before the next callback. The drain's early rule (nvn_link_latch_spare) stays inside this bound.
static inline double nvn_link_latch_reach(double room, double period) {
    if (period <= 0) return 0;
    double r = room - period / 8, cap = period - period / 8;
    if (r > cap) r = cap;
    return r > 0 ? r : 0;
}
static inline double nvn_link_latch_wait(double cb, double target_ts, double period, unsigned misses, uint64_t woken, int drain_end) {
    if (misses >= LINK_LATCH_GIVE_UP && !drain_end && woken % LINK_LATCH_PROBE) return 0;
    return nvn_link_latch_reach(target_ts - cb, period);
}

// A latched present whose GPU work ends after targetTimestamp makes the link skip its next callback at frame latency 1; the queue is empty there, so the next callback latches
// again into the same miss and the game is held at 30 fps with no timeout. Leaving the latch for a tick lets the frame stand in the queue again and feeds the drain's backoff.
typedef struct {
    int prev;              // the previous callback latched a present
    unsigned skips, clean; // skipped ticks after latched presents; clean latched ticks since the last skip
} nvn_link_latch_health;
// At each callback with the latch on (seconds). late_gpu: a latched present's GPU end passed its targetTimestamp since the last call; fresh_entry: right after a drain into the
// latch, where one skipped tick is enough. 1: leave the latch now.
static inline int nvn_link_latch_check(nvn_link_latch_health *h, double gap, double period, int late_gpu, int fresh_entry) {
    if (late_gpu) { h->skips = 0; h->clean = 0; return 1; }
    if (!h->prev) { h->skips = 0; h->clean = 0; return 0; }
    if (period > 0 && gap > LINK_LATCH_SKIP_GAP * period && gap < LINK_LATCH_SKIP_MAX) {
        h->skips++; h->clean = 0;
        if (h->skips >= 2 || fresh_entry) { h->skips = 0; return 1; }
        return 0;
    }
    if (period > 0 && gap > 0 && gap <= LINK_LATCH_SKIP_GAP * period) {
        if (++h->clean >= LINK_LATCH_GIVE_UP) h->skips = 0;
    }
    return 0;
}
