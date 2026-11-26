// Horizon's thread and synchronization SVCs, natively: one host pthread per guest thread, on the guest stack the SDK gives; nn::os runs as is.
// Semantics follow mesosphere (KConditionVariable, KAddressArbiter, KSynchronization). All waits hang on one global lock G, one condvar per thread.
#include <unistd.h>
#include "dm.h"
#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <sys/mman.h>
#include <time.h>
#include <mach/mach.h>
#include <pthread/qos.h>
#include <dlfcn.h>
#include <stdatomic.h>

#define R_TIMEOUT     0xEA01u   // module 1, description 117
#define R_CANCELLED   0xEC01u   // 118
#define R_OUT_OF_RANGE 0xEE01u  // 119
#define R_BAD_HANDLE  0xE401u   // 114
#define R_BAD_STATE   0xFA01u   // 125
#define WAITERS_BIT   0x40000000u
#define H_CUR_THREAD  0xFFFF8000u

enum { O_FREE, O_THREAD, O_EVENT_R, O_EVENT_W, O_TRANSFER_MEMORY };
enum { MAX_SYNC_OBJECTS = 64 };
typedef struct { void *obj; int type; } sync_ref_t;
// SystemEvent bindings: guest SystemEventType -> its kernel event's write end; an entry lives as long as that handle (CloseHandle unbinds).
// Lock order: G, then g_se_lock.
static struct { void *ev; uint32_t wh; uint64_t id; } g_se[512];
static int g_nse;
static uint64_t g_next_se_id = 1;   // binding identity survives neither address nor handle reuse
static pthread_mutex_t g_se_lock = PTHREAD_MUTEX_INITIALIZER;
static uint32_t se_find(void *ev) {   // g_se_lock held
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) return g_se[i].wh;
    return 0;
}
static void se_unbind(uint32_t wh) {   // G held
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; ) if (g_se[i].wh == wh) g_se[i] = g_se[--g_nse]; else i++;
    pthread_mutex_unlock(&g_se_lock);
}
typedef struct event { int signaled; int refs; } event_t;
typedef struct thr {
    uint32_t handle; uint64_t id;
    uint64_t entry, arg, stack_top; int prio, core; uint64_t mask;
    uint64_t born_ns, exit_ns;
    int started, entered, exited, cancel;   // entered: dispatched, going on to guest code
    pthread_t pt; pthread_cond_t cv;
    // wkind: 0 none, 1 lock, 2 condition, 3 address, 4 synchronization object
    int wkind, woke; uint64_t waddr, wkey; uint32_t wtag; int64_t wres;
    _Atomic int sleeping;      // in SleepThread's timed sleep: not runnable
    int yield_pending;         // woke a higher-priority thread pinned to its core: hand off before returning to guest code
    _Atomic uint64_t hand_epoch;   // threads it makes runnable belong to this handoff until its next thread SVC returns
    struct thr *start_by; uint64_t start_epoch;   // the lower-priority same-core starter whose StartThread waits for this thread, and that handoff
    pthread_cond_t hcv; int hwait, hcv_init;   // initialized on first use (G held)
    struct { struct thr *by; uint64_t epoch; } hand[4]; int nhand;   // the handoffs this thread was made runnable for, until it blocks
    _Atomic int *host_work;
    int native_wait;          // G held
    int host_owned;            // made by the host (dm_guest_thread_spawn): waits in host code, not one of the game's threads
    uint64_t wseq;             // at equal priority first come, first woken, as KThread's wait tree
    struct thr *next;
    struct thr *all;
    int last_svc; uint64_t last_lr;
    int closed, refs;           // freed only after handle close, exit and the last reference (thr_put)
    uint64_t nsvc;
    uint64_t nyield, nyield_seen, nbackoff, nbackoff_seen;   // *_seen: the counts at the last power report
    uint64_t nclkyield, nclkyield_seen;
    uint64_t nspin, nspin_seen, spin_site; const char *spin_mod;   // spin wait passes (those that yield count in nyield too)
    uint64_t srun_svc, srun_t0, spin_ret;
    uint64_t cpu_seen_us;                     // µs
    uint64_t yrun_svc, yrun_yield, yrun_t0, yrun_sig, yrun_sig2, poll_ret;   // a run of steps lasts while no other SVC comes between and the signature matches one of the last two (thr_yield); yrun_yield: nsvc + 1 at the last yield
    uint64_t yrun_clk;                        // a pass that read the clock since makes the yield a clock wait
    uint64_t cw_sig, cw_run, cw_last, cw_end, cw_per, cw_per2, cw_wake, ncwpred, ncwpred_seen;   // the clock wait followed (clock_wait), by its x19-x28
    int cw_ok, cw_pred, cw_after, cw_trip;
    mach_port_t mport;         // for CPU time
    _Atomic int qos_pending;   // only a thread can set its own QoS: applied at its next SVC
    uint32_t qos;
    // Pausing (SetThreadActivity, garbage collectors): inside an SVC the context is the gate frame (svc_regs), in guest code the signal handler saves it.
    _Atomic int pause_req; int paused;
    uint64_t *_Atomic svc_regs; uint64_t svc_pc, svc_x18;   // svc_x18: the guest's x18 at SVC entry (the gate frame holds Apple's platform register)
    // bit 0 inside an SVC, bit 1 pause requested: the exit takes G only when bit 1 is set, so a thread counted stopped in an SVC cannot
    // leave it without G (svc_regs stays valid)
    _Atomic int svc_state;
    sync_ref_t wobjs[MAX_SYNC_OBJECTS]; int wn, widx;   // retained objects (KThread's sync object buffer): independent of handle reuse and guest memory
    int ctx_ok;
    uint64_t ctx_x[29], ctx_fp, ctx_lr, ctx_sp, ctx_pc;
    uint32_t ctx_pstate, ctx_fpcr, ctx_fpsr;
    __uint128_t ctx_v[32];
    pthread_cond_t pcv;
    _Atomic int sig_pend;   // wakes signalled after G is let go and not yet done: thr_free waits for them
} thr_t;
static thr_t *g_all;
extern __thread _Atomic int dm_host_work;
static _Atomic uint64_t g_preempt_n, g_preempt_ns;
static int g_core_preempt;   // also check already-runnable peers after every thread SVC (DM_PREEMPT)

static pthread_mutex_t G = PTHREAD_MUTEX_INITIALIZER;
// G only through g_lock, g_unlock, g_cond_wait. Wakes are signalled with G let go (else the woken thread sleeps again on G): queued
// in t_wdef, signalled after the unlock. DM_WAKE_UNLOCKED=0 signals under G.
enum { WDEF = 8 };
static __thread struct thr *t_wdef[WDEF];
static __thread int t_nwdef;
static int g_wake_unlocked = 1;
static inline void wake_signal(struct thr *t) { pthread_cond_signal(&t->cv); atomic_fetch_sub_explicit(&t->sig_pend, 1, memory_order_release); }
static inline void g_lock(void) {
    pthread_mutex_lock(&G);
    if (t_nwdef) die("thread lock: %d wakes still queued from an earlier hold (G let go without g_unlock)", t_nwdef);
}
static inline void g_unlock(void) {
    int n = t_nwdef; struct thr *d[WDEF];
    for (int i = 0; i < n; i++) d[i] = t_wdef[i];
    t_nwdef = 0;
    pthread_mutex_unlock(&G);
    for (int i = 0; i < n; i++) wake_signal(d[i]);
}
// dl: CLOCK_REALTIME deadline, NULL none; may return early, callers recheck
static inline int g_cond_wait(pthread_cond_t *cv, const struct timespec *dl) {
    if (t_nwdef) { g_unlock(); g_lock(); return 0; }   // this hold's wakes first, with G let go: a spurious wakeup
    return dl ? pthread_cond_timedwait(cv, &G, dl) : pthread_cond_wait(cv, &G);
}
#define MAX_H 4096
static struct { int type; void *obj; } g_h[MAX_H];
static pthread_key_t g_keythr;
static uint64_t g_next_id = 0x51;
static thr_t *g_wait;

static uint32_t h_try_new(int type, void *obj) {     // G held
    for (int i = 1; i < MAX_H; i++) if (!g_h[i].type) {
        g_h[i].type = type;
        g_h[i].obj = obj;
        return 0x1000 + i;
    }
    return 0;
}
static uint32_t h_new(int type, void *obj) {     // G held
    uint32_t h = h_try_new(type, obj);
    if (!h) die("handle table full");
    return h;
}
static void *h_peek(uint32_t h, int type) {      // no report
    if (h == H_CUR_THREAD && type == O_THREAD) return pthread_getspecific(g_keythr);
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H || g_h[i].type != type) return NULL;
    return g_h[i].obj;
}
static void *h_get(uint32_t h, int type) {       // reports a miss (a wrong handle is a real error)
    void *o = h_peek(h, type);
    if (!o) LOG("invalid handle %#x (wanted type %d)", h, type);
    return o;
}
static thr_t *cur(void) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) die("thread SVC called from a non-guest thread");
    return t;
}
// A woken higher-priority thread pinned to the caller's core runs until it blocks (Horizon's per-core order: docs/reference/threads.md,
// "Same-core wake handoffs"). Wait for guest blocking, not a dispatch snapshot or a deadline.
void dm_thread_mark_host(void) { thr_t *t = pthread_getspecific(g_keythr); if (t) t->host_owned = 1; }
// G held. wake() clears wkind before the pthread resumes: it counts as runnable already. Host GPU work and contended native locks occupy no guest core.
static int thr_runnable(const thr_t *t) {
    return !t->wkind && !t->sleeping && !t->native_wait && !t->pause_req && !(t->host_work && *t->host_work);
}
// G held
static int thr_in_handoff(const thr_t *t, const thr_t *self) {
    for (int i = 0; i < t->nhand; i++) if (t->hand[i].by == self && t->hand[i].epoch == self->hand_epoch) return 1;
    return 0;
}
// G held. g_hand_cv: core_preempt waiters, which wait for every runnable higher-priority peer of their core
static pthread_cond_t g_hand_cv = PTHREAD_COND_INITIALIZER;
static int g_hand_waiters;
static _Atomic uint64_t g_hand_timeouts;
static void hand_signal(thr_t *t) {
    for (int i = 0; i < t->nhand; i++) {
        thr_t *by = t->hand[i].by;
        if (by->hwait && by->hand_epoch == t->hand[i].epoch) pthread_cond_signal(&by->hcv);
    }
    if (g_hand_waiters) pthread_cond_broadcast(&g_hand_cv);
}
// G held: a block gives the core back, so it no longer holds the handoffs that made it runnable
static void hand_leave(thr_t *t) { hand_signal(t); t->nhand = 0; t->start_by = NULL; }
// A file system call is an IPC request on Horizon: the caller blocks and a lower-priority thread of its core runs. The host answers in
// the caller's thread, so a started thread's first request waits (bounded) until its starter is back in guest code.
#define SERVICE_START_WAIT_NS 5000000
#define SERVICE_ROUND_TRIP_NS 200000
void dm_thread_service_request(void) {
    thr_t *me = pthread_getspecific(g_keythr);
    if (!me) return;
    g_lock();
    if (!me->nhand) { g_unlock(); return; }
    thr_t *by = me->start_by; uint64_t epoch = me->start_epoch;
    hand_leave(me);
    me->start_by = by;   // thr_free clears it if the starter goes away
    me->native_wait++;   // not runnable meanwhile
    uint64_t t0 = by ? now_ns() : 0; int back = 0;
    while (me->start_by && now_ns() - t0 < SERVICE_START_WAIT_NS) {
        if (atomic_load(&me->start_by->hand_epoch) != epoch) { back = 1; break; }
        g_unlock();
        { struct timespec ts = {0, 50000}; nanosleep(&ts, 0); }
        g_lock();
    }
    me->start_by = NULL; me->native_wait--;
    g_unlock();
    if (back) { struct timespec ts = {0, SERVICE_ROUND_TRIP_NS}; nanosleep(&ts, 0); }
}
// Called with the HLE's mutex held: the waiter cannot miss a signal between publishing its blocked state and pthread_cond_wait.
void dm_thread_native_wait(int entering) {
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t) return;
    g_lock();
    if (entering) { t->native_wait++; hand_leave(t); }
    else { if (!t->native_wait) die("unbalanced native HLE wait"); t->native_wait--; }
    g_unlock();
}

