// Tegra X1 block-linear (deko3d / Ryujinx BlockLinearLayout): GOB 64 B x 8 rows, (x, y) at (x%64/32)·256 + (y%8/2)·64 + (x%32/16)·32 + (y%2)·16 + x%16;
// Switch 2 (nvn_gob_order): (x%64/32)·256 + (y%8/4)·128 + (x%32/16)·64 + (y%4/2)·32 + (y%2)·16 + x%16. Block = 1 x 2^tileH x 2^tileD GOBs; blocks x → y → z
#include "nvn_deswizzle.h"
#include <string.h>

static uint32_t gob_off(uint32_t x, uint32_t y, int switch2) {
    if (switch2) return (x % 64 / 32) * 256 + (y % 8 / 4) * 128 + (x % 32 / 16) * 64 + (y % 4 / 2) * 32 + (y % 2) * 16 + x % 16;
    return (x % 64 / 32) * 256 + (y % 8 / 2) * 64 + (x % 32 / 16) * 32 + (y % 2) * 16 + x % 16;
}

uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned th, unsigned td, int switch2) {
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t gobs_x = (row_bytes + 63) / 64, blocks_y = (rows + 8 * bh - 1) / (8 * bh);
    uint64_t block = 512ull * bh * bd;
    return (z / bd) * gobs_x * blocks_y * block
         + (y / (8 * bh)) * gobs_x * block
         + (x / 64) * block
         + ((z % bd) * bh + (y % (8 * bh)) / 8) * 512ull
         + gob_off(x, y, switch2);
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }

// -1: unsupported texture
int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices, uint64_t *base, unsigned *th, unsigned *td, uint32_t *bpb) {
    if (d->target == 5 || d->target == 6 || d->target == 10 || (d->flags & 0x50) || d->samples > 1) return -1;   // MS / BUFFER / LINEAR / LINEAR_RENDER_TARGET
    int bw, bh;
    *bpb = (uint32_t)nvn_fmt_bpb(d->format, &bw, &bh);
    if (!*bpb || level >= (d->levels ? d->levels : 1)) return -1;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    if (layer >= L.layers) return -1;
    int one_d = d->target == 0 || d->target == 3;
    uint32_t w = mip(d->width, level), h = one_d ? 1 : mip(d->height, level);
    *slices = d->target == 2 ? mip(d->depth ? d->depth : 1, level) : 1;
    *row_bytes = (w + bw - 1) / bw * *bpb;
    *rows = (h + bh - 1) / bh;
    *base = (uint64_t)layer * L.layer_size + nvn_tex_level_offset(d, level);
    nvn_tex_level_tiles(d, level, th, td);
    return 0;
}

static int move(const nvn_texdesc *d, unsigned level, unsigned layer, uint8_t *bl, uint64_t bl_at, uint8_t *lin, size_t pitch, int to_linear) {
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb)) return -1;
    if (pitch < row_bytes || base < bl_at) return -1;
    bl += base - bl_at;
    uint32_t lb = nvn_tex_level_gobs(d, level) * 64;
    if (lb < row_bytes) lb = row_bytes;
    // nvn_bl_offset hoisted: at x = 16 j the block column is j >> 2 and the GOB offset one of four
    static const uint16_t kox1[4] = {0, 32, 256, 288};
    static const uint16_t koy1[8] = {0, 16, 64, 80, 128, 144, 192, 208};      // (y%8/2)·64 + (y%2)·16
    static const uint16_t kox2[4] = {0, 64, 256, 320};                        // Switch 2 order
    static const uint16_t koy2[8] = {0, 16, 32, 48, 128, 144, 160, 176};      // (y%8/4)·128 + (y%4/2)·32 + (y%2)·16
    const int s2 = nvn_gob_order(d);
    const uint16_t *kox = s2 ? kox2 : kox1, *koy = s2 ? koy2 : koy1;
    uint32_t bh = 1u << th, bd = 1u << td;
    uint64_t block = 512ull * bh * bd, gobs_x = (uint64_t)(lb + 63) >> 6, blocks_y = ((uint64_t)rows + 8 * bh - 1) >> (3 + th);
    for (uint32_t z = 0; z < slices; z++) {
        uint64_t zb = (uint64_t)(z >> td) * gobs_x * blocks_y * block + (uint64_t)(z & (bd - 1)) * bh * 512;
        for (uint32_t y = 0; y < rows; y++) {
            uint64_t p = (uint64_t)(y >> (3 + th)) * gobs_x * block + (uint64_t)((y & (8 * bh - 1)) >> 3) * 512 + koy[y & 7];
            uint8_t *row = lin + ((size_t)z * rows + y) * pitch;
            uint64_t yb = zb + p;
            uint32_t nfull = row_bytes >> 4, tail = row_bytes & 15;
            if (to_linear) {
                for (uint32_t j = 0; j < nfull; j++) memcpy(row + (j << 4), bl + yb + ((uint64_t)(j >> 2) * block) + kox[j & 3], 16);
                if (tail) memcpy(row + (nfull << 4), bl + yb + ((uint64_t)(nfull >> 2) * block) + kox[nfull & 3], tail);
            } else {
                for (uint32_t j = 0; j < nfull; j++) memcpy(bl + yb + ((uint64_t)(j >> 2) * block) + kox[j & 3], row + (j << 4), 16);
                if (tail) memcpy(bl + yb + ((uint64_t)(nfull >> 2) * block) + kox[nfull & 3], row + (nfull << 4), tail);
            }
        }
    }
    return 0;
}

