// Graceful exit for SIGTERM and oe::ExecuteProgram's hand-over; a player's quit is atexit's.
#include "hle.h"
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <signal.h>
#include <unistd.h>
void hlecount_flush(void);
void dm_rec_finish(void);
void dm_fs_temp_empty(void);
// a pipeline compile can hold the drain for seconds: the archive write runs beside it
enum { EXIT_DRAIN_MS = 1500, EXIT_LATE_MS = 500 };
static _Atomic int g_exit_taken;   // one path ends the process: the exit or the deadline
static void exit_end(void (*then)(void *), void *arg) {
    if (atomic_exchange(&g_exit_taken, 1)) for (;;) pause();
    dm_fs_temp_empty();   // _exit runs no atexit: the software closes here, and a Switch empties the temporary storage at the close
    if (then) then(arg);   // returns only on failure
    _exit(0);
}
static int ms_left(uint64_t t0, int budget_ms) { int64_t left = budget_ms - (int64_t)((now_ns() - t0) / 1000000); return left > 0 ? (int)left : 0; }
void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg) {
    static _Atomic int once;
    if (atomic_exchange(&once, 1)) return;
    uint64_t t0 = now_ns();
    LOG("received %s: graceful exit", why);
    // Past every wait: end anyway, starting the next program so a hand-over does not become a quit
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, (int64_t)(DM_QUIT_ARCH_MS + EXIT_LATE_MS) * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{
        LOG("[RED] graceful exit not done in %.1f seconds, exiting at once", (DM_QUIT_ARCH_MS + EXIT_LATE_MS) / 1e3);
        exit_end(then, arg);
    });
    hlecount_flush();                               // _exit does not run atexit
    dm_rec_finish();
    dm_audio_suspend(1);
    dm_gfx_quit_begin();
    dispatch_semaphore_t drained = dispatch_semaphore_create(0);
    dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INTERACTIVE, 0), ^{ dm_gfx_suspend(1); dispatch_semaphore_signal(drained); });
    if (dispatch_semaphore_wait(drained, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms_left(t0, EXIT_DRAIN_MS) * NSEC_PER_MSEC)))
        LOG("[RED] graceful exit: graphics not suspended in %.1f seconds (a draw under way, most often one compiling its pipeline): the pipeline archive is written all the same", EXIT_DRAIN_MS / 1e3);
    dm_gfx_quit(ms_left(t0, DM_QUIT_ARCH_MS));
    LOG("graceful exit done (%.3f s)", (now_ns() - t0) / 1e9);
    exit_end(then, arg);
}
void dm_host_shutdown(const char *why) { dm_host_shutdown_then(why, NULL, NULL); }
void dm_term_install(void) {
    signal(SIGTERM, SIG_IGN);                       // the GCD signal source handles it
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0));
    dispatch_source_set_event_handler(src, ^{ dm_host_shutdown("SIGTERM"); });
    dispatch_resume(src);
}
