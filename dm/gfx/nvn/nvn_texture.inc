// The replay's textures: lookups, sRGB pairs, image usage and atomics, packaged textures and their uploads.
#include "../../host/front.h"

// Texture lookups run on the submit thread and other game threads; the caller's reference to the object must be valid for the check.
static BOOL obj_is_tex(id object) {
    if (!object) return NO;
    enum { CACHE_SIZE = 8 };
    static _Thread_local struct {
        struct { __unsafe_unretained Class cls; BOOL is_texture; } entries[CACHE_SIZE];
        unsigned next;
    } cache;
    Class cls = object_getClass(object);
    for (unsigned i = 0; i < CACHE_SIZE; i++)
        if (cache.entries[i].cls == cls) return cache.entries[i].is_texture;

    BOOL is_texture = [object conformsToProtocol:@protocol(MTLTexture)];
    unsigned slot = cache.next++ % CACHE_SIZE;
    cache.entries[slot].cls = cls;
    cache.entries[slot].is_texture = is_texture;
    return is_texture;
}

static int g_red_once[176];   // one past the highest key: a new key raises this, and the macro refuses to compile otherwise
#define RED_ONCE(k, ...) do { _Static_assert((k) < sizeof g_red_once / sizeof *g_red_once, "a RED_ONCE key past g_red_once: raise its size"); if (!g_red_once[k]++) MLOG("[RED] " __VA_ARGS__); } while (0)
// Metal has only buffer slots 0–30 (30 is the constant fallback buffer): streams 0–13 go to 16–29, 14 and 15 stay.
#define VB_SLOT(st) ((st) < 14 ? 16 + (st) : (st))

