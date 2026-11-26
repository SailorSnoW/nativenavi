// The present pass, the display link and present thread, present snapshots, the waits for presents, mtl_stats, the watchdog's dump and the memory census.
// fx: the scaler state, nil = bilinear; uvb: the pass's crop constants (one buffer per present allocator, CPU-written)
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], id<MTLBuffer> uvb, fx_state *fx) {
    NSUInteger r[4]; if (!present_crop_rect(src, crop, r)) return 0;
    present_pipeline(dst.pixelFormat);
    double dw = dst.width, dh = dst.height;
    MTLViewport vp = present_fit(dw, dh);
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    int flipped = atomic_load_explicit(&g_origin_ll, memory_order_relaxed), wrote = 0;
    static int direct_ok = -1;
    if (direct_ok < 0) { const char *e = getenv("DM_FX_DIRECT"); direct_ok = !(e && !strcmp(e, "0")); }
    // MetalFX may write dst itself, as the pass would only copy its output 1:1
    id<MTLTexture> tex = present_raw_src(src), up = fx ? fx_encode(fx, cb, tex, r, vp, dst.pixelFormat, fx == &g_fx_present, direct_ok && full && !flipped ? dst : nil, &wrote) : nil;
    if (wrote) return PRESENT_METALFX;
    if (up && fx == &g_fx_present) {
        static int once;
        if (!once++)
            MLOG("present upscaling: the present pass draws MetalFX's output into the drawable: %s (drawable %lux%lu format %lu usage %#lx, picture %lux%lu)",
                 !direct_ok ? "DM_FX_DIRECT=0" : !full ? "the picture does not fill it" : flipped ? "the lower-left origin flips it" : "the scaler cannot write it",
                 (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat, (unsigned long)dst.usage, (unsigned long)up.width, (unsigned long)up.height);
    }
    if (up) { tex = up; r[0] = r[1] = 0; r[2] = up.width; r[3] = up.height; }   // the scaled picture, the viewport's size: drawn 1:1
    MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
    rp.colorAttachments[0].texture = dst;
    rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    if (!en) { MLOG("[RED] failed to create the M4 present render encoder"); return 0; }
    [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [en setArgumentTable:g_present_args atStages:MTLRenderStageFragment];
    [g_present_args setTexture:tex.gpuResourceID atIndex:0];
    [g_present_args setSamplerState:g_blit_samp.gpuResourceID atIndex:0];
    float *uv = uvb.contents; NSUInteger tw = tex.width, th = tex.height, cx = r[0], cy = r[1], cw = r[2], ch = r[3];
    uv[0] = (float)cx / tw; uv[1] = (float)cy / th; uv[2] = (float)cw / tw; uv[3] = (float)ch / th;
    uv[4] = (cx + 0.5f) / tw; uv[5] = (cy + 0.5f) / th; uv[6] = (cx + cw - 0.5f) / tw; uv[7] = (cy + ch - 0.5f) / th;
    if (flipped) { uv[1] += uv[3]; uv[3] = -uv[3]; }   // lower-left origin: the window image is stored bottom-up
    at_addr(g_present_args, uvb.gpuAddress, 0, "present table slot", 1);
    [en setViewport:vp]; [en setRenderPipelineState:g_blit_pso];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    m4_end(en);
    return up ? PRESENT_METALFX : PRESENT_STANDARD;
}
// out: private, render target. Resources stay resident and retained until GPU completion.
static int present_offscreen_m4(id<MTLTexture> src, const int crop[4], id<MTLTexture> out, void (^done)(int)) {
    id<MTL4CommandAllocator> al = [g_dev newCommandAllocator];
    id<MTL4CommandBuffer> cb = [g_dev newCommandBuffer];
    id<MTLBuffer> uvb = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
    if (!al || !cb || !uvb) { MLOG("[RED] offscreen present: no command allocator or buffer"); return 0; }
    id<MTLTexture> raw = present_raw_src(src);
    fx_state fx = {0};
    res_add(out); res_add(uvb); res_commit();
    [cb beginCommandBufferWithAllocator:al];
    int kind = present_encode_m4(cb, raw, out, crop, uvb, &fx);
    cb_end(cb, nil);
    if (!kind) { res_remove(out); res_remove(uvb); res_remove(fx.in); res_remove(fx.out); return 0; }
    MTL4CommitOptions *options = [MTL4CommitOptions new];
    [options addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        (void)al; (void)cb; (void)raw; (void)fx.sc;   // keep the Metal 4 commands, source view and scaler alive too
        res_remove(out); res_remove(uvb); res_remove(fx.in); res_remove(fx.out);
        if (f.error) { MLOG("[RED] offscreen present: %s", f.error.description.UTF8String); gpu_lost_check(f.error, "offscreen present"); }
        done(f.error ? PRESENT_FAILED : kind);
    }];
    [g_q4 commit:&cb count:1 options:options];
    return 1;
}
// a timeout leaves cleanup with the GPU completion handler
static int present_offscreen_wait(id<MTLTexture> src, const int crop[4], id<MTLTexture> out) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block int kind = PRESENT_FAILED;
    if (!present_offscreen_m4(src, crop, out, ^(int k) { kind = k; dispatch_semaphore_signal(sem); })) return 0;
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) { MLOG("[RED] offscreen present: the GPU did not finish within 10 s"); return 0; }
    return kind;
}
static void sort_doubles(double *a, int n) {
    for (int i = 1; i < n; i++) {
        double x = a[i]; int j = i - 1;
        while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; }
        a[j + 1] = x;
    }
}
// under g_pq_mu (the feedback handler)
static void present_gpu_note(double s, int fx) {
    static double v[2][600]; static int n[2];
    if (!(s > 0 && s < 1)) return;
    g_present_gpu_s = g_present_gpu_s > 0 ? g_present_gpu_s * 0.75 + s * 0.25 : s;
    v[fx][n[fx]++] = s * 1e3;
    if (n[fx] < 600) return;
    double *a = v[fx];
    sort_doubles(a, 600);
    MLOG("present command buffer GPU time over 600 presents %s: p50 %.3f ms, p95 %.3f ms, max %.3f ms", fx ? "through MetalFX" : "through the bilinear pass", a[299], a[569], a[599]);
    n[fx] = 0;
}
static double g_link_it_tin;   // only the display-link thread uses it
// latch_ts: the targetTimestamp of a present the latch took, by when its GPU work must end (0 = not latched)
static int present_now(const tex_t *t, double period, uint64_t ticket, const int crop[4], id<CAMetalDrawable> supplied, double target_ts, double latch_ts) {
    if (g_present_stall_ns) { struct timespec ts = {0, (long)g_present_stall_ns}; nanosleep(&ts, NULL); }
    g_present_n++;
    if (bootprof() && (g_present_n == 1 || g_present_n % 120 == 0)) {
        if (g_present_n == 1) g_bp_first_present = bp_now() - g_bp_t0;
        MLOG("[bootprof] t=%.3f s present #%llu: metallibs loaded %llu / %.1f ms; pipelines created %llu / %.1f ms (all synchronous on the submit thread); prewarmed and used: shaders %llu / render pipelines %llu / compute pipelines %llu; "
             "texture uploads %.1f MB / %llu first uploads + %llu re-uploads / %.1f ms; first present t=%.3f s", (bp_now() - g_bp_t0) / 1e9, g_present_n,
             g_bp_lib_n, g_bp_lib_ns / 1e6, g_bp_pso_n, g_bp_pso_ns / 1e6, g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps,
             g_bp_up_bytes / 1048576.0, g_pkg_uploads, g_pkg_reuploads,
             g_up_total_ns / 1e6, g_bp_first_present / 1e9);
    }
    PD("X ser=%llu", t->serial);
    id<MTLTexture> src = tex_mtl(t);
    if (!src) { RED_ONCE(8, "present texture could not get an MTLTexture"); return 0; }
    // A window texture presented before any submission used it would be outside the committed residency set: a page fault, after which macOS ignores the process's command buffers.
    res_commit();
    // DM_NVN_REGLUM: a command buffer of its own after the frame's rendering, on the same queue; nothing waits for it.
    static int rl = -1;
    static float rr[4];
    static FILE *rlf;
    static pthread_mutex_t rlmu = PTHREAD_MUTEX_INITIALIZER;
    if (rl < 0) {
        const char *e = getenv("DM_NVN_REGLUM"), *c = e ? strchr(e, ':') : NULL;
        rl = c && sscanf(e, "%f,%f,%f,%f", &rr[0], &rr[1], &rr[2], &rr[3]) == 4 && (rlf = fopen(c + 1, "w")) != NULL;
        if (e && !rl) MLOG("[RED] DM_NVN_REGLUM argument or file invalid: %s", e);
    }
    if (rl && (src.pixelFormat == MTLPixelFormatRGBA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB)) {
        NSUInteger x0 = (NSUInteger)(rr[0] * src.width), y0 = (NSUInteger)(rr[1] * src.height), w = (NSUInteger)(rr[2] * src.width) - x0, h = (NSUInteger)(rr[3] * src.height) - y0;
        id<MTLBuffer> rb = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        [b endEncoding];
        unsigned long long n = g_present_n, tns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) {
            (void)c;
            const uint8_t *q = rb.contents; uint64_t sum = 0;
            uint8_t g[4096]; NSUInteger ng = 0;
            for (NSUInteger y = 0; y < h; y += 4)
                for (NSUInteger x = 0; x < w && ng < sizeof g; x += 4) { const uint8_t *px = q + 4 * (y * w + x); g[ng++] = (uint8_t)((px[0] + px[1] + px[2]) / 3); }
            for (NSUInteger k = 0; k < w * h; k++) sum += q[4 * k] + q[4 * k + 1] + q[4 * k + 2];
            pthread_mutex_lock(&rlmu);
            fprintf(rlf, "%llu\t%llu\t%.4f\t", n, tns, sum / (3.0 * w * h));
            for (NSUInteger k = 0; k < ng; k++) fprintf(rlf, "%02x", g[k]);
            fputc('\n', rlf); fflush(rlf);
            pthread_mutex_unlock(&rlmu);
        }];
        [cb commit];
    }
    static long shots[4096];
    static int nshot = -1;
    static const char *shot_path;
    if (nshot < 0) {
        nshot = 0;
        const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) {
            shot_path = c + 1;
            for (const char *q = e; q < c && nshot < 4096; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) shots[nshot++] = atol(q);
        }
    }
    for (int i = 0; i < nshot; i++)
        if ((long)g_present_n == shots[i]) { char path[1024]; snprintf(path, sizeof path, shot_path, (int)g_present_n); shot(src, path, 1, crop); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) {
        if (g_present_n >= g_shotq[i].frame) { shot(src, g_shotq[i].path, 1, crop); g_shotq[i] = g_shotq[--g_nshotq]; }
        else i++;
    }
    pthread_mutex_unlock(&g_shotq_mu);

    if (atomic_load(&g_gpu_lost)) return 0;   // the display command buffer would be ignored too: take no drawable from the screen the front end is about to close
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer) return 0;
    if (!layer.displaySyncEnabled) layer.displaySyncEnabled = YES;
    present_pipeline(layer.pixelFormat);   // the first frame compiles the pipeline before it takes the drawable
    if (g_grab_cb) grab_present(src, crop, layer.drawableSize);   // a peek without the lock; grab_present locks
    uint64_t dt0 = now_ns();
    id<CAMetalDrawable> dr = supplied ? supplied : [layer nextDrawable];
    if (!supplied) g_drawable_ns += now_ns() - dt0;
    if (!dr) return 0;
    int alloc_i = (int)(ticket % 3);
    if (g_present_alloc_fence[alloc_i]) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_present_alloc_done[alloc_i] < g_present_alloc_fence[alloc_i]) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        pthread_mutex_unlock(&g_pq_mu);
    }
    [g_present_alloc4[alloc_i] reset];
    [g_q4 waitForDrawable:dr];
    id<MTL4CommandBuffer> cb = g_present_cb[alloc_i];
    [cb beginCommandBufferWithAllocator:g_present_alloc4[alloc_i]];
    int kind = present_encode_m4(cb, src, dr.texture, crop, g_present_uv[alloc_i], &g_fx_present);
    id<MTLResidencySet> drs = ((CAMetalLayer *)dr.layer).residencySet;
    if (!kind) { cb_end(cb, drs); [g_q4 signalDrawable:dr]; return 0; }
    int fx_kind = kind == PRESENT_METALFX;
    cb_end(cb, drs);
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) { MLOG("[RED] display command buffer error: %s", f.error.description.UTF8String); gpu_lost_check(f.error, "display"); }
        if (latch_ts > 0 && f.GPUEndTime > latch_ts) atomic_fetch_add(&g_link_latch_late_gpu, 1);
        pthread_mutex_lock(&g_pq_mu);
        if (ticket > g_pq_done) g_pq_done = ticket;
        if (ticket > g_present_alloc_done[alloc_i]) g_present_alloc_done[alloc_i] = ticket;
        g_pq_inflight--;
        pthread_cond_broadcast(&g_pq_cv);
        present_gpu_note(f.GPUEndTime - f.GPUStartTime, fx_kind);
        pthread_mutex_unlock(&g_pq_mu);
    }];
    id<MTL4CommandBuffer> buffers[] = {cb};
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
    [g_q4 commit:buffers count:1 options:o];
    if (target_ts > 0) {
        static double leads[300], e2e[300];   // e2e: game enqueue -> system-scheduled present
        static uint64_t lead_n;
        leads[lead_n % 300] = (target_ts - CACurrentMediaTime()) * 1000.0;
        e2e[lead_n % 300] = g_link_it_tin > 0 ? (target_ts - g_link_it_tin) * 1000.0 : 0;
        lead_n++;
        if (lead_n % 300 == 0) {
            double a[300]; memcpy(a, leads, sizeof a);
            sort_doubles(a, 300);
            double b[300]; memcpy(b, e2e, sizeof b);
            sort_doubles(b, 300);
            MLOG("display link up to tick %llu: ticks 300, presents %llu, empty ticks %llu (held for the present interval %llu), old frames dropped by merging %llu; submission ahead of target display p50 %.3f ms / min %.3f ms; game enqueue to target display p50 %.3f ms / min %.3f / max %.3f ms; standing frames drained %llu times; late latch took %llu presents, timed out %llu times",
                 (unsigned long long)g_link_tick_n, (unsigned long long)g_present_n, (unsigned long long)g_link_empty, (unsigned long long)g_link_held,
                 (unsigned long long)g_link_dropped, a[149], a[0], b[149], b[0], b[299], (unsigned long long)atomic_load(&g_link_drained), (unsigned long long)g_link_latched, (unsigned long long)g_link_latch_out);
        }
    }
    [g_q4 signalDrawable:dr];
    g_present_alloc_fence[alloc_i] = ticket;
    uint64_t pn = ticket;   // by the game's present number: the display link merges and drops old frames
    [dr addPresentedHandler:^(id<MTLDrawable> d) {
        CFTimeInterval tt = d.presentedTime;
        nvn_present_history_put(&g_present_history, pn, tt > 0 ? tt : -1);
        pres_seen(pn, tt);
    }];
    // each frame is displayed for at least one present period, so completion jitter around a refresh boundary does not stutter
    if (target_ts > 0) [dr present];
    else if (period > 0) [dr presentAfterMinimumDuration:period]; else [dr present];
    return 1;
}
// QueuePresentTexture only queues the present (the game often calls it holding a lock): taking the drawable, the blit and the present run on this thread in enqueue order.
// AcquireTexture waits until the ticket of that texture's previous present is processed (mtl_present_wait), so the game's drawing is ordered after that blit.
#define PQ 8
typedef struct {
    tex_t t; double period; uint64_t frame; int crop[4];
    double t_in;   // CACurrentMediaTime
    uint64_t snap_v, sub_v;   // snap_v: the snapshot copy this present shows; sub_v: the submissions a present of the game's own texture follows
} pq_item;
static void snap_commit_wait(uint64_t v);
static pq_item g_pq[PQ];
static uint64_t g_pq_in;
static double g_link_vperiod, g_link_vhalf;   // seconds per counted tick (0 = every callback is a tick) and half a refresh

