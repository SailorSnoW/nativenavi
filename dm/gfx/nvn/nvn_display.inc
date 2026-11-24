// The present pass, the display link and the present thread, present snapshots, the waits for presents, the power line's counts (mtl_stats), the watchdog's dump and the memory census.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// The present pass: src's valid rectangle drawn into dst (the drawable, or a screenshot's texture) as present_encode draws it, through MetalFX first where fx_encode takes it
// (fx: the scaler state to use, nil = bilinear). uvb: the pass's crop constants (one buffer per present allocator, written by the CPU)
static int present_encode_m4(id<MTL4CommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4], id<MTLBuffer> uvb, fx_state *fx) {
    NSUInteger r[4]; if (!present_crop_rect(src, crop, r)) return 0;
    present_pipeline(dst.pixelFormat);
    double dw = dst.width, dh = dst.height;
    MTLViewport vp = present_fit(dw, dh);
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    int flipped = atomic_load_explicit(&g_origin_ll, memory_order_relaxed), wrote = 0;
    static int direct_ok = -1; if (direct_ok < 0) { const char *e = getenv("DM_FX_DIRECT"); direct_ok = !(e && !strcmp(e, "0")); }
    // The picture fills dst unflipped: MetalFX may write dst itself, as the pass would only copy its output 1:1 (fx_encode)
    id<MTLTexture> tex = present_raw_src(src), up = fx ? fx_encode(fx, cb, tex, r, vp, dst.pixelFormat, fx == &g_fx_present, direct_ok && full && !flipped ? dst : nil, &wrote) : nil;
    if (wrote) return PRESENT_METALFX;
    if (up && fx == &g_fx_present) { static int once; if (!once++)
        MLOG("present upscaling: the present pass draws MetalFX's output into the drawable: %s (drawable %lux%lu format %lu usage %#lx, picture %lux%lu)",
             !direct_ok ? "DM_FX_DIRECT=0" : !full ? "the picture does not fill it" : flipped ? "the lower-left origin flips it" : "the scaler cannot write it",
             (unsigned long)dst.width, (unsigned long)dst.height, (unsigned long)dst.pixelFormat, (unsigned long)dst.usage, (unsigned long)up.width, (unsigned long)up.height); }
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
// The present's picture drawn into an offscreen texture (out: private, render target) the way present_now draws it into the drawable, MetalFX included, on the Metal 4 queue;
// returns whether it was submitted. Completion reports the encoding kind, or PRESENT_FAILED on a GPU error. Resources remain resident and retained until GPU completion.
static int present_offscreen_m4(id<MTLTexture> src, const int crop[4], id<MTLTexture> out, void (^done)(int)) {
    id<MTL4CommandAllocator> al = [g_dev newCommandAllocator]; id<MTL4CommandBuffer> cb = [g_dev newCommandBuffer];
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
// Diagnostic readback and tests wait for the same asynchronous capture used by whole-window screenshots. A timeout leaves cleanup with the GPU completion handler.
static int present_offscreen_wait(id<MTLTexture> src, const int crop[4], id<MTLTexture> out) {
    dispatch_semaphore_t sem = dispatch_semaphore_create(0); __block int kind = PRESENT_FAILED;
    if (!present_offscreen_m4(src, crop, out, ^(int k) { kind = k; dispatch_semaphore_signal(sem); })) return 0;
    if (dispatch_semaphore_wait(sem, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_SEC))) { MLOG("[RED] offscreen present: the GPU did not finish within 10 s"); return 0; }
    return kind;
}
// The present command buffer's GPU time (its commit feedback: from its first to its last command, the MetalFX scaler and the present pass), every 600 presents as p50 / p95, apart
// for presents through MetalFX and through the bilinear pass. Under g_pq_mu (the feedback handler)
static void present_gpu_note(double s, int fx) {
    static double v[2][600]; static int n[2];
    if (!(s > 0 && s < 1)) return;
    v[fx][n[fx]++] = s * 1e3;
    if (n[fx] < 600) return;
    double *a = v[fx]; for (int i = 1; i < 600; i++) { double x = a[i]; int j = i - 1; while (j >= 0 && a[j] > x) { a[j + 1] = a[j]; j--; } a[j + 1] = x; }
    MLOG("present command buffer GPU time over 600 presents %s: p50 %.3f ms, p95 %.3f ms, max %.3f ms", fx ? "through MetalFX" : "through the bilinear pass", a[299], a[569], a[599]);
    n[fx] = 0;
}
static double g_link_it_tin;   // Enqueue time of the frame the display-link callback is presenting (read and written only on the display-link thread)
static int present_now(const tex_t *t, double period, uint64_t frame, uint64_t ticket, const int crop[4], id<CAMetalDrawable> supplied, double target_ts) {
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
    // A texture made on first resolution joins the residency set (res_add), which a submission commits; a window texture presented before any submission has used it was still
    // outside the committed set when the display command buffer sampled it. Super Smash Bros. Ultimate presents its window texture in its first frames before drawing into it:
    // the display queue page-faulted (kIOGPUCommandBufferCallbackErrorPageFault) and macOS then ignored every later command buffer of the process, so the window stayed pink
    // (headless runs never present, and every game before drew into a window texture before presenting it). A commit with nothing new costs nothing
    res_commit();
    // DM_NVN_REGLUM=x0,y0,x1,y1:file (normalized coordinates): read back this region on every present and record "present sequence number, present time ns, region mean luminance, subsampled grayscale thumbnail (hex)" (diagnostic: verify that animation speed is independent of frame rate).
    // A separate command buffer is queued after this frame's rendering (the same queue executes in submit order); does not wait for the GPU and does not disturb frame pacing
    static int rl = -1; static float rr[4]; static FILE *rlf; static pthread_mutex_t rlmu = PTHREAD_MUTEX_INITIALIZER;
    if (rl < 0) { const char *e = getenv("DM_NVN_REGLUM"), *c = e ? strchr(e, ':') : NULL;
        rl = c && sscanf(e, "%f,%f,%f,%f", &rr[0], &rr[1], &rr[2], &rr[3]) == 4 && (rlf = fopen(c + 1, "w")) != NULL;
        if (e && !rl) MLOG("[RED] DM_NVN_REGLUM argument or file invalid: %s", e); }
    if (rl && (src.pixelFormat == MTLPixelFormatRGBA8Unorm || src.pixelFormat == MTLPixelFormatBGRA8Unorm || src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB || src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB)) {
        NSUInteger x0 = (NSUInteger)(rr[0] * src.width), y0 = (NSUInteger)(rr[1] * src.height), w = (NSUInteger)(rr[2] * src.width) - x0, h = (NSUInteger)(rr[3] * src.height) - y0;
        id<MTLBuffer> rb = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(x0, y0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4];
        [b endEncoding];
        unsigned long long n = g_present_n, tns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        [cb addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c; const uint8_t *q = rb.contents; uint64_t sum = 0;
            uint8_t g[4096]; NSUInteger ng = 0;   // Subsampled grayscale thumbnail (every 4th row / column, at most 4096 points), recorded along with the mean, for per-frame image correlation analysis
            for (NSUInteger y = 0; y < h; y += 4) for (NSUInteger x = 0; x < w && ng < sizeof g; x += 4) { const uint8_t *px = q + 4 * (y * w + x); g[ng++] = (uint8_t)((px[0] + px[1] + px[2]) / 3); }
            for (NSUInteger k = 0; k < w * h; k++) sum += q[4 * k] + q[4 * k + 1] + q[4 * k + 2];
            pthread_mutex_lock(&rlmu); fprintf(rlf, "%llu\t%llu\t%.4f\t", n, tns, sum / (3.0 * w * h));
            for (NSUInteger k = 0; k < ng; k++) fprintf(rlf, "%02x", g[k]);
            fputc('\n', rlf); fflush(rlf); pthread_mutex_unlock(&rlmu); }];
        [cb commit];
    }
    // DM_NVN_SHOT=frame number[,frame number...]:path (%d in the path is replaced with the frame number)
    static long shots[4096]; static int nshot = -1; static const char *shot_path;   // up to 4096 frames (a burst over a frame range, for scoring frame by frame)
    if (nshot < 0) {
        nshot = 0; const char *e = getenv("DM_NVN_SHOT"), *c = e ? strchr(e, ':') : NULL;
        if (c) { shot_path = c + 1; for (const char *q = e; q < c && nshot < 4096; q = strchr(q, ',') ? strchr(q, ',') + 1 : c) shots[nshot++] = atol(q); }
    }
    for (int i = 0; i < nshot; i++) if ((long)g_present_n == shots[i]) { char path[1024]; snprintf(path, sizeof path, shot_path, (int)g_present_n); shot(src, path, 1, crop); }
    pthread_mutex_lock(&g_shotq_mu);
    for (int i = 0; i < g_nshotq;) if (g_present_n >= g_shotq[i].frame) { shot(src, g_shotq[i].path, 1, crop); g_shotq[i] = g_shotq[--g_nshotq]; } else i++;
    pthread_mutex_unlock(&g_shotq_mu);

    if (atomic_load(&g_gpu_lost)) return 0;   // the display command buffer would be ignored too: take no drawable from the screen the front end is about to close
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    if (!layer) return 0;
    if (!layer.displaySyncEnabled) layer.displaySyncEnabled = YES;   // Windowed mode follows the display's vsync to prevent tearing
    present_pipeline(layer.pixelFormat);   // Same as the original path: the first frame compiles the pipeline before taking the drawable
    if (g_grab_cb) grab_present(src, crop, layer.drawableSize);   // Game picture needed by the whole-window screenshot (peek without the lock first; the real take locks inside the function)
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
    id<MTLResidencySet> drs = ((CAMetalLayer *)dr.layer).residencySet;   // the drawable's
    if (!kind) { cb_end(cb, drs); [g_q4 signalDrawable:dr]; return 0; }
    int fx_kind = kind == PRESENT_METALFX;
    cb_end(cb, drs);
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) { MLOG("[RED] display command buffer error: %s", f.error.description.UTF8String); gpu_lost_check(f.error, "display"); }
        pthread_mutex_lock(&g_pq_mu); if (ticket > g_pq_done) g_pq_done = ticket; if (ticket > g_present_alloc_done[alloc_i]) g_present_alloc_done[alloc_i] = ticket; g_pq_inflight--; pthread_cond_broadcast(&g_pq_cv);
        present_gpu_note(f.GPUEndTime - f.GPUStartTime, fx_kind); pthread_mutex_unlock(&g_pq_mu);
    }];
    id<MTL4CommandBuffer> buffers[] = {cb};
    pthread_mutex_lock(&g_pq_mu); g_pq_inflight++; pthread_mutex_unlock(&g_pq_mu);
    [g_q4 commit:buffers count:1 options:o];
    if (target_ts > 0) {
        static double leads[300], e2e[300];   // e2e: game enqueue -> system-scheduled present (including the time waiting in the queue for the display-link callback), the full span of input latency that our side can read
        static uint64_t lead_n;
        leads[lead_n % 300] = (target_ts - CACurrentMediaTime()) * 1000.0; e2e[lead_n % 300] = g_link_it_tin > 0 ? (target_ts - g_link_it_tin) * 1000.0 : 0;
        lead_n++;
        if (lead_n % 300 == 0) {
            double a[300]; memcpy(a, leads, sizeof a);
            for (int i = 1; i < 300; i++) { double v = a[i]; int j = i - 1; while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; } a[j + 1] = v; }
            double b[300]; memcpy(b, e2e, sizeof b);
            for (int i = 1; i < 300; i++) { double v = b[i]; int j = i - 1; while (j >= 0 && b[j] > v) { b[j + 1] = b[j]; j--; } b[j + 1] = v; }
            MLOG("display link up to tick %llu: ticks 300, presents %llu, empty ticks %llu (held for the present interval %llu), old frames dropped by merging %llu; submission ahead of target display p50 %.3f ms / min %.3f ms; game enqueue to target display p50 %.3f ms / min %.3f / max %.3f ms; standing frames drained %llu times; late latch took %llu presents, timed out %llu times",
                 (unsigned long long)g_link_tick_n, (unsigned long long)g_present_n, (unsigned long long)g_link_empty, (unsigned long long)g_link_held,
                 (unsigned long long)g_link_dropped, a[149], a[0], b[149], b[0], b[299], (unsigned long long)atomic_load(&g_link_drained), (unsigned long long)g_link_latched, (unsigned long long)g_link_latch_out);
        }
    }
    [g_q4 signalDrawable:dr];
    g_present_alloc_fence[alloc_i] = ticket;
    uint64_t pn = ticket;   // Record the present time by the game's present sequence number (ticket number): hle's present statistics are taken by "the game's nth present"; in the display link path the actual present count != the game's present count (old frames get merged and dropped)
    [dr addPresentedHandler:^(id<MTLDrawable> d) {   // Actual presentation time (the time this frame is really put on screen, CACurrentMediaTime base)
        CFTimeInterval tt = d.presentedTime;
        nvn_present_history_put(&g_present_history, pn, tt > 0 ? tt : -1);
        pres_seen(pn, tt);
    }];
    // Each frame is displayed for at least one present period: when the GPU completion time jitters around a 120 Hz refresh boundary, the paired "25 ms + 8.33 ms" stutter no longer appears
    if (target_ts > 0) [dr present];
    else if (period > 0) [dr presentAfterMinimumDuration:period]; else [dr present];
    (void)frame;
    return 1;
}
// ---------- Present thread ----------
// NVN semantics: QueuePresentTexture only queues the present and returns immediately (the game often calls it while holding a lock: waiting for nextDrawable inside it
// would hold the lock for most of each frame and starve the loading thread). Taking the drawable, blit and present are all done on this thread in enqueue order.
// Ticket number = which enqueue it is (starting from 1); g_pq_done = the ticket number that has been processed (blit committed to the GPU queue).
// Swapchain texture return: before AcquireTexture gets a texture it waits for the ticket of that texture's previous present to be processed (mtl_present_wait), after which the commands the game draws into it
// are necessarily ordered after that blit on the GPU queue.
#define PQ 8
typedef struct { tex_t t; double period; uint64_t frame; int crop[4]; double t_in; uint64_t snap_v, sub_v; } pq_item;   // snap_v: the snapshot copy this present shows (snap_commit_wait); sub_v: the submissions a present of the game's own texture follows (sub_commit_wait)   // t_in: the time the game handed this frame into the present queue (CACurrentMediaTime); ev_val: see g_ext_ev
static void snap_commit_wait(uint64_t v);
static pq_item g_pq[PQ];
static uint64_t g_pq_in;
static double g_link_vperiod, g_link_vhalf;   // virtual ticks (link_range): seconds per counted tick (0 = every callback is a tick) and half a refresh of the screen