static int fmt_compressed(int f) { return (f >= 0x41 && f <= 0x50) || (f >= 0x79 && f <= 0x94); }
static int fmt_depth(int f) { return f >= 0x31 && f <= 0x36; }
typedef struct { MTLPixelFormat a, b; } pfv_pair;
static const pfv_pair g_pfv_srgb[] = {
    {MTLPixelFormatRGBA8Unorm, MTLPixelFormatRGBA8Unorm_sRGB},
    {MTLPixelFormatBGRA8Unorm, MTLPixelFormatBGRA8Unorm_sRGB},
    {MTLPixelFormatBC1_RGBA, MTLPixelFormatBC1_RGBA_sRGB},
    {MTLPixelFormatBC2_RGBA, MTLPixelFormatBC2_RGBA_sRGB},
    {MTLPixelFormatBC3_RGBA, MTLPixelFormatBC3_RGBA_sRGB},
    {MTLPixelFormatBC7_RGBAUnorm, MTLPixelFormatBC7_RGBAUnorm_sRGB},
};
// MTLPixelFormatInvalid for a format that is not sRGB
static MTLPixelFormat pf_srgb_linear(MTLPixelFormat pf) {
    for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++) if (pf == g_pfv_srgb[i].b) return g_pfv_srgb[i].a;
    return MTLPixelFormatInvalid;
}
static int tex_need_pfv(MTLPixelFormat base, MTLPixelFormat view) {
    if (base == view) return 0;
    for (size_t i = 0; i < sizeof g_pfv_srgb / sizeof *g_pfv_srgb; i++)
        if ((base == g_pfv_srgb[i].a && view == g_pfv_srgb[i].b) || (base == g_pfv_srgb[i].b && view == g_pfv_srgb[i].a)) return 0;
    return 1;
}
// the table stores CFBridgingRetain'ed objects, CFRelease'd on replace / delete
static void hm_rel(void *v) { CFRelease(v); }
// Under g_smu: the shared packaged textures (g_pkg_share, weak, partitioned by pool) and the image descriptions.
static nvn_hmap g_pkg_share = {.klen = 8};   // pool address → malloc'ed nvn_hmap (alias_key → NvPkgRef)
// A dynamic-resolution family declares its largest image before any resolution is used, so its first allocation can cover the family exactly.
static nvn_hmap g_image_desc = {.klen = 8};   // serial → malloc'ed tex_t, under g_smu
enum { IMAGE_NOT_SHARED = NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET };
static int image_shape(const tex_t *t) {
    return t && t->pool && !t->has_layout && (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && !(t->d.flags & IMAGE_NOT_SHARED)
        && t->d.target == NVN_TEXTURE_TARGET_2D && t->d.depth == 1 && t->d.levels == 1 && t->d.samples <= 1 && !t->d.stride;
}
static void image_desc_drop_pool(const void *pool) {   // caller holds g_smu
    for (uint32_t i = 0; i < g_image_desc.cap;) {
        if (g_image_desc.vals[i] && ((tex_t *)g_image_desc.vals[i])->pool == pool) {
            uint64_t serial; memcpy(&serial, g_image_desc.keys + (size_t)i * 8, 8);
            free(nvn_hm_del(&g_image_desc, &serial));   // deletion shifts entries: examine this slot again
        } else i++;
    }
}
static void treg_drop_pool_views(const void *pool);
static void cpu_tex_drop_pool(const void *pool);
static void gws_drop_pool(const pool_t *p);
void own_hook_drop(id o, int keep_resident) { if (keep_resident) defer_obj_unref(o); else defer_obj_release(o); }   // released once the submission in flight completes
uint64_t own_hook_submission(void) { return g_fence + 1; }
uint64_t own_hook_frame(void) { return atomic_load(&g_cur_frame); }
void mtl_tex_init(const tex_t *t) {
    if (!t->serial) return;
    tex_t *desc = image_shape(t) ? malloc(sizeof *desc) : NULL; if (desc) *desc = *t;
    pthread_mutex_lock(&g_smu);
    own_tex_live_put_locked(t);
    if (desc) free(nvn_hm_put(&g_image_desc, &t->serial, desc));
    pthread_mutex_unlock(&g_smu);
}
// spare: the texture the last re-upload replaced, rewritten by the next once the GPU has passed spare_fence, so a texture rewritten every frame alternates between two.
@interface NvPkg : NSObject { @public id<MTLTexture> tex, spare, view; uint64_t stamp, seen, hash, spare_fence, spare_t, vgen, vseen, sub, own, lost_gen, size; uintptr_t mem; uint32_t used_tu; const void *span; int sparse, vbase, part; uint8_t *edge[2]; uint32_t edge_n[2], stored; id ps; id<MTLBuffer> eb; uint64_t *ph, up_frame, up_sub; uint32_t nph, missed; } @end   // span: its tracked range (NULL = untracked)
// ps: its placement sparse texture; edge: uploaded bytes of the storage's shared first and last host pages (in eb once the GPU stores into it).
// Its release retires its textures (defer_obj_release): serials that took its texture over keep it resident.
@implementation NvPkg
- (void)dealloc { defer_obj_release(tex); defer_obj_release(spare); if (eb) defer_obj_release(eb); else { free(edge[0]); free(edge[1]); } free(ph); }
@end
// Spares older than g_spare_idle_ns are retired at a submission's start, every spare at memory pressure. Submit thread only, as are re-uploads.
static uint64_t g_spare_idle_ns = 3000000000ull;
static NSHashTable *g_spare_pkgs; static uint64_t g_spare_last; static _Atomic uint64_t g_spare_trimmed, g_spare_trimmed_bytes, g_spare_held;
static void spare_note(NvPkg *pk) { if (!g_spare_pkgs) g_spare_pkgs = [NSHashTable weakObjectsHashTable]; pk->spare_t = clock_gettime_nsec_np(CLOCK_MONOTONIC); [g_spare_pkgs addObject:pk]; }
static uint64_t spare_trim(int all, unsigned *n_out) { @autoreleasepool {
    unsigned n = 0; uint64_t b = 0, now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (!g_spare_pkgs.count || (!all && now - g_spare_last < 250000000ull)) { if (n_out) *n_out = 0; return 0; }
    g_spare_last = now;
    NSMutableArray *gone = nil;
    uint64_t held = 0;
    for (NvPkg *pk in g_spare_pkgs) {
        if (pk->spare && !all && now - pk->spare_t < g_spare_idle_ns) { held++; continue; }
        if (pk->spare) { n++; b += pk->spare.heap ? 0 : pk->spare.allocatedSize; defer_obj_release(pk->spare); pk->spare = nil; }
        if (!gone) gone = [NSMutableArray array];
        [gone addObject:pk];
    }
    for (NvPkg *pk in gone) [g_spare_pkgs removeObject:pk];
    atomic_fetch_add(&g_spare_trimmed, n); atomic_fetch_add(&g_spare_trimmed_bytes, b); atomic_store(&g_spare_held, held);
    if (n_out) *n_out = n;
    return b;
} }
// Engines make many texture objects over one texture's bytes: a new serial with the same pool, storage and description takes the same NvPkg. Weak: the serials' entries own it.
// Under g_smu.
@interface NvPkgRef : NSObject { @public __weak NvPkg *pkg; } @end
@implementation NvPkgRef @end
static void pkg_share_compact(nvn_hmap *pm) {   // caller holds g_smu
    nvn_hmap o = *pm;
    uint32_t live = 0;
    for (uint32_t i = 0; i < o.cap; i++) {
        if (!o.vals[i]) continue;
        NvPkg *pk = ((NvPkgRef *)(__bridge id)o.vals[i])->pkg;
        if (!pk) { CFRelease(o.vals[i]); o.vals[i] = NULL; }
        else live++;
    }
    uint64_t need = ((uint64_t)live + 1) * 2;
    uint32_t ncap = 64;
    while ((uint64_t)ncap < need && ncap < (1u << 30)) ncap *= 2;
    pm->keys = malloc((size_t)ncap * o.klen);
    pm->vals = calloc(ncap, sizeof *pm->vals);
    if (!pm->keys || !pm->vals) MDIE("packaged-texture sharing table compaction");
    pm->cap = ncap; pm->n = 0;
    for (uint32_t i = 0; i < o.cap; i++) if (o.vals[i]) nvn_hm_put(pm, o.keys + (size_t)i * o.klen, o.vals[i]);
    free(o.keys); free(o.vals);
}
void mtl_alias_drop_pool(const void *pool) {
    if (!pool) return;
    pthread_mutex_lock(&g_smu);
    image_desc_drop_pool(pool);
    NSMutableSet *pkgs = [NSMutableSet set];
    { uint64_t pkey = (uint64_t)(uintptr_t)pool;   // a later pool at this address holds other bytes
      nvn_hmap *pm = nvn_hm_del(&g_pkg_share, &pkey);
      if (pm) {
          for (uint32_t i = 0; i < pm->cap; i++) {
              if (!pm->vals[i]) continue;
              NvPkg *pk = ((NvPkgRef *)(__bridge id)pm->vals[i])->pkg;
              if (pk) [pkgs addObject:pk];
              CFRelease(pm->vals[i]);
          }
          free(pm->keys); free(pm->vals); free(pm);
      } }
    own_pool_finalize_locked(pool, (__bridge CFSetRef)pkgs);   // a new epoch: every texture ID takes the slow path once
    pthread_mutex_unlock(&g_smu);
    // The pool's texture IDs are marked gone first: a pool walk in between would find an ID not gone and no longer CPU-written, and hand out its NvPkg as the texture.
    treg_drop_pool_views(pool);   // outside g_smu: g_rmu is taken before g_smu
    cpu_tex_drop_pool(pool);
    gws_drop_pool(pool);
}
void own_hook_parts(id o, void (^add)(id part)) { if (![o isKindOfClass:[NvPkg class]]) return; NvPkg *pk = o; add(pk->tex); add(pk->view); add(pk->spare); }
static alias_key tex_alias_key(const tex_t *t) {
    alias_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)t->pool; k.off = t->off; k.d = t->d;
    // IMAGE does not change storage: a depth texture and its stencil-sampling object may differ only by it, and separate Metal textures would make the stencil read zero.
    k.d.flags &= ~NVN_TEXTURE_FLAGS_IMAGE;
    return k;
}
static NvPkg *pkg_shared(const tex_t *t) {
    uint64_t pkey = (uint64_t)(uintptr_t)t->pool; NvPkg *pk = nil;
    pthread_mutex_lock(&g_smu);
    nvn_hmap *pm = nvn_hm_get(&g_pkg_share, &pkey);
    if (pm) {
        alias_key k = tex_alias_key(t);
        NvPkgRef *r = hm_get_id(pm, &k);
        if (r) { pk = r->pkg; if (!pk) hm_del_id(pm, &k); }   // a dead entry reads as a missing one
    }
    pthread_mutex_unlock(&g_smu);
    return pk;
}
static void pkg_share_put(const tex_t *t, NvPkg *pk) {
    alias_key k = tex_alias_key(t); NvPkgRef *r = [NvPkgRef new]; r->pkg = pk;
    uint64_t pkey = (uint64_t)(uintptr_t)t->pool;
    pthread_mutex_lock(&g_smu);
    nvn_hmap *pm = nvn_hm_get(&g_pkg_share, &pkey);
    if (!pm) {
        pm = calloc(1, sizeof *pm);
        if (!pm) MDIE("packaged-texture sharing table");
        pm->klen = sizeof(alias_key);
        nvn_hm_put(&g_pkg_share, &pkey, pm);
    } else if (pm->cap && (pm->n + 1) * 4 > pm->cap * 3) {
        pkg_share_compact(pm);
    }
    hm_put_id(pm, &k, r);
    pthread_mutex_unlock(&g_smu);
}
// A submission sees the CPU writes and mappings made before it (a map is a CPU call, not a command): a sparse texture is checked once a submission.
static inline int sparse_checked(const NvPkg *pk) { return pk->sub == g_sub_serial && pk->own == nvn_dirty_own_writes(); }
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_miss, g_pkg_edge_skips;
static uint64_t g_pfv_upgrades;
// DM_NVN_DIRTYCHECK: content changed with no write the tracker saw is a missed detection
static int dirtycheck(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_DIRTYCHECK") != NULL; return v; }
static uint64_t full_hash(const uint8_t *m, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ m[i]) * 1099511628211ull; return h; }
static void pkg_hash_take(NvPkg *pk, const tex_t *t) {
    if (!dirtycheck() || pk->sparse) return;
    pk->hash = full_hash(t->mem, t->L.size);
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)t->mem, e = a + t->L.size, p0 = a & ~(pg - 1);
    uint32_t n = (uint32_t)((e - p0 + pg - 1) / pg);
    if (n != pk->nph) { free(pk->ph); pk->ph = malloc(n * sizeof *pk->ph); pk->nph = pk->ph ? n : 0; }
    for (uint32_t i = 0; i < pk->nph; i++) { uintptr_t lo = MAX(a, p0 + i * pg), hi = MIN(e, p0 + (i + 1) * pg); pk->ph[i] = full_hash((const uint8_t *)lo, hi - lo); }
    pk->up_frame = atomic_load(&g_cur_frame); pk->up_sub = g_sub_serial; pk->missed = 0;
}
static int g_pkg_miss_reports;
static void pkg_miss_report(NvPkg *pk, const tex_t *t, uint64_t now) {
    if (pk->missed || g_pkg_miss_reports >= 16) return;
    pk->missed = 1; g_pkg_miss_reports++;
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)t->mem, e = a + t->L.size, p0 = a & ~(pg - 1);
    uint32_t changed = 0, first = UINT32_MAX, last = 0;
    for (uint32_t i = 0; i < pk->nph; i++) { uintptr_t lo = MAX(a, p0 + i * pg), hi = MIN(e, p0 + (i + 1) * pg);
        if (full_hash((const uint8_t *)lo, hi - lo) != pk->ph[i]) { changed++; if (first == UINT32_MAX) first = i; last = i; } }
    const void *now_span = nvn_dirty_span(t->mem, t->L.size);
    MLOG("[RED] dirty-page miss: texture ser=%llu %ux%ux%u format %#x target %d levels %u, %llu bytes at %p: its content changed with no write seen; %u of its %u host pages changed (pages %d-%d), "
         "uploaded at frame %llu submission %llu (now %llu, %llu), stamp %llu, clock %llu, GPU stores into it %u, kept range %p, range now %p",
         (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (unsigned long long)t->L.size, (void *)t->mem, changed, pk->nph,
         first == UINT32_MAX ? -1 : (int)first, first == UINT32_MAX ? -1 : (int)last, (unsigned long long)pk->up_frame, (unsigned long long)pk->up_sub,
         (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)g_sub_serial, (unsigned long long)pk->stamp, (unsigned long long)now, pk->stored, pk->span, now_span);
    int shown = 0;
    for (uint32_t i = 0; i < pk->nph && shown < 6; i++) {
        uintptr_t lo = MAX(a, p0 + i * pg), hi = MIN(e, p0 + (i + 1) * pg);
        if (full_hash((const uint8_t *)lo, hi - lo) == pk->ph[i]) continue;
        shown++;
        uint64_t o = lo - a; unsigned layer = t->L.layer_size ? (unsigned)(o / t->L.layer_size) : 0, lv = 0; uint64_t ol = t->L.layer_size ? o % t->L.layer_size : o;
        while (lv + 1 < t->d.levels && nvn_tex_level_offset(&t->d, lv + 1) <= ol) lv++;
        char d[512]; nvn_dirty_describe((const void *)lo, pk->span, d, sizeof d);
        MLOG("[RED] dirty-page miss ser=%llu page %u (%p, its bytes %#llx-%#llx, layer %u level %u): %s", (unsigned long long)t->serial, i, (void *)(p0 + i * pg), (unsigned long long)o,
             (unsigned long long)(hi - a), layer, lv, d);
    }
}
// The tracker marks whole 16 KB host pages; guest pools are 4 KB aligned, so a texture's edge pages are shared with data the game may write every frame. A texture
// whose only written pages are its edges, with its bytes there unchanged, is protected again instead of re-uploaded.
static void pkg_edge_ranges(const tex_t *t, uintptr_t r[2][2], uintptr_t *ia, uintptr_t *ie) {
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)t->mem, e = a + t->L.size;
    *ia = (a + pg - 1) & ~(pg - 1); *ie = e & ~(pg - 1);
    if (*ie <= *ia) { r[0][0] = a; r[0][1] = e; r[1][0] = r[1][1] = 0; *ia = *ie = 0; return; }
    r[0][0] = a; r[0][1] = *ia; r[1][0] = *ie; r[1][1] = e;
}
static void pkg_edges_drop(NvPkg *pk) {
    for (int i = 0; i < 2; i++) { if (!pk->eb) free(pk->edge[i]); pk->edge[i] = NULL; pk->edge_n[i] = 0; }
}
static void pkg_edges_take(NvPkg *pk, const tex_t *t) {
    pkg_edges_drop(pk);
    if (pk->sparse || !pk->span || !t->mem) return;
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    for (int i = 0; i < 2; i++) if (r[i][1] > r[i][0]) { pk->edge_n[i] = (uint32_t)(r[i][1] - r[i][0]);
        pk->edge[i] = pk->eb ? (uint8_t *)pk->eb.contents + (i ? r[0][1] - r[0][0] : 0) : malloc(pk->edge_n[i]); memcpy(pk->edge[i], (const void *)r[i][0], pk->edge_n[i]); }
    // a write after the upload's stamp may have reached the copy but not the upload: no copy to trust
    if (nvn_dirty_since_span(pk->span, t->mem, t->L.size, pk->stamp)) pkg_edges_drop(pk);
}
// A GPU store changes the edge bytes too, unseen by the tracker: from its first store on they live in a buffer the swz kernel writes as well. *e1s: ~0 when none
static id<MTLBuffer> pkg_edges_gpu(NvPkg *pk, const tex_t *t, uint64_t *e0n, uint64_t *e1s, uint64_t *e1n) {
    *e0n = *e1n = 0; *e1s = ~0ull;
    if (!pk || pk->sparse || !pk->span || !t->mem) return nil;
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    uint64_t n0 = r[0][1] - r[0][0], n1 = r[1][1] - r[1][0];
    if (!n0 && !n1) return nil;
    if (!pk->eb) {
        id<MTLBuffer> b = [g_dev newBufferWithLength:n0 + n1 options:MTLResourceStorageModeShared];
        if (!b) return nil;
        res_add(b);
        for (int i = 0; i < 2; i++) if (pk->edge_n[i]) { memcpy((uint8_t *)b.contents + (i ? n0 : 0), pk->edge[i], pk->edge_n[i]); free(pk->edge[i]); pk->edge[i] = (uint8_t *)b.contents + (i ? n0 : 0); }
        pk->eb = b;
    }
    *e0n = n0; if (n1) { *e1s = r[1][0] - (uintptr_t)t->mem; *e1n = n1; }
    return pk->eb;
}
static int pkg_inner_written(const NvPkg *pk, const tex_t *t) {
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    return ie > ia && nvn_dirty_since_span(pk->span, (const void *)ia, ie - ia, pk->stamp);
}
static int pkg_edges_clean(NvPkg *pk, const tex_t *t) {   // 1 = only edge pages were written, and the texture's bytes there are as uploaded
    if (!pk->edge_n[0] && !pk->edge_n[1]) return 0;
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size);   // take the stamp, protect, then read: a later write is seen at the next check
    if (ie > ia && nvn_dirty_since_span(pk->span, (const void *)ia, ie - ia, pk->stamp)) return 0;
    for (int i = 0; i < 2; i++) if (pk->edge_n[i] && memcmp(pk->edge[i], (const void *)r[i][0], pk->edge_n[i])) return 0;
    pk->stamp = st; g_pkg_edge_skips++;
    return 1;
}
// Metal runs texture atomics only on textures with the shader-atomic usage, and NVN does not say which images a shader will use atomically.
static inline int pf_r32i(MTLPixelFormat pf) { return pf == MTLPixelFormatR32Uint || pf == MTLPixelFormatR32Sint; }
// 64-bit texture atomics run on RG32Uint with the shader-atomic usage, on Apple GPU family 9 and later; older GPUs have none.
static int dev_atomic64(void) { static int v = -1; if (v < 0 && g_dev) v = [g_dev supportsFamily:MTLGPUFamilyApple9] ? 1 : 0; return v > 0; }
static inline int pf_atomic(MTLPixelFormat pf) { return pf_r32i(pf) || (pf == MTLPixelFormatRG32Uint && dev_atomic64()); }
static MTLTextureUsage img_usage(const tex_t *t, MTLPixelFormat pf) {
    if (!(t->d.flags & NVN_TEXTURE_FLAGS_IMAGE)) return 0;
    return MTLTextureUsageShaderWrite | (pf_atomic(pf) ? MTLTextureUsageShaderAtomic : 0);
}
// Metal leaves MTLTextureUsageShaderAtomic out of a texture's usage property: every shader-writable R32 texture the host makes has it, and this tests that.
static int tex_atomic_capable(id<MTLTexture> base) { return pf_atomic(base.pixelFormat) && (base.usage & MTLTextureUsageShaderWrite); }
// The 32-bit colour formats other than R32 integers, whose bytes NVN reads through an R32 image and Metal views as R32
static int pf_bytes32(MTLPixelFormat pf) {
    return pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB || pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRGBA8Snorm
        || pf == MTLPixelFormatRGBA8Uint || pf == MTLPixelFormatRGBA8Sint || pf == MTLPixelFormatRGB10A2Unorm || pf == MTLPixelFormatRGB10A2Uint || pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGB9E5Float
        || pf == MTLPixelFormatRG16Unorm || pf == MTLPixelFormatRG16Snorm || pf == MTLPixelFormatRG16Uint || pf == MTLPixelFormatRG16Sint || pf == MTLPixelFormatRG16Float || pf == MTLPixelFormatR32Float;
}
// Metal runs texture atomics only on R32 storage, NVN on any 32-bit format through an R32 image: such a texture moves to R32 storage with its own format as a view.
// Not packaged textures: those are the guest's bytes.
static int tex_atomic_alias(id<MTLTexture> base) {
    return pf_bytes32(base.pixelFormat) && (base.textureType == MTLTextureType2D || base.textureType == MTLTextureType2DArray) && base.sampleCount == 1 && base.storageMode == MTLStorageModePrivate && !base.buffer && !base.isSparse;
}
// Formats whose textures over one storage are views of one Metal texture: an sRGB / linear pair, or two of the 32-bit colour formats and R32 integers
static int pf_storage_twins(MTLPixelFormat a, MTLPixelFormat b) {
    if (a == b) return 0;
    if (!tex_need_pfv(a, b)) return 1;
    return (pf_bytes32(a) || pf_r32i(a)) && (pf_bytes32(b) || pf_r32i(b));
}
static MTLTextureDescriptor *tex_desc(const tex_t *t, int packaged) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf) { RED_ONCE(0, "texture format %#x has no Metal equivalent (first case)", t->d.format); return nil; }
    // past Metal's limits Metal aborts the process on an assertion
    if (t->d.width > 16384 || t->d.height > 16384 || t->d.depth > 2048) { RED_ONCE(71, "texture size exceeds Metal limits: %u×%u×%u levels %u format %#x target %d (first case ser=%llu)", t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, (unsigned long long)t->serial); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = (MTLPixelFormat)pf;
    d.width = t->d.width ? t->d.width : 1; d.height = t->d.height ? t->d.height : 1; d.depth = 1; d.arrayLength = 1;
    d.mipmapLevelCount = t->d.levels ? t->d.levels : 1;
    switch (t->d.target) {
        // 1D textures as one-row 2D: translated shaders always sample them as 2D, and a Metal 1D texture bound to a texture2d argument reads undefined.
        case 0: d.textureType = MTLTextureType2D; d.height = 1; break;
        case 2: d.textureType = MTLTextureType3D; d.depth = t->d.depth; break;
        case 3: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.height; d.height = 1; break;
        case 4: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 5: d.textureType = MTLTextureType2DMultisample; d.sampleCount = t->d.samples; d.mipmapLevelCount = 1; break;
        case 6: d.textureType = MTLTextureType2DMultisampleArray; d.sampleCount = t->d.samples; d.arrayLength = t->d.depth; d.mipmapLevelCount = 1; break;
        // Cube textures as 6-layer 2D arrays: Metal only allows cube array views of a 2D array base.
        case 8: d.textureType = MTLTextureType2DArray; d.arrayLength = 6; break;
        // NVN gives a cube array's depth in layer-faces; as a Metal cube array it could ask for more cubes than Metal's 341, and Metal aborts.
        case 9: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 10: RED_ONCE(1, "buffer textures (TARGET_BUFFER) not implemented"); return nil;
        default: d.textureType = MTLTextureType2D; break;
    }
    // Fields NVN takes that Metal refuses abort the process (validateWithDevice): no layers → one; extra levels are not made (tex_level_ok skips copies to them);
    // a sample count the GPU lacks, or of one sample, is not made.
    if (d.depth < 1) d.depth = 1;
    if (d.arrayLength < 1) d.arrayLength = 1;
    if (d.arrayLength > 2048) { RED_ONCE(113, "texture of %lu layers exceeds Metal's 2048: not made (format %#x target %d, first case ser=%llu)", (unsigned long)d.arrayLength, t->d.format, t->d.target, (unsigned long long)t->serial); return nil; }
    NSUInteger big = MAX(d.width, d.height), fit = 1;
    if (d.textureType == MTLTextureType3D) big = MAX(big, d.depth);
    while (big >> fit) fit++;
    if (d.mipmapLevelCount > fit) {
        RED_ONCE(114, "texture %u×%u×%u declares %lu levels, its size holds %lu: made with %lu, the others are not kept (format %#x, first case ser=%llu)", t->d.width, t->d.height, t->d.depth,
                 (unsigned long)d.mipmapLevelCount, (unsigned long)fit, (unsigned long)fit, t->d.format, (unsigned long long)t->serial);
        d.mipmapLevelCount = fit;
    }
    if (d.textureType == MTLTextureType2DMultisample || d.textureType == MTLTextureType2DMultisampleArray) {
        if (d.sampleCount < 2 || ![g_dev supportsTextureSampleCount:d.sampleCount]) {
            RED_ONCE(115, "multisample texture of %lu samples: this GPU has no such texture, not made (format %#x, first case ser=%llu)", (unsigned long)d.sampleCount, t->d.format, (unsigned long long)t->serial);
            return nil;
        }
    }
    d.storageMode = MTLStorageModePrivate;
    d.usage = MTLTextureUsageShaderRead | (fmt_compressed(t->d.format) || packaged ? 0 : MTLTextureUsageRenderTarget)
            | img_usage(t, d.pixelFormat);
    // The stencil is read through an X32_Stencil8 view, which Metal's contract allows only with the format-view usage; the validation layer aborts without it.
    if (d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) d.usage |= MTLTextureUsagePixelFormatView;
    // NVIDIA GPUs have no RGB9E5 colour target: a game draws into one through an R32_UI view, so without the usage each would be upgraded at its first pass.
    if (d.pixelFormat == MTLPixelFormatRGB9E5Float && (d.usage & MTLTextureUsageRenderTarget)) d.usage |= MTLTextureUsagePixelFormatView;
    return d;
}
// A 1D or 1D array texture is one row; a 1D array's NVN height is its layer count, which taken as rows writes past each slice.
static inline void tex_lsize(const tex_t *t, unsigned l, uint32_t *w, uint32_t *h, uint32_t *z) {
    uint32_t H = t->d.target == 0 || t->d.target == 3 ? 1u : t->d.height, Z = t->d.target == 2 ? t->d.depth : 1u;
    *w = l < 32 ? MAX(t->d.width >> l, 1u) : 1u; *h = l < 32 ? MAX(H >> l, 1u) : 1u;
    if (z) *z = l < 32 ? MAX(Z >> l, 1u) : 1u;
}
// Without the format-view usage (it costs lossless compression), a texture needing a different-format view is remade with it and its contents copied. Also adds
// render-target usage for a whole-level clear.
static id<MTLTexture> tex_upgrade(const tex_t *t, id<MTLTexture> asked, MTLTextureUsage add, MTLPixelFormat view_pf) {
    id holder = tex_get(t->serial);
    // the parent is upgraded and the view of its storage (an sRGB / linear twin, another 32-bit format) made again on it, so the two keep sharing its bytes
    id<MTLTexture> view = ![holder isKindOfClass:[NvPkg class]] && obj_is_tex(holder) && ((id<MTLTexture>)holder).parentTexture
        && (holder == asked || ((id<MTLTexture>)holder).parentTexture == asked) ? holder : nil;
    id<MTLTexture> base = view ? view.parentTexture : asked;
    if (base.isSparse) { RED_ONCE(149, "a placement sparse texture needs usage %#lx it was not made with (ser=%llu format %#x): kept as it is, not remade whole", (unsigned long)add, (unsigned long long)t->serial, t->d.format); return base; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = base.textureType; d.pixelFormat = base.pixelFormat; d.width = base.width; d.height = base.height; d.depth = base.depth;
    d.arrayLength = base.arrayLength; d.mipmapLevelCount = base.mipmapLevelCount; d.sampleCount = base.sampleCount;
    d.storageMode = base.storageMode; d.cpuCacheMode = base.cpuCacheMode; d.hazardTrackingMode = base.hazardTrackingMode;
    d.resourceOptions = base.resourceOptions; d.usage = base.usage | add | (tex_atomic_capable(base) ? MTLTextureUsageShaderAtomic : 0);   // the usage property leaves the atomic usage out
    // A linear texture aliases the guest pool: adding usage must keep that backing, or later GPU writes never reach the CPU map.
    id<MTLBuffer> backing = base.buffer;
    int reinterpret = (add & MTLTextureUsageShaderAtomic) && !pf_atomic(base.pixelFormat);
    if (reinterpret && (!tex_atomic_alias(base) || !pf_r32i(view_pf) || [holder isKindOfClass:[NvPkg class]])) return asked;
    id<MTLTexture> upgraded;
    if (reinterpret) {
        d.pixelFormat = view_pf; d.usage |= MTLTextureUsagePixelFormatView;
        id<MTLTexture> raw = [g_dev newTextureWithDescriptor:d];
        upgraded = raw ? [raw newTextureViewWithPixelFormat:base.pixelFormat] : nil;
        if (upgraded) { res_add(raw); NvAtomicRaw *own = [NvAtomicRaw new]; own->raw = raw; objc_setAssociatedObject(upgraded, &g_atomic_raw_key, own, OBJC_ASSOCIATION_RETAIN_NONATOMIC); }
    } else upgraded = backing ? [backing newTextureWithDescriptor:d offset:base.bufferOffset bytesPerRow:base.bufferBytesPerRow] : [g_dev newTextureWithDescriptor:d];
    if (!upgraded) { RED_ONCE(45, "texture upgrade failed ser=%llu format %lu → %lu usage %#lx", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, (unsigned long)d.usage); return base; }
    if (!reinterpret) res_add(upgraded);
    id<MTLTexture> upgraded_view = nil;
    id<MTL4ComputeCommandEncoder> b = backing ? nil : blit();   // shared backing already holds the pixels; copying it onto itself would overlap
    NSUInteger slices = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    for (NSUInteger l = 0; !backing && l < base.mipmapLevelCount; l++) {
        NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = base.textureType == MTLTextureType3D ? MAX(base.depth >> l, 1u) : 1;
        if (base.textureType == MTLTextureType3D)
            enc_b_copy_texture(b, base, 0, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, z), upgraded, 0, l, MTLOriginMake(0, 0, 0));
        else for (NSUInteger s = 0; s < slices; s++)
            enc_b_copy_texture(b, base, s, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), upgraded, s, l, MTLOriginMake(0, 0, 0));
    }
    if ([holder isKindOfClass:[NvPkg class]]) { id<MTLTexture> old = ((NvPkg *)holder)->tex; ((NvPkg *)holder)->tex = upgraded; defer_obj_release(old); own_epoch();
        if (((NvPkg *)holder)->view) { defer_obj_unref(((NvPkg *)holder)->view); ((NvPkg *)holder)->view = nil; ((NvPkg *)holder)->vbase = 0; } }
    else {
        // Every name of the old texture moves to the upgraded one, or readers would keep the old copy. Slots are replaced in place (no resize during the walk); an sRGB / linear
        // twin's view is published in the rename's hold, so no reader finds the old view once its parent left the sharing table.
        if (view) {
            upgraded_view = [upgraded newTextureViewWithPixelFormat:view.pixelFormat textureType:view.textureType levels:NSMakeRange(0, upgraded.mipmapLevelCount) slices:NSMakeRange(0, upgraded.arrayLength) swizzle:view.swizzle];
            static int nview; if (nview++ < 4) MLOG("storage view ser=%llu made again on its upgraded parent (%d so far)", (unsigned long long)t->serial, nview);
            if (!upgraded_view) RED_ONCE(151, "cannot make the storage view ser=%llu again on its upgraded parent: it keeps the old storage", (unsigned long long)t->serial);
        }
        own_upgrade(base, upgraded, t->serial, view ? upgraded_view : upgraded);
        defer_obj_release(base);
    }
    MLOG("texture upgraded to allow %s ser=%llu base format %lu → view format %lu (upgrades=%llu)", add & MTLTextureUsageShaderWrite ? "image writes" : add & MTLTextureUsageRenderTarget ? "render-target use" : "format views", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, ++g_pfv_upgrades);
    return upgraded_view ? upgraded_view : upgraded;
}
static id<MTLTexture> tex_upgrade_pfv(const tex_t *t, id<MTLTexture> base, MTLPixelFormat view_pf) { return tex_upgrade(t, base, MTLTextureUsagePixelFormatView, view_pf); }
// Packaged texture upload: take the stamp, then protect, then read. A write that faults while the pages are being protected gets a generation above the stamp; a stamp taken
// after the protect would miss it. Uploads of a submission share one command buffer, committed before the replay's on the same queue.
static _Atomic uint64_t g_upload_ns;
static _Atomic uint64_t g_upload_gpu_ns;
static id<MTL4CommandBuffer> g_up_cb;
static id<MTL4ComputeCommandEncoder> g_up_ce;
static _Atomic int g_up_alloc_busy[3];
// The completion handler broadcasts after it updates them; the 10 ms timeout is only a safety net.
static pthread_mutex_t g_up_wmu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_up_wcv = PTHREAD_COND_INITIALIZER;
static void up_wake(void) { pthread_mutex_lock(&g_up_wmu); pthread_cond_broadcast(&g_up_wcv); pthread_mutex_unlock(&g_up_wmu); }
static void up_wait_zero(_Atomic int *v, int site) {
    if (!atomic_load(v)) { if (ws_on()) ws_put(site, 0, 0); return; }
    if (atomic_load(&g_hold_on) && on_sub_thread()) hold_block(0);   // the batch may be a held commit (nvn_rec.h)
    uint64_t w0 = ws_t0();
    pthread_mutex_lock(&g_up_wmu);
    while (atomic_load(v)) { struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 10000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } pthread_cond_timedwait(&g_up_wcv, &g_up_wmu, &dl); }
    pthread_mutex_unlock(&g_up_wmu);
    if (ws_on()) ws_put(site, w0, 1);
}
static unsigned g_up_seq;
#define UP_BLOCK_SIZE (32u * 1024u * 1024u)
// Each slot keeps its staging resident until its next batch, its first block grown to the previous batch's demand, plus one overflow.
#define UP_SLOT_BLOCK (4u * 1024u * 1024u)
static __strong id<MTLBuffer> g_up_slot_blk[3], g_up_slot_over[3]; static unsigned g_up_cur_slot;
static size_t g_up_slot_want[3], g_up_batch_bytes; static int g_up_over_used;
static size_t g_up_src_bytes;
static size_t up_src_budget(void) {
    static size_t bytes = SIZE_MAX;
    if (bytes == SIZE_MAX) {
        const char *e = getenv("DM_NVN_UPLOAD_SRC_MB");
        char *end;
        long long mb = e ? strtoll(e, &end, 10) : 0;
        bytes = mb > 0 && (unsigned long long)mb <= (SIZE_MAX >> 20) && !*end ? (size_t)mb << 20 : 0;
    }
    return bytes;
}
static int up_reuse(void) {
    static int on = -1;
    if (on < 0) { const char *e = getenv("DM_NVN_UPLOAD_REUSE"); on = !e || atoi(e); }
    return on;
}
static int up_retained(id<MTLBuffer> b, unsigned slot) { return b && (b == g_up_slot_blk[slot] || b == g_up_slot_over[slot]); }
static size_t up_slot_block_size(size_t demand) {
    size_t want = UP_SLOT_BLOCK;
    while (want < demand) want <<= 1;
    return want;
}