@interface NvMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end

// A display link silent in the foreground would leave the game waiting forever: past STALL_NS the waiting frame and those before it are dropped and the game runs on.
// Under g_pq_mu.
#define STALL_NS 100000000ull
#define STALL_RED_NS 2000000000ull
static uint64_t g_stall_t0, g_stall_drop, g_stall_n;
static int g_stall_red, g_stall_logged;
// Past LINK_RESCUE_NS of a silent link while active, the present thread takes drawables itself (macOS pauses the link of a window it takes for covered); the link is retried
// after g_rescue_back_ns, doubling up to 60 s. Rescue state changes take g_pq_mu; link_sync handles the link object on the enqueue thread.
#define LINK_RESCUE_NS 500000000ull
static _Atomic int g_rescue_mode;   // also read without g_pq_mu, to stop pacing on silent ticks immediately
static uint64_t g_rescue_back_at, g_rescue_back_ns = 3000000000ull, g_rescue_n;
// A link retried after a rescue that has not called back within link_probe_ns() of active time goes back to the rescue at once. Under g_pq_mu.
#define LINK_PROBE_NS 67000000ull
static int g_link_probe_pending;
static uint64_t g_link_probe_at, g_link_probe_fail;
// While the app is inactive or in the background the system does not call back anyway: the watchdog counts active time only.
__attribute__((weak)) int dm_focus_now(void) { return 1; }   // graphics tests without the host count as always active
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
// macOS fires a covered window's link about once a second, focus or not: while the front end says so, link_sync runs mode 0 as a rescue does (the acquire follows
// the host's clock) and the link's silence is no stall. docs/reference/frame-rate.md#a-covered-windows-presents-follow-the-hosts-clock
__attribute__((weak)) int front_window_covered(void) { return 0; }   // front_mac.m; graphics tests without the host are never covered
static _Atomic int g_covered;   // link_sync's sample of front_window_covered, also read without g_pq_mu
static int link_watchdog_active(void) { return dm_focus_now() == 1 && !atomic_load(&g_covered) && !front_window_covered(); }
static int g_link_drain_on = -1;   // -1 = not read yet (file-level so that t_present_rescue can set it)
static int link_drain_on(void) {
    if (g_link_drain_on < 0) {
        const char *e = getenv("DM_PRESENT_DRAIN"); g_link_drain_on = e ? atoi(e) != 0 : 1;
        MLOG("display link: frames standing in the present queue are %s (DM_PRESENT_DRAIN)", g_link_drain_on ? "drained: the acquire waits them out once the game has kept up for 2 s" : "left standing");
    }
    return g_link_drain_on;
}
static int g_link_latch_on = -1;   // -1 = not read yet
static int link_latch_on(void) {
    if (g_link_latch_on < 0) {
        const char *e = getenv("DM_PRESENT_LATCH"); g_link_latch_on = e ? atoi(e) != 0 : 1;
        MLOG("display link: %s (DM_PRESENT_LATCH)", g_link_latch_on ? "the callback that woke the acquire waits briefly for its present and takes it" : "each callback takes the frame already queued");
    }
    return g_link_latch_on;
}
// Seconds from the wake (wk_cb) to the end of the frame's GPU work and present pass had the latch taken it there; -1 without the frame's GPU times. The ring's GPU times
// are CACurrentMediaTime nanoseconds; work still running counts as ending now (cb).
static double link_gpu_lead(uint64_t frame, double t_in, double cb, double wk_cb, double pres_gpu) {
    if (wk_cb < 0) return -1;
    unsigned k = frame % GRING; uint64_t tag = frame + 1;
    if (atomic_load_explicit(&g_gpu[k].frame, memory_order_acquire) != tag) return -1;
    int pend = atomic_load_explicit(&g_gpu[k].pending, memory_order_acquire);
    uint64_t t1 = atomic_load_explicit(&g_gpu[k].t1, memory_order_relaxed);
    if (atomic_load_explicit(&g_gpu[k].frame, memory_order_acquire) != tag) return -1;
    double end;
    if (pend) end = cb;
    else if (t1) end = t1 / 1e9;
    else return -1;
    if (t_in > end) end = t_in;
    return end + pres_gpu - wk_cb;
}
// On the display-link thread, once per frame (seconds). Not with present snapshots, which run the game a frame ahead on purpose, nor on the 120 Hz link.
static void link_drain_note(double cb, double age, int dropped, double lead, double gpu_lead, double room) {
    if (!link_drain_on()) return;
    int rate = mtl_link_rate(), ok = !dropped && rate != 120 && !mtl_snap_on();
    int k = nvn_link_drain_next(&g_link_drain, cb, age, 1.0 / rate, atomic_load(&g_link_blocked_ns) / 1e9, ok, lead, gpu_lead, room);
    if (k) atomic_store(&g_link_drain_req, k);
    if (k & LINK_DRAIN_LATCH) g_link_latch_misses = 0;
}
static struct timespec link_deadline(uint64_t ns);
// recorded as not presented, so the present statistics count them as dropped. Caller holds g_pq_mu.
static void pq_drop(uint64_t first, uint64_t last) {
    for (uint64_t k = first; k <= last; k++) { nvn_present_history_put(&g_present_history, k, -1); mtl_tex_unpin(g_pq[(k - 1) % PQ].t.serial); }
    g_link_dropped += last + 1 - first;
}
@implementation NvMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    double cb = CACurrentMediaTime();
    pthread_mutex_lock(&g_pq_mu);
    // a callback that cannot take a present neither resets the watchdog nor counts a pacing tick
    if (!g_dm_present_link || g_rescue_mode || atomic_load(&g_covered)) { pthread_mutex_unlock(&g_pq_mu); return; }
    g_link_probe_at = 0;
    if (g_stall_t0) {
        uint64_t since = dm_focus_in_since(), t = now_ns();
        MLOG("display link recovered: stalled %.0f ms (%.0f ms of it active), fallback dropped %llu frames (stall #%llu)", (t - g_stall_t0) / 1e6, (t - (g_stall_t0 > since ? g_stall_t0 : since)) / 1e6, (unsigned long long)g_stall_drop, (unsigned long long)g_stall_n);
        g_stall_t0 = 0;
    }
    pthread_mutex_unlock(&g_pq_mu);
    double vp = g_link_vperiod, tick_time = update.targetPresentationTimestamp;
    if (vp > 0) {   // virtual ticks: only the refresh nearest each slot of a steady clock at the wanted rate counts
        static double next, vp_was;
        if (vp != vp_was || tick_time > next + vp) { next = tick_time; vp_was = vp; }
        if (tick_time < next - g_link_vhalf) return;
        tick_time = next;   // the hold follows counted slots even when their physical refreshes are unevenly spaced
        next += vp;
    }
    const int rate = mtl_link_rate();
    const double tick_period = vp > 0 ? vp : 1.0 / rate;
    static double cb_prev, gap_max;
    static int nticks;
    const double cb_gap = cb_prev > 0 ? cb - cb_prev : 0;
    if (cb_gap > gap_max) gap_max = cb_gap;
    cb_prev = cb; nticks++;
    pthread_mutex_lock(&g_link_mu);
    __atomic_store_n(&g_link_tick_ns, now_ns(), __ATOMIC_RELAXED);
    const uint64_t tick = ++g_link_tick_n;
    const int woke = g_link_waiter && tick >= g_link_waiter;
    const int drain_end = woke && g_link_waiter_end;
    pthread_cond_broadcast(&g_link_cv);
    pthread_mutex_unlock(&g_link_mu);
    pthread_mutex_lock(&g_pq_mu);
    // checked again: the queue lock was released to publish the tick, and a rescue or mode change may have taken the queue meanwhile
    if (!g_dm_present_link || g_rescue_mode || atomic_load(&g_covered)) { pthread_mutex_unlock(&g_pq_mu); return; }
    // a multi-tick frame stays on screen until its hold ends, even with the next frame queued
    const int hold = g_link_hold_until > 0 && tick_time < g_link_hold_until - 0.002;
    // The late latch (nvn_link_latch.h): wait briefly for the woken acquire's present, g_pq_mu released meanwhile. Only into an empty queue, and only on the 60 Hz link,
    // where it was measured.
    const int latch = link_latch_on() && rate == 60 && !mtl_snap_on();
    const double room = latch ? update.targetTimestamp - cb : 0;
    static uint64_t woken_n;
    if (woke) woken_n++;
    // a latched present whose GPU work ended late, or a skipped tick after one: leave the latch for this tick (nvn_link_latch_check)
    static uint64_t seen_late_gpu;
    int force_exit = 0;
    if (latch) {
        uint64_t lg = atomic_load(&g_link_latch_late_gpu);
        int gpu_missed = lg != seen_late_gpu;
        seen_late_gpu = lg;
        int fresh = g_link_drain.in && g_link_drain.held < LINK_LATCH_GIVE_UP;
        force_exit = nvn_link_latch_check(&g_link_latch_health, cb_gap, tick_period, gpu_missed, fresh);
        if (force_exit) { g_link_latch_misses = LINK_LATCH_GIVE_UP; g_link_latch_exits++; }
    } else {
        seen_late_gpu = atomic_load(&g_link_latch_late_gpu);
        g_link_latch_health = (nvn_link_latch_health){0};
    }
    if (latch) {
        static double rmin = 1e9, rsum;
        static int rn;
        double r = room;
        if (r < rmin) rmin = r;
        rsum += r;
        if (++rn == 300) {
            MLOG("display link: from the callback to its targetTimestamp over 300 ticks: min %.2f ms, mean %.2f ms (late latch: %llu presents taken, %llu timeouts, %llu exits (%llu late GPU), entered by %llu drains, the next needs %.0f s of early frames)", rmin * 1e3, rsum / rn * 1e3, (unsigned long long)g_link_latched, (unsigned long long)g_link_latch_out, (unsigned long long)g_link_latch_exits, (unsigned long long)atomic_load(&g_link_latch_late_gpu), (unsigned long long)g_link_drain.entries, LINK_DRAIN_SECS * (double)(1u << g_link_drain.back));
            rmin = 1e9; rsum = 0; rn = 0;
        }
    }
    // the last two callbacks that woke an acquire and their room, so each frame taken is timed from the wake before it
    static double wk_cb[2] = {-1, -1}, wk_room[2];
    if (woke && latch) { wk_cb[1] = wk_cb[0]; wk_room[1] = wk_room[0]; wk_cb[0] = cb; wk_room[0] = room; }
    int latched_now = 0;
    if (!force_exit && woke && !hold && g_pq_read == g_pq_in && latch) {
        double w = nvn_link_latch_wait(cb, cb + room, tick_period, g_link_latch_misses, woken_n, drain_end);
        if (w > 0) {
            const uint64_t in0 = g_pq_in; struct timespec ts = link_deadline((uint64_t)(w * 1e9));
            while (g_pq_in == in0) if (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) == ETIMEDOUT) break;
            if (g_pq_in > in0) { g_link_latched++; g_link_latch_misses = 0; latched_now = 1; } else { g_link_latch_out++; g_link_latch_misses++; }
        }
    }
    if (g_pq_read == g_pq_in) { g_link_latch_health.prev = 0; g_link_empty++; pthread_mutex_unlock(&g_pq_mu); return; }
    if (hold) { g_link_latch_health.prev = 0; g_link_held++; pthread_mutex_unlock(&g_pq_mu); return; }
    uint64_t first = g_pq_read + 1, ticket = g_pq_in;
    // In order, one frame per tick and at most one behind the newest: taking the newest drops a frame whenever two land in the same refresh.
    static int fifo = -1;
    if (fifo < 0) {
        const char *e = getenv("DM_PRESENT_FIFO");
        fifo = !(e && !strcmp(e, "0"));
        MLOG("display link: %s", fifo ? "frames in order, at most one behind" : "the newest frame each tick (DM_PRESENT_FIFO=0)");
    }
    if (fifo && ticket > first) ticket = ticket - first >= 2 ? g_pq_in - 1 : first;
    pq_item it = g_pq[(ticket - 1) % PQ]; g_link_it_tin = it.t_in;
    const double pres_gpu = g_present_gpu_s;
    g_pq_read = ticket;
    if (ticket > first) pq_drop(first, ticket - 1);
    if (ticket > 1 && g_pq_done < ticket - 1) g_pq_done = ticket - 1;
    nvn_link_history_put(&g_present_history, (nvn_link_time){ticket, update.targetPresentationTimestamp, (update.targetPresentationTimestamp - cb) * 1e3, gap_max * 1e3, nticks});
    nticks = 0; gap_max = 0;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    // whole ticks: the acquire spreads fractional ticks, and present_period truncates to whole nanoseconds
    const double hold_ticks = floor((it.period + 1e-9) / tick_period);
    // The drain (nvn_link_drain.h): the next acquire waits the ticks the queued frames have to spare, so the queue runs empty without a repeated or dropped frame.
    {
        const int k = !latch || hold_ticks >= 2 ? -1 : it.t_in >= wk_cb[0] ? 0 : it.t_in >= wk_cb[1] ? 1 : -1;
        const double gl = k < 0 ? -1 : link_gpu_lead(it.frame, it.t_in, cb, wk_cb[k], pres_gpu);
        link_drain_note(cb, cb - it.t_in, ticket > first, k < 0 || wk_cb[k] < 0 ? -1 : it.t_in - wk_cb[k], gl, k < 0 ? 0 : wk_room[k]);
    }
    int submitted;
    snap_commit_wait(it.snap_v); sub_commit_wait(it.sub_v);
    @autoreleasepool { submitted = present_now(&it.t, it.period, ticket, it.crop, update.drawable, update.targetPresentationTimestamp, latched_now ? update.targetTimestamp : 0); mtl_tex_unpin(it.t.serial); }
    g_link_latch_health.prev = submitted && latched_now;
    g_link_hold_until = submitted && hold_ticks >= 2 ? tick_time + hold_ticks * tick_period : 0;
    if (!submitted) {
        pthread_mutex_lock(&g_pq_mu);
        if (ticket > g_pq_done) g_pq_done = ticket;
        pthread_cond_broadcast(&g_pq_cv);
        pthread_mutex_unlock(&g_pq_mu);
    }
}
@end

