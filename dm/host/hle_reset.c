// The game's reset: what Home > close the software > start it again does on a Switch, inside this process. The Mac window's Game > Reset Game (⌘R) and DM_CTL's "reset" line
// ask for it (dm_reset_request); the window, the shader libraries and every pipeline made so far stay, so the game starts again at once instead of after the app's start
// (about 14 s on a 16 GB M2 Pro: three pipeline archives opened, then the prepare step; the game's own boot to its title follows either way).
//
// Stopping the game: on Horizon the kernel ends every thread of the process wherever it is. Here a guest thread can be stopped only where it holds nothing of the host's: at a gate
// (an SVC, a system counter read, a spin wait pass, an observation gate: gate_dispatch) or at an entry into the host stack (every HLE take-over and NVN call: hle_bigstack.c),
// with nothing of the host's below it (dm_reset_can_end: host code that called guest code may hold a host lock). A worker exits there as ExitThread does, on its gate stack; the
// main guest thread, which runs the game on its own pthread's stack, jumps back to the boot loop (dm_guest_boot) and does the rest of the reset there. The waits a guest thread may
// be in when the reset comes return (block(), the start of a created thread, long sleeps, the guest clock's freeze, the blocking oe notification pop, socket polls), and the audio
// renderer's render thread leaves its loop as at CloseAudioRenderer, so each reaches its next boundary.
//
// Then the host's side of the game goes back to a new process's (reset_rest): graphics, audio and the video decoders first (their host threads read or write the game's memory), the
// services' state, the kernel's tables, the guest address block and the modules' writable parts; the main thread then runs the boot again.
//
// If the game cannot be stopped this way (a thread that does not come back within RESET_STOP_MS, time in the prepare step aside), or a GPU capture keeps the game's pointers
// (DM_NVN_CAPTURE), the process starts again instead: the graceful exit (hle_lifecycle.c), then this same host's image again with the same arguments and environment, which is the
// app's start without its settings window.
#define DM_RESET_IMPL   // the definitions below, not dm.h's weak ones
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
_Atomic int dm_reset_stop;            // 1 from the request until the main guest thread starts the game again: gate_dispatch and the waits below look at it
uintptr_t dm_reset_main_base;         // guest_main's frame (main.c): the main thread's chain of guest frames ends there
static jmp_buf g_boot_jb;
static _Atomic int g_requested;       // a reset is under way (one at a time)
static uint64_t g_req_ns; static char g_why[64];
static char **g_argv;
enum { RESET_STOP_MS = 3000 };
void dm_reset_set_argv(char **argv) { g_argv = argv; }

// AArch64 frame records, read without faulting (a frame pointer of guest code is the game's to keep): [fp] = the caller's fp, [fp + 8] = the return address into it
static int rec_read(uint64_t fp, uint64_t rec[2]) {
    vm_size_t got = 0;
    return fp && !(fp & 7) && !vm_read_overwrite(mach_task_self(), (vm_address_t)fp, 16, (vm_address_t)rec, &got) && got == 16;
}
int dm_guest_as_contains(uintptr_t a);   // hle_svc.c
// Whether a guest thread at a boundary from guest code into the host (sp, fp and lr of the guest code there) has nothing of the host's below it, so that it can end there.
// role: 1 a worker, 2 the main guest thread (dm_thread_reset_role). Host code that calls guest code (an HLE take-over calling the SDK's own function, the NVN layer, a log line)
// runs on the thread's host stack (hle_bigstack.c), so guest code on a worker's own stack, the guest address block, has only guest code below it; the one exception, host code on a
// guest stack, is the host's own guest threads (dm_guest_thread_spawn), which are role 3 and never end at a boundary. The frame chain cannot tell this for a worker: the game's
// gRPC threads run code without frame records, and their chains end in the middle of main. The main guest thread runs the game on its pthread's own stack, where host code runs
// too, so for it the chain must reach guest_main's frame through guest return addresses only (the game's main loop and the SDK keep frame records)
int dm_reset_can_end(int role, uint64_t sp, uint64_t fp, uint64_t lr) {
    if (!module_of((void *)(uintptr_t)lr)) return 0;
    if (role == 1) return dm_guest_as_contains((uintptr_t)sp);
    if (role != 2) return 0;
    for (int i = 0; i < 4096; i++) {
        if (fp == dm_reset_main_base) return 1;
        uint64_t rec[2];
        if (!rec_read(fp, rec) || rec[0] <= fp) return 0;
        if (rec[0] == dm_reset_main_base) return 1;   // the record of the guest function guest_main called: its return address is guest_main's
        if (!module_of((void *)(uintptr_t)rec[1])) return 0;   // a host frame: not here
        fp = rec[0];
    }
    return 0;
}
// The same from a gate frame (entry.S dm_gate: x29 at [29], at [99] the gate block's sp, where it pushed the guest's x29 and x30)
int dm_reset_gate_can_end(const uint64_t *regs, int role) {
    uint64_t pushed[2];
    return rec_read(regs[99], pushed) && dm_reset_can_end(role, regs[99], regs[29], pushed[1]);
}

