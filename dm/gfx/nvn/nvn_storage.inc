// Texture storage: initial content, linear and CPU-written textures, the storage the GPU writes, compressible aliases, buffer textures, and tex_mtl, which gives a texture its host texture.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// Zero newly created non-packaged textures: a new Metal texture's content is undefined, while a freshly mapped memory pool on the Switch reads as 0. Games depend on it: SMBW's fountain water surface uses a 512×1 RGBA16F wave texture
// that every frame is "read last frame → render into a render target → copy back" and is never cleared; the undefined initial value keeps swinging through the feedback and pushes the water vertices to the top of the screen (the tall panels above the fountain).
// Copied level by level / slice by slice from one shared all-zero buffer, executed with the upload batch (ordered before this replay). Depth textures are zeroed too: Pokemon Brilliant
// Diamond's movie quad tests GEQUAL against a newly allocated window depth buffer without clearing it, and Metal's undefined initial depth hid every movie even after VP8 decoding.
// A single-sample Depth16Unorm / Depth32Float texture is copied into like a colour one, in pieces of rows that fit the zero buffer, not cleared in a render pass: a clear-only pass on a large
// depth attachment fails with kIOGPUCommandBufferCallbackErrorOutOfMemory whatever memory is free, and every later command buffer of the queue fails the same way (M3 Pro, standalone Metal 4
// program: Depth32Float 4096x4096 and Depth16Unorm from about 5000x5000 up; copies into the same textures up to 16384x16384 never failed). Ocarina of Time makes a 12288x12288 Depth16Unorm
// (296 MB) and a 4096x4096 Depth32Float at each save load, and half its loads there ended at the 32-failures stop (res_top_once; PR #2306). Stencil formats keep the clear load actions
// (a plain buffer copy cannot write a combined depth-stencil texture), as do multisample ones.
// Multisample colour textures use a clear load action too, per layer (a buffer copy cannot write samples): Rune Factory 5 loads its new 1920×1080 two-sample RG11B10F scene target without
// clearing it and blends into it, and Metal's undefined content gave a green background that kept the silhouettes of moving characters. Block-compressed formats are not filled by this path.
static size_t mtl_pf_bytes(MTLPixelFormat pf) {
    switch ((int)pf) {
        case 10: case 11: case 12: case 13: case 14: return 1;                                   // R8
        case 20: case 22: case 23: case 24: case 25: case 30: case 31: case 32: case 33: case 34: return 2;   // R16 / RG8
        case 40: case 41: case 42: case 43: return 2;                                            // packed 16-bit colour
        case 53: case 54: case 55: case 60: case 62: case 63: case 64: case 65:
        case 70: case 71: case 72: case 73: case 74: case 80: case 81:
        case 90: case 91: case 92: case 93: case 94: return 4;                                   // R32 / RG16 / RGBA8 / BGRA8 / 10-10-10-2 / 11-11-10 / 9-9-9-5
        case 103: case 104: case 105: case 110: case 112: case 113: case 114: case 115: return 8;   // RG32 / RGBA16
        case 123: case 124: case 125: return 16;                                                 // RGBA32
    }
    return 0;
}
// Shared all-zero buffer (for zeroing new textures and scissored zero clears): replaced by a bigger one when too small; the old one is released after in-flight submissions finish
static id<MTLBuffer> zero_buf(size_t need) {
    static id<MTLBuffer> zb; static size_t zn;
    if (need > zn) {
        size_t n = MAX(need, (size_t)4 << 20);
        id<MTLBuffer> b = [g_dev newBufferWithLength:n options:MTLResourceStorageModeShared];
        if (!b) return nil;
        memset(b.contents, 0, n); res_add(b);
        if (zb) defer_obj_release(zb);
        zb = b; zn = n;
    }
    return zb;
}
static uint64_t g_tex_zeroed;
static void tex_zero_fill(id<MTLTexture> mt) {
    static int warned;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread()) return;   // the upload batch belongs to the submit thread only
    MTLTextureType tt = mt.textureType;
    MTLPixelFormat pf = mt.pixelFormat;
    int depth = pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8;
    int stencil = pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth32Float_Stencil8;
    int multisample = tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray;
    if (multisample && !depth && !stencil && !(mt.usage & MTLTextureUsageRenderTarget)) return;   // no render pass can clear it (tex_desc gives every multisample texture the usage)
    size_t dbpp = multisample ? 0 : pf == MTLPixelFormatDepth16Unorm ? 2 : pf == MTLPixelFormatDepth32Float ? 4 : 0;
    if (dbpp) {   // by copies, in pieces of rows: a row is at most 64 KB (16384 x 4), the zero buffer at least 4 MB, so it is never grown for a depth texture
        id<MTLBuffer> zb = zero_buf((size_t)mt.width * dbpp); if (!zb) return;
        NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
        up_begin();
        for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
            NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), step = MAX((NSUInteger)1, MIN(h, zb.length / (w * dbpp)));
            for (NSUInteger y = 0; y < slices; y++) for (NSUInteger y0 = 0; y0 < h; y0 += step) {
                NSUInteger n = MIN(step, h - y0);
                up_blit_add_at(zb, 0, w * dbpp, w * dbpp * n, MTLSizeMake(w, n, 1), mt, y, l, MTLOriginMake(0, y0, 0));
            }
        }
        [g_up_texs addObject:mt];
        if (!g_cb) up_flush();
        if (!g_tex_zeroed++) MLOG("new-texture zeroing: on (new Metal textures are undefined; new Switch memory reads 0)");
        return;
    }
    if (depth || stencil || multisample) {
        up_begin();
        if (g_up_ce) { m4_end(g_up_ce); g_up_ce = nil; }
        NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
        for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) for (NSUInteger y = 0; y < slices; y++) {
            MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
            if (!depth && !stencil) { pd.colorAttachments[0].texture = mt; pd.colorAttachments[0].level = l; pd.colorAttachments[0].slice = y;
                pd.colorAttachments[0].loadAction = MTLLoadActionClear; pd.colorAttachments[0].storeAction = MTLStoreActionStore; pd.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0); }
            if (depth) { pd.depthAttachment.texture = mt; pd.depthAttachment.level = l; pd.depthAttachment.slice = y;
                pd.depthAttachment.loadAction = MTLLoadActionClear; pd.depthAttachment.storeAction = MTLStoreActionStore; pd.depthAttachment.clearDepth = 0; }
            if (stencil) { pd.stencilAttachment.texture = mt; pd.stencilAttachment.level = l; pd.stencilAttachment.slice = y;
                pd.stencilAttachment.loadAction = MTLLoadActionClear; pd.stencilAttachment.storeAction = MTLStoreActionStore; pd.stencilAttachment.clearStencil = 0; }
            id<MTL4RenderCommandEncoder> e = [g_up_cb renderCommandEncoderWithDescriptor:pd];
            [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()]; m4_end(e);
        }
        g_up_ce = [g_up_cb computeCommandEncoder];
        [g_up_ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
        [g_up_ce setArgumentTable:g_up_ct]; [g_up_texs addObject:mt];
        if (!g_cb) up_flush();
        if (multisample && !depth && !stencil) { static int ms; if (!ms++) MLOG("new-texture zeroing: multisample colour textures are cleared in a render pass (first %lux%lu format %lu, %lu samples)", (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)pf, (unsigned long)mt.sampleCount); }
        return;
    }
    if (tt == MTLTextureTypeTextureBuffer) return;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat);
    if (!bpp) { if (!warned++) MLOG("new-texture zeroing: pixel format %lu not cleared (compressed / unsupported)", (unsigned long)mt.pixelFormat); return; }
    NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
    size_t need = (size_t)mt.width * bpp * mt.height * mt.depth;   // level 0, one slice is the largest
    if (need > (64u << 20)) { static int big; if (!big++) MLOG("new-texture zeroing: %lux%lux%lu exceeds 64 MB, not cleared", (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)mt.depth); return; }
    // In pieces of rows, as depth above: a row is at most 256 KB (16384 x 16), so the zero buffer stays at its 4 MB. Copying a whole image grew it to the largest texture zeroed, and
    // it stayed resident at that size for good: Ocarina of Time's was 64 MB
    id<MTLBuffer> zb = zero_buf((size_t)mt.width * bpp); if (!zb) return;
    up_begin();
    for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
        NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), z = tt == MTLTextureType3D ? MAX(mt.depth >> l, 1u) : 1;
        NSUInteger row = w * bpp, rows = MIN(h, zb.length / row);
        NSUInteger depth = MAX((NSUInteger)1, MIN(z, zb.length / (row * h)));   // copy whole depth slices together when they fit
        for (NSUInteger slice = 0; slice < slices; slice++) for (NSUInteger d = 0; d < z; d += depth) for (NSUInteger y = 0; y < h; y += rows) {
            NSUInteger n = MIN(rows, h - y);
            up_blit_add_at(zb, 0, row, row * n, MTLSizeMake(w, n, MIN(depth, z - d)), mt, slice, l, MTLOriginMake(0, y, d));
        }
    }
    [g_up_texs addObject:mt];
    if (!g_cb) up_flush();
    if (!g_tex_zeroed++) MLOG("new-texture zeroing: on (new Metal textures are undefined; new Switch memory reads 0)");
}

// Initial content: what a memory pool's storage holds at nvnMemoryPoolInitialize is the pool's content, also in a pool the CPU cannot access afterwards (CPU_NO_ACCESS), where the
// game never writes it again. Rhythm Heaven Groove reads each texture file into memory and makes a CPU_NO_ACCESS | GPU_CACHED pool on it (245 by its first screens), with BC7 textures
// that have no packaged layout; as GPU textures of zeros its sprite shader discarded every fragment (alpha 0) and the screen stayed black. A new texture in such a pool whose storage
// holds data is filled from it once, as a GPU texture that can still be rendered into (no re-upload on page writes, unlike packaged / CPU-written ones). Left out: storage of zeros
// (the zero fill does the same), COMPRESSIBLE textures (their memory is meaningless without the compression tags), multisample, depth / stencil and sparse ones.
// In a pool the CPU can write, the same holds for storage the game flushed (nvnMemoryPoolFlushMappedRange) before the texture's first use, when the GPU has not written it yet
// (nvn_flushed in hle_nvn.c: Breath of the Wild's sky scattering tables, copied into their pools and flushed; without them the sky was brownish grey). DM_NVN_FLUSH_FILL=0 leaves
// those out, as before. In a CPU_UNCACHED pool the CPU's writes reach memory with no flush, and the GPU reads that memory: what the storage holds at the texture's first use is its
// content there too, flushed or not. Donkey Kong Country: Tropical Freeze's title logo and menu panels (ASTC 5x5, no packaged layout) sit in CPU_UNCACHED | GPU_CACHED pools
// (flags 0x22) whose storage holds their texels at first use (the 912x512 logo: 242,374 of 376,832 bytes non-zero), never flushed; as GPU textures of zeros they were magenta
int nvn_flushed(const void *p, uint64_t n);   // hle_nvn.c
static int mem_any(const uint8_t *p, uint64_t n) {   // whether any byte is non-zero (stops at the first)
    uint64_t i = 0, v;
    if (!p) return 0;
    for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); if (v) return 1; }
    for (; i < n; i++) if (p[i]) return 1;
    return 0;
}
static int tex_init_fill(const tex_t *t, id<MTLTexture> mt) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout) return 0;
    if (t->d.samples > 1 || ((int)mt.pixelFormat >= 250 && (int)mt.pixelFormat <= 262)) return 0;   // 250..262: Depth16Unorm … X24_Stencil8 (some names are macOS-only)
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0, uncached = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) != 0;
    // COMPRESSIBLE is a memory-compression hint, not a data format. In a CPU_NO_ACCESS pool the game never writes the storage, so the memory may mean nothing to the CPU (Rhythm Heaven Groove's render
    // targets). In a pool the CPU writes the texels are plain: SD GUNDAM G GENERATION CROSS RAYS (SDK 9.4.1) sets it on every texture it creates in 0xa2 pools and writes ordinary texels into them,
    // and skipping the fill left all 74 of them with content 0, the screen black but for text
    if (no_cpu && (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE)) return 0;
    static int flush_fill = -1; if (flush_fill < 0) { const char *e = getenv("DM_NVN_FLUSH_FILL"); flush_fill = !e || atoi(e) != 0; }
    if (!no_cpu && !uncached && (!flush_fill || !nvn_flushed(t->mem, t->L.size))) return 0;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread() || !mem_any(t->mem, t->L.size)) return 0;   // the upload batch belongs to the submit thread
    if (pkg_upload(t, NULL, mt, 0, nil, NULL, 0, NULL) != mt) return 0;
    int why = no_cpu ? 0 : uncached ? 1 : 2;
    static uint64_t n[3]; if (n[why]++ < 4) MLOG("initial content: ser=%llu %ux%ux%u format %#x filled from its storage, %s (%llu so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format,
                                                 (const char *[]){"a CPU_NO_ACCESS pool's", "a CPU_UNCACHED pool's", "flushed by the CPU before its first use"}[why], (unsigned long long)n[why]);
    return 1;
}

