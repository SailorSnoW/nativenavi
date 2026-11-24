// NVN border colours beyond Metal's three (0 transparent black, 1 opaque black, 2 opaque white): a twin sampler of colour bw lets the shader correct a filtered sample,
// r = s_n + (s_w - s_n) k, k = (B - Bn) / (Bw - Bn) per channel; exact, filtering is linear in the border colour
#pragma once
#include <stdint.h>
#include <string.h>

static const float nvn_mtl_border[3][4] = { {0, 0, 0, 0}, {0, 0, 0, 1}, {1, 1, 1, 1} };
// wrap: NVN 4 mirror-clamp-to-border, 5 clamp-to-border; reduction: 1 min, 2 max. Returns 1 when a twin is needed (bw, k set)
static inline int nvn_border(const float B[4], const uint8_t wrap[3], int reduction, int *bn, int *bw, float k[4]) {
    *bn = B[3] > 0.5f ? (B[0] > 0.5f ? 2 : 1) : 0;
    for (int c = 0; c < 3; c++) if (!memcmp(B, nvn_mtl_border[c], sizeof nvn_mtl_border[c])) { *bn = c; return 0; }
    int border = 0;
    for (int i = 0; i < 3; i++) border |= wrap[i] == 4 || wrap[i] == 5;
    if (!border || reduction == 1 || reduction == 2) return 0;
    for (int pass = 0; pass < 2; pass++) {
        if (pass) *bn = 0;
        for (int c = 0; c < 3; c++) {
            if (c == *bn) continue;
            int ok = 1;
            for (int ch = 0; ch < 4; ch++) ok &= nvn_mtl_border[c][ch] != nvn_mtl_border[*bn][ch] || B[ch] == nvn_mtl_border[*bn][ch];
            if (!ok) continue;
            *bw = c;
            for (int ch = 0; ch < 4; ch++) { float d = nvn_mtl_border[c][ch] - nvn_mtl_border[*bn][ch]; k[ch] = d ? (B[ch] - nvn_mtl_border[*bn][ch]) / d : 0; }
            return 1;
        }
    }
    return 0;
}
