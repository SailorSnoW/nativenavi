// A submission on the GPU: timing, runtime switches, GPU captures, CPU-side writes at completion, the commit, replay ahead, SyncWait and Finish.
// Mach absolute time, which Metal's GPU and CPU timestamps are on (CLOCK_MONOTONIC also counts the Mac's sleep).
static uint64_t bp_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint64_t g_bp_t0;
__attribute__((constructor)) static void bp_t0(void) { g_bp_t0 = bp_now(); }
static int bootprof(void) { static int v = -1; if (v < 0) v = getenv("DM_BOOTPROF") != NULL; return v; }
static uint64_t g_bp_lib_n, g_bp_lib_ns, g_bp_pso_n, g_bp_pso_ns, g_bp_up_bytes, g_bp_first_present;
static uint64_t g_up_total_ns;
#define GRING 1024
static struct { _Atomic uint64_t frame, ns, t0, t1; _Atomic int pending; } g_gpu[GRING];   // t0 / t1: earliest GPUStart / latest GPUEnd (mach absolute time, ns)
// DM_NVN_UB: alternating phases in one process, so the power / frequency drift cancels; each phase discards its first 45 frames.
static int g_ub_n = -1, g_ub_N = 180;
static _Atomic int g_ub_cur = -1;
static char g_ub_name[16][16], g_ub_keys[16][16][48];
static int g_ub_nk[16];
static _Atomic int g_ubp_on, g_ubp_c[4];
static _Atomic uint64_t g_ubp_sum_us;
void mtl_ub_present(double iv) {   // -1 dropped, 0 no reading
    if (!atomic_load(&g_ubp_on) || iv == 0) return;
    if (iv < 0) { atomic_fetch_add(&g_ubp_c[3], 1); return; }
    atomic_fetch_add(&g_ubp_c[iv <= 20.8 ? 0 : iv <= 29.2 ? 1 : 2], 1);
    atomic_fetch_add(&g_ubp_sum_us, (uint64_t)(iv * 1000));
}
// registered here: found even when a stripped build's symbols are not in the dynamic symbol table
static int *ub_var(const char *nm) {
    extern int g_dm_present_link;
    static const struct { const char *n; int *p; } t[] = { {"g_dm_present_link", &g_dm_present_link} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(nm, t[i].n)) return t[i].p;
    return dlsym(RTLD_DEFAULT, nm);
}
static void ub_frame(uint64_t f) {
    if (g_ub_n < 0) {
        g_ub_n = 0;
        const char *e = getenv("DM_NVN_UB"), *n = getenv("DM_NVN_UB_N");
        if (n && atoi(n) >= 60) g_ub_N = atoi(n);
        for (const char *q = e; q && *q && g_ub_n < 16; ) {
            const char *eq = strchr(q, '='), *end = strchr(q, ';');
            if (!end) end = q + strlen(q);
            if (!eq || eq > end) break;
            int c = g_ub_n++;
            size_t l = (size_t)(eq - q);
            if (l > 15) l = 15;
            memcpy(g_ub_name[c], q, l);
            for (const char *k = eq + 1; k < end && g_ub_nk[c] < 16; ) {
                size_t kl = 0;
                while (k + kl < end && k[kl] != ',') kl++;
                size_t cl = kl > 47 ? 47 : kl;
                memcpy(g_ub_keys[c][g_ub_nk[c]++], k, cl);
                k += kl;
                if (k < end) k++;
            }
            q = *end ? end + 1 : end;
        }
        if (g_ub_n) MLOG("DM_NVN_UB: %d categories, %d frames per phase (measured alternately in one process; phases that drop draws render wrong)", g_ub_n, g_ub_N);
    }
    if (!g_ub_n) return;
    uint64_t ph = f / (uint64_t)g_ub_N, r = f % (uint64_t)g_ub_N;
    if (r == 45) {
        for (int i = 0; i < 4; i++) atomic_store(&g_ubp_c[i], 0);
        atomic_store(&g_ubp_sum_us, 0);
        atomic_store(&g_ubp_on, 1);
    }
    if (r != 0) return;
    if (f && atomic_load(&g_ubp_on)) {
        atomic_store(&g_ubp_on, 0);
        uint64_t pp = ph - 1;
        int pc = (pp & 1) ? (int)((pp / 2) % (uint64_t)g_ub_n) : -1;
        int c0 = atomic_load(&g_ubp_c[0]), c1 = atomic_load(&g_ubp_c[1]), c2 = atomic_load(&g_ubp_c[2]), c3 = atomic_load(&g_ubp_c[3]), n_ = c0 + c1 + c2;
        MLOG("UB present %llu %s frames %d mean %.3f ms 2 ticks %d 3 ticks %d 4+ ticks %d discarded %d", (unsigned long long)pp, pc < 0 ? "base" : g_ub_name[pc], n_, n_ ? atomic_load(&g_ubp_sum_us) / 1000.0 / n_ : 0.0, c0, c1, c2, c3);
    }
    int nc = (ph & 1) ? (int)((ph / 2) % (uint64_t)g_ub_n) : -1;
    static int *sp[16];
    static int sv[16], ns;
    for (int i = 0; i < ns; i++) *sp[i] = sv[i];
    ns = 0;
    for (int i = 0; nc >= 0 && i < g_ub_nk[nc] && ns < 16; i++) {
        const char *k = g_ub_keys[nc][i];
        if (strncmp(k, "@set:", 5)) continue;
        char nm[48];
        const char *eq = strchr(k, '=');
        if (!eq) continue;
        size_t l = (size_t)(eq - (k + 5));
        if (l >= sizeof nm) continue;
        memcpy(nm, k + 5, l);
        nm[l] = 0;
        int *a_ = ub_var(nm);
        static int bad, ok;
        if (!a_) {
            if (bad++ < 8) MLOG("[RED] DM_NVN_UB: global variable not found: %s (on a device, register it in the ub_var table first)", nm);
            continue;
        }
        sp[ns] = a_; sv[ns] = *a_; ns++;
        *a_ = atoi(eq + 1);
        if (ok++ < 8) MLOG("DM_NVN_UB: %s = %d (was %d)", nm, atoi(eq + 1), sv[ns - 1]);
    }
    atomic_store(&g_ub_cur, nc);
}
static int ub_skip(const char *key) {
    int uc = atomic_load(&g_ub_cur);
    if (uc < 0) return 0;
    for (int i = 0; i < g_ub_nk[uc]; i++) {
        const char *k = g_ub_keys[uc][i];
        if (k[0] == '@') continue;
        if (k[0] == '*' || !strncmp(key, k, strlen(k))) return 1;
    }
    return 0;
}
static long ab_len(void); static void ab_frame(uint64_t f);
// DM_GPU_CAPTURE: captures the Metal 4 queue only (Metal refuses to capture a Metal 4 device); needs MTL_CAPTURE_ENABLED=1.
static long g_cap_f0 = -2, g_cap_n; static int g_cap_on;
static NSString *gpu_capture_dir(void) {
    const char *e = getenv("DM_GPU_CAPTURE_DIR");
    NSString *dir;
    if (e && *e) dir = @(e);
    else {
        char cwd[1024];
        if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, ".");
        dir = [@(cwd) stringByAppendingPathComponent:@"gpu_captures"];
    }
    NSError *err = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&err]) {
        MLOG("[RED] DM_GPU_CAPTURE: cannot make the folder %s: %s", dir.UTF8String, err.localizedDescription.UTF8String);
        return nil;
    }
    return dir;
}
static void gpu_capture_prune(NSString *dir) {
    const char *e = getenv("DM_GPU_CAPTURE_KEEP");
    long keep = e ? atol(e) : 2;
    if (keep < 1) keep = 1;
    NSFileManager *fm = NSFileManager.defaultManager; NSMutableArray<NSURL *> *traces = [NSMutableArray array];
    for (NSURL *u in [fm contentsOfDirectoryAtURL:[NSURL fileURLWithPath:dir isDirectory:YES] includingPropertiesForKeys:@[NSURLContentModificationDateKey] options:0 error:nil]) {
        NSString *n = u.lastPathComponent;
        if ([n hasPrefix:@"dm_"] && [n hasSuffix:@".gputrace"]) [traces addObject:u];
    }
    [traces sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) {
        NSDate *da = nil, *db = nil;
        [a getResourceValue:&da forKey:NSURLContentModificationDateKey error:nil];
        [b getResourceValue:&db forKey:NSURLContentModificationDateKey error:nil];
        return [db ?: NSDate.distantPast compare:da ?: NSDate.distantPast];
    }];
    for (NSUInteger i = (NSUInteger)keep - 1; i < traces.count; i++)
        if ([fm removeItemAtURL:traces[i] error:nil]) MLOG("DM_GPU_CAPTURE: removed %s (older captures beyond DM_GPU_CAPTURE_KEEP=%ld)", traces[i].lastPathComponent.UTF8String, keep);
}
static void gpu_capture_frame(uint64_t f) {
    static char last[32];
    {
        const char *e = getenv("DM_GPU_CAPTURE");
        if (!g_cap_on && e && strncmp(e, last, sizeof last - 1)) { snprintf(last, sizeof last, "%s", e); g_cap_f0 = -2; }
    }
    static double gpu_thr;
    static long after;
    static int over;
    if (g_cap_f0 == -2) {
        const char *e = getenv("DM_GPU_CAPTURE");
        gpu_thr = 0;
        over = 0;
        if (e && !strncmp(e, "gpu:", 4)) {
            gpu_thr = atof(e + 4);
            const char *c = strchr(e + 4, ':');
            after = c ? atol(c + 1) : 0;
            g_cap_n = 1;
            g_cap_f0 = gpu_thr > 0 ? LONG_MAX : -1;
            if (gpu_thr > 0) MLOG("DM_GPU_CAPTURE: the first frame after frame %ld that follows 60 frames in a row with more than %.1f ms of GPU time is captured", after, gpu_thr);
        } else {
            g_cap_f0 = e ? atol(e) : -1;
            g_cap_n = e && strchr(e, ':') ? atol(strchr(e, ':') + 1) : 1;
            if (g_cap_n < 1) g_cap_n = 1;
            if (g_cap_f0 <= 0) g_cap_f0 = -1;
        }
    }
    if (g_cap_f0 < 0) return;
    if (gpu_thr > 0 && !g_cap_on && f > 8 && (long)f >= after) {
        double g = mtl_gpu_ms(f - 8);
        over = g >= gpu_thr ? over + 1 : g >= 0 ? 0 : over;
        if (over >= 60) g_cap_f0 = (long)f;
    }
    MTLCaptureManager *m = [MTLCaptureManager sharedCaptureManager];
    if (!g_cap_on && f == (uint64_t)g_cap_f0) {
        g_cap_f0 = -1;   // one capture per setting: a failure does not retry every frame
        if (![m supportsDestination:MTLCaptureDestinationGPUTraceDocument]) {
            MLOG("[RED] DM_GPU_CAPTURE: this run cannot write GPU traces (set MTL_CAPTURE_ENABLED=1 in the environment)");
            return;
        }
        NSString *dir = gpu_capture_dir();
        if (!dir) return;
        gpu_capture_prune(dir);
        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        char ts[32];
        strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
        NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"dm_%s_f%llu.gputrace", ts, (unsigned long long)f]];
        MTLCaptureDescriptor *d = [MTLCaptureDescriptor new];
        d.captureObject = g_q4 ? (id)g_q4 : (id)g_dev;
        d.destination = MTLCaptureDestinationGPUTraceDocument;
        d.outputURL = [NSURL fileURLWithPath:path];
        NSError *err = nil;
        if ([m startCaptureWithDescriptor:d error:&err]) {
            g_cap_on = 1;
            g_cap_f0 = (long)f;
            MLOG("DM_GPU_CAPTURE: capturing frames %llu to %llu into %s", (unsigned long long)f, (unsigned long long)(f + g_cap_n - 1), path.UTF8String);
        }
        else MLOG("[RED] DM_GPU_CAPTURE: the capture did not start: %s", err.localizedDescription.UTF8String);
    } else if (g_cap_on && f >= (uint64_t)g_cap_f0 + (uint64_t)g_cap_n) {
        [m stopCapture];
        g_cap_on = 0;
        g_cap_f0 = -1;
        gpu_thr = 0;
        MLOG("DM_GPU_CAPTURE: capture stopped at frame %llu (the trace is written in the background)", (unsigned long long)f);
    }
}
void mtl_set_frame(uint64_t f) {
    atomic_store(&g_cur_frame, f);
    if (g_ub_n) ub_frame(f);
    if (ab_len()) ab_frame(f);
    gpu_capture_frame(f);
}
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) {
        atomic_store(&g_gpu[k].frame, f);
        atomic_store(&g_gpu[k].ns, 0);
        atomic_store(&g_gpu[k].pending, 0);
        atomic_store(&g_gpu[k].t0, UINT64_MAX);
        atomic_store(&g_gpu[k].t1, 0);
    }
    atomic_fetch_add(&g_gpu[k].pending, 1);
}
static void gpu_tag_done(uint64_t f, double g0, double g1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) return;
    double d = g1 - g0;
    if (d > 0 && g0 > 0) {   // a command buffer can complete with no start time (0)
        atomic_fetch_add(&g_gpu[k].ns, (uint64_t)(d * 1e9));
        uint64_t s0 = (uint64_t)(g0 * 1e9), s1 = (uint64_t)(g1 * 1e9), v;
        for (v = atomic_load(&g_gpu[k].t0); s0 < v && !atomic_compare_exchange_weak(&g_gpu[k].t0, &v, s0);) {}
        for (v = atomic_load(&g_gpu[k].t1); s1 > v && !atomic_compare_exchange_weak(&g_gpu[k].t1, &v, s1);) {}
    }
    atomic_fetch_sub(&g_gpu[k].pending, 1);
}
double mtl_gpu_ms(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return -1;
    return atomic_load(&g_gpu[k].ns) / 1e6;
}
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f || atomic_load(&g_gpu[k].pending)) return 0;
    *t0 = atomic_load(&g_gpu[k].t0); *t1 = atomic_load(&g_gpu[k].t1);
    return *t1 >= *t0 && *t1;
}
static void rp_flush_at(const char *fn, int line);
#define rp_flush() rp_flush_at(__func__, __LINE__)
enum { FB_DS_DEPTH = 1, FB_DS_STENCIL = 2, FB_DS_BOTH = 3 };
static void fb_ds_note_write(uint32_t planes);
static const char *cctx_snap(void);
typedef struct cbs_list cbs_list;
static cbs_list *cbs_take(void);
static void cbs_feedback(cbs_list *l, NSError *err, const char *where);
static void cbs_fly_report(uint64_t fence);
static void cbs_fly_put(uint64_t fence, const cbs_list *l);
static void ind_fly_put(const char *key, uint64_t ind, uint64_t off, void *buf);
static void lc_land(void);
static void lc_take_rt(void);
static int g_nlc;
// Run once, by the feedback handler or by a SyncWait that woke on the GPU's signal of the fence first (DM_NVN_FAST_WAKE).
typedef struct { _Atomic uint64_t fence; _Atomic int claimed; int n; post_op *ops; seg_clk *sc; uint32_t seg_last; uint64_t t_commit, t_ready; } post_rec;   // t_commit: guest ns at commit; t_ready: ready to run (0 = not known)
static post_rec g_postr[POST_RING];
static pthread_mutex_t g_post_mu = PTHREAD_MUTEX_INITIALIZER;
// A successful claim holds the lock until post_run finishes: another waiter must see the writes before completion is published.
static int post_claim(post_rec *r, uint64_t fence) {
    pthread_mutex_lock(&g_post_mu);
    if (r->fence != fence || r->claimed) { pthread_mutex_unlock(&g_post_mu); return 0; }
    return 1;
}
// post_complete places records in fence order, each post_run under g_post_mu. Every submission sets the timeline's end to its own end, so no later report reads earlier.
static uint64_t g_line_fence, g_line_end;   // guest ns
static uint64_t sub_place(const post_rec *r, uint64_t c0, uint64_t c1) {
    uint64_t fence = atomic_load(&r->fence);
    if (ts_mode() != 1 || fence <= g_line_fence) return 0;
    uint64_t s = c0, gaps = 0, off = 0;
    if (r->sc && r->sc->ncm) { s = atomic_load(&r->sc->t[r->sc->cm[0]][0]); gaps = seg_idle_before(r->sc, c1, c0); }
    uint64_t end = c1 ? c1 : guest_ns();   // with no GPU end, the time of the write
    if (gaps < end) end -= gaps;
    if (r->t_ready && s && c1 >= s) {
        uint64_t at = g_line_end > r->t_ready ? g_line_end : r->t_ready;
        if (at < s) { off = s - at; end -= off; }
    }
    if (end > g_line_end) g_line_end = end;
    g_line_fence = fence;
    return off;
}
static void post_run(post_rec *r, uint64_t c0, uint64_t c1) {
    // SyncWait may run completion on a thread whose outer pool never drains, and MTLBuffer.contents may autorelease its receiver
    @autoreleasepool {
    int n = r->n; post_op *ops = r->ops;
    seg_clk *sc = r->sc; uint32_t seg_last = r->seg_last;
    uint64_t off = sub_place(r, c0, c1);
    for (int i = 0; i < n; i++) {
        if (ops[i].kind == 1) continue;   // events publish below, once every earlier GPU write has landed
        else if (ops[i].kind == POST_BUFFER_COPY) nvn_dirty_gpu_written(ops[i].addr, ops[i].copy_end - ops[i].addr);
        else if (ops[i].kind == 3) {
            if (ops[i].mode == VIS_ZERO) { g_vis_acc = 0; continue; }
            vis_list *l = ops[i].vis;
            for (int k = 0; k < l->n; k++) {
                g_vis_acc += ((const uint64_t *)((__bridge id<MTLBuffer>)l->s[k].buf).contents)[l->s[k].slot];
                CFRelease(l->s[k].buf);
            }
            free(l);
            if (ops[i].mode == VIS_REPORT) { uint64_t rec[2] = {g_vis_acc, gpu_ticks()}; memcpy((void *)ops[i].addr, rec, sizeof rec); }
        }
        else {   // a timestamp report's counter field is 0, as on the GPU
            uint64_t t = rep_ticks(&ops[i], sc, seg_last, c0, c1, off), rec[2] = {0, t};
            memcpy((void *)ops[i].addr, rec, sizeof rec);
        }
    }
    // Forwarded counter values expire before the events publish, so a released CPU writer is never hidden by stale knowledge.
    uint64_t completed=atomic_load_explicit(&g_rc_complete,memory_order_relaxed);
    while(completed<r->fence && !atomic_compare_exchange_weak_explicit(&g_rc_complete,&completed,r->fence,memory_order_release,memory_order_relaxed)) {}
    // Mirror readbacks appended after the signal are earlier GPU writes: CPU-visible before the event is released.
    for (int i=0;i<n;i++) if(ops[i].kind==1) nvn_event_publish_gpu(ops[i].val,ops[i].mode,ops[i].v,r->fence);
    free(ops);
    seg_clk_put(sc);
    r->ops = NULL;
    r->sc = NULL;
    atomic_store(&r->claimed, 1);
    pthread_mutex_unlock(&g_post_mu);
    }
}
// Records complete in submission order; delayed callbacks carry their fence, never a recyclable slot.
// A command buffer that failed (a fault, a hang, a victim, access revoked) completes without its signal of g_ev: the fast-wake waits on it would wait out their timeout
static void fence_ev_raise(uint64_t f) {
    if (!g_ev || g_ev.signaledValue >= f) return;
    static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT;   // never lowered
    os_unfair_lock_lock(&lk);
    if (g_ev && g_ev.signaledValue < f) g_ev.signaledValue = f;
    os_unfair_lock_unlock(&lk);
}
static void post_complete(uint64_t fence, uint64_t c0, uint64_t c1) {
    uint64_t now = guest_ns();
    for (uint64_t f = g_done + 1; f <= fence; f++) {
        post_rec *r = &g_postr[f % POST_RING];
        if (!post_claim(r, f)) continue;
        uint64_t start = r->t_commit;
        if (r->sc && r->sc->ncm) {
            uint64_t pe = atomic_load(&r->sc->t[r->sc->cm[r->sc->ncm - 1]][1]);
            if (pe > start) start = pe;
        }
        if (start > now) start = now;
        post_run(r, f == fence && c1 ? c0 : seg_ts() ? start : 0, f == fence && c1 ? c1 : seg_ts() ? now : 0);
    }
    pthread_mutex_lock(&g_mu);
    if (fence > g_done) g_done = fence;
    uint64_t done = g_done;
    uint32_t *scr = atomic_load(&g_mesh_scr);
    if (scr) __atomic_store_n(scr + DMSH_MESH_SCRATCH_DONE, (uint32_t)done, __ATOMIC_RELAXED);
    pthread_cond_broadcast(&g_cv);
    pthread_mutex_unlock(&g_mu);
    fence_ev_raise(done);
    nvn_queue_gpu_progress();
}
static uint64_t g_sub_ready;
void mtl_sub_ready(uint64_t t) { g_sub_ready = t; }
static void rt_preserve_bound(void);
static void queue_program_preserve(void);
static void vc_commit(void);
// Every commit of the submission's command buffer goes through cb_commit. Sealed first: the blit encoder, the upload batch (committed before this one),
// the argument blocks' pool table words, then the residency set.
static void cb_seal(void) {
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    gmem_commit_patch();
}
static void cb_commit(DmCommitFB *o, int end) {
    cb_seal();
    res_commit();
    cb_end(g_cb, nil);
    rec_commit(g_cb, o, end); g_cbgen++;
}
uint64_t mtl_sub_commit(void) {
    lc_land();
    rp_flush(); ubd_land();
    g_ts_pend = -1;
    cb_seal();   // before the fence and the writes it counts
    g_cut_copy = 0;
    own_ghost_trim();
    vc_commit();
    treg_finish_submission();
    rt_preserve_bound();   // the game may reuse completed command memory: bindings must not point into its texture snapshots
    queue_program_preserve();   // submitted metadata may retire while these programs stay bound
    uint64_t fence = ++g_fence;
    if (g_nvs) { post((post_op){3, NULL, VIS_FOLD, 0, 0, 0, vis_snapshot()}); vis_clear(); }
    gw_commit(fence); rc_commit(fence);
    if (g_vis_ring_i >= 0) g_vis_ring[g_vis_ring_i].fence = fence;
    int n = g_npost;
    post_op *ops = n ? malloc(n * sizeof *ops) : NULL;
    post_rec *pr = &g_postr[fence % POST_RING];
    if (ws_on()) for (int i = 0; i < n; i++) {   // 1 event, 2 samples passed, 3 timestamp, 4 other counter
        if (g_post[i].kind == POST_BUFFER_COPY) continue;
        int k = g_post[i].kind == 2 ? (g_post[i].type == 0 || g_post[i].type == 0x10 ? 3 : 4) : g_post[i].kind == 3 ? (g_post[i].mode == VIS_REPORT ? 2 : 0) : 1;
        atomic_fetch_add(&g_ws_post[k], 1);
    }
    if (n) memcpy(ops, g_post, n * sizeof *ops);
    NSArray<dispatch_block_t> *commit_blocks = [g_commit_blocks copy];
    DmCommitFB *o = [DmCommitFB new];
    atomic_fetch_add(&g_seg_inflight, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        (void)f;
        atomic_fetch_sub(&g_seg_inflight, 1);
        for (dispatch_block_t block in commit_blocks) block();
    }];
    uint64_t gpu_f = atomic_load(&g_cur_frame); gpu_tag_begin(gpu_f);
    NSString *cctx = @(cctx_snap());
    cbs_list *cbs = cbs_take(); cbs_fly_put(fence, cbs);
    seg_clk *sc = g_sclk; uint32_t seg_last = g_seg; g_sclk = NULL;
    pthread_mutex_lock(&g_post_mu);
    if (!atomic_load(&pr->claimed) && pr->fence) MDIE("post ring: the writes of fence %llu never ran, %d submissions on (more in flight than the allocator pool allows)", (unsigned long long)pr->fence, POST_RING);
    pr->n = n; pr->ops = ops; pr->sc = sc; pr->seg_last = seg_last; pr->t_commit = guest_ns(); pr->t_ready = g_sub_ready;
    g_sub_ready = 0;
    atomic_store(&pr->claimed, 0);
    atomic_store(&pr->fence, fence);
    pthread_mutex_unlock(&g_post_mu);
    int lost_test = gpu_lost_test();
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> cb) {
        gpu_tag_done(gpu_f, cb.GPUStartTime, cb.GPUEndTime);
        if (ws_on()) ws_fb(fence, cb.GPUEndTime);
        uint64_t c0 = 0, c1 = 0;
        if (seg_ts()) { c0 = gpu_s_guest_ns(cb.GPUStartTime); c1 = gpu_s_guest_ns(cb.GPUEndTime); }
        if (cb.error) {
            atomic_store(&g_last_commit_error, 1);
            res_top_once(cb.error);
            static int n, nv;
            int victim = [cb.error.description containsString:@"InnocentVictim"];
            if (victim ? nv++ < 1 : n++ < 8) MLOG("[RED] command buffer error details fence=%llu: %s | GPU %.3f→%.3f", (unsigned long long)fence, cb.error.description.UTF8String, cb.GPUStartTime, cb.GPUEndTime);
            if (!victim) MLOG("[RED] state of the failed submission fence=%llu: %s", (unsigned long long)fence, cctx.UTF8String);
            MLOG("[RED] command buffer execution error: %s", cb.error.localizedDescription.UTF8String);
        }
        char where[64];
        snprintf(where, sizeof where, "fence %llu, its last command buffer", (unsigned long long)fence);
        cbs_feedback(cbs, cb.error, where);
        gpu_lost_check(lost_test ? gpu_lost_test_error() : cb.error, "submission");
        post_complete(fence, c0, c1);
        defer_drain(); defer_idle_commit();
    }];
    void arena_commit(uint64_t fence); arena_commit(fence);
    if (g_gp_state == 1 && !gp_active()) {
        int gn = g_gp_n;
        gp_lab *gl = malloc(sizeof(gp_lab) * (gn ? gn : 1));
        memcpy(gl, g_gp_lab, sizeof(gp_lab) * gn);
        uint64_t gf = g_gp_frame;
        g_gp_state = 2;
        atomic_store(&g_gp_busy, 1);
        [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)f; gp_report(gn, gl, gf); }];
    }
    if (atomic_load(&g_hold_on)) rec_queue_block(^{
        pthread_mutex_lock(&g_post_mu);
        if (atomic_load(&pr->fence) == fence) pr->t_commit = guest_ns();
        pthread_mutex_unlock(&g_post_mu);
    });
    cb_commit(o, 1);   // the stores it holds come before any later upload batch
    if (!lost_test) rec_signal(g_ev, fence);
    // With the encode thread the replay goes on with the next submission; waiting for the encode thread here would idle the GPU before most submissions.
    static int drain = -1;
    if (drain < 0) drain = sw_mode("DM_NVN_SUB_DRAIN", 0);
    if (rec_on() && g_rb_started) {   // soft: a replay-ahead hold is not waited for
        rec_t *r = rec_put(RO_NOTE, 0);
        r->a[0] = (uint64_t)(uintptr_t)&g_sub_cmt;
        r->a[1] = fence;
        if (drain && sw_on(drain)) rec_drain_soft(); else rec_publish();
    }
    else atomic_store(&g_sub_cmt, fence);
    g_alloc_fence[g_alloc_cur] = fence;
    g_cb = nil;
    atomic_store(&g_sub_open, 0);   // after ++g_fence and the last encoding: a buffer retired from now on waits for this fence at most
    defer_idle_commit();   // a feedback handler that drained since this submission's own commit saw it open
    g_commit_blocks = nil;
    objc_autoreleasePoolPop(g_sub_pool); g_sub_pool = NULL;
    return fence;
}

