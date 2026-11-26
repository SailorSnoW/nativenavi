// Save writes and transactions in hle_fs.c (takes seconds) with a scriptable pwrite. Expected values are hand-written from the semantics; the numbered steps are printed below.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>
#include <sys/clonefile.h>                // declared before the define, so only hle_fs.c's calls go to the fake
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off);
static int fake_clonefile(const char *src, const char *dst, uint32_t flags);
#define pwrite fake_pwrite
#define clonefile fake_clonefile
#include "../hle_fs.c"
#include "t_files.h"
#undef pwrite
#undef clonefile
ssize_t pwrite(int, const void *, size_t, off_t);
static const uint64_t k_user1[2] = {DM_UID0, DM_UID1};
static int g_clone_enotsup;                   // 1: clonefile fails with ENOTSUP, as on HFS+ and exFAT
static int fake_clonefile(const char *src, const char *dst, uint32_t flags) {
    if (g_clone_enotsup) { errno = ENOTSUP; return -1; }
    return clonefile(src, dst, flags);
}

struct dm_flags g_flag; uint64_t g_t0_ns; uint64_t now_ns(void) { return 0; }
void dm_guest_write_prepare(void *a, size_t n) { (void)a; (void)n; }
extern int g_nfail, g_lasterr;                                        // the strong implementation in t_fs_stub.c replaces hle_fs.c's weak one
static int g_mode, g_calls;                   // 0 write normally; 1 always return 0; 2 write only 3 bytes the first time
static ssize_t fake_pwrite(int fd, const void *buf, size_t n, off_t off) {
    g_calls++;
    if (g_mode == 1) { if (g_calls > 1000) { printf("  ✗ pwrite retried over 1000 times after returning 0 (endless loop)\n"); exit(1); } return 0; }
    if (g_mode == 2 && g_calls == 1 && n > 3) n = 3;
    return pwrite(fd, buf, n, off);
}
static int fails, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { fails++; printf("  ✗ "); } else printf("  ✓ "); printf(__VA_ARGS__); printf("\n"); } while (0)
static void put(const char *p, const char *s) { FILE *f = fopen(p, "w"); fputs(s, f); fclose(f); }
static int same(const char *p, const char *s) {
    char b[64] = {0}; FILE *f = fopen(p, "r"); if (!f) return 0; size_t k = fread(b, 1, sizeof b - 1, f); fclose(f);
    return k == strlen(s) && !memcmp(b, s, k);
}
static Result write_save(const char *path, const char *s) {
    uint64_t h; Result r = h_fs_OpenFile(&h, path, MODE_WRITE); if (r) return r;
    h_fs_SetFileSize(h, (int64_t)strlen(s));
    r = h_fs_WriteFile(h, 0, s, strlen(s), NULL); h_fs_CloseFile(h); return r;
}
static int read_fs(const char *path, char *out, size_t n) { uint64_t h; size_t got = 0; if (h_fs_OpenFile(&h, path, MODE_READ)) return 0; Result r = h_fs_ReadFile(&got, h, 0, out, n - 1); h_fs_CloseFile(h); out[got] = 0; return !r; }

