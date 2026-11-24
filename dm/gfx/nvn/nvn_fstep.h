// Frame step in whole vsyncs (docs/reference/frame-rate.md#frame-step): the rewriter's stub steps the game by the vsyncs handed each frame, and the host shows each frame k vsyncs.
// Late time is a debt paid back a vsync a frame. k steps up after three late frames when the last FSTEP_RUN fall short of k on average (the longest left out), and tries k − 1
// after FSTEP_RUN frames that fit it, a failed trial doubling the hold-off (1 s → 8 s). Probes try kmin when the cost cannot justify the hold (a slowed CPU or GPU, the cost that
// held kmin, a hold the GPU does not explain); a probe's first FSTEP_GRACE frames may be late.
#pragma once
#include <math.h>
#include <stdint.h>
#include <string.h>

// The mean of the last NVN_FDELTA_N present intervals in s, 0 at the target rate (under 1.01 frames); a stall counts at most 0.1 s, the result at most 4 frames of 60 fps
#define NVN_FDELTA_N 8
typedef struct { uint64_t last; double t[NVN_FDELTA_N]; int n, i; } nvn_fdelta;
static inline float nvn_fdelta_next(nvn_fdelta *f, uint64_t now_ns) {
    uint64_t was = f->last;
    f->last = now_ns;
    if (!was) return 0;
    double s = (double)(now_ns - was) / 1e9, sum = 0;
    f->t[f->i] = s > 0.1 ? 0.1 : s;
    f->i = (f->i + 1) % NVN_FDELTA_N;
    if (f->n < NVN_FDELTA_N) f->n++;
    for (int i = 0; i < f->n; i++) sum += f->t[i];
    double mean = sum / f->n;
    return mean * 60 < 1.01 ? 0.0f : (float)fmin(mean, 4.0 / 60);
}

// Presents the game made no frame for (its stub's count at state +0x30 did not move: frame advance, boot) are not judged; the frame after one steps at kmin
// (docs/reference/frame-rate.md#frame-advance-stepped-by-the-hold-and-the-measured-frame-time)
typedef struct { uint64_t stepped; int armed, held; } nvn_fheld;
static inline int nvn_fheld_judged(nvn_fheld *h, uint64_t stepped) {
    int held = h->armed && stepped == h->stepped, judged = !held && !h->held;
    h->stepped = stepped;
    h->held = held;
    h->armed = 1;
    return judged;
}

#define FSTEP_MAX 3           // a game's own interval logic uses 1–3
#define FSTEP_LATE 0.1        // vsyncs (1.7 ms) past k that make a frame late: 53 fps is not 60
#define FSTEP_SHORT_MAX 2.0  // vsyncs past its hold a frame counts at most toward the average
#define FSTEP_FIT 0.95        // share of k − 1 a cost fits in (docs/reference/frame-rate.md#a-scene-that-fits-hold-1-kept-at-30-fps-by-the-fits-margin)
#define FSTEP_RUN 30          // frames that fit k − 1 before a trial, and frames a trial must hold
#define FSTEP_DEBT_MAX 15.0   // 0.25 s: the rest of a long stall is dropped
#define FSTEP_HOLD0 60.0      // 1 s, doubled after each failed trial up to 8 s
#define FSTEP_HOLD_MAX 480.0
#define FSTEP_GRACE 30        // a probe's frames whose lateness does not count
#define FSTEP_SAME 1.1        // cost at the longer hold within this of the cost that held kmin: the same scene
// The grace renews while a probe's frames keep getting cheaper by FSTEP_RAMP (the clock still rising), up to FSTEP_PROBE_MAX frames
#define FSTEP_RAMP 0.05       // above a 25 ms frame's noise (4%, 1% over 15 frames)
#define FSTEP_PROBE_MAX 180   // 3 s at 60 fps
#define FSTEP_GPU_IDLE 0.05   // GPU time (frame units) under which nothing is drawn: 0.8 ms at 60 fps
#define FSTEP_UHOLD_MAX 3600.0   // 60 s
// A stall (pause, covered window, fullscreen switch, silent link) is not late; its time inflates no timer and the FSTEP_GRACE frames after it do not count.
// settle: the caller names the disruption; its excess time is not debt
#define FSTEP_STALL 8.0       // 133 ms: over twice the longest hold

