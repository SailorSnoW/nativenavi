// Texture storage: initial content, linear and CPU-written textures, storage the GPU writes, compressible aliases, buffer textures, and tex_mtl.
// New textures are zeroed (Switch memory reads 0, a new Metal texture is undefined) by copies from a zero buffer, depth included: a clear-only pass on a large depth texture
// fails with an out-of-memory error (docs/reference/memory.md, A clear pass on a large depth texture fails with `Insufficient Memory`).
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
// replaced by a bigger one when too small; the old one is released after in-flight submissions finish
static id<MTLBuffer> zero_buf(size_t need) {
    static id<MTLBuffer> zb;
    static size_t zn;
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
static void tex_zero_queued(id<MTLTexture> mt) {
    [g_up_texs addObject:mt];
    if (!g_cb) up_flush();
    if (!g_tex_zeroed++) MLOG("new-texture zeroing: on (new Metal textures are undefined; new Switch memory reads 0)");
}
static void tex_zero_fill(id<MTLTexture> mt) {
    static int warned;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread()) return;   // the upload batch belongs to the submit thread only
    MTLTextureType tt = mt.textureType;
    MTLPixelFormat pf = mt.pixelFormat;
    NSUInteger slices = mt.arrayLength * (tt == MTLTextureTypeCube || tt == MTLTextureTypeCubeArray ? 6 : 1);
    int depth = pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth32Float_Stencil8;
    int stencil = pf == MTLPixelFormatStencil8 || pf == MTLPixelFormatDepth32Float_Stencil8;
    int multisample = tt == MTLTextureType2DMultisample || tt == MTLTextureType2DMultisampleArray;
    if (multisample && !depth && !stencil && !(mt.usage & MTLTextureUsageRenderTarget)) return;   // no render pass can clear it
    size_t dbpp = multisample ? 0 : pf == MTLPixelFormatDepth16Unorm ? 2 : pf == MTLPixelFormatDepth32Float ? 4 : 0;
    if (dbpp) {   // a row is at most 64 KB, the zero buffer at least 4 MB, so it never grows for a depth texture
        id<MTLBuffer> zb = zero_buf((size_t)mt.width * dbpp);
        if (!zb) return;
        up_begin();
        for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
            NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), step = MAX((NSUInteger)1, MIN(h, zb.length / (w * dbpp)));
            for (NSUInteger y = 0; y < slices; y++) for (NSUInteger y0 = 0; y0 < h; y0 += step) {
                NSUInteger n = MIN(step, h - y0);
                up_blit_add_at(zb, 0, w * dbpp, w * dbpp * n, MTLSizeMake(w, n, 1), mt, y, l, MTLOriginMake(0, y0, 0));
            }
        }
        tex_zero_queued(mt);
        return;
    }
    if (depth || stencil || multisample) {
        up_begin();
        if (g_up_ce) { m4_end(g_up_ce); g_up_ce = nil; }
        for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) for (NSUInteger y = 0; y < slices; y++) {
            MTL4RenderPassDescriptor *pd = [MTL4RenderPassDescriptor new];
            if (!depth && !stencil) {
                pd.colorAttachments[0].texture = mt; pd.colorAttachments[0].level = l; pd.colorAttachments[0].slice = y;
                pd.colorAttachments[0].loadAction = MTLLoadActionClear; pd.colorAttachments[0].storeAction = MTLStoreActionStore; pd.colorAttachments[0].clearColor = MTLClearColorMake(0,0,0,0);
            }
            if (depth) {
                pd.depthAttachment.texture = mt; pd.depthAttachment.level = l; pd.depthAttachment.slice = y;
                pd.depthAttachment.loadAction = MTLLoadActionClear; pd.depthAttachment.storeAction = MTLStoreActionStore; pd.depthAttachment.clearDepth = 0;
            }
            if (stencil) {
                pd.stencilAttachment.texture = mt; pd.stencilAttachment.level = l; pd.stencilAttachment.slice = y;
                pd.stencilAttachment.loadAction = MTLLoadActionClear; pd.stencilAttachment.storeAction = MTLStoreActionStore; pd.stencilAttachment.clearStencil = 0;
            }
            id<MTL4RenderCommandEncoder> e = [g_up_cb renderCommandEncoderWithDescriptor:pd];
            [e barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()]; m4_end(e);
        }
        g_up_ce = [g_up_cb computeCommandEncoder];
        [g_up_ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
        [g_up_ce setArgumentTable:g_up_ct]; [g_up_texs addObject:mt];
        if (!g_cb) up_flush();
        static int ms;
        if (multisample && !depth && !stencil && !ms++) MLOG("new-texture zeroing: multisample colour textures are cleared in a render pass (first %lux%lu format %lu, %lu samples)", (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)pf, (unsigned long)mt.sampleCount);
        return;
    }
    if (tt == MTLTextureTypeTextureBuffer) return;
    size_t bpp = mtl_pf_bytes(mt.pixelFormat);
    if (!bpp) {
        if (!warned++) MLOG("new-texture zeroing: pixel format %lu not cleared (compressed / unsupported)", (unsigned long)mt.pixelFormat);
        return;
    }
    size_t need = (size_t)mt.width * bpp * mt.height * mt.depth;
    if (need > (64u << 20)) {
        static int big;
        if (!big++) MLOG("new-texture zeroing: %lux%lux%lu exceeds 64 MB, not cleared", (unsigned long)mt.width, (unsigned long)mt.height, (unsigned long)mt.depth);
        return;
    }
    // in pieces of rows, as depth above (docs/reference/memory.md, A full colour texture grew the shared zero buffer)
    id<MTLBuffer> zb = zero_buf((size_t)mt.width * bpp);
    if (!zb) return;
    up_begin();
    for (NSUInteger l = 0; l < mt.mipmapLevelCount; l++) {
        NSUInteger w = MAX(mt.width >> l, 1u), h = MAX(mt.height >> l, 1u), z = tt == MTLTextureType3D ? MAX(mt.depth >> l, 1u) : 1;
        NSUInteger row = w * bpp, rows = MIN(h, zb.length / row);
        NSUInteger depth = MAX((NSUInteger)1, MIN(z, zb.length / (row * h)));
        for (NSUInteger slice = 0; slice < slices; slice++)
            for (NSUInteger d = 0; d < z; d += depth)
                for (NSUInteger y = 0; y < h; y += rows) {
                    NSUInteger n = MIN(rows, h - y);
                    up_blit_add_at(zb, 0, row, row * n, MTLSizeMake(w, n, MIN(depth, z - d)), mt, slice, l, MTLOriginMake(0, y, d));
                }
    }
    tex_zero_queued(mt);
}

// A pool's storage at nvnMemoryPoolInitialize is its textures' content, also in a CPU_NO_ACCESS pool; flushed storage and CPU_UNCACHED storage at first use too
// (docs/reference/nvn-objects.md, A pool's storage at creation is its textures' content).
int nvn_flushed(const void *p, uint64_t n);
static int mem_any(const uint8_t *p, uint64_t n) {   // stops at the first non-zero byte
    uint64_t i = 0, v;
    if (!p) return 0;
    for (; i + 8 <= n; i += 8) { memcpy(&v, p + i, 8); if (v) return 1; }
    for (; i < n; i++) if (p[i]) return 1;
    return 0;
}
static int tex_init_fill(const tex_t *t, id<MTLTexture> mt) {
    if (!t->mem || !t->pool || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) || t->has_layout) return 0;
    if (t->d.samples > 1 || ((int)mt.pixelFormat >= 250 && (int)mt.pixelFormat <= 262)) return 0;   // 250..262: Depth16Unorm … X24_Stencil8
    int no_cpu = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) != 0, uncached = (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED) != 0;
    // COMPRESSIBLE is only a hint; in a CPU_NO_ACCESS pool the memory may mean nothing to the CPU (docs/reference/nvn-objects.md, `COMPRESSIBLE` in a pool the CPU writes is only a hint)
    if (no_cpu && (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE)) return 0;
    static int flush_fill = -1;
    if (flush_fill < 0) {
        const char *e = getenv("DM_NVN_FLUSH_FILL");
        flush_fill = !e || atoi(e) != 0;
    }
    if (!no_cpu && !uncached && (!flush_fill || !nvn_flushed(t->mem, t->L.size))) return 0;
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread() || !mem_any(t->mem, t->L.size)) return 0;   // the upload batch belongs to the submit thread
    if (pkg_upload(t, NULL, mt, 0, nil, NULL, 0, NULL) != mt) return 0;
    int why = no_cpu ? 0 : uncached ? 1 : 2;
    static uint64_t n[3];
    if (n[why]++ < 4) MLOG("initial content: ser=%llu %ux%ux%u format %#x filled from its storage, %s (%llu so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format,
                           (const char *[]){"a CPU_NO_ACCESS pool's", "a CPU_UNCACHED pool's", "flushed by the CPU before its first use"}[why], (unsigned long long)n[why]);
    return 1;
}

