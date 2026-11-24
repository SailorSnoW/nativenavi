// Shaders: the shader map, c1 banks, the user manifest, library loading, the depth mode's entry points, Metal 4 function descriptors and their specializations, side entry points, shader_load and the programs' shaders.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ================= Shaders / descriptor pool / pipelines / draws =================
// Game threads register textures and samplers, the submit thread encodes arguments: lend a waiting submitter's priority to the owner.
// Measurements: docs/reference/frame-rate.md, Registry lock priority inheritance.
static os_unfair_lock g_rmu = OS_UNFAIR_LOCK_INIT;
// ---------- Shader mapping table: Switch control section hash → PC metallib (produced by dm/gfx/tools/shader_split.py) ----------
static NSMutableDictionary<NSString *, NSString *> *g_shmap;
static NSString *g_shmap_path, *g_msl_dir;
// The library may be one file beside its folder, b-<chain>.img (dm/tools/shader_lib.py, msl_image.h), read in place. Each file comes from the folder first: what a partial dmsh_build.py run rebuilt goes
// over the image, a build folder from before the image works as it did, and a host running from a folder goes on when a build packs it (the image is whole before the folder goes). So the image is
// opened at the first file the folder lacks, and tried again each time while it is not there. img: the image the entry is in
static _Atomic(romfs_img *) g_msl_img;
static pthread_mutex_t g_msl_img_lock = PTHREAD_MUTEX_INITIALIZER;
static const img_ent *msl_image_file(NSString *path, romfs_img **img) {
    if (!g_msl_dir || path.length <= g_msl_dir.length || ![path hasPrefix:g_msl_dir]) return NULL;   // a .metallib from elsewhere (the tests' lib_from_file)
    romfs_img *m = atomic_load_explicit(&g_msl_img, memory_order_acquire);
    if (!m) {
        pthread_mutex_lock(&g_msl_img_lock);
        if (!(m = atomic_load_explicit(&g_msl_img, memory_order_acquire))) {
            m = romfs_image_open([g_msl_dir stringByAppendingString:@".img"].fileSystemRepresentation);
            if (m) atomic_store_explicit(&g_msl_img, m, memory_order_release);
        }
        pthread_mutex_unlock(&g_msl_img_lock);
    }
    *img = m;
    return m ? msl_image_find(m, path.lastPathComponent.UTF8String) : NULL;
}
static NSData *msl_data(NSString *path) {   // a file of the library, nil when it has none
    NSData *d = [NSData dataWithContentsOfFile:path];
    if (d) return d;
    romfs_img *m; const img_ent *x = msl_image_file(path, &m);
    if (!x) return nil;
    NSMutableData *o = [NSMutableData dataWithLength:(NSUInteger)x->b];
    return romfs_image_read(m->fd, o.mutableBytes, (size_t)x->b, (off_t)x->a) ? o : nil;
}
static NSString *exe_rel(const char *rel) {
    char buf[4096]; uint32_t n = sizeof buf;
    if (_NSGetExecutablePath(buf, &n)) return nil;
    char *dir = dirname(buf);
    return [NSString stringWithFormat:@"%s/%s", dir, rel];
}
// Shader mapping table (output of tools/shader_split.py): at runtime only 'Switch control section key → stage' is used; shaders are always translations of the Switch originals (tools/dmsh_build.py produces <key>_b.metallib + reflection offline)
static void shmap_read(void);
static void warm_start(void);
// The shader map, the library directory and its tables, read once from any thread (the prefetch, mtl_prog_prefetch, runs on the game's threads)
static void shmap_read_once(void) { static dispatch_once_t once; dispatch_once(&once, ^{ shmap_read(); }); }
// That, then the prewarm (warm_start) once: the submit thread and the tools only. The flag is set before warm_start runs, so a call from inside it (pso_prebuild) returns at
// once; inside the dispatch_once that call re-entered it and libdispatch aborted (Tears of the Kingdom and Pokémon Scarlet at boot, chain d0e99bd4b2fe)
static void shmap_load(void) { shmap_read_once(); static int warmed; if (!warmed) { warmed = 1; warm_start(); } }
static void shmap_read(void) {
    if (g_shmap) return;
    const char *e = getenv("DM_NVN_SHMAP");
    NSString *path = e ? @(e) : exe_rel([NSString stringWithFormat:@"../games/%s/shader_map.tsv", DM_GAME_ID].UTF8String);   // default: the current game profile folder (dm/games/<id>; the Mac app's launcher sets DM_NVN_SHMAP to the copy in its Resources/dm)
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    if (!txt) MDIE("cannot read the shader map %s (DM_NVN_SHMAP can name another)", path.UTF8String);
    g_shmap = [NSMutableDictionary dictionary];
    for (NSString *ln in [txt componentsSeparatedByString:@"\n"]) {
        if (!ln.length || [ln hasPrefix:@"#"] || [ln hasPrefix:@"package"]) continue;
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count > 3) g_shmap[c[3]] = c[2];
    }
    g_shmap_path = path;
    const char *m = getenv("DM_MSL_DIR");
    // Library directory = <DM_MSL_DIR or default cache>/b-<translation chain version>: the version comes from dm/gfx/nvn/dmsh_chain.h (tools/dmsh_chain.py), one for every game; a host built from an old source tree only accepts libraries of the old version
    g_msl_dir = [NSString stringWithFormat:@"%@/b-%s", m ? @(m) : [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work"], DMSH_CHAIN];
    if (![NSFileManager.defaultManager fileExistsAtPath:g_msl_dir]) {
        NSString *image = [g_msl_dir stringByAppendingString:@".img"];
        if ([NSFileManager.defaultManager fileExistsAtPath:image]) MLOG("shader libraries: one file, %s", image.UTF8String);
        else MLOG("[RED] shader library %s (a folder or a .img file) does not exist: translation chain version %s of this source tree has not been built yet (dm/gfx/tools/dmsh_build.py)", g_msl_dir.UTF8String, DMSH_CHAIN);
    }
    // The library's stamp (inputs.tsv, written by a full dmsh_build.py run): the game and the digest of the profile inputs it was built from. The chain version is the same for every
    // game and every state of a profile's tables, so the stamp is what ties c1_full.tsv / cb_extent.tsv to these libraries: each names the inputs of the library it was made from
    NSData *stamp_data = msl_data([NSString stringWithFormat:@"%@/inputs.tsv", g_msl_dir]);
    NSString *stamp = stamp_data ? [[NSString alloc] initWithData:stamp_data encoding:NSUTF8StringEncoding] : nil, *st_game = nil, *st_inputs = nil;
    for (NSString *ln in [stamp componentsSeparatedByString:@"\n"]) {
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count == 2 && [c[0] isEqual:@"game"]) st_game = c[1]; else if (c.count == 2 && [c[0] isEqual:@"inputs"]) st_inputs = c[1];
    }
    if (!st_inputs) MLOG("[RED] shader library %s has no stamp (inputs.tsv): no full dmsh_build.py run made it as it is, so c1_full.tsv and cb_extent.tsv are not taken", g_msl_dir.UTF8String);
    else if (st_game && ![st_game isEqual:@(DM_GAME_ID)]) MLOG("[RED] shader library %s was built for game %s, not %s", g_msl_dir.UTF8String, st_game.UTF8String, DM_GAME_ID);
    else MLOG("shader library stamp: game %s, inputs %s", st_game.UTF8String, st_inputs.UTF8String);
    NSString *cp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"c1_full.tsv"];   // Same directory as the mapping table (dm/games/<id>, or Resources/dm inside the Mac app, copied along by dm/mac/pack_app.py)
    NSString *ct = cp ? [NSString stringWithContentsOfFile:cp encoding:NSUTF8StringEncoding error:nil] : nil;
    NSString *head = ct ? [[ct componentsSeparatedByString:@"\n"] firstObject] : nil;
    NSString *want = [NSString stringWithFormat:@"# chain b-%s inputs %@ ", DMSH_CHAIN, st_inputs ?: @"-"];   // a library without a stamp matches no table
    if (!head || ![head hasPrefix:want]) {
        MLOG("[RED] c1_full.tsv is stale or missing (its header must name this library's chain and inputs; a full dmsh_build.py run makes it); every c1 gets its own 64 KiB");
    } else {
        g_c1_full_valid = 1; g_c1_full = [NSMutableSet set];
        for (NSString *line in [ct componentsSeparatedByString:@"\n"]) if (line.length && ![line hasPrefix:@"#"]) [g_c1_full addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    }
    {   // cb_extent.tsv (tools/cb_extent.py), beside c1_full.tsv: without it a uniform block updated in part inside a pass is copied whole, up to 64 KB (ubp_get)
        NSString *xp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"cb_extent.tsv"];
        NSString *xt = [NSString stringWithContentsOfFile:xp encoding:NSUTF8StringEncoding error:nil];
        NSString *xh = xt ? [[xt componentsSeparatedByString:@"\n"] firstObject] : nil;
        if (!xh || ![xh hasPrefix:want]) MLOG("cb_extent.tsv is stale or missing: uniform reads are taken to reach the whole binding");
        else { NSMutableDictionary *d = [NSMutableDictionary dictionary];
            for (NSString *line in [xt componentsSeparatedByString:@"\n"]) { if (!line.length || [line hasPrefix:@"#"]) continue;
                NSRange t = [line rangeOfString:@"\t"]; if (t.location != NSNotFound) d[[line substringToIndex:t.location]] = [line substringFromIndex:t.location + 1]; }
            g_cb_ext = d; MLOG("uniform read extents: %lu shaders (cb_extent.tsv)", (unsigned long)d.count); }
    }
    MLOG("shader route B (translated Switch originals): shader map %lu entries (%s), metallib directory %s", (unsigned long)g_shmap.count, path.UTF8String, g_msl_dir.UTF8String);
}

