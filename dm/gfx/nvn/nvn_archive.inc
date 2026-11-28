// Metal 4 pipelines and the archive of their bases: a base per pipeline with its colour state unspecialized (a base serves every depth / stencil format), each pipeline
// specialized from it; flush periods, the archive's index, bundled and prewarm files, compaction, compiler service deaths.
// The archive (pso_archive/) holds one file per flush, written by a binaries-only serializer (with descriptor capture it writes nothing); a base found in a file is taken from it
// directly (MTL4Archive), not through lookupArchives, which would capture it again. DM_PSO_MTL4=0: the MTLDevice path; DM_PSO_ARCHIVE=off | <folder>.
// newArchiveWithURL reads a file on an external drive whole into memory (docs/reference/memory.md, A pipeline archive on an external drive is read into memory).
static int pso4_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_PSO_MTL4");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_PSO_MTL4=0: render pipelines are built through the MTLDevice (no bases, no pipeline archive)");
        // Metal cannot capture pipelines made from bases, and its capture layer crashes on the archives: a run Metal lets capture GPU frames builds through the MTLDevice
        // (docs/reference/shaders.md, GPU frame captures need pipelines built through the device). MTL_CAPTURE_ENABLED counts only as a non-zero number; asked once.
        if (v) {
            MTLCaptureManager *m = [MTLCaptureManager sharedCaptureManager];
            int trace = [m supportsDestination:MTLCaptureDestinationGPUTraceDocument], tools = [m supportsDestination:MTLCaptureDestinationDeveloperTools];
            if (trace || tools) { v = 0; MLOG("render pipelines through the MTLDevice: Metal may capture GPU frames in this run (%s)", tools ? "Xcode" : "MTL_CAPTURE_ENABLED"); }
            else MLOG("render pipelines from Metal 4 bases and the pipeline archive (Metal cannot capture GPU frames in this run)");
        }
    }
    return v;
}
// what the archive does not capture (specializations, compiles without an archive folder); captured compiles use their flush period's own compiler (ArchEpoch)
static id<MTL4Compiler> g_comp4;
// g_arch4: files that may hold bases with a vertex layout; g_arch4p: files that hold none (p<seq>.mtl4a). A base with a layout is looked up in g_arch4 alone: each miss costs a file lookup.
static NSArray<id<MTL4Archive>> *g_arch4, *g_arch4p; static NSString *g_arch_dir; static int g_arch_seq;
// Compaction (docs/reference/rendering.md, Every start after play captures the whole pipeline archive again): run files by size tier (arch_tier); past PSO_ARCH_COMPACT files
// in a tier, the bases the prewarm takes from it and smaller tiers are captured again from the original functions and the old files removed once written. p<seq> files stay.
enum { PSO_ARCH_COMPACT = 8 };
static int g_compact_enabled;   // immutable after pso4_init; unlike g_compact_files, never cleared by the archive queue
static _Atomic int g_compact_bases; static _Atomic uint64_t g_compact_ns;
static NSArray<NSString *> *g_compact_files; static _Atomic int g_compacted;
static unsigned char *g_compact_state;   // per run file (g_arch4's order): 1 when the compaction replaces it; immutable after pso4_init
// periods: recapturing periods not written yet; closed: the last is handed over; bad: the old files stay; harvests: recaptures queued and in no period yet,
// so the compaction closes only once there are none (a failed late recapture would find its old file removed).
static _Atomic int g_compact_periods, g_compact_closed, g_compact_bad, g_compact_harvests;
// The crash-recovery marker names the OS build and capture method; a failed flush of this method disables compaction on this OS build
// (docs/reference/shaders.md, A compaction flush that crashes, then crashes every start).
static NSString *arch_compact_marker(void) { return [g_arch_dir stringByAppendingPathComponent:@"compacting"]; }
static NSString *arch_os_build(void) { char b[64] = ""; size_t n = sizeof b; sysctlbyname("kern.osversion", b, &n, NULL, 0); return [NSString stringWithFormat:@"os\t%s\n", b]; }
static NSString *arch_compact_stamp(void) { return [arch_os_build() stringByAppendingString:@"capture\toriginal-functions-v1\n"]; }
static int arch_compact_allowed(void) {
    NSString *m = [NSString stringWithContentsOfFile:arch_compact_marker() encoding:NSUTF8StringEncoding error:nil];
    if (!m) return 1;
    if ([m isEqualToString:arch_compact_stamp()]) { MLOG("[RED] pipeline archive: a compaction did not finish on this system build (issue #342): the files are left as they are"); return 0; }
    unlink(arch_compact_marker().fileSystemRepresentation);   // another system build or capture method: try once more
    return 1;
}
// on 1 writes it, 0 removes it once the write returned; -1: the process is ending, so a cut-short write is no Metal crash and no marker is written again
static os_unfair_lock g_mark_lock = OS_UNFAIR_LOCK_INIT; static int g_mark_mine, g_mark_off;
static void arch_mark(int on) {
    os_unfair_lock_lock(&g_mark_lock);
    if (on > 0 && !g_mark_off) { [arch_compact_stamp() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil]; g_mark_mine = 1; }
    else if (on <= 0 && g_mark_mine) { unlink(arch_compact_marker().fileSystemRepresentation); g_mark_mine = 0; }
    if (on < 0) g_mark_off = 1;
    os_unfair_lock_unlock(&g_mark_lock);
}
static dispatch_queue_t g_arch_q; static _Atomic int g_arch_new, g_arch_new_vd, g_arch_sched;
// A flush period: its own compiler and serializer, written once every compile in it has returned and never used again. Workaround: a compile under way across a serializer's
// write crashes in Metal (docs/reference/rendering.md, A pipeline compile crashes while an archive period is written). A compile that never returns holds only its own period's file.
static _Atomic uint64_t g_arch_deaths;   // every unwritten period predating a compiler-service death is untrusted, including ones already handed over
static _Atomic uint64_t g_arch_discarded;   // periods arch_write_file did not write (a compiler death, an unresolved plain compile, a failed write or index)
@interface ArchEpoch : NSObject { @public id<MTL4Compiler> comp; id<MTL4PipelineDataSetSerializer> ser; dispatch_group_t busy; _Atomic int active, n, vd, cs, ms, failed, crashed; int compacting; uint64_t deaths;
    NSMutableData *keys; os_unfair_lock klock; _Atomic int unkeyed; } @end
