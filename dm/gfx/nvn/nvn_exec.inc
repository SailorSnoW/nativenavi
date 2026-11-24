// Texture clears, submission segments (DM_NVN_CUT) and pass splits, clears a load action cannot express, runs of buffer copies, and mtl_exec, the replay of a command stream.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// The colour channels a Metal format has, as a channel write mask: a clear whose mask covers them all writes every texel whole (FINAL FANTASY TACTICS - The Ivalice Chronicles clears an
// R8 render target with mask 0x1)
static unsigned mtl_pf_chmask(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 1: return 0x8;                                                                                      // A8
        case 10: case 11: case 12: case 13: case 14: case 20: case 22: case 23: case 24: case 25: case 53: case 54: case 55: return 0x1;   // R8 / R16 / R32
        case 30: case 31: case 32: case 33: case 34: case 60: case 62: case 63: case 64: case 65: case 103: case 104: case 105: return 0x3;   // RG8 / RG16 / RG32
        case 40: case 92: case 93: return 0x7;                                                                   // B5G6R5 / RG11B10F / RGB9E5
    }
    return 0xf;
}
// GPU: clear a texture region with the given value (previously mislisted as a no-op, called 5 times at initialization; the initial value of a Metal private texture is undefined, and the game relies on the cleared initial value).
// Whole level and whole face, all four channels written -> open one clear-only render pass per layer / depth slice (the hardware converts the clear value by the texture format, including integer and depth);
// View with changed format: clear on that view. Partial colour regions / channel masks use a triangle and preserve everything else.
static void clear_tex_tri(id<MTLTexture> t, unsigned lv, unsigned l0, const p_clear_tex *c);
static void clear_tex(const p_clear_tex *c) {
    id<MTLTexture> t = tex_mtl_all(c->tex); sparse_gpu_wrote(c->tex); tu_wrote(t);
    tex_t pair; int paired = t && bcp_before(c->tex, t, &pair);   // a side of a compressed-alias pair: filled first, the cleared region goes into its other side after
    PD("X ser=%llu view format %#x region %d,%d,%d %dx%dx%d mask %#x %s %g,%g,%g,%g", c->tex ? c->tex->serial : 0ull, c->has_view && c->view.set_format ? c->view.format : -1, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, c->mask,
       c->is_uint ? "ui" : "f", c->is_uint ? (double)c->c.u[0] : c->c.f[0], c->is_uint ? (double)c->c.u[1] : c->c.f[1], c->is_uint ? (double)c->c.u[2] : c->c.f[2], c->is_uint ? (double)c->c.u[3] : c->c.f[3]);
    if (!t) return;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
    int is3d = t.textureType == MTLTextureType3D, nz = c->r.depth ? c->r.depth : 1;
    if (!(t.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(t.pixelFormat) && t.sampleCount == 1)
        t = tex_upgrade(c->tex, t, MTLTextureUsageRenderTarget, t.pixelFormat);   // a clear of a texture made without render-target use (packaged): give it that use
    if (c->has_view && c->view.set_format) {   // View with changed format: the clear value is interpreted in the view format (all 5 of the game's calls are the ui version with an integer view), and the clear is done on a view of the same format
        uint32_t vpf = 0; uint8_t swz[4];
        if (!nvn_fmt_to_mtl(c->view.format, &vpf, swz) || !vpf) { RED_ONCE(42, "texture clear view format %#x has no equivalent", c->view.format); return; }
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf) && !(t.usage & MTLTextureUsagePixelFormatView)) t = tex_upgrade_pfv(c->tex, t, (MTLPixelFormat)vpf);
        if (tex_need_pfv(t.pixelFormat, (MTLPixelFormat)vpf)) {
            id<MTLTexture> v = [t newTextureViewWithPixelFormat:(MTLPixelFormat)vpf];
            if (!v) { RED_ONCE(43, "texture clear: cannot create the view: format %lu → %u", (unsigned long)t.pixelFormat, vpf); return; }
            t = v;
        }
    }
    id<MTLTexture> whole = t.parentTexture ? t.parentTexture : t;
    MTLPixelFormat pf = t.pixelFormat;
    int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8, stencil = pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth32Float_Stencil8;
    if (c->r.xoffset || c->r.yoffset || (NSUInteger)c->r.width != w || (NSUInteger)c->r.height != h || (c->mask & mtl_pf_chmask(pf)) != mtl_pf_chmask(pf)
        || !(t.usage & MTLTextureUsageRenderTarget)) {
        if (!depth && !stencil && (t.usage & MTLTextureUsageRenderTarget)) {
            clear_tex_tri(t, lv, l0, c);
            gsb_store(c->tex, whole, lv, (is3d ? 0 : l0) + (unsigned)c->r.zoffset, (unsigned)nz, &c->r);   // the clear writes the storage too, as a render pass does (rt_mirror_flush)
            if (paired) bcp_after_to(c->tex, &pair, lv, &c->r);
            return;
        }
        RED_ONCE(44, "texture clear case not implemented: region %d,%d %dx%d (level size %lux%lu) mask %#x render-target capable %d (format %#x flags %#x)", c->r.xoffset, c->r.yoffset, c->r.width, c->r.height,
                 (unsigned long)w, (unsigned long)h, c->mask, !!(t.usage & MTLTextureUsageRenderTarget), c->tex->d.format, c->tex->d.flags);
        return;
    }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : stencil ? rp.stencilAttachment : rp.colorAttachments[0];
        a.texture = t; a.level = lv; a.loadAction = MTLLoadActionClear; a.storeAction = MTLStoreActionStore;
        if (is3d) a.depthPlane = c->r.zoffset + z; else a.slice = l0 + c->r.zoffset + z;
        if (depth) { rp.depthAttachment.clearDepth = pf == MTLPixelFormatDepth16Unorm ? nvn_depth16_clear_value(c->c.f[0]) : c->c.f[0];
            if (stencil) { rp.stencilAttachment.texture = t; rp.stencilAttachment.level = lv; rp.stencilAttachment.slice = a.slice; rp.stencilAttachment.loadAction = MTLLoadActionClear;
                           rp.stencilAttachment.storeAction = MTLStoreActionStore; rp.stencilAttachment.clearStencil = c->c.u[1]; } }
        else if (stencil) rp.stencilAttachment.clearStencil = c->c.u[0];
        else rp.colorAttachments[0].clearColor = c->is_uint ? MTLClearColorMake(c->c.u[0], c->c.u[1], c->c.u[2], c->c.u[3]) : MTLClearColorMake(c->c.f[0], c->c.f[1], c->c.f[2], c->c.f[3]);
        enc_r_end(enc_r_begin(rp));
    }
    if (depth || stencil) return;
    gsb_store(c->tex, whole, lv, (is3d ? 0 : l0) + (unsigned)c->r.zoffset, (unsigned)nz, &c->r);
    if (paired) bcp_after_to(c->tex, &pair, lv, &c->r);
}

