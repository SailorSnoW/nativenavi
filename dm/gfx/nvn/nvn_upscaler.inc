// A game's own upscaler, and its chain, drawn by MetalFX (DM_UPSCALER).
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---------- A game's own upscaler drawn by MetalFX (DM_UPSCALER) ----------
// Donkey Kong Bananza upscales its frame with agl's NDSR network: one compute dispatch (agl_ndsr.pck, 24 variants; Splatoon Raiders ships 20) reading the scene colour (texture slot 0, R11G11B10F; see up_mfx_color)
// and writing the output picture (image slot 1), between the render-resolution passes and the output-resolution ones, which draw the UI over it. Its push constants (c[24], S.push[1])
// give the two rectangles: input origin x, y and last texel relative to it (inclusive), then the same for the output. On an M1 the network takes 3.5-5 ms a frame in handheld mode
// (960x540 → 1920x1080), MetalFX's spatial scaler about 1 ms. With DM_UPSCALER=metalfx (the launcher's 3D upscaler: MetalFX), a dispatch of one of the game's upscaler shaders
// (DM_GAME_UPSCALER_KEYS: game.py takes the compute shaders the shader map found in an NDSR pack, whatever engine ships it) is replaced by the spatial scaler from the input rectangle to the output rectangle, in the colour mode of that shader. MetalFX takes whole textures: each side is the
// game's own texture where it can be (up_mfx_encode; the input in its own format), else a texture of the scaler's own (RGBA16F; RGBA8Unorm for the output of sRGB input), which the input rectangle is drawn
// into first or which is drawn into the output rectangle after (copy_t2t_draw, a render pass each). An output rectangle not larger than the input in both directions, an indirect dispatch, or anything
// the checks below refuse runs the game's shader. The look changes (Apple's scaler instead of the game's network), so it is the player's choice; DM_UPSCALER=game (the default)
// keeps the network. Found with an earlier NDSR replacement (PR #1073), which decoded the rectangles. The network's own cost on the GPU is the translator's
// LDSM / MOVM shuffles, open in docs/reference/shaders.md (Ampere HMMA through simdgroup_matrix)
// key: input w, h, output w, h, sRGB input, colour format, output format. in, out: the scaler's own input and output, made on first use (up_mfx_scratch) by a side that cannot take the game's texture;
// read: what the game's image is drawn from (out, or its sRGB view)
typedef struct { id<MTL4FXSpatialScaler> sc; id<MTLTexture> in, out, read; NSUInteger key[7]; int refused; } up_mfx_scaler;
enum { UP_MFX_SCALERS = 8 };   // dynamic resolution moves the input, and a format MetalFX refuses keeps its slot (refused) beside the RGBA16F scaler that stands in
static up_mfx_scaler g_up_mfx[UP_MFX_SCALERS]; static int g_up_mfx_next;
static uint64_t g_up_mfx_n, g_up_mfx_kept, g_up_mfx_straight[2];   // dispatches drawn by MetalFX, dispatches of a named shader left to it, of the drawn: input and output that were the game's own textures
static int up_mfx_on(void) {
    static int v = -1;
    if (v >= 0) return v;
    const char *e = getenv("DM_UPSCALER"); v = e && !strcmp(e, "metalfx");
    if (e && *e && strcmp(e, "metalfx") && strcmp(e, "game")) MLOG("[RED] DM_UPSCALER=%s: metalfx or game; keeping the game's", e);
    if (v && !(g_q4 && [MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev])) { MLOG("upscaler: this GPU has no Metal 4 MetalFX spatial scaler; the game's own upscaler draws"); v = 0; }
    if (v) MLOG("upscaler (DM_UPSCALER): metalfx, the game's upscaler shaders (%d, from the shader map) are drawn by MetalFX's spatial scaler", DM_GAME_UPSCALER_N);
    return v;
}
// The colour a variant of the network takes and writes, from the sRGB curves in its code (their constants are immediates, whole bytes in the code). The variants differ inside a pack, so this is the shader's and not the game's:
// Bananza's all encode linear input (0.0031308, 1 / 2.4 as floats) and come in threes: no decode at the end (the output stays encoded), a decode clamped to 0-1, a decode up to 128. Splatoon Raiders' all read colour that is encoded
// already and come in twos: no decode, a decode clamped to 0-1 (as half floats: 0.04045, 2.4 and 1 / 12.92 rounded to 0.0404358, 2.3984375 and 0.0774536). Only the variants with linear output are drawn, in these two shapes; a variant
// with encoded output keeps the game's shader. A constant counts from 8 uses, far fewer than a curve per channel of every output tile: two bytes also match by chance.
// A third shape converts by squaring: Ocarina of Time's agl_ndsr_vr.pck and agl_ndsr_vr_light.pck networks read colour a pass before them square-rooted (gamma 2, into RGB9E5) and
// write their result squared with its sign kept, x * |x| (FMUL in _vr, HMUL2 in _vr_light; up_mfx_squares), into R11G11B10F: square-rooted input, linear output, with no power and no
// curve constant. Drawn as linear, MetalFX left the result root-encoded (a washed-out picture, PR #1459); its input is squared on the copy into the scaler's RGBA16F texture instead
// (COPY_T2T_SQUARE), then HDR mode. Of the two packs' 37 compute shaders in that game: 18 square-rooted networks (10 in _vr, 8 in _vr_light), 12 that square and then encode (the curve
// in floats with its powers: kept) and 7 small pre-passes. Any other shape keeps the game's shader, a variant with no curve and no square among them: nothing in its code says
// what its colour is (Ocarina of Time's pre-passes square-root with MUFU.SQRT and no constant)
enum { UP_COLOR_UNREAD, UP_COLOR_LINEAR, UP_COLOR_SRGB, UP_COLOR_KEEP, UP_COLOR_ROOT };   // linear in and out: HDR mode; sRGB in, linear out: perceptual mode, then decoded; square-rooted in,
                                                                                         // linear out: squared into linear, then HDR mode; UP_COLOR_KEEP, UP_COLOR_UNREAD: the game's shader
