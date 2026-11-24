#pragma once
#include <stdint.h>
#include <string.h>
#include "nvn_obj.h"
#include "nvn_pso_ds.h"

typedef struct {
    const void *vs, *fs; uint32_t cfmt[8], dfmt, sfmt, samples, blend_en, chmask, a2c; blend_st blend[8];
    va_st attr[16]; vs_st stream[16];
    uint8_t lmap[8]; const void *fsfn;
    uint32_t atest, logic;   // 1 = alpha test / logic op (fragment from <key>_bx); added fields go last so an older prewarm row reads them as 0
    va_st attr_hi[16];       // NVN2's attributes 16 to 31
    uint32_t border, border_spare;   // 1 = a bound sampler has a twin: shaders keep their border colour corrections; border_spare pads to 8 bytes
} pso_key;
static inline va_st pso_key_attr(const pso_key *k, int a) { return a < 16 ? k->attr[a] : k->attr_hi[a - 16]; }

typedef struct {
    uint8_t kind, fbmask;
    int8_t fx[32];
} pso_fz_desc;

typedef struct { char vs[24], fs[24]; uint8_t vs_stage, fs_stage, merged, fz; pso_fz_desc fz_desc; pso_key k; } pso_skey;

// docs/reference/shaders.md, "Pipeline keys omit unused colour state"
static inline void pso_key_norm(pso_key *k, int merged) {
    pso_ds_pair(&k->dfmt, &k->sfmt);
    for (int i = 0; i < 8; i++) {
        if (!merged && !k->cfmt[i]) { k->blend_en &= ~(1u << i); k->chmask &= ~(0xfu << (4 * i)); }
        if (!(k->blend_en >> i & 1)) memset(&k->blend[i], 0, sizeof k->blend[i]);
    }
}

static inline void pso_skey_from(const pso_key *k, NvShader *vs, NvShader *fs, int merged, const pso_fz_desc *fz, pso_skey *out) {
    memset(out, 0, sizeof *out);
    memcpy(&out->k, k, sizeof out->k);
    out->k.vs = out->k.fs = out->k.fsfn = NULL;
    if (vs) { memcpy(out->vs, vs->key, sizeof out->vs); out->vs_stage = (uint8_t)vs->stage; }
    if (fs) { memcpy(out->fs, fs->key, sizeof out->fs); out->fs_stage = (uint8_t)fs->stage; }
    out->merged = merged != 0;
    out->fz = k->fsfn != NULL;
    if (fz) memcpy(&out->fz_desc, fz, sizeof out->fz_desc);
}

static inline void pso_skey_hex(const pso_skey *key, char *out) {
    static const char hex[] = "0123456789abcdef";
    const uint8_t *p = (const uint8_t *)key;
    for (size_t i = 0; i < sizeof *key; i++) { out[i * 2] = hex[p[i] >> 4]; out[i * 2 + 1] = hex[p[i] & 15]; }
    out[sizeof *key * 2] = 0;
}

static inline int pso_hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static inline int pso_skey_parse(const char *hex, pso_skey *out) {
    // shorter rows predate border, attr_hi or atest / logic: those stay 0
    size_t n = sizeof *out * 2, old2 = n - 2 * (sizeof out->k.border + sizeof out->k.border_spare), old1 = old2 - 2 * sizeof out->k.attr_hi,
           old = old1 - 2 * (sizeof out->k.atest + sizeof out->k.logic);
    if (!hex || (strlen(hex) != n && strlen(hex) != old2 && strlen(hex) != old1 && strlen(hex) != old)) return -1;
    if (strlen(hex) != n) { n = strlen(hex); memset(out, 0, sizeof *out); }
    uint8_t *p = (uint8_t *)out;
    for (size_t i = 0; i < n; i += 2) {
        int a = pso_hex_digit(hex[i]), b = pso_hex_digit(hex[i + 1]);
        if (a < 0 || b < 0) return -1;
        p[i / 2] = (uint8_t)(a << 4 | b);
    }
    pso_key_norm(&out->k, out->merged);
    return 0;
}

// M rows: no tile-specialized fragment or vertex layout; the object shader is carried separately
static inline int pso_mesh_skey_parse(const char *hex, pso_skey *out) {
    if (pso_skey_parse(hex, out) || !out->vs[0] || (out->vs_stage != 4 && out->vs_stage != 7) ||
        (out->fs[0] && out->fs_stage != 1) || out->merged || out->fz ||
        out->k.vs || out->k.fs || out->k.fsfn || !pso_ds_valid(out->k.dfmt, out->k.sfmt)) return -1;
    out->vs_stage = 4;   // older S rows may hold NVN2's stage 7
    memset(out->k.attr, 0, sizeof out->k.attr);
    memset(out->k.attr_hi, 0, sizeof out->k.attr_hi);
    memset(out->k.stream, 0, sizeof out->k.stream);
    return 0;
}
