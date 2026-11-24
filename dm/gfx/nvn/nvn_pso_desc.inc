// The pipeline description: blend sources and modes, the pipeline cache key, target classes, the side library's alpha test and logic operation entry points, NVIDIA's blender and colour rounding, pso_desc.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// NVN permits an unwritten secondary blend source (its value is undefined). Choose zero, as the shader translator's dual-source output initialization does.
// Fold its factors instead of asking Metal for an absent [[color(0), index(1)]]: MTLDevice rejects that descriptor and Metal 4's blend compiler can crash on it.
static int blend_zero_src1(int f) {
    switch (f) {
        case NVN_BLEND_FUNC_SRC1_COLOR: case NVN_BLEND_FUNC_SRC1_ALPHA: return NVN_BLEND_FUNC_ZERO;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return NVN_BLEND_FUNC_ONE;
        default: return f;
    }
}
static int blend_is_src1(int f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
static blend_st adv_blend(blend_st b);
// The colour attachment that takes the shader's output 0 blends with a SRC1 factor: the pipeline reads the shader's output 1 as its second source (dual-source blending)
static int pso_reads_src1(const pso_key *k, int merged) {
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i] || (merged ? k->lmap[i] : i) != 0 || !(k->blend_en & 1)) continue;
        blend_st b = adv_blend(k->blend[0]);
        if (blend_is_src1(b.src_rgb) || blend_is_src1(b.dst_rgb) || blend_is_src1(b.src_a) || blend_is_src1(b.dst_a)) return 1;
    }
    return 0;
}
// An advanced blend mode (blend_st.adv, hle_nvn.c) replaces the blend functions. The Porter-Duff modes with uncorrelated overlap are sums of the source and destination weighted by
// 1, 0, As, Ad and their complements (NV_blend_equation_advanced, with premultiplied colours), which the fixed-function factors give exactly; the result goes through the ADD equation.
// A source that is not premultiplied is multiplied by As first, which only a source factor of ONE can take (SRC_ALPHA). The other modes (multiply, screen, overlay, HSL, ...) need
// the destination colour in the shader and keep the state's own functions, with a red line
static blend_st adv_blend(blend_st b) {
    if (!b.adv) return b;
    enum { Z = NVN_BLEND_FUNC_ZERO, O = NVN_BLEND_FUNC_ONE, SA = NVN_BLEND_FUNC_SRC_ALPHA, ISA = NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA, DA = NVN_BLEND_FUNC_DST_ALPHA, IDA = NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA };
    // {source factor, destination factor} for colour, then for alpha
    static const uint8_t pd[][4] = {
        [NVN_BLEND_ADVANCED_MODE_ZERO] = {Z, Z, Z, Z},        [NVN_BLEND_ADVANCED_MODE_SRC] = {O, Z, O, Z},           [NVN_BLEND_ADVANCED_MODE_DST] = {Z, O, Z, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_OVER] = {O, ISA, O, ISA}, [NVN_BLEND_ADVANCED_MODE_DST_OVER] = {IDA, O, IDA, O},
        [NVN_BLEND_ADVANCED_MODE_SRC_IN] = {DA, Z, DA, Z},     [NVN_BLEND_ADVANCED_MODE_DST_IN] = {Z, SA, Z, SA},
        [NVN_BLEND_ADVANCED_MODE_SRC_OUT] = {IDA, Z, IDA, Z},  [NVN_BLEND_ADVANCED_MODE_DST_OUT] = {Z, ISA, Z, ISA},
        [NVN_BLEND_ADVANCED_MODE_SRC_ATOP] = {DA, ISA, Z, O},  [NVN_BLEND_ADVANCED_MODE_DST_ATOP] = {IDA, SA, O, Z},
        [NVN_BLEND_ADVANCED_MODE_XOR] = {IDA, ISA, IDA, ISA},  [NVN_BLEND_ADVANCED_MODE_PLUS] = {O, O, O, O},
        [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED] = {O, O, O, O}, [NVN_BLEND_ADVANCED_MODE_PLUS_CLAMPED_ALPHA] = {O, O, O, O},
    };
    const uint8_t *f = b.adv < sizeof pd / sizeof *pd && pd[b.adv][0] ? pd[b.adv] : NULL;
    if (!f || b.adv_ovl || (b.adv_npm && f[0] != O && f[0] != Z)) {
        static uint64_t seen; if (!(seen >> (b.adv & 63) & 1)) { seen |= 1ull << (b.adv & 63);
            LOG("[RED] advanced blend mode %#x (overlap %d, %spremultiplied source) has no fixed-function equivalent: the state's blend functions are used", (unsigned)b.adv, (int)b.adv_ovl, b.adv_npm ? "not " : ""); }
        return b;
    }
    b.src_rgb = b.adv_npm && f[0] == O ? SA : f[0]; b.dst_rgb = f[1]; b.src_a = f[2]; b.dst_a = f[3];
    b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD;
    return b;
}
static MTLBlendOperation bop(int e) { switch (e) { case 2: return MTLBlendOperationSubtract; case 3: return MTLBlendOperationReverseSubtract; case 4: return MTLBlendOperationMin; case 5: return MTLBlendOperationMax; } return MTLBlendOperationAdd; }

