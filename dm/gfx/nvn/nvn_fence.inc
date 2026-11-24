// A submission on the GPU: boot and frame timing, runtime switches, GPU captures (DM_GPU_CAPTURE), the CPU-side writes at completion (post_run), the commit (mtl_sub_commit), replay ahead, the game's SyncWait and Finish.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---------- Boot instrumentation (DM_BOOTPROF=1): metallib loading, pipeline creation (both happen synchronously during replay on the submit thread), texture upload, first-frame present ----------
// Elapsed time on the host's clock (hle_os.c now_ns: Mach absolute time, which Metal's GPU and CPU timestamps are on); graphics unit tests use it as now_ns, so their guest clock
// (guest_ns above) agrees with Metal's times as the host's does. CLOCK_MONOTONIC also counts the Mac's sleep, and differs from them by every sleep since boot
static uint64_t bp_now(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static uint64_t g_bp_t0;   // Time of process start (when this module is loaded)
__attribute__((constructor)) static void bp_t0(void) { g_bp_t0 = bp_now(); }
static int bootprof(void) { static int v = -1; if (v < 0) v = getenv("DM_BOOTPROF") != NULL; return v; }
static uint64_t g_bp_lib_n, g_bp_lib_ns, g_bp_pso_n, g_bp_pso_ns, g_bp_up_bytes, g_bp_first_present;
static uint64_t g_up_total_ns;   // Cumulative upload time (same source as the per-frame @upload_ms)
// ---------- Per-frame GPU time aggregation ----------
#define GRING 1024
static struct { _Atomic uint64_t frame, ns, t0, t1; _Atomic int pending; } g_gpu[GRING];   // t0 / t1: the frame's earliest GPUStart / latest GPUEnd (mach absolute time, ns)
static _Atomic uint64_t g_cur_frame;
// ---------- Same-process alternating measurement (DM_NVN_UB, off by default: without the environment variable the code below only adds one 'is it enabled' check) ----------
// On a hot real device the system keeps adjusting power / frequency, and segment comparison (segments A and B each run for a minute) cannot resolve differences of <10%; within the same run, toggle by phase in turn and take differences of adjacent phase pairs, so the drift cancels.
// DM_NVN_UB="name=item,item;name=item", one phase every DM_NVN_UB_N frames (default 180, minimum 60): even phases = baseline (nothing changed), odd phases take one category in turn. Each item of a category:
//   @set:<global variable name>=<value>   on entering the phase set this int global variable to the value, on leaving restore it (address found in the registration table below, else by symbol name)
//   <fragment shader key prefix> or *   the phase drops all draws of these fragment shaders (the picture will be wrong; it only measures 'how much this kind of draw is worth'; a nearly idle category needs a round of its own)
// Readings: each phase discards its first 45 frames (transition + present lag); for the remaining frames, the mean of the actual present interval (same source as perf.txt) and the frame counts in the three buckets 16.7 / 25 / ≥33 ms, one log line "UB present".
static int g_ub_n = -1, g_ub_N = 180; static _Atomic int g_ub_cur = -1; static char g_ub_name[16][16], g_ub_keys[16][16][48]; static int g_ub_nk[16];
static _Atomic int g_ubp_on, g_ubp_c[4]; static _Atomic uint64_t g_ubp_sum_us;
void mtl_ub_present(double iv) {   // Called once per frame by the present thread: actual present interval in ms (−1 = dropped, 0 = no reading)
    if (!atomic_load(&g_ubp_on) || iv == 0) return;
    if (iv < 0) { atomic_fetch_add(&g_ubp_c[3], 1); return; }
    atomic_fetch_add(&g_ubp_c[iv <= 20.8 ? 0 : iv <= 29.2 ? 1 : 2], 1); atomic_fetch_add(&g_ubp_sum_us, (uint64_t)(iv * 1000));
}
// Registered runtime switches: name → address. A switch registered here is found whether or not the main program's symbols are in its dynamic
// symbol table (a stripped release build's are not); unregistered ones are looked up by symbol name.
static int *ub_var(const char *nm) {
    extern int g_dm_present_link;
    static const struct { const char *n; int *p; } t[] = { {"g_dm_present_link", &g_dm_present_link} };
    for (size_t i = 0; i < sizeof t / sizeof t[0]; i++) if (!strcmp(nm, t[i].n)) return t[i].p;
    return dlsym(RTLD_DEFAULT, nm);
}
static void ub_frame(uint64_t f) {
    if (g_ub_n < 0) { g_ub_n = 0; const char *e = getenv("DM_NVN_UB"), *n = getenv("DM_NVN_UB_N"); if (n && atoi(n) >= 60) g_ub_N = atoi(n);
        for (const char *q = e; q && *q && g_ub_n < 16; ) { const char *eq = strchr(q, '='), *end = strchr(q, ';'); if (!end) end = q + strlen(q); if (!eq || eq > end) break;
            int c = g_ub_n++; size_t l = (size_t)(eq - q); if (l > 15) l = 15; memcpy(g_ub_name[c], q, l);
            for (const char *k = eq + 1; k < end && g_ub_nk[c] < 16; ) { size_t kl = 0; while (k + kl < end && k[kl] != ',') kl++; size_t cl = kl > 47 ? 47 : kl; memcpy(g_ub_keys[c][g_ub_nk[c]++], k, cl); k += kl; if (k < end) k++; }
            q = *end ? end + 1 : end; }
        if (g_ub_n) MLOG("DM_NVN_UB: %d categories, %d frames per phase (measured alternately in one process; phases that drop draws render wrong)", g_ub_n, g_ub_N); }
    if (!g_ub_n) return;
    uint64_t ph = f / (uint64_t)g_ub_N, r = f % (uint64_t)g_ub_N;
    if (r == 45) { for (int i = 0; i < 4; i++) atomic_store(&g_ubp_c[i], 0); atomic_store(&g_ubp_sum_us, 0); atomic_store(&g_ubp_on, 1); }
    if (r != 0) return;
    if (f && atomic_load(&g_ubp_on)) { atomic_store(&g_ubp_on, 0); uint64_t pp = ph - 1; int pc = (pp & 1) ? (int)((pp / 2) % (uint64_t)g_ub_n) : -1;
        int c0 = atomic_load(&g_ubp_c[0]), c1 = atomic_load(&g_ubp_c[1]), c2 = atomic_load(&g_ubp_c[2]), c3 = atomic_load(&g_ubp_c[3]), n_ = c0 + c1 + c2;
        MLOG("UB present %llu %s frames %d mean %.3f ms 2 ticks %d 3 ticks %d 4+ ticks %d discarded %d", (unsigned long long)pp, pc < 0 ? "base" : g_ub_name[pc], n_, n_ ? atomic_load(&g_ubp_sum_us) / 1000.0 / n_ : 0.0, c0, c1, c2, c3); }
    int nc = (ph & 1) ? (int)((ph / 2) % (uint64_t)g_ub_n) : -1;
    static int *sp[16]; static int sv[16], ns;   // Variables set by the previous phase: restore first
    for (int i = 0; i < ns; i++) *sp[i] = sv[i]; ns = 0;
    for (int i = 0; nc >= 0 && i < g_ub_nk[nc] && ns < 16; i++) { const char *k = g_ub_keys[nc][i]; if (strncmp(k, "@set:", 5)) continue;
        char nm[48]; const char *eq = strchr(k, '='); if (!eq) continue; size_t l = (size_t)(eq - (k + 5)); if (l >= sizeof nm) continue; memcpy(nm, k + 5, l); nm[l] = 0;
        int *a_ = ub_var(nm); static int bad, ok;
        if (!a_) { if (bad++ < 8) MLOG("[RED] DM_NVN_UB: global variable not found: %s (on a device, register it in the ub_var table first)", nm); continue; }
        sp[ns] = a_; sv[ns] = *a_; ns++; *a_ = atoi(eq + 1); if (ok++ < 8) MLOG("DM_NVN_UB: %s = %d (was %d)", nm, atoi(eq + 1), sv[ns - 1]); }
    atomic_store(&g_ub_cur, nc);
}
static int ub_skip(const char *key) {   // Whether this phase should drop draws of this fragment shader
    int uc = atomic_load(&g_ub_cur); if (uc < 0) return 0;
    for (int i = 0; i < g_ub_nk[uc]; i++) { const char *k = g_ub_keys[uc][i]; if (k[0] == '@') continue; if (k[0] == '*' || !strncmp(key, k, strlen(k))) return 1; }
    return 0;
}
static long ab_len(void); static void ab_frame(uint64_t f);
// DM_GPU_CAPTURE=<frame>[:<frames>] (diagnostic): a Metal GPU trace of those frames (default 1) for Xcode's Metal debugger (per-draw cost, shader profiler), or DM_GPU_CAPTURE=gpu:<ms>[:<after frame>]
// of one frame in the first scene, after that frame, whose GPU time passes <ms> in 60 frames in a row (so a single long frame in a light scene does not fire it). Read again
// when it changes, so DM_CTL's "env DM_GPU_CAPTURE=..." can set it while the game runs. It needs MTL_CAPTURE_ENABLED=1 in the environment. It captures the Metal 4 queue (Metal refuses to capture a Metal 4 device), so the old-queue helpers (present, some uploads)
// are not in it. The trace is dm_<date>-<time>_f<frame>.gputrace in DM_GPU_CAPTURE_DIR, by default gpu_captures/ in the working directory (the run folder). A trace takes 1.5-2 GB, so before each capture that folder alone is pruned: of its dm_*.gputrace entries the newest DM_GPU_CAPTURE_KEEP - 1 stay (default 2 with the new
// one); nothing outside it is removed
static long g_cap_f0 = -2, g_cap_n; static int g_cap_on;
static NSString *gpu_capture_dir(void) {
    const char *e = getenv("DM_GPU_CAPTURE_DIR"); NSString *dir;
    if (e && *e) dir = @(e); else { char cwd[1024]; if (!getcwd(cwd, sizeof cwd)) strcpy(cwd, "."); dir = [@(cwd) stringByAppendingPathComponent:@"gpu_captures"]; }
    NSError *err = nil;
    if (![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:&err]) { MLOG("[RED] DM_GPU_CAPTURE: cannot make the folder %s: %s", dir.UTF8String, err.localizedDescription.UTF8String); return nil; }
    return dir;
}
static void gpu_capture_prune(NSString *dir) {   // only dm_*.gputrace entries directly in the capture folder; the newest DM_GPU_CAPTURE_KEEP - 1 stay
    const char *e = getenv("DM_GPU_CAPTURE_KEEP"); long keep = e ? atol(e) : 2; if (keep < 1) keep = 1;
    NSFileManager *fm = NSFileManager.defaultManager; NSMutableArray<NSURL *> *traces = [NSMutableArray array];
    for (NSURL *u in [fm contentsOfDirectoryAtURL:[NSURL fileURLWithPath:dir isDirectory:YES] includingPropertiesForKeys:@[NSURLContentModificationDateKey] options:0 error:nil]) {
        NSString *n = u.lastPathComponent; if ([n hasPrefix:@"dm_"] && [n hasSuffix:@".gputrace"]) [traces addObject:u]; }
    [traces sortUsingComparator:^NSComparisonResult(NSURL *a, NSURL *b) { NSDate *da = nil, *db = nil;
        [a getResourceValue:&da forKey:NSURLContentModificationDateKey error:nil]; [b getResourceValue:&db forKey:NSURLContentModificationDateKey error:nil];
        return [db ?: NSDate.distantPast compare:da ?: NSDate.distantPast]; }];   // newest first
    for (NSUInteger i = (NSUInteger)keep - 1; i < traces.count; i++) if ([fm removeItemAtURL:traces[i] error:nil]) MLOG("DM_GPU_CAPTURE: removed %s (older captures beyond DM_GPU_CAPTURE_KEEP=%ld)", traces[i].lastPathComponent.UTF8String, keep);
}
static void gpu_capture_frame(uint64_t f) {
    static char last[32];   // read again when it changes
    { const char *e = getenv("DM_GPU_CAPTURE"); if (!g_cap_on && e && strncmp(e, last, sizeof last - 1)) { snprintf(last, sizeof last, "%s", e); g_cap_f0 = -2; } }
    static double gpu_thr; static long after; static int over;
    if (g_cap_f0 == -2) { const char *e = getenv("DM_GPU_CAPTURE"); gpu_thr = 0; over = 0;
        if (e && !strncmp(e, "gpu:", 4)) { gpu_thr = atof(e + 4); const char *c = strchr(e + 4, ':'); after = c ? atol(c + 1) : 0; g_cap_n = 1; g_cap_f0 = gpu_thr > 0 ? LONG_MAX : -1;   // armed: the frame is not known yet
            if (gpu_thr > 0) MLOG("DM_GPU_CAPTURE: the first frame after frame %ld that follows 60 frames in a row with more than %.1f ms of GPU time is captured", after, gpu_thr); }
        else { g_cap_f0 = e ? atol(e) : -1; g_cap_n = e && strchr(e, ':') ? atol(strchr(e, ':') + 1) : 1; if (g_cap_n < 1) g_cap_n = 1; if (g_cap_f0 <= 0) g_cap_f0 = -1; } }
    if (g_cap_f0 < 0) return;
    if (gpu_thr > 0 && !g_cap_on && f > 8 && (long)f >= after) { double g = mtl_gpu_ms(f - 8); over = g >= gpu_thr ? over + 1 : g >= 0 ? 0 : over; if (over >= 60) g_cap_f0 = (long)f; }
    MTLCaptureManager *m = [MTLCaptureManager sharedCaptureManager];
    if (!g_cap_on && f == (uint64_t)g_cap_f0) {
        g_cap_f0 = -1;   // one capture per setting: a failure below does not retry every frame
        if (![m supportsDestination:MTLCaptureDestinationGPUTraceDocument]) { MLOG("[RED] DM_GPU_CAPTURE: this run cannot write GPU traces (set MTL_CAPTURE_ENABLED=1 in the environment)"); return; }
        NSString *dir = gpu_capture_dir(); if (!dir) return;
        gpu_capture_prune(dir);
        time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y%m%d-%H%M%S", &tm);
        NSString *path = [dir stringByAppendingPathComponent:[NSString stringWithFormat:@"dm_%s_f%llu.gputrace", ts, (unsigned long long)f]];
        MTLCaptureDescriptor *d = [MTLCaptureDescriptor new]; d.captureObject = g_q4 ? (id)g_q4 : (id)g_dev; d.destination = MTLCaptureDestinationGPUTraceDocument; d.outputURL = [NSURL fileURLWithPath:path];
        NSError *err = nil;
        if ([m startCaptureWithDescriptor:d error:&err]) { g_cap_on = 1; g_cap_f0 = (long)f; MLOG("DM_GPU_CAPTURE: capturing frames %llu to %llu into %s", (unsigned long long)f, (unsigned long long)(f + g_cap_n - 1), path.UTF8String); }
        else MLOG("[RED] DM_GPU_CAPTURE: the capture did not start: %s", err.localizedDescription.UTF8String);
    } else if (g_cap_on && f >= (uint64_t)g_cap_f0 + (uint64_t)g_cap_n) { [m stopCapture]; g_cap_on = 0; g_cap_f0 = -1; gpu_thr = 0; MLOG("DM_GPU_CAPTURE: capture stopped at frame %llu (the trace is written in the background)", (unsigned long long)f); }
}
void mtl_set_frame(uint64_t f) { atomic_store(&g_cur_frame, f); if (g_ub_n) ub_frame(f); if (ab_len()) ab_frame(f); gpu_capture_frame(f); }
// GPU timing registration: gpu_tag_begin(frame) before commit, gpu_tag_done(frame, GPU start, GPU end) on completion (seconds on the CACurrentMediaTime base).
// The Metal 4 main submission backfills in the feedback of mtl_sub_commit (when switching to Metal 4 all the call sites were lost and the GPU column of the timing line became empty)
static void gpu_tag_begin(uint64_t f) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) { atomic_store(&g_gpu[k].frame, f); atomic_store(&g_gpu[k].ns, 0); atomic_store(&g_gpu[k].pending, 0); atomic_store(&g_gpu[k].t0, UINT64_MAX); atomic_store(&g_gpu[k].t1, 0); }
    atomic_fetch_add(&g_gpu[k].pending, 1);
}
static void gpu_tag_done(uint64_t f, double g0, double g1) {
    unsigned k = f % GRING;
    if (atomic_load(&g_gpu[k].frame) != f) return;
    double d = g1 - g0;
    if (d > 0 && g0 > 0) {   // a command buffer can complete with no start time (0, while a movie's frames upload each frame in Big Brain Academy: Brain vs. Brain), and its span would be the whole clock
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
static const char *cctx_snap(void);   // What is in this submission (printed into the failure line on error; defined before mtl_exec)
typedef struct cbs_list cbs_list;   // The game's shaders one command buffer encoded (defined before encode_args)
static cbs_list *cbs_take(void);
static void cbs_feedback(cbs_list *l, NSError *err, const char *where);
static void cbs_fly_report(uint64_t fence); static void cbs_fly_put(uint64_t fence, const cbs_list *l); static void ind_fly_put(const char *key, uint64_t ind, uint64_t off, void *buf);
static void lc_land(void); static void lc_take_rt(void); static int g_nlc;   // lazy clears (lc_push): land / take the waiting ones, and how many wait
// A submission's CPU-side writes at its end (events, samples-passed and counter reports): run once, by its feedback handler or, when a game's SyncWait wakes on the
// GPU's own signal of the fence first, by that waiter (DM_NVN_FAST_WAKE, on by default): the feedback handler comes 0.26 ms after the GPU end on an M2 Max
typedef struct { _Atomic uint64_t fence; _Atomic int claimed; int n; post_op *ops; seg_clk *sc; uint32_t seg_last; uint64_t t_commit, t_ready; } post_rec;   // t_commit: guest ns when it was committed; t_ready: when it was ready to run (mtl_sub_ready, 0 = not known)
static post_rec g_postr[POST_RING];   // POST_RING: see its enum, before mtl_sub_begin
static pthread_mutex_t g_post_mu = PTHREAD_MUTEX_INITIALIZER;
// A successful claim holds the lock until post_run finishes. Ownership alone
// cannot publish completion: another waiter must see the actual writes first.
static int post_claim(post_rec *r, uint64_t fence) {
    pthread_mutex_lock(&g_post_mu);
    if (r->fence != fence || r->claimed) { pthread_mutex_unlock(&g_post_mu); return 0; }
    return 1;
}
// A submission's place on the GPU timeline (see place_back), in fence order: post_complete runs the records up to a fence in order, each post_run under g_post_mu. Returns how far its
// first command buffer moves back. With no ready time (replay tools, tests), its first command buffer's feedback not arrived yet, or no GPU times, it stays where it is. Every submission,
// moved or not, sets the timeline's end to its own end as its reports read it, so no later report reads earlier than one of it. A submission completed by a SyncWait that woke on the
// GPU's signal before its feedback (post_early) is placed by the times post_complete estimates for it
static uint64_t g_line_fence, g_line_end;   // the last submission placed, and the timeline's end (guest ns)
static uint64_t sub_place(const post_rec *r, uint64_t c0, uint64_t c1) {
    uint64_t fence = atomic_load(&r->fence);
    if (ts_mode() != 1 || fence <= g_line_fence) return 0;
    uint64_t s = c0, gaps = 0, off = 0;   // its first command buffer's real GPU start, and the gaps between its command buffers
    if (r->sc && r->sc->ncm) { s = atomic_load(&r->sc->t[r->sc->cm[0]][0]); gaps = seg_idle_before(r->sc, c1, c0); }
    uint64_t end = c1 ? c1 : guest_ns();   // its end as its last report reads it unmoved (rep_ticks: with no GPU end, the time of the write)
    if (gaps < end) end -= gaps;
    if (r->t_ready && s && c1 >= s) { uint64_t at = g_line_end > r->t_ready ? g_line_end : r->t_ready; if (at < s) { off = s - at; end -= off; } }
    if (end > g_line_end) g_line_end = end;
    g_line_fence = fence;
    return off;
}
static void post_run(post_rec *r, uint64_t c0, uint64_t c1) {
    // A guest SyncWait may run completion on a thread whose outer pool never drains.
    // MTLBuffer.contents may autorelease its receiver; keep that lifetime inside the completed copy.
    @autoreleasepool {
    int n = r->n; post_op *ops = r->ops; seg_clk *sc = r->sc; uint32_t seg_last = r->seg_last;
    uint64_t off = sub_place(r, c0, c1);
    for (int i = 0; i < n; i++) {
        if (ops[i].kind == 1) continue;   // A terminal release publishes after every earlier GPU write has landed.
        else if (ops[i].kind == POST_BUFFER_COPY) nvn_dirty_gpu_written(ops[i].addr, ops[i].copy_end - ops[i].addr);
        else if (ops[i].kind == 3) {   // Samples passed: the running total, in submission order
            if (ops[i].mode == VIS_ZERO) { g_vis_acc = 0; continue; }
            vis_list *l = ops[i].vis;
            for (int k = 0; k < l->n; k++) { g_vis_acc += ((const uint64_t *)((__bridge id<MTLBuffer>)l->s[k].buf).contents)[l->s[k].slot]; CFRelease(l->s[k].buf); }
            free(l); if (ops[i].mode == VIS_REPORT) { uint64_t rec[2] = {g_vis_acc, gpu_ticks()}; memcpy((void *)ops[i].addr, rec, sizeof rec); }
        }
        else { uint64_t t = rep_ticks(&ops[i], sc, seg_last, c0, c1, off), rec[2] = {0, t}; memcpy((void *)ops[i].addr, rec, sizeof rec); }   // a timestamp report's counter field is 0, as on the GPU (see rc_value)
    }
    // Forwarded counter values expire once their CPU-visible reports have landed.
    // This precedes event publication so a released CPU writer is never hidden by stale knowledge.
    uint64_t completed=atomic_load_explicit(&g_rc_complete,memory_order_relaxed);
    while(completed<r->fence && !atomic_compare_exchange_weak_explicit(&g_rc_complete,&completed,r->fence,memory_order_release,memory_order_relaxed)) {}
    // Pass teardown can append mirror readbacks after the recorded signal. They are
    // preceding GPU writes, and must be CPU-visible before this prefix releases its event.
    for (int i=0;i<n;i++) if(ops[i].kind==1) nvn_event_publish_gpu(ops[i].val,ops[i].mode,ops[i].v,r->fence);
    free(ops); seg_clk_put(sc); r->ops = NULL; r->sc = NULL;
    atomic_store(&r->claimed, 1);
    pthread_mutex_unlock(&g_post_mu);
    }
}
// Both GPU feedback and an early waiter complete records in submission order.
// Delayed callbacks carry their fence, never the identity of a recyclable slot.
static void post_complete(uint64_t fence, uint64_t c0, uint64_t c1) {
    uint64_t now = guest_ns();
    for (uint64_t f = g_done + 1; f <= fence; f++) {
        post_rec *r = &g_postr[f % POST_RING];
        if (!post_claim(r, f)) continue;
        uint64_t start = r->t_commit;
        if (r->sc && r->sc->ncm) {   // nor before the command buffer committed before it ended
            uint64_t pe = atomic_load(&r->sc->t[r->sc->cm[r->sc->ncm - 1]][1]); if (pe > start) start = pe;
        }
        if (start > now) start = now;
        post_run(r, f == fence && c1 ? c0 : seg_ts() ? start : 0, f == fence && c1 ? c1 : seg_ts() ? now : 0);
    }
    pthread_mutex_lock(&g_mu); if (fence > g_done) g_done = fence;
    uint32_t *scr = atomic_load(&g_mesh_scr); if (scr) __atomic_store_n(scr + DMSH_MESH_SCRATCH_DONE, (uint32_t)g_done, __ATOMIC_RELAXED);   // the slices of groups this fence cut off are free again
    pthread_cond_broadcast(&g_cv); pthread_mutex_unlock(&g_mu);
    nvn_queue_gpu_progress();
}
static uint64_t g_sub_ready;   // mtl_sub_ready, for the submission being encoded
void mtl_sub_ready(uint64_t t) { g_sub_ready = t; }
static void rt_preserve_bound(void);
static void queue_program_preserve(void);
static void vc_commit(void);
// Every commit of the submission's command buffer (mtl_sub_commit, sub_cut, mdi_wait) ends it through cb_commit, so that none misses a step: mdi_wait committed without patching the raw
// pool table words until PR #2330. Sealed first: the blit encoder, the upload batch (its own command buffer, committed before this one) and the
// raw pool table words of the argument blocks encoded so far (gmem_commit_patch); then the residency set. A commit whose bookkeeping must see all of that (the submission's fence and the writes
// it counts, a segment's state list) seals before it: cb_commit seals again and finds nothing left to do
static void cb_seal(void) {
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    up_flush();
    gmem_commit_patch();
}
static void cb_commit(DmCommitFB *o, int end) {   // end: the submission ends with this command buffer (rec_commit)
    cb_seal();
    res_commit();
    cb_end(g_cb, nil);
    rec_commit(g_cb, o, end); g_cbgen++;
}
uint64_t mtl_sub_commit(void) {
    lc_land();   // clears still waiting (the command list ends with them)
    rp_flush(); ubd_land();
    g_ts_pend = -1;   // nothing after it: the last segment's end is the command buffer's own (rep_ticks)
    cb_seal();   // before the fence and the writes it counts
    g_cut_copy = 0;
    own_ghost_trim();   // idle finalized textures past the ghost table's size go behind this submission's fence
    vc_commit();   // and the texture views of the textures the owner has let go, those evictions among them
    treg_finish_submission();   // Late descriptor views retire behind this submission's fence.
    rt_preserve_bound();   // The game may reuse completed command memory; queue bindings must not point into its texture snapshots.
    queue_program_preserve(); // Submitted metadata may retire while these programs remain bound.
    uint64_t fence = ++g_fence;
    if (g_nvs) { post((post_op){3, NULL, VIS_FOLD, 0, 0, 0, vis_snapshot()}); vis_clear(); }   // this submission's slots after its last report: read at its completion (Occlusion queries)
    gw_commit(fence); rc_commit(fence);
    if (g_vis_ring_i >= 0) g_vis_ring[g_vis_ring_i].fence = fence;
    // CPU-visible effects after completion: copy the op table into the callback
    int n = g_npost; post_op *ops = n ? malloc(n * sizeof *ops) : NULL; post_rec *pr = &g_postr[fence % POST_RING];
    if (ws_on()) for (int i = 0; i < n; i++) { if (g_post[i].kind == POST_BUFFER_COPY) continue; int k = g_post[i].kind == 2 ? (g_post[i].type == 0 || g_post[i].type == 0x10 ? 3 : 4) : g_post[i].kind == 3 ? (g_post[i].mode == VIS_REPORT ? 2 : 0) : 1; atomic_fetch_add(&g_ws_post[k], 1); }   // 1 event, 2 samples passed, 3 timestamp, 4 other counter
    if (n) memcpy(ops, g_post, n * sizeof *ops);
    NSArray<dispatch_block_t> *commit_blocks = [g_commit_blocks copy];
    DmCommitFB *o = [DmCommitFB new];
    atomic_fetch_add(&g_seg_inflight, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)f; atomic_fetch_sub(&g_seg_inflight, 1); for (dispatch_block_t block in commit_blocks) block(); }];
    uint64_t gpu_f = atomic_load(&g_cur_frame); gpu_tag_begin(gpu_f);
    NSString *cctx = @(cctx_snap());
    cbs_list *cbs = cbs_take(); cbs_fly_put(fence, cbs);
    seg_clk *sc = g_sclk; uint32_t seg_last = g_seg; g_sclk = NULL;
    pthread_mutex_lock(&g_post_mu);
    if (!atomic_load(&pr->claimed) && pr->fence) MDIE("post ring: the writes of fence %llu never ran, %d submissions on (more in flight than the allocator pool allows)", (unsigned long long)pr->fence, POST_RING);
    pr->n = n; pr->ops = ops; pr->sc = sc; pr->seg_last = seg_last; pr->t_commit = guest_ns(); pr->t_ready = g_sub_ready; g_sub_ready = 0; atomic_store(&pr->claimed, 0); atomic_store(&pr->fence, fence);
    pthread_mutex_unlock(&g_post_mu);
    int lost_test = gpu_lost_test();
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> cb) {
        gpu_tag_done(gpu_f, cb.GPUStartTime, cb.GPUEndTime);
        if (ws_on()) ws_fb(fence, cb.GPUEndTime);
        uint64_t c0 = 0, c1 = 0; if (seg_ts()) { c0 = gpu_s_guest_ns(cb.GPUStartTime); c1 = gpu_s_guest_ns(cb.GPUEndTime); }
        if (cb.error) { atomic_store(&g_last_commit_error, 1); res_top_once(cb.error); static int n, nv; int victim = [cb.error.description containsString:@"InnocentVictim"]; if (victim ? nv++ < 1 : n++ < 8) MLOG("[RED] command buffer error details fence=%llu: %s | GPU %.3f→%.3f", (unsigned long long)fence, cb.error.description.UTF8String, cb.GPUStartTime, cb.GPUEndTime); if (!victim) MLOG("[RED] state of the failed submission fence=%llu: %s", (unsigned long long)fence, cctx.UTF8String); MLOG("[RED] command buffer execution error: %s", cb.error.localizedDescription.UTF8String); }
        char where[64]; snprintf(where, sizeof where, "fence %llu, its last command buffer", (unsigned long long)fence); cbs_feedback(cbs, cb.error, where);
        gpu_lost_check(lost_test ? gpu_lost_test_error() : cb.error, "submission");
        post_complete(fence, c0, c1);
        if (atomic_load(&g_gpu_lost)) gpu_lost_release();   // the GPU signals none of these fences
        defer_drain(); defer_idle_commit();
    }];
    void arena_commit(uint64_t fence); arena_commit(fence);
    if (g_gp_state == 1 && !gp_active()) { int gn = g_gp_n; gp_lab *gl = malloc(sizeof(gp_lab) * (gn ? gn : 1)); memcpy(gl, g_gp_lab, sizeof(gp_lab) * gn); uint64_t gf = g_gp_frame; g_gp_state = 2; atomic_store(&g_gp_busy, 1);
        [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { (void)f; gp_report(gn, gl, gf); }]; }
    if (atomic_load(&g_hold_on)) rec_queue_block(^{ pthread_mutex_lock(&g_post_mu); if (atomic_load(&pr->fence) == fence) pr->t_commit = guest_ns(); pthread_mutex_unlock(&g_post_mu); });   // a held commit: its time is when the queue gets it
    cb_commit(o, 1);   // the stores it holds come before any later upload batch (gsb_held)
    if (!lost_test) rec_signal(g_ev, fence);
    // With the encode thread the records go to it and the replay goes on with the next submission: what comes after it on the queue is a record too (the next submission's commits,
    // upload batches, snapshot copies), a CPU wait for the GPU waits for the fence's signal behind this commit, and a present of the game's own texture waits for this note
    // (sub_commit_wait). The replay used to wait for the encode thread here: Ocarina of Time makes about 30 submissions a frame (one at each event signal), and the GPU
    // sat idle before most of them while the replay waited and then translated the next one (docs/reference/rendering.md)
    // DM_NVN_SUB_DRAIN=1 waits for the encode thread here as before (ab: alternates with the DM_AB phases, for measurement)
    static int drain = -1; if (drain < 0) drain = sw_mode("DM_NVN_SUB_DRAIN", 0);
    if (rec_on() && g_rb_started) { rec_t *r = rec_put(RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&g_sub_cmt; r->a[1] = fence; if (drain && sw_on(drain)) rec_drain_soft(); else rec_publish(); }   // soft: a replay-ahead hold is not waited for (nvn_rec.h)
    else atomic_store(&g_sub_cmt, fence);
    g_alloc_fence[g_alloc_cur] = fence;
    g_cb = nil;
    atomic_store(&g_sub_open, 0);   // after ++g_fence and the last encoding: a buffer retired from now on waits for this fence at most
    defer_idle_commit();   // a feedback handler that drained since this submission's own commit saw it open
    g_commit_blocks = nil;
    objc_autoreleasePoolPop(g_sub_pool); g_sub_pool = NULL;
    return fence;
}

