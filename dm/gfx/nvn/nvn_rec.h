// Deferred encoding (DM_NVN_ENC_THREAD=1; nvn_mtl.m only): the replay calls stand-ins for the main command buffer, its encoders and argument tables; each call becomes a record the
// encode thread makes in the same order on the real objects. Commits and signals are records too, and the submit thread drains (rec_drain) at each submission's end and before
// anything that waits for the GPU or uses another queue. Objects are made on the submit thread (device calls are thread-safe); queue commits take the residency lock.
// A selector the stand-ins lack stops the host.

// Copy queue: a worker fills large uniform shadows (ubp_get) in queue order; every commit first waits for the copies queued before it (ubc_wait), a CPU read for its own.
// Hand-off without a mutex: the copy thread sleeps on g_ubc_head, waiters on g_ubc_done (os_sync_wait_on_address); only the sleeper clears its own flag, waiters are counted
// (docs/reference/frame-rate.md#the-uniform-copy-threads-hand-off-convoys-on-its-mutex)
typedef struct { void *d; const void *s; size_t n; } ubc_op;
enum { UBC_N = 8192, UBC_SPIN = 4096 };   // UBC_SPIN: a waiter's spins before it sleeps
static ubc_op g_ubc[UBC_N];
static _Alignas(128) _Atomic uint64_t g_ubc_head;   // the copy thread sleeps on it
static _Alignas(128) _Atomic uint64_t g_ubc_done;   // waiters sleep on it
static _Alignas(128) _Atomic uint32_t g_ubc_sleeping, g_ubc_waiting;   // asleep or about to be; only the copy thread clears its flag
static _Atomic uint64_t g_ubc_wakes;
#ifndef UBC_T_SLEEPING   // t_ubc_wait: delays the copy thread's sleep so a late wake lands there
#define UBC_T_SLEEPING() ((void)0)
#endif
static int ubc_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_UB_COPY_THREAD");
        v = !(e && !strcmp(e, "0"));
    }
    return v;
}
// only ubc_push calls it, after its store to g_ubc_head
static inline void ubc_kick(void) {
    if (atomic_load(&g_ubc_sleeping)) {
        atomic_fetch_add_explicit(&g_ubc_wakes, 1, memory_order_relaxed);
        os_sync_wake_by_address_any(&g_ubc_head, sizeof g_ubc_head, OS_SYNC_WAKE_BY_ADDRESS_NONE);
    }
}
static void *ubc_thread(void *arg) {
    (void)arg;
    pthread_setname_np("dm-ub-copy");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    for (uint64_t t = 0;;) {
        uint64_t h = atomic_load_explicit(&g_ubc_head, memory_order_acquire);
        for (int spin = 0; spin < 2000 && t == h; spin++) h = atomic_load_explicit(&g_ubc_head, memory_order_acquire);
        while (t == h) {   // flag, then word (seq_cst, as the pusher's store and flag load): a push sees the flag or this sees the push
            atomic_store(&g_ubc_sleeping, 1);
            if ((h = atomic_load(&g_ubc_head)) != t) break;
            UBC_T_SLEEPING();
            os_sync_wait_on_address(&g_ubc_head, t, sizeof g_ubc_head, OS_SYNC_WAIT_ON_ADDRESS_NONE);
            atomic_store(&g_ubc_sleeping, 0);
            h = atomic_load(&g_ubc_head);
        }
        atomic_store_explicit(&g_ubc_sleeping, 0, memory_order_relaxed);
        for (; t < h; t++) {
            const ubc_op *o = &g_ubc[t % UBC_N];
            memcpy(o->d, o->s, o->n);
        }
        atomic_store(&g_ubc_done, t);
        if (atomic_load(&g_ubc_waiting)) {
            atomic_fetch_add_explicit(&g_ubc_wakes, 1, memory_order_relaxed);
            os_sync_wake_by_address_all(&g_ubc_done, sizeof g_ubc_done, OS_SYNC_WAKE_BY_ADDRESS_NONE);
        }
    }
    return NULL;
}
// replay or encode thread
static void ubc_wait(uint64_t seq) {
    if (atomic_load_explicit(&g_ubc_done, memory_order_acquire) >= seq) return;
    uint64_t w0 = ws_t0();
    for (int spin = 0;; spin++) {
        if (spin < UBC_SPIN) {
            __builtin_arm_yield();
            if (atomic_load_explicit(&g_ubc_done, memory_order_acquire) >= seq) break;
            continue;
        }
        atomic_fetch_add(&g_ubc_waiting, 1);   // count, then word (as the copy thread's store and count load): it sees this waiter or this sees its store
        uint64_t d = atomic_load(&g_ubc_done);
        if (d < seq) os_sync_wait_on_address(&g_ubc_done, d, sizeof g_ubc_done, OS_SYNC_WAIT_ON_ADDRESS_NONE);
        atomic_fetch_sub(&g_ubc_waiting, 1);
        if (atomic_load_explicit(&g_ubc_done, memory_order_acquire) >= seq) break;
    }
    if (ws_on()) ws_put(WS_UBC, w0, 1);
}
static inline uint64_t ubc_head(void) { return atomic_load_explicit(&g_ubc_head, memory_order_relaxed); }
// submit thread only; returns the op count that completes it
static uint64_t ubc_push(void *d, const void *s, size_t n) {
    static int started;
    if (!started) {
        pthread_t th;
        if (pthread_create(&th, NULL, ubc_thread, NULL)) MDIE("cannot start the uniform copy thread");
        pthread_detach(th);
        started = 1;
    }
    uint64_t h = atomic_load_explicit(&g_ubc_head, memory_order_relaxed);
    if (h - atomic_load_explicit(&g_ubc_done, memory_order_acquire) >= UBC_N) ubc_wait(h - UBC_N + 1);
    g_ubc[h % UBC_N] = (ubc_op){d, s, n};
    atomic_store(&g_ubc_head, h + 1);
    ubc_kick();
    return h + 1;
}

enum {
    RO_CB_NEW, RO_CB_BEGIN, RO_CB_END, RO_COMMIT, RO_COMMIT_REAL, RO_SIGNAL, RO_CB_BLOCK, RO_Q_BLOCK,
    RO_RE_BEGIN, RO_CE_BEGIN, RO_END,
    RO_BAR_QUEUE, RO_BAR_AFTER_STAGES, RO_BAR_ENCODER,
    RO_R_TABLE, RO_R_PSO, RO_R_DSS, RO_R_VIEWPORT, RO_R_SCISSOR, RO_R_CULL, RO_R_WINDING, RO_R_FILL, RO_R_BIAS, RO_R_CLIP, RO_R_SREF, RO_R_BLEND, RO_R_VIS, RO_R_AMP, RO_R_CMAP,
    RO_R_DRAW, RO_R_DRAW_INST, RO_R_DRAW_IND, RO_R_DRAW_IDX, RO_R_DRAW_IDX_IND, RO_R_MESH, RO_R_MESH_IND, RO_R_STORE_C, RO_R_STORE_D, RO_R_STORE_S,
    RO_C_TABLE, RO_C_PSO, RO_C_DISPATCH, RO_C_DISPATCH_IND, RO_C_COPY_BB, RO_C_COPY_BT, RO_C_COPY_BT_OPT, RO_C_COPY_TB, RO_C_COPY_TB_OPT, RO_C_COPY_TT, RO_C_FILL,
    RO_T_ADDR, RO_T_TEX, RO_T_SAMP, RO_T_RESOURCE,
    RO_LABEL, RO_R_TS, RO_C_TS,
    RO_NOTE,   // a[0] an _Atomic uint64_t set to a[1] once every earlier call is made
};
typedef struct { uint32_t op, nobj; uint64_t a[15]; } rec_t;   // the first nobj slots are retained objects (0 = nil), released once made
enum { REC_BLK = 256 };
typedef struct rec_blk { struct rec_blk *next; int n; rec_t r[REC_BLK]; } rec_blk;

static int g_rec = -1;
static inline int rec_on(void) {
    if (__builtin_expect(g_rec >= 0, 1)) return g_rec;
    const char *e = getenv("DM_NVN_ENC_THREAD");
    g_rec = e ? atoi(e) > 0 : DM_GAME_ENC_THREAD;
    if (g_rec) MLOG("Metal encoding runs on its own thread (DM_NVN_ENC_THREAD=0 turns it off)");
    return g_rec;
}
static pthread_mutex_t g_rb_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_rb_work = PTHREAD_COND_INITIALIZER, g_rb_idle = PTHREAD_COND_INITIALIZER;
static rec_blk *g_rb_cur, *g_rb_head, *g_rb_tail, *g_rb_free;
static int g_rb_busy, g_rb_started, g_rb_sleeping;

