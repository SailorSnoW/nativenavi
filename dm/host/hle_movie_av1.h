// AV1 decoding on VideoToolbox or dav1d for nn::codec's decoders
#pragma once
#include <stddef.h>
#include <stdint.h>

typedef struct mav1 mav1;
// valid until the next call; matrix: BT.601 = 1, BT.709 = 2, BT.2020 = 5; dav1d decodes ahead, so ts is the picture's own unit's
typedef struct { const uint8_t *y, *uv; size_t ystride, uvstride; int w, h, matrix, full_range; int64_t ts; } mav1_picture;

struct movie_vt_control;
mav1 *mav1_new(struct movie_vt_control *control);
void mav1_free(mav1 *a);
void mav1_reset(mav1 *a);
// 1: *out is the oldest picture not given; 0: nothing to show yet; -1: refused
int mav1_decode(mav1 *a, const uint8_t *data, size_t n, int64_t ts, mav1_picture *out);
// drain 1 also waits for frames dav1d still decodes; once it gave a picture, call until 0 before the next mav1_decode (dav1d_step)
int mav1_next(mav1 *a, int drain, mav1_picture *out);
const char *mav1_backend(const mav1 *a);   // "VideoToolbox" (M3 and later) or "dav1d"
// for its test: an av1C record from a sequence header OBU; 0 when unreadable
int mav1_av1c(const uint8_t *obu, size_t n, uint8_t c[4], int *w, int *h);
