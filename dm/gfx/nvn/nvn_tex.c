// NVN texture storage size, alignment and level offsets (Tegra block-linear; one GOB = 64 bytes x 8 rows).
// Ported from deko3d source/dk_image.cpp (zlib license, Copyright (C) 2018-2020 fincs).
#include "nvn_tex.h"

typedef struct { uint8_t bpb, bw, bh; } fmt_t;

static fmt_t fmt_of(int f) {
    static const uint8_t astc[14][2] = {{4,4},{5,4},{5,5},{6,5},{6,6},{8,5},{8,6},{8,8},{10,5},{10,6},{10,8},{10,10},{12,10},{12,12}};
    if (f >= 0x79 && f <= 0x94) {
        int k = (f - 0x79) % 14;
        return (fmt_t){16, astc[k][0], astc[k][1]};
    }
    if (f >= 0x41 && f <= 0x50) {                         // DXT / RGTC / BPTC: 4×4 blocks
        int b8 = (f <= 0x42) || (f == 0x45 || f == 0x46) || (f == 0x49 || f == 0x4A);
        return (fmt_t){b8 ? 8 : 16, 4, 4};
    }
    static const struct { uint8_t lo, hi, bpb; } r[] = {
        {0x01,0x04,1},{0x05,0x09,2},{0x0A,0x0C,4},{0x0D,0x10,2},{0x11,0x15,4},{0x16,0x18,8},
        {0x19,0x1C,3},{0x1D,0x21,6},{0x22,0x24,12},{0x25,0x28,4},{0x29,0x2D,8},{0x2E,0x30,16},
        {0x31,0x31,1},{0x32,0x32,2},{0x33,0x35,4},{0x36,0x36,8},{0x37,0x38,4},{0x39,0x3C,2},{0x3D,0x40,4},
        {0x51,0x52,1},{0x53,0x54,2},{0x55,0x56,4},{0x57,0x58,2},{0x59,0x5A,4},{0x5B,0x5C,8},
        {0x5D,0x5E,3},{0x5F,0x60,6},{0x61,0x62,12},{0x63,0x64,4},{0x65,0x66,8},{0x67,0x68,16},
        {0x69,0x6C,4},{0x6D,0x70,4},{0x71,0x75,8},{0x76,0x78,16},{0x95,0x98,2},{0x99,0x9C,4},
    };
    for (unsigned i = 0; i < sizeof r / sizeof *r; i++)
        if (f >= r[i].lo && f <= r[i].hi) return (fmt_t){r[i].bpb, 1, 1};
    return (fmt_t){0, 1, 1};
}

int nvn_fmt_bpb(int f, int *bw, int *bh) {
    fmt_t t = fmt_of(f);
    if (bw) *bw = t.bw;
    if (bh) *bh = t.bh;
    return t.bpb;
}

static uint32_t mip(uint32_t s, unsigned l) { s >>= l; return s ? s : 1; }
static uint32_t blk(uint32_t s, uint32_t b) { return (s + b - 1) / b; }
static unsigned pick_tile(uint32_t gobs) { return gobs >= 16 ? 4 : gobs >= 8 ? 3 : gobs >= 4 ? 2 : gobs >= 2 ? 1 : 0; }
// packaged byte 1's low nibble, log2 GOBs (0 = none): pads the tiled levels even without SPARSE, and aligns layers
static unsigned packaged_width_log2(const nvn_texdesc *d) {
    return d->pkg_layout ? ((unsigned)(d->pkg_layout - 1) >> 8) & 0xf : 0;
}
static unsigned adj_tile(unsigned shift, uint32_t unit, uint32_t dim) {
    if (!shift) return 0;
    uint32_t x = unit << (shift - 1);
    if (x >= dim) while (--shift) { x >>= 1; if (x < dim) break; }
    return shift;
}

