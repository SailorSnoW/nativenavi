// ---------- NVN2 task and mesh shaders: nvnCommandBufferDrawMeshTasks ----------
// A program's task and mesh shaders are Metal object and mesh functions (dmsh_meshstage.py), kept in stages 3 and 4 (sh[3], sh[4]; hle_nvn.c st_host). The draw goes through draw() with its
// pixel stage alone, a program of its own (g_mtk_raster), so it takes the render pass and the pixel stage's state like any draw; mtk_pso gives its pipeline and
// mtk_encode binds the two stages and draws. The mesh stage shares the vertex stage's table, and a task shader has the object stage's; it reads the
// draw's {count, first} at buffer 29 (DMSH_MESH_DRAW_SLOT). A mesh shader with no task has an object function of its own (mesh_obj, dmsh_meshstage.py main0_o), which reads the same:
// Metal holds a mesh grid to DMSH_MESH_GRID threadgroups, so it launches the draw's workgroups in blocks of that many, one object threadgroup a block. Native mesh draws have no
// equivalent fallback: the demanded pipeline must finish compiling before its first draw, so each one built at a draw is recorded as an M row (pso_user.tsv) and the prewarm builds
// the rows before the game reaches them (docs/reference/shaders.md, Native mesh pipelines compiled at their first draw)
static struct { int on, miss; NvShader *ts, *ms; uint32_t first, count; uint64_t ind, ind_rec; } g_mtk;   // ind: an indirect draw's threadgroups (mdx_mask), ind_rec: its {count, first} record
static NvProg *g_mtk_raster;   // while a task / mesh draw goes through draw_: the program it draws with, its pixel stage alone, in place of the bound one
static int mtk_drawing(void) { return g_mtk.on; }
static void draw_(const p_draw *dw, const p_draw_idx *di);
static uint64_t g_mtk_n, g_mtk_wait;
static nvn_hmap g_pso_mtk = {.klen = sizeof(pso_key)};   // only the submit thread uses it: prewarm workers build pipelines (mtk_build) and the submit thread claims them (warm_claim)
// A mesh pipeline must have a fragment function, where a vertex one need not (validateWithDevice). A task / mesh program with no pixel shader draws depth only (Ocarina of Time's foliage shadows).
// Noted for Metal 4 (fn4_note) so that such a pipeline goes through the compiler and the archive like the others
static id<MTLFunction> mtk_nop_fragment(void) {
    static id<MTLFunction> fn; static dispatch_once_t once;
    dispatch_once(&once, ^{ NSError *e = nil; id<MTLLibrary> l = dev_lib(@"fragment void dm_mesh_nop() {}", &e);
        fn = fn4_note([l newFunctionWithName:@"dm_mesh_nop"], l, @"dm_mesh_nop", nil); if (!fn) MLOG("[RED] the empty fragment function did not compile: %s", e.localizedDescription.UTF8String); });
    return fn;
}
// The Metal 4 descriptor of a native mesh pipeline, whole: no base with open colour state, as a game has some hundred of them, each with functions of its own. Its depth / stencil
// formats are not named (Metal 4 pipelines serve any). nil when a function has no Metal 4 descriptor (fn4_of: the diagnostic fragment overrides)
static MTL4MeshRenderPipelineDescriptor *mtk_desc4(MTLRenderPipelineDescriptor *d, NvShader *ts, NvShader *ms, id<MTLFunction> ff, int border) {
    MTL4FunctionDescriptor *mf = fn4_of_b(ms->fn, !border), *of = ts ? fn4_of_b(ts->fn, !border) : fn4_of(ms->mesh_obj), *f4 = fn4_of_b(ff, !border);
    if (!mf || !of || !f4) return nil;
    MTL4MeshRenderPipelineDescriptor *m = [MTL4MeshRenderPipelineDescriptor new];
    m.meshFunctionDescriptor = mf; m.fragmentFunctionDescriptor = f4; m.maxTotalThreadsPerMeshThreadgroup = ms->mesh_threads;
    m.objectFunctionDescriptor = of; m.payloadMemoryLength = ts ? (ts->mesh_payload + 15u) & ~15u : 16; m.maxTotalThreadsPerObjectThreadgroup = ts ? ts->mesh_threads : 1;   // with no task: a block's first workgroup, one thread a block
    m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageState = d.alphaToCoverageEnabled ? MTL4AlphaToCoverageStateEnabled : MTL4AlphaToCoverageStateDisabled;
    for (int i = 0; i < 8; i++) pso_col4(m.colorAttachments[i], d.colorAttachments[i]);
    return m;
}
// A native mesh pipeline for key k (mesh shader ms, optional task shader ts, optional pixel shader fs), on any thread, kept in the pipeline archive as compute pipelines are
// (cps_new_b). With the Metal 4 compiler it is looked up in the archive files (the app's bundle among them): a hit costs a lookup, where a build took 1.9-5 s on an iPad Pro (M5)
// and up to 40 s on an M1 with Metal's own cache cold (Ocarina of Time). On a miss the MTLDevice builds it, which the system's shader cache serves when it holds it, and the
// Metal 4 compiler compiles it again into the current flush period in the background (arch_harvest), so the next start takes it from the file that period writes; the capture
// compiles it whole, which the draw or the prewarm does not wait for. A pipeline written with other pipelines may sit in a plain file, which a compacting prewarm replaces, so one
// taken from such a file then is captured again. Without the compiler, or for a function with no Metal 4 descriptor, only the MTLDevice build. *archived: taken from the archive
static id<MTLRenderPipelineState> mtk_build(const pso_key *k, NvShader *ts, NvShader *ms, NvShader *fs, NSError **error, int *archived) {
    if (archived) *archived = 0;
    if (!k || !ms || !ms->fn || (!ts && !ms->mesh_obj)) return nil;
    MTLRenderPipelineDescriptor *d = pso_desc(k, nil, fs, nil, 0);
    if (!pso_ds_valid((uint32_t)d.depthAttachmentPixelFormat, (uint32_t)d.stencilAttachmentPixelFormat)) return nil;
    id<MTLFunction> ff = d.fragmentFunction ?: mtk_nop_fragment();
    pso4_init();
    MTL4MeshRenderPipelineDescriptor *m4 = g_comp4 ? mtk_desc4(d, ts, ms, ff, k->border) : nil;
    if (m4) {
        id<MTLRenderPipelineState> ps = nil; int from_run = 0;
        ps = arch_lookup(m4, 0, &from_run);   // a period of mesh pipelines alone is a p file (no vertex layout)
        if (ps) {
            g_mtk_arch++; if (archived) *archived = 1;
            if (from_run && g_compact_enabled && atomic_load(&g_warm_running)) arch_harvest(m4, 1);
            return ps;
        }
    }
    MTLMeshRenderPipelineDescriptor *m = [MTLMeshRenderPipelineDescriptor new];
    m.meshFunction = fn_dev_b(ms->fn, !k->border); m.fragmentFunction = fn_dev_b(ff, !k->border); m.maxTotalThreadsPerMeshThreadgroup = ms->mesh_threads;
    m.objectFunction = ts ? fn_dev_b(ts->fn, !k->border) : fn_dev(ms->mesh_obj); m.payloadMemoryLength = ts ? (ts->mesh_payload + 15u) & ~15u : 16; m.maxTotalThreadsPerObjectThreadgroup = ts ? ts->mesh_threads : 1;
    m.rasterSampleCount = d.rasterSampleCount; m.alphaToCoverageEnabled = d.alphaToCoverageEnabled;
    for (int i = 0; i < 8; i++) m.colorAttachments[i] = d.colorAttachments[i];
    m.depthAttachmentPixelFormat = d.depthAttachmentPixelFormat; m.stencilAttachmentPixelFormat = d.stencilAttachmentPixelFormat;
    id<MTLRenderPipelineState> ps = dev_mps(m, error);
    if (ps) { if (m4) { g_mtk_built++; arch_harvest(m4, 0); } else g_mtk_dev++; }
    return ps;
}
static void mtk_bind_key(pso_key *k, NvShader *ts, NvShader *ms, NvShader *fs) {
    k->vs = (__bridge void *)ms; k->fs = (__bridge void *)fs;
    k->fsfn = (__bridge void *)(ts ? ts->fn : ms ? ms->fn : nil);   // the third function, distinct from a specialized fragment
}
static void mtk_claim(pso_key *k, NvShader *ts, NvShader *ms, NvShader *fs, id<MTLRenderPipelineState> ps) {
    if (!ms || !ms->fn || !ps) return;
    mtk_bind_key(k, ts, ms, fs);
    id current = hm_get_id(&g_pso_mtk, k);
    if (!current || current == NSNull.null) hm_put_id(&g_pso_mtk, k, ps);
}
static id<MTLRenderPipelineState> mtk_pso(NvProg *raster) {
    if (!raster || !g_mtk.ms || !g_mtk.ms->fn) return nil;
    NvShader *ts = g_mtk.ts, *ms = g_mtk.ms, *fs = raster->sh[1];
    pso_key k; pso_key_fill(&k, ms, fs, g_pass.cfmt, g_pass.dfmt, g_pass.sfmt, g_pass.samples, 0);
    k.border |= border_need(ts, 3);
    memset(k.attr, 0, sizeof k.attr); memset(k.attr_hi, 0, sizeof k.attr_hi); memset(k.stream, 0, sizeof k.stream);
    mtk_bind_key(&k, ts, ms, fs);
    id ps = hm_get_id(&g_pso_mtk, &k);
    if (ps) return ps == NSNull.null ? nil : ps;
    NSError *error = nil; uint64_t start = bp_now(); int archived = 0;
    ps = mtk_build(&k, ts, ms, fs, &error, &archived);
    uint64_t elapsed = bp_now() - start;
    g_bp_pso_n++; g_bp_pso_ns += elapsed;
    const char *task = ts ? ts->key : "-";
    if (!ps) {
        MLOG("[RED] task / mesh pipeline failed: task %s mesh %s fragment %s: %s", task, ms->key, fs ? fs->key : "-",
             error ? error.localizedDescription.UTF8String : "invalid depth/stencil formats");
        hm_put_id(&g_pso_mtk, &k, NSNull.null); return nil;
    }
    hm_put_id(&g_pso_mtk, &k, ps);
    k.fsfn = NULL;   // The third function has its own key column; this is not a specialized fragment.
    pso_skey sk; char hex[sizeof sk * 2 + 1];
    pso_skey_from(&k, ms, fs, 0, NULL, &sk); pso_skey_hex(&sk, hex);
    pso_record("M\t%s\t%s\n", task, hex); pso_user_record("M\t%s\t%s\n", task, hex);
    MLOG("task / mesh pipeline: task %s mesh %s fragment %s, %.1f ms%s required at frame %llu", task, ms->key, fs ? fs->key : "-",
         elapsed / 1e6, archived ? " (from the pipeline archive)" : "", (unsigned long long)atomic_load(&g_cur_frame));
    return ps;
}
// The scratch buffer of the mesh shaders whose stage array is a device slice (reflection mesh_spill, dmsh_meshstage.py to_stage, DMSH_MESH_SCRATCH_* in dmsh_abi.h): one buffer for every draw,
// made on first use, in shared storage so that the host writes the head's words and reads the overflow count. Its slices must outnumber the groups the GPU runs at once: with the stage array out of
// threadgroup memory an Apple GPU core holds 3072 threads, 96 groups of 32, and twice that keeps a claim to about two tickets. The host reads its GPU's cores (gpu-core-count), and asks for
// 8192 slices when it cannot. The flags start 0 as a new buffer's bytes do, and a group zeroes its own slice, so nothing clears the buffer between draws (gpu_cores: nvn_mtl.m)
static uint32_t *mesh_scratch(uint64_t *gpu) {
    static uint64_t addr; static uint32_t *head; static __strong id<MTLBuffer> buf; static dispatch_once_t once;
    dispatch_once(&once, ^{
        unsigned cores = gpu_cores(), want = cores ? cores * 96 * 2 : 8192, n = 1024;
        while (n < want && n < DMSH_MESH_SCRATCH_SLICES) n <<= 1;
        buf = [g_dev newBufferWithLength:DMSH_MESH_SCRATCH_HEAD + (size_t)n * DMSH_MESH_SCRATCH_STRIDE options:MTLResourceStorageModeShared];
        if (!buf) MDIE("failed to create the mesh scratch buffer (%u slices)", n);
        head = buf.contents; head[DMSH_MESH_SCRATCH_MASK] = n - 1; head[DMSH_MESH_SCRATCH_DONE] = (uint32_t)atomic_load(&g_done);
        res_add(buf); res_commit(); addr = buf.gpuAddress;
        atomic_store(&g_mesh_scr, head);   // post_complete keeps DONE from now on (one it missed before this is only a later reclaim)
        MLOG("mesh scratch: %u slices of %u bytes for the mesh shaders whose stage memory is a device slice (%u GPU cores%s)", n, DMSH_MESH_SCRATCH_STRIDE, cores, cores ? "" : ", not read");
    });
    *gpu = addr;
    return head;
}
// Before a draw of a spilled mesh shader: the tag its groups claim slices with, the fence of the submission being encoded (or the next one), so that no tag a running group holds is DONE or
// older (the commit comes after this store, and the tag only grows); a tag is never 0, a free flag. The groups that found no free slice drew nothing: a red line, the pool is too small
static void mesh_scratch_bind(id<MTL4RenderCommandEncoder> en) {
    uint64_t gpu; uint32_t *head = mesh_scratch(&gpu);
    uint64_t f = atomic_load(&g_sub_open); if (!f) f = atomic_load(&g_fence) + 1;
    __atomic_store_n(head + DMSH_MESH_SCRATCH_TAG, (uint32_t)f ? (uint32_t)f : 1u, __ATOMIC_RELAXED);
    uint32_t over = __atomic_load_n(head + DMSH_MESH_SCRATCH_OVER, __ATOMIC_RELAXED);
    if (over) RED_ONCE(173, "mesh scratch: %u mesh groups found none of the %u slices free and drew nothing (dmsh_abi.h DMSH_MESH_SCRATCH_*: the GPU ran more groups at once than the pool was sized for)", over, head[DMSH_MESH_SCRATCH_MASK] + 1);
    enc_r_vbuf(en, DMSH_MESH_SCRATCH_SLOT, gpu);   // the mesh function's stage array (the table persists; enc_r_vbuf skips an unchanged address)
}
static void mtk_encode(id<MTL4RenderCommandEncoder> en) {
    NvShader *ts = g_mtk.ts, *ms = g_mtk.ms;
    [en setArgumentTable:g_vt atStages:MTLRenderStageMesh];
    if (ms->rf.mesh_spill) mesh_scratch_bind(en);
    uint64_t draw_gpu = g_mtk.ind_rec;
    if (!g_mtk.ind) { id<MTLBuffer> b; size_t o = arena_alloc(16, &b); uint32_t rec[2] = {g_mtk.count, g_mtk.first}; memcpy((uint8_t *)b.contents + o, rec, 8); draw_gpu = b.gpuAddress + o; }
    [en setArgumentTable:g_ot atStages:MTLRenderStageObject];
    if (ts) { args_dirty(3); encode_args(en, ts, 3, 0); }
    za_check(draw_gpu, "object table slot", DMSH_MESH_DRAW_SLOT);
    if (rec_on()) rec_table_op(RO_T_ADDR, g_ot, draw_gpu, DMSH_MESH_DRAW_SLOT); else [g_ot setAddress:draw_gpu atIndex:DMSH_MESH_DRAW_SLOT];
    args_dirty(4); encode_args(en, ms, 4, 1);
    // A task shader's threadgroups are the draw's workgroups; with no task, the object function's are blocks of DMSH_MESH_GRID of them (mdx_mask writes their count at +16)
    MTLSize to = MTLSizeMake(ts ? ts->mesh_threads : 1, 1, 1), tm = MTLSizeMake(ms->mesh_threads, 1, 1);
    if (g_mtk.ind) za_check(g_mtk.ind, "indirect mesh draw record", -1);
    if (g_mtk.ind) [en drawMeshThreadgroupsWithIndirectBuffer:g_mtk.ind + (ts ? 0 : 16) threadsPerObjectThreadgroup:to threadsPerMeshThreadgroup:tm];
    else [en drawMeshThreadgroups:MTLSizeMake(ts ? g_mtk.count : (g_mtk.count + DMSH_MESH_GRID - 1) / DMSH_MESH_GRID, 1, 1) threadsPerObjectThreadgroup:to threadsPerMeshThreadgroup:tm];
}
// DrawMeshTasksIndirect: {count, first}, which the game's culling writes earlier in the submission, while Metal's indirect mesh draw takes {x, y, z}. mdx_run copies the count to a {count, 1, 1} on the
// GPU (and the object blocks of a mesh shader with no task, mtk_encode), before the render pass and for every such draw up to the next write, and the task or object function reads
// {count, first} from the record itself. Reading the record on the CPU instead took a wait for the GPU at each hazard:
// 73 % of the submit thread in Ocarina of Time's Kokiri Forest, frames of 90 ms and more against 25 ms of GPU work
static void mesh_tasks_draw(const p_draw *dw) {
    if (!S.prog || (!dw->ind && dw->count <= 0)) return;
    // Diagnostic DM_NVN_SKIP_MESH=1 (off by default): drops every task / mesh draw, as DM_NVN_SKIP_FS drops draws by fragment shader, to measure what they cost in all (the picture is wrong)
    { static int skip = -1; if (skip < 0) { const char *e = getenv("DM_NVN_SKIP_MESH"); skip = e && atoi(e); if (skip) MLOG("DM_NVN_SKIP_MESH: task / mesh draws dropped (diagnostic; the picture is wrong)"); } if (skip) return; }
    __unsafe_unretained NvProg *pr = prog_get(S.prog);
    if (skip_fs_draw(pr)) return;   // DM_NVN_SKIP_FS by the fragment shader, as for other draws
    NvShader *ts = pr ? pr->sh[3] : nil, *ms = pr ? pr->sh[4] : nil;
    if (ts && ts->mesh_kind != 1) ts = nil;
    if (!ms || ms->mesh_kind != 2 || !ms->fn || (ts ? !ts->fn : !ms->mesh_notask || !ms->mesh_obj)) {
        RED_ONCE(150, "DrawMeshTasks without its task and mesh shaders (task %s mesh %s): draw skipped", ts ? ts->key : "-", ms ? ms->key : "-"); return; }
    image_prepare(pr, 1u << 3 | 1u << 4);
    if (g_nubd && (g_enc || ubd_lazy()) && ((ts && ubd_conflict_sh(ts, 3)) || ubd_conflict_sh(ms, 4))) { g_ubd_conflicts++; rp_end(); ubd_land(); }   // as draw_ does for its stages
    NvProg *raster = pr->mesh_raster;   // the pixel stage alone
    if (!raster || raster->sh[1] != pr->sh[1] || raster->sh[0]) { raster = [NvProg new]; raster->sh[1] = pr->sh[1]; pr->mesh_raster = raster; }
    uint64_t ind = 0, ind_rec = 0;
    if (dw->ind) { void *mb; uint64_t mo;
        if (!mdx_find(dw->ind, dw->ind, &ind)) { mdx_run(dw, NULL); if (!mdx_find(dw->ind, dw->ind, &ind)) ind = 0; }
        if (!ind || !mtl_gpu_addr(dw->ind, 8, &mb, &mo, &ind_rec)) { RED_ONCE(154, "DrawMeshTasksIndirect record at %#llx is in no memory pool: draw skipped", (unsigned long long)dw->ind); return; }
    }
    PD("m %llu mesh tasks task=%s mesh=%s fs=%s first=%d count=%d indirect=%#llx", g_stat_pass + !g_enc, ts ? ts->key : "-", ms->key, pr->sh[1] ? pr->sh[1]->key : "-", dw->first, dw->count, (unsigned long long)dw->ind);   // the draw_ below is a stand-in triangle
    g_mtk.on = 1; g_mtk.miss = 0; g_mtk.ts = ts; g_mtk.ms = ms; g_mtk.first = (uint32_t)dw->first; g_mtk.count = (uint32_t)dw->count;
    g_mtk.ind = ind; g_mtk.ind_rec = ind_rec;
    g_mtk_raster = raster;
    p_draw triangle = {.prim = 4, .count = 3, .instances = 1};
    draw_(&triangle, NULL);
    g_mtk_raster = nil; g_mtk.on = 0; args_dirty_all();
    if (g_mtk.miss) { g_mtk_wait++; return; }
    swrite_note(pr); g_wr_epoch++;
    if (!g_mtk_n++) MLOG("task / mesh: first draw, task %s mesh %s fragment %s, %u workgroups from %u%s", ts ? ts->key : "-", ms->key, pr->sh[1] ? pr->sh[1]->key : "-", g_mtk.count, g_mtk.first, ind ? " (indirect)" : "");
}