// Context description of one submit (mtl_sub_commit takes one per submit and prints it into the failure line when the command buffer errors): for the GPU memory fault on the first frame entering Travincal, the failure line had only the sequence number and error code, and it was impossible to tell what was in that submit
static const char *cctx_snap(void) {
    static char b[320]; static uint64_t p_pass, p_up, p_reup, p_ser; uint64_t up = 0, reup = 0; mtl_pkg_stats(&up, &reup);
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    snprintf(b, sizeof b, "frame %llu; since the last submission: passes +%llu, first texture uploads +%llu, re-uploads +%llu, serial table changes +%llu; deferred-release queue %lu",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)(g_stat_pass - p_pass), (unsigned long long)(up - p_up), (unsigned long long)(reup - p_reup),
             (unsigned long long)(g_ser_gen - p_ser), nd);
    p_pass = g_stat_pass; p_up = up; p_reup = reup; p_ser = g_ser_gen; return b;
}
// When the game itself asks for 30 fps (pacing by the game's present interval, interval 2, hle_nvn.c) the display link runs at 30 Hz too: games whose logic counts frames, such as Engage, are not pulled up to 60 fps by the display link
__attribute__((weak)) int nvn_game_fps(void) { return 60; }   // hle_nvn.c provides the strong definition
int mtl_fps_target(void) { static int env = -2; if (env == -2) { const char *e = getenv("DM_FPS"); env = e ? atoi(e) : -1; if (env != 30 && env != 60) env = -1; else MLOG("DM_FPS=%d: frame rate pinned by the test switch", env); }
    int g = nvn_game_fps(); return env > 0 ? env : g == 30 || g == 120 ? g : 60; }
