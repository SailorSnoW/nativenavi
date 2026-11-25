// Submissions: start, command buffers, Metal 4 barriers, the GPU profiler, the encoders' calls (enc_r_*, enc_c_*, enc_b_*), low GPU address checks, copy encoder order.
// A frame's submissions share an allocator: Metal asks only that all its command buffers complete before its reset. ALLOC_SHARE bounds one while no frame ends.
static _Atomic uint64_t g_cur_frame;
static uint64_t g_alloc_frame, g_alloc_shared, g_alloc_shares;   // the frame it was taken in, the submissions it served since its reset
static int g_alloc_used;
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static int mtl_wait_at(uint64_t fence, uint64_t timeout_ns, int site);
static void up_flush(void);
static void up_idle_release(void);
// The upload batch's encoder belongs to the submit thread: every entry point checks the thread and aborts on another one, so a wrong thread shows deterministically.
static pthread_t g_sub_thread;
static _Atomic int g_sub_thread_set;
static int on_sub_thread(void) { return atomic_load(&g_sub_thread_set) && pthread_equal(pthread_self(), g_sub_thread); }
static void up_owner_check(const char *where) {
    if (!atomic_load(&g_sub_thread_set) || on_sub_thread()) return;
    char name[64] = ""; pthread_getname_np(pthread_self(), name, sizeof name);
    MLOG("[RED] upload batches belong to the submit thread only: %s called on thread '%s'", where, name);
    void *bt[32];
    int n = backtrace(bt, 32);
    backtrace_symbols_fd(bt, n, 2);
    abort();
}
// On the submit thread the batch's uploads are flushed first. The old queue has no ordering with the Metal 4 queue the uploads are on, so the upload is also waited for.
static void up_wait_zero(_Atomic int *v, int site);
static id<MTLCommandBuffer> q_cb(void) {
    if (on_sub_thread()) {
        up_flush();
        rec_drain();
        up_wait_zero(&g_up_pending, WS_OLDQ);
    }
    return cl_cb();
}
static post_op *g_post;
static int g_npost, g_postcap;
enum { B2B_MAX = 4096 };
static uint64_t g_b2b_lo[B2B_MAX], g_b2b_hi[B2B_MAX];
static int g_b2b_n, g_b2b_over;
// Submissions run on a long-lived guest thread whose implicit pool never drains, so the replay needs a pool of its own.
void *objc_autoreleasePoolPush(void);
void objc_autoreleasePoolPop(void *);
static void *g_sub_pool;
static void treg_finish_submission(void);

