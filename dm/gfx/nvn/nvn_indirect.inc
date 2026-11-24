// Indirect quads, fans and 8-bit indices: the GPU usually writes the record earlier in the frame, so a compute pass (dm/gfx/msl/ixc.metal) converts it into an indexed indirect draw.
// A compute pass cannot run inside a render pass: the first such draw converts every later one up to the next buffer write (g_wr_epoch), so only one met inside an open pass splits it.
#define IXC_MAX 128
#define IXC_CAP (3 * 16384)
typedef struct { uint64_t ind, ibuf; int32_t prim, itype; uint64_t args, ixb; uint32_t ixlen; } ixc_ent;   // args 0: the record could not be converted
static ixc_ent g_ixc[IXC_MAX]; static int g_ixc_n; static uint64_t g_ixc_epoch, g_ixc_sub, g_ixc_runs, g_ixc_splits;
static int ixc_wanted(const p_draw *dw, const p_draw_idx *di) {
    if (dw) return dw->ind && (dw->prim == 6 || dw->prim == 7);
    return di->ind && (di->prim == 6 || di->prim == 7 || di->itype == 0);
}
static const ixc_ent *ixc_find(const p_draw *dw, const p_draw_idx *di) {
    if (g_ixc_epoch != g_wr_epoch || g_ixc_sub != g_sub_serial) return NULL;
    uint64_t ind = dw ? dw->ind : di->ind, ib = dw ? 0 : di->ibuf; int prim = dw ? dw->prim : di->prim, it = dw ? -1 : di->itype;
    for (int i = 0; i < g_ixc_n; i++) if (g_ixc[i].ind == ind && g_ixc[i].ibuf == ib && g_ixc[i].prim == prim && g_ixc[i].itype == it) return &g_ixc[i];
    return NULL;
}
static void ixc_take(const p_draw *dw, const p_draw_idx *di) {
    if (g_ixc_n < IXC_MAX && !ixc_find(dw, di)) g_ixc[g_ixc_n++] = (ixc_ent){dw ? dw->ind : di->ind, dw ? 0 : di->ibuf, dw ? dw->prim : di->prim, dw ? -1 : di->itype, 0, 0, 0};
}
static void ixc_run(const p_draw *dw, const p_draw_idx *di) {
    static id<MTLComputePipelineState> ps[2]; static id<MTL4ArgumentTable> at; static int failed;
    if (failed) return;
    if (!at) { NSError *e = nil; id<MTLLibrary> lib = host_lib("ixc", &e);
        for (int k = 0; k < 2 && lib; k++) { id<MTLFunction> f = [lib newFunctionWithName:k ? @"ixc_expand" : @"ixc_arrays"]; ps[k] = f ? dev_cps(f, &e) : nil; }
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; at = ps[0] && ps[1] ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!at) { failed = 1; MLOG("[RED] indirect draw conversion: cannot create the compute pipeline %s (indirect draws of quads / fans / 8-bit indices are skipped)", e.localizedDescription.UTF8String); return; } }
    if (g_ixc_epoch != g_wr_epoch || g_ixc_sub != g_sub_serial || g_ixc_n > IXC_MAX - 8) { g_ixc_n = 0; g_ixc_epoch = g_wr_epoch; g_ixc_sub = g_sub_serial; }
    int n0 = g_ixc_n; ixc_take(dw, di);
    if (g_exec_next && g_ret_n <= 8) {   // later draws in replay order, up to the next buffer write
        op_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
        const uint8_t *q; op_hdr h; const void *a;
        for (int guard = 0; guard < 200000 && g_ixc_n < IXC_MAX && op_next(&cur, &q, &h, &a); guard++) {
            if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_UPDATE_UB || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B) break;
            p_draw dt; p_draw_idx it; const p_draw *d = h.op == OP_DRAW ? draw_rec(a, h.size, &dt) : NULL; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
            if (d && ixc_wanted(d, NULL)) ixc_take(d, NULL);
            else if (x && ixc_wanted(NULL, x)) ixc_take(NULL, x);
        }
    }
    if (g_enc) { g_ixc_splits++; PD("M %llu indirect draw conversion: render pass split here", g_stat_pass); rp_end(); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    int n = g_ixc_n - n0, na = 0; if (n <= 0) return;
    typedef struct { uint64_t rec, args; uint32_t prim, cap; } ajob;
    typedef struct { uint64_t rec, ib, out, args; uint32_t prim, isz, ilen, cap; } ejob;
    id<MTLBuffer> ab; size_t ao = arena_alloc((size_t)n * (32 + sizeof(ajob)), &ab); uint64_t ag = ab.gpuAddress + ao;   // n argument records (32 bytes apart), then the arrays jobs
    ajob *aj = (ajob *)((uint8_t *)ab.contents + ao + (size_t)n * 32);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); [ce setArgumentTable:at];
    for (int i = 0; i < n; i++) {
        ixc_ent *e = &g_ixc[n0 + i]; uint64_t rec, ib = 0; void *mb; uint64_t mo; e->args = 0;
        if (!mtl_gpu_addr(e->ind, e->itype < 0 ? 16 : 20, &mb, &mo, &rec)) continue;
        if (e->itype < 0) {
            if (e->prim == 7) quad_ib_make(); else fan_ib_make();
            e->ixb = e->prim == 7 ? g_quad_gpu : g_fan_gpu; e->ixlen = (uint32_t)(e->prim == 7 ? g_quad_ib.length : g_fan_ib.length);
            aj[na++] = (ajob){rec, ag + 32 * (uint64_t)i, (uint32_t)e->prim, e->ixlen / 4}; e->args = ag + 32 * (uint64_t)i;
        } else {
            if (!mtl_gpu_addr(e->ibuf, 1, &mb, &mo, &ib)) continue;
            id<MTLBuffer> ob; size_t oo = arena_alloc(IXC_CAP * 4 + sizeof(ejob), &ob);
            ejob *j = (ejob *)((uint8_t *)ob.contents + oo + IXC_CAP * 4);
            *j = (ejob){rec, ib, ob.gpuAddress + oo, ag + 32 * (uint64_t)i, (uint32_t)e->prim, e->itype == 0 ? 1u : e->itype == 1 ? 2u : 4u, (uint32_t)MIN(((__bridge id<MTLBuffer>)mb).length - mo, 0xffffffffull), IXC_CAP};
            at_addr(at, ob.gpuAddress + oo + IXC_CAP * 4, 0, "index conversion table slot", 0); [ce setComputePipelineState:ps[1]];
            [ce dispatchThreadgroups:MTLSizeMake(IXC_CAP / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
            e->ixb = ob.gpuAddress + oo; e->ixlen = IXC_CAP * 4; e->args = ag + 32 * (uint64_t)i;
            { static int once; if (!once++) MLOG("indirect indexed draw (primitive %d, %d-byte indices) expanded to 32-bit indices on the GPU, at most %d", e->prim, e->itype == 0 ? 1 : e->itype == 1 ? 2 : 4, IXC_CAP); }
        }
    }
    if (na) { at_addr(at, ag + (uint64_t)n * 32, 0, "index conversion table slot", 0); [ce setComputePipelineState:ps[0]]; [ce dispatchThreadgroups:MTLSizeMake((NSUInteger)na, 1, 1) threadsPerThreadgroup:MTLSizeMake(1, 1, 1)]; }
    enc_c_end(ce);
    args_dirty_all();
    if (!g_ixc_runs++) MLOG("indirect draw conversion: first time, %d draws (records for quads / fans / 8-bit indices become indexed indirect arguments in a compute pass before the render pass)", n);
    else if (g_ixc_runs % 3600 == 0) MLOG("indirect draw conversion: %llu times, render pass split %llu times", (unsigned long long)g_ixc_runs, (unsigned long long)g_ixc_splits);
    static int lg = -1; if (lg < 0) { const char *v = getenv("DM_NVN_IXCLOG"); lg = v ? atoi(v) : 0; }
    for (int i = 0; i < n && lg > 0; i++) { const ixc_ent *e = &g_ixc[n0 + i]; if (!e->args) continue; lg--;
        const uint32_t *src = (const uint32_t *)(uintptr_t)e->ind, *arg = (const uint32_t *)((const uint8_t *)ab.contents + ao + 32 * (size_t)i);
        int prim = e->prim, it = e->itype; unsigned long long fr = (unsigned long long)atomic_load(&g_cur_frame), pass = g_stat_pass, splits = g_ixc_splits; id<MTLBuffer> keep = ab;
        [g_commit_blocks addObject:^{ (void)keep; MLOG("indirect draw conversion frame %llu pass %llu: primitive %d index type %d record %u,%u,%u,%u → arguments %u,%u,%u,%d,%u (splits so far %llu)", fr, pass, prim, it, src[0], src[1], src[2], src[3], arg[0], arg[1], arg[2], (int)arg[3], arg[4], splits); }]; }
}
// 8-bit indices a buffer write earlier in this submission covers (pend_hits): Metal has no 8-bit index type and the CPU would read stale bytes, so they are widened on the GPU
// (dm/gfx/msl/wide.metal), batched as in ixc_run. Adapted from MacNativeGames' widen_u8_gpu (github.com/ly05010419/MacNativeGames, MIT).
#define WIDE_MAX 256
typedef struct { uint64_t ibuf; int32_t count; uint64_t gpu; } wide_ent;   // gpu: 0 = not widened
static wide_ent g_wide[WIDE_MAX]; static int g_wide_n; static uint64_t g_wide_epoch, g_wide_sub, g_wide_runs, g_wide_draws, g_wide_splits;
static int wide_wanted(const p_draw_idx *di) { return !di->ind && di->itype == 0 && di->prim != 6 && di->prim != 7 && di->count > 0 && pend_hits(di->ibuf, (uint64_t)di->count); }
static const wide_ent *wide_find(const p_draw_idx *di) {
    if (g_wide_epoch != g_wr_epoch || g_wide_sub != g_sub_serial) return NULL;
    for (int i = 0; i < g_wide_n; i++) if (g_wide[i].ibuf == di->ibuf && g_wide[i].count == di->count) return &g_wide[i];
    return NULL;
}
static void wide_take(const p_draw_idx *di) { if (g_wide_n < WIDE_MAX && !wide_find(di)) g_wide[g_wide_n++] = (wide_ent){di->ibuf, di->count, 0}; }
static void fsp_encode(id<MTL4RenderCommandEncoder> en, MTLPrimitiveType mp, const p_draw *dw, const p_draw_idx *di) {
    uint64_t gpu = 0, il = 0; MTLIndexType ty = MTLIndexTypeUInt32;
    if (di && di->itype == 0) {
        const wide_ent *w = wide_wanted(di) ? wide_find(di) : NULL; ty = MTLIndexTypeUInt16; il = (uint64_t)di->count * 2;
        if (w && w->gpu) gpu = w->gpu;
        else { void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, di->count, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
            id<MTLBuffer> ab; size_t off = arena_alloc(((size_t)di->count * 2 + 15) & ~(size_t)15, &ab);
            uint16_t *d = (uint16_t *)((uint8_t *)ab.contents + off); src += mo;
            for (int32_t i = 0; i < di->count; i++) d[i] = src[i];
            gpu = g_cur_arena->gpu + off; }
    } else if (di) { void *mb; uint64_t mo;
        if (!mtl_gpu_addr(di->ibuf, (uint64_t)di->count * (di->itype == 1 ? 2 : 4), &mb, &mo, &gpu)) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
        il = ((__bridge id<MTLBuffer>)mb).length - mo; ty = di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32; }
    for (int i = 0; i < g_fsp_n; i++) { __unsafe_unretained id<MTLRenderPipelineState> ps = g_fsp_ps[i]; if (!ps) continue;
        if (ps != g_enc_pso) { enc_r_pso(en, ps); g_enc_pso = ps; }
        [(id<MTL4RenderCommandEncoder>)en setColorAttachmentMap:g_fsp_phys ? g_fsm_map[g_fsp_phys][i] : g_fsp_map[i]];
        MTLVertexAmplificationViewMapping vm = {0, g_fsp_layer[i]}; [en setVertexAmplificationCount:1 viewMappings:&vm];
        if (di) enc_r_draw_indexed(en, mp, di->count, ty, gpu, il, 1, di->base_vertex, di->base_instance);
        else enc_r_draw(en, mp, dw->first, dw->count);
    }
}
// DM_NVN_FACE_MERGE: face-split passes of one level size are merged into one pass (docs/reference/rendering.md, "Face-split passes of one level size, one per texture").
// A draw joins when nothing passed over touches its targets or writes what it samples; the draws passed over may only draw into colour targets and sample known textures.
static int fsm_on(void) {
    static int m = -1;
    if (m < 0) {
        m = sw_mode("DM_NVN_FACE_MERGE", 1);
        if (!m) MLOG("DM_NVN_FACE_MERGE=0: each face-split draw keeps a pass of its own");
    }
    return sw_on(m);
}
static int fsm_taken_has(const uint8_t *p) { for (int i = 0; i < g_fsm_ntaken; i++) if (g_fsm_taken[i] == p) return 1; return 0; }
static int fsm_take(const uint8_t *p) { for (int i = 0; i < g_fsm_ntaken; i++) if (g_fsm_taken[i] == p) { g_fsm_taken[i] = g_fsm_taken[--g_fsm_ntaken]; return 1; } return 0; }
// serials identify views of one texture; storage ranges catch distinct serials sharing a backing pool
typedef struct { uint64_t serial, lo, hi; } fsm_access;
static int fsm_resource(fsm_access *out, uint64_t serial, uint64_t addr, uint64_t size) {
    if (addr && (!size || size > UINT64_MAX - addr || nvn_sparse_alias(addr, addr + size))) return 0;
    *out = (fsm_access){serial, addr, addr ? addr + size : 0};
    return 1;
}
static int fsm_tex_access(fsm_access *out, const tex_t *tex) {
    return fsm_resource(out, tex->serial, (uint64_t)tex->mem, tex->L.size);
}
static int fsm_overlap(fsm_access a, fsm_access b) {
    return (a.serial && a.serial == b.serial) || (a.hi && b.hi && a.lo < b.hi && b.lo < a.hi);
}
static int fsm_add_read(fsm_access *out, int *n, int cap, fsm_access a) {
    if (!a.serial && !a.hi) return 1;
    for (int i = 0; i < *n; i++)
        if (out[i].serial == a.serial && out[i].lo == a.lo && out[i].hi == a.hi) return 1;
    if (*n == cap) return 0;
    out[(*n)++] = a;
    return 1;
}
static int fsm_reads(NvProg *pr, const p_draw *dw, const p_draw_idx *di, fsm_access *out, int *n, int cap) {
    if (S.rast_discard || (dw && (dw->ind || dw->mdi_cnt)) || (di && (di->ind || di->mdi_cnt))) return 0;
    for (int i = 0; i < 16; i++) {
        fsm_access a;
        if (!fsm_resource(&a, 0, S.vbuf[i].address, S.vbuf[i].size) || !fsm_add_read(out, n, cap, a)) return 0;
    }
    if (di) {
        fsm_access a;
        if ((unsigned)di->itype > 2 || di->count <= 0 ||
            !fsm_resource(&a, 0, di->ibuf, (uint64_t)di->count << di->itype) || !fsm_add_read(out, n, cap, a)) return 0;
    }
    for (int st = 0; st < 2; st++) {
        __unsafe_unretained NvShader *sh = pr->sh[st];
        if (!sh) continue;
        if (sh->rf.bindless || sh->rf.pool || sh->rf.dyn || sh->rf.htab) return 0;
        for (int i = 0; i < sh->rf.nb; i++) {
            const nvn_rbind *b = &sh->rf.b[i];
            fsm_access a;
            uint64_t h;
            switch (b->kind) {
                case NVN_RB_CBUF: case NVN_RB_UBOP: {
                    if (b->reg >= 16) return 0;
                    const p_bind_buf *u = &S.ubo[st][b->reg];
                    if (!fsm_resource(&a, 0, u->addr, u->size) || !fsm_add_read(out, n, cap, a)) return 0;
                    continue;
                }
                case NVN_RB_SAMP: case NVN_RB_SUPPORT: case NVN_RB_C1: case NVN_RB_DRV0: case NVN_RB_DRV2:
                case NVN_RB_SAMPT: case NVN_RB_SAMPU: case NVN_RB_SAMPI: case NVN_RB_SAMPD: case NVN_RB_SAMPA:
                case NVN_RB_SAMPP: case NVN_RB_SAMPH: case NVN_RB_BORDER: case NVN_RB_PUSH: case NVN_RB_CBSIZE:
                case NVN_RB_TBIAS: case NVN_RB_SBLEN:
                    continue;
                case NVN_RB_TEX:
                    if (b->reg >= NTEX) return 0;
                    h = bound_tex(st, b->reg);
                    break;
                case NVN_RB_IMG:
                    if (b->rw || b->atom || b->reg >= 32) return 0;
                    h = bound_img(st, b->reg);
                    break;
                default:
                    return 0;
            }
            int tid = (int)(h & 0xfffff);
            if (!tid) continue;
            os_unfair_lock_lock(&g_rmu);
            tex_t tex = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t : (tex_t){0};
            os_unfair_lock_unlock(&g_rmu);
            if (!tex.serial || !fsm_tex_access(&a, &tex) || !fsm_add_read(out, n, cap, a)) return 0;
        }
    }
    return 1;
}
// Only layered targets already made, with plain level / layer views: inspecting one here cannot upgrade it, mark a write or open another encoder.
typedef struct {
    __unsafe_unretained id<MTLTexture> tex;
    NSUInteger level, lo, hi;
    uint8_t layer[8];
} fsm_tgt;
static int fsm_target(int n, fsm_tgt *o) {
    const tex_t *t = R.rt.color[0];
    id<MTLTexture> tex = tex_made(t);
    if (!tex || !(tex.usage & MTLTextureUsageRenderTarget) || tex.sampleCount != 1 ||
        (tex.textureType != MTLTextureType2DArray && tex.textureType != MTLTextureTypeCube && tex.textureType != MTLTextureTypeCubeArray)) return 0;
    NSUInteger slices[8], used = 0;
    *o = (fsm_tgt){.tex = tex, .lo = NSUIntegerMax};
    NSUInteger count = tex.arrayLength * (tex.textureType == MTLTextureType2DArray ? 1 : 6);
    for (int i = 0; i < n; i++) {
        const view_t *v = &R.rt.cview[i];
        if (!R.rt.color[i] || R.rt.color[i]->serial != t->serial || !R.rt.has_cview[i] || v->set_format || !v->set_layers) return 0;
        NSUInteger level = v->set_levels ? v->base_level : 0;
        NSUInteger layer = v->min_layer;
        if (level >= tex.mipmapLevelCount || (i && level != o->level) || layer >= count) return 0;
        o->level = level;
        slices[i] = layer;
        o->lo = MIN(o->lo, layer);
        o->hi = MAX(o->hi, layer);
        for (int j = 0; j < i; j++) if (slices[j] == layer) return 0;
    }
    if (o->hi - o->lo >= 8) return 0;
    for (int i = 0; i < n; i++) {
        o->layer[i] = (uint8_t)(slices[i] - o->lo);
        used |= (NSUInteger)1 << o->layer[i];
    }
    return used == ((NSUInteger)1 << (o->hi - o->lo + 1)) - 1;
}
static int fsm_scissor_empty(NSUInteger tw, NSUInteger th) {   // draw_'s test: the draw would be dropped
    if (!R.has_sc || S.rast_discard) return 0;
    long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
    if (x1 > (long)tw) x1 = (long)tw; if (y1 > (long)th) y1 = (long)th;
    return x1 <= x0 || y1 <= y0;
}
static void fsm_plan(NvProg *pr0, const p_draw *dw0, const p_draw_idx *di0) {
    g_fsm_np = 0;
    if (!fsm_on() || !g_exec_next || g_ret_n > 8 || g_nlc || g_nubd || g_vis_counting || !pr0) return;
    if (g_fsm_taken_sub != g_sub_serial) { g_fsm_ntaken = 0; g_fsm_taken_sub = g_sub_serial; }   // a submission's moved draws are all reached in it
    if (g_fsm_ntaken > FSM_TAKEN - FSM_MAX) return;
    fsm_tgt t0; int n0 = g_fsp_plan.n; uint32_t fmt0 = g_fsp_plan.fmt;
    if (!fsm_target(n0, &t0)) return;
    NSUInteger w0 = mip_extent(t0.tex.width, t0.level), h0 = mip_extent(t0.tex.height, t0.level), nl0 = t0.hi - t0.lo + 1;
    enum { FW = 48, FR = 64 };
    typedef struct { fsm_access a; NSUInteger level, lo, hi; } fsm_write;
    fsm_write wr[FW];
    fsm_access rd[FR];
    int nw = 1, nr = 0;
    if (!fsm_reads(pr0, dw0, di0, rd, &nr, FR) || !fsm_tex_access(&wr[0].a, R.rt.color[0])) return;
    wr[0].level = t0.level; wr[0].lo = t0.lo; wr[0].hi = t0.hi;
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R; int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty;
    __typeof__(g_fsp_plan) fp = g_fsp_plan; __unsafe_unretained id<MTLRenderPipelineState> fps[8]; for (int i = 0; i < 8; i++) fps[i] = g_fsp_ps[i];
    op_cursor cur = {.p = g_exec_next}; for (int k = 0; k < g_ret_n; k++) cur.ret[cur.ret_n++] = g_ret[k];
    for (int guard = 0, passed = 0; guard < 2048 && passed < 32 && g_fsm_np < FSM_MAX; guard++) {
        const uint8_t *q; op_hdr h; const void *a;
        if (!op_next(&cur, &q, &h, &a)) break;
        if (h.op == OP_SET_RT) { R.rt = *(const p_set_rt *)a; R.has_rt = 1; continue; }
        if (h.op == OP_BARRIER) continue;   // no draw that joins depends on what it orders
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) {
            if (fsm_taken_has(q)) continue;
            p_draw td; p_draw_idx ti; const p_draw *d = h.op == OP_DRAW ? draw_rec(a, h.size, &td) : NULL; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &ti) : NULL;
            prog_resolve(); __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil;
            int n = 0; while (R.has_rt && n < R.rt.n && n < 8 && R.rt.color[n]) n++;
            if (!pr || !pr->sh[0] || !pr->sh[1] || pr->sh[3] || pr->sh[4] || !R.has_rt || R.rt.depth || n < 1) break;
            fsm_access reads[FR], targets[8];
            int nc = 0, known = fsm_reads(pr, d, x, reads, &nc, FR);
            for (int i = 0; i < R.rt.n && i < 8 && known; i++)
                if (R.rt.color[i]) known = fsm_tex_access(&targets[i], R.rt.color[i]);
            if (!known) break;
            const tex_t *c0 = R.rt.color[0];
            fsm_tgt t;
            int join = n == n0 && fsm_target(n0, &t) && t.hi - t.lo + 1 == nl0
                && mip_extent(t.tex.width, t.level) == w0 && mip_extent(t.tex.height, t.level) == h0
                && S.poly.cull != NVN_FACE_FRONT_AND_BACK && !fsm_scissor_empty(w0, h0);
            for (int i = 0; i < nc && join; i++)
                for (int j = 0; j < nw && join; j++) join = !fsm_overlap(reads[i], wr[j].a);
            for (int j = 0; j < nw && join; j++) {
                const fsm_write *w = &wr[j];
                if (w->a.serial == c0->serial) join = w->level != t.level || w->hi < t.lo || t.hi < w->lo;
                else join = !fsm_overlap(w->a, targets[0]);
            }
            for (int i = 0; i < nr && join; i++) join = !fsm_overlap(rd[i], targets[0]);
            for (int i = 0; i < nc && join; i++) join = !fsm_overlap(reads[i], targets[0]);
            if (join) {
                fsp_plan(pr, d, x);
                join = g_fsp_plan.on && g_fsp_plan.n == n0 && g_fsp_plan.fmt == fmt0;
            }
            for (int i = 0; i < nc && known; i++) known = fsm_add_read(rd, &nr, FR, reads[i]);
            if (!known || nw == FW) break;
            if (join) {
                fsm_draw *m = &g_fsm[g_fsm_np++];
                m->op = q; m->rt = R.rt; m->level = t.level; m->slice = t.lo; m->ser = c0->serial; m->n = n0;
                memcpy(m->layer, t.layer, sizeof m->layer);
                for (int i = 0; i < 8; i++) m->ps[i] = g_fsp_ps[i];
                image_prepare(pr, 0x1f); vbuf_prepare(&pr->sh[0]->rf, d, x);
                wr[nw++] = (fsm_write){targets[0], t.level, t.lo, t.hi};
                continue;
            }
            passed++;
            // attachments after a hole may be written too
            for (int i = 0; i < R.rt.n && i < 8; i++) {
                if (!R.rt.color[i]) continue;
                NSUInteger level = R.rt.has_cview[i] && R.rt.cview[i].set_levels ? R.rt.cview[i].base_level : 0;
                int have = 0;
                for (int j = 0; j < nw && !have; j++)
                    have = wr[j].a.serial == targets[i].serial && wr[j].level == level && wr[j].hi == NSUIntegerMax;
                if (have) continue;
                if (nw == FW) { passed = 32; break; }
                wr[nw++] = (fsm_write){targets[i], level, 0, NSUIntegerMax};
            }
            continue; }
        if (h.op == OP_UPDATE_UB) break;
        if (state_op(h.op, a)) continue;
        break;
    }
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave; g_pso_dirty = pso_dirty; g_dss_dirty = dss_dirty; prog_resolve();
    g_fsp_plan = fp; for (int i = 0; i < 8; i++) g_fsp_ps[i] = fps[i];
}
static void fsm_attach(MTL4RenderPassDescriptor *pd) {
    MTLRenderPassColorAttachmentDescriptor *c0 = pd.colorAttachments[0]; NSUInteger nl = pd.renderTargetArrayLength ? pd.renderTargetArrayLength : 1;
    for (int k = 0; k < g_fsm_np && k < FSM_MAX; k++) { fsm_draw *m = &g_fsm[k]; MTLRenderPassColorAttachmentDescriptor *c = pd.colorAttachments[k + 1];
        if (c.texture || !c0.texture) break;
        attach(c, m->rt.color[0], &m->rt.cview[0], m->rt.has_cview[0]);
        id<MTLTexture> t = c.texture;
        NSUInteger layers = t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? t.arrayLength * 6 : t.arrayLength;
        if (!t || c.level != m->level || t.pixelFormat != c0.texture.pixelFormat || t.textureType == MTLTextureType2D || t.textureType == MTLTextureType3D || m->slice + nl > layers
            || mip_extent(t.width, c.level) != mip_extent(c0.texture.width, c0.level) || mip_extent(t.height, c.level) != mip_extent(c0.texture.height, c0.level)) { pd.colorAttachments[k + 1] = nil; break; }
        c.slice = m->slice; c.loadAction = MTLLoadActionLoad; c.storeAction = MTLStoreActionStore;
        uint64_t b = att_bytes(t, c.level); g_bw_load += b; g_bw_store += b;
        PD("P %llu c%d ser=%llu fmt=%lu %lux%lu ms1 lv%lu sl%lu load mtl=%p", g_stat_pass + 1, k + 1, (unsigned long long)m->ser, (unsigned long)t.pixelFormat, (unsigned long)t.width,
           (unsigned long)t.height, (unsigned long)c.level, (unsigned long)c.slice, (__bridge void *)t);
        g_fsm_na = k + 1;
    }
    if (!g_fsm_na) return;
    if (!g_fsm_map[1][0]) for (int k = 1; k <= FSM_MAX; k++) for (int i = 0; i < 8; i++) { g_fsm_map[k][i] = [MTLLogicalToPhysicalColorAttachmentMap new]; [g_fsm_map[k][i] setPhysicalIndex:(NSUInteger)k forLogicalIndex:(NSUInteger)i]; }
    g_fsm_npass++;
    static int once; if (!once++) MLOG("face-split draws merged by level: %d later face-split draws of the same level size joined the pass (first case pass %llu; DM_NVN_FACE_MERGE)", g_fsm_na, g_stat_pass + 1);
    PD("L %llu face-split draws of %d later targets joined this pass as attachments 1-%d", g_stat_pass + 1, g_fsm_na, g_fsm_na);
}
// What draw_ does for a face-split draw. 1 drawn, 0 not (the replay draws it where it is), -1 the pass ended.
static int fsm_encode(id<MTL4RenderCommandEncoder> en, NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    NSUInteger tw = g_pass.tw, th = g_pass.th;
    MTLViewport v = S.has_vp ? (MTLViewport){S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0], S.dr[1]} : (MTLViewport){0, 0, tw, th, 0, 1};
    if (vp_origin_ll()) { v.originY += v.height; v.height = -v.height; }
    { uint32_t sw; memcpy(&sw, S.vp_sw[0], 4); if (sw == 0x100) { v.originY += v.height; v.height = -v.height; } else if (sw) return 0; }
    MTLScissorRect sc = {0, 0, tw, th};
    if (R.has_sc && !S.rast_discard) {
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw) x1 = (long)tw; if (y1 > (long)th) y1 = (long)th;
        if (x1 <= x0 || y1 <= y0) return 0;
        sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK) return 0;
    enc_r_viewport(en, v); g_ed.vp = v; g_ed.ok |= ED_VP;
    enc_r_scissor(en, sc); g_ed.sc = sc; g_ed.ok |= ED_SC;
    int cu = S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone;
    int wi = (S.poly.front == NVN_FRONT_FACE_CCW) != g_queue_origin_ll ? MTLWindingCounterClockwise : MTLWindingClockwise;
    int fi = S.poly.mode == NVN_POLYGON_MODE_LINE ? MTLTriangleFillModeLines : MTLTriangleFillModeFill;
    if (!(g_ed.ok & ED_CULL) || g_ed.cull != cu) { enc_r_cull(en, (MTLCullMode)cu); g_ed.cull = cu; g_ed.ok |= ED_CULL; }
    if (!(g_ed.ok & ED_WIND) || g_ed.wind != wi) { enc_r_winding(en, (MTLWinding)wi); g_ed.wind = wi; g_ed.ok |= ED_WIND; }
    if (!(g_ed.ok & ED_FILL) || g_ed.fill != fi) { enc_r_fill(en, (MTLTriangleFillMode)fi); g_ed.fill = fi; g_ed.ok |= ED_FILL; }
    if (bind_vbufs(en, &pr->sh[0]->rf) >= 0) return 0;
    vconv_bind(en, pr->sh[0], S.rast_discard, dw, di);
    encode_args(en, pr->sh[0], pr->sh[0]->stage == 4 ? 4 : 0, 1);
    encode_args(en, pr->sh[1], 1, 0);
    if ((id)g_enc != (id)en) return -1;   // binding upgraded a texture, which ended the pass
    if (pr->sh[1]->has_bz) bz_bind(en, NULL);
    g_stat_draw++;
    fsp_encode(en, (dw ? dw->prim : di->prim) == 5 ? MTLPrimitiveTypeTriangleStrip : MTLPrimitiveTypeTriangle, dw, di);
    return 1;
}
static void fsm_run(id<MTL4RenderCommandEncoder> en) {
    int n = g_fsm_na; g_fsm_na = 0;
    if (!n || !g_exec_next) return;
    static typeof(S) Ssave; memcpy(&Ssave, &S, sizeof S); typeof(R) Rsave = R; int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty, fn = g_fsp_n;
    uint8_t fl[8]; memcpy(fl, g_fsp_layer, sizeof fl); __unsafe_unretained id<MTLRenderPipelineState> fps[8]; for (int i = 0; i < 8; i++) fps[i] = g_fsp_ps[i];
    op_cursor cur = {.p = g_exec_next}; for (int k = 0; k < g_ret_n; k++) cur.ret[cur.ret_n++] = g_ret[k];
    for (int guard = 0, k = 0; guard < 4096 && k < n; guard++) {
        const uint8_t *q; op_hdr h; const void *a;
        if (!op_next(&cur, &q, &h, &a)) break;
        if (h.op == OP_SET_RT) { R.rt = *(const p_set_rt *)a; R.has_rt = 1; continue; }
        if (h.op == OP_DRAW || h.op == OP_DRAW_IDX) {
            if (q != g_fsm[k].op) continue;
            p_draw td; p_draw_idx ti; const p_draw *d = h.op == OP_DRAW ? draw_rec(a, h.size, &td) : NULL; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &ti) : NULL;
            prog_resolve(); __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil;
            g_fsp_n = g_fsm[k].n; memcpy(g_fsp_layer, g_fsm[k].layer, sizeof g_fsp_layer); for (int i = 0; i < 8; i++) g_fsp_ps[i] = g_fsm[k].ps[i]; g_fsp_phys = k + 1;
            int r = pr && pr->sh[0] && pr->sh[1] ? fsm_encode(en, pr, d, x) : 0;
            g_fsp_phys = 0;
            if (r < 0) break;
            if (r > 0) { g_fsm_taken[g_fsm_ntaken++] = q; g_fsm_moved++; PD("L %llu face-split draw into ser=%llu lv%lu sl%lu joined as attachment %d", g_stat_pass, (unsigned long long)g_fsm[k].ser,
                                                                            (unsigned long)g_fsm[k].level, (unsigned long)g_fsm[k].slice, k + 1); }
            k++; continue; }
        if (h.op == OP_BARRIER || state_op(h.op, a)) continue;
        break;   // fsm_plan passed only state, targets, barriers and draws
    }
    memcpy(&S, &Ssave, sizeof S); args_dirty_all(); R = Rsave; g_pso_dirty = 1 | pso_dirty; g_dss_dirty = 1 | dss_dirty; prog_resolve();
    g_fsp_n = fn; memcpy(g_fsp_layer, fl, sizeof fl); for (int i = 0; i < 8; i++) g_fsp_ps[i] = fps[i];
}
static void wide_run(const p_draw_idx *di) {
    static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at; static int failed;
    if (failed) return;
    if (!at) { NSError *e = nil; id<MTLLibrary> lib = host_lib("wide", &e); id<MTLFunction> f = lib ? [lib newFunctionWithName:@"wide_u8"] : nil;
        ps = f ? dev_cps(f, &e) : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; at = ps ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!at) { failed = 1; MLOG("[RED] 8-bit index GPU expansion: cannot create the compute pipeline %s (the CPU expands them as before)", e.localizedDescription.UTF8String); return; } }
    if (g_wide_epoch != g_wr_epoch || g_wide_sub != g_sub_serial || g_wide_n > WIDE_MAX - 8) { g_wide_n = 0; g_wide_epoch = g_wr_epoch; g_wide_sub = g_sub_serial; }
    int n0 = g_wide_n; wide_take(di);
    if (g_exec_next && g_ret_n <= 8) {   // later draws in replay order, up to the next buffer write
        op_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
        const uint8_t *q; op_hdr h; const void *a;
        for (int guard = 0; guard < 200000 && g_wide_n < WIDE_MAX && op_next(&cur, &q, &h, &a); guard++) {
            if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_UPDATE_UB || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B) break;
            p_draw_idx it; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
            if (x && wide_wanted(x)) wide_take(x);
        }
    }
    int n = g_wide_n - n0; if (n <= 0) return;
    if (g_enc) { g_wide_splits++; PD("M %llu 8-bit index GPU expansion: render pass split here", g_stat_pass); rp_end(); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    typedef struct { uint64_t src, dst; uint32_t n, shift; } wjob;
    id<MTLBuffer> jb; size_t jo = arena_alloc((size_t)n * sizeof(wjob), &jb); wjob *jp = (wjob *)((uint8_t *)jb.contents + jo);
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); [ce setArgumentTable:at]; [ce setComputePipelineState:ps];
    for (int i = 0; i < n; i++) {
        wide_ent *e = &g_wide[n0 + i]; void *mb; uint64_t mo, src; e->gpu = 0;
        if (!mtl_gpu_addr(e->ibuf, (uint64_t)e->count, &mb, &mo, &src)) continue;   // the draw reports it
        id<MTLBuffer> ob; size_t oo = arena_alloc((size_t)e->count * 2, &ob);
        jp[i] = (wjob){src & ~15ull, ob.gpuAddress + oo, (uint32_t)e->count, (uint32_t)(src & 15)};
        at_addr(at, jb.gpuAddress + jo + (uint64_t)i * sizeof(wjob), 0, "wide index table slot", 0);
        [ce dispatchThreadgroups:MTLSizeMake(((NSUInteger)e->count + 63) / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
        e->gpu = ob.gpuAddress + oo;
    }
    enc_c_end(ce);
    args_dirty_all();
    g_wide_draws += (uint64_t)n;
    if (!g_wide_runs++) MLOG("8-bit index GPU expansion: first time, %d draws (the indices have GPU-side writes in this submission; a compute pass expands them to 16 bits before the render pass)", n);
    else if (g_wide_runs % 3600 == 0) MLOG("8-bit index GPU expansion: %llu times, %llu draws, render pass split %llu times", (unsigned long long)g_wide_runs, (unsigned long long)g_wide_draws, (unsigned long long)g_wide_splits);
}
// DM_NVN_UBP_CHECK=1 (diagnostic): each shadow made from memory is compared, after a GPU wait, with what the GPU reads.
static void mdi_wait(void);
static uint64_t g_ubpk_same, g_ubpk_diff;
static int ubp_check_on(void) {
    static int v = -1;
    if (v < 0) {
        v = getenv("DM_NVN_UBP_CHECK") != NULL;
        if (v) MLOG("DM_NVN_UBP_CHECK: every uniform shadow made from memory is compared with the GPU's memory after the updates land (slow)");
    }
    return v;
}
static void ubp_check(uint64_t lo, uint64_t hi, const uint8_t *sh) {
    ubc_wait(g_ubp_ready);   // the copy worker may still be filling it
    size_t n = (size_t)(hi - lo); uint8_t *tmp = malloc(n); memcpy(tmp, sh, n);
    mdi_wait();
    const uint8_t *m = (const uint8_t *)(uintptr_t)lo; size_t bad = 0, first = 0;
    for (size_t i = 0; i < n; i++) if (tmp[i] != m[i]) { if (!bad++) first = i; }
    if (bad) { g_ubpk_diff++; if (g_ubpk_diff <= 10) MLOG("[RED] uniform shadow check: [%#llx +%zu] differs from the GPU's memory in %zu bytes, first at +%zu (shadow %02x, memory %02x) frame %llu pass %llu",
        (unsigned long long)lo, n, bad, first, tmp[first], m[first], (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass); }
    else g_ubpk_same++;
    if ((g_ubpk_same + g_ubpk_diff) % 2000 == 0) MLOG("uniform shadow check: %llu equal, %llu different", (unsigned long long)g_ubpk_same, (unsigned long long)g_ubpk_diff);
    free(tmp);
}
// Whether the deferred inline updates must land before this draw: a uniform binding they cover in part whose rest cannot be read from memory, a bindless handle there,
// or their data read as vertices, indices, indirect arguments or storage.
static int ubd_buffer_texture_hit(int tid) {   // caller holds g_rmu; no texture creation inside an open pass
    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok || g_treg[tid].gone) return 0;
    const tex_t *t = &g_treg[tid].t;
    if (t->d.target != NVN_TEXTURE_TARGET_BUFFER) return 0;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint64_t lo = (uint64_t)(uintptr_t)t->mem, n = (uint64_t)t->d.width * (uint64_t)bpb;
    return ubd_hits(lo, lo + n) || nvn_sparse_alias(lo, lo + n);
}
static int ubd_conflict_textures(NvShader *sh, int st) {
    if (!g_nubd) return 0;
    int hit = 0;
    os_unfair_lock_lock(&g_rmu);
    for (int i = 0; i < sh->rf.nb && !hit; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        switch (b->kind) {
        case NVN_RB_TEXP: case NVN_RB_TEXH: hit = b->dtt == MTLTextureTypeTextureBuffer; break;   // selected on the GPU: any update may overlap
        case NVN_RB_TEX: case NVN_RB_TBUF: hit = ubd_buffer_texture_hit((int)(bound_tex(st, b->reg) & 0xfffff)); break;
        case NVN_RB_TEXU: case NVN_RB_TBUFU:
            for (int k = 0; k < (b->n ? b->n : 1) && !hit; k++) hit = ubd_buffer_texture_hit(ubo_tex_id(b->n ? ubo_handle(st, b->reg, b->off + 8u * (unsigned)k) : ubo_bind_handle(st, b)));
            break;
        case NVN_RB_IMG: hit = b->reg < 32 && ubd_buffer_texture_hit((int)(bound_img(st, b->reg) & 0xfffff)); break;
        case NVN_RB_IMGU: hit = ubd_buffer_texture_hit(ubo_tex_id(ubo_handle(st, b->reg, b->off))); break;
        case NVN_RB_TEXD:
            for (int k = 0; k < b->n && k < 128 && !hit; k++) hit = ubd_buffer_texture_hit((int)(bound_handle(S.tex[st][k] ? S.tex[st][k] : S.tex[st][NVN_SEPTEX_SLOT0 + k]) & 0xfffff));
            break;
        case NVN_RB_TEXA:
            for (int k = 0; k < b->n && !hit; k++) hit = ubd_buffer_texture_hit((int)(c2_handle(st, b->off + 8u * (unsigned)k) & 0xfffff));
            break;
        default: break;
        }
    }
    os_unfair_lock_unlock(&g_rmu);
    return hit;
}
static int ubd_conflict_sh(NvShader *sh, int st) {
    if (ubd_conflict_textures(sh, st)) return 1;
    for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i];
        if (b->kind == NVN_RB_GMEM) { if (ubd_hits(0, UINT64_MAX)) return 1; continue; }
        if (b->reg >= 16) continue;
        const p_bind_buf *u = b->kind == NVN_RB_SBUFP ? &S.ssbo[st][b->reg] : &S.ubo[st][b->reg];
        if (!u->addr) continue;
        if (b->kind == NVN_RB_CBUF) { uint64_t g, ul = cb_len(sh, b->reg, u->size); const uint8_t *c;
            if (ubd_hits(u->addr, u->addr + ul) && !ubd_covers(u->addr, u->addr + ul)) {
                if (!ubp_get(u->addr, u->addr + ul, &g, &c)) { g_ubp_refused++; return 1; }
                if (ubp_check_on()) { ubp_check(u->addr, u->addr + ul, c); return 1; } } }
        else if (b->kind == NVN_RB_TEXU || b->kind == NVN_RB_SAMPU || b->kind == NVN_RB_SBUFU || b->kind == NVN_RB_IMGU || b->kind == NVN_RB_TBUFU) { uint64_t h = u->addr + (b->n ? 0 : b->off), n = b->n ? 8ull * b->n : b->kind == NVN_RB_SBUFU ? 8 : 4; uint64_t g; const uint8_t *c;   // an array: its handles at 8k
            if (ubd_hits(h, h + n) && !ubd_covers(h, h + n) && !ubp_get(h, h + n, &g, &c)) { g_ubp_refused++; return 1; }
            if (b->soff && b->sreg < 16) { uint64_t h2 = S.ubo[st][b->sreg].addr + (b->soff - 1u); if (ubd_hits(h2, h2 + 4) && !ubd_covers(h2, h2 + 4) && !ubp_get(h2, h2 + 4, &g, &c)) { g_ubp_refused++; return 1; } } }
        else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) { if (ubd_hits(u->addr, u->addr + (u->size ? u->size : 1))) return 1; } }
    return 0;
}
static int ubd_conflict(NvProg *pr, const p_draw *dw, const p_draw_idx *di) {
    for (int s_ = 0; s_ < 16; s_++) if (S.vbuf[s_].address && ubd_hits(S.vbuf[s_].address, S.vbuf[s_].address + S.vbuf[s_].size)) return 1;
    if (di && di->ibuf && ubd_hits(di->ibuf, di->ibuf + (uint64_t)di->count * 4)) return 1;
    uint64_t ind = dw ? dw->ind : di ? di->ind : 0; if (ind && ubd_hits(ind, ind + 32)) return 1;
    for (int st = 0; pr && st < 2; st++) if (pr->sh[st] && ubd_conflict_sh(pr->sh[st], st)) return 1;
    return 0;
}
// The same for a dispatch: else they would land after it, at the next copy into the encoder it shares.
static int ubd_conflict_cs(NvShader *cs, uint64_t ind) { return (ind && ubd_hits(ind, ind + 12)) || ubd_conflict_sh(cs, 5); }
// Indirect-count multi-draws Metal cannot mask on the GPU (ixc_run's, or records in no Metal buffer): before the first such draw after GPU writes the replay commits, waits,
// and reads the count on the CPU; later draws of the group use the same value.
static uint64_t g_mdi_waits, g_mdi_epoch = ~0ull, g_mdi_sub, g_mdi_addr; static uint32_t g_mdi_count;
static void mdi_wait(void) {
    int ok = gpu_read_wait(WS_MDI);
    if (!ok) RED_ONCE(97, "indirect-count multi-draw: the GPU did not finish within 10 s; the count is read anyway");
    if (!g_mdi_waits++) MLOG("indirect-count multi-draw: first wait for the GPU-written count at frame %llu pass %llu", (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
}
static uint32_t mdi_count(uint64_t cnt) {
    if (cnt != g_mdi_addr || g_mdi_epoch != g_wr_epoch || g_mdi_sub != g_sub_serial) {
        // no write since this submission's last drain can have changed the count
        int fresh = drw_valid() && !drw_hits(cnt, 4) && !nvn_sparse_alias(cnt, cnt + 4);
        if (!fresh && (g_mdi_epoch != g_wr_epoch || g_mdi_sub != g_sub_serial)) { mdi_wait(); g_mdi_epoch = g_wr_epoch; g_mdi_sub = g_sub_serial; }
        uint32_t v = 0;
        if (nvn_host_readable(cnt, 4)) memcpy(&v, (const void *)(uintptr_t)cnt, 4);
        else RED_ONCE(98, "indirect-count multi-draw: the count at %#llx is in an unmapped page of a sparse pool, nothing drawn", (unsigned long long)cnt);
        g_mdi_addr = cnt; g_mdi_count = v;
    }
    return g_mdi_count;
}
static int mdi_live(uint64_t cnt, int i) { return (uint32_t)i < mdi_count(cnt); }
// Indirect-count multi-draws masked on the GPU: a compute pass copies each record, or zeros past the count, and the draw reads the copy. Batched as in ixc_run up to the next
// write (g_mdx_epoch, g_mdx_stop); copies are keyed by record and count address (a game may draw the same records with two counts).
#define MDX_SLOTS 16384   // a power of two; a scan fills at most a quarter
static const p_multi_draw *g_mdx_range;
typedef struct { uint64_t ind, cnt, out, tag; } mdx_ent;   // out: 0 when the record or count is in no Metal buffer
static mdx_ent g_mdx[MDX_SLOTS]; static int g_mdx_n; static uint64_t g_mdx_tag = 1, g_mdx_ep = ~0ull, g_mdx_sub, g_mdx_runs, g_mdx_splits, g_mdx_draws, g_mdx_fast, g_mdx_pre, g_mdx_wr = ~0ull; static const uint8_t *g_mdx_stop;
// Draws can write records too, ordered by GAME_BARRIER_FULL: such a barrier after a shader write since g_mdx_wr ends the copies (a zero copy kept past it would hide
// the draws after a depth prepass). Kernel: dm/gfx/msl/mdx.metal
static int mdx_wanted(const p_draw *dw, const p_draw_idx *di) { return dw ? dw->mdi_cnt && !ixc_wanted(dw, NULL) : di->mdi_cnt && !ixc_wanted(NULL, di); }
static mdx_ent *mdx_slot(uint64_t ind, uint64_t cnt) {
    for (uint64_t h = ((ind ^ cnt * 0xD6E8FEB86659FD93ull) * 0x9E3779B97F4A7C15ull) >> 50;; h = (h + 1) & (MDX_SLOTS - 1)) { mdx_ent *e = &g_mdx[h]; if (e->tag != g_mdx_tag || (e->ind == ind && e->cnt == cnt)) return e; }
}
static int mdx_find(uint64_t ind, uint64_t cnt, uint64_t *out) {
    if (g_mdx_ep != g_mdx_epoch || g_mdx_sub != g_sub_serial) return 0;
    const mdx_ent *e = mdx_slot(ind, cnt); if (e->tag != g_mdx_tag) return 0;
    *out = e->out; return 1;
}
static void mdx_run(const p_draw *dw, const p_draw_idx *di) {
    static id<MTLComputePipelineState> ps; static id<MTL4ArgumentTable> at; static int failed;
    if (failed) return;
    if (!at) { NSError *e = nil; id<MTLLibrary> lib = host_lib("mdx", &e); id<MTLFunction> f = [lib newFunctionWithName:@"mdx_mask"];
        ps = f ? dev_cps(f, &e) : nil;
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxBufferBindCount = 1; at = ps ? rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]) : nil;
        if (!at) { failed = 1; MLOG("[RED] indirect-count multi-draw masking: cannot create the compute pipeline %s (those draws wait for the GPU)", e.localizedDescription.UTF8String); return; } }
    if (g_mdx_ep != g_mdx_epoch || g_mdx_sub != g_sub_serial || g_mdx_n > MDX_SLOTS / 4) { g_mdx_tag++; g_mdx_n = 0; g_mdx_ep = g_mdx_epoch; g_mdx_sub = g_sub_serial; g_mdx_wr = ~0ull; }
    enum { QMAX = MDX_SLOTS / 4 };
    static struct { mdx_ent *e; uint32_t i, words; } q[QMAX]; int nq = 0;
    uint64_t lo = ~0ull, hi = 0;   // for the uniform update check
    #define MDX_TAKE(IND, CNT, I, W) do { mdx_ent *e_ = mdx_slot((IND), (CNT)); if (e_->tag != g_mdx_tag && nq < QMAX) { *e_ = (mdx_ent){(IND), (CNT), 0, g_mdx_tag}; g_mdx_n++; \
        q[nq].e = e_; q[nq].i = (uint32_t)(I); q[nq++].words = (W); lo = MIN(lo, MIN((IND), (CNT))); hi = MAX(hi, MAX((IND) + 4 * (W), (CNT) + 4)); } } while (0)
    // words 0: an indirect mesh draw, keyed by its record alone; neither: mdx_prescan, only the draws after this one
    if (dw && dw->prim == DM_PRIM_MESH_TASKS) MDX_TAKE(dw->ind, dw->ind, DMSH_MESH_GRID, 0);
    else if (dw) MDX_TAKE(dw->ind, dw->mdi_cnt, dw->mdi_i, 4);
    else if (di) MDX_TAKE(di->ind, di->mdi_cnt, di->mdi_i, 5);
    if (g_mdx_range && (dw || di)) {
        const p_multi_draw *m = g_mdx_range;
        for (int i = (dw ? dw->mdi_i : di->mdi_i) + 1; i < m->max && nq < QMAX; i++) {
            uint64_t ind = m->ind + (uint64_t)i * (uint64_t)m->stride;
            MDX_TAKE(ind, m->cnt, i, dw ? 4u : 5u);
        }
    } else if (g_exec_next && g_ret_n <= 8) {
        op_cursor cur = {.p = g_exec_next, .ret_n = g_ret_n}; memcpy(cur.ret, g_ret, sizeof(*g_ret) * (size_t)g_ret_n);
        const uint8_t *p; op_hdr h; const void *a;
        for (int guard = 0; guard < 200000 && nq < QMAX && op_next(&cur, &p, &h, &a); guard++) {
            if (h.op == OP_DISPATCH || h.op == OP_COPY_B2B || h.op == OP_CLEAR_BUF || h.op == OP_COPY_T2B) break;
            if (h.op == OP_UPDATE_UB) { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8);
                int hit = 0; for (int k = 0; d < hi && lo < d + n && k < nq && !hit; k++) { const mdx_ent *e = q[k].e; hit = (d < e->ind + 4 * q[k].words && e->ind < d + n) || (d < e->cnt + 4 && e->cnt < d + n); }
                if (hit) { g_mdx_stop = p; break; } continue; }
            p_draw dt; p_draw_idx it; const p_draw *d = h.op == OP_DRAW ? draw_rec(a, h.size, &dt) : NULL; const p_draw_idx *x = h.op == OP_DRAW_IDX ? draw_idx_rec(a, h.size, &it) : NULL;
            if (d && d->prim == DM_PRIM_MESH_TASKS && d->ind) MDX_TAKE(d->ind, d->ind, DMSH_MESH_GRID, 0);
            else if (d && mdx_wanted(d, NULL)) MDX_TAKE(d->ind, d->mdi_cnt, d->mdi_i, 4);
            else if (x && mdx_wanted(NULL, x)) MDX_TAKE(x->ind, x->mdi_cnt, x->mdi_i, 5);
        }
    }
    #undef MDX_TAKE
    if (!nq) return;
    if (g_enc) { g_mdx_splits++; PD("M %llu indirect-count multi-draw masking: render pass split here", g_stat_pass); rp_end(); }
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    typedef struct { uint64_t rec, cnt, out; uint32_t i, words; } mjob;
    int nj = (nq + 63) & ~63;
    id<MTLBuffer> ab; size_t ao = arena_alloc((size_t)nj * sizeof(mjob) + (size_t)nq * 32, &ab);   // jobs padded to whole threadgroups, then the copies 32 bytes apart
    mjob *j = (mjob *)((uint8_t *)ab.contents + ao); uint64_t og = ab.gpuAddress + ao + (uint64_t)nj * sizeof(mjob);
    memset(j, 0, (size_t)nj * sizeof(mjob));
    for (int k = 0; k < nq; k++) { mdx_ent *e = q[k].e; void *mb; uint64_t mo, rg, cg;
        if (!mtl_gpu_addr(e->ind, 4 * (q[k].words ? q[k].words : 2), &mb, &mo, &rg) || !mtl_gpu_addr(e->cnt, 4, &mb, &mo, &cg)) continue;
        e->out = og + 32 * (uint64_t)k; j[k] = (mjob){rg, cg, e->out, q[k].i, q[k].words}; }
    id<MTL4ComputeCommandEncoder> ce = enc_c_begin(NULL); [ce setArgumentTable:at]; at_addr(at, ab.gpuAddress + ao, 0, "multi-draw indirect table slot", 0); [ce setComputePipelineState:ps];
    [ce dispatchThreadgroups:MTLSizeMake((NSUInteger)nj / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    enc_c_end(ce);
    args_dirty_all();
    g_mdx_draws += (uint64_t)nq; if (g_mdx_wr == ~0ull) g_mdx_wr = g_vbuf_write_epoch;
    if (!g_mdx_runs++) MLOG("indirect-count multi-draw masking: first time, %d draws (their records are copied on the GPU, zeros past the count, before the render pass)", nq);
    else if (g_mdx_runs % 3600 == 0) MLOG("indirect-count multi-draw masking: %llu times, %llu draws (%llu encoded alone, mdx_next), render pass split %llu times (scans at a pass's opening %llu); waits for the GPU-written count %llu",
                                          (unsigned long long)g_mdx_runs, (unsigned long long)g_mdx_draws, (unsigned long long)g_mdx_fast, (unsigned long long)g_mdx_splits, (unsigned long long)g_mdx_pre, (unsigned long long)g_mdi_waits);
}
// Before a pass opens, the multi-draws after it are copied, so none splits the pass; once per epoch and submission, only once g_mdx_runs.
static void mdx_prescan(void) {
    static uint64_t ep = ~0ull, sub; static int mode = -1; if (mode < 0) mode = sw_mode("DM_NVN_MDX_PRESCAN", 1);
    if (!g_mdx_runs || !sw_on(mode) || (ep == g_mdx_epoch && sub == g_sub_serial)) return;
    ep = g_mdx_epoch; sub = g_sub_serial; g_mdx_pre++;
    mdx_run(NULL, NULL);
}
// Consecutive draws of one multi-draw in the same pass are encoded alone, without the rest of draw's work.
static struct { const uint8_t *next; const void *enc; uint64_t cnt, ig, il, pass; int i; MTLPrimitiveType mp; MTLIndexType it; } g_mdxc;
static void mdx_chain(uint64_t cnt, int i, MTLPrimitiveType mp, MTLIndexType it, uint64_t ig, uint64_t il) {
    const uint8_t *next = g_mdx_range ? g_exec_cur : g_exec_next;
    g_mdxc = !next ? (typeof(g_mdxc)){0} : (typeof(g_mdxc)){next, (__bridge const void *)g_enc, cnt, ig, il, g_stat_pass, i, mp, it};
}
static int mdx_next(const p_draw *dw, const p_draw_idx *di) {
    uint64_t cnt = dw ? dw->mdi_cnt : di->mdi_cnt, out; int i = dw ? dw->mdi_i : di->mdi_i;
    if (!g_mdxc.next || g_mdxc.next != g_exec_cur || !g_enc || g_mdxc.enc != (__bridge const void *)g_enc || g_mdxc.pass != g_stat_pass || g_mdxc.cnt != cnt || g_mdxc.i + 1 != i
        || passdump() || !mdx_find(dw ? dw->ind : di->ind, cnt, &out) || !out) return 0;
    if (dw) enc_r_draw_indirect(g_enc, g_mdxc.mp, out); else enc_r_draw_indexed_indirect(g_enc, g_mdxc.mp, g_mdxc.it, g_mdxc.ig, g_mdxc.il, out);
    g_stat_draw++; g_mdx_fast++; g_mdxc.next = g_mdx_range ? g_exec_cur : g_exec_next; g_mdxc.i = i;
    return 1;
}

static int skip_fs_draw(NvProg *pr);   // diagnostic
