// A draw that samples its own attachment (docs/reference/rendering.md#same-pass-feedback-read-through-a-snapshot): the Switch reads what it held before the draw,
// Metal leaves it undefined. NVN makes rendering visible to texture reads only through nvnCommandBufferBarrier, so a snapshot is reused until the game's next barrier.
static int g_fb_mode = -1;   // 0 none, 1 writers with reuse until a barrier, 2 every sampling draw, fresh
static uint64_t g_fb_bar, g_fb_reuse, g_fb_split;
static uint64_t g_fb_pass = UINT64_MAX;
static uint32_t g_fb_wmask, g_fb_rmask, g_fb_dread, g_fb_dwrote;
typedef struct { uint64_t ser, bar, sub, use, draw; int lv, sl; uint32_t lvm; __strong id<MTLTexture> snap; } fb_cache_t;   // use: last taken or reused (g_fbc_use); draw: last handed to (g_draw_seq); lvm: levels copied into it
static fb_cache_t g_fbc[8];
static uint64_t g_fbc_use;
static int fb_mode(void) {
    if (g_fb_mode < 0) {
        const char *e = getenv("DM_NVN_FB_SNAP");
        g_fb_mode = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "all") ? 2 : 1;
        if (g_fb_mode != 1) MLOG("same-pass feedback sampling snapshot: %s (DM_NVN_FB_SNAP)", g_fb_mode ? "a fresh one for every draw sampling an attachment of its pass" : "off");
    }
    return g_fb_mode;
}
static inline void fb_pass_sync(void) {
    uint64_t p = g_stat_pass + !g_enc;
    if (p != g_fb_pass) { g_fb_pass = p; g_fb_wmask = g_fb_rmask = g_fb_dread = g_fb_dwrote = 0; g_fb_dread_pool = 0; }
}
static void fb_ds_note_write(uint32_t planes) { fb_pass_sync(); g_fb_dwrote |= planes; }
static void fb_ds_before_write(uint32_t planes) {
    if (g_enc && (planes & g_fb_dread)) {
        g_fb_split++; g_fb_split_pool += g_fb_dread_pool; rp_end(); fb_pass_sync();
    }
}
typedef struct { MTLPixelFormat pf; MTLTextureType type; NSUInteger w, h, arr, nl; } fb_shape_t;
static inline int fb_shape(const fb_cache_t *e, const fb_shape_t *k) {
    return e->snap && e->snap.pixelFormat == k->pf && e->snap.textureType == k->type && e->snap.width == k->w && e->snap.height == k->h && e->snap.arrayLength == k->arr && e->snap.mipmapLevelCount == k->nl;
}
static inline int fb_live(const fb_cache_t *e) { return e->ser && e->bar == g_fb_bar && e->sub == g_sub_serial; }
// Callers copy after rp_flush: a waiting clear becomes the load action of the pass this draw opens, and a copy before it would miss the clear.
// An entry handed to the current draw is never taken for another key of that draw: its snapshot is bound already.
static fb_cache_t *fb_snap_shape(uint64_t ser, int lv, int sl, const fb_shape_t *k, int force, int *fresh) {
    fb_cache_t *e = NULL;
    for (int i = 0; i < 8 && !e; i++) if (g_fbc[i].snap && g_fbc[i].ser == ser && g_fbc[i].lv == lv && g_fbc[i].sl == sl) e = &g_fbc[i];
    if (e && !fb_shape(e, k)) { e->ser = 0; e = NULL; }
    if (e && !force && fb_live(e)) { e->use = ++g_fbc_use; e->draw = g_draw_seq; *fresh = 0; return e; }
    if (!e) {   // by rank: same shape and not live, not live, same shape; LRU within a rank; none this draw holds
        int best = 1 << 30;
        for (int i = 0; i < 8; i++) {
            fb_cache_t *c = &g_fbc[i];
            if (c->draw && c->draw == g_draw_seq) continue;
            int r = fb_live(c) ? (fb_shape(c, k) ? 2 : 3) : fb_shape(c, k) ? 0 : 1;
            if (r < best || (r == best && c->use < e->use)) { best = r; e = c; }
        }
        if (!e) { RED_ONCE(170, "same-pass feedback sampling: every snapshot entry is held by this draw; the attachment is read as it is"); return NULL; }
        if (e->snap && !fb_shape(e, k)) { defer_obj_release(e->snap); e->snap = nil; }
    }
    if (!e->snap) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = k->type; d.pixelFormat = k->pf; d.width = k->w; d.height = k->h; d.arrayLength = k->arr; d.mipmapLevelCount = k->nl;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
        e->snap = rh_place(d);
        if (!e->snap) e->snap = [g_dev newTextureWithDescriptor:d];
        if (!e->snap) { e->ser = 0; return NULL; }
        res_add(e->snap);
    }
    e->ser = ser; e->lv = lv; e->sl = sl; e->bar = g_fb_bar; e->sub = g_sub_serial; e->use = ++g_fbc_use; e->draw = g_draw_seq; e->lvm = 0;
    *fresh = 1;
    return e;
}
static fb_cache_t *fb_snap(uint64_t ser, int lv, int sl, MTLPixelFormat pf, NSUInteger w, NSUInteger h, int force, int *fresh) {
    fb_shape_t k = {pf, MTLTextureType2D, w, h, 1, 1};
    fb_cache_t *e = fb_snap_shape(ser, lv, sl, &k, force, fresh);
    if (e && *fresh) e->lvm = 1;
    return e;
}
static void fb_substitute(id<MTLTexture> src, id<MTLTexture> use) {
    if (g_fb_use[g_fb_n] && g_fb_use[g_fb_n] != use) defer_obj_unref(g_fb_use[g_fb_n]);
    g_fb_src[g_fb_n] = src; g_fb_use[g_fb_n] = use; g_fb_rid[g_fb_n] = use.gpuResourceID._impl; g_fb_n++;
}
// Same-pass feedback read from the tile (docs/reference/rendering.md#same-pass-feedback-read-from-the-tile): the readable-tile version (<key>_bf) reads [[color(k)]],
// which holds every earlier fragment of the pass in order. Its pipeline builds in the background; until then the draw takes a snapshot.
static int fb_tile_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_FB_TILE");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("same-pass feedback sampling: no tile reads (DM_NVN_FB_TILE=0)");
    }
    return v;
}
static int mtk_drawing(void);
static uint64_t g_fb_tile_draws, g_fb_tile_wait, g_fb_tseq;
static id<MTLFunction> g_fb_tfn;
static uint8_t g_fb_tlmap[8];
static pso_fz_desc g_fb_tdesc;
enum { FB_TP_MAX = 256 };
static struct { pso_key k; id<MTLRenderPipelineState> ps; int done; } g_fb_tp[FB_TP_MAX];
static int g_fb_tp_n;
static os_unfair_lock g_fb_tp_lock = OS_UNFAIR_LOCK_INIT;
// A draw that opens its pass reads the tile with the formats learned from the pass rp_begin_ex opened for it before (fb_early_after); a pass that differs ends, takes the snapshot
// and opens again, and after three of those the entry plans no more.
typedef struct { uint32_t cfmt[8], dfmt, sfmt, samples, tw, th; } fb_pinfo;
typedef struct { uint64_t key; fb_pinfo pi; uint8_t ok, fails; } fb_early_t;
static fb_early_t g_fb_el[64];
static uint64_t g_fb_early_seq, g_fb_learn_seq, g_fb_early_key, g_fb_early_n, g_fb_early_miss;
static int g_fb_noearly;
static int fb_early_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_FB_TILE_EARLY");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("same-pass feedback sampling: a draw that opens its pass takes a snapshot (DM_NVN_FB_TILE_EARLY=0)");
    }
    return v;
}
static uint64_t fb_early_keyof(NvProg *pr) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)pr->sh[0];
#define FB_MIX(v) (h = (h ^ (uint64_t)(v)) * 0xff51afd7ed558ccdull, h ^= h >> 29)
    FB_MIX((uintptr_t)pr->sh[1]); FB_MIX(R.rt.n);
    for (int k = 0; k < R.rt.n && k < 8; k++) { FB_MIX(R.rt.color[k] ? R.rt.color[k]->serial : 0); FB_MIX(R.rt.color[k] && R.rt.has_cview[k] && R.rt.cview[k].set_levels ? R.rt.cview[k].base_level + 1 : 0); }
    FB_MIX(R.rt.depth ? R.rt.depth->serial : 0);
