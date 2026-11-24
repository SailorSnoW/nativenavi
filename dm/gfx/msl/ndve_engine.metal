// nn::ndve's movie network in place of its translated dispatches (DM_NDVE_ENGINE=1; docs/reference/shaders.md#the-ndve-movie-network-by-the-host): the translated shaders'
// arithmetic in their order, so the same bits. Tensors: f16 NHWC with a one-pixel zero border, (x, y, c) of W x H x C at ((y + 1) * (W + 2) + x + 1) * C + c, spare zero rows
// below; writers store only the interior. Safe math: the game's addition order, no FMA the game does not have
#pragma METAL fp math_mode(safe)
#include <metal_stdlib>
using namespace metal;

typedef simdgroup_half8x8 hmat;

struct LayerArgs { uint W, H; uint tiles_x, tiles_y; };   // pixels; tiles of 8 x RY

// Each 8 products summed in f32 and rounded once to f16, as Ampere does (shaders.md#hmma-rounds-its-f16-accumulator-once-per-8-products). A new matrix: rounding into acc's
// own thread_elements() is many times slower
static inline hmat hmma_round(simdgroup_float8x8 f) { hmat h; h.thread_elements()[0] = half(f.thread_elements()[0]); h.thread_elements()[1] = half(f.thread_elements()[1]); return h; }
static inline void hmma(thread hmat &acc, hmat a0, hmat a1, hmat b0, hmat b1, bool first) {
    simdgroup_float8x8 f;
    if (first) simdgroup_multiply(f, a0, b0);
    else simdgroup_multiply_accumulate(f, a0, b0, acc);
    const hmat c = hmma_round(f);
    simdgroup_multiply_accumulate(f, a1, b1, c);
    acc = hmma_round(f);
}
// lane -> (row, first of two columns) of an 8x8 simdgroup matrix (Apple7 layout, measured)
static inline ushort2 lane_rc(uint l) { return ushort2(((l >> 4) & 1) * 4 + ((l >> 1) & 3), ((l >> 3) & 1) * 4 + (l & 1) * 2); }
template <bool RELU>
static inline half2 epi(hmat acc, device const half *bias, int c) {
    float v0 = float(acc.thread_elements()[0]) + float(bias[c]), v1 = float(acc.thread_elements()[1]) + float(bias[c + 1]);
    if (RELU) { v0 = max(v0, 0.0f); v1 = max(v1, 0.0f); }
    return half2(half(v0), half(v1));
}

