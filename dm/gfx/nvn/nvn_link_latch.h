// Display-link late latch: the callback that woke an acquire waits a bounded time for its present, only into an empty queue
// (docs/reference/frame-rate.md#the-callback-took-the-frame-queued-after-the-previous-tick). Pure logic, tested by tests/t_link_latch.c.
#pragma once
#include <stdint.h>

#define LINK_LATCH_GIVE_UP 8   // timed-out latches in a row before it stops waiting on every tick
#define LINK_LATCH_PROBE 64    // woken callbacks between tries once stopped

// CACurrentMediaTime seconds; 0 = take the queued frame at once. The wait ends an eighth of a period before targetTimestamp (time to encode and commit)
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
