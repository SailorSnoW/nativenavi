// nn::audio AudioRenderer (audren) -- in-process rendering: use the complete renderer the game's SDK ships with, no mixer of our own.
// Newer SDKs link the renderer server side (nn::audio::server: InfoUpdater / CommandGenerator ...) and the CPU version of the DSP command processing (nn::audio::dsp::CommandListProcessor:
// ADPCM / PCM decoding, resampling, mixing, reverb / delay / biquad / limiter / compressor ...) into the application, for the "CPU rendering + manual execution" mode (AudioRendererParameter
// +0x2c renderingDevice = 1 CPU, +0x30 executionMode = 1 manual). In this mode OpenAudioRenderer goes through the in-process DFC manager (CreateAudioRendererManagerByDfc),
// RequestUpdate is parsed in-process, and rendering happens on the CPU of whichever thread calls nn::audio::ExecuteAudioRendererRendering, which then signals the renderer event (the renderer's own: ar_open).
// A game can open more than one renderer, as on Horizon (Mario & Luigi: Brothership: one at 48 kHz and Unreal's mixer at 32 kHz): each gets its own
// state (ar_t), PCM blocks, render thread and output, and the sound card mixes them.
// The host does three things:
//   1. Takes over GetAudioRendererWorkBufferSize / OpenAudioRenderer: preserves the DSP service's work-buffer requirement, then runs the SDK's CPU + manual renderer.
//      The local SDK uses a different command format whose storage can exceed the service's budget; that backend storage is supplied by the host. Explicit CPU renderers keep the SDK contract;
//   2. A render thread (a guest thread started by the host): fills each returned 5 ms buffer, spacing fresh renders if the output returns several buffers together;
//   3. Takes over AudioRenderSystem::ExecuteAudioRendererRendering (reached through the PLT): generates commands and runs CommandListProcessor like the original,
//      then takes the frame out of the command list's DeviceSink (a no-op for the CPU processor): its six channels and a stereo fold, each clamped after mixing, queued for the output to select for the current route (hle_audio.c).
// What reads the game's data is in Rust (audren_rs.h; dm/host/rust/src/audren.rs, which also describes the command list's format): the work size and the frame out of each list.
#include "hle.h"
#include "audren_rs.h"
#include <mach/mach.h>
#include <mach/mach_time.h>
#include <mach/thread_policy.h>
#include <stdatomic.h>
#include <time.h>
#include <unistd.h>

void *dm_guest_thread_spawn(void (*fn)(void *), void *arg, int prio, size_t stack_size);
void dm_guest_thread_join(void *tt);
void dm_pcmout_close(void *k);
void dm_pcmout_start(void *k);
int dm_pcmout_append(void *k, void *b);
void *dm_pcmout_released(void *k);
void dm_pcmout_wait(void *k, int ms);

// P_SIZE: bytes of the game's AudioRendererParameter copied before the two fields are changed. SDK 16's struct is 0x38 bytes; SDK 8's (Link's Awakening) has one more word,
// its revision at +0x38: copying only 0x38 left the revision uninitialized and the SDK rejected it (CheckValidRevision, Result 0x499). The SDK reads only its own size of the copy
enum { P_DEVICE = 0x2c, P_MODE = 0x30, P_SIZE = 0x40, WORK_PAGE = 0x1000, FRAME_MAX = DM_AUDREN_FRAME_MAX, NBUF = 4 };
_Static_assert(DM_AUDREN_CHANNELS == DM_AUDIO_SINK_CHANNELS, "audren.rs writes the channels the output takes");
static void *gsym(const char *n) { void *p = guest_sym(n); if (!p) die("audren: missing SDK symbol %s", n); return p; }
static void cpu_manual(uint8_t *dst, const uint8_t *src) { memcpy(dst, src, P_SIZE); *(int32_t *)(dst + P_DEVICE) = 1; *(int32_t *)(dst + P_MODE) = 1; }

// Horizon's renderer service budget, through renderer revision 15: dm_audren_work_size (audren_rs.h; audren.rs work_size says how it was derived). The SDK's local server
// sizes its own CPU command structures, so forwarding its size would inflate the guest's service requirement. Unknown revisions retain the SDK requirement. SDK 8 keeps the
// public parameter revision at +0x38, while SDK 9+ uses +0x34 (the copied parameter holds both)