// A handoff waits only for the peers it made runnable (all = 1: every runnable higher-priority peer), and not when the caller's mask allows
// another core (docs/reference/threads.md#a-same-core-handoff-holds-only-a-waker-pinned-to-that-core). The 50 µs timeout rechecks unsignalled changes.
#define HANDOFF_BACKSTOP_NS 50000
int dm_yield_to_core_peers(int all) {
    thr_t *self = pthread_getspecific(g_keythr); if (!self) return 0;
    uint64_t bit = 1ull << (__atomic_load_n(&self->core, __ATOMIC_RELAXED) & 63); int waited = 0;   // read without G, as GetCurrentProcessorNumber does
    if (__atomic_load_n(&self->mask, __ATOMIC_RELAXED) & ~bit) return 0;
    g_lock();
    for (;;) {
        int busy = 0;
        for (thr_t *t = g_all; t && !busy; t = t->all)
            if (t != self && t->started && !t->exited && !t->host_owned && t->mask == bit && t->prio < self->prio && thr_runnable(t) && (all || thr_in_handoff(t, self))) busy = 1;
        if (!busy) break;
        waited = 1;
        struct timespec dl;
        clock_gettime(CLOCK_REALTIME, &dl);
        dl.tv_nsec += HANDOFF_BACKSTOP_NS;
        if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
        int rc;
        if (all) {
            g_hand_waiters++;
            rc = g_cond_wait(&g_hand_cv, &dl);
            g_hand_waiters--;
        } else {
            if (!self->hcv_init) {
                rc = pthread_cond_init(&self->hcv, NULL);
                if (rc) die("handoff condition variable: %s", strerror(rc));
                self->hcv_init = 1;
            }
            self->hwait = 1;
            rc = g_cond_wait(&self->hcv, &dl);
            self->hwait = 0;
        }
        if (rc == ETIMEDOUT) atomic_fetch_add_explicit(&g_hand_timeouts, 1, memory_order_relaxed);
    }
    g_unlock();
    return waited;
}
// A guest thread waiting for a lock an HLE call holds across guest code is in a native wait, so it holds no handoff
// (docs/reference/threads.md#a-host-lock-held-across-a-same-core-handoff)
void dm_host_lock(pthread_mutex_t *m) {
    if (!pthread_mutex_trylock(m)) return;
    dm_thread_native_wait(1);
    pthread_mutex_lock(m);
    dm_thread_native_wait(0);
}

static void wait_unlink(thr_t *t) {
    for (thr_t **p = &g_wait; *p; p = &(*p)->next) if (*p == t) { *p = t->next; break; }
    t->next = NULL;
}
// Kernel order: lower priority value first, then earlier registered (G held). kind 1 by lock address, 2 by condition key, 3 by address
static uint64_t g_wseq;
static thr_t *pick(int kind, uint64_t a) {
    thr_t *b = NULL;
    for (thr_t *t = g_wait; t; t = t->next) {
        if (t->wkind != kind || (kind == 2 ? t->wkey : t->waddr) != a) continue;
        if (!b || t->prio < b->prio || (t->prio == b->prio && t->wseq < b->wseq)) b = t;
    }
    return b;
}
// G held. t joins the waker's handoff and those the waker is part of; a wake of an equal-priority thread is no handoff of its own
static void note_runnable(thr_t *t) {
    thr_t *self = pthread_getspecific(g_keythr);
    t->nhand = 0;
    if (!self || self == t || t->host_owned) return;
    t->hand[t->nhand].by = self; t->hand[t->nhand++].epoch = self->hand_epoch;
    for (int i = 0; i < self->nhand && t->nhand < 4; i++)
        if (self->hand[i].by != t && self->hand[i].by->hand_epoch == self->hand[i].epoch) t->hand[t->nhand++] = self->hand[i];
    if (t->prio < self->prio && t->mask == 1ull << (self->core & 63) && !(self->mask & ~t->mask)) self->yield_pending = 1;   // a waker that can run on another core is not held
}
// G held; signalled now when the list is full
static void wake_queue(thr_t *t) {
    if (g_wake_unlocked && t_nwdef < WDEF) { atomic_fetch_add_explicit(&t->sig_pend, 1, memory_order_relaxed); t_wdef[t_nwdef++] = t; }
    else pthread_cond_signal(&t->cv);
}
static void wake(thr_t *t, int64_t res) {
    wait_unlink(t); t->wkind = 0; t->woke = 1; t->wres = res; wake_queue(t);
    note_runnable(t);
}
// a wait in guest code that host code called goes on: the host code above may hold a lock
static int thr_reset_can_stop(thr_t *t) {
    const uint64_t *r = atomic_load(&t->svc_regs);
    return r && !t->host_owned && dm_reset_gate_can_end(r, t->handle == 0x1001 ? 2 : 1);
}
// ns < 0 infinite, 0 immediate; returns wres, R_TIMEOUT, or R_CANCELLED on a reset. Host time even in replay: the wait may hold up the present that advances the guest clock.
static int64_t block(thr_t *t, int kind, int64_t ns) {
    t->wkind = kind; t->woke = 0; t->next = g_wait; g_wait = t; t->wseq = ++g_wseq;
    if (ns == 0) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    hand_leave(t);
    struct timespec dl;
    if (ns > 0) { clock_gettime(CLOCK_REALTIME, &dl); dl.tv_sec += ns / 1000000000; dl.tv_nsec += ns % 1000000000;
                  if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; } }
    while (!t->woke) {
        if (atomic_load_explicit(&dm_reset_stop, memory_order_relaxed) && thr_reset_can_stop(t)) {
            wait_unlink(t);
            t->wkind = 0;
            return R_CANCELLED;
        }
        int rc = g_cond_wait(&t->cv, ns > 0 ? &dl : NULL);
        if (rc == ETIMEDOUT && !t->woke) { wait_unlink(t); t->wkind = 0; return R_TIMEOUT; }
    }
    return t->wres;
}

static inline uint32_t ld32(uint64_t a) { return __atomic_load_n((uint32_t *)a, __ATOMIC_SEQ_CST); }
static inline uint64_t ld64(uint64_t a) { return __atomic_load_n((uint64_t *)a, __ATOMIC_SEQ_CST); }
static inline void st32(uint64_t a, uint32_t v) { __atomic_store_n((uint32_t *)a, v, __ATOMIC_SEQ_CST); }
static inline int cas32(uint64_t a, uint32_t *exp, uint32_t v) { return __atomic_compare_exchange_n((uint32_t *)a, exp, v, 0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); }

// G held
static void lock_handoff(uint64_t addr) {
    thr_t *w = pick(1, addr); int more = 0;
    if (!w) { st32(addr, 0); return; }
    for (thr_t *t = g_wait; t; t = t->next) if (t != w && t->wkind == 1 && t->waddr == addr) { more = 1; break; }
    st32(addr, w->wtag | (more ? WAITERS_BIT : 0));
    wake(w, 0);
}
// G held
static void lock_acquire_for(thr_t *t, uint64_t addr, uint32_t tag) {
    uint32_t v = ld32(addr);
    for (;;) {
        if (v == 0) { if (cas32(addr, &v, tag)) { wake(t, 0); return; } continue; }
        if (cas32(addr, &v, v | WAITERS_BIT)) break;
    }
    t->wkind = 1; t->waddr = addr; t->wtag = tag;
    t->wseq = ++g_wseq;                                  // requeued: at equal priority after the existing lock waiters, as the kernel does
}

extern void dm_thread_enter(uint64_t entry, uint64_t arg, uint64_t sp);
void crash_thread_init(void);
void guest_thread_init(void);   // the single place that sets the TSD slots (x18, TLS, TLR)
void guest_thread_set_id(uint64_t id);
void guest_gate_stack(int own_stack);
// Switch priority -> QoS class (DM_QOS_MAP=1, off by default); otherwise every guest thread is user-interactive. Bands: <= e1 (44, the main
// thread) user-interactive, <= e2 user-initiated, else utility; DM_QOS_EDGES=e1,e2. Read once, before any other guest thread.
static int g_qos_map, g_qos_e1 = 44, g_qos_e2 = 59;
static void qos_map_init(void) {
    const char *e = getenv("DM_QOS_MAP"), *d = getenv("DM_QOS_EDGES"); g_qos_map = e && atoi(e);
    if (d) sscanf(d, "%d,%d", &g_qos_e1, &g_qos_e2);
    if (g_qos_map) LOG("DM_QOS_MAP=1: guest threads get QoS by priority (≤%d interactive, ≤%d user-initiated, the rest utility)", g_qos_e1, g_qos_e2);
}
static qos_class_t prio_qos(int prio) {
    if (!g_qos_map) return QOS_CLASS_USER_INTERACTIVE;
    return prio <= g_qos_e1 ? QOS_CLASS_USER_INTERACTIVE : prio <= g_qos_e2 ? QOS_CLASS_USER_INITIATED : QOS_CLASS_UTILITY;
}
static void qos_apply(thr_t *t) {   // on the thread itself, G held
    qos_class_t q = prio_qos(t->prio);
    if ((uint32_t)q != t->qos && !pthread_set_qos_class_self_np(q, 0)) t->qos = (uint32_t)q;
}
// a host helper inherits its creator guest thread's class; under DM_QOS_MAP it sets its own
void dm_host_helper_qos(qos_class_t q) { if (g_qos_map) pthread_set_qos_class_self_np(q, 0); }
static const char *thr_name(thr_t *t, char *buf, size_t n);
static void thread_exit(thr_t *t);
static void *thread_main(void *p) {
    thr_t *t = p;
    pthread_setspecific(g_keythr, t);
    // CreateThread may already have linked t into g_all
    g_lock();
    pthread_set_qos_class_self_np(prio_qos(t->prio), 0);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    t->mport = pthread_mach_thread_np(pthread_self()); t->host_work = &dm_host_work;
    g_unlock();
    crash_thread_init();
    guest_thread_init(); guest_thread_set_id(t->id);
    guest_gate_stack(1);   // this pthread's stack below this frame is where its SVCs run
    // CloseHandle before StartThread destroys the thread, as on Horizon (closed_unstarted)
    g_lock(); while (!t->started && !t->closed && !atomic_load(&dm_reset_stop)) g_cond_wait(&t->cv, NULL);
    int run = t->started && !atomic_load(&dm_reset_stop); if (run) t->entered = 1; g_unlock();
    if (!run) thread_exit(t);
    { char nb[48], hn[64]; snprintf(hn, sizeof hn, "%#x %s", t->handle, thr_name(t, nb, sizeof nb)); pthread_setname_np(hn); }
    dm_thread_enter(t->entry, t->arg, t->stack_top);   // does not return: the guest exits through ExitThread
    die("guest thread entry returned");
}
static void thr_free(thr_t *t) {   // G held
    for (int i = 0; i < t_nwdef; ) if (t_wdef[i] == t) { atomic_fetch_sub_explicit(&t->sig_pend, 1, memory_order_relaxed); t_wdef[i] = t_wdef[--t_nwdef]; } else i++;
    while (atomic_load_explicit(&t->sig_pend, memory_order_acquire)) sched_yield();   // another waker's signal after its unlock
    for (thr_t **p = &g_all; *p; p = &(*p)->all) if (*p == t) { *p = t->all; break; }
    for (thr_t *u = g_all; u; u = u->all)   // no handoff entry may point at the freed object
    {
        for (int i = 0; i < u->nhand; ) if (u->hand[i].by == t) u->hand[i] = u->hand[--u->nhand]; else i++;
        if (u->start_by == t) u->start_by = NULL;
    }
    pthread_cond_destroy(&t->cv);
    pthread_cond_destroy(&t->pcv);
    if (t->hcv_init) pthread_cond_destroy(&t->hcv);
    free(t);
}
static void thr_put(thr_t *t) {   // G held; the last holder of a closed, exited thread frees it
    if (!--t->refs && t->closed && t->exited) thr_free(t);
}
static void wake_sync_waiters(const void *obj);
static void thread_exit(thr_t *t) {
    t->exit_ns = now_ns();
    // No logging: the exit runs on the guest stack, which can be a dozen KiB; printf with floating point overflows it
    g_lock();
    t->exited = 1;
    hand_signal(t);
    wake_sync_waiters(t);
    pthread_setspecific(g_keythr, NULL);
    if (t->closed && !t->refs) thr_free(t);
    g_unlock();
    pthread_exit(NULL);
}
void dm_thread_returned(void) { die("guest thread entry function returned (Horizon requires ExitThread)"); }
// 0 not a guest thread, 1 worker, 2 main guest thread, 3 the host's own
int dm_thread_reset_role(void) { thr_t *t = pthread_getspecific(g_keythr); return !t ? 0 : t->handle == 0x1001 ? 2 : t->host_owned ? 3 : 1; }
void dm_thread_reset_exit(void) { thread_exit(cur()); }
void dm_thread_reset_main_out(void) {   // it leaves the table as exited; its pthread goes back to the boot loop
    thr_t *t = cur();
    t->exit_ns = now_ns();
    g_lock();
    t->exited = 1;
    hand_signal(t);
    wake_sync_waiters(t);
    pthread_setspecific(g_keythr, NULL);
    g_unlock();
}
void dm_thread_reset_wake(void) {
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        pthread_cond_broadcast(&t->cv);
        pthread_cond_broadcast(&t->pcv);
        if (t->hcv_init) pthread_cond_broadcast(&t->hcv);
    }
    pthread_cond_broadcast(&g_hand_cv);
    g_unlock();
}
void dm_thread_reset_dump(void) {
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->exited || !t->mport) continue;
        char nb[48], line[2048];
        int p = snprintf(line, sizeof line, "[dm] reset: thread %#x '%s' last svc %#x wait kind %d:", t->handle, thr_name(t, nb, sizeof nb), t->last_svc, t->wkind);
        if (thread_suspend(t->mport) == KERN_SUCCESS) {
            arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
            if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
                uint64_t fp = __darwin_arm_thread_state64_get_fp(st), pc = __darwin_arm_thread_state64_get_pc(st), lr = __darwin_arm_thread_state64_get_lr(st);
                for (int k = 0; k < 40 && p < (int)sizeof line - 96; k++) {
                    uint64_t a = k == 0 ? pc : k == 1 ? lr : 0;
                    if (k >= 2) {
                        uint64_t fr[2];
                        vm_size_t g = 0;
                        if (!fp || vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g) || g != 16 || fr[0] <= fp) break;
                        a = fr[1];
                        fp = fr[0];
                    }
                    module_t *m = module_of((void *)a); Dl_info di;
                    if (m) p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, a - (uint64_t)m->base);
                    else if (dladdr((void *)a, &di) && di.dli_sname) p += snprintf(line + p, sizeof line - p, " %s+%#lx", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
                    else p += snprintf(line + p, sizeof line - p, " %#llx", a);
                }
            }
            thread_resume(t->mport);
        }
        fprintf(stderr, "%s\n", line);
    }
    g_unlock();
}
// Thread IDs restart as in a new process; binding numbers keep counting: a host thread signalling a gone binding must not hit a reused number
void dm_thread_reset_tables(void) {
    g_lock();
    int nthr = 0, nev = 0;
    for (thr_t *t = g_all, *n; t; t = n) {
        n = t->all; nthr++;
        while (atomic_load_explicit(&t->sig_pend, memory_order_acquire)) sched_yield();
        pthread_cond_destroy(&t->cv);
        pthread_cond_destroy(&t->pcv);
        if (t->hcv_init) pthread_cond_destroy(&t->hcv);
        free(t);
    }
    g_all = NULL; g_wait = NULL;
    for (int i = 1; i < MAX_H; i++) {
        if (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W) { event_t *e = g_h[i].obj; if (--e->refs == 0) { free(e); nev++; } }
        g_h[i].type = O_FREE; g_h[i].obj = NULL;
    }
    g_next_id = 0x51;
    pthread_mutex_lock(&g_se_lock); int nse = g_nse; g_nse = 0; pthread_mutex_unlock(&g_se_lock);
    g_unlock();
    LOG("reset: kernel tables emptied (%d thread objects, %d events, %d system event bindings)", nthr, nev, nse);
}
int dm_thread_reset_live(int *main_live) {
    int n = 0; *main_live = 0;
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) if (!t->exited) { n++; if (t->handle == 0x1001) *main_live = 1; }
    g_unlock();
    return n;
}