static inline uint64_t rec_obj(id o) { return o ? (uint64_t)(uintptr_t)CFBridgingRetain(o) : 0; }
static inline id rec_id(uint64_t v) { return (__bridge id)(void *)(uintptr_t)v; }
static inline uint64_t rec_f(float f) { uint32_t u; memcpy(&u, &f, 4); return u; }
static inline float rec_fv(uint64_t v) { uint32_t u = (uint32_t)v; float f; memcpy(&f, &u, 4); return f; }

static pthread_mutex_t g_note_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_note_cv = PTHREAD_COND_INITIALIZER;
// the encode thread's progress only, never the GPU
static void rec_note_wait(_Atomic uint64_t *p, uint64_t v) {
    if (atomic_load(p) >= v) return;
    pthread_mutex_lock(&g_note_mu);
    while (atomic_load(p) < v) pthread_cond_wait(&g_note_cv, &g_note_mu);
    pthread_mutex_unlock(&g_note_mu);
}

// Held commits (replay_ahead; docs/reference/frame-rate.md#the-gpu-waits-at-a-frames-start-for-commands-the-game-queued-long-before): a queue parked on a CPU-signalled event is
// encoded meanwhile, but what reaches the Metal queue is kept in order until the wait is satisfied. HOLD_MAX_NS after a thread first waits for held work (it may be the signaller)
// it is made at once, with a red line
enum { HOLD_MAX_NS = 200000000 };
static _Atomic uint64_t g_hold_stuck;        // ns; 0 = none; cleared only as the hold ends
static _Atomic uint32_t g_hold_gen;          // a request for one hold does not end the next
static _Atomic int g_hold_on;
static _Atomic uint64_t g_hold_req;          // hold << 2 | 1 satisfied, 2 forced, 3 host drain
static _Atomic uint32_t g_hold_recs;         // submit thread
static volatile uint32_t *g_hold_word;
static unsigned g_hold_mode;
static uint32_t g_hold_value;
static uint64_t g_hold_t0, g_hold_first, g_hold_idle;   // idle: the encode thread's, when the work committed before the hold had left the GPU
static NSMutableArray *g_hold_ops;           // encode thread's
static pthread_mutex_t g_hold_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hold_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_hold_n, g_hold_ops_n, g_hold_forced, g_hold_drained, g_hold_blocks, g_hold_block_recs, g_hold_ns;
static uint64_t g_hold_ap_base, g_hold_ab_base;              // the ahead-write counters when the hold began
static uint64_t g_hold_ap, g_hold_ab, g_hold_ahead_holds, g_hold_ahead_holds_w;   // guest writes between a hold's begin and its signal
static const char *volatile g_hold_force_why;
static uint64_t g_rc_ahead_waits;
static const char HOLD_DRAIN[] = "drain";
static int on_sub_thread(void);
void nvn_queue_ahead_refuse(volatile uint32_t *word);
int crash_ending(void);
static void hold_stuck(void) {
    uint64_t z = 0;
    atomic_compare_exchange_strong(&g_hold_stuck, &z, clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
}
static int hold_pred(uint32_t c) { return g_hold_mode == 0 ? c == g_hold_value : (uint32_t)(c - g_hold_value) < 0x80000000u; }
static int hold_ready(void) { return hold_pred(atomic_load_explicit((_Atomic uint32_t *)g_hold_word, memory_order_acquire)); }
// encode thread
static int hold_check(void) {
    if (!atomic_load_explicit(&g_hold_on, memory_order_acquire)) return 0;
    uint64_t rq = atomic_load(&g_hold_req);
    int req = rq >> 2 == atomic_load(&g_hold_gen) ? (int)(rq & 3) : 0;
    uint64_t now = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    if (!req && hold_ready()) req = 1;
    uint64_t stuck = atomic_load(&g_hold_stuck);
    // The game signals once the frame before has left the GPU: a wait counts from then. The process exits after a crash report, which may run on the game thread that
    // signals, or the watchdog's last dump: nothing to force
    if (stuck && !g_hold_idle && atomic_load(&g_done) + 1 >= g_hold_first) g_hold_idle = now;
    uint64_t from = stuck > g_hold_idle ? stuck : g_hold_idle;
    if (!req && stuck && g_hold_idle && now > from && now - from > HOLD_MAX_NS && !crash_ending()) {
        req = 2;
        g_hold_force_why = "a thread had waited for them for 200 ms with no signal";
    }
    if (!req) return 1;
    for (void (^op)(void) in g_hold_ops) op();
    g_hold_n++;
    g_hold_ops_n += g_hold_ops.count;
    g_hold_ns += now - g_hold_t0;
    [g_hold_ops removeAllObjects];
    if (req == 3) g_hold_drained++;
    if (req == 2) {
        nvn_queue_ahead_refuse(g_hold_word);
        if (!g_hold_forced++) MLOG("[RED] replay ahead: held commands were handed to the GPU before the game's signal (%s); further ones are counted", g_hold_force_why ?: "?");
    }
    // the hold ends here, with the game's signal: the writes made while it was open are the ones the ahead translation could have missed
    nvn_dirty_ahead_watch(0);
    {
        uint64_t ap, ab, ah;
        nvn_dirty_ahead_stats(&ap, &ab, &ah);
        uint64_t dp = ap - g_hold_ap_base, db = ab - g_hold_ab_base;
        g_hold_ap += dp; g_hold_ab += db; g_hold_ahead_holds++;
        if (dp) {
            g_hold_ahead_holds_w++;
            static _Atomic uint64_t said;
            if (atomic_fetch_add(&said, 1) < 8)
                MLOG("replay ahead: %llu guest pages (%.1f MB) written between a hold's begin and its signal; the copies made at translation read them ahead of the signal (%llu of %llu holds so far)",
                     (unsigned long long)dp, db / 1048576.0, (unsigned long long)g_hold_ahead_holds_w, (unsigned long long)g_hold_ahead_holds);
        }
    }
    if (g_hold_n % 3000 == 0) MLOG("replay ahead: %llu holds, %.1f queue operations and %.2f ms a hold, %llu forced, %llu ended by a drain of the host's, submit thread stopped %llu times before the signal (%.1f queue operations recorded by then); of them at a condition whose report was in flight %llu; guest pages written between a hold's begin and its signal %llu (%.1f MB) in %llu of %llu holds", (unsigned long long)g_hold_n, (double)g_hold_ops_n / g_hold_n, g_hold_ns / 1e6 / g_hold_n, (unsigned long long)g_hold_forced, (unsigned long long)g_hold_drained, (unsigned long long)g_hold_blocks, g_hold_blocks ? (double)g_hold_block_recs / g_hold_blocks : 0.0, (unsigned long long)g_rc_ahead_waits, (unsigned long long)g_hold_ap, g_hold_ab / 1048576.0, (unsigned long long)g_hold_ahead_holds_w, (unsigned long long)g_hold_ahead_holds);
    pthread_mutex_lock(&g_hold_mu);
    atomic_store(&g_hold_stuck, 0);
    g_hold_idle = 0;
    atomic_store_explicit(&g_hold_on, 0, memory_order_release);
    pthread_cond_broadcast(&g_hold_cv);
    pthread_mutex_unlock(&g_hold_mu);
    return 0;
}
static int hold_req_now(void) {
    uint64_t rq = atomic_load(&g_hold_req);
    return rq >> 2 == atomic_load(&g_hold_gen) && (rq & 3);
}
static void hold_op(void (^op)(void)) {
    if (!hold_check()) {
        op();
        return;
    }
    if (!g_hold_ops) g_hold_ops = [NSMutableArray new];
    [g_hold_ops addObject:[op copy]];
}

static id g_rx_cb, g_rx_enc;
// Submissions committed together (DM_NVN_SUB_MERGE): while the GPU runs an earlier main command buffer, a submission's ended command buffer is held and the next ones join it in one
// commit, keeping their place among other queue work (docs/reference/frame-rate.md#the-gpu-idles-with-the-next-command-buffer-already-committed). Not merged into one command
// buffer: an upload batch recorded inside a submission would follow its draws. GPU idleness is read from g_rx_ev, signalled after each commit
enum { RX_PEND_MAX = 48 };
enum { REC_CMT_NOW = 0, REC_CMT_END = 1, REC_CMT_SEG = 2 };   // RO_COMMIT a[5]: commit now, submission end, or segment cut (held while rx_inflight >= cut_busy_limit)
static id g_rx_held_cb[RX_PEND_MAX];   // oldest first; stamp: heap slot of the boundary after it (-1 none)
static DmCommitFB *g_rx_fb[RX_PEND_MAX];
static id g_rx_stamp_heap[RX_PEND_MAX];
static int g_rx_stamp[RX_PEND_MAX], g_rx_npend;
static int g_rx_after, g_rx_want_stamp;
static uint64_t g_rx_ubc;   // newest uniform shadow the held ones read
static struct { uint64_t v; _Atomic uint64_t *p; } g_rx_post[2 * RX_PEND_MAX + 4];   // signals and notes after held commits: made after the real one, in order
static id g_rx_post_ev[2 * RX_PEND_MAX + 4];
static int g_rx_npost;
static _Atomic int g_rx_fly, g_rx_held, g_rx_drain, g_rx_busy_hold;   // fly: committed, not reported back; held; drain: rec_drain callers waiting; busy_hold: tests
static id<MTLSharedEvent> g_rx_ev;   // ++g_rx_seq after every commit on g_q4
static _Atomic uint64_t g_rx_seq, g_rx_need_fence;   // g_rx_need_fence: highest fence a blocking waiter needs committed
static uint64_t g_rx_merged, g_rx_cbs;
static int g_rx_last_nb;
static int sw_mode(const char *name, int dflt);
static inline int sw_on(int m);
static int rx_merge_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_SUB_MERGE");
        v = e && !strncmp(e, "ab", 2) ? sw_mode("DM_NVN_SUB_MERGE", 0) : !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_SUB_MERGE=0: every submission is committed on its own");
    }
    return sw_on(v);
}
static void rx_mark_locked(void) {   // under g_res_commit_mu
    id<MTLSharedEvent> ev = g_rx_ev;
    if (!ev) {
        ev = [g_dev newSharedEvent];
        if (!ev) MDIE("failed to create the Metal 4 queue progress event");
        g_rx_ev = ev;
    }
    uint64_t s = atomic_load_explicit(&g_rx_seq, memory_order_relaxed) + 1;
    atomic_store_explicit(&g_rx_seq, s, memory_order_release);
    [g_q4 signalEvent:ev value:s];
}
static void rx_mark(void) {
    pthread_mutex_lock(&g_res_commit_mu);
    rx_mark_locked();
    pthread_mutex_unlock(&g_res_commit_mu);
}
static uint64_t rx_inflight(void) {
    static int fbm = -1;
    if (fbm < 0) fbm = sw_mode("DM_NVN_SUB_IDLE_FB", 0);
    uint64_t hold = (uint64_t)atomic_load_explicit(&g_rx_busy_hold, memory_order_relaxed);
    if (fbm && sw_on(fbm)) {
        int fly = rec_on() ? atomic_load_explicit(&g_rx_fly, memory_order_relaxed) : atomic_load_explicit(&g_seg_inflight, memory_order_relaxed);
        return hold + (fly > 0 ? (uint64_t)fly : 0);
    }
    uint64_t seq = atomic_load_explicit(&g_rx_seq, memory_order_acquire);
    if (!seq) return hold;
    uint64_t done = g_rx_ev.signaledValue;
    return hold + (seq > done ? seq - done : 0);
}
static inline uint64_t rx_hold_limit(void) { int lim = cut_busy_limit(); return lim > 0 ? (uint64_t)lim : 1; }
static inline int rx_need_flush(void) {
    return atomic_load_explicit(&g_rx_drain, memory_order_acquire)
        || atomic_load_explicit(&g_rx_need_fence, memory_order_seq_cst) > atomic_load_explicit(&g_sub_cmt, memory_order_seq_cst);
}
static double rx_stamp_s(id heap, int slot) {   // s on the Mach clock, as commit feedback times; 0 = not written
    if (!heap || slot < 0) return 0;
    NSData *d = [(id<MTL4CounterHeap>)heap resolveCounterRange:NSMakeRange((NSUInteger)slot, 1)];
    const MTL4TimestampHeapEntry *en = d.bytes;
    if (!en || d.length < sizeof *en || !en->timestamp) return 0;
    MTLTimestamp cpu = 0, gpu = 0;
    [g_dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
    static double scale;
    if (!scale) {
        uint64_t hz = [g_dev queryTimestampFrequency];
        scale = hz ? 1e9 / (double)hz : 1;
    }
    double ago = (double)gpu - (double)en->timestamp * scale;
    return cpu && ago >= 0 && ago < 2e9 ? ((double)cpu - ago) / 1e9 : 0;
}
// held command buffers, then (cur) the encode thread's own; the held ones' feedback first
static void rx_commit(int cur, DmCommitFB *own, uint64_t ubc) {
    int n = g_rx_npend, nb = n + (cur != 0), cnt = nb;
    if (!nb) return;
    id<MTL4CommandBuffer> buffers[RX_PEND_MAX + 1];
    NSMutableArray *fbs = [NSMutableArray arrayWithCapacity:(NSUInteger)cnt], *heaps = [NSMutableArray arrayWithCapacity:(NSUInteger)(n ? n : 1)];
    NSMutableData *st = [NSMutableData dataWithLength:(NSUInteger)(n ? n : 1) * sizeof(int)];
    int *sl = st.mutableBytes;
    for (int i = 0; i < n; i++) {
        buffers[i] = g_rx_held_cb[i];
        [fbs addObject:g_rx_fb[i]];
        [heaps addObject:g_rx_stamp_heap[i] ?: (id)NSNull.null];
        sl[i] = g_rx_stamp[i];
        g_rx_held_cb[i] = nil;
        g_rx_fb[i] = nil;
        g_rx_stamp_heap[i] = nil;
    }
    if (cur) {
        buffers[n] = g_rx_cb;
        [fbs addObject:own ?: (id)NSNull.null];
    }
    if (g_rx_ubc > ubc) ubc = g_rx_ubc;
    g_rx_ubc = 0;
    g_rx_npend = 0;
    g_rx_want_stamp = 0;
    atomic_store(&g_rx_held, 0);
    ubc_wait(ubc);
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    atomic_fetch_add(&g_rx_fly, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        atomic_fetch_sub(&g_rx_fly, 1);
        if (cnt == 1) {
            if (fbs[0] != NSNull.null) for (void (^b)(id<MTL4CommitFeedback>) in ((DmCommitFB *)fbs[0])->blocks) b(f);
            return;
        }
        double t[RX_PEND_MAX + 2];
        const int *ss = st.bytes;
        t[0] = f.GPUStartTime;
        t[cnt] = f.GPUEndTime;
        for (int i = 1; i < cnt; i++) {
            double at = i - 1 < n && heaps[(NSUInteger)(i - 1)] != NSNull.null ? rx_stamp_s(heaps[(NSUInteger)(i - 1)], ss[i - 1]) : 0;
            t[i] = at > 0 && at >= t[0] && at <= t[cnt] ? at : -1;
        }
        for (int i = cnt - 1; i >= 1; i--) if (t[i] < 0 || t[i] > t[i + 1]) t[i] = t[i + 1];   // no stamp (an empty encoder writes none): ends at the next known boundary
        for (int i = 0; i < cnt; i++) {
            if (fbs[(NSUInteger)i] == NSNull.null) continue;
            DmPartFB *part = [DmPartFB new];
            part->t0 = t[i];
            part->t1 = t[i + 1];
            part->err = f.error;
            for (void (^b)(id<MTL4CommitFeedback>) in ((DmCommitFB *)fbs[(NSUInteger)i])->blocks) b((id<MTL4CommitFeedback>)part);
        }
    }];
    // kept by a replay-ahead hold until the game's signal, with what followed
    NSMutableArray *bl = [NSMutableArray arrayWithCapacity:(NSUInteger)nb];
    for (int i = 0; i < nb; i++) [bl addObject:buffers[i]];
    int npost = g_rx_npost;
    NSMutableArray *pev = [NSMutableArray arrayWithCapacity:(NSUInteger)(npost ? npost : 1)];
    NSData *pv = [NSData dataWithBytes:g_rx_post length:(NSUInteger)npost * sizeof *g_rx_post];
    for (int i = 0; i < npost; i++) {
        [pev addObject:g_rx_post_ev[i] ?: (id)NSNull.null];
        g_rx_post_ev[i] = nil;
    }
    g_rx_npost = 0;
    hold_op(^{
        id<MTL4CommandBuffer> qb[RX_PEND_MAX + 1];
        for (int i = 0; i < nb; i++) qb[i] = bl[(NSUInteger)i];
        pthread_mutex_lock(&g_res_commit_mu);
        [g_q4 commit:qb count:(NSUInteger)nb options:o];
        rx_mark_locked();
        pthread_mutex_unlock(&g_res_commit_mu);
        const typeof(*g_rx_post) *ps = pv.bytes;
        for (int i = 0; i < npost; i++) {
            if (pev[(NSUInteger)i] != NSNull.null) {
                [g_q4 signalEvent:pev[(NSUInteger)i] value:ps[i].v];
                continue;
            }
            _Atomic uint64_t *np = ps[i].p;
            pthread_mutex_lock(&g_note_mu);
            if (ps[i].v > atomic_load(np)) atomic_store(np, ps[i].v);
            pthread_cond_broadcast(&g_note_cv);
            pthread_mutex_unlock(&g_note_mu);
        }
    });
    if (cur) g_rx_cb = nil;
    g_rx_cbs++;
    g_rx_merged += (uint64_t)n;
    g_rx_last_nb = nb;
    if (g_rx_cbs % 20000 == 0) MLOG("held submissions: %llu commits of main command buffers so far, %llu submissions committed with a later one (DM_NVN_SUB_MERGE)", (unsigned long long)g_rx_cbs, (unsigned long long)g_rx_merged);
}
static void rx_flush(void) { if (g_rx_npend) rx_commit(0, nil, 0); }
static void rec_exec(const rec_t *r) {
    const uint64_t *a = r->a;
    id e = g_rx_enc;
    int after = g_rx_after;
    if (r->op != RO_SIGNAL && r->op != RO_NOTE) g_rx_after = 0;
    switch (r->op) {
    case RO_CB_NEW: g_rx_cb = [g_dev newCommandBuffer]; break;
    case RO_CB_BEGIN: [(id<MTL4CommandBuffer>)g_rx_cb beginCommandBufferWithAllocator:rec_id(a[0])]; break;
    case RO_CB_END: cb_end(g_rx_cb, nil); break;
    case RO_CB_BLOCK: ((void (^)(id<MTL4CommandBuffer>))rec_id(a[0]))(g_rx_cb); break;
    case RO_COMMIT: {
        DmCommitFB *fb = rec_id(a[0]);
        int mode = (int)a[5], lim = cut_busy_limit();
        uint64_t cap = lim > 0 ? (uint64_t)lim : 1;
        int can_hold = (mode == REC_CMT_END && rx_inflight() >= cap) || (mode == REC_CMT_SEG && lim > 0 && rx_inflight() >= (uint64_t)lim);
        if (can_hold && fb && g_rx_npend < RX_PEND_MAX && !atomic_load(&g_rx_drain) && rx_merge_on()) {
            g_rx_held_cb[g_rx_npend] = g_rx_cb;
            g_rx_fb[g_rx_npend] = fb;
            g_rx_stamp[g_rx_npend] = -1;
            g_rx_stamp_heap[g_rx_npend] = nil;
            g_rx_npend++;
            if (a[2] > g_rx_ubc) g_rx_ubc = a[2];
            g_rx_cb = nil;
            g_rx_after = mode == REC_CMT_END;
            g_rx_want_stamp = 1;
            atomic_store(&g_rx_held, 1);
            break;
        }
        rx_commit(1, fb, a[2]);
        break;
    }
    case RO_COMMIT_REAL: {   // a whole command buffer after everything recorded before it
        rx_flush();
        id<MTL4CommandBuffer> cb = rec_id(a[1]);
        MTL4CommitOptions *o = rec_id(a[0]);
        hold_op(^{
            id<MTL4CommandBuffer> buffers[] = {cb};
            pthread_mutex_lock(&g_res_commit_mu);   // apart from the residency set's commit
            if (o) [g_q4 commit:buffers count:1 options:o];
            else [g_q4 commit:buffers count:1];
            rx_mark_locked();
            pthread_mutex_unlock(&g_res_commit_mu);
        });
        break;
    }
    case RO_SIGNAL: {
        if (after && g_rx_npend && g_rx_npost < (int)(sizeof g_rx_post / sizeof *g_rx_post)) {
            g_rx_post_ev[g_rx_npost] = rec_id(a[0]);
            g_rx_post[g_rx_npost].v = a[1];
            g_rx_post[g_rx_npost++].p = NULL;
            g_rx_after = 1;
            break;
        }
        rx_flush();
        id ev = rec_id(a[0]);
        uint64_t v = a[1];
        hold_op(^{ [g_q4 signalEvent:ev value:v]; });
        break;
    }
    case RO_Q_BLOCK: rx_flush(); hold_op((void (^)(void))rec_id(a[0])); break;
    case RO_RE_BEGIN: g_rx_enc = [(id<MTL4CommandBuffer>)g_rx_cb renderCommandEncoderWithDescriptor:rec_id(a[0])]; break;
    case RO_CE_BEGIN: g_rx_enc = [(id<MTL4CommandBuffer>)g_rx_cb computeCommandEncoder]; break;
    case RO_END:
        [(id<MTL4CommandEncoder>)e endEncoding];
        g_rx_enc = nil;
        g_rx_want_stamp = 0;
        if (g_rx_npend && (atomic_load(&g_rx_drain) || rx_inflight() < rx_hold_limit())) rx_flush();
        break;
    case RO_BAR_QUEUE: [(id<MTL4CommandEncoder>)e barrierAfterQueueStages:a[0] beforeStages:a[1] visibilityOptions:a[2]]; break;
    case RO_BAR_AFTER_STAGES: [(id<MTL4CommandEncoder>)e barrierAfterStages:a[0] beforeQueueStages:a[1] visibilityOptions:a[2]]; break;
    case RO_BAR_ENCODER: [(id<MTL4CommandEncoder>)e barrierAfterEncoderStages:a[0] beforeEncoderStages:a[1] visibilityOptions:a[2]]; break;
    case RO_R_TABLE: [(id<MTL4RenderCommandEncoder>)e setArgumentTable:rec_id(a[0]) atStages:a[1]]; break;
    case RO_R_PSO: [(id<MTL4RenderCommandEncoder>)e setRenderPipelineState:rec_id(a[0])]; break;
    case RO_R_DSS: [(id<MTL4RenderCommandEncoder>)e setDepthStencilState:rec_id(a[0])]; break;
    case RO_R_VIEWPORT: { MTLViewport v; memcpy(&v, a, sizeof v); [(id<MTL4RenderCommandEncoder>)e setViewport:v]; break; }
    case RO_R_SCISSOR: { MTLScissorRect s; memcpy(&s, a, sizeof s); [(id<MTL4RenderCommandEncoder>)e setScissorRect:s]; break; }
    case RO_R_CULL: [(id<MTL4RenderCommandEncoder>)e setCullMode:a[0]]; break;
    case RO_R_WINDING: [(id<MTL4RenderCommandEncoder>)e setFrontFacingWinding:a[0]]; break;
    case RO_R_FILL: [(id<MTL4RenderCommandEncoder>)e setTriangleFillMode:a[0]]; break;
    case RO_R_BIAS: [(id<MTL4RenderCommandEncoder>)e setDepthBias:rec_fv(a[0]) slopeScale:rec_fv(a[1]) clamp:rec_fv(a[2])]; break;
    case RO_R_CLIP: [(id<MTL4RenderCommandEncoder>)e setDepthClipMode:a[0]]; break;
    case RO_R_SREF: [(id<MTL4RenderCommandEncoder>)e setStencilFrontReferenceValue:(uint32_t)a[0] backReferenceValue:(uint32_t)a[1]]; break;
    case RO_R_BLEND: [(id<MTL4RenderCommandEncoder>)e setBlendColorRed:rec_fv(a[0]) green:rec_fv(a[1]) blue:rec_fv(a[2]) alpha:rec_fv(a[3])]; break;
    case RO_R_VIS: [(id<MTL4RenderCommandEncoder>)e setVisibilityResultMode:a[0] offset:a[1]]; break;
    case RO_R_AMP: { MTLVertexAmplificationViewMapping m = {(uint32_t)a[1], (uint32_t)a[2]}; [(id<MTL4RenderCommandEncoder>)e setVertexAmplificationCount:a[0] viewMappings:a[0] ? &m : NULL]; break; }   // count 0 or 1 (slice-merged passes)
    case RO_R_CMAP: [(id<MTL4RenderCommandEncoder>)e setColorAttachmentMap:(__bridge MTLLogicalToPhysicalColorAttachmentMap *)(void *)(uintptr_t)a[0]]; break;   // a g_fsp_map, kept for the process
    case RO_R_DRAW: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] vertexStart:a[1] vertexCount:a[2]]; break;
    case RO_R_DRAW_INST: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] vertexStart:a[1] vertexCount:a[2] instanceCount:a[3] baseInstance:a[4]]; break;
    case RO_R_DRAW_IND: [(id<MTL4RenderCommandEncoder>)e drawPrimitives:a[0] indirectBuffer:a[1]]; break;
    case RO_R_DRAW_IDX: [(id<MTL4RenderCommandEncoder>)e drawIndexedPrimitives:a[0] indexCount:a[1] indexType:a[2] indexBuffer:a[3] indexBufferLength:a[4] instanceCount:a[5] baseVertex:(NSInteger)a[6] baseInstance:a[7]]; break;
    case RO_R_DRAW_IDX_IND: [(id<MTL4RenderCommandEncoder>)e drawIndexedPrimitives:a[0] indexType:a[1] indexBuffer:a[2] indexBufferLength:a[3] indirectBuffer:a[4]]; break;
    case RO_R_MESH: [(id<MTL4RenderCommandEncoder>)e drawMeshThreadgroups:MTLSizeMake(a[0], 1, 1) threadsPerObjectThreadgroup:MTLSizeMake(a[2] ? a[2] : 1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(a[1], 1, 1)]; break;
    case RO_R_MESH_IND: [(id<MTL4RenderCommandEncoder>)e drawMeshThreadgroupsWithIndirectBuffer:a[0] threadsPerObjectThreadgroup:MTLSizeMake(a[2] ? a[2] : 1, 1, 1) threadsPerMeshThreadgroup:MTLSizeMake(a[1], 1, 1)]; break;
    case RO_R_STORE_C: [(id<MTL4RenderCommandEncoder>)e setColorStoreAction:a[0] atIndex:a[1]]; break;
    case RO_R_STORE_D: [(id<MTL4RenderCommandEncoder>)e setDepthStoreAction:a[0]]; break;
    case RO_R_STORE_S: [(id<MTL4RenderCommandEncoder>)e setStencilStoreAction:a[0]]; break;
    case RO_C_TABLE: [(id<MTL4ComputeCommandEncoder>)e setArgumentTable:rec_id(a[0])]; break;
    case RO_C_PSO: [(id<MTL4ComputeCommandEncoder>)e setComputePipelineState:rec_id(a[0])]; break;
    case RO_C_DISPATCH: [(id<MTL4ComputeCommandEncoder>)e dispatchThreadgroups:MTLSizeMake(a[0], a[1], a[2]) threadsPerThreadgroup:MTLSizeMake(a[3], a[4], a[5])]; break;
    case RO_C_DISPATCH_IND: [(id<MTL4ComputeCommandEncoder>)e dispatchThreadgroupsWithIndirectBuffer:a[0] threadsPerThreadgroup:MTLSizeMake(a[1], a[2], a[3])]; break;
    case RO_C_COPY_BB: [(id<MTL4ComputeCommandEncoder>)e copyFromBuffer:rec_id(a[0]) sourceOffset:a[2] toBuffer:rec_id(a[1]) destinationOffset:a[3] size:a[4]]; break;
    case RO_C_COPY_BT: [(id<MTL4ComputeCommandEncoder>)e copyFromBuffer:rec_id(a[0]) sourceOffset:a[2] sourceBytesPerRow:a[3] sourceBytesPerImage:a[4] sourceSize:MTLSizeMake(a[5], a[6], a[7])
                           toTexture:rec_id(a[1]) destinationSlice:a[8] destinationLevel:a[9] destinationOrigin:MTLOriginMake(a[10], a[11], a[12])]; break;
    case RO_C_COPY_BT_OPT: [(id<MTL4ComputeCommandEncoder>)e copyFromBuffer:rec_id(a[0]) sourceOffset:a[2] sourceBytesPerRow:a[3] sourceBytesPerImage:a[4] sourceSize:MTLSizeMake(a[5], a[6], a[7])
                           toTexture:rec_id(a[1]) destinationSlice:a[8] destinationLevel:a[9] destinationOrigin:MTLOriginMake(a[10], a[11], a[12]) options:(MTLBlitOption)a[13]]; break;
    case RO_C_COPY_TB: [(id<MTL4ComputeCommandEncoder>)e copyFromTexture:rec_id(a[0]) sourceSlice:a[2] sourceLevel:a[3] sourceOrigin:MTLOriginMake(a[4], a[5], a[6]) sourceSize:MTLSizeMake(a[7], a[8], a[9])
                           toBuffer:rec_id(a[1]) destinationOffset:a[10] destinationBytesPerRow:a[11] destinationBytesPerImage:a[12]]; break;
    case RO_C_COPY_TB_OPT: [(id<MTL4ComputeCommandEncoder>)e copyFromTexture:rec_id(a[0]) sourceSlice:a[2] sourceLevel:a[3] sourceOrigin:MTLOriginMake(a[4], a[5], a[6]) sourceSize:MTLSizeMake(a[7], a[8], a[9])
                           toBuffer:rec_id(a[1]) destinationOffset:a[10] destinationBytesPerRow:a[11] destinationBytesPerImage:a[12] options:(MTLBlitOption)a[13]]; break;
    case RO_C_COPY_TT: [(id<MTL4ComputeCommandEncoder>)e copyFromTexture:rec_id(a[0]) sourceSlice:a[2] sourceLevel:a[3] sourceOrigin:MTLOriginMake(a[4], a[5], a[6]) sourceSize:MTLSizeMake(a[7], a[8], a[9])
                           toTexture:rec_id(a[1]) destinationSlice:a[10] destinationLevel:a[11] destinationOrigin:MTLOriginMake(a[12], a[13], a[14])]; break;
    case RO_C_FILL: [(id<MTL4ComputeCommandEncoder>)e fillBuffer:rec_id(a[0]) range:NSMakeRange(a[1], a[2]) value:(uint8_t)a[3]]; break;
    case RO_T_ADDR: [(id<MTL4ArgumentTable>)rec_id(a[0]) setAddress:a[1] atIndex:a[2]]; break;
    case RO_T_TEX: [(id<MTL4ArgumentTable>)rec_id(a[0]) setTexture:(MTLResourceID){a[1]} atIndex:a[2]]; break;
    case RO_T_SAMP: [(id<MTL4ArgumentTable>)rec_id(a[0]) setSamplerState:(MTLResourceID){a[1]} atIndex:a[2]]; break;
    case RO_T_RESOURCE: [(id<MTL4ArgumentTable>)rec_id(a[0]) setResource:(MTLResourceID){a[1]} atBufferIndex:a[2]]; break;
    case RO_LABEL: [(id<MTL4CommandEncoder>)e setLabel:rec_id(a[0])]; break;
    case RO_R_TS: [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:a[1] afterStage:a[2] intoHeap:rec_id(a[0]) atIndex:a[3]];
        if (g_rx_want_stamp && g_rx_npend && rec_id(a[0]) == (id)g_ts_heap) {   // the submission's begin stamp ends the held one before it
            g_rx_stamp_heap[g_rx_npend - 1] = rec_id(a[0]);
            g_rx_stamp[g_rx_npend - 1] = (int)a[3];
            g_rx_want_stamp = 0;
        }
        break;
    case RO_C_TS: [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:a[1] intoHeap:rec_id(a[0]) atIndex:a[2]];
        if (g_rx_want_stamp && g_rx_npend && rec_id(a[0]) == (id)g_ts_heap) {
            g_rx_stamp_heap[g_rx_npend - 1] = rec_id(a[0]);
            g_rx_stamp[g_rx_npend - 1] = (int)a[2];
            g_rx_want_stamp = 0;
        }
        break;
    case RO_NOTE: {
        _Atomic uint64_t *p = (_Atomic uint64_t *)(uintptr_t)a[0];
        uint64_t v = a[1];
        if (after && g_rx_npend && g_rx_npost < (int)(sizeof g_rx_post / sizeof *g_rx_post)) {
            g_rx_post_ev[g_rx_npost] = nil;
            g_rx_post[g_rx_npost].v = v;
            g_rx_post[g_rx_npost++].p = p;
            g_rx_after = 1;
            break;
        }
        // in a hold: after the operations it follows
        hold_op(^{
            pthread_mutex_lock(&g_note_mu);
            if (v > atomic_load(p)) atomic_store(p, v);
            pthread_cond_broadcast(&g_note_cv);
            pthread_mutex_unlock(&g_note_mu);
        });
        break;
    }
    default: MDIE("deferred encoding: unknown record %u", r->op);
    }
    for (uint32_t i = 0; i < r->nobj; i++) if (a[i]) CFRelease((CFTypeRef)(uintptr_t)a[i]);
}
#include "nvn_rt.h"
static void *rec_thread(void *arg) {
    (void)arg;
    pthread_setname_np("dm-encode");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    void *rt = nvn_rt_join("encode thread");   // it makes the frame's Metal calls, so it declares the frame's deadline
    for (;;) {
        pthread_mutex_lock(&g_rb_mu);
        while (!g_rb_head) {
            if (g_rx_npend) {   // held commits, nothing to encode: wait briefly (every 50 us) while the GPU still has work
                for (int k = 0; k < 9 && !g_rb_head && !rx_need_flush() && rx_inflight() >= rx_hold_limit(); k++) {
                    struct timespec ts = {0, 50000};
                    g_rb_sleeping = 1;
                    pthread_cond_timedwait_relative_np(&g_rb_work, &g_rb_mu, &ts);
                    g_rb_sleeping = 0;
                }
                if (g_rb_head) break;
                g_rb_busy = 1;
                pthread_mutex_unlock(&g_rb_mu);
                @autoreleasepool { rx_flush(); }
                pthread_mutex_lock(&g_rb_mu);
                g_rb_busy = 0;
                if (!g_rb_head) pthread_cond_broadcast(&g_rb_idle);
                continue;
            }
            if (atomic_load(&g_hold_on)) {   // held: hold_poke wakes this thread; a store into a mapped event word is found by the poll
                pthread_mutex_unlock(&g_rb_mu);
                int held;
                @autoreleasepool { held = hold_check(); }
                pthread_mutex_lock(&g_rb_mu);
                if (g_rb_head) break;
                if (held) {
                    struct timespec tick = {0, clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - g_hold_t0 < 50000000ull ? 500000 : 5000000};   // a frame gate is signalled within ms; a long-parked queue is polled less
                    g_rb_sleeping = 1;
                    if (!g_rb_head && !hold_req_now()) pthread_cond_timedwait_relative_np(&g_rb_work, &g_rb_mu, &tick);
                    g_rb_sleeping = 0;
                }
                continue;
            }
            g_rb_sleeping = 1;
            pthread_cond_wait(&g_rb_work, &g_rb_mu);
            g_rb_sleeping = 0;
        }
        rec_blk *b = g_rb_head;
        g_rb_head = b->next;
        if (!g_rb_head) g_rb_tail = NULL;
        g_rb_busy = 1;
        pthread_mutex_unlock(&g_rb_mu);
        nvn_rt_tick(rt);
        @autoreleasepool {
            for (int i = 0; i < b->n; i++) {
                if (atomic_load_explicit(&g_hold_on, memory_order_relaxed) && g_hold_ops.count) hold_check();
                rec_exec(&b->r[i]);
            }
        }
        pthread_mutex_lock(&g_rb_mu);
        b->n = 0;
        b->next = g_rb_free;
        g_rb_free = b;
        g_rb_busy = 0;
        if (!g_rb_head) pthread_cond_broadcast(&g_rb_idle);
        pthread_mutex_unlock(&g_rb_mu);
    }
    return NULL;
}

