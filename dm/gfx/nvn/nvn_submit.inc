// Submissions: their start (mtl_sub_begin), command buffers, Metal 4 barriers, the GPU profiler (DM_NVN_GPUPROF), the encoders' calls (enc_r_*, enc_c_*, enc_b_*), the check of low GPU addresses and the order of the copy encoder.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// The submissions of one frame share an allocator: Metal lets an allocator serve command buffers one after another and asks only that all of them are complete before its reset
// (MTL4CommandBuffer.h), which is what its fence, the last one's, says. So the pool holds an allocator per frame in flight, not per submission. Since the queue replays a guest
// submission in pieces (nvn_queue.inc), a game makes many small ones a frame: Ocarina of Time's 28-37 took the pool's 8 about once a frame, and the submit thread waited for the
// GPU (PR #2090). ALLOC_SHARE bounds an allocator's memory while no frame ends (a loading screen): after that many submissions the next takes another
static _Atomic uint64_t g_cur_frame;
static uint64_t g_alloc_frame, g_alloc_shared, g_alloc_shares; static int g_alloc_used;   // the current allocator: the frame it was taken in, the submissions it has served since its reset; submissions that shared one so far
int mtl_wait(uint64_t fence, uint64_t timeout_ns);
static int mtl_wait_at(uint64_t fence, uint64_t timeout_ns, int site);
static void up_flush(void);
static void up_idle_release(void);
// The upload batch belongs only to the submit thread (the thread that called mtl_sub_begin): the batch's encoder cannot be used across threads. Every entry point asserts current thread == submit thread,
// and on mismatch prints the stack and aborts on the spot - so that 'another thread touched the batch' is exposed deterministically on Mac too, not by timing (a game stuck at its main menu: the present thread flushed the submit thread's batch via q_cb).
static pthread_t g_sub_thread; static _Atomic int g_sub_thread_set;
static int on_sub_thread(void) { return atomic_load(&g_sub_thread_set) && pthread_equal(pthread_self(), g_sub_thread); }
static void up_owner_check(const char *where) {
    if (!atomic_load(&g_sub_thread_set) || on_sub_thread()) return;
    char name[64] = ""; pthread_getname_np(pthread_self(), name, sizeof name);
    MLOG("[RED] upload batches belong to the submit thread only: %s called on thread '%s'", where, name);
    void *bt[32]; int n = backtrace(bt, 32); backtrace_symbols_fd(bt, n, 2);
    abort();
}
// Every command buffer created outside the replay / upload batch goes through this. On the submit thread: flush this batch's uploads first, so that commands that then read or write these textures are ordered after the uploads;
// other threads (the present thread etc.) creating a command buffer do not touch the batch: they do not read or write textures uploaded in this batch; commands that need these textures all come after this replay submission (the batch is flushed before it)
// Old-queue command buffers (readback / diagnostics / present; to be deleted once M4-4 and M4-5 are migrated away): uploads are already on the Metal 4 queue, and there is no ordering guarantee between the two queues -
// wait for the upload to complete after flushing it, and only then can the old queue read the upload result (mtl_read_texels waited first and flushed afterwards, the readback beat the upload, 4 failures on t_nvn upload content)
static void up_wait_zero(_Atomic int *v, int site);
static id<MTLCommandBuffer> q_cb(void) { if (on_sub_thread()) { up_flush(); rec_drain(); up_wait_zero(&g_up_pending, WS_OLDQ); } return cl_cb(); }
static post_op *g_post; static int g_npost, g_postcap;
enum { B2B_MAX = 4096 };   // Destinations of the current run of buffer copies (b2b_note, b2b_flush)
static uint64_t g_b2b_lo[B2B_MAX], g_b2b_hi[B2B_MAX]; static int g_b2b_n, g_b2b_over;
// One autorelease pool per submission: submissions run on a guest thread (long-lived, no run loop, never drains the implicit pool), so objects autoreleased in the Metal replay
// (command buffers, encoders, pass descriptors, etc.) are never released unless put in a pool. Without one the host heap grew by about 47 objects a frame on a character select screen.
void *objc_autoreleasePoolPush(void); void objc_autoreleasePoolPop(void *);
static void *g_sub_pool;
static void treg_finish_submission(void);

