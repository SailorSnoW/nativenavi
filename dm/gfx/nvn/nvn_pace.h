// Present pacer, vsync-like: late by under a period keeps the phase; a period or more restarts from now (no burst of presents to catch up). Period 0 = no wait.
#pragma once
#include <stdint.h>

typedef struct { uint64_t target; } nvn_pacer;

// now, period and the result (time to wait) in ns
static inline uint64_t nvn_pace(nvn_pacer *p, uint64_t now, uint64_t period) {
    if (!period || !p->target) { p->target = now; return 0; }
    uint64_t next = p->target + period;
    if (now <= next) { p->target = next; return next - now; }
    p->target = now - next < period ? next : now;
    return 0;
}

// A frame paced by something else (display link, interval 0) restarts the beat, which would otherwise run ahead without bound and make a fallback wait sleep the whole lead.
static inline void nvn_pace_follow(nvn_pacer *p, uint64_t now) { p->target = now; }