static uint64_t g_sub_serial;
static int g_pkg_kept;   // the next submission starts a write generation, as a write would
// The prepare step (warm_start) before the first submission is encoded; the submit entry calls it outside the graphics gate. The guest clock is frozen across it
// (hle_os.c reason 3): it can stall the render thread for minutes, and the game would otherwise skip frames and tear down resources frame 1 still waits on.
void mtl_prepare(void) {
    if (!g_warm_started && prepare_on()) {
        guest_clock_freeze_for(3, 1);
        mtl_init(); shmap_load();
        guest_clock_freeze_for(3, 0);
    }
}
static void sub_cut_begin(void);
static void ts_sub_begin(void), ts_boundary(void);
static int ts_on(void);
static void tu_tick(void);
static void gmem_commit_patch(void);
static void treg_sparse_notes(void);
// One record per submission in flight: at most ALLOC_MAX allocators of ALLOC_SHARE submissions stand uncompleted, so a slot's previous holder has completed.
enum { POST_RING = SUB_FLIGHT_MAX };
static void pass_split_try(void);
static uint64_t g_setrt_n;
static int g_split_resume;   // the pass was split at this draw: it opens again where the dispatch scan already looked
static void mem_trim(void);
static void pkg_sweep(void);
static uint64_t spare_trim(int all, unsigned *n_out);
static unsigned g_evict_why[5];   // the last trim's packages: considered, recently used, of a kind not freed, GPU-written, held
void mtl_sub_begin(void) {
    mtl_init();
    g_sub_serial++; g_seg = 0; g_vis_need = 1;
    treg_sparse_notes();
    if (g_pkg_kept) { g_pkg_kept = 0; nvn_dirty_touch(); }
    seg_clk_put(g_sclk);
    g_sclk = NULL;
    sub_cut_begin();
    defer_drain();
    rh_trim();
    tu_tick();
    spare_trim(0, NULL);
    mem_trim();
    pkg_sweep();
    mtl_prepare();   // done by then in the submit entry; the replay tool reaches it here
    warm_claim();
    if (g_cb) MDIE("submit re-entered");
    if (atomic_load(&g_hold_on) && g_fence - g_done >= POST_RING - 16) hold_block(0);   // a hold's fences complete only after it: the post ring must not come round to one
    if (!atomic_load(&g_sub_thread_set) || !pthread_equal(g_sub_thread, pthread_self())) nvn_dirty_own_thread();
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    up_idle_release();
    atomic_store(&g_last_commit_error, 0);
    g_sub_pool = objc_autoreleasePoolPush();
    atomic_store(&g_sub_open, g_fence + 1); atomic_thread_fence(memory_order_seq_cst);   // before any pool lookup of this submission; only this thread moves g_fence
    g_cb = rec_cb_new();
    // No GPU wait mid-frame: a free allocator, else a new one, and only at the cap a wait for the oldest. The game's swapchain acquire bounds how far ahead the CPU runs.
    int alloc_i = -1; uint64_t done = g_done, alloc_frame = atomic_load(&g_cur_frame);
    int alloc_share = g_alloc_used && g_alloc_frame == alloc_frame && g_alloc_shared < ALLOC_SHARE && allocprof_level() != 2;
    if (alloc_share) alloc_i = g_alloc_cur;
    for (int i = 0; alloc_i < 0 && i < g_alloc_n; i++) if (g_alloc_fence[i] <= done) { alloc_i = i; break; }
    if (alloc_i < 0 && g_alloc_n < ALLOC_MAX) {
        NSError *ae = nil;
        MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        g_alloc4[g_alloc_n] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_alloc4[g_alloc_n]) MDIE("failed to create a new MTL4CommandAllocator: %s", ae.localizedDescription.UTF8String);
        alloc_i = g_alloc_n++;
        g_alloc_grown++;
    }
    if (alloc_i < 0) {
        alloc_i = 0;
        for (int i = 1; i < g_alloc_n; i++) if (g_alloc_fence[i] < g_alloc_fence[alloc_i]) alloc_i = i;
        g_alloc_waits++;
        mtl_wait_at(g_alloc_fence[alloc_i], UINT64_MAX, WS_ALLOC);
    }
    if (allocprof_level() == 2) {
        if (g_alloc4[alloc_i].allocatedSize) {
            uint64_t z = g_alloc4[alloc_i].allocatedSize;
            g_cm_sub_n++; g_cm_sub_sum += z;
            if (z > g_cm_sub_max) g_cm_sub_max = z;
            int hb = 0;
            while (hb < 6 && z >= (4ull << 20) << hb) hb++;
            g_cm_hist[hb]++;
        }
        {
            uint64_t ne = g_cm_enc - g_cm_enc0, nd = g_cm_mdraws - g_cm_mdraws0, zz = g_alloc4[g_alloc_cur].allocatedSize;
            if (zz > g_cm_big_z) { g_cm_big_z = zz; g_cm_big_enc = ne; g_cm_big_draws = nd; }
            g_cm_enc0 = g_cm_enc; g_cm_mdraws0 = g_cm_mdraws;
        }
        g_alloc4[alloc_i] = [g_dev newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new] error:nil];
        g_cm_last = 0;
    }
    g_alloc_cur = alloc_i;
    if (alloc_share) { g_alloc_shared++; g_alloc_shares++; }
    else {
        [g_alloc4[alloc_i] reset];
        g_alloc_frame = alloc_frame; g_alloc_shared = 1; g_alloc_used = 1;
    }
    [g_cb beginCommandBufferWithAllocator:g_alloc4[alloc_i]];
    ts_sub_begin();
    g_commit_blocks = [NSMutableArray array];
    if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis);
    g_npost = 0; g_vis = nil; g_vis_n = 0; g_vis_cur = -1; pend_reset();
    g_b2b_n = 0; g_b2b_over = 0;   // copies made outside mtl_exec (tests) keep only their completion stamp
    g_vis_ring_i = -1;
    void arena_begin(void); arena_begin();
}

