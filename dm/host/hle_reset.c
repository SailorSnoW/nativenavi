// The game's reset (Home > close > start again) inside this process. A guest thread can stop only at a gate or a host-stack entry with
// nothing of the host's below it; if one does not within RESET_STOP_MS, or DM_NVN_CAPTURE is on, the process starts again instead.
#define DM_RESET_IMPL   // not dm.h's weak ones
#include "hle.h"
#include <mach/mach.h>
#include <mach-o/dyld.h>
#include <setjmp.h>
#include <stdatomic.h>
#include <unistd.h>
#include <limits.h>
#include <spawn.h>
#include <dispatch/dispatch.h>

extern char **environ;
_Atomic int dm_reset_stop;            // from the request until the game starts again
uintptr_t dm_reset_main_base;         // the main thread's chain of guest frames ends there
static jmp_buf g_boot_jb;
static _Atomic int g_requested;       // one at a time
static uint64_t g_req_ns;
static char **g_argv;
enum { RESET_STOP_MS = 3000 };
void dm_reset_set_argv(char **argv) { g_argv = argv; }

// read without faulting: a guest frame pointer is the game's to keep
static int rec_read(uint64_t fp, uint64_t rec[2]) {
    vm_size_t got = 0;
    return fp && !(fp & 7) && !vm_read_overwrite(mach_task_self(), (vm_address_t)fp, 16, (vm_address_t)rec, &got) && got == 16;
}
int dm_guest_as_contains(uintptr_t a);
// role 1 worker, 2 main guest thread. A worker's guest stack has only guest code below (host code runs on the big host stack; the game's
// gRPC threads lack frame records); the main thread's chain must reach guest_main's frame through guest code.
int dm_reset_can_end(int role, uint64_t sp, uint64_t fp, uint64_t lr) {
    if (!module_of((void *)(uintptr_t)lr)) return 0;
    if (role == 1) return dm_guest_as_contains((uintptr_t)sp);
    if (role != 2) return 0;
    for (int i = 0; i < 4096; i++) {
        if (fp == dm_reset_main_base) return 1;
        uint64_t rec[2];
        if (!rec_read(fp, rec) || rec[0] <= fp) return 0;
        if (rec[0] == dm_reset_main_base) return 1;
        if (!module_of((void *)(uintptr_t)rec[1])) return 0;
        fp = rec[0];
    }
    return 0;
}
// regs[29] = x29; at regs[99] the gate block's sp, where it pushed the guest's x29 and x30
int dm_reset_gate_can_end(const uint64_t *regs, int role) {
    uint64_t pushed[2];
    return rec_read(regs[99], pushed) && dm_reset_can_end(role, regs[99], regs[29], pushed[1]);
}

int dm_thread_reset_role(void);       // 0 not a guest thread, 1 worker, 2 main guest thread, 3 the host's own
void dm_thread_reset_exit(void);
void dm_thread_reset_main_out(void);
uintptr_t guest_gate_stack_top(void);
// the exit runs key destructors, which need room and free the big host stack: move to the gate stack first
void reset_exit_on_gate_stack(uintptr_t top) __attribute__((noreturn));
__asm__(".text\n.p2align 2\n_reset_exit_on_gate_stack:\n    mov sp, x0\n    mov x29, #0\n    bl _dm_thread_reset_exit\n    brk #0\n");
static void end_here(int role, int on_gate_stack) {
    if (role == 2) {
        dm_thread_reset_main_out();
        _longjmp(g_boot_jb, 1);
    }
    if (on_gate_stack) dm_thread_reset_exit();
    reset_exit_on_gate_stack(guest_gate_stack_top() & ~(uintptr_t)15);
}
void dm_reset_gate(const uint64_t *regs) {
    int role = dm_thread_reset_role();
    if ((role == 1 || role == 2) && dm_reset_gate_can_end(regs, role)) end_here(role, 1);
}
// its frame at sp + 0xa0 holds the guest's x29 and x30
void dm_reset_entry(uintptr_t sp) {
    int role = dm_thread_reset_role();
    const uint64_t *fr = (const uint64_t *)(sp + 0xa0);
    if ((role == 1 || role == 2) && dm_reset_can_end(role, sp, fr[0], fr[1])) end_here(role, 0);
}

// Fallback: this image again, after the graceful exit
long dm_ctl_offset(void);
static void relaunch_exec(void *arg) {
    (void)arg;
    char path[PATH_MAX];
    uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) || !g_argv) {
        LOG("[RED] reset: this host's path or arguments are unknown: it ends instead of starting again");
        return;
    }
    char off[32];
    snprintf(off, sizeof off, "%ld", dm_ctl_offset());
    setenv("DM_CTL_FROM", off, 1);
    LOG("reset: starting %s again", path);
    // only stdio handed over: with every file kept (the Metal caches'), the new image's pipeline prepare stalls
    posix_spawnattr_t at;
    posix_spawn_file_actions_t fa;
    int err;
    if ((err = posix_spawnattr_init(&at)) || (err = posix_spawnattr_setflags(&at, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT)) ||
        (err = posix_spawn_file_actions_init(&fa))) {
        LOG("[RED] reset: posix_spawn setup failed (%d): the host ends", err);
        return;
    }
    for (int fd = 0; fd <= 2; fd++) posix_spawn_file_actions_addinherit_np(&fa, fd);
    err = posix_spawn(NULL, path, &fa, &at, g_argv, environ);
    LOG("[RED] reset: starting %s again failed (%d): the host ends", path, err);
}
static void relaunch(const char *why) {
    static _Atomic int once;   // the deadline and the main thread can both come here: one goes on
    if (atomic_exchange(&once, 1)) for (;;) pause();
    LOG("reset: %s, so the whole process starts again (%.0f ms after the request)", why, (now_ns() - g_req_ns) / 1e6);
    dm_host_shutdown_then("reset", relaunch_exec, NULL);
    for (;;) pause();   // returns only when another exit (SIGTERM) is under way
}
void dm_thread_reset_dump(void);
static void stragglers(const char *what) {
    LOG("[RED] reset: %s within %d ms; the guest threads still running:", what, RESET_STOP_MS);
    dm_thread_reset_dump();
}