static CAMetalDisplayLink *g_link;
static CFRunLoopRef g_link_rl;
static int g_link_started;
static int g_link_owns_layer, g_classic_busy;   // under g_pq_mu: nextDrawable throws while the layer has a display link attached, so the two exclude each other
static int link_on(void) { return g_dm_present_link && !atomic_load(&g_rescue_mode) && !atomic_load(&g_covered) && g_link != nil; }
// On a screen whose rate is no multiple of the link's (144, 165 Hz) the system fires a 60 Hz link every third refresh (55 Hz): the link runs at the screen's rate and only
// the refreshes nearest a steady 1/60 s clock count as ticks.
__attribute__((weak)) int front_screen_settling(void) { return 0; }   // front_mac.m: startup fullscreen not entered yet, screen match pending, or mode switched under 1 s ago
__attribute__((weak)) int front_screen_max_fps(void) { return 0; }
__attribute__((weak)) void front_screen_match(int fps) { (void)fps; }
static CAFrameRateRange link_range(int mode) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_LINK_VTICK"); on = !(e && !strcmp(e, "0")); }
    int rate = mode == 3 ? 30 : mode == 2 ? 120 : 60, hz = on ? front_screen_max_fps() : 0;
    if (hz > rate && hz % rate) {   // at 120 fps too: a game stepping by its target must not get 144 ticks a second
        if (g_link_vperiod != 1.0 / rate) MLOG("display link at the screen's %d Hz, counting a tick every 1/%d s (the screen's rate is not a multiple of it)", hz, rate);
        g_link_vhalf = 0.5 / hz; g_link_vperiod = 1.0 / rate;
        return CAFrameRateRangeMake(rate, hz, hz);
    }
    g_link_vperiod = 0;
    return mode == 3 ? CAFrameRateRangeMake(30, 30, 30) : mode == 2 ? CAFrameRateRangeMake(60, 120, 120) : CAFrameRateRangeMake(60, 60, 60);
}
// Workaround: frame latency 2 in variable-refresh fullscreen, where at latency 1 the system shows some presents one 120 Hz refresh late and the link skips its next tick.
// Open: why it misses at 16 ms (docs/reference/frame-rate.md).
__attribute__((weak)) int front_fullscreen(void) { return 0; }
__attribute__((weak)) int front_screen_variable_refresh(void) { return 0; }
// CAMetalDisplayLink accepts only 1 or 2: compare its readback with the supported request, or an oversized override rewrites and logs every frame.
static int link_latency(void) { return g_present_latency > 0 ? MIN(g_present_latency, 2) : front_fullscreen() && front_screen_variable_refresh() ? 2 : 1; }
static const char *link_latency_why(void) { return g_present_latency > 0 ? "DM_PRESENT_LATENCY" : !front_fullscreen() ? "window" : front_screen_variable_refresh() ? "fullscreen, variable refresh" : "fullscreen, fixed refresh"; }
// Workaround: the 60 Hz link paces a 30 fps target in fullscreen on a fixed-rate screen, where the 30 Hz link skipped ticks; why is open (docs/reference/frame-rate.md).
// In a window the drain makes the 60 Hz link better too, so the 30 Hz one stays only without the drain or with present snapshots.
static int link_30hz(void) { return front_fullscreen() ? front_screen_variable_refresh() : !link_drain_on() || mtl_snap_on(); }
// Workaround: the 120 Hz link in variable-refresh fullscreen with no fixed-rate multiple mode, where the system showed every 60 Hz-link present one refresh late
// and the link skipped every other tick (docs/reference/frame-rate.md).
static int link_vrr_120(void) {
    int hz = front_screen_max_fps();
    return !g_present_link_env && front_fullscreen() && front_screen_variable_refresh() && hz >= 120 && hz % 120 == 0;
}
static _Atomic int g_test_link_hold;   // DM_TEST_LINK_STALL: a link made during the stall starts paused
static int g_link_mode = 1;   // 1 / 2 as g_dm_present_link, 3 = the 30 Hz power-saving mode
static uint64_t link_probe_ns(void) { return g_link_mode == 3 ? 2 * LINK_PROBE_NS : LINK_PROBE_NS; }
static void *link_main(void *arg) {
    (void)arg;
    pthread_setname_np("nvn-present-link");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    pthread_mutex_lock(&g_pq_mu);   // the present thread first hands over the frame it holds
    g_link_owns_layer = 1;
    while (g_classic_busy) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
    pthread_mutex_unlock(&g_pq_mu);
    NvMetalDisplayLinkDelegate *delegate = [NvMetalDisplayLinkDelegate new];
    CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    link.preferredFrameRateRange = link_range(g_link_mode);
    link.preferredFrameLatency = link_latency();
    MLOG("display link: frame latency %d (%s)", link_latency(), link_latency_why());
    link.delegate = delegate;
    if (atomic_load(&g_test_link_hold)) link.paused = YES;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    pthread_mutex_lock(&g_pq_mu);
    g_link_rl = CFRunLoopGetCurrent(); g_link = link;
    if (g_link_probe_pending) {
        g_link_probe_pending = 0;
        if (!g_rescue_mode && !atomic_load(&g_covered) && g_dm_present_link) {
            g_stall_t0 = 0; g_link_probe_at = now_ns();
            __atomic_store_n(&g_link_tick_ns, g_link_probe_at, __ATOMIC_RELAXED);
        }
        pthread_cond_broadcast(&g_pq_cv);
    }
    pthread_mutex_unlock(&g_pq_mu);
    CFRunLoopRun();   // nextDrawable gets no drawable while the layer has a link attached: pausing is not enough
    g_link_started = 0;
    return NULL;
}
static void link_sync(void) {   // at every present, on the game's thread: the link follows g_dm_present_link and the power-saving mode, which change at run time
    static int applied;
    pthread_mutex_lock(&g_pq_mu);
    int want = g_dm_present_link;
    if (want < 0 || want > 2) { g_dm_present_link = applied == 3 ? 1 : applied; pthread_mutex_unlock(&g_pq_mu); return; }
    int cov = front_window_covered() != 0;
    if (g_rescue_mode && (want != g_rescue_mode || (!cov && now_ns() >= g_rescue_back_at))) {
        g_rescue_mode = 0; g_stall_t0 = 0; __atomic_store_n(&g_link_tick_ns, now_ns(), __ATOMIC_RELAXED);
        g_link_probe_pending = want != 0 && !cov; g_link_probe_at = 0;   // probation starts once the link is live on its run loop
        if (cov) MLOG("display link: rescue ended (window covered; pacing mode %d waits, frame %llu)", want, (unsigned long long)atomic_load(&g_cur_frame));
        else MLOG("display link: rescue ended, trying pacing mode %d (frame %llu)", want, (unsigned long long)atomic_load(&g_cur_frame));
    }
    if (cov != atomic_load(&g_covered)) {
        atomic_store(&g_covered, cov);
        if (!cov) { g_stall_t0 = 0; __atomic_store_n(&g_link_tick_ns, now_ns(), __ATOMIC_RELAXED); }   // the link's silence while covered was no stall
        if (want && !g_rescue_mode) MLOG("display link: window %s (frame %llu)", cov ? "covered, presents follow the host's clock" : "visible, back to the display link", (unsigned long long)atomic_load(&g_cur_frame));
    }
    if (g_rescue_mode || cov) want = 0;
    if (!want) { g_link_probe_pending = 0; g_link_probe_at = 0; }
    pthread_mutex_unlock(&g_pq_mu);
    if (want && mtl_fps_target() == 30 && link_30hz()) want = 3;
    if (want) front_screen_match(want == 3 ? 30 : want == 2 ? 120 : 60);
    static int vrr_was;
    int vrr = want == 1 && link_vrr_120();
    if (vrr != vrr_was) {
        vrr_was = vrr;
        MLOG("display link: %s (frame %llu)", vrr ? "120 Hz in fullscreen on a variable-refresh screen, a frame every other tick (DM_PRESENT_LINK=1 keeps 60 Hz)" : "60 Hz again (window, fixed-rate screen or another frame rate)", (unsigned long long)atomic_load(&g_cur_frame));
    }
    if (vrr) want = 2;
    // Taken only once the link object exists, so a change while its thread starts is not lost.
    static int hz_was;
    int hz = front_screen_max_fps();
    CAMetalDisplayLink *lk = g_link;
    if (lk && want) {
        pthread_mutex_lock(&g_pq_mu);
        if (g_link_probe_pending) {
            g_link_probe_pending = 0; g_link_probe_at = now_ns();
            __atomic_store_n(&g_link_tick_ns, g_link_probe_at, __ATOMIC_RELAXED);
            pthread_cond_broadcast(&g_pq_cv);
        }
        pthread_mutex_unlock(&g_pq_mu);
    }
    if (lk && hz != hz_was) {
        hz_was = hz;
        if (want && want == applied) {
            lk.preferredFrameRateRange = link_range(want);
            MLOG("display link: the screen runs at %d Hz now, range set again (frame %llu)", hz, (unsigned long long)atomic_load(&g_cur_frame));
        }
    }
    int lat = link_latency();
    if (lk && lk.preferredFrameLatency != lat) {
        lk.preferredFrameLatency = lat;
        MLOG("display link: frame latency %d (%s; frame %llu)", lat, link_latency_why(), (unsigned long long)atomic_load(&g_cur_frame));
    }
    // a rescue may ask for mode 0 before an enqueue applied the link's first mode: mode 0 with a link present still invalidates it, or the layer is never handed back
    if (want == applied && (want || !lk)) return;
    g_link_mode = want;
    if (!g_link_started) {
        if (!want) { applied = 0; return; }
        if (!front_metal_layer()) return;
        // Not while the screen settles, so the link starts on the final mode; the present thread shows frames meanwhile.
        static int waited;
        if (front_screen_settling()) {
            if (!waited++) MLOG("display link: waits for fullscreen and the screen's mode to settle (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame));
            return;
        }
        if (waited) { MLOG("display link: the screen settled after %d frames", waited); waited = 0; }
        g_link_started = 1;
        pthread_t th;
        if (pthread_create(&th, NULL, link_main, NULL)) MDIE("cannot start the display link thread");
        pthread_detach(th);
        return;
    }
    CAMetalDisplayLink *link = g_link;
    if (!link) return;
    if (want) link.preferredFrameRateRange = link_range(want);
    else {
        CFRunLoopRef rl = g_link_rl; g_link = nil;
        CFRunLoopPerformBlock(rl, kCFRunLoopDefaultMode, ^{
            [link invalidate];
            pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
            CFRunLoopStop(rl);
        });
        CFRunLoopWakeUp(rl);
    }
    if (want == applied) MLOG("display link: returning the layer to the present thread during startup (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame));
    else MLOG("present pacing mode %d → %d (frame %llu)", applied, want, (unsigned long long)atomic_load(&g_cur_frame));
    applied = want;
    pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
// In the background the link no longer calls back, so waiting for the queue to empty would never return: queued frames are dropped.
static void present_drop_for_suspend(void) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_link_owns_layer) {
        uint64_t n = g_pq_in - g_pq_read;
        pq_drop(g_pq_read + 1, g_pq_in);
        g_pq_read = g_pq_in;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < g_pq_in) g_pq_done = g_pq_in;
        pthread_cond_broadcast(&g_pq_cv);
        if (n) MLOG("entering the background: dropping %llu queued frames not yet displayed (the display link does not fire in the background)", (unsigned long long)n);
    }
    pthread_mutex_unlock(&g_pq_mu);
}
static void *present_main(void *arg) {
    (void)arg;
    pthread_setname_np("nvn-present");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp);
    atomic_store(&g_present_qos, (uint32_t)qc);
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_read == g_pq_in || g_link_owns_layer) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // empty means all taken
        pq_item it = g_pq[g_pq_read % PQ];
        uint64_t ticket = ++g_pq_read; g_classic_busy = 1;
        pthread_mutex_unlock(&g_pq_mu);
        int submitted;
        snap_commit_wait(it.snap_v); sub_commit_wait(it.sub_v);
        @autoreleasepool { submitted = present_now(&it.t, it.period, ticket, it.crop, nil, 0, 0); mtl_tex_unpin(it.t.serial); }
        pthread_mutex_lock(&g_pq_mu); g_classic_busy = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
        if (!submitted) {
            pthread_mutex_lock(&g_pq_mu);
            if (ticket > g_pq_done) g_pq_done = ticket;
            pthread_cond_broadcast(&g_pq_cv);
            pthread_mutex_unlock(&g_pq_mu);
            pres_seen(ticket, -1);   // never reaches the screen: the screen wait must not wait for it
        }
    }
    return NULL;
}
uint32_t mtl_present_qos(void) { return atomic_load(&g_present_qos); }
static void present_start(void) {
    pthread_t th;
    if (pthread_create(&th, NULL, present_main, NULL)) MDIE("cannot start the present thread");
    pthread_detach(th);
}
static _Atomic int g_present_discard;
void mtl_link_pause(int on);
// Presents are dropped while the front end goes to the background and waits for the save; queued frames too, or the game waits for its previous present
// in nvnWindowAcquireTexture and the save is not written.
void mtl_present_discard(int on) {
    atomic_store(&g_present_discard, on != 0);
    if (on) present_drop_for_suspend();
}
static uint64_t present_enqueue(const tex_t *t, double period, const int crop[4], uint64_t *copy);
uint64_t mtl_present(const tex_t *t, double period, const int crop[4], uint64_t *copy) { return present_enqueue(t, period, crop, copy); }
extern _Atomic uint64_t dm_present_ns;
// Present snapshots: each present is copied into a host ring texture right after the frame's work, so the game's texture is free at once and the game runs up to a frame ahead
// (with two swapchain textures the GPU would idle between frames, and macOS holds it at its lowest clock). A slot is reused once its present is processed and its copy completed.
#define SNAP_MAX 4
// old: the slot's texture before the presented size or format last changed, resident until the slot's next use
static struct { id<MTLTexture> tex, old; uint64_t serial; _Atomic uint64_t ticket, ev; } g_snap[SNAP_MAX];
static id<MTL4CommandAllocator> g_snap_alloc[SNAP_MAX];
static id<MTL4CommandBuffer> g_snap_cb[SNAP_MAX];
static id<MTLSharedEvent> g_snap_ev;
static uint64_t g_snap_v;
static _Atomic uint64_t g_snap_next, g_snap_cmt;   // g_snap_cmt: the last copy the encode thread has committed
static int g_snap_n = 2;
int mtl_snap_on(void) {
    static int v = -1;
    if (v >= 0) return v;
    const char *e = getenv("DM_PRESENT_SNAP");
    int n = e ? atoi(e) : DM_GAME_PRESENT_SNAP ? 2 : 0;
    v = n > 0; g_snap_n = n < 2 ? 2 : n > SNAP_MAX ? SNAP_MAX : n;
    if (v) MLOG("present snapshots (DM_PRESENT_SNAP=%d): each present is copied into a ring of %d host textures, so the screen never samples the game's texture", n, g_snap_n);
    return v;
}
static void snap_commit_wait(uint64_t v) { if (v) rec_note_wait(&g_snap_cmt, v); }
static void snap_drain(void) {
    // tickets may have been dropped during a suspend, but their copies are still on the Metal 4 queue
    uint64_t v = g_snap_v;
    if (!v) return;
    snap_commit_wait(v);
    if (![g_snap_ev waitUntilSignaledValue:v timeoutMS:10000]) MDIE("present snapshots: GPU did not drain within 10 s");
}
uint64_t mtl_snap_acquire_wait(void) {
    if (!mtl_snap_on() || atomic_load(&g_present_discard)) return 0;
    int k = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
    uint64_t ticket = atomic_load(&g_snap[k].ticket), ev = atomic_load(&g_snap[k].ev), w0 = ws_t0();
    pthread_mutex_lock(&g_pq_mu); int blocked = ticket > g_pq_done; pthread_mutex_unlock(&g_pq_mu);
    if (ticket) mtl_present_wait(ticket);
    if (ev && !atomic_load(&g_present_discard) && g_snap_ev.signaledValue < ev) {
        blocked = 1;
        // on a timeout the present takes its original texture; the busy allocator is never reset
        if (![g_snap_ev waitUntilSignaledValue:ev timeoutMS:2000]) RED_ONCE(111, "present snapshots: the previous copy into a ring slot did not finish within 2 s");
    }
    if (ws_on()) ws_put(WS_SNAP, w0, blocked);
    return ws_t0() - w0;
}
// 0 = no snapshot: the game's texture is presented
static uint64_t snap_take(const tex_t *t) {
    int k = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
    pthread_mutex_lock(&g_pq_mu); int busy = atomic_load(&g_snap[k].ticket) > g_pq_done; pthread_mutex_unlock(&g_pq_mu);
    uint64_t ev = atomic_load(&g_snap[k].ev);
    if (busy || (ev && g_snap_ev.signaledValue < ev)) return 0;
    // every present that read it was committed before the replacing copy, and g_snap_ev signals after all earlier work: it can leave the residency set
    if (g_snap[k].old) { [g_snap_res removeAllocation:g_snap[k].old]; [g_snap_res commit]; g_snap[k].old = nil; }
    id<MTLTexture> src = tex_mtl(t);
    if (!src) return 0;
    // the copy reads the source before present_now runs: a window texture made just now is outside the committed residency set
    res_commit();
    if (!g_snap_ev) {
        g_snap_ev = [g_dev newSharedEvent];
        if (!g_snap_ev) MDIE("present snapshots: no shared event");
        for (int i = 0; i < g_snap_n; i++) {
            g_snap_alloc[i] = [g_dev newCommandAllocator]; g_snap_cb[i] = [g_dev newCommandBuffer];
            if (!g_snap_alloc[i] || !g_snap_cb[i]) MDIE("present snapshots: no command allocator / buffer");
        }
    }
    id<MTLTexture> dst = g_snap[k].tex;
    if (!dst || dst.width != src.width || dst.height != src.height || dst.pixelFormat != src.pixelFormat) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:src.pixelFormat width:src.width height:src.height mipmapped:NO];
        d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead;
        // resident until the slot's next use: its last present was committed, not necessarily run
        g_snap[k].old = dst;
        dst = [g_dev newTextureWithDescriptor:d];
        if (!dst) { RED_ONCE(112, "present snapshots: no texture"); return 0; }
        [g_snap_res addAllocation:dst]; [g_snap_res commit];
        if (!g_snap[k].serial) { static _Atomic uint64_t n; g_snap[k].serial = (1ull << 61) + atomic_fetch_add(&n, 1) + 1; }
        serial_put(g_snap[k].serial, dst); g_snap[k].tex = dst;
        MLOG("present snapshots: ring slot %d %lux%lu format %lu", k, (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat);
    }
    [g_snap_alloc[k] reset];
    id<MTL4CommandBuffer> cb = g_snap_cb[k];
    [cb beginCommandBufferWithAllocator:g_snap_alloc[k]];
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];   // after the frame's work and the slot's last blit to the screen
    [ce copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(src.width, src.height, 1) toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];   // the game's next writes to its texture come after the copy
    [ce endEncoding]; cb_end(cb, nil);
    uint64_t v = ++g_snap_v;
    if (rec_on() && g_rb_started) {
        if (on_sub_thread()) rec_publish();   // what the submitting thread recorded outside a submission goes first
        rec_blk *b = rec_own_begin();
        rec_t *r = rec_own_put(b, RO_COMMIT_REAL, 2); r->a[0] = 0; r->a[1] = rec_obj(cb);
        r = rec_own_put(b, RO_SIGNAL, 1); r->a[0] = rec_obj(g_snap_ev); r->a[1] = v;
        r = rec_own_put(b, RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&g_snap_cmt; r->a[1] = v;
        rec_own_push(b);
    } else {
        pthread_mutex_lock(&g_res_commit_mu);
        [g_q4 commit:&cb count:1]; [g_q4 signalEvent:g_snap_ev value:v];
        pthread_mutex_unlock(&g_res_commit_mu);
        atomic_store(&g_snap_cmt, v);
    }
    g_snap[k].ev = v;
    return g_snap[k].serial;
}
// copy (may be NULL): the g_snap_ev value of this present's snapshot copy, 0 when none
static uint64_t present_enqueue(const tex_t *t, double period, const int crop[4], uint64_t *copy) {
    atomic_store(&dm_present_ns, now_ns());
    if (copy) *copy = 0;
    if (atomic_load(&g_present_discard)) return 0;   // ticket 0 = no pending present: the swapchain texture's acquire does not wait
    // Resolved here, on the thread that may open the upload batch, and the batch committed, so the display command buffer comes after the upload on the queue.
    if (on_sub_thread()) { tex_mtl(t); if (tex_from_storage(t)) up_flush(); }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, present_start);
    link_sync();
    {   // DM_TEST_LINK_STALL=<start s>,<duration s>: pauses the link, to check the fallback
        static double t0, at = -1, dur;
        static int st;
        double now = CACurrentMediaTime();
        if (at < 0) {
            at = 0;
            const char *e = getenv("DM_TEST_LINK_STALL");
            if (e && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0) { t0 = now; st = 1; } else at = 0;
        }
        if (st == 1 && link_on() && now - t0 >= at) {
            st = 2; atomic_store(&g_test_link_hold, 1); mtl_link_pause(1);
            MLOG("DM_TEST_LINK_STALL: pausing the display link for %.1f s (frame %llu)", dur, (unsigned long long)atomic_load(&g_cur_frame));
        } else if (st == 2 && now - t0 >= at + dur) {
            st = 3; atomic_store(&g_test_link_hold, 0); mtl_link_pause(0);
            MLOG("DM_TEST_LINK_STALL: resuming the display link (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame));
        }
    }
    if (ws_on()) {
        static uint64_t f0;
        uint64_t f = atomic_load(&g_cur_frame);
        if (!f0) f0 = f;
        else if (f >= f0 + 300) { ws_report(f, f - f0); f0 = f; }
    }
    tex_t snap; uint64_t ss = 0; int slot = 0;
    if (mtl_snap_on()) {
        slot = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
        if ((ss = snap_take(t))) {
            memset(&snap, 0, sizeof snap); snap.magic = MAGIC('T', 'E', 'X', 0); snap.serial = ss;
            t = &snap;
        }
    }
    mtl_tex_pin(t->serial);
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_in - g_pq_done >= PQ) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // only when the queue is full: AcquireTexture usually blocks first
    pq_item *it = &g_pq[g_pq_in % PQ]; *it = (pq_item){*t, period, atomic_load(&g_cur_frame), {0}, CACurrentMediaTime(), ss ? g_snap[slot].ev : 0, !ss && on_sub_thread() ? g_fence : 0};
    if (crop) memcpy(it->crop, crop, sizeof it->crop);
    uint64_t tk = ++g_pq_in;
    g_pres_tin[tk % PRING] = it->t_in;
    g_pres_frm[tk % PRING] = it->frame;
    if (ss) {
        g_snap[slot].ticket = tk; atomic_fetch_add(&g_snap_next, 1);
        if (copy) *copy = g_snap[slot].ev;
    }
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    // the game's next acquire of this texture waits for the copy, not for the screen
    return tk;
}
// Without the display link, nothing ties the acquire's clock to what the screen shows, so each late present would add a period to the queue for good. As on the Switch,
// the acquire also waits until at most DM_PRESENT_DEPTH queued presents are not yet shown; it gives up after two periods until the handlers report again.
uint64_t mtl_present_screen_wait(double period) {
    static int mode = -1, silent;   // silent: the wait gave up while the newest present seen was silent_seen
    static uint64_t silent_seen;
    if (mode < 0) {
        const char *e = getenv("DM_PRESENT_DEPTH");
        int d = e && *e ? atoi(e) : 1;
        g_sdw_depth = d < 0 ? 0 : d > PQ ? PQ : d;
        mode = sw_mode("DM_PRESENT_SCREEN", 1);
        MLOG("present: without the display link the acquire waits until at most %d present(s) queued are not yet on screen (DM_PRESENT_DEPTH; DM_PRESENT_SCREEN %s)", g_sdw_depth, mode == 1 ? "on" : mode ? "ab" : "off");
    }
    if (!sw_on(mode) || link_on() || atomic_load(&g_covered) || front_window_covered() || mtl_snap_on() || atomic_load(&g_present_discard)) return 0;   // covered: no screen to wait for
    if (!(period > 0)) period = 1.0 / 60;
    uint64_t t0 = now_ns(); int waited = 0, to = 0;
    pthread_mutex_lock(&g_pq_mu);
    // a flag, not the ticket: a screen that has reported no present at all is silent too
    if (silent && atomic_load(&g_pres_seen) == silent_seen) { pthread_mutex_unlock(&g_pq_mu); return 0; }
    silent = 0;
    struct timespec ts = link_deadline((uint64_t)(2 * period * 1e9));
    // Counted in the game's presents, not the replay's: g_pq_in grows when the queue worker replays a present, a variable time later.
    for (;;) {
        uint64_t rf = atomic_load(&g_rec_frame), want, have; int byframe = rf > 0;
        if (byframe) { want = rf > (uint64_t)g_sdw_depth ? rf - (uint64_t)g_sdw_depth : 0; have = atomic_load(&g_seen_frame); }
        else { want = g_pq_in > (uint64_t)g_sdw_depth ? g_pq_in - (uint64_t)g_sdw_depth : 0; have = atomic_load(&g_pres_seen); }
        if (have >= want || atomic_load(&g_present_discard)) break;
        waited = 1;
        if (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) == ETIMEDOUT) {
            if ((byframe ? atomic_load(&g_seen_frame) : atomic_load(&g_pres_seen)) < want) { to = 1; silent = 1; silent_seen = atomic_load(&g_pres_seen); }
            break;
        }
    }
    pthread_mutex_unlock(&g_pq_mu);
    uint64_t dt = now_ns() - t0;
    if (waited) { atomic_fetch_add(&g_sdw_n, 1); atomic_fetch_add(&g_sdw_ns, dt); }
    if (to) {
        atomic_fetch_add(&g_sdw_to, 1);
        static int nred;
        if (nred++ < 3) MLOG("present: the screen reported no frame for %.0f ms while %llu presents wait (newest seen %llu); the acquire stops waiting for the screen until it reports again", 2 * period * 1e3, (unsigned long long)g_pq_in, (unsigned long long)atomic_load(&g_pres_seen));
    }
    return waited ? dt : 0;
}
// With a snapshot (ev != 0) the texture is the game's again once its copy has run: the game never has more frames in flight than its swapchain holds.
void mtl_present_wait_texture(uint64_t tk, uint64_t ev) {
    if (!ev) { mtl_present_wait(tk); return; }
    if (atomic_load(&g_present_discard) || g_snap_ev.signaledValue >= ev) return;
    if (![g_snap_ev waitUntilSignaledValue:ev timeoutMS:2000]) RED_ONCE(164, "present snapshots: a texture's copy did not finish within 2 s; the game draws into it again");
}
// Not during discard: a frame queued just after the flag was set has nobody to take it. Not forever in display-link mode (STALL_NS).
void mtl_present_wait(uint64_t tk) {
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_done < tk && !atomic_load(&g_present_discard)) {
        if (!g_link_owns_layer) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }
        if (g_pq_read >= tk) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }   // taken by the display link already: until it is handed to the GPU
        uint64_t pns = link_probe_ns(), wait_ns = STALL_NS;
        if (g_link_probe_at && link_watchdog_active()) {
            uint64_t pnow = now_ns(), psince = dm_focus_in_since(), pfrom = g_link_probe_at > psince ? g_link_probe_at : psince;
            uint64_t pel = pnow >= pfrom ? pnow - pfrom : 0, rem = pel >= pns ? 0 : pns - pel;
            if (rem < wait_ns) wait_ns = rem;
        }
        struct timespec ts = link_deadline(wait_ns);
        // already in a stall: drop at once instead of waiting in vain each frame
        if (!g_stall_t0 && wait_ns && (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT || g_pq_done >= tk)) continue;
        uint64_t now = now_ns(), last = __atomic_load_n(&g_link_tick_ns, __ATOMIC_RELAXED);
        if (g_pq_read >= tk) continue;
        int active = link_watchdog_active(); uint64_t since = dm_focus_in_since();
        uint64_t pfrom = g_link_probe_at > since ? g_link_probe_at : since;
        uint64_t pelapsed = now >= pfrom ? now - pfrom : 0;
        int probe_failed = active && g_link_probe_at && pelapsed >= pns;
        if (!probe_failed && (now < last || now - last < STALL_NS)) {
            ts = link_deadline(wait_ns ? wait_ns : STALL_NS);
            pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts);
            continue;
        }
        uint64_t n = tk - g_pq_read;
        pq_drop(g_pq_read + 1, tk);
        g_pq_read = tk;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < tk) g_pq_done = tk;
        pthread_cond_broadcast(&g_pq_cv);
        if (!g_stall_t0) {
            g_stall_t0 = last ? last : now;
            g_stall_drop = 0; g_stall_red = 0; g_stall_logged = 0; g_stall_n++;
        }
        g_stall_drop += n;
        active = link_watchdog_active(); since = dm_focus_in_since();
        uint64_t from = g_stall_t0 > since ? g_stall_t0 : since;
        uint64_t elapsed = now >= from ? now - from : 0;   // focus may have come back after the now sample
        pfrom = g_link_probe_at > since ? g_link_probe_at : since;
        pelapsed = now >= pfrom ? now - pfrom : 0;
        probe_failed = active && g_link_probe_at && pelapsed >= pns;
        if (active && !g_stall_logged && elapsed >= STALL_NS) {
            g_stall_logged = 1;
            MLOG("display link silent for %.0f ms (active): dropping %llu queued frames not yet displayed, the game continues (stall #%llu; back to normal when the link returns)", elapsed / 1e6, (unsigned long long)n, (unsigned long long)g_stall_n);
        }
        if (active && !g_rescue_mode && g_dm_present_link && (probe_failed || elapsed >= LINK_RESCUE_NS)) {
            g_rescue_mode = g_dm_present_link; g_rescue_n++; g_rescue_back_at = now + g_rescue_back_ns;
            if (probe_failed) {
                g_link_probe_fail++;
                MLOG("display link tried again and silent for %.0f ms: the present thread takes drawables again (rescue #%llu, %llu tries failed), the link is tried again in %.0f s", pelapsed / 1e6, (unsigned long long)g_rescue_n, (unsigned long long)g_link_probe_fail, g_rescue_back_ns / 1e9);
            }
            else MLOG("display link silent for %.0f ms while active: the present thread takes drawables itself (rescue #%llu), the link is tried again in %.0f s", elapsed / 1e6, (unsigned long long)g_rescue_n, g_rescue_back_ns / 1e9);
            g_link_probe_pending = 0; g_link_probe_at = 0;
            g_rescue_back_ns = g_rescue_back_ns < 30000000000ull ? g_rescue_back_ns * 2 : 60000000000ull;
        }
        if (active && !g_stall_red && elapsed >= STALL_RED_NS) {
            g_stall_red = 1;
            MLOG("[RED] display link silent for %.1f s while active; presenting relies entirely on fallback frame drops, the picture is frozen", elapsed / 1e9);
        }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
