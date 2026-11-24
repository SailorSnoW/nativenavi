// Large sparse textures as Metal placement sparse textures: NVN's 64 KB tiles have the shape of Metal's, so a fully mapped tile gets a heap tile
// (docs/reference/nvn-objects.md, Large sparse textures as Metal placement sparse textures). Smaller ones keep the whole upload.
#define SP_TILE 0x10000u
#define SP_HEAP_TILES 4096u
#define SP_MIN (64ull << 20)
int nvn_sparse_tile_state(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp);
const uint8_t *nvn_sparse_tile_phys(const void *pool, uint64_t a);
uint64_t nvn_sparse_log_seq(const void *pool);
int nvn_sparse_log_read(const void *pool, uint64_t *seq, uint64_t (*out)[2], int max);
void nvn_sparse_protect_tile(const void *pool, uint64_t a, uint64_t n);
static pthread_mutex_t g_sp_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableArray *g_sp_heaps;     // a slot is heap × SP_HEAP_TILES + tile
static uint32_t *g_sp_free;
static size_t g_sp_nfree, g_sp_freecap;
static struct sp_late { uint32_t slot; uint64_t fence; } *g_sp_late;   // freed once the GPU is past the fence
static size_t g_sp_nlate, g_sp_latecap;
static uint64_t g_sp_live, g_sp_peak;
typedef struct sp_tl { uintptr_t p; uint64_t stamp; uint32_t rc; int fmt; uint8_t drawn; } sp_tl;
static sp_tl *sp_tl_of(uint32_t slot);
static int sp_slot(uint32_t *out) {   // caller holds g_sp_mu
    uint64_t done = atomic_load(&g_done); size_t k = 0;
    for (size_t i = 0; i < g_sp_nlate; i++) {
        if (g_sp_late[i].fence > done) { g_sp_late[k++] = g_sp_late[i]; continue; }
        if (g_sp_nfree == g_sp_freecap) {
            g_sp_freecap = g_sp_freecap ? 2 * g_sp_freecap : 4096;
            g_sp_free = dm_realloc(g_sp_free, g_sp_freecap * sizeof *g_sp_free, "the sparse tile list");
        }
        g_sp_free[g_sp_nfree++] = g_sp_late[i].slot;
    }
    g_sp_nlate = k;
    if (!g_sp_nfree) {
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate;
        hd.size = (NSUInteger)SP_HEAP_TILES * SP_TILE; hd.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
        id<MTLHeap> h = [g_dev newHeapWithDescriptor:hd];
        if (!h) { RED_ONCE(144, "cannot make a 256 MB placement heap for sparse tiles (%llu tiles mapped)", (unsigned long long)g_sp_live); return 0; }
        if (!g_sp_heaps) g_sp_heaps = [NSMutableArray array];
        uint32_t base = (uint32_t)g_sp_heaps.count * SP_HEAP_TILES;
        [g_sp_heaps addObject:h];
        res_add(h);
        if (g_sp_freecap < g_sp_nfree + SP_HEAP_TILES) {
            g_sp_freecap = g_sp_nfree + SP_HEAP_TILES;
            g_sp_free = dm_realloc(g_sp_free, g_sp_freecap * sizeof *g_sp_free, "the sparse tile list");
        }
        for (uint32_t i = SP_HEAP_TILES; i-- > 0;) g_sp_free[g_sp_nfree++] = base + i;
        MLOG("sparse tiles: placement heap %lu (256 MB), %llu tiles mapped", (unsigned long)g_sp_heaps.count, (unsigned long long)g_sp_live);
    }
    *out = g_sp_free[--g_sp_nfree];
    if (++g_sp_live > g_sp_peak) g_sp_peak = g_sp_live;
    *sp_tl_of(*out) = (sp_tl){.rc = 1};
    return 1;
}
static void sp_slot_free(uint32_t slot) {   // caller holds g_sp_mu; the GPU may still read the tile
    if (g_sp_nlate == g_sp_latecap) {
        g_sp_latecap = g_sp_latecap ? 2 * g_sp_latecap : 1024;
        g_sp_late = dm_realloc(g_sp_late, g_sp_latecap * sizeof *g_sp_late, "the sparse tile list");
    }
    g_sp_late[g_sp_nlate++] = (struct sp_late){slot, atomic_load(&g_fence) + 1};
    g_sp_live--;
}
// A tile of 16 consecutive physical pages shares the heap tile of that memory, as the page table does; until the GPU draws through it a slot is a copy of the memory,
// after that it holds what the memory does not (docs/reference/nvn-objects.md, Physical pages mapped at several places of a placement sparse texture).
static sp_tl *g_sp_tl;
static size_t g_sp_ntl;
static uint32_t *g_sp_ph;   // physical memory and format → slot + 1, linear probing
static size_t g_sp_phcap, g_sp_phn;
static uint64_t g_sp_shared;
static sp_tl *sp_tl_of(uint32_t slot) {   // caller holds g_sp_mu
    if (slot >= g_sp_ntl) {
        size_t n = ((size_t)slot / SP_HEAP_TILES + 1) * SP_HEAP_TILES;
        g_sp_tl = dm_realloc(g_sp_tl, n * sizeof *g_sp_tl, "the sparse tile table");
        memset(g_sp_tl + g_sp_ntl, 0, (n - g_sp_ntl) * sizeof *g_sp_tl);
        g_sp_ntl = n;
    }
    return &g_sp_tl[slot];
}
static size_t sp_ph_at(uintptr_t p, int fmt) { return (size_t)((((uint64_t)p >> 16) ^ (uint64_t)fmt * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 20) & (g_sp_phcap - 1); }
static uint32_t sp_ph_find(uintptr_t p, int fmt) {   // caller holds g_sp_mu; → slot, or ~0u
    if (!g_sp_phcap) return ~0u;
    for (size_t i = sp_ph_at(p, fmt);; i = (i + 1) & (g_sp_phcap - 1)) {
        uint32_t e = g_sp_ph[i];
        if (!e) return ~0u;
        if (g_sp_tl[e - 1].p == p && g_sp_tl[e - 1].fmt == fmt) return e - 1;
    }
}
static void sp_ph_put(uint32_t slot) {   // caller holds g_sp_mu
    if (2 * (g_sp_phn + 1) > g_sp_phcap) {
        uint32_t *old = g_sp_ph;
        size_t oc = g_sp_phcap;
        g_sp_phcap = oc ? 2 * oc : 4096;
        if (!(g_sp_ph = calloc(g_sp_phcap, sizeof *g_sp_ph))) MDIE("the shared sparse tile table");
        for (size_t i = 0; i < oc; i++) {
            if (!old[i]) continue;
            size_t j = sp_ph_at(g_sp_tl[old[i] - 1].p, g_sp_tl[old[i] - 1].fmt);
            while (g_sp_ph[j]) j = (j + 1) & (g_sp_phcap - 1);
            g_sp_ph[j] = old[i];
        }
        free(old);
    }
    size_t j = sp_ph_at(g_sp_tl[slot].p, g_sp_tl[slot].fmt);
    while (g_sp_ph[j]) j = (j + 1) & (g_sp_phcap - 1);
    g_sp_ph[j] = slot + 1; g_sp_phn++; g_sp_shared++;
}
static void sp_ph_del(uint32_t slot) {   // caller holds g_sp_mu
    size_t i = sp_ph_at(g_sp_tl[slot].p, g_sp_tl[slot].fmt);
    while (g_sp_ph[i] && g_sp_ph[i] != slot + 1) i = (i + 1) & (g_sp_phcap - 1);
    if (!g_sp_ph[i]) return;
    g_sp_ph[i] = 0; g_sp_phn--; g_sp_shared--;
    for (size_t j = (i + 1) & (g_sp_phcap - 1); g_sp_ph[j]; j = (j + 1) & (g_sp_phcap - 1)) {
        size_t h = sp_ph_at(g_sp_tl[g_sp_ph[j] - 1].p, g_sp_tl[g_sp_ph[j] - 1].fmt);
        if ((j > i && (h <= i || h > j)) || (j < i && h <= i && h > j)) {
            g_sp_ph[i] = g_sp_ph[j];
            g_sp_ph[j] = 0;
            i = j;
        }
    }
}
static void sp_release(uint32_t slot) {   // caller holds g_sp_mu
    sp_tl *e = sp_tl_of(slot);
    if (e->rc && --e->rc) return;
    if (e->p && e->drawn) return;   // GPU content only this slot holds: kept for the next place mapping that memory
    if (e->p) sp_ph_del(slot);
    *e = (sp_tl){0}; sp_slot_free(slot);
}
void mtl_sp_phys_gone(const void *mem, uint64_t size) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    pthread_mutex_lock(&g_sp_mu);
    for (size_t i = 0; g_sp_phn && i < g_sp_phcap; i++) {
        uint32_t e = g_sp_ph[i];
        if (!e) continue;
        sp_tl *t = &g_sp_tl[e - 1];
        if (t->p < lo || t->p >= hi) continue;
        sp_ph_del(e - 1);
        i--;   // the deletion may shift a later entry into slot i
        if (t->rc) {
            t->p = 0; t->drawn = 0;
        } else {
            *t = (sp_tl){0};
            sp_slot_free(e - 1);
        }
    }
    pthread_mutex_unlock(&g_sp_mu);
}

