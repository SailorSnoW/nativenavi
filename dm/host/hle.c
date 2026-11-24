// Import resolution (host implementations or named stubs) and host gate dispatch.
#include "hle.h"
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <dlfcn.h>
#include <time.h>
#include <stdatomic.h>

void *hlecount_wrap(const char *, const char *, void *);

#define MAX_STUB 4096                 // = entry.S .rept count
static unsigned g_nstub;
static const char *g_stub_name[MAX_STUB];
static uint8_t g_stub_seen[MAX_STUB];
extern uint64_t unimpl_stubs[MAX_STUB];

uint64_t unimpl_c(unsigned idx, uint64_t *regs) {
    if (!g_stub_seen[idx]) {
        g_stub_seen[idx] = 1;
        LOG("UNIMPL %s (x0=%#llx x1=%#llx x2=%#llx)", g_stub_name[idx], regs[0], regs[1], regs[2]);
    }
    return 0;
}

// Startup resolution is single-threaded.
static uint32_t name_h(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}
static unsigned *g_hle_idx, g_hle_mask;
static const hle_ent *g_hle_sec;
static const hle_ent *hle_find(const char *name) {
    if (!g_hle_mask) {
        unsigned long secsz;
        g_hle_sec = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swhle", &secsz);
        unsigned n = g_hle_sec ? (unsigned)(secsz / sizeof *g_hle_sec) : 0;
        unsigned cap = 16;
        while (cap < n * 2) cap <<= 1;
        g_hle_idx = calloc(cap, sizeof *g_hle_idx);
        g_hle_mask = cap - 1;
        for (unsigned i = 0; i < n; i++)   // the first duplicate wins
            for (unsigned j = name_h(g_hle_sec[i].name) & g_hle_mask;; j = (j + 1) & g_hle_mask) {
                if (!g_hle_idx[j]) { g_hle_idx[j] = i + 1; break; }
                if (!strcmp(g_hle_sec[g_hle_idx[j] - 1].name, g_hle_sec[i].name)) break;
            }
    }
    if (!g_hle_sec) return NULL;
    for (unsigned j = name_h(name) & g_hle_mask; g_hle_idx[j]; j = (j + 1) & g_hle_mask)
        if (!strcmp(g_hle_sec[g_hle_idx[j] - 1].name, name)) return &g_hle_sec[g_hle_idx[j] - 1];
    return NULL;
}

uint64_t resolve_import(module_t *m, const char *name) {
    const hle_ent *e = hle_find(name);
    if (e) {
        if (g_flag.imports) {
            Dl_info di;
            dladdr(e->fn, &di);
            LOG("import %s -> %s", name, di.dli_sname);
        }
        return (uint64_t)hlecount_wrap("HLE", name, e->fn);
    }
    static unsigned *stub_idx, stub_mask;
    if (stub_idx)
        for (unsigned j = name_h(name) & stub_mask; stub_idx[j]; j = (j + 1) & stub_mask)
            if (!strcmp(g_stub_name[stub_idx[j] - 1], name)) return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[stub_idx[j] - 1]);
    if (g_nstub >= MAX_STUB) die("stub overflow");
    unsigned k = g_nstub++;
    g_stub_name[k] = name;
    if (!stub_idx || (unsigned)g_nstub * 2 > stub_mask + 1) {
        unsigned cap = stub_idx ? (stub_mask + 1) * 2 : 512;
        free(stub_idx);
        stub_idx = calloc(cap, sizeof *stub_idx);
        stub_mask = cap - 1;
        for (unsigned i = 0; i < (unsigned)g_nstub; i++)
            for (unsigned j = name_h(g_stub_name[i]) & stub_mask;; j = (j + 1) & stub_mask) {
                if (!stub_idx[j]) { stub_idx[j] = i + 1; break; }
                if (!strcmp(g_stub_name[stub_idx[j] - 1], g_stub_name[i])) break;
            }
    } else
        for (unsigned j = name_h(name) & stub_mask;; j = (j + 1) & stub_mask)
            if (!stub_idx[j]) { stub_idx[j] = k + 1; break; }
    (void)m;
    return (uint64_t)hlecount_wrap("STUB", name, (void *)&unimpl_stubs[k]);
}

int hle_implements(const char *name) { return hle_find(name) != NULL; }

void stubs_report(void) { LOG("stubs=%u", g_nstub); }