void mtl_present_flush(void) {
    pthread_mutex_lock(&g_pq_mu);
    uint64_t h = g_pq_in;
    pthread_mutex_unlock(&g_pq_mu);
    mtl_present_wait(h);
}
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_pass; *draws = g_stat_draw; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse, uint64_t *patch) { *enc = g_stat_args_enc; *reuse = g_stat_args_reuse; *patch = g_stat_args_patch; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) {
    *resolve = g_vbuf_resolve; *reuse = g_vbuf_reuse; *set = g_vbuf_set; *skip = g_vbuf_skip; *copies = g_vbuf_copies; *copy_bytes = g_vbuf_copy_bytes;
}
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = 0; *skip = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = g_bw_load; *store = g_bw_store; *full = g_pass_full_rw; }
uint64_t mtl_udc_saved_bytes(void) { return g_udc_saved_bytes; }
int mtl_has_window(void) { return front_metal_layer() != NULL; }
int mtl_link_active(void) { return link_on(); }
int mtl_link_stalled(void) { return __atomic_load_n(&g_stall_t0, __ATOMIC_RELAXED) != 0; }
void mtl_link_pause(int on) { CAMetalDisplayLink *l = g_link; if (l) l.paused = on != 0; }
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    // A frame already a tick behind starts at once, or a frame slightly over a period would wait a whole one more. A frame presented with interval N blocks the game
    // for N vsyncs on the Switch: a game scaling its time step by the interval would otherwise run at double speed.
    if (vsyncs < 1) vsyncs = 1;
    return mtl_link_wait_ticks(g_link_mode == 3 ? (vsyncs + 1) / 2 : vsyncs * (g_link_mode == 2 ? 2 : 1), timeout_ns);
}
void mtl_link_mode_set(int mode) { pthread_mutex_lock(&g_pq_mu); g_dm_present_link = mode; pthread_mutex_unlock(&g_pq_mu); }   // 120 fps follows the screen: link_sync applies it
int mtl_link_rate(void) { return g_link_mode == 3 ? 30 : g_link_mode == 2 ? 120 : 60; }
// CLOCK_REALTIME, as pthread_cond_timedwait takes it
static struct timespec link_deadline(uint64_t ns) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ns / 1000000000ull; ts.tv_nsec += ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}
static uint64_t g_link_started_tick;   // under g_link_mu: the tick the last wait ended on (only the game's present thread waits)
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    const uint64_t step = ticks < 1 ? 1 : (uint64_t)ticks;
    // a wait that finds its tick already passed is a game that fell behind since: no drain
    const int req = atomic_exchange(&g_link_drain_req, 0), drain = req & (LINK_DRAIN_LATCH - 1), into_latch = !!(req & LINK_DRAIN_LATCH);
    pthread_mutex_lock(&g_link_mu);
    if (g_link_tick_n >= g_link_started_tick + step) {
        g_link_started_tick = g_link_tick_n;
        pthread_mutex_unlock(&g_link_mu);
        atomic_store(&g_link_blocked_ns, 0);
        return 1;
    }
    uint64_t w0 = now_ns();
    struct timespec ts = link_deadline(timeout_ns);
    g_link_waiter = drain > 0 ? 0 : g_link_started_tick + step;
    while (g_link_tick_n < g_link_started_tick + step)
        if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) {
            g_link_waiter = 0;
            pthread_mutex_unlock(&g_link_mu);
            atomic_store(&g_link_blocked_ns, 0);
            return 0;
        }
    g_link_waiter = 0;
    uint64_t w1 = now_ns(), drained = 0;
    atomic_store(&g_link_blocked_ns, w1 - w0);
    if (drain > 0) {   // a link that goes silent meanwhile ends the drain after two tick periods each
        const uint64_t until = g_link_tick_n + (uint64_t)drain;
        ts = link_deadline((uint64_t)drain * 2000000000ull / (uint64_t)mtl_link_rate());
        if (into_latch) { g_link_waiter = until; g_link_waiter_end = 1; }   // by its end tick the queue has run empty, and that callback latches this frame's present
        while (g_link_tick_n < until) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) break;
        g_link_waiter = 0; g_link_waiter_end = 0;
        drained = now_ns() - w1;
    }
    g_link_started_tick = g_link_tick_n;
    pthread_mutex_unlock(&g_link_mu);
    if (drain > 0) {
        atomic_fetch_add(&g_link_drain_ns, drained);
        uint64_t n = atomic_fetch_add(&g_link_drained, 1) + 1;
        if (n <= 20) MLOG("display link: frames stood in the present queue while the game kept up, the acquire waited %d more tick%s (%.1f ms) to let them out (drain #%llu, frame %llu%s)",
                          drain, drain == 1 ? "" : "s", drained / 1e6, (unsigned long long)n, (unsigned long long)atomic_load(&g_cur_frame), n == 20 ? "; further drains only counted" : "");
    }
    return 1;
}
// the caller counts it as pacing, not as a frame that took longer
uint64_t mtl_link_drain_take_ns(void) { return atomic_exchange(&g_link_drain_ns, 0); }

