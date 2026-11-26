// NVN host object layouts and command records (hle_nvn.c writes, nvn_mtl.m replays), in place in the game's object memory within nvn.h's sizes (FITS).
#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn.h"
#include "nvn_tex.h"
#include "dmsh_abi.h"

#define MAGIC(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
#define FITS(T, H) _Static_assert(sizeof(T) <= sizeof(H), #T " exceeds the reserved size of " #H)

typedef struct { uint32_t flags; } dev_builder_t;                       FITS(dev_builder_t, NVNdeviceBuilder);
typedef struct { uint32_t magic, flags, origin, depth_mode; void *dbg_cb, *dbg_data; } dev_t_;  FITS(dev_t_, NVNdevice);

// Memory sizes as the driver keeps them: 32-bit, 0 until set, a flag each; the queue's defaults apply at use
enum { QB_CMD, QB_COMP, QB_CTL, QB_RT };
typedef struct { NVNdevice *dev; uint32_t flags; uint32_t sz[4], flush; uint8_t set[4]; void *mem; uint64_t mem_sz; } queue_builder_t;
FITS(queue_builder_t, NVNqueueBuilder);
typedef struct { uint32_t magic; NVNdevice *dev; uint64_t submits; int origin_ll; uint64_t serial; } queue_t;  FITS(queue_t, NVNqueue);

typedef struct { NVNdevice *dev; void *mem; uint64_t size; uint32_t flags; } pool_builder_t;  FITS(pool_builder_t, NVNmemoryPoolBuilder);
// Virtual pool (flags 0x200): vp NULL when aliased; vmode 0 unmapped, 1 aliased, 2 sparse; asked: an address in the raw pool table was handed out
typedef struct { uint32_t magic, flags; uint8_t *mem; uint64_t size; struct vpool *vp; int vmode; uint8_t asked; } pool_t;   FITS(pool_t, NVNmemoryPool);

typedef struct { const pool_t *pool; int64_t off; int32_t n; } dpool_t;
FITS(dpool_t, NVNtexturePool); FITS(dpool_t, NVNsamplerPool);

typedef struct {
    NVNdevice *dev; nvn_texdesc d; pool_t *pool; int64_t off;
    uint8_t has_layout, layout[8];
    const void *pkg_data;                // copied into the storage at TextureInitialize
    uint8_t set_swz : 1, stencil : 1; uint16_t swz;   // swz: r | g << 3 | b << 6 | a << 9 (NVN and Metal channel enums agree)
                                         // stencil: DepthStencilMode STENCIL (reads the stencil plane)
} tex_builder_t;                                                                             FITS(tex_builder_t, NVNtextureBuilder);
// Packed to 0x80: NVN 55 allocates NVNtexture as 0x80 bytes, and a larger record writes into the next heap block
typedef struct {
    uint32_t magic; nvn_texdesc d; uint8_t has_layout, set_swz : 1, stencil : 1; uint16_t swz; nvn_texlayout L; uint8_t *mem; pool_t *pool; int64_t off; uint8_t layout[8];
    uint64_t serial;                     // table key of the MTLTexture: the game may copy the object by value
} tex_t;                                                                                     FITS(tex_t, NVNtexture);
_Static_assert(sizeof(tex_t) <= 0x80, "tex_t exceeds the newer SDKs' NVNtexture (0x80)");
// one 2D level of rows: the explicit or aligned stride, as GetStorageSize counts it
static inline size_t tex_linear_row(const tex_t *t) { return (size_t)(t->L.layer_size / (t->d.height ? t->d.height : 1)); }
// NVN 55's NVNtextureView is 16 bytes (games keep it in a 16-byte stack slot)
typedef struct { uint8_t set_levels : 1, set_layers : 1, set_format : 1, set_target : 1, set_swizzle : 1, set_dsmode : 1, dsmode : 2;
                 uint8_t target, base_level, nlevels; uint16_t format, min_layer, nlayers; uint8_t swz[4]; uint16_t _rsv; } view_t;
FITS(view_t, NVNtextureView);
_Static_assert(sizeof(view_t) <= 16, "view_t exceeds the newer SDKs' NVNtextureView (16 bytes)");