static uint64_t g_sub_serial;   // Submission index (used by the parallel precompile's 'predict only once per submission')
static int g_pkg_kept;   // a texture written during this submission kept its texture (tex_mtl_): the next submission starts a write generation, as a write would
// The first submission: the prepare step (warm_start), before anything of it is encoded. No GPU work: the submit entry (hle_nvn.c) calls it outside the graphics gate.
// Freeze the guest clock and park guest clock reads across it (reason 3, hle_os.c): the step can stall the render thread for seconds or minutes, and without this a game
// whose main thread bounds its render-thread wait skips hundreds of frames and tears down frame 1's resources while frame 1 is still waiting here (13 Sentinels: Aegis Rim)
void mtl_prepare(void) {
    if (!g_warm_started && prepare_on()) {
        guest_clock_freeze_for(3, 1);
        mtl_init(); shmap_load();
        guest_clock_freeze_for(3, 0);
    }
}
static void sub_cut_begin(void); static void ts_sub_begin(void), ts_boundary(void); static int ts_on(void);
static void tu_tick(void);
static void gmem_commit_patch(void);   // the raw pool table words of the arguments encoded so far follow the pools (gmem_slot_note)
static void treg_sparse_notes(void);
// mtl_sub_commit's ring of completion records, one a fence (post_rec). One record per submission in flight: mtl_sub_begin lets at most ALLOC_MAX allocators of ALLOC_SHARE
// submissions each stand uncompleted, so the submission that reuses a slot comes after the one that last held it has completed and run its writes (post_complete runs every
// record up to a fence before it raises g_done). 128 records, sized for one allocator per submission, wrapped once allocators were shared (PR #2090): Super Mario Party's
// 70-80 submissions a frame overran them two frames behind the feedback (PR #2469)
enum { POST_RING = SUB_FLIGHT_MAX };
static void pass_split_try(void);
static uint64_t g_setrt_n;   // render target changes so far (pass_split_try: splits per NVN pass)
static int g_split_resume;   // the pass was split at this draw: it opens again where the dispatch scan already looked (cs_hoist_try)
static void mem_trim(void);
static void pkg_sweep(void);
static uint64_t spare_trim(int all, unsigned *n_out);
static unsigned g_evict_why[5];   // the last trim's packages (pkg_evict_idle): considered, used within PKG_IDLE_DS, of a kind not freed (sparse, placed, a view), GPU-written, held (pin, sharing table, takeover)
void mtl_sub_begin(void) {
    mtl_init(); g_sub_serial++; g_seg = 0; g_vis_need = 1;   // until its first samples-passed reset or report (vis_need_scan)
    treg_sparse_notes();   // the mappings made before this submission
    if (g_pkg_kept) { g_pkg_kept = 0; nvn_dirty_touch(); } seg_clk_put(g_sclk); g_sclk = NULL;
    sub_cut_begin();
    defer_drain();
    rh_trim();
    tu_tick();   // the render targets drawn over since their last use go, once a second
    spare_trim(0, NULL);   // packaged textures' spares no re-upload took
    mem_trim();   // memory pressure asked for one (mem_pressure_event)
    pkg_sweep();   // small devices: uploaded textures idle for PKG_IDLE_DS
    mtl_prepare();   // done by then in the submit entry; the replay tool (nvn_replay) reaches it here
    warm_claim();
    if (g_cb) MDIE("submit re-entered");
    if (atomic_load(&g_hold_on) && g_fence - g_done >= POST_RING - 16) hold_block(0);   // a hold's fences complete only after it: the post ring (mtl_sub_commit) must not come round to one
    if (!atomic_load(&g_sub_thread_set) || !pthread_equal(g_sub_thread, pthread_self())) nvn_dirty_own_thread();
    g_sub_thread = pthread_self(); atomic_store(&g_sub_thread_set, 1);
    up_idle_release();
    atomic_store(&g_last_commit_error, 0);
    g_sub_pool = objc_autoreleasePoolPush();
    atomic_store(&g_sub_open, g_fence + 1); atomic_thread_fence(memory_order_seq_cst);   // before any pool lookup of this submission (pool_gone_release); only this thread moves g_fence
    g_cb = rec_cb_new();
    // Do not wait for the GPU mid-frame (hot real device: with a fixed rotation of 3, when the GPU cannot keep up every frame blocks here, and the submit thread runs at low frequency after waking, instruction count −18% but CPU time +49%):
    // pick an allocator the GPU has finished with; if all are in use create a new one; only at the cap wait for the oldest one. How far ahead it runs is bounded by the wait where the game acquires the swapchain texture, same as the old interface.
    int alloc_i = -1; uint64_t done = g_done, alloc_frame = atomic_load(&g_cur_frame);
    int alloc_share = g_alloc_used && g_alloc_frame == alloc_frame && g_alloc_shared < ALLOC_SHARE && allocprof_level() != 2;
    if (alloc_share) alloc_i = g_alloc_cur;
    for (int i = 0; alloc_i < 0 && i < g_alloc_n; i++) if (g_alloc_fence[i] <= done) { alloc_i = i; break; }
    if (alloc_i < 0 && g_alloc_n < ALLOC_MAX) { NSError *ae = nil; MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        g_alloc4[g_alloc_n] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae]; if (!g_alloc4[g_alloc_n]) MDIE("failed to create a new MTL4CommandAllocator: %s", ae.localizedDescription.UTF8String);
        alloc_i = g_alloc_n++; g_alloc_grown++; }
    if (alloc_i < 0) { alloc_i = 0; for (int i = 1; i < g_alloc_n; i++) if (g_alloc_fence[i] < g_alloc_fence[alloc_i]) alloc_i = i; g_alloc_waits++; mtl_wait_at(g_alloc_fence[alloc_i], UINT64_MAX, WS_ALLOC); }
    if (allocprof_level() == 2) {   // the GPU is done with this allocator (picked free, or waited for): what it holds is the last submission encoded with it
        if (g_alloc4[alloc_i].allocatedSize) { uint64_t z = g_alloc4[alloc_i].allocatedSize; g_cm_sub_n++; g_cm_sub_sum += z; if (z > g_cm_sub_max) g_cm_sub_max = z;
            int hb = 0; while (hb < 6 && z >= (4ull << 20) << hb) hb++; g_cm_hist[hb]++; }
        { uint64_t ne = g_cm_enc - g_cm_enc0, nd = g_cm_mdraws - g_cm_mdraws0, zz = g_alloc4[g_alloc_cur].allocatedSize;   // the submission just ended was encoded with g_alloc_cur
          if (zz > g_cm_big_z) { g_cm_big_z = zz; g_cm_big_enc = ne; g_cm_big_draws = nd; } g_cm_enc0 = g_cm_enc; g_cm_mdraws0 = g_cm_mdraws; }
        g_alloc4[alloc_i] = [g_dev newCommandAllocatorWithDescriptor:[MTL4CommandAllocatorDescriptor new] error:nil]; g_cm_last = 0;
    }
    g_alloc_cur = alloc_i;
    if (alloc_share) { g_alloc_shared++; g_alloc_shares++; }
    else { [g_alloc4[alloc_i] reset]; g_alloc_frame = alloc_frame; g_alloc_shared = 1; g_alloc_used = 1; }
    [g_cb beginCommandBufferWithAllocator:g_alloc4[alloc_i]];
    ts_sub_begin();
    g_commit_blocks = [NSMutableArray array];
    // The overflow buffer created when the previous submission ran out of slots (see g_vis_n >= VIS_SLOTS): retire it. It used to be just cleared here and stayed in the residency set for good (from d2r-mobile 2eb575877)
    if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis);
    g_npost = 0; g_vis = nil; g_vis_n = 0; g_vis_cur = -1; pend_reset();
    g_b2b_n = 0; g_b2b_over = 0;   // copies made outside mtl_exec (tests) keep only their completion stamp
    g_vis_ring_i = -1;   // its visibility buffer is taken by its first pass that counts samples (vis_take)
    void arena_begin(void); arena_begin();
}

