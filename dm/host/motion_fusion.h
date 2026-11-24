// Controller motion fusion for the six-axis sensor. Pure C.
// In: GCMotion acceleration in G (CoreMotion: face up = (0, 0, -1)), rotation in rad/s. Out: SixAxisSensorState in G, revolutions/s, revolutions.
// Switch frame: x right, y forward, z up out of the face. The axis map is a signed permutation; rotation is multiplied by its determinant.
// The gyro is integrated and tilt pulled toward gravity (Mahony) while |acc| is near 1 G; yaw drifts, as on the console.
#pragma once
#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>

typedef struct { int src[3]; float sign[3]; float acc_sign; float det; } motion_map;
typedef struct {
    float R[3][3];        // controller -> world (columns = its axes)
    float angle[3];       // revolutions
    float acc[3], gyro[3];// G, revolutions / s
    int primed;
} motion_state;

// 0 and the identity on a malformed string
static inline int motion_map_parse(motion_map *m, const char *s) {
    motion_map id = {{0, 1, 2}, {1, 1, 1}, 1, 1};
    *m = id;
    if (!s || !*s) return 1;
    motion_map r = id;
    int used = 0;
    for (int i = 0; i < 3; i++) {
        while (*s == ' ') s++;
        float sg = 1;
        if (*s == '-') { sg = -1; s++; } else if (*s == '+') s++;
        int a = *s == 'x' || *s == 'X' ? 0 : *s == 'y' || *s == 'Y' ? 1 : *s == 'z' || *s == 'Z' ? 2 : -1;
        if (a < 0 || used >> a & 1) return 0;
        used |= 1 << a;
        r.src[i] = a;
        r.sign[i] = sg;
        s++;
        while (*s == ' ') s++;
        if (i < 2) { if (*s != ',') return 0; s++; }
    }
    if (*s) return 0;
    // sign product times the permutation's parity
    int inv = (r.src[0] > r.src[1]) + (r.src[0] > r.src[2]) + (r.src[1] > r.src[2]);
    r.det = r.sign[0] * r.sign[1] * r.sign[2] * (inv & 1 ? -1.f : 1.f);
    *m = r;
    return 1;
}
static inline void motion_map_apply(const motion_map *m, const float in[3], float out[3], float scale) {
    for (int i = 0; i < 3; i++) out[i] = m->sign[i] * in[m->src[i]] * scale;
}
static inline void motion_reset(motion_state *s) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < 3; i++) s->R[i][i] = 1;
    s->acc[2] = -1;
}
static inline void m3_mul(float o[3][3], const float a[3][3], const float b[3][3]) {
    float t[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) t[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j] + a[i][2] * b[2][j];
    memcpy(o, t, sizeof t);
}
static inline void m3_orthonormalize(float R[3][3]) {   // Gram-Schmidt
    float c[3][3];
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) c[j][i] = R[i][j];
    for (int k = 0; k < 3; k++) {
        for (int p = 0; p < k; p++) {
            float d = c[k][0] * c[p][0] + c[k][1] * c[p][1] + c[k][2] * c[p][2];
            for (int i = 0; i < 3; i++) c[k][i] -= d * c[p][i];
        }
        float n = sqrtf(c[k][0] * c[k][0] + c[k][1] * c[k][1] + c[k][2] * c[k][2]);
        if (n < 1e-6f) n = 1;
        for (int i = 0; i < 3; i++) c[k][i] /= n;
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = c[j][i];
}
// dt in seconds (0 first); k = tilt gain (rad/s per unit error)
static inline void motion_update(motion_state *s, const motion_map *m, const float acc_raw[3], const float rot_raw[3], float dt, float k) {
    for (int i = 0; i < 3; i++) if (!isfinite(acc_raw[i]) || !isfinite(rot_raw[i])) return;
    if (!isfinite(dt) || dt < 0) dt = 0;
    if (!isfinite(k) || k < 0) k = 0;
    float a[3], w[3];
    motion_map_apply(m, acc_raw, a, m->acc_sign);
    motion_map_apply(m, rot_raw, w, m->det);
    if (!s->primed || dt <= 0 || dt > 0.25f) {
        if (!s->primed) {
            float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
            if (n > 0.5f && n < 1.5f) {
                // R's third row = -g_body / |g|
                float z[3] = {-a[0] / n, -a[1] / n, -a[2] / n}, y0[3] = {0, 1, 0};
                // held upright: another heading reference
                if (fabsf(z[1]) > 0.99f) { y0[0] = 1; y0[1] = 0; }
                float d = y0[0] * z[0] + y0[1] * z[1] + y0[2] * z[2];
                float y[3] = {y0[0] - d * z[0], y0[1] - d * z[1], y0[2] - d * z[2]};
                float yn = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
                if (yn > 1e-3f) {
                    for (int i = 0; i < 3; i++) y[i] /= yn;
                    float x[3] = {y[1] * z[2] - y[2] * z[1], y[2] * z[0] - y[0] * z[2], y[0] * z[1] - y[1] * z[0]};
                    for (int j = 0; j < 3; j++) { s->R[0][j] = x[j]; s->R[1][j] = y[j]; s->R[2][j] = z[j]; }
                }
            }
            s->primed = 1;
        }
    } else {
        // predicted gravity in the body = -(third row of R)
        float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
        if (k > 0 && n > 0.8f && n < 1.2f) {
            float gm[3] = {a[0] / n, a[1] / n, a[2] / n}, gp[3] = {-s->R[2][0], -s->R[2][1], -s->R[2][2]};
            float e[3] = {gm[1] * gp[2] - gm[2] * gp[1], gm[2] * gp[0] - gm[0] * gp[2], gm[0] * gp[1] - gm[1] * gp[0]};
            for (int i = 0; i < 3; i++) w[i] += k * e[i];
        }
        float th = sqrtf(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]) * dt;
        if (isfinite(th) && th > 1e-9f) {
            float u[3] = {w[0] * dt / th, w[1] * dt / th, w[2] * dt / th}, c = cosf(th), sn = sinf(th), v = 1 - c;
            float E[3][3] = {
                {c + u[0] * u[0] * v,        u[0] * u[1] * v - u[2] * sn, u[0] * u[2] * v + u[1] * sn},
                {u[1] * u[0] * v + u[2] * sn, c + u[1] * u[1] * v,        u[1] * u[2] * v - u[0] * sn},
                {u[2] * u[0] * v - u[1] * sn, u[2] * u[1] * v + u[0] * sn, c + u[2] * u[2] * v}};
            m3_mul(s->R, s->R, E);
            m3_orthonormalize(s->R);
        }
    }
    // without the tilt correction term
    float wraw[3];
    motion_map_apply(m, rot_raw, wraw, m->det / (2 * (float)M_PI));
    for (int i = 0; i < 3; i++) {
        s->gyro[i] = wraw[i];
        s->acc[i] = a[i];
        if (dt > 0 && dt <= 0.25f) {
            float angle = s->angle[i] + wraw[i] * dt;
            if (isfinite(angle)) s->angle[i] = angle;
        }
    }
}
// dir rows = the controller's axes in the world
static inline void motion_fill(const motion_state *s, float acc[3], float gyro[3], float angle[3], float dir[9]) {
    for (int i = 0; i < 3; i++) {
        acc[i] = s->acc[i];
        gyro[i] = s->gyro[i];
        angle[i] = s->angle[i];
    }
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) dir[3 * i + j] = s->R[j][i];
}
// Gyro rest offset = a still window's mean rotation. A slow turn about gravity looks still too: the first offset needs two agreeing windows and
// at most MOTION_REST_FIRST_MAX; later ones must agree with it. Ramped in, so the game's bias estimate sees no step (as switch2_proto.h s2_rest).
#define MOTION_REST_S 1.0
#define MOTION_REST_GYRO_SPAN 0.03f       // rad/s (1.7 deg/s)
#define MOTION_REST_ACC_SPAN 0.02f        // G
#define MOTION_REST_FIRST_MAX 0.0349f     // rad/s (2 deg/s)
#define MOTION_REST_AGREE 0.0218f         // rad/s (1.25 deg/s)
#define MOTION_REST_RAMP_S 0.75
typedef struct { int have, pending; double t0, sum[3], t_set; unsigned n; float bias[3], cand[3], from[3], gmin[3], gmax[3], amin[3], amax[3]; } motion_rest;
static inline void motion_rest_break(motion_rest *r) { r->n = 0; r->pending = 0; }
static inline void motion_rest_reset(motion_rest *r) { memset(r, 0, sizeof *r); }
static inline int motion_rest_near(const float a[3], const float b[3]) {
    for (int i = 0; i < 3; i++) if (fabsf(a[i] - b[i]) > MOTION_REST_AGREE) return 0;
    return 1;
}
static inline void motion_rest_now(const motion_rest *r, double t, float out[3]) {
    double f = r->have ? (t - r->t_set) / MOTION_REST_RAMP_S : 0;
    if (f < 0) f = 0;
    if (f > 1) f = 1;
    for (int i = 0; i < 3; i++) out[i] = r->have ? (float)(r->from[i] + (r->bias[i] - r->from[i]) * f) : 0;
}
static inline void motion_rest_set(motion_rest *r, const float m[3], double t) {
    float cur[3];
    motion_rest_now(r, t, cur);
    for (int i = 0; i < 3; i++) {
        r->from[i] = cur[i];
        r->bias[i] = r->have ? 0.5f * r->bias[i] + 0.5f * m[i] : m[i];
    }
    r->have = 1;
    r->t_set = t;
}
static inline void motion_rest_update(motion_rest *r, const float acc[3], const float rot[3], double t) {
    for (int i = 0; i < 3; i++) if (!isfinite(acc[i]) || !isfinite(rot[i])) { motion_rest_break(r); return; }
    if (!r->n) {
        r->t0 = t;
        for (int i = 0; i < 3; i++) {
            r->sum[i] = 0;
            r->gmin[i] = r->gmax[i] = rot[i];
            r->amin[i] = r->amax[i] = acc[i];
        }
    }
    for (int i = 0; i < 3; i++) {
        r->sum[i] += rot[i];
        if (rot[i] < r->gmin[i]) r->gmin[i] = rot[i];
        if (rot[i] > r->gmax[i]) r->gmax[i] = rot[i];
        if (acc[i] < r->amin[i]) r->amin[i] = acc[i];
        if (acc[i] > r->amax[i]) r->amax[i] = acc[i];
        if (r->gmax[i] - r->gmin[i] > MOTION_REST_GYRO_SPAN || r->amax[i] - r->amin[i] > MOTION_REST_ACC_SPAN) { motion_rest_break(r); return; }
    }
    r->n++;
    if (t - r->t0 < MOTION_REST_S || r->n < 10) return;
    float m[3];
    for (int i = 0; i < 3; i++) m[i] = (float)(r->sum[i] / r->n);
    r->n = 0;
    if (r->have) {
        if (motion_rest_near(m, r->bias)) motion_rest_set(r, m, t);
        return;
    }
    for (int i = 0; i < 3; i++) if (fabsf(m[i]) > MOTION_REST_FIRST_MAX) { r->pending = 0; return; }
    if (r->pending && motion_rest_near(m, r->cand)) {
        motion_rest_set(r, m, t);
        r->pending = 0;
        return;
    }
    memcpy(r->cand, m, sizeof r->cand);   // the next window must agree (any break drops it)
    r->pending = 1;
}
static inline void motion_rest_apply(const motion_rest *r, const float rot[3], double t, float out[3]) {
    float b[3];
    motion_rest_now(r, t, b);
    for (int i = 0; i < 3; i++) out[i] = rot[i] - b[i];
}