@interface NvMetalDisplayLinkDelegate : NSObject <CAMetalDisplayLinkDelegate>
@end

// Fallback for the display link going silent while in the foreground (two smoke-test hangs before merge: the display link stopped calling back while the app was in the foreground, nobody takes the queued frames, and the game waits forever for its previous present to be processed before "acquiring the next swapchain texture"):
// when mtl_present_wait has waited longer than STALL_NS and the display link has not called back for that long, drop the waiting frame and earlier queued frames as when entering the background (accounted as dropped frames) and let the game keep running; when the display link comes back things return to normal automatically.
// One stall logs one hint line, and one line on recovery (neither is a failure); only when the same stall lasts longer than STALL_RED_NS is it a failure. All under g_pq_mu
#define STALL_NS 100000000ull
#define STALL_RED_NS 2000000000ull
static uint64_t g_stall_t0, g_stall_drop, g_stall_n; static int g_stall_red, g_stall_logged;
// When the display link stays silent for LINK_RESCUE_NS while the app is active, presents stop waiting for it: mode 0 (the present thread takes drawables itself), so the
// picture keeps moving instead of standing still on dropped frames (d2r-mobile afad65c, via MacNativeGames cf1e629:
// macOS pauses the link of a window it takes for covered). The link is tried again after g_rescue_back_ns (link_sync), which doubles on each new
// stall up to 60 s. The requested g_dm_present_link stays unchanged; all rescue state changes take g_pq_mu, and link_sync handles the link object on the enqueue thread.
#define LINK_RESCUE_NS 500000000ull
static _Atomic int g_rescue_mode;   // Also read by link_on without g_pq_mu, to stop pacing on silent ticks immediately
static uint64_t g_rescue_back_at, g_rescue_back_ns = 3000000000ull, g_rescue_n;
// A link tried again after a rescue is on probation: once attached to its run loop, one that has not called back within link_probe_ns() of active time goes back to the rescue
// at once instead of waiting STALL_NS (100 ms) and dropping frames for LINK_RESCUE_NS (500 ms). The first accepted callback ends probation. Under g_pq_mu
#define LINK_PROBE_NS 67000000ull
static int g_link_probe_pending;
static uint64_t g_link_probe_at, g_link_probe_fail;
// The display link watchdog only recognizes the "active" state: when the app loses active status (the window is not key) and in the background the system does not call back anyway, and the picture is covered too -- the frame-drop fallback stays as before,
// but it is not counted as stuck and no failure is reported; the stall is measured from the later of "the most recent callback / the most recent activation" (A16 logs: 8–17 seconds inactive and 17 seconds in the background were both reported as "stuck in foreground")
__attribute__((weak)) int dm_focus_now(void) { return 1; }   // The host's hle_oe.c provides the strong definition; when graphics unit tests do not link the host it is treated as always active
__attribute__((weak)) uint64_t dm_focus_in_since(void) { return 0; }
// DM_PRESENT_DRAIN=1 / 0: whether frames standing in the present queue are drained (nvn_link_drain.h). On by default
static int g_link_drain_on = -1;   // -1 = not read yet (file-level so that t_present_rescue can set it)
static int link_drain_on(void) {
    if (g_link_drain_on < 0) {
        const char *e = getenv("DM_PRESENT_DRAIN"); g_link_drain_on = e ? atoi(e) != 0 : 1;
        MLOG("display link: frames standing in the present queue are %s (DM_PRESENT_DRAIN)", g_link_drain_on ? "drained: the acquire waits them out once the game has kept up for 2 s" : "left standing");
    }
    return g_link_drain_on;
}
// DM_PRESENT_LATCH=1 / 0: whether the callback that woke the game's acquire waits for its present (nvn_link_latch.h). On by default
static int g_link_latch_on = -1;   // -1 = not read yet
static int link_latch_on(void) {
    if (g_link_latch_on < 0) {
        const char *e = getenv("DM_PRESENT_LATCH"); g_link_latch_on = e ? atoi(e) != 0 : 1;
        MLOG("display link: %s (DM_PRESENT_LATCH)", g_link_latch_on ? "the callback that woke the acquire waits briefly for its present and takes it" : "each callback takes the frame already queued");
    }
    return g_link_latch_on;
}
// Once per frame the display-link callback takes (its thread): cb the callback's time, age how long the frame waited in the queue (both seconds), dropped whether older
// frames were merged away on this tick, lead and room the frame's present after the callback that woke its acquire and that callback's room (nvn_link_latch_spare; room 0
// when the latch does not run for it). Asks the next acquire for the ticks the queued frames have to spare (g_link_drain_req). Not with present snapshots, which run the
// game a frame ahead on purpose, and not on the 120 Hz link: there the system already shows presents late in variable-refresh fullscreen (link_vrr_120) with the queue's
// slack in place, and a drain there is unmeasured
static void link_drain_note(double cb, double age, int dropped, double lead, double room) {
    if (!link_drain_on()) return;
    int rate = mtl_link_rate(), ok = !dropped && rate != 120 && !mtl_snap_on();
    int k = nvn_link_drain_next(&g_link_drain, cb, age, 1.0 / rate, atomic_load(&g_link_blocked_ns) / 1e9, ok, lead, room);
    if (k) atomic_store(&g_link_drain_req, k);
    if (k & LINK_DRAIN_LATCH) g_link_latch_misses = 0;   // its early frames show the presents come in time again: the end of the drain latches whatever the give-up said
}
static struct timespec link_deadline(uint64_t ns);
@implementation NvMetalDisplayLinkDelegate
- (void)metalDisplayLink:(CAMetalDisplayLink *)link needsUpdate:(CAMetalDisplayLinkUpdate *)update {
    (void)link;
    double cb = CACurrentMediaTime();
    pthread_mutex_lock(&g_pq_mu);
    // A callback that cannot take a present must not reset the watchdog or count a pacing tick. The next enqueue that hands off the layer may be waiting for a texture.
    if (!g_dm_present_link || g_rescue_mode) { pthread_mutex_unlock(&g_pq_mu); return; }
    g_link_probe_at = 0;   // the tried link called back: off probation
    if (g_stall_t0) { uint64_t since = dm_focus_in_since(), t = now_ns();
        MLOG("display link recovered: stalled %.0f ms (%.0f ms of it active), fallback dropped %llu frames (stall #%llu)", (t - g_stall_t0) / 1e6, (t - (g_stall_t0 > since ? g_stall_t0 : since)) / 1e6, (unsigned long long)g_stall_drop, (unsigned long long)g_stall_n); g_stall_t0 = 0; }
    pthread_mutex_unlock(&g_pq_mu);
    double vp = g_link_vperiod, tick_time = update.targetPresentationTimestamp;
    if (vp > 0) {   // virtual ticks: only the refresh nearest each slot of a steady clock at the wanted rate counts (link_range)
        static double next, vp_was;
        if (vp != vp_was || tick_time > next + vp) { next = tick_time; vp_was = vp; }   // the first callback, a new rate, or after a stall: the clock starts again here
        if (tick_time < next - g_link_vhalf) return;
        tick_time = next;   // the hold follows counted slots even when their physical refreshes are unevenly spaced
        next += vp;
    }
    const int rate = mtl_link_rate();
    const double tick_period = vp > 0 ? vp : 1.0 / rate;
    static double cb_prev, gap_max; static int nticks;   // for link history: callbacks counted as ticks since the previous present, and the longest interval between them
    if (cb_prev > 0 && cb - cb_prev > gap_max) gap_max = cb - cb_prev;
    cb_prev = cb; nticks++;
    pthread_mutex_lock(&g_link_mu);
    __atomic_store_n(&g_link_tick_ns, now_ns(), __ATOMIC_RELAXED);
    const uint64_t tick = ++g_link_tick_n;
    const int woke = g_link_waiter && tick >= g_link_waiter;   // this tick wakes an acquire, whose present follows shortly
    const int drain_end = woke && g_link_waiter_end;   // and it ends a drain into the latch (mtl_link_wait_ticks): the queue has run empty for it
    pthread_cond_broadcast(&g_link_cv);
    pthread_mutex_unlock(&g_link_mu);
    pthread_mutex_lock(&g_pq_mu);
    // Checked again: the queue lock was released to publish the tick, and rescue or a mode change can take the queue away from this callback in that interval.
    if (!g_dm_present_link || g_rescue_mode) { pthread_mutex_unlock(&g_pq_mu); return; }
    // Keep a multi-tick frame on screen until its hold ends, even if the next frame is already queued.
    const int hold = g_link_hold_until > 0 && tick_time < g_link_hold_until - 0.002;
    // The late latch (nvn_link_latch.h): wait briefly for the woken acquire's present (g_pq_mu is released while waiting, so present_enqueue can queue it; a present that
    // finds the queue full waits for this callback, and the wait is bounded). Not with present snapshots, which run the game a frame ahead of the screen on purpose.
    // Only into an empty queue: a frame that stands (one that missed the previous tick) goes out first, in order as without the latch, until a drain (link_drain_note) lets
    // the queue run empty again. Taking the latched present over it dropped that frame: about 3 a second on a built-in ProMotion screen, where presents came late more often
    // Only on the 60 Hz link, where it was measured. Not on the 120 Hz link: there a 60 fps acquire waits two ticks (the drain does not run there
    // either). Not on the 30 Hz power-saving link (a 30 fps target in variable-refresh fullscreen), which has not been measured with it
    const int latch = link_latch_on() && rate == 60 && !mtl_snap_on();
    const double room = latch ? update.targetTimestamp - cb : 0;   // read only with the latch: the update's targetTimestamp is all it needs it for
    static uint64_t woken_n; if (woke) woken_n++;   // woken callbacks: the probe counts these, whatever ticks the acquire waits
    if (latch) {   // the room the latch has: the time from the callback to its targetTimestamp, logged every 300 ticks
        static double rmin = 1e9, rsum; static int rn; double r = room;
        if (r < rmin) rmin = r;
        rsum += r;
        if (++rn == 300) { MLOG("display link: from the callback to its targetTimestamp over 300 ticks: min %.2f ms, mean %.2f ms (late latch: %llu presents taken, %llu timeouts, entered by %llu drains, the next needs %.0f s of early frames)", rmin * 1e3, rsum / rn * 1e3, (unsigned long long)g_link_latched, (unsigned long long)g_link_latch_out, (unsigned long long)g_link_drain.entries, LINK_DRAIN_SECS * (double)(1u << g_link_drain.back)); rmin = 1e9; rsum = 0; rn = 0; }
    }
    // The drain's clock with the latch (nvn_link_latch_spare): the last two callbacks that woke an acquire and their room to targetTimestamp, so each frame taken is timed from the wake before it
    static double wk_cb[2] = {-1, -1}, wk_room[2];
    if (woke && latch) { wk_cb[1] = wk_cb[0]; wk_room[1] = wk_room[0]; wk_cb[0] = cb; wk_room[0] = room; }
    if (woke && !hold && g_pq_read == g_pq_in && latch) {
        double w = nvn_link_latch_wait(cb, cb + room, tick_period, g_link_latch_misses, woken_n, drain_end);
        if (w > 0) {
            const uint64_t in0 = g_pq_in; struct timespec ts = link_deadline((uint64_t)(w * 1e9));
            while (g_pq_in == in0) if (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) == ETIMEDOUT) break;
            if (g_pq_in > in0) { g_link_latched++; g_link_latch_misses = 0; } else { g_link_latch_out++; g_link_latch_misses++; }
        }
    }
    if (g_pq_read == g_pq_in) { g_link_empty++; pthread_mutex_unlock(&g_pq_mu); return; }
    if (hold) { g_link_held++; pthread_mutex_unlock(&g_pq_mu); return; }
    uint64_t first = g_pq_read + 1, ticket = g_pq_in;
    // In order, one frame per tick and at most one behind the newest: two waiting, the older now and the newer on the next tick; three or more, the older ones are dropped. Taking the newest
    // dropped the one before whenever two frames landed in the same refresh, as frames of a 60 fps game that take 16-17.5 ms do, and the next frame then stayed up 33 ms
    // (Dark Souls Remastered, from PR #31: presents within 14.7-18.7 ms 98.2 -> 99.98 %, about 2 ms more from enqueue to display). DM_PRESENT_FIFO=0 takes the newest, as before
    static int fifo = -1; if (fifo < 0) { const char *e = getenv("DM_PRESENT_FIFO"); fifo = !(e && !strcmp(e, "0")); MLOG("display link: %s", fifo ? "frames in order, at most one behind" : "the newest frame each tick (DM_PRESENT_FIFO=0)"); }
    if (fifo && ticket > first) ticket = ticket - first >= 2 ? g_pq_in - 1 : first;
    pq_item it = g_pq[(ticket - 1) % PQ]; g_link_it_tin = it.t_in;
    g_pq_read = ticket;
    if (ticket > first) { g_link_dropped += ticket - first; for (uint64_t k = first; k < ticket; k++) { nvn_present_history_put(&g_present_history, k, -1); mtl_tex_unpin(g_pq[(k-1)%PQ].t.serial); } }   // Old frames merged and dropped are recorded as "not presented", so the present statistics honestly count them as dropped frames
    if (ticket > 1 && g_pq_done < ticket - 1) g_pq_done = ticket - 1;
    nvn_link_history_put(&g_present_history, (nvn_link_time){ticket, update.targetPresentationTimestamp, (update.targetPresentationTimestamp - cb) * 1e3, gap_max * 1e3, nticks}); nticks = 0; gap_max = 0;
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    // Hold the whole ticks of the frame's period. The acquire distributes fractional ticks (e.g. 75 fps on a 120 Hz link), and present_period truncates to whole nanoseconds.
    const double hold_ticks = floor((it.period + 1e-9) / tick_period);
    // The drain (nvn_link_drain.h), with the latch's rule for frames of one vsync: a frame the callback holds for two ticks (present interval 2) is taken on a tick that woke
    // no acquire, so the latch cannot run there. A drain into the latch asks the next acquire to wait the ticks the queued frames have to spare: the callback shows them on the
    // ticks between, so the screen neither repeats nor drops a frame (the game loses those ticks of wall time once), the queue runs empty, and the callback that ends the drain
    // latches its own present, as do the woken callbacks after it. A frame that comes late after that stands again: the callback goes back to taking frames in order, and a
    // fallback soon after the drain backs it off (nvn_link_drain_next)
    {
        const int k = !latch || hold_ticks >= 2 ? -1 : it.t_in >= wk_cb[0] ? 0 : it.t_in >= wk_cb[1] ? 1 : -1;
        link_drain_note(cb, cb - it.t_in, ticket > first, k < 0 || wk_cb[k] < 0 ? -1 : it.t_in - wk_cb[k], k < 0 ? 0 : wk_room[k]);
    }
    int submitted;
    snap_commit_wait(it.snap_v); sub_commit_wait(it.sub_v);
    @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, update.drawable, update.targetPresentationTimestamp); mtl_tex_unpin(it.t.serial); }
    g_link_hold_until = submitted && hold_ticks >= 2 ? tick_time + hold_ticks * tick_period : 0;
    if (!submitted) {
        pthread_mutex_lock(&g_pq_mu);
        if (ticket > g_pq_done) g_pq_done = ticket;
        pthread_cond_broadcast(&g_pq_cv);
        pthread_mutex_unlock(&g_pq_mu);
    }
}
@end