static void rp_end_at(const char *fn, int line);
// Every render pass end names its call site in a pass dump (E <pass> pass ends in <function>:<line>): a pass that ends with no record of its own was found only by reading the code (Splatoon Raiders'
// third boss stage, about 1,500 one-draw passes a frame on its ink map, dm/games/0400380020056000/README.md). The line is in the file that holds the function (this one or an .inc it includes), at the build the log came from
#define rp_end() rp_end_at(__func__, __LINE__)
// Metal 4 conservative barriers (first version): a consumer barrier at the start of an encoder (all prior work → this encoder) and a producer barrier at the end (this encoder and earlier → later encoders).
// Only the one at the start is not enough: t_rt_range 'pass 1 → pass 2 → copy' read back black, and after adding the end barrier it is 19/19. Old-queue encoders (upload / diagnostics / present) do not go through these barriers.
static int g_pass_swrite;   // the open draw pass has shader writes (swrite_note)
static int g_pass_order;    // ordering barriers came in the open draw pass after its draws, none of which wrote (1 primitives, 2 fragments): the next draw that writes is ordered after them first (order_draw)
static int g_pass_late_res; // a draw of the open pass reads memory in a stage after the vertex stage (order_note)
static uint64_t g_order_vbar;   // ordering barriers kept inside the pass as a vertex-to-vertex barrier (prog_swrites)
static void img_fit_upgrade(void); static int g_imfit_npend;   // textures an image slot needs a format view of (img_fit), upgraded before the next draw pass opens
static int g_last_end_relaxed, g_game_barrier;   // the last encoder ended was a draw pass with the relaxed end barrier (nothing else since); the bits of the game's nvnCommandBufferBarrier calls since the last draw pass began
#define GAME_BARRIER_FULL (NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_INDIRECT_DATA_BIT)   // the bits that order earlier work before later vertex or indirect fetch: the next draw pass takes the full barrier (barrier_game)
static uint64_t g_bar_skip, g_bar_full;            // draw passes begun without / with the consumer barrier (DM_NVN_BARRIER=game)
static void gp_end(id e);
// An encoder's end barrier flushes to the device coherence point only. MTL4VisibilityOptionResourceAlias (aliased virtual addresses: overlapping large-pool chunks, image family heaps,
// resource_vis) costs a command allocator about 100 KB per encoder on the end barrier (barrierAfterStages:beforeQueueStages:) and nothing on the start barrier, measured on an M3 Pro:
// 600 render passes took 88.5 MB with it on both, 27.7 MB with it on the start barrier alone. Ocarina of Time's forest submissions of 600-1,400 encoders took 35-85 MB each, and the
// main submission's 8 allocators kept 36-80 MB each. Every encoder this host opens on the main command buffer starts with a barrier carrying resource_vis() (the draw passes, enc_r_begin,
// enc_c_begin, blit_open, and the scaler's own before MetalFX), which waits for all earlier work and then flushes the aliases, so the flush for an encoder's writes still comes before
// any later encoder reads them. A copy encoder that wrote through an aliased buffer address (g_blit_alias) keeps the alias flush on its end barrier as well
// DM_NVN_END_ALIAS=1 puts the alias flush back on every end barrier (ab: alternated with the DM_AB phases, for measuring)
static int sw_mode(const char *name, int dflt); static inline int sw_on(int m);
static inline void m4_end(id e) {
    gp_end(e);
    static int end_alias = -1; if (end_alias < 0) end_alias = sw_mode("DM_NVN_END_ALIAS", 0);
    MTL4VisibilityOptions vis = e == g_blit && g_blit_alias ? blit_vis() : end_alias && sw_on(end_alias) ? resource_vis() : MTL4VisibilityOptionDevice; if (e == g_blit) g_blit_alias = 0;
    if ([e conformsToProtocol:@protocol(MTL4CommandEncoder)]) [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageAll beforeQueueStages:MTLStageAll visibilityOptions:vis];
    [e endEncoding]; g_last_end_relaxed = 0; cm_enc_mark(-1);
}
// DM_NVN_BARRIER=game (default the profile's draw_barriers, full unless it says game): draw passes are ordered by the game's own barriers. A draw pass ends with a barrier from its vertex and fragment stages to the fragment, compute and copy stages of
// later work, so the next pass's vertex work can overlap this pass's fragments (with the full barriers every pass waited for the previous one to finish completely: about 350 passes a frame in Breath of
// the Wild). The next draw pass adds the full consumer barrier when the game's barriers since the last one ordered primitives or indirect data (GAME_BARRIER_FULL: later vertex, task, mesh or indirect
// fetch after earlier work; ORDER_FRAGMENTS and the invalidations ask only for what the relaxed end barrier gives), or when anything other than a draw pass ran in between (compute, copies and uploads
// keep their full barriers). The end barrier flushes to the device only; while aliases exist (resource_vis) the next pass, if it skips the full barrier, flushes them in a start barrier before its
// fragment stage, which costs no allocator memory (see m4_end). Only vertex-stage reads of the previous pass's results without a primitive barrier change order, which NVN does not order either
// DM_NVN_BARRIER=ab... alternates game and full with the DM_AB phases (measurement)
static int sw_mode(const char *name, int dflt); static inline int sw_on(int m);
static int barrier_game(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_BARRIER"); v = e && !strncmp(e, "ab", 2) ? sw_mode("DM_NVN_BARRIER", 0) : e ? !strcmp(e, "game") : DM_GAME_DRAW_BARRIERS_GAME; if (v == 1) MLOG("DM_NVN_BARRIER=game: draw passes are ordered by the game's own barriers (fragment work still has barriers between passes; vertex work may overlap the previous pass's fragments)"); } return sw_on(v); }
void mtl_barrier_stats(uint64_t *skip, uint64_t *full) { *skip = g_bar_skip; *full = g_bar_full; }
uint64_t mtl_cut_count(void);
// DM_NVN_GPUPROF=first frame[-last frame] (diagnostic): GPU time of every encoder of the main command buffers in those frames. A relaxed timestamp is written into a counter heap when an
// encoder begins (before its first draw or dispatch) and when it ends (after its fragment stage); nothing is serialized beyond the barriers the replay already sets, so the frame runs
// as usual. Printed as "[gpuprof]" lines once the frame's last command buffer completes: one per encoder, then a frame line with the render and compute / copy time, the attachment
// traffic and the span from the first start to the last end. dm/tools/gpuprof.py sums them by pass shape.
// DM_NVN_GPUPROF=every:<n> profiles one frame in every n (frames that are multiples of n), each reported on its own: for a device run whose scene cannot be timed to a frame number.
// A render pass line also gives what it loaded and stored of its attachments (KB, the bandwidth ledger's view: DontCare stores are not counted) and the Metal pixel formats of
// colour attachment 0 and of the depth / stencil attachment (0: none)
enum { GP_MAX = 4096 };   // the largest timestamp heap Metal makes
typedef struct { char kind; uint16_t w, h; uint8_t nc, depth, loads; uint32_t draws, seg; uint64_t pass; uint32_t ld_kb, st_kb; uint16_t cfmt, dfmt; } gp_lab;   // seg: the command buffer (segment) of the submission it is in
static id<MTL4CounterHeap> g_gp_heap; static gp_lab g_gp_lab[GP_MAX / 2]; static int g_gp_n, g_gp_state, g_gp_cur = -1; static long g_gp_f0 = -2, g_gp_f1, g_gp_every; static uint64_t g_gp_frame;
static const void *g_gp_e;   // the encoder g_gp_cur belongs to: m4_end also ends the upload / diagnostics / present encoders, which may end while a profiled pass is open (#531)
static _Atomic int g_gp_busy;   // a periodic profile's report has not been printed yet: the heap is still being read
static uint64_t g_bw_load, g_bw_store, g_gp_bw_l0, g_gp_bw_s0;   // the bandwidth ledger (defined with the pass statistics below) and its values at the last render pass end
static uint64_t g_stat_pass, g_stat_draw; static _Atomic uint64_t g_cur_frame; static uint32_t g_seg; static uint32_t g_gp_copies, g_gp_disp, g_gp_ubd, g_gp_bar;   // copies / dispatches encoded, and barriers inside the encoder (for the compute encoders' labels)
static int gp_active(void) {
    if (__builtin_expect(g_gp_f0 == -1, 1)) return 0;
    if (g_gp_f0 == -2) { const char *e = getenv("DM_NVN_GPUPROF");
        if (e && !strncmp(e, "every:", 6)) { g_gp_every = atol(e + 6); g_gp_f0 = g_gp_every > 0 ? 0 : -1; if (g_gp_every > 0) MLOG("DM_NVN_GPUPROF: one frame in every %ld profiled", g_gp_every); }
        else { g_gp_f0 = e ? atol(e) : -1; g_gp_f1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : g_gp_f0; }
        if (g_gp_f0 < 0) return 0; }
    long f = (long)atomic_load(&g_cur_frame);
    if (g_gp_every > 0) return f > 0 && f % g_gp_every == 0;
    return f >= g_gp_f0 && f <= g_gp_f1;
}
static void gp_report(int n, gp_lab *lab, uint64_t frame);
static void gp_periodic_finish(void) {   // on the submit thread, before the next frame's first encoder can touch the heap
    if (!g_gp_every || g_gp_state != 1 || g_gp_frame == atomic_load(&g_cur_frame)) return;
    int n = g_gp_n; gp_lab *lab = malloc(sizeof *lab * (n ? n : 1)); memcpy(lab, g_gp_lab, sizeof *lab * n); uint64_t frame = g_gp_frame;
    g_gp_state = 2; atomic_store(&g_gp_busy, 1);
    static MTLSharedEventListener *listener; if (!listener) listener = [MTLSharedEventListener new];
    // g_fence covers all submissions of the sampled frame. No GPU wait or extra command buffer is needed, including for every:1.
    [g_ev notifyListener:listener atValue:g_fence block:^(id<MTLSharedEvent> event, uint64_t value) { (void)event; (void)value; gp_report(n, lab, frame); }];
}
static id<MTL4CounterHeap> g_ts_heap; static int g_ts_pend = -1;   // ts_on; pend: the boundary the next encoder writes (ts_stamp)
static void gp_begin(id e, char kind, MTL4RenderPassDescriptor *d) {   // right after an encoder of the main command buffer is made
    if (g_ts_pend >= 0) { NSUInteger slot = (NSUInteger)g_ts_pend; g_ts_pend = -1;
        if (kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageVertex intoHeap:g_ts_heap atIndex:slot];
        else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_ts_heap atIndex:slot]; }
    gp_periodic_finish();
    if (g_gp_every > 0 && g_gp_state == 2 && !atomic_load(&g_gp_busy)) g_gp_state = 0;   // the previous periodic profile has been reported
    if (!gp_active() || g_gp_state == 2) return;
    if (!g_gp_heap) { MTL4CounterHeapDescriptor *hd = [MTL4CounterHeapDescriptor new]; hd.type = MTL4CounterHeapTypeTimestamp; hd.count = GP_MAX; NSError *er = nil;
        g_gp_heap = [g_dev newCounterHeapWithDescriptor:hd error:&er]; if (!g_gp_heap) { MLOG("[RED] GPUPROF: no counter heap: %s", er.description.UTF8String); g_gp_f0 = -1; return; } }
    if (!g_gp_state) { g_gp_state = 1; g_gp_n = 0; g_gp_frame = atomic_load(&g_cur_frame); [g_gp_heap invalidateCounterRange:NSMakeRange(0, GP_MAX)]; }
    if (2 * g_gp_n + 2 > GP_MAX) return;
    gp_lab l = {kind, 0, 0, 0, 0, 0, kind == 'R' ? (uint32_t)g_stat_draw : g_gp_copies, g_seg, g_stat_pass, 0, 0, 0, 0}; if (kind != 'R') { l.w = (uint16_t)g_gp_disp; l.h = (uint16_t)g_gp_ubd; l.ld_kb = (uint32_t)g_gp_bar; }
    if (d) { for (int i = 0; i < 8; i++) { MTLRenderPassColorAttachmentDescriptor *c = d.colorAttachments[i]; if (!c.texture) continue;
            if (!l.nc) l.cfmt = (uint16_t)c.texture.pixelFormat; l.nc++; l.w = (uint16_t)MAX(c.texture.width >> c.level, 1); l.h = (uint16_t)MAX(c.texture.height >> c.level, 1); l.loads += c.loadAction == MTLLoadActionLoad; }
        MTLRenderPassDepthAttachmentDescriptor *z = d.depthAttachment;
        if (z.texture || d.stencilAttachment.texture) l.dfmt = (uint16_t)(z.texture ? z.texture : d.stencilAttachment.texture).pixelFormat;
        if (z.texture) { l.depth = 1; l.w = (uint16_t)MAX(z.texture.width >> z.level, 1); l.h = (uint16_t)MAX(z.texture.height >> z.level, 1); l.loads += z.loadAction == MTLLoadActionLoad; } }
    g_gp_lab[g_gp_n] = l; g_gp_cur = g_gp_n; g_gp_e = (__bridge const void *)e;
    if (kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageVertex intoHeap:g_gp_heap atIndex:2 * g_gp_n];
    else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_gp_heap atIndex:2 * g_gp_n];
    g_gp_n++;
}
static void gp_end(id e) {   // right before that encoder ends
    if (g_gp_cur < 0 || (__bridge const void *)e != g_gp_e) return;   // another queue's encoder: the open pass keeps its label and gets its end stamp from its own end
    int k = g_gp_cur; g_gp_cur = -1; g_gp_e = NULL;
    if (g_gp_lab[k].kind == 'R') { g_gp_lab[k].ld_kb = (uint32_t)((g_bw_load - g_gp_bw_l0) >> 10); g_gp_lab[k].st_kb = (uint32_t)((g_bw_store - g_gp_bw_s0) >> 10); }   // since the previous pass end (rp_end); sd_finish has set the stores
    if (g_gp_lab[k].kind == 'R') g_gp_lab[k].draws = (uint32_t)g_stat_draw - g_gp_lab[k].draws; else { g_gp_lab[k].draws = g_gp_copies - g_gp_lab[k].draws; g_gp_lab[k].w = (uint16_t)(g_gp_disp - g_gp_lab[k].w); g_gp_lab[k].h = (uint16_t)(g_gp_ubd - g_gp_lab[k].h); g_gp_lab[k].ld_kb = g_gp_bar - g_gp_lab[k].ld_kb; }
    if (g_gp_lab[k].kind == 'R') [(id<MTL4RenderCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed afterStage:MTLRenderStageFragment intoHeap:g_gp_heap atIndex:2 * k + 1];
    else [(id<MTL4ComputeCommandEncoder>)e writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_gp_heap atIndex:2 * k + 1];
}
static void gp_report(int n, gp_lab *lab, uint64_t frame) {   // the frame's last command buffer has completed
    NSData *r = [g_gp_heap resolveCounterRange:NSMakeRange(0, 2 * n)]; const uint64_t *t = r.bytes; if (!t) { free(lab); atomic_store(&g_gp_busy, 0); return; }
    double tick_ns = 1e9 / (double)[g_dev queryTimestampFrequency], tot[2] = {0}, span = 0; uint64_t t0 = 0, t1 = 0, ld = 0, st = 0; int np = 0;
    for (int i = 0; i < n; i++) { uint64_t a = t[2 * i], b = t[2 * i + 1]; if (!a || !b || b < a) continue;
        double us = (b - a) * tick_ns / 1e3; const gp_lab *l = &lab[i]; tot[l->kind != 'R'] += us;
        if (!t0 || a < t0) t0 = a; if (b > t1) t1 = b;
        if (l->kind == 'R') { np++; ld += l->ld_kb; st += l->st_kb;
            MLOG("[gpuprof] F%llu %4d %8.1f us render pass %llu %ux%u colors %u depth %u loads %u draws %u start %+.1f seg %u load %u KB store %u KB fmt %u/%u", (unsigned long long)frame, i, us, (unsigned long long)l->pass, l->w, l->h, l->nc, l->depth, l->loads, l->draws, (a - t[0]) * tick_ns / 1e3, l->seg, l->ld_kb, l->st_kb, l->cfmt, l->dfmt); }
        else MLOG("[gpuprof] F%llu %4d %8.1f us compute / copy after pass %llu start %+.1f seg %u copies %u (uniform updates %u) dispatches %u barriers %u", (unsigned long long)frame, i, us, (unsigned long long)l->pass, (a - t[0]) * tick_ns / 1e3, l->seg, l->draws, l->h, l->w, l->ld_kb); }
    span = (t1 - t0) * tick_ns / 1e3;
    MLOG("[gpuprof] frame %llu: render %.2f ms, compute / copy %.2f ms over %d encoders (%d render passes, attachments load %.1f MB store %.1f MB), first start to last end %.2f ms", (unsigned long long)frame, tot[0] / 1e3, tot[1] / 1e3, n, np, ld / 1024.0, st / 1024.0, span / 1e3);
    free(lab); atomic_store(&g_gp_busy, 0);
}
static inline id<MTL4RenderCommandEncoder> enc_r_begin_pass(MTL4RenderPassDescriptor *d) { g_cm_enc++; cm_enc_mark(2);   // the draw passes (rp_begin_ex); see barrier_game
    id<MTL4RenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d]; cm_enc_mark(0);
    if (g_last_end_relaxed && !(g_game_barrier & GAME_BARRIER_FULL) && barrier_game()) { g_bar_skip++;   // ordered by the relaxed end barrier; the alias flush, when aliases exist, is a consumer barrier before this pass's fragments (see m4_end)
        MTL4VisibilityOptions vis = resource_vis(); if (vis & MTL4VisibilityOptionResourceAlias) [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageFragment visibilityOptions:vis]; }
    else { [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()]; g_bar_full++; }
    g_game_barrier = 0;
    [e setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [e setArgumentTable:g_ft atStages:MTLRenderStageFragment]; gp_begin(e, 'R', d); return e;
}
static inline void enc_r_end_pass(id e) {
    if (!barrier_game()) { m4_end(e); return; }
    gp_end(e); [(id<MTL4CommandEncoder>)e barrierAfterStages:MTLStageVertex | MTLStageObject | MTLStageMesh | MTLStageFragment beforeQueueStages:MTLStageFragment | MTLStageDispatch | MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
    [e endEncoding]; g_last_end_relaxed = 1;
}
static inline id<MTL4RenderCommandEncoder> enc_r_begin(MTL4RenderPassDescriptor *d) { g_cm_enc++; cm_enc_mark(2); id<MTL4RenderCommandEncoder> e = [g_cb renderCommandEncoderWithDescriptor:d]; cm_enc_mark(0); [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()]; [e setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [e setArgumentTable:g_ft atStages:MTLRenderStageFragment]; gp_begin(e, 'R', d); return e; }
static inline void enc_r_end(id e) { m4_end(e); }
static inline void enc_r_pso(id<MTL4RenderCommandEncoder> e, id<MTLRenderPipelineState> p) { [e setRenderPipelineState:p]; }
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
// A GPU address below ZA_LOW handed to Metal by the host: an argument table's slot, an index buffer, an indirect record, an address slot of a shader's argument block (encode_args).
// No Metal allocation lies that low, so the GPU reads through a base of 0. The M4 Pro and the M6 read 0 there with no error (dm/gfx/tests/t_gpu_fault_kinds), but the M5s of
// issue #1289 page-faulted at GPU addresses 0 and 0x40, and DM_RES_CHECK cannot see it, as no object is bound. The first ZA_LOG are printed as they come; each command buffer
// keeps its first ZA_SHOW, taken with its shader list (cbs_take), and a failed one prints them, or none (cbs_report). Every address goes through za_check: the game's tables in the
// enc_* functions and bind_args, the host passes' own tables in at_addr. Not seen: a table slot a shader reads that nothing ever set (tables are made once and keep what was set;
// the game's shader libraries carry no reflection to say which slots a function reads) and an address a shader computes
enum { ZA_SHOW = 4, ZA_TEXT = 176, ZA_LOG = 16 };
#define ZA_LOW 0x100000ull
static char g_za[ZA_SHOW][ZA_TEXT], g_za_key[64] = "none"; static uint32_t g_za_n; static uint64_t g_za_total; static const void *g_za_sh;
static inline void za_shader(const void *sh, const char *key) { if (sh != g_za_sh) { g_za_sh = sh; strlcpy(g_za_key, key, sizeof g_za_key); } }   // the last game shader encode_args encoded
// aside: in a command buffer of its own (uploads, presents), which keeps no list: printed only
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
// A host pass's own argument table (t: an MTL4ArgumentTable, or its recording stand-in): every address the host puts in one goes through here
static inline void at_addr(id t, uint64_t gpu, NSUInteger i, const char *where, int aside) { if (__builtin_expect(gpu < ZA_LOW, 0)) za_note_(where, (long)i, gpu, aside); [(id<MTL4ArgumentTable>)t setAddress:gpu atIndex:i]; }
// The table persists across encoders and submissions. Track every write to it, including converted streams
// and shader arguments; helper passes with their own tables do not change this state.
static inline void enc_r_vbuf(id<MTL4RenderCommandEncoder> e, NSUInteger i, uint64_t gpu) {
    (void)e; uint32_t bit = 1u << i; za_check(gpu, "vertex table slot", (long)i);
    if (g_vbuf_cache && (g_vt_valid & bit) && g_vt_addr[i] == gpu) { g_vbuf_skip++; return; }
    if (rec_on()) rec_table_op(RO_T_ADDR, g_vt, gpu, i); else [g_vt setAddress:gpu atIndex:i];
    g_vt_addr[i] = gpu; g_vt_valid |= bit; g_vbuf_set++;
}
static inline void enc_r_args(id<MTL4RenderCommandEncoder> e, MTLRenderStages s, uint64_t gpu, NSUInteger i) {
    if (s == MTLRenderStageVertex) { enc_r_vbuf(e, i, gpu); return; }
    // A run of mesh draws is drawn with the fragment table as it is when the run is drawn: the run is drawn before the table changes, with the addresses its draws had
    za_check(gpu, "fragment table slot", (long)i);
    uint32_t bit = 1u << i; if (!(g_ft_valid & bit) || g_ft_addr[i] != gpu) { g_ft_addr[i] = gpu; g_ft_valid |= bit; }
    if (rec_on()) rec_table_op(RO_T_ADDR, g_ft, gpu, i); else [g_ft setAddress:gpu atIndex:i];
}
static inline void enc_r_draw(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n) { g_cm_mdraws++; [e drawPrimitives:p vertexStart:v vertexCount:n]; }
static inline void enc_r_draw_instanced(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger v, NSUInteger n, NSUInteger i, NSUInteger b) { g_cm_mdraws++; [e drawPrimitives:p vertexStart:v vertexCount:n instanceCount:i baseInstance:b]; }
static inline void enc_r_draw_indirect(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, uint64_t gpu) { za_check(gpu, "indirect draw record", -1); g_cm_mdraws++; [e drawPrimitives:p indirectBuffer:gpu]; }
static inline void enc_r_draw_indexed(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, NSUInteger n, MTLIndexType t, uint64_t gpu, NSUInteger len, NSUInteger i, NSInteger bv, NSUInteger bi) { za_check(gpu, "index buffer", -1); g_cm_mdraws++; [e drawIndexedPrimitives:p indexCount:n indexType:t indexBuffer:gpu indexBufferLength:len instanceCount:i baseVertex:bv baseInstance:bi]; }
static inline void enc_r_draw_indexed_indirect(id<MTL4RenderCommandEncoder> e, MTLPrimitiveType p, MTLIndexType t, uint64_t gpu, NSUInteger len, uint64_t ind) { za_check(gpu, "index buffer", -1); za_check(ind, "indexed indirect draw record", -1); g_cm_mdraws++; [e drawIndexedPrimitives:p indexType:t indexBuffer:gpu indexBufferLength:len indirectBuffer:ind]; }
static void ubd_land_compute(void);
static inline id<MTL4ComputeCommandEncoder> enc_c_begin(MTLComputePassDescriptor *d) { g_cm_enc++; (void)d; ubd_land_compute(); cm_enc_mark(3); id<MTL4ComputeCommandEncoder> e = [g_cb computeCommandEncoder]; cm_enc_mark(1); [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()]; [e setArgumentTable:g_ct]; gp_begin(e, 'C', nil); return e; }
static inline void enc_c_end(id e) { m4_end(e); }
static inline void enc_c_pso(id e, id<MTLComputePipelineState> p) { [e setComputePipelineState:p]; }
static inline void enc_c_args(id e, uint64_t gpu, NSUInteger i) { (void)e; za_check(gpu, "compute table slot", (long)i); if (rec_on()) rec_table_op(RO_T_ADDR, g_ct, gpu, i); else [g_ct setAddress:gpu atIndex:i]; }
static inline void enc_c_dispatch(id e, MTLSize n, MTLSize t) { [e dispatchThreadgroups:n threadsPerThreadgroup:t]; }
static inline void enc_c_dispatch_indirect(id e, uint64_t ind, MTLSize t) { za_check(ind, "indirect dispatch record", -1); [(id<MTL4ComputeCommandEncoder>)e dispatchThreadgroupsWithIndirectBuffer:ind threadsPerThreadgroup:t]; }
static inline id enc_b_begin(id cb) {
    if (cb == g_cb) { g_cm_enc++; cm_enc_mark(3); }
    id e = [cb respondsToSelector:@selector(beginCommandBufferWithAllocator:)] ? [cb computeCommandEncoder] : [cb blitCommandEncoder];
    if (cb == g_cb) cm_enc_mark(1);
    return e; }
static inline void enc_b_end(id e) { m4_end(e); }
static inline void enc_b_copy_buffer(id e, id<MTLBuffer> s, NSUInteger so, id<MTLBuffer> d, NSUInteger doff, NSUInteger n) { [e copyFromBuffer:s sourceOffset:so toBuffer:d destinationOffset:doff size:n]; }
static inline void enc_b_copy_buffer_texture(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o]; }
static inline void enc_b_copy_texture_buffer(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img]; }
// The same with a blit option: one plane of a combined depth / stencil texture (MTLBlitOptionDepthFromDepthStencil)
static inline void enc_b_copy_buffer_texture_opt(id e, id<MTLBuffer> s, NSUInteger so, NSUInteger row, NSUInteger img, MTLSize z, id<MTLTexture> d, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLBlitOption opt) { [e copyFromBuffer:s sourceOffset:so sourceBytesPerRow:row sourceBytesPerImage:img sourceSize:z toTexture:d destinationSlice:sl destinationLevel:lv destinationOrigin:o options:opt]; }
static inline void enc_b_copy_texture_buffer_opt(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin o, MTLSize z, id<MTLBuffer> d, NSUInteger doff, NSUInteger row, NSUInteger img, MTLBlitOption opt) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:o sourceSize:z toBuffer:d destinationOffset:doff destinationBytesPerRow:row destinationBytesPerImage:img options:opt]; }
static inline void enc_b_copy_texture(id e, id<MTLTexture> s, NSUInteger sl, NSUInteger lv, MTLOrigin so, MTLSize z, id<MTLTexture> d, NSUInteger dl, NSUInteger dv, MTLOrigin doo) { [e copyFromTexture:s sourceSlice:sl sourceLevel:lv sourceOrigin:so sourceSize:z toTexture:d destinationSlice:dl destinationLevel:dv destinationOrigin:doo]; }
static void blit_note(void);   // Per-pass dump: a copy / buffer update ended the render pass (defined after PD)
// Ordering inside the copy encoder: Metal 4 does not order copies / fills within one encoder (no automatic hazard tracking); an operation reading or writing a range an earlier one wrote needs an intra-encoder barrier.
// Without it, SMBW's two updates per frame to the same uniform slot ("write 4 bytes −1, then 16 bytes of coefficients") occasionally landed in reverse order; the fountain water wave simulation took −1 as its self coefficient, diverged, and the water vertices were pushed to the top of the screen.
// Buffer operations check conflicts by guest address range: a barrier when an operation reads a range written earlier, or writes a range read or written earlier; texture operations get
// one whenever the encoder already holds an operation. Reads were not tracked at first, so a copy writing bytes an earlier copy reads could run first (seen in Cuphead while its pool
// allocator gave out overlapping blocks, see hle_nvn.c zcull_size; NVN runs copies in order for any game)
enum { BW_MAX = 64 };
static uint64_t g_bw_lo[BW_MAX], g_bw_hi[BW_MAX], g_br_lo[BW_MAX], g_br_hi[BW_MAX]; static int g_bw_n, g_br_n, g_blit_ops; static uint64_t g_blit_barriers;
enum { DW_MAX = 256 };
static uint64_t g_dw_lo[DW_MAX], g_dw_hi[DW_MAX], g_dr_lo[DW_MAX], g_dr_hi[DW_MAX]; static int g_dw_n, g_dr_n, g_dsp_game_bar;   // what dispatches ordered by the game's barriers wrote and read since the last barrier (dispatch_enc_cs), apart from the copies' sets: copies are ordered after them, dispatches only when the game asked (g_dsp_game_bar). Emptied by every barrier and by a new copy encoder; empty with DM_NVN_DISPATCH_BARRIER=full
static uint64_t g_blit_splits;   // copies / buffer updates / fills that ended an open draw pass (the residency set line; a pass ended this way stores and the next one reloads its attachments)
uint64_t mtl_blit_splits(void) { return g_blit_splits; }
static int enc_open(void);
static int g_blit_ct;   // the open copy encoder has the compute argument table set (dispatches share it, dispatch_enc)
static id<MTL4ComputeCommandEncoder> blit_open(void) { if (!g_blit) { g_blit = enc_b_begin(g_cb); [g_blit barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:blit_vis()]; gp_begin(g_blit, 'C', nil); g_bw_n = g_br_n = g_dw_n = g_dr_n = 0; g_blit_ops = 0; g_blit_ct = 0; } return g_blit; }
static void ubd_flush(void); static int g_nubd;   // the inline updates deferred (see mtl_sub_update)
static int g_ubd_landing;
static id<MTL4ComputeCommandEncoder> blit_raw(void) {
    blit_note(); if (enc_open()) g_blit_splits++; rp_end(); id<MTL4ComputeCommandEncoder> e = blit_open();
    if (g_nubd && !g_ubd_landing) { g_ubd_landing = 1; ubd_flush(); g_ubd_landing = 0; }   // deferred inline updates come before this copy or dispatch in command order
    return e;
}
// Land the deferred inline updates before other GPU work outside the copy encoder (helper compute passes, the end of the submission, a GPU wait)
static void ubd_land(void) { if (g_nubd && !g_ubd_landing) { rp_end(); g_ubd_landing = 1; ubd_flush(); g_ubd_landing = 0; } }
static void ubd_land_compute(void) { if (g_nubd && !g_ubd_landing) { ubd_land(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; } } }   // before a compute encoder of its own (no other encoder may be open then)
static void blit_barrier(void) {
    // Only the stages a compute encoder has: MTLStageAll here fails Metal validation (macOS 27: "must be a valid combination of MTLStages for the current encoder type")
    [g_blit barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:blit_vis()];
    g_bw_n = g_br_n = g_dw_n = g_dr_n = 0; g_blit_ops = 0; g_blit_barriers++; g_gp_bar++;
}
uint64_t mtl_blit_barriers(void) { return g_blit_barriers; }
static int bw_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_bw_n; i++) if (lo < g_bw_hi[i] && g_bw_lo[i] < hi) return 1; return 0; }
static int br_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_br_n; i++) if (lo < g_br_hi[i] && g_br_lo[i] < hi) return 1; return 0; }
static int dw_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_dw_n; i++) if (lo < g_dw_hi[i] && g_dw_lo[i] < hi) return 1; return 0; }
static int dr_hit(uint64_t lo, uint64_t hi) { for (int i = 0; i < g_dr_n; i++) if (lo < g_dr_hi[i] && g_dr_lo[i] < hi) return 1; return 0; }
// buffer → buffer / fill: reads [rlo, rhi) (0, 0 when nothing is read), writes [wlo, whi)
static uint64_t g_wr_epoch;   // Bumped by every buffer write outside a render pass (copy / update / fill, texture → buffer, dispatch): indirect-draw conversions made before it are stale (ixc_run)
static uint64_t g_mdx_epoch;  // Bumped by GPU writes that can change an indirect-count multi-draw's records or count (copy, fill, texture → buffer, dispatch; see mdx_run)
// Writes since the last drain. A drain (mdi_wait: commit, then wait for the GPU) completes every GPU write encoded so far, so until the submission ends a CPU read of guest memory is
// stale only when a GPU write encoded after the drain overlaps it. The submission-wide sets (pend, sbw) cannot tell before from after, so each later write is also kept here: storage
// writes and copies, updates and fills (sbw_note, pend_add). The list is trusted only while g_wr_epoch has not moved since the drain and the list has not overflowed: otherwise
// readers go back to the submission-wide sets
enum { DRW_N = 512 };
static uint64_t g_drw_lo[DRW_N], g_drw_hi[DRW_N], g_drain_sub = ~0ull, g_drain_epoch; static int g_ndrw, g_drw_over;
static void drw_note(uint64_t lo, uint64_t n) {
    if (g_drain_sub != g_sub_serial || g_drw_over) return;
    if (!n || lo > UINT64_MAX - n) { g_drw_over = 1; return; }
    for (int i = g_ndrw - 1; i >= 0 && i >= g_ndrw - 8; i--) if (lo <= g_drw_hi[i] && g_drw_lo[i] <= lo + n) { g_drw_lo[i] = MIN(lo, g_drw_lo[i]); g_drw_hi[i] = MAX(lo + n, g_drw_hi[i]); return; }
    if (g_ndrw == DRW_N) { g_drw_over = 1; return; }
    g_drw_lo[g_ndrw] = lo; g_drw_hi[g_ndrw++] = lo + n;
}
static inline void args_dirty_all(void);
// Bindings re-encode after the drain (encode_args notes their storage writes only when it encodes, not when it reuses an argument buffer)
static void drw_drained(void) { g_drain_sub = g_sub_serial; g_drain_epoch = g_wr_epoch; g_ndrw = g_drw_over = 0; args_dirty_all(); }
static int drw_valid(void) { return g_drain_sub == g_sub_serial && !g_drw_over && g_wr_epoch == g_drain_epoch; }
static int drw_hits(uint64_t lo, uint64_t n) { for (int i = 0; i < g_ndrw; i++) if (lo < g_drw_hi[i] && g_drw_lo[i] < lo + n) return 1; return 0; }
int nvn_sparse_alias(uint64_t lo, uint64_t hi);   // hle_nvn.c: a sparse guest range may alias another pool at a different address
static id<MTL4ComputeCommandEncoder> blit_buf(uint64_t rlo, uint64_t rhi, uint64_t wlo, uint64_t whi) {
    g_wr_epoch++;
    g_blit_alias = 1;   // before blit_raw, so an encoder it opens starts with alias visibility
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    g_blit_alias = 1;   // again: blit_raw may have ended an encoder in between
    // Sparse mappings can join unrelated physical runs, so conservatively order
    // their operations with everything before and after. Ordinary pools keep the
    // range-based batching, including separate Metal buffers over one CPU range.
    if (nvn_sparse_alias(rlo, rhi) || nvn_sparse_alias(wlo, whi)) { wlo = 0; whi = ~0ull; }
    if (g_blit_ops && (g_bw_n == BW_MAX || g_br_n == BW_MAX || (rhi > rlo && (bw_hit(rlo, rhi) || dw_hit(rlo, rhi))) || bw_hit(wlo, whi) || br_hit(wlo, whi) || dw_hit(wlo, whi) || dr_hit(wlo, whi))) blit_barrier();
    g_bw_lo[g_bw_n] = wlo; g_bw_hi[g_bw_n++] = whi; g_blit_ops++; g_gp_copies++;
    if (rhi > rlo) { g_br_lo[g_br_n] = rlo; g_br_hi[g_br_n++] = rhi; }
    return e;
}
// texture copies / readbacks: barrier if anything came before, and later operations must wait for it too
static id<MTL4ComputeCommandEncoder> blit(void) {
    id<MTL4ComputeCommandEncoder> e = blit_raw();
    if (g_blit_ops) blit_barrier();
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_br_n = 0; g_blit_ops = 1;   // record as "everything written": the next operation always gets a barrier
    return e;
}

