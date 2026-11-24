// Placement sparse textures: the game's mappings onto heap tiles.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---------- Placement sparse textures ----------
// A sparse texture in a sparse virtual pool holds what the game mapped into it, tile by tile: Mario Kart World's terrain keeps six 8192x8192x68 arrays of 12 levels (two BC6H, 5,803 MB
// each made whole; four ASTC 8x8, 1,451 MB) in such pools and maps a small part of them. Made whole, the six took more than the 16 GB Mac had, and every command buffer failed with
// kIOGPUCommandBufferCallbackErrorOutOfMemory, so no frame was presented. NVN lays such a texture out in 64 KB tiles (nvn_tex.c level_walk; a tile is 64 KB of storage of its own,
// nvn_deswizzle_tile) of the same shape as Metal's 64 KB sparse tiles (256x256 texels of 16-byte blocks, 512x512 of ASTC 8x8, 128x128 of RGBA8, measured on an M6), and its mip tail starts at
// the same level (the first smaller than a tile). So it is made a Metal placement sparse texture: no memory until the replay maps a tile from a placement heap, which it does for each
// tile all of whose pages the game has mapped, uploading that tile's 64 KB; a tile the game unmaps is unmapped and its heap tile reused once the GPU is past it. Each layer's mip tail is
// one Metal tile, mapped and uploaded when the game has mapped all of its storage. Only a texture that would take at least SP_MIN bytes made whole goes this way: smaller ones keep the
// whole upload (pkg_upload_), which costs no mapping operations and keeps sparse_view's level clamp (Monster Hunter Rise's streamed clothes)
#define SP_TILE 0x10000u
#define SP_HEAP_TILES 4096u            // 256 MB placement heaps
#define SP_MIN (64ull << 20)
int nvn_sparse_tile_state(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp);   // hle_nvn.c
const uint8_t *nvn_sparse_tile_phys(const void *pool, uint64_t a);
uint64_t nvn_sparse_log_seq(const void *pool); int nvn_sparse_log_read(const void *pool, uint64_t *seq, uint64_t (*out)[2], int max); void nvn_sparse_protect_tile(const void *pool, uint64_t a, uint64_t n);
static pthread_mutex_t g_sp_mu = PTHREAD_MUTEX_INITIALIZER;
static NSMutableArray *g_sp_heaps;     // id<MTLHeap>, SP_HEAP_TILES tiles each; a slot is heap × SP_HEAP_TILES + tile
static uint32_t *g_sp_free; static size_t g_sp_nfree, g_sp_freecap;
static struct sp_late { uint32_t slot; uint64_t fence; } *g_sp_late; static size_t g_sp_nlate, g_sp_latecap;   // unmapped tiles, free once the GPU is past the fence
static uint64_t g_sp_live, g_sp_peak;
typedef struct sp_tl { uintptr_t p; uint64_t stamp; uint32_t rc; int fmt; uint8_t drawn; } sp_tl; static sp_tl *sp_tl_of(uint32_t slot);   // shared tiles, below
static int sp_slot(uint32_t *out) {   // caller holds g_sp_mu
    uint64_t done = atomic_load(&g_done); size_t k = 0;
    for (size_t i = 0; i < g_sp_nlate; i++) {
        if (g_sp_late[i].fence > done) { g_sp_late[k++] = g_sp_late[i]; continue; }
        if (g_sp_nfree == g_sp_freecap) { g_sp_freecap = g_sp_freecap ? 2 * g_sp_freecap : 4096; g_sp_free = dm_realloc(g_sp_free, g_sp_freecap * sizeof *g_sp_free, "the sparse tile list"); }
        g_sp_free[g_sp_nfree++] = g_sp_late[i].slot;
    }
    g_sp_nlate = k;
    if (!g_sp_nfree) {
        MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
        hd.type = MTLHeapTypePlacement; hd.storageMode = MTLStorageModePrivate; hd.size = (NSUInteger)SP_HEAP_TILES * SP_TILE; hd.maxCompatiblePlacementSparsePageSize = MTLSparsePageSize64;
        id<MTLHeap> h = [g_dev newHeapWithDescriptor:hd];
        if (!h) { RED_ONCE(144, "cannot make a 256 MB placement heap for sparse tiles (%llu tiles mapped)", (unsigned long long)g_sp_live); return 0; }
        if (!g_sp_heaps) g_sp_heaps = [NSMutableArray array];
        uint32_t base = (uint32_t)g_sp_heaps.count * SP_HEAP_TILES; [g_sp_heaps addObject:h]; res_add(h);
        if (g_sp_freecap < g_sp_nfree + SP_HEAP_TILES) { g_sp_freecap = g_sp_nfree + SP_HEAP_TILES; g_sp_free = dm_realloc(g_sp_free, g_sp_freecap * sizeof *g_sp_free, "the sparse tile list"); }
        for (uint32_t i = SP_HEAP_TILES; i-- > 0;) g_sp_free[g_sp_nfree++] = base + i;   // taken from the front of the heap first
        MLOG("sparse tiles: placement heap %lu (256 MB), %llu tiles mapped", (unsigned long)g_sp_heaps.count, (unsigned long long)g_sp_live);
    }
    *out = g_sp_free[--g_sp_nfree]; if (++g_sp_live > g_sp_peak) g_sp_peak = g_sp_live;
    *sp_tl_of(*out) = (sp_tl){.rc = 1};
    return 1;
}
static void sp_slot_free(uint32_t slot) {   // caller holds g_sp_mu; the GPU may still read the tile through work committed so far and the submission being built
    if (g_sp_nlate == g_sp_latecap) { g_sp_latecap = g_sp_latecap ? 2 * g_sp_latecap : 1024; g_sp_late = dm_realloc(g_sp_late, g_sp_latecap * sizeof *g_sp_late, "the sparse tile list"); }
    g_sp_late[g_sp_nlate++] = (struct sp_late){slot, atomic_load(&g_fence) + 1}; g_sp_live--;
}
// Shared tiles. On hardware a sparse texture's content is the physical memory the game maps, and a game may map one physical page at several places at once: Mario Kart World maps each page of
// its sparse pools at about three (976 MB of memory behind 2,959 MB of mappings in a race), always in textures of one format: its RGBA8 SNORM 3072² and 6144² arrays (which the GPU draws into)
// with each other, its BC6H and ASTC 8x8 terrain arrays within themselves. A tile whose 16 pages are consecutive physical memory takes the heap tile of that memory and format, so every place
// that maps it maps the same heap tile, as the hardware's page table does: one copy where there were three, and a GPU write through one place shows at the others. Metal reads one heap tile
// through several textures only without lossless compression, which no placement sparse texture has (sparse_make). A slot counts the places mapping it. Until the GPU draws through a texture
// that maps it (sparse_gpu_wrote) a slot is a copy of the memory: each new place uploads it again, as the memory may have changed in ways no fault shows (the GPU writing it as a buffer), and it
// goes back when no place maps it. A drawn slot holds what the memory does not: no new place uploads it unless the CPU wrote the memory since, and it stays while its physical pool lives, as the
// memory would, going back at the pool's Finalize (mtl_sp_phys_gone). The tail, tiles of scattered pages and tiles cut by the level's edge (only part of the tile is uploaded) keep a slot of their own
// sp_tl, per slot: p the physical memory a shared slot stands for (0: one place's own); stamp its content's dirty-page stamp; drawn: the GPU wrote through a place that mapped it
static sp_tl *g_sp_tl; static size_t g_sp_ntl;
static uint32_t *g_sp_ph; static size_t g_sp_phcap, g_sp_phn;   // physical memory and format → slot + 1, linear probing
static uint64_t g_sp_shared;   // shared slots
static sp_tl *sp_tl_of(uint32_t slot) {   // caller holds g_sp_mu
    if (slot >= g_sp_ntl) { size_t n = ((size_t)slot / SP_HEAP_TILES + 1) * SP_HEAP_TILES; g_sp_tl = dm_realloc(g_sp_tl, n * sizeof *g_sp_tl, "the sparse tile table"); memset(g_sp_tl + g_sp_ntl, 0, (n - g_sp_ntl) * sizeof *g_sp_tl); g_sp_ntl = n; }
    return &g_sp_tl[slot];
}
static size_t sp_ph_at(uintptr_t p, int fmt) { return (size_t)((((uint64_t)p >> 16) ^ (uint64_t)fmt * 0x9E3779B97F4A7C15ull) * 0x9E3779B97F4A7C15ull >> 20) & (g_sp_phcap - 1); }
static uint32_t sp_ph_find(uintptr_t p, int fmt) {   // caller holds g_sp_mu; → slot, or ~0u
    if (!g_sp_phcap) return ~0u;
    for (size_t i = sp_ph_at(p, fmt);; i = (i + 1) & (g_sp_phcap - 1)) { uint32_t e = g_sp_ph[i]; if (!e) return ~0u; if (g_sp_tl[e - 1].p == p && g_sp_tl[e - 1].fmt == fmt) return e - 1; }
}
static void sp_ph_put(uint32_t slot) {   // caller holds g_sp_mu
    if (2 * (g_sp_phn + 1) > g_sp_phcap) {
        uint32_t *old = g_sp_ph; size_t oc = g_sp_phcap; g_sp_phcap = oc ? 2 * oc : 4096;
        if (!(g_sp_ph = calloc(g_sp_phcap, sizeof *g_sp_ph))) MDIE("the shared sparse tile table");
        for (size_t i = 0; i < oc; i++) if (old[i]) { size_t j = sp_ph_at(g_sp_tl[old[i] - 1].p, g_sp_tl[old[i] - 1].fmt); while (g_sp_ph[j]) j = (j + 1) & (g_sp_phcap - 1); g_sp_ph[j] = old[i]; }
        free(old);
    }
    size_t j = sp_ph_at(g_sp_tl[slot].p, g_sp_tl[slot].fmt); while (g_sp_ph[j]) j = (j + 1) & (g_sp_phcap - 1);
    g_sp_ph[j] = slot + 1; g_sp_phn++; g_sp_shared++;
}
static void sp_ph_del(uint32_t slot) {   // caller holds g_sp_mu; backward-shift deletion
    size_t i = sp_ph_at(g_sp_tl[slot].p, g_sp_tl[slot].fmt); while (g_sp_ph[i] && g_sp_ph[i] != slot + 1) i = (i + 1) & (g_sp_phcap - 1);
    if (!g_sp_ph[i]) return;
    g_sp_ph[i] = 0; g_sp_phn--; g_sp_shared--;
    for (size_t j = (i + 1) & (g_sp_phcap - 1); g_sp_ph[j]; j = (j + 1) & (g_sp_phcap - 1)) {
        size_t h = sp_ph_at(g_sp_tl[g_sp_ph[j] - 1].p, g_sp_tl[g_sp_ph[j] - 1].fmt);
        if ((j > i && (h <= i || h > j)) || (j < i && h <= i && h > j)) { g_sp_ph[i] = g_sp_ph[j]; g_sp_ph[j] = 0; i = j; }
    }
}
static void sp_release(uint32_t slot) {   // caller holds g_sp_mu: one place stops mapping the slot
    sp_tl *e = sp_tl_of(slot);
    if (e->rc && --e->rc) return;
    if (e->p && e->drawn) return;   // the GPU's content, which only this slot holds: it stays for the next place that maps that memory
    if (e->p) sp_ph_del(slot);
    *e = (sp_tl){0}; sp_slot_free(slot);
}
void mtl_sp_phys_gone(const void *mem, uint64_t size) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    pthread_mutex_lock(&g_sp_mu);
    for (size_t i = 0; g_sp_phn && i < g_sp_phcap; i++) {
        uint32_t e = g_sp_ph[i]; if (!e) continue;
        sp_tl *t = &g_sp_tl[e - 1]; if (t->p < lo || t->p >= hi) continue;
        sp_ph_del(e - 1); i--;   // the deletion may shift a later entry into slot i. No new pool over this memory finds it: a place still mapping it moves off at its next look
        if (t->rc) { t->p = 0; t->drawn = 0; } else { *t = (sp_tl){0}; sp_slot_free(e - 1); }
    }
    pthread_mutex_unlock(&g_sp_mu);
}