// A garbage collector stops the world: threads really stop, or it misses pointers held only in registers (docs/reference/threads.md#pausing-threads-and-reading-their-context-for-garbage-collectors).
// paused acknowledges the current request only: the resume clears it, and the waiter sets it again on every wakeup.
module_t *module_of(const void *p);
static void on_pause_sig(int sig, siginfo_t *si, void *uc_) {
    (void)sig; (void)si;
    thr_t *t = pthread_getspecific(g_keythr);
    if (!t || !atomic_load(&t->pause_req) || atomic_load(&t->svc_regs)) return;
    ucontext_t *uc = uc_; const struct __darwin_arm_thread_state64 *ss = &uc->uc_mcontext->__ss;
    if (!module_of((void *)__darwin_arm_thread_state64_get_pc(*ss))) return;   // in host code: not stopped; the requester resends
    g_lock();
    if (t->pause_req) {
        for (int i = 0; i < 29; i++) t->ctx_x[i] = ss->__x[i];
        // x18 is Apple's platform register: report the guest's from its slot (code may hold a pointer there). The rewriter's x18 sequences park
        // registers up to 24 bytes below sp, so sp is reported 32 lower.
        t->ctx_x[18] = (uint64_t)(uintptr_t)pthread_getspecific(g_key18);
        t->ctx_fp = __darwin_arm_thread_state64_get_fp(*ss); t->ctx_lr = __darwin_arm_thread_state64_get_lr(*ss);
        t->ctx_sp = __darwin_arm_thread_state64_get_sp(*ss) - 32;
        t->ctx_pc = __darwin_arm_thread_state64_get_pc(*ss);
        t->ctx_pstate = ss->__cpsr;
        memcpy(t->ctx_v, uc->uc_mcontext->__ns.__v, sizeof t->ctx_v);
        t->ctx_fpcr = uc->uc_mcontext->__ns.__fpcr;
        t->ctx_fpsr = uc->uc_mcontext->__ns.__fpsr;
        t->ctx_ok = 1;
        while (t->pause_req) { t->paused = 1; pthread_cond_broadcast(&t->pcv); g_cond_wait(&t->pcv, NULL); }
        t->paused = 0; t->ctx_ok = 0;
    }
    g_unlock();
}
void thr_svc_enter(uint64_t *regs, uint64_t pc) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    t->svc_pc = pc;
    t->svc_x18 = (uint64_t)(uintptr_t)pthread_getspecific(g_key18);
    atomic_store(&t->svc_regs, regs);
    atomic_fetch_or(&t->svc_state, 1);
    if (atomic_load_explicit(&t->qos_pending, memory_order_relaxed) && atomic_exchange(&t->qos_pending, 0)) {
        g_lock(); qos_apply(t); g_unlock();
    }
    if (atomic_load(&t->pause_req)) { g_lock(); if (t->pause_req && !t->paused) { t->paused = 1; pthread_cond_broadcast(&t->pcv); } g_unlock(); }
}
void thr_svc_leave(void) {
    thr_t *t = pthread_getspecific(g_keythr); if (!t) return;
    int in = 1; if (atomic_compare_exchange_strong(&t->svc_state, &in, 0)) { atomic_store(&t->svc_regs, NULL); return; }   // no pause requested: G is not needed
    g_lock();
    while (t->pause_req) { t->paused = 1; pthread_cond_broadcast(&t->pcv); g_cond_wait(&t->pcv, NULL); }
    t->paused = 0; atomic_fetch_and(&t->svc_state, ~1); atomic_store(&t->svc_regs, NULL);
    g_unlock();
}
static void pause_install(void) {
    struct sigaction sa = {.sa_sigaction = on_pause_sig, .sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESTART};
    sigemptyset(&sa.sa_mask);
    sigaction(SIGUSR2, &sa, NULL);
}
// Horizon svc::ThreadContext (arm64, 0x320 bytes)
typedef struct { uint64_t r[29], fp, lr, sp, pc; uint32_t pstate, pad; __uint128_t v[32]; uint32_t fpcr, fpsr; uint64_t tpidr; } hz_ctx_t;
_Static_assert(sizeof(hz_ctx_t) == 0x320, "ThreadContext");
static void ctx_fill(const thr_t *t, hz_ctx_t *o) {   // G held, t stopped
    memset(o, 0, sizeof *o);
    const uint64_t *r = atomic_load(&t->svc_regs);
    if (r) {   // gate frame: x0..x30 at +0, q0..q31 at +256, nzcv / fpsr / fpcr at +768 / +776 / +784, the gate block's sp at +792
        for (int i = 0; i < 29; i++) o->r[i] = r[i];
        o->r[18] = t->svc_x18;   // the gate frame's x18 is the host's
        o->fp = r[29]; o->lr = r[30]; o->sp = r[99]; o->pc = t->svc_pc; o->pstate = (uint32_t)r[96];
        memcpy(o->v, r + 32, sizeof o->v); o->fpsr = (uint32_t)r[97]; o->fpcr = (uint32_t)r[98];
    } else if (t->ctx_ok) {
        memcpy(o->r, t->ctx_x, sizeof o->r);
        o->fp = t->ctx_fp;
        o->lr = t->ctx_lr;
        o->sp = t->ctx_sp;
        o->pc = t->ctx_pc;
        o->pstate = t->ctx_pstate;
        memcpy(o->v, t->ctx_v, sizeof o->v); o->fpcr = t->ctx_fpcr; o->fpsr = t->ctx_fpsr;
    }
}

