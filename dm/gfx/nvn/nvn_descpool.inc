// The descriptor pool: texture and sampler IDs to objects, views by ID, texture buffers, the IDs shaders read through pools, slot types, and the argument block arenas.
// Registration snapshots the whole texture object (NVN: the descriptor is written into the pool); view: the ID's MTLTexture (CFBridgingRetain) and the generations it was taken at.
typedef struct { tex_t t; int ok; view_t v; int has_v; void *view; uint64_t view_id, view_rid, view_ser, view_dirty, view_stamp; const void *view_span; int view_pkg; uint32_t view_pf; uint8_t view_tt;   // view_pkg: CPU-written; only those depend on guest writes. view_pf / view_tt: view's pixel format and type, which never change
                 void *palt[4], *palt_of[4]; uint64_t palt_rid[4];   // palt[k]: the view a pool table of layered type k holds (pool_fit), apart from alt so binds and tables do not take turns
                 void *alt, *alt_of; uint64_t alt_rid; uint8_t alt_type;   // alt: the view for a shader's other declared type (tex_fit)
                 void *im, *im_of; uint64_t im_rid;
                 int prev, next, retired, gone; uint64_t transient_sub; uint8_t tt1, sp, evicted;   // prev / next: descriptor index + 1, by serial; gone: its pool was finalized; sp: in g_treg_sp; evicted: idle package freed
                 void *stand; uint64_t stand_rid, stand_sum, reg_frame; uint8_t stand_tt1, stand_no;
                 uint32_t used; } treg_t;
enum { STAND_NO = 1, STAND_WAIT = 2 };   // STAND_WAIT: none until a mapping over its pages notes it
static treg_t *g_treg; static int g_ntreg;
static nvn_hmap g_treg_serial = {.klen = 8};   // under g_rmu
// ID views are valid for an epoch (view_ser): ser_sync sends only the IDs of noted serials down the slow path; a change without a note starts a new epoch. Under g_rmu.
static uint64_t g_treg_ep = 1, g_ser_synced = 1;
static void ser_sync(void) {
    uint64_t s[SER_NOTES];
    uint64_t sg; unsigned n = own_notes_take(s, &sg);
    if (sg - g_ser_synced != n) g_treg_ep++;
    else for (unsigned i = 0; i < n; i++) for (int k = (int)(uintptr_t)nvn_hm_get(&g_treg_serial, &s[i]); k; k = g_treg[k - 1].next) g_treg[k - 1].view_ser = 0;
    g_ser_synced = sg;
}
static inline uint64_t treg_ep(void) { if (g_ser_gen != g_ser_synced) ser_sync(); return g_treg_ep; }
// A sparse mapping changes what a texture reads with no registration or serial change: at each submission's start the mappings since the last note the serials of
// the textures they overlap, so their IDs resolve again and the tables patch just those. Mappings made during a replay are noted at the next one.
static int *g_treg_sp; static size_t g_ntreg_sp, g_treg_sp_cap;   // under g_rmu
static uint64_t g_sp_noted;   // submit thread
static void treg_sparse_notes(void) {
    if (nvn_sparse_gen() == g_sp_noted) return;
    enum { NOTES = 4096 }; static uint64_t (*r)[2], *ser; static size_t ser_cap;
    if (!r) r = dm_realloc(NULL, NOTES * sizeof *r, "the sparse mapping notes");
    int n = nvn_sparse_notes(&g_sp_noted, r, NOTES), m = 0;
    if (n > 0) {   // disjoint ranges in order: the first that ends past a texture's start is the only one that can overlap it
        qsort(r, (size_t)n, sizeof r[0], sp_cmp);
        for (int k = 1; k < n; k++) { if (r[k][0] <= r[m][1]) { if (r[k][1] > r[m][1]) r[m][1] = r[k][1]; } else { m++; r[m][0] = r[k][0]; r[m][1] = r[k][1]; } }
        m++;
    }
    if (!n) return;
    size_t ns = 0, w = 0;
    os_unfair_lock_lock(&g_rmu);
    for (size_t i = 0; i < g_ntreg_sp; i++) {
        int id_ = g_treg_sp[i]; treg_t *rp = &g_treg[id_];
        if (!rp->ok || !rp->t.pool || !nvn_sparse_pool(rp->t.pool)) { rp->sp = 0; continue; }
        g_treg_sp[w++] = id_;
        uint64_t lo = (uint64_t)(uintptr_t)rp->t.mem, hi = lo + rp->t.L.size;
        int hit = n < 0;
        if (!hit) { int a = 0, b = m; while (a < b) { int c = (a + b) / 2; if (r[c][1] <= lo) a = c + 1; else b = c; } hit = a < m && r[a][0] < hi; }
        if (!hit) continue;
        if (rp->stand_no == STAND_WAIT) rp->stand_no = 0;
        if (!rp->t.serial) continue;
        if (ns == ser_cap) { ser_cap = ser_cap ? ser_cap * 2 : 64; ser = dm_realloc(ser, ser_cap * sizeof *ser, "the sparse mapping notes"); }
        ser[ns++] = rp->t.serial;
    }
    g_ntreg_sp = w;
    os_unfair_lock_unlock(&g_rmu);
    if (ns > 1) qsort(ser, ns, sizeof *ser, sp_cmp);
    for (size_t i = 0; i < ns; i++) if (!i || ser[i] != ser[i - 1]) own_note(ser[i]);
}
static int *g_treg_transient; static size_t g_ntreg_transient, g_treg_transient_cap;
// Lock-free ID → serial: IDs are 20 bits (BSS, only touched pages used); release write at registration, relaxed read without g_rmu.
static _Atomic uint64_t g_tser[1 << 20];
static samp_par *g_sreg; static uint8_t *g_sreg_ok; static void **g_sview; static uint64_t *g_srid; static int g_nsreg;
static void **g_sview_w; static uint64_t *g_srid_w; static float (*g_sbk)[4];   // a sampler's twin for a border colour Metal lacks (retained, NULL = none) and its factors k (nvn_border.h)
static _Atomic int g_border_any;   // sticky; set before the lazy Metal creation, so the first draw keeps the corrections
static int border_need(NvShader *sh, int nvn_stage);
// Pool Finalize: drop the IDs' cached textures, or a linear texture built on the pool keeps the whole pool buffer alive.
static void treg_drop_im(treg_t *r) {
    if (!r->im) return;
    defer_obj_unref((__bridge id)r->im);
    CFRelease(r->im);
    r->im = r->im_of = NULL;
    r->im_rid = 0;
}
static void treg_drop_views(treg_t *r) {
    if (r->view) { defer_obj_unref((__bridge id)r->view); CFRelease(r->view); r->view = NULL; r->view_id = r->view_rid = 0; r->view_pf = 0; r->view_tt = 0; }
    if (r->alt) { defer_obj_unref((__bridge id)r->alt); CFRelease(r->alt); r->alt = r->alt_of = NULL; r->alt_rid = 0; }
    for (int k = 0; k < 4; k++) if (r->palt[k]) { defer_obj_unref((__bridge id)r->palt[k]); CFRelease(r->palt[k]); r->palt[k] = r->palt_of[k] = NULL; r->palt_rid[k] = 0; }
    treg_drop_im(r);
}
// The stand-in goes with the registration or when the texture is made; tables naming it are read up to the open submission at most (defer_obj_unref).
static uint64_t g_stand_live, g_stand_done;
static void treg_drop_stand(treg_t *r) {
    if (r->stand) { defer_obj_unref((__bridge id)r->stand); CFRelease(r->stand); r->stand = NULL; r->stand_rid = 0; g_stand_live--; }
    r->stand_no = 0;
}
static void treg_unlink(int id_) {
    treg_t *r = &g_treg[id_];
    if (!r->ok || !r->t.serial) return;
    if (r->prev) g_treg[r->prev - 1].next = r->next;
    else if (r->next) nvn_hm_put(&g_treg_serial, &r->t.serial, (void *)(uintptr_t)r->next);
    else nvn_hm_del(&g_treg_serial, &r->t.serial);
    if (r->next) g_treg[r->next - 1].prev = r->prev;
    r->prev = r->next = 0;
}
static void treg_retire_serial(uint64_t ser) {
    // Finalize retires the API object, not the descriptor's metadata: cached views go now, or streamed uploads stay pinned in IDs never reused.
    os_unfair_lock_lock(&g_rmu);
    for (int n = (int)(uintptr_t)nvn_hm_get(&g_treg_serial, &ser); n; n = g_treg[n - 1].next) {
        treg_t *r = &g_treg[n - 1]; r->retired = 1; treg_drop_views(r);
    }
    os_unfair_lock_unlock(&g_rmu);
}
static void treg_finish_submission(void) {
    if (!g_ntreg_transient) return;
    os_unfair_lock_lock(&g_rmu);
    for (size_t i = 0; i < g_ntreg_transient; i++) {
        treg_t *r = &g_treg[g_treg_transient[i]];
        if (r->retired && r->transient_sub == g_sub_serial) treg_drop_views(r);
    }
    g_ntreg_transient = 0;
    os_unfair_lock_unlock(&g_rmu);
}
// View cache keyed by the Metal texture (a view retains its parent, so the address names no other texture while the entry lives): rings of IDs, finalized textures
// and per-frame re-initialized targets reach the same texture again. Entries exist only while the owner holds the texture and go at the first commit after it lets go
// (vc_commit: the owner runs under g_smu, which cannot take g_rmu). Under g_rmu.
typedef struct { uint32_t pf, l0, nl, s0, ns; uint8_t tt, sw[4], pad[3]; } vc_key;
enum { VC_WAYS = 8, VC_MAX = 16384 };   // 8: a mip chain's per-level feedback views (fb_view) beside the registrations' own; with 4 they took turns
typedef struct { const void *pool; unsigned next; vc_key k[VC_WAYS]; void *view[VC_WAYS];
                 void *rows; uint64_t rows_rid, rows_n; } vc_set;
