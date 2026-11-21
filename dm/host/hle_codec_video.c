// nn::codec's AV1 decoder (the Switch 2's SDK), taken over on the host: the game's movies are AV1 (Event/Movie/*.webm), which its movie library (subsdk0, NintendoSDK_movie)
// demuxes and hands to nn::codec packet by packet. The SDK sends every call to a decoder service (IPC) the host does not serve. The SDK's protocol, read from its implementation
// (sdk 20.5.19 Ounce; this game's is 22.2.8, whose names the entries below carry):
//   - the client makes one image pool per output picture (MakeDecodeImagePreference → CalculateDecoderImagePoolBuffer → InitializeDecoderImagePool) and registers each with the
//     decoder under an index (RegisterAv1DecoderBuffer). Registering puts the index in the decoder's queue of acquirable buffers (8 deep), with a sync handle already signalled.
//   - Acquire takes the oldest index and its sync handle (description 3604 when the queue is empty); ReleaseAv1DecoderOutputBuffer gives the buffer to the decoder to fill and puts
//     it back in the queue at once, with a new sync handle that signals when the decoder has written a picture into it. The client waits for that handle through a sync point
//     event, then reads the picture's properties (description 3604 until it is filled) and the picture (ReadDecoderOutputImageNv12, the CPU route, which this game takes)
//   - SubmitAv1DecoderInputBuffer takes one packet with its time and an end-of-stream flag; description 3603 when the decoder cannot take more (the client retries after the
//     submission-ready event).
// Pictures go into the released buffers in release order. A submission queues its packet and returns, as the SDK's does (its decoder is another process): each decoder decodes on
// a thread of its own (cv_worker). Decoded inside the submission, a heavy packet held the client's thread for the whole decode, and that thread is the one that gives shown
// pictures back and takes decoded ones: the game's movie reader was held 30 ms and more once per group of pictures of its 1080p 60 fps title movie (dav1d), the game
// found its shown picture not yet given back, and repeated it for a frame or two. Pictures with no released buffer to go into wait in a short queue, and packets are refused
// once packets and pictures waiting fill it. The SDK's opaque types hold a host pointer at +0 (pool: 0x20 bytes are the SDK's; decoder: 0x400), the decoder's event lives in its type at +0x100.
// Decoded pictures are kept as NV12, the layout the game reads them in (cv_pic): a read is a copy. Not served: encrypted input and alias pools.
// A player can also take pictures the GPU route: GetDecoderImagePoolNativeImageHandle's handle goes through nvnNativeImage to an RGBA texture (hle_nvn.c "Native images"), so a
// pool whose handle was asked for keeps each picture as RGBA in the pool's own memory, which the texture is made over. The game links it and does not call it
#include "hle.h"
#include <pthread.h>
#include <stdbool.h>
#include <sys/mman.h>
#include "hle_movie_av1.h"   // AV1 on VideoToolbox or dav1d
#include "hle_movie_vt.h"

#define CV_MAGIC 0x56436d44u        // 'DmCV'
#define CV_SYNC 0x53436d44u         // 'DmCS': a sync handle the host made (dec id, index, generation)
#define CV_BUFS 32
#define CV_READY 4                  // packets and decoded pictures waiting (cv_pending) at which the submission-ready event goes down
#define CV_HOLD 12                  // and at which packets are refused. Mario Tennis Fever's player sends one packet per signal and ignores the result (nro0+0xd16130), so a signal in flight when the
                                    // event goes down must still find room: refused at 4, a packet about 6 s into its title movie was lost, and every frame after it was corrupt
#define CV_STAT 600                 // packets between two looks at the decode times (cv_stat)
#define CV_EVENT_OFF 0x100          // the submission-ready SystemEventType inside the decoder type
#define CV_R(desc) ((Result)(0x6f | ((desc) << 9)))   // nn::codec results (module 111)
#define CV_BUSY CV_R(3603)          // the decoder cannot take the packet now
#define CV_NONE CV_R(3604)          // no buffer to acquire / no picture in the buffer yet

