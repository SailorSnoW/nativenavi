// Compute pipelines, library prefetch, dispatch encoders and barriers, loop containment, kernels in two passes.
// cps_key.off: 1 without the border corrections, | FN_SPLIT0 / FN_SPLIT1
typedef struct { const void *cs; uintptr_t off; } cps_key;
static nvn_hmap g_cps = {.klen = sizeof(cps_key)};
// shader_load can return distinct objects for one shader: equal when the library key and the selected entry match
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
        if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
        if (item->kind == 0) {
            if (!g_shcache[item->key]) {
                g_shcache[item->key] = item->shader; g_warm_claim_shaders++;
                if (item->record) {
                    pso_record("S\t%s\t%d\n", item->key.UTF8String, item->shader->stage);
                    pso_user_record("S\t%s\t%d\n", item->key.UTF8String, item->shader->stage);
                }
            }
        } else if (item->kind == 1 || item->kind == 3) {
            NvShader *vs = g_shcache[item->vs_key], *fs = item->fs_key.length ? g_shcache[item->fs_key] : nil;
            if (!vs) { vs = item->vs; g_shcache[item->vs_key] = vs; }
            if (item->fs_key.length && !fs) { fs = item->fs; g_shcache[item->fs_key] = fs; }
            if (item->kind == 3) {
                NvShader *ts = item->ts_key ? g_shcache[item->ts_key] : nil;
                if (item->ts_key && !ts) { ts = item->ts; g_shcache[item->ts_key] = ts; }
                if (warm_mesh_shader_equal(vs, item->vs) && warm_mesh_shader_equal(fs, item->fs) && warm_mesh_shader_equal(ts, item->ts)) mtk_claim(&item->pso, ts, vs, fs, item->object);
                else MLOG("native mesh prewarm: live shader variant differs, recompile if required (%s)", vs->key);
            } else {
                item->pso.vs = (__bridge void *)vs; item->pso.fs = (__bridge void *)fs;
                nvn_hmap *cache = item->pull ? &g_pso_hy : &g_pso;
                if (!hm_get_id(cache, &item->pso)) { hm_put_id(cache, &item->pso, item->object); g_warm_claim_rps++; }
            }
        } else {
            NvShader *cs = g_shcache[item->key];
            if (!cs) { cs = item->shader; g_shcache[item->key] = cs; }
            cps_key key = {(__bridge const void *)cs, (uintptr_t)item->cps_off};
            if (!hm_get_id(&g_cps, &key)) { hm_put_id(&g_cps, &key, item->object); g_warm_claim_cps++; }
        }
    }
}
// A program's libraries load in the background when the game makes it; the next submission takes them (warm_claim).
static _Atomic uint64_t g_progpf_loaded;
static int prefetch_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_PREFETCH"); v = !(e && !strcmp(e, "0")); }
    return v;
}
void mtl_prog_prefetch(const prog_t *cp) {
    prog_t *p = (prog_t *)cp;
    if (!g_dev || !prefetch_on()) return;
    shmap_read_once();   // the map only: the prewarm starts on the submit thread
    for (unsigned i = 0; i < p->nstage; i++) {
        uint8_t md[8]; if (!prog_key(p, i, md)) MDIE("program stage %u has no shader key", i);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *stage = g_shmap[k]; int st = stage ? stage_of(stage) : -1;
        if (st < 0) continue;   // not in the package: a built-in kernel
        NSString *pc = [k stringByAppendingString:@"_b"];
        if (!progpf_first(pc)) continue;
        dispatch_async(pp_bg_queue(), ^{ @autoreleasepool {
            NvShader *sh = shader_load(pc, st);
            if (!sh) return;
            if (st == 0 && sh->has_pull && hybrid_on()) pull_load(sh);
            WarmItem *item = [WarmItem new];
            item->kind = 0; item->record = 1; item->key = pc; item->shader = sh;
            warm_enqueue(item);
            if (!g_progpf_loaded++) MLOG("shader prefetch at program creation: first library %s loaded in the background (DM_NVN_PREFETCH=0 turns it off)", pc.UTF8String);
        } });
    }
}
// Dispatches share the open copy encoder, each ordered by a barrier inside it (docs/reference/rendering.md#compute-dispatches-share-the-copy-encoder).
// Workaround: an indirect dispatch does not join an encoder that already holds work, as the barrier did not order its group counts after the dispatch that wrote them.
// Open: which read misses it (docs/reference/rendering.md#an-indirect-dispatch-read-its-group-counts-before-the-dispatch-that-wrote-them).
static int ind_split(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_IND_SPLIT");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_IND_SPLIT=0: indirect dispatches join the open copy encoder (their group counts can be read before an earlier dispatch in it wrote them)");
    }
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
// DM_CTL bufsnap: g_bs_mu covers the request
static pthread_mutex_t g_bs_mu = PTHREAD_MUTEX_INITIALIZER;
static char g_bs_key[32], g_bs_path[512];
static uint64_t g_bs_addr;
static size_t g_bs_n;
static int g_bs_on;
void dm_gfx_bufsnap(const char *key, uint64_t a, size_t n, const char *path) {
    pthread_mutex_lock(&g_bs_mu);
    snprintf(g_bs_key, sizeof g_bs_key, "%s", key); snprintf(g_bs_path, sizeof g_bs_path, "%s", path);
    g_bs_addr = a; g_bs_n = n; g_bs_on = 1;
    pthread_mutex_unlock(&g_bs_mu);
    MLOG("DM_CTL: bufsnap of %#llx + %zu bytes after the next dispatch of %s → %s", (unsigned long long)a, n, key, path);
}
static void bufsnap_after(NvShader *cs, id ce) {
    if (!g_bs_on) return;
    pthread_mutex_lock(&g_bs_mu);
    if (!g_bs_on || strncmp(cs->key, g_bs_key, strlen(g_bs_key))) { pthread_mutex_unlock(&g_bs_mu); return; }
    uint64_t a = g_bs_addr; size_t n = g_bs_n; NSString *path = @(g_bs_path); g_bs_on = 0;
    pthread_mutex_unlock(&g_bs_mu);
    void *mb; uint64_t mo;
    if (!n || !mtl_addr(a, n, &mb, &mo)) { MLOG("[RED] bufsnap: %#llx + %zu bytes is in no memory pool", (unsigned long long)a, n); return; }
    id<MTLBuffer> dst = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
    if (!dst) { MLOG("[RED] bufsnap: no buffer of %zu bytes", n); return; }
    res_add(dst); res_commit();
    [(id<MTL4ComputeCommandEncoder>)ce barrierAfterEncoderStages:MTLStageDispatch | MTLStageBlit beforeEncoderStages:MTLStageDispatch | MTLStageBlit visibilityOptions:blit_vis()];
    [(id<MTL4ComputeCommandEncoder>)ce copyFromBuffer:(__bridge id<MTLBuffer>)mb sourceOffset:mo toBuffer:dst destinationOffset:0 size:n];
    g_bw_lo[0] = 0; g_bw_hi[0] = ~0ull; g_bw_n = 1; g_br_n = 0; g_blit_ops = 1;   // "everything written": the next dispatch gets a barrier, so it cannot overwrite what the copy still reads
    NSString *key = @(cs->key);
    [g_commit_blocks addObject:^{ BOOL ok = [[NSData dataWithBytes:dst.contents length:n] writeToFile:path atomically:NO];
        MLOG("bufsnap after %s: %zu bytes %s %s", key.UTF8String, n, ok ? "saved to" : "could not be written to", path.UTF8String); }];
}
// Dispatches follow the game's barriers, as NVN orders them: their ranges cannot replace those, since dispatches share the buffers they write.
// Ranges only order them against the host's copies.
static int dbar_game(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_DISPATCH_BARRIER");
        v = e && !strncmp(e, "ab", 2) ? sw_mode("DM_NVN_DISPATCH_BARRIER", 0) : !(e && !strcmp(e, "full"));
        if (!v) MLOG("DM_NVN_DISPATCH_BARRIER=full: every compute dispatch is ordered after everything before it in its encoder (diagnostic)");
    }
    return sw_on(v);
}
typedef struct { uint64_t lo, hi; int write; } cs_range;
typedef struct { cs_range r[64]; uint64_t writes; int n, unknown; } cs_access;   // unknown: a range that cannot be listed (no size, sparse, more than 64)
static void cs_access_add(cs_access *a, uint64_t lo, uint64_t size, int write) {
    if (!lo) return;
    if (!size || lo > UINT64_MAX - size || nvn_sparse_alias(lo, lo + size)) { a->unknown = 1; return; }
    for (int i = 0; i < a->n; i++) if (a->r[i].lo == lo && a->r[i].hi == lo + size) { a->r[i].write |= write; if (write) a->writes |= 1ull << i; return; }
    if (a->n == 64) { a->unknown = 1; return; }
    a->r[a->n] = (cs_range){lo, lo + size, write};
    if (write) a->writes |= 1ull << a->n;
    a->n++;
}
static cs_access cs_access_of(NvShader *sh, uint64_t ind) {
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
// A binding whose reach is no range follows the game's barriers against dispatches and counts as all memory against the host's copies.
static int dbar_unknown_game(void) {
    static int mode = -1;
    if (mode < 0) mode = sw_mode("DM_NVN_DISPATCH_RANGELESS", 1);
    return sw_on(mode);
}
static uint64_t g_dbar_game, g_dbar_copy, g_dbar_unknown, g_dbar_free, g_dbar_ufree;
void mtl_dispatch_barrier_stats(uint64_t *game, uint64_t *copy, uint64_t *unknown, uint64_t *none) {
    *game = g_dbar_game; *copy = g_dbar_copy; *unknown = g_dbar_unknown; *none = g_dbar_free + g_dbar_ufree; }
static id<MTL4ComputeCommandEncoder> dispatch_enc_cs(NvShader *cs, uint64_t ind) {
    if (!dbar_game()) { g_dsp_game_bar = 0; return dispatch_enc(); }
    cs_access a = cs_access_of(cs, ind);
    if (a.unknown && !dbar_unknown_game()) { g_dbar_unknown++; g_dsp_game_bar = 0; return dispatch_enc(); }
    id<MTL4ComputeCommandEncoder> e = blit_open(); g_gp_disp++;
    if (a.unknown) {
        if (g_blit_ops && (g_dsp_game_bar || g_bw_n || g_br_n)) { g_dbar_unknown++; blit_barrier(); } else g_dbar_ufree++;
        g_dw_lo[0] = 0; g_dw_hi[0] = ~0ull; g_dw_n = 1; g_dr_lo[0] = 0; g_dr_hi[0] = ~0ull; g_dr_n = 1;
    } else {
        int copy = g_dr_n + a.n > DW_MAX || g_dw_n + a.n > DW_MAX;
        for (int i = 0; i < a.n && !copy; i++) copy = bw_hit(a.r[i].lo, a.r[i].hi) || (a.r[i].write && br_hit(a.r[i].lo, a.r[i].hi));
        if (g_blit_ops && (g_dsp_game_bar || copy)) { if (g_dsp_game_bar) g_dbar_game++; else g_dbar_copy++; blit_barrier(); } else g_dbar_free++;
        for (int i = 0; i < a.n; i++) {   // a range already in its set is not added again: dispatches share their tables
            uint64_t *lo = a.r[i].write ? g_dw_lo : g_dr_lo, *hi = a.r[i].write ? g_dw_hi : g_dr_hi;
            int *n = a.r[i].write ? &g_dw_n : &g_dr_n;
            int have = 0;
            for (int k = 0; k < *n && !have; k++) have = lo[k] == a.r[i].lo && hi[k] == a.r[i].hi;
            if (!have) { lo[*n] = a.r[i].lo; hi[(*n)++] = a.r[i].hi; }
        }
    }
    g_dsp_game_bar = 0;
    g_blit_ops++;
    if (!g_blit_ct) { [e setArgumentTable:g_ct]; g_blit_ct = 1; }
    args_dirty_all();
    return e;
}
// The site recorded is the lowest-numbered one that exhausted its budget, not proof that its loop is the cause.
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
static int ndve_may_take(NvShader *cs);
static int up_chain_named(NvShader *cs);
static int dispatch_host_may_take(NvShader *cs) { return ndve_may_take(cs) || up_chain_named(cs) || up_mfx_wanted(cs); }
// nil when it cannot run (logged once, remembered)
static id<MTLComputePipelineState> dispatch_cps(NvShader *cs, int off, const cps_key *k) {
    if (cs->cps_failed) return nil;
    NSError *e = nil; uint64_t bt = bp_now(); id<MTLComputePipelineState> ps = cps_new_b(cs->fn, off, &e);
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
    if (!ps) { cs->cps_failed = 1; MLOG("[RED] failed to create compute pipeline for %s: %s (its dispatches are skipped)", cs->key, e.localizedDescription.UTF8String); return nil; }
    // The thread limit falls with the kernel's registers on the M1 and M2; a dispatch past it runs no invocation.
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
// docs/reference/shaders.md, agl's NDSR network on simdgroup matrices, in two passes. Both passes or none: one that does not build leaves the shader in one pass.
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
    [(id<MTL4ComputeCommandEncoder>)ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()];
    enc_c_pso(ce, sp[1]); enc_c_dispatch(ce, n, t);
    static int once; if (!once++) MLOG("compute shader %s: in two passes, %u words a thread between them (%.1f MB of frame scratch a dispatch; DM_NVN_SPLIT=0 runs one pass)", cs->key, cs->rf.split_words,
                                       groups * threads * cs->rf.split_words * 4 / 1048576.0);
}
static void dispatch_(const p_dispatch *dp) {
    // Metal rejects an empty direct grid; an indirect grid's counts are the GPU's
    if (!dp->ind && (!dp->x || !dp->y || !dp->z)) return;
    g_wr_epoch++; g_mdx_epoch++;
    if (!S.cprog) { RED_ONCE(27, "no program bound before the dispatch"); return; }
    void *ib = NULL; uint64_t io = 0, ig = 0;
    if (dp->ind && !mtl_gpu_addr(dp->ind, 12, &ib, &io, &ig)) { RED_ONCE(94, "indirect dispatch skipped: argument address %#llx is in no memory pool", (unsigned long long)dp->ind); return; }
    if (dp->ind) { static int once; if (!once++) MLOG("indirect dispatch: first time (the GPU reads the threadgroup counts from %#llx)", (unsigned long long)dp->ind); }
    NvShader *cs = prog_get(S.cprog)->sh[5];
    if (!cs) { RED_ONCE(28, "the dispatched program has no compute shader (or it is not mapped)"); return; }
    if (dp->ind) ind_fly_put(cs->key, dp->ind, io, ib);
    if (passdump()) {
        uint32_t n[3] = {0, 0, 0};
        if (dp->ind && nvn_host_readable(dp->ind, 12)) memcpy(n, (const void *)(uintptr_t)dp->ind, 12);
        PD("g %llu dispatch %s %ux%ux%u indirect=%#llx (now %u %u %u)", g_stat_pass, cs->key, dp->x, dp->y, dp->z, (unsigned long long)dp->ind, n[0], n[1], n[2]);
    }
    int off = !border_need(cs, 5);
    cps_key k = {(__bridge const void *)cs, (uintptr_t)off};
    id<MTLComputePipelineState> ps = hm_get_id(&g_cps, &k);
    // Its pipeline waits until the host leaves it to the shader: Apple's compiler takes minutes on each of nn::ndve's kernels.
    const int later = !ps && dispatch_host_may_take(cs);
    if (!ps && !later && !(ps = dispatch_cps(cs, off, &k))) return;
    const uint16_t *tg = cs->rf.tg;
    if (!tg[0]) { RED_ONCE(29, "compute shader lacks threadgroup size reflection"); return; }
    // A store made on the first invocation only (dmsh_rewrite.py first_store): the host makes it, as Apple's GPUs start workgroups in no order
    for (int i = 0; i < cs->rf.nfirst; i++) {
        const p_bind_buf *u = &S.ssbo[5][cs->rf.first[i].reg];
        if (u->addr) mtl_sub_fill(u->addr + 4ull * cs->rf.first[i].word, 4, cs->rf.first[i].value);
    }
    if (g_nubd && ubd_conflict_cs(cs, dp->ind)) { g_ubd_conflicts++; ubd_land(); }
    rp_flush();   // a pending full clear comes before this dispatch, which may use its targets as images
    image_prepare(prog_get(S.cprog), 0x20);
    if (ndve_engine_dispatch(cs, dp)) return;
    {
        static char sk[16][20]; static int nsk = -1;
        if (nsk < 0) {
            nsk = 0;
            for (const char *q = getenv("DM_NVN_SKIP_CS"); q && *q && nsk < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
                size_t n = strcspn(q, ",");
                if (n > 16) n = 16;
                memcpy(sk[nsk], q, n); sk[nsk++][n] = 0;
            }
            if (nsk) MLOG("DM_NVN_SKIP_CS: dropping the dispatches of %d compute shaders (diagnostic; the picture is wrong)", nsk);
        }
        for (int i = 0; i < nsk; i++) if (!strncmp(cs->key, sk[i], strlen(sk[i]))) return;
    }
    // The upscaler chain comes before the single-dispatch upscaler: the chain's network is one of the upscaler shaders too.
    if (up_chain_dispatch(cs, dp->ind != 0)) return;
    if (up_mfx_wanted(cs) && !dp->ind && up_mfx_encode(cs)) return;
    if (!ps && !(ps = dispatch_cps(cs, off, &k))) return;
    id<MTLComputePipelineState> sp[2] = {nil, nil};
    if (cs->rf.split_words && split_on() && !cs->rf.tg_spill) split_pipes(cs, off, dp, sp);
    // Threadgroup memory past Metal's 32 KB in a device buffer (dmsh_rewrite.py tg_spill): an indirect grid is read after waiting for the GPU (mdi_wait).
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
    if (dp->ind && g_blit && ind_split()) { enc_b_end(g_blit); g_blit = nil;
        if (!g_ind_splits++) MLOG("indirect dispatch: the copy encoder ends before it, so its group counts are read after the work before it (first time, %s)", cs->key); }
    g_dispatch_dim[0] = (uint32_t)dp->x; g_dispatch_dim[1] = (uint32_t)dp->y; g_dispatch_dim[2] = (uint32_t)dp->z;   // read back by the driver segment c0
    g_c0_dim_buf = nil;
    // The table is encoded before dispatch_enc's copy-to-dispatch barrier: a sampled format view may copy its texture's pixels, which would race the dispatch.
    encode_args(nil, cs, 5, 0);
    id<MTL4ComputeCommandEncoder> ce = dispatch_enc_cs(cs, dp->ind);
    enc_c_pso(ce, ps);
    if (cs->rf.loop_sites) enc_c_args(ce, loop_state(cs), DMSH_LOOP_SLOT);
    if (tg_groups) {
        __strong id<MTLBuffer> tb = nil; size_t to = arena_alloc((size_t)(tg_groups * cs->rf.tg_spill), &tb);
        enc_c_args(ce, tb.gpuAddress + to, DMSH_TG_SLOT);
        static int once; if (!once++) MLOG("compute shader %s: %u bytes of threadgroup memory a workgroup, past Metal's %d, from a device buffer (first dispatch %llu workgroups)", cs->key, cs->rf.tg_spill, DMSH_TG_MAX, (unsigned long long)tg_groups);
    }
    if (dp->ind) {   // the shader reads the group counts from c0 + 0x200 (Metal 4 orders nothing inside an encoder without a barrier)
        if (g_c0_dim_buf) {
            [ce copyFromBuffer:(__bridge id<MTLBuffer>)ib sourceOffset:io toBuffer:g_c0_dim_buf destinationOffset:g_c0_dim_off size:12];
            [ce copyFromBuffer:(__bridge id<MTLBuffer>)ib sourceOffset:io toBuffer:g_c0_dim_buf destinationOffset:g_c0_dim_off - DMSH_C0_DISPATCH + DMSH_C0_DISPATCH_NVN2 size:12];
            [ce barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageDispatch visibilityOptions:resource_vis()];
        }
        enc_c_dispatch_indirect(ce, ig, MTLSizeMake(tg[0], tg[1], tg[2]));
    } else if (sp[1]) split_dispatch(ce, cs, sp, dp, tg);
    else enc_c_dispatch(ce, MTLSizeMake(dp->x, dp->y, dp->z), MTLSizeMake(tg[0], tg[1], tg[2]));
    bufsnap_after(cs, ce);
}
// Image stores into one side of a compressed-alias pair are copied to the other side once the dispatch is encoded, also when a host kernel took it over.
// Not while a draw's pass is open: then they are copied when it ends.
static void dispatch(const p_dispatch *dp) {
    dispatch_(dp);
    if (g_nbcp && !g_enc) bcp_flush();
}