// A present of the game's own texture waits for the submissions before it (g_sub_cmt), as the display command buffer goes to the queue directly and must come after the work that drew the picture
static void sub_commit_wait(uint64_t fence) { if (fence && rec_on()) rec_note_wait(&g_sub_cmt, fence); }
uint64_t mtl_completed_fence(void) { return atomic_load_explicit(&g_done,memory_order_acquire); }
// Replay ahead (profile key replay_ahead, DM_NVN_REPLAY_AHEAD overrides; needs the encode thread): nvn_queue.inc nq_worker translates a queue parked on an event the game's CPU
// signals, and nvn_rec.h keeps what that sends to the Metal queue until the wait is satisfied
int mtl_replay_ahead(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_REPLAY_AHEAD"); v = (e ? atoi(e) != 0 : DM_GAME_REPLAY_AHEAD) && rec_on();
        if (v) MLOG("replay ahead: a queue parked on an event the game signals is translated meanwhile, its commits held until the signal (DM_NVN_REPLAY_AHEAD=0 turns it off)"); }
    return v;
}
int mtl_hold_begin(volatile uint32_t *word, unsigned mode, uint32_t value) { return mtl_replay_ahead() && !g_cb && hold_begin(word, mode, value, g_fence + 1); }
int mtl_hold_active(void) { return atomic_load_explicit(&g_hold_on, memory_order_acquire); }
void mtl_hold_signal(volatile uint32_t *word, uint32_t value) { uint32_t gen = atomic_load(&g_hold_gen); if (atomic_load_explicit(&g_hold_on, memory_order_acquire) && g_hold_word == word && hold_pred(value)) hold_poke(NULL, gen); }   // value: the word as the signal left it, so a later reset does not hide it
void mtl_hold_force(volatile uint32_t *word) { uint32_t gen = atomic_load(&g_hold_gen); if (atomic_load_explicit(&g_hold_on, memory_order_acquire) && g_hold_word == word) hold_poke("commands that signal its event were submitted behind it", gen); }
void mtl_hold_drain(void) { hold_poke(HOLD_DRAIN, atomic_load(&g_hold_gen)); }
void mtl_hold_wait(void) { if (on_sub_thread()) hold_block(1); }
void mtl_hold_satisfied(void) { hold_poke(NULL, atomic_load(&g_hold_gen)); }
uint64_t mtl_last_fence(void) { return g_fence; }
int mtl_last_commit_error(void) { return atomic_load(&g_last_commit_error); }