static int g_up_idle_report;
static uint64_t up_idle_period(void) {
    static int64_t ns = -1;
    if (ns < 0) {
        const char *e = getenv("DM_NVN_UPLOAD_IDLE_MS");
        g_up_idle_report = e && *e;
        char *end;
        int64_t ms = g_up_idle_report ? strtoll(e, &end, 10) : 0;
        ns = ms > 0 && ms <= INT64_MAX / 1000000 && !*end ? ms * 1000000 : 0;
    }
    return (uint64_t)ns;
}
typedef struct {
    uint64_t used_ns, overflow_ns, window_ns;
    size_t peak;
} up_idle_slot;
static up_idle_slot g_up_idle_slot[3];
static uint64_t g_up_idle_n, g_up_idle_bytes, g_up_blocks_new;

static void up_idle_drop(__strong id<MTLBuffer> *block) {
    if (!*block) return;
    g_up_idle_n++;
    g_up_idle_bytes += (*block).length;
    res_remove(*block);
    *block = nil;
}

static void up_idle_release(void) {
    uint64_t idle = up_idle_period();
    if (!g_up_idle_report || g_up_cb) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    uint64_t before = g_up_idle_n;
    for (unsigned s = 0; idle && s < 3; s++) {
        // the feedback handler clears busy after GPU completion
        up_idle_slot *age = &g_up_idle_slot[s];
        if (!age->used_ns || atomic_load(&g_up_alloc_busy[s])) continue;
        if (now - age->used_ns >= idle) {
            up_idle_drop(&g_up_slot_blk[s]);
            up_idle_drop(&g_up_slot_over[s]);
            g_up_slot_want[s] = 0;
            *age = (up_idle_slot){0};
            continue;
        }
        if (g_up_slot_over[s] && now - age->overflow_ns >= idle) up_idle_drop(&g_up_slot_over[s]);
        if (now - age->window_ns >= idle) {
            if (g_up_slot_blk[s].length > up_slot_block_size(age->peak)) up_idle_drop(&g_up_slot_blk[s]);
            g_up_slot_want[s] = age->peak;
            age->peak = 0;
            age->window_ns = now;
        }
    }
    // commit the removals before this submission can allocate more GPU memory
    if (g_up_idle_n != before) res_commit();
    static uint64_t said_ns;
    if (!said_ns || now - said_ns >= 30000000000ull || (!before && g_up_idle_n)) {
        said_ns = now;
        size_t held = 0;
        for (unsigned s = 0; s < 3; s++) held += g_up_slot_blk[s].length + g_up_slot_over[s].length;
        MLOG("upload staging: slots hold %zu MiB; %llu blocks allocated, %llu trimmed (%llu MiB total)", held >> 20,
             (unsigned long long)g_up_blocks_new, (unsigned long long)g_up_idle_n, (unsigned long long)(g_up_idle_bytes >> 20));
    }
}
typedef struct {
    __unsafe_unretained id<MTLBuffer> src;
    size_t off, row, image;
    MTLSize size;
    __unsafe_unretained id<MTLTexture> dst;   // strongly referenced by g_up_texs until the flush is encoded
    NSUInteger slice, level;
    MTLOrigin at;
    __unsafe_unretained id<MTLTexture> tsrc;   // copied from this texture's same slice and level instead of from src; kept by g_up_views
    uint8_t t2b, bar;   // t2b: tsrc's region goes into src at off instead; bar: behind a barrier after the copies before it
} up_blit;
static up_blit *g_up_blits; static int g_nup_blits, g_up_blitcap;
static NSMutableArray *g_up_texs, *g_up_views;
static id<MTLBuffer> g_up_block;
static size_t g_up_block_off, g_up_block_size;
static NSMutableArray *g_up_keep;
// a single texture larger than the budget gets its own block (uploads are not tiled here)
static size_t g_up_staging_bytes;
#define UP_BATCH_BYTES (128u * 1024u * 1024u)