@interface NvSparse : NSObject { @public
    __weak id<MTLTexture> tex; uint32_t tw, th, ntiled, levels, layers, per_layer, tail_at;   // tiles of each layer: those of the tiled levels from loff_t[l], then its tail (at tail_at)
    uint32_t tiles_x[16], tiles_y[16], loff_t[16]; uint64_t loff[16], tail_off, layer_size;
    uint32_t *slot;                    // per tile: heap slot + 1, 0 = not mapped
    uint64_t *nmap;                    // mapped tiles per level (the tail at ntiled)
    uint64_t gen, stamp;               // the page table's generation and the dirty-page stamp at the last look
    int gpu;                           // the GPU writes it (a render target or image): tiles are mapped and unmapped, nothing is uploaded
    uint64_t seq;                      // the pool's change log up to here has been handled (nvn_sparse_log_read)
    uint64_t wseen, wframe, wown;      // sparse writes, frame and replay-ordered writes at the last tile sync
    uint8_t undrawn;                   // a tile was mapped since the GPU last wrote through this texture (sparse_gpu_wrote)
} @end   // tex is weak: the NvPkg of a packaged texture owns it, a GPU texture owns its NvSparse (sparse_gpu_look)
@implementation NvSparse
- (void)dealloc { pthread_mutex_lock(&g_sp_mu); for (uint32_t i = 0; slot && i < layers * per_layer; i++) if (slot[i]) sp_release(slot[i] - 1); pthread_mutex_unlock(&g_sp_mu); free(slot); free(nmap); }
@end