static CAMetalDisplayLink *g_link; static CFRunLoopRef g_link_rl; static int g_link_started;
static int g_link_owns_layer, g_classic_busy;   // Under g_pq_mu: nextDrawable throws while the layer has the display link object attached, so the present thread that takes its own drawable and the display link object are mutually exclusive
static int link_on(void) { return g_dm_present_link && !atomic_load(&g_rescue_mode) && g_link != nil; }
// Screens whose refresh rate is not a multiple of the link's (144 or 165 Hz against 60 or 30): asked for 60 Hz, the system fires the link on every third refresh of a 165 Hz screen,
// 55 Hz, and a game paced by the link runs at 55 fps (27.5 at 30). There the link runs at the screen's rate and only the refreshes nearest a steady clock of the wanted rate count as
// ticks (g_link_vperiod, NvMetalDisplayLinkDelegate): 60 a second on average, each frame shown on the refreshes nearest its 1/60 s slot. DM_LINK_VTICK=0 keeps the system's choice.
// In fullscreen the Mac front end switches such a screen to a rate that is a multiple where it has one (front_screen_match), and the range is set again for it (link_sync)
__attribute__((weak)) int front_screen_settling(void) { return 0; }   // front_mac.m: startup fullscreen not entered yet, fullscreen match pending, or mode switched under 1 s ago
__attribute__((weak)) int front_screen_max_fps(void) { return 0; }   // front_mac.m; some graphics unit tests supply their own
__attribute__((weak)) void front_screen_match(int fps) { (void)fps; }   // front_mac.m; graphics unit tests have none
static CAFrameRateRange link_range(int mode) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_LINK_VTICK"); on = !(e && !strcmp(e, "0")); }
    int rate = mode == 3 ? 30 : mode == 2 ? 120 : 60, hz = on ? front_screen_max_fps() : 0;
    if (hz > rate && hz % rate) {   // at 120 fps too: a game stepping by its target must not get 144 ticks a second
        if (g_link_vperiod != 1.0 / rate) MLOG("display link at the screen's %d Hz, counting a tick every 1/%d s (the screen's rate is not a multiple of it)", hz, rate);
        g_link_vhalf = 0.5 / hz; g_link_vperiod = 1.0 / rate;
        return CAFrameRateRangeMake(rate, hz, hz);
    }
    g_link_vperiod = 0;
    return mode == 3 ? CAFrameRateRangeMake(30, 30, 30) : mode == 2 ? CAFrameRateRangeMake(60, 120, 120) : CAFrameRateRangeMake(60, 60, 60);
}
// The display link's frame latency: 2 in variable-refresh fullscreen, 1 otherwise (DM_PRESENT_LATENCY overrides both). In a window the system calls back about 50 ms before the target display
// time; in fullscreen it shows the layer directly and, at latency 1, calls back only one 60 Hz period (16 ms) ahead. There, on ProMotion screens (M4 Pro, M5 Max), it shows some
// presents one 120 Hz refresh late and the link skips its next tick, though the callbacks run on time and no GPU work is in the way; the frame step read the late frames as load
// and stepped to 20 fps on frames costing 0.1-0.6 vsyncs, and the game presented 15 frames a second (issues #347, #348). Latency 2 gives 33 ms and removed most of the misses on
// both machines, for one more frame of input lag in variable-refresh fullscreen only. Fixed-rate screens do not need this workaround (#822). Why the system misses at 16 ms,
// and why a fixed-rate screen still has about 20 ms more presentation delay with the link at either latency, remain open (docs/reference/frame-rate.md)
__attribute__((weak)) int front_fullscreen(void) { return 0; }   // front_mac.m; graphics unit tests have none
__attribute__((weak)) int front_screen_variable_refresh(void) { return 0; }   // front_mac.m; graphics unit tests have none
// CAMetalDisplayLink accepts only 1 or 2. Compare its readback with the supported request, or an oversized override rewrites and logs every frame.
static int link_latency(void) { return g_present_latency > 0 ? MIN(g_present_latency, 2) : front_fullscreen() && front_screen_variable_refresh() ? 2 : 1; }
static const char *link_latency_why(void) { return g_present_latency > 0 ? "DM_PRESENT_LATENCY" : !front_fullscreen() ? "window" : front_screen_variable_refresh() ? "fullscreen, variable refresh" : "fullscreen, fixed refresh"; }
// The 30 Hz link at a 30 fps target, except in fullscreen on a fixed-rate screen. There the link skipped ticks: Ocarina of Time at a 30 fps cap on a MacBook Air M4 (60 Hz) got
// 16.5-25 callbacks a second over stretches of 300 frames, also on frames with under 1 ms of GPU, and each skipped tick kept a frame on screen 66.7 ms (a 180 s walk: 388 and 996
// frames over 50 ms; 3-4 in a window, where the same link held 30.0). Why is open (docs/reference/frame-rate.md). The 60 Hz link paces the 30 through the present interval, as it
// does interval-2 frames (two ticks per acquire, mtl_link_wait_tick; each frame held for its period, g_link_hold_until): 7 frames over 50 ms on the same walk
// In a window the 30 Hz link is kept only while standing frames are not drained (DM_PRESENT_DRAIN=0) or present snapshots run the game ahead. With the drain a
// frame presented right after a tick waits one tick of the link whatever the frame rate, and the drain moves the game onto the 60 Hz ticks between the callback's takes:
// 15.5 ms in the queue at 30 fps on the 60 Hz link against 32.2 ms on the 30 Hz one (Ocarina of Time in a window: 65 against 82 ms from the game's present to the screen),
// for 60 callbacks a second instead of 30. Without the drain the 60 Hz link gains nothing reliably: the takes stay on the acquire's ticks or not as the last late frame left them
static int link_30hz(void) { return front_fullscreen() ? front_screen_variable_refresh() : !link_drain_on() || mtl_snap_on(); }
// The 120 Hz link (mode 2, a frame every other tick) in place of the 60 Hz one in fullscreen on a variable-refresh screen whose rate is a multiple of 120 (a ProMotion
// screen), unless DM_PRESENT_LINK chose the mode. There, with the 60 Hz link at frame latency 2, the system showed every present one 120 Hz refresh after the link's
// target time (@pres_at_ms − @link_target_ms = 8.33 ms) and the link then skipped every other tick (@link_gap_ms 16.7, 33.3, 16.7, …): 40 callbacks a second, so a game
// paced by the link ran at 40 fps with its GPU at 10 ms of the 16.7 (Ocarina of Time on an M5 Pro, macOS 27.2; at latency 1 the frame step went to 4 vsyncs, 15 fps).
// The Mac front end first switches such a screen to a fixed-rate mode (front_screen_match: 60 Hz, 60.0 fps at latency 1 there), so this is the fallback for a screen with
// no such mode or with DM_SCREEN_MATCH=0: the 120 Hz link held 59.2 fps, about 2% of frames up 25 ms. A window keeps the 60 Hz link, where every present lands on its
// target (docs/reference/frame-rate.md)
static int link_vrr_120(void) { int hz = front_screen_max_fps(); return !g_present_link_env && front_fullscreen() && front_screen_variable_refresh() && hz >= 120 && hz % 120 == 0; }
static _Atomic int g_test_link_hold;   // DM_TEST_LINK_STALL: a link made during the stall (tried again after a rescue) starts paused, silent from its start as one that never calls back
static int g_link_mode = 1;   // Current mode of the display link object: 1 / 2 same as g_dm_present_link, 3 = power-saving mode 30 Hz
static uint64_t link_probe_ns(void) { return g_link_mode == 3 ? 2 * LINK_PROBE_NS : LINK_PROBE_NS; }   // 4 refreshes at 60 Hz (~67 ms) or 30 Hz (~134 ms); 67 ms floor at 120 Hz
static void *link_main(void *arg) {   // Display link thread: started when switching to display-link mode, exits when switching back to 0
    (void)arg;
    pthread_setname_np("nvn-present-link");
    pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
    CAMetalLayer *layer = (__bridge CAMetalLayer *)front_metal_layer();
    pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 1; while (g_classic_busy) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu);   // Wait for the present thread to finish handing over the frame it holds
    NvMetalDisplayLinkDelegate *delegate = [NvMetalDisplayLinkDelegate new];
    CAMetalDisplayLink *link = [[CAMetalDisplayLink alloc] initWithMetalLayer:layer];
    link.preferredFrameRateRange = link_range(g_link_mode);
    link.preferredFrameLatency = link_latency(); MLOG("display link: frame latency %d (%s)", link_latency(), link_latency_why());
    link.delegate = delegate;
    if (atomic_load(&g_test_link_hold)) link.paused = YES;
    [link addToRunLoop:[NSRunLoop currentRunLoop] forMode:NSDefaultRunLoopMode];
    pthread_mutex_lock(&g_pq_mu);
    g_link_rl = CFRunLoopGetCurrent(); g_link = link;
    if (g_link_probe_pending) {
        g_link_probe_pending = 0;
        if (!g_rescue_mode && g_dm_present_link) {
            g_stall_t0 = 0; g_link_probe_at = now_ns();
            __atomic_store_n(&g_link_tick_ns, g_link_probe_at, __ATOMIC_RELAXED);
        }
        pthread_cond_broadcast(&g_pq_cv);
    }
    pthread_mutex_unlock(&g_pq_mu);
    CFRunLoopRun();   // When switching back to mode 0 the display link object is invalidated and this thread exits (nextDrawable cannot get a drawable while the layer still has the display link object attached, so pausing alone is not enough)
    g_link_started = 0;
    return NULL;
}
static void link_sync(void) {   // Called every frame by the game's present thread: aligns the display link object's state to g_dm_present_link and the power-saving mode (can change at run time)
    static int applied;
    pthread_mutex_lock(&g_pq_mu);
    int want = g_dm_present_link;
    if (want < 0 || want > 2) { g_dm_present_link = applied == 3 ? 1 : applied; pthread_mutex_unlock(&g_pq_mu); return; }
    if (g_rescue_mode && (want != g_rescue_mode || now_ns() >= g_rescue_back_at)) {
        g_rescue_mode = 0; g_stall_t0 = 0; __atomic_store_n(&g_link_tick_ns, now_ns(), __ATOMIC_RELAXED);
        g_link_probe_pending = want != 0; g_link_probe_at = 0;   // probation starts once the link is live on its run loop
        MLOG("display link: rescue ended, trying pacing mode %d (frame %llu)", want, (unsigned long long)atomic_load(&g_cur_frame));
    }
    if (g_rescue_mode) want = 0;
    if (!want) { g_link_probe_pending = 0; g_link_probe_at = 0; }
    pthread_mutex_unlock(&g_pq_mu);
    if (want && mtl_fps_target() == 30 && link_30hz()) want = 3;   // Power-saving mode: display link at 30 Hz
    if (want) front_screen_match(want == 3 ? 30 : want == 2 ? 120 : 60);   // the game's rate: a variable-refresh screen goes to a fixed-rate mode at a multiple where it has one
    static int vrr_was; int vrr = want == 1 && link_vrr_120();   // variable-refresh fullscreen that kept its mode: the 120 Hz link paces the 60 (link_vrr_120)
    if (vrr != vrr_was) { vrr_was = vrr; MLOG("display link: %s (frame %llu)", vrr ? "120 Hz in fullscreen on a variable-refresh screen, a frame every other tick (DM_PRESENT_LINK=1 keeps 60 Hz)" : "60 Hz again (window, fixed-rate screen or another frame rate)", (unsigned long long)atomic_load(&g_cur_frame)); }
    if (vrr) want = 2;
    // The screen's rate changed (switched by front_screen_match, or another screen): the range again, virtual ticks or none. Taken only once the link object exists, so a change
    // while its thread starts (after link_main read the rate) is not lost
    static int hz_was; int hz = front_screen_max_fps(); CAMetalDisplayLink *lk = g_link;
    if (lk && want) {   // existing link kept alive across a mode change that ended rescue: arm probation now
        pthread_mutex_lock(&g_pq_mu);
        if (g_link_probe_pending) {
            g_link_probe_pending = 0; g_link_probe_at = now_ns();
            __atomic_store_n(&g_link_tick_ns, g_link_probe_at, __ATOMIC_RELAXED);
            pthread_cond_broadcast(&g_pq_cv);
        }
        pthread_mutex_unlock(&g_pq_mu);
    }
    if (lk && hz != hz_was) { hz_was = hz; if (want && want == applied) { lk.preferredFrameRateRange = link_range(want); MLOG("display link: the screen runs at %d Hz now, range set again (frame %llu)", hz, (unsigned long long)atomic_load(&g_cur_frame)); } }
    int lat = link_latency();   // fullscreen, screen or refresh mode changed: a link created since took the current policy
    if (lk && lk.preferredFrameLatency != lat) { lk.preferredFrameLatency = lat; MLOG("display link: frame latency %d (%s; frame %llu)", lat, link_latency_why(), (unsigned long long)atomic_load(&g_cur_frame)); }
    // Rescue can ask for mode 0 after link_main has attached the layer but before an enqueue has applied the link's first mode, so applied is still 0 while a link owns the layer. Mode 0 with a link present still has to invalidate it, or the layer is never handed back.
    if (want == applied && (want || !lk)) return;
    g_link_mode = want;
    if (!g_link_started) {
        if (!want) { applied = 0; return; }
        if (!front_metal_layer()) return;
        // Not while the screen settles (front_screen_settling): startup fullscreen and the screen match's mode switch come first, so the link starts on the final mode and not
        // mid-switch. The present thread shows frames meanwhile
        static int waited;
        if (front_screen_settling()) { if (!waited++) MLOG("display link: waits for fullscreen and the screen's mode to settle (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame)); return; }
        if (waited) { MLOG("display link: the screen settled after %d frames", waited); waited = 0; }
        g_link_started = 1; pthread_t th; if (pthread_create(&th, NULL, link_main, NULL)) MDIE("cannot start the display link thread"); pthread_detach(th);
        return;   // The display link object is created on a new thread; align again next frame
    }
    CAMetalDisplayLink *link = g_link; if (!link) return;
    if (want) link.preferredFrameRateRange = link_range(want);
    else { CFRunLoopRef rl = g_link_rl; g_link = nil; CFRunLoopPerformBlock(rl, kCFRunLoopDefaultMode, ^{ [link invalidate]; pthread_mutex_lock(&g_pq_mu); g_link_owns_layer = 0; pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu); CFRunLoopStop(rl); }); CFRunLoopWakeUp(rl); }
    if (want == applied) MLOG("display link: returning the layer to the present thread during startup (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame));
    else MLOG("present pacing mode %d → %d (frame %llu)", applied, want, (unsigned long long)atomic_load(&g_cur_frame));
    applied = want;
    pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
}
// Before going to the background (mtl_drain): when the display link drives presents, frames queued but not yet presented can only be taken by the display-link callback, and in the background the system no longer calls back --
// waiting as-is for "the present queue to empty" would wait forever, and the caller going to the background would never return.
// These frames will not be seen anyway: discard them directly (accounted as dropped frames) and only wait for the one present already submitted to the GPU to finish.
static void present_drop_for_suspend(void) {
    pthread_mutex_lock(&g_pq_mu);
    if (g_link_owns_layer) {
        uint64_t n = g_pq_in - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= g_pq_in; k++) { nvn_present_history_put(&g_present_history, k, -1); mtl_tex_unpin(g_pq[(k-1)%PQ].t.serial); }
        g_link_dropped += n; g_pq_read = g_pq_in;
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
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp); atomic_store(&g_present_qos, (uint32_t)qc);
    for (;;) {
        pthread_mutex_lock(&g_pq_mu);
        while (g_pq_read == g_pq_in || g_link_owns_layer) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // In display-link mode the display-link callback takes them; an empty queue is judged by "taken"; "done" has to wait for the feedback of the present submit and lags behind taken
        pq_item it = g_pq[g_pq_read % PQ];
        uint64_t ticket = ++g_pq_read; g_classic_busy = 1;
        pthread_mutex_unlock(&g_pq_mu);
        int submitted;
        snap_commit_wait(it.snap_v); sub_commit_wait(it.sub_v);
        @autoreleasepool { submitted = present_now(&it.t, it.period, it.frame, ticket, it.crop, nil, 0); mtl_tex_unpin(it.t.serial); }
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
static void present_start(void) { pthread_t th; if (pthread_create(&th, NULL, present_main, NULL)) MDIE("cannot start the present thread"); pthread_detach(th); }
static _Atomic int g_present_discard;
void mtl_link_pause(int on);
// The short period when the front end goes to the background and waits for the game to save: presents are dropped directly (the display link does not call back in the background, enqueueing would block the game thread; nobody sees the picture anyway).
// When turned on, also clear the frames already queued: the system first stops the display-link callback and only afterwards notifies about going to the background, and in that gap the game has already filled the swapchain and is stuck at "before acquiring the next texture, wait for its previous present to be processed"
// (hle_nvn.c nvnWindowAcquireTexture -> mtl_present_wait). Without clearing, the game cannot advance a single tick during those 2.5 seconds of waiting for the save, and the save is not written (the same pattern three times in player logs: 3 frames queued, 0 submits)
void mtl_present_discard(int on) { atomic_store(&g_present_discard, on != 0); if (on) present_drop_for_suspend(); }
static uint64_t present_enqueue(const tex_t *t, double period, const int crop[4], uint64_t *copy);
uint64_t mtl_present(const tex_t *t, double period, const int crop[4], uint64_t *copy) { return present_enqueue(t, period, crop, copy); }
extern _Atomic uint64_t dm_present_ns;   // hle_thread.c (DM_HITCH_DUMP); the game's presents all come through present_enqueue
// Present snapshots (DM_PRESENT_SNAP). A game with two swapchain textures (Breath of the Wild) gets one back only when the display link has taken its previous present on a
// tick, so the next frame's CPU work starts on the tick and the GPU idles from the end of one frame to the first work of the next. macOS reads that idle time as room and holds the GPU
// at its lowest clock (444 MHz on an M2 Max) even while frames miss the screen, and there 12 ms of GPU work a frame plus the time to the frame's first submission often missed 16.7 ms.
// With snapshots each present is copied on the game's queue, right after the frame's work, into a texture of the host's own (a ring of g_snap_n), the display link shows the copy, and
// the game's texture is free at once: the game runs up to a frame ahead of the screen, and a frame whose GPU work runs long is absorbed instead of shown late. A ring slot is reused
// once its present has been processed and its copy has completed; AcquireTexture waits for that capacity and counts it as pacing, not as the frame's cost.
// A present that reaches a busy slot (several textures acquired ahead of time) keeps the original texture and its ticket instead of waiting while the game may hold a lock.
// With Metal encoding on its own thread (DM_NVN_ENC_THREAD) the copy is a block of the encode thread's queue (rec_own_push): it runs behind everything the game submitted before
// the present, the presenting thread does not wait for the encode thread, and the frame's present waits only until that block is encoded (snap_commit_wait), before its blit.
// The ring's length is the number of frames the game may run ahead of the screen: DM_PRESENT_SNAP=<n> (2 to SNAP_MAX; 1 = 2). Off unless the profile's present_snap is on; the launcher's Frame buffering setting enables it.
#define SNAP_MAX 4
// old: the slot's texture before the presented size or format last changed, resident until the slot's next use (snap_take)
static struct { id<MTLTexture> tex, old; uint64_t serial; _Atomic uint64_t ticket, ev; } g_snap[SNAP_MAX];
static id<MTL4CommandAllocator> g_snap_alloc[SNAP_MAX]; static id<MTL4CommandBuffer> g_snap_cb[SNAP_MAX];
static id<MTLSharedEvent> g_snap_ev; static uint64_t g_snap_v;
static _Atomic uint64_t g_snap_next, g_snap_cmt;   // g_snap_cmt: the last copy the encode thread has committed (its g_snap_v)
static int g_snap_n = 2;
int mtl_snap_on(void) {
    static int v = -1; if (v >= 0) return v;
    const char *e = getenv("DM_PRESENT_SNAP");
    int n = e ? atoi(e) : DM_GAME_PRESENT_SNAP ? 2 : 0;   // the profile's present_snap (a ring of 2)
    v = n > 0; g_snap_n = n < 2 ? 2 : n > SNAP_MAX ? SNAP_MAX : n;
    if (v) MLOG("present snapshots (DM_PRESENT_SNAP=%d): each present is copied into a ring of %d host textures, so the screen never samples the game's texture", n, g_snap_n);
    return v;
}
static void snap_commit_wait(uint64_t v) { if (v) rec_note_wait(&g_snap_cmt, v); }
static void snap_drain(void) {
    // Display tickets may have been dropped during suspend, but their copies still belong to the Metal 4 queue.
    uint64_t v = g_snap_v; if (!v) return;
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
        // On timeout present falls back to its original texture; the busy allocator is never reset.
        if (![g_snap_ev waitUntilSignaledValue:ev timeoutMS:2000]) RED_ONCE(111, "present snapshots: the previous copy into a ring slot did not finish within 2 s");
    }
    if (ws_on()) ws_put(WS_SNAP, w0, blocked);
    return ws_t0() - w0;
}
// Copy the presented texture into the next ring slot; returns the slot's serial (0 = no snapshot: the game's texture is presented as before)
static uint64_t snap_take(const tex_t *t) {
    int k = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
    pthread_mutex_lock(&g_pq_mu); int busy = atomic_load(&g_snap[k].ticket) > g_pq_done; pthread_mutex_unlock(&g_pq_mu);
    uint64_t ev = atomic_load(&g_snap[k].ev);
    if (busy || (ev && g_snap_ev.signaledValue < ev)) return 0;
    // The texture this slot held before it was last replaced: every present that read it had been committed when the replacing copy was (the busy check then), and the queue
    // signals g_snap_ev only after all work committed before the signal, so those presents have run and it can leave the residency set
    if (g_snap[k].old) { [g_snap_res removeAllocation:g_snap[k].old]; [g_snap_res commit]; g_snap[k].old = nil; }
    id<MTLTexture> src = tex_mtl(t); if (!src) return 0;
    // The copy reads the source before present_now runs, so it needs the same commit as present_now: a window texture presented before any submission has used it was made just
    // now and is outside the committed residency set. DRAGON QUEST XI S presents three times before its first submission; on an A17 Pro the copy faulted silently (its command buffer
    // has no error handler) and every later command buffer of the process was ignored (GitHub issue #626). A commit with nothing new costs nothing
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
        // The old texture stays resident (and referenced) until the slot's next use: the busy check above means its last present was committed, not that the GPU has run it,
        // and a texture removed here was sampled outside the residency set by a display command buffer still queued
        g_snap[k].old = dst;
        dst = [g_dev newTextureWithDescriptor:d]; if (!dst) { RED_ONCE(112, "present snapshots: no texture"); return 0; }
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
    if (rec_on() && g_rb_started) {   // a block of the encode thread's queue, behind the frame's work (see above)
        if (on_sub_thread()) rec_publish();   // what the submitting thread recorded outside a submission goes first
        rec_blk *b = rec_own_begin();
        rec_t *r = rec_own_put(b, RO_COMMIT_REAL, 2); r->a[0] = 0; r->a[1] = rec_obj(cb);
        r = rec_own_put(b, RO_SIGNAL, 1); r->a[0] = rec_obj(g_snap_ev); r->a[1] = v;
        r = rec_own_put(b, RO_NOTE, 0); r->a[0] = (uint64_t)(uintptr_t)&g_snap_cmt; r->a[1] = v;
        rec_own_push(b);
    } else {
        pthread_mutex_lock(&g_res_commit_mu); [g_q4 commit:&cb count:1]; [g_q4 signalEvent:g_snap_ev value:v]; pthread_mutex_unlock(&g_res_commit_mu);
        atomic_store(&g_snap_cmt, v);
    }
    g_snap[k].ev = v;
    return g_snap[k].serial;
}
// copy (may be NULL): the g_snap_ev value of the snapshot copy this present took, 0 when it took none (mtl_present_wait_texture)
static uint64_t present_enqueue(const tex_t *t, double period, const int crop[4], uint64_t *copy) {
    atomic_store(&dm_present_ns, now_ns()); if (copy) *copy = 0;
    if (atomic_load(&g_present_discard)) return 0;   // Ticket number 0 = no pending present; the swapchain texture acquire does not wait
    // A texture whose content comes from its storage is uploaded when it is first resolved (and again when its pages change), in an upload batch only the submit thread may open.
    // Bastion fills both window textures with WriteTexels at boot (main+0x9374) and presents one before drawing into it: resolved on the present thread, the upload aborted the host.
    // Resolve it here, where the game queues the present, and commit the batch, so the display command buffer comes after the upload on the queue and the present thread finds it uploaded.
    // A texture first used by the present is resolved here too: its first use decides whether it is taken from its storage (tex_cpu_content), so tex_from_storage cannot tell before,
    // and resolved on the present thread a window texture holding the CPU's data in a pool the CPU can write was uploaded there and aborted the host (t_nvn, whose window textures
    // nothing draws into, since c06d1e79 stopped a sliver of another span from making them the GPU's)
    if (on_sub_thread()) { tex_mtl(t); if (tex_from_storage(t)) up_flush(); }
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, present_start);
    link_sync();
    {   // Test switch DM_TEST_LINK_STALL=start seconds,duration seconds: counting from the first present, pause the display link at that point (imitating the display link not calling back while in the foreground) and release it when the duration is up. For verifying the fallback; not set in the product
        static double t0, at = -1, dur; static int st; double now = CACurrentMediaTime();
        if (at < 0) { at = 0; const char *e = getenv("DM_TEST_LINK_STALL"); if (e && sscanf(e, "%lf,%lf", &at, &dur) == 2 && at > 0 && dur > 0) { t0 = now; st = 1; } else at = 0; }
        if (st == 1 && link_on() && now - t0 >= at) { st = 2; atomic_store(&g_test_link_hold, 1); mtl_link_pause(1); MLOG("DM_TEST_LINK_STALL: pausing the display link for %.1f s (frame %llu)", dur, (unsigned long long)atomic_load(&g_cur_frame)); }
        else if (st == 2 && now - t0 >= at + dur) { st = 3; atomic_store(&g_test_link_hold, 0); mtl_link_pause(0); MLOG("DM_TEST_LINK_STALL: resuming the display link (frame %llu)", (unsigned long long)atomic_load(&g_cur_frame)); }
    }
    if (ws_on()) { static uint64_t f0; uint64_t f = atomic_load(&g_cur_frame); if (!f0) f0 = f; else if (f >= f0 + 300) { ws_report(f, f - f0); f0 = f; } }
    tex_t snap; uint64_t ss = 0; int slot = 0;
    if (mtl_snap_on()) {
        slot = (int)(atomic_load(&g_snap_next) % (uint64_t)g_snap_n);
        if ((ss = snap_take(t))) { memset(&snap, 0, sizeof snap); snap.magic = MAGIC('T', 'E', 'X', 0); snap.serial = ss; t = &snap; }
    }
    mtl_tex_pin(t->serial);
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_in - g_pq_done >= PQ) pthread_cond_wait(&g_pq_cv, &g_pq_mu);   // Wait only when the queue is full; normally AcquireTexture blocks first
    pq_item *it = &g_pq[g_pq_in % PQ]; *it = (pq_item){*t, period, atomic_load(&g_cur_frame), {0}, CACurrentMediaTime(), ss ? g_snap[slot].ev : 0, !ss && on_sub_thread() ? g_fence : 0};
    if (crop) memcpy(it->crop, crop, sizeof it->crop);   // The current frame's state is enqueued with the ticket number, rather than reading the mutable window late on the present thread
    uint64_t tk = ++g_pq_in;
    g_pres_tin[tk % PRING] = it->t_in;
    g_pres_frm[tk % PRING] = it->frame;
    if (ss) { g_snap[slot].ticket = tk; atomic_fetch_add(&g_snap_next, 1); if (copy) *copy = g_snap[slot].ev; }
    pthread_cond_broadcast(&g_pq_cv);
    pthread_mutex_unlock(&g_pq_mu);
    // A snapshot present returns its ticket and, in *copy, its copy: the game's next acquire of this texture waits for the copy (mtl_present_wait_texture), not for the screen
    return tk;
}
// For the layer's frame history (hle_vi.c): the ticket of the last present queued, and the last one the present queue has handed on (to the GPU and the display, or dropped)
// Do not wait during discard: the frame enqueued only after the flag was set (the present thread had just passed the flag check) has nobody to take it, and waiting for it would stall again; it is left for the following mtl_drain to clear
// Do not wait indefinitely in display-link mode: see the note at STALL_NS. Only frames "not yet taken by the display link" are dropped; the one already on its way to the screen is waited for until handed to the GPU (same dropping as when entering the background)
// The game's next acquire of the texture a present showed (nvn_queue_present_wait), and nvnQueueFinish after a present (nvn_queue_finish); tk and ev are what mtl_present
// returned for that present. Without a snapshot (ev 0) the screen samples that texture: wait for the present (below).
// With one, the texture is the game's again once its copy has run on the GPU, and with it the frame that drew into it: the game never has more frames in flight than
// its swapchain holds, and still need not wait for the screen. Before, a snapshot present handed the texture back at once (ticket 0) and only the ring slot paced the
// acquire. The slot is counted when the queue worker replays the present, so a game that acquires its next texture before presenting the current frame, or before the
// worker has replayed that present, then ran a frame further ahead of the GPU per ring slot, and one that sizes its
// CPU-written per-frame buffers on the swapchain's depth rewrote data a queued frame's draws still read: Xenoblade Chronicles 3's scene came out one flat colour in a
// third to a half of the frames (2 slots 32/80, 4 slots 49/80, snapshots off 0/80; LNPANDY/NX2Apple#1117)
// Without the display link (DM_PRESENT_LINK=0) the acquire paces the game on the host's own clock (hle_nvn.c: the texture's slot time, nvn_pace), and the present thread holds
// each frame on screen for its period (presentAfterMinimumDuration). Nothing ties the two clocks to what the screen really shows: a texture is the game's again once its
// present is committed, not shown. At a cap the game holds, both run at the same rate, so each present that reaches the screen late adds a period to the queue for good
// (frames waiting in the present queue and in the layer's drawables), until the swapchain and the layer's drawables are full: Ocarina of Time at a 30 fps cap showed its
// frames 92-155 ms after queuing them, against 32-45 ms when the game ran slower than the cap. On the Switch the acquire blocks until the screen has let go of the texture.
// The acquire now also waits until at most DM_PRESENT_DEPTH (default 1) presents queued are not yet on screen or dropped, as the presented handlers report. 0 is the
// Switch's double buffering; a larger depth trades latency for slack. The wait gives up after two periods and then stays off until the handlers report again (a covered
// window, a screen asleep or a headless run report rarely or never). DM_PRESENT_SCREEN=0 turns it off (ab with DM_AB). The display link has its own pacing and is left alone
// (it drains frames standing in its queue instead, nvn_link_drain.h), and so are present snapshots, which run the game a frame ahead of the screen on purpose and are paced by
// their ring (mtl_snap_acquire_wait). The time waited is the frame's pacing for the frame step (hle_nvn.c g_screen_at): a frame it lengthens is one the screen showed late
uint64_t mtl_present_screen_wait(double period) {
    static int mode = -1, silent; static uint64_t silent_seen;   // silent: the wait gave up while the newest present seen was silent_seen (0 when the screen has reported none yet)
    if (mode < 0) {
        const char *e = getenv("DM_PRESENT_DEPTH"); int d = e && *e ? atoi(e) : 1; g_sdw_depth = d < 0 ? 0 : d > PQ ? PQ : d;
        mode = sw_mode("DM_PRESENT_SCREEN", 1);
        MLOG("present: without the display link the acquire waits until at most %d present(s) queued are not yet on screen (DM_PRESENT_DEPTH; DM_PRESENT_SCREEN %s)", g_sdw_depth, mode == 1 ? "on" : mode ? "ab" : "off");
    }
    if (!sw_on(mode) || link_on() || mtl_snap_on() || atomic_load(&g_present_discard)) return 0;
    if (!(period > 0)) period = 1.0 / 60;
    uint64_t t0 = now_ns(); int waited = 0, to = 0;
    pthread_mutex_lock(&g_pq_mu);
    // A flag, not the ticket: a screen that has reported no present at all (newest seen 0: a window covered from its first frame, a headless run) is silent too, and with 0 read
    // as "not silent" every acquire waited the whole two periods, a third of the frame rate for the run
    if (silent && atomic_load(&g_pres_seen) == silent_seen) { pthread_mutex_unlock(&g_pq_mu); return 0; }
    silent = 0;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); uint64_t tmo = (uint64_t)(2 * period * 1e9);
    ts.tv_sec += tmo / 1000000000ull; ts.tv_nsec += tmo % 1000000000ull; if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    // Counted in the game's presents, not the replay's: g_pq_in grows only when the queue worker replays a present, a variable time after the game recorded it, and against
    // that stale count the acquire was released early (a short frame) and then held (a long one): the game's present calls ran 25, 25, 48 ms apart at a 30 fps cap while the
    // replay and the screen were even. A present recorded in game frame f waits until the present of frame f - depth is shown. Without recorded presents: the replay's count
    for (;;) {
        uint64_t rf = atomic_load(&g_rec_frame), want, have; int byframe = rf > 0;
        if (byframe) { want = rf > (uint64_t)g_sdw_depth ? rf - (uint64_t)g_sdw_depth : 0; have = atomic_load(&g_seen_frame); }
        else { want = g_pq_in > (uint64_t)g_sdw_depth ? g_pq_in - (uint64_t)g_sdw_depth : 0; have = atomic_load(&g_pres_seen); }
        if (have >= want || atomic_load(&g_present_discard)) break;
        waited = 1;
        if (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) == ETIMEDOUT) { if ((byframe ? atomic_load(&g_seen_frame) : atomic_load(&g_pres_seen)) < want) { to = 1; silent = 1; silent_seen = atomic_load(&g_pres_seen); } break; }
    }
    pthread_mutex_unlock(&g_pq_mu);
    uint64_t dt = now_ns() - t0;
    if (waited) { atomic_fetch_add(&g_sdw_n, 1); atomic_fetch_add(&g_sdw_ns, dt); }
    if (to) { atomic_fetch_add(&g_sdw_to, 1); static int nred; if (nred++ < 3) MLOG("present: the screen reported no frame for %.0f ms while %llu presents wait (newest seen %llu); the acquire stops waiting for the screen until it reports again", 2 * period * 1e3, (unsigned long long)g_pq_in, (unsigned long long)atomic_load(&g_pres_seen)); }
    return waited ? dt : 0;
}
void mtl_present_wait_texture(uint64_t tk, uint64_t ev) {
    if (!ev) { mtl_present_wait(tk); return; }
    if (atomic_load(&g_present_discard) || g_snap_ev.signaledValue >= ev) return;
    if (![g_snap_ev waitUntilSignaledValue:ev timeoutMS:2000]) RED_ONCE(164, "present snapshots: a texture's copy did not finish within 2 s; the game draws into it again");
}
void mtl_present_wait(uint64_t tk) {
    pthread_mutex_lock(&g_pq_mu);
    while (g_pq_done < tk && !atomic_load(&g_present_discard)) {
        if (!g_link_owns_layer) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }
        if (g_pq_read >= tk) { pthread_cond_wait(&g_pq_cv, &g_pq_mu); continue; }   // This one has already been taken by the display link and is being presented: wait for it to be handed to the GPU (the GPU completion callback will wake us)
        uint64_t pns = link_probe_ns(), wait_ns = STALL_NS;
        if (g_link_probe_at && dm_focus_now() == 1) {
            uint64_t pnow = now_ns(), psince = dm_focus_in_since(), pfrom = g_link_probe_at > psince ? g_link_probe_at : psince;
            uint64_t pel = pnow >= pfrom ? pnow - pfrom : 0, rem = pel >= pns ? 0 : pns - pel;
            if (rem < wait_ns) wait_ns = rem;
        }
        struct timespec ts = link_deadline(wait_ns);
        // Already determined to be in a stall (when the display link comes back the callback first clears g_stall_t0, then records the display-link tick time): no longer wait 100 ms in vain every frame, drop directly, and the game proceeds by its own display-link wait timeout (about two ticks)
        if (!g_stall_t0 && wait_ns && (pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts) != ETIMEDOUT || g_pq_done >= tk)) continue;
        uint64_t now = now_ns(), last = __atomic_load_n(&g_link_tick_ns, __ATOMIC_RELAXED);
        if (g_pq_read >= tk) continue;
        int active = dm_focus_now() == 1; uint64_t since = dm_focus_in_since();
        uint64_t pfrom = g_link_probe_at > since ? g_link_probe_at : since;
        uint64_t pelapsed = now >= pfrom ? now - pfrom : 0;
        int probe_failed = active && g_link_probe_at && pelapsed >= pns;
        if (!probe_failed && (now < last || now - last < STALL_NS)) { ts = link_deadline(wait_ns ? wait_ns : STALL_NS); pthread_cond_timedwait(&g_pq_cv, &g_pq_mu, &ts); continue; }   // The display link is still running, it is just not this one's turn yet: keep waiting (bounded, no busy spinning)
        uint64_t n = tk - g_pq_read;
        for (uint64_t k = g_pq_read + 1; k <= tk; k++) { nvn_present_history_put(&g_present_history, k, -1); mtl_tex_unpin(g_pq[(k-1)%PQ].t.serial); }
        g_link_dropped += n; g_pq_read = tk;
        while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu);
        if (g_pq_done < tk) g_pq_done = tk;
        pthread_cond_broadcast(&g_pq_cv);
        if (!g_stall_t0) { g_stall_t0 = last ? last : now; g_stall_drop = 0; g_stall_red = 0; g_stall_logged = 0; g_stall_n++; }
        g_stall_drop += n;
        active = dm_focus_now() == 1; since = dm_focus_in_since();
        uint64_t from = g_stall_t0 > since ? g_stall_t0 : since;
        uint64_t elapsed = now >= from ? now - from : 0;   // Focus can return after the earlier now sample, including while waiting for an in-flight present
        pfrom = g_link_probe_at > since ? g_link_probe_at : since;
        pelapsed = now >= pfrom ? now - pfrom : 0;
        probe_failed = active && g_link_probe_at && pelapsed >= pns;
        if (active && !g_stall_logged && elapsed >= STALL_NS) { g_stall_logged = 1;
            MLOG("display link silent for %.0f ms (active): dropping %llu queued frames not yet displayed, the game continues (stall #%llu; back to normal when the link returns)", elapsed / 1e6, (unsigned long long)n, (unsigned long long)g_stall_n); }
        if (active && !g_rescue_mode && g_dm_present_link && (probe_failed || elapsed >= LINK_RESCUE_NS)) {
            g_rescue_mode = g_dm_present_link; g_rescue_n++; g_rescue_back_at = now + g_rescue_back_ns;
            if (probe_failed) { g_link_probe_fail++; MLOG("display link tried again and silent for %.0f ms: the present thread takes drawables again (rescue #%llu, %llu tries failed), the link is tried again in %.0f s", pelapsed / 1e6, (unsigned long long)g_rescue_n, (unsigned long long)g_link_probe_fail, g_rescue_back_ns / 1e9); }
            else MLOG("display link silent for %.0f ms while active: the present thread takes drawables itself (rescue #%llu), the link is tried again in %.0f s", elapsed / 1e6, (unsigned long long)g_rescue_n, g_rescue_back_ns / 1e9);
            g_link_probe_pending = 0; g_link_probe_at = 0;
            g_rescue_back_ns = g_rescue_back_ns < 30000000000ull ? g_rescue_back_ns * 2 : 60000000000ull;
        }
        if (active && !g_stall_red && elapsed >= STALL_RED_NS) { g_stall_red = 1; MLOG("[RED] display link silent for %.1f s while active; presenting relies entirely on fallback frame drops, the picture is frozen", elapsed / 1e9); }
    }
    pthread_mutex_unlock(&g_pq_mu);
}
// Wait until all queued presents have been processed (used by mtl_finish = nvnQueueFinish etc.): goes through mtl_present_wait, so there is still a fallback when the display link stops (real device: the game called QueueFinish once while the display link was stopped,
// and this used to wait here without a timeout; nobody took the 3 queued frames and the game stalled; those two runs on Mac did not hit this call)
void mtl_present_flush(void) { pthread_mutex_lock(&g_pq_mu); uint64_t h = g_pq_in; pthread_mutex_unlock(&g_pq_mu); mtl_present_wait(h); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_stat_pass; *draws = g_stat_draw; cp_report(); }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse, uint64_t *patch) { *enc = g_stat_args_enc; *reuse = g_stat_args_reuse; *patch = g_stat_args_patch; }
void mtl_vbuf_stats(uint64_t *resolve, uint64_t *reuse, uint64_t *set, uint64_t *skip, uint64_t *copies, uint64_t *copy_bytes) {
    *resolve = g_vbuf_resolve; *reuse = g_vbuf_reuse; *set = g_vbuf_set; *skip = g_vbuf_skip; *copies = g_vbuf_copies; *copy_bytes = g_vbuf_copy_bytes;
}
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = 0; *skip = 0; }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = g_bw_load; *store = g_bw_store; *full = g_pass_full_rw; }
uint64_t mtl_udc_saved_bytes(void) { return g_udc_saved_bytes; }
int mtl_has_window(void) { return front_metal_layer() != NULL; }
int mtl_link_active(void) { return link_on(); }
int mtl_link_stalled(void) { return __atomic_load_n(&g_stall_t0, __ATOMIC_RELAXED) != 0; }   // the display link has gone silent and mtl_present_wait drops frames (see STALL_NS) until it calls back
void mtl_link_pause(int on) { CAMetalDisplayLink *l = g_link; if (l) l.paused = on != 0; }   // For testing (DM_TEST_LINK_STALL): imitates the system stopping the display-link callback in the background
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    // Wait for the display link only when running faster than the display: if a display-link tick has already passed since the previous frame started work (this frame is already behind), start immediately instead of waiting in vain for the next display-link tick --
    // otherwise when the frame time is slightly more than one display-link period it would "miss the tick -> wait another whole tick", locking the frame rate straight to half (measured on Mac under windowed load: runs of dropped frames)
    // How many display-link ticks one frame takes: the game's present interval (vsyncs, 60 Hz) times 1 with a 60 Hz display link, times 2 with a 120 Hz one, halved and rounded up at 30 Hz.
    // A frame presented with interval N blocks the game for N vsyncs on the Switch, as the display callback holds it N ticks: games that scale their time step by the interval (BotW's VFR: interval / 2)
    // ran at double speed while they produced interval-2 frames every 60 Hz tick (Link sprinting with B, the interval flapping 1 ↔ 2)
    if (vsyncs < 1) vsyncs = 1;
    return mtl_link_wait_ticks(g_link_mode == 3 ? (vsyncs + 1) / 2 : vsyncs * (g_link_mode == 2 ? 2 : 1), timeout_ns);
}
// The display link's rate: 120 Hz (DM_PRESENT_LINK=2), 60, or 30 in power-saving mode
void mtl_link_mode_set(int mode) { pthread_mutex_lock(&g_pq_mu); g_dm_present_link = mode; pthread_mutex_unlock(&g_pq_mu); }   // 120 fps follows the screen: link_sync applies it
int mtl_link_rate(void) { return g_link_mode == 3 ? 30 : g_link_mode == 2 ? 120 : 60; }
// The same wait counted in the display link's own ticks: one per frame for a frame rate as high as the link's (present_interval display at DM_PRESENT_HZ=120 on a 120 Hz link)
// The absolute time (CLOCK_REALTIME, as pthread_cond_timedwait takes it) ns nanoseconds from now
static struct timespec link_deadline(uint64_t ns) {
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += ns / 1000000000ull; ts.tv_nsec += ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    return ts;
}
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns) {
    if (!mtl_link_active()) return 0;
    static uint64_t started_tick;   // Display-link tick number when the previous frame started work (only the game's present thread calls this function)
    const uint64_t step = ticks < 1 ? 1 : (uint64_t)ticks;
    // A drain the callback asked for (nvn_link_drain.h) is taken by this wait or not at all: a wait that finds its tick already passed is a game that fell behind since
    const int req = atomic_exchange(&g_link_drain_req, 0), drain = req & (LINK_DRAIN_LATCH - 1), into_latch = !!(req & LINK_DRAIN_LATCH);
    pthread_mutex_lock(&g_link_mu);
    if (g_link_tick_n >= started_tick + step) { started_tick = g_link_tick_n; pthread_mutex_unlock(&g_link_mu); atomic_store(&g_link_blocked_ns, 0); return 1; }
    uint64_t w0 = now_ns();
    struct timespec ts = link_deadline(timeout_ns);
    g_link_waiter = drain > 0 ? 0 : started_tick + step;   // the callback of this tick waits for this frame's present (with a drain into the latch, the tick that ends it, below)
    while (g_link_tick_n < started_tick + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { g_link_waiter = 0; pthread_mutex_unlock(&g_link_mu); atomic_store(&g_link_blocked_ns, 0); return 0; }
    g_link_waiter = 0;
    uint64_t w1 = now_ns(), drained = 0;
    atomic_store(&g_link_blocked_ns, w1 - w0);
    if (drain > 0) {   // the standing frames go to the screen on these ticks; a link that goes silent meanwhile ends the drain after two tick periods each
        const uint64_t until = g_link_tick_n + (uint64_t)drain;
        ts = link_deadline((uint64_t)drain * 2000000000ull / (uint64_t)mtl_link_rate());
        if (into_latch) { g_link_waiter = until; g_link_waiter_end = 1; }   // by its end tick the queue has run empty, and that callback latches this frame's present
        while (g_link_tick_n < until) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) break;
        g_link_waiter = 0; g_link_waiter_end = 0;
        drained = now_ns() - w1;
    }
    started_tick = g_link_tick_n;
    pthread_mutex_unlock(&g_link_mu);
    if (drain > 0) {
        atomic_fetch_add(&g_link_drain_ns, drained);
        uint64_t n = atomic_fetch_add(&g_link_drained, 1) + 1;
        if (n <= 20) MLOG("display link: frames stood in the present queue while the game kept up, the acquire waited %d more tick%s (%.1f ms) to let them out (drain #%llu, frame %llu%s)",
                          drain, drain == 1 ? "" : "s", drained / 1e6, (unsigned long long)n, (unsigned long long)atomic_load(&g_cur_frame), n == 20 ? "; further drains only counted" : "");
    }
    return 1;
}
// The time acquires spent draining standing frames since the last call (nanoseconds): the caller counts it as pacing, not as a frame that took longer
uint64_t mtl_link_drain_take_ns(void) { return atomic_exchange(&g_link_drain_ns, 0); }