static void rec_publish(void) {
    if (!g_rb_cur || !g_rb_cur->n) return;
    pthread_mutex_lock(&g_rb_mu);
    g_rb_cur->next = NULL;
    if (g_rb_tail) g_rb_tail->next = g_rb_cur;
    else g_rb_head = g_rb_cur;
    g_rb_tail = g_rb_cur;
    g_rb_cur = NULL;
    if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
    pthread_mutex_unlock(&g_rb_mu);
}
static rec_t *rec_put(uint32_t op, uint32_t nobj) {
    if (g_rb_cur && g_rb_cur->n == REC_BLK) rec_publish();
    if (!g_rb_cur) {
        pthread_mutex_lock(&g_rb_mu);
        if (!g_rb_started) {
            pthread_t th;
            pthread_attr_t at;
            pthread_attr_init(&at);
            pthread_attr_setstacksize(&at, 1 << 20);
            if (pthread_create(&th, &at, rec_thread, NULL)) MDIE("deferred encoding: cannot start the encode thread");
            pthread_attr_destroy(&at);
            g_rb_started = 1;
        }
        g_rb_cur = g_rb_free;
        if (g_rb_cur) g_rb_free = g_rb_cur->next;
        pthread_mutex_unlock(&g_rb_mu);
        if (!g_rb_cur) {
            g_rb_cur = malloc(sizeof *g_rb_cur);
            if (!g_rb_cur) MDIE("deferred encoding: out of memory");
        }
        g_rb_cur->n = 0;
    }
    rec_t *r = &g_rb_cur->r[g_rb_cur->n++];
    r->op = op;
    r->nobj = nobj;
    return r;
}
// the hold ends first, with the game's signal
static void hold_block(int any) {   // any: also with nothing held yet
    if (!atomic_load(&g_hold_on) || (!any && !atomic_load(&g_hold_recs))) return;
    rec_publish();
    g_hold_blocks++;
    g_hold_block_recs += atomic_load(&g_hold_recs);
    hold_stuck();
    pthread_mutex_lock(&g_hold_mu);
    while (atomic_load(&g_hold_on)) pthread_cond_wait(&g_hold_cv, &g_hold_mu);
    pthread_mutex_unlock(&g_hold_mu);
}
// Any thread: the wait is satisfied, or why held work must reach the GPU now; gen: the hold the caller saw
static void hold_poke(const char *why, uint32_t gen) {
    if (!atomic_load(&g_hold_on) || atomic_load(&g_hold_gen) != gen) return;
    uint64_t req = (uint64_t)gen << 2 | (!why ? 1 : why == HOLD_DRAIN ? 3 : 2), old = atomic_load(&g_hold_req);
    do { if (old >> 2 == gen && (old & 3)) return; } while (!atomic_compare_exchange_weak(&g_hold_req, &old, req));   // the first request for this hold stands
    if (why && why != HOLD_DRAIN) g_hold_force_why = why;
    pthread_mutex_lock(&g_rb_mu);
    if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
    pthread_mutex_unlock(&g_rb_mu);
}
// Hard drain (before a GPU wait or another queue): held operations are made first, waiting for the game's signal; a submission's end needs only rec_drain_soft
static void rec_drain_soft(void);
static void rec_drain(void) {
    if (on_sub_thread()) hold_block(0);
    rec_drain_soft();
}
static void rec_drain_soft(void) {
    if (!g_rb_started) return;
    rec_publish();
    uint64_t w0 = ws_t0();
    int blocked = 0;
    pthread_mutex_lock(&g_rb_mu);
    atomic_fetch_add(&g_rx_drain, 1);   // held commits go now
    if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
    while (g_rb_head || g_rb_busy || atomic_load(&g_rx_held)) {
        blocked = 1;
        struct timespec ts = {0, 2000000};
        pthread_cond_timedwait_relative_np(&g_rb_idle, &g_rb_mu, &ts);
        if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
    }
    atomic_fetch_sub(&g_rx_drain, 1);
    pthread_mutex_unlock(&g_rb_mu);
    if (ws_on()) ws_put(WS_DRAIN, w0, blocked);
}
// wake the encode thread so a held fence's command buffer goes now, not after the 50 us idle loop
static void rec_wake_for_fence(uint64_t fence) {
    if (!rec_on() || !g_rb_started || atomic_load_explicit(&g_sub_cmt, memory_order_acquire) >= fence) return;
    uint64_t cur = atomic_load_explicit(&g_rx_need_fence, memory_order_relaxed);
    while (cur < fence && !atomic_compare_exchange_weak_explicit(&g_rx_need_fence, &cur, fence, memory_order_seq_cst, memory_order_relaxed)) {}
    if (atomic_load_explicit(&g_rx_held, memory_order_seq_cst)) {
        pthread_mutex_lock(&g_rb_mu);
        if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
        pthread_mutex_unlock(&g_rb_mu);
    }
}
// Records from another thread (the present snapshot's copy): one block behind all published, so after the frame's work, without waiting
static rec_blk *rec_own_begin(void) {
    pthread_mutex_lock(&g_rb_mu);
    rec_blk *b = g_rb_free;
    if (b) g_rb_free = b->next;
    pthread_mutex_unlock(&g_rb_mu);
    if (!b) {
        b = malloc(sizeof *b);
        if (!b) MDIE("deferred encoding: out of memory");
    }
    b->n = 0;
    return b;
}
static rec_t *rec_own_put(rec_blk *b, uint32_t op, uint32_t nobj) {
    if (b->n == REC_BLK) MDIE("deferred encoding: an own block is full");
    rec_t *r = &b->r[b->n++];
    r->op = op;
    r->nobj = nobj;
    return r;
}
static void rec_own_push(rec_blk *b) {
    pthread_mutex_lock(&g_rb_mu);
    b->next = NULL;
    if (g_rb_tail) g_rb_tail->next = b;
    else g_rb_head = b;
    g_rb_tail = b;
    if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
    pthread_mutex_unlock(&g_rb_mu);
}
static __attribute__((noreturn)) void rec_missing(id o, SEL s) { MDIE("deferred encoding: %s has no stand-in for -%s (DM_NVN_ENC_THREAD=0 runs without it)", object_getClassName(o), sel_getName(s)); }