typedef struct {
    int k, step;              // k: vsyncs on screen; step: handed to the game (k, or k + 1 while paying back)
    int kmin;                 // a lower one takes effect at once
    double debt;              // vsyncs the game is behind
    int late_run, fit_run, trial;   // trial: frames left of a trial at k − 1 (0 = none)
    int sgrace;               // grace frames left after a stall
    int probe, grace;         // probe 1: the trial is a probe; 2: it held, its run at kmin not over yet
    int ramp;                 // set by the caller: renew the grace (FSTEP_RAMP)
    double pa, pb; int plate, ptotal;   // the grace window's half costs and late frames; grace frames taken in all
    double hold, since;       // hold: vsyncs a trial waits after a step up
    int tpend; double tsince; // a held trial whose run has not lasted FSTEP_HOLD_MAX (a step up fails it)
    int per_vsync;            // set by the caller: ticks per 1/60 s vsync (0 = 1; the unit is 1/120 s at 120 fps)
    double shown_slack;       // set by the caller: vsyncs a frame on time may show past k (screens not a multiple of 60)
    int settle;               // set per call, cleared on return: a host-known pause or window change
    int screen;               // set per call, cleared on return: 0 judge by shown and cost; 1 / 2 the screen showed a recent frame on time / late (present snapshots)
    double run, crun, cref;   // vsyncs at kmin in a row; crun: their on-time cost (running mean, not in a probe's trial); cref: crun when the last long run ended
    double ccur; int ck, cn;  // on-time cost at the longer hold ck (running mean), its count
    int gpu_on; double gpu;   // set per call, cleared: gpu is the latest finished frame's GPU time (shown's unit)
    int urun;
    int ilate_run, uprobe;    // ilate_run counts late frames with the GPU idle too; uprobe: a probe started by nvn_fstep_unpaced
    int up_idle;
    double uhold;             // hold-off before such a probe: 1 s, doubled per failure up to FSTEP_UHOLD_MAX
    int ckmin;                // the kmin run, crun and cref belong to
    double cref_at[FSTEP_MAX + 1];   // kept while another kmin is in force
    double over[FSTEP_RUN], over_sum; int over_i, over_k;   // shown − k of the last FSTEP_RUN frames at hold over_k (clamped), their sum, next slot
    double last_cost[3];
    int no_payback;           // set by the caller: the game does not take the step (120 fps)
    uint64_t late, paid, ups, downs, failed, probes, stalls; double dropped;
} nvn_fstep;

// A display-link drain inside a frame (nvn_link_drain.h) is cut from its time: neither late nor owed. first: drain in this frame's acquire (in *pacing); before: in the
// previous frame's later acquires. Returns span less the cut; ns
static inline uint64_t nvn_fstep_drain_cut(uint64_t span, uint64_t hold, uint64_t first, uint64_t before, uint64_t *pacing) {
    if (first > *pacing) first = *pacing;
    uint64_t over = span > hold ? span - hold : 0, cut = first + before < over ? first + before : over;
    *pacing -= cut < first ? cut : first;
    return span - cut;
}

static inline int nvn_fstep_same(const nvn_fstep *s) { return s->cref > 0 && s->cn >= FSTEP_RUN && s->ccur <= FSTEP_SAME * s->cref; }
static inline int nvn_fstep_unpaced(const nvn_fstep *s) { return s->urun >= FSTEP_RUN; }