static void shape(const nvn_texdesc *d, unsigned *dpl, int *layered, uint32_t *w, uint32_t *h, uint32_t *z) {
    *w = d->width; *h = d->height; *z = d->depth; *layered = 0;
    switch (d->target) {
        case 0: *dpl = 1; *h = 1; *z = 1; break;                       // 1D
        case 3: *dpl = 1; *z = d->height; *h = 1; *layered = 1; break; // 1D_ARRAY: layers in height
        case 2: *dpl = 3; break;                                        // 3D
        case 4: case 6: *dpl = 2; *layered = 1; break;                  // 2D_ARRAY / MS_ARRAY
        case 8: *dpl = 2; *z = 6; *layered = 1; break;                  // CUBEMAP
        case 9: *dpl = 2; *layered = 1; break;                          // CUBEMAP_ARRAY: depth counts layer-faces
        default: *dpl = 2; *z = 1; break;                               // 2D / MS / RECTANGLE
    }
    if (*z == 0) *z = 1;
}

static void samples_xy(int s, uint32_t *sx, uint32_t *sy) {
    *sx = *sy = 1;
    if (s == 2) {
        *sx = 2;
    } else if (s == 4) {
        *sx = *sy = 2;
    } else if (s == 8) {
        *sx = 4;
        *sy = 2;
    }
}

// Sparse textures map in 64 KB tiles: a level at least one tile in each dimension is padded to whole tiles, smaller ones form the mip tail.
// Streamed textures come pre-swizzled in these tile shapes.
enum { SPARSE_TILE = 0x10000 };
static int sparse_of(const nvn_texdesc *d) { return (d->flags & 0x20) && !(d->flags & 0x50) && d->target != 10; }
// log2 GOBs of the standard 2D 64 KB tile's height (256×256 to 64×64 blocks), by bytes per block; a packaged layout keeps its own
static int sparse_tile_h(unsigned bpb) { return bpb == 1 ? 5 : bpb == 2 || bpb == 4 ? 4 : bpb == 8 || bpb == 16 ? 3 : -1; }
// the standard 3D tile (64x32x32 to 16x16x16 blocks): log2 GOBs high and deep
static int sparse_tile_h3(unsigned bpb) { return bpb == 1 || bpb == 2 || bpb == 4 ? 2 : bpb == 8 || bpb == 16 ? 1 : -1; }
static int sparse_tile_d3(unsigned bpb) { return bpb == 1 || bpb == 2 ? 5 : bpb == 4 || bpb == 8 || bpb == 16 ? 4 : -1; }
static unsigned tile_shape_h(const nvn_texdesc *d, unsigned dpl, fmt_t t, unsigned tileH) {
    return dpl == 2 && !d->pkg_layout && sparse_tile_h(t.bpb) >= 0 ? (unsigned)sparse_tile_h(t.bpb) : tileH;
}
// *gobs: the GOB columns it is laid out with, may be NULL
static uint64_t level_walk(const nvn_texdesc *d, unsigned level, unsigned dpl, uint32_t w, uint32_t h, uint32_t z,
                           unsigned tileH, unsigned tileD, fmt_t t, uint32_t *gobs) {
    uint32_t sx, sy;
    samples_xy(d->samples, &sx, &sy);
    unsigned sh = tile_shape_h(d, dpl, t, tileH);
    unsigned pw = packaged_width_log2(d);
    uint32_t tw = pw ? 1u << pw : 128u >> (sh + tileD);   // GOB columns of a tile
    if (!tw) tw = 1;
    int tiled = sparse_of(d) || pw;
    uint64_t off = 0;
    for (unsigned i = 0;; i++) {
        uint32_t lw = blk(mip(w * sx, i), t.bw);
        uint32_t lh = dpl >= 2 ? blk(mip(h * sy, i), t.bh) : 1;
        uint32_t ld = dpl >= 3 ? mip(z, i) : 1;
        uint32_t lwb = lw * t.bpb;
        unsigned th = adj_tile(tileH, 8, lh), td = adj_tile(tileD, 1, ld);
        uint32_t wg = (lwb + 63) / 64, hg = (lh + 7) / 8;
        tiled = tiled && lwb >= tw * 64 && lh >= 8u << sh && ld >= 1u << tileD;
        if (tiled) wg = (wg + tw - 1) / tw * tw;
        if (i == level) { if (gobs) *gobs = wg; return off; }
        uint32_t ht = (hg + (1u << th) - 1) >> th, dt = (ld + (1u << td) - 1) >> td;
        off += (uint64_t)wg * ht * dt << (9 + th + td);
    }
}
static uint64_t level_offset(const nvn_texdesc *d, unsigned level, unsigned dpl, uint32_t w, uint32_t h, uint32_t z, unsigned tileH, unsigned tileD, fmt_t t) {
    return level_walk(d, level, dpl, w, h, z, tileH, tileD, t, 0);
}
static uint64_t layer_bytes(const nvn_texdesc *d, unsigned dpl, int layered, uint32_t w, uint32_t h, uint32_t z, unsigned tileH, unsigned tileD, fmt_t t) {
    uint64_t ls = level_offset(d, d->levels ? d->levels : 1, dpl, w, h, z, tileH, tileD, t);
    if (sparse_of(d)) ls = (ls + SPARSE_TILE - 1) & ~(uint64_t)(SPARSE_TILE - 1);
    if (layered) {
        uint32_t hh = blk(h, t.bh);
        unsigned th = tileH, td = tileD;
        while (th && hh <= (8u << (th - 1))) th--;
        while (td && z <= (1u << (td - 1))) td--;
        unsigned pw = packaged_width_log2(d);
        unsigned sh = pw ? pw + tileH + tileD + 9 : th + td + 9;
        uint64_t g = ls >> sh;
        if (ls != g << sh) ls = (g + 1) << sh;
    }
    return ls;
}