// stand-ins are kept for good, so the real table is passed without a retain
@interface DmRecTable : NSObject { @public id real; } @end
static inline void rec_table_op(uint32_t op, id t, uint64_t v, NSUInteger i) { rec_t *r = rec_put(op, 0); r->a[0] = (uint64_t)(uintptr_t)(__bridge void *)((DmRecTable *)t)->real; r->a[1] = v; r->a[2] = i; }
@implementation DmRecTable
- (void)setAddress:(uint64_t)gpu atIndex:(NSUInteger)i { rec_table_op(RO_T_ADDR, self, gpu, i); }
- (void)setTexture:(MTLResourceID)t atIndex:(NSUInteger)i { rec_table_op(RO_T_TEX, self, t._impl, i); }
- (void)setSamplerState:(MTLResourceID)t atIndex:(NSUInteger)i { rec_table_op(RO_T_SAMP, self, t._impl, i); }
- (void)setResource:(MTLResourceID)t atBufferIndex:(NSUInteger)i { rec_table_op(RO_T_RESOURCE,self,t._impl,i); }
- (id)forwardingTargetForSelector:(SEL)s { rec_missing(self, s); }
@end
static inline id rec_table_real(id t) { return [t isKindOfClass:[DmRecTable class]] ? ((DmRecTable *)t)->real : t; }