// The display's vsync for vi::GetDisplayVsyncEvent (hle_sdk16.c): returns 1 at the first display-link tick of the next 60 Hz vsync (120 Hz at 120 fps, nvn_vsync_hz) after *seen (the tick count it last returned, updated),
// 0 when the link does not run, runs at 30 Hz (power saving: the 60 Hz event cannot come from it) or gives no tick within the timeout; the caller then keeps its own clock
int nvn_vsync_hz(void);   // hle_nvn.c: 120 while 120 fps holds one tick of 1/120 s, else 60
int mtl_link_wait_vsync(uint64_t *seen, uint64_t timeout_ns) {
    if (!mtl_link_active() || g_link_mode == 3) return 0;
    const uint64_t step = g_link_mode == 2 && nvn_vsync_hz() != 120 ? 2 : 1;   // a 120 Hz link: every tick at 120 fps, every other one otherwise
    pthread_mutex_lock(&g_link_mu);
    if (*seen > g_link_tick_n || g_link_tick_n > *seen + 4 * step) *seen = g_link_tick_n;   // first call, or far behind (a stall): from the current tick on, no burst
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ns / 1000000000ull; ts.tv_nsec += timeout_ns % 1000000000ull;
    if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
    while (g_link_tick_n < *seen + step) if (pthread_cond_timedwait(&g_link_cv, &g_link_mu, &ts) == ETIMEDOUT) { pthread_mutex_unlock(&g_link_mu); return 0; }
    *seen += step;
    pthread_mutex_unlock(&g_link_mu); return 1;
}



