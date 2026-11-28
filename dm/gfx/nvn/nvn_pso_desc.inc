// The pipeline description: blend, the pipeline cache key, target classes, alpha test and logic operation entry points, NVIDIA's blender and colour rounding.
// An unwritten secondary blend source is undefined in NVN: fold it as zero, since Metal rejects an absent [[color(0), index(1)]] and Metal 4's blend compiler can crash on it.
static int blend_zero_src1(int f) {
    switch (f) {
        case NVN_BLEND_FUNC_SRC1_COLOR: case NVN_BLEND_FUNC_SRC1_ALPHA: return NVN_BLEND_FUNC_ZERO;
        case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return NVN_BLEND_FUNC_ONE;
        default: return f;
    }
}
static int blend_is_src1(int f) { return f >= NVN_BLEND_FUNC_SRC1_COLOR && f <= NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA; }
static blend_st adv_blend(blend_st b);
static int pso_reads_src1(const pso_key *k, int merged) {
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i] || (merged ? k->lmap[i] : i) != 0 || !(k->blend_en & 1)) continue;
        blend_st b = adv_blend(k->blend[0]);
        if (blend_is_src1(b.src_rgb) || blend_is_src1(b.dst_rgb) || blend_is_src1(b.src_a) || blend_is_src1(b.dst_a)) return 1;
    }
    return 0;
}
// Advanced blend modes: the uncorrelated Porter-Duff ones are sums of fixed-function factors (NV_blend_equation_advanced, premultiplied) under ADD; the other modes need the
// destination in the shader and keep the state's own functions, with a red line.
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
        static uint64_t seen;
        if (!(seen >> (b.adv & 63) & 1)) {
            seen |= 1ull << (b.adv & 63);
            LOG("[RED] advanced blend mode %#x (overlap %d, %spremultiplied source) has no fixed-function equivalent: the state's blend functions are used", (unsigned)b.adv, (int)b.adv_ovl, b.adv_npm ? "not " : "");
        }
        return b;
    }
    b.src_rgb = b.adv_npm && f[0] == O ? SA : f[0]; b.dst_rgb = f[1]; b.src_a = f[2]; b.dst_a = f[3];
    b.eq_rgb = b.eq_a = NVN_BLEND_EQUATION_ADD;
    return b;
}
static MTLBlendOperation bop(int e) {
    switch (e) {
        case 2: return MTLBlendOperationSubtract;
        case 3: return MTLBlendOperationReverseSubtract;
        case 4: return MTLBlendOperationMin;
        case 5: return MTLBlendOperationMax;
    }
    return MTLBlendOperationAdd;
}

