// Vertex inputs: formats corrected for the shader's types, integer inputs, the pulling entry point, conversions on the CPU (vconv).
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// Correct the vertex format by the shader input's component type: Metal requires integer inputs to get integer formats of the same signedness, and float inputs float / normalized formats.
// Integer input (inferred at translation time as "only used as an integer", dmsh_entry.py int_inputs) with an integer format: on a signedness mismatch switch to the same width with the other sign (bit pattern unchanged for 32-bit components;
// 8 / 16-bit ones would be widened the wrong way, so vconv_wanted converts them first and this switch is only the fallback when the conversion slots run out); with a four-component 32-bit float format read it as an integer of the same width (bit pattern unchanged). Short float vectors need float padding through a shadow or CPU conversion.
// Other mismatches (integer input with a normalized / half format, float input with an integer format) cannot keep the bit pattern: report once and keep the format.
static MTLVertexFormat vfmt_for_ct(MTLVertexFormat f, int ct, int nvn_fmt) {
    static const MTLVertexFormat U[] = { MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4, MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4, MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4 };
    static const MTLVertexFormat I[] = { MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4, MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4, MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4 };
    static const MTLVertexFormat F[] = { MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4 };
    int iu = -1, ii = -1, iff = -1;
    for (int i = 0; i < 12; i++) { if (U[i] == f) iu = i; if (I[i] == f) ii = i; }
    for (int i = 0; i < 4; i++) if (F[i] == f) iff = i;
    if (ct == NVN_CT_UINT) { if (iu >= 0) return f; if (ii >= 0) return U[ii]; if (iff >= 0) return U[8 + iff]; }
    else if (ct == NVN_CT_INT) { if (ii >= 0) return f; if (iu >= 0) return I[iu]; if (iff >= 0) return I[8 + iff]; }
    else { if (iu < 0 && ii < 0) return f; if ((iu >= 8 && iu < 12) || (ii >= 8 && ii < 12)) return F[(iu >= 8 ? iu : ii) - 8]; }   // 32-bit integers into a float input: the same bits as floats
    // An integer input fed a normalized, half or packed format: Maxwell would hand the shader the bits of the converted float, which Metal cannot, and Metal refuses the whole pipeline
    // ("Vertex attribute … of type uint4 cannot be read using MTLAttributeFormatHalf2"). Read the same bytes as integers of the same layout instead, so the pipeline exists and the draw is kept.
    // Fire Emblem: Three Houses: KTGL's character shaders read attribute 5 as integer bits (skinning data) and static meshes bind a Half2 there; with the pipeline refused, the characters'
    // shading draws were dropped and only black silhouettes remained (gender choice, in the prologue)
    if (ct == NVN_CT_UINT || ct == NVN_CT_INT) {
        static const struct { MTLVertexFormat f; uint8_t u; } P[] = {   // u = index into U / I with the same component size and count
            { MTLVertexFormatUCharNormalized, 0 }, { MTLVertexFormatCharNormalized, 0 }, { MTLVertexFormatUChar2Normalized, 1 }, { MTLVertexFormatChar2Normalized, 1 },
            { MTLVertexFormatUChar3Normalized, 2 }, { MTLVertexFormatChar3Normalized, 2 }, { MTLVertexFormatUChar4Normalized, 3 }, { MTLVertexFormatChar4Normalized, 3 }, { MTLVertexFormatUChar4Normalized_BGRA, 3 },
            { MTLVertexFormatUShortNormalized, 4 }, { MTLVertexFormatShortNormalized, 4 }, { MTLVertexFormatHalf, 4 }, { MTLVertexFormatUShort2Normalized, 5 }, { MTLVertexFormatShort2Normalized, 5 }, { MTLVertexFormatHalf2, 5 },
            { MTLVertexFormatUShort3Normalized, 6 }, { MTLVertexFormatShort3Normalized, 6 }, { MTLVertexFormatHalf3, 6 }, { MTLVertexFormatUShort4Normalized, 7 }, { MTLVertexFormatShort4Normalized, 7 }, { MTLVertexFormatHalf4, 7 },
            { MTLVertexFormatUInt1010102Normalized, 8 }, { MTLVertexFormatInt1010102Normalized, 8 }, { MTLVertexFormatFloatRG11B10, 8 },
        };
        for (unsigned i = 0; i < sizeof P / sizeof *P; i++) if (P[i].f == f) {
            RED_ONCE(91, "vertex attribute format %#x feeds a %s input: Metal does not allow it and the conversion slots are full; the raw bytes are read with the integer format of the same layout (the bit pattern differs from Maxwell's converted one)", nvn_fmt, ct == NVN_CT_UINT ? "uint" : "int");
            return ct == NVN_CT_UINT ? U[P[i].u] : I[P[i].u];
        }
    }
    RED_ONCE(66, "vertex attribute format %#x does not match shader input type %s; the bit pattern is not preserved (original format kept)", nvn_fmt, ct == NVN_CT_UINT ? "uint" : ct == NVN_CT_INT ? "int" : "float");
    return f;
}
// Integer shader inputs fed float-type vertex formats (half, unorm / snorm 8 and 16, 10:10:10:2, 11:11:10, BGRA8). Maxwell's fetch converts by the format and the register holds float32
// bits, which such shaders take back with as_type<float> (`int_inputs` declared the input uint because its first translation only bit-cast it). Metal can neither convert into an integer
// input nor build such a pipeline, and reading the raw bytes as integers (vfmt_for_ct's fallback) gives denormals: Fire Emblem: Three Houses' and Age of Calamity's soldiers and Claude came
// out flat tan, their UVs collapsed onto one texel. So the replay converts these attributes on the CPU into float32 bits, four components with the missing ones (0, 0, 0, 1.0) as the fetch
// fills them, in the frame arena, and the pipeline reads them as UInt4 / Int4 from buffer slots of their own (VCONV_SLOT0 + n, in reflection order; slots 1-12 are unused by translated shaders)
enum { VCONV_SLOT0 = 1, VCONV_MAX = 12 };
static int vconv_ncomp(int f) {   // components of an NVN float-type vertex format that needs the conversion; 0 = not one
    switch (f) {
        // Reinterpreting Float1/2/3 as UInt1/2/3 preserves supplied bits but pads w
        // with integer 1. The float fetch (or CPU fallback) must supply float 1.0.
        // Float4 has no missing components and keeps the direct reinterpretation.
        case 0x01: case 0x02: case 0x05: case 0x06: case 0x07: case 0x0A: return 1;
        case 0x0D: case 0x0E: case 0x11: case 0x12: case 0x13: case 0x16: return 2;
        case 0x19: case 0x1A: case 0x1D: case 0x1E: case 0x1F: case 0x22: case 0x3F: return 3;
        case 0x25: case 0x26: case 0x29: case 0x2A: case 0x2B: case 0x3D: case 0x69: case 0x9A: return 4;
    }
    return 0;
}
// 8- and 16-bit integer formats: Maxwell's fetch widens each component into the 32-bit register by the format's own signedness (sign for SINT, zero for UINT). Metal reads them only into
// an input of the same signedness, and vfmt_for_ct's switch to the other sign keeps the 8 / 16 bits but widens them the other way. So an input of the other signedness takes the 32-bit
// values converted here too. Fire Emblem: Three Houses: the terrain's heights are RGBA16I into a uint input; negative heights read as 32768-65535 and the ground under close-up
// cameras was never drawn (the lighting then saw depth 0 and painted fog there)
static int vconv_int(int f, int *bits, int *sgn) {   // components of an 8 / 16-bit integer format (with its component size and signedness); 0 = not one
    static const uint8_t T[16] = { 0x03, 0x04, 0x08, 0x09, 0x0F, 0x10, 0x14, 0x15, 0x1B, 0x1C, 0x20, 0x21, 0x27, 0x28, 0x2C, 0x2D };   // per component count: UI8, I8, UI16, I16
    for (int i = 0; i < 16; i++) if (T[i] == f) { *bits = i & 2 ? 16 : 8; *sgn = i & 1; return i / 4 + 1; }
    return 0;
}
// *_UI2F / *_I2F formats (0x51-0x68, R8 R16 R32 RG8 … RGBA32, unsigned then signed): integers that the fetch converts to float without normalizing (200 → 200.0). Metal has no such vertex
// format and refuses an integer format into a float input, so they are converted here too; an integer input of the same format takes the float32 bits, as from Maxwell's fetch.
// Breath of the Wild: an RG8_UI2F attribute (0x57, at the title) had no Metal format and read 0
static int vconv_i2f(int f, int *bits, int *sgn) {   // components of an integer-read-as-float format (with its component size and signedness); 0 = not one
    if (f < 0x51 || f > 0x68) return 0;
    int i = (f - 0x51) >> 1; *sgn = (f - 0x51) & 1; *bits = 8 << (i % 3); return i / 3 + 1;
}
static int vconv_size(int f) {   // bytes of one element
    int ib, is, in = vconv_int(f, &ib, &is); if (in) return in * ib / 8;
    if ((in = vconv_i2f(f, &ib, &is))) return in * ib / 8;
    switch (f) { case 0x01: case 0x02: return 1; case 0x05: case 0x06: case 0x07: case 0x0D: case 0x0E: return 2; case 0x19: case 0x1A: return 3;
                 case 0x1D: case 0x1E: case 0x1F: return 6; case 0x16: case 0x29: case 0x2A: case 0x2B: return 8; case 0x22: return 12; }
    return 4;
}
static float vconv_uf(uint32_t v, int mbits) {   // unsigned small float of R11G11B10F: 5-bit exponent (bias 15), mbits-bit mantissa
    uint32_t e = v >> mbits & 31, m = v & ((1u << mbits) - 1);
    if (!e) return ldexpf((float)m, -14 - mbits);
    if (e == 31) return m ? NAN : INFINITY;
    return ldexpf(1.0f + (float)m / (float)(1u << mbits), (int)e - 15);
}
static void vconv_elem(int f, const uint8_t *p, float o[4]) {
    int ib, is, in = vconv_int(f, &ib, &is);
    if (in) {   // integer format: the register's 32-bit value, the missing components (0, 0, 0, 1) as integers
        uint32_t u[4] = { 0, 0, 0, 1 };
        for (int i = 0; i < in; i++) {
            if (ib == 8) u[i] = is ? (uint32_t)(int32_t)(int8_t)p[i] : p[i];
            else { uint16_t h; memcpy(&h, p + 2 * i, 2); u[i] = is ? (uint32_t)(int32_t)(int16_t)h : h; } }
        memcpy(o, u, sizeof u); return; }
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    if ((in = vconv_i2f(f, &ib, &is))) {   // integers read as floats: each value as a float, the missing components (0, 0, 0, 1.0)
        for (int i = 0; i < in; i++) {
            if (ib == 8) o[i] = is ? (float)(int8_t)p[i] : (float)p[i];
            else if (ib == 16) { uint16_t h; memcpy(&h, p + 2 * i, 2); o[i] = is ? (float)(int16_t)h : (float)h; }
            else { uint32_t w; memcpy(&w, p + 4 * i, 4); o[i] = is ? (float)(int32_t)w : (float)w; } }
        return; }
    int n = vconv_ncomp(f);
    for (int i = 0; i < n; i++) switch (f) {
        case 0x0A: case 0x16: case 0x22: memcpy(o + i, p + 4 * i, 4); break;
        case 0x01: case 0x0D: case 0x19: case 0x25: o[i] = p[i] / 255.0f; break;
        case 0x02: case 0x0E: case 0x1A: case 0x26: o[i] = fmaxf((int8_t)p[i] / 127.0f, -1.0f); break;
        case 0x05: case 0x11: case 0x1D: case 0x29: { _Float16 h; memcpy(&h, p + 2 * i, 2); o[i] = (float)h; break; }
        case 0x06: case 0x12: case 0x1E: case 0x2A: { uint16_t u; memcpy(&u, p + 2 * i, 2); o[i] = u / 65535.0f; break; }
        case 0x07: case 0x13: case 0x1F: case 0x2B: { int16_t s; memcpy(&s, p + 2 * i, 2); o[i] = fmaxf(s / 32767.0f, -1.0f); break; }
        case 0x9A: o[i] = p[i == 3 ? 3 : 2 - i] / 255.0f; break;   // BGRA8 unorm: the fetch hands the shader R, G, B, A
        case 0x3D: case 0x69: case 0x3F: {
            uint32_t w; memcpy(&w, p, 4);
            if (f == 0x3F) o[i] = vconv_uf(i == 0 ? w & 0x7ff : i == 1 ? w >> 11 & 0x7ff : w >> 22, i == 2 ? 5 : 6);
            else { uint32_t bits = i == 3 ? 2 : 10, v = w >> (10 * i) & ((1u << bits) - 1);
                   if (f == 0x3D) o[i] = (float)v / (float)((1u << bits) - 1);
                   else { int32_t sv = (int32_t)(v << (32 - bits)) >> (32 - bits); o[i] = fmaxf((float)sv / (float)((1u << (bits - 1)) - 1), -1.0f); } }
            break; }
    }
}
// Shadow inputs instead of the conversion: a vertex shader with integer inputs also has a library <key>_bv (dmsh_vin.py) whose entry points declare each integer input again as a float4
// and as the integer of the other sign, at Metal attribute indices 16-30, and OR the three into the input the body reads. A pipeline sends each attribute to the declaration of its
// format's class (half / normalized formats to the float one, 8 / 16-bit integers to the one of their own sign) and the zero buffer to the others, and Metal's fetch then gives the
// shader Maxwell's register bits: no CPU conversion, whatever mix of formats the pipeline has (Age of Calamity's soldiers feed Half2 beside RGBA8UI and Short2 to one shader; the
// conversion was about a quarter of its replay thread in battle). DM_NVN_VCONV_CPU=1 converts on the CPU as before
static int vin_cpu(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VCONV_CPU"); v = e && !strcmp(e, "1"); if (v) MLOG("DM_NVN_VCONV_CPU=1: vertex attributes Metal's fetch cannot read are converted on the CPU (no shadow inputs, no pulling)"); } return v; }
static os_unfair_lock g_vin_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t g_vin_loaded, g_vin_failed, g_vin_psos;   // shadow-input libraries loaded / not usable, pipelines built with them
static id<MTLFunction> vin_fn(NvShader *vs) {   // the shadow-input entry point, loaded on first need; a failure is final for the shader (it keeps the conversion)
    if (!vs->rf.has_vin || vin_cpu()) return nil;
    os_unfair_lock_lock(&g_vin_lock);
    if (!vs->vfn_state) {
        vs->vfn_state = 2;
        size_t kl = strlen(vs->key); NSString *path = [NSString stringWithFormat:@"%@/%.*s_bv.metallib", g_msl_dir, (int)(kl > 2 ? kl - 2 : kl), vs->key];
        NSString *name = vs->vinentry ?: vs->ventry;   // the reflection's name for it in this library (shader_load)
        NSError *e = nil; id<MTLLibrary> lib = lib_from_file(path, &e); id<MTLFunction> f = lib && name ? fn4_note([lib newFunctionWithName:name], lib, name, nil) : nil;
        if (f && f.functionConstantsDictionary.count) f = fn_spec(lib, name, [MTLFunctionConstantValues new], &e);   // as shader_load
        const char *why = !lib ? "no library" : !f ? "no entry point" : NULL;
        if (f && vs->rf.nb && [f newArgumentEncoderWithBufferIndex:vs->rf.abi].encodedLength != vs->arg_len) { why = "its argument buffer differs from the base library's"; f = nil; }
        if (f) { vs->vfn = f; vs->vfn_state = 1; g_vin_loaded++; }
        else if (g_vin_failed++ < 8) MLOG("[RED] shadow-input library of %s not usable (%s%s%s): its integer inputs keep the CPU conversion", vs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    }
    id<MTLFunction> f = vs->vfn_state == 1 ? vs->vfn : nil;
    os_unfair_lock_unlock(&g_vin_lock);
    return f;
}
// The pulling entry point (in <key>_bp, dmsh_pull.py, under the name the reflection's "pull" gives it: pentry, today the shader's own, main0 or main0_z), loaded on first need; a
// failure is final for the shader (its draws keep waiting for their pipelines as before)
static os_unfair_lock g_pull_lock = OS_UNFAIR_LOCK_INIT;
// Loads the pulling entry into vs (pfn_state 1, or 2 when it is not usable). pull_fn holds g_pull_lock around it; shader_prefetch_rest calls it on a shader no other thread has yet
static void pull_load(NvShader *vs) {
    {
        vs->pfn_state = 2;
        size_t kl = strlen(vs->key); NSString *path = [NSString stringWithFormat:@"%@/%.*s_bp.metallib", g_msl_dir, (int)(kl > 2 ? kl - 2 : kl), vs->key];
        NSString *name = vs->pentry ?: vs->ventry;
        NSError *e = nil; id<MTLLibrary> lib = lib_from_file(path, &e); id<MTLFunction> f = lib && name ? fn4_note([lib newFunctionWithName:name], lib, name, nil) : nil;
        if (f && f.functionConstantsDictionary.count) f = fn_spec(lib, name, [MTLFunctionConstantValues new], &e);
        const char *why = !lib ? "no library" : !f ? "no entry point" : NULL;
        if (f && vs->rf.nb && [f newArgumentEncoderWithBufferIndex:vs->rf.abi].encodedLength != vs->arg_len) { why = "its argument buffer differs from the base library's"; f = nil; }
        if (f) { vs->pfn = f; vs->pfn_state = 1;
            NSString *wn = vs->wlentry ?: [NSString stringWithFormat:@"%@_wl", vs->ventry]; id<MTLFunction> w = fn4_note([lib newFunctionWithName:wn], lib, wn, nil);
            if (w && w.functionConstantsDictionary.count) w = fn_spec(lib, wn, [MTLFunctionConstantValues new], &e);
            vs->wlfn = w; }
        else { static _Atomic int n; if (n++ < 8) MLOG("[RED] pulling library of %s not usable (%s%s%s): its draws wait for their pipelines", vs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : ""); }
    }
}
static id<MTLFunction> pull_fn(NvShader *vs) {
    if (!vs->has_pull) return nil;
    os_unfair_lock_lock(&g_pull_lock);
    if (!vs->pfn_state) pull_load(vs);
    id<MTLFunction> f = vs->pfn_state == 1 ? vs->pfn : nil;
    os_unfair_lock_unlock(&g_pull_lock);
    return f;
}
static inline int vin_fmt(const pso_key *k, int a) { return a >= 32 ? 0 : k ? pso_key_attr(k, a).fmt : a < S.nattr ? S.attr[a].fmt : 0; }   // as pso_key_fill fills the key
// The component type input v takes its attribute as: the class of the attribute's format when a shadow of that class exists (on), else the declared type
static int vin_ct(const nvn_refl *rf, int v, int f, int on) {
    int ct = rf->v[v].ct, ib, is;
    if (!on || ct == NVN_CT_FLOAT) return ct;
    int want = vconv_ncomp(f) ? NVN_CT_FLOAT : vconv_int(f, &ib, &is) ? (is ? NVN_CT_INT : NVN_CT_UINT) : ct;
    return want != ct && rf->v[v].sh[want] != 0xff ? want : ct;
}
// Whether this pipeline (k, or the current state at bind time: both give the same answer) uses the shadow inputs: some input takes a shadow and the library loads. Asked by pso_desc
// and vconv_bind alike, so the converted attributes and the pipeline's buffer slots agree
static int vin_on(NvShader *vs, const pso_key *k, int plain) {   // plain: a pipeline with no fragment stage (rasterizer discard, the vertex dump's capture), which keeps the conversion
    const nvn_refl *rf = &vs->rf;
    if (plain || !rf->has_vin || vin_cpu()) return 0;
    int need = 0; for (int v = 0; v < rf->nv && !need; v++) need = vin_ct(rf, v, vin_fmt(k, rf->v[v].nvn), 1) != rf->v[v].ct;
    return need && vin_fn(vs) != nil;
}
// A float input fed an 8 / 16-bit integer format takes the same 32-bit register values through a Float4 attribute, which Metal passes through bit for bit: Monster Hunter Rise's
// skinned vertex shaders (40609cd5c1443df4, 1369a37113da0012) read RGBA8UI attribute 5 as flags (as_type<int>(x) & 2 ...) and also as a float, so the build leaves the input float
static int vconv_wanted(const nvn_refl *rf, int v, const pso_key *k, int on) {   // does Metal input v of this pipeline take converted data (on: it uses the shadow inputs, vin_on)
    int a = rf->v[v].nvn, ct = rf->v[v].ct, ib, is;
    if (a >= 32) return 0;
    int f = k ? pso_key_attr(k, a).fmt : S.attr[a].fmt;
    if (vconv_i2f(f, &ib, &is)) return 1;   // whatever the input type
    if (ct == NVN_CT_FLOAT) return vconv_int(f, &ib, &is) > 0;
    if (ct != NVN_CT_UINT && ct != NVN_CT_INT) return 0;
    if (vin_ct(rf, v, f, on) != ct) return 0;   // a shadow input takes it
    return vconv_ncomp(f) || (vconv_int(f, &ib, &is) && is != (ct == NVN_CT_INT));
}
// Whether the pipeline for key k draws with the vertex shader's pulling entry as its own pipeline (pull_pso): an attribute of its layout needs the CPU conversion (vconv_wanted, after
// the shadow inputs took what they can) and the shader has a pulling library. dm_fetch decodes every format on the GPU into the register words Maxwell's fetch gives, for the
// vertices drawn only (docs/reference/rendering.md, "Attributes the vertex fetch cannot read, pulled"). DM_NVN_VCONV_CPU=1 keeps the CPU conversion
static int pull_conversion(NvShader *vs, const pso_key *k) {
    if (!vs->has_pull || vin_cpu()) return 0;
    int on = vin_on(vs, k, 0);
    for (int v = 0; v < vs->rf.nv; v++) if (vconv_wanted(&vs->rf, v, k, on)) return 1;
    return 0;
}
// nv elements from src (stride bytes apart) into d, four 32-bit values each. The 8 / 16-bit integer and the half formats (Age of Calamity's soldiers: Short2, Half2) have loops
// of their own with the format decoded once; vconv_elem decodes it for every element
static void vconv_fill(int f, uint64_t src, uint32_t stride, uint64_t nv, float *d) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)src; uint32_t *u = (uint32_t *)d; int ib, is, in = vconv_int(f, &ib, &is);
    if (in) {   // as vconv_elem: the register's 32-bit value, the missing components (0, 0, 0, 1)
        for (uint64_t i = 0; i < nv; i++, p += stride, u += 4) {
            u[0] = u[1] = u[2] = 0; u[3] = 1;
            for (int c = 0; c < in; c++) {
                if (ib == 8) u[c] = is ? (uint32_t)(int32_t)(int8_t)p[c] : p[c];
                else { uint16_t h; memcpy(&h, p + 2 * c, 2); u[c] = is ? (uint32_t)(int32_t)(int16_t)h : h; } } }
        return; }
    if (f == 0x05 || f == 0x11 || f == 0x1D || f == 0x29) {   // R16F … RGBA16F
        int n = vconv_ncomp(f);
        for (uint64_t i = 0; i < nv; i++, p += stride, d += 4) {
            d[0] = d[1] = d[2] = 0; d[3] = 1;
            for (int c = 0; c < n; c++) { _Float16 h; memcpy(&h, p + 2 * c, 2); d[c] = (float)h; } }
        return; }
    for (uint64_t i = 0; i < nv; i++) vconv_elem(f, p + i * stride, d + 4 * i);
}
// Profile vconv_range (Age of Calamity): convert only the vertices each draw reads. KTGL binds whole mesh pools (up to ~590,000 vertices) and draws parts of them, so
// converting the bound range came to millions of vertices a frame in a battle with hundreds of soldiers: 24 of the 32 ms the replay took per frame on the Mac (26 fps at
// a 30 or a 60 fps target). The range is first / count, or for a direct indexed draw the indices' smallest and largest plus the base vertex; the conversion is bound as many
// vertices before its start as the range starts at. The indices' range is kept while their pages stay unwritten (write-protected as packaged textures, nvn_dirty_protect),
// and never trusted again once a GPU copy, update or fill is queued on them (pend_add). Keeping the conversions too did not pay: the soldiers' attributes change every frame.
// DM_NVN_VCONV_CHECK=1 scans the indices again at every reuse and reports differences. DM_NVN_VCONV_RANGE=0 / 1 overrides the profile
enum { VC_RGN = 1 << 16 };   // bits of the region filter
typedef struct { uint64_t ibuf; int32_t count, itype; } ir_key;
typedef struct { uint32_t mn, mx; uint64_t stamp, seen, lo, n; const void *span; int gpuw; } ir_ent;   // gpuw: a GPU write was queued on the indices: never trusted again
static nvn_hmap g_ir = {.klen = sizeof(ir_key)};
static uint64_t g_vc_rgn[VC_RGN / 64], g_vc_stat, g_vc_conv, g_vc_whole, g_vc_checks, g_vc_bad;   // g_vc_rgn: 1 MB regions holding a kept index range (hashed); g_vc_conv / g_vc_whole: vertices converted / in the bound ranges
static int vc_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VCONV_RANGE"); v = e && *e ? atoi(e) != 0 : DM_GAME_VCONV_RANGE; MLOG("vertex conversion of the draw's vertices only: %s", v ? "on" : "off"); } return v; }
static int vc_check(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VCONV_CHECK"); v = e && *e && atoi(e); } return v; }
static inline uint32_t vc_rgn(uint64_t r) { return (uint32_t)((r * 0x9E3779B97F4A7C15ull) >> 48) & (VC_RGN - 1); }
static void vc_rgn_mark(uint64_t lo, uint64_t n) { for (uint64_t r = lo >> 20; r <= (lo + n - 1) >> 20; r++) { uint32_t b = vc_rgn(r); g_vc_rgn[b >> 6] |= 1ull << (b & 63); } }
static int vc_rgn_hit(uint64_t lo, uint64_t n) { for (uint64_t r = lo >> 20; r <= (lo + n - 1) >> 20; r++) { uint32_t b = vc_rgn(r); if (g_vc_rgn[b >> 6] >> (b & 63) & 1) return 1; } return 0; }
static void vc_gpu_write(uint64_t dst, uint64_t n) {   // pend_add: a GPU write queued on [dst, dst + n)
    vk_gpu_write(dst, n ? n : 1);
    if (!n || !g_ir.n || !vc_rgn_hit(dst, n)) return;
    for (uint32_t i = 0; i < g_ir.cap; i++) { ir_ent *e = g_ir.vals[i]; if (e && dst < e->lo + e->n && dst + n > e->lo) e->gpuw = 1; }
}
static void ix_minmax(const void *p, int isz, int32_t count, uint32_t *mn, uint32_t *mx) {
    uint32_t a = UINT32_MAX, b = 0;
    if (isz == 1) for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint8_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    else if (isz == 2) for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint16_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    else for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint32_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    *mn = a; *mx = b;
}
// Smallest and largest index of a direct indexed draw whose indices no GPU write queued in this submission covers; 0 = not known (the whole bound range is converted)
static int ix_range(const p_draw_idx *di, uint32_t *mn, uint32_t *mx) {
    int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; uint64_t n = (uint64_t)di->count * (uint64_t)isz;
    if (di->ind || di->mdi_cnt || di->count <= 0 || !di->ibuf || pend_hits(di->ibuf, n)) return 0;
    const void *p = (const void *)(uintptr_t)di->ibuf;
    ir_key k = {di->ibuf, di->count, di->itype}; ir_ent *e = nvn_hm_get(&g_ir, &k); uint64_t now = nvn_dirty_now();
    if (e && e->gpuw) return 0;
    if (e && (now == e->seen || !nvn_dirty_since_span(e->span, p, (size_t)n, e->stamp))) {
        e->seen = now; *mn = e->mn; *mx = e->mx;
        if (vc_check()) { uint32_t a, b; ix_minmax(p, isz, di->count, &a, &b); g_vc_checks++;
            if ((a != e->mn || b != e->mx) && g_vc_bad++ < 20) MLOG("[RED] vertex conversion: the kept index range of %#llx (%d indices) is %u-%u, the indices now %u-%u", (unsigned long long)di->ibuf, di->count, e->mn, e->mx, a, b); }
        return 1;
    }
    const void *span = nvn_dirty_span(p, (size_t)n);
    if (!span) return 0;   // not in a tracked memory pool
    if (!e) { if (g_ir.n >= 1 << 16) { nvn_hm_clear(&g_ir, free); memset(g_vc_rgn, 0, sizeof g_vc_rgn); } e = calloc(1, sizeof *e); nvn_hm_put(&g_ir, &k, e); }
    e->stamp = e->seen = nvn_dirty_now(); nvn_dirty_protect(p, (size_t)n); e->span = span; e->lo = di->ibuf; e->n = n; vc_rgn_mark(e->lo, e->n);   // take the stamp, protect, then read (as pkg_upload)
    ix_minmax(p, isz, di->count, &e->mn, &e->mx); *mn = e->mn; *mx = e->mx;
    return 1;
}
// The vertex ids a direct draw reads: [*lo, *hi]; 0 = not known (indirect, multi-draw, indices not known)
static int vc_draw_range(const p_draw *dw, const p_draw_idx *di, int64_t *lo, int64_t *hi) {
    if (dw) { if (dw->ind || dw->mdi_cnt || dw->count <= 0) return 0; *lo = dw->first; *hi = (int64_t)dw->first + dw->count - 1; return 1; }
    uint32_t mn, mx; if (!di || !ix_range(di, &mn, &mx)) return 0;
    *lo = (int64_t)mn + di->base_vertex; *hi = (int64_t)mx + di->base_vertex; return 1;
}
// Kept conversions: a conversion whose source bytes have not changed is bound again in later submissions, not made again. A football stadium's streams (EA SPORTS FC 27, 8- and
// 16-bit integers into float inputs) come to 7.65 million vertices a submission, 122 MB written every frame: 26.8 ms of the submitting thread on an M4. A key (range, format,
// layout) is only noted at its first submission, with 64 samples of its source; from the next one that finds the samples unchanged, its conversion is made into a buffer of its own and kept while
//   - no game or host write reached the source: the pages wholly inside it are write-protected (nvn_dirty_protect, as a kept index range), and the bytes before and after them, in
//     pages the stream shares with other data, are compared with a copy, so a neighbour that is written every frame neither drops the conversion nor takes a fault for it;
//   - no GPU write was queued on it (vk_gpu_write: copies, updates, fills, writable storage bindings), and none was in flight or queued when it was made.
// A source that changed drops its conversion (the buffer is released once the submissions that read it complete; it is never rewritten) and is tried again after 32 submissions,
// twice as many after each further change (VK_STRIKES), so a stream the game rewrites every frame costs a write-protection that often. Not kept: ranges below vk_min() vertices
// (the conversion costs less than the bookkeeping), sources outside tracked pools, anything while the game has sparse mappings (a write through an alias takes no fault), and
// conversions past VK_BYTES in all. A key not drawn for VK_IDLE submissions is forgotten. DM_NVN_VCONV_KEEP=0 turns it off, =n sets the smallest range kept
enum { VK_MAX = 1024, VK_ENTS = 8192, VK_IDLE = 600, VK_STRIKES = 10 };
static const uint64_t VK_BYTES = 256ull << 20;
typedef struct { uint64_t addr, size, v0, nv, nt; uint32_t fmt, off, stride, zero; } vk_key;
typedef struct { vk_key k; void *buf; uint64_t gpu, bytes, lo, n, plo, pn, stamp, seen, ok, used, kept, retry; const void *span; uint8_t *edge; uint64_t sig; uint32_t strikes; int li; } vk_ent;   // buf: retained; [lo, lo+n): the source bytes, [plo, plo+pn): its protected pages; edge: a copy of the rest; ok: the submission it was last found unchanged in; li: its place in g_vk_list
static nvn_hmap g_vk = {.klen = sizeof(vk_key)};
static vk_ent *g_vk_list[VK_MAX]; static int g_vk_n;   // the entries that hold a conversion
static uint64_t g_vk_bytes, g_vk_lo = UINT64_MAX, g_vk_hi, g_vk_swept, g_vk_reused, g_vk_made, g_vk_dropped, g_vk_why[4];   // g_vk_lo / hi: the span of the kept sources; reused: vertices bound from a kept conversion, made / dropped: conversions, why: dropped by a write to its pages, to its bytes in a shared page, by the GPU, as not drawn (the vertex conversion line)
static int vk_min(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_VCONV_KEEP"); v = e && *e ? atoi(e) : 4096; if (v < 0) v = 0; if (v == 1) v = 4096; if (!v) MLOG("DM_NVN_VCONV_KEEP=0: vertex conversions are made again every submission"); } return v; }
static void vk_drop(vk_ent *e, int strike, int why) {   // the conversion goes; the entry stays for its next try
    if (e->buf) {
        g_vk_why[why]++;
        id<MTLBuffer> b = (__bridge_transfer id<MTLBuffer>)e->buf; e->buf = NULL; defer_obj_release(b);
        g_vk_bytes -= e->bytes; g_vk_list[e->li] = g_vk_list[--g_vk_n]; g_vk_list[e->li]->li = e->li; g_vk_dropped++;
        if (!g_vk_n) { g_vk_lo = UINT64_MAX; g_vk_hi = 0; }
    }
    free(e->edge); e->edge = NULL;
    if (strike) { if (e->strikes < VK_STRIKES) e->strikes++; e->retry = g_sub_serial + (16ull << e->strikes); }
}
static void vk_gpu_write(uint64_t dst, uint64_t n) {
    if (!g_vk_n) return;
    if (!n) { while (g_vk_n) vk_drop(g_vk_list[0], 1, 2); return; }
    if (dst >= g_vk_hi || n > UINT64_MAX - dst || dst + n <= g_vk_lo) return;
    for (int i = g_vk_n; i-- > 0;) { vk_ent *e = g_vk_list[i]; if (dst < e->lo + e->n && dst + n > e->lo) vk_drop(e, 1, 2); }
}
static void vk_free(void *v) { vk_ent *e = v; vk_drop(e, 0, 3); free(e); }
static void vk_sweep(void) {   // every 64 submissions: keys not drawn for VK_IDLE submissions go
    if (g_sub_serial - g_vk_swept < 64) return;
    g_vk_swept = g_sub_serial;
    static vk_ent *old[256]; int n = 0;   // a removal moves entries, so they are listed first; the rest goes at the next sweep
    for (uint32_t i = 0; i < g_vk.cap && n < 256; i++) { vk_ent *e = g_vk.vals[i]; if (e && g_sub_serial - e->used > VK_IDLE) old[n++] = e; }
    for (int i = 0; i < n; i++) { vk_key k = old[i]->k; nvn_hm_del(&g_vk, &k); vk_free(old[i]); }
}
static int vk_edges_same(const vk_ent *e) {
    uint64_t head = e->pn ? e->plo - e->lo : e->n, tail = e->pn ? e->lo + e->n - (e->plo + e->pn) : 0;
    return !memcmp(e->edge, (const void *)(uintptr_t)e->lo, (size_t)head) && !memcmp(e->edge + head, (const void *)(uintptr_t)(e->lo + e->n - tail), (size_t)tail);
}
static void vconv_fill(int f, uint64_t src, uint32_t stride, uint64_t nv, float *d);
// 64 samples of the source: a key whose samples differ from its last sighting's is being rewritten, and is not kept (a change the samples miss is found once it is kept)
static uint64_t vk_sig(uint64_t src, uint64_t sn) {
    uint64_t h = sn, step = sn > 8 ? (sn - 8) / 63 : 0;
    if (sn < 8) { uint64_t w = 0; memcpy(&w, (const void *)(uintptr_t)src, (size_t)sn); return w + 1; }
    for (int i = 0; i < 64; i++) { uint64_t w; memcpy(&w, (const void *)(uintptr_t)(src + i * step), 8); h = (h ^ w) * 0x9E3779B97F4A7C15ull; h ^= h >> 29; }
    return h;
}
// The kept conversion of k (nv elements of es bytes from src, then zeros up to nt), made when it is due; 0 = none: the caller converts into the frame arena
static uint64_t vk_get(const vk_key *k, uint64_t src, uint64_t es) {
    int mn = vk_min();
    if (!mn || k->nt < (uint64_t)mn || !k->nv || !k->stride) return 0;
    vk_ent *e = nvn_hm_get(&g_vk, k);
    if (!e) {
        if (g_vk.n >= VK_ENTS) return 0;   // the sweep makes room
        e = calloc(1, sizeof *e); e->k = *k; e->used = g_sub_serial; e->retry = g_sub_serial + 1; e->sig = vk_sig(src, (k->nv - 1) * k->stride + es); nvn_hm_put(&g_vk, k, e); return 0;
    }
    e->used = g_sub_serial;
    if (e->buf) {
        if (e->ok == g_sub_serial) { g_vk_reused += k->nv; return e->gpu; }
        uint64_t now = nvn_dirty_now();
        int pages = e->pn && now != e->seen && nvn_dirty_since_span(e->span, (const void *)(uintptr_t)e->plo, (size_t)e->pn, e->stamp);
        if (!pages && vk_edges_same(e)) {
            e->seen = now; e->ok = g_sub_serial; g_vk_reused += k->nv;
            if (e->strikes && g_sub_serial - e->kept > VK_IDLE) e->strikes = 0;
            return e->gpu;
        }
        vk_drop(e, 1, pages ? 0 : 1); e->sig = vk_sig(e->lo, e->n); return 0;
    }
    uint64_t sn = (k->nv - 1) * k->stride + es, bytes = k->nt * 16, sig = vk_sig(src, sn); int changed = sig != e->sig; e->sig = sig;
    if (changed && g_sub_serial >= e->retry) { vk_drop(e, 1, 0); return 0; }   // rewritten since its last sighting
    if (g_sub_serial < e->retry || g_vk_n >= VK_MAX || g_vk_bytes + bytes > VK_BYTES) return 0;
    if (nvn_sparse_gen() || pend_hits(src, sn) || g_sbw_over || sbw_hits(src, src + sn) || gw_live(src, src + sn)) { vk_drop(e, 1, 2); return 0; }   // the GPU writes it, or may
    const void *span = nvn_dirty_span((const void *)(uintptr_t)src, (size_t)sn);
    if (!span) { e->retry = UINT64_MAX; return 0; }   // not in a tracked memory pool
    id<MTLBuffer> b = [g_dev newBufferWithLength:(NSUInteger)bytes options:MTLResourceStorageModeShared];
    if (!b) return 0;
    uint64_t pg = (uint64_t)getpagesize(), plo = (src + pg - 1) & ~(pg - 1), phi = (src + sn) & ~(pg - 1), pn = phi > plo ? phi - plo : 0;
    e->lo = src; e->n = sn; e->plo = plo; e->pn = pn; e->span = span;
    e->stamp = e->seen = nvn_dirty_now(); if (pn) nvn_dirty_protect((const void *)(uintptr_t)plo, (size_t)pn);   // take the stamp, protect, then read (as ix_range)
    uint64_t head = pn ? plo - src : sn, tail = pn ? src + sn - phi : 0;
    e->edge = malloc((size_t)(head + tail) + 1); memcpy(e->edge, (const void *)(uintptr_t)src, (size_t)head); memcpy(e->edge + head, (const void *)(uintptr_t)(src + sn - tail), (size_t)tail);
    float *d = (float *)b.contents;
    vconv_fill((int)k->fmt, src, k->stride, k->nv, d);
    if (k->nt > k->nv) { static const uint8_t zeros[16]; vconv_fill((int)k->fmt, (uint64_t)(uintptr_t)zeros, 0, k->nt - k->nv, d + 4 * k->nv); }
    // the copy was taken before the conversion read the same bytes: a write between the two is found now, and one after it at the next submission
    if (!vk_edges_same(e)) { free(e->edge); e->edge = NULL; e->retry = g_sub_serial + 1; return 0; }
    res_add(b); e->gpu = b.gpuAddress; e->bytes = bytes; e->buf = (__bridge_retained void *)b; e->ok = e->kept = g_sub_serial;
    e->li = g_vk_n; g_vk_list[g_vk_n++] = e; g_vk_bytes += bytes; g_vk_made++;
    if (src < g_vk_lo) g_vk_lo = src; if (src + sn > g_vk_hi) g_vk_hi = src + sn;
    return e->gpu;
}
static void vc_stat(void) {   // every 3600 submissions
    if (g_sub_serial - g_vc_stat < 3600) return;
    if (g_vc_stat) MLOG("vertex conversion: %.0f vertices per submission (the bound ranges: %.0f; from kept conversions %.0f more: %d kept, %llu MB, %llu made, %llu dropped: %llu by a write, %llu by a write to a shared page, %llu by the GPU, %llu not drawn), %u index ranges kept%s; shadow-input libraries loaded %llu (not usable %llu), pipelines built with them %llu", g_vc_conv / 3600.0, g_vc_whole / 3600.0, g_vk_reused / 3600.0, g_vk_n, (unsigned long long)(g_vk_bytes >> 20), (unsigned long long)g_vk_made, (unsigned long long)g_vk_dropped, (unsigned long long)g_vk_why[0], (unsigned long long)g_vk_why[1], (unsigned long long)g_vk_why[2], (unsigned long long)g_vk_why[3], g_ir.n,
        vc_check() ? [NSString stringWithFormat:@"; checked %llu, %llu differed", (unsigned long long)g_vc_checks, (unsigned long long)g_vc_bad].UTF8String : "",
        (unsigned long long)g_vin_loaded, (unsigned long long)g_vin_failed, (unsigned long long)g_vin_psos);
    g_vc_stat = g_sub_serial; g_vc_conv = g_vc_whole = g_vk_reused = 0;
}
static void vconv_note(int f, int ct) {   // once per kind of conversion
    int ib, is;
    if (vconv_i2f(f, &ib, &is)) { static int once; if (!once++) MLOG("vertex attribute format %#x (integers read as floats, which Metal has no vertex format for): converted on the CPU at replay and bound separately", f); }
    else if (ct == NVN_CT_FLOAT) RED_ONCE(99, "vertex attribute format %#x (8 / 16-bit integer) feeds a float input: the 32-bit register values Maxwell's fetch makes are bound as float bit patterns, converted on the CPU at replay", f);
    else if (vconv_ncomp(f)) RED_ONCE(89, "vertex attribute format %#x feeds an integer input: converted to float32 bit patterns on the CPU at replay and bound separately (the conversion Maxwell does on vertex fetch)", f);
    else RED_ONCE(96, "vertex attribute format %#x (8 / 16-bit integer) feeds an integer input of the opposite signedness: extended to 32 bits with the format's own signedness on the CPU at replay and bound separately", f);
}
// Per submission, one conversion per (range, format, layout): KTGL draws the same mesh several times a frame (depth, shadow, G-buffer)
static struct { uint64_t addr, size, serial, v0, nv, nt; uint32_t fmt, off, stride; uint64_t gpu; } g_vconv[64]; static int g_vconv_next;
static void vconv_bind(id<MTL4RenderCommandEncoder> en, NvShader *vs, int plain, const p_draw *dw, const p_draw_idx *di) {   // plain: no shadow inputs (vin_on)
    const nvn_refl *rf = &vs->rf; int slot = VCONV_SLOT0, vc = vc_on(), rk = -1, on = vin_on(vs, NULL, plain); int64_t rlo = 0, rhi = 0;   // rk: the draw's vertex range is known (-1 = not looked up yet)
    vc_stat(); vk_sweep();
    for (int v = 0; v < rf->nv && slot < VCONV_SLOT0 + VCONV_MAX; v++) {
        if (!vconv_wanted(rf, v, NULL, on) || rf->v[v].nvn >= 32 || rf->v[v].nvn >= S.nattr) continue;   // S.attr holds 32
        int a = rf->v[v].nvn, st = S.attr[a].stream, f = S.attr[a].fmt; uint32_t off = S.attr[a].offset, stride = st < 16 ? S.stream[st].stride : 0;
        uint64_t addr = st < 16 ? S.vbuf[st].address : 0, size = st < 16 ? S.vbuf[st].size : 0, gpu = 0;
        uint64_t es = (uint64_t)vconv_size(f), v0 = 0, nv = size >= off + es ? stride == 0 ? 1 : (size - off - es) / stride + 1 : 0, whole = nv;
        if (vc && nv && stride && !S.stream[st].divisor) {   // per-vertex stream: only the vertices the draw reads
            if (rk < 0) rk = vc_draw_range(dw, di, &rlo, &rhi);
            int64_t a0 = rlo < 0 ? 0 : rlo, a1 = rhi < (int64_t)nv - 1 ? rhi : (int64_t)nv - 1;
            if (rk && a0 <= a1) { v0 = (uint64_t)a0; nv = (uint64_t)(a1 - a0 + 1); }
        }
        // Elements past the bound range read as zeros on Maxwell (vbuf_need): the conversion goes on to the last one the draw reads with the conversion of zero bytes
        uint64_t last, nt = nv; int lk = vstream_last(st, dw, di, &last);
        if (!lk && rk > 0 && stride && !S.stream[st].divisor && rhi >= 0) { last = (uint64_t)rhi; lk = 1; }
        if (lk && last + 1 > v0 + nv) nt = last + 1 - v0;
        for (int i = 0; i < 64 && !gpu; i++) if (g_vconv[i].serial == g_sub_serial && g_vconv[i].addr == addr && g_vconv[i].size == size && g_vconv[i].v0 == v0 && g_vconv[i].nv == nv && g_vconv[i].nt == nt && g_vconv[i].fmt == (uint32_t)f && g_vconv[i].off == off && g_vconv[i].stride == stride) gpu = g_vconv[i].gpu;
        if (!gpu && addr && nt && nt * 16 <= (64u << 20)) {
            vk_key kk = {addr, size, v0, nv, nt, (uint32_t)f, off, stride, 0};
            if ((gpu = vk_get(&kk, addr + off + v0 * stride, es))) { g_vc_whole += whole; vconv_note(f, rf->v[v].ct); }
        }
        if (!gpu && addr) {
            // 64 MiB, 4,194,304 vertices: at 4 MiB a longer range stayed unconverted (red line 90) and a stadium's 915,138-vertex stream of 16-bit integers read as floats drew stretched
            if (nt && nt * 16 <= (64u << 20)) {
                id<MTLBuffer> b; size_t o = arena_alloc((size_t)nt * 16, &b); float *d = (float *)((uint8_t *)b.contents + o);
                if (nv) vconv_fill(f, addr + off + v0 * stride, stride, nv, d);
                if (nt > nv) { static const uint8_t zeros[16]; vconv_fill(f, (uint64_t)(uintptr_t)zeros, 0, nt - nv, d + 4 * nv); }
                gpu = b.gpuAddress + o;
                g_vc_conv += nv; g_vc_whole += whole;
                int c = g_vconv_next++ % 64; g_vconv[c].addr = addr; g_vconv[c].size = size; g_vconv[c].v0 = v0; g_vconv[c].nv = nv; g_vconv[c].nt = nt; g_vconv[c].serial = g_sub_serial; g_vconv[c].fmt = (uint32_t)f; g_vconv[c].off = off; g_vconv[c].stride = stride; g_vconv[c].gpu = gpu;
                vconv_note(f, rf->v[v].ct);
            } else if (nt) RED_ONCE(90, "vertex attribute conversion: a range of %llu vertices exceeds the per-frame staging (format %#x)", (unsigned long long)nt, f);
        }
        if (gpu) enc_r_vbuf(en, (NSUInteger)slot, gpu - v0 * 16);   // vertex id v0 reads the first converted element
        slot++;
    }
}
static MTLVertexFormat vfmt(int f) {
    switch (f) {
        case 0x01: return MTLVertexFormatUCharNormalized; case 0x02: return MTLVertexFormatCharNormalized; case 0x03: return MTLVertexFormatUChar; case 0x04: return MTLVertexFormatChar;
        case 0x05: return MTLVertexFormatHalf; case 0x06: return MTLVertexFormatUShortNormalized; case 0x07: return MTLVertexFormatShortNormalized;
        case 0x08: return MTLVertexFormatUShort; case 0x09: return MTLVertexFormatShort; case 0x0A: return MTLVertexFormatFloat; case 0x0B: return MTLVertexFormatUInt; case 0x0C: return MTLVertexFormatInt;
        case 0x0D: return MTLVertexFormatUChar2Normalized; case 0x0E: return MTLVertexFormatChar2Normalized; case 0x0F: return MTLVertexFormatUChar2; case 0x10: return MTLVertexFormatChar2;
        case 0x11: return MTLVertexFormatHalf2; case 0x12: return MTLVertexFormatUShort2Normalized; case 0x13: return MTLVertexFormatShort2Normalized; case 0x14: return MTLVertexFormatUShort2;
        case 0x15: return MTLVertexFormatShort2; case 0x16: return MTLVertexFormatFloat2; case 0x17: return MTLVertexFormatUInt2; case 0x18: return MTLVertexFormatInt2;
        case 0x19: return MTLVertexFormatUChar3Normalized; case 0x1A: return MTLVertexFormatChar3Normalized; case 0x1B: return MTLVertexFormatUChar3; case 0x1C: return MTLVertexFormatChar3;
        case 0x1D: return MTLVertexFormatHalf3; case 0x1E: return MTLVertexFormatUShort3Normalized; case 0x1F: return MTLVertexFormatShort3Normalized; case 0x20: return MTLVertexFormatUShort3;
        case 0x21: return MTLVertexFormatShort3; case 0x22: return MTLVertexFormatFloat3; case 0x23: return MTLVertexFormatUInt3; case 0x24: return MTLVertexFormatInt3;
        case 0x25: return MTLVertexFormatUChar4Normalized; case 0x26: return MTLVertexFormatChar4Normalized; case 0x27: return MTLVertexFormatUChar4; case 0x28: return MTLVertexFormatChar4;
        case 0x29: return MTLVertexFormatHalf4; case 0x2A: return MTLVertexFormatUShort4Normalized; case 0x2B: return MTLVertexFormatShort4Normalized; case 0x2C: return MTLVertexFormatUShort4;
        case 0x2D: return MTLVertexFormatShort4; case 0x2E: return MTLVertexFormatFloat4; case 0x2F: return MTLVertexFormatUInt4; case 0x30: return MTLVertexFormatInt4;
        case 0x3D: return MTLVertexFormatUInt1010102Normalized; case 0x69: return MTLVertexFormatInt1010102Normalized; case 0x3F: return MTLVertexFormatFloatRG11B10; case 0x9A: return MTLVertexFormatUChar4Normalized_BGRA;
    }
    int ib, is, in = vconv_i2f(f, &ib, &is);   // integers read as floats: the integer format of the same layout (the pipeline reads vconv_bind's conversion)
    if (in) {
        static const MTLVertexFormat U[3][4] = { { MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4 }, { MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4 }, { MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4 } };
        static const MTLVertexFormat I[3][4] = { { MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4 }, { MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4 }, { MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4 } };
        return (is ? I : U)[ib == 8 ? 0 : ib == 16 ? 1 : 2][in - 1];
    }
    return MTLVertexFormatInvalid;
}
static MTLBlendFactor bfac(int f) {
    switch (f) { case 1: return MTLBlendFactorZero; case 2: return MTLBlendFactorOne; case 3: return MTLBlendFactorSourceColor; case 4: return MTLBlendFactorOneMinusSourceColor;
        case 5: return MTLBlendFactorSourceAlpha; case 6: return MTLBlendFactorOneMinusSourceAlpha; case 7: return MTLBlendFactorDestinationAlpha; case 8: return MTLBlendFactorOneMinusDestinationAlpha;
        case 9: return MTLBlendFactorDestinationColor; case 10: return MTLBlendFactorOneMinusDestinationColor; case 11: return MTLBlendFactorSourceAlphaSaturated;
        case 0x10: return MTLBlendFactorSource1Color; case 0x11: return MTLBlendFactorOneMinusSource1Color; case 0x12: return MTLBlendFactorSource1Alpha; case 0x13: return MTLBlendFactorOneMinusSource1Alpha;
        case 0x61: return MTLBlendFactorBlendColor; case 0x62: return MTLBlendFactorOneMinusBlendColor; case 0x63: return MTLBlendFactorBlendAlpha; case 0x64: return MTLBlendFactorOneMinusBlendAlpha; }
    return MTLBlendFactorOne;
}