static int clear_rect_zero(int i);
static NSUInteger tbuf_align(NSUInteger al);
// Metal's texture-buffer alignment or its linear-texture one, whichever is larger
static NSUInteger tbuf_view_align(MTLPixelFormat pf) {
    NSUInteger a = [g_dev minimumTextureBufferAlignmentForPixelFormat:pf], l = [g_dev minimumLinearTextureAlignmentForPixelFormat:pf];
    return tbuf_align(a > l ? a : l);
}
static uint32_t tbuf_lead(int tid, id<MTLTexture> t);
// 1 yes (*pf, *mb, *mo, *row filled), 0 not a linear texture this applies to, -1 its offset or row pitch is not aligned to the device's minimum
static int linear_fit(const tex_t *t, uint32_t *pf, void **mb, uint64_t *mo, size_t *row) {
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem || !t->pool) return 0;
    if (t->d.target != NVN_TEXTURE_TARGET_2D || t->d.levels > 1 || t->d.samples > 1) return 0;
    uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!nvn_fmt_to_mtl(t->d.format, pf, swz) || !*pf || !bpb || bw != 1 || bh != 1) return 0;
    *row = tex_linear_row(t);
    if (!mtl_addr((uint64_t)t->mem, (uint64_t)*row * (t->d.height ? t->d.height : 1), mb, mo)) { RED_ONCE(64, "linear texture storage is not in a memory pool (%ux%u format %#x)", t->d.width, t->d.height, t->d.format); return 0; }
    NSUInteger al = tbuf_align([g_dev minimumLinearTextureAlignmentForPixelFormat:(MTLPixelFormat)*pf]);
    if (*mo % al || *row % al) {
        static int n;
        if (n++ < 4) MLOG("linear texture ser=%llu %ux%u format %#x flags %#x: offset %#llx / row pitch %zu not aligned to %lu bytes (%d so far)",
                          (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, t->d.flags, (unsigned long long)*mo, *row, (unsigned long)al, n);
        return -1;
    }
    return 1;
}
// In NVN a linear texture's content is its storage until the GPU writes it. LINEAR_RENDER_TARGET or IMAGE only say the GPU may write it: such storage holds the content
// while the GPU has not written it (gws_owned). In a pool the CPU cannot write, GPU writes are not recorded: there it stays a GPU texture.
static int gws_owned(uintptr_t lo, uintptr_t hi);
static int linear_gpu_may_write(const tex_t *t) {   // IMAGE alone is a block-linear texture
    return (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) && (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_IMAGE));
}
static int linear_storage_ok(const tex_t *t) {
    if (!(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) || !t->mem) return 0;
    if (!linear_gpu_may_write(t)) return 1;
    return t->pool && !(t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) && !gws_owned((uintptr_t)t->mem, (uintptr_t)t->mem + t->L.size);
}
// made directly on the pool buffer, so the GPU reads the current content at execution time, as on NVN
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
// CPU-written textures with no packaged layout resolve through the packaged path. A finalized serial stays, retired, until its pool is finalized: records and descriptors
// may still resolve it. Under g_cpu_tex_mu.
typedef struct { uintptr_t lo, hi; } cpu_unc;
typedef struct { nvn_hmap live, retired, unc; } cpu_pool;
static pthread_mutex_t g_cpu_tex_mu = PTHREAD_MUTEX_INITIALIZER;
static nvn_hmap g_cpu_pools = {.klen = 8};   // (void *)1 for a pool-less test texture
static unsigned g_cpu_tex_n;
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
// Storage the GPU writes, by address range, not per pool (docs/reference/nvn-objects.md, Uncompressed storage beside storage the GPU writes). A span's whole pages are
// write-protected so a CPU write afterwards is seen; spans do not overlap. Open: a resubmitted command buffer does not stamp its writes again.
typedef struct { uintptr_t lo, hi; uint64_t stamp, seen; const void *rg; int taken; } gws_span;
static gws_span *g_gws;
static size_t g_ngws, g_cgws;
static pthread_mutex_t g_gws_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic int g_gws_new;
static void whole_pages(uintptr_t lo, uintptr_t hi, uintptr_t *a, uintptr_t *e) {
    uintptr_t pg = (uintptr_t)getpagesize();
    *a = (lo + pg - 1) & ~(pg - 1);
    *e = hi & ~(pg - 1);
}
static size_t gws_find(uintptr_t a) {   // g_gws_mu held
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
static void gws_stamp(gws_span *s) {   // the stamp first, then the protection; g_gws_mu held
    uintptr_t a, e;
    whole_pages(s->lo, s->hi, &a, &e);
    s->stamp = s->seen = nvn_dirty_now();
    s->rg = e > a ? nvn_dirty_span((const void *)a, e - a) : NULL;
    if (s->rg) {
        nvn_dirty_protect((const void *)a, e - a);
    }
}
// g_gws_mu held
static int gws_cpu_since(gws_span *s, uintptr_t lo, uintptr_t hi) {
    uintptr_t a, e;
    whole_pages(MAX(lo, s->lo), MIN(hi, s->hi), &a, &e);
    if (e <= a || !s->rg) {
        return 0;
    }
    return nvn_dirty_since_cached(&s->rg, (const void *)a, e - a, s->stamp);
}
static void gws_rewritten(gws_span *s) {   // g_gws_mu held
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
static void gws_set(uintptr_t lo, uintptr_t hi) {   // the spans it overlaps keep their parts outside it, with their own stamps; g_gws_mu held
    size_t i = gws_find(lo), j = i;
    while (j < g_ngws && g_gws[j].lo < hi) {
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
// A sliver (no whole page of [lo, hi), not covering it) does not decide: the dirty tracker cannot answer for it (docs/reference/nvn-objects.md, A span's sliver inside a texture).
static int gws_sliver(const gws_span *s, uintptr_t lo, uintptr_t hi) {
    if (s->lo <= lo && s->hi >= hi) {
        return 0;
    }
    uintptr_t a, e;
    whole_pages(MAX(lo, s->lo), MIN(hi, s->hi), &a, &e);
    return e <= a;
}
static int gws_owned(uintptr_t lo, uintptr_t hi) {   // the GPU wrote some of [lo, hi) and the CPU has not rewritten it since
    int owned = 0;
    pthread_mutex_lock(&g_gws_mu);
    for (size_t i = gws_find(lo); i < g_ngws && g_gws[i].lo < hi && !owned; i++) {
        owned = !gws_sliver(&g_gws[i], lo, hi) && !gws_cpu_since(&g_gws[i], lo, hi);
    }
    pthread_mutex_unlock(&g_gws_mu);
    return owned;
}
static void gws_take(uintptr_t lo, uintptr_t hi) {   // the next GPU write there makes it a GPU texture
    pthread_mutex_lock(&g_gws_mu);
    for (size_t i = gws_find(lo); i < g_ngws && g_gws[i].lo < hi; i++) {
        g_gws[i].taken = 1;
    }
    pthread_mutex_unlock(&g_gws_mu);
}
// A buffer made while any of its pages is read-only drops every GPU write through it silently, so its protected pages are opened while it is made.
static id<MTLBuffer> pool_buf(uintptr_t a, uintptr_t b) {
    nvn_dirty_open((void *)a, b - a);
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    nvn_dirty_close();
    return buf;
}
static void gws_drop_pool(const pool_t *p) {
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
int mtl_tex_gpu_owned(uint64_t serial) {
    __unsafe_unretained id o = tex_get(serial);
    if (!obj_is_tex(o)) return 0;
    return (((id<MTLTexture>)o).usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite)) != 0;
}
void mtl_tex_cpu_written(const tex_t *t) {
    uint64_t serial = t->serial;
    if (!serial) return;
    // nvn_ft_poll can mark a snapshot after Finalize: the live check is serialized with release (g_smu → g_cpu_tex_mu).
    pthread_mutex_lock(&g_smu);
    pthread_mutex_lock(&g_cpu_tex_mu);
    int live = own_tex_live_locked(serial);
    if (live || (t->pool && t->mem)) {
        cpu_pool *cp = cpu_pool_get(t->pool, 1);
        if (!nvn_hm_get(&cp->live, &serial) && !nvn_hm_get(&cp->retired, &serial)) {
            nvn_hm_put(live ? &cp->live : &cp->retired, &serial, t->pool ? (void *)t->pool : (void *)1);
            if (live) g_cpu_tex_n++;
            own_note(serial);
            static int n;
            if (n++ < 16) MLOG("CPU-written texture ser=%llu %ux%ux%u format %#x target %d levels %u: uploaded / re-uploaded like a swizzled texture", (unsigned long long)serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels);
        }
    }
    pthread_mutex_unlock(&g_cpu_tex_mu);
    pthread_mutex_unlock(&g_smu);
}
unsigned mtl_cpu_tex_count(void) {
    pthread_mutex_lock(&g_cpu_tex_mu);
    unsigned n = g_cpu_tex_n;
    pthread_mutex_unlock(&g_cpu_tex_mu);
    return n;
}
static int tex_cpu_written(const tex_t *t) {
    if (!t || !t->serial) return 0;
    uint64_t serial = t->serial;
    pthread_mutex_lock(&g_cpu_tex_mu);
    const cpu_pool *cp = cpu_pool_get(t->pool, 0);
    int r = cp && (nvn_hm_get(&cp->live, &serial) != NULL || nvn_hm_get(&cp->retired, &serial) != NULL);
    pthread_mutex_unlock(&g_cpu_tex_mu);
    return r;
}
// docs/reference/nvn-objects.md, Sampled textures in a sparse pool uploaded through the page table. mtl_read_texels reads these from the storage too, on any thread.
static int tex_from_storage(const tex_t *t) {
    if (!t->mem) return 0;
    if (t->has_layout || tex_cpu_written(t)) return 1;
    if (!t->pool || t->d.target == NVN_TEXTURE_TARGET_BUFFER || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET |
        NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_IMAGE))) return 0;
    if (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)
        return nvn_sparse_pool(t->pool);
    // SPARSE selects the layout even in non-virtual memory (docs/reference/nvn-objects.md, Block-compressed textures with sparse layout in CPU-visible pools)
    return (t->d.flags & NVN_TEXTURE_FLAGS_SPARSE) && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) &&
        t->d.samples <= 1 && t->d.target != NVN_TEXTURE_TARGET_1D_ARRAY && fmt_compressed(t->d.format);
}
// Storage the CPU wrote before first use: the GPU cannot have written it yet (docs/reference/nvn-objects.md, Block-compressed storage the CPU wrote before first use;
// Uncompressed storage the CPU wrote, in a pool the GPU has not written).
static int tex_unc(const tex_t *t) { return !fmt_compressed(t->d.format) && t->d.target != NVN_TEXTURE_TARGET_3D; }   // the uncompressed 2D kind, which the GPU can render into
// unc: tex_unc
static int tex_storage_ok(const tex_t *t, int unc) {
    if (!t->mem || !t->pool || t->has_layout || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER) return 0;
    // not what only the GPU or the decoder fills
    if (unc && (fmt_depth(t->d.format) || (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE)))) return 0;
    if (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) return 0;
    // IMAGE only matters for an uncompressed texture: shaders cannot write a block-compressed one, which a game may flag IMAGE all the same
    return !((t->d.flags & (NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_SPARSE))
             || ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && !fmt_compressed(t->d.format)));
}
static void cpu_unc_take(const tex_t *t) {   // the GPU's next write there makes it a GPU texture again
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
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DM_NVN_STORAGE_FIRST_USE");
        on = !e || atoi(e) != 0;
    }
    int unc = tex_unc(t);
    const pool_t *pool = t->pool;
    if (!on || !pool || !tex_storage_ok(t, unc)) return 0;
    // gws_owned takes a lock
    if ((unc && gws_owned((uintptr_t)t->mem, (uintptr_t)t->mem + t->L.size)) || !mem_any(t->mem, t->L.size)) return 0;
    if (unc) {
        cpu_unc_take(t);
    }
    static uint64_t n[2];
    if (n[unc]++ < 4) MLOG("texture written by the CPU before its first use ser=%llu %ux%ux%u format %#x%s pool flags %#x: uploaded from its storage, re-uploaded when it changes (%llu so far)",
                           (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, unc ? " (uncompressed, in storage the GPU has not written)" : "", pool->flags, (unsigned long long)n[unc]);
    return 1;
}
// The CPU rewrote a registered texture after its first use: the tex_cpu_content rule again. Its host texture's usage cannot tell, as tex_desc gives every uncompressed
// texture render-target use. A block-compressed texture is uploaded whatever the GPU wrote: GPU writes into it reach its storage too.
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
// One a view on its pool's buffer can take is that view, which reads the storage itself: made CPU-written it would upload the whole texture at each write.
void mtl_tex_linear_cpu_written(const tex_t *t) {
    uint32_t pf; void *mb; uint64_t mo; size_t row;
    if (linear_fit(t, &pf, &mb, &mo, &row) == 1) return;
    if (linear_gpu_may_write(t)) cpu_unc_take(t);
    mtl_tex_cpu_written(t);
}
// The storage is no longer the whole truth: the texture becomes a GPU texture with the content it has. Submit thread, as tex_cpu_content is.
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

            if (n == cap) {
                cap = cap ? cap * 2 : 64;
                uint64_t *grown = realloc(ser, cap * sizeof *ser);
                if (!grown) MDIE("cpu_unc_drop: out of memory");
                ser = grown;
            }
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
        if ([o isKindOfClass:[NvPkg class]]) {
            NvPkg *pk = o;
            id<MTLTexture> mt = pk->view ? pk->view : pk->tex;
            if (mt) tex_put(ser[i], mt);
        }
        own_note(ser[i]);
    }
    static int nl;
    if (n && nl++ < 8) MLOG("storage the GPU writes: %u uncompressed textures taken from it are GPU textures from now on", n);
    free(ser);
}
static int tex_storage_readable(const tex_t *t) {
    return t->mem && t->d.samples <= 1 && t->d.target != NVN_TEXTURE_TARGET_BUFFER && t->d.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE && t->d.target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY
           && !(t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
}
// in_storage (may be NULL): set off the submit thread when making the host texture would take an upload; nil is returned and the caller reads the storage
static void *tex_mtl_resolve(const tex_t *ct, int *in_storage, id<MTLTexture> __strong *owned);
// Present and ReadTexels can outlast a replay fence: a strong reference, taken inside the resolver before its local owners go away
static id<MTLTexture> tex_mtl_(const tex_t *ct, int *in_storage) {
    id<MTLTexture> owned = nil;
    tex_mtl_resolve(ct, in_storage, &owned);
    return owned;
}
static int bcp_up_fill(const tex_t *t, id<MTLTexture> mt);
static void bcp_up_follow(const tex_t *t, id<MTLTexture> mt);
static id<MTLTexture> tex_mtl(const tex_t *ct) { AP_B; id<MTLTexture> r = tex_mtl_(ct, NULL); AP_E(4); return r; }
// a serial or package holds the result until the submission fence (nvn_owner.h)
static void *tex_mtl_replay_ref(const tex_t *ct) { AP_B; void *r = tex_mtl_resolve(ct, NULL, NULL); AP_E(4); return r; }
#define tex_mtl_replay(ct) ((__bridge id<MTLTexture>)tex_mtl_replay_ref(ct))
static inline void *tex_mtl_result(id<MTLTexture> texture, id<MTLTexture> __strong *owned) {
    if (owned) *owned = texture;
    return (__bridge void *)texture;
}
static void reup_note(const NvPkg *pk, const tex_t *t, unsigned taken, uint64_t bytes, uint64_t ns) {
    static int on = -1, nl;
    if (on < 0) on = getenv("DM_NVN_REUPLOADS") != NULL;
    if (!on || nl >= 400) return;
    nl++;
    uintptr_t first = 0;
    size_t pg = (size_t)getpagesize(), n = pk->sparse ? 0 : nvn_dirty_since_except(pk->span, t->mem, t->L.size, pk->stamp, NULL, 0, &first);
    unsigned layer = 0, lv = 0;
    if (n && n != SIZE_MAX && t->L.layer_size) {
        uint64_t o = first - (uintptr_t)t->mem;
        layer = (unsigned)(o / t->L.layer_size);
        o %= t->L.layer_size;
        while (lv + 1 < t->d.levels && nvn_tex_level_offset(&t->d, lv + 1) <= o) lv++;
    }
    char w[96];
    if (pk->sparse) snprintf(w, sizeof w, "a sparse pool's");
    else if (n == SIZE_MAX) snprintf(w, sizeof w, "untracked");
    else snprintf(w, sizeof w, "%zu of %zu, the first in layer %u level %u", n, (size_t)((t->L.size + pg - 1) / pg), layer, lv);
    char u[64];
    if (taken) snprintf(u, sizeof u, "%.1f MB read, %u images copied from the texture it replaces", bytes / 1048576.0, taken);
    else snprintf(u, sizeof u, "read whole");
    MLOG("re-upload ser=%llu %ux%ux%u format %#x target %d levels %u: %.1f MB, %s, %.2f ms, frame %llu submission %llu; pages written since its upload: %s; GPU writes stored into its storage since then: %u",
         (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->L.size / 1048576.0, u, ns / 1e6, (unsigned long long)atomic_load(&g_cur_frame),
         (unsigned long long)g_sub_serial, w, pk->stored);
}
// A sparse texture is bound from its first fully mapped level (docs/reference/rendering.md, Sparse textures bound from their first mapped level). Single-layer 2D only.
static void sparse_view(NvPkg *pk, const tex_t *t) {
    unsigned n = (unsigned)pk->tex.mipmapLevelCount, r = n;
    if (t->d.target == NVN_TEXTURE_TARGET_2D && n > 1) {
        while (r > 0) {
            uint64_t lo = nvn_tex_level_offset(&t->d, r - 1), hi = r < n ? nvn_tex_level_offset(&t->d, r) : t->L.size;
            if (!nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + lo, hi - lo)) break;
            r--;
        }
    } else r = 0;
    if (r == n) r = n - 1;   // nothing mapped: the smallest level (reads 0)
    if (pk->view) defer_obj_unref(pk->view);   // a view shares its texture's allocation: drop only the reference
    pk->view = r ? [pk->tex newTextureViewWithPixelFormat:pk->tex.pixelFormat textureType:pk->tex.textureType levels:NSMakeRange(r, n - r) slices:NSMakeRange(0, 1)] : nil;
    static int nl;
    if (r != (unsigned)pk->vbase && nl++ < 6) MLOG("sparse texture ser=%llu %ux%u: sampled from level %u of %u (finer levels not mapped)", (unsigned long long)t->serial, t->d.width, t->d.height, r, n);
    pk->vbase = (int)r;
}
static uint64_t g_texinfo_ser[8];
static int g_texinfo_n;
// A 4-sample W x H colour target and the 2W x 2H single-sample image over one NVN offset are one storage; Apple GPUs lay them out alike in a heap when both have the same
// compression (checked per kind, pair_layout_ok), so the second is placed at offset 0 of the other's heap. While it lives, barriers flush aliased memory.
@interface NvStoragePair : NSObject @end   // held by the second texture of a pair
@implementation NvStoragePair
- (void)dealloc { atomic_fetch_sub(&g_pair_n, 1); }
@end
static char g_pair_key;
static int pair_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_STORAGE_ALIAS");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("storage pairs (a 4-sample target over a single-sample image's bytes): kept apart (DM_NVN_STORAGE_ALIAS=0)");
    }
    return v;
}
static int pair_shape(const tex_t *t) {
    if (!t->pool || !t->pool->mem || !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || nvn_sparse_pool(t->pool) || t->has_layout) return 0;
    if ((t->d.flags & IMAGE_NOT_SHARED) || t->d.depth != 1 || t->d.levels != 1 || t->d.stride || fmt_depth(t->d.format) || fmt_compressed(t->d.format)) return 0;
    return (t->d.target == NVN_TEXTURE_TARGET_2D && t->d.samples <= 1) || (t->d.target == NVN_TEXTURE_TARGET_2D_MULTISAMPLE && t->d.samples == 4);
}
static int pair_of(const nvn_texdesc *a, const nvn_texdesc *b) {
    if (a->format != b->format || (a->samples > 1) == (b->samples > 1) || (b->flags & IMAGE_NOT_SHARED) || b->depth != 1 || b->levels != 1 || b->stride) return 0;
    const nvn_texdesc *m = a->samples > 1 ? a : b, *s = a->samples > 1 ? b : a;
    if (m->target != NVN_TEXTURE_TARGET_2D_MULTISAMPLE || m->samples != 4 || s->target != NVN_TEXTURE_TARGET_2D || s->width != 2 * m->width || s->height != 2 * m->height) return 0;
    nvn_texlayout la, lb; nvn_tex_layout(a, &la); nvn_tex_layout(b, &lb);
    return la.size && la.size == lb.size;
}
// Checked once per kind before the first such pair (dm/gfx/msl/pair.metal): only GPUs that were tried are known to lay it out so. *bad: UINT32_MAX = not checked
static _Atomic unsigned g_pair_probes;
static size_t mtl_pf_block_bytes(MTLPixelFormat pf);
static int pf_int_kind(MTLPixelFormat f);
static int pair_layout_ok(MTLTextureDescriptor *a, MTLTextureDescriptor *b, uint32_t *bad) {
    typedef struct { NSUInteger pf, w, h; uint8_t c1, c4, ok; uint32_t bad; } kind;
    static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT;
    static kind seen[32];
    static unsigned n;
    MTLTextureDescriptor *one = a.sampleCount > 1 ? b : a, *four = a.sampleCount > 1 ? a : b;
    kind k = {one.pixelFormat, one.width, one.height, (uint8_t)(one.allowGPUOptimizedContents && !(one.usage & MTLTextureUsagePixelFormatView)),
              (uint8_t)(four.allowGPUOptimizedContents && !(four.usage & MTLTextureUsagePixelFormatView)), 0, UINT32_MAX};
    os_unfair_lock_lock(&lk);
    for (unsigned i = 0; i < n; i++)
        if (seen[i].pf == k.pf && seen[i].w == k.w && seen[i].h == k.h && seen[i].c1 == k.c1 && seen[i].c4 == k.c4) {
            *bad = seen[i].bad;
            os_unfair_lock_unlock(&lk);
            return seen[i].ok;
        }
    os_unfair_lock_unlock(&lk);
    static id<MTLComputePipelineState> ps[3];
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSError *e = nil;
        id<MTLLibrary> lib = host_lib("pair", &e);
        const char *fn[3] = {"pair_cmp_f", "pair_cmp_u", "pair_cmp_i"};
        for (int i = 0; i < 3 && lib; i++) {
            id<MTLFunction> f = [lib newFunctionWithName:@(fn[i])];
            ps[i] = f ? dev_cps(f, &e) : nil;
        }
    });
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    size_t bpt = mtl_pf_block_bytes(one.pixelFormat), row = one.width * bpt, bytes = row * one.height;
    id<MTLComputePipelineState> cmp = ps[pf_int_kind(one.pixelFormat)];
    MTLSizeAndAlign s1 = [g_dev heapTextureSizeAndAlignWithDescriptor:one], s4 = [g_dev heapTextureSizeAndAlignWithDescriptor:four];
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.size = MAX(s1.size, s4.size);
    id<MTLHeap> heap = cmp && bpt && hd.size ? [g_dev newHeapWithDescriptor:hd] : nil;
    id<MTLTexture> t1 = [heap newTextureWithDescriptor:one offset:0], t4 = [heap newTextureWithDescriptor:four offset:0];
    id<MTLBuffer> pat = t1 && t4 ? [g_dev newBufferWithLength:bytes options:MTLResourceStorageModeShared] : nil;
    id<MTLBuffer> cnt = [g_dev newBufferWithLength:4 options:MTLResourceStorageModeShared];
    if (pat && cnt) {
        uint32_t *q = pat.contents;   // each byte below 0x40 so no float format reads an infinity or a NaN
        for (size_t i = 0; i < bytes / 4; i++) q[i] = ((uint32_t)i * 2654435761u >> 2 & 0x3f3f3f3fu) | 1u;
        *(uint32_t *)cnt.contents = 0;
        id<MTLCommandBuffer> cb = cl_cb();   // two command buffers: the placement heap's textures are not tracked
        id<MTLBlitCommandEncoder> be = [cb blitCommandEncoder];
        [be copyFromBuffer:pat sourceOffset:0 sourceBytesPerRow:row sourceBytesPerImage:bytes sourceSize:MTLSizeMake(one.width, one.height, 1) toTexture:t1 destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
        [be endEncoding]; [cb commit]; [cb waitUntilCompleted];
        id<MTLCommandBuffer> cc = cl_cb();
        id<MTLComputeCommandEncoder> ce = [cc computeCommandEncoder];
        [ce useResource:t1 usage:MTLResourceUsageRead]; [ce useResource:t4 usage:MTLResourceUsageRead];
        [ce setComputePipelineState:cmp]; [ce setTexture:t1 atIndex:0]; [ce setTexture:t4 atIndex:1]; [ce setBuffer:cnt offset:0 atIndex:0];
        [ce dispatchThreads:MTLSizeMake(four.width, four.height, 1) threadsPerThreadgroup:MTLSizeMake(8, 8, 1)];
        [ce endEncoding]; [cc commit]; [cc waitUntilCompleted];
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
    own_alias_at_locked(t->pool, t->off, ^(const alias_key *k, id shared) {
        if (!other && pair_of(&t->d, &k->d) && [shared conformsToProtocol:@protocol(MTLTexture)]) other = shared;
    });
    pthread_mutex_unlock(&g_smu);
    if (!other) return nil;
    char *key = &g_image_heap_owner_key; id owner = objc_getAssociatedObject(other, key);
    if (!owner) { key = &g_rh_owner_key; owner = objc_getAssociatedObject(other, key); }
    id<MTLHeap> heap = owner ? other.heap : nil;
    const char *why = !heap || other.heapOffset ? "is not at the start of a heap of the host's" : NULL;
    MTLTextureDescriptor *d = [d0 copy];
    d.allowGPUOptimizedContents = other.allowGPUOptimizedContents; d.usage |= other.usage & MTLTextureUsagePixelFormatView;   // the format-view usage turns compression off
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
    id<MTLTexture> mt = [heap newTextureWithDescriptor:d offset:0];
    if (!mt) return nil;
    objc_setAssociatedObject(mt, key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    atomic_fetch_add(&g_pair_n, 1);
    objc_setAssociatedObject(mt, &g_pair_key, [NvStoragePair new], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    res_add(mt);
    static int n;
    if (n++ < 6) MLOG("storage pair: ser=%llu %ux%u %d-sample format %#x is placed over the %lux%lu %lu-sample texture at its pool bytes (%llu MB, compression %d)", (unsigned long long)t->serial,
                      t->d.width, t->d.height, t->d.samples, t->d.format, (unsigned long)other.width, (unsigned long)other.height, (unsigned long)other.sampleCount,
                      (unsigned long long)(heap.size >> 20), (int)d.allowGPUOptimizedContents);
    return mt;
}
// Distinct compressible image descriptions at one NVN address alias physical memory (a resolution family), in one placement heap. Inherited texels are not translated
// between incompatible layouts. Making a member must not zero another alias's live content.
static id<MTLTexture> image_texture(const tex_t *t, MTLTextureDescriptor *d0) {
    if (!image_shape(t) || !t->pool->mem || !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) || nvn_sparse_pool(t->pool)) return nil;
    // Uncompressed: a member reading bytes another resolution wrote would decode its compressed blocks as garbage; uncompressed they are misplaced texels, as on the console.
    static int nocomp = -1;
    if (nocomp < 0) {
        const char *e = getenv("DM_NVN_FAMILY_NOCOMPRESS");
        nocomp = !(e && !strcmp(e, "0"));
    }
    MTLTextureDescriptor *d = d0;
    if (nocomp) {
        d = [d0 copy];
        d.allowGPUOptimizedContents = NO;
    }
    MTLSizeAndAlign need = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    if (!need.size) return nil;
    __block NvImageHeap *owner = nil;
    MTLTextureDescriptor *largest = d;
    NSUInteger capacity = need.size;
    int family = 0;
    pthread_mutex_lock(&g_smu);
    own_alias_at_locked(t->pool, t->off, ^(const alias_key *k, id shared) {
        if (k->d.format != t->d.format) return;
        id<MTLTexture> other = shared;
        NvImageHeap *o = objc_getAssociatedObject(other, &g_image_heap_owner_key);
        // Metal omits ShaderAtomic from usage
        MTLTextureUsage usage = other.usage | (tex_atomic_capable(other) ? MTLTextureUsageShaderAtomic : 0);
        if (o && o->heap.size >= need.size && usage == d.usage && (!owner || o->heap.size < owner->heap.size)) owner = o;
    });
    if (!owner) for (uint32_t i = 0; i < g_image_desc.cap; i++) if (g_image_desc.vals[i]) {
        const tex_t *other = g_image_desc.vals[i];
        if (other->pool != t->pool || other->off != t->off || other->d.format != t->d.format) continue;
        family |= other->d.width != t->d.width || other->d.height != t->d.height;
        MTLTextureDescriptor *od = tex_desc(other, 0);
        if (!od) continue;
        if (nocomp) od.allowGPUOptimizedContents = NO;
        MTLSizeAndAlign sz = [g_dev heapTextureSizeAndAlignWithDescriptor:od];
        if (sz.size > capacity) { capacity = sz.size; largest = od; }
    }
    pthread_mutex_unlock(&g_smu);
    int fresh = !owner;
    if (fresh) {
        if (!family) return nil;
        int reused;
        id<MTLHeap> heap = rh_heap(capacity, d.storageMode, d.cpuCacheMode, d.hazardTrackingMode, &reused);
        if (!heap) return nil;
        owner = [NvImageHeap new]; owner->heap = heap;
        atomic_fetch_add(&g_image_heap_n, 1);
    }
    id<MTLTexture> mt = [owner->heap newTextureWithDescriptor:d offset:0];
    if (!mt) return nil;
    objc_setAssociatedObject(mt, &g_image_heap_owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    res_add(mt);
    if (fresh) {
        // only a new heap is zeroed; later descriptors alias its existing bytes
        id<MTLTexture> zero = largest == d ? mt : [owner->heap newTextureWithDescriptor:largest offset:0];
        if (!zero) return nil;
        objc_setAssociatedObject(zero, &g_image_heap_owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
        tex_zero_fill(zero);
    }
    return mt;
}
// for a texture whose storage is gone; nil if it has none
static id<MTLTexture> tex_made(const tex_t *t) {
    id have = tex_get(t->serial);
    if ([have isKindOfClass:[NvPkg class]]) { NvPkg *pk = have; return pk->view ? pk->view : pk->tex; }
    return have;
}
// a sparse texture bound from its first mapped level: NVN level l is the view's l - vbase. 0 for every other texture
static unsigned tex_level_shift(const tex_t *t, id<MTLTexture> base) {
    id have = tex_get(t->serial);
    if (![have isKindOfClass:[NvPkg class]]) return 0;
    NvPkg *pk = have; return pk->view && pk->view == base ? (unsigned)pk->vbase : 0;
}
// copies name levels as NVN does: through the view, a copy into level 0 of a texture bound from level 1 would go to level 1
static id<MTLTexture> tex_mtl_all(const tex_t *t) { id<MTLTexture> mt = tex_mtl_replay(t); return mt && tex_level_shift(t, mt) ? mt.parentTexture : mt; }
// up to 4, zero-terminated (an RGBA8 texture has RGBA8_SRGB and RGBX8_SRGB)
// The shared texture over ak's storage whose key differs only by a format pf_storage_twins pairs with pf, nil if none; *pk its key. The first one made over the storage is the
// parent, so the order the game's textures are first used in does not matter. Formats other than an sRGB / linear twin only in a memory pool: textures with none share no storage
static id<MTLTexture> storage_twin(const alias_key *ak, MTLPixelFormat pf, alias_key *pk) {
    __block id<MTLTexture> got = nil;
    pthread_mutex_lock(&g_smu);
    own_alias_at_locked((const void *)(uintptr_t)ak->pool, ak->off, ^(const alias_key *k, id o) {
        if (got || k->d.format == ak->d.format || ![o conformsToProtocol:@protocol(MTLTexture)]) return;
        alias_key x; memcpy(&x, k, sizeof x); x.d.format = ak->d.format;   // memcpy keeps the zeroed padding the table hashes
        id<MTLTexture> b = o;
        if (memcmp(&x, ak, sizeof x) || b.parentTexture || !pf_storage_twins(b.pixelFormat, pf) || (!ak->pool && tex_need_pfv(b.pixelFormat, pf))) return;
        got = b; memcpy(pk, k, sizeof *pk);
    });
    pthread_mutex_unlock(&g_smu);
    return got;
}
// Every use the guest may make, whatever its IMAGE flag: a buffer view has no compression to lose, and its NVN texture objects share one view. Metal makes no writable BGRA8Unorm texture buffer.
static MTLTextureUsage tbuf_usage(MTLPixelFormat pf) {
    MTLTextureUsage u = MTLTextureUsageShaderRead | MTLTextureUsagePixelFormatView;
    if (pf != MTLPixelFormatBGRA8Unorm) u |= MTLTextureUsageShaderWrite | (pf_atomic(pf) ? MTLTextureUsageShaderAtomic : 0);
    return u;
}
// zero copy: the GPU reads the pool's current content at execution time, as NVN does
static id<MTLTexture> tex_buffer(const tex_t *t) {
    uint32_t pf = 0; uint8_t swz[4]; int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    void *mb; uint64_t mo;
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf || !bpb) { RED_ONCE(36, "buffer texture format %#x not mapped", t->d.format); return nil; }
    if (!t->mem || !mtl_addr((uint64_t)t->mem, (uint64_t)t->d.width * bpb, &mb, &mo)) { RED_ONCE(37, "buffer texture storage is not in a memory pool"); return nil; }
    id<MTLBuffer> b = (__bridge id<MTLBuffer>)mb;
    NSUInteger al = tbuf_view_align((MTLPixelFormat)pf);
    // Metal makes a buffer texture only at an aligned offset, and a view made below it is addressed from the boundary below: the view starts there and shader accesses move
    // their coordinates by the lead (TranslationFlags.MetalBufferImageBias). An access with no table entry reads an aligned copy (tbuf_shadow).
    NSUInteger lead = (NSUInteger)(mo % al);
    if (lead % (NSUInteger)bpb) { RED_ONCE(38, "buffer texture offset %#llx not aligned to its %d-byte elements", (unsigned long long)mo, bpb); return nil; }
    NSUInteger w = t->d.width + lead / (NSUInteger)bpb, row = (w * (NSUInteger)bpb + al - 1) / al * al;
    if (mo - lead + row > b.length) { RED_ONCE(166, "buffer texture at pool offset %#llx, %lu elements of format %#x, ends too near its pool buffer's end for a view from the %lu-byte boundary below: the slot reads 0",
                                              (unsigned long long)mo, (unsigned long)t->d.width, t->d.format, (unsigned long)al); return nil; }
    static int once;
    if (lead && !once++) MLOG("buffer texture at pool offset %#llx, below Metal's %lu-byte alignment: viewed from %#llx, its coordinates moved by %lu elements (first case)", (unsigned long long)mo, (unsigned long)al, (unsigned long long)(mo - lead), (unsigned long)(lead / (NSUInteger)bpb));
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
    // Off the submit thread nothing is uploaded or synced. Whether a texture is taken from its storage is decided on this one look: no caller's earlier look can decide for it.
    int off = atomic_load(&g_sub_thread_set) && !on_sub_thread();
    if (t->magic != MAGIC('T', 'E', 'X', 0)) {
        RED_ONCE(70, "texture object no longer valid (magic %#x, first case ser=%llu): no MTLTexture created", t->magic, (unsigned long long)t->serial);
        return NULL;
    }
    // t->pool may be a finalized pool's freed memory, named by a copy of the texture that an ID or a record keeps
    if (t->pool && t->pool->magic != MAGIC('P', 'O', 'O', 'L')) {
        id<MTLTexture> made = tex_made(t);
        static uint64_t n;
        if (n++ < 4) MLOG("texture of a memory pool that is gone ser=%llu %ux%u format %#x: %s (%llu so far)", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format,
                          made ? "keeps its host texture, not refreshed" : "no host texture made", (unsigned long long)n);
        return tex_mtl_result(made, owned);
    }
    int packaged = tex_from_storage(t);
    {   // DM_NVN_TEXINFO=WxH: logs storage and description of textures of that size on first resolution
        static int ti = -1;
        static uint32_t tw, th;
        static uint64_t last[64];
        static int nl;
        if (ti > 0 && t->d.width == tw && t->d.height == th && g_texinfo_n < 8) {
            int dup = 0;
            for (int i = 0; i < g_texinfo_n; i++) dup |= g_texinfo_ser[i] == t->serial;
            if (!dup && !(t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE)) g_texinfo_ser[g_texinfo_n++] = t->serial;
        }
        if (ti < 0) {
            const char *e = getenv("DM_NVN_TEXINFO");
            ti = e && sscanf(e, "%ux%u", &tw, &th) == 2;
        }
        if (ti && t->d.width == tw && t->d.height == th) {
            int seen = 0;
            for (int i = 0; i < nl; i++) if (last[i] == t->serial) seen = 1;
            static uint64_t n;
            if (!seen && (n++ < 40 || n % 600 == 0)) {
                if (nl < 64) last[nl++] = t->serial;
                else last[n % 64] = t->serial;
                uint64_t nz = 0;
                if (t->mem && t->L.size <= (64u << 20)) {
                    uint8_t *m = t->mem, *part = NULL;
                    if (t->pool && nvn_sparse_pool(t->pool) && (part = malloc(t->L.size))) {
                        nvn_sparse_copy(t->pool, part, (uint64_t)(uintptr_t)t->mem, t->L.size);
                        m = part;
                    }
                    for (uint64_t i = 0; i < t->L.size; i++) nz += m[i] != 0;
                    free(part);
                }
                MLOG("TEXINFO ser=%llu swizzled=%d has_layout=%d mem=%p pool=%p(%#x) off=%#llx target=%d format=%#x flags=%#x samples=%d %ux%ux%u levels=%u stride=%u pkg_layout=%#x storage %llu bytes non-zero %llu",
                     (unsigned long long)t->serial, packaged, t->has_layout, t->mem, t->pool, t->pool ? t->pool->flags : 0, (unsigned long long)t->off, t->d.target, t->d.format, t->d.flags, t->d.samples,
                     t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.stride, t->d.pkg_layout, (unsigned long long)t->L.size, (unsigned long long)nz);
            }
        }
    }
    id have = tex_get(t->serial);
    if (!packaged && !have && DM_GAME_TEX_DIRTY_UPLOAD && t->mem && on_sub_thread()) {   // first use of a texture the CPU wrote since init without a flush
        int nvn_ft_written(const tex_t *);
        packaged = nvn_ft_written(t);
    }
    if (!packaged && !have && on_sub_thread() && tex_cpu_content(t)) { mtl_tex_cpu_written(t); packaged = 1; }   // storage the CPU wrote before its first use
    if (!packaged && !have && on_sub_thread() && linear_storage_ok(t)) {   // a linear texture no view on its buffer can take (row pitch off the device's alignment)
        uint32_t pf; void *mb; uint64_t mo; size_t row;
        if (linear_fit(t, &pf, &mb, &mo, &row) < 0) {
            if (linear_gpu_may_write(t)) cpu_unc_take(t);
            mtl_tex_cpu_written(t); packaged = 1;
        }
    }
    if (packaged) {
        NvPkg *pk = [have isKindOfClass:[NvPkg class]] ? have : nil;   // resolved as GPU-only before the CPU wrote it: dropped and rebuilt as packaged
        // Off the submit thread a reader takes the storage: uploading or rebuilding is the submit thread's alone.
        if (off && in_storage) { *in_storage = 1; return NULL; }
        if (!pk && !have && !(t->pool && nvn_sparse_pool(t->pool)) && (pk = pkg_shared(t))) tex_put(t->serial, pk);
        // Other threads use an uploaded one as it is; the submit thread's next look still finds a write since (pk->seen stays).
        if (off) {
            if (pk) return tex_mtl_result(pk->ps ? pk->tex : pk->view ? pk->view : pk->tex, owned);
            RED_ONCE(168, "texture ser=%llu taken from its storage resolved first off the submit thread: no host texture until the submit thread uploads it", (unsigned long long)t->serial);
            return NULL;
        }
        if (pk && pk->ps) { sparse_look(pk->ps, t); return tex_mtl_result(pk->tex, owned); }
        if (!pk) {
            id<MTLTexture> made = nil;
            NvSparse *ps = sparse_make(t, 0, &made);
            if (ps) {
                pk = [NvPkg new];
                pk->ps = ps; pk->tex = made; pk->sparse = 1;
                sparse_sync(ps, t, 1);
                tex_put(t->serial, pk);
                return tex_mtl_result(pk->tex, owned);
            }
        }
        if (pk) {
            if (!g_pool_walk) pk->used_tu = g_tu_now;   // a walk's lookup is not a use
            if (gsb_held(t->mem, t->L.size)) return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);   // its storage lacks a GPU write this command buffer stores: uploaded at its next use if the CPU wrote it
            uint64_t now = nvn_dirty_now(), own = nvn_dirty_own_writes();
            int tracked = pk->span != NULL;   // a texture's pool stays registered while the texture lives
            // Looked at already in this submission: it keeps that look's texture. A write another thread makes during the replay races the GPU's run of that submission on the console
            // too, and the next submission's first look finds it; looking at each use fed on itself (docs/reference/frame-rate.md).
            int written = !tracked || (now != pk->seen && nvn_dirty_since_span(pk->span, t->mem, t->L.size, pk->stamp));
            int kept = written && tracked && !pk->sparse && atomic_load_explicit(&g_sub_open, memory_order_relaxed) && pk->sub == g_sub_serial && pk->own == own;
            int clean = !written || kept;
            // pkg_edges_clean protects the storage again, so not while a page inside it is being written
            if (kept && (pk->edge_n[0] || pk->edge_n[1]) && !pkg_inner_written(pk, t) && pkg_edges_clean(pk, t)) return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
            if (kept) {
                now = pk->seen;
                g_pkg_kept = 1;
            } else if (!pk->sparse) {
                pk->sub = g_sub_serial; pk->own = own;
            }
            // a write or mapping another thread makes during the replay is found at the next submission's check
            if (pk->sparse && sparse_checked(pk)) { clean = 1; now = pk->seen; }
            else if (pk->sparse) {
                uint64_t g = nvn_sparse_gen();
                clean = (now == pk->seen && g == pk->vseen) || !nvn_sparse_since(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size, pk->vgen, pk->stamp);
                pk->vseen = g; pk->sub = g_sub_serial; pk->own = own;
            }
            pk->seen = now;
            // the report of a dropped batch comes after later uploads were encoded
            int whole = pk->part && pk->lost_gen != atomic_load(&g_up_lost_gen);
            if (whole && clean) {
                clean = 0;
                static int nl;
                if (nl++ < 8) MLOG("re-uploading a texture holding images copied from one uploaded before a batch was dropped ser=%llu", (unsigned long long)t->serial);
            }
            if (!clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) whole = 1;
            if (clean && atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && up_lost_take(pk->tex)) {
                clean = 0; whole = 1;
                uint64_t k = atomic_load(&g_up_lost_redo);
                if (k <= 3 || !atomic_load(&g_up_lost_n)) MLOG("re-uploading a texture whose upload was dropped ser=%llu (%llu in total, %d left in the record)", (unsigned long long)t->serial, (unsigned long long)k, atomic_load(&g_up_lost_n));
            }
            if (clean) {
                if (dirtycheck() && !kept && !pk->sparse && !pk->stored && full_hash(t->mem, t->L.size) != pk->hash) { g_pkg_miss++; pkg_miss_report(pk, t, now); }
                return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
            }
            if (!whole && !pk->sparse && pkg_edges_clean(pk, t)) return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
            // In-flight draws still read the old texture: switch to the spare when the GPU is done with it, else a new one.
            id<MTLTexture> reuse = nil;
            if (pk->spare) {
                pthread_mutex_lock(&g_mu);
                uint64_t done = g_done;
                pthread_mutex_unlock(&g_mu);
                if (pk->spare_fence <= done && pk->spare.usage == pk->tex.usage) reuse = pk->spare;
            }
            if (reuse && !own_object_free(reuse)) reuse = nil;   // serials that took it over still draw it
            uint64_t st, vg = nvn_sparse_gen(), b0 = g_bp_up_bytes, lg = atomic_load(&g_up_lost_gen), t0 = bp_now();
            unsigned taken = 0;
            id<MTLTexture> nt = pkg_upload(t, &st, reuse, pk->tex.usage, whole ? nil : pk->tex, pk->span, pk->stamp, &taken);
            reup_note(pk, t, taken, g_bp_up_bytes - b0, bp_now() - t0);
            if (nt) {
                id<MTLTexture> old = pk->tex;
                pk->tex = nt; pk->stored = 0; pk->part = taken != 0;
                if (!taken) pk->lost_gen = lg;
                if (pk->spare && pk->spare != nt) defer_obj_release(pk->spare);
                if (pk->sparse) sparse_view(pk, t);
                zb_report(t, pk->sparse ? (unsigned)pk->vbase : 0, "re-upload");
                pk->spare = old; pk->spare_fence = g_fence + 1;
                spare_note(pk);
                own_note(t->serial);
                pk->stamp = st; pk->vgen = pk->vseen = vg; pk->span = nvn_dirty_span(t->mem, t->L.size); pk->seen = st;
                g_pkg_reuploads++;
                pkg_hash_take(pk, t);
                pkg_edges_take(pk, t);
            }
            if (nt && !pk->sparse) bcp_up_follow(t, nt);   // the CPU's bytes reach the other side of a compressed-alias pair too
            return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
        }
        pk = [NvPkg new];
        pk->sparse = t->pool && nvn_sparse_pool(t->pool); pk->vgen = pk->vseen = nvn_sparse_gen();
        pk->used_tu = g_tu_now; pk->mem = (uintptr_t)t->mem; pk->size = t->L.size;
        pk->lost_gen = atomic_load(&g_up_lost_gen);
        pk->sub = g_sub_serial; pk->own = nvn_dirty_own_writes();
        pk->tex = pkg_upload(t, &pk->stamp, nil, 0, nil, NULL, 0, NULL);
        if (!pk->tex) return NULL;
        pk->span = nvn_dirty_span(t->mem, t->L.size);
        pk->seen = pk->stamp;   // a write that faulted during the upload moved "now" past the stamp
        g_pkg_uploads++;
        pkg_hash_take(pk, t);
        pkg_edges_take(pk, t);
        if (pk->sparse) sparse_view(pk, t);
        zb_report(t, pk->sparse ? (unsigned)pk->vbase : 0, "upload");
        tex_put(t->serial, pk);
        if (!pk->sparse) { pkg_share_put(t, pk); bcp_up_follow(t, pk->tex); }   // the CPU's bytes reach the other side of a compressed-alias pair too
        return tex_mtl_result(pk->view ? pk->view : pk->tex, owned);
    }
    // A view of a shared storage (tex_mtl_resolve's storage twins) whose parent left the sharing table no longer shares the storage's bytes: dropped, so no thread resolves to it.
    if (obj_is_tex(have) && ((id<MTLTexture>)have).parentTexture && own_view_drop_stale(t->serial, have)) {
        static int nstale;
        if (nstale++ < 4) MLOG("stale storage view dropped ser=%llu format %lu (%d so far)", (unsigned long long)t->serial, (unsigned long)((id<MTLTexture>)have).pixelFormat, nstale);
        have = nil;
    }
    if (!have && t->d.target == NVN_TEXTURE_TARGET_BUFFER) { alias_key key = tex_alias_key(t); have = own_alias_take(t->serial, &key); }
    if (have && !off && t->d.target == NVN_TEXTURE_TARGET_BUFFER && t->pool && nvn_sparse_pool(t->pool)) {
        // a BUFFER's serial and sharing entry still name its previous physical mapping: refreshed only when it changed
        void *mb = NULL; uint64_t mo = 0;
        int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
        if (!mtl_addr((uint64_t)(uintptr_t)t->mem, (uint64_t)t->d.width * (uint64_t)bpb, &mb, &mo)) return NULL;
        id<MTLTexture> root = have;
        while (root.parentTexture) root = root.parentTexture;
        NSUInteger al = tbuf_view_align(root.pixelFormat);
        if (root.buffer != (__bridge id<MTLBuffer>)mb || root.bufferOffset != mo - mo % al) {
            id<MTLTexture> fresh = tex_buffer(t);
            if (!fresh) return NULL;
            res_add(fresh);
            own_upgrade(have, fresh, t->serial, fresh); have = fresh;
        }
    }
    if (have) {
        if (!off && t->pool && nvn_sparse_pool(t->pool)) sparse_gpu_look(t, have);   // its mappings are synced on the submit thread
        if (!g_pool_walk) tu_stamp(have, 0);
        return tex_mtl_result(have, owned);
    }
    alias_key ak = tex_alias_key(t);
    id shared = own_alias_take(t->serial, &ak);   // found and published in one hold
    if (shared) {
        if (!g_pool_walk) tu_stamp(shared, 0);
        return tex_mtl_result(shared, owned);
    }
    // Shared by storage as every other texture is: buffer textures rebuilt every frame over the same ranges would otherwise add a view to the residency set each time.
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) {
        id<MTLTexture> mt = tex_buffer(t);
        if (!mt) return NULL;
        res_add(mt);
        id<MTLTexture> got = own_alias_publish(t->serial, &ak, mt);
        if (got != mt) defer_obj_release(mt);   // another thread registered one over the same storage meanwhile: that one is shared
        return tex_mtl_result(got, owned);
    }
    {   // textures over the same storage share its bytes; one of another format the two share (pf_storage_twins) is a view of the shared one
        uint32_t pf = 0; uint8_t swz[4];
        alias_key k;
        id<MTLTexture> b = nvn_fmt_to_mtl(t->d.format, &pf, swz) && pf ? storage_twin(&ak, (MTLPixelFormat)pf, &k) : nil, v = nil;
        MTLTextureDescriptor *d = b ? tex_desc(t, 0) : nil;
        if (!d || d.sampleCount > 1 || (d.textureType != MTLTextureType2D && d.textureType != MTLTextureType2DArray) || b.textureType != d.textureType) b = nil;
        // A view of another format than an sRGB / linear twin needs the format-view usage, which costs compression: the parent is made again with it on the submit thread
        // (tex_upgrade copies its content and renames it everywhere)
        // Not a resolution family's member or a storage pair's half: made again outside its heap, it would no longer alias the others
        int pfv = b && tex_need_pfv(b.pixelFormat, d.pixelFormat), up = pfv && !(b.usage & MTLTextureUsagePixelFormatView);
        int placed = up && (objc_getAssociatedObject(b, &g_image_heap_owner_key) || objc_getAssociatedObject(b, &g_pair_key));
        if (up && !off && !b.isSparse && !placed) b = tex_upgrade_pfv(t, b, d.pixelFormat);
        if (b && (!pfv || (b.usage & MTLTextureUsagePixelFormatView)))
            v = [b newTextureViewWithPixelFormat:d.pixelFormat textureType:b.textureType levels:NSMakeRange(0, b.mipmapLevelCount) slices:NSMakeRange(0, b.arrayLength) swizzle:b.swizzle];
        // b may have left the sharing table on another thread since the lookup: own_view_put checks that k still names b in the same hold of g_smu.
        if (v && !own_view_put(t->serial, &k, v)) v = nil;
        // A parent placed in a heap cannot be remade with the format-view usage (it would leave its heap, and its aliases with it). This texture is placed in the
        // same heap at the same offset instead: placement-heap aliasing shares the storage's bytes with the parent without a view (as a storage pair's halves do).
        id<MTLTexture> ht = nil;
        if (b && !v && placed) {
            id<MTLHeap> heap = b.heap;
            MTLTextureDescriptor *hd = [d copy];
            hd.allowGPUOptimizedContents = b.allowGPUOptimizedContents;   // aliased bytes are the same only uncompressed (a view of compressed storage is garbage too)
            MTLSizeAndAlign need = (!b.allowGPUOptimizedContents && heap && !b.heapOffset) ? [g_dev heapTextureSizeAndAlignWithDescriptor:hd] : (MTLSizeAndAlign){0};
            if (need.size && need.size <= heap.size) ht = [heap newTextureWithDescriptor:hd offset:0];
            if (ht) {
                id owner = objc_getAssociatedObject(b, &g_image_heap_owner_key);
                char *okey = &g_image_heap_owner_key;
                if (!owner) { owner = objc_getAssociatedObject(b, &g_pair_key); okey = &g_pair_key; }
                if (owner) objc_setAssociatedObject(ht, okey, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
                res_add(ht);
                static int nh;
                if (nh++ < 4) MLOG("texture ser=%llu %ux%u format %#x is placed in its storage's heap over the shared texture of format %lu, at its offset (%d so far): they share the storage's bytes without a view",
                                  (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, (unsigned long)b.pixelFormat, nh);
            }
        }
        if (b && !v && !ht) RED_ONCE(175, "texture ser=%llu %ux%u format %#x over the storage of a shared texture of format %lu: no view of it (%s), so it keeps its own and does not see what is written through the other",
                              (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, (unsigned long)b.pixelFormat, placed ? "its parent, placed in a heap with other textures over the storage, has no format-view usage" : up && (off || b.isSparse) ? "its parent needs the format-view usage, given only on the submit thread and not to a sparse texture" : "Metal made none, or the parent left the sharing table");
        if (ht) {
            id<MTLTexture> got = own_alias_publish(t->serial, &ak, ht);   // the parent's entry is held by its serial, so the takeover spares it
            if (got != ht) defer_obj_release(ht);
            if (!g_pool_walk) tu_stamp(got, 0);
            return tex_mtl_result(got, owned);
        }
        if (v) {
            static int nv;
            if (pfv && nv++ < 4) MLOG("texture ser=%llu %ux%u format %#x is a view of the shared texture over its storage, format %lu (%d so far)",
                                                                              (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, (unsigned long)b.pixelFormat, nv);
            if (!g_pool_walk) tu_stamp(v, 0);
            return tex_mtl_result(v, owned);
        }
    }
    // Unresolved and read off the submit thread: its storage holds its content. Made here, its first use on the submit thread (tex_cpu_content) would never come.
    if (off && in_storage && tex_storage_readable(t)) { *in_storage = 1; return NULL; }
    id<MTLTexture> mt = tex_linear(t); int tu = 0;
    if (!mt && t->pool && nvn_sparse_pool(t->pool)) mt = sparse_gpu_make(t);
    if (!mt) {
        MTLTextureDescriptor *d = tex_desc(t, 0);
        if (!d) return NULL;
        tu = (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && rh_fits(t, d.usage);
        mt = pair_texture(t, d);   // neither filled: their storage is shared
        if (!mt) mt = image_texture(t, d);
        if (!mt) {
            mt = rh_texture(t, d);
            if (!mt) mt = [g_dev newTextureWithDescriptor:d];
            if (mt) {
                res_add(mt);
                if (!bcp_up_fill(t, mt) && !tex_init_fill(t, mt)) tex_zero_fill(mt);
            }
        }
        if (!mt) { RED_ONCE(2, "failed to create MTLTexture %ux%u format %#x target %d", t->d.width, t->d.height, t->d.format, t->d.target); return NULL; }
    }
    if (mt.allocatedSize >= (256u << 20)) {   // a GPU out-of-memory error says nothing of which textures took the memory
        static int nb;
        if (nb++ < 20) MLOG("large texture ser=%llu %ux%ux%u format %#x target %d levels %u: %llu MB%s", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels,
             (unsigned long long)(mt.allocatedSize >> 20), t->pool && nvn_sparse_pool(t->pool) ? ", in a sparse virtual pool" : t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) ? ", in a virtual pool" : "");
    }
    if (g_allocprof > 0) {
        static int n;
        if (n++ < 30 || n % 500 == 0) MLOG("new texture (%s) #%d ser=%llu %ux%ux%u format %#x target %d levels %u storage %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem);
    }
    if (getenv("DM_NVN_TEXCPU") && t->pool && !(t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) MLOG("non-swizzled texture on a CPU-visible pool ser=%llu %ux%ux%u format %#x target %d levels %u texture flags %#x pool flags %#x storage %p", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, t->d.flags, t->pool->flags, (void *)t->mem);
    res_add(mt);
    // A texture another thread registered under the key since the lookup is the one the storage's serials share: this one is retired.
    id<MTLTexture> got = own_alias_publish(t->serial, &ak, mt);
    if (got != mt) {
        static int nrace;
        if (nrace++ < 4) MLOG("texture ser=%llu made twice over one storage: the first one registered is shared (%d so far)", (unsigned long long)t->serial, nrace);
        defer_obj_release(mt);
    }
    else if (tu) tu_register(mt, t->serial);
    return tex_mtl_result(got, owned);
}
static void top_of(const uint64_t v[64], int *top, int n) {
    for (int k = 0; k < n; k++) top[k] = -1;
    for (int i = 0; i < 64; i++)
        for (int k = 0; k < n; k++)
            if (top[k] < 0 || v[i] > v[top[k]]) {
                for (int j = n - 1; j > k; j--) top[j] = top[j - 1];
                top[k] = i;
                break;
            }
}
// The system kills a process for memory with no notice: only readouts left beforehand show which number kept climbing. Integers only: the frame hook runs on a small guest stack.
static _Atomic uint64_t g_gptr_bindings, g_gptr_reads, g_gptr_added, g_gptr_bytes;
void mtl_mem_stat(unsigned long long frame, unsigned long long rss_mb) {
    static unsigned long long last_rss, last_gpu, last_frame, last_wired;
    static int lines;
    unsigned long long gpu = (unsigned long long)(g_dev.currentAllocatedSize >> 20);
    sys_mem_t sm = sys_mem();
    unsigned long long dr = rss_mb > last_rss ? rss_mb - last_rss : last_rss - rss_mb;
    unsigned long long dg = gpu > last_gpu ? gpu - last_gpu : last_gpu - gpu;
    unsigned long long dw = sm.wired > last_wired ? sm.wired - last_wired : last_wired - sm.wired;
    // not on low free memory alone: the Mac keeps little free
    static uint64_t last_ev;
    uint64_t ev = atomic_load(&g_mp_n[0]) + atomic_load(&g_mp_n[1]) + atomic_load(&g_mp_n[2]);
    int sparse = lines >= 400;
    if (lines && frame - last_frame < 3000 && (sparse || (dr < 64 && dg < 64 && dw < 256 && ev == last_ev))) return;
    if (!sparse) lines++;
    last_rss = rss_mb; last_gpu = gpu; last_frame = frame; last_wired = sm.wired; last_ev = ev;
    pthread_mutex_lock(&g_deferred_mu); unsigned long nd = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    pthread_mutex_lock(&g_res_mu); unsigned long nr = (unsigned long)g_res_objects.count; pthread_mutex_unlock(&g_res_mu);
    int live = 0, peak = 0, cap = 0;
    nvn_dirty_stat(&live, &peak, &cap);
    pthread_mutex_lock(&g_smu);
    own_stats os = own_stats_locked();
    own_alias_stats as = own_alias_stats_locked();
    unsigned ns = os.live, ashared = as.n, nghost = os.ghost, nwait = os.retire_wait;
    uint64_t ghost_puts = os.ghost_puts;
    own_alias_unheld au = own_alias_unheld_locked();
    pthread_mutex_unlock(&g_smu);
    unsigned long dead_n = 0;
    unsigned long long dead_mb = 0;
    // objects read from a weak table come back autoreleased, and the guest thread's pool never drains
    int pres = 0;
    @autoreleasepool {
        pthread_mutex_lock(&g_mu);
        pres = g_pool_res_n;
        for (id<MTLBuffer> b in g_dead_pools) { dead_n++; dead_mb += b.length; }
        pthread_mutex_unlock(&g_mu);
    }
    dead_mb >>= 20;
    // os_proc_available_memory has no answer on macOS: the readout writes "none"
    os_unfair_lock_lock(&g_rh_lk); unsigned rh_n = g_rh_n; uint64_t rh_bytes = g_rh_bytes; os_unfair_lock_unlock(&g_rh_lk);
    char avail[32] = "none";
    MLOG("memory up to frame %llu: resident %llu MB, GPU allocated %llu MB, deferred-release queue %lu, residency set %lu, serial objects %u, memory pools %d (resident %d; peak %d / tracking table capacity %d; freed but alive %lu, %llu MB), shared textures %u (%u kept by the sharing table alone, %llu MB, %llu MB of it drawn into or shader-written, the oldest idle %llu submissions; evicted with pools %llu (serial entries that went with them %llu), on storage takeover %llu in total), texture re-uploads %llu in total, per-frame arena blocks created %llu in total (alive %llu, %llu MB; released when idle %llu, %llu MB in total), finalized textures still referenced %u (small table, %llu put in total), retired but held by a serial %u, recycled texture heaps made %llu, reused %llu, freed %llu, waiting %u (%llu MB); headroom to the system memory limit %s", frame, rss_mb, gpu, nd, nr, ns, live, pres, peak, cap, dead_n, dead_mb, ashared, au.entries, (unsigned long long)(au.bytes >> 20), (unsigned long long)(au.writable_bytes >> 20), (unsigned long long)au.oldest_idle, (unsigned long long)as.evicted, (unsigned long long)as.pool_entries, (unsigned long long)as.taken, (unsigned long long)g_pkg_reuploads, (unsigned long long)g_arena_made, (unsigned long long)g_arena_live, (unsigned long long)(g_arena_live_bytes >> 20), (unsigned long long)g_arena_freed, (unsigned long long)(g_arena_freed_bytes >> 20), nghost, (unsigned long long)ghost_puts, nwait,
         (unsigned long long)g_rh_made, (unsigned long long)g_rh_reused, (unsigned long long)g_rh_trimmed, rh_n, (unsigned long long)(rh_bytes >> 20), avail);
    if (sm.ok) {
        char pl[220];
        mem_pressure_said(pl, sizeof pl);
        MLOG("system memory at frame %llu: free %llu MB, wired %llu MB, compressed %llu MB, file-backed %llu MB, purgeable %llu MB, of %llu MB; residency set %llu MB; %s", frame,
             (unsigned long long)sm.free, (unsigned long long)sm.wired, (unsigned long long)sm.compressed, (unsigned long long)sm.file, (unsigned long long)sm.purgeable, (unsigned long long)sm.total,
             (unsigned long long)(g_res ? g_res.allocatedSize >> 20 : 0), pl);
    }
    {
        void mtl_vc_stats(unsigned *, uint64_t *, uint64_t *), mtl_tb2_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *, uint64_t *);
        uint64_t mtl_tex_mis_count(void);
        unsigned vn;
        uint64_t vh, vm, tb, tm, tf, tc, tcb;
        mtl_vc_stats(&vn, &vh, &vm); mtl_tb2_stats(&tb, &tm, &tf, &tc, &tcb);
        MLOG("texture views by Metal texture (vc_get): %u textures hold cached views; %llu views reused, %llu made in total; texture buffers bound as 2D rows for wrapped 1D fetches %llu times, %llu made, %llu failed, %llu copied into aligned rows (%llu MB) "
           "(tbuf_as_2d); texture type mismatches no view bridges %llu in total (tex_mis_note)",
           vn, (unsigned long long)vh, (unsigned long long)vm, (unsigned long long)tb, (unsigned long long)tm, (unsigned long long)tf, (unsigned long long)tc, (unsigned long long)(tcb >> 20), (unsigned long long)mtl_tex_mis_count());
    }
    if (g_allocprof > 0) {
        char ab[256];
        int o = 0;
        for (int i = 0; i < g_alloc_n && o < 200; i++) o += snprintf(ab + o, sizeof ab - o, " #%d %llu MB", i, (unsigned long long)(g_alloc4[i].allocatedSize >> 20));
        for (int i = 0; i < 3 && o < 240; i++) o += snprintf(ab + o, sizeof ab - o, " up%d %llu pres%d %llu", i, (unsigned long long)(g_up_alloc4[i].allocatedSize >> 20), i, (unsigned long long)(g_present_alloc4[i].allocatedSize >> 20));
        MLOG("command allocator usage:%s", ab);
        if (g_allocprof == 2 && g_cm_sub_n) {
            int top[6];
            char cl[400];
            int co = 0;
            top_of(g_cm_op, top, 6);
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
                 (unsigned long long)g_md_masked_calls, (unsigned long long)g_md_masked, (unsigned long long)g_md_counted_calls, (unsigned long long)g_md_counted);
        }
        MLOG("GPU allocation growth by function: pass end %llu MB, pass begin %llu MB, pipeline fetch %llu MB, argument buffers %llu MB, texture fetch %llu MB", g_ap_fn[0] >> 20, g_ap_fn[1] >> 20, g_ap_fn[2] >> 20, g_ap_fn[3] >> 20, g_ap_fn[4] >> 20);
    }
    if (g_allocprof > 0) {
        int top[4];
        top_of(g_ap_bytes, top, 4);
        MLOG("GPU allocation growth by op: op%d %llu MB, op%d %llu MB, op%d %llu MB, op%d %llu MB", top[0], g_ap_bytes[top[0]] >> 20, top[1], g_ap_bytes[top[1]] >> 20, top[2], g_ap_bytes[top[2]] >> 20, top[3], g_ap_bytes[top[3]] >> 20);
    }
    {
        void mtl_gmem_words(uint64_t *, uint64_t *, uint64_t *);
        uint64_t gw, go, gb;
        mtl_gmem_words(&gw, &go, &gb);
        if (gw) MLOG("raw pool table words at commit: %llu, of them %llu held an older table than the current one and %llu the current one behind the pools", (unsigned long long)gw, (unsigned long long)go, (unsigned long long)gb);
    }
    if (g_gptr_bindings) MLOG("raw pointer discovery up to frame %llu: %llu bindings, %llu pointer words read, %llu previously unaddressed ranges (%llu logical MB)", frame,
                            (unsigned long long)g_gptr_bindings, (unsigned long long)g_gptr_reads, (unsigned long long)g_gptr_added, (unsigned long long)(g_gptr_bytes >> 20));
}
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = g_pkg_uploads; *reuploads = g_pkg_reuploads; }
void mtl_spare_stats(uint64_t *held, uint64_t *trimmed, uint64_t *trimmed_mb) { *held = atomic_load(&g_spare_held); *trimmed = atomic_load(&g_spare_trimmed); *trimmed_mb = atomic_load(&g_spare_trimmed_bytes) >> 20; }
uint64_t mtl_pkg_edge_skips(void) { return g_pkg_edge_skips; }
uint64_t mtl_pkg_miss(void) { return g_pkg_miss; }
static void treg_retire_serial(uint64_t ser);
int mtl_tex_release(tex_t *t) {
    if (!t->serial) return 1;
    pthread_mutex_lock(&g_smu);
    int was_live = own_tex_end_locked(t);   // -1: a copy's Finalize, the texture's own object still holds it
    if (was_live < 0) { pthread_mutex_unlock(&g_smu); return 0; }
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
    // NVNtexture is plain data, so a game may finalize several copies: only the first release retires the serial's descriptors.
    if (was_live) treg_retire_serial(t->serial);
    return 1;
}