// 0 when the link does not run, runs at 30 Hz or gives no tick within the timeout: the caller then keeps its own clock
int nvn_vsync_hz(void);
int mtl_link_wait_vsync(uint64_t *seen, uint64_t timeout_ns) {
    if (!mtl_link_active() || g_link_mode == 3) return 0;
    const uint64_t step = g_link_mode == 2 && nvn_vsync_hz() != 120 ? 2 : 1;
    pthread_mutex_lock(&g_link_mu);
    if (*seen > g_link_tick_n || g_link_tick_n > *seen + 4 * step) *seen = g_link_tick_n;   // first call, or far behind: from the current tick on, no burst
    struct timespec ts = link_deadline(timeout_ns);
    while (g_link_tick_n < *seen + step)
        if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    *seen += step;
    pthread_mutex_unlock(&g_link_mu);
    return 1;
}

void mtl_wait_state_dump(void) {
    MLOG("wait points: main fence submitted %llu / completed %llu; main allocator fences [%llu %llu %llu]; uploads pending %d, allocators busy [%d %d %d], seq %u, unflushed batch %d; present queue in %llu / done %llu; main command buffer encoding %d, submit thread set %d",
         (unsigned long long)g_fence, (unsigned long long)g_done, (unsigned long long)g_alloc_fence[0], (unsigned long long)g_alloc_fence[1], (unsigned long long)g_alloc_fence[2],
         atomic_load(&g_up_pending), atomic_load(&g_up_alloc_busy[0]), atomic_load(&g_up_alloc_busy[1]), atomic_load(&g_up_alloc_busy[2]), g_up_seq, g_up_cb != nil,
         (unsigned long long)g_pq_in, (unsigned long long)g_pq_done, g_cb != nil, atomic_load(&g_sub_thread_set));
}
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = g_alloc_waits; *grown = g_alloc_grown; *n = g_alloc_n; }
// Memory census (diagnostic). Under no lock of the caller's.
static char census_flags_key;
static id<MTLTexture> census_base(id in) {
    id o = [in isKindOfClass:[NvPkg class]] ? ((NvPkg *)in)->tex : in;
    if (![o conformsToProtocol:@protocol(MTLTexture)]) return nil;
    id<MTLTexture> t = o;
    return t.parentTexture ? t.parentTexture : t;
}
void mem_census(const char *why) { @autoreleasepool {
    NSHashTable *named = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    NSHashTable *named_ret = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    // Retained under the lock that names it until the census returns: the tables hold bare pointers, and a texture can be freed while the census runs on its own thread.
    NSMutableSet *keep = [NSMutableSet set];
    #define CN_ADD(o) do { id<MTLTexture> b_ = census_base(o); if (b_) { [named addObject:(__bridge id)(__bridge void *)b_]; [keep addObject:b_]; } } while (0)
    pthread_mutex_lock(&g_smu);
    void (^census_one)(id o) = ^(id o) { CN_ADD(o); if ([o isKindOfClass:[NvPkg class]] && ((NvPkg *)o)->spare) CN_ADD(((NvPkg *)o)->spare); };
    own_each_locked(^(uint64_t serial, id o, int ghost) { (void)serial; (void)ghost; census_one(o); });
    own_alias_each_locked(^(const alias_key *k, id o) { (void)k; census_one(o); });
    pthread_mutex_unlock(&g_smu);
    NSMapTable *by_mem = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality valueOptions:NSPointerFunctionsStrongMemory];
    NSMapTable *by_flags = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality valueOptions:NSPointerFunctionsStrongMemory];   // texture flags | pool flags << 32
    os_unfair_lock_lock(&g_rmu);
    uint64_t nreg = 0, nretired = 0;
    for (int i = 1; i < g_ntreg; i++) {
        treg_t *r = &g_treg[i];
        if (!r->ok) continue;
        nreg++; nretired += r->retired;
        void *vs[7] = {r->view, r->alt, r->im, r->palt[0], r->palt[1], r->palt[2], r->palt[3]};
        for (int k = 0; k < 7; k++) if (vs[k]) {
            id<MTLTexture> b = census_base((__bridge id)vs[k]);
            if (!b) continue;
            [r->retired ? named_ret : named addObject:(__bridge id)(__bridge void *)b]; [keep addObject:b];
            if (k == 0 && r->t.mem) {
                const void *key = r->t.mem;
                NSHashTable *set = [by_mem objectForKey:(__bridge id)key];
                if (!set) { set = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality]; [by_mem setObject:set forKey:(__bridge id)key]; }
                [set addObject:(__bridge id)(__bridge void *)b];
                if (![by_flags objectForKey:(__bridge id)key]) [by_flags setObject:@(((uint64_t)(r->t.pool && r->t.pool->magic == MAGIC('P', 'O', 'O', 'L') ? r->t.pool->flags : 0) << 32) | r->t.d.flags) forKey:(__bridge id)key];
            }
        }
    }
    os_unfair_lock_unlock(&g_rmu);
    NSHashTable *waiting = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    pthread_mutex_lock(&g_deferred_mu);
    for (NvDeferredRelease *d in g_deferred) {
        id<MTLTexture> b = census_base(d->object);
        if (b) { [waiting addObject:(__bridge id)(__bridge void *)b]; [keep addObject:b]; }
    }
    pthread_mutex_unlock(&g_deferred_mu);
    uint64_t small_n[2][2] = {{0}}, small_b[2][2] = {{0}};
    uint64_t n_ret = 0, b_ret = 0, n_named = 0, b_named = 0, n_wait = 0, b_wait = 0, n_none = 0, b_none = 0, n_buf = 0, b_buf = 0;
    enum { NF = 24 };
    struct { unsigned long pf; uint64_t n, b; } fm[NF] = {{0}};
    int nfm = 0;
    pthread_mutex_lock(&g_res_mu);
    for (id<MTLAllocation> o in g_res_objects.allObjects) {
        uint64_t z = o.allocatedSize;
        if (z <= 65536) {
            int tx = [(id)o conformsToProtocol:@protocol(MTLTexture)], sh = ((id<MTLResource>)o).storageMode == MTLStorageModeShared;
            small_n[tx][sh]++; small_b[tx][sh] += z;
        }
        if (![(id)o conformsToProtocol:@protocol(MTLTexture)]) { n_buf++; b_buf += z; continue; }
        id<MTLTexture> t = (id)o;
        if (t.parentTexture) continue;
        unsigned long pf = (unsigned long)t.pixelFormat;
        int i = 0;
        while (i < nfm && fm[i].pf != pf) i++;
        if (i == nfm && nfm < NF) fm[nfm++].pf = pf;
        if (i < NF) { fm[i].n++; fm[i].b += z; }
        if ([named containsObject:(__bridge id)(__bridge void *)t]) { n_named++; b_named += z; }
        else if ([named_ret containsObject:(__bridge id)(__bridge void *)t]) { n_ret++; b_ret += z; }
        else if ([waiting containsObject:(__bridge id)(__bridge void *)t]) { n_wait++; b_wait += z; }
        else { n_none++; b_none += z; }
    }
    pthread_mutex_unlock(&g_res_mu);
    uint64_t dup_addr = 0, dup_tex = 0, dup_b = 0;
    for (id key in by_mem) {
        NSHashTable *set = [by_mem objectForKey:key];
        if (set.count < 2) continue;
        dup_addr++;
        uint64_t biggest = 0, sum = 0;
        for (id b in set) { uint64_t z = ((id<MTLTexture>)b).allocatedSize; sum += z; if (z > biggest) biggest = z; }
        dup_tex += set.count - 1; dup_b += sum - biggest;
    }
    {
        NSMutableArray *groups = [NSMutableArray array];
        for (id key in by_mem) {
            NSHashTable *set = [by_mem objectForKey:key];
            if (set.count >= 2) { [groups addObject:set]; objc_setAssociatedObject(set, &census_flags_key, [by_flags objectForKey:key], OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
        }
        [groups sortUsingComparator:^NSComparisonResult(NSHashTable *x, NSHashTable *y) {
            uint64_t a_ = 0, b_ = 0;
            for (id t in x) a_ += ((id<MTLTexture>)t).allocatedSize;
            for (id t in y) b_ += ((id<MTLTexture>)t).allocatedSize;
            return a_ > b_ ? NSOrderedAscending : a_ < b_ ? NSOrderedDescending : NSOrderedSame;
        }];
        for (NSUInteger g = 0; g < groups.count && g < 8; g++) {
            char gl[700]; size_t go = 0;
            for (id<MTLTexture> t in groups[g]) if (go < sizeof gl) go += snprintf(gl + go, sizeof gl - go, "%s[fmt %lu %lux%lux%lu type %lu mips %lu slices %lu usage %#lx %lu KB]", go ? " " : "",
                (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth, (unsigned long)t.textureType, (unsigned long)t.mipmapLevelCount,
                (unsigned long)t.arrayLength, (unsigned long)t.usage, (unsigned long)(t.allocatedSize >> 10));
            uint64_t fl = [objc_getAssociatedObject(groups[g], &census_flags_key) unsignedLongLongValue];
            MLOG("%s census: shared storage group %lu (%lu textures, texture flags %#llx, pool flags %#llx): %s", why, (unsigned long)g, (unsigned long)((NSHashTable *)groups[g]).count, fl & 0xffffffffull, fl >> 32, gl);
        }
    }
    for (int i = 0; i < nfm; i++) for (int j = i + 1; j < nfm; j++) if (fm[j].b > fm[i].b) { __typeof__(fm[0]) x = fm[i]; fm[i] = fm[j]; fm[j] = x; }
    char line[900] = "none"; size_t o = 0;
    for (int i = 0; i < nfm && i < 12 && o < sizeof line; i++) o += snprintf(line + o, sizeof line - o, "%s%lu: %llu, %llu MB", i ? "; " : "", fm[i].pf, (unsigned long long)fm[i].n, (unsigned long long)(fm[i].b >> 20));
    MLOG("%s census: textures named by serials, sharing or live descriptors' views %llu, %llu MB; only by retired descriptors' views %llu, %llu MB; only in the deferred-release queue %llu, %llu MB; named by nothing %llu, %llu MB; buffers %llu, %llu MB; descriptors %llu (%llu retired); storage addresses with several Metal textures %llu (%llu extra textures, %llu MB)",
         why, (unsigned long long)n_named, (unsigned long long)(b_named >> 20), (unsigned long long)n_ret, (unsigned long long)(b_ret >> 20), (unsigned long long)n_wait, (unsigned long long)(b_wait >> 20), (unsigned long long)n_none, (unsigned long long)(b_none >> 20),
         (unsigned long long)n_buf, (unsigned long long)(b_buf >> 20), (unsigned long long)nreg, (unsigned long long)nretired, (unsigned long long)dup_addr, (unsigned long long)dup_tex, (unsigned long long)(dup_b >> 20));
    MLOG("%s census: textures by Metal format: %s", why, line);
    MLOG("%s census: residency objects of 64 KB or less: buffers shared %llu (%llu KB), private %llu (%llu KB); textures shared %llu (%llu KB), private %llu (%llu KB)", why,
         (unsigned long long)small_n[0][1], (unsigned long long)(small_b[0][1] >> 10), (unsigned long long)small_n[0][0], (unsigned long long)(small_b[0][0] >> 10),
         (unsigned long long)small_n[1][1], (unsigned long long)(small_b[1][1] >> 10), (unsigned long long)small_n[1][0], (unsigned long long)(small_b[1][0] >> 10));
    {   // 0 only texture uploads, 1 anything else, 2 nothing, 3 both
        uint64_t nb[4] = {0}, bb[4] = {0};
        pthread_mutex_lock(&g_mu);
        for (int i = 0; i < g_npool; i++) {
            pool_ent *p = &g_pools[i];
            int c = p->n_tex_reads && p->n_other ? 3 : p->n_other ? 1 : p->n_tex_reads ? 0 : 2;
            nb[c]++; bb[c] += p->hi - p->lo;
        }
        pthread_mutex_unlock(&g_mu);
        MLOG("%s census: memory pools looked up only by texture uploads %llu, %llu MB; by other uses only %llu, %llu MB; by both %llu, %llu MB; by nothing %llu, %llu MB", why,
             (unsigned long long)nb[0], (unsigned long long)(bb[0] >> 20), (unsigned long long)nb[1], (unsigned long long)(bb[1] >> 20), (unsigned long long)nb[3], (unsigned long long)(bb[3] >> 20), (unsigned long long)nb[2], (unsigned long long)(bb[2] >> 20));
    }
    #undef CN_ADD
} }