extern void *(*t_guest_sym)(const char *);
static int g_sdk_cache_mounted, g_sdk_asked, g_sdk_unmounts, g_sdk_no_is_mounted;
static int sdk_is_mounted(const char *name) { g_sdk_asked++; return g_sdk_cache_mounted && !strcmp(name, "cache"); }
static void sdk_unmount(const char *name) { g_sdk_unmounts++; if (!g_sdk_cache_mounted || strcmp(name, "cache")) abort(); g_sdk_cache_mounted = 0; }   // as the SDK's: not mounted aborts
static void *sdk_sym(const char *name) {
    if (!strcmp(name, "_ZN2nn2fs9IsMountedEPKc")) return g_sdk_no_is_mounted ? NULL : (void *)sdk_is_mounted;
    if (!strcmp(name, "_ZN2nn2fs7UnmountEPKc")) return (void *)sdk_unmount;
    return NULL;
}
#define VT_FILE ((uint64_t)0x7bbd001230)   // the SDK's handles: FileAccessor / DirectoryAccessor pointers
#define VT_DIR ((uint64_t)0x7bbd004560)
#define VT_INI "vtapi:/VoiceText/vt/vtpath.ini"
static int g_vt_calls, g_vt_closed, g_vt_dirs_closed, g_vt_mount_asks;
static int vt_is_mounted(const char *n) { g_vt_mount_asks++; return !strcmp(n, "vtapi"); }
static int vt_on(const char *p) { return !strncmp(p, "vtapi:", 6); }
static Result vt_entry_type(int32_t *out, const char *p) { g_vt_calls++; if (!strcmp(p, VT_INI)) { *out = ENT_FILE; return RES_OK; } if (!strcmp(p, "vtapi:/VoiceText")) { *out = ENT_DIR; return RES_OK; } return RES_PATH_NOT_FOUND; }
static Result vt_path(const char *p) { g_vt_calls++; return vt_on(p) ? RES_OK : RES_FAIL; }
static Result vt_create_file(const char *p, int64_t sz) { (void)sz; return vt_path(p); }
static Result vt_rename(const char *a, const char *b) { g_vt_calls++; return vt_on(a) && vt_on(b) ? RES_OK : RES_FAIL; }
static Result vt_open(uint64_t *out, const char *p, int mode) { (void)mode; g_vt_calls++; if (strcmp(p, VT_INI)) return RES_PATH_NOT_FOUND; *out = VT_FILE; return RES_OK; }
static Result vt_read(size_t *out, uint64_t h, int64_t off, void *buf, size_t sz) {
    g_vt_calls++; if (h != VT_FILE) abort();   // the SDK's ReadFile dereferences its handle as a FileAccessor*
    size_t k = off >= 3 ? 0 : (size_t)(3 - off) < sz ? (size_t)(3 - off) : sz; memcpy(buf, &"ini"[off], k); *out = k; return RES_OK;
}
static Result vt_write(uint64_t h, int64_t off, const void *buf, size_t sz, const void *opt) { (void)off; (void)buf; (void)sz; (void)opt; g_vt_calls++; return h == VT_FILE ? RES_OK : RES_FAIL; }
static Result vt_size(int64_t *out, uint64_t h) { g_vt_calls++; *out = 3; return h == VT_FILE ? RES_OK : RES_FAIL; }
static Result vt_set_size(uint64_t h, int64_t sz) { (void)sz; g_vt_calls++; return h == VT_FILE ? RES_OK : RES_FAIL; }
static Result vt_flush(uint64_t h) { g_vt_calls++; return h == VT_FILE ? RES_OK : RES_FAIL; }
static void vt_close(uint64_t h) { g_vt_calls++; if (h == VT_FILE) g_vt_closed++; }
static int vt_open_mode(uint64_t h) { g_vt_calls++; return h == VT_FILE ? (MODE_READ | MODE_WRITE) : 0; }
static Result vt_open_dir(uint64_t *out, const char *p, int mode) { (void)mode; g_vt_calls++; if (strcmp(p, "vtapi:/VoiceText")) return RES_PATH_NOT_FOUND; *out = VT_DIR; return RES_OK; }
static Result vt_read_dir(int64_t *n, DirectoryEntry *e, uint64_t h, int64_t max) { g_vt_calls++; if (h != VT_DIR || max < 1) abort(); memset(e, 0, sizeof *e); strcpy(e->name, "vt"); e->type = ENT_DIR; *n = 1; return RES_OK; }
static Result vt_dir_count(int64_t *out, uint64_t h) { g_vt_calls++; if (h != VT_DIR) abort(); *out = 1; return RES_OK; }
static void vt_close_dir(uint64_t h) { g_vt_calls++; if (h == VT_DIR) g_vt_dirs_closed++; }
static void *vt_sym(const char *name) {
    static const struct { const char *name; void *fn; } t[] = {
        { "_ZN2nn2fs9IsMountedEPKc", (void *)vt_is_mounted },
        { "_ZN2nn2fs12GetEntryTypeEPNS0_18DirectoryEntryTypeEPKc", (void *)vt_entry_type },
        { "_ZN2nn2fs15CreateDirectoryEPKc", (void *)vt_path },
        { "_ZN2nn2fs10CreateFileEPKcl", (void *)vt_create_file },
        { "_ZN2nn2fs10DeleteFileEPKc", (void *)vt_path },
        { "_ZN2nn2fs26DeleteDirectoryRecursivelyEPKc", (void *)vt_path },
        { "_ZN2nn2fs15DeleteDirectoryEPKc", (void *)vt_path },
        { "_ZN2nn2fs10RenameFileEPKcS2_", (void *)vt_rename },
        { "_ZN2nn2fs15RenameDirectoryEPKcS2_", (void *)vt_rename },
        { "_ZN2nn2fs8OpenFileEPNS0_10FileHandleEPKci", (void *)vt_open },
        { "_ZN2nn2fs8ReadFileEPmNS0_10FileHandleElPvm", (void *)vt_read },
        { "_ZN2nn2fs9WriteFileENS0_10FileHandleElPKvmRKNS0_11WriteOptionE", (void *)vt_write },
        { "_ZN2nn2fs11GetFileSizeEPlNS0_10FileHandleE", (void *)vt_size },
        { "_ZN2nn2fs11SetFileSizeENS0_10FileHandleEl", (void *)vt_set_size },
        { "_ZN2nn2fs9FlushFileENS0_10FileHandleE", (void *)vt_flush },
        { "_ZN2nn2fs9CloseFileENS0_10FileHandleE", (void *)vt_close },
        { "_ZN2nn2fs15GetFileOpenModeENS0_10FileHandleE", (void *)vt_open_mode },
        { "_ZN2nn2fs13OpenDirectoryEPNS0_15DirectoryHandleEPKci", (void *)vt_open_dir },
        { "_ZN2nn2fs13ReadDirectoryEPlPNS0_14DirectoryEntryENS0_15DirectoryHandleEl", (void *)vt_read_dir },
        { "_ZN2nn2fs22GetDirectoryEntryCountEPlNS0_15DirectoryHandleE", (void *)vt_dir_count },
        { "_ZN2nn2fs14CloseDirectoryENS0_15DirectoryHandleE", (void *)vt_close_dir },
    };
    for (size_t i = 0; i < sizeof t / sizeof *t; i++) if (!strcmp(name, t[i].name)) return t[i].fn;
    return NULL;
}

