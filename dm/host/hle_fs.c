// nn::fs: on the real device it goes through the fsp-srv service; the host connects directly to the Mac file system. Mount name → host directory:
//   rom   → $DM_ROMFS (default $DM_ROOT/romfs, read-only; the game parses the storage blocks in it itself)
//   save  → $DM_SAVE (must be set, pointing to this run's copy of the standard save save_ref; if unset, report a failure and abort)
//   temp  → $DM_TEMP (default $DM_ROOT/run/platform/temp)
// Paths look like "rom:/data/x". FileHandle / DirectoryHandle are 8 bytes passed by value, here they store the host fd (FH_OUT) / DIR*.
// Struct layout follows the public Horizon ABI: DirectoryEntry = {char name[0x301], pad[3], s8 type, pad[3], s64 size} (0x310 bytes).
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

#define RES_PATH_NOT_FOUND 0x202u      // fs module 2, description 1
#define RES_PATH_EXISTS    0x402u      // fs module 2, description 2
#define RES_RENAME_OTHER_FS 0x2F2402u  // fs module 2, description 6034: ResultRenameToOtherFileSystem
enum { MODE_READ = 1, MODE_WRITE = 2, MODE_APPEND = 4 };
enum { ENT_DIR = 0, ENT_FILE = 1 };
enum { DIRMODE_DIR = 1, DIRMODE_FILE = 2 };

typedef struct { char name[0x301]; uint8_t pad0[3]; int8_t type; uint8_t pad1[3]; int64_t size; } DirectoryEntry;
_Static_assert(sizeof(DirectoryEntry) == 0x310, "DirectoryEntry layout");

#define MAX_MOUNT 512   // the table is permanent (Unmount frees no entry), so it is sized far above the names a game mounts
// Save transactions (HLE audit items 1–3): on the real device a save can be rolled back as a whole before commit, and after a crash / kill a remount sees only the version from the last Commit.
// Host: at mount, the committed directory commit (= $DM_SAVE) is clonefile'd as a whole tree into the working directory <commit>.work (APFS copy-on-write, nearly zero cost), and the mount points at the working directory,
// all game reads and writes happen in the working directory; Commit = clonefile(work → <commit>.new), then renamex_np(RENAME_SWAP) swaps it atomically with commit, then the swapped-out old version is deleted.
// Killed at any moment, commit is either the old version or the new version, never half-written / a mix of old and new; the next mount discards leftover .work / .new.
// RomFS image (dm/tools/romfs_image.py): DM_ROMFS may name one image file instead of a folder. Its index is read at mount; a file handle is then an entry of the image
// (IMG_HANDLE, bit 58 set, never a valid fd), read with pread at its offset in the one descriptor kept open, and a directory lists its consecutive child entries.
// One file instead of a few hundred thousand: the image was made for copies to an iPhone at full speed (a folder copy of Tears of the Kingdom's 304,000 files ran the phone's file service out of open files)
#define IMG_HANDLE(mi, k) ((1ull << 58) | ((uint64_t)(mi) << 32) | (uint64_t)(k))
#define IMG_H(h) (((h) >> 58) == 1)
#define IMG_M(h) ((int)(((h) >> 32) & 0x3ffffff))
#define IMG_K(h) ((uint32_t)(h))
// On Horizon a FileHandle is a pointer to the SDK's FileAccessor, a heap object, so its low bits are clear, and a game may use them: Metroid Prime 4's file device masks
// them off (handle & ~7) before ReadFile, which read image entry 7 as entry 0 (another file, then a null pointer at boot). The guest sees the host's handle shifted
// left by 4, 16-byte aligned as AArch64 heap blocks are (the image flag then lands on bit 62, as before): FH_OUT where a handle goes to the guest, FH_IN in each call that takes one
#define FH_OUT(h) ((uint64_t)(h) << 4)
#define FH_IN(h) ((uint64_t)(h) >> 4)
typedef struct {
    char name[32]; char dir[1024]; char overlay[4096]; char commit[1024];
    romfs_img *img;                  // non-NULL: dir is a RomFS image file
    int ro;
    int romfs;                       // the game's RomFS: reads look in the overlay folders first (DM_ROMFS_OVERLAY, dm_fs_overlay_set)
    int txn_root;                    // canonical mount entry for this backing transaction, or -1
    _Atomic int poison;              // failure state belongs to this backing, not to the process
    _Atomic uint64_t first_write_ns;
} mount_t;
static mount_t g_mnt[MAX_MOUNT];   // commit non-empty = transactional save mount
static int g_nmnt;
// Whether entry m is mounted under the n-byte name s
static int mnt_is(const mount_t *m, const char *s, size_t n) { return strlen(m->name) == n && !strncmp(m->name, s, n); }
static int mnt_named(const mount_t *m, const char *name) { return mnt_is(m, name, strlen(name)); }
static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_path_lock = PTHREAD_MUTEX_INITIALIZER;
static void path_unlock(pthread_mutex_t **m) { pthread_mutex_unlock(*m); }
#define PATH_LOCK() pthread_mutex_t *path_guard __attribute__((cleanup(path_unlock))) = &g_path_lock; pthread_mutex_lock(path_guard)
static uint8_t g_fmode[65536];              // fd → open mode
static _Atomic uint16_t g_ftxn[65536];       // fd → 1-based canonical transaction index, or 0
static _Atomic int g_save_writes;           // number of writes on the save mount (shared by the injection point and the commit log)
static pthread_once_t g_save_test_once = PTHREAD_ONCE_INIT;
static int g_save_kill_n, g_save_fail_n, g_save_fail_e;
static void save_test_init(void) {
    const char *k = getenv("DM_TEST_KILL_AT"), *f = getenv("DM_TEST_FS_FAIL");
    g_save_kill_n = k && !strncmp(k, "write:", 6) ? atoi(k + 6) : 0;
    if (f && !strncmp(f, "write:", 6)) { g_save_fail_n = atoi(f + 6); const char *c = strchr(f + 6, ':'); g_save_fail_e = c ? atoi(c + 1) : ENOSPC; }
}
static pthread_rwlock_t g_txn_lock[MAX_MOUNT];
static pthread_once_t g_txn_lock_once = PTHREAD_ONCE_INIT;
static void txn_locks_init(void) { for (int i = 0; i < MAX_MOUNT; i++) pthread_rwlock_init(&g_txn_lock[i], NULL); }
static void txn_unlock(pthread_rwlock_t **m) { if (*m) pthread_rwlock_unlock(*m); }
// Write failures are refused per transaction: on the real device save space is reserved when the save is created, overwrites cannot fail, and the game does not check the return values of WriteFile / SetFileSize either, it Commits as usual after a failure
// (after injecting a failure on the 1st write, the committed save file was 0 bytes). When a host write fails (disk full etc.):
// if any save write / set-size failed between the last Commit and this Commit, this Commit is refused -- the committed version is unchanged, the working directory is discarded and re-cloned from the committed version,
// and the front end is notified (front_save_failed); the next transaction (the game's next full save rewrite) is attempted as usual and committed if it succeeds. Each backing save records its own first error.
// The same host directory can have several guest mount names, but independent
// save directories must never consume each other's failure state.
static void save_poison_txn(int txn, int e) {
    // Callers use a published, permanent mount slot. Failed fd I/O does not
    // hold the path lock, so it must not read the mutable mount count.
    if (txn < 0 || txn >= MAX_MOUNT) return;
    int z = 0;
    atomic_compare_exchange_strong(&g_mnt[txn].poison, &z, e ? e : EIO);
}
static void save_poison(int e) { // Test helper: poison the first mounted transaction.
    for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].txn_root == i) { save_poison_txn(i, e); return; }
}
// The default weak implementation only logs; a front end that shows the failure provides a strong one of the same name (the unit tests count the calls with theirs). Called on the game thread
__attribute__((weak)) void front_save_failed(int err) { LOG("front_save_failed(errno=%d): no front end attached (default weak implementation)", err); }
// DM_FSLOG=<file>: each ReadFile logs one TSV line "seconds since boot\tguest path\toffset\tlength\tguest buffer address" (which bytes boot read, for offline reverse lookup against the manifest)
static FILE *g_fslog; static char *g_fdpath[4096]; static pthread_mutex_t g_fslog_lock = PTHREAD_MUTEX_INITIALIZER;
static void fslog_init(void) { static int done; if (done) return; done = 1; const char *e = getenv("DM_FSLOG"); if (e && !(g_fslog = fopen(e, "w"))) die("cannot open DM_FSLOG %s", e); if (g_fslog) setvbuf(g_fslog, NULL, _IOLBF, 0); }   // line-buffered: runs end at the time-limit alarm or a crash, which never flush stdio

