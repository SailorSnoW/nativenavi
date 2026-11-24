// Compute: pipelines by shader, the prefetch of libraries, dispatch encoders and barriers, GPU containment, kernels in two passes, dispatch.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// Compute pipelines by shader and whether the pipeline is without the border corrections: a dispatch takes that function unless a sampler it binds needs them (border_need, cps_new_b),
// as a draw does, and the prewarm builds the common one
typedef struct { const void *cs; uintptr_t off; } cps_key;
static nvn_hmap g_cps = {.klen = sizeof(cps_key)};
// shader_load can return distinct objects for a prefetch and a warm task. The library key is
// immutable during a run; the selected entry (including entry_z) distinguishes depth variants.
// Both loaders use the library's default function constants. Never equate unnamed test/stub shaders.
static int warm_mesh_shader_equal(NvShader *a, NvShader *b) {
    if (a == b) return 1;
    return a && b && a->fn && b->fn && a->key[0] && !strcmp(a->key, b->key) &&
           a->stage == b->stage && a->ventry.length && [a->ventry isEqualToString:b->ventry] &&
           [a->fn.name isEqualToString:b->fn.name] && a->mesh_kind == b->mesh_kind &&
           a->mesh_threads == b->mesh_threads && a->mesh_payload == b->mesh_payload && a->mesh_notask == b->mesh_notask;
}
static void warm_claim(void) {
    if (!atomic_exchange_explicit(&g_warm_ready, 0, memory_order_acq_rel)) return;
    os_unfair_lock_lock(&g_warm_lock);
    NSArray<WarmItem *> *items = [g_warm_items copy];
    [g_warm_items removeAllObjects];
    os_unfair_lock_unlock(&g_warm_lock);
    for (WarmItem *item in items) {
        if (item->kind == 0) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            if (!g_shcache[item->key]) { g_shcache[item->key] = item->shader; g_warm_claim_shaders++;
                if (item->record) { pso_record("S\t%s\t%d\n", item->key.UTF8String, item->shader->stage); pso_user_record("S\t%s\t%d\n", item->key.UTF8String, item->shader->stage); } }
        } else if (item->kind == 1 || item->kind == 3) {
            if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
            NvShader *vs = g_shcache[item->vs_key], *fs = item->fs_key.length ? g_shcache[item->fs_key] : nil;
            if (!vs) { vs = item->vs; g_shcache[item->vs_key] = vs; }
            if (item->fs_key.length && !fs) { fs = item->fs; g_shcache[item->fs_key] = fs; }
            if (item->kind == 3) {
                NvShader *ts = item->ts_key ? g_shcache[item->ts_key] : nil;
                if (item->ts_key && !ts) { ts = item->ts; g_shcache[item->ts_key] = ts; }
                // Claim under the live objects, but only for the same library and selected entry.
                if (warm_mesh_shader_equal(vs, item->vs) && warm_mesh_shader_equal(fs, item->fs) && warm_mesh_shader_equal(ts, item->ts)) mtk_claim(&item->pso, ts, vs, fs, item->object);
                else MLOG("native mesh prewarm: live shader variant differs, recompile if required (%s)", vs->key);
            } else {
                item->pso.vs = (__bridge void *)vs; item->pso.fs = (__bridge void *)fs;
                nvn_hmap *cache = item->pull ? &g_pso_hy : &g_pso;
                if (!hm_get_id(cache, &item->pso)) { hm_put_id(cache, &item->pso, item->object); g_warm_claim_rps++; }
            }
        } else {
            NvShader *cs = g_shcache[item->key];
            if (!cs) { cs = item->shader; if (!g_shcache) g_shcache = [NSMutableDictionary dictionary]; g_shcache[item->key] = cs; }
            cps_key key = {(__bridge const void *)cs, (uintptr_t)item->cps_off};   // warm_task built the common function, without the border corrections, and a split's passes
            if (!hm_get_id(&g_cps, &key)) { hm_put_id(&g_cps, &key, item->object); g_warm_claim_cps++; }
        }
    }
}
// A program's libraries are loaded in the background when the game makes the program (nvnProgramSetShaders, most of them while it loads a scene), and the next submission
// takes them (warm_claim) instead of its first draw loading each on the submit thread: in Donkey Kong Bananza a level's first frames loaded 3 to 7 libraries there, 20-90 ms
// with the drive idle and up to 4 s with it busy. A draw that comes before its library has loaded loads it itself, as before. DM_NVN_PREFETCH=0 turns it off
static _Atomic uint64_t g_progpf_loaded;
static int prefetch_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_PREFETCH"); v = !(e && !strcmp(e, "0")); } return v; }
void mtl_prog_prefetch(const prog_t *cp) {
    prog_t *p = (prog_t *)cp;
    if (!g_dev || !prefetch_on()) return;
    shmap_read_once();   // the map only: the prewarm starts on the submit thread (shmap_load)
    for (unsigned i = 0; i < p->nstage; i++) {
        uint8_t md[8]; if (!prog_key(p, i, md)) MDIE("program stage %u has no shader key", i);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *stage = g_shmap[k]; int st = stage ? stage_of(stage) : -1;
        if (st < 0) continue;   // not in the package: a built-in kernel (prog_get)
        NSString *pc = [k stringByAppendingString:@"_b"];
        if (!progpf_first(pc)) continue;
        dispatch_async(pp_bg_queue(), ^{ @autoreleasepool {
            NvShader *sh = shader_load(pc, st);
            if (sh && st == 0 && sh->has_pull && hybrid_on()) pull_load(sh);   // its pulling entry too, as the prewarm does (warm_shader)
            if (!sh) return;
            WarmItem *item = [WarmItem new]; item->kind = 0; item->record = 1; item->key = pc; item->shader = sh; warm_enqueue(item);
            if (!g_progpf_loaded++) MLOG("shader prefetch at program creation: first library %s loaded in the background (DM_NVN_PREFETCH=0 turns it off)", pc.UTF8String);
        } });
    }
}
// Dispatches share the open copy encoder (a Metal 4 compute encoder) with the copies and dispatches around them, each ordered after everything the encoder already holds by a barrier inside it,
// as a texture copy is (blit). An encoder of their own, with full queue barriers on both sides, cost the GPU a drain per dispatch: Hyrule Warriors: Age of Calamity's battles after the first cutscene
// run about 1,060 small dispatches a frame, and the Mac GPU went from 6.7 to 16 ms a frame at the same draws.
// An indirect dispatch does not join a copy encoder that already holds work: the encoder ends first, and the dispatch goes into the next one, which begins with a queue barrier
// (blit_open) that waits for everything before it. Inside one Metal 4 compute encoder, the barrier dispatch_enc puts between two dispatches did not order an indirect dispatch's
// group counts after the earlier dispatch that wrote them: Ocarina of Time's foliage cull (compute shaders 83df46a0b1b379b4, 60dd0e6eac91f712, cee08c634ef1f343, 58f47abfa0a84773,
// e855420a39e60874, then the list its mesh shaders draw) came out short in 39 % of frames on an M1 Max, 16 of its 276 clusters missing at random, and the flower bushes showed holes
// where their front leaves and flowers should be; with this, 0 of 600 frames. Which of Metal's reads misses the encoder's barrier, the indirect-argument fetch or the copy of the
// counts into the driver segment (dispatch), is not known. The direct dispatches keep sharing the encoder (an encoder each for them did not change the count: 41 % of frames).
// DM_NVN_IND_SPLIT=0 lets indirect dispatches join the open encoder again (to measure)
static int ind_split(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_IND_SPLIT"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_IND_SPLIT=0: indirect dispatches join the open copy encoder (their group counts can be read before an earlier dispatch in it wrote them)"); }
    return v;
}
static uint64_t g_ind_splits;
static id<MTL4ComputeCommandEncoder> dispatch_enc(void) {
    id<MTL4ComputeCommandEncoder> e = blit_open(); g_gp_disp++;
    if (g_blit_ops) blit_barrier();
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_br_n = 0; g_blit_ops = 1;   // recorded as "everything written": the next copy or dispatch gets a barrier
    if (!g_blit_ct) { [e setArgumentTable:g_ct]; g_blit_ct = 1; }
    args_dirty_all();
    return e;
}
// DM_CTL bufsnap (hle_hid.c): guest memory as the GPU leaves it right after the next dispatch of a shader, copied in the same encoder behind a barrier and saved when the
// submission completes. A read of guest memory at some later moment sees what later work of the frame, or the next frame's clear, left there (PRAGMATA's culling chain)
static pthread_mutex_t g_bs_mu = PTHREAD_MUTEX_INITIALIZER; static char g_bs_key[32], g_bs_path[512]; static uint64_t g_bs_addr; static size_t g_bs_n; static int g_bs_on;
void dm_gfx_bufsnap(const char *key, uint64_t a, size_t n, const char *path) {
    pthread_mutex_lock(&g_bs_mu); snprintf(g_bs_key, sizeof g_bs_key, "%s", key); snprintf(g_bs_path, sizeof g_bs_path, "%s", path); g_bs_addr = a; g_bs_n = n; g_bs_on = 1; pthread_mutex_unlock(&g_bs_mu);
    MLOG("DM_CTL: bufsnap of %#llx + %zu bytes after the next dispatch of %s → %s", (unsigned long long)a, n, key, path);
}
static void bufsnap_after(NvShader *cs, id ce) {
    if (!g_bs_on) return;
    pthread_mutex_lock(&g_bs_mu);
    if (!g_bs_on || strncmp(cs->key, g_bs_key, strlen(g_bs_key))) { pthread_mutex_unlock(&g_bs_mu); return; }
    uint64_t a = g_bs_addr; size_t n = g_bs_n; NSString *path = @(g_bs_path); g_bs_on = 0; pthread_mutex_unlock(&g_bs_mu);
    void *mb; uint64_t mo;
    if (!n || !mtl_addr(a, n, &mb, &mo)) { MLOG("[RED] bufsnap: %#llx + %zu bytes is in no memory pool", (unsigned long long)a, n); return; }
    id<MTLBuffer> dst = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!dst) { MLOG("[RED] bufsnap: no buffer of %zu bytes", n); return; }
    res_add(dst); res_commit();
    [(id<MTL4ComputeCommandEncoder>)ce barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:blit_vis()];
    [(id<MTL4ComputeCommandEncoder>)ce copyFromBuffer:(__bridge id<MTLBuffer>)mb sourceOffset:mo toBuffer:dst destinationOffset:0 size:n];
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_br_n = 0; g_blit_ops = 1;   // recorded as "everything written", as blit() does: the next copy or dispatch is ordered after it (a dispatch ordered by the game's barriers would otherwise overwrite what the copy still reads)
    NSString *key = @(cs->key);
    [g_commit_blocks addObject:^{ BOOL ok = [[NSData dataWithBytes:dst.contents length:n] writeToFile:path atomically:NO];
        MLOG("bufsnap after %s: %zu bytes %s %s", key.UTF8String, n, ok ? "saved to" : "could not be written to", path.UTF8String); }];
}
// Compute dispatches are ordered by the game's own barriers, as NVN orders them (DM_NVN_DISPATCH_BARRIER=full: each after everything before it in its encoder, as before; game, the
// default; ab alternates the two with the DM_AB phases, for measurement). NVN makes a dispatch's writes visible to later work when the game asks with nvnCommandBufferBarrier; the full
// order made the GPU run every dispatch of an encoder one at a time. Kirby Air Riders' particle systems are about 230 dispatches a frame in one encoder: 6.6-9.8 ms of GPU time at
// about 30 us each, 0.4-1.2 ms ordered by the game's barriers, 0.15 ms with no barrier at all (measured with the copy encoder's barriers removed, which also drops the ones the game asked for, and faults). Their
// memory ranges cannot replace the game's barriers: the systems share the buffers they write, and 91 % of the dispatches met an earlier one's range. A dispatch gets a barrier when the
// game issued one since the dispatch before it, when it reads a range a copy wrote or writes one a copy read or wrote since the last barrier (uniform updates, uploads: the host's own
// order), or when its sets are full; what it writes and reads is kept apart from the copies' sets (g_dw, g_dr), so that later copies are ordered after it and later dispatches are not.
// A dispatch with a binding whose reach is no range (an image, a buffer texture, a handle in a uniform slot, global memory traced to no storage buffer) keeps the full order on both
// sides, as do the host's own dispatches (dispatch_enc). The driver's own segments are no guest memory. A draw pass ends the copy encoder and begins with its own barrier
static int dbar_game(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_DISPATCH_BARRIER"); v = e && !strncmp(e, "ab", 2) ? sw_mode("DM_NVN_DISPATCH_BARRIER", 0) : !(e && !strcmp(e, "full"));
        if (!v) MLOG("DM_NVN_DISPATCH_BARRIER=full: every compute dispatch is ordered after everything before it in its encoder (diagnostic)"); }
    return sw_on(v);
}
typedef struct { uint64_t lo, hi; int write; } cs_range;
typedef struct { cs_range r[64]; uint64_t writes; int n, unknown; } cs_access;   // guest memory ranges read or written; unknown: a range that cannot be listed (no size, sparse, more than 64)
static void cs_access_add(cs_access *a, uint64_t lo, uint64_t size, int write) {
    if (!lo) return;
    if (!size || lo > UINT64_MAX - size || nvn_sparse_alias(lo, lo + size)) { a->unknown = 1; return; }
    for (int i = 0; i < a->n; i++) if (a->r[i].lo == lo && a->r[i].hi == lo + size) { a->r[i].write |= write; if (write) a->writes |= 1ull << i; return; }
    if (a->n == 64) { a->unknown = 1; return; }
    a->r[a->n] = (cs_range){lo, lo + size, write};
    if (write) a->writes |= 1ull << a->n;
    a->n++;
}
static cs_access cs_access_of(NvShader *sh, uint64_t ind) {   // the guest memory a dispatch reads and writes through its bindings and its indirect record; unknown when a binding's reach is no range
    cs_access a = {0};
    if (ind) cs_access_add(&a, ind, 12, 0);
    for (int i = 0; i < sh->rf.nb && !a.unknown; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        switch (b->kind) {
        case NVN_RB_CBUF: case NVN_RB_UBOP: case NVN_RB_SBUFP: {
            if (b->reg >= 16) { a.unknown = 1; break; }
            const p_bind_buf *u = b->kind == NVN_RB_SBUFP ? &S.ssbo[5][b->reg] : &S.ubo[5][b->reg];
            cs_access_add(&a, u->addr, b->kind == NVN_RB_CBUF ? cb_len(sh, b->reg, u->size) : u->size, b->kind == NVN_RB_SBUFP && b->rw && !(sh->sb_ro >> b->reg & 1));
            break; }
        case NVN_RB_SUPPORT: case NVN_RB_C1: case NVN_RB_DRV0: case NVN_RB_DRV2: case NVN_RB_CBSIZE: case NVN_RB_TBIAS: case NVN_RB_SBLEN: break;
        default: a.unknown = 1;
        }
    }
    return a;
}
// NVN barriers order guest dispatches. Without known ranges, treat the
// dispatch as reading and writing all memory for the host's copies, in both
// directions. The diagnostic override restores the former full ordering.
static int dbar_unknown_game(void) {
    static int mode = -1;
    if (mode < 0) mode = sw_mode("DM_NVN_DISPATCH_RANGELESS", 1);
    return sw_on(mode);
}
static uint64_t g_dbar_game, g_dbar_copy, g_dbar_unknown, g_dbar_free, g_dbar_ufree;
void mtl_dispatch_barrier_stats(uint64_t *game, uint64_t *copy, uint64_t *unknown, uint64_t *none) {   // the periodic [nvn] line (hle_nvn.c)
    *game = g_dbar_game; *copy = g_dbar_copy; *unknown = g_dbar_unknown; *none = g_dbar_free + g_dbar_ufree; }
