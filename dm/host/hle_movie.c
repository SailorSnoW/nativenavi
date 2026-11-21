// The movie signal of the frame step and the VideoToolbox decoders' cancellation. The game's movies are AV1 WebM files (Event/Movie, 26 of them) that its movie library (subsdk0,
// NintendoSDK_movie) demuxes itself and decodes through nn::codec's Av1Decoder, which the host serves (hle_codec_video.c, hle_movie_av1.c)
#include "hle.h"
#include <pthread.h>
#include "hle_movie_vt.h"

// The movie signal of the frame step (hle_nvn.c fps_step_cap and high_cap_tick). The players of the games it steps take one picture per game frame rather than by the picture's time,
// so the step must follow the movie's picture rate, whatever the cap: Ocarina of Time's title and opening movies (AV1, 60 a second, nn::codec in hle_codec_video.c) went at
// half speed at a 30 fps cap (PR #2622: 2 s of movie in 4 s). Each video decoder handing out pictures has a slot with its rate, from its pictures' times; the movie's rate is the fastest
// of the decoders that handed out a picture in the last MD_PLAYING_NS (a 30 fps movie gives one every 33 ms). Stopping or destroying a decoder ends its part at once. A decoder whose
// times give no rate does not count.
// Every MD_PACE_PICS pictures a decoder's line says how much of its stream it handed out in how much time: a movie at the wrong speed shows there
#define MD_PLAYING_NS 500000000ull
#define MD_SLOTS 8
#define MD_PACE_PICS 600
#define MD_RATE_PICS 8   // the rate is taken over the last 8 pictures: Ocarina of Time's times are whole milliseconds, 16 and 17 apart, 63 and 59 a picture alone
typedef struct { const void *dec; uint64_t ns, base_ns; int64_t ts, base_ts, win[MD_RATE_PICS]; int hz, logged_hz, nwin; uint32_t n, pace_n; } md_slot;
static md_slot g_md_slots[MD_SLOTS];
static pthread_mutex_t g_md_slots_mu = PTHREAD_MUTEX_INITIALIZER;
// Stop / Flush / Finalize cancel what a VideoToolbox decoder's thread is doing for a session it lost (hle_movie_vt.h): making a session again and replaying the stream into it
static pthread_mutex_t g_vt_mu = PTHREAD_MUTEX_INITIALIZER;
void movie_vt_cancel(movie_vt_control *control) {
    pthread_mutex_lock(&g_vt_mu); control->cancelled = 1; pthread_mutex_unlock(&g_vt_mu);
}
void movie_vt_continue(movie_vt_control *control) {
    pthread_mutex_lock(&g_vt_mu); control->cancelled = 0; pthread_mutex_unlock(&g_vt_mu);
}
int movie_vt_cancelled(movie_vt_control *control) {
    pthread_mutex_lock(&g_vt_mu); int cancelled = control && control->cancelled; pthread_mutex_unlock(&g_vt_mu); return cancelled;
}
static int md_slot_hz(const md_slot *s, uint64_t now) { return !s->dec || now - s->ns >= MD_PLAYING_NS ? 0 : s->hz; }
void dm_movie_picture(const void *dec, int64_t ts_ns) {
    uint64_t now = now_ns();
    pthread_mutex_lock(&g_md_slots_mu);
    md_slot *s = NULL, *spare = &g_md_slots[0];   // spare: a free slot, else one whose decoder handed out nothing for MD_PLAYING_NS, else the one idle longest
    for (int i = 0; i < MD_SLOTS && !s; i++) {
        md_slot *e = &g_md_slots[i];
        if (e->dec == dec) s = e;
        else if (spare->dec && (!e->dec || e->ns < spare->ns)) spare = e;
    }
    if (!s) { s = spare; *s = (md_slot){ .dec = dec }; }
    int64_t dt = s->n ? ts_ns - s->ts : 0;
    if (dt > 4000000 && dt < 200000000) {   // 250 to 5 pictures a second; a seek, a loop or a pause starts the window again and leaves the rate as it was
        if (!s->nwin) s->win[s->nwin++] = s->ts;
        if (s->nwin == MD_RATE_PICS) memmove(s->win, s->win + 1, sizeof s->win[0] * --s->nwin);
        s->win[s->nwin++] = ts_ns;
        int64_t span = ts_ns - s->win[0], gaps = s->nwin - 1; s->hz = (int)((gaps * 1000000000 + span / 2) / span);
    } else s->nwin = 0;
    if (!s->n || dt < 0 || dt > 1000000000) { s->base_ts = ts_ns; s->base_ns = now; s->pace_n = 0; }   // the pace line counts from here
    else if (++s->pace_n >= MD_PACE_PICS) {
        LOG("[movie] decoder %p: %.2f s of pictures in %.2f s (%d pictures a second)", dec, (double)(ts_ns - s->base_ts) / 1e9, (double)(now - s->base_ns) / 1e9, s->hz);
        s->base_ts = ts_ns; s->base_ns = now; s->pace_n = 0;
    }
    if (s->nwin == MD_RATE_PICS && abs(s->hz - s->logged_hz) > 1) { LOG("[movie] decoder %p: %d pictures a second", dec, s->hz); s->logged_hz = s->hz; }
    s->ts = ts_ns; s->ns = now; s->n++;
    pthread_mutex_unlock(&g_md_slots_mu);
}
void dm_movie_stopped(const void *dec) {
    pthread_mutex_lock(&g_md_slots_mu);
    for (int i = 0; i < MD_SLOTS; i++) if (g_md_slots[i].dec == dec) g_md_slots[i] = (md_slot){0};
    pthread_mutex_unlock(&g_md_slots_mu);
}
int dm_movie_hz(void) {
    uint64_t now = now_ns(); int hz = 0;
    pthread_mutex_lock(&g_md_slots_mu);
    for (int i = 0; i < MD_SLOTS; i++) { int h = md_slot_hz(&g_md_slots[i], now); if (h > hz) hz = h; }
    pthread_mutex_unlock(&g_md_slots_mu);
    return hz;
}