// Called by the watchdog (no progress): writes the state of all blocking wait points on the graphics side to the log -- who is waiting for whom is visible at a glance (after switching to the Metal 4 submit model there was once a case of "ran to 900 frames then stopped, no failure line")
void mtl_wait_state_dump(void) {
    MLOG("wait points: main fence submitted %llu / completed %llu; main allocator fences [%llu %llu %llu]; uploads pending %d, allocators busy [%d %d %d], seq %u, unflushed batch %d; present queue in %llu / done %llu; main command buffer encoding %d, submit thread set %d",
         (unsigned long long)g_fence, (unsigned long long)g_done, (unsigned long long)g_alloc_fence[0], (unsigned long long)g_alloc_fence[1], (unsigned long long)g_alloc_fence[2],
         atomic_load(&g_up_pending), atomic_load(&g_up_alloc_busy[0]), atomic_load(&g_up_alloc_busy[1]), atomic_load(&g_up_alloc_busy[2]), g_up_seq, g_up_cb != nil,
         (unsigned long long)g_pq_in, (unsigned long long)g_pq_done, g_cb != nil, atomic_load(&g_sub_thread_set));
}
void mtl_alloc_stats(uint64_t *waits, uint64_t *grown, int *n) { *waits = g_alloc_waits; *grown = g_alloc_grown; *n = g_alloc_n; }   // Main submit allocator pool: how many times it waited, how many times it grew, how many exist now
// Memory census for restop (diagnostic): residency-set textures by Metal format; which of them anything still names (serial tables, sharing table, descriptor views) and which only the
// deferred-release queue or nothing holds; and game textures at one storage address held as several Metal textures. Under no lock of the caller's
static char census_flags_key;
static id<MTLTexture> census_base(id in) { id o = [in isKindOfClass:[NvPkg class]] ? ((NvPkg *)in)->tex : in; if (![o conformsToProtocol:@protocol(MTLTexture)]) return nil; id<MTLTexture> t = o; return t.parentTexture ? t.parentTexture : t; }
void mem_census(const char *why) { @autoreleasepool {
    NSHashTable *named = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    NSHashTable *named_ret = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];   // named only through retired descriptors (until a live one names it)
    // The textures met, retained: the tables here hold bare pointers, and a texture they name can be freed while the census runs on its own thread (a load's census read the size
    // of one freed meanwhile and crashed in objc_msgSend). Each is retained under the lock that names it, and kept until the census returns, so every texture it reads stays valid
    NSMutableSet *keep = [NSMutableSet set];
    #define CN_ADD(o) do { id<MTLTexture> b_ = census_base(o); if (b_) { [named addObject:(__bridge id)(__bridge void *)b_]; [keep addObject:b_]; } } while (0)
    pthread_mutex_lock(&g_smu);
    void (^census_one)(id o) = ^(id o) { CN_ADD(o); if ([o isKindOfClass:[NvPkg class]] && ((NvPkg *)o)->spare) CN_ADD(((NvPkg *)o)->spare); };
    own_each_locked(^(uint64_t serial, id o, int ghost) { (void)serial; (void)ghost; census_one(o); });   // the serial tables (nvn_owner.m), then the sharing table
    own_alias_each_locked(^(const alias_key *k, id o) { (void)k; census_one(o); });
    pthread_mutex_unlock(&g_smu);
    // registered descriptors: their views' bases, and per storage address the distinct bases
    NSMapTable *by_mem = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality valueOptions:NSPointerFunctionsStrongMemory];
    NSMapTable *by_flags = [NSMapTable mapTableWithKeyOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality valueOptions:NSPointerFunctionsStrongMemory];   // texture flags | pool flags << 32
    os_unfair_lock_lock(&g_rmu);
    uint64_t nreg = 0, nretired = 0;
    for (int i = 1; i < g_ntreg; i++) { treg_t *r = &g_treg[i]; if (!r->ok) continue; nreg++; nretired += r->retired;
        void *vs[7] = {r->view, r->alt, r->im, r->palt[0], r->palt[1], r->palt[2], r->palt[3]};
        for (int k = 0; k < 7; k++) if (vs[k]) { id<MTLTexture> b = census_base((__bridge id)vs[k]); if (!b) continue; [r->retired ? named_ret : named addObject:(__bridge id)(__bridge void *)b]; [keep addObject:b];
            if (k == 0 && r->t.mem) { const void *key = r->t.mem; NSHashTable *set = [by_mem objectForKey:(__bridge id)key];
                if (!set) { set = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality]; [by_mem setObject:set forKey:(__bridge id)key]; }
                [set addObject:(__bridge id)(__bridge void *)b];
                if (![by_flags objectForKey:(__bridge id)key]) [by_flags setObject:@(((uint64_t)(r->t.pool && r->t.pool->magic == MAGIC('P', 'O', 'O', 'L') ? r->t.pool->flags : 0) << 32) | r->t.d.flags) forKey:(__bridge id)key]; } } }
    os_unfair_lock_unlock(&g_rmu);
    NSHashTable *waiting = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    pthread_mutex_lock(&g_deferred_mu); for (NvDeferredRelease *d in g_deferred) { id<MTLTexture> b = census_base(d->object); if (b) { [waiting addObject:(__bridge id)(__bridge void *)b]; [keep addObject:b]; } } pthread_mutex_unlock(&g_deferred_mu);
    uint64_t small_n[2][2] = {{0}}, small_b[2][2] = {{0}};   // [texture][shared storage]: residency objects of 64 KB or less
    uint64_t n_ret = 0, b_ret = 0, n_named = 0, b_named = 0, n_wait = 0, b_wait = 0, n_none = 0, b_none = 0, n_buf = 0, b_buf = 0;
    enum { NF = 24 }; struct { unsigned long pf; uint64_t n, b; } fm[NF] = {{0}}; int nfm = 0;
    pthread_mutex_lock(&g_res_mu);
    for (id<MTLAllocation> o in g_res_objects.allObjects) { uint64_t z = o.allocatedSize;
        if (z <= 65536) { int tx = [(id)o conformsToProtocol:@protocol(MTLTexture)], sh = ((id<MTLResource>)o).storageMode == MTLStorageModeShared; small_n[tx][sh]++; small_b[tx][sh] += z; }
        if (![(id)o conformsToProtocol:@protocol(MTLTexture)]) { n_buf++; b_buf += z; continue; }
        id<MTLTexture> t = (id)o; if (t.parentTexture) continue;
        unsigned long pf = (unsigned long)t.pixelFormat; int i = 0; while (i < nfm && fm[i].pf != pf) i++; if (i == nfm && nfm < NF) fm[nfm++].pf = pf; if (i < NF) { fm[i].n++; fm[i].b += z; }
        if ([named containsObject:(__bridge id)(__bridge void *)t]) { n_named++; b_named += z; }
        else if ([named_ret containsObject:(__bridge id)(__bridge void *)t]) { n_ret++; b_ret += z; }
        else if ([waiting containsObject:(__bridge id)(__bridge void *)t]) { n_wait++; b_wait += z; }
        else { n_none++; b_none += z; } }
    pthread_mutex_unlock(&g_res_mu);
    uint64_t dup_addr = 0, dup_tex = 0, dup_b = 0;
    for (id key in by_mem) { NSHashTable *set = [by_mem objectForKey:key]; if (set.count < 2) continue; dup_addr++;
        uint64_t biggest = 0, sum = 0; for (id b in set) { uint64_t z = ((id<MTLTexture>)b).allocatedSize; sum += z; if (z > biggest) biggest = z; } dup_tex += set.count - 1; dup_b += sum - biggest; }
    {   // the largest groups: each Metal texture over one storage address, so a reader can tell views made as copies from different images that reuse the memory
        NSMutableArray *groups = [NSMutableArray array];
        for (id key in by_mem) { NSHashTable *set = [by_mem objectForKey:key]; if (set.count >= 2) { [groups addObject:set]; objc_setAssociatedObject(set, &census_flags_key, [by_flags objectForKey:key], OBJC_ASSOCIATION_RETAIN_NONATOMIC); } }
        [groups sortUsingComparator:^NSComparisonResult(NSHashTable *x, NSHashTable *y) {
            uint64_t a_ = 0, b_ = 0; for (id t in x) a_ += ((id<MTLTexture>)t).allocatedSize; for (id t in y) b_ += ((id<MTLTexture>)t).allocatedSize;
            return a_ > b_ ? NSOrderedAscending : a_ < b_ ? NSOrderedDescending : NSOrderedSame; }];
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
    char line[900] = "none"; size_t o = 0; for (int i = 0; i < nfm && i < 12 && o < sizeof line; i++) o += snprintf(line + o, sizeof line - o, "%s%lu: %llu, %llu MB", i ? "; " : "", fm[i].pf, (unsigned long long)fm[i].n, (unsigned long long)(fm[i].b >> 20));
    MLOG("%s census: textures named by serials, sharing or live descriptors' views %llu, %llu MB; only by retired descriptors' views %llu, %llu MB; only in the deferred-release queue %llu, %llu MB; named by nothing %llu, %llu MB; buffers %llu, %llu MB; descriptors %llu (%llu retired); storage addresses with several Metal textures %llu (%llu extra textures, %llu MB)",
         why, (unsigned long long)n_named, (unsigned long long)(b_named >> 20), (unsigned long long)n_ret, (unsigned long long)(b_ret >> 20), (unsigned long long)n_wait, (unsigned long long)(b_wait >> 20), (unsigned long long)n_none, (unsigned long long)(b_none >> 20),
         (unsigned long long)n_buf, (unsigned long long)(b_buf >> 20), (unsigned long long)nreg, (unsigned long long)nretired, (unsigned long long)dup_addr, (unsigned long long)dup_tex, (unsigned long long)(dup_b >> 20));
    MLOG("%s census: textures by Metal format: %s", why, line);
    MLOG("%s census: residency objects of 64 KB or less: buffers shared %llu (%llu KB), private %llu (%llu KB); textures shared %llu (%llu KB), private %llu (%llu KB)", why,
         (unsigned long long)small_n[0][1], (unsigned long long)(small_b[0][1] >> 10), (unsigned long long)small_n[0][0], (unsigned long long)(small_b[0][0] >> 10),
         (unsigned long long)small_n[1][1], (unsigned long long)(small_b[1][1] >> 10), (unsigned long long)small_n[1][0], (unsigned long long)(small_b[1][0] >> 10));
    { uint64_t nb[4] = {0}, bb[4] = {0};   // 0: only texture uploads looked it up, 1: anything else did, 2: nothing did, 3: both
      pthread_mutex_lock(&g_mu);
      for (int i = 0; i < g_npool; i++) { pool_ent *p = &g_pools[i]; int c = p->n_tex_reads && p->n_other ? 3 : p->n_other ? 1 : p->n_tex_reads ? 0 : 2; nb[c]++; bb[c] += p->hi - p->lo; }
      pthread_mutex_unlock(&g_mu);
      MLOG("%s census: memory pools looked up only by texture uploads %llu, %llu MB; by other uses only %llu, %llu MB; by both %llu, %llu MB; by nothing %llu, %llu MB", why,
           (unsigned long long)nb[0], (unsigned long long)(bb[0] >> 20), (unsigned long long)nb[1], (unsigned long long)(bb[1] >> 20), (unsigned long long)nb[3], (unsigned long long)(bb[3] >> 20), (unsigned long long)nb[2], (unsigned long long)(bb[2] >> 20)); }
    #undef CN_ADD
} }