// Once per frame, in vsyncs: shown since the previous start; cost max(CPU, GPU); kmin 1 at a 60 fps cap, 2 at 30; kmax 2 at a 30 fps cap (the 30 Hz link holds even counts);
// fixed: the game's own step (held as is); slow: the CPU is slowed for reasons other than heat. Returns the next step; s->k is its time on screen
static inline int nvn_fstep_next(nvn_fstep *s, double shown, double cost, int kmin, int kmax, int fixed, int slow) {
    const int pv = s->per_vsync ? s->per_vsync : 1;
    const double hold0 = FSTEP_HOLD0 * pv, hold_max = FSTEP_HOLD_MAX * pv, uhold_max = FSTEP_UHOLD_MAX * pv;
    if (kmin < 1) kmin = 1;
    if (kmax > FSTEP_MAX) kmax = FSTEP_MAX;
    if (kmin > FSTEP_MAX) kmin = FSTEP_MAX;
    if (kmax < kmin) kmax = kmin;
    if (!s->hold) s->hold = hold0;
    if (!s->uhold) s->uhold = hold0;
    int gpu_on = s->gpu_on;
    double gpu = s->gpu;
    s->gpu_on = 0;
    s->gpu = 0;
    int settle = s->settle;
    s->settle = 0;
    int scr = s->screen;
    s->screen = 0;
    if (fixed > 0) {   // the game's own step: nothing to pay back
        s->k = s->step = fixed < FSTEP_MAX ? fixed : FSTEP_MAX;
        s->debt = 0;
        s->late_run = s->ilate_run = s->fit_run = s->trial = s->probe = s->grace = s->sgrace = s->tpend = 0;
        s->run = 0;
        s->kmin = FSTEP_MAX + 1;
        return s->step;
    }
    if (!s->k) { s->k = s->step = s->kmin = kmin; return s->step; }
    int held = s->k;
    int stall = settle || shown >= FSTEP_STALL * pv, sgrace = stall || s->sgrace > 0;
    if (stall) {
        s->sgrace = FSTEP_GRACE;
        s->stalls++;
    } else if (s->sgrace) {
        s->sgrace--;
    }
    if (held != s->over_k) {   // judged against the hold in force
        s->over_k = held;
        s->over_sum = 0;
        s->over_i = 0;
        memset(s->over, 0, sizeof s->over);
    }
    double over = stall ? 0 : shown - held;
    if (over < -1) over = -1;
    if (over > FSTEP_SHORT_MAX) over = FSTEP_SHORT_MAX;
    s->over_sum += over - s->over[s->over_i];
    s->over[s->over_i] = over;
    s->over_i = (s->over_i + 1) % FSTEP_RUN;
    if (!stall) {
        s->last_cost[0] = s->last_cost[1];
        s->last_cost[1] = s->last_cost[2];
        s->last_cost[2] = cost;
    }
    double dt = stall && shown > held ? held : shown;
    if (settle && shown > held) {
        s->dropped += shown - held;
        s->debt += held - s->step;
    } else {
        s->debt += shown - s->step;
    }
    if (s->debt > FSTEP_DEBT_MAX) {
        s->dropped += s->debt - FSTEP_DEBT_MAX;
        s->debt = FSTEP_DEBT_MAX;
    }
    if (s->debt < -1) s->debt = -1;   // ahead: at most one vsync is remembered
    s->since += dt;
    s->tsince += dt;
    if (s->tpend && s->tsince >= hold_max) {
        s->tpend = 0;
        s->hold = hold0;
    }
    int cap_probe = kmin < s->kmin && s->kmin <= FSTEP_MAX && s->k > kmin;
    if (cap_probe) {   // the cap rose: a probe at once, its grace covering clocks still set for the longer hold
        s->k = kmin;
        s->probes++;
        s->trial = FSTEP_RUN;
        s->probe = 1;
        s->uprobe = 0;
        s->grace = FSTEP_GRACE;
        s->fit_run = s->late_run = s->ilate_run = 0;
        s->pa = s->pb = 0;
        s->plate = s->ptotal = 0;
    } else if (kmin < s->kmin || s->k < kmin) s->k = kmin;
    if (s->k > kmax) s->k = kmax;
    s->kmin = kmin;
    int idle = gpu_on && gpu < FSTEP_GPU_IDLE;   // nothing drawn: lateness is load work, not the scene
    int late = scr ? scr == 2 : shown > held + FSTEP_LATE + s->shown_slack || cost > held + FSTEP_LATE, grace = s->grace > 0;
    if (grace && !cap_probe && !stall) {   // the frame at the old cap or a stall does not use up the new hold's grace
        if (s->grace > FSTEP_GRACE / 2) s->pa += cost;
        else s->pb += cost;
        s->plate += late;
        s->ptotal++;
        if (!--s->grace && s->ramp && s->plate && s->ptotal + FSTEP_GRACE <= FSTEP_PROBE_MAX && s->pb <= (1 - FSTEP_RAMP) * s->pa) {
            s->grace = FSTEP_GRACE;
            s->pa = s->pb = 0;
            s->plate = 0;
        }
    }
    // late with the GPU idle: neither counts nor ends the run; FSTEP_RUN of them step up anyway (a CPU-heavy scene)
    if (late) {
        if (!stall) s->late++;
        s->late_run = grace || sgrace ? 0 : s->late_run + !idle;
        s->ilate_run = grace || sgrace ? 0 : s->ilate_run + 1;
        s->fit_run = 0;
    } else {
        s->late_run = s->ilate_run = 0;
    }
    // k − 1, not kmin: the GPU clock falls at a longer hold, so one that fits kmin may read above it there
    s->urun = held > kmin && s->k == held && gpu_on && gpu <= FSTEP_FIT * (held - 1) && cost >= shown - FSTEP_LATE ? s->urun + 1 : 0;
    if (kmin != s->ckmin) {   // another cap: what held it before comes back
        if (s->ckmin >= 1 && s->ckmin <= FSTEP_MAX) {
            // a cap can end a long run before any hitch saved its cost
            if (s->run >= hold_max && s->crun > 0) s->cref = s->crun;
            s->cref_at[s->ckmin] = s->cref;
        }
        s->ckmin = kmin;
        s->run = s->crun = 0;
        s->cref = kmin <= FSTEP_MAX ? s->cref_at[kmin] : 0;
    }
    // on time on screen but over its hold (absorbed by snapshots): not what the hold costs
    int fits = !scr || cost <= held;
    if (held <= kmin) {
        s->run += dt;
        s->cn = 0;
        if (!late && !stall && fits && s->probe != 1) s->crun = s->crun > 0 ? s->crun + (cost - s->crun) / 32 : cost;
    } else {
        s->run = 0;
        if (held != s->ck) {
            s->ck = held;
            s->cn = 0;
        }
        if (!late && !stall && fits) {
            s->ccur = s->cn ? s->ccur + (cost - s->ccur) / 8 : cost;
            s->cn++;
        }
    }
    // a rate below k over the last FSTEP_RUN frames, the longest left out (one stall is not a rate)
    double omax = 0;
    for (int i = 0; i < FSTEP_RUN; i++) if (s->over[i] > omax) omax = s->over[i];
    int short_of_k = scr || s->no_payback || s->over_sum - omax > FSTEP_LATE * FSTEP_RUN;
    if ((s->late_run >= 3 || s->ilate_run >= FSTEP_RUN) && s->k < kmax && short_of_k) {   // to the cheapest of the three late frames' cost
        double c3 = s->last_cost[0] < s->last_cost[1] ? s->last_cost[0] : s->last_cost[1];
        if (s->last_cost[2] < c3) c3 = s->last_cost[2];
        int want = (int)ceil(c3 - 0.05);
        if (want < s->k + 1) want = s->k + 1;
        if (want > kmax) want = kmax;
        if (s->trial || s->tpend || (s->probe == 2 && s->since < hold_max)) {
            s->failed++;
            s->trial = 0;
            s->hold = s->hold * 2 < hold_max ? s->hold * 2 : hold_max;
            if (s->uprobe) s->uhold = s->uhold * 2 < uhold_max ? s->uhold * 2 : uhold_max;
        } else if (s->probe == 2) {
            s->hold = s->uhold = hold0;
        }
        if (held == kmin && s->run >= hold_max) {
            s->cref = s->crun;
            s->uhold = hold0;
        }
        s->up_idle = s->late_run < 3;
        s->k = want;
        s->ups++;
        s->late_run = s->ilate_run = s->fit_run = s->urun = 0;
        s->since = 0;
        s->probe = s->uprobe = 0;
        s->tpend = 0;
    } else if (s->trial && !grace && !sgrace && !--s->trial) {   // a held trial is judged by how long its run lasts
        if (s->probe) {
            s->probe = 2;
            s->since = 0;
        } else {
            s->tpend = 1;
            s->tsince = 0;
        }
    }
    // a frame also fits while the hold's on-time mean does (docs/reference/frame-rate.md#single-frames-over-the-fit-keep-a-longer-hold)
    double fitc = s->ck == s->k && s->cn && s->ccur < cost ? s->ccur : cost;
    if (!late) s->fit_run = s->k > kmin && fitc <= FSTEP_FIT * (s->k - 1) ? s->fit_run + 1 : 0;
    if (s->fit_run >= FSTEP_RUN && s->since >= s->hold && !s->trial) {
        s->k--;
        s->downs++;
        s->trial = FSTEP_RUN;
        s->fit_run = 0;
    } else if ((slow || nvn_fstep_same(s) || (nvn_fstep_unpaced(s) && s->since >= s->uhold)) && s->k > kmin && s->since >= s->hold && !s->trial) {   // probe: straight to kmin, so the controller sees the whole deadline
        s->uprobe = !slow && !nvn_fstep_same(s);
        s->k = kmin;
        s->downs++;
        s->probes++;
        s->trial = FSTEP_RUN;
        s->probe = 1;
        s->grace = FSTEP_GRACE;
        s->fit_run = s->late_run = s->ilate_run = 0;
        s->pa = s->pb = 0;
        s->plate = s->ptotal = 0;
    }
    s->step = s->k + (s->debt >= 1 && s->k < FSTEP_MAX);
    s->paid += s->step > s->k;
    return s->step;
}

