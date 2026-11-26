// nn::fs on the Mac file system: rom → $DM_ROMFS (default $DM_ROOT/romfs, read-only), save → $DM_SAVE (required), temp → $DM_TEMP.
// DirectoryEntry = {char name[0x301], pad[3], s8 type, pad[3], s64 size} (0x310 bytes).
#include "hle.h"
#include "account_users.h"
#include <mach/mach.h>
#include "front.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
#include <stdatomic.h>
#include <sys/clonefile.h>
#include <sys/xattr.h>
#include "romfs_image.h"

#define RES_PATH_NOT_FOUND 0x202u      // module 2, description 1
#define RES_PATH_EXISTS    0x402u      // module 2, description 2
#define RES_RENAME_OTHER_FS 0x2F2402u  // 2-6034 ResultRenameToOtherFileSystem
enum { MODE_READ = 1, MODE_WRITE = 2, MODE_APPEND = 4 };
enum { ENT_DIR = 0, ENT_FILE = 1 };
enum { DIRMODE_DIR = 1, DIRMODE_FILE = 2 };

typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry layout");

#define MAX_MOUNT 512   // permanent (Unmount frees nothing), so sized far above a game's names
// Save transactions: the mount is a clonefile'd <commit>.work; Commit clones it to <commit>.new and swaps it in (RENAME_SWAP), so a kill leaves the old or new save, never a mix.
// DM_ROMFS may name a RomFS image (dm/tools/romfs_image.py): a handle is then an entry (bit 58, never a valid fd), read with pread from one descriptor.
#define IMG_HANDLE(mi, k) ((1ull << 58) | ((uint64_t)(mi) << 32) | (uint64_t)(k))
#define IMG_H(h) (((h) >> 58) == 1)
#define IMG_M(h) ((int)(((h) >> 32) & 0x3ffffff))
#define IMG_K(h) ((uint32_t)(h))
// Horizon's FileHandle is a heap pointer whose low bits a game may use: the guest sees the host handle << 4 (the image flag lands on bit 62).
#define FH_OUT(h) ((uint64_t)(h) << 4)
#define FH_IN(h) ((uint64_t)(h) >> 4)
typedef struct {
    char name[32]; char dir[1024]; char overlay[4096]; char commit[1024];
    romfs_img *img;                  // non-NULL: dir is a RomFS image file
    int ro;
    int romfs;                       // reads look in the overlay folders first
    int txn_root;                    // canonical mount of this backing transaction, or -1
    _Atomic int poison;              // per backing, not per process
    _Atomic uint64_t first_write_ns;
} mount_t;
static mount_t g_mnt[MAX_MOUNT];   // commit non-empty = transactional save mount
static int g_nmnt;
static int mnt_is(const mount_t *m, const char *s, size_t n) { return strlen(m->name) == n && !strncmp(m->name, s, n); }
static int mnt_named(const mount_t *m, const char *name) { return mnt_is(m, name, strlen(name)); }
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static void path_unlock(pthread_mutex_t **m) { pthread_mutex_unlock(*m); }
#define PATH_LOCK() pthread_mutex_t *path_guard __attribute__((cleanup(path_unlock))) = &g_path_lock; pthread_mutex_lock(path_guard)
static uint8_t g_fmode[65536];
static _Atomic uint16_t g_ftxn[65536];       // fd → 1-based canonical transaction, or 0
static _Atomic int g_save_writes;
static pthread_once_t g_save_test_once = PTHREAD_ONCE_INIT;
static int g_save_kill_n, g_save_fail_n, g_save_fail_e;
static void save_test_init(void) {
    const char *k = getenv("DM_TEST_KILL_AT"), *f = getenv("DM_TEST_FS_FAIL");
    g_save_kill_n = k && !strncmp(k, "write:", 6) ? atoi(k + 6) : 0;
    if (f && !strncmp(f, "write:", 6)) {
        g_save_fail_n = atoi(f + 6);
        const char *c = strchr(f + 6, ':');
        g_save_fail_e = c ? atoi(c + 1) : ENOSPC;
    }
}
static pthread_rwlock_t g_txn_lock[MAX_MOUNT];
static pthread_once_t g_txn_lock_once = PTHREAD_ONCE_INIT;
static void txn_locks_init(void) { for (int i = 0; i < MAX_MOUNT; i++) pthread_rwlock_init(&g_txn_lock[i], NULL); }
static void txn_unlock(pthread_rwlock_t **m) { if (*m) pthread_rwlock_unlock(*m); }
// A Switch's save writes never fail and the game checks nothing before it Commits: after a failed host write the transaction's Commit is refused,
// the work directory recloned and the front end told (front_save_failed). Each backing keeps its own first error.
static void save_poison_txn(int txn, int e) {
    // Failed fd I/O does not hold the path lock: do not read the mutable mount count.
    if (txn < 0 || txn >= MAX_MOUNT) return;
    int z = 0;
    atomic_compare_exchange_strong(&g_mnt[txn].poison, &z, e ? e : EIO);
}
static void __attribute__((unused)) save_poison(int e) {   // test helper (t_fs)
    for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].txn_root == i) { save_poison_txn(i, e); return; }
}
// Weak: a front end may define its own. Game thread.
__attribute__((weak)) void front_save_failed(int err) { LOG("front_save_failed(errno=%d): no front end attached (default weak implementation)", err); }
// DM_FSLOG=<file>: a TSV line per ReadFile: seconds, guest path, offset, length, buffer
static FILE *g_fslog;
static char *g_fdpath[4096];
static pthread_mutex_t g_fslog_lock = PTHREAD_MUTEX_INITIALIZER;
static void fslog_init(void) {   // line-buffered: runs end at an alarm or crash, which flush no stdio
    static int done;
    if (done) return;
    done = 1;
    const char *e = getenv("DM_FSLOG");
    if (e && !(g_fslog = fopen(e, "w"))) die("cannot open DM_FSLOG %s", e);
    if (g_fslog) setvbuf(g_fslog, NULL, _IOLBF, 0);
}

