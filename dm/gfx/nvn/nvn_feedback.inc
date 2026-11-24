// A draw that samples its own attachment: snapshots, reads from the tile, the depth attachment sampled in its own pass (fb_prepare).
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// Same-pass feedback sampling: a texture view this draw's fragment stage samples includes the level and layer of a color attachment of the current render pass, and the pass writes that attachment
// (agl's post-effect program in Super Mario 3D World + Bowser's Fury, Captain Toad and Super Mario Odyssey writes the tone-mapped RGB10A2 image as an attachment while sampling it). On the Switch the read
// returns the content from before this draw; on Metal, writing and sampling the same attachment within one render pass is undefined: the M4 Pro happens to read correctly, while the A20 Pro and the Apple M6
// now and then read whole 16x16 blocks as (1,0,1), (0,0,0) or (0,1,0) (blocks of the compressed attachment caught while another tile stores them).
// The fix: before such a draw opens its render pass, end the current render pass (the attachment lands in memory), copy that level and layer into a snapshot texture in the replay stream, make this draw
// sample the snapshot; later draws reopen the render pass as usual (loading the attachment). A view over several levels or layers (mip-chain and layer passes) gets what the levels its sampler
// reads need (fb_prepare): a view without the attachment's level, or a snapshot of the whole view; other shapes are reported once.
// Which draws: those that write the sampled attachment (channel mask and fragment output) or follow a draw of the same Metal pass that wrote it; a draw that samples an attachment nothing in its pass has
// written reads it directly, and a later draw of that pass that writes it starts a new pass (on a tiled GPU the earlier reads in other tiles could see these writes). Which snapshot: NVN makes rendering
// visible to texture reads only through nvnCommandBufferBarrier, so a snapshot taken since the game's last barrier (and in this submission) holds everything the game may expect to read; later such
// draws reuse it without ending the pass, unless the pass also read that attachment directly. Breath of the Wild's G-buffer decals write and sample their own targets, about 460 draws a frame outdoors:
// a fresh snapshot for each took the Mac's GPU time from about 4 to 28 ms a frame, reuse until a barrier needs about ten a frame (GPU time within the noise of none).
// A game barrier between such draws makes the next one take a fresh snapshot, and Tears of the Kingdom's shrines set one before each decal (0x12, about 263 a frame): about 284
// snapshots a frame, each a pass end and full-size copies (GitHub #737). A draw that samples the attachment at its own pixel reads the tile instead (fb_tile_plan): no pass end, no copy.
// DM_NVN_FB_SNAP=0 none, all a fresh snapshot for every draw sampling an attachment of its pass (diagnostic); DM_NVN_FB_TILE=0 no tile reads
static int g_fb_mode = -1;   // 0 none, 1 writers with reuse until a barrier, 2 every sampling draw, fresh
static uint64_t g_fb_bar, g_fb_reuse, g_fb_split;   // game barriers so far (OP_BARRIER with bits); draws that reused a snapshot; passes ended before a write to an attachment an earlier draw read
static uint64_t g_fb_pass = UINT64_MAX; static uint32_t g_fb_wmask, g_fb_rmask, g_fb_dread, g_fb_dwrote;   // color slots written and read directly; depth/stencil planes read and written
typedef struct { uint64_t ser, bar, sub, use, draw; int lv, sl; uint32_t lvm; __strong id<MTLTexture> snap; } fb_cache_t;   // use: when it was last taken or reused (g_fbc_use); draw: the draw it was last handed to (g_draw_seq); lvm: the levels copied into it (fb_prepare's range snapshots)
static fb_cache_t g_fbc[8]; static uint64_t g_fbc_use;
static int fb_mode(void) {
    if (g_fb_mode < 0) { const char *e = getenv("DM_NVN_FB_SNAP"); g_fb_mode = e && !strcmp(e, "0") ? 0 : e && !strcmp(e, "all") ? 2 : 1;
        if (g_fb_mode != 1) MLOG("same-pass feedback sampling snapshot: %s (DM_NVN_FB_SNAP)", g_fb_mode ? "a fresh one for every draw sampling an attachment of its pass" : "off"); }
    return g_fb_mode;
}
static inline void fb_pass_sync(void) { uint64_t p = g_stat_pass + !g_enc; if (p != g_fb_pass) { g_fb_pass = p; g_fb_wmask = g_fb_rmask = g_fb_dread = g_fb_dwrote = 0; g_fb_dread_pool = 0; } }
static void fb_ds_note_write(uint32_t planes) { fb_pass_sync(); g_fb_dwrote |= planes; }
static void fb_ds_before_write(uint32_t planes) {
    if (g_enc && (planes & g_fb_dread)) {
        g_fb_split++; g_fb_split_pool += g_fb_dread_pool; rp_end(); fb_pass_sync();
    }
}
// A snapshot's shape: a single-level 2D texture for a view of the attachment's level alone, else the sampled view's type, size of its base level, layers and levels (fb_prepare)
typedef struct { MTLPixelFormat pf; MTLTextureType type; NSUInteger w, h, arr, nl; } fb_shape_t;
static inline int fb_shape(const fb_cache_t *e, const fb_shape_t *k) {
    return e->snap && e->snap.pixelFormat == k->pf && e->snap.textureType == k->type && e->snap.width == k->w && e->snap.height == k->h && e->snap.arrayLength == k->arr && e->snap.mipmapLevelCount == k->nl;
}
static inline int fb_live(const fb_cache_t *e) { return e->ser && e->bar == g_fb_bar && e->sub == g_sub_serial; }   // a later draw may still reuse it
// The snapshot of an attachment's level and layer: the cached one (valid: *fresh = 0) or a texture to copy into (*fresh = 1; the entry is then valid from this barrier and submission on).
// A new key takes an entry whose texture already has its format and size, one no later draw can reuse first, else the least recently used. Taking the entries in turn made a texture for
// nearly every snapshot when a frame snapshots several sizes: Tears of the Kingdom's 4-5 a frame (1600x896 down to 400x224) each made one, 4-6 GB of textures every 300 frames on the Mac
// and a residency commit for each texture added and each removed (GitHub #931). The callers copy after rp_flush, not only after the open pass ends: a clear with no draw after it waits
// to be the load action of the pass this draw opens (R.clear_c / clear_d), so a copy taken before it held what the attachment had before the clear, which a game sees only up to its clear
// (a clear, a barrier, then a draw that samples and writes the target; from issue #1932's patch).
// An entry handed to the current draw is never taken for another key of that draw: its snapshot is bound already, and a copy into it would replace what the earlier binding reads. With
// every entry live, a draw sampling two attachments of one format and size found the first one's entry the only one of that shape and copied the second over it (PR #813). A draw binds
// at most four snapshots (g_fb_n), so four entries are left unless snapshot views fail to be made (the red line below)
static fb_cache_t *fb_snap_shape(uint64_t ser, int lv, int sl, const fb_shape_t *k, int force, int *fresh) {
    fb_cache_t *e = NULL;
    for (int i = 0; i < 8 && !e; i++) if (g_fbc[i].snap && g_fbc[i].ser == ser && g_fbc[i].lv == lv && g_fbc[i].sl == sl) e = &g_fbc[i];
    if (e && !fb_shape(e, k)) { e->ser = 0; e = NULL; }   // the attachment was made again in another format or size, or the view's shape changed: its snapshot is no longer this key's
    if (e && !force && fb_live(e)) { e->use = ++g_fbc_use; e->draw = g_draw_seq; *fresh = 0; return e; }
    if (!e) {   // by rank: same shape and not live, not live, same shape (every entry live), least recently used within a rank; none this draw holds
        int best = 1 << 30;
        for (int i = 0; i < 8; i++) { fb_cache_t *c = &g_fbc[i]; if (c->draw && c->draw == g_draw_seq) continue;
            int r = fb_live(c) ? (fb_shape(c, k) ? 2 : 3) : fb_shape(c, k) ? 0 : 1;
            if (r < best || (r == best && c->use < e->use)) { best = r; e = c; } }
        if (!e) { RED_ONCE(170, "same-pass feedback sampling: every snapshot entry is held by this draw; the attachment is read as it is"); return NULL; }
        if (e->snap && !fb_shape(e, k)) { defer_obj_release(e->snap); e->snap = nil; }
    }
    if (!e->snap) { MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = k->type; d.pixelFormat = k->pf; d.width = k->w; d.height = k->h; d.arrayLength = k->arr; d.mipmapLevelCount = k->nl;
        d.usage = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView; d.storageMode = MTLStorageModePrivate;
        e->snap = rh_place(d); if (!e->snap) e->snap = [g_dev newTextureWithDescriptor:d];   // recycled memory: a resized attachment's snapshot takes the heap of an earlier size's
        if (!e->snap) { e->ser = 0; return NULL; } res_add(e->snap); }   // full queue barriers separate a copy from the draws that read the earlier content
    e->ser = ser; e->lv = lv; e->sl = sl; e->bar = g_fb_bar; e->sub = g_sub_serial; e->use = ++g_fbc_use; e->draw = g_draw_seq; e->lvm = 0; *fresh = 1; return e;
}
// The attachment's level and layer alone, which the caller copies when it is fresh
static fb_cache_t *fb_snap(uint64_t ser, int lv, int sl, MTLPixelFormat pf, NSUInteger w, NSUInteger h, int force, int *fresh) {
    fb_shape_t k = {pf, MTLTextureType2D, w, h, 1, 1}; fb_cache_t *e = fb_snap_shape(ser, lv, sl, &k, force, fresh);
    if (e && *fresh) e->lvm = 1;
    return e;
}
// ---- Same-pass feedback read from the tile ----
// A draw whose every sample of an attachment of its open pass is a projective sample at its vertex shader's clip → screen varying (scr_proof), or a sample at its pixel position times
// 1 / the target's size (fpos_proof), reads this pixel of the attachment:
// on Apple's GPUs the readable-tile version (<key>_bf, dmsh_fetch.py) reads it from the tile ([[color(k)]]), which holds every earlier fragment of the pass in order. That is what NVN
// guarantees such a draw after a barrier (and one of the values it leaves undefined without one), so the draw needs no snapshot and the pass goes on. Breath of the Wild's and Tears
// of the Kingdom's G-buffer decals (4d3d5a4408c2e8d9, 7d1b1cac266f08f2) sample their targets so, and Splatoon Raiders' ink stamps their ink map (6a5b76a60670e513, a pass and a full
// 80 MB snapshot each before, GitHub #1063). Only in an open pass (its formats name the pipeline) or the pass the draw opens once its formats are learned (fb_early_after), only a single-level 2D view
// that is the attachment's level and layer in its own format with no channel mapping beyond filling missing channels (sw_tile), and not with the alpha test, a logic operation, multisampling, or a geometry or slice-merged draw. The pipeline is built in the background on first use, as sm_amp_get does: until it is there the draw
// takes a snapshot, so no frame waits for the compiler. lmap is the identity over the shader's outputs (an attachment it does not write keeps its content, as in pso_desc)
static int fb_tile_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_FB_TILE"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("same-pass feedback sampling: no tile reads (DM_NVN_FB_TILE=0)"); } return v; }
static int mtk_drawing(void);   // a task / mesh draw (nvn_mesh.inc), whose pipeline is its own
static uint64_t g_fb_tile_draws, g_fb_tile_wait, g_fb_tseq; static id<MTLFunction> g_fb_tfn; static uint8_t g_fb_tlmap[8]; static pso_fz_desc g_fb_tdesc;
enum { FB_TP_MAX = 256 };
static struct { pso_key k; id<MTLRenderPipelineState> ps; int done; } g_fb_tp[FB_TP_MAX]; static int g_fb_tp_n; static os_unfair_lock g_fb_tp_lock = OS_UNFAIR_LOCK_INIT;
// A draw that opens its pass (no pass open at fb_tile_plan) can read the tile too: the pass loads the attachment, or clears it to what a waiting clear gave it, so the tile holds what a
// snapshot would. Its pipeline is named by the formats of a pass that is not open yet, so they are learned: the first such draw of a program on a set of targets that samples an
// attachment it writes takes its snapshot and notes the pass rp_begin_ex opened for it (fb_early_after); later ones plan with those formats and are checked against the pass that
// really opened. One that differs (another depth / stencil mode, an added attachment) ends that pass, takes the snapshot and opens it again, and after three of those the entry plans
// no more. Breath of the Wild opened about 3.75 passes a frame with such a draw, each a full-size copy (0.45 ms of a 6 ms frame on an M4 Max, 8% of the submit thread's
// instructions). DM_NVN_FB_TILE_EARLY=0: those draws take snapshots, as before
typedef struct { uint32_t cfmt[8], dfmt, sfmt, samples, tw, th; } fb_pinfo;
static struct { uint64_t key; fb_pinfo pi; uint8_t ok, fails; } g_fb_el[64];
static uint64_t g_fb_early_seq, g_fb_learn_seq, g_fb_early_key, g_fb_early_n, g_fb_early_miss; static int g_fb_noearly;
static int fb_early_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_FB_TILE_EARLY"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("same-pass feedback sampling: a draw that opens its pass takes a snapshot (DM_NVN_FB_TILE_EARLY=0)"); } return v; }
static uint64_t fb_early_keyof(NvProg *pr) {
    uint64_t h = 0x9e3779b97f4a7c15ull ^ (uint64_t)(uintptr_t)pr->sh[0];
#define FB_MIX(v) (h = (h ^ (uint64_t)(v)) * 0xff51afd7ed558ccdull, h ^= h >> 29)
    FB_MIX((uintptr_t)pr->sh[1]); FB_MIX(R.rt.n);
    for (int k = 0; k < R.rt.n && k < 8; k++) { FB_MIX(R.rt.color[k] ? R.rt.color[k]->serial : 0); FB_MIX(R.rt.color[k] && R.rt.has_cview[k] && R.rt.cview[k].set_levels ? R.rt.cview[k].base_level + 1 : 0); }
    FB_MIX(R.rt.depth ? R.rt.depth->serial : 0);
#undef FB_MIX
    return h ? h : 1;
}
static void fb_pinfo_now(fb_pinfo *pi) { memset(pi, 0, sizeof *pi); memcpy(pi->cfmt, g_pass.cfmt, sizeof pi->cfmt); pi->dfmt = g_pass.dfmt; pi->sfmt = g_pass.sfmt; pi->samples = g_pass.samples; pi->tw = (uint32_t)g_pass.tw; pi->th = (uint32_t)g_pass.th; }
// After rp_begin_ex opened the pass of a draw fb_tile_plan met with none open. 0: the draw planned a tile read for a pass that is not this one (the caller goes back to a snapshot)
static int fb_early_after(void) {
    if (g_fb_learn_seq == g_draw_seq) { g_fb_learn_seq = 0; __typeof__(g_fb_el[0]) *e = &g_fb_el[g_fb_early_key & 63];
        if (e->key != g_fb_early_key) { e->key = g_fb_early_key; e->fails = 0; } fb_pinfo_now(&e->pi); e->ok = !g_sm_pass; return 1; }
    if (g_fb_early_seq != g_draw_seq) return 1;
    g_fb_early_seq = 0; __typeof__(g_fb_el[0]) *e = &g_fb_el[g_fb_early_key & 63]; fb_pinfo now; fb_pinfo_now(&now);
    if (e->key == g_fb_early_key && !g_sm_pass && !memcmp(&now, &e->pi, sizeof now)) { g_fb_early_n++; return 1; }   // NOLINT(bugprone-suspicious-memory-comparison): fb_pinfo_now zeroes the whole struct first
    if (e->key == g_fb_early_key) { e->pi = now; if (e->fails < 255) e->fails++; }
    g_fb_early_miss++;
    static int once; if (!once++) MLOG("same-pass feedback sampling: a draw planned a tile read for the pass it opens, and another pass opened (formats, size or a slice-merged pass); it takes a snapshot (first case pass %llu)", g_stat_pass);
    return 0;
}
static int fb_tile_ready(NvShader *vs, NvShader *fs, id<MTLFunction> fn, const uint8_t lmap[8], const pso_fz_desc *fz, const fb_pinfo *pi) {
    const uint8_t *l0 = g_pso_lmap; g_pso_lmap = lmap;   // the key pso_get makes for this draw (atest_key and lop_key read the mapping)
    pso_key k; pso_key_fill(&k, vs, fs, pi->cfmt, pi->dfmt, pi->sfmt, pi->samples, 1); g_pso_lmap = l0;
    memcpy(k.lmap, lmap, 8); k.fsfn = (__bridge void *)fn;
    if (hm_get_id(&g_pso, &k)) return 1;
    os_unfair_lock_lock(&g_fb_tp_lock); int i = 0; for (; i < g_fb_tp_n; i++) if (!memcmp(&g_fb_tp[i].k, &k, sizeof k)) break;   // NOLINT(bugprone-suspicious-memory-comparison): pso_key_fill zeroes the whole key first, padding included (as for sm_amp_get's table)
    if (i < g_fb_tp_n) { id ps = g_fb_tp[i].done ? g_fb_tp[i].ps : nil; os_unfair_lock_unlock(&g_fb_tp_lock);
        if (!ps) return 0;   // building, or failed (its snapshot stays)
        hm_put_id(&g_pso, &k, ps);
        pso_skey sk; char shex[sizeof sk * 2 + 1]; pso_skey_from(&k, vs, fs, 1, fz, &sk); pso_skey_hex(&sk, shex); pso_record("R\t%s\n", shex); pso_user_record("R\t%s\n", shex);   // prewarmed next time (kind 1: bf_fn)
        return 1; }
    if (g_fb_tp_n >= FB_TP_MAX) { os_unfair_lock_unlock(&g_fb_tp_lock); static int once; if (!once++) MLOG("same-pass feedback sampling: %d tile-reading pipelines built; later ones keep the snapshot", FB_TP_MAX); return 0; }
    g_fb_tp[i].k = k; g_fb_tp[i].ps = nil; g_fb_tp[i].done = 0; g_fb_tp_n++; os_unfair_lock_unlock(&g_fb_tp_lock);
    MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, fn, 1); int off = pso_off(&k, fs, 1);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(d, off, &e);
        if (!ps) MLOG("[RED] same-pass feedback sampling: no tile-reading pipeline for vertex %s fragment %s: %s (its draws keep the snapshot)", vs->key, fs->key, e.localizedDescription.UTF8String);
        os_unfair_lock_lock(&g_fb_tp_lock); g_fb_tp[i].ps = ps; g_fb_tp[i].done = 1; os_unfair_lock_unlock(&g_fb_tp_lock); });
    return 0;
}
// The texture slots (bit per slot 0-255) whose attachment this draw reads from the tile; 0 = none (then g_fb_tfn and the rest are not this draw's)
static int fb_tile_plan(NvProg *pr, uint32_t wm, uint64_t tile[4]) {
    NvShader *vs = pr->sh[0], *fs = pr->sh[1]; memset(tile, 0, 4 * sizeof *tile);
    if (g_fb_mode != 1 || !vs || !(fs->rf.fc_proj | fs->rf.fc_fpos) || !fs->rf.color_known || !fs->rf.nfo || !fb_tile_on()) return 0;
    fb_pinfo pi; int learn = 0;
    if (g_enc) { fb_pinfo_now(&pi); if (g_sm_pass) return 0; }
    else {   // the draw opens its pass: the formats learned for this program on these targets (fb_early_after)
        if (!fb_early_on() || g_fb_noearly) return 0;
        uint64_t key = fb_early_keyof(pr); __typeof__(g_fb_el[0]) *e = &g_fb_el[key & 63]; g_fb_early_key = key;
        if (e->key != key) { learn = 1; memset(&pi, 0, sizeof pi); }   // not learned yet: noted below, once the draw is seen to sample an attachment it writes
        else if (!e->ok || e->fails >= 3) return 0;
        else pi = e->pi;
    }
    if (atest_on() || lop_on() || pi.samples > 1 || S.rast_discard || mtk_drawing() || ms_fs_override(fs)) return 0;
    NSUInteger W = pi.tw, H = pi.th;
    int fx[32]; for (int i = 0; i < 32; i++) fx[i] = -1; uint8_t fbmask = 0;
    for (int i = 0; i < fs->rf.nb; i++) { const nvn_rbind *b = &fs->rf.b[i]; if (b->kind != NVN_RB_TEX || b->reg >= NTEX) continue;
        int tid = (int)(bound_tex(1, b->reg) & 0xfffff); uint64_t ser = tid_ser(tid); if (!ser) continue;
        int att = 0; for (int k = 0; k < R.rt.n && k < 8; k++) att |= R.rt.color[k] && R.rt.color[k]->serial == ser && ((wm | g_fb_wmask) >> k & 1);
        if (!att) continue;   // the texture only when it is an attachment this pass wrote, under g_rmu, and still the ID's
        os_unfair_lock_lock(&g_rmu); void *tv = NULL; if (tid < g_ntreg && g_treg[tid].ok && g_treg[tid].t.serial == ser) tex_fast_rid(tid, &tv); id<MTLTexture> t = (__bridge id<MTLTexture>)tv; os_unfair_lock_unlock(&g_rmu);
        if (!t) continue;
        for (int k = 0; k < R.rt.n && k < 8; k++) { if (!R.rt.color[k] || R.rt.color[k]->serial != ser || !((wm | g_fb_wmask) >> k & 1)) continue;   // nothing in this pass wrote it: read directly, as before
            if (learn) { g_fb_learn_seq = g_draw_seq; return 0; }   // its snapshot this time; fb_early_after notes the pass that opens (most draws that open a pass sample no attachment of it, and learn nothing)
            uint64_t aser; int lv, sl; rt_sub(&R.rt, k, &aser, &lv, &sl);
            id<MTLTexture> base = t.parentTexture ? t.parentTexture : t;
            if (t.textureType != MTLTextureType2D || t.mipmapLevelCount != 1 || t.sampleCount != 1 || base.parentTexture || (t.parentTexture ? t.parentRelativeLevel : 0) != (NSUInteger)lv
                || (t.parentTexture ? t.parentRelativeSlice : 0) != (NSUInteger)sl || t.pixelFormat != (MTLPixelFormat)pi.cfmt[k] || b->dtt != MTLTextureType2D
                || !sw_tile(t)
                || MAX(base.width >> lv, 1ul) != W || MAX(base.height >> lv, 1ul) != H || (fs->rf.fc_mixed[b->reg >> 6] >> (b->reg & 63) & 1)) break;
            int any = 0, ok = 1;
            for (int c = 0; c < fs->rf.nfc; c++) if (fs->rf.fc_reg[c] == b->reg) { if (fs->rf.fc_fpos >> c & 1 ? fpos_proof(fs, c, W, H) : fs->rf.fc_proj >> c & 1 && scr_proof(pr, fs->rf.fc_locn[c], W, H)) any = 1; else ok = 0; }
            if (!ok || !any) break;
            for (int c = 0; c < fs->rf.nfc; c++) if (fs->rf.fc_reg[c] == b->reg) fx[c] = k;
            fbmask |= (uint8_t)(1u << k); tile[b->reg >> 6] |= 1ull << (b->reg & 63);
            break; } }
    if (!fbmask) return 0;
    uint8_t lmap[8]; memset(lmap, 0xff, 8); for (int q = 0; q < 8; q++) if (fs->rf.color_mask >> q & 1) lmap[q] = (uint8_t)q;
    pso_fz_desc d; memset(&d, 0, sizeof d); d.kind = 1; d.fbmask = fbmask; for (int i = 0; i < 32; i++) d.fx[i] = (int8_t)fx[i];
    id<MTLFunction> fn = bf_fn(fs, fx, fbmask, lmap);
    if (!fn || !fb_tile_ready(vs, fs, fn, lmap, &d, &pi)) { g_fb_tile_wait += fn != nil; memset(tile, 0, 4 * sizeof *tile); return 0; }
    g_fb_tfn = fn; memcpy(g_fb_tlmap, lmap, 8); g_fb_tdesc = d;
    if (!g_enc) g_fb_early_seq = g_draw_seq;
    return 1;
}
// The levels a draw's sampler lets it fetch through texture slot reg (combined handle's sampler sid), counted from the view's base and capped at its last level n - 1: *lo to *hs carry
// weight, *hi also a level a linear mip filter fetches at weight 0 (the one above an integer LOD). 0 when unknown: only a combined handle's sampler (the shader's SAMPT binding of the
// same slot) is known here. The sampler's minimum and maximum LOD clamp whatever LOD the shader asks for (an explicit one too); without a mip filter it reads the base level. A texel
// fetch names its level without the sampler: the slot's reads are taken as samples
static int fb_reach(const NvShader *fs, unsigned reg, int sid, int n, int *lo, int *hs, int *hi) {
    int combined = 0; for (int j = 0; j < fs->rf.nb && !combined; j++) combined = fs->rf.b[j].kind == NVN_RB_SAMPT && fs->rf.b[j].reg == reg;
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
void mtl_fb_stats(uint64_t *snaps, uint64_t *reused, uint64_t *tile, uint64_t *wait) { *snaps = g_fb_count; *reused = g_fb_reuse; *tile = g_fb_tile_draws; *wait = g_fb_tile_wait; }   // the power line's feedback counts (hle_nvn.c)
// Look at the view encode_args will bind. Unknown views and images must retain both plane dependencies.
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
// Which planes the fragment shader's uniform handles (TEXU) read: resolve as encode_args reads them. Fire Emblem Fortune's Weave's
// decals are bindless and write depth; taking every bindless shader as a depth reader ended the pass before each of them (about 30 passes a frame, each loading and storing four targets).
// texa: whether a table of handles in c2 (TEXA), not read here, counts as naming it
static uint32_t fb_texu_reads(NvShader *fs, uint64_t ser, int texa) {
    uint32_t planes = 0;
    for (int i = 0; i < fs->rf.nb; i++) { const nvn_rbind *b = &fs->rf.b[i]; if (b->kind == NVN_RB_TEXA) { if (texa) return FB_DS_BOTH; continue; }
        if (b->kind == NVN_RB_TEXH) { __unsafe_unretained id<MTLBuffer> pb = nil; int hit = 0;   // a table of a uniform buffer's handles: its serials
            os_unfair_lock_lock(&g_rmu); htab_gpu(1, b, &pb); for (uint32_t k = 0; k < g_htab_last->n && !hit; k++) hit = g_htab_last->ser[k] == ser; os_unfair_lock_unlock(&g_rmu);
            if (hit) return FB_DS_BOTH; continue; }
        if (b->kind != NVN_RB_TEXU) continue;
        for (int k = 0; k < (b->n ? b->n : 1); k++) {
            int tid = ubo_tex_id(b->n ? ubo_handle(1, b->reg, b->off + 8 * k) : ubo_bind_handle(1, b));
            if (tid_ser(tid) == ser) { planes |= fb_depth_planes(tid, ser); if (planes == FB_DS_BOTH) return planes; } } }
    return planes;
}
// ---- The depth attachment sampled in its own pass ----
// On the console a draw that samples the depth attachment reads what is stored when it starts; on Apple's GPUs a depth attachment sampled in the pass that writes it reads whole
// compressed blocks of undefined content, and Metal has no tile read for depth. So, as for colour (the snapshot rule above), by what this pass and this draw write:
// - a draw that writes a plane it samples through a texture slot reads a copy, taken after the pass ends and reused until the game's next barrier
//   (NVN makes rendering visible to texture reads only through one). Ocarina of Time's cave and cliff fixtures (25a8adc977ff813d, a full-screen triangle that samples the scene depth
//   and writes it with LEQUAL, then its decals) painted black and sky-blue blocks over the rock; at Death Mountain Trail's cave entrance 1,000 such draws took about 190 copies, with
//   the same picture as a copy for each (PR #1917);
// - a draw that samples a plane an earlier draw or clear wrote, and does not write it itself, ends the pass first and reads the stored plane. Red Dead Redemption's
//   cloud fog (c0ab3c051753e36d) samples it through a uniform-buffer handle after the billboards and terrain of its pass: 8x8 blocks of bare sky over the trees (PR #1866);
// - a draw that samples it first in its pass reads it directly, and a later draw that writes the same plane ends the pass (g_fb_dread).
// Reads it can name (a texture or image slot, a uniform-buffer handle by serial: fb_texu_reads), so it runs in depth-only passes too; there a handle table in c2 (TEXA), which it cannot
// read, does not count, and pool shaders and dynamic tables are taken as possible readers only in colour passes, as before (shadow passes would otherwise end at every draw). A read
// through an image slot or a handle by a draw that also writes depth stays in the pass: no copy can stand in for those. DM_NVN_FB_DEPTH=0 reads the attachment directly, as before
static uint64_t g_fb_dsnaps, g_fb_dsnap_reuse, g_fb_dsplit; static int g_fb_depth_m = -1;
static int fb_depth_on(void) {
    if (g_fb_depth_m < 0) { const char *e = getenv("DM_NVN_FB_DEPTH"); g_fb_depth_m = !(e && !strcmp(e, "0")); if (!g_fb_depth_m) MLOG("same-pass depth sampling: draws read their depth attachment in the pass (DM_NVN_FB_DEPTH=0)"); }
    return g_fb_depth_m;
}
// This draw's texture slot b, texture tid of serial ser, the depth attachment: substitute a copy of its level and layer; 0 when that cannot be (not a single-level 2D view of it)
static int fb_depth_snap(NvShader *fs, const nvn_rbind *b, int tid, uint64_t ser) {
    if (g_fb_n >= 4 || b->dtt != MTLTextureType2D) return 0;
    uint64_t dser; int lv, sl; rt_sub(&R.rt, 8, &dser, &lv, &sl);
    os_unfair_lock_lock(&g_rmu); void *tv = NULL; tex_fast_rid(tid, &tv); id<MTLTexture> t = (__bridge id<MTLTexture>)tv; os_unfair_lock_unlock(&g_rmu);
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
        rp_flush();   // the same for a clear still waiting (see fb_snap)
        enc_b_copy_texture(blit(), base, (NSUInteger)sl, (NSUInteger)lv, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), e->snap, 0, 0, MTLOriginMake(0, 0, 0));
        fb_pass_sync();
    } else g_fb_dsnap_reuse++;
    if (g_fb_use[g_fb_n] && g_fb_use[g_fb_n] != use) defer_obj_unref(g_fb_use[g_fb_n]);
    g_fb_src[g_fb_n] = t; g_fb_use[g_fb_n] = use; g_fb_rid[g_fb_n] = use.gpuResourceID._impl; g_fb_n++;
    if (!fresh) { PD("N %llu same-pass depth sampling: depth ser=%llu level %d layer %d: this draw samples the copy taken since the last barrier", g_stat_pass + !g_enc, (unsigned long long)ser, lv, sl); return 1; }
    if (++g_fb_dsnaps == 1 || g_fb_dsnaps == 10 || g_fb_dsnaps == 1000 || g_fb_dsnaps % 100000 == 0)
        MLOG("same-pass depth sampling: copy #%llu, %llu draws reused one (frame %llu pass %llu, shader %s slot %d samples its depth attachment ser=%llu %lux%lu format %lu while writing it)", (unsigned long long)g_fb_dsnaps,
             (unsigned long long)g_fb_dsnap_reuse, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass + !g_enc, fs->key, b->reg, (unsigned long long)ser, (unsigned long)w, (unsigned long)h, (unsigned long)base.pixelFormat);
    PD("N %llu same-pass depth sampling: depth ser=%llu level %d layer %d copied into a snapshot; this draw samples the snapshot", g_stat_pass + !g_enc, (unsigned long long)ser, lv, sl);
    return 1;
}
// The depth rule (above) for a draw whose pass has a depth attachment; dsw holds the planes written by this draw. Run before the colour attachments' snapshots and tile reads, which need the pass
// this decides on
static void fb_depth_rule(NvShader *fs, uint32_t dsw) {
    uint64_t dser = R.rt.depth->serial; int slot[4], stid[4], ns = 0;
    uint32_t direct = fs->rf.bindless ? fb_texu_reads(fs, dser, R.rt.n > 0) : 0;
    uint32_t slot_planes[4];
    for (int i = 0; i < fs->rf.nb; i++) {
        const nvn_rbind *b = &fs->rf.b[i];
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
    // Resolve a clear before sampling its plane, including a clear that has not reached an encoder yet.
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
// Called before the draw opens its render pass: if a snapshot is needed, end the current render pass, copy the snapshot (or reuse a valid one) and register the substitution (used by this draw's argument encoding)
static void fb_prepare(NvProg *pr) {
    g_draw_seq++; g_fb_n = 0;
    if (g_fb_rebind) { args_dirty_all(); g_fb_rebind = 0; }   // the previous draw had a substituted binding: re-encode with the original bindings this time
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
    uint32_t wm = 0;   // the attachments this draw writes
    for (int k = 0; k < R.rt.n && k < 8; k++) if (R.rt.color[k] && ((S.chmask.mask >> (4 * k)) & 0xf) && (fs->rf.color_known ? fs->rf.color_mask >> k & 1 : 1)) wm |= 1u << k;
    if (g_enc && (wm & g_fb_rmask)) { g_fb_split++; rp_end(); fb_pass_sync(); }   // an earlier draw of this pass read an attachment this one writes: it must not see these writes
    fb_ds_before_write(dsw);
    if (R.rt.depth) fb_depth_rule(fs, dsw);
    if (R.rt.n && R.rt.depth && g_fb_dread != FB_DS_BOTH && ((fs->rf.pool && pool_ds_read(R.rt.depth) && (g_fb_dread_pool = 1)) || fs->rf.dyn)) g_fb_dread |= FB_DS_BOTH;
    uint64_t tile[4]; int ntile = fb_tile_plan(pr, wm, tile);
    for (int i = 0; i < fs->rf.nb && g_fb_n < 4; i++) {
        const nvn_rbind *b = &fs->rf.b[i];
        int image = b->kind == NVN_RB_IMG && !b->rw && !b->atom && b->reg < 32;
        // A texture named by one handle in a uniform buffer (TEXU): encode_args resolves it as below and substitutes the snapshot as it does a slot's (fb_snapshot_slot). FINAL FANTASY XII
        // THE ZODIAC AGE's limit-break echo (e40adef79f6f29b7, 6 draws a frame) samples the 1920x1080 layer it blends into through one: magenta strokes over the effect while only slots were
        // looked at here. A handle has no slot, so it never reads the tile, and its sampler is not a slot's, so no level is pinned for it. Arrays and tables of handles are not taken
        int texu = b->kind == NVN_RB_TEXU && !b->n;
        if (!image && !texu && (b->kind != NVN_RB_TEX || b->reg >= NTEX)) continue;
        uint64_t handle = image ? bound_img(1, b->reg) : texu ? ubo_bind_handle(1, b) : bound_tex(1, b->reg);
        int tid = texu ? ubo_tex_id((uint32_t)handle) : (int)(handle & 0xfffff); uint64_t ser = tid_ser(tid); if (!ser) continue;
        if (R.rt.depth && R.rt.depth->serial == ser) continue;   // fb_depth_rule
        for (int k = 0; k < R.rt.n && k < 8 && g_fb_n < 4; k++) { if (!R.rt.color[k] || R.rt.color[k]->serial != ser) continue;
            if (!image && !texu && (tile[b->reg >> 6] >> (b->reg & 63) & 1)) { PD("N %llu same-pass feedback sampling: slot %d reads attachment %d ser=%llu from the tile", g_stat_pass + !g_enc, b->reg, k, (unsigned long long)ser); continue; }   // fb_tile_plan
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
            int dup = 0; for (int q = 0; q < g_fb_n; q++) dup |= g_fb_src[q] == t; if (dup) continue;
            id<MTLTexture> base = t.parentTexture ? t.parentTexture : t; NSUInteger tl = t.parentTexture ? t.parentRelativeLevel : 0, ts = t.parentTexture ? t.parentRelativeSlice : 0;
            if (tl + t.mipmapLevelCount <= (NSUInteger)lv || tl > (NSUInteger)lv) continue;   // the sampled levels do not include the attachment's level (mip generation etc.): not feedback
            NSUInteger tn = t.textureType == MTLTextureType2DArray || t.textureType == MTLTextureTypeCubeArray || t.textureType == MTLTextureType2DMultisampleArray ? t.arrayLength * (t.textureType == MTLTextureTypeCubeArray ? 6 : 1) : t.textureType == MTLTextureTypeCube ? 6 : 1;
            if (ts + tn <= (NSUInteger)sl || ts > (NSUInteger)sl) continue;   // nor its layer: not feedback
            // What the draw is given depends on the levels it reads (fb_reach), a: the attachment's level in the view. The attachment's level read through a texture holds no defined data
            // on Apple's GPUs while its pass renders it, and a linear mip filter fetches the level above an integer LOD even at weight 0: Donkey Kong Bananza's environment cube (128x128,
            // 8 levels, R11G11B10F, levels 1..7 each drawn from the one above with an explicit level) got NaN texels from sampling its full view; its reflections spread them over the scene
            // through the cube's prefilter, and bloom turned them into white blotches.
            // - One level other than the attachment's (a pinned sampler): a view of that level alone, whichever side it lies on. Donkey Kong Bananza's bloom chains (576x324 R11G11B10F,
            //   5 levels) upsample with ad1a5991aaad135e, its sampler's LOD clamp n + 1..n + 1; the view of the levels below clamped that read to n - 1: the stage intro's neon letters
            //   came out fat and filled. Several levels, all above the attachment's: the view as it is.
            // - Levels from below the attachment's, or no sampler known (a mip-chain pass that picks the level below with an explicit LOD through a sampler that clamps nothing: the cube
            //   above, agl's mip chain de4e1bfab0572149 in Ocarina of Time): a view of the levels below, with the same base and so the same level numbers. No copy. Exact when the clamp
            //   ends below the attachment's level; a shader that picks a level at or above it with its own LOD reads the view's last level instead (no game is known to do it).
            // - Otherwise the clamp starts at the attachment's level (a pinned read of the level the pass renders, a range or a fractional LOD from there up), or the pass renders the
            //   view's base level with no sampler known: a snapshot of the whole view, its type, sizes, layers and levels, so the shader's LODs and the sampler's clamp keep their meaning,
            //   with the levels the sampler can reach copied (every level when it is not known). A view of the attachment's level and layer alone is the single-level 2D snapshot this
            //   rule began with
            int n = (int)t.mipmapLevelCount, a = lv - (int)tl, lo = 0, hs = n - 1, hi = n - 1;
            int known = !image && !texu && fb_reach(fs, b->reg, (int)(handle >> 20), n, &lo, &hs, &hi);
            if (g_fb_mode == 1 && !((wm | g_fb_wmask) >> k & 1)) { g_fb_rmask |= 1u << k; continue; }   // nothing in this pass has written it so far: read directly
            if (n != 1 && known && lo == hs && lo != a) {
                id<MTLTexture> use = [base newTextureViewWithPixelFormat:t.pixelFormat textureType:t.textureType levels:NSMakeRange(tl + (NSUInteger)lo, 1) slices:NSMakeRange(ts, tn) swizzle:t.swizzle];
                if (!use) { RED_ONCE(143, "same-pass feedback sampling: cannot make the view of level %lu that the sampler pins (format %lu); left as is", (unsigned long)(tl + (NSUInteger)lo), (unsigned long)t.pixelFormat); continue; }
                if (g_fb_use[g_fb_n] && g_fb_use[g_fb_n] != use) defer_obj_unref(g_fb_use[g_fb_n]);
                g_fb_src[g_fb_n] = t; g_fb_use[g_fb_n] = use; g_fb_rid[g_fb_n] = use.gpuResourceID._impl; g_fb_n++;
                static int once; if (!once++) MLOG("same-pass feedback sampling: shader %s renders level %d of a view of levels %lu+%lu; its sampler pins level %lu, which it samples alone", fs->key, lv, (unsigned long)tl, (unsigned long)t.mipmapLevelCount, (unsigned long)(tl + (NSUInteger)lo));
                continue;
            }
            if (known && lo > a) continue;   // a range of levels above the attachment's: the view as it is
            if (a > 0 && (!known || lo < a)) {
                id<MTLTexture> use = [base newTextureViewWithPixelFormat:t.pixelFormat textureType:t.textureType levels:NSMakeRange(tl, (NSUInteger)a) slices:NSMakeRange(ts, tn) swizzle:t.swizzle];
                if (!use) { RED_ONCE(82, "same-pass feedback sampling: cannot make the view of levels %lu+%d below the attachment (format %lu); left as is", (unsigned long)tl, a, (unsigned long)t.pixelFormat); continue; }
                if (g_fb_use[g_fb_n] && g_fb_use[g_fb_n] != use) defer_obj_unref(g_fb_use[g_fb_n]);
                g_fb_src[g_fb_n] = t; g_fb_use[g_fb_n] = use; g_fb_rid[g_fb_n] = use.gpuResourceID._impl; g_fb_n++;
                static int once; if (!once++) MLOG("same-pass feedback sampling: shader %s renders level %d of a view of levels %lu+%lu; it samples a view of the levels below (mip-chain pass)", fs->key, lv, (unsigned long)tl, (unsigned long)t.mipmapLevelCount);
                continue;
            }
            // The snapshot has the view's type; the draw samples a view of it in the type the shader declares (the host holds cube maps as 2D arrays: encode_args would fit one)
            int single = n == 1 && tn == 1, ty = (int)t.textureType, vt = b->dtt;
            int fits = vt == ty || (ty == MTLTextureType2DArray && ((vt == MTLTextureTypeCube && tn == 6) || (vt == MTLTextureTypeCubeArray && tn % 6 == 0)))
                || ((ty == MTLTextureTypeCube || ty == MTLTextureTypeCubeArray) && vt == MTLTextureType2DArray);
            if (t.sampleCount != 1 || base.parentTexture || !fits || (single ? ty != MTLTextureType2D :
                    ty != MTLTextureType2D && ty != MTLTextureType2DArray && ty != MTLTextureTypeCube && ty != MTLTextureTypeCubeArray)) {
                RED_ONCE(80, "same-pass feedback sampling: a sampled view the snapshot cannot stand in for (type %lu as %u, levels %lu+%d, layers %lu+%lu, attachment level %d layer %d, shader %s); left as is",
                         (unsigned long)t.textureType, (unsigned)b->dtt, (unsigned long)tl, n, (unsigned long)ts, (unsigned long)tn, lv, sl, fs->key);
                continue; }
            if (!known) lo = 0, hi = n - 1;   // every level the shader may name
            uint32_t need = (uint32_t)(((2ull << hi) - 1) & ~((1ull << lo) - 1));
            fb_shape_t shp = {base.pixelFormat, t.textureType, MAX(base.width >> tl, 1ul), MAX(base.height >> tl, 1ul), single ? 1 : t.arrayLength, (NSUInteger)n};
            NSUInteger w = shp.w, h = shp.h; int fresh;
            fb_cache_t *e = fb_snap_shape(ser, (int)tl, (int)ts, &shp, g_fb_mode == 2, &fresh); if (!e) continue;
            id<MTLTexture> snap = e->snap;
            uint32_t miss = need & ~e->lvm;   // a snapshot taken since the last barrier lacks levels an earlier draw did not reach: copy those too
            if (miss) {
                rp_flush();   // ends the pass, or lands a clear still waiting to be its load action: the snapshot holds the cleared content (fb_snap)
                id<MTL4ComputeCommandEncoder> be = blit();
                for (int m = 0; m < n; m++) if (miss >> m & 1) for (NSUInteger q = 0; q < tn; q++)
                    enc_b_copy_texture(be, base, ts + q, tl + (NSUInteger)m, MTLOriginMake(0, 0, 0), MTLSizeMake(MAX(w >> m, 1ul), MAX(h >> m, 1ul), 1), snap, q, (NSUInteger)m, MTLOriginMake(0, 0, 0));
                e->lvm |= miss;
            } else if (g_enc && (g_fb_rmask >> k & 1)) rp_end();   // an earlier draw of this pass read the attachment directly: it must not see this pass's writes
            if (!g_enc) fb_pass_sync();
            MTLTextureSwizzleChannels sw = t.swizzle;
            int same = t.pixelFormat == snap.pixelFormat && (int)snap.textureType == vt && sw.red == MTLTextureSwizzleRed && sw.green == MTLTextureSwizzleGreen && sw.blue == MTLTextureSwizzleBlue && sw.alpha == MTLTextureSwizzleAlpha;
            id<MTLTexture> use = same ? snap : [snap newTextureViewWithPixelFormat:t.pixelFormat textureType:(MTLTextureType)vt levels:NSMakeRange(0, (NSUInteger)n) slices:NSMakeRange(0, tn) swizzle:sw];
            if (!use) { RED_ONCE(81, "same-pass feedback sampling: cannot create the snapshot view (format %lu); left as is", (unsigned long)t.pixelFormat); continue; }
            if (g_fb_use[g_fb_n] && g_fb_use[g_fb_n] != use) defer_obj_unref(g_fb_use[g_fb_n]);
            g_fb_src[g_fb_n] = t; g_fb_use[g_fb_n] = use; g_fb_rid[g_fb_n] = use.gpuResourceID._impl; g_fb_n++;
            if (!miss) { g_fb_reuse++; PD("N %llu same-pass feedback sampling: attachment %d ser=%llu level %d layer %d: this draw samples the snapshot taken since the last barrier", g_stat_pass + !g_enc, k, (unsigned long long)ser, lv, sl); continue; }
            if (!single) { static int once; if (!once++) MLOG("same-pass feedback sampling: shader %s renders level %d layer %d of a view of levels %lu+%d, layers %lu+%lu; it samples a snapshot of the view (levels %d..%d copied)",
                                                            fs->key, lv, sl, (unsigned long)tl, n, (unsigned long)ts, (unsigned long)tn, lo, hi); }
            g_fb_count++;
            if (g_fb_count == 1 || g_fb_count == 10 || g_fb_count == 1000 || g_fb_count % 100000 == 0)
                MLOG("same-pass feedback sampling: snapshot #%llu, %llu draws reused one, %llu read the tile (%llu waited for their pipeline), %llu passes ended before a write to an attachment read directly (frame %llu pass %llu, shader %s %s %d samples attachment %d ser=%llu %lux%lu format %lu)", (unsigned long long)g_fb_count, (unsigned long long)g_fb_reuse, (unsigned long long)g_fb_tile_draws, (unsigned long long)g_fb_tile_wait, (unsigned long long)g_fb_split,
                     (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass + !g_enc, fs->key, texu ? "handle in uniform slot" : image ? "image slot" : "slot", b->reg, k, (unsigned long long)ser, (unsigned long)w, (unsigned long)h, (unsigned long)base.pixelFormat);
            PD("N %llu same-pass feedback sampling: attachment %d ser=%llu level %d layer %d copied into a snapshot; this draw samples the snapshot", g_stat_pass + !g_enc, k, (unsigned long long)ser, lv, sl);
        } }
    g_fb_wmask |= wm; g_fb_dwrote |= dsw;
    if (g_fb_n) { g_fb_seq = g_draw_seq; args_dirty_all(); g_fb_rebind = 1; }
    if (ntile) { g_fb_tseq = g_draw_seq; g_fb_tile_draws++;
        if (g_fb_tile_draws == 1 || g_fb_tile_draws == 1000 || g_fb_tile_draws % 100000 == 0)
            MLOG("same-pass feedback sampling: draw #%llu reads its attachments from the tile, %llu snapshots so far (frame %llu, shader %s, attachments %#x)", (unsigned long long)g_fb_tile_draws, (unsigned long long)g_fb_count,
                 (unsigned long long)atomic_load(&g_cur_frame), fs->key, g_fb_tdesc.fbmask); }
}
