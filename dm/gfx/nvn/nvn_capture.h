// NVN frame capture format and record relocation, shared by the host, the replayer and t_capture.c: plain C, no Metal.
// Record pointers are stored as CAP_ADDR (region, offset) or CAP_OBJ (kind, index); submit 0 holds the starting state, submit k what changed before it.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include "nvn_obj.h"

#define CAP_MAGIC "NVNCAP\0\0"
#define CAP_VERSION 1u

#define CAP_TAG(a, b, c, d) ((uint32_t)(a) | (uint32_t)(b) << 8 | (uint32_t)(c) << 16 | (uint32_t)(d) << 24)
enum {
    CAP_HEAD = CAP_TAG('H', 'E', 'A', 'D'),   // key \t value text lines
    CAP_LAYT = CAP_TAG('L', 'A', 'Y', 'T'),
    CAP_STAT = CAP_TAG('S', 'T', 'A', 'T'),
    CAP_REGN = CAP_TAG('R', 'E', 'G', 'N'),
    CAP_PADD = CAP_TAG('P', 'A', 'D', 'D'),
    CAP_PDIF = CAP_TAG('P', 'D', 'I', 'F'),   // {u32 region, u32 0, u64 off, u64 len, bytes}...
    CAP_POBJ = CAP_TAG('P', 'O', 'B', 'J'),
    CAP_TOBJ = CAP_TAG('T', 'O', 'B', 'J'),   // {u32 index, u32 0, tex_t (relocated)}...
    CAP_PROG = CAP_TAG('P', 'R', 'O', 'G'),
    CAP_DPOL = CAP_TAG('D', 'P', 'O', 'L'),
    CAP_SYNC = CAP_TAG('S', 'Y', 'N', 'C'),   // u32 count
    CAP_TREG = CAP_TAG('T', 'R', 'E', 'G'),
    CAP_SREG = CAP_TAG('S', 'R', 'E', 'G'),
    CAP_TKND = CAP_TAG('T', 'K', 'N', 'D'),
    CAP_TPIX = CAP_TAG('T', 'P', 'I', 'X'),   // cap_tpix + cap_tsub + bytes per level and slice
    CAP_SEGM = CAP_TAG('S', 'E', 'G', 'M'),   // {u32 index, u32 length, bytes}
    CAP_SUBM = CAP_TAG('S', 'U', 'B', 'M'),   // {u32 n, u32 segment[n]}
    CAP_PRES = CAP_TAG('P', 'R', 'E', 'S'),
    CAP_PHSH = CAP_TAG('P', 'H', 'S', 'H'),   // {u32 region, u32 blocks, u64 hash[blocks]}... of 4 KiB blocks after the frame
    CAP_LPIX = CAP_TAG('L', 'P', 'I', 'X'),   // optional: presented pixels, level 0, rows packed
    CAP_FAIL = CAP_TAG('F', 'A', 'I', 'L'),   // readers must reject a capture holding it
    CAP_END_ = CAP_TAG('E', 'N', 'D', ' '),
};
enum { CAP_RAW = 0, CAP_LZFSE = 1 };
typedef struct { uint32_t tag, codec; uint64_t raw, stored; uint32_t submit, pad; } cap_chunk;

// Host addresses stay below 2^48: a set top byte marks an encoded value
#define CAP_ADDR_TAG 0xCAull
#define CAP_REGION_BITS 20
#define CAP_OFF_BITS 36
static inline uint64_t cap_addr(uint32_t region, uint64_t off) { return CAP_ADDR_TAG << 56 | (uint64_t)region << CAP_OFF_BITS | off; }
static inline int cap_is_addr(uint64_t v) { return v >> 56 == CAP_ADDR_TAG; }
static inline uint32_t cap_addr_region(uint64_t v) { return (uint32_t)(v >> CAP_OFF_BITS) & ((1u << CAP_REGION_BITS) - 1); }
static inline uint64_t cap_addr_off(uint64_t v) { return v & ((1ull << CAP_OFF_BITS) - 1); }
enum { CAP_K_TEX = 1, CAP_K_TEXIN, CAP_K_PROG, CAP_K_DPOOL, CAP_K_SYNC, CAP_K_SEG, CAP_K_POOL, CAP_K_N };   // CAP_K_TEXIN: a tex_t snapshot inside the record, index = its payload offset
static inline uint64_t cap_obj(int kind, uint64_t index) { return (0xC0ull + (uint64_t)kind) << 56 | index; }
static inline int cap_obj_kind(uint64_t v) { uint64_t t = v >> 56; return t > 0xC0 && t < 0xC0 + CAP_K_N ? (int)(t - 0xC0) : 0; }
static inline uint64_t cap_obj_index(uint64_t v) { return v & ((1ull << 56) - 1); }

