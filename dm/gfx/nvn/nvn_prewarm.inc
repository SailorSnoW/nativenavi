// The prebuild of every shader pair's base, the draw's pipeline lookup (pso_get) and the pipelines of slice-merged passes, the prewarm lists, the prepare step before the first frame, the prebuild tool, depth-stencil states.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---- Prebuild: the base of every shader pair of the game, into the pipeline archive ----
// For each pair in the profile's shader_pairs.tsv, the base a hybrid draw needs: the pulling vertex entry (<key>_bp; a vertex shader without stage inputs: its own entry), the pixel
// shader, no vertex descriptor, one sample, no alpha-to-coverage, the colour state unspecialized: the descriptor pso_desc_pull / pso_desc make at run time, so the archive gives it back
// there (the archive matches libraries by content). Bases already in the archive are skipped; what is built goes into the archive, flushed every 1,000 bases (the serializer holds
// what it captured until then). Runs once per chain version, depth mode and pair list, and again when a library file it lacked arrives (pso_archive/prebuilt.tsv, pre_stamp_done): in the app after the prewarm, on a background queue at utility QoS
// (DM_PSO_PREBUILD=0: not at all), or as a tool on the Mac (DM_PSO_PREBUILD=tool: build, write, exit). Vertex shaders that write the layer, pairs whose vertex shader has stage
// inputs but no pulling library, and integer-output or multisampled bases are left to the draws. An app that bundles these bases made offline (DM_PSO_BUNDLE, pso_bundle_covers)
// builds none of them, only the pairs that archive left to the device (rest_pairs.tsv beside it: functions with constants)
static _Atomic int g_pre_running; static _Atomic int g_pre_next, g_pre_done, g_pre_built, g_pre_archived, g_pre_skipped, g_pre_failed;   // g_pre_next: pairs handed to a worker
// Pairs whose base failed to build, kept with the system build and the stamp they failed under (prebuild_failed.tsv beside prebuilt.tsv): prebuilt.tsv is written with them, and a
// later start with both the same leaves them to the draws instead of building them again, which would fail the same way (Metal reports a compile that its compiler service
// could not finish only after retrying it itself: Super Mario Odyssey's alRenderMaterialUber at -O2, 33.7 minutes, on every start in the background). Another system build (a new
// GPU compiler) or another stamp tries them again. g_pre_known: those lines by pair ("vertex<TAB>pixel"); g_pre_fails: this run's new lines
static NSDictionary<NSString *, NSString *> *g_pre_known; static NSMutableArray<NSString *> *g_pre_fails; static os_unfair_lock g_pre_fail_lock = OS_UNFAIR_LOCK_INIT;
static _Atomic int g_pre_known_n;   // pairs left to the draws because they failed on an earlier start
// Pairs whose capture found one of its shader functions under way in another (fn_claim), or another build of its vertex function (its stripe, vs_enter): put back with those
// functions (k, n) and that stripe (vs, -1 for none), and built once none of them is under way. g_pre_later, under g_fn_mu, so a worker picks one whose functions and stripe are
// free and a release (fn_release) wakes the ones waiting for one (a stripe's leave wakes none: they look again within a second); g_pre_deferred: the put-backs this prebuild
typedef struct { int p, n, vs; uint64_t k[3]; } PreLater;
static PreLater *g_pre_later; static int g_pre_later_n, g_pre_later_cap; static _Atomic int g_pre_deferred;
static void pre_later_put(const PreLater *l) {
    pthread_mutex_lock(&g_fn_mu);
    if (g_pre_later_n == g_pre_later_cap) {
        int cap = g_pre_later_cap ? g_pre_later_cap * 2 : 256; PreLater *m = realloc(g_pre_later, (size_t)cap * sizeof *m);
        if (!m) abort();
        g_pre_later = m; g_pre_later_cap = cap;
    }
    g_pre_later[g_pre_later_n++] = *l; g_pre_deferred++;
    pthread_cond_broadcast(&g_fn_cv); pthread_mutex_unlock(&g_fn_mu);
}
static int pre_later_free(const PreLater *l) {   // under g_fn_mu; a stalled stripe is free, as vs_enter lets builds past it
    return !fn_busy(l->k, l->n) && g_fn_nbusy + l->n <= FN_BUSY_MAX && (l->vs < 0 || !atomic_load(&g_vs_in[l->vs]) || atomic_load(&g_vs_stalled[l->vs]));
}
// → 1 and *l: a put-back pair none of whose functions is under way, taken off the list. wait: until there is one, without a limit: what it waits for is a compile under way, which
// holds its worker as long, so one that does not return (#294) keeps the prebuild from finishing either way. → 0: none (wait: none left, or the process is ending)
static int pre_later_take(PreLater *l, int wait) {
    pthread_mutex_lock(&g_fn_mu);
    for (;;) {
        for (int i = 0; i < g_pre_later_n; i++)
            if (pre_later_free(&g_pre_later[i])) {
                *l = g_pre_later[i]; g_pre_later[i] = g_pre_later[--g_pre_later_n]; pthread_mutex_unlock(&g_fn_mu); return 1;
            }
        if (!wait || !g_pre_later_n || atomic_load(&g_warm_stop)) break;
        struct timespec until; clock_gettime(CLOCK_REALTIME, &until); until.tv_sec++;   // the process ending sets no condition: looked at every second
        pthread_cond_timedwait(&g_fn_cv, &g_fn_mu, &until);
    }
    pthread_mutex_unlock(&g_fn_mu);
    return 0;
}
// Pairs with a library file the app does not carry (a package without the pulling libraries _bp.metallib; a shader a partial build has not made yet):
// their bases cannot be made yet, so the stamp names one such file (pre_device_stamp) and a later start builds them once it is there. g_pre_nolib_file: the first one seen in this
// run, a name in the library, under g_pre_fail_lock
static _Atomic int g_pre_nolib; static NSString *g_pre_nolib_file;
// Whether the library has the file: in its folder or in its image (msl_image_file), as msl_data and lib_from_file read it
static int msl_has(NSString *path) { romfs_img *m; return !access(path.fileSystemRepresentation, F_OK) || msl_image_file(path, &m) != NULL; }
// 1 (and the file noted) when the library lacks <key><suffix>
static int pre_lib_absent(NSString *key, NSString *suffix) {
    NSString *name = [key stringByAppendingString:suffix];
    if (msl_has([g_msl_dir stringByAppendingPathComponent:name])) return 0;
    g_pre_nolib++; os_unfair_lock_lock(&g_pre_fail_lock); if (!g_pre_nolib_file) g_pre_nolib_file = name; os_unfair_lock_unlock(&g_pre_fail_lock);
    return 1;
}
static NSString *pre_fail_head(NSString *want) { return [arch_os_build() stringByAppendingString:want]; }   // what a failure list must have been written for
// The failure list at path, by pair, when it was written for this system build and stamp (else empty). *other: a list written for another system build or stamp exists
static NSDictionary<NSString *, NSString *> *pre_fail_load(NSString *path, NSString *want, int *other) {
    NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
    *other = 0; if (!txt) return @{};
    NSMutableString *head = [NSMutableString string]; NSMutableDictionary<NSString *, NSString *> *pairs = [NSMutableDictionary dictionary];
    for (NSString *l in [txt componentsSeparatedByString:@"\n"]) {
        if (!l.length || [l hasPrefix:@"#"]) continue;
        NSArray<NSString *> *c = [l componentsSeparatedByString:@"\t"];
        if (c.count >= 3 && [c[0] isEqualToString:@"fail"]) pairs[[NSString stringWithFormat:@"%@\t%@", c[1], c[2]]] = l;
        else [head appendFormat:@"%@\n", l];
    }
    if ([head isEqualToString:pre_fail_head(want)]) return pairs;
    *other = 1; return @{};
}
static MTL4FunctionDescriptor *pre_fn(id<MTLLibrary> lib, NSString *name) {   // the pair archive's specialization: unused border corrections off, blend fix-up on (false) so one base serves every blend state (pso_offline.py HOST_CONSTANTS, hybrid_get)
    id<MTLFunction> f = [lib newFunctionWithName:name]; if (!f) return nil;
    MTL4LibraryFunctionDescriptor *ld = [MTL4LibraryFunctionDescriptor new]; ld.library = lib; ld.name = name;
    if (!f.functionConstantsDictionary.count) return ld;
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new];
    if (f.functionConstantsDictionary[@"dm_border_off"]) { BOOL yes = YES; [cv setConstantValue:&yes type:MTLDataTypeBool atIndex:DMSH_BORDER_FC]; }
    if (f.functionConstantsDictionary[@"dm_blend_off"]) { BOOL no = NO; [cv setConstantValue:&no type:MTLDataTypeBool atIndex:DMSH_BLEND_OFF_FC]; }
    MTL4SpecializedFunctionDescriptor *sd = [MTL4SpecializedFunctionDescriptor new]; sd.functionDescriptor = ld; sd.constantValues = cv; return sd;
}
static NSDictionary *pre_json(NSString *key) { NSData *d = msl_data([NSString stringWithFormat:@"%@/%@_b.json", g_msl_dir, key]); return d ? [NSJSONSerialization JSONObjectWithData:d options:0 error:nil] : nil; }
// The base of one pair as the draws make it (pso_desc_pull / pso_desc), or nil when the pair is left to the draws. Call inside an autorelease pool
static MTL4RenderPipelineDescriptor *pre_desc(NSString *vk, NSString *fk, int zero) {
    NSDictionary *vj = pre_json(vk), *fj = pre_json(fk);
    if ((!vj && pre_lib_absent(vk, @"_b.json")) || (!fj && pre_lib_absent(fk, @"_b.json"))) return nil;
    if (!vj || !fj || ![vj[@"chain"] isEqual:@DMSH_CHAIN] || ![fj[@"chain"] isEqual:@DMSH_CHAIN]) return nil;
    if (vj[@"mesh_stage"]) return nil;   // a mesh shader's pair (shader_pairs.tsv holds them for its padding): mesh pipelines are built at the draw (nvn_mesh.inc mtk_pso), no base
    NSString *ventry = zero && vj[@"entry_z"] ? vj[@"entry_z"] : vj[@"entry"], *fentry = fj[@"entry"] ?: @"main0";
    int pull = [vj[@"pull"] isKindOfClass:[NSDictionary class]];
    if (pull) ventry = side_entry(vj[@"pull"], zero && vj[@"entry_z"] ? @"entry_z" : @"entry", ventry);   // its name in the pulling library, as shader_load gives pull_load
    if (!pull && [vj[@"vertex_inputs"] count]) return nil;   // stage inputs and no pulling library: its pipelines need the draw's layout
    NSDictionary *fr = fj[@"raster"]; if ([fr isKindOfClass:[NSDictionary class]] && [fr[@"primitive_id"] intValue]) return nil;   // reads the primitive ID: its pipelines are built whole (pso_build_ex), no base
    NSError *e = nil;
    if (pre_lib_absent(vk, pull ? @"_bp.metallib" : @"_b.metallib") || pre_lib_absent(fk, @"_b.metallib")) return nil;
    id<MTLLibrary> vl = lib_from_file([NSString stringWithFormat:@"%@/%@%@.metallib", g_msl_dir, vk, pull ? @"_bp" : @"_b"], &e);
    if (!vl) return nil;
    id<MTLLibrary> fl = lib_from_file([NSString stringWithFormat:@"%@/%@_b.metallib", g_msl_dir, fk], &e);
    MTL4FunctionDescriptor *vf = pre_fn(vl, ventry), *ff = fl ? pre_fn(fl, fentry) : nil;
    if (!vf || !ff) return nil;
    MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
    m.vertexFunctionDescriptor = vf; m.fragmentFunctionDescriptor = ff; m.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor]; m.rasterSampleCount = 1;
    for (int i = 0; i < 8; i++) { MTL4RenderPipelineColorAttachmentDescriptor *a = m.colorAttachments[i];
        a.pixelFormat = MTLPixelFormatUnspecialized; a.writeMask = MTLColorWriteMaskUnspecialized; a.blendingState = MTL4BlendStateUnspecialized;
        a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
        a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized; }
    return m;
}
// → 1 when another capture has one of its shader functions (fn_claim) or another build its vertex function (vs_enter), neither waited for here: the pair is not built, l->k, l->n
// and l->vs name its functions and stripe (the prebuild puts it back)
static int pre_pair(NSString *vk, NSString *fk, int zero, PreLater *l) {
    @autoreleasepool {
        if (g_pre_known.count && g_pre_known[[NSString stringWithFormat:@"%@\t%@", vk, fk]]) { g_pre_skipped++; g_pre_known_n++; return 0; }
        MTL4RenderPipelineDescriptor *m = pre_desc(vk, fk, zero);
        if (!m) { g_pre_skipped++; return 0; }
        // another build of its vertex function (vs_enter: a draw's, a lookup's, another pair's) is not waited for either: the pairs of one shader are handed out together (pre_order)
        int go; vs_guard_t vg = vs_enter(m, 0, &go);
        if (!go) { l->n = fn_ids(m, l->k); l->vs = vs_stripe_of(m); return 1; }
        if (arch_lookup(m, 0, NULL)) { vs_leave(vg); g_pre_archived++; return 0; }
        NSError *e = nil; int busy = 0; id ps = arch_capture_try(m, &e, 0, NULL, 0, &busy);
        vs_leave(vg);
        if (busy) { l->n = fn_ids(m, l->k); l->vs = vs_stripe_of(m); return 1; }
        if (ps) g_pre_built++;
        else {
            if (g_pre_failed++ < 4) MLOG("[RED] pipeline prebuild: the base of %s + %s failed: %s", vk.UTF8String, fk.UTF8String, e.localizedDescription.UTF8String);
            NSString *why = [[[e.localizedDescription ?: @"no error" componentsSeparatedByCharactersInSet:NSCharacterSet.whitespaceAndNewlineCharacterSet]
                              filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"length > 0"]] componentsJoinedByString:@" "];
            os_unfair_lock_lock(&g_pre_fail_lock); [g_pre_fails addObject:[NSString stringWithFormat:@"fail\t%@\t%@\t%@", vk, fk, why]]; os_unfair_lock_unlock(&g_pre_fail_lock);
        }
    }
    return 0;
}
// What a prebuild covers (prebuilt.tsv): the chain version, the depth mode, and the pair lists' length in bytes and SHA-256 (first 16 hex digits). dm/gfx/tools/pso_offline.py writes
// the same for the archive it makes offline, from the same files
static NSString *pre_stamp(NSString *txt, int zero) {
    NSData *d = [txt dataUsingEncoding:NSUTF8StringEncoding]; uint8_t h[CC_SHA256_DIGEST_LENGTH]; CC_SHA256(d.bytes, (CC_LONG)d.length, h);
    return [NSString stringWithFormat:@"chain\t%s\ndepth\t%s\npairs\t%lu\t%02x%02x%02x%02x%02x%02x%02x%02x\n", DMSH_CHAIN, zero ? "near=0" : "near=-w", (unsigned long)d.length,
            h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]];
}
// The device's own prebuilt.tsv: that stamp, then whether the run found every library file its pairs need: "libs all", or "libs missing <file>" naming one it lacked (pre_lib_absent).
// The stamp describes the pair lists, not the libraries the app carries, which a later package of the same chain can add (the pulling libraries), so a stamp
// that names a file counts as done only while the library still lacks it. A stamp without this line was written before it existed and may hide pairs left out for an absent
// library, so it does not count as done; the bases already in the archive make that one more run short. The archive made offline keeps the stamp alone (pso_bundle_covers)
static NSString *pre_device_stamp(NSString *want) { return g_pre_nolib_file ? [want stringByAppendingFormat:@"libs\tmissing\t%@\n", g_pre_nolib_file] : [want stringByAppendingString:@"libs\tall\n"]; }
static int pre_stamp_done(NSString *have, NSString *want) {
    if (![have hasPrefix:want]) return 0;
    NSString *libs = [have substringFromIndex:want.length], *head = @"libs\tmissing\t";
    if ([libs isEqualToString:@"libs\tall\n"]) return 1;
    if (![libs hasPrefix:head] || ![libs hasSuffix:@"\n"] || libs.length <= head.length + 1) return 0;
    NSString *name = [libs substringWithRange:NSMakeRange(head.length, libs.length - head.length - 1)];
    if ([name rangeOfCharacterFromSet:[NSCharacterSet characterSetWithCharactersInString:@"/\t\n"]].location != NSNotFound) return 0;
    return !msl_has([g_msl_dir stringByAppendingPathComponent:name]);   // still absent: the same app, nothing more to build
}
static NSArray<NSArray<NSString *> *> *pre_parse(NSString *txt) {   // (vertex key, pixel key) per line, without empty lines, comments and repeated lines
    NSMutableArray<NSArray<NSString *> *> *pairs = [NSMutableArray array]; NSMutableSet<NSString *> *seen = [NSMutableSet set];
    for (NSString *l in [txt componentsSeparatedByString:@"\n"]) { if (!l.length || [l hasPrefix:@"#"] || [seen containsObject:l]) continue; [seen addObject:l]; NSArray *c = [l componentsSeparatedByString:@"\t"]; if (c.count >= 2) [pairs addObject:c]; }
    return pairs;
}
// Whether the archive bundled with the app (pso_bundle_open) holds this run's prebuild: its prebuilt.tsv is the stamp the device would write for these pairs and this depth mode,
// and a probe of up to 16 pairs spread over the list (not those it left to the device, rest) finds every base it makes in it (an archive that opens but whose binaries this system
// does not take finds none)
enum { PRE_PROBE = 16 };
static const char *stamp_line(NSString *st) {   // a stamp on one line, for the log
    return [[[st stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet] stringByReplacingOccurrencesOfString:@"\n" withString:@"; "] stringByReplacingOccurrencesOfString:@"\t" withString:@" "].UTF8String;
}
static int pso_bundle_covers(NSArray<NSArray<NSString *> *> *pairs, NSString *want, int zero, NSString *rest) {
    NSString *have = [NSString stringWithContentsOfFile:[g_bundle_dir stringByAppendingPathComponent:@"prebuilt.tsv"] encoding:NSUTF8StringEncoding error:nil];
    if (![have isEqualToString:want]) {
        MLOG("pipeline archive bundled with the app: made for %s, this run needs %s: the device builds the bases it does not hold%s", have ? stamp_line(have) : "(no prebuilt.tsv)", stamp_line(want),
             have && [have rangeOfString:zero ? @"depth\tnear=-w" : @"depth\tnear=0"].location != NSNotFound ? " (the game's depth mode is not the one built for: the profile key depth_mode)" : "");
        return 0;
    }
    NSMutableSet<NSArray<NSString *> *> *left = [NSMutableSet setWithArray:pre_parse(rest ?: @"")];
    NSMutableArray<NSArray<NSString *> *> *cand = [NSMutableArray array];
    for (NSArray<NSString *> *p in pairs) if (![left containsObject:@[p[0], p[1]]]) [cand addObject:p];
    uint64_t t0 = bp_now(); int probed = 0, found = 0; NSUInteger n = cand.count;
    for (NSUInteger k = 0; k < PRE_PROBE && k < n; k++) @autoreleasepool {
        NSArray<NSString *> *p = cand[k * n / MIN(n, (NSUInteger)PRE_PROBE)];
        MTL4RenderPipelineDescriptor *m = pre_desc(p[0], p[1], zero);
        if (!m) continue;
        probed++; int go; vs_guard_t vg = vs_enter(m, 1, &go); (void)go; found += [g_arch_bundle newRenderPipelineStateWithDescriptor:m error:nil] != nil; vs_leave(vg);
    }
    int ok = probed > 0 && found == probed;   // skipped samples give no evidence that this system can take a base from the archive
    MLOG("pipeline archive bundled with the app: %s (%d of %d probed bases found, %.1f ms)%s", ok ? "holds the bases of every shader pair" : "this system does not take its bases",
         found, probed, (bp_now() - t0) / 1e6, ok ? "" : ": the device builds them");
    return ok;
}
// The prebuild tool's workers: each waits on one synchronous compile, so as many as Metal's compiler runs at once (maximumConcurrentCompilationTaskCount); the device's active cores where Metal
// does not say. Not hw.perflevel0: on the chips with three core levels that is only the Super cores (2 on an M6, 5 on an M5 Pro), which gave the tool 1 and 4 workers
// (docs/reference/shaders.md, A prebuild pool sized from the Super cores). The prepare step keeps the active cores, the background pools a share of level 0
static int compile_tasks(void) {
    NSUInteger n = [(id)g_dev respondsToSelector:@selector(maximumConcurrentCompilationTaskCount)] ? g_dev.maximumConcurrentCompilationTaskCount : 0;
    if (n >= 2) return (int)n;
    int c = 0; size_t sz = sizeof c; if (sysctlbyname("hw.activecpu", &c, &sz, NULL, 0) || c < 2) c = 2;
    return c;
}
// The prebuild's order: the pairs by the index in which they are handed out, the pairs of a vertex shader one after another and the shader with the most pairs first. The captures of
// one shader never run at once (fn_claim): the workers put its other pairs back, take other shaders' pairs, and build the ones put back one at a time as the shader comes free
// (pre_later_take). So each shader's pairs follow one another from the start, the longest run first, within few flush periods. Ocarina of Time's tool prebuild on an M4 Pro (34,649
// pairs, 14 workers, a cold archive): 493-496 s and 8.5 GB of archive, against 644-664 s and 9.2 GB in list order without fn_claim (which crashed 1 run of 6 there), and 707-713 s
// and 10 GB with the pairs spread across the list (each period then had nearly every shader, and wrote its own copy of each)
static NSArray<NSNumber *> *pre_order(NSArray<NSArray<NSString *> *> *pairs) {
    NSMutableDictionary<NSString *, NSNumber *> *c = [NSMutableDictionary dictionary];
    for (NSArray<NSString *> *p in pairs) c[p[0]] = @(c[p[0]].intValue + 1);
    NSMutableArray<NSNumber *> *ord = [NSMutableArray arrayWithCapacity:pairs.count];
    for (NSUInteger i = 0; i < pairs.count; i++) [ord addObject:@(i)];
    [ord sortWithOptions:NSSortStable usingComparator:^NSComparisonResult(NSNumber *a, NSNumber *b) {   // stable: list order within a shader
        NSString *va = pairs[a.unsignedIntegerValue][0], *vb = pairs[b.unsignedIntegerValue][0]; int na = c[va].intValue, nb = c[vb].intValue;
        return na != nb ? (na > nb ? NSOrderedAscending : NSOrderedDescending) : [va compare:vb];
    }];
    return ord;
}
// threads: workers; wait: run on the calling thread until done (the tool), else in the background. Returns the number of pairs, -1 when there is nothing to do
static int pso_prebuild(int threads, int wait, dispatch_qos_class_t qos) {
    shmap_load(); pso4_init();
    if (!g_comp4 || (!g_arch_bundle && !g_arch_cap) || atomic_exchange(&g_pre_running, 1)) return -1;
    // The pairs: shader_pairs.tsv (BNSH variations and the other readers' pairs) and pso_pairs.tsv (agl BAHS program variations, also read by vs_pad; DM_PSO_PAIRS: another path),
    // both beside the shader map
    NSString *pp = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"shader_pairs.tsv"];
    const char *ev = getenv("DM_PSO_PAIRS");
    NSString *pp2 = ev && *ev ? @(ev) : [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_pairs.tsv"];
    NSString *txt = [NSString stringWithContentsOfFile:pp encoding:NSUTF8StringEncoding error:nil], *txt2 = [NSString stringWithContentsOfFile:pp2 encoding:NSUTF8StringEncoding error:nil];
    if (!txt) { MLOG("pipeline prebuild: no %s, nothing to build", pp.UTF8String); g_pre_running = 0; atomic_store(&g_sweep_pairs, 1); arch_sweep(); return -1; }   // no pairs: no pulling base is wanted
    if (txt2) txt = [txt stringByAppendingString:txt2];
    int zero = atomic_load(&g_depth_zero);
    NSString *want = pre_stamp(txt, zero);
    NSArray<NSArray<NSString *> *> *pairs = pre_parse(txt);
    NSString *rp = g_arch_bundle ? [g_bundle_dir stringByAppendingPathComponent:@"rest_pairs.tsv"] : nil, *rest = rp ? [NSString stringWithContentsOfFile:rp encoding:NSUTF8StringEncoding error:nil] : nil;
    if (g_arch_bundle && pso_bundle_covers(pairs, want, zero, rest)) {   // the archive made offline holds them: only the pairs it left to the device (function constants) are built here
        if (!rest.length) { g_pre_running = 0; return -1; }
        txt = rest; want = pre_stamp(rest, zero); pairs = pre_parse(rest); pp = rp; txt2 = nil;
    }
    if (!g_arch_cap) { g_pre_running = 0; return -1; }
    NSString *stamp = [g_arch_dir stringByAppendingPathComponent:@"prebuilt.tsv"], *failp = [g_arch_dir stringByAppendingPathComponent:@"prebuild_failed.tsv"];
    int other = 0; g_pre_known = pre_fail_load(failp, want, &other); g_pre_fails = [NSMutableArray array]; g_pre_known_n = 0;
    // done, unless its failures were found on another system build: then they are tried again (the rest are taken from the archive)
    if (pre_stamp_done([NSString stringWithContentsOfFile:stamp encoding:NSUTF8StringEncoding error:nil], want) && !other) { g_pre_running = 0; return -1; }
    g_pre_nolib = 0; g_pre_nolib_file = nil;
    int n = (int)pairs.count; uint64_t t0 = bp_now(); int done0 = g_pre_done;
    MLOG("pipeline prebuild: %d shader pairs of %s%s%s, %d workers (%s)", n, pp.UTF8String, txt2 ? " and " : "", txt2 ? pp2.UTF8String : "", threads, wait ? "tool" : prepare_on() ? "prepare step, before the first frame" : "background");
    atomic_store(&g_pre_next, 0); g_pre_later_n = 0; g_pre_deferred = 0;
    NSArray<NSNumber *> *ord = pre_order(pairs);
    void (^pre_done)(void) = ^{
        int d = ++g_pre_done;
        if (d % 1000 == 0) {
            MLOG("pipeline prebuild: %d of %d pairs, %d built, %d already archived, %d left to the draws, %d failed, %.0f s", d, n, (int)g_pre_built, (int)g_pre_archived, (int)g_pre_skipped, (int)g_pre_failed, (bp_now() - t0) / 1e9);
            if (atomic_load(&g_arch_new) >= 1000) arch_flush();   // the serializer holds what it captured until written
        }
    };
    // a pair: built, or put back while another capture has one of its shader functions or another build its vertex function
    void (^one)(int) = ^(int p) { PreLater l = {.p = p, .vs = -1}; if (pre_pair(pairs[p][0], pairs[p][1], zero, &l)) pre_later_put(&l); else pre_done(); };
    dispatch_group_t g = dispatch_group_create();
    for (int t = 0; t < threads; t++) dispatch_group_async(g, dispatch_get_global_queue(qos, 0), ^{
        PreLater l;
        for (;;) {
            if (atomic_load(&g_warm_stop)) break;   // the process is ending: the pairs left are built at the next start
            // a pair put back whose functions are free now goes before a new one: the captures of a shader function many pairs share (Ocarina of Time: one vertex shader
            // in 996) follow one another all along the prebuild, not all at its end
            if (pre_later_take(&l, 0)) { one(l.p); continue; }
            int i = atomic_fetch_add(&g_pre_next, 1);
            // the first worker to find no pair left hands the period over: it is written once the compiles still under way in it return, not after the slowest of all
            if (i >= n) { if (i == n) arch_flush(); break; }
            one(ord[(NSUInteger)i].intValue);
        }
        while (pre_later_take(&l, 1)) one(l.p);   // then the ones still put back, each once its functions are free: every worker stays until none is left
    });
    dispatch_block_t done = ^{
        arch_flush_wait();
        NSMutableArray<NSString *> *fails = [NSMutableArray arrayWithArray:g_pre_known.allValues]; [fails addObjectsFromArray:g_pre_fails];
        if (fails.count) [[NSString stringWithFormat:@"# pipeline prebuild: pairs whose base failed to build on this system build, left to the draws until it or the stamp changes (nvn_mtl.m, pre_fail_load)\n%@%@\n",
                            pre_fail_head(want), [fails componentsJoinedByString:@"\n"]] writeToFile:failp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        else unlink(failp.fileSystemRepresentation);
        // a prebuild the process's end cut short: no stamp, so the next start builds what is left (the rest it finds in the archive)
        if (atomic_load(&g_pre_next) >= n && !g_pre_later_n) [pre_device_stamp(want) writeToFile:stamp atomically:YES encoding:NSUTF8StringEncoding error:nil];
        MLOG("pipeline prebuild done: %d pairs, %d bases built, %d already archived, %d left to the draws, %d failed, %.1f s; %d times a pair was put back while another capture had one of its shader functions (fn_claim)",
             n, (int)g_pre_built, (int)g_pre_archived, (int)g_pre_skipped, (int)g_pre_failed, (bp_now() - t0) / 1e9, (int)g_pre_deferred);
        if (g_pre_done - done0 == n && !atomic_load(&g_warm_stop)) { atomic_store(&g_sweep_pairs, 1); arch_sweep(); }   // every pair's base was asked for
        if (g_pre_nolib) MLOG("[RED] pipeline prebuild: %d pairs need a library file this app does not carry (first: %s), left to the draws until a package with them is installed (the pulling libraries <key>_bp.metallib)", (int)g_pre_nolib, g_pre_nolib_file.UTF8String);
        if (fails.count) MLOG("pipeline prebuild: %lu pairs whose base failed are left to the draws (%s; %d of them failed on an earlier start of this system build, not built again); another system build tries them again",
                              (unsigned long)fails.count, failp.UTF8String, (int)g_pre_known_n);
        g_pre_running = 0;
    };
    if (wait) { dispatch_group_wait(g, DISPATCH_TIME_FOREVER); done(); }
    else dispatch_group_notify(g, dispatch_get_global_queue(qos, 0), done);
    return n;
}
static int prepare_on(void);
static void pso_prebuild_bg(void) {   // after the prewarm, in the app without a prepare step (with one, prepare_prebuild builds them before the first frame)
    const char *e = getenv("DM_PSO_PREBUILD"); if (e && !strcmp(e, "0")) return;
    if (!hybrid_on() || prepare_on()) return;
    int pc = 0; size_t sz = sizeof pc; if (sysctlbyname("hw.perflevel0.physicalcpu", &pc, &sz, NULL, 0) || pc <= 1) pc = 4;
    pso_prebuild(pc > 4 ? pc / 2 : 2, 0, QOS_CLASS_UTILITY);
}
static id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused);
static id<MTLFunction> ms_fs_override(NvShader *fs);   // DM_NVN_MAGSCAN (defined before draw)
static int pf_int_kind(MTLPixelFormat f);
// A pipeline Metal refused: when a colour the fragment shader writes has an integer attachment, its output was most likely translated as float (a game without PC reflection has no output types,
// Tears of the Kingdom's BC4 block encoder and Ultrahand mask were the first cases). A shader with the typed-output entry point (dmsh_entry.py fs_uint) takes it for such a pipeline, and the
// six profiles' hand-written rows went with it; one without (main0 of a shape fs_uint does not know) gets the facts row that declares the outputs, for the profile's
// shader_facts_manual.tsv (merged by shader_split.py), until fs_uint knows that shape
static const char *pso_fout_hint(NvShader *fs, const uint32_t cfmt[8], char *b, size_t n) {
    if (!fs || fs->u_name || strlen(fs->key) < 16) return "";
    int o = snprintf(b, n, "; if its outputs are integers, add to shader_facts_manual.tsv: %.16s\t{\"fout\":{", fs->key), any = 0;
    for (int c = 0; c < 8; c++) { int kind = !fs->rf.color_known || fs->rf.color_mask >> c & 1 ? pf_int_kind((MTLPixelFormat)cfmt[c]) : 0;
        if (kind && o < (int)n) o += snprintf(b + o, n - o, "%s\"%d\":\"%s\"", any++ ? "," : "", c, kind == 1 ? "uint" : "sint"); }
    if (!any) return "";
    if (o < (int)n) snprintf(b + o, n - o, "}}");
    return b;
}
static id<MTLRenderPipelineState> pso_get(NvProg *pr, MTLRenderPassDescriptor *unused) { AP_B; __unsafe_unretained id<MTLRenderPipelineState> r = pso_get_(pr, unused); AP_E(2); return r; }
// The pipelines of slice-merged passes (g_sm_plan): the draw's pipeline built with maxVertexAmplificationCount 2, keyed with AMP_MARK. Built in the background on first use; until it is
// there the draw gets nil and goes to a pass of its own (sm_plan does not merge, draw splits a merged pass), so no frame waits for the compiler. A failed build is not tried again.
// Face-split draws' pipelines (FSP_MARK, one per colour output: fsp_plan) are built and kept here the same way
enum { SM_AMP_MAX = 128 };
static struct { pso_key k; id<MTLRenderPipelineState> ps; int done; } g_sm_amp[SM_AMP_MAX]; static int g_sm_amp_n; static os_unfair_lock g_sm_amp_lock = OS_UNFAIR_LOCK_INIT;
static id<MTLRenderPipelineState> sm_amp_get(const pso_key *k, NvShader *vs, NvShader *fs) {
    __unsafe_unretained id<MTLRenderPipelineState> c = hm_get_id(&g_pso, k); if (c) return c;
    os_unfair_lock_lock(&g_sm_amp_lock); int i = 0; for (; i < g_sm_amp_n; i++) if (!memcmp(&g_sm_amp[i].k, k, sizeof *k)) break;
    if (i < g_sm_amp_n) { id ps = g_sm_amp[i].done ? g_sm_amp[i].ps : nil; os_unfair_lock_unlock(&g_sm_amp_lock); if (ps) { hm_put_id(&g_pso, k, ps); return hm_get_id(&g_pso, k); } return nil; }
    if (g_sm_amp_n >= SM_AMP_MAX) { os_unfair_lock_unlock(&g_sm_amp_lock); return nil; }
    g_sm_amp[i].k = *k; g_sm_amp[i].ps = nil; g_sm_amp[i].done = 0; g_sm_amp_n++; os_unfair_lock_unlock(&g_sm_amp_lock);
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, nil, 0); int off = pso_off(k, fs, 0);
    int face = k->fsfn == FSP_MARK;
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{ NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(d, off, &e);
        if (!ps) MLOG("[RED] %s: no amplified pipeline for vertex %s fragment %s: %s (%s)", face ? "face-split draw" : "slice-merged pass", vs->key, fs ? fs->key : "-", e.localizedDescription.UTF8String,
                      face ? "those draws keep a target per layer" : "those passes stay apart");
        os_unfair_lock_lock(&g_sm_amp_lock); g_sm_amp[i].ps = ps; g_sm_amp[i].done = 1; os_unfair_lock_unlock(&g_sm_amp_lock); });
    return nil;
}
static id<MTLRenderPipelineState> pso_get_(NvProg *pr, MTLRenderPassDescriptor *unused) {
    (void)unused;
    g_pso_pull = 0;   // set again only where a pulling pipeline is returned: a stale 1 made the next draw, whose pipeline came from the cache, bind the layout block instead of its streams
    __unsafe_unretained NvShader *vs = pr->sh[0], *fs = pr->sh[1];
    if (!vs) { RED_ONCE(13, "program has no vertex shader (or it is not mapped)"); return nil; }
    pso_key k; pso_key_fill(&k, vs, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, g_pso_lmap != NULL);
    if (S.rast_discard) {
        k.fsfn = DISCARD_MARK;
        id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
        if (cached) return cached;
        if (hm_get_id(&g_pso_fail, &k)) return nil;
        NSError *error = nil;
        id<MTLRenderPipelineState> ps = pso_build(pso_desc(&k, vs, nil, nil, 0), pso_off(&k, nil, 0), &error);
        if (ps) hm_put_id(&g_pso, &k, ps);
        else { MLOG("[RED] rasterizer-discard pipeline (vertex %s): %s", vs->key, error.localizedDescription.UTF8String); hm_put_id(&g_pso_fail, &k, NSNull.null); }
        return ps;
    }
    if (g_pso_lmap) { memcpy(k.lmap, g_pso_lmap, 8); k.fsfn = (__bridge void *)g_pso_fsfn; }
    id<MTLFunction> ms_fn = ms_fs_override(fs); if (ms_fn) k.fsfn = (__bridge void *)ms_fn;   // DM_NVN_MAGSCAN phases 2 / 3
    if (g_pso_amp) { if (k.fsfn || g_pso_lmap) return nil; k.fsfn = AMP_MARK; return sm_amp_get(&k, vs, fs); }   // a slice-merged pass's pipeline: built in the background, nil until then
    // A layout with an attribute Metal's vertex fetch cannot read draws with the pulling pipeline as its own (pull_conversion, pull_pso). DM_PSO_PULL_ALL=1 (diagnostic): every
    // draw that has a pulling pipeline ready (hybrid_get) draws with it, never with the pipeline for its layout
    { static int all = -1; if (all < 0) { const char *ev = getenv("DM_PSO_PULL_ALL"); all = ev && !strcmp(ev, "1"); if (all) MLOG("DM_PSO_PULL_ALL=1: every draw with a pulling pipeline draws with it (diagnostic)"); }
      if (!g_pso_lmap && !ms_fn) {
          __unsafe_unretained id<MTLRenderPipelineState> h = pull_conversion(vs, &k) ? pull_pso(&k, vs, fs) : all && hybrid_on() ? hybrid_get(&k, vs, fs) : nil;
          if (h) { g_pso_pull = 1; return h; }
      } }
    __unsafe_unretained id<MTLRenderPipelineState> cached = hm_get_id(&g_pso, &k);
    PD("J pipeline source=draw pass=%llu key=%016llx cache=%d vs=%s fs=%s depth=%u stencil=%u samples=%u", g_pass.serial,
       (unsigned long long)nvn_hm_hash(&k, sizeof k), cached != nil, vs->key, fs ? fs->key : "-", k.dfmt, k.sfmt, k.samples);
    if (cached) return cached;
    if (hm_get_id(&g_pso_fail, &k)) return nil;
    id<MTLRenderPipelineState> ps = nil; int took = 0;
    NSError *e = nil; uint64_t bt = bp_now(), pred0 = g_pt_pred_ns;
    g_pso_pull = 0; int why = g_pso_lmap ? 1 : ms_fn ? 2 : 0;
    if (!took && !g_pso_lmap && !ms_fn && hybrid_on()) {
        why = !vs->has_pull ? 3 : 4;
        int st = pso_pend_try(&k, &ps);
        if (st == 2) took = 1;   // built in the background (nil: it failed, and is built again below for its message)
        else {
            if (st == 0) ps = pso_build_ex(pso_desc(&k, vs, fs, nil, 0), pso_off(&k, fs, 0), &e, 0);   // its base made or archived: no compile
            if (!ps) {
                // Demand first (main's order): the draw's own pipeline is queued before the prediction admits speculative builds; with a stand-in nothing waits for it
                __unsafe_unretained id<MTLRenderPipelineState> h = hybrid_get(&k, vs, fs);
                if (h) {
                    if (st == 0) pso_pend_demand(&k, vs, fs);
                    demand_record(&k, vs, fs);
                    g_pso_pull = 1; g_hy_ns += bp_now() - bt;
                    if (pso_predict_due(st == 0)) { uint64_t t0 = bp_now(); pso_predict_rest(); g_bp_pso_ns += bp_now() - t0; }
                    return h;
                }
            }
        }
    }
    if (!ps && !took) ps = g_pso_lmap || ms_fn ? nil : pso_pend_take(&k);
    if (ps) g_pp_taken++;
    else {
        MTLRenderPipelineDescriptor *d = pso_desc(&k, vs, fs, ms_fn ? ms_fn : g_pso_fsfn, g_pso_lmap != NULL);
        ps = pso_build(d, pso_off(&k, fs, g_pso_lmap != NULL), &e);
    }
    if (!took) {   // the draw waited (built here, made from its queued build, or waited for its running build)
        uint64_t w = bp_now() - bt - (g_pt_pred_ns - pred0); g_pw_n[why]++; g_pw_ns[why] += w;
        static int lg = -1; if (lg < 0) { const char *ev = getenv("DM_PSO_WAITLOG"); lg = ev && !strcmp(ev, "1") ? 40 : 0; }
        if (lg > 0 && w > 20000000) { lg--; MLOG("pipeline wait %.1f ms (%s): vertex %s fragment %s, %u samples", w / 1e6, g_pw_why[why], vs->key, fs ? fs->key : "-", k.samples); }
    }
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - bt;
    if (!ps) { char hint[200]; MLOG("[RED] failed to create render pipeline (vertex %s fragment %s): %s%s", vs->key, fs ? fs->key : "-", e.localizedDescription.UTF8String, pso_fout_hint(fs, g_pass.cfmt, hint, sizeof hint));
               hm_put_id(&g_pso_fail, &k, NSNull.null); return nil; }
    if (!ms_fn) { pso_skey sk; char shex[sizeof sk * 2 + 1]; pso_skey_from(&k, vs, fs, g_pso_lmap != NULL, g_pso_lmap ? &g_pso_fz : NULL, &sk); pso_skey_hex(&sk, shex);
        pso_record("R\t%s\n", shex);
        pso_user_record("R\t%s\n", shex); }
    hm_put_id(&g_pso, &k, ps);
    // Metal may serialize requests internally. Submit the pipeline needed now before speculative work, and preserve record-on-use above.
    static int predict = -1; if (predict < 0) { const char *e = getenv("DM_NVN_PREDICT"); predict = !(e && !strcmp(e, "0")); if (!predict) MLOG("DM_NVN_PREDICT=0: no parallel precompile prediction"); }
    if (predict && pso_predict_due(1)) {
        uint64_t scan_t0 = bp_now(); pso_predict_rest();
        g_bp_pso_ns += bp_now() - scan_t0;   // Keep lookahead work in the pipeline-miss timing, as before
    }
    return ps;
}
static NSString *warm_key(const char *s, size_t n) {
    size_t z = strnlen(s, n);
    return [[NSString alloc] initWithBytes:s length:z encoding:NSUTF8StringEncoding];
}
// The prewarm runs the lines of its lists as tasks: a shader (S), a compute pipeline (C), or every render pipeline (R) of one shader pair. The pipelines of a pair share their
// bases, so one worker builds them one after the other. A native mesh pipeline (M) is a separate task. The tasks keep the lists' order (a render pair at its first line).
// Shaders share one dictionary. Only one worker prepares a key, including its pulling entry; a failed warm load stays NSNull for this prewarm only.
@interface WarmTask : NSObject { @public int kind, stage; NSString *key, *vkey, *fkey, *tkey; NSMutableArray<NSData *> *rs; } @end
@implementation WarmTask @end
static NSMutableDictionary *g_warm_dict; static os_unfair_lock g_warm_dict_lock = OS_UNFAIR_LOCK_INIT;
@interface WarmLoad : NSObject { @public dispatch_group_t done; id shader; } @end
@implementation WarmLoad @end
static NSMutableDictionary<NSString *, WarmLoad *> *g_warm_loads;
static void warm_load_begin(void) {
    g_warm_dict = [NSMutableDictionary dictionary]; g_warm_loads = [NSMutableDictionary dictionary];
}
static _Atomic long g_warm_done;   // lines done (the prepare screen's count)
// A prewarm list may name shaders of another version of the game: the bundled list of the version the profile was brought up on, packed with another version (the packing script's
// derived profile: Hollow Knight: Silksong's 1.0.30000 list named 16 shaders its base game 1.0.27776 does not have), or a device's pso_user.tsv kept across a game update.
// A line with a key the shader map does not have is dropped quietly (the keys are counted in the "prewarm done" line); the game never draws with it
static NSString *warm_map_stage(NSString *key) {
    NSString *mk = [key hasSuffix:@"_b"] ? [key substringToIndex:key.length - 2] : key;   // library names are <map key>_b
    return g_shmap[mk];
}
static int warm_stale(NSString *key, NSMutableSet<NSString *> *stale) {
    if (warm_map_stage(key)) return 0;
    if (![stale containsObject:key]) { [stale addObject:key]; g_warm_skip_stale++; }
    return 1;
}
static NvShader *warm_shader(NSString *key, int stage) {
    os_unfair_lock_lock(&g_warm_dict_lock);
    id sh = g_warm_dict[key];
    if (sh) { os_unfair_lock_unlock(&g_warm_dict_lock); return sh == NSNull.null ? nil : sh; }
    WarmLoad *load = g_warm_loads[key];
    if (load) {
        // Hold a strong record, not the dictionary lock, across the wait. The owner has no dependency on another warm task.
        os_unfair_lock_unlock(&g_warm_dict_lock);
        dispatch_group_wait(load->done, DISPATCH_TIME_FOREVER);
        return load->shader;
    }
    load = [WarmLoad new]; load->done = dispatch_group_create();
    dispatch_group_enter(load->done); g_warm_loads[key] = load;
    os_unfair_lock_unlock(&g_warm_dict_lock);
    NvShader *n = shader_load(key, stage);
    if (n && n->stage == 0 && n->has_pull && hybrid_on()) pull_load(n);   // fully prepare before publishing
    os_unfair_lock_lock(&g_warm_dict_lock);
    g_warm_dict[key] = n ?: (id)NSNull.null;
    load->shader = n;
    [g_warm_loads removeObjectForKey:key];
    dispatch_group_leave(load->done);
    os_unfair_lock_unlock(&g_warm_dict_lock);
    return n;
}
static void warm_enqueue(WarmItem *item) {
    os_unfair_lock_lock(&g_warm_lock);
    if (!g_warm_items) g_warm_items = [NSMutableArray array];   // here, not in warm_start: the prefetch finishes libraries before the first submission, and in runs with no prewarm list
    [g_warm_items addObject:item];
    atomic_store_explicit(&g_warm_ready, 1, memory_order_release);
    os_unfair_lock_unlock(&g_warm_lock);
}
// The lists' lines as tasks; *lines: the lines they hold
static NSArray<WarmTask *> *warm_parse(NSArray<NSString *> *paths, long *lines) {
    NSMutableArray<WarmTask *> *tasks = [NSMutableArray array]; NSMutableDictionary<NSString *, WarmTask *> *pairs = [NSMutableDictionary dictionary];
    NSMutableSet<NSString *> *seen = [NSMutableSet set], *stale = [NSMutableSet set]; long n = 0;
    NSMutableSet<NSData *> *seen_k = [NSMutableSet set];   // rows recorded before pso_key_norm that are one pipeline now
    NSMutableSet<NSString *> *seen_shader = [NSMutableSet set];
    NSMutableSet<NSArray *> *seen_mesh = [NSMutableSet set];
    for (NSString *path in paths) {
        NSString *txt = [NSString stringWithContentsOfFile:path encoding:NSUTF8StringEncoding error:nil];
        if (!txt) { MLOG("[RED] cannot read prewarm list: %s", path.UTF8String); continue; }
        { unsigned long nl = 0; for (NSString *l in [txt componentsSeparatedByString:@"\n"]) nl += l.length && ![l hasPrefix:@"#"];
          MLOG("pipeline prewarm list %s: %lu lines", path.UTF8String, nl); }   // Diagnostics: how many lines each manifest gave (when a device warms up 0, shows which one was missing)
        for (NSString *line in [txt componentsSeparatedByString:@"\n"]) {
            if (!line.length || [line hasPrefix:@"#"] || [seen containsObject:line]) continue;
            [seen addObject:line];
            NSArray<NSString *> *c = [line componentsSeparatedByString:@"\t"];
            WarmTask *t = nil;
            if (([c[0] isEqualToString:@"S"] && c.count >= 3) || ([c[0] isEqualToString:@"C"] && c.count >= 2)) {
                if (!c[1].length || warm_stale(c[1], stale)) continue;
                int shader = [c[0] isEqualToString:@"S"];
                int stage = stage_of(warm_map_stage(c[1]));   // Same stage as the live loader: NVN2 task/mesh 6/7 occupy host slots 3/4. Warm loads share one object per name.
                if (stage < 0 || (!shader && stage != 5) || (shader && [seen_shader containsObject:c[1]])) continue;
                if (shader) [seen_shader addObject:c[1]];
                t = [WarmTask new]; t->kind = shader ? 0 : 2; t->key = c[1]; t->stage = stage; [tasks addObject:t];
            } else if ([c[0] isEqualToString:@"R"] && c.count >= 2) {
                pso_skey sk; if (pso_skey_parse(c[1].UTF8String, &sk)) continue;
                NSData *kd = [NSData dataWithBytes:&sk length:sizeof sk]; if ([seen_k containsObject:kd]) continue;
                [seen_k addObject:kd];
                NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
                if (!vkey.length || warm_stale(vkey, stale) || (fkey.length && warm_stale(fkey, stale))) continue;
                NSString *pk = [NSString stringWithFormat:@"%@|%@", vkey, fkey];
                t = pairs[pk];
                if (!t) { t = [WarmTask new]; t->kind = 1; t->vkey = vkey; t->fkey = fkey; t->rs = [NSMutableArray array]; pairs[pk] = t; [tasks addObject:t]; }
                [t->rs addObject:kd];
            } else if ([c[0] isEqualToString:@"M"] && c.count == 3) {
                pso_skey sk; if (pso_mesh_skey_parse(c[2].UTF8String, &sk)) continue;
                NSString *vkey = warm_key(sk.vs, sizeof sk.vs), *fkey = warm_key(sk.fs, sizeof sk.fs);
                NSString *tkey = [c[1] isEqualToString:@"-"] ? nil : c[1];
                if (!vkey.length || warm_stale(vkey, stale) || (fkey.length && warm_stale(fkey, stale)) ||
                    (tkey && (!tkey.length || warm_stale(tkey, stale)))) continue;
                // warm_shader caches by name, so reject a mismatched stage before it can publish a shader.
                if (![warm_map_stage(vkey) isEqualToString:@"mesh"] ||
                    (fkey.length && ![warm_map_stage(fkey) isEqualToString:@"pixel"]) ||
                    (tkey && ![warm_map_stage(tkey) isEqualToString:@"task"])) continue;
                NSData *kd = [NSData dataWithBytes:&sk length:sizeof sk];
                NSArray *identity = @[c[1], kd];   // The same mesh state with a different task shader is a different pipeline.
                if ([seen_mesh containsObject:identity]) continue;
                [seen_mesh addObject:identity];
                t = [WarmTask new]; t->kind = 3; t->vkey = vkey; t->fkey = fkey; t->tkey = tkey;
                t->rs = [NSMutableArray arrayWithObject:kd];
                [tasks addObject:t];
            } else continue;
            n++;
        }
    }
    // Every compute shader of the shader map, after the lists' lines: a compute pipeline is its shader alone, so all of them can be built ahead, and one no list names was built at
    // its first dispatch on the submit thread (Kirby Air Riders: 5,586 of 8,092 in no list, about 180 ms each, 8-22 of them in a race's first minute). DM_PSO_ALL_COMPUTE=0 turns it off
    { const char *ev = getenv("DM_PSO_ALL_COMPUTE"); long added = 0;
      if (!(ev && !strcmp(ev, "0")))
          for (NSString *mk in [g_shmap.allKeys sortedArrayUsingSelector:@selector(compare:)]) {
              if (![g_shmap[mk] isEqualToString:@"compute"]) continue;
              NSString *key = [mk stringByAppendingString:@"_b"], *line = [@"C\t" stringByAppendingString:key];
              if ([seen containsObject:line]) continue;
              [seen addObject:line];
              WarmTask *t = [WarmTask new]; t->kind = 2; t->key = key; t->stage = 5; [tasks addObject:t]; n++; added++;
          }
      if (added) MLOG("pipeline prewarm: %ld compute shaders of the shader map no list names, added (DM_PSO_ALL_COMPUTE=0 turns this off)", added); }
    *lines = n; return tasks;
}
static void warm_task(WarmTask *t) {
    @autoreleasepool {
        if (t->kind == 0) {
            NvShader *sh = warm_shader(t->key, t->stage);
            if (sh) { WarmItem *item = [WarmItem new]; item->kind = 0; item->key = t->key; item->shader = sh; warm_enqueue(item); g_warm_shaders++; }
            g_warm_done++; return;
        }
        if (t->kind == 2) {
            NvShader *cs = warm_shader(t->key, 5); NSError *e = nil; id ps = cs ? cps_new_b(cs->fn, 1, &e) : nil;   // the common pipeline (g_cps)
            if (cs && !ps) MLOG("[RED] background compute pipeline creation failed: %s", e.localizedDescription.UTF8String);
            if (ps && g_pre_tool) g_warm_cps++;   // the prebuild tool: captured for the archive, nothing kept
            else if (ps) { WarmItem *item = [WarmItem new]; item->kind = 2; item->key = t->key; item->shader = cs; item->object = ps; item->cps_off = 1; warm_enqueue(item); g_warm_cps++; }
            for (int pass = 0; ps && cs->rf.split_words && pass < 2; pass++) {   // a kernel in two passes: both passes' pipelines too (split_dispatch)
                int off = 1 | (pass ? FN_SPLIT1 : FN_SPLIT0); id sp = fn_off(cs->fn, off) ? cps_new_b(cs->fn, off, &e) : nil;
                if (sp && !g_pre_tool) { WarmItem *item = [WarmItem new]; item->kind = 2; item->key = t->key; item->shader = cs; item->object = sp; item->cps_off = off; warm_enqueue(item); }
            }
            g_warm_done++; return;
        }
        pso_skey sk; [t->rs[0] getBytes:&sk length:sizeof sk];
        NvShader *vs = warm_shader(t->vkey, sk.vs_stage), *fs = t->fkey.length ? warm_shader(t->fkey, sk.fs_stage) : nil;
        if (t->kind == 3) {
            NvShader *ts = t->tkey ? warm_shader(t->tkey, 3) : nil;
            if (vs && vs->stage == 4 && vs->mesh_kind == 2 && vs->fn &&
                (!t->fkey.length || (fs && fs->stage == 1 && fs->fn)) &&
                (t->tkey ? (ts && ts->stage == 3 && ts->mesh_kind == 1 && ts->fn && !vs->mesh_notask) : vs->mesh_notask)) {
                NSError *error = nil;
                id<MTLRenderPipelineState> ps = mtk_build(&sk.k, ts, vs, fs, &error, NULL);
                if (ps && g_pre_tool) g_warm_mps++;   // the prebuild tool: captured for the archive, nothing kept
                else if (ps) {
                    WarmItem *item = [WarmItem new]; item->kind = 3; item->pso = sk.k; item->object = ps;
                    item->vs = vs; item->vs_key = t->vkey; item->fs = fs; item->fs_key = t->fkey; item->ts = ts; item->ts_key = t->tkey;
                    warm_enqueue(item); g_warm_mps++;
                } else { g_pre_warm_failed++; MLOG("[RED] task / mesh pipeline prewarm failed: task %s mesh %s fragment %s: %s",
                                                   ts ? ts->key : "-", vs->key, fs ? fs->key : "-", error ? error.localizedDescription.UTF8String : "invalid depth/stencil formats"); }
            }
            g_warm_done++; return;
        }
        for (NSData *row in t->rs) {
            if (atomic_load(&g_warm_stop)) break;
            [row getBytes:&sk length:sizeof sk];
            if (!vs || (t->fkey.length && !fs)) { g_warm_done++; continue; }
            id<MTLFunction> fsfn = nil;
            if (sk.fz) {
                if (sk.fz_desc.kind == 1 && fs) { int fx[32]; for (int i = 0; i < 32; i++) fx[i] = sk.fz_desc.fx[i]; fsfn = bf_fn(fs, fx, sk.fz_desc.fbmask, sk.k.lmap); }
                if (!fsfn) { g_warm_skip_fz++; g_warm_done++; continue; }
            }
            pso_key k = sk.k; k.vs = (__bridge void *)vs; k.fs = (__bridge void *)fs; k.fsfn = (__bridge void *)fsfn;   // pso_skey_parse normalized rows written before the key was
            int pull = !sk.merged && !sk.fz && pull_conversion(vs, &k);   // the layout's pipeline is its pulling one (pull_pso)
            MTLRenderPipelineDescriptor *desc = pull ? pso_desc_pull(&k, vs, fs) : nil;
            if (!desc) { pull = 0; desc = pso_desc(&k, vs, fs, fsfn, sk.merged); }
            NSError *e = nil; id ps = pso_build(desc, pso_off(&k, fs, sk.merged), &e);
            g_warm_done++;
            if (!ps) { g_pre_warm_failed++; MLOG("[RED] background render pipeline creation failed: %s", e.localizedDescription.UTF8String); continue; }
            if (g_pre_tool) { g_warm_rps++; continue; }   // the prebuild tool: the base is in the serializer; keep nothing
            WarmItem *item = [WarmItem new]; item->kind = 1; item->pull = pull; item->vs_key = t->vkey; item->fs_key = t->fkey; item->vs = vs; item->fs = fs; item->fsfn = fsfn; item->pso = k; item->object = ps;
            warm_enqueue(item); g_warm_rps++;
        }
    }
}
static void warm_finish(void) {
    if (g_warm_rps + g_warm_cps + g_warm_mps) { atomic_store(&g_sweep_warm, 1); arch_sweep(); }   // the lists asked for their pipelines: the files none was taken from go
    MLOG("pipeline prewarm done: shaders %llu / render pipelines %llu (specialized fragment skipped %llu) / compute pipelines %llu, background time %.1f ms; shader keys not in the shader map (another game version) %llu",
         (unsigned long long)g_warm_shaders, (unsigned long long)g_warm_rps, (unsigned long long)g_warm_skip_fz, (unsigned long long)g_warm_cps, (bp_now() - g_warm_t0) / 1e6, (unsigned long long)g_warm_skip_stale);
    if (g_warm_mps) MLOG("pipeline prewarm: %llu native task / mesh pipelines; so far %llu taken from the pipeline archive, %llu built and captured for it, %llu built without it",
                         (unsigned long long)g_warm_mps, (unsigned long long)g_mtk_arch, (unsigned long long)g_mtk_built, (unsigned long long)g_mtk_dev);
    char again[96] = "";   // only a compacting start captures bases again
    if (g_compact_enabled) snprintf(again, sizeof again, " (%d of them captured again for the compaction, %.1f ms)", (int)g_compact_bases, g_compact_ns / 1e6);
    if (g_comp4) MLOG("pipeline bases so far: %llu compiled, %llu from the archive%s, %llu reused (%.1f ms for the bases); %llu specializations (their descriptors %s), %llu pipelines without a base (MTLDevice)",
                      (unsigned long long)g_pb_built, (unsigned long long)g_pb_archived, again, (unsigned long long)g_pb_reused, g_pb_base_ns / 1e6, (unsigned long long)g_pb_spec,
                      (const char *[]){"not asked for yet", "returned retained", "returned autoreleased"}[atomic_load(&g_spec_own) % 3], (unsigned long long)g_pb_legacy);
    if (g_arch_bundle || g_arch_cap) MLOG("compute pipelines so far: %llu from the archive bundled with the app, %llu from this device's archive, %llu built (%.1f ms looking up, %.1f ms building)",
                                         (unsigned long long)g_cps_arch, (unsigned long long)g_cps_run, (unsigned long long)g_cps_built, g_cps_look_ns / 1e6, g_cps_build_ns / 1e6);
    // All warm tasks have joined. WarmItems retain published shaders until the submit thread claims them.
    // Drop negative results too: demand loading keeps its existing retry behavior, independent of the warm dictionary.
    g_warm_dict = nil; g_warm_loads = nil;
    atomic_store(&g_warm_running, 0);
    arch_flush();   // the period of the recaptured bases (compaction finishes once it is written), after the prewarm stops recapturing them; recaptures still queued close it (arch_harvest)
    @autoreleasepool { pso_prebuild_bg(); }   // then, once, the bases every shader pair's hybrid draws need
}
// The process is ending (dm_gfx_quit_begin, atexit): the prewarm and the pairs' prebuild take nothing more, so the archive's write does not share the compiler with them and no
// capture joins a period after the exit handed it over; what they had not built is built at the next start. The compiles under way finish (the period's write waits for them)
void mtl_warm_stop(void) {
    if (atomic_exchange(&g_warm_stop, 1)) return;
    if (atomic_load(&g_warm_running)) MLOG("pipeline prewarm stopped: the process is ending (%llu lines done)", (unsigned long long)g_warm_done);
    if (atomic_load(&g_pre_running)) MLOG("pipeline prebuild stopped: the process is ending (%d pairs handed out)", (int)atomic_load(&g_pre_next));
}
// ---- The prepare step: the prewarm before the game's first frame, with a progress screen ----
// With a window (or DM_PSO_PREPARE=1), the first submission waits until every line of the prewarm lists (the profile's pso_manifest.tsv, then this device's pso_user.tsv) is built:
// the shaders loaded and the pipelines made, on as many workers as the device has cores, at user-initiated QoS, since the game draws nothing meanwhile. It runs at the first
// submission and not before the game starts because the vertex entry every pipeline is built with follows the game's depth mode (nvnDeviceSetDepthMode), which is known only by then.
// Each start runs it: when everything is in the Metal shader cache and the pipeline archive it takes well under a second, and only past 0.5 s (with 0.3 s or more still to go) does the front end show its screen
// ("Preparing shaders", a bar and a count: front_progress). A stamp that says the step ran would skip it exactly when the system dropped the Metal cache (an OS update), which is when it is needed. DM_PSO_PREPARE=0: the prewarm runs in the background as before (as it does without a window); DM_PSO_PREPARE_THREADS=<n> sets the workers
void front_progress(const char *title, long done, long total);
__attribute__((weak)) void front_progress(const char *title, long done, long total) { (void)title; (void)done; (void)total; }   // the front ends' (front.h); none in the unit tests
static int prepare_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_PSO_PREPARE");
        if (e && !strcmp(e, "0")) { MLOG("DM_PSO_PREPARE=0: the pipeline prewarm runs in the background, the game does not wait for it"); v = 0; }
        else v = (e && !strcmp(e, "1")) || front_metal_layer() != NULL; }
    return v;
}
static int prepare_workers(void) {
    const char *e = getenv("DM_PSO_PREPARE_THREADS"); int n = e ? atoi(e) : 0;
    if (n <= 0) { size_t sz = sizeof n; if (sysctlbyname("hw.activecpu", &n, &sz, NULL, 0) || n < 2) n = 2; }
    return n;
}
static void warm_prepare(NSArray<WarmTask *> *tasks, long lines) {
    int workers = prepare_workers(); long n = (long)tasks.count;
    uint64_t t0 = bp_now(), built0 = g_pb_built, arch0 = g_pb_archived, reused0 = g_pb_reused, spec0 = g_pb_spec;
    __block _Atomic long next = 0;
    dispatch_group_t g = dispatch_group_create();
    for (int w = 0; w < workers; w++) dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (;;) { long i = atomic_fetch_add(&next, 1); if (i >= n || atomic_load(&g_warm_stop)) break; warm_task(tasks[(NSUInteger)i]); }
    });
    int shown = 0;
    while (dispatch_group_wait(g, dispatch_time(DISPATCH_TIME_NOW, 50 * (int64_t)NSEC_PER_MSEC))) {
        // the screen shows past 0.5 s, unless the rate so far says the rest takes less than 0.3 s more (no screen that flashes up and goes)
        uint64_t el = bp_now() - t0; long done = g_warm_done; double rest = done ? (double)el * (double)(lines - done) / (double)done : 1e18;
        if (!shown && el >= 500000000ull && rest >= 3e8) { shown = 1; MLOG("pipeline prepare: %ld of %ld lines done after %.1f s, the progress screen shows", done, lines, el / 1e9); }
        if (shown) front_progress("Preparing shaders", g_warm_done, lines);
    }
    if (shown) front_progress(NULL, lines, lines);
    char again[64] = "";   // only a compacting start captures bases again
    if (g_compact_enabled) snprintf(again, sizeof again, " (%d of them captured again for the compaction)", (int)g_compact_bases);
    MLOG("pipeline prepare: %ld lines (%llu shaders, %llu render pipelines, %llu compute pipelines) in %.2f s on %d workers before the first frame, progress screen %s; "
         "bases %llu compiled, %llu from the archive%s, %llu reused; %llu specializations",
         lines, (unsigned long long)g_warm_shaders, (unsigned long long)g_warm_rps, (unsigned long long)g_warm_cps, (bp_now() - t0) / 1e9, workers, shown ? "shown" : "not shown",
         (unsigned long long)(g_pb_built - built0), (unsigned long long)(g_pb_archived - arch0), again, (unsigned long long)(g_pb_reused - reused0), (unsigned long long)(g_pb_spec - spec0));
}
// The prebuild in the prepare step: the bases of every shader pair are made before the game's first frame, behind the progress screen ("Preparing pipelines"), once per chain
// version, depth mode and pair list (prebuilt.tsv), so a running game never compiles them meanwhile. Built in the background while playing, they would slow the game's frames
// (the compiler's load on the shared power budget); a first start waits for them instead. It waits until every pair is
// handed to a worker: the compiles still under way then (at most one per worker) finish in the background, so one that does not return (a base Apple's compiler cannot build,
// GitHub issue #294) does not hold the game's start
static void prepare_prebuild(void) {
    const char *e = getenv("DM_PSO_PREBUILD"); if (e && !strcmp(e, "0")) return;
    if (!hybrid_on()) return;
    int n = pso_prebuild(prepare_workers(), 0, QOS_CLASS_USER_INITIATED);
    if (n <= 0) return;
    uint64_t t0 = bp_now(); int shown = 0;
    // every pair handed out and none put back (pre_pair): a pair whose shader function was under way waits in g_pre_later, and counts as not yet built
    for (;;) {
        pthread_mutex_lock(&g_fn_mu); int later = g_pre_later_n; pthread_mutex_unlock(&g_fn_mu);
        if (!atomic_load(&g_pre_running) || (atomic_load(&g_pre_next) >= n && !later)) break;
        usleep(50000);
        if (!shown && bp_now() - t0 >= 500000000ull) shown = 1;
        if (shown) front_progress("Preparing pipelines", g_pre_done, n);
    }
    int left = atomic_load(&g_pre_running) ? n - (int)g_pre_done : 0;   // a worker's last compile can still be under way
    if (left > 0) MLOG("pipeline prepare: every shader pair handed out after %.1f s; %d compiles still under way finish in the background", (bp_now() - t0) / 1e9, left);
    if (shown) front_progress(NULL, n, n);
}
// ---- The prebuild tool: what a first start would build, made with no game ----
// The prewarm lists a start reads: DM_PSO_MANIFEST=<file> alone, else the profile's pso_manifest.tsv (beside the shader map) and this device's pso_user.tsv; none for
// DM_PSO_MANIFEST=off (env: its value, read before the prebuild tool turns it off). warm_start and the prebuild tool
static NSArray<NSString *> *warm_paths(const char *e) {
    if (e && !strcmp(e, "off")) return nil;
    if (e && *e) return @[@(e)];
    NSString *seed = [g_shmap_path.stringByDeletingLastPathComponent stringByAppendingPathComponent:@"pso_manifest.tsv"];
    NSMutableArray<NSString *> *all = [NSMutableArray array];
    // A profile without a bundled seed list still warms what this device recorded: returning here left pso_user.tsv growing at every start and never read, and the
    // pipeline archive never compacted (Donkey Kong Bananza: 2,132 lines unread, 88 archive files opened in 7 s)
    if ([NSFileManager.defaultManager fileExistsAtPath:seed]) [all addObject:seed];
    else MLOG("[RED] pipeline prewarm: seed list does not exist: %s", seed.UTF8String);
    NSString *user = pso_user_path();
    if (user && [NSFileManager.defaultManager fileExistsAtPath:user]) [all addObject:user];
    else MLOG("pipeline prewarm: this device's user list %s%s", user ? "does not exist yet: " : "has no path (neither DM_PSO_USER nor DM_SAVE is set)", user ? user.UTF8String : "");
    return all.count ? all : nil;
}
// The prewarm lists' pipelines, captured into the archive and nothing kept: the lines a prepare step builds before the first frame (warm_parse, warm_task: the same decode, the same
// libraries, the same descriptor pso_desc makes from a row's pso_skey and the shader's reflection, which only this code knows), as the R rows' bases, the C rows' compute pipelines
// and the M rows' mesh pipelines. No specialization (pso_build: none is archived) and no S rows (a shader load captures nothing). Returns the lines
static long pre_warm_lists(NSArray<NSString *> *paths, int threads) {
    warm_load_begin(); long lines = 0; NSMutableArray<WarmTask *> *tasks = [NSMutableArray array];
    for (WarmTask *t in warm_parse(paths, &lines)) if (t->kind) [tasks addObject:t]; else lines--;
    MLOG("pipeline prebuild: %ld prewarm lines of %s in %lu tasks, %d workers", lines, [paths componentsJoinedByString:@" and "].UTF8String, (unsigned long)tasks.count, threads);
    uint64_t t0 = bp_now(), built0 = g_pb_built, arch0 = g_pb_archived; __block _Atomic long next = 0; long n = (long)tasks.count;
    dispatch_group_t g = dispatch_group_create();
    for (int w = 0; w < threads; w++) dispatch_group_async(g, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        for (;;) { long i = atomic_fetch_add(&next, 1); if (i >= n) break; warm_task(tasks[(NSUInteger)i]); }
    });
    while (dispatch_group_wait(g, dispatch_time(DISPATCH_TIME_NOW, 10 * (int64_t)NSEC_PER_SEC))) {
        MLOG("pipeline prebuild: %ld of %ld prewarm lines, %llu bases compiled, %llu from the archives, %d failed, %.0f s", (long)g_warm_done, lines, (unsigned long long)(g_pb_built - built0),
             (unsigned long long)(g_pb_archived - arch0), (int)atomic_load(&g_pre_warm_failed), (bp_now() - t0) / 1e9);
        if (atomic_load(&g_arch_new) >= 1000) arch_flush();   // the serializer holds what it captured until written
    }
    arch_flush_wait();   // with the C and M rows' captures, which the harvest queue makes after g: nothing is left for mtl_arch_save at exit
    g_warm_dict = nil; g_warm_loads = nil;
    if (g_warm_rps + g_warm_cps + g_warm_mps) { atomic_store(&g_sweep_warm, 1); arch_sweep(); }
    MLOG("pipeline prebuild: the prewarm lists' %ld lines in %.1f s: %llu render pipelines (bases %llu compiled, %llu from the archives), %llu compute, %llu mesh, %d failed",
         lines, (bp_now() - t0) / 1e9, (unsigned long long)g_warm_rps, (unsigned long long)(g_pb_built - built0), (unsigned long long)(g_pb_archived - arch0),
         (unsigned long long)g_warm_cps, (unsigned long long)g_warm_mps, (int)atomic_load(&g_pre_warm_failed));
    return lines;
}
// The bundle form's output (DM_PSO_PREBUILD_OUT): every file the work folder (DM_PSO_ARCHIVE) holds goes into the output folder as <GPU>-prewarm-<NN>.mtl4a, with its index
// (<GPU>-prewarm-<NN>.keys) and its period's pipelines script (prewarm-<NN>.mtl4-json), and prewarm.tsv, the stamp: what pso_bundle_open_prewarm opens. The folder's files are
// this run's captures and those of an earlier run into it (the packing script keeps it only while its inputs are the same). Returns 1, or 2 when the output is incomplete
static int pre_bundle_out(NSString *out, long lines) {
    NSFileManager *fm = NSFileManager.defaultManager; NSString *pre = prewarm_prefix(g_dev.architecture.name);
    NSArray<NSString *> *staged = [[[fm contentsOfDirectoryAtPath:g_arch_dir error:nil] filteredArrayUsingPredicate:[NSPredicate predicateWithFormat:@"self ENDSWITH '.mtl4a'"]] sortedArrayUsingSelector:@selector(compare:)];
    [fm createDirectoryAtPath:out withIntermediateDirectories:YES attributes:nil error:nil];
    for (NSString *old in [fm contentsOfDirectoryAtPath:out error:nil]) if ([old hasPrefix:pre] || [old hasPrefix:@"prewarm-"] || [old isEqualToString:@"prewarm.tsv"]) [fm removeItemAtPath:[out stringByAppendingPathComponent:old] error:nil];
    int k = 0, bad = 0; unsigned long long bytes = 0;
    BOOL (^put)(NSString *, NSString *) = ^BOOL(NSString *src, NSString *dst) { return [fm linkItemAtPath:src toPath:dst error:nil] || [fm copyItemAtPath:src toPath:dst error:nil]; };   // a hard link, else a copy
    for (NSString *name in staged) {
        NSString *src = [g_arch_dir stringByAppendingPathComponent:name];
        if (!arch_file_ok(src)) { bad++; MLOG("[RED] pipeline prebuild: %s is not a pipeline archive, left out", src.UTF8String); continue; }
        NSString *dst = [out stringByAppendingPathComponent:[NSString stringWithFormat:@"%@%02d.mtl4a", pre, k]];
        if (!put(src, dst)) { bad++; MLOG("[RED] pipeline prebuild: cannot put %s in %s", name.UTF8String, out.UTF8String); continue; }
        NSString *ksrc = arch_keys_path(src);   // its index goes with it (the host reads it at the open)
        if ([fm fileExistsAtPath:ksrc] && !put(ksrc, arch_keys_path(dst))) MLOG("[RED] pipeline prebuild: cannot put the index of %s in %s", name.UTF8String, out.UTF8String);
        NSString *js = [src.stringByDeletingPathExtension stringByAppendingPathExtension:@"mtl4-json"];   // the same pipelines for the other GPUs (build_app.py, metal-tt)
        if (js && [fm fileExistsAtPath:js]) put(js, [out stringByAppendingPathComponent:[NSString stringWithFormat:@"prewarm-%02d.mtl4-json", k]]);
        bytes += [[fm attributesOfItemAtPath:dst error:nil] fileSize]; k++;
    }
    NSString *stamp = [NSString stringWithFormat:@"chain\t%s\ndepth\t%s\nlines\t%ld\nfiles\t%d\n%@", DMSH_CHAIN, atomic_load(&g_depth_zero) ? "near=0" : "near=-w", lines, k, arch_os_build()];
    if (k) [stamp writeToFile:[out stringByAppendingPathComponent:@"prewarm.tsv"] atomically:YES encoding:NSUTF8StringEncoding error:nil];
    MLOG("pipeline prebuild: %d files (%.1f MB) for the app to bundle in %s", k, bytes / 1048576.0, out.UTF8String);
    int ok = !bad && !atomic_load(&g_arch_cap_off);
    if (!ok) MLOG("[RED] pipeline prebuild: the files for the app are incomplete (%d not usable, capturing %s)", bad, atomic_load(&g_arch_cap_off) ? "stopped" : "went on");
    return ok ? 1 : 2;
}
// DM_PSO_PREBUILD=tool: what a first start would build, made ahead with no game, then exit (main.c). The host's other environment as for a run: DM_NVN_SHMAP, DM_MSL_DIR, DM_SAVE or
// DM_PSO_ARCHIVE (the archive the captures go into), DM_PSO_BUNDLE (what the app bundles is not built again). First every shader pair's pulling base (pso_prebuild, skipped when
// prebuilt.tsv says it is done), then the prewarm lists' pipelines (pre_warm_lists; DM_PSO_MANIFEST names the list as for a run, DM_PSO_PREBUILD_WARM=0 leaves them to the first
// start): a first start after a chain change otherwise found only the pairs' bases and compiled the rest of its lists behind the progress screen (Ocarina of Time: 6,400 bases,
// 450-475 s on an M5 Max). A start that finds them in the archive takes seconds, so the lists are not stamped. The depth mode, which a run knows only at the first submit
// (nvnDeviceSetDepthMode), comes as DM_PSO_PREBUILD_DEPTH=zero from the profile's depth_mode key: a pipeline of the other mode is simply not found at a run, whose lookup is by
// content. DM_PSO_PREBUILD_THREADS=<n> workers (default: compile_tasks).
// The bundle form, DM_PSO_PREBUILD_OUT=<folder> (the packing script's --prebake, after pso_offline.py's pair archive): DM_PSO_ARCHIVE is a work folder, each flush period is also written
// as a pipelines script (prewarm_script), and the work folder's files go into <folder> for the app (pre_bundle_out). With the pair archive as DM_PSO_BUNDLE, the pairs build only those
// pso_offline.py left to the device (rest_pairs.tsv: function constants). Returns 0 when not asked for, 1 done, 2 failed (the bundle form's output incomplete or missing what it needs)
int mtl_pso_prebuild_tool(void) {
    const char *e = getenv("DM_PSO_PREBUILD"); if (!e || strcmp(e, "tool")) return 0;
    g_pre_tool = 1;
    const char *w = getenv("DM_PSO_PREBUILD_WARM"), *m = getenv("DM_PSO_MANIFEST"), *oc = getenv("DM_PSO_PREBUILD_OUT"), *ac = getenv("DM_PSO_ARCHIVE");
    int warm = !(w && !strcmp(w, "0")); char *manifest = m ? strdup(m) : NULL;
    if (oc && *oc) {
        if (!ac || !*ac || !strcmp(ac, "off")) { MLOG("[RED] pipeline prebuild: DM_PSO_PREBUILD_OUT (where the files for the app go) needs DM_PSO_ARCHIVE (the work folder of the captures)"); free(manifest); return 2; }
        g_pre_out = @(oc);
    }
    setenv("DM_PSO_MANIFEST", "off", 1);   // loading the shader map starts no prewarm: the tool runs the lists itself, after the pairs
    mtl_init(); const char *z = getenv("DM_PSO_PREBUILD_DEPTH"); if (z && !strcmp(z, "zero")) atomic_store(&g_depth_zero, 1);
    int threads = getenv("DM_PSO_PREBUILD_THREADS") ? atoi(getenv("DM_PSO_PREBUILD_THREADS")) : 0;
    if (threads <= 0) threads = compile_tasks();
    if (pso_prebuild(threads, 1, QOS_CLASS_USER_INITIATED) < 0) MLOG("pipeline prebuild: no pairs to build (no archive, no shader_pairs.tsv, prebuilt.tsv says it is done, or the archive bundled with the app holds every base)");
    NSArray<NSString *> *lists = warm ? warm_paths(manifest) : nil; free(manifest);
    long lines = 0;
    if (lists && g_comp4 && g_arch_cap) lines = pre_warm_lists(lists, threads);
    else if (lists) MLOG("pipeline prebuild: no Metal 4 compiler or no archive to capture into: the prewarm lists are left to the first start");
    MLOG("pipeline prebuild: %d captures found another of the same shader function under way (fn_claim); %d of them waited %d ms in vain and were built without the archive",
         (int)atomic_load(&g_fn_waits), (int)atomic_load(&g_fn_plain), g_fn_wait_ms);
    if (!g_pre_out) return 1;
    if (!g_comp4 || !g_arch_cap) { MLOG("[RED] pipeline prebuild: no Metal 4 compiler or pipeline serializer: nothing for the app"); return 2; }
    return pre_bundle_out(g_pre_out, lines);
}
static void warm_start(void) {
    if (g_warm_started++) return;
    NSArray<NSString *> *paths = warm_paths(getenv("DM_PSO_MANIFEST"));
    if (!paths) return;
    warm_load_begin();
    pso4_init();   // the pipeline archive opens before the prewarm builds from it
    g_warm_t0 = bp_now();
    atomic_store(&g_warm_running, 1);
    if (prepare_on()) {
        long lines = 0; NSArray<WarmTask *> *tasks = warm_parse(paths, &lines);
        if (lines) warm_prepare(tasks, lines);
        else MLOG("pipeline prepare: the prewarm lists hold nothing to build");
        if (atomic_load(&g_warm_stop)) return;   // the process is ending: what is left is built at the next start
        warm_finish();
        @autoreleasepool { prepare_prebuild(); }   // then the bases every shader pair's hybrid draws need, before the first frame too (once)
        return;
    }
    dispatch_queue_attr_t attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INITIATED, 0);   // Not optional background work: it decides whether the pipelines are ready before the player enters a run
    dispatch_queue_t q = dispatch_queue_create("dm.nvn.pso-warm", attr);
    atomic_store(&g_warm_bg, 1);
    dispatch_async(q, ^{ @autoreleasepool {
        long lines = 0; for (WarmTask *t in warm_parse(paths, &lines)) { if (atomic_load(&g_warm_stop)) return; warm_task(t); }
        if (!atomic_load(&g_warm_stop)) warm_finish();   // the process is ending: what is left is built at the next start
    } });
}
static id<MTLDepthStencilState> dss_get(int has_depth, int has_stencil) {
    struct { ds_st ds; uint32_t vm[2], wm[2]; int hd, hs; } k;
    memset(&k, 0, sizeof k); k.ds = S.ds; memcpy(k.vm, S.st_vmask, 8); memcpy(k.wm, S.st_wmask, 8); k.hd = has_depth; k.hs = has_stencil;
    if (!g_dss.klen) g_dss.klen = sizeof k;
    id<MTLDepthStencilState> s = hm_get_id(&g_dss, &k);
    if (s) return s;
    MTLDepthStencilDescriptor *d = [MTLDepthStencilDescriptor new];
    if (has_depth && S.ds.dt) { d.depthCompareFunction = (MTLCompareFunction)(S.ds.df - 1); d.depthWriteEnabled = S.ds.dw; }
    if (has_stencil && S.ds.st) {
        MTLStencilDescriptor *f = [MTLStencilDescriptor new], *b = [MTLStencilDescriptor new];
        f.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_f - 1); f.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_f - 1);
        f.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_f - 1); f.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_f - 1);
        f.readMask = S.st_vmask[0]; f.writeMask = S.st_wmask[0];
        b.stencilCompareFunction = (MTLCompareFunction)(S.ds.sf_b - 1); b.stencilFailureOperation = (MTLStencilOperation)(S.ds.fail_b - 1);
        b.depthFailureOperation = (MTLStencilOperation)(S.ds.zfail_b - 1); b.depthStencilPassOperation = (MTLStencilOperation)(S.ds.zpass_b - 1);
        b.readMask = S.st_vmask[1]; b.writeMask = S.st_wmask[1];
        d.frontFaceStencil = f; d.backFaceStencil = b;
    }
    s = [g_dev newDepthStencilStateWithDescriptor:d];
    hm_put_id(&g_dss, &k, s);
    return s;
}
