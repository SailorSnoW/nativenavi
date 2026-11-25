// The game's own upscaler (agl's NDSR network) or its chain, drawn by MetalFX's spatial scaler with DM_UPSCALER=metalfx (docs/reference/rendering.md, A game's own upscaler drawn by MetalFX).
// Push constants c[24]: input then output rectangle, origin x, y and last texel relative to it (inclusive). What the checks refuse runs the game's shader.
// key: input w, h, output w, h, sRGB input, colour format, output format; read: what the game's image is drawn from (out, or its sRGB view)
// used: the last lookup that found it, for least-recently-used eviction
typedef struct { id<MTL4FXSpatialScaler> sc; id<MTLTexture> in, out, read; NSUInteger key[7]; uint64_t used; int refused; } up_mfx_scaler;
enum { UP_MFX_SCALERS = 8 };   // dynamic resolution moves the input; a refused format keeps its slot
static up_mfx_scaler g_up_mfx[UP_MFX_SCALERS];
static uint64_t g_up_mfx_use;
static uint64_t g_up_mfx_n, g_up_mfx_kept, g_up_mfx_straight[2];
static int up_mfx_on(void) {
    static int v = -1;
    if (v >= 0) return v;
    const char *e = getenv("DM_UPSCALER");
    v = e && !strcmp(e, "metalfx");
    if (e && *e && strcmp(e, "metalfx") && strcmp(e, "game")) MLOG("[RED] DM_UPSCALER=%s: metalfx or game; keeping the game's", e);
    if (v && !(g_q4 && [MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev])) { MLOG("upscaler: this GPU has no Metal 4 MetalFX spatial scaler; the game's own upscaler draws"); v = 0; }
    if (v) MLOG("upscaler (DM_UPSCALER): metalfx, the game's upscaler shaders (%d, from the shader map) are drawn by MetalFX's spatial scaler", DM_GAME_UPSCALER_N);
    return v;
}
// The colour a network takes, read from the sRGB curves' constants in its code (docs/reference/rendering.md, An upscaler's colour read from its code): float encode and decode: linear,
// HDR mode; half decode, no encode: sRGB in, perceptual; signed squares and no curve (agl_ndsr_vr): squared on the copy in, HDR mode; anything else keeps the game's shader.
enum { UP_COLOR_UNREAD, UP_COLOR_LINEAR, UP_COLOR_SRGB, UP_COLOR_KEEP, UP_COLOR_ROOT };   // UP_COLOR_KEEP, UP_COLOR_UNREAD: the game's shader
static int up_mfx_uses(const void *code, size_t n, const void *v, size_t w) {
    const uint8_t *p = code, *end = p + n; int hits = 0;
    while (hits < 8 && (p = memmem(p, end - p, v, w))) { hits++; p += w; }
    return hits == 8;
}
// MUFU.EX2 / LG2, which every power takes (Ampere: opcode 0x108 in bits 0..8; function at bits 74..79: EX2 2, LG2 3)
static int up_mfx_has_pow(const void *code, size_t n) {
    for (size_t o = 0; o + 16 <= n; o += 16) {
        uint64_t lo, hi; memcpy(&lo, (const uint8_t *)code + o, 8); memcpy(&hi, (const uint8_t *)code + o + 8, 8);
        if ((lo & 0x1ff) == 0x108 && ((hi >> 10) & 0x3f) - 2 < 2) return 1;
    }
    return 0;
}
// FMUL (0x220) / HMUL2 (0x232) of a register by itself, |x| on one side (Ra 24..31, Rb 32..39; abs: Rb bit 62, Ra 73; HMUL2 swizzles: A 74..75, B 60..61), from 8 uses
static int up_mfx_squares(const void *code, size_t n) {
    int hits = 0;
    for (size_t o = 0; o + 16 <= n && hits < 8; o += 16) {
        uint64_t lo, hi; memcpy(&lo, (const uint8_t *)code + o, 8); memcpy(&hi, (const uint8_t *)code + o + 8, 8);
        unsigned op = lo & 0xfff, ra = (lo >> 24) & 0xff, rb = (lo >> 32) & 0xff;
        if ((op != 0x220 && op != 0x232) || ra != rb || ra == 0xff || ((hi >> 9) & 1) == ((lo >> 62) & 1)) continue;
        if (op == 0x232 && ((hi >> 10) & 3) != ((lo >> 60) & 3)) continue;
        hits++;
    }
    return hits == 8;
}
static int up_mfx_color(const void *code, size_t n) {
#define UP_F32(x) ({ float f_ = (x); up_mfx_uses(code, n, &f_, 4); })
#define UP_F16(x) ({ __fp16 f_ = (x); up_mfx_uses(code, n, &f_, 2); })
    int enc32 = UP_F32(0.0031308f) && UP_F32(1 / 2.4f), dec32 = UP_F32(0.04045f) && UP_F32(2.4f);
    int enc16 = UP_F16(0.41650390625f) || UP_F16(0.416748046875f);   // 1 / 2.4 either way rounded: it must not read as encoded input
    int dec16 = UP_F16(0.040435791015625f) && UP_F16(2.3984375f) && UP_F16(0.07745361328125f);
    // square-rooted input: squares, and neither a curve's constant nor a power
    int part = UP_F32(0.0031308f) || UP_F32(1 / 2.4f) || UP_F32(0.04045f) || UP_F32(2.4f) || UP_F32(12.92f) || UP_F32(1 / 12.92f) || enc16 || UP_F16(0.040435791015625f) || UP_F16(2.3984375f) ||
               UP_F16(0.07745361328125f) || UP_F16(12.92f);
#undef UP_F32
#undef UP_F16
    if (!part && !up_mfx_has_pow(code, n) && up_mfx_squares(code, n)) return UP_COLOR_ROOT;
    return enc32 && dec32 && !enc16 && !dec16 ? UP_COLOR_LINEAR : dec16 && !enc16 && !enc32 && !dec32 ? UP_COLOR_SRGB : UP_COLOR_KEEP;
}
static int up_key_listed(const char *keys, const char *key) {
    for (const char *k = keys; *k; k += k[16] == ',' ? 17 : 16) if (!strncmp(k, key, 16)) return 1;
    return 0;
}
static int up_mfx_named(NvShader *cs) { return up_key_listed(DM_GAME_UPSCALER_KEYS, cs->key); }
// The share of MetalFX's picture in the game's image (DM_UPSCALER_SHARPNESS, else the profile's upscaler_sharpness; 1 = all); the rest is its input scaled bilinearly.
// MetalFX's spatial scaler has no strength of its own. Not for sRGB input, which only that draw decodes.
static float g_up_sharp = -1;
static float up_mfx_sharpness(void) {
    if (g_up_sharp >= 0) return g_up_sharp;
    const char *e = getenv("DM_UPSCALER_SHARPNESS");
    float v = e && *e ? strtof(e, NULL) : DM_GAME_UPSCALER_SHARPNESS;
    g_up_sharp = v >= 0 && v <= 1 ? v : 1;
    if (g_up_sharp < 1) MLOG("upscaler sharpness %.2f: MetalFX's picture blended with %.0f %% of a bilinear one", g_up_sharp, 100 * (1 - g_up_sharp));
    return g_up_sharp;
}
static void up_mfx_read_color(NvShader *cs, const NVNshaderData *sd) {
    if (cs->up_color || !DM_GAME_UPSCALER_N || !up_mfx_on() || !up_mfx_named(cs)) return;
    uint32_t nctl, ncode = 0;
    if (sd->control) nvn_ctl_lens(sd->control, &nctl, &ncode);
    cs->up_color = sd->data && ncode && ncode < (1u << 24) ? up_mfx_color((const void *)(uintptr_t)sd->data, ncode) : UP_COLOR_KEEP;
    MLOG("upscaler %s: %s", cs->key, cs->up_color == UP_COLOR_LINEAR ? "linear input and output, MetalFX in HDR mode" : cs->up_color == UP_COLOR_SRGB ? "sRGB-encoded input, linear output, MetalFX in perceptual mode" :
                            cs->up_color == UP_COLOR_ROOT ? "square-rooted input, linear output (the shader squares): squared into linear, then MetalFX in HDR mode" : "neither of the colour shapes MetalFX draws; the game's shader runs");
}
static int up_mfx_wanted(NvShader *cs) {
    if (cs->up_mfx) return cs->up_mfx == 1;
    cs->up_mfx = 2;
    if (!DM_GAME_UPSCALER_N || !up_mfx_on()) return 0;
    if (up_mfx_named(cs) && (cs->up_color == UP_COLOR_LINEAR || cs->up_color == UP_COLOR_SRGB || cs->up_color == UP_COLOR_ROOT)) cs->up_mfx = 1;
    return cs->up_mfx == 1;
}
// NULL when MetalFX made none (remembered)
static up_mfx_scaler *up_mfx_scaler_for(NSUInteger iw, NSUInteger ih, NSUInteger ow, NSUInteger oh, int srgb, MTLPixelFormat cf, MTLPixelFormat of) {
    NSUInteger key[7] = {iw, ih, ow, oh, (NSUInteger)srgb, (NSUInteger)cf, (NSUInteger)of};
    up_mfx_scaler *e = NULL;
    for (int i = 0; i < UP_MFX_SCALERS; i++) {
        if (!memcmp(g_up_mfx[i].key, key, sizeof key)) { g_up_mfx[i].used = ++g_up_mfx_use; return g_up_mfx[i].refused ? NULL : &g_up_mfx[i]; }
        if (!e || g_up_mfx[i].used < e->used) e = &g_up_mfx[i];
    }
    // a recorded, held or running submission may still use the slot's scaler: its textures leave the set and it goes once those have completed (a Metal 4 command buffer
    // does not retain the scaler)
    if (e->read && e->read != e->out) defer_obj_unref(e->read);
    if (e->in) defer_obj_release(e->in);
    if (e->out) defer_obj_release(e->out);
    if (e->sc) defer_obj_release(e->sc);
    *e = (up_mfx_scaler){.key = {iw, ih, ow, oh, (NSUInteger)srgb, (NSUInteger)cf, (NSUInteger)of}, .used = ++g_up_mfx_use, .refused = 1};
    NSError *err = nil; uint64_t t0 = now_ns();
    if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
    d.colorTextureFormat = cf; d.outputTextureFormat = of;
    d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh;
    d.colorProcessingMode = srgb ? MTLFXSpatialScalerColorProcessingModePerceptual : MTLFXSpatialScalerColorProcessingModeHDR;
    id<MTL4FXSpatialScaler> sc = nil;
    @try { sc = g_fx_comp ? [d newSpatialScalerWithDevice:g_dev compiler:g_fx_comp] : nil; } @catch (NSException *x) { sc = nil; }   // a format it does not take may throw
    int own = cf == MTLPixelFormatRGBA16Float && of == (srgb ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float);   // every Metal 4 MetalFX takes these
    MLOG("%supscaler: MetalFX spatial scaler (%s) %lux%lu format %lu → %lux%lu format %lu%s, %.1f ms", sc || !own ? "" : "[RED] ", srgb ? "perceptual" : "HDR", (unsigned long)iw, (unsigned long)ih, (unsigned long)cf,
         (unsigned long)ow, (unsigned long)oh, (unsigned long)of, sc ? "" : own ? ": MetalFX made none; the game's upscaler draws this size" : ": MetalFX made none; the scaler's own format stands in", (now_ns() - t0) / 1e6);
    if (!sc) return NULL;
    e->sc = sc; e->refused = 0;
    return e;
}
// output 0: the scaler's input; 1: its output, returning what the game's image is drawn from. sRGB input's output stays encoded; an sRGB view decodes it.
static id<MTLTexture> up_mfx_scratch(up_mfx_scaler *e, int output) {
    if (output ? e->read : e->in) return output ? e->read : e->in;
    id<MTL4FXSpatialScaler> sc = e->sc; int srgb = (int)e->key[4];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:output ? sc.outputTextureFormat : sc.colorTextureFormat width:e->key[output ? 2 : 0] height:e->key[output ? 3 : 1] mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = output ? sc.outputTextureUsage | MTLTextureUsageShaderRead | (srgb ? MTLTextureUsagePixelFormatView : 0) : sc.colorTextureUsage | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    id<MTLTexture> view = output && srgb ? [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB] : t;
    if (!t || !view) { MLOG("[RED] upscaler: cannot create MetalFX's %s (%lux%lu format %lu); the game's upscaler draws", output ? "output" : "input", (unsigned long)td.width, (unsigned long)td.height, (unsigned long)td.pixelFormat); return nil; }
    t.label = output ? @"upscaler output" : @"upscaler input"; res_add(t); res_commit();
    if (!output) {
        e->in = t;
        return t;
    }
    e->out = t;
    e->read = view;
    return view;
}
// DM_NVN_MFX_DIRECT=0: only an RGBA16F input is the game's own texture
static int up_mfx_direct_on(void) {
    static int m = -1;
    if (m < 0) {
        m = sw_mode("DM_NVN_MFX_DIRECT", 1);
        if (m != 1) MLOG("DM_NVN_MFX_DIRECT=%d: MetalFX takes the game's input only in RGBA16F%s", m, m ? " in the other A/B phases" : "");
    }
    return sw_on(m);
}
static int up_mfx_rect(const uint32_t v[4], NSUInteger w, NSUInteger h, NVNcopyRegion *r) {
    uint64_t rw = (uint64_t)v[2] + 1, rh = (uint64_t)v[3] + 1;
    if (v[0] > INT_MAX || v[1] > INT_MAX || rw > INT_MAX || rh > INT_MAX || (uint64_t)v[0] + rw > w || (uint64_t)v[1] + rh > h) return 0;
    *r = (NVNcopyRegion){(int)v[0], (int)v[1], 0, (int)rw, (int)rh, 1};
    return 1;
}
// 1 when drawn, 0 to run the shader
static int up_mfx_run(id<MTLTexture> in, id<MTLTexture> out, NVNcopyRegion ir, NVNcopyRegion orr, int color_shape, const char *key);
static int up_mfx_encode(NvShader *cs) {
    int ti = -1, oi = -1;
    for (int i = 0; i < cs->rf.nb; i++) {
        int k = cs->rf.b[i].kind;
        if (k == NVN_RB_TEX && ti < 0) ti = i;
        else if (k == NVN_RB_IMG && oi < 0) oi = i;
        else if (k == NVN_RB_TEX || k == NVN_RB_IMG) return 0;
    }
    if (ti < 0 || oi < 0) {
        static int once;
        if (!once++) MLOG("[RED] upscaler %s: not one texture and one image; the shader draws", cs->key);
        return 0;
    }
    int tin = (int)(bound_tex(5, cs->rf.b[ti].reg) & 0xfffff);
    int tout = cs->rf.b[oi].reg < 32 ? (int)(bound_img(5, cs->rf.b[oi].reg) & 0xfffff) : 0;
    id<MTLTexture> in, out;
    os_unfair_lock_lock(&g_rmu);
    int ok = tin > 0 && tin < g_ntreg && g_treg[tin].ok && tout > 0 && tout < g_ntreg && g_treg[tout].ok;
    in = ok ? tex_by_id(tin) : nil;
    // the image binding, not the sampled view, which can be a read-only swizzle over a writable image
    uint64_t rid = 0; int real = 0;
    out = ok ? img_binding(tout, &cs->rf.b[oi], &rid, &real) : nil;
    if (!real) out = nil;
    os_unfair_lock_unlock(&g_rmu);
    uint32_t v[8]; memcpy(v, S.push[1], sizeof v);
    NVNcopyRegion ir, orr;
    if (!in || !out || in == out || in.textureType != MTLTextureType2D || out.textureType != MTLTextureType2D || in.sampleCount != 1 || out.sampleCount != 1 ||
        fmt_int_or_ds(in.pixelFormat) || fmt_int_or_ds(out.pixelFormat) || mtl_fmt_block(out.pixelFormat) || !up_mfx_rect(v, in.width, in.height, &ir) || !up_mfx_rect(v + 4, out.width, out.height, &orr) ||
        orr.width < ir.width || orr.height < ir.height || (orr.width == ir.width && orr.height == ir.height)) { g_up_mfx_kept++; return 0; }
    if (!up_mfx_run(in, out, ir, orr, cs->up_color, cs->key)) { g_up_mfx_kept++; return 0; }
    return 1;
}
// color_shape: UP_COLOR_LINEAR (HDR mode), UP_COLOR_SRGB (decoded into out), UP_COLOR_ROOT (squared on the copy in, HDR mode). 1 when drawn, 0 when out is untouched.
static int up_mfx_run(id<MTLTexture> in, id<MTLTexture> out, NVNcopyRegion ir, NVNcopyRegion orr, int color_shape, const char *key) {
    int srgb = color_shape == UP_COLOR_SRGB, root = color_shape == UP_COLOR_ROOT;
    float soft = srgb ? 0 : 1 - up_mfx_sharpness();
    // A side is the game's texture when it is whole and not sRGB; a square-rooted input only through the copy (COPY_T2T_SQUARE). The output only in the scaler's format: MetalFX's own
    // R11G11B10F writes come out about two mantissa steps low (t_upscaler_mfx). sRGB input's and a softened output are the scaler's own.
    MTLPixelFormat c16 = MTLPixelFormatRGBA16Float, of = srgb ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float;
    int whole_in = !root && !ir.xoffset && !ir.yoffset && (NSUInteger)ir.width == in.width && (NSUInteger)ir.height == in.height && pf_srgb_linear(in.pixelFormat) == MTLPixelFormatInvalid && !mtl_fmt_block(in.pixelFormat);
    int whole_out = !srgb && soft <= 0 && !orr.xoffset && !orr.yoffset && (NSUInteger)orr.width == out.width && (NSUInteger)orr.height == out.height && out.storageMode == MTLStorageModePrivate && out.pixelFormat == of;
    MTLPixelFormat cf = whole_in && (in.pixelFormat == c16 || up_mfx_direct_on()) ? in.pixelFormat : c16;
    up_mfx_scaler *e = NULL; int sin = 0, sout = 0;
    for (int t = 0; t < 2 && !e; t++) {
        if (t && cf == c16) break;
        MTLPixelFormat c = t ? c16 : cf;
        if (!(e = up_mfx_scaler_for((NSUInteger)ir.width, (NSUInteger)ir.height, (NSUInteger)orr.width, (NSUInteger)orr.height, srgb, c, of))) continue;
        sin = whole_in && c == in.pixelFormat && (in.usage & e->sc.colorTextureUsage) == e->sc.colorTextureUsage;
        sout = whole_out && (out.usage & e->sc.outputTextureUsage) == e->sc.outputTextureUsage;
        if (c != c16 && !sin) {
            static int once;
            if (!once++) MLOG("upscaler: MetalFX's colour usage %#lx is not the game's texture's (%#lx): the input goes through RGBA16F", (unsigned long)e->sc.colorTextureUsage, (unsigned long)in.usage);
            e = NULL;
        }
    }
    if (!e || (!sout && !(out.usage & MTLTextureUsageRenderTarget))) return 0;
    id<MTL4FXSpatialScaler> sc = e->sc;
    id<MTLTexture> color = sin ? in : up_mfx_scratch(e, 0);
    id<MTLTexture> drawn = sout ? out : up_mfx_scratch(e, 1);
    id<MTLTexture> result = sout ? out : e->out;
    if (!color || !drawn) return 0;
    rp_end();
    if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ubd_land_compute();
    NVNcopyRegion at0 = {0, 0, 0, ir.width, ir.height, 1}, at0o = {0, 0, 0, orr.width, orr.height, 1};
    if (!sin && !copy_t2t_draw(in, color, 0, 0, 0, 0, &ir, &at0, 0, root ? COPY_T2T_SQUARE : 0)) return 0;
    mfx_order(g_cb, nil, MTLOriginMake(0, 0, 0), MTLSizeMake(0, 0, 0), nil);
    NSUInteger cw = (NSUInteger)ir.width, ch = (NSUInteger)ir.height;
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {
        sc.colorTexture = color; sc.outputTexture = result; sc.inputContentWidth = cw; sc.inputContentHeight = ch;
        [sc encodeToCommandBuffer:cb];
    });
    g_copy_blend.mix = soft; g_copy_blend.t1 = color;
    if (!sout && !copy_t2t_draw(drawn, out, 0, 0, 0, 0, &at0o, &orr, 0, soft > 0 ? COPY_T2T_BLEND : 0)) {
        g_copy_blend.t1 = nil;   // only the scratch image was scaled; the game's shader can still produce the output
        args_dirty_all();
        return 0;
    }
    g_copy_blend.t1 = nil;
    args_dirty_all();
    g_up_mfx_straight[0] += sin; g_up_mfx_straight[1] += sout;
    if (!g_up_mfx_n++) MLOG("upscaler: first dispatch of %s drawn by MetalFX, %dx%d at %d,%d of %lux%lu (format %lu, %s) → %dx%d at %d,%d of %lux%lu (format %lu, %s)%s", key, ir.width, ir.height, ir.xoffset, ir.yoffset,
                            (unsigned long)in.width, (unsigned long)in.height, (unsigned long)in.pixelFormat, sin ? "straight" : "copied", orr.width, orr.height, orr.xoffset, orr.yoffset, (unsigned long)out.width,
                            (unsigned long)out.height, (unsigned long)out.pixelFormat, sout ? "straight" : "drawn back", srgb ? ", decoded from sRGB" : root ? ", squared from its square roots" : "");
    return 1;
}
// agl's variable-ratio NDSR (agl_ndsr_vr) is a chain of compute dispatches (DM_GAME_UPSCALER_CHAIN_KEYS) told apart by what they bind: none runs, and MetalFX draws the first one's
// texture into the last one's image. Another shape gives a red line and the game's shaders from the next chain on (docs/reference/rendering.md, An upscaler that runs as a chain of dispatches drawn by MetalFX).
enum { UP_CHAIN_MAX = 8 };   // this game's has four
static id<MTLTexture> g_up_chain_in;
static int g_up_chain_n, g_up_chain_off;
static uint64_t g_up_chain_frame, g_up_chain_drawn;
static int up_chain_named(NvShader *cs) {
    if (!cs->up_chain) cs->up_chain = up_key_listed(DM_GAME_UPSCALER_CHAIN_KEYS, cs->key) ? 1 : 2;
    return cs->up_chain == 1;
}
static int up_chain_fail(NvShader *cs, const char *why) {
    MLOG("[RED] upscaler chain: %s (%s, dispatch %d of the chain); the game's shaders draw from the next chain on", why, cs->key, g_up_chain_n + 1);
    g_up_chain_off = 1; g_up_chain_in = nil; g_up_chain_n = 0;
    return 0;
}
static int up_chain_bind(NvShader *cs, int k) {
    int r = -1;
    for (int i = 0; i < cs->rf.nb; i++) if (cs->rf.b[i].kind == k) { if (r >= 0) return -1; r = i; }
    return r;
}
// 1 when it belongs to a chain MetalFX draws (nothing to run), 0 to run the shader
static int up_chain_dispatch(NvShader *cs, int indirect) {
    if (!DM_GAME_UPSCALER_CHAIN_N || g_up_chain_off || !up_mfx_on()) return 0;
    uint64_t frame = atomic_load(&g_cur_frame);
    if (!up_chain_named(cs)) {
        if (g_up_chain_in) up_chain_fail(cs, "another compute shader was dispatched inside the chain");
        return 0;
    }
    if (g_up_chain_in && frame != g_up_chain_frame) { up_chain_fail(cs, "the chain did not reach its output image in its frame"); return 0; }
    if (!g_up_chain_in) {
        int ti = up_chain_bind(cs, NVN_RB_TEX);
        if (indirect || ti < 0) return up_chain_fail(cs, "its first dispatch is indirect or has not one texture");
        int tin = (int)(tex_slot_h(S.tex[5], S.img[5], cs->rf.b[ti].reg) & 0xfffff);
        os_unfair_lock_lock(&g_rmu);
        id<MTLTexture> in = tin > 0 && tin < g_ntreg && g_treg[tin].ok ? tex_by_id(tin) : nil;
        os_unfair_lock_unlock(&g_rmu);
        if (!in || in.textureType != MTLTextureType2D || in.sampleCount != 1 || fmt_int_or_ds(in.pixelFormat) || mtl_fmt_block(in.pixelFormat)) return up_chain_fail(cs, "its first dispatch's texture is not a single-sampled 2D colour texture");
        g_up_chain_in = in; g_up_chain_frame = frame; g_up_chain_n = 1;
        return 1;
    }
    // the output when its one image is a colour image larger than the picture, else a step only the chain reads
    id<MTLTexture> in = g_up_chain_in, out = nil;
    int oi = up_chain_bind(cs, NVN_RB_IMG);
    if (!indirect && oi >= 0 && cs->rf.b[oi].reg < 32) {
        int tout = (int)(S.img[5][cs->rf.b[oi].reg] & 0xfffff); uint64_t rid = 0; int real = 0;
        os_unfair_lock_lock(&g_rmu);
        if (tout > 0 && tout < g_ntreg && g_treg[tout].ok) { out = img_binding(tout, &cs->rf.b[oi], &rid, &real); if (!real) out = nil; }
        os_unfair_lock_unlock(&g_rmu);
        if (out && (out == in || out.textureType != MTLTextureType2D || out.sampleCount != 1 || fmt_int_or_ds(out.pixelFormat) || mtl_fmt_block(out.pixelFormat) || out.width < in.width || out.height < in.height ||
                    (out.width == in.width && out.height == in.height))) out = nil;
    }
    if (!out) {
        if (++g_up_chain_n > UP_CHAIN_MAX) return up_chain_fail(cs, "no dispatch wrote a colour image larger than the first one's texture");
        return 1;
    }
    NVNcopyRegion ir = {0, 0, 0, (int)in.width, (int)in.height, 1}, orr = {0, 0, 0, (int)out.width, (int)out.height, 1};
    if (!up_mfx_run(in, out, ir, orr, UP_COLOR_LINEAR, cs->key)) { up_chain_fail(cs, "MetalFX did not draw the output"); return 1; }   // the steps before it were not run: the network has nothing to read
    g_up_chain_in = nil; g_up_chain_n = 0; g_up_chain_drawn++;
    return 1;
}