typedef struct { uint8_t minf, magf, ws, wt, wr, cmp_mode, cmp_func, reduction; float lod_min, lod_max, lod_bias, aniso, border[4]; } samp_par;   // reduction takes the padding byte
typedef struct { NVNdevice *dev; samp_par p; } samp_builder_t;                                FITS(samp_builder_t, NVNsamplerBuilder);
// State from byte 8 on, every byte written: games hash a sampler from byte 8 (docs/reference/nvn-objects.md#games-hash-a-samplers-bytes-from-8-on)
typedef struct { uint32_t magic, zero; samp_par p; } samp_t;                                  FITS(samp_t, NVNsampler);
_Static_assert(__builtin_offsetof(samp_t, p) == 8 && sizeof(samp_par) == 40, "sampler state from byte 8, with no padding");

// Stage keys are not kept here: the game's NVNprogram may be smaller than 0xC0
typedef struct prog_t { uint32_t magic, nstage; NVNshaderData sd[6]; uint64_t serial; } prog_t;     FITS(prog_t, NVNprogram);   // serial: the replay's table key

typedef struct { uint32_t magic, fenced; uint64_t fence; } sync_t;   /* fenced 1: physical, 2: acquire, 3: owned logical receipt */                           FITS(sync_t, NVNsync);
typedef struct { const pool_t *pool; int64_t off; } ev_builder_t;                             FITS(ev_builder_t, NVNeventBuilder);
typedef struct { uint32_t magic; volatile uint32_t *val; const pool_t *pool; int64_t off; } ev_t; FITS(ev_t, NVNevent);

// nactive: SetNumActiveTextures (0 = all)
typedef struct { NVNdevice *dev; void *native; int32_t ntex, interval; NVNtexture *tex[4]; int32_t nactive; } win_builder_t;
FITS(win_builder_t, NVNwindowBuilder);
// avail[i]: ns (monotonic) image i can be acquired again; pace: the present pacer's target
typedef struct { uint32_t magic; void *native; int32_t ntex, interval, next, crop[4]; NVNtexture *tex[4]; uint64_t presents, avail[4], pace, ticket[4]; int32_t nactive, follow; } win_t;   // ticket[i]: image i's last present
FITS(win_t, NVNwindow);

// Records in the game's command memory, a recording's piece list in its control memory (the handle: ctl_e); the game is called back for more
typedef struct {
    uint32_t magic, recording;
    uint8_t *cur;
    uint64_t nvn_flag10;
                                         // +0x10 is the game's: its inlined NVN header code writes byte 1 there (docs/reference/nvn-objects.md#inlined-sdk-code-writes-into-nvncommandbuffer-0x10)
    uint8_t *end;
    uint8_t *begin;                      // recording 2 = not written yet
    PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb; void *cb_data;
    NVNdevice *dev;
    uint64_t used;                       // GetCommandMemoryUsed: since the game last added command memory
    int64_t copy_row, copy_image;        // command buffer state, written into each buffer <-> texture copy, not recorded
    size_t cmd_size;                     // the bytes last passed to AddCommandMemory; a host-lent segment leaves it
    uint8_t *ctl_mem;                    // last control block added (8-aligned), or a host-lent one
    uint32_t ctl_size, ctl_used;         // GetControlMemorySize / Used (counting ctl_q)
    uint64_t ctl_q[2];                   // unwritten control entries, so the control-memory callback never runs inside the command-memory one
    uint8_t *ctl_list;                   // the handle once written
    uint8_t *chunk;                      // open piece not yet in a CTL_CHUNK (NULL = none)
    uint32_t rec_id;                     // its own host-lent segments are not freed by its own writes
    uint32_t own_back;                   // end − the recording's first byte in the segment (0 = none); a host-lent segment hangs off it
} cmd_t;                                                                                   FITS(cmd_t, NVNcommandBuffer);