void dm_thread_reset_wake(void);
void dm_clock_reset_wake(void);
void dm_oe_reset_wake(void);
void dm_audren_reset_stop(void);
void dm_net_reset_wake(void);
int dm_thread_reset_live(int *main_live);
int guest_clock_frozen_for(int reason);
// The stop's time excludes the prepare step's clock hold (reason 3): the main thread waits in host code there for seconds
static uint64_t g_prep_ns, g_prep_at;
static uint64_t stop_ms(void) {
    uint64_t now = now_ns();
    if (guest_clock_frozen_for(3)) {
        if (g_prep_at) g_prep_ns += now - g_prep_at;
        g_prep_at = now;
    } else {
        g_prep_at = 0;
    }
    return (now - g_req_ns - g_prep_ns) / 1000000;
}
static _Atomic uint64_t g_req_seq, g_back_seq;
// a main thread that does not come back holds the game where nothing here can stop it
static void stop_watch(uint64_t seq) {
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (atomic_load(&g_back_seq) >= seq) return;
        if (stop_ms() < RESET_STOP_MS) { stop_watch(seq); return; }
        stragglers("the main guest thread did not stop");
        relaunch("the game could not be stopped");
    });
}
void dm_reset_request(const char *why) {
    if (atomic_exchange(&g_requested, 1)) {
        LOG("reset: one is under way already (%s ignored)", why);
        return;
    }
    g_req_ns = now_ns();
    g_prep_ns = g_prep_at = 0;
    if (getenv("DM_NVN_CAPTURE")) relaunch("DM_NVN_CAPTURE is on");
    LOG("reset: %s: stopping the game's threads", why);
    uint64_t seq = atomic_fetch_add(&g_req_seq, 1) + 1;
    atomic_store(&dm_reset_stop, 1);
    dm_thread_reset_wake();
    dm_clock_reset_wake();
    dm_oe_reset_wake();
    dm_audren_reset_stop();
    dm_net_reset_wake();
    stop_watch(seq);
}

void dm_gfx_reset(void);
void dm_audio_reset(void);
void dm_audren_reset(void);
void dm_codec_video_reset(void);
void dm_fs_reset(void);
void dm_hid_reset(void);
void dm_oe_reset(void);
void dm_sf_reset(void);
void dm_net_reset(void);
void dm_account_reset(void);
void dm_sdk16_reset(void);
void dm_sdk16_reset_wait(void);
void dm_objmap_reset(void);
void dm_thread_reset_tables(void);
void dm_guest_mem_reset(void);
void loader_reset(void);
void dm_fps_state_apply(void);
static void reset_rest(void) {
    atomic_store(&g_back_seq, atomic_load(&g_req_seq));
    uint64_t t_main = now_ns();
    int live, main_live;
    while ((live = dm_thread_reset_live(&main_live)) > 0) {
        if (stop_ms() > RESET_STOP_MS) {
            stragglers("not every guest thread stopped");
            relaunch("the game could not be stopped");
        }
        usleep(500);
    }
    uint64_t t_stop = now_ns();
    LOG("reset: every guest thread stopped (main thread after %.1f ms, the last one after %.1f ms)", (t_main - g_req_ns) / 1e6, (t_stop - g_req_ns) / 1e6);
    // First what touches the game's memory from host threads, while it is still there; then the tables, the memory, the modules' data
    dm_gfx_reset();
    dm_audio_reset();
    dm_audren_reset();
    dm_codec_video_reset();
    dm_fs_reset();
    dm_hid_reset();
    dm_oe_reset();
    dm_sf_reset();
    dm_net_reset();
    dm_account_reset();
    dm_sdk16_reset();
    dm_objmap_reset();
    dm_thread_reset_tables();
    dm_sdk16_reset_wait();
    dm_guest_mem_reset();
    loader_reset();
    dm_fps_state_apply();
    LOG("reset: the host is as at the first start (%.1f ms after the stop, %.1f ms after the request): the game starts again", (now_ns() - t_stop) / 1e6, (now_ns() - g_req_ns) / 1e6);
    atomic_store(&dm_reset_stop, 0);
    atomic_store(&g_requested, 0);
}

void dm_guest_boot(void (*boot)(void)) {
    for (;;) {
        if (!_setjmp(g_boot_jb)) {
            boot();
            return;
        }
        reset_rest();
    }
}