#undef FB_MIX
    return h ? h : 1;
}
static void fb_pinfo_now(fb_pinfo *pi) {
    memset(pi, 0, sizeof *pi);
    memcpy(pi->cfmt, g_pass.cfmt, sizeof pi->cfmt); pi->dfmt = g_pass.dfmt; pi->sfmt = g_pass.sfmt; pi->samples = g_pass.samples;
    pi->tw = (uint32_t)g_pass.tw; pi->th = (uint32_t)g_pass.th;
}
// 0: the draw planned a tile read for a pass that is not this one (the caller goes back to a snapshot)
static int fb_early_after(void) {
    if (g_fb_learn_seq == g_draw_seq) {
        g_fb_learn_seq = 0;
        fb_early_t *e = &g_fb_el[g_fb_early_key & 63];
        if (e->key != g_fb_early_key) { e->key = g_fb_early_key; e->fails = 0; }
        fb_pinfo_now(&e->pi); e->ok = !g_sm_pass;
        return 1;
    }
    if (g_fb_early_seq != g_draw_seq) return 1;
    g_fb_early_seq = 0;
    fb_early_t *e = &g_fb_el[g_fb_early_key & 63];
    fb_pinfo now; fb_pinfo_now(&now);
    if (e->key == g_fb_early_key && !g_sm_pass && !memcmp(&now, &e->pi, sizeof now)) { g_fb_early_n++; return 1; }   // NOLINT(bugprone-suspicious-memory-comparison): fb_pinfo_now zeroes the whole struct
    if (e->key == g_fb_early_key) { e->pi = now; if (e->fails < 255) e->fails++; }
    g_fb_early_miss++;
    static int once; if (!once++) MLOG("same-pass feedback sampling: a draw planned a tile read for the pass it opens, and another pass opened (formats, size or a slice-merged pass); it takes a snapshot (first case pass %llu)", g_stat_pass);
    return 0;
}
static int fb_tile_ready(NvShader *vs, NvShader *fs, id<MTLFunction> fn, const uint8_t lmap[8], const pso_fz_desc *fz, const fb_pinfo *pi) {
    const uint8_t *l0 = g_pso_lmap; g_pso_lmap = lmap;
    pso_key k; pso_key_fill(&k, vs, fs, pi->cfmt, pi->dfmt, pi->sfmt, pi->samples, 1); g_pso_lmap = l0;
    memcpy(k.lmap, lmap, 8); k.fsfn = (__bridge void *)fn;
    if (hm_get_id(&g_pso, &k)) return 1;
    os_unfair_lock_lock(&g_fb_tp_lock);
    int i = 0;
    for (; i < g_fb_tp_n; i++) if (!memcmp(&g_fb_tp[i].k, &k, sizeof k)) break;   // NOLINT(bugprone-suspicious-memory-comparison): pso_key_fill zeroes padding too
    if (i < g_fb_tp_n) {
        id ps = g_fb_tp[i].done ? g_fb_tp[i].ps : nil;
        os_unfair_lock_unlock(&g_fb_tp_lock);
        if (!ps) return 0;   // building, or failed (its snapshot stays)
        hm_put_id(&g_pso, &k, ps);
        pso_skey sk; char shex[sizeof sk * 2 + 1];
        pso_skey_from(&k, vs, fs, 1, fz, &sk); pso_skey_hex(&sk, shex);
        pso_record("R\t%s\n", shex); pso_user_record("R\t%s\n", shex);
        return 1;
    }
    if (g_fb_tp_n >= FB_TP_MAX) {
        os_unfair_lock_unlock(&g_fb_tp_lock);
        static int once;
        if (!once++) MLOG("same-pass feedback sampling: %d tile-reading pipelines built; later ones keep the snapshot", FB_TP_MAX);
        return 0;
    }
    g_fb_tp[i].k = k; g_fb_tp[i].ps = nil; g_fb_tp[i].done = 0; g_fb_tp_n++;
    os_unfair_lock_unlock(&g_fb_tp_lock);
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, fn, 1); int off = pso_off(&k, fs, 1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
        NSError *e = nil;
        id<MTLRenderPipelineState> ps = pso_build(d, off, &e);
        if (!ps) MLOG("[RED] same-pass feedback sampling: no tile-reading pipeline for vertex %s fragment %s: %s (its draws keep the snapshot)", vs->key, fs->key, e.localizedDescription.UTF8String);
        os_unfair_lock_lock(&g_fb_tp_lock); g_fb_tp[i].ps = ps; g_fb_tp[i].done = 1; os_unfair_lock_unlock(&g_fb_tp_lock);
    });
    return 0;
}
// A shader's bindings that name a texture or an image, and those ubd_conflict_sh looks at, as indices into rf.b in its order: the per-draw checks (fb_depth_rule,
// fb_prepare, ubd_conflict_*) walk these instead of every binding (docs/reference/frame-rate.md, "Per-draw checks walked every reflected binding"). Made once a
// shader under the lock; lists_nb is stored last with release and read with acquire, so a thread that sees it also sees both lists.
static os_unfair_lock g_sh_lists_lock = OS_UNFAIR_LOCK_INIT;
static __attribute__((noinline)) void sh_lists_make(NvShader *sh) {
    os_unfair_lock_lock(&g_sh_lists_lock);
    if (__atomic_load_n(&sh->lists_nb, __ATOMIC_ACQUIRE) == sh->rf.nb + 1) { os_unfair_lock_unlock(&g_sh_lists_lock); return; }
    int n = 0, m = 0;
    for (int i = 0; i < sh->rf.nb && i < NVN_REFL_MAXB; i++) {
        switch (sh->rf.b[i].kind) {   // every kind ubd_conflict_textures, fb_prepare and fb_depth_rule act on (t_binding_lists)
            case NVN_RB_TEX: case NVN_RB_TBUF: case NVN_RB_TEXU: case NVN_RB_TBUFU: case NVN_RB_IMG: case NVN_RB_IMGU:
            case NVN_RB_TEXD: case NVN_RB_TEXA: case NVN_RB_TEXP: case NVN_RB_TEXH: sh->tex_idx[n++] = (uint8_t)i; break;
            default: break;
        }
        switch (sh->rf.b[i].kind) {   // what ubd_conflict_sh looks at beside the textures
            case NVN_RB_CBUF: case NVN_RB_SBUFP: case NVN_RB_UBOP: case NVN_RB_GMEM:
            case NVN_RB_TEXU: case NVN_RB_SAMPU: case NVN_RB_SBUFU: case NVN_RB_IMGU: case NVN_RB_TBUFU: sh->buf_idx[m++] = (uint8_t)i; break;
            default: break;
        }
    }
    sh->tex_n = (uint8_t)n; sh->buf_n = (uint8_t)m; __atomic_store_n(&sh->lists_nb, sh->rf.nb + 1, __ATOMIC_RELEASE);
    os_unfair_lock_unlock(&g_sh_lists_lock);
}
static inline void sh_lists(NvShader *sh) { if (__builtin_expect(__atomic_load_n(&sh->lists_nb, __ATOMIC_ACQUIRE) != sh->rf.nb + 1, 0)) sh_lists_make(sh); }
static inline int sh_tex_list(NvShader *sh, const uint8_t **l) { sh_lists(sh); *l = sh->tex_idx; return sh->tex_n; }
static inline int sh_buf_list(NvShader *sh, const uint8_t **l) { sh_lists(sh); *l = sh->buf_idx; return sh->buf_n; }
// texture slots (bit per slot 0-255) read from the tile; 0 = none (then g_fb_tfn and the rest are not this draw's)
static int fb_tile_plan(NvProg *pr, uint32_t wm, uint64_t tile[4]) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1]; memset(tile, 0, 4 * sizeof *tile);
    if (g_fb_mode != 1 || !vs || !(fs->rf.fc_proj | fs->rf.fc_fpos) || !fs->rf.color_known || !fs->rf.nfo || !fb_tile_on()) return 0;
    fb_pinfo pi; int learn = 0;
    if (g_enc) { fb_pinfo_now(&pi); if (g_sm_pass) return 0; }
    else {
        if (!fb_early_on() || g_fb_noearly) return 0;
        uint64_t key = fb_early_keyof(pr);
        fb_early_t *e = &g_fb_el[key & 63];
        g_fb_early_key = key;
        if (e->key != key) { learn = 1; memset(&pi, 0, sizeof pi); }
        else if (!e->ok || e->fails >= 3) return 0;
        else pi = e->pi;
    }
    if (atest_on() || lop_on() || pi.samples > 1 || S.rast_discard || mtk_drawing() || ms_fs_override(fs)) return 0;
    NSUInteger W = pi.tw, H = pi.th;
    int fx[32];
    for (int i = 0; i < 32; i++) fx[i] = -1;
    uint8_t fbmask = 0;
    for (int i = 0; i < fs->rf.nb; i++) {
        const nvn_rbind *b = &fs->rf.b[i];
        if (b->kind != NVN_RB_TEX || b->reg >= NTEX) continue;
        int tid = (int)(bound_tex(1, b->reg) & 0xfffff); uint64_t ser = tid_ser(tid); if (!ser) continue;
        int att = 0;
        for (int k = 0; k < R.rt.n && k < 8; k++) att |= R.rt.color[k] && R.rt.color[k]->serial == ser && ((wm | g_fb_wmask) >> k & 1);
        if (!att) continue;   // under g_rmu, and still the ID's
        os_unfair_lock_lock(&g_rmu);
        void *tv = NULL;
        if (tid < g_ntreg && g_treg[tid].ok && g_treg[tid].t.serial == ser) tex_fast_rid(tid, &tv);
        id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
        os_unfair_lock_unlock(&g_rmu);
        if (!t) continue;
        for (int k = 0; k < R.rt.n && k < 8; k++) {
            if (!R.rt.color[k] || R.rt.color[k]->serial != ser || !((wm | g_fb_wmask) >> k & 1)) continue;   // nothing in this pass wrote it: read directly
            if (learn) { g_fb_learn_seq = g_draw_seq; return 0; }
            uint64_t aser; int lv, sl; rt_sub(&R.rt, k, &aser, &lv, &sl);
            id<MTLTexture> base = t.parentTexture ? t.parentTexture : t;
            if (t.textureType != MTLTextureType2D || t.mipmapLevelCount != 1 || t.sampleCount != 1 || base.parentTexture || (t.parentTexture ? t.parentRelativeLevel : 0) != (NSUInteger)lv
                || (t.parentTexture ? t.parentRelativeSlice : 0) != (NSUInteger)sl || t.pixelFormat != (MTLPixelFormat)pi.cfmt[k] || b->dtt != MTLTextureType2D
                || !sw_tile(t)
                || MAX(base.width >> lv, 1ul) != W || MAX(base.height >> lv, 1ul) != H || (fs->rf.fc_mixed[b->reg >> 6] >> (b->reg & 63) & 1)) break;
            int any = 0, ok = 1;
            for (int c = 0; c < fs->rf.nfc; c++) {
                if (fs->rf.fc_reg[c] != b->reg) continue;
                if (fs->rf.fc_fpos >> c & 1 ? fpos_proof(fs, c, W, H) : fs->rf.fc_proj >> c & 1 && scr_proof(pr, fs->rf.fc_locn[c], W, H)) any = 1; else ok = 0;
            }
            if (!ok || !any) break;
            for (int c = 0; c < fs->rf.nfc; c++) if (fs->rf.fc_reg[c] == b->reg) fx[c] = k;
            fbmask |= (uint8_t)(1u << k); tile[b->reg >> 6] |= 1ull << (b->reg & 63);
            break;
        }
    }
    if (!fbmask) return 0;
    uint8_t lmap[8]; memset(lmap, 0xff, 8);
    for (int q = 0; q < 8; q++) if (fs->rf.color_mask >> q & 1) lmap[q] = (uint8_t)q;
    pso_fz_desc d; memset(&d, 0, sizeof d); d.kind = 1; d.fbmask = fbmask;
    for (int i = 0; i < 32; i++) d.fx[i] = (int8_t)fx[i];
    id<MTLFunction> fn = bf_fn(fs, fx, fbmask, lmap);
    if (!fn || !fb_tile_ready(vs, fs, fn, lmap, &d, &pi)) { g_fb_tile_wait += fn != nil; memset(tile, 0, 4 * sizeof *tile); return 0; }
    g_fb_tfn = fn; memcpy(g_fb_tlmap, lmap, 8); g_fb_tdesc = d;
    if (!g_enc) g_fb_early_seq = g_draw_seq;
    return 1;
}
// *lo to *hs carry weight; *hi also a level a linear mip filter fetches at weight 0. 0 when unknown (only a combined handle's sampler is known).
// A texel fetch names its level without the sampler: the slot's reads are taken as samples.
static int fb_reach(const NvShader *fs, unsigned reg, int sid, int n, int *lo, int *hs, int *hi) {
    int combined = 0;
    for (int j = 0; j < fs->rf.nb && !combined; j++) combined = fs->rf.b[j].kind == NVN_RB_SAMPT && fs->rf.b[j].reg == reg;
    if (!combined || sid <= 0 || n < 1) return 0;
    os_unfair_lock_lock(&g_rmu); int ok = sid < g_nsreg && g_sreg_ok[sid]; samp_par sp = ok ? g_sreg[sid] : (samp_par){0}; os_unfair_lock_unlock(&g_rmu);
    if (!ok) return 0;
    if (sp.minf < 2) { *lo = *hs = *hi = 0; return 1; }   // minf 0, 1: no mip filter
    float a = fminf(fmaxf(sp.lod_min, 0), 16), b = fminf(fmaxf(sp.lod_max, 0), 16);
    if (!(a <= b)) return 0;   // an inverted (or NaN) clamp: what the GPU reads is not defined here
    int l = (int)floorf(a), s = (int)ceilf(b), h = sp.minf >= 4 ? (int)floorf(b) + 1 : s;   // minf 4, 5: linear mip filter
    *lo = MIN(l, n - 1); *hs = MIN(s, n - 1); *hi = MIN(MAX(h, s), n - 1);
    return 1;
}
void mtl_fb_stats(uint64_t *snaps, uint64_t *reused, uint64_t *tile, uint64_t *wait) { *snaps = g_fb_count; *reused = g_fb_reuse; *tile = g_fb_tile_draws; *wait = g_fb_tile_wait; }
static uint32_t fb_depth_planes(int tid, uint64_t ser) {
    os_unfair_lock_lock(&g_rmu);
    void *view = NULL;
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].t.serial == ser) tex_fast_rid(tid, &view);
    MTLPixelFormat format = view ? ((__bridge id<MTLTexture>)view).pixelFormat : MTLPixelFormatInvalid;
    os_unfair_lock_unlock(&g_rmu);
    switch (format) {
        case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatDepth32Float_Stencil8: return FB_DS_DEPTH;
        case MTLPixelFormatStencil8: case MTLPixelFormatX32_Stencil8: return FB_DS_STENCIL;
        default: return FB_DS_BOTH;
    }
}
// docs/reference/rendering.md#bindless-shaders-taken-as-depth-readers. texa: whether a TEXA handle table in c2, not read here, counts as naming it
static uint32_t fb_texu_reads(NvShader *fs, uint64_t ser, int texa) {
    uint32_t planes = 0;
    for (int i = 0; i < fs->rf.nb; i++) {
        const nvn_rbind *b = &fs->rf.b[i];
        if (b->kind == NVN_RB_TEXA) { if (texa) return FB_DS_BOTH; continue; }
        if (b->kind == NVN_RB_TEXH) {
            __unsafe_unretained id<MTLBuffer> pb = nil; int hit = 0;
            os_unfair_lock_lock(&g_rmu);
            htab_gpu(1, b, &pb);
            for (uint32_t k = 0; k < g_htab_last->n && !hit; k++) hit = g_htab_last->ser[k] == ser;
            os_unfair_lock_unlock(&g_rmu);
            if (hit) return FB_DS_BOTH;
            continue;
        }
        if (b->kind != NVN_RB_TEXU) continue;
        for (int k = 0; k < (b->n ? b->n : 1); k++) {
            int tid = ubo_tex_id(b->n ? ubo_handle(1, b->reg, b->off + 8 * k) : ubo_bind_handle(1, b));
            if (tid_ser(tid) == ser) { planes |= fb_depth_planes(tid, ser); if (planes == FB_DS_BOTH) return planes; }
        }
    }
    return planes;
}
// Depth sampled in its own pass (docs/reference/rendering.md#depth-sampled-in-the-pass-that-writes-it): Apple's GPUs read undefined compressed blocks and Metal has no tile
// read for depth. A draw writing a plane it samples reads a copy; one sampling a plane an earlier draw wrote ends the pass first.
static uint64_t g_fb_dsnaps, g_fb_dsnap_reuse, g_fb_dsplit;
static int g_fb_depth_m = -1;
static int fb_depth_on(void) {
    if (g_fb_depth_m < 0) {
        const char *e = getenv("DM_NVN_FB_DEPTH");
        g_fb_depth_m = !(e && !strcmp(e, "0"));
        if (!g_fb_depth_m) MLOG("same-pass depth sampling: draws read their depth attachment in the pass (DM_NVN_FB_DEPTH=0)");
    }
    return g_fb_depth_m;
}
// 0 when that cannot be (not a single-level 2D view of it)
static int fb_depth_snap(NvShader *fs, const nvn_rbind *b, int tid, uint64_t ser) {
    if (g_fb_n >= 4 || b->dtt != MTLTextureType2D) return 0;
    uint64_t dser; int lv, sl; rt_sub(&R.rt, 8, &dser, &lv, &sl);
    os_unfair_lock_lock(&g_rmu);
    void *tv = NULL; tex_fast_rid(tid, &tv);
    id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
    os_unfair_lock_unlock(&g_rmu);
    if (!t || t.textureType != MTLTextureType2D || t.sampleCount != 1 || t.mipmapLevelCount != 1) return 0;
    for (int q = 0; q < g_fb_n; q++) if (g_fb_src[q] == t) return 1;
    id<MTLTexture> base = t.parentTexture ? t.parentTexture : t; NSUInteger tl = t.parentTexture ? t.parentRelativeLevel : 0, ts = t.parentTexture ? t.parentRelativeSlice : 0;
    if (base.parentTexture || tl != (NSUInteger)lv || ts != (NSUInteger)sl) return 0;
    NSUInteger w = MAX(base.width >> lv, 1ul), h = MAX(base.height >> lv, 1ul); int fresh;
    fb_cache_t *e = fb_snap(ser, lv, sl, base.pixelFormat, w, h, g_fb_mode == 2, &fresh); if (!e) return 0;
    MTLTextureSwizzleChannels sw = t.swizzle;
    int same = t.pixelFormat == e->snap.pixelFormat && sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
    id<MTLTexture> use = same ? e->snap : [e->snap newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(0, 1) slices:NSMakeRange(0, 1) swizzle:sw];
    if (!use) { e->ser = 0; RED_ONCE(163, "same-pass depth sampling: cannot create the snapshot view (format %lu); left as is", (unsigned long)t.pixelFormat); return 0; }
    if (fresh) {
        if (g_enc) g_fb_split++;
        rp_flush();   // lands a waiting clear too
        enc_b_copy_texture(blit(), base, (NSUInteger)sl, (NSUInteger)lv, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), e->snap, 0, 0, MTLOriginMake(0, 0, 0));
        fb_pass_sync();
    } else g_fb_dsnap_reuse++;
    fb_substitute(t, use);
    if (!fresh) { PD("N %llu same-pass depth sampling: depth ser=%llu level %d layer %d: this draw samples the copy taken since the last barrier", g_stat_pass + !g_enc, (unsigned long long)ser, lv, sl); return 1; }
    if (++g_fb_dsnaps == 1 || g_fb_dsnaps == 10 || g_fb_dsnaps == 1000 || g_fb_dsnaps % 100000 == 0)
        MLOG("same-pass depth sampling: copy #%llu, %llu draws reused one (frame %llu pass %llu, shader %s slot %d samples its depth attachment ser=%llu %lux%lu format %lu while writing it)", (unsigned long long)g_fb_dsnaps,
             (unsigned long long)g_fb_dsnap_reuse, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass + !g_enc, fs->key, b->reg, (unsigned long long)ser, (unsigned long)w, (unsigned long)h, (unsigned long)base.pixelFormat);
    PD("N %llu same-pass depth sampling: depth ser=%llu level %d layer %d copied into a snapshot; this draw samples the snapshot", g_stat_pass + !g_enc, (unsigned long long)ser, lv, sl);
    return 1;
}
// Run before the colour attachments' snapshots and tile reads, which need the pass this decides on.
static void fb_depth_rule(NvShader *fs, uint32_t dsw) {
    uint64_t dser = R.rt.depth->serial; int slot[4], stid[4], ns = 0;
    uint32_t direct = fs->rf.bindless ? fb_texu_reads(fs, dser, R.rt.n > 0) : 0;
    uint32_t slot_planes[4];
    const uint8_t *texs; int ntexs = sh_tex_list(fs, &texs);
    for (int ti = 0; ti < ntexs; ti++) {
        int i = texs[ti]; const nvn_rbind *b = &fs->rf.b[i];
        int image = b->kind == NVN_RB_IMG && !b->rw && !b->atom && b->reg < 32;
        if (!image && (b->kind != NVN_RB_TEX || b->reg >= NTEX)) continue;
        int tid = (int)((image ? bound_img(1, b->reg) : bound_tex(1, b->reg)) & 0xfffff);
        if (!tid || tid_ser(tid) != dser) continue;
        uint32_t planes = image ? FB_DS_BOTH : fb_depth_planes(tid, dser);
        if (!image && ns < 4) { slot[ns] = i; stid[ns] = tid; slot_planes[ns++] = planes; }
        else direct |= planes;
    }
    if (!ns && !direct) return;
    if (!fb_depth_on()) { g_fb_dread |= FB_DS_BOTH; return; }
    for (int q = 0; q < ns; q++) {
        if (!(slot_planes[q] & dsw) || !fb_depth_snap(fs, &fs->rf.b[slot[q]], stid[q], dser)) direct |= slot_planes[q];
    }
    uint32_t earlier = g_enc ? g_fb_dwrote : (R.clear_d ? FB_DS_DEPTH : 0) | (R.clear_s ? FB_DS_STENCIL : 0);
    if (direct & earlier & ~dsw) {
        g_fb_split++; g_fb_dsplit++; rp_flush(); fb_pass_sync();
        if (g_fb_dsplit == 1 || g_fb_dsplit == 1000 || g_fb_dsplit % 100000 == 0)
            MLOG("same-pass depth sampling: pass end #%llu, shader %s samples the depth/stencil plane of ser=%llu written earlier (frame %llu pass %llu)", (unsigned long long)g_fb_dsplit,
                 fs->key, (unsigned long long)dser, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass);
        PD("N %llu same-pass depth sampling: this draw samples a plane of ser=%llu written earlier: the pass ended before it", g_stat_pass + 1, (unsigned long long)dser);
    }
    g_fb_dread |= direct;
}
static void fb_prepare(NvProg *pr) {
    g_draw_seq++; g_fb_n = 0;
    if (g_fb_rebind) { args_dirty_all(); g_fb_rebind = 0; }   // the previous draw had a substituted binding: re-encode with the originals
    if (!pr || (!R.rt.n && !R.rt.depth) || !fb_mode()) return;
    NvShader *fs = pr->sh[1];
    fb_pass_sync();
    uint32_t dsw = 0;
    if (R.rt.depth) {
        if (S.ds.dt && S.ds.dw) dsw |= FB_DS_DEPTH;
        if (S.ds.st && (S.st_wmask[0] || S.st_wmask[1]) && (S.ds.fail_f != NVN_STENCIL_OP_KEEP || S.ds.zfail_f != NVN_STENCIL_OP_KEEP || S.ds.zpass_f != NVN_STENCIL_OP_KEEP
            || S.ds.fail_b != NVN_STENCIL_OP_KEEP || S.ds.zfail_b != NVN_STENCIL_OP_KEEP || S.ds.zpass_b != NVN_STENCIL_OP_KEEP)) dsw |= FB_DS_STENCIL;
    }
    if (!fs) { fb_ds_before_write(dsw); g_fb_dwrote |= dsw; return; }   // vertex-only draws can still write either plane
    uint32_t wm = 0;
    for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k] && ((S.chmask.mask >> (4 * k)) & 0xf) && (fs->rf.color_known ? fs->rf.color_mask >> k & 1 : 1)) wm |= 1u << k;
    if (g_enc && (wm & g_fb_rmask)) { g_fb_split++; rp_end(); fb_pass_sync(); }   // an earlier draw of this pass read an attachment this one writes: it must not see these writes
    fb_ds_before_write(dsw);
    if (R.rt.depth) fb_depth_rule(fs, dsw);
    if (R.rt.n && R.rt.depth && g_fb_dread != FB_DS_BOTH) {
        int pool = fs->rf.pool && pool_ds_read(R.rt.depth);
        if (pool) g_fb_dread_pool = 1;
        if (pool || fs->rf.dyn) g_fb_dread |= FB_DS_BOTH;
    }
    uint64_t tile[4]; int ntile = fb_tile_plan(pr, wm, tile);
    const uint8_t *texs; int ntexs = sh_tex_list(fs, &texs);
    for (int ti = 0; ti < ntexs && g_fb_n < 4; ti++) {
        const nvn_rbind *b = &fs->rf.b[texs[ti]];
        int image = b->kind == NVN_RB_IMG && !b->rw && !b->atom && b->reg < 32;
        // A TEXU handle has no slot, so it never reads the tile, and no level is pinned for it. Arrays and tables of handles are not taken.
        int texu = b->kind == NVN_RB_TEXU && !b->n;
        if (!image && !texu && (b->kind != NVN_RB_TEX || b->reg >= NTEX)) continue;
        uint64_t handle = image ? bound_img(1, b->reg) : texu ? ubo_bind_handle(1, b) : bound_tex(1, b->reg);
        int tid = texu ? ubo_tex_id((uint32_t)handle) : (int)(handle & 0xfffff); uint64_t ser = tid_ser(tid); if (!ser) continue;
        if (R.rt.depth && R.rt.depth->serial == ser) continue;
        for (int k = 0; k < R.rt.n && k < 8 && g_fb_n < 4; k++) {
            if (!R.rt.color[k] || R.rt.color[k]->serial != ser) continue;
            if (!image && !texu && (tile[b->reg >> 6] >> (b->reg & 63) & 1)) { PD("N %llu same-pass feedback sampling: slot %d reads attachment %d ser=%llu from the tile", g_stat_pass + !g_enc, b->reg, k, (unsigned long long)ser); continue; }
            uint64_t aser; int lv, sl; rt_sub(&R.rt, k, &aser, &lv, &sl);
            os_unfair_lock_lock(&g_rmu);
            id<MTLTexture> t;
            if (image) {
                int real; uint64_t rid;
                t = img_binding(tid, b, &rid, &real);
                if (!real || !fb_image_view(tid, b, t)) t = nil;
            } else {
                void *tv = NULL; tex_fast_rid(tid, &tv); t = (__bridge id<MTLTexture>)tv;
            }
            os_unfair_lock_unlock(&g_rmu);
            if (!t) continue;
            int dup = 0;
            for (int q = 0; q < g_fb_n; q++) dup |= g_fb_src[q] == t;
            if (dup) continue;
            id<MTLTexture> base = t.parentTexture ? t.parentTexture : t; NSUInteger tl = t.parentTexture ? t.parentRelativeLevel : 0, ts = t.parentTexture ? t.parentRelativeSlice : 0;
            if (tl + t.mipmapLevelCount <= (NSUInteger)lv || tl > (NSUInteger)lv) continue;   // the sampled levels exclude the attachment's level (mip generation): not feedback
            NSUInteger tn = t.textureType == MTLTextureType2DArray || t.textureType == MTLTextureTypeCubeArray || t.textureType == MTLTextureType2DMultisampleArray ? t.arrayLength * (t.textureType == MTLTextureTypeCubeArray ? 6 : 1) : t.textureType == MTLTextureTypeCube ? 6 : 1;
            if (ts + tn <= (NSUInteger)sl || ts > (NSUInteger)sl) continue;
            // The attachment's level holds no defined data while its pass renders it, and a linear mip filter fetches the level above an integer LOD even at weight 0
            // (docs/reference/rendering.md#mip-chain-pass-sampling-the-level-it-renders): a view of other levels where it suffices, else a snapshot of the whole view.
            int n = (int)t.mipmapLevelCount, a = lv - (int)tl, lo = 0, hs = n - 1, hi = n - 1;
            int known = !image && !texu && fb_reach(fs, b->reg, (int)(handle >> 20), n, &lo, &hs, &hi);
            if (g_fb_mode == 1 && !((wm | g_fb_wmask) >> k & 1)) { g_fb_rmask |= 1u << k; continue; }
            if (n != 1 && known && lo == hs && lo != a) {
                id<MTLTexture> use = [base newTextureViewWithPixelFormat:t.pixelFormat textureType:t.textureType levels:NSMakeRange(tl + (NSUInteger)lo, 1) slices:NSMakeRange(ts, tn) swizzle:t.swizzle];
                if (!use) { RED_ONCE(143, "same-pass feedback sampling: cannot make the view of level %lu that the sampler pins (format %lu); left as is", (unsigned long)(tl + (NSUInteger)lo), (unsigned long)t.pixelFormat); continue; }
                fb_substitute(t, use);
                static int once; if (!once++) MLOG("same-pass feedback sampling: shader %s renders level %d of a view of levels %lu+%lu; its sampler pins level %lu, which it samples alone", fs->key, lv, (unsigned long)tl, (unsigned long)t.mipmapLevelCount, (unsigned long)(tl + (NSUInteger)lo));
                continue;
            }
            if (known && lo > a) continue;
            if (a > 0 && (!known || lo < a)) {
                id<MTLTexture> use = [base newTextureViewWithPixelFormat:t.pixelFormat textureType:t.textureType levels:NSMakeRange(tl, (NSUInteger)a) slices:NSMakeRange(ts, tn) swizzle:t.swizzle];
                if (!use) { RED_ONCE(82, "same-pass feedback sampling: cannot make the view of levels %lu+%d below the attachment (format %lu); left as is", (unsigned long)tl, a, (unsigned long)t.pixelFormat); continue; }
                fb_substitute(t, use);
                static int once; if (!once++) MLOG("same-pass feedback sampling: shader %s renders level %d of a view of levels %lu+%lu; it samples a view of the levels below (mip-chain pass)", fs->key, lv, (unsigned long)tl, (unsigned long)t.mipmapLevelCount);
                continue;
            }
            // the host holds cube maps as 2D arrays: the draw samples a view in the type the shader declares
            int single = n == 1 && tn == 1, ty = (int)t.textureType, vt = b->dtt;
            int fits = vt == ty || (ty == MTLTextureType2DArray && ((vt == MTLTextureTypeCube && tn == 6) || (vt == MTLTextureTypeCubeArray && tn % 6 == 0)))
                || ((ty == MTLTextureTypeCube || ty == MTLTextureTypeCubeArray) && vt == MTLTextureType2DArray);
            if (t.sampleCount != 1 || base.parentTexture || !fits || (single ? ty != MTLTextureType2D :
                    ty != MTLTextureType2D && ty != MTLTextureType2DArray && ty != MTLTextureTypeCube && ty != MTLTextureTypeCubeArray)) {
                RED_ONCE(80, "same-pass feedback sampling: a sampled view the snapshot cannot stand in for (type %lu as %u, levels %lu+%d, layers %lu+%lu, attachment level %d layer %d, shader %s); left as is",
                         (unsigned long)t.textureType, (unsigned)b->dtt, (unsigned long)tl, n, (unsigned long)ts, (unsigned long)tn, lv, sl, fs->key);
                continue; }
            if (!known) { lo = 0; hi = n - 1; }
            uint32_t need = (uint32_t)(((2ull << hi) - 1) & ~((1ull << lo) - 1));
            fb_shape_t shp = {base.pixelFormat, t.textureType, MAX(base.width >> tl, 1ul), MAX(base.height >> tl, 1ul), single ? 1 : t.arrayLength, (NSUInteger)n};
            NSUInteger w = shp.w, h = shp.h; int fresh;
            fb_cache_t *e = fb_snap_shape(ser, (int)tl, (int)ts, &shp, g_fb_mode == 2, &fresh); if (!e) continue;
            id<MTLTexture> snap = e->snap;
            uint32_t miss = need & ~e->lvm;   // a snapshot taken since the last barrier lacks levels an earlier draw did not reach
            if (miss) {
                rp_flush();   // lands a clear still waiting to be its load action: the snapshot holds the cleared content
                id<MTL4ComputeCommandEncoder> be = blit();
                for (int m = 0; m < n; m++) {
                    if (!(miss >> m & 1)) continue;
                    for (NSUInteger q = 0; q < tn; q++)
                        enc_b_copy_texture(be, base, ts + q, tl + (NSUInteger)m, MTLOriginMake(0, 0, 0), MTLSizeMake(MAX(w >> m, 1ul), MAX(h >> m, 1ul), 1), snap, q, (NSUInteger)m, MTLOriginMake(0, 0, 0));
                }
                e->lvm |= miss;
            } else if (g_enc && (g_fb_rmask >> k & 1)) rp_end();   // an earlier draw of this pass read the attachment directly: it must not see this pass's writes
            if (!g_enc) fb_pass_sync();
            MTLTextureSwizzleChannels sw = t.swizzle;
            int same = t.pixelFormat == snap.pixelFormat && (int)snap.textureType == vt && sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
            id<MTLTexture> use = same ? snap : [snap newTextureViewWithPixelFormat:t.pixelFormat textureType:(MTLTextureType)vt levels:NSMakeRange(0, (NSUInteger)n) slices:NSMakeRange(0, tn) swizzle:sw];
            if (!use) { RED_ONCE(81, "same-pass feedback sampling: cannot create the snapshot view (format %lu); left as is", (unsigned long)t.pixelFormat); continue; }
            fb_substitute(t, use);
            if (!miss) { g_fb_reuse++; PD("N %llu same-pass feedback sampling: attachment %d ser=%llu level %d layer %d: this draw samples the snapshot taken since the last barrier", g_stat_pass + !g_enc, k, (unsigned long long)ser, lv, sl); continue; }
            if (!single) { static int once; if (!once++) MLOG("same-pass feedback sampling: shader %s renders level %d layer %d of a view of levels %lu+%d, layers %lu+%lu; it samples a snapshot of the view (levels %d..%d copied)",
                                                            fs->key, lv, sl, (unsigned long)tl, n, (unsigned long)ts, (unsigned long)tn, lo, hi); }
            g_fb_count++;
            if (g_fb_count == 1 || g_fb_count == 10 || g_fb_count == 1000 || g_fb_count % 100000 == 0)
                MLOG("same-pass feedback sampling: snapshot #%llu, %llu draws reused one, %llu read the tile (%llu waited for their pipeline), %llu passes ended before a write to an attachment read directly (frame %llu pass %llu, shader %s %s %d samples attachment %d ser=%llu %lux%lu format %lu)", (unsigned long long)g_fb_count, (unsigned long long)g_fb_reuse, (unsigned long long)g_fb_tile_draws, (unsigned long long)g_fb_tile_wait, (unsigned long long)g_fb_split,
                     (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass + !g_enc, fs->key, texu ? "handle in uniform slot" : image ? "image slot" : "slot", b->reg, k, (unsigned long long)ser, (unsigned long)w, (unsigned long)h, (unsigned long)base.pixelFormat);
            PD("N %llu same-pass feedback sampling: attachment %d ser=%llu level %d layer %d copied into a snapshot; this draw samples the snapshot", g_stat_pass + !g_enc, k, (unsigned long long)ser, lv, sl);
        }
    }
    g_fb_wmask |= wm; g_fb_dwrote |= dsw;
    if (g_fb_n) { g_fb_seq = g_draw_seq; args_dirty_all(); g_fb_rebind = 1; }
    if (ntile) {
        g_fb_tseq = g_draw_seq; g_fb_tile_draws++;
        if (g_fb_tile_draws == 1 || g_fb_tile_draws == 1000 || g_fb_tile_draws % 100000 == 0)
            MLOG("same-pass feedback sampling: draw #%llu reads its attachments from the tile, %llu snapshots so far (frame %llu, shader %s, attachments %#x)", (unsigned long long)g_fb_tile_draws, (unsigned long long)g_fb_count,
                 (unsigned long long)atomic_load(&g_cur_frame), fs->key, g_fb_tdesc.fbmask);
    }
}
