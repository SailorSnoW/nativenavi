// The render replay's textures: lookups, sRGB pairs, image usage and atomics, packaged textures and their uploads (upload batches, deswizzles, gathers from sparse pools).
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ================= Render replay =================
#include "../../host/front.h"

// Texture lookups run on the submit thread and other game threads. Cache conformance for the few Metal texture and wrapper classes they see.
// Only classes and answers are kept here. The caller's reference to the object must be valid for the check.
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

static int g_red_once[175];   // one past the highest key: a new key raises this, and the macro refuses to compile otherwise
#define RED_ONCE(k, ...) do { _Static_assert((k) < sizeof g_red_once / sizeof *g_red_once, "a RED_ONCE key past g_red_once: raise its size"); if (!g_red_once[k]++) MLOG("[RED] " __VA_ARGS__); } while (0)
// NVN vertex streams 0–15 → Metal vertex buffer slots: 0–13 go to 16–29 (30 is the constant fallback buffer), 14 and 15 to 14 and 15. Metal has only the 31 slots 0–30; translated shaders use only buffer(0) (the argument buffer),
// and the host's own vertex functions read no vertex buffers. 13 Sentinels uses stream 15, and the old 16 + stream went out of range (Metal assertion: buffer index (31) must be < 31)
#define VB_SLOT(st) ((st) < 14 ? 16 + (st) : (st))