static void sub_commit_wait(uint64_t fence) { if (fence && rec_on()) rec_note_wait(&g_sub_cmt, fence); }
uint64_t mtl_completed_fence(void) { return atomic_load_explicit(&g_done,memory_order_acquire); }
// Replay ahead needs the encode thread: nq_worker translates a queue parked on a CPU-signalled event, nvn_rec.h holds its Metal work until the wait is satisfied.
int mtl_replay_ahead(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_REPLAY_AHEAD");
        v = (e ? atoi(e) != 0 : DM_GAME_REPLAY_AHEAD) && rec_on();
        if (v) MLOG("replay ahead: a queue parked on an event the game signals is translated meanwhile, its commits held until the signal (DM_NVN_REPLAY_AHEAD=0 turns it off)");
    }
    return v;
}
int mtl_hold_begin(volatile uint32_t *word, unsigned mode, uint32_t value) { return mtl_replay_ahead() && !g_cb && hold_begin(word, mode, value, g_fence + 1); }
int mtl_hold_active(void) { return atomic_load_explicit(&g_hold_on, memory_order_acquire); }
void mtl_hold_signal(volatile uint32_t *word, uint32_t value) {   // value: the word as the signal left it, so a later reset does not hide it
    uint32_t gen = atomic_load(&g_hold_gen);
    if (atomic_load_explicit(&g_hold_on, memory_order_acquire) && g_hold_word == word && hold_pred(value)) hold_poke(NULL, gen);
}
void mtl_hold_force(volatile uint32_t *word) {
    uint32_t gen = atomic_load(&g_hold_gen);
    if (atomic_load_explicit(&g_hold_on, memory_order_acquire) && g_hold_word == word) hold_poke("commands that signal its event were submitted behind it", gen);
}
void mtl_hold_drain(void) { hold_poke(HOLD_DRAIN, atomic_load(&g_hold_gen)); }
void mtl_hold_wait(void) { if (on_sub_thread()) hold_block(1); }
void mtl_hold_satisfied(void) { hold_poke(NULL, atomic_load(&g_hold_gen)); }
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_last_commit_error(void) { return atomic_load(&g_last_commit_error); }

