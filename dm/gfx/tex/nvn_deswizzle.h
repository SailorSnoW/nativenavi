// Level / layer offsets and per-level block height and depth always come from nvn_tex.c
#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../nvn/nvn_tex.h"

// dst rows are block rows dst_pitch bytes apart; a 3D level's slice k at k × dst_pitch × its block rows. -1 for multisample, LINEAR or BUFFER.
int nvn_deswizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, void *dst_linear, size_t dst_pitch);
// for unit test round trips
int nvn_swizzle(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_linear, size_t src_pitch, void *dst_storage);
// The middle block of each step x step cell (a small picture of a level too large to read whole); -1 unless blocks are 1 to 16 bytes
int nvn_deswizzle_pick(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src_storage, uint32_t step, void *dst_linear, uint32_t nx, uint32_t ny);
// tile: the 64 KB tile (tx, ty) of a sparse texture's level; *wb, *rows: the part inside the level
int nvn_deswizzle_tile(const nvn_texdesc *d, unsigned level, uint32_t tx, uint32_t ty, const void *tile, void *dst_linear, size_t dst_pitch, uint32_t *wb, uint32_t *rows);
// src points at byte src_at of the storage; the level and layer must lie at or after it
int nvn_deswizzle_from(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, uint64_t src_at, void *dst_linear, size_t dst_pitch);
// Partial read of one image (a level of a layer) whose pages were written since a stamp: the 16-byte pieces whose source host page the bitmap marks are the
// only ones read from src. wpg: one byte per host page (getpagesize()) from wpg_base on; NULL: every piece is written. The runs of consecutive pieces of one
// block row are returned in *runs (caller-grown), and their bytes counted in *nbytes; with dst they are also copied to dst_linear at their place (row pitch
// dst_pitch). nvn_deswizzle_runs_copy copies a run list made earlier. Both return -1 when unsupported.
typedef struct { uint32_t z, y, j0, n; } nvn_dwr_t;   // n pieces of 16 bytes of block row y, slice z, from piece j0
int nvn_deswizzle_written(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, uint64_t src_at,
                          const uint8_t *wpg, uintptr_t wpg_base, unsigned wpg_n, void *dst_linear, size_t dst_pitch,
                          nvn_dwr_t **runs, unsigned *nruns, unsigned *cap, uint64_t *nbytes);
int nvn_deswizzle_runs_copy(const nvn_texdesc *d, unsigned level, unsigned layer, const void *src, uint64_t src_at,
                            const nvn_dwr_t *runs, unsigned nruns, void *dst_linear, size_t dst_pitch);
// x in bytes, y in block rows; offset from the level and layer's start. row_bytes: a sparse texture's tiled levels pad rows to whole tiles.
// GOB order 0 = X1, 1 = Switch 2 (offset bits 5 and 6 exchanged): the device's flag bit 16, captured at TextureInitialize, used by every reader and writer.
static inline int nvn_gob_order(const nvn_texdesc *d) { return d->gob_order; }
uint64_t nvn_bl_offset(uint32_t x, uint32_t y, uint32_t z, uint32_t row_bytes, uint32_t rows, unsigned tileH, unsigned tileD, int switch2);

int nvn_deswizzle_geom(const nvn_texdesc *d, unsigned level, unsigned layer,
                       uint32_t *row_bytes, uint32_t *rows, uint32_t *slices,
                       uint64_t *base, unsigned *tileH, unsigned *tileD, uint32_t *bpb);
