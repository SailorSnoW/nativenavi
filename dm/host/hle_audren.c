// nn::audio AudioRenderer in process: the game's SDK renderer in CPU rendering + manual execution mode, no mixer of our own (docs/reference/audio.md).
// The host keeps the DSP service's work-buffer size, runs a render thread per renderer, and takes over ExecuteAudioRendererRendering to take each
// frame out of the command list's DeviceSink (six channels and a stereo fold, for hle_audio.c). Parsing is in Rust (audren_rs.h).
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

// SDK 8's parameter has its revision at +0x38: copy past it. The SDK reads only its own size.
enum { P_DEVICE = 0x2c, P_MODE = 0x30, P_SIZE = 0x40, WORK_PAGE = 0x1000, FRAME_MAX = DM_AUDREN_FRAME_MAX, NBUF = 4 };
_Static_assert(DM_AUDREN_CHANNELS == DM_AUDIO_SINK_CHANNELS, "audren.rs writes the channels the output takes");
static void *gsym(const char *n) {
    void *p = guest_sym(n);
    if (!p) die("audren: missing SDK symbol %s", n);
    return p;
}
static void cpu_manual(uint8_t *dst, const uint8_t *src) {
    memcpy(dst, src, P_SIZE);
    *(int32_t *)(dst + P_DEVICE) = 1;
    *(int32_t *)(dst + P_MODE) = 1;
}

// Horizon's service budget: the SDK's local server sizes larger CPU structures. Unknown revisions keep the SDK's.

// SDK 1.3.1 has no in-process renderer (only audren:u, not served): its calls go to the SDK as made.
static int ar_in_process(void) {
    static int v = -1;
    if (v < 0) {
        v = guest_sym("_ZN2nn5audio29ExecuteAudioRendererRenderingENS0_19AudioRendererHandleE") != NULL;
        if (!v) LOG("[RED] audren: the SDK has no renderer of its own: its client talks to audren:u, which this host does not serve");
    }
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
// Only the render thread and this renderer's updates touch it, besides g_render_mu (docs/reference/audio.md#two-renderers-overwriting-each-others-queued-blocks).
enum { CQ = 4, AR_MAX = 4, RENDER_MIN_NS = 4500000 };   // AR_MAX as g_rev below
typedef struct {
    void *handle[2];                         // handle[0] NULL: a free slot
    uint8_t *sys;
    _Atomic int open;
    void *render_tt;                         // Close joins it before the SDK tears the renderer down
    int16_t frame[FRAME_MAX * DM_AUDIO_SINK_CHANNELS]; int frame_n;   // six channels plus their stereo fold
    _Atomic uint64_t frames, nonsilent, updates, renders, catchups;
    uint64_t last_r; int updated;
    int16_t cq[CQ][FRAME_MAX * DM_AUDIO_SINK_CHANNELS]; int cq_n[CQ]; uint32_t cq_head, cq_tail;   // catch-up frames
    // A woken guest can need cq_mu inside the SDK's signal: take it with dm_host_lock so the waiter counts as blocked.
    pthread_mutex_t cq_mu; pthread_cond_t cq_cv;   // the catch-up queue and fresh renders
    dm_audren_state rs;
    int16_t pcm[NBUF][FRAME_MAX * DM_AUDIO_SINK_CHANNELS];   // AudioOut queues only the pointer and copies when the sound card asks
    aout_buf bufs[NBUF];
} ar_t;
static ar_t g_ar[AR_MAX];
static pthread_mutex_t g_ar_mu = PTHREAD_MUTEX_INITIALIZER;   // the slots' handle and sys fields
static __thread ar_t *t_ar;
static int ar_index(const ar_t *a) { return (int)(a - g_ar); }
// A system not seen yet belongs to the open renderer whose system is unknown: updates can come before the first render.
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

// docs/reference/audio.md#late-audio-producer-wakeups-despite-fast-rendering: a scheduling budget, not busy time.
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
        while (!nfree) {   // sleeps until the output releases a block
            aout_buf *b = dm_pcmout_released(out);
            if (b) { free_list[nfree++] = b; break; }
            dm_pcmout_wait(out, 10);
            if (!atomic_load(&a->open)) break;
        }
        if (!nfree) break;
        dm_host_lock(&a->cq_mu);
        // docs/reference/audio.md#burst-renders-when-a-device-callback-releases-multiple-blocks
        while (!prefill && a->cq_head == a->cq_tail && atomic_load(&a->open)) {
            uint64_t now = now_ns();
            if (now >= next_exec) break;
            struct timespec delay = { .tv_nsec = (long)(next_exec - now) };
            pthread_cond_timedwait_relative_np(&a->cq_cv, &a->cq_mu, &delay);
        }
        if (!atomic_load(&a->open)) { pthread_mutex_unlock(&a->cq_mu); break; }

        const int16_t *src; int got;
        int queued = a->cq_head != a->cq_tail;
        if (queued) {   // recheck after waiting
            uint32_t slot = a->cq_head % CQ;
            src = a->cq[slot]; got = a->cq_n[slot];
        } else {
            next_exec = now_ns() + RENDER_MIN_NS;
            a->frame_n = 0;
            exec(a->handle[0], a->handle[1]);   // generate commands → CPU processing → signal the renderer event
            src = a->frame; got = a->frame_n;
        }
        int n = got ? got : 240;
        aout_buf *b = free_list[--nfree];
        size_t bytes = (size_t)n * DM_AUDIO_SINK_CHANNELS * sizeof(int16_t);
        if (got) memcpy(b->ptr, src, bytes); else memset(b->ptr, 0, bytes);
        b->size = bytes;
        if (queued) { a->cq_head++; pthread_cond_broadcast(&a->cq_cv); }
        pthread_mutex_unlock(&a->cq_mu);
        if (!dm_pcmout_append(out, b)) free_list[nfree++] = b;
        else if (prefill) prefill--;
    }
    if (rt) render_rt_end();
    dm_pcmout_close(out);   // else the output keeps its callback and an AudioOut slot after the close
}