int dm_thread_reset_role(void);       // hle_thread.c: 0 not a guest thread, 1 a worker, 2 the main guest thread, 3 a guest thread of the host's own
void dm_thread_reset_exit(void);      // hle_thread.c: ends the calling worker (as ExitThread)
void dm_thread_reset_main_out(void);  // hle_thread.c: the main guest thread leaves the thread table
uintptr_t guest_gate_stack_top(void); // thread.c: the top of the calling thread's gate stack
// A worker that ends off its gate stack (at an entry into the host, on its guest stack) moves there first: the exit runs the thread's key destructors, which need room
// and free its big host stack
void reset_exit_on_gate_stack(uintptr_t top) __attribute__((noreturn));
__asm__(".text\n.p2align 2\n_reset_exit_on_gate_stack:\n    mov sp, x0\n    mov x29, #0\n    bl _dm_thread_reset_exit\n    brk #0\n");
static void end_here(int role, int on_gate_stack) {
    if (role == 2) { dm_thread_reset_main_out(); _longjmp(g_boot_jb, 1); }
    if (on_gate_stack) dm_thread_reset_exit();
    reset_exit_on_gate_stack(guest_gate_stack_top() & ~(uintptr_t)15);
}
// gate_dispatch, while dm_reset_stop is set: the calling thread ends here if nothing of the host's is below it, else it goes on to its next boundary
void dm_reset_gate(const uint64_t *regs) {
    int role = dm_thread_reset_role();
    if ((role == 1 || role == 2) && dm_reset_gate_can_end(regs, role)) end_here(role, 1);
}
// The big host stack's entry (hle_bigstack.c dm_bigstack_top: every HLE take-over and NVN call from guest code), the same: its frame on the caller's stack, at sp + 0xa0, holds the
// guest's x29 and x30 (the return address into the caller)
void dm_reset_entry(uintptr_t sp) {
    int role = dm_thread_reset_role();
    const uint64_t *fr = (const uint64_t *)(sp + 0xa0);
    if ((role == 1 || role == 2) && dm_reset_can_end(role, sp, fr[0], fr[1])) end_here(role, 0);
}

// The start again of the whole process (the fallback): this host's image again, with its own arguments and environment, after the graceful exit has written the pipeline archive
static void relaunch_exec(void *arg) {
    (void)arg;
    char path[PATH_MAX]; uint32_t n = sizeof path;
    if (_NSGetExecutablePath(path, &n) || !g_argv) { LOG("[RED] reset: this host's path or arguments are unknown: it ends instead of starting again"); return; }
    long dm_ctl_offset(void); char off[32]; snprintf(off, sizeof off, "%ld", dm_ctl_offset()); setenv("DM_CTL_FROM", off, 1);   // DM_CTL: the new process reads on from here (hle_hid.c)
    LOG("reset: starting %s again", path);
    // This process's image replaced, with only stdin, stdout and stderr handed over (POSIX_SPAWN_CLOEXEC_DEFAULT): a plain execve kept every open file, the Metal caches' among
    // them, and the new image's pipeline prepare then stopped for good after its first half second (5,584 and 680 of 61,141 lines; 7.6-8 s in all otherwise)
    posix_spawnattr_t at; posix_spawn_file_actions_t fa; int err;
    if ((err = posix_spawnattr_init(&at)) || (err = posix_spawnattr_setflags(&at, POSIX_SPAWN_SETEXEC | POSIX_SPAWN_CLOEXEC_DEFAULT)) || (err = posix_spawn_file_actions_init(&fa))) { LOG("[RED] reset: posix_spawn setup failed (%d): the host ends", err); return; }
    for (int fd = 0; fd <= 2; fd++) posix_spawn_file_actions_addinherit_np(&fa, fd);
    err = posix_spawn(NULL, path, &fa, &at, g_argv, environ);
    LOG("[RED] reset: starting %s again failed (%d): the host ends", path, err);
}
static void relaunch(const char *why) {
    static _Atomic int once; if (atomic_exchange(&once, 1)) for (;;) pause();   // the stop's deadline and the main thread can both come here: one goes on
    LOG("reset: %s, so the whole process starts again (%.0f ms after the request)", why, (now_ns() - g_req_ns) / 1e6);
    dm_host_shutdown_then("reset", relaunch_exec, NULL);
    for (;;) pause();   // the graceful exit returns only when another one (SIGTERM) is under way: that one ends the process
}
static void stragglers(const char *what) {
    void dm_thread_reset_dump(void);
    LOG("[RED] reset: %s within %d ms; the guest threads still running:", what, RESET_STOP_MS);
    dm_thread_reset_dump();
}