// DM_HITCH_DUMP=<ms>[:<s>]: when no frame was presented for <ms> (after <s> s), log every guest thread once per stall, at most 24 stalls
_Atomic uint64_t dm_present_ns;   // host monotonic ns of the last present
typedef struct {   // copied under G: a thread that exits meanwhile is not read
    uint32_t handle, svc; int prio, core, rs, sleeping, host_work, wkind;
    uint64_t mask, waddr, wkey, lr;
    char name[48];
    int have, npc, guest;
    uint64_t x[10], pc[24];
} hitch_t;
// G held. Between suspend and resume only mach calls and memory reads: no malloc, stdio or dladdr (the thread may hold their locks)
static void hitch_grab(thr_t *t, hitch_t *h) {
    memset(h, 0, sizeof *h);
    h->handle = t->handle;
    h->svc = t->last_svc;
    h->prio = t->prio;
    h->core = t->core;
    h->mask = t->mask;
    h->wkind = t->wkind & 7;
    h->waddr = t->waddr;
    h->wkey = t->wkey;
    h->lr = t->last_lr;
    h->sleeping = t->sleeping;
    h->host_work = t->host_work && *t->host_work;
    h->rs = -1;
    char nb[48]; snprintf(h->name, sizeof h->name, "%s", thr_name(t, nb, sizeof nb));
    if (!t->mport) return;
    thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
    if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) == KERN_SUCCESS) h->rs = bi.run_state;
    if (thread_suspend(t->mport) != KERN_SUCCESS) return;
    arm_thread_state64_t st; c = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &c) == KERN_SUCCESS) {
        uint64_t pc = __darwin_arm_thread_state64_get_pc(st), fp = __darwin_arm_thread_state64_get_fp(st);
        h->have = 1; h->guest = module_of((void *)pc) != NULL;
        h->x[0] = st.__x[0];
        h->x[1] = st.__x[1];
        h->x[2] = st.__x[2];
        for (int i = 19; i <= 25; i++) h->x[3 + i - 19] = st.__x[i];
        h->pc[h->npc++] = pc; h->pc[h->npc++] = __darwin_arm_thread_state64_get_lr(st);
        while (h->npc < 24) {
            uint64_t fr[2];
            vm_size_t got = 0;
            if (!fp || vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &got) || got != 16) break;
            h->pc[h->npc++] = fr[1]; fp = fr[0];
        }
    }
    thread_resume(t->mport);
}
static void hitch_log(const hitch_t *h) {
    static const char *kinds[] = {"run", "mutex", "condvar", "address", "sync"};
    char w[900]; size_t q = 0, n = sizeof w; w[0] = 0;
    if (h->have) {
        if (h->guest) { q += (size_t)snprintf(w + q, n - q, " [x0=%#llx x1=%#llx x2=%#llx", (unsigned long long)h->x[0], (unsigned long long)h->x[1], (unsigned long long)h->x[2]);
            for (int i = 0; i < 7; i++) q += (size_t)snprintf(w + q, n - q, " x%d=%#llx", 19 + i, (unsigned long long)h->x[3 + i]);
            q += (size_t)snprintf(w + q, n - q, "]"); }
        for (int k = 0, g = 0; k < h->npc && g < 8 && q < n - 80; k++) {
            uint64_t a = h->pc[k]; module_t *m = module_of((void *)a); Dl_info di;
            if (m) { q += (size_t)snprintf(w + q, n - q, " %s+%#llx", m->name, (unsigned long long)(a - (uint64_t)m->base)); g++; }
            else if (k > 5) continue;
            else if (dladdr((void *)a, &di) && di.dli_sname) q += (size_t)snprintf(w + q, n - q, " %s", di.dli_sname);
            else q += (size_t)snprintf(w + q, n - q, " %#llx", (unsigned long long)a);
        }
    }
    module_t *m = module_of((void *)h->lr);
    LOG("  %#x '%s' prio %d core %d mask %#llx %s %#llx/%#llx%s%s state %d svc %#x lr=%s+%#llx at:%s", h->handle, h->name, h->prio, h->core, (unsigned long long)h->mask,
        kinds[h->wkind], (unsigned long long)h->waddr, (unsigned long long)h->wkey, h->sleeping ? " sleeping" : "", h->host_work ? " host-work" : "", h->rs, h->svc, m ? m->name : "?",
        (unsigned long long)(m ? h->lr - (uint64_t)m->base : h->lr), w);
}
static void *hitch_main(void *arg) {
    pthread_setname_np("dm hitch dump");
    const char *e = arg; uint64_t lim = (uint64_t)atoi(e) * 1000000ull, seen = 0, skip = 0; int n = 0;
    if (strchr(e, ':')) skip = (uint64_t)atoi(strchr(e, ':') + 1) * 1000000000ull;
    extern uint64_t g_t0_ns;
    static hitch_t hs[256];
    while (n < 24) {
        { struct timespec ts = {0, 2000000}; nanosleep(&ts, 0); }
        uint64_t p = atomic_load(&dm_present_ns), now = now_ns();
        if (!p || p == seen || now - p < lim || now - g_t0_ns < skip) continue;
        seen = p; n++;
        LOG("hitch dump %d: no present for %.1f ms", n, (now - p) / 1e6);
        int nt = 0;
        g_lock();
        for (thr_t *t = g_all; t && nt < 256; t = t->all) if (t->started && !t->exited) hitch_grab(t, &hs[nt++]);
        g_unlock();
        for (int k = 0; k < nt; k++) hitch_log(&hs[k]);
    }
    return NULL;
}
static void hitch_init(void) {
    const char *e = getenv("DM_HITCH_DUMP"); if (!e || atoi(e) <= 0) return;
    pthread_t th; pthread_create(&th, NULL, hitch_main, (void *)e); pthread_detach(th);
    LOG("DM_HITCH_DUMP=%s: the guest threads are logged when no frame is presented for that many ms", e);
}
static void yield_spin_init(void);
// A reset's boot registers the main thread again into emptied tables: handle 0x1001 and the first thread ID, as a new process
static void thread_once(void) {
    pthread_key_create(&g_keythr, NULL);
    qos_map_init();
    yield_spin_init();
    { const char *e = getenv("DM_PREEMPT"); g_core_preempt = e ? atoi(e) != 0 : DM_GAME_CORE_PREEMPT; }
    { const char *e = getenv("DM_WAKE_UNLOCKED"); if (e) g_wake_unlocked = atoi(e) != 0; }
    pause_install();
    hitch_init();
}
uint32_t svc_thread_main_init(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT; pthread_once(&once, thread_once);
    thr_t *t = calloc(1, sizeof *t);
    pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
    // affinity = its ideal core alone, as Horizon creates every thread (KThread::Initialize)
    t->started = t->entered = 1;
    t->prio = 44;
    t->core = 0;
    t->mask = 1;
    t->pt = pthread_self();
    t->mport = pthread_mach_thread_np(t->pt);
    t->host_work = &dm_host_work;
    // the boot pthread has default QoS: apply the guest policy first
    qos_apply(t);
    qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0;
    pthread_get_qos_class_np(pthread_self(), &qc, &rp); t->qos = (uint32_t)qc;
    g_lock();
    t->handle = h_new(O_THREAD, t);
    t->id = g_next_id++;
    t->all = g_all;
    g_all = t;
    g_unlock();
    pthread_setspecific(g_keythr, t);
    guest_thread_set_id(t->id);
    pthread_setname_np("0x1001 main thread");
    LOG("main guest thread QoS %#x (priority %d)", t->qos, t->prio);
    return t->handle;
}

static uint64_t g_main_yield_n, g_main_yield_ns;   // written only by the guest main thread
void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = g_main_yield_n; *ns = g_main_yield_ns; }
uint32_t dm_guest_work_qos(unsigned *count) {
    uint32_t qos = QOS_CLASS_UNSPECIFIED; unsigned n = 0;
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        if (t->handle == 0x1001) continue;
        if (!n) qos = t->qos; else if (qos != t->qos) qos = UINT32_MAX;
        n++;
    }
    g_unlock();
    if (count) *count = n;
    return qos;
}

