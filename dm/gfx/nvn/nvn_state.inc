static struct {
    const prog_t *prog;
    const prog_t *cprog;
    p_prim_restart restart;
    const prog_t *bst[5];      // NVN stage bits 1 vertex, 2 fragment, 8 task, 16 mesh (4, geometry, has no shader here); indices as in NvProg.sh
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    va_st attr[32]; int nattr; vs_st stream[16]; int nstream;
    NVNbufferRange vbuf[16];
    p_bind_buf ubo[NVN_RESOURCE_STAGES][16], ssbo[NVN_RESOURCE_STAGES][16]; uint16_t ubo_set[NVN_RESOURCE_STAGES];   // ubo_set: slots a bind has set, an empty binding included
    uint64_t tex[NVN_RESOURCE_STAGES][NTEX], samp[NVN_RESOURCE_STAGES][128], img[NVN_RESOURCE_STAGES][32];   // pool-relative hardware handles
    uint32_t st_vmask[2], st_wmask[2], st_ref[2];
    p_poly_offset po; float vp[16][4]; float dr[2]; uint8_t vp_origin[16]; int has_vp; int depth_clamp; float line_width;
    uint8_t vp_sw[16][4];   // XOR the identity
    float blend_color[4], alpha_ref; int64_t copy_stride, copy_img_stride;
    uint8_t push[2][DMSH_PUSH_BLOCKS * DMSH_PUSH_SIZE];
    int rast_discard;
    int tex_base, samp_base;
} S;
// An explicit viewport keeps the device origin of its recording; the implicit full-target viewport takes the current one.
static int vp_origin_ll(void) {
    return S.vp_origin[0] ? S.vp_origin[0] == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT + 1 : atomic_load_explicit(&g_origin_ll, memory_order_relaxed);
}
static inline int resource_range_fits(int stage, int first, int count, int slots) {
    return (unsigned)stage < NVN_RESOURCE_STAGES && first >= 0 && count >= 0 && first <= slots && count <= slots - first;
}
// Relative IDs resolve at use, after command lists inherited or changed the queue's pools: never save host IDs in command memory or binding state.
static inline uint64_t bound_handle(uint64_t h) {
    unsigned t = (uint32_t)h & 0xfffff, s = (uint32_t)h >> 20;
    return (t ? t + (uint64_t)S.tex_base : 0) | (s ? s + (uint64_t)S.samp_base : 0) << 20;
}
static inline uint64_t bound_tex(int stage, int reg) { return bound_handle(tex_slot_h(S.tex[stage], S.img[stage], reg)); }
static inline uint64_t bound_img(int stage, int reg) { return bound_handle(S.img[stage][reg]); }
// past slot 31 the image there
static uint64_t c2_handle(int stage, unsigned b) {
    if (b >= DMSH_C2_TEX && b < DMSH_C2_SEPTEX && !((b - DMSH_C2_TEX) % DMSH_C2_ROW)) return bound_tex(stage, (b - DMSH_C2_TEX) / DMSH_C2_ROW);
    if (b >= DMSH_C2_SEPTEX && b < DMSH_C2_SAMP && !((b - DMSH_C2_SEPTEX) % DMSH_C2_ROW)) return bound_handle(S.tex[stage][NVN_SEPTEX_SLOT0 + (b - DMSH_C2_SEPTEX) / DMSH_C2_ROW]);
    if (b >= DMSH_C2_SAMP && b < DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS && !((b - DMSH_C2_SAMP) % DMSH_C2_ROW)) return bound_handle(S.samp[stage][(b - DMSH_C2_SAMP) / DMSH_C2_ROW]);
    return 0;
}
// Argument blocks are valid by generation, from one sequence for every stage: a per-stage flag would not do, as another shader of the stage could write its block in between.
// Starting numbers differ per stage, so a block is never current on a stage it was not written for.
static uint64_t g_args_seq = NVN_RESOURCE_STAGES, g_args_gen[NVN_RESOURCE_STAGES] = {1, 2, 3, 4, 5, 6}, g_args_ugen[NVN_RESOURCE_STAGES];
static uint64_t g_args_wrote[NVN_RESOURCE_STAGES];
static inline void args_dirty(int st) { g_args_gen[st] = ++g_args_seq; }
static inline void args_dirty_ubo(int st) { g_args_ugen[st] = ++g_args_seq; }
static inline void args_dirty_all(void) { for (int st = 0; st < NVN_RESOURCE_STAGES; st++) args_dirty(st); }
static inline int args_changed(int st) { return g_args_gen[st] > g_args_wrote[st] || g_args_ugen[st] > g_args_wrote[st]; }
static void ubd_dirty(uint64_t lo, uint64_t hi) {
    for (int st = 0; st < NVN_RESOURCE_STAGES; st++) {
        if (args_changed(st)) continue;
        for (int r = 0; r < 16; r++) {
            const p_bind_buf *u = &S.ubo[st][r];
            if (u->addr && lo < u->addr + u->size && u->addr < hi) { args_dirty_ubo(st); break; }
        }
    }
}
// NVN permits byte-aligned vertex streams, Metal drops the low two address bits: such a stream is bound as an aligned copy. The same copy bounds a stream the draw reads past:
// Maxwell's fetch reads zeros past the bound size, Metal's reads on.
#define VBUF_COPY_MAX (64u << 20)
typedef struct { uint64_t gpu, epoch, writes; uint32_t sbw; int npend; } vbuf_copy;
static nvn_hmap g_vbuf_unaligned = {.klen = 24};
static uint64_t g_vbuf_write_epoch;
static uint64_t g_vbuf_need[16];
static uint64_t vbuf_unaligned(uint64_t addr, uint64_t size, uint64_t need, int prepare) {   // need: 0, or the copy's length (zeros past size)
    uint64_t key[3] = {addr, size, need}, len = need > size ? need : size;
    vbuf_copy *c = nvn_hm_get(&g_vbuf_unaligned, key);
    if (c && !prepare) return c->gpu;
    if (c) {
        int changed = c->epoch != g_wr_epoch || c->writes != g_vbuf_write_epoch || c->sbw != g_sbw_gen || c->npend != g_npend;
        int dirty = size && (g_pend_over || g_sbw_over || (changed && nvn_sparse_gen())
            || (changed && sbw_hits(addr, addr + size))
            || (c->npend != g_npend && pend_last(addr, size) >= c->npend));
        if (!dirty) {
            c->epoch = g_wr_epoch; c->writes = g_vbuf_write_epoch; c->sbw = g_sbw_gen; c->npend = g_npend;
            return c->gpu;
        }
    }
    void *mb = NULL; uint64_t mo = 0;
    if (len > VBUF_COPY_MAX || (size && !mtl_addr(addr, size, &mb, &mo))) {
        if (need) RED_ONCE(121, "a draw reads %llu bytes of the vertex stream at %#llx, bound with %llu: no copy padded with zeros, so Metal reads past the bound range", (unsigned long long)need, (unsigned long long)addr, (unsigned long long)size);
        else RED_ONCE(120, "vertex stream at an address not 4-byte aligned (%#llx, %llu bytes) could not be copied: bound as it is, which Metal reads from the aligned address below", (unsigned long long)addr, (unsigned long long)size);
        return 0;
    }
    id<MTLBuffer> src = (__bridge id<MTLBuffer>)mb;
    int cpu = !size || (!pend_hits(addr, size) && !g_sbw_over && !sbw_hits(addr, addr + size) && !gw_live(addr, addr + size)
        && !nvn_sparse_gen() && nvn_host_readable(addr, size) && src.contents);   // a sparse mapping may alias this physical range
    if (!cpu && !prepare) MDIE("unaligned vertex stream was not prepared before its render encoder");
    id<MTLBuffer> ab; size_t off = arena_alloc((size_t)len, &ab);
    if (len > size) memset((uint8_t *)ab.contents + off + size, 0, (size_t)(len - size));
    if (cpu) { if (size) memcpy((uint8_t *)ab.contents + off, (const uint8_t *)src.contents + mo, (size_t)size); }
    else {
        g_blit_alias = 1;
        id<MTL4ComputeCommandEncoder> e = blit();   // may end an encoder, so the flag is set again
        g_blit_alias = 1;
        enc_b_copy_buffer(e, src, mo, ab, off, size);
    }
    g_vbuf_copies++; g_vbuf_copy_bytes += size;
    if (!c) {
        c = malloc(sizeof *c);
        if (!c) MDIE("cannot allocate vertex copy cache entry");
        nvn_hm_put(&g_vbuf_unaligned, key, c);
    }
    *c = (vbuf_copy){ab.gpuAddress + off, g_wr_epoch, g_vbuf_write_epoch, g_sbw_gen, g_npend};
    return c->gpu;
}
static int vattr_bytes(int f) {   // 0 = not a vertex format
    static const uint8_t c[12] = {1, 1, 1, 1, 2, 2, 2, 2, 2, 4, 4, 4};   // per component of 0x01-0x30: UNORM8 SNORM8 UINT8 SINT8, F16 UNORM16 SNORM16 UINT16 SINT16, F32 UINT32 SINT32
    if (f >= 0x01 && f <= 0x30) return c[(f - 1) % 12] * ((f - 1) / 12 + 1);
    if (f == 0x3D || f == 0x69 || f == 0x3F || f == 0x9A) return 4;   // 10:10:10:2, 11:11:10 float, BGRA8
    if (f >= 0x51 && f <= 0x68) { int i = (f - 0x51) >> 1; return (i / 3 + 1) * (1 << (i % 3)); }   // integers read as floats: 8 / 16 / 32-bit components
    return 0;
}
// 0 = not known on the CPU
static int vstream_last(int st, const p_draw *dw, const p_draw_idx *di, uint64_t *last) {
    if ((unsigned)st >= 16) return 0;
    if ((dw && (dw->ind || dw->mdi_cnt)) || (di && (di->ind || di->mdi_cnt)) || (!dw && !di)) return 0;
    uint32_t stride = S.stream[st].stride, div = S.stream[st].divisor;
    if (!stride) { *last = 0; return 1; }
    if (div) {
        int32_t n = dw ? dw->instances : di->instances, b = dw ? dw->base_instance : di->base_instance;
        if (n < 1) n = 1;
        if (b < 0) return 0;
        *last = (uint64_t)b + (uint64_t)(n - 1) / div;
        return 1;
    }
    if (!dw || dw->first < 0 || dw->count <= 0) return 0;
    *last = (uint64_t)dw->first + (uint64_t)dw->count - 1;
    return 1;
}
static void vbuf_need(const nvn_refl *rf, const p_draw *dw, const p_draw_idx *di) {
    memset(g_vbuf_need, 0, sizeof g_vbuf_need);
    for (int v = 0; rf && v < rf->nv; v++) {
        int a = rf->v[v].nvn;
        if (a >= 32 || a >= S.nattr || !S.attr[a].fmt) continue;
        int st = S.attr[a].stream, eb = vattr_bytes(S.attr[a].fmt); uint64_t last;
        if (st >= 16 || !eb || !S.vbuf[st].address || !vstream_last(st, dw, di, &last)) continue;
        uint64_t end = last * S.stream[st].stride + S.attr[a].offset + (uint64_t)eb;
        if (end > S.vbuf[st].size && end > g_vbuf_need[st]) g_vbuf_need[st] = end;
    }
}
static void vbuf_prepare(const nvn_refl *rf, const p_draw *dw, const p_draw_idx *di) {   // NULL dw / di: no bounds
    static uint64_t sub, gen, sparse;
    uint64_t pool_gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire), sparse_gen = nvn_sparse_gen();
    if (sub != g_sub_serial || gen != pool_gen || sparse != sparse_gen) nvn_hm_clear(&g_vbuf_unaligned, free);
    vbuf_need(rf, dw, di);
    for (int s = 0; s < 16; s++) {
        if (!((S.vbuf[s].address & 3) && S.vbuf[s].size) && !g_vbuf_need[s]) continue;
        static int once;
        if (g_vbuf_need[s] && !once++) MLOG("a draw reads %llu bytes of vertex stream %d, bound with %llu: bound as a copy with zeros past the bound range, as Maxwell's fetch reads them", (unsigned long long)g_vbuf_need[s], s, (unsigned long long)S.vbuf[s].size);
        vbuf_unaligned(S.vbuf[s].address, S.vbuf[s].size, g_vbuf_need[s], 1);
    }
    sub = g_sub_serial; gen = pool_gen; sparse = sparse_gen;
}
static unsigned vs_streams(const nvn_refl *rf) {
    unsigned m = 0;
    for (int v = 0; rf && v < rf->nv; v++) {
        int a = rf->v[v].nvn;
        if (a < 32 && a < S.nattr && S.attr[a].fmt && S.attr[a].stream < 16) m |= 1u << S.attr[a].stream;
    }
    return m;
}
// Slot 30: zeros at offset 0, and at 16 an absent attribute's (0, 0, 0, 1.0) in register bits, as NVN gives it (ZERO_ATTR)
static id<MTLBuffer> vertex_constants(void) {
    static id<MTLBuffer> b;
    if (!b) { b = [g_dev newBufferWithLength:64 options:MTLResourceStorageModeShared]; res_add(b); ((uint32_t *)b.contents)[7] = 0x3f800000; }
    return b;
}
// A slot with an address in no pool would keep an earlier draw's buffer, so the caller skips the draw; address 0 reads the zero buffer (ZERO_ATTR).
static int bind_vbufs(id<MTL4RenderCommandEncoder> en, const nvn_refl *rf) {
    // apart from the table cache, which vconv_bind may overwrite
    static struct { uint64_t addr, gpu, gen; } cache[16];
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_acquire);
    unsigned used = vs_streams(rf); int bad = -1;
    // only the streams the shader's inputs read, which the pipeline's vertex descriptor names (pso_desc); every bound one with no shader
    unsigned want = rf ? used : 0xffffu;
    for (int s = 0; s < 16; s++) {
        uint64_t addr = S.vbuf[s].address, gpu;
        if (!addr || !(want >> s & 1)) continue;
        if (((addr & 3) && S.vbuf[s].size) || g_vbuf_need[s]) {
            uint64_t c = vbuf_unaligned(addr, S.vbuf[s].size, g_vbuf_need[s], 0);
            if (c) {
                cache[s].addr = 0;
                enc_r_vbuf(en, VB_SLOT(s), c);
                continue;
            }
        }
        if (g_vbuf_cache && cache[s].addr == addr && cache[s].gen == gen) { gpu = cache[s].gpu; g_vbuf_reuse++; }
        else {
            void *mb; uint64_t mo; g_vbuf_resolve++;
            if (!mtl_gpu_addr(addr, S.vbuf[s].size ? S.vbuf[s].size : 1, &mb, &mo, &gpu)) {
                cache[s].addr = 0;
                if (bad < 0 && used >> s & 1) bad = s;
                continue;
            }
            cache[s].addr = addr; cache[s].gpu = gpu; cache[s].gen = gen;
        }
        enc_r_vbuf(en, VB_SLOT(s), gpu);
    }
    static uint64_t zero_gpu;
    if (!zero_gpu) zero_gpu = vertex_constants().gpuAddress;
    enc_r_vbuf(en, 30, zero_gpu);
    return bad;
}
enum { PULL_SLOT = 16 };   // = dmsh_pull.py SLOT
typedef struct { uint64_t base; uint32_t stride, divisor; } pull_stream;
typedef struct { pull_stream s[16]; uint32_t attr[32]; } pull_layout;
_Static_assert(sizeof(pull_layout) == 384, "dmsh_pull.metal dm_vlayout");
static void pull_layout_fill(pull_layout *Lp, const nvn_refl *rf) {
    pull_layout L;
    memset(&L, 0, sizeof L);
    uint32_t lo[16] = {0};
    unsigned want = rf ? vs_streams(rf) : 0xffffu;   // as bind_vbufs
    for (int st = 0; st < 16; st++) {
        uint64_t addr = S.vbuf[st].address, gpu; void *mb; uint64_t mo;
        if (!addr || !(want >> st & 1)) continue;
        if (!g_vbuf_need[st] || !(gpu = vbuf_unaligned(addr, S.vbuf[st].size, g_vbuf_need[st], 0)))
            if (!mtl_gpu_addr(addr, S.vbuf[st].size ? S.vbuf[st].size : 1, &mb, &mo, &gpu)) continue;
        L.s[st].base = gpu & ~3ull; lo[st] = (uint32_t)(gpu & 3);
        L.s[st].stride = S.stream[st].stride; L.s[st].divisor = S.stream[st].divisor;
    }
    for (int a = 0; a < 32 && a < S.nattr; a++) {
        va_st v = S.attr[a];
        if (!v.fmt) continue;   // absent: dm_fetch gives (0, 0, 0, 1.0); a stream without a buffer keeps its word and reads zeros
        if (v.stream < 16) {
            if (!(want >> v.stream & 1)) continue;
            if (!L.s[v.stream].base && S.vbuf[v.stream].address) RED_ONCE(159, "vertex stream %d's buffer %#llx is in no memory pool; its attributes read zeros", v.stream, (unsigned long long)S.vbuf[v.stream].address);
            v.offset += lo[v.stream];
        }
        memcpy(&L.attr[a], &v, 4);
    }
    *Lp = L;
}
static void pull_bind(id<MTL4RenderCommandEncoder> en, const nvn_refl *rf) {
    pull_layout L; pull_layout_fill(&L, rf);
    id<MTLBuffer> ab; size_t off = arena_alloc(sizeof L, &ab);
    memcpy((uint8_t *)ab.contents + off, &L, sizeof L);
    enc_r_vbuf(en, PULL_SLOT, ab.gpuAddress + off);
}
static void bare_dims(NSUInteger *w, NSUInteger *h) {
    double x1 = S.has_vp ? MAX(S.vp[0][0], S.vp[0][0] + S.vp[0][2]) : R.has_sc ? (double)R.sc.x + R.sc.w : 1;
    double y1 = S.has_vp ? MAX(S.vp[0][1], S.vp[0][1] + S.vp[0][3]) : R.has_sc ? (double)R.sc.y + R.sc.h : 1;
    *w = (NSUInteger)MIN(MAX(ceil(x1), 1.0), 16384.0); *h = (NSUInteger)MIN(MAX(ceil(y1), 1.0), 16384.0);
}
// Metal gives no order between draws of one render pass for shader writes, so an NVN ordering barrier after such a draw ends the pass.
// → bit 0 vertex, bit 1 fragment, bit 2 another
static int prog_swrites(NvProg *pr) {
    if (pr && pr->c_sw) return pr->c_sw - 1;
    int w = 0;
    for (int st = 0; pr && st < 6; st++) {
        __unsafe_unretained NvShader *sh = pr->sh[st];
        if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) {
            const nvn_rbind *b = &sh->rf.b[i];
            if (b->rw && (b->kind == NVN_RB_IMG || b->kind == NVN_RB_IMGU || b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_GMEM)) { w |= st == 0 ? 1 : st == 1 ? 2 : 4; break; }
        }
    }
    if (pr) pr->c_sw = (int8_t)(w + 1);
    return w;
}
static void order_note(NvProg *pr) {
    for (int st = 1; pr && st < 6 && !g_pass_late_res; st++) {
        __unsafe_unretained NvShader *sh = pr->sh[st];
        if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) {
            int k = sh->rf.b[i].kind;
            if (k != NVN_RB_SAMP && k != NVN_RB_SAMPT && k != NVN_RB_SAMPU && k != NVN_RB_SAMPI && k != NVN_RB_SAMPD && k != NVN_RB_SAMPA && k != NVN_RB_SAMPP && k != NVN_RB_SAMPH
                && k != NVN_RB_SUPPORT && k != NVN_RB_C1 && k != NVN_RB_DRV0 && k != NVN_RB_DRV2 && k != NVN_RB_PUSH && k != NVN_RB_CBSIZE && k != NVN_RB_TBIAS && k != NVN_RB_SBLEN) {
                g_pass_late_res = 1;
                break;
            }
        }
    }
}
// Metal has no barrier from the fragment stage back to the vertex stage inside a pass: an in-pass barrier only serves earlier draws that read memory in their vertex stage alone,
// or a barrier that orders only primitives; otherwise the pass ends first.
static void order_draw(NvProg *pr, int merged) {   // merged: readable-tile, slice-merged or deferred pass
    int w = g_enc && g_pass_order ? prog_swrites(pr) : 0;
    if (!w) return;
    if (!(w & 4) && !((g_pass_order & 2) && g_pass_late_res) && !merged) {
        [g_enc barrierAfterEncoderStages:MTLStageVertex beforeEncoderStages:(w & 1 ? MTLStageVertex : 0) | (w & 2 ? MTLStageFragment : 0) visibilityOptions:resource_vis()];
        g_pass_order = 0; g_order_vbar++;
        static int once;
        if (!once++) MLOG("a shader write after a game barrier in a draw pass: a barrier from the vertex stage in the pass (first case pass %llu)", g_stat_pass);
        PD("M %llu a shader write after an ordering barrier; barrier from the vertex stage in the pass", g_stat_pass);
        return;
    }
    static int once;
    if (!once++) MLOG("a draw with shader writes after a game barrier in a draw pass: the pass ends before it (first case pass %llu)", g_stat_pass);
    PD("M %llu a draw with shader writes after an ordering barrier; split here", g_stat_pass);
    rp_end();
}
static void swrite_note(NvProg *pr) {
    if (!pr) return;
    if (g_pass_swrite) { g_vbuf_write_epoch++; return; }
    if (prog_swrites(pr)) { g_pass_swrite = 1; g_vbuf_write_epoch++; }
}
static void udc_note_draw(NvProg *pr, uint32_t raster_mask) {
    int image_write = pr && pr->c_iw ? pr->c_iw - 1 : 0;
    __unsafe_unretained NvShader *fs = pr ? pr->sh[1] : nil;
    uint32_t shader_color_mask = fs ? (fs->rf.color_known ? fs->rf.color_mask : UINT32_MAX) : 0;
    // bindless shaders need not use the bound slots: any writable image or storage pointer may alias an attachment
    if (pr && !pr->c_iw) for (int st = 0; st < 6; st++) {
        __unsafe_unretained NvShader *sh = pr->sh[st];
        if (!sh) continue;
        for (int i = 0; i < sh->rf.nb; i++) {
            const nvn_rbind *b = &sh->rf.b[i];
            if (b->rw && (b->kind == NVN_RB_IMG || b->kind == NVN_RB_IMGU || b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_SBUFU || b->kind == NVN_RB_SBUFQ || b->kind == NVN_RB_GMEM)) { image_write = 1; break; }
        }
        if (image_write) break;
    }
    if (pr && !pr->c_iw) pr->c_iw = (int8_t)(image_write + 1);
    if (R.has_rt) for (int i = 1; i < R.rt.n && i < 8; i++)   // a slot attached twice (left unattached) really written: the writes are lost
        if ((shader_color_mask >> i & 1) && ((S.chmask.mask >> (4 * i)) & 0xf) && rt_dup_slot(&R.rt, i))
            RED_ONCE(79, "color slot %d holds the same texture as an earlier slot (ser=%llu) and this draw writes it (mask %#x): these writes are lost", i, (unsigned long long)R.rt.color[i]->serial, (S.chmask.mask >> (4 * i)) & 0xf);
    for (int i = 0; i < g_nsd; i++) {
        sd_att *t = &g_sd[i];
        int color_write = t->depth == 0 && t->idx >= 0 && (raster_mask >> t->idx & 1);
        if (image_write) t->img = 1;
        if (color_write) t->rast = 1;
        if (image_write) { t->wrote = 1; t->wwhy = 1; }
        else if (color_write) { t->wrote = 1; t->wwhy = 2; }
        else if (t->depth == 1 && S.ds.dw) { t->wrote = 1; t->wwhy = 4; }
        else if (t->depth == 2 && S.ds.st && (S.st_wmask[0] || S.st_wmask[1])
                && (S.ds.fail_f != NVN_STENCIL_OP_KEEP || S.ds.zfail_f != NVN_STENCIL_OP_KEEP || S.ds.zpass_f != NVN_STENCIL_OP_KEEP
                    || S.ds.fail_b != NVN_STENCIL_OP_KEEP || S.ds.zfail_b != NVN_STENCIL_OP_KEEP || S.ds.zpass_b != NVN_STENCIL_OP_KEEP)) { t->wrote = 1; t->wwhy = 5; }
    }
}
// NVN's BindProgram binds per stage: the other stages stay. Mixed stages make a composite prog_t carrying only a serial; task / mesh bits 0x40, 0x80 are stages 3 and 4.
static inline int gfx_stage_bits(int stages) { return (stages & 0x1f) | (stages & 0x40 ? 8 : 0) | (stages & 0x80 ? 16 : 0); }
static const prog_t *bind_stages(const prog_t *p, int stages) {
    for (int st = 0; st < 5; st++) if (stages >> st & 1) S.bst[st] = p;
    const prog_t *v = S.bst[0] ? S.bst[0] : S.bst[4];   // a mesh program has no vertex stage
    if (!v) return NULL;
    int same = 1;
    for (int st = 1; st < 5; st++) if (S.bst[st] && S.bst[st] != v) same = 0;
    if (same && S.bst[1] == v) return v;
    enum { NCMP = 64 };
    static prog_t cp[NCMP];
    static const prog_t *ck[NCMP][5];
    static uint64_t cs[NCMP][5];
    static int ncp, next;
    for (int i = 0; i < ncp; i++) {
        int hit = 1;
        for (int st = 0; st < 5 && hit; st++) hit = ck[i][st] == S.bst[st] && (!S.bst[st] || cs[i][st] == S.bst[st]->serial);
        if (hit) return &cp[i];
    }
    int i = ncp < NCMP ? ncp++ : next++ % NCMP;
    // Never the slot S.prog names: OP_BIND_PROGRAM tells a change by the pointer, so reusing it would keep stale argument blocks.
    if (&cp[i] == S.prog) i = next++ % NCMP;
    if (cp[i].serial) serial_del(cp[i].serial);
    NvProg *o = [NvProg new];
    for (int st = 0; st < 5; st++) {
        ck[i][st] = S.bst[st];
        cs[i][st] = S.bst[st] ? S.bst[st]->serial : 0;
        if (S.bst[st]) {
            __unsafe_unretained NvProg *src = prog_get(S.bst[st]);
            o->sh[st] = src ? src->sh[st] : nil;
        }
    }
    memset(&cp[i], 0, sizeof cp[i]);
    cp[i].magic = MAGIC('P', 'R', 'O', 'G'); cp[i].serial = nvn_new_serial();
    serial_put(cp[i].serial, o);
    static int once;
    if (!once++) MLOG("per-stage program binding: first combined program (vertex %s fragment %s)", o->sh[0] ? o->sh[0]->key : "-", o->sh[1] ? o->sh[1]->key : "-");
    return &cp[i];
}
// After S was restored from a saved state, the ring slot it names may hold another combination: taken again from S.bst.
static void prog_resolve(void) {
    const prog_t *np = bind_stages(NULL, 0);
    if (np == S.prog) return;
    S.prog = np; g_pso_dirty = 1;
    static unsigned n;
    if (!n++ || !(n & (n - 1))) MLOG("combined program taken again after a state restore: its ring slot had been reused (%u times so far)", n);
}
static int g_state_inited;
static void state_reset(void) {
    g_rc_skip=0;
    g_state_inited = 1;
    memset(&S, 0, sizeof S);
    g_pso_dirty = g_dss_dirty = 1;
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
    S.line_width = 1;
}

