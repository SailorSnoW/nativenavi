// Render passes: segment timestamps, attachment stores, the pass dump, switches and A/B phases (sw_mode, DM_AB), BC targets, GPU stores into CPU-read storage (gsb), rp_begin, rp_flush.
// Clears before the first command in a pass are folded into its load action.
static struct {
    p_set_rt rt; int has_rt;
    uint8_t clear_c[8]; MTLClearColor cc[8];
    uint8_t clear_d, clear_s; double cd; uint32_t cs;
    p_scissor sc; int has_sc;
} R;

static id<MTL4RenderCommandEncoder> g_enc;
// A GPU boundary every cut_draws() draws as a relaxed timestamp, even when sub_cut keeps the command buffer open; with the encode thread the writes are recorded in order.
enum { TS_SLOTS = 4096 };   // the largest timestamp heap Metal makes
static uint32_t g_ts_next; static int g_ts_state, g_ts_s0;   // state: 0 not tried, 1 on, -1 off
static int ts_on(void) {
    if (!g_ts_state) { g_ts_state = -1;
        if (seg_ts()) { MTL4CounterHeapDescriptor *hd = [MTL4CounterHeapDescriptor new]; hd.type = MTL4CounterHeapTypeTimestamp; hd.count = TS_SLOTS; NSError *er = nil;
            g_ts_heap = [g_dev newCounterHeapWithDescriptor:hd error:&er];
            if (g_ts_heap) g_ts_state = 1; else MLOG("[RED] counter timestamps: no counter heap (%s): every segment boundary stays a commit", er.description.UTF8String); } }
    return g_ts_state > 0;
}
static int ts_stamp(void) {   // written at the start of the next encoder: a write between encoders costs the GPU time
    if (g_ts_pend >= 0) return g_ts_pend;
    int slot = (int)(g_ts_next++ % TS_SLOTS); [g_ts_heap invalidateCounterRange:NSMakeRange((NSUInteger)slot, 1)];
    if (g_blit) [g_blit writeTimestampWithGranularity:MTL4TimestampGranularityRelaxed intoHeap:g_ts_heap atIndex:(NSUInteger)slot]; else g_ts_pend = slot;
    return slot;
}
static uint64_t g_ts_begin;   // guest ns when this submission began encoding: no GPU timestamp of it is earlier
static void ts_sub_begin(void) { if (ts_on()) { g_ts_s0 = ts_stamp(); g_ts_begin = guest_ns(); } }
static void ts_clk(void) {
    if (!ts_on() || g_sclk) return;
    g_sclk = calloc(1, sizeof *g_sclk); if (!g_sclk) MDIE("segment clock allocation failed"); atomic_store(&g_sclk->refs, 1); g_sclk->b[0] = (uint16_t)g_ts_s0; g_sclk->nb = 1; g_sclk->begin = g_ts_begin;
}
static void ts_boundary(void) {
    if (!ts_on()) return;
    ts_clk(); if (g_sclk->nb <= SEG_CLK_N) g_sclk->b[g_sclk->nb++] = (uint16_t)ts_stamp();
}
static int enc_open(void) { return g_enc != nil; }
// determined once at rp_begin from the attachments and shared by every draw of the pass
static struct { uint32_t cfmt[8], dfmt, sfmt, samples; int hd, hs; NSUInteger tw, th; uint64_t serial; } g_pass;
// Slice-merged passes: consecutive passes into other layers of one level run as one layered pass, each draw reaching its layer through a vertex amplification view mapping,
// which needs pipelines built with maxVertexAmplificationCount 2 (with the default 1 every draw lands in the first layer).
static struct { int on, lo, hi; } g_sm_plan;   // set by draw() for the pass rp_begin_ex is about to open
static int g_sm_pass, g_sm_lo, g_sm_hi, g_sm_cur, g_sm_applied;   // cur: the layer the game's target names now; applied: the offset set on the encoder (-1 none)
static uint64_t g_sm_npass, g_sm_ncont, g_sm_nwait;
static int g_pso_amp;
// Target i is layer g_fsp_layer[i] of the one attachment, drawn with g_fsp_ps[i] (nil: left as it is).
static struct { int on, n; uint32_t fmt; } g_fsp_plan;   // the draw qualifies and its pipelines are built
static int g_fsp_pass, g_fsp_n; static uint8_t g_fsp_layer[8]; static __unsafe_unretained id<MTLRenderPipelineState> g_fsp_ps[8];
static MTLLogicalToPhysicalColorAttachmentMap *g_fsp_map[8];
static uint64_t g_fsp_npass;
// later face-split draws into other textures of the same level size join the open pass; the replay skips them where it reaches them (fsm_take)
enum { FSM_MAX = 3, FSM_TAKEN = 64 };
typedef struct { const uint8_t *op; p_set_rt rt; NSUInteger level, slice; uint64_t ser; int n; uint8_t layer[8]; __unsafe_unretained id<MTLRenderPipelineState> ps[8]; } fsm_draw;
static fsm_draw g_fsm[FSM_MAX]; static int g_fsm_np, g_fsm_na, g_fsp_phys;
static const uint8_t *g_fsm_taken[FSM_TAKEN]; static int g_fsm_ntaken; static uint64_t g_fsm_taken_sub;   // moved draws the replay has not reached yet
static uint64_t g_fsm_moved, g_fsm_npass;
static MTLLogicalToPhysicalColorAttachmentMap *g_fsm_map[FSM_MAX + 1][8];
static void fsm_attach(MTL4RenderPassDescriptor *pd);
void mtl_sm_stats(uint64_t *np, uint64_t *nc, uint64_t *nw) { *np = g_sm_npass; *nc = g_sm_ncont; *nw = g_sm_nwait; }
static id<MTLRenderPipelineState> g_enc_pso; static id<MTLDepthStencilState> g_enc_dss;
// g_ed.ok: one bit per item, cleared for every new encoder and after a clear triangle (which sets its own)
static struct { unsigned ok; uint32_t sref, sref_b; MTLViewport vp; MTLScissorRect sc; int cull, wind, fill, clip; float bias[3], blend[4]; } g_ed;
enum { ED_SREF = 1, ED_VP = 2, ED_SC = 4, ED_CULL = 8, ED_WIND = 16, ED_FILL = 32, ED_BIAS = 64, ED_CLIP = 128, ED_BLEND = 256 };
static int g_enc_vis, g_enc_has_vis;   // 0 off / slot number + 1
static int g_pso_dirty = 1, g_dss_dirty = 1;
static uint64_t g_stat_args_enc, g_stat_args_reuse, g_stat_args_patch;
static uint64_t g_stat_draw, g_stat_pass, g_bw_load, g_bw_store, g_pass_full_rw, g_udc_saved_bytes;
static int g_kd_force;
static _Atomic long g_pd_a0 = -2, g_pd_a1;   // -2 = not read yet, -3 = every:<n>
static long g_pd_every;
static inline int passdump(void) {
    if (g_kd_force) return 1;
    if (__builtin_expect(g_pd_a0 == -1, 1)) return 0;   // off: answered first, it is asked for every binding of every draw
    if (g_pd_a0 == -2) { const char *e = getenv("DM_NVN_PASSDUMP");
        if (e && !strncmp(e, "every:", 6)) { g_pd_every = atol(e + 6); g_pd_a1 = -1; g_pd_a0 = g_pd_every > 0 ? -3 : -1; }   // halfway between two DM_NVN_GPUPROF=every:n frames
        else { g_pd_a1 = e && strchr(e, '-') ? atol(strchr(e, '-') + 1) : e ? atol(e) : -1; g_pd_a0 = e ? atol(e) : -1; } }
    long f = (long)atomic_load(&g_cur_frame), a0 = g_pd_a0;
    if (a0 == -3) return f > 0 && f % g_pd_every == g_pd_every / 2;
    return a0 >= 0 && f >= a0 && f <= g_pd_a1;
}
void dm_gfx_passdump_next(long n) { long f = (long)atomic_load(&g_cur_frame); g_pd_a1 = f + n; g_pd_a0 = f + 1; }
#define PD(...) do { if (passdump()) { flockfile(stderr); fprintf(stderr, "[pass] F%llu ", (unsigned long long)atomic_load(&g_cur_frame)); fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); funlockfile(stderr); } } while (0)
static void blit_note(void) { if (g_enc) PD("E %llu copy / buffer update ends the render pass", g_stat_pass); }
__attribute__((weak)) void dm_guest_frame_tick(void) {}   // the host's hle_os.c has the strong definition; graphics unit tests do not link it
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }
// a stub returning 0 would send every report to the time of its completion handler
__attribute__((weak)) uint64_t guest_ns(void) { return now_ns(); }
__attribute__((weak)) uint64_t guest_ns_at_host_time(uint64_t host_ns) { return host_ns; }
#ifndef guest_clock_freeze_for
__attribute__((weak)) void guest_clock_freeze_for(int reason, int on) { (void)reason; (void)on; }
#endif
static uint64_t att_bytes(id<MTLTexture> t, NSUInteger level) {
    if (!t) return 0;
    NSUInteger w = MAX(t.width >> level, 1), h = MAX(t.height >> level, 1);
    NSUInteger bpp = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? 5 : t.pixelFormat == MTLPixelFormatRGBA16Float || t.pixelFormat == MTLPixelFormatRG32Float ? 8 :
                     t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR8Unorm || t.pixelFormat == MTLPixelFormatStencil8 ? 1 :
                     t.pixelFormat == MTLPixelFormatRG8Unorm || t.pixelFormat == MTLPixelFormatR16Float ? 2 : 4;
    return (uint64_t)w * h * bpp * t.sampleCount;
}
// the attachment selects a mip, while MTLTexture.width/height describe the base level
static NSUInteger mip_extent(NSUInteger size, NSUInteger level) {
    return level < sizeof(size) * 8 ? MAX(size >> level, 1) : 1;
}
static void pass_dims(MTL4RenderPassDescriptor *pd, NSUInteger nc, NSUInteger *w, NSUInteger *h) {
    *w = *h = 0;
    for (NSUInteger i = 0; i < nc + 2; i++) {
        MTLRenderPassAttachmentDescriptor *a = i < nc ? pd.colorAttachments[i] : i == nc ? pd.depthAttachment : pd.stencilAttachment;
        if (!a.texture) continue;
        NSUInteger aw = mip_extent(a.texture.width, a.level), ah = mip_extent(a.texture.height, a.level);
        *w = *w ? MIN(*w, aw) : aw; *h = *h ? MIN(*h, ah) : ah;
    }
    if (!*w) { *w = MAX(pd.renderTargetWidth, 1); *h = MAX(pd.renderTargetHeight, 1); }
}