// Playout delay: samples come every 15-30 ms but the game reads 5 ms ticks up to now; a little late, every tick lies between two arrived samples.
// Delay = 90th percentile of the last MOTION_LAG_N (arrival_{k+1} - t_k), capped at MOTION_LAG_MAX_NS so a stall does not delay every tick.
#define MOTION_LAG_N 32
#define MOTION_LAG_PCT 90
#define MOTION_LAG_MAX_NS 40000000ull
#define MOTION_LAG_CLIP_NS 1000000000ull   // past a second is a silence, not a spacing
typedef struct { uint32_t need_us[MOTION_LAG_N]; unsigned n, next; uint64_t delay_ns; } motion_lag;
static inline void motion_lag_reset(motion_lag *l) { memset(l, 0, sizeof *l); }
static inline void motion_lag_add(motion_lag *l, uint64_t need_ns) {
    if (need_ns > MOTION_LAG_CLIP_NS) need_ns = MOTION_LAG_CLIP_NS;
    l->need_us[l->next] = (uint32_t)(need_ns / 1000);
    l->next = (l->next + 1) % MOTION_LAG_N;
    if (l->n < MOTION_LAG_N) l->n++;
    uint32_t s[MOTION_LAG_N];
    memcpy(s, l->need_us, sizeof s);
    for (unsigned i = 1; i < l->n; i++) {
        uint32_t v = s[i];
        unsigned j = i;
        while (j && s[j - 1] > v) { s[j] = s[j - 1]; j--; }
        s[j] = v;
    }
    unsigned k = l->n * MOTION_LAG_PCT / 100;
    if (k >= l->n) k = l->n - 1;
    uint64_t d = (uint64_t)s[k] * 1000;
    l->delay_ns = d < MOTION_LAG_MAX_NS ? d : MOTION_LAG_MAX_NS;
}