static const char *envdir(const char *var, const char *rel, char *buf, size_t n) {
    const char *v = getenv(var);
    if (v) return v;
    snprintf(buf, n, "%s%s/%s", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", rel); return buf;
}

// DM_TEST_FS_FAILAT=<point>[:<count>][,...]: mountclone, reclone, guestname or renameback fail with ENOSPC (tests/t_fs.c).
static int dm_fs_test_fail(const char *pt) {
    static char spec[256];
    static int cnt[8];
    const char *e = getenv("DM_TEST_FS_FAILAT");
    if (!e || !e[0]) return 0;
    if (strcmp(spec, e)) { strlcpy(spec, e, sizeof spec); memset(cnt, 0, sizeof cnt); }
    size_t L = strlen(pt); int k = 0;
    for (const char *q = spec; *q && k < 8; k++) { const char *end = strchr(q, ','); size_t n = end ? (size_t)(end - q) : strlen(q);
        if (n >= L && !strncmp(q, pt, L) && (n == L || q[L] == ':')) { int lim = n > L ? atoi(q + L + 1) : 1; if (cnt[k] < lim) { cnt[k]++; errno = ENOSPC; LOG("DM_TEST_FS_FAILAT: %s fails with ENOSPC (#%d)", pt, cnt[k]); return 1; } return 0; }
        q = end ? end + 1 : q + n; }
    return 0;
}
static int rm_tree(const char *p);
// NULL (and a red line) when not a complete image
static romfs_img *img_open(const char *path) {
    romfs_img *m = romfs_image_open(path);
    if (m) LOG("fs RomFS image %s: %llu entries", path, (unsigned long long)m->n);
    else LOG("[RED] fs RomFS image %s is not a complete image (copied in part? errno=%d)", path, errno);
    return m;
}
// a binary search per component: the packer sorted children by bytes, a shorter prefix first
static int64_t img_find(const romfs_img *m, const char *rest) {
    uint64_t k = 0;
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        size_t len = end - rest;
        if (!(len == 1 && rest[0] == '.')) {
            if (m->e[k].type) { errno = ENOTDIR; return -1; }
            uint64_t lo = m->e[k].a, hi = lo + m->e[k].b; int found = 0;
            while (lo < hi) {
                uint64_t mid = lo + (hi - lo) / 2; const img_ent *x = &m->e[mid];
                int c = memcmp(m->names + x->name, rest, x->nlen < len ? x->nlen : len);
                if (!c) c = (x->nlen > len) - (x->nlen < len);
                if (!c) { k = mid; found = 1; break; }
                if (c < 0) lo = mid + 1; else hi = mid;
            }
            if (!found) { errno = ENOENT; return -1; }
        }
        rest = end;
    }
    return (int64_t)k;
}
// Horizon turns backslashes into slashes before normalizing (SetUpPath) on every mount but the PC host's. Too long for buf: left as it is (not found).
static const char *fwd_slashes(const char *p, char *buf, size_t n) {
    if (!p || !strchr(p, '\\') || strlen(p) >= n) return p;
    size_t i = 0; for (; p[i]; i++) buf[i] = p[i] == '\\' ? '/' : p[i];
    buf[i] = 0; return buf;
}
static int norm_rest(const char *rest, char *out, size_t n);
static int rom_overlay_path(const mount_t *m, const char *rest, char *out, size_t n, int *deleted);
// Changed while the game runs: open files stay, later opens and listings follow.
static char g_overlay_live[4096];
static int g_overlay_is_live;
static const char *overlay_now(void) { return g_overlay_is_live ? g_overlay_live : getenv("DM_ROMFS_OVERLAY"); }
void dm_fs_overlay_set(const char *list) {
    PATH_LOCK();
    strlcpy(g_overlay_live, list, sizeof g_overlay_live); g_overlay_is_live = 1;
    for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].romfs) strlcpy(g_mnt[i].overlay, list, sizeof g_mnt[i].overlay);
}
// 1: an image entry in *mi / *k (-1 with errno when none); 0: another mount, or a path an overlay folder has
static int img_path(const char *p, int *mi, int64_t *k) {
    PATH_LOCK();
    char fwd[1024];
    p = fwd_slashes(p, fwd, sizeof fwd);
    const char *c = p ? strchr(p, ':') : NULL;
    if (!c) return 0;
    size_t ln = c - p;
    for (int i = 0; i < g_nmnt; i++) {
        if (!mnt_is(&g_mnt[i], p, ln)) continue;
        if (!g_mnt[i].img) return 0;
        char normed[1024]; const char *rest = c + 1; *mi = i;
        if (strstr(rest, "/.")) { if (!norm_rest(rest, normed, sizeof normed)) { *k = -1; return 1; } rest = normed; }
        char ov[1024]; int deleted = 0;
        if (g_mnt[i].overlay[0] && rom_overlay_path(&g_mnt[i], rest, ov, sizeof ov, &deleted)) return 0;
        if (deleted) { errno = ENOENT; *k = -1; return 1; }
        *k = img_find(g_mnt[i].img, rest); return 1;
    }
    return 0;
}
static Result mount_ex(const char *name, const char *dir, int ro, int txn) {
    pthread_once(&g_txn_lock_once, txn_locks_init);
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    for (int i = 0; i < g_nmnt; i++)
        if (mnt_named(&g_mnt[i], name)) {
            // A different account's save under this name must not resolve to the old one.
            const char *backing = g_mnt[i].commit[0] ? g_mnt[i].commit : g_mnt[i].dir;
            Result r = !strcmp(backing, dir) && g_mnt[i].ro == ro ? RES_OK : RES_PATH_EXISTS;
            pthread_mutex_unlock(&g_lock); return r;
        }
    if (g_nmnt >= MAX_MOUNT) die("mount table full");
    strlcpy(g_mnt[g_nmnt].name, name, sizeof g_mnt[0].name);
    g_mnt[g_nmnt].ro = ro; g_mnt[g_nmnt].overlay[0] = 0; g_mnt[g_nmnt].commit[0] = 0;
    g_mnt[g_nmnt].txn_root = -1;
    atomic_store(&g_mnt[g_nmnt].poison, 0);
    atomic_store(&g_mnt[g_nmnt].first_write_ns, 0);
    g_mnt[g_nmnt].romfs = !strcmp(name, "rom");
    const char *overlay = g_mnt[g_nmnt].romfs ? overlay_now() : NULL;
    if (overlay) strlcpy(g_mnt[g_nmnt].overlay, overlay, sizeof g_mnt[0].overlay);
    // The transaction cannot be set up (full disk): mount the committed directory read-only instead of exiting; writes fail and the front end is told.
    int txn_err = 0, k = 0;
    while (txn && k < g_nmnt && strcmp(g_mnt[k].commit, dir)) k++;
    if (txn && k < g_nmnt) {   // a second name for a mounted save shares its transaction (a new clone would drop pending writes)
        strlcpy(g_mnt[g_nmnt].commit, g_mnt[k].commit, sizeof g_mnt[0].commit); strlcpy(g_mnt[g_nmnt].dir, g_mnt[k].dir, sizeof g_mnt[0].dir);
        g_mnt[g_nmnt].txn_root = g_mnt[k].txn_root;
    } else if (txn) {
        char work[1100], neu[1100];
        snprintf(work, sizeof work, "%s.work", dir);
        snprintf(neu, sizeof neu, "%s.new", dir);
        mkdir(dir, 0755);
        const char *why = NULL;
        if (!access(work, F_OK) && rm_tree(work)) why = "cannot clear the work directory left over from last time";
        else if (!access(neu, F_OK) && rm_tree(neu)) why = "cannot clear the .new left over from last time";
        else if (dm_fs_test_fail("mountclone") || clonefile(dir, work, 0))   // ENOTSUP: HFS+ and exFAT can neither clone nor RENAME_SWAP
            why = errno == ENOTSUP ? "the save folder's filesystem cannot clone directories (save transactions need APFS: set DM_SAVE to a folder on an APFS volume)" : "cloning the work directory failed";
        if (why) { txn_err = errno ? errno : EIO; g_mnt[g_nmnt].ro = 1; strlcpy(g_mnt[g_nmnt].dir, dir, sizeof g_mnt[0].dir);
            LOG("[RED] save transaction: %s (%s, errno=%d) -- mounting the committed directory %s read-only instead: saves cannot be written this session, the committed version is unchanged", why, work, txn_err, dir); }
        else {
            strlcpy(g_mnt[g_nmnt].commit, dir, sizeof g_mnt[0].commit);
            g_mnt[g_nmnt].txn_root = g_nmnt;
            atomic_store(&g_mnt[g_nmnt].poison, 0);
            atomic_store(&g_mnt[g_nmnt].first_write_ns, 0);
            strlcpy(g_mnt[g_nmnt].dir, work, sizeof g_mnt[0].dir); }
    } else strlcpy(g_mnt[g_nmnt].dir, dir, sizeof g_mnt[0].dir);
    g_mnt[g_nmnt].img = NULL;
    struct stat ist;
    if (ro && !txn) {
        // a stale path (an old command naming a replaced RomFS folder): stop here, not at the game's first read
        if (stat(dir, &ist) || !(S_ISDIR(ist.st_mode) || S_ISREG(ist.st_mode)))
            die("the game files %s (%s:) are missing: neither a folder nor a RomFS image (the rom mount is DM_ROMFS, or $DM_ROOT/romfs when DM_ROMFS is unset)", dir, name);
        if (S_ISREG(ist.st_mode) && !(g_mnt[g_nmnt].img = img_open(dir))) die("the game files %s are not a complete RomFS image", dir);
    }
    g_nmnt++;
    pthread_mutex_unlock(&g_lock);
    if (txn_err) front_save_failed(txn_err);
    if (!ro && !txn) mkdir(dir, 0755);
    LOG("fs mount %s: → %s%s%s", name, txn ? g_mnt[g_nmnt - 1].dir : dir, ro || txn_err ? " (read-only)" : txn ? " (transactional save: work directory, swapped in atomically on Commit)" : "", g_mnt[g_nmnt - 1].overlay[0] ? " (overlay first)" : "");
    return RES_OK;
}
static Result mount(const char *name, const char *dir, int ro) { return mount_ex(name, dir, ro, 0); }