void ts_after(void);
void ms_after(void);
// On TBDR every attachment is written back in full when the pass ends, unlike Maxwell: an attachment this pass did not change is not written back.
typedef struct { uint64_t ser, bytes; uint32_t lv, sl; int idx; uint8_t depth, state, ridx, drawn, why, load, wrote, wwhy, img, rast; const char *wkey; int wslot; } sd_att;   // depth: 0 colour 1 depth 2 stencil; state 0 undecided 1 needed 2 redundant; idx: colour attachment index, -1 for depth
// depth and stencil have independent store decisions
enum { SD_ATT_MAX = 8 + 2 };
static sd_att g_sd[SD_ATT_MAX]; static int g_nsd;
static int g_udc = -1;
// Attachments this pass did not write store DontCare. DM_NVN_UNWRITTEN_DC=0 keeps the image-only colour store rule in sd_finish.
static int udc_on(void) {
    if (g_udc < 0) { const char *e = getenv("DM_NVN_UNWRITTEN_DC"); g_udc = !(e && !strcmp(e, "0")); }
    return g_udc;
}
// DM_AB: both configurations run in one process, scene and thermal state. A switch reads its mode once and asks sw_on at each use; one that must stay the same over a pass latches it.
static _Atomic long g_ab_phase;
static long g_ab_len = -1;
static long ab_len(void) {
    if (g_ab_len < 0) {
        const char *e = getenv("DM_AB");
        g_ab_len = e ? atol(e) : 0;
        if (g_ab_len < 0) g_ab_len = 0;
        if (g_ab_len) MLOG("DM_AB: switches set to ab alternate every %ld frames (phase 0 with all of them off)", g_ab_len);
    }
    return g_ab_len;
}
enum { SW_MAX = 8 }; static const char *g_sw_name[SW_MAX]; static int g_sw_mode[SW_MAX], g_sw_n;
static inline int sw_on_at(int m, long ph) { if (m < 2) return m; int n = (m - 2) & 15, mask = (m - 2) >> 4; return mask >> (ph % n) & 1; }
// 0 off, 1 on, 2 + n + 16 * mask = on in the phases whose number modulo n is in mask; ab without DM_AB is off
static int sw_mode(const char *name, int dflt) {
    const char *e = getenv(name); int m = dflt, mask = 2, n = 2;
    if (e && !strncmp(e, "ab", 2)) {
        if (e[2] == ':') { const char *sl = strchr(e, '/'); int nn = sl ? atoi(sl + 1) : 0, mk = 0;
            for (const char *q = e + 3; sl && q < sl; ) { int k = atoi(q); if (k >= 0 && k < 15) mk |= 1 << k; while (q < sl && *q != ',') q++; if (q < sl) q++; }
            if (nn >= 2 && nn <= 15 && mk && mk < (1 << nn)) { mask = mk; n = nn; } }
        m = ab_len() ? 2 + n + 16 * mask : 0;
        if (!ab_len()) MLOG("[RED] %s=%s needs DM_AB=<frames>: the switch is off", name, e); }
    else if (e) m = atoi(e) ? 1 : 0;
    if (m >= 2) MLOG("%s: on in the DM_AB phases whose number modulo %d is in the set %#x", name, n, mask);
    if (m >= 2 && g_sw_n < SW_MAX) { g_sw_name[g_sw_n] = name; g_sw_mode[g_sw_n++] = m; }
    return m;
}
static inline int sw_on(int m) { return sw_on_at(m, atomic_load_explicit(&g_ab_phase, memory_order_relaxed)); }
// A phase is reported once its frames' GPU times have arrived, without its first quarter while the clocks settle.
enum { AB_MAXF = 4096 };
static float g_ab_gpu[AB_MAXF], g_ab_fr[AB_MAXF]; static int g_ab_nf; static long g_ab_ph = -1; static uint64_t g_ab_bw0[2], g_ab_pass0, g_ab_f0;
static uint64_t g_ab_bw1[2], g_ab_pass1, g_ab_f1;
static int flt_cmp(const void *a, const void *b) { float x = *(const float *)a, y = *(const float *)b; return x < y ? -1 : x > y; }
static void ab_frame(uint64_t f) {
    long ph = (long)(f / (uint64_t)g_ab_len);
    if (atomic_load(&g_ab_phase) != ph || f % (uint64_t)g_ab_len == 0) { g_ab_bw1[0] = g_bw_load; g_ab_bw1[1] = g_bw_store; g_ab_pass1 = g_stat_pass; g_ab_f1 = f; }
    atomic_store(&g_ab_phase, ph);
}
static void ab_report(long ph) {
    int n = g_ab_nf, skip = n / 4, m = n - skip; if (m < 10) return;
    float *g = malloc(sizeof(float) * (size_t)m), *fr = malloc(sizeof(float) * (size_t)m); double gs = 0, fs = 0;
    for (int i = 0; i < m; i++) { g[i] = g_ab_gpu[skip + i]; fr[i] = g_ab_fr[skip + i]; gs += g[i]; fs += fr[i]; }
    qsort(g, (size_t)m, sizeof *g, flt_cmp); qsort(fr, (size_t)m, sizeof *fr, flt_cmp);
    double nf = g_ab_f1 > g_ab_f0 ? (double)(g_ab_f1 - g_ab_f0) : 1;
    char on[160] = ""; size_t o = 0;
    for (int i = 0; i < g_sw_n && o < sizeof on; i++) if (g_sw_mode[i] >= 2 && sw_on_at(g_sw_mode[i], ph)) {
        const char *nm = g_sw_name[i]; if (!strncmp(nm, "DM_NVN_", 7)) nm += 7; else if (!strncmp(nm, "DM_", 3)) nm += 3;
        o += (size_t)snprintf(on + o, sizeof on - o, "%s%s", o ? "," : "", nm); }
    MLOG("[ab] phase %ld %s: %d frames, GPU p50 %.2f / p90 %.2f / mean %.2f ms, frame p50 %.2f / p90 %.2f / mean %.2f ms; per frame: attachments load %.0f MB store %.0f MB, passes %.1f",
         ph, o ? on : "base", m, g[m / 2], g[m * 9 / 10], gs / m, fr[m / 2], fr[m * 9 / 10], fs / m,
         (g_ab_bw1[0] - g_ab_bw0[0]) / nf / 1048576.0, (g_ab_bw1[1] - g_ab_bw0[1]) / nf / 1048576.0, (g_ab_pass1 - g_ab_pass0) / nf);
    free(g); free(fr);
}
// the phase is the one frame f ran in
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms) {
    if (!ab_len()) return;
    long ph = (long)(f / (uint64_t)g_ab_len);
    if (ph != g_ab_ph) {
        if (g_ab_ph >= 0) ab_report(g_ab_ph);
        g_ab_ph = ph; g_ab_nf = 0;
        memcpy(g_ab_bw0, g_ab_bw1, sizeof g_ab_bw0); g_ab_pass0 = g_ab_pass1; g_ab_f0 = g_ab_f1;
    }
    if (g_ab_nf < AB_MAXF) { g_ab_gpu[g_ab_nf] = (float)gpu_ms; g_ab_fr[g_ab_nf] = (float)frame_ms; g_ab_nf++; }
}

