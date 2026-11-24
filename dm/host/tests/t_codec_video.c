// The Switch 2's AV1 decoder API (hle_codec_video.c) without a guest, on a synthetic red 64x64 key frame (SVT-AV1 through PyAV). Submissions queue for the decoder's
// thread: settle() waits for it before a check that reads what a packet leads to. The VideoToolbox cases run only where the chip decodes AV1 (M3 on).
#include "../hle.h"
#include <stdatomic.h>
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include <VideoToolbox/VideoToolbox.h>
static OSStatus vt_test_decode(VTDecompressionSessionRef, CMSampleBufferRef, VTDecodeFrameFlags, void *, VTDecodeInfoFlags *);
static OSStatus vt_test_create(CFAllocatorRef, CMVideoFormatDescriptionRef, CFDictionaryRef, CFDictionaryRef, const VTDecompressionOutputCallbackRecord *, VTDecompressionSessionRef *);
#define VTDecompressionSessionDecodeFrame vt_test_decode
#define VTDecompressionSessionCreate vt_test_create
#include "../hle_movie.c"
#include "../hle_codec_video.c"
#include "../hle_movie_av1.c"
#undef VTDecompressionSessionDecodeFrame
#undef VTDecompressionSessionCreate
#include <assert.h>
#include <unistd.h>
#include "movie_vt_data.h"

static _Atomic int vt_callback_error, vt_hold_create, vt_held, vt_release;
static movie_vt_control *vt_hold_control;
static _Atomic int vt_double_loss, vt_initial_failed;
static _Thread_local OSStatus vt_create_status;
static mav1 *vt_callback_av1;
static OSStatus vt_test_decode(VTDecompressionSessionRef session, CMSampleBufferRef sample, VTDecodeFrameFlags flags, void *source, VTDecodeInfoFlags *info) {
    int second = atomic_load(&vt_double_loss);
    if (second && atomic_load(&vt_initial_failed) &&
        ((second == 1 && (flags & kVTDecodeFrame_DoNotOutputFrame)) || (second == 2 && !flags))) {
        atomic_store(&vt_double_loss, 0); VTDecompressionSessionInvalidate(session);
    }
    if (atomic_exchange(&vt_callback_error, 0)) {
        av1_on_frame(vt_callback_av1, source, kVTInvalidSessionErr, 0, NULL, kCMTimeInvalid, kCMTimeInvalid);
        return noErr;
    }
    OSStatus status = VTDecompressionSessionDecodeFrame(session, sample, flags, source, info);
    if (status == kVTInvalidSessionErr) atomic_store(&vt_initial_failed, 1);
    return status;
}
static OSStatus vt_test_create(CFAllocatorRef allocator, CMVideoFormatDescriptionRef format, CFDictionaryRef specification, CFDictionaryRef attributes,
                                const VTDecompressionOutputCallbackRecord *callback, VTDecompressionSessionRef *session) {
    if (atomic_exchange(&vt_hold_create, 0)) {   // held until the test lets it go on, or its decoder is cancelled
        atomic_store(&vt_held, 1);
        while (!atomic_load(&vt_release) && !movie_vt_cancelled(vt_hold_control)) usleep(200);
        if (!atomic_load(&vt_release)) { *session = NULL; return kVTVideoDecoderNotAvailableNowErr; }
    }
    vt_create_status = VTDecompressionSessionCreate(allocator, format, specification, attributes, callback, session);
    return vt_create_status;
}