void dm_thread_reset_wake(void);   // hle_thread.c: every guest wait looks at dm_reset_stop again
void dm_clock_reset_wake(void);    // hle_os.c: reads of a frozen guest clock
void dm_oe_reset_wake(void);       // hle_oe.c: the blocking notification pop
void dm_audren_reset_stop(void);   // hle_audren.c: the render threads leave their loop
void dm_net_reset_wake(void);      // hle_net.c: socket polls
int dm_thread_reset_live(int *main_live);   // hle_thread.c: guest threads not yet ended (the main thread's own state in *main_live)
int guest_clock_frozen_for(int reason);     // hle_os.c
// The time the stop has taken, less the time the graphics layer's prepare step held the clock (reason 3): the main thread waits in it, in host code, for as long as it lasts
// (seconds, behind its progress screen), and a reset asked for meanwhile goes on once it is over
static uint64_t g_prep_ns, g_prep_at;
static uint64_t stop_ms(void) {
    uint64_t now = now_ns();
    if (guest_clock_frozen_for(3)) { if (g_prep_at) g_prep_ns += now - g_prep_at; g_prep_at = now; } else g_prep_at = 0;
    return (now - g_req_ns - g_prep_ns) / 1000000;
}
static _Atomic uint64_t g_req_seq, g_back_seq;   // the request under way, and the last one the main thread came back for
static void stop_watch(uint64_t seq) {   // the main thread does the rest once it is back (dm_guest_boot); one that does not come back holds the game where nothing here can stop it
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 100 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{
        if (atomic_load(&g_back_seq) >= seq) return;
        if (stop_ms() < RESET_STOP_MS) { stop_watch(seq); return; }
        stragglers("the main guest thread did not stop");
        relaunch("the game could not be stopped");
    });
}
void dm_reset_request(const char *why) {
    if (atomic_exchange(&g_requested, 1)) { LOG("reset: one is under way already (%s ignored)", why); return; }
    g_req_ns = now_ns(); g_prep_ns = g_prep_at = 0; snprintf(g_why, sizeof g_why, "%s", why);
    // A GPU capture (DM_NVN_CAPTURE) keeps the game's pointers for its whole run: that run starts again as a whole
    if (getenv("DM_NVN_CAPTURE")) relaunch("DM_NVN_CAPTURE is on");
    LOG("reset: %s: stopping the game's threads", why);
    uint64_t seq = atomic_fetch_add(&g_req_seq, 1) + 1;
    atomic_store(&dm_reset_stop, 1);
    dm_thread_reset_wake(); dm_clock_reset_wake(); dm_oe_reset_wake(); dm_audren_reset_stop(); dm_net_reset_wake();
    stop_watch(seq);
}

void dm_gfx_reset(void); void dm_audio_reset(void); void dm_audren_reset(void); void dm_codec_video_reset(void); void dm_fs_reset(void);
void dm_hid_reset(void); void dm_oe_reset(void); void dm_sf_reset(void); void dm_net_reset(void); void dm_account_reset(void); void dm_sdk16_reset(void); void dm_sdk16_reset_wait(void);
void dm_objmap_reset(void); void dm_thread_reset_tables(void); void dm_guest_mem_reset(void); void loader_reset(void); void dm_fps_state_apply(void);
// On the main guest thread, back from its last gate: wait for the other guest threads, then put the host's side of the game back as a new process has it, and return to start it
static void reset_rest(void) {
    atomic_store(&g_back_seq, atomic_load(&g_req_seq));
    uint64_t t_main = now_ns();
    int live, main_live;
    while ((live = dm_thread_reset_live(&main_live)) > 0) {
        if (stop_ms() > RESET_STOP_MS) { stragglers("not every guest thread stopped"); relaunch("the game could not be stopped"); }
        usleep(500);
    }
    uint64_t t_stop = now_ns();
    LOG("reset: every guest thread stopped (main thread after %.1f ms, the last one after %.1f ms)", (t_main - g_req_ns) / 1e6, (t_stop - g_req_ns) / 1e6);
    // First what reads or writes the game's memory from host threads (graphics: the replay, the GPU and its completions; audio: the device callbacks; video: the decoders),
    // while that memory is still there; then the services' tables, the kernel's, the memory itself and the modules' data
    dm_gfx_reset();
    dm_audio_reset(); dm_audren_reset();
    dm_codec_video_reset();
    dm_fs_reset();
    dm_hid_reset(); dm_oe_reset(); dm_sf_reset(); dm_net_reset(); dm_account_reset(); dm_sdk16_reset();
    dm_objmap_reset();
    dm_thread_reset_tables();
    dm_sdk16_reset_wait();
    dm_guest_mem_reset();
    loader_reset(); dm_fps_state_apply();
    LOG("reset: the host is as at the first start (%.1f ms after the stop, %.1f ms after the request): the game starts again", (now_ns() - t_stop) / 1e6, (now_ns() - g_req_ns) / 1e6);
    atomic_store(&dm_reset_stop, 0);
    atomic_store(&g_requested, 0);
}

// The main guest thread's boot (main.c guest_main) and its loop: a reset brings it back here, and it starts the game again
void dm_guest_boot(void (*boot)(void)) {
    for (;;) {
        if (!_setjmp(g_boot_jb)) { boot(); return; }
        reset_rest();
    }
}