// APFS is case-insensitive, Horizon is not: components match by exact guest name; a host collision gets another physical name, the guest name in an xattr (clonefile keeps it).
#define FS_GUEST_NAME "org.d2rmobile.guest-name"   // existing saves carry this name
static int guest_name(const char *parent, const char *physical, char *out, size_t n) {
    // Only .dm-case-N names carry the xattr: no getxattr per entry
    if (strncmp(physical, ".dm-case-", 9)) { strlcpy(out, physical, n); return 1; }
    char p[1280]; snprintf(p, sizeof p, "%s/%s", parent, physical);
    ssize_t k = getxattr(p, FS_GUEST_NAME, out, n - 1, 0, XATTR_NOFOLLOW);
    if (k >= 0) { out[k] = 0; return 1; }
    if (errno != ENOATTR) return 0;
    strlcpy(out, physical, n); return 1;
}
// readdir answers both end and failure with NULL: a failure read as the end would leave a short listing in the name index for the run.
static struct dirent *dir_next(DIR *d) {
    struct dirent *e;
    do { errno = 0; e = readdir(d); } while (e && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")));
    return e;
}
// Name index of read-only mounts' directories (guest → physical), built at first lookup and kept: they never change. Callers hold PATH_LOCK.
typedef struct { const char *guest, *phys; } name_ent;
typedef struct dir_index { struct dir_index *next; uint64_t hash; char *path; size_t n; name_ent *e; char *pool; } dir_index;
#define DIR_INDEX_SLOTS 8192
static dir_index *g_dir_index[DIR_INDEX_SLOTS];
static uint64_t fnv1a(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; } return h; }
static int name_ent_cmp(const void *a, const void *b) { return strcmp(((const name_ent *)a)->guest, ((const name_ent *)b)->guest); }
// NULL when it cannot be built; nothing is kept and the next lookup tries again
static const dir_index *dir_index_get(const char *dir) {
    uint64_t h = fnv1a(dir); dir_index **slot = &g_dir_index[h % DIR_INDEX_SLOTS];
    for (dir_index *x = *slot; x; x = x->next) if (x->hash == h && !strcmp(x->path, dir)) return x;
    DIR *d = opendir(dir); if (!d) return NULL;
    size_t n = 0, cap = 64, plen = 0, pcap = 4096;
    size_t (*off)[2] = malloc(cap * sizeof *off);
    char *pool = malloc(pcap);
    if (!off || !pool) die("directory index allocation failed");
    struct dirent *e; char guest[0x301]; int ok = 1;
    while ((e = dir_next(d))) {
        if (!guest_name(dir, e->d_name, guest, sizeof guest)) { ok = 0; break; }
        size_t gl = strlen(guest) + 1, same = !strcmp(guest, e->d_name), pl = same ? 0 : strlen(e->d_name) + 1;
        if (n == cap) off = dm_realloc(off, (cap *= 2) * sizeof *off, "a directory index");
        while (plen + gl + pl > pcap) pool = dm_realloc(pool, pcap *= 2, "a directory index");
        off[n][0] = plen; memcpy(pool + plen, guest, gl); plen += gl;
        off[n][1] = same ? off[n][0] : plen; if (!same) { memcpy(pool + plen, e->d_name, pl); plen += pl; }
        n++;
    }
    if (!e && errno) ok = 0;
    closedir(d);
    if (!ok) { free(off); free(pool); return NULL; }
    dir_index *x = calloc(1, sizeof *x);
    if (!x || !(x->path = strdup(dir)) || !(x->e = malloc((n ? n : 1) * sizeof *x->e))) die("directory index allocation failed");
    for (size_t i = 0; i < n; i++) { x->e[i].guest = pool + off[i][0]; x->e[i].phys = pool + off[i][1]; }
    free(off); qsort(x->e, n, sizeof *x->e, name_ent_cmp);
    x->hash = h; x->n = n; x->pool = pool; x->next = *slot; *slot = x;
    return x;
}
static int path_component(char *parent, size_t n, const char *name, int create, int ro) {
    if (ro && !create) {
        const dir_index *x = dir_index_get(parent);
        if (x) {
            name_ent key = {name, NULL}; const name_ent *hit = bsearch(&key, x->e, x->n, sizeof *x->e, name_ent_cmp);
            if (!hit) { errno = ENOENT; return 0; }
            size_t len = strlen(parent), k = strlen(hit->phys);
            if (len + k + 2 > n) { errno = ENAMETOOLONG; return 0; }
            parent[len] = '/'; memcpy(parent + len + 1, hit->phys, k + 1); return 1;
        }
    }
    DIR *d = opendir(parent); if (!d) return 0;
    struct dirent *e; char physical[0x301] = "", guest[0x301];
    while ((e = dir_next(d))) {
        if (!guest_name(parent, e->d_name, guest, sizeof guest)) { closedir(d); return 0; }
        if (!strcmp(guest, name)) { strlcpy(physical, e->d_name, sizeof physical); break; }
    }
    if (!e && errno) { int err = errno; closedir(d); errno = err; return 0; }   // not "not found": the name may be in what was not read
    closedir(d);
    if (!physical[0]) {
        if (!create) { errno = ENOENT; return 0; }
        strlcpy(physical, name, sizeof physical);
        char p[1280]; struct stat st;
        snprintf(p, sizeof p, "%s/%s", parent, physical);
        if (!lstat(p, &st)) { // no exact guest name, but APFS matched another entry
            unsigned i = 0;
            do { snprintf(physical, sizeof physical, ".dm-case-%u", ++i);
                 snprintf(p, sizeof p, "%s/%s", parent, physical); } while (!lstat(p, &st));
        }
        if (errno != ENOENT) return 0;
    }
    size_t len = strlen(parent), k = strlen(physical);
    if (len + k + 2 > n) { errno = ENAMETOOLONG; return 0; }
    parent[len] = '/'; memcpy(parent + len + 1, physical, k + 1); return 1;
}
static int root_path(const char *root, const char *rest, char *out, size_t n) {
    strlcpy(out, root, n);
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        size_t k = end - rest; char part[0x301];
        if (k >= sizeof part) { errno = ENAMETOOLONG; return 0; }
        memcpy(part, rest, k); part[k] = 0;
        if (!strcmp(part, "..")) { errno = ENOENT; return 0; }
        if (strcmp(part, ".") && !path_component(out, n, part, 0, 1)) return 0;   // overlay roots are RomFS, read-only
        rest = end;
    }
    return 1;
}
// A mod may spell a name in another case: an overlay entry is taken when none is spelled exactly, and logged (a Switch would ignore it).
#define OVERLAY_DELETE ".dm-delete"
static int fold_component(char *parent, size_t n, const char *name) {
    const dir_index *x = dir_index_get(parent);
    for (size_t i = 0; x && i < x->n; i++) if (!strcasecmp(x->e[i].guest, name)) {
        size_t len = strlen(parent), k = strlen(x->e[i].phys);
        if (len + k + 2 > n) return 0;
        parent[len] = '/'; memcpy(parent + len + 1, x->e[i].phys, k + 1); return 1;
    }
    return 0;
}
static void fold_note(const char *found, const char *asked) {
    static uint64_t seen[64]; static int n;
    uint64_t h = fnv1a(found);
    for (int i = 0; i < n; i++) if (seen[i] == h) return;
    if (n == 64) return;
    seen[n++] = h;
    LOG("mods: warning: %s is used for the game's \"%s\": the names differ only in case, which a Switch would not match%s", found, asked, n == 64 ? " (further ones are not listed)" : "");
}
// 1 it has it (out), -1 it deletes it, 0 neither. "<name>.dm-delete" hides <name> in the game and the later folders.
static int overlay_lookup(const char *root, const char *rest, char *out, size_t n) {
    const char *asked = rest; int deleted = 0, folded = 0;
    strlcpy(out, root, n);
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        size_t k = end - rest; char part[0x301], mark[0x301 + sizeof OVERLAY_DELETE], probe[1024];
        if (k >= sizeof part) { errno = ENAMETOOLONG; return 0; }
        memcpy(part, rest, k); part[k] = 0;
        if (!strcmp(part, "..")) return -deleted;
        rest = end;
        if (!strcmp(part, ".")) continue;
        snprintf(mark, sizeof mark, "%s" OVERLAY_DELETE, part); strlcpy(probe, out, sizeof probe);
        if (!deleted && path_component(probe, sizeof probe, mark, 0, 1)) deleted = 1;
        if (path_component(out, n, part, 0, 1)) continue;
        if (!fold_component(out, n, part)) return -deleted;
        folded = 1;
    }
    if (access(out, F_OK)) return -deleted;
    if (folded) fold_note(out, asked);
    return 1;
}
static int rom_overlay_path(const mount_t *m, const char *rest, char *out, size_t n, int *deleted) {
    char *roots = strdup(m->overlay);
    if (!roots) die("rom overlay path allocation failed");
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root; root = strtok_r(NULL, ":", &save)) {
        char candidate[1024];
        int r = overlay_lookup(root, rest, candidate, sizeof candidate);
        if (r > 0) { strlcpy(out, candidate, n); free(roots); return 1; }
        if (r < 0) { if (deleted) *deleted = 1; free(roots); errno = ENOENT; return 0; }
    }
    int found = !m->img && root_path(m->dir, rest, out, n) && !access(out, F_OK);   // an image base is looked up by img_path
    free(roots);
    if (found) return 1;
    errno = ENOENT; return 0;
}
// As Horizon's PathNormalizer: "." dropped, ".." up one, above the root an error (not found here).
static int norm_rest(const char *rest, char *out, size_t n) {
    size_t len = 0; out[0] = 0;
    while (*rest) {
        while (*rest == '/') rest++;
        if (!*rest) break;
        const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
        size_t k = end - rest;
        if (k == 2 && rest[0] == '.' && rest[1] == '.') {
            if (!len) { errno = ENOENT; return 0; }
            while (len && out[len - 1] != '/') len--;
            if (len) len--;
            out[len] = 0;
        } else if (!(k == 1 && rest[0] == '.')) {
            if (len + 1 + k + 1 > n) { errno = ENAMETOOLONG; return 0; }
            out[len++] = '/'; memcpy(out + len, rest, k); len += k; out[len] = 0;
        }
        rest = end;
    }
    return 1;
}
// create: only the last component may be missing
static int host_path_ex(const char *p, char *out, size_t n, int *ro, int create) {
    char fwd[1024]; p = fwd_slashes(p, fwd, sizeof fwd); const char *c = strchr(p, ':');
    if (!c) { errno = ENOENT; return 0; }
    size_t ln = c - p;
    for (int i = 0; i < g_nmnt; i++) {
        if (!mnt_is(&g_mnt[i], p, ln)) continue;
        if (ro) *ro = g_mnt[i].ro;
        char normed[1024]; const char *rest = c + 1;
        if (strstr(rest, "/.")) { if (!norm_rest(rest, normed, sizeof normed)) return 0; rest = normed; }
        if (g_mnt[i].overlay[0] && !create) return rom_overlay_path(&g_mnt[i], rest, out, n, NULL);
        strlcpy(out, g_mnt[i].dir, n);
        while (*rest) {
            while (*rest == '/') rest++;
            if (!*rest) break;
            const char *end = strchr(rest, '/'); if (!end) end = rest + strlen(rest);
            size_t k = end - rest; char part[0x301];
            if (k >= sizeof part) { errno = ENAMETOOLONG; return 0; }
            memcpy(part, rest, k); part[k] = 0;
            if (!strcmp(part, "..")) { errno = ENOENT; return 0; }
            const char *next = end; while (*next == '/') next++;
            if (strcmp(part, ".") && !path_component(out, n, part, create && !*next, g_mnt[i].ro)) return 0;
            rest = end;
        }
        return 1;
    }
    errno = ENOENT; return 0;
}
static int set_guest_name(const char *hp, const char *path) {
    char fwd[1024]; path = fwd_slashes(path, fwd, sizeof fwd);
    const char *end = path + strlen(path); while (end > path && end[-1] == '/') end--;
    const char *start = end; while (start > path && start[-1] != '/' && start[-1] != ':') start--;
    char guest[0x301]; size_t k = end - start;
    if (k >= sizeof guest) { errno = ENAMETOOLONG; return -1; }
    memcpy(guest, start, k); guest[k] = 0;
    const char *physical = strrchr(hp, '/') + 1;
    if (strcmp(physical, guest)) return setxattr(hp, FS_GUEST_NAME, guest, strlen(guest), 0, XATTR_NOFOLLOW);
    if (!removexattr(hp, FS_GUEST_NAME, XATTR_NOFOLLOW) || errno == ENOATTR) return 0;
    return -1;
}
static Result err_res(const char *op, const char *path) {
    if (errno == ENOENT || errno == ENOTDIR) { if (g_flag.fs) LOG("fs %s not found: %s", op, path); return RES_PATH_NOT_FOUND; }   // DM_FS=1 only: games often probe
    if (errno == EEXIST) return RES_PATH_EXISTS;
    LOG("fs %s %s failed errno=%d", op, path, errno); return RES_FAIL;
}
// Refuse the next commit of the transaction behind the path's mount, found by name (a failure before any physical path counts). Path lock held.
static int save_poison_path(const char *path, int e) {
    const char *c = strchr(path, ':');
    for (int i = 0; c && i < g_nmnt; i++) if (mnt_is(&g_mnt[i], path, (size_t)(c - path))) {
        if (g_mnt[i].ro || g_mnt[i].txn_root < 0) return 0;
        save_poison_txn(g_mnt[i].txn_root, e); return 1;
    }
    return 0;
}
// Failures Horizon answers too (the working copy unchanged); any other errno is the host's, which Horizon's save storage never has.
static int fs_guest_error(int e) { return e == ENOENT || e == ENOTDIR || e == EEXIST || e == EISDIR || e == ENOTEMPTY || e == ENAMETOOLONG || e == EILSEQ || e == EINVAL; }
// a host error also refuses the save's next commit
static Result save_path_error(const char *op, const char *path) {
    int e = errno;
    if (!fs_guest_error(e)) save_poison_path(path, e);
    errno = e; return err_res(op, path);
}
// One lock for resolution, create, rename and enumeration: aliases and xattrs publish together.
#define HP_ACCESS(path, create, mutating) PATH_LOCK(); char hp[1024]; int ro = 0; if (!host_path_ex(path, hp, sizeof hp, &ro, create)) return (mutating) ? save_path_error("path", path) : err_res("path", path)
#define HP_EX(path, create) HP_ACCESS(path, create, 0)
#define HP(path) HP_EX(path, 0)
#define RW() if (ro) { LOG("fs write to a read-only mount: %s", hp); return RES_FAIL; }
// An existing target answers "already exists", the only failure the game understands (with a generic one it goes on with a null handle).
#define RW_CREATE() if (ro) { if (!access(hp, F_OK)) return RES_PATH_EXISTS; LOG("fs write to a read-only mount: %s", hp); return RES_FAIL; }