uint64_t now_ns(void) { return 1; }
__thread _Atomic int dm_host_work;
void dm_host_helper_qos(qos_class_t q) { (void)q; }
static struct { void *ev; int bound, set; pthread_t by; } g_ev[16];
static pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
static int ev_slot_locked(void *e) { for (int i = 0; i < 16; i++) if (g_ev[i].ev == e) return i; for (int i = 0; i < 16; i++) if (!g_ev[i].ev) { g_ev[i].ev = e; return i; } abort(); }
static int ev_slot(void *e) { pthread_mutex_lock(&g_ev_mu); int i = ev_slot_locked(e); pthread_mutex_unlock(&g_ev_mu); return i; }
void hle_bind_system_event(void *e, int autoclear) { (void)autoclear; pthread_mutex_lock(&g_ev_mu); g_ev[ev_slot_locked(e)].bound = 1; pthread_mutex_unlock(&g_ev_mu); }
void hle_signal_system_event(void *e) { pthread_mutex_lock(&g_ev_mu); int i = ev_slot_locked(e); g_ev[i].set = 1; g_ev[i].by = pthread_self(); pthread_mutex_unlock(&g_ev_mu); }
void hle_clear_system_event(void *e) { pthread_mutex_lock(&g_ev_mu); g_ev[ev_slot_locked(e)].set = 0; pthread_mutex_unlock(&g_ev_mu); }
static int ev_set(void *e) { pthread_mutex_lock(&g_ev_mu); int r = g_ev[ev_slot_locked(e)].set; pthread_mutex_unlock(&g_ev_mu); return r; }
static int ev_by_me(void *e) { pthread_mutex_lock(&g_ev_mu); int r = pthread_equal(g_ev[ev_slot_locked(e)].by, pthread_self()); pthread_mutex_unlock(&g_ev_mu); return r; }
static void destroy_ev(void *e) { pthread_mutex_lock(&g_ev_mu); int i = ev_slot_locked(e); g_ev[i].bound = g_ev[i].set = 0; pthread_mutex_unlock(&g_ev_mu); }
static void settle(void *t) {
    cv_dec *d = cv_of(t);
    for (;;) { pthread_mutex_lock(&d->mu); int idle = !d->busy && (!d->qcount || d->nheld >= CV_HOLD); pthread_mutex_unlock(&d->mu); if (idle) return; usleep(200); }
}
void *guest_sym(const char *n) { return !strcmp(n, "_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE") ? (void *)destroy_ev : NULL; }
static cv_pic *black_pic(unsigned w, unsigned h) {
    cv_pic *c = pic_alloc(w, h); if (!c) return NULL;
    memset(c->px, 16, (size_t)w * h); memset(c->px + (size_t)w * h, 128, (size_t)pic_uvw(c) * ((h + 1) / 2));
    return c;
}

static const uint8_t av1[] = {   // one temporal unit: temporal delimiter, sequence header (profile 0, 64x64), frame
    0x12,0x00,0x0a,0x0b,0x02,0x00,0x00,0x04,0x95,0x7f,0xfc,0x0a,0xf9,0x00,0x40,0x32,0x15,0x10,0x00,0xbc,0x00,0x49,0x22,0x08,0x08,0x21,0x10,0x00,0xad,0x3b,0x67,0x00,
    0x1e,0xb5,0xdb,0x79,0x52,0x90,
};
static bool vt_av1_available(void) {
    if (!hw_av1()) return false;
    // A virtual Mac can advertise hardware AV1 but have no decoder for a real session.
    mav1 a = {0};
    size_t n = 0;
    const uint8_t *seq = find_seq(av1, sizeof av1, &n);
    assert(seq);
    vt_create_status = noErr;
    int opened = av1_open(&a, seq, n);
    av1_close(&a);
    if (opened == 1) return true;
    assert(vt_create_status == kVTCouldNotFindVideoDecoderErr);
    return false;
}