@interface NvShader : NSObject { @public id<MTLFunction> fn, discard_fn, fn_d; NSString *u_name; id<MTLLibrary> ulib; NSMutableDictionary *ufn; int u_side, ulib_state, u_alpha, u_logic;   // u_name: the typed-output entry point (dmsh_entry.py fs_uint), nil when the shader has none; in the side library <key>_bx when u_side, else in its own (ulib: that library, loaded on first need when u_side; ulib_state as afn_state); ufn: it specialized per pipeline's target classes, alpha test and logic operation (typed_fn); u_alpha / u_logic: it holds those two (the reflection's entry_u_alpha, entry_u_logic); fn_d: the dual-source one (dmsh_entry.py fs_dual)
    id<MTLLibrary> xlib; int xlib_state;   // the side library <key>_bx, loaded once on first need (bx_lib) for at_fn, lo_fn and typed_fn
    uint32_t cb_ext[16], sb_ro; nvn_refl rf; id<MTLArgumentEncoder> ae; NSUInteger arg_len; int stage; uint64_t logged; char key[24]; id<MTLBuffer> c1buf; uint64_t c1_gpu; size_t c1off;
    uint8_t mesh_kind, mesh_notask; uint16_t mesh_threads, mesh_payload;   // an NVN2 task (1) or mesh (2) shader, a mesh shader with no task, its workgroup's threads, its payload's bytes (dmsh_meshstage.py)
    id<MTLFunction> mesh_obj;   // a mesh shader with no task: the object function that launches its workgroups in blocks of DMSH_MESH_GRID (dmsh_meshstage.py main0_o)
    int cps_failed;   // Metal refused this compute shader's pipeline (dispatch): its dispatches are skipped without asking again
    int split_failed;   // a pass of a kernel in two passes did not build (split_pipes): it runs in one pass for the rest of the run
    uint64_t loop_sub, loop_gpu; _Atomic int loop_reported;   // this submission's per-kernel cap-hit state; feedback logs at most once per loaded kernel
    uint8_t up_mfx;   // a compute shader that is the game's own upscaler (DM_GAME_UPSCALER_KEYS, from the shader map): 0 not decided, 1 drawn by MetalFX when DM_UPSCALER=metalfx, 2 not one or one its colour keeps (up_mfx_wanted)
    uint8_t up_color;   // such a shader's colour, from its code at load with DM_UPSCALER=metalfx (up_mfx_color; UP_COLOR_UNREAD otherwise)
    uint8_t up_chain;   // a compute shader of the game's upscaler chain (DM_GAME_UPSCALER_CHAIN_KEYS, from the shader map): 0 not looked up, 1 one of them, 2 not one (up_chain_named)
    uint64_t cbs_seen; uint32_t cbs_runs;   // the command buffer that listed this shader last, and how many have (cbs_note)
    // The argument buffer encoded last time, borrowed from its arena: valid only while ab_pass is the current pass. Keeping it here strongly would pin retired arenas for shaders no longer drawn.
    uint8_t ab_cbuf_only;   // 0 not decided, 1: its only bindings read through a uniform buffer are plain CBUF slots (args_cbuf_only), so a block can be patched when only those changed; 2 not
    __unsafe_unretained id<MTLBuffer> ab_buf; uint64_t ab_gpu; size_t ab_off; uint64_t ab_pass; uint32_t ab_binst; uint64_t ab_pgen, ab_gen, ab_ugen;   // ab_gen / ab_ugen: the stage's binding generations it was written at (g_args_gen); ab_pass: serial of the render pass that encoded this argument block - reused only within the same pass (the arena holding a block from another pass / another submission may already have been reclaimed and rewritten); ab_pgen: g_psnap_gen when it was encoded (pools, pool_gpu)
    id<MTLLibrary> bflib; int bflib_state; NSMutableArray *bffn;   // bflib: the readable-tile version <key>_bf (state as afn_state), bffn: its functions specialized by their numbers (NvBfFn, bf_fn)
    uint64_t mis[4];   // mis: texture slots 0-255 already reported for a type mismatch (tex_mis_note)
    NSString *ventry; id<MTLFunction> vfn; int vfn_state;
    NSString *pentry, *wlentry, *vinentry;   // a vertex shader's entry point in its pulling library, that library's wide-line one, and its entry point in the shadow-input library, as the reflection names them (side_entry); nil: ventry's name (a layer entry point)
    id<MTLFunction> pfn, wlfn; int pfn_state, has_pull;   // wlfn: the wide-line entry of the pulling library (wlentry, dmsh_pull.py; nvn_wline.inc), nil when it has none
    id<MTLLibrary> alib; id<MTLFunction> afn[2]; int afn_state, has_alpha, has_bz;   // has_bz: its entry points read the blend weight word (bz_bind)
    id<MTLLibrary> llib; NSMutableDictionary<NSNumber *, id<MTLFunction>> *lfn; int lfn_state, has_logic; } @end   // llib: the side library <key>_bx for logic operations, lfn: fn's entry point (a shader without the typed-output one, which holds its own) specialized per set of targets (lo_fn; lfn_state as afn_state; has_logic: the reflection names its entry points, dmsh_logic.py)   // afn: the alpha test entry points of fn and fn_d in the side library <key>_bx (alib), (at_fn; afn_state 0 untried, 1 loaded, 2 failed; has_alpha: the reflection names them, dmsh_alpha.py)   // pfn: the same entry of the pulling library <key>_bp (pull_fn; has_pull: the reflection names one, dmsh_pull.py)   // vertex: the entry point loaded (by the depth mode); vfn: the same entry of the shadow-input library <key>_bv (vin_fn; state 0 untried, 1 loaded, 2 failed)