void nvn_tex_layout(const nvn_texdesc *d, nvn_texlayout *o) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    uint32_t sx, sy;
    samples_xy(d->samples, &sx, &sy);
    o->bpb = t.bpb;
    o->tileH = o->tileD = 0;
    o->layers = layered ? z : 1;
    if (!t.bpb) {
        o->size = 0;
        o->align = 512;
        o->layer_size = 0;
        return;
    }
    if (d->target == 10) {                                            // BUFFER
        o->layer_size = o->size = (uint64_t)t.bpb * d->width;
        o->align = t.bpb == 12 ? 4 : t.bpb;
        return;
    }
    // LINEAR_RENDER_TARGET (0x40) is linear without LINEAR (0x10)
    if (d->flags & 0x50) {
        o->align = (d->flags & 0x40) ? 128 : 32;
        uint32_t stride = d->stride ? d->stride : (t.bpb * blk(w, t.bw) + o->align - 1) & ~(o->align - 1);
        o->layer_size = o->size = (uint64_t)stride * h;               // the driver counts pixel rows, also for block-compressed formats
        return;
    }
    unsigned tileH = 0, tileD = 0;
    int sh2 = sparse_tile_h(t.bpb), sh3 = sparse_tile_h3(t.bpb), sd3 = sparse_tile_d3(t.bpb);
    uint32_t wbytes = blk(w * sx, t.bw) * t.bpb, hblocks = blk(h * sy, t.bh);
    if (d->pkg_layout) {                                              // byte 0: log2 block height | log2 block depth << 4
        unsigned pl = (unsigned)(d->pkg_layout - 1) & 0xff;
        if (dpl >= 2) tileH = pl & 0xf;
        if (dpl == 3) tileD = pl >> 4;
    } else if (dpl == 2 && sparse_of(d) && sh2 >= 0 && wbytes >= (128u >> sh2) * 64 && hblocks >= 8u << sh2) {
        tileH = (unsigned)sh2;
    } else if (dpl == 3 && sparse_of(d) && sh3 >= 0 && wbytes >= (128u >> (sh3 + sd3)) * 64 && hblocks >= 8u << sh3 && z >= 1u << sd3) {
        tileH = (unsigned)sh3;
        tileD = (unsigned)sd3;
    } else if (dpl == 2) {
        // NVN truncates height in blocks × 1.5 / 8 before picking the power of two
        tileH = pick_tile(hblocks * 3 / 16);
    } else if (dpl == 3) {
        tileD = pick_tile(z + z / 2);
    }
    uint64_t ls = layer_bytes(d, dpl, layered, w, h, z, tileH, tileD, t);
    // MINIMAL_LAYOUT (0x200): 1-GOB blocks unless the automatic ones make the layer no larger; packaged and sparse layouts keep theirs
    if ((d->flags & 0x200) && !d->pkg_layout && !sparse_of(d) && (tileH || tileD)) {
        uint64_t one = layer_bytes(d, dpl, layered, w, h, z, 0, 0, t);
        if (one != ls) { tileH = tileD = 0; ls = one; }
    }
    o->tileH = tileH;
    o->tileD = tileD;
    o->layer_size = ls;
    o->size = layered ? ls * z : ls;
    o->align = sparse_of(d) ? SPARSE_TILE : 512;
}