static int clear_rect_zero(int i);
// Whether a linear texture can be a view on its pool's buffer: 1 yes (*pf, *mb, *mo, *row filled), 0 not a linear texture this applies to, -1 linear but its offset or row pitch is not aligned
// to the device's minimum (16 bytes on the Macs measured; DM_TBUF_ALIGN raises it, for the tests). An unaligned one whose storage holds its
// content (linear_storage_ok) is uploaded from its storage instead, and again after each CPU write (tex_mtl_)
static NSUInteger tbuf_align(NSUInteger al);
// The offset a buffer texture's view may start at a multiple of, for its format: Metal's texture-buffer alignment or its linear-texture one, whichever is larger (16 bytes on an M4 Pro)
static NSUInteger tbuf_view_align(MTLPixelFormat pf) {
    NSUInteger a = [g_dev minimumTextureBufferAlignmentForPixelFormat:pf], l = [g_dev minimumLinearTextureAlignmentForPixelFormat:pf];
    return tbuf_align(a > l ? a : l);
}
static uint32_t tbuf_lead(int tid, id<MTLTexture> t);
static int linear_fit(const tex_t *t, uint32_t *pf, void **mb, uint64_t *mo, size_t *row) {
    // LINEAR_RENDER_TARGET is a linear layout too (SMBW's sky colour ramps: four 64×1 RGBA16F, written by the CPU every frame). Also in a pool the CPU cannot access, whose buffer holds what
    //   its storage held at creation (see tex_init_fill): Rhythm Heaven Groove's 228 linear R8 textures (sprite masks), read from files into CPU_NO_ACCESS pools, were private textures of zeros
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem || !t->pool) return 0;
    if (t->d.target != NVN_TEXTURE_TARGET_2D || t->d.levels > 1 || t->d.samples > 1) return 0;
    uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!nvn_fmt_to_mtl(t->d.format, pf, swz) || !*pf || !bpb || bw != 1 || bh != 1) return 0;
    *row = tex_linear_row(t);
    if (!mtl_addr((uint64_t)t->mem, (uint64_t)*row * (t->d.height ? t->d.height : 1), mb, mo)) { RED_ONCE(64, "linear texture storage is not in a memory pool (%ux%u format %#x)", t->d.width, t->d.height, t->d.format); return 0; }
    NSUInteger al = tbuf_align([g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)*pf]);
    if (*mo % al || *row % al) {
        static int n; if (n++ < 4) MLOG("linear texture ser=%llu %ux%u format %#x flags %#x: offset %#llx / row pitch %zu not aligned to %lu bytes (%d so far)",
                                        (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.flags, (unsigned long long)*mo, *row, (unsigned long)al, n);
        return -1;
    }
    return 1;
}
// Whether a linear texture's storage holds its content, so that one no view can take is uploaded from there and again after each CPU write (tex_mtl_), and read there (mtl_read_texels).
// In NVN a linear texture's content is its storage until the GPU writes it. LINEAR alone: always, as a GPU copy into it is written into the storage too (tex_storage_dst).
// LINEAR_RENDER_TARGET or IMAGE only say that the GPU may render or store into it, not that it does: Hades II decodes its Bink movies on the CPU into IMAGE | LINEAR R8 planes of
// 736-byte rows that its shaders only sample (GitHub issue #1956). Such a texture's storage holds its content while the GPU has not written it (gws_owned, from the GPU writes the
// commands record, mtl_tex_gpu_written), and one taken from it becomes a GPU texture at the GPU's next write there (cpu_unc_take / cpu_unc_drop). In a pool the CPU cannot write,
// GPU writes are not recorded: there it stays a GPU texture (its first fill takes its storage, tex_init_fill)
static int gws_owned(uintptr_t lo, uintptr_t hi);
static int linear_gpu_may_write(const tex_t *t) {   // a linear texture the GPU may render or store into; IMAGE alone is a block-linear texture
    return (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_IMAGE));
}
static int linear_storage_ok(const tex_t *t) {
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem) return 0;
    if (!linear_gpu_may_write(t)) return 1;
    return t->pool && !(t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) && !gws_owned((uintptr_t)t->mem, (uintptr_t)t->mem + t->L.size);
}
// Linear textures (LINEAR) in a CPU-visible pool: the game writes the pool memory directly from the CPU (SMBW's glyph cache: 2048×8192 R8, IMAGE|LINEAR, pool CPU_UNCACHED|GPU_CACHED,
// the font engine rasterizes glyphs into it on the CPU). A GPU-private texture never sees those writes → all menu / dialog text is blank. On unified memory, create a linear texture directly on the pool buffer (zero copy): the GPU reads the current content at execution time, same as NVN.
static id<MTLTexture> tex_linear(const tex_t *t) {
    uint32_t pf = 0; void *mb; uint64_t mo; size_t row;
    int fit = linear_fit(t, &pf, &mb, &mo, &row);
    if (fit < 0) RED_ONCE(65, "linear texture %ux%u format %#x flags %#x: rows of %zu bytes no view can take, a private texture the CPU's writes do not reach (the GPU wrote its storage, or first used off the submit thread)",
                          t->d.width, t->d.height, t->d.format, t->d.flags, row);
    if (fit != 1) return nil;
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:(MTLPixelFormat)pf width:t->d.width ? t->d.width : 1 height:t->d.height ? t->d.height : 1 mipmapped:NO];
    d.resourceOptions = b.resourceOptions;
    d.usage = MTLTextureUsageShaderRead | img_usage(t, (MTLPixelFormat)pf) | (t->d.flags & NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET ? MTLTextureUsageRenderTarget : 0);
    return [b newTextureWithDescriptor:d offset:mo bytesPerRow:row];
}
// CPU-written block-linear textures (WriteTexels): the game gave no packaged layout and changes the content from the CPU each frame (Engage's movie frames: a Y plane R8 + a UV plane RG8, WriteTexels every frame).
// Only packaged textures used to be uploaded from guest memory, so such a texture became a GPU-only texture whose content stayed 0. Their serials are recorded and they resolve through the packaged path (upload + re-upload on dirty pages)
// Every texture resolve asks about CPU writes. Mix serial keys: consecutive serials in a callback-free CFSet made misses probe long bucket runs (see reference/frame-rate.md).
// Finalize removes live membership, but command records and registered descriptors can still resolve the serial. Keep their classification until the storage pool is finalized;
// a copied flag would miss writes made after recording. Retired metadata can grow while a pool lives: expiring it earlier requires tracking ownership of all snapshots.
// Partitioned by pool so a memory pool Finalize (cpu_tex_drop_pool) drops only that pool's tables in O(1) for unrelated pools and frees their memory, instead of walking every pool's
// retired entries under g_cpu_tex_mu (Ocarina of Time: 2.3 million retired entries in permanent texture heaps after 160 loads from the pause menu). Under g_cpu_tex_mu
typedef struct { uintptr_t lo, hi; } cpu_unc;
typedef struct { nvn_hmap live, retired, unc; } cpu_pool;
static pthread_mutex_t g_cpu_tex_mu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_cpu_pools = {.klen = 8};   // pool key (t->pool, or (void *)1 for a pool-less test texture) → malloc'ed cpu_pool
static unsigned g_cpu_tex_n;                 // total live entries across all pools (mtl_cpu_tex_count)
static inline uint64_t cpu_pool_key(const void *pool) { return (uint64_t)(uintptr_t)(pool ? pool : (const void *)1); }
static cpu_pool *cpu_pool_get(const void *pool, int make) {   // g_cpu_tex_mu held
    uint64_t k = cpu_pool_key(pool);
    cpu_pool *cp = nvn_hm_get(&g_cpu_pools, &k);
    if (cp || !make) return cp;
    cp = calloc(1, sizeof *cp);
    if (!cp) MDIE("cpu_pool_get: out of memory");
    cp->live.klen = cp->retired.klen = cp->unc.klen = 8;
    nvn_hm_put(&g_cpu_pools, &k, cp);
    return cp;
}
static void cpu_tex_drop_pool(const void *pool) {
    if (!pool) return;
    pthread_mutex_lock(&g_cpu_tex_mu);
    uint64_t k = cpu_pool_key(pool);
    cpu_pool *cp = nvn_hm_del(&g_cpu_pools, &k);
    if (cp) {
        g_cpu_tex_n -= cp->live.n;
        for (uint32_t i = 0; i < cp->unc.cap; i++) free(cp->unc.vals[i]);
        free(cp->live.keys); free(cp->live.vals);
        free(cp->retired.keys); free(cp->retired.vals);
        free(cp->unc.keys); free(cp->unc.vals);
        free(cp);
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
}
// Storage the GPU writes, by address range: a render target, a texture clear, a copy into a texture or a shader image recorded over it (mtl_tex_gpu_written). tex_cpu_content takes an
// uncompressed 2D texture from its storage unless the GPU wrote that storage and the CPU has not written it since. The rule was per pool: Ocarina of Time draws its terrain tiles' normal
// maps into the pool that also holds their height (260x260 R32F) and blend / hole (260x260 RGBA8, two layers) maps, which the CPU fills before nvnTextureInitialize with no flush, so
// every tile streamed in after the first normal map was drawn came out as zeros: holes in the ground, until a reload (aCidcOde found the pool rule's part in GitHub issue #1326). A span
// keeps the dirty-page generation of its last GPU write (stamp) and has its whole pages write-protected, so a CPU write into it afterwards (a tile's storage reused for another texture)
// is seen. Only whole pages count: a span's first and last pages are shared with whatever lies beside it, which the CPU may write at any time. Spans do not overlap: a GPU write over
// part of one cuts it, and the parts outside keep their own stamp, so a CPU write there since stays seen (merging them made the whole merged span the GPU's again). Storage the CPU
// cannot write (a CPU_NO_ACCESS or virtual pool) has no span: tex_cpu_content takes no texture there, and most render targets lie there. GPU writes are stamped when recorded, so a
// command buffer submitted again does not stamp its writes again (open: storage the CPU wrote since counts as the CPU's until the next recorded GPU write there)
typedef struct { uintptr_t lo, hi; uint64_t stamp, seen; const void *rg; int taken; } gws_span;   // seen: dirty generation at the last look; rg: the dirty-page range holding its whole pages
                                                                                                 // (NULL: none, or untracked); taken: tex_cpu_content took a texture over it since its stamp
static gws_span *g_gws; static size_t g_ngws, g_cgws; static pthread_mutex_t g_gws_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_gws_new;   // the GPU wrote storage anew, or wrote storage a texture was taken from, since cpu_unc_drop last looked
static void whole_pages(uintptr_t lo, uintptr_t hi, uintptr_t *a, uintptr_t *e) {
    uintptr_t pg = (uintptr_t)getpagesize();
    *a = (lo + pg - 1) & ~(pg - 1);
    *e = hi & ~(pg - 1);
}
static size_t gws_find(uintptr_t a) {   // the first span that ends after a; g_gws_mu held
    size_t l = 0, h = g_ngws;
    while (l < h) {
        size_t m = (l + h) / 2;
        if (g_gws[m].hi <= a) {
            l = m + 1;
            continue;
        }
        h = m;
    }
    return l;
}
static void gws_stamp(gws_span *s) {   // the stamp first, then the protection (nvn_dirty_now); g_gws_mu held
    uintptr_t a, e;
    whole_pages(s->lo, s->hi, &a, &e);
    s->stamp = s->seen = nvn_dirty_now();
    s->rg = e > a ? nvn_dirty_span((const void *)a, e - a) : NULL;
    if (s->rg) {
        nvn_dirty_protect((const void *)a, e - a);
    }
}
// The CPU wrote a whole page of [lo, hi) within the span after its stamp; g_gws_mu held. Through the span's kept range: a scan of every registered pool per look would be on the
// recording thread, at every render target, clear and copy, whenever any page anywhere was written
static int gws_cpu_since(gws_span *s, uintptr_t lo, uintptr_t hi) {
    uintptr_t a, e;
    whole_pages(MAX(lo, s->lo), MIN(hi, s->hi), &a, &e);
    if (e <= a || !s->rg) {
        return 0;
    }
    return nvn_dirty_since_cached(&s->rg, (const void *)a, e - a, s->stamp);
}
static void gws_rewritten(gws_span *s) {   // the GPU writes a span again: a CPU write since its stamp is overwritten now; g_gws_mu held
    uint64_t now = nvn_dirty_now();
    if (now == s->seen) {
        return;
    }

    s->seen = now;
    if (!gws_cpu_since(s, s->lo, s->hi)) {
        return;
    }

    gws_stamp(s);
    if (s->taken) {
        s->taken = 0;
        atomic_store(&g_gws_new, 1);
    }
}
static void gws_room(size_t more) {   // g_gws_mu held
    if (g_ngws + more <= g_cgws) {
        return;
    }

    size_t cap = g_cgws ? 2 * g_cgws : 256;
    while (cap < g_ngws + more) {
        cap *= 2;
    }
    gws_span *grown = realloc(g_gws, cap * sizeof *g_gws);
    if (!grown) {
        MDIE("GPU-written storage spans: out of memory");
    }
    g_gws = grown;
    g_cgws = cap;
}
static void gws_set(uintptr_t lo, uintptr_t hi) {   // [lo, hi) becomes one span, stamped now; the spans it overlaps keep their parts outside it, with their own stamps; g_gws_mu held
    size_t i = gws_find(lo), j = i;
    while (j < g_ngws && g_gws[j].lo < hi) {   // spans i .. j-1 overlap [lo, hi)
        j++;
    }

    int nl = i < j && g_gws[i].lo < lo, nr = i < j && g_gws[j - 1].hi > hi;
    gws_span left = nl ? g_gws[i] : (gws_span){0}, right = nr ? g_gws[j - 1] : (gws_span){0};
    left.hi = lo;
    right.lo = hi;
    size_t n = (size_t)nl + 1 + (size_t)nr;
    gws_room(n);
    memmove(g_gws + i + n, g_gws + j, (g_ngws - j) * sizeof *g_gws);
    g_ngws = g_ngws - (j - i) + n;
    if (nl) {
        g_gws[i++] = left;
    }
    g_gws[i] = (gws_span){.lo = lo, .hi = hi};
    gws_stamp(&g_gws[i]);
    if (nr) {
        g_gws[i + 1] = right;
    }
    atomic_store(&g_gws_new, 1);
}
void mtl_tex_gpu_written(const tex_t *t) {
    if (!t || !t->mem || !t->L.size || !t->pool || (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL))) {
        return;
    }

    uintptr_t lo = (uintptr_t)t->mem, hi = lo + t->L.size;
    pthread_mutex_lock(&g_gws_mu);
    size_t i = gws_find(lo);
    if (i < g_ngws && g_gws[i].lo == lo && g_gws[i].hi == hi) {
        gws_rewritten(&g_gws[i]);
    } else {
        gws_set(lo, hi);
    }
    pthread_mutex_unlock(&g_gws_mu);
}
// A span whose overlap with [lo, hi) holds no whole page of it, and which does not cover it, is a sliver of something beside it, and does not decide: textures are 512-byte aligned,
// so a GPU-drawn one may end in the page a CPU-filled one begins in; a span outlives the texture it was recorded for, so the game may hand part of that storage to a new texture; and
// a GPU write over part of a span leaves the parts outside, which can be smaller than a page (gws_set). gws_cpu_since cannot answer for a sliver (no whole page to ask the dirty
// tracker about), and taking its silence as the GPU's made the texture behind it a GPU texture: Ocarina of Time's CPU-filled 260x260 height (R32F) and hole (RGBA8) maps came up as
// zeros when the span of a 260x260 RG8 normal map (laid out at 214,528 bytes) reached 1,536 or 13,824 bytes into them, or a 2,560-byte remnant of an old span lay inside them, and
// whole terrain tiles vanished or showed another tile's surface (GitHub issue #1566). A span that covers the whole texture keeps the GPU's: a texture smaller than a page inside storage
// the GPU wrote, whose neighbours' CPU writes in the same page do not count for it either
static int gws_sliver(const gws_span *s, uintptr_t lo, uintptr_t hi) {
    if (s->lo <= lo && s->hi >= hi) {
        return 0;
    }
    uintptr_t a, e;
    whole_pages(MAX(lo, s->lo), MIN(hi, s->hi), &a, &e);
    return e <= a;
}
static int gws_owned(uintptr_t lo, uintptr_t hi) {   // the GPU wrote some of [lo, hi) and the CPU has not rewritten it since: the storage is not the content
    int owned = 0;
    pthread_mutex_lock(&g_gws_mu);
    for (size_t i = gws_find(lo); i < g_ngws && g_gws[i].lo < hi && !owned; i++) {
        owned = !gws_sliver(&g_gws[i], lo, hi) && !gws_cpu_since(&g_gws[i], lo, hi);
    }
    pthread_mutex_unlock(&g_gws_mu);
    return owned;
}
static void gws_take(uintptr_t lo, uintptr_t hi) {   // a texture over [lo, hi) is taken from its storage: the next GPU write there makes it a GPU texture (gws_rewritten, gws_set)
    pthread_mutex_lock(&g_gws_mu);
    for (size_t i = gws_find(lo); i < g_ngws && g_gws[i].lo < hi; i++) {
        g_gws[i].taken = 1;
    }
    pthread_mutex_unlock(&g_gws_mu);
}
// A pool's zero-copy buffer, which the GPU writes through. One made while any of its pages is read-only drops every GPU write through it, silently, so it is made over its protected pages
// opened (nvn_dirty_open: writable, not counted as written, protected again once it is made; issue #1591). A chunk of a large pool is made long after the pool, by when uploads and GPU writes
// have protected pages in it; a small pool's first or last page may be a neighbour's, protected by that pool's texture
static id<MTLBuffer> pool_buf(uintptr_t a, uintptr_t b) {
    nvn_dirty_open((void *)a, b - a);
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    nvn_dirty_close();
    return buf;
}
static void gws_drop_pool(const pool_t *p) {   // the pool goes: its spans with it
    if (!p->mem || (p->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) {
        return;
    }

    uintptr_t lo = (uintptr_t)p->mem, hi = lo + p->size;
    pthread_mutex_lock(&g_gws_mu);
    size_t i = gws_find(lo), j = i;
    while (j < g_ngws && g_gws[j].lo < hi) {
        j++;
    }
    memmove(g_gws + i, g_gws + j, (g_ngws - j) * sizeof *g_gws);
    g_ngws -= j - i;
    pthread_mutex_unlock(&g_gws_mu);
}
// The GPU owns the texture's content: its host texture was made to be rendered into or written by shaders (nvn_ft_poll in hle_nvn.c never re-uploads such a texture from guest memory)
int mtl_tex_gpu_owned(uint64_t serial) {
    __unsafe_unretained id o = tex_get(serial);
    if (!obj_is_tex(o)) return 0;
    return (((id<MTLTexture>)o).usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite)) != 0;
}
void mtl_tex_cpu_written(const tex_t *t) {
    uint64_t serial = t->serial;
    if (!serial) return;
    // nvn_ft_poll can mark a snapshot after Finalize. Serialize the live check with release (g_smu → g_cpu_tex_mu), so that mark goes straight to retired metadata.
    pthread_mutex_lock(&g_smu);
    pthread_mutex_lock(&g_cpu_tex_mu);
    int live = own_tex_live_locked(serial);
    if (live || (t->pool && t->mem)) {
        cpu_pool *cp = cpu_pool_get(t->pool, 1);
        if (!nvn_hm_get(&cp->live, &serial) && !nvn_hm_get(&cp->retired, &serial)) {
            nvn_hm_put(live ? &cp->live : &cp->retired, &serial, t->pool ? (void *)t->pool : (void *)1);
            if (live) g_cpu_tex_n++;
            own_note(serial);   // its texture IDs resolve again through the packaged path (tex_by_id view_pkg)
            static int n; if (n++ < 16) MLOG("CPU-written texture ser=%llu %ux%ux%u format %#x target %d levels %u: uploaded / re-uploaded like a swizzled texture", (unsigned long long)serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels);
        }
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
    pthread_mutex_unlock(&g_smu);
}
unsigned mtl_cpu_tex_count(void) { pthread_mutex_lock(&g_cpu_tex_mu); unsigned n = g_cpu_tex_n; pthread_mutex_unlock(&g_cpu_tex_mu); return n; }
static int tex_cpu_written(const tex_t *t) {
    if (!t || !t->serial) return 0;
    uint64_t serial = t->serial;
    pthread_mutex_lock(&g_cpu_tex_mu);
    const cpu_pool *cp = cpu_pool_get(t->pool, 0);
    int r = cp && (nvn_hm_get(&cp->live, &serial) != NULL || nvn_hm_get(&cp->retired, &serial) != NULL);
    pthread_mutex_unlock(&g_cpu_tex_mu);
    return r;
}
// Whether the host takes a texture's content from its storage (tex_mtl_'s packaged path): a packaged layout, CPU-written, or a sampled texture in a sparse virtual pool, which holds what
// the CPU wrote into the physical pages mapped there (Monster Hunter Rise: the hunter's innerwear, ASTC 6x6 with no packaged layout, came out magenta, and the face black) and is read
// through the page table. Render targets, images, linear and buffer textures stay GPU textures (as ft_add), except a linear texture whose storage holds its content that no view on
// its buffer can take (linear_storage_ok, made CPU-written by tex_mtl_). mtl_read_texels reads these from the storage too, on whatever thread asks
static int tex_from_storage(const tex_t *t) {
    if (!t->mem) return 0;
    if (t->has_layout || tex_cpu_written(t)) return 1;
    if (!t->pool || t->d.target == NVN_TEXTURE_TARGET_BUFFER || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET |
        NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE))) return 0;
    if (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)
        return nvn_sparse_pool(t->pool);
    // SPARSE selects the layout even in non-virtual memory. Bloodstained writes these compressed textures through a CPU-visible pool after creating them.
    // Track their storage from the start so that both the first CPU writes and later mip uploads reach the host texture.
    return (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) &&
        t->d.samples <= 1 && t->d.target != NVN_TEXTURE_TARGET_1D_ARRAY && fmt_compressed(t->d.format);
}
// Storage the CPU wrote before the texture's first use (every game): until the GPU first uses a texture it has not written it, and a block-compressed one it can never render into
// (BC and ASTC formats are not colour-renderable; GPU copies into it are written into its storage too, tex_storage_dst). So a sampled block-compressed texture in a pool the CPU can
// write, with no packaged layout, whose storage holds data at its first use (tex_mtl_, submit thread) holds what the CPU wrote, flushed or not, before or after nvnTextureInitialize:
// it is made CPU-written, uploaded now and re-uploaded when its pages change. A one-time fill (tex_init_fill) is not enough: that GPU texture goes into the sharing table (own_alias_publish),
// and a new texture object over the same storage with the same description gets it back after the CPU has rewritten the storage. The Great Ace Attorney Chronicles (Japanese release
// 0100D7F00FB1A000, GitHub issue #568) decodes its court-record pictures (256x256 to 2048x2048 BC1, a CPU_CACHED pool, no flush) into storage it reuses, building a new texture over
// it for each item: they were black, and once filled once, showed the previous item's picture. An uncompressed 2D texture cannot be told from a render target by its flags (NVN has
// none for that, so tex_desc gives it render-target use), but storage the GPU has not written holds only what the CPU wrote: there it takes its storage the same way (Mario Kart World's
// terrain patches, 260x260 R16, RG8, RGBA8, R11G11B10F and RGBA16 in a CPU_CACHED pool, written with no flush: zeros, so every patch was a flat plane at its base height; see gws_span for
// Ocarina of Time's). Once the GPU writes that storage such a texture becomes a GPU texture (cpu_unc_drop), and over storage the GPU writes it keeps the flushed-range fill. An uncompressed
// 3D texture with none of the flags below is taken from its storage in any pool: Xenoblade
// Chronicles 3's particle steering fields (10x10x10 RGBA8 and RGBA16F, flags 0) were GPU textures of zeros, so every send-off particle moved along (-1, -1, -1) and sank into the ground;
// the 3D textures it renders into carry IMAGE | COMPRESSIBLE. That is what Xenoblade Chronicles 3, Breath of the Wild and Tears of the Kingdom do, not an NVN rule: a 3D render target
// with neither flag and data in its storage at first use would still be drawn into (attach gives it render-target use), but a ReadTexels of it, or a second texture object over the
// same storage, would read the storage rather than what the GPU drew. DM_NVN_STORAGE_FIRST_USE=0 turns this off (to measure it)
static int tex_unc(const tex_t *t) { return !fmt_compressed(t->d.format) && t->d.target != NVN_TEXTURE_TARGET_3D; }   // the uncompressed 2D kind, which the GPU can render into
// Whether the texture's storage can be what the host takes its content from, whatever it holds now (tex_cpu_content at first use, mtl_tex_cpu_rewritten after it); unc: tex_unc
static int tex_storage_ok(const tex_t *t, int unc) {
    if (!t->mem || !t->pool || t->has_layout || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    // not what only the GPU or the decoder fills (a display or video-decode texture, depth or stencil)
    if (unc && (fmt_depth(t->d.format) || (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE)))) return 0;
    if (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return 0;   // the CPU cannot write it (tex_init_fill fills it once) / sparse (tex_from_storage)
    // IMAGE only matters for an uncompressed texture: shaders cannot write a block-compressed one. Pokopia gives nearly every texture IMAGE (flags 0x4), its icons BC1 / BC7 in a
    // CPU_UNCACHED pool; left to the one-time fill, a new icon over reused storage got the previous icon's picture from the sharing table (xFractals/pokopia-native issue #1)
    return !((t->d.flags & (NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE))
             || ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && !fmt_compressed(t->d.format)));
}
static void cpu_unc_take(const tex_t *t) {   // an uncompressed texture is taken from its storage: the GPU's next write there makes it a GPU texture again (cpu_unc_drop)
    uintptr_t lo = (uintptr_t)t->mem, hi = lo + t->L.size;
    cpu_unc *u = malloc(sizeof *u);
    if (!u) {
        MDIE("cpu_unc_take: out of memory");
    }
    *u = (cpu_unc){lo, hi};

    uint64_t s = t->serial;
    pthread_mutex_lock(&g_cpu_tex_mu);
    cpu_pool *cp = cpu_pool_get(t->pool, 1);
    free(nvn_hm_put(&cp->unc, &s, u));
    pthread_mutex_unlock(&g_cpu_tex_mu);
    gws_take(lo, hi);
}
static int tex_cpu_content(const tex_t *t) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_STORAGE_FIRST_USE"); on = !e || atoi(e) != 0; }
    int unc = tex_unc(t);
    const pool_t *pool = t->pool;
    if (!on || !pool || !tex_storage_ok(t, unc)) return 0;
    // not over storage the GPU writes (gws_owned: it takes a lock)
    if ((unc && gws_owned((uintptr_t)t->mem, (uintptr_t)t->mem + t->L.size)) || !mem_any(t->mem, t->L.size)) return 0;
    if (unc) {
        cpu_unc_take(t);
    }
    static uint64_t n[2]; if (n[unc]++ < 4) MLOG("texture written by the CPU before its first use ser=%llu %ux%ux%u format %#x%s pool flags %#x: uploaded from its storage, re-uploaded when it changes (%llu so far)",
                                         (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, unc ? " (uncompressed, in storage the GPU has not written)" : "", pool->flags, (unsigned long long)n[unc]);
    return 1;
}
// nvn_ft_poll (hle_nvn.c, tex_dirty_upload): the CPU wrote the pages of a registered texture after its first use. It is taken from its storage by the rule tex_cpu_content uses at first
// use: a texture whose storage can hold its content (tex_storage_ok); an uncompressed 2D one not while the GPU has written that storage and the CPU has not rewritten it since (gws_owned:
// MTL_REWRITE_GPU_NOW, which the poll asks again at the CPU's next write), and with the same way back (cpu_unc_take: the GPU's next write there makes it a GPU texture again). Its host
// texture's usage cannot tell: tex_desc gives every uncompressed texture made without a CPU fill render-target use, and with the usage as the test every such rewrite was dropped. Ocarina of
// Time rewrites its terrain tiles' height (260x260 R32F) and blend / hole (260x260 RGBA8, two layers) maps from the CPU as tile slots are reused while the player walks: the tiles kept
// what they held before, flat blue holes in the ground after an hour. Its normal maps, which the CPU zeroes and the GPU then draws, stay the GPU's. A block-compressed texture is uploaded
// whatever the GPU wrote: it cannot be rendered into, and GPU copies into it while registered are written into its storage too (tex_storage_dst), as are a compressed-alias pair's
// copies of what its alias was written with (bcp_after_to, an image store's too), so a GPU-written span over it does not make its storage stale (Tears of the Kingdom's BC4 atlas and
// ASTC arrays). Any other texture keeps the usage test (MTL_REWRITE_GPU: left to the GPU for good), as before
int mtl_tex_cpu_rewritten(const tex_t *t) {
    int unc = tex_unc(t);
    if (tex_storage_ok(t, unc)) {
        if (unc && gws_owned((uintptr_t)t->mem, (uintptr_t)t->mem + t->L.size)) return MTL_REWRITE_GPU_NOW;
        mtl_tex_cpu_written(t);
        if (unc) {
            cpu_unc_take(t);
        }
        return MTL_REWRITE_UPLOADED;
    }
    if (mtl_tex_gpu_owned(t->serial)) return MTL_REWRITE_GPU;
    mtl_tex_cpu_written(t);
    return MTL_REWRITE_UPLOADED;
}
// WriteTexels wrote a linear texture's storage (hle_nvn.c write_texels). One a view on its pool's buffer can take (linear_fit) is that view, which reads the storage itself: made
// CPU-written, it would become an upload of the whole texture at each write. Any other is taken from its storage, as tex_mtl_ takes one at its first use (linear_storage_ok), and one the
// GPU may render or store into becomes a GPU texture again at the GPU's next write there (cpu_unc_drop)
void mtl_tex_linear_cpu_written(const tex_t *t) {
    uint32_t pf; void *mb; uint64_t mo; size_t row;
    if (linear_fit(t, &pf, &mb, &mo, &row) == 1) return;
    if (linear_gpu_may_write(t)) cpu_unc_take(t);
    mtl_tex_cpu_written(t);
}
// The GPU writes storage an uncompressed texture was taken from: what the storage holds is no longer the whole truth, so the texture becomes a GPU texture with the content it has, and
// CPU writes to it are no longer uploaded. Submit thread, as tex_cpu_content is, so a texture it has just taken is seen here if its storage was written meanwhile
static void cpu_unc_drop(void) {
    if (!atomic_load_explicit(&g_gws_new, memory_order_relaxed) || !atomic_exchange(&g_gws_new, 0)) return;
    uint64_t *ser = NULL; uint32_t n = 0, cap = 0;
    pthread_mutex_lock(&g_cpu_tex_mu);
    for (uint32_t p = 0; p < g_cpu_pools.cap; p++) {
        cpu_pool *cp = g_cpu_pools.vals[p];
        if (!cp || !cp->unc.n) continue;
        for (uint32_t i = 0; i < cp->unc.cap;) {
            cpu_unc *u = cp->unc.vals[i];
            if (!u || !gws_owned(u->lo, u->hi)) {
                i++;
                continue;
            }

            if (n == cap) { uint64_t *grown = realloc(ser, (cap = cap ? cap * 2 : 64) * sizeof *ser); if (!grown) MDIE("cpu_unc_drop: out of memory"); ser = grown; }
            uint64_t s; memcpy(&s, cp->unc.keys + (size_t)i * 8, 8);
            ser[n++] = s; nvn_hm_del(&cp->unc, &s);
            if (nvn_hm_del(&cp->live, &s)) g_cpu_tex_n--;
            nvn_hm_del(&cp->retired, &s);   // deletion shifts entries: examine this slot again
            free(u);
        }
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
    for (uint32_t i = 0; i < n; i++) {
        __unsafe_unretained id o = tex_get(ser[i]);
        if ([o isKindOfClass:[NvPkg class]]) { NvPkg *pk = o; id<MTLTexture> mt = pk->view ? pk->view : pk->tex; if (mt) tex_put(ser[i], mt); }
        own_note(ser[i]);
    }
    if (n) { static int nl; if (nl++ < 8) MLOG("storage the GPU writes: %u uncompressed textures taken from it are GPU textures from now on", n); }
    free(ser);
}
// Whether mtl_read_texels can read the texture from its storage: a block-linear layout nvn_deswizzle undoes (no linear, multisample or buffer texture)
static int tex_storage_readable(const tex_t *t) {
    return t->mem && t->d.samples <= 1 && t->d.target != NVN_TEXTURE_TARGET_BUFFER && t->d.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE && t->d.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY
           && !(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
}
// in_storage (may be NULL): set when, off the submit thread, the texture's content is its storage and making the host texture would take an upload, or would make one the CPU's
// content never reaches; nil is returned then and the caller reads the storage (mtl_read_texels)
static void *tex_mtl_resolve(const tex_t *ct, int *in_storage, id<MTLTexture> __strong *owned);
// Present and ReadTexels can outlast a replay fence. Take a strong reference inside the resolver, before its local owners go away.
static id<MTLTexture> tex_mtl_(const tex_t *ct, int *in_storage) {
    id<MTLTexture> owned = nil;
    tex_mtl_resolve(ct, in_storage, &owned);
    return owned;
}
static int bcp_up_fill(const tex_t *t, id<MTLTexture> mt); static void bcp_up_follow(const tex_t *t, id<MTLTexture> mt);   // compressed-alias pairs: a side made, a side uploaded from storage
static id<MTLTexture> tex_mtl(const tex_t *ct) { AP_B; id<MTLTexture> r = tex_mtl_(ct, NULL); AP_E(4); return r; }
// An open replay can borrow until its submission fence. A serial or package holds the result, and retirement waits for that fence (nvn_owner.h).
static void *tex_mtl_replay_ref(const tex_t *ct) { AP_B; void *r = tex_mtl_resolve(ct, NULL, NULL); AP_E(4); return r; }
#define tex_mtl_replay(ct) ((__bridge id<MTLTexture>)tex_mtl_replay_ref(ct))
static inline void *tex_mtl_result(id<MTLTexture> texture, id<MTLTexture> __strong *owned) {
    if (owned) *owned = texture;
    return (__bridge void *)texture;
}
// Diagnostic DM_NVN_REUPLOADS=1: a line for each re-upload of a texture taken from its storage (the first 400): its shape and size, how much of it was read from guest memory
// (pkg_written), the time it took on the submit thread, the frame and the submission (a frame's replay is many, and each looks at the pages once), the pages written since its upload and where the first of them lies,
// and the GPU writes stored into its storage since then (gsb_swz)
static void reup_note(const NvPkg *pk, const tex_t *t, unsigned taken, uint64_t bytes, uint64_t ns) {
    static int on = -1, nl; if (on < 0) on = getenv("DM_NVN_REUPLOADS") != NULL;
    if (!on || nl >= 400) return;
    nl++;
    uintptr_t first = 0; size_t pg = (size_t)getpagesize(), n = pk->sparse ? 0 : nvn_dirty_since_except(pk->span, t->mem, t->L.size, pk->stamp, NULL, 0, &first);
    unsigned layer = 0, lv = 0;
    if (n && n != SIZE_MAX && t->L.layer_size) { uint64_t o = first - (uintptr_t)t->mem; layer = (unsigned)(o / t->L.layer_size); o %= t->L.layer_size;
        while (lv + 1 < t->d.levels && nvn_tex_level_offset(&t->d, lv + 1) <= o) lv++; }
    char w[96]; if (pk->sparse) snprintf(w, sizeof w, "a sparse pool's"); else if (n == SIZE_MAX) snprintf(w, sizeof w, "untracked");
    else snprintf(w, sizeof w, "%zu of %zu, the first in layer %u level %u", n, (size_t)((t->L.size + pg - 1) / pg), layer, lv);
    char u[64]; if (taken) snprintf(u, sizeof u, "%.1f MB read, %u images copied from the texture it replaces", bytes / 1048576.0, taken); else snprintf(u, sizeof u, "read whole");
    MLOG("re-upload ser=%llu %ux%ux%u format %#x target %d levels %u: %.1f MB, %s, %.2f ms, frame %llu submission %llu; pages written since its upload: %s; GPU writes stored into its storage since then: %u",
         (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->L.size / 1048576.0, u, ns / 1e6, (unsigned long long)atomic_load(&g_cur_frame),
         (unsigned long long)g_sub_serial, w, pk->stored);
}
// A sparse texture is bound from its first level whose pages, and those of every smaller level, are all mapped: a view of levels [r, n) samples as with a minimum LOD of r.
// The translator drops TEX's LOD clamp and residency predicate, so Monster Hunter Rise's streamed textures were sampled at levels it had not mapped yet; those read 0, which ASTC
// decodes as its error colour (magenta clothes on the title screen). Single-layer 2D textures only; the view follows each upload, as the mappings change only with one
static void sparse_view(NvPkg *pk, const tex_t *t) {
    unsigned n = (unsigned)pk->tex.mipmapLevelCount, r = n;
    if (t->d.target == NVN_TEXTURE_TARGET_2D && n > 1)
        while (r > 0) { uint64_t lo = nvn_tex_level_offset(&t->d, r - 1), hi = r < n ? nvn_tex_level_offset(&t->d, r) : t->L.size;
            if (!nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + lo, hi - lo)) break;
            r--; }
    else r = 0;
    if (r == n) r = n - 1;   // nothing mapped: the smallest level (reads 0)
    if (pk->view) defer_obj_unref(pk->view);   // a view shares its texture's allocation: drop only the reference, the residency set keeps the texture
    pk->view = r ? [pk->tex newTextureViewWithPixelFormat:pk->tex.pixelFormat textureType:pk->tex.textureType levels:NSMakeRange(r, n - r) slices:NSMakeRange(0, 1)] : nil;
    if (r != (unsigned)pk->vbase) { static int nl; if (nl++ < 6) MLOG("sparse texture ser=%llu %ux%u: sampled from level %u of %u (finer levels not mapped)", (unsigned long long)t->serial, t->d.width, t->d.height, r, n); }
    pk->vbase = (int)r;
}
static uint64_t g_texinfo_ser[8]; static int g_texinfo_n;
// NVN storage is bytes, not descriptions: texture objects over one pool offset read and write one another's texels. A 4-sample colour target of W x H and the single-sample image
// of 2W x 2H in its format over the same offset are one storage, sample s of pixel (x, y) being texel (2x + (s & 1), 2y + (s >> 1)), and an engine may draw through one and read
// through the other: Metro 2033 Redux draws its effects (particles, beams, decals, the laser sight, the muzzle flash) into a 640x360 4-sample RGBA8_SRGB target over the bytes of
// its 1280x720 scene target, which its upscale reads (0x3c0000 bytes each). As two Metal allocations the effects went where nothing read them. Metal lays the two out alike: on
// Apple GPUs a 4-sample W x H texture placed over a 2W x 2H one in a heap holds every sample at that texel, compressed or not, in every colour format and size tried, when both
// have the same compression; 2 and 8 samples do not, nor a compressed one over an uncompressed one (Metal sizes those apart), checked by t_storage_pair. So whichever of a pair is
// made second is placed at offset 0 of the live other's heap (a resolution family's, image_texture, or recycled memory, rh_place) with its compression, only when Metal gives
// both one size and this GPU was seen to lay that kind of pair out so (pair_layout_ok, once per kind: GPUs no test ran on are checked as they run).
// It holds that heap's owner, so the heap waits for its next texture (rh_wait) only once both are gone, and it is not filled: the storage holds the other's content. While it
// lives, barriers flush aliased memory (resource_vis). A partner outside a heap (DM_NVN_RT_RECYCLE=0) keeps them apart with a red line. DM_NVN_STORAGE_ALIAS=0 keeps every
// pair apart
@interface NvStoragePair : NSObject @end   // held by the second texture of a pair
@implementation NvStoragePair
- (void)dealloc { atomic_fetch_sub(&g_pair_n, 1); }
@end
static char g_pair_key;
static int pair_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_STORAGE_ALIAS"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("storage pairs (a 4-sample target over a single-sample image's bytes): kept apart (DM_NVN_STORAGE_ALIAS=0)"); } return v; }
// One level of a 2D colour image, single-sample or 4-sample, in a GPU-only pool, laid out by NVN
static int pair_shape(const tex_t *t) {
    if (!t->pool || !t->pool->mem || !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || nvn_sparse_pool(t->pool) || t->has_layout) return 0;
    if ((t->d.flags & IMAGE_NOT_SHARED) || t->d.depth != 1 || t->d.levels != 1 || t->d.stride || fmt_depth(t->d.format) || fmt_compressed(t->d.format)) return 0;
    return (t->d.target == NVN_TEXTURE_TARGET_2D && t->d.samples <= 1) || (t->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE && t->d.samples == 4);
}
// b is the other half of a's storage: the same format and NVN storage size, one of them 4-sample and half the other's width and height
static int pair_of(const nvn_texdesc *a, const nvn_texdesc *b) {
    if (a->format != b->format || (a->samples > 1) == (b->samples > 1) || (b->flags & IMAGE_NOT_SHARED) || b->depth != 1 || b->levels != 1 || b->stride) return 0;
    const nvn_texdesc *m = a->samples > 1 ? a : b, *s = a->samples > 1 ? b : a;
    if (m->target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE || m->samples != 4 || s->target != NVN_TEXTURE_TARGET_2D || s->width != 2 * m->width || s->height != 2 * m->height) return 0;
    nvn_texlayout la, lb; nvn_tex_layout(a, &la); nvn_tex_layout(b, &lb);
    return la.size && la.size == lb.size;
}
// Whether this GPU lays the pair out as NVN does, checked once for each kind (format, size, both compressions) before the first such pair is made, since only Apple GPUs that
// were tried are known to (t_storage_pair): a scratch heap holds the two at offset 0, a pattern is copied into the single-sample one, and a kernel (dm/gfx/msl/pair.metal)
// compares every sample of the 4-sample one with its texel. About a millisecond, waited for on the classic queue; it touches none of the game's textures. *bad: the samples that
// differ (UINT32_MAX: not checked)
static _Atomic unsigned g_pair_probes;
static size_t mtl_pf_block_bytes(MTLPixelFormat pf); static int pf_int_kind(MTLPixelFormat f);
static int pair_layout_ok(MTLTextureDescriptor *a, MTLTextureDescriptor *b, uint32_t *bad) {
    typedef struct { NSUInteger pf, w, h; uint8_t c1, c4, ok; uint32_t bad; } kind;
    static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT; static kind seen[32]; static unsigned n;
    MTLTextureDescriptor *one = a.sampleCount > 1 ? b : a, *four = a.sampleCount > 1 ? a : b;
    kind k = {one.pixelFormat, one.width, one.height, (uint8_t)(one.allowGPUOptimizedContents && !(one.usage & MTLTextureUsagePixelFormatView)),
              (uint8_t)(four.allowGPUOptimizedContents && !(four.usage & MTLTextureUsagePixelFormatView)), 0, UINT32_MAX};
    os_unfair_lock_lock(&lk);
    for (unsigned i = 0; i < n; i++) if (seen[i].pf == k.pf && seen[i].w == k.w && seen[i].h == k.h && seen[i].c1 == k.c1 && seen[i].c4 == k.c4) { *bad = seen[i].bad; os_unfair_lock_unlock(&lk); return seen[i].ok; }
    os_unfair_lock_unlock(&lk);
    static id<MTLComputePipelineState> ps[3]; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSError *e = nil; id<MTLLibrary> lib = host_lib("pair", &e); const char *fn[3] = {"pair_cmp_f", "pair_cmp_u", "pair_cmp_i"};
        for (int i = 0; i < 3 && lib; i++) { id<MTLFunction> f = [lib newFunctionWithName:@(fn[i])]; ps[i] = f ? dev_cps(f, &e) : nil; } });
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    size_t bpt = mtl_pf_block_bytes(one.pixelFormat), row = one.width * bpt, bytes = row * one.height;
    id<MTLComputePipelineState> cmp = ps[pf_int_kind(one.pixelFormat)];
    MTLSizeAndAlign s1 = [g_dev heapTextureSizeAndAlignWithDescriptor:one], s4 = [g_dev heapTextureSizeAndAlignWithDescriptor:four];
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.size = MAX(s1.size, s4.size);
    id<MTLHeap> heap = cmp && bpt && hd.size ? [g_dev newHeapWithDescriptor:hd] : nil;
    id<MTLTexture> t1 = [heap newTextureWithDescriptor:one offset:0], t4 = [heap newTextureWithDescriptor:four offset:0];
    id<MTLBuffer> pat = t1 && t4 ? [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared] : nil, cnt = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (pat && cnt) {
        uint32_t *q = pat.contents;   // every word its own value, each byte below 0x40 so no float format reads an infinity or a NaN
        for (size_t i = 0; i < bytes / 4; i++) q[i] = ((uint32_t)i * 2654435761u >> 2 & 0x3f3f3f3fu) | 1u;
        *(uint32_t *)cnt.contents = 0;
        id<MTLCommandBuffer> cb = cl_cb(); id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];   // two command buffers: the placement heap's textures are not tracked
        [be copyFromBuffer:pat sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:bytes sourceSize:MTLSizeMake(one.width, one.height, 1) toTexture:t1 destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        id<MTLCommandBuffer> cc = cl_cb(); id<MTLComputeCommandEncoder> ce = [cc computeCommandEncoder];
        [ce useResource:t1 usage:MTLResourceUsageRead]; [ce useResource:t4 usage:MTLResourceUsageRead];
        [ce setComputePipelineState:cmp]; [ce setTexture:t1 atIndex:0]; [ce setTexture:t4 atIndex:1]; [ce setBuffer:cnt offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(four.width, four.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)]; [ce endEncoding]; [cc commit]; [cc waitUntilCompleted];
        if (cb.status == MTLCommandBufferStatusCompleted && cc.status == MTLCommandBufferStatusCompleted) { k.bad = *(uint32_t *)cnt.contents; k.ok = !k.bad; }
    }
    atomic_fetch_add(&g_pair_probes, 1);
    MLOG("storage pair layout: %lux%lu format %lu (compression %d / %d) over its 4-sample half: %s (checked in %.2f ms)", (unsigned long)k.w, (unsigned long)k.h, (unsigned long)k.pf, k.c1, k.c4,
         k.ok ? "every sample at its NVN texel" : k.bad == UINT32_MAX ? "not checked (no scratch heap or no kernel)" : "samples elsewhere", (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6);
    os_unfair_lock_lock(&lk); if (n < 32) seen[n++] = k; os_unfair_lock_unlock(&lk);
    *bad = k.bad; return k.ok;
}
static id<MTLTexture> pair_texture(const tex_t *t, MTLTextureDescriptor *d0) {
    if (!pair_shape(t) || !pair_on()) return nil;
    __block id<MTLTexture> other = nil;
    pthread_mutex_lock(&g_smu);
    own_alias_at_locked(t->pool, t->off, ^(const alias_key *k, id shared) {   // the shared textures that start at this offset
        if (!other && pair_of(&t->d, &k->d) && [shared conformsToProtocol:@protocol(MTLTexture)]) other = shared;
    });
    pthread_mutex_unlock(&g_smu);
    if (!other) return nil;
    char *key = &g_image_heap_owner_key; id owner = objc_getAssociatedObject(other, key);
    if (!owner) { key = &g_rh_owner_key; owner = objc_getAssociatedObject(other, key); }
    id<MTLHeap> heap = owner ? other.heap : nil;
    const char *why = !heap || other.heapOffset ? "is not at the start of a heap of the host's" : NULL;
    MTLTextureDescriptor *d = [d0 copy];
    d.allowGPUOptimizedContents = other.allowGPUOptimizedContents; d.usage |= other.usage & MTLTextureUsagePixelFormatView;   // (the format-view usage turns compression off)
    MTLSizeAndAlign need = {0};
    if (!why) {
        MTLTextureDescriptor *od = [MTLTextureDescriptor new];   // the size Metal gives the other: equal only with the same layout and compression
        od.textureType = other.textureType; od.pixelFormat = other.pixelFormat; od.width = other.width; od.height = other.height; od.sampleCount = other.sampleCount;
        od.storageMode = other.storageMode; od.usage = other.usage; od.allowGPUOptimizedContents = other.allowGPUOptimizedContents;
        need = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
        uint32_t bad = 0;
        if (!need.size || need.size != [g_dev heapTextureSizeAndAlignWithDescriptor:od].size || need.size > heap.size) why = "takes another number of bytes in Metal";
        else if (!pair_layout_ok(d, od, &bad)) why = bad == UINT32_MAX ? "could not be checked against it on this GPU" : "this GPU lays out otherwise (its samples are not at NVN's texels)";
    }
    if (why) {
        RED_ONCE(174, "storage pair: ser=%llu %ux%u %d-sample format %#x shares its pool bytes with a %ux%u %d-sample texture that %s; kept apart, so what is drawn through one is not seen through the other",
                 (unsigned long long)t->serial, t->d.width, t->d.height, t->d.samples, t->d.format, (unsigned)other.width, (unsigned)other.height, (int)other.sampleCount, why);
        return nil;
    }
    id<MTLTexture> mt = [heap newTextureWithDescriptor:d offset:0]; if (!mt) return nil;
    objc_setAssociatedObject(mt, key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    atomic_fetch_add(&g_pair_n, 1); objc_setAssociatedObject(mt, &g_pair_key, [NvStoragePair new], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    res_add(mt);
    static int n; if (n++ < 6) MLOG("storage pair: ser=%llu %ux%u %d-sample format %#x is placed over the %lux%lu %lu-sample texture at its pool bytes (%llu MB, compression %d)", (unsigned long long)t->serial,
                                    t->d.width, t->d.height, t->d.samples, t->d.format, (unsigned long)other.width, (unsigned long)other.height, (unsigned long)other.sampleCount,
                                    (unsigned long long)(heap.size >> 20), (int)d.allowGPUOptimizedContents);
    return mt;
}
// Distinct compressible image descriptions at one NVN address alias physical memory, even while all their descriptor objects are live (LM3's resolution families).
// Single-description storage keeps the ordinary allocation. Families share an exact-sized placement heap, found through the existing alias table rather than a pool-lifetime owner.
// Different formats, offsets, CPU-visible, sparse, linear, display, layered and multisample images keep their existing path. Inherited texels are not translated between incompatible
// layouts: a changed layout is initialized by its GPU writer. Creating its descriptor must not zero another alias's live content in the earlier upload batch.
static id<MTLTexture> image_texture(const tex_t *t, MTLTextureDescriptor *d0) {
    if (!image_shape(t) || !t->pool->mem || !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || nvn_sparse_pool(t->pool)) return nil;
    // Family members are written uncompressed (DM_NVN_FAMILY_NOCOMPRESS, on unless =0). With lossless compression a member read after another resolution wrote the shared bytes
    // decodes the other's compressed blocks as garbage: ASTRAL CHAIN's 1600x900 HDR targets (loaded, never cleared) came back from the 1440x810 pause menu with 1.1M non-finite
    // pixels, which bloom spread into a strobing pink / white glow. Uncompressed, the inherited bytes are only misplaced texels of the same format, as on the console
    static int nocomp = -1; if (nocomp < 0) { const char *e = getenv("DM_NVN_FAMILY_NOCOMPRESS"); nocomp = !(e && !strcmp(e, "0")); }
    MTLTextureDescriptor *d = d0; if (nocomp) { d = [d0 copy]; d.allowGPUOptimizedContents = NO; }
    MTLSizeAndAlign need = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    if (!need.size) return nil;
    __block NvImageHeap *owner = nil; MTLTextureDescriptor *largest = d; NSUInteger capacity = need.size; int family = 0;
    pthread_mutex_lock(&g_smu);
    own_alias_at_locked(t->pool, t->off, ^(const alias_key *k, id shared) {   // the shared textures that start at this offset (the owner's index)
        if (k->d.format != t->d.format) return;
        id<MTLTexture> other = shared;
        NvImageHeap *o = objc_getAssociatedObject(other, &g_image_heap_owner_key);
        // Metal omits ShaderAtomic from usage; tex_desc gives every writable R32 integer image that capability.
        MTLTextureUsage usage = other.usage | (tex_atomic_capable(other) ? MTLTextureUsageShaderAtomic : 0);
        if (o && o->heap.size >= need.size && usage == d.usage && (!owner || o->heap.size < owner->heap.size)) owner = o;
    });
    if (!owner) for (uint32_t i = 0; i < g_image_desc.cap; i++) if (g_image_desc.vals[i]) {
        const tex_t *other = g_image_desc.vals[i];
        if (other->pool != t->pool || other->off != t->off || other->d.format != t->d.format) continue;
        family |= other->d.width != t->d.width || other->d.height != t->d.height;
        MTLTextureDescriptor *od = tex_desc(other, 0); if (!od) continue;
        if (nocomp) od.allowGPUOptimizedContents = NO;
        MTLSizeAndAlign sz = [g_dev heapTextureSizeAndAlignWithDescriptor:od];
        if (sz.size > capacity) { capacity = sz.size; largest = od; }
    }
    pthread_mutex_unlock(&g_smu);
    int fresh = !owner;
    if (fresh) {
        if (!family) return nil;
        int reused; id<MTLHeap> heap = rh_heap(capacity, d.storageMode, d.cpuCacheMode, d.hazardTrackingMode, &reused); if (!heap) return nil;   // from the recycled heaps
        owner = [NvImageHeap new]; owner->heap = heap; atomic_fetch_add(&g_image_heap_n, 1);
    }
    id<MTLTexture> mt = [owner->heap newTextureWithDescriptor:d offset:0]; if (!mt) return nil;
    objc_setAssociatedObject(mt, &g_image_heap_owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    res_add(mt);
    if (fresh) {
        // Initialize the whole family, including storage outside a first-used small resolution. Only a new heap is zeroed; later descriptors alias its existing bytes.
        id<MTLTexture> zero = largest == d ? mt : [owner->heap newTextureWithDescriptor:largest offset:0];
        if (!zero) return nil;
        objc_setAssociatedObject(zero, &g_image_heap_owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC); tex_zero_fill(zero);
    }
    return mt;
}
// The host texture a texture already has, made and refreshed by nothing: for a texture whose storage is gone (a memory pool finalized, tex_mtl_ and tex_by_id). nil if it has none
static id<MTLTexture> tex_made(const tex_t *t) {
    id have = tex_get(t->serial);
    if ([have isKindOfClass:[NvPkg class]]) { NvPkg *pk = have; return pk->view ? pk->view : pk->tex; }
    return have;
}
// The NVN level that level 0 of base, the host texture tex_made or tex_mtl gave for t, stands for: a sparse texture whose finer levels are not mapped is bound as the view of its levels
// from the first mapped one on (sparse_view), so NVN level l is that view's level l - vbase. 0 for every other texture
static unsigned tex_level_shift(const tex_t *t, id<MTLTexture> base) {
    id have = tex_get(t->serial);
    if (![have isKindOfClass:[NvPkg class]]) return 0;
    NvPkg *pk = have; return pk->view && pk->view == base ? (unsigned)pk->vbase : 0;
}
// The host texture for a copy or a clear, which names its levels as NVN does: a sparse texture bound from a coarser level (sparse_view) is copied through the texture its view is of, all
// of its levels. Through the view, a copy into level 0 of a texture bound from level 1 went to level 1, and gsb_store found a host texture narrower than the texture (Densha de GO!!
// Hashiro Yamanote Line streams the finer levels of its sparse ASTC textures in by copies, GitHub #2350)
static id<MTLTexture> tex_mtl_all(const tex_t *t) { id<MTLTexture> mt = tex_mtl_replay(t); return mt && tex_level_shift(t, mt) ? mt.parentTexture : mt; }
// NVN formats whose Metal format is the other one of an sRGB / linear pair (g_pfv_srgb) of a format's Metal format: up to 4, zero-terminated (an RGBA8 texture has RGBA8_SRGB and RGBX8_SRGB)
static const uint8_t *srgb_twins(uint32_t f) {
    static uint8_t tw[256][5]; static dispatch_once_t once;
    dispatch_once(&once, ^{
        for (int a = 1; a < 256; a++) { uint32_t pa = 0, pb = 0; uint8_t sw[4]; int n = 0;
            if (!nvn_fmt_to_mtl(a, &pa, sw) || !pa) continue;
            for (int b = 1; b < 256 && n < 4; b++) {
                if (b == a || !nvn_fmt_to_mtl(b, &pb, sw) || !pb || pa == pb) continue;
                for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++)
                    if ((pa == g_pfv_srgb[i].a && pb == g_pfv_srgb[i].b) || (pa == g_pfv_srgb[i].b && pb == g_pfv_srgb[i].a)) { tw[a][n++] = (uint8_t)b; break; }
            }
        }
    });
    return f < 256 ? tw[f] : tw[0];
}
// The usage of a buffer texture's view: every use the guest may make of it, whatever its IMAGE flag. A view on its pool's buffer has no lossless compression for the write and
// format-view usages to cost (measured on an M6: reads of R32F, R32UI, RGBA8, RGBA16F and RGBA32F views over 64 MB the same with and without them, within run-to-run noise), and
// its NVN texture objects share one view (tex_mtl_), the IMAGE ones among them: made with read usage alone, the first image write or format view of a view another object made
// first rebuilt it (tex_upgrade). Metal makes no writable BGRA8Unorm texture buffer (its validation refuses one; every other format the host maps takes the write usage)
static MTLTextureUsage tbuf_usage(MTLPixelFormat pf) {
    MTLTextureUsage u = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (pf != MTLPixelFormatBGRA8Unorm) u |= MTLTextureUsageShaderWrite | (pf_atomic(pf) ? MTLTextureUsageShaderAtomic : 0);
    return u;
}
// Buffer texture (TARGET_BUFFER): a texture-buffer view directly on the memory pool buffer (zero copy; the GPU reads the pool's current content at execution time, as NVN does).
// Not made resident here: tex_mtl_ does that once it is the one the storage shares
static id<MTLTexture> tex_buffer(const tex_t *t) {
    uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    void *mb; uint64_t mo;
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb) { RED_ONCE(36, "buffer texture format %#x not mapped", t->d.format); return nil; }
    if (!t->mem || !mtl_addr((uint64_t)t->mem, (uint64_t)t->d.width * bpb, &mb, &mo)) { RED_ONCE(37, "buffer texture storage is not in a memory pool"); return nil; }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
    NSUInteger al = tbuf_view_align((MTLPixelFormat)pf);
    // Metal makes a buffer texture only at an offset of its buffer that is a multiple of the device's alignment for the format (16 bytes on an M4 Pro), and the GPU
    // addresses a view made below it from the boundary below its offset (measured on an M4 Pro: R32Float views at offsets 4, 8 and 12 read elements 0, 0 and 0 instead of 1, 2 and 3,
    // shared and private buffers, bound directly or in an argument buffer; R32 image writes at offset 8 landed two elements early, Assassin's Creed II, GitHub PR #1399). Unreal puts
    // buffer textures at 4-, 8- and 12-byte offsets in its pools (Octopath Traveler 0: offset 0x168; FINAL FANTASY RESONANCE's particle data). So the view starts at that boundary,
    // wider by the elements in between (tbuf_lead), and every shader access moves its coordinates by them, from the shader's offset table (the translator's
    // TranslationFlags.MetalBufferImageBias): an image slot, a texture slot, and each element of an array of buffer textures. An access with no table entry (past the table's
    // DMSH_TBIAS_SLOTS) reads an aligned copy (tbuf_shadow). Below the element size the view stays a dummy
    NSUInteger lead = (NSUInteger)(mo % al);
    if (lead % (NSUInteger)bpb) { RED_ONCE(38, "buffer texture offset %#llx not aligned to its %d-byte elements", (unsigned long long)mo, bpb); return nil; }
    NSUInteger w = t->d.width + lead / (NSUInteger)bpb, row = (w * (NSUInteger)bpb + al - 1) / al * al;
    if (mo - lead + row > b.length) { RED_ONCE(166, "buffer texture at pool offset %#llx, %lu elements of format %#x, ends too near its pool buffer's end for a view from the %lu-byte boundary below: the slot reads 0",
                                              (unsigned long long)mo, (unsigned long)t->d.width, t->d.format, (unsigned long)al); return nil; }
    if (lead) { static int once; if (!once++) MLOG("buffer texture at pool offset %#llx, below Metal's %lu-byte alignment: viewed from %#llx, its coordinates moved by %lu elements (first case)", (unsigned long long)mo, (unsigned long)al, (unsigned long long)(mo - lead), (unsigned long)(lead / (NSUInteger)bpb)); }
    MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:(MTLPixelFormat)pf width:w resourceOptions:b.resourceOptions
                                                                                     usage:tbuf_usage((MTLPixelFormat)pf)];
    id<MTLTexture> mt = [b newTextureWithDescriptor:d offset:mo - lead bytesPerRow:row];
    if (!mt) RED_ONCE(39, "cannot create buffer texture: %u elements format %#x", t->d.width, t->d.format);
    return mt;
}
static void *tex_mtl_resolve(const tex_t *ct, int *in_storage, id<MTLTexture> __strong *owned) {
    tex_t *t = (tex_t *)ct;
    if (!t || !t->serial) return NULL;
    if (on_sub_thread()) cpu_unc_drop();
    // Off the submit thread nothing is uploaded or synced: the upload batch is the submit thread's (up_owner_check). Whether a texture is taken from its storage is decided here, on
    // what this one look finds; another thread can change it at any time (the submit thread marks it CPU-written), so no caller's earlier look can decide for it. Tears of the
    // Kingdom's bake::ModuleMgr reads textures with ReadTexels on its own thread: mtl_read_texels found one not taken from its storage, the submit thread then marked it CPU-written,
    // and here it took the packaged path and uploaded on the reader's thread, which aborted (GitHub issue #1814)
    int off = atomic_load(&g_sub_thread_set) && !on_sub_thread();
    if (t->magic != MAGIC('T', 'E', 'X', 0)) { RED_ONCE(70, "texture object no longer valid (magic %#x, first case ser=%llu): no MTLTexture created", t->magic, (unsigned long long)t->serial); return NULL; }
    // t->pool is the game's object, named by a copy of the texture that an ID or a record keeps past its life: nvnMemoryPoolFinalize clears the magic, and the game then frees the memory, which
    // then reads as anything. Everything below reads it: garbage in its vp sent an upload through a page table that was never there (nvn_sparse_protect, FINAL FANTASY RESONANCE, GitHub issue #934)
    if (t->pool && t->pool->magic != MAGIC('P', 'O', 'O', 'L')) {
        id<MTLTexture> made = tex_made(t);
        static uint64_t n; if (n++ < 4) MLOG("texture of a memory pool that is gone ser=%llu %ux%u format %#x: %s (%llu so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format,
                                             made ? "keeps its host texture, not refreshed" : "no host texture made", (unsigned long long)n);
        return tex_mtl_result(made, owned);
    }
    int packaged = tex_from_storage(t);
    {   // Diagnostic DM_NVN_TEXINFO=WxH: log storage and description of textures of that size on first resolution (to tell whether two textures share storage and why they were not merged)
        static int ti = -1; static uint32_t tw, th; static uint64_t last[64]; static int nl;
        if (ti > 0 && t->d.width == tw && t->d.height == th && g_texinfo_n < 8) { int dup = 0; for (int i = 0; i < g_texinfo_n; i++) dup |= g_texinfo_ser[i] == t->serial; if (!dup && !(t->d.flags & 8)) g_texinfo_ser[g_texinfo_n++] = t->serial; }   // remember the non-render-target ones (no COMPRESSIBLE) for DM_NVN_TEXRAW=info
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && t->d.width == tw && t->d.height == th) {
            int seen = 0; for (int i = 0; i < nl; i++) if (last[i] == t->serial) seen = 1;
            static uint64_t n; if (!seen && (n++ < 40 || n % 600 == 0)) { if (nl < 64) last[nl++] = t->serial; else last[n % 64] = t->serial;
                uint64_t nz = 0; if (t->mem && t->L.size <= (64u << 20)) { uint8_t *m = t->mem, *part = NULL;   // non-zero bytes in guest memory: content the CPU wrote (a sparse pool's texture through its page table)
                    if (t->pool && nvn_sparse_pool(t->pool) && (part = malloc(t->L.size))) { nvn_sparse_copy(t->pool, part, (uint64_t)(uintptr_t)t->mem, t->L.size); m = part; }
                    for (uint64_t i = 0; i < t->L.size; i++) nz += m[i] != 0;
                    free(part); }
                MLOG("TEXINFO ser=%llu swizzled=%d has_layout=%d mem=%p pool=%p(%#x) off=%#llx target=%d format=%#x flags=%#x samples=%d %ux%ux%u levels=%u stride=%u pkg_layout=%#x storage %llu bytes non-zero %llu",
                     (unsigned long long)t->serial, packaged, t->has_layout, t->mem, t->pool, t->pool ? t->pool->flags : 0, (unsigned long long)t->off, t->d.target, t->d.format, t->d.flags, t->d.samples,
                     t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.stride, t->d.pkg_layout, (unsigned long long)t->L.size, (unsigned long long)nz); }
        }
    }
    id have = tex_get(t->serial);
    if (!packaged && !have && DM_GAME_TEX_DIRTY_UPLOAD && t->mem && on_sub_thread()) { int nvn_ft_written(const tex_t *); packaged = nvn_ft_written(t); }   // first use of a texture the CPU wrote since init without a flush (hle_nvn.c)
    if (!packaged && !have && on_sub_thread() && tex_cpu_content(t)) { mtl_tex_cpu_written(t); packaged = 1; }   // storage the CPU wrote before its first use (tex_cpu_content)
    if (!packaged && !have && on_sub_thread() && linear_storage_ok(t)) {   // a linear texture no view on its buffer can take (a row pitch off the device's alignment: DM_TBUF_ALIGN=64 and Axiom Verge's 32-byte rows of 8x8 RGBA8): uploaded, and again after CPU writes
        uint32_t pf; void *mb; uint64_t mo; size_t row;
        if (linear_fit(t, &pf, &mb, &mo, &row) < 0) {
            if (linear_gpu_may_write(t)) cpu_unc_take(t);   // the GPU's first write into its storage makes it a GPU texture (cpu_unc_drop)
            mtl_tex_cpu_written(t); packaged = 1;
        }
    }
    if (packaged) {
        NvPkg *pk = [have isKindOfClass:[NvPkg class]] ? have : nil;   // resolved as GPU-only before the CPU wrote it: dropped and rebuilt as packaged
        // Off the submit thread a reader takes the storage, which holds the content (and what the GPU wrote into it, gsb_store): uploading or rebuilding it here is the submit
        // thread's alone, and a texture uploaded already may lack the CPU's writes since
        if (off && in_storage) { *in_storage = 1; return NULL; }
        if (!pk && !have && !(t->pool && nvn_sparse_pool(t->pool)) && (pk = pkg_shared(t))) tex_put(t->serial, pk);   // another object over the same bytes (NvPkgRef)
        // Any other thread (the present thread) uses an uploaded one as it is: a present shows the content as of when the game queued it, which present_enqueue resolved there. Its
        // pages and mappings are not looked at here, so the submit thread's next look still finds a write since (pk->seen stays)
        if (off) {
            if (pk) return tex_mtl_result(pk->ps ? pk->tex : pk->view ? pk->view : pk->tex, owned);
            RED_ONCE(168, "texture ser=%llu taken from its storage resolved first off the submit thread: no host texture until the submit thread uploads it", (unsigned long long)t->serial);
            return NULL;
        }
        if (pk && pk->ps) { sparse_look(pk->ps, t); return tex_mtl_result(pk->tex, owned); }   // a placement sparse texture: its tiles follow the game's mappings
        if (!pk) { id<MTLTexture> made = nil; NvSparse *ps = sparse_make(t, 0, &made);
            if (ps) { pk = [NvPkg new]; pk->ps = ps; pk->tex = made; pk->sparse = 1; sparse_sync(ps, t, 1); tex_put(t->serial, pk); return tex_mtl_result(pk->tex, owned); } }
        if (pk) {   // Already uploaded: check the covered pages only if there has been a guest write since the last check
            if (!g_pool_walk) pk->used_tu = g_tu_now;   // a walk's lookup is not a use (pkg_evict_idle)
            if (gsb_held(t->mem, t->L.size)) return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);   // its storage lacks a GPU write this command buffer stores through any texture over those bytes (gsb_store): uploaded at its next use if the CPU wrote it
            uint64_t now = nvn_dirty_now(), own = nvn_dirty_own_writes();
            int tracked = pk->span != NULL;   // kept from the upload: a texture's pool stays registered while the texture lives
            // Looked at already in this submission: it keeps that look's texture. The queue's thread replays a submission after the game queued it, so the writes the game made
            // before are all in the first look, and a write another thread makes during the replay races the GPU's run of that submission on the console: every draw of it reading
            // the memory as of one moment after the submit is one of its outcomes. The next submission's first look finds the write (seen stays, and g_pkg_kept sends every cache
            // of the write generation through its check again, mtl_sub_begin), so nothing stays stale once the writer is done. Looking at each use fed on itself: a look that
            // found a few pages written took a texture and uploaded, the writer wrote a few more meanwhile, and the next draw's look found those (Ocarina of Time, 22 uploads of
            // one texture in one submission, docs/reference/frame-rate.md). A write by this thread is in order with the commands it replays and is looked for (own); outside a
            // submission (a present) every look reads the pages
            int written = !tracked || (now != pk->seen && nvn_dirty_since_span(pk->span, t->mem, t->L.size, pk->stamp));
            int kept = written && tracked && !pk->sparse && atomic_load_explicit(&g_sub_open, memory_order_relaxed) && pk->sub == g_sub_serial && pk->own == own;
            int clean = !written || kept;
            // Only the pages it shares with other data, its bytes there as uploaded: clean, and no write generation started for a neighbour's writes (pkg_edges_clean protects
            // the storage again, so not while a page inside it is being written)
            if (kept && (pk->edge_n[0] || pk->edge_n[1]) && !pkg_inner_written(pk, t) && pkg_edges_clean(pk, t)) return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
            if (kept) { now = pk->seen; g_pkg_kept = 1; } else if (!pk->sparse) { pk->sub = g_sub_serial; pk->own = own; }
            // Checked already in this submission: seen and vseen stay those of that check, so a write or a mapping another thread makes during the replay is found at the next submission's
            if (pk->sparse && sparse_checked(pk)) { clean = 1; now = pk->seen; }
            else if (pk->sparse) { uint64_t g = nvn_sparse_gen(); clean = (now == pk->seen && g == pk->vseen) || !nvn_sparse_since(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size, pk->vgen, pk->stamp); pk->vseen = g; pk->sub = g_sub_serial; pk->own = own; }
            pk->seen = now;
            // Images copied from a predecessor are only as good as its upload, and the report of a dropped batch comes after later uploads were encoded
            int whole = pk->part && pk->lost_gen != atomic_load(&g_up_lost_gen);
            if (whole && clean) { clean = 0; static int nl; if (nl++ < 8) MLOG("re-uploading a texture holding images copied from one uploaded before a batch was dropped ser=%llu", (unsigned long long)t->serial); }
            if (!clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) whole = 1;
            if (clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) { clean = 0; whole = 1; uint64_t k = atomic_load(&g_up_lost_redo); if (k <= 3 || !atomic_load(&g_up_lost_n)) MLOG("re-uploading a texture whose upload was dropped ser=%llu (%llu in total, %d left in the record)", (unsigned long long)t->serial, (unsigned long long)k, atomic_load(&g_up_lost_n)); }
            if (clean) {
                if (dirtycheck() && !kept && !pk->sparse && !pk->stored && full_hash(t->mem, t->L.size) != pk->hash) { g_pkg_miss++; pkg_miss_report(pk, t, now); }
                return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
            }
            if (!whole && !pk->sparse && pkg_edges_clean(pk, t)) return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);   // only the pages it shares with other data were written
            // Pool content changed (streamed assets written into the same storage, a movie frame): switch to another texture, as in-flight draws still read the old one. That is the spare
            // when the GPU is done with it (see NvPkg), else a new one; the old one becomes the spare (pkg_upload_ made nt resident)
            id<MTLTexture> reuse = nil;
            if (pk->spare) { pthread_mutex_lock(&g_mu); uint64_t done = g_done; pthread_mutex_unlock(&g_mu); if (pk->spare_fence <= done && pk->spare.usage == pk->tex.usage) reuse = pk->spare; }
            if (reuse && !own_object_free(reuse)) reuse = nil;   // serials that took it over (cpu_unc_drop) still draw it
            uint64_t st, vg = nvn_sparse_gen(), b0 = g_bp_up_bytes, lg = atomic_load(&g_up_lost_gen), t0 = bp_now(); unsigned taken = 0;
            id<MTLTexture> nt = pkg_upload(t, &st, reuse, pk->tex.usage, whole ? nil : pk->tex, pk->span, pk->stamp, &taken);
            reup_note(pk, t, taken, g_bp_up_bytes - b0, bp_now() - t0);
            if (nt) { id<MTLTexture> old = pk->tex; pk->tex = nt; pk->stored = 0; pk->part = taken != 0; if (!taken) pk->lost_gen = lg;
                if (pk->spare && pk->spare != nt) defer_obj_release(pk->spare);   // still in use, or of another shape: retired as before
                if (pk->sparse) sparse_view(pk, t);
                zb_report(t, pk->sparse ? (unsigned)pk->vbase : 0, "re-upload");
                pk->spare = old; pk->spare_fence = g_fence + 1; spare_note(pk); own_note(t->serial); pk->stamp = st; pk->vgen = pk->vseen = vg; pk->span = nvn_dirty_span(t->mem, t->L.size); pk->seen = st; g_pkg_reuploads++; pkg_hash_take(pk, t); pkg_edges_take(pk, t); }
            if (nt && !pk->sparse) bcp_up_follow(t, nt);   // the CPU's bytes reach the other side of a compressed-alias pair too
            return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
        }
        pk = [NvPkg new];
        pk->sparse = t->pool && nvn_sparse_pool(t->pool); pk->vgen = pk->vseen = nvn_sparse_gen(); pk->used_tu = g_tu_now; pk->mem = (uintptr_t)t->mem; pk->size = t->L.size;
        pk->lost_gen = atomic_load(&g_up_lost_gen);
        pk->sub = g_sub_serial; pk->own = nvn_dirty_own_writes();
        pk->tex = pkg_upload(t, &pk->stamp, nil, 0, nil, NULL, 0, NULL);
        if (!pk->tex) return NULL;
        pk->span = nvn_dirty_span(t->mem, t->L.size);
        pk->seen = pk->stamp; g_pkg_uploads++; pkg_hash_take(pk, t);   // "nothing written since" starts at the stamp: a write that faulted during the upload moved "now" past it
        pkg_edges_take(pk, t);
        if (pk->sparse) sparse_view(pk, t);
        zb_report(t, pk->sparse ? (unsigned)pk->vbase : 0, "upload");
        tex_put(t->serial, pk);
        if (!pk->sparse) { pkg_share_put(t, pk); bcp_up_follow(t, pk->tex); }   // the CPU's bytes reach the other side of a compressed-alias pair too
        return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
    }
    // An sRGB / linear view whose parent left the sharing table (upgraded, or evicted with its pool; a takeover leaves a held view's parent) no longer shares the storage's bytes: dropped,
    // live or finalized (its ghost entry), and made again below on what holds them now. serial_del took only a live entry, so a finalized serial kept resolving to the stale view
    // whenever making the new texture failed, and to any thread in between
    if (obj_is_tex(have) && ((id<MTLTexture>)have).parentTexture && own_view_drop_stale(t->serial, have)) {
        static int nstale; if (nstale++ < 4) MLOG("stale sRGB / linear view dropped ser=%llu format %lu (%d so far)", (unsigned long long)t->serial, (unsigned long)((id<MTLTexture>)have).pixelFormat, nstale);
        have = nil;
    }
    if (!have && t->d.target == NVN_TEXTURE_TARGET_BUFFER) { alias_key key = tex_alias_key(t); have = own_alias_take(t->serial, &key); }
    if (have && !off && t->d.target == NVN_TEXTURE_TARGET_BUFFER && t->pool && nvn_sparse_pool(t->pool)) {
        // treg_sparse_notes invalidates the registered views, but a BUFFER's
        // serial and sharing entry still name its previous physical mapping.
        // Refresh only when that mapping changed; queued consumers keep the old view.
        void *mb = NULL; uint64_t mo = 0;
        int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (!mtl_addr((uint64_t)(uintptr_t)t->mem, (uint64_t)t->d.width * (uint64_t)bpb, &mb, &mo)) return NULL;
        id<MTLTexture> root = have; while (root.parentTexture) root = root.parentTexture;
        NSUInteger al = tbuf_view_align(root.pixelFormat);
        if (root.buffer != (__bridge id<MTLBuffer>)mb || root.bufferOffset != mo - mo % al) {
            id<MTLTexture> fresh = tex_buffer(t); if (!fresh) return NULL;
            res_add(fresh);
            own_upgrade(have, fresh, t->serial, fresh); have = fresh;
        }
    }
    if (have) { if (!off && t->pool && nvn_sparse_pool(t->pool)) sparse_gpu_look(t, have); if (!g_pool_walk) tu_stamp(have, 0); return tex_mtl_result(have, owned); }   // its mappings are synced on the submit thread (sparse_sync)
    alias_key ak = tex_alias_key(t);
    id shared = own_alias_take(t->serial, &ak);   // found and published in one hold (nvn_owner.m)
    if (shared) { if (!g_pool_walk) tu_stamp(shared, 0); return tex_mtl_result(shared, owned); }
    // A buffer texture's view is shared by storage as every other texture is: NVN texture objects over the same range with the same description are one view, kept by the sharing
    // table. Made per object, Unreal's transient buffer textures, built again every frame over the same ranges, added a view to the residency set each time: FINAL FANTASY
    // RESONANCE added and removed about 79,000 every 300 frames, and their finalized objects (about 175 a frame) wrapped the
    // ghost table (GitHub issues #1061 and #2423). It has no sRGB / linear twin and nothing to fill
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) {
        id<MTLTexture> mt = tex_buffer(t);
        if (!mt) return NULL;
        res_add(mt);
        id<MTLTexture> got = own_alias_publish(t->serial, &ak, mt);
        if (got != mt) defer_obj_release(mt);   // another thread registered one over the same storage meanwhile: that one is shared
        return tex_mtl_result(got, owned);
    }
    {   // NVN textures over the same storage share its bytes. One that differs from a shared texture only by the other format of an sRGB / linear pair is a view of it: as its own Metal
        // texture, Mario Kart World's Grand Prix movie (an RGBA8 render target the YUV pass writes, sampled by the composite through an RGBA8_SRGB texture) showed a frozen frame
        // The other formats of the pair are looked up directly (srgb_twins), with the rest of the key unchanged; the descriptor is built only when one is shared
        const uint8_t *tw = srgb_twins(ak.d.format); id<MTLTexture> v = nil;
        for (int j = 0; j < 4 && tw[j] && !v; j++) {
            alias_key k; memcpy(&k, &ak, sizeof k); k.d.format = tw[j];   // memcpy keeps the zeroed padding the table hashes
            id<MTLTexture> b = own_alias_peek(&k);   // borrowed: a texture the table lets go meanwhile is released only after the submission in flight
            if (!b) continue;
            MTLTextureDescriptor *d = tex_desc(t, 0);
            if (!d || d.sampleCount > 1 || (d.textureType != MTLTextureType2D && d.textureType != MTLTextureType2DArray)) break;
            if (b.pixelFormat == d.pixelFormat || tex_need_pfv(b.pixelFormat, d.pixelFormat) || b.textureType != d.textureType) continue;
            v = [b newTextureViewWithPixelFormat:d.pixelFormat textureType:b.textureType levels:NSMakeRange(0, b.mipmapLevelCount) slices:NSMakeRange(0, b.arrayLength) swizzle:b.swizzle];
            if (!v) continue;
            // The entry counts b as held from here on (count_add), so no takeover evicts it under the twin. b may have left the sharing table on another thread since the lookup
            // above (a pool's finalize on the game's thread, an upgrade): then the view is of a texture that no longer shares the storage's bytes, so it is never published (own_view_put
            // checks that k still names b, and marks k looked up, in the same hold of g_smu as the entry), and the next format of the pair or a texture of the twin's own is tried
            if (!own_view_put(t->serial, &k, v)) { v = nil; continue; }
        }
        if (v) { if (!g_pool_walk) tu_stamp(v, 0); return tex_mtl_result(v, owned); }
    }
    // A texture no thread has resolved, read off the submit thread: its storage holds its content (the GPU has written none, or it would be resolved), as the console reads it.
    // Made here, its host texture would be filled from neither (tex_init_fill and tex_zero_fill are the submit thread's), and it would keep it: its first use on the submit thread,
    // which decides whether it is taken from its storage (tex_cpu_content), would never come
    if (off && in_storage && tex_storage_readable(t)) { *in_storage = 1; return NULL; }
    id<MTLTexture> mt = tex_linear(t); int tu = 0;
    if (!mt && t->pool && nvn_sparse_pool(t->pool)) mt = sparse_gpu_make(t);   // a large one in a sparse pool: only the tiles the game maps take memory
    if (!mt) {
        MTLTextureDescriptor *d = tex_desc(t, 0);
        if (!d) return NULL;
        tu = (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && rh_fits(t, d.usage);   // a render target another one may draw over (tu_pass)
        mt = pair_texture(t, d);   // (neither filled: their storage is shared)
        if (!mt) mt = image_texture(t, d);
        if (!mt) { mt = rh_texture(t, d); if (!mt) mt = [g_dev newTextureWithDescriptor:d];
            if (mt) { res_add(mt); if (!bcp_up_fill(t, mt) && !tex_init_fill(t, mt)) tex_zero_fill(mt); } }   // a side of a compressed-alias pair takes the other side's bytes (bcp_up_fill)
        if (!mt) { RED_ONCE(2, "failed to create MTLTexture %ux%u format %#x target %d", t->d.width, t->d.height, t->d.format, t->d.target); return NULL; }
    }
    if (mt.allocatedSize >= (256u << 20)) { static int nb; if (nb++ < 20)   // the textures that take most of the GPU's memory (a GPU out-of-memory error says nothing of which)
        MLOG("large texture ser=%llu %ux%ux%u format %#x target %d levels %u: %llu MB%s", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels,
             (unsigned long long)(mt.allocatedSize >> 20), t->pool && nvn_sparse_pool(t->pool) ? ", in a sparse virtual pool" : t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) ? ", in a virtual pool" : ""); }
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("new texture (%s) #%d ser=%llu %ux%ux%u format %#x target %d levels %u storage %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    if (getenv("DM_NVN_TEXCPU") && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) MLOG("non-swizzled texture on a CPU-visible pool ser=%llu %ux%ux%u format %#x target %d levels %u texture flags %#x pool flags %#x storage %p", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->d.flags, t->pool->flags, (void *)t->mem);
    res_add(mt);
    // The takeover of the storage, the entry and the serial's publication in one hold (nvn_owner.m). A texture another thread registered under the key since the lookup above is the
    // one the storage's serials share: this one is retired
    id<MTLTexture> got = own_alias_publish(t->serial, &ak, mt);
    if (got != mt) { static int nrace; if (nrace++ < 4) MLOG("texture ser=%llu made twice over one storage: the first one registered is shared (%d so far)", (unsigned long long)t->serial, nrace); defer_obj_release(mt); }
    else if (tu) tu_register(mt, t->serial);
    return tex_mtl_result(got, owned);
}
// Memory readout line: resident memory, GPU allocated, deferred-release queue, residency set object count, registered memory pools / peak / tracking table capacity, cumulative texture re-uploads.
// Purpose: pin down "crashes with no log". The system gives no notice before killing a process for memory, so only readouts left beforehand can show which number kept climbing.
// The frame hook samples every 300 frames. For the first 400 reports, a change of at least 64 MB in resident or GPU allocated memory also triggers a report. After 400 reports, log every 3000 frames.
// All integers (the frame hook runs on the guest thread's small stack, so no floating-point formatting).
static _Atomic uint64_t g_gptr_bindings, g_gptr_reads, g_gptr_added, g_gptr_bytes;   // gmem_discover: bindings with sources, readable words, newly addressed logical pool/run ranges and bytes
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) {
    static unsigned long long last_rss, last_gpu, last_frame, last_wired; static int lines; unsigned long long gpu = (unsigned long long)(g_dev.currentAllocatedSize >> 20);
    sys_mem_t sm = sys_mem();
    unsigned long long dr = rss_mb > last_rss ? rss_mb - last_rss : last_rss - rss_mb, dg = gpu > last_gpu ? gpu - last_gpu : last_gpu - gpu, dw = sm.wired > last_wired ? sm.wired - last_wired : last_wired - sm.wired;
    // Also when the system's wired memory moved by 256 MB or a memory pressure event came (not on low free memory alone: the Mac keeps little free, which would print every time)
    static uint64_t last_ev; uint64_t ev = atomic_load(&g_mp_n[0]) + atomic_load(&g_mp_n[1]) + atomic_load(&g_mp_n[2]);
    int sparse = lines >= 400;
    if (lines && frame - last_frame < 3000 && (sparse || (dr < 64 && dg < 64 && dw < 256 && ev == last_ev))) return;
    if (!sparse) lines++;
    last_rss = rss_mb; last_gpu = gpu; last_frame = frame; last_wired = sm.wired; last_ev = ev;
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    pthread_mutex_lock(&g_res_mu); unsigned long nr = (unsigned long)g_res_objects.count; pthread_mutex_unlock(&g_res_mu);
    int live = 0, peak = 0, cap = 0; nvn_dirty_stat(&live, &peak, &cap);
    pthread_mutex_lock(&g_smu); own_stats os = own_stats_locked(); own_alias_stats as = own_alias_stats_locked(); unsigned ns = os.live, ashared = as.n, nghost = os.ghost, nwait = os.retire_wait; uint64_t ghost_puts = os.ghost_puts;
    own_alias_unheld au = own_alias_unheld_locked();   // shared textures nothing but the sharing table keeps: kept until their storage is taken over or their pool goes
    pthread_mutex_unlock(&g_smu);
    unsigned long dead_n = 0; unsigned long long dead_mb = 0;
    // Objects read from a weak table come back retained + autoreleased: the frame hook runs on a guest thread with no autorelease pool that drains, so without this wrapper every buffer counted would be pinned for good (the readout creating the leak itself)
    int pres = 0; @autoreleasepool { pthread_mutex_lock(&g_mu); pres = g_pool_res_n; for (id<MTLBuffer> b in g_dead_pools) { dead_n++; dead_mb += b.length; } pthread_mutex_unlock(&g_mu); } dead_mb >>= 20;   // host objects held by serial (textures / packaged textures / programs): if it climbs along with GPU allocated, textures are not being released
    // Headroom to the system memory limit: os_proc_available_memory (how far the process is from being terminated by the system for memory) has no answer on macOS, so the readout writes "none"
    os_unfair_lock_lock(&g_rh_lk); unsigned rh_n = g_rh_n; uint64_t rh_bytes = g_rh_bytes; os_unfair_lock_unlock(&g_rh_lk);
    char avail[32] = "none";
    MLOG("memory up to frame %llu: resident %llu MB, GPU allocated %llu MB, deferred-release queue %lu, residency set %lu, serial objects %u, memory pools %d (resident %d; peak %d / tracking table capacity %d; freed but alive %lu, %llu MB), shared textures %u (%u kept by the sharing table alone, %llu MB, %llu MB of it drawn into or shader-written, the oldest idle %llu submissions; evicted with pools %llu (serial entries that went with them %llu), on storage takeover %llu in total), texture re-uploads %llu in total, per-frame arena blocks created %llu in total (alive %llu, %llu MB; released when idle %llu, %llu MB in total), finalized textures still referenced %u (small table, %llu put in total), retired but held by a serial %u, recycled texture heaps made %llu, reused %llu, freed %llu, waiting %u (%llu MB); headroom to the system memory limit %s", frame, rss_mb, gpu, nd, nr, ns, live, pres, peak, cap, dead_n, dead_mb, ashared, au.entries, (unsigned long long)(au.bytes >> 20), (unsigned long long)(au.writable_bytes >> 20), (unsigned long long)au.oldest_idle, (unsigned long long)as.evicted, (unsigned long long)as.pool_entries, (unsigned long long)as.taken, (unsigned long long)g_pkg_reuploads, (unsigned long long)g_arena_made, (unsigned long long)g_arena_live, (unsigned long long)(g_arena_live_bytes >> 20), (unsigned long long)g_arena_freed, (unsigned long long)(g_arena_freed_bytes >> 20), nghost, (unsigned long long)ghost_puts, nwait,
         (unsigned long long)g_rh_made, (unsigned long long)g_rh_reused, (unsigned long long)g_rh_trimmed, rh_n, (unsigned long long)(rh_bytes >> 20), avail);
    if (sm.ok) { char pl[220]; mem_pressure_said(pl, sizeof pl);
        MLOG("system memory at frame %llu: free %llu MB, wired %llu MB, compressed %llu MB, file-backed %llu MB, purgeable %llu MB, of %llu MB; residency set %llu MB; %s", frame,
             (unsigned long long)sm.free, (unsigned long long)sm.wired, (unsigned long long)sm.compressed, (unsigned long long)sm.file, (unsigned long long)sm.purgeable, (unsigned long long)sm.total,
             (unsigned long long)(g_res ? g_res.allocatedSize >> 20 : 0), pl); }
    { void mtl_vc_stats(unsigned *, uint64_t *, uint64_t *), mtl_tb2_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *); uint64_t mtl_tex_mis_count(void); unsigned vn; uint64_t vh, vm, tb, tm, tf, tc, tcb;
      mtl_vc_stats(&vn, &vh, &vm); mtl_tb2_stats(&tb, &tm, &tf, &tc, &tcb);
      MLOG("texture views by Metal texture (vc_get): %u textures hold cached views; %llu views reused, %llu made in total; texture buffers bound as 2D rows for wrapped 1D fetches %llu times, %llu made, %llu failed, %llu copied into aligned rows (%llu MB) "
           "(tbuf_as_2d); texture type mismatches no view bridges %llu in total (tex_mis_note)",
           vn, (unsigned long long)vh, (unsigned long long)vm, (unsigned long long)tb, (unsigned long long)tm, (unsigned long long)tf, (unsigned long long)tc, (unsigned long long)(tcb >> 20), (unsigned long long)mtl_tex_mis_count()); }
    if (g_allocprof > 0) { char ab[256]; int o = 0; for (int i = 0; i < g_alloc_n && o < 200; i++) o += snprintf(ab + o, sizeof ab - o, " #%d %llu MB", i, (unsigned long long)(g_alloc4[i].allocatedSize >> 20));
        for (int i = 0; i < 3 && o < 240; i++) o += snprintf(ab + o, sizeof ab - o, " up%d %llu pres%d %llu", i, (unsigned long long)(g_up_alloc4[i].allocatedSize >> 20), i, (unsigned long long)(g_present_alloc4[i].allocatedSize >> 20));
        MLOG("command allocator usage:%s", ab);
        if (g_allocprof == 2 && g_cm_sub_n) { int top[6] = {-1, -1, -1, -1, -1, -1}; char cl[400]; int co = 0;
            for (int op = 0; op < 64; op++) for (int k = 0; k < 6; k++) if (top[k] < 0 || g_cm_op[op] > g_cm_op[top[k]]) { for (int j = 5; j > k; j--) top[j] = top[j - 1]; top[k] = op; break; }
            for (int k = 0; k < 6 && co < 380; k++) if (top[k] >= 0 && g_cm_op[top[k]]) co += snprintf(cl + co, sizeof cl - co, "%sop%d %llu MB over %llu ops", k ? ", " : "", top[k], (unsigned long long)(g_cm_op[top[k]] >> 20), (unsigned long long)g_cm_opn[top[k]]);
            MLOG("command memory (DM_NVN_ALLOCPROF=2, a fresh allocator per submission): %llu submissions, %llu KB on average, %llu KB at most; by operation %s",
                 (unsigned long long)g_cm_sub_n, (unsigned long long)(g_cm_sub_sum / g_cm_sub_n >> 10), (unsigned long long)(g_cm_sub_max >> 10), cl);
            MLOG("command memory by submission: under 4 MB %llu, 4-8 %llu, 8-16 %llu, 16-32 %llu, 32-64 %llu, 64-128 %llu, 128 MB or more %llu; the largest (%llu KB) began %llu encoders and %llu Metal draws",
                 (unsigned long long)g_cm_hist[0], (unsigned long long)g_cm_hist[1], (unsigned long long)g_cm_hist[2], (unsigned long long)g_cm_hist[3], (unsigned long long)g_cm_hist[4], (unsigned long long)g_cm_hist[5], (unsigned long long)g_cm_hist[6],
                 (unsigned long long)(g_cm_big_z >> 10), (unsigned long long)g_cm_big_enc, (unsigned long long)g_cm_big_draws);
            MLOG("command memory by encoder: making %llu render encoders %llu MB, inside them %llu MB; making %llu compute encoders %llu MB, inside them %llu MB; between encoders %llu MB (chunks of about 2 MB are charged to the encoder that crosses into one)",
                 (unsigned long long)g_cm_inn[2], (unsigned long long)(g_cm_in[2] >> 20), (unsigned long long)(g_cm_in[0] >> 20), (unsigned long long)g_cm_inn[3], (unsigned long long)(g_cm_in[3] >> 20),
                 (unsigned long long)(g_cm_in[1] >> 20), (unsigned long long)(g_cm_out >> 20));
            MLOG("indirect-count multi-draws: masked %llu calls encoding %llu draws (every record up to the call's maximum), counted %llu calls encoding %llu draws",
                 (unsigned long long)g_md_masked_calls, (unsigned long long)g_md_masked, (unsigned long long)g_md_counted_calls, (unsigned long long)g_md_counted); }
        MLOG("GPU allocation growth by function: pass end %llu MB, pass begin %llu MB, pipeline fetch %llu MB, argument buffers %llu MB, texture fetch %llu MB", g_ap_fn[0] >> 20, g_ap_fn[1] >> 20, g_ap_fn[2] >> 20, g_ap_fn[3] >> 20, g_ap_fn[4] >> 20); }
    if (g_allocprof > 0) { int top[4] = {-1, -1, -1, -1}; for (int o = 0; o < 64; o++) for (int k = 0; k < 4; k++) if (top[k] < 0 || g_ap_bytes[o] > g_ap_bytes[top[k]]) { for (int j = 3; j > k; j--) top[j] = top[j - 1]; top[k] = o; break; }
        MLOG("GPU allocation growth by op: op%d %llu MB, op%d %llu MB, op%d %llu MB, op%d %llu MB", top[0], g_ap_bytes[top[0]] >> 20, top[1], g_ap_bytes[top[1]] >> 20, top[2], g_ap_bytes[top[2]] >> 20, top[3], g_ap_bytes[top[3]] >> 20); }
    { void mtl_gmem_words(uint64_t *, uint64_t *, uint64_t *); uint64_t gw, go, gb; mtl_gmem_words(&gw, &go, &gb);
      if (gw) MLOG("raw pool table words at commit: %llu, of them %llu held an older table than the current one and %llu the current one behind the pools", (unsigned long long)gw, (unsigned long long)go, (unsigned long long)gb); }
    if (g_gptr_bindings) MLOG("raw pointer discovery up to frame %llu: %llu bindings, %llu pointer words read, %llu previously unaddressed ranges (%llu logical MB)", frame,
                            (unsigned long long)g_gptr_bindings, (unsigned long long)g_gptr_reads, (unsigned long long)g_gptr_added, (unsigned long long)(g_gptr_bytes >> 20));
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
void mtl_spare_stats(uint64_t *held, uint64_t *trimmed, uint64_t *trimmed_mb) { *held = atomic_load(&g_spare_held); *trimmed = atomic_load(&g_spare_trimmed); *trimmed_mb = atomic_load(&g_spare_trimmed_bytes) >> 20; }
uint64_t mtl_pkg_edge_skips(void) { return g_pkg_edge_skips; }
uint64_t mtl_pkg_miss(void) { return g_pkg_miss; }
static void treg_retire_serial(uint64_t ser);
void mtl_tex_release(tex_t *t) {
    if (!t->serial) return;
    pthread_mutex_lock(&g_smu);
    int was_live = own_tex_end_locked(t);   // -1: a copy's Finalize, the texture's own object still holds it (nvn_owner.m)
    if (was_live < 0) { pthread_mutex_unlock(&g_smu); return; }
    free(nvn_hm_del(&g_image_desc, &t->serial));
    pthread_mutex_lock(&g_cpu_tex_mu);
    cpu_pool *cp = cpu_pool_get(t->pool, 0);
    if (cp) {
        void *pool = nvn_hm_del(&cp->live, &t->serial);
        if (pool) {
            g_cpu_tex_n--;
            if (t->pool && t->mem) nvn_hm_put(&cp->retired, &t->serial, pool);
        }
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
    pthread_mutex_unlock(&g_smu);
    serial_del(t->serial);
    // Only the first release of a serial retires its descriptors. NVNtexture is plain data: Breath of the Wild copies texture objects and finalizes each copy
    // (about 100 a frame), so the same serial came back here again and again while its descriptor list grew (5,900 IDs registered with one serial); each walk
    // found them all retired already, and the walks took about as much of the game thread as the game. Descriptors registered after the first release are
    // retired at registration (mtl_reg_tex), so there is nothing left for a later walk to do
    if (was_live) treg_retire_serial(t->serial);
}