// defaults follow NVN / GL
typedef struct { uint32_t blend_en : 8, logic_op : 4, alpha_func : 4; } color_st;            FITS(color_st, NVNcolorState);
typedef struct { uint32_t mask; } chmask_st;            /* 4 bits per target: r=1 g=2 b=4 a=8, bit set = write */ FITS(chmask_st, NVNchannelMaskState);
// adv: advanced blend mode (0 none); adv_npm: source not premultiplied; adv_ovl: overlap (0 uncorrelated); adv_dst_free: NormalizedDst(false)
typedef struct { uint64_t target : 3, src_rgb : 7, dst_rgb : 7, src_a : 7, dst_a : 7, eq_rgb : 3, eq_a : 3, adv : 6, adv_npm : 1, adv_ovl : 2, adv_dst_free : 1; } blend_st;
FITS(blend_st, NVNblendState);
typedef struct { uint8_t enable, samples, a2c, a2c_dither, raster_samples, cov_mod, cov2color_en, cov2color_out, sl_en, sl_grid; } ms_st;
FITS(ms_st, NVNmultisampleState);
typedef struct { uint32_t cull : 2, front : 1, mode : 2, offset_en : 3; } poly_st;           FITS(poly_st, NVNpolygonState);
typedef struct {
    uint64_t dt : 1, dw : 1, df : 4, st : 1, sf_f : 4, sf_b : 4,
             fail_f : 4, zfail_f : 4, zpass_f : 4, fail_b : 4, zfail_b : 4, zpass_b : 4;
} ds_st;                                                                                     FITS(ds_st, NVNdepthStencilState);
typedef struct { uint32_t fmt : 8, offset : 19, stream : 5; } va_st;                         FITS(va_st, NVNvertexAttribState);
typedef struct { uint32_t stride, divisor; } vs_st;                                          FITS(vs_st, NVNvertexStreamState);