// Pipeline: cached by (program, target formats, blend / write mask, vertex layout, sample count)
static const uint8_t *g_pso_lmap; static id<MTLFunction> g_pso_fsfn; static pso_fz_desc g_pso_fz;
static os_unfair_lock g_bf_fn_lock = OS_UNFAIR_LOCK_INIT;   // bf_fn: a shader's bflib and bffn (the submit thread and the warm-up both ask)
static nvn_hmap g_pso = {.klen = sizeof(pso_key)}, g_dss;
static nvn_hmap g_pso_fail = {.klen = sizeof(pso_key)};   // pipelines that failed to build: remembered and never retried (previously every draw recompiled it, over 10,000 times in one session)
static const char g_amp_mark, g_discard_mark;
#define AMP_MARK ((const void *)&g_amp_mark)   // pso_key.fsfn of a slice-merged pass's pipeline (vertex amplification, sm_amp_get)
#define DISCARD_MARK ((const void *)&g_discard_mark)   // vertex work still runs, without rasterization or a fragment stage
static const char g_fsp_mark;
#define FSP_MARK ((const void *)&g_fsp_mark)   // pso_key.fsfn of a face-split draw's pipeline: one colour output, amplified, its output where the encoder's colour map sends it (fsp_plan)
static int pf_int_kind(MTLPixelFormat f);
static const char *pso_fout_hint(NvShader *fs, const uint32_t cfmt[8], char *b, size_t n);
// The class each colour target of this pipeline takes (bit n: target n unsigned, bit 8 + n: signed; a float or normalized one neither), over the outputs main0 declares, into *cls, and
// whether one of them is not the class main0 declares it as: then the fragment takes the typed-output entry point specialized for *cls (typed_fn). The hardware writes an output's bits to
// its target whatever the shader computed, so the pipeline's formats say what the outputs are: Luigi's Mansion 3's G-buffer is all integer (R32Uint, RGBA8Uint), Pokémon Sword's has colour
// 1 RGBA16Uint between two float targets, Kirby and the Forgotten Land's colour 5 is R8Uint beside float ones (GitHub issue #2760). Not for a merged pass (its fragment is the
// readable-tile version, bf_fn)
static int pso_cls(const pso_key *k, NvShader *fs, uint32_t *cls) {
    int need = 0; *cls = 0;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        int kind = pf_int_kind((MTLPixelFormat)k->cfmt[i]), decl;
        if (fs->rf.fcols_known) {   // what main0 declares it as; an output it does not declare takes no constant
            decl = fs->rf.ucols >> i & 1 ? 1 : fs->rf.scols >> i & 1 ? 2 : fs->rf.fcols >> i & 1 ? 0 : -1;
            if (decl < 0) continue;
        } else { if (fs->rf.color_known && !(fs->rf.color_mask >> i & 1)) continue; decl = 0; }   // a reflection without the classes: every output a float, as the translator declares them
        if (kind == 1) *cls |= 1u << i; else if (kind == 2) *cls |= 1u << (8 + i);
        need |= kind != decl;
    }
    return need;
}
// name of lib specialized for the target classes cls (pso_cls: DMSH_UINT_FC + n and DMSH_SINT_FC + n, dmsh_entry.py fs_uint), the alpha test (at: DMSH_TYPED_ALPHA_FC) and the logic
// operation (lo: DMSH_TYPED_LOGIC_FC) on or off, and attachment n read where bit n of targets is set (DMSH_LOGIC_FC + n: dmsh_logic.py); every constant set, those the function does
// not declare left aside by Metal: the typed-output entry point's (typed_fn) and fn's logic operation entry point's (lo_fn). A function object of its own per set
static id<MTLFunction> side_spec(id<MTLLibrary> lib, NSString *name, uint32_t cls, int at, int lo, uint32_t targets, NSError **e) {
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; bool a = at, l = lo;
    for (int n = 0; n < 8; n++) { bool u = cls >> n & 1, si = cls >> (8 + n) & 1, on = targets >> n & 1;
        [cv setConstantValue:&u type:MTLDataTypeBool atIndex:DMSH_UINT_FC + n]; [cv setConstantValue:&si type:MTLDataTypeBool atIndex:DMSH_SINT_FC + n]; [cv setConstantValue:&on type:MTLDataTypeBool atIndex:DMSH_LOGIC_FC + n]; }
    [cv setConstantValue:&a type:MTLDataTypeBool atIndex:DMSH_TYPED_ALPHA_FC]; [cv setConstantValue:&l type:MTLDataTypeBool atIndex:DMSH_TYPED_LOGIC_FC];
    return fn_lib_spec(lib, name, cv, e);
}
// The side library <key>_bx (dmsh_side.py: the alpha test and logic operation entry points and the typed-output one), loaded once on first need; nil when it is not there or failed
static os_unfair_lock g_bx_lock = OS_UNFAIR_LOCK_INIT;
static id<MTLLibrary> bx_lib(NvShader *fs, NSError **e) {
    os_unfair_lock_lock(&g_bx_lock);
    if (!fs->xlib_state) {
        size_t kl = strlen(fs->key); NSString *path = [NSString stringWithFormat:@"%@/%.*s_bx.metallib", g_msl_dir, (int)(kl > 2 ? kl - 2 : kl), fs->key];
        fs->xlib = lib_from_file(path, e); fs->xlib_state = fs->xlib ? 1 : 2;
    }
    id<MTLLibrary> lib = fs->xlib; os_unfair_lock_unlock(&g_bx_lock);
    return lib;
}
static int fn_args_match(NvShader *fs, id<MTLFunction> f) {   // a side entry point binds the base's argument buffer as the base encodes it
    return !fs->rf.nb || [f newArgumentEncoderWithBufferIndex:fs->rf.abi].encodedLength == fs->arg_len;
}
static os_unfair_lock g_u_lock = OS_UNFAIR_LOCK_INIT;
// The typed-output entry point specialized for the pipeline's target classes (cls, pso_cls), its alpha test (at) and logic operation (lo, reading the attachments of targets) on or off,
// made on first need per set and kept; nil when the shader has none or it failed (once a set, with a red line). Its library is the side library when the build put it there (u_side),
// loaded with the first set. The lock covers the cache only: two threads may both specialize a set, and the first one stored is kept
static id<MTLFunction> typed_fn(NvShader *fs, uint32_t cls, int at, int lo, uint32_t targets) {
    if (!fs->u_name || (at && !fs->u_alpha) || (lo && !fs->u_logic)) return nil;
    NSNumber *key = @((uint64_t)cls | (uint64_t)!!at << 16 | (uint64_t)!!lo << 17 | (uint64_t)(lo ? targets & 0xff : 0) << 24);
    os_unfair_lock_lock(&g_u_lock); id f = fs->ufn[key]; os_unfair_lock_unlock(&g_u_lock);
    if (!f) {
        NSError *e = nil; const char *why = NULL;
        if (!fs->ulib_state) { id<MTLLibrary> lib = bx_lib(fs, &e); os_unfair_lock_lock(&g_u_lock); int first = !fs->ulib_state; if (first) { fs->ulib = lib; fs->ulib_state = lib ? 1 : 2; } os_unfair_lock_unlock(&g_u_lock);
            static _Atomic int n; int k = first && lib ? atomic_fetch_add(&n, 1) + 1 : 0; if (k && (k <= 3 || !(k & (k - 1)))) MLOG("typed outputs: side library of %s loaded for target classes 0x%x (%d shaders so far)", fs->key, cls, k); }
        if (!fs->ulib) why = "no side library";
        else if (!(f = side_spec(fs->ulib, fs->u_name, cls, at, lo, lo ? targets : 0, &e))) why = "it did not specialize";
        else if (!fn_args_match(fs, f)) why = "its argument buffer differs from the base library's";
        if (why) { MLOG("[RED] shader %s: its typed-output entry point is not usable for target classes 0x%x, alpha test %d, logic operation %d (%s%s%s)", fs->key, cls, at, lo, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); f = NSNull.null; }
        os_unfair_lock_lock(&g_u_lock); if (!fs->ufn) fs->ufn = [NSMutableDictionary dictionary]; if (fs->ufn[key]) f = fs->ufn[key]; else fs->ufn[key] = f; os_unfair_lock_unlock(&g_u_lock);
    }
    return f == NSNull.null ? nil : f;
}
// NVN's alpha test (ColorStateSetAlphaTest and SetAlphaRef; Maxwell's fixed-function test of the alpha a pixel shader writes to colour 0, before blending): Metal has none.
// A pipeline drawn with an alpha function other than ALWAYS takes its fragment from the shader's side library <key>_bx (dmsh_alpha.py: <entry>_a runs the body and compares
// colour 0's alpha with the function and reference at fragment buffer DMSH_ALPHA_BUFFER), and its draws bind them there (at_bind). The function and the
// reference are data, not part of the pipeline: one pipeline serves every function, and a draw that changes the reference every frame (a dissolve) keeps it. As in OpenGL,
// there is no test when colour 0 goes to an integer target
static inline int atest_on(void) { return S.color.alpha_func && S.color.alpha_func != NVN_ALPHA_FUNC_ALWAYS; }
static uint32_t atest_key(NvShader *fs, const uint32_t cfmt[8], const uint8_t *lmap) {   // pso_key.atest; lmap: a merged pass's physical → logical targets
    if (!fs || !atest_on()) return 0;
    // Comparisons consume SPH output-0 alpha (word 18 bit 3), not an MSL float4's padding. As in Ryujinx EmitterContext.PrepareForReturn, NEVER is unconditional.
    if (S.color.alpha_func != NVN_ALPHA_FUNC_NEVER && fs->rf.omap_known && !(fs->rf.omap & 8)) return 0;
    for (int i = 0; i < 8; i++) if (cfmt[i] && (lmap ? lmap[i] : i) == 0 && pf_int_kind((MTLPixelFormat)cfmt[i])) return 0;
    if (!fs->has_alpha) { RED_ONCE(128, "shader %s draws with alpha function %u but has no alpha test entry point (its colour 0 has no float alpha): drawn without the test", fs->key, S.color.alpha_func); return 0; }
    return 1;
}
static os_unfair_lock g_at_lock = OS_UNFAIR_LOCK_INIT;
// The side library's alpha test entry point of base: fn's or fn_d's (a pipeline that takes the typed-output entry point has the test in it: typed_fn). The library is loaded on first
// need, and a failure there is final for the shader
static id<MTLFunction> at_fn(NvShader *fs, id<MTLFunction> base) {
    int w = !base ? -1 : base == fs->fn ? 0 : base == fs->fn_d ? 1 : -1;
    if (w < 0 || !fs->has_alpha) return nil;
    os_unfair_lock_lock(&g_at_lock);
    NSError *e = nil;
    if (!fs->afn_state) {
        fs->afn_state = 2;
        id<MTLLibrary> lib = bx_lib(fs, &e); const char *why = lib ? NULL : "no library";
        id<MTLFunction> own[2] = {fs->fn, fs->fn_d};
        for (int i = 0; i < 2 && !why; i++) {
            if (!own[i]) continue;
            NSString *name = [own[i].name stringByAppendingString:@"_a"]; id<MTLFunction> f = fn4_note([lib newFunctionWithName:name], lib, name, nil);
            if (f && f.functionConstantsDictionary.count) f = fn_spec(lib, name, [MTLFunctionConstantValues new], &e);   // as shader_load
            if (!f) why = "no entry point";
            else if (!fn_args_match(fs, f)) why = "its argument buffer differs from the base library's";
            else fs->afn[i] = f;
        }
        if (!why) { fs->afn_state = 1; fs->alib = lib; static int n; if (++n <= 3 || !(n & (n - 1))) MLOG("alpha test: side library of %s loaded (%d shaders so far)", fs->key, n); }
        else { static int n; if (n++ < 8) MLOG("[RED] alpha test entry points of %s not usable (%s%s%s): its alpha-tested draws go without the test", fs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); }
    }
    id f = fs->afn_state == 1 ? fs->afn[w] : nil;
    os_unfair_lock_unlock(&g_at_lock);
    return f;
}
// The function and reference (dmsh_alpha.py dm_alpha_state) at fragment buffer DMSH_ALPHA_BUFFER: a copy in the frame arena per value and pass, since the pass's earlier draws
// still read the one before
static void at_bind(id<MTL4RenderCommandEncoder> en) {
    static uint64_t gpu, pass; static uint32_t last[2];
    uint32_t v[2] = {S.color.alpha_func, 0}; memcpy(&v[1], &S.alpha_ref, 4);
    if (!gpu || pass != g_pass.serial || memcmp(v, last, sizeof v)) {
        __strong id<MTLBuffer> b; size_t off = arena_alloc(sizeof v, &b); memcpy((uint8_t *)b.contents + off, v, sizeof v);
        gpu = b.gpuAddress + off; pass = g_pass.serial; memcpy(last, v, sizeof v);
    }
    enc_r_args(en, MTLRenderStageFragment, gpu, DMSH_ALPHA_BUFFER);
}
// NVIDIA's float blender takes a term whose weight is 0 as 0 whatever the colour ("zero times anything is zero": SET_BLEND_FLOAT_OPTION, method 0x670 of the 3D engine, which NVN's
// driver sets in its start-up command list, the word 0x80010670 in a game's sdk module); Apple's multiplies as IEEE does, so a NaN or Inf colour a game writes with a zero weight
// (Ocarina of Time's mist: NaN at alpha 0 under SRC_ALPHA) reaches the target and spreads from there. Metal has no such option, so a pixel shader zeroes a non-finite source colour
// itself where its weight is zero (dmsh_rewrite.py blend_zero), and which outputs have such a weight comes from here: DMSH_BLEND_ZERO_BITS bits per colour output, for a floating-point
// target that blends by a sum or difference with a source colour factor of SRC_ALPHA (1: zero where alpha is 0) or ONE_MINUS_SRC_ALPHA (2: where alpha is 1). A factor of ZERO needs
// nothing: Apple's blender leaves that term out, NaN or not (dm/gfx/tests/t_blend_zero.m measures each case). An output whose destination factor reads the
// source colour keeps 0: zeroing the source would also change that term, where NVIDIA keeps the NaN. With blending off, MIN / MAX (no weights) and every other factor the bits are 0
// and the shader returns its colour as it is. DM_NVN_BLEND_ZERO=0 gives 0 for every draw (Apple's blender as it is), for A/B runs
static int bz_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_BLEND_ZERO"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_BLEND_ZERO=0: a non-finite colour with a zero blend weight is blended as Apple's blender does (NaN kept)"); } return v; }
// lm: the draw's map from physical targets to the shader's outputs (physical target i holds output lm[i]; 0xff none), NULL for target i = output i, as pso_desc takes the pipeline's
static uint32_t bz_word_for(const uint32_t cfmt[8], const uint8_t *lm, uint32_t blend_en, const blend_st blend[8]) {
    if (!blend_en || !bz_on()) return 0;
    uint32_t w = 0;
    for (int i = 0; i < 8; i++) {
        unsigned l = lm ? lm[i] : (unsigned)i;
        if (l >= 8 || !cfmt[i] || !(blend_en >> l & 1) || !pf_is_float((MTLPixelFormat)cfmt[i])) continue;
        blend_st b = adv_blend(blend[l]);
        if (b.eq_rgb == NVN_BLEND_EQUATION_MIN || b.eq_rgb == NVN_BLEND_EQUATION_MAX) continue;
        if (b.dst_rgb == NVN_BLEND_FUNC_SRC_COLOR || b.dst_rgb == NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR || blend_is_src1(b.dst_rgb)) continue;
        uint32_t m = b.src_rgb == NVN_BLEND_FUNC_SRC_ALPHA ? 1 : b.src_rgb == NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA ? 2 : 0;
        w |= m << (DMSH_BLEND_ZERO_BITS * l);
    }
    return w;
}
static uint32_t bz_word(const uint8_t *lm) { return bz_word_for(g_pass.cfmt, lm, S.color.blend_en, S.blend); }
// NVIDIA's colour unit converts a float colour into a normalized target by rounding the float32 product x * (2^n - 1) to nearest even; Apple's rounds the exact product to nearest
// (dm/gfx/tests/t_rop_round.m). They differ where the colour lies within rounding of a half step: Sonic Frontiers' G-buffer flags, the float32 constant 66.5 / 255, are 66 on the Switch
// and were 67 here, which turned its ambient light off. The pixel shader rounds such outputs itself (dmsh_rewrite.py blend_zero, dm_rop), and which ones and to what width comes from
// here: DMSH_BLEND_ROUND_BITS bits per colour output, for an 8-bit, 16-bit or RGB10A2 normalized target whose stored value is the conversion of the colour: blending off for it, MIN or
// MAX on both its colour and alpha (they keep one of two stored values, so a colour rounded first keeps the same one), or a logic operation (which takes the place of blending and
// rounds the colour as the target stores it; dmsh_logic.py dm_logic_norm then finds it exact). sRGB, signed, float, packed 16-bit formats and blended sums (rounded after the blend,
// out of a shader's reach) keep Apple's conversion. DM_NVN_ROP_ROUND=0 gives 0 for every draw, for A/B runs
static int rop_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_ROP_ROUND"); v = !(e && !strcmp(e, "0")); if (!v) MLOG("DM_NVN_ROP_ROUND=0: colours go into normalized targets as Apple's GPU converts them"); } return v; }
static uint32_t rop_class(MTLPixelFormat f) {
    switch (f) {
        case MTLPixelFormatR8Unorm: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatBGRA8Unorm: return DMSH_ROUND_8;
        case MTLPixelFormatR16Unorm: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRGBA16Unorm: return DMSH_ROUND_16;
        case MTLPixelFormatRGB10A2Unorm: case MTLPixelFormatBGR10A2Unorm: return DMSH_ROUND_10_2;
        default: return 0;
    }
}
static inline int lop_on(void);
static uint32_t rop_word_for(const uint32_t cfmt[8], const uint8_t *lm, uint32_t blend_en, const blend_st blend[8], int logic) {
    if (!rop_on()) return 0;
    uint32_t w = 0;
    for (int i = 0; i < 8; i++) {
        unsigned l = lm ? lm[i] : (unsigned)i;
        if (l >= 8 || !cfmt[i]) continue;
        uint32_t c = rop_class((MTLPixelFormat)cfmt[i]); if (!c) continue;
        if (!logic && blend_en >> l & 1) {
            blend_st b = adv_blend(blend[l]);
            int mm = (b.eq_rgb == NVN_BLEND_EQUATION_MIN || b.eq_rgb == NVN_BLEND_EQUATION_MAX) && (b.eq_a == NVN_BLEND_EQUATION_MIN || b.eq_a == NVN_BLEND_EQUATION_MAX);
            if (!mm || b.adv) continue;
        }
        w |= c << (DMSH_BLEND_ROUND_BITS * l);
    }
    return w;
}
static uint32_t rop_word(const uint8_t *lm) { return rop_word_for(g_pass.cfmt, lm, S.color.blend_en, S.blend, lop_on()); }   // lm as bz_word's
// The two words at fragment buffer DMSH_BLEND_BUFFER (zero, round), for every draw of a shader that reads them: a copy in the frame arena per value and pass, as at_bind.
// lm is the draw's map, the one its pipeline was made with (draw_ passes its copy: it clears g_pso_lmap once it has the pipeline)
static uint32_t g_bz_last[2];   // the words bz_bind bound last (t_blend_zero, t_rop_round)
static void bz_bind(id<MTL4RenderCommandEncoder> en, const uint8_t *lm) {
    static uint64_t gpu, pass;
    uint32_t v[2] = {bz_word(lm), rop_word(lm)};
    if (!gpu || pass != g_pass.serial || memcmp(v, g_bz_last, sizeof v)) {
        __strong id<MTLBuffer> b; size_t off = arena_alloc(16, &b); memset((uint8_t *)b.contents + off, 0, 16); memcpy((uint8_t *)b.contents + off, v, sizeof v);
        gpu = b.gpuAddress + off; pass = g_pass.serial; memcpy(g_bz_last, v, sizeof v);
        if (v[0]) { static uint64_t n; if (++n <= 3 || !(n & (n - 1))) MLOG("blend: a zero source weight on a floating-point target (outputs %#x): a non-finite colour is blended as 0 there, as NVIDIA's blender does (%llu passes or states so far)", v[0], (unsigned long long)n); }
    }
    enc_r_args(en, MTLRenderStageFragment, gpu, DMSH_BLEND_BUFFER);
}
// NVN's logic operations (ColorStateSetLogicOp: the bits a pixel shader writes combined with the target's, in place of blending; integer and normalized targets, as in OpenGL and Vulkan): Metal has none.
// A pipeline drawn with an operation other than COPY takes the typed-output entry point with its operation on (typed_fn), or in a shader without one fn's entry point from the shader's side library <key>_bx, instead
// (dmsh_logic.py: <entry>_l runs the body, reads each output's attachment and combines the two), and its draws bind the operation and the targets' channel masks at fragment buffer
// DMSH_LOGIC_BUFFER (lo_bind). Xenoblade Chronicles 2 ORs probe indices into its RG32Uint light probe lists; Skyward Sword HD ORs fog into the alpha of its RGBA8Unorm frame. A target
// with no bits to combine (floating point, sRGB, signed) keeps COPY, with a red line. The operation takes the place of blending (blending stays off in pso_desc), as both APIs define
static inline int lop_on(void) { return S.color.logic_op != NVN_LOGIC_OP_COPY; }
static int unorm_chan_masks(MTLPixelFormat f, uint32_t m[4]) {   // the bits each channel of a normalized colour format stores (alpha of a format without it: any, Metal drops the write); 0: not one
    uint32_t b = 0;
    switch (f) {
        case MTLPixelFormatR8Unorm: case MTLPixelFormatRG8Unorm: case MTLPixelFormatRGBA8Unorm: case MTLPixelFormatBGRA8Unorm: b = 0xffu; break;
        case MTLPixelFormatR16Unorm: case MTLPixelFormatRG16Unorm: case MTLPixelFormatRGBA16Unorm: b = 0xffffu; break;
        case MTLPixelFormatRGB10A2Unorm: m[0] = m[1] = m[2] = 0x3ffu; m[3] = 3u; return 1;
        case MTLPixelFormatB5G6R5Unorm: m[0] = m[2] = 0x1fu; m[1] = 0x3fu; m[3] = 0xffu; return 1;
        case MTLPixelFormatBGR5A1Unorm: m[0] = m[1] = m[2] = 0x1fu; m[3] = 1u; return 1;
        default: return 0;
    }
    for (int c = 0; c < 4; c++) m[c] = b;
    return 1;
}
static uint32_t lop_key(const pso_key *k, NvShader *fs, int merged) {   // pso_key.logic, after the formats and the alpha test are in k
    if (!fs || !lop_on()) return 0;
    int any = 0, norm = 0, other = 0;
    for (int i = 0; i < 8; i++) if (k->cfmt[i] && (!fs->rf.color_known || (fs->rf.color_mask >> i & 1)) && (k->chmask >> (4 * i) & 0xf)) {
        uint32_t bits[4]; any = 1;
        if (unorm_chan_masks((MTLPixelFormat)k->cfmt[i], bits)) norm = 1; else if (pf_int_kind((MTLPixelFormat)k->cfmt[i])) other = 1;
    }
    if (!any) return 0;   // no colour written (Xenoblade Chronicles 2's probe boxes mark stencil with the operation still set): nothing to combine
    if (merged) { RED_ONCE(134, "logic operation %u in a merged pass (shader %s): drawn with COPY", S.color.logic_op, fs->key); return 0; }
    int u = 0; if (fs->u_logic) for (int i = 0; i < 8; i++) u |= k->cfmt[i] && pf_int_kind((MTLPixelFormat)k->cfmt[i]) == 1;   // an unsigned target the typed-output entry point serves
    if (!u && (other || !norm)) { RED_ONCE(133, "shader %s draws with logic operation %u into a target with no bits to combine (floating point, sRGB, signed) or an integer target it has no entry point for: drawn with COPY", fs->key, S.color.logic_op); return 0; }
    if (k->atest && !(fs->u_logic && fs->u_alpha)) { RED_ONCE(142, "shader %s draws with logic operation %u and an alpha test, which no library combines: drawn with COPY", fs->key, S.color.logic_op); return 0; }   // the typed-output entry point holds both
    if (!fs->has_logic && !fs->u_logic) { RED_ONCE(135, "shader %s draws with logic operation %u but has no logic operation entry point: drawn with COPY", fs->key, S.color.logic_op); return 0; }
    return 1;
}
static os_unfair_lock g_lo_lock = OS_UNFAIR_LOCK_INIT;
// The side library's logic operation entry point of fn, in a shader without the typed-output entry point (which holds its own: typed_fn), specialized for the targets the pipeline has
// (targets: bit n = colour target n attached; dmsh_logic.py reads only those, DMSH_LOGIC_FC + n), loaded on first need; a library that fails is final for the shader. The dual-source
// entry point has none (a logic operation takes the place of blending)
static id<MTLFunction> lo_fn(NvShader *fs, id<MTLFunction> base, uint32_t targets) {
    NSString *name = base && base == fs->fn ? [fs->fn.name stringByAppendingString:@"_l"] : nil;
    if (!name || !fs->has_logic) return nil;
    os_unfair_lock_lock(&g_lo_lock);
    NSError *e = nil; const char *why = NULL;
    if (!fs->lfn_state) {
        fs->lfn_state = 2;
        if ((fs->llib = bx_lib(fs, &e))) { fs->lfn_state = 1; fs->lfn = [NSMutableDictionary dictionary]; static int n; if (++n <= 3 || !(n & (n - 1))) MLOG("logic operation: side library of %s loaded (%d shaders so far)", fs->key, n); }
        else why = "no library";
    }
    id<MTLFunction> f = nil; NSNumber *k = @(targets & 0xff);
    if (fs->lfn_state == 1 && !(f = fs->lfn[k])) {
        f = side_spec(fs->llib, name, 0, 0, 1, targets, &e);
        if (!f) why = "no entry point";
        else if (!fn_args_match(fs, f)) { why = "its argument buffer differs from the base library's"; f = nil; }
        if (f) fs->lfn[k] = f; else fs->lfn_state = 2;
    }
    if (why) { static int n; if (n++ < 8) MLOG("[RED] logic operation entry points of %s not usable (%s%s%s): its draws go with COPY", fs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); }
    os_unfair_lock_unlock(&g_lo_lock);
    return f;
}
static void uint_chan_masks(MTLPixelFormat f, uint32_t m[4]) {   // the bits each channel of an unsigned integer format holds: the operation's result is cut to them, as the hardware writes them
    uint32_t b = f == MTLPixelFormatR8Uint || f == MTLPixelFormatRG8Uint || f == MTLPixelFormatRGBA8Uint ? 0xffu : f == MTLPixelFormatR16Uint || f == MTLPixelFormatRG16Uint || f == MTLPixelFormatRGBA16Uint ? 0xffffu : 0xffffffffu;
    for (int c = 0; c < 4; c++) m[c] = b;
    if (f == MTLPixelFormatRGB10A2Uint) { m[0] = m[1] = m[2] = 0x3ffu; m[3] = 3u; }
}
// The operation and the channel masks (dmsh_logic.py dm_logic_state) at fragment buffer DMSH_LOGIC_BUFFER: a copy in the frame arena per value and pass, as at_bind
static void lo_bind(id<MTL4RenderCommandEncoder> en) {
    static uint64_t gpu, pass; static uint32_t last[36];
    uint32_t v[36] = {S.color.logic_op};
    for (int i = 0; i < 8; i++) if (g_pass.cfmt[i]) {   // a target with neither keeps masks of zero: its float output passes through (dmsh_logic.py dm_logic_norm)
        MTLPixelFormat f = (MTLPixelFormat)g_pass.cfmt[i];
        if (pf_int_kind(f)) uint_chan_masks(f, &v[4 + 4 * i]); else unorm_chan_masks(f, &v[4 + 4 * i]);
    }
    if (!gpu || pass != g_pass.serial || memcmp(v, last, sizeof v)) {
        __strong id<MTLBuffer> b; size_t off = arena_alloc(sizeof v, &b); memcpy((uint8_t *)b.contents + off, v, sizeof v);
        gpu = b.gpuAddress + off; pass = g_pass.serial; memcpy(last, v, sizeof v);
    }
    enc_r_args(en, MTLRenderStageFragment, gpu, DMSH_LOGIC_BUFFER);
}
#define PSO_WHOLE @"built whole"   // pso_desc's label for pso_build_ex: no base, see there
#define PSO_FACE @"built whole, colour map inherited"   // a face-split draw's pipeline (FSP_MARK): see pso_build_ex
static MTLRenderPipelineDescriptor *pso_desc(const pso_key *k, NvShader *vs, NvShader *fs, id<MTLFunction> fsfn, int merged) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs && vs->mesh_kind != 2 ? vs->fn : nil; d.fragmentFunction = fsfn ? fsfn : fs ? fs->fn : nil;   // mtk_pso copies the raster state only: a mesh function is not a vertex function
    // Diagnostic DM_NVN_HILITE=fragment shader key,...: these draws use a solid-color fragment instead (keys 1–6 in order magenta / green / cyan / yellow / blue / white), to see where the geometry lands
    // Pipelines are built on several threads at once: the keys and the library are set up once (a lazy setup raced, and a thread used a library another had just replaced: Hades II, 6 keys)
    if (fs) { static char keys[6][24]; static int nk; static id<MTLLibrary> hl; static dispatch_once_t once;
        dispatch_once(&once, ^{ const char *e = getenv("DM_NVN_HILITE");
            for (const char *q = e; q && *q && nk < 6; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) { size_t n = strcspn(q, ","); if (n > 23) n = 23; memcpy(keys[nk], q, n); keys[nk++][n] = 0; }
            if (!nk) return;
            NSError *err = nil; hl = dev_lib(@"#include <metal_stdlib>\nusing namespace metal;\nstruct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };\n"
                "fragment O hl0() { return {float4(1,0,1,1),float4(1,0,1,1),float4(1,0,1,1)}; } fragment O hl1() { return {float4(0,1,0,1),float4(0,1,0,1),float4(0,1,0,1)}; }\n"
                "fragment O hl2() { return {float4(0,1,1,1),float4(0,1,1,1),float4(0,1,1,1)}; } fragment O hl3() { return {float4(1,1,0,1),float4(1,1,0,1),float4(1,1,0,1)}; }\n"
                "fragment O hl4() { return {float4(0,0,1,1),float4(0,0,1,1),float4(0,0,1,1)}; } fragment O hl5() { return {float4(1,1,1,1),float4(1,1,1,1),float4(1,1,1,1)}; }\n", &err);
            if (!hl) MLOG("[RED] HILITE shader compile failed %s", err.localizedDescription.UTF8String); });
        for (int i = 0; i < nk; i++) if (!strncmp(fs->key, keys[i], strlen(keys[i]))) {
            if (hl) d.fragmentFunction = [hl newFunctionWithName:[NSString stringWithFormat:@"hl%d", i]];
            break; } }
    // Colour targets of another class than main0 declares (pso_cls): the typed-output entry point specialized for the pipeline's classes (dmsh_entry.py fs_uint). A float output meeting an
    // integer attachment, or the reverse: a pipeline built on its own is refused (Kirby and the Forgotten Land's 76 shaders writing an R8Uint colour 5, GitHub issue #2760: most of the
    // scene missing), one from a Metal 4 base is not, and the target takes undefined values with no error (Kirby Star Allies' material target; DRAGON BALL: Sparking!
    // ZERO's virtual texture page table, which held 255 everywhere and left the landscape flat green). It also holds the logic operation (lop_key), so a pipeline with one takes it whatever
    // its targets, and the alpha test (atest_key), which a pipeline that takes it for either runs there; a pipeline of the declared classes with an alpha test only keeps main0's alpha
    // test entry point, which needs no specialization (at_fn)
    uint32_t targets = 0; for (int i = 0; i < 8; i++) if (k->cfmt[i]) targets |= 1u << i;   // not a merged pass: target i is output i
    uint32_t cls = 0; int typed = 0, typed_at = 0;
    if (fs && !fsfn && d.fragmentFunction == fs->fn) {
        int need = pso_cls(k, fs, &cls), lo = k->logic && fs->u_logic, at = k->atest && fs->u_alpha && (need || lo);
        id<MTLFunction> tf = need || lo ? typed_fn(fs, cls, at, lo, targets) : nil;
        if (tf) { d.fragmentFunction = tf; typed = 1; typed_at = at; }
        else if (!need) {}
        else if (fs->u_name) RED_ONCE(152, "shader %s: a colour target of this pipeline (classes 0x%x) is not of the class main0 declares, and its typed-output entry point did not specialize "
                                      "for it (the line above): the target takes undefined values", fs->key, cls);
        else { char hint[200]; RED_ONCE(152, "shader %s: a colour target of this pipeline (classes 0x%x) is not of the class main0 declares, and it has no typed-output entry point (dmsh_entry.py "
                                        "fs_uint: main0 of a shape it does not know): the target takes undefined values%s", fs->key, cls, pso_fout_hint(fs, k->cfmt, hint, sizeof hint)); }
    }
    // Dual-source blending: target 0 reads the shader's output 1 as its second source. The dual-source entry point returns that output as [[color(0), index(1)]] (dmsh_entry.py fs_dual),
    // and output 1 is then no colour output: Maxwell leaves the attachment of output 1 as it was. A shader without that entry point blends with zero for the second source, as an
    // unwritten one (blend_zero_src1): passing the state to Metal as it is makes Metal 4's blend compiler crash (MTLCompilerService in buildBlendFactor, Pokémon Legends: Arceus)
    int dual = !k->logic && fs && !fsfn && fs->rf.color_known && (fs->rf.color_mask & 2) && d.fragmentFunction == fs->fn && pso_reads_src1(k, merged), dual_fn = 0;   // a logic operation takes the place of blending
    if (dual && fs->fn_d) { d.fragmentFunction = fs->fn_d; dual_fn = 1; }
    else if (dual) { want_note(&g_dual_want, fs->key);
        RED_ONCE(26, "shader %s writes output 1 and a pipeline blends with it as the second source, but it has no dual-source entry point: the second source counts as zero "
                 "(add its key to the profile's dual_want.tsv; DM_NVN_DUAL_WANT=<file> lists every such shader)", fs->key); }
    if (k->atest && fs && !typed_at) {   // the alpha test (atest_key): the same entry point's in the side library
        id<MTLFunction> af = fsfn ? nil : at_fn(fs, d.fragmentFunction);
        if (af) d.fragmentFunction = af;
        else if (fsfn) RED_ONCE(129, "alpha test not applied to a specialized fragment (readable-tile merge or diagnostic) of %s", fs->key);
    }
    if (k->logic && fs && !typed) {   // the logic operation (lop_key) of a shader without the typed-output entry point: main0's in the side library; never with the alpha test (lop_key)
        id<MTLFunction> lf = fsfn ? nil : lo_fn(fs, d.fragmentFunction, targets);
        if (lf) d.fragmentFunction = lf;
        else if (fsfn) RED_ONCE(136, "logic operation not applied to a specialized fragment (readable-tile merge or diagnostic) of %s", fs->key);
    }
    if (vs && k->fsfn == DISCARD_MARK) { d.fragmentFunction = nil; d.rasterizationEnabled = NO; }
    d.rasterSampleCount = k->samples ? k->samples : 1;
    d.alphaToCoverageEnabled = k->a2c;
    if (fs && fs->rf.reads_prim_id) d.label = PSO_WHOLE;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[i];
        c.pixelFormat = (MTLPixelFormat)k->cfmt[i];
        int l = merged ? k->lmap[i] : i;
        uint32_t m = l == 0xff ? 0 : (k->chmask >> (4 * l)) & 0xf;
        // Output numbers the fragment shader does not write: zero write mask, no blending. Maxwell writes only the targets in the shader's output map and leaves the others untouched (whatever the blend state and channel mask);
        // on Metal an attachment the shader does not write has undefined contents, and with blending enabled that undefined value is blended in. Super Mario 3D World + Bowser's Fury's volume draws after the G-buffer (50d813761ca3f1f9 and others) write only outputs 0 and 2,
        // while targets 1 (normals) and 3 (pre-lighting) have SRC_ALPHA blending and all channels enabled
        int unwritten = fs && fs->rf.color_known && !fsfn && l != 0xff && (!(fs->rf.color_mask >> l & 1) || (dual_fn && l == 1));
        if (unwritten) m = 0;
        // Components of a written output outside the shader's output map (SPH word 18): the GPU leaves them as they were; Metal writes the whole attachment, those components undefined
        // (the translator stores only the mapped ones). Lichii traced a NaN band above Ocarina of Time's horizon to this; its agl_common depth pass maps output 2's R only
        else if (fs && fs->rf.omap_known && !fsfn && l != 0xff) m &= fs->rf.omap >> (4 * l) & 0xf;
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        // Integer targets: no blending. Maxwell ignores the blend enable on them; Metal refuses the pipeline (an assertion even without the API validation layer): Luigi's Mansion 3's integer G-buffer (PR #105)
        if (!k->logic && l != 0xff && k->blend_en >> l & 1 && !unwritten && !pf_int_kind(c.pixelFormat)) {   // a logic operation takes the place of blending (lop_key)
            blend_st bl = adv_blend(k->blend[l]);
            if (fs && fs->rf.color_known && (!(fs->rf.color_mask & 2) || (dual && !dual_fn)) && !fsfn) {
                bl.src_rgb = blend_zero_src1(bl.src_rgb); bl.dst_rgb = blend_zero_src1(bl.dst_rgb);
                bl.src_a = blend_zero_src1(bl.src_a); bl.dst_a = blend_zero_src1(bl.dst_a);
            }
            // ONE + ZERO under ADD replaces the float attachment. LEGO Star Wars' bloom pass writes +Inf across an R11G11B10F target when this goes through Metal's blender.
            if (pf_is_float(c.pixelFormat) && bl.src_rgb == NVN_BLEND_FUNC_ONE && bl.dst_rgb == NVN_BLEND_FUNC_ZERO &&
                bl.src_a == NVN_BLEND_FUNC_ONE && bl.dst_a == NVN_BLEND_FUNC_ZERO &&
                bl.eq_rgb == NVN_BLEND_EQUATION_ADD && bl.eq_a == NVN_BLEND_EQUATION_ADD) continue;
            c.blendingEnabled = YES;
            c.sourceRGBBlendFactor = bfac(bl.src_rgb); c.destinationRGBBlendFactor = bfac(bl.dst_rgb);
            c.sourceAlphaBlendFactor = bfac(bl.src_a); c.destinationAlphaBlendFactor = bfac(bl.dst_a);
            c.rgbBlendOperation = bop(bl.eq_rgb); c.alphaBlendOperation = bop(bl.eq_a);
        }
    }
    d.depthAttachmentPixelFormat = (MTLPixelFormat)k->dfmt; d.stencilAttachmentPixelFormat = (MTLPixelFormat)k->sfmt;
    if (!vs) return d;   // a native mesh pipeline reuses only the fragment and attachment state
    if (k->fsfn == AMP_MARK || k->fsfn == FSP_MARK) d.maxVertexAmplificationCount = 2;   // a slice-merged pass's or a face-split draw's pipeline: the view mapping's layer offset applies (see g_sm_plan, fsp_plan)
    if (k->fsfn == FSP_MARK) d.label = PSO_FACE;
    if (k->fsfn == DISCARD_MARK) { d.vertexFunction = vs->discard_fn; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    const nvn_refl *rf = &vs->rf; int nconv = 0, on = vin_on(vs, k, k->fsfn == DISCARD_MARK);
    if (on) { d.vertexFunction = vs->vfn; g_vin_psos++; }   // the shadow-input entry point (see vin_on)
    #define ZERO_ATTR(i, ct_) do { vd.attributes[i].format = (ct_) == NVN_CT_UINT ? MTLVertexFormatUInt4 : (ct_) == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4; vd.attributes[i].offset = 0; vd.attributes[i].bufferIndex = 30; \
                                  vd.layouts[30].stride = 16; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0; } while (0)   // reads the zero buffer
    for (int v = 0; v < rf->nv; v++) {
        int loc = rf->v[v].loc, a = rf->v[v].nvn;   // Metal slot loc ← NVN attribute a
        va_st ka = a < 32 ? pso_key_attr(k, a) : (va_st){0};
        MTLVertexFormat vf = a < 32 ? vfmt(ka.fmt) : MTLVertexFormatInvalid;
        // With the shadow inputs the attribute goes to the declaration of its format's class (at) and the input's other declarations read the zero buffer
        int ct = on ? vin_ct(rf, v, vin_fmt(k, a), 1) : rf->v[v].ct, at = ct == rf->v[v].ct ? loc : rf->v[v].sh[ct];
        if (on) { if (at != loc) ZERO_ATTR(loc, rf->v[v].ct); for (int q = 0; q < 3; q++) if (rf->v[v].sh[q] != 0xff && rf->v[v].sh[q] != at) ZERO_ATTR(rf->v[v].sh[q], q); }
        // When a >= S.nattr, k.attr[a] keeps the 0 from memset, vfmt(0) is Invalid, the same fallback branch is taken and the RED_ONCE condition of fmt being non-0 does not hold.
        if (ka.fmt && vf == MTLVertexFormatInvalid) RED_ONCE(15, "vertex attribute format %#x not mapped", ka.fmt);
        if (vf == MTLVertexFormatInvalid) { ZERO_ATTR(at, ct); continue; }
        int st = ka.stream;
        if (vconv_wanted(rf, v, k, on) && nconv < VCONV_MAX) {   // converted on the CPU at draw time (vconv_bind): the register's 32-bit values, 4 components, its own slot
            int slot = VCONV_SLOT0 + nconv++;
            vd.attributes[loc].format = rf->v[v].ct == NVN_CT_UINT ? MTLVertexFormatUInt4 : rf->v[v].ct == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4; vd.attributes[loc].offset = 0; vd.attributes[loc].bufferIndex = slot;
            vd.layouts[slot].stride = 16;
            vd.layouts[slot].stepFunction = k->stream[st].stride == 0 ? MTLVertexStepFunctionConstant : k->stream[st].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
            vd.layouts[slot].stepRate = k->stream[st].stride == 0 ? 0 : k->stream[st].divisor ? k->stream[st].divisor : 1;
            continue;
        }
        vf = vfmt_for_ct(vf, ct, ka.fmt);
        vd.attributes[at].format = vf; vd.attributes[at].offset = ka.offset; vd.attributes[at].bufferIndex = VB_SLOT(st);
        vd.layouts[VB_SLOT(st)].stride = k->stream[st].stride ? k->stream[st].stride : 4;
        vd.layouts[VB_SLOT(st)].stepFunction = k->stream[st].stride == 0 ? MTLVertexStepFunctionConstant : k->stream[st].divisor ? MTLVertexStepFunctionPerInstance : MTLVertexStepFunctionPerVertex;
        vd.layouts[VB_SLOT(st)].stepRate = k->stream[st].stride == 0 ? 0 : k->stream[st].divisor ? k->stream[st].divisor : 1;
    }
    #undef ZERO_ATTR
    d.vertexDescriptor = vd;
    return d;
}
