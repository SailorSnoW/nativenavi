// Metal 4 pipelines and the game's archive of their bases: flush periods, the archive's index, the bundled and prewarm files, compaction, the compiler service's deaths.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---- Metal 4 pipelines: a base per pipeline with its colour state unspecialized, a per-game archive of the bases ----
// pso_desc stays the description of a pipeline. pso_build turns it into an MTL4RenderPipelineDescriptor whose colour attachments (format, blending, factors, operations, write mask)
// are unspecialized, builds that base once (MTL4Compiler, or takes it from the game's pipeline archive) and makes each pipeline from it by specialization: about 4 µs on an M2 Max,
// where building a pipeline whose colour state alone differs took 3-5 ms. Breath of the Wild's 4,829 recorded pipelines came from 1,818 such bases. Metal 4 pipelines name no
// depth / stencil format, so a base serves every depth / stencil attachment.
// The archive keeps the bases beside pso_user.tsv, in pso_archive/: files written by an MTL4PipelineDataSetSerializer that captures binaries only (with the descriptor capture as
// well it writes no archive and reports no error). Each serialization writes what was captured since the one before, so the folder holds one file per flush; admitted files are opened
// at start, and a base found in one is taken from it directly (MTL4Archive), not through the compiler's lookupArchives, which would capture it again into the next file. A base
// taken from an archive costs about 0.06 ms, against 10-80 ms to build one.
// DM_PSO_MTL4=0: the MTLDevice path as before (no bases, no archive). DM_PSO_ARCHIVE=off: no archive; DM_PSO_ARCHIVE=<folder>: the archive in that folder.
// On by default, and with it the hybrid pipelines and the pipeline prebuild. The archive's footprint
// was about 850 MB more for Breath of the Wild with the prebuild. arch_admit checks address space before any archive or compiler is opened.
// The prebuild's bases can also come made offline, bundled with the
// app (DM_PSO_BUNDLE, pso_bundle_open). newArchiveWithURL maps a file on the Mac's internal disk (clean pages, not in the footprint) and reads one on an external drive into memory, whole and for the
// whole run (Kirby Star Allies: 1.13 GB), and it offers no choice: docs/reference/memory.md
static int pso4_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_PSO_MTL4");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_PSO_MTL4=0: render pipelines are built through the MTLDevice (no bases, no pipeline archive)");
        // Metal cannot capture pipelines made from bases ("Capturing Render pipeline specialization is not supported"), and its Metal 4 → 3 conversion for the capture layer
        // crashes on the archives: a run that Metal lets capture GPU frames builds its pipelines through the MTLDevice. Metal answers that itself: a trace document
        // when MTL_CAPTURE_ENABLED reads as a non-zero number (1 or 2; unset, empty, 0, "true" or "YES" do not),
        // and the developer tools when Xcode started the run for capture. DM_GPU_CAPTURE alone cannot capture (gpu_capture_frame's [RED] line), and DM_CTL can set it later in
        // a run that can, so it does not decide this. Asked once: the capture manager's answer does not change during a run
        if (v) {
            MTLCaptureManager *m = [MTLCaptureManager sharedCaptureManager];
            int trace = [m supportsDestination:MTLCaptureDestinationGPUTraceDocument], tools = [m supportsDestination:MTLCaptureDestinationDeveloperTools];
            if (trace || tools) { v = 0; MLOG("render pipelines through the MTLDevice: Metal may capture GPU frames in this run (%s)", tools ? "Xcode" : "MTL_CAPTURE_ENABLED"); }
            else MLOG("render pipelines from Metal 4 bases and the pipeline archive (Metal cannot capture GPU frames in this run)");
        }
    }
    return v;
}
// g_comp4: the compiler of what the archive does not capture (specializations, and every compile without an archive folder); compiles the archive captures go through the current
// flush period's own compiler (ArchEpoch)
static id<MTL4Compiler> g_comp4;
// g_arch4: the files that may hold bases with a vertex layout, g_arch4p: the files that hold none (p<seq>.mtl4a: the prebuild's, and any flush whose bases all read their vertices
// themselves). A base with a layout is looked up in g_arch4 alone: each file a lookup misses in costs about 0.06 ms, and a cold start's misses were for such bases, in the 13 prebuilt files
static NSArray<id<MTL4Archive>> *g_arch4, *g_arch4p; static NSString *g_arch_dir; static int g_arch_seq;
// Compaction: every start writes a file or more (each flush captures only what is new), Breath of the Wild 5-7 per start, Ocarina of Time 13-273 in a play session. A file with
// an index (see arch_key) costs a lookup nothing, one without (written before the index) about 0.06 ms for each lookup it misses, and a start opens every file (about 15 ms each,
// in parallel) and the disk keeps them. Run files are taken by size tier (arch_tier, from the pipeline count in the name), each PSO_ARCH_COMPACT times the one below; a full
// period's file is in none. When a tier has more than PSO_ARCH_COMPACT files, the bases the prewarm takes from it and every smaller tier are captured again from the original shader
// functions (the #342 workaround, a full compile each), so the flush after the prewarm writes them into one file, or one per ARCH_PERIOD_MAX of them; once every period that may
// hold them is written, those files and their indexes are removed (open archives stay usable for the rest of the run). With every run file in it, a start after a few files of play
// captured the whole archive again. Bases the prewarm did not take (pso_user.tsv never named them) go with them and are built again when drawn. The prebuild's files (p<seq>) are
// left as they are
enum { PSO_ARCH_COMPACT = 8 };
static int g_compact_enabled;   // immutable after pso4_init; unlike g_compact_files, never cleared by the archive queue
static _Atomic int g_compact_bases; static _Atomic uint64_t g_compact_ns;   // render bases captured again and their time (the prepare line)
static NSArray<NSString *> *g_compact_files; static _Atomic int g_compacted;
static unsigned char *g_compact_state;   // per run file of g_compact_files (g_arch4's order): 1 when the compaction replaces it; immutable after pso4_init
// g_compact_periods: periods that may hold recaptured bases, handed over and not written yet; g_compact_closed: the last of them is handed over (the first flush after the prewarm);
// g_compact_bad: one was not written, or a base the prewarm took from an old file was not captured again: the old files stay (arch_compact_finish); g_compact_harvests: recaptures
// queued on the harvest queue (arch_harvest) and not in a period yet: the compaction closes only once there are none, so a late one is in a compacting period and its failure keeps the
// old files (before, the flush after the prewarm closed it while some were queued: one that failed then found its old file removed, and was compiled again at the next start)
static _Atomic int g_compact_periods, g_compact_closed, g_compact_bad, g_compact_harvests;
// Keep the crash-recovery guard if a compaction still fails in Metal. Markers name the OS build and capture method: a marker from the old archive-import path must not
// disable the safer original-function capture. A failed flush of this method disables compaction on this OS build until it changes; ordinary archive reads remain available.
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
// The marker around a compacting period's write (arch_write_file): on 1 writes it, 0 removes it once the write returned. -1: the process is ending (mtl_arch_save), so a write
// it cuts short is no crash in Metal: this run's marker goes (one a crashed run left stays) and none is written again
static os_unfair_lock g_mark_lock = OS_UNFAIR_LOCK_INIT; static int g_mark_mine, g_mark_off;
static void arch_mark(int on) {
    os_unfair_lock_lock(&g_mark_lock);
    if (on > 0 && !g_mark_off) { [arch_compact_stamp() writeToFile:arch_compact_marker() atomically:YES encoding:NSUTF8StringEncoding error:nil]; g_mark_mine = 1; }
    else if (on <= 0 && g_mark_mine) { unlink(arch_compact_marker().fileSystemRepresentation); g_mark_mine = 0; }
    if (on < 0) g_mark_off = 1;
    os_unfair_lock_unlock(&g_mark_lock);
}
static dispatch_queue_t g_arch_q; static _Atomic int g_arch_new, g_arch_new_vd, g_arch_sched;   // bases captured and not written yet, those of them with a vertex layout
// A flush period: a compiler of its own and the serializer it captures into. A compile the archive captures joins the current period (ep_enter) and leaves it when it returns; a
// flush (arch_flush) hands the compiles after it to a new period and writes the old period's serializer once every compile in it has returned (arch_write), and that serializer is
// never used again. In Metal (macOS 27.0) the write (-[_MTLBinaryArchive airntSerializeToURL:options:error:]) runs on the archive's _serializeQueue and then empties it (entries,
// AIR objects, library cache, file mapping) on its _queue, while each function a compile captures adds itself from the compile's completion: a new MTLBinaryEntry from the
// request's AIR script and the function's bitcode, mapped before newArchiverIdWithBinaryKey takes _queue, then added under _queue, before the compile reports back. A compile
// under way across a write came back with both nil and crashed there (dispatch_data_create_map on nil); eight threads compiling into one serializer written every 150 ms
// crashed 3 runs of 3, and still 6 of 6 when the write waited for the compiles under way but let new ones start; a serializer per period crashed in none of 118 runs, every
// pipeline found in the files. The flush used to hold a lock against every such compile instead, and one compile that did not return (Super Mario Odyssey's alRenderMaterialUber
// with the front end at -O2: 33.7 minutes until Metal gave up, GitHub issue #294) held the flush, and the flush every compile and draw that came after it. Now such a compile
// holds back the file of its own period and nothing else
static _Atomic uint64_t g_arch_deaths;   // all unwritten periods predating a compiler-service death are untrusted, including ones already handed over
@interface ArchEpoch : NSObject { @public id<MTL4Compiler> comp; id<MTL4PipelineDataSetSerializer> ser; dispatch_group_t busy; _Atomic int active, n, vd, cs, ms, failed, crashed; int compacting; uint64_t deaths;
    NSMutableData *keys; os_unfair_lock klock; _Atomic int unkeyed; } @end
