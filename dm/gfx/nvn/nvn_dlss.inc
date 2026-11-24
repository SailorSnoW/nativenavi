// NVN2's DLSS on MetalFX's temporal scaler (docs/reference/rendering.md#nvn2s-dlss-on-metalfx); the flags are NGX's feature flags.
enum { DLSS_F_DEPTH_INVERTED = 1 << 3, DLSS_F_AUTO_EXPOSURE = 1 << 6 };
// A replaced scaler is held until the submission completes: a Metal 4 command buffer does not retain what is encoded into it.
// Scalers are keyed by the textures' sizes, not the input's: a dynamic resolution renders a new size into the same textures each frame.
// MetalFX writes alpha 1 where NVN2's DLSS keeps it: the input's alpha is drawn over the output with an alpha-only write mask.
static id<MTL4ArgumentTable> g_dlss_alpha_at;
static id<MTLSamplerState> g_dlss_alpha_samp;
static id<MTLRenderPipelineState> dlss_alpha_pso(MTLPixelFormat pf) {
    static NSMutableDictionary<NSNumber *, id<MTLRenderPipelineState>> *cache;
    static id<MTLFunction> vf, ff;
    static int failed;
    if (failed) return nil;
    if (!cache) {
        NSError *e = nil;
        id<MTLLibrary> lib = host_lib("copy", &e);
        vf = [lib newFunctionWithName:@"vs"]; ff = [lib newFunctionWithName:@"fsa"];
        MTL4ArgumentTableDescriptor *ad = [MTL4ArgumentTableDescriptor new];
        ad.maxTextureBindCount = 1; ad.maxSamplerStateBindCount = 1; ad.maxBufferBindCount = 1;
        g_dlss_alpha_at = [g_dev newArgumentTableWithDescriptor:ad error:&e];
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear;
        sd.sAddressMode = sd.tAddressMode = MTLSamplerAddressModeClampToEdge; sd.supportArgumentBuffers = YES;
        g_dlss_alpha_samp = [g_dev newSamplerStateWithDescriptor:sd];
        if (!vf || !ff || !g_dlss_alpha_at || !g_dlss_alpha_samp) {
            failed = 1;
            MLOG("[RED] DLSS: cannot create the alpha-preserving pass: %s; scaling bilinearly", e.localizedDescription.UTF8String);
            return nil;
        }
        cache = [NSMutableDictionary dictionary];
    }
    NSNumber *key = @(pf); id<MTLRenderPipelineState> ps = cache[key];
    if (!ps) {
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new]; pd.vertexFunction = vf; pd.fragmentFunction = ff;
        pd.colorAttachments[0].pixelFormat = pf; pd.colorAttachments[0].writeMask = MTLColorWriteMaskAlpha;
        NSError *e = nil; ps = dev_rps(pd, &e);
        if (!ps) {
            MLOG("[RED] DLSS: cannot create the alpha-only pipeline for output format %lu: %s; scaling bilinearly", (unsigned long)pf, e.localizedDescription.UTF8String);
            return nil;
        }
        cache[key] = ps;
    }
    return ps;
}