// ---------- Texture → MTLTexture ----------
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
// The linear format of an sRGB one (the other half of its g_pfv_srgb pair), or MTLPixelFormatInvalid for a format that is not sRGB
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
// Cache tables on the replay hot path always use nvn_hmap (C hash, keys not boxed); the table stores CFBridgingRetain'ed objects, CFRelease'd on replace / delete
static void hm_rel(void *v) { CFRelease(v); }   // hm_get_id, hm_put_id and hm_del_id are nvn_owner.h's
// The serial tables' change generation (g_ser_gen) and its notes are nvn_owner.m's: own_note, own_epoch, own_notes_take, own_note_at
// The sharing table (textures shared by storage: pool + offset + description) is nvn_owner.m's: own_alias_take, own_alias_publish, own_view_put, own_upgrade,
// own_alias_drop_pool_locked (nvn_owner.h), and so are retirement's waiting objects (own_retire_locked). What stays here under g_smu: the shared packaged textures
// (g_pkg_share, weak, partitioned by pool) and the image descriptions (g_image_desc)
static nvn_hmap g_pkg_share = {.klen = 8};   // pool address → malloc'ed nvn_hmap (alias_key → NvPkgRef)
// Live GPU-only image descriptions, including those not used yet. A dynamic-resolution family declares its largest image before any particular resolution is used.
// Keeping only descriptions here lets its first allocation cover that family exactly, without allocating every resolution or rounding to a power of two.
static nvn_hmap g_image_desc = {.klen = 8};   // serial → malloc'ed tex_t, under g_smu
// GPU-written compressible images and render targets, with or without the image flag: Ocarina of Time's dynamic-resolution depth buffer (COMPRESSIBLE alone, a VIRTUAL pool) made
// 15 live Depth32Float descriptions of 1,882-1,920 pixels at one offset, 14.5 MB each, until they shared a heap. Display, video, linear and sparse images keep their own path
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
// On memory pool Finalize, the pool's textures and the serial entries that name them go (own_pool_finalize_locked says why), its image descriptions go, then its texture IDs
static void treg_drop_pool_views(const void *pool);
static void cpu_tex_drop_pool(const void *pool);
static void gws_drop_pool(const pool_t *p);
// The serial tables, the sharing table, the change generation and retirement's eligibility are nvn_owner.m's (nvn_owner.h). What it asks of the replay layer:
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
// Host record of a packaged texture: the current MTLTexture, the upload stamp, and the global write generation at the last check
// spare: the texture the last re-upload replaced, kept resident and written by the next re-upload once the GPU has passed spare_fence (a texture the CPU rewrites every frame, a movie frame,
// alternates between two textures instead of allocating one per frame: Link's Awakening's title movie made and freed an 8 MB texture a frame)
// used_tu: g_tu_now at its last resolution outside a pool walk (tex_mtl_); mem / size: its storage (pkg_evict_idle). spare_t: when the spare became one (spare_trim)
@interface NvPkg : NSObject { @public id<MTLTexture> tex, spare, view; uint64_t stamp, seen, hash, spare_fence, spare_t, vgen, vseen, sub, own, lost_gen, size; uintptr_t mem; uint32_t used_tu; const void *span; int sparse, vbase, part; uint8_t *edge[2]; uint32_t edge_n[2], stored; id ps; id<MTLBuffer> eb; uint64_t *ph, up_frame, up_sub; uint32_t nph, missed; } @end   // hash: full hash of the uploaded content under DM_NVN_DIRTYCHECK; span: its tracked range (nvn_dirty_span, NULL = untracked);
// sparse: in a sparse virtual pool, read through its page table (hle_nvn.c nvn_sparse_*), vgen the page table's generation at the upload and vseen at the last look; view: what it is bound as (sparse_view)
// ps: its placement sparse texture (NvSparse, sparse_make), which tex is: mapped and uploaded tile by tile (sparse_sync)
// edge: the uploaded bytes of the storage's first and last host pages where other data shares them (pkg_edges_take); in eb from the first GPU store into its storage on (pkg_edges_gpu),
// where the store writes them too
// Its release retires its textures (defer_obj_release, not res_remove at once): serials that took its texture over (cpu_unc_drop) keep it resident (defer_drain)
// stored: GPU writes stored into its storage since its upload (gsb_swz; the DM_NVN_REUPLOADS line), which DM_NVN_DIRTYCHECK does not count as missed CPU writes
// sub, own: the submission of its last look at its pages and nvn_dirty_own_writes then (including sparse storage: sparse_checked)
// ph, nph, up_frame, up_sub, missed: under DM_NVN_DIRTYCHECK, the hash of its bytes in each host page, the frame and submission of the upload, and whether a miss was reported (pkg_hash_take)
// part: tex holds images copied from the texture it replaced (pkg_written), so through it from every upload back to the last whole one, when g_up_lost_gen was lost_gen
@implementation NvPkg
- (void)dealloc { defer_obj_release(tex); defer_obj_release(spare); if (eb) defer_obj_release(eb); else { free(edge[0]); free(edge[1]); } free(ph); }
@end
// Spares that waited for good. A spare pays only for a texture the CPU rewrites again soon (a movie frame); one re-upload of a streamed texture left its old texture resident until the
// texture object was finalized, held by nothing but its NvPkg (up to 168 spares, 123 MB, in Ocarina of Time). The packages that have one are listed here (weak), and at a
// submission's start a spare older than g_spare_idle_ns (3 s) is retired through defer_obj_release (after the submissions that used it complete, and only once no serial entry holds it).
// At memory pressure (mem_trim) every spare goes. Submit thread only, as are re-uploads
static uint64_t g_spare_idle_ns = 3000000000ull;
static NSHashTable *g_spare_pkgs; static uint64_t g_spare_last; static _Atomic uint64_t g_spare_trimmed, g_spare_trimmed_bytes, g_spare_held;   // held: spares left after the last walk (the report)
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
// Packaged textures by pool, storage and description. Engines make many texture objects over one texture's bytes, each with a serial of its own: Ocarina of Time's 32,768 registered IDs name 3,607 storages,
// 2.0 GB, and an upload per serial took 6 GB, past what a 16 GB Mac can keep wired once the pools are added. A new serial with the same storage and description takes the same NvPkg, so its
// dirty-page checks and re-uploads are one. Weak: the serials' entries own it, and a table entry never keeps one alive. Partitioned by pool so a pool's Finalize (mtl_alias_drop_pool) drops
// only that pool's table in O(1) for unrelated pools and frees its memory; dead entries are swept and capacity resized before a pool's table grows (pkg_share_compact), so permanent pools
// keep dead entries O(live) even when no pool is finalized. Under g_smu
@interface NvPkgRef : NSObject { @public __weak NvPkg *pkg; } @end
@implementation NvPkgRef @end
static void pkg_share_compact(nvn_hmap *pm) {   // caller holds g_smu: drop dead NvPkgRef entries and resize to fit the live ones
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
    NSMutableSet *pkgs = [NSMutableSet set];   // the pool's packaged textures still alive: finalized serials may hold them, and they end with the pool as its other textures do
    { uint64_t pkey = (uint64_t)(uintptr_t)pool;   // a later pool at this address holds other bytes (g_pkg_share)
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
    own_pool_finalize_locked(pool, (__bridge CFSetRef)pkgs);   // the pool's textures and every serial entry that names one (nvn_owner.m; a new epoch: every texture ID takes the slow path once)
    pthread_mutex_unlock(&g_smu);
    // The pool's texture IDs are marked gone before their CPU-written classification goes: a pool walk (pool_gpu, holding g_rmu) reaching an ID in between found it not gone, the
    // pool's magic still set and the texture no longer CPU-written, and tex_mtl_ handed out its packaged entry (an NvPkg) as the texture: "-[NvPkg buffer]: unrecognized selector"
    // in tex_by_id (Ocarina of Time, see reference/nvn-objects.md). Marked first, a reader holding g_rmu is done before, or finds the ID gone and takes tex_made
    treg_drop_pool_views(pool);   // outside g_smu: the texture ID registry lock g_rmu is taken before g_smu
    cpu_tex_drop_pool(pool);
    gws_drop_pool(pool);
}
// A packaged texture's entries hold the package, and once the GPU has written over its storage, its texture itself (cpu_unc_drop): both are the package's
void own_hook_parts(id o, void (^add)(id part)) { if (![o isKindOfClass:[NvPkg class]]) return; NvPkg *pk = o; add(pk->tex); add(pk->view); add(pk->spare); }
static alias_key tex_alias_key(const tex_t *t) {
    alias_key k; memset(&k, 0, sizeof k); k.pool = (uint64_t)(uintptr_t)t->pool; k.off = t->off; k.d = t->d;
    // IMAGE permits shader writes; it does not change storage. Xenoblade Chronicles 3's depth and stencil-sampling objects differ only by this flag. Separate Metal textures
    // made the stencil read zero and rejected every deferred-lighting pass. Actual image writes add Metal usage before opening their encoder (image_prepare).
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
// A sparse texture's CPU-write check walks its page table (nvn_sparse_since). A submission sees the CPU writes and the mappings made before it (as NVN's: a map is a CPU call,
// not a command), so the replay checks each one once a submission (sub: the submission, g_sub_serial, of its last check). Games remap streamed pages all the time, so the
// mapping generation alone rarely let the check be skipped: Mario Kart World's terrain targets walked theirs at nearly every lookup
static inline int sparse_checked(const NvPkg *pk) { return pk->sub == g_sub_serial && pk->own == nvn_dirty_own_writes(); }
static uint64_t g_pkg_uploads, g_pkg_reuploads, g_pkg_miss, g_pkg_edge_skips;
static uint64_t g_pfv_upgrades;
// DM_NVN_DIRTYCHECK=1 (diagnostic, compared against ground truth): fully hash the pool content on every use; changed but not detected by dirty-page tracking = a missed detection
static int dirtycheck(void) { static int v = -1; if (v < 0) v = getenv("DM_NVN_DIRTYCHECK") != NULL; return v; }
static uint64_t full_hash(const uint8_t *m, uint64_t n) { uint64_t h = 1469598103934665603ull; for (uint64_t i = 0; i < n; i++) h = (h ^ m[i]) * 1099511628211ull; return h; }
// Under DM_NVN_DIRTYCHECK, at an upload: the hash of the whole content, and of the texture's bytes in each host page, so a miss can name the pages that changed (pkg_miss_report)
static void pkg_hash_take(NvPkg *pk, const tex_t *t) {
    if (!dirtycheck() || pk->sparse) return;
    pk->hash = full_hash(t->mem, t->L.size);
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)t->mem, e = a + t->L.size, p0 = a & ~(pg - 1);
    uint32_t n = (uint32_t)((e - p0 + pg - 1) / pg);
    if (n != pk->nph) { free(pk->ph); pk->ph = malloc(n * sizeof *pk->ph); pk->nph = pk->ph ? n : 0; }
    for (uint32_t i = 0; i < pk->nph; i++) { uintptr_t lo = MAX(a, p0 + i * pg), hi = MIN(e, p0 + (i + 1) * pg); pk->ph[i] = full_hash((const uint8_t *)lo, hi - lo); }
    pk->up_frame = atomic_load(&g_cur_frame); pk->up_sub = g_sub_serial; pk->missed = 0;
}
// A texture whose content changed with no write the tracker saw (DM_NVN_DIRTYCHECK): the first 16, once each. Which of its host pages changed, where they lie in it, and for the first
// few what the tracker and the kernel hold for that page (nvn_dirty_describe), which tells a write that never faulted from a page whose protection was lost
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
// Writes beside a texture. The tracker marks whole host pages (16 KB) dirty, and guest memory pools are only 4 KB aligned: the first and last pages of a texture's storage are shared with
// whatever lies beside it. Pokémon Sword keeps each texture of its GFPAK models in a pool of its own, with the pool's object 4 KB before the storage, in the same host page, and writes there
// every frame: 73,232 re-uploads in 70 s of play from a save, every one checked of unchanged content, 12 GB of upload staging in 8 minutes. So the bytes of the storage in those edge pages
// are kept at the upload (pkg_edges_take), and a texture whose only written pages are its edges, with those bytes unchanged, is protected again instead of re-uploaded (pkg_edges_clean).
// The edges are the whole storage when it lies within two pages (no page wholly inside)
static void pkg_edge_ranges(const tex_t *t, uintptr_t r[2][2], uintptr_t *ia, uintptr_t *ie) {
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)t->mem, e = a + t->L.size;
    *ia = (a + pg - 1) & ~(pg - 1); *ie = e & ~(pg - 1);
    if (*ie <= *ia) { r[0][0] = a; r[0][1] = e; r[1][0] = r[1][1] = 0; *ia = *ie = 0; return; }
    r[0][0] = a; r[0][1] = *ia; r[1][0] = *ie; r[1][1] = e;
}
static void pkg_edges_drop(NvPkg *pk) {
    for (int i = 0; i < 2; i++) { if (!pk->eb) free(pk->edge[i]); pk->edge[i] = NULL; pk->edge_n[i] = 0; }
}
static void pkg_edges_take(NvPkg *pk, const tex_t *t) {   // right after an upload, with its stamp in pk->stamp
    pkg_edges_drop(pk);
    if (pk->sparse || !pk->span || !t->mem) return;
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    for (int i = 0; i < 2; i++) if (r[i][1] > r[i][0]) { pk->edge_n[i] = (uint32_t)(r[i][1] - r[i][0]);
        pk->edge[i] = pk->eb ? (uint8_t *)pk->eb.contents + (i ? r[0][1] - r[0][0] : 0) : malloc(pk->edge_n[i]); memcpy(pk->edge[i], (const void *)r[i][0], pk->edge_n[i]); }
    // A write after the upload's stamp may have reached the copy but not the upload: then there is no copy to trust, and the next write re-uploads as before
    if (nvn_dirty_since_span(pk->span, t->mem, t->L.size, pk->stamp)) pkg_edges_drop(pk);
}
// A GPU store into the storage (gsb_swz) changes the texture's bytes in its edge pages too, which the host texture already holds, and no page fault tells the tracker. Against the bytes of
// the upload, the next write beside it then looked like a write into it and re-uploaded the texture: 17 small ones in two minutes of Minecraft Dungeons II, and a full-screen target
// beside a page the game writes each frame would be re-uploaded each frame. So from its first store on the edge bytes live in a buffer the GPU writes too (eb), and the swz kernel writes
// each byte it stores there as well, in the same order as into the storage. eb: [first edge][last edge]; *e1s: the last edge's offset in the storage, ~0 when none
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
static int pkg_inner_written(const NvPkg *pk, const tex_t *t) {   // a page wholly inside the storage was written since the upload
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    return ie > ia && nvn_dirty_since_span(pk->span, (const void *)ia, ie - ia, pk->stamp);
}
static int pkg_edges_clean(NvPkg *pk, const tex_t *t) {   // the covered pages were written since the upload: 1 = only edge pages, and the texture's bytes there are as uploaded
    if (!pk->edge_n[0] && !pk->edge_n[1]) return 0;
    uintptr_t r[2][2], ia, ie; pkg_edge_ranges(t, r, &ia, &ie);
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect(t->mem, t->L.size);   // take the stamp, protect, then read (as pkg_upload): a later write is seen at the next check
    if (ie > ia && nvn_dirty_since_span(pk->span, (const void *)ia, ie - ia, pk->stamp)) return 0;
    for (int i = 0; i < 2; i++) if (pk->edge_n[i] && memcmp(pk->edge[i], (const void *)r[i][0], pk->edge_n[i])) return 0;
    pk->stamp = st; g_pkg_edge_skips++;
    return 1;
}
// Usage an NVN image takes: shader writes, and for R32Uint / R32Sint the shader-atomic usage too, since Metal runs texture atomics only on such textures (dummy_img_atomic) and NVN does
// not say which images a shader will use atomically
static inline int pf_r32i(MTLPixelFormat pf) { return pf == MTLPixelFormatR32Uint || pf == MTLPixelFormatR32Sint; }
// 64-bit texture atomics (texture2d<ulong>::atomic_max / atomic_min, MSL 3.1; dmsh_atomic.wide_atomics): Metal runs them on an RG32Uint texture made with the shader-atomic usage,
// on Apple GPU family 9 (M3 / A17 Pro and later). PRAGMATA's visibility buffer (SURED.D.2D.MAX.U64, depth << 32 | ID) is an NVN RG32UI image. Older GPUs have no 64-bit texture
// atomic at all (the pipeline does not build there), so their RG32Uint textures keep the usage they had. After uncraftablexd's PRAGMATA bring-up (PR #1801)
static int dev_atomic64(void) { static int v = -1; if (v < 0 && g_dev) v = [g_dev supportsFamily:MTLGPUFamilyApple9] ? 1 : 0; return v > 0; }
static inline int pf_atomic(MTLPixelFormat pf) { return pf_r32i(pf) || (pf == MTLPixelFormatRG32Uint && dev_atomic64()); }
static MTLTextureUsage img_usage(const tex_t *t, MTLPixelFormat pf) {
    if (!(t->d.flags & NVN_TEXTURE_FLAGS_IMAGE)) return 0;
    return MTLTextureUsageShaderWrite | (pf_atomic(pf) ? MTLTextureUsageShaderAtomic : 0);
}
// Metal leaves MTLTextureUsageShaderAtomic out of a texture's usage property (0x3 for a texture made with 0x23), so it cannot be read back: every shader-writable R32Uint / R32Sint
// texture the host makes has it (img_usage, tex_upgrade, dummy_img_atomic), and that is what this tests
static int tex_atomic_capable(id<MTLTexture> base) { return pf_atomic(base.pixelFormat) && (base.usage & MTLTextureUsageShaderWrite); }   // RG32Uint too on Apple GPU family 9 (pf_atomic)
// Metal runs texture atomics only on R32Uint / R32Sint storage, while NVN takes them on any 32-bit format through an R32 image: Ocarina of Time's NDSR upscaler ORs into an RGBA8 target (d17eefaff4f0bc23).
// Such a texture moves to R32 storage, the same bytes, with its own format as a view for rendering and sampling (tex_upgrade). Not packaged textures: those are the guest's bytes, uploaded
static int tex_atomic_alias(id<MTLTexture> base) {
    MTLPixelFormat pf = base.pixelFormat;
    int bytes32 = pf == MTLPixelFormatRGBA8Unorm || pf == MTLPixelFormatRGBA8Unorm_sRGB || pf == MTLPixelFormatBGRA8Unorm || pf == MTLPixelFormatBGRA8Unorm_sRGB || pf == MTLPixelFormatRGBA8Snorm
        || pf == MTLPixelFormatRGBA8Uint || pf == MTLPixelFormatRGBA8Sint || pf == MTLPixelFormatRGB10A2Unorm || pf == MTLPixelFormatRGB10A2Uint || pf == MTLPixelFormatRG11B10Float || pf == MTLPixelFormatRGB9E5Float
        || pf == MTLPixelFormatRG16Unorm || pf == MTLPixelFormatRG16Snorm || pf == MTLPixelFormatRG16Uint || pf == MTLPixelFormatRG16Sint || pf == MTLPixelFormatRG16Float || pf == MTLPixelFormatR32Float;
    return bytes32 && (base.textureType == MTLTextureType2D || base.textureType == MTLTextureType2DArray) && base.sampleCount == 1 && base.storageMode == MTLStorageModePrivate && !base.buffer && !base.isSparse;
}
static MTLTextureDescriptor *tex_desc(const tex_t *t, int packaged) {
    uint32_t pf = 0; uint8_t swz[4];
    if (!nvn_fmt_to_mtl(t->d.format, &pf, swz) || !pf) { RED_ONCE(0, "texture format %#x has no Metal equivalent (first case)", t->d.format); return nil; }
    // Metal's limits: 16384 in 2D, 2048 in 3D, 2048 array layers; beyond them Metal aborts the process on an assertion, so they are caught here with a red line
    if (t->d.width > 16384 || t->d.height > 16384 || t->d.depth > 2048) { RED_ONCE(71, "texture size exceeds Metal limits: %u×%u×%u levels %u format %#x target %d (first case ser=%llu)", t->d.width, t->d.height, t->d.depth, t->d.levels, t->d.format, t->d.target, (unsigned long long)t->serial); return nil; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.pixelFormat = (MTLPixelFormat)pf;
    d.width = t->d.width ? t->d.width : 1; d.height = t->d.height ? t->d.height : 1; d.depth = 1; d.arrayLength = 1;
    d.mipmapLevelCount = t->d.levels ? t->d.levels : 1;
    switch (t->d.target) {
        // 1D / 1D array are created as one-row 2D / 2D array textures: translated shaders never declare texture1d (0 in all five games' libraries) and always sample them as 2D; a Metal 1D texture bound to a texture2d argument is a type mismatch with undefined reads.
        // Super Mario 3D World + Bowser's Fury's material shaders sample 64x1 R32F 1D lookup tables heavily every frame (on Maxwell a 2D sample of a 1D texture reads that one row)
        case 0: d.textureType = MTLTextureType2D; d.height = 1; break;
        case 2: d.textureType = MTLTextureType3D; d.depth = t->d.depth; break;
        case 3: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.height; d.height = 1; break;
        case 4: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 5: d.textureType = MTLTextureType2DMultisample; d.sampleCount = t->d.samples; d.mipmapLevelCount = 1; break;
        case 6: d.textureType = MTLTextureType2DMultisampleArray; d.sampleCount = t->d.samples; d.arrayLength = t->d.depth; d.mipmapLevelCount = 1; break;
        // Cube textures are created as 6-layer 2D arrays and sampled through cube / cube array views made by tex_fit: Metal only allows cube array views of a 2D array base texture (not of a cube base).
        // The Legend of Zelda: Breath of the Wild has 1512 shaders that sample the environment light probes as a cube array while a single cube texture is bound (texture ID 1064 onwards): with mismatched types the reads are undefined, and every lit surface came out black
        case 8: d.textureType = MTLTextureType2DArray; d.arrayLength = 6; break;
        // Cube arrays likewise, as a 2D array of all their faces: NVN gives a cube array's depth in layer-faces (a multiple of 6), where a Metal cube array's arrayLength counts cubes.
        // Created as a cube array with arrayLength = depth, DRAGON QUEST XI S's 510-face one (85 cubes, made when its prologue movie ends) asked for 510 cubes, past Metal's 341, and Metal aborted the process
        case 9: d.textureType = MTLTextureType2DArray; d.arrayLength = t->d.depth; break;
        case 10: RED_ONCE(1, "buffer textures (TARGET_BUFFER) not implemented"); return nil;
        default: d.textureType = MTLTextureType2D; break;
    }
    // Fields NVN takes that Metal refuses: Metal aborts the process on them (-[MTLTextureDescriptorInternal validateWithDevice:]). An array or 3D texture of no layers gets one; one of
    // more than 2048 is not made. More levels than the size holds (on NVN the extra ones are 1x1; a mod's 1x1 texture declaring 9, MacNativeGames f864eb0): made with the levels the
    // size holds, and copies to the others are skipped (tex_level_ok). A multisample texture of a sample count the GPU lacks (Apple GPUs have 2 and 4; NVN also 8 and 16), or of
    // one sample, has no Metal form and is not made, as a texture past Metal's size limits (fewer samples would leave shaders that read sample 4 or higher reading past the texture)
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
    d.storageMode = MTLStorageModePrivate;   // Render targets and assets both go in GPU private storage (lossless compression available)
    d.usage = MTLTextureUsageShaderRead | (fmt_compressed(t->d.format) || packaged ? 0 : MTLTextureUsageRenderTarget)
            | img_usage(t, d.pixelFormat);   // Image usage: shader-writable (compute kernels generating the 3D color lookup table, etc.
    // A depth-stencil texture has the format-view usage from the start: its stencil is read through an X32_Stencil8 view (stencil-mode views and textures, the depth-stencil copy), which Metal's
    // contract allows only with that usage. Apple GPUs read the view correctly without it, but the API validation layer aborts the process at the first such view (PR #106, found on Luigi's
    // Mansion 3), and upgrading later would recreate a depth buffer in use. Unlike colour formats it costs nothing measured: on an M4 Pro a 2560x1440 one has the same allocated size and the same
    // GPU time over eight load / store passes with and without it
    if (d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8) d.usage |= MTLTextureUsagePixelFormatView;
    // An RGB9E5 texture the GPU writes has the format-view usage from the start too: NVIDIA's GPUs have no RGB9E5 colour target, so a game draws into one through an R32_UI view
    // of it (rt_view_upgrade), packing the bits in its shader. Nintendo Switch Sports Resort's NDSR upscaler input is one, made again at each new size of its dynamic resolution: without
    // this every one of them was upgraded (copied into a new texture) at its first pass, 27 in 80 s of its menus
    if (d.pixelFormat == MTLPixelFormatRGB9E5Float && (d.usage & MTLTextureUsageRenderTarget)) d.usage |= MTLTextureUsagePixelFormatView;
    return d;
}
// A level's size as its Metal texture holds it (tex_desc): width, rows, and depth planes (z may be NULL). A 1D (target 0) or 1D array (3) texture is one row; a 1D array's NVN
// height is its layer count. Taken as rows, every layer of a 1D array went up as many rows tall as the array has layers: PRAGMATA's 512x64 R16F light-profile LUT went up as 64
// copies of 64 rows into one-row slices, which Metal's validation refuses and which without it wrote past each slice into the next layers
static inline void tex_lsize(const tex_t *t, unsigned l, uint32_t *w, uint32_t *h, uint32_t *z) {
    uint32_t H = t->d.target == 0 || t->d.target == 3 ? 1u : t->d.height, Z = t->d.target == 2 ? t->d.depth : 1u;
    *w = l < 32 ? MAX(t->d.width >> l, 1u) : 1u; *h = l < 32 ? MAX(H >> l, 1u) : 1u;
    if (z) *z = l < 32 ? MAX(Z >> l, 1u) : 1u;
}
// In-place upgrade: when the original texture was not created with PixelFormatView (off by default, to keep Apple GPU lossless compression) but a different-format view is needed, create a new one with the original spec + PixelFormatView, copy over every slice × level, and replace the registration.
// Count: across the 4 screens character select / camp / wilderness / combat, 17473 textures needed a different-format view 0 times, so this is a fallback and does not rely on predicting which textures need it
// The same upgrade adds render-target usage for a whole-level clear (clear_tex) of a texture made without it: Monster Hunter Rise clears a 32x80 packaged one
static id<MTLTexture> tex_upgrade(const tex_t *t, id<MTLTexture> asked, MTLTextureUsage add, MTLPixelFormat view_pf) {
    id holder = tex_get(t->serial);
    // The serial holds an sRGB / linear view (tex_mtl_) of the asked texture, or the asked texture is that view: the parent is upgraded and the view made again on it, so the pair keeps sharing its bytes
    id<MTLTexture> view = ![holder isKindOfClass:[NvPkg class]] && obj_is_tex(holder) && ((id<MTLTexture>)holder).parentTexture
        && (holder == asked || ((id<MTLTexture>)holder).parentTexture == asked) ? holder : nil;
    id<MTLTexture> base = view ? view.parentTexture : asked;
    if (base.isSparse) { RED_ONCE(149, "a placement sparse texture needs usage %#lx it was not made with (ser=%llu format %#x): kept as it is, not remade whole", (unsigned long)add, (unsigned long long)t->serial, t->d.format); return base; }
    MTLTextureDescriptor *d = [MTLTextureDescriptor new];
    d.textureType = base.textureType; d.pixelFormat = base.pixelFormat; d.width = base.width; d.height = base.height; d.depth = base.depth;   // Keep the original format, only add the format-view usage
    d.arrayLength = base.arrayLength; d.mipmapLevelCount = base.mipmapLevelCount; d.sampleCount = base.sampleCount;
    d.storageMode = base.storageMode; d.cpuCacheMode = base.cpuCacheMode; d.hazardTrackingMode = base.hazardTrackingMode;
    d.resourceOptions = base.resourceOptions; d.usage = base.usage | add | (tex_atomic_capable(base) ? MTLTextureUsageShaderAtomic : 0);   // the usage property leaves the atomic usage out
    // A linear texture aliases the guest pool. Adding usage must keep that backing: a new standalone texture copies the old pixels once, but later GPU writes never reach the CPU map.
    id<MTLBuffer> backing = base.buffer;
    int reinterpret = (add & MTLTextureUsageShaderAtomic) && !pf_atomic(base.pixelFormat);   // an RG32Uint texture takes 64-bit atomics as it is (pf_atomic)
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
    id<MTL4ComputeCommandEncoder> b = backing ? nil : blit();   // shared backing already contains the pixels; copying it onto itself would overlap
    NSUInteger slices = base.textureType == MTLTextureType3D ? 1 : base.arrayLength * (base.textureType == MTLTextureTypeCube || base.textureType == MTLTextureTypeCubeArray ? 6 : 1);
    for (NSUInteger l = 0; !backing && l < base.mipmapLevelCount; l++) {
        NSUInteger w = MAX(base.width >> l, 1u), h = MAX(base.height >> l, 1u), z = base.textureType == MTLTextureType3D ? MAX(base.depth >> l, 1u) : 1;
        if (base.textureType == MTLTextureType3D)
            enc_b_copy_texture(b, base, 0, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, z), upgraded, 0, l, MTLOriginMake(0, 0, 0));
        else for (NSUInteger s = 0; s < slices; s++)
            enc_b_copy_texture(b, base, s, l, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), upgraded, s, l, MTLOriginMake(0, 0, 0));
    }
    if ([holder isKindOfClass:[NvPkg class]]) { id<MTLTexture> old = ((NvPkg *)holder)->tex; ((NvPkg *)holder)->tex = upgraded; defer_obj_release(old); own_epoch();
        if (((NvPkg *)holder)->view) { defer_obj_unref(((NvPkg *)holder)->view); ((NvPkg *)holder)->view = nil; ((NvPkg *)holder)->vbase = 0; } }   // a sparse texture's view of the old one: bound whole until its next upload
    else {
        // Every name of the old texture moves to the upgraded one: other serials sharing its storage (live in g_by_serial, finalized in g_ghost) and the sharing table. Only the asking serial moved before,
        // so the others kept reading the old copy while writes went to the new one, and the old texture stayed shared. Slots are replaced in place (no insert, so no table resize during the walk);
        // the old texture is retired once the submissions that used it complete, the view caches follow g_ser_gen (PR #107). An sRGB / linear twin's view is made again on the upgraded
        // texture first and published in the rename's hold (own_upgrade), so no reader finds the serial's old view once its parent has left the sharing table
        if (view) {
            upgraded_view = [upgraded newTextureViewWithPixelFormat:view.pixelFormat textureType:view.textureType levels:NSMakeRange(0, upgraded.mipmapLevelCount) slices:NSMakeRange(0, upgraded.arrayLength) swizzle:view.swizzle];
            static int nview; if (nview++ < 4) MLOG("sRGB / linear view ser=%llu made again on its upgraded parent (%d so far)", (unsigned long long)t->serial, nview);
            if (!upgraded_view) RED_ONCE(151, "cannot make the sRGB / linear view ser=%llu again on its upgraded parent: it keeps the old storage", (unsigned long long)t->serial);
        }
        own_upgrade(base, upgraded, t->serial, view ? upgraded_view : upgraded);   // the serial tables, the pins and the sharing table, a new epoch (nvn_owner.m)
        defer_obj_release(base);
    }
    MLOG("texture upgraded to allow %s ser=%llu base format %lu → view format %lu (upgrades=%llu)", add & MTLTextureUsageShaderWrite ? "image writes" : add & MTLTextureUsageRenderTarget ? "render-target use" : "format views", t->serial, (unsigned long)base.pixelFormat, (unsigned long)view_pf, ++g_pfv_upgrades);
    return upgraded_view ? upgraded_view : upgraded;
}
static id<MTLTexture> tex_upgrade_pfv(const tex_t *t, id<MTLTexture> base, MTLPixelFormat view_pf) { return tex_upgrade(t, base, MTLTextureUsagePixelFormatView, view_pf); }
// Packaged texture upload: first take the stamp, then set read-only, then read the pool memory and deswizzle into a staging buffer, and blit into a new private texture.
// The stamp comes first: a write that faults while the pages are being protected gets a generation above it and leaves its page writable, and its bytes may land after
// the read; a stamp taken after the protect covered that generation, so the write and every later one to that page went unseen and the texture stayed stale
// (MacNativeGames a166602, missing letters in Tears of the Kingdom's glyph cache). The same order holds wherever a stamp is taken with a protect.
// Batching: uploads within one replay submission share one upload command buffer (compute encoder deswizzle + a unified blit), flushed before the replay command buffer is committed;
// the same queue executes in commit order ⇒ uploads are always finished before the first sample. Uploads outside a submission are flushed on the spot. The staging area is cut from 32 MB private blocks, held by the command buffer until completion.
static _Atomic uint64_t g_upload_ns;   // Cumulative time of deswizzle + upload encoding on the submit thread (timing instrumentation)
static _Atomic uint64_t g_upload_gpu_ns;
static id<MTL4CommandBuffer> g_up_cb;
static id<MTL4ComputeCommandEncoder> g_up_ce;
static _Atomic int g_up_alloc_busy[3];
// Waiting for upload batches to finish (g_up_pending, g_up_alloc_busy): the batches' completion handler broadcasts after it updates them, so waiters sleep until then (they used to poll every 0.2–1 ms;
// Three Houses' loader calls QueueFinish after each resource, each one waited up to a millisecond late). The 10 ms timeout is only a safety net
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
// Each upload slot keeps its staging resident until its next batch (up_begin waits for the previous batch). Grow the first block to the slot's previous batch demand, not just its first texture:
// a small first texture followed by a large one otherwise allocates and retires a fresh 32 MB overflow every frame. Keep one overflow as well, at most 64 MB per slot / 192 MB across all three.
// Bursts beyond that budget and individual uploads larger than UP_BLOCK_SIZE still use temporary blocks, retired only after their GPU work. DM_NVN_UPLOAD_REUSE=0 keeps the original one-block policy.
#define UP_SLOT_BLOCK (4u * 1024u * 1024u)
static __strong id<MTLBuffer> g_up_slot_blk[3], g_up_slot_over[3]; static unsigned g_up_cur_slot;
static size_t g_up_slot_want[3], g_up_batch_bytes; static int g_up_over_used;
static size_t g_up_src_bytes;   // page-rounded bytes of the open batch's zero-copy sources
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
static int up_reuse(void) { static int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_UPLOAD_REUSE"); on = !e || atoi(e); } return on; }
static int up_retained(id<MTLBuffer> b, unsigned slot) { return b && (b == g_up_slot_blk[slot] || b == g_up_slot_over[slot]); }
static size_t up_slot_block_size(size_t demand) {
    size_t want = UP_SLOT_BLOCK;
    while (want < demand) want <<= 1;
    return want;
}

// Idle trimming is opt-in. An explicit 0 still reports the cache for A/B runs.
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
        // Only the upload batch uses these blocks. Its feedback handler clears busy after GPU completion.
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
    // Commit the removals before this submission can allocate more GPU memory.
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
    __unsafe_unretained id<MTLTexture> dst;   // Strongly referenced by g_up_texs until the flush is encoded
    NSUInteger slice, level;
    MTLOrigin at;   // where in the level (a sparse texture's tile, sparse_sync); 0 otherwise
    __unsafe_unretained id<MTLTexture> tsrc;   // the image is copied from this texture's same slice and level instead of from src (up_copy_add); kept by g_up_views
    uint8_t t2b, bar;   // t2b: the region `size` at `at` of tsrc's slice and level goes into src at off (row, image) instead (up_t2b_add); bar: behind a barrier after the copies before it
} up_blit;
static up_blit *g_up_blits; static int g_nup_blits, g_up_blitcap;
static NSMutableArray *g_up_texs, *g_up_views;   // Targets and temporary integer-write views kept through their upload
static id<MTLBuffer> g_up_block;
static size_t g_up_block_off, g_up_block_size;
static NSMutableArray *g_up_keep;
// Limit overflow staging per batch. The three upload slots bound in-flight
// batches; up_begin publishes queued commits before waiting for a slot. A single
// texture larger than the budget gets its own block (uploads are not tiled here).
static size_t g_up_staging_bytes;
#define UP_BATCH_BYTES (128u * 1024u * 1024u)