// A placement sparse texture for t, or nil when it does not qualify: a 2D texture or 2D array in a sparse pool, laid out in tiles whose shape is Metal's, each tiled level whole tiles,
// its tail one Metal tile, and at least SP_MIN made whole. gpu: one the GPU writes (tex_mtl_'s GPU texture path, tex_desc(t, 0)), whose tiles get no upload
static NvSparse *sparse_make(const tex_t *t, int gpu, __strong id<MTLTexture> *made) {
    static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_SPARSE_TILES"); on = !e || atoi(e); }
    if (!on || !t->pool || !nvn_sparse_pool(t->pool) || !t->mem || (t->d.target != NVN_TEXTURE_TARGET_2D && t->d.target != 4) || t->d.samples > 1 || rb_packed_shift(t->d.format)) return nil;
    uint32_t tw, th, td; int n = nvn_tex_sparse_tiles(&t->d, &tw, &th, &td);
    if (n <= 0 || n > 15 || td != 1 || t->d.levels > 16) return nil;
    MTLTextureDescriptor *d = tex_desc(t, !gpu); if (!d) return nil;
    MTLSize ms = [g_dev sparseTileSizeWithTextureType:d.textureType pixelFormat:d.pixelFormat sampleCount:1 sparsePageSize:MTLSparsePageSize64];
    if (ms.width != tw || ms.height != th || ms.depth != 1) { static int nl; if (nl++ < 4) MLOG("sparse texture ser=%llu format %#x: Metal's tile %lux%lu is not NVN's %ux%u, made whole", (unsigned long long)t->serial, t->d.format, (unsigned long)ms.width, (unsigned long)ms.height, tw, th); return nil; }
    MTLSizeAndAlign sa = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    if (sa.size < SP_MIN) return nil;
    NvSparse *s = [NvSparse new];
    s->gpu = gpu; s->tw = tw; s->th = th; s->ntiled = (uint32_t)n; s->levels = (uint32_t)d.mipmapLevelCount; s->layers = (uint32_t)MAX(d.arrayLength, 1u); s->layer_size = t->L.layer_size;
    uint32_t k = 0;
    for (uint32_t l = 0; l < s->ntiled; l++) {
        uint32_t w, h; tex_lsize(t, l, &w, &h, NULL);
        s->tiles_x[l] = (w + tw - 1) / tw; s->tiles_y[l] = (h + th - 1) / th; s->loff_t[l] = k; k += s->tiles_x[l] * s->tiles_y[l];
        s->loff[l] = nvn_tex_level_offset(&t->d, l);
        uint64_t next = l + 1 < s->levels ? nvn_tex_level_offset(&t->d, l + 1) : s->layer_size;
        if (next - s->loff[l] != (uint64_t)s->tiles_x[l] * s->tiles_y[l] * SP_TILE) { static int nl; if (nl++ < 4) MLOG("sparse texture ser=%llu: level %u is not whole tiles (%#llx bytes), made whole", (unsigned long long)t->serial, l, (unsigned long long)(next - s->loff[l])); return nil; }
    }
    s->tail_at = k; s->per_layer = k + (s->ntiled < s->levels);
    s->tail_off = s->ntiled < s->levels ? nvn_tex_level_offset(&t->d, s->ntiled) : s->layer_size;
    // Every usage a later view or clear would ask for from the start: an upgrade (tex_upgrade) would have to remake the texture whole. A sparse texture gets no lossless compression from them to lose,
    // and without optimized contents no metadata for the whole texture either (184 MB for an 8192x8192x68 RGBA8 array on an M6)
    d.placementSparsePageSize = MTLSparsePageSize64; d.allowGPUOptimizedContents = NO;
    d.usage |= MTLTextureUsagePixelFormatView | (fmt_compressed(t->d.format) ? 0 : MTLTextureUsageRenderTarget);
    id<MTLTexture> mt = [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(145, "cannot make a placement sparse texture %ux%ux%u format %#x", t->d.width, t->d.height, t->d.depth, t->d.format); return nil; }
    if (s->ntiled < s->levels && (mt.firstMipmapInTail != s->ntiled || mt.tailSizeInBytes != SP_TILE)) {   // with no tail (every level tiled) Metal's tail lies past the last level
        static int nl; if (nl++ < 4) MLOG("sparse texture ser=%llu: Metal's tail starts at level %lu (%lu bytes), NVN's at %u, made whole", (unsigned long long)t->serial, (unsigned long)mt.firstMipmapInTail, (unsigned long)mt.tailSizeInBytes, s->ntiled);
        return nil;
    }
    s->tex = mt; *made = mt; res_add(mt);
    s->slot = calloc((size_t)s->layers * s->per_layer, sizeof *s->slot); s->nmap = calloc(s->ntiled + 1, sizeof *s->nmap);
    if (!s->slot || !s->nmap) MDIE("sparse texture tile table");
    s->gen = 0; s->stamp = 0;
    static int nl; if (nl++ < 20) MLOG("sparse texture ser=%llu %ux%ux%u format %#x levels %u: placement sparse%s, %u tiles a layer (%llu MB made whole)", (unsigned long long)t->serial, t->d.width, t->d.height, s->layers,
                                        t->d.format, s->levels, gpu ? " (GPU-written)" : "", s->per_layer, (unsigned long long)(sa.size >> 20));
    return s;
}