@implementation NvShader
- (void)dealloc { free(rf.c1); }
@end
// One specialization of a shader's readable-tile version: what each sample reads from the tile (fx, -1 none; only the shader's nfc entries), the attachments read (fbmask) and the
// attachment each colour output goes to (lmap); fn nil when Metal refused it (not asked again)
typedef struct { int8_t fx[32]; uint8_t fbmask, lmap[8]; } bf_key;
@interface NvBfFn : NSObject { @public bf_key k; id<MTLFunction> fn; } @end
@implementation NvBfFn @end
#include "nvn_pso_skey.h"
@interface NvProg : NSObject { @public NvShader *sh[6]; NvProg *mesh_raster; int8_t c_sw, c_iw, c_img; } @end   // c_sw / c_iw / c_img: prog_swrites, udc_note_draw's image write, image_prepare's written image slots; + 1 once known (the shaders are fixed once made)   // sh: by host stage (0 vertex, 1 pixel, 3 task, 4 mesh, 5 compute; 2, the geometry stage, stays empty); mesh_raster: a task / mesh program\'s pixel stage alone (mesh_tasks_draw)
@implementation NvProg @end
// Warm-up entry: built in the background, waiting to be taken by the submit thread. vs / fs are strong references (in pso they are only raw pointers, and the background dictionary is released when the task ends)
@interface WarmItem : NSObject { @public int kind, record, pull, cps_off; NSString *key, *vs_key, *fs_key, *ts_key; NvShader *shader, *vs, *fs, *ts; pso_key pso; id<MTLFunction> fsfn; id object; } @end   // record: a prefetched library, written to the prewarm lists when claimed (shader_get's lines)
@implementation WarmItem @end
static NSMutableDictionary<NSString *, NvShader *> *g_shcache;

// A bank with room for `room` more bytes past the current offset (a new zeroed one when the last has none); under g_c1_lock. NULL when it cannot be allocated
static id<MTLBuffer> c1_bank(size_t room) {
    if (!g_c1_banks) g_c1_banks = [NSMutableArray array];
    if (!g_c1_banks.count || g_c1_bank_off + room > C1_BANK_SIZE) {
        id<MTLBuffer> b = [g_dev newBufferWithLength:C1_BANK_SIZE options:MTLResourceStorageModeShared];
        if (!b) return nil;
        res_add(b);
        memset(b.contents, 0, C1_BANK_SIZE);
        [g_c1_banks addObject:b];
        g_c1_bank_off = 0;
    }
    return g_c1_banks.lastObject;
}

static void c1_store(NvShader *o) {
    size_t len = o->rf.c1_len;
    NSString *guardKey = [NSString stringWithUTF8String:o->key];
    if ([guardKey hasSuffix:@"_b"]) guardKey = [guardKey substringToIndex:guardKey.length - 2];
    os_unfair_lock_lock(&g_c1_lock);   // Both the warm-up thread and the submit thread load shaders
    // A dedicated c1 is 64 KiB of its own, zero past its data: a slice of a bank (zeroed when made), not an MTLBuffer of its own. Without c1_full.tsv (a partial library) every
    // shader gets one, and one buffer each put about 10,000 allocations into the queue's residency set on Hyrule Warriors: Age of Imprisonment's way to its first field; every
    // commit then spent its time in the kernel (the Metal 4 submission queue busy about 75% of the time, almost all system time), command allocators stayed in flight while the
    // GPU sat idle, and the game ran at about 10 fps. Slices of 4 MiB banks are 64 to a buffer
    int dedicated = !g_c1_full_valid || [g_c1_full containsObject:guardKey];
    size_t need = (len + 255u) & ~((size_t)255u);
    id<MTLBuffer> b = c1_bank(dedicated ? C1_DECL_SIZE : need + 256u + C1_DECL_SIZE);
    if (!b) { os_unfair_lock_unlock(&g_c1_lock); MDIE("c1 buffer block allocation failed"); }
    size_t off = g_c1_bank_off;
    memcpy((uint8_t *)b.contents + off, o->rf.c1, len);
    g_c1_bank_off += dedicated ? C1_DECL_SIZE : need + 256u;
    g_c1_count++;
    o->c1buf = b;
    o->c1_gpu = b.gpuAddress;
    o->c1off = off;
    os_unfair_lock_unlock(&g_c1_lock);
}