int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, 0, (uint8_t *)dst, dst_pitch, 1);
}
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, size_t src_pitch, void *dst) {
    return move(d, level, layer, (uint8_t *)dst, 0, (uint8_t *)src, src_pitch, 0);
}

// The middle block of each step x step cell, nx x ny of them; only 1- to 16-byte blocks (one 16-byte piece each), else -1
int nvn_deswizzle_pick(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, uint32_t step, void *dst, uint32_t nx, uint32_t ny) {
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    if (!step || nvn_deswizzle_geom(d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || slices != 1 || bpb > 16 || (bpb & (bpb - 1))) return -1;
    uint32_t lb = nvn_tex_level_gobs(d, level) * 64, bx = row_bytes / bpb;
    if (lb < row_bytes) lb = row_bytes;
    const uint8_t *bl = (const uint8_t *)src + base;
    uint8_t *o = dst;
    for (uint32_t j = 0; j < ny; j++) {
        uint32_t y = j * step + step / 2;
        if (y >= rows) y = rows - 1;
        for (uint32_t i = 0; i < nx; i++, o += bpb) {
            uint32_t x = i * step + step / 2;
            if (x >= bx) x = bx - 1;
            memcpy(o, bl + nvn_bl_offset(x * bpb, y, 0, lb, rows, th, td, nvn_gob_order(d)), bpb);
        }
    }
    return 0;
}

// A sparse tiled level is padded to whole 64 KB tiles, each one block row of 128 / block height GOB columns, so a tile decodes as its own block-linear surface.
// *wb, *nrows: 0 past the level
int nvn_deswizzle_tile(const nvn_texdesc *d, unsigned level, uint32_t tx, uint32_t ty, const void *tile, void *dst, size_t pitch, uint32_t *wb, uint32_t *nrows) {
    uint32_t row_bytes, rows, slices, bpb;
    uint64_t base;
    unsigned th, td;
    *wb = *nrows = 0;
    if (nvn_deswizzle_geom(d, level, 0, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || td || slices != 1 || th > 7) return -1;
    uint32_t trb = (128u >> th) * 64, trows = 8u << th, x0 = tx * trb, y0 = ty * trows;
    if (x0 >= row_bytes || y0 >= rows) return 0;
    uint32_t w = row_bytes - x0 < trb ? row_bytes - x0 : trb, h = rows - y0 < trows ? rows - y0 : trows;
    if (pitch < w) return -1;
    const uint8_t *s = tile;
    uint8_t *o = dst;
    for (uint32_t y = 0; y < h; y++)
        for (uint32_t x = 0; x < w; x += 16) memcpy(o + (size_t)y * pitch + x, s + nvn_bl_offset(x, y, 0, trb, trows, th, 0, nvn_gob_order(d)), w - x < 16 ? w - x : 16);
    *wb = w;
    *nrows = h;
    return 0;
}
// a sparse texture's mip tail gathered on its own: the level must lie at or after src_at
int nvn_deswizzle_from(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, uint64_t src_at, void *dst, size_t dst_pitch) {
    return move(d, level, layer, (uint8_t *)src, src_at, (uint8_t *)dst, dst_pitch, 1);
}