typedef struct { id<MTLHeap> heap; MTL4UpdateSparseTextureMappingOperation *op; size_t n, cap; } sp_ops;
static void sp_op(sp_ops *o, MTL4UpdateSparseTextureMappingOperation x) {
    if (o->n == o->cap) { o->cap = o->cap ? 2 * o->cap : 64; o->op = dm_realloc(o->op, o->cap * sizeof *o->op, "the sparse mapping operations"); }
    o->op[o->n++] = x;
}
// One look at a texture's tiles: the mapping operations by heap (unmaps, with no heap, last) and the staging of what is uploaded
typedef struct { NvSparse *s; const tex_t *t; id<MTLTexture> tex; sp_ops *ops; uint32_t nheaps; uint8_t *tile, *tail; id<MTLBuffer> stg; size_t stg_off; uint64_t up, unmapped, stamp; int bw, bh, bpb, force; } sp_look;
static void sp_stg(sp_look *c, size_t n) {
    if (c->stg && c->stg_off + n <= c->stg.length) return;
    c->stg = [g_dev newBufferWithLength:MAX((size_t)8 << 20, n) options:MTLResourceStorageModeShared]; if (!c->stg) MDIE("sparse upload staging");
    up_begin(); res_add(c->stg); [g_up_keep addObject:c->stg]; c->stg_off = 0;
}
// Tile i of layer y (the tail at s->tail_at): the game has mapped all its pages or not, and the Metal tile follows; its content is uploaded when it is newly mapped or its pages were written. Caller holds g_sp_mu
static void sp_tile(sp_look *c, uint32_t y, uint32_t i) {
    NvSparse *s = c->s; const tex_t *t = c->t;
    int is_tail = i == s->tail_at; uint32_t l = 0, tx = 0, ty = 0;
    if (!is_tail) { while (l + 1 < s->ntiled && i >= s->loff_t[l + 1]) l++; uint32_t j = i - s->loff_t[l]; tx = j % s->tiles_x[l]; ty = j / s->tiles_x[l]; }
    uint64_t a = (uint64_t)(uintptr_t)t->mem + y * s->layer_size + (is_tail ? s->tail_off : s->loff[l] + (uint64_t)(i - s->loff_t[l]) * SP_TILE), n = is_tail ? s->layer_size - s->tail_off : SP_TILE;
    uint32_t *sl = &s->slot[(size_t)y * s->per_layer + i], lev = is_tail ? s->ntiled : l; MTLRegion rg = is_tail ? MTLRegionMake3D(0, 0, 0, 1, 1, 1) : MTLRegionMake3D(tx, ty, 0, 1, 1, 1);
    int st = nvn_sparse_tile_state(t->pool, a, n, s->gen, s->stamp); if (st < 0) st = 0;
    if (!(st & 1)) {
        if (*sl) { sp_op(&c->ops[c->nheaps], (MTL4UpdateSparseTextureMappingOperation){MTLSparseTextureMappingModeUnmap, rg, lev, y, 0});
            sp_release(*sl - 1); *sl = 0; s->nmap[lev]--; c->unmapped++; }
        return;
    }
    uint32_t lw, lh, x0 = tx * s->tw, y0 = ty * s->th; tex_lsize(t, l, &lw, &lh, NULL);
    const uint8_t *p = is_tail || x0 + s->tw > lw || y0 + s->th > lh ? NULL : nvn_sparse_tile_phys(t->pool, a);   // shared tiles (see above)
    if (*sl) {
        const sp_tl *e = sp_tl_of(*sl - 1);
        if (e->p != (uintptr_t)p || (p && e->fmt != t->d.format)) {   // other memory behind the tile now: give up this place's slot (the unmaps go first, sparse_sync)
            sp_op(&c->ops[c->nheaps], (MTL4UpdateSparseTextureMappingOperation){MTLSparseTextureMappingModeUnmap, rg, lev, y, 0});
            sp_release(*sl - 1); *sl = 0; s->nmap[lev]--;
        } else if (s->gpu) return;   // the GPU's own writes are in the tile: a CPU write to its pages is not looked at
        else if (e->drawn ? !nvn_dirty_since(p, SP_TILE, e->stamp) : !c->force && !(p ? nvn_dirty_since(p, SP_TILE, e->stamp) : (st & 2))) return;   // a shared slot: the CPU wrote its memory since its content was taken, through any place
    }
    if (!*sl) {
        uint32_t slot = p ? sp_ph_find((uintptr_t)p, t->d.format) : ~0u; int fresh = slot == ~0u;
        if (fresh) { if (!sp_slot(&slot)) return; }
        else sp_tl_of(slot)->rc++;
        uint32_t hi = slot / SP_HEAP_TILES; if (hi >= c->nheaps) { RED_ONCE(146, "sparse tiles: more than 64 heaps made in one look"); sp_release(slot); return; }
        c->ops[hi].heap = g_sp_heaps[hi];
        sp_op(&c->ops[hi], (MTL4UpdateSparseTextureMappingOperation){MTLSparseTextureMappingModeMap, rg, lev, y, slot % SP_HEAP_TILES});
        *sl = slot + 1; s->nmap[lev]++; s->undrawn = 1;
        if (fresh && p) { sp_tl *e = sp_tl_of(slot); e->p = (uintptr_t)p; e->fmt = t->d.format; e->stamp = nvn_dirty_now(); sp_ph_put(slot); }
        if (s->gpu) { if (fresh && p) nvn_dirty_protect_sparse(p, SP_TILE); c->up++; return; }   // mapped, for the GPU to write (a later CPU write to the memory shows at a packaged place)
        if (!fresh && sp_tl_of(slot)->drawn && !nvn_dirty_since(p, SP_TILE, sp_tl_of(slot)->stamp)) return;   // the GPU's content, newer than the memory's
    }
    // the content: protected before it is read, as any packaged texture's, so a later CPU write shows (nvn_dirty_sparse_writes, sparse_look). A shared slot is the memory at p, which the
    // game may remap away from this place on its own thread at any moment: the content is read from p, not through the page table, so it is never kept under another memory's key
    if (p) nvn_dirty_protect_sparse(p, SP_TILE); else nvn_sparse_protect_tile(t->pool, a, n);
    int bw = c->bw, bh = c->bh, bpb = c->bpb;
    if (is_tail) {
        c->tail = dm_realloc(c->tail, n, "the sparse tail copy");
        nvn_sparse_copy(t->pool, c->tail, a, n);
        for (uint32_t tl = s->ntiled; tl < s->levels; tl++) {
            uint32_t w, h; tex_lsize(t, tl, &w, &h, NULL); size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            sp_stg(c, pitch * rows);
            if (nvn_deswizzle_from(&t->d, tl, y, c->tail, y * s->layer_size + s->tail_off, (uint8_t *)c->stg.contents + c->stg_off, pitch)) { RED_ONCE(147, "sparse tail level %u of format %#x not decoded", tl, t->d.format); break; }
            up_blit_add_at(c->stg, c->stg_off, pitch, pitch * rows, MTLSizeMake(w, h, 1), c->tex, y, tl, MTLOriginMake(0, 0, 0)); c->stg_off += (pitch * rows + 255) & ~(size_t)255;
        }
    } else {
        if (p) memcpy(c->tile, p, SP_TILE); else nvn_sparse_copy(t->pool, c->tile, a, SP_TILE);
        sp_stg(c, SP_TILE);
        uint32_t wb, rows; size_t tpitch = (size_t)(s->tw / bw) * bpb;   // a tile's row of blocks: 64 KB / its block rows
        if (nvn_deswizzle_tile(&t->d, l, tx, ty, c->tile, (uint8_t *)c->stg.contents + c->stg_off, tpitch, &wb, &rows)) { RED_ONCE(148, "sparse tile of format %#x not decoded", t->d.format); return; }
        if (wb && rows) up_blit_add_at(c->stg, c->stg_off, tpitch, tpitch * rows, MTLSizeMake(MIN(s->tw, lw - x0), MIN(s->th, lh - y0), 1), c->tex, y, l, MTLOriginMake(x0, y0, 0));
        c->stg_off += SP_TILE;
        if (p) sp_tl_of(*sl - 1)->stamp = c->stamp;   // taken before the protection above
    }
    c->up++;
}
// The tiles of the guest range [lo, hi) of the texture
static void sp_range(sp_look *c, uint64_t lo, uint64_t hi) {
    NvSparse *s = c->s; uint64_t base = (uint64_t)(uintptr_t)c->t->mem, end = base + (uint64_t)s->layers * s->layer_size;
    if (lo < base) lo = base;
    if (hi > end) hi = end;
    for (uint64_t o = (lo - base) & ~(uint64_t)(SP_TILE - 1); lo < hi && base + o < hi;) {
        uint32_t y = (uint32_t)(o / s->layer_size); uint64_t in = o % s->layer_size;
        if (in >= s->tail_off) { sp_tile(c, y, s->tail_at); o += s->layer_size - in; continue; }   // the tail once, whatever part of it the range touches
        uint32_t l = 0; while (l + 1 < s->ntiled && in >= s->loff[l + 1]) l++;
        sp_tile(c, y, s->loff_t[l] + (uint32_t)((in - s->loff[l]) / SP_TILE));
        o += SP_TILE;
    }
}
static int sp_cmp(const void *a, const void *b) { uint64_t x = *(const uint64_t *)a, y = *(const uint64_t *)b; return x < y ? -1 : x > y; }
// The game's mappings into the texture since the last look: tiles mapped now get a heap tile and their content, tiles unmapped give theirs back. All the tiles at the first look or when the pool's
// log has moved on past it, else those of the requests since (sorted and joined: a tile is handled once). The mapping operations go to the queue ahead of the upload batch that writes the tiles (rec_queue_block).
// whole 2: only the tiles mapped now, for one a write to its pages has changed; 3: all of those again, an upload batch having been dropped (up_lost_take)
static void sparse_sync(NvSparse *s, const tex_t *t, int whole) {
    id<MTLTexture> stex = s->tex; if (!stex) return;
    if (!s->gpu) tex_cpu_wait(t);   // before the CPU snapshots a tile, and before taking its stamp
    if (whole) s->wown = nvn_dirty_own_writes();   // a mapping-only scan has not checked writes into the tiles already mapped
    enum { RANGES = 4096 }; uint64_t (*ranges)[2] = NULL; int nr = -1;
    uint64_t gen = nvn_sparse_gen(), stamp = nvn_dirty_now();
    if (whole == 1) s->seq = nvn_sparse_log_seq(t->pool);
    else if (whole >= 2) { s->wseen = nvn_dirty_sparse_writes(); s->wframe = atomic_load(&g_cur_frame); }
    else { if (!(ranges = malloc(RANGES * sizeof *ranges))) MDIE("sparse mapping ranges"); nr = nvn_sparse_log_read(t->pool, &s->seq, ranges, RANGES); if (!nr) { free(ranges); return; } }
    sp_look c = {.s = s, .t = t, .tex = stex, .nheaps = (uint32_t)g_sp_heaps.count + 64, .force = whole == 3, .stamp = stamp}; c.bpb = nvn_fmt_bpb(t->d.format, &c.bw, &c.bh);
    if (!(c.ops = calloc(c.nheaps + 1, sizeof *c.ops)) || !(c.tile = malloc(SP_TILE))) MDIE("sparse mapping operations");
    pthread_mutex_lock(&g_sp_mu);
    if (nr < 0) for (uint32_t y = 0; y < s->layers; y++) for (uint32_t i = 0; i < s->per_layer; i++) { if (whole < 2 || s->slot[(size_t)y * s->per_layer + i]) sp_tile(&c, y, i); }
    else {
        qsort(ranges, (size_t)nr, sizeof ranges[0], sp_cmp);
        uint64_t lo = ranges[0][0], hi = ranges[0][1];
        for (int k = 1; k < nr; k++) { if (ranges[k][0] <= hi) { if (ranges[k][1] > hi) hi = ranges[k][1]; continue; } sp_range(&c, lo, hi); lo = ranges[k][0]; hi = ranges[k][1]; }
        sp_range(&c, lo, hi);
    }
    pthread_mutex_unlock(&g_sp_mu);
    free(c.tile); free(c.tail); free(ranges);
    if (whole) { s->gen = gen; s->stamp = stamp; }   // partial mapping scans must preserve the write baseline of tails and scattered tiles they did not visit
    for (uint32_t k = 0; k <= c.nheaps; k++) {   // the unmaps first: a tile moving to another shared slot is unmapped, then mapped again
        uint32_t h = k ? k - 1 : c.nheaps;
        if (!c.ops[h].n) continue;
        id<MTLHeap> heap = c.ops[h].heap; MTL4UpdateSparseTextureMappingOperation *op = c.ops[h].op; size_t n = c.ops[h].n;
        rec_queue_block(^{ [g_q4 updateTextureMappings:stex heap:heap operations:op count:n]; free(op); });
    }
    free(c.ops);
    if (c.up && !s->gpu) { [g_up_texs addObject:stex]; if (!g_cb) up_flush(); }
    static uint64_t looks; if (c.up || c.unmapped) { looks++; if (looks <= 20 || looks % 500 == 0) MLOG("sparse texture ser=%llu: %llu tiles uploaded, %llu unmapped; %llu tiles mapped in all (%llu of them shared by physical memory), peak %llu (%llu MB)", (unsigned long long)t->serial,
                                                        (unsigned long long)c.up, (unsigned long long)c.unmapped, (unsigned long long)g_sp_live, (unsigned long long)g_sp_shared, (unsigned long long)g_sp_peak, (unsigned long long)(g_sp_peak * SP_TILE >> 20)); }
}
// A look when the texture is resolved: only a new mapping request in its pool costs anything, or a write to a page behind a tile, which no game seen does to a tile it keeps mapped (none of 42,072
// in Mario Kart World's first race): at most once a second, as the look goes over every mapped tile, so a rewritten tile can show its old content for that long
static inline void sparse_look(NvSparse *s, const tex_t *t) {
    if (nvn_sparse_log_seq(t->pool) != s->seq) sparse_sync(s, t, 0);
    if (atomic_load_explicit(&g_up_lost_n, memory_order_relaxed) && !s->gpu && up_lost_take(s->tex)) { sparse_sync(s, t, 3); return; }
    if (!s->gpu && nvn_dirty_sparse_writes() != s->wseen && (s->wown != nvn_dirty_own_writes() || atomic_load(&g_cur_frame) - s->wframe >= 60)) sparse_sync(s, t, 2);
}
static char g_sp_key;   // a GPU texture's NvSparse, as its associated object
// A GPU-written placement sparse texture follows the game's mappings when it is resolved again (tex_mtl_)
static void sparse_gpu_look(const tex_t *t, id have) {
    NvSparse *ps = objc_getAssociatedObject(have, &g_sp_key);
    if (ps) sparse_look(ps, t);
}
// The GPU writes t through an attachment, an image, a copy or a clear (t just resolved, so its tiles are current; a packaged texture can be drawn into too, so s->gpu does not say this): the slots
// it maps now hold what the memory does not. Only the first write after a mapping change walks the tiles
static void sparse_gpu_wrote(const tex_t *t) {
    if (!t || !t->pool || !nvn_sparse_pool(t->pool)) return;
    id have = tex_get(t->serial); NvSparse *s = nil;
    if ([have isKindOfClass:[NvPkg class]]) s = ((NvPkg *)have)->ps;
    else if (have) s = objc_getAssociatedObject(have, &g_sp_key);
    if (!s || !s->undrawn) return;
    s->undrawn = 0;
    pthread_mutex_lock(&g_sp_mu);
    for (size_t i = 0, n = (size_t)s->layers * s->per_layer; i < n; i++) if (s->slot[i]) { sp_tl *e = sp_tl_of(s->slot[i] - 1); if (e->p) e->drawn = 1; }
    pthread_mutex_unlock(&g_sp_mu);
}
// A GPU texture in a sparse pool made placement sparse (sparse_make with gpu): nil when it does not qualify
static id<MTLTexture> sparse_gpu_make(const tex_t *t) {
    id<MTLTexture> made = nil; NvSparse *ps = sparse_make(t, 1, &made); if (!ps) return nil;
    objc_setAssociatedObject(made, &g_sp_key, ps, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    sparse_sync(ps, t, 1);
    return made;
}