static int fast_wake(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_FAST_WAKE");
        v = !(e && !strcmp(e, "0"));
        MLOG("game SyncWaits wake on the GPU's signal of the fence%s (DM_NVN_FAST_WAKE)", v ? "" : ": off, on the feedback handler");
    }
    return v;
}
static void post_early(uint64_t fence) {
    if (atomic_load_explicit(&g_done, memory_order_acquire) >= fence) return;
    post_complete(fence, 0, 0);
}
static int mtl_wait_at_(uint64_t fence, uint64_t timeout_ns, int site);
static int mtl_wait_at(uint64_t fence, uint64_t timeout_ns, int site) {
    if (g_done >= fence) { if (ws_on()) ws_put(site, 0, 0); return 1; }
    if (atomic_load(&g_hold_on) && fence >= g_hold_first) {   // held work reaches the GPU first: the submit thread stops for the signal
        if (on_sub_thread()) hold_block(0);
        else hold_stuck();   // a poll counts: the game may ask again until it lands, and signal only then
    }
    return mtl_wait_at_(fence, timeout_ns, site);
}
static int mtl_wait_at_(uint64_t fence, uint64_t timeout_ns, int site) {
    if (g_done >= fence) { if (ws_on()) ws_put(site, 0, 0); return 1; }
    if (timeout_ns > 0 && fence <= g_fence) rec_wake_for_fence(fence);
    uint64_t w0 = ws_t0();
    if ((site == WS_SYNC || site == WS_SYNC_NEWEST || site == WS_ACQ) && fast_wake() && g_ev && fence <= g_fence) {
        uint64_t ms = timeout_ns / 1000000ull;
        if (ms > 3600000ull) ms = 3600000ull;
        if ([g_ev waitUntilSignaledValue:fence timeoutMS:ms]) post_early(fence);
        int ok = g_done >= fence;
        if (ok || timeout_ns < 1000000ull) {
            if (ws_on()) { ws_put(site, w0, 1); ws_wake(fence); }
            return ok;
        }
        // The event and feedback waits share one budget; a timeout must not buy another full wait. The ledger counts the wait once, where it returns.
        if (timeout_ns != UINT64_MAX) {
            uint64_t used = ws_t0() - w0;
            if (used >= timeout_ns) {
                if (ws_on()) { ws_put(site, w0, 1); ws_wake(fence); }
                return 0;
            }
            timeout_ns -= used;
        }
    }
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += ns / 1000000000ull; dl.tv_nsec = ns % 1000000000ull;
    pthread_mutex_lock(&g_mu);
    int r = 0;
    struct timespec said;
    clock_gettime(CLOCK_REALTIME, &said);
    said.tv_sec += 10;
    int told = timeout_ns != UINT64_MAX;
    while (g_done < fence) {
        int late = !told && (dl.tv_sec > said.tv_sec || (dl.tv_sec == said.tv_sec && dl.tv_nsec > said.tv_nsec));
        if (!(r = pthread_cond_timedwait(&g_cv, &g_mu, late ? &said : &dl))) continue;
        if (!late) break;
        told = 1;
        static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 4) {
            uint64_t stuck = g_done + 1;
            MLOG("[RED] the host has waited 10 s for GPU work up to fence %llu (wait site %d): completed %llu, submitted %llu; the GPU runs nothing it can finish",
                 (unsigned long long)fence, site, (unsigned long long)g_done, (unsigned long long)g_fence);
            pthread_mutex_unlock(&g_mu);   // the list's lock is not taken under g_mu
            cbs_fly_report(stuck);
            pthread_mutex_lock(&g_mu);
        }
    }
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    if (ws_on()) { ws_put(site, w0, 1); if (site == WS_SYNC_NEWEST || site == WS_SYNC || site == WS_ACQ) ws_wake(fence); }
    return ok;
}
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_at(fence, timeout_ns, !timeout_ns ? WS_SYNC_POLL : fence >= g_fence ? WS_SYNC_NEWEST : WS_SYNC); }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_at(fence, timeout_ns, !timeout_ns ? WS_SYNC_POLL : WS_ACQ); }

