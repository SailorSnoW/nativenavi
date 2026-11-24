// Block-linear textures deswizzled on the GPU
#include <metal_stdlib>
using namespace metal;
struct P { ulong src_off, dst_off, base, pitch; uint row_bytes, rows, slices, th, td, gobs_x, gob2; };   // gob2: the Switch 2's order inside a GOB
kernel void deswz(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]], constant P& p [[buffer(2)]], uint3 id [[thread_position_in_grid]]) {
 uint bh = 1u << p.th, bd = 1u << p.td, gobs_x = p.gobs_x, blocks_y = (p.rows + (8u * bh) - 1u) >> (3u + p.th);
 uint tid = id.x & 31u, gob_x = id.x >> 5, gob_y = id.y, z = id.z;
 uint xseg = p.gob2 ? ((tid >> 4) << 1) | ((tid >> 2) & 1u) : ((tid >> 4) << 1) | ((tid >> 1) & 1u),
      y = (gob_y << 3) | (p.gob2 ? (((tid >> 3) & 1u) << 2) | (tid & 3u) : (((tid >> 2) & 3u) << 1) | (tid & 1u)),
      x = (gob_x << 6) | (xseg << 4);
 if (x >= p.row_bytes || y >= p.rows || z >= p.slices) return;
 uint n = min(16u, p.row_bytes - x);
 ulong block = 512ul * bh * bd;
 ulong gob_in_block = ((ulong)(z & (bd - 1u)) << p.th) + (gob_y & (bh - 1u));
 ulong so = p.src_off + p.base + ((ulong)(z >> p.td) * gobs_x * blocks_y + (gob_y >> p.th) * gobs_x + gob_x) * block + gob_in_block * 512ul + (ulong)tid * 16ul;
 ulong doff = p.dst_off + ((ulong)z * p.rows + y) * p.pitch + x;
 if (n == 16u && ((so | doff) & 15ul) == 0ul) *(device uint4 *)(dst + doff) = *(device const uint4 *)(src + so);
 else for (uint i = 0; i < n; i++) dst[doff + i] = src[so + i];
}
struct D { ulong src_off, base, layer_stride; uint width,height,layers,level,th,gobs_x,gob2; };
kernel void deswz_direct(device const uchar *src [[buffer(0)]], constant D& p [[buffer(2)]], texture2d_array<uint, access::write> dst [[texture(0)]], uint3 id [[thread_position_in_grid]]) {
 if(id.x>=p.width||id.y>=p.height||id.z>=p.layers)return;
 uint x=id.x*8u,y=id.y,bh=1u<<p.th,gy=y>>3;
 ulong off=p.src_off+p.base+ulong(id.z)*p.layer_stride+((ulong)(gy>>p.th)*p.gobs_x+(x>>6))*(512ul*bh)+(ulong)(gy&(bh-1u))*512ul;
 off+=p.gob2?((x&63u)>>5)*256u+((y&7u)>>2)*128u+((x&31u)>>4)*64u+((y&3u)>>1)*32u+(y&1u)*16u+(x&15u):((x&63u)>>5)*256u+((y&7u)>>1)*64u+((x&31u)>>4)*32u+(y&1u)*16u+(x&15u);
 dst.write(uint4(*(device const ushort4*)(src+off)),id.xy,id.z,p.level);
}
// Linear src into a level's storage, 16 bytes a thread: only bytes in [x0, x1) of block rows [y0, y0 + rows) are written, the rest keeps what it holds.
// lin != 0: linear storage, rows lin bytes apart; d3: depth planes from z0 interleaved by block depth td; pt != 0: a sparse pool's texture
struct W { ulong src_off, dst_off, base, pitch, img, layer_stride, e0n, e1s, e1n, pt, va, pt_pg; uint x0, x1, y0, rows, th, gobs_x, gob2, td, z0, blocks_y, d3, lin; };
// pt: GPU address of each 4 KB page from page pt_pg on, 0 where unmapped; such writes are dropped, as on the hardware
static device uchar *swz_at(device uchar *dst, constant W& p, ulong o) {
 if (!p.pt) return dst + o;
 ulong va = p.va + o - p.dst_off, a = reinterpret_cast<device const ulong *>(p.pt)[(va >> 12) - p.pt_pg];
 return a ? reinterpret_cast<device uchar *>(a + (va & 0xFFFul)) : nullptr;
}
kernel void swz(device const uchar *src [[buffer(0)]], device uchar *dst [[buffer(1)]], constant W& p [[buffer(2)]], device uchar *eb [[buffer(3)]], uint3 id [[thread_position_in_grid]]) {
 uint xs = (p.x0 & ~15u) + (id.x << 4), y = p.y0 + id.y;
 if (xs >= p.x1 || id.y >= p.rows) return;
 ulong o;
 if (p.lin) o = p.dst_off + p.base + (ulong)id.z * p.layer_stride + (ulong)y * p.lin + xs;
 else {
  uint bh = 1u << p.th, bd = 1u << p.td, gy = y >> 3, z = p.z0 + id.z;
  ulong block = 512ul * bh * bd;
  o = p.dst_off + p.base + (p.d3 ? (ulong)(z >> p.td) * p.gobs_x * p.blocks_y * block + (ulong)(z & (bd - 1u)) * bh * 512ul : (ulong)id.z * p.layer_stride)
    + ((ulong)(gy >> p.th) * p.gobs_x + (xs >> 6)) * block + (ulong)(gy & (bh - 1u)) * 512ul;
  o += p.gob2 ? ((xs & 63u) >> 5) * 256u + ((y & 7u) >> 2) * 128u + ((xs & 31u) >> 4) * 64u + ((y & 3u) >> 1) * 32u + (y & 1u) * 16u
              : ((xs & 63u) >> 5) * 256u + ((y & 7u) >> 1) * 64u + ((xs & 31u) >> 4) * 32u + (y & 1u) * 16u;
 }
 ulong s = p.src_off + (ulong)id.z * p.img + (ulong)id.y * p.pitch + (ulong)(id.x << 4);
 bool edge = o - p.dst_off < p.e0n || o - p.dst_off + 16ul > p.e1s;
 if (!edge && xs >= p.x0 && xs + 16u <= p.x1 && ((o | s) & 15ul) == 0ul) {   // 16 bytes in one 4 KB page (va is 16-byte aligned)
  device uchar *d = swz_at(dst, p, o); if (d) *(device uint4 *)d = *(device const uint4 *)(src + s); return; }
 for (uint i = 0; i < 16u; i++) if (xs + i >= p.x0 && xs + i < p.x1) {
  uchar v = src[s + i]; device uchar *d = swz_at(dst, p, o + i); if (d) *d = v;
  ulong q = o - p.dst_off + i;   // [0, e0n) and [e1s, e1s + e1n): edge pages, also copied to eb
  if (q < p.e0n) eb[q] = v; else if (q >= p.e1s && q - p.e1s < p.e1n) eb[p.e0n + q - p.e1s] = v;
 }
}
// Sparse texture gathered for an upload: runs after the command buffers committed before it, so it sees their GPU writes
struct G { ulong va, pt_pg, n; };
kernel void gather(device uint4 *dst [[buffer(0)]], device const ulong *pt [[buffer(1)]], constant G& p [[buffer(2)]], uint id [[thread_position_in_grid]]) {
 ulong q = (ulong)id << 4; if (q >= p.n) return;
 ulong va = p.va + q, a = pt[(va >> 12) - p.pt_pg];
 dst[id] = a ? *reinterpret_cast<device const uint4 *>(a + (va & 0xFFFul)) : uint4(0);
}