// Resolve and retain the whole list before checking readiness: a close cannot destroy an object or redirect a wait
static int sync_open(uint32_t h, sync_ref_t *ref) {   // G held; no pseudo-handles
    unsigned i = h - 0x1000;
    if (i == 0 || i >= MAX_H) return 0;
    if (g_h[i].type == O_THREAD) ((thr_t *)g_h[i].obj)->refs++;
    else if (g_h[i].type == O_EVENT_R) ((event_t *)g_h[i].obj)->refs++;
    else return 0;
    *ref = (sync_ref_t){g_h[i].obj, g_h[i].type};
    return 1;
}
static void sync_close(sync_ref_t ref) {   // G held
    if (ref.type == O_THREAD) thr_put(ref.obj);
    else {
        event_t *e = ref.obj;
        if (!--e->refs) free(e);
    }
}
static int sync_signaled(sync_ref_t ref) {   // G held
    return ref.type == O_THREAD ? ((thr_t *)ref.obj)->exited : ((event_t *)ref.obj)->signaled;
}
// a clear before the waiter resumes must not erase the notification that made it runnable
static void wake_sync_waiters(const void *obj) {
    for (thr_t *w = g_wait; w; ) { thr_t *n = w->next;
        if (w->wkind == 4)
            for (int i = 0; i < w->wn; i++) if (w->wobjs[i].obj == obj) {
                w->widx = i; wake(w, 0); break;
            }
        w = n; }
}
static const char *thr_name(thr_t *t, char *nb, size_t n) {   // SDK ThreadType+0x1a0 = name pointer
    if (!t->arg) return t->handle == 0x1001 ? "main thread" : "?";
    uint64_t np = 0; vm_size_t got = 0;
    if (vm_read_overwrite(mach_task_self(), t->arg + 0x1a0, 8, (vm_address_t)&np, &got) || !np ||
        vm_read_overwrite(mach_task_self(), np, n - 1, (vm_address_t)nb, &got)) return "?";
    nb[got] = 0; return nb;
}
// the unit tests check that waits leak none
void thr_counts(int *handles, int *live, int *objs) {
    int nh = 0, nl = 0, no = 0;
    g_lock();
    for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    for (thr_t *t = g_all; t; t = t->all) { no++; nl += t->started && !t->exited; }
    g_unlock();
    *handles = nh; *live = nl; *objs = no;
}
// as Horizon counts the thread resource limit: created and not exited; the host's own threads are not the game's
int thr_limit_used(void) {
    int n = 0;
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) n += !t->exited && !t->host_owned;
    g_unlock();
    return n;
}
// 0 if not a guest thread
int thr_name_of(pthread_t pt, char *out, size_t n) {
    int ok = 0; char nb[64];
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) if (t->pt == pt && !t->exited) {
        snprintf(out, n, "%#x %s", t->handle, thr_name(t, nb, sizeof nb));
        ok = 1;
        break;
    }
    g_unlock();
    return ok;
}
uint32_t thr_cur_handle(void) { thr_t *t = pthread_getspecific(g_keythr); return t ? t->handle : 0; }   // only reads TSD: signal-safe
// State dump (crash.c): read-only, takes no lock; other threads are suspended a moment to walk their frame chain
static void thr_bt(thr_t *t) {
    if (!t->mport || t->exited || t->mport == mach_thread_self()) return;
    if (thread_suspend(t->mport) != KERN_SUCCESS) return;
    arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
    if (thread_get_state(t->mport, ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
        // a thread spinning on a condition shows in its x19-x28 (often the object) and what they point to
        if (t->wkind == 0) { char rl[400]; int q = snprintf(rl, sizeof rl, "[dm]   %#x registers pc=", t->handle); uint64_t pc = __darwin_arm_thread_state64_get_pc(st); module_t *pm = module_of((void *)pc);
            q += pm ? snprintf(rl + q, sizeof rl - q, "%s+%#llx", pm->name, pc - (uint64_t)pm->base) : snprintf(rl + q, sizeof rl - q, "%#llx", pc);
            for (int i = 19; i <= 28; i++) q += snprintf(rl + q, sizeof rl - q, " x%d=%#llx", i, st.__x[i]);
            fprintf(stderr, "%s\n", rl);
            for (int i = 19; i <= 20; i++) { uint8_t raw[128]; vm_size_t g = 0; uint64_t a = st.__x[i];
                if (a < 0x100000000ull || vm_read_overwrite(mach_task_self(), a, sizeof raw, (vm_address_t)raw, &g) || g != sizeof raw) continue;
                char hx[420];
                int h = 0;
                for (int k = 0; k < 128; k += 4) h += snprintf(hx + h, sizeof hx - h, "%08x ", *(uint32_t *)(raw + k));
                uint64_t vt = *(uint64_t *)raw;
                module_t *vm_ = module_of((void *)vt);
                char vs[64] = "";
                if (vm_) snprintf(vs, sizeof vs, " (first 8 bytes = %s+%#llx)", vm_->name, vt - (uint64_t)vm_->base);
                fprintf(stderr, "[dm]   %#x x%d → 128 bytes from %#llx%s: %s\n", t->handle, i, a, vs, hx); } }
        uint64_t fp = __darwin_arm_thread_state64_get_fp(st), lr = __darwin_arm_thread_state64_get_lr(st);
        if (t->wkind == 0 && module_of((void *)__darwin_arm_thread_state64_get_pc(st))) { uint64_t sp = __darwin_arm_thread_state64_get_sp(st), stk[128]; vm_size_t g = 0;   // only when pc is in a guest module
            if (sp > 0x100000000ull && !vm_read_overwrite(mach_task_self(), sp, sizeof stk, (vm_address_t)stk, &g) && g == sizeof stk)
                for (int r = 0; r < 128; r += 8) fprintf(stderr, "[dm]   %#x stack sp+%#05x: %016llx %016llx %016llx %016llx %016llx %016llx %016llx %016llx\n", t->handle, r * 8,
                        stk[r], stk[r + 1], stk[r + 2], stk[r + 3], stk[r + 4], stk[r + 5], stk[r + 6], stk[r + 7]); }
        char line[1024]; int p = snprintf(line, sizeof line, "[dm]   %#x backtrace:", t->handle), nf = 0;
        for (int k = 0; k < 64 && fp && nf < 20 && p < (int)sizeof line - 40; k++) {
            module_t *m = module_of((void *)lr);
            if (m) { p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, lr - (uint64_t)m->base); nf++; }
            uint64_t fr[2]; vm_size_t g2 = 0;
            if (vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
            fp = fr[0]; lr = fr[1];
        }
        fprintf(stderr, "%s\n", line);
    }
    thread_resume(t->mport);
}
void svc_thread_dump(void) {
    static const char *kinds[] = {"running/not waiting", "mutex", "condvar", "address", "sync object"};
    int nh = 0; for (int i = 1; i < MAX_H; i++) nh += g_h[i].type != O_FREE;
    fprintf(stderr, "[dm] handle table use %d / %d, threads created so far %llu\n", nh, MAX_H, g_next_id - 0x51);
    fprintf(stderr, "[dm] same-core handoffs %llu, total %.1f ms, timed rechecks %llu\n",
            (unsigned long long)g_preempt_n, g_preempt_ns / 1e6, (unsigned long long)atomic_load_explicit(&g_hand_timeouts, memory_order_relaxed));
    for (thr_t *t = g_all; t; t = t->all) {
        module_t *m = module_of((void *)t->last_lr);
        char nb[32]; const char *nm = thr_name(t, nb, sizeof nb);
        double cpu = -1;
        if (t->mport && !t->exited) { thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT;
            if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS)
                cpu = bi.user_time.seconds + bi.user_time.microseconds / 1e6 + bi.system_time.seconds + bi.system_time.microseconds / 1e6; }
        char own[96] = ""; if (t->wkind == 1 && t->waddr) { uint32_t w = 0; vm_size_t g2 = 0;   // lock word = owner handle | 0x40000000
            if (!vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2)) snprintf(own, sizeof own, " owner %#x", w & ~WAITERS_BIT); }
        if (t->wkind == 2 && t->waddr) { uint32_t w = 0, k = 0; vm_size_t g2 = 0;
            vm_read_overwrite(mach_task_self(), t->waddr, 4, (vm_address_t)&w, &g2); vm_read_overwrite(mach_task_self(), t->wkey, 4, (vm_address_t)&k, &g2);
            snprintf(own, sizeof own, " lock word %#x condvar key %#llx=%#x wait #%llu", w, t->wkey, k, (unsigned long long)t->wseq); }   // wait #: arrival order, the wake order at equal priority
        uint8_t raw[64]; vm_size_t g3 = 0;
        int have_raw = (t->wkind == 1 || t->wkind == 2 || t->wkind == 3) && t->waddr && !vm_read_overwrite(mach_task_self(), t->waddr & ~0xfull, 64, (vm_address_t)raw, &g3) && g3 == 64;
        char life[96]; uint64_t nowns = now_ns();
        if (t->exited) snprintf(life, sizeof life, " exited %.1f s ago", (nowns - t->exit_ns) / 1e9); else if (!t->born_ns) snprintf(life, sizeof life, " present since process start"); else snprintf(life, sizeof life, " created %.1f s ago", (nowns - t->born_ns) / 1e9);
        fprintf(stderr, "[dm] thread %#x '%s' priority %d core %d mask %#llx CPU %.2fs svc %llu %s%s%s waits:%s addr=%#llx%s last svc %#x lr=%s+%#llx\n", t->handle, nm, t->prio, t->core, (unsigned long long)t->mask,
                cpu, t->nsvc, t->exited ? "exited" : "", t->started ? "" : "not started", life, kinds[t->wkind & 7], t->waddr, own, t->last_svc,
                m ? m->name : "?", m ? t->last_lr - (uint64_t)m->base : t->last_lr);
        if (have_raw) { char hx[200]; int q = 0; for (int i = 0; i < 64; i += 4) q += snprintf(hx + q, sizeof hx - q, "%08x ", *(uint32_t *)(raw + i));
                        fprintf(stderr, "[dm]   %#x waited-for object, 64 bytes from %#llx: %s\n", t->handle, t->waddr & ~0xfull, hx); }
        // Engines without frame pointers stop the backtrace at the SVC wrapper: list the stack words that point into a module. regs[99] = the gate
        // block's sp (pushed x29 / x30, guest stack at +16); read page by page up to the first unreadable one.
        uint64_t *sr = atomic_load(&t->svc_regs);
        if (sr && t->wkind) { uint64_t stk[160]; size_t got = 0; vm_address_t sa = (vm_address_t)sr[99];
            while (got < sizeof stk) { vm_size_t n = sizeof stk - got, g4 = 0, room = vm_page_size - ((sa + got) & (vm_page_size - 1)); if (n > room) n = room;
                if (vm_read_overwrite(mach_task_self(), sa + got, n, (vm_address_t)((uint8_t *)stk + got), &g4) || g4 != n) break;
                got += n; }
            if (got >= 16 && atomic_load(&t->svc_regs) == sr) {   // still in that SVC: the words are its stack
                char sl[1100]; int q = snprintf(sl, sizeof sl, "[dm]   %#x guest stack:", t->handle), nfr = 0;
                for (int i = 0; i < (int)(got / 8) && nfr < 16 && q < (int)sizeof sl - 40; i++) { module_t *m2 = module_of((void *)stk[i]);
                    if (m2 && stk[i] - (uint64_t)m2->base < m2->text_end) { q += snprintf(sl + q, sizeof sl - q, " [%d]%s+%#llx", i * 8, m2->name, stk[i] - (uint64_t)m2->base); nfr++; } }
                q += snprintf(sl + q, sizeof sl - q, " | x19=%#llx x20=%#llx x21=%#llx x22=%#llx x23=%#llx x24=%#llx x25=%#llx x26=%#llx x27=%#llx x28=%#llx", sr[19], sr[20], sr[21], sr[22], sr[23], sr[24], sr[25], sr[26], sr[27], sr[28]);
                fprintf(stderr, "%s\n", sl); } }
        thr_bt(t);
    }
    thread_act_array_t ths; mach_msg_type_number_t nth = 0;
    if (task_threads(mach_task_self(), &ths, &nth) == KERN_SUCCESS) {
        for (mach_msg_type_number_t i = 0; i < nth; i++) {
            int guest = 0; for (thr_t *t = g_all; t; t = t->all) if (t->mport == ths[i]) { guest = 1; break; }
            if (!guest && ths[i] != mach_thread_self() && thread_suspend(ths[i]) == KERN_SUCCESS) {
                arm_thread_state64_t st; mach_msg_type_number_t n = ARM_THREAD_STATE64_COUNT;
                if (thread_get_state(ths[i], ARM_THREAD_STATE64, (thread_state_t)&st, &n) == KERN_SUCCESS) {
                    char line[1200]; int p = snprintf(line, sizeof line, "[dm] host thread %#x:", ths[i]);
                    uint64_t fp = __darwin_arm_thread_state64_get_fp(st), pc = __darwin_arm_thread_state64_get_pc(st);
                    for (int k = 0; k < 12 && p < (int)sizeof line - 80; k++) {
                        Dl_info di; module_t *m = module_of((void *)pc);
                        if (m) p += snprintf(line + p, sizeof line - p, " %s+%#llx", m->name, pc - (uint64_t)m->base);
                        else if (dladdr((void *)pc, &di) && di.dli_sname) p += snprintf(line + p, sizeof line - p, " %s+%#lx", di.dli_sname, (unsigned long)(pc - (uint64_t)di.dli_saddr));
                        else p += snprintf(line + p, sizeof line - p, " %#llx", pc);
                        uint64_t fr[2]; vm_size_t g2 = 0;
                        if (!fp || vm_read_overwrite(mach_task_self(), fp, 16, (vm_address_t)fr, &g2) || g2 != 16 || fr[0] <= fp) break;
                        fp = fr[0]; pc = fr[1];
                    }
                    fprintf(stderr, "%s\n", line);
                }
                thread_resume(ths[i]);
            }
            mach_port_deallocate(mach_task_self(), ths[i]);
        }
        vm_deallocate(mach_task_self(), (vm_address_t)ths, nth * sizeof *ths);
    }
}