static void post(post_op o);
enum { POST_BUFFER_COPY = 4 };
static void post_buffer_copy(uint64_t dst, uint64_t n) {
    if (!n) return;
    // Pool compaction copies successive pieces down or up. Publish each contiguous destination once.
    uint64_t end = dst + n;
    if (g_npost) {
        post_op *last = &g_post[g_npost - 1];
        if (last->kind == POST_BUFFER_COPY && dst <= last->copy_end && last->addr <= end) { last->addr = MIN(last->addr, dst); last->copy_end = MAX(last->copy_end, end); return; }
    }
    post((post_op){.kind = POST_BUFFER_COPY, .addr = dst, .copy_end = end});
}
// Buffer copies committed before the commands after them (DM_NVN_COPY_CUT, on by default; 0 = off). Skywalker Saga moves texture storage with buffer copies (pool compaction)
// and uses textures over the destination later in the same submission. A texture taken from its storage is uploaded by the upload batch, which sub_cut and the submission's end
// commit before the main command buffer that holds the copy, and the dirty tracker learns of the copy only at its completion (post_run): a new texture came out as what the pool
// held there before, and one already uploaded was kept, for a frame or more. Now the next command after a run of copies commits the command buffer encoded so far (sub_cut, as
// after a buffer → texture copy) and stamps the copied pages, so a later lookup uploads again from a batch that runs after the copy. At the submission's end the pages are only stamped
static uint64_t g_b2b_cuts, g_b2b_nocut, g_b2b_runs, g_b2b_bytes;
static int copy_cut_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_COPY_CUT"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_COPY_CUT=0: buffer copies are left to the submission's commit, their pages stamped at completion only"); } return v; }
static void b2b_note(uint64_t dst, uint64_t n) {
    if (!n || !copy_cut_on()) return;
    uint64_t hi = dst + n;
    if (g_b2b_n && dst <= g_b2b_hi[g_b2b_n - 1] && g_b2b_lo[g_b2b_n - 1] <= hi) { g_b2b_lo[g_b2b_n - 1] = MIN(g_b2b_lo[g_b2b_n - 1], dst); g_b2b_hi[g_b2b_n - 1] = MAX(g_b2b_hi[g_b2b_n - 1], hi); return; }
    if (g_b2b_n == B2B_MAX) { g_b2b_over = 1; g_b2b_lo[B2B_MAX - 1] = MIN(g_b2b_lo[B2B_MAX - 1], dst); g_b2b_hi[B2B_MAX - 1] = MAX(g_b2b_hi[B2B_MAX - 1], hi); return; }   // the last range widens: more pages stamped, none missed
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
    if (src != dst && src < dst + n && dst < src + n) { static int once;   // Metal leaves an overlapping copy undefined; seen only in Cuphead while its pool allocator gave out overlapping blocks (hle_nvn.c zcull_size)
        if (!once++) MLOG("[RED] buffer copy with overlapping source and destination (src %#llx dst %#llx, %llu bytes): the result is undefined", (unsigned long long)src, (unsigned long long)dst, (unsigned long long)n); }
    enc_b_copy_buffer(blit_buf(src, src + n, dst, dst + n), (__bridge id<MTLBuffer>)sb, so, (__bridge id<MTLBuffer>)db, dof, n);
}