// A decoded picture as the host keeps it: NV12 in tight rows, the layout the game reads (ReadDecoderOutputImageNv12): h rows of w luma bytes, then (h + 1) / 2 rows of
// Cb Cr pairs, uvw bytes each (w rounded up to even). matrix as mav1_picture's (BT.601 = 1, BT.709 = 2, BT.2020 = 5)
typedef struct { uint16_t w, h; uint8_t matrix, full; uint8_t px[]; } cv_pic;
static uint32_t pic_uvw(const cv_pic *p) { return (p->w + 1u) & ~1u; }
static const uint8_t *pic_uv(const cv_pic *p) { return p->px + (size_t)p->w * p->h; }
static cv_pic *pic_alloc(uint32_t w, uint32_t h) {
    cv_pic *p = malloc(sizeof *p + (size_t)w * h + (size_t)((w + 1) & ~1u) * ((h + 1) / 2)); if (!p) return NULL;
    p->w = (uint16_t)w; p->h = (uint16_t)h; p->matrix = 1; p->full = 0;
    return p;
}
static cv_pic *pic_new(const mav1_picture *s) {   // an owned copy of the decoder's picture, whose memory is the decoder's until its next call
    cv_pic *p = pic_alloc((uint32_t)s->w, (uint32_t)s->h); if (!p) return NULL;
    uint32_t w = p->w, h = p->h, uvw = pic_uvw(p), h2 = (h + 1) / 2;
    p->matrix = (uint8_t)s->matrix; p->full = s->full_range != 0;
    if (s->ystride == w) memcpy(p->px, s->y, (size_t)w * h); else for (uint32_t y = 0; y < h; y++) memcpy(p->px + (size_t)y * w, s->y + (size_t)y * s->ystride, w);
    uint8_t *uv = p->px + (size_t)w * h;
    if (s->uvstride == uvw) memcpy(uv, s->uv, (size_t)uvw * h2); else for (uint32_t y = 0; y < h2; y++) memcpy(uv + (size_t)y * uvw, s->uv + (size_t)y * s->uvstride, uvw);
    return p;
}
// The picture as the client's NV12 planes of w x h in tight rows; a smaller picture is padded with black, a larger one cropped
static void pic_nv12(const cv_pic *p, uint8_t *ydst, uint8_t *uvdst, int w, int h) {
    int cw = p->w < w ? p->w : w, ch = p->h < h ? p->h : h;
    if (cw < w || ch < h) { memset(ydst, 16, (size_t)w * h); memset(uvdst, 128, (size_t)w * h / 2); }
    if (p->w == w) memcpy(ydst, p->px, (size_t)w * ch); else for (int y = 0; y < ch; y++) memcpy(ydst + (size_t)y * w, p->px + (size_t)y * p->w, (size_t)cw);
    int n = 2 * ((cw + 1) / 2), rows = (ch + 1) / 2; if (n > w) n = w & ~1; if (rows > h / 2) rows = h / 2;
    const uint8_t *uv = pic_uv(p); uint32_t uvw = pic_uvw(p);
    if (uvw == (uint32_t)w && n == w) memcpy(uvdst, uv, (size_t)w * rows); else for (int y = 0; y < rows; y++) memcpy(uvdst + (size_t)y * w, uv + (size_t)y * uvw, (size_t)n);
}
// The picture as RGBA of w x h in tight rows (a native image, ReadDecoderOutputImageRgba): by the matrix and range it declares (BT.709, else BT.601), 16-bit fixed point; a
// smaller picture is padded with black, a larger one cropped
static void pic_rgba(const cv_pic *p, uint8_t *dst, int w, int h) {
    int full = p->full, b709 = p->matrix == 2;
    double ky = full ? 1.0 : 255.0 / 219.0, kc = full ? 1.0 : 255.0 / 224.0;
    double rv = b709 ? 1.5748 : 1.402, gu = b709 ? 0.1873 : 0.3441, gv = b709 ? 0.4681 : 0.7141, bu = b709 ? 1.8556 : 1.772;
    int Y = (int)(ky * 65536), RV = (int)(rv * kc * 65536), GU = (int)(gu * kc * 65536), GV = (int)(gv * kc * 65536), BU = (int)(bu * kc * 65536), y0 = full ? 0 : 16;
    int cw = p->w < w ? p->w : w, ch = p->h < h ? p->h : h;
    if (cw < w || ch < h) for (size_t i = 0; i < (size_t)w * h; i++) ((uint32_t *)dst)[i] = 0xff000000u;
    const uint8_t *uvp = pic_uv(p); uint32_t uvw = pic_uvw(p);
    #define CL(v) ((v) < 0 ? 0 : (v) > 255 ? 255 : (v))
    for (int y = 0; y < ch; y++) {
        const uint8_t *py = p->px + (size_t)y * p->w, *puv = uvp + (size_t)(y >> 1) * uvw;
        uint8_t *o = dst + (size_t)y * w * 4;
        for (int x = 0; x < cw; x++) {
            int l = (py[x] - y0) * Y, u = puv[x & ~1] - 128, v = puv[(x & ~1) + 1] - 128;
            int r = (l + RV * v + 32768) >> 16, g = (l - GU * u - GV * v + 32768) >> 16, b = (l + BU * u + 32768) >> 16;
            o[4 * x] = (uint8_t)CL(r); o[4 * x + 1] = (uint8_t)CL(g); o[4 * x + 2] = (uint8_t)CL(b); o[4 * x + 3] = 255;
        }
    }
    #undef CL
}
typedef struct { uint16_t w, h; uint32_t fmt, mode, magic; uint32_t pad[2]; } cv_pref;   // VideoDecoderImagePreference as the host fills it (returned in x8)
typedef struct { uint32_t magic; uint16_t w, h; uint32_t fmt, count; cv_pic **img; uint8_t *mem; uint64_t size; int slot; _Atomic int native; } cv_pool;   // mem, size: the game's pool memory; native: a handle was asked for (RGBA kept in mem); slot: its place in g_cv_pools + 1
typedef struct { uint32_t magic, dec, idx, gen; } cv_sync;   // nn::gs::NativeSyncHandle (16 bytes)
typedef struct { uint64_t n, ns, max, late; } cv_times;   // packets decoded, their time in all and the longest; late: pictures whose buffer was released before they were decoded
typedef struct {
    uint32_t magic, id; pthread_mutex_t mu; void *event; uint64_t submits, pictures;
    uint16_t w, h;
    mav1 *av1;   // VideoToolbox, or dav1d where the chip has no AV1 decoder (hle_movie_av1.c)
    movie_vt_control vt;
    struct { cv_pool *pool; uint32_t pidx; int reg, filled; uint32_t gen, filled_gen; uint16_t w, h; int64_t ts; uint8_t eos; } buf[CV_BUFS];
    uint32_t fifo[8], fhead, fcount;          // acquirable indices
    uint32_t pend[CV_BUFS], phead, pcount;    // released, waiting for a picture
    cv_pic *held[CV_HOLD]; int64_t held_ts[CV_HOLD]; uint8_t held_eos[CV_HOLD]; int nheld;
    int eos_pending;
    // The decoder's thread (cv_worker). The codec state above (av1) is its own from Initialize to Finalize: it decodes outside mu
    pthread_t th; pthread_cond_t work;         // work: a packet was queued, room for pictures came back, a Stop / Flush or Finalize came
    int quit, busy, reopen;                    // busy: the thread decodes a packet; reopen: a Stop / Flush came, so it makes a new codec state before its next packet
    uint32_t epoch;                            // counts Stop / Flush: the pictures of a packet taken before one are dropped
    struct { uint8_t *data; uint64_t size; int64_t ts; uint8_t eos; } q[CV_HOLD]; uint32_t qhead, qcount;   // the packets taken and not yet decoded, in order (copies)
    cv_times st, all;                          // decode times of the current run of CV_STAT packets and of the runs before it
} cv_dec;

static cv_dec *g_cv[16];
static pthread_mutex_t g_cv_mu = PTHREAD_MUTEX_INITIALIZER;   // lock order: g_cv_mu, then a decoder's mu, then g_spt_mu. A decoder's thread holds its mu while it writes a picture into a buffer
                                                              // (a conversion to RGBA for a native image), so the client's threads take both with dm_host_mutex_lock: waiting, they are blocked

static cv_dec *cv_of(void *t) { cv_dec *d = t ? *(cv_dec **)t : NULL; return d && d->magic == CV_MAGIC ? d : NULL; }
static cv_pool *pool_of(const void *t) { cv_pool *p = t ? *(cv_pool *const *)t : NULL; return p && p->magic == CV_MAGIC ? p : NULL; }

// Sync point events (nn::syncpt::sdk): the player's worker waits on one (in a MultiWait) for the sync handle of the picture it expects. Their library is set up by the graphics
// initialisation the host replaces, so CreateSyncPointEvent jumped through its null table (pc 0). The event type (0x80 bytes) holds a SystemEvent the host
// binds at +0 (0x30 bytes) and the awaited handle at +0x40; an armed one is signalled when its buffer is filled
typedef struct { uint32_t magic, armed; cv_sync want; } cv_spt;
#define SPT(e) ((cv_spt *)((uint8_t *)(e) + 0x40))
static void *g_spt[64];   // armed events (g_spt_mu)
static pthread_mutex_t g_spt_mu = PTHREAD_MUTEX_INITIALIZER;
static bool sync_reached(const cv_sync *s);
static void spt_fire(cv_dec *d, uint32_t i) {   // buffer i of d filled: signal the events waiting for it (decoder lock held)
    pthread_mutex_lock(&g_spt_mu);
    for (int k = 0; k < 64; k++) { void *e = g_spt[k]; cv_spt *t = e ? SPT(e) : NULL;
        if (t && t->want.dec == d->id && t->want.idx == i && d->buf[i].filled_gen >= t->want.gen) { t->armed = 0; g_spt[k] = NULL; hle_signal_system_event(e); } }
    pthread_mutex_unlock(&g_spt_mu);
}
static void spt_unarm(void *e) { pthread_mutex_lock(&g_spt_mu); for (int k = 0; k < 64; k++) if (g_spt[k] == e) g_spt[k] = NULL; pthread_mutex_unlock(&g_spt_mu); SPT(e)->armed = 0; }

