// Argument buffers' stand-ins and texture pool tables: the dummy textures and the write sink, buffer texture leads, pool tables and their patches, stand-ins for textures not made, sweeps of idle textures, the texture use clock.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// Argument buffer: filled item by item per the B reflection (uniform slot r, separate texture / sampler slot r, storage buffer slot r, driver segments c0 / c2, c1, support_buffer)
// Unbound resources read 0: give all-zero dummy resources (NVN descriptors 0..255 are reserved for the driver; the game binding handle 0 means 'this slot is empty')
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
        // A multisample slot takes only a multisample texture, which lives in private storage: cleared to 0 by a render pass (128-bit integer formats have no multisample form)
        if (dpf == NVN_DPF_UINT) d.pixelFormat = MTLPixelFormatRGBA16Uint; else if (dpf == NVN_DPF_SINT) d.pixelFormat = MTLPixelFormatRGBA16Sint;
        d.sampleCount = 4; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget;
        if (!(t = [g_dev newTextureWithDescriptor:d])) { RED_ONCE(139, "cannot create the zero multisample texture (type %lu, Metal format %lu)", (unsigned long)tt, (unsigned long)d.pixelFormat); return nil; }
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        if (dpf == NVN_DPF_DEPTH) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = MTLLoadActionClear; rp.depthAttachment.clearDepth = 0; rp.depthAttachment.storeAction = MTLStoreActionStore; }
        else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = MTLLoadActionClear; rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0); rp.colorAttachments[0].storeAction = MTLStoreActionStore; }
        id<MTLCommandBuffer> cb = cl_cb(); [[cb renderCommandEncoderWithDescriptor:rp] endEncoding]; [cb commit]; [cb waitUntilCompleted];
        cache[dtt % DMY_NT][dpf & 3] = t; g_dummy_tex_rid[dtt % DMY_NT][dpf & 3] = t.gpuResourceID._impl; res_add(t);
        return t;
    }
    t = [g_dev newTextureWithDescriptor:d];   // Shared storage is all zeros when newly created
    if (pf != MTLPixelFormatDepth32Float) { uint8_t z[64] = {0}; NSUInteger faces = tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1;
        for (NSUInteger f = 0; f < faces; f++) [t replaceRegion:MTLRegionMake3D(0, 0, 0, 1, 1, 1) mipmapLevel:0 slice:f withBytes:z bytesPerRow:16 bytesPerImage:16]; }
    cache[dtt % DMY_NT][dpf & 3] = t; g_dummy_tex_rid[dtt % DMY_NT][dpf & 3] = t.gpuResourceID._impl; res_add(t);
    return t;
}
// Write sink for unbound image slots: kept separate from the dummy textures that read 0 (writes must not pollute them); one is shared per same type and same pixel class
static id<MTLTexture> dummy_img(int dtt, int dpf) {
    static __strong id<MTLTexture> cache[8][4];
    id<MTLTexture> t = cache[dtt & 7][dpf & 3]; if (t) return t;
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = (MTLTextureType)dtt; d.pixelFormat = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA16Float;
    d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModePrivate; d.usage = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite;
    t = [g_dev newTextureWithDescriptor:d]; cache[dtt & 7][dpf & 3] = t; g_dummy_img_rid[dtt & 7][dpf & 3] = t.gpuResourceID._impl; res_add(t); return t;
}
static id<MTLBuffer> dummy_buf(void);
static id<MTLTexture> dummy_tbuf(int dpf) {   // Unbound typed buffer: all zeros (pixel class matches the shader's texture_buffer<T>)
    static __strong id<MTLTexture> cache[4];
    id<MTLTexture> t = cache[dpf & 3]; if (t) return t;
    MTLPixelFormat pf = dpf == NVN_DPF_UINT ? MTLPixelFormatRGBA32Uint : dpf == NVN_DPF_SINT ? MTLPixelFormatRGBA32Sint : MTLPixelFormatRGBA32Float;
    id<MTLBuffer> b = dummy_buf();
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:b.length / 16 resourceOptions:b.resourceOptions usage:MTLTextureUsageShaderRead];
    t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:b.length];
    cache[dpf & 3] = t; g_dummy_tbuf_rid[dpf & 3] = t.gpuResourceID._impl; return t;
}
// A buffer texture's view (tex_mtl, target BUFFER) starts at the aligned boundary below its pool offset, wider by the elements in between: their count, which a shader with an offset table
// entry for the binding adds to its coordinates (the translator's TranslationFlags.MetalBufferImageBias), from the texture's own view and its registered width. 0 for any other texture,
// and for anything bound in its place (a dummy, a copy)
static uint32_t tbuf_lead(int tid, id<MTLTexture> t) {
    if (!t || t.textureType != MTLTextureTypeTextureBuffer || tid <= 0 || tid >= g_ntreg || !g_treg[tid].ok || g_treg[tid].t.d.target != NVN_TEXTURE_TARGET_BUFFER) return 0;
    id<MTLTexture> root = t; while (root.parentTexture) root = root.parentTexture;   // a format or swizzle view keeps its parent's buffer and width
    void *ov = NULL; tex_fast_rid(tid, &ov); id<MTLTexture> own = (__bridge id<MTLTexture>)ov; while (own.parentTexture) own = own.parentTexture;   // the ID's view may be a swizzle view of it
    if (root != own) return 0;
    uint32_t n = g_treg[tid].t.d.width;
    return root.width > n ? (uint32_t)(root.width - n) : 0;
}
// A buffer texture whose view starts before its offset (tbuf_lead), read through a binding with no offset table entry (a shader whose buffer textures take more than the table's
// DMSH_TBIAS_SLOTS entries), is read through a copy of its own bytes at an aligned offset of a shared staging block, made at its first binding in each submission. The fallback of
// the offset table: FINAL FANTASY RESONANCE's particle vertex factories read their CPU-written particle data so (village fires, smoke and muzzle flashes drawn at the wrong scale or
// flickering) before their slot arrays had entries. The copy holds what the CPU wrote before the submission was encoded: a GPU write in the same submission, or in one still running,
// is not seen, and a pool the CPU cannot access is bound as is (reported once). Caller holds g_rmu
enum { TBS_BLOCK = 4u << 20, TBS_CACHE = 512 };
static id<MTLBuffer> g_tbs_blk; static size_t g_tbs_off; static NSMutableArray *g_tbs_views;
static struct { __unsafe_unretained id<MTLTexture> src, copy; uint64_t epoch, rid; } g_tbs_cache[TBS_CACHE];
static uint64_t g_tbs_copies;
static id<MTLTexture> tbuf_shadow(int tid, id<MTLTexture> t, uint64_t *rid) {
    uint32_t lead = tbuf_lead(tid, t);
    if (!lead) return t;
    id<MTLTexture> root = t; while (root.parentTexture) root = root.parentTexture;   // a swizzled view keeps its parent's buffer
    int bw, bh, bpb = nvn_fmt_bpb(g_treg[tid].t.d.format, &bw, &bh);
    id<MTLBuffer> src = root.buffer; NSUInteger off = root.bufferOffset + (NSUInteger)lead * (NSUInteger)bpb, w = t.width - lead;
    if (!src || bpb <= 0 || off >= src.length) return t;
    // Preserve the unsupported-storage fallback without making an invalid CPU-access query.
    if (src.storageMode == MTLStorageModePrivate || !src.contents) { RED_ONCE(117, "buffer texture at pool offset %#lx (below Metal's view alignment) in a pool the CPU cannot access, read through a binding with no offset table entry: read %u elements early", (unsigned long)off, lead); return t; }
    unsigned k = (unsigned)(((uintptr_t)(__bridge void *)t >> 4) % TBS_CACHE);
    if (g_tbs_cache[k].src == t && g_tbs_cache[k].epoch == g_fence + 1) { *rid = g_tbs_cache[k].rid; return g_tbs_cache[k].copy; }
    size_t bytes = (size_t)w * (size_t)bpb, row = (bytes + 255u) & ~(size_t)255u, n = bytes < src.length - off ? bytes : src.length - off;   // 256: a multiple of every view alignment
    if (!g_tbs_blk || g_tbs_off + row > g_tbs_blk.length) {   // a full block and its views are released once the submissions that read them are done
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
    static id<MTLSamplerState> s; if (!s) { MTLSamplerDescriptor *d = [MTLSamplerDescriptor new]; d.supportArgumentBuffers = YES; s = [g_dev newSamplerStateWithDescriptor:d]; }
    return s;
}
static uint64_t dummy_samp_rid(void) { static uint64_t rid; if (!rid) rid = dummy_samp().gpuResourceID._impl; return rid; }
// The pools a handle traced to no slot reads (nvn_refl.h TEXP / SAMPP, dmsh_reflect.py pool_arrays): Mario Party Superstars' characters take their light textures' handles from a storage
// buffer. One buffer per element type and pool base: the bound n (DMSH_POOL_HEAD bytes), then the resource ID of each ID of the pool bound at the draw (base = S.tex_base / S.samp_base,
// as ubo_tex_id adds), the dummy where an ID holds nothing or a texture of another type, and the dummy at n for IDs past the table. Patched or rebuilt when a registration or a texture's
// storage changes (g_reg_gen, g_ser_gen); the old buffer goes when the submissions that used it have completed. Every texture is in the residency set already (res_add at creation). Caller holds g_rmu
#define POOL_SNAPS 16
// Each table keeps two lists of IDs, so that a change takes again only the IDs it can reach (n -1: more than POOL_IDS_MAX of them, and that change builds the table whole):
// dead: the IDs a texture table holds the dummy for because their storage did not exist (pool_entry), which a memory pool added later can bring (REG_POOL);
// trans: the IDs whose entry is a view of a retired registration made in submission sub, which goes at that submission's end (treg_finish_submission), so the next submission
// takes them again. Walking through Ocarina of Time's Kokiri Forest, about 100 of its 33,025 IDs hold such a view; more than 1,024 while it streams an area in behind its pause menu.
// A list holds as many IDs as one patch takes (pool_gpu): with 1,024 as its limit, each submission behind that menu built the table whole (1,927 of about 2,000 builds,
// 1.4-1.6 a frame of 9 ms, 50-55 fps for five seconds; PR #2389)
enum { POOL_IDS_MAX = 8192 };   // the IDs a patch takes at most, and a list holds
typedef struct { int *v, n, cap; } pool_ids;
typedef struct { __strong id<MTLBuffer> buf; uint64_t gpu, reg, ser, frame, sub; int samp, dtt, dpf, base, left, left_req; pool_ids dead, trans; int over; } pool_snap;   // over: its last walk found the budget full, so what it left out has its stand-in (pool_stand)
static pool_snap g_psnap[POOL_SNAPS]; static unsigned g_psnap_next;
// Each table's log of the entries written into it since its build: the patches' (pool_walk) and the in-place writes (pool_seen_scan, pool_stand_scan), so a spare the table left at log
// position lpos is brought up to date by the entries after it (pool_patch_take). A ring of a quarter of the table's entries: past that a whole copy is cheaper than the writes. build: the
// table's build (g_pool_build), which a spare names, so a spare of an earlier build, or of another table in that slot, is never brought up to date from this log
static struct { uint32_t *at; uint64_t *v; uint64_t n, build; uint32_t cap; } g_plog[POOL_SNAPS]; static uint64_t g_pool_build;
static inline void plog_add(const pool_snap *q, uint32_t at, uint64_t v) {
    int k = (int)(q - g_psnap); if (!g_plog[k].cap) return;
    uint32_t j = (uint32_t)(g_plog[k].n++ % g_plog[k].cap); g_plog[k].at[j] = at; g_plog[k].v[j] = v;
}
static void plog_build(const pool_snap *q, int n) {   // q's table built anew with n IDs: its log starts over
    int k = (int)(q - g_psnap); uint32_t cap = MAX(64u, (uint32_t)(n + 1) / 4);
    if (cap != g_plog[k].cap) { g_plog[k].at = dm_realloc(g_plog[k].at, cap * sizeof *g_plog[k].at, "a pool table's log"); g_plog[k].v = dm_realloc(g_plog[k].v, cap * sizeof *g_plog[k].v, "a pool table's log"); g_plog[k].cap = cap; }
    g_plog[k].n = 0; g_plog[k].build = ++g_pool_build;
}
static uint64_t g_psnap_gen = 1;   // bumped by each rebuild or patch: an argument block holding a pool address is not reused past it (encode_args_)
static uint64_t g_stat_pool_builds; static double g_stat_pool_ms;
static uint64_t g_stat_pool_why[5];   // why a table was built whole: no table of its kind, a registration changed (g_reg_gen), a texture's storage changed (g_ser_gen), retired IDs of an earlier submission, textures left out (the walk goes on)
// Tables patched or kept on noted changes alone; why not: [0] too many changes or a sparse texture, [1] a memory pool change over more than POOL_IDS_MAX IDs without storage,
// [2] resized, [3] a change without a note, [4] a new submission over more than POOL_IDS_MAX retired IDs. pchg / schg: of those patched or kept, the ones over memory pool changes
// and over a new submission
static uint64_t g_stat_pool_patched, g_stat_pool_kept, g_stat_pool_nopatch[5], g_stat_pool_pchg, g_stat_pool_schg;
static void ids_push(pool_ids *l, int id_) {   // past POOL_IDS_MAX the list is dropped (n -1)
    if (l->n < 0) return;
    if (l->n == POOL_IDS_MAX) { free(l->v); *l = (pool_ids){NULL, -1, 0}; return; }
    if (l->n == l->cap) { l->cap = l->cap ? l->cap * 2 : 16; l->v = dm_realloc(l->v, (size_t)l->cap * sizeof *l->v, "the pool tables' ID lists"); }
    l->v[l->n++] = id_;
}
// A list being made: each ID once (a stamp per ID, one stamp table per kind of list: [0] dead, [1] trans)
static struct { uint32_t *stamp; int cap; uint32_t cur; } g_pids[2];
static void ids_begin(int k) { if (!++g_pids[k].cur) { if (g_pids[k].stamp) memset(g_pids[k].stamp, 0, (size_t)g_pids[k].cap * sizeof *g_pids[k].stamp); g_pids[k].cur = 1; } }
static void ids_add(int k, pool_ids *l, int id_) {
    if (l->n < 0 || id_ < 0) return;
    if (id_ >= g_pids[k].cap) { int c = id_ + 4096; g_pids[k].stamp = dm_realloc(g_pids[k].stamp, (size_t)c * sizeof *g_pids[k].stamp, "the pool tables' ID lists");
        memset(g_pids[k].stamp + g_pids[k].cap, 0, (size_t)(c - g_pids[k].cap) * sizeof *g_pids[k].stamp); g_pids[k].cap = c; }
    if (g_pids[k].stamp[id_] == g_pids[k].cur) return;
    g_pids[k].stamp[id_] = g_pids[k].cur;
    ids_push(l, id_);
}
static void ids_keep(int k, pool_ids *l, const pool_ids *old) {   // an earlier list carried over
    if (old->n < 0) { free(l->v); *l = (pool_ids){NULL, -1, 0}; return; }
    for (int i = 0; i < old->n; i++) ids_add(k, l, old->v[i]);
}
// Whether a registered texture's storage still exists, before pool_gpu makes a texture no draw has bound: a game leaves IDs registered after it is done with their textures, and the
// first pool walk in Mario Party Superstars crashed once in nvn_sparse_protect (pkg_upload ← tex_by_id ← pool_gpu) on a garbage pool->vp, which ID it was was not caught, as FINAL
// FANTASY RESONANCE did later (GitHub issue #934; treg_gone, tex_mtl_ now stop that on every route). So: its memory pool object is live (nvnMemoryPoolFinalize clears the magic; freed
// and reused memory does not hold it) and an ordinary pool's storage lies in a pool the host maps (mtl_addr; otherwise the upload would read the guest address on the CPU)
static int tex_storage_live(const tex_t *t) {
    const pool_t *p = t->pool;
    if (p && p->magic != MAGIC('P', 'O', 'O', 'L')) return 0;
    if (p && nvn_sparse_pool(p)) return 1;
    return mtl_addr_known((uint64_t)(uintptr_t)t->mem, t->L.size ? t->L.size : 1);
}
// A handle read from game memory may name any registered texture, and on the console that costs nothing: the data is in the pool, and a handle is a number. Here each is a Metal texture beside the
// pool's copy, and making them all took Ocarina of Time's Kokiri Forest past Metal's 12.1 GB working set on a 16 GB Mac, once into a kernel panic in IOGPUGroupMemory at 14.7 GB wired. Shaders read few of
// them: about 300 of its 33,000 IDs, about 1 GB. So a shader's read sets the ID's bit (pool_seen, dmsh_reflect.py pool_arrays) and those are made whatever the budget, up to POOL_REQ_BYTES a frame, which
// was also the black scenery: a budget without this left out exactly the large textures those shaders read. A walk still makes the smallest textures up front, POOL_FRAME_BYTES a frame up to a twentieth of
// the working set, as a pass that runs once (a bake) has no second frame to find its texture in. Each upload reads pages the system may have swapped out and wires them: one 300-frame window spent 8.6 s
// in residency commits. A read of the rest finds the dummy for a few frames
enum { POOL_FRAME_BYTES = 32u << 20, POOL_REQ_BYTES = 128u << 20, POOL_SUB_MS = 2 };
static uint64_t g_walk_frame = ~0ull, g_walk_spent, g_walk_sub = ~0ull; static double g_walk_ms; static int g_walk_over;
static uint64_t pool_tex_size(const tex_t *t) { return t->L.size ? t->L.size : 1; }
static int pool_size_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
static uint64_t pool_budget(void) { return g_dev.recommendedMaxWorkingSetSize / 20; }
static uint64_t g_walk_req;
// A render target no pass has written holds nothing to read, and a pass makes its texture when it renders. Made for a read, one that only registers next frame's target took three times the
// textures and 17 GB allocated on Ocarina of Time: the game re-registers its IDs with transient targets, whose IDs a shader read as the previous texture
static int pool_unwritten_target(const tex_t *t) { return (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && t->serial && !tex_get(t->serial) && !pkg_shared(t); }
static int pool_budget_full(void) { return g_dev.currentAllocatedSize >= pool_budget(); }
static uint64_t pool_room(void) {   // what this frame may still make: its share of POOL_FRAME_BYTES, within the budget
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_walk_frame) { g_walk_frame = f; g_walk_spent = 0; g_walk_req = 0; }
    uint64_t want = pool_budget(), have = g_dev.currentAllocatedSize, room = have < want ? want - have : 0;
    g_walk_over = have >= want;
    uint64_t left = g_walk_spent < POOL_FRAME_BYTES ? POOL_FRAME_BYTES - g_walk_spent : 0; if (room > left) room = left;
    return room;
}
static uint64_t pool_walk_limit(int base, int n) {   // the largest texture this walk may make: this frame's room, spent from the smallest up. UINT64_MAX while they all fit
    uint64_t room = pool_room(), sum = 0;
    if (!room) return 0;   // no need to size a walk that can make nothing (33,024 IDs, a table lookup each)
    uint64_t *z = malloc(sizeof *z * (size_t)n); int m = 0;
    if (!z) return UINT64_MAX;
    for (int i = 0; i < n; i++) { int id_ = base + i; if (id_ > 0 && g_treg[id_].ok && !g_treg[id_].gone && !g_treg[id_].view && !pkg_shared(&g_treg[id_].t)) { z[m] = pool_tex_size(&g_treg[id_].t); sum += z[m]; m++; } }
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
static uint64_t pool_seen_gpu_t(int dtt) { return pool_seen_gpu() + (uint64_t)(dtt >= 0 && dtt < POOL_SEEN_TYPES ? seen_sec(dtt) : 0) * POOL_SEEN_WORDS * 4; }   // the section of a table of type dtt
// Newly read IDs also go into the existing tables in place, as past POOL_SUB_MS of walks a submission a table stands: dummy → texture is safe for a draw already encoded, which only reads the right
// texture sooner. At most POOL_REQ_BYTES of new textures a frame; the rest wait in g_seen_todo. Caller holds g_rmu
static uint32_t g_seen_todo[POOL_SEEN_WORDS];
static uint64_t g_fb_split_pool; static int g_fb_dread_pool;   // the passes ended because a pool shader that may read the depth attachment came before a depth write: if a later area reads its main depth through a pool, this counts what that costs
// The depth and stencil textures shaders have read through a pool, from the bitmap at each submission, by what makes them one Metal texture (alias_key): a pool shader may name any registered texture, and a game can keep its depth attachment in the very pool the draw reads,
// so what the pool could hold says nothing; fb_prepare asks this whether a draw can have read the depth attachment it would otherwise have to end the pass for
static nvn_hmap g_pool_ds = {.klen = sizeof(alias_key)}; static uint64_t g_pool_ds_gen;
static void pool_ds_note(int id_) {
    if (id_ <= 0 || id_ >= g_ntreg || !g_treg[id_].ok || !fmt_depth(g_treg[id_].t.d.format)) return;
    alias_key k = tex_alias_key(&g_treg[id_].t);
    if (!nvn_hm_get(&g_pool_ds, &k)) { nvn_hm_put(&g_pool_ds, &k, (void *)1); g_pool_ds_gen++; }
}
static int pool_ds_read(const tex_t *t) {
    static uint64_t ser = 0, gen = ~0ull; static int read;   // asked for every pool draw of a pass: the answer holds until the target or the set changes
    if (t->serial != ser || g_pool_ds_gen != gen) { alias_key k = tex_alias_key(t); read = nvn_hm_get(&g_pool_ds, &k) != NULL; ser = t->serial; gen = g_pool_ds_gen; }
    return read;
}
// A stand-in for a texture the walk leaves out. Once the budget is full (pool_budget) a texture no shader has read is not made until one reads its ID, and until then its table entry
// was the dummy: the first frame that sampled it read 0, the read set the ID's bit, and the texture was there for the next frame. Ocarina of Time keeps a texture set for each piece
// of distant scenery and resolution (one level each: a BC5 texture, and two three-layer arrays, BC6H and BC7, of half its size) and registers a new set 12 to 969 frames before the
// material names it, so each such step drew its surface dark for one frame: the cliff beside the castle town wall (issue #1969). Which registrations will be read is not known
// ahead: in 58 s of play the game registered 14,455 textures that stayed unread, 9.8 GB, against 10 first reads. So the entry holds a small picture of the texture instead: its own
// levels from the first of at most STAND_LEVEL bytes on, or, when it has no level that small, its last level cut down to at most STAND_PICK bytes a layer, every n-th block of every
// n-th block row (nvn_deswizzle_pick: the blocks stay as they are, so any format Metal has goes). It is read from the storage on the CPU into a shared texture cut from a heap that
// is resident from the start: no upload batch and no residency commit, and a table may name it at once. The first read then samples the texture's own colours, coarse, for the one
// frame before the texture is there. 2D textures and 2D arrays get one, of the type, layers, format and swizzle of the view tex_by_id makes for the ID; other types, sparse pools'
// textures and depth formats hold the dummy as before. Only once the budget is full: below it a later walk makes the texture (and a table walked then is walked once more when the
// budget has filled, pool_snap.over). Made where the walk leaves the ID out, at most STAND_FRAME a frame, as an area load registers thousands at once (Ocarina of Time's Hyrule
// Field: 5,152, 82 ms in one frame without the limit); the rest wait in g_stand_todo and go into the tables in place (pool_stand_scan), as a newly read ID's texture does. In play
// the game registers about 7 a frame, 6 us each on an M4 Pro, and about 6,000 are held at a time, in 16 MB of heaps. DM_NVN_POOL_STANDIN=0: the dummy, as before
enum { STAND_LEVEL = 1024, STAND_PICK = 4096, STAND_LAYERS = 8, STAND_HEAP = 4u << 20, STAND_HEAPS = 32, STAND_FRAME = 512 };
static __strong id<MTLHeap> g_stand_heap[STAND_HEAPS]; static int g_nstand_heap, g_stand_heap_at;
static uint32_t g_stand_todo[POOL_SEEN_WORDS]; static unsigned g_stand_todo_n, g_stand_frame_n; static uint64_t g_stand_frame = ~0ull;
static uint64_t g_stand_made, g_stand_picked, g_stand_full, g_stand_ns, g_first_stand, g_first_dummy, g_first_dummy_big, g_first_stale;   // first reads of a texture not made: those that found a stand-in, the dummy (and of 256 KB or more), a stand-in whose levels the game has written since
static int stand_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_POOL_STANDIN"); v = !(e && !strcmp(e, "0")); } return v; }
static id<MTLTexture> stand_alloc(MTLTextureDescriptor *d) {
    for (int k = 0; k < g_nstand_heap; k++) {   // the heap that last had room first
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
// What a stand-in holds of a texture: its levels from l0 on as they are (n of them, the first of w x h texels), or, when it has no level of STAND_LEVEL bytes or less, its last level
// cut down to nx x ny blocks, every step-th block of every step-th block row (nvn_deswizzle_pick). 0: none (a format without a block size, more levels than its size holds)
typedef struct { unsigned l0, n; uint32_t w, h, step, nx, ny; } stand_shape;
static int stand_shape_of(const tex_t *t, stand_shape *s) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t W = MAX(t->d.width, 1u), H = MAX(t->d.height, 1u), levels = MAX(t->d.levels, 1u), fit = 1;
    while (MAX(W, H) >> fit) fit++;
    if (!bpb || levels > fit) return 0;
    memset(s, 0, sizeof *s);
    unsigned l = 0; uint32_t bx = 0, by = 0;
    for (; l < levels; l++) { bx = (MAX(W >> l, 1u) + bw - 1) / bw; by = (MAX(H >> l, 1u) + bh - 1) / bh; if ((size_t)bx * by * bpb <= STAND_LEVEL) break; }
    if (l < levels) { s->l0 = l; s->n = levels - l; s->w = MAX(W >> l, 1u); s->h = MAX(H >> l, 1u); return 1; }
    if (bpb > 16 || (bpb & (bpb - 1))) return 0;   // (a block in one 16-byte piece of the storage, nvn_deswizzle_pick)
    uint32_t step = (uint32_t)sqrt((double)bx * by * bpb / STAND_PICK); if (step < 2) step = 2;
    while ((size_t)MAX(bx / step, 1u) * MAX(by / step, 1u) * bpb > STAND_PICK) step++;
    s->l0 = levels - 1; s->n = 1; s->step = step; s->nx = MAX(bx / step, 1u); s->ny = MAX(by / step, 1u); s->w = s->nx * bw; s->h = s->ny * bh;
    return 1;
}
// The layers an ID's stand-in holds and its texture type, which are those of the view tex_by_id makes for it: a 2D texture or a 2D array, read as either. 0: another type, or layers the texture lacks
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
// A texture in a sparse pool gets a stand-in too, from the levels it reads when every page behind them is mapped. Such a pool's storage is read through its page table (nvn_sparse_copy:
// an unmapped page of its address range stops the host with SIGBUS), each layer's bytes from the stand-in's first level to the layer's end (at most STAND_SPARSE_GATHER). PRAGMATA puts its
// streamed textures in sparse pools and maps their small levels first: left out by the walk (12 GB allocated against a budget of 0.9 GB), the materials first read at the tower-to-tablet
// cut read the dummy, and the G-buffer's base colour was 0.068 on that frame against 0.18 on the next
enum { STAND_SPARSE_GATHER = 256u << 10 };
static uint64_t g_stand_sparse, g_stand_sparse_unmapped;
// The bytes of layer y of t from level l0 to the layer's end: [*lo, *hi) of its storage. 0 when that is empty or more than STAND_SPARSE_GATHER
static int stand_span(const tex_t *t, unsigned y, unsigned l0, uint64_t *lo, uint64_t *hi) {
    *lo = (uint64_t)y * t->L.layer_size + nvn_tex_level_offset(&t->d, l0); *hi = MIN((uint64_t)(y + 1) * t->L.layer_size, t->L.size);
    return *hi > *lo && *hi - *lo <= STAND_SPARSE_GATHER;
}
// The stand-in's bytes, read from the texture's storage: into the stand-in, and their hash when asked for (pool_first_read). 0 when the storage cannot be read so
static int stand_fill(const tex_t *t, const stand_shape *s, unsigned s0, unsigned ns, id<MTLTexture> into, uint64_t *sum) {
    static uint8_t buf[STAND_PICK], *gat; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); uint64_t h = 1469598103934665603ull;
    int sp = t->pool && nvn_sparse_pool(t->pool);
    for (unsigned y = 0; y < ns; y++) {
        const uint8_t *src = t->mem; uint64_t at = 0, lo, hi;
        if (sp) {   // this layer's bytes from the stand-in's first level on, gathered through the page table; any page unmapped: no stand-in
            if (!stand_span(t, s0 + y, s->l0, &lo, &hi)) return 0;
            if (!gat && !(gat = malloc(STAND_SPARSE_GATHER))) return 0;
            if (nvn_sparse_copy(t->pool, gat, (uint64_t)(uintptr_t)t->mem + lo, hi - lo)) { g_stand_sparse_unmapped++; return 0; }
            src = gat; at = lo;
        }
        for (unsigned k = 0; k < s->n; k++) {
            uint32_t w = s->step ? s->w : MAX(s->w >> k, 1u), hh = s->step ? s->h : MAX(s->h >> k, 1u); size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (hh + bh - 1) / bh;
            if (pitch * rows > sizeof buf) return 0;
            // a gathered layer starts at byte at of the storage: nvn_deswizzle_pick adds the level's whole-storage offset, so it is handed the storage's would-be start
            if (s->step ? nvn_deswizzle_pick(&t->d, s->l0, s0 + y, (const void *)((uintptr_t)src - at), s->step, buf, s->nx, s->ny)
                        : sp ? nvn_deswizzle_from(&t->d, s->l0 + k, s0 + y, src, at, buf, pitch) : nvn_deswizzle(&t->d, s->l0 + k, s0 + y, src, buf, pitch)) return 0;
            if (into) [into replaceRegion:MTLRegionMake2D(0, 0, w, hh) mipmapLevel:k slice:y withBytes:buf bytesPerRow:pitch bytesPerImage:pitch * rows];
            if (sum) for (size_t i = 0; i < pitch * rows; i++) h = (h ^ buf[i]) * 1099511628211ull;
        }
    }
    if (sum) *sum = h;
    return 1;
}
static int pool_first_log(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_POOL_FIRST") != NULL; return v; }
static int stand_make_(int id_);
static int stand_make(int id_) {   // Caller holds g_rmu
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); int made = stand_make_(id_); g_stand_ns += clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
    return made;
}
static int stand_make_(int id_) {
    treg_t *r = &g_treg[id_]; const tex_t *t = &r->t;
    r->stand_no = STAND_NO;   // looked at: not again while this registration stands
    if (!t->mem || t->d.samples > 1 || t->d.width > 16384 || t->d.height > 16384 || fmt_depth(t->d.format) || rb_packed_shift(t->d.format)
        || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) return 0;
    int sp = t->pool && nvn_sparse_pool(t->pool);   // a sparse pool's: read through its page table (stand_fill)
    if ((t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) && !sp) return 0;
    unsigned s0, ns; MTLTextureType tt; stand_shape sh;
    if (!stand_layers(r, &s0, &ns, &tt) || !stand_shape_of(t, &sh)) return 0;
    if (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) { uint32_t tw, th, td; int nt = nvn_tex_sparse_tiles(&t->d, &tw, &th, &td); if (nt < 0 || sh.l0 < (unsigned)nt) return 0; }   // a sparse texture's levels from its mip tail on (laid out as any texture's, sparse_make), not its tiled ones
    // A sparse pool's: every page behind the layers' levels mapped, checked before a texture is cut from the heap. A page not mapped yet (the game maps a streamed texture's small
    // levels after registering it: PRAGMATA's tablet materials, an ID registered 1,542 frames before the cut, held the dummy at the cut while its twins held their stand-ins) waits for
    // a mapping over its pages, the only thing that can map it: treg_sparse_notes lets it be looked at again, so walks and table patches do not look at it in between
    if (sp) for (unsigned y = 0; y < ns; y++) { uint64_t lo, hi;
        if (!stand_span(t, s0 + y, sh.l0, &lo, &hi)) return 0;
        if (!nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + lo, hi - lo)) { g_stand_sparse_unmapped++; r->stand_no = STAND_WAIT; return 0; } }
    uint32_t pf = 0, view_pf = 0; uint8_t sw[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, NULL) || !pf) return 0;
    int need_sw = treg_swizzle(r, (MTLPixelFormat)pf, 0, sw, &view_pf);
    if (view_pf && view_pf != pf) { if (tex_need_pfv((MTLPixelFormat)pf, (MTLPixelFormat)view_pf)) return 0; pf = view_pf; }   // the sRGB or linear twin holds the same bytes; another format is a view the stand-in does not make
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = tt; d.pixelFormat = (MTLPixelFormat)pf; d.width = sh.w; d.height = sh.h; d.mipmapLevelCount = sh.n; if (tt == MTLTextureType2DArray) d.arrayLength = ns;
    d.storageMode = MTLStorageModeShared; d.usage = MTLTextureUsageShaderRead;
    if (need_sw) d.swizzle = MTLTextureSwizzleChannelsMake(sw[0], sw[1], sw[2], sw[3]);
    id<MTLTexture> mt = stand_alloc(d);
    uint64_t um0 = g_stand_sparse_unmapped;
    if (!mt || !stand_fill(t, &sh, s0, ns, mt, pool_first_log() ? &r->stand_sum : NULL)) {   // the texture cut from the heap goes with mt
        if (g_stand_sparse_unmapped != um0) r->stand_no = STAND_WAIT;   // a page unmapped since the check above: that unmapping is a mapping note, and so is the one that maps it again
        return 0;
    }
    r->stand = (void *)CFBridgingRetain(mt); r->stand_rid = mt.gpuResourceID._impl; r->stand_tt1 = (uint8_t)tt + 1;
    g_stand_made++; g_stand_live++; g_stand_picked += sh.step != 0; g_stand_sparse += sp;
    return 1;
}
// What a table of type dtt holds for an ID the walk leaves out: its stand-in's resource ID, or 0 (the dummy). Caller holds g_rmu
static uint64_t pool_stand(int id_, int dtt) {
    treg_t *r = &g_treg[id_];
    if (!r->stand && !r->stand_no && g_walk_over && stand_on()) {
        uint64_t f = atomic_load(&g_cur_frame); if (f != g_stand_frame) { g_stand_frame = f; g_stand_frame_n = 0; }
        if (g_stand_frame_n < STAND_FRAME) g_stand_frame_n += (unsigned)stand_make(id_);
        else if (id_ < (int)(POOL_SEEN_WORDS * 32) && !(g_stand_todo[id_ >> 5] >> (id_ & 31) & 1)) { g_stand_todo[id_ >> 5] |= 1u << (id_ & 31); g_stand_todo_n++; }
    }
    return r->stand && r->stand_tt1 - 1 == dtt ? r->stand_rid : 0;
}
// The stand-ins that waited (pool_stand), into the tables in place where the entry is still the dummy. Once a submission; caller holds g_rmu
static void pool_stand_scan(void) {
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_stand_frame) { g_stand_frame = f; g_stand_frame_n = 0; }
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
            for (int k = 0; k < POOL_SNAPS; k++) { pool_snap *q = &g_psnap[k]; int at = id_ - q->base;
                if (!q->buf || q->samp || at < 0 || (uint32_t)at >= *(uint32_t *)q->buf.contents || q->dtt != r->stand_tt1 - 1) continue;
                if (q->dtt == MTLTextureTypeTextureBuffer) { q->reg = 0; continue; }
                uint64_t *e = (uint64_t *)((uint8_t *)q->buf.contents + DMSH_POOL_HEAD);
                if (e[at] == e[*(uint32_t *)q->buf.contents]) { e[at] = r->stand_rid; plog_add(q, (uint32_t)at, r->stand_rid); } }
        }
}
// A first read of an ID whose texture is not made: what it found. DM_NVN_POOL_FIRST=1 logs each (up to 32 a frame); the totals go to the log every 300 frames in which they changed
static void pool_first_read(int id_, uint64_t f) {
    if (id_ <= 0 || id_ >= g_ntreg || !g_treg[id_].ok || g_treg[id_].gone) return;
    treg_t *r = &g_treg[id_]; const tex_t *t = &r->t;
    if (r->view || !tex_storage_live(t) || pool_unwritten_target(t) || pkg_shared(t) || tex_get(t->serial)) return;
    int log_on = pool_first_log(), stale = 0;
    if (r->stand && log_on) { unsigned s0, ns; MTLTextureType tt; stand_shape sh; uint64_t sum = 0; stale = !stand_layers(r, &s0, &ns, &tt) || !stand_shape_of(t, &sh) || !stand_fill(t, &sh, s0, ns, nil, &sum) || sum != r->stand_sum; }
    if (r->stand) { g_first_stand++; g_first_stale += (unsigned)stale; } else { g_first_dummy++; g_first_dummy_big += pool_tex_size(t) >= (256u << 10); }
    static uint64_t lf = ~0ull; static int ln; if (f != lf) { lf = f; ln = 0; }
    if (log_on && ln++ < 32)
        MLOG("[pool] frame %llu: texture ID %d read before its texture was made, found %s: ser=%llu %ux%u format %#x target %d levels %u flags %#x, %llu KB, registered %llu frames before; view %d (format %d:%#x levels %d:%u+%u layers %d:%u+%u target %d:%d swizzle %d)",
             (unsigned long long)f, id_, r->stand ? (stale ? "its stand-in, written since" : "its stand-in") : r->stand_no == STAND_WAIT ? "the dummy (its stand-in waits for a mapping)" : r->stand_no ? "the dummy (no stand-in for it)" : "the dummy", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.target, t->d.levels, t->d.flags,
             (unsigned long long)(pool_tex_size(t) >> 10), (unsigned long long)(f - r->reg_frame), r->has_v, r->v.set_format, r->v.format, r->v.set_levels, r->v.base_level, r->v.nlevels, r->v.set_layers, r->v.min_layer, r->v.nlayers, r->v.set_target, r->v.target, r->v.set_swizzle);
}
static void pool_seen_scan(void) {
    if (!g_seen_buf || g_seen_sub == g_sub_serial) return;
    g_seen_sub = g_sub_serial;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_walk_frame) { g_walk_frame = f; g_walk_spent = 0; g_walk_req = 0; }
    if (g_stand_todo_n) pool_stand_scan();
    { static uint64_t at, was; uint64_t now = g_stand_made + g_first_stand + g_first_dummy;   // the totals, every 300 frames in which they changed
      if (f >= at + 300 && now != was) { at = f; was = now;
          MLOG("pool stand-ins up to frame %llu: %llu made in %.1f ms (%llu of them a level cut down, %llu in a sparse pool; %llu of a sparse pool not made: a page unmapped), %llu gave way to their texture, %llu held now in %d heaps of %u MB (%llu not made: the heaps were full), %u waiting; first reads of a texture not made found a stand-in %llu times%s and the dummy %llu times (%llu of them textures of 256 KB or more)",
               (unsigned long long)f, (unsigned long long)g_stand_made, g_stand_ns / 1e6, (unsigned long long)g_stand_picked, (unsigned long long)g_stand_sparse, (unsigned long long)g_stand_sparse_unmapped, (unsigned long long)g_stand_done, (unsigned long long)g_stand_live, g_nstand_heap, (unsigned)(STAND_HEAP >> 20), (unsigned long long)g_stand_full, g_stand_todo_n, (unsigned long long)g_first_stand,
               g_first_stale ? [NSString stringWithFormat:@" (%llu written since the stand-in was made)", (unsigned long long)g_first_stale].UTF8String : "", (unsigned long long)g_first_dummy, (unsigned long long)g_first_dummy_big); } }
    const uint32_t *w = (const uint32_t *)g_seen_buf.contents; unsigned words = MIN((unsigned)g_ntreg / 32 + 1, (unsigned)POOL_SEEN_WORDS);
    for (unsigned i = 0; i < words; i++) {
        uint32_t got = 0, typed_new = 0;   // every section's marks, and the marks new to their section
        for (unsigned s_ = 0; s_ < POOL_SEEN_TYPES; s_++) {
            uint32_t g = w[s_ * POOL_SEEN_WORDS + i];
            if (!g) continue;
            typed_new |= g & ~g_seen_typed[s_][i]; g_seen_typed[s_][i] |= g; got |= g;
        }
        for (uint32_t bits = got; bits; bits &= bits - 1) { int id_ = (int)i * 32 + __builtin_ctz(bits); if (id_ < g_ntreg) g_treg[id_].used = g_tu_now; }
        g_seen_todo[i] |= typed_new & g_seen_done[i];   // already read through another type: the tables of the new type take it in place
        uint32_t fresh = got & ~g_seen_done[i];
        if (fresh) { g_seen_done[i] |= fresh; g_seen_todo[i] |= fresh; for (uint32_t bits = fresh; bits; bits &= bits - 1) { pool_ds_note((int)i * 32 + __builtin_ctz(bits)); pool_first_read((int)i * 32 + __builtin_ctz(bits), f); } }
        for (uint32_t bits = g_seen_todo[i]; bits; bits &= bits - 1) {
            int b = __builtin_ctz(bits), id_ = (int)i * 32 + b;
            if (id_ <= 0 || id_ >= g_ntreg || !g_treg[id_].ok || g_treg[id_].gone || (!g_treg[id_].view && !tex_storage_live(&g_treg[id_].t))) { g_seen_todo[i] &= ~(1u << b); continue; } // NOLINT(clang-analyzer-core.BitwiseShift): bits is nonzero, so ctz returns 0..31
            if (!g_treg[id_].view && pool_unwritten_target(&g_treg[id_].t)) { g_seen_todo[i] &= ~(1u << b); continue; }
            if (!g_treg[id_].view && !pkg_shared(&g_treg[id_].t) && !tex_get(g_treg[id_].t.serial)) { if (g_walk_req >= POOL_REQ_BYTES) return; g_walk_req += pool_tex_size(&g_treg[id_].t); }
            g_seen_todo[i] &= ~(1u << b);
            void *tv = NULL; uint64_t tr = tex_fast_rid(id_, &tv); if (!tv || !tr) continue;
            MTLTextureType tt = ((__bridge id<MTLTexture>)tv).textureType; int trans = g_treg[id_].retired && g_treg[id_].transient_sub == g_sub_serial;
            for (int k = 0; k < POOL_SNAPS; k++) { pool_snap *q = &g_psnap[k]; int at = id_ - q->base;
                if (!q->buf || q->samp || at < 0 || (uint32_t)at >= *(uint32_t *)q->buf.contents) continue;
                if (q->dtt == MTLTextureTypeTextureBuffer) { q->reg = 0; continue; }
                uint64_t er = tr;
                if ((MTLTextureType)q->dtt != tt) {   // a table of another type: a view when a shader read the ID through its type (pool_fit)
                    if (!pool_seen_t(id_, q->dtt)) continue;
                    er = pool_bridge_pair((int)tt, q->dtt) ? pool_fit(id_, (__bridge id<MTLTexture>)tv, q->dtt) : 0;
                    if (!er) { pool_type_note(id_, (int)tt, q->dtt); continue; } }
                ((uint64_t *)((uint8_t *)q->buf.contents + DMSH_POOL_HEAD))[at] = er; plog_add(q, (uint32_t)at, er);
                if (trans) { ids_push(&q->trans, id_); if (!q->sub) q->sub = g_sub_serial; } }   // a view of a retired registration goes at this submission's end, so the next one takes it again (pool_gpu)
        }
    }
}
// The tables' spare buffers. A table a new walk replaced, or the buffer a patch left, is used again once the submission that may read it has completed, the last segment's fence covering every
// segment before it: a new buffer each time meant an allocation, a residency add and remove, and a kernel call or more each way, about 3 a frame in Ocarina of Time. Sizes round up to 64 KB, as a
// pool grows by a few IDs at a time (the idea is from PR 1428).
// A patched table goes into another buffer, as draws already encoded read the old one: copying the whole table was about 5 MB a frame in Ocarina of Time (16-24 patches a frame of its
// 33,025-entry tables in Kokiri Forest, 0.13-0.18 ms of the submit thread on an M6 against 0.015-0.025 with spares; PR #2663). So the buffer a patch leaves names its table, that table's build and the log position it holds (g_plog): a later patch of the table takes its finished
// spare furthest along and writes the entries logged since, and copies the table whole only when it has none, or the log has moved past them. A table built whole takes a spare of no live
// table first. The spares hold at most POOL_SPARE_BYTES together; past that the oldest go. 16 MB holds about 50 of that game's 320 KB tables, enough for its patches
// over the frames in flight: 8 MB took a spare for 85-90% of them, 4 MB for about half, slower than copying, as its copies found no finished buffer and allocated one
enum { POOL_SPARE_MAX = 64, POOL_SPARE_BYTES = 16 << 20 };
static size_t g_pool_spare_cap = POOL_SPARE_BYTES;   // a variable for t_pool_replay
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence, build, lpos; int k; } pool_spare;   // k: the table (g_psnap) of build whose state at log position lpos it holds; -1 none
static pool_spare g_pool_spare[POOL_SPARE_MAX]; static int g_npool_spare; static uint64_t g_pool_spare_bytes, g_pool_made, g_pool_reused;
// Patches: into a spare of their own table, the entries that took, copied whole because the table had no spare of its own, none finished or the log had moved past them; the time they took
static uint64_t g_stat_pool_replayed, g_stat_pool_replay_n, g_stat_pool_copied[3], g_stat_pool_take_ns;
static int spare_live(const pool_spare *s) { return s->k >= 0 && g_plog[s->k].build == s->build; }
static id<MTLBuffer> spare_out(int i) { id<MTLBuffer> b = g_pool_spare[i].buf; g_pool_spare_bytes -= b.length; g_pool_spare[i] = g_pool_spare[--g_npool_spare]; g_pool_spare[g_npool_spare].buf = nil; return b; }
static id<MTLBuffer> pool_table_take(size_t need) {
    size_t len = (need + 0xffff) & ~(size_t)0xffff; uint64_t done = g_done; int o = -1;
    for (int i = 0; i < g_npool_spare; i++) { const pool_spare *s = &g_pool_spare[i];
        if (s->fence > done || s->buf.length != len) continue;
        if (o < 0 || (spare_live(&g_pool_spare[o]) && !spare_live(s)) || (spare_live(&g_pool_spare[o]) == spare_live(s) && s->fence < g_pool_spare[o].fence)) o = i; }
    if (o >= 0) { g_pool_reused++; return spare_out(o); }
    id<MTLBuffer> b = [g_dev newBufferWithLength:len options:MTLResourceStorageModeShared];
    if (b) { res_add(b); g_pool_made++; }
    return b;
}
static void pool_table_give(id<MTLBuffer> b, int k, uint64_t lpos) {   // k: the table (g_psnap) whose current build b holds at log position lpos; -1 none
    if (!b) return;
    while (g_npool_spare && (g_npool_spare == POOL_SPARE_MAX || g_pool_spare_bytes + b.length > g_pool_spare_cap)) {   // the oldest go: a size the pools have left behind, or a table far behind its log
        int o = 0; for (int i = 1; i < g_npool_spare; i++) if (g_pool_spare[i].fence < g_pool_spare[o].fence) o = i;
        defer_obj_release(spare_out(o)); }
    if (b.length > g_pool_spare_cap) { defer_obj_release(b); return; }
    g_pool_spare[g_npool_spare] = (pool_spare){b, g_fence + 1, k >= 0 ? g_plog[k].build : 0, lpos, k}; g_pool_spare_bytes += b.length; g_npool_spare++;
}
// A buffer for the next version of q's table, holding what q->buf holds now (the patch writes its entries after): its finished spare furthest along, brought up to date from the log, or a copy
static id<MTLBuffer> pool_patch_take(const pool_snap *q) {
    uint64_t t0 = mach_absolute_time(), done = g_done, ln; int k = (int)(q - g_psnap), o = -1, own = 0, fin = 0; ln = g_plog[k].n;
    for (int i = 0; i < g_npool_spare; i++) { const pool_spare *s = &g_pool_spare[i];
        if (s->k != k || s->build != g_plog[k].build || s->buf.length != q->buf.length) continue;
        own = 1; if (s->fence > done) continue;
        fin = 1; if (ln - s->lpos > g_plog[k].cap) continue;
        if (o < 0 || s->lpos > g_pool_spare[o].lpos) o = i; }
    id<MTLBuffer> b;
    if (o < 0) { g_stat_pool_copied[!own ? 0 : !fin ? 1 : 2]++; b = pool_table_take(q->buf.length); if (b) memcpy(b.contents, q->buf.contents, q->buf.length); }
    else {
        uint64_t from = g_pool_spare[o].lpos; b = spare_out(o); g_pool_reused++;
        uint64_t *e = (uint64_t *)((uint8_t *)b.contents + DMSH_POOL_HEAD); const uint32_t *at = g_plog[k].at; const uint64_t *v = g_plog[k].v; uint32_t cap = g_plog[k].cap;
        memcpy(b.contents, q->buf.contents, DMSH_POOL_HEAD);   // the head: the bound and the read bitmap's address
        for (uint64_t j = from; j < ln; j++) e[at[j % cap]] = v[j % cap];
        g_stat_pool_replayed++; g_stat_pool_replay_n += ln - from;
    }
    g_stat_pool_take_ns += mach_absolute_time() - t0;   // ticks: mtl_pool_stats converts
    return b;
}
// The power line's pool table counts (hle_nvn.c): builds, their time (us), patched, kept, of those over pool changes and over a new submission; patches into a spare of their own and the entries
// that took, patches copied whole (no spare of their own, none finished, the log past them), the patches' copy and replay time (us); the bytes of the tables' buffers in use and spare
void mtl_pool_stats(uint64_t s[POOL_STATS]) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t used = 0; for (int k = 0; k < POOL_SNAPS; k++) if (g_psnap[k].buf) used += g_psnap[k].buf.length;
    uint64_t v[POOL_STATS] = {g_stat_pool_builds, (uint64_t)(g_stat_pool_ms * 1000), g_stat_pool_patched, g_stat_pool_kept, g_stat_pool_pchg, g_stat_pool_schg, g_stat_pool_replayed, g_stat_pool_replay_n,
        g_stat_pool_copied[0], g_stat_pool_copied[1], g_stat_pool_copied[2], g_stat_pool_take_ns * tb.numer / tb.denom / 1000, used, g_pool_spare_bytes};
    memcpy(s, v, sizeof v);
}
// One entry of a pool table, as the walk takes it: the resource ID of what the ID holds (0: nothing of the type, or a texture not made). gone counts IDs without storage, and *dead is set for
// one whose storage does not exist (yet): one marked gone stays so until it is registered again, which is noted. left / left_req count the textures past limit (what this frame may still
// make) and those of them a shader read. *transient is set for a view of a retired registration, which goes at the end of this submission. Caller holds g_rmu
static uint64_t pool_entry(int samp, int dtt, int id_, uint64_t limit, int *gone, int *left, int *left_req, int *transient, int *dead, uint32_t *lead) {
    if (id_ <= 0) return 0;
    if (samp) return id_ < g_nsreg && g_sreg_ok[id_] && samp_fast(id_) ? g_srid[id_] : 0;
    if (id_ >= g_ntreg || !g_treg[id_].ok) return 0;
    const treg_t *r = &g_treg[id_];
    if (r->gone) { ++*gone; return 0; }
    if (!r->view && !tex_storage_live(&r->t)) { ++*gone; *dead = 1; return 0; }
    if (!r->view && pool_unwritten_target(&r->t)) return 0;   // dummy until a pass writes it (its new texture is noted: own_note)
    if (r->evicted && !r->view && !pool_seen(id_) && !tex_get(r->t.serial) && !pkg_shared(&r->t)) return pool_stand(id_, dtt);   // freed idle or at memory pressure: made again when a shader reads it, not up front
    if (!r->view && r->t.d.target != NVN_TEXTURE_TARGET_BUFFER && pool_tex_size(&r->t) > limit && !pkg_shared(&r->t) && !tex_get(r->t.serial) && !(pool_seen(id_) && g_walk_req < POOL_REQ_BYTES)) { ++*left; *left_req += pool_seen(id_); return pool_stand(id_, dtt); }   // BUFFER is a live view, with no upload for this budget to defer
    if (r->tt1 && r->tt1 - 1 != dtt && !(pool_bridge_pair(r->tt1 - 1, dtt) && pool_seen_t(id_, dtt))) { pool_type_note(id_, r->tt1 - 1, dtt); return 0; }   // an ID of another texture type, seen before (the table holds only its own type, or a view of an ID read through it: pool_fit): nothing to look up or charge (the idea is from PR 1428)
    if (!r->view && r->t.d.target != NVN_TEXTURE_TARGET_BUFFER && !pkg_shared(&r->t)) { g_walk_spent += pool_tex_size(&r->t); if (pool_seen(id_)) g_walk_req += pool_tex_size(&r->t); }
    void *tv = NULL; uint64_t tr = tex_fast_rid(id_, &tv);
    if (!tv) return 0;
    MTLTextureType got = ((__bridge id<MTLTexture>)tv).textureType; g_treg[id_].tt1 = (uint8_t)got + 1;
    if (got != (MTLTextureType)dtt) {   // a view when a shader read the ID through this table's type (pool_fit); else the dummy
        uint64_t b = pool_bridge_pair((int)got, dtt) && pool_seen_t(id_, dtt) ? pool_fit(id_, (__bridge id<MTLTexture>)tv, dtt) : 0;
        if (!b) { pool_type_note(id_, (int)got, dtt); return 0; }
        tr = b;
    }
    if (lead && got == MTLTextureTypeTextureBuffer) *lead = tbuf_lead(id_, (__bridge id<MTLTexture>)tv);
    if (g_treg[id_].retired && g_treg[id_].transient_sub == g_sub_serial) *transient = 1;
    return tr;
}
// Fold shader reads from g_seen_buf onto g_treg[id_].used on the g_tu_now clock (caller holds g_rmu). With clear=1 (tu_pass / tu_tick, once a second), clears the marks already folded
// into g_seen_done / g_seen_typed so the next pass sees only new reads; with clear=0 (pkg_evict_idle), records any unread shader marks right before judging idleness without stealing them from tu_pass
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
// Memory pressure and small-device periodic sweep: the Metal copies of uploaded textures nothing has used for PKG_IDLE_DS (30 s, on the g_tu_now decisecond clock). On NVN such a texture
// costs nothing beyond its pool; here it is a Metal copy, and a game keeps the texture objects of an area it has left.
// A package (NvPkg) goes when no texture ID naming one of its serials was used for PKG_IDLE_DS (g_treg[i].used: binds, UBO handles, shader pool reads) and nothing resolved it outside a pool
// walk either (used_tu), when it is not sparse, placed or bound as a view, the GPU never wrote its storage (gws_owned) nor stored writes into it (stored), and no pin, sharing-table entry or
// serial that took its texture over holds it. Everything naming the copy lets go together: its serials' entries (live and ghost via own_drop_where_locked), its g_pkg_share entry, each texture
// ID's views, the view cache, and the pool-table entries (marked evicted so a pool walk leaves the ID its stand-in or dummy until a shader reads it; a bind makes it again at once). Submit thread
enum { PKG_IDLE_DS = 300, PKG_SWEEP_DS = 100 };   // deciseconds: 30 s idle threshold, 10 s periodic sweep on small devices
static _Atomic uint64_t g_evict_n, g_evict_bytes;
static uint64_t pkg_evict_idle(unsigned *n_out) { @autoreleasepool {
    unsigned n = 0; uint64_t bytes = 0; uint32_t now = g_tu_now;
    if (n_out) *n_out = 0;
    NSMutableArray<NvPkg *> *pkgs = [NSMutableArray array]; NSHashTable *seen = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    nvn_hmap last = {.klen = sizeof(uintptr_t)};   // package address → newest use + 1
    nvn_hmap byser_m = {.klen = 8}, *byser = &byser_m, *lastp = &last;   // serial → its package (live before ghost), not retained: tables hold them under g_smu
    os_unfair_lock_lock(&g_rmu); pthread_mutex_lock(&g_smu);
    seen_stamp_used(now, 0);
    own_each_locked(^(uint64_t serial, id o, int ghost) {
        if (![o isKindOfClass:[NvPkg class]]) return;
        if (!ghost || !nvn_hm_get(byser, &serial)) nvn_hm_put(byser, &serial, (__bridge void *)o);
        if ([seen containsObject:o]) return;
        [seen addObject:o]; [pkgs addObject:o]; uintptr_t k = (uintptr_t)(__bridge void *)o; nvn_hm_put(lastp, &k, (void *)(uintptr_t)(((NvPkg *)o)->used_tu + 1ull)); });
    for (int i = 1; i < g_ntreg; i++) { treg_t *r = &g_treg[i]; if (!r->ok || !r->t.serial) continue;
        id o = (__bridge id)nvn_hm_get(byser, &r->t.serial);
        if (!o) continue;
        uintptr_t k = (uintptr_t)(__bridge void *)o, u = (uintptr_t)nvn_hm_get(&last, &k);
        if (u && r->used + 1ull > u) nvn_hm_put(&last, &k, (void *)(uintptr_t)(r->used + 1ull)); }
    pthread_mutex_unlock(&g_smu); os_unfair_lock_unlock(&g_rmu);
    free(byser_m.keys); free(byser_m.vals);
    NSHashTable *victims = [NSHashTable hashTableWithOptions:NSPointerFunctionsOpaqueMemory | NSPointerFunctionsOpaquePersonality];
    memset(g_evict_why, 0, sizeof g_evict_why); g_evict_why[0] = (unsigned)pkgs.count;
    for (NvPkg *pk in pkgs) { uintptr_t k = (uintptr_t)(__bridge void *)pk, u = (uintptr_t)nvn_hm_get(&last, &k);
        uint32_t last_use = u ? (uint32_t)(u - 1) : 0;
        if (!u || now < last_use || now - last_use < PKG_IDLE_DS) { g_evict_why[1]++; continue; }
        if (pk->sparse || pk->ps || pk->view || !pk->tex || pk->tex.heap || !pk->mem || !pk->size) { g_evict_why[2]++; continue; }
        if (pk->stored || gws_owned(pk->mem, pk->mem + pk->size)) { g_evict_why[3]++; continue; }
        [victims addObject:pk]; }
    free(last.keys); free(last.vals);
    if (!victims.count) return 0;
    // Let go of them together. g_rmu before g_smu, as everywhere
    nvn_hmap gone_ser = {.klen = 8};
    os_unfair_lock_lock(&g_rmu); pthread_mutex_lock(&g_smu);
    { NSUInteger was = victims.count;   // a pin, the sharing table or a serial that took its texture over (cpu_unc_drop) holds it (nvn_owner.m)
      for (NvPkg *pk in victims.allObjects) if (tex_object_pinned(pk) || own_shared_locked(pk->tex) || entry_holds((__bridge void *)pk->tex)) [victims removeObject:pk];
      g_evict_why[4] = (unsigned)(was - victims.count); }
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
    for (int i = 1; i < g_ntreg; i++) { treg_t *r = &g_treg[i]; if (!r->ok || !r->t.serial || !nvn_hm_get(&gone_ser, &r->t.serial)) continue;
        treg_drop_views(r); r->evicted = 1; reg_bump(i, 0); pool_seen_clear(i); }
    // The view cache is keyed by the Metal texture (vc_addr), not by serial: each copy's entry goes here, or its views, which retain their parent, would keep the freed copy alive
    // and in the residency set (upstream took the serials as keys, which never named an entry)
    for (NvPkg *pk in victims) { uint64_t a = vc_addr(pk->tex); vc_set *v = nvn_hm_del(&g_vc, &a); if (v) vc_set_free(v); }
    os_unfair_lock_unlock(&g_rmu);
    free(gone_ser.keys); free(gone_ser.vals);
    atomic_fetch_add(&g_evict_n, n); atomic_fetch_add(&g_evict_bytes, bytes);
    if (n_out) *n_out = n;
    return bytes;
} }
// Small devices (Metal recommended working set under 6 GB: 8 GB Macs): sweep idle uploaded textures every PKG_SWEEP_DS without waiting for OS memory pressure so
// an area's copies leave once the game has left it (docs/reference/memory.md). Elsewhere memory pressure alone frees them. DM_NVN_TEX_EVICT_IDLE=0 / 1 forces it
static int small_working_set(uint64_t working_set) { return working_set && working_set < (6ull << 30); }
static int g_sweep_on = -1; static uint32_t g_sweep_last;
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
static uint64_t pool_gpu(int samp, int dtt, int dpf, int base, __unsafe_unretained id<MTLBuffer> *out) {   // what it resolves is not a use (g_pool_walk)
    g_pool_walk = 1; uint64_t r = pool_walk(samp, dtt, dpf, base, out); g_pool_walk = 0; return r;
}
static uint64_t pool_walk(int samp, int dtt, int dpf, int base, __unsafe_unretained id<MTLBuffer> *out) {
    pool_seen_scan();
    uint64_t rg = atomic_load(&g_reg_gen), sg = samp ? 0 : atomic_load(&g_ser_gen);   // a sampler table follows registrations alone
    pool_snap *p = NULL, *free_ = NULL;
    for (int i = 0; i < POOL_SNAPS && !p; i++) { pool_snap *q = &g_psnap[i];
        if (!q->buf) { if (!free_) free_ = q; }
        else if (q->samp == samp && q->dtt == dtt && q->dpf == dpf && q->base == base) p = q; }
    int stale = p && p->sub && p->sub != g_sub_serial;   // its transient entries name views that went at the end of submission p->sub
    int walk_on = p && p->left && p->frame != atomic_load(&g_cur_frame) && (p->left_req || !pool_budget_full() || (!p->over && stand_on()));   // left: the walk goes on next frame, unless the budget has no room and nothing read waits: another walk would build the same table (but one, when the budget has filled since the last: what that left out for a later walk gets its stand-in)
    if (p && p->reg == rg && p->ser == sg && !stale && !walk_on) { *out = p->buf; return p->gpu; }
    // Only registrations and noted storage changes since the table was built (each generation in its log), the IDs without storage after a pool change and the transient IDs in a
    // new submission: take those IDs again, by the walk's rules and within what this frame may still make. The table is kept when none of them changes an entry; otherwise the old
    // one is copied and patched, as draws already encoded read the old one. A sparse mapping comes as notes of the textures it changes (treg_sparse_notes). Ocarina of Time,
    // a seven-minute walk through Kokiri Forest: building the tables whole took about 53 s (61,000 builds)
    int nn = (samp ? g_nsreg : g_ntreg) - base; if (nn < 1) nn = 1;
    if (p && !walk_on && (samp || dtt != MTLTextureTypeTextureBuffer)) do {
        if (rg - p->reg >= REG_LOG || sg - p->ser >= SER_LOG) { g_stat_pool_nopatch[0]++; break; }
        if (*(const uint32_t *)p->buf.contents != (uint32_t)nn) { g_stat_pool_nopatch[2]++; break; }
        static int *ids; static size_t idcap; size_t nid = 0; int known = 1, pools = 0;
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
        if (known && pools) {   // a memory pool came or went: the IDs found without storage may have it now (the IDs of a pool Finalize removed were noted, treg_gone)
            if (p->dead.n < 0) { g_stat_pool_nopatch[1]++; break; }
            for (int k = 0; k < p->dead.n; k++) POOL_ID(p->dead.v[k]);
        }
        if (known && stale) {   // a new submission: the views of retired registrations the table holds went at the end of the last one
            if (p->trans.n < 0) { g_stat_pool_nopatch[4]++; break; }
            for (int k = 0; k < p->trans.n; k++) POOL_ID(p->trans.v[k]);
        }
        #undef POOL_ID
        if (!known) { g_stat_pool_nopatch[3]++; break; }
        if (nid >= POOL_IDS_MAX) { g_stat_pool_nopatch[0]++; break; }
        uint64_t *e = (uint64_t *)((uint8_t *)p->buf.contents + DMSH_POOL_HEAD), dummy = e[nn], room = samp ? UINT64_MAX : pool_room(), spent = g_walk_spent;
        int gone = 0, left = 0, left_req = 0; id<MTLBuffer> b = nil; uint64_t *be = NULL, lpos = 0;   // lpos: the log position the old buffer holds, where this patch's entries begin
        pool_ids nd = {0}, nt = {0}; ids_begin(0); ids_begin(1);
        if (!pools) ids_keep(0, &nd, &p->dead);    // not taken again: they stay on the list for the next pool change
        if (!stale) ids_keep(1, &nt, &p->trans);   // this submission's views, still good
        for (size_t k = 0; k < nid; k++) {
            int i = ids[k] - base; if (i < 0 || i >= nn) continue;
            uint64_t used = g_walk_spent - spent, limit = used < room ? room - used : 0;   // what this patch made comes off the room
            int d = 0, t = 0; uint64_t r = pool_entry(samp, dtt, ids[k], limit, &gone, &left, &left_req, &t, &d, NULL); r = r ? r : dummy;
            if (d) ids_add(0, &nd, ids[k]);
            if (t) ids_add(1, &nt, ids[k]);
            if (r == (be ? be[i] : e[i])) continue;
            if (!b) { b = pool_patch_take(p); be = (uint64_t *)((uint8_t *)b.contents + DMSH_POOL_HEAD); lpos = g_plog[p - g_psnap].n; }
            be[i] = r; plog_add(p, (uint32_t)i, r);
        }
        g_stat_pool_pchg += pools; g_stat_pool_schg += stale; free(p->dead.v); free(p->trans.v);
        p->reg = rg; p->ser = sg; p->sub = nt.n ? g_sub_serial : 0; p->dead = nd; p->trans = nt;
        if (left) { p->left += left; p->left_req += left_req; p->frame = atomic_load(&g_cur_frame); }   // textures past this frame's room: the walk goes on next frame, as for its own
        if (!b) { g_stat_pool_kept++; *out = p->buf; return p->gpu; }
        pool_table_give(p->buf, (int)(p - g_psnap), lpos); p->buf = b; p->gpu = b.gpuAddress;
        g_psnap_gen++; g_stat_pool_patched++;
        *out = b; return p->gpu;
    } while (0);
    // A change the patch cannot take starts another walk. Small pools pay little (Mario Party Superstars: 0.1 ms), but Ocarina of Time registers something between most of its draws in a pool of
    // 33,024 IDs: 8 walks a frame, a quarter of the replay, each with a new buffer and a residency commit. So past POOL_SUB_MS of walks a submission the table stands until the next submission.
    // A read of an ID registered since then finds the dummy for that long; a replaced texture stays alive until the submission that may read it has completed. A table with views of retired
    // registrations from an earlier submission does not stand: those views have gone
    if (g_walk_sub != g_sub_serial) { g_walk_sub = g_sub_serial; g_walk_ms = 0; }
    if (p && p->buf && g_walk_ms >= POOL_SUB_MS && !stale && (samp || dtt != MTLTextureTypeTextureBuffer)) { *out = p->buf; return p->gpu; }
    g_stat_pool_why[!p ? 0 : p->ser != sg ? 2 : p->reg != rg ? 1 : stale ? 3 : 4]++;
    if (!p) p = free_ ? free_ : &g_psnap[g_psnap_next++ % POOL_SNAPS];
    uint64_t t0 = mach_absolute_time();
    int n = (samp ? g_nsreg : g_ntreg) - base; if (n < 1) n = 1;
    size_t metadata = !samp && dtt == MTLTextureTypeTextureBuffer ? 4 * (size_t)(n + 1) : 0;
    id<MTLBuffer> b = pool_table_take(DMSH_POOL_HEAD + 8 * (size_t)(n + 1) + metadata);
    uint8_t *c = b.contents; *(uint32_t *)c = (uint32_t)n; if (!samp) *(uint64_t *)(c + 8) = pool_seen_gpu_t(dtt);
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
    pool_table_give(p->buf, -1, 0); free(p->dead.v); free(p->trans.v); plog_build(p, n);   // the spares of the table's earlier build, or of another table in this slot, are of no live table now
    // Retired registrations drop their views at submission end; their resource IDs cannot outlive that submission (trans, taken again by the next one)
    *p = (pool_snap){b, b.gpuAddress, rg, sg, atomic_load(&g_cur_frame), nt.n ? g_sub_serial : 0, samp, dtt, dpf, base, left, left_req, nd, nt, samp || g_walk_over};
    g_psnap_gen++;
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    double ms = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6; g_stat_pool_ms += ms;
    if (!samp) g_walk_ms += ms;
    if (!samp && g_stat_pool_builds % 500 == 0) {   // how the walk stands: textures registered, read by a shader, left out; and what those read come to
        unsigned nseen = 0; uint64_t bseen = 0; for (int i = 0; i < n; i++) { int id_ = base + i; if (id_ > 0 && g_treg[id_].ok && pool_seen(id_)) { nseen++; bseen += pool_tex_size(&g_treg[id_].t); } }
        // What the pools wire, and how much of that is texture storage, which the GPU reads only through the Metal textures: the duplicate
        uint64_t pres = 0, pall = 0, tres = 0, tall = 0; typedef struct { uint64_t lo, hi; } rng; rng *rs = malloc(sizeof *rs * (size_t)n); int nr = 0;
        for (int i = 0; i < n && rs; i++) { int id_ = base + i; if (id_ > 0 && g_treg[id_].ok && !g_treg[id_].gone && g_treg[id_].t.mem) { rs[nr].lo = (uint64_t)(uintptr_t)g_treg[id_].t.mem; rs[nr].hi = rs[nr].lo + pool_tex_size(&g_treg[id_].t); nr++; } }
        if (rs) { qsort(rs, (size_t)nr, sizeof *rs, pool_size_cmp); pthread_mutex_lock(&g_mu);
            for (int k = 0; k < g_npool; k++) { uint64_t sz = g_pools[k].hi - g_pools[k].lo; pall += sz; if (g_pools[k].res) pres += sz; }
            uint64_t end = 0; for (int i = 0; i < nr; i++) { uint64_t lo = MAX(rs[i].lo, end), hi = rs[i].hi; if (hi <= lo) continue; end = hi; tall += hi - lo; int q = pool_peek(lo); if (q >= 0 && g_pools[q].res) tres += hi - lo; }
            pthread_mutex_unlock(&g_mu); free(rs); }
        MLOG("pool walk: tables made %llu, reused %llu; passes ended after a pool shader that may read depth %llu", (unsigned long long)g_pool_made, (unsigned long long)g_pool_reused, (unsigned long long)g_fb_split_pool);
        MLOG("pool walk: %d IDs, %d left out, %u read by a shader (%llu MB), allocated %llu MB; memory pools %llu MB, wired %llu MB; texture storage %llu MB, in wired pools %llu MB", n, left, nseen, (unsigned long long)(bseen >> 20), (unsigned long long)(g_dev.currentAllocatedSize >> 20), (unsigned long long)(pall >> 20), (unsigned long long)(pres >> 20), (unsigned long long)(tall >> 20), (unsigned long long)(tres >> 20)); }
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
// Once a second, when a submission begins: the shared render targets another one drew over and that have not been used since go (nvn_owner.m, own_alias_dead_locked). Unused for
// TU_IDLE, a target the game still reads every frame stays. Their texture IDs drop their views, so a pool walk gives them the dummy until a pass writes the texture again
// (pool_unwritten_target), as for a target never written, rather than making it again; a binding or a write makes it again (tex_mtl_), filled as a new texture is. TU_MAX a pass at
// most: each serial that goes is a note, and past SER_NOTES of them every texture ID takes the slow path once
enum { TU_IDLE = 30, TU_MAX = 256 };   // deciseconds; textures a pass
static uint32_t g_tu_last;   // g_tu_now at the last pass
static uint64_t g_tu_n, g_tu_bytes, g_tu_serials, g_tu_pinned, g_tu_ns, g_tu_passes;
static int tu_use_of(id o, own_use *u) {   // own_use_fn; the pass's mark keeps the record (the sweep below)
    uintptr_t k = (uintptr_t)(__bridge void *)o;
    os_unfair_lock_lock(&g_tu_lk); tu_rec *r = nvn_hm_get(&g_tu, &k); if (r) { r->pass = g_tu_pass_no; u->use = r->use; u->wr = r->wr; } os_unfair_lock_unlock(&g_tu_lk);
    return r != NULL;
}
static void tu_pass(void) { @autoreleasepool {
    uint64_t t0 = bp_now(); uint32_t now = g_tu_now, since = g_tu_last; g_tu_last = now;
    os_unfair_lock_lock(&g_tu_lk); g_tu_pass_no++; os_unfair_lock_unlock(&g_tu_lk);
    // The reads by texture ID since the last pass: those the shaders marked in a pool (the marks pool_seen_scan has folded into g_seen_done go from the bitmap, so the next pass
    // sees only new ones; the others it folds first) and the bindings (tex_fast_rid), onto their textures' records
    os_unfair_lock_lock(&g_rmu);
    seen_stamp_used(now, 1);
    if (g_tu.n) {
        os_unfair_lock_lock(&g_tu_lk);
        for (int i = 1; i < g_ntreg; i++) { const treg_t *r = &g_treg[i];
            if (!r->ok || !r->view || r->used < since) continue;
            tu_rec *rec = tu_find((__bridge id)r->view); if (rec && rec->use < r->used) rec->use = r->used; }
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
    // The records of textures that left the sharing table (no mark from this pass) go; the released serials are kept to count those made again
    os_unfair_lock_lock(&g_tu_lk);
    if (g_tu_gone.n > 65536) nvn_hm_clear(&g_tu_gone, NULL);
    for (uint32_t i = 0; i < nser; i++) nvn_hm_put(&g_tu_gone, &sers[i], (void *)1);
    uintptr_t *ks = NULL; uint32_t nk = 0;
    for (uint32_t i = 0; i < g_tu.cap; i++) { tu_rec *r = g_tu.vals[i]; if (!r || r->pass == g_tu_pass_no) continue;
        if (!(nk & (nk + 1))) { uintptr_t *n = realloc(ks, (nk * 2 + 2) * sizeof *ks); if (!n) break; ks = n; }
        memcpy(&ks[nk++], g_tu.keys + (size_t)i * g_tu.klen, sizeof *ks); }
    for (uint32_t i = 0; i < nk; i++) free(nvn_hm_del(&g_tu, &ks[i]));   // collected first: a removal shifts entries
    uint32_t live = g_tu.n; uint64_t remade = g_tu_remade;
    os_unfair_lock_unlock(&g_tu_lk);
    free(ks); free(sers);
    g_tu_n += st.n; g_tu_bytes += st.bytes; g_tu_serials += st.serials; g_tu_pinned += st.pinned; g_tu_ns += bp_now() - t0; g_tu_passes++;
    static uint64_t at, was; uint64_t f = atomic_load(&g_cur_frame);
    if (f >= at + 300 && g_tu_n != was) { at = f; was = g_tu_n;
        MLOG("render targets drawn over and unused for %u s, released up to frame %llu: %llu textures, %llu MB (%llu serial entries with them; %llu kept by a pin), %llu made again; %u covered now; %llu passes, %.2f ms in all",
             TU_IDLE / 10, (unsigned long long)f, (unsigned long long)g_tu_n, (unsigned long long)(g_tu_bytes >> 20), (unsigned long long)g_tu_serials, (unsigned long long)g_tu_pinned, (unsigned long long)remade, live,
             (unsigned long long)g_tu_passes, g_tu_ns / 1e6); }
} }
static int g_tu_manual;   // the tests keep the clock themselves and call tu_pass
static int tu_mode(void) { static int m = -1; if (m < 0) m = sw_mode("DM_NVN_RT_DEAD", 1); return m; }
static void tu_tick(void) {   // when a submission begins; DM_NVN_RT_DEAD=0 keeps them all (the stamps go on), ab alternates it with the DM_AB phases
    if (g_tu_manual) return;
    static uint64_t t0; uint64_t ns = clock_gettime_nsec_np(CLOCK_UPTIME_RAW); if (!t0) t0 = ns;
    g_tu_now = (uint32_t)((ns - t0) / 100000000ull) + 1;
    if (g_tu_now - g_tu_last >= 10) {
        if (sw_on(tu_mode())) tu_pass();
        else { g_tu_last = g_tu_now; os_unfair_lock_lock(&g_rmu); seen_stamp_used(g_tu_now, 1); os_unfair_lock_unlock(&g_rmu); }
    }
}
static id<MTLBuffer> dummy_buf(void) { static id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t dummy_buf_gpu(void) { static uint64_t gpu; if (!gpu) gpu = dummy_buf().gpuAddress; return gpu; }