HLE_FORCE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", "fsp-srv; rom reads the host directory directly, the cache gets only the minimum size", Result, h_fs_QueryRomCache, size_t *out) { *out = 0x1000; return RES_OK; }
HLE_FORCE("_ZN2nn2fs8MountRomEPKcPvm", "fsp-srv; rom → host RomFS directory", Result, h_fs_MountRom, const char *name, void *cache, size_t sz) { dm_thread_service_request();
    (void)cache; (void)sz; char b[1024];
    Result r = mount(name, envdir("DM_ROMFS", "romfs", b, sizeof b), 1);
    if (r == RES_OK) {
        PATH_LOCK();
        const char *overlay = overlay_now();
        for (int i = 0; i < g_nmnt; i++) if (mnt_named(&g_mnt[i], name)) {
            g_mnt[i].romfs = 1;
            strlcpy(g_mnt[i].overlay, overlay ? overlay : "", sizeof g_mnt[i].overlay);
            break;
        }
    }
    return r;
}
// The player's own console files (the Mii database)
const char *dm_system_dir(char *buf, size_t n) { return envdir("DM_SYSTEM_DIR", "run/platform/system", buf, n); }
static int host_mounted(const char *name) {
    PATH_LOCK();
    for (int i = 0; name && i < g_nmnt; i++) if (mnt_named(&g_mnt[i], name)) return 1;
    return 0;
}
// Mounts only the SDK holds (MountRamDisk): their paths and the handles they open go to the SDK's own calls; host handles never enter the list. No host lock across an SDK call.
#define SDK_HANDLES 256
static uint64_t g_sdk_h[SDK_HANDLES];                      // FileAccessor / DirectoryAccessor pointers; 0 = free
static _Atomic int g_sdk_nh;                               // 0 skips the lookup in every host file call
static pthread_mutex_t g_sdk_h_lock = PTHREAD_MUTEX_INITIALIZER;
static int sdk_handle_known(uint64_t h) {
    if (!h || !atomic_load(&g_sdk_nh)) return 0;
    pthread_mutex_lock(&g_sdk_h_lock);
    int found = 0; for (int i = 0; i < SDK_HANDLES && !found; i++) found = g_sdk_h[i] == h;
    pthread_mutex_unlock(&g_sdk_h_lock);
    return found;
}
static int sdk_handle_add(uint64_t h) {   // 0 when full
    pthread_mutex_lock(&g_sdk_h_lock);
    int i = 0; while (i < SDK_HANDLES && g_sdk_h[i]) i++;
    if (i < SDK_HANDLES) { g_sdk_h[i] = h; atomic_fetch_add(&g_sdk_nh, 1); }
    pthread_mutex_unlock(&g_sdk_h_lock);
    return i < SDK_HANDLES;
}
static void sdk_handle_forget(uint64_t h) {
    pthread_mutex_lock(&g_sdk_h_lock);
    for (int i = 0; i < SDK_HANDLES; i++) if (g_sdk_h[i] == h) {
        g_sdk_h[i] = 0;
        atomic_fetch_sub(&g_sdk_nh, 1);
        break;
    }
    pthread_mutex_unlock(&g_sdk_h_lock);
}
static int sdk_mounted(const char *name) {
    int (*is_mounted)(const char *) = guest_sym("_ZN2nn2fs9IsMountedEPKc");
    return name && is_mounted && is_mounted(name);
}
static int path_mount_name(const char *path, char *name, size_t n) {   // the part before ':' as in FindFileSystem; 0 without one
    const char *c = path ? strchr(path, ':') : NULL;
    if (!c || (size_t)(c - path) >= n) return 0;
    memcpy(name, path, (size_t)(c - path)); name[c - path] = 0;
    return 1;
}
static void *sdk_path_call(const char *path, const char *sym) {
    char name[0x30];
    if (!path_mount_name(path, name, sizeof name) || host_mounted(name) || !sdk_mounted(name)) return NULL;
    void *f = guest_sym(sym);
    if (!f) LOG("[RED] fs %s is on the SDK's mount %s, but the SDK defines no %s", path, name, sym);
    return f;
}
// NULL is reported and the call fails: host code would take the SDK's pointer for its own handle
static void *sdk_handle_sym(uint64_t h, const char *sym) {
    void *f = guest_sym(sym);
    if (!f) LOG("[RED] fs handle %#llx is the SDK's, but the SDK defines no %s", (unsigned long long)h, sym);
    return f;
}
// or closed in the SDK when the list is full
static Result sdk_opened(Result r, uint64_t *out, const char *close_sym) {
    if (r != RES_OK || sdk_handle_add(*out)) return r;
    LOG("[RED] fs: more than %d handles open on the SDK's own mounts, the open fails", SDK_HANDLES);
    void (*close_h)(uint64_t) = guest_sym(close_sym);
    if (close_h) close_h(*out);
    *out = 0; return RES_FAIL;
}
#define SDK_PATH(path, sym, type, ...) do { __typeof__(type) sdk_f = (__typeof__(type))sdk_path_call(path, sym); if (sdk_f) return sdk_f(__VA_ARGS__); } while (0)
#define SDK_HANDLE(h, sym, type, fail, ...) do { if (sdk_handle_known(h)) { __typeof__(type) sdk_f = (__typeof__(type))sdk_handle_sym(h, sym); return sdk_f ? sdk_f(__VA_ARGS__) : (fail); } } while (0)
// nnmusl checks every path with these before nn::fs; the SDK's empty mount table would fail every C library file call.
HLE_FORCE("_ZN2nn2fs13IsPathMountedEPKc", "fsp-srv; looks up the host mount table, then the SDK's own (a RAM disk)", int, h_fs_IsPathMounted, const char *path) {   // bool in w0
    const char *c = path ? strchr(path, ':') : NULL;
    {   PATH_LOCK();
        for (int i = 0; c && i < g_nmnt; i++) if (mnt_is(&g_mnt[i], path, (size_t)(c - path))) return 1;
    }
    char name[0x30];   // the SDK is asked with no host lock held
    if (path_mount_name(path, name, sizeof name) && sdk_mounted(name)) return 1;
    if (g_flag.fs) LOG("fs path not mounted: %s", path ? path : "(null)");
    return 0;
}
// Horizon's 0x300 bytes; no UTF-16 limit (false skips that check)
HLE_FORCE("_ZN2nn2fs16GetPathLengthMaxEPmPKc", "fsp-srv; every host mount takes Horizon's 0x300-byte paths", Result, h_fs_GetPathLengthMax, uint64_t *out, const char *path) {
    (void)path; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs21GetEntryNameLengthMaxEPmPKc", "fsp-srv; every host mount takes Horizon's 0x300-byte entry names", Result, h_fs_GetEntryNameLengthMax, uint64_t *out, const char *path) {
    (void)path; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs21GetUtf16PathLengthMaxEPbPmPKc", "fsp-srv; host mounts have no UTF-16 path limit", Result, h_fs_GetUtf16PathLengthMax, uint8_t *limited, uint64_t *out, const char *path) {
    (void)path; *limited = 0; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs26GetUtf16EntryNameLengthMaxEPbPmPKc", "fsp-srv; host mounts have no UTF-16 entry name limit", Result, h_fs_GetUtf16EntryNameLengthMax, uint8_t *limited, uint64_t *out, const char *path) {
    (void)path; *limited = 0; *out = 0x300; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs21MountTemporaryStorageEPKc", "fsp-srv; temporary storage → host directory", Result, h_fs_MountTemp, const char *name) {
    char b[1024]; return mount(name, envdir("DM_TEMP", "run/platform/temp", b, sizeof b), 0);
}
// Only the practice tools mount the SD card: DM_SDCARD or <DM_ROOT>/sdcard, for MountSdCard and MountSdCardForDebug alike.
HLE_ADDON("_ZN2nn2fs11MountSdCardEPKc", "fsp-srv; the SD card → host directory (practice tools)", Result, h_fs_MountSd, const char *name) {
    char b[1024]; return mount(name, envdir("DM_SDCARD", "sdcard", b, sizeof b), 0);
}
HLE_ADDON("_ZN2nn2fs19MountSdCardForDebugEPKc", "fsp-srv; the SD card → host directory (practice tools)", Result, h_fs_MountSdDebug, const char *name) {
    char b[1024]; return mount(name, envdir("DM_SDCARD", "sdcard", b, sizeof b), 0);
}
HLE_FORCE("_ZN2nn2fs16GetFreeSpaceSizeEPlPKc", "fsp-srv; free space of a mount: 256 MB (the host directory has no size limit of its own)", Result, h_fs_FreeSpace, int64_t *out, const char *path) {
    (void)path; if (out) *out = 256 << 20; return RES_OK;
}
// user 1: DM_SAVE; user N: <DM_SAVE>_userN, each with its own transaction
static int user_save_dir(char *dd, size_t cap, const char *save, int user) {
    size_t n = strlen(save); while (n > 1 && save[n - 1] == '/') n--;
    if (!n || n >= cap) return -1;
    memcpy(dd, save, n); dd[n] = 0;
    if (user > 0) {
        int len = snprintf(dd + n, cap - n, "_user%d", user + 1);
        if (len < 0 || (size_t)len >= cap - n) return -1;
    }
    return 0;
}
HLE_FORCE("_ZN2nn2fs13MountSaveDataEPKcRKNS_7account3UidE", "fsp-srv; each registered account has its own host save transaction", Result, h_fs_MountSave, const char *name, const void *uid) { dm_thread_service_request();
    int user = dm_account_user(uid);   // an unknown UID never falls back to user 1
    if (user < 0) return 0x7d402u;
    // No default: runs use a fresh copy of save_ref.
    const char *d = getenv("DM_SAVE");
    if (!d) die("DM_SAVE is not set: point it at a writable copy of a save directory");
    char dd[1024]; if (user_save_dir(dd, sizeof dd, d, user)) return RES_FAIL;
    return mount_ex(name, dd, 0, 1);
}
static _Atomic uint64_t g_save_commits;
uint64_t dm_fs_commit_count(void) { return atomic_load(&g_save_commits); }   // for the unit tests
// Path and mount locks held; the front end is told after unlocking.
static Result commit_mount_locked(int i, int *notify_error) {
    int txn = g_mnt[i].txn_root;
    if (txn < 0 || txn >= g_nmnt) return RES_FAIL;
    pthread_once(&g_txn_lock_once, txn_locks_init);
    pthread_rwlock_wrlock(&g_txn_lock[txn]);
    pthread_rwlock_t *txn_guard __attribute__((cleanup(txn_unlock))) = &g_txn_lock[txn];
    uint64_t tc1 = clock_gettime_nsec_np(CLOCK_MONOTONIC), tw0 = atomic_exchange(&g_mnt[txn].first_write_ns, 0);
    int perr = atomic_exchange(&g_mnt[txn].poison, 0);
    // Probe (open): Horizon is believed to refuse a commit with a file open for writing; the host commits, and a handle into a recloned tree loses its writes.
    int nopen = 0, nwrite = 0; uint16_t token = (uint16_t)(txn + 1);
    for (int k = 0; k < 65536; k++) if (atomic_load(&g_ftxn[k]) == token) { nopen++; nwrite += (g_fmode[k] & (MODE_WRITE | MODE_APPEND)) != 0; }
    if (nwrite) {
        static int red;
        if (red++ < 8) LOG("[RED] save commit with %d file(s) open for writing in this transaction (%d open in all); Horizon refuses such a commit, the host goes on", nwrite, nopen);
    }
    if (perr) {                                                         // a write failed in this transaction: refuse, discard the work directory
        char torn[512] = ""; size_t tl = 0; DIR *d0 = opendir(g_mnt[i].dir); struct dirent *e0;   // files truncated to 0 bytes (what the commit would have saved)
        while (d0 && (e0 = readdir(d0))) {
            if (e0->d_name[0] == '.') continue;
            char sub[1300];
            snprintf(sub, sizeof sub, "%s/%s", g_mnt[i].dir, e0->d_name);
            DIR *d1 = opendir(sub);
            struct dirent *e1;
            while (d1 && (e1 = readdir(d1))) { char f[1600]; struct stat st; snprintf(f, sizeof f, "%s/%s", sub, e1->d_name);
                if (e1->d_name[0] != '.' && !stat(f, &st) && S_ISREG(st.st_mode) && !st.st_size && tl + strlen(e0->d_name) + strlen(e1->d_name) + 3 < sizeof torn)
                    tl += snprintf(torn + tl, sizeof torn - tl, " %s/%s", e0->d_name, e1->d_name); }
            if (d1) closedir(d1);
        }
        if (d0) closedir(d0);
        // Clone the committed version beside, then swap it with the tainted work directory. On failure the taint stays and the next commit retries.
        char re[1100]; snprintf(re, sizeof re, "%s.re", g_mnt[i].dir); int rerr = 0;
        if (!access(re, F_OK) && rm_tree(re)) rerr = errno ? errno : EIO;
        else if (dm_fs_test_fail("reclone") || clonefile(g_mnt[i].commit, re, 0)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else if (renamex_np(re, g_mnt[i].dir, RENAME_SWAP)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else rm_tree(re);
        if (rerr) {
            save_poison_txn(txn, perr);
            LOG("[RED] save transaction: recloning the work directory failed errno=%d -- the work directory is left as it is and stays tainted (the next commit is refused again and retries), the committed version is unchanged", rerr);
        }
        LOG("[RED] save commit refused: a save write failed in this transaction errno=%d (0-byte files in the work directory: %s); the committed version is unchanged, the work directory %s%s", perr, tl ? torn + 1 : "none", rerr ? "could not be recloned (see the previous line)" : "was recloned from the committed version",
            nopen && !rerr ? "; [RED] save files are still open: they point into the discarded work directory, and what is written through them is lost" : nopen ? "; [RED] save files are still open" : "");
        *notify_error = perr;
        return RES_FAIL;
    }
    char neu[1100]; snprintf(neu, sizeof neu, "%s.new", g_mnt[i].commit);
    Result r = RES_OK;
    if (!access(neu, F_OK) && rm_tree(neu)) {
        LOG("[RED] save commit: cannot clear the %s left over errno=%d (committed version unchanged)", neu, errno);
        r = RES_FAIL;
    }
    else if (clonefile(g_mnt[i].dir, neu, 0)) { LOG("[RED] save commit: clonefile %s failed errno=%d (committed version unchanged)", neu, errno); r = RES_FAIL; }
    else if (renamex_np(neu, g_mnt[i].commit, RENAME_SWAP)) {
        LOG("[RED] save commit: atomic swap failed errno=%d (committed version unchanged)", errno);
        rm_tree(neu);
        r = RES_FAIL;
    }
    else rm_tree(neu);
    // microseconds: clone + swap; first save write to commit end (0 = none)
    static int n; if (r == RES_OK) { uint64_t tc2 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        LOG("save commit #%d: %s swapped atomically into %s (%d save writes so far) (clone + swap %llu us, since this transaction's first write %llu us)", ++n, g_mnt[i].dir, g_mnt[i].commit, atomic_load(&g_save_writes),
            (unsigned long long)((tc2 - tc1) / 1000), (unsigned long long)(tw0 ? (tc2 - tw0) / 1000 : 0)); atomic_fetch_add(&g_save_commits, 1);
    }
    return r;
}
HLE_FORCE("_ZN2nn2fs6CommitEPKc", "fsp-srv; transactional save: the whole work directory tree is cloned, then swapped atomically with the committed directory (renamex_np RENAME_SWAP)", Result, h_fs_Commit, const char *name) { dm_thread_service_request();
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    int i = 0; while (i < g_nmnt && !mnt_named(&g_mnt[i], name)) i++;
    if (i == g_nmnt || !g_mnt[i].commit[0]) { pthread_mutex_unlock(&g_lock); return RES_OK; }
    int notify_error = 0; Result r = commit_mount_locked(i, &notify_error);
    pthread_mutex_unlock(&g_lock);
    if (notify_error) front_save_failed(notify_error);
    return r;
}
// Host mounts are permanent. An SDK mount (MountRamDisk) is released by the SDK, after IsMounted (its Unmount aborts on unknown names). No host lock across.
HLE_FORCE("_ZN2nn2fs7UnmountEPKc", "fsp-srv; host mounts are permanent, a no-op; a mount in the SDK's own table (RAM disk) is released there", void, h_fs_Unmount, const char *name) {
    if (!name) return;
    if (host_mounted(name)) return;
    int (*sdk_is_mounted)(const char *) = guest_sym("_ZN2nn2fs9IsMountedEPKc");
    void (*sdk_unmount)(const char *) = guest_sym("_ZN2nn2fs7UnmountEPKc");
    if (!sdk_is_mounted || !sdk_unmount || !sdk_is_mounted(name)) return;
    LOG("fs unmount %s: in the SDK's own mount table, released there", name);
    sdk_unmount(name);
}

HLE_FORCE("_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", "fsp-srv → open(2)", Result, h_fs_OpenFile, uint64_t *out, const char *path, int mode) { dm_thread_service_request();
    Result (*sdk_open)(uint64_t *, const char *, int) = sdk_path_call(path, "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci");
    if (sdk_open) return sdk_opened(sdk_open(out, path, mode), out, "_ZN2nn2fs9CloseFileENS0_10FileHandleE");
    int mi; int64_t k;
    if (img_path(path, &mi, &k)) {
        if (k < 0) return err_res("open", path);
        if (!g_mnt[mi].img->e[k].type) return RES_PATH_NOT_FOUND;
        if (mode & (MODE_WRITE | MODE_APPEND)) { LOG("fs write to a read-only mount: %s", path); return RES_FAIL; }
        *out = FH_OUT(IMG_HANDLE(mi, k));
        if (g_flag.fs) LOG("fs open [%.2fs] %s mode=%d → image entry %lld", (now_ns() - g_t0_ns) / 1e9, path, mode, (long long)k);
        return RES_OK;
    }
    HP_ACCESS(path, 0, mode & (MODE_WRITE | MODE_APPEND));
    if (mode & (MODE_WRITE | MODE_APPEND)) RW();
    int fd = open(hp, (mode & (MODE_WRITE | MODE_APPEND)) ? ((mode & MODE_READ) ? O_RDWR : O_WRONLY) : O_RDONLY);
    if (fd < 0) return mode & (MODE_WRITE | MODE_APPEND) ? save_path_error("open", path) : err_res("open", hp);
    struct stat st; fstat(fd, &st);
    if (S_ISDIR(st.st_mode)) { close(fd); return RES_PATH_NOT_FOUND; }
    if (fd >= (int)sizeof g_fmode) die("fd beyond the table");
    g_fmode[fd] = (uint8_t)mode; *out = FH_OUT(fd);
    { const char *c = strchr(path, ':'); size_t ln = c ? (size_t)(c - path) : 0; uint16_t token = 0;
      for (int i = 0; c && i < g_nmnt; i++) if (mnt_is(&g_mnt[i], path, ln) && g_mnt[i].txn_root >= 0) { token = (uint16_t)(g_mnt[i].txn_root + 1); break; }
      atomic_store(&g_ftxn[fd], token); }
    fslog_init();
    if (g_fslog && fd < 4096) {
        pthread_mutex_lock(&g_fslog_lock);
        free(g_fdpath[fd]);
        g_fdpath[fd] = strdup(path);
        pthread_mutex_unlock(&g_fslog_lock);
    }
    if (g_flag.fs) LOG("fs open [%.2fs] %s mode=%d → fd %d", (now_ns() - g_t0_ns) / 1e9, path, mode, fd);
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", "fsp-srv → pread(2)", Result, h_fs_ReadFile, size_t *out, uint64_t h, int64_t off, void *buf, size_t sz) { dm_thread_service_request();
    SDK_HANDLE(h, "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", Result (*)(size_t *, uint64_t, int64_t, void *, size_t), RES_FAIL, out, h, off, buf, sz);
    h = FH_IN(h);
    if (!h) { *out = 0; return RES_FAIL; }   // 0 is stdin, never a game file: the null handle kept after a failed open
    size_t got = 0; int reprep = 0, fd = (int)h; int64_t base = 0;
    if (IMG_H(h)) {   // image entry: pread at its offset, never past its end
        const img_ent *x = &g_mnt[IMG_M(h)].img->e[IMG_K(h)];
        fd = g_mnt[IMG_M(h)].img->fd; base = (int64_t)x->a;
        sz = off < 0 || (uint64_t)off >= x->b ? 0 : (size_t)(x->b - off) < sz ? (size_t)(x->b - off) : sz;
    }
    dm_guest_write_prepare(buf, sz);   // write-protected texture pages: a kernel write gets EFAULT instead of faulting, so unprotect and mark dirty first
    while (got < sz) {
        ssize_t r = pread(fd, (uint8_t *)buf + got, sz - got, base + off + got);
        if (r < 0) { if (errno == EINTR) continue;
            // nvn_dirty.c may protect the pages again before pread writes: prepare again and read on
            if (errno == EFAULT && reprep++ < 8) {
                static _Atomic int once;
                if (!once++) LOG("[fs] ReadFile into %p+%#zx: the destination was write-protected again during the read, prepared again at +%#zx (first case)", buf, sz, got);
                dm_guest_write_prepare((uint8_t *)buf + got, sz - got); continue; }
            if (errno == EFAULT) {
                int e = errno;
                vm_address_t ra = (vm_address_t)((uint8_t *)buf + got);
                vm_size_t rs = 0;
                vm_region_basic_info_data_64_t vi;
                mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
                mach_port_t obj = MACH_PORT_NULL;
                kern_return_t kr = vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj);
                LOG("[RED] fs ReadFile into %p+%#zx failed at +%#zx (EFAULT): VM region %#lx+%#lx protection %d%s", buf, sz, got, (unsigned long)ra, (unsigned long)rs, kr == KERN_SUCCESS ? vi.protection : -1, kr == KERN_SUCCESS ? "" : " (none)");
                errno = e; }
            return err_res("pread", "fd"); }
        if (r == 0) break;
        got += r;
    }
    *out = got;
    if (g_fslog) { pthread_mutex_lock(&g_fslog_lock);
        static int hash_on = -1; if (hash_on < 0) hash_on = getenv("DM_FSLOG_HASH") != NULL;
        uint64_t fh = 0xcbf29ce484222325ull; if (hash_on) for (size_t i = 0; i < got; i++) fh = (fh ^ ((uint8_t *)buf)[i]) * 0x100000001b3ull;
        if (hash_on) fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\t%016llx\n", (now_ns() - g_t0_ns) / 1e9, !IMG_H(h) && (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf, (unsigned long long)fh);
        else fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\n", (now_ns() - g_t0_ns) / 1e9, !IMG_H(h) && (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf);
        pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) {
        static uint64_t n, bytes, next_b = 64 << 20; static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&m); n++; bytes += got;
        if (bytes >= next_b || n % 1000 == 0) {
            LOG("fs read total: %llu reads %.1f MiB (this one: fd %d offset %#llx length %#zx)", n, bytes / 1048576.0, (int)h, off, got);
            if (bytes >= next_b) next_b += 64 << 20;
        }
        pthread_mutex_unlock(&m);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "fsp-srv → pwrite(2)", Result, h_fs_WriteFile, uint64_t h, int64_t off, const void *buf, size_t sz, const void *opt) { dm_thread_service_request();
    SDK_HANDLE(h, "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", Result (*)(uint64_t, int64_t, const void *, size_t, const void *), RES_FAIL, h, off, buf, sz, opt);
    h = FH_IN(h);
    if (!h || IMG_H(h)) return RES_FAIL;
    int fd = (int)h & 0xffff;
    uint16_t token = atomic_load(&g_ftxn[fd]);
    int txn = token ? token - 1 : -1;
    pthread_rwlock_t *txn_guard __attribute__((cleanup(txn_unlock))) = NULL;
    if (txn >= 0) { txn_guard = &g_txn_lock[txn]; pthread_rwlock_rdlock(txn_guard); }
    (void)opt; size_t put = 0;
    if (txn >= 0) {
        pthread_once(&g_save_test_once, save_test_init);
        int n = atomic_fetch_add(&g_save_writes, 1) + 1;
        { uint64_t z = 0; atomic_compare_exchange_strong(&g_mnt[txn].first_write_ns, &z, clock_gettime_nsec_np(CLOCK_MONOTONIC)); }
        if (g_save_kill_n && n == g_save_kill_n) { LOG("DM_TEST_KILL_AT: save write #%d, calling _exit (simulates being killed mid-write)", n); _exit(9); }
        if (g_save_fail_n && n == g_save_fail_n) {
            LOG("DM_TEST_FS_FAIL: save write #%d returns failure errno=%d", n, g_save_fail_e);
            errno = g_save_fail_e;
            save_poison_txn(txn, g_save_fail_e);
            return err_res("pwrite (injected)", "fd");
        }
    }
    while (put < sz) {
        ssize_t r = pwrite((int)h, (const uint8_t *)buf + put, sz - put, off + put);
        if (r < 0) {
            if (errno == EINTR) continue;
            if (txn >= 0) save_poison_txn(txn, errno);
            return err_res("pwrite", "fd");
        }
        if (r == 0) {   // no progress: report it rather than loop
            errno = ENOSPC;
            if (txn >= 0) save_poison_txn(txn, ENOSPC);
            return err_res("pwrite (wrote 0 bytes)", "fd");
        }
        put += r;
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", "fsp-srv → fstat(2)", Result, h_fs_GetFileSize, int64_t *out, uint64_t h) { dm_thread_service_request();
    SDK_HANDLE(h, "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", Result (*)(int64_t *, uint64_t), RES_FAIL, out, h);
    h = FH_IN(h);
    if (!h) return RES_FAIL;
    if (IMG_H(h)) { *out = (int64_t)g_mnt[IMG_M(h)].img->e[IMG_K(h)].b; return RES_OK; }
    struct stat st; if (fstat((int)h, &st)) return err_res("fstat", "fd");
    *out = st.st_size; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", "fsp-srv → ftruncate(2)", Result, h_fs_SetFileSize, uint64_t h, int64_t sz) { dm_thread_service_request();
    SDK_HANDLE(h, "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", Result (*)(uint64_t, int64_t), RES_FAIL, h, sz);
    h = FH_IN(h);
    if (!h || IMG_H(h)) return RES_FAIL;
    int fd = (int)h & 0xffff;
    uint16_t token = atomic_load(&g_ftxn[fd]);
    int txn = token ? token - 1 : -1;
    pthread_rwlock_t *txn_guard __attribute__((cleanup(txn_unlock))) = NULL;
    if (txn >= 0) { txn_guard = &g_txn_lock[txn]; pthread_rwlock_rdlock(txn_guard); }
    if (!ftruncate((int)h, sz)) return RES_OK;
    if (txn >= 0) save_poison_txn(txn, errno);
    return err_res("ftruncate", "fd");
}
HLE_FORCE("_ZN2nn2fs9FlushFileENS0_10FileHandleE", "fsp-srv; host writes are already in the page cache, flushing is a no-op", Result, h_fs_FlushFile, uint64_t h) { dm_thread_service_request();
    SDK_HANDLE(h, "_ZN2nn2fs9FlushFileENS0_10FileHandleE", Result (*)(uint64_t), RES_FAIL, h);
    return RES_OK; }
HLE_FORCE("_ZN2nn2fs9CloseFileENS0_10FileHandleE", "fsp-srv → close(2)", void, h_fs_CloseFile, uint64_t h) { dm_thread_service_request();
    if (sdk_handle_known(h)) {   // forgotten first: the accessor's address may come back from the next open
        sdk_handle_forget(h);
        void (*sdk_close)(uint64_t) = sdk_handle_sym(h, "_ZN2nn2fs9CloseFileENS0_10FileHandleE");
        if (sdk_close) sdk_close(h);
        return;
    }
    h = FH_IN(h);
    if (!h || IMG_H(h)) return;   // the null handle after a failed open; an image entry holds no descriptor
    g_fmode[(int)h & 0xffff] = 0; atomic_store(&g_ftxn[(int)h & 0xffff], 0);
    close((int)h); }
HLE_FORCE("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", "fsp-srv; the open mode is recorded by the host", int, h_fs_GetFileOpenMode, uint64_t h) {
    SDK_HANDLE(h, "_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", int (*)(uint64_t), 0, h);
    h = FH_IN(h); return IMG_H(h) ? MODE_READ : g_fmode[(int)h & 0xffff]; }

HLE_FORCE("_ZN2nn2fs10CreateFileEPKcl", "fsp-srv → open(O_CREAT|O_EXCL)+ftruncate", Result, h_fs_CreateFile, const char *path, int64_t sz) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs10CreateFileEPKcl", Result (*)(const char *, int64_t), path, sz);
    int mi; int64_t k; if (img_path(path, &mi, &k)) return k >= 0 ? RES_PATH_EXISTS : RES_FAIL;
    HP_ACCESS(path, 1, 1); RW_CREATE();
    int fd = open(hp, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) return save_path_error("create", path);
    int rc = ftruncate(fd, sz); int e = errno; close(fd);
    if (!rc && set_guest_name(hp, path)) { rc = -1; e = errno; }
    if (rc) unlink(hp);
    errno = e; return rc ? save_path_error("create", path) : RES_OK;
}
HLE_FORCE("_ZN2nn2fs10DeleteFileEPKc", "fsp-srv → unlink(2)", Result, h_fs_DeleteFile, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs10DeleteFileEPKc", Result (*)(const char *), path);
    HP_ACCESS(path, 0, 1); RW();
    if (!unlink(hp)) return RES_OK;
    // Horizon keeps files and directories apart: DeleteFile on a directory answers PathNotFound. Apple's unlink(2) gives EPERM there, not a guest error.
    int e = errno; struct stat st;
    if ((e == EPERM || e == EISDIR) && !lstat(hp, &st) && S_ISDIR(st.st_mode)) { if (g_flag.fs) LOG("fs unlink of a directory, not found as a file: %s", path); return RES_PATH_NOT_FOUND; }
    errno = e; return save_path_error("unlink", path); }
HLE_FORCE("_ZN2nn2fs15CreateDirectoryEPKc", "fsp-srv → mkdir(2)", Result, h_fs_CreateDirectory, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs15CreateDirectoryEPKc", Result (*)(const char *), path);
    int mi; int64_t k; if (img_path(path, &mi, &k)) return k >= 0 ? RES_PATH_EXISTS : RES_FAIL;
    HP_ACCESS(path, 1, 1); RW_CREATE();
    if (mkdir(hp, 0755)) return save_path_error("mkdir", path);
    if (set_guest_name(hp, path)) {
        int e = errno;
        rmdir(hp);
        errno = e;
        return save_path_error("mkdir name", path);
    }
    return RES_OK; }
// lstat, not d_type: a file system may report DT_UNKNOWN; symlinks are never followed
static int rm_entry(const char *c) {
    struct stat st; if (lstat(c, &st)) return -1;
    return S_ISDIR(st.st_mode) ? rm_tree(c) : unlink(c);
}
static int rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }   // opendir would follow the link and empty its target
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; char c[1024]; int rc = 0;
    while ((e = dir_next(d))) {
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if ((rc = rm_entry(c))) break;   // before readdir can clear errno
    }
    int err = errno; closedir(d);
    if (rc || err) { errno = err; return -1; }
    return rmdir(p);
}
HLE_FORCE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", "fsp-srv → recursive delete (only inside writable mounts)", Result, h_fs_DeleteDirRec, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", Result (*)(const char *), path);
    HP_ACCESS(path, 0, 1); RW(); return rm_tree(hp) ? save_path_error("rmtree", path) : RES_OK;
}
HLE_FORCE("_ZN2nn2fs15DeleteDirectoryEPKc", "fsp-srv → rmdir(2) (the directory must be empty)", Result, h_fs_DeleteDir, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs15DeleteDirectoryEPKc", Result (*)(const char *), path);
    HP_ACCESS(path, 0, 1); RW(); return rmdir(hp) ? save_path_error("rmdir", path) : RES_OK;
}
static Result rename_entry(const char *from, const char *to) {
    HP_ACCESS(from, 0, 1); RW(); char hp2[1024];
    if (!host_path_ex(to, hp2, sizeof hp2, NULL, 1)) return save_path_error("rename path", to);
    if (!access(hp2, F_OK)) return RES_PATH_EXISTS;
    if (errno != ENOENT && errno != ENOTDIR) return save_path_error("rename probe", to);
    if (rename(hp, hp2)) return save_path_error("rename", from);
    if (dm_fs_test_fail("guestname") || set_guest_name(hp2, to)) { int e = errno ? errno : EIO;
        if (dm_fs_test_fail("renameback") || rename(hp2, hp)) {   // the rollback failed too: the transaction's commit is refused (recloning restores it)
            int re = errno, saved = save_poison_path(from, e);
            LOG("[RED] fs rename: writing the name metadata failed and so did the rollback errno=%d: %s stays at the new path%s", re, hp2, saved ? ", the save's commit will be refused" : "");
        }
        errno = e; return save_path_error("rename name", from); }
    return RES_OK;
}
// Horizon refuses a rename across two mounts (ResultRenameToOtherFileSystem) before touching either, even two names of one host folder.
static Result rename_any(const char *from, const char *to, const char *sym) {
    char a[0x30], b[0x30];
    if (path_mount_name(from, a, sizeof a) && path_mount_name(to, b, sizeof b) && strcmp(a, b)) {
        LOG("fs rename %s → %s: two mounts, refused", from, to);
        return RES_RENAME_OTHER_FS;
    }
    Result (*sdk_rename)(const char *, const char *) = sdk_path_call(from, sym);
    return sdk_rename ? sdk_rename(from, to) : rename_entry(from, to);
}
HLE_FORCE("_ZN2nn2fs10RenameFileEPKcS2_", "fsp-srv → rename(2)", Result, h_fs_RenameFile, const char *from, const char *to) { dm_thread_service_request(); return rename_any(from, to, "_ZN2nn2fs10RenameFileEPKcS2_"); }
HLE_FORCE("_ZN2nn2fs15RenameDirectoryEPKcS2_", "fsp-srv → rename(2) of a directory", Result, h_fs_RenameDir, const char *from, const char *to) { dm_thread_service_request(); return rename_any(from, to, "_ZN2nn2fs15RenameDirectoryEPKcS2_"); }
HLE_FORCE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", "fsp-srv → stat(2)", Result, h_fs_GetEntryType, int32_t *out, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", Result (*)(int32_t *, const char *), out, path);
    int mi; int64_t k;
    if (img_path(path, &mi, &k)) {
        if (k < 0) return err_res("stat", path);
        *out = g_mnt[mi].img->e[k].type ? ENT_FILE : ENT_DIR;
        return RES_OK;
    }
    HP(path); struct stat st;
    if (stat(hp, &st)) return err_res("stat", hp);
    *out = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; return RES_OK;
}