typedef struct { uint64_t net, used; id<MTL4FXTemporalScaler> sc; NSUInteger key[10]; float lo, hi; int fresh, mask; } dlss_scaler;
static dlss_scaler g_dlss[4];
static uint64_t g_dlss_use;
static id<MTL4Compiler> g_dlss_comp;
static void dlss_scaler_drop(dlss_scaler *e) {
    if (e->sc) defer_obj_release(e->sc);
    e->sc = nil;
}
static dlss_scaler *dlss_scaler_for(const p_dlss *c, id<MTLTexture> col, id<MTLTexture> dep, id<MTLTexture> mot, id<MTLTexture> out, id<MTLTexture> bias, NSUInteger iw, NSUInteger ih) {
    NSUInteger bw = MIN(col.width, MIN(dep.width, mot.width)), bh = MIN(col.height, MIN(dep.height, mot.height));
    if (bias) { bw = MIN(bw, bias.width); bh = MIN(bh, bias.height); }
    NSUInteger key[10] = {col.pixelFormat, dep.pixelFormat, mot.pixelFormat, out.pixelFormat, bias ? bias.pixelFormat : 0, bw, bh, out.width, out.height, c->flags};
    float s_lo = MIN((float)out.width / (float)iw, (float)out.height / (float)ih), s_hi = MAX((float)out.width / (float)iw, (float)out.height / (float)ih);
    dlss_scaler *e = NULL, *spare = NULL;
    for (int i = 0; i < 4 && !e; i++) {
        if (g_dlss[i].net == c->net) e = &g_dlss[i];
        else if (!spare || (spare->net && (!g_dlss[i].net || g_dlss[i].used < spare->used))) spare = &g_dlss[i];
    }
    if (!e) {
        e = spare;
        e->net = c->net;
        dlss_scaler_drop(e);
        memset(e->key, 0, sizeof e->key);
    }
    e->used = ++g_dlss_use;
    if (!memcmp(e->key, key, sizeof key) && (!e->sc || (s_lo >= e->lo && s_hi <= e->hi))) return e->sc ? e : NULL;
    memcpy(e->key, key, sizeof key);
    dlss_scaler_drop(e);
    e->fresh = 1;
    NSError *err = nil;
    if (!g_dlss_comp) g_dlss_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    e->lo = MIN(s_lo, MAX(MIN((float)out.width / (float)bw, (float)out.height / (float)bh), [MTLFXTemporalScalerDescriptor supportedInputContentMinScaleForDevice:g_dev]));
    e->hi = MAX(s_hi, [MTLFXTemporalScalerDescriptor supportedInputContentMaxScaleForDevice:g_dev]);
    MTLFXTemporalScalerDescriptor *d = [MTLFXTemporalScalerDescriptor new];
    d.colorTextureFormat = col.pixelFormat; d.depthTextureFormat = dep.pixelFormat; d.motionTextureFormat = mot.pixelFormat; d.outputTextureFormat = out.pixelFormat;
    d.inputWidth = bw; d.inputHeight = bh; d.outputWidth = out.width; d.outputHeight = out.height;
    d.autoExposureEnabled = (c->flags & DLSS_F_AUTO_EXPOSURE) != 0;
    d.inputContentPropertiesEnabled = YES; d.inputContentMinScale = e->lo; d.inputContentMaxScale = e->hi;
    const int fx = g_dlss_comp && [MTLFXTemporalScalerDescriptor supportsMetal4FX:g_dev];
    e->mask = bias != nil;
    if (bias) { d.reactiveMaskTextureEnabled = YES; d.reactiveMaskTextureFormat = bias.pixelFormat; }
    e->sc = fx ? [d newTemporalScalerWithDevice:g_dev compiler:g_dlss_comp] : nil;
    if (fx && !e->sc && bias) {
        MLOG("[RED] DLSS: MetalFX makes no temporal scaler with a reactive mask of format %lu; the game's bias mask is dropped", (unsigned long)bias.pixelFormat);
        d.reactiveMaskTextureEnabled = NO; e->mask = 0;
        e->sc = [d newTemporalScalerWithDevice:g_dev compiler:g_dlss_comp];
    }
    if (!e->sc) MLOG("[RED] DLSS: MetalFX makes no temporal scaler for colour format %lu, depth %lu, motion %lu, output %lu, %lux%lu (scale %.2f-%.2f) → %lux%lu; scaling bilinearly", (unsigned long)col.pixelFormat,
                     (unsigned long)dep.pixelFormat, (unsigned long)mot.pixelFormat, (unsigned long)out.pixelFormat, (unsigned long)bw, (unsigned long)bh, e->lo, e->hi, (unsigned long)out.width, (unsigned long)out.height);
    else MLOG("DLSS: MetalFX temporal scaler %lux%lu (scale %.2f-%.2f, first content %lux%lu) → %lux%lu, colour format %lu, depth %lu, motion %lu, output %lu, reactive mask %lu", (unsigned long)bw, (unsigned long)bh, e->lo, e->hi,
              (unsigned long)iw, (unsigned long)ih, (unsigned long)out.width, (unsigned long)out.height, (unsigned long)col.pixelFormat, (unsigned long)dep.pixelFormat, (unsigned long)mot.pixelFormat, (unsigned long)out.pixelFormat,
              e->mask ? (unsigned long)bias.pixelFormat : 0ul);
    return e->sc ? e : NULL;
}
// MetalFX's Metal 4 scalers open their own encoders with no barrier: an encoder around a real copy orders them
// (docs/reference/rendering.md#an-encoder-of-barriers-alone-orders-nothing); the next pass must keep its consumer barrier (enc_r_begin_pass).
static void mfx_order(id<MTL4CommandBuffer> cb, id<MTLTexture> src, MTLOrigin o, MTLSize z, id<MTLTexture> dst) {
    static id<MTLBuffer> word;
    static dispatch_once_t once;
    dispatch_once(&once, ^{
        word = [g_dev newBufferWithLength:16 options:MTLResourceStorageModePrivate];
        word.label = @"MetalFX order word";
        res_add(word);
        res_commit();
    });
    id<MTL4ComputeCommandEncoder> ce = [cb computeCommandEncoder];
    [ce barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageBlit visibilityOptions:resource_vis()];
    if (dst) [ce copyFromTexture:src sourceSlice:0 sourceLevel:0 sourceOrigin:o sourceSize:z toTexture:dst destinationSlice:0 destinationLevel:0 destinationOrigin:MTLOriginMake(0, 0, 0)];
    else [ce copyFromBuffer:word sourceOffset:0 toBuffer:word destinationOffset:8 size:4];
    [ce barrierAfterStages:MTLStageBlit beforeQueueStages:MTLStageAll visibilityOptions:resource_vis()];
    [ce endEncoding];
    if (cb == g_cb) g_last_end_relaxed = 0;
}
static void dlss_eval(const p_dlss *c) {
    rp_flush();   // the passes before it write its inputs
    id<MTLTexture> col = c->tex[DLSS_COLOR] ? tex_mtl_replay(c->tex[DLSS_COLOR]) : nil, dep = c->tex[DLSS_DEPTH] ? tex_mtl_replay(c->tex[DLSS_DEPTH]) : nil, mot = c->tex[DLSS_MOTION] ? tex_mtl_replay(c->tex[DLSS_MOTION]) : nil,
                   out = c->tex[DLSS_OUTPUT] ? tex_mtl_replay(c->tex[DLSS_OUTPUT]) : nil, bias = c->tex[DLSS_BIAS] ? tex_mtl_replay(c->tex[DLSS_BIAS]) : nil;
    if (!col || !out) {
        static int once;
        if (!once++) MLOG("[RED] DLSS evaluated without %s", col ? "an output" : "an input colour");
        return;
    }
    NSUInteger iw = c->iw > 0 ? (NSUInteger)c->iw : col.width, ih = c->ih > 0 ? (NSUInteger)c->ih : col.height;
    if (bias && (c->base[DLSS_BIAS][0] < 0 || c->base[DLSS_BIAS][1] < 0 || (NSUInteger)c->base[DLSS_BIAS][0] + iw > bias.width || (NSUInteger)c->base[DLSS_BIAS][1] + ih > bias.height)) {
        static int once;
        if (!once++) MLOG("[RED] DLSS: the bias mask's %lux%lu texture does not hold the %lux%lu input at %d,%d; the mask is not used", (unsigned long)bias.width, (unsigned long)bias.height,
                          (unsigned long)iw, (unsigned long)ih, c->base[DLSS_BIAS][0], c->base[DLSS_BIAS][1]);
        bias = nil;
    }
    rp_end();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    static int bilinear = -1;   // diagnostic
    if (bilinear < 0) {
        const char *be = getenv("DM_NVN_DLSS_BILINEAR");
        bilinear = be && atoi(be);
        if (bilinear) MLOG("DM_NVN_DLSS_BILINEAR: DLSS drawn bilinear (diagnostic)");
    }
    dlss_scaler *e = dep && mot && !bilinear ? dlss_scaler_for(c, col, dep, mot, out, bias, iw, ih) : NULL;
    int keep_alpha = (mtl_pf_chmask(out.pixelFormat) & 8) != 0;
    id<MTLRenderPipelineState> alpha_ps = e && keep_alpha ? dlss_alpha_pso(out.pixelFormat) : nil;
    if (e && keep_alpha && !alpha_ps) e = NULL;   // MetalFX's output loses alpha without the alpha pass
    if (!e) {
        NVNcopyRegion sr = {c->base[DLSS_COLOR][0], c->base[DLSS_COLOR][1], 0, (int)iw, (int)ih, 1}, dr = {c->base[DLSS_OUTPUT][0], c->base[DLSS_OUTPUT][1], 0, (int)out.width, (int)out.height, 1};
        if (!(out.usage & MTLTextureUsageRenderTarget) && !mtl_fmt_block(out.pixelFormat) && !fmt_int_or_ds(out.pixelFormat)) out = tex_upgrade(c->tex[DLSS_OUTPUT], out, MTLTextureUsageRenderTarget, out.pixelFormat);
        if (!copy_t2t_draw(col, out, 0, 0, 0, 0, &sr, &dr, 1, 0)) {
            static int once;
            if (!once++) MLOG("[RED] DLSS: the bilinear stand-in could not draw format %lu into format %lu", (unsigned long)col.pixelFormat, (unsigned long)out.pixelFormat);
        }
        return;
    }
    id<MTL4FXTemporalScaler> sc = e->sc;
    MTLTextureUsage col_use = sc.colorTextureUsage | (keep_alpha ? MTLTextureUsageShaderRead : 0);
    MTLTextureUsage out_use = sc.outputTextureUsage | (keep_alpha ? MTLTextureUsageRenderTarget : 0);
    if ((col.usage & col_use) != col_use) col = tex_upgrade(c->tex[DLSS_COLOR], col, col_use, col.pixelFormat);
    if ((dep.usage & sc.depthTextureUsage) != sc.depthTextureUsage) dep = tex_upgrade(c->tex[DLSS_DEPTH], dep, sc.depthTextureUsage, dep.pixelFormat);
    if ((mot.usage & sc.motionTextureUsage) != sc.motionTextureUsage) mot = tex_upgrade(c->tex[DLSS_MOTION], mot, sc.motionTextureUsage, mot.pixelFormat);
    if ((out.usage & out_use) != out_use) out = tex_upgrade(c->tex[DLSS_OUTPUT], out, out_use, out.pixelFormat);
    if (!e->mask) bias = nil;
    if (bias) {
        MTLTextureUsage u;
        if (@available(macOS 27.0, *)) u = sc.reactiveMaskTextureUsage;
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"   // the only name before macOS 27
        else u = sc.reactiveTextureUsage;
#pragma clang diagnostic pop
        if ((bias.usage & u) != u) bias = tex_upgrade(c->tex[DLSS_BIAS], bias, u, bias.pixelFormat);
    }
    id<MTLTexture> expo = c->tex[DLSS_EXPOSURE] ? tex_mtl_replay(c->tex[DLSS_EXPOSURE]) : nil;
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }   // a usage upgrade copies through it
    ubd_land_compute();
    mfx_order(g_cb, nil, MTLOriginMake(0, 0, 0), MTLSizeMake(0, 0, 0), nil);
    if (@available(macOS 27.0, *)) {
    } else if (c->base[DLSS_COLOR][0] || c->base[DLSS_COLOR][1] || c->base[DLSS_DEPTH][0] || c->base[DLSS_DEPTH][1] || c->base[DLSS_MOTION][0] || c->base[DLSS_MOTION][1] ||
               c->base[DLSS_OUTPUT][0] || c->base[DLSS_OUTPUT][1] || (bias && (c->base[DLSS_BIAS][0] || c->base[DLSS_BIAS][1]))) {
        static int once;
        if (!once++) MLOG("[RED] DLSS: rectangles away from the origin need macOS 27's MetalFX; taken from the origin");
    }
    const p_dlss a = *c;
    const int reset = c->reset || e->fresh;
    e->fresh = 0;
    id<MTLBuffer> alpha_pb = nil;
    size_t alpha_po = 0;
    NSUInteger ow = sc.outputWidth, oh = sc.outputHeight;   // the record's ow / oh keep the builder's size after NetworkResize
    if (keep_alpha) {
        int sx = c->base[DLSS_COLOR][0], sy = c->base[DLSS_COLOR][1], dx = c->base[DLSS_OUTPUT][0], dy = c->base[DLSS_OUTPUT][1];
        if (sx < 0 || sy < 0 || dx < 0 || dy < 0 || (NSUInteger)sx + iw > col.width || (NSUInteger)sy + ih > col.height || (NSUInteger)dx + ow > out.width || (NSUInteger)dy + oh > out.height) {
            static int once;
            if (!once++) MLOG("[RED] DLSS: alpha-preserving rectangles are out of bounds; alpha is left as MetalFX wrote it");
            keep_alpha = 0;
        } else {
            alpha_po = arena_alloc(48, &alpha_pb);
            float *p = (float *)((uint8_t *)alpha_pb.contents + alpha_po);
            p[0] = sx; p[1] = sy; p[2] = (float)iw / ow; p[3] = (float)ih / oh;
            p[4] = dx; p[5] = dy; p[6] = col.width; p[7] = col.height;
            p[8] = sx + 0.5f; p[9] = sy + 0.5f; p[10] = sx + iw - 0.5f; p[11] = sy + ih - 0.5f;
        }
    }
    // On the real command buffer: the recording's stand-in has none of MetalFX's calls
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        sc.colorTexture = col; sc.depthTexture = dep; sc.motionTexture = mot; sc.outputTexture = out; sc.exposureTexture = expo; sc.reactiveMaskTexture = bias;
        sc.inputContentWidth = iw; sc.inputContentHeight = ih;
        if (@available(macOS 27.0, *)) {   // older systems take the rectangles from the origin
            sc.colorContentOffsetX = (NSUInteger)a.base[DLSS_COLOR][0]; sc.colorContentOffsetY = (NSUInteger)a.base[DLSS_COLOR][1];
            sc.depthContentOffsetX = (NSUInteger)a.base[DLSS_DEPTH][0]; sc.depthContentOffsetY = (NSUInteger)a.base[DLSS_DEPTH][1];
            sc.motionContentOffsetX = (NSUInteger)a.base[DLSS_MOTION][0]; sc.motionContentOffsetY = (NSUInteger)a.base[DLSS_MOTION][1];
            if (bias) { sc.reactiveMaskContentOffsetX = (NSUInteger)a.base[DLSS_BIAS][0]; sc.reactiveMaskContentOffsetY = (NSUInteger)a.base[DLSS_BIAS][1]; }
            sc.outputOffsetX = (NSUInteger)a.base[DLSS_OUTPUT][0]; sc.outputOffsetY = (NSUInteger)a.base[DLSS_OUTPUT][1];
        }
        sc.motionVectorScaleX = a.mv[0]; sc.motionVectorScaleY = a.mv[1];
        sc.jitterOffsetX = a.jitter[0]; sc.jitterOffsetY = a.jitter[1];
        sc.preExposure = a.pre_exposure;
        sc.depthReversed = (a.flags & DLSS_F_DEPTH_INVERTED) != 0;
        sc.reset = reset;
        [sc encodeToCommandBuffer:cb];
        if (keep_alpha) {
            MTL4RenderPassDescriptor *rp = [MTL4RenderPassDescriptor new];
            rp.colorAttachments[0].texture = out; rp.colorAttachments[0].loadAction = MTLLoadActionLoad; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
            id<MTL4RenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
            [en barrierAfterQueueStages:MTLStageAll beforeStages:MTLStageAll visibilityOptions:resource_vis()];
            [en setArgumentTable:g_dlss_alpha_at atStages:MTLRenderStageFragment];
            [g_dlss_alpha_at setTexture:col.gpuResourceID atIndex:0]; [g_dlss_alpha_at setSamplerState:g_dlss_alpha_samp.gpuResourceID atIndex:0];
            [g_dlss_alpha_at setAddress:alpha_pb.gpuAddress + alpha_po atIndex:0];
            [en setViewport:(MTLViewport){a.base[DLSS_OUTPUT][0], a.base[DLSS_OUTPUT][1], ow, oh, 0, 1}];
            [en setScissorRect:(MTLScissorRect){(NSUInteger)a.base[DLSS_OUTPUT][0], (NSUInteger)a.base[DLSS_OUTPUT][1], ow, oh}];
            [en setRenderPipelineState:alpha_ps];
            [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
            m4_end(en);
        }
    });
}
