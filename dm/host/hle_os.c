// Guest object side table, guest clock, entropy and getenv.
#include <pthread.h>
#include "hle.h"
#include <mach/mach_time.h>
#include <stdatomic.h>

// Linear probing with backward-shift deletion: an empty slot ends every search. Keys are never NULL.
// dtor frees a host-allocated value at a reset (dm_objmap_reset): the game's own Finalize calls never run then.
#define OM_SIZE (1u << 18)
#define OM_MASK (OM_SIZE - 1)
static struct { const void *k; void *v; void (*dtor)(void *); } g_om[OM_SIZE];
static pthread_mutex_t g_om_lock = PTHREAD_MUTEX_INITIALIZER;
static unsigned om_h(const void *k) {
    uint64_t x = (uint64_t)k;
    x ^= x >> 29;
    x *= 0xbf58476d1ce4e5b9ull;
    return (unsigned)(x >> 40) & OM_MASK;
}
// g_om_lock held
static unsigned om_find(const void *k) {
    unsigned i = om_h(k);
    while (g_om[i].k && g_om[i].k != k) i = (i + 1) & OM_MASK;
    return i;
}
void *objmap_get(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    void *r = g_om[om_find(k)].v;
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
void objmap_put_d(const void *k, void *v, void (*dtor)(void *)) {
    pthread_mutex_lock(&g_om_lock);
    unsigned i = om_find(k);
    g_om[i].k = k;
    g_om[i].v = v;
    g_om[i].dtor = dtor;
    pthread_mutex_unlock(&g_om_lock);
}
void objmap_put(const void *k, void *v) { objmap_put_d(k, v, NULL); }
void *objmap_del(const void *k) {
    pthread_mutex_lock(&g_om_lock);
    unsigned i = om_find(k);
    void *r = g_om[i].v;
    if (g_om[i].k) {
        for (unsigned j = (i + 1) & OM_MASK; g_om[j].k; j = (j + 1) & OM_MASK)
            if (((j - om_h(g_om[j].k)) & OM_MASK) >= ((j - i) & OM_MASK)) {
                g_om[i] = g_om[j];
                i = j;
            }
        g_om[i].k = NULL;
        g_om[i].v = NULL;
        g_om[i].dtor = NULL;
    }
    pthread_mutex_unlock(&g_om_lock);
    return r;
}
uint64_t now_ns(void) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    return mach_absolute_time() * tb.numer / tb.denom;
}
// Every guest-readable system counter goes through here. Frozen, it does not advance, and the frozen time is subtracted after.
// DM_DETCLOCK=<step ns>: a fixed step per present from the first one on (the wall clock before: boot polls by time).
static _Atomic uint64_t g_gfrozen, g_glag, g_gdet, g_guest_frame;
static _Atomic int g_gfz, g_gdet_on;
static uint64_t g_gdet_step = 1;
uint64_t dm_guest_frame(void) { return atomic_load(&g_guest_frame); }
static int detclock_cfg(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("DM_DETCLOCK");
        g_gdet_step = e ? strtoull(e, 0, 0) : 0;
        c = g_gdet_step > 0;
    }
    return c;
}
static uint64_t wall_guest_ns(void) { return atomic_load(&g_gfz) ? atomic_load(&g_gfrozen) : now_ns() - atomic_load(&g_glag); }
uint64_t guest_ns(void) { return atomic_load(&g_gdet_on) ? atomic_load(&g_gdet) : wall_guest_ns(); }
int guest_clock_pinned(void) { return atomic_load(&g_gdet_on) || atomic_load(&g_gfz); }
uint64_t guest_ns_at_host_time(uint64_t host_ns) {
    if (atomic_load(&g_gdet_on)) return atomic_load(&g_gdet);
    if (atomic_load(&g_gfz)) return atomic_load(&g_gfrozen);
    uint64_t lag = atomic_load(&g_glag);
    return host_ns >= lag ? host_ns - lag : 0;
}
// docs/reference/frame-rate.md#the-switch-2s-system-tick-is-3125-mhz
#define GUEST_TICK_NS 32
uint64_t guest_ticks(void) { return guest_ns() / GUEST_TICK_NS; }
uint64_t guest_tick_hz(void) { return 1000000000ull / GUEST_TICK_NS; }   // a rewritten mrs cntfrq_el0 reads it
int dm_detclock_enabled(void) { return detclock_cfg() && atomic_load(&g_gdet_on); }
void dm_guest_frame_tick(void) {
    crash_watch_frame();
    atomic_fetch_add(&g_guest_frame, 1);
    if (!detclock_cfg()) return;
    if (!atomic_load(&g_gdet_on)) {
        atomic_store(&g_gdet, wall_guest_ns());
        atomic_store(&g_gdet_on, 1);
        LOG("DM_DETCLOCK: from the first present on, the guest clock advances by presents, step %llu ns", (unsigned long long)g_gdet_step);
        return;
    }
    if (!atomic_load(&g_gfz)) atomic_fetch_add(&g_gdet, g_gdet_step);
}
static void guest_clock_freeze_apply(int on) {
    if (on && !atomic_load(&g_gfz)) {
        atomic_store(&g_gfrozen, now_ns() - atomic_load(&g_glag));
        atomic_store(&g_gfz, 1);
    } else if (!on && atomic_load(&g_gfz)) {
        atomic_store(&g_glag, now_ns() - atomic_load(&g_gfrozen));
        atomic_store(&g_gfz, 0);
    }
}
// 0 tick hold, 1 background, 2 keyboard, 3 NVN prepare, 4 pipeline archive init: frozen while any holds.
static pthread_mutex_t g_gfz_mu = PTHREAD_MUTEX_INITIALIZER;
static unsigned g_gfz_reasons;
// Guest reads wait while one holds (docs/reference/services.md#guest-clock-reads-wait-while-the-app-is-in-the-background); host work reads the frozen clock.
enum { GFZ_WAIT_MASK = (1u << 1) | (1u << 3) | (1u << 4) };
static pthread_cond_t g_gfz_cv = PTHREAD_COND_INITIALIZER;
static _Atomic int g_gfz_wait;
// A run of yields that read the clock is a timed wait: the yield back-off sleeps from its start.
__thread uint64_t dm_clock_reads;
void guest_clock_read_wait(void) {
    dm_clock_reads++;
    if (!atomic_load(&g_gfz_wait) || atomic_load(&dm_host_work)) return;
    pthread_mutex_lock(&g_gfz_mu);
    while ((g_gfz_reasons & GFZ_WAIT_MASK) && !atomic_load(&dm_reset_stop)) pthread_cond_wait(&g_gfz_cv, &g_gfz_mu);   // reset: go on to the gate's end
    pthread_mutex_unlock(&g_gfz_mu);
}
int guest_clock_frozen_for(int reason) {
    pthread_mutex_lock(&g_gfz_mu);
    int on = g_gfz_reasons >> reason & 1;
    pthread_mutex_unlock(&g_gfz_mu);
    return on;
}
void dm_clock_reset_wake(void) {
    pthread_mutex_lock(&g_gfz_mu);
    pthread_cond_broadcast(&g_gfz_cv);
    pthread_mutex_unlock(&g_gfz_mu);
}
void guest_clock_freeze_for(int reason, int on) {
    pthread_mutex_lock(&g_gfz_mu);
    if (on) g_gfz_reasons |= 1u << reason;
    else g_gfz_reasons &= ~(1u << reason);
    guest_clock_freeze_apply(g_gfz_reasons != 0);
    int wait = (g_gfz_reasons & GFZ_WAIT_MASK) != 0;
    atomic_store(&g_gfz_wait, wait);
    if (!wait) pthread_cond_broadcast(&g_gfz_cv);   // after the clock runs again
    pthread_mutex_unlock(&g_gfz_mu);
}
// DM_DETCLOCK: a fixed-seed stream keeps replays byte-for-byte comparable. getentropy, GetInfo RandomEntropy and csrng share it.
static _Atomic uint64_t g_detrng = 0x5357444554524e47ull;
static _Atomic uint64_t g_detrng_n;
uint64_t dm_entropy64(void) {
    if (!detclock_cfg()) return ((uint64_t)arc4random() << 32) | arc4random();
    uint64_t z = atomic_fetch_add(&g_detrng, 0x9E3779B97F4A7C15ull) + 0x9E3779B97F4A7C15ull;
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    z ^= z >> 31;
    uint64_t n = atomic_fetch_add(&g_detrng_n, 1) + 1;
    if (n == 1 || n % 1000 == 0) LOG("DM_DETCLOCK: deterministic entropy call %llu", (unsigned long long)n);
    return z;
}
HLE("getentropy", int, g_getentropy, void *p, size_t n) {
    if (!detclock_cfg()) {
        arc4random_buf(p, n);
        return 0;
    }
    for (size_t i = 0; i < n; i += 8) {
        uint64_t v = dm_entropy64();
        memcpy((uint8_t *)p + i, &v, n - i < 8 ? n - i : 8);
    }
    return 0;
}
// docs/reference/services.md#cryptographically-random-bytes
HLE_FORCE("_ZN2nn6crypto36GenerateCryptographicallyRandomBytesEPvm", "csrng service; host entropy (the getentropy stream)", void, g_csrng_bytes, void *p, size_t n) {
    g_getentropy(p, n);
}