static const uint8_t *g_pso_lmap;
static id<MTLFunction> g_pso_fsfn;
static pso_fz_desc g_pso_fz;
static os_unfair_lock g_bf_fn_lock = OS_UNFAIR_LOCK_INIT;
static nvn_hmap g_pso = {.klen = sizeof(pso_key)}, g_dss;
static nvn_hmap g_pso_fail = {.klen = sizeof(pso_key)};
static const char g_amp_mark, g_discard_mark;
#define AMP_MARK ((const void *)&g_amp_mark)
#define DISCARD_MARK ((const void *)&g_discard_mark)   // vertex work still runs, without rasterization or a fragment stage
static const char g_fsp_mark;
#define FSP_MARK ((const void *)&g_fsp_mark)
static int pf_int_kind(MTLPixelFormat f);
static const char *pso_fout_hint(NvShader *fs, const uint32_t cfmt[8], char *b, size_t n);
// The class of each colour target (bit n: unsigned, bit 8 + n: signed): the hardware writes an output's bits whatever the shader computed, so the targets say what the outputs are
// (docs/reference/shaders.md#integer-outputs-told-by-the-pipelines-targets). Not for a merged pass.
static int pso_cls(const pso_key *k, NvShader *fs, uint32_t *cls) {
    int need = 0; *cls = 0;
    for (int i = 0; i < 8; i++) {
        if (!k->cfmt[i]) continue;
        int kind = pf_int_kind((MTLPixelFormat)k->cfmt[i]), decl;
        if (fs->rf.fcols_known) {
            decl = fs->rf.ucols >> i & 1 ? 1 : fs->rf.scols >> i & 1 ? 2 : fs->rf.fcols >> i & 1 ? 0 : -1;
            if (decl < 0) continue;
        } else { if (fs->rf.color_known && !(fs->rf.color_mask >> i & 1)) continue; decl = 0; }   // no classes in the reflection: every output a float, as the translator declares them
        if (kind == 1) *cls |= 1u << i; else if (kind == 2) *cls |= 1u << (8 + i);
        need |= kind != decl;
    }
    return need;
}
// Specialized with every constant set (DMSH_UINT_FC / SINT_FC / TYPED_ALPHA_FC / TYPED_LOGIC_FC / LOGIC_FC + n); Metal ignores those the function does not declare.
static id<MTLFunction> side_spec(id<MTLLibrary> lib, NSString *name, uint32_t cls, int at, int lo, uint32_t targets, NSError **e) {
    MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; bool a = at, l = lo;
    for (int n = 0; n < 8; n++) {
        bool u = cls >> n & 1, si = cls >> (8 + n) & 1, on = targets >> n & 1;
        [cv setConstantValue:&u type:MTLDataTypeBool atIndex:DMSH_UINT_FC + n];
        [cv setConstantValue:&si type:MTLDataTypeBool atIndex:DMSH_SINT_FC + n];
        [cv setConstantValue:&on type:MTLDataTypeBool atIndex:DMSH_LOGIC_FC + n];
    }
    [cv setConstantValue:&a type:MTLDataTypeBool atIndex:DMSH_TYPED_ALPHA_FC]; [cv setConstantValue:&l type:MTLDataTypeBool atIndex:DMSH_TYPED_LOGIC_FC];
    fc_set_mwa(cv, [lib newFunctionWithName:name].functionConstantsDictionary);
    return fn_lib_spec(lib, name, cv, e);
}
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
// Made on first need per set and kept; nil when the shader has none or it failed (once a set, in red).
// The lock covers the cache only: two threads may both specialize a set, and the first one stored is kept.
static id<MTLFunction> typed_fn(NvShader *fs, uint32_t cls, int at, int lo, uint32_t targets) {
    if (!fs->u_name || (at && !fs->u_alpha) || (lo && !fs->u_logic)) return nil;
    NSNumber *key = @((uint64_t)cls | (uint64_t)!!at << 16 | (uint64_t)!!lo << 17 | (uint64_t)(lo ? targets & 0xff : 0) << 24);
    os_unfair_lock_lock(&g_u_lock); id f = fs->ufn[key]; os_unfair_lock_unlock(&g_u_lock);
    if (!f) {
        NSError *e = nil; const char *why = NULL;
        if (!fs->ulib_state) {
            id<MTLLibrary> lib = bx_lib(fs, &e);
            os_unfair_lock_lock(&g_u_lock);
            int first = !fs->ulib_state;
            if (first) { fs->ulib = lib; fs->ulib_state = lib ? 1 : 2; }
            os_unfair_lock_unlock(&g_u_lock);
            static _Atomic int n;
            int k = first && lib ? atomic_fetch_add(&n, 1) + 1 : 0;
            if (k && (k <= 3 || !(k & (k - 1)))) MLOG("typed outputs: side library of %s loaded for target classes 0x%x (%d shaders so far)", fs->key, cls, k);
        }
        if (!fs->ulib) why = "no side library";
        else if (!(f = side_spec(fs->ulib, fs->u_name, cls, at, lo, lo ? targets : 0, &e))) why = "it did not specialize";
        else if (!fn_args_match(fs, f)) why = "its argument buffer differs from the base library's";
        if (why) { MLOG("[RED] shader %s: its typed-output entry point is not usable for target classes 0x%x, alpha test %d, logic operation %d (%s%s%s)", fs->key, cls, at, lo, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); f = NSNull.null; }
        os_unfair_lock_lock(&g_u_lock);
        if (!fs->ufn) fs->ufn = [NSMutableDictionary dictionary];
        if (fs->ufn[key]) f = fs->ufn[key]; else fs->ufn[key] = f;
        os_unfair_lock_unlock(&g_u_lock);
    }
    return f == NSNull.null ? nil : f;
}
// NVN's alpha test has no Metal equivalent (docs/reference/rendering.md#the-alpha-test-through-a-library-of-its-own): the side library's <entry>_a, with the function and reference
// as data at DMSH_ALPHA_BUFFER, so one pipeline serves every reference. As in OpenGL, there is no test when colour 0 goes to an integer target.
static inline int atest_on(void) { return S.color.alpha_func && S.color.alpha_func != NVN_ALPHA_FUNC_ALWAYS; }
static uint32_t atest_key(NvShader *fs, const uint32_t cfmt[8], const uint8_t *lmap) {
    if (!fs || !atest_on()) return 0;
    // SPH output-0 alpha (word 18 bit 3), not an MSL float4's padding; NEVER is unconditional (as Ryujinx's EmitterContext.PrepareForReturn)
    if (S.color.alpha_func != NVN_ALPHA_FUNC_NEVER && fs->rf.omap_known && !(fs->rf.omap & 8)) return 0;
    for (int i = 0; i < 8; i++) if (cfmt[i] && (lmap ? lmap[i] : i) == 0 && pf_int_kind((MTLPixelFormat)cfmt[i])) return 0;
    if (!fs->has_alpha) { RED_ONCE(128, "shader %s draws with alpha function %u but has no alpha test entry point (its colour 0 has no float alpha): drawn without the test", fs->key, S.color.alpha_func); return 0; }
    return 1;
}
static os_unfair_lock g_at_lock = OS_UNFAIR_LOCK_INIT;
// fn's or fn_d's; a library failure is final for the shader
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
            NSString *name = [own[i].name stringByAppendingString:@"_a"]; id<MTLFunction> f = fn_load(lib, name, &e);
            if (!f) why = "no entry point";
            else if (!fn_args_match(fs, f)) why = "its argument buffer differs from the base library's";
            else fs->afn[i] = f;
        }
        if (!why) {
            fs->afn_state = 1; fs->alib = lib;
            static int n;
            if (++n <= 3 || !(n & (n - 1))) MLOG("alpha test: side library of %s loaded (%d shaders so far)", fs->key, n);
        }
        else { static int n; if (n++ < 8) MLOG("[RED] alpha test entry points of %s not usable (%s%s%s): its alpha-tested draws go without the test", fs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); }
    }
    id f = fs->afn_state == 1 ? fs->afn[w] : nil;
    os_unfair_lock_unlock(&g_at_lock);
    return f;
}
// a copy in the frame arena per value and pass: the pass's earlier draws still read the one before
static void at_bind(id<MTL4RenderCommandEncoder> en) {
    static uint64_t gpu, pass; static uint32_t last[2];
    uint32_t v[2] = {S.color.alpha_func, 0}; memcpy(&v[1], &S.alpha_ref, 4);
    if (!gpu || pass != g_pass.serial || memcmp(v, last, sizeof v)) {
        __strong id<MTLBuffer> b; size_t off = arena_alloc(sizeof v, &b); memcpy((uint8_t *)b.contents + off, v, sizeof v);
        gpu = b.gpuAddress + off; pass = g_pass.serial; memcpy(last, v, sizeof v);
    }
    enc_r_args(en, MTLRenderStageFragment, gpu, DMSH_ALPHA_BUFFER);
}
// NVIDIA's float blender takes a zero-weight term as 0 whatever the colour (SET_BLEND_FLOAT_OPTION, set by NVN's driver); Apple's multiplies as IEEE does, so a NaN reaches the target
// (docs/reference/shaders.md#a-nan-colour-under-a-zero-blend-weight). The shader zeroes it (dmsh_rewrite.py blend_zero) per these bits; a dst factor reading the source keeps 0.
static int bz_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_BLEND_ZERO");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_BLEND_ZERO=0: a non-finite colour with a zero blend weight is blended as Apple's blender does (NaN kept)");
    }
    return v;
}
// lm: physical target i holds output lm[i] (0xff none); NULL for target i = output i
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
// NVIDIA rounds the float32 product x * (2^n - 1) to nearest even into a normalized target, Apple the exact product to nearest
// (docs/reference/rendering.md#float-colours-rounded-into-normalized-targets-as-nvidias-colour-unit-does): the shader rounds where the stored value is the colour's conversion.
static int rop_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_ROP_ROUND");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_ROP_ROUND=0: colours go into normalized targets as Apple's GPU converts them");
    }
    return v;
}
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
static uint32_t rop_word(const uint8_t *lm) { return rop_word_for(g_pass.cfmt, lm, S.color.blend_en, S.blend, lop_on()); }
// lm is the map the pipeline was made with (draw_ passes its copy: it clears g_pso_lmap once it has the pipeline)
static uint32_t g_bz_last[2];
static void bz_bind(id<MTL4RenderCommandEncoder> en, const uint8_t *lm) {
    static uint64_t gpu, pass;
    uint32_t v[2] = {bz_word(lm), rop_word(lm)};
    if (!gpu || pass != g_pass.serial || memcmp(v, g_bz_last, sizeof v)) {
        __strong id<MTLBuffer> b; size_t off = arena_alloc(16, &b);
        memset((uint8_t *)b.contents + off, 0, 16); memcpy((uint8_t *)b.contents + off, v, sizeof v);
        gpu = b.gpuAddress + off; pass = g_pass.serial; memcpy(g_bz_last, v, sizeof v);
        if (v[0]) { static uint64_t n; if (++n <= 3 || !(n & (n - 1))) MLOG("blend: a zero source weight on a floating-point target (outputs %#x): a non-finite colour is blended as 0 there, as NVIDIA's blender does (%llu passes or states so far)", v[0], (unsigned long long)n); }
    }
    enc_r_args(en, MTLRenderStageFragment, gpu, DMSH_BLEND_BUFFER);
}
// NVN's logic operations have no Metal equivalent (docs/reference/rendering.md#logic-operations-through-a-library-of-its-own): the typed-output or side entry point reads each
// attachment and combines; a target with no bits to combine (float, sRGB, signed) keeps COPY, in red. The operation replaces blending.
static inline int lop_on(void) { return S.color.logic_op != NVN_LOGIC_OP_COPY; }
static int unorm_chan_masks(MTLPixelFormat f, uint32_t m[4]) {   // alpha of a format without it: any, Metal drops the write; 0: not normalized
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
static uint32_t lop_key(const pso_key *k, NvShader *fs, int merged) {
    if (!fs || !lop_on()) return 0;
    int any = 0, norm = 0, other = 0;
    for (int i = 0; i < 8; i++) if (k->cfmt[i] && (!fs->rf.color_known || (fs->rf.color_mask >> i & 1)) && (k->chmask >> (4 * i) & 0xf)) {
        uint32_t bits[4]; any = 1;
        if (unorm_chan_masks((MTLPixelFormat)k->cfmt[i], bits)) norm = 1; else if (pf_int_kind((MTLPixelFormat)k->cfmt[i])) other = 1;
    }
    if (!any) return 0;
    if (merged) { RED_ONCE(134, "logic operation %u in a merged pass (shader %s): drawn with COPY", S.color.logic_op, fs->key); return 0; }
    int u = 0;
    if (fs->u_logic) for (int i = 0; i < 8; i++) u |= k->cfmt[i] && pf_int_kind((MTLPixelFormat)k->cfmt[i]) == 1;
    if (!u && (other || !norm)) { RED_ONCE(133, "shader %s draws with logic operation %u into a target with no bits to combine (floating point, sRGB, signed) or an integer target it has no entry point for: drawn with COPY", fs->key, S.color.logic_op); return 0; }
    if (k->atest && !(fs->u_logic && fs->u_alpha)) { RED_ONCE(142, "shader %s draws with logic operation %u and an alpha test, which no library combines: drawn with COPY", fs->key, S.color.logic_op); return 0; }
    if (!fs->has_logic && !fs->u_logic) { RED_ONCE(135, "shader %s draws with logic operation %u but has no logic operation entry point: drawn with COPY", fs->key, S.color.logic_op); return 0; }
    return 1;
}
static os_unfair_lock g_lo_lock = OS_UNFAIR_LOCK_INIT;
// targets: bit n = colour target n attached. The dual-source entry point has none (a logic operation replaces blending).
static id<MTLFunction> lo_fn(NvShader *fs, id<MTLFunction> base, uint32_t targets) {
    NSString *name = base && base == fs->fn ? [fs->fn.name stringByAppendingString:@"_l"] : nil;
    if (!name || !fs->has_logic) return nil;
    os_unfair_lock_lock(&g_lo_lock);
    NSError *e = nil; const char *why = NULL;
    if (!fs->lfn_state) {
        fs->lfn_state = 2;
        if ((fs->llib = bx_lib(fs, &e))) {
            fs->lfn_state = 1; fs->lfn = [NSMutableDictionary dictionary];
            static int n;
            if (++n <= 3 || !(n & (n - 1))) MLOG("logic operation: side library of %s loaded (%d shaders so far)", fs->key, n);
        }
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
static void uint_chan_masks(MTLPixelFormat f, uint32_t m[4]) {   // the operation's result is cut to them, as the hardware writes them
    uint32_t b = f == MTLPixelFormatR8Uint || f == MTLPixelFormatRG8Uint || f == MTLPixelFormatRGBA8Uint ? 0xffu : f == MTLPixelFormatR16Uint || f == MTLPixelFormatRG16Uint || f == MTLPixelFormatRGBA16Uint ? 0xffffu : 0xffffffffu;
    for (int c = 0; c < 4; c++) m[c] = b;
    if (f == MTLPixelFormatRGB10A2Uint) { m[0] = m[1] = m[2] = 0x3ffu; m[3] = 3u; }
}
static void lo_bind(id<MTL4RenderCommandEncoder> en) {
    static uint64_t gpu, pass; static uint32_t last[36];
    uint32_t v[36] = {S.color.logic_op};
    for (int i = 0; i < 8; i++) if (g_pass.cfmt[i]) {
        MTLPixelFormat f = (MTLPixelFormat)g_pass.cfmt[i];
        if (pf_int_kind(f)) uint_chan_masks(f, &v[4 + 4 * i]); else unorm_chan_masks(f, &v[4 + 4 * i]);
    }
    if (!gpu || pass != g_pass.serial || memcmp(v, last, sizeof v)) {
        __strong id<MTLBuffer> b; size_t off = arena_alloc(sizeof v, &b); memcpy((uint8_t *)b.contents + off, v, sizeof v);
        gpu = b.gpuAddress + off; pass = g_pass.serial; memcpy(last, v, sizeof v);
    }
    enc_r_args(en, MTLRenderStageFragment, gpu, DMSH_LOGIC_BUFFER);
}
#define PSO_WHOLE @"built whole"
#define PSO_FACE @"built whole, colour map inherited"
static MTLRenderPipelineDescriptor *pso_desc(const pso_key *k, NvShader *vs, NvShader *fs, id<MTLFunction> fsfn, int merged) {
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
    d.vertexFunction = vs && vs->mesh_kind != 2 ? vs->fn : nil; d.fragmentFunction = fsfn ? fsfn : fs ? fs->fn : nil;   // a mesh function is not a vertex function
    // DM_NVN_HILITE=fragment key,... (diagnostic): a solid colour per key. Pipelines are built on several threads: set up once.
    if (fs) {
        static char keys[6][24]; static int nk; static id<MTLLibrary> hl; static dispatch_once_t once;
        dispatch_once(&once, ^{
            const char *e = getenv("DM_NVN_HILITE");
            for (const char *q = e; q && *q && nk < 6; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
                size_t n = strcspn(q, ","); if (n > 23) n = 23;
                memcpy(keys[nk], q, n); keys[nk++][n] = 0;
            }
            if (!nk) return;
            NSError *err = nil;
            hl = dev_lib(@"#include <metal_stdlib>\nusing namespace metal;\nstruct O { float4 c0 [[color(0)]]; float4 c1 [[color(1)]]; float4 c2 [[color(2)]]; };\n"
                "fragment O hl0() { return {float4(1,0,1,1),float4(1,0,1,1),float4(1,0,1,1)}; } fragment O hl1() { return {float4(0,1,0,1),float4(0,1,0,1),float4(0,1,0,1)}; }\n"
                "fragment O hl2() { return {float4(0,1,1,1),float4(0,1,1,1),float4(0,1,1,1)}; } fragment O hl3() { return {float4(1,1,0,1),float4(1,1,0,1),float4(1,1,0,1)}; }\n"
                "fragment O hl4() { return {float4(0,0,1,1),float4(0,0,1,1),float4(0,0,1,1)}; } fragment O hl5() { return {float4(1,1,1,1),float4(1,1,1,1),float4(1,1,1,1)}; }\n", &err);
            if (!hl) MLOG("[RED] HILITE shader compile failed %s", err.localizedDescription.UTF8String);
        });
        for (int i = 0; i < nk; i++) if (!strncmp(fs->key, keys[i], strlen(keys[i]))) {
            if (hl) d.fragmentFunction = [hl newFunctionWithName:[NSString stringWithFormat:@"hl%d", i]];
            break;
        }
    }
    // A float output meeting an integer attachment (or the reverse) builds from a Metal 4 base and writes undefined values with no error: other classes take the typed-output entry point.
    uint32_t targets = 0;   // not a merged pass: target i is output i
    for (int i = 0; i < 8; i++) if (k->cfmt[i]) targets |= 1u << i;
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
    // Dual-source: output 1 is then no colour output (Maxwell leaves its attachment as it was). Without the dual entry point the second source is zero: passing the state as it is
    // crashes Metal 4's blend compiler (MTLCompilerService in buildBlendFactor).
    int dual = !k->logic && fs && !fsfn && fs->rf.color_known && (fs->rf.color_mask & 2) && d.fragmentFunction == fs->fn && pso_reads_src1(k, merged), dual_fn = 0;
    if (dual && fs->fn_d) { d.fragmentFunction = fs->fn_d; dual_fn = 1; }
    else if (dual) { want_note(&g_dual_want, fs->key);
        RED_ONCE(26, "shader %s writes output 1 and a pipeline blends with it as the second source, but it has no dual-source entry point: the second source counts as zero "
                 "(add its key to the profile's dual_want.tsv; DM_NVN_DUAL_WANT=<file> lists every such shader)", fs->key); }
    if (k->atest && fs && !typed_at) {
        id<MTLFunction> af = fsfn ? nil : at_fn(fs, d.fragmentFunction);
        if (af) d.fragmentFunction = af;
        else if (fsfn) RED_ONCE(129, "alpha test not applied to a specialized fragment (readable-tile merge or diagnostic) of %s", fs->key);
    }
    if (k->logic && fs && !typed) {
        id<MTLFunction> lf = fsfn ? nil : lo_fn(fs, d.fragmentFunction, targets);
        if (lf) d.fragmentFunction = lf;
        else if (fsfn) RED_ONCE(136, "logic operation not applied to a specialized fragment (readable-tile merge or diagnostic) of %s", fs->key);
    }
    // main0 alone: with an alpha test, alpha-to-coverage or a logic operation the fragment decides which samples update
    if (fs && fs->early_fn && d.fragmentFunction == fs->fn && !merged && !fsfn && !k->a2c && !k->atest && !k->logic && (k->dfmt || k->sfmt) &&
        k->early_read_only != PSO_EARLY_OFF && (!fs->early_read_only || k->early_read_only == PSO_EARLY_RO))
        d.fragmentFunction = fs->early_fn;
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
        // Outputs the shader does not write: zero write mask, no blending. Maxwell leaves those targets untouched, Metal leaves them undefined
        // (docs/reference/rendering.md#no-writes-to-colour-outputs-the-shader-does-not-write).
        int unwritten = fs && fs->rf.color_known && !fsfn && l != 0xff && (!(fs->rf.color_mask >> l & 1) || (dual_fn && l == 1));
        if (unwritten) m = 0;
        // components outside the shader's output map (SPH word 18): the GPU leaves them, Metal writes them undefined
        else if (fs && fs->rf.omap_known && !fsfn && l != 0xff) m &= fs->rf.omap >> (4 * l) & 0xf;
        c.writeMask = (m & 1 ? MTLColorWriteMaskRed : 0) | (m & 2 ? MTLColorWriteMaskGreen : 0) | (m & 4 ? MTLColorWriteMaskBlue : 0) | (m & 8 ? MTLColorWriteMaskAlpha : 0);
        // integer targets: Maxwell ignores their blend enable; Metal refuses the pipeline
        if (!k->logic && l != 0xff && k->blend_en >> l & 1 && !unwritten && !pf_int_kind(c.pixelFormat)) {
            blend_st bl = adv_blend(k->blend[l]);
            if (fs && fs->rf.color_known && (!(fs->rf.color_mask & 2) || (dual && !dual_fn)) && !fsfn) {
                bl.src_rgb = blend_zero_src1(bl.src_rgb); bl.dst_rgb = blend_zero_src1(bl.dst_rgb);
                bl.src_a = blend_zero_src1(bl.src_a); bl.dst_a = blend_zero_src1(bl.dst_a);
            }
            // ONE + ZERO under ADD left unblended: Metal's blender can write +Inf across an R11G11B10F target with it
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
    if (k->fsfn == AMP_MARK || k->fsfn == FSP_MARK) d.maxVertexAmplificationCount = 2;
    if (k->fsfn == FSP_MARK) d.label = PSO_FACE;
    if (k->fsfn == DISCARD_MARK) { d.vertexFunction = vs->discard_fn; }
    MTLVertexDescriptor *vd = [MTLVertexDescriptor vertexDescriptor];
    const nvn_refl *rf = &vs->rf; int nconv = 0, on = vin_on(vs, k, k->fsfn == DISCARD_MARK);
    if (on) { d.vertexFunction = vs->vfn; g_vin_psos++; }
    #define ZERO_ATTR(i, ct_) do { vd.attributes[i].format = (ct_) == NVN_CT_UINT ? MTLVertexFormatUInt4 : (ct_) == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4; vd.attributes[i].offset = 0; vd.attributes[i].bufferIndex = 30; \
                                  vd.layouts[30].stride = 32; vd.layouts[30].stepFunction = MTLVertexStepFunctionConstant; vd.layouts[30].stepRate = 0; } while (0)
    for (int v = 0; v < rf->nv; v++) {
        int loc = rf->v[v].loc, a = rf->v[v].nvn;   // Metal slot loc ← NVN attribute a
        va_st ka = a < 32 ? pso_key_attr(k, a) : (va_st){0};
        MTLVertexFormat vf = a < 32 ? vfmt(ka.fmt) : MTLVertexFormatInvalid;
        // shadow inputs: the attribute goes to its format class's declaration; the input's other declarations read the zero buffer
        int ct = on ? vin_ct(rf, v, vin_fmt(k, a), 1) : rf->v[v].ct, at = ct == rf->v[v].ct ? loc : rf->v[v].sh[ct];
        if (on) {
            if (at != loc) ZERO_ATTR(loc, rf->v[v].ct);
            for (int q = 0; q < 3; q++) if (rf->v[v].sh[q] != 0xff && rf->v[v].sh[q] != at) ZERO_ATTR(rf->v[v].sh[q], q);
        }
        // past S.nattr: format 0 (the key's memset), read as zero with no red line
        if (ka.fmt && vf == MTLVertexFormatInvalid) RED_ONCE(15, "vertex attribute format %#x not mapped", ka.fmt);
        if (vf == MTLVertexFormatInvalid) {   // absent (format 0) at offset 16; metal-tt wants offset + size within the stride, which a constant step does not use
            ZERO_ATTR(at, ct); if (!ka.fmt) vd.attributes[at].offset = 16;
            continue;
        }
        int st = ka.stream;
        if (vconv_wanted(rf, v, k, on) && nconv < VCONV_MAX) {
            int slot = VCONV_SLOT0 + nconv++;
            vd.attributes[loc].format = rf->v[v].ct == NVN_CT_UINT ? MTLVertexFormatUInt4 : rf->v[v].ct == NVN_CT_INT ? MTLVertexFormatInt4 : MTLVertexFormatFloat4;
            vd.attributes[loc].offset = 0; vd.attributes[loc].bufferIndex = slot;
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