@interface DmRecEnc : NSObject @end
@implementation DmRecEnc
- (BOOL)conformsToProtocol:(Protocol *)p { return p == @protocol(MTL4CommandEncoder) || p == @protocol(MTL4RenderCommandEncoder) || p == @protocol(MTL4ComputeCommandEncoder) || [super conformsToProtocol:p]; }
- (id)forwardingTargetForSelector:(SEL)s { rec_missing(self, s); }
- (void)endEncoding { rec_put(RO_END, 0); }
- (void)barrierAfterQueueStages:(MTLStages)a beforeStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { rec_t *r = rec_put(RO_BAR_QUEUE, 0); r->a[0] = a; r->a[1] = b; r->a[2] = v; }
- (void)barrierAfterStages:(MTLStages)a beforeQueueStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { rec_t *r = rec_put(RO_BAR_AFTER_STAGES, 0); r->a[0] = a; r->a[1] = b; r->a[2] = v; }
- (void)barrierAfterEncoderStages:(MTLStages)a beforeEncoderStages:(MTLStages)b visibilityOptions:(MTL4VisibilityOptions)v { rec_t *r = rec_put(RO_BAR_ENCODER, 0); r->a[0] = a; r->a[1] = b; r->a[2] = v; }
- (void)setArgumentTable:(id)t atStages:(MTLRenderStages)s { id x = rec_table_real(t); int keep = x != t; rec_t *r = rec_put(RO_R_TABLE, !keep); r->a[0] = keep ? (uint64_t)(uintptr_t)(__bridge void *)x : rec_obj(x); r->a[1] = s; }
- (void)setRenderPipelineState:(id)p { rec_t *r = rec_put(RO_R_PSO, 1); r->a[0] = rec_obj(p); }
- (void)setDepthStencilState:(id)s { rec_t *r = rec_put(RO_R_DSS, 1); r->a[0] = rec_obj(s); }
- (void)setViewport:(MTLViewport)v { rec_t *r = rec_put(RO_R_VIEWPORT, 0); memcpy(r->a, &v, sizeof v); }
- (void)setScissorRect:(MTLScissorRect)s { rec_t *r = rec_put(RO_R_SCISSOR, 0); memcpy(r->a, &s, sizeof s); }
- (void)setCullMode:(MTLCullMode)m { rec_put(RO_R_CULL, 0)->a[0] = m; }
- (void)setFrontFacingWinding:(MTLWinding)w { rec_put(RO_R_WINDING, 0)->a[0] = w; }
- (void)setTriangleFillMode:(MTLTriangleFillMode)m { rec_put(RO_R_FILL, 0)->a[0] = m; }
- (void)setDepthBias:(float)u slopeScale:(float)s clamp:(float)c { rec_t *r = rec_put(RO_R_BIAS, 0); r->a[0] = rec_f(u); r->a[1] = rec_f(s); r->a[2] = rec_f(c); }
- (void)setDepthClipMode:(MTLDepthClipMode)m { rec_put(RO_R_CLIP, 0)->a[0] = m; }
- (void)setStencilReferenceValue:(uint32_t)v { rec_t *r = rec_put(RO_R_SREF, 0); r->a[0] = r->a[1] = v; }
- (void)setStencilFrontReferenceValue:(uint32_t)f backReferenceValue:(uint32_t)b { rec_t *r = rec_put(RO_R_SREF, 0); r->a[0] = f; r->a[1] = b; }
- (void)setBlendColorRed:(float)r_ green:(float)g blue:(float)b alpha:(float)a { rec_t *r = rec_put(RO_R_BLEND, 0); r->a[0] = rec_f(r_); r->a[1] = rec_f(g); r->a[2] = rec_f(b); r->a[3] = rec_f(a); }
- (void)setVisibilityResultMode:(MTLVisibilityResultMode)m offset:(NSUInteger)o { rec_t *r = rec_put(RO_R_VIS, 0); r->a[0] = m; r->a[1] = o; }
- (void)setVertexAmplificationCount:(NSUInteger)n viewMappings:(const MTLVertexAmplificationViewMapping *)m { rec_t *r = rec_put(RO_R_AMP, 0); r->a[0] = n; r->a[1] = n && m ? m[0].viewportArrayIndexOffset : 0; r->a[2] = n && m ? m[0].renderTargetArrayIndexOffset : 0; }
- (void)setColorAttachmentMap:(MTLLogicalToPhysicalColorAttachmentMap *)m { rec_put(RO_R_CMAP, 0)->a[0] = (uint64_t)(uintptr_t)(__bridge void *)m; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)v vertexCount:(NSUInteger)n { rec_t *r = rec_put(RO_R_DRAW, 0); r->a[0] = p; r->a[1] = v; r->a[2] = n; }
- (void)drawPrimitives:(MTLPrimitiveType)p vertexStart:(NSUInteger)v vertexCount:(NSUInteger)n instanceCount:(NSUInteger)i baseInstance:(NSUInteger)b {
    rec_t *r = rec_put(RO_R_DRAW_INST, 0); r->a[0] = p; r->a[1] = v; r->a[2] = n; r->a[3] = i; r->a[4] = b; }