typedef struct { uint32_t index, kind; uint64_t old_lo, size; } cap_regn;   // kind 0: pool storage, 1: memory outside the pools
typedef struct { uint64_t mem, size; } cap_padd;                             // mem: CAP_ADDR
typedef struct { uint32_t index, flags; uint64_t mem, size; int32_t vmode, pad; } cap_pobj;
// iface[s]: stage s's binding fingerprint, so a replay whose shaders bind differently stops
typedef struct { uint32_t index, nstage; uint64_t serial; uint8_t key[6][8]; uint8_t ok, pad[7]; uint64_t iface[6]; } cap_prog;
typedef struct { uint32_t index; int32_t n; uint64_t pool; int64_t off; int32_t tex_base, samp_base; } cap_dpol;   // pool: CAP_OBJ(CAP_K_POOL)
// gone: the ID stays registered but its pool was finalized: t keeps neither pool nor storage address
typedef struct { int32_t tid; uint8_t ok, has_v, gone, pad; tex_t t; view_t v; } cap_treg;
typedef struct { int32_t sid, pad; samp_par p; } cap_sreg;
// cpu: uploaded from its pool; the replay marks it CPU-written to take the same path
enum { CAP_TK_LIVE = 1, CAP_TK_CPU = 2 };
typedef struct { tex_t t; uint32_t kind, pad; } cap_tknd;   // t: relocated
typedef struct { tex_t t; uint32_t pixfmt, type, width, height, depth, levels, slices, samples, nsub, pad; } cap_tpix;   // t: relocated
typedef struct { uint32_t level, slice, w, h, d, plane; uint64_t row, img, bytes; } cap_tsub;   // plane 0: color or depth, 1: stencil
typedef struct { uint64_t tex; int32_t crop[4]; uint8_t sha256[32]; uint64_t bytes; uint32_t width, height, pixfmt, frame; double capture_ms; } cap_pres;   // tex: CAP_OBJ(CAP_K_TEX)

// len 0 = a buffer of unknown extent; writable: the replay writes there
typedef struct {
    int (*addr)(void *ctx, uint64_t *field, uint64_t len, int writable);
    int (*obj)(void *ctx, int kind, uint64_t *field, uint8_t *payload, uint32_t size);
} cap_ops;
// 0 = an op the walk does not know: the capture fails rather than write host pointers
int cap_rec_fields(uint32_t op, uint8_t *payload, uint32_t size, const cap_ops *ops, void *ctx);
int cap_tex_fields(tex_t *t, const cap_ops *ops, void *ctx);
const char *cap_layout(void);
int cap_layout_matches(const char *layout, size_t n);
// Version 1: physical submission order only. Apart from cap_rec_fields, which the scheduler also uses.
const char *cap_record_unsupported(uint32_t op);
typedef struct { _Atomic uint64_t first; _Atomic unsigned unsupported; } cap_queue_state;
enum { CAP_QUEUE_MULTIPLE = 1, CAP_QUEUE_WAIT = 2 };
void cap_queue_note(cap_queue_state *state, uint64_t serial, int waits);
const char *cap_queue_unsupported(const cap_queue_state *state);

typedef struct { FILE *f; uint64_t raw, stored; int err; } cap_writer;
int cap_open(cap_writer *w, const char *path);
void cap_put(cap_writer *w, uint32_t tag, uint32_t submit, const void *data, uint64_t n, int compress);
int cap_close(cap_writer *w);
typedef struct { uint8_t *buf; size_t n, pos; } cap_reader;
int cap_load(cap_reader *r, const char *path);
int cap_next(cap_reader *r, cap_chunk *c, uint8_t **data);   // 1 = a chunk (*data malloc'd, free it), 0 = end, -1 = corrupt
void cap_unload(cap_reader *r);
typedef struct { uint8_t *p; size_t n, cap; } cap_buf;
void cap_buf_put(cap_buf *b, const void *data, size_t n);
static inline void cap_buf_free(cap_buf *b) {
    free(b->p);
    b->p = NULL;
    b->n = b->cap = 0;
}

// Under the submit lock, before the replay; nvn_cap_present before the present is queued
void nvn_cap_submit(int n, const uint64_t *handles);
void nvn_cap_present(const tex_t *t, const int crop[4]);
// At logical enqueue, before the scheduler removes waits. Thread-safe; never waits for GPU work.
void nvn_cap_queue(uint64_t serial, int waits);
