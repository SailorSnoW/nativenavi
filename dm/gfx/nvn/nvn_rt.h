// The CPU time per frame a frame thread declares in its time-constraint policy. A frame needing more than declared is held up, so it covers the peak too.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Called on the thread itself; nvn_rt_join returns NULL when the table is full
void *nvn_rt_join(const char *name);
void nvn_rt_tick(void *h);

#define RT_WIN 120
typedef struct {
    uint64_t frame, cpu;   // thread CPU ns at the last sample (frame 0: none yet)
    double win[RT_WIN];   // CPU ms per frame
    int n;
    double comp_ms;   // 0: none declared yet
} nvn_rt;

static inline int nvn_rt_dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static inline double nvn_rt_need(const double *win, int n, double period_ms) {
    double v[RT_WIN];
    memcpy(v, win, (size_t)n * sizeof *v);
    qsort(v, (size_t)n, sizeof *v, nvn_rt_dcmp);
    double p90 = v[(n * 9) / 10 < n ? (n * 9) / 10 : n - 1], c = p90 * 1.25 > v[n - 1] * 1.1 ? p90 * 1.25 : v[n - 1] * 1.1, cap = period_ms * 0.9;
    if (c > cap) c = cap;
    return c < 1 ? 1 : c;
}
// Frames since the last sample count as one sample (their mean): an idle loading screen adds one, not a window. Returns ms to declare, 0 = no change.
static inline double nvn_rt_sample(nvn_rt *r, uint64_t frame, uint64_t cpu_ns, double period_ms) {
    if (!r->frame || frame < r->frame) { r->frame = frame; r->cpu = cpu_ns; return 0; }
    if (frame == r->frame) return 0;
    r->win[r->n++] = (double)(cpu_ns - r->cpu) / 1e6 / (double)(frame - r->frame);
    r->frame = frame;
    r->cpu = cpu_ns;
    if (r->n < RT_WIN) return 0;
    r->n = 0;
    double c = nvn_rt_need(r->win, RT_WIN, period_ms);
    return !r->comp_ms || c > r->comp_ms * 1.15 || c < r->comp_ms * 0.85 ? c : 0;
}
