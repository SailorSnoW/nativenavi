// Walk the real HLE, libc and NVN registrations without a GPU.
// Build under ASan without a global ignorelist: every producer must preserve its section's array layout.
#include "../../host/hle.h"
#include "../nvn/nvn_mtl.h"
#import <Metal/Metal.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include <time.h>
#include <assert.h>
#if __has_feature(address_sanitizer)
#include <sanitizer/asan_interface.h>
static const char instrumented[1] __attribute__((used)) = {0};
#endif

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
struct dm_flags g_flag;
pthread_key_t g_key18;
void svc_thread_dump(void) {}
int dm_opmode(void) { return 1; }
uint64_t now_ns(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
uint64_t guest_ns(void) { return 0; }
uint64_t guest_ticks(void) { return 0; }
uint64_t guest_tick_hz(void) { return 31250000; }
void guest_clock_read_wait(void) {}
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
uint64_t unimpl_stubs[4096];
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }

HLE("registration_hle", int, registration_hle, void) { return 41; }
HLE_FORCE("registration_force", "registration test", int, registration_force, void) { return 42; }
HLE_FORCE_DIRECT("registration_direct", "registration test", int, registration_direct, void) { return 43; }

// A 24-byte entry also catches accidental alignment stronger than the type needs.
typedef struct { const char *name; uint64_t value; void *ctx; } odd_entry;
DM_REGISTER("swtest", odd_entry, odd_a, {"a", 46, NULL});
DM_REGISTER("swtest", odd_entry, odd_b, {"b", 47, NULL});

static const void *section(const char *name, size_t size, size_t align, unsigned long *count) {
    unsigned long bytes = 0;
    const void *p = getsectiondata(&_mh_execute_header, "__DATA", name, &bytes);
    assert(p && bytes && bytes % size == 0 && (uintptr_t)p % align == 0);
    *count = bytes / size; return p;
}
#define SECTION(name, type, count) section(name, sizeof(type), _Alignof(type), count)

int main(void) {
#if __has_feature(address_sanitizer)
    assert(!__asan_address_is_poisoned(instrumented));
    assert(__asan_address_is_poisoned(instrumented + sizeof instrumented));
#endif
    unsigned long nhle, nforce, nnvn, nodd;
    const hle_ent *hle = SECTION("swhle", hle_ent, &nhle);
    for (unsigned long i = 0; i < nhle; i++) {
        assert(hle[i].name && hle[i].fn && hle_implements(hle[i].name));
        assert((void *)resolve_import(NULL, hle[i].name) == hle[i].fn);
    }
    assert(nhle);   // at least this file's own entry: the HLE hooks of dm/host are not linked here
    assert(((int (*)(void))resolve_import(NULL, "registration_hle"))() == 41);
    void *(*copy)(void *, const void *, size_t) = (void *)resolve_import(NULL, "memcpy");
    char dest[4]; assert(copy(dest, "abc", 4) == dest && !strcmp(dest, "abc"));

    const hle_force_ent *force = SECTION("swforce", hle_force_ent, &nforce);
    void *(*bootstrap)(const char *) = NULL;
    unsigned found = 0;
    for (unsigned long i = 0; i < nforce; i++) {
        const hle_force_ent *e = force + i;
        assert(e->name && e->why && e->fn);
        if (!strcmp(e->name, "registration_force")) { assert(((int (*)(void))e->fn)() == 42); found++; }
        if (!strcmp(e->name, "registration_direct")) { assert(((int (*)(void))e->fn)() == 43); found++; }
        if (!strcmp(e->name, "nvnBootstrapLoader")) bootstrap = e->fn;
    }
    assert(found == 2 && bootstrap);
    // NVN's private entry type has the same {name, fn} layout; copy its bytes rather than aliasing it as another struct type.
    const unsigned char *nvn = SECTION("swnvn", hle_ent, &nnvn);
    assert(nnvn > 100);
    for (unsigned long i = 0; i < nnvn; i++) {
        hle_ent entry; memcpy(&entry, nvn + i * sizeof entry, sizeof entry);
        assert(entry.name && entry.fn && bootstrap(entry.name));
    }
    const odd_entry *odd = SECTION("swtest", odd_entry, &nodd);
    assert(nodd == 2 && odd[0].value + odd[1].value == 93);
    printf("registrations: %lu HLE, %lu forced, %lu NVN, %lu 24-byte entries: OK\n", nhle, nforce, nnvn, nodd);
    return 0;
}