static nvn_hmap g_vc = {.klen = 8};
static uint64_t g_vc_hit, g_vc_new;
static inline uint64_t vc_addr(id<MTLTexture> t) { return (uint64_t)(uintptr_t)(__bridge void *)t; }
static void vc_rows_drop(vc_set *s) { if (s->rows) { defer_obj_release((__bridge id)s->rows); CFRelease(s->rows); s->rows = NULL; s->rows_rid = s->rows_n = 0; } }
static void vc_set_free(void *p) {
    vc_set *s = p;
    for (int i = 0; i < VC_WAYS; i++) if (s->view[i]) { defer_obj_unref((__bridge id)s->view[i]); CFRelease(s->view[i]); }
    vc_rows_drop(s);
    free(s);
}
static id<MTLTexture> vc_get(id<MTLTexture> base, const vc_key *k) {
    uint64_t a = vc_addr(base); vc_set *s = nvn_hm_get(&g_vc, &a); if (!s) return nil;
    for (int i = 0; i < VC_WAYS; i++) if (s->view[i] && !memcmp(&s->k[i], k, sizeof *k)) { g_vc_hit++; return (__bridge id<MTLTexture>)s->view[i]; }
    return nil;
}
static vc_set *vc_set_for(id<MTLTexture> base, const void *pool) {
    uint64_t a = vc_addr(base); vc_set *s = nvn_hm_get(&g_vc, &a); if (s) return s;
    pthread_mutex_lock(&g_smu); int held = own_holds_locked((__bridge const void *)base); pthread_mutex_unlock(&g_smu);   // lock order g_rmu, then g_smu
    if (!held) return NULL;
    if (g_vc.n >= VC_MAX) nvn_hm_clear(&g_vc, vc_set_free);
    if (!(s = calloc(1, sizeof *s))) return NULL;
    s->pool = pool; nvn_hm_put(&g_vc, &a, s);
    return s;
}
static void vc_put(id<MTLTexture> base, const void *pool, const vc_key *k, id<MTLTexture> v) {
    g_vc_new++;
    vc_set *s = vc_set_for(base, pool); if (!s) return;
    unsigned i = s->next++ % VC_WAYS;
    if (s->view[i]) { defer_obj_unref((__bridge id)s->view[i]); CFRelease(s->view[i]); }
    s->k[i] = *k; s->view[i] = (void *)CFBridgingRetain(v);
}
// At each commit: the entries of textures the owner let go (or, past its list, of every texture it no longer holds); a listed address may be held again or reused, so each is checked.
static void vc_commit(void) {
    static vc_set **drop; static unsigned drop_cap;
    os_unfair_lock_lock(&g_rmu);
    pthread_mutex_lock(&g_smu);
    const void *const *gone; unsigned n, nd = 0;
    int over = own_gone_take_locked(&gone, &n);
    if (g_vc.n && (over || n)) {
        if (drop_cap < g_vc.n) { drop_cap = g_vc.cap; drop = dm_realloc(drop, drop_cap * sizeof *drop, "the texture views of released textures"); }
        if (over) for (uint32_t i = 0; i < g_vc.cap;) {
            vc_set *s = g_vc.vals[i]; uint64_t a; memcpy(&a, g_vc.keys + (size_t)i * 8, 8);
            if (s && !own_holds_locked((const void *)(uintptr_t)a)) { nvn_hm_del(&g_vc, &a); drop[nd++] = s; }
            else i++;
        }
        else for (unsigned j = 0; j < n; j++) {
            uint64_t a = (uintptr_t)gone[j]; vc_set *s = nvn_hm_get(&g_vc, &a);
            if (s && !own_holds_locked(gone[j])) { nvn_hm_del(&g_vc, &a); drop[nd++] = s; }
        }
    }
    pthread_mutex_unlock(&g_smu);
    for (unsigned i = 0; i < nd; i++) vc_set_free(drop[i]);
    os_unfair_lock_unlock(&g_rmu);
}
void mtl_vc_stats(unsigned *n, uint64_t *hit, uint64_t *made) { os_unfair_lock_lock(&g_rmu); *n = g_vc.n; *hit = g_vc_hit; *made = g_vc_new; os_unfair_lock_unlock(&g_rmu); }
static void tb2_gone_clear(void);
static void vc_drop_pool(const void *pool) {
    tb2_gone_clear();
    for (uint32_t i = 0; i < g_vc.cap;) {
        vc_set *s = g_vc.vals[i];
        if (s && s->pool == pool) { uint64_t a; memcpy(&a, g_vc.keys + (size_t)i * 8, 8); nvn_hm_del(&g_vc, &a); vc_set_free(s); }
        else i++;
    }
}
// A texture buffer whose 1D fetches the translator made 2D (f1d): a linear 2D texture of NVN_TBUF_ROW-texel rows over the same memory, else it would read zeros. Caller holds g_rmu.
static uint64_t g_tb2_binds, g_tb2_made, g_tb2_fail, g_tb2_copy, g_tb2_copy_bytes;
// Workaround for a platform limit: Metal views a buffer texture only at the device's linear-texture alignment, so a misaligned offset is read from an aligned copy in the arena,
// once per submission (what the CPU wrote before encoding; a CPU-inaccessible pool reads 0). Caller holds g_rmu.
enum { TB2_STAGE = 64 };
static struct { __unsafe_unretained id<MTLTexture> src, view; uint64_t epoch, rid; } g_tb2_stage[TB2_STAGE];
static id<MTLTexture> tbuf_rows_copy(id<MTLTexture> t, id<MTLBuffer> src, size_t off, MTLPixelFormat pf, uint32_t w, uint32_t h, uint32_t row, NSUInteger al, uint64_t *rid) {
    if (!src || off >= src.length) return nil;
    // asking a private buffer for contents is invalid under Metal validation
    if (src.storageMode == MTLStorageModePrivate || !src.contents) { RED_ONCE(137, "texture buffer at pool offset %#zx (not a multiple of the %lu-byte linear-texture alignment) in a pool the CPU cannot access: the slot reads 0", off, (unsigned long)al); return nil; }
    unsigned k = (unsigned)(((uintptr_t)(__bridge void *)t >> 4) % TB2_STAGE);
    if (g_tb2_stage[k].src == t && g_tb2_stage[k].epoch == g_fence + 1) { *rid = g_tb2_stage[k].rid; return g_tb2_stage[k].view; }
    while (h > 1 && off + (uint64_t)row * h > src.length) h--;
    size_t need = (size_t)row * h, n = src.length - off < need ? src.length - off : need;
    id<MTLBuffer> ab; size_t ao = arena_alloc(need + (al > 256 ? al : 0), &ab); ao = (ao + al - 1) / al * al;
    memcpy((uint8_t *)ab.contents + ao, (const uint8_t *)src.contents + off, n); memset((uint8_t *)ab.contents + ao + n, 0, need - n);
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    d.resourceOptions = ab.resourceOptions; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> v = [ab newTextureWithDescriptor:d offset:ao bytesPerRow:row];
    if (!v) { RED_ONCE(138, "cannot make the aligned 2D rows of a texture buffer (%ux%u, row %u, pool offset %#zx)", w, h, row, off); return nil; }
    defer_obj_release(v);
    g_tb2_stage[k].src = t; g_tb2_stage[k].view = v; g_tb2_stage[k].epoch = g_fence + 1; g_tb2_stage[k].rid = *rid = v.gpuResourceID._impl;
    if (!g_tb2_copy++) MLOG("texture buffer at pool offset %#zx (not a multiple of the %lu-byte linear-texture alignment) read through aligned rows %ux%u of %u bytes (first case)", off, (unsigned long)al, w, h, row);
    g_tb2_copy_bytes += n;
    return v;
}
// DM_TBUF_ALIGN=<bytes> (diagnostic): a larger alignment, to exercise the copies on a Mac
static NSUInteger tbuf_align(NSUInteger al) {
    static long v = -1;
    if (v < 0) {
        const char *e = getenv("DM_TBUF_ALIGN");
        v = e && *e ? strtol(e, NULL, 0) : 0;
        if (v < 0) v = 0;
    }
    return (NSUInteger)v > al ? (NSUInteger)v : al;
}
// An integer texture read through a float declaration is undefined in Metal: viewed as the same-size float format, whose read hands the bits over.
static MTLPixelFormat tbuf_view_format(MTLPixelFormat pf, int dpf) {
    if (dpf != NVN_DPF_FLOAT) return pf;
    switch (pf) {
    case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: return MTLPixelFormatR32Float;
    case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: return MTLPixelFormatRG32Float;
    case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: return MTLPixelFormatRGBA32Float;
    default: return pf;
    }
}
// Rows of a buffer texture the owner no longer holds, by what they view (else a new view per bind); emptied with the pools so a finalized pool's memory goes.
enum { TB2_WAYS = 1024 };
static struct { void *buf; uint64_t off, n, vf, rid; void *v; } g_tb2_gone[TB2_WAYS];
static void tb2_gone_clear(void) { for (int i = 0; i < TB2_WAYS; i++) if (g_tb2_gone[i].v) { defer_obj_release((__bridge id)g_tb2_gone[i].v); CFRelease(g_tb2_gone[i].v); memset(&g_tb2_gone[i], 0, sizeof g_tb2_gone[i]); } }
static id<MTLTexture> tbuf_as_2d(int tid, id<MTLTexture> t, int dpf, uint64_t *rid, uint32_t *bias) {
    if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok) return nil;
    const tex_t *tt = &g_treg[tid].t; MTLPixelFormat vf = tbuf_view_format(t.pixelFormat, dpf);
    id<MTLTexture> root = t; while (root.parentTexture) root = root.parentTexture;
    int bw, bh; uint32_t bpb = (uint32_t)nvn_fmt_bpb(tt->d.format, &bw, &bh), lead = tbuf_lead(tid, t), w, h, row;
    id<MTLBuffer> buf = root.buffer;
    uint64_t goff = root.bufferOffset + (uint64_t)lead * bpb, gn = t.width - lead, gvf = vf; void *gbuf = (__bridge void *)buf;
    if (bias) { *bias = lead; goff = root.bufferOffset; gn = t.width; }   // live rows: the shader moves the linear coordinate before wrapping it
    unsigned gi = (unsigned)((((uintptr_t)gbuf >> 4) ^ goff * 0x9E3779B97F4A7C15ull ^ gn * 31 ^ gvf) >> 7) % TB2_WAYS;
    if (g_tb2_gone[gi].v && g_tb2_gone[gi].buf == gbuf && g_tb2_gone[gi].off == goff && g_tb2_gone[gi].n == gn && g_tb2_gone[gi].vf == gvf) { g_tb2_binds++; *rid = g_tb2_gone[gi].rid; return (__bridge id<MTLTexture>)g_tb2_gone[gi].v; }
    uint64_t ra = vc_addr(root); vc_set *s = nvn_hm_get(&g_vc, &ra);
    if (s && s->rows) {   // an engine registers new descriptors over the same memory
        id<MTLTexture> rv = (__bridge id<MTLTexture>)s->rows;
        if (rv.buffer == buf && rv.bufferOffset == goff && rv.pixelFormat == vf && s->rows_n == gn) { g_tb2_binds++; *rid = s->rows_rid; return rv; }
    }
    NSUInteger al = tbuf_align([g_dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat]);
    int rows = buf && bw == 1 && bh == 1 && nvn_tbuf_rows((uint32_t)gn, bpb, (uint32_t)al, &w, &h, &row);
    if (rows && goff % al) {
        id<MTLTexture> sv = tbuf_rows_copy(t, buf, goff, vf, w, h, row, al, rid);
        if (sv) { g_tb2_binds++; return sv; }
    }
    if (!rows || goff % al) {
        if (!g_tb2_fail++) MLOG("[RED] texture buffer (texture ID %d, %llu elements of format %#x at pool offset %llu) has no 2D-row form for a wrapped 1D fetch: the slot reads 0", tid, (unsigned long long)gn, tt->d.format, (unsigned long long)goff);
        return nil;
    }
    while (h > 1 && goff + (uint64_t)row * h > buf.length) h--;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:vf width:w height:h mipmapped:NO];
    d.resourceOptions = buf.resourceOptions; d.usage = MTLTextureUsageShaderRead;
    id<MTLTexture> v = [buf newTextureWithDescriptor:d offset:goff bytesPerRow:row];
    if (!v) { if (!g_tb2_fail++) MLOG("[RED] cannot create the 2D-row form of a texture buffer: %ux%u format %lu row %u bytes at pool offset %llu", w, h, (unsigned long)t.pixelFormat, row, (unsigned long long)goff); return nil; }
    if (!g_tb2_made++) MLOG("texture buffer read by wrapped 1D fetches: bound as %ux%u rows of %u bytes (texture ID %d, %llu elements; first case)", w, h, row, tid, (unsigned long long)gn);
    res_add(v);
    *rid = v.gpuResourceID._impl; g_tb2_binds++;
    if ((s = vc_set_for(root, tt->pool))) { vc_rows_drop(s); s->rows = (void *)CFBridgingRetain(v); s->rows_rid = *rid; s->rows_n = gn; }
    else {
        if (g_tb2_gone[gi].v) { defer_obj_release((__bridge id)g_tb2_gone[gi].v); CFRelease(g_tb2_gone[gi].v); }
        g_tb2_gone[gi].buf = gbuf; g_tb2_gone[gi].off = goff; g_tb2_gone[gi].n = gn; g_tb2_gone[gi].vf = gvf; g_tb2_gone[gi].rid = *rid; g_tb2_gone[gi].v = (void *)CFBridgingRetain(v);
    }
    return v;
}
static id<MTLTexture> tbuf_rows_binding(int tid, id<MTLTexture> t, const nvn_rbind *b, int k, uint32_t *tbias, uint64_t *rid) {
    if (!t || t.textureType != MTLTextureTypeTextureBuffer || b->dtt != MTLTextureType2D || !(b->f1d || b->kind == NVN_RB_TEXU)) return t;
    uint32_t *bias = tbias && b->tbx ? tbias + b->tbx - 1 + k : NULL;
    if (bias) bias[b->n ? b->n : 1] = (uint32_t)(t.width - tbuf_lead(tid, t));
    return tbuf_as_2d(tid, t, b->dpf, rid, bias);
}
void mtl_tb2_stats(uint64_t *binds, uint64_t *made, uint64_t *fail, uint64_t *copy, uint64_t *copy_bytes) { *binds = g_tb2_binds; *made = g_tb2_made; *fail = g_tb2_fail; *copy = g_tb2_copy; *copy_bytes = g_tb2_copy_bytes; }
// The ID stays registered but its storage is gone: nothing may make its texture until it is registered again. Set under g_rmu before nvnMemoryPoolFinalize tears anything down,
// so a reader holding g_rmu sees a live pool for an unmarked ID. Every cached view goes behind the open submission's fence.
static void treg_gone(treg_t *r) {
    if (!r->gone) { reg_bump((int)(r - g_treg), 0); bcp_idx((int)(r - g_treg), &r->t, 0); }
    r->gone = 1;
    treg_drop_views(r);
    treg_drop_stand(r);
}
static void treg_drop_pool_views(const void *pool) {
    os_unfair_lock_lock(&g_rmu);
    vc_drop_pool(pool);
    for (int i = 0; i < g_ntreg; i++) if (g_treg[i].ok && (const void *)g_treg[i].t.pool == pool) treg_gone(&g_treg[i]);
    os_unfair_lock_unlock(&g_rmu);
}
// IDs shaders read through a pool (pool_arrays): a bitmap the shaders set, folded at each submission. Only the GPU knows which handles are read.
enum { POOL_SEEN_WORDS = (1u << 20) / 32 };
static __strong id<MTLBuffer> g_seen_buf; static uint32_t g_seen_done[POOL_SEEN_WORDS]; static uint64_t g_seen_sub = ~0ull;
static inline int pool_seen(int id_) { return id_ >= 0 && (g_seen_done[(unsigned)id_ >> 5] >> (id_ & 31) & 1); }
// One section per table type (pool_seen_gpu_t): a table of another type holds a view of an ID only once a shader read it through that type. 2D tables use section 0.
enum { POOL_SEEN_TYPES = 10 };
static inline unsigned seen_sec(int dtt) { return dtt == (int)MTLTextureType2D ? 0u : dtt == 0 ? (unsigned)MTLTextureType2D : (unsigned)dtt; }
static uint32_t g_seen_typed[POOL_SEEN_TYPES][POOL_SEEN_WORDS];
static inline int pool_seen_t(int id_, int dtt) {
    if (dtt < 0 || dtt >= POOL_SEEN_TYPES) return pool_seen(id_);
    return id_ >= 0 && id_ < (int)(POOL_SEEN_WORDS * 32) && (g_seen_typed[seen_sec(dtt)][(unsigned)id_ >> 5] >> (id_ & 31) & 1);
}
static void pool_seen_clear(int id_) {
    if (id_ < 0 || id_ >= (int)(POOL_SEEN_WORDS * 32)) return;
    uint32_t b = 1u << (id_ & 31);
    g_seen_done[(unsigned)id_ >> 5] &= ~b;
    for (unsigned s_ = 0; s_ < POOL_SEEN_TYPES; s_++) {
        g_seen_typed[s_][(unsigned)id_ >> 5] &= ~b;
        if (g_seen_buf) atomic_fetch_and((_Atomic uint32_t *)g_seen_buf.contents + s_ * POOL_SEEN_WORDS + ((unsigned)id_ >> 5), ~b);
    }
}
// a new type: the old texture's type marks would report the new one as read through them; the read mark stays
static void pool_seen_retype(int id_) {
    if (id_ < 0 || id_ >= (int)(POOL_SEEN_WORDS * 32)) return;
    uint32_t b = 1u << (id_ & 31);
    for (unsigned s_ = 0; s_ < POOL_SEEN_TYPES; s_++) {
        g_seen_typed[s_][(unsigned)id_ >> 5] &= ~b;
        if (g_seen_buf) atomic_fetch_and((_Atomic uint32_t *)g_seen_buf.contents + s_ * POOL_SEEN_WORDS + ((unsigned)id_ >> 5), ~b);
    }
}
static int tex_reg_target_of(const tex_t *t, const view_t *v) { return v && v->set_target ? (int)v->target : t->d.target; }
static int tex_reg_target(const treg_t *r) { return tex_reg_target_of(&r->t, r->has_v ? &r->v : NULL); }
void mtl_reg_tex_gone(int tid) {
    os_unfair_lock_lock(&g_rmu);
    if (tid >= 0 && tid < g_ntreg && g_treg[tid].ok) treg_gone(&g_treg[tid]);
    os_unfair_lock_unlock(&g_rmu);
}
void mtl_reg_tex(int tid, const tex_t *t, const view_t *v) {
    int id_ = tid; if (id_ < 0) return;
    os_unfair_lock_lock(&g_rmu);
    if (id_ >= g_ntreg) { int n = id_ + 1024; g_treg = dm_realloc(g_treg, n * sizeof *g_treg, "the texture registrations"); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    // registering what the ID already holds changes nothing
    treg_t *rr = &g_treg[id_];
    int same = t && rr->ok && rr->has_v == (v != NULL) && !memcmp(&rr->t, t, sizeof *t) && (!v || !memcmp(&rr->v, v, sizeof *v));
    if (rr->gone) { rr->gone = 0; reg_bump(id_, 0); if (rr->ok) bcp_idx(id_, &rr->t, 1); }
    if (!same) {
        // The read mark is the ID's and stays while it holds a texture, so a streamed replacement is made under the read budget at its first read, not left to the walk's spent room.
        reg_bump(id_, 0); if (!t) pool_seen_clear(id_); else if (rr->ok && tex_reg_target(rr) != tex_reg_target_of(t, v)) pool_seen_retype(id_);
        treg_unlink(id_); treg_drop_views(rr); treg_drop_stand(rr);
        rr->tt1 = 0; rr->reg_frame = atomic_load(&g_cur_frame);
        if (rr->ok) bcp_idx(id_, &rr->t, 0);
        rr->ok = t != NULL; if (t) rr->t = *t; rr->has_v = v != NULL; if (v) rr->v = *v;
        if (t) bcp_idx(id_, t, 1);
        rr->retired = 0; rr->transient_sub = 0; rr->evicted = 0; rr->used = 0;
        if (t && t->serial) {
            rr->retired = !own_tex_live(t->serial);
            rr->next = (int)(uintptr_t)nvn_hm_get(&g_treg_serial, &t->serial);
            if (rr->next) g_treg[rr->next - 1].prev = id_ + 1;
            nvn_hm_put(&g_treg_serial, &t->serial, (void *)(uintptr_t)(id_ + 1));
        }
        if (t && !rr->sp && t->pool && nvn_sparse_pool(t->pool)) {
            if (g_ntreg_sp == g_treg_sp_cap) { g_treg_sp_cap = g_treg_sp_cap ? g_treg_sp_cap * 2 : 256; g_treg_sp = dm_realloc(g_treg_sp, g_treg_sp_cap * sizeof *g_treg_sp, "the sparse texture IDs"); }
            g_treg_sp[g_ntreg_sp++] = id_; rr->sp = 1;
        }
        if (id_ < (1 << 20)) atomic_store_explicit(&g_tser[id_], t ? t->serial : 0, memory_order_release);
    }
    os_unfair_lock_unlock(&g_rmu);
    {   // DM_NVN_TREG_ID=n logs every registration of texture ID n
        static int want = -2; if (want == -2) { const char *e = getenv("DM_NVN_TREG_ID"); want = e ? atoi(e) : -1; }
        if (want == tid) MLOG("TREG id %d frame %llu %s ser=%llu %ux%u format=%#x target %d view %d", tid, (unsigned long long)atomic_load(&g_cur_frame), same ? "(the same)" : "→",
                              t ? (unsigned long long)t->serial : 0ull, t ? t->d.width : 0, t ? t->d.height : 0, t ? t->d.format : 0, t ? (int)t->d.target : -1, v != NULL);
    }
    {
        static int ti = -1; static uint32_t tw, th; static uint64_t n;
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && t && t->d.width == tw && t->d.height == th && (n++ < 40 || n % 600 == 0))
            MLOG("TEXINFO register id %d ser=%llu format=%#x creation swizzle %d:%d%d%d%d view %d view swizzle %d:%d%d%d%d view format %d:%#x", tid, (unsigned long long)t->serial, t->d.format,
                 t->set_swz, t->swz & 7, t->swz >> 3 & 7, t->swz >> 6 & 7, t->swz >> 9 & 7, v != NULL, v ? v->set_swizzle : 0, v ? v->swz[0] : 0, v ? v->swz[1] : 0, v ? v->swz[2] : 0, v ? v->swz[3] : 0,
                 v ? v->set_format : 0, v ? v->format : 0);
    }
}
// RegisterImage declares image use in the host snapshot only; image_prepare adds the usage before first use (tex_upgrade keeps the content), so render targets keep compression.
void mtl_reg_image(int tid, const tex_t *t, const view_t *v) {
    if (!t) { mtl_reg_tex(tid, NULL, v); return; }
    tex_t image = *t; image.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_reg_tex(tid, &image, v);
}
void mtl_reg_samp(int sid, const samp_par *sp) {
    int id_ = sid; if (id_ < 0) return;
    os_unfair_lock_lock(&g_rmu);
    if (id_ >= g_nsreg) {
        int n = id_ + 256;
        g_sreg = dm_realloc(g_sreg, n * sizeof *g_sreg, "the sampler registrations");
        g_sreg_ok = dm_realloc(g_sreg_ok, n, "the sampler registrations");
        g_sview = dm_realloc(g_sview, n * sizeof *g_sview, "the sampler registrations");
        g_srid = dm_realloc(g_srid, n * sizeof *g_srid, "the sampler registrations");
        g_sview_w = dm_realloc(g_sview_w, n * sizeof *g_sview_w, "the sampler registrations");
        g_srid_w = dm_realloc(g_srid_w, n * sizeof *g_srid_w, "the sampler registrations");
        g_sbk = dm_realloc(g_sbk, n * sizeof *g_sbk, "the sampler registrations");
        memset(g_sreg_ok + g_nsreg, 0, n - g_nsreg);
        memset(g_sview + g_nsreg, 0, (n - g_nsreg) * sizeof *g_sview);
        memset(g_srid + g_nsreg, 0, (n - g_nsreg) * sizeof *g_srid);
        memset(g_sview_w + g_nsreg, 0, (n - g_nsreg) * sizeof *g_sview_w);
        memset(g_srid_w + g_nsreg, 0, (n - g_nsreg) * sizeof *g_srid_w);
        g_nsreg = n;
    }
    if (g_sview[id_]) { CFRelease(g_sview[id_]); g_sview[id_] = NULL; g_srid[id_] = 0; }
    if (g_sview_w[id_]) { CFRelease(g_sview_w[id_]); g_sview_w[id_] = NULL; g_srid_w[id_] = 0; }
    g_sreg[id_] = *sp; g_sreg_ok[id_] = 1;
    int bn, bw; float k[4]; const uint8_t wrap[3] = {sp->ws, sp->wt, sp->wr};
    if (nvn_border(sp->border, wrap, sp->reduction, &bn, &bw, k)) atomic_store(&g_border_any, 1);
    reg_bump(id_, 1);
    os_unfair_lock_unlock(&g_rmu);
}
// deferred binds' handles are read at execution: every reader of a bind record refreshes it first (command memory is writable)
static inline void bh_fresh(const p_bind_handles *b) {
    if (b->defer && b->src && b->n > 0 && b->n <= 32) memcpy((void *)b->h, (const void *)(uintptr_t)b->src, 8 * (size_t)b->n);
}
static MTLTextureType ttype(int target, int fallback) {
    switch (target) { case 0: return MTLTextureType2D; case 1: case 7: return MTLTextureType2D; case 2: return MTLTextureType3D;
        case 3: return MTLTextureType2DArray; case 4: return MTLTextureType2DArray; case 5: return MTLTextureType2DMultisample;
        case 6: return MTLTextureType2DMultisampleArray; case 8: return MTLTextureTypeCube; case 9: return MTLTextureTypeCubeArray; }
    return (MTLTextureType)fallback;
}
// An ID past the pool is an empty slot (the game binds GetSeparateTextureHandle(0xFFFFF) to unused slots): it reads 0; unregistered IDs inside the pool are still failures.
static int g_tpool_n; static uint64_t g_stat_tex_oob;
// a game may bind the combined handle 0xFFFFFFFF (sampler ID 4095) past its sampler pool
static int g_spool_n; static uint64_t g_stat_samp_oob;
// NVN's lower-left window origin (its default): NDC y = -1 lands on a target's first row, Metal puts +1 there. Rectangles and copies address the same rows either way, so
// lower-left draws get a negative-height viewport (and the opposite winding), and the window image is flipped at present.
static _Atomic int g_origin_ll;
void mtl_set_origin_lower_left(int on) { if (atomic_exchange(&g_origin_ll, on) != on) MLOG("window origin: %s", on ? "lower left (GL: viewport flipped, winding reversed, present flipped)" : "upper left"); }
static int g_queue_origin_ll;   // SET_WINDOW_ORIGIN's raster flip is queue state, independent of later device-origin changes
void mtl_set_queue_origin_lower_left(int on) { g_queue_origin_ll = on != 0; }
void mtl_set_tex_pool_n(int n) { g_tpool_n = n; }
void mtl_set_samp_pool_n(int n) { g_spool_n = n; }
static id<MTLTexture> treg_tex(const treg_t *r) { return r->gone ? tex_made(&r->t) : tex_mtl(&r->t); }
// none for sparse textures (their page table changes too). Caller holds g_rmu.
static void treg_pkg_stamp(treg_t *rp, id<MTLTexture> base) {
    rp->view_span = NULL; if (!rp->view_pkg) return;
    id h = tex_get(rp->t.serial); NvPkg *pk = [h isKindOfClass:[NvPkg class]] ? h : nil;
    if (pk && !pk->sparse && pk->span && pk->tex == base) { rp->view_span = pk->span; rp->view_stamp = pk->stamp; }
}
// *view_pf 0: the texture's own; in_place: the base reads guest memory in place. The rule is at tex_by_id.
static int treg_swizzle(const treg_t *r, MTLPixelFormat base_pf, int in_place, uint8_t sw[4], uint32_t *view_pf) {
    uint8_t fsw[4]; int need_sw = 0; *view_pf = 0;
    sw[0] = MTLTextureSwizzleRed; sw[1] = MTLTextureSwizzleGreen; sw[2] = MTLTextureSwizzleBlue; sw[3] = MTLTextureSwizzleAlpha;
    if (!(r->has_v && r->v.set_format && nvn_fmt_to_mtl(r->v.format, view_pf, fsw) && *view_pf)) nvn_fmt_to_mtl(r->t.d.format, NULL, fsw);
    if (!in_place) for (int c = 0; c < 4; c++) if (fsw[c] >= MTLTextureSwizzleRed) fsw[c] = (uint8_t)(MTLTextureSwizzleRed + c);
    // Metal cannot cast a depth texture to its colour equivalent: the same depth storage, presented as (R, 0, 0, 1).
    if ((base_pf == MTLPixelFormatDepth32Float && *view_pf == MTLPixelFormatR32Float) ||
        (base_pf == MTLPixelFormatDepth16Unorm && *view_pf == MTLPixelFormatR16Unorm)) {
        *view_pf = (uint32_t)base_pf;
        fsw[1] = fsw[2] = MTLTextureSwizzleZero;
        fsw[3] = MTLTextureSwizzleOne;
    }
    if (r->has_v && r->v.set_swizzle) memcpy(sw, r->v.swz, 4);
    else if (r->t.set_swz) for (int c = 0; c < 4; c++) sw[c] = (uint8_t)(r->t.swz >> (3 * c) & 7);
    for (int c = 0; c < 4; c++) {
        if (sw[c] > MTLTextureSwizzleAlpha) sw[c] = (uint8_t)(MTLTextureSwizzleRed + c);
        if (sw[c] >= MTLTextureSwizzleRed) sw[c] = fsw[sw[c] - MTLTextureSwizzleRed];
        if (sw[c] != MTLTextureSwizzleRed + c) need_sw = 1;
    }
    return need_sw;
}
// Caller holds g_rmu through the handoff; the registration owns every non-null result in rp->view.
#define tex_by_id(tid) ((__bridge id<MTLTexture>)tex_by_id_ref(tid))
static void *tex_by_id_ref(int tid) {
    if (g_tpool_n && tid >= g_tpool_n) { if (!g_stat_tex_oob++) MLOG("texture ID %d outside the pool (capacity %d) treated as an empty slot: reads 0", tid, g_tpool_n); return NULL; }
    if (tid < 0 || tid >= g_ntreg || !g_treg[tid].ok) { RED_ONCE(10, "texture ID %d not registered", tid); return NULL; }
    treg_t *rp = &g_treg[tid];
    if (!g_pool_walk) rp->used = g_tu_now;
    uint64_t sg = treg_ep(), dg = nvn_dirty_now();   // generations first: a change in between takes the slow path next time
    if (rp->view && rp->view_ser == sg && (!rp->view_pkg || rp->view_dirty == dg)) return rp->view;
    if (rp->view && rp->view_ser == sg && rp->view_span && !nvn_dirty_since_span(rp->view_span, rp->t.mem, rp->t.L.size, rp->view_stamp)) { rp->view_dirty = dg; return rp->view; }
    id<MTLTexture> v = (__bridge id<MTLTexture>)rp->view;
    rp->view_pkg = (rp->t.has_layout || tex_cpu_written(&rp->t)) && rp->t.mem;
    if (v) {
        id<MTLTexture> cur = treg_tex(rp);
        // cur nil (no texture now) must not match a parentless cached base: its released resource ID would reach the tables (docs/reference/rendering.md, "A texture ID with no texture kept its cached view")
        if (cur && (cur == v || v.parentTexture == cur)) { rp->view_ser = sg; rp->view_dirty = dg; treg_pkg_stamp(rp, cur); return rp->view; }
        defer_obj_unref(v); CFRelease(rp->view); rp->view = NULL; rp->view_rid = 0; rp->view_pf = 0; rp->view_tt = 0; v = nil;
    }
    treg_t r = *rp;
    id<MTLTexture> base = treg_tex(rp);
    if (!base) {
        if (rp->gone) {
            static uint64_t n;
            if (n++ < 4) MLOG("texture ID %d read after its memory pool was finalized ser=%llu %ux%u format %#x: treated as an empty slot (%llu so far)", tid, (unsigned long long)rp->t.serial, rp->t.d.width, rp->t.d.height, rp->t.d.format, (unsigned long long)n);
        }
        return NULL;
    }
    v = base;
    // The NVN swizzle names the NVN format's components, mapped through nvn_fmt_to_mtl's own channel mapping. The RGB565 red / blue swap applies only to textures read in place
    // (guest bit order); other host textures hold Metal's order, so only the format's alpha 1 remains.
    uint8_t sw[4]; uint32_t view_pf = 0; int need_sw = treg_swizzle(&r, base.pixelFormat, base.buffer != nil, sw, &view_pf);
    // Stencil mode reads the stencil through Metal's stencil-only format; the shader declares float and takes the raw bits.
    int want_st = r.has_v && r.v.set_dsmode ? r.v.dsmode == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : r.t.stencil;
    int stv = want_st && base.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (want_st && !stv)
        RED_ONCE(104, "stencil-mode texture view of Metal format %lu (texture ID %d): no stencil-only view, depth is read", (unsigned long)base.pixelFormat, tid);
    if ((r.has_v && (r.v.set_format || r.v.set_levels || r.v.set_layers || r.v.set_target)) || need_sw || stv) {
        if (!r.has_v) memset(&r.v, 0, sizeof r.v);
        uint32_t pf = view_pf ? view_pf : (uint32_t)base.pixelFormat;
        if (stv) pf = MTLPixelFormatX32_Stencil8;
        if (r.v.set_format && nvn_fmt_bc(rp->t.d.format) && !nvn_fmt_bc(r.v.format)) {
            RED_ONCE(86, "Metal cannot create the raw-block view of a compressed texture (NVN format %#x → view %#x): treated as an empty slot", rp->t.d.format, r.v.format);
            return NULL;
        }
        if (r.v.set_format && tex_need_pfv(base.pixelFormat, (MTLPixelFormat)pf) && !(base.usage & MTLTextureUsagePixelFormatView)) {
            base = tex_upgrade_pfv(&rp->t, base, (MTLPixelFormat)pf);
            sg = treg_ep();
        }
        // view levels count from level 0, which a sparse texture's host texture may start past (tex_level_shift)
        NSUInteger lb = tex_level_shift(&rp->t, base), n = base.mipmapLevelCount + lb;
        NSUInteger l0 = r.v.set_levels ? r.v.base_level : 0;
        if (l0 >= n) { RED_ONCE(109, "texture view id %d starts at mip %lu beyond %lu levels", tid, (unsigned long)l0, (unsigned long)n); return NULL; }
        NSUInteger nl = r.v.set_levels && r.v.nlevels ? r.v.nlevels : n - l0;
        // clipped to the levels it has, as deko3d's descriptor writer does (Metal's validation refuses more)
        if (nl > n - l0) nl = n - l0;
        if (l0 < lb) { NSUInteger end = l0 + nl; l0 = lb; nl = end > lb ? end - lb : 1; }
        l0 -= lb;
        NSUInteger s0 = r.v.set_layers ? r.v.min_layer : 0;
        NSUInteger ns = base.textureType == MTLTextureType3D ? 1 : (r.v.set_layers && r.v.nlayers ? r.v.nlayers : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1) - s0);
        MTLTextureType tt = r.v.set_target ? ttype(r.v.target, base.textureType) : base.textureType;
        if (tt == MTLTextureType2D && ns > 1) ns = 1;
        // a cube view of partial cubes counts layers in cubes, as the hardware's header does; Metal counts faces and aborts on a 1-slice cube view
        if ((tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray) && ns % 6) {
            NSUInteger total = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
            int fits = (s0 + ns) * 6 <= total;
            static int noted; if (fits && !noted++) MLOG("[nvn-mtl] cube texture view of %u + %u layers (texture ID %d, %lu faces): counted in cubes, as the hardware does", (unsigned)s0, (unsigned)ns, tid, (unsigned long)total);
            if (!fits) RED_ONCE(110, "cube texture view of %u + %u layers past the %lu faces of texture ID %d: read as a 2D array", (unsigned)s0, (unsigned)ns, (unsigned long)total, tid);
            if (fits) { s0 *= 6; ns *= 6; } else tt = MTLTextureType2DArray;
        }
        vc_key k; memset(&k, 0, sizeof k); k.pf = pf; k.tt = (uint8_t)tt; k.l0 = (uint32_t)l0; k.nl = (uint32_t)nl; k.s0 = (uint32_t)s0; k.ns = (uint32_t)ns; memcpy(k.sw, sw, 4);
        if (!(v = vc_get(base, &k))) {
            id<MTLTexture> root = base; NSUInteger rl = l0, rs = s0;
            while (root.parentTexture) { rl += root.parentRelativeLevel; rs += root.parentRelativeSlice; root = root.parentTexture; }
            v = need_sw ? [root newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(rl, nl) slices:NSMakeRange(rs, ns) swizzle:MTLTextureSwizzleChannelsMake(sw[0], sw[1], sw[2], sw[3])]
                        : [root newTextureViewWithPixelFormat:(MTLPixelFormat)pf textureType:tt levels:NSMakeRange(rl, nl) slices:NSMakeRange(rs, ns)];
            if (!v) { RED_ONCE(11, "cannot create texture view id %d", tid); v = base; }
            else vc_put(base, rp->t.pool, &k, v);
        }
    }
    if (rp->retired && rp->transient_sub != g_sub_serial) {
        if (g_ntreg_transient == g_treg_transient_cap) {
            g_treg_transient_cap = g_treg_transient_cap ? g_treg_transient_cap * 2 : 64;
            g_treg_transient = dm_realloc(g_treg_transient, g_treg_transient_cap * sizeof *g_treg_transient, "the transient texture registrations");
        }
        g_treg_transient[g_ntreg_transient++] = tid; rp->transient_sub = g_sub_serial;
    }
    rp->view = (void *)CFBridgingRetain(v); rp->view_rid = v.gpuResourceID._impl; rp->view_tt = (uint8_t)v.textureType; rp->view_pf = (uint32_t)v.pixelFormat; rp->view_ser = sg; rp->view_dirty = dg; treg_pkg_stamp(rp, base);
    if (rp->stand) { treg_drop_stand(rp); g_stand_done++; }
    return rp->view;
}
// A declared type other than the descriptor's: a compatible view. Maxwell's instruction carries its dimension; Metal reads a mismatched type undefined. 2D in a cube or 3D slot
// reads zero; any other mismatch is reported once. Caller holds g_rmu.
#define DMY_NT 10
static id<MTLTexture> dummy_tex(int dtt, int dpf); static uint64_t g_dummy_tex_rid[DMY_NT][4];
// 0 when no view bridges the types. 2D, 2D array, cube and cube array are all layers to Maxwell; the declared type only says how many. A Metal cube's arrayLength is 1,
// a cube array's counts cubes.
static NSUInteger fit_layers(id<MTLTexture> t, MTLTextureType want) {
    MTLTextureType have = t.textureType;
    NSUInteger layers = have == MTLTextureType2D ? 1 : have == MTLTextureType2DArray ? t.arrayLength : have == MTLTextureTypeCube || have == MTLTextureTypeCubeArray ? t.arrayLength * 6 : 0;
    if (!layers) return 0;
    if (want == MTLTextureType2D) return 1;
    if (want == MTLTextureType2DArray) return layers;
    if (want == MTLTextureTypeCube && layers >= 6) return 6;
    if (want == MTLTextureTypeCubeArray && layers >= 6) return layers / 6 * 6;
    return 0;
}
static id<MTLTexture> tex_fit(int tid, id<MTLTexture> t, MTLTextureType want, uint64_t *rid) {
    MTLTextureType have = t.textureType; NSUInteger ns = fit_layers(t, want);
    // a 2D texture (or a 2D array under 6 layers) in a cube slot: the zero cube, a defined read
    if (!ns && (want == MTLTextureTypeCube || want == MTLTextureTypeCubeArray) && (have == MTLTextureType2D || (have == MTLTextureType2DArray && t.arrayLength < 6))) {
        // uniform-controlled branches can leave unused cube inputs holding 2D descriptors: informational
        static int noted; if (!noted++) MLOG("cube binding fallback: texture ID %d has a 2D descriptor; zero cube bound (the shader may leave this input unused)", tid);
        id<MTLTexture> z = dummy_tex((int)want, NVN_DPF_FLOAT); *rid = g_dummy_tex_rid[want % DMY_NT][NVN_DPF_FLOAT]; return z; }
    // a 2D texture in a 3D slot: the zero volume (Metal makes 3D views only of 3D textures); what the Switch reads there is not known
    if (!ns && want == MTLTextureType3D && (have == MTLTextureType2D || have == MTLTextureType2DArray)) {
        static int noted; if (!noted++) MLOG("3D binding fallback: texture ID %d is %s %lux%lu (Metal format %lu); zero 3D texture bound", tid, have == MTLTextureType2D ? "2D" : "a 2D array",
                                             (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat);
        id<MTLTexture> z = dummy_tex((int)want, NVN_DPF_FLOAT); *rid = g_dummy_tex_rid[want % DMY_NT][NVN_DPF_FLOAT]; return z; }
    if (!ns || tid <= 0 || tid >= g_ntreg) return nil;
    treg_t *r = &g_treg[tid];
    if (r->alt && r->alt_of == (__bridge void *)t && r->alt_type == (uint8_t)want) { *rid = r->alt_rid; return (__bridge id<MTLTexture>)r->alt; }
    if (r->alt) { defer_obj_unref((__bridge id)r->alt); CFRelease(r->alt); r->alt = r->alt_of = NULL; r->alt_rid = 0; }
    // Made from the texture under t: Metal takes a view's residency from its parent, and a view of a view reads as not resident under shader validation.
    id<MTLTexture> base = t; NSUInteger l0 = 0, s0 = 0;
    while (base.parentTexture) { l0 += base.parentRelativeLevel; s0 += base.parentRelativeSlice; base = base.parentTexture; }
    id<MTLTexture> v = [base newTextureViewWithPixelFormat:t.pixelFormat textureType:want levels:NSMakeRange(l0, t.mipmapLevelCount) slices:NSMakeRange(s0, ns) swizzle:t.swizzle];
    if (!v) { RED_ONCE(77, "cannot create a type-compatible texture view: %lu → %lu (texture ID %d)", (unsigned long)have, (unsigned long)want, tid); return nil; }
    r->alt = (void *)CFBridgingRetain(v); r->alt_of = (__bridge void *)t; r->alt_type = (uint8_t)want; r->alt_rid = v.gpuResourceID._impl;
    *rid = r->alt_rid; return v;
}
// Handle array elements are declared as the picked one's type; a 2D texture in a cube array is one the shader does not pick there: nil, no mismatch report.
static id<MTLTexture> tex_fit_elem(int tid, id<MTLTexture> t, MTLTextureType want, uint64_t *rid) {
    if ((want == MTLTextureTypeCube || want == MTLTextureTypeCubeArray) && (t.textureType == MTLTextureType2D || (t.textureType == MTLTextureType2DArray && t.arrayLength < 6))) return nil;
    return tex_fit(tid, t, want, rid);
}
// A pool table of a layered type holds the view tex_fit would make, for an ID a shader read through that type (the host keeps cubes as 2D arrays); what no view serves holds
// the dummy. One view per table type: a table stands over submissions, so one type's view must not go when another takes the ID. Caller holds g_rmu.
static int pool_lk(int tt) { return tt == MTLTextureType2D ? 0 : tt == MTLTextureType2DArray ? 1 : tt == MTLTextureTypeCube ? 2 : tt == MTLTextureTypeCubeArray ? 3 : -1; }
static int pool_bridge_pair(int have, int want) { return have != want && pool_lk(have) >= 0 && pool_lk(want) >= 0; }
static uint64_t pool_fit(int id_, id<MTLTexture> t, int dtt) {
    int k = pool_lk(dtt);
    if (id_ <= 0 || id_ >= g_ntreg || !t || k < 0) return 0;
    treg_t *r = &g_treg[id_];
    id<MTLTexture> base = t; NSUInteger l0 = 0, s0 = 0;   // made from the texture under t (residency)
    while (base.parentTexture) { l0 += base.parentRelativeLevel; s0 += base.parentRelativeSlice; base = base.parentTexture; }
    if (r->palt[k] && r->palt_of[k] == (__bridge void *)base) { id<MTLTexture> v = (__bridge id<MTLTexture>)r->palt[k];
        if (v.parentRelativeLevel == l0 && v.parentRelativeSlice == s0 && v.mipmapLevelCount == t.mipmapLevelCount && v.pixelFormat == t.pixelFormat) return r->palt_rid[k]; }
    if (r->palt[k]) { defer_obj_unref((__bridge id)r->palt[k]); CFRelease(r->palt[k]); r->palt[k] = r->palt_of[k] = NULL; r->palt_rid[k] = 0; }
    NSUInteger ns = fit_layers(t, (MTLTextureType)dtt); if (!ns) return 0;
    if ((dtt == MTLTextureTypeCube || dtt == MTLTextureTypeCubeArray) && t.width != t.height) return 0;   // a cube's faces are square
    id<MTLTexture> v = [base newTextureViewWithPixelFormat:t.pixelFormat textureType:(MTLTextureType)dtt levels:NSMakeRange(l0, t.mipmapLevelCount) slices:NSMakeRange(s0, ns) swizzle:t.swizzle];
    if (!v) { RED_ONCE(169, "texture pool table: cannot create a type-compatible texture view: %lu → %d (texture ID %d)", (unsigned long)t.textureType, dtt, id_); return 0; }
    r->palt[k] = (void *)CFBridgingRetain(v); r->palt_of[k] = (__bridge void *)base; r->palt_rid[k] = v.gpuResourceID._impl;
    return r->palt_rid[k];
}
static uint32_t g_pool_tlog[(1u << 20) / 32]; static unsigned g_pool_tlog_n;
static void pool_type_note(int id_, int have, int want) {
    if (id_ <= 0 || id_ >= (1 << 20) || !pool_seen_t(id_, want) || (g_pool_tlog[id_ >> 5] >> (id_ & 31) & 1) || g_pool_tlog_n >= 256) return;
    g_pool_tlog[id_ >> 5] |= 1u << (id_ & 31); g_pool_tlog_n++;
    const tex_t *t = id_ < g_ntreg ? &g_treg[id_].t : NULL;
    MLOG("[RED] [pool-type] texture ID %d (ser=%llu %ux%u format %#x target %d) is Metal type %d, read through a type %d texture pool table: left out (dummy), no view serves it", id_,
         t ? (unsigned long long)t->serial : 0ull, t ? t->d.width : 0, t ? t->d.height : 0, t ? t->d.format : 0, t ? t->d.target : -1, have, want);
}
// reported once per shader and slot; for a buffer texture, its element count and offset misalignment. Caller holds g_rmu.
static uint64_t g_tex_mis_n, g_tex_mis_shown;
uint64_t mtl_tex_mis_count(void) { return g_tex_mis_n; }
static void tex_mis_note(NvShader *sh, int stage, int slot, int tid, id<MTLTexture> t, MTLTextureType want, const char *outcome) {
    g_tex_mis_n++;
    if (slot < 0 || slot >= 256 || (sh->mis[slot >> 6] >> (slot & 63) & 1)) return;
    sh->mis[slot >> 6] |= 1ull << (slot & 63);
    if (g_tex_mis_shown++ >= 64) return;
    const tex_t *tt = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid].t : NULL;
    char bt[96] = "";
    if (t.textureType == MTLTextureTypeTextureBuffer) snprintf(bt, sizeof bt, ", %lu elements at pool offset %lu, %lu past the linear-texture alignment", (unsigned long)t.width, (unsigned long)t.bufferOffset,
                                                                (unsigned long)(t.bufferOffset % [g_dev minimumLinearTextureAlignmentForPixelFormat:t.pixelFormat]));
    MLOG("[RED] texture type mismatch: shader %s (stage %d) declares slot %d as Metal type %lu, texture ID %d is type %lu (NVN target %d, format %#x, Metal format %lu%s); %s", sh->key, stage, slot,
         (unsigned long)want, tid, (unsigned long)t.textureType, tt ? (int)tt->d.target : -1, tt ? (unsigned)tt->d.format : 0u, (unsigned long)t.pixelFormat, bt, outcome);
}
static int mtl_pf_dpf(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 13: case 23: case 33: case 53: case 63: case 73: case 91: case 103: case 113: case 123: return NVN_DPF_UINT;
        case 14: case 24: case 34: case 54: case 64: case 74: case 104: case 114: case 124: return NVN_DPF_SINT;
    }
    return NVN_DPF_FLOAT;
}
static inline int img_needs_class_view_pf(MTLTextureType tt, MTLPixelFormat pf, int dpf) {
    if (dpf == NVN_DPF_FLOAT && tt != MTLTextureTypeTextureBuffer) return 0;
    return dpf == NVN_DPF_FLOAT ? tbuf_view_format(pf, dpf) != pf : mtl_pf_dpf(pf) != dpf;
}
static int img_needs_class_view(id<MTLTexture> t, int dpf) { return img_needs_class_view_pf(t.textureType, t.pixelFormat, dpf); }
// On the Switch an image's declared format decides how its bits are read: an image of another class gets a same-size view in the shader's class (else undefined values).
// A texture without the format-view usage is upgraded before the next pass. Caller holds g_rmu.
static int g_imfit_pend[16];
static void img_fit_upgrade(void) {
    os_unfair_lock_lock(&g_rmu);
    for (int i = 0; i < g_imfit_npend; i++) { int tid = g_imfit_pend[i];
        if (tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok || g_treg[tid].gone) continue;
        id<MTLTexture> b = tex_mtl_replay(&g_treg[tid].t); if (b.parentTexture) b = b.parentTexture;
        if (b && !(b.usage & MTLTextureUsagePixelFormatView)) tex_upgrade_pfv(&g_treg[tid].t, b, MTLPixelFormatR32Uint); }
    g_imfit_npend = 0;
    os_unfair_lock_unlock(&g_rmu);
}
// The image view takes no channel mapping (Metal gives a swizzled view read usage only), the shader's declared type (Metal drops stores to a mismatched type), and an sRGB
// texture's linear twin: NVN's image access converts no sRGB, Metal's would encode twice.
static id<MTLTexture> img_fit(int tid, id<MTLTexture> t, int dpf, MTLTextureType want, uint64_t *rid) {
    size_t n = mtl_pf_bytes(t.pixelFormat); int u = dpf == NVN_DPF_UINT, cls = img_needs_class_view(t, dpf);
    const char *class_name = dpf == NVN_DPF_FLOAT ? "float" : u ? "uint" : "int";
    MTLPixelFormat lin = pf_srgb_linear(t.pixelFormat);
    MTLPixelFormat pf = !cls ? (lin != MTLPixelFormatInvalid ? lin : t.pixelFormat) : dpf == NVN_DPF_FLOAT ? tbuf_view_format(t.pixelFormat, dpf)
                      : n == 1 ? (u ? MTLPixelFormatR8Uint : MTLPixelFormatR8Sint) : n == 2 ? (u ? MTLPixelFormatR16Uint : MTLPixelFormatR16Sint) : n == 4 ? (u ? MTLPixelFormatR32Uint : MTLPixelFormatR32Sint)
                      : n == 8 ? (u ? MTLPixelFormatRG32Uint : MTLPixelFormatRG32Sint) : n == 16 ? (u ? MTLPixelFormatRGBA32Uint : MTLPixelFormatRGBA32Sint) : MTLPixelFormatInvalid;
    id<MTLTexture> base = t; NSUInteger l0 = 0, s0 = 0;
    while (base.parentTexture) { l0 += base.parentRelativeLevel; s0 += base.parentRelativeSlice; base = base.parentTexture; }
    if (pf == MTLPixelFormatInvalid || tid <= 0 || tid >= g_ntreg) {
        RED_ONCE(106, "image read as %s in the shader, texture of Metal format %lu (texture ID %d): no view of that class, bound as is (reads undefined)", class_name, (unsigned long)t.pixelFormat, tid); return nil; }
    NSUInteger ns = t.textureType != want ? fit_layers(t, want) : t.textureType == MTLTextureType3D ? 1 : t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    if (!ns) {
        RED_ONCE(157, "image slot declared as Metal type %lu holds texture ID %d of type %lu (Metal format %lu, %lux%lu): no view bridges them, bound as is (its accesses are undefined, its stores lost)",
                 (unsigned long)want, tid, (unsigned long)t.textureType, (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height); return nil; }
    if (cls && !(base.usage & MTLTextureUsagePixelFormatView)) {   // format-view usage costs lossless compression, so textures get it only when needed: upgraded before the next
        int seen = 0; for (int i = 0; i < g_imfit_npend; i++) seen |= g_imfit_pend[i] == tid;   // draw pass opens, as the upgrade's copy cannot run inside this pass;
        if (!seen && g_imfit_npend < 16) g_imfit_pend[g_imfit_npend++] = tid;                  // until then the slot is bound as is
        return nil; }
    treg_t *r = &g_treg[tid];
    if (r->im && r->im_of == (__bridge void *)t && ((__bridge id<MTLTexture>)r->im).pixelFormat == pf && ((__bridge id<MTLTexture>)r->im).textureType == want) { *rid = r->im_rid; return (__bridge id<MTLTexture>)r->im; }
    if (r->im) { defer_obj_unref((__bridge id)r->im); CFRelease(r->im); r->im = r->im_of = NULL; r->im_rid = 0; }
    vc_key k; memset(&k, 0, sizeof k); k.pf = (uint32_t)pf; k.tt = (uint8_t)want; k.l0 = (uint32_t)l0; k.nl = (uint32_t)t.mipmapLevelCount; k.s0 = (uint32_t)s0; k.ns = (uint32_t)ns;
    k.sw[0] = MTLTextureSwizzleRed; k.sw[1] = MTLTextureSwizzleGreen; k.sw[2] = MTLTextureSwizzleBlue; k.sw[3] = MTLTextureSwizzleAlpha;
    id<MTLTexture> v = vc_get(base, &k);
    if (!v && (v = [base newTextureViewWithPixelFormat:pf textureType:want levels:NSMakeRange(l0, t.mipmapLevelCount) slices:NSMakeRange(s0, ns)])) vc_put(base, r->t.pool, &k, v);
    if (!v) { RED_ONCE(107, "cannot create the image view of class %s, Metal type %lu (texture ID %d, Metal format %lu, type %lu)", !cls ? "unchanged" : class_name, (unsigned long)want, tid, (unsigned long)t.pixelFormat, (unsigned long)t.textureType); return nil; }
    { static int once; if (cls && !once++) MLOG("image read in another class than its texture: first case texture ID %d Metal format %lu → %lu", tid, (unsigned long)t.pixelFormat, (unsigned long)pf); }
    { static int once; if (t.textureType != want && !once++) MLOG("image slot of another type than its texture: first case texture ID %d, Metal type %lu → %lu, %lu layers", tid, (unsigned long)t.textureType, (unsigned long)want, (unsigned long)ns); }
    { static int once; if (!cls && pf != t.pixelFormat && !once++) MLOG("sRGB texture bound as an image: loaded and stored raw through a view of its linear format, first case texture ID %d Metal format %lu → %lu", tid, (unsigned long)t.pixelFormat, (unsigned long)pf); }
    r->im = (void *)CFBridgingRetain(v); r->im_of = (__bridge void *)t; r->im_rid = v.gpuResourceID._impl;
    *rid = r->im_rid; return v;
}
// A 32-bit integer texture in a float slot is undefined in Metal (one GPU hands the bits over, another reads wrong values): the same-size float view. Caller holds g_rmu.
static id<MTLTexture> tex_float_fit(int tid, id<MTLTexture> t, uint64_t *rid) {
    MTLPixelFormat pf = tbuf_view_format(t.pixelFormat, NVN_DPF_FLOAT);
    if (pf == t.pixelFormat || tid <= 0 || tid >= g_ntreg || t.textureType == MTLTextureTypeTextureBuffer) return nil;
    id<MTLTexture> base = t.parentTexture ? t.parentTexture : t;
    if (!(base.usage & MTLTextureUsagePixelFormatView)) {
        int seen = 0; for (int i = 0; i < g_imfit_npend; i++) seen |= g_imfit_pend[i] == tid;
        if (!seen && g_imfit_npend < 16) g_imfit_pend[g_imfit_npend++] = tid;
        return nil; }
    treg_t *r = &g_treg[tid];
    if (r->im && r->im_of == (__bridge void *)t && ((__bridge id<MTLTexture>)r->im).pixelFormat == pf && ((__bridge id<MTLTexture>)r->im).textureType == t.textureType) { *rid = r->im_rid; return (__bridge id<MTLTexture>)r->im; }
    if (r->im) { defer_obj_unref((__bridge id)r->im); CFRelease(r->im); r->im = r->im_of = NULL; r->im_rid = 0; }
    NSUInteger l0 = t.parentTexture ? t.parentRelativeLevel : 0, s0 = t.parentTexture ? t.parentRelativeSlice : 0;
    NSUInteger ns = t.textureType == MTLTextureType3D ? 1 : t.arrayLength * (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    id<MTLTexture> v = [base newTextureViewWithPixelFormat:pf textureType:t.textureType levels:NSMakeRange(l0, t.mipmapLevelCount) slices:NSMakeRange(s0, ns) swizzle:t.swizzle];
    if (!v) { RED_ONCE(100, "cannot create the float view of an integer texture (texture ID %d, Metal format %lu)", tid, (unsigned long)t.pixelFormat); return nil; }
    { static int once; if (!once++) MLOG("integer texture sampled as float: first case texture ID %d Metal format %lu read through format %lu", tid, (unsigned long)t.pixelFormat, (unsigned long)pf); }
    r->im = (void *)CFBridgingRetain(v); r->im_of = (__bridge void *)t; r->im_rid = v.gpuResourceID._impl;
    *rid = r->im_rid; return v;
}
// tt / pf (may be NULL): the view's type and pixel format, from the registration when it holds the view, with no message per texture (encode_args_full, pool_entry)
static inline __attribute__((always_inline)) uint64_t tex_fast_rid_meta(int tid, void **out, uint8_t *tt, uint32_t *pf) {
    uint64_t ep = treg_ep();   // first: a sync clears view_ser of the IDs whose serials changed, which the test below must see
    if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && g_treg[tid].view && g_treg[tid].view_ser == ep && (!g_treg[tid].view_pkg || g_treg[tid].view_dirty == nvn_dirty_now())) {
        if (!g_pool_walk) g_treg[tid].used = g_tu_now;
        if (out) *out = g_treg[tid].view;
        if (tt) *tt = g_treg[tid].view_tt;
        if (pf) *pf = g_treg[tid].view_pf;
        return g_treg[tid].view_rid; }
    id<MTLTexture> t = tex_by_id(tid); if (out) *out = (__bridge void *)t;
    if (t && tid > 0 && tid < g_ntreg && (__bridge void *)t == g_treg[tid].view) {
        if (tt) *tt = g_treg[tid].view_tt;
        if (pf) *pf = g_treg[tid].view_pf;
        return g_treg[tid].view_rid; }
    if (t) {
        if (tt) *tt = (uint8_t)t.textureType;
        if (pf) *pf = (uint32_t)t.pixelFormat;
        return t.gpuResourceID._impl; }
    return 0;
}
static inline __attribute__((always_inline)) uint64_t tex_fast_rid(int tid, void **out) { return tex_fast_rid_meta(tid, out, NULL, NULL); }
static MTLSamplerAddressMode wrapm(int w) {
    switch (w) { case 0: case 7: return MTLSamplerAddressModeClampToEdge; case 1: return MTLSamplerAddressModeRepeat;
        case 3: case 2: return MTLSamplerAddressModeMirrorClampToEdge; case 5: case 4: return MTLSamplerAddressModeClampToBorderColor;
        case 6: return MTLSamplerAddressModeMirrorRepeat; }
    return MTLSamplerAddressModeRepeat;
}
static id<MTLSamplerState> samp_by_id(int sid) {
    if (g_spool_n && sid >= g_spool_n) { if (!g_stat_samp_oob++) MLOG("sampler ID %d outside the pool (capacity %d) treated as an empty slot", sid, g_spool_n); return nil; }
    if (sid < 0 || sid >= g_nsreg || !g_sreg_ok[sid]) { RED_ONCE(12, "sampler ID %d not registered", sid); return nil; }
    if (g_sview[sid]) return (__bridge id<MTLSamplerState>)g_sview[sid];
    samp_par p = g_sreg[sid];
    MTLSamplerDescriptor *d = [MTLSamplerDescriptor new];
    d.magFilter = p.magf ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.minFilter = (p.minf == 1 || p.minf == 3 || p.minf == 5) ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
    d.mipFilter = p.minf <= 1 ? MTLSamplerMipFilterNotMipmapped : (p.minf == 2 || p.minf == 3) ? MTLSamplerMipFilterNearest : MTLSamplerMipFilterLinear;
    d.sAddressMode = wrapm(p.ws); d.tAddressMode = wrapm(p.wt); d.rAddressMode = wrapm(p.wr);
    d.lodMinClamp = p.lod_min < 0 ? 0 : p.lod_min; d.lodMaxClamp = p.lod_max;
    d.lodBias = p.lod_bias < -16 ? -16 : p.lod_bias > 15.99f ? 15.99f : p.lod_bias;   // Metal 26's range [-16, 15.99]
    d.maxAnisotropy = p.aniso < 1 ? 1 : p.aniso > 16 ? 16 : (NSUInteger)p.aniso;
    if (p.cmp_mode) d.compareFunction = (MTLCompareFunction)(p.cmp_func - 1);
    // Reduction (1 min / 2 max, Metal 26). Metal ignores it with a nearest or no mip filter, so such samplers filter mips linearly (the same level at integer LODs).
    if (p.reduction == 1 || p.reduction == 2) {
        d.reductionMode = p.reduction == 1 ? MTLSamplerReductionModeMinimum : MTLSamplerReductionModeMaximum;
        if (d.mipFilter == MTLSamplerMipFilterNotMipmapped) { d.lodMinClamp = 0; d.lodMaxClamp = 0; }
        d.mipFilter = MTLSamplerMipFilterLinear;
    }
    int bn, bw; float k[4]; const uint8_t wrap[3] = { p.ws, p.wt, p.wr }; int twin = nvn_border(p.border, wrap, p.reduction, &bn, &bw, k);
    d.borderColor = (MTLSamplerBorderColor)bn;
    d.supportArgumentBuffers = YES;
    id<MTLSamplerState> s = [g_dev newSamplerStateWithDescriptor:d];
    if (s) { g_sview[sid] = (void *)CFBridgingRetain(s); g_srid[sid] = s.gpuResourceID._impl; }
    if (s && twin) {
        d.borderColor = (MTLSamplerBorderColor)bw; id<MTLSamplerState> t = [g_dev newSamplerStateWithDescriptor:d];
        if (t) { g_sview_w[sid] = (void *)CFBridgingRetain(t); g_srid_w[sid] = t.gpuResourceID._impl; memcpy(g_sbk[sid], k, sizeof k); }
        static int nlog; if (nlog++ < 16) MLOG("sampler ID %d: border colour %g,%g,%g,%g, which Metal lacks: Metal colour %d, corrected in the shaders through a twin of colour %d", sid, p.border[0], p.border[1], p.border[2], p.border[3], bn, bw);
    }
    return s;
}
static inline __attribute__((always_inline)) void *samp_fast(int sid) {
    if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid] && g_sview[sid]) return g_sview[sid];
    return (__bridge void *)samp_by_id(sid);
}

