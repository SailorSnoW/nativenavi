// DM_HLECOUNT=<file, %d = pid>: counts guest calls of each host entry (TSV at exit).
// The stubs touch only x16 / x17 (call-clobbered).
#include "dm.h"
#include <stdatomic.h>
#include <sys/mman.h>
#include <unistd.h>
#include <libkern/OSCacheControl.h>
#define HC_MAX 8192
static struct { const char *kind, *name; void *fn; _Atomic uint64_t n; } g_hc[HC_MAX];
static unsigned g_nhc;
static uint32_t *g_hcode;
static int g_hon = -1, g_hsealed;
static const char *g_hpath;
static _Atomic uint64_t g_hsvc[256];
static int hc_on(void) {
    if (g_hon < 0) {
        g_hpath = getenv("DM_HLECOUNT");
        g_hon = g_hpath != NULL;
    }
    return g_hon;
}
static void hc_write(void) {
    char pth[1024];
    snprintf(pth, sizeof pth, g_hpath, (int)getpid());
    FILE *f = fopen(pth, "w");
    if (!f) {
        LOG("[RED] DM_HLECOUNT cannot write %s", pth);
        return;
    }
    fprintf(f, "kind\tname\tcount\n");
    for (unsigned i = 0; i < g_nhc; i++) fprintf(f, "%s\t%s\t%llu\n", g_hc[i].kind, g_hc[i].name, (unsigned long long)atomic_load(&g_hc[i].n));
    for (int k = 0; k < 256; k++)
        if (atomic_load(&g_hsvc[k])) fprintf(f, "SVC\t%#x\t%llu\n", k, (unsigned long long)atomic_load(&g_hsvc[k]));
    fclose(f);
    LOG("DM_HLECOUNT: %u entries → %s", g_nhc, pth);
}
void *hlecount_wrap(const char *kind, const char *name, void *fn) {
    if (!hc_on() || !fn) return fn;
    if (g_hsealed) {
        static int once;
        if (!once++) LOG("[RED] DM_HLECOUNT: resolution after the pages were sealed (%s), later entries are not counted", name);
        return fn;
    }
    for (unsigned i = 0; i < g_nhc; i++)
        if (g_hc[i].fn == fn && !strcmp(g_hc[i].name, name)) return g_hcode + 12 * i;
    if (g_nhc == HC_MAX) die("DM_HLECOUNT more than %d entries", HC_MAX);
    if (!g_hcode) {
        g_hcode = mmap(NULL, HC_MAX * 48, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (g_hcode == MAP_FAILED) die("DM_HLECOUNT mmap");
        atexit(hc_write);
    }
    unsigned i = g_nhc++;
    g_hc[i].kind = kind;
    g_hc[i].name = name;
    g_hc[i].fn = fn;
    uint32_t *w = g_hcode + 12 * i;                                  // 6 instructions, literals at 0x18 / 0x20
    w[0] = 0x58000000 | ((0x18 / 4) << 5) | 16;                      // ldr x16, &count
    w[1] = 0xD2800031;                                                // movz x17, #1
    w[2] = 0xF8200000 | (17u << 16) | (16u << 5) | 31;               // stadd x17, [x16]
    w[3] = 0x58000000 | (((0x20 - 0xc) / 4) << 5) | 16;              // ldr x16, fn
    w[4] = 0xD61F0200;                                                // br x16
    w[5] = 0xD503201F;
    uint64_t a = (uint64_t)&g_hc[i].n, b = (uint64_t)fn;
    memcpy(w + 6, &a, 8);
    memcpy(w + 8, &b, 8);
    return w;
}
void hlecount_seal(void) {
    if (!hc_on() || !g_hcode) return;
    if (mprotect(g_hcode, HC_MAX * 48, PROT_READ | PROT_EXEC)) die("DM_HLECOUNT mprotect");
    g_hsealed = 1;
    sys_icache_invalidate(g_hcode, HC_MAX * 48);
    LOG("DM_HLECOUNT: %u counting stubs sealed", g_nhc);
}
void hlecount_svc(int num) { if (g_hon > 0 && num >= 0 && num < 256) atomic_fetch_add(&g_hsvc[num], 1); }
void hlecount_flush(void) { if (hc_on() && g_hcode) hc_write(); }   // _exit runs no atexit