typedef struct dirh { DIR *d; char path[1024]; DirectoryEntry *snap; int64_t nsnap, psnap; int mode; struct dirh *prev, *next; } dirh_t;
// for dm_fs_reset
static dirh_t *g_dirs;
static pthread_mutex_t g_dirs_mu = PTHREAD_MUTEX_INITIALIZER;
static void dir_track(dirh_t *h) {
    pthread_mutex_lock(&g_dirs_mu);
    h->prev = NULL;
    h->next = g_dirs;
    if (g_dirs) g_dirs->prev = h;
    g_dirs = h;
    pthread_mutex_unlock(&g_dirs_mu);
}
static void dir_untrack(dirh_t *h) {
    pthread_mutex_lock(&g_dirs_mu);
    if (h->prev) h->prev->next = h->next;
    else if (g_dirs == h) g_dirs = h->next;
    if (h->next) h->next->prev = h->prev;
    pthread_mutex_unlock(&g_dirs_mu);
}
typedef struct { const char *name; size_t at; } name_at;
static int name_at_cmp(const void *a, const void *b) {
    const name_at *x = a, *y = b; int c = strcmp(x->name, y->name);
    return c ? c : (x->at > y->at) - (x->at < y->at);
}
static int name_at_casecmp(const void *a, const void *b) {
    const name_at *x = a, *y = b; int c = strcasecmp(x->name, y->name);
    return c ? c : (x->at > y->at) - (x->at < y->at);
}
// The first of each name, case-folded as the folders hold them and spelled as the game spells it (image, else base, else the first folder). One sort, not quadratic.
static size_t names_first(char (*names)[0x301], size_t n, size_t nover, size_t nfold) {
    name_at *by = malloc((n ? n : 1) * sizeof *by); uint8_t *again = calloc(n ? n : 1, 1);
    if (!by || !again) die("rom overlay directory snapshot allocation failed");
    for (size_t i = 0; i < n; i++) by[i] = (name_at){names[i], i};
    qsort(by, n, sizeof *by, name_at_casecmp);
    for (size_t g = 0, e; g < n; g = e) {
        for (e = g + 1; e < n && !strcasecmp(by[e].name, by[g].name); e++) {}
        size_t f = g; while (f < e && by[f].at < nfold) f++;
        if (f == g || (f - g == 1 && f == e)) continue;
        size_t src = by[g].at;
        if (f < e) src = by[f].at;
        else for (size_t k = g; k < f; k++) if (by[k].at >= nover) { src = by[k].at; break; }
        for (size_t k = g; k < f; k++) if (by[k].at != src) strlcpy(names[by[k].at], names[src], sizeof names[0]);
    }
    qsort(by, n, sizeof *by, name_at_cmp);
    for (size_t k = 1; k < n; k++) if (!strcmp(by[k].name, by[k - 1].name)) again[by[k].at] = 1;
    size_t m = 0;
    for (size_t i = 0; i < n; i++) if (!again[i]) { if (m != i) memcpy(names[m], names[i], sizeof names[0]); m++; }
    free(again); free(by);
    return m;
}
// -1 with errno when an overlay folder cannot be read
static int dir_snapshot(dirh_t *h, const mount_t *m, const char *rest) {
    char fwd[1024]; rest = fwd_slashes(rest, fwd, sizeof fwd);
    char *roots = strdup(m->overlay);
    size_t cap = 2048; char (*names)[0x301] = malloc(cap * sizeof *names);
    const char **all = calloc(65, sizeof *all);
    char *parent = malloc(1024), *guest = malloc(0x301), *rel = malloc(2048), *resolved = malloc(1024);
    if (!roots || !names || !all || !parent || !guest || !rel || !resolved) die("rom overlay directory snapshot allocation failed");
    size_t nn = 0; int nr = 0, err = 0;
    all[nr++] = m->dir;
    char *save = NULL;
    for (char *root = strtok_r(roots, ":", &save); root && nr < 65; root = strtok_r(NULL, ":", &save)) all[nr++] = root;
    int last = nr - 1, base = 1;   // a deleting overlay folder hides the later ones and the game's own
    for (int ri = 1; ri < nr; ri++) if (overlay_lookup(all[ri], rest, parent, 1024) < 0) {
        last = ri;
        base = 0;
        break;
    }
    size_t nover = 0;   // overlay names first: the base folder (ri 0) last
    for (int ri = last; ri >= !base; ri--) {
        if (!ri) nover = nn;
        if (ri ? overlay_lookup(all[ri], rest, parent, 1024) <= 0 : !root_path(all[ri], rest, parent, 1024)) continue;
        DIR *x = opendir(parent); if (!x) continue;
        struct dirent *e;
        while ((e = dir_next(x))) {
            if (!guest_name(parent, e->d_name, guest, 0x301)) continue;
            size_t gl = strlen(guest);
            if (ri && gl > sizeof OVERLAY_DELETE - 1 && !strcmp(guest + gl - (sizeof OVERLAY_DELETE - 1), OVERLAY_DELETE)) continue;
            if (nn == cap) names = dm_realloc(names, (cap *= 2) * sizeof *names, "a rom overlay directory snapshot");
            strlcpy(names[nn++], guest, sizeof names[0]);
        }
        err = errno; closedir(x);
        if (err) goto done;
    }
    size_t nfold = nn; if (!base) nover = nfold;
    int64_t dk = m->img && base ? img_find(m->img, rest) : -1;
    if (dk >= 0 && !m->img->e[dk].type)
        for (uint64_t j = 0; j < m->img->e[dk].b; j++) {
            const img_ent *c = &m->img->e[m->img->e[dk].a + j]; size_t l = c->nlen < 0x300 ? c->nlen : 0x300;
            memcpy(guest, m->img->names + c->name, l); guest[l] = 0;
            if (nn == cap) names = dm_realloc(names, (cap *= 2) * sizeof *names, "a rom overlay directory snapshot");
            strlcpy(names[nn++], guest, sizeof names[0]);
        }
    nn = names_first(names, nn, nover, nfold);
    h->snap = calloc(nn ? nn : 1, sizeof *h->snap); h->nsnap = 0;
    if (!h->snap) die("rom overlay directory snapshot allocation failed");
    for (size_t j = 0; j < nn; j++) {
        DirectoryEntry *o = &h->snap[h->nsnap];
        snprintf(rel, 2048, "%s/%s", rest, names[j]);
        struct stat st; int64_t k; int deleted = 0;
        if (rom_overlay_path(m, rel, resolved, 1024, &deleted)) { if (!stat(resolved, &st)) { o->type = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; o->size = S_ISDIR(st.st_mode) ? 0 : st.st_size; } }
        else if (deleted) continue;
        else if (m->img && (k = img_find(m->img, rel)) >= 0) {
            o->type = m->img->e[k].type ? ENT_FILE : ENT_DIR;
            o->size = m->img->e[k].type ? (int64_t)m->img->e[k].b : 0;
        }
        strlcpy(o->name, names[j], sizeof o->name); h->nsnap++;
    }
done:
    free(resolved); free(rel); free(guest); free(parent); free(all); free(names); free(roots);
    errno = err; return err ? -1 : 0;
}
HLE_FORCE("_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", "fsp-srv → opendir(3)", Result, h_fs_OpenDirectory, uint64_t *out, const char *path, int mode) { dm_thread_service_request();
    Result (*sdk_open)(uint64_t *, const char *, int) = sdk_path_call(path, "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci");
    if (sdk_open) return sdk_opened(sdk_open(out, path, mode), out, "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
    int mi; int64_t k;
    if (img_path(path, &mi, &k)) {   // image children are one run of entries
        if (k >= 0 && g_mnt[mi].img->e[k].type) errno = ENOTDIR, k = -1;
        if (k < 0) return err_res("opendir", path);
        const romfs_img *m = g_mnt[mi].img; const img_ent *x = &m->e[k];
        dirh_t *h = calloc(1, sizeof *h); if (!h || !(h->snap = calloc(x->b ? x->b : 1, sizeof *h->snap))) die("RomFS image directory allocation failed");
        h->nsnap = (int64_t)x->b; h->mode = mode; strlcpy(h->path, path, sizeof h->path);
        for (uint64_t j = 0; j < x->b; j++) {
            const img_ent *c = &m->e[x->a + j]; DirectoryEntry *o = &h->snap[j];
            memcpy(o->name, m->names + c->name, c->nlen < sizeof o->name ? c->nlen : sizeof o->name - 1);
            o->type = c->type ? ENT_FILE : ENT_DIR; o->size = c->type ? (int64_t)c->b : 0;
        }
        dir_track(h); *out = (uint64_t)h; return RES_OK;
    }
    HP(path); DIR *d = opendir(hp);
    if (!d) return err_res("opendir", hp);
    dirh_t *h = calloc(1, sizeof *h);
    if (!h) { int e = errno; closedir(d); errno = e; return err_res("directory allocation", hp); }
    h->d = d; strlcpy(h->path, hp, sizeof h->path); h->mode = mode;
    const char *c = strchr(path, ':');
    if (c) for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].overlay[0] && mnt_is(&g_mnt[i], path, (size_t)(c - path))) {
        if (dir_snapshot(h, &g_mnt[i], c + 1)) {
            int err = errno;
            closedir(d);
            free(h);
            errno = err;
            return err_res("opendir", hp);
        }
        break;
    }
    dir_track(h); *out = (uint64_t)h; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", "fsp-srv → readdir(3)", Result, h_fs_ReadDirectory, int64_t *count, DirectoryEntry *ents, uint64_t hh, int64_t max) { dm_thread_service_request();
    SDK_HANDLE(hh, "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", Result (*)(int64_t *, DirectoryEntry *, uint64_t, int64_t), RES_FAIL, count, ents, hh, max);
    if (!hh) { *count = 0; return RES_FAIL; }
    PATH_LOCK(); dirh_t *h = (dirh_t *)hh; int64_t n = 0; struct dirent *e;
    if (h->snap) { while (n < max && h->psnap < h->nsnap) { DirectoryEntry *src = &h->snap[h->psnap++]; if (!(h->mode & (src->type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE))) continue; ents[n++] = *src; } *count = n; return RES_OK; }
    int dfd = dirfd(h->d);
    while (n < max) {
        if (!(e = dir_next(h->d))) { if (errno) { *count = n; return err_res("readdir", h->path); } break; }
        // directories from d_type without stat; files need fstatat for their size
        struct stat st; int isdir;
        if (e->d_type == DT_DIR) isdir = 1;
        else { if (fstatat(dfd, e->d_name, &st, 0)) continue; isdir = S_ISDIR(st.st_mode); }
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        DirectoryEntry *o = &ents[n++]; memset(o, 0, sizeof *o);
        if (!guest_name(h->path, e->d_name, o->name, sizeof o->name)) {
            char c[1280];
            snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
            *count = n - 1;
            return err_res("directory name", c);
        }
        o->type = isdir ? ENT_DIR : ENT_FILE; o->size = isdir ? 0 : st.st_size;
    }
    *count = n; return RES_OK;
}
HLE_FORCE("_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", "fsp-srv → closedir(3)", void, h_fs_CloseDirectory, uint64_t hh) { dm_thread_service_request();
    if (sdk_handle_known(hh)) {   // forgotten first, as in CloseFile
        sdk_handle_forget(hh);
        void (*sdk_close)(uint64_t) = sdk_handle_sym(hh, "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE");
        if (sdk_close) sdk_close(hh);
        return;
    }
    if (!hh) return;   // the game closes handle 0 after a failed open too
    dirh_t *h = (dirh_t *)hh; dir_untrack(h); if (h->d) closedir(h->d); free(h->snap); free(h);
}
// Reset: all open files and directories close and every mount but the RomFS image goes; uncommitted writes are lost, as on Horizon when an application ends.
void dm_fs_reset(void) {
    pthread_once(&g_txn_lock_once, txn_locks_init);
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_MOUNT; i++) pthread_rwlock_wrlock(&g_txn_lock[i]);   // no write or commit can start meanwhile
    int nf = 0, nd = 0, nm = 0, k = 0;
    for (int fd = 0; fd < 65536; fd++) if (g_fmode[fd] || atomic_load(&g_ftxn[fd])) {
        close(fd);
        g_fmode[fd] = 0;
        atomic_store(&g_ftxn[fd], 0);
        nf++;
    }
    pthread_mutex_lock(&g_dirs_mu);
    for (dirh_t *h = g_dirs, *n; h; h = n) {
        n = h->next;
        if (h->d) closedir(h->d);
        free(h->snap);
        free(h);
        nd++;
    }
    g_dirs = NULL;
    pthread_mutex_unlock(&g_dirs_mu);
    for (int i = 0; i < g_nmnt; i++) {
        if (g_mnt[i].img) { if (k != i) g_mnt[k] = g_mnt[i]; k++; }
        else nm++;
    }
    g_nmnt = k;
    // they were in the game's memory
    pthread_mutex_lock(&g_sdk_h_lock);
    memset(g_sdk_h, 0, sizeof g_sdk_h);
    atomic_store(&g_sdk_nh, 0);
    pthread_mutex_unlock(&g_sdk_h_lock);
    pthread_mutex_lock(&g_fslog_lock); for (int i = 0; i < 4096; i++) { free(g_fdpath[i]); g_fdpath[i] = NULL; } pthread_mutex_unlock(&g_fslog_lock);
    for (int i = MAX_MOUNT - 1; i >= 0; i--) pthread_rwlock_unlock(&g_txn_lock[i]);
    pthread_mutex_unlock(&g_lock);
    LOG("reset: file system: %d files and %d directories closed, %d mounts dropped (saves and temporary storage mount again; what was not committed is lost)", nf, nd, nm);
}

