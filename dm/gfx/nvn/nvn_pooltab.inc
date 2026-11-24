// Dummy textures, buffer texture leads, texture pool tables and their patches, stand-ins, idle-texture sweeps, the texture use clock.
// An unbound resource reads 0 through an all-zero dummy (NVN descriptors 0..255 are the driver's; handle 0 is an empty slot).
static uint64_t g_dummy_img_rid[8][4], g_dummy_tbuf_rid[4];
static id<MTLTexture> dummy_tex(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[DMY_NT][4];
    MTLTextureType tt = (MTLTextureType)dtt;
    MTLPixelFormat pf = dpf == NVN_DPF_DEPTH ? MTLPixelFormatDepth32Float : dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA8Unorm;
    id<MTLTexture> t = cache[dtt % DMY_NT][dpf & 3];
    if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt; d.pixelFormat = pf; d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    if (tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray) {
        // multisample textures live in private storage: cleared to 0 by a render pass (128-bit integer formats have no multisample form)
        if (dpf == NVN_DPF_UINT) d.pixelFormat = MTLPixelFormatRGBA16Uint; else if (dpf == NVN_DPF_SINT) d.pixelFormat = MTLPixelFormatRGBA16Sint;
        d.sampleCount = 4; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        if (!(t = [g_dev newTextureWithDescriptor:d])) { RED_ONCE(139, "cannot create the zero multisample texture (type %lu, Metal format %lu)", (unsigned long)tt, (unsigned long)d.pixelFormat); return nil; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        if (dpf == NVN_DPF_DEPTH) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 0; rp.depthAttachment.storeAction = MTLStoreActionStore; }
        else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore; }
        id<MTLCommandBuffer> cb = cl_cb();
        [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
        cache[dtt % DMY_NT][dpf & 3] = t; g_dummy_tex_rid[dtt % DMY_NT][dpf & 3] = t.gpuResourceID._impl; res_add(t);
        return t;
    }
    t = [g_dev newTextureWithDescriptor:d];   // shared storage is all zeros when newly created
    if (pf != MTLPixelFormatDepth32Float) {
        uint8_t z[64] = {0};
        NSUInteger faces = tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1;
        for (NSUInteger f = 0; f < faces; f++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:z bytesPerRow:16 bytesPerImage:16];
    }
    cache[dtt % DMY_NT][dpf & 3] = t; g_dummy_tex_rid[dtt % DMY_NT][dpf & 3] = t.gpuResourceID._impl; res_add(t);
    return t;
}
// write sink for unbound image slots: writes must not reach the dummies that read 0
static id<MTLTexture> dummy_img(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    id<MTLTexture> t = cache[dtt & 7][dpf & 3]; if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = (MTLTextureType)dtt; d.pixelFormat = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA16Float;
    d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [g_dev newTextureWithDescriptor:d];
    cache[dtt & 7][dpf & 3] = t; g_dummy_img_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t);
    return t;
}
static id<MTLBuffer> dummy_buf(void);
static id<MTLTexture> dummy_tbuf(int dpf) {
    static __strong id<MTLTexture> cache[4];
    id<MTLTexture> t = cache[dpf & 3]; if (t) return t;
    MTLPixelFormat pf = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
    id<MTLBuffer> b = dummy_buf();
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:b.length / 16 resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
    t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:b.length];
    cache[dpf & 3] = t; g_dummy_tbuf_rid[dpf & 3] = t.gpuResourceID._impl;
    return t;
}
// The view starts at the aligned boundary below its offset: the elements in between, which the shader adds (TranslationFlags.MetalBufferImageBias); 0 for anything else.
static uint32_t tbuf_lead(int tid, id<MTLTexture> t) {
    if (!t || t.textureType != MTLTextureTypeTextureBuffer || tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok || g_treg[tid].t.d.target != NVN_TEXTURE_TARGET_BUFFER) return 0;
    id<MTLTexture> root = t; while (root.parentTexture) root = root.parentTexture;   // a format or swizzle view keeps its parent's buffer and width
    void *ov = NULL; tex_fast_rid(tid, &ov);
    id<MTLTexture> own = (__bridge id<MTLTexture>)ov;
    while (own.parentTexture) own = own.parentTexture;
    if (root != own) return 0;
    uint32_t n = g_treg[tid].t.d.width;
    return root.width > n ? (uint32_t)(root.width - n) : 0;
}
// Past DMSH_TBIAS_SLOTS, read through an aligned copy made at its first binding in each submission (GPU writes in it are not seen). Caller holds g_rmu.
enum { TBS_BLOCK = 4u << 20, TBS_CACHE = 512 };
static id<MTLBuffer> g_tbs_blk;
static size_t g_tbs_off;
static NSMutableArray *g_tbs_views;
static struct { __unsafe_unretained id<MTLTexture> src, copy; uint64_t epoch, rid; } g_tbs_cache[TBS_CACHE];
static uint64_t g_tbs_copies;
static id<MTLTexture> tbuf_shadow(int tid, id<MTLTexture> t, uint64_t *rid) {
    uint32_t lead = tbuf_lead(tid, t);
    if (!lead) return t;
    id<MTLTexture> root = t; while (root.parentTexture) root = root.parentTexture;
    int bw, bh, bpb = nvn_fmt_bpb(g_treg[tid].t.d.format, &bw, &bh);
    id<MTLBuffer> src = root.buffer; NSUInteger off = root.bufferOffset + (NSUInteger)lead * (NSUInteger)bpb, w = t.width - lead;
    if (!src || bpb <= 0 || off >= src.length) return t;
    // a private buffer's contents must not be asked for
    if (src.storageMode == MTLStorageModePrivate || !src.contents) { RED_ONCE(117, "buffer texture at pool offset %#lx (below Metal's view alignment) in a pool the CPU cannot access, read through a binding with no offset table entry: read %u elements early", (unsigned long)off, lead); return t; }
    unsigned k = (unsigned)(((uintptr_t)(__bridge void *)t >> 4) % TBS_CACHE);
    if (g_tbs_cache[k].src == t && g_tbs_cache[k].epoch == g_fence + 1) { *rid = g_tbs_cache[k].rid; return g_tbs_cache[k].copy; }
    size_t bytes = (size_t)w * (size_t)bpb, row = (bytes + 255u) & ~(size_t)255u, n = bytes < src.length - off ? bytes : src.length - off;   // 256: a multiple of every view alignment
    if (!g_tbs_blk || g_tbs_off + row > g_tbs_blk.length) {   // released once the submissions that read them are done
        if (g_tbs_blk) { defer_obj_release(g_tbs_blk); defer_obj_release(g_tbs_views); }
        g_tbs_blk = [g_dev newBufferWithLength:row > TBS_BLOCK ? row : TBS_BLOCK options:MTLResourceStorageModeShared | MTLResourceCPUCacheModeWriteCombined];
        if (!g_tbs_blk) { RED_ONCE(118, "no staging block for buffer textures below Metal's view alignment"); return t; }
        res_add(g_tbs_blk); g_tbs_views = [NSMutableArray array]; g_tbs_off = 0;
    }
    memcpy((uint8_t *)g_tbs_blk.contents + g_tbs_off, (const uint8_t *)src.contents + off, n);
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:t.pixelFormat width:w resourceOptions:g_tbs_blk.resourceOptions usage:MTLTextureUsageShaderRead];
    d.swizzle = t.swizzle;
    id<MTLTexture> c = [g_tbs_blk newTextureWithDescriptor:d offset:g_tbs_off bytesPerRow:row];
    if (!c) { RED_ONCE(25, "cannot copy a buffer texture below Metal's view alignment (%lu elements, format %lu)", (unsigned long)w, (unsigned long)t.pixelFormat); return t; }
    g_tbs_off += row; [g_tbs_views addObject:c];
    if (!g_tbs_copies++) MLOG("buffer texture at pool offset %#lx (below Metal's view alignment) read through an aligned copy, its binding having no offset table entry: %lu elements, format %lu, %zu bytes (first case)", (unsigned long)off, (unsigned long)w, (unsigned long)t.pixelFormat, n);
    g_tbs_cache[k].src = t; g_tbs_cache[k].copy = c; g_tbs_cache[k].epoch = g_fence + 1; g_tbs_cache[k].rid = *rid = c.gpuResourceID._impl;
    return c;
}
static id<MTLSamplerState> dummy_samp(void) {
    static id<MTLSamplerState> s;
    if (!s) { MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.supportArgumentBuffers = YES; s = [g_dev newSamplerStateWithDescriptor:d]; }
    return s;
}
static uint64_t dummy_samp_rid(void) { static uint64_t rid; if (!rid) rid = dummy_samp().gpuResourceID._impl; return rid; }
// Pools a handle traced to no slot reads (TEXP / SAMPP): the bound n, then each ID's resource ID at the draw's base. The old buffer goes when its submissions complete. Caller holds g_rmu.
#define POOL_SNAPS 16
// n -1: more than POOL_IDS_MAX, build whole. dead: IDs without storage, which a later pool can bring; trans: views of retired registrations, gone at submission sub's end.
enum { POOL_IDS_MAX = 8192 };
typedef struct { int *v, n, cap; } pool_ids;
typedef struct { __strong id<MTLBuffer> buf; uint64_t gpu, reg, ser, frame, sub; int samp, dtt, dpf, base, left, left_req; pool_ids dead, trans; int over; } pool_snap;   // over: its last walk found the budget full, so what it left out has its stand-in
static pool_snap g_psnap[POOL_SNAPS];
static unsigned g_psnap_next;
// Entries written since the build, so a spare at log position lpos is brought up to date; a ring of a quarter of the entries (past that a copy is cheaper).
static struct { uint32_t *at; uint64_t *v; uint64_t n, build; uint32_t cap; } g_plog[POOL_SNAPS];
static uint64_t g_pool_build;
static inline void plog_add(const pool_snap *q, uint32_t at, uint64_t v) {
    int k = (int)(q - g_psnap);
    if (!g_plog[k].cap) return;
    uint32_t j = (uint32_t)(g_plog[k].n++ % g_plog[k].cap); g_plog[k].at[j] = at; g_plog[k].v[j] = v;
}
static void plog_build(const pool_snap *q, int n) {
    int k = (int)(q - g_psnap); uint32_t cap = MAX(64u, (uint32_t)(n + 1) / 4);
    if (cap != g_plog[k].cap) {
        g_plog[k].at = dm_realloc(g_plog[k].at, cap * sizeof *g_plog[k].at, "a pool table's log");
        g_plog[k].v = dm_realloc(g_plog[k].v, cap * sizeof *g_plog[k].v, "a pool table's log");
        g_plog[k].cap = cap;
    }
    g_plog[k].n = 0; g_plog[k].build = ++g_pool_build;
}
static uint64_t g_psnap_gen = 1;   // an argument block holding a pool address is not reused past it
static uint64_t g_stat_pool_builds;
static double g_stat_pool_ms;
static uint64_t g_stat_pool_why[5];
static uint64_t g_stat_pool_patched, g_stat_pool_kept, g_stat_pool_nopatch[5], g_stat_pool_pchg, g_stat_pool_schg;
static void ids_push(pool_ids *l, int id_) {   // past POOL_IDS_MAX the list is dropped (n -1)
    if (l->n < 0) return;
    if (l->n == POOL_IDS_MAX) { free(l->v); *l = (pool_ids){NULL, -1, 0}; return; }
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = dm_realloc(l->v, (size_t)l->cap * sizeof *l->v, "the pool tables' ID lists"); }
    l->v[l->n++] = id_;
}
static struct { uint32_t *stamp; int cap; uint32_t cur; } g_pids[2];
static void ids_begin(int k) {
    if (!++g_pids[k].cur) {
        if (g_pids[k].stamp) memset(g_pids[k].stamp, 0, (size_t)g_pids[k].cap * sizeof *g_pids[k].stamp);
        g_pids[k].cur = 1;
    }
}
static void ids_add(int k, pool_ids *l, int id_) {
    if (l->n < 0 || id_ < 0) return;
    if (id_ >= g_pids[k].cap) {
        int c = id_ + 4096;
        g_pids[k].stamp = dm_realloc(g_pids[k].stamp, (size_t)c * sizeof *g_pids[k].stamp, "the pool tables' ID lists");
        memset(g_pids[k].stamp + g_pids[k].cap, 0, (size_t)(c - g_pids[k].cap) * sizeof *g_pids[k].stamp); g_pids[k].cap = c;
    }
    if (g_pids[k].stamp[id_] == g_pids[k].cur) return;
    g_pids[k].stamp[id_] = g_pids[k].cur;
    ids_push(l, id_);
}
static void ids_keep(int k, pool_ids *l, const pool_ids *old) {
    if (old->n < 0) { free(l->v); *l = (pool_ids){NULL, -1, 0}; return; }
    for (int i = 0; i < old->n; i++) ids_add(k, l, old->v[i]);
}
// A game leaves IDs registered past their textures: the pool object is live (nvnMemoryPoolFinalize clears the magic) and the storage lies in a mapped pool.
static int tex_storage_live(const tex_t *t) {
    const pool_t *p = t->pool;
    if (p && p->magic != MAGIC('P', 'O', 'O', 'L')) return 0;
    if (p && nvn_sparse_pool(p)) return 1;
    return mtl_addr_known((uint64_t)(uintptr_t)t->mem, t->L.size ? t->L.size : 1);
}
// Each registered texture would be a Metal texture past the working set (docs/reference/memory.md#shaders-report-which-textures-they-read-through-a-pool): shader reads mark IDs
// (POOL_REQ_BYTES a frame); a walk makes the smallest up front (POOL_FRAME_BYTES), as a one-time pass has no second frame.
enum { POOL_FRAME_BYTES = 32u << 20, POOL_REQ_BYTES = 128u << 20, POOL_SUB_MS = 2 };
static uint64_t g_walk_frame = ~0ull, g_walk_spent, g_walk_sub = ~0ull;
static double g_walk_ms;
static int g_walk_over;
static uint64_t pool_tex_size(const tex_t *t) { return t->L.size ? t->L.size : 1; }
static int pool_size_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint64_t pool_budget(void) { return g_dev.recommendedMaxWorkingSetSize / 20; }
static uint64_t g_walk_req;
static uint64_t walk_frame_now(void) {
    uint64_t f = atomic_load(&g_cur_frame);
    if (f != g_walk_frame) { g_walk_frame = f; g_walk_spent = 0; g_walk_req = 0; }
    return f;
}
// a target no pass has written holds nothing to read (the game re-registers IDs with transient targets)
static int pool_unwritten_target(const tex_t *t) { return (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && t->serial && !tex_get(t->serial) && !pkg_shared(t); }
static int pool_budget_full(void) { return g_dev.currentAllocatedSize >= pool_budget(); }
static uint64_t pool_room(void) {
    walk_frame_now();
    uint64_t want = pool_budget(), have = g_dev.currentAllocatedSize, room = have < want ? want - have : 0;
    g_walk_over = have >= want;
    uint64_t left = g_walk_spent < POOL_FRAME_BYTES ? POOL_FRAME_BYTES - g_walk_spent : 0; if (room > left) room = left;
    return room;
}
static uint64_t pool_walk_limit(int base, int n) {   // UINT64_MAX while they all fit
    uint64_t room = pool_room(), sum = 0;
    if (!room) return 0;
    uint64_t *z = malloc(sizeof *z * (size_t)n); int m = 0;
    if (!z) return UINT64_MAX;
    for (int i = 0; i < n; i++) {
        int id_ = base + i;
        if (id_ > 0 && g_treg[id_].ok && !g_treg[id_].gone && !g_treg[id_].view && !pkg_shared(&g_treg[id_].t)) { z[m] = pool_tex_size(&g_treg[id_].t); sum += z[m]; m++; }
    }
    uint64_t limit = UINT64_MAX;
    if (sum > room) {
        qsort(z, (size_t)m, sizeof *z, pool_size_cmp);
        limit = 0; sum = 0;
        for (int i = 0; i < m && sum + z[i] <= room; i++) { sum += z[i]; limit = z[i]; }
    }
    free(z);
    return limit;
}
static uint64_t pool_seen_gpu(void) {
    if (!g_seen_buf) { g_seen_buf = [g_dev newBufferWithLength:POOL_SEEN_TYPES * POOL_SEEN_WORDS * 4 options:MTLResourceStorageModeShared]; res_add(g_seen_buf); res_commit(); }
    return g_seen_buf.gpuAddress;
}
static uint64_t pool_seen_gpu_t(int dtt) { return pool_seen_gpu() + (uint64_t)(dtt >= 0 && dtt < POOL_SEEN_TYPES ? seen_sec(dtt) : 0) * POOL_SEEN_WORDS * 4; }
// Newly read IDs go into existing tables in place (dummy → texture is safe for an encoded draw); the rest wait in g_seen_todo. Caller holds g_rmu.
static uint32_t g_seen_todo[POOL_SEEN_WORDS];
static uint64_t g_fb_split_pool;
static int g_fb_dread_pool;
// a pool shader may name any texture, so fb_prepare asks this whether a draw can read its depth attachment
static nvn_hmap g_pool_ds = {.klen = sizeof(alias_key)};
static uint64_t g_pool_ds_gen;
static void pool_ds_note(int id_) {
    if (id_ <= 0 || id_ >= g_ntreg || !g_treg[id_].ok || !fmt_depth(g_treg[id_].t.d.format)) return;
    alias_key k = tex_alias_key(&g_treg[id_].t);
    if (!nvn_hm_get(&g_pool_ds, &k)) { nvn_hm_put(&g_pool_ds, &k, (void *)1); g_pool_ds_gen++; }
}
static int pool_ds_read(const tex_t *t) {
    static uint64_t ser = 0, gen = ~0ull; static int read;   // the answer holds until the target or the set changes
    if (t->serial != ser || g_pool_ds_gen != gen) {
        alias_key k = tex_alias_key(t);
        read = nvn_hm_get(&g_pool_ds, &k) != NULL; ser = t->serial; gen = g_pool_ds_gen;
    }
    return read;
}
// A stand-in once the budget is full (rendering.md#a-streamed-textures-first-read-drew-its-surface-dark-for-one-frame): small levels or a block sample, in a resident heap so a
// table may name it at once. DM_NVN_POOL_STANDIN=0: the dummy.
enum { STAND_LEVEL = 1024, STAND_PICK = 4096, STAND_LAYERS = 8, STAND_HEAP = 4u << 20, STAND_HEAPS = 32, STAND_FRAME = 512 };
static __strong id<MTLHeap> g_stand_heap[STAND_HEAPS];
static int g_nstand_heap, g_stand_heap_at;
static uint32_t g_stand_todo[POOL_SEEN_WORDS];
static unsigned g_stand_todo_n, g_stand_frame_n;
static uint64_t g_stand_frame = ~0ull;
static uint64_t g_stand_made, g_stand_picked, g_stand_full, g_stand_ns, g_first_stand, g_first_dummy, g_first_dummy_big, g_first_stale;
static int stand_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_NVN_POOL_STANDIN"); v = !(e && !strcmp(e, "0")); }
    return v;
}
static void stand_frame_sync(void) {
    uint64_t f = atomic_load(&g_cur_frame);
    if (f != g_stand_frame) { g_stand_frame = f; g_stand_frame_n = 0; }
}
static id<MTLTexture> stand_alloc(MTLTextureDescriptor *d) {
    for (int k = 0; k < g_nstand_heap; k++) {
        id<MTLTexture> t = [g_stand_heap[(g_stand_heap_at + k) % g_nstand_heap] newTextureWithDescriptor:d];
        if (t) { g_stand_heap_at = (g_stand_heap_at + k) % g_nstand_heap; return t; }
    }
    if (g_nstand_heap == STAND_HEAPS) { g_stand_full++; return nil; }
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.type = MTLHeapTypeAutomatic; hd.size = STAND_HEAP; hd.storageMode = MTLStorageModeShared;
    id<MTLHeap> h = [g_dev newHeapWithDescriptor:hd];
    if (!h) { g_stand_full++; return nil; }
    res_add(h); res_commit();   // before any table names a texture in it
    g_stand_heap_at = g_nstand_heap; g_stand_heap[g_nstand_heap++] = h;
    return [h newTextureWithDescriptor:d];
}
// else its last level cut to nx x ny blocks, every step-th. 0: none
typedef struct { unsigned l0, n; uint32_t w, h, step, nx, ny; } stand_shape;
static int stand_shape_of(const tex_t *t, stand_shape *s) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t W = MAX(t->d.width, 1u), H = MAX(t->d.height, 1u), levels = MAX(t->d.levels, 1u), fit = 1;
    while (MAX(W, H) >> fit) fit++;
    if (!bpb || levels > fit) return 0;
    memset(s, 0, sizeof *s);
    unsigned l = 0; uint32_t bx = 0, by = 0;
    for (; l < levels; l++) {
        bx = (MAX(W >> l, 1u) + bw - 1) / bw; by = (MAX(H >> l, 1u) + bh - 1) / bh;
        if ((size_t)bx * by * bpb <= STAND_LEVEL) break;
    }
    if (l < levels) { s->l0 = l; s->n = levels - l; s->w = MAX(W >> l, 1u); s->h = MAX(H >> l, 1u); return 1; }
    if (bpb > 16 || (bpb & (bpb - 1))) return 0;   // a block in one 16-byte piece of the storage
    uint32_t step = (uint32_t)sqrt((double)bx * by * bpb / STAND_PICK); if (step < 2) step = 2;
    while ((size_t)MAX(bx / step, 1u) * MAX(by / step, 1u) * bpb > STAND_PICK) step++;
    s->l0 = levels - 1; s->n = 1; s->step = step; s->nx = MAX(bx / step, 1u); s->ny = MAX(by / step, 1u); s->w = s->nx * bw; s->h = s->ny * bh;
    return 1;
}
// 0: another type, or layers the texture lacks
static int stand_layers(const treg_t *r, unsigned *s0, unsigned *ns, MTLTextureType *tt) {
    const tex_t *t = &r->t; int arr = t->d.target == NVN_TEXTURE_TARGET_2D_ARRAY;
    if (!arr && t->d.target != NVN_TEXTURE_TARGET_2D && t->d.target != NVN_TEXTURE_TARGET_RECTANGLE) return 0;
    unsigned depth = arr ? MAX(t->d.depth, 1u) : 1;
    *s0 = r->has_v && r->v.set_layers ? r->v.min_layer : 0; if (*s0 >= depth) return 0;
    *ns = r->has_v && r->v.set_layers && r->v.nlayers ? r->v.nlayers : depth - *s0;
    MTLTextureType base = arr ? MTLTextureType2DArray : MTLTextureType2D; *tt = r->has_v && r->v.set_target ? ttype(r->v.target, base) : base;
    if (*tt == MTLTextureType2D) *ns = 1; else if (*tt != MTLTextureType2DArray) return 0;
    return *s0 + *ns <= depth && *ns <= STAND_LAYERS;
}
// sparse pools: only when every page behind its levels is mapped (rendering.md#a-sparse-pools-streamed-textures-read-0-at-their-first-read); unmapped pages fault (SIGBUS)
enum { STAND_SPARSE_GATHER = 256u << 10 };
static uint64_t g_stand_sparse, g_stand_sparse_unmapped;
static int stand_span(const tex_t *t, unsigned y, unsigned l0, uint64_t *lo, uint64_t *hi) {
    *lo = (uint64_t)y * t->L.layer_size + nvn_tex_level_offset(&t->d, l0); *hi = MIN((uint64_t)(y + 1) * t->L.layer_size, t->L.size);
    return *hi > *lo && *hi - *lo <= STAND_SPARSE_GATHER;
}
static int stand_fill(const tex_t *t, const stand_shape *s, unsigned s0, unsigned ns, id<MTLTexture> into, uint64_t *sum) {
    static uint8_t buf[STAND_PICK], *gat;
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint64_t fnv = 1469598103934665603ull;
    int sp = t->pool && nvn_sparse_pool(t->pool);
    for (unsigned y = 0; y < ns; y++) {
        const uint8_t *src = t->mem; uint64_t at = 0, lo, hi;
        if (sp) {
            if (!stand_span(t, s0 + y, s->l0, &lo, &hi)) return 0;
            if (!gat && !(gat = malloc(STAND_SPARSE_GATHER))) return 0;
            if (nvn_sparse_copy(t->pool, gat, (uint64_t)(uintptr_t)t->mem + lo, hi - lo)) { g_stand_sparse_unmapped++; return 0; }
            src = gat; at = lo;
        }
        for (unsigned k = 0; k < s->n; k++) {
            uint32_t w = s->step ? s->w : MAX(s->w >> k, 1u), hh = s->step ? s->h : MAX(s->h >> k, 1u);
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (hh + bh - 1) / bh;
            if (pitch * rows > sizeof buf) return 0;
            // nvn_deswizzle_pick adds the level's whole-storage offset, so it is handed the storage's would-be start
            if (s->step ? nvn_deswizzle_pick(&t->d, s->l0, s0 + y, (const void *)((uintptr_t)src - at), s->step, buf, s->nx, s->ny)
                        : sp ? nvn_deswizzle_from(&t->d, s->l0 + k, s0 + y, src, at, buf, pitch) : nvn_deswizzle(&t->d, s->l0 + k, s0 + y, src, buf, pitch)) return 0;
            if (into) [into replaceRegion:MTLRegionMake2D(0, 0, w, hh) mipmapLevel:k slice:y withBytes:buf bytesPerRow:pitch bytesPerImage:pitch * rows];
            if (sum) for (size_t i = 0; i < pitch * rows; i++) fnv = (fnv ^ buf[i]) * 1099511628211ull;
        }
    }
    if (sum) *sum = fnv;
    return 1;
}
static int pool_first_log(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_POOL_FIRST") != NULL; return v; }
static int stand_make_(int id_);
static int stand_make(int id_) {   // caller holds g_rmu
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); int made = stand_make_(id_); g_stand_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    return made;
}
static int stand_make_(int id_) {
    treg_t *r = &g_treg[id_]; const tex_t *t = &r->t;
    r->stand_no = STAND_NO;   // not again while this registration stands
    if (!t->mem || t->d.samples > 1 || t->d.width > 16384 || t->d.height > 16384 || fmt_depth(t->d.format) || rb_packed_shift(t->d.format)
        || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) return 0;
    int sp = t->pool && nvn_sparse_pool(t->pool);
    if ((t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) && !sp) return 0;
    unsigned s0, ns; MTLTextureType tt; stand_shape sh;
    if (!stand_layers(r, &s0, &ns, &tt) || !stand_shape_of(t, &sh)) return 0;
    if (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) {   // from its mip tail on, not its tiled levels
        uint32_t tw, th, td; int nt = nvn_tex_sparse_tiles(&t->d, &tw, &th, &td);
        if (nt < 0 || sh.l0 < (unsigned)nt) return 0;
    }
    // an unmapped page waits for a mapping note over its pages (treg_sparse_notes)
    if (sp) for (unsigned y = 0; y < ns; y++) {
        uint64_t lo, hi;
        if (!stand_span(t, s0 + y, sh.l0, &lo, &hi)) return 0;
        if (!nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + lo, hi - lo)) { g_stand_sparse_unmapped++; r->stand_no = STAND_WAIT; return 0; }
    }
    uint32_t pf = 0, view_pf = 0; uint8_t sw[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, NULL) || !pf) return 0;
    int need_sw = treg_swizzle(r, (MTLPixelFormat)pf, 0, sw, &view_pf);
    if (view_pf && view_pf != pf) { if (tex_need_pfv((MTLPixelFormat)pf, (MTLPixelFormat)view_pf)) return 0; pf = view_pf; }   // the sRGB or linear twin holds the same bytes; another format is a view the stand-in does not make
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt; d.pixelFormat = (MTLPixelFormat)pf; d.width = sh.w; d.height = sh.h; d.mipmapLevelCount = sh.n;
    if (tt == MTLTextureType2DArray) d.arrayLength = ns;
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    if (need_sw) d.swizzle = MTLTextureSwizzleChannelsMake(sw[0], sw[1], sw[2], sw[3]);
    id<MTLTexture> mt = stand_alloc(d);
    uint64_t um0 = g_stand_sparse_unmapped;
    if (!mt || !stand_fill(t, &sh, s0, ns, mt, pool_first_log() ? &r->stand_sum : NULL)) {
        if (g_stand_sparse_unmapped != um0) r->stand_no = STAND_WAIT;   // unmapped since the check: that unmapping is a mapping note, and so is the one that maps it again
        return 0;
    }
    r->stand = (void *)CFBridgingRetain(mt); r->stand_rid = mt.gpuResourceID._impl; r->stand_tt1 = (uint8_t)tt + 1;
    g_stand_made++; g_stand_live++; g_stand_picked += sh.step != 0; g_stand_sparse += sp;
    return 1;
}
// 0: the dummy. Caller holds g_rmu.
static uint64_t pool_stand(int id_, int dtt) {
    treg_t *r = &g_treg[id_];
    if (!r->stand && !r->stand_no && g_walk_over && stand_on()) {
        stand_frame_sync();
        if (g_stand_frame_n < STAND_FRAME) g_stand_frame_n += (unsigned)stand_make(id_);
        else if (id_ < (int)(POOL_SEEN_WORDS * 32) && !(g_stand_todo[id_ >> 5] >> (id_ & 31) & 1)) { g_stand_todo[id_ >> 5] |= 1u << (id_ & 31); g_stand_todo_n++; }
    }
    return r->stand && r->stand_tt1 - 1 == dtt ? r->stand_rid : 0;
}
// once a submission; caller holds g_rmu
static void pool_stand_scan(void) {
    stand_frame_sync();
    unsigned words = MIN((unsigned)g_ntreg / 32 + 1, (unsigned)POOL_SEEN_WORDS);
    for (unsigned i = 0; i < words && g_stand_todo_n && g_stand_frame_n < STAND_FRAME; i++)
        for (uint32_t bits = g_stand_todo[i]; bits && g_stand_frame_n < STAND_FRAME; bits &= bits - 1) {
            int b = __builtin_ctz(bits), id_ = (int)i * 32 + b;
            g_stand_todo[i] &= ~(1u << b); g_stand_todo_n--;   // NOLINT(clang-analyzer-core.BitwiseShift): bits is nonzero, so ctz returns 0..31
            if (id_ <= 0 || id_ >= g_ntreg) continue;
            treg_t *r = &g_treg[id_];
            if (!r->ok || r->gone || r->view || r->stand || r->stand_no || !tex_storage_live(&r->t) || pool_unwritten_target(&r->t) || pkg_shared(&r->t) || tex_get(r->t.serial)) continue;
            if (!stand_make(id_)) continue;
            g_stand_frame_n++;
            for (int k = 0; k < POOL_SNAPS; k++) {
                pool_snap *q = &g_psnap[k]; int at = id_ - q->base;
                if (!q->buf || q->samp || at < 0 || (uint32_t)at >= *(uint32_t *)q->buf.contents || q->dtt != r->stand_tt1 - 1) continue;
                if (q->dtt == MTLTextureTypeTextureBuffer) { q->reg = 0; continue; }
                uint64_t *e = (uint64_t *)((uint8_t *)q->buf.contents + DMSH_POOL_HEAD);
                if (e[at] == e[*(uint32_t *)q->buf.contents]) { e[at] = r->stand_rid; plog_add(q, (uint32_t)at, r->stand_rid); }
            }
        }
}
// DM_NVN_POOL_FIRST=1 logs each
static void pool_first_read(int id_, uint64_t f) {
    if (id_ <= 0 || id_ >= g_ntreg || !g_treg[id_].ok || g_treg[id_].gone) return;
    treg_t *r = &g_treg[id_]; const tex_t *t = &r->t;
    if (r->view || !tex_storage_live(t) || pool_unwritten_target(t) || pkg_shared(t) || tex_get(t->serial)) return;
    int log_on = pool_first_log(), stale = 0;
    if (r->stand && log_on) {
        unsigned s0, ns; MTLTextureType tt; stand_shape sh; uint64_t sum = 0;
        stale = !stand_layers(r, &s0, &ns, &tt) || !stand_shape_of(t, &sh) || !stand_fill(t, &sh, s0, ns, nil, &sum) || sum != r->stand_sum;
    }
    if (r->stand) { g_first_stand++; g_first_stale += (unsigned)stale; } else { g_first_dummy++; g_first_dummy_big += pool_tex_size(t) >= (256u << 10); }
    static uint64_t lf = ~0ull; static int ln;
    if (f != lf) { lf = f; ln = 0; }
    if (log_on && ln++ < 32)
        MLOG("[pool] frame %llu: texture ID %d read before its texture was made, found %s: ser=%llu %ux%u format %#x target %d levels %u flags %#x, %llu KB, registered %llu frames before; view %d (format %d:%#x levels %d:%u+%u layers %d:%u+%u target %d:%d swizzle %d)",
             (unsigned long long)f, id_, r->stand ? (stale ? "its stand-in, written since" : "its stand-in") : r->stand_no == STAND_WAIT ? "the dummy (its stand-in waits for a mapping)" : r->stand_no ? "the dummy (no stand-in for it)" : "the dummy", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.target, t->d.levels, t->d.flags,
             (unsigned long long)(pool_tex_size(t) >> 10), (unsigned long long)(f - r->reg_frame), r->has_v, r->v.set_format, r->v.format, r->v.set_levels, r->v.base_level, r->v.nlevels, r->v.set_layers, r->v.min_layer, r->v.nlayers, r->v.set_target, r->v.target, r->v.set_swizzle);
}
static void pool_seen_scan(void) {
    if (!g_seen_buf || g_seen_sub == g_sub_serial) return;
    g_seen_sub = g_sub_serial;
    uint64_t f = walk_frame_now();
    if (g_stand_todo_n) pool_stand_scan();
    {
        static uint64_t at, was;
        uint64_t now = g_stand_made + g_first_stand + g_first_dummy;
        if (f >= at + 300 && now != was) {
            at = f; was = now;
            MLOG("pool stand-ins up to frame %llu: %llu made in %.1f ms (%llu of them a level cut down, %llu in a sparse pool; %llu of a sparse pool not made: a page unmapped), %llu gave way to their texture, %llu held now in %d heaps of %u MB (%llu not made: the heaps were full), %u waiting; first reads of a texture not made found a stand-in %llu times%s and the dummy %llu times (%llu of them textures of 256 KB or more)",
                 (unsigned long long)f, (unsigned long long)g_stand_made, g_stand_ns / 1e6, (unsigned long long)g_stand_picked, (unsigned long long)g_stand_sparse, (unsigned long long)g_stand_sparse_unmapped, (unsigned long long)g_stand_done, (unsigned long long)g_stand_live, g_nstand_heap, (unsigned)(STAND_HEAP >> 20), (unsigned long long)g_stand_full, g_stand_todo_n, (unsigned long long)g_first_stand,
                 g_first_stale ? [NSString stringWithFormat:@" (%llu written since the stand-in was made)", (unsigned long long)g_first_stale].UTF8String : "", (unsigned long long)g_first_dummy, (unsigned long long)g_first_dummy_big);
        }
    }
    const uint32_t *w = (const uint32_t *)g_seen_buf.contents; unsigned words = MIN((unsigned)g_ntreg / 32 + 1, (unsigned)POOL_SEEN_WORDS);
    for (unsigned i = 0; i < words; i++) {
        uint32_t got = 0, typed_new = 0;
        for (unsigned s_ = 0; s_ < POOL_SEEN_TYPES; s_++) {
            uint32_t g = w[s_ * POOL_SEEN_WORDS + i];
            if (!g) continue;
            typed_new |= g & ~g_seen_typed[s_][i]; g_seen_typed[s_][i] |= g; got |= g;
        }
        for (uint32_t bits = got; bits; bits &= bits - 1) { int id_ = (int)i * 32 + __builtin_ctz(bits); if (id_ < g_ntreg) g_treg[id_].used = g_tu_now; }
        g_seen_todo[i] |= typed_new & g_seen_done[i];   // already read through another type: the tables of the new type take it in place
        uint32_t fresh = got & ~g_seen_done[i];
        if (fresh) {
            g_seen_done[i] |= fresh; g_seen_todo[i] |= fresh;
            for (uint32_t bits = fresh; bits; bits &= bits - 1) {
                int id_ = (int)i * 32 + __builtin_ctz(bits);
                pool_ds_note(id_); pool_first_read(id_, f);
            }
        }
        for (uint32_t bits = g_seen_todo[i]; bits; bits &= bits - 1) {
            int b = __builtin_ctz(bits), id_ = (int)i * 32 + b;
            if (id_ <= 0 || id_ >= g_ntreg || !g_treg[id_].ok || g_treg[id_].gone || (!g_treg[id_].view && !tex_storage_live(&g_treg[id_].t))) { g_seen_todo[i] &= ~(1u << b); continue; } // NOLINT(clang-analyzer-core.BitwiseShift): bits is nonzero, so ctz returns 0..31
            if (!g_treg[id_].view && pool_unwritten_target(&g_treg[id_].t)) { g_seen_todo[i] &= ~(1u << b); continue; }
            if (!g_treg[id_].view && !pkg_shared(&g_treg[id_].t) && !tex_get(g_treg[id_].t.serial)) { if (g_walk_req >= POOL_REQ_BYTES) return; g_walk_req += pool_tex_size(&g_treg[id_].t); }
            g_seen_todo[i] &= ~(1u << b);
            void *tv = NULL; uint64_t tr = tex_fast_rid(id_, &tv); if (!tv || !tr) continue;
            MTLTextureType tt = ((__bridge id<MTLTexture>)tv).textureType; int trans = g_treg[id_].retired && g_treg[id_].transient_sub == g_sub_serial;
            for (int k = 0; k < POOL_SNAPS; k++) {
                pool_snap *q = &g_psnap[k]; int at = id_ - q->base;
                if (!q->buf || q->samp || at < 0 || (uint32_t)at >= *(uint32_t *)q->buf.contents) continue;
                if (q->dtt == MTLTextureTypeTextureBuffer) { q->reg = 0; continue; }
                uint64_t er = tr;
                if ((MTLTextureType)q->dtt != tt) {
                    if (!pool_seen_t(id_, q->dtt)) continue;
                    er = pool_bridge_pair((int)tt, q->dtt) ? pool_fit(id_, (__bridge id<MTLTexture>)tv, q->dtt) : 0;
                    if (!er) { pool_type_note(id_, (int)tt, q->dtt); continue; }
                }
                ((uint64_t *)((uint8_t *)q->buf.contents + DMSH_POOL_HEAD))[at] = er; plog_add(q, (uint32_t)at, er);
                if (trans) { ids_push(&q->trans, id_); if (!q->sub) q->sub = g_sub_serial; }
            }
        }
    }
}
// Spare table buffers, reused once their submission completed (docs/reference/memory.md#pool-tables-and-readback-buffers-made-anew-each-frame); a patch takes its table's
// spare furthest along and replays the log (nvn-objects.md#a-patched-pool-table-copied-whole).
enum { POOL_SPARE_MAX = 64, POOL_SPARE_BYTES = 16 << 20 };
static size_t g_pool_spare_cap = POOL_SPARE_BYTES;   // a variable for t_pool_replay
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence, build, lpos; int k; } pool_spare;   // k: the table whose build's state at lpos it holds; -1 none
static pool_spare g_pool_spare[POOL_SPARE_MAX];
static int g_npool_spare;
static uint64_t g_pool_spare_bytes, g_pool_made, g_pool_reused;
static uint64_t g_stat_pool_replayed, g_stat_pool_replay_n, g_stat_pool_copied[3], g_stat_pool_take_ns;
static int spare_live(const pool_spare *s) { return s->k >= 0 && g_plog[s->k].build == s->build; }
static id<MTLBuffer> spare_out(int i) {
    id<MTLBuffer> b = g_pool_spare[i].buf;
    g_pool_spare_bytes -= b.length;
    g_pool_spare[i] = g_pool_spare[--g_npool_spare]; g_pool_spare[g_npool_spare].buf = nil;
    return b;
}
static id<MTLBuffer> pool_table_take(size_t need) {
    size_t len = (need + 0xffff) & ~(size_t)0xffff; uint64_t done = g_done; int o = -1;
    for (int i = 0; i < g_npool_spare; i++) {
        const pool_spare *s = &g_pool_spare[i];
        if (s->fence > done || s->buf.length != len) continue;
        if (o < 0 || (spare_live(&g_pool_spare[o]) && !spare_live(s)) || (spare_live(&g_pool_spare[o]) == spare_live(s) && s->fence < g_pool_spare[o].fence)) o = i;
    }
    if (o >= 0) { g_pool_reused++; return spare_out(o); }
    id<MTLBuffer> b = [g_dev newBufferWithLength:len options:MTLResourceStorageModeShared];
    if (b) { res_add(b); g_pool_made++; }
    return b;
}
static void pool_table_give(id<MTLBuffer> b, int k, uint64_t lpos) {
    if (!b) return;
    while (g_npool_spare && (g_npool_spare == POOL_SPARE_MAX || g_pool_spare_bytes + b.length > g_pool_spare_cap)) {
        int o = 0;
        for (int i = 1; i < g_npool_spare; i++) if (g_pool_spare[i].fence < g_pool_spare[o].fence) o = i;
        defer_obj_release(spare_out(o));
    }
    if (b.length > g_pool_spare_cap) { defer_obj_release(b); return; }
    g_pool_spare[g_npool_spare] = (pool_spare){b, g_fence + 1, k >= 0 ? g_plog[k].build : 0, lpos, k}; g_pool_spare_bytes += b.length; g_npool_spare++;
}
static id<MTLBuffer> pool_patch_take(const pool_snap *q) {
    uint64_t t0 = mach_absolute_time(), done = g_done;
    int k = (int)(q - g_psnap), o = -1, own = 0, fin = 0;
    uint64_t ln = g_plog[k].n;
    for (int i = 0; i < g_npool_spare; i++) {
        const pool_spare *s = &g_pool_spare[i];
        if (s->k != k || s->build != g_plog[k].build || s->buf.length != q->buf.length) continue;
        own = 1;
        if (s->fence > done) continue;
        fin = 1;
        if (ln - s->lpos > g_plog[k].cap) continue;
        if (o < 0 || s->lpos > g_pool_spare[o].lpos) o = i;
    }
    id<MTLBuffer> b;
    if (o < 0) { g_stat_pool_copied[!own ? 0 : !fin ? 1 : 2]++; b = pool_table_take(q->buf.length); if (b) memcpy(b.contents, q->buf.contents, q->buf.length); }
    else {
        uint64_t from = g_pool_spare[o].lpos; b = spare_out(o); g_pool_reused++;
        uint64_t *e = (uint64_t *)((uint8_t *)b.contents + DMSH_POOL_HEAD);
        const uint32_t *at = g_plog[k].at; const uint64_t *v = g_plog[k].v; uint32_t cap = g_plog[k].cap;
        memcpy(b.contents, q->buf.contents, DMSH_POOL_HEAD);
        for (uint64_t j = from; j < ln; j++) e[at[j % cap]] = v[j % cap];
        g_stat_pool_replayed++; g_stat_pool_replay_n += ln - from;
    }
    g_stat_pool_take_ns += mach_absolute_time() - t0;   // ticks: mtl_pool_stats converts
    return b;
}
// times in us
void mtl_pool_stats(uint64_t s[POOL_STATS]) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t used = 0;
    for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf) used += g_psnap[k].buf.length;
    uint64_t v[POOL_STATS] = {g_stat_pool_builds, (uint64_t)(g_stat_pool_ms * 1000), g_stat_pool_patched, g_stat_pool_kept, g_stat_pool_pchg, g_stat_pool_schg, g_stat_pool_replayed, g_stat_pool_replay_n,
        g_stat_pool_copied[0], g_stat_pool_copied[1], g_stat_pool_copied[2], g_stat_pool_take_ns * tb.numer / tb.denom / 1000, used, g_pool_spare_bytes};
    memcpy(s, v, sizeof v);
}
// *dead: storage does not exist yet; *transient: a view of a retired registration. Caller holds g_rmu.
static uint64_t pool_entry(int samp, int dtt, int id_, uint64_t limit, int *gone, int *left, int *left_req, int *transient, int *dead, uint32_t *lead) {
    if (id_ <= 0) return 0;
    if (samp) return id_ < g_nsreg && g_sreg_ok[id_] && samp_fast(id_) ? g_srid[id_] : 0;
    if (id_ >= g_ntreg || !g_treg[id_].ok) return 0;
    const treg_t *r = &g_treg[id_];
    if (r->gone) { ++*gone; return 0; }
    if (!r->view && !tex_storage_live(&r->t)) { ++*gone; *dead = 1; return 0; }
    if (!r->view && pool_unwritten_target(&r->t)) return 0;   // dummy until a pass writes it
    if (r->evicted && !r->view && !pool_seen(id_) && !tex_get(r->t.serial) && !pkg_shared(&r->t)) return pool_stand(id_, dtt);   // freed idle or at memory pressure: made again when a shader reads it
    if (!r->view && r->t.d.target != NVN_TEXTURE_TARGET_BUFFER && pool_tex_size(&r->t) > limit && !pkg_shared(&r->t) && !tex_get(r->t.serial) && !(pool_seen(id_) && g_walk_req < POOL_REQ_BYTES)) { ++*left; *left_req += pool_seen(id_); return pool_stand(id_, dtt); }
    if (r->tt1 && r->tt1 - 1 != dtt && !(pool_bridge_pair(r->tt1 - 1, dtt) && pool_seen_t(id_, dtt))) { pool_type_note(id_, r->tt1 - 1, dtt); return 0; }
    if (!r->view && r->t.d.target != NVN_TEXTURE_TARGET_BUFFER && !pkg_shared(&r->t)) { g_walk_spent += pool_tex_size(&r->t); if (pool_seen(id_)) g_walk_req += pool_tex_size(&r->t); }
    void *tv = NULL; uint64_t tr = tex_fast_rid(id_, &tv);
    if (!tv) return 0;
    MTLTextureType got = ((__bridge id<MTLTexture>)tv).textureType; g_treg[id_].tt1 = (uint8_t)got + 1;
    if (got != (MTLTextureType)dtt) {
        uint64_t b = pool_bridge_pair((int)got, dtt) && pool_seen_t(id_, dtt) ? pool_fit(id_, (__bridge id<MTLTexture>)tv, dtt) : 0;
        if (!b) { pool_type_note(id_, (int)got, dtt); return 0; }
        tr = b;
    }
    if (lead && got == MTLTextureTypeTextureBuffer) *lead = tbuf_lead(id_, (__bridge id<MTLTexture>)tv);
    if (g_treg[id_].retired && g_treg[id_].transient_sub == g_sub_serial) *transient = 1;
    return tr;
}
// Caller holds g_rmu. clear=0 (pkg_evict_idle) leaves the marks for tu_pass.
static void seen_stamp_used(uint32_t now, int clear) {
    if (!g_seen_buf) return;
    uint32_t *w = (uint32_t *)g_seen_buf.contents; unsigned words = MIN((unsigned)g_ntreg / 32 + 1, (unsigned)POOL_SEEN_WORDS);
    for (unsigned s_ = 0; s_ < POOL_SEEN_TYPES; s_++) for (unsigned i = 0; i < words; i++) {
        uint32_t g = w[s_ * POOL_SEEN_WORDS + i]; if (!g) continue;
        for (uint32_t b = g; b; b &= b - 1) { int id_ = (int)i * 32 + __builtin_ctz(b); if (id_ < g_ntreg) g_treg[id_].used = now; }
        if (clear) {
            uint32_t clr = g & g_seen_done[i] & g_seen_typed[s_][i];
            if (clr) atomic_fetch_and((_Atomic uint32_t *)w + s_ * POOL_SEEN_WORDS + i, ~clr);
        }
    }
}
// Idle uploaded textures' Metal copies go after PKG_IDLE_DS (docs/reference/memory.md#idle-uploaded-textures-re-upload-spares-and-memory-pressure-trims); everything naming
// a copy lets go together. Submit thread.
enum { PKG_IDLE_DS = 300, PKG_SWEEP_DS = 100 };   // deciseconds
static _Atomic uint64_t g_evict_n, g_evict_bytes;
static uint64_t pkg_evict_idle(unsigned *n_out) { @autoreleasepool {
    unsigned n = 0; uint64_t bytes = 0; uint32_t now = g_tu_now;
    if (n_out) *n_out = 0;
    NSMutableArray<NvPkg *> *pkgs = [NSMutableArray array];
    NSHashTable *seen = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    nvn_hmap last = {.klen = sizeof(uintptr_t)};
    nvn_hmap byser_m = {.klen = 8}, *byser = &byser_m, *lastp = &last;   // not retained: tables hold them under g_smu
    os_unfair_lock_lock(&g_rmu); pthread_mutex_lock(&g_smu);
    seen_stamp_used(now, 0);
    own_each_locked(^(uint64_t serial, id o, int ghost) {
        if (![o isKindOfClass:[NvPkg class]]) return;
        if (!ghost || !nvn_hm_get(byser, &serial)) nvn_hm_put(byser, &serial, (__bridge void *)o);
        if ([seen containsObject:o]) return;
        [seen addObject:o]; [pkgs addObject:o];
        uintptr_t k = (uintptr_t)(__bridge void *)o;
        nvn_hm_put(lastp, &k, (void *)(uintptr_t)(((NvPkg *)o)->used_tu + 1ull));
    });
    for (int i = 1; i < g_ntreg; i++) {
        treg_t *r = &g_treg[i];
        if (!r->ok || !r->t.serial) continue;
        id o = (__bridge id)nvn_hm_get(byser, &r->t.serial);
        if (!o) continue;
        uintptr_t k = (uintptr_t)(__bridge void *)o, u = (uintptr_t)nvn_hm_get(&last, &k);
        if (u && r->used + 1ull > u) nvn_hm_put(&last, &k, (void *)(uintptr_t)(r->used + 1ull));
    }
    pthread_mutex_unlock(&g_smu); os_unfair_lock_unlock(&g_rmu);
    free(byser_m.keys); free(byser_m.vals);
    NSHashTable *victims = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    memset(g_evict_why, 0, sizeof g_evict_why); g_evict_why[0] = (unsigned)pkgs.count;
    for (NvPkg *pk in pkgs) {
        uintptr_t k = (uintptr_t)(__bridge void *)pk, u = (uintptr_t)nvn_hm_get(&last, &k);
        uint32_t last_use = u ? (uint32_t)(u - 1) : 0;
        if (!u || now < last_use || now - last_use < PKG_IDLE_DS) { g_evict_why[1]++; continue; }
        if (pk->sparse || pk->ps || pk->view || !pk->tex || pk->tex.heap || !pk->mem || !pk->size) { g_evict_why[2]++; continue; }
        if (pk->stored || gws_owned(pk->mem, pk->mem + pk->size)) { g_evict_why[3]++; continue; }
        [victims addObject:pk];
    }
    free(last.keys); free(last.vals);
    if (!victims.count) return 0;
    // g_rmu before g_smu, the lock order everywhere
    nvn_hmap gone_ser = {.klen = 8};
    os_unfair_lock_lock(&g_rmu); pthread_mutex_lock(&g_smu);
    {
        NSUInteger was = victims.count;
        for (NvPkg *pk in victims.allObjects) if (tex_object_pinned(pk) || own_shared_locked(pk->tex) || entry_holds((__bridge void *)pk->tex)) [victims removeObject:pk];
        g_evict_why[4] = (unsigned)(was - victims.count);
    }
    if (!victims.count) { pthread_mutex_unlock(&g_smu); os_unfair_lock_unlock(&g_rmu); return 0; }
    nvn_hmap *gs = &gone_ser;
    own_drop_where_locked(^int(id o) { return [victims containsObject:o]; }, ^(uint64_t serial) { nvn_hm_put(gs, &serial, (void *)1); });
    for (uint32_t i = 0; i < g_pkg_share.cap; i++) {
        nvn_hmap *pm = g_pkg_share.vals[i]; if (!pm) continue;
        uint32_t ns = 0; alias_key *ks = NULL;
        for (uint32_t j = 0; j < pm->cap; j++) {
            NvPkgRef *r = (__bridge NvPkgRef *)pm->vals[j]; if (!r) continue;
            NvPkg *p = r->pkg; if (!p || ![victims containsObject:p]) continue;
            alias_key *nk = realloc(ks, (ns + 1) * sizeof *ks); if (!nk) break;
            ks = nk; memcpy(&ks[ns++], pm->keys + (size_t)j * pm->klen, sizeof *ks);
        }
        for (uint32_t j = 0; j < ns; j++) hm_del_id(pm, &ks[j]);
        free(ks);
    }
    for (NvPkg *pk in victims) { n++; bytes += pk->tex.allocatedSize + (pk->spare && !pk->spare.heap ? pk->spare.allocatedSize : 0); }
    pthread_mutex_unlock(&g_smu);
    for (int i = 1; i < g_ntreg; i++) {
        treg_t *r = &g_treg[i];
        if (!r->ok || !r->t.serial || !nvn_hm_get(&gone_ser, &r->t.serial)) continue;
        treg_drop_views(r); r->evicted = 1; reg_bump(i, 0); pool_seen_clear(i);
    }
    // The view cache is keyed by the Metal texture: its views retain their parent and would keep the freed copy resident.
    for (NvPkg *pk in victims) { uint64_t a = vc_addr(pk->tex); vc_set *v = nvn_hm_del(&g_vc, &a); if (v) vc_set_free(v); }
    os_unfair_lock_unlock(&g_rmu);
    free(gone_ser.keys); free(gone_ser.vals);
    atomic_fetch_add(&g_evict_n, n); atomic_fetch_add(&g_evict_bytes, bytes);
    if (n_out) *n_out = n;
    return bytes;
} }
// under 6 GB of working set (8 GB Macs) sweep without waiting for memory pressure
static int small_working_set(uint64_t working_set) { return working_set && working_set < (6ull << 30); }
static int g_sweep_on = -1;
static uint32_t g_sweep_last;
static void pkg_sweep(void) {
    if (g_sweep_on < 0) {
        const char *e = getenv("DM_NVN_TEX_EVICT_IDLE"); g_sweep_on = e && *e ? atoi(e) > 0 : small_working_set(g_dev ? g_dev.recommendedMaxWorkingSetSize : 0);
        if (g_sweep_on) MLOG("uploaded textures nothing used for %d s are freed every %d s, not only at memory pressure (%s)", PKG_IDLE_DS / 10, PKG_SWEEP_DS / 10, e && *e ? "DM_NVN_TEX_EVICT_IDLE" : "Metal's working set under 6 GB");
    }
    if (!g_sweep_on || (g_sweep_last && g_tu_now >= g_sweep_last && g_tu_now - g_sweep_last < PKG_SWEEP_DS)) return;
    g_sweep_last = g_tu_now;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); unsigned n = 0; uint64_t b = pkg_evict_idle(&n);
    if (!n) return;
    atomic_fetch_add(&g_sweep_n, n); atomic_fetch_add(&g_sweep_bytes, b);
    static int nl; if (nl++ < 8) MLOG("[mem] idle uploaded textures freed: %u, %llu MB, %.1f ms (in all %llu, %llu MB)", n, (unsigned long long)(b >> 20), (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6,
                                     (unsigned long long)atomic_load(&g_sweep_n), (unsigned long long)(atomic_load(&g_sweep_bytes) >> 20));
}
static uint64_t pool_walk(int samp, int dtt, int dpf, int base, __unsafe_unretained id<MTLBuffer> *out);
static uint64_t pool_gpu(int samp, int dtt, int dpf, int base, __unsafe_unretained id<MTLBuffer> *out) {
    g_pool_walk = 1;
    uint64_t r = pool_walk(samp, dtt, dpf, base, out);
    g_pool_walk = 0;
    return r;
}
static uint64_t pool_walk(int samp, int dtt, int dpf, int base, __unsafe_unretained id<MTLBuffer> *out) {
    pool_seen_scan();
    uint64_t rg = atomic_load(&g_reg_gen), sg = samp ? 0 : atomic_load(&g_ser_gen);
    pool_snap *p = NULL, *free_ = NULL;
    for (int i = 0; i < POOL_SNAPS && !p; i++) {
        pool_snap *q = &g_psnap[i];
        if (!q->buf) { if (!free_) free_ = q; }
        else if (q->samp == samp && q->dtt == dtt && q->dpf == dpf && q->base == base) p = q;
    }
    int stale = p && p->sub && p->sub != g_sub_serial;
    // the walk goes on next frame unless it would build the same table
    int walk_on = p && p->left && p->frame != atomic_load(&g_cur_frame) && (p->left_req || !pool_budget_full() || (!p->over && stand_on()));
    if (p && p->reg == rg && p->ser == sg && !stale && !walk_on) { *out = p->buf; return p->gpu; }
    // patched (nvn-objects.md#pool-tables-patched-instead-of-built-whole): kept when no entry changes
    int nn = (samp ? g_nsreg : g_ntreg) - base; if (nn < 1) nn = 1;
    if (p && !walk_on && (samp || dtt != MTLTextureTypeTextureBuffer)) do {
        if (rg - p->reg >= REG_LOG || sg - p->ser >= SER_LOG) { g_stat_pool_nopatch[0]++; break; }
        if (*(const uint32_t *)p->buf.contents != (uint32_t)nn) { g_stat_pool_nopatch[2]++; break; }
        static int *ids;
        static size_t idcap;
        size_t nid = 0; int known = 1, pools = 0;
        #define POOL_ID(x) do { if (nid == idcap) { idcap = idcap ? idcap * 2 : 256; ids = dm_realloc(ids, idcap * sizeof *ids, "the pool table patch"); } ids[nid++] = (x); } while (0)
        for (uint64_t g = p->reg + 1; g <= rg && known; g++) {
            int nid_, nsamp; known = reg_read(g, &nid_, &nsamp);
            if (!known) break;
            if (nsamp == REG_POOL) pools = !samp;
            else if (nsamp == (samp != 0)) POOL_ID(nid_);
        }
        if (known && p->ser != sg) {
            for (uint64_t g = p->ser + 1; g <= sg && known; g++) {
                uint64_t serial = 0; known = own_note_at(g, &serial);
                if (known) for (int k = (int)(uintptr_t)nvn_hm_get(&g_treg_serial, &serial); k && nid < POOL_IDS_MAX; k = g_treg[k - 1].next) POOL_ID(k - 1);
            }
        }
        if (known && pools) {
            if (p->dead.n < 0) { g_stat_pool_nopatch[1]++; break; }
            for (int k = 0; k < p->dead.n; k++) POOL_ID(p->dead.v[k]);
        }
        if (known && stale) {
            if (p->trans.n < 0) { g_stat_pool_nopatch[4]++; break; }
            for (int k = 0; k < p->trans.n; k++) POOL_ID(p->trans.v[k]);
        }
        #undef POOL_ID
        if (!known) { g_stat_pool_nopatch[3]++; break; }
        if (nid >= POOL_IDS_MAX) { g_stat_pool_nopatch[0]++; break; }
        uint64_t *e = (uint64_t *)((uint8_t *)p->buf.contents + DMSH_POOL_HEAD), dummy = e[nn], room = samp ? UINT64_MAX : pool_room(), spent = g_walk_spent;
        int gone = 0, left = 0, left_req = 0; id<MTLBuffer> b = nil; uint64_t *be = NULL, lpos = 0;
        pool_ids nd = {0}, nt = {0}; ids_begin(0); ids_begin(1);
        if (!pools) ids_keep(0, &nd, &p->dead);    // they stay on the list for the next pool change
        if (!stale) ids_keep(1, &nt, &p->trans);
        for (size_t k = 0; k < nid; k++) {
            int i = ids[k] - base; if (i < 0 || i >= nn) continue;
            uint64_t used = g_walk_spent - spent, limit = used < room ? room - used : 0;
            int d = 0, t = 0; uint64_t r = pool_entry(samp, dtt, ids[k], limit, &gone, &left, &left_req, &t, &d, NULL); r = r ? r : dummy;
            if (d) ids_add(0, &nd, ids[k]);
            if (t) ids_add(1, &nt, ids[k]);
            if (r == (be ? be[i] : e[i])) continue;
            if (!b) { b = pool_patch_take(p); be = (uint64_t *)((uint8_t *)b.contents + DMSH_POOL_HEAD); lpos = g_plog[p - g_psnap].n; }
            be[i] = r; plog_add(p, (uint32_t)i, r);
        }
        g_stat_pool_pchg += pools; g_stat_pool_schg += stale; free(p->dead.v); free(p->trans.v);
        p->reg = rg; p->ser = sg; p->sub = nt.n ? g_sub_serial : 0; p->dead = nd; p->trans = nt;
        if (left) { p->left += left; p->left_req += left_req; p->frame = atomic_load(&g_cur_frame); }
        if (!b) { g_stat_pool_kept++; *out = p->buf; return p->gpu; }
        pool_table_give(p->buf, (int)(p - g_psnap), lpos); p->buf = b; p->gpu = b.gpuAddress;
        g_psnap_gen++; g_stat_pool_patched++;
        *out = b; return p->gpu;
    } while (0);
    // past POOL_SUB_MS of walks a submission the table stands until the next one, unless it holds views from an earlier submission
    if (g_walk_sub != g_sub_serial) { g_walk_sub = g_sub_serial; g_walk_ms = 0; }
    if (p && p->buf && g_walk_ms >= POOL_SUB_MS && !stale && (samp || dtt != MTLTextureTypeTextureBuffer)) { *out = p->buf; return p->gpu; }
    g_stat_pool_why[!p ? 0 : p->ser != sg ? 2 : p->reg != rg ? 1 : stale ? 3 : 4]++;
    if (!p) p = free_ ? free_ : &g_psnap[g_psnap_next++ % POOL_SNAPS];
    uint64_t t0 = mach_absolute_time();
    int n = (samp ? g_nsreg : g_ntreg) - base; if (n < 1) n = 1;
    size_t metadata = !samp && dtt == MTLTextureTypeTextureBuffer ? 4 * (size_t)(n + 1) : 0;
    id<MTLBuffer> b = pool_table_take(DMSH_POOL_HEAD + 8 * (size_t)(n + 1) + metadata);
    uint8_t *c = b.contents; *(uint32_t *)c = (uint32_t)n;
    if (!samp) *(uint64_t *)(c + 8) = pool_seen_gpu_t(dtt);
    uint32_t *leads = metadata ? (uint32_t *)(void *)(c + DMSH_POOL_HEAD + 8 * (size_t)(n + 1)) : NULL;
    *(uint32_t *)(void *)(c + DMSH_POOL_BIAS) = leads ? (uint32_t)((uint8_t *)leads - c) : 0;
    if (leads) memset(leads, 0, metadata);
    uint64_t *e = (uint64_t *)(c + DMSH_POOL_HEAD), dummy = samp ? dummy_samp_rid() : (dummy_tex(dtt, dpf) ? g_dummy_tex_rid[dtt % DMY_NT][dpf & 3] : 0);
    int real = 0, gone = 0, left = 0, left_req = 0; uint64_t limit = samp ? UINT64_MAX : pool_walk_limit(base, n);
    pool_ids nd = {0}, nt = {0}; ids_begin(0); ids_begin(1);
    for (int i = 0; i < n; i++) {
        int d = 0, t = 0; uint64_t r = pool_entry(samp, dtt, base + i, limit, &gone, &left, &left_req, &t, &d, leads ? leads + i : NULL);
        if (d) ids_add(0, &nd, base + i);
        if (t) ids_add(1, &nt, base + i);
        e[i] = r ? r : dummy; real += r != 0;
    }
    e[n] = dummy;
    pool_table_give(p->buf, -1, 0); free(p->dead.v); free(p->trans.v); plog_build(p, n);
    // retired registrations' resource IDs cannot outlive the submission
    *p = (pool_snap){b, b.gpuAddress, rg, sg, atomic_load(&g_cur_frame), nt.n ? g_sub_serial : 0, samp, dtt, dpf, base, left, left_req, nd, nt, samp || g_walk_over};
    g_psnap_gen++;
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    double ms = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6; g_stat_pool_ms += ms;
    if (!samp) g_walk_ms += ms;
    if (!samp && g_stat_pool_builds % 500 == 0) {
        unsigned nseen = 0; uint64_t bseen = 0;
        for (int i = 0; i < n; i++) {
            int id_ = base + i;
            if (id_ > 0 && g_treg[id_].ok && pool_seen(id_)) { nseen++; bseen += pool_tex_size(&g_treg[id_].t); }
        }
        uint64_t pres = 0, pall = 0, tres = 0, tall = 0;
        typedef struct { uint64_t lo, hi; } rng;
        rng *rs = malloc(sizeof *rs * (size_t)n); int nr = 0;
        for (int i = 0; i < n && rs; i++) {
            int id_ = base + i;
            if (id_ > 0 && g_treg[id_].ok && !g_treg[id_].gone && g_treg[id_].t.mem) { rs[nr].lo = (uint64_t)(uintptr_t)g_treg[id_].t.mem; rs[nr].hi = rs[nr].lo + pool_tex_size(&g_treg[id_].t); nr++; }
        }
        if (rs) {
            qsort(rs, (size_t)nr, sizeof *rs, pool_size_cmp);
            pthread_mutex_lock(&g_mu);
            for (int k = 0; k < g_npool; k++) { uint64_t sz = g_pools[k].hi - g_pools[k].lo; pall += sz; if (g_pools[k].res) pres += sz; }
            uint64_t end = 0;
            for (int i = 0; i < nr; i++) {
                uint64_t lo = MAX(rs[i].lo, end), hi = rs[i].hi;
                if (hi <= lo) continue;
                end = hi; tall += hi - lo;
                int q = pool_peek(lo);
                if (q >= 0 && g_pools[q].res) tres += hi - lo;
            }
            pthread_mutex_unlock(&g_mu); free(rs);
        }
        MLOG("pool walk: tables made %llu, reused %llu; passes ended after a pool shader that may read depth %llu", (unsigned long long)g_pool_made, (unsigned long long)g_pool_reused, (unsigned long long)g_fb_split_pool);
        MLOG("pool walk: %d IDs, %d left out, %u read by a shader (%llu MB), allocated %llu MB; memory pools %llu MB, wired %llu MB; texture storage %llu MB, in wired pools %llu MB", n, left, nseen, (unsigned long long)(bseen >> 20), (unsigned long long)(g_dev.currentAllocatedSize >> 20), (unsigned long long)(pall >> 20), (unsigned long long)(pres >> 20), (unsigned long long)(tall >> 20), (unsigned long long)(tres >> 20));
    }
    static int gone_logged; int log_gone = gone && !gone_logged++;
    if (g_stat_pool_builds++ < 16 || g_stat_pool_builds % 1000 == 0 || log_gone)
        MLOG("pool for handles traced to no slot: %s pool (texture type %d, class %d) at base %d, %d IDs of which %d hold %s and %d have no storage (memory pool finalized or gone), built in %.2f ms (%llu builds, %.0f ms in all; "
             "built because new %llu, registration %llu, storage %llu, retired IDs %llu, textures left out %llu; taken alone: %llu tables patched, %llu kept (%llu over memory pool changes, %llu over a new submission); "
             "not: too many %llu, a pool change over too many IDs without storage %llu, a new submission over too many retired IDs %llu, resized %llu, a change without a note %llu)", samp ? "sampler" : "texture",
             samp ? -1 : dtt, samp ? -1 : dpf, base, n, real, samp ? "samplers" : "textures of the type", gone, ms, (unsigned long long)g_stat_pool_builds, g_stat_pool_ms,
             (unsigned long long)g_stat_pool_why[0], (unsigned long long)g_stat_pool_why[1], (unsigned long long)g_stat_pool_why[2], (unsigned long long)g_stat_pool_why[3], (unsigned long long)g_stat_pool_why[4],
             (unsigned long long)g_stat_pool_patched, (unsigned long long)g_stat_pool_kept, (unsigned long long)g_stat_pool_pchg, (unsigned long long)g_stat_pool_schg,
             (unsigned long long)g_stat_pool_nopatch[0], (unsigned long long)g_stat_pool_nopatch[1], (unsigned long long)g_stat_pool_nopatch[4], (unsigned long long)g_stat_pool_nopatch[2], (unsigned long long)g_stat_pool_nopatch[3]);
    *out = b; return p->gpu;
}
// Once a second, shared render targets drawn over and unused for TU_IDLE go; TU_MAX a pass: each serial is a note.
enum { TU_IDLE = 30, TU_MAX = 256 };   // deciseconds; textures a pass
static uint32_t g_tu_last;
static uint64_t g_tu_n, g_tu_bytes, g_tu_serials, g_tu_pinned, g_tu_ns, g_tu_passes;
static int tu_use_of(id o, own_use *u) {
    uintptr_t k = (uintptr_t)(__bridge void *)o;
    os_unfair_lock_lock(&g_tu_lk);
    tu_rec *r = nvn_hm_get(&g_tu, &k);
    if (r) { r->pass = g_tu_pass_no; u->use = r->use; u->wr = r->wr; }
    os_unfair_lock_unlock(&g_tu_lk);
    return r != NULL;
}
static void tu_pass(void) { @autoreleasepool {
    uint64_t t0 = bp_now(); uint32_t now = g_tu_now, since = g_tu_last; g_tu_last = now;
    os_unfair_lock_lock(&g_tu_lk); g_tu_pass_no++; os_unfair_lock_unlock(&g_tu_lk);
    os_unfair_lock_lock(&g_rmu);
    seen_stamp_used(now, 1);
    if (g_tu.n) {
        os_unfair_lock_lock(&g_tu_lk);
        for (int i = 1; i < g_ntreg; i++) {
            const treg_t *r = &g_treg[i];
            if (!r->ok || !r->view || r->used < since) continue;
            tu_rec *rec = tu_find((__bridge id)r->view);
            if (rec && rec->use < r->used) rec->use = r->used;
        }
        os_unfair_lock_unlock(&g_tu_lk);
    }
    os_unfair_lock_unlock(&g_rmu);
    if (!g_tu.n) return;
    uint64_t *sers = NULL; uint32_t nser = 0;
    pthread_mutex_lock(&g_smu);
    own_dead_stats st = own_alias_dead_locked(now, TU_IDLE, tu_use_of, TU_MAX, &sers, &nser);
    pthread_mutex_unlock(&g_smu);
    if (nser) {
        os_unfair_lock_lock(&g_rmu);
        for (uint32_t i = 0; i < nser; i++) for (int k = (int)(uintptr_t)nvn_hm_get(&g_treg_serial, &sers[i]); k; k = g_treg[k - 1].next) treg_drop_views(&g_treg[k - 1]);
        os_unfair_lock_unlock(&g_rmu);
    }
    os_unfair_lock_lock(&g_tu_lk);
    if (g_tu_gone.n > 65536) nvn_hm_clear(&g_tu_gone, NULL);
    for (uint32_t i = 0; i < nser; i++) nvn_hm_put(&g_tu_gone, &sers[i], (void *)1);
    uintptr_t *ks = NULL; uint32_t nk = 0;
    for (uint32_t i = 0; i < g_tu.cap; i++) {
        tu_rec *r = g_tu.vals[i];
        if (!r || r->pass == g_tu_pass_no) continue;
        if (!(nk & (nk + 1))) { uintptr_t *n = realloc(ks, (nk * 2 + 2) * sizeof *ks); if (!n) break; ks = n; }
        memcpy(&ks[nk++], g_tu.keys + (size_t)i * g_tu.klen, sizeof *ks);
    }
    for (uint32_t i = 0; i < nk; i++) free(nvn_hm_del(&g_tu, &ks[i]));   // collected first: a removal shifts entries
    uint32_t live = g_tu.n; uint64_t remade = g_tu_remade;
    os_unfair_lock_unlock(&g_tu_lk);
    free(ks); free(sers);
    g_tu_n += st.n; g_tu_bytes += st.bytes; g_tu_serials += st.serials; g_tu_pinned += st.pinned; g_tu_ns += bp_now() - t0; g_tu_passes++;
    static uint64_t at, was; uint64_t f = atomic_load(&g_cur_frame);
    if (f >= at + 300 && g_tu_n != was) {
        at = f; was = g_tu_n;
        MLOG("render targets drawn over and unused for %u s, released up to frame %llu: %llu textures, %llu MB (%llu serial entries with them; %llu kept by a pin), %llu made again; %u covered now; %llu passes, %.2f ms in all",
             TU_IDLE / 10, (unsigned long long)f, (unsigned long long)g_tu_n, (unsigned long long)(g_tu_bytes >> 20), (unsigned long long)g_tu_serials, (unsigned long long)g_tu_pinned, (unsigned long long)remade, live,
             (unsigned long long)g_tu_passes, g_tu_ns / 1e6);
    }
} }
static int g_tu_manual;
static int tu_mode(void) { static int m = -1; if (m < 0) m = sw_mode("DM_NVN_RT_DEAD", 1); return m; }
static void tu_tick(void) {   // DM_NVN_RT_DEAD=0 keeps them all
    if (g_tu_manual) return;
    static uint64_t t0; uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); if (!t0) t0 = ns;
    g_tu_now = (uint32_t)((ns - t0) / 100000000ull) + 1;
    if (g_tu_now - g_tu_last >= 10) {
        if (sw_on(tu_mode())) tu_pass();
        else { g_tu_last = g_tu_now; os_unfair_lock_lock(&g_rmu); seen_stamp_used(g_tu_now, 1); os_unfair_lock_unlock(&g_rmu); }
    }
}
static id<MTLBuffer> dummy_buf(void) {
    static id<MTLBuffer> b;
    if (!b) { b = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared]; res_add(b); }
    return b;
}
static uint64_t dummy_buf_gpu(void) { static uint64_t gpu; if (!gpu) gpu = dummy_buf().gpuAddress; return gpu; }
