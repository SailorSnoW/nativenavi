// Shader reflection json → fixed-length arrays, converted once per shader so draws touch no NSDictionary. The json's DMSH_KIND_* (dmsh_abi.h) map to NVN_RB_*.
// SUPPORT: Ryujinx's auxiliary constants; C1: the shader's c[1]; DRV0 / DRV2: driver segments c[0] (storage buffer addresses) / c[2] (handles)
#pragma once
#include <stdint.h>

enum { NVN_RB_CBUF = 1, NVN_RB_SAMP, NVN_RB_TEX, NVN_RB_IMG, NVN_RB_TBUF,
       NVN_RB_SBUFP, NVN_RB_SUPPORT, NVN_RB_C1, NVN_RB_DRV0, NVN_RB_DRV2, NVN_RB_SAMPT, NVN_RB_UBOP, NVN_RB_TEXU, NVN_RB_SAMPU, NVN_RB_SAMPI, NVN_RB_SBUFU, NVN_RB_TEXD, NVN_RB_SAMPD, NVN_RB_TEXA, NVN_RB_SAMPA, NVN_RB_TEXP, NVN_RB_SAMPP, NVN_RB_TEXH, NVN_RB_SAMPH, NVN_RB_BORDER, NVN_RB_PUSH, NVN_RB_GMEM, NVN_RB_GMEML, NVN_RB_IMGU, NVN_RB_TBUFU, NVN_RB_CBSIZE, NVN_RB_SBUFQ, NVN_RB_TBIAS, NVN_RB_SBLEN };
// GMEM: the pool table for global memory traced to no storage buffer; GMEML: its write log. TEXP / SAMPP: the whole pool indexed by the handle's ID (no slot traced)
// TEXA / SAMPA: n c2 handles indexed at run time, element k at off + 8k. TEXD / SAMPD: Ryujinx's untraced handle (type 4), n slots by ID, c2 holding slot indices
// SAMPI: constant sampler ID (reg). UBOP: compute uniform slot r's address (Maxwell compute reads uniforms through c0). SAMPT: sampler from texture slot r's handle
enum { NVN_DPF_FLOAT = 0, NVN_DPF_UINT, NVN_DPF_SINT, NVN_DPF_DEPTH };
enum { NVN_CT_FLOAT = 0, NVN_CT_UINT, NVN_CT_INT };                               // vertex input component type
// 32 texture and 32 sampler slots a stage, its buffers, images and segments, one border twin per sampler
#define NVN_REFL_MAXB 128
#define NVN_REFL_MAXARG 768   // an array binding takes n (a c2 handle array 301, 602 with its samplers)
#define NVN_REFL_MAXV 32