@interface NvSparse : NSObject { @public
    __weak id<MTLTexture> tex; uint32_t tw, th, ntiled, levels, layers, per_layer, tail_at;
    uint32_t tiles_x[16], tiles_y[16], loff_t[16]; uint64_t loff[16], tail_off, layer_size;
    uint32_t *slot;                    // heap slot + 1, 0 = not mapped
    uint64_t *nmap;
    uint64_t gen, stamp;
    int gpu;                           // GPU-written: tiles are mapped and unmapped, nothing is uploaded
    uint64_t seq;
    uint64_t wseen, wframe, wown;
    uint8_t undrawn;
} @end   // tex is weak: an NvPkg owns a packaged texture, a GPU texture owns its NvSparse
@implementation NvSparse
- (void)dealloc {
    pthread_mutex_lock(&g_sp_mu);
    for (uint32_t i = 0; slot && i < layers * per_layer; i++) if (slot[i]) sp_release(slot[i] - 1);
    pthread_mutex_unlock(&g_sp_mu);
    free(slot); free(nmap);
}
@end

static NvSparse *sparse_make(const tex_t *t, int gpu, __strong id<MTLTexture> *made) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DM_NVN_SPARSE_TILES");
        on = !e || atoi(e);
    }
    if (!on || !t->pool || !nvn_sparse_pool(t->pool) || !t->mem) return nil;
    if ((t->d.target != NVN_TEXTURE_TARGET_2D && t->d.target != NVN_TEXTURE_TARGET_2D_ARRAY) || t->d.samples > 1 || rb_packed_shift(t->d.format)) return nil;
    uint32_t tw, th, td; int n = nvn_tex_sparse_tiles(&t->d, &tw, &th, &td);
    if (n <= 0 || n > 15 || td != 1 || t->d.levels > 16) return nil;
    MTLTextureDescriptor *d = tex_desc(t, !gpu);
    if (!d) return nil;
    MTLSize ms = [g_dev sparseTileSizeWithTextureType:d.textureType pixelFormat:d.pixelFormat sampleCount:1 sparsePageSize:MTLSparsePageSize64];
    if (ms.width != tw || ms.height != th || ms.depth != 1) {
        static int nl;
        if (nl++ < 4) MLOG("sparse texture ser=%llu format %#x: Metal's tile %lux%lu is not NVN's %ux%u, made whole", (unsigned long long)t->serial, t->d.format, (unsigned long)ms.width, (unsigned long)ms.height, tw, th);
        return nil;
    }
    MTLSizeAndAlign sa = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    if (sa.size < SP_MIN) return nil;
    NvSparse *s = [NvSparse new];
    s->gpu = gpu; s->tw = tw; s->th = th; s->ntiled = (uint32_t)n;
    s->levels = (uint32_t)d.mipmapLevelCount; s->layers = (uint32_t)MAX(d.arrayLength, 1u); s->layer_size = t->L.layer_size;
    uint32_t k = 0;
    for (uint32_t l = 0; l < s->ntiled; l++) {
        uint32_t w, h; tex_lsize(t, l, &w, &h, NULL);
        s->tiles_x[l] = (w + tw - 1) / tw; s->tiles_y[l] = (h + th - 1) / th;
        s->loff_t[l] = k;
        k += s->tiles_x[l] * s->tiles_y[l];
        s->loff[l] = nvn_tex_level_offset(&t->d, l);
        uint64_t next = l + 1 < s->levels ? nvn_tex_level_offset(&t->d, l + 1) : s->layer_size;
        if (next - s->loff[l] != (uint64_t)s->tiles_x[l] * s->tiles_y[l] * SP_TILE) {
            static int nl;
            if (nl++ < 4) MLOG("sparse texture ser=%llu: level %u is not whole tiles (%#llx bytes), made whole", (unsigned long long)t->serial, l, (unsigned long long)(next - s->loff[l]));
            return nil;
        }
    }
    s->tail_at = k; s->per_layer = k + (s->ntiled < s->levels);
    s->tail_off = s->ntiled < s->levels ? nvn_tex_level_offset(&t->d, s->ntiled) : s->layer_size;
    // Every usage up front: an upgrade (tex_upgrade) would remake the texture whole.
    d.placementSparsePageSize = MTLSparsePageSize64; d.allowGPUOptimizedContents = NO;
    d.usage |= MTLTextureUsagePixelFormatView | (fmt_compressed(t->d.format) ? 0 : MTLTextureUsageRenderTarget);
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(145, "cannot make a placement sparse texture %ux%ux%u format %#x", t->d.width, t->d.height, t->d.depth, t->d.format); return nil; }
    if (s->ntiled < s->levels && (mt.firstMipmapInTail != s->ntiled || mt.tailSizeInBytes != SP_TILE)) {   // with every level tiled, Metal's tail lies past the last level
        static int nl;
        if (nl++ < 4) MLOG("sparse texture ser=%llu: Metal's tail starts at level %lu (%lu bytes), NVN's at %u, made whole", (unsigned long long)t->serial, (unsigned long)mt.firstMipmapInTail, (unsigned long)mt.tailSizeInBytes, s->ntiled);
        return nil;
    }
    s->tex = mt; *made = mt; res_add(mt);
    s->slot = calloc((size_t)s->layers * s->per_layer, sizeof *s->slot);
    s->nmap = calloc(s->ntiled + 1, sizeof *s->nmap);
    if (!s->slot || !s->nmap) MDIE("sparse texture tile table");
    s->gen = 0; s->stamp = 0;
    static int nl;
    if (nl++ < 20) MLOG("sparse texture ser=%llu %ux%ux%u format %#x levels %u: placement sparse%s, %u tiles a layer (%llu MB made whole)", (unsigned long long)t->serial, t->d.width, t->d.height, s->layers,
                        t->d.format, s->levels, gpu ? " (GPU-written)" : "", s->per_layer, (unsigned long long)(sa.size >> 20));
    return s;
}