static id<MTL4ComputeCommandEncoder> dispatch_enc_cs(NvShader *cs, uint64_t ind) {
    if (!dbar_game()) { g_dsp_game_bar = 0; return dispatch_enc(); }
    cs_access a = cs_access_of(cs, ind);
    if (a.unknown && !dbar_unknown_game()) { g_dbar_unknown++; g_dsp_game_bar = 0; return dispatch_enc(); }
    if (a.unknown) {
        id<MTL4ComputeCommandEncoder> e = blit_open(); g_gp_disp++;
        if (g_blit_ops && (g_dsp_game_bar || g_bw_n || g_br_n)) { g_dbar_unknown++; blit_barrier(); } else g_dbar_ufree++;   // after the game's barrier or a copy: as before
        g_dsp_game_bar = 0;
        g_dw_lo[0] = 0; g_dw_hi[0] = ~0ull; g_dw_n = 1; g_dr_lo[0] = 0; g_dr_hi[0] = ~0ull; g_dr_n = 1;   // read and written everything, for the copies after it (blit_buf, blit)
        g_blit_ops++;
        if (!g_blit_ct) { [e setArgumentTable:g_ct]; g_blit_ct = 1; }
        args_dirty_all();
        return e;
    }
    id<MTL4ComputeCommandEncoder> e = blit_open(); g_gp_disp++;
    int copy = g_dr_n + a.n > DW_MAX || g_dw_n + a.n > DW_MAX;
    for (int i = 0; i < a.n && !copy; i++) copy = bw_hit(a.r[i].lo, a.r[i].hi) || (a.r[i].write && br_hit(a.r[i].lo, a.r[i].hi));
    if (g_blit_ops && (g_dsp_game_bar || copy)) { if (g_dsp_game_bar) g_dbar_game++; else g_dbar_copy++; blit_barrier(); } else g_dbar_free++;
    g_dsp_game_bar = 0;
    for (int i = 0; i < a.n; i++) {   // a range already in its set is not added again: the systems share their tables
        uint64_t *lo = a.r[i].write ? g_dw_lo : g_dr_lo, *hi = a.r[i].write ? g_dw_hi : g_dr_hi; int *n = a.r[i].write ? &g_dw_n : &g_dr_n, have = 0;
        for (int k = 0; k < *n && !have; k++) have = lo[k] == a.r[i].lo && hi[k] == a.r[i].hi;
        if (!have) { lo[*n] = a.r[i].lo; hi[(*n)++] = a.r[i].hi; }
    }
    g_blit_ops++;
    if (!g_blit_ct) { [e setArgumentTable:g_ct]; g_blit_ct = 1; }
    args_dirty_all();
    return e;
}
// GPU-only compute containment (#2664), with one abort word per submission and one hit record per kernel in it. The captured arena buffer and shader survive until
// completion, before the arena can be reused (g_commit_blocks runs before post_complete). The hit records are separate from the shared abort: a kernel shortened by
// another's runaway must not be blamed. A recorded site is the lowest numbered site that exhausted its function budget, not proof that this loop caused the original bug.
static uint64_t loop_state(NvShader *cs) {
    if (!cs->rf.loop_sites) return 0;
    static uint64_t sub, abort_gpu;
    uint64_t fence = atomic_load(&g_fence) + 1;
    if (sub != fence) {
        __strong id<MTLBuffer> ab = nil; size_t off = arena_alloc(4, &ab);
        *(uint32_t *)((uint8_t *)ab.contents + off) = 0;
        abort_gpu = ab.gpuAddress + off; sub = fence;
    }
    if (cs->loop_sub != fence) {
        __strong id<MTLBuffer> b = nil; size_t off = arena_alloc(DMSH_LOOP_BYTES, &b);
        uint8_t *state = (uint8_t *)b.contents + off; uint32_t hit = UINT32_MAX;
        memcpy(state, &abort_gpu, sizeof abort_gpu); memcpy(state + DMSH_LOOP_HIT, &hit, sizeof hit);
        if (!cs->loop_sub) MLOG("compute loop containment: kernel %s, %u loop sites (including helpers)", cs->key, cs->rf.loop_sites);
        cs->loop_sub = fence; cs->loop_gpu = b.gpuAddress + off;
        [g_commit_blocks addObject:^{
            uint32_t site; memcpy(&site, (const uint8_t *)b.contents + off + DMSH_LOOP_HIT, sizeof site);
            if (site != UINT32_MAX && !atomic_exchange(&cs->loop_reported, 1))
                MLOG("[RED] loop cap hit: submission %llu, kernel %s, loop site %u (loop_sites in its reflection; cause open)", (unsigned long long)fence, cs->key, site);
        }];
    }
    return cs->loop_gpu;
}
static int ndve_may_take(NvShader *cs); static int up_chain_named(NvShader *cs);
static int dispatch_host_may_take(NvShader *cs) { return ndve_may_take(cs) || up_chain_named(cs) || up_mfx_wanted(cs); }
// The common pipeline of a compute shader a dispatch found none for: made here, on the dispatching thread, and kept (g_cps); nil when it cannot run (logged once, remembered)
static id<MTLComputePipelineState> dispatch_cps(NvShader *cs, int off, const cps_key *k) {
    if (cs->cps_failed) return nil;   // the same function fails the same way: asked again at every dispatch, Mario Kart World's refused shaders took 3,138 pipeline builds in 60 s
    NSError *e = nil; uint64_t bt = bp_now(); id<MTLComputePipelineState> ps = cps_new_b(cs->fn, off, &e);
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
    if (!ps) { cs->cps_failed = 1; MLOG("[RED] failed to create compute pipeline for %s: %s (its dispatches are skipped)", cs->key, e.localizedDescription.UTF8String); return nil; }
    // A pipeline's thread limit falls with the kernel's registers on the M1 and M2 (dmsh_rewrite.py max_threads); a dispatch of the shader's local size past it runs no invocation
    NSUInteger want = (NSUInteger)MAX(cs->rf.tg[0], 1) * MAX(cs->rf.tg[1], 1) * MAX(cs->rf.tg[2], 1);
    if (cs->rf.tg[0] && want > ps.maxTotalThreadsPerThreadgroup) {
        cs->cps_failed = 1;
        MLOG("[RED] compute pipeline for %s allows %lu threads a threadgroup, its local size %ux%ux%u needs %lu (its dispatches are skipped)", cs->key,
             (unsigned long)ps.maxTotalThreadsPerThreadgroup, cs->rf.tg[0], cs->rf.tg[1], cs->rf.tg[2], (unsigned long)want);
        return nil;
    }
    pso_record("C\t%s\n", cs->key);
    if (cs->key[0]) pso_user_record("C\t%s\n", cs->key);
    hm_put_id(&g_cps, k, ps);
    return ps;
}
// A kernel in two passes (dm/gfx/tools/dmsh_ndsr.py, the reflection's split: agl's NDSR upscaler network): pass 0 (DMSH_SPLIT_FC false) runs the network and writes each thread's
// words to the buffer at DMSH_SPLIT_SLOT, pass 1 (true) reads them and runs the shader's epilogue, whose registers no longer cap the network's threads (shaders.md, "agl's NDSR network
// on simdgroup matrices, in two passes"). Their pipelines sit beside the common one in g_cps (off | FN_SPLIT0 / FN_SPLIT1), the prewarm builds them with it, and the offline archive
// holds them (pso_offline.py). Both or none: a pass that does not build leaves the shader in one pass for the run. DM_NVN_SPLIT=0 runs the one pass (the kernel unspecialized)
static int split_on(void) { static int m = -1; if (m < 0) m = sw_mode("DM_NVN_SPLIT", 1); return sw_on(m); }
static int split_pipes(NvShader *cs, int off, const p_dispatch *dp, id<MTLComputePipelineState> __strong sp[2]) {
    if (cs->split_failed) return 0;
    if (dp->ind) { static int once; if (!once++) MLOG("compute shader %s: an indirect dispatch runs in one pass (its words' scratch needs the grid)", cs->key); return 0; }
    const NSUInteger want = (NSUInteger)MAX(cs->rf.tg[0], 1) * MAX(cs->rf.tg[1], 1) * MAX(cs->rf.tg[2], 1);
    for (int p = 0; p < 2; p++) {
        cps_key k = {(__bridge const void *)cs, (uintptr_t)(off | (p ? FN_SPLIT1 : FN_SPLIT0))};
        if ((sp[p] = hm_get_id(&g_cps, &k))) continue;
        NSError *e = nil; uint64_t bt = bp_now();
        sp[p] = fn_off(cs->fn, (int)k.off) ? cps_new_b(cs->fn, (int)k.off, &e) : nil;
        g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
        if (!sp[p] || sp[p].maxTotalThreadsPerThreadgroup < want) {
            MLOG("[RED] compute shader %s: pass %d of its two does not build (%s): it runs in one pass", cs->key, p,
                 sp[p] ? "fewer threads a threadgroup than its local size" : e ? e.localizedDescription.UTF8String : "the library has no such specialization");
            cs->split_failed = 1; sp[0] = sp[1] = nil; return 0;
        }
        hm_put_id(&g_cps, &k, sp[p]);
    }
    return 1;
}
static void split_dispatch(id ce, NvShader *cs, id<MTLComputePipelineState> __strong sp[2], const p_dispatch *dp, const uint16_t *tg) {
    const uint64_t groups = (uint64_t)dp->x * dp->y * dp->z, threads = (uint64_t)tg[0] * tg[1] * tg[2];
    __strong id<MTLBuffer> wb = nil; size_t wo = arena_alloc((size_t)(groups * threads * cs->rf.split_words * 4), &wb);   // frame scratch: the submission keeps it until the GPU is done
    enc_c_args(ce, wb.gpuAddress + wo, DMSH_SPLIT_SLOT);
    const MTLSize n = MTLSizeMake(dp->x, dp->y, dp->z), t = MTLSizeMake(tg[0], tg[1], tg[2]);
    enc_c_pso(ce, sp[0]); enc_c_dispatch(ce, n, t);
    [(id<MTL4ComputeCommandEncoder>)ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()];   // pass 1 reads what pass 0 wrote
    enc_c_pso(ce, sp[1]); enc_c_dispatch(ce, n, t);
    static int once; if (!once++) MLOG("compute shader %s: in two passes, %u words a thread between them (%.1f MB of frame scratch a dispatch; DM_NVN_SPLIT=0 runs one pass)", cs->key, cs->rf.split_words,
                                       groups * threads * cs->rf.split_words * 4 / 1048576.0);
}
static void dispatch_(const p_dispatch *dp) {
    // An empty direct grid invokes no shader and changes no resources. Metal
    // rejects it; indirect counts belong to the GPU and are left untouched.
    if (!dp->ind && (!dp->x || !dp->y || !dp->z)) return;
    g_wr_epoch++; g_mdx_epoch++;
    if (!S.cprog) { RED_ONCE(27, "no program bound before the dispatch"); return; }
    // DispatchComputeIndirect: the group counts {x, y, z} at ind (MTLDispatchThreadgroupsIndirectArguments) are usually written by the GPU earlier in the frame; Metal reads them when the dispatch runs
    void *ib = NULL; uint64_t io = 0, ig = 0;
    if (dp->ind && !mtl_gpu_addr(dp->ind, 12, &ib, &io, &ig)) { RED_ONCE(94, "indirect dispatch skipped: argument address %#llx is in no memory pool", (unsigned long long)dp->ind); return; }
    if (dp->ind) { static int once; if (!once++) MLOG("indirect dispatch: first time (the GPU reads the threadgroup counts from %#llx)", (unsigned long long)dp->ind); }
    NvShader *cs = prog_get(S.cprog)->sh[5];
    if (!cs) { RED_ONCE(28, "the dispatched program has no compute shader (or it is not mapped)"); return; }
    if (dp->ind) ind_fly_put(cs->key, dp->ind, io, ib);
    if (passdump()) { uint32_t n[3] = {0, 0, 0}; if (dp->ind && nvn_host_readable(dp->ind, 12)) memcpy(n, (const void *)(uintptr_t)dp->ind, 12);
        PD("g %llu dispatch %s %ux%ux%u indirect=%#llx (now %u %u %u)", g_stat_pass, cs->key, dp->x, dp->y, dp->z, (unsigned long long)dp->ind, n[0], n[1], n[2]); }
    int off = !border_need(cs, 5);
    cps_key k = {(__bridge const void *)cs, (uintptr_t)off};
    id<MTLComputePipelineState> ps = hm_get_id(&g_cps, &k);
    // A dispatch the host may draw itself (nn::ndve's network by the host's engine, the game's upscaler by MetalFX) gets its pipeline only once the host has left it to the
    // shader, below: a pipeline made first was compiled for nothing, and without the offline archive Apple's compiler takes minutes on each of nn::ndve's kernels
    const int later = !ps && dispatch_host_may_take(cs);
    if (!ps && !later && !(ps = dispatch_cps(cs, off, &k))) return;
    const uint16_t *tg = cs->rf.tg;
    if (!tg[0]) { RED_ONCE(29, "compute shader lacks threadgroup size reflection"); return; }
    // A store the shader makes on its very first invocation only, before the others run on the Switch: the build takes it out of the shader (dmsh_rewrite.py first_store), as Apple's GPUs start workgroups in no order
    if (dp->ind || (dp->x && dp->y && dp->z)) for (int i = 0; i < cs->rf.nfirst; i++) { const p_bind_buf *u = &S.ssbo[5][cs->rf.first[i].reg]; if (u->addr) mtl_sub_fill(u->addr + 4ull * cs->rf.first[i].word, 4, cs->rf.first[i].value); }
    if (g_nubd && ubd_conflict_cs(cs, dp->ind)) { g_ubd_conflicts++; ubd_land(); }   // see ubd_conflict_cs
    rp_flush();   // A full clear with no draw after it waits for the next pass on its targets, but it comes before this dispatch, which may read or write them as images: land it first, as copy_t2t does
    image_prepare(prog_get(S.cprog), 0x20);
    if (ndve_engine_dispatch(cs, dp)) return;   // nn::ndve's movie network as the host's kernels (DM_NDVE_ENGINE=1)
    {   // DM_NVN_SKIP_CS=<compute key>[,…] (diagnostic, up to 16): drops the dispatches of those compute shaders, to measure what one costs inside an encoder that holds several; what it writes stays stale
        static char sk[16][20]; static int nsk = -1;
        if (nsk < 0) { nsk = 0; for (const char *q = getenv("DM_NVN_SKIP_CS"); q && *q && nsk < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 16) n = 16; memcpy(sk[nsk], q, n); sk[nsk++][n] = 0; }
                       if (nsk) MLOG("DM_NVN_SKIP_CS: dropping the dispatches of %d compute shaders (diagnostic; the picture is wrong)", nsk); }
        for (int i = 0; i < nsk; i++) if (!strncmp(cs->key, sk[i], strlen(sk[i]))) return;
    }
    // A dispatch of the game's upscaler chain, which MetalFX draws as a whole (DM_UPSCALER=metalfx). Before the single-dispatch upscaler: the chain's network is one of the game's
    // upscaler shaders too (Ocarina of Time's square-rooted networks, up_mfx_color), which then replaces only that one dispatch, once the chain has given up (up_chain_fail)
    if (up_chain_dispatch(cs, dp->ind != 0)) return;
    if (up_mfx_wanted(cs) && !dp->ind && up_mfx_encode(cs)) return;   // the game's upscaler, drawn by MetalFX instead (DM_UPSCALER=metalfx)
    if (!ps && !(ps = dispatch_cps(cs, off, &k))) return;   // the host left it to the shader (later, above)
    id<MTLComputePipelineState> sp[2] = {nil, nil};   // a kernel in two passes (split_dispatch)
    if (cs->rf.split_words && split_on() && !cs->rf.tg_spill) split_pipes(cs, off, dp, sp);
    // Threadgroup memory past Metal's 32 KB, in a device buffer (dmsh_rewrite.py tg_spill): a slice for every workgroup of the grid. An indirect dispatch's grid is in GPU memory, written earlier
    // in the frame: the replay waits for the GPU (as an indirect-count multi-draw does, mdi_wait) and reads it. Mario Kart World's spilled dispatches are all direct, of 1 to 3 workgroups
    uint64_t tg_groups = 0;
    if (cs->rf.tg_spill) {
        uint32_t n[3] = {(uint32_t)dp->x, (uint32_t)dp->y, (uint32_t)dp->z};
        if (dp->ind) {
            mdi_wait();
            if (!nvn_host_readable(dp->ind, 12)) { RED_ONCE(161, "indirect dispatch of %s with threadgroup memory in a device buffer: its counts at %#llx are in an unmapped page of a sparse pool; skipped", cs->key, (unsigned long long)dp->ind); return; }
            memcpy(n, (const void *)(uintptr_t)dp->ind, 12);
            static int once; if (!once++) MLOG("compute shader %s: indirect dispatch with threadgroup memory in a device buffer; the replay waits for the GPU to read its %ux%ux%u workgroups", cs->key, n[0], n[1], n[2]);
        }
        tg_groups = MAX((uint64_t)n[0] * n[1] * n[2], 1ull);
        if (tg_groups > (g_dev.maxBufferLength - (4u << 20)) / cs->rf.tg_spill) { RED_ONCE(162, "dispatch of %s: %llu workgroups of %u bytes of threadgroup memory exceed Metal's largest buffer; skipped", cs->key, (unsigned long long)tg_groups, cs->rf.tg_spill); return; }
    }
    if (dp->ind && g_blit && ind_split()) { enc_b_end(g_blit); g_blit = nil;   // an indirect dispatch begins an encoder (ind_split)
        if (!g_ind_splits++) MLOG("indirect dispatch: the copy encoder ends before it, so its group counts are read after the work before it (first time, %s)", cs->key); }
    g_dispatch_dim[0] = (uint32_t)dp->x; g_dispatch_dim[1] = (uint32_t)dp->y; g_dispatch_dim[2] = (uint32_t)dp->z;   // read back by the driver segment c0 (encode_args_)
    g_c0_dim_buf = nil;
    // A sampled format view may upgrade its texture and copy its pixels. Prepare the compute table before dispatch_enc's copy-to-dispatch barrier:
    // otherwise the upgrade's copy races the dispatch (#1743).
    encode_args(nil, cs, 5, 0);
    id<MTL4ComputeCommandEncoder> ce = dispatch_enc_cs(cs, dp->ind);
    enc_c_pso(ce, ps);
    if (cs->rf.loop_sites) enc_c_args(ce, loop_state(cs), DMSH_LOOP_SLOT);
    if (tg_groups) {
        __strong id<MTLBuffer> tb = nil; size_t to = arena_alloc((size_t)(tg_groups * cs->rf.tg_spill), &tb);
        enc_c_args(ce, tb.gpuAddress + to, DMSH_TG_SLOT);
        static int once; if (!once++) MLOG("compute shader %s: %u bytes of threadgroup memory a workgroup, past Metal's %d, from a device buffer (first dispatch %llu workgroups)", cs->key, cs->rf.tg_spill, DMSH_TG_MAX, (unsigned long long)tg_groups);
    }
    if (dp->ind) {   // The shader reads the group counts from c0 + 0x200: copy the GPU-written ones there first (Metal 4 orders nothing inside an encoder without a barrier)
        if (g_c0_dim_buf) { [ce copyFromBuffer:(__bridge id<MTLBuffer>)ib sourceOffset:io toBuffer:g_c0_dim_buf destinationOffset:g_c0_dim_off size:12];
            [ce copyFromBuffer:(__bridge id<MTLBuffer>)ib sourceOffset:io toBuffer:g_c0_dim_buf destinationOffset:g_c0_dim_off - DMSH_C0_DISPATCH + DMSH_C0_DISPATCH_NVN2 size:12];
            [ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()]; }
        enc_c_dispatch_indirect(ce, ig, MTLSizeMake(tg[0], tg[1], tg[2]));
    } else if (sp[1]) split_dispatch(ce, cs, sp, dp, tg);
    else enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(tg[0], tg[1], tg[2]));
    bufsnap_after(cs, ce);
}
// Image stores into a side of a compressed-alias pair (image_prepare noted them) go into its other side when the dispatch is encoded, also one the host's own kernels took over
// (ndve_engine_dispatch, up_mfx_encode return early): left waiting, its copy came after the next pass, which may read the other side. Not while a draw's pass is open (a dispatch
// that encoded nothing: its notes are the pass's, copied when it ends)
static void dispatch(const p_dispatch *dp) {
    dispatch_(dp);
    if (g_nbcp && !g_enc) bcp_flush();
}
