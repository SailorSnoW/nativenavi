// Drains a frame standing in the present queue after a late frame: the next acquire waits the spare ticks once
// (docs/reference/frame-rate.md#a-frame-standing-in-the-present-queue-after-a-late-frame; with the latch, #the-callback-took-the-frame-queued-after-the-previous-tick)
#pragma once
#include <math.h>
#include <stdint.h>
#include "nvn_link_latch.h"

#define LINK_DRAIN_MARGIN 0.002    // s: still queued at its tick after a drain (the game presents about 1 ms after a tick)
#define LINK_DRAIN_SECS 2.0        // s: a hiccup's catch-up frames are not a standing queue
#define LINK_DRAIN_TAKES 30
#define LINK_DRAIN_BLOCKED 0.15    // share of a tick (and 2 ms) the acquire waited before each frame: room to spare
#define LINK_DRAIN_LATCH 0x100     // flag in nvn_link_drain_next's result: the drain ends in the latch
#define LINK_LATCH_KEEP 600        // latched frames in a row that count as holding (10 s at 60 fps)
#define LINK_LATCH_BACK_MAX 4      // max doublings of LINK_DRAIN_SECS

typedef struct {
    int run;                  // frames in a row that waited a tick too long, the game waiting
    double since, min_age;
    int erun, min_woke;       // with the latch: frames early after their wake; a run of its own, or run's min_age would drop under a tick
    double esince;
    unsigned back;            // a drain into the latch needs LINK_DRAIN_SECS << back
    int in;                   // a drain into the latch under way or holding
    unsigned stand, held;
    uint64_t drains, entries; // entries: drains into the latch
} nvn_link_drain;

// Whole ticks the frame could have been presented later and still be taken on this tick
static inline int nvn_link_drain_spare(double age, double tick) { return tick > 0 && age > LINK_DRAIN_MARGIN ? (int)floor((age - LINK_DRAIN_MARGIN) / tick) : 0; }

// With the latch: ticks from the callback that woke the frame's acquire to the take, when its present came early enough after it (under half of room and an eighth of a tick
// inside the latch's wait) that the latch would have taken it; -1 otherwise (docs/reference/frame-rate.md#the-callback-took-the-frame-queued-after-the-previous-tick)
static inline int nvn_link_latch_spare(double age, double lead, double room, double tick) {
    if (tick <= 0 || room <= 0 || lead < 0) return -1;
    double limit = room / 2, inside = nvn_link_latch_reach(room, tick) - tick / 8;
    if (limit > inside) limit = inside;
    return lead < limit ? (int)floor((age + lead) / tick + 0.5) : -1;
}

// Per frame the callback takes; seconds. age < 0: the latch waited for it; blocked 0: the acquire did not wait; ok 0: says nothing about a standing queue; room 0: no latch.
// Returns the ticks the next acquire waits more, | LINK_DRAIN_LATCH. A fallback soon after a drain into the latch doubles the time the next one needs
static inline int nvn_link_drain_next(nvn_link_drain *d, double now, double age, double tick, double blocked, int ok, double lead, double room) {
    if (d->in) {   // a latched frame holds; one standing past the drain's ticks is a fallback
        if (room <= 0) d->in = 0;   // another link rate or a present interval of 2: no verdict
        else if (age < 0) {
            d->stand = 0;
            if (++d->held == LINK_LATCH_KEEP) d->back = 0;
        } else if (!d->held && d->stand) {
            d->stand--;
        } else {
            if (d->held < LINK_LATCH_KEEP && d->back < LINK_LATCH_BACK_MAX) d->back++;
            d->in = 0;
        }
    }
    double need = LINK_DRAIN_BLOCKED * tick > 0.002 ? LINK_DRAIN_BLOCKED * tick : 0.002;
    int good = ok && blocked >= need, as = good ? nvn_link_drain_spare(age, tick) : 0, ls = good ? nvn_link_latch_spare(age, lead, room, tick) : 0;
    if (as < 1) {
        d->run = 0;
    } else if (!d->run++) {
        d->since = now;
        d->min_age = age;
    } else if (age < d->min_age) {
        d->min_age = age;
    }
    if (ls < 1) {
        d->erun = 0;
    } else if (!d->erun++) {
        d->esince = now;
        d->min_woke = ls;
    } else if (ls < d->min_woke) {
        d->min_woke = ls;
    }
    if (d->erun >= LINK_DRAIN_TAKES && now - d->esince >= LINK_DRAIN_SECS * (double)(1u << d->back)) {   // by the frame closest to its wake
        int k = d->min_woke;
        d->run = d->erun = 0;
        d->drains++;
        d->entries++;
        d->in = 1;
        d->stand = (unsigned)k;
        d->held = 0;
        return k | LINK_DRAIN_LATCH;
    }
    int spare = d->run >= LINK_DRAIN_TAKES && now - d->since >= LINK_DRAIN_SECS ? nvn_link_drain_spare(d->min_age, tick) : 0;   // by the frame that waited least
    if (spare < 1) return 0;
    d->run = d->erun = 0;
    d->drains++;
    return spare;
}