typedef struct { id<MTLHeap> heap; MTL4UpdateSparseTextureMappingOperation *op; size_t n, cap; } sp_ops;
static void sp_op(sp_ops *o, MTL4UpdateSparseTextureMappingOperation x) {
    if (o->n == o->cap) {
        o->cap = o->cap ? 2 * o->cap : 64;
        o->op = dm_realloc(o->op, o->cap * sizeof *o->op, "the sparse mapping operations");
    }
    o->op[o->n++] = x;
}
typedef struct {
    NvSparse *s; const tex_t *t; id<MTLTexture> tex;
    sp_ops *ops; uint32_t nheaps;
    uint8_t *tile, *tail; id<MTLBuffer> stg; size_t stg_off;
    uint64_t up, unmapped, stamp; int bw, bh, bpb, force;
} sp_look;
static void sp_stg(sp_look *c, size_t n) {
    if (c->stg && c->stg_off + n <= c->stg.length) return;
    c->stg = [g_dev newBufferWithLength:MAX((size_t)8 << 20, n) options:MTLResourceStorageModeShared];
    if (!c->stg) MDIE("sparse upload staging");
    up_begin(); res_add(c->stg); [g_up_keep addObject:c->stg]; c->stg_off = 0;
}
// caller holds g_sp_mu
static void sp_tile(sp_look *c, uint32_t y, uint32_t i) {
    NvSparse *s = c->s; const tex_t *t = c->t;
    int is_tail = i == s->tail_at; uint32_t l = 0, tx = 0, ty = 0;
    if (!is_tail) {
        while (l + 1 < s->ntiled && i >= s->loff_t[l + 1]) l++;
        uint32_t j = i - s->loff_t[l];
        tx = j % s->tiles_x[l]; ty = j / s->tiles_x[l];
    }
    uint64_t a = (uint64_t)(uintptr_t)t->mem + y * s->layer_size + (is_tail ? s->tail_off : s->loff[l] + (uint64_t)(i - s->loff_t[l]) * SP_TILE);
    uint64_t n = is_tail ? s->layer_size - s->tail_off : SP_TILE;
    uint32_t *sl = &s->slot[(size_t)y * s->per_layer + i], lev = is_tail ? s->ntiled : l;
    MTLRegion rg = is_tail ? MTLRegionMake3D(0, 0, 0, 1, 1, 1) : MTLRegionMake3D(tx, ty, 0, 1, 1, 1);
    int st = nvn_sparse_tile_state(t->pool, a, n, s->gen, s->stamp); if (st < 0) st = 0;
    if (!(st & 1)) {
        if (*sl) {
            sp_op(&c->ops[c->nheaps], (MTL4UpdateSparseTextureMappingOperation){MTLSparseTextureMappingModeUnmap, rg, lev, y, 0});
            sp_release(*sl - 1); *sl = 0; s->nmap[lev]--;
            c->unmapped++;
        }
        return;
    }
    uint32_t lw, lh, x0 = tx * s->tw, y0 = ty * s->th; tex_lsize(t, l, &lw, &lh, NULL);
    const uint8_t *p = is_tail || x0 + s->tw > lw || y0 + s->th > lh ? NULL : nvn_sparse_tile_phys(t->pool, a);
    if (*sl) {
        const sp_tl *e = sp_tl_of(*sl - 1);
        if (e->p != (uintptr_t)p || (p && e->fmt != t->d.format)) {   // unmaps go first (sparse_sync)
            sp_op(&c->ops[c->nheaps], (MTL4UpdateSparseTextureMappingOperation){MTLSparseTextureMappingModeUnmap, rg, lev, y, 0});
            sp_release(*sl - 1); *sl = 0; s->nmap[lev]--;
        } else if (s->gpu) return;
        else if (e->drawn ? !nvn_dirty_since(p, SP_TILE, e->stamp) : !c->force && !(p ? nvn_dirty_since(p, SP_TILE, e->stamp) : (st & 2))) return;
    }
    if (!*sl) {
        uint32_t slot = p ? sp_ph_find((uintptr_t)p, t->d.format) : ~0u; int fresh = slot == ~0u;
        if (fresh) { if (!sp_slot(&slot)) return; }
        else sp_tl_of(slot)->rc++;
        uint32_t hi = slot / SP_HEAP_TILES;
        if (hi >= c->nheaps) {
            RED_ONCE(146, "sparse tiles: more than 64 heaps made in one look");
            sp_release(slot);
            return;
        }
        c->ops[hi].heap = g_sp_heaps[hi];
        sp_op(&c->ops[hi], (MTL4UpdateSparseTextureMappingOperation){MTLSparseTextureMappingModeMap, rg, lev, y, slot % SP_HEAP_TILES});
        *sl = slot + 1; s->nmap[lev]++; s->undrawn = 1;
        if (fresh && p) {
            sp_tl *e = sp_tl_of(slot);
            e->p = (uintptr_t)p; e->fmt = t->d.format; e->stamp = nvn_dirty_now();
            sp_ph_put(slot);
        }
        if (s->gpu) {
            if (fresh && p) nvn_dirty_protect_sparse(p, SP_TILE);
            c->up++;
            return;
        }
        if (!fresh && sp_tl_of(slot)->drawn && !nvn_dirty_since(p, SP_TILE, sp_tl_of(slot)->stamp)) return;
    }
    // Protected before it is read so a later CPU write shows. A shared slot's is read from p, not the page table: the game may remap this place on its own thread at any moment.
    if (p) nvn_dirty_protect_sparse(p, SP_TILE);
    else nvn_sparse_protect_tile(t->pool, a, n);
    int bw = c->bw, bh = c->bh, bpb = c->bpb;
    if (is_tail) {
        c->tail = dm_realloc(c->tail, n, "the sparse tail copy");
        nvn_sparse_copy(t->pool, c->tail, a, n);
        for (uint32_t tl = s->ntiled; tl < s->levels; tl++) {
            uint32_t w, h;
            tex_lsize(t, tl, &w, &h, NULL);
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            sp_stg(c, pitch * rows);
            if (nvn_deswizzle_from(&t->d, tl, y, c->tail, y * s->layer_size + s->tail_off, (uint8_t *)c->stg.contents + c->stg_off, pitch)) { RED_ONCE(147, "sparse tail level %u of format %#x not decoded", tl, t->d.format); break; }
            up_blit_add_at(c->stg, c->stg_off, pitch, pitch * rows, MTLSizeMake(w, h, 1), c->tex, y, tl, MTLOriginMake(0, 0, 0));
            c->stg_off += (pitch * rows + 255) & ~(size_t)255;
        }
    } else {
        if (p) memcpy(c->tile, p, SP_TILE);
        else nvn_sparse_copy(t->pool, c->tile, a, SP_TILE);
        sp_stg(c, SP_TILE);
        uint32_t wb, rows; size_t tpitch = (size_t)(s->tw / bw) * bpb;
        if (nvn_deswizzle_tile(&t->d, l, tx, ty, c->tile, (uint8_t *)c->stg.contents + c->stg_off, tpitch, &wb, &rows)) { RED_ONCE(148, "sparse tile of format %#x not decoded", t->d.format); return; }
        if (wb && rows) up_blit_add_at(c->stg, c->stg_off, tpitch, tpitch * rows, MTLSizeMake(MIN(s->tw, lw - x0), MIN(s->th, lh - y0), 1), c->tex, y, l, MTLOriginMake(x0, y0, 0));
        c->stg_off += SP_TILE;
        if (p) sp_tl_of(*sl - 1)->stamp = c->stamp;   // taken before the protection above
    }
    c->up++;
}
static void sp_range(sp_look *c, uint64_t lo, uint64_t hi) {
    NvSparse *s = c->s; uint64_t base = (uint64_t)(uintptr_t)c->t->mem, end = base + (uint64_t)s->layers * s->layer_size;
    if (lo < base) lo = base;
    if (hi > end) hi = end;
    for (uint64_t o = (lo - base) & ~(uint64_t)(SP_TILE - 1); lo < hi && base + o < hi;) {
        uint32_t y = (uint32_t)(o / s->layer_size); uint64_t in = o % s->layer_size;
        if (in >= s->tail_off) { sp_tile(c, y, s->tail_at); o += s->layer_size - in; continue; }
        uint32_t l = 0; while (l + 1 < s->ntiled && in >= s->loff[l + 1]) l++;
        sp_tile(c, y, s->loff_t[l] + (uint32_t)((in - s->loff[l]) / SP_TILE));
        o += SP_TILE;
    }
}
static int sp_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
// whole 0: the tiles of the requests since the last look (all once the pool's log moved past them); 1: all; 2: only newly mapped tiles with a changed page;
// 3: all of those again, an upload batch having been dropped. The mapping operations go to the queue ahead of the upload batch.
static void sparse_sync(NvSparse *s, const tex_t *t, int whole) {
    id<MTLTexture> stex = s->tex;
    if (!stex) return;
    if (!s->gpu) tex_cpu_wait(t);   // before the CPU snapshots a tile, and before taking its stamp
    if (whole) s->wown = nvn_dirty_own_writes();
    enum { RANGES = 4096 };
    uint64_t (*ranges)[2] = NULL;
    int nr = -1;
    uint64_t gen = nvn_sparse_gen(), stamp = nvn_dirty_now();
    if (whole == 1) s->seq = nvn_sparse_log_seq(t->pool);
    else if (whole >= 2) { s->wseen = nvn_dirty_sparse_writes(); s->wframe = atomic_load(&g_cur_frame); }
    else {
        if (!(ranges = malloc(RANGES * sizeof *ranges))) MDIE("sparse mapping ranges");
        nr = nvn_sparse_log_read(t->pool, &s->seq, ranges, RANGES);
        if (!nr) {
            free(ranges);
            return;
        }
    }
    sp_look c = {.s = s, .t = t, .tex = stex, .nheaps = (uint32_t)g_sp_heaps.count + 64, .force = whole == 3, .stamp = stamp};
    c.bpb = nvn_fmt_bpb(t->d.format, &c.bw, &c.bh);
    if (!(c.ops = calloc(c.nheaps + 1, sizeof *c.ops)) || !(c.tile = malloc(SP_TILE))) MDIE("sparse mapping operations");
    pthread_mutex_lock(&g_sp_mu);
    if (nr < 0) {
        for (uint32_t y = 0; y < s->layers; y++)
            for (uint32_t i = 0; i < s->per_layer; i++)
                if (whole < 2 || s->slot[(size_t)y * s->per_layer + i]) sp_tile(&c, y, i);
    } else {
        qsort(ranges, (size_t)nr, sizeof ranges[0], sp_cmp);
        uint64_t lo = ranges[0][0], hi = ranges[0][1];
        for (int k = 1; k < nr; k++) {
            if (ranges[k][0] <= hi) {
                if (ranges[k][1] > hi) hi = ranges[k][1];
                continue;
            }
            sp_range(&c, lo, hi);
            lo = ranges[k][0]; hi = ranges[k][1];
        }
        sp_range(&c, lo, hi);
    }
    pthread_mutex_unlock(&g_sp_mu);
    free(c.tile); free(c.tail); free(ranges);
    if (whole) { s->gen = gen; s->stamp = stamp; }   // a partial scan keeps the write baseline of tiles it did not visit
    for (uint32_t k = 0; k <= c.nheaps; k++) {   // unmaps first: a tile moving to another shared slot is unmapped, then mapped again
        uint32_t h = k ? k - 1 : c.nheaps;
        if (!c.ops[h].n) continue;
        id<MTLHeap> heap = c.ops[h].heap;
        MTL4UpdateSparseTextureMappingOperation *op = c.ops[h].op;
        size_t n = c.ops[h].n;
        rec_queue_block(^{ [g_q4 updateTextureMappings:stex heap:heap operations:op count:n]; free(op); });
    }
    free(c.ops);
    if (c.up && !s->gpu) { [g_up_texs addObject:stex]; if (!g_cb) up_flush(); }
    static uint64_t looks;
    if (c.up || c.unmapped) {
        looks++;
        if (looks <= 20 || looks % 500 == 0) MLOG("sparse texture ser=%llu: %llu tiles uploaded, %llu unmapped; %llu tiles mapped in all (%llu of them shared by physical memory), peak %llu (%llu MB)", (unsigned long long)t->serial,
                                                  (unsigned long long)c.up, (unsigned long long)c.unmapped, (unsigned long long)g_sp_live, (unsigned long long)g_sp_shared, (unsigned long long)g_sp_peak, (unsigned long long)(g_sp_peak * SP_TILE >> 20));
    }
}
// Writes behind a mapped tile are looked at once a second at most (a walk over every mapped tile): a rewritten tile can show its old content for that long.
static inline void sparse_look(NvSparse *s, const tex_t *t) {
    if (nvn_sparse_log_seq(t->pool) != s->seq) sparse_sync(s, t, 0);
    if (atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && !s->gpu && up_lost_take(s->tex)) { sparse_sync(s, t, 3); return; }
    if (!s->gpu && nvn_dirty_sparse_writes() != s->wseen && (s->wown != nvn_dirty_own_writes() || atomic_load(&g_cur_frame) - s->wframe >= 60)) sparse_sync(s, t, 2);
}
static char g_sp_key;
static void sparse_gpu_look(const tex_t *t, id have) {
    NvSparse *ps = objc_getAssociatedObject(have, &g_sp_key);
    if (ps) sparse_look(ps, t);
}
// t was just resolved, so its tiles are current; a packaged texture can be drawn into too, which s->gpu does not say
static void sparse_gpu_wrote(const tex_t *t) {
    if (!t || !t->pool || !nvn_sparse_pool(t->pool)) return;
    id have = tex_get(t->serial); NvSparse *s = nil;
    if ([have isKindOfClass:[NvPkg class]]) s = ((NvPkg *)have)->ps;
    else if (have) s = objc_getAssociatedObject(have, &g_sp_key);
    if (!s || !s->undrawn) return;
    s->undrawn = 0;
    pthread_mutex_lock(&g_sp_mu);
    for (size_t i = 0, n = (size_t)s->layers * s->per_layer; i < n; i++) {
        if (!s->slot[i]) continue;
        sp_tl *e = sp_tl_of(s->slot[i] - 1);
        if (e->p) e->drawn = 1;
    }
    pthread_mutex_unlock(&g_sp_mu);
}
static id<MTLTexture> sparse_gpu_make(const tex_t *t) {
    id<MTLTexture> made = nil;
    NvSparse *ps = sparse_make(t, 1, &made);
    if (!ps) return nil;
    objc_setAssociatedObject(made, &g_sp_key, ps, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    sparse_sync(ps, t, 1);
    return made;
}
