// Texture copies drawn as a render pass
#include <metal_stdlib>
using namespace metal;
struct V { float4 p [[position]]; };
vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); return o; }
// p[0]: source origin, source texels per destination texel; p[1]: destination origin, source size
fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; return t.sample(s, st / p[1].zw, level(0)); }
// p[2]: the input's texel-center bounds, so padding never contributes alpha
fragment float4 fsa(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; st = clamp(st, p[2].xy, p[2].zw); return float4(0, 0, 0, t.sample(s, st / p[1].zw, level(0)).a); }
fragment float4 fsq(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; float4 c = t.sample(s, st / p[1].zw, level(0)); return float4(c.rgb * abs(c.rgb), c.a); }
// p[2].x = t1's weight, p[2].zw = t1's coordinate per source texel
fragment float4 fsup(V in [[stage_in]], texture2d<float> t [[texture(0)]], texture2d<float> t1 [[texture(1)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; float4 c = t.sample(s, st / p[1].zw, level(0));
  if (p[2].x > 0) c = mix(c, t1.sample(s, (st - p[0].xy) * p[2].zw, level(0)), p[2].x);
  return c; }
struct D { float d [[depth(any)]]; };
fragment D fsd(V in [[stage_in]], depth2d<float> t [[texture(0)]], sampler s [[sampler(0)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; D o; o.d = t.read(uint2(st)); return o; }
struct DS { float d [[depth(any)]]; uint s [[stencil]]; };
fragment DS fsds(V in [[stage_in]], depth2d<float> t [[texture(0)]], texture2d<uint> ts [[texture(1)]], constant float4 *p [[buffer(0)]]) {
  float2 st = p[0].xy + (in.p.xy - p[1].xy) * p[0].zw; DS o; o.d = t.read(uint2(st)); o.s = ts.read(uint2(st)).r; return o; }