HLE_FORCE("_ZN2nn2fs8MountRomEPKc", "fsp-srv; rom → host RomFS directory (overload without the cache arguments)", Result, h_fs_MountRom1, const char *name) { return h_fs_MountRom(name, NULL, 0); }
// a short read is a failure
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", "fsp-srv → pread(2), must read in full", Result, h_fs_ReadFileExact, uint64_t h, int64_t off, void *buf, size_t sz) {
    size_t got = 0; Result r = h_fs_ReadFile(&got, h, off, buf, sz);
    if (r != RES_OK) return r;
    if (got != sz) { LOG("fs ReadFile short read: fd %d offset %lld wanted %zu got %zu", (int)FH_IN(h), (long long)off, sz, got); return RES_FAIL; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", "fsp-srv → pread(2) (overload with read options, which the host ignores)", Result, h_fs_ReadFileOpt,
          size_t *out, uint64_t h, int64_t off, void *buf, size_t sz, const void *opt) { (void)opt; return h_fs_ReadFile(out, h, off, buf, sz); }
// Unhooked, the SDK reads the host's handle as a FileAccessor*.
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE", "fsp-srv → pread(2), must read in full (overload with read options, which the host ignores)", Result, h_fs_ReadFileExactOpt,
          uint64_t h, int64_t off, void *buf, size_t sz, const void *opt) { (void)opt; return h_fs_ReadFileExact(h, off, buf, sz); }