// vd, cs, ms: bases with a vertex layout, compute, mesh; crashed: the compiler service died under it; deaths: service deaths at creation (older periods are rejected);
// keys: index keys of what it captured (under klock); unkeyed: its index is not complete
@implementation ArchEpoch @end
// The index (docs/reference/rendering.md, Archive lookups asked every file, so a few files made a start compile every base again): <file>.keys beside each file, so a lookup asks one file.
// A key is the SHA-256 (8 bytes) of everything the Metal 4 descriptor sets: a field the host starts setting must go in the key too. 0: no key (an unnamed library's function).
typedef struct { CC_SHA256_CTX c; int ok; } arch_kb;
static void akb_u(arch_kb *k, uint64_t v) { CC_SHA256_Update(&k->c, &v, sizeof v); }
static void akb_s(arch_kb *k, NSString *s) { const char *u = s.UTF8String ?: ""; size_t n = strlen(u); akb_u(k, n); CC_SHA256_Update(&k->c, u, (CC_LONG)n); }
static void akb_size(arch_kb *k, MTLSize s) { akb_u(k, s.width); akb_u(k, s.height); akb_u(k, s.depth); }
static void akb_fn(arch_kb *k, MTL4FunctionDescriptor *f) {
    if (!f) { akb_u(k, 0); return; }
    if ([f isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) {
        // Metal has no getter for constant values, but the set's description lists them by value (macOS 27.2: the same in every process and order)
        MTL4SpecializedFunctionDescriptor *s = (MTL4SpecializedFunctionDescriptor *)f; MTLFunctionConstantValues *cv = s.constantValues;
        NSString *desc = cv.description; NSRange nl = [desc rangeOfString:@"\n"];
        akb_u(k, 2); akb_u(k, cv ? cv.hash : 0); akb_s(k, cv ? (nl.location != NSNotFound ? [desc substringFromIndex:nl.location] : @"?") : @"(none)");
        akb_s(k, s.specializedName); akb_fn(k, s.functionDescriptor); return;
    }
    if ([f isKindOfClass:MTL4LibraryFunctionDescriptor.class]) {
        MTL4LibraryFunctionDescriptor *l = (MTL4LibraryFunctionDescriptor *)f;
        NSString *lib = l.library ? objc_getAssociatedObject(l.library, &g_lib_tag) : nil;
        if (!lib || !l.name) { k->ok = 0; return; }
        akb_u(k, 1); akb_s(k, lib); akb_s(k, l.name); return;
    }
    k->ok = 0;
}
static void akb_linked(arch_kb *k, MTL4StaticLinkingDescriptor *s) { if (s.functionDescriptors.count || s.privateFunctionDescriptors.count || s.groups.count) k->ok = 0; }
static void akb_colors(arch_kb *k, MTL4RenderPipelineColorAttachmentDescriptorArray *ca) {
    for (NSUInteger i = 0; i < 8; i++) {
        MTL4RenderPipelineColorAttachmentDescriptor *a = ca[i];
        akb_u(k, a.pixelFormat); akb_u(k, a.writeMask); akb_u(k, (uint64_t)a.blendingState); akb_u(k, a.sourceRGBBlendFactor); akb_u(k, a.destinationRGBBlendFactor);
        akb_u(k, a.rgbBlendOperation); akb_u(k, a.sourceAlphaBlendFactor); akb_u(k, a.destinationAlphaBlendFactor); akb_u(k, a.alphaBlendOperation);
    }
}
static uint64_t arch_key(MTL4PipelineDescriptor *d) {
    arch_kb k = {.ok = 1}; CC_SHA256_Init(&k.c);
    if ([d isKindOfClass:MTL4RenderPipelineDescriptor.class]) {
        MTL4RenderPipelineDescriptor *r = (MTL4RenderPipelineDescriptor *)d;
        akb_u(&k, 'R'); akb_fn(&k, r.vertexFunctionDescriptor); akb_fn(&k, r.fragmentFunctionDescriptor);
        MTLVertexDescriptor *vd = r.vertexDescriptor; akb_u(&k, vd != nil);
        for (NSUInteger i = 0; vd && i < 31; i++) {
            MTLVertexAttributeDescriptor *a = vd.attributes[i]; MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
            akb_u(&k, a.format); akb_u(&k, a.offset); akb_u(&k, a.bufferIndex); akb_u(&k, l.stride); akb_u(&k, l.stepFunction); akb_u(&k, l.stepRate);
        }
        akb_u(&k, r.rasterSampleCount); akb_u(&k, (uint64_t)r.alphaToCoverageState); akb_u(&k, (uint64_t)r.alphaToOneState); akb_u(&k, r.rasterizationEnabled);
        akb_u(&k, r.maxVertexAmplificationCount); akb_u(&k, r.inputPrimitiveTopology); akb_u(&k, r.supportVertexBinaryLinking); akb_u(&k, r.supportFragmentBinaryLinking);
        akb_u(&k, (uint64_t)r.colorAttachmentMappingState); akb_u(&k, (uint64_t)r.supportIndirectCommandBuffers); akb_colors(&k, r.colorAttachments);
        akb_linked(&k, r.vertexStaticLinkingDescriptor); akb_linked(&k, r.fragmentStaticLinkingDescriptor);
    } else if ([d isKindOfClass:MTL4MeshRenderPipelineDescriptor.class]) {
        MTL4MeshRenderPipelineDescriptor *m = (MTL4MeshRenderPipelineDescriptor *)d;
        akb_u(&k, 'M'); akb_fn(&k, m.objectFunctionDescriptor); akb_fn(&k, m.meshFunctionDescriptor); akb_fn(&k, m.fragmentFunctionDescriptor);
        akb_u(&k, m.maxTotalThreadsPerObjectThreadgroup); akb_u(&k, m.maxTotalThreadsPerMeshThreadgroup); akb_size(&k, m.requiredThreadsPerObjectThreadgroup); akb_size(&k, m.requiredThreadsPerMeshThreadgroup);
        akb_u(&k, m.objectThreadgroupSizeIsMultipleOfThreadExecutionWidth); akb_u(&k, m.meshThreadgroupSizeIsMultipleOfThreadExecutionWidth); akb_u(&k, m.payloadMemoryLength);
        akb_u(&k, m.maxTotalThreadgroupsPerMeshGrid); akb_u(&k, m.rasterSampleCount); akb_u(&k, (uint64_t)m.alphaToCoverageState); akb_u(&k, (uint64_t)m.alphaToOneState);
        akb_u(&k, m.rasterizationEnabled); akb_u(&k, m.maxVertexAmplificationCount); akb_u(&k, m.supportObjectBinaryLinking); akb_u(&k, m.supportMeshBinaryLinking);
        akb_u(&k, m.supportFragmentBinaryLinking); akb_u(&k, (uint64_t)m.colorAttachmentMappingState); akb_u(&k, (uint64_t)m.supportIndirectCommandBuffers); akb_colors(&k, m.colorAttachments);
        akb_linked(&k, m.objectStaticLinkingDescriptor); akb_linked(&k, m.meshStaticLinkingDescriptor); akb_linked(&k, m.fragmentStaticLinkingDescriptor);
    } else if ([d isKindOfClass:MTL4ComputePipelineDescriptor.class]) {
        MTL4ComputePipelineDescriptor *c = (MTL4ComputePipelineDescriptor *)d;
        akb_u(&k, 'C'); akb_fn(&k, c.computeFunctionDescriptor); akb_u(&k, c.threadGroupSizeIsMultipleOfThreadExecutionWidth); akb_u(&k, c.maxTotalThreadsPerThreadgroup);
        akb_size(&k, c.requiredThreadsPerThreadgroup); akb_u(&k, c.supportBinaryLinking); akb_u(&k, (uint64_t)c.supportIndirectCommandBuffers); akb_linked(&k, c.staticLinkingDescriptor);
    } else return 0;
    MTL4PipelineOptions *o = d.options; akb_u(&k, o ? (uint64_t)o.shaderValidation : 0); akb_u(&k, o ? (uint64_t)o.shaderReflection : 0);
    if (!k.ok) return 0;
    uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_Final(h, &k.c);
    uint64_t v; memcpy(&v, h, sizeof v);
    return v ? v : 1;   // 0 means no key
}
// "DMPSOIX2", complete and count (two uint32), then the keys (uint64 each). "DMPSOIX1" named libraries by file, which no lookup computes now: pso4_init removes it.
static NSString *arch_keys_path(NSString *archive) { return [archive.stringByDeletingPathExtension stringByAppendingString:@".keys"]; }
static int arch_keys_write(NSString *path, NSData *keys, int complete) {
    uint32_t head[2] = {complete != 0, (uint32_t)(keys.length / sizeof(uint64_t))};
    NSMutableData *d = [NSMutableData dataWithBytes:"DMPSOIX2" length:8]; [d appendBytes:head length:sizeof head]; if (keys) [d appendData:keys];
    return [d writeToFile:path atomically:YES];
}
static NSData *arch_keys_read(NSString *path, int *complete) {
    NSData *d = [NSData dataWithContentsOfFile:path]; uint32_t head[2];
    if (d.length < 16 || memcmp(d.bytes, "DMPSOIX2", 8)) return nil;
    memcpy(head, (const uint8_t *)d.bytes + 8, sizeof head);
    if (head[0] > 1 || d.length != 16 + (uint64_t)head[1] * sizeof(uint64_t)) return nil;
    *complete = head[0] == 1;
    return [d subdataWithRange:NSMakeRange(16, d.length - 16)];
}
static int arch_keys_old(NSString *path) {
    char m[8]; int fd = open(path.fileSystemRepresentation, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, m, sizeof m); close(fd);
    return n == (ssize_t)sizeof m && !memcmp(m, "DMPSOIX1", 8);
}
// g_arch_ix: key → the newest file naming it, filled before the first frame (pso4_init, then arch_adopt_prebuilt) and read only after (no lock). g_arch_ixf: files with a
// complete index, reached only through it. g_arch_run: every plain run file, in g_compact_files' order. g_arch_opened: the device's files opened this run. g_arch_asks: for the tests.
static nvn_hmap g_arch_ix = {.klen = sizeof(uint64_t)};
static NSArray<id<MTL4Archive>> *g_arch_ixf, *g_arch_run;
static NSMutableSet<NSString *> *g_arch_opened;
static _Atomic uint64_t g_arch_asks;
// Opened files with how many pipelines each gave: arch_sweep moves out those none came from once every wanted pipeline was asked (a chain or library change leaves old keys unreachable).
@interface ArchUse : NSObject { @public NSString *path; __weak id<MTL4Archive> a; int pf, gone; uint64_t hits, keys; } @end
@implementation ArchUse @end
static char g_arch_use_tag; static NSArray<ArchUse *> *g_arch_use;
static _Atomic int g_sweep_warm, g_sweep_pairs;
static void arch_used(id<MTL4Archive> a) { ArchUse *u = objc_getAssociatedObject(a, &g_arch_use_tag); if (u) __atomic_fetch_add(&u->hits, 1, __ATOMIC_RELAXED); }
// Into DM_OBSOLETE's folder for this run (emptied by the packing script's next build), else deleted (the app). Returns 1 when it has left the archive.
static int arch_retire(NSString *p) {
    static NSString *dir; static dispatch_once_t once;
    dispatch_once(&once, ^{
        const char *o = getenv("DM_OBSOLETE"); if (!o || !*o) return;
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char s[32]; strftime(s, sizeof s, "%Y-%m-%d %H%M%S", &tm);
        dir = [@(o) stringByAppendingPathComponent:[NSString stringWithFormat:@"%s %@", s, g_arch_dir.lastPathComponent ?: @"pso_archive"]];
    });
    int ok = 1;
    for (NSString *f in @[p, arch_keys_path(p)]) {
        if (access(f.fileSystemRepresentation, F_OK)) continue;
        if (dir && [NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil] &&
            !rename(f.fileSystemRepresentation, [dir stringByAppendingPathComponent:f.lastPathComponent].fileSystemRepresentation)) continue;
        if (unlink(f.fileSystemRepresentation) && errno != ENOENT) ok = 0;
    }
    return ok;
}
// The file the index names for its key alone, else each file without a complete index. *from_run: from a run file this compaction takes, so the prewarm captures it again.
static int arch_compact_takes(id<MTL4Archive> a);
static id arch_lookup(MTL4PipelineDescriptor *d, int plain, int *from_run) {
    if (from_run) *from_run = 0;
    int compute = [d isKindOfClass:MTL4ComputePipelineDescriptor.class];
    id (^ask)(id<MTL4Archive>) = ^id(id<MTL4Archive> a) {
        atomic_fetch_add_explicit(&g_arch_asks, 1, memory_order_relaxed);
        id ps = compute ? (id)[a newComputePipelineStateWithDescriptor:(MTL4ComputePipelineDescriptor *)d error:nil] : (id)[a newRenderPipelineStateWithDescriptor:d error:nil];
        if (ps) arch_used(a);
        return ps;
    };
    uint64_t k = g_arch_ix.n ? arch_key(d) : 0;
    id<MTL4Archive> named = k ? (__bridge id<MTL4Archive>)nvn_hm_get(&g_arch_ix, &k) : nil;
    if (named) {
        id ps = ask(named);
        if (ps && from_run) *from_run = arch_compact_takes(named);
        return ps;
    }
    if (!plain) for (id<MTL4Archive> a in g_arch4p) { id ps = ask(a); if (ps) return ps; }
    for (id<MTL4Archive> a in g_arch4) { id ps = ask(a); if (ps) { if (from_run) *from_run = arch_compact_takes(a); return ps; } }
    return nil;
}
static ArchEpoch *g_ep; static os_unfair_lock g_ep_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic uint64_t g_ep_t0;
static int g_arch_cap;   // immutable after pso4_init
static dispatch_group_t g_arch_pending;
static _Atomic uint64_t g_pb_built, g_pb_archived, g_pb_reused, g_pb_spec, g_pb_legacy, g_pb_base_ns;
static _Atomic uint64_t g_pt_hit_n, g_pt_hit_ns, g_pt_miss_n, g_pt_miss_ns, g_pt_spec_ns, g_pt_comp_ns, g_pt_pred_n, g_pt_pred_ns, g_pp_done_n;
typedef struct { const void *vf, *ff; uint32_t samples, a2c, rast, topo, amp, off; uint32_t va[31][3], vl[31][3]; } pbase_key;   // what a base depends on: pso_desc's descriptor less the colour attachments; off: without the border corrections
static nvn_hmap g_pbase = {.klen = sizeof(pbase_key)};
// Bases the archives lack: after the prepare step nothing adds to the opened archives in a run (arch_adopt_prebuilt clears this before the first frame), so a miss holds
// for the process and later calls skip the search.
static nvn_hmap g_pbase_miss = {.klen = sizeof(pbase_key)};
// Only callers for the same base wait on this group. compiled: its owner asked the compiler; looked: left once the owner looked in the archives; found: set before it.
@interface PbaseBuild : NSObject { @public dispatch_group_t done, looked; id<MTLRenderPipelineState> ps, found; NSError *error; int compiled; } @end
@implementation PbaseBuild @end
static nvn_hmap g_pbase_pending = {.klen = sizeof(pbase_key)};
static os_unfair_lock g_pbase_lock = OS_UNFAIR_LOCK_INIT;
static NSString *pso_user_path(void);
// The offline archive bundled with the app (DM_PSO_BUNDLE, pso_offline.py): <GPU architecture>.mtl4a and prebuilt.tsv, opened read only and looked up first among the files
// without a vertex layout, never written. A system or GPU that cannot open it gets an info line and builds the bases itself.
static id<MTL4Archive> g_arch_bundle; static NSString *g_bundle_dir;
// The prewarm lists' pipelines made offline by the prebuild tool (DM_PSO_PREBUILD_OUT) as <GPU>-prewarm-<NN>.mtl4a, one per flush period (ARCH_PERIOD_MAX), indexed before the device's files.
// g_pre_tool: the tool is running (nothing retained, no specialization, its own output not opened); g_pre_out: the bundle form's output folder.
static int g_pre_tool; static NSString *g_pre_out; static _Atomic int g_pre_warm_failed;
static NSUInteger g_prewarm_files; static NSMutableArray<id<MTL4Archive>> *g_prewarm_open;
typedef struct { uint64_t off[64], len[64]; int n; } arch_reads;
static void arch_reads_add(arch_reads *rd, uint64_t off, uint64_t len) { if (rd && len && rd->n < 64) { rd->off[rd->n] = off; rd->len[rd->n++] = len; } }
// Metal's loader crashes on malformed containers rather than returning an error: every slice's load commands and file ranges are checked first (not the compiler's payload).
static int arch_macho_ok(int fd, uint64_t off, uint64_t size, arch_reads *rd) {
    struct mach_header_64 h;
    if (size < sizeof h || pread(fd, &h, sizeof h, (off_t)off) != sizeof h || h.magic != MH_MAGIC_64 || h.filetype != MH_GPU_EXECUTE ||
        !h.ncmds || h.sizeofcmds > size - sizeof h || h.ncmds > h.sizeofcmds / sizeof(struct load_command)) return 0;
    uint64_t pos = sizeof h, end = pos + h.sizeofcmds; int pipeline = 0;
    for (uint32_t i = 0; i < h.ncmds; i++) {
        struct load_command lc;
        if (end - pos < sizeof lc || pread(fd, &lc, sizeof lc, (off_t)(off + pos)) != sizeof lc || lc.cmdsize < sizeof lc || lc.cmdsize > end - pos) return 0;
        if (lc.cmd == LC_SEGMENT_64) {
            struct segment_command_64 s;
            if (lc.cmdsize < sizeof s || pread(fd, &s, sizeof s, (off_t)(off + pos)) != sizeof s || s.fileoff > size || s.filesize > size - s.fileoff ||
                s.nsects > (lc.cmdsize - sizeof s) / sizeof(struct section_64)) return 0;
            // what the open reads: every segment but __TEXT, and __TEXT's __metallib; the GPU code is read when a pipeline is taken
            if (strncmp(s.segname, "__TEXT", sizeof s.segname)) arch_reads_add(rd, off + s.fileoff, s.filesize);
            else for (uint32_t j = 0; rd && j < s.nsects; j++) {
                struct section_64 sc;
                if (pread(fd, &sc, sizeof sc, (off_t)(off + pos + sizeof s + j * sizeof sc)) != sizeof sc) return 0;
                if (!strncmp(sc.sectname, "__metallib", sizeof sc.sectname) && sc.offset <= size && sc.size <= size - sc.offset) arch_reads_add(rd, off + sc.offset, sc.size);
            }
        } else if (lc.cmd == LC_NOTE) {
            struct note_command n;
            if (lc.cmdsize < sizeof n || pread(fd, &n, sizeof n, (off_t)(off + pos)) != sizeof n || n.offset > size || n.size > size - n.offset) return 0;
            if (n.size && !memcmp(n.data_owner, "AIR_PIPELINE", sizeof "AIR_PIPELINE")) pipeline = 1;
        }
        pos += lc.cmdsize;
    }
    return pos == end && pipeline;
}
static int arch_container_ok(int fd, uint64_t size, arch_reads *rd) {
    struct fat_header h;
    if (size < sizeof h || pread(fd, &h, sizeof h, 0) != sizeof h) return 0;
    if (h.magic == MH_MAGIC_64) return arch_macho_ok(fd, 0, size, rd);
    // the serializer's 0xcbfebabe wrapper uses fat_arch entries and includes a Metal library slice beside the GPU images
    int metal = h.magic == OSSwapBigToHostInt32(0xcbfebabe), wide = h.magic == OSSwapBigToHostInt32(FAT_MAGIC_64);
    if (!metal && !wide && h.magic != OSSwapBigToHostInt32(FAT_MAGIC)) return 0;
    uint32_t n = OSSwapBigToHostInt32(h.nfat_arch); uint64_t ent = wide ? sizeof(struct fat_arch_64) : sizeof(struct fat_arch);
    if (!n || n > (size - sizeof h) / ent) return 0;
    uint64_t table = sizeof h + n * ent; int gpu = 0;
    for (uint32_t i = 0; i < n; i++) {
        uint64_t off, len;
        if (wide) {
            struct fat_arch_64 a; if (pread(fd, &a, sizeof a, (off_t)(sizeof h + i * ent)) != sizeof a) return 0;
            off = OSSwapBigToHostInt64(a.offset); len = OSSwapBigToHostInt64(a.size);
        } else {
            struct fat_arch a; if (pread(fd, &a, sizeof a, (off_t)(sizeof h + i * ent)) != sizeof a) return 0;
            off = OSSwapBigToHostInt32(a.offset); len = OSSwapBigToHostInt32(a.size);
        }
        if (off < table || off > size || len > size - off) return 0;
        uint32_t magic;
        if (len < sizeof magic || pread(fd, &magic, sizeof magic, (off_t)off) != sizeof magic) return 0;
        if (metal && magic == 0x424c544d) {
            uint64_t lib[11];
            if (len < sizeof lib || pread(fd, lib, sizeof lib, (off_t)off) != sizeof lib || lib[2] != len) return 0;
            for (int j = 3; j < 11; j += 2) if (lib[j] > len || lib[j + 1] > len - lib[j]) return 0;
        } else {
            if (!arch_macho_ok(fd, off, len, rd)) return 0;
            gpu = 1;
        }
    }
    return gpu;
}
static uint64_t arch_file_size(NSString *path, arch_reads *rd) {
    int fd = open(path.fileSystemRepresentation, O_RDONLY); if (fd < 0) return 0;
    struct stat st; int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && arch_container_ok(fd, (uint64_t)st.st_size, rd);
    close(fd); return ok ? (uint64_t)st.st_size : 0;
}
static int arch_file_check(NSString *path, arch_reads *rd) { return arch_file_size(path, rd) != 0; }
static int arch_file_ok(NSString *path) { return arch_file_check(path, NULL); }
// Metal's open hashes every library in the file on the calling thread, one page fault at a time (docs/reference/shaders.md, A bundled pipeline archive read a page at a time when
// it opens): those ranges are read ahead 8 MB at a time on every core with MADV_WILLNEED, one mapping at a time, into the file cache Metal's mapping shares.
static id<MTL4Archive> arch_open(NSString *path, NSError **err) {
    arch_reads rd = {0};
    if (!arch_file_check(path, &rd)) return nil;
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd >= 0) {
        enum { CH = 8 << 20 }; size_t first[65] = {0}, *fp = first; const arch_reads *r = &rd;
        for (int i = 0; i < rd.n; i++) first[i + 1] = first[i] + (rd.len[i] + CH - 1) / CH;
        dispatch_apply(first[rd.n], dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t k) {
            int i = 0; while (k >= fp[i + 1]) i++;
            uint64_t at = r->off[i] + (uint64_t)(k - fp[i]) * CH, len = MIN((uint64_t)CH, r->len[i] - (uint64_t)(k - fp[i]) * CH);
            uint64_t pg = (uint64_t)getpagesize(), a0 = at / pg * pg; size_t ml = (size_t)(len + (at - a0));
            void *w = mmap(NULL, ml, PROT_READ, MAP_SHARED, fd, (off_t)a0);
            if (w != MAP_FAILED) { (void)madvise(w, ml, MADV_WILLNEED); munmap(w, ml); }
        });
        close(fd);
    }
    return [g_dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:err];
}
// every source is snapshotted before admission and the same set opened; files another run publishes later wait until the next start
@interface ArchPlan : NSObject { @public NSString *dir, *gpu, *bundle; NSArray<NSString *> *prewarm, *run; } @end
@implementation ArchPlan @end
static ArchPlan *arch_plan(NSString *device) {
    ArchPlan *p = [ArchPlan new];
    const char *e = getenv("DM_PSO_BUNDLE");
    if (e && *e && strcmp(e, "off")) {
        p->dir = @(e); p->gpu = [(id)g_dev respondsToSelector:@selector(architecture)] ? g_dev.architecture.name : nil;
        if (p->gpu) {
            p->bundle = [p->dir stringByAppendingPathComponent:[p->gpu stringByAppendingPathExtension:@"mtl4a"]];
            if (!g_pre_tool) p->prewarm = [[[NSFileManager.defaultManager contentsOfDirectoryAtPath:p->dir error:nil]
                filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self BEGINSWITH %@ AND self ENDSWITH '.mtl4a'", [p->gpu stringByAppendingString:@"-prewarm-"]]] sortedArrayUsingSelector:@selector(compare:)];
        }
    }
    if (device) p->run = [[[NSFileManager.defaultManager contentsOfDirectoryAtPath:device error:nil]
        filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]] sortedArrayUsingComparator:^NSComparisonResult(NSString *x, NSString *y) {
        int a = [x substringFromIndex:[x hasPrefix:@"p"]].intValue, b = [y substringFromIndex:[y hasPrefix:@"p"]].intValue;
        return a > b ? NSOrderedAscending : a < b ? NSOrderedDescending : NSOrderedSame;
    }];
    return p;
}
// Mappings and reservations all take address space (docs/reference/memory.md, Pipeline archives exhaust a restricted task map); bounded by TASK_VM_INFO. A failed read cannot show an archive fits.
static int arch_vm_room(uint64_t *free, uint64_t *largest) {
    task_vm_info_data_t vi; mach_msg_type_number_t count = TASK_VM_INFO_COUNT;
    if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &count) != KERN_SUCCESS || count < TASK_VM_INFO_REV2_COUNT || vi.max_address <= vi.min_address) return 0;
    uint64_t prev = vi.min_address, hi = vi.max_address; vm_address_t a = prev; vm_size_t size;
    *free = *largest = 0;
    while ((uint64_t)a < hi) {
        natural_t depth = 0; vm_region_submap_info_data_64_t info; count = VM_REGION_SUBMAP_INFO_COUNT_64;
        kern_return_t kr = vm_region_recurse_64(mach_task_self(), &a, &size, &depth, (vm_region_recurse_info_t)&info, &count);
        if (kr == KERN_INVALID_ADDRESS) break;
        if (kr != KERN_SUCCESS || !size || size > UINT64_MAX - (uint64_t)a) return 0;
        uint64_t start = MIN((uint64_t)a, hi), end = MIN((uint64_t)a + size, hi);
        if (start > prev) { uint64_t gap = start - prev; *free += gap; *largest = MAX(*largest, gap); }
        if (end <= prev) return 0;
        prev = end; a = end;
    }
    if (prev < hi) { uint64_t gap = hi - prev; *free += gap; *largest = MAX(*largest, gap); }
    return 1;
}
static int arch_admit(ArchPlan *plan, NSString *device) {
    NSMutableArray<NSString *> *paths = [NSMutableArray array];
    if (plan->bundle) [paths addObject:plan->bundle];
    for (NSString *n in plan->prewarm) [paths addObject:[plan->dir stringByAppendingPathComponent:n]];
    for (NSString *n in plan->run) {
        NSString *p = [device stringByAppendingPathComponent:n];
        if (!arch_keys_old(arch_keys_path(p))) [paths addObject:p];
    }
    uint64_t bytes = 0, pg = (uint64_t)getpagesize(); unsigned files = 0;
    for (NSString *p in paths) {
        uint64_t n = arch_file_size(p, NULL); if (!n) continue;
        if (n > UINT64_MAX - (pg - 1)) return 0;
        n = (n + pg - 1) / pg * pg;
        if (n > UINT64_MAX - bytes) return 0;
        bytes += n; files++;
    }
    uint64_t free, largest;
    if (!arch_vm_room(&free, &largest)) { MLOG("archive admission: address-space bounds unavailable: pipelines through the MTLDevice, no archives, prebuild or captures"); return 0; }
    // 2 GiB left for growth after this early reading; the whole set plus the reserve must fit in one gap.
    const uint64_t reserve = 2ull << 30;
    int ok = bytes <= largest && reserve <= largest - bytes;
    MLOG("archive admission: %u files, %.1f MiB; free address space %.1f MiB, largest gap %.1f MiB, runtime reserve %.1f MiB: %s", files, bytes / 1048576.0,
         free / 1048576.0, largest / 1048576.0, reserve / 1048576.0, ok ? "Metal 4" : "pipelines through the MTLDevice, no archives, prebuild or captures");
    return ok;
}
static void pso_bundle_open(NSMutableArray<id<MTL4Archive>> *archp, ArchPlan *plan) {
    NSString *dir = plan->dir, *gpu = plan->gpu, *p = plan->bundle;
    if (!dir) return;
    if (!p || ![NSFileManager.defaultManager fileExistsAtPath:p]) {
        NSArray<NSString *> *have = [[NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]];
        MLOG("pipeline archive bundled with the app: none for this GPU (%s; %s has %s): the bases are built on the device", gpu ? gpu.UTF8String : "architecture unknown", dir.UTF8String,
             have.count ? [have componentsJoinedByString:@", "].UTF8String : "none");
        return;
    }
    uint64_t t0 = bp_now(); NSError *ae = nil;
    id<MTL4Archive> a = arch_open(p, &ae);
    if (!a && !ae) { MLOG("[RED] pipeline archive bundled with the app: %s is not a pipeline archive: the bases are built on the device", p.UTF8String); return; }
    if (!a) { MLOG("pipeline archive bundled with the app: this system cannot open %s (%s; made for another system version, or an OS update changed the GPU compiler): the bases are built on the device",
                   p.UTF8String, ae.localizedDescription.UTF8String); return; }
    g_arch_bundle = a; g_bundle_dir = dir; [archp addObject:a];
    MLOG("pipeline archive bundled with the app: %s, %.1f MB, opened in %.1f ms", p.UTF8String, [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize] / 1048576.0, (bp_now() - t0) / 1e6);
}
// A file with a complete index is known by its keys; one without goes first among the files with a vertex layout; one this system cannot open is left out with a log line.
// Call between arch_open_begin and arch_open_end, before the device's files (the first file to name a key keeps it).
static NSString *prewarm_prefix(NSString *gpu) { return [gpu stringByAppendingString:@"-prewarm-"]; }
// A period's captures as a .mtl4-json pipelines script beside its file, so metal-tt builds the same bases for other GPUs. Descriptor capture is not needed, and with it
// the archive is not written (macOS 27.3).
static void prewarm_script(id<MTL4PipelineDataSetSerializer> ser, NSString *path) {
    NSError *e = nil; NSData *js = [ser serializeAsPipelinesScriptWithError:&e];
    NSString *sp = [path.stringByDeletingPathExtension stringByAppendingPathExtension:@"mtl4-json"];
    if (!js || !sp || ![js writeToFile:sp atomically:YES]) MLOG("[RED] pipeline prebuild: no pipelines script for %s (%s): other GPUs build these pipelines on their first start", path.lastPathComponent.UTF8String,
                                                          e ? e.localizedDescription.UTF8String : "not written");
}
static void pso_bundle_open_prewarm(NSMutableArray<id<MTL4Archive>> *arch, ArchPlan *plan) {
    NSString *dir = plan->dir; NSArray<NSString *> *names = plan->prewarm;
    if (!names.count) return;
    uint64_t t0 = bp_now(); unsigned long long bytes = 0; NSUInteger n = 0, nidx = 0, nkeys = 0, nplain = 0; g_prewarm_open = [NSMutableArray array];
    for (NSString *name in names) {
        NSString *p = [dir stringByAppendingPathComponent:name]; NSError *ae = nil;
        id<MTL4Archive> a = arch_open(p, &ae);
        if (!a) { MLOG("[RED] prewarm archive bundled with the app: %s not usable (%s): its bases are built on the device", p.UTF8String, ae ? ae.localizedDescription.UTF8String : "not a pipeline archive"); continue; }
        int complete = 0; NSData *keys = arch_keys_read(arch_keys_path(p), &complete);
        n++; bytes += [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize];
        if (keys && complete) {
            const uint64_t *kv = keys.bytes; nidx++;
            for (NSUInteger j = 0; j < keys.length / sizeof(uint64_t); j++) if (!nvn_hm_get(&g_arch_ix, &kv[j])) { nvn_hm_put(&g_arch_ix, &kv[j], (__bridge void *)a); nkeys++; }
        } else [arch insertObject:a atIndex:nplain++];
        [g_prewarm_open addObject:a];
    }
    g_prewarm_files = n;
    MLOG("prewarm archive bundled with the app: %lu of %lu files, %lu with an index of %lu bases, %.1f MB, opened in %.1f ms", (unsigned long)n, (unsigned long)names.count, (unsigned long)nidx, (unsigned long)nkeys, bytes / 1048576.0, (bp_now() - t0) / 1e6);
}
// gpuarchiver-* folders are Metal's compiler state, one per serializer, never removed by Metal (docs/reference/shaders.md, "Metal's build folders left by pipeline archive flushes").
// Without a bundle ID the folder is shared with other programs, so nothing is removed; with one, an exclusive flock at start proves no other copy runs. Never remove at flush or exit.
static NSString *ga_dir(void) {
    static NSString *dir; static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *(*metal)(void) = (NSString *(*)(void))dlsym(RTLD_DEFAULT, "MTLGetGPUArchiverCachePath");
        NSString *d = metal ? metal() : nil;
        if (![d isKindOfClass:NSString.class] || !d.length) {
            char b[PATH_MAX]; NSString *base = confstr(_CS_DARWIN_USER_CACHE_DIR, b, sizeof b) > 1 ? @(b) : NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
            NSString *bid = NSBundle.mainBundle.bundleIdentifier;
            d = base ? [(bid.length ? [base stringByAppendingPathComponent:bid] : base) stringByAppendingPathComponent:@"com.apple.gpuarchiver"] : nil;
        }
        dir = d.stringByStandardizingPath;
    });
    return dir;
}
static int ga_own(void) { return NSBundle.mainBundle.bundleIdentifier.length > 0 && ga_dir() != nil; }
static NSSet<NSString *> *ga_list(NSString *dir) {
    NSArray<NSString *> *a = dir ? [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] : nil;
    return [NSSet setWithArray:[a filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self BEGINSWITH 'gpuarchiver-'"]] ?: @[]];
}
// Held shared for the run through *fd. Returns earlier runs' folders when no other copy holds the lock, listed under the exclusive lock before any serializer is made.
static NSSet<NSString *> *ga_claim(NSString *dir, int *fd) {
    *fd = -1;
    if (!dir || ![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) return nil;
    int f = open([dir.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"gpuarchiver.lock"].fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (f < 0) return nil;
    NSSet<NSString *> *old = flock(f, LOCK_EX | LOCK_NB) ? nil : ga_list(dir);
    int r; while ((r = flock(f, LOCK_SH)) && errno == EINTR) {}
    if (r) { close(f); return nil; }
    *fd = f;
    return old;
}
static ArchEpoch *ep_new(void) {
    MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new]; sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
    ArchEpoch *ep = [ArchEpoch new]; ep->deaths = atomic_load(&g_arch_deaths); ep->ser = [g_dev newPipelineDataSetSerializerWithDescriptor:sd];
    if (!ep->ser) return nil;
    MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new]; cd.pipelineDataSetSerializer = ep->ser;
    ep->comp = [g_dev newCompilerWithDescriptor:cd error:nil];
    if (!ep->comp) return nil;
    ep->busy = dispatch_group_create();
    return ep;
}
static ArchEpoch *ep_enter(void) {   // joined: a flush hands it to arch_write only once every compile that joined it has left
    os_unfair_lock_lock(&g_ep_lock);
    ArchEpoch *ep = g_ep; atomic_fetch_add(&ep->active, 1); dispatch_group_enter(ep->busy);
    os_unfair_lock_unlock(&g_ep_lock);
    return ep;
}
static void ep_leave(ArchEpoch *ep) { atomic_fetch_sub(&ep->active, 1); dispatch_group_leave(ep->busy); }
// Workaround of a Metal bug (docs/reference/rendering.md, A pipeline compile crashes while an archive period is written): a serialization frees binary-cache entries a compile
// through another compiler reads, so a write waits for the compiles under way and holds new ones back (writer first).
static pthread_mutex_t g_ser_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ser_cv = PTHREAD_COND_INITIALIZER;
static int g_ser_compiles, g_ser_plain, g_ser_want;
static _Atomic uint64_t g_ser_held_ns, g_ser_holds;
static void ser_compile_enter(int plain) {
    pthread_mutex_lock(&g_ser_mu);
    if (g_ser_want) {
        uint64_t t0 = bp_now(); while (g_ser_want) pthread_cond_wait(&g_ser_cv, &g_ser_mu);
        atomic_fetch_add(&g_ser_held_ns, bp_now() - t0); atomic_fetch_add(&g_ser_holds, 1);
    }
    g_ser_compiles++; g_ser_plain += plain; pthread_mutex_unlock(&g_ser_mu);
}
static void ser_compile_begin(void) { ser_compile_enter(0); }
static void plain_compile_begin(void) { ser_compile_enter(1); }
static void ser_compile_leave(int plain) { pthread_mutex_lock(&g_ser_mu); g_ser_plain -= plain; if (!--g_ser_compiles && g_ser_want) pthread_cond_broadcast(&g_ser_cv); pthread_mutex_unlock(&g_ser_mu); }
static void ser_compile_end(void) { ser_compile_leave(0); }
static void plain_compile_end(void) { ser_compile_leave(1); }
// After SER_WAIT_MS a write proceeds beside other periods' stuck captures; an unresolved plain compile could still report service death, so the writer drops its period instead.
enum { SER_WAIT_MS = 2000 };
static int ser_begin(void) {
    pthread_mutex_lock(&g_ser_mu); g_ser_want++;
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += SER_WAIT_MS / 1000; until.tv_nsec += (long)(SER_WAIT_MS % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    while (g_ser_compiles && pthread_cond_timedwait(&g_ser_cv, &g_ser_mu, &until) != ETIMEDOUT) {}
    int left = g_ser_compiles, plain = g_ser_plain; pthread_mutex_unlock(&g_ser_mu);
    static _Atomic int told; if (left && !atomic_exchange(&told, 1)) MLOG("pipeline archive: %d compiles still under way after %d ms (%d plain): %s", left, SER_WAIT_MS, plain,
        plain ? "the period is not written before those compiles report their errors" : "the write goes ahead beside captures of other periods (Metal may crash on that, see ser_begin)");
    return plain;
}
static void ser_end(void) { pthread_mutex_lock(&g_ser_mu); g_ser_want--; pthread_cond_broadcast(&g_ser_cv); pthread_mutex_unlock(&g_ser_mu); }
// Metal's per-serializer LMDB store has a fixed map, and once full every compile through that compiler fails (MDB_MAP_FULL; docs/reference/rendering.md, Metal's archiver
// store filled by one flush period): the ARCH_PERIOD_MAX-th capture hands the period over. The tests lower g_arch_period_max.
enum { ARCH_PERIOD_MAX = 2000 };
static int g_arch_period_max = ARCH_PERIOD_MAX;
// <seq>-<count>.mtl4a: tier 0 under PSO_ARCH_COMPACT pipelines, each next PSO_ARCH_COMPACT times larger; 8, in none, for a full period's file; an old name counts as the top tier below that.
static int arch_tier(NSString *path) {
    NSString *n = path.lastPathComponent.stringByDeletingPathExtension; NSRange r = [n rangeOfString:@"-"];
    NSString *cs = r.location == NSNotFound ? @"" : [n substringFromIndex:r.location + 1];
    int c = cs.length && cs.length < 10 && [cs rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location == NSNotFound ? cs.intValue : 0, t = 0;
    if (c <= 0) c = g_arch_period_max - 1;
    if (c >= g_arch_period_max) return 8;
    for (; c >= PSO_ARCH_COMPACT && t < 7; c /= PSO_ARCH_COMPACT) t++;
    return t;
}
static void arch_compact_pick(NSArray<NSString *> *paths) {
    int per[9] = {0}, top = -1;
    for (NSString *p in paths) per[arch_tier(p)]++;
    for (int t = 0; t < 8; t++) if (per[t] > PSO_ARCH_COMPACT) top = t;
    if (top < 0 || !arch_compact_allowed()) return;
    if (!(g_compact_state = calloc(paths.count + 1, 1))) return;
    for (NSUInteger i = 0; i < paths.count; i++) if (arch_tier(paths[i]) <= top) g_compact_state[i] = 1;
    g_compact_files = paths; g_compact_enabled = 1;
}
static int arch_compact_takes(id<MTL4Archive> a) {
    if (!g_compact_enabled || !g_compact_state) return g_compact_enabled;
    NSUInteger i = [g_arch_run.count ? g_arch_run : g_arch4 indexOfObjectIdenticalTo:a]; return i != NSNotFound && g_compact_state[i];
}
// Files no pipeline was taken from leave once every lookup that could reach one was made (the prewarm lists and the prebuild's pairs). Not in the bundle form, nor for a file the compaction takes.
static void arch_sweep(void) {
    if (!g_arch_use.count || g_pre_out || !atomic_load(&g_sweep_warm) || !atomic_load(&g_sweep_pairs) || atomic_load(&g_warm_stop)) return;
    static os_unfair_lock lock = OS_UNFAIR_LOCK_INIT; int n = 0, np = 0; unsigned long long bytes = 0;
    os_unfair_lock_lock(&lock);
    for (ArchUse *u in g_arch_use) {
        if (u->gone || __atomic_load_n(&u->hits, __ATOMIC_RELAXED)) continue;
        id<MTL4Archive> a = u->a;
        if (a && arch_compact_takes(a)) continue;
        unsigned long long size = [[NSFileManager.defaultManager attributesOfItemAtPath:u->path error:nil] fileSize];
        if (arch_retire(u->path)) { u->gone = 1; n++; np += u->pf; bytes += size; }
    }
    static int told; int kept = 0, low = 0; unsigned long long taken = 0, named = 0, low_bytes = 0;
    for (ArchUse *u in g_arch_use) {
        if (u->gone || !u->keys) continue;
        uint64_t h = MIN(__atomic_load_n(&u->hits, __ATOMIC_RELAXED), u->keys); kept++; taken += h; named += u->keys;
        if (h * 4 < u->keys) { low++; low_bytes += [[NSFileManager.defaultManager attributesOfItemAtPath:u->path error:nil] fileSize]; }
    }
    int say_use = kept && !told; told = 1;
    os_unfair_lock_unlock(&lock);
    const char *o = getenv("DM_OBSOLETE");
    if (n) MLOG("pipeline archive: %d files no pipeline was taken from (%d of bases without a vertex layout), %.1f MB, %s", n, np, bytes / 1048576.0, o && *o ? "moved to the obsolete folder" : "removed");
    if (say_use) MLOG("pipeline archive: %d indexed files kept, %llu of their %llu pipelines taken; %d of them under a quarter used (%.1f MB)", kept, taken, named, low, low_bytes / 1048576.0);
}
static _Atomic int g_arch_cap_off;   // a failed capture in an empty period: the store was not full, so no more captures this run
static int arch_capturing(void) { return g_arch_cap && !atomic_load_explicit(&g_arch_cap_off, memory_order_relaxed); }
static void arch_flush_from(ArchEpoch *only);
static void arch_at_exit(void);
// The archive is a cache, never a condition of drawing: a refused capture's pipeline is used. A period with captures is taken as full and handed over; a failure in an empty one stops captures.
static void ep_key(ArchEpoch *ep, MTL4PipelineDescriptor *d) {   // recorded in its period before the compile leaves it, so the write sees it
    uint64_t k = arch_key(d);
    if (!k) { atomic_store(&ep->unkeyed, 1); return; }
    os_unfair_lock_lock(&ep->klock); if (!ep->keys) ep->keys = [NSMutableData data]; [ep->keys appendBytes:&k length:sizeof k]; os_unfair_lock_unlock(&ep->klock);
}
// Metal's error names the lost XPC connection; its domain is the GPU family's and its code a generic 2, so the text tells it apart
static int err_compiler_died(NSError *e) {
    NSString *t = e.localizedDescription;
    return t && ([t containsString:@"XPC_ERROR_CONNECTION_INTERRUPTED"] || [t containsString:@"interrupted connection"]);
}
static void arch_cap_failed(ArchEpoch *ep, NSError *ce, const char *what) {
    const char *why = ce ? ce.localizedDescription.UTF8String : "no error given";
    int n = atomic_load(&ep->n);
    if (!n) { if (!atomic_exchange(&g_arch_cap_off, 1)) MLOG("[RED] pipeline archive: capturing a %s pipeline failed in a new flush period (%s): no more captures this run, pipelines are built without the archive", what, why); return; }
    if (atomic_exchange(&ep->failed, 1)) return;
    MLOG("[RED] pipeline archive: capturing a %s pipeline failed after %d in its flush period (%s): built without the archive, the captures go on in a new period", what, n, why);
    arch_flush_from(ep);
}
// A service death takes every compile it held down, the current period's captures among them (docs/reference/rendering.md, An archive period written after the compiler
// service died): the period is marked and handed over.
static void arch_died(NSError *e) {
    if (!e || !g_arch_cap || !err_compiler_died(e)) return;
    atomic_fetch_add(&g_arch_deaths, 1);
    os_unfair_lock_lock(&g_ep_lock); ArchEpoch *ep = g_ep; os_unfair_lock_unlock(&g_ep_lock);
    if (!ep) return;
    if (!atomic_exchange(&ep->crashed, 1)) MLOG("[RED] pipeline archive: the compiler service died under a compile (%d captured in the flush period so far): the period is not written, the captures go on in a new one", atomic_load(&ep->n));
    arch_flush_from(ep);
}
static id<MTLRenderPipelineState> plain_rps(MTL4PipelineDescriptor *d, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id ps = [g_comp4 newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:&e]; return compile_done(ps, e, out);
}
static id<MTLComputePipelineState> plain_cps(MTL4ComputePipelineDescriptor *d, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id ps = [g_comp4 newComputePipelineStateWithDescriptor:d compilerTaskOptions:nil error:&e]; return compile_done(ps, e, out);
}
static id<MTLRenderPipelineState> plain_spec(MTL4RenderPipelineDescriptor *d, id<MTLRenderPipelineState> base, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id ps = [g_comp4 newRenderPipelineStateBySpecializationWithDescriptor:d pipeline:base error:&e]; return compile_done(ps, e, out);
}
// Workaround of a Metal bug (docs/reference/rendering.md, Two captures of one shader function crash in Metal's binary cache): captures sharing a vertex-stage function never
// compile at once. One that cannot claim its functions waits g_fn_wait_ms and is built by the plain compiler, capturing nothing (the prebuild puts its pair back instead).
enum { FN_BUSY_MAX = 256, FN_WAIT_MS = 2000 };
static int g_fn_wait_ms = FN_WAIT_MS, g_fn_guard = 1;
static pthread_mutex_t g_fn_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_fn_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_fn_busy[FN_BUSY_MAX]; static int g_fn_nbusy;
static _Atomic int g_fn_waits, g_fn_plain;
static uint64_t fn_id(MTL4FunctionDescriptor *fd) {
    MTL4FunctionDescriptor *f = fd;
    while ([f isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) f = ((MTL4SpecializedFunctionDescriptor *)f).functionDescriptor;
    if (![f isKindOfClass:MTL4LibraryFunctionDescriptor.class]) return 0;
    MTL4LibraryFunctionDescriptor *l = (MTL4LibraryFunctionDescriptor *)f; id<MTLLibrary> lib = l.library;
    if (!lib || !l.name) return 0;
    NSString *tag = objc_getAssociatedObject(lib, &g_lib_tag);
    uint64_t h = (tag ? (uint64_t)tag.hash : (uint64_t)(uintptr_t)lib) * 0x9E3779B97F4A7C15ull ^ (uint64_t)l.name.hash;
    return h ? h : 1;
}
static int fn_ids(MTL4PipelineDescriptor *d, uint64_t k[3]) {
    MTL4FunctionDescriptor *f[3] = {nil, nil, nil};
    if ([d isKindOfClass:MTL4RenderPipelineDescriptor.class]) f[0] = ((MTL4RenderPipelineDescriptor *)d).vertexFunctionDescriptor;
    else if ([d isKindOfClass:MTL4MeshRenderPipelineDescriptor.class]) { MTL4MeshRenderPipelineDescriptor *m = (MTL4MeshRenderPipelineDescriptor *)d; f[0] = m.objectFunctionDescriptor; f[1] = m.meshFunctionDescriptor; }
    else if ([d isKindOfClass:MTL4ComputePipelineDescriptor.class]) f[0] = ((MTL4ComputePipelineDescriptor *)d).computeFunctionDescriptor;
    int n = 0; for (int i = 0; i < 3; i++) if ((k[n] = fn_id(f[i]))) n++;
    return n;
}
static int fn_busy(const uint64_t *k, int n) { for (int i = 0; i < n; i++) for (int j = 0; j < g_fn_nbusy; j++) if (g_fn_busy[j] == k[i]) return 1; return 0; }
// 1: claimed until fn_release (or none to claim); 0: still busy, or the table full, after wait_ms
static int fn_claim(const uint64_t *k, int n, int wait_ms) {
    if (!n || !g_fn_guard) return 1;
    pthread_mutex_lock(&g_fn_mu);
    struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec += wait_ms / 1000; until.tv_nsec += (long)(wait_ms % 1000) * 1000000;
    if (until.tv_nsec >= 1000000000) { until.tv_sec++; until.tv_nsec -= 1000000000; }
    if (fn_busy(k, n)) atomic_fetch_add(&g_fn_waits, 1);
    while ((fn_busy(k, n) || g_fn_nbusy + n > FN_BUSY_MAX) && wait_ms > 0 && pthread_cond_timedwait(&g_fn_cv, &g_fn_mu, &until) != ETIMEDOUT) {}
    int ok = !fn_busy(k, n) && g_fn_nbusy + n <= FN_BUSY_MAX;
    if (ok) for (int i = 0; i < n; i++) g_fn_busy[g_fn_nbusy++] = k[i];
    pthread_mutex_unlock(&g_fn_mu);
    return ok;
}
static void fn_release(const uint64_t *k, int n) {
    if (!n || !g_fn_guard) return;
    pthread_mutex_lock(&g_fn_mu);
    for (int i = 0; i < n; i++) for (int j = 0; j < g_fn_nbusy; j++) if (g_fn_busy[j] == k[i]) { g_fn_busy[j] = g_fn_busy[--g_fn_nbusy]; break; }
    pthread_cond_broadcast(&g_fn_cv); pthread_mutex_unlock(&g_fn_mu);
}
static void fn_gave_up(const char *what) {
    if (!atomic_fetch_add(&g_fn_plain, 1))
        MLOG("pipeline archive: a %s capture waited %d ms for another of the same shader function: built without the archive (Metal crashes on two at once, fn_claim); "
             "the prebuild tool's last line counts them", what, g_fn_wait_ms);
}
// The prebuild tool's capture ledger (g_pre_tool or g_pre_out; nothing in play): each pipeline it asked for, its descriptor kept until its period is written, so the
// captures a dropped period lost are made again in new periods (arch_pkg_recover). A pipeline Metal refuses outright (not a compiler death) is not tried again.
enum { ARCH_PKG_PENDING, ARCH_PKG_WRITTEN, ARCH_PKG_LOST_BUILDABLE, ARCH_PKG_LOST_UNCONFIRMED, ARCH_PKG_REJECTED };
@interface ArchPkgItem : NSObject { @public uint64_t key; MTL4PipelineDescriptor *desc; int vd, compute, state, retries; NSString *reason; } @end
@implementation ArchPkgItem @end
static os_unfair_lock g_pkg_lock = OS_UNFAIR_LOCK_INIT;
static NSMutableDictionary<NSNumber *, ArchPkgItem *> *g_pkg_items;
static _Atomic int g_pkg_unkeyed_lost;
static int g_arch_pkg_max_retries = 3;
static int g_pkg_last_requested, g_pkg_last_published, g_pkg_last_recovered, g_pkg_last_missing_lost, g_pkg_last_missing_died, g_pkg_last_rejected, g_pkg_last_rounds;
static int arch_pkg_on(void) { return g_pre_tool || g_pre_out != nil; }
static void arch_pkg_reset(void) {
    os_unfair_lock_lock(&g_pkg_lock); g_pkg_items = nil; os_unfair_lock_unlock(&g_pkg_lock);
    atomic_store(&g_pkg_unkeyed_lost, 0);
    g_pkg_last_requested = g_pkg_last_published = g_pkg_last_recovered = g_pkg_last_missing_lost = g_pkg_last_missing_died = g_pkg_last_rejected = g_pkg_last_rounds = 0;
}
static ArchPkgItem *arch_pkg_item(uint64_t k) {   // under g_pkg_lock
    if (!g_pkg_items) g_pkg_items = [NSMutableDictionary dictionary];
    ArchPkgItem *it = g_pkg_items[@(k)];
    if (!it) { it = [ArchPkgItem new]; it->key = k; g_pkg_items[@(k)] = it; }
    return it;
}
static void arch_pkg_note_entered(MTL4PipelineDescriptor *d, int vd, int compute) {
    uint64_t k = arch_key(d);
    if (!k) return;
    MTL4PipelineDescriptor *copy = [d copy];
    os_unfair_lock_lock(&g_pkg_lock);
    ArchPkgItem *it = arch_pkg_item(k);
    if (it->state != ARCH_PKG_WRITTEN) {
        it->state = ARCH_PKG_PENDING; it->vd = vd; it->compute = compute;
        if (!it->desc) it->desc = copy;
    }
    os_unfair_lock_unlock(&g_pkg_lock);
}
// not captured: built plainly (lost, buildable), lost to a compiler death (unconfirmed), or refused by both compilers (rejected). ce: the capture's error, pe the plain one's
static void arch_pkg_note_fallback(MTL4PipelineDescriptor *d, int vd, int compute, int built, NSError *ce, NSError *pe) {
    uint64_t k = arch_key(d);
    int state = built ? ARCH_PKG_LOST_BUILDABLE : err_compiler_died(pe) ? ARCH_PKG_LOST_UNCONFIRMED : pe ? ARCH_PKG_REJECTED : err_compiler_died(ce) ? ARCH_PKG_LOST_UNCONFIRMED : ARCH_PKG_REJECTED;
    if (!k) { if (state != ARCH_PKG_REJECTED) atomic_fetch_add(&g_pkg_unkeyed_lost, 1); return; }
    MTL4PipelineDescriptor *copy = state == ARCH_PKG_REJECTED ? nil : [d copy];
    NSString *why = pe.localizedDescription ?: ce.localizedDescription;
    os_unfair_lock_lock(&g_pkg_lock);
    ArchPkgItem *it = arch_pkg_item(k);
    if (it->state != ARCH_PKG_WRITTEN) {
        it->state = state; it->vd = vd; it->compute = compute;
        if (state == ARCH_PKG_REJECTED) it->desc = nil;
        else if (!it->desc) it->desc = copy;
        if (why) it->reason = why;
    }
    os_unfair_lock_unlock(&g_pkg_lock);
}
static void arch_pkg_period_done(ArchEpoch *ep, int ok) {
    if (!arch_pkg_on()) return;
    if (!ok && atomic_load(&ep->unkeyed)) atomic_fetch_add(&g_pkg_unkeyed_lost, 1);
    NSData *keys = ep->keys;
    if (!keys.length) return;
    const uint64_t *kv = keys.bytes; NSUInteger nk = keys.length / sizeof(uint64_t);
    os_unfair_lock_lock(&g_pkg_lock);
    for (NSUInteger i = 0; i < nk; i++) {
        ArchPkgItem *it = g_pkg_items[@(kv[i])];
        if (!it || it->state == ARCH_PKG_WRITTEN) continue;
        if (ok) { it->state = ARCH_PKG_WRITTEN; it->desc = nil; }
        else it->state = ARCH_PKG_LOST_BUILDABLE;
    }
    os_unfair_lock_unlock(&g_pkg_lock);
}
static id<MTLRenderPipelineState> arch_plain_rps(MTL4PipelineDescriptor *d, NSError **e, int vd, NSError *ce) {
    NSError *pe = nil; id<MTLRenderPipelineState> ps = plain_rps(d, &pe);
    if (e && pe) *e = pe;
    if (arch_pkg_on()) arch_pkg_note_fallback(d, vd, 0, ps != nil, ce, pe);
    return ps;
}
static id<MTLComputePipelineState> arch_plain_cps(MTL4ComputePipelineDescriptor *d, NSError **e, NSError *ce) {
    NSError *pe = nil; id<MTLComputePipelineState> ps = plain_cps(d, &pe);
    if (e && pe) *e = pe;
    if (arch_pkg_on()) arch_pkg_note_fallback(d, 0, 1, ps != nil, ce, pe);
    return ps;
}
// A captured compile in the current period (vd: a base with a vertex layout). No task options: a captured base never imports archive entries. A refused capture is built
// plainly (arch_cap_failed). busy (the prebuild): a capture that could not claim its functions is not built (*busy = 1, nil).
static id<MTLRenderPipelineState> arch_capture_try(MTL4PipelineDescriptor *d, NSError **e, int vd, int *captured, int wait_ms, int *busy) {
    if (captured) *captured = 0;
    if (busy) *busy = 0;
    if (!arch_capturing()) return arch_plain_rps(d, e, vd, nil);
    uint64_t fk[3]; int fn = fn_ids(d, fk);
    if (!fn_claim(fk, fn, wait_ms)) {
        if (busy) { *busy = 1; return nil; }
        fn_gave_up("render");
        return arch_plain_rps(d, e, vd, nil);
    }
    ser_compile_begin(); ArchEpoch *ep = ep_enter(); NSError *ce = nil;
    id<MTLRenderPipelineState> ps = [ep->comp newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:&ce];
    // the period may already belong to its writer: mark it while still joined, before any fallback
    if (!ps && err_compiler_died(ce)) { atomic_store(&ep->crashed, 1); arch_died(ce); }
    fn_release(fk, fn);
    int full = 0;
    if (ps) { ep_key(ep, d); if (arch_pkg_on()) arch_pkg_note_entered(d, vd, 0);
              atomic_fetch_add(&g_arch_new, 1); if (vd) { atomic_fetch_add(&ep->vd, 1); atomic_fetch_add(&g_arch_new_vd, 1); }
              if ([d isKindOfClass:MTL4MeshRenderPipelineDescriptor.class]) atomic_fetch_add(&ep->ms, 1);
              full = atomic_fetch_add(&ep->n, 1) + 1 == g_arch_period_max; }
    ep_leave(ep); ser_compile_end();
    if (ps) { if (captured) *captured = 1; if (full) arch_flush_from(ep); return ps; }
    ps = arch_plain_rps(d, e, vd, ce);
    if (ps && !atomic_load(&ep->crashed)) arch_cap_failed(ep, ce, "render"); else if (!ps && e && !*e) *e = ce;
    return ps;
}
static id<MTLRenderPipelineState> arch_capture(MTL4PipelineDescriptor *d, NSError **e, int vd, int *captured) { return arch_capture_try(d, e, vd, captured, g_fn_wait_ms, NULL); }
static id<MTLComputePipelineState> arch_capture_cs(MTL4ComputePipelineDescriptor *d, NSError **e, int *captured) {
    if (captured) *captured = 0;
    if (!arch_capturing()) return arch_plain_cps(d, e, nil);
    uint64_t fk[3]; int fn = fn_ids(d, fk);
    if (!fn_claim(fk, fn, g_fn_wait_ms)) {
        fn_gave_up("compute");
        return arch_plain_cps(d, e, nil);
    }
    ser_compile_begin(); ArchEpoch *ep = ep_enter(); NSError *ce = nil;
    id<MTLComputePipelineState> ps = [ep->comp newComputePipelineStateWithDescriptor:d compilerTaskOptions:nil error:&ce];
    if (!ps && err_compiler_died(ce)) { atomic_store(&ep->crashed, 1); arch_died(ce); }
    fn_release(fk, fn);
    int full = 0;
    if (ps) { ep_key(ep, d); if (arch_pkg_on()) arch_pkg_note_entered(d, 0, 1);
              atomic_fetch_add(&g_arch_new, 1); atomic_fetch_add(&ep->cs, 1); full = atomic_fetch_add(&ep->n, 1) + 1 == g_arch_period_max; }
    ep_leave(ep); ser_compile_end();
    if (ps) { if (captured) *captured = 1; if (full) arch_flush_from(ep); return ps; }
    ps = arch_plain_cps(d, e, ce);
    if (ps && !atomic_load(&ep->crashed)) arch_cap_failed(ep, ce, "compute"); else if (!ps && e && !*e) *e = ce;
    return ps;
}
// Workaround of a platform limit: Foundation reads an archive on an external drive whole into memory (docs/reference/memory.md, A pipeline archive on an external drive is read
// into memory), so while the host opens its archives a .mtl4a read asks for a mapping. Without the method, the files are read into memory.
typedef id (*ns_read_fn)(id, SEL, NSString *, NSUInteger, NSUInteger, NSError **);
static ns_read_fn g_ns_read;
static id arch_read_mapped(id self, SEL cmd, NSString *path, NSUInteger options, NSUInteger max, NSError **err) {
    if ([path hasSuffix:@".mtl4a"]) options = (options & ~(NSUInteger)NSDataReadingMappedIfSafe) | NSDataReadingMappedAlways;
    return g_ns_read(self, cmd, path, options, max, err);
}
static Method arch_read_method(void) { return class_getInstanceMethod([NSData class], sel_registerName("initWithContentsOfFile:options:maxLength:error:")); }
static void arch_open_begin(void) { Method m = arch_read_method(); if (m) g_ns_read = (ns_read_fn)method_setImplementation(m, (IMP)arch_read_mapped); }
static void arch_open_end(void) { Method m = arch_read_method(); if (m && g_ns_read) method_setImplementation(m, (IMP)g_ns_read); }
static void pso4_init_once(void) {
        if (!pso4_on() || ![(id)g_dev respondsToSelector:@selector(newCompilerWithDescriptor:error:)]) return;
        const char *e = getenv("DM_PSO_ARCHIVE"); NSString *user = pso_user_path();
        g_arch_dir = e && !strcmp(e, "off") ? nil : e && *e ? @(e) : user ? [user.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_archive"] : nil;
        ArchPlan *plan = arch_plan(g_arch_dir);
        if (!arch_admit(plan, g_arch_dir)) { g_arch_dir = nil; return; }
        NSMutableArray<id<MTL4Archive>> *arch = [NSMutableArray array], *archp = [NSMutableArray array];
        arch_open_begin();
        pso_bundle_open(archp, plan);
        pso_bundle_open_prewarm(arch, plan);
        if (g_arch_dir) {
            g_arch_q = dispatch_queue_create("dm.nvn.pso-archive", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
            g_arch_pending = dispatch_group_create();
            if (ga_own()) {
                static int lock_fd; NSString *gd = ga_dir(); NSSet<NSString *> *old = ga_claim(gd, &lock_fd);
                if (lock_fd < 0) MLOG("[RED] pipeline archive: no lock beside %s: Metal's build folders there are left alone this run, and this run's may go with another copy's start", gd.UTF8String);
                else if (!old) MLOG("pipeline archive: another copy of this app is running: Metal's build folders in %s are left alone", gd.UTF8String);
                if (old.count) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    uint64_t t0 = bp_now(); int n = 0; for (NSString *name in old) n += [NSFileManager.defaultManager removeItemAtPath:[gd stringByAppendingPathComponent:name] error:nil];
                    MLOG("pipeline archive: %d of %lu of Metal's build folders left by earlier runs removed from %s in %.1f s", n, (unsigned long)old.count, gd.UTF8String, (bp_now() - t0) / 1e9);
                });
            }
            uint64_t t0 = bp_now(); unsigned long long bytes = 0;
            // a write a stopped run left (<file>.mtl4a.tmp, renamed once its index is written)
            for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:g_arch_dir error:nil])
                if ([f hasSuffix:@".mtl4a.tmp"]) { [NSFileManager.defaultManager removeItemAtPath:[g_arch_dir stringByAppendingPathComponent:f] error:nil]; MLOG("pipeline archive: %s removed (a write a stopped run left)", f.UTF8String); }
            NSMutableArray<NSString *> *runf = [NSMutableArray array];
            NSArray<NSString *> *all = [NSFileManager.defaultManager contentsOfDirectoryAtPath:g_arch_dir error:nil]; NSSet<NSString *> *present = [NSSet setWithArray:all ?: @[]];
            NSArray<NSString *> *names = plan->run;
            for (NSString *n in all) if ([n hasSuffix:@".keys"] && ![present containsObject:[n.stringByDeletingPathExtension stringByAppendingString:@".mtl4a"]])
                unlink([g_arch_dir stringByAppendingPathComponent:n].fileSystemRepresentation);
            // An old-index file (arch_keys_old) is reachable by no key and would be opened at every start: it goes, and a prebuild file takes prebuilt.tsv with it so the prebuild runs again.
            NSMutableArray<NSString *> *keep = [NSMutableArray array]; int nold = 0, pold = 0;
            for (NSString *n in names) {
                NSString *p = [g_arch_dir stringByAppendingPathComponent:n];
                if (!arch_keys_old(arch_keys_path(p))) { [keep addObject:n]; continue; }
                arch_retire(p); nold++; pold += [n hasPrefix:@"p"];
            }
            if (pold) unlink([g_arch_dir stringByAppendingPathComponent:@"prebuilt.tsv"].fileSystemRepresentation);
            if (nold) MLOG("pipeline archive: %d files indexed by library file names moved out (%d of them the prebuild's): their bases are built again once", nold, pold);
            names = keep;
            NSUInteger nn = names.count; NSMutableArray *opened = [NSMutableArray arrayWithCapacity:nn], *ixs = [NSMutableArray arrayWithCapacity:nn], *why = [NSMutableArray arrayWithCapacity:nn];
            for (NSUInteger i = 0; i < nn; i++) { [opened addObject:NSNull.null]; [ixs addObject:NSNull.null]; [why addObject:NSNull.null]; }
            dispatch_apply(nn, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t i) { @autoreleasepool {
                NSString *p = [g_arch_dir stringByAppendingPathComponent:names[i]]; NSError *ae = nil; int complete = 0;
                id<MTL4Archive> a = arch_open(p, &ae);
                NSData *keys = a ? arch_keys_read(arch_keys_path(p), &complete) : nil;
                @synchronized (opened) {
                    if (a) opened[i] = a;
                    if (keys) ixs[i] = @[keys, @(complete)];
                    if (ae) why[i] = ae.localizedDescription ?: @"";
                }
            } });
            NSMutableArray<id<MTL4Archive>> *ixf = [NSMutableArray array], *run = [NSMutableArray array]; int nopen = 0, np = 0, nidx = 0; unsigned long nkeys = 0;
            NSMutableArray<ArchUse *> *use = [NSMutableArray array];
            g_arch_opened = [NSMutableSet set];
            for (NSUInteger i = 0; i < nn; i++) {
                NSString *n = names[i], *p = [g_arch_dir stringByAppendingPathComponent:n]; int pf = [n hasPrefix:@"p"];
                g_arch_seq = MAX(g_arch_seq, [n substringFromIndex:pf].intValue + 1);
                if (opened[i] == NSNull.null) {
                    // an archive opens only on the major OS version it was made for; a prebuild file takes the prebuild's stamp with it
                    MLOG("[RED] pipeline archive file %s not usable (%s): moved out, its bases are built again", p.UTF8String, why[i] != NSNull.null ? [why[i] UTF8String] : "not a pipeline archive");
                    arch_retire(p);
                    if (pf) unlink([g_arch_dir stringByAppendingPathComponent:@"prebuilt.tsv"].fileSystemRepresentation);
                    continue;
                }
                id<MTL4Archive> a = opened[i]; NSArray *ix = ixs[i] != NSNull.null ? ixs[i] : nil;
                [g_arch_opened addObject:p];
                ArchUse *u = [ArchUse new]; u->path = p; u->a = a; u->pf = pf; objc_setAssociatedObject(a, &g_arch_use_tag, u, OBJC_ASSOCIATION_RETAIN_NONATOMIC); [use addObject:u];
                if (ix) u->keys = [ix[0] length] / sizeof(uint64_t);
                bytes += [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize];
                nopen++; np += pf; nidx += ix != nil;
                if (!pf) { [runf addObject:p]; [run addObject:a]; }
                if (!ix) { [pf ? archp : arch addObject:a]; continue; }
                NSData *keys = ix[0]; const uint64_t *kv = keys.bytes; NSUInteger nk = keys.length / sizeof(uint64_t);
                for (NSUInteger j = 0; j < nk; j++) if (!nvn_hm_get(&g_arch_ix, &kv[j])) { nvn_hm_put(&g_arch_ix, &kv[j], (__bridge void *)a); nkeys++; }
                if ([ix[1] intValue]) [ixf addObject:a]; else [pf ? archp : arch addObject:a];
            }
            g_arch_ixf = ixf; g_arch_run = run; g_arch_use = use;
            arch_compact_pick(runf);
            MLOG("pipeline archive %s: %d files (%d of bases without a vertex layout, %d with an index of %lu pipelines), %.1f MB, opened in %.1f ms", g_arch_dir.UTF8String,
                 nopen, np, nidx, nkeys, bytes / 1048576.0, (bp_now() - t0) / 1e6);
        }
        arch_open_end();
        g_arch4 = arch; g_arch4p = archp;
        NSError *ce = nil; g_comp4 = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&ce];
        if (!g_comp4) MLOG("[RED] no Metal 4 compiler (%s): render pipelines are built through the MTLDevice", ce.localizedDescription.UTF8String);
        else if (g_arch_dir) {
            g_ep = ep_new(); g_arch_cap = g_ep != nil; atomic_store(&g_ep_t0, bp_now());
            if (g_arch_cap) atexit(arch_at_exit);
            if (!g_ep) MLOG("[RED] pipeline archive: Metal made no pipeline serializer: nothing is captured into %s", g_arch_dir.UTF8String);
        }
}
static void pso4_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // opening can take seconds on a slow external disk: hold guest clock reads (reason 4, hle_os.c)
        int freeze = !g_pre_tool;
        if (freeze) guest_clock_freeze_for(4, 1);
        pso4_init_once();
        if (freeze) guest_clock_freeze_for(4, 0);
    });
}
static int pso4_compiler(void) { pso4_init(); return g_comp4 != nil; }
// Compute pipelines depend on their function alone: from the bundled archive, else the device's files, else built through the MTLDevice and captured again on a background queue
// (arch_harvest) so the next start finds it (docs/reference/shaders.md, Compute pipelines outside the bundled archive built at every start). Embedded kernels are neither looked up nor captured.
static _Atomic uint64_t g_cps_arch, g_cps_run, g_cps_built, g_cps_look_ns, g_cps_build_ns;
static void arch_note_new(void);
static dispatch_queue_t arch_harvest_q(void) {   // one capture at a time, at utility QoS: a network shader's compile can take seconds, beside the game's frames
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("dm.nvn.arch-harvest", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q;
}
static void arch_harvest(MTL4PipelineDescriptor *d, int compact) {
    if (!d) return;
    if (!arch_capturing()) { if (compact) g_compact_bad = 1; return; }
    if (compact) atomic_fetch_add(&g_compact_harvests, 1);
    dispatch_async(arch_harvest_q(), ^{ @autoreleasepool {
        NSError *ce = nil; int cap = 0, compute = [d isKindOfClass:MTL4ComputePipelineDescriptor.class];
        id ps = compute ? (id)arch_capture_cs((MTL4ComputePipelineDescriptor *)d, &ce, &cap) : (id)arch_capture(d, &ce, 0, &cap);
        if (cap) { if (compact) g_compacted++; arch_note_new(); }
        else if (compact) g_compact_bad = 1;
        if (!ps) {
            MTL4FunctionDescriptor *fd = compute ? ((MTL4ComputePipelineDescriptor *)d).computeFunctionDescriptor : ((MTL4MeshRenderPipelineDescriptor *)d).meshFunctionDescriptor;
            if ([fd isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) fd = ((MTL4SpecializedFunctionDescriptor *)fd).functionDescriptor;
            MLOG("[RED] %s pipeline %s: the Metal 4 compiler refused it (%s): not kept in the pipeline archive", compute ? "compute" : "task / mesh",
                 [fd isKindOfClass:MTL4LibraryFunctionDescriptor.class] ? ((MTL4LibraryFunctionDescriptor *)fd).name.UTF8String : "(unnamed)", ce ? ce.localizedDescription.UTF8String : "no error given");
        }
        if (compact && atomic_fetch_sub(&g_compact_harvests, 1) == 1 && !atomic_load(&g_warm_running)) arch_flush_from(nil);
    } });
}
// Waits (up to until) for the captures already queued to join their period, so a waiting flush does not leave them to an unwritten next period (docs/reference/rendering.md,
// Captures still queued on the harvest queue left out of a waiting flush). Non-zero on timeout.
static long arch_harvest_wait(dispatch_time_t until) {
    dispatch_block_t mark = dispatch_block_create(0, ^{});
    dispatch_async(arch_harvest_q(), mark);
    return dispatch_block_wait(mark, until);
}
// only a function whose constants are all the host's (pso_offline.py HOST_CONSTANTS, the split pass) is looked up and captured; others are built from fn_dev's specialization
static int fd_host_fc(MTL4FunctionDescriptor *fd) {
    if (![fd isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) return 0;
    MTL4FunctionDescriptor *ld = ((MTL4SpecializedFunctionDescriptor *)fd).functionDescriptor;
    if (![ld isKindOfClass:MTL4LibraryFunctionDescriptor.class]) return 0;
    MTL4LibraryFunctionDescriptor *l = (MTL4LibraryFunctionDescriptor *)ld;
    // NOLINTNEXTLINE(clang-analyzer-nullability.NullablePassedToNonnull): the descriptor's name is set when it is made and never changed
    NSDictionary<NSString *, MTLFunctionConstant *> *fc = [l.library newFunctionWithName:l.name].functionConstantsDictionary;
    if (!fc.count) return 0;
    for (NSString *k in fc)
        if (![k isEqualToString:@"dm_border_off"] && ![k isEqualToString:@"dm_blend_off"] && ![k isEqualToString:@"dm_mwa3_fc"] && fc[k].index != DMSH_SPLIT_FC) return 0;
    return 1;
}
static id<MTLComputePipelineState> cps_new_b(id<MTLFunction> f, int off, NSError **e) {
    pso4_init();
    MTL4FunctionDescriptor *fd = fn4_of_b(f, off); MTL4ComputePipelineDescriptor *d = nil; id<MTLComputePipelineState> ps = nil; uint64_t t0 = bp_now();
    if ([fd isKindOfClass:MTL4LibraryFunctionDescriptor.class] || fd_host_fc(fd) || (off & (FN_SPLIT0 | FN_SPLIT1))) {
        d = [MTL4ComputePipelineDescriptor new]; d.computeFunctionDescriptor = fd;
        if (g_arch_bundle && (ps = [g_arch_bundle newComputePipelineStateWithDescriptor:d error:nil])) atomic_fetch_add(&g_cps_arch, 1);
        else { int from_run = 0; if ((ps = arch_lookup(d, 1, &from_run))) {
            atomic_fetch_add(&g_cps_run, 1);
            if (from_run && g_compact_enabled && atomic_load(&g_warm_running)) arch_harvest(d, 1);
        } }
        atomic_fetch_add(&g_cps_look_ns, bp_now() - t0);
        if (ps) return ps;
    }
    uint64_t t1 = bp_now();
    ps = dev_cps(fn_dev_b(f, off), e);
    if (ps) { atomic_fetch_add(&g_cps_built, 1); atomic_fetch_add(&g_cps_build_ns, bp_now() - t1); arch_harvest(d, 0); }
    return ps;
}
static void arch_compact_finish(void);
static void arch_write_file(ArchEpoch *ep);
static void arch_write(ArchEpoch *ep) {
    arch_write_file(ep);
    if (ep->compacting && atomic_fetch_sub(&g_compact_periods, 1) == 1 && atomic_load(&g_compact_closed)) arch_compact_finish();
}
static void arch_write_file(ArchEpoch *ep) {
    int n = atomic_load(&ep->n), vd = atomic_load(&ep->vd), cs = atomic_load(&ep->cs), ms = atomic_load(&ep->ms);
    if (!n) return;
    int plain = ser_begin();   // a plain compile may report service death only after Metal's retries: an expired wait cannot certify this period
    int crashed = atomic_load(&ep->crashed) || ep->deaths != atomic_load(&g_arch_deaths);
    if (crashed || plain) {
        ser_end();
        atomic_fetch_add(&g_arch_discarded, 1);
        arch_pkg_period_done(ep, 0);
        atomic_fetch_sub(&g_arch_new, n); atomic_fetch_sub(&g_arch_new_vd, vd);
        if (ep->compacting) g_compact_bad = 1;
        if (crashed) MLOG("[RED] pipeline archive: a flush period the compiler service died in is not written (%d pipelines: %d bases, %d compute, %d task / mesh); they are captured again at a later start",
                          n, n - cs - ms, cs, ms);
        else MLOG("pipeline archive: a flush period with %d unfinished plain compiles is not written (%d pipelines); their errors are not known yet, the pipelines are captured again at a later start", plain, n);
        return;
    }
    [NSFileManager.defaultManager createDirectoryAtPath:g_arch_dir withIntermediateDirectories:YES attributes:nil error:nil];
    uint64_t t0 = bp_now(); NSError *e = nil;
    NSString *path = [g_arch_dir stringByAppendingPathComponent:vd || cs ? [NSString stringWithFormat:@"%05d-%d.mtl4a", g_arch_seq++, n] : [NSString stringWithFormat:@"p%05d.mtl4a", g_arch_seq++]], *tmp = [path stringByAppendingString:@".tmp"];
    if (g_pre_out) prewarm_script(ep->ser, path);
    if (ep->compacting) arch_mark(1);
    BOOL ok = [ep->ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:tmp] error:&e];
    if (ep->compacting) arch_mark(0);
    ser_end();
    atomic_fetch_sub(&g_arch_new, n); atomic_fetch_sub(&g_arch_new_vd, vd);
    // its index first: a keys file without its archive file is removed at the next start
    NSString *kp = arch_keys_path(path); int keyed = ok && arch_keys_write(kp, ep->keys, !atomic_load(&ep->unkeyed));
    ok = ok && !rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation);
    if (!ok && keyed) unlink(kp.fileSystemRepresentation);
    if (!ok || !keyed) atomic_fetch_add(&g_arch_discarded, 1);
    arch_pkg_period_done(ep, ok && keyed);
    if (!ok) { MLOG("[RED] pipeline archive: writing %s failed%s%s", path.UTF8String, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); unlink(tmp.fileSystemRepresentation); if (ep->compacting) g_compact_bad = 1; }
    else {
        MLOG("pipeline archive: %d new pipeline bases%s%s written to %s (%.1f MB, %.0f ms; compiles held back by the writes so far: %llu, %.0f ms in all)", n - cs - ms,
             cs ? [NSString stringWithFormat:@" and %d compute pipelines", cs].UTF8String : "", ms ? [NSString stringWithFormat:@" and %d task / mesh pipelines", ms].UTF8String : "",
             path.lastPathComponent.UTF8String, [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize] / 1048576.0, (bp_now() - t0) / 1e6,
             (unsigned long long)atomic_load(&g_ser_holds), atomic_load(&g_ser_held_ns) / 1e6);
    }
}
// A flush hands later compiles to a new period and writes this one once its compiles return; returns at once, any thread. only: that period alone, while current; any_time: also during the prewarm.
// During a prewarm the game plays through, a timer flush (DM_PSO_ARCH_WARM_FLUSH_MS) waits for the period's age, doubling up to 16 times (docs/reference/rendering.md, A cold start leaves a file per timer flush, and the next start compacts them all).
static int g_warm_flush_ms = -1; static _Atomic int g_warm_flushes;
static int arch_warm_flush_due(void) {
    if (g_warm_flush_ms < 0) { const char *e = getenv("DM_PSO_ARCH_WARM_FLUSH_MS"); g_warm_flush_ms = e && *e ? MAX(atoi(e), 0) : 60000; }
    if (!g_warm_flush_ms || g_compact_enabled || !atomic_load(&g_warm_bg)) return 0;
    uint64_t wait_ns = ((uint64_t)g_warm_flush_ms << MIN(atomic_load(&g_warm_flushes), 4)) * NSEC_PER_MSEC;
    return bp_now() - atomic_load(&g_ep_t0) >= wait_ns;
}
static void arch_hand_over(ArchEpoch *only, int any_time) {
    int warm_due = 0;
    if (!g_arch_cap || (!only && !any_time && atomic_load(&g_warm_running) && !(warm_due = arch_warm_flush_due()))) return;
    // a period older than a compiler death is handed over though empty: a new one has a live compiler
    os_unfair_lock_lock(&g_ep_lock);
    int stale = atomic_load(&g_ep->crashed) || g_ep->deaths != atomic_load(&g_arch_deaths);
    int any = (!only || g_ep == only) && (atomic_load(&g_ep->n) || atomic_load(&g_ep->active) || stale);
    os_unfair_lock_unlock(&g_ep_lock);
    // nothing captured or under way: the period goes on. A compaction due to close closes all the same (left open, the next start would compact again).
    if (!any) {
        if (!only && g_compact_enabled && atomic_load(&g_compacted) && !atomic_load(&g_warm_running) && !atomic_load(&g_compact_harvests) && !atomic_exchange(&g_compact_closed, 1))
            dispatch_async(g_arch_q, ^{ if (!atomic_load(&g_compact_periods)) arch_compact_finish(); });
        return;
    }
    ArchEpoch *ne = ep_new();
    if (!ne) { MLOG("[RED] pipeline archive: Metal made no compiler for a new flush period: the captures go on into the current one"); return; }
    os_unfair_lock_lock(&g_ep_lock);
    ArchEpoch *old = g_ep; any = (!only || old == only) && (atomic_load(&old->n) || atomic_load(&old->active) || atomic_load(&old->crashed) || old->deaths != atomic_load(&g_arch_deaths));
    if (any) {
        g_ep = ne; atomic_store(&g_ep_t0, bp_now());
        if (warm_due) atomic_fetch_add(&g_warm_flushes, 1);
        if (g_compact_enabled && atomic_load(&g_compacted) && !atomic_load(&g_compact_closed)) {
            old->compacting = 1; atomic_fetch_add(&g_compact_periods, 1);
            if (!atomic_load(&g_warm_running) && !atomic_load(&g_compact_harvests)) atomic_store(&g_compact_closed, 1);
        }
    }
    os_unfair_lock_unlock(&g_ep_lock);
    if (!any) return;
    dispatch_group_enter(g_arch_pending);
    dispatch_group_notify(old->busy, g_arch_q, ^{ @autoreleasepool { arch_write(old); } dispatch_group_leave(g_arch_pending); });
}
static void arch_flush_from(ArchEpoch *only) { arch_hand_over(only, 0); }
static void arch_flush(void) { arch_flush_from(nil); }
// Writes what is captured and not written (docs/reference/rendering.md, A run that ends in its prewarm loses what it compiled). Background: wait_ms 0, written behind (.tmp then rename).
// Quitting: waits up to wait_ms for the period's compiles, the harvest queue and the writes; a write the exit cuts short is no Metal crash (arch_mark).
void mtl_arch_save(int wait_ms, int ending) {
    if (!g_arch_cap) return;
    uint64_t t0 = bp_now(); dispatch_time_t until = dispatch_time(DISPATCH_TIME_NOW, (int64_t)wait_ms * NSEC_PER_MSEC);
    long late = wait_ms > 0 && arch_harvest_wait(until);
    int n = atomic_load(&g_arch_new);
    arch_hand_over(nil, 1);
    if (!wait_ms && !ending) return;
    if (n && wait_ms > 0) MLOG("pipeline archive at exit: writing %d pipelines (up to %d ms)", n, wait_ms);
    late = (wait_ms > 0 && dispatch_group_wait(g_arch_pending, until)) || late;
    if (ending) arch_mark(-1);
    if (n || late) MLOG("pipeline archive at exit: %d pipelines not written yet, %s (%d left, %.0f ms)", n,
                        late ? "not all written in time: those left are built again at the next start" : !wait_ms ? "not waited for" : "written", atomic_load(&g_arch_new), (bp_now() - t0) / 1e6);
}
static void arch_at_exit(void) { mtl_warm_stop(); mtl_arch_save(MTL_QUIT_ARCH_MS, 1); }
static void arch_flush_wait(void) { arch_harvest_wait(DISPATCH_TIME_FOREVER); arch_flush(); if (g_arch_pending) dispatch_group_wait(g_arch_pending, DISPATCH_TIME_FOREVER); }
// The prebuild tool: captures again what dropped periods lost, once the writes are done (never on g_arch_q), up to g_arch_pkg_max_retries rounds. The pipelines known to
// build go first, in batches; each lost to a compiler death then gets a period of its own, so one that kills the compiler every time loses no other. Returns the
// pipelines still not captured (those Metal refuses aside).
static int arch_pkg_retry(ArchPkgItem *it, int want) {
    MTL4PipelineDescriptor *d = nil; int vd = 0, compute = 0;
    os_unfair_lock_lock(&g_pkg_lock);
    if (it->state == want && it->desc && it->retries < g_arch_pkg_max_retries) { it->retries++; it->state = ARCH_PKG_PENDING; d = it->desc; vd = it->vd; compute = it->compute; }
    os_unfair_lock_unlock(&g_pkg_lock);
    if (!d) return 0;
    @autoreleasepool {
        NSError *e = nil; int cap = 0;
        if (compute) (void)arch_capture_cs((MTL4ComputePipelineDescriptor *)d, &e, &cap);
        else (void)arch_capture_try(d, &e, vd, &cap, g_fn_wait_ms, NULL);
    }
    return 1;
}
static int arch_pkg_recover(void) {
    if (!arch_pkg_on()) return 0;
    arch_flush_wait();
    int rounds = 0;
    while (arch_capturing()) {
        NSMutableArray<ArchPkgItem *> *buildable = [NSMutableArray array], *unconfirmed = [NSMutableArray array];
        os_unfair_lock_lock(&g_pkg_lock);
        for (ArchPkgItem *it in g_pkg_items.allValues) {
            if (!it->desc || it->retries >= g_arch_pkg_max_retries) continue;
            if (it->state == ARCH_PKG_LOST_BUILDABLE) [buildable addObject:it];
            else if (it->state == ARCH_PKG_LOST_UNCONFIRMED) [unconfirmed addObject:it];
        }
        os_unfair_lock_unlock(&g_pkg_lock);
        if (!buildable.count && !unconfirmed.count) break;
        rounds++;
        MLOG("pipeline prebuild: capturing again what dropped periods lost, round %d of %d (%lu known to build, %lu lost to a compiler death)",
             rounds, g_arch_pkg_max_retries, (unsigned long)buildable.count, (unsigned long)unconfirmed.count);
        if (buildable.count) {
            for (ArchPkgItem *it in buildable) { if (!arch_capturing()) break; arch_pkg_retry(it, ARCH_PKG_LOST_BUILDABLE); }
            arch_flush_wait();
        }
        for (ArchPkgItem *it in unconfirmed) {
            if (!arch_capturing()) break;
            if (arch_pkg_retry(it, ARCH_PKG_LOST_UNCONFIRMED)) arch_flush_wait();
        }
    }
    g_pkg_last_rounds += rounds;
    int missing = atomic_load(&g_pkg_unkeyed_lost);
    os_unfair_lock_lock(&g_pkg_lock);
    for (ArchPkgItem *it in g_pkg_items.allValues) if (it->state != ARCH_PKG_WRITTEN && it->state != ARCH_PKG_REJECTED) missing++;
    os_unfair_lock_unlock(&g_pkg_lock);
    return missing;
}
static void arch_note_new(void) {
    if (!g_arch_q || atomic_exchange(&g_arch_sched, 1)) return;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC), g_arch_q, ^{ atomic_store(&g_arch_sched, 0); arch_flush();
        if (atomic_load(&g_warm_running) && atomic_load(&g_warm_bg) && atomic_load(&g_arch_new) > 0) arch_note_new(); });
}
static void arch_compact_finish(void) {
    if (!g_compact_files || !g_compacted) return;
    if (g_compact_bad) { MLOG("[RED] pipeline archive: compaction left the old files (a flush failed, or a base was not captured again)"); g_compact_files = nil; return; }
    int gone = 0; for (NSUInteger i = 0; i < g_compact_files.count; i++) if (!g_compact_state || g_compact_state[i]) { unlink(g_compact_files[i].fileSystemRepresentation); unlink(arch_keys_path(g_compact_files[i]).fileSystemRepresentation); gone++; }
    MLOG("pipeline archive compacted: %d files of bases with a vertex layout gave %d bases to the files just written and were removed", gone, (int)g_compacted);
    g_compact_files = nil;
}
static void pso_col4(MTL4RenderPipelineColorAttachmentDescriptor *a, MTLRenderPipelineColorAttachmentDescriptor *c) {
    a.pixelFormat = c.pixelFormat; a.writeMask = c.writeMask; a.blendingState = c.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
    a.sourceRGBBlendFactor = c.sourceRGBBlendFactor; a.destinationRGBBlendFactor = c.destinationRGBBlendFactor; a.rgbBlendOperation = c.rgbBlendOperation;
    a.sourceAlphaBlendFactor = c.sourceAlphaBlendFactor; a.destinationAlphaBlendFactor = c.destinationAlphaBlendFactor; a.alphaBlendOperation = c.alphaBlendOperation;
}