// The upload command buffer was discarded by the system (dropped along during GPU error recovery; the first frame on entering Travincal drops one every run): the objects of this batch of textures are built and the timestamps updated, but the content was never written in,
// and if the pool memory on the game side no longer changes it will never be re-uploaded. Record this batch of target textures, and the next time any of them is fetched treat it as dirty and re-upload once.
// DM_TEST_UP_LOST=<batch index> (for testing): treat that batch of uploads as dropped, to verify the re-upload path.
static _Atomic uint64_t g_up_lost_gen;   // batches dropped so far: a texture holding images copied from its predecessor (NvPkg part) cannot tell whether the drop reached them, and is uploaded whole
static NSHashTable *g_up_lost; static os_unfair_lock g_up_lost_lk = OS_UNFAIR_LOCK_INIT; static _Atomic int g_up_lost_n; static _Atomic uint64_t g_up_lost_redo;   // Weak-reference table: if a texture in the record is never fetched again and is released normally, this table does not keep it alive
static int up_lost_take(id tex) {   // This texture's upload was dropped: take it out of the record and return 1
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
        // The source may be the target of this encoder's earlier copies (a texture uploaded and replaced within one batch)
        if (x->tsrc != from) { from = x->tsrc; [b barrierAfterEncoderStages:MTLStageBlit beforeEncoderStages:MTLStageBlit visibilityOptions:MTL4VisibilityOptionDevice]; }
        enc_b_copy_texture(b, x->tsrc, x->slice, x->level, MTLOriginMake(0, 0, 0), x->size, x->dst, x->slice, x->level, MTLOriginMake(0, 0, 0));
    }
    if (b) { m4_end(b); g_up_ce = nil; }
    id<MTL4CommandBuffer> cb = g_up_cb;
    unsigned slot = (g_up_seq - 1) % 3;
    // Only this upload batch uses its overflow staging. Retire it on upload
    // completion, even when the main rendering submission is still being built.
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
            own_epoch();   // Make the view cache of every texture ID take the slow path once (on a cache hit tex_mtl is not passed through and this record is not seen)
            MLOG("%supload command buffer dropped: the %lu textures of this batch are marked unwritten and re-uploaded on next use (%d awaiting re-upload)", fake_lost ? "DM_TEST_UP_LOST: " : "", (unsigned long)dsts.count, n);
        }
        double d = f.GPUEndTime - f.GPUStartTime;
        if (d > 0) atomic_fetch_add(&g_upload_gpu_ns, (uint64_t)(d * 1e9));
        @autoreleasepool {
            for (id<MTLResource> obj in staging) res_remove(obj);
            [staging removeAllObjects];
            [views removeAllObjects];   // upload-only integer views share their targets' residency
            res_commit();
        }
        atomic_store(&g_up_alloc_busy[slot], 0);
        atomic_fetch_sub(&g_up_pending, 1);
        up_wake();
    }];
    res_commit();
    cb_end(cb, nil);
    rec_commit_upload(cb, o);
    // Outside a submission nothing else hands the encode thread its current block until the next submission, so a batch flushed on the spot stayed a record and every wait for it
    // (nvnQueueFinish on any thread, up_begin on another) waited for that submission: hand it over now (from PR #141, which found the nvnQueueFinish half)
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
    // The slot's last batch may still be only a commit record in the encode thread's current block (rec_commit_upload), which is handed over when it fills or the submission drains:
    // waiting without handing it over waited for a batch never committed. Little Nightmares II's first render passes zero-filled enough new targets to come round to a slot within one
    // block, and its RHI thread waited for good inside nvnQueueSubmitCommands (q_cb drains before it waits for the same reason)
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

