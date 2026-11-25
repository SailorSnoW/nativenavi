// Shaders: the shader map, c1 banks, the user manifest, library loading, depth-mode entry points, Metal 4 function descriptors and specializations, shader_load, programs.
// Unfair lock: lends a waiting submitter's priority to the owner (docs/reference/frame-rate.md, Registry lock priority inheritance).
static os_unfair_lock g_rmu = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSString *, NSString *> *g_shmap;
static NSString *g_shmap_path, *g_msl_dir;
// The folder is looked in before the image b-<chain>.img, so files a partial dmsh_build.py run rebuilt override it.
static _Atomic(romfs_img *) g_msl_img;
static pthread_mutex_t g_msl_img_lock = PTHREAD_MUTEX_INITIALIZER;
static const img_ent *msl_image_file(NSString *path, romfs_img **img) {
    if (!g_msl_dir || path.length <= g_msl_dir.length || ![path hasPrefix:g_msl_dir]) return NULL;   // a .metallib from elsewhere (the tests)
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
static NSData *msl_data(NSString *path) {
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
static void shmap_read(void);
static void warm_start(void);
// read once from any thread (mtl_prog_prefetch runs on the game's threads)
static void shmap_read_once(void) { static dispatch_once_t once; dispatch_once(&once, ^{ shmap_read(); }); }
// The flag is set before warm_start, which calls back in: a dispatch_once re-entered aborts.
static void shmap_load(void) {
    shmap_read_once();
    static int warmed;
    if (!warmed) {
        warmed = 1;
        warm_start();
    }
}
// c1_full.tsv / cb_extent.tsv header: "# chain b-<V> inputs <digest>", or that and a space (older tables, with the time they were made)
static BOOL table_head_ok(NSString *head, NSString *want) {
    if (!head) return NO;
    NSString *h = [head hasSuffix:@"\r"] ? [head substringToIndex:head.length - 1] : head;
    return [h isEqualToString:want] || [h hasPrefix:[want stringByAppendingString:@" "]];
}
static void shmap_read(void) {
    if (g_shmap) return;
    const char *e = getenv("DM_NVN_SHMAP");
    NSString *path = e ? @(e) : exe_rel([NSString stringWithFormat:@"../games/%s/shader_map.tsv", DM_GAME_ID].UTF8String);
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
    // a host takes only the libraries of the chain it was built with
    const char *root = getenv("DM_ROOT");
    NSString *cache = m ? @(m) : [NSString stringWithFormat:@"%s%s/build/cache/metal_shader_cache", root ?: getenv("HOME"), root ? "" : "/dm-work"];
    g_msl_dir = [NSString stringWithFormat:@"%@/b-%s", cache, DMSH_CHAIN];
    if (![NSFileManager.defaultManager fileExistsAtPath:g_msl_dir]) {
        NSString *image = [g_msl_dir stringByAppendingString:@".img"];
        if ([NSFileManager.defaultManager fileExistsAtPath:image]) MLOG("shader libraries: one file, %s", image.UTF8String);
        else MLOG("[RED] shader library %s (a folder or a .img file) does not exist: translation chain version %s of this source tree has not been built yet (dm/gfx/tools/dmsh_build.py)", g_msl_dir.UTF8String, DMSH_CHAIN);
    }
    // The stamp ties c1_full.tsv and cb_extent.tsv to these libraries: the chain version does not follow the profile's tables.
    NSData *stamp_data = msl_data([NSString stringWithFormat:@"%@/inputs.tsv", g_msl_dir]);
    NSString *stamp = stamp_data ? [[NSString alloc] initWithData:stamp_data encoding:NSUTF8StringEncoding] : nil, *st_game = nil, *st_inputs = nil;
    for (NSString *ln in [stamp componentsSeparatedByString:@"\n"]) {
        NSArray *c = [ln componentsSeparatedByString:@"\t"];
        if (c.count != 2) continue;
        if ([c[0] isEqual:@"game"]) st_game = c[1];
        else if ([c[0] isEqual:@"inputs"]) st_inputs = c[1];
    }
    if (!st_inputs) MLOG("[RED] shader library %s has no stamp (inputs.tsv): no full dmsh_build.py run made it as it is, so c1_full.tsv and cb_extent.tsv are not taken", g_msl_dir.UTF8String);
    else if (st_game && ![st_game isEqual:@(DM_GAME_ID)]) MLOG("[RED] shader library %s was built for game %s, not %s", g_msl_dir.UTF8String, st_game.UTF8String, DM_GAME_ID);
    else MLOG("shader library stamp: game %s, inputs %s", st_game.UTF8String, st_inputs.UTF8String);
    NSString *cp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"c1_full.tsv"];
    NSString *ct = cp ? [NSString stringWithContentsOfFile:cp encoding:NSUTF8StringEncoding error:nil] : nil;
    NSString *head = ct ? [[ct componentsSeparatedByString:@"\n"] firstObject] : nil;
    NSString *want = [NSString stringWithFormat:@"# chain b-%s inputs %@", DMSH_CHAIN, st_inputs ?: @"-"];   // a library without a stamp matches no table
    if (!table_head_ok(head, want)) {
        MLOG("[RED] c1_full.tsv is stale or missing (its header must name this library's chain and inputs; a full dmsh_build.py run makes it); every c1 gets its own 64 KiB");
    } else {
        g_c1_full_valid = 1; g_c1_full = [NSMutableSet set];
        for (NSString *line in [ct componentsSeparatedByString:@"\n"])
            if (line.length && ![line hasPrefix:@"#"]) [g_c1_full addObject:[line stringByTrimmingCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]];
    }
    {   // without cb_extent.tsv a uniform block updated in part inside a pass is copied whole
        NSString *xp = [[path stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"cb_extent.tsv"];
        NSString *xt = [NSString stringWithContentsOfFile:xp encoding:NSUTF8StringEncoding error:nil];
        NSString *xh = xt ? [[xt componentsSeparatedByString:@"\n"] firstObject] : nil;
        if (!table_head_ok(xh, want)) MLOG("cb_extent.tsv is stale or missing: uniform reads are taken to reach the whole binding");
        else {
            NSMutableDictionary *d = [NSMutableDictionary dictionary];
            for (NSString *line in [xt componentsSeparatedByString:@"\n"]) {
                if (!line.length || [line hasPrefix:@"#"]) continue;
                NSRange t = [line rangeOfString:@"\t"];
                if (t.location != NSNotFound) d[[line substringToIndex:t.location]] = [line substringFromIndex:t.location + 1];
            }
            g_cb_ext = d;
            MLOG("uniform read extents: %lu shaders (cb_extent.tsv)", (unsigned long)d.count);
        }
    }
    MLOG("shader route B (translated Switch originals): shader map %lu entries (%s), metallib directory %s", (unsigned long)g_shmap.count, path.UTF8String, g_msl_dir.UTF8String);
}

// A *_state is 0 untried, 1 loaded, 2 failed
@interface NvShader : NSObject { @public
    id<MTLFunction> fn, discard_fn, fn_d;   // fn_d: the dual-source entry point
    // u_name: the typed-output entry point, in <key>_bx when u_side; ufn: it specialized per pipeline (typed_fn)
    NSString *u_name; id<MTLLibrary> ulib; NSMutableDictionary *ufn; int u_side, ulib_state, u_alpha, u_logic;
    id<MTLLibrary> xlib; int xlib_state;
    uint32_t cb_ext[16], sb_ro; nvn_refl rf; id<MTLArgumentEncoder> ae; NSUInteger arg_len; int stage; uint64_t logged; char key[24];
    id<MTLBuffer> c1buf; uint64_t c1_gpu; size_t c1off;
    uint8_t mesh_kind, mesh_notask; uint16_t mesh_threads, mesh_payload;   // task (1) or mesh (2)
    id<MTLFunction> mesh_obj;   // no task: the object function launching its workgroups in blocks of DMSH_MESH_GRID
    int cps_failed;
    int split_failed;
    uint64_t loop_sub, loop_gpu; _Atomic int loop_reported;
    uint8_t up_mfx;   // 0 not decided, 1 drawn by MetalFX when DM_UPSCALER=metalfx, 2 not one or one its colour keeps
    uint8_t up_color;
    uint8_t up_chain;   // 0 not looked up, 1 one of the upscaler chain, 2 not one
    uint8_t tex_n, tex_idx[NVN_REFL_MAXB], buf_n, buf_idx[NVN_REFL_MAXB]; int lists_nb;   // sh_lists_make; lists_nb: the rf.nb they were made for + 1, 0 not made
    uint64_t cbs_seen; uint32_t cbs_runs;
    uint8_t ab_cbuf_only;   // 1: its only uniform-buffer reads are plain CBUF slots, so a block can be patched; 0 not decided, 2 not
    // Borrowed from its arena (a strong reference would pin retired arenas) and reused only within render pass ab_pass: another pass's arena may have been reclaimed.
    __unsafe_unretained id<MTLBuffer> ab_buf; uint64_t ab_gpu; size_t ab_off; uint64_t ab_pass; uint32_t ab_binst; uint64_t ab_pgen, ab_gen, ab_ugen;
    id<MTLLibrary> bflib; int bflib_state; NSMutableArray *bffn;   // bflib: the readable-tile version <key>_bf
    uint64_t mis[4];
    NSString *ventry; id<MTLFunction> vfn; int vfn_state;   // ventry: the entry point loaded (by the depth mode)
    NSString *pentry, *wlentry, *vinentry;   // nil: ventry's name
    id<MTLFunction> pfn, wlfn; int pfn_state, has_pull;
    id<MTLLibrary> alib; id<MTLFunction> afn[2]; int afn_state, has_alpha, has_bz;
    id<MTLLibrary> llib; NSMutableDictionary<NSNumber *, id<MTLFunction>> *lfn; int lfn_state, has_logic;
} @end
@implementation NvShader
- (void)dealloc { free(rf.c1); }
@end
// fx: per sample, -1 none; fn nil when Metal refused it (not asked again)
typedef struct { int8_t fx[32]; uint8_t fbmask, lmap[8]; } bf_key;
@interface NvBfFn : NSObject { @public bf_key k; id<MTLFunction> fn; } @end
@implementation NvBfFn @end
#include "nvn_pso_skey.h"
// sh: by host stage (0 vertex, 1 pixel, 3 task, 4 mesh, 5 compute; 2 stays empty). c_*: + 1 once known (the shaders are fixed once made)
@interface NvProg : NSObject { @public NvShader *sh[6]; NvProg *mesh_raster; int8_t c_sw, c_iw, c_img; } @end
@implementation NvProg @end
// pso's are raw pointers, and the background's dictionary goes with its task
@interface WarmItem : NSObject { @public
    int kind, record, pull, cps_off;
    NSString *key, *vs_key, *fs_key, *ts_key;
    NvShader *shader, *vs, *fs, *ts;
    pso_key pso; id<MTLFunction> fsfn; id object;
} @end
@implementation WarmItem @end
static NSMutableDictionary<NSString *, NvShader *> *g_shcache;

// under g_c1_lock; NULL when it cannot be allocated
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
    NSString *base_key = [NSString stringWithUTF8String:o->key];
    if ([base_key hasSuffix:@"_b"]) base_key = [base_key substringToIndex:base_key.length - 2];
    os_unfair_lock_lock(&g_c1_lock);   // the prewarm and the submit thread both load shaders
    // A dedicated c1 is a slice of a zeroed bank, not an MTLBuffer of its own: thousands of buffers in the residency set make every commit slow.
    int dedicated = !g_c1_full_valid || [g_c1_full containsObject:base_key];
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
static _Atomic int g_warm_running;
static _Atomic int g_warm_bg;   // the prewarm runs beside the game, not before its first frame
static _Atomic int g_warm_stop;
static _Atomic uint64_t g_warm_shaders, g_warm_rps, g_warm_mps, g_warm_cps, g_warm_skip_fz, g_warm_skip_stale;
static id<MTLRenderPipelineState> mtk_build(const pso_key *, NvShader *, NvShader *, NvShader *, NSError **, int *);
static _Atomic uint64_t g_mtk_arch, g_mtk_built, g_mtk_dev;
static uint64_t g_warm_claim_shaders, g_warm_claim_rps, g_warm_claim_cps;
static id<MTLFunction> bf_fn(NvShader *fs, const int *fx, uint8_t fbmask, const uint8_t *lmap);
static FILE *g_pso_record;
static int g_pso_record_checked;
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
// The user manifest: the shaders and pipelines this Mac used, prewarmed at the next start with the seed.
static NSString *pso_user_path(void) {
    const char *u = getenv("DM_PSO_USER"), *save = getenv("DM_SAVE");
    if (u && *u) return @(u);
    return save && *save ? [[@(save) stringByDeletingLastPathComponent] stringByAppendingPathComponent:@"pso_user.tsv"] : nil;
}
static FILE *g_pso_user;
static int g_pso_user_checked;
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
        NSMutableArray<NSString *> *paths = [NSMutableArray array];
        if (seed) [paths addObject:seed];
        NSString *user = pso_user_path();
        if (user) [paths addObject:user];
        for (NSString *path in paths) {
            NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
            for (NSString *old in [txt componentsSeparatedByString:@"\n"]) if (old.length && ![old hasPrefix:@"#"]) [g_pso_user_seen addObject:old];
        }
    }
    size_t ll = strlen(line);
    NSString *s = [[NSString alloc] initWithBytes:line length:ll && line[ll - 1] == '\n' ? ll - 1 : ll encoding:NSUTF8StringEncoding];
    if (!s || [g_pso_user_seen containsObject:s]) return;
    [g_pso_user_seen addObject:s]; fputs(line, f); fflush(f);
}
// The first 16 bytes of the library's SHA-256, not its file name: Metal hands out one MTLFunction per function content, device-wide, so identical libraries share one function.
static char g_lib_tag;
static id<MTLLibrary> lib_tag(id<MTLLibrary> lib, NSString *name) { if (lib && name) objc_setAssociatedObject(lib, &g_lib_tag, name, OBJC_ASSOCIATION_COPY_NONATOMIC); return lib; }
static NSString *lib_digest(const void *p, size_t n) {
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(p, (CC_LONG)n, h);
    char hex[33]; for (int i = 0; i < 16; i++) snprintf(hex + 2 * i, 3, "%02x", h[i]);
    return @(hex);
}
// Mapped and closed at once: newLibraryWithURL keeps its file open for the library's life, and a run holds more libraries than the soft limit of 256 descriptors.
static id<MTLLibrary> lib_from_file(NSString *path, NSError **err) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); struct stat st;
    if (fd < 0) {   // not in the folder: the library's image, whose one descriptor is shared
        int open_errno = errno;
        romfs_img *img;
        const img_ent *x = msl_image_file(path, &img);
        msl_map f;
        if (x && msl_image_map(img, x, &f)) {
            void *base = f.base; size_t base_len = f.base_len;
            dispatch_data_t d = dispatch_data_create(f.data, f.size, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(base, base_len); });
            return lib_tag([g_dev newLibraryWithData:d error:err], lib_digest(f.data, f.size));
        }
        if (!x) errno = open_errno;
    }
    if (fd < 0 || fstat(fd, &st) || st.st_size <= 0) {
        if (fd >= 0) close(fd);
        if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"cannot open %@", path]}];
        return nil;
    }
    size_t len = (size_t)st.st_size;
    void *m = mmap(NULL, len, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (m == MAP_FAILED) {
        if (err) *err = [NSError errorWithDomain:NSPOSIXErrorDomain code:errno userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"mmap failed %@", path]}];
        return nil;
    }
    dispatch_data_t d = dispatch_data_create(m, len, dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ munmap(m, len); });
    return lib_tag([g_dev newLibraryWithData:d error:err], lib_digest(m, len));
}
// NVN depth mode: main0 rewrites z for the GL convention (near = -w, NVN's default), main0_z leaves it (near = 0).
static _Atomic int g_depth_zero, g_vs_loaded;
void mtl_set_depth_zero(int on) {
    if (atomic_exchange(&g_depth_zero, on) == on) return;
    MLOG("depth mode: %s", on ? "near=0, vertex shaders use the main0_z entry" : "near=-w (GL), vertex shaders use the main0 entry");
    int n = atomic_load(&g_vs_loaded);
    if (n) MLOG("[RED] depth mode changed after %d vertex shaders were loaded: they keep the old convention", n);
}
// MTL4Compiler takes a library, an entry name and constant values, not an MTLFunction, so every function made from a library is noted here.
// A function with no note (the diagnostic fragment overrides) keeps the MTLDevice path.
static os_unfair_lock g_fn4_lock = OS_UNFAIR_LOCK_INIT;
static NSMapTable *g_fn4;
static NSMapTable *map_by_identity(void) {   // keys compared by pointer, keys and values held strongly
    return [[NSMapTable alloc] initWithKeyOptions:NSPointerFunctionsStrongMemory | NSPointerFunctionsObjectPointerPersonality valueOptions:NSPointerFunctionsStrongMemory capacity:0];
}
static id<MTLFunction> fn4_note(id<MTLFunction> f, id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv) {
    if (!f || !lib || !name) return f;
    MTL4LibraryFunctionDescriptor *ld = [MTL4LibraryFunctionDescriptor new]; ld.library = lib; ld.name = name;
    MTL4FunctionDescriptor *d = ld;
    if (cv) {
        MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new];
        sd.functionDescriptor = ld; sd.constantValues = cv;
        d = sd;
    }
    os_unfair_lock_lock(&g_fn4_lock);
    if (!g_fn4) g_fn4 = map_by_identity();
    [g_fn4 setObject:d forKey:f];
    os_unfair_lock_unlock(&g_fn4_lock);
    return f;
}
static MTL4FunctionDescriptor *fn4_of(id<MTLFunction> f) {
    if (!f) return nil;
    os_unfair_lock_lock(&g_fn4_lock); MTL4FunctionDescriptor *d = [g_fn4 objectForKey:f]; os_unfair_lock_unlock(&g_fn4_lock);
    return d;
}
// On the Metal 4 path the pipeline descriptor specializes the function: newFunctionWithName:constantValues: would compile it synchronously on the loading thread.
static int pso4_compiler(void);
static os_unfair_lock g_fn_dev_lock = OS_UNFAIR_LOCK_INIT;
@interface FnDev : NSObject { @public id<MTLFunction> fn; } @end
@implementation FnDev @end
static NSMapTable *g_fn_dev;
static id<MTLFunction> fn_lib_spec(id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv, NSError **e) {
    if (!lib || !name || !cv) return nil;
    return fn4_note(dev_fn(lib, name, cv, e), lib, name, cv);
}
static id<MTLFunction> fn_spec(id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv, NSError **e) {
    if (!lib || !name) return nil;
    if (!pso4_compiler()) return fn_lib_spec(lib, name, cv, e);
    id<MTLFunction> f = [lib newFunctionWithName:name];
    if (!f) {
        if (e) *e = [NSError errorWithDomain:@"dm" code:1 userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"no function %@ in the library", name]}];
        return nil;
    }
    if (!f.functionConstantsDictionary.count) return fn4_note(f, lib, name, nil);
    os_unfair_lock_lock(&g_fn_dev_lock);
    if (!g_fn_dev) g_fn_dev = map_by_identity();
    if (![g_fn_dev objectForKey:f]) [g_fn_dev setObject:[FnDev new] forKey:f];
    os_unfair_lock_unlock(&g_fn_dev_lock);
    return fn4_note(f, lib, name, cv);
}
static id<MTLFunction> fn_dev(id<MTLFunction> f) {
    if (!f) return f;
    os_unfair_lock_lock(&g_fn_dev_lock); FnDev *dev = [g_fn_dev objectForKey:f]; os_unfair_lock_unlock(&g_fn_dev_lock);
    if (!dev) return f;
    @synchronized(dev) {   // only this function's callers wait for its compilation: the map lock never covers a compiler call
        if (dev->fn) return dev->fn;
        MTL4SpecializedFunctionDescriptor *sd = (MTL4SpecializedFunctionDescriptor *)fn4_of(f);
        MTL4LibraryFunctionDescriptor *ld = (MTL4LibraryFunctionDescriptor *)sd.functionDescriptor;
        id<MTLLibrary> lib = ld.library; NSString *name = ld.name; MTLFunctionConstantValues *cv = sd.constantValues;
        if (!lib || !name || !cv) return f;
        NSError *e = nil; dev->fn = fn_lib_spec(lib, name, cv, &e);
        if (!dev->fn) {
            MLOG("[RED] function %s: specializing its function constants for an MTLDevice pipeline failed: %s", f.name.UTF8String, e.localizedDescription.UTF8String);
            return f;
        }
        return dev->fn;
    }
}
// Metal hands out one MTLFunction per function content, device-wide, so a variant (no border corrections, etc.) has no function object of its own:
// the pipeline builder names it by mask and takes fn4_of_b / fn_dev_b.
@interface FnOff : NSObject { @public MTL4FunctionDescriptor *d4; MTLFunctionConstantValues *cv; id<MTLFunction> dev; int none, dev_failed; } @end
@implementation FnOff @end
// a mask whose constants f's entry point does not declare is none (it builds as on)
enum { FN_OFF_BORDER = 1, FN_OFF_BLEND = 2, FN_SPLIT0 = 4, FN_SPLIT1 = 8, FN_MASKS = 16 };
static int fc_index(NSDictionary *fc, NSUInteger index) {   // by its index, whatever the MSL calls it
    for (MTLFunctionConstant *c in fc.allValues) if (c.index == index) return 1;
    return 0;
}
@interface FnOffSet : NSObject { @public FnOff *m[FN_MASKS]; } @end
@implementation FnOffSet @end
static os_unfair_lock g_fn_off_lock = OS_UNFAIR_LOCK_INIT;
static NSMapTable *g_fn_off;
static FnOff *fn_off(id<MTLFunction> f, int mask) {
    mask &= FN_MASKS - 1;
    if (!mask || (mask & FN_SPLIT0 && mask & FN_SPLIT1)) return nil;
    MTL4FunctionDescriptor *fd = fn4_of(f);
    if (!fd) return nil;   // no library to specialize from (diagnostic overrides)
    os_unfair_lock_lock(&g_fn_off_lock);
    if (!g_fn_off) g_fn_off = map_by_identity();
    FnOffSet *set = [g_fn_off objectForKey:fd];
    if (!set) {
        set = [FnOffSet new];
        [g_fn_off setObject:set forKey:fd];
    }
    FnOff *o = set->m[mask];
    if (!o) o = set->m[mask] = [FnOff new];
    os_unfair_lock_unlock(&g_fn_off_lock);
    @synchronized(o) {
        if (o->d4 || o->none) return o->none ? nil : o;
        MTLFunctionConstantValues *cv = nil;
        if ([fd isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) {
            MTL4SpecializedFunctionDescriptor *sd = (MTL4SpecializedFunctionDescriptor *)fd;
            cv = [sd.constantValues copy]; fd = sd.functionDescriptor;
        }
        MTL4LibraryFunctionDescriptor *ld = [fd isKindOfClass:MTL4LibraryFunctionDescriptor.class] ? (MTL4LibraryFunctionDescriptor *)fd : nil;
        // NOLINTNEXTLINE(clang-analyzer-nullability.NullablePassedToNonnull): the name is set when the descriptor is made and never changed
        NSDictionary *fc = ld ? [ld.library newFunctionWithName:ld.name].functionConstantsDictionary : nil;   // the entry's, not f's: a specialized f lists no constants
        int border = (mask & FN_OFF_BORDER) && fc[@"dm_border_off"];
        int blend = (mask & FN_OFF_BLEND) && fc[@"dm_blend_off"];
        int split = (mask & (FN_SPLIT0 | FN_SPLIT1)) && fc_index(fc, DMSH_SPLIT_FC);
        if (!border && !blend && !split) { o->none = 1; return nil; }
        if (!cv) cv = [MTLFunctionConstantValues new];
        BOOL yes = YES;
        if (border) [cv setConstantValue:&yes type:MTLDataTypeBool atIndex:DMSH_BORDER_FC];
        if (blend) [cv setConstantValue:&yes type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC];
        if (split) { BOOL p1 = (mask & FN_SPLIT1) != 0; [cv setConstantValue:&p1 type:MTLDataTypeBool atIndex:DMSH_SPLIT_FC]; }
        MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = cv;
        o->cv = cv; o->d4 = sd;
        return o;
    }
}
static MTL4FunctionDescriptor *fn4_of_b(id<MTLFunction> f, int off) { FnOff *o = fn_off(f, off); return o ? o->d4 : fn4_of(f); }
// a failed specialization keeps the fix-ups
static id<MTLFunction> fn_dev_b(id<MTLFunction> f, int off) {
    FnOff *o = fn_off(f, off);
    if (!o) return fn_dev(f);
    @synchronized(o) {
        if (!o->dev && !o->dev_failed) {
            MTL4LibraryFunctionDescriptor *ld = (MTL4LibraryFunctionDescriptor *)((MTL4SpecializedFunctionDescriptor *)o->d4).functionDescriptor;
            // NOLINTNEXTLINE(clang-analyzer-nullability.NullablePassedToNonnull): the name is never nil
            NSError *e = nil; o->dev = dev_fn(ld.library, ld.name, o->cv, &e);
            if (!o->dev) {
                o->dev_failed = 1;
                MLOG("[RED] function %s: specializing it (mask %d: 1 border corrections off, 2 blend fix-up off, 4 / 8 a split's pass 0 / 1) failed: %s (its pipelines are the unspecialized one's)", f.name.UTF8String, off, e.localizedDescription.UTF8String);
            }
        }
        if (o->dev) return o->dev;
    }
    return fn_dev(f);
}
// own: its name in the shader's own library, when the side names none. shader_load, pre_desc and pso_offline.py's vertex_entry take it alike.
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
    if (o->rf.dyn && !DM_GAME_TEX_DYN_SLOTS) {   // dynamic texture handles only where the profile says the game picks them from c2's tables
        MLOG("[RED] shader %s reflection conversion failed: a texture handle the translator could not trace to one slot (profile key tex_dyn_slots off)", pc.UTF8String);
        return nil;   // dealloc frees rf.c1
    }
    NSString *entry = j[@"entry"]; int ez = 0;
    if (stage == 0) {
        atomic_fetch_add(&g_vs_loaded, 1);
        if (atomic_load(&g_depth_zero)) {
            if (j[@"entry_z"]) {
                entry = j[@"entry_z"];
                ez = 1;
            } else RED_ONCE(85, "vertex shader %s has no near=0 entry (entry_z): depth stays in the GL convention", pc.UTF8String);
        }
    }
    NSDictionary *mst = [j[@"mesh_stage"] isKindOfClass:[NSDictionary class]] ? j[@"mesh_stage"] : nil;
    if (mst) {
        o->mesh_kind = [mst[@"kind"] isEqual:@"ts"] ? 1 : 2;
        o->mesh_threads = (uint16_t)[mst[@"threads"] unsignedIntValue];
        o->mesh_payload = (uint16_t)[mst[@"payload"] unsignedIntValue];
        o->mesh_notask = o->mesh_kind == 2 && ![mst[@"task"] boolValue];
        if (o->mesh_kind == 2 && atomic_load(&g_depth_zero) && j[@"entry_z"]) entry = j[@"entry_z"];
    }
    o->fn = fn4_note([lib newFunctionWithName:entry], lib, entry, nil);
    o->stage = stage; strlcpy(o->key, pc.UTF8String, sizeof o->key); o->ventry = entry;
    if (stage == 0) {
        o->pentry = side_entry(j[@"pull"], ez ? @"entry_z" : @"entry", entry);
        o->wlentry = side_entry(j[@"pull"], ez ? @"entry_z_wl" : @"entry_wl", [entry stringByAppendingString:@"_wl"]);
        o->vinentry = side_entry(j[@"vin"], ez ? @"entry_z" : @"entry", entry);
    }
    if (o->mesh_notask && !(o->mesh_obj = j[@"entry_object"] ? fn4_note([lib newFunctionWithName:j[@"entry_object"]], lib, j[@"entry_object"], nil) : nil))
        MLOG("[RED] mesh shader %s: no object entry point (entry_object) in its library: its draws are skipped", pc.UTF8String);
    if (stage == 0) o->has_pull = [j[@"pull"] isKindOfClass:[NSDictionary class]];
    if (stage == 1) {
        o->has_alpha = [j[@"alpha"] isKindOfClass:[NSDictionary class]];
        o->has_bz = [j[@"blend_zero"] boolValue];
        o->has_logic = [j[@"logic"] isKindOfClass:[NSDictionary class]];
    }
    // A function declaring function constants needs specializing before a pipeline takes it, even when every constant is optional: an empty set keeps the defaults.
    if (j[@"entry_discard"]) {
        o->discard_fn = fn4_note([lib newFunctionWithName:j[@"entry_discard"]], lib, j[@"entry_discard"], nil);
        if (o->discard_fn.functionConstantsDictionary.count) {
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; NSError *error = nil;
            o->discard_fn = fn_spec(lib, j[@"entry_discard"], cv, &error);
        }
        if (!o->discard_fn) MLOG("[RED] shader %s: no rasterizer-discard entry point %s in its library", pc.UTF8String, [j[@"entry_discard"] UTF8String]);
    }
    if (stage == 1 && [j[@"entry_u"] isKindOfClass:NSString.class]) {
        o->u_name = j[@"entry_u"];
        o->u_side = [j[@"entry_u_side"] boolValue];
        o->u_alpha = [j[@"entry_u_alpha"] boolValue]; o->u_logic = [j[@"entry_u_logic"] boolValue];
        if (!o->u_side) {
            o->ulib = lib; o->ulib_state = 1;
            if (![lib newFunctionWithName:o->u_name]) {
                MLOG("[RED] shader %s: no typed-output entry point %s in its library", pc.UTF8String, o->u_name.UTF8String);
                o->u_name = nil;
            }
        }
    }
    if (stage == 1 && j[@"entry_d"]) {
        o->fn_d = fn4_note([lib newFunctionWithName:j[@"entry_d"]], lib, j[@"entry_d"], nil);
        if (!o->fn_d) MLOG("[RED] shader %s: no dual-source entry point %s in its library", pc.UTF8String, [j[@"entry_d"] UTF8String]);
    }
    if (o->fn.functionConstantsDictionary.count) {
        // dm_blend_off = false explicitly, so the descriptors match the offline pair archive (pre_fn, pso_offline.py)
        NSError *fe = nil; MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
        if (o->fn.functionConstantsDictionary[@"dm_blend_off"]) { BOOL no = NO; [cv setConstantValue:&no type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC]; }
        id<MTLFunction> f = fn_spec(lib, entry, cv, &fe);
        if (f) o->fn = f;
        else MLOG("[RED] shader %s: specializing its function constants failed: %s", pc.UTF8String, fe.localizedDescription.UTF8String);
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
    NSString *ext = g_cb_ext[[pc hasSuffix:@"_b"] ? [pc substringToIndex:pc.length - 2] : pc];
    for (NSString *f in [ext componentsSeparatedByString:@" "]) {
        int r = 0, n = 0;
        if (sscanf(f.UTF8String, "%d:%d", &r, &n) == 2 && r >= 0 && r < 16 && n > 0) o->cb_ext[r] = (uint32_t)n;
        else if (sscanf(f.UTF8String, "s%d", &r) == 1 && r >= 0 && r < 32) o->sb_ro |= 1u << r;
    }
    return o;
}
static inline uint64_t cb_len(NvShader *sh, int reg, uint64_t size) { uint32_t e = sh && reg >= 0 && reg < 16 ? sh->cb_ext[reg] : 0; return e && e < size ? e : size; }
// -1 for a geometry, hull or domain shader: the host has no geometry stage and no tessellation
static int stage_of(NSString *stage) {
    if ([stage isEqualToString:@"vertex"]) return 0;
    if ([stage isEqualToString:@"pixel"]) return 1;
    if ([stage isEqualToString:@"compute"]) return 5;
    if ([stage isEqualToString:@"task"]) return 3;
    if ([stage isEqualToString:@"mesh"]) return 4;
    return -1;
}
// from any thread: the prefetch takes a library only the first time its name comes here
static os_unfair_lock g_progpf_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableSet<NSString *> *g_progpf_seen;
static int progpf_first(NSString *pc) {
    os_unfair_lock_lock(&g_progpf_lock);
    if (!g_progpf_seen) g_progpf_seen = [NSMutableSet set];
    int fresh = ![g_progpf_seen containsObject:pc];
    if (fresh) [g_progpf_seen addObject:pc];
    os_unfair_lock_unlock(&g_progpf_lock);
    return fresh;
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
// A plain pointer (the serial table and lpr hold the object): a function returning an object hands it over retained and autoreleased, two runtime calls at each
// of the calls a draw makes (docs/reference/frame-rate.md, "Retain and release in every draw's helpers").
#define prog_get(cp) ((__bridge NvProg *)prog_get_ref(cp))
static void *prog_get_ref(const prog_t *cp) {   // the submit thread only
    prog_t *p = (prog_t *)cp;
    if (!p) return NULL;
    static const prog_t *lp;
    static uint64_t lser, lgen;
    static NvProg *lpr;
    uint64_t gen = g_ser_gen;
    if (p == lp && p->serial == lser && gen == lgen && lpr) return (__bridge void *)lpr;
    __unsafe_unretained NvProg *have = p->serial ? serial_get(p->serial) : nil;
    if (have) {
        lp = p; lser = p->serial; lgen = gen; lpr = have;
        return (__bridge void *)have;
    }
    shmap_load();
    NvProg *o = [NvProg new];
    for (unsigned i = 0; i < p->nstage; i++) {
        // Computed at SetShaders: by now the game may have freed the control section.
        uint8_t md[8];
        if (!prog_key(p, i, md)) MDIE("program stage %u has no shader key", i);
        NSString *k = [NSString stringWithFormat:@"%02x%02x%02x%02x%02x%02x%02x%02x", md[0], md[1], md[2], md[3], md[4], md[5], md[6], md[7]];
        NSString *stage = g_shmap[k];
        if (!stage) {
            MLOG("[RED] shader %s is not in the shader map (DM_NVN_SHDUMP=directory saves its control section and code area)", k.UTF8String);
            static const char *shd;
            static int shd_init;
            if (!shd_init) {
                shd = getenv("DM_NVN_SHDUMP");
                shd_init = 1;
                if (shd && *shd) mkdir(shd, 0777);
            }
            if (shd) {
                char path[1024];
                FILE *f;
                uint32_t ncode, nctl;
                nvn_ctl_lens(p->sd[i].control, &nctl, &ncode);   // with SHDUMP set the control section is the host's copy
                snprintf(path, sizeof path, "%s/%s.ctl", shd, k.UTF8String);
                if ((f = fopen(path, "wb"))) { fwrite(p->sd[i].control, 1, nctl, f); fclose(f); }
                snprintf(path, sizeof path, "%s/%s.code", shd, k.UTF8String);
                if (p->sd[i].data && ncode && ncode < (1u << 24) && (f = fopen(path, "wb"))) { fwrite((const void *)(uintptr_t)p->sd[i].data, 1, ncode, f); fclose(f); }
            }
            continue;
        }
        int st = stage_of(stage);
        if (st < 0) { MLOG("[RED] shader %s is a %s shader: the host has no such stage (no geometry stage, no tessellation: no shader of the game needs them), and its program draws without it", k.UTF8String, stage.UTF8String); continue; }
        NvShader *sh = shader_get([k stringByAppendingString:@"_b"], st);
        if (sh) o->sh[st] = sh;
        else MLOG("[RED] shader %s (%s) failed to load: build the shaders with dm/gfx/tools/dmsh_build.py first", k.UTF8String, stage.UTF8String);
        if (sh && st == 5) up_mfx_read_color(sh, &p->sd[i]);
    }
    if (p->serial) { serial_put(p->serial, o); return (__bridge void *)o; }
    return (void *)CFAutorelease(CFBridgingRetain(o));   // no serial: alive until the caller's pool drains, as a returned object was
}
void mtl_prog_snapshot(prog_t *copy,const prog_t *source) {
    (void)prog_get(source); *copy=*source;
    if(!source->serial) MDIE("cannot retain a program without an initialization serial");
    own_prog_pin(source->serial);
}