static int up_mfx_uses(const void *code, size_t n, const void *v, size_t w) {
    const uint8_t *p = code, *end = p + n; int hits = 0;
    while (hits < 8 && (p = memmem(p, end - p, v, w))) { hits++; p += w; }
    return hits == 8;
}
// Whether the code has a MUFU.EX2 or MUFU.LG2 (Ampere: opcode 0x108 in the low 9 bits, any form; the function at bits 74..79, EX2 2 and LG2 3), the instructions every power takes
static int up_mfx_has_pow(const void *code, size_t n) {
    for (size_t o = 0; o + 16 <= n; o += 16) {
        uint64_t lo, hi; memcpy(&lo, (const uint8_t *)code + o, 8); memcpy(&hi, (const uint8_t *)code + o + 8, 8);
        if ((lo & 0x1ff) == 0x108 && ((hi >> 10) & 0x3f) - 2 < 2) return 1;
    }
    return 0;
}
// Whether the code squares a value keeping its sign, from 8 times: FMUL (0x220) or HMUL2 (0x232) of a register by itself (not RZ), with |x| on exactly one side (Ra 24..31, Rb 32..39;
// Rb's abs at bit 62, Ra's at 73), and for HMUL2 the same lanes on both sides (A's swizzle at 74..75, B's at 60..61). Ocarina of Time's square-rooted networks have 32 to 108
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
    int enc16 = UP_F16(0.41650390625f) || UP_F16(0.416748046875f), dec16 = UP_F16(0.040435791015625f) && UP_F16(2.3984375f) && UP_F16(0.07745361328125f);   // enc16 (1 / 2.4 either way rounded): no pack has it; it must not read as encoded input
    // Square-rooted input: squares, and no part of a curve, neither a constant of one (each counted alone, from 8 uses as above) nor a power, which every curve takes wherever its constants come from
    int part = UP_F32(0.0031308f) || UP_F32(1 / 2.4f) || UP_F32(0.04045f) || UP_F32(2.4f) || UP_F32(12.92f) || UP_F32(1 / 12.92f) || enc16 || UP_F16(0.040435791015625f) || UP_F16(2.3984375f) ||
               UP_F16(0.07745361328125f) || UP_F16(12.92f);