static int fast_wake(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_FAST_WAKE"); v = !(e && !strcmp(e, "0")); MLOG("game SyncWaits wake on the GPU's signal of the fence%s (DM_NVN_FAST_WAKE)", v ? "" : ": off, on the feedback handler"); } return v; }
// A SyncWait woken by the GPU's signal: the CPU-side writes of every submission up to the fence that no feedback handler has run yet, run here. The fence's last segment ended
// now (c1); it started at the later of its commit and the previous segment's end (c0), as near as the CPU can tell without its feedback
static void post_early(uint64_t fence) {
    post_complete(fence, 0, 0);
}
static int mtl_wait_at_(uint64_t fence, uint64_t timeout_ns, int site);
static int mtl_wait_at(uint64_t fence, uint64_t timeout_ns, int site) {
    if (g_done >= fence) { if (ws_on()) ws_put(site, 0, 0); return 1; }
    if (atomic_load(&g_hold_on) && fence >= g_hold_first) {   // held work reaches the GPU first (nvn_rec.h): the submit thread stops for the signal; another thread waits as for parked work, and the hold's limit runs
        if (on_sub_thread()) hold_block(0);
        else hold_stuck();   // a poll counts: the game may ask again until it lands, and signal only then
    }
    return mtl_wait_at_(fence, timeout_ns, site);
}
static int mtl_wait_at_(uint64_t fence, uint64_t timeout_ns, int site) {
    if (g_done >= fence) { if (ws_on()) ws_put(site, 0, 0); return 1; }
    if (timeout_ns > 0 && fence <= g_fence) rec_wake_for_fence(fence);   // the encode thread's idle loop takes the waited-for records at once
    uint64_t w0 = ws_t0();
    if ((site == WS_SYNC || site == WS_SYNC_NEWEST || site == WS_ACQ) && fast_wake() && g_ev && fence <= g_fence) {   // the game's own wait: wake on the GPU's signal (rec_signal(g_ev, fence) after each submission)
        uint64_t ms = timeout_ns / 1000000ull; if (ms > 3600000ull) ms = 3600000ull;
        if ([g_ev waitUntilSignaledValue:fence timeoutMS:ms]) post_early(fence);
        int ok = g_done >= fence;
        if (ws_on()) { ws_put(site, w0, 1); ws_wake(fence); }
        if (ok || timeout_ns < 1000000ull) return ok;
        // The event and feedback waits share a finite budget; a timeout must not buy another full wait.
        if (timeout_ns != UINT64_MAX) {
            uint64_t used = ws_t0() - w0;
            if (used >= timeout_ns) return 0;
            timeout_ns -= used;
        }
    }
    struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl);
    uint64_t ns = (uint64_t)dl.tv_nsec + (timeout_ns > 3600000000000ull ? 3600000000000ull : timeout_ns);
    dl.tv_sec += ns / 1000000000ull; dl.tv_nsec = ns % 1000000000ull;
    pthread_mutex_lock(&g_mu);
    int r = 0;
    // An unbounded wait still waiting after 10 s is a hang: said once, with what it waits for (PRAGMATA's queue worker stopped here at its first 3D scene, no GPU error)
    struct timespec said; clock_gettime(CLOCK_REALTIME, &said); said.tv_sec += 10; int told = timeout_ns != UINT64_MAX;
    while (g_done < fence) {
        int late = !told && (dl.tv_sec > said.tv_sec || (dl.tv_sec == said.tv_sec && dl.tv_nsec > said.tv_nsec));
        if (!(r = pthread_cond_timedwait(&g_cv, &g_mu, late ? &said : &dl))) continue;
        if (!late) break;
        told = 1; static _Atomic int n;
        if (atomic_fetch_add(&n, 1) < 4) {
            uint64_t stuck = g_done + 1;
            MLOG("[RED] the host has waited 10 s for GPU work up to fence %llu (wait site %d): completed %llu, submitted %llu; the GPU runs nothing it can finish",
                 (unsigned long long)fence, site, (unsigned long long)g_done, (unsigned long long)g_fence);
            pthread_mutex_unlock(&g_mu); cbs_fly_report(stuck); pthread_mutex_lock(&g_mu);   // the list's lock is not taken under g_mu
        }
    }
    int ok = g_done >= fence;
    pthread_mutex_unlock(&g_mu);
    if (ws_on()) { ws_put(site, w0, 1); if (site == WS_SYNC_NEWEST || site == WS_SYNC || site == WS_ACQ) ws_wake(fence); }
    return ok;
}
// The game's SyncWait (hle_nvn.c), with what it waits for in the wait ledger
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_at(fence, timeout_ns, !timeout_ns ? WS_SYNC_POLL : fence >= g_fence ? WS_SYNC_NEWEST : WS_SYNC); }
int mtl_wait_acq(uint64_t fence, uint64_t timeout_ns) { return mtl_wait_at(fence, timeout_ns, !timeout_ns ? WS_SYNC_POLL : WS_ACQ); }