// KS = 3 (stride 1, padding 1) or 2 (stride 2); RY output rows per SIMD-group; w[t][k][n], t = KS dy + dx
template <int KS, int K, int N, int RY>
static inline bool conv_acc(thread hmat (&acc)[RY][N / 8], device const half *in, device const half *w, constant LayerArgs &a, uint tile, thread uint &x0, thread uint &y0) {
    const uint tx = tile % a.tiles_x, ty = tile / a.tiles_x;
    if (ty >= a.tiles_y) return false;
    x0 = tx * 8; y0 = ty * RY;
    const uint IW = KS == 3 ? a.W + 2 : a.W * 2 + 2;
    _Pragma("unroll") for (int t = 0; t < KS * KS; t++) {
        const int dy = t / KS, dx = t % KS;
        _Pragma("unroll") for (int kb = 0; kb < K; kb += 16) {
            hmat b0[N / 8], b1[N / 8];
            _Pragma("unroll") for (int q = 0; q < N / 8; q++) {
                device const half *wp = w + (t * K + kb) * N + q * 8;
                simdgroup_load(b0[q], wp, N); simdgroup_load(b1[q], wp + 8 * N, N);
            }
            _Pragma("unroll") for (int r = 0; r < RY; r++) {
                uint iy, ix; ulong st;
                if (KS == 3) { iy = y0 + r + dy; ix = x0 + dx; st = K; }
                else { iy = 2 * (y0 + r) + dy + 1; ix = 2 * x0 + dx + 1; st = 2 * K; }
                device const half *ap = in + ((ulong)iy * IW + ix) * K + kb;
                hmat a0, a1; simdgroup_load(a0, ap, st); simdgroup_load(a1, ap + 8, st);
                _Pragma("unroll") for (int q = 0; q < N / 8; q++) hmma(acc[r][q], a0, a1, b0[q], b1[q], t == 0 && kb == 0);
            }
        }
    }
    return true;
}
template <int KS, int K, int N, int RY>
static inline void conv_body(device const half *in, device half *out, device const half *w, device const half *bias, constant LayerArgs &a, uint tile, uint lane) {
    hmat acc[RY][N / 8]; uint x0, y0;
    if (!conv_acc<KS, K, N, RY>(acc, in, w, a, tile, x0, y0)) return;
    const ushort2 rc = lane_rc(lane);
    for (int r = 0; r < RY; r++) {
        if (y0 + r >= a.H) break;
        for (int q = 0; q < N / 8; q++) *(device half2 *)(out + ((ulong)(y0 + r + 1) * (a.W + 2) + x0 + rc.x + 1) * N + q * 8 + rc.y) = epi<true>(acc[r][q], bias, q * 8 + rc.y);
    }
}
#define CONV(name, KS, K, N, RY) \
kernel void name(device const half *in [[buffer(0)]], device half *out [[buffer(1)]], device const half *w [[buffer(2)]], device const half *b [[buffer(3)]], \
                 constant LayerArgs &a [[buffer(4)]], uint sg [[simdgroup_index_in_threadgroup]], uint nsg [[simdgroups_per_threadgroup]], \
                 uint tgi [[threadgroup_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) { conv_body<KS, K, N, RY>(in, out, w, b, a, tgi * nsg + sg, lane); }
CONV(down_16_32, 2, 16, 32, 2)
CONV(down_32_64, 2, 32, 64, 1)

// 3x3 with the input tile in threadgroup memory: 4 SIMD-groups share (RY + 2) x 34 x K inputs; conv_acc's operations in its order
template <int K, int N, int RY>
static inline void conv3s_body(device const half *in, device half *out, device const half *w, device const half *bias, constant LayerArgs &a,
                               uint tgi, uint sg, uint lane, uint tid, threadgroup half *st) {
    const uint tgx = (a.W + 31) / 32;
    const uint tx = tgi % tgx, ty = tgi / tgx;
    if (ty >= a.tiles_y) return;
    const uint X0 = tx * 32, y0 = ty * RY, IW = a.W + 2;
    constexpr uint SW = 34, ROWH = SW * K;
    for (int r = 0; r < RY + 2; r++) {
        device const half4 *sp = (device const half4 *)(in + ((ulong)(y0 + r) * IW + X0) * K);
        threadgroup half4 *tp = (threadgroup half4 *)(st + r * ROWH);
        for (uint i = tid; i < ROWH / 4; i += 128) tp[i] = sp[i];
    }
    threadgroup_barrier(mem_flags::mem_threadgroup);
    const uint x0 = X0 + sg * 8;
    if (x0 >= a.W) return;
    hmat acc[RY][N / 8];
    for (int t = 0; t < 9; t++) {
        const int dy = t / 3, dx = t % 3;
        for (int kb = 0; kb < K; kb += 16) {
            hmat b0[N / 8], b1[N / 8];
            for (int q = 0; q < N / 8; q++) {
                device const half *wp = w + (t * K + kb) * N + q * 8;
                simdgroup_load(b0[q], wp, N); simdgroup_load(b1[q], wp + 8 * N, N);
            }
            for (int r = 0; r < RY; r++) {
                threadgroup const half *ap = st + (r + dy) * ROWH + (sg * 8 + dx) * K + kb;
                hmat a0, a1; simdgroup_load(a0, ap, K); simdgroup_load(a1, ap + 8, K);
                for (int q = 0; q < N / 8; q++) hmma(acc[r][q], a0, a1, b0[q], b1[q], t == 0 && kb == 0);
            }
        }
    }
    const ushort2 rc = lane_rc(lane);
    for (int r = 0; r < RY; r++) {
        if (y0 + r >= a.H) break;
        for (int q = 0; q < N / 8; q++) *(device half2 *)(out + ((ulong)(y0 + r + 1) * IW + x0 + rc.x + 1) * N + q * 8 + rc.y) = epi<true>(acc[r][q], bias, q * 8 + rc.y);
    }
}
#define CONV3S(name, K, N, RY) \
kernel void name(device const half *in [[buffer(0)]], device half *out [[buffer(1)]], device const half *w [[buffer(2)]], device const half *b [[buffer(3)]], \
                 constant LayerArgs &a [[buffer(4)]], uint sg [[simdgroup_index_in_threadgroup]], uint tgi [[threadgroup_position_in_grid]], \
                 uint lane [[thread_index_in_simdgroup]], uint tid [[thread_index_in_threadgroup]]) { \
    threadgroup half st[(RY + 2) * 34 * K]; conv3s_body<K, N, RY>(in, out, w, b, a, tgi, sg, lane, tid, st); }
CONV3S(conv3s_16_16, 16, 16, 4)
CONV3S(conv3s_32_32, 32, 32, 2)
CONV3S(conv3s_64_64, 64, 64, 1)

// 2x2 stride-2 transposed convolution, the skip connection as the chains' C; a.W, a.H: the input's size
template <int K, int N, int RY>
static inline void up_body(device const half *in, device const half *skip, device half *out, device const half *w, device const half *bias, constant LayerArgs &a,
                           uint tile, uint lane) {
    const uint tx = tile % a.tiles_x, ty = tile / a.tiles_x;
    if (ty >= a.tiles_y) return;
    const uint x0 = tx * 8, y0 = ty * RY, IW = a.W + 2, OW = 2 * a.W + 2;
    const ushort2 rc = lane_rc(lane);
    _Pragma("unroll") for (int t = 0; t < 4; t++) {
        const int dy = t >> 1, dx = t & 1;
        hmat acc[RY][N / 8];
        _Pragma("unroll") for (int r = 0; r < RY; r++) for (int q = 0; q < N / 8; q++)
            simdgroup_load(acc[r][q], skip + ((ulong)(2 * (y0 + r) + dy + 1) * OW + 2 * x0 + dx + 1) * N + q * 8, 2 * N);
        _Pragma("unroll") for (int kb = 0; kb < K; kb += 16) {
            hmat b0[N / 8], b1[N / 8];
            _Pragma("unroll") for (int q = 0; q < N / 8; q++) {
                device const half *wp = w + (t * K + kb) * N + q * 8;
                simdgroup_load(b0[q], wp, N); simdgroup_load(b1[q], wp + 8 * N, N);
            }
            _Pragma("unroll") for (int r = 0; r < RY; r++) {
                device const half *ap = in + ((ulong)(y0 + r + 1) * IW + x0 + 1) * K + kb;
                hmat a0, a1; simdgroup_load(a0, ap, K); simdgroup_load(a1, ap + 8, K);
                _Pragma("unroll") for (int q = 0; q < N / 8; q++) hmma(acc[r][q], a0, a1, b0[q], b1[q], false);
            }
        }
        _Pragma("unroll") for (int r = 0; r < RY; r++) {
            if (y0 + r >= a.H) break;
            _Pragma("unroll") for (int q = 0; q < N / 8; q++)
                *(device half2 *)(out + ((ulong)(2 * (y0 + r) + dy + 1) * OW + 2 * (x0 + rc.x) + dx + 1) * N + q * 8 + rc.y) = epi<true>(acc[r][q], bias, q * 8 + rc.y);
        }
    }
}
#define UP(name, K, N, RY) \
kernel void name(device const half *in [[buffer(0)]], device half *out [[buffer(1)]], device const half *w [[buffer(2)]], device const half *b [[buffer(3)]], \
                 constant LayerArgs &a [[buffer(4)]], device const half *skip [[buffer(5)]], uint sg [[simdgroup_index_in_threadgroup]], uint nsg [[simdgroups_per_threadgroup]], \
                 uint tgi [[threadgroup_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) { up_body<K, N, RY>(in, skip, out, w, b, a, tgi * nsg + sg, lane); }
UP(up_64_32, 64, 32, 1)
UP(up_32_16, 32, 16, 2)

// Last layer: 3x3 16 -> 16, no ReLU, unpacked 2x2 onto the input picture: component c of sub-pixel s from channel (2s, 2s + 1, 8 + 2s, 9 + 2s)[c], (rgba + D) + bias in f32
kernel void out_16(device const half *in [[buffer(0)]], device const half *w [[buffer(2)]], device const half *bias [[buffer(3)]], constant LayerArgs &a [[buffer(4)]],
                   texture2d<float, access::read> rgba [[texture(0)]], texture2d<float, access::write> dst [[texture(1)]],
                   uint sg [[simdgroup_index_in_threadgroup]], uint nsg [[simdgroups_per_threadgroup]], uint tgi [[threadgroup_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) {
    constexpr int RY = 4;
    threadgroup half dd[4][RY][8][16];
    hmat acc[RY][2]; uint x0, y0;
    if (!conv_acc<3, 16, 16, RY>(acc, in, w, a, tgi * nsg + sg, x0, y0)) return;
    for (int r = 0; r < RY; r++) for (int q = 0; q < 2; q++) simdgroup_store(acc[r][q], &dd[sg][r][0][0], 16, ulong2(q * 8, 0));
    simdgroup_barrier(mem_flags::mem_threadgroup);
    for (int j = 0; j < 4; j++) {
        const uint idx = j * 32 + lane, sub = idx & 3, px = (idx >> 2) & 7, r = idx >> 5;
        if (y0 + r >= a.H) continue;
        const uint2 p = uint2(2 * (x0 + px) + (sub & 1), 2 * (y0 + r) + (sub >> 1));
        const float4 t = rgba.read(p);
        threadgroup const half *d = &dd[sg][r][px][0];
        const uint n0 = 2 * sub, n1 = 8 + 2 * sub;
        float4 v;
        v.x = (t.x + float(d[n0])) + float(bias[n0]); v.y = (t.y + float(d[n0 + 1])) + float(bias[n0 + 1]);
        v.z = (t.z + float(d[n1])) + float(bias[n1]); v.w = (t.w + float(d[n1 + 1])) + float(bias[n1 + 1]);
        dst.write(v, p);
    }
}

// Input: the RGBA8 frame packed 2x2 into 16 channels, channel 4 (2 sy + sx) + c, rounded to f16 as the game's first convolution loads it
kernel void pack_input(texture2d<float, access::read> img [[texture(0)]], device half *out [[buffer(1)]], constant LayerArgs &a [[buffer(4)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= a.W || g.y >= a.H) return;
    device half *op = out + ((ulong)(g.y + 1) * (a.W + 2) + g.x + 1) * 16;
    for (int s = 0; s < 4; s++) {
        float4 v = img.read(uint2(2 * g.x + (s & 1), 2 * g.y + (s >> 1)));
        *(device half4 *)(op + 4 * s) = half4(v);
    }
}

// Resampler 4f41ccf2, 1920x1080 to 2560x1440. rin: input rect; s2.zw: output pixel size in uv; c4.z / c4.w: vertical flip (row base, on); c5.x: dither
struct RsConst { int4 rin; int4 rout; float4 s2; float4 s3; int4 c4; int4 c5; };
kernel void rs_luma(texture2d<float, access::read> pic [[texture(0)]], device half *luma [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= 1920 || g.y >= 1080) return;
    const float3 c = pic.read(g).xyz;
    const float t = c.y * 0.715200006961822509765625f;
    luma[g.y * 1920 + g.x] = half(fma(c.z, 0.072200000286102294921875f, fma(c.x, 0.2125999927520751953125f, t)));
}
// One SIMD-group per 8x8 output block: the 16x16 luma window at 6 bx - 5, 6 by - 5 (clamped), six blocks, the delta added to a bilinear sample, dithered, rounded to 8 bits
static inline hmat relu_h(hmat a) { hmat h; h.thread_elements()[0] = half(max(float(a.thread_elements()[0]), 0.0f)); h.thread_elements()[1] = half(max(float(a.thread_elements()[1]), 0.0f)); return h; }
// fixed loop bounds keep the matrices in registers
static inline void rs_block(thread hmat (&X)[2][2], device const half *wb) {
    hmat H[2][2][2];
    _Pragma("unroll") for (int g = 0; g < 2; g++) {
        hmat A[2][2];
        _Pragma("unroll") for (int mi = 0; mi < 2; mi++) _Pragma("unroll") for (int ki = 0; ki < 2; ki++) simdgroup_load(A[mi][ki], wb + 256 * g + 128 * ki + 64 * mi, 8);
        _Pragma("unroll") for (int mi = 0; mi < 2; mi++) _Pragma("unroll") for (int ni = 0; ni < 2; ni++) hmma(H[g][mi][ni], A[mi][0], A[mi][1], X[0][ni], X[1][ni], true);
    }
    _Pragma("unroll") for (int mi = 0; mi < 2; mi++) _Pragma("unroll") for (int j = 0; j < 2; j++) {
        hmat acc;
        _Pragma("unroll") for (int g = 0; g < 2; g++) {
            hmat B0, B1; device const half *wp = wb + 512 + 256 * g + 128 * j;
            simdgroup_load(B0, wp, 8); simdgroup_load(B1, wp + 64, 8);
            hmma(acc, H[g][mi][0], H[g][mi][1], B0, B1, g == 0);
        }
        X[mi][j] = relu_h(acc);
    }
}
static inline hmat rs_last(thread hmat (&X)[2][2], device const half *wb) {
    hmat H[2][2];
    _Pragma("unroll") for (int g = 0; g < 2; g++) {
        hmat A0, A1; simdgroup_load(A0, wb + 256 * g, 8); simdgroup_load(A1, wb + 256 * g + 128, 8);
        _Pragma("unroll") for (int ni = 0; ni < 2; ni++) hmma(H[g][ni], A0, A1, X[0][ni], X[1][ni], true);
    }
    hmat acc;
    _Pragma("unroll") for (int g = 0; g < 2; g++) {
        hmat B0, B1; device const half *wp = wb + 512 + 256 * g;
        simdgroup_load(B0, wp, 8); simdgroup_load(B1, wp + 64, 8);
        hmma(acc, H[g][0], H[g][1], B0, B1, g == 0);
    }
    return acc;
}
kernel void resample(device const half *luma [[buffer(0)]], device const half *w [[buffer(2)]], constant RsConst &cc [[buffer(3)]],
                     texture2d<float, access::sample> pic [[texture(0)]], texture2d<float, access::read> dith [[texture(1)]], texture2d<float, access::write> dst [[texture(2)]],
                     uint sg [[simdgroup_index_in_threadgroup]], uint2 tgp [[threadgroup_position_in_grid]], uint lane [[thread_index_in_simdgroup]]) {
    constexpr sampler smp(coord::normalized, address::clamp_to_edge, filter::linear, min_filter::nearest, mag_filter::linear, mip_filter::linear);
    threadgroup half win[4][16 * 16];
    const uint bx = tgp.x * 4 + sg, by = tgp.y;
    for (uint i = lane; i < 256; i += 32) {
        const int r = int(i >> 4), c = int(i & 15);
        const int y = min(max(0, int(6 * by) - 5 + r), cc.rin.w), x = min(max(0, int(6 * bx) - 5 + c), cc.rin.z);
        win[sg][i] = luma[y * 1920 + x];
    }
    simdgroup_barrier(mem_flags::mem_threadgroup);
    hmat X[2][2];   // [k block][n block]
    for (int a = 0; a < 2; a++) for (int b = 0; b < 2; b++) simdgroup_load(X[a][b], &win[sg][0], 16, ulong2(8 * b, 8 * a));
    for (int blk = 0; blk < 5; blk++) rs_block(X, w + 1024 * blk);
    const hmat d6 = rs_last(X, w + 1024 * 5);
    const ushort2 rc = lane_rc(lane);
    for (int e = 0; e < 2; e++) {
        const uint x = 8 * bx + rc.y + e, y = 8 * by + rc.x;
        const float delta = float(d6.thread_elements()[e]);
        const float2 uv = float2((float(x) + 0.5f) * cc.s2.z, (float(y) + 0.5f) * cc.s2.w);
        const float3 s = pic.sample(smp, uv, level(0.0f)).xyz;
        const float3 dv = dith.read(uint2(x & 511, y & 511)).xyz;
        const float3 off = fma(dv, 2.0f, -1.0f);
        const float3 v = -0.0f + (s + delta);
        const float3 k = rint(fma(v, 255.0f, off));
        const uint yo = cc.c4.w ? uint(cc.c4.z - int(y)) : y;
        dst.write(float4(k * 0.0039215688593685626983642578125f, 1.0f), uint2(x, yo));
    }
}

// DM_NDVE_ENGINE=check: n[0] texels that differ, n[1] the largest component difference (1/1023 units)
kernel void cmp_img(texture2d<float, access::read> a [[texture(0)]], texture2d<float, access::read> b [[texture(1)]], device atomic_uint *n [[buffer(1)]], uint2 g [[thread_position_in_grid]]) {
    if (g.x >= a.get_width() || g.y >= a.get_height()) return;
    const float4 d = abs(a.read(g) - b.read(g));
    const float m = max(max(d.x, d.y), max(d.z, d.w));
    if (m != 0.0f) { atomic_fetch_add_explicit(&n[0], 1u, memory_order_relaxed); atomic_fetch_max_explicit(&n[1], uint(rint(m * 1023.0f)), memory_order_relaxed); }
}
