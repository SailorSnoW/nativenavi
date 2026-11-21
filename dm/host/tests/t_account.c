// Account contracts: the registry of local users beside the save store, read by a fresh process, and live sessions.
#include "../hle_account.c"
#include <assert.h>
#include <sys/wait.h>
#include <unistd.h>

void hle_signal_system_event(void *g) { (void)g; }
int dm_guest_as_contains(uintptr_t a) { (void)a; return 0; }
static void *session_thread(void *unused) {
    (void)unused;
    for (int i = 0; i < 100; i++) {
        UserHandle h; assert(h_acc_OpenUser(&h, &k_uid) == RES_OK); h_acc_CloseUser(&h); h_acc_CloseUser(&h);
    }
    return NULL;
}
static void child(const char *exe, const char *mode, const char *save) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) { execl(exe, exe, mode, save, NULL); _exit(127); }
    int status; assert(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0);
}
static void registry(const char *save, const char *text) {
    char path[1100]; snprintf(path, sizeof path, "%s.users", save);
    FILE *f = fopen(path, "w"); assert(f); fputs(text, f); fclose(f);
}
int main(int argc, char **argv) {
    unsetenv("DM_NICK");
    Uid out, list[8], u2 = user_uid(1), u3 = user_uid(2); UserHandle h, other; int n; uint8_t exists; char nick[33];
    if (argc == 3) {
        setenv("DM_SAVE", argv[2], 1);
        if (!strcmp(argv[1], "--registered")) {   // users 1 and 3 are registered; a fresh process has no sessions
            assert(h_acc_ListAll(&n, list, 8) == RES_OK && n == 2 && dm_uid_user(&list[0]) == 0 && dm_uid_user(&list[1]) == 2);
            assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
            assert(h_acc_Exists(&exists, &u3) == RES_OK && exists && h_acc_Exists(&exists, &u2) == RES_OK && !exists);
            assert(h_acc_UserCount(&n) == RES_OK && n == 2);
            assert(h_acc_ListQual(&n, list, 1) == RES_OK && n == 1);
            assert(h_acc_ListAll(&n, NULL, 8) == RES_OK && n == 0);
            assert(h_acc_GetNick(nick, &u3) == RES_OK && !strcmp(nick, "Player 3") && h_acc_GetNick(nick, &u2) == RES_FAIL);
            assert(h_acc_OpenUser(&h, &u2) == RES_FAIL);
            assert(h_acc_OpenUser(&h, &u3) == RES_OK && h_acc_OpenUser(&other, &u3) == RES_OK && h.v[2] != other.v[2]);
            assert(h_acc_EnsureNsa(&h) == 0x27c); // ResultCancelledByUser for an open local account
            h_acc_CloseUser(&h); h_acc_CloseUser(&h);
            assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 1 && dm_uid_user(list) == 2);
            h_acc_CloseUser(&other);
            assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
        } else {   // --corrupt: a registry that cannot be read registers nobody, user 1 included
            assert(h_acc_ListAll(&n, list, 8) == RES_FAIL && n == 0);
            assert(h_acc_UserCount(&n) == RES_FAIL && n == 0);
            assert(h_acc_Exists(&exists, &k_uid) == RES_OK && !exists && !h_acc_TryOpenPre(&h));
        }
        return 0;
    }
    char root[] = "/private/tmp/t_account.XXXXXX", save[1024], path[1100]; assert(mkdtemp(root));
    snprintf(save, sizeof save, "%s/save", root); setenv("DM_SAVE", save, 1);
    // A save store without a registry has user 1.
    h_acc_Init();
    assert(h_acc_ListAll(&n, list, 8) == RES_OK && n == 1 && !memcmp(&list[0], &k_uid, 16));
    assert(h_acc_UserCount(&n) == RES_OK && n == 1 && h_acc_Exists(&exists, &u2) == RES_OK && !exists);
    assert(h_acc_GetNick(nick, &k_uid) == RES_OK && !strcmp(nick, "Player"));
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    assert(h_acc_TryOpenPre(&h) && h_acc_OpenUser(&other, &k_uid) == RES_OK && h.v[2] != other.v[2]);
    assert(h_acc_GetUserId(&out, &h) == RES_OK && !memcmp(&out, &k_uid, 16));
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 1);
    h_acc_CloseUser(&h); h_acc_CloseUser(&h);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 1);
    h_acc_CloseUser(&other);
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    // SDK GetUserId copies even foreign/empty handles, without validating a token.
    const uint64_t third[] = {2, 8, 0, UINT64_MAX};
    for (unsigned i = 0; i < sizeof third / sizeof third[0]; i++) {
        h = (UserHandle){{0x7012222b20, 0x400000001, third[i]}};
        assert(h_acc_GetUserId(&out, &h) == RES_OK && out.v[0] == h.v[0] && out.v[1] == h.v[1] && h.v[2] == third[i]);
        h_acc_CloseUser(&h);
        h.v[0] = h.v[1] = 0; assert(h_acc_GetUserId(&out, &h) == RES_OK && !out.v[0] && !out.v[1]);
    }
    pthread_t threads[4];
    for (int i = 0; i < 4; i++) assert(!pthread_create(&threads[i], NULL, session_thread, NULL));
    for (int i = 0; i < 4; i++) assert(!pthread_join(threads[i], NULL));
    assert(h_acc_ListOpen(&n, list, 8) == RES_OK && n == 0);
    registry(save, "nxnative-users-1 05\n"); child(argv[0], "--registered", save);
    registry(save, "broken registry\n"); child(argv[0], "--corrupt", save);
    registry(save, "nxnative-users-1 04\n"); child(argv[0], "--corrupt", save);       // without user 1
    registry(save, "nxnative-users-1 105\n"); child(argv[0], "--corrupt", save);      // a ninth user
    registry(save, "nxnative-users-1 05 more\n"); child(argv[0], "--corrupt", save);
    snprintf(path, sizeof path, "%s.users", save); unlink(path);
    rmdir(root);
    puts("t_account: OK (registry in a fresh process, sessions, concurrent closes)");
    return 0;
}