static void mtl_finish_gpu_at(int site) {
    if (g_fence) mtl_wait_at(g_fence, UINT64_MAX, site);
    if (on_sub_thread()) rec_publish();   // a batch's commit may still be a record in the encode thread's current block (rec_commit_upload), as in up_begin and q_cb
    up_wait_zero(&g_up_pending, site);
}
#define mtl_finish_gpu() mtl_finish_gpu_at(WS_DIAG)
// A CPU read of GPU-written storage needs completion, not merely queue order. Shared by CPU texture uploads and indirect-count reads; GPU uploads stay asynchronous.
static id<MTLSharedEvent> g_read_ev; static uint64_t g_read_v;
static int gpu_read_wait(int site) {
    rp_end(); ubd_land(); cb_commit(nil, 0);
    if (!g_read_ev) g_read_ev = [g_dev newSharedEvent];
    rec_signal(g_read_ev, ++g_read_v); rec_drain();
    uint64_t w0 = ws_t0(); int ok = [g_read_ev waitUntilSignaledValue:g_read_v timeoutMS:10000];
    if (ws_on()) ws_put(site, w0, 1);
    if (ok) drw_drained();
    g_cb = rec_cb_new(); [g_cb beginCommandBufferWithAllocator:g_alloc4[g_alloc_cur]];
    return ok;
}
void mtl_present_flush(void);
// nvnQueueFinish waits for the GPU work submitted so far, not for queued presents to reach the screen: with a window, the display link takes one present per tick, so waiting for them
// made every QueueFinish cost one or two display frames. Fire Emblem: Three Houses' loader submits a copy and calls QueueFinish for each resource it uploads, so its first loading screen
// paid that for every one. A swapchain texture is not handed back before its previous present is processed (AcquireTexture waits for that ticket)
void mtl_finish(void) { mtl_finish_gpu_at(WS_FINISH); }
static void present_drop_for_suspend(void);
static void snap_drain(void);
void mtl_drain(void) { mtl_hold_drain(); present_drop_for_suspend(); mtl_present_flush(); snap_drain(); mtl_init(); id<MTLCommandBuffer> cb = q_cb(); [cb commit]; [cb waitUntilCompleted]; mtl_finish_gpu(); }