static uint64_t g_sd_dead_bytes, g_sd_n_dead, g_sd_n_all;
static void sd_finish(void);
static int pf_is_float(MTLPixelFormat f) {   // RGB9E5 cannot store NaN and does not count
    switch (f) { case MTLPixelFormatR16Float: case MTLPixelFormatRG16Float: case MTLPixelFormatRGBA16Float: case MTLPixelFormatR32Float:
        case MTLPixelFormatRG32Float: case MTLPixelFormatRGBA32Float: case MTLPixelFormatRG11B10Float: return 1; default: return 0; }
}
// A pass continues only while later draws do not sample its attachments: within one Metal pass, sampling the attachment texture is not ordered with earlier draws.
static int g_enc_mode;          // rp_begin_ex argument: 0 all attachments, 1 only for clears
// 0: the whole attachments. A pass stopping at the far corner of its scissor neither loads nor stores what lies beyond; a draw whose scissor leaves the area ends it.
static NSUInteger g_rp_aw, g_rp_ah; static void rt_dims(NSUInteger *tw, NSUInteger *th);
static int rpa_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_RP_AREA");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_RP_AREA=0: passes on very large targets cover the whole target");
    }
    return v;
}
static int g_rp_whole;   // the pass about to open must cover the whole target
static int g_rt_merged;
static uint64_t g_rt_merge_n;
static int g_el_w, g_el_h, g_el_draws; static char g_el_key[12];
static int enc_label_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_ENC_LABEL"); v = e && atoi(e); } return v; }
static int g_nbcrt; static void bcrt_flush(void);
// GPU writes into a texture whose storage the CPU can read reach that storage, as on the hardware (gsb_store, swz in deswz.metal), in the same command buffer.
// The CPU's page tracking does not see them: while a command buffer holds a store into a texture's storage, the texture keeps its host texture (gsb_held). Depth and multisample
// textures are not stored.
static struct { tex_t t; void *tex; unsigned level, slice, n; int att; } g_rtm[8]; static int g_nrtm;   // n: layers drawn from slice on; att: the colour slot
static int tex_storage_dst(const tex_t *t);
static int mtl_fmt_depth(MTLPixelFormat f); static size_t mtl_pf_block_bytes(MTLPixelFormat pf); static int mtl_fmt_block(MTLPixelFormat f);
static id<MTL4ComputeCommandEncoder> dispatch_enc(void); static int rb_copy_shift(const tex_t *t, id<MTLTexture> mt);
static int rb_swap_gpu(uint64_t src, size_t row, size_t img, uint64_t dst, size_t drow, size_t dimg, size_t wb, size_t rows, size_t nz, int shift);
static uint64_t g_gsb_stores, g_gsb_bytes;
void mtl_gsb_stats(uint64_t *n, uint64_t *bytes) { *n = g_gsb_stores; *bytes = g_gsb_bytes; }
static int gsb_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DM_NVN_GPU_STORAGE");
        on = !e || atoi(e) != 0;
        if (!on) MLOG("DM_NVN_GPU_STORAGE=0: what the GPU writes into a texture is not written into its storage");
    }
    return on;
}
static int gsb_target(const tex_t *t) {
    if (!t || !gsb_on() || !t->mem || !t->pool || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER || t->d.target == NVN_TEXTURE_TARGET_1D_ARRAY) return 0;
    if (tex_storage_dst(t)) return 1;
    return tex_storage_ok(t, !fmt_compressed(t->d.format)) && !tex_cpu_written(t);
}
// not a format the host widens, a depth or multisample texture, or an uncompressed stand-in of a BC format
static int gsb_host_ok(const tex_t *t, id<MTLTexture> mt) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    return mt && bpb && mt.sampleCount == 1 && !mtl_fmt_depth(mt.pixelFormat) && mt.pixelFormat != MTLPixelFormatStencil8 && mtl_pf_block_bytes(mt.pixelFormat) == (size_t)bpb
           && !fmt_compressed(t->d.format) == !mtl_fmt_block(mt.pixelFormat) && mt.width == t->d.width && (mt.textureType != MTLTextureType3D || mt.depth == t->d.depth);
}
// a store that finds the set full holds all storage until the command buffer is committed
static uint64_t g_gsbh_gen; static uint64_t g_gsbh_lo[64], g_gsbh_hi[64]; static int g_gsbh_n, g_gsbh_full;
static void gsb_hold(const void *mem, uint64_t size) {
    uint64_t lo = (uint64_t)(uintptr_t)mem, hi = lo + size;
    if (g_gsbh_gen != g_cbgen + 1) { g_gsbh_gen = g_cbgen + 1; g_gsbh_n = g_gsbh_full = 0; }
    for (int i = 0; i < g_gsbh_n; i++) if (g_gsbh_lo[i] <= lo && hi <= g_gsbh_hi[i]) return;
    if (g_gsbh_n == (int)(sizeof g_gsbh_lo / sizeof g_gsbh_lo[0])) g_gsbh_full = 1; else { g_gsbh_lo[g_gsbh_n] = lo; g_gsbh_hi[g_gsbh_n++] = hi; }
}
static int gsb_held(const void *mem, uint64_t size) {
    if (g_gsbh_gen != g_cbgen + 1) return 0;
    if (g_gsbh_full) return 1;
    uint64_t lo = (uint64_t)(uintptr_t)mem, hi = lo + size;
    for (int i = 0; i < g_gsbh_n; i++) if (lo < g_gsbh_hi[i] && g_gsbh_lo[i] < hi) return 1;
    return 0;
}
// bytes [xb0, xb1) of block rows [by0, by0 + rows) of the level, n images from slice; tx..th: the region in texels, widened to whole blocks
typedef struct { uint64_t base, layer_stride; uint32_t xb0, xb1, by0, rows, th, td, gobs_x, blocks_y, d3, z0, lin, bpb, level; int32_t tx, ty, tw, tth; } gsb_geo;
static int gsb_geom(const tex_t *t, unsigned level, unsigned slice, unsigned n, const NVNcopyRegion *r, gsb_geo *g) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!n || !bpb || level >= (t->d.levels ? t->d.levels : 1)) return -1;
    memset(g, 0, sizeof *g); g->bpb = (uint32_t)bpb; g->level = level;
    int32_t lw = (int32_t)MAX(t->d.width >> level, 1u), lh = (int32_t)MAX(t->d.height >> level, 1u);
    int32_t x0 = 0, y0 = 0, x1 = lw, y1 = lh;
    if (r) { x0 = MAX(r->xoffset, 0); y0 = MAX(r->yoffset, 0); x1 = MIN(r->xoffset + r->width, lw); y1 = MIN(r->yoffset + r->height, lh); }
    if (x1 <= x0 || y1 <= y0) return -1;
    uint32_t bx0 = (uint32_t)(x0 / bw), by0 = (uint32_t)(y0 / bh), bx1 = (uint32_t)((x1 + bw - 1) / bw), by1 = (uint32_t)((y1 + bh - 1) / bh);
    g->tx = (int32_t)bx0 * bw; g->ty = (int32_t)by0 * bh; g->tw = MIN((int32_t)bx1 * bw, lw) - g->tx; g->tth = MIN((int32_t)by1 * bh, lh) - g->ty;
    g->xb0 = bx0 * (uint32_t)bpb; g->xb1 = bx1 * (uint32_t)bpb; g->by0 = by0; g->rows = by1 - by0;
    nvn_texlayout L; nvn_tex_layout(&t->d, &L);
    if (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR) {
        if (t->d.target != NVN_TEXTURE_TARGET_2D || level || slice || n != 1 || bw != 1 || bh != 1 || !t->d.height) return -1;
        size_t srow = tex_linear_row(t);
        if (g->xb1 > srow || (size_t)by1 * srow > t->L.size || srow > UINT32_MAX) return -1;
        g->lin = (uint32_t)srow; return 0;
    }
    uint32_t row, rows, slices, gbpb; unsigned th, td;
    g->d3 = t->d.target == NVN_TEXTURE_TARGET_3D;
    if (nvn_deswizzle_geom(&t->d, level, g->d3 ? 0 : slice, &row, &rows, &slices, &g->base, &th, &td, &gbpb) || gbpb != (uint32_t)bpb || g->xb1 > row || by1 > rows) return -1;
    if (g->d3 ? slice + n > slices : (slices != 1 || td || slice + n > L.layers)) return -1;
    g->th = th; g->td = td; g->z0 = g->d3 ? slice : 0; g->layer_stride = L.layer_size;
    g->gobs_x = MAX(nvn_tex_level_gobs(&t->d, level), (row + 63u) >> 6); g->blocks_y = (rows + (8u << th) - 1) >> (3 + th);
    return 0;
}
static void gsb_missed(const tex_t *t, unsigned level, const char *why);
// src: GPU address of the linear region's first image at column xb0 & ~15
static void gsb_swz(const tex_t *t, const gsb_geo *g, unsigned n, uint64_t src, size_t pitch, size_t img) {
    static id<MTLComputePipelineState> ps; static int failed;
    if (failed) return;
    if (!ps) {
        NSError *e = nil; id<MTLLibrary> lib = host_lib("deswz", &e); id<MTLFunction> fn = [lib newFunctionWithName:@"swz"];
        ps = fn ? dev_cps(fn, &e) : nil;
        if (!ps) { failed = 1; MLOG("[RED] cannot create the GPU swizzle into texture storage: %s (what the GPU writes into textures stays out of their storage)", e.localizedDescription.UTF8String); return; }
    }
    void *pb; uint64_t po, pg, pt = 0, va = (uint64_t)(uintptr_t)t->mem, lo = 0, hi = 0;
    int sparse = t->pool && nvn_sparse_pool(t->pool);
    if (sparse) {   // a table of the GPU addresses of the physical memory behind the pages the store reaches
        unsigned level = g->level;
        uint64_t span = (level + 1 < (t->d.levels ? t->d.levels : 1u) ? nvn_tex_level_offset(&t->d, level + 1) : t->L.layer_size) - nvn_tex_level_offset(&t->d, level);
        lo = g->base; hi = MIN(g->base + (g->d3 ? 0 : (uint64_t)(n - 1) * g->layer_stride) + span, t->L.size);
        if ((va & 15) || hi <= lo) { gsb_missed(t, level, "a sparse texture's storage the store cannot address"); return; }
        uint64_t np = sp_pages_n(va + lo, hi - lo); id<MTLBuffer> tb; size_t to = arena_alloc((size_t)np * 8, &tb);
        if (!sp_pages_gpu(t->pool, va + lo, hi - lo, (uint64_t *)((uint8_t *)tb.contents + to), 1)) return;
        pt = tb.gpuAddress + to; pg = pt;
    } else if (!mtl_gpu_addr(va, t->L.size, &pb, &po, &pg)) return;
    id<MTLBuffer> qb; size_t qo = arena_alloc(160, &qb);
    __unsafe_unretained id o = tex_get(t->serial); NvPkg *pk = [o isKindOfClass:[NvPkg class]] ? o : nil;
    uint64_t e0n, e1s, e1n; id<MTLBuffer> eb = pkg_edges_gpu(pk, t, &e0n, &e1s, &e1n);
    struct { uint64_t src_off, dst_off, base, pitch, img, layer_stride, e0n, e1s, e1n, pt, va, pt_pg; uint32_t x0, x1, y0, rows, th, gobs_x, gob2, td, z0, blocks_y, d3, lin; } *p = (void *)((uint8_t *)qb.contents + qo);
    _Static_assert(sizeof *p <= 160, "the swz parameters fit their arena slot");
    *p = (typeof(*p)){0, 0, g->base, pitch, img, g->layer_stride, e0n, e1s, e1n, pt, va, (va + lo) >> 12, g->xb0, g->xb1, g->by0, g->rows, g->th, g->gobs_x, (uint32_t)nvn_gob_order(&t->d), g->td, g->z0, g->blocks_y, g->d3, g->lin};
    g_wr_epoch++; g_mdx_epoch++; sbw_note(va, t->L.size, SBW_GPU);
    id<MTL4ComputeCommandEncoder> e = dispatch_enc(); enc_c_pso(e, ps);
    enc_c_args(e, src, 0); enc_c_args(e, pg, 1); enc_c_args(e, qb.gpuAddress + qo, 2); enc_c_args(e, eb ? eb.gpuAddress : pg, 3);
    uint32_t nx = (g->xb1 - (g->xb0 & ~15u) + 15) / 16;
    enc_c_dispatch(e, MTLSizeMake((nx + 31) / 32, g->rows, n), MTLSizeMake(32, 1, 1));
    gsb_hold(t->mem, t->L.size);
    if (pk) pk->stored++;
    g_gsb_bytes += (uint64_t)(g->xb1 - g->xb0) * g->rows * n;
    if (g_gsb_stores++ < 4) MLOG("texture in storage the CPU can read ser=%llu %ux%ux%u format %#x: what the GPU wrote is written into its storage on the GPU (bytes %u-%u of %u rows from %u, %u images)",
                                 (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, g->xb0, g->xb1, g->rows, g->by0, n);
}
static void gsb_missed(const tex_t *t, unsigned level, const char *why) {   // the storage of a texture uploaded from it misses this write: its next upload drops it
    static int n; if (tex_storage_dst(t) && n++ < 8) MLOG("[RED] GPU write into ser=%llu %ux%ux%u format %#x target %d level %u not written into its storage (%s): the next upload from there drops it",
                                                         (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, level, why);
}
// r: in texels; NULL for the whole level
static void gsb_store(const tex_t *t, id<MTLTexture> mt, unsigned level, unsigned slice, unsigned n, const NVNcopyRegion *r) {
    if (!n || !mt || !gsb_target(t)) return;
    if (!gsb_host_ok(t, mt) || level >= mt.mipmapLevelCount) { gsb_missed(t, level, "the host texture does not hold its bytes"); return; }
    gsb_geo g; if (gsb_geom(t, level, slice, n, r, &g) || (16 % g.bpb)) { gsb_missed(t, level, "a region or layout the store does not take"); return; }
    if (!g.d3 && slice + n > mt.arrayLength * (mt.textureType == MTLTextureTypeCube || mt.textureType == MTLTextureTypeCubeArray ? 6 : 1)) return;
    uint32_t pad = g.xb0 & 15u;
    size_t pitch = ((size_t)(g.xb1 - (g.xb0 & ~15u)) + 15) & ~(size_t)15, img = pitch * g.rows;
    id<MTLBuffer> ab; size_t ao = arena_alloc(img * n, &ab);
    id<MTL4ComputeCommandEncoder> e = blit();
    for (unsigned z = 0; z < n; z++)
        enc_b_copy_texture_buffer(e, mt, g.d3 ? 0 : slice + z, level, MTLOriginMake((NSUInteger)g.tx, (NSUInteger)g.ty, g.d3 ? slice + z : 0), MTLSizeMake((NSUInteger)g.tw, (NSUInteger)g.tth, 1),
                                  ab, ao + z * img + pad, pitch, img);
    int shift = rb_copy_shift(t, mt);
    if (shift && !rb_swap_gpu(ab.gpuAddress + ao + pad, pitch, img, ab.gpuAddress + ao + pad, pitch, img, (size_t)(g.xb1 - g.xb0), g.rows, n, shift)) return;
    gsb_swz(t, &g, n, ab.gpuAddress + ao, pitch, img);
}
// src already holds the storage's bytes
static void gsb_store_buf(const tex_t *t, unsigned level, unsigned slice, unsigned n, const NVNcopyRegion *r, uint64_t src, size_t row, size_t img) {
    int bw, bh;
    if (!n || !gsb_target(t) || !nvn_fmt_bpb(t->d.format, &bw, &bh) || bw <= 0 || bh <= 0) return;
    gsb_geo g;
    if (r->xoffset < 0 || r->yoffset < 0 || r->xoffset % bw || r->yoffset % bh || gsb_geom(t, level, slice, n, r, &g)
        || (uint32_t)((r->width + bw - 1) / bw) * g.bpb != g.xb1 - g.xb0 || (uint32_t)((r->height + bh - 1) / bh) != g.rows) { gsb_missed(t, level, "a region or layout the store does not take"); return; }
    gsb_swz(t, &g, n, src - (g.xb0 & 15u), row, img);
}
static void rt_mirror_add(const tex_t *t, id<MTLTexture> mt, NSUInteger level, NSUInteger slice, int att) {
    if (!mt || !gsb_target(t)) return;
    if (!gsb_host_ok(t, mt)) { gsb_missed(t, (unsigned)level, "a render target that does not hold its bytes"); return; }
    for (int i = 0; i < g_nrtm; i++) if (g_rtm[i].t.serial == t->serial && g_rtm[i].level == level && g_rtm[i].slice == slice) return;
    if (g_nrtm == (int)(sizeof g_rtm / sizeof g_rtm[0])) { RED_ONCE(131, "more than %d render targets in storage the CPU can read in one pass: the rest are not stored into it", g_nrtm); return; }
    g_rtm[g_nrtm++] = (typeof(g_rtm[0])){*t, (void *)CFBridgingRetain(mt), (unsigned)level, (unsigned)slice, 1, att};
}
static void rt_mirror_layers(MTL4RenderPassDescriptor *pd) {
    for (int i = 0; i < g_nrtm; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[g_rtm[i].att];
        g_rtm[i].slice = (unsigned)(c.texture.textureType == MTLTextureType3D ? c.depthPlane : c.slice); g_rtm[i].n = pd.renderTargetArrayLength > 1 ? (unsigned)pd.renderTargetArrayLength : 1; }
}
// The stores open the copy encoder, which is closed again: callers of rp_end may open an encoder of their own next.
static void rt_mirror_flush(void) {
    int n = g_nrtm; g_nrtm = 0;
    for (int i = 0; i < n; i++) {
        id<MTLTexture> mt = CFBridgingRelease(g_rtm[i].tex);
        gsb_store(&g_rtm[i].t, mt, g_rtm[i].level, g_rtm[i].slice, g_rtm[i].n, NULL);
    }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
}
static int g_nbcp;
static void bcp_flush(void);
static void bcp_wrote(const tex_t *t, id<MTLTexture> m);
static void bcp_idx(int id_, const tex_t *t, int add);
static int bcp_before(const tex_t *t, id<MTLTexture> m, tex_t *o);
static void bcp_after(const tex_t *t, unsigned level, const NVNcopyRegion *r);
static void bcp_after_to(const tex_t *t, const tex_t *op, unsigned level, const NVNcopyRegion *r);
// An empty binding carries no GPU work: the open pass is kept so the next SET_RT can continue it. The old attachments and queued mesh draws are finalized before it takes effect.
static int g_unb; static p_set_rt g_unb_rt; static uint64_t g_unb_n;
static int unb_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_UNBIND_DEFER");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_UNBIND_DEFER=0: unbinding the targets ends the pass");
    }
    return v;
}
static void rp_end_at(const char *fn, int line) { if (g_enc) { AP_B; PD("E %llu pass ends in %s:%d", g_stat_pass, fn, line);
    if (enc_label_on()) { static uint64_t lf = ~0ull, base; uint64_t f = atomic_load(&g_cur_frame); if (f != lf) { lf = f; base = g_stat_pass - 1; }   // the same pass has the same name every frame, so the table can aggregate by name
        [(id)g_enc setLabel:[NSString stringWithFormat:@"p%02llu %dx%d %s d%d", (unsigned long long)(g_stat_pass - base), g_el_w, g_el_h, g_el_key[0] ? g_el_key : "-", g_el_draws]]; }
    sd_finish(); enc_r_end_pass(g_enc); g_gp_bw_l0 = g_bw_load; g_gp_bw_s0 = g_bw_store;
    g_enc = nil; g_rt_merged = 0; g_sm_pass = 0; g_fsp_pass = 0; ts_after(); ms_after();
    if (g_nubd && g_nubd >= ubd_lazy()) ubd_flush();   // across passes deferred updates wait for the next copy or dispatch
    if (g_nbcrt) bcrt_flush();
    if (g_nrtm) rt_mirror_flush();
    if (g_nbcp) { bcp_flush(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; } }   // after the shadows' blocks; the copy encoder is closed again, as rt_mirror_flush does
    AP_E(0);
}
    if (g_unb) { g_unb = 0; R.rt = g_unb_rt; }   // finalize the old attachments and queued mesh draws before applying the unbind
}
// Shared by attach and pso_predict_rest so both agree. Metal can neither attach a compressed format nor make compressed <-> uncompressed views (validation aborts):
// a BC texture with a view of one block per pixel is drawn through a shadow (bcrt_attach), other compressed targets are left out.
static int mtl_fmt_block(MTLPixelFormat f) { return (f >= MTLPixelFormatBC1_RGBA && f <= MTLPixelFormatBC7_RGBAUnorm_sRGB) || (f >= MTLPixelFormatEAC_R11Unorm && f < MTLPixelFormatGBGR422); }   // BC, then EAC / ETC2 and ASTC (no NVN format maps to PVRTC)
static int nvn_fmt_bc(uint32_t f) { return f >= 0x41 && f <= 0x50; }   // 0x41 RGB_DXT1 … 0x50 BC6H unsigned
static int bc_block_bytes(MTLPixelFormat f) {   // 0 for other formats
    if (f < MTLPixelFormatBC1_RGBA || f > MTLPixelFormatBC7_RGBAUnorm_sRGB) return 0;
    return f <= MTLPixelFormatBC1_RGBA_sRGB || f == MTLPixelFormatBC4_RUnorm || f == MTLPixelFormatBC4_RSnorm ? 8 : 16;
}
// Metal can neither attach a depth format as colour nor make a colour view of it: D16 and D32F through a same-size colour view draw through a shadow too
// (D24S8 is Depth32Float_Stencil8 here, whose bytes are not the game's).
static int shrt_block(MTLPixelFormat f, int *side) {   // 0 for any other format
    int bb = bc_block_bytes(f); if (bb) { *side = 4; return bb; }
    *side = 1; return f == MTLPixelFormatDepth16Unorm ? 2 : f == MTLPixelFormatDepth32Float ? 4 : 0;
}
static int mtl_fmt_depth(MTLPixelFormat f) { return f == MTLPixelFormatDepth16Unorm || f == MTLPixelFormatDepth32Float || f == MTLPixelFormatDepth32Float_Stencil8 || f == MTLPixelFormatX32_Stencil8 || f == MTLPixelFormatStencil8; }
static int bcrt_view(id<MTLTexture> base, const view_t *v, MTLPixelFormat *pf_out) {
    uint32_t pf = 0; uint8_t swz[4]; int bw = 0, bh = 0, side = 0, bb = base ? shrt_block(base.pixelFormat, &side) : 0;
    if (!bb || !v->set_format || !nvn_fmt_to_mtl(v->format, &pf, swz) || !pf || mtl_fmt_block((MTLPixelFormat)pf) || mtl_fmt_depth((MTLPixelFormat)pf)) return 0;
    if (nvn_fmt_bpb(v->format, &bw, &bh) != bb || bw != 1 || bh != 1) return 0;
    *pf_out = (MTLPixelFormat)pf; return 1;
}
// The shadow's blocks are copied in through a buffer before the pass and back after it (bcrt_flush); it is always loaded and stored, outside the store elision.
typedef struct { id<MTLTexture> base, shadow; id<MTLBuffer> buf; NSUInteger level, slice, z, w, h, bw, bh, bpb; } bcrt_t;
static bcrt_t g_bcrt[8];
static __strong id<MTLTexture> g_bcrt_sh[8]; static __strong id<MTLBuffer> g_bcrt_buf[8];
static int bcrt_is(id<MTLTexture> t) { for (int i = 0; i < g_nbcrt; i++) if (t && g_bcrt[i].shadow == t) return 1; return 0; }
static void bcrt_flush(void) {
    int n = g_nbcrt; g_nbcrt = 0;
    for (int i = 0; i < n; i++) { bcrt_t e = g_bcrt[i]; g_bcrt[i] = (bcrt_t){0};
        NSUInteger row = e.bw * e.bpb, img = row * e.bh;
        enc_b_copy_texture_buffer(blit(), e.shadow, 0, 0, MTLOriginMake(0, 0, 0), MTLSizeMake(e.bw, e.bh, 1), e.buf, 0, row, img);
        enc_b_copy_buffer_texture(blit(), e.buf, 0, row, img, MTLSizeMake(e.w, e.h, 1), e.base, e.slice, e.level, MTLOriginMake(0, 0, e.z));
    }
}
// returns 1 when a now holds the shadow
static int bcrt_attach(MTLRenderPassAttachmentDescriptor *a, id<MTLTexture> base, const view_t *v, int has_v) {
    MTLPixelFormat spf; int k = g_nbcrt; if (k >= 8 || !has_v || !v || !bcrt_view(base, v, &spf)) return 0;
    int is3d = base.textureType == MTLTextureType3D, side = 1;
    NSUInteger bpb = (NSUInteger)shrt_block(base.pixelFormat, &side);
    NSUInteger lv = a.level, sl = is3d ? 0 : a.slice, z = is3d ? a.depthPlane : 0, w = mip_extent(base.width, lv), h = mip_extent(base.height, lv), bw = (w + side - 1) / side, bh = (h + side - 1) / side;
    NSUInteger row = bw * bpb, img = row * bh;
    if (!g_bcrt_sh[k] || g_bcrt_sh[k].pixelFormat != spf || g_bcrt_sh[k].width != bw || g_bcrt_sh[k].height != bh) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:spf width:bw height:bh mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> sh = [g_dev newTextureWithDescriptor:d]; id<MTLBuffer> buf = [g_dev newBufferWithLength:img options:MTLResourceStorageModePrivate];
        if (!sh || !buf) { RED_ONCE(74, "render target in a compressed or depth texture: cannot create its %lux%lu-block shadow (Metal format %lu)", (unsigned long)bw, (unsigned long)bh, (unsigned long)spf); return 0; }
        if (g_bcrt_sh[k]) { defer_obj_release(g_bcrt_sh[k]); defer_obj_release(g_bcrt_buf[k]); }
        res_add(sh); res_add(buf); g_bcrt_sh[k] = sh; g_bcrt_buf[k] = buf;
    }
    id<MTLTexture> sh = g_bcrt_sh[k]; id<MTLBuffer> buf = g_bcrt_buf[k];
    enc_b_copy_texture_buffer(blit(), base, sl, lv, MTLOriginMake(0, 0, z), MTLSizeMake(w, h, 1), buf, 0, row, img);
    enc_b_copy_buffer_texture(blit(), buf, 0, row, img, MTLSizeMake(bw, bh, 1), sh, 0, 0, MTLOriginMake(0, 0, 0));
    g_bcrt[g_nbcrt++] = (bcrt_t){base, sh, buf, lv, sl, z, w, h, bw, bh, bpb};
    a.texture = sh; a.level = 0; a.slice = 0; a.depthPlane = 0;
    static int once[2]; if (!once[side == 1]++) MLOG("render target in a %s texture: view format %#x drawn into a %lux%lu-block shadow (Metal format %lu) and copied back after the pass (first case pass %llu)",
                                       side == 1 ? "depth" : "compressed", v->format, (unsigned long)bw, (unsigned long)bh, (unsigned long)spf, g_stat_pass + 1);
    return 1;
}
static MTLPixelFormat rt_view_fmt(id<MTLTexture> base, const view_t *v, int has_v) {
    MTLPixelFormat spf;
    if (has_v && bcrt_view(base, v, &spf)) return spf;
    if (base && mtl_fmt_block(base.pixelFormat)) {
        RED_ONCE(78, "render target is a compressed-format texture (Metal format %lu %lux%lu layers %lu, view format change %d → NVN %#x): not attached; draws to this attachment are lost", (unsigned long)base.pixelFormat, (unsigned long)base.width, (unsigned long)base.height,
                 (unsigned long)base.arrayLength, has_v ? v->set_format : 0, has_v ? v->format : 0);
        return MTLPixelFormatInvalid;
    }
    if (!base || !has_v || !v->set_format) return base.pixelFormat;
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(v->format, &pf, swz) || !pf || pf == (uint32_t)base.pixelFormat) return base.pixelFormat;
    if (tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) return base.pixelFormat;
    return (MTLPixelFormat)pf;
}
// The same level / layer at several colour slots: only the first is attached. On TBDR each attachment has its own tile memory stored back at the pass end, so a later
// unwritten copy would overwrite the first one's writes.
static int rt_dup_slot(const p_set_rt *r, int i) {
    if (!r->color[i]) return 0;
    uint8_t lv = r->has_cview[i] && r->cview[i].set_levels ? r->cview[i].base_level : 0; uint16_t ly = r->has_cview[i] && r->cview[i].set_layers ? r->cview[i].min_layer : 0;
    for (int j = 0; j < i; j++) {
        if (!r->color[j] || r->color[j]->serial != r->color[i]->serial) continue;   // records hold copies of the texture objects, so the pointers differ
        uint8_t lj = r->has_cview[j] && r->cview[j].set_levels ? r->cview[j].base_level : 0; uint16_t yj = r->has_cview[j] && r->cview[j].set_layers ? r->cview[j].min_layer : 0;
        if (lj == lv && yj == ly) return 1;
    }
    return 0;
}
// 0: the rest
static NSUInteger att_layers(MTLRenderPassAttachmentDescriptor *a, const view_t *v, int has_v) {
    id<MTLTexture> t = a.texture; if (!t) return 1;
    NSUInteger all = t.textureType == MTLTextureType3D ? MAX(t.depth >> a.level, 1u) : t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    NSUInteger from = t.textureType == MTLTextureType3D ? a.depthPlane : a.slice, n = all > from ? all - from : 1;
    if (has_v && v->set_layers && v->nlayers) n = MIN(n, (NSUInteger)v->nlayers);
    return MAX(n, 1u);
}
// A colour view of another format on a texture without the format-view usage (it costs lossless compression): the texture is upgraded first. Drawing in the base format is no
// fallback for a class change (Metal refuses a uint entry on a float attachment). Refusals are remembered so each costs one try. Replay thread only.
enum { RTV_BAD_N = 32 };
static struct { uint32_t base, view; } g_rtv_bad_pf[RTV_BAD_N]; static int g_rtv_bad_npf;
static uint64_t g_rtv_bad_ser[RTV_BAD_N]; static unsigned g_rtv_bad_nser;   // a ring: the oldest goes first
static int rtv_bad_pair(MTLPixelFormat b, MTLPixelFormat v) {
    for (int i = 0; i < g_rtv_bad_npf; i++) if (g_rtv_bad_pf[i].base == (uint32_t)b && g_rtv_bad_pf[i].view == (uint32_t)v) return 1;
    return 0;
}
static void rtv_note_bad_pair(MTLPixelFormat b, MTLPixelFormat v) { if (!rtv_bad_pair(b, v) && g_rtv_bad_npf < RTV_BAD_N) g_rtv_bad_pf[g_rtv_bad_npf++] = (typeof(*g_rtv_bad_pf)){(uint32_t)b, (uint32_t)v}; }
static int rtv_bad_serial(uint64_t s) { for (unsigned i = 0; i < MIN(g_rtv_bad_nser, (unsigned)RTV_BAD_N); i++) if (g_rtv_bad_ser[i] == s) return 1; return 0; }
// views must keep the texel size; single-sample, plain colour format, not refused before
static int rtv_upgradable(const tex_t *t, id<MTLTexture> base, const view_t *v, MTLPixelFormat *want) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!base || !v->set_format || !nvn_fmt_to_mtl(v->format, &pf, swz) || !pf || !tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) || (base.usage & MTLTextureUsagePixelFormatView)) return 0;
    size_t n = mtl_pf_bytes(base.pixelFormat);
    if (!n || n != mtl_pf_bytes((MTLPixelFormat)pf) || base.sampleCount != 1 || rtv_bad_pair(base.pixelFormat, (MTLPixelFormat)pf) || rtv_bad_serial(t->serial)) return 0;
    *want = (MTLPixelFormat)pf; return 1;
}
static id<MTLTexture> rt_view_upgrade(const tex_t *t, id<MTLTexture> base, const view_t *v) {
    MTLPixelFormat want;
    if (!rtv_upgradable(t, base, v, &want)) return base;
    id<MTLTexture> u = tex_upgrade_pfv(t, base, want);
    if (!((u.parentTexture ? u.parentTexture : u).usage & MTLTextureUsagePixelFormatView)) g_rtv_bad_ser[g_rtv_bad_nser++ % RTV_BAD_N] = t->serial;   // failed or refused: once
    return u;
}
// All before any slot is attached: an upgrade moves every serial sharing the storage to the new texture, so a slot attached earlier would keep the retired one.
// Upgrades copy, so this runs outside any encoder.
static void rt_view_upgrade_all(int clear_only) {
    for (int i = 0; i < R.rt.n && i < 8; i++) {
        if (!R.rt.color[i] || !R.rt.has_cview[i] || !R.rt.cview[i].set_format || (clear_only == 1 && !R.clear_c[i]) || rt_dup_slot(&R.rt, i)) continue;
        rt_view_upgrade(R.rt.color[i], tex_mtl_replay(R.rt.color[i]), &R.rt.cview[i]);
    }
}
static MTLPixelFormat rt_view_fmt_predict(const tex_t *t, id<MTLTexture> base, const view_t *v, int has_v) {
    MTLPixelFormat want;
    if (has_v && rtv_upgradable(t, base, v, &want)) return want;
    return rt_view_fmt(base, v, has_v);
}
static void attach(MTLRenderPassAttachmentDescriptor *a, const tex_t *t, const view_t *v, int has_v) {
    id<MTLTexture> base = tex_mtl_replay(t); sparse_gpu_wrote(t); tu_wrote(base); bcp_wrote(t, base);
    // Metal's validation stops at an attachment without render-target use
    if (base && !(base.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(base.pixelFormat) && base.sampleCount == 1) base = tex_upgrade(t, base, MTLTextureUsageRenderTarget, base.pixelFormat);
    a.texture = base;
    if (has_v && v->set_levels) a.level = v->base_level;
    if (has_v && v->set_layers) { if (t->d.target == 2) a.depthPlane = v->min_layer; else a.slice = v->min_layer; }
    if (base && mtl_fmt_block(base.pixelFormat)) { if (!bcrt_attach(a, base, v, has_v)) { rt_view_fmt(base, v, has_v); a.texture = nil; } return; }   // compressed formats: a shadow or nothing
    if (base && mtl_fmt_depth(base.pixelFormat) && bcrt_attach(a, base, v, has_v)) return;   // a depth texture through a colour view: a shadow
    // A format-changing view is attached as a Metal view of that format: through an RGBA8_SRGB view the hardware encodes sRGB on write. It lives until this submission completes.
    if (base && has_v && v->set_format) {
        base = rt_view_upgrade(t, base, v); a.texture = base;
        MTLPixelFormat pf = rt_view_fmt(base, v, has_v);
        if (pf != base.pixelFormat) {
            id<MTLTexture> vw = [base newTextureViewWithPixelFormat:pf];
            if (vw) { a.texture = vw; defer_obj_unref(vw); }
            else { RED_ONCE(67, "cannot create the render target view: format %#x (base texture Metal format %lu)", v->format, (unsigned long)base.pixelFormat); rtv_note_bad_pair(base.pixelFormat, pf); }
        } else { uint32_t want = 0; uint8_t swz[4];
            if (nvn_fmt_to_mtl(v->format, &want, swz) && want && want != (uint32_t)base.pixelFormat)
                RED_ONCE(68, "render target view format %#x cannot be attached (base texture Metal format %lu was created without the format-view usage): drawing in the base format", v->format, (unsigned long)base.pixelFormat); }
    }
}
// clear_only: only the attachments that have clears (attaching the others would cost a load and a store on TBDR)
static void *rp_begin_ex_ref(int clear_only);
#define rp_begin_ex(clear_only) ((__bridge id<MTL4RenderCommandEncoder>)rp_begin_ex_ref(clear_only))
static NSUInteger g_rp_aw; static int g_rp_whole;
static id<MTL4RenderCommandEncoder> rp_begin(void) { if (g_enc && g_rp_aw) rp_end(); g_rp_whole = 1; id<MTL4RenderCommandEncoder> e = rp_begin_ex(0); g_rp_whole = 0; return e; }   // the callers draw anywhere in the target
static void *rp_begin_ex_(int clear_only);
static void *rp_begin_ex_ref(int clear_only) { AP_B; void *e = rp_begin_ex_(clear_only); AP_E(1); return e; }
static void bare_dims(NSUInteger *w, NSUInteger *h);
// 0 when the registry gives none
static unsigned gpu_cores(void) {
    static unsigned n; static dispatch_once_t once;
    dispatch_once(&once, ^{
        io_iterator_t it;
        if (IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("AGXAccelerator"), &it) != KERN_SUCCESS) return;
        for (io_object_t s; !n && (s = IOIteratorNext(it)); IOObjectRelease(s)) {
            CFTypeRef v = IORegistryEntryCreateCFProperty(s, CFSTR("gpu-core-count"), kCFAllocatorDefault, 0);
            if (v && CFGetTypeID(v) == CFNumberGetTypeID()) CFNumberGetValue((CFNumberRef)v, kCFNumberIntType, &n);
            if (v) CFRelease(v);
        }
        IOObjectRelease(it);
    });
    return n;
}
// A tile-based GPU shades each tile of a pass on one core: a pass with fewer than four 32x32 tiles (times its layers) per GPU core takes 16x16, Metal's smallest
// (8x8 is refused). Smaller tiles everywhere cost more.
static uint64_t g_tile_small_n;
static int tile_small_on(void) { static int m = -1; if (m < 0) m = sw_mode("DM_NVN_TILE_SMALL", 1); return sw_on(m); }
static void pass_dims(MTL4RenderPassDescriptor *pd, NSUInteger nc, NSUInteger *w, NSUInteger *h);
static void tile_small(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    if (!tile_small_on()) return;
    NSUInteger w, h; pass_dims(pd, nc, &w, &h);
    if (pd.renderTargetWidth && pd.renderTargetHeight) { w = MIN(w, pd.renderTargetWidth); h = MIN(h, pd.renderTargetHeight); }
    NSUInteger tiles = ((w + 31) / 32) * ((h + 31) / 32) * MAX(pd.renderTargetArrayLength, (NSUInteger)1);
    unsigned cores = gpu_cores() ?: 6;   // not read: 6, fewer than any Mac's GPU has
    if (tiles >= 4 * cores) return;
    pd.tileWidth = 16; pd.tileHeight = 16; g_tile_small_n++;
    static int once; if (!once++) MLOG("small draw passes take 16x16 tiles: fewer 32x32 tiles than four per GPU core (%lu cores; first case pass %llu, %lux%lu; DM_NVN_TILE_SMALL=0 turns it off)",
                                       (unsigned long)cores, g_stat_pass + 1, (unsigned long)w, (unsigned long)h);
}
static void *rp_begin_ex_(int clear_only) {
    if (g_enc) return (__bridge void *)g_enc;
    if (g_nlc) lc_take_rt();   // waiting lazy clears: none waits past a pass that opens
    g_enc_mode = clear_only; g_pass_swrite = g_pass_order = g_pass_late_res = 0; g_sm_pass = 0; g_fsp_pass = 0;
    if (!R.has_rt) { RED_ONCE(3, "no render target set before the draw"); return NULL; }
    if (g_imfit_npend) img_fit_upgrade();   // no pass is open here: the upgrade's copy may run
    rt_view_upgrade_all(clear_only);   // before any slot is attached
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
    int any = 0;
    for (int i = 0; i < R.rt.n; i++) {
        if (!R.rt.color[i] || (clear_only == 1 && !R.clear_c[i])) continue;
        if (rt_dup_slot(&R.rt, i)) continue;
        attach(pd.colorAttachments[i], R.rt.color[i], &R.rt.cview[i], R.rt.has_cview[i]);
        if (!pd.colorAttachments[i].texture) continue;
        any = 1;
        pd.colorAttachments[i].loadAction = R.clear_c[i] ? MTLLoadActionClear : MTLLoadActionLoad;
        pd.colorAttachments[i].clearColor = R.cc[i];
        if (R.clear_c[i]) PD("C %llu c%d cleared to %g,%g,%g,%g", g_stat_pass + 1, i, R.cc[i].red, R.cc[i].green, R.cc[i].blue, R.cc[i].alpha);
        if (bcrt_is(pd.colorAttachments[i].texture)) { pd.colorAttachments[i].storeAction = MTLStoreActionStore; continue; }   // a shadow: loaded and stored as is, outside the bookkeeping below
        pd.colorAttachments[i].storeAction = MTLStoreActionStore;
        rt_mirror_add(R.rt.color[i], pd.colorAttachments[i].texture, pd.colorAttachments[i].level,
                      pd.colorAttachments[i].texture.textureType == MTLTextureType3D ? pd.colorAttachments[i].depthPlane : pd.colorAttachments[i].slice, i);
    }
    if (g_fsp_plan.on && !clear_only && !R.rt.depth && !g_nrtm) {
        MTLRenderPassColorAttachmentDescriptor *c0 = pd.colorAttachments[0]; id<MTLTexture> t = c0.texture; int n = g_fsp_plan.n;
        int ok = t && t.pixelFormat == (MTLPixelFormat)g_fsp_plan.fmt && t.textureType != MTLTextureType3D && !bcrt_is(t);
        NSUInteger lo = NSUIntegerMax, hi = 0, sl[8];
        for (int i = 0; i < n && ok; i++) { MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
            ok = c.texture == t && c.level == c0.level && c.loadAction == MTLLoadActionLoad; sl[i] = c.slice; lo = MIN(lo, sl[i]); hi = MAX(hi, sl[i]);
            for (int j = 0; j < i && ok; j++) ok = sl[j] != sl[i]; }
        for (int i = n; i < 8 && ok; i++) ok = !pd.colorAttachments[i].texture;
        if (ok && hi - lo < 8) {
            for (int i = 0; i < n; i++) g_fsp_layer[i] = (uint8_t)(sl[i] - lo);
            for (int i = 1; i < n; i++) pd.colorAttachments[i] = nil;
            c0.slice = lo; pd.renderTargetArrayLength = hi - lo + 1; pd.supportColorAttachmentMapping = YES;
            g_fsp_pass = 1; g_fsp_n = n; g_fsp_npass++;
            static int once; if (!once++) MLOG("face-split draw: a draw into %d layers of one texture level draws once per layer, one colour output each (first case pass %llu; DM_NVN_FACE_SPLIT)", n, g_stat_pass + 1);
            PD("L %llu face-split pass: targets 0-%d are layers %lu-%lu of one attachment", g_stat_pass + 1, n - 1, (unsigned long)lo, (unsigned long)hi);
        }
    }
    if (g_sm_plan.on && !clear_only && pd.colorAttachments[0].texture && !R.rt.depth) {
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[0];
        g_sm_cur = (int)c.slice; c.slice = (NSUInteger)g_sm_plan.lo; if (c.loadAction == MTLLoadActionDontCare) c.loadAction = MTLLoadActionLoad;   // the other layers' content stays
        pd.renderTargetArrayLength = (NSUInteger)(g_sm_plan.hi - g_sm_plan.lo + 1);
        g_sm_pass = 1; g_sm_lo = g_sm_plan.lo; g_sm_hi = g_sm_plan.hi; g_sm_applied = -1; g_sm_npass++;
        static int once; if (!once++) MLOG("slice-merged pass: draws into layers %d-%d of one texture level run as one layered pass (first case pass %llu; DM_NVN_SLICE_MERGE)", g_sm_lo, g_sm_hi, g_stat_pass + 1);
        PD("L %llu slice-merged pass: layers %d-%d, first draw into layer %d", g_stat_pass + 1, g_sm_lo, g_sm_hi, g_sm_cur);
    }
    if (R.rt.depth && !(clear_only == 1 && !R.clear_d && !R.clear_s)) {
        attach(pd.depthAttachment, R.rt.depth, &R.rt.dview, R.rt.has_dview);
        id<MTLTexture> t = pd.depthAttachment.texture;
        MTLPixelFormat pf = t.pixelFormat;
        // Combined storage stays on both attachments, as the pipeline keys name both planes; the unused plane loads and stores unchanged.
        int combined = pf == MTLPixelFormatDepth32Float_Stencil8;
        int depth = combined || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float;
        int stencil = combined || pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatX32_Stencil8;
        if (t && depth) {
            pd.depthAttachment.loadAction = R.clear_d ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.depthAttachment.clearDepth = pf == MTLPixelFormatDepth16Unorm ? nvn_depth16_clear_value(R.cd) : R.cd; pd.depthAttachment.storeAction = MTLStoreActionStore;
        } else pd.depthAttachment.texture = nil;
        if (t && stencil) {
            pd.stencilAttachment.texture = t; pd.stencilAttachment.level = pd.depthAttachment.level;
            pd.stencilAttachment.slice = pd.depthAttachment.slice; pd.stencilAttachment.depthPlane = pd.depthAttachment.depthPlane;
            pd.stencilAttachment.loadAction = R.clear_s ? MTLLoadActionClear : MTLLoadActionLoad;
            pd.stencilAttachment.clearStencil = R.cs; pd.stencilAttachment.storeAction = MTLStoreActionStore;
        }
        any |= t && (depth || stencil);
    }
    // No target at all (a fragment shader that only stores to a storage buffer): an attachment-less pass covering the viewport, else the scissor.
    int bare = !any && clear_only != 1 && !R.rt.depth;
    for (int i = 0; i < R.rt.n && bare; i++) if (R.rt.color[i]) bare = 0;
    if (bare) {
        NSUInteger bw, bh; bare_dims(&bw, &bh);
        pd.renderTargetWidth = bw; pd.renderTargetHeight = bh; pd.defaultRasterSampleCount = 1;
        static int once; if (!once++) MLOG("attachment-less pass: the game draws with no render target set, pass opened at the viewport size %lux%lu (first case pass %llu)", (unsigned long)bw, (unsigned long)bh, g_stat_pass + 1);
        PD("P %llu attachment-less %lux%lu", g_stat_pass + 1, (unsigned long)bw, (unsigned long)bh);
        any = 1;
    }
    if (!any) {
        RED_ONCE(4, "no render target could get an MTLTexture");
        if (passdump()) {
            char b[256]; size_t o = 0;
            for (int i = 0; i < R.rt.n && i < 8; i++) if (R.rt.color[i]) o += snprintf(b + o, sizeof b - o, " c%d=%llu/f%#x%s", i, (unsigned long long)R.rt.color[i]->serial, R.rt.color[i]->d.format, fmt_compressed(R.rt.color[i]->d.format) ? "(compr)" : "");
            if (R.rt.depth) o += snprintf(b + o, sizeof b - o, " d=%llu", (unsigned long long)R.rt.depth->serial);
            PD("N %llu pass skipped (no attachment could be attached) mode=%d%s", g_stat_pass + 1, clear_only, b);
        }
        return NULL;
    }
    g_rp_aw = g_rp_ah = 0;
    if (rpa_on() && !g_rp_whole && !clear_only && R.has_sc && !R.clear_d && !R.clear_s) {
        int cl = 0; for (int i = 0; i < 8; i++) cl |= R.clear_c[i];
        NSUInteger tw = 0, th = 0; if (!cl) rt_dims(&tw, &th);
        if (tw * th >= (1u << 24)) { NSUInteger w = MIN(tw, (NSUInteger)MAX((long)R.sc.x + R.sc.w, 1)), h = MIN(th, (NSUInteger)MAX((long)R.sc.y + R.sc.h, 1));
            if (w * h * 2 <= tw * th) { pd.renderTargetWidth = w; pd.renderTargetHeight = h; g_rp_aw = w; g_rp_ah = h; } } }
    // A pass covers only its smallest attachment and its layers, while NVN clears the whole target view: a larger cleared attachment is cleared in a pass of its own first.
    {   NSUInteger pw, ph; pass_dims(pd, (NSUInteger)R.rt.n, &pw, &ph);
        for (NSUInteger i = 0; i < (NSUInteger)R.rt.n + 1; i++) {
            MTLRenderPassAttachmentDescriptor *a = i < (NSUInteger)R.rt.n ? pd.colorAttachments[i] : pd.depthAttachment.texture ? pd.depthAttachment : pd.stencilAttachment;
            int ds = i == (NSUInteger)R.rt.n, cd = ds && pd.depthAttachment.texture && pd.depthAttachment.loadAction == MTLLoadActionClear, cs = ds && pd.stencilAttachment.texture && pd.stencilAttachment.loadAction == MTLLoadActionClear;
            if (!a.texture || (ds ? !cd && !cs : a.loadAction != MTLLoadActionClear)) continue;
            NSUInteger nl = att_layers(a, ds ? &R.rt.dview : &R.rt.cview[i], ds ? R.rt.has_dview : R.rt.has_cview[i]);
            if (nl < 2 && mip_extent(a.texture.width, a.level) <= pw && mip_extent(a.texture.height, a.level) <= ph) continue;
            MTL4RenderPassDescriptor *cp = [MTL4RenderPassDescriptor new];
            if (nl > 1) cp.renderTargetArrayLength = nl;
            if (!ds) { MTLRenderPassColorAttachmentDescriptor *c = cp.colorAttachments[0], *o = pd.colorAttachments[i];
                c.texture = o.texture; c.level = o.level; c.slice = o.slice; c.depthPlane = o.depthPlane; c.clearColor = o.clearColor; c.loadAction = MTLLoadActionClear; c.storeAction = MTLStoreActionStore;
                o.loadAction = MTLLoadActionLoad; }
            else {
                if (cd) { MTLRenderPassDepthAttachmentDescriptor *d = cp.depthAttachment, *o = pd.depthAttachment;
                    d.texture = o.texture; d.level = o.level; d.slice = o.slice; d.clearDepth = o.clearDepth; d.loadAction = MTLLoadActionClear; d.storeAction = MTLStoreActionStore; o.loadAction = MTLLoadActionLoad; }
                if (cs) { MTLRenderPassStencilAttachmentDescriptor *s = cp.stencilAttachment, *o = pd.stencilAttachment;
                    s.texture = o.texture; s.level = o.level; s.slice = o.slice; s.clearStencil = o.clearStencil; s.loadAction = MTLLoadActionClear; s.storeAction = MTLStoreActionStore; o.loadAction = MTLLoadActionLoad; }
            }
            PD("C %llu %s%lu cleared in a pass of its own (%lux%lu, %lu layers; the pass covers %lux%lu)", g_stat_pass + 1, ds ? "d" : "c", (unsigned long)i, (unsigned long)mip_extent(a.texture.width, a.level),
               (unsigned long)mip_extent(a.texture.height, a.level), (unsigned long)nl, (unsigned long)pw, (unsigned long)ph);
            enc_r_end(enc_r_begin(cp));
        }
    }
    int nload = 0, natt = 0;
    for (int i = 0; i < R.rt.n; i++) {
        MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
        if (!c.texture) continue;
        uint64_t b = att_bytes(c.texture, c.level); natt++; g_bw_store += b; if (c.loadAction == MTLLoadActionLoad) { g_bw_load += b; nload++; }
        if (R.rt.has_cview[i] && R.rt.cview[i].set_format) PD("V %llu c%d view format %#x (base texture NVN format %#x)", g_stat_pass + 1, i, R.rt.cview[i].format, R.rt.color[i]->d.format);
        PD("P %llu c%d ser=%llu fmt=%lu %lux%lu ms%lu lv%lu sl%lu %s mtl=%p", g_stat_pass + 1, i, R.rt.color[i]->serial, (unsigned long)c.texture.pixelFormat,
           (unsigned long)c.texture.width, (unsigned long)c.texture.height, (unsigned long)c.texture.sampleCount, (unsigned long)c.level, (unsigned long)c.slice,
           c.loadAction == MTLLoadActionClear ? "clear" : c.loadAction == MTLLoadActionDontCare ? "dontcare" : "load", (__bridge void *)(c.texture.parentTexture ? c.texture.parentTexture : c.texture));
    }
    {
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t whole = att_bytes(dt ? dt : stt, dt ? pd.depthAttachment.level : pd.stencilAttachment.level);
        int comb = (dt ? dt : stt).pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
        uint64_t db = dt ? (comb ? whole / 5 * 4 : whole) : 0, sb = stt && stt != dt ? (comb ? whole / 5 : whole) : stt ? whole - db : 0;
        if (dt) { natt++; g_bw_store += db; if (pd.depthAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += db; nload++; } }
        if (stt) { natt += !dt; g_bw_store += sb; if (pd.stencilAttachment.loadAction == MTLLoadActionLoad) { g_bw_load += sb; nload += !dt; } }
        if (dt) PD("P %llu d ser=%llu fmt=%lu %lux%lu %s", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)dt.pixelFormat,
                   (unsigned long)dt.width, (unsigned long)dt.height, pd.depthAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
        else if (stt) PD("P %llu s ser=%llu fmt=%lu %lux%lu %s (stencil only)", g_stat_pass + 1, R.rt.depth->serial, (unsigned long)stt.pixelFormat,
                   (unsigned long)stt.width, (unsigned long)stt.height, pd.stencilAttachment.loadAction == MTLLoadActionClear ? "clear" : "load");
    }
    if (natt && nload == natt) g_pass_full_rw++;
    g_nsd = 0;
    {
        // a standalone clear has stored already; only a clear load action here writes this pass's tiles
        for (int i = 0; i < R.rt.n && i < 8; i++) {
            MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[i];
            if (!c.texture || bcrt_is(c.texture)) continue;
            g_sd[g_nsd++] = (sd_att){.ser = R.rt.color[i]->serial, .bytes = att_bytes(c.texture, c.level), .lv = (uint32_t)c.level,
                                     .sl = (uint32_t)(c.texture.textureType == MTLTextureType3D ? c.depthPlane : c.slice), .idx = i,
                                     .load = c.loadAction == MTLLoadActionLoad, .wrote = R.clear_c[i] != 0, .rast = c.loadAction == MTLLoadActionClear};
            c.storeAction = MTLStoreActionUnknown;
        }
        id<MTLTexture> dt = pd.depthAttachment.texture, stt = pd.stencilAttachment.texture;
        uint64_t db = att_bytes(dt, pd.depthAttachment.level), sb = 0;
        if (stt) {
            int comb = stt.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
            sb = att_bytes(stt, pd.stencilAttachment.level) / (comb ? 5 : 1);
            if (stt == dt) db -= sb;
        }
        if (dt) {
            g_sd[g_nsd++] = (sd_att){.ser = R.rt.depth->serial, .bytes = db, .lv = (uint32_t)pd.depthAttachment.level, .sl = (uint32_t)pd.depthAttachment.slice, .idx = -1,
                                     .depth = 1, .load = pd.depthAttachment.loadAction == MTLLoadActionLoad, .wrote = R.clear_d != 0};
            pd.depthAttachment.storeAction = MTLStoreActionUnknown;
        }
        if (stt) {
            g_sd[g_nsd++] = (sd_att){.ser = R.rt.depth->serial, .bytes = sb, .lv = (uint32_t)pd.stencilAttachment.level, .sl = (uint32_t)pd.stencilAttachment.slice, .idx = -1,
                                     .depth = 2, .load = pd.stencilAttachment.loadAction == MTLLoadActionLoad, .wrote = R.clear_s != 0};
            pd.stencilAttachment.storeAction = MTLStoreActionUnknown;
        }
    }
    uint32_t ds_clear = (R.clear_d && pd.depthAttachment.texture ? FB_DS_DEPTH : 0) |
                        (R.clear_s && pd.stencilAttachment.texture ? FB_DS_STENCIL : 0);
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
    {
        uint64_t ser = g_pass.serial + 1; memset(&g_pass, 0, sizeof g_pass); g_pass.serial = ser;
        for (int i = 0; i < R.rt.n && i < 8; i++) { id<MTLTexture> t = R.rt.color[i] ? pd.colorAttachments[i].texture : nil;
            if (t) { g_pass.cfmt[i] = (uint32_t)t.pixelFormat; g_pass.samples = (uint32_t)t.sampleCount; } }
        id<MTLTexture> dt = R.rt.depth ? pd.depthAttachment.texture : nil, stt = R.rt.depth ? pd.stencilAttachment.texture : nil;
        g_pass.hd = dt != nil; g_pass.hs = stt != nil;
        g_pass.dfmt = dt ? (uint32_t)dt.pixelFormat : 0; g_pass.sfmt = stt ? (uint32_t)stt.pixelFormat : 0;
        PD("J pass=%llu depth-stencil NVN=%#x view=%#x Metal=%u/%u use=%d/%d mode=%d", g_stat_pass + 1, R.rt.depth ? R.rt.depth->d.format : 0,
           R.rt.has_dview && R.rt.dview.set_format ? R.rt.dview.format : 0, g_pass.dfmt, g_pass.sfmt, g_pass.hd, g_pass.hs, clear_only);
        if (dt || stt) g_pass.samples = (uint32_t)(dt ? dt : stt).sampleCount;
        pass_dims(pd, (NSUInteger)R.rt.n, &g_pass.tw, &g_pass.th);
        g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
    }
    if (g_vis_on) {
        // only a previous overflow buffer is retired: a ring buffer stays in rotation and in the residency set
        if (!g_vis) vis_take();
        if (!g_vis || g_vis_n >= VIS_SLOTS) { if (g_vis && !vis_in_ring(g_vis)) defer_obj_release(g_vis); g_vis = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis); g_vis_n = 0; }
        pd.visibilityResultBuffer = g_vis;
    }
    g_enc_vis = 0; g_enc_has_vis = g_vis_on;
    if (g_nrtm) rt_mirror_layers(pd);
    g_fsm_na = 0;
    if (g_fsm_np) { if (g_fsp_pass) fsm_attach(pd); g_fsm_np = 0; }
    { void td_note(id pd, NSUInteger nc); td_note(pd, (NSUInteger)R.rt.n);
      void ts_note(id pd, NSUInteger nc); ts_note(pd, (NSUInteger)R.rt.n);
      void ms_note(id pd, NSUInteger nc); ms_note(pd, (NSUInteger)R.rt.n); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    tile_small(pd, (NSUInteger)R.rt.n);
    g_stat_pass++;
    g_enc = enc_r_begin_pass(pd);
    if (g_enc && ds_clear) fb_ds_note_write(ds_clear);
    args_dirty_all();
    if (enc_label_on()) { id<MTLTexture> lt = pd.colorAttachments[0].texture ?: pd.depthAttachment.texture ?: pd.stencilAttachment.texture; g_el_w = (int)lt.width; g_el_h = (int)lt.height; g_el_draws = 0; g_el_key[0] = 0; }
    return (__bridge void *)g_enc;
}
// the selected level of the first valid target bounds the pass's size
static void rt_dims(NSUInteger *tw, NSUInteger *th) {
    id<MTLTexture> t0 = nil; NSUInteger level = 0;
    for (int i = 0; i < R.rt.n && i < 8 && !t0; i++) if (R.rt.color[i]) {
        t0 = tex_mtl_replay(R.rt.color[i]); level = R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0;
    }
    if (!t0 && R.rt.depth) { t0 = tex_mtl_replay(R.rt.depth); level = R.rt.has_dview && R.rt.dview.set_levels ? R.rt.dview.base_level : 0; }
    *tw = t0 ? mip_extent(t0.width, level) : 16384; *th = t0 ? mip_extent(t0.height, level) : 16384;   // no targets: an attachment-less pass can be up to 16384
}
static void rp_flush_at(const char *fn, int line) {
    int pend = R.clear_d || R.clear_s;
    for (int i = 0; i < 8; i++) pend |= R.clear_c[i];
    if (pend && !g_enc && g_cb) rp_begin_ex(1);
    rp_end_at(fn, line);
}
static int scissor_full(const tex_t *t, const view_t *v, int has_v) {
    NSUInteger level = has_v && v->set_levels ? v->base_level : 0;
    uint32_t w, h; tex_lsize(t, (unsigned)MIN(level, 32ul), &w, &h, NULL);
    return !R.has_sc || (R.sc.x <= 0 && R.sc.y <= 0 && (int64_t)R.sc.x + R.sc.w >= (int64_t)w && (int64_t)R.sc.y + R.sc.h >= (int64_t)h);
}
// Multisample, 3D, depth and compressed formats are not handled (0: the caller clears another way).
static int clear_rect_zero(int i) {
    const tex_t *t = R.rt.color[i]; id<MTLTexture> mt = tex_mtl_replay(t); if (!mt) return 0;
    MTLTextureType tt = mt.textureType;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray || tt == MTLTextureType3D || tt == MTLTextureTypeTextureBuffer) return 0;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat); if (!bpp) return 0;
    const view_t *v = &R.rt.cview[i]; int hv = R.rt.has_cview[i];
    NSUInteger lv = hv && v->set_levels ? v->base_level : 0, sl = hv && v->set_layers ? v->min_layer : 0;
    long lw = (long)MAX(mt.width >> lv, 1u), lh = (long)MAX(mt.height >> lv, 1u);
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > lw) x1 = lw; if (y1 > lh) y1 = lh;
    if (x1 <= x0 || y1 <= y0) return 1;
    size_t row = (size_t)(x1 - x0) * bpp;
    id<MTLBuffer> zb = zero_buf(row); if (!zb) return 0;   // in pieces of rows, so the zero buffer stays at its 4 MB
    lc_land(); rp_flush();   // waiting clears, then a pending full clear on the same attachment, land first or they would overwrite this one
    sparse_gpu_wrote(t); tu_wrote(mt); tex_t pair; int paired = bcp_before(t, mt, &pair);
    NVNcopyRegion rr = {(int)x0, (int)y0, 0, (int)(x1 - x0), (int)(y1 - y0), 1};
    long step = MAX(1L, MIN(y1 - y0, (long)(zb.length / row)));
    for (long y = y0; y < y1; y += step) { long n = MIN(step, y1 - y);
        enc_b_copy_buffer_texture(blit(), zb, 0, row, row * (size_t)n, MTLSizeMake((NSUInteger)(x1 - x0), (NSUInteger)n, 1), mt, sl, lv, MTLOriginMake((NSUInteger)x0, (NSUInteger)y, 0)); }
    { id<MTLTexture> whole = mt; while (whole.parentTexture) whole = whole.parentTexture; gsb_store(t, whole, (unsigned)lv, (unsigned)sl, 1, &rr); }
    if (paired) bcp_after_to(t, &pair, (unsigned)lv, &rr);
    static uint64_t n; if (!n++) MLOG("scissored clear: first case %ldx%ld @%ld,%ld (target %lux%lu level %lu)", x1 - x0, y1 - y0, x0, y0, (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)lv);
    return 1;
}