// Hand a submission to the GPU in segments (DM_NVN_CUT=draw count, 0 = off; default 64): when the game holds back the next frame's submit until this frame's GPU work is done (The Legend of Zelda: Breath of the Wild: the main thread's nvnSyncWait on the fence of the frame just submitted),
// the GPU sits idle for the milliseconds the host spends translating inside the submit (in the shrine each frame is GPU 13 ms + translation 7 ms back to back). Each time this many draws have accumulated, the command buffer encoded so far is committed between two passes: the GPU runs the earlier passes while the CPU translates the later ones.
// Cut only when no encoder is open and no pass is deferred / merged / hoisted; commit this batch's uploads first (later draws use them). The fence signal and completion work stay on the last segment only: the queue executes in commit order and every encoder starts with a queue barrier
// A buffer → texture copy is cut after as well, at the next pass boundary whatever the draw count (g_cut_copy, set by copy_b2t): on the Switch the GPU runs a submission's copies as soon as the
// submit returns, and a game may reuse the copy's source right after. Catherine: Full Body's resource thread writes the next texture into the staging block 64 ms after the main thread
// recorded a copy from it (main+0x305e64, frame start) and before that frame's submit had even returned: the host was building pipelines inside the submit for 300 ms, and committed the
// copy only at its end, so the subtitle band's gradient (BC3, 32x512) came out as the next texture's blocks. Committed at the first pass boundary, the copies run while the host encodes the rest
static int cut_draws(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_CUT"); v = e ? atoi(e) : 64; MLOG("segmented submission: a segment is submitted between passes every %d draws (DM_NVN_CUT, 0 = off)", v); } return v; }
static uint64_t g_cut_draw0, g_cuts, g_copy_cuts;
// The count starts with the submission (mtl_sub_begin). It used to start at the submission's first sub_cut, which only a render target change reaches (OP_SET_RT): the draws a
// submission makes into the target it inherited, before its first target change, were never counted, and the first segment came that many draws late
static void sub_cut_begin(void) { g_cut_draw0 = g_stat_draw; }
// DM_FSTEP_PROBE_RAMP=1 (default) / 0 / ab: a frame-step probe's grace is renewed while its frames keep getting cheaper (nvn_fstep.h FSTEP_RAMP), the time macOS's GPU clock takes to rise for the shorter deadline
static int probe_ramp_mode(void) { static int m = -1; if (m < 0) m = sw_mode("DM_FSTEP_PROBE_RAMP", 1); return m; }
int mtl_probe_ramp_on(void) { return sw_on(probe_ramp_mode()); }
// Whether a segment may end here, the pass aside: pass_split_try ends a pass only when the commit after it will be made
static int sub_cut_ready(void) {
    int n = cut_draws();
    return g_cb && (g_cut_copy || (n > 0 && g_stat_draw - g_cut_draw0 >= (uint64_t)n)) && !g_rt_merged;
}
static int cut_busy_limit(void) {
    static int lim = -1;
    if (lim < 0) {
        const char *e = getenv("DM_NVN_CUT_BUSY");
        lim = e ? atoi(e) : 1;
        if (lim < 0) lim = 1;
        if (lim != 1) MLOG("segmented submission: busy limit %d (DM_NVN_CUT_BUSY; 0 = every cut commits, N = commit while fewer than N commits are in flight)", lim);
    }
    return lim;
}
static void sub_cut(void) {
    if (g_enc || !sub_cut_ready()) return;
    // The cut is there so that the GPU does not sit idle while the host encodes. With earlier work still running, queueing many command buffers triggers kernel stalls on Metal's one
    // submission queue (docs/reference/frame-rate.md). Live GPU progress is read from g_q4's shared event (rx_inflight) rather than addFeedbackHandler, which lags GPU completion by
    // 0.17–1.2 ms. On the encode thread (rec_on() && rx_merge_on()), the segment is still split here and held on the encode thread (REC_CMT_SEG) until rx_inflight() < lim, so submit-thread
    // lookahead cannot collapse the rest of the frame into one command buffer. A copy still commits immediately (REC_CMT_NOW), as its data must land. DM_NVN_CUT_BUSY=0: every cut commits;
    // DM_NVN_CUT_BUSY=N: commit while fewer than N commits are in flight (default 1: only when earlier work on g_q4 has finished)
    int lim = cut_busy_limit();
    int can_hold = lim > 0 && !g_cut_copy && (!seg_ts() || ts_on());
    if (can_hold && (!rec_on() || !rx_merge_on()) && rx_inflight() >= (uint64_t)lim) {
        if (ts_on()) { ts_boundary(); g_seg++; }
        g_cut_draw0 = g_stat_draw; return; }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    if (ts_on()) ts_boundary();
    if (g_cut_copy) g_copy_cuts++;
    g_cut_copy = 0;
    cb_seal();   // before the segment's state list is taken
    uint64_t f = atomic_load(&g_cur_frame); gpu_tag_begin(f);
    uint32_t k = g_seg++; seg_clk *sc = NULL;   // reports encoded so far keep this segment's GPU times (seg_ts)
    if (seg_ts()) { if (!g_sclk) { g_sclk = calloc(1, sizeof *g_sclk); if (!g_sclk) MDIE("segment clock allocation failed"); atomic_store(&g_sclk->refs, 1); } sc = g_sclk; atomic_fetch_add(&sc->refs, 1); if (k < SEG_CLK_N) sc->cm[sc->ncm++] = (uint16_t)k; }   // this command buffer ends with segment k (seg_idle_before, sub_place)
    cbs_list *cbs = cbs_take(); uint64_t cbs_fence = g_fence + 1; cbs_fly_put(cbs_fence, cbs);
    DmCommitFB *o = [DmCommitFB new];
    atomic_fetch_add(&g_seg_inflight, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> cb) {
        atomic_fetch_sub(&g_seg_inflight, 1);
        gpu_tag_done(f, cb.GPUStartTime, cb.GPUEndTime);
        if (sc) { if (k < SEG_CLK_N) { atomic_store(&sc->t[k][0], gpu_s_guest_ns(cb.GPUStartTime)); atomic_store(&sc->t[k][1], gpu_s_guest_ns(cb.GPUEndTime)); } seg_clk_put(sc); }
        if (cb.error) { atomic_store(&g_last_commit_error, 1); static int k; if (k++ < 8) MLOG("[RED] segment command buffer execution error: %s", cb.error.description.UTF8String); res_top_once(cb.error); }
        char where[64]; snprintf(where, sizeof where, "fence %llu, segment %u", (unsigned long long)cbs_fence, k); cbs_feedback(cbs, cb.error, where);
        gpu_lost_check(cb.error, "segment");
    }];
    cb_commit(o, (can_hold && rec_on() && rx_merge_on()) ? REC_CMT_SEG : REC_CMT_NOW);
    if (rec_on()) rec_publish();   // the segment reaches the encode thread now, not when its block of records fills
    g_cb = rec_cb_new();
    [g_cb beginCommandBufferWithAllocator:g_alloc4[g_alloc_cur]];   // one allocator encodes several command buffers in sequence; it is reset only once its fence (the last segment) has passed, when a later submission starts
    g_cut_draw0 = g_stat_draw;
    if (!g_cuts++) MLOG("segmented submission: first segment submitted at frame %llu pass %llu", (unsigned long long)f, (unsigned long long)g_stat_pass);
}
// A long render pass is one segment: Breath of the Wild draws 2,700 of its 4,100 draws a frame into the G-buffer, and the GPU, done with the shadow passes before it,
// waited for the whole pass to be translated (about 2.5 ms of the 9 ms frame at 120 fps on the Mac). With no segment left on the GPU and DM_NVN_PASS_SPLIT draws (default 256,
// 0 = off) encoded since the last cut, the pass is ended (its attachments stored), the segment committed, and the next draw opens the pass again, loading them. Each split
// costs a store and a load of the attachments, so it is taken only while the GPU would otherwise wait
// A split is taken only when sub_cut will commit after it (sub_cut_ready): otherwise the pass would be
// stored and loaded for nothing (DM_NVN_CUT=0, or above DM_NVN_PASS_SPLIT)
static uint64_t g_pass_splits, g_split_pass; static int g_split_n;   // g_split_n: splits of the pass g_split_pass so far
static int pass_split_draws(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_PASS_SPLIT"); v = e ? atoi(e) : 256; } return v; }
static int pass_split_max(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_PASS_SPLIT_MAX"); v = e ? atoi(e) : 2; } return v; }   // per NVN pass: each split stores and loads its attachments
static void pass_split_try(void) {
    int n = pass_split_draws();
    if (n <= 0 || !g_enc || g_stat_draw - g_cut_draw0 < (uint64_t)n || rx_inflight() > 0 || !sub_cut_ready()) return;
    if (g_sm_pass) return;
    if (g_split_pass != g_setrt_n) { g_split_pass = g_setrt_n; g_split_n = 0; }
    if (pass_split_max() > 0 && g_split_n >= pass_split_max()) return;
    g_split_n++;
    rp_end();
    sub_cut(); g_split_resume = 1;
    if (!g_pass_splits++) MLOG("pass split: a long render pass was ended and its segment committed while the GPU had nothing to do (DM_NVN_PASS_SPLIT=%d, frame %llu pass %llu)", n, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
}
uint64_t mtl_cut_count(void) { return g_cuts; }
uint64_t mtl_copy_cut_count(void) { return g_copy_cuts; }

// ---------- Clears a load action cannot express ----------
// A colour clear issued inside an open render pass, with a partial channel mask or with a scissor smaller than the target, and a depth / stencil clear with a scissor inside an
// open pass (Tears of the Kingdom issues all of them) are drawn as one triangle over the whole pass in the render encoder. It writes the clear value into the masked channels of
// that attachment, or the depth / the stencil bits of the clear's write mask, inside the scissor, with the depth test ALWAYS and no visibility counting. The value is read from
// the frame arena through the triangle's own argument tables (the draws' g_vt / g_ft are put back after it), so the pipelines depend only on the pass formats, the slot and the
// mask. Clears the load action expresses keep it. Adapted from MacNativeGames' clear_quad / clear_ds_quad (github.com/ly05010419/MacNativeGames, MIT).
static int pf_int_kind(MTLPixelFormat f) {   // the fragment output type a colour attachment takes: 0 float (also normalized), 1 uint, 2 int
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint: return 1;
        case MTLPixelFormatR8Sint: case MTLPixelFormatR16Sint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG32Sint:
        case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Sint: return 2;
        default: return 0;
    }
}
static id<MTLLibrary> clr_lib(void) {   // clr_vs (the triangle, depth from buffer 0) and clr_fs_<slot>_<float|uint|int> (that colour slot's value from buffer 0): dm/gfx/msl/clear.metal
    static id<MTLLibrary> lib; static int failed;
    if (lib || failed) return lib;
    NSError *e = nil; lib = host_lib("clear", &e);
    if (!lib) { failed = 1; MLOG("[RED] clear-triangle shaders: no library: %s", e.localizedDescription.UTF8String); }
    return lib;
}
typedef struct { int32_t slot, kind, mask; uint32_t cfmt[8], dfmt, sfmt, samples; } clr_key;   // slot -1: a depth / stencil clear (no fragment function)
static id<MTLRenderPipelineState> clr_pso(const clr_key *k) {
    static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *cache; if (!cache) cache = [NSMutableDictionary dictionary];
    NSData *kd = [NSData dataWithBytes:k length:sizeof *k]; id<MTLRenderPipelineState> ps = cache[kd]; if (ps) return ps;
    id<MTLLibrary> lib = clr_lib(); if (!lib) return nil;
    static const char *ty[3] = {"float", "uint", "int"};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"clr_vs"]; if (k->slot >= 0) d.fragmentFunction = [lib newFunctionWithName:[NSString stringWithFormat:@"clr_fs_%d_%s", k->slot, ty[k->kind]]];
    for (int i = 0; i < 8; i++) if (k->cfmt[i]) { d.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->cfmt[i]; d.colorAttachments[i].writeMask = MTLColorWriteMaskNone; }   // the other attachments stay as they are
    if (k->slot >= 0) d.colorAttachments[k->slot].writeMask = (MTLColorWriteMask)((k->mask & 1 ? MTLColorWriteMaskRed : 0) | (k->mask & 2 ? MTLColorWriteMaskGreen : 0) | (k->mask & 4 ? MTLColorWriteMaskBlue : 0) | (k->mask & 8 ? MTLColorWriteMaskAlpha : 0));
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt; d.rasterSampleCount = k->samples ? k->samples : 1;
    NSError *e = nil; ps = dev_rps(d, &e);
    if (!ps) { static int n; if (n++ < 4) MLOG("[RED] cannot create the clear-triangle pipeline (slot %d color format %u depth %u stencil %u samples %u): %s; this clear is skipped", k->slot, k->slot >= 0 ? k->cfmt[k->slot] : 0, k->dfmt, k->sfmt, k->samples, e.localizedDescription.UTF8String); return nil; }
    cache[kd] = ps; return ps;
}
static id<MTLDepthStencilState> clr_dss(int dw, unsigned sm) {   // depth ALWAYS, written when dw; stencil ALWAYS → REPLACE through write mask sm (no stencil test when 0)
    static id<MTLDepthStencilState> c[2][256];
    if (c[dw][sm]) return c[dw][sm];
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = dw != 0;
    if (sm) { MTLStencilDescriptor *st = [MTLStencilDescriptor new]; st.stencilCompareFunction = MTLCompareFunctionAlways; st.stencilFailureOperation = st.depthFailureOperation = MTLStencilOperationKeep;
        st.depthStencilPassOperation = MTLStencilOperationReplace; st.readMask = 0xff; st.writeMask = sm; d.frontFaceStencil = d.backFaceStencil = st; }
    return c[dw][sm] = [g_dev newDepthStencilStateWithDescriptor:d];
}
static int clr_scissor(MTLScissorRect *sc) {   // the clear's scissor in the open pass, clamped as for draws; 0 = empty
    NSUInteger tw = g_pass.tw, th = g_pass.th; *sc = (MTLScissorRect){0, 0, tw, th};
    if (!R.has_sc) return tw && th;
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > (long)tw) x1 = (long)tw; if (y1 > (long)th) y1 = (long)th;
    if (x1 <= x0 || y1 <= y0) return 0;
    *sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)}; return 1;
}
static void clr_tri(id<MTL4RenderCommandEncoder> en, id<MTLRenderPipelineState> ps, id<MTLDepthStencilState> ds, MTLScissorRect sc, const uint32_t val[4], float z, uint32_t sref, NSUInteger tw, NSUInteger th) {
    static id<MTL4ArgumentTable> vt, ft;
    if (!ft) { NSError *e = nil; MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1;
        vt = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]); ft = vt ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!ft) { vt = nil; static int n; if (!n++) MLOG("[RED] cannot create the clear-triangle argument table: %s; this clear is skipped", e.localizedDescription.UTF8String); return; } }
    id<MTLBuffer> b; size_t o = arena_alloc(32, &b); uint8_t *p = (uint8_t *)b.contents + o; memcpy(p, val, 16); memcpy(p + 16, &z, 4);   // colour at 0, depth at 16
    at_addr(ft, b.gpuAddress + o, 0, "clear table slot", 0); at_addr(vt, b.gpuAddress + o + 16, 0, "clear table slot", 0);
    [en setArgumentTable:vt atStages:MTLRenderStageVertex]; [en setArgumentTable:ft atStages:MTLRenderStageFragment];
    enc_r_pso(en, ps); g_enc_pso = nil; enc_r_dss(en, ds); g_enc_dss = nil;   // the next draw sets its own again
    if (g_enc_vis) { enc_r_visibility(en, MTLVisibilityResultModeDisabled, 0); g_enc_vis = 0; }   // not counted by an occlusion query; the next counted draw turns counting back on
    enc_r_viewport(en, (MTLViewport){0, 0, (double)tw, (double)th, 0, 1}); enc_r_scissor(en, sc);
    enc_r_cull(en, MTLCullModeNone); enc_r_fill(en, MTLTriangleFillModeFill); enc_r_depth_bias(en, 0, 0, 0); enc_r_depth_clip(en, YES); enc_r_stencil_ref(en, sref, sref); g_ed.ok = 0;
    enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3);
    [en setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [en setArgumentTable:g_ft atStages:MTLRenderStageFragment];
}
static void clear_tex_tri(id<MTLTexture> t, unsigned lv, unsigned l0, const p_clear_tex *c) {
    int mask = c->mask & mtl_pf_chmask(t.pixelFormat); if (!mask) return;
    NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
    long x0 = MAX(c->r.xoffset, 0), y0 = MAX(c->r.yoffset, 0);
    long x1 = MIN((long)c->r.xoffset + c->r.width, (long)w), y1 = MIN((long)c->r.yoffset + c->r.height, (long)h);
    if (x1 <= x0 || y1 <= y0) return;
    int kind = pf_int_kind(t.pixelFormat);
    clr_key k = {0}; k.kind = kind; k.mask = mask; k.cfmt[0] = t.pixelFormat; k.samples = t.sampleCount;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(0, 0); if (!ps || !ds) return;
    uint32_t v[4];
    for (int i = 0; i < 4; i++) {
        if (c->is_uint && kind) { v[i] = c->c.u[i]; continue; }
        float f = c->is_uint ? (float)c->c.u[i] : c->c.f[i]; int32_t si;
        if (kind == 1) v[i] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
        else if (kind == 2) { si = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f; memcpy(&v[i], &si, 4); }
        else memcpy(&v[i], &f, 4);
    }
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0, nz = c->r.depth ? c->r.depth : 1; z < nz; z++) {
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        rp.colorAttachments[0].texture = t; rp.colorAttachments[0].level = lv;
        if (t.textureType == MTLTextureType3D) rp.colorAttachments[0].depthPlane = c->r.zoffset + z;
        else rp.colorAttachments[0].slice = l0 + c->r.zoffset + z;
        rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTL4RenderCommandEncoder> en = enc_r_begin(rp);
        clr_tri(en, ps, ds, (MTLScissorRect){x0,y0,x1-x0,y1-y0}, v, 0, 0, w, h); enc_r_end(en);
    }
}
static void clr_note(int slot, int depth, int stencil) {   // as udc_note_draw for a draw: the attachment was written (unwritten-attachment store)
    for (int i = 0; i < g_nsd; i++) { sd_att *t = &g_sd[i];
        if ((t->depth == 0 && slot >= 0 && t->idx == slot) || (t->depth == 1 && depth) || (t->depth == 2 && stencil)) { t->wrote = t->rast = 1; t->wwhy = 2; } }
}
static uint64_t g_clr_tri[2];   // clears drawn as a triangle: colour, depth / stencil
static void clear_color_tri(const p_clear_color *c) {
    int slot = c->index, mask = c->mask & 0xf;
    if (!mask || slot < 0 || slot >= 8 || slot >= R.rt.n || !R.rt.color[slot]) return;
    id<MTL4RenderCommandEncoder> en = rp_begin();   // a clear outside a pass opens it as a draw would (pending clears land first)
    if (en && !g_pass.cfmt[slot] && g_enc_mode) { rp_end(); en = rp_begin(); }   // opened without this attachment (a clear-only pass): reopen with all of them
    if (!en) return;
    int at = slot;
    if (!g_pass.cfmt[at] && rt_dup_slot(&R.rt, slot))   // a slot attached twice is attached only once (rt_dup_slot): clear the first one
        for (int j = 0; j < slot; j++) { const tex_t *a = R.rt.color[j], *b = R.rt.color[slot]; if (!a || a->serial != b->serial || !g_pass.cfmt[j]) continue;
            const view_t *va = &R.rt.cview[j], *vb = &R.rt.cview[slot]; int ha = R.rt.has_cview[j], hb = R.rt.has_cview[slot];
            if ((ha && va->set_levels ? va->base_level : 0) == (hb && vb->set_levels ? vb->base_level : 0) && (ha && va->set_layers ? va->min_layer : 0) == (hb && vb->set_layers ? vb->min_layer : 0)) { at = j; break; } }
    if (!g_pass.cfmt[at]) { PD("c %llu color-clear triangle skipped: slot %d is not in this pass", g_stat_pass, slot); return; }
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu color-clear triangle skipped: empty scissor slot %d", g_stat_pass, slot); return; }
    int kind = pf_int_kind((MTLPixelFormat)g_pass.cfmt[at]);
    clr_key k; memset(&k, 0, sizeof k); k.slot = at; k.kind = kind; k.mask = mask; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(0, 0); if (!ps || !ds) return;
    uint32_t v[4];
    for (int i = 0; i < 4; i++) { float f = c->rgba[i]; int32_t si;   // integer attachments take the value converted, as a load-action clear does
        if (kind == 1) v[i] = f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
        else if (kind == 2) { si = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f; memcpy(&v[i], &si, 4); }
        else memcpy(&v[i], &f, 4); }
    clr_tri(en, ps, ds, sc, v, 0, 0, g_pass.tw, g_pass.th); clr_note(at, 0, 0);
    if (!g_clr_tri[0]++) MLOG("mid-pass / partial-mask / partial-scissor color clear: drawn as a full-pass triangle (first case pass %llu slot %d mask %#x scissor %lu,%lu %lux%lu)", g_stat_pass, at, mask,
                              (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
    PD("c %llu color clear drawn as a triangle slot%d ser=%llu mask=%#x scissor=%lu,%lu,%lu,%lu color=%g,%g,%g,%g", g_stat_pass, at, (unsigned long long)R.rt.color[at]->serial, mask,
       (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
}
static void clear_ds_tri(const p_clear_ds *c) {
    unsigned want_s = (unsigned)c->stencil_mask & 0xff;
    if (!c->depth_mask && !want_s) return;
    if (g_enc) {
        fb_pass_sync();
        fb_ds_before_write((c->depth_mask && g_pass.hd ? FB_DS_DEPTH : 0) | (want_s && g_pass.hs ? FB_DS_STENCIL : 0));
    }
    id<MTL4RenderCommandEncoder> en = rp_begin();
    if (en && g_enc_mode && ((c->depth_mask && !g_pass.hd) || (want_s && !g_pass.hs))) { rp_end(); en = rp_begin(); }   // opened without the depth or stencil plane (a clear-only pass): reopen with all attachments
    if (!en) return;
    int dw = c->depth_mask && g_pass.hd; unsigned sm = g_pass.hs ? want_s : 0;
    if (!dw && !sm) return;
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu depth-stencil clear triangle skipped: empty scissor", g_stat_pass); return; }
    clr_key k; memset(&k, 0, sizeof k); k.slot = -1; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(dw, sm); if (!ps || !ds) return;
    float z = c->depth > 0 ? (c->depth < 1 ? c->depth : 1) : 0;   // as clearDepth: within [0, 1] (NaN → 0)
    const uint32_t v[4] = {0};
    clr_tri(en, ps, ds, sc, v, z, (uint32_t)c->stencil & 0xff, g_pass.tw, g_pass.th); clr_note(-1, dw, sm != 0);
    fb_ds_note_write((dw ? FB_DS_DEPTH : 0) | (sm ? FB_DS_STENCIL : 0));
    if (!g_clr_tri[1]++) MLOG("mid-pass scissored depth-stencil clear: drawn as a full-pass triangle (first case pass %llu depth %d stencil mask %#x scissor %lu,%lu %lux%lu)", g_stat_pass, dw, sm,
                              (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
    PD("c %llu depth-stencil clear drawn as a triangle ser=%llu depth=%g(%d) stencil=%d mask=%#x scissor=%lu,%lu,%lu,%lu", g_stat_pass, R.rt.depth ? (unsigned long long)R.rt.depth->serial : 0ull, z, dw, c->stencil, sm,
       (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height);
}

static int unb_keeps(uint32_t op) {   // state an open pass carries over to the targets set again (see g_unb)
    switch (op) {
        case OP_SET_RT: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR:
        case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_DEPTH_CLAMP: case OP_LINE_WIDTH: case OP_VP_SWIZZLE: case OP_PRIM_RESTART: case OP_RAST_DISCARD: case OP_SET_TEX_POOL:
        case OP_SET_SAMP_POOL: case OP_JUMP: case OP_CALL: return 1;
        default: return 0;
    }
}

// A run of buffer copies ends (see b2b_note): cut 1 commits what was encoded so far, then the copied pages are stamped
static void b2b_flush(int cut) {
    if (!g_b2b_n) return;
    if (cut) { uint64_t gen = g_cbgen; if (!g_enc) { if (g_blit) { enc_b_end(g_blit); g_blit = nil; } g_cut_copy = 1; sub_cut(); } if (g_cbgen != gen) g_b2b_cuts++; else g_b2b_nocut++; }
    for (int i = 0; i < g_b2b_n; i++) { nvn_dirty_gpu_written(g_b2b_lo[i], g_b2b_hi[i] - g_b2b_lo[i]); g_b2b_bytes += g_b2b_hi[i] - g_b2b_lo[i]; }
    if (++g_b2b_runs <= 3 || g_b2b_runs % 500 == 0) MLOG("buffer copy runs %llu: committed before the next command %llu, not committable there %llu (an encoder open or a pass held back), %.1f MB stamped%s",
        (unsigned long long)g_b2b_runs, (unsigned long long)g_b2b_cuts, (unsigned long long)g_b2b_nocut, g_b2b_bytes / 1048576.0, g_b2b_over ? ", range list overflowed (merged)" : "");
    g_b2b_n = 0; g_b2b_over = 0;
}
void mtl_exec(const uint8_t *p) {
    if (!g_state_inited) state_reset();
    static int dump_all = -1; if (dump_all < 0) dump_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
    int trace = dump_all && passdump();   // Normal mode does not check the frame range per operation; the full command position is only used for the designated diagnostic frames
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        g_exec_cur = p;
        if (trace) PD("o cmd=%p pass=%llu op=%u size=%u", p, g_stat_pass, (unsigned)h.op, (unsigned)h.size);
        // Diagnostic DM_NVN_ALLOCPROF=1: growth of the GPU allocated size is charged to the previous operation's type (rough: allocations on other threads are counted too); the memory report line appends the top 4
        allocprof_level();
        if (g_allocprof == 2) { static int prev = -1; uint64_t now = g_alloc4[g_alloc_cur].allocatedSize; if (prev >= 0 && prev < 64 && now > g_cm_last) g_cm_op[prev] += now - g_cm_last; g_cm_last = now; prev = (int)h.op; if (h.op < 64) g_cm_opn[h.op]++; }
        if (g_allocprof) { static uint64_t last; static int prev = -1; uint64_t now = g_dev.currentAllocatedSize; if (prev >= 0 && prev < 64 && now > last) g_ap_bytes[prev] += now - last; last = now; prev = h.op; }
        const void *a = p + 8;
        if (g_unb && !unb_keeps(h.op) && !(h.op == OP_END && g_ret_n)) { PD("M %llu unbound targets land before op %u", g_stat_pass, (unsigned)h.op); rp_flush(); }   // see g_unb
        if (g_nlc && !lc_keeps(h.op) && !(h.op == OP_END && g_ret_n)) lc_land();   // see lc_keeps (a sub-list's end returns to its caller)
        if (g_b2b_n && h.op != OP_COPY_B2B && !(h.op == OP_END && g_ret_n)) b2b_flush(h.op != OP_END || g_nrest);   // a run of buffer copies ends (b2b_note); a handle's end with more to replay (nvn_replay) is not the commit
        switch (h.op) {
            case OP_END: g_exec_cur = NULL;
                return;   // An end-of-pass triggered outside a list (present / CPU readback etc.) must not scan expired command memory: a null start point means store everything
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (g_ret_n < 16) g_ret[g_ret_n] = p + h.size; g_ret_n++;   // Return stack for lookahead: after a sub-list ends, continue looking at the caller
                mtl_exec((const uint8_t *)t); g_ret_n--; break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                PD("Y signal_event cmd=%p dst=%p mode=%u value=%u", p, val, m, v); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8);
                PD("Q report type=%llu cmd=%p dst=%#llx", (unsigned long long)t, p, (unsigned long long)ad);
                rc_note(ad, t == 0 || t == 0x10 ? 1 : t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 2 : 3);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                    if (!g_vis_on) {   // a report with no reset before it: the counter runs from the queue's creation, so counting starts here (Occlusion queries)
                        g_vis_on = 1; g_vis_counting = 1; MLOG("occlusion queries: samples passed reported with no ResetCounter before it: counting from this report");
                        if (g_enc && !g_enc_has_vis) { PD("M %llu occlusion query counting starts at a report: render pass split here", g_stat_pass); rp_end(); } }
                    hw_note(ad); post((post_op){3, NULL, VIS_REPORT, 0, ad, (uint32_t)t, vis_snapshot()}); vis_clear(); vis_need_scan(p + h.size); }   // written at completion, as a counter report
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RENDER_COND: { const p_render_cond *rc = a;   // SetRenderEnable / SetRenderEnableConditional (hle_nvn.c)
                if (rc->mode >= 2) { g_rc_skip = rc->mode == 3; PD("E %llu render enable %d", g_stat_pass, rc->mode == 2); }
                else { uint64_t x = 0, y = 0; int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y);
                    g_rc_skip = known && (rc->mode ? x == y : x != y);
                    PD("E %llu conditional render %s addr=%#llx values %llu / %llu%s%s", g_stat_pass, rc->mode ? "draw if not equal" : "draw if equal", (unsigned long long)rc->addr, (unsigned long long)x, (unsigned long long)y, known ? "" : " (unknown, drawn)", g_rc_skip ? " → not drawn" : ""); }
                break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1;   // the fence value this submission (segment) commits with
                PD("Y fence_sync cmd=%p sync=%p fence=%llu", p, (void *)s, (unsigned long long)s->fence); break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); PD("Q reset type=%llu cmd=%p", (unsigned long long)t, p);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                    g_vis_on = 1; g_vis_counting = 1; vis_clear(); post((post_op){3, NULL, VIS_ZERO, 0, 0, (uint32_t)t, NULL}); vis_need_scan(p + h.size);
                    // The first reset can come inside a render pass that was begun without a visibility buffer (Octopath Traveler 0: Unreal's occlusion queries, once at boot): split the pass
                    // here, so the continuation is begun with the buffer and its samples are counted (they used not to be: RED "occlusion query counting began in a pass without a visibility buffer")
                    if (g_enc && !g_enc_has_vis) { PD("M %llu occlusion query counting starts: render pass split here", g_stat_pass); rp_end(); } }
                break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
                PD("Y copy_b2b cmd=%p src=%#llx dst=%#llx bytes=%llu", p, (unsigned long long)s_, (unsigned long long)d, (unsigned long long)n); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                if (p == g_mdx_stop) { g_mdx_epoch++; g_mdx_stop = NULL; }   // it writes a record or count that masked copies were made of (mdx_run)
                PD("Y update_ub cmd=%p src=%#llx dst=%#llx bytes=%llu f=%g,%g,%g,%g", p, (unsigned long long)((const uint8_t *)a + 16), (unsigned long long)d, (unsigned long long)n,
                   n >= 4 ? ((const float *)((const uint8_t *)a + 16))[0] : 0.f, n >= 8 ? ((const float *)((const uint8_t *)a + 16))[1] : 0.f, n >= 12 ? ((const float *)((const uint8_t *)a + 16))[2] : 0.f, n >= 16 ? ((const float *)((const uint8_t *)a + 16))[3] : 0.f); mtl_sub_update((const uint8_t *)a + 16, d, n); break; }
            case OP_SET_RT: {
                const p_set_rt *r = a; g_setrt_n++; g_split_resume = 0;   // a split's resume is for its own pass only
                int pend = R.clear_d || R.clear_s; for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                int empty = !r->depth; for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) empty = 0;
                PD("M %llu set_rt n=%d depth=%d empty=%d enc=%d", g_stat_pass, r->n, !!r->depth, empty, !!g_enc);
                if (empty && g_enc && g_enc_mode != 1 && !pend && R.has_rt && rtc_on() && unb_on()) { g_unb = 1; g_unb_rt = *r;
                    if (!g_unb_n++) MLOG("deferred unbind: first case pass %llu", g_stat_pass); break; }
                if (g_unb) g_unb = 0;   // targets again: compared with the open pass's below
                // Do not continue a render pass opened only for a clear; one with fewer depth / stencil attachments can be continued (check the planes before drawing)
                if (g_enc && R.has_rt && !empty) {
                    int same = rt_same(&R.rt, r), can_continue = g_enc_mode != 1 && !pend && rtc_on();
                    if (same && can_continue) { g_rt_merged = 1; if (!g_rt_merge_n++) MLOG("same-target continuation: first case pass %llu", g_stat_pass); PD("M %llu same-target continuation pass", g_stat_pass); R.rt = *r; break; }
                    if (!same && can_continue) { static int nd; if (nd++ < 8) MLOG("same-target continuation refused (%s) pass %llu", g_rt_diff ?: "?", g_stat_pass); PD("M %llu new targets (%s)", g_stat_pass, g_rt_diff ?: "?"); }
                    if (same) { static int nw; if (nw++ < 8) MLOG("same targets, no continuation: enc_mode=%d pend=%d rtc=%d pass %llu", g_enc_mode, pend, rtc_on(), g_stat_pass); }
                }
                if (g_enc && g_sm_pass && !pend && !g_game_barrier) { int sl;   // another layer of the slice-merged pass's level (sm_scan planned it)
                    if (sm_rt_slice(&R.rt, r, &sl) && sl >= g_sm_lo && sl <= g_sm_hi) { R.rt = *r; g_sm_cur = sl; g_sm_ncont++;
                        { void td_note_layer(id<MTLTexture>, uint64_t, NSUInteger, NSUInteger); td_note_layer(tex_mtl_replay(r->color[0]), r->color[0]->serial, r->cview[0].set_levels ? r->cview[0].base_level : 0, (NSUInteger)sl); } PD("M %llu slice-merged pass goes on in layer %d", g_stat_pass, sl); break; } }
                if (pend && !g_enc && g_cb && lc_on()) lc_push(); else rp_flush();   // clears with no draw wait for the next pass that attaches their targets (lc_push)
                sub_cut(); R.rt = *r; R.has_rt = 1; break;
            }
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = a;
                if (g_rc_skip) { PD("c %llu color clear skipped by conditional rendering slot%d", g_stat_pass, c->index); break; }
                // A scissored clear to 0 outside a pass with all channels (SMBW clears pyramid levels as sub-rectangles of 480×272 and other targets): copy from the zero buffer into the scissor rectangle in the replay stream
                if (!g_enc && c->mask == 0xf && !c->rgba[0] && !c->rgba[1] && !c->rgba[2] && !c->rgba[3] && R.has_sc && c->index >= 0 && c->index < R.rt.n && R.rt.color[c->index]
                    && !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index]) && clear_rect_zero(c->index)) break;
                if (g_rt_merged || g_sm_pass || g_fsp_pass) rp_end();   // A clear arrived in a continued render pass: end it here, and the clear is attached to the start of the next render pass as usual
                if (c->index < 0 || c->index >= R.rt.n || !R.rt.color[c->index]) { RED_ONCE(5, "color clear target %d invalid", c->index); break; }
                if (g_enc || c->mask != 0xf || !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index])) {
                    // Inside a pass, with a partial mask or a partial scissor: drawn as a triangle (clear_color_tri)
                    static uint64_t n; static int clog = -1; if (clog < 0) clog = getenv("DM_NVN_CLEARLOG") != NULL; if (clog && n++ < 40) MLOG("color clear drawn as a triangle frame %llu pass %llu target %d ser=%llu %ux%u mid=%d mask=%#x scissor=%d,%d,%d,%d color=%g,%g,%g,%g", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, c->index,
                        (unsigned long long)R.rt.color[c->index]->serial, R.rt.color[c->index]->d.width, R.rt.color[c->index]->d.height, g_enc != nil, c->mask, R.sc.x, R.sc.y, R.sc.w, R.sc.h, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                    clear_color_tri(c); break; }
                R.clear_c[c->index] = 1; R.cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                PD("c %llu color clear recorded slot%d ser=%llu mask=%#x scissor=%d:%d,%d,%d,%d", g_stat_pass, c->index, (unsigned long long)R.rt.color[c->index]->serial, c->mask, R.has_sc, R.sc.x, R.sc.y, R.sc.w, R.sc.h);
                break;
            }
            case OP_CLEAR_DS: {
                const p_clear_ds *c = a;
                if (g_rc_skip) { PD("c %llu depth-stencil clear skipped by conditional rendering", g_stat_pass); break; }
                if (g_rt_merged || g_sm_pass || g_fsp_pass) rp_end();
                // A load-action clear covers the whole attachment and every stencil bit. A clear whose scissor does not cover the depth target, or whose stencil mask is partial, is drawn as a triangle
                // (clear_ds_tri), in or before a pass: before the first draw and for a partial stencil mask in a pass it used to clear the whole target (PR #108)
                unsigned sm = (unsigned)c->stencil_mask & 0xff;
                // A partial stencil mask with no pass open and a depth clear over the whole target: only the stencil needs the triangle, and the depth half is the pass's load action, so
                // the depth plane is not loaded only to be cleared (Donkey Kong Bananza: 22.8 MB of depth and stencil loaded a frame for a stencil mask of 1, 4.6 MB this way)
                if (R.rt.depth && !g_enc && c->depth_mask && sm && sm != 0xff && scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview)) {
                    R.clear_d = 1; R.cd = c->depth; p_clear_ds s = *c; s.depth_mask = 0; clear_ds_tri(&s); break;
                }
                if (R.rt.depth && (!scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview) || (sm && sm != 0xff))) { clear_ds_tri(c); break; }
                // A full clear in the middle of a pass: end the pass and attach the clear to the start of the next one (color attachments load as usual, as a continued pass does above when a clear arrives).
                // It used to be skipped, and later draws depth-tested against stale depth
                if (g_enc) {
                    { static int once; if (!once++) MLOG("mid-pass depth-stencil clear: ending this pass; the clear moves to the start of the next pass (first case pass %llu)", g_stat_pass); }
                    rp_end();
                }
                if (c->depth_mask) { R.clear_d = 1; R.cd = c->depth; }
                if (c->stencil_mask) { R.clear_s = 1; R.cs = (uint32_t)c->stencil; }
                PD("c %llu depth-stencil clear recorded ser=%llu depth=%g depth_mask=%d stencil=%d stencil_mask=%#x", g_stat_pass, R.rt.depth ? (unsigned long long)R.rt.depth->serial : 0ull, c->depth, c->depth_mask, c->stencil, c->stencil_mask);
                break;
            }
            case OP_MULTI_DRAW:
                // A lookahead from outside the range stops at its boundary, including scans for inline writes to the arguments.
                g_mdx_epoch++; g_mdx_stop = NULL;
                if (!g_rc_skip) draw_multi(a);
                g_mdx_epoch++;
                break;
            case OP_DRAW: { if (g_rc_skip) break; if (g_fsm_ntaken && fsm_take(p)) { PD("L %llu face-split draw already drawn in an earlier pass (DM_NVN_FACE_MERGE)", g_stat_pass); break; }
                p_draw t; g_exec_cur = p; g_exec_next = p + h.size; draw(draw_rec(a, h.size, &t), NULL); g_exec_next = NULL; break; }   // Lookahead start point
            case OP_DRAW_IDX: { if (g_rc_skip) break; if (g_fsm_ntaken && fsm_take(p)) { PD("L %llu face-split draw already drawn in an earlier pass (DM_NVN_FACE_MERGE)", g_stat_pass); break; }
                p_draw_idx t; g_exec_cur = p; g_exec_next = p + h.size; draw(NULL, draw_idx_rec(a, h.size, &t)); g_exec_next = NULL; break; }
            // Do not mark dirty if the state did not change (the game often re-sends the same program / state block / vertex format before each draw; the dirty flag only decides whether to re-fetch the pipeline / depth-stencil object, and equal values mean the same object)
            case OP_BIND_PROGRAM: { const p_bind_program *b = a; state_op(h.op,a);
                if (passdump()) { NvProg *pr = S.prog ? prog_get(S.prog) : nil;
                    PD("Y bind_program cmd=%p program=%p stages=%#x vs=%s fs=%s",p,(void *)b->prog,b->stages,
                       pr && pr->sh[0] ? pr->sh[0]->key : "-", pr && pr->sh[1] ? pr->sh[1]->key : "-"); }
                break; }
            case OP_SCISSOR: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
            case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_LINE_WIDTH: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_COPY_STRIDE:
                state_op(h.op, a); break;
            // PASSDUMP O lines: the viewports the game sets, with the pass they fall in (one set before its pass opens takes that pass's number, as the P lines do). The D line
            // prints the one a draw used, so a viewport the pass set is told from one carried over. Printed here, not in state_op: look-ahead scans replay state_op too
            case OP_VIEWPORTS: { const p_viewports *b = a; state_op(h.op, a);
                if (passdump()) { unsigned long long pn = g_enc ? g_stat_pass : g_stat_pass + 1; PD("O %llu set_viewports first=%d n=%d", pn, b->first, b->n);
                    for (int i = 0; i < b->n && i < 4; i++) PD("O %llu   vp[%d] = %.0f,%.0f,%.0f,%.0f", pn, b->first + i, b->v[i][0], b->v[i][1], b->v[i][2], b->v[i][3]); }
                break; }
            // Numbers no entry records any more (transform feedback's buffers and state, SetPatchSize): the host has neither feature, and their NVN entries report in red when called
            case OP_TF_BIND_RETIRED: case OP_TF_STATE_RETIRED: case OP_PATCH_SIZE_RETIRED: { static int red; if (!red++) MLOG("[RED] command op %u (transform feedback or patch size) in the command stream: the host does not replay it", h.op); break; }
            case OP_PRIM_RESTART: state_op(h.op, a); break;
            case OP_RAST_DISCARD: PD("Y rasterizer discard %d", ((const p_rast_discard *)a)->on); state_op(h.op, a); break;
            case OP_COPY_B2T: copy_b2t(a, h.size); break;
            case OP_COPY_T2B: copy_t2b(a, h.size); break;
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_DRAW_TEX: draw_tex(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DLSS: dlss_eval(a); break;
            case OP_DISPATCH: if (g_csh_n && csh_take(p)) { PD("H %llu dispatch already run ahead of the pass", g_stat_pass); break; }
                dispatch(a); break;
            case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4);   // ordered at the next draw pass (barrier_game); inside a pass with shader writes, the pass ends first (swrite_note); in one without, before its next write (prog_swrites)
                int order = g_enc && (bits & (NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT)), split = order && g_pass_swrite;
                PD("I %llu barrier bits %#x%s", g_stat_pass, bits, split ? ": ends the pass (shader writes)" : order ? " (render pass open; a later shader write is ordered after its draws)" : g_enc ? " (render pass open)" : "");
                if (split) { rp_end(); static int once; if (!once++) MLOG("a game barrier (bits %#x) after shader writes in a draw pass: the pass ends there (first case pass %llu)", bits, (unsigned long long)g_stat_pass); }
                else if (order) g_pass_order |= (bits & NVN_BARRIER_ORDER_PRIMITIVES_BIT ? 1 : 0) | (bits & (NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT) ? 2 : 0);
                if ((bits & GAME_BARRIER_FULL) && g_mdx_wr != ~0ull && g_mdx_wr != g_vbuf_write_epoch) g_mdx_epoch++;   // a draw may have written a record or count that was copied before it (g_mdx_wr)
                if (bits) { g_game_barrier |= bits; g_fb_bar++; g_dsp_game_bar = 1; } break; }   // g_fb_bar: snapshots taken before it are stale (see fb_prepare)
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: state_op(h.op, a); break;
            default: break;
        }
        p += h.size;
    }
}
