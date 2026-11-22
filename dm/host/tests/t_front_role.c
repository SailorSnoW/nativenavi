// Check the windowless task role (../front_role.h) and the effect on interactive thread priority.
// XNU gives a task role's QoS ceiling to apps only (TASK_APPTYPE_APP_DEFAULT / APP_NONUI): any other apptype gets USER_INITIATED whatever its role, and a task
// takes its parent's apptype. Started under a process launchd spawned as a daemon (a terminal server's shells, sshd), the role would change nothing here, so the
// checks run in a child spawned as an app, as Terminal's shells are. docs/reference/audio.md, Underruns in a run without a window.
#include "../front_role.h"
#include <errno.h>
#include <mach-o/dyld.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <string.h>
#include <sys/wait.h>
extern char **environ;
int posix_spawnattr_setprocesstype_np(posix_spawnattr_t *, const int);   // spawn_private.h; in libSystem since macOS 10.8
enum { PROC_TYPE_APP_DEFAULT = 0x100 };   // POSIX_SPAWN_PROC_TYPE_APP_DEFAULT, XNU bsd/sys/spawn_internal.h
static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); } } while (0)
enum { LEGACY_PRIORITY = 31 };   // XNU BASEPRI_DEFAULT, the ceiling for TASK_UNSPECIFIED
static int role(void) {
    task_category_policy_data_t cat = {0}; mach_msg_type_number_t n = TASK_CATEGORY_POLICY_COUNT; boolean_t def = 0;
    return task_policy_get(mach_task_self(), TASK_CATEGORY_POLICY, (task_policy_t)&cat, &n, &def) == KERN_SUCCESS ? (int)cat.role : -99;
}
static int ui_priority(void) {   // the base priority of this thread once it asks for USER_INTERACTIVE
    if (pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0)) return -1;
    thread_extended_info_data_t ei; mach_msg_type_number_t n = THREAD_EXTENDED_INFO_COUNT; mach_port_t self = mach_thread_self();
    kern_return_t kr = thread_info(self, THREAD_EXTENDED_INFO, (thread_info_t)&ei, &n); mach_port_deallocate(mach_task_self(), self);
    return kr == KERN_SUCCESS ? ei.pth_priority : -1;
}
static int run_as_app(void) {   // this executable again, spawned with the apptype of an app; its exit status
    char self[4096]; uint32_t size = sizeof self;
    if (_NSGetExecutablePath(self, &size)) { fprintf(stderr, "FAIL: find this executable's path\n"); return 1; }
    char *args[] = {self, "--as-app", NULL};
    posix_spawnattr_t attr; pid_t pid; int status;
    int err = posix_spawnattr_init(&attr);
    if (!err) err = posix_spawnattr_setprocesstype_np(&attr, PROC_TYPE_APP_DEFAULT);
    if (!err) err = posix_spawn(&pid, self, NULL, &attr, args, environ);
    posix_spawnattr_destroy(&attr);
    if (err) { fprintf(stderr, "FAIL: spawn the checks as an app: %s\n", strerror(err)); return 1; }
    while (waitpid(pid, &status, 0) < 0)
        if (errno != EINTR) { fprintf(stderr, "FAIL: wait for the checks: %s\n", strerror(errno)); return 1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    fprintf(stderr, "FAIL: the checks stopped on signal %d\n", WIFSIGNALED(status) ? WTERMSIG(status) : 0);
    return 1;
}
int main(int argc, char **argv) {
    if (argc < 2 || strcmp(argv[1], "--as-app")) return run_as_app();
    kern_return_t kr;
    int initial_role = role();
    CHECK(initial_role != -99, "read the initial task role");
    CHECK(front_windowless_role("0", &kr) == 0 && kr == KERN_SUCCESS && role() == initial_role, "DM_WINDOWLESS_ROLE=0 keeps the existing task role");
    int before = ui_priority();
    CHECK(before >= 0, "read the initial interactive priority");
    CHECK(front_windowless_role(NULL, &kr) == 1, "unset applies the role");
    CHECK(role() == TASK_DEFAULT_APPLICATION, "the task has the default application role");
    int after = ui_priority();
    // A launch QoS clamp can hold both samples below the unspecified role's legacy ceiling.
    if (before >= 0 && before < LEGACY_PRIORITY && after == before)
        printf("interactive QoS lift check skipped: priority held at %d below the legacy ceiling\n", before);
    else
        CHECK(after > LEGACY_PRIORITY && (initial_role != TASK_UNSPECIFIED || after > before), "the default application role lifts the interactive QoS ceiling");
    CHECK(front_windowless_role("1", &kr) == 1 && role() == TASK_DEFAULT_APPLICATION, "DM_WINDOWLESS_ROLE=1 also applies the role");
    printf("windowless role: task role %d -> %d, USER_INTERACTIVE base priority %d -> %d (%d checks, %d failed)\n",
           initial_role, role(), before, after, checks, fails);
    return fails != 0;
}