static NSMutableArray<WarmItem *> *g_warm_items;
static os_unfair_lock g_warm_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_warm_ready;
static uint64_t g_warm_t0;
static _Atomic int g_warm_running;   // Background warm-up has not finished yet
static _Atomic int g_warm_bg;   // the prewarm runs beside the game (no prepare step: no window, or DM_PSO_PREPARE=0), not before its first frame
static _Atomic int g_warm_stop;   // the process is ending (mtl_warm_stop): the prewarm takes no more lines
static _Atomic uint64_t g_warm_shaders, g_warm_rps, g_warm_mps, g_warm_cps, g_warm_skip_fz, g_warm_skip_stale;   // the prewarm's workers count into them
static id<MTLRenderPipelineState> mtk_build(const pso_key *, NvShader *, NvShader *, NvShader *, NSError **, int *);
static _Atomic uint64_t g_mtk_arch, g_mtk_built, g_mtk_dev;   // native mesh pipelines (mtk_build, all threads): taken from the pipeline archive, built and captured for it (arch_harvest), built with no Metal 4 descriptor (not kept)
static uint64_t g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps;
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap);
static FILE *g_pso_record; static int g_pso_record_checked;
static FILE *pso_record_file(void) {
    if (g_pso_record_checked) return g_pso_record;
    g_pso_record_checked = 1;
    const char *path = getenv("DM_PSO_RECORD");
    if (path && *path) g_pso_record = fopen(path, "a");
    return g_pso_record;
}
static void pso_record(const char *fmt, ...) {
    FILE *f = pso_record_file();
    if (!f) return;
    va_list ap; va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap); fflush(f);
}
// User manifest: shaders / pipelines used on this machine, including demands served by hybrid stand-ins before their builds finish; warmed at the next launch with the seed.
// Path = DM_PSO_USER, or pso_user.tsv under the parent of the save directory (DM_SAVE); tests use a fresh save copy per run, so they do not pollute each other
static NSString *pso_user_path(void) {
    const char *u = getenv("DM_PSO_USER"), *save = getenv("DM_SAVE");
    if (u && *u) return @(u);
    return save && *save ? [[@(save) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pso_user.tsv"] : nil;
}
static FILE *g_pso_user; static int g_pso_user_checked;
static NSMutableSet<NSString *> *g_pso_user_seen;
static FILE *pso_user_file(void) {
    if (g_pso_user_checked) return g_pso_user;
    g_pso_user_checked = 1;
    NSString *path = pso_user_path();
    if (path) g_pso_user = fopen(path.fileSystemRepresentation, "a");
    return g_pso_user;
}
static void pso_user_record(const char *fmt, ...) {
    FILE *f = pso_user_file();
    if (!f) return;
    char line[8192]; va_list ap; va_start(ap, fmt); vsnprintf(line, sizeof line, fmt, ap); va_end(ap);
    if (!g_pso_user_seen) {
        g_pso_user_seen = [NSMutableSet set];
        NSString *seed = g_shmap_path ? [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"] : nil;
        NSMutableArray<NSString *> *paths = [NSMutableArray array]; if (seed) [paths addObject:seed]; NSString *user = pso_user_path(); if (user) [paths addObject:user];
        for (NSString *path in paths) { NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            for (NSString *old in [txt componentsSeparatedByString:@"\n"]) if (old.length && ![old hasPrefix:@"#"]) [g_pso_user_seen addObject:old]; }
    }
    size_t ll = strlen(line); NSString *s = [[NSString alloc] initWithBytes:line length:ll && line[ll - 1] == '\n' ? ll - 1 : ll encoding:NSUTF8StringEncoding];   // Compared with the manifest file line by line (without the newline)
    if (!s || [g_pso_user_seen containsObject:s]) return;
    [g_pso_user_seen addObject:s]; fputs(line, f); fflush(f);
}
// A library's name for the pipeline archive's index (arch_key): a digest of its bytes (SHA-256, first 16 bytes in hex), the same at every start and in every container. Not the file
// it came from: Metal hands out one MTLFunction per function content, device-wide, so two shader keys whose libraries are the same bytes (Unity's sprite shaders: Cuphead's
// c58ee1901c9eaaff, 229e46ed4370120b, bf6707736e88cca0 and 8e36438c7208b3c6 are one 23,038-byte library) share one function object, and fn4_note keeps the descriptor of whichever
// of them was noted last. Which one that is depends on the order the prewarm's workers and the draws load them, so a key by file name changed from one start to the next and the
// index missed bases the archive held: Cuphead compiled 8 or 9 of its 28 prewarm bases again at every start. By content, any of them gives the same key, and the pipeline is the same:
// one compile serves them all. A library with no name (one made from source: the diagnostic overrides, embedded kernels) has no key and is looked up in the files without an index
static char g_lib_tag;
static id<MTLLibrary> lib_tag(id<MTLLibrary> lib, NSString *name) { if (lib && name) objc_setAssociatedObject(lib, &g_lib_tag, name, OBJC_ASSOCIATION_COPY_NONATOMIC); return lib; }
static NSString *lib_digest(const void *p, size_t n) {
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(p, (CC_LONG)n, h);
    char hex[33]; for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return @(hex);
}
// Build the library from a .metallib file without permanently occupying a file descriptor: newLibraryWithURL keeps that file open for as long as the library object lives (one per game shader: a run holds more of them than a process's default
// soft limit of 256 descriptors). Here the file is mapped into memory and closed immediately, and the library is built from the in-memory data; the mapping is released when the library's data object is freed.
// The library is tagged with its digest (lib_tag), from the mapped bytes
static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); struct stat st;
    if (fd < 0) {   // not in the folder: the library's image, whose one descriptor is shared, and only this file's pages are mapped
        int open_errno = errno; romfs_img *img; const img_ent *x = msl_image_file(path, &img); msl_map f;
        if (x && msl_image_map(img, x, &f)) {
            void *base = f.base; size_t base_len = f.base_len;
            dispatch_data_t d = dispatch_data_create(f.data, f.size, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(base, base_len); });
            return lib_tag([g_dev newLibraryWithData:d error:err], lib_digest(f.data, f.size));
        }
        if (!x) errno = open_errno;
    }
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) { if (fd >= 0) close(fd); if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"cannot open %@", path]}]; return nil; }
    size_t len = (size_t)st.st_size; void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0); close(fd);
    if (m == MAP_FAILED) { if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"mmap failed %@", path]}]; return nil; }
    dispatch_data_t d = dispatch_data_create(m, len, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(m, len); });
    return lib_tag([g_dev newLibraryWithData:d error:err], lib_digest(m, len));
}
// NVN depth mode (nvnDeviceSetDepthMode): every vertex library has two entry points (dmsh_entry.py vs_depth): main0 rewrites z for the GL convention (near = -w, NVN's default),
// main0_z leaves z as is (near = 0, which Link's Awakening, 13 Sentinels and Engage set). Loading picks by the mode the game set, so no game needs a setting for it
static _Atomic int g_depth_zero, g_vs_loaded;
void mtl_set_depth_zero(int on) {
    if (atomic_exchange(&g_depth_zero, on) == on) return;
    MLOG("depth mode: %s", on ? "near=0, vertex shaders use the main0_z entry" : "near=-w (GL), vertex shaders use the main0 entry");
    int n = atomic_load(&g_vs_loaded);
    if (n) MLOG("[RED] depth mode changed after %d vertex shaders were loaded: they keep the old convention", n);
}
// The Metal 4 function descriptor of each function pipelines are built from (pso_build): MTL4Compiler takes a library, an entry name and the constant values a function was
// specialized with, not an MTLFunction, so every function made from a library is noted here as it is made. A function with no note (the diagnostic fragment overrides
// DM_NVN_HILITE and DM_NVN_MAGSCAN) keeps the MTLDevice path
static os_unfair_lock g_fn4_lock = OS_UNFAIR_LOCK_INIT;
static NSMapTable *g_fn4;   // id<MTLFunction> → MTL4FunctionDescriptor
static id<MTLFunction> fn4_note(id<MTLFunction> f, id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv) {
    if (!f || !lib || !name) return f;
    MTL4LibraryFunctionDescriptor *ld = [MTL4LibraryFunctionDescriptor new]; ld.library = lib; ld.name = name;
    MTL4FunctionDescriptor *d = ld;
    if (cv) { MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = cv; d = sd; }
    os_unfair_lock_lock(&g_fn4_lock);
    if (!g_fn4) g_fn4 = [[NSMapTable alloc] initWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory capacity:0];   // by identity: a function made again from the same library and name (other constants: bf_fn) is another function
    [g_fn4 setObject:d forKey:f];
    os_unfair_lock_unlock(&g_fn4_lock);
    return f;
}
static MTL4FunctionDescriptor *fn4_of(id<MTLFunction> f) {
    if (!f) return nil;
    os_unfair_lock_lock(&g_fn4_lock); MTL4FunctionDescriptor *d = [g_fn4 objectForKey:f]; os_unfair_lock_unlock(&g_fn4_lock);
    return d;
}
// A function that declares function constants, specialized with cv (an empty set keeps every default). On the Metal 4 path the pipeline's descriptor specializes it (fn4_note
// records cv, and MTL4Compiler compiles it with the pipeline), so the MTLFunction stays unspecialized: its reflection and argument encoders are the same. MTLLibrary's
// newFunctionWithName:constantValues: compiles through the MTLDevice, synchronously, on the thread that loads the shader: when Animal Crossing loaded such a shader during play,
// the Metal performance HUD counted it ("Compiled 2 shaders. Total compilation time is 102.70ms. Consider adopting MTL4Compiler"). A pipeline the MTLDevice
// builds after all (pso_build's fallback) takes the function fn_dev specializes from the descriptor
static int pso4_compiler(void);
static os_unfair_lock g_fn_dev_lock = OS_UNFAIR_LOCK_INIT;
@interface FnDev : NSObject { @public id<MTLFunction> fn; } @end
@implementation FnDev @end
static NSMapTable *g_fn_dev;   // a function fn_spec left unspecialized → its stable specialization record (fn filled by fn_dev)
static id<MTLFunction> fn_lib_spec(id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv, NSError **e) {
    if (!lib || !name || !cv) return nil;
    return fn4_note(dev_fn(lib, name, cv, e), lib, name, cv);
}
static id<MTLFunction> fn_spec(id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv, NSError **e) {
    if (!lib || !name) return nil;
    if (!pso4_compiler()) return fn_lib_spec(lib, name, cv, e);
    id<MTLFunction> f = [lib newFunctionWithName:name];
    if (!f) { if (e) *e = [NSError errorWithDomain:@"dm" code:1 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"no function %@ in the library", name]}]; return nil; }
    if (!f.functionConstantsDictionary.count) return fn4_note(f, lib, name, nil);
    os_unfair_lock_lock(&g_fn_dev_lock);
    if (!g_fn_dev) g_fn_dev = [[NSMapTable alloc] initWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory capacity:0];
    if (![g_fn_dev objectForKey:f]) [g_fn_dev setObject:[FnDev new] forKey:f];
    os_unfair_lock_unlock(&g_fn_dev_lock);
    return fn4_note(f, lib, name, cv);
}
// The function an MTLDevice pipeline is built from: f specialized through its library (once) when fn_spec left it unspecialized, else f itself
static id<MTLFunction> fn_dev(id<MTLFunction> f) {
    if (!f) return f;
    os_unfair_lock_lock(&g_fn_dev_lock); FnDev *dev = [g_fn_dev objectForKey:f]; os_unfair_lock_unlock(&g_fn_dev_lock);
    if (!dev) return f;
    // Only callers of this function wait for its compilation; the map lock never covers a library/compiler call.
    @synchronized(dev) {
        if (dev->fn) return dev->fn;
        MTL4SpecializedFunctionDescriptor *sd = (MTL4SpecializedFunctionDescriptor *)fn4_of(f); MTL4LibraryFunctionDescriptor *ld = (MTL4LibraryFunctionDescriptor *)sd.functionDescriptor;
        id<MTLLibrary> lib = ld.library; NSString *name = ld.name; MTLFunctionConstantValues *cv = sd.constantValues;
        if (!lib || !name || !cv) return f;   // fn_spec notes all three
        NSError *e = nil; dev->fn = fn_lib_spec(lib, name, cv, &e);
        if (!dev->fn) { MLOG("[RED] function %s: specializing its function constants for an MTLDevice pipeline failed: %s", f.name.UTF8String, e.localizedDescription.UTF8String); return f; }
        return dev->fn;
    }
}
// A pipeline without the border corrections (dmsh_rewrite.py BORDER_LIVE_FC, DMSH_BORDER_FC): the second specialization of a function already in use. Metal hands out one MTLFunction
// per function content, device-wide: newFunctionWithName: on the same library, or on another library loaded from the same data, returns the same object (macOS 27.2, M4), and the host
// keys a function's Metal 4 descriptor and its MTLDevice specialization by that object (fn4_note, fn_dev). So the variant is not a function object of its own: whoever builds a pipeline
// says which one it builds (off: pso_build, cps_new_b, the mesh pipelines, from pso_key.border), and takes the descriptor from fn4_of_b and the device function from fn_dev_b.
// A descriptor always holds the shader's own function. fn_off keeps every prior constant of f's descriptor (alpha test, logic operation, merged outputs) and adds the border one;
// cached by that descriptor. nil: f has no variant (no library descriptor, or its entry point does not declare the constant), and off builds what on builds
@interface FnOff : NSObject { @public MTL4FunctionDescriptor *d4; MTLFunctionConstantValues *cv; id<MTLFunction> dev; int none, dev_failed; } @end
@implementation FnOff @end
// off is a bitmask: FN_OFF_BORDER (border corrections off, DMSH_BORDER_FC) and FN_OFF_BLEND (blend fix-up off, DMSH_BLEND_OFF_FC, for a pipeline whose blend words are both 0),
// and for a kernel in two passes (dmsh_ndsr.py) FN_SPLIT0 / FN_SPLIT1, its DMSH_SPLIT_FC false or true (split_dispatch).
// One FnOff per descriptor and mask; a mask whose constants f's entry point does not declare is none (it builds as on)
enum { FN_OFF_BORDER = 1, FN_OFF_BLEND = 2, FN_SPLIT0 = 4, FN_SPLIT1 = 8, FN_MASKS = 16 };
static int fc_index(NSDictionary *fc, NSUInteger index) {   // the entry point declares a function constant at that index (by its index, whatever the MSL calls it)
    for (MTLFunctionConstant *c in fc.allValues) if (c.index == index) return 1;
    return 0;
}
@interface FnOffSet : NSObject { @public FnOff *m[FN_MASKS]; } @end
@implementation FnOffSet @end
static os_unfair_lock g_fn_off_lock = OS_UNFAIR_LOCK_INIT;
static NSMapTable *g_fn_off;   // MTL4FunctionDescriptor (fn4_of) → FnOffSet
static FnOff *fn_off(id<MTLFunction> f, int mask) {
    mask &= FN_MASKS - 1; if (!mask || (mask & FN_SPLIT0 && mask & FN_SPLIT1)) return nil;
    MTL4FunctionDescriptor *fd = fn4_of(f);
    if (!fd) return nil;   // no library to specialize from (diagnostic overrides)
    os_unfair_lock_lock(&g_fn_off_lock);
    if (!g_fn_off) g_fn_off = [[NSMapTable alloc] initWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory capacity:0];
    FnOffSet *set = [g_fn_off objectForKey:fd];
    if (!set) { set = [FnOffSet new]; [g_fn_off setObject:set forKey:fd]; }
    FnOff *o = set->m[mask]; if (!o) o = set->m[mask] = [FnOff new];
    os_unfair_lock_unlock(&g_fn_off_lock);
    @synchronized(o) {
        if (o->d4 || o->none) return o->none ? nil : o;
        MTLFunctionConstantValues *cv = nil;
        if ([fd isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) {
            MTL4SpecializedFunctionDescriptor *sd = (MTL4SpecializedFunctionDescriptor *)fd;
            cv = [sd.constantValues copy]; fd = sd.functionDescriptor;
        }
        MTL4LibraryFunctionDescriptor *ld = [fd isKindOfClass:MTL4LibraryFunctionDescriptor.class] ? (MTL4LibraryFunctionDescriptor *)fd : nil;
        // NOLINTNEXTLINE(clang-analyzer-nullability.NullablePassedToNonnull): the descriptor's name is set when it is made (fn4_note refuses a nil name, pre_fn takes the name its function was made from) and never changed
        NSDictionary *fc = ld ? [ld.library newFunctionWithName:ld.name].functionConstantsDictionary : nil;   // the entry's, not f's: a specialized f lists no constants
        int border = (mask & FN_OFF_BORDER) && fc[@"dm_border_off"], blend = (mask & FN_OFF_BLEND) && fc[@"dm_blend_off"], split = (mask & (FN_SPLIT0 | FN_SPLIT1)) && fc_index(fc, DMSH_SPLIT_FC);
        if (!border && !blend && !split) { o->none = 1; return nil; }
        if (!cv) cv = [MTLFunctionConstantValues new];
        BOOL yes = YES; if (border) [cv setConstantValue:&yes type:MTLDataTypeBool atIndex:DMSH_BORDER_FC]; if (blend) [cv setConstantValue:&yes type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC];
        if (split) { BOOL p1 = (mask & FN_SPLIT1) != 0; [cv setConstantValue:&p1 type:MTLDataTypeBool atIndex:DMSH_SPLIT_FC]; }
        MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = cv;
        o->cv = cv; o->d4 = sd;
        return o;
    }
}
// f's Metal 4 descriptor, specialized per FN_OFF_* mask
static MTL4FunctionDescriptor *fn4_of_b(id<MTLFunction> f, int off) { FnOff *o = fn_off(f, off); return o ? o->d4 : fn4_of(f); }
// The function an MTLDevice pipeline is built from (fn_dev), specialized per FN_OFF_* mask through its library, once. A failed specialization keeps the fix-ups
static id<MTLFunction> fn_dev_b(id<MTLFunction> f, int off) {
    FnOff *o = fn_off(f, off);
    if (!o) return fn_dev(f);
    @synchronized(o) {
        if (!o->dev && !o->dev_failed) {
            MTL4LibraryFunctionDescriptor *ld = (MTL4LibraryFunctionDescriptor *)((MTL4SpecializedFunctionDescriptor *)o->d4).functionDescriptor;
            // NOLINTNEXTLINE(clang-analyzer-nullability.NullablePassedToNonnull): fn_off made o->d4 from this ld after looking its name up; names are never nil (fn4_note, pre_fn)
            NSError *e = nil; o->dev = dev_fn(ld.library, ld.name, o->cv, &e);
            if (!o->dev) { o->dev_failed = 1; MLOG("[RED] function %s: specializing it (mask %d: 1 border corrections off, 2 blend fix-up off, 4 / 8 a split's pass 0 / 1) failed: %s (its pipelines are the unspecialized one's)", f.name.UTF8String, off, e.localizedDescription.UTF8String); }
        }
        if (o->dev) return o->dev;
    }
    return fn_dev(f);
}
// A vertex shader's entry point in one of its side libraries: side is the reflection's "pull" (<key>_bp, dmsh_pull.py) or "vin" (<key>_bv, dmsh_vin.py), which names the entry point
// under key (entry, entry_z, entry_wl, entry_z_wl). A reflection that names none there means own, the name that entry point has by the shader's own (today both libraries repeat the
// shader's names, main0 / main0_z; the reflection decides, so a library may name its entry points otherwise). shader_load, pre_desc and pso_offline.py's vertex_entry take it alike
static NSString *side_entry(id side, NSString *key, NSString *own) {
    id n = [side isKindOfClass:[NSDictionary class]] ? ((NSDictionary *)side)[key] : nil;
    return [n isKindOfClass:[NSString class]] && [n length] ? n : own;
}
static NvShader *shader_load(NSString *pc, int stage) {
    NSError *e = nil;
    NSData *jd = msl_data([NSString stringWithFormat:@"%@/%@.json", g_msl_dir, pc]);
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    NSString *lp = [NSString stringWithFormat:@"%@/%@.metallib", g_msl_dir, pc];
    id<MTLLibrary> lib = lib_from_file(lp, &e);
    if (!lib || !j) { MLOG("[RED] failed to load shader %s: %s", pc.UTF8String, e ? e.localizedDescription.UTF8String : "no reflection json"); return nil; }
    if (![j[@"chain"] isEqual:@DMSH_CHAIN]) { MLOG("[RED] shader %s has translation chain version %s, but this host expects %s", pc.UTF8String, [[j[@"chain"] description] UTF8String], DMSH_CHAIN); return nil; }
    NvShader *o = [NvShader new];
    char err[160];
    if (nvn_refl_from_json(j, &o->rf, err, sizeof err)) { MLOG("[RED] shader %s reflection conversion failed: %s", pc.UTF8String, err); return nil; }
    if (o->rf.dyn && !DM_GAME_TEX_DYN_SLOTS) {   // dynamic texture handles (TEXD / SAMPD) only where the profile says the game picks them from c2's tables, as before otherwise
        // (rf.c1 is freed by dealloc when o goes: freeing it here too was a double free, which aborted FINAL FANTASY RESONANCE at its first such shader)
        MLOG("[RED] shader %s reflection conversion failed: a texture handle the translator could not trace to one slot (profile key tex_dyn_slots off)", pc.UTF8String); return nil; }
    NSString *entry = j[@"entry"]; int ez = 0;   // ez: a vertex shader's entry is its near = 0 one
    if (stage == 0) {
        atomic_fetch_add(&g_vs_loaded, 1);
        if (atomic_load(&g_depth_zero)) { if (j[@"entry_z"]) { entry = j[@"entry_z"]; ez = 1; } else RED_ONCE(85, "vertex shader %s has no near=0 entry (entry_z): depth stays in the GL convention", pc.UTF8String); }
    }
    NSDictionary *mst = [j[@"mesh_stage"] isKindOfClass:[NSDictionary class]] ? j[@"mesh_stage"] : nil;
    if (mst) { o->mesh_kind = [mst[@"kind"] isEqual:@"ts"] ? 1 : 2; o->mesh_threads = (uint16_t)[mst[@"threads"] unsignedIntValue]; o->mesh_payload = (uint16_t)[mst[@"payload"] unsignedIntValue];
               o->mesh_notask = o->mesh_kind == 2 && ![mst[@"task"] boolValue];
               if (o->mesh_kind == 2 && atomic_load(&g_depth_zero) && j[@"entry_z"]) entry = j[@"entry_z"]; }   // the depth convention, as a vertex shader takes it above
    o->fn = fn4_note([lib newFunctionWithName:entry], lib, entry, nil); o->stage = stage; strlcpy(o->key, pc.UTF8String, sizeof o->key); o->ventry = entry;
    if (stage == 0) {   // the same entry point in the side libraries <key>_bp and <key>_bv (pull_load, vin_fn), by the names the reflection gives them
        o->pentry = side_entry(j[@"pull"], ez ? @"entry_z" : @"entry", entry);
        o->wlentry = side_entry(j[@"pull"], ez ? @"entry_z_wl" : @"entry_wl", [entry stringByAppendingString:@"_wl"]);
        o->vinentry = side_entry(j[@"vin"], ez ? @"entry_z" : @"entry", entry);
    }
    if (o->mesh_notask && !(o->mesh_obj = j[@"entry_object"] ? fn4_note([lib newFunctionWithName:j[@"entry_object"]], lib, j[@"entry_object"], nil) : nil))
        MLOG("[RED] mesh shader %s: no object entry point (entry_object) in its library: its draws are skipped", pc.UTF8String);
    if (stage == 0) o->has_pull = [j[@"pull"] isKindOfClass:[NSDictionary class]];
    if (stage == 1) o->has_alpha = [j[@"alpha"] isKindOfClass:[NSDictionary class]];
    if (stage == 1) o->has_bz = [j[@"blend_zero"] boolValue];
    if (stage == 1) o->has_logic = [j[@"logic"] isKindOfClass:[NSDictionary class]];
    // A function that declares function constants cannot go into a pipeline until it is specialized, even when every constant is optional. SPIRV-Cross declares one for shaders that read a
    // texel buffer (spvLinearTextureAlignmentOverride, used only through is_function_constant_defined; one of Pokémon Legends: Z-A's imgui pixel shaders, 2bd5037003980716): an empty set of
    // values keeps its default. Without it the first pipeline with that shader failed Metal's validation (`fragmentFunction main0 cannot be used to build a pipeline state`)
    if (j[@"entry_discard"]) {
        o->discard_fn = fn4_note([lib newFunctionWithName:j[@"entry_discard"]], lib, j[@"entry_discard"], nil);
        if (o->discard_fn.functionConstantsDictionary.count) {
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; NSError *error = nil;
            o->discard_fn = fn_spec(lib, j[@"entry_discard"], cv, &error);
        }
        if (!o->discard_fn) MLOG("[RED] shader %s: no rasterizer-discard entry point %s in its library", pc.UTF8String, [j[@"entry_discard"] UTF8String]);
    }
    if (stage == 1 && [j[@"entry_u"] isKindOfClass:NSString.class]) { o->u_name = j[@"entry_u"]; o->u_side = [j[@"entry_u_side"] boolValue];   // in the side library: looked up when a pipeline takes it (typed_fn)
        o->u_alpha = [j[@"entry_u_alpha"] boolValue]; o->u_logic = [j[@"entry_u_logic"] boolValue];
        if (!o->u_side) { o->ulib = lib; o->ulib_state = 1; if (![lib newFunctionWithName:o->u_name]) { MLOG("[RED] shader %s: no typed-output entry point %s in its library", pc.UTF8String, o->u_name.UTF8String); o->u_name = nil; } } }
    if (stage == 1 && j[@"entry_d"]) { o->fn_d = fn4_note([lib newFunctionWithName:j[@"entry_d"]], lib, j[@"entry_d"], nil); if (!o->fn_d) MLOG("[RED] shader %s: no dual-source entry point %s in its library", pc.UTF8String, [j[@"entry_d"] UTF8String]); }
    if (o->fn.functionConstantsDictionary.count) {   // a compute shader's pipeline is looked up by this specialized descriptor too (cps_new_b); the MTLDevice builds it from fn_dev's
        // Set dm_blend_off = false explicitly when declared so the shader's base descriptor and FN_OFF_BORDER match the offline pair archive (pre_fn, pso_offline.py)
        NSError *fe = nil; MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        if (o->fn.functionConstantsDictionary[@"dm_blend_off"]) { BOOL no = NO; [cv setConstantValue:&no type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC]; }
        id<MTLFunction> f = fn_spec(lib, entry, cv, &fe);
        if (f) o->fn = f; else MLOG("[RED] shader %s: specializing its function constants failed: %s", pc.UTF8String, fe.localizedDescription.UTF8String);
        if (o->fn_d) {
            MTLFunctionConstantValues *dcv = [MTLFunctionConstantValues new];
            if (o->fn_d.functionConstantsDictionary[@"dm_blend_off"]) { BOOL no = NO; [dcv setConstantValue:&no type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC]; }
            o->fn_d = fn_spec(lib, j[@"entry_d"], dcv, &fe);
        }
    }
    if (o->rf.nb) {
        o->ae = [o->fn newArgumentEncoderWithBufferIndex:o->rf.abi];
        int max_aid = -1;
        for (int i = 0; i < o->rf.nb; i++) max_aid = MAX(max_aid, o->rf.b[i].aid + (o->rf.b[i].n ? o->rf.b[i].n - 1 : 0));   // an array takes n entries
        o->arg_len = o->ae.encodedLength;
        if (o->arg_len != (NSUInteger)(8 * (max_aid + 1)) || (o->arg_len & 7) || max_aid >= NVN_REFL_MAXARG)
            MDIE("shader %s has an unexpected argument buffer layout: encodedLength=%lu maxAid=%d", o->key, (unsigned long)o->arg_len, max_aid);
    }
    if (o->rf.has_c1) c1_store(o);
    NSString *ext = g_cb_ext[[pc hasSuffix:@"_b"] ? [pc substringToIndex:pc.length - 2] : pc];   // bytes each uniform slot can read (cb_len), storage slots never written (sb_ro)
    for (NSString *f in [ext componentsSeparatedByString:@" "]) { int r = 0, n = 0;
        if (sscanf(f.UTF8String, "%d:%d", &r, &n) == 2 && r >= 0 && r < 16 && n > 0) o->cb_ext[r] = (uint32_t)n;
        else if (sscanf(f.UTF8String, "s%d", &r) == 1 && r >= 0 && r < 32) o->sb_ro |= 1u << r; }
    return o;
}
// The part of uniform slot reg's binding the shader can read: its first cb_ext bytes when cb_extent.tsv bounds them, else the whole binding
static inline uint64_t cb_len(NvShader *sh, int reg, uint64_t size) { uint32_t e = sh && reg >= 0 && reg < 16 ? sh->cb_ext[reg] : 0; return e && e < size ? e : size; }
static int stage_of(NSString *stage) {
    if ([stage isEqualToString:@"vertex"]) return 0; if ([stage isEqualToString:@"pixel"]) return 1; if ([stage isEqualToString:@"compute"]) return 5;
    if ([stage isEqualToString:@"task"]) return 3; if ([stage isEqualToString:@"mesh"]) return 4;   // NVN2 (hle_nvn.c st_host). A geometry, hull or domain shader has no stage here: the host has no geometry stage and no tessellation
    return -1;
}
// Libraries loaded or being loaded, from any thread: the prefetch (mtl_prog_prefetch) takes a library only the first time its name comes here
static os_unfair_lock g_progpf_lock = OS_UNFAIR_LOCK_INIT; static NSMutableSet<NSString *> *g_progpf_seen;
static int progpf_first(NSString *pc) {
    os_unfair_lock_lock(&g_progpf_lock); if (!g_progpf_seen) g_progpf_seen = [NSMutableSet set];
    int fresh = ![g_progpf_seen containsObject:pc]; if (fresh) [g_progpf_seen addObject:pc];
    os_unfair_lock_unlock(&g_progpf_lock); return fresh;
}
static NvShader *shader_get(NSString *pc, int stage) {
    if (!g_shcache) g_shcache = [NSMutableDictionary dictionary];
    NvShader *o = g_shcache[pc];
    if (o) return o;
    uint64_t bt = bp_now(); o = shader_load(pc, stage);
    if (!o) return nil;
    progpf_first(pc);
    g_shcache[pc] = o;
    pso_record("S\t%s\t%d\n", pc.UTF8String, stage);
    pso_user_record("S\t%s\t%d\n", pc.UTF8String, stage);
    g_bp_lib_n++; g_bp_lib_ns += bp_now() - bt;
    return o;
}
static void up_mfx_read_color(NvShader *cs, const NVNshaderData *sd);
static NvProg *prog_get(const prog_t *cp) {   // Called only on the submit thread
    prog_t *p = (prog_t *)cp;
    if (!p) return nil;   // no program bound: nil, as the callers that test S.prog first get (draw_'s passdump line tests it, the lines after it did not)
    static const prog_t *lp; static uint64_t lser, lgen; static NvProg *lpr;   // Last result: same object, same serial and unchanged serial table means it is used directly
    uint64_t gen = g_ser_gen;
    if (p == lp && p->serial == lser && gen == lgen && lpr) return lpr;
    __unsafe_unretained NvProg *have = p->serial ? serial_get(p->serial) : nil;
    if (have) { lp = p; lser = p->serial; lgen = gen; lpr = have; return have; }
    shmap_load();
    NvProg *o = [NvProg new];
    for (unsigned i = 0; i < p->nstage; i++) {
        // Key = sha256(first 0x48 bytes of the NVN2 control section, +0x30 as zero, + code area): many shaders share a control section but differ in code (dm/gfx/tools/shader_split.py). It was computed at SetShaders (hle_nvn.c): by now the game may have freed the control section
        uint8_t md[8]; if (!prog_key(p, i, md)) MDIE("program stage %u has no shader key", i);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *stage = g_shmap[k];
        if (!stage) {   // not one of the game's shaders
            MLOG("[RED] shader %s is not in the shader map (DM_NVN_SHDUMP=directory saves its control section and code area)", k.UTF8String);
            // DM_NVN_SHDUMP=directory: shaders missing from the map are saved as <key>.ctl (the control section's key bytes) and <key>.code (code area, nvn_ctl_lens), to trace their source / add them to the map offline
            static const char *shd; static int shd_init; if (!shd_init) { shd = getenv("DM_NVN_SHDUMP"); shd_init = 1; if (shd && *shd) mkdir(shd, 0777); }   // create the dump folder: a missing one made every fopen fail silently
            if (shd) { char path[1024]; FILE *f; uint32_t ncode, nctl; nvn_ctl_lens(p->sd[i].control, &nctl, &ncode);   // with SHDUMP set the control section is the host's copy
                snprintf(path, sizeof path, "%s/%s.ctl", shd, k.UTF8String); if ((f = fopen(path, "wb"))) { fwrite(p->sd[i].control, 1, nctl, f); fclose(f); }
                snprintf(path, sizeof path, "%s/%s.code", shd, k.UTF8String);
                if (p->sd[i].data && ncode && ncode < (1u << 24) && (f = fopen(path, "wb"))) { fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f); } }
            continue;
        }
        int st = stage_of(stage);
        if (st < 0) { MLOG("[RED] shader %s is a %s shader: the host has no such stage (no geometry stage, no tessellation: no shader of the game needs them), and its program draws without it", k.UTF8String, stage.UTF8String); continue; }
        NvShader *sh = shader_get([k stringByAppendingString:@"_b"], st);
        if (sh) o->sh[st] = sh; else MLOG("[RED] shader %s (%s) failed to load: build the shaders with dm/gfx/tools/dmsh_build.py first", k.UTF8String, stage.UTF8String);
        if (sh && st == 5) up_mfx_read_color(sh, &p->sd[i]);
    }
    if (p->serial) serial_put(p->serial, o);
    return o;
}
// A submitted descriptor and a queue binding pin the program by its serial (own_prog_pin; mtl_prog_release and mtl_prog_unpin are nvn_owner.m's)
void mtl_prog_snapshot(prog_t *copy,const prog_t *source) {
    prog_get(source); *copy=*source;
    if(!source->serial) MDIE("cannot retain a program without an initialization serial");
    own_prog_pin(source->serial);
}