typedef struct {
    uint8_t kind, rw;        // rw: read_write / write / device
    uint8_t dtt, dpf;        // dummy for an unbound slot: MTLTextureType, pixel class
    uint16_t reg, aid;       // NVN slot, argument buffer index
    uint16_t off;            // TEXU / SAMPU: the handle's byte in the uniform buffer; SBUFU: the 64-bit address's
    uint16_t n;              // array length (entries aid .. aid + n - 1)
    uint16_t soff;           // a second word ORed into the handle (type 1): TEXA / SAMPA its c2 byte; TEXU / SAMPU / TBUFU its byte + 1 in slot sreg; 0 = none
    uint8_t sreg;
    uint8_t f1d;             // read by 1D fetches as 2D rows of 16384 texels: a buffer texture is bound as such rows
    uint8_t atom;            // image atomics: the texture must allow them; 2: 64-bit (RG32Uint)
    uint16_t fixw;           // words read at fixed offsets: a shorter binding is padded with zeros
    uint8_t bw;              // border twin: its bit in the BORDER block + 1; 0 = ordinary
    uint8_t raw_id;          // the uniform word is the sampler ID, not a handle with it in the high 12 bits
    uint16_t tbx;            // offset table (TBIAS) index + 1; TEX* with f1d: of row leads and widths (TEXD / TEXA: element k at tbx - 1 + k); 0 = none
    uint8_t sbx;             // length table (SBLEN) slot + 1, bounding its accesses; 0 = unbounded
} nvn_rbind;
typedef struct {
    int nb, abi, nv;         // abi: argument buffer slot
    nvn_rbind b[NVN_REFL_MAXB];
    struct { uint8_t loc, ct, nvn, sh[3]; } v[NVN_REFL_MAXV];   // loc: Metal attribute; nvn: NVN attribute; sh[ct]: the shadow of that type in <key>_bv (0xff = none)
    uint8_t has_vin;
    uint16_t tg[3];
    uint32_t tg_spill;       // threadgroup memory bytes taken from a device buffer at DMSH_TG_SLOT
    uint32_t loop_sites;     // 0: no containment state to bind
    uint8_t split_words;     // two passes: pass 0 writes this many u32s a thread at DMSH_SPLIT_SLOT; 0 = one
    uint8_t mesh_spill;      // stage array in a scratch buffer at DMSH_MESH_SCRATCH_SLOT
    struct { uint8_t reg; uint16_t word; uint32_t value; } first[4]; uint8_t nfirst;   // stores the host makes before each dispatch
    uint8_t route_b, has_c1;
    uint8_t bindless;        // TEXU / SAMPU: textures known only at replay
    uint8_t border;          // index in b + 1, 0 = none
    uint8_t dyn;             // TEXD / SAMPD: c2 holds slot indices
    uint16_t tbias_n;
    uint8_t sblen;
    uint8_t pool;            // TEXP / SAMPP (sets bindless): the argument block holds pool addresses, which change when a pool is rebuilt
    uint8_t htab;            // TEXH / SAMPH (sets bindless): likewise
    uint8_t drv_read[3];     // [0], [2]: reads c0 / c2 at run time
    uint16_t c1_len;
    // First-draw coverage proof (missing = not provable): position xyz from attribute pos_attr (-1 = no), w = 1 (pos_w1) or its .w
    int8_t pos_attr; uint8_t pos_w1, may_discard, writes_smask, color_mask, color_known;   // color_mask can be trusted
    uint8_t fcols, ucols, scols, fcols_known;   // outputs declared float / uint / sint; a target of another class takes the typed-output entry point
    uint8_t reads_prim_id;
    uint8_t pos_far;         // z = w: drawn with a depth bias past far and depth clamping
    // SPH output map (word 18): 4 bits per colour output, RGBA from the low bit; the GPU writes only these. omap_known 0: no component mask
    uint32_t omap; uint8_t omap_known;
    // Readable-tile version <key>_bf: nfc samples (texture slot fc_reg, output fc_locn); fc_mixed slots must not read the tile; uv_attr[locn]: .xy's attribute (-1 = no)
    uint8_t nfc, fc_reg[32], fc_locn[32], nfo, fo_color[4]; uint64_t fc_mixed[4]; int8_t uv_attr[32];   // slots 128+: separate textures
    // .xy of output locn = fma(attribute uv_aff_attr[locn], scale, offset); uniform reference: (slot << 16) | (vector << 2) | component
    int8_t uv_aff_attr[32]; uint32_t uv_aff[32][4];   // [sx, ox, sy, oy]
    // fc_proj bit i: sample i reads at m_X.xy / m_X.w; scr_locn bit K: output K is gl_Position's ((x + w) / 2, (w − y) / 2, ·, w)
    uint32_t fc_proj, scr_locn;
    // bit i: sample i reads at gl_FragCoord.xy * fc_scale[i] (uniform references); its fc_locn is 0xff
    uint32_t fc_fpos; uint32_t fc_scale[32][2];
    // read on argument-block reuse as well as a fresh encode
    uint8_t ngptr; struct { uint8_t reg; uint16_t off; } gptr[64];   // DMSH_GPTR_MAX, checked in nvn_refl.m
    uint8_t *c1;             // c1_len bytes, malloc'd (NULL without one)
} nvn_refl;

#ifdef __OBJC__
#import <Foundation/Foundation.h>
// 0 on success, else the reason in err
int nvn_refl_from_json(NSDictionary *j, nvn_refl *out, char *err, int errlen);
#endif
