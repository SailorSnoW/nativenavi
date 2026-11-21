// The vertex dump (DM_NVN_VSDUMP=<file>, DM_NVN_VSDUMP_KEY=<vertex shader key or *>, DM_NVN_VSDUMP_FROM=<frame>, DM_NVN_VSDUMP_MAX=<n>; diagnostic): what a vertex shader
// writes for the vertices of one of its draws, taken by a draw of its own through the shader's capture entry point. The build gives every vertex shader that entry
// (dmsh_geometry.capture: main0_gs, the reflection's entry_geometry_capture; it was made for the geometry stage's lowering, which this host does not have, as no shader of the
// game is a geometry shader), and it stores each vertex's outputs as a record of VSDUMP_WORDS words at the address the capture block names.
static void lc_land(void);
enum { VSDUMP_WORDS = 144 };   // words a vertex's record has (dmsh_geometry.py CAPTURE_WORDS)
typedef struct { uint64_t dst, pad; uint32_t info[4]; uint32_t stride; int16_t slot[VSDUMP_WORDS]; } vsdump_capture;   // the capture block at vertex buffer 13: dst the records, info {first vertex, vertices, base instance, instances}, stride the words a record keeps, slot the record word of each capture word
_Static_assert(__builtin_offsetof(vsdump_capture, info) == 16 && __builtin_offsetof(vsdump_capture, stride) == 32 && __builtin_offsetof(vsdump_capture, slot) == 36, "dm_geometry_capture layout");
// The capture entry point of vertex shader vs, from its library at the first dump of it: no draw of the game uses it, so shader_load leaves it alone
static id<MTLFunction> vsdump_fn(NvShader *vs) {
    if (vs->dump_fn || vs->dump_tried) return vs->dump_fn;
    vs->dump_tried = 1;
    NSData *jd = msl_data([NSString stringWithFormat:@"%@/%s.json", g_msl_dir, vs->key]);
    NSDictionary *j = jd ? [NSJSONSerialization JSONObjectWithData:jd options:0 error:nil] : nil;
    NSString *name = [j isKindOfClass:NSDictionary.class] ? j[@"entry_geometry_capture"] : nil;
    if (![name isKindOfClass:NSString.class]) { MLOG("vertex dump %s: the shader has no capture entry point", vs->key); return nil; }
    NSError *e = nil; id<MTLLibrary> lib = lib_from_file([NSString stringWithFormat:@"%@/%s.metallib", g_msl_dir, vs->key], &e);
    if (lib) vs->dump_fn = fn_spec(lib, name, [MTLFunctionConstantValues new], &e);
    if (!vs->dump_fn) MLOG("[RED] vertex dump %s: capture entry point %s failed: %s", vs->key, name.UTF8String, e.localizedDescription.UTF8String);
    return vs->dump_fn;
}
static id<MTLRenderPipelineState> vsdump_pso(NvShader *vs) {
    pso_key k; static const uint32_t cf[8]; pso_key_fill(&k, vs, nil, cf, 0, 0, 1, 0); k.fsfn = VSDUMP_MARK;
    id<MTLRenderPipelineState> ps = hm_get_id(&g_pso, &k);
    if (ps || hm_get_id(&g_pso_fail, &k)) return ps;
    NSError *e = nil;
    ps = pso_build_dev(pso_desc(&k, vs, nil, nil, 0), !k.border, &e);   // a vertex shader that samples declares the border corrections' constant: specialized, as every device pipeline's (fn_dev_b)
    if (ps) hm_put_id(&g_pso, &k, ps);
    else { hm_put_id(&g_pso_fail, &k, NSNull.null); MLOG("[RED] vertex dump %s: no capture pipeline: %s", vs->key, e.localizedDescription.UTF8String); }
    return ps;
}
// One snapshot through the shader's own vertex fetch. Shaders with writes are refused: observing them must not run their side effects twice
static void vertex_dump(NvShader *vs, const p_draw *dw, const p_draw_idx *di) {
    static const char *key, *path; static int configured, captured, limit; static uint64_t frame;
    if (!configured) { key = getenv("DM_NVN_VSDUMP_KEY"); path = getenv("DM_NVN_VSDUMP");
        const char *f = getenv("DM_NVN_VSDUMP_FROM"), *n = getenv("DM_NVN_VSDUMP_MAX");
        frame = f ? strtoull(f,NULL,0) : 0; limit = n ? atoi(n) : key && !strcmp(key,"*") ? 256 : 1; configured = 1; }
    int all = key && !strcmp(key,"*");
    if (captured >= limit || !key || !path || atomic_load(&g_cur_frame) < frame || !vs || (!all && strncmp(vs->key, key, 16)) ||
        (dw ? dw->ind || dw->mdi_cnt : di->ind || di->mdi_cnt) || S.rast_discard) return;
    for (int i = 0; i < vs->rf.nb; i++) if (vs->rf.b[i].rw) return;
    int count = dw ? dw->count : di->count, instances = dw ? dw->instances : di->instances;
    if (count <= 0 || instances > 1 || (di && (di->itype < 0 || di->itype > 2)) || !vsdump_fn(vs)) return;
    unsigned number = captured++; lc_land(); rp_flush(); mdi_wait();
    uint32_t lo = dw ? (uint32_t)dw->first : UINT32_MAX, hi = dw ? lo + count - 1 : 0;
    if (di) {
        int size = 1 << di->itype;
        if (!nvn_host_readable(di->ibuf, (uint64_t)count * size)) return;
        for (int i = 0; i < count; i++) { uint32_t n = 0; memcpy(&n, (void *)(uintptr_t)(di->ibuf + (uint64_t)i * size), size);
            if (S.restart.on && n == (S.restart.index & (size == 1 ? 255u : size == 2 ? 65535u : UINT32_MAX))) continue;
            lo = MIN(lo, n); hi = MAX(hi, n); }
        if (lo == UINT32_MAX || (int64_t)lo + di->base_vertex < 0 || (int64_t)hi + di->base_vertex > UINT32_MAX) return;
        lo += di->base_vertex; hi += di->base_vertex;
    }
    uint64_t span = (uint64_t)hi - lo + 1, bytes = span * 144 * 4;
    if (!span || bytes > (64u << 20)) return;
    id<MTLRenderPipelineState> ps = vsdump_pso(vs); if (!ps) return;
    id<MTLBuffer> output; size_t offset = arena_alloc(bytes + 512, &output);
    uint64_t gpu = output.gpuAddress + offset;
    uint64_t info[4] = {gpu + 512, 0, (uint64_t)lo | span << 32, (uint32_t)(dw ? dw->base_instance : di->base_instance) | 1ull << 32};
    vsdump_capture gc = {info[0], 0, {(uint32_t)lo, (uint32_t)span, (uint32_t)info[3], 1}, VSDUMP_WORDS, {0}};
    for (int k = 0; k < VSDUMP_WORDS; k++) gc.slot[k] = (int16_t)k;   // every word, in order
    memcpy((uint8_t *)output.contents + offset, &gc, sizeof gc);
    p_draw range = {.first = (int32_t)lo, .count = (int32_t)span, .instances = 1, .base_instance = (int32_t)(info[3] & UINT32_MAX)};
    vbuf_prepare(&vs->rf, &range, NULL);   // the vertices the capture reads: streams it reads past get zeros there (vbuf_need)
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    MTL4RenderPassDescriptor *rd = [MTL4RenderPassDescriptor new]; rd.renderTargetWidth = rd.renderTargetHeight = rd.defaultRasterSampleCount = 1;
    id<MTL4RenderCommandEncoder> en = enc_r_begin(rd); enc_r_pso(en, ps);
    { int bad = bind_vbufs(en, &vs->rf); if (bad >= 0) { RED_ONCE(158, "vertex stream %d's buffer %#llx is in no memory pool; draw skipped", bad, (unsigned long long)S.vbuf[bad].address); enc_r_end(en); return; } }
    vconv_bind(en, vs, 1, &range, NULL); args_dirty_all(); encode_args(en, vs, 0, 1); enc_r_vbuf(en, 13, gpu);
    enc_r_draw_instanced(en, MTLPrimitiveTypePoint, lo, span, 1, (uint32_t)info[3]); enc_r_end(en);
    mdi_wait();
    char filename[1200]; if (all) snprintf(filename,sizeof filename,"%s_%04u_%s.bin",path,number,vs->key); else strlcpy(filename,path,sizeof filename);
    FILE *f = fopen(filename, "wb"); if (f) { uint32_t header[4] = {lo, (uint32_t)span, all ? 4u : 144u, (uint32_t)info[3]}; fwrite(header, sizeof header, 1, f);
        if (all) for (uint64_t i=0;i<span;i++) fwrite((uint8_t *)output.contents + offset + 512 + i*144*4,1,16,f);
        else fwrite((uint8_t *)output.contents + offset + 512, 1, bytes, f); fclose(f); }
    MLOG("vertex dump %s: first %u, count %llu, %s", vs->key, lo, (unsigned long long)span, filename);
    args_dirty_all(); g_enc_pso = nil; g_enc_dss = nil; g_ed.ok = 0;
}