static void rp_end_at(const char *fn, int line);
// pass dumps name the call site of every render pass end
#define rp_end() rp_end_at(__func__, __LINE__)
// Metal 4 conservative barriers: a consumer barrier at an encoder's start and a producer barrier at its end; the start one alone is not enough. Old-queue encoders take neither.
static int g_pass_swrite;
static int g_pass_order;    // ordering barriers after draws none of which wrote (1 primitives, 2 fragments): the next draw that writes is ordered after them first
static int g_pass_late_res;
static uint64_t g_order_vbar;
static void img_fit_upgrade(void);
static int g_imfit_npend;   // upgraded before the next draw pass opens
static int g_last_end_relaxed, g_game_barrier;
#define GAME_BARRIER_FULL (NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_INDIRECT_DATA_BIT)   // bits that order earlier work before later vertex or indirect fetch: the next draw pass takes the full barrier
static uint64_t g_bar_skip, g_bar_full;
static void gp_end(id e);
// MTL4VisibilityOptionResourceAlias costs a command allocator about 100 KB per encoder on the end barrier and nothing on the start barrier, so only start barriers carry it
// (a copy encoder that wrote through an aliased address keeps it at its end too). Every encoder on the main command buffer starts with resource_vis().
static inline void m4_end(id e) {
    gp_end(e);
    static int end_alias = -1; if (end_alias < 0) end_alias = sw_mode("DM_NVN_END_ALIAS", 0);
    MTL4VisibilityOptions vis = e == g_blit && g_blit_alias ? blit_vis() : end_alias && sw_on(end_alias) ? resource_vis() : MTL4VisibilityOptionDevice;
    if (e == g_blit) g_blit_alias = 0;
    if ([e conformsToProtocol:@protocol(MTL4CommandEncoder)]) [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:vis];
    [e endEncoding]; g_last_end_relaxed = 0; cm_enc_mark(-1);
}
// Draw passes ordered by the game's barriers: a pass ends with a barrier from its vertex and fragment stages to later fragment, compute and copy work, so the next pass's vertex
// work overlaps its fragments. The next pass adds the full consumer barrier after GAME_BARRIER_FULL or non-draw work in between.
static int barrier_game(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_BARRIER");
        v = e && !strncmp(e, "ab", 2) ? sw_mode("DM_NVN_BARRIER", 0) : e ? !strcmp(e, "game") : DM_GAME_DRAW_BARRIERS_GAME;
        if (v == 1) MLOG("DM_NVN_BARRIER=game: draw passes are ordered by the game's own barriers (fragment work still has barriers between passes; vertex work may overlap the previous pass's fragments)");
    }
    return sw_on(v);
}
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = g_bar_skip; *full = g_bar_full; }
uint64_t mtl_cut_count(void);
// DM_NVN_GPUPROF: relaxed timestamps at each encoder's start and end, so nothing is serialized beyond the replay's own barriers. dm/tools/gpuprof.py sums the lines by pass shape.
enum { GP_MAX = 4096 };   // the largest timestamp heap Metal makes
typedef struct { char kind; uint16_t w, h; uint8_t nc, depth, loads; uint32_t draws, seg; uint64_t pass; uint32_t ld_kb, st_kb; uint16_t cfmt, dfmt; } gp_lab;
static id<MTL4CounterHeap> g_gp_heap;
static gp_lab g_gp_lab[GP_MAX / 2];
static int g_gp_n, g_gp_state, g_gp_cur = -1;
static long g_gp_f0 = -2, g_gp_f1, g_gp_every;
static uint64_t g_gp_frame;
static const void *g_gp_e;   // m4_end also ends the upload / diagnostics / present encoders, which may end while a profiled pass is open
static _Atomic int g_gp_busy;   // a periodic profile's report has not been printed yet: the heap is still being read
static uint64_t g_bw_load, g_bw_store, g_gp_bw_l0, g_gp_bw_s0;
static uint64_t g_stat_pass, g_stat_draw;
static uint32_t g_gp_copies, g_gp_disp, g_gp_ubd, g_gp_bar;
static int gp_active(void) {
    if (__builtin_expect(g_gp_f0 == -1, 1)) return 0;
    if (g_gp_f0 == -2) {
        const char *e = getenv("DM_NVN_GPUPROF");
        if (e && !strncmp(e, "every:", 6)) {
            g_gp_every = atol(e + 6);
            g_gp_f0 = g_gp_every > 0 ? 0 : -1;
            if (g_gp_every > 0) MLOG("DM_NVN_GPUPROF: one frame in every %ld profiled", g_gp_every);
        } else {
            g_gp_f0 = e ? atol(e) : -1;
            g_gp_f1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_gp_f0;
        }
        if (g_gp_f0 < 0) return 0;
    }
    long f = (long)atomic_load(&g_cur_frame);
    if (g_gp_every > 0) return f > 0 && f % g_gp_every == 0;
    return f >= g_gp_f0 && f <= g_gp_f1;
}
static void gp_report(int n, gp_lab *lab, uint64_t frame);
static void gp_periodic_finish(void) {   // on the submit thread, before the next frame's first encoder can touch the heap
    if (!g_gp_every || g_gp_state != 1 || g_gp_frame == atomic_load(&g_cur_frame)) return;
    int n = g_gp_n;
    gp_lab *lab = malloc(sizeof *lab * (n ? n : 1));
    memcpy(lab, g_gp_lab, sizeof *lab * n);
    uint64_t frame = g_gp_frame;
    g_gp_state = 2; atomic_store(&g_gp_busy, 1);
    static MTLSharedEventListener *listener; if (!listener) listener = [MTLSharedEventListener new];
    // g_fence covers all submissions of the sampled frame
    [g_ev notifyListener:listener atValue:g_fence block:^(id<MTLSharedEvent> event, uint64_t value) { (void)event; (void)value; gp_report(n, lab, frame); }];
}
static id<MTL4CounterHeap> g_ts_heap;
static int g_ts_pend = -1;
static void gp_begin(id e, char kind, MTL4RenderPassDescriptor *d) {
    if (g_ts_pend >= 0) {
        NSUInteger slot = (NSUInteger)g_ts_pend;
        g_ts_pend = -1;
        if (kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageVertex intoHeap:g_ts_heap atIndex:slot];
        else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_ts_heap atIndex:slot];
    }
    gp_periodic_finish();
    if (g_gp_every > 0 && g_gp_state == 2 && !atomic_load(&g_gp_busy)) g_gp_state = 0;
    if (!gp_active() || g_gp_state == 2) return;
    if (!g_gp_heap) {
        MTL4CounterHeapDescriptor *hd = [MTL4CounterHeapDescriptor new];
        hd.type = MTL4CounterHeapTypeTimestamp; hd.count = GP_MAX;
        NSError *er = nil;
        g_gp_heap = [g_dev newCounterHeapWithDescriptor:hd error:&er];
        if (!g_gp_heap) {
            MLOG("[RED] GPUPROF: no counter heap: %s", er.description.UTF8String);
            g_gp_f0 = -1;
            return;
        }
    }
    if (!g_gp_state) {
        g_gp_state = 1;
        g_gp_n = 0;
        g_gp_frame = atomic_load(&g_cur_frame);
        [g_gp_heap invalidateCounterRange:NSMakeRange(0, GP_MAX)];
    }
    if (2 * g_gp_n + 2 > GP_MAX) return;
    gp_lab l = {kind, 0, 0, 0, 0, 0, kind == 'R' ? (uint32_t)g_stat_draw : g_gp_copies, g_seg, g_stat_pass, 0, 0, 0, 0};
    if (kind != 'R') { l.w = (uint16_t)g_gp_disp; l.h = (uint16_t)g_gp_ubd; l.ld_kb = (uint32_t)g_gp_bar; }
    if (d) {
        for (int i = 0; i < 8; i++) {
            MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i];
            if (!c.texture) continue;
            if (!l.nc) l.cfmt = (uint16_t)c.texture.pixelFormat;
            l.nc++;
            l.w = (uint16_t)MAX(c.texture.width >> c.level, 1); l.h = (uint16_t)MAX(c.texture.height >> c.level, 1);
            l.loads += c.loadAction == MTLLoadActionLoad;
        }
        MTLRenderPassDepthAttachmentDescriptor *z = d.depthAttachment;
        if (z.texture || d.stencilAttachment.texture) l.dfmt = (uint16_t)(z.texture ? z.texture : d.stencilAttachment.texture).pixelFormat;
        if (z.texture) {
            l.depth = 1;
            l.w = (uint16_t)MAX(z.texture.width >> z.level, 1); l.h = (uint16_t)MAX(z.texture.height >> z.level, 1);
            l.loads += z.loadAction == MTLLoadActionLoad;
        }
    }
    g_gp_lab[g_gp_n] = l; g_gp_cur = g_gp_n; g_gp_e = (__bridge const void *)e;
    if (kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageVertex intoHeap:g_gp_heap atIndex:2 * g_gp_n];
    else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_gp_heap atIndex:2 * g_gp_n];
    g_gp_n++;
}
static void gp_end(id e) {
    if (g_gp_cur < 0 || (__bridge const void *)e != g_gp_e) return;   // another queue's encoder: the open pass gets its end stamp from its own end
    int k = g_gp_cur; g_gp_cur = -1; g_gp_e = NULL;
    if (g_gp_lab[k].kind == 'R') {
        g_gp_lab[k].ld_kb = (uint32_t)((g_bw_load - g_gp_bw_l0) >> 10);
        g_gp_lab[k].st_kb = (uint32_t)((g_bw_store - g_gp_bw_s0) >> 10);
    }
    if (g_gp_lab[k].kind == 'R') g_gp_lab[k].draws = (uint32_t)g_stat_draw - g_gp_lab[k].draws;
    else {
        g_gp_lab[k].draws = g_gp_copies - g_gp_lab[k].draws;
        g_gp_lab[k].w = (uint16_t)(g_gp_disp - g_gp_lab[k].w);
        g_gp_lab[k].h = (uint16_t)(g_gp_ubd - g_gp_lab[k].h);
        g_gp_lab[k].ld_kb = g_gp_bar - g_gp_lab[k].ld_kb;
    }
    if (g_gp_lab[k].kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageFragment intoHeap:g_gp_heap atIndex:2 * k + 1];
    else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_gp_heap atIndex:2 * k + 1];
}
static void gp_report(int n, gp_lab *lab, uint64_t frame) {
    NSData *r = [g_gp_heap resolveCounterRange:NSMakeRange(0, 2 * n)];
    const uint64_t *t = r.bytes;
    if (!t) {
        free(lab);
        atomic_store(&g_gp_busy, 0);
        return;
    }
    double tick_ns = 1e9 / (double)[g_dev queryTimestampFrequency], tot[2] = {0}, span = 0;
    uint64_t t0 = 0, t1 = 0, ld = 0, st = 0;
    int np = 0;
    for (int i = 0; i < n; i++) {
        uint64_t a = t[2 * i], b = t[2 * i + 1];
        if (!a || !b || b < a) continue;
        double us = (b - a) * tick_ns / 1e3;
        const gp_lab *l = &lab[i];
        tot[l->kind != 'R'] += us;
        if (!t0 || a < t0) t0 = a;
        if (b > t1) t1 = b;
        if (l->kind == 'R') {
            np++; ld += l->ld_kb; st += l->st_kb;
            MLOG("[gpuprof] F%llu %4d %8.1f us render pass %llu %ux%u colors %u depth %u loads %u draws %u start %+.1f seg %u load %u KB store %u KB fmt %u/%u", (unsigned long long)frame, i, us, (unsigned long long)l->pass, l->w, l->h, l->nc, l->depth, l->loads, l->draws, (a - t[0]) * tick_ns / 1e3, l->seg, l->ld_kb, l->st_kb, l->cfmt, l->dfmt);
        }
        else MLOG("[gpuprof] F%llu %4d %8.1f us compute / copy after pass %llu start %+.1f seg %u copies %u (uniform updates %u) dispatches %u barriers %u", (unsigned long long)frame, i, us, (unsigned long long)l->pass, (a - t[0]) * tick_ns / 1e3, l->seg, l->draws, l->h, l->w, l->ld_kb);
    }
    span = (t1 - t0) * tick_ns / 1e3;
    MLOG("[gpuprof] frame %llu: render %.2f ms, compute / copy %.2f ms over %d encoders (%d render passes, attachments load %.1f MB store %.1f MB), first start to last end %.2f ms", (unsigned long long)frame, tot[0] / 1e3, tot[1] / 1e3, n, np, ld / 1024.0, st / 1024.0, span / 1e3);
    free(lab); atomic_store(&g_gp_busy, 0);
}
static inline id<MTL4RenderCommandEncoder> enc_r_begin_pass(MTL4RenderPassDescriptor *d) {
    g_cm_enc++;
    cm_enc_mark(2);
    id<MTL4RenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d];
    cm_enc_mark(0);
    if (g_last_end_relaxed && !(g_game_barrier & GAME_BARRIER_FULL) && barrier_game()) {   // the alias flush, when aliases exist, is a consumer barrier before this pass's fragments (m4_end)
        g_bar_skip++;
        MTL4VisibilityOptions vis = resource_vis();
        if (vis & MTL4VisibilityOptionResourceAlias) [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:vis];
    }
    else { [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()]; g_bar_full++; }
    g_game_barrier = 0;
    [e setArgumentTable:g_vt atStages:MTLRenderStageVertex];
    [e setArgumentTable:g_ft atStages:MTLRenderStageFragment];
    gp_begin(e, 'R', d);
    return e;
}
static inline void enc_r_end_pass(id e) {
    if (!barrier_game()) { m4_end(e); return; }
    gp_end(e);
    [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageVertex | MTLStageObject | MTLStageMesh | MTLStageFragment beforeQueueStages:MTLStageFragment | MTLStageDispatch | MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    [e endEncoding];
    g_last_end_relaxed = 1;
}
static inline id<MTL4RenderCommandEncoder> enc_r_begin(MTL4RenderPassDescriptor *d) {
    g_cm_enc++;
    cm_enc_mark(2);
    id<MTL4RenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d];
    cm_enc_mark(0);
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [e setArgumentTable:g_vt atStages:MTLRenderStageVertex];
    [e setArgumentTable:g_ft atStages:MTLRenderStageFragment];
    gp_begin(e, 'R', d);
    return e;
}
static inline void enc_r_end(id e) { m4_end(e); }
static inline void enc_r_pso(__unsafe_unretained id<MTL4RenderCommandEncoder> e, __unsafe_unretained id<MTLRenderPipelineState> p) { [e setRenderPipelineState:p]; }   // the caller holds both
static inline void enc_r_dss(id<MTL4RenderCommandEncoder> e, id<MTLDepthStencilState> s) { [e setDepthStencilState:s]; }
static inline void enc_r_viewport(id<MTL4RenderCommandEncoder> e, MTLViewport v) { [e setViewport:v]; }
static inline void enc_r_scissor(id<MTL4RenderCommandEncoder> e, MTLScissorRect r) { [e setScissorRect:r]; }
static inline void enc_r_cull(id<MTL4RenderCommandEncoder> e, MTLCullMode m) { [e setCullMode:m]; }
static inline void enc_r_winding(id<MTL4RenderCommandEncoder> e, MTLWinding w) { [e setFrontFacingWinding:w]; }
static inline void enc_r_fill(id<MTL4RenderCommandEncoder> e, MTLTriangleFillMode m) { [e setTriangleFillMode:m]; }
static inline void enc_r_depth_bias(id<MTL4RenderCommandEncoder> e, float u, float s, float c) { [e setDepthBias:u slopeScale:s clamp:c]; }
static inline void enc_r_depth_clip(id<MTL4RenderCommandEncoder> e, BOOL v) { [e setDepthClipMode:v ? MTLDepthClipModeClip : MTLDepthClipModeClamp]; }
static inline void enc_r_stencil_ref(id<MTL4RenderCommandEncoder> e, uint32_t f, uint32_t b) { [e setStencilFrontReferenceValue:f backReferenceValue:b]; }
static inline void enc_r_blend_color(id<MTL4RenderCommandEncoder> e, float r, float g, float b, float a) { [e setBlendColorRed:r green:g blue:b alpha:a]; }
static inline void enc_r_visibility(id<MTL4RenderCommandEncoder> e, MTLVisibilityResultMode m, NSUInteger o) { [e setVisibilityResultMode:m offset:o]; }
// No Metal allocation lies below ZA_LOW, so the GPU reads through a base of 0: some chips read 0 with no error, M5s page-fault, and DM_RES_CHECK cannot see it.
// Every address the host hands Metal goes through za_check.
enum { ZA_SHOW = 4, ZA_TEXT = 176, ZA_LOG = 16 };
#define ZA_LOW 0x100000ull
static char g_za[ZA_SHOW][ZA_TEXT], g_za_key[64] = "none";
static uint32_t g_za_n;
static uint64_t g_za_total;
static const void *g_za_sh;
static inline void za_shader(const void *sh, const char *key) {
    if (sh != g_za_sh) { g_za_sh = sh; strlcpy(g_za_key, key, sizeof g_za_key); }
}
// aside: in a command buffer of its own, which keeps no list: printed only
static __attribute__((noinline, cold)) void za_note_(const char *where, long slot, uint64_t gpu, int aside) {
    char t[ZA_TEXT];
    if (aside) snprintf(t, sizeof t, "%s %ld = %#llx (a command buffer of its own)", where, slot, (unsigned long long)gpu);
    else snprintf(t, sizeof t, "%s %ld = %#llx (pass %llu, last game shader %s)", where, slot, (unsigned long long)gpu, (unsigned long long)g_stat_pass, g_za_key);
    if (!aside) { if (g_za_n < ZA_SHOW) memcpy(g_za[g_za_n], t, ZA_TEXT); g_za_n++; }
    uint64_t n = ++g_za_total;
    if (n <= ZA_LOG) MLOG("[RED] zero GPU address #%llu: %s, frame %llu", (unsigned long long)n, t, (unsigned long long)atomic_load(&g_cur_frame));
}
static void za_note(const char *where, long slot, uint64_t gpu) { za_note_(where, slot, gpu, 0); }
static inline void za_check(uint64_t gpu, const char *where, long slot) { if (__builtin_expect(gpu < ZA_LOW, 0)) za_note_(where, slot, gpu, 0); }
static inline void at_addr(id t, uint64_t gpu, NSUInteger i, const char *where, int aside) {
    if (__builtin_expect(gpu < ZA_LOW, 0)) za_note_(where, (long)i, gpu, aside);
    [(id<MTL4ArgumentTable>)t setAddress:gpu atIndex:i];
}
// The vertex table persists across encoders and submissions: every write to it is tracked.
static inline void enc_r_vbuf(id<MTL4RenderCommandEncoder> e, NSUInteger i, uint64_t gpu) {
    (void)e;
    uint32_t bit = 1u << i;
    za_check(gpu, "vertex table slot", (long)i);
    if (g_vbuf_cache && (g_vt_valid & bit) && g_vt_addr[i] == gpu) { g_vbuf_skip++; return; }
    if (rec_on()) rec_table_op(RO_T_ADDR, g_vt, gpu, i); else [g_vt setAddress:gpu atIndex:i];
    g_vt_addr[i] = gpu; g_vt_valid |= bit; g_vbuf_set++;
}
static inline void enc_r_args(id<MTL4RenderCommandEncoder> e, MTLRenderStages s, uint64_t gpu, NSUInteger i) {
    if (s == MTLRenderStageVertex) { enc_r_vbuf(e, i, gpu); return; }
    // a run of mesh draws is drawn before the table changes, with the addresses its draws had
    za_check(gpu, "fragment table slot", (long)i);
    uint32_t bit = 1u << i;
    if (!(g_ft_valid & bit) || g_ft_addr[i] != gpu) { g_ft_addr[i] = gpu; g_ft_valid |= bit; }
    if (rec_on()) rec_table_op(RO_T_ADDR, g_ft, gpu, i); else [g_ft setAddress:gpu atIndex:i];
}
static inline void enc_r_draw(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n) { g_cm_mdraws++; [e drawPrimitives:p vertexStart:v vertexCount:n]; }
static inline void enc_r_draw_instanced(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n, NSUInteger i, NSUInteger b) { g_cm_mdraws++; [e drawPrimitives:p vertexStart:v vertexCount:n instanceCount:i baseInstance:b]; }
static inline void enc_r_draw_indirect(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, uint64_t gpu) { za_check(gpu, "indirect draw record", -1); g_cm_mdraws++; [e drawPrimitives:p indirectBuffer:gpu]; }
static inline void enc_r_draw_indexed(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger n, MTLIndexType t, uint64_t gpu, NSUInteger len, NSUInteger i, NSInteger bv, NSUInteger bi) {
    za_check(gpu, "index buffer", -1);
    g_cm_mdraws++;
    [e drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:gpu indexBufferLength:len instanceCount:i baseVertex:bv baseInstance:bi];
}
static inline void enc_r_draw_indexed_indirect(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, MTLIndexType t, uint64_t gpu, NSUInteger len, uint64_t ind) {
    za_check(gpu, "index buffer", -1);
    za_check(ind, "indexed indirect draw record", -1);
    g_cm_mdraws++;
    [e drawIndexedPrimitives:p indexType:t indexBuffer:gpu indexBufferLength:len indirectBuffer:ind];
}
static void ubd_land_compute(void);
static inline id<MTL4ComputeCommandEncoder> enc_c_begin(MTLComputePassDescriptor *d) {
    g_cm_enc++;
    (void)d;
    ubd_land_compute();
    cm_enc_mark(3);
    id<MTL4ComputeCommandEncoder> e = [g_cb computeCommandEncoder];
    cm_enc_mark(1);
    [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [e setArgumentTable:g_ct];
    gp_begin(e, 'C', nil);
    return e;
}
static inline void enc_c_end(id e) { m4_end(e); }
static inline void enc_c_pso(id e, id<MTLComputePipelineState> p) { [e setComputePipelineState:p]; }
static inline void enc_c_args(id e, uint64_t gpu, NSUInteger i) {
    (void)e;
    za_check(gpu, "compute table slot", (long)i);
    if (rec_on()) rec_table_op(RO_T_ADDR, g_ct, gpu, i); else [g_ct setAddress:gpu atIndex:i];
}
static inline void enc_c_dispatch(id e, MTLSize n, MTLSize t) { [e dispatchThreadgroups:n threadsPerThreadgroup:t]; }
static inline void enc_c_dispatch_indirect(id e, uint64_t ind, MTLSize t) {
    za_check(ind, "indirect dispatch record", -1);
    [(id<MTL4ComputeCommandEncoder>)e dispatchThreadgroupsWithIndirectBuffer:ind threadsPerThreadgroup:t];
}
static inline id enc_b_begin(id cb) {
    if (cb == g_cb) { g_cm_enc++; cm_enc_mark(3); }
    id e = [cb respondsToSelector:@selector(beginCommandBufferWithAllocator:)] ? [cb computeCommandEncoder] : [cb blitCommandEncoder];
    if (cb == g_cb) cm_enc_mark(1);
    return e;
}
static inline void enc_b_end(id e) { m4_end(e); }
static inline void enc_b_copy_buffer(id e, id<MTLBuffer> s, NSUInteger so, id<MTLBuffer> d, NSUInteger doff, NSUInteger n) { [e copyFromBuffer:s sourceOffset:so toBuffer:d destinationOffset:doff size:n]; }
static inline void enc_b_copy_buffer_texture(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o]; }
static inline void enc_b_copy_texture_buffer(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img]; }
static inline void enc_b_copy_buffer_texture_opt(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLBlitOption opt) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o options:opt]; }
static inline void enc_b_copy_texture_buffer_opt(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img, MTLBlitOption opt) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img options:opt]; }
static inline void enc_b_copy_texture(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin so, MTLSize z, id<MTLTexture> d, NSUInteger dl, NSUInteger dv, MTLOrigin doo) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:so sourceSize:z toTexture:d destinationSlice:dl destinationLevel:dv destinationOrigin:doo]; }
static void blit_note(void);
// Metal 4 does not order copies and fills within one encoder, NVN runs copies in order: an operation that conflicts with an earlier one by guest range takes a barrier first.
// A texture operation takes one whenever the encoder already holds an operation.
enum { BW_MAX = 64 };
static uint64_t g_bw_lo[BW_MAX], g_bw_hi[BW_MAX], g_br_lo[BW_MAX], g_br_hi[BW_MAX];
static int g_bw_n, g_br_n, g_blit_ops;
static uint64_t g_blit_barriers;
// What dispatches ordered by the game's barriers wrote and read since the last barrier: copies are ordered after them, dispatches only when the game asked.
enum { DW_MAX = 256 };
static uint64_t g_dw_lo[DW_MAX], g_dw_hi[DW_MAX], g_dr_lo[DW_MAX], g_dr_hi[DW_MAX];
static int g_dw_n, g_dr_n, g_dsp_game_bar;
static uint64_t g_blit_splits;
uint64_t mtl_blit_splits(void) { return g_blit_splits; }
static int enc_open(void);
static int g_blit_ct;   // dispatches share it
static id<MTL4ComputeCommandEncoder> blit_open(void) {
    if (!g_blit) {
        g_blit = enc_b_begin(g_cb);
        [g_blit barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:blit_vis()];
        gp_begin(g_blit, 'C', nil);
        g_bw_n = g_br_n = g_dw_n = g_dr_n = 0; g_blit_ops = 0; g_blit_ct = 0;
    }
    return g_blit;
}
static void ubd_flush(void);
static int g_nubd;
static int g_ubd_landing;
static id<MTL4ComputeCommandEncoder> blit_raw(void) {
    blit_note();
    if (enc_open()) g_blit_splits++;
    rp_end();
    id<MTL4ComputeCommandEncoder> e = blit_open();
    if (g_nubd && !g_ubd_landing) { g_ubd_landing = 1; ubd_flush(); g_ubd_landing = 0; }   // deferred inline updates come before this copy or dispatch in command order
    return e;
}
static void ubd_land(void) {
    if (g_nubd && !g_ubd_landing) {
        rp_end();
        g_ubd_landing = 1;
        ubd_flush();
        g_ubd_landing = 0;
    }
}
static void ubd_land_compute(void) {   // no other encoder may be open then
    if (g_nubd && !g_ubd_landing) {
        ubd_land();
        if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    }
}
static void blit_barrier(void) {
    // MTLStageAll here fails Metal validation for a compute encoder
    [g_blit barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:blit_vis()];
    g_bw_n = g_br_n = g_dw_n = g_dr_n = 0; g_blit_ops = 0;
    g_blit_barriers++; g_gp_bar++;
}
uint64_t mtl_blit_barriers(void) { return g_blit_barriers; }
static int ranges_hit(const uint64_t *lo, const uint64_t *hi, int n, uint64_t a, uint64_t b) {
    for (int i = 0; i < n; i++) if (a < hi[i] && lo[i] < b) return 1;
    return 0;
}
static int bw_hit(uint64_t lo, uint64_t hi) { return ranges_hit(g_bw_lo, g_bw_hi, g_bw_n, lo, hi); }
static int br_hit(uint64_t lo, uint64_t hi) { return ranges_hit(g_br_lo, g_br_hi, g_br_n, lo, hi); }
static int dw_hit(uint64_t lo, uint64_t hi) { return ranges_hit(g_dw_lo, g_dw_hi, g_dw_n, lo, hi); }
static int dr_hit(uint64_t lo, uint64_t hi) { return ranges_hit(g_dr_lo, g_dr_hi, g_dr_n, lo, hi); }
static uint64_t g_wr_epoch;   // bumped by every buffer write outside a render pass: indirect-draw conversions made before it are stale
static uint64_t g_mdx_epoch;
// Writes since the last drain (mdi_wait), which completed every GPU write before it; trusted only while g_wr_epoch has not moved and the list has not overflowed.
enum { DRW_N = 512 };
static uint64_t g_drw_lo[DRW_N], g_drw_hi[DRW_N], g_drain_sub = ~0ull, g_drain_epoch;
static int g_ndrw, g_drw_over;
static void drw_note(uint64_t lo, uint64_t n) {
    if (g_drain_sub != g_sub_serial || g_drw_over) return;
    if (!n || lo > UINT64_MAX - n) { g_drw_over = 1; return; }
    for (int i = g_ndrw - 1; i >= 0 && i >= g_ndrw - 8; i--)
        if (lo <= g_drw_hi[i] && g_drw_lo[i] <= lo + n) { g_drw_lo[i] = MIN(lo, g_drw_lo[i]); g_drw_hi[i] = MAX(lo + n, g_drw_hi[i]); return; }
    if (g_ndrw == DRW_N) { g_drw_over = 1; return; }
    g_drw_lo[g_ndrw] = lo; g_drw_hi[g_ndrw++] = lo + n;
}
static inline void args_dirty_all(void);
// encode_args notes storage writes only when it encodes, not when it reuses an argument buffer
static void drw_drained(void) {
    g_drain_sub = g_sub_serial;
    g_drain_epoch = g_wr_epoch;
    g_ndrw = g_drw_over = 0;
    args_dirty_all();
}
static int drw_valid(void) { return g_drain_sub == g_sub_serial && !g_drw_over && g_wr_epoch == g_drain_epoch; }
static int drw_hits(uint64_t lo, uint64_t n) { return ranges_hit(g_drw_lo, g_drw_hi, g_ndrw, lo, lo + n); }
int nvn_sparse_alias(uint64_t lo, uint64_t hi);
// reads [rlo, rhi) (0, 0 when nothing is read), writes [wlo, whi)
static id<MTL4ComputeCommandEncoder> blit_buf(uint64_t rlo, uint64_t rhi, uint64_t wlo, uint64_t whi) {
    g_wr_epoch++;
    g_blit_alias = 1;   // before blit_raw, so an encoder it opens starts with alias visibility
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    g_blit_alias = 1;   // again: blit_raw may have ended an encoder in between
    // sparse mappings can join unrelated physical runs: ordered with everything
    if (nvn_sparse_alias(rlo, rhi) || nvn_sparse_alias(wlo, whi)) { wlo = 0; whi = ~0ull; }
    if (g_blit_ops && (g_bw_n == BW_MAX || g_br_n == BW_MAX || (rhi > rlo && (bw_hit(rlo, rhi) || dw_hit(rlo, rhi))) ||
                       bw_hit(wlo, whi) || br_hit(wlo, whi) || dw_hit(wlo, whi) || dr_hit(wlo, whi))) blit_barrier();
    g_bw_lo[g_bw_n] = wlo; g_bw_hi[g_bw_n++] = whi; g_blit_ops++; g_gp_copies++;
    if (rhi > rlo) { g_br_lo[g_br_n] = rlo; g_br_hi[g_br_n++] = rhi; }
    return e;
}
static id<MTL4ComputeCommandEncoder> blit(void) {
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    if (g_blit_ops) blit_barrier();
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_br_n = 0; g_blit_ops = 1;   // "everything written": the next operation always gets a barrier
    return e;
}

