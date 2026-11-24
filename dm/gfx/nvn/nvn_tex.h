// NVN texture storage layout (Tegra X1 block-linear)
#pragma once
#include <stdint.h>

typedef struct {
    int target, format, flags, samples;   // samples: 0 or 1 = single-sampled
    uint32_t width, height, depth, levels, stride;
    uint16_t pkg_layout;                  // byte 0 | (byte 1 & 0xF) << 8, + 1; 0 = automatic
    uint16_t gob_order;                   // 0 Tegra X1, 1 Switch 2 (the device's)
} nvn_texdesc;

typedef struct {
    uint64_t size, layer_size;
    uint32_t align, layers, bpb;
    unsigned tileH, tileD;                // log2 of the GOB count
} nvn_texlayout;

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o);
uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level);
uint32_t nvn_tex_level_gobs(const nvn_texdesc *d, unsigned level);   // width in GOBs, padded in sparse tiled and packaged levels
int nvn_fmt_bpb(int format, int *bw, int *bh);   // 0 = unknown format
void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD);
// tiled levels (before the mip tail), one 64 KB tile in texels; 0 when not sparse
int nvn_tex_sparse_tiles(const nvn_texdesc *d, uint32_t *tw, uint32_t *th, uint32_t *td);
// A texture buffer as a linear 2D texture of NVN_TBUF_ROW-texel rows (the translator's x & 0x3FFF, x >> 14 fetch); 0 when n or bpb is 0
#define NVN_TBUF_ROW 16384u
int nvn_tbuf_rows(uint64_t n, uint32_t bpb, uint32_t align, uint32_t *w, uint32_t *h, uint32_t *row);