static const char *envdir(const char *var, const char *rel, char *buf, size_t n) {
    const char *v = getenv(var);
    if (v) return v;
    snprintf(buf, n, "%s%s/%s", getenv("DM_ROOT") ?: getenv("HOME"), getenv("DM_ROOT") ? "" : "/dm-work", rel); return buf;
}

// Steps that end in a failed write: test injection DM_TEST_FS_FAILAT=<point>[:<count>][,...] -- in a save transaction, mountclone (clone the work directory at mount),
// reclone (reclone after a refused commit), guestname (write the guest name after a rename), renameback (rename rollback) fail with ENOSPC for the first <count> times (default 1),
// to check "no exit, committed version unchanged, next commit works" (tests/t_fs.c 6). Without the variable it costs one getenv
static int dm_fs_test_fail(const char *pt) {
    static char spec[256]; static int cnt[8]; const char *e = getenv("DM_TEST_FS_FAILAT"); if (!e || !e[0]) return 0;
    if (strcmp(spec, e)) { strlcpy(spec, e, sizeof spec); memset(cnt, 0, sizeof cnt); }   // a new set of injections starts counting from zero
    size_t L = strlen(pt); int k = 0;
    for (const char *q = spec; *q && k < 8; k++) { const char *end = strchr(q, ','); size_t n = end ? (size_t)(end - q) : strlen(q);
        if (n >= L && !strncmp(q, pt, L) && (n == L || q[L] == ':')) { int lim = n > L ? atoi(q + L + 1) : 1; if (cnt[k] < lim) { cnt[k]++; errno = ENOSPC; LOG("DM_TEST_FS_FAILAT: %s fails with ENOSPC (#%d)", pt, cnt[k]); return 1; } return 0; }
        q = end ? end + 1 : q + n; }
    return 0;
}
static int rm_tree(const char *p);
// Reads and checks a RomFS image's header and index; NULL (and a red line) when it is not a complete image
static romfs_img *img_open(const char *path) {
    romfs_img *m = romfs_image_open(path);
    if (m) LOG("fs RomFS image %s: %llu entries", path, (unsigned long long)m->n);
    else LOG("[RED] fs RomFS image %s is not a complete image (copied in part? errno=%d)", path, errno);
    return m;
}
// The entry a path names in an image: each component is a binary search of its directory's children, which the packer sorted by name (bytes, a shorter prefix first)
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
// Horizon's file system accessor turns each backslash in a path into a slash before it normalizes the path (FileSystemAccessor::SetUpPath, InitializeWithReplaceBackslash),
// on every mount but the PC host's: Metroid Prime 4 (SDK 19) names its RomFS files rom:\StartupUI.pak. Each guest path resolver takes p through here first: p itself, or
// its copy in buf with slashes (a path too long for buf is left as it is, and is not found)
static const char *fwd_slashes(const char *p, char *buf, size_t n) {
    if (!p || !strchr(p, '\\') || strlen(p) >= n) return p;
    size_t i = 0; for (; p[i]; i++) buf[i] = p[i] == '\\' ? '/' : p[i];
    buf[i] = 0; return buf;
}
static int norm_rest(const char *rest, char *out, size_t n);
static int rom_overlay_path(const mount_t *m, const char *rest, char *out, size_t n, int *deleted);
// The overlay folders while the game runs (the Mods menu turns mods on and off): open files stay as they are, later opens and listings follow the new list
static char g_overlay_live[4096]; static int g_overlay_is_live;
static const char *overlay_now(void) { return g_overlay_is_live ? g_overlay_live : getenv("DM_ROMFS_OVERLAY"); }
void dm_fs_overlay_set(const char *list) {
    PATH_LOCK();
    strlcpy(g_overlay_live, list, sizeof g_overlay_live); g_overlay_is_live = 1;
    for (int i = 0; i < g_nmnt; i++) if (g_mnt[i].romfs) strlcpy(g_mnt[i].overlay, list, sizeof g_mnt[i].overlay);
}
// A path on an image mount: returns 1 with the mount in *mi and the entry in *k (-1 with errno set when there is none); 0 for a path on any other mount, or one an overlay folder (DM_ROMFS_OVERLAY) has, which then opens as a host file
static int img_path(const char *p, int *mi, int64_t *k) {
    PATH_LOCK(); char fwd[1024]; p = fwd_slashes(p, fwd, sizeof fwd); const char *c = p ? strchr(p, ':') : NULL; if (!c) return 0;
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
            // Repeating the same mount is harmless, but a different account's
            // save under this name must not silently resolve to the old account.
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
    // The transaction directory cannot be set up (leftovers cannot be cleared / clone failed, which really happens on a full disk): do not exit; mount the committed directory read-only -- saves still read, every write fails, commits are no-ops, the committed version is untouched, and the front end is told
    // (each of these three failures used to call die: with an iPhone's storage full the game exited as soon as it mounted its saves)
    int txn_err = 0, k = 0;
    while (txn && k < g_nmnt && strcmp(g_mnt[k].commit, dir)) k++;
    if (txn && k < g_nmnt) {   // a second name for a save already mounted: it shares that transaction, whose pending writes a new clone would drop
        strlcpy(g_mnt[g_nmnt].commit, g_mnt[k].commit, sizeof g_mnt[0].commit); strlcpy(g_mnt[g_nmnt].dir, g_mnt[k].dir, sizeof g_mnt[0].dir);
        g_mnt[g_nmnt].txn_root = g_mnt[k].txn_root;
    } else if (txn) {
        char work[1100], neu[1100]; snprintf(work, sizeof work, "%s.work", dir); snprintf(neu, sizeof neu, "%s.new", dir);
        mkdir(dir, 0755);
        const char *why = NULL;
        if (!access(work, F_OK) && rm_tree(work)) why = "cannot clear the work directory left over from last time";
        else if (!access(neu, F_OK) && rm_tree(neu)) why = "cannot clear the .new left over from last time";
        else if (dm_fs_test_fail("mountclone") || clonefile(dir, work, 0))   // ENOTSUP: HFS+ and exFAT can neither clone nor swap directories (RENAME_SWAP), so no transaction can work on them
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
    if (ro && !txn) {   // game files: a RomFS folder, or a RomFS image file in its place
        // A path that is neither is a stale or mistyped one (an old run command still naming the RomFS folder a rebuild replaced with RomFS.img): stop here, not at the game's
        // first failed read (Echoes of Wisdom crashed 0.3 s after nnMain, GitHub issue #615)
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

// APFS is case-insensitive by default, Horizon paths are case-sensitive: every component is looked up exactly by guest name.
// Ordinary host names are unchanged; only when a name collides on the host is a separate physical name chosen, with the guest name stored in an xattr.
// clonefile preserves xattr, so save Commit/remount needs no separate mapping table; enumeration must not leak physical aliases.
#define FS_GUEST_NAME "org.d2rmobile.guest-name"   // the name this host has always used: existing saves carry it, so it is kept
static int guest_name(const char *parent, const char *physical, char *out, size_t n) {
    // Only the physical names chosen on a case collision (.dm-case-N) carry the xattr (set_guest_name writes it only when the physical name differs from the guest name); any other physical name is the guest name,
    // so no per-entry getxattr is needed -- that was one system call per entry when listing, seconds for the 216,000 entries Engage walks in RomFS at boot
    if (strncmp(physical, ".dm-case-", 9)) { strlcpy(out, physical, n); return 1; }
    char p[1280]; snprintf(p, sizeof p, "%s/%s", parent, physical);
    ssize_t k = getxattr(p, FS_GUEST_NAME, out, n - 1, 0, XATTR_NOFOLLOW);
    if (k >= 0) { out[k] = 0; return 1; }
    if (errno != ENOATTR) return 0;
    strlcpy(out, physical, n); return 1;
}
// The next entry of a directory, without . and ..: NULL at its end with errno 0, NULL with errno set when reading failed. readdir answers both with NULL and only errno tells them apart:
// a failure read as the end gave a short listing, which a name index kept for the whole run (each later open of a name after it not found) and a clean reported as done
static struct dirent *dir_next(DIR *d) {
    struct dirent *e;
    do { errno = 0; e = readdir(d); } while (e && (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")));
    return e;
}
// Name index of the directories of read-only mounts (RomFS, a save that fell back to read-only): guest name → physical name, sorted by guest name. The mount table is resident and a read-only mount does not change for the whole process,
// so each directory's index is built once at its first lookup and stays valid. The entry-by-entry comparison below calls getxattr on every entry of the parent directory for each path component:
// Engage's voice bank directories have 38,000 entries, and walking the whole RomFS at boot (9,680 directories) took about 17 s of black screen on the Mac and longer on an iPhone;
// in play every audio file Wwise opened also took tens of milliseconds, all while holding the global path lock. Callers all hold PATH_LOCK (like host_path_ex), so the index takes no lock of its own.
typedef struct { const char *guest, *phys; } name_ent;
typedef struct dir_index { struct dir_index *next; uint64_t hash; char *path; size_t n; name_ent *e; char *pool; } dir_index;
#define DIR_INDEX_SLOTS 8192
static dir_index *g_dir_index[DIR_INDEX_SLOTS];
static uint64_t fnv1a(const char *s) { uint64_t h = 1469598103934665603ull; while (*s) { h ^= (uint8_t)*s++; h *= 1099511628211ull; } return h; }
static int name_ent_cmp(const void *a, const void *b) { return strcmp(((const name_ent *)a)->guest, ((const name_ent *)b)->guest); }
// Returns NULL when it cannot be built (the directory cannot be opened or read, reading an entry's xattr fails), and nothing is kept: the caller then takes the original path, and the next lookup builds it again
static const dir_index *dir_index_get(const char *dir) {
    uint64_t h = fnv1a(dir); dir_index **slot = &g_dir_index[h % DIR_INDEX_SLOTS];
    for (dir_index *x = *slot; x; x = x->next) if (x->hash == h && !strcmp(x->path, dir)) return x;
    DIR *d = opendir(dir); if (!d) return NULL;
    // First gather the names into one pool (a physical name is stored separately only when it differs from the guest name), with offsets in off[]; they become pointers once the pool stops growing
    size_t n = 0, cap = 64, plen = 0, pcap = 4096; size_t (*off)[2] = malloc(cap * sizeof *off); char *pool = malloc(pcap);
    if (!off || !pool) die("directory index allocation failed");
    struct dirent *e; char guest[0x301]; int ok = 1;
    while ((e = dir_next(d))) {
        if (!guest_name(dir, e->d_name, guest, sizeof guest)) { ok = 0; break; }
        size_t gl = strlen(guest) + 1, same = !strcmp(guest, e->d_name), pl = same ? 0 : strlen(e->d_name) + 1;
        if (n == cap && !(off = realloc(off, (cap *= 2) * sizeof *off))) die("directory index allocation failed");
        while (plen + gl + pl > pcap) if (!(pool = realloc(pool, pcap *= 2))) die("directory index allocation failed");
        off[n][0] = plen; memcpy(pool + plen, guest, gl); plen += gl;
        off[n][1] = same ? off[n][0] : plen; if (!same) { memcpy(pool + plen, e->d_name, pl); plen += pl; }
        n++;
    }
    if (!e && errno) ok = 0;
    closedir(d);
    if (!ok) { free(off); free(pool); return NULL; }
    dir_index *x = calloc(1, sizeof *x); if (!x || !(x->path = strdup(dir)) || !(x->e = malloc((n ? n : 1) * sizeof *x->e))) die("directory index allocation failed");
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
        if (!lstat(p, &st)) { // the exact guest name does not exist, but APFS matched the physical name to another entry
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
        if (strcmp(part, ".") && !path_component(out, n, part, 0, 1)) return 0;   // overlay roots are all RomFS, read-only
        rest = end;
    }
    return 1;
}
// A mod's folder may spell a name in another case than the game does (made on Windows or macOS, where both spellings name one file): a Switch would
// not match it and the mod would silently do nothing, so an overlay folder's entry is taken when no exactly spelled one is there, and the log says so
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
// One overlay folder's answer for a path: 1 it has it (out), -1 it deletes it, 0 neither. "<name>.dm-delete" beside where <name> would be hides
// the game's file or directory of that name, and those of the folders after this one; the folder's own <name>, if it has one, takes its place
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
// The path after the mount name is normalized lexically as Horizon's PathNormalizer does: "." is dropped, ".." goes up one level, and going above the root is an error (ResultDirectoryUnobtainable on hardware, reported here as not found).
// Metroid Dread's stream sound paths are "rom:/system/snd/../../sounds/streams/...": ".." used to be reported as not found, and Atk's stream load task got an unexpected error code and aborted (main+0x1225a8c)
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
// create only allows the last component to not exist yet; intermediate directories still need an exact match.
static int host_path_ex(const char *p, char *out, size_t n, int *ro, int create) {
    char fwd[1024]; p = fwd_slashes(p, fwd, sizeof fwd); const char *c = strchr(p, ':');
    if (!c) { errno = ENOENT; return 0; }
    size_t ln = c - p;
    for (int i = 0; i < g_nmnt; i++) {
        if (!mnt_is(&g_mnt[i], p, ln)) continue;
        if (ro) *ro = g_mnt[i].ro;
        char normed[1024]; const char *rest = c + 1;
        if (strstr(rest, "/.")) { if (!norm_rest(rest, normed, sizeof normed)) return 0; rest = normed; }   // only paths with "." components are normalized; the rest walk component by component as before
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
    if (errno == ENOENT || errno == ENOTDIR) { if (g_flag.fs) LOG("fs %s not found: %s", op, path); return RES_PATH_NOT_FOUND; }   // Logged under DM_FS=1 (the game often probes with "not found", not logged by default)
    if (errno == EEXIST) return RES_PATH_EXISTS;
    LOG("fs %s %s failed errno=%d", op, path, errno); return RES_FAIL;
}
// Refuse the next commit of the save transaction behind a guest path's mount (aliases share it), found by the mount name, so a failure before any physical path exists counts too.
// Read-only and non-transactional mounts have none. Callers hold the path lock. Returns whether a transaction was marked.
static int save_poison_path(const char *path, int e) {
    const char *c = strchr(path, ':');
    for (int i = 0; c && i < g_nmnt; i++) if (mnt_is(&g_mnt[i], path, (size_t)(c - path))) {
        if (g_mnt[i].ro || g_mnt[i].txn_root < 0) return 0;
        save_poison_txn(g_mnt[i].txn_root, e); return 1;
    }
    return 0;
}
// What the guest asked for cannot be done (missing, existing, wrong type, not empty, a bad name or argument): Horizon answers these too, and the working copy is unchanged.
// Any other error (ENOSPC, EIO, EACCES, EMFILE...) is the host's, where Horizon's save storage never fails: the game does not check, and its next Commit would publish an incomplete save.
static int fs_guest_error(int e) { return e == ENOENT || e == ENOTDIR || e == EEXIST || e == EISDIR || e == ENOTEMPTY || e == ENAMETOOLONG || e == EILSEQ || e == EINVAL; }
// The result of a failed change in a writable mount: a host error also refuses the save's next commit (the committed version stays, the front end is told).
static Result save_path_error(const char *op, const char *path) {
    int e = errno;
    if (!fs_guest_error(e)) save_poison_path(path, e);
    errno = e; return err_res(op, path);
}
// Name resolution shares one lock with create/rename/enumerate, aliases and xattr are published to the guest together.
#define HP_ACCESS(path, create, mutating) PATH_LOCK(); char hp[1024]; int ro = 0; if (!host_path_ex(path, hp, sizeof hp, &ro, create)) return (mutating) ? save_path_error("path", path) : err_res("path", path)
#define HP_EX(path, create) HP_ACCESS(path, create, 0)
#define HP(path) HP_EX(path, 0)
#define RW() if (ro) { LOG("fs write to a read-only mount: %s", hp); return RES_FAIL; }
// Create directory / create file: if the target exists, answer "already exists" as a writable mount would -- that is the only failure the game understands (the original save storage never fails to write); with a generic failure it carries on with a null handle
#define RW_CREATE() if (ro) { if (!access(hp, F_OK)) return RES_PATH_EXISTS; LOG("fs write to a read-only mount: %s", hp); return RES_FAIL; }

// ---------- Mount ----------
HLE_FORCE("_ZN2nn2fs22QueryMountRomCacheSizeEPm", "fsp-srv; rom reads the host directory directly, the cache gets only the minimum size", Result, h_fs_QueryRomCache, size_t *out) { *out = 0x1000; return RES_OK; }
HLE_FORCE("_ZN2nn2fs8MountRomEPKcPvm", "fsp-srv; rom → host RomFS directory", Result, h_fs_MountRom, const char *name, void *cache, size_t sz) { dm_thread_service_request();
    (void)cache; (void)sz; char b[1024];
    Result r = mount(name, envdir("DM_ROMFS", "romfs", b, sizeof b), 1);
    if (r == RES_OK) {
        PATH_LOCK();
        const char *overlay = overlay_now();
        for (int i = 0; i < g_nmnt; i++) if (mnt_named(&g_mnt[i], name)) {
            g_mnt[i].romfs = 1; strlcpy(g_mnt[i].overlay, overlay ? overlay : "", sizeof g_mnt[i].overlay); break;
        }
    }
    return r;
}
// The player's own console files: $DM_SYSTEM_DIR (default $DM_ROOT/run/platform/system). The console's Mii database is read from it (hle_mii.c)
const char *dm_system_dir(char *buf, size_t n) { return envdir("DM_SYSTEM_DIR", "run/platform/system", buf, n); }
static int host_mounted(const char *name) {
    PATH_LOCK();
    for (int i = 0; name && i < g_nmnt; i++) if (mnt_named(&g_mnt[i], name)) return 1;
    return 0;
}
// ---------- Mounts in the SDK's own table ----------
// A mount call the host does not take over (nn::fs::MountRamDisk) runs in the SDK and registers in the SDK's own table, which the host's file calls did not consult: every path on such a
// mount was "path not found". Nintendo Switch Sports Resort (04008E001F098000) mounts its VoiceText speech data as the RAM disk "vtapi" and aborted at boot on GetEntryType("vtapi:/VoiceText")
// (Result 0x202, main+0x414dbc). A path whose mount name the host does not hold but the SDK's IsMounted does goes to the SDK's own call (guest_sym: the module's definition, not the host's
// take-over), and the handles it opens are listed, so the calls that take them go to the SDK too. The host's own handles (an fd or an image entry shifted by FH_OUT, a dirh_t pointer) never
// enter the list, and a path on a host mount never asks the SDK. No host lock is held across an SDK call: the SDK takes its own locks
#define SDK_HANDLES 256
static uint64_t g_sdk_h[SDK_HANDLES];                      // file and directory handles the SDK opened (FileAccessor / DirectoryAccessor pointers); 0 = a free slot
static _Atomic int g_sdk_nh;                               // how many: 0 skips the lookup in every host file call
static pthread_mutex_t g_sdk_h_lock = PTHREAD_MUTEX_INITIALIZER;
static int sdk_handle_known(uint64_t h) {
    if (!h || !atomic_load(&g_sdk_nh)) return 0;
    pthread_mutex_lock(&g_sdk_h_lock);
    int found = 0; for (int i = 0; i < SDK_HANDLES && !found; i++) found = g_sdk_h[i] == h;
    pthread_mutex_unlock(&g_sdk_h_lock);
    return found;
}
static int sdk_handle_add(uint64_t h) {   // 0 when the list is full
    pthread_mutex_lock(&g_sdk_h_lock);
    int i = 0; while (i < SDK_HANDLES && g_sdk_h[i]) i++;
    if (i < SDK_HANDLES) { g_sdk_h[i] = h; atomic_fetch_add(&g_sdk_nh, 1); }
    pthread_mutex_unlock(&g_sdk_h_lock);
    return i < SDK_HANDLES;
}
static void sdk_handle_forget(uint64_t h) {
    pthread_mutex_lock(&g_sdk_h_lock);
    for (int i = 0; i < SDK_HANDLES; i++) if (g_sdk_h[i] == h) { g_sdk_h[i] = 0; atomic_fetch_sub(&g_sdk_nh, 1); break; }
    pthread_mutex_unlock(&g_sdk_h_lock);
}
static int sdk_mounted(const char *name) {
    int (*is_mounted)(const char *) = guest_sym("_ZN2nn2fs9IsMountedEPKc");
    return name && is_mounted && is_mounted(name);
}
static int path_mount_name(const char *path, char *name, size_t n) {   // the mount name, the part before ':' as in FindFileSystem; 0 without one
    const char *c = path ? strchr(path, ':') : NULL;
    if (!c || (size_t)(c - path) >= n) return 0;
    memcpy(name, path, (size_t)(c - path)); name[c - path] = 0;
    return 1;
}
// The SDK's own definition of sym when path is on a mount only the SDK holds, else NULL (the host's call goes on as before)
static void *sdk_path_call(const char *path, const char *sym) {
    char name[0x30];
    if (!path_mount_name(path, name, sizeof name) || host_mounted(name) || !sdk_mounted(name)) return NULL;
    void *f = guest_sym(sym);
    if (!f) LOG("[RED] fs %s is on the SDK's mount %s, but the SDK defines no %s", path, name, sym);
    return f;
}
// The SDK's own definition of sym for a handle the SDK opened (sdk_handle_known); NULL is reported, and the caller fails the call: the host's own code would take the SDK's pointer for its own handle
static void *sdk_handle_sym(uint64_t h, const char *sym) {
    void *f = guest_sym(sym);
    if (!f) LOG("[RED] fs handle %#llx is the SDK's, but the SDK defines no %s", (unsigned long long)h, sym);
    return f;
}
// After the SDK opened a file or a directory: list its handle, or close it in the SDK when the list is full
static Result sdk_opened(Result r, uint64_t *out, const char *close_sym) {
    if (r != RES_OK || sdk_handle_add(*out)) return r;
    LOG("[RED] fs: more than %d handles open on the SDK's own mounts, the open fails", SDK_HANDLES);
    void (*close_h)(uint64_t) = guest_sym(close_sym);
    if (close_h) close_h(*out);
    *out = 0; return RES_FAIL;
}
#define SDK_PATH(path, sym, type, ...) do { __typeof__(type) sdk_f = (__typeof__(type))sdk_path_call(path, sym); if (sdk_f) return sdk_f(__VA_ARGS__); } while (0)
#define SDK_HANDLE(h, sym, type, fail, ...) do { if (sdk_handle_known(h)) { __typeof__(type) sdk_f = (__typeof__(type))sdk_handle_sym(h, sym); return sdk_f ? sdk_f(__VA_ARGS__) : (fail); } } while (0)
// IsPathMounted / GetPathLengthMax: the SDK's C library (nnmusl) checks every path with them before it calls nn::fs (sdk+0x8ef50, under open / stat / opendir), and the original SDK answers from
// its own empty mount table. Ace Attorney Investigations Collection (Unity 2022 IL2CPP): File.Exists → SystemNative_Stat → stat → open failed with no nn::fs call at all, so Addressables
// never found StreamingAssets/aa/settings.json and the game crashed on a null asset (main+0x31100ec; found by armyja, GitHub issue #18). The mount name is the part before ':', as in FindFileSystem
HLE_FORCE("_ZN2nn2fs13IsPathMountedEPKc", "fsp-srv; looks up the host mount table, then the SDK's own (a RAM disk)", int, h_fs_IsPathMounted, const char *path) {   // bool in w0: 1 / 0
    const char *c = path ? strchr(path, ':') : NULL;
    {   PATH_LOCK();
        for (int i = 0; c && i < g_nmnt; i++) if (mnt_is(&g_mnt[i], path, (size_t)(c - path))) return 1;
    }
    char name[0x30];   // the SDK is asked with no host lock held
    if (path_mount_name(path, name, sizeof name) && sdk_mounted(name)) return 1;
    if (g_flag.fs) LOG("fs path not mounted: %s", path ? path : "(null)");
    return 0;
}
// The length limits nnmusl checks next (sdk+0x8f17c..0x8f254): Horizon's 0x300 bytes for paths and entry names, and no separate UTF-16 limit (false skips that check)
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
// The SD card: only the practice tools mount it (sd:/sloperinaGZ/: their settings, log, translations; hle_practice.c), the game never does. A host directory, DM_SDCARD or
// <DM_ROOT>/sdcard, which the mount creates. Their release asks MountSdCard first and MountSdCardForDebug when that is refused; both are the same folder here
HLE_ADDON("_ZN2nn2fs11MountSdCardEPKc", "fsp-srv; the SD card → host directory (practice tools)", Result, h_fs_MountSd, const char *name) {
    char b[1024]; return mount(name, envdir("DM_SDCARD", "sdcard", b, sizeof b), 0);
}
HLE_ADDON("_ZN2nn2fs19MountSdCardForDebugEPKc", "fsp-srv; the SD card → host directory (practice tools)", Result, h_fs_MountSdDebug, const char *name) {
    char b[1024]; return mount(name, envdir("DM_SDCARD", "sdcard", b, sizeof b), 0);
}
HLE_FORCE("_ZN2nn2fs16GetFreeSpaceSizeEPlPKc", "fsp-srv; free space of a mount: 256 MB (the host directory has no size limit of its own)", Result, h_fs_FreeSpace, int64_t *out, const char *path) {
    (void)path; if (out) *out = 256 << 20; return RES_OK;
}
// Each registered user has a save directory of its own, with its own transaction: user 1's is DM_SAVE, user N's is <DM_SAVE>_userN
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
    // No default directory: silently landing on a directory with no characters distorts the "character select" measurement (this once caused a misjudgment). Runs always use a fresh copy of save_ref
    const char *d = getenv("DM_SAVE");
    if (!d) die("DM_SAVE is not set: point it at a writable copy of a save directory");
    char dd[1024]; if (user_save_dir(dd, sizeof dd, d, user)) return RES_FAIL;
    return mount_ex(name, dd, 0, 1);
}
static _Atomic uint64_t g_save_commits;   // number of successful commits
uint64_t dm_fs_commit_count(void) { return atomic_load(&g_save_commits); }   // the unit tests tell a published commit from a refused one by it
// Commit's transaction engine. Caller holds the path
// and mount locks; front-end failure notification is delivered after unlocking.
static Result commit_mount_locked(int i, int *notify_error) {
    int txn = g_mnt[i].txn_root;
    if (txn < 0 || txn >= g_nmnt) return RES_FAIL;
    pthread_once(&g_txn_lock_once, txn_locks_init);
    pthread_rwlock_wrlock(&g_txn_lock[txn]);
    pthread_rwlock_t *txn_guard __attribute__((cleanup(txn_unlock))) = &g_txn_lock[txn];
    uint64_t tc1 = clock_gettime_nsec_np(CLOCK_MONOTONIC), tw0 = atomic_exchange(&g_mnt[txn].first_write_ns, 0);
    int perr = atomic_exchange(&g_mnt[txn].poison, 0);
    // Probe: files of this transaction still open at the commit. Horizon's fs client is believed to refuse a commit while a file is open for writing (WriteModeFileNotClosed;
    // not checked against the SDK); the host commits anyway. A refused commit below reclones the work directory, and a handle opened before it then points into the
    // discarded tree: what is written through it is lost at the next commit. Named so a game that does this shows itself before the host follows Horizon's rule (open)
    int nopen = 0, nwrite = 0; uint16_t token = (uint16_t)(txn + 1);
    for (int k = 0; k < 65536; k++) if (atomic_load(&g_ftxn[k]) == token) { nopen++; nwrite += (g_fmode[k] & (MODE_WRITE | MODE_APPEND)) != 0; }
    if (nwrite) { static int red; if (red++ < 8) LOG("[RED] save commit with %d file(s) open for writing in this transaction (%d open in all); Horizon refuses such a commit, the host goes on", nwrite, nopen); }
    if (perr) {                                                         // a write failed in this transaction: refuse the commit, discard the working directory
        char torn[512] = ""; size_t tl = 0; DIR *d0 = opendir(g_mnt[i].dir); struct dirent *e0;   // record files truncated to 0 bytes (the damage that would enter the save if committed as usual)
        while (d0 && (e0 = readdir(d0))) {
            if (e0->d_name[0] == '.') continue;
            char sub[1300]; snprintf(sub, sizeof sub, "%s/%s", g_mnt[i].dir, e0->d_name); DIR *d1 = opendir(sub); struct dirent *e1;
            while (d1 && (e1 = readdir(d1))) { char f[1600]; struct stat st; snprintf(f, sizeof f, "%s/%s", sub, e1->d_name);
                if (e1->d_name[0] != '.' && !stat(f, &st) && S_ISREG(st.st_mode) && !st.st_size && tl + strlen(e0->d_name) + strlen(e1->d_name) + 3 < sizeof torn)
                    tl += snprintf(torn + tl, sizeof torn - tl, " %s/%s", e0->d_name, e1->d_name); }
            if (d1) closedir(d1);
        }
        if (d0) closedir(d0);
        // First clone the committed version into a sibling temporary directory; once that succeeds, atomically swap it with the tainted work directory and delete the swapped-out one. If any step fails, do not exit: the work directory stays and the "tainted" mark is put back
        // (the next commit is refused again and recloning is retried); the committed version is unchanged (before, the work directory was deleted first and then recloned, and a failed clone called die, so with a full disk the process exited and the work directory was gone)
        char re[1100]; snprintf(re, sizeof re, "%s.re", g_mnt[i].dir); int rerr = 0;
        if (!access(re, F_OK) && rm_tree(re)) rerr = errno ? errno : EIO;
        else if (dm_fs_test_fail("reclone") || clonefile(g_mnt[i].commit, re, 0)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else if (renamex_np(re, g_mnt[i].dir, RENAME_SWAP)) { rerr = errno ? errno : EIO; rm_tree(re); }
        else rm_tree(re);                                               // the swapped-out tainted work directory
        if (rerr) { save_poison_txn(txn, perr); LOG("[RED] save transaction: recloning the work directory failed errno=%d -- the work directory is left as it is and stays tainted (the next commit is refused again and retries), the committed version is unchanged", rerr); }
        LOG("[RED] save commit refused: a save write failed in this transaction errno=%d (0-byte files in the work directory: %s); the committed version is unchanged, the work directory %s%s", perr, tl ? torn + 1 : "none", rerr ? "could not be recloned (see the previous line)" : "was recloned from the committed version",
            nopen && !rerr ? "; [RED] save files are still open: they point into the discarded work directory, and what is written through them is lost" : nopen ? "; [RED] save files are still open" : "");
        *notify_error = perr;
        return RES_FAIL;
    }
    char neu[1100]; snprintf(neu, sizeof neu, "%s.new", g_mnt[i].commit);
    Result r = RES_OK;
    if (!access(neu, F_OK) && rm_tree(neu)) { LOG("[RED] save commit: cannot clear the %s left over errno=%d (committed version unchanged)", neu, errno); r = RES_FAIL; }
    else if (clonefile(g_mnt[i].dir, neu, 0)) { LOG("[RED] save commit: clonefile %s failed errno=%d (committed version unchanged)", neu, errno); r = RES_FAIL; }
    else if (renamex_np(neu, g_mnt[i].commit, RENAME_SWAP)) { LOG("[RED] save commit: atomic swap failed errno=%d (committed version unchanged)", errno); rm_tree(neu); r = RES_FAIL; }
    else rm_tree(neu);                                                  // the old version that was swapped out
    // Timing readouts (integer microseconds, no floating point): how long the clone + atomic swap took; how long in total from this transaction's first save write to the end of the commit (the game writing files + the commit; 0 = no save write in this transaction)
    static int n; if (r == RES_OK) { uint64_t tc2 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
        LOG("save commit #%d: %s swapped atomically into %s (%d save writes so far) (clone + swap %llu us, since this transaction's first write %llu us)", ++n, g_mnt[i].dir, g_mnt[i].commit, atomic_load(&g_save_writes),
            (unsigned long long)((tc2 - tc1) / 1000), (unsigned long long)(tw0 ? (tc2 - tw0) / 1000 : 0)); atomic_fetch_add(&g_save_commits, 1);
    }
    return r;
}
HLE_FORCE("_ZN2nn2fs6CommitEPKc", "fsp-srv; transactional save: the whole work directory tree is cloned, then swapped atomically with the committed directory (renamex_np RENAME_SWAP)", Result, h_fs_Commit, const char *name) { dm_thread_service_request();
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    int i = 0; while (i < g_nmnt && !mnt_named(&g_mnt[i], name)) i++;
    if (i == g_nmnt || !g_mnt[i].commit[0]) { pthread_mutex_unlock(&g_lock); return RES_OK; }   // non-transactional mount: nothing to commit
    int notify_error = 0; Result r = commit_mount_locked(i, &notify_error);
    pthread_mutex_unlock(&g_lock);
    if (notify_error) front_save_failed(notify_error);
    return r;
}
// The host's mount table is permanent, so unmounting one of its names changes nothing. A mount call the host does not take over runs in the SDK and registers in the SDK's own table
// (nn::fs::MountRamDisk), and only the SDK can release it. Paper Mario: The Thousand-Year Door unmounts its "cache" RAM disk, frees the buffer and mounts it again, which with the entry left
// behind failed with MountNameAlreadyExists and aborted (main+0x303b74, GitHub issue #1012). The SDK's Unmount aborts on a name it does not hold, hence its IsMounted first; a name in
// neither table stays a no-op. No host lock is held across the SDK's calls, which take the SDK's own locks and free guest memory
HLE_FORCE("_ZN2nn2fs7UnmountEPKc", "fsp-srv; host mounts are permanent, a no-op; a mount in the SDK's own table (RAM disk) is released there", void, h_fs_Unmount, const char *name) {
    if (!name) return;
    if (host_mounted(name)) return;
    int (*sdk_is_mounted)(const char *) = guest_sym("_ZN2nn2fs9IsMountedEPKc");
    void (*sdk_unmount)(const char *) = guest_sym("_ZN2nn2fs7UnmountEPKc");
    if (!sdk_is_mounted || !sdk_unmount || !sdk_is_mounted(name)) return;
    LOG("fs unmount %s: in the SDK's own mount table, released there", name);
    sdk_unmount(name);
}

// ---------- Files ----------
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
    if (g_fslog && fd < 4096) { pthread_mutex_lock(&g_fslog_lock); free(g_fdpath[fd]); g_fdpath[fd] = strdup(path); pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) LOG("fs open [%.2fs] %s mode=%d → fd %d", (now_ns() - g_t0_ns) / 1e9, path, mode, fd);
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", "fsp-srv → pread(2)", Result, h_fs_ReadFile, size_t *out, uint64_t h, int64_t off, void *buf, size_t sz) { dm_thread_service_request();
    SDK_HANDLE(h, "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", Result (*)(size_t *, uint64_t, int64_t, void *, size_t), RES_FAIL, out, h, off, buf, sz);
    h = FH_IN(h);
    if (!h) { *out = 0; return RES_FAIL; }   // a file handle is an fd; 0 is stdin and never a file the game opened: the null handle the game keeps using after a failed open
    size_t got = 0; int reprep = 0, fd = (int)h; int64_t base = 0;
    if (IMG_H(h)) {   // an image entry: read the image's descriptor at the entry's offset, never past its end
        const img_ent *x = &g_mnt[IMG_M(h)].img->e[IMG_K(h)];
        fd = g_mnt[IMG_M(h)].img->fd; base = (int64_t)x->a;
        sz = off < 0 || (uint64_t)off >= x->b ? 0 : (size_t)(x->b - off) < sz ? (size_t)(x->b - off) : sz;
    }
    dm_guest_write_prepare(buf, sz);   // The destination may be a texture pool page write-protected by the graphics layer: a kernel write to a read-only page returns EFAULT instead of page-faulting, so unprotect and mark dirty first
    while (got < sz) {
        ssize_t r = pread(fd, (uint8_t *)buf + got, sz - got, base + off + got);
        if (r < 0) { if (errno == EINTR) continue;
            // The graphics layer write-protects tracked pages again after each upload (nvn_dirty.c), so a page made writable above can be read-only again by the time pread
            // writes it: a plain write would fault into the tracker's handler, a system call gets EFAULT. Prepare the rest again (unprotect, mark dirty) and read on. Monster Train 2
            // at 60 fps lost one read this way in two of three runs on the Mac (0x2c000 bytes of pool pages, protection read-only), never at 30 fps, where uploads come half as often
            if (errno == EFAULT && reprep++ < 8) {
                static _Atomic int once; if (!once++) LOG("[fs] ReadFile into %p+%#zx: the destination was write-protected again during the read, prepared again at +%#zx (first case)", buf, sz, got);
                dm_guest_write_prepare((uint8_t *)buf + got, sz - got); continue; }
            if (errno == EFAULT) {   // the destination is not writable host memory: say where, with the protection of its VM region
                int e = errno; vm_address_t ra = (vm_address_t)((uint8_t *)buf + got); vm_size_t rs = 0; vm_region_basic_info_data_64_t vi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
                kern_return_t kr = vm_region_64(mach_task_self(), &ra, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj);
                LOG("[RED] fs ReadFile into %p+%#zx failed at +%#zx (EFAULT): VM region %#lx+%#lx protection %d%s", buf, sz, got, (unsigned long)ra, (unsigned long)rs, kr == KERN_SUCCESS ? vi.protection : -1, kr == KERN_SUCCESS ? "" : " (none)");
                errno = e; }
            return err_res("pread", "fd"); }
        if (r == 0) break;
        got += r;
    }
    *out = got;
    if (g_fslog) { pthread_mutex_lock(&g_fslog_lock);
        static int hash_on = -1; if (hash_on < 0) hash_on = getenv("DM_FSLOG_HASH") != NULL;   // DM_FSLOG_HASH=1: append a column with the FNV-1a 64 of the content read (to compare whether the bytes read from two resource packs match)
        uint64_t fh = 0xcbf29ce484222325ull; if (hash_on) for (size_t i = 0; i < got; i++) fh = (fh ^ ((uint8_t *)buf)[i]) * 0x100000001b3ull;
        if (hash_on) fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\t%016llx\n", (now_ns() - g_t0_ns) / 1e9, !IMG_H(h) && (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf, (unsigned long long)fh);
        else fprintf(g_fslog, "%.4f\t%s\t%lld\t%zu\t%p\n", (now_ns() - g_t0_ns) / 1e9, !IMG_H(h) && (int)h < 4096 && g_fdpath[(int)h] ? g_fdpath[(int)h] : "?", (long long)off, got, buf);
        pthread_mutex_unlock(&g_fslog_lock); }
    if (g_flag.fs) {   // Read metering: print a line every 64 MiB accumulated or every 1000 reads
        static uint64_t n, bytes, next_b = 64 << 20; static pthread_mutex_t m = PTHREAD_MUTEX_INITIALIZER;
        pthread_mutex_lock(&m); n++; bytes += got;
        if (bytes >= next_b || n % 1000 == 0) { LOG("fs read total: %llu reads %.1f MiB (this one: fd %d offset %#llx length %#zx)", n, bytes / 1048576.0, (int)h, off, got); if (bytes >= next_b) next_b += 64 << 20; }
        pthread_mutex_unlock(&m);
    }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", "fsp-srv → pwrite(2)", Result, h_fs_WriteFile, uint64_t h, int64_t off, const void *buf, size_t sz, const void *opt) { dm_thread_service_request();
    SDK_HANDLE(h, "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", Result (*)(uint64_t, int64_t, const void *, size_t, const void *), RES_FAIL, h, off, buf, sz, opt);
    h = FH_IN(h);
    if (!h || IMG_H(h)) return RES_FAIL;
    int fd = (int)h & 0xffff; uint16_t token = atomic_load(&g_ftxn[fd]); int txn = token ? token - 1 : -1;
    pthread_rwlock_t *txn_guard __attribute__((cleanup(txn_unlock))) = NULL;
    if (txn >= 0) { txn_guard = &g_txn_lock[txn]; pthread_rwlock_rdlock(txn_guard); }
    (void)opt; size_t put = 0;
    if (txn >= 0) {                                                     // Save write injection point (for deterministic regression; only writes on the save mount are counted)
        pthread_once(&g_save_test_once, save_test_init);
        int n = atomic_fetch_add(&g_save_writes, 1) + 1;
        { uint64_t z = 0; atomic_compare_exchange_strong(&g_mnt[txn].first_write_ns, &z, clock_gettime_nsec_np(CLOCK_MONOTONIC)); }
        if (g_save_kill_n && n == g_save_kill_n) { LOG("DM_TEST_KILL_AT: save write #%d, calling _exit (simulates being killed mid-write)", n); _exit(9); }
        if (g_save_fail_n && n == g_save_fail_n) { LOG("DM_TEST_FS_FAIL: save write #%d returns failure errno=%d", n, g_save_fail_e); errno = g_save_fail_e; save_poison_txn(txn, g_save_fail_e); return err_res("pwrite (injected)", "fd"); }
    }
    while (put < sz) {
        ssize_t r = pwrite((int)h, (const uint8_t *)buf + put, sz - put, off + put);
        if (r < 0) { if (errno == EINTR) continue; if (txn >= 0) save_poison_txn(txn, errno); return err_res("pwrite", "fd"); }
        if (r == 0) { errno = ENOSPC; if (txn >= 0) save_poison_txn(txn, ENOSPC); return err_res("pwrite (wrote 0 bytes)", "fd"); }   // This used to loop forever here
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
    int fd = (int)h & 0xffff; uint16_t token = atomic_load(&g_ftxn[fd]); int txn = token ? token - 1 : -1;
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
    if (sdk_handle_known(h)) {   // forgotten first: the SDK frees the accessor, and its address may come back from the next open
        sdk_handle_forget(h);
        void (*sdk_close)(uint64_t) = sdk_handle_sym(h, "_ZN2nn2fs9CloseFileENS0_10FileHandleE");
        if (sdk_close) sdk_close(h);
        return;
    }
    h = FH_IN(h);
    if (!h || IMG_H(h)) return;   // the null handle after a failed open (see ReadFile): do not touch fd 0; an image entry holds no descriptor
    g_fmode[(int)h & 0xffff] = 0; atomic_store(&g_ftxn[(int)h & 0xffff], 0);
    close((int)h); }
HLE_FORCE("_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", "fsp-srv; the open mode is recorded by the host", int, h_fs_GetFileOpenMode, uint64_t h) {
    SDK_HANDLE(h, "_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", int (*)(uint64_t), 0, h);
    h = FH_IN(h); return IMG_H(h) ? MODE_READ : g_fmode[(int)h & 0xffff]; }

// ---------- Path operations ----------
HLE_FORCE("_ZN2nn2fs10CreateFileEPKcl", "fsp-srv → open(O_CREAT|O_EXCL)+ftruncate", Result, h_fs_CreateFile, const char *path, int64_t sz) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs10CreateFileEPKcl", Result (*)(const char *, int64_t), path, sz);
    int mi; int64_t k; if (img_path(path, &mi, &k)) return k >= 0 ? RES_PATH_EXISTS : RES_FAIL;   // as RW_CREATE answers on a read-only folder
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
    HP_ACCESS(path, 0, 1); RW(); return unlink(hp) ? save_path_error("unlink", path) : RES_OK; }
HLE_FORCE("_ZN2nn2fs15CreateDirectoryEPKc", "fsp-srv → mkdir(2)", Result, h_fs_CreateDirectory, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs15CreateDirectoryEPKc", Result (*)(const char *), path);
    int mi; int64_t k; if (img_path(path, &mi, &k)) return k >= 0 ? RES_PATH_EXISTS : RES_FAIL;
    HP_ACCESS(path, 1, 1); RW_CREATE();
    if (mkdir(hp, 0755)) return save_path_error("mkdir", path);
    if (set_guest_name(hp, path)) { int e = errno; rmdir(hp); errno = e; return save_path_error("mkdir name", path); }
    return RES_OK; }
// Delete one directory entry: a directory with its tree, anything else (a symlink too, never followed) with unlink. lstat, not d_type: a filesystem may report DT_UNKNOWN
static int rm_entry(const char *c) {
    struct stat st; if (lstat(c, &st)) return -1;
    return S_ISDIR(st.st_mode) ? rm_tree(c) : unlink(c);
}
static int rm_tree(const char *p) {
    struct stat st; if (lstat(p, &st)) return -1;
    if (!S_ISDIR(st.st_mode)) { errno = ENOTDIR; return -1; }   // a symlink to a directory is not entered: opendir would follow it and empty its target
    DIR *d = opendir(p); if (!d) return -1;
    struct dirent *e; char c[1024]; int rc = 0;
    while ((e = dir_next(d))) {
        snprintf(c, sizeof c, "%s/%s", p, e->d_name);
        if ((rc = rm_entry(c))) break;   // keep the first failure, before readdir can clear errno
    }
    int err = errno; closedir(d);
    if (rc || err) { errno = err; return -1; }
    return rmdir(p);
}
HLE_FORCE("_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", "fsp-srv → recursive delete (only inside writable mounts)", Result, h_fs_DeleteDirRec, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", Result (*)(const char *), path);
    HP_ACCESS(path, 0, 1); RW(); return rm_tree(hp) ? save_path_error("rmtree", path) : RES_OK;
}
// SDK 11 (Metroid Dread): delete an empty directory
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
        if (dm_fs_test_fail("renameback") || rename(hp2, hp)) {   // the rollback fails too: the file stays at the new path with wrong guest-name metadata, whatever the first error was; do not exit -- inside a transactional save, its commit is refused (committed version unchanged; recloning restores it). rename_any keeps both paths on one mount
            int re = errno, saved = save_poison_path(from, e);
            LOG("[RED] fs rename: writing the name metadata failed and so did the rollback errno=%d: %s stays at the new path%s", re, hp2, saved ? ", the save's commit will be refused" : "");
        }
        errno = e; return save_path_error("rename name", from); }
    return RES_OK;
}
// Horizon's RenameFile and RenameDirectory find each path's file system by its mount name and refuse two before touching either (the two accessors must be one, ResultRenameToOtherFileSystem):
// so a rename never reaches a read-only mount from a writable one, and two names of one save, one folder on the host, are two mounts here as there. On one mount, the SDK's mount is the SDK's
static Result rename_any(const char *from, const char *to, const char *sym) {
    char a[0x30], b[0x30];
    if (path_mount_name(from, a, sizeof a) && path_mount_name(to, b, sizeof b) && strcmp(a, b)) { LOG("fs rename %s → %s: two mounts, refused", from, to); return RES_RENAME_OTHER_FS; }
    Result (*sdk_rename)(const char *, const char *) = sdk_path_call(from, sym);
    return sdk_rename ? sdk_rename(from, to) : rename_entry(from, to);
}
HLE_FORCE("_ZN2nn2fs10RenameFileEPKcS2_", "fsp-srv → rename(2)", Result, h_fs_RenameFile, const char *from, const char *to) { dm_thread_service_request(); return rename_any(from, to, "_ZN2nn2fs10RenameFileEPKcS2_"); }
// The Great Ace Attorney Chronicles (SDK 10) imports it; a directory carries its guest name the same way as a file
HLE_FORCE("_ZN2nn2fs15RenameDirectoryEPKcS2_", "fsp-srv → rename(2) of a directory", Result, h_fs_RenameDir, const char *from, const char *to) { dm_thread_service_request(); return rename_any(from, to, "_ZN2nn2fs15RenameDirectoryEPKcS2_"); }
HLE_FORCE("_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", "fsp-srv → stat(2)", Result, h_fs_GetEntryType, int32_t *out, const char *path) { dm_thread_service_request();
    SDK_PATH(path, "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", Result (*)(int32_t *, const char *), out, path);
    int mi; int64_t k;
    if (img_path(path, &mi, &k)) { if (k < 0) return err_res("stat", path); *out = g_mnt[mi].img->e[k].type ? ENT_FILE : ENT_DIR; return RES_OK; }
    HP(path); struct stat st;
    if (stat(hp, &st)) return err_res("stat", hp);
    *out = S_ISDIR(st.st_mode) ? ENT_DIR : ENT_FILE; return RES_OK;
}

// ---------- Directories ----------
typedef struct dirh { DIR *d; char path[1024]; DirectoryEntry *snap; int64_t nsnap, psnap; int mode; struct dirh *prev, *next; } dirh_t;
// The directories the game has open, for a reset (dm_fs_reset), which closes them as the end of a process does
static dirh_t *g_dirs; static pthread_mutex_t g_dirs_mu = PTHREAD_MUTEX_INITIALIZER;
static void dir_track(dirh_t *h) { pthread_mutex_lock(&g_dirs_mu); h->prev = NULL; h->next = g_dirs; if (g_dirs) g_dirs->prev = h; g_dirs = h; pthread_mutex_unlock(&g_dirs_mu); }
static void dir_untrack(dirh_t *h) { pthread_mutex_lock(&g_dirs_mu); if (h->prev) h->prev->next = h->next; else if (g_dirs == h) g_dirs = h->next; if (h->next) h->next->prev = h->prev; pthread_mutex_unlock(&g_dirs_mu); }
typedef struct { const char *name; size_t at; } name_at;
static int name_at_cmp(const void *a, const void *b) {
    const name_at *x = a, *y = b; int c = strcmp(x->name, y->name);
    return c ? c : (x->at > y->at) - (x->at < y->at);
}
static int name_at_casecmp(const void *a, const void *b) {
    const name_at *x = a, *y = b; int c = strcasecmp(x->name, y->name);
    return c ? c : (x->at > y->at) - (x->at < y->at);
}
// The first of each name in names[0..n), in the order they came: the overlay folders' in [0, nover), the base folder's in [nover, nfold), then the image's; returns how many are left.
// A folder's names that differ only in case are one name, as the case-insensitive folders they come from hold them, spelled as the game spells it: by the image's first name of that
// case, else the base folder's, else as the first folder has it (the image's own names are case-sensitive, so they are only told apart from each other exactly). One sort: a scan of
// the names so far for each name was quadratic, 722 million comparisons and 3.6 s of OpenDirectory under the path lock for a folder of 38,000 entries (synthetic; Engage's voice bank
// folders are this size), with DM_ROMFS_OVERLAY set
static size_t names_first(char (*names)[0x301], size_t n, size_t nover, size_t nfold) {
    name_at *by = malloc((n ? n : 1) * sizeof *by); uint8_t *again = calloc(n ? n : 1, 1);
    if (!by || !again) die("rom overlay directory snapshot allocation failed");
    for (size_t i = 0; i < n; i++) by[i] = (name_at){names[i], i};
    qsort(by, n, sizeof *by, name_at_casecmp);
    for (size_t g = 0, e; g < n; g = e) {   // one run of names equal but for case, in the order they came: the folders' first
        for (e = g + 1; e < n && !strcasecmp(by[e].name, by[g].name); e++) {}
        size_t f = g; while (f < e && by[f].at < nfold) f++;
        if (f == g || (f - g == 1 && f == e)) continue;   // nothing to respell: no folder's name, or one with no image name of its case
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
// 0, or -1 with errno set when a folder of the overlay cannot be read: nothing is kept
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
    int last = nr - 1, base = 1;   // an overlay folder that deletes this directory hides the folders after it and the game's own
    for (int ri = 1; ri < nr; ri++) if (overlay_lookup(all[ri], rest, parent, 1024) < 0) { last = ri; base = 0; break; }
    size_t nover = 0;   // the overlay folders' names come first: the base folder (ri 0) is read last
    for (int ri = last; ri >= !base; ri--) {
        if (!ri) nover = nn;
        if (ri ? overlay_lookup(all[ri], rest, parent, 1024) <= 0 : !root_path(all[ri], rest, parent, 1024)) continue;
        DIR *x = opendir(parent); if (!x) continue;   // an image base is no folder: its names are added below
        struct dirent *e;
        while ((e = dir_next(x))) {
            if (!guest_name(parent, e->d_name, guest, 0x301)) continue;
            size_t gl = strlen(guest);
            if (ri && gl > sizeof OVERLAY_DELETE - 1 && !strcmp(guest + gl - (sizeof OVERLAY_DELETE - 1), OVERLAY_DELETE)) continue;
            if (nn == cap && !(names = realloc(names, (cap *= 2) * sizeof *names))) die("rom overlay directory snapshot allocation failed");
            strlcpy(names[nn++], guest, sizeof names[0]);
        }
        err = errno; closedir(x);
        if (err) goto done;
    }
    size_t nfold = nn; if (!base) nover = nfold;
    int64_t dk = m->img && base ? img_find(m->img, rest) : -1;
    if (dk >= 0 && !m->img->e[dk].type)   // the image's own entries of this directory, after the overlay folders'
        for (uint64_t j = 0; j < m->img->e[dk].b; j++) {
            const img_ent *c = &m->img->e[m->img->e[dk].a + j]; size_t l = c->nlen < 0x300 ? c->nlen : 0x300;
            memcpy(guest, m->img->names + c->name, l); guest[l] = 0;
            if (nn == cap && !(names = realloc(names, (cap *= 2) * sizeof *names))) die("rom overlay directory snapshot allocation failed");
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
        else if (m->img && (k = img_find(m->img, rel)) >= 0) { o->type = m->img->e[k].type ? ENT_FILE : ENT_DIR; o->size = m->img->e[k].type ? (int64_t)m->img->e[k].b : 0; }
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
    if (img_path(path, &mi, &k)) {   // an image directory: its children are one run of entries, listed from a snapshot like the overlay's
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
        if (dir_snapshot(h, &g_mnt[i], c + 1)) { int err = errno; closedir(d); free(h); errno = err; return err_res("opendir", hp); }
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
        // a directory's type comes from d_type without stat; files need a size, taken with fstatat relative to the directory fd (following links, like the old stat)
        struct stat st; int isdir;
        if (e->d_type == DT_DIR) isdir = 1;
        else { if (fstatat(dfd, e->d_name, &st, 0)) continue; isdir = S_ISDIR(st.st_mode); }
        if (!(h->mode & (isdir ? DIRMODE_DIR : DIRMODE_FILE))) continue;
        DirectoryEntry *o = &ents[n++]; memset(o, 0, sizeof *o);
        if (!guest_name(h->path, e->d_name, o->name, sizeof o->name)) { char c[1280]; snprintf(c, sizeof c, "%s/%s", h->path, e->d_name); *count = n - 1; return err_res("directory name", c); }
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
    if (!hh) return;   // the game also closes after a failed open (handle 0): nothing to do in the original either
    dirh_t *h = (dirh_t *)hh; dir_untrack(h); if (h->d) closedir(h->d); free(h->snap); free(h);
}
// A reset (hle_reset.c), once the game's threads have ended: the file system as a new process finds it. Every file and directory the game had open is closed, and every mount
// but the game's RomFS image goes: the next MountSaveData clears the work directory and clones it from the committed save again, so what the stopped game wrote and never
// committed is lost, as on Horizon when an application is ended, and is never published. The RomFS image's mount stays (its index is the process's; the game's MountRom finds it
// mounted already, which is harmless), as do the mods' RomFS overlays and the directory indexes
void dm_fs_reset(void) {
    pthread_once(&g_txn_lock_once, txn_locks_init);
    PATH_LOCK(); pthread_mutex_lock(&g_lock);
    for (int i = 0; i < MAX_MOUNT; i++) pthread_rwlock_wrlock(&g_txn_lock[i]);   // no write or commit is under way (the game's threads are gone), and none starts meanwhile
    int nf = 0, nd = 0, nm = 0, k = 0;
    for (int fd = 0; fd < 65536; fd++) if (g_fmode[fd] || atomic_load(&g_ftxn[fd])) { close(fd); g_fmode[fd] = 0; atomic_store(&g_ftxn[fd], 0); nf++; }
    pthread_mutex_lock(&g_dirs_mu);
    for (dirh_t *h = g_dirs, *n; h; h = n) { n = h->next; if (h->d) closedir(h->d); free(h->snap); free(h); nd++; }
    g_dirs = NULL;
    pthread_mutex_unlock(&g_dirs_mu);
    for (int i = 0; i < g_nmnt; i++) {
        if (g_mnt[i].img) { if (k != i) g_mnt[k] = g_mnt[i]; k++; }
        else nm++;
    }
    g_nmnt = k;
    pthread_mutex_lock(&g_sdk_h_lock); memset(g_sdk_h, 0, sizeof g_sdk_h); atomic_store(&g_sdk_nh, 0); pthread_mutex_unlock(&g_sdk_h_lock);   // the SDK's own handles (its RAM disk) were in the game's memory
    pthread_mutex_lock(&g_fslog_lock); for (int i = 0; i < 4096; i++) { free(g_fdpath[i]); g_fdpath[i] = NULL; } pthread_mutex_unlock(&g_fslog_lock);
    for (int i = MAX_MOUNT - 1; i >= 0; i--) pthread_rwlock_unlock(&g_txn_lock[i]);
    pthread_mutex_unlock(&g_lock);
    LOG("reset: file system: %d files and %d directories closed, %d mounts dropped (saves and temporary storage mount again; what was not committed is lost)", nf, nd, nm);
}

// ---------- Overloads from newer SDKs (SDK 16: MountRom without a cache, ReadFile without the bytes-read output / with read options, directory entry count) ----------
HLE_FORCE("_ZN2nn2fs8MountRomEPKc", "fsp-srv; rom → host RomFS directory (overload without the cache arguments)", Result, h_fs_MountRom1, const char *name) { return h_fs_MountRom(name, NULL, 0); }
// ReadFile without the bytes-read output: the read must be complete; a short read (past the end of file) is returned as a failure
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvm", "fsp-srv → pread(2), must read in full", Result, h_fs_ReadFileExact, uint64_t h, int64_t off, void *buf, size_t sz) {
    size_t got = 0; Result r = h_fs_ReadFile(&got, h, off, buf, sz);
    if (r != RES_OK) return r;
    if (got != sz) { LOG("fs ReadFile short read: fd %d offset %lld wanted %zu got %zu", (int)FH_IN(h), (long long)off, sz, got); return RES_FAIL; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvmRKNS0_10ReadOptionE", "fsp-srv → pread(2) (overload with read options, which the host ignores)", Result, h_fs_ReadFileOpt,
          size_t *out, uint64_t h, int64_t off, void *buf, size_t sz, const void *opt) { (void)opt; return h_fs_ReadFile(out, h, off, buf, sz); }
// With read options and no bytes-read output: must read in full, like h_fs_ReadFileExact. Unhooked, the SDK's own reads the host's handle as a FileAccessor* (SD GUNDAM G GENERATION CROSS RAYS)
HLE_FORCE("_ZN2nn2fs8ReadFileENS0_10FileHandleElPvmRKNS0_10ReadOptionE", "fsp-srv → pread(2), must read in full (overload with read options, which the host ignores)", Result, h_fs_ReadFileExactOpt,
          uint64_t h, int64_t off, void *buf, size_t sz, const void *opt) { (void)opt; return h_fs_ReadFileExact(h, off, buf, sz); }
HLE_FORCE("_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", "fsp-srv; directory entry count (filtered by the mode given at open, read position untouched)", Result, h_fs_DirCount, int64_t *out, uint64_t hh) { dm_thread_service_request();
    SDK_HANDLE(hh, "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", Result (*)(int64_t *, uint64_t), RES_FAIL, out, hh);
    PATH_LOCK(); dirh_t *h = (dirh_t *)hh; int64_t n = 0;
    if (h->snap) { for (int64_t i = 0; i < h->nsnap; i++) if (h->mode & (h->snap[i].type == ENT_DIR ? DIRMODE_DIR : DIRMODE_FILE)) n++; *out = n; return RES_OK; }
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
// File system access log and thread priority: on the real device both ask fsp-srv (the access log switch lives in the service); the host has no access log and priority does not affect pread
HLE_FORCE("_ZN2nn2fs26SetPriorityOnCurrentThreadENS0_8PriorityE", "fsp-srv; file read priority, ignored by the host", void, h_fs_SetPrio, int prio) { (void)prio; }
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEj", "fsp-srv; access log always off (returns bool)", unsigned, h_fs_AccessLogMask, unsigned mask) { (void)mask; return 0; }
HLE_FORCE("_ZN2nn2fs6detail18IsEnabledAccessLogEv", "fsp-srv; access log always off (returns bool)", unsigned, h_fs_AccessLog, void) { return 0; }
