// Local profiles belong to this app's save store: a tiny registry beside DM_SAVE, read here,
// outside any user's save transaction. A store without one has user 1 only.
#include "dm.h"
#include "account_users.h"

static pthread_mutex_t g_users_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_users_path[1024];
static unsigned g_users_mask;
static int g_users_loaded;

// Called under g_users_lock. A changed DM_SAVE is a different store (also used by
// synthetic fs tests); normal game runs keep it fixed for the process lifetime.
static void users_load(void) {
    const char *save = getenv("DM_SAVE");
    char path[1024] = "";
    if (save && *save) {
        size_t n = strlen(save); while (n > 1 && save[n - 1] == '/') n--;
        if (n + sizeof ".users" > sizeof path) { g_users_mask = 0; g_users_loaded = 0; return; }
        memcpy(path, save, n); memcpy(path + n, ".users", sizeof ".users");
    }
    if (g_users_loaded && !strcmp(path, g_users_path)) return;
    strlcpy(g_users_path, path, sizeof g_users_path);
    g_users_loaded = 1; g_users_mask = 1;
    if (!*path) return;   // user 1 still exists without a save store
    FILE *f = fopen(path, "r");
    if (!f && errno == ENOENT) return;
    unsigned mask = 0; char extra;
    int valid = f && fscanf(f, "nxnative-users-1 %x %c", &mask, &extra) == 1 && (mask & 1) && mask < (1u << DM_USERS) && !ferror(f);
    if (f) fclose(f);
    if (valid) g_users_mask = mask;
    else { g_users_mask = 0; LOG("[RED] account: cannot read user registry %s; leaving it intact", path); }
}

unsigned dm_account_users(void) {
    pthread_mutex_lock(&g_users_lock); users_load();
    unsigned mask = g_users_mask;
    pthread_mutex_unlock(&g_users_lock); return mask;
}
int dm_account_user(const void *uid) {
    int i = dm_uid_user(uid);
    return i >= 0 && (dm_account_users() & (1u << i)) ? i : -1;
}