// core_preempt (DM_PREEMPT) also checks already-runnable peers after every thread SVC; cooperative boundaries, not a guest-core scheduler
static int svc_thread_impl(int num, uint64_t *regs);
// Yield back-off: a loop that polls and yields is free on a Switch but spins a host core. A run of yields passes as sched_yield for 2 ms, then each
// sleeps an eighth of the run (10-250 µs); a clock wait backs off from its first yield. DM_YIELD_BACKOFF=0 | <cap µs>[:<start µs>]
// These settings are read once by thread_once, before any other guest thread exists: the hot paths read them unlocked.
static int64_t g_bo_cap = 250000, g_bo_start = 2000000;
static int g_yield_clock = 2;   // DM_YIELD_CLOCK: 0 plain yields, 1 back off from the start, 2 (default) also sleep to the predicted end
static int g_spin_on = 1;       // DM_SPIN=0: spin waits do not yield or sleep
static uint64_t g_spin_poll_ns = 2000000;   // DM_SPIN_POLL µs
static void yield_spin_init(void) {
    const char *e = getenv("DM_YIELD_BACKOFF");
    g_bo_cap = 250000; g_bo_start = 2000000;
    if (e && *e) {
        g_bo_cap = atoll(e) * 1000;
        const char *c = strchr(e, ':');
        if (c) g_bo_start = atoll(c + 1) * 1000;
    }
    if (!g_bo_cap) LOG("DM_YIELD_BACKOFF=0: no back-off on repeated yields (plain sched_yield spin)"); else if (e && *e) LOG("DM_YIELD_BACKOFF: back off after %lld µs of repeated yields, sleeping at most %lld µs at a time", (long long)(g_bo_start / 1000), (long long)(g_bo_cap / 1000));
    e = getenv("DM_YIELD_CLOCK");
    g_yield_clock = e && *e ? atoi(e) : 2;
    if (g_yield_clock != 2) LOG("DM_YIELD_CLOCK=%d: yield loops that read the clock %s", g_yield_clock, g_yield_clock ? "back off from their start, with no sleep to a predicted end" : "back off after the start, as other yields");
    e = getenv("DM_SPIN");
    const char *p = getenv("DM_SPIN_POLL");
    g_spin_poll_ns = (p && *p ? strtoull(p, 0, 10) : 2000) * 1000;
    g_spin_on = !(e && *e && atoi(e) == 0);
    if (!g_spin_on) LOG("DM_SPIN=0: spin waits do not yield or sleep"); else if (p && *p) LOG("DM_SPIN_POLL: spin waits poll for %llu µs, then yield", (unsigned long long)(g_spin_poll_ns / 1000));
}
// 0: off
static uint64_t yield_backoff_cap(void) { return (uint64_t)g_bo_cap; }
static uint64_t yield_backoff_ns(uint64_t run_ns, int clock) {
    if (!g_bo_cap || (!clock && run_ns < (uint64_t)g_bo_start)) return 0;
    uint64_t s = run_ns / 8; if (s < 10000) s = 10000;
    return s < (uint64_t)g_bo_cap ? s : (uint64_t)g_bo_cap;
}
static void sleep_precise(uint64_t dl);
static void backoff_sleep(thr_t *me, uint64_t bo) { me->nbackoff++; me->sleeping = 1; struct timespec ts = {0, (long)bo}; nanosleep(&ts, 0); me->sleeping = 0; }
static void yield_for(thr_t *me, uint64_t run_ns, int clock) {
    uint64_t bo = yield_backoff_ns(run_ns, clock);
    if (bo) backoff_sleep(me, bo); else sched_yield();
}
// A run of yields is a wait only while the loop makes no progress (x19-x28 unchanged, or back to the yield before last); a yield that made
// progress returns at once (docs/reference/threads.md#yields-that-make-progress-return-at-once-thr_yield). A pass that read the clock makes a clock wait.
static inline uint64_t fnv_mix(uint64_t sig, uint64_t v) { return (sig ^ v) * 0x100000001b3ull; }
static uint64_t yield_sig(const uint64_t *regs) {
    uint64_t sig = 0xcbf29ce484222325ull;
    for (int i = 19; i <= 28; i++) sig = fnv_mix(sig, regs[i]);
    return sig;
}
static void clock_wait(thr_t *me, uint64_t sig, uint64_t now, int predict);
static void thr_yield(thr_t *me, const uint64_t *regs) {
    if (!me) { sched_yield(); return; }
    uint64_t y0 = now_ns(), sig = yield_sig(regs);
    int same = me->yrun_svc + 1 == me->nsvc;
    int moved = same && me->yrun_sig != sig && me->yrun_sig2 != sig;
    int clock = g_yield_clock && me->yrun_yield && me->yrun_yield == me->nsvc && !moved && dm_clock_reads != me->yrun_clk;   // only yields in a row: a wait or sleep between leaves yrun_clk stale
    me->nyield++;
    if (!same || moved) me->yrun_t0 = y0;
    me->yrun_svc = me->nsvc;
    me->yrun_yield = me->nsvc + 1;
    me->yrun_sig2 = me->yrun_sig;
    me->yrun_sig = sig;
    me->yrun_clk = dm_clock_reads;
    if (clock) { me->nclkyield++; clock_wait(me, sig, y0, g_yield_clock == 2 && !dm_detclock_enabled()); }
    else if (!moved) yield_for(me, y0 - me->yrun_t0, 0);
    me->poll_ret = now_ns();
    if (clock && sig == me->cw_sig) me->cw_last = me->poll_ret;
    if (me->handle == 0x1001) { g_main_yield_ns += me->poll_ret - y0; g_main_yield_n++; }
}
// A clock wait at a steady period predicts its end (last end + period): after three periods within CW_TOL it sleeps to CW_MARGIN before it,
// then polls (docs/reference/threads.md#clock-waits-sleep-to-their-predicted-end). make -C dm/host/tests build/t_clockwait checks the cases.
#define CW_MARGIN 150000   // sleep_precise ends within 20 µs
#define CW_TOL 250000
static void clock_wait(thr_t *me, uint64_t sig, uint64_t now, int predict) {
    if (me->cw_run != me->yrun_t0) {   // the run's first clock yield (its second: the first follows another SVC)
        me->cw_run = me->yrun_t0;
        if (sig != me->cw_sig) {
            if (me->cw_sig && now - me->cw_last < 1000000000ull) { yield_for(me, now - me->yrun_t0, 1); return; }
            me->cw_sig = sig;
            me->cw_end = me->cw_per = me->cw_per2 = 0;
            me->cw_ok = me->cw_pred = me->cw_trip = 0;
        } else {
            if (me->cw_pred && !me->cw_after) {   // ended at the first poll after its predicted sleep: maybe late
                // once: a period the back-off ended carries its lateness, predict shorter; twice in a row: the period changed
                if (me->cw_trip++ || me->cw_per < 4 * CW_MARGIN) { me->cw_ok = 0; me->cw_trip = 0; }
                else { me->cw_per -= 2 * CW_MARGIN; me->cw_per2 = 0; }
            } else if (me->cw_end) {
                me->cw_trip = 0;
                uint64_t per = me->cw_last - me->cw_end;
                me->cw_ok = me->cw_per && per + CW_TOL > me->cw_per && per < me->cw_per + CW_TOL ? me->cw_ok + (me->cw_ok < 3) : 0;
                me->cw_per2 = me->cw_per; me->cw_per = per;
            }
            me->cw_end = me->cw_last;
        }
        me->cw_pred = me->cw_after = 0;
        if (predict && me->cw_ok >= 3) {
            uint64_t per = me->cw_per2 && me->cw_per2 < me->cw_per ? me->cw_per2 : me->cw_per, dl = me->cw_end + per;
            if (dl > now + 2 * CW_MARGIN) {
                me->nbackoff++; me->ncwpred++; me->sleeping = 1; sleep_precise(dl - CW_MARGIN); me->sleeping = 0;
                me->cw_pred = 1; me->cw_wake = now_ns(); return;
            }
        }
    } else if (me->cw_pred) {
        me->cw_after++;
        uint64_t past = now > me->cw_wake ? now - me->cw_wake : 0;
        if (past < 2 * CW_MARGIN) sched_yield(); else yield_for(me, past - 2 * CW_MARGIN, 1);
        return;
    }
    yield_for(me, now - me->yrun_t0, 1);
}
// Spin waits (patch_nso.py "Spin waits"): a guest poll with no SVC, free on a Switch while its core has work. It polls on for
// DM_SPIN_POLL µs (default 2000), then sleeps as a run of yields; DM_SPIN=0: no sleeps.
#define SPIN_GAP_NS 20000
void dm_spin_wait(uint64_t gate, uint64_t site) {
    thr_t *me = pthread_getspecific(g_keythr);
    if (!me) return;
    uint64_t now = now_ns();
    if (me->srun_svc != me->nsvc || now - me->spin_ret > SPIN_GAP_NS) me->srun_t0 = now;
    me->srun_svc = me->nsvc; me->nspin++;
    if (site != me->spin_site || !me->spin_mod) {
        const module_t *m = module_of((void *)gate);
        me->spin_mod = m ? m->name : "?";
        me->spin_site = site;
    }
    if (g_spin_on && now - me->srun_t0 >= g_spin_poll_ns) { me->nyield++; yield_for(me, now - me->srun_t0, 0); }
    me->spin_ret = now_ns();
}
// A timed wait under 20 µs that times out is a poll, a step of the run of yields; past the back-off's start its timeout becomes the back-off
// sleep (docs/reference/threads.md#short-timed-waits-are-polls). Returns the timeout to wait with.
static int64_t poll_wait_begin(int64_t ns) {
    if (ns <= 0 || ns >= 20000) return ns;
    thr_t *me = pthread_getspecific(g_keythr); if (!me) return ns;
    uint64_t now = now_ns();
    if (me->yrun_svc + 1 != me->nsvc || now - me->poll_ret > SPIN_GAP_NS) me->yrun_t0 = now;
    me->yrun_svc = me->nsvc;
    uint64_t bo = yield_backoff_ns(now - me->yrun_t0, 0);
    if (bo <= (uint64_t)ns) return ns;
    me->nyield++; me->nbackoff++;
    return (int64_t)bo;
}
// A timed-out wait with a timeout of 0 is a step of the run of yields by their rules; its signature mixes x19-x28 (0 is also the SDK's try form) with
// target_sig, the wait's target and lr, since the SDK's non-tail-call wrappers overwrite x19-x21 with constants. Past the back-off's start the thread
// sleeps after it, never as a waiter, and still answers TimedOut as Horizon does (docs/reference/threads.md#zero-timeout-waits-are-polls).
static void poll_wait_end(int64_t ns, int64_t r, const uint64_t *regs, uint64_t target_sig) {
    if (ns < 0 || ns >= 20000) return;
    thr_t *me = pthread_getspecific(g_keythr); if (!me) return;
    if (r != R_TIMEOUT) { me->yrun_svc = 0; return; }
    if (ns) { me->poll_ret = now_ns(); return; }
    uint64_t now = now_ns(), sig = fnv_mix(yield_sig(regs), target_sig);
    int same = me->yrun_svc && me->yrun_svc + 1 == me->nsvc && now >= me->poll_ret && now - me->poll_ret <= SPIN_GAP_NS;
    if (!same || (me->yrun_sig != sig && me->yrun_sig2 != sig)) me->yrun_t0 = now;
    me->yrun_svc = me->nsvc; me->yrun_sig2 = me->yrun_sig; me->yrun_sig = sig;
    uint64_t bo = yield_backoff_ns(now - me->yrun_t0, 0);
    if (bo) { me->nyield++; backoff_sleep(me, bo); }
    me->poll_ret = now_ns();
}
// A sleep shorter than the back-off cap is a poll, a step of the run of yields (docs/reference/threads.md#short-sleeps-are-polls)
void dm_guest_sleep_ns(int64_t ns);
static void poll_sleep(thr_t *me, int64_t ns) {
    uint64_t now = now_ns();
    if (me->yrun_svc + 1 != me->nsvc || now - me->poll_ret > SPIN_GAP_NS) me->yrun_t0 = now;
    me->yrun_svc = me->nsvc;
    uint64_t bo = yield_backoff_ns(now - me->yrun_t0, 0);
    if (bo > (uint64_t)ns) { me->nyield++; backoff_sleep(me, bo); }
    else dm_guest_sleep_ns(ns);
    me->poll_ret = now_ns();
}
// the two threads that yielded or spun most since the last call, for the power line
void dm_yield_top(char *out, size_t n) {
    thr_t *top[2] = {0}; uint64_t d[2] = {0}, b[2] = {0}, c[2] = {0}, cp[2] = {0}, s[2] = {0}, r[2] = {0};
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        uint64_t y = t->nyield - t->nyield_seen, bo = t->nbackoff - t->nbackoff_seen, ck = t->nclkyield - t->nclkyield_seen, pr = t->ncwpred - t->ncwpred_seen, sp = t->nspin - t->nspin_seen, k = y + sp;
        t->nyield_seen = t->nyield;
        t->nbackoff_seen = t->nbackoff;
        t->nclkyield_seen = t->nclkyield;
        t->ncwpred_seen = t->ncwpred;
        t->nspin_seen = t->nspin;
        if (k > r[0]) {
            top[1] = top[0];
            d[1] = d[0];
            b[1] = b[0];
            c[1] = c[0];
            cp[1] = cp[0];
            s[1] = s[0];
            r[1] = r[0];
            top[0] = t;
            d[0] = y;
            b[0] = bo;
            c[0] = ck;
            cp[0] = pr;
            s[0] = sp;
            r[0] = k;
        }
        else if (k > r[1]) { top[1] = t; d[1] = y; b[1] = bo; c[1] = ck; cp[1] = pr; s[1] = sp; r[1] = k; }
    }
    size_t o = 0; char nb[32];
    for (int i = 0; i < 2 && top[i] && o < n; i++) {
        o += snprintf(out + o, n - o, "%s%#x '%s' %llu times (back-off sleeps %llu", i ? ", " : "", top[i]->handle, thr_name(top[i], nb, sizeof nb), (unsigned long long)d[i], (unsigned long long)b[i]);
        if (o < n && c[i]) o += snprintf(out + o, n - o, "; clock waits %llu, %llu of them slept to a predicted end", (unsigned long long)c[i], (unsigned long long)cp[i]);
        if (o < n && s[i]) o += snprintf(out + o, n - o, "; spin passes %llu, last at %s+%#llx", (unsigned long long)s[i], top[i]->spin_mod, (unsigned long long)top[i]->spin_site);
        if (o < n) o += snprintf(out + o, n - o, ")");
    }
    g_unlock();
    if (!o && n) snprintf(out, n, "none");
}
// Guest CPU time by Switch priority band (<= 44, 45-59, >= 60) and the three busiest threads; secs = the window length
void dm_thread_cpu(char *out, size_t n, double secs) {
    thr_t *top[3] = {0}; double d[3] = {0}, band[3] = {0};
    g_lock();
    for (thr_t *t = g_all; t; t = t->all) {
        if (!t->mport || t->exited) continue;
        thread_basic_info_data_t bi; mach_msg_type_number_t c = THREAD_BASIC_INFO_COUNT;
        if (thread_info(t->mport, THREAD_BASIC_INFO, (thread_info_t)&bi, &c) != KERN_SUCCESS) continue;
        uint64_t us = (uint64_t)(bi.user_time.seconds + bi.system_time.seconds) * 1000000u + (uint64_t)(bi.user_time.microseconds + bi.system_time.microseconds);
        double ms = t->cpu_seen_us && us > t->cpu_seen_us ? (us - t->cpu_seen_us) / 1e3 / secs : 0; t->cpu_seen_us = us;   // ms per second; the first report takes the baseline
        band[t->prio <= 44 ? 0 : t->prio < 60 ? 1 : 2] += ms;
        for (int i = 0; i < 3; i++) if (ms > d[i]) { for (int j = 2; j > i; j--) { top[j] = top[j - 1]; d[j] = d[j - 1]; } top[i] = t; d[i] = ms; break; }
    }
    size_t o = (size_t)snprintf(out, n, "by guest priority ≤44 %.0f / 45–59 %.0f / ≥60 %.0f ms/s; top: ", band[0], band[1], band[2]); char nb[32];
    for (int i = 0; i < 3 && top[i] && o < n; i++)
        o += (size_t)snprintf(out + o, n - o, "%s%#x '%s' priority %d %.0f ms/s%s", i ? ", " : "", top[i]->handle, thr_name(top[i], nb, sizeof nb), top[i]->prio, d[i], top[i]->host_owned ? " (host-created)" : "");
    if (!top[0] && o < n) snprintf(out + o, n - o, "none");
    g_unlock();
}
// macOS sleeps carry timer slack (docs/reference/threads.md#timer-slack-in-guest-sleeps-sleep_precise): sleep 3/4 of the rest, loop until
// within 20 µs. Steps are at most SLEEP_STEP_MAX, each checking dm_reset_stop.
#define SLEEP_STEP_MAX 100000000ull
static void sleep_precise(uint64_t dl) {
    for (int i = 0; i < 16;) {
        uint64_t now = now_ns(); if (now + 20000 >= dl || atomic_load_explicit(&dm_reset_stop, memory_order_relaxed)) return;
        uint64_t rem = dl - now, s = rem < 100000 ? rem : rem - rem / 4;
        if (s > SLEEP_STEP_MAX) s = SLEEP_STEP_MAX; else i++;
        struct timespec ts = {(time_t)(s / 1000000000u), (long)(s % 1000000000u)}; nanosleep(&ts, 0);
    }
}
// host time: the thread resumes even when its work is needed to present the next frame
void dm_guest_sleep_ns(int64_t ns) {
    if (ns <= 0) return;
    thr_t *me = pthread_getspecific(g_keythr); if (me) me->sleeping = 1;
    sleep_precise(now_ns() + (uint64_t)ns);
    if (me) me->sleeping = 0;
}
// the caller checks its condition and the clock
void dm_guest_cond_wait_ns(pthread_cond_t *c, pthread_mutex_t *m, int64_t ns) {
    thr_t *me = pthread_getspecific(g_keythr); if (me) me->sleeping = 1;
    struct timespec ts = {ns / 1000000000, ns % 1000000000};
    pthread_cond_timedwait_relative_np(c, m, &ts);
    if (me) me->sleeping = 0;
}
int svc_thread(int num, uint64_t *regs) {
    int r = svc_thread_impl(num, regs);
    thr_t *me = pthread_getspecific(g_keythr);
    if (me && (g_core_preempt || me->yield_pending)) {
        uint64_t t0 = now_ns(); int w = dm_yield_to_core_peers(g_core_preempt);
        if (w) { g_preempt_n++; g_preempt_ns += now_ns() - t0; }
    }
    if (me) { me->yield_pending = 0; me->hand_epoch++; }   // the handoff is complete
    return r;
}
static int svc_thread_impl(int num, uint64_t *regs) {
    { thr_t *me = pthread_getspecific(g_keythr); if (me) { me->last_svc = num; me->last_lr = regs[30]; me->nsvc++; } }
    if (g_flag.thr && num != 0x0B) {
        thr_t *me = pthread_getspecific(g_keythr);
        LOG("[%#x] svc %#x x0=%#llx x1=%#llx x2=%#llx x3=%#llx", me ? me->handle : 0, num, regs[0], regs[1], regs[2], regs[3]);
    }
    switch (num) {
    case 0x08: {  // CreateThread(→w1 handle; x1 entry, x2 argument, x3 stack top, w4 priority, w5 core)
        thr_t *t = calloc(1, sizeof *t);
        pthread_cond_init(&t->cv, NULL); pthread_cond_init(&t->pcv, NULL);
        t->entry = regs[1]; t->arg = regs[2]; t->stack_top = regs[3] & ~0xFull;
        // Core -2 = the process's default ideal core (0), a mask of that core alone; GetCurrentProcessorNumber must never answer -2
        // (docs/reference/threads.md#ideal-core-2-resolves-to-the-process-default-core)
        t->prio = (int)regs[4]; t->core = (int)regs[5] >= 0 ? (int)regs[5] : 0; t->mask = 1ull << t->core;
        t->born_ns = now_ns();
        g_lock(); t->handle = h_new(O_THREAD, t); t->id = g_next_id++; g_unlock();
        pthread_attr_t a; pthread_attr_init(&a); pthread_attr_setstacksize(&a, 1 << 20);
        pthread_attr_setdetachstate(&a, PTHREAD_CREATE_DETACHED);   // the guest waits through the handle, no join
        // EAGAIN: no room for its 1 MB stack, or a thread limit
        pthread_t pt;
        int rc = pthread_create(&pt, &a, thread_main, t);
        if (rc) die("pthread_create for guest thread %#x: %s", t->handle, strerror(rc));
        pthread_attr_destroy(&a);
        // pthread_create writes its output without G
        g_lock(); t->pt = pt; t->all = g_all; g_all = t; g_unlock();
        if (g_flag.thr) LOG("CreateThread h=%#x entry=%#llx arg=%#llx sp=%#llx prio=%d core=%d", t->handle, t->entry, t->arg, t->stack_top, t->prio, t->core);
        regs[0] = 0; regs[1] = t->handle; return 1;
    }
    case 0x09: {  // StartThread(w0)
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) {
            t->started = 1; wake_queue(t); note_runnable(t);
            thr_t *self = pthread_getspecific(g_keythr);
            if (self && self->yield_pending) { t->start_by = self; t->start_epoch = atomic_load(&self->hand_epoch); }
        }
        g_unlock();
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x0A: thread_exit(cur()); return 1;
    case 0x0B: {  // SleepThread(x0 ns); 0 / -1 / -2: yields
        int64_t ns = (int64_t)regs[0];
        thr_t *me = pthread_getspecific(g_keythr);
        // A sleep and YieldToAnyThread (-2) take the thread off its core, ending its part in a handoff; 0 / -1 pass the core only to equal or higher priority
        if ((ns > 0 || ns == -2) && me && me->nhand) { g_lock(); hand_leave(me); g_unlock(); }
        if (me && ns >= 20000 && (uint64_t)ns < yield_backoff_cap()) poll_sleep(me, ns);   // a short sleep: a poll
        else if (ns >= 20000) dm_guest_sleep_ns(ns);
        // sleeps under 20 µs return at once from sleep_precise: as yields, they get the back-off
        else thr_yield(me, regs);
        return 1;
    }
    case 0x0C: { g_lock(); thr_t *t = h_get((uint32_t)regs[1], O_THREAD);
        regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)t->prio : 0; g_unlock(); return 1; }
    case 0x0D: { g_lock(); thr_t *t = h_get((uint32_t)regs[0], O_THREAD);   // SetThreadPriority; under DM_QOS_MAP the class follows (at the target's next SVC)
        if (t) { t->prio = (int)regs[1]; if (g_qos_map) { if (t == cur()) qos_apply(t); else atomic_store(&t->qos_pending, 1); } }
        regs[0] = t ? 0 : R_BAD_HANDLE; g_unlock(); return 1; }
    // G across lookup and read: CloseHandle frees an exited thread under G
    case 0x0E: { g_lock(); thr_t *t = h_get((uint32_t)regs[2], O_THREAD);
                 regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? (uint64_t)(uint32_t)t->core : 0; regs[2] = t ? t->mask : 0; g_unlock(); return 1; }
    case 0x0F: {  // SetThreadCoreMask(w0, w1 ideal core, x2 mask): -2 process default, -1 don't care, -3 keep the ideal core
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { int c = (int)regs[1], core = t->core; uint64_t mask = t->mask;
            if (c >= 0) core = c; if (c == -2) { core = 0; mask = 1; } else if (regs[2]) mask = regs[2];
            // An ideal core outside the new mask moves to the mask's lowest core, as on Horizon: else per-core job queues of cores 1 / 2 are never drained
            if (mask && (core < 0 || !(mask >> core & 1))) core = __builtin_ctzll(mask);
            // one store each: GetCurrentProcessorNumber reads them without G
            __atomic_store_n(&t->mask, mask, __ATOMIC_RELAXED); __atomic_store_n(&t->core, core, __ATOMIC_RELAXED); }
        g_unlock();
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x10: { thr_t *t = cur(); int c = __atomic_load_n(&t->core, __ATOMIC_RELAXED); uint64_t m = __atomic_load_n(&t->mask, __ATOMIC_RELAXED);   // never negative; no ideal core: the lowest core of the mask
                 // without G: asked thousands of times a second
                 regs[0] = (uint64_t)(uint32_t)(c >= 0 ? c : m ? __builtin_ctzll(m) : 0); return 1; }
    case 0x32: {  // SetThreadActivity(w0 thread, w1 0 runnable / 1 paused)
        thr_t *me = cur(); int act = (int)regs[1];
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (t == me) r = R_BAD_STATE;
        else if (act == 1) {
            atomic_store(&t->pause_req, 1); hand_signal(t);
            int st = atomic_fetch_or(&t->svc_state, 2);
            if (!t->started || t->exited || (st & 1)) t->paused = 1;
            uint64_t t0 = now_ns(); int warned = 0;
            // the loop lets go of G: a reference keeps t alive
            t->refs++;
            while (!t->paused && !t->exited) {
                g_unlock(); pthread_kill(t->pt, SIGUSR2); g_lock();
                if (t->paused || t->exited) break;
                struct timespec dl; clock_gettime(CLOCK_REALTIME, &dl); dl.tv_nsec += 100000; if (dl.tv_nsec >= 1000000000) { dl.tv_sec++; dl.tv_nsec -= 1000000000; }
                g_cond_wait(&t->pcv, &dl);
                if (!warned && now_ns() - t0 > 1000000000ull) {
                    warned = 1;
                    LOG("[RED] thread %#x still not suspended after more than 1 second (stuck in host code?)", t->handle);
                }
            }
            thr_put(t);
        } else { atomic_store(&t->pause_req, 0); atomic_fetch_and(&t->svc_state, ~2); t->paused = 0; pthread_cond_broadcast(&t->pcv); }
        g_unlock();
        { static int n; if (n++ < 4) LOG("SetThreadActivity %#x → %s (%s)", (uint32_t)regs[0], act ? "pause" : "resume", r ? "failed" : "ok"); }
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x33: {  // GetThreadContext3(x0 out, w1 thread): stopped threads only
        g_lock();
        thr_t *t = h_get((uint32_t)regs[1], O_THREAD); int64_t r = 0;
        if (!t) r = R_BAD_HANDLE;
        else if (!t->paused) r = R_BAD_STATE;
        else ctx_fill(t, (hz_ctx_t *)regs[0]);
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x25: { g_lock(); thr_t *t = h_get((uint32_t)regs[1], O_THREAD); regs[0] = t ? 0 : R_BAD_HANDLE; regs[1] = t ? t->id : 0; g_unlock(); return 1; }   // under G: see 0x0E

    case 0x1A: {  // ArbitrateLock(w0 owner handle, x1 address, w2 tag)
        thr_t *t = cur(); uint64_t addr = regs[1];
        g_lock();
        int64_t r = 0;
        if (ld32(addr) == ((uint32_t)regs[0] | WAITERS_BIT)) { t->waddr = addr; t->wtag = (uint32_t)regs[2];
            // Under DM_QOS_MAP the owner takes the waiter's class until the handover, as the kernel lends priority
            thr_t *o = g_qos_map ? h_peek((uint32_t)regs[0], O_THREAD) : NULL;
            pthread_override_t ov = o && o->pt && !o->exited && t->qos > o->qos ? pthread_override_qos_class_start_np(o->pt, (qos_class_t)t->qos, 0) : NULL;
            r = block(t, 1, -1);
            if (ov) pthread_override_qos_class_end_np(ov); }
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1B: g_lock(); lock_handoff(regs[0]); g_unlock(); regs[0] = 0; return 1;
    case 0x1C: {  // WaitProcessWideKeyAtomic(x0 lock address, x1 key, w2 tag, x3 timeout)
        thr_t *t = cur(); uint64_t addr = regs[0], key = regs[1];
        g_lock();
        // Set the key before releasing the mutex: Signal skips svc 0x1D when it reads key 0 in user mode, so a release first could lose the wake.
        // The link into the wait chain happens in block, under G, so 0x1D cannot come before it.
#ifdef DM_TEST_HOOKS
#define CV_TEST_HOOK() { extern void (*dm_test_cv_after_release)(void); if (dm_test_cv_after_release) dm_test_cv_after_release(); }   // unit test: after the release, before the link
#else
#define CV_TEST_HOOK()
#endif
#ifndef DM_TEST_CV_OLD_ORDER
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
        lock_handoff(addr); CV_TEST_HOOK();
#else   // t_condvar's negative control: the old order
        lock_handoff(addr); CV_TEST_HOOK();
        st32(key, 1);
        t->wkey = key; t->waddr = addr; t->wtag = (uint32_t)regs[2];
#endif
        int64_t r = block(t, 2, poll_wait_begin((int64_t)regs[3]));   // timeout: the lock is not re-acquired (user mode takes it)
        g_unlock();
        uint64_t wsig = !regs[3] && r == R_TIMEOUT ? fnv_mix(fnv_mix(fnv_mix(0x1c, addr), key), regs[30]) : 0;
        poll_wait_end((int64_t)regs[3], r, regs, wsig);
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x1D: {  // SignalProcessWideKey(x0 key, w1 count, <= 0 all)
        uint64_t key = regs[0]; int cnt = (int)regs[1], n = 0;
        g_lock();
        for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(2, key)); n++) lock_acquire_for(w, w->waddr, w->wtag);   // kernel order; a lock wait or a woken one is no longer kind 2
        int left = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 2 && w->wkey == key) { left = 1; break; }
        if (!left) st32(key, 0);
        g_unlock();
        regs[0] = 0; return 1;
    }

    case 0x34: {  // WaitForAddress(x0 address, w1 type, x2 value, x3 timeout): 0 wait if less, 1 decrement if less then wait, 2 wait if equal, 3 wait if 64-bit equal
        // type 3: Horizon 18, used by SDK 19's nn::os::WaitForAddress(const long *, long)
        thr_t *t = cur(); uint64_t addr = regs[0]; int type = (int)regs[1]; int32_t val = (int32_t)regs[2];
        g_lock();
        int64_t r; uint32_t v = ld32(addr);
        int ok = type == 3 ? ld64(addr) == regs[2] : type == 2 ? (int32_t)v == val : (int32_t)v < val;
        if (ok && type == 1) { while (!cas32(addr, &v, v - 1)) if (!((int32_t)v < val)) { ok = 0; break; } }
        if (!ok) r = R_BAD_STATE;
        else { t->waddr = addr; r = block(t, 3, poll_wait_begin((int64_t)regs[3])); }
        g_unlock();
        if (ok) {
            uint64_t wsig = !regs[3] && r == R_TIMEOUT ? fnv_mix(fnv_mix(fnv_mix(fnv_mix(0x34, addr), (uint64_t)type), regs[2]), regs[30]) : 0;
            poll_wait_end((int64_t)regs[3], r, regs, wsig);
        }
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x35: {  // SignalToAddress(x0 address, w1 type, w2 value, w3 count): 0 wake, 1 increment if equal and wake, 2 modify by waiter count if equal and wake
        uint64_t addr = regs[0];
        int type = (int)regs[1];
        int32_t val = (int32_t)regs[2];
        int cnt = (int)regs[3];
        g_lock();
        int nw = 0; for (thr_t *w = g_wait; w; w = w->next) if (w->wkind == 3 && w->waddr == addr) nw++;
        int64_t r = 0;
        if (type != 0) {
            int32_t nv = val + 1;
            if (type == 2) {
                // Waking all with waiters subtracts 2, not 1 (mesosphere): job fences read it (docs/reference/threads.md#signaltoaddress-type-2-with-count--0)
                if (cnt <= 0) nv = nw > 0 ? val - 2 : val + 1;
                else nv = nw == 0 ? val + 1 : nw <= cnt ? val - 1 : val;
            }
            uint32_t e = (uint32_t)val;
            if (!cas32(addr, &e, (uint32_t)nv)) r = R_BAD_STATE;
        }
        if (!r) { int n = 0;
            for (thr_t *w; (cnt <= 0 || n < cnt) && (w = pick(3, addr)); n++) wake(w, 0); }
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }

    case 0x45: {  // CreateEvent(→w1 write end, w2 read end)
        event_t *e = calloc(1, sizeof *e); e->refs = 2;
        g_lock(); uint32_t hw = h_new(O_EVENT_W, e), hr = h_new(O_EVENT_R, e); g_unlock();
        regs[0] = 0; regs[1] = hw; regs[2] = hr; return 1;
    }
    case 0x11: case 0x12: case 0x17: {  // SignalEvent(write end) / ClearEvent(either end) / ResetSignal(read end, error if not signaled)
        g_lock();
        uint32_t h = (uint32_t)regs[0];
        event_t *e = num == 0x11 ? h_peek(h, O_EVENT_W) : num == 0x17 ? h_peek(h, O_EVENT_R)
                   : (h_peek(h, O_EVENT_W) ? h_peek(h, O_EVENT_W) : h_peek(h, O_EVENT_R));
        int64_t r = 0;
        if (!e) { r = R_BAD_HANDLE; LOG("event SVC %#x invalid handle %#x", num, h); }
        else if (num == 0x11) { e->signaled = 1; wake_sync_waiters(e); }
        else if (num == 0x17 && !e->signaled) r = R_BAD_STATE;
        else e->signaled = 0;
        g_unlock();
        regs[0] = (uint64_t)r; return 1;
    }
    case 0x18: {  // WaitSynchronization(→w1 index; x1 handles, w2 count, x3 timeout)
        thr_t *t = cur(); int n = (int)regs[2]; int64_t ns = (int64_t)regs[3];
        if (n < 0 || n > MAX_SYNC_OBJECTS) { regs[0] = R_OUT_OF_RANGE; regs[1] = 0; return 1; }
        int64_t wait_ns = poll_wait_begin(ns);
        uint32_t hs[MAX_SYNC_OBJECTS]; sync_ref_t *objects = t->wobjs;
        if (n) memcpy(hs, (const void *)regs[1], n * sizeof *hs);
        g_lock();
        int held = 0, idx = -1; int64_t r = R_BAD_HANDLE;
        while (held < n && sync_open(hs[held], &objects[held])) held++;
        if (held == n) {
            for (int i = 0; i < n; i++) if (sync_signaled(objects[i])) { idx = i; break; }
            // ready objects, then a zero-time poll, come before a pending cancellation
            if (idx >= 0) r = 0;
            else if (!ns) r = R_TIMEOUT;
            else if (t->cancel) { t->cancel = 0; r = R_CANCELLED; }
            else {
                t->wn = n; t->widx = -1;
                r = block(t, 4, wait_ns); idx = t->widx;
                t->wn = 0;
            }
        }
        for (int i = 0; i < held; i++) sync_close(objects[i]);
        g_unlock();
        uint64_t wsig = 0;
        if (!ns && r == R_TIMEOUT) {
            wsig = fnv_mix(fnv_mix(0x18, (uint64_t)n), regs[30]);
            for (int i = 0; i < n; i++) wsig = fnv_mix(wsig, hs[i]);
        }
        poll_wait_end(ns, r, regs, wsig);
        regs[0] = (uint64_t)r; regs[1] = idx >= 0 ? (uint64_t)idx : 0; return 1;
    }
    case 0x19: {  // CancelSynchronization(w0 thread)
        g_lock();
        thr_t *t = h_get((uint32_t)regs[0], O_THREAD);
        if (t) { if (t->wkind == 4) wake(t, R_CANCELLED); else t->cancel = 1; }
        g_unlock();
        regs[0] = t ? 0 : R_BAD_HANDLE; return 1;
    }
    case 0x16: {  // CloseHandle(w0); synchronization waits keep their objects
        g_lock();
        unsigned i = (uint32_t)regs[0] - 0x1000;
        if (i > 0 && i < MAX_H && (g_h[i].type == O_EVENT_R || g_h[i].type == O_EVENT_W)) {
            if (g_h[i].type == O_EVENT_W) se_unbind((uint32_t)regs[0]);
            event_t *e = g_h[i].obj; if (--e->refs == 0) free(e);
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_THREAD) {
            thr_t *t = g_h[i].obj;
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
            t->closed = 1;
            if (!t->started) pthread_cond_broadcast(&t->cv);
            if (t->exited && !t->refs) thr_free(t);
        } else if (i > 0 && i < MAX_H && g_h[i].type == O_TRANSFER_MEMORY) {
            g_h[i].type = O_FREE; g_h[i].obj = NULL;
        }
        g_unlock();
        regs[0] = 0; return 1;
    }
    }
    return 0;
}

uint32_t hle_new_transfer_memory(void) {
    g_lock(); uint32_t h = h_try_new(O_TRANSFER_MEMORY, NULL); g_unlock();
    return h;
}

// The SDK's own CreateSystemEvent(inter-process) on a guest thread makes the kernel event (SVC 0x45); its write end is kept so any thread can signal it.
void hle_bind_system_event(void *ev, int autoclear) {      // guest thread only: runs SDK code
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) return;
    if (!pthread_getspecific(g_keythr)) die("hle_bind_system_event must be called on a guest thread");
    static uint32_t (*create)(void *, int, int); static uint32_t (*getw)(const void *);
    if (!create) { create = guest_sym("_ZN2nn2os17CreateSystemEventEPNS0_15SystemEventTypeENS0_14EventClearModeEb");
                   getw = guest_sym("_ZN2nn2os30GetWritableHandleOfSystemEventEPKNS0_15SystemEventTypeE"); }
    uint32_t r = create(ev, autoclear, 1);
    if (r) die("CreateSystemEvent failed %#x", r);
    wh = getw(ev);
    pthread_mutex_lock(&g_se_lock);
    if (g_nse >= (int)(sizeof g_se / sizeof g_se[0])) die("SystemEvent binding table full");
    g_se[g_nse].ev = ev; g_se[g_nse].wh = wh; g_se[g_nse++].id = g_next_se_id++;
    pthread_mutex_unlock(&g_se_lock);
}
uint32_t hle_dup_readable_handle(uint32_t h) {   // each end closes on its own; 0 if h is no read end
    g_lock();
    event_t *e = h_get(h, O_EVENT_R); uint32_t r = 0;
    if (e) { e->refs++; r = h_new(O_EVENT_R, e); }
    g_unlock();
    return r;
}
uint64_t hle_system_event_binding(void *ev) {
    uint64_t id = 0;
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; i++) if (g_se[i].ev == ev) { id = g_se[i].id; break; }
    pthread_mutex_unlock(&g_se_lock);
    return id;
}
int hle_signal_system_event_binding(uint64_t binding) {
    // lifetime match and signal under G, as CloseHandle: a stale producer cannot signal a reused number
    g_lock();
    uint32_t wh = 0;
    pthread_mutex_lock(&g_se_lock);
    for (int i = 0; i < g_nse; i++) if (g_se[i].id == binding) { wh = g_se[i].wh; break; }
    pthread_mutex_unlock(&g_se_lock);
    event_t *e = wh ? h_get(wh, O_EVENT_W) : NULL;
    if (e) { e->signaled = 1; wake_sync_waiters(e); }
    g_unlock();
    return e != NULL;
}
void hle_signal_system_event(void *ev) {   // an unbound one is bound on the spot as manual-clear (guest threads only)
    for (int bound = 0; ; bound = 1) {
        g_lock();
        pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
        if (wh) {
            event_t *e = h_get(wh, O_EVENT_W);
            if (!e) die("SystemEvent write-end handle invalid %#x", wh);
            e->signaled = 1; wake_sync_waiters(e);
            g_unlock();
            return;
        }
        g_unlock();
        if (bound) die("SystemEvent %p still not found after binding", ev);
        // a host thread signalling an event the game destroyed: binding needs a guest thread; skipped
        if (!pthread_getspecific(g_keythr)) {
            static _Atomic int once;
            if (!atomic_exchange(&once, 1)) LOG("[RED] SystemEvent %p signalled off a guest thread after the game destroyed it: skipped", ev);
            return;
        }
        hle_bind_system_event(ev, 0);   // runs guest code (SVCs take G): outside the lock
    }
}
void hle_clear_system_event(void *ev) {   // an unbound one has nothing to clear
    g_lock();
    pthread_mutex_lock(&g_se_lock); uint32_t wh = se_find(ev); pthread_mutex_unlock(&g_se_lock);
    if (wh) {
        event_t *e = h_get(wh, O_EVENT_W);
        if (!e) die("SystemEvent write-end handle invalid %#x", wh);
        e->signaled = 0;
    }
    g_unlock();
}