// Bindings belong to a queue: a game may destroy one and make another.
typedef struct queue_state {
    uint64_t serial; typeof(S) s; typeof(R) r;
    tex_t rt_tex[9]; uint64_t rt_pins[9]; prog_t programs[6]; int rc_skip;
    struct queue_state *next;
} queue_state;
static queue_state *g_queue_states, *g_active_queue;
// The bound targets' descriptions are in command memory the game may overwrite once this submission finishes: copied into the queue's state.
static void rt_preserve_bound(void) {
    if (!R.has_rt) return;
    static tex_t replay_tex[9];
    tex_t *owned = g_active_queue ? g_active_queue->rt_tex : replay_tex;
    const tex_t *source[9]; unsigned pending = 0;
    for (int k = 0; k < 9; k++) {
        source[k] = k == 8 ? R.rt.depth : k < R.rt.n ? R.rt.color[k] : NULL;
        if (source[k] && source[k] != &owned[k]) pending |= 1u << k;
    }
    if (g_active_queue) for(int k=0;k<9;k++) {
        uint64_t serial=source[k]?source[k]->serial:0, old=g_active_queue->rt_pins[k];
        if(serial!=old) {
            mtl_tex_pin(serial);
            mtl_tex_unpin(old);
            g_active_queue->rt_pins[k]=serial;
        }
    }
    if (!pending) return;
    tex_t next[9];
    for (int k = 0; k < 9; k++) if (pending & (1u << k)) next[k] = *source[k];
    for (int k = 0; k < 9; k++) if (pending & (1u << k)) {
        owned[k] = next[k];
        if (k == 8) R.rt.depth = &owned[k];
        else R.rt.color[k] = &owned[k];
    }
}
static void queue_program_preserve(void) {
    queue_state *q=g_active_queue;
    if(!q) return;
    const prog_t *bound[6]={S.bst[0],S.bst[1],S.bst[2],S.bst[3],S.bst[4],S.cprog};
    int changed=0;
    for(int i=0;i<6;i++) if(bound[i] && ((uintptr_t)bound[i]<(uintptr_t)q->programs || (uintptr_t)bound[i]>=(uintptr_t)(q->programs+6))) changed=1;
    if(!changed) return;
    prog_t next[6]={0}; int index[6],count=0;
    for(int i=0;i<6;i++) {
        index[i]=-1;
        if(!bound[i]) continue;
        int k=0; while(k<count && next[k].serial!=bound[i]->serial) k++;
        if(k==count) { mtl_prog_snapshot(&next[k],bound[i]); count++; }
        index[i]=k;
    }
    for(int i=0;i<6;i++) if(q->programs[i].serial) mtl_prog_unpin(&q->programs[i]);
    memset(q->programs,0,sizeof q->programs);
    for(int i=0;i<count;i++) q->programs[i]=next[i];
    for(int i=0;i<5;i++) S.bst[i]=index[i]<0?NULL:&q->programs[index[i]];
    S.cprog=index[5]<0?NULL:&q->programs[index[5]];
    S.prog=bind_stages(NULL,0);
    args_dirty_all();
}
void mtl_queue_activate(uint64_t serial) {   // caller holds the submission lock
    if (g_active_queue && g_active_queue->serial == serial) return;
    // another queue's work must see all prior writes
    g_last_end_relaxed = 0;
    if (g_active_queue) {
        g_active_queue->s = S; g_active_queue->r = R;
        g_active_queue->rc_skip=g_rc_skip;
    }
    queue_state *q = g_queue_states;
    while (q && q->serial != serial) q = q->next;
    if (q) {
        S = q->s; R = q->r;
        g_rc_skip=q->rc_skip;
        prog_resolve();
    } else {
        q = calloc(1, sizeof *q);
        if (!q) MDIE("queue state allocation failed");
        q->serial = serial; q->next = g_queue_states; g_queue_states = q;
        state_reset(); memset(&R, 0, sizeof R);
    }
    g_active_queue = q; g_pso_dirty = g_dss_dirty = 1; args_dirty_all();
}
void mtl_queue_forget(uint64_t serial) {   // caller holds the submission lock
    queue_state **p = &g_queue_states;
    while (*p && (*p)->serial != serial) p = &(*p)->next;
    if (!*p) return;
    queue_state *q = *p; *p = q->next;
    if (g_active_queue == q) {
        g_active_queue = NULL;
        state_reset(); memset(&R, 0, sizeof R);
        args_dirty_all();
    }
    for(int i=0;i<6;i++) if(q->programs[i].serial) mtl_prog_unpin(&q->programs[i]);
    for(int i=0;i<9;i++) mtl_tex_unpin(q->rt_pins[i]);
    free(q);
}
// NVN reads a program only when bound, so a game may finalize one a queue still has bound and reuse its memory.
static int prog_unbind(typeof(S) *s, const prog_t *p) {
    int n = 0;
    if (s->prog == p) { s->prog = NULL; n++; }
    if (s->cprog == p) { s->cprog = NULL; n++; }
    for (int st = 0; st < 5; st++) if (s->bst[st] == p) { s->bst[st] = NULL; n++; }
    return n;
}
void mtl_prog_forget(const prog_t *p) {   // caller holds the submission lock
    const prog_t *prog = S.prog, *cprog = S.cprog;
    int active = prog_unbind(&S, p) > 0, inactive = 0;
    if (active) S.prog = bind_stages(NULL, 0);
    if (S.prog != prog || S.cprog != cprog) { g_pso_dirty = 1; args_dirty_all(); }
    for (queue_state *q = g_queue_states; q; q = q->next) if (q != g_active_queue && prog_unbind(&q->s, p)) inactive++;
    if (active || inactive) {
        static unsigned n;
        if (!n++ || !(n & (n - 1))) MLOG("finalized program %p dropped from queue bindings (active queue %d, inactive queues %d; %u times so far)", (const void *)p, active, inactive, n);
    }
}