// For a stub that steps by max(host step, measured frame time vs): the measured time carries a late frame's loss, so no whole vsync is paid beside it; the debt counts what the
// game stepped. Call after nvn_fstep_next; returns the vsyncs to publish, 0 = none
static inline double nvn_fstep_measured(nvn_fstep *s, double vs) {
    if (vs <= 0 || s->debt <= 0) return 0;
    if (s->step > s->k) {
        s->step = s->k;
        s->paid--;
    }
    if (vs > s->step) s->debt -= vs - s->step;
    return vs;
}

// The GPU clock follows the hold: the GPU counts as slowed (nvn_fstep_next's slow) when its time at a hold above kmin is FSTEP_GPU_SLOW times its time at kmin and alone does
// not fit kmin. A failed probe measures the scene's own ratio (its GPU time at kmin once the clock rose, against the longer hold's); after FSTEP_GPU_FAILS of them the largest
// ratio replaces the reference (docs/reference/frame-rate.md#a-scene-that-does-not-fit-the-shorter-hold-probed-against-another-scenes-gpu-time)
#define FSTEP_GPU_SLOW 1.3
#define FSTEP_GPU_N 30        // frames at the longer hold before comparing
#define FSTEP_GPU_PN 3        // a probe's frames at kmin after half its grace before a ratio
#define FSTEP_GPU_FAILS 2
#define FSTEP_GPU_RMIN 0.75   // the smallest ratio a probe gives: below it the frames were another scene or a stall
typedef struct {
    double ref, cur, avg; int n, k, kmin; double ref_at[FSTEP_MAX + 1];   // ref: GPU time at kmin (ref_at for each); cur, avg: at hold k, means of 8 and 32 frames
    double pre, post; int pre_k, pn, was, fail;   // the last probe: pre at hold pre_k (0 = none), post its kmin frames, pn their count; was: the last frame's probe; fail: its ratio waits for FSTEP_GPU_N frames
    double ratio_at[FSTEP_MAX + 1][FSTEP_MAX + 1]; int ratio_n[FSTEP_MAX + 1][FSTEP_MAX + 1];   // [kmin][k]: the largest failed-probe ratio, and their count
} nvn_fgpu;
// 0: the reference decides
static inline double nvn_fgpu_ratio(const nvn_fgpu *g) {
    return g->kmin >= 1 && g->kmin <= FSTEP_MAX && g->k >= 1 && g->k <= FSTEP_MAX && g->ratio_n[g->kmin][g->k] >= FSTEP_GPU_FAILS ? g->ratio_at[g->kmin][g->k] : 0;
}
static inline int nvn_fgpu_slow(const nvn_fgpu *g) {
    if (g->n < FSTEP_GPU_N || g->cur <= FSTEP_FIT * g->kmin) return 0;
    double r = nvn_fgpu_ratio(g);
    return r > 0 ? g->cur / r <= g->kmin : g->ref > 0 && g->cur >= FSTEP_GPU_SLOW * g->ref;
}
static inline void nvn_fgpu_learn(nvn_fgpu *g, int kmin, int k, double t) {
    double r = t / g->post, *at = &g->ratio_at[kmin][k];
    if (r < FSTEP_GPU_RMIN) return;
    if (r > *at) *at = r;
    g->ratio_n[kmin][k]++;
}
// once per frame; gpu in vsyncs (0 = none finished yet)
static inline int nvn_fstep_gpu_slowed(nvn_fgpu *g, double gpu, const nvn_fstep *s, int kmin) {
    if (kmin < 1 || kmin > FSTEP_MAX) return 0;
    int k = s->k ? s->k : kmin, probe = s->probe;
    if (kmin != g->kmin) g->fail = 0;   // another cap: the clocks the probe measured are not this one's
    if (probe == 1 && g->was != 1) {
        g->pre_k = g->kmin == kmin && g->k > kmin && g->k <= FSTEP_MAX && g->n >= FSTEP_GPU_N ? g->k : 0;
        g->pre = g->cur < g->avg ? g->cur : g->avg;
        g->post = 0;
        g->pn = 0;
        g->fail = 0;
    } else if (probe != 1 && g->was == 1) {
        g->fail = !probe && k > kmin && g->pn >= FSTEP_GPU_PN;   // failed when the hold went up again
    }
    g->was = probe;
    if (probe == 1 && k <= kmin && gpu > 0 && s->grace <= FSTEP_GRACE / 2) {   // the clock at its highest
        g->post = g->pn ? g->post + (gpu - g->post) / 4 : gpu;
        g->pn++;
    }
    g->ref = g->ref_at[kmin];
    g->kmin = kmin;
    if (gpu <= 0) return nvn_fgpu_slow(g);
    if (k <= kmin) {
        if (probe != 1) g->ref = g->ref_at[kmin] = g->ref > 0 ? g->ref + (gpu - g->ref) / 32 : gpu;
        g->n = 0;
        g->k = k;
        g->fail = 0;
        return 0;
    }
    if (k != g->k) {
        g->k = k;
        g->n = 0;
    }
    g->n++;
    g->cur = g->n > 1 ? g->cur + (gpu - g->cur) / 8 : gpu;
    g->avg = g->n > 1 ? g->avg + (gpu - g->avg) / (g->n < 32 ? g->n : 32) : gpu;
    if (g->fail && g->n >= FSTEP_GPU_N && k <= FSTEP_MAX) {
        double t = g->cur < g->avg ? g->cur : g->avg;
        if (k == g->pre_k) {
            nvn_fgpu_learn(g, kmin, k, g->pre < t ? g->pre : t);
        } else {
            nvn_fgpu_learn(g, kmin, k, t);
            if (g->pre_k) nvn_fgpu_learn(g, kmin, g->pre_k, g->pre);
        }
        g->fail = 0;
    }
    return nvn_fgpu_slow(g);
}