static void post(post_op o);
enum { POST_BUFFER_COPY = 4 };
static void post_buffer_copy(uint64_t dst, uint64_t n) {
    if (!n) return;
    uint64_t end = dst + n;
    if (g_npost) {
        post_op *last = &g_post[g_npost - 1];
        if (last->kind == POST_BUFFER_COPY && dst <= last->copy_end && last->addr <= end) {
            last->addr = MIN(last->addr, dst);
            last->copy_end = MAX(last->copy_end, end);
            return;
        }
    }
    post((post_op){.kind = POST_BUFFER_COPY, .addr = dst, .copy_end = end});
}
// A game may move texture storage with buffer copies and use textures over the destination later in the submission; the upload batch commits before the main command buffer,
// so the next command after a run of copies commits what was encoded so far (sub_cut) and stamps the copied pages.
static uint64_t g_b2b_cuts, g_b2b_nocut, g_b2b_runs, g_b2b_bytes;
static int copy_cut_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_COPY_CUT");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_COPY_CUT=0: buffer copies are left to the submission's commit, their pages stamped at completion only");
    }
    return v;
}
static void b2b_note(uint64_t dst, uint64_t n) {
    if (!n || !copy_cut_on()) return;
    uint64_t hi = dst + n;
    if (g_b2b_n && dst <= g_b2b_hi[g_b2b_n - 1] && g_b2b_lo[g_b2b_n - 1] <= hi) {
        g_b2b_lo[g_b2b_n - 1] = MIN(g_b2b_lo[g_b2b_n - 1], dst);
        g_b2b_hi[g_b2b_n - 1] = MAX(g_b2b_hi[g_b2b_n - 1], hi);
        return;
    }
    if (g_b2b_n == B2B_MAX) {   // the last range widens: more pages stamped, none missed
        g_b2b_over = 1;
        g_b2b_lo[B2B_MAX - 1] = MIN(g_b2b_lo[B2B_MAX - 1], dst);
        g_b2b_hi[B2B_MAX - 1] = MAX(g_b2b_hi[B2B_MAX - 1], hi);
        return;
    }
    g_b2b_lo[g_b2b_n] = dst; g_b2b_hi[g_b2b_n++] = hi;
}
static void buffer_copy_note(uint64_t dst, uint64_t n) { post_buffer_copy(dst, n); b2b_note(dst, n); }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) {
    rc_copy(src,dst,n);
    if (src != dst) nvn_buffer_write_ranges(dst, n, buffer_copy_note);
    pend_add(dst, n, 2); g_mdx_epoch++;
    void *sb, *db; uint64_t so, dof;
    if (!mtl_addr(src, n, &sb, &so) || !mtl_addr(dst, n, &db, &dof))
        MDIE("buffer copy address is in no memory pool src=%#llx dst=%#llx n=%#llx", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    if (src != dst && src < dst + n && dst < src + n) {   // Metal leaves an overlapping copy undefined
        static int once;
        if (!once++) MLOG("[RED] buffer copy with overlapping source and destination (src %#llx dst %#llx, %llu bytes): the result is undefined", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n);
    }
    enc_b_copy_buffer(blit_buf(src, src + n, dst, dst + n), (__bridge id<MTLBuffer>)sb, so, (__bridge id<MTLBuffer>)db, dof, n);
}