HLE_FORCE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", "fsp-srv; directory entry count (filtered by the mode given at open, read position untouched)", Result, h_fs_DirCount, int64_t *out, uint64_t hh) { dm_thread_service_request();
    SDK_HANDLE(hh, "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", Result (*)(int64_t *, uint64_t), RES_FAIL, out, hh);
    PATH_LOCK(); dirh_t *h = (dirh_t *)hh; int64_t n = 0;
    if (h->snap) {
        for (int64_t i = 0; i < h->nsnap; i++) if (h->mode & (h->snap[i].type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE)) n++;
        *out = n;
        return RES_OK;
    }
    DIR *d = opendir(h->path); if (!d) return err_res("opendir", h->path);
    struct dirent *e;
    while ((e = dir_next(d))) {
        char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name);
        struct stat st; if (stat(c, &st)) continue;
        if (h->mode & (S_ISDIR(st.st_mode) ? DIRMODE_DIR : DIRMODE_FILE)) n++;
    }
    int err = errno; closedir(d);
    if (err) { errno = err; return err_res("readdir", h->path); }
    *out = n; return RES_OK;
}
// No access log; priority does not affect pread.
HLE_FORCE("_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", "fsp-srv; file read priority, ignored by the host", void, h_fs_SetPrio, int prio) { (void)prio; }
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEj", "fsp-srv; access log always off (returns bool)", unsigned, h_fs_AccessLogMask, unsigned mask) { (void)mask; return 0; }
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEv", "fsp-srv; access log always off (returns bool)", unsigned, h_fs_AccessLog, void) { return 0; }