- (void)drawPrimitives:(MTLPrimitiveType)p indirectBuffer:(uint64_t)g { rec_t *r = rec_put(RO_R_DRAW_IND, 0); r->a[0] = p; r->a[1] = g; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexCount:(NSUInteger)n indexType:(MTLIndexType)t indexBuffer:(uint64_t)g indexBufferLength:(NSUInteger)len instanceCount:(NSUInteger)i baseVertex:(NSInteger)bv baseInstance:(NSUInteger)bi {
    rec_t *r = rec_put(RO_R_DRAW_IDX, 0); r->a[0] = p; r->a[1] = n; r->a[2] = t; r->a[3] = g; r->a[4] = len; r->a[5] = i; r->a[6] = (uint64_t)bv; r->a[7] = bi; }
- (void)drawIndexedPrimitives:(MTLPrimitiveType)p indexType:(MTLIndexType)t indexBuffer:(uint64_t)g indexBufferLength:(NSUInteger)len indirectBuffer:(uint64_t)ind {
    rec_t *r = rec_put(RO_R_DRAW_IDX_IND, 0); r->a[0] = p; r->a[1] = t; r->a[2] = g; r->a[3] = len; r->a[4] = ind; }
- (void)drawMeshThreadgroups:(MTLSize)g threadsPerObjectThreadgroup:(MTLSize)o threadsPerMeshThreadgroup:(MTLSize)m { rec_t *r = rec_put(RO_R_MESH, 0); r->a[0] = g.width; r->a[1] = m.width; r->a[2] = o.width; }
- (void)drawMeshThreadgroupsWithIndirectBuffer:(MTLGPUAddress)ib threadsPerObjectThreadgroup:(MTLSize)o threadsPerMeshThreadgroup:(MTLSize)m { rec_t *r = rec_put(RO_R_MESH_IND, 0); r->a[0] = ib; r->a[1] = m.width; r->a[2] = o.width; }
- (void)setColorStoreAction:(MTLStoreAction)a atIndex:(NSUInteger)i { rec_t *r = rec_put(RO_R_STORE_C, 0); r->a[0] = a; r->a[1] = i; }
- (void)setDepthStoreAction:(MTLStoreAction)a { rec_put(RO_R_STORE_D, 0)->a[0] = a; }
- (void)setStencilStoreAction:(MTLStoreAction)a { rec_put(RO_R_STORE_S, 0)->a[0] = a; }
// Metal 4 copies are on the compute encoder
- (void)setArgumentTable:(id)t { id x = rec_table_real(t); int keep = x != t; rec_t *r = rec_put(RO_C_TABLE, !keep); r->a[0] = keep ? (uint64_t)(uintptr_t)(__bridge void *)x : rec_obj(x); }
- (void)setComputePipelineState:(id)p { rec_t *r = rec_put(RO_C_PSO, 1); r->a[0] = rec_obj(p); }
- (void)dispatchThreadgroups:(MTLSize)n threadsPerThreadgroup:(MTLSize)t { rec_t *r = rec_put(RO_C_DISPATCH, 0); r->a[0] = n.width; r->a[1] = n.height; r->a[2] = n.depth; r->a[3] = t.width; r->a[4] = t.height; r->a[5] = t.depth; }
- (void)dispatchThreadgroupsWithIndirectBuffer:(uint64_t)g threadsPerThreadgroup:(MTLSize)t { rec_t *r = rec_put(RO_C_DISPATCH_IND, 0); r->a[0] = g; r->a[1] = t.width; r->a[2] = t.height; r->a[3] = t.depth; }
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so toBuffer:(id)d destinationOffset:(NSUInteger)doff size:(NSUInteger)n {
    rec_t *r = rec_put(RO_C_COPY_BB, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = so; r->a[3] = doff; r->a[4] = n; }
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)img sourceSize:(MTLSize)z toTexture:(id)d destinationSlice:(NSUInteger)sl destinationLevel:(NSUInteger)lv destinationOrigin:(MTLOrigin)o {
    rec_t *r = rec_put(RO_C_COPY_BT, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = so; r->a[3] = row; r->a[4] = img; r->a[5] = z.width; r->a[6] = z.height; r->a[7] = z.depth;
    r->a[8] = sl; r->a[9] = lv; r->a[10] = o.x; r->a[11] = o.y; r->a[12] = o.z; }
- (void)copyFromBuffer:(id)s sourceOffset:(NSUInteger)so sourceBytesPerRow:(NSUInteger)row sourceBytesPerImage:(NSUInteger)img sourceSize:(MTLSize)z toTexture:(id)d destinationSlice:(NSUInteger)sl destinationLevel:(NSUInteger)lv destinationOrigin:(MTLOrigin)o options:(MTLBlitOption)opt {
    rec_t *r = rec_put(RO_C_COPY_BT_OPT, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = so; r->a[3] = row; r->a[4] = img; r->a[5] = z.width; r->a[6] = z.height; r->a[7] = z.depth;
    r->a[8] = sl; r->a[9] = lv; r->a[10] = o.x; r->a[11] = o.y; r->a[12] = o.z; r->a[13] = opt; }
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)sl sourceLevel:(NSUInteger)lv sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z toBuffer:(id)d destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)img {
    rec_t *r = rec_put(RO_C_COPY_TB, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = sl; r->a[3] = lv; r->a[4] = o.x; r->a[5] = o.y; r->a[6] = o.z; r->a[7] = z.width; r->a[8] = z.height; r->a[9] = z.depth;
    r->a[10] = doff; r->a[11] = row; r->a[12] = img; }
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)sl sourceLevel:(NSUInteger)lv sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z toBuffer:(id)d destinationOffset:(NSUInteger)doff destinationBytesPerRow:(NSUInteger)row destinationBytesPerImage:(NSUInteger)img options:(MTLBlitOption)opt {
    rec_t *r = rec_put(RO_C_COPY_TB_OPT, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = sl; r->a[3] = lv; r->a[4] = o.x; r->a[5] = o.y; r->a[6] = o.z; r->a[7] = z.width; r->a[8] = z.height; r->a[9] = z.depth;
    r->a[10] = doff; r->a[11] = row; r->a[12] = img; r->a[13] = opt; }
- (void)copyFromTexture:(id)s sourceSlice:(NSUInteger)sl sourceLevel:(NSUInteger)lv sourceOrigin:(MTLOrigin)o sourceSize:(MTLSize)z toTexture:(id)d destinationSlice:(NSUInteger)dsl destinationLevel:(NSUInteger)dlv destinationOrigin:(MTLOrigin)dorg {
    rec_t *r = rec_put(RO_C_COPY_TT, 2); r->a[0] = rec_obj(s); r->a[1] = rec_obj(d); r->a[2] = sl; r->a[3] = lv; r->a[4] = o.x; r->a[5] = o.y; r->a[6] = o.z; r->a[7] = z.width; r->a[8] = z.height; r->a[9] = z.depth;
    r->a[10] = dsl; r->a[11] = dlv; r->a[12] = dorg.x; r->a[13] = dorg.y; r->a[14] = dorg.z; }
- (void)fillBuffer:(id)b range:(NSRange)rg value:(uint8_t)v { rec_t *r = rec_put(RO_C_FILL, 1); r->a[0] = rec_obj(b); r->a[1] = rg.location; r->a[2] = rg.length; r->a[3] = v; }
- (void)setLabel:(NSString *)l { rec_put(RO_LABEL, 1)->a[0] = rec_obj(l); }
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)g afterStage:(MTLRenderStages)st intoHeap:(id)h atIndex:(NSUInteger)i { rec_t *r = rec_put(RO_R_TS, 1); r->a[0] = rec_obj(h); r->a[1] = g; r->a[2] = st; r->a[3] = i; }
- (void)writeTimestampWithGranularity:(MTL4TimestampGranularity)g intoHeap:(id)h atIndex:(NSUInteger)i { rec_t *r = rec_put(RO_C_TS, 1); r->a[0] = rec_obj(h); r->a[1] = g; r->a[2] = i; }
@end

@interface DmRecCB : NSObject @end
@implementation DmRecCB
- (id)forwardingTargetForSelector:(SEL)s { rec_missing(self, s); }
- (void)beginCommandBufferWithAllocator:(id)a { rec_put(RO_CB_BEGIN, 1)->a[0] = rec_obj(a); }
- (void)endCommandBuffer { rec_put(RO_CB_END, 0); }
- (id)renderCommandEncoderWithDescriptor:(MTL4RenderPassDescriptor *)d { rec_put(RO_RE_BEGIN, 1)->a[0] = rec_obj([d copy]); return [DmRecEnc new]; }   // copied: the replay may change the descriptor afterwards
- (id)computeCommandEncoder { rec_put(RO_CE_BEGIN, 0); return [DmRecEnc new]; }
@end

static id rec_cb_new(void) {
    if (!rec_on()) return [g_dev newCommandBuffer];
    rec_put(RO_CB_NEW, 0);
    return [DmRecCB new];
}
// work a framework encodes itself (MetalFX) needs the real command buffer, in order
static void rec_cb_block(id cb, void (^block)(id<MTL4CommandBuffer>)) {
    if ([cb isKindOfClass:[DmRecCB class]]) rec_put(RO_CB_BLOCK, 1)->a[0] = rec_obj([block copy]);
    else block(cb);
}
static id rec_table(id t) {
    if (!rec_on() || !t) return t;
    DmRecTable *s = [DmRecTable new];
    s->real = t;
    return s;
}
// committed once the uniform shadows queued so far are made
static void rec_commit(id cb, DmCommitFB *fb, int end) {
    if ([cb isKindOfClass:[DmRecCB class]]) {
        rec_t *r = rec_put(RO_COMMIT, 1);
        r->a[0] = rec_obj(fb);
        r->a[2] = ubc_head();
        r->a[5] = (uint64_t)end;
        atomic_fetch_add(&g_hold_recs, 1);
        return;
    }
    ubc_wait(ubc_head());
    id<MTL4CommandBuffer> buffers[] = {cb};
    MTL4CommitOptions *o = commit_opts(fb);
    if (o) [g_q4 commit:buffers count:1 options:o];
    else [g_q4 commit:buffers count:1];
    rx_mark();
}
// after what is recorded so far (it reads no uniform shadow)
static void rec_commit_upload(id cb, MTL4CommitOptions *o) {
    if (rec_on() && g_rb_started) {
        rec_t *r = rec_put(RO_COMMIT_REAL, 2);
        r->a[0] = rec_obj(o);
        r->a[1] = rec_obj(cb);
        atomic_fetch_add(&g_hold_recs, 1);
        return;
    }
    id<MTL4CommandBuffer> buffers[] = {cb};
    if (o) [g_q4 commit:buffers count:1 options:o];
    else [g_q4 commit:buffers count:1];
    rx_mark();
}
// a queue operation (sparse mapping update) in its place among the recorded commits
static void rec_queue_block(void (^block)(void)) {
    if (!rec_on() || !g_rb_started) {
        block();
        return;
    }
    rec_put(RO_Q_BLOCK, 1)->a[0] = rec_obj([block copy]);
    atomic_fetch_add(&g_hold_recs, 1);
}
static void rec_signal(id ev, uint64_t v) {
    if (!rec_on() || !g_rb_started) {
        [g_q4 signalEvent:ev value:v];
        return;
    }
    rec_t *r = rec_put(RO_SIGNAL, 1);
    r->a[0] = rec_obj(ev);
    r->a[1] = v;
    atomic_fetch_add(&g_hold_recs, 1);
}
// submit thread, idle between submissions
static int hold_begin(volatile uint32_t *word, unsigned mode, uint32_t value, uint64_t first_fence) {
    if (!rec_on() || !g_rb_started || atomic_load(&g_hold_on)) return 0;
    rec_drain_soft();
    g_hold_word = word;
    g_hold_mode = mode;
    g_hold_value = value;
    g_hold_first = first_fence;
    g_hold_t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    atomic_store(&g_hold_recs, 0);
    atomic_store(&g_hold_stuck, 0);
    atomic_fetch_add(&g_hold_gen, 1);
    nvn_dirty_ahead_stats(&g_hold_ap_base, &g_hold_ab_base, NULL);
    nvn_dirty_ahead_watch(1);   // from here to the signal, count the guest writes the ahead translation could miss
    atomic_store_explicit(&g_hold_on, 1, memory_order_release);
    pthread_mutex_lock(&g_rb_mu);   // the encode thread polls from now, also with nothing recorded
    if (g_rb_sleeping) pthread_cond_signal(&g_rb_work);
    pthread_mutex_unlock(&g_rb_mu);
    return 1;
}