static void vt_session_lifecycle(void) {
    mav1 *a = mav1_new(NULL); mav1_picture pic; int av1_pictures = 0;
    for (int i = 0; i < 3; i++) {
        if (i == 1) { assert(a->vs && !a->soft); VTDecompressionSessionInvalidate(a->vs); }
        if (mav1_decode(a, av1, sizeof av1, i * 1000, &pic) == 1) av1_pictures++;
    }
    mav1_free(a);
    printf("invalidated VideoToolbox sessions: AV1 %d/3 pictures\n", av1_pictures);
    assert(av1_pictures == 3);
    fflush(stdout);
}
static uint64_t vt_hash(const uint8_t *p, size_t stride, int width, int height) {
    uint64_t hash = 0;
    for (int y = 0; y < height; y++) for (int x = 0; x < width; x++) hash = hash * 31 + p[(size_t)y * stride + x];
    return hash;
}
static void vt_reference_recovery(void) {
    uint64_t expected_av1[36];
    for (int loss = -1; loss < 14; loss++) {
        int got;
        mav1 *a = mav1_new(NULL); mav1_picture pic; got = 0;
        for (int loop = 0; loop < 3; loop++) for (int i = 0; i < 12; i++) {
            if ((loss < 12 ? loss : 7) == i && a->vs) {
                atomic_store(&vt_initial_failed, 0); atomic_store(&vt_double_loss, loss < 12 ? 0 : loss - 11);
                VTDecompressionSessionInvalidate(a->vs);
            }
            assert(mav1_decode(a, vt_av1_data + vt_av1_packets[i].offset, vt_av1_packets[i].size, i * 33333, &pic) == 1);
            assert(pic.ts == i * 33333);
            uint64_t hash = vt_hash(pic.y, pic.ystride, pic.w, pic.h) ^ vt_hash(pic.uv, pic.uvstride, pic.w, pic.h / 2);
            if (loss < 0) expected_av1[got] = hash; else assert(expected_av1[got] == hash);
            got++;
        }
        assert(got == 36 && !a->soft); mav1_reset(a); assert(!a->history.count); mav1_free(a);
    }
    puts("VideoToolbox reference recovery: all 36 original pictures and timestamps across three loops, loss at each packet and again during replay/resubmission");
}
static void vt_av1_random_access(void) {
    mav1 a = {0}; size_t n = 0; const uint8_t *seq = find_seq(av1, sizeof av1, &n);
    assert(seq && n <= sizeof a.seq); memcpy(a.seq, seq, n); a.nseq = n;
    const uint8_t shown[] = {0x32, 1, 0x10}, hidden[] = {0x32, 1, 0}, inter[] = {0x32, 1, 0x30}, existing[] = {0x32, 1, 0x80};
    assert(av1_key(&a, shown, sizeof shown));
    assert(!av1_key(&a, hidden, sizeof hidden));
    assert(!av1_key(&a, inter, sizeof inter) && !av1_key(&a, existing, sizeof existing));
    assert(!av1_key(&a, seq, n));
    a.seq[2] |= 8;
    assert(av1_key(&a, hidden, sizeof hidden));
}
// a session lost mid-packet is made again on the decoder's thread and the stream replayed; Flush, Stop or Finalize meanwhile must not deadlock
static void vt_worker_lifecycle(void) {
    for (int action = 0; action < 4; action++) {
        _Alignas(16) uint8_t dec[0x400] = {0}; uint64_t id;
        const uint8_t *packet = av1; size_t size = sizeof av1;
        assert(!cv_dec_init(dec, 64, 64, 1, 3));
        cv_dec *d = cv_of(dec); vt_callback_av1 = d->av1;
        assert(!cv_dec_submit(&id, dec, packet, size, 1000, false)); settle(dec);
        assert(d->nheld == 1 && d->held_ts[0] == 1000);
        assert(d->av1->vs && !d->av1->soft);
        atomic_store(&vt_callback_error, 1);
        vt_hold_control = &d->vt; atomic_store(&vt_held, 0); atomic_store(&vt_release, 0); atomic_store(&vt_hold_create, 1);
        assert(!cv_dec_submit(&id, dec, packet, size, 2000, false));
        alarm(5);
        while (!atomic_load(&vt_held)) usleep(200);
        pthread_mutex_lock(&d->mu); assert(d->busy && d->nheld == 1 && !d->eos_pending); pthread_mutex_unlock(&d->mu);
        if (action == 3) assert(!cv_dec_fin(dec));
        else {
            if (action) {
                assert(!cv_dec_reset(dec, action == 2)); settle(dec);
                assert(!d->nheld && !cv_dec_submit(&id, dec, packet, size, 3000, true));
            }
            atomic_store(&vt_release, 1); settle(dec);
            assert(d->nheld == (action ? 1 : 2));
            assert(d->held_ts[action ? 0 : 1] == (action ? 3000 : 2000));
            assert(!cv_dec_fin(dec));
        }
        vt_hold_control = NULL; alarm(0);
        vt_callback_av1 = NULL;
    }
    puts("VideoToolbox workers: callback invalidation, the session made again, and Flush, Stop and Finalize while it is being made");
}
int main(void) {
    bool test_av1 = vt_av1_available();
    if (!test_av1) puts("VideoToolbox lifecycle: hardware AV1 unavailable; the VideoToolbox cases are skipped");
    else { vt_session_lifecycle(); vt_reference_recovery(); vt_worker_lifecycle(); }
    vt_av1_random_access();
    _Alignas(16) static uint8_t dec[0x400], pool[3][0x40], spt[0x80], mem[64 * 64 * 4];
    uint64_t size, align, id; uint32_t idx; cv_sync sync; uint16_t w, h, cw, ch; int64_t ts; bool eos;

    // NV12 is format 3; the 16-bit sizes come unextended
    cv_pref pref = h_cv_pref(0xffff0040, 0xffff0040, 3, 0); assert(pref.w == 64 && pref.h == 64 && pref.fmt == 3 && pref.magic == CV_MAGIC);
    assert(!h_cv_poolsize(&size, &align, &pref, 1) && size >= 64 * 64 * 4 && align == CV_NATIVE_ALIGN);
    for (int i = 0; i < 3; i++) assert(!h_cv_pool(pool[i], mem, size, &pref, 1) && pool_of(pool[i]));

    assert(!h_cv_av1rm(dec, 0x10000, 0xabcd0040, 0xabcd0040, 1, 1, 3, 2));
    cv_dec *d = cv_of(dec); assert(d && d->av1 && d->w == 64 && d->h == 64);
    void *ev = h_cv_av1event(dec); assert(ev == dec + CV_EVENT_OFF && g_ev[ev_slot(ev)].bound && ev_set(ev));

    // an empty queue answers 3604
    for (uint32_t i = 0; i < 2; i++) assert(!h_cv_av1reg(dec, i, pool[i], 0));
    cv_sync sync0, sync1;
    assert(!h_cv_av1acq(&idx, &sync0, dec) && idx == 0 && sync0.magic == CV_SYNC && sync0.gen == 1 && !h_gs_trywait(&sync0));
    assert(!h_cv_av1acq(&idx, &sync1, dec) && idx == 1 && sync1.magic == CV_SYNC && sync1.gen == 1 && !h_gs_trywait(&sync1));
    assert(h_cv_av1acq(&idx, &sync, dec) == CV_NONE);
    assert(h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);

    assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 1000, false) && id == 1);
    settle(dec); assert(h_gs_trywait(&sync0) && !h_gs_trywait(&sync1));
    assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) && w == 64 && h == 64 && cw == 64 && ch == 64 && ts == 1000 && !eos);
    static uint8_t nv12[64 * 64 * 3 / 2], rgba[64 * 64 * 4];   // Y 81, Cb 90, Cr 240: red (BT.601, video range)
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 64, 64, nv12, 64, 4096, nv12 + 4096, 64, 2048));
    for (int i = 0; i < 4096; i++) assert(abs(nv12[i] - 81) <= 4);
    for (int i = 4096; i < 6144; i += 2) { assert(abs(nv12[i] - 90) <= 6); assert(abs(nv12[i + 1] - 240) <= 6); }
    static uint8_t y[4096], uv[2048];
    assert(!h_cv_nv12(pool[0], 0, 0, 0, 64, 64, y, 64, 4096, uv, 64, 2048) && !memcmp(y, nv12, 4096) && !memcmp(uv, nv12 + 4096, 2048));
    {
        static uint8_t small[32 * 32 * 3 / 2], big[96 * 80 * 3 / 2];
        assert(!h_cv_nv12(pool[0], 0, 0, 0, 32, 32, small, 32, 1024, small + 1024, 32, 512));
        for (int r = 0; r < 32; r++) assert(!memcmp(small + r * 32, nv12 + r * 64, 32));
        for (int r = 0; r < 16; r++) assert(!memcmp(small + 1024 + r * 32, nv12 + 4096 + r * 64, 32));
        assert(!h_cv_nv12(pool[0], 0, 0, 0, 96, 80, big, 96, 7680, big + 7680, 96, 3840));
        for (int r = 0; r < 80; r++) for (int c = 0; c < 96; c++) assert(big[r * 96 + c] == (r < 64 && c < 64 ? nv12[r * 64 + c] : 16));
        for (int r = 0; r < 40; r++) for (int c = 0; c < 96; c++) assert(big[7680 + r * 96 + c] == (r < 32 && c < 64 ? nv12[4096 + r * 64 + c] : 128));
    }
    assert(!h_cv_rgba(pool[0], 0, 0, 0, 64, 64, rgba, 256, sizeof rgba));
    for (int i = 0; i < 64 * 64 * 4; i += 4) assert(rgba[i] >= 235 && rgba[i + 1] <= 20 && rgba[i + 2] <= 20 && rgba[i + 3] == 255);
    assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 1001, false) && id == 2);
    settle(dec); assert(h_gs_trywait(&sync1) && !d->pcount);
    h_cv_av1rel(dec, 0, sync0);
    assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 0 && sync.gen == 2 && !h_gs_trywait(&sync));
    assert(h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) == CV_NONE);
    assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 1002, false) && id == 3);
    settle(dec); assert(h_gs_trywait(&sync) && !d->pcount);
    assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 0) && ts == 1002);

    // no released buffer: the event goes down at CV_READY, packets are refused (3603) at CV_HOLD, a pending packet counting as its picture
    for (int i = 0; i < CV_READY; i++) assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 2000 + i, false));
    assert(!ev_set(ev));
    settle(dec); assert(d->nheld == CV_READY && !d->qcount && !ev_set(ev));
    for (int i = CV_READY; i < CV_HOLD; i++) assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 2000 + i, false));
    assert(h_cv_av1submit(&id, dec, av1, sizeof av1, 9999, false) == CV_BUSY && !ev_set(ev));
    settle(dec); assert(d->nheld == CV_HOLD && !d->qcount && !ev_set(ev));
    assert(h_cv_av1submit(&id, dec, av1, sizeof av1, 9999, false) == CV_BUSY);
    for (int i = CV_HOLD; i > CV_READY; i--) { h_cv_av1rel(dec, 1, sync); assert(d->nheld == i - 1 && !ev_set(ev)); assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 1); }
    h_cv_av1rel(dec, 1, sync);
    assert(d->nheld == CV_READY - 1 && ev_set(ev));
    assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 1 && h_gs_trywait(&sync));
    hle_clear_system_event(ev); assert(!ev_set(ev));
    assert(!h_cv_av1start(dec) && ev_set(ev));
    // a client counts each signal as one packet it may send: a release with no packet taken signals nothing
    hle_clear_system_event(ev);
    h_cv_av1rel(dec, 1, sync); assert(d->nheld == CV_READY - 2 && !ev_set(ev));
    assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 1);
    assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 2000 + CV_HOLD, false) && ev_set(ev) && ev_by_me(ev));
    settle(dec); assert(d->nheld == CV_READY - 1 && ev_set(ev));
    assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 2000 + CV_HOLD - CV_READY + 1);

    assert(!h_spt_create(spt) && g_ev[ev_slot(spt)].bound);
    h_cv_av1rel(dec, 0, sync);
    assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 0);
    assert(!h_spt_request(spt, &sync) && ev_set(spt) && !SPT(spt)->armed);
    assert(!h_cv_av1reg(dec, 2, pool[2], 0) && !h_cv_av1acq(&idx, &sync, dec) && idx == 2 && sync.gen == 1 && h_gs_trywait(&sync));
    h_cv_av1rel(dec, 0, sync); assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 0 && !d->nheld);
    h_cv_av1rel(dec, 2, sync); assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 2 && sync.gen == 2 && !h_gs_trywait(&sync));
    assert(!h_spt_request(spt, &sync) && !ev_set(spt) && SPT(spt)->armed);
    assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 3000, false));
    settle(dec); assert(ev_set(spt) && !SPT(spt)->armed && h_gs_trywait(&sync));
    assert(!ev_by_me(spt));

    assert(!h_cv_av1submit(&id, dec, NULL, 0, 0, true));
    settle(dec); assert(d->eos_pending);
    h_cv_av1rel(dec, 2, sync); assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 2 && h_gs_trywait(&sync));
    assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 2) && eos && w == 0 && h == 0);

    h_cv_poolfin(pool[1]); assert(!pool_of(pool[1]) && d->buf[1].pool == NULL);
    h_cv_av1rel(dec, 1, sync); assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 1 && !h_gs_trywait(&sync));
    assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 4000, false));
    settle(dec); assert(h_gs_trywait(&sync));
    assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 4000 && w == 64);
    // a flush drops waiting pictures and undecoded packets wherever the decoder's thread is with them
    for (int round = 0; round < 8; round++) {
        for (int i = 0; i < 6; i++) assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 5000 + i, false));
        if (round & 1) usleep(100u * (unsigned)round);
        assert(!h_cv_av1flush(dec) && d->nheld == 0 && !d->qcount && ev_set(ev));
        settle(dec); assert(d->nheld == 0 && !d->eos_pending);
        h_cv_av1rel(dec, 1, sync); assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 1 && !h_gs_trywait(&sync));
        assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 6000 + round, false));
        settle(dec); assert(!d->reopen && h_gs_trywait(&sync));
        assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dec, 1) && ts == 6000 + round && w == 64 && d->nheld == 0);
    }
    // movie signal (dm_movie_hz): two pictures 1/60 s apart after a gap make a 60-picture movie; a flush keeps it, a stop ends it
    assert(dm_movie_hz() == 0);
    for (int k = 0; k < 2; k++) {
        h_cv_av1rel(dec, 1, sync); assert(!h_cv_av1acq(&idx, &sync, dec) && idx == 1);
        assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 1000000000 + k * 16666667, false));
        settle(dec); assert(h_gs_trywait(&sync));
    }
    assert(dm_movie_hz() == 60);
    assert(!h_cv_av1flush(dec) && dm_movie_hz() == 60);
    assert(!h_cv_av1stop(dec) && dm_movie_hz() == 0);
    assert(!h_cv_av1fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    memset(dec, 0, sizeof dec);
    assert(!h_cv_av1rm(dec, 0x10000, 64, 64, 1, 1, 3, 2) && cv_of(dec) && g_ev[ev_slot(ev)].bound && ev_set(ev));
    for (int i = 0; i < 6; i++) assert(!h_cv_av1submit(&id, dec, av1, sizeof av1, 7000 + i, false));
    assert(!h_cv_av1fin(dec) && !cv_of(dec) && !g_ev[ev_slot(ev)].bound);
    for (int i = 0; i < 3; i += 2) { h_cv_poolfin(pool[i]); assert(!pool_of(pool[i])); }
    // The movie library's start (subsdk0+0x3a40): Register, Acquire and RequestSyncPointEvent, then Start at once with no release.
    // The event stays armed until a real picture lands (fired at once, it races Start).
    {   _Alignas(16) static uint8_t dk[0x400], pk[0x40], mk[64 * 64 * 4], ek[0x80];
        assert(!h_cv_pool(pk, mk, size, &pref, 1) && pool_of(pk));
        assert(!h_cv_av1rm(dk, 0x10000, 64, 64, 1, 1, 3, 2)); cv_dec *k = cv_of(dk);
        assert(!h_spt_create(ek));
        assert(!h_cv_av1reg(dk, 0, pk, 0) && k->pcount == 1 && !h_cv_av1acq(&idx, &sync, dk) && idx == 0 && sync.gen == 1 && !h_gs_trywait(&sync));
        assert(!h_spt_request(ek, &sync) && !ev_set(ek) && SPT(ek)->armed);
        assert(h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dk, 0) == CV_NONE);
        assert(!h_cv_av1start(dk) && !ev_set(ek) && SPT(ek)->armed);
        assert(!h_cv_av1submit(&id, dk, av1, sizeof av1, 5000, false));
        settle(dk); assert(ev_set(ek) && !SPT(ek)->armed && !ev_by_me(ek) && h_gs_trywait(&sync));
        assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dk, 0) && w == 64 && h == 64 && ts == 5000 && !eos && !k->pcount);
        h_cv_av1rel(dk, 0, sync); assert(k->pcount == 1 && !h_cv_av1acq(&idx, &sync, dk) && idx == 0 && sync.gen == 2 && !h_gs_trywait(&sync));
        assert(!h_spt_request(ek, &sync) && !ev_set(ek) && SPT(ek)->armed);
        h_cv_av1rel(dk, 0, sync); assert(k->pcount == 1 && !h_cv_av1acq(&idx, &sync, dk) && idx == 0 && sync.gen == 3 && !h_gs_trywait(&sync));
        assert(!h_spt_request(ek, &sync) && !ev_set(ek) && SPT(ek)->armed);
        assert(!h_cv_av1submit(&id, dk, av1, sizeof av1, 6000, false));
        settle(dk); assert(ev_set(ek) && !SPT(ek)->armed && h_gs_trywait(&sync));
        assert(!h_cv_av1props(&w, &h, &cw, &ch, &ts, &eos, dk, 0) && ts == 6000);
        h_spt_destroy(ek);
        assert(!h_cv_av1fin(dk));
        h_cv_poolfin(pk);
    }
    {   // the transfer-memory initializer's last two arguments lie on the stack (8-byte slots)
        uint8_t da[0x400]; memset(da, 0, sizeof da);
        assert(!h_cv_av1tm(da, NULL, 0, 0x10000, 0xabcd0080, 0xabcd0040, 1, 3, 2, 2) && cv_of(da) && cv_of(da)->w == 0x80 && cv_of(da)->h == 0x40);
        assert(h_cv_av1event(da) == da + CV_EVENT_OFF && ev_set(da + CV_EVENT_OFF));
        static const uint8_t delimiter[] = {0x12, 0x00};
        uint64_t n; assert(!h_cv_av1submit(&n, da, delimiter, sizeof delimiter, 0, false));
        settle(da); assert(!cv_of(da)->nheld);
        for (int k = 0; k < 7; k++) assert(!h_cv_av1submit(&n, da, av1, sizeof av1, 9000 + k, k == 6));
        settle(da); assert(cv_of(da)->nheld == 7 && !cv_of(da)->eos_pending);
        for (int k = 0; k < 7; k++) assert(cv_of(da)->held_ts[k] == 9000 + k && cv_of(da)->held_eos[k] == (k == 6) && cv_of(da)->held[k]->w == 64);
        assert(!h_cv_av1fin(da) && !cv_of(da));
        // av1C from a hand-built sequence header: profile 0, level 12 tier 0, 2560x1440, 8-bit 4:2:0, chroma position 0
        uint8_t obu[32] = {0x0a, 0}; size_t bit = 16;
        #define PUT(v, nb) do { for (int k_ = (nb) - 1; k_ >= 0; k_--, bit++) if (((uint32_t)(v) >> k_) & 1) obu[bit / 8] |= (uint8_t)(0x80 >> (bit % 8)); } while (0)
        PUT(0, 3); PUT(0, 1); PUT(0, 1);            // profile, still picture, reduced header
        PUT(0, 1); PUT(0, 1); PUT(0, 5);            // no timing info, no display delay, one operating point
        PUT(0, 12); PUT(12, 5); PUT(0, 1);          // idc, level 12, tier
        PUT(11, 4); PUT(10, 4); PUT(2559, 12); PUT(1439, 11);   // frame size bits and frame size
        PUT(0, 1); PUT(0, 3); PUT(0, 4);            // no frame ids, block tools, inter tools
        PUT(1, 1); PUT(0, 2);                       // order hint, jnt_comp and ref_frame_mvs
        PUT(1, 1); PUT(1, 1);                       // screen content tools: choose; integer mv: choose
        PUT(6, 3); PUT(0, 3);                       // order hint bits, superres / cdef / restoration
        PUT(0, 1); PUT(0, 1); PUT(0, 1);            // not high bit depth, not monochrome, no colour description
        PUT(0, 1); PUT(0, 2); PUT(0, 1);            // colour range, chroma position, separate delta q
        #undef PUT
        obu[1] = (uint8_t)((bit + 7) / 8 - 2);
        uint8_t c[4]; int aw = 0, ah = 0;
        assert(mav1_av1c(obu, (bit + 7) / 8, c, &aw, &ah) && aw == 2560 && ah == 1440);
        assert(c[0] == 0x81 && c[1] == 12 && c[2] == 0x0c && c[3] == 0);
        size_t seqn = 0; assert(find_seq(obu, (bit + 7) / 8, &seqn) == obu && seqn == (bit + 7) / 8);
        assert(!mav1_av1c(delimiter, sizeof delimiter, c, &aw, &ah) && !find_seq(delimiter, sizeof delimiter, &seqn));
        // on each backend this Mac has (VideoToolbox where the chip decodes AV1; dav1d with DM_AV1_DAV1D=1)
        for (int soft = VTIsHardwareDecodeSupported(kCMVideoCodecType_AV1) ? 0 : 1; soft < 2; soft++) {
            setenv("DM_AV1_DAV1D", soft ? "1" : "0", 1);
            mav1 *a = mav1_new(NULL); assert(!strcmp(mav1_backend(a), soft ? "dav1d" : "VideoToolbox"));
            // dav1d holds several frames: a unit's picture comes with that unit's time, at once or at the drain (mav1_next)
            mav1_picture pic; int got = mav1_decode(a, av1, sizeof av1, 4242, &pic);
            if (!got) { assert(!strcmp(mav1_backend(a), "dav1d")); got = mav1_next(a, 1, &pic); }   // a VM's VideoToolbox session fails (-12906): dav1d instead
            assert(got == 1 && pic.ts == 4242 && pic.w == 64 && pic.h == 64 && pic.matrix == 1 && !pic.full_range);
            for (int y = 0; y < 64; y++) for (int x = 0; x < 64; x++) assert(abs(pic.y[(size_t)y * pic.ystride + x] - 81) <= 4);
            for (int y = 0; y < 32; y++) for (int x = 0; x < 64; x += 2) { assert(abs(pic.uv[(size_t)y * pic.uvstride + x] - 90) <= 6); assert(abs(pic.uv[(size_t)y * pic.uvstride + x + 1] - 240) <= 6); }
            assert(mav1_next(a, 1, &pic) == 0);
            int64_t want = 100;
            for (int k = 0; k < 9; k++) {
                for (int r = mav1_decode(a, av1, sizeof av1, 100 + k, &pic); r == 1; r = mav1_next(a, 0, &pic)) { assert(pic.ts == want && pic.w == 64); want++; }
                if (k == 4) while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want); want++; }
            }
            while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want && abs(pic.y[0] - 81) <= 4); want++; }
            assert(want == 109);
            // a drain left halfway breaks dav1d (dav1d_step): drains at any point, with 1-3 frames in flight
            uint32_t rng = 12345; int64_t sent = 1000; want = 1000;
            for (int k = 0; k < 600; k++) {
                for (int r = mav1_decode(a, av1, sizeof av1, sent++, &pic); r == 1; r = mav1_next(a, 0, &pic)) { assert(pic.ts == want); want++; }
                rng = rng * 1664525u + 1013904223u;
                if ((rng >> 28) < 5) { while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want); want++; } assert(want == sent); }
            }
            while (mav1_next(a, 1, &pic) == 1) { assert(pic.ts == want); want++; }
            assert(want == sent);
            mav1_reset(a); assert(mav1_decode(a, delimiter, sizeof delimiter, 0, &pic) == 0 && mav1_next(a, 1, &pic) == 0);
            mav1_free(a);
        }
        unsetenv("DM_AV1_DAV1D");
    }
    {
        cv_pref p = h_cv_pref(0xffff0020, 0xffff0010, 2, 2); assert(p.w == 32 && p.h == 16);
        uint64_t sz, al; assert(!h_cv_poolsize(&sz, &al, &p, 2) && al == CV_NATIVE_ALIGN && sz == 2 * CV_NATIVE_ALIGN);
        uint8_t pt[0x40] = {0}; void *m; assert(!posix_memalign(&m, al, sz)); memset(m, 0, sz);
        assert(!h_cv_pool(pt, m, sz, &p, 2));
        uint32_t ha = h_cv_native(pt, 1), nw, nh, ns; uint8_t *nm = NULL; uint64_t nz;
        assert(ha && dm_native_image(ha, &nm, &nz, &nw, &nh, &ns) && nm == (uint8_t *)m + CV_NATIVE_ALIGN && nz == CV_NATIVE_ALIGN && nw == 32 && nh == 16 && ns == 128);
        cv_pic *im = black_pic(32, 16);
        pool_native_write(pool_of(pt), 1, im); free(im);
        assert(nm[0] == 0 && nm[3] == 255 && nm[ns * 15 + 31 * 4 + 3] == 255);
        h_cv_poolfin(pt); assert(!dm_native_image(ha, &nm, &nz, &nw, &nh, &ns)); free(m);
    }
    {
        uint32_t nw, nh, ns; uint8_t *nm = NULL; uint64_t nz;
        assert(!dm_native_image(0, &nm, &nz, &nw, &nh, &ns) && !dm_native_image(0x80000001u, &nm, &nz, &nw, &nh, &ns));
    }
    printf("t_codec_video: all passed\n");
    return 0;
}
