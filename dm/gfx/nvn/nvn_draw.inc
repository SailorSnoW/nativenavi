// Lazy clears (DM_NVN_LAZY_CLEAR): clears with no draw wait (g_lc) for the next pass that attaches the same subresource and view, as its load action; the rest land
// before anything could see their textures. Every pass that opens takes them: a clear left waiting while a pass is open would wipe its draws.
typedef struct { p_set_rt rt; uint8_t clear_c[8]; MTLClearColor cc[8]; uint8_t clear_d, clear_s; double cd; uint32_t cs; } lc_group;
enum { LC_MAX = 8 };
static lc_group g_lc[LC_MAX];
static uint64_t g_lc_taken, g_lc_landed;
void mtl_lc_stats(uint64_t *taken, uint64_t *landed) { *taken = g_lc_taken; *landed = g_lc_landed; }
static int lc_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_LAZY_CLEAR");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_LAZY_CLEAR=0: clears with no draw open a pass of their own as before");
    }
    return v;
}
static int lc_same_sub(const p_set_rt *a, int ka, const p_set_rt *b, int kb) {   // k = 8: depth / stencil
    uint64_t s1, s2; int l1, l2, x1, x2;
    rt_sub(a, ka, &s1, &l1, &x1); rt_sub(b, kb, &s2, &l2, &x2);
    return s1 && s1 == s2 && l1 == l2 && x1 == x2;
}
static int lc_same(const p_set_rt *a, int ka, const p_set_rt *b, int kb) {
    if (!lc_same_sub(a, ka, b, kb)) return 0;
    int h1 = ka == 8 ? a->has_dview : a->has_cview[ka], h2 = kb == 8 ? b->has_dview : b->has_cview[kb];
    return h1 == h2 && (!h1 || !memcmp(ka == 8 ? &a->dview : &a->cview[ka], kb == 8 ? &b->dview : &b->cview[kb], sizeof(view_t)));
}
static void lc_land(void) {
    if (!g_nlc) return;
    if (g_enc) rp_end();
    int n = g_nlc; g_nlc = 0;
    typeof(R) save = R;
    for (int gi = 0; gi < n; gi++) {
        const lc_group *g = &g_lc[gi];
        int any = g->clear_d || g->clear_s;
        for (int i = 0; i < 8; i++) any |= g->clear_c[i];
        if (!any || !g_cb) continue;
        R.rt = g->rt; R.has_rt = 1; memcpy(R.clear_c, g->clear_c, sizeof R.clear_c); memcpy(R.cc, g->cc, sizeof R.cc);
        R.clear_d = g->clear_d; R.clear_s = g->clear_s; R.cd = g->cd; R.cs = g->cs;
        rp_begin_ex(1);
        rp_end();
        g_lc_landed++;
    }
    R = save;
}
static void lc_push(void) {
    if (g_nlc == LC_MAX) lc_land();
    lc_group *g = &g_lc[g_nlc];
    g->rt = R.rt; memcpy(g->clear_c, R.clear_c, sizeof g->clear_c); memcpy(g->cc, R.cc, sizeof g->cc); g->clear_d = R.clear_d; g->clear_s = R.clear_s; g->cd = R.cd; g->cs = R.cs;
    for (int gi = 0; gi < g_nlc; gi++) {
        lc_group *o = &g_lc[gi];
        for (int i = 0; i < 8; i++)
            if (o->clear_c[i])
                for (int j = 0; j < 8; j++)
                    if (g->clear_c[j] && lc_same_sub(&o->rt, i, &g->rt, j)) { o->clear_c[i] = 0; break; }
        if ((o->clear_d || o->clear_s) && (g->clear_d || g->clear_s) && lc_same_sub(&o->rt, 8, &g->rt, 8)) {
            if (g->clear_d) o->clear_d = 0;
            if (g->clear_s) o->clear_s = 0;
        }
    }
    g_nlc++;
    memset(R.clear_c, 0, sizeof R.clear_c); R.clear_d = R.clear_s = 0;
}
// a program with bindless or dynamic handles counts as sampling
static int lc_sampled(NvProg *pr) {
    if (!pr) return 1;
    for (int st = 0; st < 5; st++) {
        __unsafe_unretained NvShader *sh = pr->sh[st];
        if (!sh || sh->stage >= 6) continue;
        if (sh->rf.bindless || sh->rf.dyn) return 1;
        for (int i = 0; i < sh->rf.nb; i++) {
            const nvn_rbind *b = &sh->rf.b[i];
            uint64_t h;
            if (b->kind == NVN_RB_IMG) h = b->reg < 32 ? bound_img(sh->stage, b->reg) : 0;
            else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TBUF) h = bound_tex(sh->stage, b->reg);
            else continue;
            uint64_t ser = h ? h_ser(h) : 0;
            if (!ser) continue;
            for (int gi = 0; gi < g_nlc; gi++) {
                const lc_group *g = &g_lc[gi];
                for (int k = 0; k < 8; k++) if (g->clear_c[k] && g->rt.color[k] && g->rt.color[k]->serial == ser) return 1;
                if ((g->clear_d || g->clear_s) && g->rt.depth && g->rt.depth->serial == ser) return 1;
            }
        }
    }
    return 0;
}
// the pass's own pending clear is newer still
static void lc_take_rt(void) {
    for (int gi = g_nlc - 1; gi >= 0; gi--) {
        lc_group *g = &g_lc[gi];
        for (int i = 0; i < 8; i++) if (g->clear_c[i])
            for (int j = 0; j < R.rt.n && j < 8; j++) if (R.rt.color[j] && !rt_dup_slot(&R.rt, j) && !nvn_fmt_bc(R.rt.color[j]->d.format) && lc_same(&g->rt, i, &R.rt, j)) {
                if (!R.clear_c[j]) { R.clear_c[j] = 1; R.cc[j] = g->cc[i]; }
                g->clear_c[i] = 0;
                g_lc_taken++;
                break;
            }
        if ((g->clear_d || g->clear_s) && R.rt.depth && lc_same(&g->rt, 8, &R.rt, 8)) {
            if (g->clear_d) { if (!R.clear_d) { R.clear_d = 1; R.cd = g->cd; } g->clear_d = 0; }
            if (g->clear_s) { if (!R.clear_s) { R.clear_s = 1; R.cs = g->cs; } g->clear_s = 0; }
            g_lc_taken++;
        }
    }
    lc_land();
}
static void lc_take(NvProg *pr) {
    if (lc_sampled(pr)) { lc_land(); return; }
    lc_take_rt();
}
static int lc_keeps(uint32_t op) {
    switch (op) {
        case OP_SET_RT: case OP_CLEAR_COLOR: case OP_CLEAR_DS: case OP_DRAW: case OP_DRAW_IDX: case OP_BEGIN: case OP_JUMP: case OP_CALL:
        case OP_SCISSOR: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF: case OP_BIND_VBUF1:
        case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_TEX1: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL:
        case OP_POLY_OFFSET: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_DEPTH_CLAMP: case OP_LINE_WIDTH: case OP_VP_SWIZZLE: case OP_BLEND_COLOR: case OP_ALPHA_REF: case OP_PUSH_CONST: case OP_COPY_STRIDE:
        case OP_RAST_DISCARD: case OP_PRIM_RESTART: case OP_BARRIER: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: case OP_RENDER_COND:
        case OP_UPDATE_UB: case OP_COPY_B2B: case OP_CLEAR_BUF: return 1;
        default: return 0;
    }
}