int main(void) {
    char root[1024], save[1100], work[1200], p[1200];
    const char *sp = getenv("TMPDIR"); snprintf(root, sizeof root, "%s/t_fs.XXXXXX", sp ? sp : "/private/tmp");
    if (!mkdtemp(root)) { perror("mkdtemp"); return 1; }
    char rom[1100], overlay[1100], rdata[1200], odata[1200], fn[1300], got[64];
    snprintf(rom, sizeof rom, "%s/rom", root); snprintf(overlay, sizeof overlay, "%s/overlay", root); snprintf(rdata, sizeof rdata, "%s/data", rom); snprintf(odata, sizeof odata, "%s/data", overlay);
    mkdir(rom, 0755); mkdir(overlay, 0755); mkdir(rdata, 0755); mkdir(odata, 0755);
    snprintf(fn, sizeof fn, "%s/same", rdata); put(fn, "base"); snprintf(fn, sizeof fn, "%s/same", odata); put(fn, "overlay"); snprintf(fn, sizeof fn, "%s/base", rdata); put(fn, "only-base"); snprintf(fn, sizeof fn, "%s/over", odata); put(fn, "only-overlay");
    setenv("DM_ROMFS", rom, 1); setenv("DM_ROMFS_OVERLAY", overlay, 1); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount RomFS with two layers (base + overlay)");
    CHECK(read_fs("rom:/data/same", got, sizeof got) && !strcmp(got, "overlay"), "the overlay file wins"); CHECK(read_fs("rom:/data/base", got, sizeof got) && !strcmp(got, "only-base"), "falls back to the base when the overlay lacks the file");
    uint64_t rh; CHECK(h_fs_OpenFile(&rh, "rom:/data/same", MODE_WRITE) == RES_FAIL, "the two-layer mount refuses writes");
    CHECK(read_fs("rom:\\data\\over", got, sizeof got) && !strcmp(got, "only-overlay") && read_fs("rom:\\data\\base", got, sizeof got) && !strcmp(got, "only-base"), "backslashes separate as slashes do, in both layers");
    CHECK(h_fs_OpenFile(&rh, "rom:/data/base", MODE_READ) == RES_OK && rh && !(rh & 15), "a folder file's handle is 16-byte aligned, as a Horizon FileAccessor pointer is (%#llx)", (unsigned long long)rh); h_fs_CloseFile(rh);
    g_nmnt = 0;
    unsetenv("DM_ROMFS_OVERLAY");
    {   // a stale path or fifo stops the rom mount with FATAL: in a child
        char gone[1200], fifo[1200], out[1200]; snprintf(gone, sizeof gone, "%s/RomFS", root); snprintf(fifo, sizeof fifo, "%s/fifo", root); snprintf(out, sizeof out, "%s/fatal.txt", root);
        mkfifo(fifo, 0644);
        const char *paths[2] = {gone, fifo};
        for (int i = 0; i < 2; i++) {
            fflush(NULL); pid_t pid = fork();
            if (!pid) { freopen(out, "w", stderr); setenv("DM_ROMFS", paths[i], 1); h_fs_MountRom("rom", NULL, 0); _exit(0); }
            int st = 0; waitpid(pid, &st, 0);
            char line[512] = {0}; FILE *f = fopen(out, "r"); if (f) { fread(line, 1, sizeof line - 1, f); fclose(f); }
            CHECK(WIFSIGNALED(st) && WTERMSIG(st) == SIGABRT && strstr(line, "FATAL the game files") && strstr(line, paths[i]), "%s game files stop the rom mount", i ? "a fifo as the" : "missing");
        }
    }

    printf("RomFS image\n");   // by hand in romfs_image.py's layout: /data/a = "hello", /data/b empty, /data/c = "xy"
    {   uint8_t img[32 + 7 + 5 * 24 + 7] = {0}; uint64_t hd[3] = {39, 5 * 24 + 7, 5};
        img_ent e[5] = {{1, 1, 0, 0, 0, 0}, {2, 3, 0, 4, 0, 0}, {32, 5, 4, 1, 1, 0}, {5, 0, 5, 1, 0, 0}, {37, 2, 6, 1, 1, 0}};
        memcpy(img, "DMROMFS1", 8); memcpy(img + 8, hd, sizeof hd); memcpy(img + 32, "helloxy", 7); memcpy(img + 39, e, sizeof e); memcpy(img + 39 + sizeof e, "dataabc", 7);
        char ip[1100]; snprintf(ip, sizeof ip, "%s/rom.img", root); FILE *f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
        setenv("DM_ROMFS", ip, 1); unsetenv("DM_ROMFS_OVERLAY"); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image");
        CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "hello"), "reads a file"); CHECK(read_fs("rom:/data/../data/c", got, sizeof got) && !strcmp(got, "xy"), "reads the next file, through ..");
        uint64_t h; size_t n = 0; int64_t sz = 0; char b[16] = {0};
        CHECK(h_fs_OpenFile(&h, "rom:/data/a", MODE_READ) == RES_OK && h_fs_ReadFile(&n, h, 3, b, 10) == RES_OK && n == 2 && !memcmp(b, "lo", 2), "a read stops at the end of the file (got %zu)", n);
        CHECK(h_fs_GetFileSize(&sz, h) == RES_OK && sz == 5 && h_fs_GetFileOpenMode(h) == MODE_READ, "size 5, open for reading");
        { const uint32_t opt = 0; memset(b, 0, sizeof b);
          CHECK(h_fs_ReadFileExact(h, 1, b, 4) == RES_OK && !memcmp(b, "ello", 4) && h_fs_ReadFileExactOpt(h, 0, b, 5, &opt) == RES_OK && !memcmp(b, "hello", 5), "full reads without a byte count succeed");
          CHECK(h_fs_ReadFileExact(h, 3, b, 10) != RES_OK && h_fs_ReadFileExactOpt(h, 3, b, 10, &opt) != RES_OK, "short reads without a byte count fail"); }
        h_fs_CloseFile(h);
        // a game's file device may mask a handle's low bits (handle & ~7) as on a Horizon pointer
        CHECK(h_fs_OpenFile(&h, "rom:/data/c", MODE_READ) == RES_OK && !(h & 15) && h_fs_ReadFile(&n, h & ~7ull, 0, b, 10) == RES_OK && n == 2 && !memcmp(b, "xy", 2), "entry 4 still reads after its handle's low bits are masked off (%#llx)", (unsigned long long)h);
        CHECK(h_fs_GetFileSize(&sz, h & ~7ull) == RES_OK && sz == 2, "and is 2 bytes"); h_fs_CloseFile(h & ~7ull);
        CHECK(read_fs("rom:\\data\\c", got, sizeof got) && !strcmp(got, "xy") && read_fs("rom:\\data\\..\\data\\a", got, sizeof got) && !strcmp(got, "hello"), "backslash paths, also through ..");
        CHECK(h_fs_OpenFile(&h, "rom:/data/x", MODE_READ) == RES_PATH_NOT_FOUND, "a missing file is not found");
        CHECK(h_fs_OpenFile(&h, "rom:/data/A", MODE_READ) == RES_PATH_NOT_FOUND, "names are case-sensitive");
        CHECK(h_fs_OpenFile(&h, "rom:/data/b", MODE_READ) == RES_PATH_NOT_FOUND, "a directory does not open as a file");
        CHECK(h_fs_OpenFile(&h, "rom:/data/a", MODE_WRITE) == RES_FAIL && h_fs_CreateFile("rom:/data/a", 1) == RES_PATH_EXISTS, "refuses writes; create answers \"exists\"");
        int32_t t = -1; CHECK(h_fs_GetEntryType(&t, "rom:/data/b") == RES_OK && t == ENT_DIR && h_fs_GetEntryType(&t, "rom:/data/c") == RES_OK && t == ENT_FILE, "entry types");
        DirectoryEntry de[4]; int64_t cnt = 0;
        CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 3, "a directory of 3 entries (%lld)", (long long)cnt);
        CHECK(h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 3 && !strcmp(de[0].name, "a") && de[0].size == 5 && de[1].type == ENT_DIR && !strcmp(de[2].name, "c"), "lists a, b, c"); h_fs_CloseDirectory(h);
        CHECK(h_fs_OpenDirectory(&h, "rom:/", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 0, "the file filter hides directories"); h_fs_CloseDirectory(h);
        CHECK(h_fs_OpenDirectory(&h, "rom:/data/b", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 0, "an empty directory"); h_fs_CloseDirectory(h);
        {   // an image directory reads by cursor, with no snapshot; DirCount leaves the cursor
            CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_FILE) == RES_OK && !((dirh_t *)h)->snap && ((dirh_t *)h)->img, "image directory opens with a cursor and no snapshot");
            memset(de, 0xaa, sizeof de);
            CHECK(h_fs_ReadDirectory(&cnt, de, h, 0) == RES_OK && cnt == 0, "a 0-entry read returns 0 and leaves the cursor at the start");
            CHECK(h_fs_ReadDirectory(&cnt, de, h, 1) == RES_OK && cnt == 1 && !strcmp(de[0].name, "a") && de[0].type == ENT_FILE && de[0].size == 5, "first filtered 1-entry batch returns a");
            int zero = 1; for (size_t i = 2; i < sizeof de[0].name; i++) zero &= !de[0].name[i];
            for (size_t i = 0; i < sizeof de[0].pad0; i++) zero &= !de[0].pad0[i];
            for (size_t i = 0; i < sizeof de[0].pad1; i++) zero &= !de[0].pad1[i];
            CHECK(zero, "DirectoryEntry padding and name tail are zeroed");
            CHECK(h_fs_DirCount(&cnt, h) == RES_OK && cnt == 2, "DirCount counts filtered files (2) mid-read");
            CHECK(h_fs_ReadDirectory(&cnt, de, h, 1) == RES_OK && cnt == 1 && !strcmp(de[0].name, "c") && de[0].type == ENT_FILE && de[0].size == 2, "second batch skips directory b and returns c (cursor untouched by DirCount)");
            CHECK(h_fs_ReadDirectory(&cnt, de, h, 1) == RES_OK && cnt == 0, "third batch reaches the end");
            h_fs_CloseDirectory(h);
            CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 1, "dir-only filter counts 1");
            CHECK(h_fs_ReadDirectory(&cnt, de, h, 1) == RES_OK && cnt == 1 && !strcmp(de[0].name, "b") && de[0].type == ENT_DIR && de[0].size == 0, "dir-only batch returns b with size 0");
            CHECK(h_fs_ReadDirectory(&cnt, de, h, 1) == RES_OK && cnt == 0, "dir-only second batch reaches the end");
            h_fs_CloseDirectory(h);
            CHECK(h_fs_OpenDirectory(&h, "rom:/data", 0) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 0 && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 0, "mode 0 counts and lists nothing");
            h_fs_CloseDirectory(h);
        }
        g_nmnt = 0;
        char ov[1100], ovd[1200]; snprintf(ov, sizeof ov, "%s/imgov", root); snprintf(ovd, sizeof ovd, "%s/data", ov); mkdir(ov, 0755); mkdir(ovd, 0755);
        snprintf(fn, sizeof fn, "%s/a", ovd); put(fn, "mod"); snprintf(fn, sizeof fn, "%s/new", ovd); put(fn, "added");
        setenv("DM_ROMFS_OVERLAY", ov, 1); CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image with an overlay folder");
        CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "mod"), "the overlay file wins over the image");
        CHECK(read_fs("rom:/data/c", got, sizeof got) && !strcmp(got, "xy") && read_fs("rom:/data/new", got, sizeof got) && !strcmp(got, "added"), "image files and overlay-only files both read");
        CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 4, "the listing merges both, once each (%lld)", (long long)cnt);
        int seen = 0; for (int i = 0; i < cnt; i++) { if (!strcmp(de[i].name, "a")) seen += de[i].size == 3; if (!strcmp(de[i].name, "b")) seen += de[i].type == ENT_DIR; if (!strcmp(de[i].name, "c")) seen += de[i].size == 2; if (!strcmp(de[i].name, "new")) seen += de[i].size == 5; }
        CHECK(seen == 4, "with the right types and sizes"); h_fs_CloseDirectory(h);
        CHECK(read_fs("rom:\\data\\a", got, sizeof got) && !strcmp(got, "mod") && read_fs("rom:\\data\\c", got, sizeof got) && !strcmp(got, "xy"), "backslash paths reach the overlay folder and the image");
        h = 0; cnt = -1; CHECK(h_fs_OpenDirectory(&h, "rom:\\data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_DirCount(&cnt, h) == RES_OK && cnt == 4, "and list the merged directory (%lld)", (long long)cnt); h_fs_CloseDirectory(h);
        g_nmnt = 0;
        {   // three folders, highest first: one that deletes, the one above, one differing from the game's in case
            char o2[1100], o3[1100], list[3400], dir[1200];
            snprintf(o2, sizeof o2, "%s/ovdel", root); snprintf(o3, sizeof o3, "%s/ovcase", root);
            snprintf(dir, sizeof dir, "%s/data", o2); int staged = !t_mkdirs(dir); snprintf(dir, sizeof dir, "%s/DATA/B", o3); CHECK(staged && !t_mkdirs(dir), "stage the overlay folders");
            snprintf(fn, sizeof fn, "%s/data/c.dm-delete", o2); put(fn, ""); snprintf(fn, sizeof fn, "%s/data/new.dm-delete", o2); put(fn, "");
            snprintf(fn, sizeof fn, "%s/DATA/C", o3); put(fn, "lower"); snprintf(fn, sizeof fn, "%s/DATA/Extra", o3); put(fn, "x"); snprintf(fn, sizeof fn, "%s/DATA/B/inner", o3); put(fn, "in");
            snprintf(list, sizeof list, "%s:%s:%s", o2, ov, o3); setenv("DM_ROMFS_OVERLAY", list, 1);
            CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image with three overlay folders");
            CHECK(read_fs("rom:/data/a", got, sizeof got) && !strcmp(got, "mod"), "a file of the middle folder still reads");
            CHECK(h_fs_OpenFile(&h, "rom:/data/c", MODE_READ) == RES_PATH_NOT_FOUND && h_fs_GetEntryType(&t, "rom:/data/c") != RES_OK, "a delete marker hides the game's file, and a lower folder's of that name");
            CHECK(h_fs_OpenFile(&h, "rom:/data/new", MODE_READ) == RES_PATH_NOT_FOUND, "a delete marker hides a lower folder's file");
            CHECK(read_fs("rom:/data/Extra", got, sizeof got) && !strcmp(got, "x") && read_fs("rom:/data/extra", got, sizeof got) && !strcmp(got, "x"), "a folder's names that differ in case from the asked path are taken");
            CHECK(read_fs("rom:/data/b/inner", got, sizeof got) && !strcmp(got, "in"), "also below a directory the game has");
            DirectoryEntry d8[8]; cnt = 0; seen = 0;
            CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, d8, h, 8) == RES_OK && cnt == 3, "the listing leaves out deleted files and markers (%lld)", (long long)cnt);
            for (int i = 0; i < cnt; i++) seen += (!strcmp(d8[i].name, "a") && d8[i].size == 3) || (!strcmp(d8[i].name, "b") && d8[i].type == ENT_DIR) || (!strcmp(d8[i].name, "Extra") && d8[i].size == 1);
            CHECK(seen == 3, "and names what the game has by the game's spelling"); h_fs_CloseDirectory(h);
            g_nmnt = 0;
            snprintf(o2, sizeof o2, "%s/ovdir", root); snprintf(dir, sizeof dir, "%s/data", o2); CHECK(!t_mkdirs(dir), "stage a folder that replaces a directory");
            snprintf(fn, sizeof fn, "%s/data.dm-delete", o2); put(fn, ""); snprintf(fn, sizeof fn, "%s/data/only", o2); put(fn, "o");
            snprintf(list, sizeof list, "%s:%s", o2, ov); setenv("DM_ROMFS_OVERLAY", list, 1);
            CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the image with a folder that replaces a directory");
            CHECK(read_fs("rom:/data/only", got, sizeof got) && !strcmp(got, "o") && h_fs_OpenFile(&h, "rom:/data/a", MODE_READ) == RES_PATH_NOT_FOUND && h_fs_OpenFile(&h, "rom:/data/c", MODE_READ) == RES_PATH_NOT_FOUND,
                  "a deleted directory with the folder's own in its place: only the folder's files");
            cnt = 0; CHECK(h_fs_OpenDirectory(&h, "rom:/data", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, d8, h, 8) == RES_OK && cnt == 1 && !strcmp(d8[0].name, "only"), "and its listing (%lld)", (long long)cnt); h_fs_CloseDirectory(h);
            cnt = 0; CHECK(h_fs_OpenDirectory(&h, "rom:/", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, d8, h, 8) == RES_OK && cnt == 1 && !strcmp(d8[0].name, "data"), "the root lists the directory, not the marker (%lld)", (long long)cnt); h_fs_CloseDirectory(h);
        }
        unsetenv("DM_ROMFS_OVERLAY"); g_nmnt = 0;
        CHECK(h_fs_MountRom("rom", NULL, 0) == RES_OK, "mount the RomFS"); h_fs_Unmount("rom");
        CHECK(host_mounted("rom") && read_fs("rom:/data/c", got, sizeof got) && !strcmp(got, "xy"), "Unmount leaves the RomFS mounted");
        g_nmnt = 0;
        {   // an entry's flags byte must be 0
            romfs_img *clean = img_open(ip); CHECK(clean != NULL, "the image opens by itself"); if (clean) romfs_image_close(clean);
            e[2].flags = 1; memcpy(img + 39, e, sizeof e); f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
            CHECK(!img_open(ip), "a flag on a file is refused");
            e[2].flags = 0; e[1].flags = 1; memcpy(img + 39, e, sizeof e); f = fopen(ip, "wb"); fwrite(img, 1, sizeof img, f); fclose(f);
            CHECK(!img_open(ip), "a flag on a directory is refused");
            e[1].flags = 0; memcpy(img + 39, e, sizeof e);
        }
        f = fopen(ip, "wb"); fwrite(img, 1, 39 + 50, f); fclose(f);   // cut off inside the index
        CHECK(!img_open(ip), "a cut-off image is refused");
    }
    snprintf(save, sizeof save, "%s/save", root); snprintf(work, sizeof work, "%s.work", save);
    mkdir(save, 0755); snprintf(p, sizeof p, "%s/a", save); put(p, "old");
    setenv("DM_SAVE", save, 1); setenv("DM_TEST_FS_FAIL", "write:5:28", 1);   // errno 28 = ENOSPC
    CHECK(h_fs_MountSave("save", k_user1) == RES_OK, "mount the save");

    // Save write count: (1)=1st (2)=2nd (3)=3rd (4)=4th/5th/6th/7th (5)=8th
    printf("① pwrite returns 0\n");
    g_mode = 1; g_calls = 0;
    CHECK(write_save("save:/a", "xyz") == RES_FAIL, "WriteFile returns failure 0x2A2");
    CHECK(g_calls == 1, "pwrite called only once (actually %d times; it used to retry forever)", g_calls);

    printf("② pwrite writes in pieces\n");
    g_mode = 2; g_calls = 0;
    CHECK(write_save("save:/a", "abcdefg") == RES_OK, "WriteFile succeeds");
    CHECK(g_calls == 2, "pwrite called twice (3 bytes + 4 bytes; actually %d)", g_calls);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "abcdefg"), "the work directory holds the complete abcdefg");
    g_mode = 0;

    printf("③ transaction\n");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 1 && g_lasterr == ENOSPC, "the Commit of the transaction holding ①② is refused, front end notified once errno=ENOSPC (actually %d times errno=%d)", g_nfail, g_lasterr);
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "old"), "work directory discarded, back to the committed version old");
    CHECK(write_save("save:/a", "new") == RES_OK, "next transaction: write new");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "old"), "before Commit the committed directory is still old");
    CHECK(h_fs_Commit("save") == RES_OK, "Commit");
    CHECK(same(p, "new"), "after Commit the committed directory is new");
    snprintf(p, sizeof p, "%s.new", save); CHECK(access(p, F_OK) != 0, "the swapped-out old version is deleted (no .new left)");

    printf("④ DM_TEST_FS_FAIL=write:5:28\n");
    CHECK(write_save("save:/b", "4") == RES_PATH_NOT_FOUND, "(b does not exist: open fails, not counted as a write)");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "4") == RES_OK, "4th write succeeds");
    errno = 0; CHECK(write_save("save:/b", "5") == RES_FAIL && errno == 28, "5th write fails errno=28 (actually errno=%d)", errno);
    CHECK(write_save("save:/b", "6") == RES_OK, "6th write succeeds");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == 2 && g_lasterr == 28, "that transaction's Commit is refused, front end notified errno=28 (%d times in total)", g_nfail);
    snprintf(p, sizeof p, "%s/b", save); CHECK(access(p, F_OK) != 0, "committed directory unchanged (no b)");
    snprintf(p, sizeof p, "%s/b", work); CHECK(access(p, F_OK) != 0, "work directory discarded (no b)");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "committed directory a is still new");
    h_fs_CreateFile("save:/b", 0);
    CHECK(write_save("save:/b", "7") == RES_OK && h_fs_Commit("save") == RES_OK, "next transaction: write b=7, Commit succeeds");
    snprintf(p, sizeof p, "%s/b", save); CHECK(same(p, "7"), "final committed b = 7 (the next transaction's content)");
    CHECK(g_nfail == 2, "a successful transaction sends no notification (still %d times in total)", g_nfail);

    printf("⑤ remounting discards uncommitted content\n");
    CHECK(write_save("save:/a", "v9") == RES_OK, "write a=v9 without committing");
    g_nmnt = 0;                                                        // a process restart
    CHECK(h_fs_MountSave("save", k_user1) == RES_OK, "remount");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "work directory a is back to the committed version new");
    snprintf(p, sizeof p, "%s/b", work); CHECK(same(p, "7"), "work directory b = the committed 7");

    printf("⑥ failed writes are handled without exiting (DM_TEST_FS_FAILAT)\n");
    unsetenv("DM_TEST_FS_FAIL");
    // 6a a failed reclone after a refused commit does not exit; the commit after the next reclone succeeds
    setenv("DM_TEST_FS_FAILAT", "reclone:1", 1);
    CHECK(write_save("save:/a", "bad") == RES_OK, "write a=bad");
    save_poison(ENOSPC); int nf0 = g_nfail;
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "this transaction is refused (injected reclone failure), the process is still alive, front end notified once");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "new"), "committed directory a is still new");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "bad"), "work directory not deleted (a is still bad)");
    CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 2, "still tainted: the next commit is refused too");
    snprintf(p, sizeof p, "%s/a", work); CHECK(same(p, "new"), "this time the reclone works: work directory a is back to new");
    snprintf(p, sizeof p, "%s.re", work); CHECK(access(p, F_OK) != 0, "no .re left");
    CHECK(write_save("save:/a", "good") == RES_OK && h_fs_Commit("save") == RES_OK, "next transaction writes a=good, commit succeeds");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "committed a = good");
    // 6b a failed guest-name write and rollback in a transactional save: commit refused, recloning recovers
    setenv("DM_TEST_FS_FAILAT", "guestname:1,renameback:1", 1);
    h_fs_CreateFile("save:/c", 0); CHECK(write_save("save:/c", "c") == RES_OK, "write c");
    CHECK(h_fs_RenameFile("save:/c", "save:/d") != RES_OK, "rename c → d returns failure (both steps injected to fail), the process is still alive");
    nf0 = g_nfail; CHECK(h_fs_Commit("save") == RES_FAIL && g_nfail == nf0 + 1, "this transaction's commit is refused");
    snprintf(p, sizeof p, "%s/c", save); CHECK(access(p, F_OK) != 0, "committed directory has no c");
    snprintf(p, sizeof p, "%s/d", save); CHECK(access(p, F_OK) != 0, "committed directory has no d");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "good"), "committed a is still good");
    CHECK(write_save("save:/a", "ok2") == RES_OK && h_fs_Commit("save") == RES_OK, "the next transaction commits as usual");
    // 6c a failed work-directory clone at mount: mounted read-only from the committed directory, front end notified once
    setenv("DM_TEST_FS_FAILAT", "mountclone:1", 1); g_nmnt = 0; nf0 = g_nfail;
    t_rmtree(work);
    CHECK(h_fs_MountSave("save", k_user1) == RES_OK && g_nfail == nf0 + 1, "mount returns success, front end notified once (actually %d times)", g_nfail - nf0);
    CHECK(read_fs("save:/a", got, sizeof got) && !strcmp(got, "ok2"), "the read-only mount reads the committed a=ok2 (read %s)", got);
    CHECK(write_save("save:/a", "zzz") != RES_OK, "writes fail");
    CHECK(h_fs_Commit("save") == RES_OK, "commit is a no-op");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok2"), "committed a is still ok2");
    unsetenv("DM_TEST_FS_FAILAT"); g_nmnt = 0;
    CHECK(h_fs_MountSave("save", k_user1) == RES_OK && write_save("save:/a", "ok3") == RES_OK && h_fs_Commit("save") == RES_OK, "remount without injection: transactional as usual, writing a=ok3 and committing succeed");
    snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok3"), "committed a = ok3");

    printf("⑦ backslash paths on a save\n");
    {   uint64_t h; DirectoryEntry de[4]; int64_t cnt = 0; int32_t t = -1;
        CHECK(h_fs_CreateDirectory("save:\\dir") == RES_OK && h_fs_CreateFile("save:\\dir\\f", 0) == RES_OK && write_save("save:\\dir\\f", "bs") == RES_OK, "create and write save:\\dir\\f");
        CHECK(read_fs("save:/dir/f", got, sizeof got) && !strcmp(got, "bs"), "it is save:/dir/f");
        CHECK(h_fs_RenameFile("save:\\dir\\f", "save:\\dir\\g") == RES_OK && h_fs_GetEntryType(&t, "save:/dir/g") == RES_OK && t == ENT_FILE, "rename by backslash path");
        h = 0; CHECK(h_fs_OpenDirectory(&h, "save:\\dir", DIRMODE_FILE) == RES_OK && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 1 && !strcmp(de[0].name, "g"), "the directory lists g, not a name with backslashes (%s)", cnt ? de[0].name : "-"); h_fs_CloseDirectory(h);
    }

    printf("⑦b a rename between two mounts is refused before either is touched, as Horizon refuses it (ResultRenameToOtherFileSystem)\n");
    {   char rod[1100], tmpd[1100], q[1200]; int32_t t = -1;
        snprintf(rod, sizeof rod, "%s/rodir", root); mkdir(rod, 0755); snprintf(tmpd, sizeof tmpd, "%s/tmpst", root); mkdir(tmpd, 0755);
        CHECK(mount("rodir", rod, 1) == RES_OK && mount("tmpst", tmpd, 0) == RES_OK && mount_ex("dsave", save, 0, 1) == RES_OK && g_mnt[g_nmnt - 1].txn_root == g_mnt[0].txn_root,
              "mount a read-only folder, a writable one and a second name for the save (one transaction)");
        CHECK(h_fs_RenameFile("save:/dir/g", "rodir:/g") == RES_RENAME_OTHER_FS && h_fs_GetEntryType(&t, "save:/dir/g") == RES_OK, "a file into the read-only mount is refused, and stays");
        snprintf(q, sizeof q, "%s/g", rod); CHECK(access(q, F_OK) != 0, "nothing is written into the read-only folder");
        CHECK(h_fs_RenameDir("save:/dir", "tmpst:/dir") == RES_RENAME_OTHER_FS && h_fs_GetEntryType(&t, "save:/dir") == RES_OK && h_fs_GetEntryType(&t, "tmpst:/dir") != RES_OK,
              "a directory into another writable mount is refused, and stays");
        CHECK(h_fs_RenameFile("dsave:/dir/g", "save:/dir/h") == RES_RENAME_OTHER_FS && h_fs_GetEntryType(&t, "dsave:/dir/g") == RES_OK && h_fs_GetEntryType(&t, "save:/dir/h") != RES_OK,
              "between two names of one save too: two mounts on Horizon");
        CHECK(h_fs_RenameFile("dsave:/dir/g", "dsave:/dir/h") == RES_OK && h_fs_GetEntryType(&t, "save:/dir/h") == RES_OK, "on one mount the rename is made");
        CHECK(h_fs_CreateFile("tmpst:/a", 0) == RES_OK && h_fs_RenameFile("tmpst:/a", "tmpst:/b") == RES_OK && h_fs_GetEntryType(&t, "tmpst:/b") == RES_OK, "and on a plain writable mount");
    }

    printf("⑧ a save folder that cannot clone, and symlinks in recursive deletes\n");
    {   g_nmnt = 0; g_clone_enotsup = 1; int nf1 = g_nfail;
        CHECK(h_fs_MountSave("save", k_user1) == RES_OK && g_mnt[0].ro && g_nfail == nf1 + 1, "clonefile ENOTSUP: the committed save mounts read-only and the front end is told once");
        CHECK(read_fs("save:/a", got, sizeof got) && !strcmp(got, "ok3"), "the committed save still reads (a = %s)", got);
        CHECK(write_save("save:/a", "lost") != RES_OK, "a write is refused");
        snprintf(p, sizeof p, "%s/a", save); CHECK(same(p, "ok3"), "committed a is still ok3");
        g_clone_enotsup = 0; g_nmnt = 0;
        CHECK(h_fs_MountSave("save", k_user1) == RES_OK && !g_mnt[0].ro, "with cloning back, the save mounts writable");
        char victim[1100], vf[1200], lnk[1200];
        snprintf(victim, sizeof victim, "%s/victim", root); mkdir(victim, 0755); snprintf(vf, sizeof vf, "%s/keep", victim); put(vf, "keep");
        CHECK(h_fs_CreateDirectory("save:/d") == RES_OK, "create save:/d");
        snprintf(lnk, sizeof lnk, "%s/d/lnk", work); CHECK(symlink(victim, lnk) == 0, "a symlink to a directory outside the save inside save:/d");
        CHECK(h_fs_DeleteDirRec("save:/d") == RES_OK && access(lnk, F_OK) && same(vf, "keep"), "DeleteDirectoryRecursively removes the symlink, not its target's files");
        snprintf(lnk, sizeof lnk, "%s/lnk2", root); symlink(victim, lnk); errno = 0;
        CHECK(rm_tree(lnk) == -1 && errno == ENOTDIR && same(vf, "keep"), "rm_tree of a symlink to a directory refuses it (ENOTDIR) and leaves the target alone");
    }

    {   // Unmount follows the mount's owner: the host's table is permanent, the SDK's entry is released in the SDK
        CHECK(host_mounted("save"), "the host holds save");
        h_fs_Unmount("cache"); CHECK(1, "Unmount with no SDK in the process is a no-op");
        t_guest_sym = sdk_sym; g_sdk_cache_mounted = 1;
        h_fs_Unmount("save");
        CHECK(!g_sdk_asked && !g_sdk_unmounts && host_mounted("save"), "a host mount stays and the SDK is not asked");
        h_fs_Unmount("nowhere");
        CHECK(g_sdk_asked == 1 && !g_sdk_unmounts, "a name in neither table is a no-op (the SDK's Unmount would abort on it)");
        h_fs_Unmount(NULL);
        CHECK(g_sdk_asked == 1 && !g_sdk_unmounts, "a null name reaches no SDK call");
        h_fs_Unmount("cache");
        CHECK(g_sdk_unmounts == 1 && !g_sdk_cache_mounted, "an SDK-owned mount is released in the SDK");
        h_fs_Unmount("cache");
        CHECK(g_sdk_unmounts == 1, "unmounting it again does not reach the SDK's Unmount");
        g_sdk_cache_mounted = 1; g_sdk_no_is_mounted = 1; h_fs_Unmount("cache");
        CHECK(g_sdk_unmounts == 1 && g_sdk_cache_mounted, "an SDK without an IsMounted symbol: no blind Unmount");
        t_guest_sym = NULL;
    }

    printf("⑩ paths and handles on a mount in the SDK's own table (a RAM disk) go to the SDK\n");
    {   int32_t et = -1; uint64_t h = 0; size_t n = 0; int64_t sz = 0, cnt = 0; DirectoryEntry de[4]; char b[16] = {0}; int calls;
        CHECK(h_fs_GetEntryType(&et, "vtapi:/VoiceText") == RES_PATH_NOT_FOUND, "with no SDK in the process, a name the host does not hold is not found");
        t_guest_sym = vt_sym;
        CHECK(h_fs_IsPathMounted("vtapi:/VoiceText") && h_fs_IsPathMounted("save:/a") && !h_fs_IsPathMounted("nowhere:/x"), "IsPathMounted answers from the host's table and the SDK's");
        calls = g_vt_calls;
        CHECK(h_fs_GetEntryType(&et, "vtapi:/VoiceText") == RES_OK && et == ENT_DIR && g_vt_calls == calls + 1, "GetEntryType on the RAM disk is the SDK's");
        CHECK(h_fs_CreateDirectory("vtapi:/VoiceText/vt") == RES_OK && h_fs_CreateFile("vtapi:/VoiceText/vt/new", 4) == RES_OK && g_vt_calls == calls + 3, "CreateDirectory and CreateFile on the RAM disk are the SDK's");
        CHECK(h_fs_OpenFile(&h, "vtapi:/VoiceText/vt/vtpath.ini", MODE_READ | MODE_WRITE) == RES_OK && h == VT_FILE, "OpenFile on the RAM disk returns the SDK's handle as it is");
        CHECK(h_fs_GetFileSize(&sz, h) == RES_OK && sz == 3, "GetFileSize of an SDK handle is the SDK's");
        CHECK(h_fs_ReadFile(&n, h, 0, b, sizeof b) == RES_OK && n == 3 && !memcmp(b, "ini", 3), "ReadFile of an SDK handle reads the SDK's file");
        memset(b, 0, sizeof b);
        CHECK(h_fs_ReadFileExact(h, 1, b, 2) == RES_OK && !memcmp(b, "ni", 2), "the ReadFile overload without a byte count reads through it too");
        CHECK(h_fs_WriteFile(h, 0, "INI", 3, NULL) == RES_OK && h_fs_SetFileSize(h, 3) == RES_OK && h_fs_FlushFile(h) == RES_OK && h_fs_GetFileOpenMode(h) == (MODE_READ | MODE_WRITE), "WriteFile, SetFileSize, FlushFile and GetFileOpenMode of an SDK handle are the SDK's");
        calls = g_vt_calls; h_fs_CloseFile(h);
        CHECK(g_vt_closed == 1 && g_vt_calls == calls + 1, "CloseFile of an SDK handle closes it in the SDK");
        calls = g_vt_calls;
        CHECK(!sdk_handle_known(VT_FILE) && h_fs_GetFileSize(&sz, h) != RES_OK && g_vt_calls == calls, "a closed SDK handle is forgotten: the next call does not reach the SDK");
        CHECK(h_fs_OpenDirectory(&h, "vtapi:/VoiceText", DIRMODE_DIR | DIRMODE_FILE) == RES_OK && h == VT_DIR, "OpenDirectory on the RAM disk returns the SDK's handle");
        CHECK(h_fs_DirCount(&cnt, h) == RES_OK && cnt == 1 && h_fs_ReadDirectory(&cnt, de, h, 4) == RES_OK && cnt == 1 && !strcmp(de[0].name, "vt"), "GetDirectoryEntryCount and ReadDirectory of an SDK handle list the SDK's directory");
        calls = g_vt_calls; h_fs_CloseDirectory(h);
        CHECK(g_vt_dirs_closed == 1 && g_vt_calls == calls + 1 && !sdk_handle_known(VT_DIR), "CloseDirectory closes it in the SDK and forgets it");
        calls = g_vt_calls;
        CHECK(h_fs_RenameFile("vtapi:/VoiceText/vt/new", "vtapi:/VoiceText/vt/old") == RES_OK && h_fs_DeleteFile("vtapi:/VoiceText/vt/old") == RES_OK
              && h_fs_DeleteDirRec("vtapi:/VoiceText/vt") == RES_OK && h_fs_DeleteDir("vtapi:/VoiceText") == RES_OK && g_vt_calls == calls + 4,
              "RenameFile, DeleteFile and the directory deletes on the RAM disk are the SDK's");
        calls = g_vt_calls;
        CHECK(h_fs_RenameFile("vtapi:/VoiceText/vt/vtpath.ini", "save:/vtpath.ini") == RES_RENAME_OTHER_FS && g_vt_calls == calls, "a rename between the SDK's mount and the host's is refused as two mounts and reaches neither");
        calls = g_vt_calls; g_vt_mount_asks = 0;
        CHECK(h_fs_CreateFile("save:/vt", 0) == RES_OK && write_save("save:/vt", "x") == RES_OK && read_fs("save:/vt", b, sizeof b) && h_fs_GetEntryType(&et, "save:/vt") == RES_OK && g_vt_calls == calls && !g_vt_mount_asks,
              "the host's own mounts never ask the SDK");
        CHECK(h_fs_GetEntryType(&et, "nowhere:/x") == RES_PATH_NOT_FOUND && g_vt_calls == calls, "a name in neither table is still not found");
        t_guest_sym = NULL;
    }

    t_rmtree(root);
    printf("== %d checks, %d failed ==\n", checks, fails);
    return fails != 0;
}