// Keyed by the gate block's return point (absolute address).
enum { G_SVC, G_CNTPCT, G_CNTVCT, G_CNTFRQ, G_OBS, G_SPIN };
static const char *const k_gate_kind[] = {[G_SVC] = "svc", [G_CNTPCT] = "cntpct", [G_CNTVCT] = "cntvct", [G_CNTFRQ] = "cntfrq", [G_OBS] = "obs", [G_SPIN] = "spin"};
static void (*g_obs_fn[OBS_MAX])(uint64_t *regs, int param);
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param)) {
    if (id <= 0 || id >= OBS_MAX) die("observation gate number out of range %d", id);
    g_obs_fn[id] = fn;
}
typedef struct { uint64_t ret; int kind, arg; uint64_t orig; } gate_t;
#define GATE_BITS 15                     // ≤ 8192 gates at load factor ≤ 1/4
static gate_t g_gate[1u << GATE_BITS];
static int g_ngate;
static inline unsigned gate_h(uint64_t a) { return (unsigned)(((a >> 2) * 0x9E3779B97F4A7C15ull) >> (64 - GATE_BITS)); }

void gate_register(uint64_t ret_abs, const char *kind, int arg, uint64_t orig) {
    int k = -1;
    for (int i = 0; i < (int)(sizeof k_gate_kind / sizeof *k_gate_kind); i++)
        if (!strcmp(kind, k_gate_kind[i])) { k = i; break; }
    if (k < 0) die("unknown gate kind %s", kind);
    if (k == G_OBS && ((arg & 0xff) <= 0 || (arg & 0xff) >= OBS_MAX)) die("observation gate number out of range %d", arg & 0xff);
    if (++g_ngate > (1 << GATE_BITS) / 4) die("gate overflow");
    unsigned i = gate_h(ret_abs);
    while (g_gate[i].ret) {
        if (g_gate[i].ret == ret_abs) die("gate registered twice %#llx", ret_abs);
        i = (i + 1) & ((1u << GATE_BITS) - 1);
    }
    g_gate[i] = (gate_t){ret_abs, k, arg, orig};
}

uint64_t svc_dispatch(int num, uint64_t *regs, uint64_t orig_addr);
// The gate frame is a paused thread's context.
void thr_svc_enter(uint64_t *regs, uint64_t pc);
void thr_svc_leave(void);
void dm_spin_wait(uint64_t gate, uint64_t site);

// dm_gate does not restore x18: the guest's x18 is the thread slot K18.
static void put_x(uint64_t *regs, int n, uint64_t v) {
    if (n == 18) pthread_setspecific(g_key18, (void *)(uintptr_t)v);
    else regs[n] = v;
}

uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs) {
    // Reset: a thread stops here before the gate acts, or after a wait the reset ended.
    if (__builtin_expect(atomic_load_explicit(&dm_reset_stop, memory_order_relaxed), 0)) dm_reset_gate(regs);
    for (unsigned i = gate_h(ret_addr);; i = (i + 1) & ((1u << GATE_BITS) - 1)) {
        const gate_t *g = &g_gate[i];
        if (g->ret == ret_addr) switch (g->kind) {
            case G_CNTPCT: case G_CNTVCT: {
                guest_clock_read_wait();
                uint64_t t = guest_ticks();    // 31.25 MHz
                put_x(regs, g->arg, t);
                return t;
            }
            case G_CNTFRQ: {   // the guest counter's rate, not the Mac's 24 MHz
                uint64_t f = guest_tick_hz();
                put_x(regs, g->arg, f);
                return f;
            }
            case G_OBS:
                if (g_obs_fn[g->arg & 0xff]) g_obs_fn[g->arg & 0xff](regs, g->arg >> 8);
                return 0;
            case G_SPIN:   // sleeps inside an SVC frame, so a thread paused there has its context
                thr_svc_enter(regs, g->orig);
                dm_spin_wait(ret_addr, g->orig);
                thr_svc_leave();
                return 0;
            default: {
                thr_svc_enter(regs, g->orig);
                uint64_t r = svc_dispatch(g->arg, regs, g->orig);
                thr_svc_leave();
                if (__builtin_expect(atomic_load_explicit(&dm_reset_stop, memory_order_relaxed), 0)) dm_reset_gate(regs);
                return r;
            }
        }
        if (!g->ret) die("unknown gate at %#llx", ret_addr);
    }
}