@interface NvArena : NSObject { @public id<MTLBuffer> buf; uint8_t *cpu; uint64_t gpu, fence; size_t used, len; } @end
@implementation NvArena @end
static NSMutableArray<NvArena *> *g_arenas; static NvArena *g_cur_arena; static NSMutableArray<NvArena *> *g_sub_arenas;
// The next submission goes on in the block the last one ended in while it has room, so blocks follow the bytes in flight, not the submissions. DM_NVN_ARENA_SHARE=0: a block each.
static NvArena *g_arena_next;
static int arena_share_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_NVN_ARENA_SHARE"); on = !e || atoi(e); }
    return on;
}
static int arena_trim_on(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_NVN_ARENA_TRIM"); on = !e || atoi(e); }
    return on;
}
static NvArena *arena_new(size_t minimum) {
    // The for-in variable does not retain: take a strong reference before removing it. The smallest that fits, last used first, so blocks a peak made go once idle.
    NvArena *r = nil;
    for (NvArena *it in g_arenas) if (it->fence && it->fence <= g_done && it->len >= minimum && (!r || it->len < r->len || (it->len == r->len && it->fence > r->fence))) r = it;
    if (r) { if (r == g_arena_next) g_arena_next = nil; [g_arenas removeObjectIdenticalTo:r]; r->fence = 0; r->used = 0; return r; }
    NvArena *a = [NvArena new]; a->buf = [g_dev newBufferWithLength:MAX(minimum, 4u << 20) options:MTLResourceStorageModeShared];
    if (!a->buf) MDIE("cannot allocate %zu bytes of frame scratch", minimum);
    a->gpu = a->buf.gpuAddress; a->len = a->buf.length; a->cpu = a->buf.contents; res_add(a->buf); g_arena_made++; g_arena_live++; g_arena_live_bytes += a->len; return a;
}
// Arenas above ARENA_KEEP (and usual blocks past ARENA_KEEP_BLOCKS) go after ARENA_IDLE idle submissions, never before their last fence nor while written. DM_NVN_ARENA_TRIM=0 keeps them.
enum { ARENA_KEEP = 32 << 20, ARENA_IDLE = 600, ARENA_KEEP_BLOCKS = 4 };
static unsigned arena_trim(uint64_t idle) {
    uint64_t done = g_done; int trim = arena_trim_on(); unsigned n = 0, usual = 0;
    for (NvArena *a in g_arenas) usual += a->len <= ARENA_KEEP;
    for (NvArena *a in g_sub_arenas) usual += a->len <= ARENA_KEEP;
    for (NSUInteger i = g_arenas.count; i-- > 0;) { NvArena *a = g_arenas[i];
        if (!a->fence || a->fence > done || g_fence < a->fence + idle) continue;
        int big = a->len > ARENA_KEEP;
        if (!big && (!trim || usual <= ARENA_KEEP_BLOCKS)) continue;
        if (a == g_arena_next) g_arena_next = nil;
        res_remove(a->buf); g_arena_live--; g_arena_live_bytes -= a->len; n++;
        g_arena_freed++; g_arena_freed_bytes += a->len; if (!big) usual--;
        [g_arenas removeObjectAtIndex:i];
    }
    return n;
}
void arena_begin(void) {
    if (!g_arenas) { g_arenas = [NSMutableArray array]; g_sub_arenas = [NSMutableArray array]; }
    g_cur_arena = nil;
    arena_trim(ARENA_IDLE);
    if (g_arena_next) { NvArena *a = g_arena_next; g_arena_next = nil; [g_arenas removeObjectIdenticalTo:a]; [g_sub_arenas addObject:a]; g_cur_arena = a; }
}
void arena_commit(uint64_t fence) {
    for (NvArena *a in g_sub_arenas) { a->fence = fence; [g_arenas addObject:a]; }
    [g_sub_arenas removeAllObjects];
    g_arena_next = arena_share_on() && g_cur_arena && g_cur_arena->len <= ARENA_KEEP && g_cur_arena->used + 256 <= g_cur_arena->len ? g_cur_arena : nil;
    g_cur_arena = nil;
}
// An unqualified id * out-parameter is __autoreleasing under ARC: __strong avoids autoreleasing the arena buffer each allocation (docs/reference/frame-rate.md, Arena buffer ownership on allocation).
// Use the addresses arena_take returns, never g_cur_arena after it: any allocation in between can roll it over to a new block.
typedef struct { size_t off; uint8_t *cpu; uint64_t gpu; } arena_at;
static arena_at arena_take(size_t n, __strong id<MTLBuffer> *buf) {
    n = (n + 255) & ~(size_t)255;
    if (!g_cur_arena || g_cur_arena->used + n > g_cur_arena->len) {
        pthread_mutex_lock(&g_mu); g_cur_arena = arena_new(n); pthread_mutex_unlock(&g_mu);
        [g_sub_arenas addObject:g_cur_arena];
    }
    NvArena *a = g_cur_arena; size_t off = a->used; a->used += n; *buf = a->buf;
    return (arena_at){off, a->cpu + off, a->gpu + off};
}
static size_t arena_alloc(size_t n, __strong id<MTLBuffer> *buf) { return arena_take(n, buf).off; }