#undef UP_F32
#undef UP_F16
    if (!part && !up_mfx_has_pow(code, n) && up_mfx_squares(code, n)) return UP_COLOR_ROOT;
    return enc32 && dec32 && !enc16 && !dec16 ? UP_COLOR_LINEAR : dec16 && !enc16 && !enc32 && !dec32 ? UP_COLOR_SRGB : UP_COLOR_KEEP;
}
// Whether cs is one of the game's upscaler shaders (DM_GAME_UPSCALER_KEYS: comma-separated 16-digit keys)
static int up_mfx_named(NvShader *cs) {
    for (const char *k = DM_GAME_UPSCALER_KEYS; *k; k += k[16] == ',' ? 17 : 16) if (!strncmp(k, cs->key, 16)) return 1;
    return 0;
}
// How much of MetalFX's own picture the game's image gets (DM_UPSCALER_SHARPNESS, the launcher's 3D upscaler sharpness; the profile's upscaler_sharpness when unset; 1 = all of it): the rest
// is the scaler's input scaled bilinearly, blended in by the draw into the game's image. MetalFX's spatial scaler has no strength of its own (its one setting is the colour mode), and
// agl's networks give a softer picture than it does. Not for sRGB-encoded input, whose result is decoded only by that draw
static float g_up_sharp = -1;
static float up_mfx_sharpness(void) {
    if (g_up_sharp >= 0) return g_up_sharp;
    const char *e = getenv("DM_UPSCALER_SHARPNESS"); float v = e && *e ? strtof(e, NULL) : DM_GAME_UPSCALER_SHARPNESS;
    g_up_sharp = v >= 0 && v <= 1 ? v : 1;
    if (g_up_sharp < 1) MLOG("upscaler sharpness %.2f: MetalFX's picture blended with %.0f %% of a bilinear one", g_up_sharp, 100 * (1 - g_up_sharp));
    return g_up_sharp;
}
static void up_mfx_read_color(NvShader *cs, const NVNshaderData *sd) {
    if (cs->up_color || !DM_GAME_UPSCALER_N || !up_mfx_on() || !up_mfx_named(cs)) return;
    uint32_t nctl, ncode = 0; if (sd->control) nvn_ctl_lens(sd->control, &nctl, &ncode);
    cs->up_color = sd->data && ncode && ncode < (1u << 24) ? up_mfx_color((const void *)(uintptr_t)sd->data, ncode) : UP_COLOR_KEEP;
    MLOG("upscaler %s: %s", cs->key, cs->up_color == UP_COLOR_LINEAR ? "linear input and output, MetalFX in HDR mode" : cs->up_color == UP_COLOR_SRGB ? "sRGB-encoded input, linear output, MetalFX in perceptual mode" :
                            cs->up_color == UP_COLOR_ROOT ? "square-rooted input, linear output (the shader squares): squared into linear, then MetalFX in HDR mode" : "neither of the colour shapes MetalFX draws; the game's shader runs");
}
// Whether cs is one of the game's upscaler shaders, the player chose MetalFX and its colour is one MetalFX draws
static int up_mfx_wanted(NvShader *cs) {
    if (cs->up_mfx) return cs->up_mfx == 1;
    cs->up_mfx = 2;
    if (!DM_GAME_UPSCALER_N || !up_mfx_on()) return 0;
    if (up_mfx_named(cs) && (cs->up_color == UP_COLOR_LINEAR || cs->up_color == UP_COLOR_SRGB || cs->up_color == UP_COLOR_ROOT)) cs->up_mfx = 1;
    return cs->up_mfx == 1;
}
// The scaler for these sizes, colour (sRGB input: perceptual mode) and formats: cf, the colour texture's, and of, the output's; NULL when MetalFX made none (remembered)
static up_mfx_scaler *up_mfx_scaler_for(NSUInteger iw, NSUInteger ih, NSUInteger ow, NSUInteger oh, int srgb, MTLPixelFormat cf, MTLPixelFormat of) {
    NSUInteger key[7] = {iw, ih, ow, oh, (NSUInteger)srgb, (NSUInteger)cf, (NSUInteger)of};
    for (int i = 0; i < UP_MFX_SCALERS; i++) if (!memcmp(g_up_mfx[i].key, key, sizeof key)) return g_up_mfx[i].refused ? NULL : &g_up_mfx[i];
    up_mfx_scaler *e = &g_up_mfx[g_up_mfx_next++ % UP_MFX_SCALERS];
    if (e->read && e->read != e->out) defer_obj_unref(e->read);
    if (e->in) { res_remove(e->in); defer_obj_release(e->in); } if (e->out) { res_remove(e->out); defer_obj_release(e->out); }   // a recorded scale still holds them
    *e = (up_mfx_scaler){.key = {iw, ih, ow, oh, (NSUInteger)srgb, (NSUInteger)cf, (NSUInteger)of}, .refused = 1};
    NSError *err = nil; uint64_t t0 = now_ns();
    if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
    MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
    d.colorTextureFormat = cf; d.outputTextureFormat = of; d.inputWidth = iw; d.inputHeight = ih; d.outputWidth = ow; d.outputHeight = oh;
    d.colorProcessingMode = srgb ? MTLFXSpatialScalerColorProcessingModePerceptual : MTLFXSpatialScalerColorProcessingModeHDR;
    id<MTL4FXSpatialScaler> sc = nil;
    @try { sc = g_fx_comp ? [d newSpatialScalerWithDevice:g_dev compiler:g_fx_comp] : nil; } @catch (NSException *x) { sc = nil; }   // a format it does not take may throw
    int own = cf == MTLPixelFormatRGBA16Float && of == (srgb ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float);   // the scaler's own formats, which every Metal 4 MetalFX takes
    MLOG("%supscaler: MetalFX spatial scaler (%s) %lux%lu format %lu → %lux%lu format %lu%s, %.1f ms", sc || !own ? "" : "[RED] ", srgb ? "perceptual" : "HDR", (unsigned long)iw, (unsigned long)ih, (unsigned long)cf,
         (unsigned long)ow, (unsigned long)oh, (unsigned long)of, sc ? "" : own ? ": MetalFX made none; the game's upscaler draws this size" : ": MetalFX made none; the scaler's own format stands in", (now_ns() - t0) / 1e6);
    if (!sc) return NULL;
    e->sc = sc; e->refused = 0;
    return e;
}
// The scaler's own texture for one side (output 0: its input, 1: its output, returning what the game's image is drawn from), made on first use in the scaler's format; nil when it cannot be made.
// The output of sRGB input stays encoded, in 0-1 as the network clamps it, and an sRGB view of it decodes it for the draw into the game's image
static id<MTLTexture> up_mfx_scratch(up_mfx_scaler *e, int output) {
    if (output ? e->read : e->in) return output ? e->read : e->in;
    id<MTL4FXSpatialScaler> sc = e->sc; int srgb = (int)e->key[4];
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:output ? sc.outputTextureFormat : sc.colorTextureFormat width:e->key[output ? 2 : 0] height:e->key[output ? 3 : 1] mipmapped:NO];
    td.storageMode = MTLStorageModePrivate;
    td.usage = output ? sc.outputTextureUsage | MTLTextureUsageShaderRead | (srgb ? MTLTextureUsagePixelFormatView : 0) : sc.colorTextureUsage | MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td], view = output && srgb ? [t newTextureViewWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB] : t;
    if (!t || !view) { MLOG("[RED] upscaler: cannot create MetalFX's %s (%lux%lu format %lu); the game's upscaler draws", output ? "output" : "input", (unsigned long)td.width, (unsigned long)td.height, (unsigned long)td.pixelFormat); return nil; }
    t.label = output ? @"upscaler output" : @"upscaler input"; res_add(t); res_commit();
    if (!output) return e->in = t;
    e->out = t; return e->read = view;
}
// DM_NVN_MFX_DIRECT=0|1|ab (on by default): an input texture that is the whole rectangle MetalFX reads (up_mfx_encode) is the game's texture in its own format, not only in RGBA16F; =0 keeps
// the scaler's input in RGBA16F, so only an RGBA16F input is the game's, as before. Ocarina of Time scales R11G11B10F into R11G11B10F (1440x810 → 1920x1080), which took a copy into RGBA16F and a
// draw back, a render pass each; the copy goes, the draw back stays (see up_mfx_encode). On an M6, 1,200 such dispatches: 340 µs each with both copies, 316 with the input straight, 271 with both
// straight; PR #2559 measured both going in game on an M1: 0.64 and 0.97 ms a frame, in A/B runs of both polarities
static int up_mfx_direct_on(void) {
    static int m = -1;
    if (m < 0) { m = sw_mode("DM_NVN_MFX_DIRECT", 1); if (m != 1) MLOG("DM_NVN_MFX_DIRECT=%d: MetalFX takes the game's input only in RGBA16F%s", m, m ? " in the other A/B phases" : ""); }
    return sw_on(m);
}
// A rectangle of the push constants (origin, inclusive last texel relative to it) inside a w x h texture
static int up_mfx_rect(const uint32_t v[4], NSUInteger w, NSUInteger h, NVNcopyRegion *r) {
    uint64_t rw = (uint64_t)v[2] + 1, rh = (uint64_t)v[3] + 1;
    if (v[0] > INT_MAX || v[1] > INT_MAX || rw > INT_MAX || rh > INT_MAX || (uint64_t)v[0] + rw > w || (uint64_t)v[1] + rh > h) return 0;
    *r = (NVNcopyRegion){(int)v[0], (int)v[1], 0, (int)rw, (int)rh, 1}; return 1;
}
// The dispatch of upscaler cs as MetalFX's spatial scaler from its texture slot's rectangle to its image slot's (see above): 1 when drawn, 0 to run the shader
static int up_mfx_run(id<MTLTexture> in, id<MTLTexture> out, NVNcopyRegion ir, NVNcopyRegion orr, int color_shape, const char *key);
static int up_mfx_encode(NvShader *cs) {
    int ti = -1, oi = -1;
    for (int i = 0; i < cs->rf.nb; i++) { int k = cs->rf.b[i].kind; if (k == NVN_RB_TEX && ti < 0) ti = i; else if (k == NVN_RB_IMG && oi < 0) oi = i; else if (k == NVN_RB_TEX || k == NVN_RB_IMG) return 0; }
    if (ti < 0 || oi < 0) { static int once; if (!once++) MLOG("[RED] upscaler %s: not one texture and one image; the shader draws", cs->key); return 0; }
    int tin = (int)(bound_tex(5, cs->rf.b[ti].reg) & 0xfffff), tout = cs->rf.b[oi].reg < 32 ? (int)(bound_img(5, cs->rf.b[oi].reg) & 0xfffff) : 0;
    id<MTLTexture> in, out;
    os_unfair_lock_lock(&g_rmu);
    int ok = tin > 0 && tin < g_ntreg && g_treg[tin].ok && tout > 0 && tout < g_ntreg && g_treg[tout].ok;
    in = ok ? tex_by_id(tin) : nil;
    // The sampled view can be read-only (channel swizzle) even when the image backing is writable.
    // Use the shader's image binding so its level and slice survive without replacing the backing with a sampled view.
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
// in's rectangle ir scaled by MetalFX's spatial scaler into out's rectangle orr (color_shape: UP_COLOR_LINEAR, linear colour in HDR mode; UP_COLOR_SRGB, sRGB-encoded
// input decoded into out; UP_COLOR_ROOT, square-rooted input squared into the scaler's input, HDR mode), blended with a bilinear picture of its input by up_mfx_sharpness unless sRGB.
// 1 when drawn, 0 when nothing reached out (the caller's shader can still run)
static int up_mfx_run(id<MTLTexture> in, id<MTLTexture> out, NVNcopyRegion ir, NVNcopyRegion orr, int color_shape, const char *key) {
    int srgb = color_shape == UP_COLOR_SRGB, root = color_shape == UP_COLOR_ROOT;
    float soft = srgb ? 0 : 1 - up_mfx_sharpness();
    // Each side is the game's own texture when MetalFX can take it as it is: the rectangle is the whole texture (MetalFX reads and writes from the origin) and the format is not sRGB (as the
    // copies, which keep its decode). The input in any format MetalFX takes, unless it is square-rooted: only the copy squares it (COPY_T2T_SQUARE). The output, private, only in the
    // scaler's own: MetalFX's own writes of R11G11B10F come out about two steps of its mantissa low (2.0 as 1.94, 0.5 as 0.484), where its RGBA16F output drawn back gives 1.98 and 0.496
    // (t_upscaler_mfx), so a packed output stays drawn back. sRGB input's output is always the scaler's own, for its decode, and so is a softened one's, for its blend (COPY_T2T_BLEND).
    // Else the scaler's own texture, through a copy
    MTLPixelFormat c16 = MTLPixelFormatRGBA16Float, of = srgb ? MTLPixelFormatRGBA8Unorm : MTLPixelFormatRGBA16Float;
    int whole_in = !root && !ir.xoffset && !ir.yoffset && (NSUInteger)ir.width == in.width && (NSUInteger)ir.height == in.height && pf_srgb_linear(in.pixelFormat) == MTLPixelFormatInvalid && !mtl_fmt_block(in.pixelFormat);
    int whole_out = !srgb && soft <= 0 && !orr.xoffset && !orr.yoffset && (NSUInteger)orr.width == out.width && (NSUInteger)orr.height == out.height && out.storageMode == MTLStorageModePrivate && out.pixelFormat == of;
    MTLPixelFormat cf = whole_in && (in.pixelFormat == c16 || up_mfx_direct_on()) ? in.pixelFormat : c16;
    up_mfx_scaler *e = NULL; int sin = 0, sout = 0;
    for (int t = 0; t < 2 && !e; t++) {   // the game's format first; one MetalFX refuses (no scaler made, or a usage the game's texture lacks) falls back to the scaler's own
        if (t && cf == c16) break;
        MTLPixelFormat c = t ? c16 : cf;
        if (!(e = up_mfx_scaler_for((NSUInteger)ir.width, (NSUInteger)ir.height, (NSUInteger)orr.width, (NSUInteger)orr.height, srgb, c, of))) continue;
        sin = whole_in && c == in.pixelFormat && (in.usage & e->sc.colorTextureUsage) == e->sc.colorTextureUsage;
        sout = whole_out && (out.usage & e->sc.outputTextureUsage) == e->sc.outputTextureUsage;
        if (c != c16 && !sin) {
            static int once; if (!once++) MLOG("upscaler: MetalFX's colour usage %#lx is not the game's texture's (%#lx): the input goes through RGBA16F", (unsigned long)e->sc.colorTextureUsage, (unsigned long)in.usage);
            e = NULL;
        }
    }
    if (!e || (!sout && !(out.usage & MTLTextureUsageRenderTarget))) return 0;
    id<MTL4FXSpatialScaler> sc = e->sc;
    id<MTLTexture> color = sin ? in : up_mfx_scratch(e, 0), drawn = sout ? out : up_mfx_scratch(e, 1), result = sout ? out : e->out;
    if (!color || !drawn) return 0;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    ubd_land_compute();
    NVNcopyRegion at0 = {0, 0, 0, ir.width, ir.height, 1}, at0o = {0, 0, 0, orr.width, orr.height, 1};
    if (!sin && !copy_t2t_draw(in, color, 0, 0, 0, 0, &ir, &at0, 0, root ? COPY_T2T_SQUARE : 0)) return 0;   // to the origin of the scaler's input (a nearest draw: same size)
    mfx_order(g_cb, nil, MTLOriginMake(0, 0, 0), MTLSizeMake(0, 0, 0), nil);   // the scaler after the frame's work so far, and the next pass after the scaler
    NSUInteger cw = (NSUInteger)ir.width, ch = (NSUInteger)ir.height;
    rec_cb_block(g_cb, ^(id<MTL4CommandBuffer> cb) {   // the real command buffer, on the encoding thread when it runs (the profile's enc_thread)
        sc.colorTexture = color; sc.outputTexture = result; sc.inputContentWidth = cw; sc.inputContentHeight = ch;
        [sc encodeToCommandBuffer:cb];
    });
    g_copy_blend.mix = soft; g_copy_blend.t1 = color;   // the softer picture: the scaler's input (the whole texture: the game's, or the scaler's own at the origin), linear as its result is
    if (!sout && !copy_t2t_draw(drawn, out, 0, 0, 0, 0, &at0o, &orr, 0, soft > 0 ? COPY_T2T_BLEND : 0)) {
        g_copy_blend.t1 = nil; args_dirty_all(); return 0;   // only the scratch image was scaled; the game's shader can still produce the output
    }
    g_copy_blend.t1 = nil;
    args_dirty_all();
    g_up_mfx_straight[0] += sin; g_up_mfx_straight[1] += sout;
    if (!g_up_mfx_n++) MLOG("upscaler: first dispatch of %s drawn by MetalFX, %dx%d at %d,%d of %lux%lu (format %lu, %s) → %dx%d at %d,%d of %lux%lu (format %lu, %s)%s", key, ir.width, ir.height, ir.xoffset, ir.yoffset,
                            (unsigned long)in.width, (unsigned long)in.height, (unsigned long)in.pixelFormat, sin ? "straight" : "copied", orr.width, orr.height, orr.xoffset, orr.yoffset, (unsigned long)out.width,
                            (unsigned long)out.height, (unsigned long)out.pixelFormat, sout ? "straight" : "drawn back", srgb ? ", decoded from sRGB" : root ? ", squared from its square roots" : "");
    return 1;
}
// ---------- A game's upscaler chain, drawn by MetalFX as a whole ----------
// agl's variable-ratio NDSR (agl_ndsr_vr.pck, agl_ndsr_vr_light.pck; Ocarina of Time on the Switch 2) is a chain of compute dispatches each frame, not one: a tile list read from the
// scene colour (its one texture), an indirect dispatch that marks edges, one that packs colour and edges, then the network, which is chosen by the ratio of the render size to the
// output and alone writes the output picture (a colour image larger than the scene colour), which the UI is drawn over. The packs' compute shaders are DM_GAME_UPSCALER_CHAIN_KEYS
// (game.py, from the shader map); the dispatches are told apart by what they bind, so no key or dispatch count is named here. With DM_UPSCALER=metalfx (the launcher's 3D upscaler:
// MetalFX) the chain's dispatches are not run and MetalFX's spatial scaler draws the first one's texture into the last one's image, both whole, in HDR mode: the scene colour and
// the output are both linear R11G11B10F here. The look changes (the network smooths edges as it scales, the spatial scaler only scales), so it is the player's choice.
// A chain that does not have this shape (a first dispatch without one float colour texture, no output within UP_CHAIN_MAX dispatches or within the frame, an output that is not a
// render target, MetalFX refusing) gives a red line and the game's shaders run from the next chain on; the frame it happens in may show a stale picture
enum { UP_CHAIN_MAX = 8 };   // dispatches of one chain at most: Ocarina's has four
static id<MTLTexture> g_up_chain_in; static int g_up_chain_n, g_up_chain_off; static uint64_t g_up_chain_frame, g_up_chain_drawn;
static int up_chain_named(NvShader *cs) {
    if (!cs->up_chain) { cs->up_chain = 2; for (const char *k = DM_GAME_UPSCALER_CHAIN_KEYS; *k; k += k[16] == ',' ? 17 : 16) if (!strncmp(k, cs->key, 16)) { cs->up_chain = 1; break; } }
    return cs->up_chain == 1;
}
static int up_chain_fail(NvShader *cs, const char *why) {
    MLOG("[RED] upscaler chain: %s (%s, dispatch %d of the chain); the game's shaders draw from the next chain on", why, cs->key, g_up_chain_n + 1);
    g_up_chain_off = 1; g_up_chain_in = nil; g_up_chain_n = 0; return 0;
}
// The one binding of kind k in cs (-1: none or several)
static int up_chain_bind(NvShader *cs, int k) {
    int r = -1;
    for (int i = 0; i < cs->rf.nb; i++) if (cs->rf.b[i].kind == k) { if (r >= 0) return -1; r = i; }
    return r;
}
// A dispatch of compute shader cs (indirect or not): 1 when it belongs to a chain MetalFX draws (nothing to run: the last one has been drawn by MetalFX), 0 to run the shader
static int up_chain_dispatch(NvShader *cs, int indirect) {
    if (!DM_GAME_UPSCALER_CHAIN_N || g_up_chain_off || !up_mfx_on()) return 0;
    uint64_t frame = atomic_load(&g_cur_frame);
    if (!up_chain_named(cs)) { if (g_up_chain_in) up_chain_fail(cs, "another compute shader was dispatched inside the chain"); return 0; }
    if (g_up_chain_in && frame != g_up_chain_frame) { up_chain_fail(cs, "the chain did not reach its output image in its frame"); return 0; }
    if (!g_up_chain_in) {   // the chain's first dispatch: its one texture is the picture to scale
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
    // A later dispatch: the output when its one image is a colour image larger than the picture, else a step of the chain whose result only the chain reads
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
    if (!out) { if (++g_up_chain_n > UP_CHAIN_MAX) return up_chain_fail(cs, "no dispatch wrote a colour image larger than the first one's texture"); return 1; }
    NVNcopyRegion ir = {0, 0, 0, (int)in.width, (int)in.height, 1}, orr = {0, 0, 0, (int)out.width, (int)out.height, 1};
    if (!up_mfx_run(in, out, ir, orr, UP_COLOR_LINEAR, cs->key)) { up_chain_fail(cs, "MetalFX did not draw the output"); return 1; }   // the steps before it were not run: the network has nothing to read
    g_up_chain_in = nil; g_up_chain_n = 0; g_up_chain_drawn++;
    return 1;
}