// {op, size with header, 8-aligned} + payload
typedef struct { uint32_t op, size; } op_hdr;
// A recording's control list: 8-byte entries, the handle the first; ends and links take no command memory, as on the driver
// (docs/reference/nvn-objects.md#a-recordings-end-and-links-are-in-control-memory).
//   bits 63-62 kind (CTL_CHUNK records at addr, len bytes; CTL_LINK the list goes on at addr); bit 61 CTL_LAST; bits 60-44 len / 8 (CTL_ONE: one record); bits 43-0 addr / 8
enum { CTL_CHUNK = 1, CTL_LINK = 3 };
#define CTL_LAST (UINT64_C(1) << 61)
#define CTL_ONE (((UINT64_C(1) << 17) - 1) * 8)
#define CTL_CHUNK_MAX (CTL_ONE - 8)
static inline uint64_t ctl_e(unsigned kind, const void *addr, uint64_t len) { return (uint64_t)kind << 62 | (len >> 3) << 44 | (uint64_t)(uintptr_t)addr >> 3; }
static inline unsigned ctl_kind(uint64_t e) { return (unsigned)(e >> 62); }
static inline const uint8_t *ctl_addr(uint64_t e) { return (const uint8_t *)(uintptr_t)((e & ((UINT64_C(1) << 44) - 1)) << 3); }
static inline uint64_t ctl_len(uint64_t e) { return (e >> 44 & ((UINT64_C(1) << 17) - 1)) << 3; }
static inline size_t ctl_chunk_bytes(uint64_t e) {
    uint64_t n = ctl_len(e);
    if (n == CTL_ONE) {
        op_hdr h;
        memcpy(&h, ctl_addr(e), sizeof h);
        n = h.size;
    }
    return (size_t)n;
}
// Compact records, about the driver's size (docs/reference/memory.md#model-heaps-sized-by-the-hosts-command-record-sizes); the queue's flatten expands them (rec_expand).
// Header op: OPC_* in the low 8 bits, small arguments in the upper 24 | payload
enum {
    OPC_BASE = 0x80,
    OPC_DRAW = OPC_BASE,   // prim | first, count (i32); one instance
    OPC_DRAW_INST,         // prim | first, count, base instance, instances
    OPC_DRAW_IND,          // prim | ind (u64)
    OPC_DRAW_IDX,          // prim, index type << 8 | count, base vertex, ibuf (u64); one instance
    OPC_DRAW_IDX_INST,     // prim, index type << 8 | count, base vertex, base instance, instances, ibuf
    OPC_DRAW_IDX_IND,      // prim, index type << 8 | ibuf, ind (u64)
    OPC_VIEWPORTS,         // first | n << 8 | (origin + 1) << 16; n × {x, y, w, h} (f32)
    OPC_STENCIL,           // face, which << 2, value << 4 (20 bits) | none
    OPC_HANDLES,           // op, stage << 6, first << 9, n << 17 | n × handle (u32), moved to the bound pools
    OPC_BUFS,              // op, stage << 6, first << 9, n << 17 | n × {addr (u64), size (u32)} → n records of op
    OPC_VBUFS,             // first, n << 8 | n × {addr (u64), size (u32)}
    OPC_VATTR,             // n | n × attribute state (u32), from 0
    OPC_VSTREAM,           // n | n × stream state (8 bytes), from 0
    OPC_UBO,               // stage, first << 3, n << 7 | n × (addr >> 8 | size << 40) → OP_BIND_UBO_SMALL
    OPC_STATE,             // kind, value << 3 (21 bits) | none: a 4-byte state
    OPC_STATE8,            // kind | the 8-byte state
    OPC_BARRIER,           // bits (24) | none
    OPC_PRIM_RESTART,      // on, all-ones index << 1, index << 2 (22 bits) | none
    OPC_SCISSOR,           // none | x, y, w, h (u16)
    OPC_END_
};
#define REC_EXPAND_MAX 4096   // canonical bytes one compact record expands to at most
static inline int rec_compact(uint32_t op) { return (op & 0xff) >= OPC_BASE; }
// malloc'd, ends in OP_END: for tests and diagnostics
uint8_t *nvn_rec_linear(uint64_t handle, size_t *len);
enum {
    OP_END, OP_JUMP, OP_BEGIN, OP_CALL,                                             // OP_BEGIN is not recorded; numbers stay for captures
    OP_SIGNAL_EVENT, OP_REPORT_COUNTER, OP_COPY_B2B, OP_UPDATE_UB,
    OP_SET_RT, OP_CLEAR_COLOR, OP_CLEAR_DS, OP_VIEWPORTS, OP_SCISSOR, OP_DEPTH_RANGE,
    OP_BIND_PROGRAM, OP_BIND_STATE, OP_BIND_VATTR, OP_BIND_VSTREAM, OP_BIND_VBUF,
    OP_BIND_UBO, OP_BIND_SSBO, OP_BIND_TEX, OP_BIND_SAMP,
    OP_STENCIL, OP_POLY_OFFSET, OP_DRAW, OP_DRAW_IDX, OP_DISPATCH, OP_BARRIER,
    OP_SET_TEX_POOL, OP_SET_SAMP_POOL, OP_COPY_B2T, OP_COPY_STRIDE, OP_BLEND_COLOR, OP_COPY_T2B, OP_BIND_IMG, OP_COPY_T2T, OP_RESET_COUNTER, OP_CLEAR_TEX, OP_CLEAR_BUF, OP_DEPTH_CLAMP, OP_RENDER_COND, OP_FENCE_SYNC, OP_TF_BIND_RETIRED, OP_TF_STATE_RETIRED, OP_RAST_DISCARD, OP_VP_SWIZZLE, OP_BIND_TEX1, OP_PRIM_RESTART, OP_BIND_UBO_SMALL, OP_BIND_VBUF1, OP_DRAW_TEX, OP_ALPHA_REF, OP_PUSH_CONST, OP_DLSS, OP_MULTI_DRAW, OP_WAIT_EVENT, OP_WAIT_SYNC, OP_PATCH_SIZE_RETIRED, OP_LINE_WIDTH,
    OP_COUNT_
};
typedef struct {
    uint64_t ind, cnt, ibuf;
    int64_t stride;
    int32_t max, prim, itype;  // itype -1: DrawArrays
} p_multi_draw;
typedef struct { int32_t n; const tex_t *color[8]; view_t cview[8]; uint8_t has_cview[8]; const tex_t *depth; view_t dview; uint8_t has_dview; } p_set_rt;
typedef struct { int32_t index, mask; float rgba[4]; } p_clear_color;
typedef struct { uint64_t h; NVNdrawTextureRegion d, s; } p_draw_tex;   // texture handle, destination (pixels) and source (texels)
typedef struct { int32_t mode, pad; uint64_t addr; } p_render_cond;   // mode 0 / 1: render if the counters at addr and addr + 16 are equal / not; 2 / 3: SetRenderEnable(true / false)
typedef struct { float depth; int32_t depth_mask, stencil, stencil_mask; } p_clear_ds;
// the first viewport's unused near slot carries the origin: 0 legacy (device origin), else 1 + NVNwindowOriginMode
typedef struct {
    int32_t first, n;
    union { float v[16][6]; struct { float xywh[4]; int32_t origin; }; };
} p_viewports;
typedef struct { int32_t x, y, w, h; } p_scissor;
typedef struct { float n, f; } p_depth_range;
typedef struct { uint32_t on, index; } p_prim_restart;
typedef struct { float w; } p_line_width;
typedef struct { int32_t on; } p_depth_clamp;
typedef struct { int32_t first, n; uint8_t sw[16][4]; } p_vp_swizzle;   // source of clip x, y, z, w XOR the identity (0, 2, 4, 6): 0 = unchanged
typedef struct { const prog_t *prog; int32_t stages; } p_bind_program;
// kind: 0 blend 1 color 2 chmask 3 ms 4 poly 5 ds; only the state's own bytes are recorded; readers size by kind
typedef struct { int32_t kind; uint8_t bytes[0x18]; } p_bind_state;
typedef struct { int32_t first, n; uint32_t st[32]; } p_bind_vattr;
typedef struct { int32_t first, n; vs_st st[16]; } p_bind_vstream;
typedef struct { int32_t first, n; NVNbufferRange r[16]; } p_bind_vbuf;
// 24 bytes with its header, as the native command; larger ranges take the full record
typedef struct { uint32_t index, size; uint64_t addr; } p_bind_vbuf1;
static inline const p_bind_vbuf *vbuf_rec(uint32_t op, const void *a, p_bind_vbuf *t) {
    if (op != OP_BIND_VBUF1) return a;
    const p_bind_vbuf1 *b = a;
    t->first = (int32_t)b->index;
    t->n = 1;
    t->r[0] = (NVNbufferRange){b->addr, b->size};
    return t;
}
typedef struct { int32_t stage, index; uint64_t addr, size; } p_bind_buf;
// 24 bytes, not 32, likewise
typedef struct { uint16_t stage, index; uint32_t size; uint64_t addr; } p_bind_ubo_small;
static inline const p_bind_buf *bind_buf_rec(uint32_t op, const void *a, p_bind_buf *t) {
    if (op != OP_BIND_UBO_SMALL) return a;
    const p_bind_ubo_small *b = a;
    *t = (p_bind_buf){b->stage, b->index, b->addr, b->size};
    return t;
}
// defer: handles read from src at execution (h only a fallback; bh_fresh)
typedef struct { int32_t stage, first, n, defer; uint64_t src; uint64_t h[32]; } p_bind_handles;
// BindTexture as the hardware command: the handle at byte 16, where engines patch recorded commands (32 bits: texture ID | sampler ID << 20). Read with bind_rec
typedef struct { int32_t stage, slot; uint64_t h; } p_bind_tex1;
static inline const p_bind_handles *bind_rec(uint32_t op, const void *a, p_bind_handles *t) {
    if (op != OP_BIND_TEX1) return a;
    const p_bind_tex1 *b = a;
    t->stage = b->stage;
    t->first = b->slot;
    t->n = 1;
    t->defer = 0;
    t->src = 0;
    t->h[0] = (uint32_t)b->h;
    return t;
}
// 0-127 BindTexture's combined handles, 128-255 separate textures: the driver's two c2 tables, so a game may bind both
#define NVN_SEPTEX_SLOT0 DMSH_SEPTEX_SLOT0
// vertex 0, fragment 1, geometry 2, task 3, mesh 4 (NVN2's 6, 7), compute 5; NVN2 ray tracing (8) is refused
enum { NVN_RESOURCE_STAGES = 6 };
typedef struct { int32_t face, value_mask, mask, ref, which; } p_stencil;  // which: 0 value mask, 1 write mask, 2 reference
typedef struct { float factor, units, clamp; } p_poly_offset;
enum { DM_PRIM_MESH_TASKS = 0x40 };   // DrawMeshTasks (first, count): no NVN primitive has this value
typedef struct { int32_t prim, first, count, instances, base_instance; uint64_t ind; uint64_t mdi_cnt; int32_t mdi_i, pad; } p_draw;   // instances 0 = 1; ind: indirect arguments' address; mdi_cnt: draw mdi_i of an indirect-count multi-draw while mdi_i < the u32 there
typedef struct { int32_t prim, itype, count, base_vertex, base_instance, instances; uint64_t ibuf, ind; uint64_t mdi_cnt; int32_t mdi_i, pad; } p_draw_idx;   // ind: {count, instances, first index, base vertex, base instance}; mdi_cnt as in p_draw
// A draw record stops before ind unless indirect or multi-draw; draw_rec / draw_idx_rec give the rest as 0
static inline const p_draw *draw_rec(const void *a, uint32_t size, p_draw *t) {
    if (size - 8 >= sizeof *t) return a;
    memset(t, 0, sizeof *t);
    memcpy(t, a, size - 8);
    return t;
}
static inline const p_draw_idx *draw_idx_rec(const void *a, uint32_t size, p_draw_idx *t) {
    if (size - 8 >= sizeof *t) return a;
    memset(t, 0, sizeof *t);
    memcpy(t, a, size - 8);
    return t;
}
typedef struct { int32_t x, y, z; uint64_t caller, ind; } p_dispatch;   // caller: the game call site's return address; ind: indirect group counts' address
typedef struct { const dpool_t *pool; } p_set_pool;
typedef struct { uint64_t src; const tex_t *tex; view_t view; uint8_t has_view; NVNcopyRegion r; int32_t flags; } p_copy_b2t;
// host-only flag: a Downsample (resolve) recorded as a texture copy
#define DM_COPY_RESOLVE 0x40000000
typedef struct { const tex_t *src, *dst; view_t sv, dv; uint8_t has_sv, has_dv; NVNcopyRegion sr, dr; int32_t flags; } p_copy_t2t;   // T2B: src is the destination buffer address
typedef struct { const tex_t *tex; view_t view; uint8_t has_view, is_uint; NVNcopyRegion r; int32_t mask; union { float f[4]; uint32_t u[4]; } c; } p_clear_tex;
typedef struct { int64_t stride; int image; } p_copy_stride;   // image: SetCopyImageStride; older captures only
// After the texture snapshot only when set, so a default copy keeps its size (docs/reference/nvn-objects.md#copy-strides-are-command-buffer-state)
typedef struct { int64_t row, image; } p_copy_strides;
#define COPY_STRIDES_OFF ((((sizeof(p_copy_b2t) + 7) & ~(size_t)7)) + sizeof(tex_t))
static inline const p_copy_strides *copy_strides(const void *a, uint32_t size) {
    return size >= 8 && size - 8 >= COPY_STRIDES_OFF + sizeof(p_copy_strides) ? (const p_copy_strides *)((const uint8_t *)a + COPY_STRIDES_OFF) : NULL;
}
// each texture snapped after the record
enum { DLSS_COLOR, DLSS_DEPTH, DLSS_MOTION, DLSS_OUTPUT, DLSS_BIAS, DLSS_EXPOSURE, DLSS_TEXTURES };
// its serial (p_dlss.net) is new at each Initialize
typedef struct { int32_t w, h, method, preset; uint32_t flags; } dlss_builder_t;
typedef struct { dlss_builder_t b; uint64_t serial; } dlss_net_t;
typedef struct { const tex_t *tex[DLSS_TEXTURES]; uint64_t net; int32_t iw, ih, ow, oh, base[DLSS_TEXTURES][2]; float mv[2], jitter[2], pre_exposure; uint32_t flags, reset; } p_dlss;
typedef struct { uint64_t addr, size; uint32_t value, _pad; } p_clear_buf;
typedef struct { float rgba[4]; } p_blend_color;
typedef struct { float ref; } p_alpha_ref;
typedef struct { uint16_t compute, offset; uint32_t size; uint8_t data[]; } p_push_const;   // at offset in the group's c[24] (block b at b * DMSH_PUSH_SIZE)
typedef struct { int32_t on; } p_rast_discard;