uint64_t nvn_tex_level_offset(const nvn_texdesc *d, unsigned level) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || (d->flags & 0x50) || d->target == 10) return 0;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    return level_offset(d, level, dpl, w, h, z, L.tileH, L.tileD, t);
}

uint32_t nvn_tex_level_gobs(const nvn_texdesc *d, unsigned level) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    uint32_t w, h, z, g = 0;
    shape(d, &dpl, &layered, &w, &h, &z);
    if (!t.bpb || (d->flags & 0x50) || d->target == 10) return 0;
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    level_walk(d, level, dpl, w, h, z, L.tileH, L.tileD, t, &g);
    return g;
}

int nvn_tex_sparse_tiles(const nvn_texdesc *d, uint32_t *tw, uint32_t *th, uint32_t *td) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    *tw = *th = *td = 0;
    if (!sparse_of(d) || !t.bpb) return 0;
    uint32_t sx, sy;
    samples_xy(d->samples, &sx, &sy);
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    unsigned sh = tile_shape_h(d, dpl, t, L.tileH);
    uint32_t gw = 128u >> (sh + L.tileD);   // as level_walk
    if (!gw) gw = 1;
    *tw = gw * 64 / t.bpb * t.bw / sx;
    *th = dpl >= 2 ? (8u << sh) * t.bh / sy : 1;
    *td = dpl >= 3 ? 1u << L.tileD : 1;
    unsigned levels = d->levels ? d->levels : 1, n = 0;
    for (; n < levels; n++) {   // level_walk's tiled-level test
        uint32_t lwb = blk(mip(w * sx, n), t.bw) * t.bpb;
        uint32_t lh = dpl >= 2 ? blk(mip(h * sy, n), t.bh) : 1;
        uint32_t ld = dpl >= 3 ? mip(z, n) : 1;
        if (lwb < gw * 64 || lh < 8u << sh || ld < 1u << L.tileD) break;
    }
    return (int)n;
}

void nvn_tex_level_tiles(const nvn_texdesc *d, unsigned level, unsigned *tileH, unsigned *tileD) {
    fmt_t t = fmt_of(d->format);
    unsigned dpl;
    int layered;
    uint32_t w, h, z;
    shape(d, &dpl, &layered, &w, &h, &z);
    uint32_t sx, sy;
    samples_xy(d->samples, &sx, &sy);
    nvn_texlayout L;
    nvn_tex_layout(d, &L);
    uint32_t lh = dpl >= 2 ? blk(mip(h * sy, level), t.bh) : 1;
    uint32_t ld = dpl >= 3 ? mip(z, level) : 1;
    *tileH = adj_tile(L.tileH, 8, lh);
    *tileD = adj_tile(L.tileD, 1, ld);
}

int nvn_tbuf_rows(uint64_t n, uint32_t bpb, uint32_t align, uint32_t *w, uint32_t *h, uint32_t *row) {
    if (!n || !bpb) return 0;
    if (!align) align = 1;
    *w = n < NVN_TBUF_ROW ? (uint32_t)n : NVN_TBUF_ROW;
    *h = (uint32_t)((n + NVN_TBUF_ROW - 1) / NVN_TBUF_ROW);
    uint64_t r = (uint64_t)*w * bpb;
    r = (r + align - 1) / align * align;
    *row = (uint32_t)r;
    return *h == 1 || r == (uint64_t)NVN_TBUF_ROW * bpb;   // rows must lie back to back, as the wrapped address assumes
}