static void dispatch(const p_dispatch *dp);
static int up_mfx_wanted(NvShader *cs);
static int up_mfx_encode(NvShader *cs);
static int up_chain_dispatch(NvShader *cs, int indirect);
static int ndve_engine_dispatch(NvShader *cs, const p_dispatch *dp);
// DM_NVN_CS_HOIST: Metal cannot dispatch inside a render pass. A dispatch up to the next pass-ending op runs before the pass if the draws before it neither read what it
// writes nor write memory, and it samples no colour attachment (its depth only while no draw before it writes depth).
static int cshoist_on(void) {
    static int m = -1;
    if (m < 0) {
        const char *e = getenv("DM_NVN_CS_HOIST");
        m = !(e && !strcmp(e, "0"));
        if (!m) MLOG("DM_NVN_CS_HOIST=0: compute dispatches between the draws of a pass end it");
    }
    return m;
}
typedef struct { uint64_t lo, hi; } csh_rng;
enum { CSH_RN = 1024, CSH_MAX = 64 };
static const uint8_t *g_csh_op[CSH_MAX];
static int g_csh_n;
static uint64_t g_csh_moved, g_csh_depth, g_csh_stop[8];
static int csh_take(const uint8_t *p) {
    for (int i = 0; i < g_csh_n; i++) if (g_csh_op[i] == p) { g_csh_op[i] = g_csh_op[--g_csh_n]; return 1; }
    return 0;
}
static int csh_add(csh_rng *r, int *n, uint64_t a, uint64_t sz) {
    if (!a) return 1;
    // sparse addresses can name the same storage through different pools
    if (!sz || sz > UINT64_MAX - a || *n >= CSH_RN || nvn_sparse_alias(a, a + sz)) return 0;
    r[(*n)++] = (csh_rng){a, a + sz}; return 1;
}
static int csh_hit(const csh_rng *a, int na, const csh_rng *b, int nb) {
    for (int i = 0; i < na; i++)
        for (int j = 0; j < nb; j++) if (a[i].lo < b[j].hi && b[j].lo < a[i].hi) return 1;
    return 0;
}
static int csh_attachment_hit(uint64_t addr, uint64_t size) {
    if (!addr) return 0;
    csh_rng access[1]; int na = 0;
    if (!csh_add(access, &na, addr, size)) return 1;
    for (int k = 0; k < 9; k++) {
        const tex_t *t = k == 8 ? R.rt.depth : k < R.rt.n ? R.rt.color[k] : NULL;
        if (!t || !t->mem) continue;
        csh_rng storage[1]; int ns = 0;
        if (!csh_add(storage, &ns, (uint64_t)t->mem, t->L.size) || csh_hit(access, na, storage, ns)) return 1;
    }
    return 0;
}
static int csh_draw_reads(NvProg *pr, const p_draw *dw, const p_draw_idx *di, csh_rng *r, int *n) {
    if (!pr || pr->sh[3] || pr->sh[4] || S.rast_discard || (dw && dw->mdi_cnt) || (di && (di->mdi_cnt || di->ind))) return 0;
    for (int k = 0; k < 16; k++) if (S.vbuf[k].size && !csh_add(r, n, S.vbuf[k].address, S.vbuf[k].size)) return 0;
    if (di && !csh_add(r, n, di->ibuf, (uint64_t)(di->count > 0 ? di->count : 0) * 4 + 16)) return 0;
    if (!csh_add(r, n, dw ? dw->ind : di ? di->ind : 0, 32)) return 0;
    for (int st = 0; st < 2; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
        if (sh->rf.bindless || sh->rf.dyn) return 0;
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i];
            switch (b->kind) {
                case NVN_RB_CBUF: case NVN_RB_UBOP: if (b->reg < 16 && !csh_add(r, n, S.ubo[st][b->reg].addr, S.ubo[st][b->reg].size)) return 0; break;
                case NVN_RB_TEX: { uint64_t h = bound_tex(st, b->reg); int tid = (int)(h & 0xfffff); if (!tid) break;
                    os_unfair_lock_lock(&g_rmu); tex_t t = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t : (tex_t){0}; os_unfair_lock_unlock(&g_rmu);
                    // linear and buffer textures read pool buffers directly, even when reflected as TEX
                    if (!t.serial || !csh_add(r, n, (uint64_t)t.mem, t.L.size)) return 0;
                    break; }
                case NVN_RB_TEXU: case NVN_RB_TEXD: case NVN_RB_TEXA: return 0;
                case NVN_RB_SBUFP: case NVN_RB_SBUFU: case NVN_RB_SBUFQ: case NVN_RB_IMG: case NVN_RB_TBUF: case NVN_RB_GMEM: return 0;
                default: break; } } }
    return 1;
}
// storage buffers count as written; returns 1 when it samples a colour attachment of the pass, 2 only the depth
static int csh_cs(NvShader *cs, const p_dispatch *dp, csh_rng *w, int *nw, int *depth) {
    if (!cs || dp->ind || cs->rf.bindless || cs->rf.dyn) return 0;
    for (int i = 0; i < cs->rf.nb; i++) { const nvn_rbind *b = &cs->rf.b[i];
        switch (b->kind) {
            case NVN_RB_SBUFP: if (b->reg >= 16 || csh_attachment_hit(S.ssbo[5][b->reg].addr, S.ssbo[5][b->reg].size) || !csh_add(w, nw, S.ssbo[5][b->reg].addr, S.ssbo[5][b->reg].size)) return 0; break;
            case NVN_RB_CBUF: case NVN_RB_UBOP: if (b->reg >= 16 || csh_attachment_hit(S.ubo[5][b->reg].addr, S.ubo[5][b->reg].size)) return 0; break;
            case NVN_RB_SBUFU: case NVN_RB_SBUFQ: case NVN_RB_IMG: case NVN_RB_TEXU: case NVN_RB_TEXD: case NVN_RB_TEXA: case NVN_RB_GMEM: return 0;
            case NVN_RB_TEX: case NVN_RB_TBUF: { uint64_t h = bound_tex(5, b->reg); int tid = (int)(h & 0xfffff); if (!tid) break;
                os_unfair_lock_lock(&g_rmu); tex_t t = tid < g_ntreg && g_treg[tid].ok ? g_treg[tid].t : (tex_t){0}; os_unfair_lock_unlock(&g_rmu);
                if (!t.serial) return 0;
                for (int k = 0; k < R.rt.n && k < 8; k++) if (tex_storage_overlap(&t, R.rt.color[k])) return 0;
                if (tex_storage_overlap(&t, R.rt.depth)) *depth = 1;
                break; }
            default: break; } }
    return 1;
}
// a cheap op-code scan: the full one saves and restores S and R and dirties every argument table
static int csh_dispatch_ahead(void) {
    op_cursor cur = {.p = g_exec_next}; for (int k = 0; k < g_ret_n; k++) cur.ret[cur.ret_n++] = g_ret[k];
    for (int guard = 0; guard < 4096; guard++) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!op_next(&cur, &q, &hq, &aq)) return 0;
        switch (hq.op) {
            case OP_DISPATCH: return 1;
            case OP_BARRIER: { uint32_t bits; memcpy(&bits, aq, sizeof bits); if (bits) return 0; continue; }
            case OP_DRAW: case OP_DRAW_IDX:
            case OP_ALPHA_REF: case OP_BIND_IMG: case OP_BIND_PROGRAM: case OP_BIND_SAMP: case OP_BIND_SSBO: case OP_BIND_STATE: case OP_BIND_TEX: case OP_BIND_TEX1:
            case OP_BIND_UBO: case OP_BIND_UBO_SMALL: case OP_BIND_VATTR: case OP_BIND_VBUF: case OP_BIND_VBUF1: case OP_BIND_VSTREAM: case OP_BLEND_COLOR: case OP_COPY_STRIDE:
            case OP_DEPTH_CLAMP: case OP_DEPTH_RANGE: case OP_POLY_OFFSET: case OP_PRIM_RESTART: case OP_PUSH_CONST: case OP_RAST_DISCARD: case OP_SCISSOR: case OP_SET_SAMP_POOL:
            case OP_SET_TEX_POOL: case OP_STENCIL: case OP_VIEWPORTS: case OP_VP_SWIZZLE:
                continue;
            default: return 0;
        }
    }
    return 1;
}
static void cs_hoist_try(NvProg *pr0, const p_draw *dw0, const p_draw_idx *di0) {
    if (!g_exec_next || g_ret_n > 8 || g_nlc || R.clear_d || R.clear_s) return;
    for (int i = 0; i < 8; i++) if (R.clear_c[i]) return;   // a clear this pass takes as its load action comes before anything a moved dispatch could read
    {
        static int pre = -1;
        if (pre < 0) { const char *e = getenv("DM_NVN_CSH_PRESCAN"); pre = !(e && !strcmp(e, "0")); }
        if (pre && !csh_dispatch_ahead()) { g_csh_stop[5]++; return; }
    }
    static csh_rng rd[CSH_RN]; int nrd = 0;
    if (!csh_draw_reads(pr0, dw0, di0, rd, &nrd)) { g_csh_stop[0]++; return; }
    op_cursor cur = {.p = g_exec_next}; for (int k = 0; k < g_ret_n; k++) cur.ret[cur.ret_n++] = g_ret[k];
    static typeof(S) Ssave;
    memcpy(&Ssave, &S, sizeof S);
    typeof(R) Rsave = R;
    int pso_dirty = g_pso_dirty, dss_dirty = g_dss_dirty, moved = 0;
    int dwrote = R.rt.depth && (S.ds.dw || S.ds.st);   // either plane: a compute texture view may read stencil
    for (int guard = 0; guard < 4096; guard++) {
        const uint8_t *q; op_hdr hq; const void *aq;
        if (!op_next(&cur, &q, &hq, &aq)) break;
        if (hq.op == OP_BARRIER) { uint32_t bits; memcpy(&bits, aq, sizeof bits); if (bits) { g_csh_stop[5]++; break; } continue; }
        if (hq.op == OP_DRAW || hq.op == OP_DRAW_IDX) {
            p_draw tdw; p_draw_idx tdi;
            const p_draw *d = hq.op == OP_DRAW ? draw_rec(aq, hq.size, &tdw) : NULL;
            const p_draw_idx *x = hq.op == OP_DRAW_IDX ? draw_idx_rec(aq, hq.size, &tdi) : NULL;
            prog_resolve();
            __unsafe_unretained NvProg *pr = S.prog ? prog_get(S.prog) : nil;
            if (!csh_draw_reads(pr, d, x, rd, &nrd)) { g_csh_stop[1]++; break; }
            if (R.rt.depth && (S.ds.dw || S.ds.st)) dwrote = 1;
            continue; }
        if (hq.op == OP_DISPATCH) {
            const p_dispatch *dp = aq;
            __unsafe_unretained NvShader *cs = S.cprog && prog_get(S.cprog) ? prog_get(S.cprog)->sh[5] : nil;
            static csh_rng wr[CSH_RN]; int nwr = 0, depth = 0;
            if (!csh_cs(cs, dp, wr, &nwr, &depth)) { g_csh_stop[2]++; break; }
            if (depth && dwrote) { g_csh_stop[7]++; break; }
            if (csh_hit(wr, nwr, rd, nrd)) { g_csh_stop[3]++; break; }
            if (g_csh_n >= CSH_MAX) { g_csh_stop[4]++; break; }
            PD("H %llu dispatch %s moved ahead of the pass%s", g_stat_pass + 1, cs->key, depth ? " (samples the pass's depth, which no draw before it wrote)" : "");
            dispatch(dp);
            g_csh_op[g_csh_n++] = q; g_csh_moved++; g_csh_depth += depth; moved++;
            continue; }
        if (state_op(hq.op, aq)) continue;
        g_csh_stop[5]++; break;
    }
    if (cur.bad) g_csh_stop[6]++;
    memcpy(&S, &Ssave, sizeof S);
    args_dirty_all();
    R = Rsave;
    g_pso_dirty = pso_dirty || moved; g_dss_dirty = dss_dirty || moved;
    prog_resolve();
    { static int once; if (moved && !once++) MLOG("compute dispatches moved ahead of the render pass they would split (DM_NVN_CS_HOIST): first case pass %llu, %d dispatches", g_stat_pass + 1, moved); }
}
void mtl_cshoist_stats(uint64_t *moved, uint64_t *depth, uint64_t *stops) { *moved = g_csh_moved; *depth = g_csh_depth; for (int i = 0; i < 8; i++) stops[i] = g_csh_stop[i]; }
// a merged pass remaps logical outputs and channel masks; a specialized fragment's outputs are unknown here
static uint32_t draw_color_writes(NvProg *pr) {
    NvShader *fs = pr ? pr->sh[1] : nil;
    if (!fs || S.rast_discard) return 0;
    uint32_t outputs = g_pso_fsfn || ms_fs_override(fs) || !fs->rf.color_known ? UINT32_MAX : fs->rf.color_mask;
    uint32_t mask = 0;
    for (int i = 0; i < 8; i++) {
        unsigned l = g_pso_lmap ? g_pso_lmap[i] : (unsigned)i;
        if (l < 8 && ((S.chmask.mask >> (4 * l)) & 0xf) && (outputs >> l & 1)) mask |= 1u << i;
    }
    return mask;
}
// Coincident records (stride zero) need a predicate per index; past the mask table's capacity the count is read once.
static void draw_multi(const p_multi_draw *m) {
    if (!m->cnt) return;
    ubd_land();
    p_draw arr = {.prim = m->prim, .instances = 1, .ind = m->ind, .mdi_cnt = m->cnt};
    p_draw_idx idx = {.prim = m->prim, .itype = m->itype, .instances = 1, .ibuf = m->ibuf, .ind = m->ind, .mdi_cnt = m->cnt};
    p_draw *dw = m->itype < 0 ? &arr : NULL;
    p_draw_idx *di = dw ? NULL : &idx;
    uint32_t n = (uint32_t)m->max;
    if (n <= MDX_SLOTS && m->stride && mdx_wanted(dw, di)) {
        g_mdx_range = m; g_md_masked_calls++; g_md_masked += n;
    } else {
        n = MIN(n, mdi_count(m->cnt));
        arr.mdi_cnt = idx.mdi_cnt = 0; g_md_counted_calls++; g_md_counted += n;
    }
    for (uint32_t i = 0; i < n; i++) {
        arr.mdi_i = idx.mdi_i = (int32_t)i;
        arr.ind = idx.ind = m->ind + (uint64_t)i * (uint64_t)m->stride;
        draw(dw, di);
    }
    g_mdx_range = NULL;
    g_mdxc.next = NULL;
}
static void draw_(const p_draw *dw, const p_draw_idx *di);
// DM_NVN_SKIP_FS=<fragment key>,... (diagnostic)
static int skip_fs_draw(NvProg *pr) {
    static char sk[16][20];
    static int nsk = -1;
    if (nsk < 0) {
        nsk = 0;
        for (const char *q = getenv("DM_NVN_SKIP_FS"); q && *q && nsk < 16; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
            size_t n = strcspn(q, ",");
            if (n > 16) n = 16;
            memcpy(sk[nsk], q, n);
            sk[nsk++][n] = 0;
        }
        if (nsk) MLOG("DM_NVN_SKIP_FS: dropping the draws of %d fragment shaders (diagnostic; the picture is wrong)", nsk);
    }
    if (!pr || !pr->sh[1]) return 0;
    if (g_ub_n > 0 && ub_skip(pr->sh[1]->key)) return 1;
    for (int i = 0; i < nsk; i++) if (!strncmp(pr->sh[1]->key, sk[i], strlen(sk[i]))) return 1;
    return 0;
}
static void draw(const p_draw *dw, const p_draw_idx *di) {
    cp_stamp start = cp_begin();
    if (dw && dw->prim == DM_PRIM_MESH_TASKS) mesh_tasks_draw(dw); else draw_(dw, di);
    cp_end(5, start);
}
static void draw_(const p_draw *dw, const p_draw_idx *di) {
    g_draw_binst = (uint32_t)(dw ? dw->base_instance : di ? di->base_instance : 0);   // c0 + 0x644 of the stages that read the driver segment
    if (passdump()) { NvProg *p = g_mtk_raster ? g_mtk_raster : S.prog ? prog_get(S.prog) : nil;
        PD("j %llu draw-entry vs=%s fs=%s prim=%d count=%d instances=%d indexed=%d indirect=%#llx discard=%d", g_stat_pass + !g_enc,
           p && p->sh[0] ? p->sh[0]->key : "-", p && p->sh[1] ? p->sh[1]->key : "-",
           dw ? dw->prim : di->prim, dw ? dw->count : di->count, dw ? dw->instances : di->instances, di != NULL,
           (unsigned long long)(dw ? dw->ind : di->ind), S.rast_discard); }
    // DM_NVN_INDLOG=n
    static long indlog = -1; if (indlog < 0) { const char *v = getenv("DM_NVN_INDLOG"); indlog = v ? atol(v) : 0; }
    uint64_t ia = dw ? dw->ind : di->ind;
    if (ia && (passdump() || indlog > 0)) { if (indlog > 0) indlog--;
        unsigned long long pass = g_stat_pass + !g_enc, fr = (unsigned long long)atomic_load(&g_cur_frame); uint64_t ib = di ? di->ibuf : 0; int isz = di ? (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4) : 0;
        uint32_t e_[2] = {0}; vm_size_t eg = 0; int eok = vm_read_overwrite(mach_task_self(), (vm_address_t)ia, sizeof e_, (vm_address_t)e_, &eg) == KERN_SUCCESS; uint32_t e0 = e_[0], e1 = e_[1];
        [g_commit_blocks addObject:^{
            uint32_t a[16] = {0}, ix[6] = {0}; vm_size_t got = 0; int ok = vm_read_overwrite(mach_task_self(), (vm_address_t)ia, sizeof a, (vm_address_t)a, &got) == KERN_SUCCESS;
            uint8_t raw[24] = {0}; int ok_i = ok && ib && vm_read_overwrite(mach_task_self(), (vm_address_t)(ib + (uint64_t)a[2] * (uint64_t)isz), (vm_size_t)(6 * isz), (vm_address_t)raw, &got) == KERN_SUCCESS;
            for (int k = 0; ok_i && k < 6; k++) ix[k] = isz == 1 ? raw[k] : isz == 2 ? ((uint16_t *)raw)[k] : ((uint32_t *)raw)[k];
            if (!ok) MLOG("[pass] indirect args F%llu pass %llu at %#llx: unreadable", fr, pass, (unsigned long long)ia);
            else MLOG("[pass] indirect args F%llu pass %llu at %#llx: %u %u %u %d %u (at encode %s%u %u)%s indices %u %u %u %u %u %u; words 5-15 %x %x %x %x %x %x %x %x %x %x %x", fr, pass, (unsigned long long)ia,
                      a[0], a[1], a[2], (int)a[3], a[4], eok ? "" : "unreadable ", e0, e1, ok_i ? "" : " (index buffer unreadable)", ix[0], ix[1], ix[2], ix[3], ix[4], ix[5],
                      a[5], a[6], a[7], a[8], a[9], a[10], a[11], a[12], a[13], a[14], a[15]); }]; }
    if (!g_mtk_raster && S.prog) {
        NvProg *bound = prog_get(S.prog);
        if (!g_wl.on && wl_wanted(bound, dw, di) && wl_draw(bound, dw, di)) return;
    }
    if (g_enc && g_fsp_pass) rp_end();
    if (g_enc && !g_mtk_raster) pass_split_try();
    int mdx_on = (dw && dw->mdi_cnt) || (di && di->mdi_cnt) ? mdx_wanted(dw, di) : 0;
    if (!mdx_on && ((di && di->mdi_cnt && !mdi_live(di->mdi_cnt, di->mdi_i)) || (dw && dw->mdi_cnt && !mdi_live(dw->mdi_cnt, dw->mdi_i)))) return;
    if (mdx_on && mdx_next(dw, di)) return;
    g_mdxc.next = NULL;
    if (!S.prog) { RED_ONCE(18, "no program bound before the draw"); PD("Z %llu draw skipped: no program bound", g_stat_pass + !g_enc); return; }
    __unsafe_unretained NvProg *pr = g_mtk_raster ? g_mtk_raster : prog_get(S.prog);
    image_prepare(pr, 0x1f);
    // Rasterizer discard suppresses fragments, not vertex execution or its storage and image writes (such a vertex shader has main0_discard).
    if (S.rast_discard && (!pr->sh[0] || !pr->sh[0]->discard_fn)) { PD("Z %llu draw skipped: rasterizer discard with no vertex storage or image writes vs=%s", g_stat_pass + !g_enc, pr->sh[0] ? pr->sh[0]->key : "-"); return; }
    if (g_nubd && (g_enc || ubd_lazy()) && ubd_conflict(pr, dw, di)) { g_ubd_conflicts++; rp_end(); ubd_land(); }
    if (g_nlc && !g_enc) lc_take(pr);
    if (ixc_wanted(dw, di) && !ixc_find(dw, di)) ixc_run(dw, di);
    uint64_t mdx = 0;
    if (mdx_on) { uint64_t ind = dw ? dw->ind : di->ind, cnt = dw ? dw->mdi_cnt : di->mdi_cnt; int i = dw ? dw->mdi_i : di->mdi_i;
        if (!mdx_find(ind, cnt, &mdx)) { mdx_run(dw, di); if (!mdx_find(ind, cnt, &mdx)) mdx = 0; }
        if (!mdx && !mdi_live(cnt, i)) return; }
    else if (!g_enc && g_exec_next) mdx_prescan();
    if (di && !g_wl.on && wide_wanted(di) && !wide_find(di)) wide_run(di);
    // draws that cannot draw anything return before the pass opens
    { if (skip_fs_draw(pr)) return;
      if (ms_draw(pr)) return; }
    // DM_NVN_SKIP_SMALL=<width> (diagnostic)
    { static int small = -1; if (small < 0) { const char *e = getenv("DM_NVN_SKIP_SMALL"); small = e ? atoi(e) : 0; if (small) MLOG("DM_NVN_SKIP_SMALL: dropping draws whose target width is ≤ %d (diagnostic; the picture is wrong)", small); }
      if (small > 0 && R.has_rt) { int k = R.rt.n && R.rt.color[0] ? 0 : R.rt.depth ? 8 : -1;
        if (k >= 0) { uint64_t s_; int lv, sl; rt_sub(&R.rt, k, &s_, &lv, &sl); const tex_t *t = k == 8 ? R.rt.depth : R.rt.color[0]; if ((int)MAX(t->d.width >> lv, 1u) <= small) return; } } }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK && !S.rast_discard && !g_wl.on) { PD("Z %llu draw skipped (before the pass begins): both faces culled", g_stat_pass + !g_enc); return; }
    g_samples_fb_left = 1;
    int rt_sampled = g_rt_merged && draw_samples_rt(pr); g_samples_fb_left = 0;
    if (rt_sampled) { static int once; if (!once++) MLOG("same-target continuation: a draw in the continued pass samples an attachment; split here (first case pass %llu)", g_stat_pass);
        PD("M %llu a draw in the continued pass samples an attachment (%s); split here", g_stat_pass, g_dsr_why ?: "?"); { static int nw; if (nw++ < 6) MLOG("continued pass split: %s (pass %llu)", g_dsr_why ?: "?", g_stat_pass); } rp_end(); }
    order_draw(pr, g_sm_pass);
    if (g_enc && g_vis_counting && g_vis_need && g_vis_n >= VIS_SLOTS) rp_end();   // the visibility buffer is bound for the whole pass
    if (g_enc && g_rp_aw && (!R.has_sc || (long)R.sc.x + R.sc.w > (long)g_rp_aw || (long)R.sc.y + R.sc.h > (long)g_rp_ah)) rp_end();
    fb_prepare(pr);
    if (R.has_sc && !g_enc && !S.rast_discard) {
        NSUInteger tw0 = 0, th0 = 0; rt_dims(&tw0, &th0);
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y < 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw0) x1 = tw0; if (y1 > (long)th0) y1 = th0;
        if (x1 <= x0 || y1 <= y0) { PD("Z %llu draw skipped (before the pass begins): empty scissor (%d,%d %dx%d, target %lux%lu)", g_stat_pass + 1, R.sc.x, R.sc.y, R.sc.w, R.sc.h, (unsigned long)tw0, (unsigned long)th0); return; }
    }
    if (!g_enc && !g_mtk_raster && cshoist_on() && !g_split_resume) cs_hoist_try(pr, dw, di);
    g_split_resume = 0;
    if (g_enc && g_sm_pass && draw_samples_rt(pr)) { PD("M %llu a draw in the slice-merged pass samples its attachment; split here", g_stat_pass); rp_end(); }
    if (g_enc && g_sm_pass && g_wl.on) { PD("M %llu a wide line in the slice-merged pass: split here", g_stat_pass); rp_end(); }
    if (!g_enc && g_exec_next && !g_mtk_raster && !g_wl.on && sm_on()) sm_plan(pr);
    if (!g_enc && !g_wl.on && !g_mtk.on) fsp_plan(pr, dw, di);
    if (g_fsp_plan.on) fsm_plan(pr, dw, di);
    vbuf_prepare(pr->sh[0] ? &pr->sh[0]->rf : NULL, dw, di);   // an unaligned GPU-written stream, or one the draw reads past, needs its ordered copy before the render encoder
    id<MTL4RenderCommandEncoder> en = rp_begin_ex(0);
    g_sm_plan.on = 0; g_fsp_plan.on = 0;
    if (!en) { PD("Z %llu draw skipped: the render pass did not begin vs=%.16s fs=%.16s", g_stat_pass, pr && pr->sh[0] ? pr->sh[0]->key : "-", pr && pr->sh[1] ? pr->sh[1]->key : "-"); return; }
    if ((g_fb_early_seq == g_draw_seq || g_fb_learn_seq == g_draw_seq) && !fb_early_after()) {
        rp_end(); g_fb_noearly = 1; fb_prepare(pr); g_fb_noearly = 0;
        vbuf_prepare(pr->sh[0] ? &pr->sh[0]->rf : NULL, dw, di); en = rp_begin_ex(0);
        if (!en) return;
    }
    // The pipeline also depends on whether a bound sampler has a border colour twin (pso_key.border); binding a sampler does not mark it dirty, so the draw compares it.
    static int cur_border; int border = prog_border(pr);
    static id<MTLRenderPipelineState> cur_ps; static NvProg *cur_pr; static uint64_t cur_ps_pass; static int cur_pull;
    if (g_fb_tseq == g_draw_seq && g_fb_tfn) { g_pso_lmap = g_fb_tlmap; g_pso_fsfn = g_fb_tfn; g_pso_fz = g_fb_tdesc; g_pso_dirty = 1; }
    if (g_mtk.on) { cur_ps = mtk_pso(pr); cur_pull = 0; cur_pr = nil; if (!cur_ps) { g_mtk.miss = 1; return; } }
    else if (g_wl.on) { cur_ps = wl_pso(pr); cur_pull = 1; cur_pr = nil; if (!cur_ps) { g_wl.miss = 1; return; } }
    else if (g_fsp_pass) { cur_ps = nil; for (int i = 0; i < g_fsp_n && !cur_ps; i++) cur_ps = g_fsp_ps[i]; cur_pull = 0; cur_pr = nil; g_pso_dirty = 1; }
    else if (g_pso_dirty || pr != cur_pr || cur_ps_pass != g_pass.serial || border != cur_border) {
        g_pso_amp = g_sm_pass; cur_ps = pso_get(pr, nil); g_pso_amp = 0;
        cur_pull = g_pso_pull; cur_pr = pr; cur_ps_pass = g_pass.serial; cur_border = border; g_pso_dirty = !cur_ps || cur_pull;
    }
    if (!cur_ps && g_sm_pass) {
        g_sm_nwait++; PD("M %llu slice-merged pass: no amplified pipeline yet for vertex %s fragment %s; split here", g_stat_pass, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-");
        rp_end(); en = rp_begin_ex(0); if (!en) return;
        cur_ps = pso_get(pr, nil); cur_pull = g_pso_pull; cur_pr = pr; cur_ps_pass = g_pass.serial; cur_border = border; g_pso_dirty = !cur_ps || cur_pull; }
    uint32_t raster_mask = draw_color_writes(pr);
    uint8_t lm_copy[8]; const uint8_t *draw_lmap = g_pso_lmap ? memcpy(lm_copy, g_pso_lmap, sizeof lm_copy) : NULL;
    if (g_pso_lmap) { g_pso_lmap = NULL; g_pso_fsfn = nil; memset(&g_pso_fz, 0, sizeof g_pso_fz); g_pso_dirty = 1; }
    __unsafe_unretained id<MTLRenderPipelineState> ps = cur_ps;
    if (!ps) { PD("Z %llu draw skipped: no render pipeline (vertex %s fragment %s)", g_stat_pass, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-"); return; }
    if (ps != g_enc_pso) { enc_r_pso(en, ps); g_enc_pso = ps; }
    if (g_sm_pass && g_sm_applied != g_sm_cur) {
        MTLVertexAmplificationViewMapping vm = {0, (uint32_t)(g_sm_cur - g_sm_lo)}; [en setVertexAmplificationCount:1 viewMappings:&vm]; g_sm_applied = g_sm_cur; }
    static id<MTLDepthStencilState> cur_ds; static uint64_t cur_ds_pass;
    // DM_NVN_DEPTHOVR=fragment key prefix,... (diagnostic): depth compares ALWAYS
    static char dko[8][24]; static int ndko = -1;
    if (ndko < 0) { ndko = 0; const char *e = getenv("DM_NVN_DEPTHOVR");
        for (const char *q = e; q && *q && ndko < 8; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 23) n = 23; memcpy(dko[ndko], q, n); dko[ndko++][n] = 0; } }
    if (ndko) { int ovr = 0; for (int i = 0; i < ndko && pr->sh[1]; i++) ovr |= !strncmp(pr->sh[1]->key, dko[i], strlen(dko[i]));
        unsigned df0 = (unsigned)S.ds.df; if (ovr) S.ds.df = 8; cur_ds = dss_get(g_pass.hd, g_pass.hs); S.ds.df = df0; cur_ds_pass = g_pass.serial; g_dss_dirty = 0; }
    else if (g_dss_dirty || cur_ds_pass != g_pass.serial) { cur_ds = dss_get(g_pass.hd, g_pass.hs); cur_ds_pass = g_pass.serial; g_dss_dirty = 0; }
    if (cur_ds != g_enc_dss) { enc_r_dss(en, cur_ds); g_enc_dss = cur_ds; }
    // each face has its own reference value (docs/reference/rendering.md, "Front and back stencil reference values")
    { uint32_t sr = S.st_ref[0], sb = S.st_ref[1];
      if (!(g_ed.ok & ED_SREF) || g_ed.sref != sr || g_ed.sref_b != sb) { enc_r_stencil_ref(en, sr, sb); g_ed.sref = sr; g_ed.sref_b = sb; g_ed.ok |= ED_SREF; } }
    NSUInteger tw = g_pass.tw, th = g_pass.th;
    { MTLViewport v = S.has_vp ? (MTLViewport){S.vp[0][0], S.vp[0][1], S.vp[0][2], S.vp[0][3], S.dr[0], S.dr[1]} : (MTLViewport){0, 0, tw, th, 0, 1};
      if (vp_origin_ll()) { v.originY += v.height; v.height = -v.height; }
      // NEGATIVE_Y is the viewport flipped about its centre (facing is decided after it); other swizzles are drawn unswizzled.
      { uint32_t sw; memcpy(&sw, S.vp_sw[0], 4);
        if (sw == 0x100) { v.originY += v.height; v.height = -v.height; }
        else if (sw) RED_ONCE(108, "viewport swizzle %d,%d,%d,%d not implemented (drawn unswizzled)", S.vp_sw[0][0], S.vp_sw[0][1] ^ 2, S.vp_sw[0][2] ^ 4, S.vp_sw[0][3] ^ 6); }
      if (!(g_ed.ok & ED_VP) || memcmp(&g_ed.vp, &v, sizeof v)) { enc_r_viewport(en, v); g_ed.vp = v; g_ed.ok |= ED_VP; } }
    MTLScissorRect sc = {0, 0, tw, th};
    if (R.has_sc && !S.rast_discard) {   // raster state cannot suppress vertex storage writes
        long x0 = R.sc.x < 0 ? 0 : R.sc.x, y0 = R.sc.y <= 0 ? 0 : R.sc.y, x1 = (long)R.sc.x + R.sc.w, y1 = (long)R.sc.y + R.sc.h;
        if (x1 > (long)tw) x1 = tw; if (y1 > (long)th) y1 = th;
        if (x1 <= x0 || y1 <= y0) { PD("Z %llu draw skipped: empty scissor (%d,%d %dx%d, target %lux%lu)", g_stat_pass, R.sc.x, R.sc.y, R.sc.w, R.sc.h, (unsigned long)tw, (unsigned long)th); return; }
        sc = (MTLScissorRect){(NSUInteger)x0, (NSUInteger)y0, (NSUInteger)(x1 - x0), (NSUInteger)(y1 - y0)};
    }
    if (!(g_ed.ok & ED_SC) || memcmp(&g_ed.sc, &sc, sizeof sc)) { enc_r_scissor(en, sc); g_ed.sc = sc; g_ed.ok |= ED_SC; }
    if (S.poly.cull == NVN_FACE_FRONT_AND_BACK && !S.rast_discard && !g_wl.on) { PD("Z %llu draw skipped: both faces culled", g_stat_pass); return; }
    { int cu = g_wl.on ? MTLCullModeNone : S.poly.cull == NVN_FACE_FRONT ? MTLCullModeFront : S.poly.cull == NVN_FACE_BACK ? MTLCullModeBack : MTLCullModeNone;
      int wi = (S.poly.front == NVN_FRONT_FACE_CCW) != g_queue_origin_ll ? MTLWindingCounterClockwise : MTLWindingClockwise;   // NVN's raster flip is taken when the queue is initialized, not when the device origin changes
      int fi = S.poly.mode == NVN_POLYGON_MODE_LINE && !g_wl.on ? MTLTriangleFillModeLines : MTLTriangleFillModeFill, cl = !S.depth_clamp;
      int own_bias = (S.poly.offset_en & (g_wl.on ? NVN_POLYGON_OFFSET_ENABLE_LINE : NVN_POLYGON_OFFSET_ENABLE_FILL)) != 0;
      float bi[3] = {0, 0, 0}; if (own_bias) { bi[0] = S.po.units; bi[1] = S.po.factor; bi[2] = S.po.clamp; }
      // Workaround for Apple's rasterizer (docs/reference/rendering.md, depth on the far plane): z = w vertices come out up to 2^-23 short of the far value, so a bias of 16 units
      // with clamping puts them on it. A reversed range (far below near) stays 2^-24 off: Metal scales the bias by the depth's exponent (open).
      if (pr->sh[0] && pr->sh[0]->rf.pos_far && !pr->sh[3] && !pr->sh[4] && !own_bias && !g_wl.on && (S.has_vp ? S.dr[1] > S.dr[0] : 1)) { bi[0] = 16; cl = 0; }
      if (!(g_ed.ok & ED_CULL) || g_ed.cull != cu) { enc_r_cull(en, (MTLCullMode)cu); g_ed.cull = cu; g_ed.ok |= ED_CULL; }
      if (!(g_ed.ok & ED_WIND) || g_ed.wind != wi) { enc_r_winding(en, (MTLWinding)wi); g_ed.wind = wi; g_ed.ok |= ED_WIND; }
      if (!(g_ed.ok & ED_FILL) || g_ed.fill != fi) { enc_r_fill(en, (MTLTriangleFillMode)fi); g_ed.fill = fi; g_ed.ok |= ED_FILL; }
      if (!(g_ed.ok & ED_BIAS) || memcmp(g_ed.bias, bi, sizeof bi)) { enc_r_depth_bias(en, bi[0], bi[1], bi[2]); memcpy(g_ed.bias, bi, sizeof bi); g_ed.ok |= ED_BIAS; }
      if (!(g_ed.ok & ED_CLIP) || g_ed.clip != cl) { enc_r_depth_clip(en, cl); g_ed.clip = cl; g_ed.ok |= ED_CLIP; }
      if (!(g_ed.ok & ED_BLEND) || memcmp(g_ed.blend, S.blend_color, sizeof g_ed.blend)) {
          enc_r_blend_color(en, S.blend_color[0], S.blend_color[1], S.blend_color[2], S.blend_color[3]);
          memcpy(g_ed.blend, S.blend_color, sizeof g_ed.blend);
          g_ed.ok |= ED_BLEND;
      }
    }
    if (cur_pull) { pull_bind(en, pr->sh[0] ? &pr->sh[0]->rf : NULL); g_hy_draws += !g_wl.on; }   // dm_fetch decodes every format
    else {
        int bad = bind_vbufs(en, pr->sh[0] ? &pr->sh[0]->rf : NULL);
        if (bad >= 0) { RED_ONCE(158, "vertex stream %d's buffer %#llx is in no memory pool; draw skipped", bad, (unsigned long long)S.vbuf[bad].address); return; }
        if (pr->sh[0]) vconv_bind(en, pr->sh[0], S.rast_discard, dw, di);
    }
    g_kd_force = kd_draw(pr, dw, di);
    if (pr->sh[0]) encode_args(en, pr->sh[0], pr->sh[0]->stage == 4 ? 4 : 0, 1);
    if (pr->sh[1] && !S.rast_discard) encode_args(en, pr->sh[1], 1, 0);
    // Binding a view can upgrade its texture through blit(), which ends the pass: drawing on the ended encoder crashes in the driver.
    if (g_enc != en) {
        static int again; if (again) { RED_ONCE(130, "binding a draw's arguments ended its render pass twice; draw skipped"); return; }
        PD("M %llu binding the draw's arguments ended the render pass (texture upgrade); encoded again", g_stat_pass);
        again = 1; draw(dw, di); again = 0; return;
    }
    // encode_args does not know the draw's map
    if (pr->sh[1] && !S.rast_discard) {
        NvShader *fs = pr->sh[1];
        if (fs->has_alpha && atest_on()) at_bind(en);
        if (fs->has_bz) bz_bind(en, draw_lmap);
        if ((fs->has_logic || fs->u_logic) && lop_on()) lo_bind(en);
    }
    int prim = dw ? dw->prim : di->prim;
    MTLPrimitiveType mp;
    if (g_wl.on) mp = MTLPrimitiveTypeTriangle;
    else switch (prim) {
        case 0: mp = MTLPrimitiveTypePoint; break; case 1: mp = MTLPrimitiveTypeLine; break; case 3: mp = MTLPrimitiveTypeLineStrip; break;
        case 4: case 6: case 7: mp = MTLPrimitiveTypeTriangle; break; case 5: mp = MTLPrimitiveTypeTriangleStrip; break;   // 6 triangle fan, 7 quads: Metal has neither, expanded into triangle lists
        default: { static int n; if (n++ < 8) MLOG("[RED] primitive type %d not implemented (vertex %s fragment %s; draw skipped). 14 is patches: the host has no tessellation (no shader of the game is a hull or domain shader)",   // nor the adjacency types 10-13, a geometry shader's inputs
                                                    prim, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-"); }
            PD("Z %llu draw skipped: primitive type %d", g_stat_pass, prim); return;
    }
    if (g_vis_counting && g_vis_need) {
        if (!g_enc_has_vis) RED_ONCE(34, "occlusion query counting began in a pass without a visibility buffer (the first reset happened mid-pass); that pass's samples are not counted");
        else {
            if (g_vis_cur < 0 || g_vis_cur_pass != g_pass.serial) {
                if (g_vis_n >= VIS_SLOTS) RED_ONCE(35, "visibility slots exhausted");
                else { g_vis_cur = (int)g_vis_n++; g_vis_cur_pass = g_pass.serial;
                    if (g_nvs == g_vscap) { g_vscap = g_vscap ? g_vscap * 2 : 64; g_vs = dm_realloc(g_vs, g_vscap * sizeof *g_vs, "the visibility slots"); }
                    g_vs[g_nvs++] = (vis_slot){(void *)CFBridgingRetain(g_vis), (uint32_t)g_vis_cur}; }
            }
            if (g_vis_cur >= 0 && g_enc_vis != g_vis_cur + 1) { enc_r_visibility(en, MTLVisibilityResultModeCounting, (NSUInteger)g_vis_cur * 8); g_enc_vis = g_vis_cur + 1; }
        }
    }
    else if (g_vis_counting) {
        if (!g_vis_unseen++) MLOG("occlusion queries: draws between a samples-passed report and the next reset are not counted (first in pass %llu; DM_NVN_VIS_NEED)", g_stat_pass);
        if (g_enc_vis) { enc_r_visibility(en, MTLVisibilityResultModeDisabled, 0); g_enc_vis = 0; } }
    g_stat_draw++;
    if (enc_label_on() && !g_el_draws++ && pr->sh[1]) { memcpy(g_el_key, pr->sh[1]->key, 8); g_el_key[8] = 0; }
    if (passdump()) {
        static uint64_t last_pass = ~0ull; static int all = -1; if (all < 0) all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;
        if (all || g_kd_force) PD("d %llu bv=%d bi=%d first=%d vs=%s fs=%s vc=%d inst=%d blend=%#x b0=%#llx dt=%d dw=%d df=%d cull=%d st=%d sf=%d,%d sop=%d,%d,%d/%d,%d,%d smask=%#x,%#x/%#x,%#x sref=%#x,%#x ib=%#llx it=%d cmask=%#x b1=%#llx b2=%#llx b3=%#llx po=%d/%g,%g,%g dc=%d", g_stat_pass, di ? di->base_vertex : 0, di ? di->base_instance : 0, dw ? dw->first : 0, pr->sh[0] ? pr->sh[0]->key : "-", pr->sh[1] ? pr->sh[1]->key : "-",
                    dw ? dw->count : di->count, dw ? dw->instances : di->instances, S.color.blend_en, (unsigned long long)*(uint64_t *)&S.blend[0], (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.poly.cull,
                    (int)S.ds.st, (int)S.ds.sf_f, (int)S.ds.sf_b, (int)S.ds.fail_f, (int)S.ds.zfail_f, (int)S.ds.zpass_f,
                    (int)S.ds.fail_b, (int)S.ds.zfail_b, (int)S.ds.zpass_b, S.st_vmask[0], S.st_vmask[1], S.st_wmask[0], S.st_wmask[1], S.st_ref[0], S.st_ref[1],
                    di ? (unsigned long long)di->ibuf : 0ull, di ? di->itype : 0, S.chmask.mask, (unsigned long long)*(uint64_t *)&S.blend[1], (unsigned long long)*(uint64_t *)&S.blend[2], (unsigned long long)*(uint64_t *)&S.blend[3],
                    (int)S.poly.offset_en, S.po.factor, S.po.units, S.po.clamp, S.depth_clamp);
        if (all && pr->sh[0]) {
            char ub[400]; size_t o = 0; NvShader *v = pr->sh[0];
            for (int i = 0; i < v->rf.nb && o < sizeof ub - 40; i++) if (v->rf.b[i].kind == NVN_RB_CBUF && v->rf.b[i].reg < 16) {
                const p_bind_buf *u = &S.ubo[0][v->rf.b[i].reg]; void *mb; uint64_t mo; const uint8_t *base; uint64_t h = 1469598103934665603ull;
                if (!u->addr || !u->size) { o += snprintf(ub + o, sizeof ub - o, " r%d=-", v->rf.b[i].reg); continue; }
                if (pend_hits(u->addr, u->size) || !mtl_addr(u->addr, u->size, &mb, &mo) || !(base = [(__bridge id<MTLBuffer>)mb contents])) { o += snprintf(ub + o, sizeof ub - o, " r%d=P/%llu", v->rf.b[i].reg, (unsigned long long)u->size); continue; }
                uint64_t h64 = 0; for (uint64_t q = 0; q < u->size; q++) { h = (h ^ base[mo + q]) * 1099511628211ull; if (q == 63) h64 = h; }
                { static uint64_t hp = ~0ull; static int hn; if (hp != g_stat_pass) { hp = g_stat_pass; hn = 0; }
                  if (hn < 12 && u->size <= 608 && v->rf.b[i].reg == 3) { hn++; char hx[1300]; for (uint64_t q = 0; q < u->size; q++) snprintf(hx + 2 * q, 3, "%02x", base[mo + q]); PD("x %llu r%d %s", g_stat_pass, v->rf.b[i].reg, hx); } }
                o += snprintf(ub + o, sizeof ub - o, " r%d=%016llx/%llu/%016llx", v->rf.b[i].reg, (unsigned long long)h, (unsigned long long)u->size, (unsigned long long)h64); }
            PD("u %llu%s", g_stat_pass, ub); }
        if (all) { char vb[1600]; size_t o = 0; o += snprintf(vb + o, sizeof vb - o, "nattr=%d", S.nattr);
            for (int i = 0; i < S.nattr && i < 32 && o < sizeof vb - 100; i++) { int st = S.attr[i].stream; if (!S.attr[i].fmt) continue;
                o += snprintf(vb + o, sizeof vb - o, " a%d{fmt=%#x off=%d s=%d stride=%u div=%u vb=%#llx/%llu}", i, S.attr[i].fmt, S.attr[i].offset, st,
                              st < 16 ? S.stream[st].stride : 0, st < 16 ? S.stream[st].divisor : 0, st < 16 ? (unsigned long long)S.vbuf[st].address : 0ull, st < 16 ? (unsigned long long)S.vbuf[st].size : 0ull); }
            PD("v %llu %s", g_stat_pass, vb);
            const char *cbd = getenv("DM_NVN_CBDUMP"), *cbk = getenv("DM_NVN_CBDUMP_KEY");
            if (cbd && cbk && pr->sh[0] && !strcmp(pr->sh[0]->key, cbk)) {
                char fp[1024];
                for (int st = 0; st < 16; st++) if (S.vbuf[st].address && S.vbuf[st].size) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_vb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, st, (unsigned long long)S.vbuf[st].address);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)S.vbuf[st].address, 1, S.vbuf[st].size, f); fclose(f); } }
                if (di && di->ibuf) {
                    snprintf(fp, sizeof fp, "%s/F%llu_p%llu_ib%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, di->itype, (unsigned long long)di->ibuf);
                    FILE *f = fopen(fp, "wb"); if (f) { fwrite((const void *)di->ibuf, 1, (size_t)di->count << (di->itype == 2 ? 2 : di->itype), f); fclose(f); } }
            } }
        if (last_pass != g_stat_pass) {
            last_pass = g_stat_pass;
            PD("D %llu prim=%d vc=%d inst=%d blend=%#x wm=%#x dt=%d dw=%d df=%d st=%d sc=%lu,%lu,%lu,%lu tgt=%lux%lu vp=%.0f,%.0f,%.0f,%.0f dr=%g,%g lo=%u", g_stat_pass, prim,
               dw ? dw->count : di->count, dw ? dw->instances : di->instances, S.color.blend_en, S.chmask.mask, (int)S.ds.dt, (int)S.ds.dw, (int)S.ds.df, (int)S.ds.st,
               (unsigned long)sc.x, (unsigned long)sc.y, (unsigned long)sc.width, (unsigned long)sc.height, (unsigned long)tw, (unsigned long)th,
               S.has_vp ? S.vp[0][0] : 0, S.has_vp ? S.vp[0][1] : 0, S.has_vp ? S.vp[0][2] : (float)tw, S.has_vp ? S.vp[0][3] : (float)th, S.has_vp ? S.dr[0] : 0.f, S.has_vp ? S.dr[1] : 1.f, (unsigned)S.color.logic_op);
            PD("s %llu front cmp=%u fail=%u zfail=%u pass=%u ref=%#x read=%#x write=%#x back cmp=%u fail=%u zfail=%u pass=%u ref=%#x read=%#x write=%#x cull=%u winding=%u", g_stat_pass,
               S.ds.sf_f, S.ds.fail_f, S.ds.zfail_f, S.ds.zpass_f, S.st_ref[0], S.st_vmask[0], S.st_wmask[0],
               S.ds.sf_b, S.ds.fail_b, S.ds.zfail_b, S.ds.zpass_b, S.st_ref[1], S.st_vmask[1], S.st_wmask[1], S.poly.cull, S.poly.front);
        }
    }
    g_kd_force = 0;
    udc_note_draw(pr, raster_mask);
    swrite_note(pr); order_note(pr);
    if (g_fsp_pass) { fsp_encode(en, mp, dw, di); if (g_fsm_na) fsm_run(en); rp_end(); return; }
    if (g_mtk.on) { mtk_encode(en); return; }
    if (g_wl.on) { wl_encode(en); return; }
    // a draw of 0 primitives is not encoded: Metal's validation aborts on a zero count
    if ((dw && dw->ind) || (di && di->ind)) {   // NVN's records have the layouts of MTLDraw(Indexed)PrimitivesIndirectArguments
        const ixc_ent *e = ixc_wanted(dw, di) ? ixc_find(dw, di) : NULL; void *mb, *ab; uint64_t mo, ao, ig;
        uint64_t ind = dw ? dw->ind : di->ind;
        if (ixc_wanted(dw, di)) {
            if (e && e->args) enc_r_draw_indexed_indirect(en, mp, MTLIndexTypeUInt32, e->ixb, e->ixlen, e->args);
            else RED_ONCE(88, "indirect draw skipped: the arguments for primitive %d were not converted (argument address %#llx is in no memory pool, or the conversion pipeline was not created)", prim, (unsigned long long)ind);
        }
        else if (!mdx && !mtl_addr(ind, dw ? 16 : 20, &ab, &ao)) RED_ONCE(24, "indirect draw skipped: argument address %#llx is in no memory pool", (unsigned long long)ind);
        else if (dw) { enc_r_draw_indirect(en, mp, mdx ? mdx : ((__bridge id<MTLBuffer>)ab).gpuAddress + ao); if (mdx) mdx_chain(dw->mdi_cnt, dw->mdi_i, mp, MTLIndexTypeUInt32, 0, 0); }
        else if (!mtl_gpu_addr(di->ibuf, 1, &mb, &mo, &ig)) RED_ONCE(22, "index buffer is in no memory pool");
        else { MTLIndexType ty = di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32; uint64_t il = ((__bridge id<MTLBuffer>)mb).length - mo;
            enc_r_draw_indexed_indirect(en, mp, ty, ig, il, mdx ? mdx : ((__bridge id<MTLBuffer>)ab).gpuAddress + ao); if (mdx) mdx_chain(di->mdi_cnt, di->mdi_i, mp, ty, ig, il); }
    } else if (dw && prim == 7) {
        quad_ib_make();
        if (dw->count >= 4) enc_r_draw_indexed(en, mp, (dw->count / 4) * 6, MTLIndexTypeUInt32, g_quad_gpu, g_quad_ib.length, dw->instances > 0 ? dw->instances : 1, dw->first, dw->base_instance);
    } else if (dw && prim == 6) {   // first as the base vertex
        fan_ib_make();
        if (dw->count >= 3) enc_r_draw_indexed(en, mp, (dw->count - 2) * 3, MTLIndexTypeUInt32, g_fan_gpu, g_fan_ib.length, dw->instances > 0 ? dw->instances : 1, dw->first, dw->base_instance);
    } else if (dw) {
        if (dw->count && (dw->instances > 1 || dw->base_instance)) enc_r_draw_instanced(en, mp, dw->first, dw->count, dw->instances > 0 ? dw->instances : 1, dw->base_instance);
        else if (dw->count) enc_r_draw(en, mp, dw->first, dw->count);
    } else {
        if (prim == 6 || prim == 7) {
            int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4;
            uint32_t nt = prim == 7 ? (uint32_t)(di->count / 4) * 2 : di->count >= 3 ? (uint32_t)di->count - 2 : 0;
            if (!nt) return;
            void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, (uint64_t)di->count * isz, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
            if (pend_hits(di->ibuf, (uint64_t)di->count * isz)) RED_ONCE(73, "triangle fan / quad index buffer has GPU-side writes in this submission: expanded from the current content");
            src += mo;
            id<MTLBuffer> ab; arena_at at = arena_take(((size_t)nt * 12 + 15) & ~(size_t)15, &ab);
            uint32_t *d = (uint32_t *)(void *)at.cpu;
            // The restart index (compared before the base vertex is added) ends a fan or quad; the next one starts after it.
            uint32_t count = nvn_expand_primitive(d, src, (uint32_t)di->count, (unsigned)isz, (unsigned)prim, S.restart.on, S.restart.index);
            if (count) enc_r_draw_indexed(en, mp, count, MTLIndexTypeUInt32, at.gpu, (NSUInteger)count * 4, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
            return;
        }
        if (di->itype == 0) {
            // Metal has only 16 / 32-bit indices
            if (!di->count) return;
            if (prim == 3 || prim == 5 || prim == 11 || prim == 13) { static int once; if (!once++) MLOG("8-bit indices in a strip (primitive %d, first case pass %llu): a primitive restart index 0xff stays 0x00ff when widened, which Metal does not restart on", prim, g_stat_pass); }
            const wide_ent *w = wide_wanted(di) ? wide_find(di) : NULL;
            if (w && w->gpu) {
                enc_r_draw_indexed(en, mp, di->count, MTLIndexTypeUInt16, w->gpu, (NSUInteger)di->count * 2, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
                return;
            }
            void *mb; uint64_t mo; const uint8_t *src;
            if (!mtl_addr(di->ibuf, di->count, &mb, &mo) || !(src = [(__bridge id<MTLBuffer>)mb contents])) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
            if (pend_hits(di->ibuf, di->count)) RED_ONCE(48, "8-bit index buffer has GPU-side writes in this submission and the GPU expansion failed: expanded from the current content");
            id<MTLBuffer> ab; arena_at at = arena_take(((size_t)di->count * 2 + 15) & ~(size_t)15, &ab);
            uint16_t *d = (uint16_t *)(void *)at.cpu; src += mo;
            for (int32_t i = 0; i < di->count; i++) d[i] = src[i];
            enc_r_draw_indexed(en, mp, di->count, MTLIndexTypeUInt16, at.gpu, (NSUInteger)di->count * 2, di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
            return;
        }
        void *mb; uint64_t mo;
        uint64_t gpu;
        if (!mtl_gpu_addr(di->ibuf, (uint64_t)di->count * (di->itype == 1 ? 2 : 4), &mb, &mo, &gpu)) { RED_ONCE(22, "index buffer is in no memory pool"); return; }
        if (di->count) enc_r_draw_indexed(en, mp, di->count, di->itype == 1 ? MTLIndexTypeUInt16 : MTLIndexTypeUInt32, gpu, ((__bridge id<MTLBuffer>)mb).length - mo,
                di->instances ? di->instances : 1, di->base_vertex, di->base_instance);
    }
}
