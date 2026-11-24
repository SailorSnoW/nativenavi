// Copies: texture to buffer and back, ReadTexels, copies a blit cannot do, DrawTexture, compressed-alias pairs, texture to texture.
// Metal does not check a blit's level: a copy at a level the host texture lacks is skipped.
static int tex_level_ok(id<MTLTexture> t, NSUInteger level, const char *what) {
    if (level < t.mipmapLevelCount) return 1;
    RED_ONCE(116, "%s at level %lu of a texture with %lu levels: skipped (first case)", what, (unsigned long)level, (unsigned long)t.mipmapLevelCount);
    return 0;
}
// RGB565-family red / blue exchange on the GPU: the copy runs after earlier GPU writes, so its bytes can only be converted there. src == dst converts in place.
static int rb_swap_gpu(uint64_t src, size_t row, size_t img, uint64_t dst, size_t drow, size_t dimg, size_t wb, size_t rows, size_t nz, int shift) {
    static id<MTLComputePipelineState> ps; static int failed;
    if (failed) return 0;
    if (!ps) {
        NSError *e = nil; id<MTLLibrary> lib = host_lib("rb_swap", &e);
        id<MTLFunction> fn = [lib newFunctionWithName:@"rb_swap"];
        ps = fn ? dev_cps(fn, &e) : nil;
        if (!ps) { failed = 1; MLOG("[RED] cannot create the red / blue exchange for RGB565-family copies: %s", e.localizedDescription.UTF8String); return 0; }
    }
    id<MTLBuffer> pb; size_t po = arena_alloc(48, &pb);
    struct { uint64_t s, d; uint32_t w, h, row, img, drow, dimg, shift; } *p = (void *)((uint8_t *)pb.contents + po);
    *p = (typeof(*p)){src, dst, (uint32_t)(wb / 2), (uint32_t)rows, (uint32_t)row, (uint32_t)img, (uint32_t)drow, (uint32_t)dimg, (uint32_t)shift};
    id<MTL4ComputeCommandEncoder> e = dispatch_enc();
    at_addr(g_ct, pb.gpuAddress + po, 0, "row swap table slot", 0); enc_c_pso(e, ps);
    enc_c_dispatch(e, MTLSizeMake((wb / 2 + 63) / 64, rows, nz), MTLSizeMake(64, 1, 1));
    static int once; if (!once++) MLOG("RGB565-family copy between guest memory and a host texture: red and blue exchanged on the GPU (first case %zux%zux%zu texels)", wb / 2, rows, nz);
    return 1;
}
// 0 for other formats and for a texture on a pool's buffer, which holds the guest's bits
static int rb_copy_shift(const tex_t *t, id<MTLTexture> mt) { return mt.buffer ? 0 : rb_packed_shift(t->d.format); }
// strides from SetCopyRowStride / SetCopyImageStride, 0 = tightly packed
static void copy_t2b(const p_copy_b2t *c, uint32_t size) {
    rp_flush();   // pending clears land before the read
    g_wr_epoch++; g_mdx_epoch++;
    id<MTLTexture> t = tex_mtl_all(c->tex);
    if (!t) return;
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (!tex_level_ok(t, level, "texture-to-buffer copy")) return;
    const p_copy_strides *cs = copy_strides(c, size); int64_t rs = cs ? cs->row : S.copy_stride, is = cs ? cs->image : S.copy_img_stride;   // S: OP_COPY_STRIDE of an older capture
    size_t row = rs ? (size_t)rs : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = is ? (size_t)is : row * rows;
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(14, "texture→buffer copy destination is in no memory pool"); return; }
    sbw_note(c->src, img * nz, SBW_GPU);
    id<MTL4ComputeCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_texture_buffer(b, t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset), MTLSizeMake(c->r.width, c->r.height, nz), (__bridge id<MTLBuffer>)mb, mo, row, img);
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture_buffer(b, t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0), MTLSizeMake(c->r.width, c->r.height, 1), (__bridge id<MTLBuffer>)mb, mo + z * img, row, img);
    int shift = rb_copy_shift(c->tex, t);
    if (shift) {
        uint64_t a = ((__bridge id<MTLBuffer>)mb).gpuAddress + mo; size_t wb = (size_t)((c->r.width + bw - 1) / bw) * bpb;
        if (!rb_swap_gpu(a, row, img, a, row, img, wb, rows, nz, shift)) RED_ONCE(6, "RGB565-family texture→buffer copy left in Metal's order: red and blue exchanged");
    }
}
static void read_texels_storage(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    // A sparse pool's texture is read through a copy gathered from its page table (unmapped pages as 0): reading unmapped storage directly faults with SIGBUS.
    const uint8_t *mem = t->mem; uint8_t *part = NULL;
    if (t->pool && nvn_sparse_pool(t->pool)) {
        if (!(part = malloc(t->L.size ? t->L.size : 1))) MDIE("sparse texture copy of %llu bytes failed", (unsigned long long)t->L.size);
        nvn_sparse_copy(t->pool, part, (uint64_t)(uintptr_t)t->mem, t->L.size); mem = part;
    }
    uint32_t lw = MAX(t->d.width >> level, 1u), lh = MAX(t->d.height >> level, 1u), lz = is3d ? MAX(t->d.depth >> level, 1u) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    uint8_t *lin = malloc(lp * lr * lz);
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
        if ((!is3d || z == 0) && nvn_deswizzle(&t->d, level, layer, mem, lin, lp)) { RED_ONCE(17, "ReadTexels deswizzle failed"); break; }
        const uint8_t *src = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, src + (y0 + y) * lp + x0, wb);
    }
    free(lin); free(part);
}
// the caller has waited for the GPU, as NVN requires
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    PD("U cpu_read ser=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d dst=%p row=%lu img=%lu", t->serial, level, layer0, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, r->depth, dst, (unsigned long)row, (unsigned long)img);
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    int nz = r->depth ? r->depth : 1, is3d = t->d.target == 2;
    // A texture whose content comes from guest memory is read there, on any thread: the upload batch belongs to the submit thread, and an unresolved one would read as zeros.
    int nvn_ft_written(const tex_t *);
    // A 2D linear texture whose storage holds its content is read from there: resolving it off the submit thread would make a private texture the CPU's later writes never reach.
    int lin_cpu = (linear_gpu_may_write(t) ? tex_cpu_written(t) || (!tex_get(t->serial) && linear_storage_ok(t)) : linear_storage_ok(t))
                  && t->pool && !nvn_sparse_pool(t->pool) && t->d.target == NVN_TEXTURE_TARGET_2D && t->d.levels <= 1 && t->d.samples <= 1 && bw == 1 && bh == 1;
    if (lin_cpu) {
        size_t srow = tex_linear_row(t);
        if (level || layer0 + r->zoffset || nz != 1 || x0 + wb > srow || (y0 + hb) * srow > t->L.size) {
            RED_ONCE(155, "ReadTexels of a linear texture outside its one level of rows (%ux%u format %#x, stride %zu)", t->d.width, t->d.height, t->d.format, srow);
            memset(dst, 0, img * nz); return;
        }
        mtl_finish_gpu_at(WS_READTEX);   // what the GPU writes into it is stored in its storage by its submission (gsb_store)
        for (size_t y = 0; y < hb; y++) memcpy(dst + y * row, (const uint8_t *)t->mem + (y0 + y) * srow + x0, wb);
        return;
    }
    if (tex_from_storage(t) || (DM_GAME_TEX_DIRTY_UPLOAD && t->mem && nvn_ft_written(t))) { read_texels_storage(t, level, layer0, r, dst, row, img); return; }
    // a guest thread never drains an outer autorelease pool (docs/reference/memory.md, GPU readback without an autorelease pool)
    @autoreleasepool {
    // off the submit thread tex_mtl_ itself says when the content is the storage: the look above can be overtaken
    int in_storage = 0; AP_B;
    id<MTLTexture> mt = tex_mtl_(t, &in_storage); AP_E(4);
    if (in_storage) { read_texels_storage(t, level, layer0, r, dst, row, img); return; }
    if (!mt || !tex_level_ok(mt, level, "CPU readback")) { memset(dst, 0, img * nz); return; }
    mtl_finish_gpu_at(WS_READTEX);
    id<MTLBuffer> buf = [g_dev newBufferWithLength:wb * hb * nz options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = enc_b_begin(cb);
    for (int z = 0; z < nz; z++)
        [b copyFromTexture:mt sourceSlice:is3d ? 0 : layer0 + r->zoffset + z sourceLevel:level sourceOrigin:MTLOriginMake(r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0)
                sourceSize:MTLSizeMake(r->width, r->height, 1) toBuffer:buf destinationOffset:z * wb * hb destinationBytesPerRow:wb destinationBytesPerImage:wb * hb];
    enc_b_end(b); [cb commit]; [cb waitUntilCompleted];
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, (uint8_t *)buf.contents + z * wb * hb + y * wb, wb);
    int shift = rb_copy_shift(t, mt);
    if (shift) rb_swap_rows(dst, wb, hb, (size_t)nz, row, img, shift);
    }
}
// Whether the host takes a texture's content from its storage, now or once the CPU writes it; GPU writes into such a texture are stored there too (gsb_store), as the hardware
// writes them. A sparse pool's texture counts through its page table (gsb_swz); a placement sparse texture does not (its tiles hold what the GPU wrote).
static int tex_storage_dst(const tex_t *t) {
    if (t->mem && t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_VIRTUAL)) {
        if (!nvn_sparse_pool(t->pool) || !tex_from_storage(t) || t->d.samples > 1 || mtl_tex_gpu_owned(t->serial)) return 0;
        __unsafe_unretained id o = tex_get(t->serial);
        return !([o isKindOfClass:[NvPkg class]] && ((NvPkg *)o)->ps);
    }
    if (!t->mem || !t->pool || (t->pool->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) || t->d.samples > 1 || t->d.target == NVN_TEXTURE_TARGET_BUFFER
        || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET | NVN_TEXTURE_FLAGS_COMPRESSIBLE))
        // IMAGE excludes it unless compressed (never shader-written): tex_cpu_content still uploads that from the storage
        || ((t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && !fmt_compressed(t->d.format))
        || ((t->d.flags & NVN_TEXTURE_FLAGS_LINEAR) && !tex_cpu_written(t))) return 0;
    if (t->has_layout || tex_cpu_written(t) || nvn_ft_tracked(t->serial)) return !mtl_tex_gpu_owned(t->serial);
    return [tex_get(t->serial) isKindOfClass:[NvPkg class]];
}
static size_t mtl_pf_block_bytes(MTLPixelFormat pf) {   // per block of a compressed format; 0 when not known
    int bb = bc_block_bytes(pf);
    if (bb) return (size_t)bb;
    if (pf >= MTLPixelFormatASTC_4x4_sRGB && pf <= MTLPixelFormatASTC_12x12_HDR) return 16;
    if (pf >= MTLPixelFormatEAC_R11Unorm && pf <= MTLPixelFormatETC2_RGB8A1_sRGB) return pf == MTLPixelFormatEAC_RG11Unorm || pf == MTLPixelFormatEAC_RG11Snorm || pf == MTLPixelFormatEAC_RGBA8 || pf == MTLPixelFormatEAC_RGBA8_sRGB ? 16 : 8;
    return mtl_pf_bytes(pf);
}
static void copy_b2t(const p_copy_b2t *c, uint32_t size) {
    id<MTLTexture> t = tex_mtl_all(c->tex); sparse_gpu_wrote(c->tex); tu_wrote(t);
    if (!t) { PD("C ser=%llu (no host texture)", c->tex->serial); return; }
    tex_t pair; int paired = bcp_before(c->tex, t, &pair);   // a compressed-alias pair: the copied region goes into its other side after
    int bw, bh, bpb = nvn_fmt_bpb(c->tex->d.format, &bw, &bh);
    unsigned level = c->has_view && c->view.set_levels ? c->view.base_level : 0, layer0 = c->has_view && c->view.set_layers ? c->view.min_layer : 0;
    if (!tex_level_ok(t, level, "buffer-to-texture copy")) return;
    const p_copy_strides *cs = copy_strides(c, size); int64_t rs = cs ? cs->row : S.copy_stride, is = cs ? cs->image : S.copy_img_stride;   // S: OP_COPY_STRIDE of an older capture
    size_t row = rs ? (size_t)rs : (size_t)((c->r.width + bw - 1) / bw) * bpb;
    size_t rows = (c->r.height + bh - 1) / bh, img = is ? (size_t)is : row * rows;
    int is3d = c->tex->d.target == 2, nz = c->r.depth ? c->r.depth : 1;
    PD("C ser=%llu target %d %ux%ux%u format %#x lv%u sl%u region %d,%d,%d %dx%dx%d from %#llx row %zu image %zu", c->tex->serial, c->tex->d.target, c->tex->d.width, c->tex->d.height, c->tex->d.depth,
       c->tex->d.format, level, layer0, c->r.xoffset, c->r.yoffset, c->r.zoffset, c->r.width, c->r.height, c->r.depth, (unsigned long long)c->src, row, img);
    void *mb; uint64_t mo;
    if (!mtl_addr(c->src, img * nz, &mb, &mo)) { RED_ONCE(23, "buffer→texture copy source is in no memory pool"); return; }
    g_cut_copy = 1;
    if (is3d && passdump()) {   // F<frame>_c3d_<serial>.bin
        const char *dd = getenv("DM_NVN_CBDUMP");
        if (dd) {
            char p[1024]; snprintf(p, sizeof p, "%s/F%llu_c3d_%llu.bin", dd, (unsigned long long)atomic_load(&g_cur_frame), (unsigned long long)c->tex->serial);
            FILE *f = fopen(p, "wb");
            if (f) { fwrite((const uint8_t *)((__bridge id<MTLBuffer>)mb).contents + mo, 1, img * nz, f); fclose(f); }
        }
    }
    // into Metal's order on the GPU, via a staging area of the frame arena
    id<MTLBuffer> sb = (__bridge id<MTLBuffer>)mb; int shift = rb_copy_shift(c->tex, t);
    uint64_t gsrc = sb.gpuAddress + mo; size_t grow = row, gimg = img;
    if (shift) {
        size_t wb = (size_t)((c->r.width + bw - 1) / bw) * bpb;
        id<MTLBuffer> ab; size_t ao = arena_alloc(wb * rows * nz, &ab);
        if (rb_swap_gpu(sb.gpuAddress + mo, row, img, ab.gpuAddress + ao, wb, wb * rows, wb, rows, nz, shift)) { sb = ab; mo = ao; row = wb; img = wb * rows; }
        else RED_ONCE(7, "RGB565-family buffer→texture copy left in the guest's order: red and blue exchanged");
    }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (is3d)
        enc_b_copy_buffer_texture(b, sb, mo, row, img, MTLSizeMake(c->r.width, c->r.height, nz), t, 0, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, c->r.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_buffer_texture(b, sb, mo + z * img, row, img, MTLSizeMake(c->r.width, c->r.height, 1), t, layer0 + c->r.zoffset + z, level, MTLOriginMake(c->r.xoffset, c->r.yoffset, 0));
    // the copy writes the texture's storage too, as the hardware's does, so a later upload from the storage keeps it
    gsb_store_buf(c->tex, level, is3d ? (unsigned)c->r.zoffset : layer0 + (unsigned)c->r.zoffset, (unsigned)nz, &c->r, gsrc, grow, gimg);
    if (paired) bcp_after(c->tex, level, &c->r);
}

// Copies a blit cannot do (format change, scaled, MIRROR_X / Y): a render pass per layer drawing the source region, nearest or bilinear; anything else keeps the red line.
// Depth is written from the fragment shader, stencil through [[stencil]] export from an X32_Stencil8 view (else cleared to 0): a later pass may test a halved depth-stencil buffer's stencil.
static int fmt_int_or_ds(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatR8Uint: case MTLPixelFormatR8Sint: case MTLPixelFormatR16Uint: case MTLPixelFormatR16Sint: case MTLPixelFormatRG8Uint: case MTLPixelFormatRG8Sint:
        case MTLPixelFormatR32Uint: case MTLPixelFormatR32Sint: case MTLPixelFormatRG16Uint: case MTLPixelFormatRG16Sint: case MTLPixelFormatRGBA8Uint: case MTLPixelFormatRGBA8Sint:
        case MTLPixelFormatRGB10A2Uint: case MTLPixelFormatRG32Uint: case MTLPixelFormatRG32Sint: case MTLPixelFormatRGBA16Uint: case MTLPixelFormatRGBA16Sint:
        case MTLPixelFormatRGBA32Uint: case MTLPixelFormatRGBA32Sint: case MTLPixelFormatDepth16Unorm: case MTLPixelFormatDepth32Float: case MTLPixelFormatStencil8:
        case MTLPixelFormatDepth32Float_Stencil8: case MTLPixelFormatX32_Stencil8: return 1;
        default: return 0;
    }
}
// Host-only flags above NVN's: COPY_T2T_SQUARE draws c * |c| (an upscaler's square-rooted input); COPY_T2T_BLEND blends in g_copy_blend.t1 by g_copy_blend.mix.
enum { COPY_T2T_SQUARE = 1 << 30, COPY_T2T_BLEND = 1 << 29 };
static struct { float mix; __unsafe_unretained id<MTLTexture> t1; } g_copy_blend;   // set by up_mfx_run around each such copy
static int copy_t2t_draw(id<MTLTexture> s, id<MTLTexture> d, unsigned sl, unsigned ss, unsigned dl, unsigned ds, const NVNcopyRegion *sr, const NVNcopyRegion *dr, int linear, int flags) {
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pso;
    static id<MTLFunction> vf, ff, fq, fup, fd, fds;
    static id<MTLSamplerState> samp[2];
    static id<MTL4ArgumentTable> at;
    static id<MTLDepthStencilState> dss, dss_s;
    static int failed;
    if (failed || sr->width <= 0 || sr->height <= 0 || dr->width <= 0 || dr->height <= 0) return 0;
    int depth = (s.pixelFormat == MTLPixelFormatDepth32Float || s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 || s.pixelFormat == MTLPixelFormatDepth16Unorm) && d.pixelFormat == s.pixelFormat;
    int stencil = depth && d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8;
    if (!depth && (fmt_int_or_ds(s.pixelFormat) || fmt_int_or_ds(d.pixelFormat))) return 0;
    if (mtl_fmt_block(d.pixelFormat) || !(d.usage & MTLTextureUsageRenderTarget) || s.sampleCount > 1 || d.sampleCount > 1) return 0;
    if (!pso) {
        NSError *e = nil; id<MTLLibrary> lib = host_lib("copy", &e);
        vf = [lib newFunctionWithName:@"vs"]; ff = [lib newFunctionWithName:@"fs"]; fq = [lib newFunctionWithName:@"fsq"];
        fup = [lib newFunctionWithName:@"fsup"]; fd = [lib newFunctionWithName:@"fsd"]; fds = [lib newFunctionWithName:@"fsds"];
        MTLDepthStencilDescriptor *dd = [MTLDepthStencilDescriptor new]; dd.depthCompareFunction = MTLCompareFunctionAlways; dd.depthWriteEnabled = YES;
        dss = [g_dev newDepthStencilStateWithDescriptor:dd];
        MTLStencilDescriptor *sd_ = [MTLStencilDescriptor new]; sd_.stencilCompareFunction = MTLCompareFunctionAlways; sd_.depthStencilPassOperation = MTLStencilOperationReplace;
        dd.frontFaceStencil = dd.backFaceStencil = sd_; dss_s = [g_dev newDepthStencilStateWithDescriptor:dd];
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 2; ad.maxSamplerStateBindCount = 1; ad.maxBufferBindCount = 1;
        at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]);
        for (int i = 0; i < 2; i++) {
            MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = i ? MTLSamplerMinMagFilterLinear : MTLSamplerMinMagFilterNearest;
            sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES;
            samp[i] = [g_dev newSamplerStateWithDescriptor:sd];
        }
        if (!vf || !ff || !fq || !fup || !fd || !fds || !dss || !dss_s || !at || !samp[0] || !samp[1]) {
            failed = 1;
            MLOG("[RED] cannot create the pipelines for format-converting / scaling texture copies: %s", e.localizedDescription.UTF8String);
            return 0;
        }
        pso = [NSMutableDictionary dictionary];
    }
    // a registered source may be a view: the copy's views are made on its root, offsets added and the swizzle kept
    id<MTLTexture> root = s; NSUInteger rl = sl, rs = ss;
    while (root.parentTexture) { rl += root.parentRelativeLevel; rs += root.parentRelativeSlice; root = root.parentTexture; }
    id<MTLTexture> stv0 = stencil && s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? [root newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8 textureType:MTLTextureType2D levels:NSMakeRange(rl, 1) slices:NSMakeRange(rs + sr->zoffset, 1)] : nil;
    int scopy = stv0 != nil;
    int square = (flags & COPY_T2T_SQUARE) && !depth, blend = (flags & COPY_T2T_BLEND) && !depth && !square;
    NSNumber *pk = @(((d.pixelFormat * 2 + scopy) * 2 + square) * 2 + blend);
    id<MTLRenderPipelineState> ps = pso[pk];
    if (!ps) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = vf; pd.fragmentFunction = depth ? (scopy ? fds : fd) : square ? fq : blend ? fup : ff;
        if (depth) {
            pd.depthAttachmentPixelFormat = d.pixelFormat;
            if (stencil) pd.stencilAttachmentPixelFormat = d.pixelFormat;
        } else pd.colorAttachments[0].pixelFormat = d.pixelFormat;
        NSError *e = nil; ps = dev_rps(pd, &e);
        if (!ps) { RED_ONCE(83, "format-converting / scaling texture copy: cannot create the pipeline for destination format %lu: %s", (unsigned long)d.pixelFormat, e.localizedDescription.UTF8String); return 0; }
        pso[pk] = ps;
    }
    unsigned sw = MAX(s.width >> sl, 1ul), sh = MAX(s.height >> sl, 1ul), dw = MAX(d.width >> dl, 1ul), dh = MAX(d.height >> dl, 1ul);
    if ((unsigned)(dr->xoffset + dr->width) > dw || (unsigned)(dr->yoffset + dr->height) > dh) return 0;
    int nz = sr->depth ? sr->depth : 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    for (int z = 0; z < nz; z++) {
        id<MTLTexture> sv = [root newTextureViewWithPixelFormat:s.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(rl, 1) slices:NSMakeRange(rs + sr->zoffset + z, 1) swizzle:s.swizzle];
        id<MTLBuffer> pb; size_t po = arena_alloc(48, &pb);   // in the frame arena (resident already): a buffer per copy would be a residency add, a commit and a release
        if (!sv || !pb) { RED_ONCE(84, "format-converting / scaling texture copy: cannot create the source view / argument block"); return z > 0; }
        float *p = (float *)((uint8_t *)pb.contents + po); p[0] = sr->xoffset; p[1] = sr->yoffset; p[2] = (float)sr->width / dr->width; p[3] = (float)sr->height / dr->height;
        if (flags & NVN_COPY_FLAGS_MIRROR_X_BIT) { p[0] += sr->width; p[2] = -p[2]; }
        if (flags & NVN_COPY_FLAGS_MIRROR_Y_BIT) { p[1] += sr->height; p[3] = -p[3]; }
        p[4] = dr->xoffset; p[5] = dr->yoffset; p[6] = sw; p[7] = sh;
        id<MTLTexture> t1 = blend ? g_copy_blend.t1 : nil;
        p[8] = t1 ? g_copy_blend.mix : 0; p[9] = 0; p[10] = 1.0f / sr->width; p[11] = 1.0f / sr->height;
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLRenderPassAttachmentDescriptor *a = depth ? (MTLRenderPassAttachmentDescriptor *)rp.depthAttachment : (MTLRenderPassAttachmentDescriptor *)rp.colorAttachments[0];
        int whole = !dr->xoffset && !dr->yoffset && (unsigned)dr->width == dw && (unsigned)dr->height == dh;   // the copy draws every pixel of the level: its old content is not loaded
        a.texture = d; a.level = dl; a.slice = ds + dr->zoffset + z; a.loadAction = whole ? MTLLoadActionDontCare : MTLLoadActionLoad; a.storeAction = MTLStoreActionStore;
        id<MTLTexture> stv = scopy ? (z ? [root newTextureViewWithPixelFormat:MTLPixelFormatX32_Stencil8 textureType:MTLTextureType2D levels:NSMakeRange(rl, 1) slices:NSMakeRange(rs + sr->zoffset + z, 1)] : stv0) : nil;
        if (stencil) {
            MTLRenderPassStencilAttachmentDescriptor *st = rp.stencilAttachment;
            st.texture = d; st.level = dl; st.slice = a.slice;
            st.loadAction = scopy ? (whole ? MTLLoadActionDontCare : MTLLoadActionLoad) : MTLLoadActionClear; st.clearStencil = 0; st.storeAction = MTLStoreActionStore;
        }
        id<MTL4RenderCommandEncoder> en = enc_r_begin(rp);
        if (depth) [en setDepthStencilState:scopy ? dss_s : dss];
        [en setArgumentTable:at atStages:MTLRenderStageFragment];
        [at setTexture:sv.gpuResourceID atIndex:0];
        if (stv) [at setTexture:stv.gpuResourceID atIndex:1];
        if (blend) [at setTexture:(t1 ?: sv).gpuResourceID atIndex:1];
        [at setSamplerState:samp[(!!linear || blend) && !depth].gpuResourceID atIndex:0];
        at_addr(at, pb.gpuAddress + po, 0, "texture copy draw table slot", 0);
        [en setViewport:(MTLViewport){dr->xoffset, dr->yoffset, dr->width, dr->height, 0, 1}];
        [en setScissorRect:(MTLScissorRect){(NSUInteger)dr->xoffset, (NSUInteger)dr->yoffset, (NSUInteger)dr->width, (NSUInteger)dr->height}];
        [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_r_end(en);
        defer_obj_unref(sv); if (stv) defer_obj_unref(stv);   // released after this submission has run
    }
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
    { static int once[2]; if (!once[depth]++) MLOG("format-converting / scaling texture copy: first case %dx%d format %lu → %dx%d format %lu%s%s", sr->width, sr->height, (unsigned long)s.pixelFormat, dr->width, dr->height, (unsigned long)d.pixelFormat,
                                                    linear && !depth ? " (bilinear)" : "", stencil ? (scopy ? " (depth + stencil)" : " (no stencil view possible, cleared to 0)") : ""); }
    return 1;
}
// nvnCommandBufferDrawTexture: destination in window coordinates, which are the targets' own rows in either origin (the host keeps the Switch's memory layout).
// Its own pass per target with the scissor and channel mask; float or unorm targets only, no blending (red when it is on).
static void draw_tex(const p_draw_tex *c) {
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *pso;
    static id<MTLFunction> vf, ff;
    static id<MTL4ArgumentTable> at;
    static int failed;
    if (failed || g_rc_skip || !R.has_rt) return;
    float dw = c->d.x1 - c->d.x0, dh = c->d.y1 - c->d.y0;
    if (dw == 0 || dh == 0) return;
    if (!pso) {
        NSError *e = nil; id<MTLLibrary> lib = host_lib("draw_tex", &e);
        vf = [lib newFunctionWithName:@"vs"]; ff = [lib newFunctionWithName:@"fs"];
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new]; ad.maxTextureBindCount = 1; ad.maxSamplerStateBindCount = 1; ad.maxBufferBindCount = 1;
        at = rec_table([g_dev newArgumentTableWithDescriptor:ad error:&e]);
        if (!vf || !ff || !at) { failed = 1; MLOG("[RED] cannot create the pipeline for nvnCommandBufferDrawTexture: %s", e.localizedDescription.UTF8String); return; }
        pso = [NSMutableDictionary dictionary];
    }
    os_unfair_lock_lock(&g_rmu);
    id<MTLTexture> t = tex_by_id((int)(bound_handle(c->h) & 0xfffff));
    id<MTLSamplerState> ss = samp_by_id((int)(bound_handle(c->h) >> 20));
    os_unfair_lock_unlock(&g_rmu);
    if (!t || !ss) { RED_ONCE(122, "DrawTexture: texture handle %#llx has no texture or sampler", (unsigned long long)c->h); return; }
    if (t.textureType != MTLTextureType2D || fmt_int_or_ds(t.pixelFormat)) { RED_ONCE(123, "DrawTexture: source type %lu format %lu not drawn (2D float / unorm only)", (unsigned long)t.textureType, (unsigned long)t.pixelFormat); return; }
    if (S.color.blend_en) RED_ONCE(124, "DrawTexture with blending on (targets %#x): drawn without it", (unsigned)S.color.blend_en);
    rp_flush(); rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }   // pending clears of the targets land first
    for (int k = 0; k < R.rt.n && k < 8; k++) {
        const tex_t *rt = R.rt.color[k];
        if (!rt) continue;
        uint32_t wm = (S.chmask.mask >> (4 * k)) & 0xf;
        if (!wm) continue;
        id<MTLTexture> d = tex_mtl_replay(rt); uint64_t ser; int lv, sl; rt_sub(&R.rt, k, &ser, &lv, &sl);
        if (!d || fmt_int_or_ds(d.pixelFormat) || mtl_fmt_block(d.pixelFormat) || !(d.usage & MTLTextureUsageRenderTarget)) { RED_ONCE(125, "DrawTexture: target %d (format %lu) not drawn", k, d ? (unsigned long)d.pixelFormat : 0ul); continue; }
        // a GPU write as a pass's would be: noted for the sparse and use bookkeeping, and a compressed-alias pair's other side follows
        sparse_gpu_wrote(rt); tu_wrote(d);
        tex_t pair; int paired = bcp_before(rt, d, &pair);
        NSNumber *pk = @(d.pixelFormat * 16 + wm);
        id<MTLRenderPipelineState> ps = pso[pk];
        if (!ps) {
            MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
            pd.vertexFunction = vf; pd.fragmentFunction = ff; pd.colorAttachments[0].pixelFormat = d.pixelFormat;
            pd.colorAttachments[0].writeMask = (wm & 1 ? MTLColorWriteMaskRed : 0) | (wm & 2 ? MTLColorWriteMaskGreen : 0) | (wm & 4 ? MTLColorWriteMaskBlue : 0) | (wm & 8 ? MTLColorWriteMaskAlpha : 0);
            NSError *e = nil; ps = dev_rps(pd, &e);
            if (!ps) { RED_ONCE(126, "DrawTexture: cannot create the pipeline for target format %lu: %s", (unsigned long)d.pixelFormat, e.localizedDescription.UTF8String); continue; }
            pso[pk] = ps;
        }
        NSUInteger tw = MAX(d.width >> lv, 1ul), th = MAX(d.height >> lv, 1ul);
        double x0 = MIN(c->d.x0, c->d.x1), x1 = MAX(c->d.x0, c->d.x1), y0 = MIN(c->d.y0, c->d.y1), y1 = MAX(c->d.y0, c->d.y1);
        if (R.has_sc) {
            x0 = MAX(x0, (double)R.sc.x); y0 = MAX(y0, (double)R.sc.y);
            x1 = MIN(x1, (double)R.sc.x + R.sc.w); y1 = MIN(y1, (double)R.sc.y + R.sc.h);
        }
        x0 = MAX(x0, 0.0); y0 = MAX(y0, 0.0); x1 = MIN(x1, (double)tw); y1 = MIN(y1, (double)th);
        NSUInteger sx = (NSUInteger)ceil(x0 - 0.5), sy = (NSUInteger)ceil(y0 - 0.5), ex = (NSUInteger)ceil(x1 - 0.5), ey = (NSUInteger)ceil(y1 - 0.5);   // pixels whose centres are inside
        if (ex <= sx || ey <= sy) continue;
        id<MTLBuffer> pb; size_t po = arena_alloc(32, &pb);
        if (!pb) { RED_ONCE(127, "DrawTexture: no argument block"); return; }
        float *q = (float *)((uint8_t *)pb.contents + po);
        q[0] = c->d.x0; q[1] = c->d.y0; q[2] = (c->s.x1 - c->s.x0) / dw; q[3] = (c->s.y1 - c->s.y0) / dh;
        q[4] = c->s.x0; q[5] = c->s.y0; q[6] = (float)t.width; q[7] = (float)t.height;
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLRenderPassColorAttachmentDescriptor *a = rp.colorAttachments[0];
        a.texture = d; a.level = lv; a.slice = sl; a.loadAction = MTLLoadActionLoad; a.storeAction = MTLStoreActionStore;
        if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        id<MTL4RenderCommandEncoder> en = enc_r_begin(rp);
        [en setArgumentTable:at atStages:MTLRenderStageFragment];
        [at setTexture:t.gpuResourceID atIndex:0];
        [at setSamplerState:ss.gpuResourceID atIndex:0];
        at_addr(at, pb.gpuAddress + po, 0, "draw texture table slot", 0);
        [en setViewport:(MTLViewport){0, 0, tw, th, 0, 1}];
        [en setScissorRect:(MTLScissorRect){sx, sy, ex - sx, ey - sy}];
        [en setRenderPipelineState:ps]; [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; enc_r_end(en);
        NVNcopyRegion rr = {(int)sx, (int)sy, 0, (int)(ex - sx), (int)(ey - sy), 1};
        gsb_store(rt, d, (unsigned)lv, (unsigned)sl, 1, &rr);
        if (paired) bcp_after_to(rt, &pair, (unsigned)lv, &rr);
        PD("T %llu draw texture id %d → target %d ser=%llu region %g,%g..%g,%g from %g,%g..%g,%g", g_stat_pass, (int)(c->h & 0xfffff), k, (unsigned long long)rt->serial, c->d.x0, c->d.y0, c->d.x1, c->d.y1, c->s.x0, c->s.y0, c->s.x1, c->s.y1);
        { static int once; if (!once++) MLOG("DrawTexture: first case %gx%g texels of a %lux%lu texture (format %lu) → %gx%g pixels of target %d (%lux%lu, format %lu)", c->s.x1 - c->s.x0, c->s.y1 - c->s.y0,
                                            (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, dw, dh, k, (unsigned long)tw, (unsigned long)th, (unsigned long)d.pixelFormat); }
    }
    args_dirty_all(); g_pso_dirty = g_dss_dirty = 1;
}
// Compressed-alias pairs (DM_NVN_BC_ALIAS): on the Switch a 2D texture of 8- or 16-byte texels at a BC texture's storage, sized in blocks, is that texture's blocks. Metal can neither
// view nor place the two over the same bytes, so the host keeps the pair equal by GPU copies through a staging buffer after every GPU write of a side, and fills a side from the
// other before a partial write (bcp_synced). An uncompressed side without a texture gets none: copies into it go to the compressed one (docs/reference/rendering.md).
static int bcp_on(void) {
    static int on = -1;
    if (on < 0) {
        const char *e = getenv("DM_NVN_BC_ALIAS");
        on = !(e && !strcmp(e, "0"));
        if (!on) MLOG("DM_NVN_BC_ALIAS=0: textures aliasing compressed ones are written alone (diagnostic)");
    }
    return on;
}
// bytes a texel or a block (8 or 16), negated for a BC format; 0 for none
static int bcp_kind(const tex_t *t) {
    if (!t->mem || t->d.target != NVN_TEXTURE_TARGET_2D || t->d.samples > 1 || (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) return 0;
    int bw = 0, bh = 0, b = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (b != 8 && b != 16) return 0;
    if (bw == 4 && bh == 4 && nvn_fmt_bc(t->d.format)) return -b;
    return bw == 1 && bh == 1 ? b : 0;
}
static int bcp_match(const tex_t *a, int ka, const tex_t *b, int kb) {
    if (!ka || ka + kb || a->mem != b->mem || a->serial == b->serial) return 0;
    const tex_t *bc = ka < 0 ? a : b, *u = ka < 0 ? b : a;
    return (bc->d.width + 3) / 4 == u->d.width && (bc->d.height + 3) / 4 == u->d.height;
}
// Storage address → the registrations there that can be a side. Under its own leaf lock, so a lookup needs no g_rmu (tex_mtl_ often runs under it).
typedef struct { int id; tex_t t; } bcp_ent;
typedef struct { uint32_t n, cap; bcp_ent e[]; } bcp_cell;
static nvn_hmap g_bcp_idx = {.klen = sizeof(void *)};
static os_unfair_lock g_bcp_lk = OS_UNFAIR_LOCK_INIT;
static void bcp_idx(int id_, const tex_t *t, int add) {   // caller holds g_rmu, and takes g_bcp_lk here
    if (!bcp_kind(t)) return;
    const void *k = t->mem;
    os_unfair_lock_lock(&g_bcp_lk);
    bcp_cell *c = nvn_hm_get(&g_bcp_idx, &k);
    uint32_t i = 0; if (c) while (i < c->n && c->e[i].id != id_) i++;
    if (add) {
        if (!c || i == c->n) {
            if (!c || c->n == c->cap) {
                uint32_t cap = c ? c->cap * 2 : 2;
                bcp_cell *nc = dm_realloc(c, sizeof *nc + cap * sizeof nc->e[0], "the compressed-alias index");
                if (!c) nc->n = 0;
                nc->cap = cap; c = nc; nvn_hm_put(&g_bcp_idx, &k, c);
            }
            i = c->n++;
        }
        c->e[i] = (bcp_ent){id_, *t};
    } else if (c && i < c->n) {
        c->e[i] = c->e[--c->n];
        if (!c->n) { nvn_hm_del(&g_bcp_idx, &k); free(c); }
    }
    os_unfair_lock_unlock(&g_bcp_lk);
}
// takes g_bcp_lk only, so any thread may ask, holding g_rmu or not
static int bcp_find(const tex_t *t, tex_t *o) {
    int k = bcp_kind(t);
    if (!k || !g_bcp_idx.n || !bcp_on()) return 0;
    int got = 0; const void *key = t->mem;
    os_unfair_lock_lock(&g_bcp_lk);
    const bcp_cell *c = nvn_hm_get(&g_bcp_idx, &key);
    for (uint32_t i = 0; c && i < c->n && !got; i++) if (bcp_match(t, k, &c->e[i].t, bcp_kind(&c->e[i].t))) { *o = c->e[i].t; got = 1; }
    os_unfair_lock_unlock(&g_bcp_lk);
    return got;
}
// made by nothing
static id<MTLTexture> bcp_have(const tex_t *t) {
    id o = tex_made(t);
    if (!o && t->pool) { alias_key k = tex_alias_key(t); o = own_alias_peek(&k); }
    if (!o && t->pool) { NvPkg *pk = pkg_shared(t); if (pk) o = pk->tex; }
    if (![o conformsToProtocol:@protocol(MTLTexture)]) return nil;
    id<MTLTexture> m = o; while (m.parentTexture) m = m.parentTexture;
    return m;
}
// each holds the other's address, compared and never followed: a texture made again is another texture
static char g_bcp_key;
static int bcp_synced(id<MTLTexture> a, id<MTLTexture> b) {
    NSValue *v = objc_getAssociatedObject(a, &g_bcp_key);
    return v && v.pointerValue == (__bridge void *)b;
}
static void bcp_mark(id<MTLTexture> a, id<MTLTexture> b) {
    objc_setAssociatedObject(a, &g_bcp_key, [NSValue valueWithPointer:(__bridge void *)b], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    objc_setAssociatedObject(b, &g_bcp_key, [NSValue valueWithPointer:(__bridge void *)a], OBJC_ASSOCIATION_RETAIN_NONATOMIC);
}
static uint64_t g_bc_alias_n, g_bcp_copies, g_bcp_fills, g_bcp_ups;
// One side compressed (4x4 pixels a block), the other one texel a block, clipped to both levels. 0 when they cannot be copied; g->img 0 when the region lies outside a level.
enum { BCP_WHOLE = 1 << 30 };   // "to the level's edge"
typedef struct { MTLOrigin fo, too; MTLSize fs, ts; NSUInteger row, img; } bcp_geo;
static int bcp_geom(id<MTLTexture> from, unsigned fl, NSUInteger fx, NSUInteger fy, NSUInteger nx, NSUInteger ny, id<MTLTexture> to, unsigned tl, NSUInteger tx, NSUInteger ty, bcp_geo *g) {
    int fc = mtl_fmt_block(from.pixelFormat), tc = mtl_fmt_block(to.pixelFormat);
    size_t tb = fc ? (size_t)bc_block_bytes(from.pixelFormat) : mtl_pf_bytes(from.pixelFormat);
    if (fc == tc || (tb != 8 && tb != 16) || tb != (tc ? (size_t)bc_block_bytes(to.pixelFormat) : mtl_pf_bytes(to.pixelFormat)) || mtl_fmt_depth(fc ? to.pixelFormat : from.pixelFormat)
        || fl >= from.mipmapLevelCount || tl >= to.mipmapLevelCount) return 0;
    *g = (bcp_geo){0};
    NSUInteger fw = MAX(from.width >> fl, (NSUInteger)1), fh = MAX(from.height >> fl, (NSUInteger)1), tw = MAX(to.width >> tl, (NSUInteger)1), th = MAX(to.height >> tl, (NSUInteger)1);
    NSUInteger fbw = fc ? (fw + 3) / 4 : fw, fbh = fc ? (fh + 3) / 4 : fh, tbw = tc ? (tw + 3) / 4 : tw, tbh = tc ? (th + 3) / 4 : th;
    if (fx >= fbw || fy >= fbh || tx >= tbw || ty >= tbh) return 1;
    nx = MIN(nx, MIN(fbw - fx, tbw - tx)); ny = MIN(ny, MIN(fbh - fy, tbh - ty));
    if (!nx || !ny) return 1;
    g->row = nx * tb; g->img = g->row * ny;
    g->fo = fc ? MTLOriginMake(fx * 4, fy * 4, 0) : MTLOriginMake(fx, fy, 0); g->too = tc ? MTLOriginMake(tx * 4, ty * 4, 0) : MTLOriginMake(tx, ty, 0);
    g->fs = fc ? MTLSizeMake(MIN(nx * 4, fw - fx * 4), MIN(ny * 4, fh - fy * 4), 1) : MTLSizeMake(nx, ny, 1);   // a compressed region may end in a partial block only at the level's edge
    g->ts = tc ? MTLSizeMake(MIN(nx * 4, tw - tx * 4), MIN(ny * 4, th - ty * 4), 1) : MTLSizeMake(nx, ny, 1);
    return 1;
}
// In the copy encoder in replay order, through a reused private staging buffer, with the option-less forms: deferred encoding records only those (RO_C_COPY_TB / RO_C_COPY_BT).
static int bcp_blocks(id<MTLTexture> from, unsigned fl, unsigned fz, NSUInteger fx, NSUInteger fy, NSUInteger nx, NSUInteger ny, id<MTLTexture> to, unsigned tl, unsigned tz, NSUInteger tx, NSUInteger ty) {
    bcp_geo g;
    if (!bcp_geom(from, fl, fx, fy, nx, ny, to, tl, tx, ty, &g)) return 0;
    if (!g.img) return 1;
    static id<MTLBuffer> st;   // replaced (deferred) by a larger one
    if (!st || st.length < g.img) {
        if (st) defer_obj_release(st);
        st = [g_dev newBufferWithLength:g.img options:MTLResourceStorageModePrivate];
        if (!st) { RED_ONCE(156, "compressed alias copy: no %#lx-byte staging buffer", (unsigned long)g.img); return 0; }
        res_add(st);
    }
    enc_b_copy_texture_buffer(blit(), from, fz, fl, g.fo, g.fs, st, 0, g.row, g.img);
    enc_b_copy_buffer_texture(blit(), st, 0, g.row, g.img, g.ts, to, tz, tl, g.too);
    return 1;
}
// In the upload batch, which runs before the replay's next segment: for a side made where no copy may be encoded in replay order. Submit thread only.
static int bcp_up_blocks(id<MTLTexture> from, id<MTLTexture> to) {
    if (!atomic_load(&g_sub_thread_set) || !on_sub_thread()) return 0;   // the upload batch belongs to the submit thread
    bcp_geo g;
    if (!bcp_geom(from, 0, 0, 0, BCP_WHOLE, BCP_WHOLE, to, 0, 0, 0, &g) || !g.img) return 0;
    up_begin(); size_t off = 0; id<MTLBuffer> sb = up_alloc(g.img, &off); up_begin();
    up_t2b_add(from, 0, 0, g.fo, g.fs, sb, off, g.row, g.img);
    up_blit_add_at(sb, off, g.row, g.img, g.ts, to, 0, 0, g.too); g_up_blits[g_nup_blits - 1].bar = 1;
    [g_up_texs addObject:to];
    if (!g_cb) up_flush();
    g_bcp_ups++;
    return 1;
}
// 1 when t has a pair (*o its other side)
static int bcp_before(const tex_t *t, id<MTLTexture> mt, tex_t *o) {
    if (!mt || !bcp_find(t, o)) return 0;
    id<MTLTexture> m = mt; while (m.parentTexture) m = m.parentTexture;
    id<MTLTexture> om = bcp_have(o);
    if (om && om != m && !bcp_synced(m, om) && bcp_blocks(om, 0, 0, 0, 0, BCP_WHOLE, BCP_WHOLE, m, 0, 0, 0, 0)) {
        bcp_mark(m, om);
        if (!g_bcp_fills++) MLOG("compressed alias: ser=%llu (%ux%u format %#x) filled from ser=%llu (%ux%u format %#x) before its first write", (unsigned long long)t->serial, t->d.width, t->d.height,
                                 t->d.format, (unsigned long long)o->serial, o->d.width, o->d.height, o->d.format);
        PD("t compressed alias fill: ser=%llu from ser=%llu", (unsigned long long)t->serial, (unsigned long long)o->serial);
    }
    return 1;
}
// r: the written region in t's own texels (blocks of an uncompressed side, pixels of a compressed one); NULL for the whole level
static void bcp_after_to(const tex_t *t, const tex_t *op, unsigned level, const NVNcopyRegion *r) {
    tex_t o = *op;
    id<MTLTexture> m = bcp_have(t);
    if (!m) return;
    int to_bc = bcp_kind(&o) < 0;
    id<MTLTexture> om = to_bc ? tex_mtl_replay(&o) : bcp_have(&o);   // the compressed side is made (shaders sample it); an uncompressed one follows only a texture it has
    while (om.parentTexture) om = om.parentTexture;
    if (!om || om == m) return;
    NSUInteger bx = 0, by = 0, nx = BCP_WHOLE, ny = BCP_WHOLE;
    if (r) {
        if (r->xoffset < 0 || r->yoffset < 0 || r->width <= 0 || r->height <= 0) return;
        if (to_bc) { bx = (NSUInteger)r->xoffset; by = (NSUInteger)r->yoffset; nx = (NSUInteger)r->width; ny = (NSUInteger)r->height; }
        else { bx = (NSUInteger)r->xoffset / 4; by = (NSUInteger)r->yoffset / 4; nx = ((NSUInteger)r->xoffset + r->width + 3) / 4 - bx; ny = ((NSUInteger)r->yoffset + r->height + 3) / 4 - by; }
    }
    int was = bcp_synced(m, om); bcp_geo g;
    if (!bcp_geom(m, level, bx, by, nx, ny, om, level, bx, by, &g) || !bcp_blocks(m, level, 0, bx, by, nx, ny, om, level, 0, bx, by)) return;
    if (was || (!r && !level)) bcp_mark(m, om);   // a whole level 0 makes them equal; a part keeps them as they were
    // a write through the other side that is not stored itself (an image store) reaches the storage this way
    if (g.img) gsb_store(&o, om, level, 0, 1, &(NVNcopyRegion){(int)g.too.x, (int)g.too.y, 0, (int)g.ts.width, (int)g.ts.height, 1});
    if (!g_bcp_copies++) MLOG("compressed alias: a GPU write of ser=%llu (%ux%u format %#x) goes into ser=%llu (%ux%u format %#x), level %u%s", (unsigned long long)t->serial, t->d.width, t->d.height,
                              t->d.format, (unsigned long long)o.serial, o.d.width, o.d.height, o.d.format, level, r ? "" : ", whole");
    PD("t compressed alias: ser=%llu → ser=%llu level %u blocks %lu,%lu size %lux%lu", (unsigned long long)t->serial, (unsigned long long)o.serial, level, (unsigned long)bx, (unsigned long)by,
       (unsigned long)MIN(nx, (NSUInteger)99999), (unsigned long)MIN(ny, (NSUInteger)99999));
}
static void bcp_after(const tex_t *t, unsigned level, const NVNcopyRegion *r) { tex_t o; if (bcp_find(t, &o)) bcp_after_to(t, &o, level, r); }
// The fill runs in the upload batch, so it misses a write of the other side earlier in the same segment: not marked equal, so the first write fills it again (bcp_before).
static int bcp_up_fill(const tex_t *t, id<MTLTexture> mt) {
    tex_t o;
    if (!mt || !bcp_find(t, &o)) return 0;
    id<MTLTexture> om = bcp_have(&o), m = mt; while (m.parentTexture) m = m.parentTexture;
    if (!om || om == m || !bcp_up_blocks(om, m)) return 0;
    static int nl; if (nl++ < 4) MLOG("compressed alias: ser=%llu (%ux%u format %#x) made after ser=%llu (%ux%u format %#x): filled from it in the upload batch", (unsigned long long)t->serial,
                                    t->d.width, t->d.height, t->d.format, (unsigned long long)o.serial, o.d.width, o.d.height, o.d.format);
    return 1;
}
// on the console the CPU's write is a write of both
static void bcp_up_follow(const tex_t *t, id<MTLTexture> mt) {
    tex_t o;
    if (!mt || !bcp_find(t, &o)) return;
    id<MTLTexture> om = bcp_have(&o), m = mt; while (m.parentTexture) m = m.parentTexture;
    if (!om || om == m || !bcp_up_blocks(m, om)) return;
    static int nl; if (nl++ < 4) MLOG("compressed alias: the CPU's bytes uploaded into ser=%llu (%ux%u format %#x) go into ser=%llu (%ux%u format %#x) in the upload batch", (unsigned long long)t->serial,
                                    t->d.width, t->d.height, t->d.format, (unsigned long long)o.serial, o.d.width, o.d.height, o.d.format);
}
// Image stores and render targets: copied into the other side once the dispatch or pass has ended, once however often it wrote.
enum { BCP_PEND = 16 };
static tex_t g_bcp_pend[BCP_PEND][2];   // found when noted: the flush runs in rp_end_at, maybe with g_rmu held, so it looks nothing up
static void bcp_wrote(const tex_t *t, id<MTLTexture> m) {
    if (!bcp_kind(t) || !g_bcp_idx.n) return;
    tex_t o;
    if (!bcp_before(t, m, &o)) return;
    for (int i = 0; i < g_nbcp; i++) if (g_bcp_pend[i][0].serial == t->serial && g_bcp_pend[i][0].mem == t->mem) return;
    if (g_nbcp == BCP_PEND) { RED_ONCE(30, "compressed alias: more than %d sides written in one pass; ser=%llu is not copied into its other side", BCP_PEND, (unsigned long long)t->serial); return; }
    g_bcp_pend[g_nbcp][0] = *t; g_bcp_pend[g_nbcp++][1] = o;
}
static void bcp_flush(void) {
    int n = g_nbcp; g_nbcp = 0;
    for (int i = 0; i < n; i++) bcp_after_to(&g_bcp_pend[i][0], &g_bcp_pend[i][1], 0, NULL);
}
// Runs before copy_t2t makes the destination's texture; 1 when it took the copy.
static int bc_alias_copy(const p_copy_t2t *c, id<MTLTexture> s, unsigned sl, unsigned sz0, unsigned dl, unsigned dz0) {
    const tex_t *dt = c->dst; tex_t o;
    if (!dt || !c->src || c->src->d.target == 2 || (c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT) || mtl_fmt_block(s.pixelFormat) || dz0) return 0;
    if (c->sr.width != c->dr.width || c->sr.height != c->dr.height || (c->sr.depth ? c->sr.depth : 1) != 1 || c->sr.width <= 0 || c->sr.height <= 0 || c->sr.xoffset < 0 || c->sr.yoffset < 0
        || c->dr.xoffset < 0 || c->dr.yoffset < 0) return 0;
    if (bcp_kind(dt) <= 0 || !bcp_find(dt, &o) || bcp_have(dt)) return 0;
    id<MTLTexture> bc = tex_mtl_replay(&o); while (bc.parentTexture) bc = bc.parentTexture;
    bcp_geo g;
    if (!bc || !bcp_geom(s, sl, (NSUInteger)c->sr.xoffset, (NSUInteger)c->sr.yoffset, (NSUInteger)c->sr.width, (NSUInteger)c->sr.height, bc, dl, (NSUInteger)c->dr.xoffset, (NSUInteger)c->dr.yoffset, &g)
        || !bcp_blocks(s, sl, sz0, (NSUInteger)c->sr.xoffset, (NSUInteger)c->sr.yoffset, (NSUInteger)c->sr.width, (NSUInteger)c->sr.height, bc, dl, 0, (NSUInteger)c->dr.xoffset, (NSUInteger)c->dr.yoffset)) return 0;
    if (g.img) gsb_store(&o, bc, dl, 0, 1, &(NVNcopyRegion){(int)g.too.x, (int)g.too.y, 0, (int)g.ts.width, (int)g.ts.height, 1});
    if (!g_bc_alias_n++) MLOG("texture copy into a texture aliasing a compressed one: %dx%d texels of Metal format %lu (serial %llu) go into blocks %d,%d of compressed serial %llu (format %lu, level %u)",
                              c->sr.width, c->sr.height, (unsigned long)s.pixelFormat, (unsigned long long)dt->serial, c->dr.xoffset, c->dr.yoffset, (unsigned long long)o.serial, (unsigned long)bc.pixelFormat, dl);
    PD("t compressed alias: dst=%llu → ser=%llu at blocks %d,%d size %dx%d", dt->serial, o.serial, c->dr.xoffset, c->dr.yoffset, c->sr.width, c->sr.height);
    return 1;
}
// same size and format, not mirrored: a blit per layer; format change, scale or mirror: drawn; the rest is red
static id<MTLTexture> copy_t2t_enc(const p_copy_t2t *c) {
    rp_flush();   // blit() would end the pass anyway
    id<MTLTexture> s = tex_mtl_all(c->src);
    PD("T src=%llu dst=%llu %dx%dx%d→%dx%dx%d flags=%#x", c->src->serial, c->dst->serial, c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth, c->flags);
    if (!s || !c->dst) return nil;
    unsigned sl = c->has_sv && c->sv.set_levels ? c->sv.base_level : 0, dl = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    unsigned sz0 = c->has_sv && c->sv.set_layers ? c->sv.min_layer : 0, dz0 = c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0;
    if (!tex_level_ok(s, sl, "texture copy source")) return nil;
    if (bc_alias_copy(c, s, sl, sz0, dl, dz0)) return nil;
    id<MTLTexture> d = tex_mtl_all(c->dst); sparse_gpu_wrote(c->dst); tu_wrote(d);
    if (!d || !tex_level_ok(d, dl, "texture copy destination")) return nil;
    { tex_t pair; bcp_before(c->dst, d, &pair); }
    if (c->flags & DM_COPY_RESOLVE) {   // multisample resolve (depth takes sample 0); equal sample counts: a plain whole copy
        if (s.sampleCount == d.sampleCount && s.pixelFormat == d.pixelFormat && s.width == d.width && s.height == d.height) {
            enc_b_copy_texture(blit(), s, sz0, sl, MTLOriginMake(0, 0, 0), MTLSizeMake(s.width, s.height, 1), d, dz0, dl, MTLOriginMake(0, 0, 0));
            return d;
        }
        if (s.sampleCount == 1 && d.sampleCount == 1 && c->src->d.target != 2 && c->dst->d.target != 2) {
            if (!(d.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(d.pixelFormat) && !fmt_int_or_ds(d.pixelFormat)) d = tex_upgrade(c->dst, d, MTLTextureUsageRenderTarget, d.pixelFormat);
            NVNcopyRegion sr = {0, 0, 0, (int)MAX(s.width >> sl, 1u), (int)MAX(s.height >> sl, 1u), 1}, dr = {0, 0, 0, (int)MAX(d.width >> dl, 1u), (int)MAX(d.height >> dl, 1u), 1};
            if (copy_t2t_draw(s, d, sl, sz0, dl, dz0, &sr, &dr, !fmt_int_or_ds(s.pixelFormat), 0)) return d;
        }
        if (s.sampleCount <= 1 || d.sampleCount != 1 || s.pixelFormat != d.pixelFormat || s.width != d.width || s.height != d.height) {
            RED_ONCE(72, "Downsample case not implemented: %lux%lu %lu samples format %lu → %lux%lu %lu samples format %lu", (unsigned long)s.width, (unsigned long)s.height, (unsigned long)s.sampleCount,
                     (unsigned long)s.pixelFormat, (unsigned long)d.width, (unsigned long)d.height, (unsigned long)d.sampleCount, (unsigned long)d.pixelFormat);
            return nil;
        }
        rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
        MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
        MTLPixelFormat pf = s.pixelFormat;
        int depth = pf == MTLPixelFormatDepth32Float || pf == MTLPixelFormatDepth16Unorm || pf == MTLPixelFormatDepth32Float_Stencil8;
        MTLRenderPassAttachmentDescriptor *a = depth ? rp.depthAttachment : rp.colorAttachments[0];
        a.texture = s; a.level = sl; a.slice = sz0; a.resolveTexture = d; a.resolveLevel = dl; a.resolveSlice = dz0;
        a.loadAction = MTLLoadActionLoad; a.storeAction = MTLStoreActionStoreAndMultisampleResolve;
        if (depth) rp.depthAttachment.depthResolveFilter = MTLMultisampleDepthResolveFilterSample0;
        enc_r_end(enc_r_begin(rp));
        return d;
    }
    PD("t src=%llu lv%u sl%u xyz=%d,%d,%d dst=%llu lv%u sl%u xyz=%d,%d,%d size=%d,%d,%d", c->src->serial, sl, sz0, c->sr.xoffset, c->sr.yoffset, c->sr.zoffset,
       c->dst->serial, dl, dz0, c->dr.xoffset, c->dr.yoffset, c->dr.zoffset, c->sr.width, c->sr.height, c->sr.depth);
    int s3 = c->src->d.target == 2, d3 = c->dst->d.target == 2, nz = c->sr.depth ? c->sr.depth : 1;
    // a destination made without render-target use gets it first
    if ((c->sr.width != c->dr.width || c->sr.height != c->dr.height || s.pixelFormat != d.pixelFormat || (c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))) && !s3 && !d3
        && !(d.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(d.pixelFormat) && d.sampleCount == 1 && !fmt_int_or_ds(s.pixelFormat) && !fmt_int_or_ds(d.pixelFormat))
        d = tex_upgrade(c->dst, d, MTLTextureUsageRenderTarget, d.pixelFormat);
    if ((c->sr.width != c->dr.width || c->sr.height != c->dr.height || s.pixelFormat != d.pixelFormat || (c->flags & (NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))) && !s3 && !d3
        && (c->sr.depth ? c->sr.depth : 1) == (c->dr.depth ? c->dr.depth : 1)
        && !(c->flags & ~(NVN_COPY_FLAGS_ENGINE_2D_BIT | NVN_COPY_FLAGS_LINEAR_FILTER_BIT | NVN_COPY_FLAGS_MIRROR_X_BIT | NVN_COPY_FLAGS_MIRROR_Y_BIT))
        && copy_t2t_draw(s, d, sl, sz0, dl, dz0, &c->sr, &c->dr, !!(c->flags & NVN_COPY_FLAGS_LINEAR_FILTER_BIT), c->flags))
        return d;
    // a blit copies only between equal formats, so depth goes through a buffer from / to the combined texture's depth plane
    if (!s3 && !d3 && c->sr.width == c->dr.width && c->sr.height == c->dr.height && (c->sr.depth ? c->sr.depth : 1) == 1 && (c->dr.depth ? c->dr.depth : 1) == 1
        && !(c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT & ~NVN_COPY_FLAGS_LINEAR_FILTER_BIT) && s.sampleCount == 1 && d.sampleCount == 1
        && ((s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 && d.pixelFormat == MTLPixelFormatDepth32Float) || (s.pixelFormat == MTLPixelFormatDepth32Float && d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8))) {
        static id<MTLBuffer> plane;   // replaced (deferred) by a larger one
        NSUInteger row = (NSUInteger)c->sr.width * 4, img = row * (NSUInteger)c->sr.height;
        if (!plane || plane.length < img) {
            if (plane) defer_obj_release(plane);
            plane = [g_dev newBufferWithLength:img options:MTLResourceStorageModePrivate];
            if (!plane) { RED_ONCE(132, "depth plane copy: cannot create a %#lx-byte staging buffer", (unsigned long)img); return nil; }
            res_add(plane);
        }
        MTLBlitOption so = s.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
        MTLBlitOption dop = d.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
        enc_b_copy_texture_buffer_opt(blit(), s, sz0, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, 0), MTLSizeMake(c->sr.width, c->sr.height, 1), plane, 0, row, img, so);
        enc_b_copy_buffer_texture_opt(blit(), plane, 0, row, img, MTLSizeMake(c->sr.width, c->sr.height, 1), d, dz0, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, 0), dop);
        static int once; if (!once++) MLOG("depth plane copy: %dx%d Metal format %lu → %lu through a staging buffer", c->sr.width, c->sr.height, (unsigned long)s.pixelFormat, (unsigned long)d.pixelFormat);
        return d;
    }
    if (c->sr.width != c->dr.width || c->sr.height != c->dr.height || (c->sr.depth ? c->sr.depth : 1) != (c->dr.depth ? c->dr.depth : 1) || (c->flags & ~NVN_COPY_FLAGS_ENGINE_2D_BIT & ~NVN_COPY_FLAGS_LINEAR_FILTER_BIT)
        || s.pixelFormat != d.pixelFormat || s3 != d3) {
        RED_ONCE(33, "texture→texture copy case not implemented: %dx%dx%d→%dx%dx%d flags %#x format %lu→%lu 3D %d→%d", c->sr.width, c->sr.height, c->sr.depth, c->dr.width, c->dr.height, c->dr.depth,
                 c->flags, (unsigned long)s.pixelFormat, (unsigned long)d.pixelFormat, s3, d3);
        return nil;
    }
    id<MTL4ComputeCommandEncoder> b = blit();
    if (s3)
        enc_b_copy_texture(b, s, 0, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, c->sr.zoffset), MTLSizeMake(c->sr.width, c->sr.height, nz), d, 0, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, c->dr.zoffset));
    else
        for (int z = 0; z < nz; z++)
            enc_b_copy_texture(b, s, sz0 + c->sr.zoffset + z, sl, MTLOriginMake(c->sr.xoffset, c->sr.yoffset, 0), MTLSizeMake(c->sr.width, c->sr.height, 1), d, dz0 + c->dr.zoffset + z, dl, MTLOriginMake(c->dr.xoffset, c->dr.yoffset, 0));
    // DM_NVN_T2T_TRACE=WxH:file (diagnostic, from frame DM_NVN_T2T_FROM): RGBA16F copies of that size traced on the GPU timeline; written after 240
    {
        static int on = -1;
        static uint32_t tw, th;
        static const char *tf;
        static id<MTLBuffer> tb;
        static int tn;
        static uint64_t tfr[240], from;
        if (on < 0) {
            const char *e = getenv("DM_NVN_T2T_TRACE"), *fe = getenv("DM_NVN_T2T_FROM");
            on = e && sscanf(e, "%ux%u", &tw, &th) == 2 && (tf = strchr(e, ':')) && *++tf;
            from = fe ? strtoull(fe, NULL, 10) : 0;
        }
        if (on > 0 && tn < 240 && atomic_load(&g_cur_frame) >= from && d.width == tw && d.height == th && d.pixelFormat == MTLPixelFormatRGBA16Float && !s3) {
            if (!tb) { tb = [g_dev newBufferWithLength:(size_t)240 * tw * th * 8 options:MTLResourceStorageModeShared]; res_add(tb); }
            enc_b_copy_texture_buffer(blit(), d, dz0, dl, MTLOriginMake(0, 0, 0), MTLSizeMake(tw, th, 1), tb, (size_t)tn * tw * th * 8, (size_t)tw * 8, (size_t)tw * th * 8);
            tfr[tn++] = atomic_load(&g_cur_frame);
            if (tn == 240) {
                rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
                mtl_finish_gpu();
                FILE *f = fopen(tf, "wb");
                if (f) {
                    fprintf(f, "%u %u %d\n", tw, th, tn);
                    fwrite(tfr, 8, 240, f); fwrite(tb.contents, 1, (size_t)240 * tw * th * 8, f);
                    fclose(f);
                }
                MLOG("[t2t-trace] %d copies → %s (frames %llu–%llu)", tn, tf, (unsigned long long)tfr[0], (unsigned long long)tfr[239]);
            }
        }
    }
    return d;
}
// a 3D destination's planes from the region's z, any other's layers from the view's first
static void copy_t2t(const p_copy_t2t *c) {
    id<MTLTexture> d = copy_t2t_enc(c);
    if (!d) return;
    unsigned level = c->has_dv && c->dv.set_levels ? c->dv.base_level : 0;
    bcp_after(c->dst, level, &c->dr);
    gsb_store(c->dst, d, level, (c->dst->d.target == NVN_TEXTURE_TARGET_3D ? 0 : c->has_dv && c->dv.set_layers ? c->dv.min_layer : 0) + (unsigned)c->dr.zoffset,
              (unsigned)(c->dr.depth ? c->dr.depth : 1), &c->dr);
}
