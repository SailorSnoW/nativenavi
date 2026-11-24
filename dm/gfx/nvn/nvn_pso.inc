// Pipelines made from their bases (pso_build), the vertex-function stripes, the pipeline key, parallel precompile and hybrid pipelines.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// pso_desc's fixed state (everything but the functions, the vertex layout and the colour state) in a Metal 4 descriptor: its base's, and again its specialization's.
// newRenderPipelineDescriptorForSpecialization hands back the colour state open and the fixed state at its defaults (1 sample, alpha to coverage off, no input primitive class,
// amplification count 1) whatever the base was built with, and the specialized pipeline is described by that descriptor. The GPU draws with the base's state, but Metal's API validation
// reads the descriptor: a pipeline of a 4x pass read as 1-sample and stopped validation at its bind (PRAGMATA's 4x depth-only passes, "color/raster sample count (4) does not match
// the renderPipelineState's (1)", at the first gameplay frame). One function for both, so the specialization always says what its base was built with. t_pso_msaa
static void pso_fixed4(MTL4RenderPipelineDescriptor *m, MTLRenderPipelineDescriptor *d) {
    m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageState = d.alphaToCoverageEnabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    m.rasterizationEnabled = d.rasterizationEnabled; m.inputPrimitiveTopology = d.inputPrimitiveTopology;
    if (d.maxVertexAmplificationCount > 1) m.maxVertexAmplificationCount = d.maxVertexAmplificationCount;
}
// pso_desc's descriptor as a Metal 4 one: unspec leaves every colour attachment unspecialized (the base), else copies them. nil when a function has no Metal 4 descriptor (fn4_of)
static MTL4RenderPipelineDescriptor *pso_desc4(MTLRenderPipelineDescriptor *d, int unspec, int off) {   // off: without the border corrections (fn4_of_b)
    // Metal 4 requires a fragment function when rasterization is enabled. Depth-only draws still need rasterization.
    if (d.rasterizationEnabled && !d.fragmentFunction) return nil;
    MTL4FunctionDescriptor *vf = fn4_of_b(d.vertexFunction, off), *ff = d.fragmentFunction ? fn4_of_b(d.fragmentFunction, off) : nil;
    if (!vf || (d.fragmentFunction && !ff)) return nil;
    MTL4RenderPipelineDescriptor *m = [MTL4RenderPipelineDescriptor new];
    m.vertexFunctionDescriptor = vf; m.fragmentFunctionDescriptor = ff; m.vertexDescriptor = d.vertexDescriptor;
    pso_fixed4(m, d);
    for (int i = 0; i < 8; i++) {
        MTL4RenderPipelineColorAttachmentDescriptor *a = m.colorAttachments[i];
        if (!unspec) { pso_col4(a, d.colorAttachments[i]); continue; }
        a.pixelFormat = MTLPixelFormatUnspecialized; a.writeMask = MTLColorWriteMaskUnspecialized; a.blendingState = MTL4BlendStateUnspecialized;
        a.sourceRGBBlendFactor = a.destinationRGBBlendFactor = a.sourceAlphaBlendFactor = a.destinationAlphaBlendFactor = MTLBlendFactorUnspecialized;
        a.rgbBlendOperation = a.alphaBlendOperation = MTLBlendOperationUnspecialized;
    }
    return m;
}
static void pbase_key_of(MTLRenderPipelineDescriptor *d, int off, pbase_key *k) {
    memset(k, 0, sizeof *k); k->off = (uint32_t)off;   // the FN_OFF_* mask
    k->vf = (__bridge const void *)d.vertexFunction; k->ff = (__bridge const void *)d.fragmentFunction;
    k->samples = (uint32_t)d.rasterSampleCount; k->a2c = d.alphaToCoverageEnabled; k->rast = d.rasterizationEnabled; k->topo = (uint32_t)d.inputPrimitiveTopology; k->amp = (uint32_t)d.maxVertexAmplificationCount;
    MTLVertexDescriptor *vd = d.vertexDescriptor;
    for (NSUInteger i = 0; vd && i < 31; i++) {
        MTLVertexAttributeDescriptor *a = vd.attributes[i]; MTLVertexBufferLayoutDescriptor *l = vd.layouts[i];
        k->va[i][0] = (uint32_t)a.format; k->va[i][1] = (uint32_t)a.offset; k->va[i][2] = (uint32_t)a.bufferIndex;
        k->vl[i][0] = (uint32_t)l.stride; k->vl[i][1] = (uint32_t)l.stepFunction; k->vl[i][2] = (uint32_t)l.stepRate;
    }
}
// A pipeline with no Metal 4 descriptor, through the MTLDevice: from specialized functions (fn_dev_b)
static id<MTLRenderPipelineState> pso_build_dev(MTLRenderPipelineDescriptor *d, int off, NSError **e) {
    id<MTLFunction> vf = fn_dev_b(d.vertexFunction, off), ff = fn_dev_b(d.fragmentFunction, off);
    if (vf == d.vertexFunction && ff == d.fragmentFunction) return dev_rps(d, e);
    MTLRenderPipelineDescriptor *dd = [d copy]; dd.vertexFunction = vf; dd.fragmentFunction = ff;
    return dev_rps(dd, e);
}
// newRenderPipelineDescriptorForSpecialization's descriptor, owned by the caller whatever the driver returns. Its new prefix promises a +1 object, and macOS 27.0 returns one. A driver
// that returns it autoreleased (one of Apple's G16 drivers did, -[AGXG16GFamilyRenderPipeline newRenderPipelineDescriptorForSpecialization]) has
// ARC's release at the end of pso_build_ex free it, and the release of the warm task's pool crashed in objc_release before the first frame
// (NSZombieEnabled: -[MTL4RenderPipelineDescriptor release]: message sent to deallocated instance; Axiom Verge with DM_PSO_MTL4=1). That one release freed it, so the driver keeps no
// reference of its own. The first call finds out which: a weak reference outlives the call's own pool only if the caller owns the descriptor. The answer holds for the process: every base
// comes from the one device's compiler or its archives (with the Metal debug layer on, every one is wrapped alike). *own: 0 not known yet, 1 returned retained, 2 returned autoreleased
static MTL4RenderPipelineDescriptor *spec_desc(id base, _Atomic int *own) {   // base: the base pipeline (id: the test gives stand-ins of both kinds)
    void *(*call)(id, SEL) = (void *(*)(id, SEL))objc_msgSend; SEL sel = @selector(newRenderPipelineDescriptorForSpecialization);
    int o = atomic_load_explicit(own, memory_order_relaxed);
    if (o == 1) return (__bridge_transfer MTL4RenderPipelineDescriptor *)call(base, sel);
    if (o == 2) { MTL4RenderPipelineDescriptor *s; @autoreleasepool { s = (__bridge MTL4RenderPipelineDescriptor *)call(base, sel); } return s; }
    void *raw; __weak MTL4RenderPipelineDescriptor *w;
    @autoreleasepool { raw = call(base, sel); w = (__bridge MTL4RenderPipelineDescriptor *)raw; }
    if (!raw) return nil;   // nothing to tell the two apart by
    MTL4RenderPipelineDescriptor *s = w;
    if (!s) { atomic_store_explicit(own, 2, memory_order_relaxed); return spec_desc(base, own); }   // the pool freed it: ask again for one to keep
    atomic_store_explicit(own, 1, memory_order_relaxed);
    CFRelease(raw);   // the call's +1; s holds its own
    return s;
}
static _Atomic int g_spec_own;
// The pipeline pso_desc describes: its base (built, from the archive or already made), then specialized with its colour state. Called on several threads.
// compile 0: nil on an archive miss (*e untouched), with no MTLDevice fallback. Prewarm compaction may still refresh a found base for serialization.
// Apple's G16X driver keeps one entry per vertex program variant. The thread that creates a variant compiles it, or reads it from an archive, and a completion block stores the result and leaves the
// entry's dispatch group. With several threads building pipelines of one vertex function at once (a prewarm worker's whole pipeline beside others' bases, a compacting start's recaptures) that
// block has run for an entry already left or emptied: SIGTRAP "unbalanced call to dispatch_group_leave" or SIGSEGV at 0x30 in createVertexProgramVariant's block, in three Ocarina of Time runs.
// Why the driver does this is not known; builds that share a vertex function are made one at a time (DM_PSO_VS_SERIAL, on unless 0), through one of VS_STRIPES locks picked by the function's
// library name and entry name (lib_tag, as the archive's key names them, so one file loaded twice is one function; a specialized function by the function it specializes). Builds of other vertex
// functions stay parallel. The waits are bounded, so a guard can only slow a build, never stop one: a lookup that must not compile (compile 0) never waits, a game thread (user-interactive QoS)
// waits 50 ms at most and then goes on unguarded, and any other thread waits VS_WAIT_NS; one that waited that long marks the stripe stalled (a [RED] line) and its builds go on unguarded until
// the holder returns. A specialization of a base compiles no vertex function and a mesh pipeline has none: not guarded
enum { VS_STRIPES = 1024 };
static uint64_t g_vs_wait_ns = 20ull * 1000000000ull, g_vs_game_ns = 50ull * 1000000;
static dispatch_semaphore_t g_vs_sem[VS_STRIPES]; static _Atomic int g_vs_stalled[VS_STRIPES], g_vs_in[VS_STRIPES];
static _Atomic uint64_t g_vs_guarded, g_vs_waited, g_vs_skipped, g_vs_unguarded, g_vs_stalls, g_vs_max_in;
typedef struct { int idx; int held; } vs_guard_t;   // idx < 0: not guarded; held: the stripe is ours to leave
static int vs_serial(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_PSO_VS_SERIAL"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("pipelines that share a vertex function are built in parallel (DM_PSO_VS_SERIAL=0)"); } return v; }
static uint64_t vs_fnv(uint64_t h, const char *s) { for (; s && *s; s++) h = (h ^ (uint8_t)*s) * 1099511628211ull; return (h ^ 0xff) * 1099511628211ull; }
// The stripe of a pipeline's vertex function, -1 for none (a descriptor with no vertex function, or one of another kind)
static int vs_stripe_of(MTL4PipelineDescriptor *d) {
    if (![d isKindOfClass:MTL4RenderPipelineDescriptor.class]) return -1;
    MTL4FunctionDescriptor *f = ((MTL4RenderPipelineDescriptor *)d).vertexFunctionDescriptor;
    while ([f isKindOfClass:MTL4SpecializedFunctionDescriptor.class]) f = ((MTL4SpecializedFunctionDescriptor *)f).functionDescriptor;
    if (![f isKindOfClass:MTL4LibraryFunctionDescriptor.class]) return -1;
    MTL4LibraryFunctionDescriptor *l = (MTL4LibraryFunctionDescriptor *)f;
    NSString *lib = l.library ? objc_getAssociatedObject(l.library, &g_lib_tag) : nil;
    uint64_t h = 14695981039346656037ull;
    if (lib) h = vs_fnv(h, lib.UTF8String);
    else if (l.library) { uintptr_t a = (uintptr_t)(__bridge const void *)l.library; h = (h ^ a) * 1099511628211ull; }   // an unnamed library: the object
    else return -1;
    h = vs_fnv(h, l.name.UTF8String);
    return (int)((h ^ (h >> 32)) % VS_STRIPES);
}
// compile 0: take the stripe only if it is free. Otherwise wait as above. go 0 only for compile 0 that found the stripe busy: the caller leaves the build to the one in flight
static vs_guard_t vs_enter(MTL4PipelineDescriptor *d, int compile, int *go) {
    vs_guard_t g = {-1, 0}; *go = 1;
    if (!vs_serial()) return g;
    int i = vs_stripe_of(d); if (i < 0) return g;
    static dispatch_once_t once; dispatch_once(&once, ^{ for (int k = 0; k < VS_STRIPES; k++) g_vs_sem[k] = dispatch_semaphore_create(1); });
    atomic_fetch_add(&g_vs_guarded, 1);
    if (atomic_load(&g_vs_stalled[i])) { atomic_fetch_add(&g_vs_unguarded, 1); return g; }
    uint64_t cap = !compile ? 0 : qos_class_self() == QOS_CLASS_USER_INTERACTIVE ? g_vs_game_ns : g_vs_wait_ns;
    if (dispatch_semaphore_wait(g_vs_sem[i], DISPATCH_TIME_NOW)) {   // busy
        if (!compile) { atomic_fetch_add(&g_vs_skipped, 1); *go = 0; return g; }
        atomic_fetch_add(&g_vs_waited, 1);
        if (dispatch_semaphore_wait(g_vs_sem[i], dispatch_time(DISPATCH_TIME_NOW, (int64_t)cap))) {
            atomic_fetch_add(&g_vs_unguarded, 1);
            if (cap >= g_vs_wait_ns && !atomic_exchange(&g_vs_stalled[i], 1)) {
                atomic_fetch_add(&g_vs_stalls, 1);
                MLOG("[RED] a build of a vertex function (stripe %d) has held its stripe for %.0f s: the builds that share it go on in parallel until it returns (the driver can crash then, see DM_PSO_VS_SERIAL)", i, cap / 1e9);
            }
            return g;
        }
    }
    int in = atomic_fetch_add(&g_vs_in[i], 1) + 1; uint64_t m = atomic_load(&g_vs_max_in); while ((uint64_t)in > m && !atomic_compare_exchange_weak(&g_vs_max_in, &m, (uint64_t)in)) {}
    g.idx = i; g.held = 1; return g;
}
static void vs_leave(vs_guard_t g) {
    if (!g.held) return;
    atomic_fetch_sub(&g_vs_in[g.idx], 1);
    atomic_store(&g_vs_stalled[g.idx], 0);   // the holder returned: the stripe guards again
    dispatch_semaphore_signal(g_vs_sem[g.idx]);
}
static id<MTLRenderPipelineState> pso_build_ex(MTLRenderPipelineDescriptor *d, int off, NSError **e, int compile) {
    // Do not send conflicting or non-renderable aspect formats to Metal, which can
    // abort instead of returning an NSError (also on the MTLDevice fallback).
    if (!pso_ds_valid((uint32_t)d.depthAttachmentPixelFormat, (uint32_t)d.stencilAttachmentPixelFormat)) {
        if (e) *e = [NSError errorWithDomain:@"nativenavi.NVN.Pipeline" code:1
            userInfo:@{NSLocalizedDescriptionKey: [NSString stringWithFormat:@"invalid depth/stencil pipeline formats %lu/%lu (compile=%d)",
                (unsigned long)d.depthAttachmentPixelFormat, (unsigned long)d.stencilAttachmentPixelFormat, compile]}];
        return nil;
    }
    pso4_init();
    int rast = d.rasterizationEnabled;
    // A face-split draw's pipeline (fsp_plan): built whole, so that the compiler drops the work of the colour outputs the pipeline has no attachment for (a specialization of a base keeps
    // the base's code for all of them), and with its colour output going where the encoder's colour attachment map sends it, which only a Metal 4 descriptor can ask for. None otherwise
    if (rast && [d.label isEqualToString:PSO_FACE]) {
        MTL4RenderPipelineDescriptor *whole = g_comp4 ? pso_desc4(d, 0, off) : nil;
        if (!whole) { if (compile && e) *e = [NSError errorWithDomain:@"nativenavi.NVN.Pipeline" code:2 userInfo:@{NSLocalizedDescriptionKey: @"a colour-mapped pipeline needs the Metal 4 compiler and function descriptors"}]; return nil; }
        if (!compile) return nil;
        whole.colorAttachmentMappingState = MTL4LogicalToPhysicalColorAttachmentMappingStateInherited;
        return [g_comp4 newRenderPipelineStateWithDescriptor:whole compilerTaskOptions:nil error:e];
    }
    // A pipeline specialized from a base with open colour state gives its fragment function 0 for [[primitive_id]], where one built whole gives the real IDs (macOS 27.2, Apple M6: 4 triangles of a draw
    // counted per ID, 16384 0 0 0 against 4096 each, whatever the vertex function). So a fragment shader that reads the ID gets a whole pipeline, with no base to share. Fire Emblem Fortune's Weave's
    // ambient occlusion draws 4 triangles and writes the array layer of each one's primitive ID: every one wrote layer 0, so 3 pixels in 4 had no sun light. A platform limit: the ID has no other source
    if (rast && g_comp4 && [d.label isEqualToString:PSO_WHOLE]) {
        MTL4RenderPipelineDescriptor *whole = pso_desc4(d, 0, off);
        if (whole) {
            if (!compile) return nil;
            int go; vs_guard_t vg = vs_enter(whole, 1, &go); (void)go;
            id<MTLRenderPipelineState> wps = plain_rps(whole, e);
            vs_leave(vg); return wps;
        }
    }
    MTL4RenderPipelineDescriptor *b = g_comp4 ? pso_desc4(d, rast, off) : nil;   // with rasterization off there is no colour state to leave open: the base is the pipeline
    if (!b) { if (!compile) return nil; g_pb_legacy++; return pso_build_dev(d, off, e); }
    pbase_key bk; pbase_key_of(d, off, &bk);
    id<MTLRenderPipelineState> base = nil; PbaseBuild *job = nil; int owner = 0, known = 0;   // known: the archives have been searched for it and lack it
    for (;;) {
        os_unfair_lock_lock(&g_pbase_lock);
        base = hm_get_id(&g_pbase, &bk); job = nil; owner = 0; known = 0;
        if (!base) {
            known = hm_get_id(&g_pbase_miss, &bk) != nil;
            if (known && !compile) { os_unfair_lock_unlock(&g_pbase_lock); return nil; }
            job = hm_get_id(&g_pbase_pending, &bk);
            if (!job) {
                job = [PbaseBuild new]; job->done = dispatch_group_create(); job->looked = dispatch_group_create();
                dispatch_group_enter(job->done); dispatch_group_enter(job->looked); hm_put_id(&g_pbase_pending, &bk, job); owner = 1;
            }
        }
        os_unfair_lock_unlock(&g_pbase_lock);
        if (!job || owner) break;
        // Another thread is taking or building it. A compile 0 call waits for that thread's archive lookup (milliseconds) and takes the base it found, never for its compile.
        // Returning nil at once, as before, sent a draw that came while a background worker (pso_bg_look, hy_base_kick) looked up its pulling base to no stand-in and then
        // to a compile of its own base on the submit thread (Ocarina of Time's first visits to Hyrule Field and Kakariko: "draws that waited:
        // pulling base not made yet"), while every lookup the workers made found its base
        if (!compile) { dispatch_group_wait(job->looked, DISPATCH_TIME_FOREVER); base = job->found; if (base) break; return nil; }
        dispatch_group_wait(job->done, DISPATCH_TIME_FOREVER); base = job->ps;
        if (base) break;
        if (job->compiled) { if (e) *e = job->error; return nil; }
        // its owner only looked in the archive (compile 0) and found none: build it here
    }
    if (base) g_pb_reused++;
    else {
        int vsgo; vs_guard_t vsg = vs_enter(b, compile, &vsgo);
        if (!vsgo) {   // a build of this vertex function is in flight and this lookup may not compile: left to a caller that can (the loop above builds it there), nothing recorded as missing
            // Its archive result is published as well, as none found: a compile 0 call that took this job from the pending table meanwhile waits on looked, with no time
            // limit. Leaving done alone left that call waiting for ever. When it was the submit thread (a draw looking for its pulling base while a lookup worker owned the
            // job), the picture stopped for good with the sound on and no red line: Ocarina of Time on an M2 Pro, three times in one evening, each during an area load that
            // compiled pipelines in play, when builds that share a vertex function are most often in flight
            dispatch_group_leave(job->looked);
            os_unfair_lock_lock(&g_pbase_lock); job->ps = nil; hm_rel(nvn_hm_del(&g_pbase_pending, &bk)); dispatch_group_leave(job->done); os_unfair_lock_unlock(&g_pbase_lock);
            return nil;
        }
        uint64_t t0 = bp_now();
        int vd = 0; for (int i = 0; i < 31 && !vd; i++) vd = bk.va[i][0] != 0;   // a vertex layout (MTLVertexFormatInvalid is 0)
        int from_run = 0; base = known ? nil : arch_lookup(b, vd, &from_run);
        uint64_t t1 = bp_now();
        if (base) { g_pb_archived++; g_pt_hit_n++; g_pt_hit_ns += t1 - t0; }
        job->found = base; dispatch_group_leave(job->looked);   // before the compaction's capture below, which compiles
        if (base && from_run && g_compact_enabled && atomic_load(&g_warm_running)) {
            // Metal can crash in materializeAirNTFromOnlineForSlice when serializing archive imports mixed with fresh bases (#342).
            // Work around that framework bug by capturing the original shader functions, without lookupArchives. The normal compiler cache still applies.
            int cap = 0; id<MTLRenderPipelineState> cb = arch_capturing() ? arch_capture(b, nil, vd, &cap) : nil;
            g_compact_ns += bp_now() - t1;
            if (cb) base = cb;
            if (cap) { g_compacted++; g_compact_bases++; } else g_compact_bad = 1;   // not in the new files: the old ones stay
        }
        if (!base) {
            if (!known && g_arch4.count + g_arch4p.count) { g_pt_miss_n++; g_pt_miss_ns += t1 - t0; }
            if (compile) {
                NSError *be = nil; base = arch_capture(b, &be, vd, NULL); job->error = be; job->compiled = 1;
                g_pt_comp_ns += bp_now() - t1;
                if (base) { g_pb_built++; if (g_arch_cap) arch_note_new(); }
            }
        }
        if (base || compile) g_pb_base_ns += bp_now() - t0;
        vs_leave(vsg);
        os_unfair_lock_lock(&g_pbase_lock);
        job->ps = base;
        if (base) hm_put_id(&g_pbase, &bk, base);
        else if (!known) hm_put_id(&g_pbase_miss, &bk, NSNull.null);   // searched and not there, compiled or not
        hm_rel(nvn_hm_del(&g_pbase_pending, &bk)); dispatch_group_leave(job->done);
        os_unfair_lock_unlock(&g_pbase_lock);
        if (!base) { if (compile && e) *e = job->error; return nil; }
    }
    if (!rast || g_pre_tool) return base;   // the prebuild tool wants the base alone (a specialization is never archived: g_comp4 has no serializer), its colour state is the device's
    uint64_t ts = bp_now();
    MTL4RenderPipelineDescriptor *s = spec_desc(base, &g_spec_own);
    pso_fixed4(s, d);   // the base's fixed state, which the specialization descriptor comes back without
    for (int i = 0; i < 8; i++) pso_col4(s.colorAttachments[i], d.colorAttachments[i]);
    id<MTLRenderPipelineState> ps = plain_spec(s, base, e);
    if (ps) g_pb_spec++;
    g_pt_spec_ns += bp_now() - ts;
    return ps;
}
static id<MTLRenderPipelineState> pso_build(MTLRenderPipelineDescriptor *d, int off, NSError **e) { return pso_build_ex(d, off, e, 1); }   // off: FN_OFF_* mask (pso_off)
// Whether a pipeline of key k needs dm_blend0's runtime words: pure function of (k, fs, merged), so prewarmed rows keep their keys and predictions never read g_pass.cfmt
static int pso_blend_live(const pso_key *k, NvShader *fs, int merged) {
    if (!fs || !fs->has_bz) return 0;
    const uint8_t *lm = merged ? k->lmap : NULL;
    return (bz_word_for(k->cfmt, lm, k->blend_en, k->blend) | rop_word_for(k->cfmt, lm, k->blend_en, k->blend, k->logic != 0)) != 0;
}
// The FN_OFF_* mask a pipeline of key k is built with (fn_off): without border corrections when no bound sampler has a twin, without the blend fix-up when fs has it and both words are 0
static inline int pso_off(const pso_key *k, NvShader *fs, int merged) {
    return (k->border ? 0 : FN_OFF_BORDER) | (fs && fs->has_bz && !pso_blend_live(k, fs, merged) ? FN_OFF_BLEND : 0);
}
// Pipeline key: current state S + pass formats (normal pass; a merged pass additionally carries lmap / specialized function, filled in by pso_get)
static void pso_key_fill(pso_key *k, NvShader *vs, NvShader *fs, const uint32_t cfmt[8], uint32_t dfmt, uint32_t sfmt, uint32_t samples, int lmap) {
    memset(k, 0, sizeof *k);
    k->vs = (__bridge void *)vs; k->fs = (__bridge void *)fs;
    memcpy(k->cfmt, cfmt, sizeof k->cfmt); k->dfmt = dfmt; k->sfmt = sfmt; k->samples = samples;
    k->blend_en = S.color.blend_en; k->chmask = S.chmask.mask; k->a2c = S.ms.a2c; k->atest = atest_key(fs, cfmt, lmap ? g_pso_lmap : NULL);
    for (int i = 0; i < 8; i++) if (k->cfmt[i] || lmap) k->blend[i] = S.blend[i];
    k->logic = lop_key(k, fs, lmap);
    k->border = border_need(vs, vs->stage) || border_need(fs, 1);   // as prog_border, which the draw compares
    const nvn_refl *rf = &vs->rf;
    for (int v = 0; v < rf->nv; v++) { int a = rf->v[v].nvn; if (a < 32 && a < S.nattr) { if (a < 16) k->attr[a] = S.attr[a]; else k->attr_hi[a - 16] = S.attr[a]; int st = S.attr[a].stream; if (st < 16) k->stream[st] = S.stream[st]; } }
    pso_key_norm(k, lmap);
}
// ---- Parallel precompile: pipelines met for the first time used to be compiled synchronously one by one on the submit thread; 9 misses in a row in one frame = 284 ms (heavy scene on a real device).
// After satisfying the first pipeline miss in a submission, scan ahead once along the remaining commands (replaying state only, no drawing) and kick a bounded number of normal-pass pipelines to the background;
// when the submit thread reaches that draw it only waits for 'the remaining time not yet compiled'. The compiled pipelines are identical to synchronously compiled ones (same descriptor). Pipeline keys of merged passes / specialized fragments depend on the planning result at replay time, are not predicted, and are compiled synchronously as before.
#define PSO_PEND_MAX 384
// PSO_PEND_ACTIVE / PSO_PREDICT_BUDGET bound the guesses only (a key with another depth / stencil attachment than the draw's). The draws the scan met with their attachments as they
// are come later in this submission, so each of their pipelines is admitted (pso_pend_kick_next), bounded by the table: bounded by 2 as well, a cold load of FINAL FANTASY
// RESONANCE into Grandshelt Castle waited 2.8 s for pipelines, worst frame 2.2 s, against 0.74-0.79 s (PR #1344)
enum { PSO_PEND_ACTIVE = 2, PSO_PREDICT_BUDGET = 2 };
// One budget per prediction, retained by its lookup workers. Both the submit
// thread and workers spend it under g_pp_mu, only when a slot is admitted.
// A single generation/count pair loses an older prediction's count when its
// workers finish after a newer prediction's workers (issue #2827, PR #2677).
@interface PsoPredictBudget : NSObject { @public unsigned admitted; } @end
@implementation PsoPredictBudget @end
// order: admission order (the oldest finished, untaken entry is reclaimed first); demand: a build a draw asked for while it draws with its pulling stand-in (hybrid pipelines),
// outside the speculative limit; run: 0 queued, 1 taken by its worker; taking: the submit thread waits for this slot, so no admission may reclaim it after completion;
// d: the descriptor while queued (a draw that would wait for a queued build makes the pipeline itself:
// pso_pend_take); gen tells a slot's fills apart, so a worker whose slot was taken that way and filled again does nothing
static struct { pso_key k; id<MTLRenderPipelineState> ps; MTLRenderPipelineDescriptor *d; int used, done, run, demand, taking, off; uint32_t gen; uint64_t order; } g_pp[PSO_PEND_MAX];
static pthread_mutex_t g_pp_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_pp_cv = PTHREAD_COND_INITIALIZER;
static uint64_t g_pp_submit_mark, g_pp_taken, g_pp_stolen;   // Whether this submission has already been predicted (by submission serial); cumulative taken counts
static _Atomic uint64_t g_pp_kicked;   // admissions by the submit thread and base-lookup workers; also read by timing summaries
static _Atomic uint64_t g_pp_admit;   // speculative builds the predictions admitted (on the submit thread, or by a base lookup handed to the background: pso_bg_look)
static _Atomic uint64_t g_bgl_n, g_bgl_done, g_bgl_found, g_bgl_ns;   // the predictions' base lookups handed to the background (pso_bg_look), finished, of them found a base (the key's own or its pulling one), and the workers' time
static void pso_predict_rest(void);
// Whether the submission's prediction (pso_predict_rest) runs now: once per submission, at its first new miss, a draw that waits or a draw that takes a stand-in and asks for its
// own build now (fresh). The prediction can only help draws it reaches before their first encode; a draw whose own build is already under way drew in an earlier frame, and so did
// the submission around it as a rule. Run at every first miss, it replayed the same submissions in every frame of an area's stand-in warm-up and found nothing new: Ocarina of
// Time's village arrival, 5,600-12,700 predictions in 3,000 frames, 1.4-1.6 s of replay; at the first new miss 270-310, 0.16-0.21 s, with as many builds admitted and as many
// draws waiting (issue #2438)
static int pso_predict_due(int fresh) {
    if (!fresh || g_pp_submit_mark == g_sub_serial) return 0;
    g_pp_submit_mark = g_sub_serial; return 1;
}
static int pso_pend_find(const pso_key *k) { for (int i = 0; i < PSO_PEND_MAX; i++) if (g_pp[i].used && !memcmp(&g_pp[i].k, k, sizeof *k)) return i; return -1; }
static int hybrid_on(void);
// The queue of the builds hybrid draws asked for (the draw has a pulling stand-in, so nothing waits for them): serial queues at utility QoS, one per two performance cores, round
// robin. At user-initiated QoS on the global queue, a cold start's burst (777 builds on entering Zora's Domain, Breath of the Wild) ran on every core, beside the game's own threads,
// for the seconds it took. The prediction's builds (pso_pend_kick_next, and the guesses of pso_pend_kick: at most PSO_PEND_ACTIVE at a time) keep the global queue: they are draws
// of this submission that would wait, and a draw that reaches its build while still queued makes it itself (pso_pend_take)
static dispatch_queue_t pp_bg_queue(void) {
    static dispatch_queue_t q[8]; static int n; static _Atomic unsigned rr; static dispatch_once_t once;
    dispatch_once(&once, ^{ int pc = 0; size_t sz = sizeof pc; if (sysctlbyname("hw.perflevel0.physicalcpu", &pc, &sz, NULL, 0) || pc < 2) pc = 2;
        n = pc / 2 > 8 ? 8 : pc / 2;
        for (int i = 0; i < n; i++) q[i] = dispatch_queue_create("dm.nvn.pso-bg", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q[atomic_fetch_add(&rr, 1) % (unsigned)n];
}
// Draws that waited for their pipeline on the submit thread, by why no stand-in served them: 0 hybrid pipelines off, 1 merged pass or readable-tile fragment, 2 diagnostic fragment,
// 3 no pulling entry (no pulling library), 4 pulling base not made yet; count and time (prediction walks left out)
static uint64_t g_pw_n[5], g_pw_ns[5];
static const char *const g_pw_why[5] = {"hybrid off", "merged pass or readable tile", "diagnostic fragment", "no pulling entry", "pulling base not made yet"};
// Takes a slot for k (with g_pp_mu held): a free one, else the oldest finished entry no draw took (its worker no longer touches it). spec: 0 a demanded build; 1 a speculative guess,
// refused while PSO_PEND_ACTIVE speculative builds run; 2 a later draw of this submission, speculative but not refused. Returns the slot, -1 when refused or k is in the table already
static int pso_pend_slot(const pso_key *k, int spec, uint32_t *gen) {
    int slot = -1, reclaim = -1, active = 0;
    if (pso_pend_find(k) >= 0) return -1;
    for (int i = 0; i < PSO_PEND_MAX; i++) {
        if (!g_pp[i].used) { if (slot < 0) slot = i; }
        else if (!g_pp[i].done) active += !g_pp[i].demand;
        else if (!g_pp[i].taking && (reclaim < 0 || g_pp[i].order < g_pp[reclaim].order)) reclaim = i;
    }
    if (spec == 1 && active >= PSO_PEND_ACTIVE) return -1;
    if (slot < 0) slot = reclaim;
    if (slot < 0) return -1;
    g_pp[slot].k = *k; g_pp[slot].ps = nil; g_pp[slot].d = nil; g_pp[slot].done = g_pp[slot].run = g_pp[slot].taking = 0; g_pp[slot].used = 1; g_pp[slot].demand = !spec;
    g_pp[slot].order = ++g_pp_kicked; *gen = ++g_pp[slot].gen;
    return slot;
}
static void pso_pend_start(int slot, uint32_t gen, MTLRenderPipelineDescriptor *d, int off, dispatch_queue_t q) {
    pthread_mutex_lock(&g_pp_mu); if (g_pp[slot].gen == gen) { g_pp[slot].d = d; g_pp[slot].off = off; } pthread_mutex_unlock(&g_pp_mu);   // only the submit thread takes a queued build, so none has yet
    dispatch_async(q, ^{
        pthread_mutex_lock(&g_pp_mu);
        int go = g_pp[slot].used && g_pp[slot].gen == gen && !g_pp[slot].run; if (go) { g_pp[slot].run = 1; g_pp[slot].d = nil; }
        pthread_mutex_unlock(&g_pp_mu);
        if (!go) return;   // the draw made it itself
        NSError *e = nil; id<MTLRenderPipelineState> ps = pso_build(d, off, &e);
        pthread_mutex_lock(&g_pp_mu); if (g_pp[slot].gen == gen) { g_pp[slot].ps = ps; g_pp[slot].done = 1; } g_pp_done_n++; pthread_cond_broadcast(&g_pp_cv); pthread_mutex_unlock(&g_pp_mu);
    });
}
// Admission of a build of d, the descriptor of k (spec as pso_pend_slot: 1 a speculative guess, which waits for no backlog in front of a demanded compile; 2 a later draw of
// this submission as the scan met it, admitted whatever runs). Any thread (g_pp_mu)
static int pso_pend_kick_d(const pso_key *k, MTLRenderPipelineDescriptor *d, int spec, PsoPredictBudget *budget) {
    pthread_mutex_lock(&g_pp_mu); uint32_t gen = 0; int slot = -1;
    if (spec != 1 || !budget || budget->admitted < PSO_PREDICT_BUDGET) {
        slot = pso_pend_slot(k, spec, &gen);
        if (slot >= 0 && spec == 1 && budget) budget->admitted++;
    }
    pthread_mutex_unlock(&g_pp_mu);
    if (slot < 0) return 0;
    pso_pend_start(slot, gen, d, pso_off(k, (__bridge NvShader *)k->fs, 0), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    return 1;
}
static int pso_pend_kick_budget(const pso_key *k, NvShader *vs, NvShader *fs, int spec, PsoPredictBudget *budget) {   // the submit thread
    if (hm_get_id(&g_pso, k)) return 0;
    PD("J pipeline source=predict key=%016llx vs=%s fs=%s depth=%u stencil=%u samples=%u", (unsigned long long)nvn_hm_hash(k, sizeof *k), vs->key, fs ? fs->key : "-", k->dfmt, k->sfmt, k->samples);
    return pso_pend_kick_d(k, pso_desc(k, vs, fs, nil, 0), spec, budget);
}
static int pso_pend_kick_(const pso_key *k, NvShader *vs, NvShader *fs, int spec) { return pso_pend_kick_budget(k, vs, fs, spec, nil); }
// Nonblocking admission of a speculative guess: no speculative backlog in front of a demanded compile
static int __attribute__((unused)) pso_pend_kick(const pso_key *k, NvShader *vs, NvShader *fs) { return pso_pend_kick_(k, vs, fs, 1); }   // the tests' (t_pso_pending)
// A later draw of this submission as the scan met it, which would wait for its pipeline: admitted whatever runs. Its draw takes it when built, or makes it at once while it is
// still queued (pso_pend_take), so it never waits behind the others; the Metal compiler service builds them side by side
static int pso_pend_kick_next(const pso_key *k, NvShader *vs, NvShader *fs) { return pso_pend_kick_(k, vs, fs, 2); }
// A pipeline served by a hybrid stand-in is a demand even if its background build has not finished when the process exits. Record it for the next start's prepare step,
// independently of build admission; the existing writer drops the identical completion row. The map bounds early recording to 65,536 keys and avoids serializing repeated
// stand-in draws (submit thread). DM_PSO_DEMAND_RECORD=0 keeps record-on-completion. Measurements and the prepare-time tradeoff: docs/reference/shaders.md.
static nvn_hmap g_demand_rec = {.klen = sizeof(pso_key)};
static void demand_record(const pso_key *k, NvShader *vs, NvShader *fs) {
    static int on = -1; if (on < 0) { const char *ev = getenv("DM_PSO_DEMAND_RECORD"); on = !(ev && !strcmp(ev, "0")); }
    if (!on || nvn_hm_get(&g_demand_rec, k) || g_demand_rec.n >= (1u << 16)) return;
    nvn_hm_put(&g_demand_rec, k, (void *)1);
    pso_skey sk; char shex[sizeof sk * 2 + 1]; pso_skey_from(k, vs, fs, 0, NULL, &sk); pso_skey_hex(&sk, shex);
    pso_user_record("R\t%s\n", shex);
}
// The build a hybrid draw asked for while it draws with its stand-in: outside the speculative limit, on pp_bg_queue
static int pso_pend_demand(const pso_key *k, NvShader *vs, NvShader *fs) {
    if (hm_get_id(&g_pso, k)) return 0;
    pthread_mutex_lock(&g_pp_mu); uint32_t gen = 0; int slot = pso_pend_slot(k, 0, &gen); pthread_mutex_unlock(&g_pp_mu);
    if (slot < 0) return 0;
    pso_pend_start(slot, gen, pso_desc(k, vs, fs, nil, 0), pso_off(k, fs, 0), pp_bg_queue());
    return 1;
}
// Called by the submit thread: a build in the table is taken (waited for while its worker runs it; made here at once while it is still queued, so a draw that waits waits for its
// own pipeline alone, not for the builds queued before it: Breath of the Wild's cold start waited 0.5-0.9 s for one pipeline behind a burst); nil when it is not in the table
static id<MTLRenderPipelineState> pso_pend_take(const pso_key *k) {
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k); id<MTLRenderPipelineState> ps = nil;
    if (at >= 0 && !g_pp[at].done && !g_pp[at].run && g_pp[at].d) {
        MTLRenderPipelineDescriptor *d = g_pp[at].d; int off = g_pp[at].off; g_pp[at].d = nil; g_pp[at].used = 0; g_pp_stolen++;
        pthread_mutex_unlock(&g_pp_mu);
        NSError *e = nil; return pso_build(d, off, &e);
    }
    if (at >= 0) {
        g_pp[at].taking = 1;   // lookup workers may admit/reclaim while the condition wait releases g_pp_mu
        while (!g_pp[at].done) pthread_cond_wait(&g_pp_cv, &g_pp_mu);
        ps = g_pp[at].ps; g_pp[at].ps = nil; g_pp[at].used = g_pp[at].taking = 0;
    }
    pthread_mutex_unlock(&g_pp_mu);
    return ps;
}
// Without waiting: 0 = not in the table; 1 = in it, still being built; 2 = built and taken into *out (nil when the build failed)
static int pso_pend_try(const pso_key *k, id<MTLRenderPipelineState> *out) {
    pthread_mutex_lock(&g_pp_mu);
    int at = pso_pend_find(k), st = at < 0 ? 0 : g_pp[at].done ? 2 : 1;
    *out = nil;
    if (st == 2) { *out = g_pp[at].ps; g_pp[at].ps = nil; g_pp[at].d = nil; g_pp[at].used = 0; }
    pthread_mutex_unlock(&g_pp_mu);
    return st;
}
// ---- Hybrid pipelines: a draw never waits for the Metal compiler ----
// On a pipeline miss the draw first takes its pipeline without compiling (the base made already, or in the pipeline archive: pso_build_ex(..., 0)). Failing that it draws with the
// pair's pulling pipeline (<key>_bp, dmsh_pull.py: no vertex descriptor; the vertex streams are read through a per-draw layout block, pull_bind), specialized with the draw's colour
// state, while the pipeline for its vertex layout is built in the background (pso_pend_demand); a later draw takes that one when it is ready (pso_pend_try). The pulling pipeline costs
// vertex time (the run-time format decode: about 1 ms per million vertices on an M2 Max), so it only bridges those frames. Both compute the same positions: [[position, invariant]],
// built with preserved invariance, from bit-identical inputs (t_pull.m, t_pull_inv.m). Only a pulling base already made or in the archive is used (the prebuild makes them); otherwise
// the draw waits as before and the pulling base is built in the background for the pair's next miss. Not for merged passes or readable-tile or diagnostic fragments.
// DM_PSO_HYBRID=0 turns it off
static int hybrid_on(void) {
    static int v = -1;
    if (v < 0) { const char *e = getenv("DM_PSO_HYBRID"); pso4_init(); v = g_comp4 && !(e && !strcmp(e, "0")); if (!v) MLOG("hybrid pipelines off: a draw whose pipeline is not built yet waits for it"); }
    return v;
}
static nvn_hmap g_pso_hy = {.klen = sizeof(pso_key)};   // pulling pipelines by the key they stand in for (submit thread)
static int g_pso_pull;   // the pipeline pso_get_ returned reads its vertex streams through the layout block
static _Atomic uint64_t g_hy_draws, g_hy_made, g_hy_none, g_hy_kicked, g_hy_ns;   // draws with a pulling pipeline, pulling pipelines made, misses with no pulling base ready, pulling bases built for later, time
static NSMutableSet<NSData *> *g_hy_kick_set; static os_unfair_lock g_hy_lock = OS_UNFAIR_LOCK_INIT;
static MTLRenderPipelineDescriptor *pso_desc_pull(const pso_key *k, NvShader *vs, NvShader *fs) {   // pso_desc's pipeline with the pulling entry point and no vertex descriptor
    id<MTLFunction> pf = pull_fn(vs); if (!pf) return nil;
    MTLRenderPipelineDescriptor *d = pso_desc(k, vs, fs, nil, 0);
    d.vertexFunction = pf; d.vertexDescriptor = [MTLVertexDescriptor vertexDescriptor];
    return d;
}
// A pulling base neither made nor archived: built in the background for the pair's next miss, once (any thread)
static void hy_base_kick(MTLRenderPipelineDescriptor *d, int off) {
    pbase_key bk; pbase_key_of(d, off, &bk); NSData *kd = [NSData dataWithBytes:&bk length:sizeof bk];
    os_unfair_lock_lock(&g_hy_lock); if (!g_hy_kick_set) g_hy_kick_set = [NSMutableSet set];
    int fresh = ![g_hy_kick_set containsObject:kd]; if (fresh) [g_hy_kick_set addObject:kd]; os_unfair_lock_unlock(&g_hy_lock);
    if (fresh) { g_hy_kicked++; dispatch_async(pp_bg_queue(), ^{ NSError *be = nil; (void)pso_build(d, off, &be); }); }
}
static id<MTLRenderPipelineState> hybrid_get(const pso_key *k, NvShader *vs, NvShader *fs) {
    __unsafe_unretained id<MTLRenderPipelineState> h = hm_get_id(&g_pso_hy, k);
    if (h) return h;
    MTLRenderPipelineDescriptor *d = vs->has_pull ? pso_desc_pull(k, vs, fs) : nil;
    if (!d) return nil;
    NSError *e = nil; int off = k->border ? 0 : FN_OFF_BORDER;   // pair archive specialization (pre_fn, pso_offline.py): dm_blend_off = false serves every blend state of the pair
    id<MTLRenderPipelineState> ps = pso_build_ex(d, off, &e, 0);
    if (!ps) { hy_base_kick(d, off); g_hy_none++; return nil; }
    g_hy_made++; hm_put_id(&g_pso_hy, k, ps);
    return ps;
}
// The pipeline of a layout pull_conversion picks: the pulling entry, no vertex descriptor, the key's colour state (pso_desc_pull), made as any pipeline is (pso_build: from the base
// made or archived, else compiled; through the MTLDevice without the Metal 4 compiler), so a draw that would wait for its layout's pipeline waits for this one instead. With hybrid
// pipelines on and the layout's own pipeline already made (an earlier run's prewarm list, or before this rule), that one draws, converting on the CPU, until the pulling base
// is made in the background (hybrid_get): no draw waits for it. Kept with the hybrid stand-ins (g_pso_hy, the same pipelines); one that fails is not tried again, and its draws
// convert on the CPU. The key goes into the prewarm lists as the layout's (warm_task makes this pipeline for it)
static nvn_hmap g_pso_pull_fail = {.klen = sizeof(pso_key)};
static id<MTLRenderPipelineState> pull_pso(const pso_key *k, NvShader *vs, NvShader *fs) {
    __unsafe_unretained id<MTLRenderPipelineState> h = hm_get_id(&g_pso_hy, k);
    if (h) return h;
    if (hm_get_id(&g_pso_pull_fail, k)) return nil;
    if (hybrid_on() && hm_get_id(&g_pso, k)) return hybrid_get(k, vs, fs);
    MTLRenderPipelineDescriptor *d = pso_desc_pull(k, vs, fs);
    NSError *e = nil; uint64_t t0 = bp_now(); id<MTLRenderPipelineState> ps = d ? pso_build(d, pso_off(k, fs, 0), &e) : nil;
    g_bp_pso_n++; g_bp_pso_ns += bp_now() - t0;
    if (!ps) {
        if (d) MLOG("[RED] pulling pipeline for vertex %s fragment %s: %s (its draws convert their vertex attributes on the CPU)", vs->key, fs ? fs->key : "-", e.localizedDescription.UTF8String);
        hm_put_id(&g_pso_pull_fail, k, NSNull.null); return nil;
    }
    hm_put_id(&g_pso_hy, k, ps); g_hy_made++;
    pso_skey sk; char hex[sizeof sk * 2 + 1]; pso_skey_from(k, vs, fs, 0, NULL, &sk); pso_skey_hex(&sk, hex);
    pso_record("R\t%s\n", hex); pso_user_record("R\t%s\n", hex);
    return ps;
}
void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none) { *draws = g_hy_draws; *made = g_hy_made; *none = g_hy_none; }
// The pipeline time line of the timing summary (hle_nvn.c), the difference since the last call
void mtl_pso_line(char *o, size_t n) {
    // A background admission may also finish between independent loads: snapshot
    // all three together so in-flight cannot underflow from mixed instants.
    pthread_mutex_lock(&g_pp_mu); uint64_t kicked = g_pp_kicked, done = g_pp_done_n, stolen = g_pp_stolen; pthread_mutex_unlock(&g_pp_mu);
    static uint64_t p[22]; uint64_t v[22] = {g_bp_pso_ns, g_bp_lib_ns, g_pt_hit_n, g_pt_hit_ns, g_pt_miss_n, g_pt_miss_ns, g_pb_spec, g_pt_spec_ns, g_pb_built, g_pt_comp_ns, g_pt_pred_n, g_pt_pred_ns, g_hy_draws, g_hy_made, kicked, done, stolen, g_hy_none, g_pp_admit, g_bgl_n, g_bgl_found, g_bgl_ns};
    uint64_t d[22]; for (int i = 0; i < 22; i++) d[i] = v[i] - p[i];
    snprintf(o, n, "on the submit thread pipelines %.1f ms, shader libraries %.1f ms, predictions %llu / %.1f ms (builds admitted %llu); all threads: archive lookups found %llu / %.1f ms, found none %llu / %.1f ms, "
             "specializations %llu / %.1f ms, bases compiled %llu / %.1f ms; draws with a hybrid pipeline %llu (%llu pulling pipelines made, %llu misses with no pulling base ready); background builds kicked %llu, done %llu, made by the draw %llu, in flight %llu; base lookups handed to the background %llu (found %llu, %.1f ms)",
             d[0] / 1e6, d[1] / 1e6, (unsigned long long)d[10], d[11] / 1e6, (unsigned long long)d[18], (unsigned long long)d[2], d[3] / 1e6, (unsigned long long)d[4], d[5] / 1e6, (unsigned long long)d[6], d[7] / 1e6,
             (unsigned long long)d[8], d[9] / 1e6, (unsigned long long)d[12], (unsigned long long)d[13], (unsigned long long)d[17], (unsigned long long)d[14], (unsigned long long)d[15], (unsigned long long)d[16], (unsigned long long)(v[14] - v[15] - v[16]),
             (unsigned long long)d[19], (unsigned long long)d[20], d[21] / 1e6);
    memcpy(p, v, sizeof p);
    static uint64_t pn[5], pns[5]; size_t o2 = strlen(o);
    o2 += snprintf(o + o2, n > o2 ? n - o2 : 0, "; draws that waited:");
    for (int i = 0; i < 5; i++) if (g_pw_n[i] > pn[i]) o2 += snprintf(o + o2, n > o2 ? n - o2 : 0, " %s %llu / %.1f ms,", g_pw_why[i], (unsigned long long)(g_pw_n[i] - pn[i]), (g_pw_ns[i] - pns[i]) / 1e6);
    memcpy(pn, g_pw_n, sizeof pn); memcpy(pns, g_pw_ns, sizeof pns);
    static uint64_t pca, pcr, pcb, pcl, pcn; uint64_t ca = g_cps_arch, cr = g_cps_run, cb = g_cps_built, cl = g_cps_look_ns, cn = g_cps_build_ns;   // compute pipelines (cps_new_b)
    if (ca > pca || cr > pcr || cb > pcb)
        snprintf(o + o2, n > o2 ? n - o2 : 0, "; compute pipelines from the archive bundled with the app %llu, from this device's archive %llu, built %llu (%.1f ms looking up, %.1f ms building)",
                 (unsigned long long)(ca - pca), (unsigned long long)(cr - pcr), (unsigned long long)(cb - pcb), (cl - pcl) / 1e6, (cn - pcn) / 1e6);
    pca = ca; pcr = cr; pcb = cb; pcl = cl; pcn = cn;
}