// A native image: one picture's RGBA in the pool's memory, tight rows when 4 w is on the 64-byte stride NVN's linear textures take, and each picture on a 16 KB page, as a Metal buffer over it needs
#define CV_NATIVE_ALIGN 0x4000ull
static uint32_t native_stride(uint32_t w) { return (w * 4 + 63) & ~63u; }
static uint64_t native_size(uint32_t w, uint32_t h) { return ((uint64_t)native_stride(w) * h + CV_NATIVE_ALIGN - 1) & ~(CV_NATIVE_ALIGN - 1); }
static void pool_native_write(cv_pool *p, uint32_t i, const cv_pic *im) {
    if (!p->native || !p->mem || (i + 1) * native_size(p->w, p->h) > p->size) return;
    uint8_t *dst = p->mem + i * native_size(p->w, p->h);
    uint32_t stride = native_stride(p->w);
    if (stride == (uint32_t)p->w * 4) { pic_rgba(im, dst, p->w, p->h); return; }
    uint8_t *tmp = malloc((size_t)p->w * p->h * 4);
    if (!tmp) return;
    pic_rgba(im, tmp, p->w, p->h);
    for (uint32_t y = 0; y < p->h; y++) memcpy(dst + (size_t)y * stride, tmp + (size_t)y * p->w * 4, (size_t)p->w * 4);
    free(tmp);
}
static int cv_pending(const cv_dec *d) { return d->nheld + (int)d->qcount + d->busy; }   // what the decoder holds for the client: packets not yet decoded and pictures no released buffer has taken (decoder lock held)
// Pictures and end of stream into the released buffers, in order (decoder lock held). decoded: the decoder's thread calls, with the pictures of the packet it has just decoded:
// a buffer they go into was waiting for them
static void fill(cv_dec *d, int decoded) {
    int had = d->nheld;
    while (d->pcount && (d->nheld || d->eos_pending)) {
        uint32_t i = d->pend[d->phead]; d->phead = (d->phead + 1) % CV_BUFS; d->pcount--;
        typeof(d->buf[0]) *b = &d->buf[i];
        if (d->nheld) {
            cv_pic *im = d->held[0]; int64_t ts = d->held_ts[0]; uint8_t eos = d->held_eos[0];
            memmove(d->held, d->held + 1, sizeof d->held[0] * (CV_HOLD - 1)); memmove(d->held_ts, d->held_ts + 1, sizeof d->held_ts[0] * (CV_HOLD - 1));
            memmove(d->held_eos, d->held_eos + 1, CV_HOLD - 1); d->nheld--;
            b->w = im->w; b->h = im->h; b->ts = ts; b->eos = eos;
            if (b->pool && b->pidx < b->pool->count) { pool_native_write(b->pool, b->pidx, im); free(b->pool->img[b->pidx]); b->pool->img[b->pidx] = im; } else free(im);
            if (!d->pictures++) LOG("[codec] AV1 decoder %u: first picture %ux%u into buffer %u", d->id, b->w, b->h, i);
            dm_movie_picture(d, ts);   // the frame step follows its rate (hle_movie.c); its times are in ns
            if (decoded) d->st.late++;
        } else { b->w = b->h = 0; b->ts = 0; b->eos = 1; d->eos_pending = 0; }
        if (b->eos) LOG("[codec] AV1 decoder %u: end of stream after %llu pictures", d->id, (unsigned long long)d->pictures);
        b->filled = 1; b->filled_gen = b->gen;
        spt_fire(d, i);
    }
    if (had >= CV_HOLD && d->nheld < CV_HOLD) pthread_cond_signal(&d->work);   // the decoder's thread waited for room for its next packet's pictures
}
// The event says a packet can come: it rises when one was taken (took) or room came back (before: cv_pending before the change), not at every release (decoder lock held).
// A player may count each signal as one packet it may send (Mario Tennis Fever's): raised at every release, its count grew while a seek held back the samples, and the
// packets it then sent overran CV_HOLD
static void ready(cv_dec *d, int before, int took) {
    if (!d->event) return;
    if (cv_pending(d) >= CV_READY) hle_clear_system_event(d->event); else if (took || before >= CV_READY) hle_signal_system_event(d->event);
}
// Decode times, for a movie that stutters at a steady frame rate: a decoder slower than the movie leaves the client's released buffers waiting for pictures (late), and the
// client shows a picture again. One line for a run of CV_STAT packets with more late pictures than its start explains (the buffers released before the first picture wait for
// it: the registered ones at most, and a Stop or Flush begins a new run), and one at Finalize. Ocarina of Time's title movie on an M2 Pro: 0 to 3 in a run with the decoder
// ahead, 29 and 18 in the two heaviest with dav1d on one frame at a time, where the player showed 5 to 7 pictures twice
static void cv_stat_line(const cv_dec *d, const cv_times *s, const char *what) {
    LOG("[codec] AV1 decoder %u: %s%llu packets decoded in %.2f ms each, %.2f ms the longest; %llu pictures came after their buffer was released", d->id, what,
        (unsigned long long)s->n, s->n ? (double)s->ns / 1e6 / (double)s->n : 0.0, (double)s->max / 1e6, (unsigned long long)s->late);
}
static void cv_stat_sum(cv_dec *d) {   // the current run joins the earlier ones
    d->all.n += d->st.n; d->all.ns += d->st.ns; d->all.late += d->st.late; if (d->st.max > d->all.max) d->all.max = d->st.max;
    memset(&d->st, 0, sizeof d->st);
}
static void cv_stat(cv_dec *d, uint64_t dt) {   // one packet took dt (decoder lock held)
    d->st.n++; d->st.ns += dt; if (dt > d->st.max) d->st.max = dt;
    if (d->st.n < CV_STAT) return;
    uint64_t reg = 0; for (int i = 0; i < CV_BUFS; i++) reg += (uint64_t)d->buf[i].reg;
    if (d->st.late > reg) cv_stat_line(d, &d->st, "the decoder is behind the movie: of the last ");
    cv_stat_sum(d);
}

HLE_FORCE("_ZN2nn5codec5video10InitializeEv", "the decoder service is the host's (hle_codec_video.c)", Result, h_cv_init, void) { return 0; }
HLE_FORCE("_ZN2nn5codec5video8FinalizeEv", "the decoder service is the host's", Result, h_cv_fin, void) { return 0; }

