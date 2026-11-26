// Crash state (registers as module offsets) and page fault hooks; the graphics unit tests link this file alone.
#include "dm.h"
#include <signal.h>
#include <dlfcn.h>
#include <sys/ucontext.h>
#include <unistd.h>
#include <sys/mman.h>
#include <libkern/OSCacheControl.h>
#include <mach/mach.h>
#include <stdatomic.h>
#include <time.h>
#include <crt_externs.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>

static _Atomic uint64_t g_watch_last_ns;
static unsigned g_watch_stall;
static void (*g_dump_hook)(void);
static void (*g_wait_dump)(void);
static _Atomic int g_ending;
int crash_ending(void) { return atomic_load_explicit(&g_ending, memory_order_relaxed); }
void crash_end_begin(void) { atomic_store(&g_ending, 1); }

static uint64_t watch_now_ns(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
void crash_watch_frame(void) { atomic_store(&g_watch_last_ns, watch_now_ns()); }
static int watch_sample(const char *path) {
    char pid[32];
    snprintf(pid, sizeof pid, "%d", (int)getpid());
    char *argv[] = {"/usr/bin/sample", pid, "1", "-f", (char *)path, NULL};
    posix_spawn_file_actions_t actions;
    int error = posix_spawn_file_actions_init(&actions);
    if (error) return -1;
    error = posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", O_WRONLY, 0);
    if (!error) error = posix_spawn_file_actions_adddup2(&actions, STDOUT_FILENO, STDERR_FILENO);
    pid_t child;
    if (!error) error = posix_spawn(&child, argv[0], &actions, NULL, argv, *_NSGetEnviron());
    posix_spawn_file_actions_destroy(&actions);
    if (error) return -1;
    int status;
    pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    return waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0 ? 0 : -1;
}
static void *watchdog_main(void *arg) {
    (void)arg;
    for (;;) {
        usleep(250000);
        // last before now: the other order can make now - last underflow
        uint64_t last = atomic_load(&g_watch_last_ns);
        uint64_t now = watch_now_ns();
        if (last > now) continue;
        if (g_watch_stall && now - last >= (uint64_t)g_watch_stall * 1000000000ull) {
            crash_end_begin();
            LOG("[RED] watchdog: no frame presented for %llu s", (unsigned long long)((now - last) / 1000000000ull));
            if (g_dump_hook) g_dump_hook();
            if (g_wait_dump) g_wait_dump();
            const char *d = getenv("DM_WATCH_SAMPLE");
            if (d) {
                if (watch_sample(d)) LOG("watchdog: sampling failed");
                else LOG("watchdog: thread stacks written to %s", d);
            }
            _exit(128 + SIGALRM);
        }
    }
    return NULL;
}
void crash_off(const char *tag, uint64_t a) {
    module_t *m = module_of((void *)a);
    Dl_info di;
    if (m) fprintf(stderr, " %s%s%s+%#llx", tag, *tag ? "=" : "", m->name, a - (uint64_t)m->base);
    else if (dladdr((void *)a, &di) && di.dli_sname) fprintf(stderr, " %s%s%s+%#lx", tag, *tag ? "=" : "", di.dli_sname, (unsigned long)(a - (uint64_t)di.dli_saddr));
    else fprintf(stderr, " %s%s%#llx", tag, *tag ? "=" : "", a);
}
// vm_read_overwrite fails instead of faulting: code without frame pointers keeps data in x29
void dm_backtrace(uint64_t fp) {
    fprintf(stderr, "[dm] bt:");
    for (int i = 0; i < 32 && fp && !(fp & 7); i++) {
        uint64_t w[2];
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)fp, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) break;
        uint64_t nfp = w[0], ra = w[1];
        crash_off("", ra);
        if (nfp <= fp) break;
        fp = nfp;
    }
    fputc('\n', stderr);
}
// Called in registration order, 1 = handled; signal handler context: no allocation, no locking.
#define MAX_FAULT_HOOK 4
static int (*g_fault_hook[MAX_FAULT_HOOK])(void *addr, int is_write, void *uc);
static void (*g_write_prep[MAX_FAULT_HOOK])(void *addr, size_t len);
static int g_nfault_hook, g_nwrite_prep;
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc)) {
    if (g_nfault_hook >= MAX_FAULT_HOOK) die("too many page fault hooks");
    g_fault_hook[g_nfault_hook++] = fn;
}
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len)) {
    if (g_nwrite_prep >= MAX_FAULT_HOOK) die("too many host pre-write hooks");
    g_write_prep[g_nwrite_prep++] = fn;
}
// A system call writing a write-protected page returns EFAULT instead of faulting
void dm_guest_write_prepare(void *addr, size_t len) { for (int i = 0; i < g_nwrite_prep; i++) g_write_prep[i](addr, len); }