void mtl_exec(const uint8_t *handle);
void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value);
void mtl_exec_rest(const uint64_t *handles, int n);   // the handles of this submission after the current one (pass lookahead)
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4], uint64_t *copy);   // returns the ticket at once; period_s: minimum display time (0 = none); *copy: its snapshot copy (0 = none)
void mtl_present_wait(uint64_t ticket);                    // its blit committed to the GPU queue
void mtl_present_wait_texture(uint64_t ticket, uint64_t copy);   // with a snapshot, its copy has run on the GPU; else as mtl_present_wait
void mtl_present_flush(void);
void mtl_present_test_stall(uint64_t ns);
int mtl_display_hz(void);                                   // 0 without a window
void mtl_set_tex_pool_n(int n);                            // IDs outside it read as empty
void mtl_set_samp_pool_n(int n);
// a frame's GPU time is readable once all its command buffers completed (else −1)
void mtl_set_frame(uint64_t f);
void mtl_ub_present(double iv_ms);
double mtl_gpu_ms(uint64_t f);
void mtl_ab_gpu(uint64_t f, double gpu_ms, double frame_ms);
void xv_test_counts(int *dc, int *viol);
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1);   // mach ns; 0 if not all complete
void mtl_stats(uint64_t *passes, uint64_t *draws);
void mtl_args_stats(uint64_t *enc, uint64_t *reuse, uint64_t *patch);
void mtl_use_stats(uint64_t *call, uint64_t *skip);
uint64_t mtl_drawable_ns_take(void);
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full_rw_passes);
uint64_t mtl_udc_saved_bytes(void);
int mtl_has_window(void);
int mtl_fps_target(void);
int mtl_link_active(void);
int mtl_link_wait_tick(int vsyncs, uint64_t timeout_ns);   // vsyncs: the game's present interval
int mtl_link_wait_ticks(int ticks, uint64_t timeout_ns);
uint64_t mtl_link_drain_take_ns(void);   // ns spent draining standing frames since the last call
int mtl_link_rate(void);
void mtl_link_mode_set(int mode);   // DM_PRESENT_LINK's values, applied at the next present
double mtl_presented_time(uint64_t n);   // s; 0 = not ready, −1 = dropped; n from 1
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms);   // 0 = none (no link, or not taken yet)
int mtl_link_stalled(void);   // queued frames are dropped instead of shown
int mtl_snap_on(void);
uint64_t mtl_snap_acquire_wait(void);
void mtl_tex_init(const tex_t *t);      // the serial is a live texture's until mtl_tex_release
int mtl_tex_release(tex_t *t);         // 0: a copy's Finalize, the texture lives on in the object it was initialized in
void mtl_tex_pin(uint64_t serial);
void mtl_tex_unpin(uint64_t serial);
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
void mtl_spare_stats(uint64_t *held, uint64_t *trimmed, uint64_t *trimmed_mb);
unsigned mtl_cpu_tex_count(void);
uint64_t mtl_pkg_edge_skips(void);
uint64_t mtl_pkg_miss(void);
uint64_t mtl_upload_ns_take(void);
uint64_t mtl_upload_gpu_ns_take(void);
void mtl_create_stats(uint64_t *lib, uint64_t *pso);
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img);
void mtl_reg_tex(int id, const tex_t *t, const view_t *v);
void mtl_reg_tex_gone(int id);   // the ID stays registered, its pool finalized
void mtl_reg_image(int id, const tex_t *t, const view_t *v);   // declares image use even without the builder's IMAGE flag
// WriteTexels: uploaded from guest memory and re-uploaded on dirty pages, as packaged textures
void mtl_tex_cpu_written(const tex_t *t);
void mtl_tex_linear_cpu_written(const tex_t *t);   // except one viewed on its pool's buffer, which reads its storage itself
void mtl_tex_gpu_written(const tex_t *t);   // uncompressed textures over it are no longer taken from storage
int mtl_tex_gpu_owned(uint64_t serial);                                  // a render target or shader-writable: never re-uploaded
enum { MTL_REWRITE_UPLOADED, MTL_REWRITE_GPU, MTL_REWRITE_GPU_NOW };
int mtl_tex_cpu_rewritten(const tex_t *t);                               // the CPU wrote its pages after its first use (MTL_REWRITE_*)
// GPU writes into it go to its storage too
int nvn_ft_tracked(uint64_t serial);
int nvn_tex_level_range(const tex_t *t, unsigned level, unsigned layer, uint64_t *lo, uint64_t *hi);   // −1 out of range
// First 8 bytes of sha256(first 0x48 bytes of the NVN2 control section, +0x30 zeroed, + code area): at SetShaders (the section may be freed after), or at first use for code in no pool yet.
// Keyed by the program's serial, so copies of the NVNprogram find them. 0 = no such stage
int prog_key(const prog_t *p, unsigned stage, uint8_t key[8]);
void nvn_ctl_lens(const void *control, uint32_t *nctl, uint32_t *ncode);   // a section of another form: its 8 bytes of magic and version, no code
void mtl_reg_samp(int id, const samp_par *p);
void mtl_prog_release(prog_t *p);
void mtl_prog_unpin(prog_t *p); // submit lock held
void mtl_prog_snapshot(prog_t *copy, const prog_t *source); // submit lock held
void mtl_prog_prefetch(const prog_t *p);
void mtl_prog_forget(const prog_t *p);   // submit lock held
// > 0, increasing
uint64_t nvn_new_serial(void);
typedef void (*nvn_buffer_range_fn)(uint64_t addr, uint64_t size);
void nvn_buffer_write_ranges(uint64_t addr, uint64_t size, nvn_buffer_range_fn note);
// captures keep the live run's serials
void nvn_serial_floor(uint64_t s);