HLE_FORCE("_ZN2nn5codec25MakeDecodeImagePreferenceEttNS0_24VideoDecoderOutputFormatENS0_15ImageTilingModeE", "the preference only carries the size and format to the host's pool",
          cv_pref, h_cv_pref, uint64_t w, uint64_t h, uint32_t fmt, uint32_t tiling) {   // the 16-bit sizes come unextended
    return (cv_pref){(uint16_t)w, (uint16_t)h, fmt, tiling, CV_MAGIC, {0, 0}};
}
HLE_FORCE("_ZN2nn5codec31CalculateDecoderImagePoolBufferEPmS1_RKNS0_27VideoDecoderImagePreferenceEj", "pool memory: room for RGBA pictures (a native image's texture is made over it; otherwise the host keeps its own copies)",
          Result, h_cv_poolsize, uint64_t *size, uint64_t *align, const cv_pref *p, uint32_t n) {
    *size = native_size(p->w, p->h) * (n ? n : 1); *align = CV_NATIVE_ALIGN;
    return 0;
}
static cv_pool *g_cv_pools[256];   // what a native image handle names (g_cv_mu): (slot + 1) << 8 | picture
static Result cv_pool_init(void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n) {
    cv_pool *o = calloc(1, sizeof *o); o->magic = CV_MAGIC; o->w = p->w; o->h = p->h; o->fmt = p->fmt; o->count = n ? n : 1; o->img = calloc(o->count, sizeof *o->img); o->mem = mem; o->size = size;
    *(cv_pool **)t = o;
    static int once; if (!once++) LOG("[codec] image pool %ux%u format %u, tiling or mode %u, %u picture(s): the host keeps the decoded pictures", p->w, p->h, p->fmt, p->mode, o->count);
    return 0;
}
HLE_FORCE("_ZN2nn5codec26InitializeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeEPvmRKNS0_27VideoDecoderImagePreferenceEj", "a host pool of decoded pictures",
          Result, h_cv_pool, void *t, void *mem, uint64_t size, const cv_pref *p, uint32_t n) { return cv_pool_init(t, mem, size, p, n); }
HLE_FORCE("_ZN2nn5codec24FinalizeDecoderImagePoolEPNS0_25VideoDecoderImagePoolTypeE", "frees the host pool", void, h_cv_poolfin, void *t) {
    cv_pool *p = pool_of(t); if (!p) return;
    dm_host_mutex_lock(&g_cv_mu);   // a decoder still holding a buffer of this pool keeps it, with no pool to write its picture into
    for (int k = 0; k < 16; k++) if (g_cv[k]) { dm_host_mutex_lock(&g_cv[k]->mu); for (int i = 0; i < CV_BUFS; i++) if (g_cv[k]->buf[i].pool == p) g_cv[k]->buf[i].pool = NULL; pthread_mutex_unlock(&g_cv[k]->mu); }
    if (p->slot) g_cv_pools[p->slot - 1] = NULL;
    pthread_mutex_unlock(&g_cv_mu);
    for (uint32_t i = 0; i < p->count; i++) free(p->img[i]);
    free(p->img); p->magic = 0; free(p); *(cv_pool **)t = NULL;
}
HLE_FORCE("_ZN2nn5codec36GetDecoderImagePoolNativeImageHandleEPKNS0_25VideoDecoderImagePoolTypeEj", "a handle the host's nvnNativeImage knows; the pool keeps its pictures as RGBA from now on",
          uint32_t, h_cv_native, const void *t, uint32_t i) {
    cv_pool *p = pool_of(t); if (!p || i >= p->count) return 0;
    dm_host_mutex_lock(&g_cv_mu);
    if (!p->slot) for (int k = 0; k < 256; k++) if (!g_cv_pools[k]) { g_cv_pools[k] = p; p->slot = k + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!p->slot) { static int once; if (!once++) LOG("[RED] nn::codec: more than 256 pools with native images"); return 0; }
    p->native = 1;
    return (uint32_t)p->slot << 8 | i;
}
// Blank native images: a picture the system would fill that the host has no source for (GameChat's camera pictures, hle_misc.c), all zeros. Their handles have the top bit set, apart from
// the decoder pools' (slot << 8 | image). The storage holds any uncompressed format up to 16 bytes a pixel, as the texture's format is known only when the game makes it
#define BLANK_MAX 32
static struct { uint32_t w, h; uint8_t *mem; uint64_t size; } g_blank[BLANK_MAX];
static uint32_t blank_stride(uint32_t w) { return (w * 16 + 63) & ~63u; }
uint32_t dm_native_image_blank(uint32_t w, uint32_t h) {   // a blank image's handle; 0 when there is no room
    uint64_t size = ((uint64_t)blank_stride(w) * h + CV_NATIVE_ALIGN - 1) & ~(CV_NATIVE_ALIGN - 1);
    void *mem = size ? mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0) : MAP_FAILED;
    if (mem == MAP_FAILED) return 0;
    dm_host_mutex_lock(&g_cv_mu);
    uint32_t k = 0; while (k < BLANK_MAX && g_blank[k].mem) k++;
    if (k < BLANK_MAX) { g_blank[k].w = w; g_blank[k].h = h; g_blank[k].mem = mem; g_blank[k].size = size; }
    pthread_mutex_unlock(&g_cv_mu);
    if (k == BLANK_MAX) { munmap(mem, size); LOG("[RED] native images: more than %d blank images", BLANK_MAX); return 0; }
    return 0x80000000u | (k + 1);
}
void dm_native_image_blank_free(uint32_t handle) {   // a texture on it must be finalized first, as on the console
    uint32_t k = (handle & 0x7fffffffu) - 1;
    if (!(handle >> 31) || k >= BLANK_MAX) return;
    dm_host_mutex_lock(&g_cv_mu);
    uint8_t *mem = g_blank[k].mem; uint64_t size = g_blank[k].size; memset(&g_blank[k], 0, sizeof g_blank[k]);
    pthread_mutex_unlock(&g_cv_mu);
    if (mem) munmap(mem, size);
}
// A native image handle's picture for hle_nvn.c: 1 and its memory, size and layout; 0 for a handle no pool or blank image holds
int dm_native_image(uint32_t handle, uint8_t **mem, uint64_t *size, uint32_t *w, uint32_t *h, uint32_t *stride) {
    uint32_t slot = handle >> 8, i = handle & 0xff; int ok = 0;
    dm_host_mutex_lock(&g_cv_mu);
    uint32_t k = (handle & 0x7fffffffu) - 1;
    if (handle >> 31 && k < BLANK_MAX && g_blank[k].mem) {
        *mem = g_blank[k].mem; *size = g_blank[k].size; *w = g_blank[k].w; *h = g_blank[k].h; *stride = blank_stride(g_blank[k].w);
        pthread_mutex_unlock(&g_cv_mu); return 1;
    }
    cv_pool *p = slot && slot <= 256 ? g_cv_pools[slot - 1] : NULL;
    if (p && p->mem && i < p->count && (i + 1) * native_size(p->w, p->h) <= p->size) {
        *mem = p->mem + i * native_size(p->w, p->h); *size = native_size(p->w, p->h); *w = p->w; *h = p->h; *stride = native_stride(p->w); ok = 1;
    }
    pthread_mutex_unlock(&g_cv_mu);
    return ok;
}
HLE_FORCE("_ZN2nn5codec29ReadDecoderOutputImageRgbaHdrEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "HDR pictures are not served: black",
          Result, h_cv_rgbahdr, const void *t, uint32_t i, uint64_t x, uint64_t y, uint64_t w, uint64_t h, void *dst, uint64_t stride, uint64_t size) {
    (void)t; (void)i; (void)x; (void)y; (void)w; (void)h; (void)stride;
    static int once; if (!once++) LOG("[RED] nn::codec::ReadDecoderOutputImageRgbaHdr: HDR pictures are not served, black");
    memset(dst, 0, size); return 0;
}
// The picture of pool image i (the region x, y, w, h; the game reads it whole) as RGBA or as NV12
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageRgbaEPKNS0_25VideoDecoderImagePoolTypeEjttttPvmm", "converts the host's picture (pic_rgba)",
          Result, h_cv_rgba, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *dst, uint64_t stride, uint64_t size) {
    cv_pool *p = pool_of(t); const cv_pic *im = p && i < p->count ? p->img[i] : NULL;
    if (!im) { memset(dst, 0, size); return 0; }
    if (x || y || stride != (uint64_t)w * 4 || size < (uint64_t)w * h * 4) { static int once; if (!once++) LOG("[RED] nn::codec::ReadDecoderOutputImageRgba: region %u,%u stride %llu: only whole tight pictures are served", x, y, (unsigned long long)stride); return 0; }
    pic_rgba(im, dst, w, h);
    return 0;
}
HLE_FORCE("_ZN2nn5codec26ReadDecoderOutputImageNv12EPKNS0_25VideoDecoderImagePoolTypeEjttttPvmmS4_mm", "a copy of the host's picture (pic_nv12)",
          Result, h_cv_nv12, const void *t, uint32_t i, uint16_t x, uint16_t y, uint16_t w, uint16_t h, void *ydst, uint64_t ystride, uint64_t ysize, void *uvdst, uint64_t uvstride, uint64_t uvsize) {
    cv_pool *p = pool_of(t); const cv_pic *im = p && i < p->count ? p->img[i] : NULL;
    if (x || y || ystride != w || uvstride != w || ysize < (uint64_t)w * h || uvsize < (uint64_t)w * h / 2) { static int once; if (!once++) LOG("[RED] nn::codec::ReadDecoderOutputImageNv12: region %u,%u strides %llu / %llu: only whole tight pictures are served", x, y, (unsigned long long)ystride, (unsigned long long)uvstride); return 0; }
    if (!im) { memset(ydst, 16, ysize); memset(uvdst, 128, uvsize); return 0; }
    pic_nv12(im, ydst, uvdst, w, h);
    return 0;
}