static void mtl_finish_gpu_at(int site) {
    if (g_fence) mtl_wait_at(g_fence, UINT64_MAX, site);
    if (on_sub_thread()) rec_publish();   // a batch's commit may still be a record in the encode thread's current block
    up_wait_zero(&g_up_pending, site);
}
#define mtl_finish_gpu() mtl_finish_gpu_at(WS_DIAG)
// A CPU read of GPU-written storage needs completion, not merely queue order.
static id<MTLSharedEvent> g_read_ev;
static uint64_t g_read_v;
static int gpu_read_wait(int site) {
    rp_end(); ubd_land(); cb_commit(nil, 0);
    if (!g_read_ev) g_read_ev = [g_dev newSharedEvent];
    rec_signal(g_read_ev, ++g_read_v); rec_drain();
    uint64_t w0 = ws_t0();
    int ok = [g_read_ev waitUntilSignaledValue:g_read_v timeoutMS:10000];
    if (ws_on()) ws_put(site, w0, 1);
    if (ok) drw_drained();
    g_cb = rec_cb_new(); [g_cb beginCommandBufferWithAllocator:g_alloc4[g_alloc_cur]];
    return ok;
}
void mtl_present_flush(void);
// nvnQueueFinish waits for the GPU work, not for queued presents to reach the screen (one per display tick); AcquireTexture waits for a texture's previous present.
void mtl_finish(void) { mtl_finish_gpu_at(WS_FINISH); }
static void present_drop_for_suspend(void);
static void snap_drain(void);
void mtl_drain(void) {
    mtl_hold_drain();
    present_drop_for_suspend();
    mtl_present_flush();
    snap_drain();
    mtl_init();
    id<MTLCommandBuffer> cb = q_cb();
    [cb commit];
    [cb waitUntilCompleted];
    mtl_finish_gpu();
}
