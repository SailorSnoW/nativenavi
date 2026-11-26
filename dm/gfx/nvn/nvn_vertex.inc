// Vertex inputs: formats corrected for the shader's types, integer inputs, the pulling entry point, CPU conversions (vconv).
// Metal needs integer formats of the input's signedness for integer inputs and float formats for float inputs (docs/reference/rendering.md#integer-vertex-inputs).
static MTLVertexFormat vfmt_for_ct(MTLVertexFormat f, int ct, int nvn_fmt) {
    static const MTLVertexFormat U[] = { MTLVertexFormatUChar, MTLVertexFormatUChar2, MTLVertexFormatUChar3, MTLVertexFormatUChar4, MTLVertexFormatUShort, MTLVertexFormatUShort2, MTLVertexFormatUShort3, MTLVertexFormatUShort4, MTLVertexFormatUInt, MTLVertexFormatUInt2, MTLVertexFormatUInt3, MTLVertexFormatUInt4 };
    static const MTLVertexFormat I[] = { MTLVertexFormatChar, MTLVertexFormatChar2, MTLVertexFormatChar3, MTLVertexFormatChar4, MTLVertexFormatShort, MTLVertexFormatShort2, MTLVertexFormatShort3, MTLVertexFormatShort4, MTLVertexFormatInt, MTLVertexFormatInt2, MTLVertexFormatInt3, MTLVertexFormatInt4 };
    static const MTLVertexFormat F[] = { MTLVertexFormatFloat, MTLVertexFormatFloat2, MTLVertexFormatFloat3, MTLVertexFormatFloat4 };
    int iu = -1, ii = -1, iff = -1;
    for (int i = 0; i < 12; i++) { if (U[i] == f) iu = i; if (I[i] == f) ii = i; }
    for (int i = 0; i < 4; i++) if (F[i] == f) iff = i;
    if (ct == NVN_CT_UINT) {
        if (iu >= 0) return f;
        if (ii >= 0) return U[ii];
        if (iff >= 0) return U[8 + iff];
    }
    else if (ct == NVN_CT_INT) {
        if (ii >= 0) return f;
        if (iu >= 0) return I[iu];
        if (iff >= 0) return I[8 + iff];
    }
    else {   // 32-bit integers into a float input: the same bits as floats
        if (iu < 0 && ii < 0) return f;
        if ((iu >= 8 && iu < 12) || (ii >= 8 && ii < 12)) return F[(iu >= 8 ? iu : ii) - 8];
    }
    // Metal refuses the whole pipeline for an integer input fed a normalized, half or packed format: the bytes are read as integers of the same layout so the draw is kept.
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
// Integer inputs fed float-type formats: Maxwell's register holds the converted float32 bits, which Metal cannot give, so they are converted on the CPU
// (docs/reference/rendering.md#cpu-conversion-of-vertex-attributes-the-input-type-cannot-take). Slots 1-12 are unused by translated shaders.
enum { VCONV_SLOT0 = 1, VCONV_MAX = 12 };
static int vconv_ncomp(int f) {   // 0 = not one
    switch (f) {
        // Float1/2/3 read as UInt1/2/3 pad w with integer 1, so the fetch must supply float 1.0
        case 0x01: case 0x02: case 0x05: case 0x06: case 0x07: case 0x0A: return 1;
        case 0x0D: case 0x0E: case 0x11: case 0x12: case 0x13: case 0x16: return 2;
        case 0x19: case 0x1A: case 0x1D: case 0x1E: case 0x1F: case 0x22: case 0x3F: return 3;
        case 0x25: case 0x26: case 0x29: case 0x2A: case 0x2B: case 0x3D: case 0x69: case 0x9A: return 4;
    }
    return 0;
}
// Maxwell widens 8 / 16-bit components by the format's signedness; vfmt_for_ct's switch to the other sign would widen them the wrong way.
static int vconv_int(int f, int *bits, int *sgn) {   // 0 = not one
    static const uint8_t T[16] = { 0x03, 0x04, 0x08, 0x09, 0x0F, 0x10, 0x14, 0x15, 0x1B, 0x1C, 0x20, 0x21, 0x27, 0x28, 0x2C, 0x2D };   // per component count: UI8, I8, UI16, I16
    for (int i = 0; i < 16; i++) if (T[i] == f) { *bits = i & 2 ? 16 : 8; *sgn = i & 1; return i / 4 + 1; }
    return 0;
}
// *_UI2F / *_I2F (0x51-0x68): integers converted to float without normalizing (200 -> 200.0), which Metal has no format for.
static int vconv_i2f(int f, int *bits, int *sgn) {   // 0 = not one
    if (f < 0x51 || f > 0x68) return 0;
    int i = (f - 0x51) >> 1;
    *sgn = (f - 0x51) & 1;
    *bits = 8 << (i % 3);
    return i / 3 + 1;
}
static int vconv_size(int f) {
    int ib, is, in = vconv_int(f, &ib, &is);
    if (in) return in * ib / 8;
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
    if (in) {   // the missing components (0, 0, 0, 1) as integers
        uint32_t u[4] = { 0, 0, 0, 1 };
        for (int i = 0; i < in; i++) {
            if (ib == 8) u[i] = is ? (uint32_t)(int32_t)(int8_t)p[i] : p[i];
            else { uint16_t h; memcpy(&h, p + 2 * i, 2); u[i] = is ? (uint32_t)(int32_t)(int16_t)h : h; }
        }
        memcpy(o, u, sizeof u);
        return;
    }
    o[0] = o[1] = o[2] = 0; o[3] = 1;
    if ((in = vconv_i2f(f, &ib, &is))) {   // the missing components (0, 0, 0, 1.0)
        for (int i = 0; i < in; i++) {
            if (ib == 8) o[i] = is ? (float)(int8_t)p[i] : (float)p[i];
            else if (ib == 16) { uint16_t h; memcpy(&h, p + 2 * i, 2); o[i] = is ? (float)(int16_t)h : (float)h; }
            else { uint32_t w; memcpy(&w, p + 4 * i, 4); o[i] = is ? (float)(int32_t)w : (float)w; }
        }
        return;
    }
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
            else {
                uint32_t bits = i == 3 ? 2 : 10, v = w >> (10 * i) & ((1u << bits) - 1);
                if (f == 0x3D) o[i] = (float)v / (float)((1u << bits) - 1);
                else {
                    int32_t sv = (int32_t)(v << (32 - bits)) >> (32 - bits);
                    o[i] = fmaxf((float)sv / (float)((1u << (bits - 1)) - 1), -1.0f);
                }
            }
            break;
        }
    }
}
// docs/reference/rendering.md#shadow-vertex-inputs: <key>_bv declares each integer input again as float4 and as the other sign, so Metal's fetch gives Maxwell's register bits.
static int vin_cpu(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_VCONV_CPU");
        v = e && !strcmp(e, "1");
        if (v) MLOG("DM_NVN_VCONV_CPU=1: vertex attributes Metal's fetch cannot read are converted on the CPU (no shadow inputs, no pulling)");
    }
    return v;
}
// nil with *why when it is not usable
static id<MTLFunction> side_lib_entry(NvShader *vs, const char *suffix, NSString *name, id<MTLLibrary> *lib_out, NSError **e, const char **why) {
    size_t kl = strlen(vs->key);
    NSString *path = [NSString stringWithFormat:@"%@/%.*s%s.metallib", g_msl_dir, (int)(kl > 2 ? kl - 2 : kl), vs->key, suffix];
    id<MTLLibrary> lib = lib_from_file(path, e);
    id<MTLFunction> f = fn_load(lib, name, e);
    *why = !lib ? "no library" : !f ? "no entry point" : NULL;
    if (f && vs->rf.nb && [f newArgumentEncoderWithBufferIndex:vs->rf.abi].encodedLength != vs->arg_len) { *why = "its argument buffer differs from the base library's"; f = nil; }
    *lib_out = lib;
    return f;
}
static os_unfair_lock g_vin_lock = OS_UNFAIR_LOCK_INIT;
static uint64_t g_vin_loaded, g_vin_failed, g_vin_psos;
static id<MTLFunction> vin_fn(NvShader *vs) {   // a failure is final for the shader (it keeps the conversion)
    if (!vs->rf.has_vin || vin_cpu()) return nil;
    os_unfair_lock_lock(&g_vin_lock);
    if (!vs->vfn_state) {
        vs->vfn_state = 2;
        NSString *name = vs->vinentry ?: vs->ventry;
        NSError *e = nil;
        id<MTLLibrary> lib;
        const char *why;
        id<MTLFunction> f = side_lib_entry(vs, "_bv", name, &lib, &e, &why);
        if (f) { vs->vfn = f; vs->vfn_state = 1; g_vin_loaded++; }
        else if (g_vin_failed++ < 8) MLOG("[RED] shadow-input library of %s not usable (%s%s%s): its integer inputs keep the CPU conversion", vs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
    }
    id<MTLFunction> f = vs->vfn_state == 1 ? vs->vfn : nil;
    os_unfair_lock_unlock(&g_vin_lock);
    return f;
}
// a failure is final for the shader (its draws keep waiting for their pipelines)
static os_unfair_lock g_pull_lock = OS_UNFAIR_LOCK_INIT;
// pull_fn holds g_pull_lock around it; shader_prefetch_rest calls it on a shader no other thread has yet
static void pull_load(NvShader *vs) {
    vs->pfn_state = 2;
    NSError *e = nil;
    id<MTLLibrary> lib;
    const char *why;
    id<MTLFunction> f = side_lib_entry(vs, "_bp", vs->pentry ?: vs->ventry, &lib, &e, &why);
    if (f) {
        vs->pfn = f;
        vs->pfn_state = 1;
        NSString *wn = vs->wlentry ?: [NSString stringWithFormat:@"%@_wl", vs->ventry];
        vs->wlfn = fn_load(lib, wn, &e);
    } else {
        static _Atomic int n;
        if (n++ < 8) MLOG("[RED] pulling library of %s not usable (%s%s%s): its draws wait for their pipelines", vs->key, why, e ? ": " : "", e ? e.localizedDescription.UTF8String : "");
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
static int vin_ct(const nvn_refl *rf, int v, int f, int on) {
    int ct = rf->v[v].ct, ib, is;
    if (!on || ct == NVN_CT_FLOAT) return ct;
    int want = vconv_ncomp(f) ? NVN_CT_FLOAT : vconv_int(f, &ib, &is) ? (is ? NVN_CT_INT : NVN_CT_UINT) : ct;
    return want != ct && rf->v[v].sh[want] != 0xff ? want : ct;
}
// Asked by pso_desc and vconv_bind alike, so the converted attributes and the pipeline's buffer slots agree.
static int vin_on(NvShader *vs, const pso_key *k, int plain) {   // plain: no fragment stage, which keeps the conversion
    const nvn_refl *rf = &vs->rf;
    if (plain || !rf->has_vin || vin_cpu()) return 0;
    int need = 0;
    for (int v = 0; v < rf->nv && !need; v++) need = vin_ct(rf, v, vin_fmt(k, rf->v[v].nvn), 1) != rf->v[v].ct;
    return need && vin_fn(vs) != nil;
}
// A float input fed an 8 / 16-bit integer format takes 32-bit register values through a Float4 attribute, passed bit for bit.
static int vconv_wanted(const nvn_refl *rf, int v, const pso_key *k, int on) {   // on: it uses the shadow inputs
    int a = rf->v[v].nvn, ct = rf->v[v].ct, ib, is;
    if (a >= 32) return 0;
    int f = k ? pso_key_attr(k, a).fmt : S.attr[a].fmt;
    if (vconv_i2f(f, &ib, &is)) return 1;
    if (ct == NVN_CT_FLOAT) return vconv_int(f, &ib, &is) > 0;
    if (ct != NVN_CT_UINT && ct != NVN_CT_INT) return 0;
    if (vin_ct(rf, v, f, on) != ct) return 0;   // a shadow input takes it
    return vconv_ncomp(f) || (vconv_int(f, &ib, &is) && is != (ct == NVN_CT_INT));
}
// docs/reference/rendering.md#attributes-the-vertex-fetch-cannot-read-pulled
static int pull_conversion(NvShader *vs, const pso_key *k) {
    if (!vs->has_pull || vin_cpu()) return 0;
    int on = vin_on(vs, k, 0);
    for (int v = 0; v < vs->rf.nv; v++) if (vconv_wanted(&vs->rf, v, k, on)) return 1;
    return 0;
}
static void vconv_fill(int f, uint64_t src, uint32_t stride, uint64_t nv, float *d) {
    const uint8_t *p = (const uint8_t *)(uintptr_t)src;
    uint32_t *u = (uint32_t *)d;
    int ib, is, in = vconv_int(f, &ib, &is);
    if (in) {
        for (uint64_t i = 0; i < nv; i++, p += stride, u += 4) {
            u[0] = u[1] = u[2] = 0; u[3] = 1;
            for (int c = 0; c < in; c++) {
                if (ib == 8) u[c] = is ? (uint32_t)(int32_t)(int8_t)p[c] : p[c];
                else { uint16_t h; memcpy(&h, p + 2 * c, 2); u[c] = is ? (uint32_t)(int32_t)(int16_t)h : h; }
            }
        }
        return;
    }
    if (f == 0x05 || f == 0x11 || f == 0x1D || f == 0x29) {   // R16F ... RGBA16F
        int n = vconv_ncomp(f);
        for (uint64_t i = 0; i < nv; i++, p += stride, d += 4) {
            d[0] = d[1] = d[2] = 0; d[3] = 1;
            for (int c = 0; c < n; c++) { _Float16 h; memcpy(&h, p + 2 * c, 2); d[c] = (float)h; }
        }
        return;
    }
    for (uint64_t i = 0; i < nv; i++) vconv_elem(f, p + i * stride, d + 4 * i);
}
// Only the vertices each draw reads are converted. An index range is kept while its pages stay write-protected, and never trusted again once a GPU write is queued on them.
enum { VC_RGN = 1 << 16 };
typedef struct { uint64_t ibuf; int32_t count, itype; } ir_key;
typedef struct { uint32_t mn, mx; uint64_t stamp, seen, lo, n; const void *span; int gpuw; } ir_ent;   // gpuw: a GPU write was queued on the indices
static nvn_hmap g_ir = {.klen = sizeof(ir_key)};
static uint64_t g_vc_rgn[VC_RGN / 64];   // 1 MB regions holding a kept index range (hashed)
static uint64_t g_vc_stat, g_vc_conv, g_vc_whole, g_vc_checks, g_vc_bad;
static int vc_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_VCONV_RANGE");
        v = e && *e ? atoi(e) != 0 : DM_GAME_VCONV_RANGE;
        MLOG("vertex conversion of the draw's vertices only: %s", v ? "on" : "off");
    }
    return v;
}
static int vc_check(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_VCONV_CHECK");
        v = e && *e && atoi(e);
    }
    return v;
}
static inline uint32_t vc_rgn(uint64_t r) { return (uint32_t)((r * 0x9E3779B97F4A7C15ull) >> 48) & (VC_RGN - 1); }
static void vc_rgn_mark(uint64_t lo, uint64_t n) {
    for (uint64_t r = lo >> 20; r <= (lo + n - 1) >> 20; r++) {
        uint32_t b = vc_rgn(r);
        g_vc_rgn[b >> 6] |= 1ull << (b & 63);
    }
}
static int vc_rgn_hit(uint64_t lo, uint64_t n) {
    for (uint64_t r = lo >> 20; r <= (lo + n - 1) >> 20; r++) {
        uint32_t b = vc_rgn(r);
        if (g_vc_rgn[b >> 6] >> (b & 63) & 1) return 1;
    }
    return 0;
}
static void vc_gpu_write(uint64_t dst, uint64_t n) {
    vk_gpu_write(dst, n ? n : 1);
    if (!n || !g_ir.n || !vc_rgn_hit(dst, n)) return;
    for (uint32_t i = 0; i < g_ir.cap; i++) {
        ir_ent *e = g_ir.vals[i];
        if (e && dst < e->lo + e->n && dst + n > e->lo) e->gpuw = 1;
    }
}
static void ix_minmax(const void *p, int isz, int32_t count, uint32_t *mn, uint32_t *mx) {
    uint32_t a = UINT32_MAX, b = 0;
    if (isz == 1) for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint8_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    else if (isz == 2) for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint16_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    else for (int32_t i = 0; i < count; i++) { uint32_t x = ((const uint32_t *)p)[i]; if (x < a) a = x; if (x > b) b = x; }
    *mn = a; *mx = b;
}
// 0 = not known (the whole bound range is converted)
static int ix_range(const p_draw_idx *di, uint32_t *mn, uint32_t *mx) {
    int isz = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; uint64_t n = (uint64_t)di->count * (uint64_t)isz;
    if (di->ind || di->mdi_cnt || di->count <= 0 || !di->ibuf || pend_hits(di->ibuf, n)) return 0;
    const void *p = (const void *)(uintptr_t)di->ibuf;
    ir_key k = {di->ibuf, di->count, di->itype};
    ir_ent *e = nvn_hm_get(&g_ir, &k);
    uint64_t now = nvn_dirty_now();
    if (e && e->gpuw) return 0;
    if (e && (now == e->seen || !nvn_dirty_since_span(e->span, p, (size_t)n, e->stamp))) {
        e->seen = now; *mn = e->mn; *mx = e->mx;
        if (vc_check()) {
            uint32_t a, b;
            ix_minmax(p, isz, di->count, &a, &b);
            g_vc_checks++;
            if ((a != e->mn || b != e->mx) && g_vc_bad++ < 20) MLOG("[RED] vertex conversion: the kept index range of %#llx (%d indices) is %u-%u, the indices now %u-%u", (unsigned long long)di->ibuf, di->count, e->mn, e->mx, a, b);
        }
        return 1;
    }
    const void *span = nvn_dirty_span(p, (size_t)n);
    if (!span) return 0;
    if (!e) {
        if (g_ir.n >= 1 << 16) { nvn_hm_clear(&g_ir, free); memset(g_vc_rgn, 0, sizeof g_vc_rgn); }
        e = calloc(1, sizeof *e);
        nvn_hm_put(&g_ir, &k, e);
    }
    e->stamp = e->seen = nvn_dirty_now();   // take the stamp, protect, then read
    nvn_dirty_protect(p, (size_t)n);
    e->span = span; e->lo = di->ibuf; e->n = n;
    vc_rgn_mark(e->lo, e->n);
    ix_minmax(p, isz, di->count, &e->mn, &e->mx); *mn = e->mn; *mx = e->mx;
    return 1;
}
// 0 = not known
static int vc_draw_range(const p_draw *dw, const p_draw_idx *di, int64_t *lo, int64_t *hi) {
    if (dw) {
        if (dw->ind || dw->mdi_cnt || dw->count <= 0) return 0;
        *lo = dw->first; *hi = (int64_t)dw->first + dw->count - 1;
        return 1;
    }
    uint32_t mn, mx;
    if (!di || !ix_range(di, &mn, &mx)) return 0;
    *lo = (int64_t)mn + di->base_vertex; *hi = (int64_t)mx + di->base_vertex;
    return 1;
}
// Kept vertex conversions (docs/reference/rendering.md#kept-vertex-conversions): kept while the source's own pages are write-protected, the bytes it shares with other data
// match a copy, and no GPU write was queued on it. A source that changed is retried after 32 submissions, doubling after each change.
enum { VK_MAX = 1024, VK_ENTS = 8192, VK_IDLE = 600, VK_STRIKES = 10 };
static const uint64_t VK_BYTES = 256ull << 20;
typedef struct { uint64_t addr, size, v0, nv, nt; uint32_t fmt, off, stride, zero; } vk_key;
typedef struct { vk_key k; void *buf; uint64_t gpu, bytes, lo, n, plo, pn, stamp, seen, ok, used, kept, retry; const void *span; uint8_t *edge; uint64_t sig; uint32_t strikes; int li; } vk_ent;   // buf: retained; [plo, plo+pn): its protected pages; edge: a copy of the rest; ok: last found unchanged in; li: its place in g_vk_list
static nvn_hmap g_vk = {.klen = sizeof(vk_key)};
static vk_ent *g_vk_list[VK_MAX];
static int g_vk_n;
static uint64_t g_vk_bytes, g_vk_lo = UINT64_MAX, g_vk_hi, g_vk_swept, g_vk_reused, g_vk_made, g_vk_dropped, g_vk_why[4];
static int vk_min(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_VCONV_KEEP");
        v = e && *e ? atoi(e) : 4096;
        if (v < 0) v = 0;
        if (v == 1) v = 4096;
        if (!v) MLOG("DM_NVN_VCONV_KEEP=0: vertex conversions are made again every submission");
    }
    return v;
}
static void vk_drop(vk_ent *e, int strike, int why) {   // the entry stays for its next try
    if (e->buf) {
        g_vk_why[why]++;
        id<MTLBuffer> b = (__bridge_transfer id<MTLBuffer>)e->buf;
        e->buf = NULL;
        defer_obj_release(b);
        g_vk_bytes -= e->bytes;
        g_vk_list[e->li] = g_vk_list[--g_vk_n];
        g_vk_list[e->li]->li = e->li;
        g_vk_dropped++;
        if (!g_vk_n) { g_vk_lo = UINT64_MAX; g_vk_hi = 0; }
    }
    free(e->edge); e->edge = NULL;
    if (strike) {
        if (e->strikes < VK_STRIKES) e->strikes++;
        e->retry = g_sub_serial + (16ull << e->strikes);
    }
}
static void vk_gpu_write(uint64_t dst, uint64_t n) {
    if (!g_vk_n) return;
    if (!n) { while (g_vk_n) vk_drop(g_vk_list[0], 1, 2); return; }
    if (dst >= g_vk_hi || n > UINT64_MAX - dst || dst + n <= g_vk_lo) return;
    for (int i = g_vk_n; i-- > 0;) {
        vk_ent *e = g_vk_list[i];
        if (dst < e->lo + e->n && dst + n > e->lo) vk_drop(e, 1, 2);
    }
}
static void vk_free(void *v) { vk_ent *e = v; vk_drop(e, 0, 3); free(e); }
static void vk_sweep(void) {
    if (g_sub_serial - g_vk_swept < 64) return;
    g_vk_swept = g_sub_serial;
    static vk_ent *old[256]; int n = 0;   // a removal moves entries, so they are listed first
    for (uint32_t i = 0; i < g_vk.cap && n < 256; i++) {
        vk_ent *e = g_vk.vals[i];
        if (e && g_sub_serial - e->used > VK_IDLE) old[n++] = e;
    }
    for (int i = 0; i < n; i++) {
        vk_key k = old[i]->k;
        nvn_hm_del(&g_vk, &k);
        vk_free(old[i]);
    }
}
static int vk_edges_same(const vk_ent *e) {
    uint64_t head = e->pn ? e->plo - e->lo : e->n, tail = e->pn ? e->lo + e->n - (e->plo + e->pn) : 0;
    return !memcmp(e->edge, (const void *)(uintptr_t)e->lo, (size_t)head) && !memcmp(e->edge + head, (const void *)(uintptr_t)(e->lo + e->n - tail), (size_t)tail);
}
static void vconv_fill(int f, uint64_t src, uint32_t stride, uint64_t nv, float *d);
// a change the samples miss is found once it is kept
static uint64_t vk_sig(uint64_t src, uint64_t sn) {
    uint64_t h = sn, step = sn > 8 ? (sn - 8) / 63 : 0;
    if (sn < 8) {
        uint64_t w = 0;
        memcpy(&w, (const void *)(uintptr_t)src, (size_t)sn);
        return w + 1;
    }
    for (int i = 0; i < 64; i++) {
        uint64_t w;
        memcpy(&w, (const void *)(uintptr_t)(src + i * step), 8);
        h = (h ^ w) * 0x9E3779B97F4A7C15ull;
        h ^= h >> 29;
    }
    return h;
}
// 0 = none: the caller converts into the frame arena
static uint64_t vk_get(const vk_key *k, uint64_t src, uint64_t es) {
    int mn = vk_min();
    if (!mn || k->nt < (uint64_t)mn || !k->nv || !k->stride) return 0;
    vk_ent *e = nvn_hm_get(&g_vk, k);
    if (!e) {
        if (g_vk.n >= VK_ENTS) return 0;
        e = calloc(1, sizeof *e);
        e->k = *k; e->used = g_sub_serial; e->retry = g_sub_serial + 1; e->sig = vk_sig(src, (k->nv - 1) * k->stride + es);
        nvn_hm_put(&g_vk, k, e);
        return 0;
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
        vk_drop(e, 1, pages ? 0 : 1);
        e->sig = vk_sig(e->lo, e->n);
        return 0;
    }
    uint64_t sn = (k->nv - 1) * k->stride + es, bytes = k->nt * 16, sig = vk_sig(src, sn);
    int changed = sig != e->sig;
    e->sig = sig;
    if (changed && g_sub_serial >= e->retry) { vk_drop(e, 1, 0); return 0; }
    if (g_sub_serial < e->retry || g_vk_n >= VK_MAX || g_vk_bytes + bytes > VK_BYTES) return 0;
    if (nvn_sparse_gen() || pend_hits(src, sn) || g_sbw_over || sbw_hits(src, src + sn) || gw_live(src, src + sn)) {
        vk_drop(e, 1, 2);
        return 0;
    }
    const void *span = nvn_dirty_span((const void *)(uintptr_t)src, (size_t)sn);
    if (!span) { e->retry = UINT64_MAX; return 0; }
    id<MTLBuffer> b = [g_dev newBufferWithLength:(NSUInteger)bytes options:MTLResourceStorageModeShared];
    if (!b) return 0;
    uint64_t pg = (uint64_t)getpagesize(), plo = (src + pg - 1) & ~(pg - 1), phi = (src + sn) & ~(pg - 1), pn = phi > plo ? phi - plo : 0;
    e->lo = src; e->n = sn; e->plo = plo; e->pn = pn; e->span = span;
    e->stamp = e->seen = nvn_dirty_now();   // take the stamp, protect, then read
    if (pn) nvn_dirty_protect((const void *)(uintptr_t)plo, (size_t)pn);
    uint64_t head = pn ? plo - src : sn, tail = pn ? src + sn - phi : 0;
    e->edge = malloc((size_t)(head + tail) + 1);
    memcpy(e->edge, (const void *)(uintptr_t)src, (size_t)head);
    memcpy(e->edge + head, (const void *)(uintptr_t)(src + sn - tail), (size_t)tail);
    float *d = (float *)b.contents;
    vconv_fill((int)k->fmt, src, k->stride, k->nv, d);
    if (k->nt > k->nv) { static const uint8_t zeros[16]; vconv_fill((int)k->fmt, (uint64_t)(uintptr_t)zeros, 0, k->nt - k->nv, d + 4 * k->nv); }
    // the copy was taken before the conversion read the same bytes: a write between the two is found now
    if (!vk_edges_same(e)) {
        free(e->edge);
        e->edge = NULL;
        e->retry = g_sub_serial + 1;
        return 0;
    }
    res_add(b);
    e->gpu = b.gpuAddress; e->bytes = bytes; e->buf = (__bridge_retained void *)b; e->ok = e->kept = g_sub_serial;
    e->li = g_vk_n; g_vk_list[g_vk_n++] = e; g_vk_bytes += bytes; g_vk_made++;
    if (src < g_vk_lo) g_vk_lo = src;
    if (src + sn > g_vk_hi) g_vk_hi = src + sn;
    return e->gpu;
}
static void vc_stat(void) {
    if (g_sub_serial - g_vc_stat < 3600) return;
    if (g_vc_stat) MLOG("vertex conversion: %.0f vertices per submission (the bound ranges: %.0f; from kept conversions %.0f more: %d kept, %llu MB, %llu made, %llu dropped: %llu by a write, %llu by a write to a shared page, %llu by the GPU, %llu not drawn), %u index ranges kept%s; shadow-input libraries loaded %llu (not usable %llu), pipelines built with them %llu", g_vc_conv / 3600.0, g_vc_whole / 3600.0, g_vk_reused / 3600.0, g_vk_n, (unsigned long long)(g_vk_bytes >> 20), (unsigned long long)g_vk_made, (unsigned long long)g_vk_dropped, (unsigned long long)g_vk_why[0], (unsigned long long)g_vk_why[1], (unsigned long long)g_vk_why[2], (unsigned long long)g_vk_why[3], g_ir.n,
        vc_check() ? [NSString stringWithFormat:@"; checked %llu, %llu differed", (unsigned long long)g_vc_checks, (unsigned long long)g_vc_bad].UTF8String : "",
        (unsigned long long)g_vin_loaded, (unsigned long long)g_vin_failed, (unsigned long long)g_vin_psos);
    g_vc_stat = g_sub_serial; g_vc_conv = g_vc_whole = g_vk_reused = 0;
}
static void vconv_note(int f, int ct) {
    int ib, is;
    if (vconv_i2f(f, &ib, &is)) {
        static int once;
        if (!once++) MLOG("vertex attribute format %#x (integers read as floats, which Metal has no vertex format for): converted on the CPU at replay and bound separately", f);
    }
    else if (ct == NVN_CT_FLOAT) RED_ONCE(99, "vertex attribute format %#x (8 / 16-bit integer) feeds a float input: the 32-bit register values Maxwell's fetch makes are bound as float bit patterns, converted on the CPU at replay", f);
    else if (vconv_ncomp(f)) RED_ONCE(89, "vertex attribute format %#x feeds an integer input: converted to float32 bit patterns on the CPU at replay and bound separately (the conversion Maxwell does on vertex fetch)", f);
    else RED_ONCE(96, "vertex attribute format %#x (8 / 16-bit integer) feeds an integer input of the opposite signedness: extended to 32 bits with the format's own signedness on the CPU at replay and bound separately", f);
}
// a mesh is often drawn several times a frame
static struct { uint64_t addr, size, serial, v0, nv, nt; uint32_t fmt, off, stride; uint64_t gpu; } g_vconv[64];
static int g_vconv_next;
static void vconv_bind(id<MTL4RenderCommandEncoder> en, NvShader *vs, int plain, const p_draw *dw, const p_draw_idx *di) {
    const nvn_refl *rf = &vs->rf;
    int slot = VCONV_SLOT0, vc = vc_on(), rk = -1, on = vin_on(vs, NULL, plain);   // rk: -1 = not looked up yet
    int64_t rlo = 0, rhi = 0;
    vc_stat(); vk_sweep();
    for (int v = 0; v < rf->nv && slot < VCONV_SLOT0 + VCONV_MAX; v++) {
        if (!vconv_wanted(rf, v, NULL, on) || rf->v[v].nvn >= 32 || rf->v[v].nvn >= S.nattr) continue;   // S.attr holds 32
        int a = rf->v[v].nvn, st = S.attr[a].stream, f = S.attr[a].fmt;
        uint32_t off = S.attr[a].offset, stride = st < 16 ? S.stream[st].stride : 0;
        uint64_t addr = st < 16 ? S.vbuf[st].address : 0, size = st < 16 ? S.vbuf[st].size : 0, gpu = 0;
        uint64_t es = (uint64_t)vconv_size(f), v0 = 0, nv = size >= off + es ? stride == 0 ? 1 : (size - off - es) / stride + 1 : 0, whole = nv;
        if (vc && nv && stride && !S.stream[st].divisor) {
            if (rk < 0) rk = vc_draw_range(dw, di, &rlo, &rhi);
            int64_t a0 = rlo < 0 ? 0 : rlo, a1 = rhi < (int64_t)nv - 1 ? rhi : (int64_t)nv - 1;
            if (rk && a0 <= a1) { v0 = (uint64_t)a0; nv = (uint64_t)(a1 - a0 + 1); }
        }
        // elements past the bound range read as zeros on Maxwell
        uint64_t last, nt = nv; int lk = vstream_last(st, dw, di, &last);
        if (!lk && rk > 0 && stride && !S.stream[st].divisor && rhi >= 0) { last = (uint64_t)rhi; lk = 1; }
        if (lk && last + 1 > v0 + nv) nt = last + 1 - v0;
        for (int i = 0; i < 64 && !gpu; i++)
            if (g_vconv[i].serial == g_sub_serial && g_vconv[i].addr == addr && g_vconv[i].size == size && g_vconv[i].v0 == v0 && g_vconv[i].nv == nv && g_vconv[i].nt == nt &&
                g_vconv[i].fmt == (uint32_t)f && g_vconv[i].off == off && g_vconv[i].stride == stride) gpu = g_vconv[i].gpu;
        if (!gpu && addr && nt && nt * 16 <= (64u << 20)) {
            vk_key kk = {addr, size, v0, nv, nt, (uint32_t)f, off, stride, 0};
            if ((gpu = vk_get(&kk, addr + off + v0 * stride, es))) { g_vc_whole += whole; vconv_note(f, rf->v[v].ct); }
        }
        if (!gpu && addr) {
            // a longer range stays unconverted (red line 90) and draws wrong
            if (nt && nt * 16 <= (64u << 20)) {
                id<MTLBuffer> b; size_t o = arena_alloc((size_t)nt * 16, &b); float *d = (float *)((uint8_t *)b.contents + o);
                if (nv) vconv_fill(f, addr + off + v0 * stride, stride, nv, d);
                if (nt > nv) { static const uint8_t zeros[16]; vconv_fill(f, (uint64_t)(uintptr_t)zeros, 0, nt - nv, d + 4 * nv); }
                gpu = b.gpuAddress + o;
                g_vc_conv += nv; g_vc_whole += whole;
                int c = g_vconv_next++ % 64;
                g_vconv[c].addr = addr; g_vconv[c].size = size; g_vconv[c].v0 = v0; g_vconv[c].nv = nv; g_vconv[c].nt = nt;
                g_vconv[c].serial = g_sub_serial; g_vconv[c].fmt = (uint32_t)f; g_vconv[c].off = off; g_vconv[c].stride = stride; g_vconv[c].gpu = gpu;
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
    int ib, is, in = vconv_i2f(f, &ib, &is);
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