// A batch the system discarded (GPU error recovery) wrote no content: its textures are re-uploaded at their next fetch. DM_TEST_UP_LOST tests the path.
static _Atomic uint64_t g_up_lost_gen;   // a texture holding images copied from its predecessor cannot tell whether a drop reached them, and is uploaded whole
static NSHashTable *g_up_lost; static os_unfair_lock g_up_lost_lk = OS_UNFAIR_LOCK_INIT; static _Atomic int g_up_lost_n; static _Atomic uint64_t g_up_lost_redo;   // weak: a texture never fetched again is not kept alive
static int up_lost_take(id tex) {
    if (!tex) return 0; os_unfair_lock_lock(&g_up_lost_lk); int hit = [g_up_lost containsObject:tex]; if (hit) { [g_up_lost removeObject:tex]; atomic_store(&g_up_lost_n, (int)g_up_lost.allObjects.count); atomic_fetch_add(&g_up_lost_redo, 1); } os_unfair_lock_unlock(&g_up_lost_lk); return hit;
}
static void up_diag(void);
static void up_flush(void) {
    up_owner_check("up_flush");
    if (!g_up_cb) return;
    if (g_up_ce) {
        m4_end(g_up_ce); g_up_ce = nil;
    }
    id<MTL4ComputeCommandEncoder> b = g_nup_blits ? g_up_ce = [g_up_cb computeCommandEncoder] : nil;
    if (b) [b barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    id<MTLTexture> from = nil;
    for (int i = 0; b && i < g_nup_blits; i++) {
        up_blit *x = &g_up_blits[i];
        if (x->bar) [b barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice];
        if (x->t2b) { enc_b_copy_texture_buffer(b, x->tsrc, x->slice, x->level, x->at, x->size, x->src, x->off, x->row, x->image); continue; }
        if (!x->tsrc) { enc_b_copy_buffer_texture(b, x->src, x->off, x->row, x->image, x->size, x->dst, x->slice, x->level, x->at); continue; }
        // the source may be the target of this encoder's earlier copies
        if (x->tsrc != from) { from = x->tsrc; [b barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice]; }
        enc_b_copy_texture(b, x->tsrc, x->slice, x->level, MTLOriginMake(0, 0, 0), x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    if (b) { m4_end(b); g_up_ce = nil; }
    id<MTL4CommandBuffer> cb = g_up_cb;
    unsigned slot = (g_up_seq - 1) % 3;
    // retired on upload completion, even while the main submission is still being built
    NSMutableArray *staging = g_up_keep;
    if (g_up_block && !up_retained(g_up_block, slot)) [staging addObject:g_up_block];
    NSMutableArray *views = g_up_views; g_up_views = nil;
    g_up_keep = nil; g_up_block = nil;
    g_up_block_off = g_up_block_size = g_up_staging_bytes = 0;
    NSMutableSet *targets = [NSMutableSet setWithArray:g_up_texs]; for (int i = 0; i < g_nup_blits; i++) if (g_up_blits[i].dst) [targets addObject:g_up_blits[i].dst];
    NSArray *dsts = targets.allObjects;
    static int lost_at = -2; static uint64_t batch_n; if (lost_at == -2) { const char *e = getenv("DM_TEST_UP_LOST"); lost_at = e ? atoi(e) : -1; } int fake_lost = dsts.count && lost_at > 0 && (int)++batch_n == lost_at;
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    atomic_fetch_add(&g_up_pending, 1);
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) {
        if (f.error) { MLOG("[RED] upload command buffer execution error: %s", f.error.description.UTF8String); res_top_once(f.error); gpu_lost_check(f.error, "upload"); }
        if (f.error || fake_lost) {
            atomic_fetch_add(&g_up_lost_gen, 1);
            os_unfair_lock_lock(&g_up_lost_lk); if (!g_up_lost) g_up_lost = [NSHashTable weakObjectsHashTable]; for (id d in dsts) [g_up_lost addObject:d]; int n = (int)g_up_lost.allObjects.count; atomic_store(&g_up_lost_n, n); os_unfair_lock_unlock(&g_up_lost_lk);
            own_epoch();   // on a cache hit tex_mtl is not passed through
            MLOG("%supload command buffer dropped: the %lu textures of this batch are marked unwritten and re-uploaded on next use (%d awaiting re-upload)", fake_lost ? "DM_TEST_UP_LOST: " : "", (unsigned long)dsts.count, n);
        }
        double d = f.GPUEndTime - f.GPUStartTime;
        if (d > 0) atomic_fetch_add(&g_upload_gpu_ns, (uint64_t)(d * 1e9));
        @autoreleasepool {
            for (id<MTLResource> obj in staging) res_remove(obj);
            [staging removeAllObjects];
            [views removeAllObjects];
            res_commit();
        }
        atomic_store(&g_up_alloc_busy[slot], 0);
        atomic_fetch_sub(&g_up_pending, 1);
        up_wake();
    }];
    res_commit();
    cb_end(cb, nil);
    rec_commit_upload(cb, o);
    // Outside a submission nothing else hands the encode thread its current block: a batch flushed on the spot would stay a record that every wait waits for.
    if (!g_cb) rec_publish();
    if (up_reuse()) g_up_slot_want[slot] = MAX(g_up_slot_want[slot], g_up_batch_bytes);
    if (g_up_batch_bytes && up_idle_period()) {
        up_idle_slot *age = &g_up_idle_slot[slot];
        age->used_ns = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        if (g_up_over_used) age->overflow_ns = age->used_ns;
        if (!age->window_ns) age->window_ns = age->used_ns;
        age->peak = MAX(age->peak, g_up_batch_bytes);
    }
    g_up_cb = nil;
    up_diag();
    g_nup_blits = 0;
    [g_up_texs removeAllObjects];
}

static void up_begin(void) {
    up_owner_check("up_begin");
    if (g_up_cb) return;
    unsigned slot = g_up_seq++ % 3;
    // The slot's last batch may still be a commit record in the encode thread's current block: waiting without handing it over waits for good.
    if (atomic_load(&g_up_alloc_busy[slot])) rec_publish();
    up_wait_zero(&g_up_alloc_busy[slot], WS_UPALLOC);
    atomic_store(&g_up_alloc_busy[slot], 1); g_up_cur_slot = slot;
    g_up_batch_bytes = g_up_src_bytes = 0; g_up_over_used = 0;
    [g_up_alloc4[slot] reset];
    g_up_cb = [g_dev newCommandBuffer];
    [g_up_cb beginCommandBufferWithAllocator:g_up_alloc4[slot]];
    g_up_ce = [g_up_cb computeCommandEncoder];
    [g_up_ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
    [g_up_ce setArgumentTable:g_up_ct];
    if (!g_up_texs) g_up_texs = [NSMutableArray array];
    if (!g_up_keep) g_up_keep = [NSMutableArray array];
}

static uint64_t g_up_swaps, g_up_swap_bytes; static NSHashTable *g_up_old;
static void up_diag(void) { @autoreleasepool {
    // weak-table enumeration autoreleases the buffers it finds
    static uint64_t said_swaps = ~0ull, last_ns; static int lines, follow; uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    int chg = g_up_swaps != said_swaps, fol = follow && now - last_ns >= 5000000000ull; if ((!chg && !fol) || lines >= 200) return;
    if (chg && last_ns && now - last_ns < 1000000000ull) return; last_ns = now; said_swaps = g_up_swaps; lines++; follow = chg;
    unsigned long alive = 0, inres = 0; uint64_t abytes = 0; NSArray *olds = g_up_old.allObjects;
    pthread_mutex_lock(&g_res_mu); unsigned long nres = (unsigned long)g_res_objects.allObjects.count; for (id<MTLBuffer> o in olds) { alive++; abytes += o.length; inres += [g_res_objects containsObject:o]; } pthread_mutex_unlock(&g_res_mu);
    pthread_mutex_lock(&g_deferred_mu); unsigned long ndef = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    MLOG("upload staging blocks%s: %llu created in total, swapped within a batch %llu times (%llu MB of old blocks swapped out); swapped-out blocks still alive now %lu, %llu MB, still in the residency set %lu; GPU allocated %llu MB, residency set %lu objects, deferred-release queue %lu", fol && !chg ? " (settled)" : "",
         (unsigned long long)g_up_blocks_new, (unsigned long long)g_up_swaps, (unsigned long long)(g_up_swap_bytes >> 20), alive, (unsigned long long)(abytes >> 20), inres, (unsigned long long)(g_dev.currentAllocatedSize >> 20), nres, ndef);
} }
static id<MTLBuffer> up_alloc(size_t n, size_t *off) {
    size_t need = (n + 255u) & ~((size_t)255u);
    size_t extra = MAX(need, UP_BLOCK_SIZE);
    // prior deswizzles and copies are complete records, so the batch can be split without ending the render pass
    if (g_up_staging_bytes && (!g_up_block || need > g_up_block_size - g_up_block_off)
        && extra > UP_BATCH_BYTES - MIN(g_up_staging_bytes, UP_BATCH_BYTES)) {
        up_flush();
        up_begin();
    }
    g_up_batch_bytes += MIN(need, UP_BLOCK_SIZE - g_up_batch_bytes);
    if (!g_up_block && need <= UP_BLOCK_SIZE) {   // the slot's own block, grown to the next power of two that holds it
        __strong id<MTLBuffer> *sb = &g_up_slot_blk[g_up_cur_slot];
        size_t demand = up_reuse() ? MAX(need, g_up_slot_want[g_up_cur_slot]) : need;
        size_t want = up_slot_block_size(demand);
        if (*sb && (*sb).length < want) { defer_obj_release(*sb); *sb = nil; }   // the slot's previous batch has finished (up_begin waited for it)
        if (!*sb) { *sb = [g_dev newBufferWithLength:want options:MTLResourceStorageModePrivate]; if (!*sb) MDIE("failed to create the swizzled-upload staging block %#zx", want); res_add(*sb); g_up_blocks_new++; }
        g_up_block = *sb; g_up_block_size = (*sb).length; g_up_block_off = 0;
        g_up_staging_bytes += (*sb).length;
    }
    if (!g_up_block || g_up_block_off + need > g_up_block_size) {
        // The old block is still referenced by commands of this batch: handed to g_up_keep, retired with the other staging after the flush commits.
        if (g_up_block) { g_up_swaps++; g_up_swap_bytes += g_up_block_size; if (!up_retained(g_up_block, g_up_cur_slot)) { @autoreleasepool { if (!g_up_old) g_up_old = [NSHashTable weakObjectsHashTable]; [g_up_old addObject:g_up_block]; } }
            if (!up_retained(g_up_block, g_up_cur_slot)) { if (!g_up_keep) g_up_keep = [NSMutableArray array]; [g_up_keep addObject:g_up_block]; } }
        if (up_reuse() && need <= UP_BLOCK_SIZE && !g_up_over_used) {
            __strong id<MTLBuffer> *sb = &g_up_slot_over[g_up_cur_slot];
            if (!*sb) { *sb = [g_dev newBufferWithLength:UP_BLOCK_SIZE options:MTLResourceStorageModePrivate]; if (!*sb) MDIE("failed to create the overflow staging block"); res_add(*sb); g_up_blocks_new++; }
            g_up_block = *sb; g_up_block_size = (*sb).length; g_up_over_used = 1;
        } else {
            g_up_block_size = n > UP_BLOCK_SIZE ? need : UP_BLOCK_SIZE;
            g_up_block = [g_dev newBufferWithLength:g_up_block_size options:MTLResourceStorageModePrivate];
            res_add(g_up_block); g_up_blocks_new++;
        }
        if (!g_up_block) MDIE("failed to create the swizzled-upload staging block %#zx", g_up_block_size);
        g_up_staging_bytes += g_up_block_size;
        g_up_block_off = 0;
    }
    *off = g_up_block_off;
    g_up_block_off += need;
    return g_up_block;
}
// On the submit thread at a submission's start, which owns the upload slots: only what no frame needs and nothing waits for goes.
static uint64_t pkg_evict_idle(unsigned *n_out);
static void mem_trim(void) {
    int lv = atomic_exchange(&g_mp_want, 0);
    if (!lv) return;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    unsigned rn = 0; uint64_t rb = rh_trim_n(1, &rn);
    unsigned sn = 0; uint64_t sb = spare_trim(1, &sn);
    unsigned en = 0; uint64_t eb = pkg_evict_idle(&en);
    unsigned un = 0; uint64_t ub = 0;
    for (unsigned s = 0; s < 3; s++) {
        if ((g_up_cb && s == g_up_cur_slot) || atomic_load(&g_up_alloc_busy[s])) continue;
        __strong id<MTLBuffer> *bs[2] = {&g_up_slot_blk[s], &g_up_slot_over[s]};
        int dropped = 0;
        for (int k = 0; k < 2; k++) if (*bs[k] && *bs[k] != g_up_block) { un++; ub += (*bs[k]).length; defer_obj_release(*bs[k]); *bs[k] = nil; dropped = 1; }
        if (dropped) { g_up_slot_want[s] = 0; g_up_idle_slot[s] = (up_idle_slot){0}; }
    }
    pthread_mutex_lock(&g_deferred_mu); unsigned long d0 = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    defer_drain();
    pthread_mutex_lock(&g_deferred_mu); unsigned long d1 = (unsigned long)g_deferred.count; pthread_mutex_unlock(&g_deferred_mu);
    res_commit();
    atomic_fetch_add(&g_mp_trims, 1); atomic_fetch_add(&g_mp_freed, rb + sb + eb + ub);
    sys_mem_t m = sys_mem();
    MLOG("[mem] memory pressure trim (%s): recycled render-target heaps %u, %llu MB; packaged textures' spares %u, %llu MB; uploaded textures idle 30 s %u, %llu MB (of %u packages: used lately %u, sparse / placed / a view %u, GPU-written %u, held %u); upload staging blocks %u, %llu MB (freed when their submission completes); deferred releases drained %lu, still waiting for the GPU %lu; %.1f ms; system free %llu MB, wired %llu MB, residency set %llu MB",
         lv == 2 ? "critical" : "warn", rn, (unsigned long long)(rb >> 20), sn, (unsigned long long)(sb >> 20), en, (unsigned long long)(eb >> 20), g_evict_why[0], g_evict_why[1], g_evict_why[2], g_evict_why[3], g_evict_why[4], un, (unsigned long long)(ub >> 20), d0 > d1 ? d0 - d1 : 0, d1, (clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0) / 1e6,
         (unsigned long long)m.free, (unsigned long long)m.wired, (unsigned long long)(g_res ? g_res.allocatedSize >> 20 : 0));
}

// 64 bytes per cell, cut from a 64 KB shared block; the old block is released by fence
static id<MTLBuffer> g_up_pblock; static size_t g_up_pblock_off;
static id<MTLBuffer> up_param_alloc(size_t *off) {
    if (!g_up_pblock || g_up_pblock_off + 64 > g_up_pblock.length) {
        if (g_up_pblock) defer_obj_release(g_up_pblock);
        g_up_pblock = [g_dev newBufferWithLength:64 << 10 options:MTLResourceStorageModeShared];
        if (!g_up_pblock) MDIE("failed to create the deswizzle parameter block");
        res_add(g_up_pblock); g_up_pblock_off = 0;
    }
    *off = g_up_pblock_off; g_up_pblock_off += 64;
    return g_up_pblock;
}
static void up_blit_add_at(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                           id<MTLTexture> dst, NSUInteger slice, NSUInteger level, MTLOrigin at) {
    if (g_nup_blits == g_up_blitcap) {
        g_up_blitcap = g_up_blitcap ? g_up_blitcap * 2 : 256;
        g_up_blits = dm_realloc(g_up_blits, g_up_blitcap * sizeof *g_up_blits, "the upload blits");
    }
    g_up_blits[g_nup_blits++] = (up_blit){src, off, row, image, size, dst, slice, level, at, nil};
}
// behind a barrier (it may read what earlier copies wrote); the buffer→texture copy that reads it next goes behind one too
static void up_t2b_add(id<MTLTexture> src, NSUInteger slice, NSUInteger level, MTLOrigin at, MTLSize size, id<MTLBuffer> dst, size_t off, size_t row, size_t image) {
    up_blit_add_at(dst, off, row, image, size, nil, slice, level, at);
    up_blit *x = &g_up_blits[g_nup_blits - 1]; x->tsrc = src; x->t2b = 1; x->bar = 1;
    if (!g_up_views) g_up_views = [NSMutableArray array];
    [g_up_views addObject:src];
}
static void up_copy_add(id<MTLTexture> src, MTLSize size, id<MTLTexture> dst, NSUInteger slice, NSUInteger level) {
    up_blit_add_at(nil, 0, 0, 0, size, dst, slice, level, MTLOriginMake(0, 0, 0));
    g_up_blits[g_nup_blits - 1].tsrc = src;
    if (!g_up_views) g_up_views = [NSMutableArray array];
    if (g_up_views.lastObject != src) [g_up_views addObject:src];
}
static void up_blit_add(id<MTLBuffer> src, size_t off, size_t row, size_t image, MTLSize size,
                        id<MTLTexture> dst, NSUInteger slice, NSUInteger level) { up_blit_add_at(src, off, row, image, size, dst, slice, level, MTLOriginMake(0, 0, 0)); }

static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse, MTLTextureUsage keep, id<MTLTexture> prev, const void *span, uint64_t since, unsigned *taken);
static int g_direct_upload_on = -1; static uint64_t g_direct_uploads, g_direct_dispatches;
static int direct_upload_on(void) { if (g_direct_upload_on < 0) { const char *e=getenv("DM_NVN_DIRECT_UPLOAD"); g_direct_upload_on=!e||atoi(e); } return g_direct_upload_on; }
int nvn_host_readable(uint64_t a, uint64_t n);
int nvn_sparse_pool(const void *pool); uint64_t nvn_sparse_gen(void); uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n); int nvn_sparse_notes(uint64_t *seen, uint64_t (*out)[2], int max);
int nvn_sparse_pages(const void *pool, uint64_t a, uint64_t n, const uint8_t **out);
// A sparse pool's texture uploaded whole is gathered on the GPU through a table of each 4 KB page's GPU address, in the queue's order: a CPU gather at encode time would read
// the memory before an earlier command buffer's writes reached it. → 0 when a mapped page lies in memory the GPU cannot address (the caller keeps the CPU's way).
static uint64_t sp_pages_n(uint64_t va, uint64_t n) { return ((va + n + 0xFFF) >> 12) - (va >> 12); }
// A CPU gather reads that memory when encoded, not in queue order, so it waits for the stores behind it (sp_store_wait). A ring: a store pushed out leaves its fence (g_sps_lost).
enum { SPS_N = 256 };
static struct { uint64_t lo, hi, fence; } g_sps[SPS_N]; static unsigned g_nsps; static uint64_t g_sps_lost;
static void sps_note(uint64_t lo, uint64_t n) {
    uint64_t f = g_fence + 1, i = g_nsps % SPS_N;
    if (g_nsps >= SPS_N && g_sps[i].fence > g_done && g_sps[i].fence > g_sps_lost) g_sps_lost = g_sps[i].fence;
    g_sps[i].lo = lo; g_sps[i].hi = lo + n; g_sps[i].fence = f; g_nsps++;
}
static id<MTLBuffer> up_big_src(uint64_t addr, uint64_t len, uint64_t *off, int may_split);
// A gather reads through zero-copy buffers the upload batch holds and retires, over spans of sorted pages across gaps of up to SP_GAP: the pools' own buffers would stay
// in the residency set for good.
enum { SP_GAP = 1u << 20, SP_SRC_N = 256 };
static struct { uintptr_t a, b; __unsafe_unretained id<MTLBuffer> buf; } g_sp_src[SP_SRC_N]; static int g_nsp_src; static unsigned g_sp_src_seq;   // this batch's (g_up_seq); g_up_keep holds them
static int sp_src_gpu(uint64_t a, uint64_t len, uint64_t *g) {
    if (g_sp_src_seq != g_up_seq) { g_nsp_src = 0; g_sp_src_seq = g_up_seq; }
    for (int k = 0; k < g_nsp_src; k++) if (a >= g_sp_src[k].a && a + len <= g_sp_src[k].b) { *g = g_sp_src[k].buf.gpuAddress + (a - g_sp_src[k].a); return 1; }
    uint64_t o; void *b; id<MTLBuffer> u = up_big_src(a, len, &o, 0);   // every source the gather's page table names must stay in this batch
    if (!u) return mtl_gpu_addr(a, len, &b, &o, g);
    if (g_nsp_src < SP_SRC_N) { g_sp_src[g_nsp_src].a = a - o; g_sp_src[g_nsp_src].b = a - o + u.length; g_sp_src[g_nsp_src++].buf = u; }
    *g = u.gpuAddress + o; return 1;
}
typedef struct { uint64_t p, k; } sp_pg;
static int sp_pg_cmp(const void *x, const void *y) { uint64_t a = ((const sp_pg *)x)->p, b = ((const sp_pg *)y)->p; return (a > b) - (a < b); }
// A gather's table names buffers of the upload batch it is built in, and runs in that batch.
static int sp_pages_gpu(const void *pool, uint64_t va, uint64_t n, uint64_t *pt, int wrote) {   // wrote: the GPU writes the pages
    uint64_t np = sp_pages_n(va, n); const uint8_t **ph = malloc((size_t)np * sizeof *ph);
    if (!ph) MDIE("sparse page table of %llu pages", (unsigned long long)np);
    int ok = nvn_sparse_pages(pool, va & ~0xFFFull, np << 12, ph);
    if (!ok) memset(pt, 0, (size_t)np * sizeof *pt);
    if (ok && wrote) for (uint64_t i = 0; i < np;) {
        if (!ph[i]) { pt[i++] = 0; continue; }
        uint64_t j = i + 1; while (j < np && ph[j] == ph[j - 1] + 0x1000) j++;
        void *b; uint64_t o, g;
        sbw_note((uint64_t)(uintptr_t)ph[i], (j - i) << 12, SBW_GPU); sps_note((uint64_t)(uintptr_t)ph[i], (j - i) << 12);
        if (mtl_gpu_addr((uint64_t)(uintptr_t)ph[i], (j - i) << 12, &b, &o, &g)) { for (uint64_t k = i; k < j; k++) pt[k] = g + ((k - i) << 12); i = j; continue; }
        for (; i < j; i++) { pt[i] = mtl_gpu_addr((uint64_t)(uintptr_t)ph[i], 0x1000, &b, &o, &g) ? g : 0; ok &= pt[i] != 0; }   // a run over two pools
    } else if (ok) {
        sp_pg *v = malloc((size_t)np * sizeof *v); uint64_t m = 0;
        if (!v) MDIE("sparse page list of %llu pages", (unsigned long long)np);
        for (uint64_t k = 0; k < np; k++) { pt[k] = 0; if (ph[k]) { v[m].p = (uint64_t)(uintptr_t)ph[k]; v[m++].k = k; } }
        qsort(v, (size_t)m, sizeof *v, sp_pg_cmp);
        for (uint64_t i = 0, j, g; i < m; i = j) {
            for (j = i + 1; j < m && v[j].p <= v[j - 1].p + 0x1000 + SP_GAP; j++) {}
            if (sp_src_gpu(v[i].p, v[j - 1].p + 0x1000 - v[i].p, &g)) { for (uint64_t x = i; x < j; x++) pt[v[x].k] = g + (v[x].p - v[i].p); continue; }
            for (uint64_t x = i, e; x < j; x = e) {
                for (e = x + 1; e < j && v[e].p <= v[e - 1].p + 0x1000; e++) {}
                if (sp_src_gpu(v[x].p, v[e - 1].p + 0x1000 - v[x].p, &g)) { for (uint64_t y = x; y < e; y++) pt[v[y].k] = g + (v[y].p - v[x].p); continue; }
                for (uint64_t y = x; y < e; y++) { if (sp_src_gpu(v[y].p, 0x1000, &g)) pt[v[y].k] = g; else ok = 0; }
            }
        }
        free(v);
    }
    free(ph);
    if (!ok) RED_ONCE(171, "a page mapped in a sparse pool lies in memory the GPU cannot address: its textures are gathered on the CPU, and GPU writes into them stay out of their storage");
    return ok;
}
static void sp_store_wait(const tex_t *t) {
    uint64_t done = g_done, f = g_sps_lost > done ? g_sps_lost : 0, n = g_nsps < SPS_N ? g_nsps : SPS_N, live = 0;
    for (uint64_t i = 0; i < n; i++) live |= g_sps[i].fence > done;
    if (!live && !f) return;
    uint64_t va = (uint64_t)(uintptr_t)t->mem, np = sp_pages_n(va, t->L.size); const uint8_t **ph = malloc((size_t)np * sizeof *ph);
    if (!ph) MDIE("sparse page table of %llu pages", (unsigned long long)np);
    if (nvn_sparse_pages(t->pool, va & ~0xFFFull, np << 12, ph))
        for (uint64_t k = 0; k < np; k++) if (ph[k]) { uint64_t a = (uint64_t)(uintptr_t)ph[k];
            for (uint64_t i = 0; i < n; i++) if (g_sps[i].fence > done && g_sps[i].fence > f && a < g_sps[i].hi && g_sps[i].lo < a + 0x1000) f = g_sps[i].fence; }
    free(ph);
    if (f <= done) return;
    if (f > g_fence) { RED_ONCE(172, "a texture of a sparse pool gathered on the CPU while the command buffer being encoded stores a GPU write into that pool: the upload may lack it"); return; }
    if (on_sub_thread()) rec_publish();   // a commit may still be a record in the encode thread's block
    mtl_wait_at(f, UINT64_MAX, WS_UPALLOC);
}
static id<MTLComputePipelineState> sp_gather_pso(void) {
    static id<MTLComputePipelineState> ps; static int failed;
    if (!ps && !failed) {
        NSError *e = nil; id<MTLLibrary> lib = host_lib("deswz", &e); id<MTLFunction> fn = [lib newFunctionWithName:@"gather"];
        ps = fn ? dev_cps(fn, &e) : nil;
        if (!ps) { failed = 1; MLOG("[RED] cannot create the GPU gather of sparse textures: %s (they are gathered on the CPU)", e.localizedDescription.UTF8String); }
    }
    return ps;
}
// before the deswizzles that read it; the batch keeps the page table
static void sp_gather(const tex_t *t, const uint64_t *pt, uint64_t np, id<MTLBuffer> dst, uint64_t off) {
    id<MTLBuffer> tb = [g_dev newBufferWithBytes:pt length:(NSUInteger)np * sizeof *pt options:MTLResourceStorageModeShared];
    if (!tb) MDIE("sparse page table buffer of %llu pages", (unsigned long long)np);
    res_add(tb); [g_up_keep addObject:tb];
    size_t poff; id<MTLBuffer> params = up_param_alloc(&poff);
    uint64_t va = (uint64_t)(uintptr_t)t->mem, n = (t->L.size + 15) & ~15ull;
    struct { uint64_t va, pt_pg, n; } p = {va, va >> 12, n}; memcpy((uint8_t *)params.contents + poff, &p, sizeof p);
    [g_up_ce setArgumentTable:g_up_ct]; [g_up_ce setComputePipelineState:sp_gather_pso()];
    at_addr(g_up_ct, dst.gpuAddress + off, 0, "sparse gather table slot", 1); at_addr(g_up_ct, tb.gpuAddress, 1, "sparse gather table slot", 1); at_addr(g_up_ct, params.gpuAddress + poff, 2, "sparse gather table slot", 1);
    [g_up_ce dispatchThreadgroups:MTLSizeMake((NSUInteger)((n >> 4) + 63) / 64, 1, 1) threadsPerThreadgroup:MTLSizeMake(64, 1, 1)];
    [g_up_ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
}
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp); void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n); int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n);
// keep: usage the replaced texture was upgraded to, or every re-upload of a drawn-into texture would be upgraded again. prev: only images written since `since` are read
// from guest memory, the others copied on the GPU (*taken of them, 0: read whole).
static id<MTLTexture> pkg_upload(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse, MTLTextureUsage keep, id<MTLTexture> prev, const void *span, uint64_t since, unsigned *taken) {
    struct timespec a, b; clock_gettime(CLOCK_MONOTONIC, &a);
    if (taken) *taken = 0;
    id<MTLTexture> r = pkg_upload_(t, stamp, reuse, keep, prev, span, since, taken);
    clock_gettime(CLOCK_MONOTONIC, &b); uint64_t d_ = (uint64_t)((b.tv_sec - a.tv_sec) * 1000000000ll + b.tv_nsec - a.tv_nsec);
    atomic_fetch_add(&g_upload_ns, d_); g_up_total_ns += d_;
    return r;
}
uint64_t mtl_upload_ns_take(void) { return atomic_exchange(&g_upload_ns, 0); }
uint64_t mtl_upload_gpu_ns_take(void) { return atomic_exchange(&g_upload_gpu_ns, 0); }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = g_bp_lib_n; *pso = g_bp_pso_n; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = g_bp_lib_ns; *pso_ns = g_bp_pso_ns; }
uint64_t mtl_upload_bytes(void) { return g_bp_up_bytes; }
// DM_NVN_ZBLOCK: an all-zero ASTC block decodes as the error colour (magenta); logs textures whose bound levels hold any.
static int zblock_on(void) {
    static int v = -1;
    if (v < 0) {
        v = getenv("DM_NVN_ZBLOCK") != NULL;
        if (v) MLOG("DM_NVN_ZBLOCK: counting all-zero blocks of ASTC textures at upload (diagnostic)");
    }
    return v;
}
static uint64_t g_zb_ser; static uint32_t g_zb[16], g_zb_tot[16]; static int g_zb_n;
static void zb_report(const tex_t *t, unsigned base, const char *why) {
    if (!zblock_on() || g_zb_ser != t->serial) return;
    int bad = 0; for (int l = (int)base; l < g_zb_n; l++) bad |= g_zb[l] != 0;
    g_zb_ser = 0; if (!bad) return;
    static int nl; if (nl++ >= 400) return;
    char b[512]; size_t o = 0; int sp = t->pool && nvn_sparse_pool(t->pool);
    for (int l = 0; l < g_zb_n && o < sizeof b - 48; l++) { uint64_t lo = nvn_tex_level_offset(&t->d, (unsigned)l), hi = l + 1 < (int)t->d.levels ? nvn_tex_level_offset(&t->d, (unsigned)l + 1) : t->L.size;
        o += (size_t)snprintf(b + o, sizeof b - o, " L%d %u/%u%s", l, g_zb[l], g_zb_tot[l], sp ? (nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + lo, hi - lo) ? "" : " unmapped") : ""); }
    MLOG("[zb] %s ser=%llu format %#x %ux%u levels %u flags %#x mem=%p size %#llx sparse %d bound from level %u frame %llu:%s", why, (unsigned long long)t->serial, t->d.format, t->d.width, t->d.height, t->d.levels,
         t->d.flags, t->mem, (unsigned long long)t->L.size, sp, base, (unsigned long long)atomic_load(&g_cur_frame), b);
    const char *dir = getenv("DM_NVN_ZBLOCK"); static int nd; static uint64_t seen[16];   // =<folder>: also the raw storage of the first such texture of each format and size
    uint64_t kind = (uint64_t)t->d.format << 48 | (uint64_t)t->d.width << 24 | t->d.height; int dup = 0; for (int i = 0; i < nd; i++) dup |= seen[i] == kind;
    if (dir && dir[0] == '/' && nd < 16 && !dup) { seen[nd] = kind; uint64_t n = sp ? MAX(t->L.size, 2u << 20) : t->L.size; uint8_t *m = malloc(n); char fp[1024]; FILE *f;
        if (m) { if (sp) nvn_sparse_copy(t->pool, m, (uint64_t)(uintptr_t)t->mem, n); else memcpy(m, t->mem, n);
            snprintf(fp, sizeof fp, "%s/zb_%llu.bin", dir, (unsigned long long)t->serial); if ((f = fopen(fp, "wb"))) { fwrite(m, 1, n, f); fclose(f); nd++; }
            if (sp) { for (uint64_t o = 0; o < n; o += 0x1000) m[o >> 12] = (uint8_t)nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + o, 0x1000);
                snprintf(fp, sizeof fp, "%s/zbm_%llu.bin", dir, (unsigned long long)t->serial); if ((f = fopen(fp, "wb"))) { fwrite(m, 1, n >> 12, f); fclose(f); } }
            free(m); }
        MLOG("[zb] ser=%llu: block height log2 %d (packaged layout %#x), stride %u, depth %u, target %d, samples %d", (unsigned long long)t->serial, (int)t->L.tileH, t->d.pkg_layout, t->d.stride, t->d.depth, t->d.target, t->d.samples); }
}
// Packed 16-bit formats Metal has only with red and blue exchanged (RGB5, RGB5A1, RGB565) or nibbles reversed (RGBA4): host textures hold Metal's order, guest memory
// the guest's, converted wherever bytes cross. Textures reading guest memory in place take the swap when sampled (tex_by_id).
static int rb_packed_shift(int f) {
    uint32_t pf = 0; uint8_t sw[4];
    if (!nvn_fmt_to_mtl(f, &pf, sw) || sw[0] == NVN_SWZ_R) return 0;
    return pf == MTLPixelFormatB5G6R5Unorm ? 11 : pf == MTLPixelFormatBGR5A1Unorm ? 10 : pf == MTLPixelFormatABGR4Unorm ? 4 : 0;
}
// shift 4 (RGBA4) reverses the four nibbles; the same exchange both ways
static void rb_swap_rows(uint8_t *p, size_t wb, size_t rows, size_t nz, size_t row, size_t img, int shift) {
    uint16_t m = (uint16_t)(31u | 31u << shift);
    for (size_t z = 0; z < nz; z++) for (size_t y = 0; y < rows; y++) {
        uint16_t *q = (uint16_t *)(p + z * img + y * row);
        if (shift == 4) for (size_t i = 0; i < wb / 2; i++) q[i] = (uint16_t)(q[i] << 12 | (q[i] & 0xf0u) << 4 | (q[i] >> 4 & 0xf0u) | q[i] >> 12);
        else for (size_t i = 0; i < wb / 2; i++) q[i] = (uint16_t)((q[i] & ~m) | (q[i] & 31u) << shift | (q[i] >> shift & 31u));
    }
}
// A one-time read must not leave a large pool's memory wired. Made over the pages the upload just protected, not through pool_buf, which would make them writable again.
static uint64_t g_up_src_made;
static id<MTLBuffer> up_big_src(uint64_t addr, uint64_t len, uint64_t *off, int may_split) {
    // not once the raw memory table holds the whole pool: the lookup finds that buffer
    pthread_mutex_lock(&g_mu); int k = big_of(addr), in = k >= 0 && !g_big[k].full;
    if (k < 0 && len) { int i = pool_find(addr, len); in = i >= 0 && !g_pools[i].res && !addr_meets(g_pools[i].lo, g_pools[i].hi); }
    pthread_mutex_unlock(&g_mu);
    if (!in || !len) return nil;
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)addr & ~(pg - 1), b = ((uintptr_t)addr + len + pg - 1) & ~(pg - 1);
    // The staging budget does not count guest pages: the batch ends only between textures.
    size_t budget = up_src_budget();
    if (may_split && budget && g_up_src_bytes && b - a > budget - MIN(g_up_src_bytes, budget)) { up_flush(); up_begin(); }
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    if (!buf) return nil;
    g_up_src_made++;
    if (budget) g_up_src_bytes += b - a;
    res_add(buf); [g_up_keep addObject:buf]; *off = (uint64_t)addr - a;
    return buf;
}
// Only the images with a page written since the stamp are read (each layer's level has a byte range of its own). A byte each (level * layers + layer); NULL: read whole.
static int g_partial_upload_on = -1;
static uint8_t *pkg_written(const tex_t *t, const void *span, uint64_t since, unsigned levels, unsigned layers, unsigned *nup) {
    if (g_partial_upload_on < 0) { const char *e = getenv("DM_NVN_PARTIAL_UPLOAD"); g_partial_upload_on = !e || atoi(e); }
    if (!g_partial_upload_on) return NULL;
    nvn_texlayout L; nvn_tex_layout(&t->d, &L);
    if (!span || !levels || levels > 16 || levels > t->d.levels || !L.layer_size || L.size != t->L.size || L.layer_size != t->L.layer_size || (uint64_t)layers * L.layer_size != L.size) return NULL;
    uint8_t *need = calloc((size_t)levels * layers, 1); if (!need) return NULL;
    uint64_t lo[17]; for (unsigned l = 0; l <= levels; l++) lo[l] = l < t->d.levels ? nvn_tex_level_offset(&t->d, l) : L.layer_size;
    *nup = 0;
    for (unsigned y = 0; y < layers; y++) {
        const uint8_t *m = (const uint8_t *)t->mem + (uint64_t)y * L.layer_size;
        if (!nvn_dirty_since_span(span, m, L.layer_size, since)) continue;
        for (unsigned l = 0; l < levels; l++) if (lo[l + 1] <= lo[l] || nvn_dirty_since_span(span, m + lo[l], lo[l + 1] - lo[l], since)) { need[l * layers + y] = 1; ++*nup; }
    }
    return need;
}
// GPU uploads express this dependency through queue order instead
static int tex_cpu_wait(const tex_t *t) {
    uint64_t lo = (uint64_t)(uintptr_t)t->mem, hi = lo + t->L.size;
    // sparse mappings can alias another guest address, so a CPU upload cannot prove disjointness there
    int pending = drw_valid() ? drw_hits(lo, t->L.size) || (nvn_sparse_gen() && g_ndrw) :
        pend_hits(lo, t->L.size) || g_sbw_over || sbw_hits(lo, hi) || gw_live(lo, hi) || (nvn_sparse_gen() && (g_npend || g_nsbw || g_nsbw_w || g_fence > g_done));
    if (g_cb && pending) {
        if (!gpu_read_wait(WS_UPALLOC)) MDIE("CPU texture upload: the GPU did not finish its storage writes within 10 s");
        return 1;
    }
    if (!g_cb && (gw_live(lo, hi) || nvn_sparse_gen())) { mtl_finish_gpu_at(WS_UPALLOC); return 1; }
    return 0;
}
static id<MTLTexture> pkg_upload_(const tex_t *t, uint64_t *stamp, id<MTLTexture> reuse, MTLTextureUsage keep, id<MTLTexture> prev, const void *span, uint64_t since, unsigned *taken) {
    MTLTextureDescriptor *d = tex_desc(t, 1);
    if (!d) return nil;
    d.usage |= keep & (MTLTextureUsageRenderTarget | MTLTextureUsagePixelFormatView);
    int rb_shift = rb_packed_shift(t->d.format);
    int linear = (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;   // linear storage no view could take: its rows are copied on the CPU
    int direct = !linear && !rb_shift && stamp && direct_upload_on() && g_nvn_deswz_gpu && !(t->pool && nvn_sparse_pool(t->pool)) && nvn_deswz_gpu_direct_supported(&t->d);
    if (direct) d.usage |= MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    id<MTLTexture> mt = reuse && reuse.pixelFormat == d.pixelFormat && reuse.width == d.width && reuse.height == d.height && reuse.mipmapLevelCount == d.mipmapLevelCount && reuse.textureType == d.textureType
                        && (reuse.usage & d.usage) == d.usage
                        ? reuse : [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(2, "failed to create MTLTexture %ux%u format %#x target %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
    if (mt.allocatedSize >= (256u << 20)) { static int nb; if (nb++ < 20)   // a GPU out-of-memory error says nothing of which textures took the memory
        MLOG("large texture ser=%llu %ux%ux%u format %#x target %d levels %u: %llu MB%s", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels,
             (unsigned long long)(mt.allocatedSize >> 20), t->pool && nvn_sparse_pool(t->pool) ? ", in a sparse virtual pool" : t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) ? ", in a virtual pool" : ""); }
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("new texture (%s) #%d ser=%llu %ux%ux%u format %#x target %d levels %u storage %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    res_add(mt);   // Metal 4 does not make resources resident automatically: a missed registration is a GPU page fault
    int sparse = t->pool && nvn_sparse_pool(t->pool);
    if (stamp) {   // no stamp: a one-time upload with no write tracking
        *stamp = nvn_dirty_now();
        if (sparse) nvn_sparse_protect(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size); else nvn_dirty_protect(t->mem, t->L.size); }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned layers = (t->d.target == 4 || t->d.target == 9) ? t->d.depth : t->d.target == 8 ? 6 : t->d.target == 3 ? t->d.height : 1;   // a cube array's depth counts layer-faces
    // taken after the stamp and the protect: a write from here on faults and is found at the next look
    uint8_t *need = NULL; unsigned nimg = (unsigned)d.mipmapLevelCount * layers, nup = 0;
    if (prev && prev != mt && stamp && !sparse && !linear && bpb && d.sampleCount <= 1 && prev.pixelFormat == mt.pixelFormat && prev.textureType == mt.textureType && prev.sampleCount == mt.sampleCount
        && prev.width == mt.width && prev.height == mt.height && prev.depth == mt.depth && prev.arrayLength == mt.arrayLength && prev.mipmapLevelCount == mt.mipmapLevelCount)
        need = pkg_written(t, span, since, (unsigned)d.mipmapLevelCount, layers, &nup);
    if (need && nup == nimg) { free(need); need = NULL; }
    if (need && taken) *taken = nimg - nup;
    size_t total = 0;
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w, h, z; tex_lsize(t, l, &w, &h, &z);
        unsigned n = layers; if (need) { n = 0; for (unsigned y = 0; y < layers; y++) n += need[l * layers + y]; }
        total += (size_t)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * z * n;
    }
    g_bp_up_bytes += total;
    void *mb; uint64_t mo;
    id<MTLBuffer> upsrc = nil; unsigned src_batch = 0;
    if (!linear && !rb_shift && !sparse && g_nvn_deswz_gpu) { up_begin(); upsrc = up_big_src((uint64_t)t->mem, t->L.size, &mo, 1); src_batch = g_up_seq; if (upsrc) mb = (__bridge void *)upsrc; }
    // gathered on the CPU for DM_NVN_ZBLOCK, which counts its blocks there
    uint64_t *spt = NULL, spn = 0;
    t_tex_upload_lookup = 1;
    if (sparse && !linear && !rb_shift && g_nvn_deswz_gpu && sp_gather_pso() && !((uintptr_t)t->mem & 15) && !(zblock_on() && t->d.format >= 0x79 && t->d.format <= 0x94)) {
        spn = sp_pages_n((uint64_t)(uintptr_t)t->mem, t->L.size);
        if (!(spt = malloc((size_t)spn * sizeof *spt))) MDIE("sparse page table of %llu pages", (unsigned long long)spn);
        up_begin(); src_batch = g_up_seq;
        if (!sp_pages_gpu(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size, spt, 0)) { free(spt); spt = NULL; }
    }
    int use_gpu = !linear && !rb_shift && g_nvn_deswz_gpu && (sparse ? spt != NULL : upsrc || mtl_addr((uint64_t)t->mem, t->L.size, &mb, &mo));
    t_tex_upload_lookup = 0;
    if (g_nvn_deswz_gpu && !use_gpu && !sparse && !rb_shift && !linear) RED_ONCE(40, "swizzled texture storage is not in a memory pool; falling back to the CPU");
    if (use_gpu) {
        // The GPU reads the memory pool at execution time; the dirty-page stamp is still taken before encoding.
        up_begin();
        if (direct && !need && !(mo & 7)) {
            id<MTLTexture> view = [mt newTextureViewWithPixelFormat:MTLPixelFormatRGBA16Uint textureType:MTLTextureType2DArray levels:NSMakeRange(0,mt.mipmapLevelCount) slices:NSMakeRange(0,mt.arrayLength)];
            if (view) {
                if (!g_up_views) g_up_views=[NSMutableArray array];[g_up_views addObject:view];
                int ok=1;
                for(unsigned l=0;l<d.mipmapLevelCount;l++) {
                    size_t poff;id<MTLBuffer> params=up_param_alloc(&poff);
                    if(nvn_deswz_gpu_direct_m4(g_up_ce,g_up_ct,params,poff,(__bridge id<MTLBuffer>)mb,mo,&t->d,l,view)){ok=0;break;}
                    g_direct_dispatches++;
                }
                if(ok){[g_up_texs addObject:mt];g_direct_uploads++;if(!g_cb)up_flush();return mt;}
                // encoded writes finish before fallback copies replace them
                [g_up_ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            }
        }
        // one allocation, as a second could flush the batch that holds the first
        size_t stg_off, sto = ((total ? total : 16) + 255) & ~(size_t)255, gsz = spt ? (t->L.size + 255) & ~(size_t)255 : 0;
        id<MTLBuffer> stg = up_alloc(sto + gsz, &stg_off);
        if (spt) {   // up_alloc may have flushed the batch that holds the buffers the page table names
            if (g_up_seq != src_batch) sp_pages_gpu(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size, spt, 0);
            mb = (__bridge void *)stg; mo = stg_off + sto; sp_gather(t, spt, spn, stg, mo); free(spt); spt = NULL;
        }
        // only when up_alloc flushed the batch that held the first source buffer
        if (upsrc && g_up_seq != src_batch) {
            upsrc = up_big_src((uint64_t)t->mem, t->L.size, &mo, 0);
            mb = (__bridge void *)upsrc;   // if the new buffer was refused, fall back to the CPU instead of using the old batch's address
        }
        size_t off = 0; int enc_ok = g_up_ce != nil && mb != NULL, nb0 = g_nup_blits;
        for (unsigned l = 0; enc_ok && l < d.mipmapLevelCount; l++) {
            uint32_t w, h, z; tex_lsize(t, l, &w, &h, &z);
            size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
            for (unsigned y = 0; y < layers; y++) {
                if (need && !need[l * layers + y]) { up_copy_add(prev, MTLSizeMake(w, h, z), mt, y, l); continue; }
                size_t poff; id<MTLBuffer> params = up_param_alloc(&poff);   // Metal 4 has no setBytes
                if (nvn_deswz_gpu_encode_m4(g_up_ce, g_up_ct, params, poff, (__bridge id<MTLBuffer>)mb, mo, &t->d, l, y, stg, stg_off + off, pitch)) { enc_ok = 0; break; }
                up_blit_add(stg, stg_off + off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
                off += pitch * rows * z;
            }
        }
        if (enc_ok) {
            [g_up_texs addObject:mt];
            if (!g_cb) up_flush();
            free(need);
            return mt;
        }
        g_nup_blits = nb0;   // the encoded deswizzle only writes the staging area and is harmless; this texture takes the CPU path below
        if (!g_cb) up_flush();
        if (mb) RED_ONCE(41, "GPU deswizzle does not support format %#x target %d; falling back to the CPU", t->d.format, t->d.target);
        else RED_ONCE(19, "could not retain a texture source in the new upload batch; falling back to the CPU");
    }

    // unmapped pages as 0: reading a mip tail's host alias at a 4 KB offset stopped the host with SIGBUS
    if (tex_cpu_wait(t) && stamp) {
        // CPU writes could have unprotected pages while we waited: a new stamp and protect before snapshotting
        *stamp = nvn_dirty_now();
        if (sparse) nvn_sparse_protect(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size); else nvn_dirty_protect(t->mem, t->L.size);
    }
    const uint8_t *src = t->mem; uint8_t *part = NULL;
    free(spt);
    if (sparse) {
        if (!(part = malloc(t->L.size ? t->L.size : 1))) MDIE("sparse texture copy of %llu bytes failed", (unsigned long long)t->L.size);
        sp_store_wait(t); nvn_sparse_copy(t->pool, part, (uint64_t)(uintptr_t)t->mem, t->L.size); src = part;
    }
    up_begin();
    id<MTLBuffer> stg = [g_dev newBufferWithLength:total ? total : 16 options:MTLResourceStorageModeShared];
    res_add(stg); [g_up_keep addObject:stg];
    size_t off = 0;
    int zb = zblock_on() && t->d.format >= 0x79 && t->d.format <= 0x94; if (zb) { g_zb_ser = t->serial; g_zb_n = (int)MIN(d.mipmapLevelCount, 16u); memset(g_zb, 0, sizeof g_zb); memset(g_zb_tot, 0, sizeof g_zb_tot); }
    for (unsigned l = 0; l < d.mipmapLevelCount; l++) {
        uint32_t w, h, z; tex_lsize(t, l, &w, &h, &z);
        size_t pitch = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh;
        for (unsigned y = 0; y < layers; y++) {
            if (need && !need[l * layers + y]) { up_copy_add(prev, MTLSizeMake(w, h, z), mt, y, l); continue; }
            if (linear) {   // one 2D level: rows of the storage's stride, the pitch GetStorageSize counts
                size_t srow = tex_linear_row(t);
                if (l || y || z != 1 || srow < pitch || srow * rows > t->L.size) { RED_ONCE(153, "linear texture upload: %ux%u format %#x target %d levels %u is not one 2D level of rows (stride %zu)", t->d.width, t->d.height, t->d.format, t->d.target, t->d.levels, srow); break; }
                for (size_t r = 0; r < rows; r++) memcpy((uint8_t *)stg.contents + off + r * pitch, src + r * srow, pitch);
            } else if (nvn_deswizzle(&t->d, l, y, src, (uint8_t *)stg.contents + off, pitch)) { RED_ONCE(9, "deswizzle failed format %#x target %d", t->d.format, t->d.target); break; }
            if (rb_shift) rb_swap_rows((uint8_t *)stg.contents + off, pitch, rows, z, pitch, pitch * rows, rb_shift);
            if (zb && l < 16) { const uint64_t *q = (const uint64_t *)((const uint8_t *)stg.contents + off); size_t nb = pitch * rows * z / 16;
                for (size_t i = 0; i < nb; i++) g_zb[l] += !(q[2 * i] | q[2 * i + 1]);
                g_zb_tot[l] += (uint32_t)nb; }
            up_blit_add(stg, off, pitch, pitch * rows, MTLSizeMake(w, h, z), mt, y, l);
            off += pitch * rows * z;
        }
    }
    free(part); free(need);
    up_flush();
    return mt;
}