// Diagnostic readout (low frequency; printed only when the block switch count changed and ≥ 1 second since the last line, at most 200 lines per game): how many times an upload batch ran out of staging block and switched blocks midway; GPU allocated, residency set object count, deferred-release queue length.
// If the old block is not retired on a switch it stays in the residency set and can never be freed, and "GPU allocated" climbs one block at a time with the switch count
static uint64_t g_up_swaps, g_up_swap_bytes; static NSHashTable *g_up_old;   // g_up_old: old blocks switched out (weak references); the diagnostic line counts how many are still alive
static void up_diag(void) { @autoreleasepool {
    // Weak-table enumeration autoreleases the buffers it finds. Do not keep
    // completed batches alive until the whole rendering submission ends.
    // Print a line when the block switch count changes; then one more line 5 seconds later (that batch's commit is long done and deferred retirement drained, so "GPU allocated" is the settled number)
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
    // This call starts one texture's upload: all prior deswizzles and copies are
    // complete records, so the batch can be split without ending the render pass.
    if (g_up_staging_bytes && (!g_up_block || need > g_up_block_size - g_up_block_off)
        && extra > UP_BATCH_BYTES - MIN(g_up_staging_bytes, UP_BATCH_BYTES)) {
        up_flush();
        up_begin();
    }
    g_up_batch_bytes += MIN(need, UP_BLOCK_SIZE - g_up_batch_bytes);   // demand saturates at one retained block
    if (!g_up_block && need <= UP_BLOCK_SIZE) {   // first allocation of the batch: the slot's own block, grown to the next power of two that holds it (a movie frame re-uploaded every frame, 8 MB in Link's Awakening)
        __strong id<MTLBuffer> *sb = &g_up_slot_blk[g_up_cur_slot];
        size_t demand = up_reuse() ? MAX(need, g_up_slot_want[g_up_cur_slot]) : need;
        size_t want = up_slot_block_size(demand);
        if (*sb && (*sb).length < want) { defer_obj_release(*sb); *sb = nil; }   // the slot's previous batch has finished (up_begin waited for it)
        if (!*sb) { *sb = [g_dev newBufferWithLength:want options:MTLResourceStorageModePrivate]; if (!*sb) MDIE("failed to create the swizzled-upload staging block %#zx", want); res_add(*sb); g_up_blocks_new++; }
        g_up_block = *sb; g_up_block_size = (*sb).length; g_up_block_off = 0;
        g_up_staging_bytes += (*sb).length;
    }
    if (!g_up_block || g_up_block_off + need > g_up_block_size) {
        // Block switch: the old block is still referenced by copy / decompress commands already recorded in this batch and cannot be freed on the spot; hand it to g_up_keep, to be retired later with the other staging after the flush commits (removed from the residency set on retire).
        // Previously g_up_block was simply replaced with the new block here: nobody retired the old block, yet it stayed in the residency set, so each extra block switch in an upload batch leaked one block (usually 32 MB) of GPU memory.
        // The old block and the "current block" retired at flush are two objects, so nothing is retired twice
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
// Memory pressure asked for a trim (mem_pressure_event): at a submission's start, on the submit thread, which owns the upload slots, before anything of the submission is encoded. What goes
// is what no frame needs and nothing waits for: the recycled render-target heaps waiting for a target (rh_trim_n), every packaged texture's spare (spare_trim), uploaded textures unused for
// 30 s (pkg_evict_idle), the staging blocks of upload slots with no batch open or in flight, and completed deferred releases. Each trim is logged with what it freed and the system's free memory after it
static uint64_t pkg_evict_idle(unsigned *n_out);
static void mem_trim(void) {
    int lv = atomic_exchange(&g_mp_want, 0);
    if (!lv) return;
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    unsigned rn = 0; uint64_t rb = rh_trim_n(1, &rn);
    unsigned sn = 0; uint64_t sb = spare_trim(1, &sn);   // every packaged texture's spare (NvPkg)
    unsigned en = 0; uint64_t eb = pkg_evict_idle(&en);   // uploaded textures nothing used for 30 s
    unsigned un = 0; uint64_t ub = 0;
    for (unsigned s = 0; s < 3; s++) {
        if ((g_up_cb && s == g_up_cur_slot) || atomic_load(&g_up_alloc_busy[s])) continue;   // a batch open in this slot or still on the GPU
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

// Deswizzle parameter blocks: 64 bytes per cell, cut sequentially from a 64 KB shared-storage block; a full block is replaced by a new one, and the old block is released lazily by fence
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
// A region of a texture into a buffer in the upload batch, behind a barrier (it may read what the batch's earlier copies wrote); the buffer→texture copy that reads it next goes
// behind one too (up_blit_add_at, then .bar). Compressed alias pairs fill a side from the other with these (bcp_up_blocks)
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
int nvn_host_readable(uint64_t a, uint64_t n);   // hle_nvn.c: guest memory and sparse pools
int nvn_sparse_pool(const void *pool); uint64_t nvn_sparse_gen(void); uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n); int nvn_sparse_notes(uint64_t *seen, uint64_t (*out)[2], int max);
int nvn_sparse_pages(const void *pool, uint64_t a, uint64_t n, const uint8_t **out);
// A texture of a sparse pool uploaded whole (pkg_upload_) is its storage as the pool's page table maps it: the physical memory behind each 4 KB page, 0 where nothing is mapped. The GPU
// reads and writes it there through a table of each page's GPU address, so that both happen in the queue's order. A copy into such a texture, a clear or a render pass writes that
// memory (gsb_swz), as the hardware's does, and the upload batch gathers it after every command buffer committed before it (the gather kernel). Gathered on the CPU when the upload
// was encoded, a later upload read the memory before an earlier command buffer's write reached it, and none was stored there at all: Densha de GO!! Hashiro Yamanote Line fills the
// finer levels of 117 sparse 512x512 ASTC 6x6 textures with buffer-to-texture copies, and each upload after a mapping change put back the zeros of its storage (magenta, GitHub #2350).
// The table of the pages over [va, va + n), from page va >> 12 on, into pt (sp_pages_n entries): → 1, or 0 when a mapped page lies in memory the GPU cannot address (the caller then
// keeps the CPU's way). Each run of consecutive physical pages is looked up once (mtl_gpu_addr makes its pool resident)
static uint64_t sp_pages_n(uint64_t va, uint64_t n) { return ((va + n + 0xFFF) >> 12) - (va >> 12); }
// The physical memory GPU writes were stored into (gsb_swz), by the main command buffer that stores it (g_fence + 1 when encoded): an upload that gathers a sparse texture on the CPU (a
// format the GPU deswizzle does not take, DM_NVN_ZBLOCK) reads that memory when it is encoded, not in the queue's order, so it waits for the stores into the memory behind it
// (sp_store_wait). A ring: a store pushed out while its command buffer may still run leaves its fence for every such upload (g_sps_lost)
enum { SPS_N = 256 };
static struct { uint64_t lo, hi, fence; } g_sps[SPS_N]; static unsigned g_nsps; static uint64_t g_sps_lost;
static void sps_note(uint64_t lo, uint64_t n) {
    uint64_t f = g_fence + 1, i = g_nsps % SPS_N;
    if (g_nsps >= SPS_N && g_sps[i].fence > g_done && g_sps[i].fence > g_sps_lost) g_sps_lost = g_sps[i].fence;
    g_sps[i].lo = lo; g_sps[i].hi = lo + n; g_sps[i].fence = f; g_nsps++;
}
static id<MTLBuffer> up_big_src(uint64_t addr, uint64_t len, uint64_t *off, int may_split);
// A gather reads the physical memory behind a texture once: through zero-copy buffers that the open upload batch holds and retires (up_big_src), not through the pools' own buffers, which
// mtl_gpu_addr puts into the residency set for good, committed on the spot (pool_res). Resident Evil 4 had 148 pools and 777 MB wired that way by the forest, against 8 and 52 MB with
// the CPU gather (issue #2350). Its pages lie scattered through the pools: a buffer per run of pages made 193,000 in 5 minutes, residency commits 2.4 s against 0.9 s while it loaded;
// a buffer over each whole pool, made again in each batch, wired 2 to 3 times the bytes of the runs in the forest. So the pages are sorted by address and taken in spans across gaps of up
// to SP_GAP (a buffer costs about what 1 MB of wiring does in a residency commit), and a buffer the batch already holds over a span is used again. A pool already resident is read
// through its own buffer (mtl_gpu_addr)
enum { SP_GAP = 1u << 20, SP_SRC_N = 256 };
static struct { uintptr_t a, b; __unsafe_unretained id<MTLBuffer> buf; } g_sp_src[SP_SRC_N]; static int g_nsp_src; static unsigned g_sp_src_seq;   // this batch's (g_up_seq); g_up_keep holds them
static int sp_src_gpu(uint64_t a, uint64_t len, uint64_t *g) {
    if (g_sp_src_seq != g_up_seq) { g_nsp_src = 0; g_sp_src_seq = g_up_seq; }
    for (int k = 0; k < g_nsp_src; k++) if (a >= g_sp_src[k].a && a + len <= g_sp_src[k].b) { *g = g_sp_src[k].buf.gpuAddress + (a - g_sp_src[k].a); return 1; }
    uint64_t o; void *b; id<MTLBuffer> u = up_big_src(a, len, &o, 0);   // every source the sparse gather's page table names must stay in this batch
    if (!u) return mtl_gpu_addr(a, len, &b, &o, g);
    if (g_nsp_src < SP_SRC_N) { g_sp_src[g_nsp_src].a = a - o; g_sp_src[g_nsp_src].b = a - o + u.length; g_sp_src[g_nsp_src++].buf = u; }
    *g = u.gpuAddress + o; return 1;
}
typedef struct { uint64_t p, k; } sp_pg;   // a mapped page's physical address and its index in the table
static int sp_pg_cmp(const void *x, const void *y) { uint64_t a = ((const sp_pg *)x)->p, b = ((const sp_pg *)y)->p; return (a > b) - (a < b); }
// The GPU addresses of the pages of [va, va + n) in a sparse pool into pt (0: unmapped, or memory the GPU cannot address); 0 if any mapped page is in such memory. A gather's table
// names buffers of the upload batch it is built in, and runs in that batch
static int sp_pages_gpu(const void *pool, uint64_t va, uint64_t n, uint64_t *pt, int wrote) {   // wrote: the GPU writes the pages (sbw_note each run)
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
            for (uint64_t x = i, e; x < j; x = e) {   // the span lies over two pools: its runs, then their pages
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
// Before a CPU gather of t: the last command buffer that stores into the physical memory behind it, waited for (g_sps)
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
    if (on_sub_thread()) rec_publish();   // a commit may still be a record in the encode thread's block (mtl_finish_gpu_at)
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
// Into dst at off, in the open upload batch, before the deswizzles that read it: the texture's storage, through its page table pt of np pages (sp_pages_gpu), which the batch keeps
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
    [g_up_ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];   // the deswizzles read it
}
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp); void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n); int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n);
// reuse: a texture of the same description, idle on the GPU, to upload into instead of a new one; keep: usage the texture it replaces was upgraded to (tex_upgrade), which the new one
// is made with: a texture drawn into and re-uploaded (a CPU write into one the game renders into) was made without render-target use again and upgraded at its next attachment, a
// second texture and a copy of every level for each re-upload, and its spare (of other usage) was never reused
// prev: the texture this upload replaces, made of the same storage as it was at stamp `since` (span: its tracked range). Only the images written since are read from guest memory
// (pkg_written); the others are copied from prev on the GPU, *taken of them (0: read whole)
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
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = g_bp_lib_ns; *pso_ns = g_bp_pso_ns; }   // Cumulative time spent synchronously creating shader libraries / pipelines on the spot (the long-frame log takes the per-frame difference)
uint64_t mtl_upload_bytes(void) { return g_bp_up_bytes; }   // Cumulative texture upload bytes
// Diagnostic DM_NVN_ZBLOCK=1: all-zero 16-byte blocks of each ASTC texture uploaded on the CPU, per level (an all-zero ASTC block decodes as the error colour, magenta). tex_mtl_ logs
// a texture whose bound levels hold any (zb_report), with each level's zero blocks, its total and whether all its pages are mapped (sparse pools)
static int zblock_on(void) { static int v = -1; if (v < 0) { v = getenv("DM_NVN_ZBLOCK") != NULL; if (v) MLOG("DM_NVN_ZBLOCK: counting all-zero blocks of ASTC textures at upload (diagnostic)"); } return v; }
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
    const char *dir = getenv("DM_NVN_ZBLOCK"); static int nd; static uint64_t seen[16];   // DM_NVN_ZBLOCK=<folder>: also the raw storage of the first such texture of each format and size (up to 16), zb_<serial>.bin (unmapped pages as 0)
    uint64_t kind = (uint64_t)t->d.format << 48 | (uint64_t)t->d.width << 24 | t->d.height; int dup = 0; for (int i = 0; i < nd; i++) dup |= seen[i] == kind;
    if (dir && dir[0] == '/' && nd < 16 && !dup) { seen[nd] = kind; uint64_t n = sp ? MAX(t->L.size, 2u << 20) : t->L.size; uint8_t *m = malloc(n); char fp[1024]; FILE *f;   // a sparse texture: 2 MB, and zbm_<serial>.bin a byte per 4 KB page (1 mapped)
        if (m) { if (sp) nvn_sparse_copy(t->pool, m, (uint64_t)(uintptr_t)t->mem, n); else memcpy(m, t->mem, n);
            snprintf(fp, sizeof fp, "%s/zb_%llu.bin", dir, (unsigned long long)t->serial); if ((f = fopen(fp, "wb"))) { fwrite(m, 1, n, f); fclose(f); nd++; }
            if (sp) { for (uint64_t o = 0; o < n; o += 0x1000) m[o >> 12] = (uint8_t)nvn_sparse_mapped(t->pool, (uint64_t)(uintptr_t)t->mem + o, 0x1000);
                snprintf(fp, sizeof fp, "%s/zbm_%llu.bin", dir, (unsigned long long)t->serial); if ((f = fopen(fp, "wb"))) { fwrite(m, 1, n >> 12, f); fclose(f); } }
            free(m); }
        MLOG("[zb] ser=%llu: block height log2 %d (packaged layout %#x), stride %u, depth %u, target %d, samples %d", (unsigned long long)t->serial, (int)t->L.tileH, t->d.pkg_layout, t->d.stride, t->d.depth, t->d.target, t->d.samples); }
}
// A packed 16-bit format that Metal has only with red and blue exchanged (RGB5, RGB5A1, RGB565: nvn_fmt_to_mtl's r/b swap): the bit position of red in Metal's format (blue is at bit 0); 4 for
// RGBA4, whose four nibbles Metal's ABGR4 holds in reverse (nvn_fmt_to_mtl's reversed mapping); else 0.
// Every host texture of these formats holds Metal's order, as Metal renders, clears and copies into it; only guest memory holds the guest's (red at bit 0). Bytes are converted where they cross:
// uploads (pkg_upload_), buffer ↔ texture copies (rb_swap_gpu), ReadTexels (rb_swap_rows) and stores into storage (gsb_store, rb_swap_gpu). Textures that read guest memory in place (linear and buffer textures, on a
// pool's buffer) take the swap when sampled instead (tex_by_id). Tears of the Kingdom's Depths: smeared rock with red and blue exchanged (GitHub issue #183); Super Mario RPG's RGBA4 shadowmask
// read its channels reversed and took the sunlight off Mushroom Way's battle ground
static int rb_packed_shift(int f) {
    uint32_t pf = 0; uint8_t sw[4];
    if (!nvn_fmt_to_mtl(f, &pf, sw) || sw[0] == NVN_SWZ_R) return 0;
    return pf == MTLPixelFormatB5G6R5Unorm ? 11 : pf == MTLPixelFormatBGR5A1Unorm ? 10 : pf == MTLPixelFormatABGR4Unorm ? 4 : 0;
}
// Exchanges the 5-bit red and blue fields of a region's 16-bit texels in place (rows of wb bytes, row / img apart); green and alpha stay. Shift 4 (RGBA4) reverses the four nibbles. The same
// exchange both ways
static void rb_swap_rows(uint8_t *p, size_t wb, size_t rows, size_t nz, size_t row, size_t img, int shift) {
    uint16_t m = (uint16_t)(31u | 31u << shift);
    for (size_t z = 0; z < nz; z++) for (size_t y = 0; y < rows; y++) {
        uint16_t *q = (uint16_t *)(p + z * img + y * row);
        if (shift == 4) for (size_t i = 0; i < wb / 2; i++) q[i] = (uint16_t)(q[i] << 12 | (q[i] & 0xf0u) << 4 | (q[i] >> 4 & 0xf0u) | q[i] >> 12);
        else for (size_t i = 0; i < wb / 2; i++) q[i] = (uint16_t)((q[i] & ~m) | (q[i] & 31u) << shift | (q[i] >> shift & 31u));
    }
}
// A zero-copy buffer over [addr, addr + len) for the open upload batch, which retires it with its staging, for a large pool, or a pool no address was given out in and that is not resident: a
// one-time read must not leave that memory wired. In Ocarina of Time's first scene, pools that hold only textures took 4.6 GB of the working set against 1.6 GB for the textures made from them.
// nil elsewhere (or if it is refused: the caller looks the address up as usual). The GPU only reads it, so it is made over the pages the upload has just protected, not through pool_buf,
// which would make them writable again and the texture count as written at once
static uint64_t g_up_src_made;   // source buffers made (t_upload_src)
static id<MTLBuffer> up_big_src(uint64_t addr, uint64_t len, uint64_t *off, int may_split) {
    // Not once the raw memory table holds the whole pool (big_full): the lookup finds that buffer. Ocarina of Time's first scene made 4,664 of these in one batch, 2.1 GB counted against the working set for pages already wired
    pthread_mutex_lock(&g_mu); int k = big_of(addr), in = k >= 0 && !g_big[k].full;
    if (k < 0 && len) { int i = pool_find(addr, len); in = i >= 0 && !g_pools[i].res && !addr_meets(g_pools[i].lo, g_pools[i].hi); }
    pthread_mutex_unlock(&g_mu);
    if (!in || !len) return nil;
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)addr & ~(pg - 1), b = ((uintptr_t)addr + len + pg - 1) & ~(pg - 1);
    // The staging budget does not count guest pages. End the batch only between textures so its completion can release those pages without invalidating a source already being assembled.
    size_t budget = up_src_budget();
    if (may_split && budget && g_up_src_bytes && b - a > budget - MIN(g_up_src_bytes, budget)) { up_flush(); up_begin(); }
    id<MTLBuffer> buf = [g_dev newBufferWithBytesNoCopy:(void *)a length:b - a options:MTLResourceStorageModeShared deallocator:nil];
    if (!buf) return nil;
    g_up_src_made++;
    if (budget) g_up_src_bytes += b - a;
    res_add(buf); [g_up_keep addObject:buf]; *off = (uint64_t)addr - a;
    return buf;
}
// A re-upload read every layer and level of a texture from guest memory when one page of it was written. Ocarina of Time fills a 1800x1380x8 RGBA8 array (38.5 MB) layer by layer
// from its CPU while it draws with it: 13 whole uploads in 6 frames, six of them in one frame, for 3 to 936 of its 2,466 pages each (docs/reference/frame-rate.md). A layer's level
// (a 3D texture's level) lies in a byte range of its own (nvn_tex.c), so the images to read are those with a page written since the stamp; pages are 16 KB, so an image that shares
// one with a written neighbour is read too. A byte each (level * layers + layer), *nup of them set; NULL when the layout does not say (read whole)
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
// CPU snapshots of whole textures and placement tiles share the same storage hazard check. GPU uploads express this dependency through queue order instead.
static int tex_cpu_wait(const tex_t *t) {
    uint64_t lo = (uint64_t)(uintptr_t)t->mem, hi = lo + t->L.size;
    // The existing write sets cover copies, updates and shader stores. Sparse mappings can alias another guest address, so a CPU upload cannot prove disjointness there.
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
    int rb_shift = rb_packed_shift(t->d.format);   // the RGB565 family is repacked into Metal's order on the CPU (rb_packed_shift)
    int linear = (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) != 0;   // linear storage no view could take (linear_fit): its rows are copied on the CPU
    int direct = !linear && !rb_shift && stamp && direct_upload_on() && g_nvn_deswz_gpu && !(t->pool && nvn_sparse_pool(t->pool)) && nvn_deswz_gpu_direct_supported(&t->d);
    if (direct) d.usage |= MTLTextureUsageShaderWrite | MTLTextureUsagePixelFormatView;
    id<MTLTexture> mt = reuse && reuse.pixelFormat == d.pixelFormat && reuse.width == d.width && reuse.height == d.height && reuse.mipmapLevelCount == d.mipmapLevelCount && reuse.textureType == d.textureType
                        && (reuse.usage & d.usage) == d.usage
                        ? reuse : [g_dev newTextureWithDescriptor:d];
    if (!mt) { RED_ONCE(2, "failed to create MTLTexture %ux%u format %#x target %d", t->d.width, t->d.height, t->d.format, t->d.target); return nil; }
    if (mt.allocatedSize >= (256u << 20)) { static int nb; if (nb++ < 20)   // the textures that take most of the GPU's memory (a GPU out-of-memory error says nothing of which)
        MLOG("large texture ser=%llu %ux%ux%u format %#x target %d levels %u: %llu MB%s", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels,
             (unsigned long long)(mt.allocatedSize >> 20), t->pool && nvn_sparse_pool(t->pool) ? ", in a sparse virtual pool" : t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL) ? ", in a virtual pool" : ""); }
    if (g_allocprof > 0) { static int n; if (n++ < 30 || n % 500 == 0) MLOG("new texture (%s) #%d ser=%llu %ux%ux%u format %#x target %d levels %u storage %p", __func__, n, (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.target, t->d.levels, (void *)t->mem); }
    res_add(mt);   // Both first upload and re-upload register residency here (Metal 4 does not make resources resident automatically; a missed registration = GPU address page fault)
    int sparse = t->pool && nvn_sparse_pool(t->pool);
    if (stamp) {   // no stamp: a one-time upload of content the CPU will not change (tex_init_fill), with no write tracking
        *stamp = nvn_dirty_now();
        if (sparse) nvn_sparse_protect(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size); else nvn_dirty_protect(t->mem, t->L.size); }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    unsigned layers = (t->d.target == 4 || t->d.target == 9) ? t->d.depth : t->d.target == 8 ? 6 : t->d.target == 3 ? t->d.height : 1;   // a cube array's depth counts layer-faces (tex_desc)
    // Taken after the stamp and the protect: a write that lands from here on faults and is found at the next look
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
    // A texture in a large pool (BIG_MIN) or one that holds only textures is read through a buffer of its own, held by the upload batch: a one-time read must not leave that memory wired for good
    id<MTLBuffer> upsrc = nil; unsigned src_batch = 0;   // the upload batch that holds upsrc (g_up_seq)
    if (!linear && !rb_shift && !sparse && g_nvn_deswz_gpu) { up_begin(); upsrc = up_big_src((uint64_t)t->mem, t->L.size, &mo, 1); src_batch = g_up_seq; if (upsrc) mb = (__bridge void *)upsrc; }
    // A sparse pool's texture is gathered on the GPU through its page table (sp_pages_gpu), but on the CPU for DM_NVN_ZBLOCK, which counts its blocks there
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
        // The GPU reads the memory pool at execution time, which is closer to a real Switch than a snapshot at CPU upload time; the dirty-page stamp is still taken before encoding, and later writes still trigger a re-upload.
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
                // Any encoded writes must finish before fallback copies replace
                // them; both versions read the same source and write all texels.
                [g_up_ce barrierAfterEncoderStages:MTLStageDispatch beforeEncoderStages:MTLStageDispatch visibilityOptions:MTL4VisibilityOptionDevice];
            }
        }
        // A sparse pool's texture is gathered into the same staging block, after the deswizzled levels: one allocation, as a second could flush the batch that holds the first
        size_t stg_off, sto = ((total ? total : 16) + 255) & ~(size_t)255, gsz = spt ? (t->L.size + 255) & ~(size_t)255 : 0;
        id<MTLBuffer> stg = up_alloc(sto + gsz, &stg_off);
        if (spt) {   // up_alloc may have flushed the batch that holds the buffers the page table names: the table again, in this batch (a page it no longer reaches reads as 0)
            if (g_up_seq != src_batch) sp_pages_gpu(t->pool, (uint64_t)(uintptr_t)t->mem, t->L.size, spt, 0);
            mb = (__bridge void *)stg; mo = stg_off + sto; sp_gather(t, spt, spn, stg, mo); free(spt); spt = NULL;
        }
        // up_alloc may have flushed the batch that held the first source buffer: only then the new batch needs one of its own. Made for every texture, each staged upload from a large pool
        // put its storage twice into the residency set (Switch Sports Resort's first court frame: 855 textures, 1.8 GB of source buffers for 908 MB of storage; t_upload_src)
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
                size_t poff; id<MTLBuffer> params = up_param_alloc(&poff);   // Metal 4 has no setBytes: the parameter block of each deswizzle is cut from a shared block (no new GPU buffer each time)
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
        g_nup_blits = nb0;   // Roll back the copies already recorded for this texture: the already-encoded deswizzle only writes the staging area and is harmless; this texture takes the CPU path below instead
        if (!g_cb) up_flush();
        if (mb) RED_ONCE(41, "GPU deswizzle does not support format %#x target %d; falling back to the CPU", t->d.format, t->d.target);
        else RED_ONCE(19, "could not retain a texture source in the new upload batch; falling back to the CPU");
    }

    // A sparse pool's texture is read through a copy gathered from its page table, unmapped pages as 0 (a mip tail at a 4 KB offset has no host alias; reading the alias stopped the host with SIGBUS)
    // (after the GPU writes stored into that memory: sp_store_wait)
    if (tex_cpu_wait(t) && stamp) {
        // CPU writes could have unprotected pages while we waited. Take a new stamp and protect again before snapshotting them.
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
            if (linear) {   // one 2D level (linear_fit): rows of the storage's stride, the same pitch GetStorageSize counts
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