// Horizon gives no environment: the profile's game_env answers its names; others go to the SDK's getenv.
static const char **g_env_kv;   // "KEY=VALUE" strings, NULL-terminated
static void game_env_init(void) {
    const char *e = getenv("DM_GAME_ENV");
    if (!e) e = DM_GAME_ENV;
    int n = 0;
    for (const char *p = e; *p; p++) n += *p == '=';
    if (!(g_env_kv = calloc((size_t)n + 1, sizeof *g_env_kv))) die("game_env allocation failed");
    char *s = strdup(e);
    int k = 0;
    for (char *t = strtok(s, " "); t && k < n; t = strtok(NULL, " "))
        if (strchr(t, '=')) g_env_kv[k++] = t;
    if (k) LOG("game environment (%s): %s", getenv("DM_GAME_ENV") ? "DM_GAME_ENV" : "game profile game_env", e);
    else free(s);   // no accepted entry points into the copy
}
static const char *const *game_env(void) {
    static pthread_once_t once = PTHREAD_ONCE_INIT;
    pthread_once(&once, game_env_init);
    return g_env_kv;
}
HLE_FORCE("getenv", "the game profile's game_env answers its names; the SDK's own getenv (an empty environ on Horizon) the rest", char *, g_getenv, const char *name) {
    static char *(*orig)(const char *);
    size_t n = name ? strlen(name) : 0;
    for (const char *const *p = game_env(); n && *p; p++)
        if (!strncmp(*p, name, n) && (*p)[n] == '=') return (char *)*p + n + 1;
    if (!orig && !(orig = guest_sym("getenv"))) return NULL;
    return orig(name);
}

// Reset: every key left is the stopped game's. A value with a destructor is freed as its Close would have freed it:
// the game's threads are stopped without running the guest's destructors, so its Finalize calls never come.
// The destructors run after the table is cleared and unlocked, so one may use the table itself.
void dm_objmap_reset(void) {
    pthread_mutex_lock(&g_om_lock);
    unsigned n = 0;
    for (unsigned i = 0; i < OM_SIZE; i++)
        if (g_om[i].k) n++;
    struct { void *v; void (*dtor)(void *); } *dt = n ? malloc(n * sizeof *dt) : NULL;
    unsigned m = 0;
    for (unsigned i = 0; i < OM_SIZE; i++)
        if (g_om[i].k && g_om[i].dtor) dt[m++] = (__typeof__(*dt)){g_om[i].v, g_om[i].dtor};
    memset(g_om, 0, sizeof g_om);
    pthread_mutex_unlock(&g_om_lock);
    for (unsigned i = 0; i < m; i++) dt[i].dtor(dt[i].v);
    free(dt);
    if (n) LOG("reset: %u host objects of the game's left in the side table, %u of them freed, the rest forgotten", n, m);
}
