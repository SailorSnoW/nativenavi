// Clears, submission segments and pass splits, buffer-copy runs, and mtl_exec, the replay of a command stream.
static unsigned mtl_pf_chmask(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 1: return 0x8;                                                                                      // A8
        case 10: case 11: case 12: case 13: case 14: case 20: case 22: case 23: case 24: case 25: case 53: case 54: case 55: return 0x1;   // R8 / R16 / R32
        case 30: case 31: case 32: case 33: case 34: case 60: case 62: case 63: case 64: case 65: case 103: case 104: case 105: return 0x3;   // RG8 / RG16 / RG32
        case 40: case 92: case 93: return 0x7;                                                                   // B5G6R5 / RG11B10F / RGB9E5
    }
    return 0xf;
}
// ClearTexture: a private texture starts undefined and the game relies on the cleared value; a partial region or channel mask is drawn as a triangle.
static void clear_tex_tri(id<MTLTexture> t, unsigned lv, unsigned l0, const p_clear_tex *c);
static void clear_tex(const p_clear_tex *c) {
    id<MTLTexture> t = tex_mtl_all(c->tex); sparse_gpu_wrote(c->tex); tu_wrote(t);
    tex_t pair; int paired = t && bcp_before(c->tex, t, &pair);   // a compressed-alias pair: the cleared region goes into its other side after
    PD("X ser=%llu view format %#x region %d,%d,%d %dx%dx%d mask %#x %s %g,%g,%g,%g", c->tex ? c->tex->serial : 0ull, c->has_view && c->view.set_format ? c->view.format : -1, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, c->mask,
       c->is_uint ? "ui" : "f", c->is_uint ? (double)c->c.u[0] : c->c.f[0], c->is_uint ? (double)c->c.u[1] : c->c.f[1], c->is_uint ? (double)c->c.u[2] : c->c.f[2], c->is_uint ? (double)c->c.u[3] : c->c.f[3]);
    if (!t) return;
    unsigned lv = c->has_view && c->view.set_levels ? c->view.base_level : 0, l0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
    int is3d = t.textureType == MTLTextureType3D, nz = c->r.depth ? c->r.depth : 1;
    if (!(t.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(t.pixelFormat) && t.sampleCount == 1)
        t = tex_upgrade(c->tex, t, MTLTextureUsageRenderTarget, t.pixelFormat);
    if (c->has_view && c->view.set_format) {   // the clear value is in the view's format
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
            gsb_store(c->tex, whole, lv, (is3d ? 0 : l0) + (unsigned)c->r.zoffset, (unsigned)nz, &c->r);
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

static const char *cctx_snap(void) {
    static char b[320];
    static uint64_t p_pass, p_up, p_reup, p_ser;
    uint64_t up = 0, reup = 0;
    mtl_pkg_stats(&up, &reup);
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    snprintf(b, sizeof b, "frame %llu; since the last submission: passes +%llu, first texture uploads +%llu, re-uploads +%llu, serial table changes +%llu; deferred-release queue %lu",
             (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)(g_stat_pass - p_pass), (unsigned long long)(up - p_up), (unsigned long long)(reup - p_reup),
             (unsigned long long)(g_ser_gen - p_ser), nd);
    p_pass = g_stat_pass; p_up = up; p_reup = reup; p_ser = g_ser_gen;
    return b;
}
// the display link follows the game's present interval, so frame-counting logic is not pulled up to 60
__attribute__((weak)) int nvn_game_fps(void) { return 60; }   // hle_nvn.c provides the strong definition
int mtl_fps_target(void) {
    static int env = -2;
    if (env == -2) {
        const char *e = getenv("DM_FPS");
        env = e ? atoi(e) : -1;
        if (env != 30 && env != 60) env = -1;
        else MLOG("DM_FPS=%d: frame rate pinned by the test switch", env);
    }
    int g = nvn_game_fps();
    return env > 0 ? env : g == 30 || g == 120 ? g : 60;
}
// DM_NVN_CUT: every that many draws, the command buffer so far is committed between passes so the GPU runs while the host translates; the fence signal stays on the last segment.
// A buffer → texture copy also cuts: the Switch runs a submission's copies at once and a game may reuse the source.
static int cut_draws(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_CUT");
        v = e ? atoi(e) : 64;
        MLOG("segmented submission: a segment is submitted between passes every %d draws (DM_NVN_CUT, 0 = off)", v);
    }
    return v;
}
static uint64_t g_cut_draw0, g_cuts, g_copy_cuts;
static void sub_cut_begin(void) { g_cut_draw0 = g_stat_draw; }
// grace renewed while frames get cheaper: the time macOS's GPU clock takes to rise
static int probe_ramp_mode(void) { static int m = -1; if (m < 0) m = sw_mode("DM_FSTEP_PROBE_RAMP", 1); return m; }
int mtl_probe_ramp_on(void) { return sw_on(probe_ramp_mode()); }
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
    // Queued command buffers stall in the kernel on Metal's one submission queue (docs/reference/frame-rate.md): the encode thread holds the segment until rx_inflight() < lim.
    int lim = cut_busy_limit();
    int can_hold = lim > 0 && !g_cut_copy && (!seg_ts() || ts_on());
    if (can_hold && (!rec_on() || !rx_merge_on()) && rx_inflight() >= (uint64_t)lim) {
        if (ts_on()) { ts_boundary(); g_seg++; }
        g_cut_draw0 = g_stat_draw;
        return;
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    if (ts_on()) ts_boundary();
    if (g_cut_copy) g_copy_cuts++;
    g_cut_copy = 0;
    cb_seal();
    uint64_t f = atomic_load(&g_cur_frame); gpu_tag_begin(f);
    uint32_t k = g_seg++; seg_clk *sc = NULL;
    if (seg_ts()) {
        if (!g_sclk) {
            g_sclk = calloc(1, sizeof *g_sclk);
            if (!g_sclk) MDIE("segment clock allocation failed");
            atomic_store(&g_sclk->refs, 1);
        }
        sc = g_sclk; atomic_fetch_add(&sc->refs, 1);
        if (k < SEG_CLK_N) sc->cm[sc->ncm++] = (uint16_t)k;
    }
    cbs_list *cbs = cbs_take(); uint64_t cbs_fence = g_fence + 1; cbs_fly_put(cbs_fence, cbs);
    DmCommitFB *o = [DmCommitFB new];
    atomic_fetch_add(&g_seg_inflight, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> cb) {
        atomic_fetch_sub(&g_seg_inflight, 1);
        gpu_tag_done(f, cb.GPUStartTime, cb.GPUEndTime);
        if (sc) { if (k < SEG_CLK_N) { atomic_store(&sc->t[k][0], gpu_s_guest_ns(cb.GPUStartTime)); atomic_store(&sc->t[k][1], gpu_s_guest_ns(cb.GPUEndTime)); } seg_clk_put(sc); }
        if (cb.error) {
            atomic_store(&g_last_commit_error, 1);
            static int reds;
            if (reds++ < 8) MLOG("[RED] segment command buffer execution error: %s", cb.error.description.UTF8String);
            res_top_once(cb.error);
        }
        char where[64]; snprintf(where, sizeof where, "fence %llu, segment %u", (unsigned long long)cbs_fence, k); cbs_feedback(cbs, cb.error, where);
        gpu_lost_check(cb.error, "segment");
    }];
    cb_commit(o, (can_hold && rec_on() && rx_merge_on()) ? REC_CMT_SEG : REC_CMT_NOW);
    if (rec_on()) rec_publish();
    g_cb = rec_cb_new();
    [g_cb beginCommandBufferWithAllocator:g_alloc4[g_alloc_cur]];   // an allocator is reset only once its fence (the last segment) has passed
    g_cut_draw0 = g_stat_draw;
    if (!g_cuts++) MLOG("segmented submission: first segment submitted at frame %llu pass %llu", (unsigned long long)f, (unsigned long long)g_stat_pass);
}
// DM_NVN_PASS_SPLIT: with no segment left on the GPU, a long pass is ended and committed, and the next draw reopens it; a split costs a store and load of the attachments.
static uint64_t g_pass_splits, g_split_pass;
static int g_split_n;
static int pass_split_draws(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_PASS_SPLIT"); v = e ? atoi(e) : 256; }
    return v;
}
static int pass_split_max(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_PASS_SPLIT_MAX"); v = e ? atoi(e) : 2; }
    return v;
}
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

// Clears a load action cannot express are drawn as one triangle over the pass. Adapted from MacNativeGames' clear_quad / clear_ds_quad (github.com/ly05010419/MacNativeGames, MIT).
static int pf_int_kind(MTLPixelFormat f) {   // 0 float (also normalized), 1 uint, 2 int
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR16Uint: case MTLPixelFormatR32Uint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG32Uint:
        case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA32Uint: return 1;
        case MTLPixelFormatR8Sint: case MTLPixelFormatR16Sint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG8Sint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRG32Sint:
        case MTLPixelFormatRGBA8Sint: case MTLPixelFormatRGBA16Sint: case MTLPixelFormatRGBA32Sint: return 2;
        default: return 0;
    }
}
static id<MTLLibrary> clr_lib(void) {
    static id<MTLLibrary> lib; static int failed;
    if (lib || failed) return lib;
    NSError *e = nil; lib = host_lib("clear", &e);
    if (!lib) { failed = 1; MLOG("[RED] clear-triangle shaders: no library: %s", e.localizedDescription.UTF8String); }
    return lib;
}
typedef struct { int32_t slot, kind, mask; uint32_t cfmt[8], dfmt, sfmt, samples; } clr_key;   // slot -1: a depth / stencil clear
static id<MTLRenderPipelineState> clr_pso(const clr_key *k) {
    static NSMutableDictionary<NSData *, id<MTLRenderPipelineState>> *cache;
    if (!cache) cache = [NSMutableDictionary dictionary];
    NSData *kd = [NSData dataWithBytes:k length:sizeof *k];
    id<MTLRenderPipelineState> ps = cache[kd];
    if (ps) return ps;
    id<MTLLibrary> lib = clr_lib(); if (!lib) return nil;
    static const char *ty[3] = {"float", "uint", "int"};
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = [lib newFunctionWithName:@"clr_vs"];
    if (k->slot >= 0) d.fragmentFunction = [lib newFunctionWithName:[NSString stringWithFormat:@"clr_fs_%d_%s", k->slot, ty[k->kind]]];
    for (int i = 0; i < 8; i++) if (k->cfmt[i]) { d.colorAttachments[i].pixelFormat = (MTLPixelFormat)k->cfmt[i]; d.colorAttachments[i].writeMask = MTLColorWriteMaskNone; }
    if (k->slot >= 0) d.colorAttachments[k->slot].writeMask = (MTLColorWriteMask)((k->mask & 1 ? MTLColorWriteMaskRed : 0) | (k->mask & 2 ? MTLColorWriteMaskGreen : 0) | (k->mask & 4 ? MTLColorWriteMaskBlue : 0) | (k->mask & 8 ? MTLColorWriteMaskAlpha : 0));
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt; d.rasterSampleCount = k->samples ? k->samples : 1;
    NSError *e = nil; ps = dev_rps(d, &e);
    if (!ps) {
        static int n;
        if (n++ < 4) MLOG("[RED] cannot create the clear-triangle pipeline (slot %d color format %u depth %u stencil %u samples %u): %s; this clear is skipped", k->slot, k->slot >= 0 ? k->cfmt[k->slot] : 0, k->dfmt, k->sfmt, k->samples, e.localizedDescription.UTF8String);
        return nil;
    }
    cache[kd] = ps;
    return ps;
}
static id<MTLDepthStencilState> clr_dss(int dw, unsigned sm) {
    static id<MTLDepthStencilState> c[2][256];
    if (c[dw][sm]) return c[dw][sm];
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new]; d.depthCompareFunction = MTLCompareFunctionAlways; d.depthWriteEnabled = dw != 0;
    if (sm) {
        MTLStencilDescriptor *st = [MTLStencilDescriptor new];
        st.stencilCompareFunction = MTLCompareFunctionAlways; st.stencilFailureOperation = st.depthFailureOperation = MTLStencilOperationKeep;
        st.depthStencilPassOperation = MTLStencilOperationReplace; st.readMask = 0xff; st.writeMask = sm;
        d.frontFaceStencil = d.backFaceStencil = st;
    }
    return c[dw][sm] = [g_dev newDepthStencilStateWithDescriptor:d];
}
static int clr_scissor(MTLScissorRect *sc) {   // 0 = empty
    NSUInteger tw = g_pass.tw, th = g_pass.th; *sc = (MTLScissorRect){0, 0, tw, th};
    if (!R.has_sc) return tw && th;
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > (long)tw) x1 = (long)tw;
    if (y1 > (long)th) y1 = (long)th;
    if (x1 <= x0 || y1 <= y0) return 0;
    *sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    return 1;
}
static void clr_tri(id<MTL4RenderCommandEncoder> en, id<MTLRenderPipelineState> ps, id<MTLDepthStencilState> ds, MTLScissorRect sc, const uint32_t val[4], float z, uint32_t sref, NSUInteger tw, NSUInteger th) {
    static id<MTL4ArgumentTable> vt, ft;
    if (!ft) {
        NSError *e = nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1;
        vt = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]);
        ft = vt ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!ft) {
            vt = nil;
            static int n;
            if (!n++) MLOG("[RED] cannot create the clear-triangle argument table: %s; this clear is skipped", e.localizedDescription.UTF8String);
            return;
        }
    }
    id<MTLBuffer> b; size_t o = arena_alloc(32, &b);
    uint8_t *p = (uint8_t *)b.contents + o; memcpy(p, val, 16); memcpy(p + 16, &z, 4);   // colour at 0, depth at 16
    at_addr(ft, b.gpuAddress + o, 0, "clear table slot", 0); at_addr(vt, b.gpuAddress + o + 16, 0, "clear table slot", 0);
    [en setArgumentTable:vt atStages:MTLRenderStageVertex]; [en setArgumentTable:ft atStages:MTLRenderStageFragment];
    enc_r_pso(en, ps); g_enc_pso = nil; enc_r_dss(en, ds); g_enc_dss = nil;
    if (g_enc_vis) { enc_r_visibility(en, MTLVisibilityResultModeDisabled, 0); g_enc_vis = 0; }
    enc_r_viewport(en, (MTLViewport){0, 0, (double)tw, (double)th, 0, 1}); enc_r_scissor(en, sc);
    enc_r_cull(en, MTLCullModeNone); enc_r_fill(en, MTLTriangleFillModeFill); enc_r_depth_bias(en, 0, 0, 0); enc_r_depth_clip(en, YES); enc_r_stencil_ref(en, sref, sref); g_ed.ok = 0;
    enc_r_draw(en, MTLPrimitiveTypeTriangle, 0, 3);
    [en setArgumentTable:g_vt atStages:MTLRenderStageVertex]; [en setArgumentTable:g_ft atStages:MTLRenderStageFragment];
}
// integer kinds take it converted, as a load-action clear does
static uint32_t clr_word(float f, int kind) {
    if (kind == 1) return f > 0 ? (f < 4294967040.f ? (uint32_t)f : 0xffffffffu) : 0;
    uint32_t v;
    if (kind == 2) { int32_t si = !(f == f) ? 0 : f >= 2147483520.f ? INT32_MAX : f <= -2147483648.f ? INT32_MIN : (int32_t)f; memcpy(&v, &si, 4); }
    else memcpy(&v, &f, 4);
    return v;
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
    for (int i = 0; i < 4; i++) v[i] = c->is_uint && kind ? c->c.u[i] : clr_word(c->is_uint ? (float)c->c.u[i] : c->c.f[i], kind);
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
static void clr_note(int slot, int depth, int stencil) {
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i];
        if ((t->depth == 0 && slot >= 0 && t->idx == slot) || (t->depth == 1 && depth) || (t->depth == 2 && stencil)) { t->wrote = t->rast = 1; t->wwhy = 2; }
    }
}
static uint64_t g_clr_tri[2];
static void clear_color_tri(const p_clear_color *c) {
    int slot = c->index, mask = c->mask & 0xf;
    if (!mask || slot < 0 || slot >= 8 || slot >= R.rt.n || !R.rt.color[slot]) return;
    id<MTL4RenderCommandEncoder> en = rp_begin();
    if (en && !g_pass.cfmt[slot] && g_enc_mode) { rp_end(); en = rp_begin(); }   // opened without this attachment (a clear-only pass): reopen with all of them
    if (!en) return;
    int at = slot;
    if (!g_pass.cfmt[at] && rt_dup_slot(&R.rt, slot))   // a slot attached twice is attached only once (rt_dup_slot)
        for (int j = 0; j < slot; j++) {
            const tex_t *a = R.rt.color[j], *b = R.rt.color[slot];
            if (!a || a->serial != b->serial || !g_pass.cfmt[j]) continue;
            const view_t *va = &R.rt.cview[j], *vb = &R.rt.cview[slot];
            int ha = R.rt.has_cview[j], hb = R.rt.has_cview[slot];
            if ((ha && va->set_levels ? va->base_level : 0) == (hb && vb->set_levels ? vb->base_level : 0)
                && (ha && va->set_layers ? va->min_layer : 0) == (hb && vb->set_layers ? vb->min_layer : 0)) { at = j; break; }
        }
    if (!g_pass.cfmt[at]) { PD("c %llu color-clear triangle skipped: slot %d is not in this pass", g_stat_pass, slot); return; }
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu color-clear triangle skipped: empty scissor slot %d", g_stat_pass, slot); return; }
    int kind = pf_int_kind((MTLPixelFormat)g_pass.cfmt[at]);
    clr_key k; memset(&k, 0, sizeof k);
    k.slot = at; k.kind = kind; k.mask = mask; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
    id<MTLRenderPipelineState> ps = clr_pso(&k); id<MTLDepthStencilState> ds = clr_dss(0, 0); if (!ps || !ds) return;
    uint32_t v[4];
    for (int i = 0; i < 4; i++) v[i] = clr_word(c->rgba[i], kind);
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
    if (en && g_enc_mode && ((c->depth_mask && !g_pass.hd) || (want_s && !g_pass.hs))) { rp_end(); en = rp_begin(); }   // opened without the depth or stencil plane: reopen
    if (!en) return;
    int dw = c->depth_mask && g_pass.hd; unsigned sm = g_pass.hs ? want_s : 0;
    if (!dw && !sm) return;
    MTLScissorRect sc; if (!clr_scissor(&sc)) { PD("c %llu depth-stencil clear triangle skipped: empty scissor", g_stat_pass); return; }
    clr_key k; memset(&k, 0, sizeof k);
    k.slot = -1; memcpy(k.cfmt, g_pass.cfmt, sizeof k.cfmt); k.dfmt = g_pass.dfmt; k.sfmt = g_pass.sfmt; k.samples = g_pass.samples;
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

static int unb_keeps(uint32_t op) {
    switch (op) {
        case OP_SET_RT: case OP_VIEWPORTS: case OP_SCISSOR: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR:
        case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_DEPTH_CLAMP: case OP_LINE_WIDTH: case OP_VP_SWIZZLE: case OP_PRIM_RESTART: case OP_RAST_DISCARD: case OP_SET_TEX_POOL:
        case OP_SET_SAMP_POOL: case OP_JUMP: case OP_CALL: return 1;
        default: return 0;
    }
}

static void b2b_flush(int cut) {
    if (!g_b2b_n) return;
    if (cut) {
        uint64_t gen = g_cbgen;
        if (!g_enc) {
            if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
            g_cut_copy = 1; sub_cut();
        }
        if (g_cbgen != gen) g_b2b_cuts++; else g_b2b_nocut++;
    }
    for (int i = 0; i < g_b2b_n; i++) { nvn_dirty_gpu_written(g_b2b_lo[i], g_b2b_hi[i] - g_b2b_lo[i]); g_b2b_bytes += g_b2b_hi[i] - g_b2b_lo[i]; }
    if (++g_b2b_runs <= 3 || g_b2b_runs % 500 == 0) MLOG("buffer copy runs %llu: committed before the next command %llu, not committable there %llu (an encoder open or a pass held back), %.1f MB stamped%s",
        (unsigned long long)g_b2b_runs, (unsigned long long)g_b2b_cuts, (unsigned long long)g_b2b_nocut, g_b2b_bytes / 1048576.0, g_b2b_over ? ", range list overflowed (merged)" : "");
    g_b2b_n = 0; g_b2b_over = 0;
}
void mtl_exec(const uint8_t *p) {
    if (!g_state_inited) state_reset();
    static int dump_all = -1;
    if (dump_all < 0) dump_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
    int trace = dump_all && passdump();
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        g_exec_cur = p;
        if (trace) PD("o cmd=%p pass=%llu op=%u size=%u", p, g_stat_pass, (unsigned)h.op, (unsigned)h.size);
        // DM_NVN_ALLOCPROF=1: GPU allocation growth charged to the previous operation's type (rough)
        allocprof_level();
        if (g_allocprof == 2) {
            static int prev = -1;
            uint64_t now = g_alloc4[g_alloc_cur].allocatedSize;
            if (prev >= 0 && prev < 64 && now > g_cm_last) g_cm_op[prev] += now - g_cm_last;
            g_cm_last = now; prev = (int)h.op;
            if (h.op < 64) g_cm_opn[h.op]++;
        }
        if (g_allocprof) {
            static uint64_t last; static int prev = -1;
            uint64_t now = g_dev.currentAllocatedSize;
            if (prev >= 0 && prev < 64 && now > last) g_ap_bytes[prev] += now - last;
            last = now; prev = h.op;
        }
        const void *a = p + 8;
        if (g_unb && !unb_keeps(h.op) && !(h.op == OP_END && g_ret_n)) { PD("M %llu unbound targets land before op %u", g_stat_pass, (unsigned)h.op); rp_flush(); }
        if (g_nlc && !lc_keeps(h.op) && !(h.op == OP_END && g_ret_n)) lc_land();
        if (g_b2b_n && h.op != OP_COPY_B2B && !(h.op == OP_END && g_ret_n)) b2b_flush(h.op != OP_END || g_nrest);
        switch (h.op) {
            case OP_END: g_exec_cur = NULL;
                return;   // a pass ended outside a list must not scan expired command memory: a null start stores everything
            case OP_BEGIN: break;
            case OP_JUMP: { const uint8_t *t; memcpy(&t, a, 8); p = t; continue; }
            case OP_CALL: { uint64_t t; memcpy(&t, a, 8);
                if (g_ret_n < 16) g_ret[g_ret_n] = p + h.size;
                g_ret_n++;
                mtl_exec((const uint8_t *)t); g_ret_n--; break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4);
                PD("Y signal_event cmd=%p dst=%p mode=%u value=%u", p, val, m, v); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8);
                PD("Q report type=%llu cmd=%p dst=%#llx", (unsigned long long)t, p, (unsigned long long)ad);
                rc_note(ad, t == 0 || t == 0x10 ? 1 : t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 2 : 3);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                    if (!g_vis_on) {   // no reset before it: the counter runs from the queue's creation
                        g_vis_on = 1; g_vis_counting = 1;
                        MLOG("occlusion queries: samples passed reported with no ResetCounter before it: counting from this report");
                        if (g_enc && !g_enc_has_vis) { PD("M %llu occlusion query counting starts at a report: render pass split here", g_stat_pass); rp_end(); }
                    }
                    hw_note(ad); post((post_op){3, NULL, VIS_REPORT, 0, ad, (uint32_t)t, vis_snapshot()}); vis_clear(); vis_need_scan(p + h.size);
                }
                else mtl_sub_counter(ad, (uint32_t)t);
                break; }
            case OP_RENDER_COND: { const p_render_cond *rc = a;
                if (rc->mode >= 2) { g_rc_skip = rc->mode == 3; PD("E %llu render enable %d", g_stat_pass, rc->mode == 2); }
                else { uint64_t x = 0, y = 0; int known = rc_value(rc->addr, &x) & rc_value(rc->addr + 16, &y);
                    g_rc_skip = known && (rc->mode ? x == y : x != y);
                    PD("E %llu conditional render %s addr=%#llx values %llu / %llu%s%s", g_stat_pass, rc->mode ? "draw if not equal" : "draw if equal", (unsigned long long)rc->addr, (unsigned long long)x, (unsigned long long)y, known ? "" : " (unknown, drawn)", g_rc_skip ? " → not drawn" : ""); }
                break; }
            case OP_FENCE_SYNC: { sync_t *s; memcpy(&s, a, 8); s->fence = g_fence + 1; s->fenced = 1;
                PD("Y fence_sync cmd=%p sync=%p fence=%llu", p, (void *)s, (unsigned long long)s->fence); break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); PD("Q reset type=%llu cmd=%p", (unsigned long long)t, p);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) {
                    g_vis_on = 1; g_vis_counting = 1; vis_clear(); post((post_op){3, NULL, VIS_ZERO, 0, 0, (uint32_t)t, NULL}); vis_need_scan(p + h.size);
                    // the first reset inside a pass begun without a visibility buffer: split it so the rest is counted
                    if (g_enc && !g_enc_has_vis) { PD("M %llu occlusion query counting starts: render pass split here", g_stat_pass); rp_end(); }
                }
                break; }
            case OP_CLEAR_BUF: { const p_clear_buf *cb = a; mtl_sub_fill(cb->addr, cb->size, cb->value); break; }
            case OP_COPY_B2B: { uint64_t s_, d, n; memcpy(&s_, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8);
                PD("Y copy_b2b cmd=%p src=%#llx dst=%#llx bytes=%llu", p, (unsigned long long)s_, (unsigned long long)d, (unsigned long long)n); mtl_sub_copy(s_, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                if (p == g_mdx_stop) { g_mdx_epoch++; g_mdx_stop = NULL; }
                PD("Y update_ub cmd=%p src=%#llx dst=%#llx bytes=%llu f=%g,%g,%g,%g", p, (unsigned long long)((const uint8_t *)a + 16), (unsigned long long)d, (unsigned long long)n,
                   n >= 4 ? ((const float *)((const uint8_t *)a + 16))[0] : 0.f, n >= 8 ? ((const float *)((const uint8_t *)a + 16))[1] : 0.f, n >= 12 ? ((const float *)((const uint8_t *)a + 16))[2] : 0.f, n >= 16 ? ((const float *)((const uint8_t *)a + 16))[3] : 0.f);
                mtl_sub_update((const uint8_t *)a + 16, d, n); break; }
            case OP_SET_RT: {
                const p_set_rt *r = a; g_setrt_n++; g_split_resume = 0;
                int pend = R.clear_d || R.clear_s;
                for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
                int empty = !r->depth;
                for (int k = 0; k < r->n && k < 8; k++) if (r->color[k]) empty = 0;
                PD("M %llu set_rt n=%d depth=%d empty=%d enc=%d", g_stat_pass, r->n, !!r->depth, empty, !!g_enc);
                if (empty && g_enc && g_enc_mode != 1 && !pend && R.has_rt && rtc_on() && unb_on()) { g_unb = 1; g_unb_rt = *r;
                    if (!g_unb_n++) MLOG("deferred unbind: first case pass %llu", g_stat_pass); break; }
                if (g_unb) g_unb = 0;
                // a clear-only pass is not continued
                if (g_enc && R.has_rt && !empty) {
                    int same = rt_same(&R.rt, r), can_continue = g_enc_mode != 1 && !pend && rtc_on();
                    if (same && can_continue) {
                        g_rt_merged = 1;
                        if (!g_rt_merge_n++) MLOG("same-target continuation: first case pass %llu", g_stat_pass);
                        PD("M %llu same-target continuation pass", g_stat_pass);
                        R.rt = *r; break;
                    }
                    if (!same && can_continue) {
                        static int nd;
                        if (nd++ < 8) MLOG("same-target continuation refused (%s) pass %llu", g_rt_diff ?: "?", g_stat_pass);
                        PD("M %llu new targets (%s)", g_stat_pass, g_rt_diff ?: "?");
                    }
                    if (same) { static int nw; if (nw++ < 8) MLOG("same targets, no continuation: enc_mode=%d pend=%d rtc=%d pass %llu", g_enc_mode, pend, rtc_on(), g_stat_pass); }
                }
                if (g_enc && g_sm_pass && !pend && !g_game_barrier) {
                    int sl;
                    if (sm_rt_slice(&R.rt, r, &sl) && sl >= g_sm_lo && sl <= g_sm_hi) {
                        R.rt = *r; g_sm_cur = sl; g_sm_ncont++;
                        void td_note_layer(id<MTLTexture>, uint64_t, NSUInteger, NSUInteger);
                        td_note_layer(tex_mtl_replay(r->color[0]), r->color[0]->serial, r->cview[0].set_levels ? r->cview[0].base_level : 0, (NSUInteger)sl);
                        PD("M %llu slice-merged pass goes on in layer %d", g_stat_pass, sl); break;
                    }
                }
                if (pend && !g_enc && g_cb && lc_on()) lc_push(); else rp_flush();
                sub_cut(); R.rt = *r; R.has_rt = 1; break;
            }
            case OP_CLEAR_COLOR: {
                const p_clear_color *c = a;
                if (g_rc_skip) { PD("c %llu color clear skipped by conditional rendering slot%d", g_stat_pass, c->index); break; }
                // a scissored clear to 0 outside a pass: a copy from the zero buffer
                if (!g_enc && c->mask == 0xf && !c->rgba[0] && !c->rgba[1] && !c->rgba[2] && !c->rgba[3] && R.has_sc && c->index >= 0 && c->index < R.rt.n && R.rt.color[c->index]
                    && !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index]) && clear_rect_zero(c->index)) break;
                if (g_rt_merged || g_sm_pass || g_fsp_pass) rp_end();
                if (c->index < 0 || c->index >= R.rt.n || !R.rt.color[c->index]) { RED_ONCE(5, "color clear target %d invalid", c->index); break; }
                if (g_enc || c->mask != 0xf || !scissor_full(R.rt.color[c->index], &R.rt.cview[c->index], R.rt.has_cview[c->index])) {
                    static uint64_t n; static int clog = -1;
                    if (clog < 0) clog = getenv("DM_NVN_CLEARLOG") != NULL;
                    if (clog && n++ < 40) MLOG("color clear drawn as a triangle frame %llu pass %llu target %d ser=%llu %ux%u mid=%d mask=%#x scissor=%d,%d,%d,%d color=%g,%g,%g,%g", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, c->index,
                        (unsigned long long)R.rt.color[c->index]->serial, R.rt.color[c->index]->d.width, R.rt.color[c->index]->d.height, g_enc != nil, c->mask, R.sc.x, R.sc.y, R.sc.w, R.sc.h, c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                    clear_color_tri(c); break;
                }
                R.clear_c[c->index] = 1; R.cc[c->index] = MTLClearColorMake(c->rgba[0], c->rgba[1], c->rgba[2], c->rgba[3]);
                PD("c %llu color clear recorded slot%d ser=%llu mask=%#x scissor=%d:%d,%d,%d,%d", g_stat_pass, c->index, (unsigned long long)R.rt.color[c->index]->serial, c->mask, R.has_sc, R.sc.x, R.sc.y, R.sc.w, R.sc.h);
                break;
            }
            case OP_CLEAR_DS: {
                const p_clear_ds *c = a;
                if (g_rc_skip) { PD("c %llu depth-stencil clear skipped by conditional rendering", g_stat_pass); break; }
                if (g_rt_merged || g_sm_pass || g_fsp_pass) rp_end();
                // A load-action clear covers the whole attachment and every stencil bit: other depth / stencil clears are drawn as a triangle.
                unsigned sm = (unsigned)c->stencil_mask & 0xff;
                // partial stencil mask, whole depth: only the stencil takes the triangle, so the depth plane is not loaded only to be cleared
                if (R.rt.depth && !g_enc && c->depth_mask && sm && sm != 0xff && scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview)) {
                    R.clear_d = 1; R.cd = c->depth; p_clear_ds s = *c; s.depth_mask = 0; clear_ds_tri(&s); break;
                }
                if (R.rt.depth && (!scissor_full(R.rt.depth, &R.rt.dview, R.rt.has_dview) || (sm && sm != 0xff))) { clear_ds_tri(c); break; }
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
                g_mdx_epoch++; g_mdx_stop = NULL;
                if (!g_rc_skip) draw_multi(a);
                g_mdx_epoch++;
                break;
            case OP_DRAW: { if (g_rc_skip) break; if (g_fsm_ntaken && fsm_take(p)) { PD("L %llu face-split draw already drawn in an earlier pass (DM_NVN_FACE_MERGE)", g_stat_pass); break; }
                p_draw t; g_exec_cur = p; g_exec_next = p + h.size; draw(draw_rec(a, h.size, &t), NULL); g_exec_next = NULL; break; }
            case OP_DRAW_IDX: { if (g_rc_skip) break; if (g_fsm_ntaken && fsm_take(p)) { PD("L %llu face-split draw already drawn in an earlier pass (DM_NVN_FACE_MERGE)", g_stat_pass); break; }
                p_draw_idx t; g_exec_cur = p; g_exec_next = p + h.size; draw(NULL, draw_idx_rec(a, h.size, &t)); g_exec_next = NULL; break; }
            // state_op marks state dirty only on change: the game re-sends the same program / state before each draw
            case OP_BIND_PROGRAM: { const p_bind_program *b = a; state_op(h.op,a);
                if (passdump()) { NvProg *pr = S.prog ? prog_get(S.prog) : nil;
                    PD("Y bind_program cmd=%p program=%p stages=%#x vs=%s fs=%s",p,(void *)b->prog,b->stages,
                       pr && pr->sh[0] ? pr->sh[0]->key : "-", pr && pr->sh[1] ? pr->sh[1]->key : "-"); }
                break; }
            case OP_SCISSOR: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
            case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
            case OP_POLY_OFFSET: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_LINE_WIDTH: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_COPY_STRIDE:
                state_op(h.op, a); break;
            // not in state_op: look-ahead scans replay it
            case OP_VIEWPORTS: { const p_viewports *b = a; state_op(h.op, a);
                if (passdump()) { unsigned long long pn = g_enc ? g_stat_pass : g_stat_pass + 1; PD("O %llu set_viewports first=%d n=%d", pn, b->first, b->n);
                    for (int i = 0; i < b->n && i < 4; i++) PD("O %llu   vp[%d] = %.0f,%.0f,%.0f,%.0f", pn, b->first + i, b->v[i][0], b->v[i][1], b->v[i][2], b->v[i][3]); }
                break; }
            // ops nothing records (transform feedback, SetPatchSize): their NVN entries report in red
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
            case OP_BARRIER: { int32_t bits; memcpy(&bits, a, 4);   // ordered at the next draw pass (barrier_game), or by swrite_note / prog_swrites inside one
                int order = g_enc && (bits & (NVN_BARRIER_ORDER_PRIMITIVES_BIT | NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT)), split = order && g_pass_swrite;
                PD("I %llu barrier bits %#x%s", g_stat_pass, bits, split ? ": ends the pass (shader writes)" : order ? " (render pass open; a later shader write is ordered after its draws)" : g_enc ? " (render pass open)" : "");
                if (split) { rp_end(); static int once; if (!once++) MLOG("a game barrier (bits %#x) after shader writes in a draw pass: the pass ends there (first case pass %llu)", bits, (unsigned long long)g_stat_pass); }
                else if (order) g_pass_order |= (bits & NVN_BARRIER_ORDER_PRIMITIVES_BIT ? 1 : 0) | (bits & (NVN_BARRIER_ORDER_FRAGMENTS_BIT | NVN_BARRIER_ORDER_FRAGMENTS_TILED_BIT) ? 2 : 0);
                if ((bits & GAME_BARRIER_FULL) && g_mdx_wr != ~0ull && g_mdx_wr != g_vbuf_write_epoch) g_mdx_epoch++;
                if (bits) { g_game_barrier |= bits; g_fb_bar++; g_dsp_game_bar = 1; } break; }   // snapshots taken before it are stale (fb_prepare)
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: state_op(h.op, a); break;
            default: break;
        }
        p += h.size;
    }
}