// n: pipelines captured; vd, cs, ms: those of them that are bases with a vertex layout, compute pipelines, native mesh pipelines; failed: a capture in it failed (arch_cap_failed); crashed: the compiler service died
// while it was current or under one of its captures (arch_died, arch_capture_try, arch_capture_cs); deaths: service deaths when created (arch_write_file rejects older periods); compacting: it may hold
// recaptured bases (arch_flush); keys: the index keys of what it captured (arch_key, under klock), unkeyed: it captured a pipeline with no key, so its index is not complete
@implementation ArchEpoch @end
// The archive's index. Each file of the archive gets a list of the pipelines in it beside it (<file>.keys: arch_write_file), and a start reads every list (pso4_init), so a lookup asks
// the one file that holds the pipeline and a miss asks none. Before, a lookup asked every file in turn, about 0.06 ms each one the pipeline is not in, so each flush's file made every
// later lookup dearer, and more than PSO_ARCH_COMPACT files made a start a compaction: one that compiles every base again (the #342 workaround, a full compile each; Ocarina of Time
// on an M4: 8,859 bases, 762 s before the first frame, GitHub issue #1747 and PR #1880). A pipeline's key is what its Metal 4 descriptor sets, digested (SHA-256, first 8 bytes): its
// functions by library (a digest of its bytes: lib_tag), entry name and constant values (akb_fn), the vertex layout, the colour attachments and every other field the descriptor has, so the same
// pipeline has the same key at every start, and two that Metal tells apart have two. A field the host starts setting must go in the key too, or two pipelines that differ only there
// share a key: the index names one's file for both, and the other misses there, is compiled and written again at every start. A key the index names a file for is looked up in that
// file alone: a miss there (the file's binaries no longer fit this system, a library changed under the same name) compiles it, and its new file, newer, holds it from the next start
// on. 0: no key (a function from a library with no name: the diagnostic overrides, embedded kernels), looked up in the files without a complete index
typedef struct { CC_SHA256_CTX c; int ok; } arch_kb;
static void akb_u(arch_kb *k, uint64_t v) { CC_SHA256_Update(&k->c, &v, sizeof v); }
static void akb_s(arch_kb *k, NSString *s) { const char *u = s.UTF8String ?: ""; size_t n = strlen(u); akb_u(k, n); CC_SHA256_Update(&k->c, u, (CC_LONG)n); }
static void akb_size(arch_kb *k, MTLSize s) { akb_u(k, s.width); akb_u(k, s.height); akb_u(k, s.depth); }
static void akb_fn(arch_kb *k, MTL4FunctionDescriptor *f) {
    if (!f) { akb_u(k, 0); return; }
    if ([f isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) {
        // its constant values by their values: Metal has no getter for them, and the descriptor copies the set it is given, but the set's class compares, hashes and describes them
        // by value (macOS 27.2: the same in every process and in whatever order they were set); the description lists every index and named constant with its type and value after
        // the line naming the object's address
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
    k->ok = 0;   // another kind of function descriptor: no name to give it
}
static void akb_linked(arch_kb *k, MTL4StaticLinkingDescriptor *s) { if (s.functionDescriptors.count || s.privateFunctionDescriptors.count || s.groups.count) k->ok = 0; }   // the host links none
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
// <file>.keys: "DMPSOIX2", then complete (1: every pipeline in the file has its key here) and the count (two uint32), then the keys (uint64 each). "DMPSOIX1": keys that named a
// library by its file (before lib_tag took its bytes), which no lookup computes now: such a file is reached through the index alone, so pso4_init removes it (arch_keys_old)
static NSString *arch_keys_path(NSString *archive) { return [archive.stringByDeletingPathExtension stringByAppendingString:@".keys"]; }
static int arch_keys_write(NSString *path, NSData *keys, int complete) {
    uint32_t head[2] = {complete != 0, (uint32_t)(keys.length / sizeof(uint64_t))};
    NSMutableData *d = [NSMutableData dataWithBytes:"DMPSOIX2" length:8]; [d appendBytes:head length:sizeof head]; if (keys) [d appendData:keys];
    return [d writeToFile:path atomically:YES];   // a temporary file renamed into place
}
static NSData *arch_keys_read(NSString *path, int *complete) {   // nil: no index, or not one this code wrote
    NSData *d = [NSData dataWithContentsOfFile:path]; uint32_t head[2];
    if (d.length < 16 || memcmp(d.bytes, "DMPSOIX2", 8)) return nil;
    memcpy(head, (const uint8_t *)d.bytes + 8, sizeof head);
    if (head[0] > 1 || d.length != 16 + (uint64_t)head[1] * sizeof(uint64_t)) return nil;
    *complete = head[0] == 1;
    return [d subdataWithRange:NSMakeRange(16, d.length - 16)];
}
static int arch_keys_old(NSString *path) {   // an index of keys by library file (DMPSOIX1)
    char m[8]; int fd = open(path.fileSystemRepresentation, O_RDONLY); if (fd < 0) return 0;
    ssize_t n = read(fd, m, sizeof m); close(fd);
    return n == (ssize_t)sizeof m && !memcmp(m, "DMPSOIX1", 8);
}
// g_arch_ix: key → the newest file whose index names it, filled in pso4_init and read only after (so with no lock). g_arch_ixf: the files with a complete index, which only the
// index leads to (it holds them; g_arch4 and g_arch4p hold the files without one, and those whose index is not complete). g_arch_run: every plain run file, in g_compact_files'
// order (arch_compact_takes). g_arch_asks: archive files asked for a pipeline (the tests)
static nvn_hmap g_arch_ix = {.klen = sizeof(uint64_t)};
static NSArray<id<MTL4Archive>> *g_arch_ixf, *g_arch_run;
static _Atomic uint64_t g_arch_asks;
// The device's archive files opened at start, with how many pipelines were taken from each (arch_lookup, all threads): arch_sweep moves out those none was taken from once every
// pipeline a start can want was asked for. A pipeline's key covers its libraries' bytes, so a chain change, a profile edit or a host change that alters a library or a descriptor
// leaves the old pipelines where no lookup reaches them, and before this nothing removed them: Ocarina of Time's archive held about 33,700 indexed pipelines against a prewarm of
// under 10,000 (13.9 GB). A file with some live pipelines stays whole; compaction drops its dead ones when it rewrites it
@interface ArchUse : NSObject { @public NSString *path; __weak id<MTL4Archive> a; int pf, gone; uint64_t hits, keys; } @end   // keys: the pipelines its index names (0: none)
@implementation ArchUse @end
static char g_arch_use_tag; static NSArray<ArchUse *> *g_arch_use;
static _Atomic int g_sweep_warm, g_sweep_pairs;   // the prewarm lists' pipelines, and every shader pair's pulling base (the prebuild), were asked for in this run
static void arch_used(id<MTL4Archive> a) { ArchUse *u = objc_getAssociatedObject(a, &g_arch_use_tag); if (u) __atomic_fetch_add(&u->hits, 1, __ATOMIC_RELAXED); }
// An archive file this host no longer uses, with its index: moved into DM_OBSOLETE's folder for this run, <DM_OBSOLETE>/<date time> <archive folder's name>/, which the packing script's
// next successful build empties (the Mac run command sets DM_OBSOLETE to <id>_build/obsolete, so what is unused is in one folder to look at or delete), else deleted (the app: nobody
// reaches its files, and a move to another volume would be a copy). Returns 1 when it has left the archive
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
// A pipeline from the archive: the file the index names for its key alone, else each file without a complete index (the bundle first among the p files, which plain skips: a base
// with a vertex layout and a compute pipeline are never in one). *from_run: it came from a run file this compaction takes, so the compacting prewarm captures it again
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
static ArchEpoch *g_ep; static os_unfair_lock g_ep_lock = OS_UNFAIR_LOCK_INIT;   // the current period
static _Atomic uint64_t g_ep_t0;   // when the current period began (bp_now): a prewarm's timer flush hands it over once it is old enough (arch_warm_flush_due)
static int g_arch_cap;   // compiles are captured (an archive folder, a serializer): immutable after pso4_init
static dispatch_group_t g_arch_pending;   // periods handed over and not written yet
static _Atomic uint64_t g_pb_built, g_pb_archived, g_pb_reused, g_pb_spec, g_pb_legacy, g_pb_base_ns;
// Time by kind, all threads (the 'pipelines up to frame' line): archive lookups that found the base / found none, specializations, base compiles; predictions (submit thread)
static _Atomic uint64_t g_pt_hit_n, g_pt_hit_ns, g_pt_miss_n, g_pt_miss_ns, g_pt_spec_ns, g_pt_comp_ns, g_pt_pred_n, g_pt_pred_ns, g_pp_done_n;   // bases compiled / taken from the archive / reused, specializations, MTLDevice builds
typedef struct { const void *vf, *ff; uint32_t samples, a2c, rast, topo, amp, off; uint32_t va[31][3], vl[31][3]; } pbase_key;   // off: its functions without the border corrections (fn_off);   // what a base depends on: pso_desc's descriptor less the colour attachments
static nvn_hmap g_pbase = {.klen = sizeof(pbase_key)};
// Bases the archives do not have. pso4_init opens the archives once and nothing adds to them in a run, so a miss holds for the process: a later call for the base skips the search
// (a compile 0 one returns nil, a compiling one goes straight to the compiler), and g_pbase, looked at first, takes the base once it is built. Without it every draw of a pipeline
// whose build the full table refused (pso_pend_demand) searched every archive again while it drew with its stand-in: Ocarina of Time's castle town, 9,208 searches / 30.5 s in
// 300 frames, CPU translation 114 ms a frame (PR #1825)
static nvn_hmap g_pbase_miss = {.klen = sizeof(pbase_key)};
// Only callers for the same base wait on this group; unrelated bases compile in parallel.
// compiled: its owner asked the compiler (a compile 0 owner only looks in the archive, and a waiter that needs the base builds it itself when that found none).
// looked: left once the owner has looked in the archives (or skipped them for a base known missing), found: the base that found, set before; a compile 0 waiter waits on it
@interface PbaseBuild : NSObject { @public dispatch_group_t done, looked; id<MTLRenderPipelineState> ps, found; NSError *error; int compiled; } @end
@implementation PbaseBuild @end
static nvn_hmap g_pbase_pending = {.klen = sizeof(pbase_key)};
static os_unfair_lock g_pbase_lock = OS_UNFAIR_LOCK_INIT;
static NSString *pso_user_path(void);
// The pipeline archive made offline (dm/gfx/tools/pso_offline.py, metal-tt) and bundled with the app: DM_PSO_BUNDLE=<folder> (the Mac app's launcher sets it to its Resources/pso folder when it has one;
// off: not used) holds <GPU architecture>.mtl4a per architecture it was built for and prebuilt.tsv, what it covers (pso_prebuild). The file named after this device's architecture is
// opened read only and looked up first among the files of bases without a vertex layout; it is never written or compacted. The device's own archive (pso_archive/) so takes only
// what it lacks: a draw, the prewarm and the prebuild compile a base, and the serializer captures it, only after the bundle missed (it holds no base with a vertex layout). A system
// that cannot open it (one older than the archive's minimum, or an OS update that changed the GPU compiler) or a GPU it was not built for gets an info line, and the device builds
// the bases itself as before (the on-device prebuild is the fallback)
static id<MTL4Archive> g_arch_bundle; static NSString *g_bundle_dir;
// The prewarm lists' pipelines (the R rows' bases with a vertex layout, and the mesh pipelines, which the archive above does not hold), made offline by the host itself (the prebuild
// tool's bundle form, mtl_pso_prebuild_tool with DM_PSO_PREBUILD_OUT, the packing script's --prebake) and bundled beside it as <GPU architecture>-prewarm-<NN>.mtl4a: Metal's serializer
// store fills past about 2,000 captures (MDB_MAP_FULL, ARCH_PERIOD_MAX), so the tool writes one file per flush period, as a run does, with the index beside it (<file>.keys,
// arch_write_file). They are opened with the bundle, before the device's files: their index goes into g_arch_ix first, so a lookup asks the one file that holds the base, and a file
// with no usable index is searched first among the files of bases with a vertex layout (g_arch4). With them a first start's prepare step finds its R rows' bases and compiles none
// (Ocarina of Time, 40,000 lines: 6,400 bases, 450-475 s without, seconds with).
// g_pre_tool: the prebuild tool is running (nothing retained, no specialization, and the bundle's prewarm files, its own earlier output, are not opened); g_pre_out: the bundle form's
// output folder (each flush period is also written as a pipelines script); g_prewarm_files: the prewarm files opened
static int g_pre_tool; static NSString *g_pre_out; static _Atomic int g_pre_warm_failed;
static NSUInteger g_prewarm_files; static NSMutableArray<id<MTL4Archive>> *g_prewarm_open;   // g_prewarm_open: the prewarm files, which g_arch_ix names without owning them
// The ranges of an archive file that Metal reads when it opens it (arch_open): rd collects them while the container is checked, rd == NULL only checks
typedef struct { uint64_t off[64], len[64]; int n; } arch_reads;
static void arch_reads_add(arch_reads *rd, uint64_t off, uint64_t len) { if (rd && len && rd->n < 64) { rd->off[rd->n] = off; rd->len[rd->n++] = len; } }
// Metal's loader crashes on malformed containers rather than returning an error. Check the GPU image's load commands and referenced file ranges, including every slice of a
// universal archive, before giving it to Metal. This checks the container, not the compiler's private payload: Metal still decides whether this system can use its binaries.
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
            // what the open reads: every segment but __TEXT (the pipelines' records, __AIR_DATA), and in __TEXT the libraries (__metallib); the GPU code is read when a pipeline is taken
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
    // The serializer's 0xcbfebabe wrapper uses fat_arch entries and includes a Metal library slice beside the GPU images.
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
        if (metal && magic == 0x424c544d) {   // MTLB: its header declares the full slice length and four section ranges
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
static uint64_t arch_file_size(NSString *path, arch_reads *rd) {   // validated file size, or 0; rd: what the open reads, or NULL
    int fd = open(path.fileSystemRepresentation, O_RDONLY); if (fd < 0) return 0;
    struct stat st; int ok = !fstat(fd, &st) && S_ISREG(st.st_mode) && st.st_size > 0 && arch_container_ok(fd, (uint64_t)st.st_size, rd);
    close(fd); return ok ? (uint64_t)st.st_size : 0;
}
static int arch_file_check(NSString *path, arch_reads *rd) { return arch_file_size(path, rd) != 0; }
static int arch_file_ok(NSString *path) { return arch_file_check(path, NULL); }
// Opens an archive file once arch_file_ok has passed it; nil with *err (or nil when the file is no archive). Metal's open (MTL4ArchiveImpl::loadFromURL, macOS 27) makes a library of
// every Metal library in the file and hashes all of it (SHA-256), on the calling thread: it reads the whole __AIR_DATA segment and __metallib section of every GPU image (the GPU
// code it leaves until a pipeline is taken). From a mapping not read yet, that is one 16 KB page fault after another: Ocarina of Time's bundled archive (4.4 GB, 2.9 GB of
// libraries) took 12.5 s on an M4 Max with the file not in memory, 1.5 s with it. So those ranges are asked for first, 8 MB at a time on every core, and Metal finds them in
// memory: 1.6 s. Each piece is mapped on its own, given MADV_WILLNEED and unmapped: the kernel reads it into the file cache, which Metal's mapping shares, with no buffer of ours
// and no copy when the pages are already in memory. The first version read the pieces into 8 MB heap buffers (the 1.6 s): as fast on an M6, but malloc kept the freed buffers,
// +99 MB of footprint on its 12 cores. Only a piece at a time is mapped,
// as Metal maps the whole file too. If the system ignores the advice, Metal reads as before.
// The device's own files are opened in parallel (pso4_init) and each is read ahead the same way
static id<MTL4Archive> arch_open(NSString *path, NSError **err) {
    arch_reads rd = {0};
    if (!arch_file_check(path, &rd)) return nil;
    int fd = open(path.fileSystemRepresentation, O_RDONLY);
    if (fd >= 0) {
        enum { CH = 8 << 20 }; size_t first[65] = {0}, *fp = first; const arch_reads *r = &rd;   // first[i]: the first 8 MB piece of range i
        for (int i = 0; i < rd.n; i++) first[i + 1] = first[i] + (rd.len[i] + CH - 1) / CH;
        dispatch_apply(first[rd.n], dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t k) {
            int i = 0; while (k >= fp[i + 1]) i++;
            uint64_t at = r->off[i] + (uint64_t)(k - fp[i]) * CH, len = MIN((uint64_t)CH, r->len[i] - (uint64_t)(k - fp[i]) * CH);
            uint64_t pg = (uint64_t)getpagesize(), a0 = at / pg * pg; size_t ml = (size_t)(len + (at - a0));   // a mapping starts on a page
            void *w = mmap(NULL, ml, PROT_READ, MAP_SHARED, fd, (off_t)a0);
            if (w != MAP_FAILED) { (void)madvise(w, ml, MADV_WILLNEED); munmap(w, ml); }
        });
        close(fd);
    }
    return [g_dev newArchiveWithURL:[NSURL fileURLWithPath:path] error:err];
}
// Snapshot every source before admission and use that same set for the opens, including the parallel device opens. Files another run publishes later wait until next start.
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
        return a > b ? NSOrderedAscending : a < b ? NSOrderedDescending : NSOrderedSame;   // newest first, including prebuild files
    }];
    return p;
}
// Top-level mappings and reservations all occupy address space, including whole submaps. Do not count their interior holes as room the task can allocate. Bound both ends by
// TASK_VM_INFO rather than a device model or an entitlement: a container's actual task map is what matters. Failure to read it cannot establish that an archive fits.
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
        if (!arch_keys_old(arch_keys_path(p))) [paths addObject:p];   // obsolete indexes are retired before any device file is opened
    }
    uint64_t bytes = 0, pg = (uint64_t)getpagesize(); unsigned files = 0;
    for (NSString *p in paths) {
        // Malformed or missing files never reach Metal (arch_open), so they need no mapping budget.
        uint64_t n = arch_file_size(p, NULL); if (!n) continue;
        if (n > UINT64_MAX - (pg - 1)) return 0;
        n = (n + pg - 1) / pg * pg;
        if (n > UINT64_MAX - bytes) return 0;
        bytes += n; files++;
    }
    uint64_t free, largest;
    if (!arch_vm_room(&free, &largest)) { MLOG("archive admission: address-space bounds unavailable: pipelines through the MTLDevice, no archives, prebuild or captures"); return 0; }
    // Leave 2 GiB for growth after this early reading: the #2444 BotW Mac census held about 1.8 GiB in Metal shader caches, shared GPU buffers, malloc and stacks beside
    // the guest and archives. This is a conservative host policy, not a measured phone requirement. Requiring the entire set plus that reserve in one gap also bounds the
    // simultaneous opens without assuming that fragmented free space can fit their mappings. Even an empty cache must leave room for the compiler and its later captures.
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
// The prewarm files of the bundle (see g_prewarm_files). A file with a complete index is known to g_arch_ix by its keys; one without goes first among the files of bases with a vertex
// layout. A file that is not an archive, or that this system cannot open, is left out with a line in the log: its bases are built on the device as before. Call between
// arch_open_begin and arch_open_end, before the device's files (the first file to name a key keeps it)
static NSString *prewarm_prefix(NSString *gpu) { return [gpu stringByAppendingString:@"-prewarm-"]; }
// A flush period's captures as a Metal 4 pipelines script (the .mtl4-json metal-tt reads) beside its archive file, <name>.mtl4-json: the serializer that captures the binaries hands out
// the descriptors as well, vertex layouts included, so metal-tt builds the same bases for a GPU this Mac does not have (the packing script's --gpu). Asking the serializer to capture the
// descriptors too is not needed, and with it the archive is not written (macOS 27.3). A script that cannot be made only leaves those GPUs to build the bases on their first start
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
        [g_prewarm_open addObject:a];   // the archives stay open for the run
    }
    g_prewarm_files = n;
    MLOG("prewarm archive bundled with the app: %lu of %lu files, %lu with an index of %lu bases, %.1f MB, opened in %.1f ms", (unsigned long)n, (unsigned long)names.count, (unsigned long)nidx, (unsigned long)nkeys, bytes / 1048576.0, (bp_now() - t0) / 1e6);
}
// gpuarchiver-* folders are Metal's private compiler state: one per pipeline data set serializer, so one per flush period (ep_new), up to about 127 MB each, never removed by
// Metal. Metal puts them in <user cache folder>/<main bundle ID>/com.apple.gpuarchiver, or <user cache folder>/com.apple.gpuarchiver for a process without a bundle ID
// (getCacheMainFolder in Metal; its exported MTLGetGPUArchiverCachePath returns the folder). A host without a bundle (dmhost from a build) shares that folder with every other
// such program: a directory appearing there during our capture or flush may belong to another compiler, so it removes none (scripts/clean_metal_cache.py does, from outside). A
// host with a bundle ID (the Mac app) has a folder of its own, shared only with other copies of the same game's app. Every such host holds a shared lock on
// gpuarchiver.lock beside the folder for its whole run, from before its first serializer; at start it tries an exclusive one first, and with it no other copy is running, so the
// folders there are earlier runs' and it removes them. Never infer ownership from a live directory delta or remove compiler state at flush or exit: serialization success does not
// end a folder's lifetime (docs/reference/shaders.md, "Metal's build folders left by pipeline archive flushes")
static NSString *ga_dir(void) {
    static NSString *dir; static dispatch_once_t once;
    dispatch_once(&once, ^{
        NSString *(*metal)(void) = (NSString *(*)(void))dlsym(RTLD_DEFAULT, "MTLGetGPUArchiverCachePath");
        NSString *d = metal ? metal() : nil;
        if (![d isKindOfClass:NSString.class] || !d.length) {   // a system without it: the same rule
            char b[PATH_MAX]; NSString *base = confstr(_CS_DARWIN_USER_CACHE_DIR, b, sizeof b) > 1 ? @(b) : NSSearchPathForDirectoriesInDomains(NSCachesDirectory, NSUserDomainMask, YES).firstObject;
            NSString *bid = NSBundle.mainBundle.bundleIdentifier;
            d = base ? [(bid.length ? [base stringByAppendingPathComponent:bid] : base) stringByAppendingPathComponent:@"com.apple.gpuarchiver"] : nil;
        }
        dir = d.stringByStandardizingPath;
    });
    return dir;
}
static int ga_own(void) { return NSBundle.mainBundle.bundleIdentifier.length > 0 && ga_dir() != nil; }   // the folder is this game's app's, not every program's
static NSSet<NSString *> *ga_list(NSString *dir) {
    NSArray<NSString *> *a = dir ? [NSFileManager.defaultManager contentsOfDirectoryAtPath:dir error:nil] : nil;
    return [NSSet setWithArray:[a filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self BEGINSWITH 'gpuarchiver-'"]] ?: @[]];
}
// The lock beside dir, held shared from here on through *fd (left open for the run: the lock ends with the process, a forked child sharing it). → the folders of earlier runs
// to remove, when no other copy holds the lock; nil otherwise. Listed under the exclusive lock, before this process makes a serializer: a copy starting meanwhile waits for the
// listing at its shared lock, so no folder of a running copy is ever among them. A copy that cannot take the lock at all removes nothing and goes on unprotected
static NSSet<NSString *> *ga_claim(NSString *dir, int *fd) {
    *fd = -1;
    if (!dir || ![NSFileManager.defaultManager createDirectoryAtPath:dir withIntermediateDirectories:YES attributes:nil error:nil]) return nil;
    int f = open([dir.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"gpuarchiver.lock"].fileSystemRepresentation, O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (f < 0) return nil;
    NSSet<NSString *> *old = flock(f, LOCK_EX | LOCK_NB) ? nil : ga_list(dir);
    int r; while ((r = flock(f, LOCK_SH)) && errno == EINTR) {}   // from the exclusive lock, or after a starting copy's listing
    if (r) { close(f); return nil; }
    *fd = f;
    return old;
}
static ArchEpoch *ep_new(void) {   // a flush period: its compiler and serializer, nil when Metal makes none
    MTL4PipelineDataSetSerializerDescriptor *sd = [MTL4PipelineDataSetSerializerDescriptor new]; sd.configuration = MTL4PipelineDataSetSerializerConfigurationCaptureBinaries;
    ArchEpoch *ep = [ArchEpoch new]; ep->deaths = atomic_load(&g_arch_deaths); ep->ser = [g_dev newPipelineDataSetSerializerWithDescriptor:sd];
    if (!ep->ser) return nil;
    MTL4CompilerDescriptor *cd = [MTL4CompilerDescriptor new]; cd.pipelineDataSetSerializer = ep->ser;
    ep->comp = [g_dev newCompilerWithDescriptor:cd error:nil];
    if (!ep->comp) return nil;
    ep->busy = dispatch_group_create();
    return ep;
}
static ArchEpoch *ep_enter(void) {   // the current period, joined: a flush hands it to arch_write only once every compile that joined it has left
    os_unfair_lock_lock(&g_ep_lock);
    ArchEpoch *ep = g_ep; atomic_fetch_add(&ep->active, 1); dispatch_group_enter(ep->busy);
    os_unfair_lock_unlock(&g_ep_lock);
    return ep;
}
static void ep_leave(ArchEpoch *ep) { atomic_fetch_sub(&ep->active, 1); dispatch_group_leave(ep->busy); }
// A period's serialization (arch_write_file) and a pipeline compile do not overlap. Metal frees entries of its compiled-binary cache while it serializes a period's archive, and
// a compile under way through another compiler then reads one: SIGSEGV in MultiLevelAirntBinaryCache::getElementAtDestination under newRenderPipelineStateWithDescriptor,
// every time with the MTLBinaryArchive_Serialize thread in airntSerializeToURL (Super Mario Party's prebuild of 39,111 pairs, three crashes on an M4 Pro with macOS 27.2,
// after 4,000 to 22,000 pairs; GitHub issue #2513). The cause is in Metal: the host keeps the two apart. A serialization waits for the compiles under way and holds new ones
// back until it is written (writer first: the prebuild's 14 workers would otherwise keep it waiting for good); the compiles of the archive's own paths go through here
static pthread_mutex_t g_ser_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_ser_cv = PTHREAD_COND_INITIALIZER;
static int g_ser_compiles, g_ser_plain, g_ser_want;   // compiles under way, those without a capture period, serializations waiting or under way
static _Atomic uint64_t g_ser_held_ns, g_ser_holds;   // compiles held back by a serialization: time in all, how many
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
// A stuck capture must not stop every later compile behind a waiting write: after SER_WAIT_MS the write can proceed beside captures of other periods. An unresolved plain compile
// could still report service death, so the writer drops its period instead. New compiles stay held back until ser_end. Returns the unresolved plain count under that exclusion
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
// A flush period's size. Metal keeps each serializer's state in an LMDB environment of fixed map size (10 MB, in its own com.apple.gpuarchiver/gpuarchiver-* folder under the
// user cache folder, PersistentState), and once that is full every compile through the period's compiler fails ("cannot commit transaction …: MDB_MAP_FULL", "cannot write
// 'UNITSMAPPINGS' database"), not only its capture. Periods of 3,508 bases were written whole; Ocarina of Time's compacting prewarm of a 40,000-row list, whose flushes waited
// for the prewarm's end, put 8,693 bases into one and filled it, and 1,502 pipelines failed to build (GitHub PR #1735). So the ARCH_PERIOD_MAX-th capture of a period
// hands it over (arch_flush_from), compaction or not, and each period gets a fresh environment. g_arch_period_max: the tests lower it
enum { ARCH_PERIOD_MAX = 2000 };
static int g_arch_period_max = ARCH_PERIOD_MAX;
// A run file's size tier from its name, <seq>-<count>.mtl4a: 0 under PSO_ARCH_COMPACT pipelines, each next PSO_ARCH_COMPACT times larger; 8, in none, for a full period's file. One
// named before (<seq>.mtl4a), or with a count that is not all digits, counts as the largest tier below a full period, so an earlier compaction's files stay until that tier fills
static int arch_tier(NSString *path) {
    NSString *n = path.lastPathComponent.stringByDeletingPathExtension; NSRange r = [n rangeOfString:@"-"];
    NSString *cs = r.location == NSNotFound ? @"" : [n substringFromIndex:r.location + 1];
    int c = cs.length && cs.length < 10 && [cs rangeOfCharacterFromSet:NSCharacterSet.decimalDigitCharacterSet.invertedSet].location == NSNotFound ? cs.intValue : 0, t = 0;
    if (c <= 0) c = g_arch_period_max - 1;
    if (c >= g_arch_period_max) return 8;
    for (; c >= PSO_ARCH_COMPACT && t < 7; c /= PSO_ARCH_COMPACT) t++;
    return t;
}
static void arch_compact_pick(NSArray<NSString *> *paths) {   // at the open: the files of the highest tier with more than PSO_ARCH_COMPACT, and of every smaller one
    int per[9] = {0}, top = -1;
    for (NSString *p in paths) per[arch_tier(p)]++;
    for (int t = 0; t < 8; t++) if (per[t] > PSO_ARCH_COMPACT) top = t;
    if (top < 0 || !arch_compact_allowed()) return;
    if (!(g_compact_state = calloc(paths.count + 1, 1))) return;   // no memory for it: no compaction this run
    for (NSUInteger i = 0; i < paths.count; i++) if (arch_tier(paths[i]) <= top) g_compact_state[i] = 1;
    g_compact_files = paths; g_compact_enabled = 1;
}
static int arch_compact_takes(id<MTL4Archive> a) {   // a pipeline was found in a: this compaction takes that file (every run file without a pick, as before)
    if (!g_compact_enabled || !g_compact_state) return g_compact_enabled;
    NSUInteger i = [g_arch_run.count ? g_arch_run : g_arch4 indexOfObjectIdenticalTo:a]; return i != NSNotFound && g_compact_state[i];   // g_arch4: a start that found no run file (the tests set it, files of no index)
}
// The archive files no pipeline was taken from, out of the archive (arch_retire), once every lookup that could reach one was made in this run: the prewarm lists' pipelines (every
// pipeline a draw builds goes into pso_user.tsv, which the prewarm walks; g_sweep_warm) and every shader pair's pulling base (the prebuild's walk, which runs after a chain change, or
// no pairs at all; g_sweep_pairs). Both, for every file: a run file can hold pulling bases beside bases with a vertex layout. A newer file that holds a key takes its lookups (the
// index names the newest), so an older copy goes as well. Not in the bundle form of the prebuild (its work folder becomes the app's files), nor for a file the compaction
// takes (it goes then). A pipeline a draw wants that the prewarm cannot make again (a specialized fragment it skipped) is built again when drawn, as after a compaction
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
    // What the files kept hold that no lookup took: a file with some live pipelines stays whole, and only compaction (which compiles each pipeline it keeps again, about 85 ms each
    // on Ocarina of Time) would drop the rest. Counted from the indexes, once, so that whether compacting mostly unused files is worth that is a measurement, not a guess
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
static _Atomic int g_arch_cap_off;   // capturing failed in a period that held nothing yet: the store was not full, so no more captures this run (arch_cap_failed)
static int arch_capturing(void) { return g_arch_cap && !atomic_load_explicit(&g_arch_cap_off, memory_order_relaxed); }
static void arch_flush_from(ArchEpoch *only);
static void arch_at_exit(void);
// The capture refused a pipeline the plain compiler built. The archive is a cache and never a condition of drawing: the pipeline is used all the same. A period that has captured
// pipelines is taken as full and handed over, so the next captures go into a fresh environment (once per period: compiles under way in it fail too); a failure in a period that
// has captured nothing is not the store's size, and the run captures no more
static void ep_key(ArchEpoch *ep, MTL4PipelineDescriptor *d) {   // a capture's index key, recorded in its period before the compile leaves it (so the write sees it)
    uint64_t k = arch_key(d);
    if (!k) { atomic_store(&ep->unkeyed, 1); return; }
    os_unfair_lock_lock(&ep->klock); if (!ep->keys) ep->keys = [NSMutableData data]; [ep->keys appendBytes:&k length:sizeof k]; os_unfair_lock_unlock(&ep->klock);
}
// The compiler service (MTLCompilerService) died under the compile: Metal's error then names the XPC connection it lost. Its domain is the GPU family's (AGXMetalG14G on an M2)
// and its code a generic 2, so the text is what tells it apart
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
// Any other compile that failed because the compiler service died (a plain or MTLDevice pipeline, a function specialization): the service took every compile it held down,
// the captures of the current period among them, whether or not one of them reported it (Ocarina of Time: a second crash met only plain compute compiles, and the period then
// written, with 42 task / mesh pipelines, aborted the next start's lookup as in arch_cap_failed). The current period is marked and handed over, so later captures go to a sound one
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
// Captures that share a vertex-stage function (a render pipeline's vertex function, a mesh pipeline's object and mesh functions, a compute pipeline's kernel) never compile at the
// same time. A capturing compile looks its program up in its period's archive (MultiLevelAirntBinaryCache::
// getElementAtDestination → -[_MTLBinaryArchive getBinaryDataForKey:reflectionType:]) and messages the entry it gets back without holding it, while another compile of the same
// program captures it and frees that entry: SIGSEGV in objc_msgSend at getElementAtDestination+60, the other workers in getBinaryDataForKey, and no archive write under way
// (Ocarina of Time's prebuild 3 runs of 3 on an M2 Max, GitHub issue #815; Breath of the Wild 1.9.0's once on an M4 Pro, macOS 27.2.1). The cause is in Metal; the prebuild's
// pairs came in list order, and consecutive ones often share a vertex shader (Ocarina of Time: 17,515 of 33,365). A function is its library (its digest, lib_tag, else the object)
// and entry name, hashed: a collision only makes two compiles wait for each other. The fragment function is not claimed: every crash compiled a vertex program
// (compileProgram<AGX::VertexProgramKey>), and Octopath Traveler II, whose 1,910 pairs each have their own vertex shader and share a few pixel shaders, prebuilt on 14 workers in
// some 45 regress.py runs without one; claiming it held that prebuild to one compile at a time (17 s → 118 s). A capture that cannot claim its functions never compiles beside the other: the prebuild puts
// its pair back and builds it once they are free (pre_later_take), and every other caller waits up to g_fn_wait_ms and then builds the pipeline with the plain compiler, which
// captures nothing and reads no period's archive (the archive misses it this run; it is captured at a later start). So a compile that does not return (#294) holds back a draw's
// capture of its own functions by that much, not for good, and none of them races it (t_pso_stall). The tests lower it. Around it, vs_enter (DM_PSO_VS_SERIAL) keeps every
// build of one vertex function apart, captured, looked up or plain, and is taken first: the claim adds a mesh pipeline's functions and a kernel, and a build that went on past its stripe
enum { FN_BUSY_MAX = 256, FN_WAIT_MS = 2000 };
static int g_fn_wait_ms = FN_WAIT_MS, g_fn_guard = 1;   // g_fn_guard: t_pso_race --unguarded turns the guard off, as before it existed
static pthread_mutex_t g_fn_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_fn_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_fn_busy[FN_BUSY_MAX]; static int g_fn_nbusy;   // the functions of the captures under way, one entry per capture
static _Atomic int g_fn_waits, g_fn_plain;   // captures that found one of their functions under way; of them, the ones built without the archive after the wait (fn_gave_up)
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
static int fn_ids(MTL4PipelineDescriptor *d, uint64_t k[3]) {   // the functions it claims, 0 to 2: the stages before the rasterizer, or the kernel
    MTL4FunctionDescriptor *f[3] = {nil, nil, nil};
    if ([d isKindOfClass:MTL4RenderPipelineDescriptor.class]) f[0] = ((MTL4RenderPipelineDescriptor *)d).vertexFunctionDescriptor;
    else if ([d isKindOfClass:MTL4MeshRenderPipelineDescriptor.class]) { MTL4MeshRenderPipelineDescriptor *m = (MTL4MeshRenderPipelineDescriptor *)d; f[0] = m.objectFunctionDescriptor; f[1] = m.meshFunctionDescriptor; }
    else if ([d isKindOfClass:MTL4ComputePipelineDescriptor.class]) f[0] = ((MTL4ComputePipelineDescriptor *)d).computeFunctionDescriptor;
    int n = 0; for (int i = 0; i < 3; i++) if ((k[n] = fn_id(f[i]))) n++;
    return n;
}
static int fn_busy(const uint64_t *k, int n) { for (int i = 0; i < n; i++) for (int j = 0; j < g_fn_nbusy; j++) if (g_fn_busy[j] == k[i]) return 1; return 0; }
// 1: k's functions are this capture's until fn_release (or it has none to claim); 0: one stayed under way in another capture, or the table stayed full, for wait_ms (0: no wait)
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
static void fn_gave_up(const char *what) {   // a capture's wait ran out: it is built without the archive
    if (!atomic_fetch_add(&g_fn_plain, 1))
        MLOG("pipeline archive: a %s capture waited %d ms for another of the same shader function: built without the archive (Metal crashes on two at once, fn_claim); "
             "the prebuild tool's last line counts them", what, g_fn_wait_ms);
}
// A compile the archive captures, in the current flush period, counted there when it built (d: a base, or a native mesh pipeline (mtk_build); vd: the base has a vertex layout; *captured: it is in the period). No task options: a
// captured base never imports archive entries (see the compaction crash below). One the capture refuses is built by the plain compiler (arch_cap_failed); a pipeline both refuse
// returns the plain compiler's error, or the capture's when it gave none. wait_ms: how long it waits for its functions (fn_claim); busy (the prebuild): a capture that could not
// claim them is not built at all (*busy = 1, nil)
static id<MTLRenderPipelineState> arch_capture_try(MTL4PipelineDescriptor *d, NSError **e, int vd, int *captured, int wait_ms, int *busy) {
    if (captured) *captured = 0;
    if (busy) *busy = 0;
    if (!arch_capturing()) return plain_rps(d, e);
    uint64_t fk[3]; int fn = fn_ids(d, fk);
    if (!fn_claim(fk, fn, wait_ms)) {
        if (busy) { *busy = 1; return nil; }
        fn_gave_up("render");
        return plain_rps(d, e);
    }
    ser_compile_begin(); ArchEpoch *ep = ep_enter(); NSError *ce = nil;
    id<MTLRenderPipelineState> ps = [ep->comp newRenderPipelineStateWithDescriptor:d compilerTaskOptions:nil error:&ce];
    // The period may already belong to its writer. Mark it while still joined, before any fallback, even if the fallback also fails or its caller ignores the error.
    if (!ps && err_compiler_died(ce)) { atomic_store(&ep->crashed, 1); arch_died(ce); }
    fn_release(fk, fn);
    int full = 0;
    if (ps) { ep_key(ep, d); atomic_fetch_add(&g_arch_new, 1); if (vd) { atomic_fetch_add(&ep->vd, 1); atomic_fetch_add(&g_arch_new_vd, 1); }
              if ([d isKindOfClass:MTL4MeshRenderPipelineDescriptor.class]) atomic_fetch_add(&ep->ms, 1);
              full = atomic_fetch_add(&ep->n, 1) + 1 == g_arch_period_max; }
    ep_leave(ep); ser_compile_end();
    if (ps) { if (captured) *captured = 1; if (full) arch_flush_from(ep); return ps; }
    ps = plain_rps(d, e);
    if (ps && !atomic_load(&ep->crashed)) arch_cap_failed(ep, ce, "render"); else if (!ps && e && !*e) *e = ce;
    return ps;
}
static id<MTLRenderPipelineState> arch_capture(MTL4PipelineDescriptor *d, NSError **e, int vd, int *captured) { return arch_capture_try(d, e, vd, captured, g_fn_wait_ms, NULL); }
// The same for a compute pipeline (cps_new_b). It counts as a base with a vertex layout does: its file is a plain one, never a p<seq> file, so its lookups skip the prebuild's files
static id<MTLComputePipelineState> arch_capture_cs(MTL4ComputePipelineDescriptor *d, NSError **e, int *captured) {
    if (captured) *captured = 0;
    if (!arch_capturing()) return plain_cps(d, e);
    uint64_t fk[3]; int fn = fn_ids(d, fk);
    if (!fn_claim(fk, fn, g_fn_wait_ms)) {
        fn_gave_up("compute");
        return plain_cps(d, e);
    }
    ser_compile_begin(); ArchEpoch *ep = ep_enter(); NSError *ce = nil;
    id<MTLComputePipelineState> ps = [ep->comp newComputePipelineStateWithDescriptor:d compilerTaskOptions:nil error:&ce];
    if (!ps && err_compiler_died(ce)) { atomic_store(&ep->crashed, 1); arch_died(ce); }
    fn_release(fk, fn);
    int full = 0;
    if (ps) { ep_key(ep, d); atomic_fetch_add(&g_arch_new, 1); atomic_fetch_add(&ep->cs, 1); full = atomic_fetch_add(&ep->n, 1) + 1 == g_arch_period_max; }
    ep_leave(ep); ser_compile_end();
    if (ps) { if (captured) *captured = 1; if (full) arch_flush_from(ep); return ps; }
    ps = plain_cps(d, e);
    if (ps && !atomic_load(&ep->crashed)) arch_cap_failed(ep, ce, "compute"); else if (!ps && e && !*e) *e = ce;
    return ps;
}
// Metal opens a pipeline archive through NSData with "mapped if safe", and Foundation takes a file on an external drive as not safe: it reads it into memory, whole and for the whole run (docs/reference/memory.md). newArchiveWithURL has no option for this, so while the host
// opens its archives (one window in pso4_init, around every open: the bundled archive and the device's folder) the read of a .mtl4a asks for a mapping, as the internal disk gets. The game already runs from that drive; a drive pulled out ends it either way. If a system has no such method, the files are read as before
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
        if (!pso4_on() || ![(id)g_dev respondsToSelector:@selector(newCompilerWithDescriptor:error:)]) return;   // no Metal 4 compiler (t_pso_pending's fake device): the MTLDevice path
        const char *e = getenv("DM_PSO_ARCHIVE"); NSString *user = pso_user_path();
        g_arch_dir = e && !strcmp(e, "off") ? nil : e && *e ? @(e) : user ? [user.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_archive"] : nil;
        ArchPlan *plan = arch_plan(g_arch_dir);
        if (!arch_admit(plan, g_arch_dir)) { g_arch_dir = nil; return; }   // no compiler also selects the normal render / compute fallback and disables hybrids and prebuild
        NSMutableArray<id<MTL4Archive>> *arch = [NSMutableArray array], *archp = [NSMutableArray array];
        arch_open_begin();   // every archive opened from here to arch_open_end is mapped: the bundled one and the device's
        pso_bundle_open(archp, plan);   // first: it holds every pair's base
        pso_bundle_open_prewarm(arch, plan);   // then the prewarm lists' pipelines made at build time (the prebuild tool's bundle form, --prebake), before the device's files
        if (g_arch_dir) {
            g_arch_q = dispatch_queue_create("dm.nvn.pso-archive", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0));
            g_arch_pending = dispatch_group_create();
            if (ga_own()) {   // before the first serializer (ep_new below): this run's folder is never among them
                static int lock_fd; NSString *gd = ga_dir(); NSSet<NSString *> *old = ga_claim(gd, &lock_fd);
                if (lock_fd < 0) MLOG("[RED] pipeline archive: no lock beside %s: Metal's build folders there are left alone this run, and this run's may go with another copy's start", gd.UTF8String);
                else if (!old) MLOG("pipeline archive: another copy of this app is running: Metal's build folders in %s are left alone", gd.UTF8String);
                if (old.count) dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
                    uint64_t t0 = bp_now(); int n = 0; for (NSString *name in old) n += [NSFileManager.defaultManager removeItemAtPath:[gd stringByAppendingPathComponent:name] error:nil];
                    MLOG("pipeline archive: %d of %lu of Metal's build folders left by earlier runs removed from %s in %.1f s", n, (unsigned long)old.count, gd.UTF8String, (bp_now() - t0) / 1e9);
                });
            }
            uint64_t t0 = bp_now(); unsigned long long bytes = 0;
            // A write a stopped run left (arch_write_file writes <file>.mtl4a.tmp and renames it once its index is written; the graceful exit's hard deadline can end the
            // process inside the write). Nothing writes the folder yet
            for (NSString *f in [NSFileManager.defaultManager contentsOfDirectoryAtPath:g_arch_dir error:nil])
                if ([f hasSuffix:@".mtl4a.tmp"]) { [NSFileManager.defaultManager removeItemAtPath:[g_arch_dir stringByAppendingPathComponent:f] error:nil]; MLOG("pipeline archive: %s removed (a write a stopped run left)", f.UTF8String); }
            NSMutableArray<NSString *> *runf = [NSMutableArray array];
            NSArray<NSString *> *all = [NSFileManager.defaultManager contentsOfDirectoryAtPath:g_arch_dir error:nil]; NSSet<NSString *> *present = [NSSet setWithArray:all ?: @[]];
            NSArray<NSString *> *names = plan->run;   // newest first: the first index naming a key holds it
            for (NSString *n in all) if ([n hasSuffix:@".keys"] && ![present containsObject:[n.stringByDeletingPathExtension stringByAppendingString:@".mtl4a"]])
                unlink([g_arch_dir stringByAppendingPathComponent:n].fileSystemRepresentation);   // an index whose file was never renamed into place (arch_write_file)
            // A file indexed by library file names (arch_keys_old): its index promises it complete, so only a key it names leads to it, and none does now. Kept, it would be opened at
            // every start and never asked; read as unindexed instead, every lookup that misses would ask it. So it goes, and its bases are built again once, into indexed files; a
            // prebuild file going takes the prebuild's stamp (prebuilt.tsv) with it, so the prebuild runs again instead of leaving its bases to the draws
            NSMutableArray<NSString *> *keep = [NSMutableArray array]; int nold = 0, pold = 0;
            for (NSString *n in names) {
                NSString *p = [g_arch_dir stringByAppendingPathComponent:n];
                if (!arch_keys_old(arch_keys_path(p))) { [keep addObject:n]; continue; }
                arch_retire(p); nold++; pold += [n hasPrefix:@"p"];
            }
            if (pold) unlink([g_arch_dir stringByAppendingPathComponent:@"prebuilt.tsv"].fileSystemRepresentation);
            if (nold) MLOG("pipeline archive: %d files indexed by library file names moved out (%d of them the prebuild's): their bases are built again once", nold, pold);
            names = keep;
            // each file with its index, in parallel: opening one takes about 15 ms (Breath of the Wild: 52 files in 715 ms, 78 in 1.2 s, one at a time)
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
            for (NSUInteger i = 0; i < nn; i++) {
                NSString *n = names[i], *p = [g_arch_dir stringByAppendingPathComponent:n]; int pf = [n hasPrefix:@"p"];
                g_arch_seq = MAX(g_arch_seq, [n substringFromIndex:pf].intValue + 1);
                if (opened[i] == NSNull.null) {
                    // never usable again on this system (an archive opens only on the major OS version it was made for): removed, as the old indexes are above; a
                    // prebuild file takes the prebuild's stamp with it, or the prebuild is skipped and its bases are left to the draws (about 0.3 s each on the submit thread, M1 Pro)
                    MLOG("[RED] pipeline archive file %s not usable (%s): moved out, its bases are built again", p.UTF8String, why[i] != NSNull.null ? [why[i] UTF8String] : "not a pipeline archive");
                    arch_retire(p);
                    if (pf) unlink([g_arch_dir stringByAppendingPathComponent:@"prebuilt.tsv"].fileSystemRepresentation);
                    continue;
                }
                id<MTL4Archive> a = opened[i]; NSArray *ix = ixs[i] != NSNull.null ? ixs[i] : nil;
                ArchUse *u = [ArchUse new]; u->path = p; u->a = a; u->pf = pf; objc_setAssociatedObject(a, &g_arch_use_tag, u, OBJC_ASSOCIATION_RETAIN_NONATOMIC); [use addObject:u];
                if (ix) u->keys = [ix[0] length] / sizeof(uint64_t);
                bytes += [[NSFileManager.defaultManager attributesOfItemAtPath:p error:nil] fileSize];
                nopen++; np += pf; nidx += ix != nil;
                if (!pf) { [runf addObject:p]; [run addObject:a]; }
                if (!ix) { [pf ? archp : arch addObject:a]; continue; }
                NSData *keys = ix[0]; const uint64_t *kv = keys.bytes; NSUInteger nk = keys.length / sizeof(uint64_t);
                for (NSUInteger j = 0; j < nk; j++) if (!nvn_hm_get(&g_arch_ix, &kv[j])) { nvn_hm_put(&g_arch_ix, &kv[j], (__bridge void *)a); nkeys++; }   // newest first: the first holds it
                if ([ix[1] intValue]) [ixf addObject:a]; else [pf ? archp : arch addObject:a];   // an incomplete index: its pipelines with no key are looked for in it
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
            if (g_arch_cap) atexit(arch_at_exit);   // what is not written yet, written at quit (mtl_arch_save)
            if (!g_ep) MLOG("[RED] pipeline archive: Metal made no pipeline serializer: nothing is captured into %s", g_arch_dir.UTF8String);
        }
}
static void pso4_init(void) {
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        // Opening the pipeline archives can take seconds on a slow external disk even with DM_PSO_PREPARE=0 or no prewarm list: hold guest clock reads (reason 4, hle_os.c)
        int freeze = !g_pre_tool;
        if (freeze) guest_clock_freeze_for(4, 1);
        pso4_init_once();
        if (freeze) guest_clock_freeze_for(4, 0);
    });
}
static int pso4_compiler(void) { pso4_init(); return g_comp4 != nil; }   // render pipelines go through the Metal 4 compiler (fn_spec)
// Compute pipelines from the archive bundled with the app: it holds one per compute shader of the shader map and of the prewarm list's C rows (pso_offline.py), made from <key>_b's
// function alone. A compute pipeline depends on nothing else here: the MTLDevice builds it from the function (newComputePipelineStateWithFunction, no threadgroup size or other
// option; the dispatch gives the threadgroup size), so the lookup names the function alone too, by the descriptor shader_load noted (fn4_note). One the bundle does not hold
// (no bundle, a shader outside the map, an OS update that dropped the bundle's binaries) is looked up in this device's own archive files; on a miss it is built through the
// MTLDevice as before, and compiled again through the Metal 4 compiler into the current flush period on a background queue (arch_harvest), so the next start takes it from the
// file that period writes: the MTLDevice's build is the one the system's shader cache serves, the capture compiles it whole (Star Fox's first 5 at its title: 1.2 ms in all
// through the MTLDevice, 95 ms through the capture), so the dispatch never waits for the capture. Before, such a pipeline was built again at every start, and in full at each
// one after the system's shader cache dropped it. A function with constants is looked up and captured by its specialized descriptor (fn_spec; fn_off's, without the border corrections,
// which pso_offline.py builds into the bundle), and built through the MTLDevice from the specialization fn_dev makes. Not looked up and not captured: the embedded kernels, which have no
// note. A miss costs a lookup of a fraction of a millisecond per file. g_cps_arch, g_cps_run, g_cps_built: compute pipelines taken from the
// bundle, from this device's files, built (the prewarm's last lines); g_cps_look_ns, g_cps_build_ns: the time looking up, building
static _Atomic uint64_t g_cps_arch, g_cps_run, g_cps_built, g_cps_look_ns, g_cps_build_ns;
static void arch_note_new(void);
static dispatch_queue_t arch_harvest_q(void) {   // one capture at a time, at utility QoS: a network shader's compile can take seconds, beside the game's frames
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("dm.nvn.arch-harvest", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q;
}
// d into the flush period, in the background: a compute pipeline (cps_new_b) or a native mesh pipeline (mtk_build), which the MTLDevice built for its first use; compact: a
// compaction's capture of a pipeline the prewarm took from an old file (as pso_build's bases)
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
        // the prewarm's last recapture, after the prewarm: the flush that closes the compaction (warm_finish's held back while this was queued)
        if (compact && atomic_fetch_sub(&g_compact_harvests, 1) == 1 && !atomic_load(&g_warm_running)) arch_flush_from(nil);
    } });
}
// Waits until every capture queued on the harvest queue so far has joined its period (a marker behind them on that serial queue: captures queued later are not waited for, so
// draws that keep building pipelines cannot hold the wait), at most until until. Non-zero when it timed out. A waiting flush (arch_flush_wait, mtl_arch_save) calls it before it
// hands the period over: a capture still queued then joined the next period, which no one wrote before the process ended. In the prebuild tool's bundle form that left the C and M
// rows' captures out of the bundle and wrote them at exit, where their pipelines script crashed the tool in Metal's serializer and the packing script dropped all the tool's files
// (Ocarina of Time, GitHub issue #2498); a quitting game lost them and compiled them again at the next start
static long arch_harvest_wait(dispatch_time_t until) {
    dispatch_block_t mark = dispatch_block_create(0, ^{});
    dispatch_async(arch_harvest_q(), mark);
    return dispatch_block_wait(mark, until);
}
// A function the border corrections' constant specializes: its compute pipeline is in the bundle and the archive by that descriptor. Any other function with constants is built
// from fn_dev's specialization and is not looked up or captured
static int fd_border_fc(MTL4FunctionDescriptor *fd) {
    if (![fd isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) return 0;
    MTL4FunctionDescriptor *ld = ((MTL4SpecializedFunctionDescriptor *)fd).functionDescriptor;
    if (![ld isKindOfClass:MTL4LibraryFunctionDescriptor.class]) return 0;
    MTL4LibraryFunctionDescriptor *l = (MTL4LibraryFunctionDescriptor *)ld;
    // NOLINTNEXTLINE(clang-analyzer-nullability.NullablePassedToNonnull): the descriptor's name is set when it is made (fn4_note refuses a nil name, pre_fn takes the name its function was made from) and never changed
    return [l.library newFunctionWithName:l.name].functionConstantsDictionary[@"dm_border_off"] != nil;
}
static id<MTLComputePipelineState> cps_new_b(id<MTLFunction> f, int off, NSError **e) {   // off: without the border corrections (fn_off), the common dispatch's
    pso4_init();
    MTL4FunctionDescriptor *fd = fn4_of_b(f, off); MTL4ComputePipelineDescriptor *d = nil; id<MTLComputePipelineState> ps = nil; uint64_t t0 = bp_now();
    if ([fd isKindOfClass:MTL4LibraryFunctionDescriptor.class] || fd_border_fc(fd) || (off & (FN_SPLIT0 | FN_SPLIT1))) {   // a specialized one: fn_spec's or fn_off's, of a function with the border constant, or a split's pass (pso_offline.py holds both)
        d = [MTL4ComputePipelineDescriptor new]; d.computeFunctionDescriptor = fd;
        if (g_arch_bundle && (ps = [g_arch_bundle newComputePipelineStateWithDescriptor:d error:nil])) atomic_fetch_add(&g_cps_arch, 1);
        else { int from_run = 0; if ((ps = arch_lookup(d, 1, &from_run))) {   // arch_capture_cs writes them to plain files only
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
// Writes one flush period's captures as the next file of the archive, on g_arch_q once every compile in the period has returned
static void arch_compact_finish(void);
static void arch_write_file(ArchEpoch *ep);
static void arch_write(ArchEpoch *ep) {
    arch_write_file(ep);
    if (ep->compacting && atomic_fetch_sub(&g_compact_periods, 1) == 1 && atomic_load(&g_compact_closed)) arch_compact_finish();   // the compaction's last period to be written
}
static void arch_write_file(ArchEpoch *ep) {
    int n = atomic_load(&ep->n), vd = atomic_load(&ep->vd), cs = atomic_load(&ep->cs), ms = atomic_load(&ep->ms);   // exact: no compile of the period is under way
    if (!n) return;
    int plain = ser_begin();   // a plain compile may report service death only after Metal's retries: an expired wait cannot certify this period
    int crashed = atomic_load(&ep->crashed) || ep->deaths != atomic_load(&g_arch_deaths);
    if (crashed || plain) {
        ser_end();
        atomic_fetch_sub(&g_arch_new, n); atomic_fetch_sub(&g_arch_new_vd, vd);
        if (ep->compacting) g_compact_bad = 1;   // the old files keep the bases this period would have held
        if (crashed) MLOG("[RED] pipeline archive: a flush period the compiler service died in is not written (%d pipelines: %d bases, %d compute, %d task / mesh); they are captured again at a later start",
                          n, n - cs - ms, cs, ms);
        else MLOG("pipeline archive: a flush period with %d unfinished plain compiles is not written (%d pipelines); their errors are not known yet, the pipelines are captured again at a later start", plain, n);
        return;
    }
    [NSFileManager.defaultManager createDirectoryAtPath:g_arch_dir withIntermediateDirectories:YES attributes:nil error:nil];
    uint64_t t0 = bp_now(); NSError *e = nil;
    NSString *path = [g_arch_dir stringByAppendingPathComponent:vd || cs ? [NSString stringWithFormat:@"%05d-%d.mtl4a", g_arch_seq++, n] : [NSString stringWithFormat:@"p%05d.mtl4a", g_arch_seq++]], *tmp = [path stringByAppendingString:@".tmp"];
    if (g_pre_out) prewarm_script(ep->ser, path);   // the prebuild tool's bundle form: the period as a pipelines script too, for the other GPUs
    if (ep->compacting) arch_mark(1);
    BOOL ok = [ep->ser serializeAsArchiveAndFlushToURL:[NSURL fileURLWithPath:tmp] error:&e];
    if (ep->compacting) arch_mark(0);
    ser_end();
    atomic_fetch_sub(&g_arch_new, n); atomic_fetch_sub(&g_arch_new_vd, vd);
    // its index first: a keys file whose archive file never came is removed at the next start (pso4_init), an archive file without one is looked up as before
    NSString *kp = arch_keys_path(path); int keyed = ok && arch_keys_write(kp, ep->keys, !atomic_load(&ep->unkeyed));
    ok = ok && !rename(tmp.fileSystemRepresentation, path.fileSystemRepresentation);
    if (!ok && keyed) unlink(kp.fileSystemRepresentation);
    if (!ok) { MLOG("[RED] pipeline archive: writing %s failed%s%s", path.UTF8String, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); unlink(tmp.fileSystemRepresentation); if (ep->compacting) g_compact_bad = 1; }
    else {
        MLOG("pipeline archive: %d new pipeline bases%s%s written to %s (%.1f MB, %.0f ms; compiles held back by the writes so far: %llu, %.0f ms in all)", n - cs - ms,
             cs ? [NSString stringWithFormat:@" and %d compute pipelines", cs].UTF8String : "", ms ? [NSString stringWithFormat:@" and %d task / mesh pipelines", ms].UTF8String : "",
             path.lastPathComponent.UTF8String, [[NSFileManager.defaultManager attributesOfItemAtPath:path error:nil] fileSize] / 1048576.0, (bp_now() - t0) / 1e6,
             (unsigned long long)atomic_load(&g_ser_holds), atomic_load(&g_ser_held_ns) / 1e6);
    }
}
// A flush: the compiles from here on capture into a new period, and the current one is written once its compiles have returned. Returns at once, on any thread; a compile that
// does not return holds back only its own period's file. only: hand over that period alone, and only while it is the current one (a full period, a failed capture); those go
// ahead during the prewarm, where the timer's flushes wait so that its bases, compiled or captured again, make as few files as they can: a cold start's timer flushes wrote a
// file each 10 s (Ocarina of Time: 80 files; GitHub issue #1747's, 43), more than PSO_ARCH_COMPACT, and the next start compacted them all. any_time: hand it over during the
// prewarm too (going to the background, quitting: mtl_arch_save)
// A timer flush during a prewarm the game plays through (g_warm_bg; DM_PSO_ARCH_WARM_FLUSH_MS, default 60000; 0: none, the prewarm writes full periods alone): the period is handed
// over once it is that old, and each next one waits twice as long, up to 16 times (60 s, 120 s, ... 16 min), so a long cold start's prewarm adds a file per doubling and not one per
// 10 s. Without it, a prewarm that outlived the run (Ocarina of Time without a window: 31,663 lines after the first frame, every warm-up run of 2026-11-10) wrote nothing until the
// exit, and the 1.3 s the SIGTERM exit left could not write the ~425 bases it held (1.8-4 s for 900): every next run compiled them again, 57-167 bases at area loads (PR #2625). Not
// in the prepare step, which ends before the first frame (a quit there gets MTL_QUIT_ARCH_MS for at most a period), nor while a compaction runs: their bases go into as few files as
// they can. No cap by count: a period handed over at 512 makes a file of the top compacted tier, and a cold start's 8,600 bases in 16 of them would make the next start compact them
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
    os_unfair_lock_lock(&g_ep_lock); int any = (!only || g_ep == only) && (atomic_load(&g_ep->n) || atomic_load(&g_ep->active) || atomic_load(&g_ep->crashed)); os_unfair_lock_unlock(&g_ep_lock);
    // nothing captured or under way: the period goes on. A compaction due to close (the prewarm over, no recapture queued) closes all the same: its recaptures are all in periods
    // handed over, and the last of them to be written finishes it, or this does when they all are (before, it stayed open for the run and the next start compacted again)
    if (!any) {
        if (!only && g_compact_enabled && atomic_load(&g_compacted) && !atomic_load(&g_warm_running) && !atomic_load(&g_compact_harvests) && !atomic_exchange(&g_compact_closed, 1))
            dispatch_async(g_arch_q, ^{ if (!atomic_load(&g_compact_periods)) arch_compact_finish(); });   // on the queue that writes them: after any write under way
        return;
    }
    ArchEpoch *ne = ep_new();
    if (!ne) { MLOG("[RED] pipeline archive: Metal made no compiler for a new flush period: the captures go on into the current one"); return; }
    os_unfair_lock_lock(&g_ep_lock);
    ArchEpoch *old = g_ep; any = (!only || old == only) && (atomic_load(&old->n) || atomic_load(&old->active) || atomic_load(&old->crashed));   // again: another flush may just have handed it over
    if (any) {
        g_ep = ne; atomic_store(&g_ep_t0, bp_now());
        if (warm_due) atomic_fetch_add(&g_warm_flushes, 1);
        // Every period from the first recaptured base on may hold some (see arch_compact_allowed); the first one handed over after the prewarm is the compaction's last
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
// What the archive has captured and not written, written now, prewarm or not: a quit during a cold start's prewarm otherwise lost the period under way, up to ARCH_PERIOD_MAX
// bases (Ocarina of Time on an M4: about 2,000 bases, minutes of compiling), built again at the next start. Going to the background (dm_gfx_suspend): wait_ms 0, the write goes
// on behind (a file is written as .tmp and renamed, so a process killed meanwhile leaves none half-written). Quitting (atexit for ⌘Q, closing the window and exit(); dm_gfx_quit
// in the graceful exit): ending, waiting up to wait_ms for the compiles under way in the period and the writes (a file of 900 bases took 1.8-4 s on Macs, so a full period some
// 4-9 s). A write the exit then cuts short is not a compaction that crashed Metal: its marker goes, and none is written from here on (arch_mark). A wait (wait_ms > 0) first lets the
// harvest queue capture what it holds (arch_harvest_wait), within the same time. Every end the host controls stops the prewarm first (mtl_warm_stop) and waits MTL_QUIT_ARCH_MS
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
// What the harvest queue holds captured, then a flush, then every period handed over is written (not on g_arch_q, which writes them, nor on the harvest queue)
static void arch_flush_wait(void) { arch_harvest_wait(DISPATCH_TIME_FOREVER); arch_flush(); if (g_arch_pending) dispatch_group_wait(g_arch_pending, DISPATCH_TIME_FOREVER); }
static void arch_note_new(void) {   // a base was compiled: flush 10 s later (one flush for a burst of them)
    if (!g_arch_q || atomic_exchange(&g_arch_sched, 1)) return;
    // during a prewarm the game plays through, the flush waits for the period's age (arch_warm_flush_due): look again 10 s later while captures wait, new ones or not
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC), g_arch_q, ^{ atomic_store(&g_arch_sched, 0); arch_flush();
        if (atomic_load(&g_warm_running) && atomic_load(&g_warm_bg) && atomic_load(&g_arch_new) > 0) arch_note_new(); });
}
static void arch_compact_finish(void) {   // after every period that may hold recaptured bases is written (arch_write): the old files go once their bases are in the new ones
    if (!g_compact_files || !g_compacted) return;
    if (g_compact_bad) { MLOG("[RED] pipeline archive: compaction left the old files (a flush failed, or a base was not captured again)"); g_compact_files = nil; return; }   // once, as below
    int gone = 0; for (NSUInteger i = 0; i < g_compact_files.count; i++) if (!g_compact_state || g_compact_state[i]) { unlink(g_compact_files[i].fileSystemRepresentation); unlink(arch_keys_path(g_compact_files[i]).fileSystemRepresentation); gone++; }
    MLOG("pipeline archive compacted: %d files of bases with a vertex layout gave %d bases to the files just written and were removed", gone, (int)g_compacted);
    g_compact_files = nil;
}
static void pso_col4(MTL4RenderPipelineColorAttachmentDescriptor *a, MTLRenderPipelineColorAttachmentDescriptor *c) {
    a.pixelFormat = c.pixelFormat; a.writeMask = c.writeMask; a.blendingState = c.blendingEnabled ? MTL4BlendStateEnabled : MTL4BlendStateDisabled;
    a.sourceRGBBlendFactor = c.sourceRGBBlendFactor; a.destinationRGBBlendFactor = c.destinationRGBBlendFactor; a.rgbBlendOperation = c.rgbBlendOperation;
    a.sourceAlphaBlendFactor = c.sourceAlphaBlendFactor; a.destinationAlphaBlendFactor = c.destinationAlphaBlendFactor; a.alphaBlendOperation = c.alphaBlendOperation;
}