static int (*g_trap_hook)(ucontext_t *uc);
void crash_set_trap_hook(int (*fn)(ucontext_t *uc)) { g_trap_hook = fn; }
void crash_set_dump_hook(void (*fn)(void)) { g_dump_hook = fn; }
void crash_set_wait_dump(void (*fn)(void)) { g_wait_dump = fn; }
void crash_dump_now(const char *why) {
    fprintf(stderr, "[dm] state dump (%s)\n", why);
    if (g_dump_hook) g_dump_hook();
    if (g_wait_dump) g_wait_dump();
}
// Only the first crashing thread prints; later ones pause(), and a re-entry (nested crash, 15 s alarm) exits.
static _Atomic uintptr_t g_crash_owner;
static void crash_gate(int sig) {
    uintptr_t me = (uintptr_t)pthread_self(), none = 0;
    if (atomic_compare_exchange_strong(&g_crash_owner, &none, me)) {
        crash_end_begin();
        alarm(15);
        return;
    }
    if (none == me) _exit(128 + sig);
    for (;;) pause();
}
__attribute__((weak)) void dm_gfx_addr_note(uint64_t a, char *note, size_t n) { (void)a; if (n) note[0] = 0; }
__attribute__((weak)) void dm_vm_census(FILE *f) { (void)f; }
static void on_crash(int sig, siginfo_t *si, void *uc_) {
    ucontext_t *uc = uc_;
    if (sig == SIGTRAP && g_trap_hook && g_trap_hook(uc)) return;
    if (sig == SIGSEGV || sig == SIGBUS) {
        int is_write = (int)((uc->uc_mcontext->__es.__esr >> 6) & 1);   // ESR.WnR
        for (int i = 0; i < g_nfault_hook; i++) if (g_fault_hook[i](si->si_addr, is_write, uc)) return;
    }
    crash_gate(sig);
    // Another thread may hold the stderr lock: after 1 s, one write() line and exit (file locks are recursive)
    for (int i = 0; ftrylockfile(stderr); i++) {
        if (i < 100) {
            usleep(10000);
            continue;
        }
        char b[256];
        int n = snprintf(b, sizeof b, "[dm] CRASH sig=%d (stderr held for 1 s, no dump)", sig);
        uint64_t at[2] = {uc->uc_mcontext->__ss.__pc, uc->uc_mcontext->__ss.__lr};
        for (int k = 0; k < 2; k++) {
            module_t *m = module_of((void *)at[k]);
            n += m ? snprintf(b + n, sizeof b - n, " %s=%s+%#llx", k ? "lr" : "pc", m->name, at[k] - (uint64_t)m->base) : snprintf(b + n, sizeof b - n, " %s=%#llx", k ? "lr" : "pc", at[k]);
        }
        n += snprintf(b + n, sizeof b - n, "\n");
        if (write(2, b, (size_t)n) < 0) {}
        _exit(128 + sig);
    }
    funlockfile(stderr);
    fprintf(stderr, "[dm] CRASH sig=%d", sig);
    crash_off("addr", (uint64_t)si->si_addr);
    crash_off("pc", uc->uc_mcontext->__ss.__pc);
    crash_off("lr", uc->uc_mcontext->__ss.__lr);
    fprintf(stderr, " sp=%#llx\n", uc->uc_mcontext->__ss.__sp);
    for (int i = 0; i < 29; i++) fprintf(stderr, "x%d=%#llx%s", i, uc->uc_mcontext->__ss.__x[i], i % 6 == 5 ? "\n" : " ");
    fprintf(stderr, "\n");
    for (int i = 0; i < 29; i++) {
        uint64_t a = uc->uc_mcontext->__ss.__x[i], w[6];
        vm_size_t got = 0;
        if (a < 0x100000000ull || (a & 7) || vm_read_overwrite(mach_task_self(), (vm_address_t)a, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) continue;
        fprintf(stderr, "[dm] *x%d:", i);
        for (int k = 0; k < 6; k++) crash_off("", w[k]);
        fputc('\n', stderr);
    }
    for (int i = 0; i < 29; i++) {
        uint64_t a = uc->uc_mcontext->__ss.__x[i];
        char note[512];
        note[0] = 0;
        if (a < 0x100000000ull) continue;
        dm_gfx_addr_note(a, note, sizeof note);
        if (note[0]) fprintf(stderr, "[dm] x%d %#llx:%s\n", i, (unsigned long long)a, note);
    }
    // DM_CRASH_PEEK="sp+0x248*,x22-0x20": 0x100 bytes at sp or a register plus an offset; * dereferences first
    if (getenv("DM_CRASH_PEEK")) {
        char spec[512];
        snprintf(spec, sizeof spec, "%s", getenv("DM_CRASH_PEEK"));
        for (char *tok = strtok(spec, ","); tok; tok = strtok(NULL, ",")) {
            uint64_t a = 0;
            char *p = tok;
            if (!strncmp(p, "sp", 2)) {
                a = uc->uc_mcontext->__ss.__sp;
                p += 2;
            } else if (*p == 'x') {
                long r = strtol(p + 1, &p, 10);
                if (r < 0 || r > 28) continue;
                a = uc->uc_mcontext->__ss.__x[r];
            } else {
                continue;
            }
            if (*p == '+' || *p == '-') {
                long long d = strtoll(p, &p, 0);
                a += (uint64_t)d;
            }
            if (*p == '*') {
                uint64_t v = 0;
                vm_size_t got = 0;
                if (vm_read_overwrite(mach_task_self(), (vm_address_t)a, 8, (vm_address_t)&v, &got) != KERN_SUCCESS || got != 8) {
                    fprintf(stderr, "[dm] peek %s: %#llx unreadable\n", tok, (unsigned long long)a);
                    continue;
                }
                a = v;
            }
            uint64_t w[32];
            vm_size_t got = 0;
            char note[512];
            note[0] = 0;
            if (vm_read_overwrite(mach_task_self(), (vm_address_t)a, sizeof w, (vm_address_t)w, &got) != KERN_SUCCESS || got != sizeof w) {
                fprintf(stderr, "[dm] peek %s: %#llx unreadable\n", tok, (unsigned long long)a);
                continue;
            }
            dm_gfx_addr_note(a, note, sizeof note);
            fprintf(stderr, "[dm] peek %s = %#llx:%s", tok, (unsigned long long)a, note);
            for (int k = 0; k < 32; k++) {
                if (!(k % 4)) fprintf(stderr, "\n[dm]   +%#04x", k * 8);
                crash_off("", w[k]);
            }
            fputc('\n', stderr);
        }
    }
    dm_backtrace(uc->uc_mcontext->__ss.__fp);
    uint64_t sp = uc->uc_mcontext->__ss.__sp;
    fprintf(stderr, "[dm] fp=%#llx  0x100 bytes below sp:", (unsigned long long)uc->uc_mcontext->__ss.__fp);
    for (int i = -32; i < 0 && sp && !(sp & 7); i++) fprintf(stderr, "%s%llx", (i + 32) % 4 ? " " : "\n[dm]   ", (unsigned long long)((uint64_t *)sp)[i]);
    fputc('\n', stderr);
    int nscan = 0;
    fprintf(stderr, "[dm] stack scan sp=%#llx:", (unsigned long long)sp);
    for (int i = -32; i < 512 && nscan < 24 && sp && !(sp & 7); i++) {
        uint64_t v = ((uint64_t *)sp)[i];
        Dl_info di;
        if (v < 0x100000000ull || (v & 3)) continue;
        int mod = -1;
        for (int k = 0; k < g_nmod; k++) {
            uint64_t b = (uint64_t)(uintptr_t)g_mod[k].base;
            if (v >= b && v < b + g_mod[k].size) mod = k;
        }
        if (mod >= 0) {
            fprintf(stderr, " [%+d]%s+%#llx", i * 8, g_mod[mod].name, (unsigned long long)(v - (uint64_t)(uintptr_t)g_mod[mod].base));
            nscan++;
        } else if (dladdr((void *)v, &di) && di.dli_sname && di.dli_fname && strstr(di.dli_fname, "dmhost")) {
            fprintf(stderr, " [%+d]%s+%#llx", i * 8, di.dli_sname, (unsigned long long)(v - (uint64_t)di.dli_saddr));
            nscan++;
        }
    }
    fputc('\n', stderr);
    dm_vm_census(stderr);
    if (g_dump_hook) g_dump_hook();
    _exit(128 + sig);
}
// Per thread: each guest thread installs its own at start; the key's destructor frees it.
static pthread_key_t g_altstack_key;
static pthread_once_t g_altstack_once = PTHREAD_ONCE_INIT;
static void altstack_free(void *p) {
    stack_t off = {.ss_flags = SS_DISABLE};
    sigaltstack(&off, 0);
    free(p);
}
static void altstack_key_init(void) { if (pthread_key_create(&g_altstack_key, altstack_free)) die("altstack key"); }
void crash_thread_init(void) {
    pthread_once(&g_altstack_once, altstack_key_init);
    if (pthread_getspecific(g_altstack_key)) return;
    stack_t ss = {.ss_sp = malloc(65536), .ss_size = 65536};
    sigaltstack(&ss, 0);
    pthread_setspecific(g_altstack_key, ss.ss_sp);
}
void crash_install(void) {
    crash_thread_init();
    struct sigaction sa = {.sa_sigaction = on_crash, .sa_flags = SA_SIGINFO | SA_ONSTACK};
    sigaction(SIGALRM, &sa, 0);
    // Only when DM_WATCH / DM_WATCH_STALL is set: the pause menu stops presenting frames
    const char *watch = getenv("DM_WATCH"), *stall = getenv("DM_WATCH_STALL");
    if (watch) alarm(atoi(watch));
    if (watch || stall) {
        g_watch_stall = stall && atoi(stall) > 0 ? (unsigned)atoi(stall) : 15;
        atomic_store(&g_watch_last_ns, watch_now_ns());
        pthread_t th;
        if (pthread_create(&th, NULL, watchdog_main, NULL)) die("watchdog thread");
        pthread_detach(th);
    }
    sigaction(SIGSEGV, &sa, 0);
    sigaction(SIGBUS, &sa, 0);
    sigaction(SIGILL, &sa, 0);
    sigaction(SIGTRAP, &sa, 0);
    sigaction(SIGABRT, &sa, 0);
}