// docs/reference/audio.md#the-sdks-open-with-an-event-calls-the-open-without-one-t_in_open: the nested open starts no thread.
static __thread int t_in_open;
// The renderer's own event, lent to the SDK until Close: the game's object need not outlive the renderer
// (docs/reference/audio.md#the-renderer-event-belongs-to-the-renderer-not-the-game).
static struct { void *obj; uint8_t ev[0x80] __attribute__((aligned(16))); } g_rev[4];   // obj: the owning renderer (handle[0]), NULL when free
static pthread_mutex_t g_rev_mu = PTHREAD_MUTEX_INITIALIZER;
static void *rev_take(void) {
    pthread_mutex_lock(&g_rev_mu); void *ev = NULL;
    for (int i = 0; i < 4 && !ev; i++) if (!g_rev[i].obj) {
        g_rev[i].obj = (void *)1;
        memset(g_rev[i].ev, 0, sizeof g_rev[i].ev);
        ev = g_rev[i].ev;
    }
    pthread_mutex_unlock(&g_rev_mu); return ev;
}
static void rev_set(void *ev, void *obj) {
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
    // The caller's buffer follows the DSP service size; the larger CPU backend storage is supplied here when it does not fit.
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
    // A game opening a CPU renderer itself keeps the SDK's event, as on Horizon.
    void *sev = ev && *(int32_t *)(param + P_DEVICE) == 0 ? rev_take() : NULL;
    if (ev && *(int32_t *)(param + P_DEVICE) == 0 && !sev) LOG("[RED] audren: all renderer events in use: the SDK keeps the game's event, which must then outlive the renderer");
    t_in_open = 1;
    Result r = ev ? orig_ev(handle, sev ? sev : ev, p, work, sz) : orig(handle, p, work, sz);
    t_in_open = 0;
    if (r) {
        free(owned);
        rev_set(sev, NULL);
        LOG("[RED] audren: SDK OpenAudioRenderer failed %#x", r);
        return r;
    }
    if (owned) objmap_put(handle[0], owned);   // freed by Close after the render thread and the SDK are done
    // A local auto-clear event wakes only its highest-priority waiter: the renderer's event is a kernel event, and the game's object gets a copy of its
    // readable handle (docs/reference/audio.md#the-renderer-event-as-the-service-gives-it-a-kernel-event).
    if (ev && *(int32_t *)(param + P_DEVICE) == 0) {
        static void (*destroy)(void *); if (!destroy) destroy = gsym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        if (sev) {
            static void (*attach)(void *, uint32_t, int, int); static uint32_t (*getr)(const void *);
            if (!attach) { attach = gsym("_ZN2nn2os33AttachReadableHandleToSystemEventEPNS0_15SystemEventTypeEjbNS0_14EventClearModeE");
                           getr = gsym("_ZN2nn2os30GetReadableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
            destroy(sev); hle_bind_system_event(sev, 1);
            attach(ev, hle_dup_readable_handle(getr(sev)), 1, 1);   // managed, AutoClear
            rev_set(sev, handle[0]);
        } else { destroy(ev); hle_bind_system_event(ev, 1); }   // no free renderer event: needs the game's object to live
    }
    module_t *m = module_of(ra);
    pthread_mutex_lock(&g_ar_mu);
    ar_t *a = NULL;
    for (int i = 0; i < AR_MAX && !a; i++) if (!g_ar[i].handle[0]) a = &g_ar[i];
    if (a) {   // a closed renderer's slot; memset leaves no lock held
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
    // Join the render thread first: it may be inside exec() with the renderer the SDK is about to free.
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
    if (a) {
        pthread_mutex_lock(&g_ar_mu);
        a->handle[0] = NULL;
        a->sys = NULL;
        pthread_mutex_unlock(&g_ar_mu);
    }
    if (sev) {   // the SDK's Close leaves the event; the game's copied read end stays the game's
        static void (*destroy)(void *); if (!destroy) destroy = gsym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
        destroy(sev); rev_set(sev, NULL);
    }
}

// Reset: render threads leave as at Close and end at ExitThread's gate; the game's Close never comes.
void dm_audren_reset_stop(void) {
    for (int i = 0; i < AR_MAX; i++) {
        ar_t *a = &g_ar[i];
        if (!a->handle[0] || !a->render_tt) continue;
        pthread_mutex_lock(&a->cq_mu);
        atomic_store(&a->open, 0);
        pthread_cond_broadcast(&a->cq_cv);
        pthread_mutex_unlock(&a->cq_mu);
    }
}

// docs/reference/audio.md#a-renderer-below-48-khz-with-no-upsampler-on-the-cpu: "none" for a null manager; the host resamples 3:2.
HLE_FORCE("_ZN2nn5audio6server16UpsamplerManager8AllocateEv", "upsampler for a DeviceSink below 48 kHz: none with CPU rendering (no manager), the host resamples its output",
          void *, h_up_Allocate, void *mgr) {
    static void *(*orig)(void *); if (!orig) orig = gsym("_ZN2nn5audio6server16UpsamplerManager8AllocateEv");
    return mgr ? orig(mgr) : NULL;
}

// g_scan: capture's state for all renderers, under g_render_mu.
static dm_audren_scan g_scan = DM_AUDREN_SCAN_INIT;
static int capture(ar_t *a, const uint8_t *cl, size_t len, int16_t *dst) {
    int loud = 0, n = dm_audren_capture(&g_scan, &a->rs, cl, len, dst, FRAME_MAX * DM_AUDIO_SINK_CHANNELS, &loud);
    if (n < 0) return 0;
    atomic_fetch_add(&a->frames, 1); if (loud) atomic_fetch_add(&a->nonsilent, 1);
    uint64_t f = atomic_load(&a->frames);
    if (f == 200 || f % 12000 == 0) LOG("audren: renderer %d rendered %llu frames, %llu of them not silent", ar_index(a), (unsigned long long)f, (unsigned long long)atomic_load(&a->nonsilent));
    return n;
}
// One lock over all renderers: the two callers share one command buffer, and capture's state is shared.
static pthread_mutex_t g_render_mu = PTHREAD_MUTEX_INITIALIZER;
// SDK 20+: +0x48 rendering device, +0x4c execution mode (both 1), +0xf8 / +0x100 command buffer start / length; GenerateCommand returns a Span in x0 / x1.
typedef struct { uint8_t *p; size_t n; } span_t;
// The update lock's offset moves between SDKs: read it off Update / UpdateImpl (docs/reference/audio.md#the-update-lock-read-off-updateimpl).
static void *plt_target(const uint32_t *pc) {   // adrp / ldr / br x17 → the GOT slot's function; else the address
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
// no readable lock in either: *off stays the caller's guess (a red line)
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
    if (md != 1 || dv != 1) return 0x40299;
    pthread_mutex_lock(&g_render_mu);
    uint64_t t0 = now_ns();
    // Generation excludes the game's RequestUpdate, as the original's SendCommandToDsp does; processing stays outside the lock.
    lock(sys + mu_off);
    span_t sp = gen(sys, *(uint8_t **)(sys + 0xf8), *(size_t *)(sys + 0x100));
    uint8_t *cl = sp.p;
    size_t n = sp.n;
    unlock(sys + mu_off);
    atomic_fetch_add(&a->renders, 1);
    uint64_t proc[16] __attribute__((aligned(16))) = {0};   // CommandListProcessor (0x60 bytes in SDK 21)
    tmax(proc, 0xffffffffu); setup(proc, cl, n); process(proc);
    if (dst) *nout = capture(a, cl, n, dst);
    static uint64_t sum, mx, cnt; uint64_t d = now_ns() - t0; sum += d; if (d > mx) mx = d;
    if (++cnt == 2000) {
        LOG("audren: render time mean %.2f ms / max %.2f ms (5 ms budget per frame, last 2000 frames); catch-up renders so far %llu", sum / 2000 / 1e6, mx / 1e6, (unsigned long long)atomic_load(&a->catchups));
        sum = mx = cnt = 0;
    }
    pthread_mutex_unlock(&g_render_mu);
    return RES_OK;
}
// A game executing its own CPU renderer renders into a frame nothing plays.
static Result ar_exec(uint8_t *sys) {
    ar_t *a = t_ar;
    if (a && !a->sys) { pthread_mutex_lock(&g_ar_mu); a->sys = sys; pthread_mutex_unlock(&g_ar_mu); }
    if (!a) a = ar_of_sys(sys);
    if (!a) { LOG("[RED] audren: a render of a renderer the host did not open"); return RES_FAIL; }
    return render_once(a, sys, a->frame, &a->frame_n);
}
// docs/reference/audio.md#the-render-threads-route-through-the-public-executeaudiorendererrendering-sdk-20
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem29ExecuteAudioRendererRenderingEv", "render one frame manually (reached through the PLT): SDK generates commands + CPU processing under the host's lock, the host takes the DeviceSink output",
          Result, h_ars_Exec, uint8_t *sys) { return ar_exec(sys); }

// At least one frame between two updates (docs/reference/audio.md#at-least-one-rendered-frame-between-two-updates): else render a catch-up frame,
// which the render thread plays in place of its next render. A full queue waits instead of discarding (#a-sound-driver-that-never-waits-on-the-renderer-event).
HLE_FORCE("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm", "renderer update: if nothing was rendered since the last update, render a catch-up frame first (so effects get initialized), then hand over to the SDK",
          Result, h_ars_Update, uint8_t *sys, void *o, size_t on, void *p, size_t pn, const void *in, size_t inn) {
    static Result (*orig)(void *, void *, size_t, void *, size_t, const void *, size_t);
    if (!orig) orig = gsym("_ZN2nn5audio6server17AudioRenderSystem6UpdateEPvmS3_mPKvm");
    ar_t *a = ar_of_sys(sys);
    if (!a) return orig(sys, o, on, p, pn, in, inn);
    uint64_t u = atomic_fetch_add(&a->updates, 1) + 1;
    dm_host_lock(&a->cq_mu);
    if (atomic_load(&a->open) && a->updated && atomic_load(&a->renders) == a->last_r) {
        uint64_t c = atomic_fetch_add(&a->catchups, 1) + 1;
        if (c <= 3) LOG("audren: renderer %d: no frame rendered before update %llu, rendering a catch-up frame first (%llu so far)", ar_index(a), (unsigned long long)u, (unsigned long long)c);
        if (a->cq_tail - a->cq_head >= CQ) {
            struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 50000000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
            while (atomic_load(&a->open) && a->cq_tail - a->cq_head >= CQ && !pthread_cond_timedwait(&a->cq_cv, &a->cq_mu, &dl)) {}
        }
        if (atomic_load(&a->open)) {
            if (a->cq_tail - a->cq_head < CQ) {
                uint32_t slot = a->cq_tail % CQ;
                render_once(a, sys, a->cq[slot], &a->cq_n[slot]);
                a->cq_tail++;
                pthread_cond_broadcast(&a->cq_cv);
            } else render_once(a, sys, NULL, NULL);   // the output stopped: render, output discarded
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

// One logical output with the current route's channels.
enum { DEVNAME = 0x100 };
static void out_devname(char *out) { memset(out, 0, DEVNAME); strlcpy(out, "AudioBuiltInSpeakerOutput", DEVNAME); }
HLE_FORCE("_ZN2nn5audio25ListAudioOutputDeviceNameEPNS0_15AudioDeviceNameEi", "output device list: 1 built-in speaker", int, h_ad_List, char *names, int n) { if (n <= 0 || !names) return 0; out_devname(names); return 1; }
HLE_FORCE("_ZN2nn5audio30GetActiveAudioOutputDeviceNameEPNS0_15AudioDeviceNameE", "active output device: built-in speaker", void, h_ad_Active, char *name) { out_devname(name); }
HLE_FORCE("_ZN2nn5audio41GetActiveAudioDeviceChannelCountForOutputEv", "output channel count: the route's (6 on a six-channel device, else 2)", int, h_ad_Channels, void) { return dm_audio_route(); }
// kept, not applied: the system volume applies
static _Atomic float g_dev_vol = 1.0f;
HLE_FORCE("_ZN2nn5audio26SetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameEf", "device volume; kept for the getter, not applied by the host", void, h_ad_SetVol, const char *name, float v) { (void)name; atomic_store(&g_dev_vol, v); }
HLE_FORCE("_ZN2nn5audio26GetAudioDeviceOutputVolumeEPKNS0_15AudioDeviceNameE", "device volume: what the game last set (1.0 at start)", float, h_ad_GetVol, const char *name) { (void)name; return atomic_load(&g_dev_vol); }
// kept and read back, off at start
static atomic_uchar g_dev_autotune;
HLE_FORCE("_ZN2nn5audio40SetDeviceOutputVolumeAutoTuneModeEnabledEb", "device volume auto-tune; kept for the getter, not applied by the host", void, h_ad_SetAutoTune, unsigned on) { atomic_store(&g_dev_autotune, on & 1); }
HLE_FORCE("_ZN2nn5audio39IsDeviceOutputVolumeAutoTuneModeEnabledEv", "device volume auto-tune: what the game last set (off at start)", unsigned, h_ad_AutoTune, void) { return atomic_load(&g_dev_autotune); }
// Signaled on a route change. 0, or 0x499 when already acquired (the pre-21 SDKs' per-process flag); unserved, aud:u would abort.
enum { AD_FREE, AD_BOUND, AD_CHANGING };
static atomic_int g_ad_notify;
static _Atomic uint64_t g_ad_binding;
void dm_audio_device_changed(void) { hle_signal_system_event_binding(atomic_load(&g_ad_binding)); }
HLE_FORCE("_ZN2nn5audio36AcquireAudioOutputDeviceNotificationEPNS_2os11SystemEventE", "output device change notification (SDK 21 name): follows the host route", Result, h_ad_Notify21, void *ev) {
    int expected = AD_FREE;
    if (!atomic_compare_exchange_strong(&g_ad_notify, &expected, AD_CHANGING)) return 0x499;
    dm_audio_route();   // watch the route before publishing the subscriber
    hle_bind_system_event(ev, 0);
    atomic_store(&g_ad_binding, hle_system_event_binding(ev));
    atomic_store(&g_ad_notify, AD_BOUND);
    return RES_OK;
}
// The SDK's would abort on aud:u. Destroying the bound event drops the binding; the flag is cleared for a later acquire.
HLE_FORCE("_ZN2nn5audio36ReleaseAudioOutputDeviceNotificationEPNS_2os11SystemEventE", "release the output device change notification (SDK 21 name): the event is destroyed",
          void, h_ad_NotifyRelease21, void *ev) {
    int expected = AD_BOUND;
    if (!atomic_compare_exchange_strong(&g_ad_notify, &expected, AD_CHANGING)) return;
    // Keeps another acquire out until the destruction completes.
    atomic_store(&g_ad_binding, 0);
    static void (*destroy)(void *); if (!destroy) destroy = gsym("_ZN2nn2os18DestroySystemEventEPNS0_15SystemEventTypeE");
    destroy(ev);
    atomic_store(&g_ad_notify, AD_FREE);
}

// Reset, after every guest thread ended and before the game's memory goes: frees the host's work buffers, thread records and lent events.
void dm_audren_reset(void) {
    int n = 0;
    pthread_mutex_lock(&g_ar_mu);
    for (int i = 0; i < AR_MAX; i++) {
        ar_t *a = &g_ar[i];
        if (!a->handle[0]) continue;
        free(objmap_del(a->handle[0])); free(a->render_tt);   // its stack was the SDK's malloc
        a->render_tt = NULL; a->handle[0] = a->handle[1] = NULL; a->sys = NULL; n++;
    }
    pthread_mutex_unlock(&g_ar_mu);
    pthread_mutex_lock(&g_rev_mu); for (int i = 0; i < 4; i++) { g_rev[i].obj = NULL; memset(g_rev[i].ev, 0, sizeof g_rev[i].ev); } pthread_mutex_unlock(&g_rev_mu);
    pthread_mutex_lock(&g_render_mu);
    g_scan = (dm_audren_scan)DM_AUDREN_SCAN_INIT;
    pthread_mutex_unlock(&g_render_mu);
    atomic_store(&g_dev_vol, 1.0f); atomic_store(&g_dev_autotune, 0);
    atomic_store(&g_ad_binding, 0); atomic_store(&g_ad_notify, AD_FREE);   // else a second Acquire would be refused (0x499)
    LOG("reset: audio renderer: %d renderers forgotten", n);
}