// An SDK without the renderer in the process (SDK 1.3.1: no ExecuteAudioRendererRendering, only CreateAudioRendererManagerByHipc) is only the client of audren:u, which this
// host does not serve. Ocarina of Time's SDK (22.2.8) renders in the process; for any other, the calls go to the SDK as the game made them and the log says so
static int ar_in_process(void) {
    static int v = -1;
    if (v < 0) { v = guest_sym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE") != NULL; if (!v) LOG("[RED] audren: the SDK has no renderer of its own: its client talks to audren:u, which this host does not serve"); }
    return v;
}
static size_t ar_service_work_size(const uint8_t *p) {
    uint32_t revision = *(const uint32_t *)(p + 0x34);
    if ((revision & 0xffffff) != 0x564552) revision = *(const uint32_t *)(p + 0x38);
    return dm_audren_work_size(p, revision);
}
HLE_FORCE("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE", "renderer work buffer: DSP service budget, with extra local CPU backend storage supplied by the host",
          size_t, h_ar_WorkSize, const uint8_t *param) {
    static size_t (*orig)(const uint8_t *); if (!orig) orig = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
    if (!ar_in_process()) return orig(param);
    uint8_t p[P_SIZE]; cpu_manual(p, param); size_t n = orig(p);
    size_t service = *(int32_t *)(param + P_DEVICE) == 0 ? ar_service_work_size(p) : 0;
    size_t guest_n = service ? service : n;
    { static int once; if (!once++) LOG("audren: work buffer guest %#zx, local CPU/manual renderer %#zx", guest_n, n); }
    return guest_n;
}
// ---------- rendering and output ----------
// One renderer's state. Only the render thread and the updates of this renderer touch it, apart from the one lock over all renderers' rendering (g_render_mu). Until 2026-11-04 this was one set of
// globals: a second renderer's updates rendered catch-up frames into the first one's output queue, about 10% of its frames (Mario & Luigi: Brothership), and its own frames were never rendered otherwise
enum { CQ = 4, AR_MAX = 4, RENDER_MIN_NS = 4500000 };   // AR_MAX as g_rev below
typedef struct {
    void *handle[2];                         // AudioRendererHandle (renderer object, client state); handle[0] NULL: a free slot
    uint8_t *sys;                            // its AudioRenderSystem, from its first render or update (ar_of_sys)
    _Atomic int open;
    void *render_tt;                         // the render thread (ThreadType from dm_guest_thread_spawn): Close joins it before the SDK tears the renderer down
    int16_t frame[FRAME_MAX * DM_AUDIO_SINK_CHANNELS]; int frame_n;   // the latest frame (six channels plus their stereo fold)
    _Atomic uint64_t frames, nonsilent, updates, renders, catchups;
    uint64_t last_r; int updated;            // the render count at the last update, once there was one
    int16_t cq[CQ][FRAME_MAX * DM_AUDIO_SINK_CHANNELS]; int cq_n[CQ]; uint32_t cq_head, cq_tail;   // output queue of catch-up frames
    // A fresh render holds cq_mu while the SDK signals the renderer event. A woken guest thread can enter RequestUpdate and need cq_mu before the signal's handoff returns.
    // Take the mutex with dm_host_lock so a thread waiting for it counts as blocked.
    pthread_mutex_t cq_mu; pthread_cond_t cq_cv;   // serializes catch-up publication, dequeue and fresh renders; wake on enqueue, dequeue or close
    dm_audren_state rs;                      // capture's state of this renderer (audren.rs State): the 3:2 resampler's history, whether its sink was logged
    int16_t pcm[NBUF][FRAME_MAX * DM_AUDIO_SINK_CHANNELS]; aout_buf bufs[NBUF];   // the blocks its output plays from (six channels plus their stereo fold): AudioOut queues only the pointer and copies the samples when the sound card asks (issue #1336)
} ar_t;
static ar_t g_ar[AR_MAX];
static pthread_mutex_t g_ar_mu = PTHREAD_MUTEX_INITIALIZER;   // the slots' handle and sys fields
static __thread ar_t *t_ar;                  // the renderer this render thread drives
static int ar_index(const ar_t *a) { return (int)(a - g_ar); }
// The renderer an AudioRenderSystem belongs to, for the SDK's calls that name only that. A system not seen yet belongs to the one open renderer whose system is not known: the render thread
// names its system at its first render, right after the open, and a game's first updates can come before that (two updates 0.04 ms apart at start-up, below)
static ar_t *ar_of_sys(uint8_t *sys) {
    pthread_mutex_lock(&g_ar_mu);
    ar_t *found = NULL, *unknown = NULL; int nunknown = 0;
    for (int i = 0; i < AR_MAX && !found; i++) {
        if (g_ar[i].sys == sys) found = &g_ar[i];
        else if (g_ar[i].handle[0] && !g_ar[i].sys) { unknown = &g_ar[i]; nunknown++; }
    }
    if (!found && nunknown == 1) { found = unknown; found->sys = sys; }
    pthread_mutex_unlock(&g_ar_mu);
    return found;
}

// QoS alone does not give this 5 ms producer a deadline: late wakeups drain the output even when mixing takes only 0.07 ms (#3059).
// The computation is a scheduling budget, not busy time. Keep output setup/teardown outside real-time policy and preserve burst-refill pacing.
static int render_rt(void) {
    const char *e = getenv("DM_AUDREN_RT");
    if (e && atoi(e) == 0) { LOG("audren: render thread keeps its default policy (DM_AUDREN_RT=0)"); return 0; }
    mach_timebase_info_data_t tb;
    kern_return_t kr = mach_timebase_info(&tb);
    if (kr == KERN_SUCCESS) {
        thread_time_constraint_policy_data_t p = {
            .period = (uint32_t)(5000000ull * tb.denom / tb.numer),
            .computation = (uint32_t)(1000000ull * tb.denom / tb.numer),
            .constraint = (uint32_t)(5000000ull * tb.denom / tb.numer), .preemptible = TRUE
        };
        kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    }
    LOG("%saudren: render thread time-constraint policy (requested period 5 ms, computation 1 ms) %s (%d)", kr == KERN_SUCCESS ? "" : "[RED] ",
        kr == KERN_SUCCESS ? "set" : "failed to set; keeping default policy", (int)kr);
    return kr == KERN_SUCCESS;
}
static void render_rt_end(void) {
    kern_return_t kr = thread_policy_set(pthread_mach_thread_np(pthread_self()), THREAD_STANDARD_POLICY, NULL, THREAD_STANDARD_POLICY_COUNT);
    if (kr != KERN_SUCCESS) LOG("[RED] audren: cannot restore render thread default policy (%d)", (int)kr);
}

static void render_main(void *arg) {
    ar_t *a = arg; t_ar = a;
    void (*exec)(void *, void *) = gsym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE");
    void *out = dm_pcmout_open_sink();
    aout_buf *free_list[NBUF]; int nfree = NBUF;
    for (int i = 0; i < NBUF; i++) { a->bufs[i] = (aout_buf){.ptr = a->pcm[i], .cap = sizeof a->pcm[i], .size = 0}; free_list[i] = &a->bufs[i]; }
    dm_pcmout_start(out);
    LOG("audren: render thread %d started (%d buffers in flight, paced by the output)", ar_index(a), NBUF);
    uint64_t next_exec = 0;
    int prefill = NBUF;
    int rt = render_rt();
    while (atomic_load(&a->open)) {
        while (!nfree) {   // wait for the output to hand a block back (silent sink / sound card, one every 5 ms); it signals each release, so the thread sleeps until then (it used to poll every 1 ms)
            aout_buf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            dm_pcmout_wait(out, 10);
            if (!atomic_load(&a->open)) break;
        }
        if (!nfree) break;
        dm_host_lock(&a->cq_mu);
        // Leave some slack below the 5 ms block duration. Device callbacks can return several blocks at once,
        // but back-to-back SDK renders can coalesce their events before the game can update.
        while (!prefill && a->cq_head == a->cq_tail && atomic_load(&a->open)) {
            uint64_t now = now_ns();
            if (now >= next_exec) break;
            struct timespec delay = { .tv_nsec = (long)(next_exec - now) };
            pthread_cond_timedwait_relative_np(&a->cq_cv, &a->cq_mu, &delay);
        }
        if (!atomic_load(&a->open)) { pthread_mutex_unlock(&a->cq_mu); break; }

        const int16_t *src; int got;
        int queued = a->cq_head != a->cq_tail;
        if (queued) {   // recheck after waiting, before executing a newer frame
            uint32_t slot = a->cq_head % CQ;
            src = a->cq[slot]; got = a->cq_n[slot];
        } else {
            next_exec = now_ns() + RENDER_MIN_NS;
            a->frame_n = 0;
            exec(a->handle[0], a->handle[1]);   // SDK: generate commands → CPU processing → signal the game's renderer event
            src = a->frame; got = a->frame_n;
        }
        int n = got ? got : 240;
        aout_buf *b = free_list[--nfree];
        size_t bytes = (size_t)n * DM_AUDIO_SINK_CHANNELS * sizeof(int16_t);
        if (got) memcpy(b->ptr, src, bytes); else memset(b->ptr, 0, bytes);
        b->size = bytes;
        if (queued) { a->cq_head++; pthread_cond_broadcast(&a->cq_cv); }   // the producer can reuse the slot after the copy
        pthread_mutex_unlock(&a->cq_mu);
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
        else if (prefill) prefill--;
    }
    if (rt) render_rt_end();
    dm_pcmout_close(out);   // the output would otherwise keep its sound card callback or silent sink running and hold one of the AudioOut slots after the renderer closes
}

// Both OpenAudioRenderer forms: CPU rendering + manual execution, then the host's render thread. With the event the game waits on it, which each render signals; without it the
// game paces its updates itself (Balatro: LÖVE's OpenAL Soft) and still needs the renderer to run every 5 ms: the form without an event used to start no render thread, so only the
// game's updates rendered (catch-up frames, which nothing played) and the game was silent.
// SDK 17.5.3's event form opens the renderer by calling the form without one through the PLT (sdk+0x1e489c, ROMANCE OF THE THREE KINGDOMS 8 REMAKE), which the loader binds to the
// hook below since that form has one (Balatro): the inner call started the render thread before the event form had created its event, and the outer call then logged a second
// renderer. The inner call (t_in_open) goes straight to the SDK with the host's copy of the parameter, as it did before the form without an event was hooked
static __thread int t_in_open;
// The renderer's own event (the write end the service keeps on Horizon). Every SDK's DSP open keeps no pointer to the game's SystemEventType: it stores NULL in the handle's event
// slot (SDK 3 sdk+0x17e004, SDK 12 sdk+0x4d2b4: handle[1]; SDK 18: *handle[1]) and attaches the service's readable handle to the game's object. The CPU + manual open the host
// substitutes stores the object in that slot instead (SDK 3 sdk+0x17dffc, SDK 12 sdk+0x4d274), and ExecuteAudioRendererRendering signals it after every render, so the game's
// object had to outlive the renderer, which Horizon never asks of it. LEGO Star Wars: The Skywalker Saga (SDK 12) passes one on its stack, Little Nightmares (SDK 3) reuses its
// memory, and the render thread's first SignalSystemEvent aborted on a state byte (+0x28) that was no longer the event's. The SDK now gets one of these, alive until Close
static struct { void *obj; uint8_t ev[0x80] __attribute__((aligned(16))); } g_rev[4];   // obj: the renderer (handle[0]) it belongs to, NULL when free
static pthread_mutex_t g_rev_mu = PTHREAD_MUTEX_INITIALIZER;
static void *rev_take(void) {
    pthread_mutex_lock(&g_rev_mu); void *ev = NULL;
    for (int i = 0; i < 4 && !ev; i++) if (!g_rev[i].obj) { g_rev[i].obj = (void *)1; memset(g_rev[i].ev, 0, sizeof g_rev[i].ev); ev = g_rev[i].ev; }
    pthread_mutex_unlock(&g_rev_mu); return ev;
}
static void rev_set(void *ev, void *obj) {   // obj NULL frees the slot
    pthread_mutex_lock(&g_rev_mu);
    for (int i = 0; i < 4; i++) if (g_rev[i].ev == ev) g_rev[i].obj = obj;
    pthread_mutex_unlock(&g_rev_mu);
}
static void *rev_find(void *obj) {
    pthread_mutex_lock(&g_rev_mu); void *ev = NULL;
    for (int i = 0; i < 4 && !ev; i++) if (g_rev[i].obj == obj) ev = g_rev[i].ev;
    pthread_mutex_unlock(&g_rev_mu); return ev;
}
static Result ar_open(void **handle, void *ev, const uint8_t *param, void *work, size_t sz, void *ra) {
    static Result (*orig_ev)(void **, void *, const uint8_t *, void *, size_t), (*orig)(void **, const uint8_t *, void *, size_t);
    if (!orig_ev) orig_ev = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm");
    if (!orig) orig = gsym("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm");
    if (t_in_open || !ar_in_process()) return ev ? orig_ev(handle, ev, param, work, sz) : orig(handle, param, work, sz);
    uint8_t p[P_SIZE]; cpu_manual(p, param);
    LOG("audren: OpenAudioRenderer%s rate %d samples per frame %d mix buffers %d submixes %d voices %d sinks %d effects %d splitters %d (original device %d mode %d → CPU / manual) work buffer %p+%#zx",
        ev ? "" : " (no event)", *(int32_t *)param, *(int32_t *)(param + 4), *(int32_t *)(param + 8), *(int32_t *)(param + 0xc), *(int32_t *)(param + 0x10), *(int32_t *)(param + 0x14),
        *(int32_t *)(param + 0x18), *(int32_t *)(param + 0x24), *(int32_t *)(param + P_DEVICE), *(int32_t *)(param + P_MODE), work, sz);
    // The application's buffer follows the DSP service contract; the local server's larger CPU command representation is a backend cost. Keep a full-size caller buffer
    // when it fits the backend, otherwise supply the backend storage here. The nested no-event open receives this same buffer and starts no second thread.
    void *owned = NULL;
    if (*(int32_t *)(param + P_DEVICE) == 0 && work && !((uintptr_t)work & (WORK_PAGE - 1)) && sz >= WORK_PAGE && !(sz & (WORK_PAGE - 1))) {
        size_t (*size)(const uint8_t *) = gsym("_ZN2nn5audio30GetAudioRendererWorkBufferSizeERKNS0_22AudioRendererParameterE");
        size_t need = size(p);
        size_t service = ar_service_work_size(p), guest_need = service ? service : need;
        if (sz < guest_need) { LOG("[RED] audren: work buffer %#zx is smaller than the service requirement %#zx", sz, guest_need); return RES_FAIL; }
        if (need > sz) {
            if (posix_memalign(&owned, WORK_PAGE, need)) { LOG("[RED] audren: cannot allocate local renderer work buffer %#zx", need); return RES_FAIL; }
            memset(owned, 0, need); work = owned; sz = need;
            LOG("audren: host backs local renderer work buffer %p+%#zx", work, sz);
        }
    }
    // A game that opens a CPU renderer itself gets the SDK's contract: the SDK keeps its event, as on Horizon. The nested open (t_in_open) gets the outer open's event
    void *sev = ev && *(int32_t *)(param + P_DEVICE) == 0 ? rev_take() : NULL;
    if (ev && *(int32_t *)(param + P_DEVICE) == 0 && !sev) LOG("[RED] audren: all renderer events in use: the SDK keeps the game's event, which must then outlive the renderer");
    t_in_open = 1; Result r = ev ? orig_ev(handle, sev ? sev : ev, p, work, sz) : orig(handle, p, work, sz); t_in_open = 0;
    if (r) { free(owned); rev_set(sev, NULL); LOG("[RED] audren: SDK OpenAudioRenderer failed %#x", r); return r; }
    if (owned) objmap_put(handle[0], owned);   // Close releases it only after the render thread and the SDK have finished with it
    // The renderer event as the service gives it. With the DSP service (device 0, what the game asked for) OpenAudioRenderer attaches the service's kernel event (auto-clear,
    // AttachReadableHandleToSystemEvent); with CPU + manual it creates a local one instead (CreateSystemEvent(ev, AutoClear, false), SDK 4: sdk+0x190b6c). A signal of a local
    // auto-clear event wakes only its highest-priority waiter, a kernel event wakes every waiter (each tries ResetSignal). Donkey Kong Country: Tropical Freeze waits on it from
    // two threads: its audio thread (priority 29) every frame, and the main thread while CAudioMemoryPool::Shutdown detaches a pool as a level loads (main+0x6fa8c); the audio
    // thread took every wake and the main thread waited forever. So the renderer's own event becomes a kernel event (auto-clear), and the game's object gets a copy of its
    // readable handle, managed by the game as the service's copied handle is: the game may destroy or reuse its object at any time, and the renderer never touches it again
    if (ev && *(int32_t *)(param + P_DEVICE) == 0) {
        static void (*destroy)(void *); if (!destroy) destroy = gsym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        if (sev) {
            static void (*attach)(void *, uint32_t, int, int); static uint32_t (*getr)(const void *);
            if (!attach) { attach = gsym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
                           getr = gsym("_ZN2nn2os30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
            destroy(sev); hle_bind_system_event(sev, 1);
            attach(ev, hle_dup_readable_handle(getr(sev)), 1, 1);   // managed (the game's DestroySystemEvent closes its copy), EventClearMode_AutoClear
            rev_set(sev, handle[0]);
        } else { destroy(ev); hle_bind_system_event(ev, 1); }   // no free renderer event: the earlier behaviour, which needs the game's object to live
    }
    module_t *m = module_of(ra);
    pthread_mutex_lock(&g_ar_mu);
    ar_t *a = NULL;
    for (int i = 0; i < AR_MAX && !a; i++) if (!g_ar[i].handle[0]) a = &g_ar[i];
    if (a) {   // the slot's old contents are of a closed renderer, whose thread has ended; memset leaves no lock held
        memset(a, 0, sizeof *a);
        pthread_mutex_init(&a->cq_mu, NULL); pthread_cond_init(&a->cq_cv, NULL);
        a->handle[0] = handle[0]; a->handle[1] = handle[1]; atomic_store(&a->open, 1);
    }
    pthread_mutex_unlock(&g_ar_mu);
    LOG("audren: renderer %d opened from %s+%#llx", a ? ar_index(a) : -1, m ? m->name : "host", m ? (unsigned long long)((uint8_t *)ra - m->base) : 0ULL);
    if (!a) { LOG("[RED] audren: more than %d renderers: this one is not rendered", AR_MAX); return r; }
    a->render_tt = dm_guest_thread_spawn(render_main, a, 9, 0x10000);
    return r;
}
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleEPNS_2os11SystemEventERKNS0_22AudioRendererParameterEPvm", "open the renderer (with event): CPU rendering + manual execution, the host starts a render thread",
          Result, h_ar_Open, void **handle, void *ev, const uint8_t *param, void *work, size_t sz) { return ar_open(handle, ev, param, work, sz, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio17OpenAudioRendererEPNS0_19AudioRendererHandleERKNS0_22AudioRendererParameterEPvm", "open the renderer (no event): CPU rendering + manual execution, the host starts a render thread",
          Result, h_ar_Open0, void **handle, const uint8_t *param, void *work, size_t sz) { return ar_open(handle, NULL, param, work, sz, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE", "close the renderer: stop the host render thread first, then hand over to the SDK", void, h_ar_Close, void *obj, void *state) {
    static void (*orig)(void *, void *); if (!orig) orig = gsym("_ZN2nn5audio18CloseAudioRendererENS0_19AudioRendererHandleE");
    if (!ar_in_process()) { orig(obj, state); return; }
    // Join the render thread first (it checks open every round and at most every 10 ms while waiting for the output): it may be inside exec(), which uses the renderer the SDK is about to free.
    // A fixed 20 ms sleep used to stand in for this, and a renderer reopened inside that window ran a second thread over the first one's blocks
    ar_t *a = NULL;
    for (int i = 0; i < AR_MAX && !a; i++) if (g_ar[i].handle[0] == obj) a = &g_ar[i];
    if (a && a->render_tt) {
        dm_host_lock(&a->cq_mu);
        atomic_store(&a->open, 0);
        pthread_cond_broadcast(&a->cq_cv);
        pthread_mutex_unlock(&a->cq_mu);
        dm_guest_thread_join(a->render_tt);
        a->render_tt = NULL;
    }
    void *owned = objmap_del(obj), *sev = rev_find(obj);
    orig(obj, state);
    free(owned);
    if (a) { pthread_mutex_lock(&g_ar_mu); a->handle[0] = NULL; a->sys = NULL; pthread_mutex_unlock(&g_ar_mu); }
    if (sev) {   // the SDK's Close does not touch the event (SDK 3 to 18); destroying it closes the renderer's ends, and the game's copied read end stays the game's
        static void (*destroy)(void *); if (!destroy) destroy = gsym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        destroy(sev); rev_set(sev, NULL);
    }
}

// A reset (hle_reset.c): each render thread leaves its loop as at Close (it checks open every round and every 10 ms while waiting for the output), closes its output and returns, and
// the SDK's thread entry then ends it at ExitThread's gate. Its renderer goes with the game's memory; the game's own Close never comes
void dm_audren_reset_stop(void) {
    for (int i = 0; i < AR_MAX; i++) {
        ar_t *a = &g_ar[i];
        if (!a->handle[0] || !a->render_tt) continue;
        pthread_mutex_lock(&a->cq_mu); atomic_store(&a->open, 0); pthread_cond_broadcast(&a->cq_cv); pthread_mutex_unlock(&a->cq_mu);
    }
}

// A renderer at 32 kHz (DRAGON QUEST VII Reimagined: 160 samples per 5 ms frame) has its DeviceSink upsampled to the output's 48 kHz by the SDK's upsampler, which AudioRenderSystem::
//   SetUpRenderResourceSet allocates only for the DSP (SDK 19: sdk+0x21eb6c, sample rate 0x7d00 and rendering device +0x48 == 0). With the host's CPU rendering there is no
//   UpsamplerManager, and the sink's command generation (GenerateSinkCommands, sdk+0x225a80) called Allocate on null and crashed in its mutex. The takeover below answers "no upsampler"
//   for a null manager (the generator then skips the upsample command, sdk+0x225b40), and the host upsamples the sink's output 3:2 itself
//   (audren.rs up_3_2: a windowed sinc, 16 taps in three phases)
HLE_FORCE("_ZN2nn5audio6server16UpsamplerManager8AllocateEv", "upsampler for a DeviceSink below 48 kHz: none with CPU rendering (no manager), the host resamples its output",
          void *, h_up_Allocate, void *mgr) {
    static void *(*orig)(void *); if (!orig) orig = gsym("_ZN2nn5audio6server16UpsamplerManager8AllocateEv");
    return mgr ? orig(mgr) : NULL;
}

// The frame out of a command list (audren.rs capture: the DeviceSink's mix buffers, six channels then their stereo fold per sample, resampled to 48 kHz) into dst, which
// holds FRAME_MAX samples; its sample count, 0 for none. g_scan is what capture keeps for all renderers (whether a list has walked yet, the sink's ID, the frame being
// mixed): capture runs under g_render_mu (render_once)
static dm_audren_scan g_scan = DM_AUDREN_SCAN_INIT;
static int capture(ar_t *a, const uint8_t *cl, size_t len, int16_t *dst) {
    int loud = 0, n = dm_audren_capture(&g_scan, &a->rs, cl, len, dst, FRAME_MAX * DM_AUDIO_SINK_CHANNELS, &loud);
    if (n < 0) return 0;
    atomic_fetch_add(&a->frames, 1); if (loud) atomic_fetch_add(&a->nonsilent, 1);
    uint64_t f = atomic_load(&a->frames); if (f == 200 || f % 12000 == 0) LOG("audren: renderer %d rendered %llu frames, %llu of them not silent", ar_index(a), (unsigned long long)f, (unsigned long long)atomic_load(&a->nonsilent));
    return n;
}
// Render one frame (generate commands + CPU processing); dst = output (six channels and their stereo fold, *nout samples). The two callers (render thread, catch-up) are serialized by a host lock: they share one command buffer.
// One lock over all renderers: capture keeps what it learns from the first lists (whether one has walked yet, the sink's ID) for all of them; a frame takes hundredths of a millisecond (0.02 ms mean)
static pthread_mutex_t g_render_mu = PTHREAD_MUTEX_INITIALIZER;
// The AudioRenderSystem of SDK 20 on (The Legend of Zelda: Breath of the Wild 1.9.0's sdk+0x22a70c ExecuteAudioRendererRendering): the member functions for mode / device / command buffer are inlined
//   (+0x48 rendering device and +0x4c execution mode, both must be 1; +0xf8 / +0x100 command buffer start / length), and GenerateCommand takes a Span<std::byte> and returns a Span
//   (x0 start, x1 length: the generated command list)
typedef struct { uint8_t *p; size_t n; } span_t;
// The update lock is read off the SDK's own code: an SdkMutexType inside the AudioRenderSystem whose offset moves between SDKs (+0x2d0 in 19.3–21.4, +0x2d8 in 22.2, the sdk builds of the games on hand).
//   AudioRenderSystem::Update reads the system tick and calls UpdateImpl, which takes it with SdkMutexType::Lock. A fixed offset taken for another SDK wrote a lock word into another field and excluded
//   nothing: on The Legend of Zelda: Ocarina of Time (22.2.8, +0x2d0) the game's RequestUpdate rewrote a voice while its commands were generated, and AddressInfo::GetReference read a memory pool pointer
//   that had just become null. The prologue copies `this`, adds the offset into a register and moves it to x0 before the first bl, a PLT stub whose GOT slot the loader bound to the lock function
static void *plt_target(const uint32_t *pc) {   // adrp x16, page; ldr x17, [x16, #off]; …; br x17 → the function the GOT slot holds; else the address itself
    uint32_t a = pc[0], l = pc[1];
    if ((a & 0x9f00001fu) != 0x90000010u || (l & 0xffc003ffu) != 0xf9400211u) return (void *)pc;
    int64_t imm = (int64_t)((uint64_t)((((a >> 5) & 0x7ffff) << 2) | ((a >> 29) & 3)) << 43) >> 31;   // immhi:immlo, sign-extended, × 4 KB
    uint64_t page = ((uint64_t)pc & ~0xfffull) + (uint64_t)imm;
    return *(void **)(page + ((l >> 10) & 0xfff) * 8);
}
static int lock_in(const char *fn, size_t *off) {
    const uint32_t *f = guest_sym(fn);
    void *sdk_lock = guest_sym("_ZN2nn2os12SdkMutexType4LockEv");
    if (!f) return 0;
    uint32_t self = 1u << 0; int64_t add[32]; for (int r = 0; r < 32; r++) add[r] = -1;
    int64_t x0 = -1;
    for (int i = 0; i < 24; i++) {
        uint32_t w = f[i]; int rd = w & 31, rn = (w >> 5) & 31, rm = (w >> 16) & 31;
        if ((w & 0xffe0ffe0u) == 0xaa0003e0u) {   // mov xd, xm
            if (self >> rm & 1) self |= 1u << rd;
            if (rd == 0) x0 = add[rm];
            else add[rd] = add[rm];
        } else if ((w & 0xffc00000u) == 0x91000000u && rn != 31 && (self >> rn & 1)) {   // add xd, x<this>, #imm12
            add[rd] = (w >> 10) & 0xfff; if (rd == 0) x0 = add[rd];
        } else if ((w & 0xfc000000u) == 0x94000000u) {   // the first bl
            void *t = plt_target(f + i + (int32_t)(w << 6) / 64);
            if (x0 < 0 || !t || t != sdk_lock) return 0;
            *off = (size_t)x0; return 1;
        }
    }
    return 0;
}
// Update, then UpdateImpl: *off stays as it is when neither takes a lock it can read, which leaves the caller's guess and may exclude nothing, so that is a red line
static int update_lock(size_t *off) {
    if (lock_in("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", off) || lock_in("_ZN2nn5audio6server17AudioRenderSystem10UpdateImplEPvmS3_mPKvm", off)) return 1;
    LOG("[RED] audren: no update lock read off AudioRenderSystem::Update or UpdateImpl: keeping +%#zx, where command generation may race the game's RequestUpdate", *off);
    return 0;
}
static Result render_once(ar_t *a, uint8_t *sys, int16_t *dst, int *nout) {
    static span_t (*gen)(void *, void *, size_t); static size_t mu_off;
    static void (*tmax)(void *, uint32_t), (*setup)(void *, const void *, size_t), (*process)(void *), (*lock)(void *), (*unlock)(void *);
    if (!process) {
        gen = gsym("_ZN2nn5audio6server17AudioRenderSystem15GenerateCommandENS_4SpanISt4byteEE");
        mu_off = 0x2d0;
        update_lock(&mu_off);
        lock = gsym("_ZN2nn2os12SdkMutexType4LockEv"); unlock = gsym("_ZN2nn2os12SdkMutexType6UnlockEv");
        LOG("audren: AudioRenderSystem update lock SdkMutexType at +%#zx", mu_off);
        tmax = gsym("_ZN2nn5audio3dsp20CommandListProcessor17SetProcessTimeMaxEj"); setup = gsym("_ZN2nn5audio3dsp20CommandListProcessor5SetupEPKvm");
        process = gsym("_ZN2nn5audio3dsp20CommandListProcessor7ProcessEv");
    }
    int md = *(int32_t *)(sys + 0x4c), dv = *(int32_t *)(sys + 0x48);
    if (md != 1 || dv != 1) return 0x40299;   // same as the original: manual rendering needs CPU + manual mode
    pthread_mutex_lock(&g_render_mu);
    uint64_t t0 = now_ns();
    // Command generation must exclude the game thread's RequestUpdate (AudioRenderSystem::Update, which holds the mutex at mu_off): the original system thread (SendCommandToDsp) generates under the same lock. Processing (the CPU DSP) stays outside the lock, as in the original.
    lock(sys + mu_off);
    span_t sp = gen(sys, *(uint8_t **)(sys + 0xf8), *(size_t *)(sys + 0x100)); uint8_t *cl = sp.p; size_t n = sp.n;
    unlock(sys + mu_off);
    atomic_fetch_add(&a->renders, 1);
    uint64_t proc[16] __attribute__((aligned(16))) = {0};   // CommandListProcessor (0x60 bytes in SDK 21, whose Process reads +0x58)
    tmax(proc, 0xffffffffu); setup(proc, cl, n); process(proc);
    if (dst) *nout = capture(a, cl, n, dst);
    // render time per frame (budget: 5 ms per frame): mean / max every 2000 frames
    static uint64_t sum, mx, cnt; uint64_t d = now_ns() - t0; sum += d; if (d > mx) mx = d;
    if (++cnt == 2000) { LOG("audren: render time mean %.2f ms / max %.2f ms (5 ms budget per frame, last 2000 frames); catch-up renders so far %llu", sum / 2000 / 1e6, mx / 1e6, (unsigned long long)atomic_load(&a->catchups)); sum = mx = cnt = 0; }
    pthread_mutex_unlock(&g_render_mu);
    return RES_OK;
}
// A render from the host's render thread names the thread's renderer and its system; a game that executes its own CPU renderer renders into that renderer's frame, which nothing plays, as before
static Result ar_exec(uint8_t *sys) {
    ar_t *a = t_ar;
    if (a && !a->sys) { pthread_mutex_lock(&g_ar_mu); a->sys = sys; pthread_mutex_unlock(&g_ar_mu); }
    if (!a) a = ar_of_sys(sys);
    if (!a) { LOG("[RED] audren: a render of a renderer the host did not open"); return RES_FAIL; }
    return render_once(a, sys, a->frame, &a->frame_n);
}
// From SDK 20 (The Legend of Zelda: Breath of the Wild 1.9.0) the public ExecuteAudioRendererRendering (sdk+0x221e90) calls the AudioRendererImpl version through the vtable (sdk+0x22ca88, just x0 += 0x60 and a jump through the PLT to here).
// The original generates commands here without the update lock; with the render thread on the original and the game thread's RequestUpdate rendering catch-up frames ("nothing rendered since the last update"),
// both sides wrote the same command buffer at once and CommandListProcessor::Process sometimes read a half-written command list and crashed (once at 60 s and once at 134 s in the shrine). Taken over here, so rendering goes through the host's locked render
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv", "render one frame manually (reached through the PLT): SDK generates commands + CPU processing under the host's lock, the host takes the DeviceSink output",
          Result, h_ars_Exec, uint8_t *sys) { return ar_exec(sys); }

// At least one rendered frame between two RequestUpdates. An effect's "needs initialization" is set only by the update in which the game flags it as new (e.g. LightLimiterInfo::Update: isNew → parameter status 0);
// the next update overwrites it with the game's value; if no commands were generated in between, the DSP never receives the initialization and ApplyLightLimiterEffect later reads a null pointer. The real renderer produces a frame every 5 ms on its own
// and the game waits on the renderer event, so normally it is one update per frame; but at start-up the game updates twice in a row (0.04 ms apart), and with real audio output the sound card opens slowly and the first frame comes late, leaving no frame in between.
// Here, when nothing has been rendered since the last update, one catch-up frame is rendered on the current thread first; its output is queued and played by the render thread in place of its next render.
// Catch-up frames keep real time: when the queue is full the update waits for the render thread to play one (at most 50 ms). Square Enix's sound driver (DRAGON QUEST XI S,
// SQEX::Sd::Driver::Core::CoreAudioOut, main+0x442c5c0) refills its voices in a loop: append a wave buffer, RequestUpdate, take the next released buffer, and it waits on the renderer
// event only when none is released. Every update there renders a catch-up frame, which releases the buffer just appended, so the loop never waits: discarding catch-up output when the
// queue was full ran it at 11,000 updates a second on one core, the game's sound about 50 times faster than the output. Waiting keeps the queue full instead: the render thread then
// only plays queued frames, and the driver's own updates render every frame. A queued frame does not signal the renderer event: the SDK signals it in each render, and a game woken by a
// queued frame updated with nothing rendered since, which rendered the next catch-up frame, and so on (Big Brain Academy: Brain vs. Brain, one catch-up per update instead of 37 a run)
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", "renderer update: if nothing was rendered since the last update, render a catch-up frame first (so effects get initialized), then hand over to the SDK",
          Result, h_ars_Update, uint8_t *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    static Result (*orig)(void *, void *, size_t, void *, size_t, const void *, size_t); if (!orig) orig = gsym("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    ar_t *a = ar_of_sys(sys);
    if (!a) return orig(sys, o, on, p, pn, in, inn);   // two renderers just opened, neither has rendered: at most one catch-up frame less
    uint64_t u = atomic_fetch_add(&a->updates, 1) + 1;
    dm_host_lock(&a->cq_mu);
    if (atomic_load(&a->open) && a->updated && atomic_load(&a->renders) == a->last_r) {
        uint64_t c = atomic_fetch_add(&a->catchups, 1) + 1;
        if (c <= 3) LOG("audren: renderer %d: no frame rendered before update %llu, rendering a catch-up frame first (%llu so far)", ar_index(a), (unsigned long long)u, (unsigned long long)c);
        if (a->cq_tail - a->cq_head >= CQ) {   // queue full: wait for the render thread to play one (every 5 ms), so catch-up frames keep real time; at most 50 ms (an output that stopped)
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 50000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            while (atomic_load(&a->open) && a->cq_tail - a->cq_head >= CQ && !pthread_cond_timedwait(&a->cq_cv, &a->cq_mu, &dl)) {}
        }
        if (atomic_load(&a->open)) {
            if (a->cq_tail - a->cq_head < CQ) {
                uint32_t slot = a->cq_tail % CQ;
                render_once(a, sys, a->cq[slot], &a->cq_n[slot]);
                a->cq_tail++;
                pthread_cond_broadcast(&a->cq_cv);
            } else render_once(a, sys, NULL, NULL);   // still full (the output stopped): render anyway, output discarded
        }
    }
    if (u < 200 && getenv("DM_AUDREN_TRACE")) LOG("audren update #%llu t=%.3f ms, %llu frames rendered since the last update", (unsigned long long)u, now_ns() / 1e6, (unsigned long long)(atomic_load(&a->renders) - (a->updated ? a->last_r : 0)));
    pthread_mutex_unlock(&a->cq_mu);
    Result res = orig(sys, o, on, p, pn, in, inn);
    dm_host_lock(&a->cq_mu);
    a->last_r = atomic_load(&a->renders); a->updated = 1;
    pthread_mutex_unlock(&a->cq_mu);
    return res;
}

// Output device: one logical output, with the current route's channels (the audio device service still goes through IPC; the host answers directly)
enum { DEVNAME = 0x100 };
static void out_devname(char *out) { memset(out, 0, DEVNAME); strlcpy(out, "AudioBuiltInSpeakerOutput", DEVNAME); }
HLE_FORCE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", "output device list: 1 built-in speaker", int, h_ad_List, char *names, int n) { if (n <= 0 || !names) return 0; out_devname(names); return 1; }
HLE_FORCE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", "active output device: built-in speaker", void, h_ad_Active, char *name) { out_devname(name); }
HLE_FORCE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", "output channel count: the route's (6 on a six-channel device, else 2)", int, h_ad_Channels, void) { return dm_audio_route(); }
// Device volume: kept and read back (DRAGON QUEST VII Reimagined reads it at boot, main+0xee8edc), not applied: the host's output has the system's own volume
static _Atomic float g_dev_vol = 1.0f;
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "device volume; kept for the getter, not applied by the host", void, h_ad_SetVol, const char *name, float v) { (void)name; atomic_store(&g_dev_vol, v); }
HLE_FORCE("_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", "device volume: what the game last set (1.0 at start)", float, h_ad_GetVol, const char *name) { (void)name; return atomic_load(&g_dev_vol); }
// Output volume auto-tune (Switch 2 SDKs, through the audio device service the host does not serve; Donkey Kong Bananza reads it at boot, main+0x57dc7c): kept and read back, off at start
static atomic_uchar g_dev_autotune;
HLE_FORCE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", "device volume auto-tune; kept for the getter, not applied by the host", void, h_ad_SetAutoTune, unsigned on) { atomic_store(&g_dev_autotune, on & 1); }
HLE_FORCE("_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", "device volume auto-tune: what the game last set (off at start)", unsigned, h_ad_AutoTune, void) { return atomic_load(&g_dev_autotune); }
// The output device change notification, signaled when the host's route changes (dm_audio_device_changed, from hle_audio.c). Returns a Result: 0, or 0x499 when the notification is
// already acquired, before which it binds nothing: the flag the SDKs before 21 kept for the process (Metroid Dread's sdk+0x534a0). Fire Emblem Fortune's Weave's sdk (SDK 22.2.0)
// calls it on an audio thread at start (main+0xe7b9c): unserved, its user session with aud:u aborted (Result 0xe15). That sdk's version (sdk+0x1f1e00) keeps no per-process flag and
// attaches the event AutoClear; the two differ only on a second acquire (0x499 here), which no game has been seen to make
enum { AD_FREE, AD_BOUND, AD_CHANGING };
static atomic_int g_ad_notify;
static _Atomic uint64_t g_ad_binding;
void dm_audio_device_changed(void) { hle_signal_system_event_binding(atomic_load(&g_ad_binding)); }
HLE_FORCE("_ZN2nn5audio36AcquireAudioOutputDeviceNotificationEPNS_2os11SystemEventE", "output device change notification (SDK 21 name): follows the host route", Result, h_ad_Notify21, void *ev) {
    int expected = AD_FREE;
    if (!atomic_compare_exchange_strong(&g_ad_notify, &expected, AD_CHANGING)) return 0x499;
    dm_audio_route();   // start watching the route before publishing the subscriber
    hle_bind_system_event(ev, 0);
    atomic_store(&g_ad_binding, hle_system_event_binding(ev));
    atomic_store(&g_ad_notify, AD_BOUND);
    return RES_OK;
}
// Its release (Nintendo Switch Sports Resort, main+0x3af119c). The SDK's (sdk+0x1f2ac0) asks aud:u through the device session coordinator (unserved: it would abort as the acquire did),
// then destroys the event; the game destroys the event itself only if it is still initialized afterwards (main+0x3af11a0). The host destroys the event it bound, which drops the
// binding, and clears the flag so a later acquire binds again; with nothing acquired there is nothing to destroy. Returns nothing (the SDK aborts on a failed release)
HLE_FORCE("_ZN2nn5audio36ReleaseAudioOutputDeviceNotificationEPNS_2os11SystemEventE", "release the output device change notification (SDK 21 name): the event is destroyed",
          void, h_ad_NotifyRelease21, void *ev) {
    int expected = AD_BOUND;
    if (!atomic_compare_exchange_strong(&g_ad_notify, &expected, AD_CHANGING)) return;
    // Keep another acquire out until destruction is complete; it must not publish a binding that this release clears.
    atomic_store(&g_ad_binding, 0);
    static void (*destroy)(void *); if (!destroy) destroy = gsym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    destroy(ev);
    atomic_store(&g_ad_notify, AD_FREE);
}

// A reset (hle_reset.c), once every guest thread has ended (the render threads among them, dm_audren_reset_stop) and before the game's memory goes: the renderers' host state as a new process has it. The SDK's
// renderer and the render thread's ThreadType and stack were in the game's memory; the host's work buffers, thread records and the renderer events it lends the SDK are freed
void dm_audren_reset(void) {
    int n = 0;
    pthread_mutex_lock(&g_ar_mu);
    for (int i = 0; i < AR_MAX; i++) {
        ar_t *a = &g_ar[i];
        if (!a->handle[0]) continue;
        free(objmap_del(a->handle[0])); free(a->render_tt);   // the host_thread record (hle_guestthread.c); its stack was the SDK's malloc
        a->render_tt = NULL; a->handle[0] = a->handle[1] = NULL; a->sys = NULL; n++;
    }
    pthread_mutex_unlock(&g_ar_mu);
    pthread_mutex_lock(&g_rev_mu); for (int i = 0; i < 4; i++) { g_rev[i].obj = NULL; memset(g_rev[i].ev, 0, sizeof g_rev[i].ev); } pthread_mutex_unlock(&g_rev_mu);
    pthread_mutex_lock(&g_render_mu); g_scan = (dm_audren_scan)DM_AUDREN_SCAN_INIT; pthread_mutex_unlock(&g_render_mu);
    atomic_store(&g_dev_vol, 1.0f); atomic_store(&g_dev_autotune, 0);
    atomic_store(&g_ad_binding, 0); atomic_store(&g_ad_notify, AD_FREE);   // the device change notification the game acquired: a second Acquire would be refused (0x499)
    LOG("reset: audio renderer: %d renderers forgotten", n);
}
