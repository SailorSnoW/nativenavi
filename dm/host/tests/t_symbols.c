// Exercise the real HLE, forced-takeover and stub indexes with synthetic Mach-O sections; no game files required.
#include "../hle.h"
#include <mach-o/getsect.h>
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static hle_ent entries[4];
static hle_force_ent forces[3], addons[1];
static hle_force_ent *force_sec = forces; static unsigned long force_sec_n = 3;   // the alias phase swaps in its own table
static int have_sections;
static uint8_t *test_section(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; assert(!strcmp(seg, "__DATA"));
    *size = 0;
    if (!have_sections) return NULL;
    if (!strcmp(sect, "swhle")) { *size = sizeof entries; return (uint8_t *)entries; }
    if (!strcmp(sect, "swaddon")) { *size = sizeof addons; return (uint8_t *)addons; }
    assert(!strcmp(sect, "swforce")); *size = force_sec_n * sizeof *force_sec; return (uint8_t *)force_sec;
}
#define getsectiondata test_section
#include "../hle.c"
#include "../loader.c"
void dm_mods_patch_module(module_t *m, const char *dir) { (void)m; (void)dir; }
#undef getsectiondata

struct dm_flags g_flag;
uint64_t unimpl_stubs[MAX_STUB];
uint64_t now_ns(void) { return 0; }
uint64_t guest_ns(void) { return 0; }
uint64_t guest_ticks(void) { return 0; }
uint64_t guest_tick_hz(void) { return 31250000; }
void guest_clock_read_wait(void) {}
void hlecount_seal(void) {}
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
static const char *wrap_kind, *wrap_name;
static unsigned wraps;
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    wrap_kind = kind; wrap_name = name; wraps++; return fn;
}

static void reset_sections(void) {
    free(g_hle_idx); g_hle_idx = NULL; g_hle_mask = 0; g_hle_sec = NULL;
    free(g_fidx); g_fidx = NULL; g_fmask = 0;
    free(g_force_hits); g_force_hits = NULL;
}

// Failures run in a child, so duplicate rejection and the hard stub limit are checked without killing the suite.
static void expect_abort(int duplicate) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
        freopen("/dev/null", "w", stderr);
        if (duplicate == 2) addon_init();
        else if (duplicate) { forces[2] = forces[0]; force_init(); }
        else resolve_import(NULL, "one_more_missing_symbol");
        _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGABRT);
}

int main(void) {
    // Empty/missing sections must still give misses after lazy initialization.
    assert(!hle_implements("absent")); assert(!hle_implements("absent"));
    force_init(); assert(!force_lookup("absent")); reset_sections();

    // Three different names in the same initial bucket exercise probing and a duplicate behind a collision.
    char names[3][32]; unsigned found = 0;
    for (unsigned i = 0; found < 3; i++) {
        char candidate[32]; snprintf(candidate, sizeof candidate, "symbol_%u", i);
        if ((name_h(candidate) & 15) == 15) strcpy(names[found++], candidate);
    }
    int values[4];
    entries[0] = (hle_ent){names[0], &values[0]};
    entries[1] = (hle_ent){names[1], &values[1]};
    entries[2] = (hle_ent){names[1], &values[2]};
    entries[3] = (hle_ent){names[2], &values[3]};
    for (unsigned i = 0; i < 3; i++) forces[i] = (hle_force_ent){names[i], &values[i], "test"};
    have_sections = 1; force_init();
    for (unsigned i = 0; i < 3; i++) {
        unsigned before = wraps;
        assert(hle_implements(names[i]) && wraps == before);
        uint64_t want = (uint64_t)&values[i == 2 ? 3 : i];
        assert(resolve_import(NULL, names[i]) == want);
        assert(!strcmp(wrap_kind, "HLE") && !strcmp(wrap_name, names[i]));
        assert(force_lookup(names[i]) == &values[i]);
        assert(!strcmp(wrap_kind, "FORCE") && !strcmp(wrap_name, names[i]));
        assert(g_force_hits[i] == 1);
    }
    assert(!hle_implements("absent") && !force_lookup("absent") && !g_nstub);
    assert(force_lookup(names[0]) == &values[0] && g_force_hits[0] == 2);
    expect_abort(1);

    // An add-on takeover binds for an add-on module's import only: the game's module that imports the same name gets what it got without it (a stub here)
    int addon_value; addons[0] = (hle_force_ent){"addon_only", &addon_value, "test"}; addon_init();
    static const char strtab[] = "\0addon_only\0";
    Elf64_Sym sym = {.st_name = 1, .st_info = 1 << 4};   // global, undefined
    module_t game = {.strtab = strtab}, addon = {.strtab = strtab, .addon = 1}; int other, weak0;
    assert(bind_sym(&addon, &sym, &other, &weak0) == (uint64_t)&addon_value && !strcmp(wrap_kind, "ADDON") && g_addon_hits == 1);
    assert(bind_sym(&game, &sym, &other, &weak0) == (uint64_t)&unimpl_stubs[0] && !strcmp(wrap_kind, "STUB") && g_addon_hits == 1);
    addons[0] = forces[0]; expect_abort(2);   // a name that is both a forced and an add-on takeover

    // Fill through every growth boundary, then query in reverse using different pointers to the same names.
    static char missing[MAX_STUB][32];
    for (unsigned i = 1; i < MAX_STUB; i++) {   // stub 0 is the add-on test's
        snprintf(missing[i], sizeof missing[i], "missing_%u", i);
        assert(resolve_import(NULL, missing[i]) == (uint64_t)&unimpl_stubs[i]);
        assert(!strcmp(wrap_kind, "STUB") && !strcmp(wrap_name, missing[i]));
    }
    for (unsigned i = MAX_STUB; i-- > 1;) {
        char copy[32]; strcpy(copy, missing[i]);
        assert(resolve_import(NULL, copy) == (uint64_t)&unimpl_stubs[i]);
    }
    assert(g_nstub == MAX_STUB); expect_abort(0);
    reset_sections();

    puts("Symbol indexes: collisions, duplicate precedence/rejection, add-on takeovers, misses, wrappers and 4096 reusable stubs: OK");
    return 0;
}