// The host decoder behind an Av1DecoderType: VideoToolbox or dav1d (hle_movie_av1.c), whose NV12 pictures are copied for the pool (pic_new)
static void cv_open(cv_dec *d) {   // (re)creates the codec state: a new stream starts at the next key frame
    if (!d->av1) d->av1 = mav1_new(&d->vt); else mav1_reset(d->av1);
}
static void *cv_worker(void *arg);
static Result cv_dec_init(void *t, uint16_t w, uint16_t h, uint32_t in, uint32_t out) {
    cv_dec *d = calloc(1, sizeof *d);
    d->magic = CV_MAGIC; d->w = w; d->h = h; pthread_mutex_init(&d->mu, NULL); pthread_cond_init(&d->work, NULL);
    cv_open(d);
    dm_host_mutex_lock(&g_cv_mu);
    for (int i = 0; i < 16; i++) if (!g_cv[i]) { g_cv[i] = d; d->id = (uint32_t)i + 1; break; }
    pthread_mutex_unlock(&g_cv_mu);
    if (!d->id) die("nn::codec: more than 16 video decoders");
    d->event = (uint8_t *)t + CV_EVENT_OFF; hle_bind_system_event(d->event, 0); hle_signal_system_event(d->event);
    pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
    if (pthread_create(&d->th, &a, cv_worker, d)) die("nn::codec: cannot start a video decoder's thread");
    pthread_attr_destroy(&a);
    *(cv_dec **)t = d;
    LOG("[codec] AV1 decoder %u: %ux%u, input format %u, output format %u (%s on the host)", d->id, w, h, in, out, mav1_backend(d->av1));
    return 0;
}
static Result cv_dec_fin(void *t) {
    cv_dec *d = cv_of(t); if (!d) return 0;
    dm_host_mutex_lock(&g_cv_mu); g_cv[d->id - 1] = NULL; dm_host_mutex_lock(&d->mu); d->quit = 1; pthread_cond_signal(&d->work); pthread_mutex_unlock(&d->mu); pthread_mutex_unlock(&g_cv_mu);   // no sync handle lookup is still inside
    movie_vt_cancel(&d->vt);
    dm_host_thread_join(d->th, NULL);   // a packet it decodes ends first (a codec cannot be stopped inside one); its pictures are dropped
    dm_movie_stopped(d);
    cv_stat_sum(d);
    if (d->all.n) cv_stat_line(d, &d->all, "");
    if (d->av1) mav1_free(d->av1);
    for (int i = 0; i < d->nheld; i++) free(d->held[i]);
    for (uint32_t k = 0; k < d->qcount; k++) free(d->q[(d->qhead + k) % CV_HOLD].data);
    d->magic = 0;
    // The submission-ready event goes with the decoder, as in the SDK. Kept, it stayed in the host's bound-event table, so the next decoder initialised in the same memory found its
    // event "bound" and was not given one: the game had cleared that memory, and nn::os aborted on the uninitialised event (found on Donkey Kong Bananza's second movie)
    static void (*destroy)(void *); if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::DestroySystemEvent missing");
    destroy(d->event);
    pthread_cond_destroy(&d->work); pthread_mutex_destroy(&d->mu); free(d); *(cv_dec **)t = NULL;
    return 0;
}
static Result cv_dec_reg(void *t, uint32_t i, void *pool, uint32_t pidx) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) { LOG("[RED] nn::codec::RegisterAv1DecoderBuffer index %u", i); return CV_R(2004); }
    dm_host_mutex_lock(&d->mu);
    d->buf[i].pool = pool_of(pool); d->buf[i].pidx = pidx; d->buf[i].reg = 1; d->buf[i].filled = 0; d->buf[i].gen = 0; d->buf[i].filled_gen = 0;
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] nn::codec: acquirable queue full at register %u", i);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
// Stop / Flush: packets not yet decoded, pictures not yet given to a buffer and a pending end of stream are dropped, and the decoder starts over at the next key frame. The codec
// state is the decoder's thread's: it makes the new one before its next packet (reopen), and drops the pictures of a packet it was decoding (epoch)
static Result cv_dec_reset(void *t, int stop) {
    cv_dec *d = cv_of(t); if (!d) return 0;
    dm_host_mutex_lock(&d->mu);
    if (stop) dm_movie_stopped(d);   // its movie ends now (under the lock fill holds, so no picture of it renews it); a Flush (a seek, a loop) keeps the movie playing
    for (uint32_t k = 0; k < d->qcount; k++) free(d->q[(d->qhead + k) % CV_HOLD].data);
    d->qhead = d->qcount = 0;
    for (int i = 0; i < d->nheld; i++) free(d->held[i]);
    d->nheld = 0; d->eos_pending = 0;
    d->epoch++; d->reopen = 1;
    movie_vt_cancel(&d->vt);
    cv_stat_sum(d);
    pthread_cond_signal(&d->work);   // it may have waited for room
    if (d->event) hle_signal_system_event(d->event);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
// A started decoder that has room takes packets, whatever became of the signal Initialize gave: a player may clear that one and drop it while it is not yet started, then count
// only later signals (Mario Tennis Fever's: with none at Start it never sent a packet, and every movie stayed black)
static Result cv_dec_start(void *t) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    dm_host_mutex_lock(&d->mu);
    if (d->event && cv_pending(d) < CV_READY) hle_signal_system_event(d->event);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
// A packet's pictures on their way from the codec to the pictures waiting (held). The packet's last one is kept back until the packet ends, as it carries an end of stream
typedef struct { cv_dec *d; uint32_t epoch; cv_pic *last; int64_t last_ts; int stop; uint64_t waited; } cv_out;   // stop: a Stop / Flush or Finalize came; waited: time spent waiting for room
// Room for one more waiting picture (decoder lock held, on the decoder's thread): a release makes some (fill). 0 when a Stop / Flush or Finalize came instead
static int cv_room(cv_dec *d, cv_out *o) {
    if (d->nheld >= CV_HOLD && !d->quit && o->epoch == d->epoch) {
        uint64_t t0 = now_ns();
        while (d->nheld >= CV_HOLD && !d->quit && o->epoch == d->epoch) pthread_cond_wait(&d->work, &d->mu);
        o->waited += now_ns() - t0;
    }
    return !d->quit && o->epoch == d->epoch;
}
static void cv_hold(cv_dec *d, cv_pic *c, int64_t ts, int eos) {   // decoder lock held, with room
    d->held[d->nheld] = c; d->held_ts[d->nheld] = ts; d->held_eos[d->nheld] = (uint8_t)eos; d->nheld++;
}
// A picture of the packet being decoded (the decoder's thread, outside the lock). The one before it, now known not to be the packet's last, joins the waiting pictures; the
// submission-ready event is left to the packet's end, which counts the packet out and its pictures in at once (a client counts its signals)
static void cv_put(cv_out *o, cv_pic *c, int64_t ts) {
    if (!c) return;
    if (o->last) {
        cv_dec *d = o->d;
        pthread_mutex_lock(&d->mu);
        if (cv_room(d, o)) { cv_hold(d, o->last, o->last_ts, 0); fill(d, 1); } else { free(o->last); o->stop = 1; }
        pthread_mutex_unlock(&d->mu);
    }
    o->last = c; o->last_ts = ts;
}
static int cv_idle(cv_dec *d) { pthread_mutex_lock(&d->mu); int idle = !d->qcount; pthread_mutex_unlock(&d->mu); return idle; }   // no packet waits behind the one being decoded
// One packet through the codec, on the decoder's thread and outside the decoder's lock (the codec state is that thread's from Initialize to Finalize)
static void cv_decode(cv_dec *d, const uint8_t *data, uint64_t size, int64_t ts, int eos, cv_out *o) {
    // dav1d decodes several frames at once, so a picture comes out some packets after its own went in. With a packet waiting behind this one that is what makes the decoder
    // fast, and it goes on to that packet. With none (a client that sends a packet and waits for its picture, or one the decoder is ahead of) the pictures still inside are
    // asked for, each waited for, as at the end of the stream; begun, that goes to its end whatever arrives meanwhile (mav1_next)
    mav1_picture pic;
    int r = size ? mav1_decode(d->av1, data, (size_t)size, ts, &pic) : 0;
    while (r == 1 && !o->stop) { cv_put(o, pic_new(&pic), pic.ts); r = mav1_next(d->av1, 0, &pic); }
    if (!o->stop && (eos || cv_idle(d))) while (mav1_next(d->av1, 1, &pic) == 1) if (!o->stop) cv_put(o, pic_new(&pic), pic.ts);
}
// The decoder's thread: the packets in the order they were taken, one at a time; a packet's pictures wait for released buffers (end of stream marks the last one, or comes as a
// buffer of its own). It starts a packet, and adds a picture to those waiting, only with room for one (a release makes some, fill): no picture is dropped for want of it
void dm_host_helper_qos(qos_class_t q);   // hle_thread.c
static void *cv_worker(void *arg) {
    cv_dec *d = arg;
    pthread_setname_np("dm codec video");
    dm_host_helper_qos(QOS_CLASS_USER_INITIATED);   // under DM_QOS_MAP: decodes ahead of playback, not on the frame's critical path
    pthread_mutex_lock(&d->mu);
    for (;;) {
        while (!d->quit && (!d->qcount || d->nheld >= CV_HOLD)) pthread_cond_wait(&d->work, &d->mu);
        if (d->quit) break;
        typeof(d->q[0]) p = d->q[d->qhead]; d->qhead = (d->qhead + 1) % CV_HOLD; d->qcount--;
        cv_out o = {.d = d, .epoch = d->epoch};
        int reopen = d->reopen; d->reopen = 0; d->busy = 1;
        if (reopen) movie_vt_continue(&d->vt);
        pthread_mutex_unlock(&d->mu);
        if (reopen) cv_open(d);
        uint64_t t0 = now_ns();
        cv_decode(d, p.data, p.size, p.ts, p.eos, &o);
        uint64_t dt = now_ns() - t0 - o.waited;
        free(p.data);
        pthread_mutex_lock(&d->mu);
        if (o.last) cv_room(d, &o);   // before the packet is counted out: the client finds the decoder full until then
        int before = cv_pending(d);
        d->busy = 0;
        if (d->quit || o.epoch != d->epoch) { free(o.last); if (d->quit) break; }   // Finalize, or a Stop / Flush since the packet was taken: its pictures are dropped
        else {
            if (o.last) cv_hold(d, o.last, o.last_ts, p.eos); else if (p.eos) d->eos_pending = 1;
            fill(d, 1);
        }
        ready(d, before, 0);
        cv_stat(d, dt);
    }
    pthread_mutex_unlock(&d->mu);
    return NULL;
}
// One packet: copied into the decoder's queue for its thread, and the call returns, as the SDK's does.
// A refusal (CV_BUSY, nn::codec 3603) is the decoder's defined answer, not a fault: NintendoSDK_movie's detail::SubmitDecoderInputBuffer turns exactly that result into its
// own retry code 0x7a, which VideoDecoder::DecodeAsync hands back to the player, and aborts on any other failure (subsdk0 +0x10fd20, +0x103840). So it is
// logged plainly
static Result cv_dec_submit(uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    dm_host_mutex_lock(&d->mu);
    uint8_t *copy = NULL;
    if (cv_pending(d) >= CV_HOLD || (size && !(copy = malloc((size_t)size)))) {
        static int n; if (n++ < 4) LOG("nn::codec AV1: busy (result 3603), a packet refused with %d pictures waiting for a released buffer and %u packets to decode", d->nheld, d->qcount + (uint32_t)d->busy);
        if (d->event) hle_clear_system_event(d->event);
        pthread_mutex_unlock(&d->mu); return CV_BUSY;
    }
    if (size) memcpy(copy, data, (size_t)size);
    typeof(d->q[0]) *p = &d->q[(d->qhead + d->qcount) % CV_HOLD]; d->qcount++;
    p->data = copy; p->size = size; p->ts = ts; p->eos = eos;
    *out = ++d->submits;
    ready(d, 0, 1);
    pthread_cond_signal(&d->work);
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static Result cv_dec_acq(uint32_t *idx, cv_sync *sync, void *t) {
    cv_dec *d = cv_of(t); if (!d) return CV_R(2004);
    dm_host_mutex_lock(&d->mu);
    if (!d->fcount) { pthread_mutex_unlock(&d->mu); return CV_NONE; }
    uint32_t i = d->fifo[d->fhead]; d->fhead = (d->fhead + 1) % 8; d->fcount--;
    *idx = i; *sync = (cv_sync){CV_SYNC, d->id, i, d->buf[i].gen};
    pthread_mutex_unlock(&d->mu);
    return 0;
}
static void cv_dec_rel(void *t, uint32_t i, cv_sync sync) {
    (void)sync; cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS || !d->buf[i].reg) { LOG("[RED] nn::codec::ReleaseAv1DecoderOutputBuffer index %u", i); return; }
    dm_host_mutex_lock(&d->mu);
    d->buf[i].gen++; d->buf[i].filled = 0;
    if (d->pcount < CV_BUFS) { d->pend[(d->phead + d->pcount) % CV_BUFS] = i; d->pcount++; }
    if (d->fcount < 8) { d->fifo[(d->fhead + d->fcount) % 8] = i; d->fcount++; } else LOG("[RED] nn::codec: acquirable queue full at release %u", i);
    int before = cv_pending(d);
    fill(d, 0); ready(d, before, 0);
    pthread_mutex_unlock(&d->mu);
}
static Result cv_dec_props(uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) {
    cv_dec *d = cv_of(t); if (!d || i >= CV_BUFS) return CV_R(2004);
    dm_host_mutex_lock(&d->mu);
    int ok = d->buf[i].filled;
    if (ok) { *w = *cw = d->buf[i].w; *h = *ch = d->buf[i].h; *ts = d->buf[i].ts; *eos = d->buf[i].eos != 0; }
    pthread_mutex_unlock(&d->mu);
    return ok ? 0 : CV_NONE;
}
// The decoder type's entry points, by their names in this game's SDK (22.2.8). Past the eight registers the guest's AAPCS64 gives each argument an 8-byte slot where Apple's
// arm64 packs them, so those are declared 64 bits wide; the 16-bit size arguments are masked, as the guest does not extend them
HLE_FORCE("_ZN2nn5codec36InitializeAv1DecoderByTransferMemoryEPNS0_14Av1DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
          "decoded on the host", Result, h_cv_av1tm, void *t, void *work, uint64_t wsize, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint64_t out, uint64_t mode) {
    (void)work; (void)wsize; (void)bs; (void)refs; (void)mode; return cv_dec_init(t, (uint16_t)w, (uint16_t)h, in, (uint32_t)out);
}
HLE_FORCE("_ZN2nn5codec34InitializeAv1DecoderByRemoteMemoryEPNS0_14Av1DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
          "decoded on the host", Result, h_cv_av1rm, void *t, uint64_t bs, uint64_t w, uint64_t h, uint32_t refs, uint32_t in, uint32_t out, uint32_t mode) {
    (void)bs; (void)refs; (void)mode; return cv_dec_init(t, (uint16_t)w, (uint16_t)h, in, out);
}
HLE_FORCE("_ZN2nn5codec33GetAv1DecoderSubmissionReadyEventEPNS0_14Av1DecoderTypeE", "signalled while the host decoder takes packets", void *, h_cv_av1event, void *t) { cv_dec *d = cv_of(t); return d ? d->event : NULL; }
HLE_FORCE("_ZN2nn5codec18FinalizeAv1DecoderEPNS0_14Av1DecoderTypeE", "frees the host decoder", Result, h_cv_av1fin, void *t) { return cv_dec_fin(t); }
HLE_FORCE("_ZN2nn5codec24RegisterAv1DecoderBufferEPNS0_14Av1DecoderTypeEjPNS0_25VideoDecoderImagePoolTypeEj", "a buffer of the host decoder, acquirable at once",
          Result, h_cv_av1reg, void *t, uint32_t i, void *pool, uint32_t pidx) { return cv_dec_reg(t, i, pool, pidx); }
HLE_FORCE("_ZN2nn5codec15StartAv1DecoderEPNS0_14Av1DecoderTypeE", "the host decoder takes packets from here on", Result, h_cv_av1start, void *t) { return cv_dec_start(t); }
HLE_FORCE("_ZN2nn5codec14StopAv1DecoderEPNS0_14Av1DecoderTypeE", "drops undecoded packets and undelivered pictures", Result, h_cv_av1stop, void *t) { return cv_dec_reset(t, 1); }
HLE_FORCE("_ZN2nn5codec15FlushAv1DecoderEPNS0_14Av1DecoderTypeE", "drops undecoded packets and undelivered pictures", Result, h_cv_av1flush, void *t) { return cv_dec_reset(t, 0); }
HLE_FORCE("_ZN2nn5codec27SubmitAv1DecoderInputBufferEPmPNS0_14Av1DecoderTypeEPKvmNS_8TimeSpanEb", "queues the packet for the host decoder's thread",
          Result, h_cv_av1submit, uint64_t *out, void *t, const void *data, uint64_t size, int64_t ts, bool eos) { return cv_dec_submit(out, t, data, size, ts, eos); }
HLE_FORCE("_ZN2nn5codec29AcquireAv1DecoderOutputBufferEPjPNS_2gs16NativeSyncHandleEPNS0_14Av1DecoderTypeE", "the oldest acquirable buffer and its sync handle",
          Result, h_cv_av1acq, uint32_t *idx, cv_sync *sync, void *t) { return cv_dec_acq(idx, sync, t); }
HLE_FORCE("_ZN2nn5codec29ReleaseAv1DecoderOutputBufferEPNS0_14Av1DecoderTypeEjNS_2gs16NativeSyncHandleE", "the buffer waits for the next picture and is acquirable again",
          void, h_cv_av1rel, void *t, uint32_t i, cv_sync sync) { cv_dec_rel(t, i, sync); }
HLE_FORCE("_ZN2nn5codec35GetAv1DecoderOutputBufferPropertiesEPtS1_S1_S1_PNS_8TimeSpanEPbPNS0_14Av1DecoderTypeEj", "the filled buffer's picture size, time and end of stream",
          Result, h_cv_av1props, uint16_t *w, uint16_t *h, uint16_t *cw, uint16_t *ch, int64_t *ts, bool *eos, void *t, uint32_t i) { return cv_dec_props(w, h, cw, ch, ts, eos, t, i); }
// The movie library also links the VP8, VP9 and H.264 decoder types. The game's 26 movies are all AV1 (the codec ID of each WebM's video track), so none of those is ever made.
// One that were (a movie of a later update) is refused here with its reason: the SDK's own initializer would send the request to the decoder service the host does not serve
static Result cv_other(const char *codec) { LOG("[RED] nn::codec: a %s decoder was asked for: this host decodes AV1 only (hle_codec_video.c)", codec); return CV_R(2004); }
#define CV_OTHER(tag, name, s_tm, s_rm) \
    HLE_FORCE(s_tm, "refused: only AV1 is decoded", Result, h_cv_##tag##tm, void) { return cv_other(name); } \
    HLE_FORCE(s_rm, "refused: only AV1 is decoded", Result, h_cv_##tag##rm, void) { return cv_other(name); }
CV_OTHER(vp9, "VP9", "_ZN2nn5codec36InitializeVp9DecoderByTransferMemoryEPNS0_14Vp9DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeVp9DecoderByRemoteMemoryEPNS0_14Vp9DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE")
CV_OTHER(vp8, "VP8", "_ZN2nn5codec36InitializeVp8DecoderByTransferMemoryEPNS0_14Vp8DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE",
    "_ZN2nn5codec34InitializeVp8DecoderByRemoteMemoryEPNS0_14Vp8DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeE")
CV_OTHER(h264, "H.264", "_ZN2nn5codec37InitializeH264DecoderByTransferMemoryEPNS0_15H264DecoderTypeEPvmmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm",
    "_ZN2nn5codec35InitializeH264DecoderByRemoteMemoryEPNS0_15H264DecoderTypeEmttjNS0_23VideoDecoderInputFormatENS0_24VideoDecoderOutputFormatENS0_22VideoDecoderOutputModeEPKvm")
#undef CV_OTHER
// The host's sync handles signal when their buffer holds the picture of that release; a handle the host did not make (another nn::gs user) reads as signalled
static bool sync_reached(const cv_sync *s) {
    if (s->magic != CV_SYNC) { static int once; if (!once++ && (s->magic || s->dec)) LOG("[RED] nn::codec: a sync handle the host did not make (%#x): signalled", s->magic); return true; }
    if (!s->gen) return true;
    dm_host_mutex_lock(&g_cv_mu);   // held until the decoder's lock is: FinalizeAv1Decoder cannot free it in between
    cv_dec *d = s->dec && s->dec <= 16 ? g_cv[s->dec - 1] : NULL;
    bool r = true;
    if (d && s->idx < CV_BUFS) { dm_host_mutex_lock(&d->mu); r = d->buf[s->idx].filled && d->buf[s->idx].filled_gen >= s->gen; pthread_mutex_unlock(&d->mu); }
    pthread_mutex_unlock(&g_cv_mu);
    return r;
}
HLE_FORCE("_ZN2nn2gs17TryWaitNativeSyncERKNS0_16NativeSyncHandleE", "decoder sync handles are the host's (hle_codec_video.c)", bool, h_gs_trywait, const cv_sync *s) {
    return sync_reached(s);
}
HLE_FORCE("_ZN2nn6syncpt3sdk20CreateSyncPointEventEPNS1_18SyncPointEventTypeE", "a host-bound SystemEvent inside the event type", Result, h_spt_create, void *e) {
    memset(e, 0, 0x58); hle_bind_system_event(e, 0); SPT(e)->magic = CV_SYNC;
    return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk25InitializeMultiWaitHolderEPNS_2os19MultiWaitHolderTypeEPNS1_18SyncPointEventTypeE", "links the event's SystemEvent", void, h_spt_holder, void *holder, void *e) {
    static void (*init)(void *, void *); if (!init && !(init = guest_sym("_ZN2nn2os25InitializeMultiWaitHolderEPNS0_19MultiWaitHolderTypeEPNS0_15SystemEventTypeE"))) die("nn::os::InitializeMultiWaitHolder(SystemEventType) missing");
    init(holder, e);
}
HLE_FORCE("_ZN2nn6syncpt3sdk21RequestSyncPointEventEPNS1_18SyncPointEventTypeERKNS1_15SyncPointHandleE", "signalled when the decoder fills the awaited buffer",
          Result, h_spt_request, void *e, const cv_sync *s) {
    spt_unarm(e); hle_clear_system_event(e);
    SPT(e)->want = *s; SPT(e)->armed = 1;
    pthread_mutex_lock(&g_spt_mu); int k = 0; while (k < 64 && g_spt[k]) k++; if (k < 64) g_spt[k] = e; pthread_mutex_unlock(&g_spt_mu);
    if (k == 64) { LOG("[RED] nn::syncpt: more than 64 armed events: signalled at once"); hle_signal_system_event(e); return 0; }
    if (sync_reached(s)) { spt_unarm(e); hle_signal_system_event(e); }
    return 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk20CancelSyncPointEventEPNS1_18SyncPointEventTypeE", "disarms the event", void, h_spt_cancel, void *e) { spt_unarm(e); hle_clear_system_event(e); }
// Whether the awaited picture is in its buffer: the event's SystemEvent is signalled (manual clear, so the look leaves it signalled). The SDK's own went through the same null
// table as CreateSyncPointEvent (pc 0)
HLE_FORCE("_ZN2nn6syncpt3sdk21TryWaitSyncPointEventEPKNS1_18SyncPointEventTypeE", "the event's SystemEvent is signalled", bool, h_spt_trywait, const void *e) {
    static uint32_t (*tw)(void *); if (!tw && !(tw = guest_sym("_ZN2nn2os18TryWaitSystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::TryWaitSystemEvent missing");
    return (tw((void *)e) & 0xff) != 0;
}
HLE_FORCE("_ZN2nn6syncpt3sdk21DestroySyncPointEventEPNS1_18SyncPointEventTypeE", "destroys the event's SystemEvent", void, h_spt_destroy, void *e) {
    spt_unarm(e);
    static void (*destroy)(void *); if (!destroy && !(destroy = guest_sym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE"))) die("nn::os::DestroySystemEvent missing");
    destroy(e); SPT(e)->magic = 0;
}
