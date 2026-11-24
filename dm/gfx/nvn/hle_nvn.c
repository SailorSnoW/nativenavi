// NVN replaced wholesale: nvnBootstrapLoader is taken over and the SDK's NVN driver never runs. Entries with CPU-visible results work on real objects here;
// GPU state, bindings and draws are recorded into the game's command memory and replayed at submit (nvn_mtl.m); the rest report through nvn_unimpl_entry.
// GPU address = CPU address.
#include "../../host/hle.h"
#include "nvn_protos.h"
#include "nvn_count.h"
#include "nvn_tex.h"
#include "../tex/nvn_deswizzle.h"
#include "nvn_mtl.h"
#include "nvn_pace.h"
#include "nvn_fstep.h"
#include "nvn_rt.h"
#include "nvn_timestamp.h"
#include "../../host/front.h"
#include "nvn_dirty.h"
#include <mach/mach_time.h>
#include <CommonCrypto/CommonDigest.h>
#include <CoreFoundation/CoreFoundation.h>
#include <mach/mach.h>
#include <unistd.h>
#include <sys/mman.h>
#include <errno.h>
#define DM_MEM_CAP_DEFAULT_GB 24
#include <pthread/qos.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <mach/thread_policy.h>
#include <assert.h>
#include <stdatomic.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>

typedef struct { _Atomic uint64_t cnt; void *fn; } nvn_slot_t;
nvn_slot_t nvn_slot[NVN_NFUNC];                 // the thunk indexes it by i*16: the layout must not change
_Static_assert(sizeof(nvn_slot_t) == 16, "the trampolines are addressed in 16-byte steps");
extern void *nvn_thunk[NVN_NFUNC], *nvn_thunk_fast[NVN_NFUNC];
// counted only when asked: each count is an atomic add on a cache line every recording thread shares
static int nvn_counting(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_COUNT"), *c = getenv("DM_NVN_TSV_CALLS"), *t = getenv("DM_NVN_TSV");
        v = (e && atoi(e)) || (t && *t && !(c && !atoi(c)));
    }
    return v;
}
extern const char *nvn_names[NVN_NFUNC];
extern void nvn_unimpl_entry(void);
extern void nvn_prof_entry(void);
extern void nvn_big_entry(void);
void *nvn_big_real[NVN_NFUNC];            // functions that run on the big host stack (nvn_entry.S)
void *nvn_real[NVN_NFUNC];               // DM_NVN_PROF: called by the timing wrapper
_Atomic uint64_t nvn_prof_ticks[NVN_NFUNC]; // DM_NVN_PROF: cntvct counts
static uint8_t g_unimpl_seen[NVN_NFUNC];

uint64_t nvn_unimpl_c(nvn_slot_t *slot, uint64_t x0, uint64_t x1, uint64_t x2) {
    unsigned i = (unsigned)(slot - nvn_slot);
    if (!g_unimpl_seen[i]) {   // a flag: a byte counter would wrap at 256 and print the once line again
        g_unimpl_seen[i] = 1;
        LOG("[RED] NVN unimplemented function called: %s (x0=%#llx x1=%#llx x2=%#llx)", nvn_names[i], x0, x1, x2);
    }
    return 0;
}

typedef struct { const char *name; void *fn; } nvn_impl;
#define NVN(name, ret, ...)                                                                       \
    static ret h_##name(__VA_ARGS__);                                                             \
    static const NVNT_##name chk_##name __attribute__((unused)) = h_##name;                       \
    DM_REGISTER("swnvn", nvn_impl, e_##name, {#name, (void *)h_##name}); \
    static ret h_##name(__VA_ARGS__)
// no prototype: no signature check
#define NVN_NOPROTO(name, ret, ...)                                                               \
    static ret h_##name(__VA_ARGS__);                                                             \
    DM_REGISTER("swnvn", nvn_impl, e_##name, {#name, (void *)h_##name}); \
    static ret h_##name(__VA_ARGS__)

static int g_log;
static pthread_mutex_t g_sub_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_serial;
uint64_t nvn_new_serial(void) { return ++g_serial; }
void nvn_serial_floor(uint64_t s) { uint64_t c = atomic_load(&g_serial); while (c < s && !atomic_compare_exchange_weak(&g_serial, &c, s)) {} }
static unsigned g_nsem;
#define OLOG(...) do { if (g_log) LOG("[nvn] " __VA_ARGS__); } while (0)
// The game's objects holding host state, by address, from Initialize to Finalize: a reset (dm_gfx_reset) finalizes those left
enum { LV_POOL, LV_TEX, LV_PROG, LV_SYNC, LV_QUEUE, LV_WIN, LV_N };
static CFMutableSetRef g_live[LV_N];
static pthread_mutex_t g_live_mu = PTHREAD_MUTEX_INITIALIZER;
static void live_add(int k, const void *o) {
    pthread_mutex_lock(&g_live_mu);
    if (!g_live[k]) g_live[k] = CFSetCreateMutable(NULL, 0, NULL);
    CFSetAddValue(g_live[k], o);
    pthread_mutex_unlock(&g_live_mu);
}
static void live_del(int k, const void *o) {
    pthread_mutex_lock(&g_live_mu);
    if (g_live[k]) CFSetRemoveValue(g_live[k], o);
    pthread_mutex_unlock(&g_live_mu);
}
static _Atomic uint64_t g_boot;   // the game's starts in this process: per-thread guest address caches on a thread that outlives a reset check it
// presents before the game's current start: Metal numbers presents across the process, a window from its own first
static uint64_t g_present_base;

static void nvn_init_slots(void) {
    static int done;
    if (done) return;
    done = 1;
    g_log = getenv("DM_NVN_LOG") != NULL;
    unsigned long sz;
    const nvn_impl *im = (const nvn_impl *)getsectiondata(&_mh_execute_header, "__DATA", "swnvn", &sz);
    unsigned nim = im ? (unsigned)(sz / sizeof *im) : 0, hit = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++) nvn_slot[i].fn = (void *)nvn_unimpl_entry;
    for (unsigned k = 0; k < nim; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], im[k].name)) i++;
        if (i == NVN_NFUNC) die("NVN implementation %s is not in main's name table", im[k].name);
        nvn_slot[i].fn = im[k].fn;
        hit++;
    }
    g_nsem = hit;
}

static void *nvn_lookup(const char *name) {
    nvn_init_slots();
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (!strcmp(nvn_names[i], name)) return nvn_counting() ? nvn_thunk[i] : nvn_thunk_fast[i];
    LOG("[RED] NVN address lookup of an unknown name: %s", name ? name : "(null)");
    return NULL;
}

HLE_FORCE("nvnBootstrapLoader", "the NVN graphics API is replaced wholesale with a host null device (Metal behind it); the SDK's NVN driver does not run",
          void *, h_nvnBootstrapLoader, const char *name) { return nvn_lookup(name); }

// Switch callers do not extend booleans: only the low 8 bits count
#define B(x) ((x) & 0xff)

#include "nvn_obj.h"
#include "nvn_queue.h"
#include "nvn_capture.h"

NVN(nvnDeviceBuilderSetDefaults, void, NVNdeviceBuilder *b) { memset(b, 0, sizeof(dev_builder_t)); }
NVN(nvnDeviceBuilderSetFlags, void, NVNdeviceBuilder *b, int f) { ((dev_builder_t *)b)->flags = f; }
NVN(nvnDeviceBuilderGetFlags, int, const NVNdeviceBuilder *b) { return (int)((const dev_builder_t *)b)->flags; }
NVN(nvnDeviceInitialize, NVNboolean, NVNdevice *d, const NVNdeviceBuilder *b) {
    dev_t_ *o = (dev_t_ *)d;
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('D', 'E', 'V', 0);
    o->flags = ((const dev_builder_t *)b)->flags;
    o->origin = NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT;   // NVN's default
    mtl_set_origin_lower_left(1);
    mtl_set_depth_zero(0);   // NVN's default: near = -w (GL)
    OLOG("DeviceInitialize %p flags=%#x", (void *)d, o->flags);
    return 1;
}
NVN(nvnDeviceFinalize, void, NVNdevice *d) { ((dev_t_ *)d)->magic = 0; }
NVN(nvnDeviceGetProcAddress, PFNNVNGENERICFUNCPTRPROC, const NVNdevice *d, const char *name) {
    (void)d; return (PFNNVNGENERICFUNCPTRPROC)nvn_lookup(name);
}

// Device properties: NVN2 answers most items from constants (g_devinfo2); items it reads from the GPU at run time take the X1's values
// (g_devinfo, nnheaders nvn_DeviceConstantsNX.h). Items 0 and 1, the API version, are nvn_api's.
enum { DEVINFO_RT = -1 };   // an item NVN2 reads from the GPU at run time
static const int g_devinfo[] = {
    [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16, [0x07] = 4, [0x08] = 16, [0x09] = 32,
    [0x0A] = 16, [0x0B] = 4, [0x0C] = 4, [0x0D] = 4, [0x0E] = 16, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256,
    [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = 2, [0x18] = 256, [0x19] = 4096, [0x1A] = 131072,
    [0x1B] = 16, [0x1C] = 1536, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1536, [0x20] = 65535, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1,
    [0x2B] = 262144, [0x2C] = 15, [0x2D] = 2048, [0x2E] = 1, [0x2F] = 1, [0x30] = 14, [0x31] = 5, [0x32] = 1, [0x33] = 8,
    [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = 128, [0x38] = 32, [0x39] = 128, [0x3A] = 65536, [0x3B] = 1, [0x3C] = 4,
    [0x3D] = 16384, [0x3E] = 134217728, [0x3F] = 2048, [0x40] = 16384, [0x41] = 16384, [0x42] = 1, [0x43] = 1, [0x44] = 1,
    [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16,
    [0x4E] = 1024, [0x4F] = 32, [0x50] = 4096, [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384,
    [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 32, [0x60] = 65535, [0x61] = 1, [0x62] = 32, [0x63] = 256, [0x64] = 16,
    [0x65] = 65535, [0x66] = 65535, [0x67] = 256, [0x68] = 16383,
};
// NVN2's constants (versions read at run time: dm_sdk_nvn2_item); DEVINFO_RT items take the X1 table's value, else 0 with a red line.
// 32 vertex bindings are answered; the host keeps 16, which all its tables hold.
static const int g_devinfo2[] = {
    [0x00] = 231, [0x01] = 39, [0x02] = 14, [0x03] = 65536, [0x04] = 256, [0x05] = 8, [0x06] = 16 /* NVN2 32 */, [0x07] = 4, [0x08] = 16, [0x09] = 32, [0x0A] = 16, [0x0B] = 4,
    [0x0C] = 4, [0x0D] = 4, [0x0E] = 32, [0x0F] = 32, [0x10] = 32, [0x11] = 256, [0x12] = 256, [0x13] = 4, [0x14] = 8, [0x15] = 512, [0x16] = 256, [0x17] = DEVINFO_RT,
    [0x18] = DEVINFO_RT, [0x19] = 4096, [0x1A] = 16384, [0x1B] = 16, [0x1C] = 1024, [0x1D] = 1024, [0x1E] = 64, [0x1F] = 1024, [0x20] = 2147483647, [0x21] = 65535, [0x22] = 65535,
    [0x23] = 8, [0x24] = 1048576, [0x25] = 4096, [0x26] = 16, [0x27] = 4096, [0x28] = 1, [0x29] = 1, [0x2A] = 1, [0x2B] = DEVINFO_RT, [0x2C] = 16, [0x2D] = DEVINFO_RT,
    [0x2E] = 201, [0x2F] = 201, [0x30] = 20, [0x31] = 0, [0x32] = 1, [0x33] = 8, [0x34] = 8, [0x35] = 4, [0x36] = 32, [0x37] = DEVINFO_RT, [0x38] = DEVINFO_RT, [0x39] = 128,
    [0x3A] = DEVINFO_RT, [0x3B] = 1, [0x3C] = 4, [0x3D] = DEVINFO_RT, [0x3E] = DEVINFO_RT, [0x3F] = DEVINFO_RT, [0x40] = DEVINFO_RT, [0x41] = DEVINFO_RT, [0x42] = 1, [0x43] = 1,
    [0x44] = 1, [0x45] = 1, [0x46] = 5, [0x47] = 1, [0x48] = 1, [0x49] = 1, [0x4A] = 1, [0x4B] = 1, [0x4C] = 1, [0x4D] = 16, [0x4E] = 2048, [0x4F] = 32, [0x50] = 4096,
    [0x51] = 65536, [0x52] = 65536, [0x53] = 256, [0x54] = 16384, [0x55] = 262144, [0x56] = 4096, [0x57] = 1, [0x58] = 4, [0x59] = 2, [0x5A] = 0, [0x5B] = 4096, [0x5C] = 16384,
    [0x5D] = 4096, [0x5E] = 128, [0x5F] = 32, [0x60] = 65535, [0x61] = 1, [0x62] = DEVINFO_RT, [0x63] = 256, [0x64] = 16, [0x65] = 65535, [0x66] = 65535, [0x67] = 256,
    [0x68] = 1, [0x69] = 16, [0x6A] = 16, [0x6B] = 16, [0x6C] = 1, [0x6D] = 1, [0x6E] = 1, [0x6F] = 256, [0x70] = 0, [0x71] = 16, [0x72] = 8, [0x73] = 16, [0x74] = 32,
    [0x75] = 31, [0x76] = 536870912, [0x77] = 16777216, [0x78] = 16777216, [0x79] = 1073741824, [0x7A] = 32, [0x7B] = 1, [0x7C] = 64, [0x7D] = DEVINFO_RT, [0x7E] = 1,
    [0x7F] = 2147483647, [0x80] = 32, [0x81] = 32, [0x82] = 4194303, [0x83] = 512, [0x84] = 256, [0x85] = 32, [0x86] = 16383, [0x87] = DEVINFO_RT, [0x88] = 8, [0x89] = 0,
    [0x8A] = 0, [0x8B] = 4, [0x8C] = 256, [0x8D] = 16, [0x8E] = 0, [0x8F] = 1, [0x90] = 31, [0x91] = 0, [0x92] = 64, [0x93] = 1, [0x94] = 58368, [0x95] = 24576, [0x96] = 0,
    [0x97] = 256, [0x98] = 262144, [0x99] = DEVINFO_RT, [0x9A] = DEVINFO_RT, [0x9B] = 0, [0x9C] = 201, [0x9D] = 14, [0x9E] = 201, [0x9F] = 20, [0xA0] = 163840, [0xA1] = 8,
    [0xA2] = 4, [0xA3] = 256, [0xA4] = 256, [0xA5] = 4, [0xA6] = 65536, [0xA7] = 201, [0xA8] = 5, [0xA9] = 1,
};
__attribute__((weak)) int dm_sdk_major(void) { return 0; }   // graphics unit tests have no loader
__attribute__((weak)) int dm_sdk_nvn2_item(unsigned item, int *out) { (void)item; (void)out; return 0; }
static void nvn_api(int api[2]) {
    // NVN2 is API 231 and its minor moves with the SDK; games create their graphics system only when both match
    api[0] = 231;
    if (!dm_sdk_nvn2_item(1, &api[1])) api[1] = g_devinfo2[1];
    LOG("[nvn] device API %d.%d (SDK %d)", api[0], api[1], dm_sdk_major());
}
NVN(nvnDeviceGetInteger, void, const NVNdevice *d, NVNdeviceInfo what, int *out) {
    (void)d;
    unsigned w = (unsigned)what;
    // 0x67, shader code alignment in a pool: the host reads shaders by key, so it only has to be valid
    const unsigned n1 = sizeof g_devinfo / sizeof *g_devinfo, n2 = sizeof g_devinfo2 / sizeof *g_devinfo2;
    if (w > 1 && w < n2 && g_devinfo2[w] != DEVINFO_RT) {   // the sdk's own constant: GPU code versions move with the SDK
        *out = g_devinfo2[w]; dm_sdk_nvn2_item(w, out); OLOG("DeviceGetInteger %#x -> %d (NVN2)", w, *out); return;
    }
    if (w >= n1) {   // past the X1's items: read from the GPU at run time, or unknown
        int rt = w < n2 && g_devinfo2[w] == DEVINFO_RT;
        LOG("[RED] nvnDeviceGetInteger item %#x: %s; returning 0", w, rt ? "NVN2 reads it from the GPU at run time, not known here" : "unknown item");
        *out = 0; return;
    }
    *out = g_devinfo[w];
    if (w <= 1) { static int api[2] = {-1, -1}; if (api[0] < 0) { nvn_api(api); const char *e = getenv("DM_NVN_API"); if (e) sscanf(e, "%d.%d", &api[0], &api[1]); } *out = api[w]; }
    OLOG("DeviceGetInteger %#x -> %d", w, *out);
}
NVN(nvnDeviceGetImageHandle, NVNimageHandle, const NVNdevice *d, int id) { (void)d; return (NVNimageHandle)id; }
// Inferred from the shaders (cx[URb][offset] is the buffer's address): no caller seen, so the first call's arguments are logged (open)
NVN_NOPROTO(nvnDeviceGetUniformBufferHandle, uint64_t, const NVNdevice *d, NVNbufferAddress addr, size_t size) {
    (void)d;
    static int once; if (!once++) LOG("[nvn] DeviceGetUniformBufferHandle address %#llx size %#zx (first call)", (unsigned long long)addr, size);
    return addr;
}
// the descriptor index; the sampler's in bit 20 and up, as in combined handles
NVN(nvnDeviceGetSeparateTextureHandle, NVNseparateTextureHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateTextureHandle){(uint64_t)id}; }
NVN(nvnDeviceGetSeparateSamplerHandle, NVNseparateSamplerHandle, const NVNdevice *d, int id) { (void)d; return (NVNseparateSamplerHandle){(uint64_t)id << 20}; }
NVN(nvnDeviceInstallDebugCallback, void, NVNdevice *d, const PFNNVNDEBUGCALLBACKPROC cb, void *data, uint32_t enable) {
    dev_t_ *o = (dev_t_ *)d; o->dbg_cb = B(enable) ? (void *)cb : NULL; o->dbg_data = data;
}
NVN(nvnDeviceSetWindowOriginMode, void, NVNdevice *d, NVNwindowOriginMode m) { ((dev_t_ *)d)->origin = m; mtl_set_origin_lower_left(m == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT); LOG("[nvn] window origin %s", m ? "upper left (as D3D / Metal)" : "lower left (GL)"); }
NVN(nvnDeviceSetDepthMode, void, NVNdevice *d, NVNdepthMode m) { ((dev_t_ *)d)->depth_mode = m; mtl_set_depth_zero(m != 0); LOG("[nvn] depth mode %s", m ? "near=0 (as D3D / Metal)" : "near=-w (GL)"); }
NVN(nvnDeviceGetWindowOriginMode, NVNwindowOriginMode, const NVNdevice *d) { return ((const dev_t_ *)d)->origin; }
NVN(nvnDeviceGetDepthMode, NVNdepthMode, const NVNdevice *d) { return ((const dev_t_ *)d)->depth_mode; }
NVN(nvnDeviceFinalizeCommandHandle, void, NVNdevice *d, NVNcommandHandle h) { (void)d; (void)h; }
// only the (empty) debug groups take it: any distinct nonzero number serves
NVN(nvnDeviceGenerateDebugDomainId, NVNdebugDomainId, const NVNdevice *d, const char *name) { static atomic_int next; (void)d; (void)name; return atomic_fetch_add(&next, 1) + 1; }

NVN(nvnMemoryPoolBuilderSetDefaults, void, NVNmemoryPoolBuilder *b) {
    // SetDefaults keeps SetDevice; NVN's default is CPU_NO_ACCESS | GPU_CACHED
    pool_builder_t *o = (pool_builder_t *)b; o->mem = NULL; o->size = 0;
    o->flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED;
}
NVN(nvnMemoryPoolBuilderSetDevice, void, NVNmemoryPoolBuilder *b, NVNdevice *d) { ((pool_builder_t *)b)->dev = d; }
NVN(nvnMemoryPoolBuilderSetStorage, void, NVNmemoryPoolBuilder *b, void *m, size_t n) { ((pool_builder_t *)b)->mem = m; ((pool_builder_t *)b)->size = n; }
NVN(nvnMemoryPoolBuilderSetFlags, void, NVNmemoryPoolBuilder *b, int f) { ((pool_builder_t *)b)->flags = f; }
NVN(nvnMemoryPoolBuilderGetDevice, NVNdevice *, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_t *)b)->dev; }
NVN(nvnMemoryPoolBuilderGetMemory, void *, const NVNmemoryPoolBuilder *b) { return ((const pool_builder_t *)b)->mem; }
NVN(nvnMemoryPoolBuilderGetSize, size_t, const NVNmemoryPoolBuilder *b) { return (size_t)((const pool_builder_t *)b)->size; }
NVN(nvnMemoryPoolBuilderGetFlags, NVNmemoryPoolFlags, const NVNmemoryPoolBuilder *b) { return (NVNmemoryPoolFlags)((const pool_builder_t *)b)->flags; }
static uint8_t *pool_cpu_map(const pool_t *p) {
    // GPU-only and virtual pools have a host address but no CPU mapping in NVN
    return (p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_VIRTUAL)) ? NULL : p->mem;
}
static uint8_t *vpool_reserve(pool_t *o); static void vpool_release(pool_t *o); static void vpool_taken(const pool_t *o);
NVN(nvnMemoryPoolInitialize, NVNboolean, NVNmemoryPool *p, const NVNmemoryPoolBuilder *b) {
    const pool_builder_t *s = (const pool_builder_t *)b; pool_t *o = (pool_t *)p;
    o->magic = MAGIC('P', 'O', 'O', 'L'); o->flags = s->flags; o->mem = s->mem; o->size = s->size; o->vp = NULL; o->vmode = 0; o->asked = 0;
    if (!o->mem && (o->flags & 0x200)) { uint8_t *vr = vpool_reserve(o); if (vr) o->mem = vr; }   // a virtual pool's own address range until its first mapping decides
    else if (o->mem) { mtl_pool_add(o->mem, o->size); nvn_dirty_add_range(o->mem, o->size); }
    if (o->mem && !(o->flags & 0x200)) dm_device_share(o->mem, o->size, 1);   // the GPU maps the storage: the host refuses to unmap it until Finalize, as Horizon does
    if (!o->mem && !(o->flags & 0x200)) LOG("[RED] MemoryPoolInitialize: non-virtual pool has no storage flags=%#x size=%#llx", o->flags, o->size);
    if (getenv("DM_NVN_VLOG")) { LOG("[nvn] MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, (unsigned long long)o->size, o->flags); void dm_backtrace(uint64_t fp); dm_backtrace((uint64_t)__builtin_frame_address(0)); }
    OLOG("MemoryPool %p mem=%p size=%#llx flags=%#x", (void *)p, (void *)o->mem, o->size, o->flags);
    live_add(LV_POOL, p);
    return 1;
}
static void fl_add(const uint8_t *a, uint64_t n); static void fl_drop(const uint8_t *a, uint64_t n);
NVN(nvnMemoryPoolFinalize, void, NVNmemoryPool *p) {
    // dead to the replay first: a texture resolving on the submit thread must not find the page table or storage gone
    live_del(LV_POOL, p); mtl_alias_drop_pool(p); ((pool_t *)p)->magic = 0;
    if (((pool_t *)p)->flags & 0x200 && ((pool_t *)p)->vp) vpool_release((pool_t *)p);
    if (((pool_t *)p)->mem && !(((pool_t *)p)->flags & 0x200)) {   // a virtual pool's storage is borrowed from a physical pool
 nvn_dirty_remove_range(((pool_t *)p)->mem); mtl_pool_del(((pool_t *)p)->mem); mtl_sp_phys_gone(((pool_t *)p)->mem, ((pool_t *)p)->size); fl_drop(((pool_t *)p)->mem, ((pool_t *)p)->size);
 dm_device_share(((pool_t *)p)->mem, ((pool_t *)p)->size, 0); } }
static void ft_flush(const uint8_t *a, uint64_t n);
NVN(nvnMemoryPoolFlushMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    const pool_t *o = (const pool_t *)p;
    if (!o->mem) return;
    if (!(o->flags & 0x200)) fl_add(o->mem + off, n);
    ft_flush(o->mem + off, n);
}
// A GPU address exists only once the driver gives one (a buffer, nvnTextureGetTextureAddress); raw global memory reaches only those (mtl_pool_addressed)
static void pool_mark_addressed(const pool_t *o) { vpool_taken(o); ((pool_t *)o)->asked = 1; static __thread const uint8_t *last; static __thread uint64_t last_boot; uint64_t boot = atomic_load_explicit(&g_boot, memory_order_relaxed);
    if (o->mem && (o->mem != last || last_boot != boot)) { last = o->mem; last_boot = boot; mtl_pool_addressed(o->mem, o->size); } }
// A shader-code pool's address only places programs: the GPU never reads that code (shaders run translated), so the pool stays out of the raw pool table
// (docs/reference/memory.md, Shader-code pools made resident by the raw pool table). A buffer in the pool still puts it in.
NVN(nvnMemoryPoolGetBufferAddress, NVNbufferAddress, const NVNmemoryPool *p) {
    const pool_t *o = (const pool_t *)p;
    if ((o->flags & (NVN_MEMORY_POOL_FLAGS_VIRTUAL | NVN_MEMORY_POOL_FLAGS_SHADER_CODE)) != NVN_MEMORY_POOL_FLAGS_SHADER_CODE) pool_mark_addressed(o);
    return (NVNbufferAddress)o->mem;
}

// NVN numbers IDs per descriptor pool; the replay keeps one table per kind, so each pool gets a base there, in creation order.
// The capacity is the end of the last numbering, not the last pool's size: an earlier, larger pool's IDs would read as empty
enum { NDPOOL = 16 };
static struct dpool_base { const void *pool; int base; } g_tpb[NDPOOL], g_spb[NDPOOL];
static int g_ntpb, g_nspb, g_tnext, g_snext, g_tend, g_send;   // next: the next pool's base (256-aligned); end: one past any pool's last ID
static pthread_mutex_t g_dpool_mu = PTHREAD_MUTEX_INITIALIZER;
static int dpool_add(struct dpool_base *t, int *nt, int *next, int *end, const void *pool, int n, const char *kind) {
    pthread_mutex_lock(&g_dpool_mu);
    int i = 0; while (i < *nt && t[i].pool != pool) i++;
    if (i == *nt) {
        if (*nt == NDPOOL) { pthread_mutex_unlock(&g_dpool_mu); LOG("[RED] %s pools exceed %d: later ones share the first pool's numbering", kind, NDPOOL); return 0; }
        t[i] = (struct dpool_base){pool, *next}; (*nt)++; *next += (n + 255) & ~255;
        if (i) LOG("[nvn] pool #%d (%s, %d entries): numbered from %d in the host table, separate from the earlier pools", i + 1, kind, n, t[i].base);
    }
    int b = t[i].base; if (b + n > *end) *end = b + n;
    pthread_mutex_unlock(&g_dpool_mu); return b;
}
static int dpool_base(const struct dpool_base *t, int nt, const void *pool) { for (int i = 0; i < nt; i++) if (t[i].pool == pool) return t[i].base; return 0; }
int nvn_dpool_base(const void *pool, int tex) { return tex ? dpool_base(g_tpb, g_ntpb, pool) : dpool_base(g_spb, g_nspb, pool); }
// pool selection is queue state at execution, inherited by command lists that bind none
NVN(nvnTexturePoolInitialize, NVNboolean, NVNtexturePool *tp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)tp = (dpool_t){(const pool_t *)p, off, n}; OLOG("TexturePool n=%d", n);
    dpool_add(g_tpb, &g_ntpb, &g_tnext, &g_tend, tp, n, "texture"); mtl_set_tex_pool_n(g_tend);
    return 1;
}
NVN(nvnTexturePoolFinalize, void, NVNtexturePool *tp) { (void)tp; }
NVN(nvnTexturePoolRegisterTexture, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    mtl_reg_tex(dpool_base(g_tpb, g_ntpb, tp) + id, (const tex_t *)t, (const view_t *)v);
}
// image handles index the texture pool's table
NVN(nvnTexturePoolRegisterImage, void, const NVNtexturePool *tp, int id, const NVNtexture *t, const NVNtextureView *v) {
    mtl_reg_image(dpool_base(g_tpb, g_ntpb, tp) + id, (const tex_t *)t, (const view_t *)v);
}
NVN(nvnSamplerPoolInitialize, NVNboolean, NVNsamplerPool *sp, const NVNmemoryPool *p, ptrdiff_t off, int n) {
    *(dpool_t *)sp = (dpool_t){(const pool_t *)p, off, n}; OLOG("SamplerPool n=%d", n); dpool_add(g_spb, &g_nspb, &g_snext, &g_send, sp, n, "sampler"); mtl_set_samp_pool_n(g_send); return 1;
}
NVN(nvnSamplerPoolFinalize, void, NVNsamplerPool *sp) { (void)sp; }
NVN(nvnSamplerPoolRegisterSampler, void, const NVNsamplerPool *sp, int id, const NVNsampler *s) { mtl_reg_samp(dpool_base(g_spb, g_nspb, sp) + id, &((const samp_t *)s)->p); }
NVN(nvnSamplerPoolRegisterSamplerBuilder, void, const NVNsamplerPool *sp, int id, const NVNsamplerBuilder *b) { mtl_reg_samp(dpool_base(g_spb, g_nspb, sp) + id, &((const samp_builder_t *)b)->p); }
NVN(nvnTexturePoolGetMemoryPool, const NVNmemoryPool *, const NVNtexturePool *tp) { return (const NVNmemoryPool *)((const dpool_t *)tp)->pool; }
NVN(nvnTexturePoolGetMemoryOffset, ptrdiff_t, const NVNtexturePool *tp) { return (ptrdiff_t)((const dpool_t *)tp)->off; }
NVN(nvnTexturePoolGetSize, int, const NVNtexturePool *tp) { return ((const dpool_t *)tp)->n; }
NVN(nvnSamplerPoolGetMemoryPool, const NVNmemoryPool *, const NVNsamplerPool *sp) { return (const NVNmemoryPool *)((const dpool_t *)sp)->pool; }
NVN(nvnSamplerPoolGetMemoryOffset, ptrdiff_t, const NVNsamplerPool *sp) { return (ptrdiff_t)((const dpool_t *)sp)->off; }
NVN(nvnSamplerPoolGetSize, int, const NVNsamplerPool *sp) { return ((const dpool_t *)sp)->n; }

#define TB(b) ((tex_builder_t *)(b))
NVN(nvnTextureBuilderSetDefaults, void, NVNtextureBuilder *b) {
    NVNdevice *dev = TB(b)->dev; memset(b, 0, sizeof(tex_builder_t)); TB(b)->dev = dev;
    TB(b)->d.target = NVN_TEXTURE_TARGET_2D; TB(b)->d.levels = 1;
}
NVN(nvnTextureBuilderSetDevice, void, NVNtextureBuilder *b, NVNdevice *d) { TB(b)->dev = d; }
NVN(nvnTextureBuilderGetDevice, NVNdevice *, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->dev; }
NVN(nvnTextureBuilderSetFlags, void, NVNtextureBuilder *b, int f) { TB(b)->d.flags = f; }
NVN(nvnTextureBuilderSetTarget, void, NVNtextureBuilder *b, NVNtextureTarget t) { TB(b)->d.target = t; }
NVN(nvnTextureBuilderSetWidth, void, NVNtextureBuilder *b, int v) { TB(b)->d.width = v; }
NVN(nvnTextureBuilderSetHeight, void, NVNtextureBuilder *b, int v) { TB(b)->d.height = v; }
NVN(nvnTextureBuilderSetDepth, void, NVNtextureBuilder *b, int v) { TB(b)->d.depth = v; }
NVN(nvnTextureBuilderSetSize1D, void, NVNtextureBuilder *b, int w) { TB(b)->d.width = w; TB(b)->d.height = TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetLevels, void, NVNtextureBuilder *b, int v) { TB(b)->d.levels = v; }
NVN(nvnTextureBuilderSetFormat, void, NVNtextureBuilder *b, NVNformat f) { TB(b)->d.format = f; }
NVN(nvnTextureBuilderSetSamples, void, NVNtextureBuilder *b, int s) { TB(b)->d.samples = s; }
NVN(nvnTextureBuilderSetStride, void, NVNtextureBuilder *b, ptrdiff_t s) { TB(b)->d.stride = (uint32_t)s; }
NVN(nvnTextureBuilderSetStorage, void, NVNtextureBuilder *b, NVNmemoryPool *p, ptrdiff_t off) { TB(b)->pool = (pool_t *)p; TB(b)->off = off; }
NVN(nvnTextureBuilderSetPackagedTextureLayout, void, NVNtextureBuilder *b, const NVNpackagedTextureLayout *l) {
    TB(b)->has_layout = l != NULL; if (l) memcpy(TB(b)->layout, l->layout, 8);
    TB(b)->d.pkg_layout = l ? (l->layout[0] | (l->layout[1] & 0xf) << 8) + 1 : 0;
}
NVN(nvnTextureBuilderGetFlags, NVNtextureFlags, const NVNtextureBuilder *b) { return (NVNtextureFlags)((const tex_builder_t *)b)->d.flags; }
static uint64_t g_layout_logged;
static void log_layout(const tex_builder_t *b, const nvn_texlayout *L) {
    if (b->has_layout && g_layout_logged++ < 16)
        LOG("[nvn] packaged texture %ux%ux%u levels %u format %#x target %d layout bytes %02x%02x%02x%02x%02x%02x%02x%02x → block height log2=%u width alignment log2=%u size %#llx",
            b->d.width, b->d.height, b->d.depth, b->d.levels, b->d.format, b->d.target, b->layout[0], b->layout[1], b->layout[2],
            b->layout[3], b->layout[4], b->layout[5], b->layout[6], b->layout[7], L->tileH, b->layout[1] & 0xf, L->size);
}
NVN(nvnTextureBuilderGetStorageSize, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L);
    if (!L.bpb) LOG("[RED] TextureBuilderGetStorageSize unknown format %#x", ((const tex_builder_t *)b)->d.format);
    log_layout((const tex_builder_t *)b, &L);
    return L.size;
}
NVN(nvnTextureBuilderGetStorageAlignment, size_t, const NVNtextureBuilder *b) {
    nvn_texlayout L; nvn_tex_layout(&((const tex_builder_t *)b)->d, &L); return L.align;
}
// a game may size the mip tail as storage(all levels) − storage(numTiledLevels levels)
static void sparse_layout(const nvn_texdesc *d, NVNtextureSparseTileLayout *o) {
    uint32_t tw, th, td;
    memset(o, 0, sizeof *o);
    o->numTiledLevels = nvn_tex_sparse_tiles(d, &tw, &th, &td);
    o->tileWidth = (int)tw;
    o->tileHeight = (int)th;
    o->tileDepth = (int)td;
}
NVN(nvnTextureBuilderGetSparseTileLayout, void, const NVNtextureBuilder *b, NVNtextureSparseTileLayout *o) { sparse_layout(&((const tex_builder_t *)b)->d, o); }
#define TX(t) ((tex_t *)(t))
#define CTX(t) ((const tex_t *)(t))
// Textures the CPU fills in place and publishes by FlushMappedRange (profile tex_flush_upload): registered by address range, a flush over one marks it CPU-written.
// Off by default: a flushed range could cover an image a shader writes, whose GPU content the upload would replace.
// edge_h: its bytes' hashes in its edge pages at the stamp (edged: taken)
typedef struct { const uint8_t *mem; uint64_t size; tex_t t; uint64_t stamp; const void *span; uint64_t edge_h[2]; int edged; } flush_tex;
static flush_tex *g_ft; static size_t g_nft, g_cft; static pthread_mutex_t g_ft_mu = PTHREAD_MUTEX_INITIALIZER;
// The tracker marks 16 KB pages: a texture whose only written pages are its edges, its bytes there unchanged, is stamped again, not taken as written
// (docs/reference/nvn-objects.md#writes-beside-a-registered-texture-in-a-shared-host-page)
static void ft_edge_ranges(const uint8_t *mem, uint64_t size, uintptr_t r[2][2], uintptr_t *ia, uintptr_t *ie) {
    uintptr_t pg = (uintptr_t)getpagesize(), a = (uintptr_t)mem, e = a + size;
    *ia = (a + pg - 1) & ~(pg - 1); *ie = e & ~(pg - 1);
    if (*ie <= *ia) { r[0][0] = a; r[0][1] = e; r[1][0] = r[1][1] = 0; *ia = *ie = 0; return; }
    r[0][0] = a; r[0][1] = *ia; r[1][0] = *ie; r[1][1] = e;
}
static uint64_t ft_hash(uintptr_t lo, uintptr_t hi) {
    const uint8_t *p = (const uint8_t *)lo; size_t n = hi - lo, i = 0; uint64_t h = 0x9e3779b97f4a7c15ull ^ n, w;
    for (; i + 8 <= n; i += 8) { memcpy(&w, p + i, 8); h ^= w * 0x87c37b91114253d5ull; h = ((h << 31) | (h >> 33)) * 0x4cf5ad432745937full; }
    for (; i < n; i++) h = (h ^ p[i]) * 0x100000001b3ull;
    return h ^ (h >> 29);
}
static void ft_edges_take(flush_tex *f) {   // right after the stamp and the protection; dropped if a write reached them meanwhile
    uintptr_t r[2][2], ia, ie; ft_edge_ranges(f->mem, f->size, r, &ia, &ie);
    for (int i = 0; i < 2; i++) f->edge_h[i] = r[i][1] > r[i][0] ? ft_hash(r[i][0], r[i][1]) : 0;
    f->edged = !nvn_dirty_since_span(f->span, f->mem, f->size, f->stamp);
}
static uint64_t g_ft_edge_skips;
static int ft_written(flush_tex *f) {   // g_ft_mu held. Only edge pages written, their bytes unchanged: stamped again, 0
    if (!nvn_dirty_since_span(f->span, f->mem, f->size, f->stamp)) return 0;
    if (!f->edged) return 1;
    uintptr_t r[2][2], ia, ie; ft_edge_ranges(f->mem, f->size, r, &ia, &ie);
    uint64_t st = nvn_dirty_now(); nvn_dirty_protect(f->mem, f->size);   // stamp, protect, then read: a later write is seen at the next check
    if (ie > ia && nvn_dirty_since_span(f->span, (const void *)ia, ie - ia, f->stamp)) return 1;
    for (int i = 0; i < 2; i++) if (r[i][1] > r[i][0] && ft_hash(r[i][0], r[i][1]) != f->edge_h[i]) return 1;
    f->stamp = st;
    if (++g_ft_edge_skips <= 4) LOG("[nvn] registered texture: only the pages it shares with other data were written, its bytes there unchanged: not taken as written (ser=%llu %ux%u format %#x, %llu so far)",
                                    (unsigned long long)f->t.serial, f->t.d.width, f->t.d.height, f->t.d.format, (unsigned long long)g_ft_edge_skips);
    return 0;
}
static void ft_add(const tex_t *t) {
    // Buffer textures are views onto the pool buffer. COMPRESSIBLE is only a hint (docs/reference/nvn-objects.md#compressible-in-a-pool-the-cpu-writes-is-only-a-hint).
    // A sparse pool's addresses are no range nvn_dirty tracks and would read as dirty at once: writes behind one are seen through the page table
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !t->mem || !t->pool || t->pool->vp || t->has_layout || t->d.target == NVN_TEXTURE_TARGET_BUFFER || (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) ||
        (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) return;
    flush_tex f = {t->mem, t->L.size, *t, nvn_dirty_now(), NULL, {0, 0}, 0};
    if (DM_GAME_TEX_DIRTY_UPLOAD) nvn_dirty_protect(t->mem, t->L.size);
    f.span = nvn_dirty_span(t->mem, t->L.size);
    if (DM_GAME_TEX_DIRTY_UPLOAD) ft_edges_take(&f);
    pthread_mutex_lock(&g_ft_mu);
    if (g_nft == g_cft) g_ft = dm_realloc(g_ft, (g_cft = g_cft ? 2 * g_cft : 1024) * sizeof *g_ft, "the texture flush table");
    g_ft[g_nft++] = f;
    pthread_mutex_unlock(&g_ft_mu);
}
static void ft_del(uint64_t serial) {
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !serial) return;
    pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft; i++) if (g_ft[i].t.serial == serial) { g_ft[i] = g_ft[--g_nft]; break; }
    pthread_mutex_unlock(&g_ft_mu);
}
// Profile tex_dirty_upload: CPU fills with no NVN flush. ft_add write-protects a registered texture; a write before its first resolution makes it CPU-written
int nvn_ft_written(const tex_t *t) {
    if (!DM_GAME_TEX_DIRTY_UPLOAD || !t->serial) return 0;
    int r = 0; pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft; i++) if (g_ft[i].t.serial == t->serial) {
        if (ft_written(&g_ft[i])) { r = 1; g_ft[i] = g_ft[--g_nft]; }
        break; }
    pthread_mutex_unlock(&g_ft_mu);
    if (r) mtl_tex_cpu_written(t);
    return r;
}
int nvn_ft_tracked(uint64_t serial) {   // registered and not resolved yet: its storage holds the CPU's writes and the GPU's
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !serial) return 0;
    int r = 0; pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft; i++) if (g_ft[i].t.serial == serial) { r = 1; break; }
    pthread_mutex_unlock(&g_ft_mu);
    return r;
}
// Later CPU rewrites: once per submission after a guest write, textures written since registration go to mtl_tex_cpu_rewritten unless their content is the GPU's.
// One the GPU wrote after the CPU stays with a new stamp, taken before the question; entries are found again by serial after it (one removed meanwhile is not put back).
enum { FT_POLL_BYTES = 128 << 20 };
void nvn_ft_poll(void) {
    if (!DM_GAME_TEX_DIRTY_UPLOAD) return;
    static uint64_t gen; static size_t cur, left;
    static struct { tex_t t; size_t at; int how; } *hit; static size_t chit; size_t nhit = 0;
    uint64_t now = nvn_dirty_now(), scanned = 0;
    pthread_mutex_lock(&g_ft_mu);
    if (now != gen) { gen = now; left = g_nft; }   // guest memory changed: every entry gets one more look
    while (left && g_nft && scanned < FT_POLL_BYTES) {
        if (cur >= g_nft) cur = 0;
        scanned += g_ft[cur].size; left--;
        if (ft_written(&g_ft[cur])) {   // edge-only writes restamp it and are no hit
            if (nhit == chit) hit = dm_realloc(hit, (chit = chit ? 2 * chit : 64) * sizeof *hit, "the texture flush poll's hits");
            hit[nhit].t = g_ft[cur].t; hit[nhit++].at = cur;
        }
        cur++;
    }
    pthread_mutex_unlock(&g_ft_mu);
    if (!nhit) return;
    uint64_t st = nvn_dirty_now();
    for (size_t i = 0; i < nhit; i++) nvn_dirty_protect(hit[i].t.mem, hit[i].t.L.size);
    static uint64_t up, kept;
    for (size_t i = 0; i < nhit; i++) {   // outside g_ft_mu: the question takes the serial table's and the spans' locks
        const tex_t *t = &hit[i].t;
        hit[i].how = mtl_tex_cpu_rewritten(t);
        if (hit[i].how == MTL_REWRITE_UPLOADED && ++up <= 4)
            LOG("[nvn] registered texture written by the CPU after its first use: uploaded from its storage (ser=%llu %ux%ux%u format %#x levels %u, %llu bytes; %llu so far)",
                (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.levels, (unsigned long long)t->L.size, (unsigned long long)up);
        if (hit[i].how == MTL_REWRITE_GPU_NOW && ++kept <= 4)
            LOG("[nvn] registered texture written by the CPU after its first use, left to the GPU until the CPU rewrites it (the GPU wrote its storage after the CPU): ser=%llu %ux%ux%u format %#x levels %u, %llu bytes (%llu so far, %llu uploaded)",
                (unsigned long long)t->serial, t->d.width, t->d.height, t->d.depth, t->d.format, t->d.levels, (unsigned long long)t->L.size, (unsigned long long)kept, (unsigned long long)up);
    }
    pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < nhit; i++) {   // a removal moves the last entry into its place
        size_t j = hit[i].at;
        if (j >= g_nft || g_ft[j].t.serial != hit[i].t.serial)
            for (j = 0; j < g_nft && g_ft[j].t.serial != hit[i].t.serial; j++) {}
        if (j == g_nft) continue;
        if (hit[i].how == MTL_REWRITE_GPU_NOW) { g_ft[j].stamp = st; ft_edges_take(&g_ft[j]); }   // read after st's protection
        else g_ft[j] = g_ft[--g_nft];
    }
    pthread_mutex_unlock(&g_ft_mu);
}
static void ft_flush(const uint8_t *a, uint64_t n) {
    if (!DM_GAME_TEX_FLUSH_UPLOAD || !a || !n) return;
    pthread_mutex_lock(&g_ft_mu);
    for (size_t i = 0; i < g_nft;)
        if (g_ft[i].mem < a + n && a < g_ft[i].mem + g_ft[i].size) { mtl_tex_cpu_written(&g_ft[i].t); g_ft[i] = g_ft[--g_nft]; } else i++;
    pthread_mutex_unlock(&g_ft_mu);
}
// Flushed ranges: disjoint sorted [lo, hi) spans, touching ones merged, dropped with their pool. Storage flushed before a texture's first GPU use is its content
// (tex_init_fill); later CPU writes need the profile's tex_flush_upload / tex_dirty_upload
typedef struct { const uint8_t *lo, *hi; } fl_span;
static fl_span *g_fl;
static size_t g_nfl, g_cfl;
static pthread_mutex_t g_fl_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t fl_find(const uint8_t *a, int touch) {   // g_fl_mu held
    size_t l = 0, h = g_nfl;
    while (l < h) { size_t m = (l + h) / 2; if (g_fl[m].hi < a || (!touch && g_fl[m].hi == a)) l = m + 1; else h = m; }
    return l;
}
static void fl_add(const uint8_t *a, uint64_t n) {
    if (!a || !n) return;
    const uint8_t *b = a + n;
    pthread_mutex_lock(&g_fl_mu);
    size_t i = fl_find(a, 1), j = i;
    if (i < g_nfl && g_fl[i].lo <= a && b <= g_fl[i].hi) { pthread_mutex_unlock(&g_fl_mu); return; }   // already covered
    while (j < g_nfl && g_fl[j].lo <= b) { if (g_fl[j].lo < a) a = g_fl[j].lo; if (g_fl[j].hi > b) b = g_fl[j].hi; j++; }
    if (i == j) {
        if (g_nfl == g_cfl) g_fl = dm_realloc(g_fl, (g_cfl = g_cfl ? 2 * g_cfl : 256) * sizeof *g_fl, "the flushed ranges");
        memmove(g_fl + i + 1, g_fl + i, (g_nfl - i) * sizeof *g_fl); g_nfl++;
    } else if (j > i + 1) { memmove(g_fl + i + 1, g_fl + j, (g_nfl - j) * sizeof *g_fl); g_nfl -= j - i - 1; }
    g_fl[i] = (fl_span){a, b};
    pthread_mutex_unlock(&g_fl_mu);
}
static void fl_drop(const uint8_t *a, uint64_t n) {   // neighbouring pools' spans may have merged with it
    const uint8_t *b = a + n;
    pthread_mutex_lock(&g_fl_mu);
    for (size_t i = fl_find(a, 0); i < g_nfl && g_fl[i].lo < b;) {
        fl_span s = g_fl[i];
        if (s.lo < a && s.hi > b) {
            if (g_nfl == g_cfl) g_fl = dm_realloc(g_fl, (g_cfl = 2 * g_cfl) * sizeof *g_fl, "the flushed ranges");
            memmove(g_fl + i + 2, g_fl + i + 1, (g_nfl - i - 1) * sizeof *g_fl); g_nfl++;
            g_fl[i].hi = a; g_fl[i + 1] = (fl_span){b, s.hi}; break;
        }
        if (s.lo < a) { g_fl[i].hi = a; i++; }
        else if (s.hi > b) { g_fl[i].lo = b; break; }
        else { memmove(g_fl + i, g_fl + i + 1, (g_nfl - i - 1) * sizeof *g_fl); g_nfl--; }
    }
    pthread_mutex_unlock(&g_fl_mu);
}
int nvn_flushed(const void *p, uint64_t n) {
    const uint8_t *a = p;
    pthread_mutex_lock(&g_fl_mu);
    size_t i = fl_find(a, 0); int r = i < g_nfl && g_fl[i].lo < a + n;
    pthread_mutex_unlock(&g_fl_mu);
    return r;
}
NVN(nvnTextureInitialize, NVNboolean, NVNtexture *t, const NVNtextureBuilder *b) {
    const tex_builder_t *s = (const tex_builder_t *)b; tex_t *o = TX(t);
    // NOLINTNEXTLINE(clang-analyzer-core.UndefinedBinaryOperatorResult): the object's bytes are the game's, read on purpose
    if (o->magic == MAGIC('T', 'E', 'X', 0) && o->serial) { static uint64_t n; if (!n++) LOG("[nvn] TextureInitialize on a texture object that is still live (no Finalize): releasing the old host texture (first case ser=%llu)", (unsigned long long)o->serial); mtl_tex_release(o); }
    memset(o, 0, sizeof *o);
    o->magic = MAGIC('T', 'E', 'X', 0); o->d = s->d; o->pool = s->pool; o->off = s->off;
    // NVN2 device flag bit 16 selects X1 GOB order; the texture keeps its device's
    { const dev_t_ *device = (const dev_t_ *)s->dev; o->d.gob_order = !device || !(device->flags & 0x10000); }
    o->has_layout = s->has_layout; memcpy(o->layout, s->layout, 8); o->set_swz = s->set_swz; o->stencil = s->stencil; o->swz = s->swz;
    nvn_tex_layout(&o->d, &o->L);
    if (s->pool) vpool_taken(s->pool);
    o->mem = s->pool && s->pool->mem ? s->pool->mem + s->off : NULL;
    o->serial = nvn_new_serial(); mtl_tex_init(o);
    {   // DM_NVN_TEXINFO=WxH logs every texture of that size as initialized
        static int ti = -1; static uint32_t tw, th;
        if (ti < 0) { const char *e = getenv("DM_NVN_TEXINFO"); ti = e && sscanf(e, "%ux%u", &tw, &th) == 2; }
        if (ti && o->d.width == tw && o->d.height == th) { static uint64_t n; if (n++ < 200) LOG("[nvn] TEXINFO init ser=%llu mem=%p pool=%p(mem %p size %#llx flags %#x) off=%#llx target=%d format=%#x flags=%#x samples=%d %ux%ux%u levels=%u stride=%u size=%#llx",
            (unsigned long long)o->serial, (void *)o->mem, (void *)o->pool, o->pool ? (void *)o->pool->mem : NULL, o->pool ? (unsigned long long)o->pool->size : 0ull, o->pool ? o->pool->flags : 0,
            (unsigned long long)o->off, o->d.target, o->d.format, o->d.flags, o->d.samples, o->d.width, o->d.height, o->d.depth, o->d.levels, o->d.stride, (unsigned long long)o->L.size); }
    }
    // linear textures are made on the pool buffer (tex_linear) and read what was copied in, not re-uploaded as packaged
    if (s->pkg_data && o->mem) {
        memcpy(o->mem, s->pkg_data, o->L.size);
        if (!(o->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET))) mtl_tex_cpu_written(o);
    }
    OLOG("Texture %p %ux%ux%u lv%u fmt=%#x tgt=%d flags=%#x size=%#llx", (void *)t, o->d.width, o->d.height, o->d.depth,
         o->d.levels, o->d.format, o->d.target, o->d.flags, o->L.size);
    ft_add(o); live_add(LV_TEX, t);
    return 1;
}
NVN(nvnTextureFinalize, void, NVNtexture *t) { live_del(LV_TEX, t); ft_del(TX(t)->serial); mtl_tex_release(TX(t)); TX(t)->magic = 0; }
NVN(nvnTextureGetFlags, NVNtextureFlags, const NVNtexture *t) { return (NVNtextureFlags)CTX(t)->d.flags; }
NVN(nvnTextureGetWidth, int, const NVNtexture *t) { return (int)CTX(t)->d.width; }
NVN(nvnTextureGetHeight, int, const NVNtexture *t) { return (int)CTX(t)->d.height; }
NVN(nvnTextureGetDepth, int, const NVNtexture *t) { return (int)CTX(t)->d.depth; }
NVN(nvnTextureGetStorageSize, int, const NVNtexture *t) { return (int)CTX(t)->L.size; }
NVN(nvnTextureGetStride, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->d.stride; }   // 0 for block-linear
// a game may map the pool these report
NVN(nvnTextureGetLevels, int, const NVNtexture *t) { return (int)CTX(t)->d.levels; }
NVN(nvnTextureGetSparseTileLayout, void, const NVNtexture *t, NVNtextureSparseTileLayout *o) { sparse_layout(&CTX(t)->d, o); }
NVN(nvnTextureGetSamples, int, const NVNtexture *t) { return CTX(t)->d.samples; }
NVN(nvnTextureGetMemoryPool, NVNmemoryPool *, const NVNtexture *t) { return (NVNmemoryPool *)CTX(t)->pool; }
NVN(nvnTextureGetMemoryOffset, ptrdiff_t, const NVNtexture *t) { return (ptrdiff_t)CTX(t)->off; }
// A CPU_NO_ACCESS block-linear texture's content is its own Metal texture, never its storage's bytes: its address does not put the pool into the raw pool table
static int tex_bytes_elsewhere(const tex_t *o) {
    return o->pool && (o->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) && o->d.target != NVN_TEXTURE_TARGET_BUFFER && !(o->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
}
NVN(nvnTextureGetTextureAddress, NVNtextureAddress, const NVNtexture *t) {
    const tex_t *o = CTX(t);
    if (o->pool) {
        vpool_taken(o->pool);
        if (!tex_bytes_elsewhere(o)) { ((pool_t *)o->pool)->asked = 1; mtl_pool_addressed(o->pool->mem + o->off, o->L.size ? o->L.size : 1); }
    }
    return o->pool ? (NVNtextureAddress)(o->pool->mem + o->off) : 0;
}
// The host does no ZCull, but games allocate the area from their pools and NVN never reports 0. The hardware's formula is unknown: 8 bytes per 32x32 tile and layer, at least 256
static size_t zcull_size(const nvn_texdesc *d) {
    uint64_t tiles = (uint64_t)((d->width + 31) / 32) * ((d->height + 31) / 32) * (d->depth ? d->depth : 1);
    return tiles ? (size_t)((tiles * 8 + 0xff) & ~0xffull) : 0x100;
}
NVN(nvnTextureGetZCullStorageSize, size_t, const NVNtexture *t) { return zcull_size(&CTX(t)->d); }
static unsigned tex_swz(const tex_t *t) { return t->set_swz ? t->swz : 2u | 3u << 3 | 4u << 6 | 5u << 9; }   // identity R G B A when SetSwizzle was not called
// the mapping counts: a game replaces a registered descriptor only if the two compare different
NVN(nvnTextureCompare, NVNboolean, const NVNtexture *a, const NVNtexture *b) {
    const tex_t *x = CTX(a), *y = CTX(b);
    return !memcmp(&x->d, &y->d, sizeof x->d) && x->mem == y->mem && tex_swz(x) == tex_swz(y);
}
// No prototype; callers compare the result with 1. Meaning unverified (open)
NVN_NOPROTO(nvnTextureGetRawStorageClass, int, const NVNtexture *t) { (void)t; return 1; }
// the class a texture would report; the host's virtual pools do not distinguish storage classes
NVN_NOPROTO(nvnTextureBuilderGetRawStorageClass, int, const NVNtextureBuilder *b) { (void)b; return 1; }
#define VW(v) ((view_t *)(v))
NVN(nvnTextureViewSetDefaults, void, NVNtextureView *v) { memset(v, 0, sizeof(view_t)); }
NVN(nvnTextureViewSetLevels, void, NVNtextureView *v, int base, int n) { VW(v)->set_levels = 1; VW(v)->base_level = base; VW(v)->nlevels = n; }
NVN(nvnTextureViewSetLayers, void, NVNtextureView *v, int min, int n) { VW(v)->set_layers = 1; VW(v)->min_layer = min; VW(v)->nlayers = n; }
NVN(nvnTextureViewSetFormat, void, NVNtextureView *v, NVNformat f) { VW(v)->set_format = 1; VW(v)->format = f; }
NVN(nvnTextureViewSetTarget, void, NVNtextureView *v, NVNtextureTarget t) { VW(v)->set_target = 1; VW(v)->target = t; }
// meaning taken from the name: no caller found to confirm it (open)
NVN(nvnTextureViewCompare, NVNboolean, const NVNtextureView *a, const NVNtextureView *b) {
    const view_t *x = (const view_t *)a, *y = (const view_t *)b;
    return x->set_levels == y->set_levels && x->set_layers == y->set_layers && x->set_format == y->set_format && x->set_target == y->set_target && x->set_swizzle == y->set_swizzle && x->set_dsmode == y->set_dsmode
        && (!x->set_levels || (x->base_level == y->base_level && x->nlevels == y->nlevels)) && (!x->set_layers || (x->min_layer == y->min_layer && x->nlayers == y->nlayers))
        && (!x->set_format || x->format == y->format) && (!x->set_target || x->target == y->target) && (!x->set_swizzle || !memcmp(x->swz, y->swz, sizeof x->swz)) && (!x->set_dsmode || x->dsmode == y->dsmode);
}
NVN(nvnTextureViewGetLayers, NVNboolean, const NVNtextureView *v, int *min, int *n) {
    const view_t *o = (const view_t *)v; *min = o->min_layer; *n = o->nlayers; return o->set_layers;
}
static ptrdiff_t view_offset(const nvn_texdesc *d, uint64_t layer_size, const view_t *w) {
    if (!w) return 0;
    uint64_t off = w->set_levels ? nvn_tex_level_offset(d, (unsigned)w->base_level) : 0;
    if (w->set_layers) off += (uint64_t)w->min_layer * layer_size;
    return (ptrdiff_t)off;
}
NVN(nvnTextureGetViewOffset, ptrdiff_t, const NVNtexture *t, const NVNtextureView *v) {
    const tex_t *o = CTX(t); return view_offset(&o->d, o->L.layer_size, (const view_t *)v);
}
// no prototype in this API's nvn.h
NVN_NOPROTO(nvnTextureBuilderGetViewOffset, ptrdiff_t, const NVNtextureBuilder *b, const NVNtextureView *v) {
    const nvn_texdesc *d = &((const tex_builder_t *)b)->d; nvn_texlayout L; nvn_tex_layout(d, &L);
    return view_offset(d, L.layer_size, (const view_t *)v);
}
// packaged textures read from pool memory, the ground truth; GPU-written ones from their MTLTexture
static void read_texels(const tex_t *o, const view_t *v, const NVNcopyRegion *r, uint8_t *dst, ptrdiff_t row, ptrdiff_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(o->d.format, &bw, &bh);
    uint32_t wb = (uint32_t)((r->width + bw - 1) / bw) * bpb, hb = (uint32_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    mtl_read_texels(o, level, layer0, r, dst, (size_t)row, (size_t)img);
}
NVN(nvnTextureReadTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst) {
    read_texels(CTX(t), (const view_t *)v, r, dst, 0, 0);
}
NVN(nvnTextureReadTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, void *dst, ptrdiff_t row, ptrdiff_t img) {
    read_texels(CTX(t), (const view_t *)v, r, dst, row, img);
}
NVN(nvnTextureInvalidateTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }

#define SB(b) (&((samp_builder_t *)(b))->p)
NVN(nvnSamplerBuilderSetDefaults, void, NVNsamplerBuilder *b) {
    NVNdevice *dev = ((samp_builder_t *)b)->dev; memset(b, 0, sizeof(samp_builder_t)); ((samp_builder_t *)b)->dev = dev;
    samp_par *o = SB(b);
    o->minf = NVN_MIN_FILTER_LINEAR; o->magf = NVN_MAG_FILTER_LINEAR;   // NVN defaults: no mip filtering
    o->ws = o->wt = o->wr = NVN_WRAP_MODE_REPEAT; o->cmp_func = NVN_COMPARE_FUNC_LESS;
    o->lod_min = 0.f; o->lod_max = 1000.f; o->aniso = 1.f;
}
NVN(nvnSamplerBuilderSetDevice, void, NVNsamplerBuilder *b, NVNdevice *d) { ((samp_builder_t *)b)->dev = d; }
NVN(nvnSamplerBuilderGetDevice, NVNdevice *, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->dev; }
NVN(nvnSamplerBuilderSetMinMagFilter, void, NVNsamplerBuilder *b, NVNminFilter mi, NVNmagFilter ma) { SB(b)->minf = mi; SB(b)->magf = ma; }
NVN(nvnSamplerBuilderSetWrapMode, void, NVNsamplerBuilder *b, NVNwrapMode s, NVNwrapMode t, NVNwrapMode r) { SB(b)->ws = s; SB(b)->wt = t; SB(b)->wr = r; }
NVN(nvnSamplerBuilderSetLodClamp, void, NVNsamplerBuilder *b, float lo, float hi) { SB(b)->lod_min = lo; SB(b)->lod_max = hi; }
NVN(nvnSamplerBuilderSetLodBias, void, NVNsamplerBuilder *b, float v) { SB(b)->lod_bias = v; }
NVN(nvnSamplerBuilderSetCompare, void, NVNsamplerBuilder *b, NVNcompareMode m, NVNcompareFunc f) { SB(b)->cmp_mode = m; SB(b)->cmp_func = f; }
NVN(nvnSamplerBuilderSetBorderColor, void, NVNsamplerBuilder *b, const float *c) { memcpy(SB(b)->border, c, 16); }
// Metal's fixed border colours are selected numerically, also for integer textures: a bit-cast integer 1 would select transparent black
NVN(nvnSamplerBuilderSetBorderColori, void, NVNsamplerBuilder *b, const int *c) { for (int i = 0; i < 4; i++) SB(b)->border[i] = (float)c[i]; }
NVN(nvnSamplerBuilderSetBorderColorui, void, NVNsamplerBuilder *b, const uint32_t *c) { for (int i = 0; i < 4; i++) SB(b)->border[i] = (float)c[i]; }
NVN(nvnSamplerBuilderSetMaxAnisotropy, void, NVNsamplerBuilder *b, float v) { SB(b)->aniso = v; }
NVN(nvnSamplerBuilderSetLodSnap, void, NVNsamplerBuilder *b, float v) { (void)b; (void)v; }   // no Metal equivalent
NVN(nvnSamplerBuilderGetMinMagFilter, void, const NVNsamplerBuilder *b, NVNminFilter *mi, NVNmagFilter *ma) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mi = (NVNminFilter)o->minf; *ma = (NVNmagFilter)o->magf;
}
NVN(nvnSamplerBuilderGetWrapMode, void, const NVNsamplerBuilder *b, NVNwrapMode *s, NVNwrapMode *t, NVNwrapMode *r) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *s = (NVNwrapMode)o->ws; *t = (NVNwrapMode)o->wt; *r = (NVNwrapMode)o->wr;
}
NVN(nvnSamplerBuilderGetLodClamp, void, const NVNsamplerBuilder *b, float *lo, float *hi) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *lo = o->lod_min; *hi = o->lod_max;
}
NVN(nvnSamplerBuilderGetLodBias, float, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->p.lod_bias; }
NVN(nvnSamplerBuilderGetCompare, void, const NVNsamplerBuilder *b, NVNcompareMode *mode, NVNcompareFunc *func) {
    const samp_par *o = &((const samp_builder_t *)b)->p; *mode = (NVNcompareMode)o->cmp_mode; *func = (NVNcompareFunc)o->cmp_func;
}
NVN(nvnSamplerBuilderGetBorderColor, void, const NVNsamplerBuilder *b, float *c) { memcpy(c, ((const samp_builder_t *)b)->p.border, 16); }
NVN(nvnSamplerBuilderGetMaxAnisotropy, float, const NVNsamplerBuilder *b) { return ((const samp_builder_t *)b)->p.aniso; }
NVN(nvnSamplerBuilderGetReductionFilter, NVNsamplerReduction, const NVNsamplerBuilder *b) { return (NVNsamplerReduction)((const samp_builder_t *)b)->p.reduction; }
NVN(nvnSamplerInitialize, NVNboolean, NVNsampler *s, const NVNsamplerBuilder *b) {
    samp_t *o = (samp_t *)s; memset(o, 0, sizeof *o); o->magic = MAGIC('S', 'M', 'P', 0); o->p = ((const samp_builder_t *)b)->p; return 1;   // games hash the bytes
}
NVN(nvnSamplerFinalize, void, NVNsampler *s) { ((samp_t *)s)->magic = 0; }

NVN(nvnProgramInitialize, NVNboolean, NVNprogram *p, NVNdevice *d) {
    (void)d; memset(p, 0, sizeof(prog_t)); ((prog_t *)p)->magic = MAGIC('P', 'R', 'O', 'G'); live_add(LV_PROG, p); return 1;
}
NVN(nvnProgramFinalize, void, NVNprogram *p) {
    live_del(LV_PROG, p);
    pthread_mutex_lock(&g_sub_mu); mtl_prog_forget((const prog_t *)p); mtl_prog_release((prog_t *)p); pthread_mutex_unlock(&g_sub_mu);
    ((prog_t *)p)->magic = 0;
}
// Per-stage keys by prog_t.serial: a game may draw with a copy of an NVNprogram, which keeps the serial but not the address. Records last the process:
// a copy can outlive its program.
typedef struct { uint8_t key[6][8]; uint8_t ok; } prog_keys_t;
static pthread_mutex_t g_pk_mu = PTHREAD_MUTEX_INITIALIZER; static CFMutableDictionaryRef g_pk;
static prog_keys_t *pk_get(uint64_t serial) {
    pthread_mutex_lock(&g_pk_mu);
    if (!g_pk) g_pk = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    prog_keys_t *k = (prog_keys_t *)CFDictionaryGetValue(g_pk, (const void *)(uintptr_t)serial);
    if (!k) { k = calloc(1, sizeof *k); CFDictionarySetValue(g_pk, (const void *)(uintptr_t)serial, k); }
    pthread_mutex_unlock(&g_pk_mu);
    return k;
}
// mapped = 1: only when the code area is in a registered pool (a game may fill it in after SetShaders).
// NVN2 control sections (version 0xc9 at +4): code length at +0x44; the key takes the first 0x48 bytes, +0x30 hashed as zero (BNSH and BAHS differ there). NVN1's have no key.
#define NVN2_KEY_CTL 0x48
static int nvn2_ctl(const uint8_t *c) { uint32_t magic, ver; memcpy(&magic, c, 4); memcpy(&ver, c + 4, 4); return magic == 0x98761234u && ver == 0xc9; }
void nvn_ctl_lens(const void *control, uint32_t *nctl, uint32_t *ncode) {
    const uint8_t *c = control;
    if (nvn2_ctl(c)) { memcpy(ncode, c + 0x44, 4); *nctl = NVN2_KEY_CTL; return; }
    *nctl = 8; *ncode = 0;
}
static int keys_hash(const NVNshaderData *sd, int mapped, uint8_t out[8]) {
    uint32_t ncode = 0, nctl;
    if (!sd->control) return 0;
    if (!nvn2_ctl(sd->control)) { static _Atomic int said; if (!atomic_exchange(&said, 1)) LOG("[RED] a shader's control section is not NVN2's (magic 0x98761234, version 0xc9): no shader key for it, as the shader library holds only NVN2 shaders"); return 0; }
    nvn_ctl_lens(sd->control, &nctl, &ncode);
    int code = sd->data && ncode && ncode < (1u << 24);
    if (mapped && !(code && mtl_addr_known(sd->data, ncode))) return 0;   // the pool is not made resident for that: the GPU runs the translated shader
    uint8_t md[CC_SHA256_DIGEST_LENGTH]; CC_SHA256_CTX hc; CC_SHA256_Init(&hc);
    uint8_t ctl[NVN2_KEY_CTL]; memcpy(ctl, sd->control, NVN2_KEY_CTL); memset(ctl + 0x30, 0, 4);
    CC_SHA256_Update(&hc, ctl, NVN2_KEY_CTL);
    if (code) CC_SHA256_Update(&hc, (const void *)(uintptr_t)sd->data, ncode);
    CC_SHA256_Final(md, &hc); memcpy(out, md, 8);
    return 1;
}
int prog_key(const prog_t *p, unsigned st, uint8_t key[8]) {
    if (st >= p->nstage) return 0;
    if (!p->serial) return keys_hash(&p->sd[st], 0, key);   // no SetShaders (the game wrote the struct): no record
    prog_keys_t *k = pk_get(p->serial);
    if (!(k->ok >> st & 1)) { if (!keys_hash(&p->sd[st], 0, k->key[st])) return 0; k->ok |= (uint8_t)(1u << st); }
    memcpy(key, k->key[st], 8);
    return 1;
}
NVN(nvnProgramSetShaders, NVNboolean, NVNprogram *p, int n, const NVNshaderData *sd) {
    prog_t *o = (prog_t *)p;
    if (n < 0 || n > 6) { LOG("[RED] ProgramSetShaders stage count %d", n); return 0; }
    pthread_mutex_lock(&g_sub_mu); mtl_prog_release(o); pthread_mutex_unlock(&g_sub_mu);
    o->nstage = n; memcpy(o->sd, sd, n * sizeof *sd); o->serial = nvn_new_serial();
    // keys computed here: the control section is valid only during this call
    static int dump = -1; if (dump < 0) dump = getenv("DM_NVN_SHDUMP") != NULL;
    static const char *trace; static int trace_init;
    if (!trace_init) { trace = getenv("DM_NVN_PROGTRACE"); trace_init = 1; }
    prog_keys_t *k = pk_get(o->serial);
    for (int i = 0; i < n; i++) {
        if (keys_hash(&sd[i], 1, k->key[i])) k->ok |= (uint8_t)(1u << i);
        // A stage keyed later is keyed from the host's copy of its control section's key bytes (NVN lets the game free its own); kept for the process, as the record
        if (sd[i].control && (dump || !(k->ok >> i & 1))) {
            uint32_t nctl, ncode; nvn_ctl_lens(sd[i].control, &nctl, &ncode);
            void *c = malloc(nctl); memcpy(c, sd[i].control, nctl); o->sd[i].control = c;
        }
        // diagnostic: trace the loader of selected shader keys (hex, comma-separated)
        if (trace && (k->ok & (1u << i))) {
            char key[17]; for (int j = 0; j < 8; j++) snprintf(key + j * 2, 3, "%02x", k->key[i][j]);
            if (strstr(trace, key)) {
                LOG("[nvn] ProgramSetShaders key=%s control=%p code=%#llx", key, sd[i].control, (unsigned long long)sd[i].data);
                dm_backtrace((uint64_t)__builtin_frame_address(0));
            }
        }
    }
    OLOG("ProgramSetShaders %p n=%d data0=%#llx", (void *)p, n, n ? (unsigned long long)sd[0].data : 0ull);
    mtl_prog_prefetch(o);   // libraries load in the background now, not at the first draw
    return 1;
}

NVN(nvnSyncInitialize, NVNboolean, NVNsync *s, NVNdevice *d) { (void)d; *(sync_t *)s = (sync_t){MAGIC('S', 'Y', 'N', 'C'), 0, 0}; live_add(LV_SYNC, s); return 1; }
NVN(nvnSyncFinalize, void, NVNsync *s) { live_del(LV_SYNC, s); nvn_sync_release((sync_t *)s); ((sync_t *)s)->magic = 0; }
NVN(nvnSyncSetDebugLabel, void, NVNsync *s, const char *l) { (void)s; (void)l; }
// all commands submitted before have completed: the latest submit's fence value
NVN(nvnQueueFenceSync, void, NVNqueue *q, NVNsync *s, NVNsyncCondition c, int f) { (void)c; (void)f; nvn_queue_fence((queue_t *)q,(sync_t *)s); }
// a dependency captures the current receipt: re-fencing or finalizing the sync later cannot retarget queued work
NVN(nvnQueueWaitSync, NVNboolean, NVNqueue *q, const NVNsync *s) { nvn_queue_wait_sync((queue_t *)q,(const sync_t *)s); return 1; }
// the host does no ZCull
NVN(nvnCommandBufferRestoreZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
NVN(nvnCommandBufferSaveZCullData, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n) { (void)c; (void)a; (void)n; }
// a culling hint only; the host does no ZCull
NVN_NOPROTO(nvnCommandBufferSetStencilCullCriteria, void, NVNcommandBuffer *c, int func, int ref) { (void)c; (void)func; (void)ref; }
static _Atomic uint64_t g_sync_ns;
// an expired long wait is what a game's GPU watchdog acts on: logged with the sync's state
static NVNsyncWaitResult sync_expired(const sync_t *o, uint64_t timeout, uint64_t waited) {
    static _Atomic int n;
    if (timeout >= 1000000000ull && atomic_fetch_add(&n, 1) < 8)
        LOG("[RED] nvnSyncWait: a %.1f s wait on sync %p expired after %.1f s (%s, fence %#llx)", timeout / 1e9, (const void *)o, waited / 1e9,
            o->fenced == 0 ? "never fenced since its Initialize" : o->fenced == 3 ? "its queue receipt not published" : "its fence not reached", (unsigned long long)o->fence);
    return NVN_SYNC_WAIT_RESULT_TIMEOUT_EXPIRED;
}
NVN(nvnSyncWait, NVNsyncWaitResult, const NVNsync *s, uint64_t timeout) {
    const sync_t *o = (const sync_t *)s;
    // never fenced since Initialize: signalled (on the console such a wait does not expire)
    if (!o->fenced) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    if(o->fenced==3) { if(nvn_sync_wait(o,0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED; uint64_t t0=now_ns(); int ok=nvn_sync_wait(o,timeout); atomic_fetch_add(&g_sync_ns,now_ns()-t0); return ok?NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED:sync_expired(o,timeout,now_ns()-t0); }
    int (*w)(uint64_t, uint64_t) = o->fenced == 2 ? mtl_wait_acq : mtl_wait;
    if (w(o->fence, 0)) return NVN_SYNC_WAIT_RESULT_ALREADY_SIGNALED;
    uint64_t t0 = now_ns(); int ok = w(o->fence, timeout); atomic_fetch_add(&g_sync_ns, now_ns() - t0);   // @sync_ms
    return ok ? NVN_SYNC_WAIT_RESULT_CONDITION_SATISFIED : sync_expired(o, timeout, now_ns() - t0);
}
NVN(nvnEventBuilderSetDefaults, void, NVNeventBuilder *b) { memset(b, 0, sizeof(ev_builder_t)); }
NVN(nvnEventBuilderSetStorage, void, NVNeventBuilder *b, const NVNmemoryPool *p, int64_t off) { *(ev_builder_t *)b = (ev_builder_t){(const pool_t *)p, off}; }
NVN(nvnEventInitialize, NVNboolean, NVNevent *e, const NVNeventBuilder *b) {
    const ev_builder_t *s = (const ev_builder_t *)b;
    if (!s->pool || !s->pool->mem) { LOG("[RED] EventInitialize has no storage"); return 0; }
    vpool_taken(s->pool); *(ev_t *)e = (ev_t){MAGIC('E', 'V', 'N', 'T'), (volatile uint32_t *)(s->pool->mem + s->off), s->pool, s->off}; return 1;
}
// games map the event's pool: it may not be null
NVN_NOPROTO(nvnEventGetMemoryPool, NVNmemoryPool *, const NVNevent *e) { return (NVNmemoryPool *)((const ev_t *)e)->pool; }
NVN_NOPROTO(nvnEventGetMemoryOffset, int64_t, const NVNevent *e) { return ((const ev_t *)e)->off; }
NVN(nvnEventFinalize, void, NVNevent *e) { ((ev_t *)e)->magic = 0; }
NVN(nvnEventGetValue, uint32_t, const NVNevent *e) { return atomic_load_explicit((_Atomic uint32_t *)((const ev_t *)e)->val,memory_order_acquire); }
static void ev_signal(const ev_t *e, int mode, uint32_t v) { nvn_event_publish(e->val,mode,v); }
NVN(nvnEventSignal, void, NVNevent *e, NVNeventSignalMode m, uint32_t v) { ev_signal((const ev_t *)e, m, v); }

#define QB(b) ((queue_builder_t *)(b))
// sizes start unset and each setter marks its own, as in NVN2's driver; unset ones take qb_size's defaults
NVN(nvnQueueBuilderSetDefaults, void, NVNqueueBuilder *b) {
    NVNdevice *dev = QB(b)->dev; memset(b, 0, sizeof(queue_builder_t)); QB(b)->dev = dev;
}
static void qb_set(NVNqueueBuilder *b, int k, size_t n) { QB(b)->sz[k] = (uint32_t)n; QB(b)->set[k] = 1; }
static uint64_t qb_size(const queue_builder_t *o, int k) { static const uint32_t def[4] = {65536, 262144, 16384, 0}; return o->set[k] ? o->sz[k] : def[k]; }
NVN(nvnQueueBuilderSetDevice, void, NVNqueueBuilder *b, NVNdevice *d) { QB(b)->dev = d; }
NVN(nvnQueueBuilderSetFlags, void, NVNqueueBuilder *b, int f) { QB(b)->flags = f; }
NVN(nvnQueueBuilderSetCommandMemorySize, void, NVNqueueBuilder *b, size_t n) { qb_set(b, QB_CMD, n); }
NVN(nvnQueueBuilderSetComputeMemorySize, void, NVNqueueBuilder *b, size_t n) { qb_set(b, QB_COMP, n); }
NVN(nvnQueueBuilderSetControlMemorySize, void, NVNqueueBuilder *b, size_t n) { qb_set(b, QB_CTL, n); }
NVN_NOPROTO(nvnQueueBuilderSetRayTracingMemorySize, void, NVNqueueBuilder *b, size_t n) { qb_set(b, QB_RT, n); }   // NVN2; the host traces no rays and only reports it back
NVN(nvnQueueBuilderSetCommandFlushThreshold, void, NVNqueueBuilder *b, size_t n) { QB(b)->flush = (uint32_t)n; }
// as NVN2's driver: the size, and whether it was set
static NVNboolean qb_get(const NVNqueueBuilder *b, int k, int *size) { *size = (int)QB(b)->sz[k]; return QB(b)->set[k]; }
NVN_NOPROTO(nvnQueueBuilderGetCommandMemorySize, NVNboolean, const NVNqueueBuilder *b, int *size) { return qb_get(b, QB_CMD, size); }
NVN_NOPROTO(nvnQueueBuilderGetComputeMemorySize, NVNboolean, const NVNqueueBuilder *b, int *size) { return qb_get(b, QB_COMP, size); }
NVN_NOPROTO(nvnQueueBuilderGetControlMemorySize, NVNboolean, const NVNqueueBuilder *b, int *size) { return qb_get(b, QB_CTL, size); }
NVN_NOPROTO(nvnQueueBuilderGetRayTracingMemorySize, NVNboolean, const NVNqueueBuilder *b, int *size) { return qb_get(b, QB_RT, size); }
// command + compute + control rounded to 4 KiB; the host does not use it
NVN(nvnQueueBuilderGetQueueMemorySize, size_t, const NVNqueueBuilder *b) {
    const queue_builder_t *o = (const queue_builder_t *)b;
    return (qb_size(o, QB_CMD) + qb_size(o, QB_COMP) + qb_size(o, QB_CTL) + 4095) & ~(size_t)4095;
}
NVN(nvnQueueBuilderSetQueueMemory, void, NVNqueueBuilder *b, void *m, size_t n) { QB(b)->mem = m; QB(b)->mem_sz = n; }
NVN(nvnQueueInitialize, NVNboolean, NVNqueue *q, const NVNqueueBuilder *b) {
    NVNdevice *d = ((const queue_builder_t *)b)->dev;
    // NVN sets the window origin in the queue's initial GPU state: a later device origin change does not rewrite it
    *(queue_t *)q = (queue_t){MAGIC('Q', 'U', 'E', 'U'), d, 0,
                            !d || ((const dev_t_ *)d)->origin == NVN_WINDOW_ORIGIN_MODE_LOWER_LEFT, nvn_new_serial()};
    OLOG("QueueInitialize %p", (void *)q); live_add(LV_QUEUE, q); return 1;
}
NVN(nvnQueueFinalize, void, NVNqueue *q) {
    live_del(LV_QUEUE, q);
    nvn_queue_forget((queue_t *)q);
    pthread_mutex_lock(&g_sub_mu); mtl_queue_forget(((queue_t *)q)->serial); ((queue_t *)q)->magic = 0; pthread_mutex_unlock(&g_sub_mu);
}
NVN(nvnQueueGetError, NVNqueueGetErrorResult, NVNqueue *q, NVNqueueErrorInfo *info) {
    (void)q; if (info) memset(info, 0, sizeof *info); return NVN_QUEUE_GET_ERROR_RESULT_GPU_NO_ERROR;
}
NVN(nvnQueueFlush, void, NVNqueue *q) { (void)q; }
NVN(nvnQueueFinish, void, NVNqueue *q) { nvn_queue_finish((queue_t *)q); }

#define CB(c) ((cmd_t *)(c))
// Command memory holds only records; a recording's end, links and calls are in control memory (ctl_e), as in NVN, so nothing is written past
// GetCommandMemoryUsed, which games size and place their command memory by (docs/reference/nvn-objects.md#a-recordings-end-and-links-are-in-control-memory)

NVN(nvnCommandBufferInitialize, NVNboolean, NVNcommandBuffer *c, NVNdevice *d) {
    memset(c, 0, sizeof(cmd_t)); CB(c)->magic = MAGIC('C', 'M', 'D', 'B'); CB(c)->dev = d; return 1;
}
NVN(nvnCommandBufferFinalize, void, NVNcommandBuffer *c) { CB(c)->magic = 0; }
NVN(nvnCommandBufferSetMemoryCallback, void, NVNcommandBuffer *c, PFNNVNCOMMANDBUFFERMEMORYCALLBACKPROC cb) { CB(c)->cb = cb; }
NVN(nvnCommandBufferSetMemoryCallbackData, void, NVNcommandBuffer *c, void *d) { CB(c)->cb_data = d; }
static uint8_t *align8(uint8_t *p) { return (uint8_t *)(((uintptr_t)p + 7) & ~(uintptr_t)7); }
// DM_NVN_CMDSTAT=1: every 300 frames, a line of command recording usage; counts only
static struct { int on; uint64_t bytes, recs, grants, grant_bytes, grant_min, grant_max, cbs, hostseg, op_n[256], op_b[256], frame0, waste; } g_cs = { .on = -1 };
static pthread_mutex_t g_cs_mu = PTHREAD_MUTEX_INITIALIZER;
uint64_t dm_nvn_frame_count(void);
static int cs_on(void) {
    if (g_cs.on < 0) g_cs.on = getenv("DM_NVN_CMDSTAT") != NULL;
    return g_cs.on;
}
static void cs_op(uint32_t op, uint32_t size) {
    op &= 0xff;
    if (!cs_on()) return;
    pthread_mutex_lock(&g_cs_mu);
    g_cs.op_n[op]++;
    g_cs.op_b[op] += size;
    pthread_mutex_unlock(&g_cs_mu);
}
static void cs_dump(void) {
    if (!cs_on()) return;
    uint64_t f = dm_nvn_frame_count();
    if (f - g_cs.frame0 < 300) return;
    pthread_mutex_lock(&g_cs_mu);
    double n = (double)(f - g_cs.frame0);
    char top[700];
    int k = 0;
    uint8_t used[256] = {0};
    for (int r = 0; r < 8; r++) {
        int bi = -1;
        for (int i = 0; i < 256; i++) if (!used[i] && g_cs.op_b[i] && (bi < 0 || g_cs.op_b[i] > g_cs.op_b[bi])) bi = i;
        if (bi < 0) break;
        used[bi] = 1;
        k += snprintf(top + k, sizeof top - k, " op %d: %.0f × %.0f bytes = %.1f KB;", bi, g_cs.op_n[bi] / n, (double)g_cs.op_b[bi] / (double)g_cs.op_n[bi], g_cs.op_b[bi] / n / 1024.0);
    }
    LOG("[nvn] command memory up to frame %llu: recorded per frame %.1f KB / %.0f records; segments from the game per frame %.2f, mean %.1f KB each (min %.1f / max %.1f); out-of-memory callbacks per frame %.2f; host-lent segments per frame %.3f; segment tails dropped on a switch per frame %.1f KB | per frame%s",
        (unsigned long long)f, g_cs.bytes / n / 1024.0, g_cs.recs / n, g_cs.grants / n, g_cs.grants ? g_cs.grant_bytes / (double)g_cs.grants / 1024.0 : 0, g_cs.grant_min / 1024.0, g_cs.grant_max / 1024.0, g_cs.cbs / n, g_cs.hostseg / n, g_cs.waste / n / 1024.0, top);
    int on = g_cs.on;
    memset(&g_cs, 0, sizeof g_cs);
    g_cs.on = on;
    g_cs.frame0 = f;
    pthread_mutex_unlock(&g_cs_mu);
}
static void put_hdr(uint8_t *p, uint32_t op, uint32_t size) {
    op_hdr h = {op, size};
    memcpy(p, &h, sizeof h);
    if (op) cs_op(op, size);
}
static void ctl_chunk_close(cmd_t *o);
static uint8_t *cmd_own(const cmd_t *o) { return o->own_back ? o->end - o->own_back : NULL; }
static void cmd_own_at(cmd_t *o, uint8_t *p) { if (!o->own_back && p < o->end && (uint64_t)(o->end - p) <= UINT32_MAX) o->own_back = (uint32_t)(o->end - p); }
// the open recording's records in the old segment become one piece of its control list (ctl_push at its next record)
static void cmd_new_segment(cmd_t *o, uint8_t *mem, size_t n) {
    uint8_t *start = align8(mem), *end = mem + n;
    if (o->recording == 1 && o->cur && cs_on() && o->end > o->cur) { pthread_mutex_lock(&g_cs_mu); g_cs.waste += (uint64_t)(o->end - o->cur); pthread_mutex_unlock(&g_cs_mu); }
    if (o->recording) ctl_chunk_close(o);
    o->cur = start; o->end = end; o->own_back = 0;
}
// Lent when the game's command memory runs out, reclaimed when another recording writes over the guest range it hangs off (docs/reference/nvn-objects.md#host-lent-command-memory-segments).
// The pool grows on demand, as many small command buffers may each overflow once a frame; abort only past HOSTSEG_CAP. Small empty-pool grants get HOSTSEG_SMALL.
#define HOSTSEG_SZ (256u << 10)
#define HOSTSEG_SMALL (4u << 10)
#define HOSTSEG_CAP (1u << 30)
uint64_t dm_nvn_frame_count(void);
typedef struct { uint8_t *mem; size_t n; uint64_t frame; int busy; uintptr_t org_lo, org_hi; uint32_t rec_id; } hostseg_t;   // org: the guest range of the recording it hangs off (0 = never reclaimed)
static hostseg_t *g_hostseg; static int g_nhostseg, g_caphostseg; static uint64_t g_hostseg_bytes;
static _Atomic int g_hostseg_held;   // 0 lets hostseg_written return without the lock
static _Atomic uint32_t g_rec_serial;   // a wrap onto a recording whose segment is still lent only keeps that segment lent
static pthread_mutex_t g_hostseg_mu = PTHREAD_MUTEX_INITIALIZER;
static size_t hostseg_size(const cmd_t *o, int empty_pool, size_t want) {
    if (empty_pool) return want && want <= HOSTSEG_SMALL && !o->recording ? HOSTSEG_SMALL : HOSTSEG_SZ;
    return want > HOSTSEG_SZ ? (want + 0xfff) & ~(size_t)0xfff : HOSTSEG_SZ;
}
// origin = the recording's guest bytes in the segment it ran short in, [own, write position); overflowing again keeps that segment's origin. No owned byte: no range
static uint8_t *hostseg_take(cmd_t *o, size_t n) {
    static unsigned total; uint64_t f = dm_nvn_frame_count(); uint8_t *m = NULL;
    if (o && o->recording) ctl_chunk_close(o);   // the records so far count as written before a free segment is chosen
    uintptr_t lo = 0, hi = 0, cur = o ? (uintptr_t)o->cur : 0;
    pthread_mutex_lock(&g_hostseg_mu);
    int in_seg = 0;   // by the segment's end too: a small segment can end where another starts
    for (int i = 0; i < g_nhostseg && cur; i++) if (cur >= (uintptr_t)g_hostseg[i].mem && o->end == g_hostseg[i].mem + g_hostseg[i].n) { lo = g_hostseg[i].org_lo; hi = g_hostseg[i].org_hi; in_seg = 1; break; }
    if (!in_seg && o && o->recording && cur) {
        uintptr_t own = cmd_own(o) && (uintptr_t)cmd_own(o) <= cur ? (uintptr_t)cmd_own(o) : cur < (uintptr_t)o->end ? cur : 0;
        if (own) { lo = own; hi = cur > lo ? cur : lo + 1; }
        else { static int once; if (!once++) LOG("[RED] NVN command memory: a recording ran short at the very end of the game's memory with no byte of its own there; the segment the host lends it is never reclaimed"); }
    }
    for (int i = 0; i < g_nhostseg && !m; i++)
        if (!g_hostseg[i].busy && g_hostseg[i].n == n) { g_hostseg[i] = (hostseg_t){ g_hostseg[i].mem, n, f, 1, lo, hi, o ? o->rec_id : 0 }; m = g_hostseg[i].mem; }
    if (!m && g_hostseg_bytes + n <= HOSTSEG_CAP && (m = malloc(n))) {
        if (g_nhostseg == g_caphostseg) { g_caphostseg = g_caphostseg ? g_caphostseg * 2 : 64; g_hostseg = dm_realloc(g_hostseg, sizeof *g_hostseg * (size_t)g_caphostseg, "the host segment table"); }
        g_hostseg[g_nhostseg++] = (hostseg_t){ m, n, f, 1, lo, hi, o ? o->rec_id : 0 }; g_hostseg_bytes += n;
    }
    if (m && hi > lo) atomic_fetch_add_explicit(&g_hostseg_held, 1, memory_order_relaxed);
    unsigned t = ++total; int ns = g_nhostseg; uint64_t bytes = g_hostseg_bytes;
    pthread_mutex_unlock(&g_hostseg_mu);
    if (!m) die("NVN command memory: the game's pool is empty and host-lent segments reached %u MB without being reclaimed by the game's blocks", HOSTSEG_CAP >> 20);
    if (t == 1 || t % 1024 == 0) LOG("[nvn] the game's command memory ran short, the host lends a segment of %zu KB (%u times so far, frame %llu, segment pool %d segments, %llu KB)", n >> 10, t, (unsigned long long)f, ns, (unsigned long long)(bytes >> 10));
    return m;
}
// [p, p + n) written by o (n = 1: a record tried at p): other recordings' segments over these guest bytes are dead
static void hostseg_written(const cmd_t *o, const uint8_t *p, uint64_t n) {
    if (!n || !atomic_load_explicit(&g_hostseg_held, memory_order_relaxed)) return;
    uintptr_t lo = (uintptr_t)p, hi = lo + n;
    pthread_mutex_lock(&g_hostseg_mu);
    for (int i = 0; i < g_nhostseg; i++) {
        hostseg_t *s = &g_hostseg[i];
        if (s->busy && s->org_hi > s->org_lo && s->rec_id != o->rec_id && s->org_hi > lo && s->org_lo < hi) { s->busy = 0; atomic_fetch_sub_explicit(&g_hostseg_held, 1, memory_order_relaxed); }
    }
    pthread_mutex_unlock(&g_hostseg_mu);
}
NVN(nvnCommandBufferAddCommandMemory, void, NVNcommandBuffer *c, const NVNmemoryPool *p, ptrdiff_t off, size_t n) {
    const pool_t *pl = (const pool_t *)p;
    if (pl) __atomic_store_n(&CB(c)->cmd_size, n, __ATOMIC_RELAXED);   // the memory the game added, whether the host records into it or a lent segment
    // grants as small as 40 bytes keep their guest storage, so an overflow has an origin to reclaim by; cmd_alloc checks each record fits
    if (!pl || n < sizeof(op_hdr)) { if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.hostseg++; pthread_mutex_unlock(&g_cs_mu); } size_t sz = hostseg_size(CB(c), !pl, n); cmd_new_segment(CB(c), hostseg_take(CB(c), sz), sz); return; }
    if (!pl->mem) { LOG("[RED] AddCommandMemory: pool has no CPU storage"); return; }
    vpool_taken(pl);
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.grants++; g_cs.grant_bytes += n; if (!g_cs.grant_min || n < g_cs.grant_min) g_cs.grant_min = n; if (n > g_cs.grant_max) g_cs.grant_max = n; pthread_mutex_unlock(&g_cs_mu); }
    { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ng; if (cblog && (++ng <= 60 || ng % 500 == 0)) LOG("[nvn] AddCommandMemory #%llu cmd=%p pool %p +%#lx length %#zx (recording %d)", (unsigned long long)ng, (void *)c, (const void *)pl, (long)off, n, CB(c)->recording); }
    cmd_new_segment(CB(c), pl->mem + off, n);
    CB(c)->used = 0;   // counts from the start of the memory the game last added, across recordings
}
// A non-final control entry is written only with 8 bytes left after it, so a list cut off by new control memory can always be linked on (CTL_LINK)
static void ctl_set(cmd_t *o, uint8_t *m, size_t n) {
    uint8_t *a = m ? align8(m) : NULL; size_t pad = (size_t)(a - m); n = a && n > pad ? (n - pad) & ~(size_t)7 : 0;
    if (n > 0xfffffff8u) n = 0xfffffff8u;
    if (o->ctl_list && o->ctl_mem) {
        if (n < 8) return;   // nothing to link to: the list goes on in the old block's reserved bytes
        uint64_t e = ctl_e(CTL_LINK, a, 0); memcpy(o->ctl_mem + o->ctl_used, &e, 8);
    }
    o->ctl_mem = a; o->ctl_size = (uint32_t)n; o->ctl_used = 0;
}
// lent when the game's callback gives none; never reclaimed (nothing tells when its handles are dead)
#define CTLSEG_SZ (64u << 10)
static uint8_t *ctlseg_take(void) {
    static uint64_t bytes; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu); uint64_t b = bytes += CTLSEG_SZ; pthread_mutex_unlock(&mu);
    if (b > (256u << 20)) die("NVN control memory: the game gives none and host-lent control memory reached 256 MB");
    uint8_t *m = malloc(CTLSEG_SZ); if (!m) die("NVN control memory: cannot lend a block");
    if (b == CTLSEG_SZ || b % (16u << 20) == 0) LOG("[nvn] the game gave no control memory when asked, the host lends a block (%llu KB so far)", (unsigned long long)(b >> 10));
    return m;
}
static void ctl_put(cmd_t *o, uint64_t e) {
    uint32_t need = e & CTL_LAST ? 8 : 16;
    for (int tries = 0; !o->ctl_mem || o->ctl_size - o->ctl_used < need; tries++) {
        if (!o->cb || tries == 2) { ctl_set(o, ctlseg_take(), CTLSEG_SZ); break; }
        o->cb((NVNcommandBuffer *)o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_CONTROL_MEMORY, need, o->cb_data);
    }
    uint8_t *p = o->ctl_mem + o->ctl_used;
    // ctlseg_take aborts on allocation failure; the analyzer loses that check
    memcpy(p, &e, 8);   // NOLINT(clang-analyzer-unix.cstring.NullArg)
    o->ctl_used += 8;
    if (!o->ctl_list) o->ctl_list = p;   // the list's first entry is the recording's handle
}
// the newest entry waits for the next (is it the last?); one closed by a segment switch waits for host code (ctl_flush), not inside AddCommandMemory
static void ctl_push(cmd_t *o, uint64_t e) {
    if (o->ctl_q[1]) { ctl_put(o, o->ctl_q[0]); o->ctl_q[0] = o->ctl_q[1]; o->ctl_q[1] = 0; }
    o->ctl_q[o->ctl_q[0] ? 1 : 0] = e;
}
static void ctl_flush(cmd_t *o) { if (o->ctl_q[1]) { ctl_put(o, o->ctl_q[0]); o->ctl_q[0] = o->ctl_q[1]; o->ctl_q[1] = 0; } }
static void ctl_chunk_close(cmd_t *o) {
    if (o->chunk && o->cur > o->chunk) { hostseg_written(o, o->chunk, (uint64_t)(o->cur - o->chunk)); ctl_push(o, ctl_e(CTL_CHUNK, o->chunk, (uint64_t)(o->cur - o->chunk))); }
    o->chunk = NULL;
}
NVN(nvnCommandBufferAddControlMemory, void, NVNcommandBuffer *c, void *m, size_t n) { ctl_set(CB(c), m, n); }
// not enough room: the game's callback adds command memory, per NVN
static uint8_t *cmd_alloc(cmd_t *o, uint32_t size) {
    size = (size + 7) & ~7u;
    for (int tries = 0; !o->cur || o->cur + size > o->end; tries++) {
        // the record goes here in the game's count: a lent segment hanging off this position belongs to a dead list
        if (o->recording && o->cur && o->cur < o->end) { cmd_own_at(o, o->cur); hostseg_written(o, o->cur, 1); }
        // the callback cannot supply enough: go on in a host segment
        if (!o->cb || tries == 2) {
            static int once; if (!once++) LOG("[nvn] command memory: the game's callback did not add enough (cb=%p, need %u bytes), using a host-lent segment", (void *)o->cb, size);
            size_t sz = hostseg_size(o, 0, size); cmd_new_segment(o, hostseg_take(o, sz), sz); break;
        }
        if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.cbs++; pthread_mutex_unlock(&g_cs_mu); }
        { static int cblog = -1; if (cblog < 0) cblog = getenv("DM_NVN_CBLOG") != NULL; static uint64_t ncb;
          if (cblog && (++ncb <= 40 || ncb % 1000 == 0)) LOG("[nvn] command memory callback #%llu cmd=%p need %u (used in this segment %ld)", (unsigned long long)ncb, (void *)o, size, o->cur && o->end ? (long)(o->cur - (o->end - 1)) : -1L); }
        o->cb((NVNcommandBuffer *)o, NVN_COMMAND_BUFFER_MEMORY_EVENT_OUT_OF_COMMAND_MEMORY, size, o->cb_data);
    }
    if (cs_on()) { pthread_mutex_lock(&g_cs_mu); g_cs.bytes += size; g_cs.recs++; pthread_mutex_unlock(&g_cs_mu); }
    uint8_t *p = o->cur; o->cur += size; o->used += size; put_hdr(p, 0, size);
    if (o->recording == 2) { o->begin = p; o->recording = 1; }
    if (o->recording) cmd_own_at(o, p);
    if (o->recording) {
        if (o->chunk && (uint64_t)(o->cur - o->chunk) > CTL_CHUNK_MAX) { if (p > o->chunk) { hostseg_written(o, o->chunk, (uint64_t)(p - o->chunk)); ctl_push(o, ctl_e(CTL_CHUNK, o->chunk, (uint64_t)(p - o->chunk))); } o->chunk = NULL; }
        if (size > CTL_CHUNK_MAX) { hostseg_written(o, p, size); ctl_push(o, ctl_e(CTL_CHUNK, p, CTL_ONE)); }   // longer than a piece can say: an entry of its own
        else if (!o->chunk) o->chunk = p;
        ctl_flush(o);
    }
    return p;
}
// As in NVN, no BEGIN record: an empty recording takes no command memory, and games sum scratch recordings' GetCommandMemoryUsed to size theirs
NVN(nvnCommandBufferBeginRecording, void, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    if (o->recording) LOG("[RED] BeginRecording re-entered");
    o->recording = 2;   // 2: recording, no command yet (cmd_alloc sets begin)
    o->rec_id = atomic_fetch_add_explicit(&g_rec_serial, 1, memory_order_relaxed) + 1;
    o->chunk = NULL; o->ctl_list = NULL; o->ctl_q[0] = o->ctl_q[1] = 0; o->own_back = 0;
}
// the last entry carries CTL_LAST; an empty recording's list is one empty piece
NVN(nvnCommandBufferEndRecording, NVNcommandHandle, NVNcommandBuffer *c) {
    cmd_t *o = CB(c);
    ctl_chunk_close(o);
    if (!o->ctl_q[0]) o->ctl_q[0] = ctl_e(CTL_CHUNK, NULL, 0);
    ctl_flush(o); ctl_put(o, o->ctl_q[0] | CTL_LAST); o->ctl_q[0] = 0;
    uint8_t *h = o->ctl_list; o->ctl_list = NULL; o->recording = 0;
    return (NVNcommandHandle)h;
}
NVN(nvnCommandBufferIsRecording, NVNboolean, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->recording != 0; }
// NULL past the last; a handle that is not a list: red line, no pieces
typedef struct { const uint8_t *e; int done; } ctl_iter;
static int ctl_next(ctl_iter *it, uint64_t *out) {
    for (int links = 0; !it->done && links < 1024; links++) {
        uint64_t v; memcpy(&v, it->e, 8);
        if (ctl_kind(v) == CTL_LINK) { it->e = ctl_addr(v); continue; }
        if (ctl_kind(v) != CTL_CHUNK) { LOG("[RED] NVN command list: control entry %#llx at %p is no piece", (unsigned long long)v, (const void *)it->e); it->done = 1; return 0; }
        it->e += 8; it->done = !!(v & CTL_LAST); *out = v & ~CTL_LAST; return 1;
    }
    if (!it->done) { LOG("[RED] NVN command list: more than 1024 links in a row at %p", (const void *)it->e); it->done = 1; }
    return 0;
}
static int ctl_handle_ok(NVNcommandHandle h) {
    if (h && !(h & 7) && !(h >> 47)) return 1;
    LOG("[RED] CallCommands / CopyCommands: handle %#llx is no command list", (unsigned long long)h); return 0;
}
// 0 at the piece's end or at a record that does not fit: pieces are read as their memory holds them at submit, as the GPU does
// (docs/reference/nvn-objects.md#a-called-recording-recorded-over-before-its-caller-is-submitted)
static int nvn_piece_record(const void *list, uint64_t v, const uint8_t *p, const uint8_t *pe, op_hdr *h) {
    if (p >= pe) return 0;
    memcpy(h, p, sizeof *h);
    if (h->size >= 8 && !(h->size & 7) && h->size <= (size_t)(pe - p)) return 1;
    static _Atomic uint64_t n; uint64_t k = atomic_fetch_add(&n, 1) + 1;
    if (k <= 8 || k % 1000 == 0)
        LOG("[RED] NVN command list %p: record {op %u, size %u} at %p, %zu bytes into its piece of %zu at %p, does not fit: the piece was recorded over after it was called; the rest of the piece is skipped (%llu so far)",
            list, h->op, h->size, (const void *)p, (size_t)(p - ctl_addr(v)), (size_t)(pe - ctl_addr(v)), (const void *)ctl_addr(v), (unsigned long long)k);
    return 0;
}
// the called pieces join the caller's list and run in place at submit; no command memory
NVN(nvnCommandBufferCallCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    cmd_t *o = CB(c);
    if (!o->recording) { LOG("[RED] CallCommands outside a recording"); return; }
    for (int i = 0; i < n; i++) {
        if (!ctl_handle_ok(h[i])) continue;
        ctl_chunk_close(o);
        ctl_iter it = { (const uint8_t *)(uintptr_t)h[i], 0 }; uint64_t v;
        while (ctl_next(&it, &v)) if (ctl_len(v)) { ctl_push(o, v); ctl_flush(o); }
    }
}
// records copied, as the driver copies the GPU commands: the source may be recorded over at once
NVN(nvnCommandBufferCopyCommands, void, NVNcommandBuffer *c, int n, const NVNcommandHandle *h) {
    cmd_t *o = CB(c);
    if (!o->recording) { LOG("[RED] CopyCommands outside a recording"); return; }
    for (int i = 0; i < n; i++) {
        if (!ctl_handle_ok(h[i])) continue;
        ctl_iter it = { (const uint8_t *)(uintptr_t)h[i], 0 }; uint64_t v; op_hdr r;
        while (ctl_next(&it, &v))
            for (const uint8_t *p = ctl_addr(v), *pe = p + ctl_chunk_bytes(v); nvn_piece_record((const void *)(uintptr_t)h[i], v, p, pe, &r); p += r.size) {
                uint8_t *d = cmd_alloc(o, r.size); memcpy(d, p, r.size); cs_op(r.op, r.size);
            }
    }
}
static size_t rec_expand(const uint8_t *rec_, uint8_t *out);
uint8_t *nvn_rec_linear(uint64_t handle, size_t *len) {
    size_t n = 0, cap = 4096; uint8_t *buf = malloc(cap);
    if (!buf) die("NVN command list: out of memory for a linear copy");
    ctl_iter it = { (const uint8_t *)(uintptr_t)handle, 0 }; uint64_t v;
    for (;;) {
        size_t k = ctl_next(&it, &v) ? ctl_chunk_bytes(v) : 0, want = n + k + 8;
        if (want > cap) { uint8_t *nb = realloc(buf, cap = want * 2); if (!nb) die("NVN command list: out of memory for a linear copy"); buf = nb; }
        if (!k) { if (it.done) break; continue; }
        op_hdr h;
        for (const uint8_t *p = ctl_addr(v), *pe = p + k; nvn_piece_record((const void *)(uintptr_t)handle, v, p, pe, &h); ) {   // compact records expanded, as the queue's flatten does
            uint8_t x[REC_EXPAND_MAX] __attribute__((aligned(8))); const uint8_t *src = p; size_t m = h.size;
            if (rec_compact(h.op)) { m = rec_expand(p, x); src = x; }
            if (n + m + 8 > cap) { uint8_t *nb = realloc(buf, cap = (n + m + 8) * 2); if (!nb) die("NVN command list: out of memory for a linear copy"); buf = nb; }
            memcpy(buf + n, src, m); n += m; p += h.size;
        }
    }
    put_hdr(buf + n, OP_END, 8); n += 8;
    if (len) *len = n;
    return buf;
}
NVN(nvnCommandBufferWaitEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventWaitMode mode, uint32_t value) {
    uint8_t *p=cmd_alloc(CB(c),24); put_hdr(p,OP_WAIT_EVENT,24); volatile uint32_t *word=((const ev_t *)e)->val; uint32_t m=mode;
    memcpy(p+8,&word,8); memcpy(p+16,&m,4); memcpy(p+20,&value,4);
}
NVN(nvnCommandBufferWaitSync, void, NVNcommandBuffer *c, const NVNsync *s) {
    uint8_t *p=cmd_alloc(CB(c),16); put_hdr(p,OP_WAIT_SYNC,16); memcpy(p+8,&s,8);
}
NVN(nvnCommandBufferSignalEvent, void, NVNcommandBuffer *c, const NVNevent *e, NVNeventSignalMode m, NVNeventSignalLocation loc, int flags, uint32_t v) {
    (void)loc; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    const ev_t *ev = (const ev_t *)e; volatile uint32_t *val = ev->val; uint32_t mm = m;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// fenced with the submission that reaches this point
NVN(nvnCommandBufferFenceSync, void, NVNcommandBuffer *c, NVNsync *s, NVNsyncCondition cond, int flags) {
    (void)cond; (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_FENCE_SYNC, 16); memcpy(p + 8, &s, 8);
}
NVN(nvnCommandBufferResetCounter, void, NVNcommandBuffer *c, NVNcounterType t) {
    uint8_t *p = cmd_alloc(CB(c), 16); put_hdr(p, OP_RESET_COUNTER, 16); uint64_t tt = t; memcpy(p + 8, &tt, 8);
}
NVN(nvnCommandBufferReportCounter, void, NVNcommandBuffer *c, NVNcounterType t, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_REPORT_COUNTER, 24); uint64_t tt = t; memcpy(p + 8, &tt, 8); memcpy(p + 16, &a, 8);
}
NVN(nvnCommandBufferCopyBufferToBuffer, void, NVNcommandBuffer *c, NVNbufferAddress src, NVNbufferAddress dst, size_t n, int flags) {
    (void)flags;
    uint8_t *p = cmd_alloc(CB(c), 32); put_hdr(p, OP_COPY_B2B, 32); uint64_t nn = n;
    memcpy(p + 8, &src, 8); memcpy(p + 16, &dst, 8); memcpy(p + 24, &nn, 8);
}
// data copied at record time (the caller may change it after) and written at submit
NVN(nvnCommandBufferUpdateUniformBuffer, void, NVNcommandBuffer *c, NVNbufferAddress buf, size_t bufsz, ptrdiff_t off, size_t n, const void *data) {
    (void)bufsz;
    uint32_t sz = (uint32_t)(24 + n);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, OP_UPDATE_UB, (sz + 7) & ~7u);
    uint64_t dst = buf + off, nn = n; memcpy(p + 8, &dst, 8); memcpy(p + 16, &nn, 8); memcpy(p + 24, data, n);
}

static void *rec(NVNcommandBuffer *c, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, op, sz);
    memset(p + 8, 0, sz - 8);
    return p + 8;
}
#define REC(c, OP, T) ((T *)rec((c), (OP), sizeof(T)))
// sized by the actual count: the replay reads only the first n
#define RECN(c, OP, T, ARR, N) ((T *)rec((c), (OP), offsetof(T, ARR) + sizeof(((T *)0)->ARR[0]) * (size_t)(N)))
// Compact records (OPC_*): no more bytes than the NVN2 driver's command where the 8-byte header allows, as games size command memory by them.
// g_rec_compact = 0 (tests) records the canonical forms
static int g_rec_compact = 1;
static uint8_t *crec(NVNcommandBuffer *c, uint32_t opc, uint32_t args, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7);
    uint8_t *p = cmd_alloc(CB(c), sz); put_hdr(p, opc | args << 8, sz); memset(p + 8, 0, sz - 8);
    return p + 8;
}
static void put32(uint8_t **q, uint32_t v) { memcpy(*q, &v, 4); *q += 4; }
static void put64(uint8_t **q, uint64_t v) { memcpy(*q, &v, 8); *q += 8; }
// indirect / multi-draw fields (ind onwards) only when used
static void rec_draw(NVNcommandBuffer *c, p_draw d) {
    if (g_rec_compact && !d.mdi_cnt && (uint32_t)d.prim < 0x100) {
        uint8_t *q;
        if (d.ind) { q = crec(c, OPC_DRAW_IND, (uint32_t)d.prim, 8); put64(&q, d.ind); return; }
        if (d.instances == 1 && !d.base_instance) { q = crec(c, OPC_DRAW, (uint32_t)d.prim, 8); put32(&q, (uint32_t)d.first); put32(&q, (uint32_t)d.count); return; }
        q = crec(c, OPC_DRAW_INST, (uint32_t)d.prim, 16); put32(&q, (uint32_t)d.first); put32(&q, (uint32_t)d.count); put32(&q, (uint32_t)d.base_instance); put32(&q, (uint32_t)d.instances); return;
    }
    size_t n = d.ind || d.mdi_cnt ? sizeof d : offsetof(p_draw, ind); memcpy(rec(c, OP_DRAW, n), &d, n);
}
static void rec_draw_idx(NVNcommandBuffer *c, p_draw_idx d) {
    if (g_rec_compact && !d.mdi_cnt && (uint32_t)d.prim < 0x100 && (uint32_t)d.itype < 0x100) {
        uint32_t a = (uint32_t)d.prim | (uint32_t)d.itype << 8; uint8_t *q;
        if (d.ind) { if (d.count || d.base_vertex || d.base_instance || d.instances != 1) goto full; q = crec(c, OPC_DRAW_IDX_IND, a, 16); put64(&q, d.ibuf); put64(&q, d.ind); return; }
        if (d.instances == 1 && !d.base_instance) { q = crec(c, OPC_DRAW_IDX, a, 16); put32(&q, (uint32_t)d.count); put32(&q, (uint32_t)d.base_vertex); put64(&q, d.ibuf); return; }
        q = crec(c, OPC_DRAW_IDX_INST, a, 24); put32(&q, (uint32_t)d.count); put32(&q, (uint32_t)d.base_vertex); put32(&q, (uint32_t)d.base_instance); put32(&q, (uint32_t)d.instances); put64(&q, d.ibuf); return;
    }
full:;
    size_t n = d.ind || d.mdi_cnt ? sizeof d : offsetof(p_draw_idx, ind); memcpy(rec(c, OP_DRAW_IDX, n), &d, n);
}
// out: at least REC_EXPAND_MAX bytes
static uint8_t *xrec(uint8_t *out, size_t *n, uint32_t op, size_t payload) {
    uint32_t sz = (uint32_t)((8 + payload + 7) & ~(size_t)7); uint8_t *p = out + *n;
    if (*n + sz > REC_EXPAND_MAX) die("NVN compact record: its expansion exceeds REC_EXPAND_MAX");
    op_hdr h = {op, sz}; memcpy(p, &h, 8); memset(p + 8, 0, sz - 8); *n += sz; return p + 8;
}
static uint32_t get32(const uint8_t **q) { uint32_t v; memcpy(&v, *q, 4); *q += 4; return v; }
static uint64_t get64(const uint8_t **q) { uint64_t v; memcpy(&v, *q, 8); *q += 8; return v; }
static size_t rec_expand(const uint8_t *rec_, uint8_t *out) {
    op_hdr h; memcpy(&h, rec_, 8); uint32_t a = h.op >> 8; const uint8_t *q = rec_ + 8; size_t n = 0;
    switch (h.op & 0xff) {
    case OPC_DRAW: case OPC_DRAW_INST: case OPC_DRAW_IND: {
        p_draw d = { .prim = (int32_t)(a & 0xff), .instances = 1 };
        if ((h.op & 0xff) == OPC_DRAW_IND) d.ind = get64(&q);
        else { d.first = (int32_t)get32(&q); d.count = (int32_t)get32(&q); if ((h.op & 0xff) == OPC_DRAW_INST) { d.base_instance = (int32_t)get32(&q); d.instances = (int32_t)get32(&q); } }
        size_t k = d.ind ? sizeof d : offsetof(p_draw, ind); memcpy(xrec(out, &n, OP_DRAW, k), &d, k); break; }
    case OPC_DRAW_IDX: case OPC_DRAW_IDX_INST: case OPC_DRAW_IDX_IND: {
        p_draw_idx d = { .prim = (int32_t)(a & 0xff), .itype = (int32_t)(a >> 8 & 0xff), .instances = 1 };
        if ((h.op & 0xff) == OPC_DRAW_IDX_IND) { d.ibuf = get64(&q); d.ind = get64(&q); }
        else { d.count = (int32_t)get32(&q); d.base_vertex = (int32_t)get32(&q); if ((h.op & 0xff) == OPC_DRAW_IDX_INST) { d.base_instance = (int32_t)get32(&q); d.instances = (int32_t)get32(&q); } d.ibuf = get64(&q); }
        size_t k = d.ind ? sizeof d : offsetof(p_draw_idx, ind); memcpy(xrec(out, &n, OP_DRAW_IDX, k), &d, k); break; }
    case OPC_VIEWPORTS: {
        p_viewports *r = (p_viewports *)xrec(out, &n, OP_VIEWPORTS, sizeof *r); r->first = (int32_t)(a & 0xff); r->n = (int32_t)(a >> 8 & 0xff); r->origin = (uint8_t)(a >> 16 & 3);
        for (int i = 0; i < r->n; i++) { memcpy(r->v[i], q, 16); q += 16; }
        break; }
    case OPC_STENCIL: {
        p_stencil s = { .face = (int32_t)(a & 3), .which = (int32_t)(a >> 2 & 3) }; int32_t v = (int32_t)(a >> 4);
        if (s.which == 0) s.value_mask = v; else if (s.which == 1) s.mask = v; else s.ref = v;
        memcpy(xrec(out, &n, OP_STENCIL, sizeof s), &s, sizeof s); break; }
    case OPC_HANDLES: {
        int cnt = (int)(a >> 17 & 0x3f);
        p_bind_handles *r = (p_bind_handles *)xrec(out, &n, a & 0x3f, offsetof(p_bind_handles, h) + 8 * (size_t)cnt);
        r->stage = (int32_t)(a >> 6 & 7); r->first = (int32_t)(a >> 9 & 0xff); r->n = cnt;
        for (int i = 0; i < cnt; i++) r->h[i] = get32(&q);
        break; }
    case OPC_BUFS: {
        uint32_t op = a & 0x3f, st = a >> 6 & 7, first = a >> 9 & 0xff, cnt = a >> 17 & 0x3f;
        for (uint32_t i = 0; i < cnt; i++) {
            uint64_t addr = get64(&q); uint32_t size = get32(&q);
            if (op == OP_BIND_UBO_SMALL) *(p_bind_ubo_small *)xrec(out, &n, op, sizeof(p_bind_ubo_small)) = (p_bind_ubo_small){(uint16_t)st, (uint16_t)(first + i), size, addr};
            else *(p_bind_buf *)xrec(out, &n, op, sizeof(p_bind_buf)) = (p_bind_buf){(int32_t)st, (int32_t)(first + i), addr, size};
        }
        break; }
    case OPC_VBUFS: {
        int cnt = (int)(a >> 8 & 0xff);
        p_bind_vbuf *r = (p_bind_vbuf *)xrec(out, &n, OP_BIND_VBUF, offsetof(p_bind_vbuf, r) + sizeof(NVNbufferRange) * (size_t)cnt); r->first = (int32_t)(a & 0xff); r->n = cnt;
        for (int i = 0; i < cnt; i++) { r->r[i].address = get64(&q); r->r[i].size = get32(&q); }
        break; }
    case OPC_VATTR: {
        int cnt = (int)(a & 0xff);
        p_bind_vattr *r = (p_bind_vattr *)xrec(out, &n, OP_BIND_VATTR, offsetof(p_bind_vattr, st) + 4 * (size_t)cnt); r->n = cnt; memcpy(r->st, q, 4 * (size_t)cnt);
        break; }
    case OPC_VSTREAM: {
        int cnt = (int)(a & 0xff);
        p_bind_vstream *r = (p_bind_vstream *)xrec(out, &n, OP_BIND_VSTREAM, offsetof(p_bind_vstream, st) + 8 * (size_t)cnt); r->n = cnt; memcpy(r->st, q, 8 * (size_t)cnt);
        break; }
    case OPC_UBO: {
        uint32_t st = a & 7, first = a >> 3 & 0xf, cnt = a >> 7 & 0x1f;
        for (uint32_t i = 0; i < cnt; i++) { uint64_t v = get64(&q); *(p_bind_ubo_small *)xrec(out, &n, OP_BIND_UBO_SMALL, sizeof(p_bind_ubo_small)) = (p_bind_ubo_small){(uint16_t)st, (uint16_t)(first + i), (uint32_t)(v >> 40), (v & ((UINT64_C(1) << 40) - 1)) << 8}; }
        break; }
    case OPC_STATE: case OPC_STATE8: {
        size_t k = (h.op & 0xff) == OPC_STATE ? 4 : 8;
        p_bind_state *r = (p_bind_state *)xrec(out, &n, OP_BIND_STATE, offsetof(p_bind_state, bytes) + k); r->kind = (int32_t)(a & 7);
        if (k == 4) { uint32_t v = a >> 3; memcpy(r->bytes, &v, 4); } else memcpy(r->bytes, q, 8);
        break; }
    case OPC_BARRIER: { int32_t b = (int32_t)a; memcpy(xrec(out, &n, OP_BARRIER, 8), &b, 4); break; }
    case OPC_PRIM_RESTART: *(p_prim_restart *)xrec(out, &n, OP_PRIM_RESTART, sizeof(p_prim_restart)) = (p_prim_restart){a & 1, a & 2 ? UINT32_MAX : a >> 2}; break;
    case OPC_SCISSOR: { uint16_t v[4]; memcpy(v, q, 8); *(p_scissor *)xrec(out, &n, OP_SCISSOR, sizeof(p_scissor)) = (p_scissor){v[0], v[1], v[2], v[3]}; break; }
    default: die("NVN command list: unknown compact record %#x", h.op);
    }
    return n;
}
static void copy_view(view_t *dst, uint8_t *has, const NVNtextureView *v) { *has = v != NULL; if (v) *dst = *(const view_t *)v; }
// Records snapshot their texture objects after the record struct: the game may finalize, free or reuse the object before submitting
#define SNAP_OFF(T) ((sizeof(T) + 7) & ~(size_t)7)
#define REC_SNAP(c, OP, T, K) ((T *)rec((c), (OP), SNAP_OFF(T) + (size_t)(K) * sizeof(tex_t)))
static const tex_t *tex_snap(void *rec_, size_t off, int k, const NVNtexture *t, uint64_t ra) {
    if (!t) return NULL;
    tex_t *s = (tex_t *)((uint8_t *)rec_ + off) + k;
    memcpy(s, t, sizeof *s);
    // invalid at record time (re-initialized before submit): keep the pointer and read it at submit; replay checks the magic
    if (s->magic != MAGIC('T', 'E', 'X', 0)) {
        static int red;
        if (red++ < 4) {   // log the call site and the object's start
            module_t *m = module_of((void *)ra); const uint64_t *q = (const uint64_t *)t;
            LOG("[RED] texture object invalid while recording (uninitialized or already finalized): %p index %d, call site %s+%#llx, object start %llx %llx %llx %llx %llx %llx; read again at submit", (const void *)t, k,
                m ? m->name : "?", m ? (unsigned long long)(ra - (uint64_t)m->base) : (unsigned long long)ra,
                (unsigned long long)q[0], (unsigned long long)q[1], (unsigned long long)q[2], (unsigned long long)q[3], (unsigned long long)q[4], (unsigned long long)q[5]);
        }
        return (const tex_t *)t;
    }
    return s;
}
NVN(nvnCommandBufferSetRenderTargets, void, NVNcommandBuffer *c, int n, const NVNtexture *const *ct, const NVNtextureView *const *cv,
    const NVNtexture *dt, const NVNtextureView *dv) {
    if (n < 0 || n > 8) { LOG("[RED] SetRenderTargets count %d", n); n = n < 0 ? 0 : 8; }
    p_set_rt *r = REC_SNAP(c, OP_SET_RT, p_set_rt, n + 1); r->n = n;
    for (int i = 0; i < n; i++) { r->color[i] = ct ? tex_snap(r, SNAP_OFF(p_set_rt), i, ct[i], (uint64_t)__builtin_return_address(0)) : NULL; copy_view(&r->cview[i], &r->has_cview[i], cv ? cv[i] : NULL); }
    r->depth = tex_snap(r, SNAP_OFF(p_set_rt), n, dt, (uint64_t)__builtin_return_address(0)); copy_view(&r->dview, &r->has_dview, dv);
    for (int i = 0; i < n; i++) if (r->color[i]) mtl_tex_gpu_written(r->color[i]);
    if (r->depth) mtl_tex_gpu_written(r->depth);
}
NVN(nvnCommandBufferClearColor, void, NVNcommandBuffer *c, int index, const float *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; memcpy(r->rgba, rgba, 16);
}
// integer clears recorded as floats, converted back by both replay paths: exact up to 2^24, 0xffffffff rounds to 2^32 and back
NVN(nvnCommandBufferClearColori, void, NVNcommandBuffer *c, int index, const int *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearColorui, void, NVNcommandBuffer *c, int index, const uint32_t *rgba, int mask) {
    p_clear_color *r = REC(c, OP_CLEAR_COLOR, p_clear_color); r->index = index; r->mask = mask; for (int i = 0; i < 4; i++) r->rgba[i] = (float)rgba[i];
}
NVN(nvnCommandBufferClearDepthStencil, void, NVNcommandBuffer *c, float depth, uint32_t dmask, int stencil, int smask) {
    p_clear_ds *r = REC(c, OP_CLEAR_DS, p_clear_ds); *r = (p_clear_ds){depth, (int)B(dmask), stencil, smask};
}
// the leading slots of [first, first + n) inside a stage's table, without forming first + n (overflow near INT_MAX); st 0 for state without a stage
static inline int slots_fit(int st, int first, int n, int slots) { return (unsigned)st >= NVN_RESOURCE_STAGES || first < 0 || n < 0 || first >= slots ? 0 : n < slots - first ? n : slots - first; }
// binds the slots inside its table, red line for the rest
static int slots_bound(const char *what, int st, int first, int n, int slots) {
    int k = slots_fit(st, first, n, slots);
    if (k != n) { static _Atomic int red; if (red++ < 16) LOG("[RED] %s stage %d first=%d n=%d: the host keeps %d slots a stage (%d resource stages), %d bound", what, st, first, n, slots, NVN_RESOURCE_STAGES, k); }
    return k;
}
NVN(nvnCommandBufferSetViewports, void, NVNcommandBuffer *c, int first, int n, const float *v) {
    if (slots_fit(0, first, n, 16) != n) { LOG("[RED] SetViewports first=%d n=%d", first, n); return; }
    if (g_rec_compact && first >= 0) { uint8_t *q = crec(c, OPC_VIEWPORTS, (uint32_t)first | (uint32_t)n << 8 | (uint32_t)(((dev_t_ *)CB(c)->dev)->origin + 1) << 16, 16 * (size_t)n); memcpy(q, v, 16 * (size_t)n); return; }
    p_viewports *r = REC(c, OP_VIEWPORTS, p_viewports); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) memcpy(r->v[i], v + 4 * i, 16);
    r->origin = ((dev_t_ *)CB(c)->dev)->origin + 1;
}
NVN(nvnCommandBufferSetScissor, void, NVNcommandBuffer *c, int x, int y, int w, int h) {
    if (g_rec_compact && (unsigned)x < 0x10000 && (unsigned)y < 0x10000 && (unsigned)w < 0x10000 && (unsigned)h < 0x10000) {
        uint16_t v[4] = {(uint16_t)x, (uint16_t)y, (uint16_t)w, (uint16_t)h}; memcpy(crec(c, OPC_SCISSOR, 0, 8), v, 8); return;
    }
    *REC(c, OP_SCISSOR, p_scissor) = (p_scissor){x, y, w, h};
}
NVN(nvnCommandBufferSetDepthRange, void, NVNcommandBuffer *c, float n, float f) { *REC(c, OP_DEPTH_RANGE, p_depth_range) = (p_depth_range){n, f}; }
NVN(nvnCommandBufferBindProgram, void, NVNcommandBuffer *c, const NVNprogram *p, int stages) {
    *REC(c, OP_BIND_PROGRAM, p_bind_program) = (p_bind_program){(const prog_t *)p, stages};
}
static void bind_state(NVNcommandBuffer *c, int kind, const void *st, size_t n) {
    if (g_rec_compact && (unsigned)kind < 8 && n == 4) { uint32_t v; memcpy(&v, st, 4); if (v < (1u << 21)) { crec(c, OPC_STATE, (uint32_t)kind | v << 3, 0); return; } }
    if (g_rec_compact && (unsigned)kind < 8 && n == 8) { memcpy(crec(c, OPC_STATE8, (uint32_t)kind, 8), st, 8); return; }
    p_bind_state *r = rec(c, OP_BIND_STATE, offsetof(p_bind_state, bytes) + n); r->kind = kind; memcpy(r->bytes, st, n); }   // only the state's own bytes
NVN(nvnCommandBufferBindBlendState, void, NVNcommandBuffer *c, const NVNblendState *st) { bind_state(c, 0, st, sizeof(blend_st)); }
NVN(nvnCommandBufferBindColorState, void, NVNcommandBuffer *c, const NVNcolorState *st) { bind_state(c, 1, st, sizeof(color_st)); }
NVN(nvnCommandBufferBindChannelMaskState, void, NVNcommandBuffer *c, const NVNchannelMaskState *st) { bind_state(c, 2, st, sizeof(chmask_st)); }
NVN(nvnCommandBufferBindMultisampleState, void, NVNcommandBuffer *c, const NVNmultisampleState *st) { bind_state(c, 3, st, sizeof(ms_st)); }
NVN(nvnCommandBufferBindPolygonState, void, NVNcommandBuffer *c, const NVNpolygonState *st) { bind_state(c, 4, st, sizeof(poly_st)); }
NVN(nvnCommandBufferBindDepthStencilState, void, NVNcommandBuffer *c, const NVNdepthStencilState *st) { bind_state(c, 5, st, sizeof(ds_st)); }
NVN(nvnCommandBufferBindVertexAttribState, void, NVNcommandBuffer *c, int n, const NVNvertexAttribState *st) {
    // 32 on NVN2: a refused bind would leave draws without vertex layouts
    if (n < 0 || n > 32) { LOG("[RED] BindVertexAttribState n=%d", n); return; }
    if (g_rec_compact) { memcpy(crec(c, OPC_VATTR, (uint32_t)n, 4 * (size_t)n), st, 4 * (size_t)n); return; }
    p_bind_vattr *r = RECN(c, OP_BIND_VATTR, p_bind_vattr, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 4 * n);
}
NVN(nvnCommandBufferBindVertexStreamState, void, NVNcommandBuffer *c, int n, const NVNvertexStreamState *st) {
    if (n < 0 || n > 16) { LOG("[RED] BindVertexStreamState n=%d", n); return; }
    if (g_rec_compact) { memcpy(crec(c, OPC_VSTREAM, (uint32_t)n, 8 * (size_t)n), st, 8 * (size_t)n); return; }
    p_bind_vstream *r = RECN(c, OP_BIND_VSTREAM, p_bind_vstream, st, n); r->first = 0; r->n = n; memcpy(r->st, st, 8 * n);
}
NVN(nvnCommandBufferBindVertexBuffers, void, NVNcommandBuffer *c, int first, int n, const NVNbufferRange *rg) {
    // games clear streams 16 to 31 with a null range: only a real range past NVN's 16 is wrong
    if (first >= 16 && n == 1 && !rg[0].address && !rg[0].size) return;
    if (slots_fit(0, first, n, 16) != n) { static int red; if (red++ < 8) LOG("[RED] BindVertexBuffers first=%d n=%d", first, n); return; }
    if (n == 1 && (unsigned)first < 16 && rg[0].size <= UINT32_MAX) {
        *REC(c, OP_BIND_VBUF1, p_bind_vbuf1) = (p_bind_vbuf1){(uint32_t)first, (uint32_t)rg[0].size, rg[0].address}; return;
    }
    int fit = g_rec_compact && first >= 0; for (int i = 0; i < n && fit; i++) fit = rg[i].size <= UINT32_MAX;
    if (fit) { uint8_t *q = crec(c, OPC_VBUFS, (uint32_t)first | (uint32_t)n << 8, 12 * (size_t)n); for (int i = 0; i < n; i++) { put64(&q, rg[i].address); put32(&q, (uint32_t)rg[i].size); } return; }
    p_bind_vbuf *r = RECN(c, OP_BIND_VBUF, p_bind_vbuf, r, n); r->first = first; r->n = n; memcpy(r->r, rg, sizeof *rg * n);
}
// NVN2's task and mesh stages (6, 7) use host stages 3 and 4; ray tracing (8) has no table and every bind refuses it in red. Only public entry points normalize.
static inline NVNshaderStage st_host(NVNshaderStage st) { return (unsigned)st == 6 ? 3 : (unsigned)st == 7 ? 4 : st; }
// 256-aligned address and size below 2^18: 8 bytes each after one header
static int rec_ubo(NVNcommandBuffer *c, int st, int first, int n, const NVNbufferRange *rg) {
    if (!g_rec_compact || (unsigned)st >= NVN_RESOURCE_STAGES || first < 0 || n < 1 || first >= 16 || n > 16 - first) return 0;
    for (int i = 0; i < n; i++) if ((rg[i].address & 0xff) || rg[i].address >> 48 || rg[i].size >= (1u << 18)) return 0;
    uint8_t *q = crec(c, OPC_UBO, (uint32_t)st | (uint32_t)first << 3 | (uint32_t)n << 7, 8 * (size_t)n);
    for (int i = 0; i < n; i++) put64(&q, rg[i].address >> 8 | (uint64_t)rg[i].size << 40);
    return 1;
}
static void record_uniform_buffer(NVNcommandBuffer *c, int st, int i, NVNbufferAddress a, size_t n) {
    NVNbufferRange r = {a, n}; if (rec_ubo(c, st, i, 1, &r)) return;
    if (n <= UINT32_MAX) {
        *REC(c, OP_BIND_UBO_SMALL, p_bind_ubo_small) = (p_bind_ubo_small){(uint16_t)st, (uint16_t)i, (uint32_t)n, a}; return;
    }
    *REC(c, OP_BIND_UBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindUniformBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) { st = st_host(st);
    if (slots_bound("BindUniformBuffer", st, i, 1, 16)) record_uniform_buffer(c, st, i, a, n);
}
// when each fits its canonical record's compact form
static int rec_bufs(NVNcommandBuffer *c, uint32_t op, int st, int first, int n, const NVNbufferRange *rg, int slots) {
    if (!g_rec_compact || n < 1 || n > 63 || (unsigned)st >= NVN_RESOURCE_STAGES || first < 0 || first > 0xff || first >= slots || n > slots - first) return 0;
    for (int i = 0; i < n; i++) if (rg[i].size > UINT32_MAX) return 0;
    uint8_t *q = crec(c, OPC_BUFS, op | (uint32_t)st << 6 | (uint32_t)first << 9 | (uint32_t)n << 17, 12 * (size_t)n);
    for (int i = 0; i < n; i++) { put64(&q, rg[i].address); put32(&q, (uint32_t)rg[i].size); }
    return 1;
}
NVN(nvnCommandBufferBindUniformBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) { st = st_host(st);
    n = slots_bound("BindUniformBuffers", st, first, n, 16);
    if (n >= 2 && (rec_ubo(c, st, first, n, rg) || rec_bufs(c, OP_BIND_UBO_SMALL, st, first, n, rg, 16))) return;
    for (int i = 0; i < n; i++) record_uniform_buffer(c, st, first + i, rg[i].address, rg[i].size);
}
NVN(nvnCommandBufferBindStorageBuffer, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNbufferAddress a, size_t n) { st = st_host(st);
    if (!slots_bound("BindStorageBuffer", st, i, 1, 16)) return;   // 16 storage buffer slots a stage (NVN's limit)
    NVNbufferRange r = {a, n}; if (rec_bufs(c, OP_BIND_SSBO, st, i, 1, &r, 256)) return;
    *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, i, a, n};
}
NVN(nvnCommandBufferBindStorageBuffers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNbufferRange *rg) { st = st_host(st);
    n = slots_bound("BindStorageBuffers", st, first, n, 16);
    if (rec_bufs(c, OP_BIND_SSBO, st, first, n, rg, 256)) return;
    for (int i = 0; i < n; i++) *REC(c, OP_BIND_SSBO, p_bind_buf) = (p_bind_buf){st, first + i, rg[i].address, rg[i].size};
}
// compact: one OPC_HANDLES record of final values; BindTexturesDeferred edits its record afterwards, so it takes the canonical one
static p_bind_handles *bind_handles_ex(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h, int compact) {
    if (n < 0 || n > 32) { LOG("[RED] bind handle count %d", n); return NULL; }   // a record holds 32
    // textures 0-255 (separate texture r at 128 + r), samplers 0-127, images 0-31
    n = slots_bound(op == OP_BIND_SAMP ? "BindSamplers" : op == OP_BIND_IMG ? "BindImages" : "BindTextures", st, first, n,
                    op == OP_BIND_SAMP ? DMSH_C2_SEP_SLOTS : op == OP_BIND_IMG ? 32 : NVN_SEPTEX_SLOT0 + DMSH_C2_SEP_SLOTS);
    if (!n) return NULL;
    p_bind_handles tmp, *r;
    if (compact && g_rec_compact && (unsigned)st < NVN_RESOURCE_STAGES && first >= 0 && first <= 0xff && n >= 1) r = &tmp;
    else r = RECN(c, op, p_bind_handles, h, n);
    r->stage = st; r->first = first; r->n = n; r->defer = 0; r->src = 0;
    // The driver writes each handle into the c2 table as a 32-bit word (texture ID bits 0-19, sampler ID 20-31): the GPU never sees the upper half
    for (int i = 0; i < n; i++) r->h[i] = (uint32_t)h[i];
    // indices belong to the pools active at execution, inherited ones included: keep the hardware's relative handles
    if (r == &tmp) {
        uint8_t *q = crec(c, OPC_HANDLES, (uint32_t)op | (uint32_t)st << 6 | (uint32_t)first << 9 | (uint32_t)n << 17, 4 * (size_t)n);
        for (int i = 0; i < n; i++) put32(&q, (uint32_t)r->h[i]);
        return NULL;
    }
    return r;
}
static p_bind_handles *bind_handles(NVNcommandBuffer *c, int op, int st, int first, int n, const uint64_t *h) { return bind_handles_ex(c, op, st, first, n, h, 1); }
// image handle = texture pool index
NVN(nvnCommandBufferBindImages, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNimageHandle *h) { st = st_host(st);
    bind_handles(c, OP_BIND_IMG, st, first, n, (const uint64_t *)h);
}
// separate textures fill their own handle table on hardware (c2 0x168 + 8 slot): slots 128 + n
NVN(nvnCommandBufferBindSeparateTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateTextureHandle *h) { st = st_host(st);
    bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateTextureHandle h) { st = st_host(st); uint64_t v; memcpy(&v, &h, 8); bind_handles(c, OP_BIND_TEX, st, NVN_SEPTEX_SLOT0 + i, 1, &v); }
NVN(nvnCommandBufferBindSeparateSamplers, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNseparateSamplerHandle *h) { st = st_host(st);
    bind_handles(c, OP_BIND_SAMP, st, first, n, (const uint64_t *)h);
}
NVN(nvnCommandBufferBindSeparateSampler, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNseparateSamplerHandle h) { st = st_host(st);
    bind_handles(c, OP_BIND_SAMP, st, i, 1, &h.value);
}
// which: 0 value mask, 1 write mask, 2 reference value
static void rec_stencil(NVNcommandBuffer *c, NVNface f, int which, int v) {
    if (g_rec_compact && (unsigned)f < 4 && (unsigned)v < (1u << 20)) { crec(c, OPC_STENCIL, (uint32_t)f | (uint32_t)which << 2 | (uint32_t)v << 4, 0); return; }
    *REC(c, OP_STENCIL, p_stencil) = (p_stencil){f, which == 0 ? v : 0, which == 1 ? v : 0, which == 2 ? v : 0, which};
}
NVN(nvnCommandBufferSetStencilValueMask, void, NVNcommandBuffer *c, NVNface f, int m) { rec_stencil(c, f, 0, m); }
NVN(nvnCommandBufferSetStencilMask, void, NVNcommandBuffer *c, NVNface f, int m) { rec_stencil(c, f, 1, m); }
NVN(nvnCommandBufferSetStencilRef, void, NVNcommandBuffer *c, NVNface f, int v) { rec_stencil(c, f, 2, v); }
NVN(nvnCommandBufferSetPolygonOffsetClamp, void, NVNcommandBuffer *c, float fa, float un, float cl) {
    *REC(c, OP_POLY_OFFSET, p_poly_offset) = (p_poly_offset){fa, un, cl};
}
NVN(nvnCommandBufferDrawArrays, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n) { rec_draw(c, (p_draw){pr, first, n, 1, 0, 0}); }
// Indirect: the record at ind is usually GPU-written earlier in the frame, so the replay draws indirectly from the pool buffer.
// NVN2 mesh shading: task workgroups from first, or mesh workgroups without a task shader
NVN_NOPROTO(nvnCommandBufferDrawMeshTasks, void, NVNcommandBuffer *c, int first, int count) { rec_draw(c, (p_draw){DM_PRIM_MESH_TASKS, first, count, 1, 0, 0}); }
// the record {count, first} is GPU-written earlier in the frame (zeros at record time)
NVN_NOPROTO(nvnCommandBufferDrawMeshTasksIndirect, void, NVNcommandBuffer *c, NVNbufferAddress ind) { rec_draw(c, (p_draw){DM_PRIM_MESH_TASKS, 0, 0, 1, 0, ind}); }
// Variable rate shading has no per-tile Metal equivalent, and shading every pixel is the most a rate asks: nothing is recorded
NVN_NOPROTO(nvnCommandBufferBindShadingRateImage, void, NVNcommandBuffer *c, const void *view, const void *tex) { (void)c; (void)view; (void)tex; }
NVN_NOPROTO(nvnCommandBufferShadingRateImagePalette, void, NVNcommandBuffer *c, int first, int n, const void *entries) { (void)c; (void)first; (void)n; (void)entries; }
NVN_NOPROTO(nvnCommandBufferSetShadingRatePaletteMode, void, NVNcommandBuffer *c, int mode) { (void)c; (void)mode; }
NVN(nvnCommandBufferDrawArraysIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind) { rec_draw(c, (p_draw){pr, 0, 0, 1, 0, ind}); }
NVN(nvnCommandBufferDrawElementsInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib,
    int bv, int bi, int inst) {
    if (!inst) return;   // no instance: nothing is drawn (a record's 0 instances counts as one)
    rec_draw_idx(c, (p_draw_idx){pr, it, n, bv, bi, inst, ib});
}
NVN(nvnCommandBufferDispatchCompute, void, NVNcommandBuffer *c, int x, int y, int z) {
    // the thunk tail-jumps, so the return address is the game's call site
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){x, y, z, (uint64_t)__builtin_return_address(0)};
}
// counts read by the GPU when the dispatch runs
NVN(nvnCommandBufferDispatchComputeIndirect, void, NVNcommandBuffer *c, NVNbufferAddress ind) {
    *REC(c, OP_DISPATCH, p_dispatch) = (p_dispatch){0, 0, 0, (uint64_t)__builtin_return_address(0), ind};
}
NVN(nvnCommandBufferBarrier, void, NVNcommandBuffer *c, int bits) {
    if (g_rec_compact && (unsigned)bits < (1u << 24)) { crec(c, OPC_BARRIER, (uint32_t)bits, 0); return; }
    *(int32_t *)rec(c, OP_BARRIER, 8) = bits;
}
NVN(nvnCommandBufferSetTexturePool, void, NVNcommandBuffer *c, const NVNtexturePool *p) { REC(c, OP_SET_TEX_POOL, p_set_pool)->pool = (const dpool_t *)p; }
NVN(nvnCommandBufferSetSamplerPool, void, NVNcommandBuffer *c, const NVNsamplerPool *p) { REC(c, OP_SET_SAMP_POOL, p_set_pool)->pool = (const dpool_t *)p; }

// the record, its texture snapshot, then the copy strides when set
static p_copy_b2t *rec_copy(NVNcommandBuffer *c, uint32_t op) {
    const cmd_t *o = CB(c); int st = o->copy_row || o->copy_image;
    p_copy_b2t *r = (p_copy_b2t *)rec(c, op, COPY_STRIDES_OFF + (st ? sizeof(p_copy_strides) : 0));
    if (st) { p_copy_strides cs = { o->copy_row, o->copy_image }; memcpy((uint8_t *)r + COPY_STRIDES_OFF, &cs, sizeof cs); }
    return r;
}
NVN(nvnCommandBufferCopyBufferToTexture, void, NVNcommandBuffer *c, NVNbufferAddress src, const NVNtexture *t, const NVNtextureView *v,
    const NVNcopyRegion *r, int flags) {
    p_copy_b2t *o = rec_copy(c, OP_COPY_B2T); o->src = src; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
    if (o->tex) mtl_tex_gpu_written(o->tex);
}
NVN(nvnCommandBufferCopyTextureToBuffer, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r,
    NVNbufferAddress dst, int flags) {
    p_copy_b2t *o = rec_copy(c, OP_COPY_T2B); o->src = dst; o->tex = tex_snap(o, SNAP_OFF(p_copy_b2t), 0, t, (uint64_t)__builtin_return_address(0)); copy_view(&o->view, &o->has_view, v); o->r = *r; o->flags = flags;
}
NVN(nvnCommandBufferClearTexture, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const float *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); if (o->tex) mtl_tex_gpu_written(o->tex); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 0; memcpy(o->c.f, rgba, 16);
}
NVN(nvnCommandBufferClearTextureui, void, NVNcommandBuffer *c, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const uint32_t *rgba, int mask) {
    p_clear_tex *o = REC_SNAP(c, OP_CLEAR_TEX, p_clear_tex, 1); o->tex = tex_snap(o, SNAP_OFF(p_clear_tex), 0, t, (uint64_t)__builtin_return_address(0)); if (o->tex) mtl_tex_gpu_written(o->tex); copy_view(&o->view, &o->has_view, v); o->r = *r; o->mask = mask; o->is_uint = 1; memcpy(o->c.u, rgba, 16);
}
NVN(nvnCommandBufferCopyTextureToTexture, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNcopyRegion *sr,
    const NVNtexture *d, const NVNtextureView *dv, const NVNcopyRegion *dr, int flags) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2); o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, (uint64_t)__builtin_return_address(0)); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, (uint64_t)__builtin_return_address(0));
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv); o->sr = *sr; o->dr = *dr; o->flags = flags;
    if (o->dst) mtl_tex_gpu_written(o->dst);
}
// replayed as draw_tex with the pools active at execution; only the handle's lower 32 bits reach the GPU
NVN(nvnCommandBufferDrawTexture, void, NVNcommandBuffer *c, NVNtextureHandle h, const NVNdrawTextureRegion *d, const NVNdrawTextureRegion *s) {
    *REC(c, OP_DRAW_TEX, p_draw_tex) = (p_draw_tex){(uint32_t)h, *d, *s};
}
// command buffer state, as in NVN: no command memory; each later copy carries them
NVN(nvnCommandBufferSetCopyRowStride, void, NVNcommandBuffer *c, ptrdiff_t st) { CB(c)->copy_row = st; }
NVN(nvnCommandBufferSetCopyImageStride, void, NVNcommandBuffer *c, ptrdiff_t st) { CB(c)->copy_image = st; }
NVN(nvnCommandBufferGetCopyRowStride, ptrdiff_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->copy_row; }
NVN(nvnCommandBufferGetCopyImageStride, ptrdiff_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->copy_image; }
NVN(nvnCommandBufferSetBlendColor, void, NVNcommandBuffer *c, const float *v) { memcpy(REC(c, OP_BLEND_COLOR, p_blend_color)->rgba, v, 16); }

#define ST(T, x) ((T *)(x))
NVN(nvnBlendStateSetDefaults, void, NVNblendState *s) { *ST(blend_st, s) = (blend_st){0, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD}; }
NVN(nvnBlendStateSetBlendTarget, void, NVNblendState *s, int t) { ST(blend_st, s)->target = t; }
NVN(nvnBlendStateSetBlendFunc, void, NVNblendState *s, NVNblendFunc sr, NVNblendFunc dr, NVNblendFunc sa, NVNblendFunc da) {
    blend_st *o = ST(blend_st, s); o->src_rgb = sr; o->dst_rgb = dr; o->src_a = sa; o->dst_a = da;
}
NVN(nvnBlendStateSetBlendEquation, void, NVNblendState *s, NVNblendEquation er, NVNblendEquation ea) { ST(blend_st, s)->eq_rgb = er; ST(blend_st, s)->eq_a = ea; }
// Advanced blend: Porter-Duff modes replay as the fixed-function factors they amount to (adv_blend); multiply, screen, overlay etc. have no Metal blend state
NVN(nvnBlendStateSetAdvancedMode, void, NVNblendState *s, NVNblendAdvancedMode mode) { ST(blend_st, s)->adv = (unsigned)mode <= NVN_BLEND_ADVANCED_MODE_HSL_LUMINOSITY ? mode : 0; }
NVN(nvnBlendStateSetAdvancedPremultipliedSrc, void, NVNblendState *s, uint32_t pm) { ST(blend_st, s)->adv_npm = !B(pm); }
NVN(nvnBlendStateSetAdvancedNormalizedDst, void, NVNblendState *s, uint32_t n) { ST(blend_st, s)->adv_dst_free = !B(n); }   // kept for the getter; the replay's factors read a normalized target as it is
NVN(nvnBlendStateSetAdvancedOverlap, void, NVNblendState *s, NVNblendAdvancedOverlap o) { ST(blend_st, s)->adv_ovl = (unsigned)o & 3; }
NVN(nvnColorStateSetDefaults, void, NVNcolorState *s) { *ST(color_st, s) = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; }
NVN(nvnColorStateSetBlendEnable, void, NVNcolorState *s, int i, uint32_t en) {
    color_st *o = ST(color_st, s); if (i < 0 || i > 7) return;
    o->blend_en = B(en) ? (o->blend_en | 1u << i) : (o->blend_en & ~(1u << i));
}
NVN(nvnChannelMaskStateSetDefaults, void, NVNchannelMaskState *s) { ST(chmask_st, s)->mask = 0xffffffffu; }
NVN(nvnChannelMaskStateSetChannelMask, void, NVNchannelMaskState *s, int i, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    if (i < 0 || i > 7) return;
    uint32_t m = (B(r) ? 1 : 0) | (B(g) ? 2 : 0) | (B(b) ? 4 : 0) | (B(a) ? 8 : 0);
    chmask_st *o = ST(chmask_st, s); o->mask = (o->mask & ~(0xfu << (4 * i))) | m << (4 * i);
}
NVN(nvnMultisampleStateSetDefaults, void, NVNmultisampleState *s) { memset(s, 0, sizeof(ms_st)); ST(ms_st, s)->enable = ST(ms_st, s)->a2c_dither = 1; }
NVN(nvnMultisampleStateSetMultisampleEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->enable = B(e) != 0; }
NVN(nvnMultisampleStateSetSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetAlphaToCoverageEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocationsEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_en = B(e) != 0; }
NVN(nvnMultisampleStateSetSampleLocations, void, NVNmultisampleState *s, int start, int n, const float *v) { (void)s; (void)start; (void)n; (void)v; }
NVN(nvnMultisampleStateGetSamples, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->samples; }
NVN(nvnMultisampleStateGetMultisampleEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->enable; }
NVN(nvnPolygonStateSetDefaults, void, NVNpolygonState *s) { *ST(poly_st, s) = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0}; }
NVN(nvnPolygonStateSetCullFace, void, NVNpolygonState *s, NVNface f) { ST(poly_st, s)->cull = f; }
NVN(nvnPolygonStateSetFrontFace, void, NVNpolygonState *s, NVNfrontFace f) { ST(poly_st, s)->front = f; }
NVN(nvnPolygonStateSetPolygonMode, void, NVNpolygonState *s, NVNpolygonMode m) { ST(poly_st, s)->mode = m; }
NVN(nvnPolygonStateSetPolygonOffsetEnables, void, NVNpolygonState *s, int e) { ST(poly_st, s)->offset_en = e; }
NVN(nvnDepthStencilStateSetDefaults, void, NVNdepthStencilState *s) {
    *ST(ds_st, s) = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS,
                            NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP, NVN_STENCIL_OP_KEEP};
}
NVN(nvnDepthStencilStateSetDepthTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dt = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthWriteEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->dw = B(e) != 0; }
NVN(nvnDepthStencilStateSetDepthFunc, void, NVNdepthStencilState *s, NVNdepthFunc f) { ST(ds_st, s)->df = f; }
NVN(nvnDepthStencilStateSetStencilTestEnable, void, NVNdepthStencilState *s, uint32_t e) { ST(ds_st, s)->st = B(e) != 0; }
NVN(nvnDepthStencilStateSetStencilFunc, void, NVNdepthStencilState *s, NVNface f, NVNstencilFunc fn) {
    ds_st *o = ST(ds_st, s); if (f & NVN_FACE_FRONT) o->sf_f = fn; if (f & NVN_FACE_BACK) o->sf_b = fn;
}
NVN(nvnDepthStencilStateSetStencilOp, void, NVNdepthStencilState *s, NVNface f, NVNstencilOp fail, NVNstencilOp zfail, NVNstencilOp zpass) {
    ds_st *o = ST(ds_st, s);
    if (f & NVN_FACE_FRONT) { o->fail_f = fail; o->zfail_f = zfail; o->zpass_f = zpass; }
    if (f & NVN_FACE_BACK) { o->fail_b = fail; o->zfail_b = zfail; o->zpass_b = zpass; }
}
NVN(nvnVertexAttribStateSetDefaults, void, NVNvertexAttribState *s) { *ST(va_st, s) = (va_st){0, 0, 0}; }
NVN(nvnVertexAttribStateSetFormat, void, NVNvertexAttribState *s, NVNformat f, ptrdiff_t off) {
    if (off < 0 || off >= (1 << 19)) LOG("[RED] vertex attribute offset %td exceeds the packed range", off);
    ST(va_st, s)->fmt = f; ST(va_st, s)->offset = (uint32_t)off;
}
NVN(nvnVertexAttribStateSetStreamIndex, void, NVNvertexAttribState *s, int i) { ST(va_st, s)->stream = i; }
NVN(nvnVertexStreamStateSetDefaults, void, NVNvertexStreamState *s) { *ST(vs_st, s) = (vs_st){0, 0}; }
NVN(nvnVertexStreamStateSetStride, void, NVNvertexStreamState *s, ptrdiff_t st) { ST(vs_st, s)->stride = (uint32_t)st; }
NVN(nvnVertexStreamStateSetDivisor, void, NVNvertexStreamState *s, int d) { ST(vs_st, s)->divisor = d; }
NVN(nvnVertexStreamStateGetStride, ptrdiff_t, const NVNvertexStreamState *s) { return (ptrdiff_t)((const vs_st *)s)->stride; }
NVN(nvnVertexStreamStateGetDivisor, int, const NVNvertexStreamState *s) { return ((const vs_st *)s)->divisor; }
NVN(nvnVertexAttribStateGetFormat, void, const NVNvertexAttribState *s, NVNformat *f, ptrdiff_t *off) { const va_st *o = (const va_st *)s; if (f) *f = (NVNformat)o->fmt; if (off) *off = (ptrdiff_t)o->offset; }
NVN(nvnVertexAttribStateGetStreamIndex, int, const NVNvertexAttribState *s) { return ((const va_st *)s)->stream; }

static _Atomic uint64_t g_sub_wall_ns, g_sub_cpu_ns, g_sub_instr;   // submit wall / thread CPU / retired instructions this frame, excluding present
static _Atomic uint64_t g_cum_sub_instr, g_cum_sub_cpu_ns;   // cumulative, for the translation summary row
extern int thread_selfcounts(int type, void *buf, size_t nbytes);   // {instructions, cycles}: libSystem private interface
static void thr_counts(uint64_t *instr, uint64_t *cyc) { uint64_t c[2] = {0, 0}; thread_selfcounts(1, c, sizeof c); *instr = c[0]; *cyc = c[1]; }
// the thread's own port from pthread: mach_thread_self costs two more kernel calls, and this runs thousands of times a second
static uint64_t thr_sys_ns(void) {
    thread_basic_info_data_t bi; mach_msg_type_number_t n = THREAD_BASIC_INFO_COUNT; uint64_t r = 0;
    if (thread_info(pthread_mach_thread_np(pthread_self()), THREAD_BASIC_INFO, (thread_info_t)&bi, &n) == KERN_SUCCESS) r = (uint64_t)bi.system_time.seconds * 1000000000ull + (uint64_t)bi.system_time.microseconds * 1000ull;
    return r;
}
static _Atomic uint64_t g_cum_sub_cyc, g_cum_sub_sys_ns, g_cum_sub_wall_ns;
static int frame_rec(void);
static _Atomic uint64_t g_nvn_ns;   // host time in submit / present this frame

// dm_gfx_suspend(1): later submits / presents block until resumed; returns once calls in progress have left and in-flight GPU work has completed
static pthread_mutex_t g_susp_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_susp_cv = PTHREAD_COND_INITIALIZER;
static int g_susp, g_inflight_api;
__thread _Atomic int dm_host_work;   // defined here: the graphics unit tests compile this file but not hle_thread.c
static void gfx_enter(void) { pthread_mutex_lock(&g_susp_mu); while (g_susp) pthread_cond_wait(&g_susp_cv, &g_susp_mu); g_inflight_api++; pthread_mutex_unlock(&g_susp_mu); dm_host_work++; }
static void gfx_leave(void) { dm_host_work--; pthread_mutex_lock(&g_susp_mu); g_inflight_api--; pthread_cond_broadcast(&g_susp_cv); pthread_mutex_unlock(&g_susp_mu); }
void dm_gfx_enter(void) { gfx_enter(); }
void dm_gfx_suspend(int on) {
    pthread_mutex_lock(&g_susp_mu);
    g_susp = on != 0;
    if (on) mtl_hold_drain();   // a submit thread stopped at held work is in flight: its hold ends first
    if (on) while (g_inflight_api) pthread_cond_wait(&g_susp_cv, &g_susp_mu);
    else pthread_cond_broadcast(&g_susp_cv);
    pthread_mutex_unlock(&g_susp_mu);
    if (on) { mtl_drain(); mtl_arch_save(0, 0); LOG("[nvn] graphics suspended: in-flight GPU work drained"); } else { dm_fstep_settle(); LOG("[nvn] graphics resumed"); }
    // the archive starts writing: a background process may be ended without a word
}
void dm_gfx_quit(int wait_ms) { mtl_arch_save(wait_ms, 1); }
void dm_gfx_quit_begin(void) { mtl_warm_stop(); mtl_arch_save(0, 0); }   // no more prewarm compiles; the archive writes beside the drain, which can wait on a draw that compiles
_Static_assert(DM_QUIT_ARCH_MS == MTL_QUIT_ARCH_MS, "a graceful exit gives the pipeline archive the time a quit does");
// One submission at a time: one command buffer and upload batch (g_cb), and games may submit from two threads. The first prepare step runs under the lock
// but outside the graphics gate, so a suspend does not wait for it; a submission waiting for the lock is not in flight.
#include "nvn_queue.inc"
NVN(nvnQueueSubmitCommands, void, NVNqueue *q, int n, const NVNcommandHandle *h) {
    ((queue_t *)q)->submits++;
    nvn_queue_submit((queue_t *)q,n,(const uint64_t *)h);
}

// GPU-only commands with nothing to do here (the thunk has counted them)
static void nvn_nop(void) {}
static const char *const g_nops[] = {
    "nvnCommandBufferSetDepthBounds",
    "nvnCommandBufferSetShaderScratchMemory",
    "nvnCommandBufferPushDebugGroupDynamic",
    "nvnCommandBufferPopDebugGroup",
    "nvnCommandBufferInsertDebugMarkerDynamic",
    "nvnCommandBufferInsertDebugMarker",
    "nvnCommandBufferPushDebugGroup",      // NVN 53
    "nvnCommandBufferPushDebugGroupStatic",
    "nvnCommandBufferPopDebugGroupId",
    "nvnCommandBufferInsertDebugMarkerStatic",
};
__attribute__((constructor)) static void nvn_nops_register(void) {
    // a misspelled name (not in main's name table) aborts at startup
    nvn_init_slots();
    for (unsigned k = 0; k < sizeof g_nops / sizeof *g_nops; k++) {
        unsigned i = 0;
        while (i < NVN_NFUNC && strcmp(nvn_names[i], g_nops[k])) i++;
        if (i == NVN_NFUNC) die("NVN empty implementation %s is not in main's name table", g_nops[k]);
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry) die("NVN %s has an implementation and is also listed as an empty implementation", g_nops[k]);
        nvn_slot[i].fn = (void *)nvn_nop;
    }
    unsigned nnop = sizeof g_nops / sizeof *g_nops;
    // Implemented entries except command recording run on the big host stack: they do Metal work, and a game thread's stack may be too small
    unsigned nbig = 0;
    for (unsigned i = 0; i < NVN_NFUNC; i++)
        if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && nvn_slot[i].fn != (void *)nvn_nop && strncmp(nvn_names[i], "nvnCommandBuffer", 16)) {
            nvn_big_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_big_entry; nbig++;
        }
    // DM_NVN_PROF=1: timing wrappers on implemented functions, present excepted (its display-link wait is counted apart)
    if (getenv("DM_NVN_PROF"))
        for (unsigned i = 0; i < NVN_NFUNC; i++)
            if (nvn_slot[i].fn != (void *)nvn_unimpl_entry && strcmp(nvn_names[i], "nvnQueuePresentTexture")) { nvn_real[i] = nvn_slot[i].fn; nvn_slot[i].fn = (void *)nvn_prof_entry; }
    LOG("[nvn] null device: %u names, %u implemented (%u semantic + %u empty, %u of them on the large host stack), the other %u go to the red-reporting stub",
        NVN_NFUNC, g_nsem + nnop, g_nsem, nnop, nbig, NVN_NFUNC - g_nsem - nnop);
}

#define WB(b) ((win_builder_t *)(b))
NVN(nvnWindowBuilderSetDefaults, void, NVNwindowBuilder *b) { NVNdevice *dev = WB(b)->dev; memset(b, 0, sizeof(win_builder_t)); WB(b)->dev = dev; WB(b)->interval = 1; }
NVN(nvnWindowBuilderSetDevice, void, NVNwindowBuilder *b, NVNdevice *d) { WB(b)->dev = d; }
NVN(nvnWindowBuilderSetNativeWindow, void, NVNwindowBuilder *b, NVNnativeWindow w) { WB(b)->native = w; }
NVN(nvnWindowBuilderSetTextures, void, NVNwindowBuilder *b, int n, NVNtexture *const *t) {
    if (n < 0 || n > 4) { LOG("[RED] WindowBuilderSetTextures count %d", n); n = n < 0 ? 0 : 4; }
    WB(b)->ntex = n; for (int i = 0; i < n; i++) WB(b)->tex[i] = t[i];
}
NVN(nvnWindowBuilderSetPresentInterval, void, NVNwindowBuilder *b, int v) { WB(b)->interval = v; }
NVN(nvnWindowBuilderGetPresentInterval, int, const NVNwindowBuilder *b) { return WB(b)->interval; }
// the first nactive (SetNumActiveTextures), or all: fewer active textures keep fewer frames queued
static int window_active(const win_t *o) { return o->nactive > 0 && o->nactive < o->ntex ? o->nactive : o->ntex; }
NVN(nvnWindowInitialize, NVNboolean, NVNwindow *w, const NVNwindowBuilder *b) {
    const win_builder_t *s = (const win_builder_t *)b; win_t *o = (win_t *)w;
    memset(o, 0, sizeof *o); o->magic = MAGIC('W', 'I', 'N', 'D'); o->native = s->native; o->ntex = s->ntex; o->interval = s->interval; o->nactive = s->nactive;
    memcpy(o->tex, s->tex, sizeof o->tex);
    LOG("[nvn] WindowInitialize swap chain %d textures (%d active), interval %d, first %dx%d format %#x", o->ntex, window_active(o), o->interval,
        o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0, o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0, o->ntex && o->tex[0] ? CTX(o->tex[0])->d.format : 0);
    live_add(LV_WIN, w);
    return 1;
}
static int window_interval(const win_t *o);
NVN(nvnWindowGetPresentInterval, int, const NVNwindow *w) { return window_interval((const win_t *)w); }
NVN(nvnWindowSetPresentInterval, void, NVNwindow *w, int v) {
    if (((win_t *)w)->interval != v) { module_t *m = module_of(__builtin_return_address(0));
        LOG("[nvn] the game sets present interval %d → %d (from %s+%#llx)", ((win_t *)w)->interval, v, m ? m->name : "?", m ? (unsigned long long)((uint64_t)__builtin_return_address(0) - (uint64_t)m->base) : 0ULL); }
    ((win_t *)w)->interval = v;
}
NVN(nvnWindowSetCrop, void, NVNwindow *w, int x, int y, int ww, int hh) {
    win_t *o = (win_t *)w; int v[4] = {x, y, ww, hh};
    if (memcmp(o->crop, v, sizeof v)) LOG("[nvn] WindowCrop %d,%d %dx%d", x, y, ww, hh);
    memcpy(o->crop, v, sizeof v);
}
NVN(nvnWindowGetCrop, void, const NVNwindow *w, NVNrectangle *r) {   // before any SetCrop: the whole first texture, as NVN starts it
    const win_t *o = (const win_t *)w;
    if (o->crop[2] > 0 && o->crop[3] > 0) { r->x = o->crop[0]; r->y = o->crop[1]; r->width = o->crop[2]; r->height = o->crop[3]; return; }
    r->x = r->y = 0; r->width = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.width : 0; r->height = o->ntex && o->tex[0] ? (int)CTX(o->tex[0])->d.height : 0;
}
static void trow(uint64_t f, const char *name, double v);
static void rt_acquire(uint64_t period_ns);
static void rt_line(char *b, size_t n);
static double g_rt_comp_ms;   // ms; 0 = not declared
static void sleep_until_ns(uint64_t dl);
static uint64_t present_period(const win_t *o);
static uint64_t rt_period_ns(const win_t *o, uint64_t period);
static int present_itv(const win_t *o);
static int present_unpaced(const win_t *o);
static int link_wait(const win_t *o, uint64_t timeout_ns);
__attribute__((weak)) void dm_main_yield_stats(uint64_t *n, uint64_t *ns) { *n = 0; *ns = 0; }   // 0 in graphics unit tests
__attribute__((weak)) uint32_t dm_guest_work_qos(unsigned *count) { if (count) *count = 0; return QOS_CLASS_UNSPECIFIED; }
int mtl_probe_ramp_on(void);
__attribute__((weak)) void dm_yield_top(char *out, size_t n) { if (n) snprintf(out, n, "-"); }   // none in graphics unit tests
__attribute__((weak)) void dm_thread_cpu(char *out, size_t n, double secs) { (void)secs; if (n) snprintf(out, n, "-"); }   // guest-thread CPU by Switch priority band since the last call
void mtl_res_stats(char *out, size_t n);
void mtl_barrier_stats(uint64_t *skip, uint64_t *full);
extern uint32_t mtl_present_qos(void);
static _Atomic uint64_t g_frame;
static uint64_t g_acq_wait_ns, g_pq_wait_ns;   // display-link and present-queue waits inside acquire, per frame (several acquires a frame)
static uint64_t g_fs_start, g_fs_wait;   // fstep_tick: the first acquire's return after a present (0 = none yet) and its pacing wait
// Pauses and window changes (nvn_fstep.h settle), taken at g_fs_start with the frame's start, so a pause between acquire and present goes to the frame that holds it
uint64_t dm_focus_in_since(void);
static _Atomic uint64_t g_fs_settle_seq;
static uint64_t g_fs_focus_at, g_fs_settle_at;
void dm_fstep_settle(void) { atomic_fetch_add(&g_fs_settle_seq, 1); }
// A drain of standing frames is extra time, neither late nor owed (g_fs_drain: in the frame's first acquire, _late: a later one). Without the link the screen wait
// is the pacing itself and stays in the frame's time.
static uint64_t g_fs_drain, g_fs_drain_late;
static _Atomic uint64_t g_screen_at;   // when the screen wait released this frame's acquire (0 = no wait since the last present): the pacer takes the screen's phase
uint64_t dm_nvn_frame_count(void) { return g_frame; }
extern uint64_t g_t0_ns;   // host process start (main.c), as the UIMSG timestamps
uint64_t mtl_present_screen_wait(double period);
NVN(nvnWindowAcquireTexture, NVNwindowAcquireTextureResult, NVNwindow *w, NVNsync *s, int *idx) {
    win_t *o = (win_t *)w;
    int n = window_active(o);
    *idx = n ? o->next++ % n : 0;
    // Vsync blocks here, as on hardware when every swap-chain texture waits for display, not in present: the game often presents holding a lock
    uint64_t at = *idx < 4 ? o->avail[*idx] : 0, now = dm_detclock_enabled() ? guest_ns() : now_ns(), wait = at > now ? at - now : 0, pace_ns = 0, drain_ns = 0;
    if (!dm_detclock_enabled()) {   // time-constraint acquire thread, also when constantly behind
        uint64_t period = present_period(o); rt_acquire(rt_period_ns(o, period));
        uint64_t p0 = now_ns(); int snap = mtl_snap_on();
        o->follow = 1;   // the beat paces this frame only where the branches below wait for the slot (nvn_pace_follow)
        if (present_unpaced(o)) {}   // present interval 0: no vsync wait, only the ticket wait for a free texture
        else if (snap && mtl_link_active() && mtl_link_stalled()) { o->follow = 0; if (wait) sleep_until_ns(at); }   // snapshots with a silent link: the slot time paces, as frames are dropped and nothing frees a ring slot
        else if (mtl_link_active()) { if (!link_wait(o, period ? 2 * period : 33333333ull)) { o->follow = 0; if (wait) sleep_until_ns(at); } }   // DM_PRESENT_LINK: the display link paces; the host's 60 Hz clock only when no tick arrives
        else { o->follow = 0; if (wait) sleep_until_ns(at); }
        if (!present_unpaced(o) && mtl_present_screen_wait(period / 1e9)) atomic_store(&g_screen_at, now_ns());   // without the link: until the screen has caught up (DM_PRESENT_DEPTH)
        drain_ns = mtl_link_drain_take_ns();   // the ticks a drain of standing frames added to the link's wait
        pace_ns = now_ns() - p0;
    } else {   // DM_DETCLOCK_PACE=1 (screenshot gate): each frame's wall clock is at least one guest step, so frames and in-game time are deterministic under load
        static int pace = -1; static uint64_t step, last;
        if (pace < 0) { const char *e = getenv("DM_DETCLOCK_PACE"), *d = getenv("DM_DETCLOCK"); step = d ? strtoull(d, 0, 0) : 0; pace = e && atoi(e) && step; }
        if (pace) { if (last && now_ns() < last + step) sleep_until_ns(last + step); last = now_ns(); }
    }
    pace_ns += mtl_snap_acquire_wait();   // ring capacity is waited for at acquire, where the game holds no present lock
    g_acq_wait_ns += wait;
    uint64_t woke = wait ? now_ns() : at;
    trow(g_frame, "@late_ms", woke > at ? (woke - at) / 1e6 : 0);   // a display-link tick can wake the acquire before the slot
    // this texture's previous present not yet taken by the present thread (a whole swap-chain round behind): wait for it
    uint64_t tk = *idx < 4 ? o->ticket[*idx] : 0;
    if (tk) { uint64_t w0 = now_ns(); nvn_queue_present_wait(tk); nvn_queue_present_release(tk); o->ticket[*idx]=0; g_pq_wait_ns += now_ns() - w0; }
    if (s) { nvn_sync_release((sync_t *)s); ((sync_t *)s)->fenced = 2; ((sync_t *)s)->fence = mtl_last_fence(); }   // available once the commits before this have completed (fenced 2: an acquire sync, for the wait ledger)
    if (!g_fs_start) { g_fs_start = now_ns(); g_fs_wait = pace_ns; g_fs_drain += drain_ns; g_fs_focus_at = dm_focus_in_since(); g_fs_settle_at = atomic_load(&g_fs_settle_seq); }   // the frame starts when the first acquire after a present returns
    else g_fs_drain_late += drain_ns;
    return NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS;
}
// NVN 53's API: the GPU waits for the texture and no sync is handed out; the host replays in order, so this is AcquireTexture without a sync
NVN(nvnWindowFinalize, void, NVNwindow *w) {
    win_t *o=(win_t *)w; live_del(LV_WIN, w);
    for(int i=0;i<4;i++) { nvn_queue_present_release(o->ticket[i]); o->ticket[i]=0; }
}
NVN(nvnQueueAcquireTexture, NVNqueueAcquireTextureResult, NVNqueue *q, NVNwindow *w, int *idx) {
    (void)q; return h_nvnWindowAcquireTexture(w, NULL, idx) == NVN_WINDOW_ACQUIRE_TEXTURE_RESULT_SUCCESS ? (NVNqueueAcquireTextureResult)0 : (NVNqueueAcquireTextureResult)1;
}

// per present: each NVN function's call count this frame
static uint64_t g_prev[NVN_NFUNC];
static FILE *g_tsv;
static void dump_frame(void) {
    static int inited, nframes = 3, calls = 1;
    if (!inited) {
        inited = 1;
        const char *e = getenv("DM_NVN_FRAMES"); if (e) nframes = atoi(e);
        const char *c = getenv("DM_NVN_TSV_CALLS"); if (c) calls = atoi(c);   // 0 = only the @ timing rows
        const char *t = getenv("DM_NVN_TSV"); if (t && *t && !(g_tsv = fopen(t, "w"))) LOG("[RED] cannot open %s", t);   // set but empty: off
        if (g_tsv) fprintf(g_tsv, "# kind: nvn-per-frame  value = call count when name is an NVN function name; names starting with @ are timing probes (ms / count)\nframe\tname\tvalue\n");
    }
    uint64_t total = 0; unsigned nfn = 0;
    char line[8192]; size_t o = 0; line[0] = 0;
    if (!nvn_counting()) { if (!g_frame) LOG("[nvn] call counts off (DM_NVN_COUNT=1 turns them on)"); g_frame++; cs_dump(); goto counts_done; }
    for (unsigned i = 0; i < NVN_NFUNC; i++) {
        uint64_t c = atomic_load(&nvn_slot[i].cnt), d = c - g_prev[i];
        g_prev[i] = c;
        if (!d) continue;
        total += d; nfn++;
        if (g_tsv && calls) fprintf(g_tsv, "%llu\t%s\t%llu\n", g_frame, nvn_names[i], d);
        if (g_frame <= (uint64_t)nframes && o < sizeof line - 96) o += snprintf(line + o, sizeof line - o, " %s=%llu", nvn_names[i] + 3, d);
    }
    if (g_tsv) fflush(g_tsv);
    if (g_frame <= (uint64_t)nframes) LOG("[nvn] %s %llu: %u functions %llu calls |%s", g_frame ? "frame" : "init", g_frame, nfn, total, line);
    else if (g_frame % 300 == 0) LOG("[nvn] frame %llu: %u functions %llu calls", g_frame, nfn, total);
    g_frame++; cs_dump();
counts_done:
    // Every 300 frames: resident memory for the memory report, and the watchdog: past DM_MEM_CAP_GB (default 24, 0 = none) it logs a failure and exits,
    // as runaway host memory can panic the kernel
    if (g_frame % 300 == 0) { task_vm_info_data_t vi; mach_msg_type_number_t cnt = TASK_VM_INFO_COUNT;
        if (task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &cnt) == KERN_SUCCESS) { unsigned long long mb = (unsigned long long)(vi.phys_footprint >> 20); void mtl_mem_stat(unsigned long long, unsigned long long); mtl_mem_stat(g_frame, mb);
            static long cap = -1; if (cap < 0) { const char *e = getenv("DM_MEM_CAP_GB"); cap = e ? atol(e) : DM_MEM_CAP_DEFAULT_GB; }
            if (cap > 0 && mb > (unsigned long long)cap * 1024) { LOG("[RED] memory watchdog: resident %llu MB over the limit of %ld GB (frame %llu), exiting", mb, cap, (unsigned long long)g_frame); _exit(86); } } }
    dm_guest_frame_tick();   // DM_DETCLOCK: each present advances a fixed step; a no-op when off
}
// Timing rows to DM_NVN_TSV (frame / @metric / value); every 300 frames a p50 / p95 / max summary
#define TWIN 300
static double g_tw[6][TWIN]; static unsigned g_tn[6];
static double g_pres_iv[TWIN]; static unsigned g_presn;   // ms; -1 = dropped by the system, 0 = no reading
static double g_pres_exp[TWIN], g_pres_exp_last;   // ms the frame should have stayed on screen; the last one for slow_emit
// each present's hold (60 Hz vsyncs) by present number: on-screen intervals arrive 8 presents late and are judged against their own presents' holds
static uint8_t g_hold_ring[16];
static int fstep_hz(void);
static double pres_expect_ms(uint64_t n) {   // the interval between presents n - 1 and n shows frame n - 1: the larger of its hold (display link) and n's (the pacer)
    int k = g_hold_ring[n % 16] > g_hold_ring[(n - 1) % 16] ? g_hold_ring[n % 16] : g_hold_ring[(n - 1) % 16];
    double held = (k ? k : 1) * 1000.0 / fstep_hz(), tgt = 1000.0 / mtl_fps_target();   // at least the target's frame period
    return held > tgt ? held : tgt;
}
__attribute__((weak)) double dm_brightness_now(void) { return -1; }
__attribute__((weak)) int dm_thermal_now(void) { return -1; }   // thermalState, -1 = unknown (the Mac front end gives none); t_thermal.c defines it
__attribute__((weak)) int dm_low_power_now(void) { return 0; }   // the Mac front end reports none
// GPU time arrives 8 frames late: store first, judge later
#define LFN 16
typedef struct { uint64_t f; double fr, nv, t_s, make_ms, up_mb, up_ms; unsigned pso, lib, up, rup, hy; } lf_rec;   // hy: draws with a hybrid (pulling) pipeline
static lf_rec g_lf[LFN];
static void dm_longframe_note(const char *what) { LOG("[longframe] %s", what); }
extern void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns); extern void mtl_hybrid_stats(uint64_t *draws, uint64_t *made, uint64_t *none); extern uint64_t mtl_upload_bytes(void); extern void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads);
static const char *const g_tname[6] = {"frame time", "frame work", "NVN submit+present", "GPU", "translation layer recording", "translation layer submit"};
static void tpush(int k, double v) { g_tw[k][g_tn[k]++ % TWIN] = v; }
static int dcmp(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
// Power over the window, whole process: CPU energy (task_energy, nJ), CPU time (100% = one core), wakeups, GPU time, thermal state; then render-target traffic per frame
__attribute__((weak)) void dm_gpu_share_line(uint64_t frame, double frames) { (void)frame; (void)frames; }
static void power_line(uint64_t frame) {
    static uint64_t pt, pe, pcpu, pp, piw, ppw, pg, pf, pld, pst, pfu, pudc; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    task_power_info_v2_data_t pw; mach_msg_type_number_t n = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&pw, &n) != KERN_SUCCESS) return;
    uint64_t t = now_ns(), cpu = (pw.cpu_energy.total_user + pw.cpu_energy.total_system) * tb.numer / tb.denom, p = pw.task_ptime * tb.numer / tb.denom;
#if defined(__arm64__)
    uint64_t e = pw.task_energy;
#else
    uint64_t e = 0;
#endif
    uint64_t iw = pw.cpu_energy.task_interrupt_wakeups, idw = pw.cpu_energy.task_platform_idle_wakeups, g = pw.gpu_energy.task_gpu_utilisation;
    if (pt && t > pt) { double s = (t - pt) / 1e9; char yt[192]; dm_yield_top(yt, sizeof yt);
        double nf = frame > pf ? (double)(frame - pf) : 1;   // per frame: fair when one build holds the frame rate better than another
        LOG("[nvn] power up to frame %llu: CPU energy %.2f W, %.1f mJ per frame (%.1f fps); CPU %.0f%% (100 = one core), %.0f%% of it on performance cores; wakeups %.0f/s (out of platform idle %.0f); GPU %.1f ms/s; thermal %d; top yielders: %s", frame,
            (e - pe) / 1e9 / s, (e - pe) / 1e6 / nf, nf / s, (cpu - pcpu) / 1e7 / s, cpu > pcpu ? 100.0 * (double)(p - pp) / (double)(cpu - pcpu) : 0, (iw - piw) / s, (idw - ppw) / s, (g - pg) / 1e6 / s, dm_thermal_now(), yt);
        void mtl_dispatch_barrier_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *); static uint64_t pdg, pdc, pdu, pdn; uint64_t dg, dc, du, dn; mtl_dispatch_barrier_stats(&dg, &dc, &du, &dn);
        if (dg + dc + du + dn != pdg + pdc + pdu + pdn) LOG("[nvn] compute dispatches up to frame %llu: %llu after a barrier of the game's, %llu after a copy's range, %llu in the full order (a binding with no range), %llu with no barrier (DM_NVN_DISPATCH_BARRIER)", frame,
            (unsigned long long)(dg - pdg), (unsigned long long)(dc - pdc), (unsigned long long)(du - pdu), (unsigned long long)(dn - pdn));
        pdg = dg; pdc = dc; pdu = du; pdn = dn;
        dm_gpu_share_line(frame, nf);
        char rs[400]; mtl_res_stats(rs, sizeof rs); static uint64_t pbs, pbf; uint64_t bs, bf; mtl_barrier_stats(&bs, &bf);
        uint64_t mtl_cut_count(void), mtl_copy_cut_count(void), mtl_blit_splits(void); void mtl_ubd_stats(uint64_t *, uint64_t *); static uint64_t pcut, pccut, pbsp, pud, puc; uint64_t cut = mtl_cut_count(), ccut = mtl_copy_cut_count(), bsp = mtl_blit_splits(), ud, uc; mtl_ubd_stats(&ud, &uc);
        void mtl_ubp_stats(uint64_t *, uint64_t *, uint64_t *); static uint64_t ppm, ppr, ppf; uint64_t pm, pr, pf_; mtl_ubp_stats(&pm, &pr, &pf_);
        void mtl_lc_stats(uint64_t *, uint64_t *); static uint64_t plt, pll; uint64_t lt, ll; mtl_lc_stats(&lt, &ll);
        void mtl_ubp_why(uint64_t w[6]); static uint64_t pw5[6]; uint64_t w5[6]; mtl_ubp_why(w5);
        uint64_t ld, st, fu, udc = mtl_udc_saved_bytes(); mtl_bw(&ld, &st, &fu);
        void mtl_sm_stats(uint64_t *, uint64_t *, uint64_t *); static uint64_t psmp, psmc, psmw; uint64_t smp, smc, smw; mtl_sm_stats(&smp, &smc, &smw);
        void mtl_cshoist_stats(uint64_t *, uint64_t *, uint64_t *); static uint64_t pchm, pchd, pchs[8]; uint64_t chm, chd, chs[8]; mtl_cshoist_stats(&chm, &chd, chs);
        LOG("[nvn] residency set up to frame %llu: %s; draw-pass entry barriers full %llu / saved %llu; split submits %llu (%llu after buffer → texture copies); passes broken by copies / buffer updates %llu; in-pass uniform updates deferred %llu, passes ended to land them for a read %llu, partly covered blocks read from memory %llu (cached %llu, refused %llu: other GPU writes %llu, earlier submission running %llu, a report still to land %llu, copy or fill queued %llu, size %llu, unreadable %llu); waiting clears taken by a later pass %llu, landed in passes of their own %llu; slice-merged passes %llu (layer switches taken in %llu, waits for an amplified pipeline %llu); compute dispatches moved ahead of their pass %llu (sampling its depth %llu; "
            "scans stopped by a draw %llu, a dispatch %llu, a read of its writes %llu, depth written before it %llu, another op %llu); attachments per frame load %.0f MB / store %.0f MB, load-all store-all passes %.0f, saved by not storing unwritten attachments %.1f MB", frame, rs, (unsigned long long)(bf - pbf), (unsigned long long)(bs - pbs),
            (unsigned long long)(cut - pcut), (unsigned long long)(ccut - pccut), (unsigned long long)(bsp - pbsp), (unsigned long long)(ud - pud), (unsigned long long)(uc - puc), (unsigned long long)(pm - ppm), (unsigned long long)(pr - ppr), (unsigned long long)(pf_ - ppf), (unsigned long long)(w5[1] - pw5[1]), (unsigned long long)(w5[2] - pw5[2]), (unsigned long long)(w5[5] - pw5[5]), (unsigned long long)(w5[3] - pw5[3]), (unsigned long long)(w5[0] - pw5[0]), (unsigned long long)(w5[4] - pw5[4]), (unsigned long long)(lt - plt), (unsigned long long)(ll - pll), (unsigned long long)(smp - psmp), (unsigned long long)(smc - psmc), (unsigned long long)(smw - psmw), (unsigned long long)(chm - pchm), (unsigned long long)(chd - pchd),
            (unsigned long long)(chs[1] - pchs[1]), (unsigned long long)(chs[2] - pchs[2]), (unsigned long long)(chs[3] - pchs[3]), (unsigned long long)(chs[7] - pchs[7]), (unsigned long long)(chs[5] - pchs[5]),
            (ld - pld) / 1048576.0 / nf, (st - pst) / 1048576.0 / nf, (fu - pfu) / nf, (udc - pudc) / 1048576.0 / nf); pbs = bs; pbf = bf; pcut = cut; pccut = ccut; pbsp = bsp; pud = ud; puc = uc; ppm = pm; ppr = pr; ppf = pf_; memcpy(pw5, w5, sizeof pw5); plt = lt; pll = ll; psmp = smp; psmc = smc; psmw = smw; pchm = chm; pchd = chd; memcpy(pchs, chs, sizeof pchs);
        void mtl_fb_stats(uint64_t *, uint64_t *, uint64_t *, uint64_t *); static uint64_t pfs, pfr, pft, pfw; uint64_t fs_, fr, ft, fw; mtl_fb_stats(&fs_, &fr, &ft, &fw);
        if (fs_ != pfs || fr != pfr || ft != pft) LOG("[nvn] same-pass feedback up to frame %llu, per frame: %.1f snapshots taken, %.1f draws reused one, %.1f read the tile (%.1f waited for their pipeline)",
            frame, (fs_ - pfs) / nf, (fr - pfr) / nf, (ft - pft) / nf, (fw - pfw) / nf);
        pfs = fs_; pfr = fr; pft = ft; pfw = fw;
        static uint64_t pps[POOL_STATS]; uint64_t ps[POOL_STATS]; mtl_pool_stats(ps);
        if (ps[0] != pps[0] || ps[2] != pps[2] || ps[3] != pps[3]) {
            uint64_t into = ps[6] - pps[6], whole = ps[8] - pps[8] + ps[9] - pps[9] + ps[10] - pps[10];
            LOG("[nvn] pool tables up to frame %llu, per frame: %.2f built whole (%.2f ms), %.2f patched, %.2f kept; of these two %.2f over memory pool changes, %.2f over a new submission; "
                "patches into a spare of their table %.2f (%.0f entries written each), copied whole %.2f (no spare of the table %.2f, none finished %.2f, the log past them %.2f), %.3f ms; buffers %.1f MB in tables, %.1f MB spare",
                frame, (ps[0] - pps[0]) / nf, (ps[1] - pps[1]) / 1e3 / nf, (ps[2] - pps[2]) / nf, (ps[3] - pps[3]) / nf, (ps[4] - pps[4]) / nf, (ps[5] - pps[5]) / nf,
                into / nf, into ? (double)(ps[7] - pps[7]) / (double)into : 0.0, whole / nf, (ps[8] - pps[8]) / nf, (ps[9] - pps[9]) / nf, (ps[10] - pps[10]) / nf, (ps[11] - pps[11]) / 1e3 / nf, ps[12] / 1048576.0, ps[13] / 1048576.0); }
        memcpy(pps, ps, sizeof pps);
        void mtl_gsb_stats(uint64_t *, uint64_t *); static uint64_t pgn, pgb; uint64_t gn, gb; mtl_gsb_stats(&gn, &gb);
        if (gn != pgn) LOG("[nvn] GPU texture storage up to frame %llu: %llu regions, %.1f MB written into their storage on the GPU (%.2f regions, %.2f MB per frame)",
            frame, (unsigned long long)(gn - pgn), (gb - pgb) / 1048576.0, (gn - pgn) / nf, (gb - pgb) / 1048576.0 / nf);
        pgn = gn; pgb = gb;
        char tc[400]; dm_thread_cpu(tc, sizeof tc, s);
        LOG("[nvn] threads up to frame %llu: CPU %s", frame, tc); }
    else { char yt[192], rs[400], tc[400]; dm_yield_top(yt, sizeof yt); mtl_res_stats(rs, sizeof rs); dm_thread_cpu(tc, sizeof tc, 1); }   // the first call only takes the baselines
    pt = t; pe = e; pcpu = cpu; pp = p; piw = iw; ppw = idw; pg = g; pf = frame; mtl_bw(&pld, &pst, &pfu); pudc = mtl_udc_saved_bytes();
}
static void tsummary(uint64_t frame) {
    char line[1024]; size_t o = 0;
    for (int k = 0; k < 6; k++) {
        unsigned n = g_tn[k] < TWIN ? g_tn[k] : TWIN; if (!n) continue;
        double v[TWIN]; memcpy(v, g_tw[k], n * sizeof *v); qsort(v, n, sizeof *v, dcmp);
        o += snprintf(line + o, sizeof line - o, " %s p50 %.2f / p95 %.2f / max %.2f ms (%u frames);", g_tname[k], v[n / 2], v[(n * 95) / 100 < n ? (n * 95) / 100 : n - 1], v[n - 1], n);
    }
    LOG("[nvn] timing up to frame %llu:%s", frame, line);
    { void mtl_pso_line(char *, size_t); char pl[1024]; mtl_pso_line(pl, sizeof pl); LOG("[nvn] pipelines up to frame %llu: %s", frame, pl); }
    double tgt = 1000.0 / mtl_fps_target();   // ms one frame should take at the target (the counts below use it)
    { unsigned n = g_tn[0] < TWIN ? g_tn[0] : TWIN, over = 0; double v[TWIN];   // frames over one period in this window, and p99
      if (n) { memcpy(v, g_tw[0], n * sizeof *v); qsort(v, n, sizeof *v, dcmp); for (unsigned i = 0; i < n; i++) over += v[i] > tgt;
        LOG("[nvn] frame interval up to frame %llu: >%.1fms %u / %u frames, p99 %.2f ms", frame, tgt, over, n, v[(n * 99) / 100 < n ? (n * 99) / 100 : n - 1]); } }
    { unsigned n = g_presn < TWIN ? g_presn : TWIN, k = 0, drop = 0, discard = 0; double v[TWIN];   // dropped = on screen longer than its hold × 1.5 (pres_expect_ms), or discarded
      for (unsigned i = 0; i < n; i++) { double x = g_pres_iv[i]; if (x < 0) { drop++; discard++; } else if (x > 0) { v[k++] = x; drop += x > g_pres_exp[i] * 1.5; } }
      task_vm_info_data_t vi; mach_msg_type_number_t vc = TASK_VM_INFO_COUNT; double fp = task_info(mach_task_self(), TASK_VM_INFO, (task_info_t)&vi, &vc) == KERN_SUCCESS ? vi.phys_footprint / 1048576.0 : -1;
      double br = dm_brightness_now();   // on the present row: independent of the tick-entry observation gate
      if (k) { qsort(v, k, sizeof *v, dcmp); LOG("[nvn] on-screen up to frame %llu: %u frames, dropped %u (discarded %u), p99 %.2f ms; footprint %.0f MiB; brightness %.2f; thermal %d", frame, k + discard, drop, discard, v[(k * 99) / 100 < k ? (k * 99) / 100 : k - 1], fp, br, dm_thermal_now()); }
      g_presn = 0; }
    power_line(frame);
    { static uint64_t prev[6], pf; uint64_t v[6]; mtl_vbuf_stats(&v[0], &v[1], &v[2], &v[3], &v[4], &v[5]);
      if (pf && frame > pf) { double nf = (double)(frame - pf);
          LOG("[nvn] vertex bindings up to frame %llu: per frame address lookups %.0f / reused %.0f, vertex table writes %.0f / skipped %.0f, streams not 4-byte aligned copied %.1f (%.2f MB)", frame,
              (v[0] - prev[0]) / nf, (v[1] - prev[1]) / nf, (v[2] - prev[2]) / nf, (v[3] - prev[3]) / nf, (v[4] - prev[4]) / nf, (v[5] - prev[5]) / nf / 1048576.0); }
      memcpy(prev, v, sizeof prev); pf = frame; }
    { static uint64_t pi, pc, pd, pf; uint64_t ci = atomic_load(&g_cum_sub_instr), cc = atomic_load(&g_cum_sub_cpu_ns), ps_, dr_; mtl_stats(&ps_, &dr_);
      if (pf && frame > pf && dr_ > pd) LOG("[nvn] translation up to frame %llu: %.0f instructions per draw, submit per frame %.2f M instructions / CPU %.3f ms, %.0f draws per frame", frame,
          (double)(ci - pi) / (double)(dr_ - pd), (ci - pi) / 1e6 / (double)(frame - pf), (cc - pc) / 1e6 / (double)(frame - pf), (double)(dr_ - pd) / (double)(frame - pf));
      { static uint64_t py, pk, pw; uint64_t cy = atomic_load(&g_cum_sub_cyc), ck = atomic_load(&g_cum_sub_sys_ns), cw = atomic_load(&g_cum_sub_wall_ns);
        if (pf && frame > pf && ci > pi && cy > py) { double nf = (double)(frame - pf), cyc = (double)(cy - py), cpu = (double)(cc - pc);
          qos_class_t qc = QOS_CLASS_UNSPECIFIED; int rp = 0; pthread_get_qos_class_np(pthread_self(), &qc, &rp);
          uint64_t aw = 0, ag = 0; int an = 0; void mtl_alloc_stats(uint64_t *, uint64_t *, int *); mtl_alloc_stats(&aw, &ag, &an);
          static uint64_t pyn, pyns; uint64_t yn = 0, yns = 0; dm_main_yield_stats(&yn, &yns);
          LOG("[nvn] translation detail up to frame %llu: submit per frame %.2f M cycles (%.2f cycles per instruction), CPU %.3f ms of which kernel %.3f ms, wall clock %.3f ms; mean clock %.2f GHz (cycles / CPU time); this thread's QoS %#x; allocator pools %d, GPU waits so far %llu; real-time declared computation %.2f ms; main thread yields per frame %.0f / %.3f ms", frame,
              cyc / 1e6 / nf, cyc / (double)(ci - pi), cpu / 1e6 / nf, (double)(ck - pk) / 1e6 / nf, (double)(cw - pw) / 1e6 / nf, cpu > 0 ? cyc / cpu : 0, (unsigned)qc, an, (unsigned long long)aw, g_rt_comp_ms, (double)(yn - pyn) / nf, (double)(yns - pyns) / 1e6 / nf); pyn = yn; pyns = yns; }
        if (frame % 300 == 0) { unsigned wn = 0; uint32_t wq = dm_guest_work_qos(&wn); char rl[1024]; rt_line(rl, sizeof rl);   // the declaring threads, not the presenting one
            LOG("[nvn] thread policy: %s; %u guest worker threads QoS %#x; present QoS %#x", rl, wn, wq, mtl_present_qos()); }
        py = cy; pk = ck; pw = cw; }
      pi = ci; pc = cc; pd = dr_; pf = frame; }
    uint64_t up, re, es = mtl_pkg_edge_skips(), fa, pr, op, oc; mtl_pkg_stats(&up, &re); nvn_dirty_stats(&fa, &pr); nvn_dirty_open_stats(&op, &oc);
    { uint64_t sh, st, sm; mtl_spare_stats(&sh, &st, &sm); LOG("[nvn] packaged textures' spares: held %llu, retired idle or at memory pressure %llu, %llu MB", (unsigned long long)sh, (unsigned long long)st, (unsigned long long)sm); }
    LOG("[nvn] packaged textures: first uploads %llu, re-uploads on pool content change %llu, writes beside a texture only (not re-uploaded) %llu; write-protection faults %llu, pages unprotected before host writes %llu, opened for pool buffers %llu (changed while open %llu); missed %llu (cumulative); CPU-written textures alive %u", up, re, es, fa, pr, op, oc, mtl_pkg_miss(), mtl_cpu_tex_count());
    memset(g_tn, 0, sizeof g_tn);
}
// DM_SLOWFRAME=1: timings kept for 16 frames and judged 8 late (GPU and on-screen times are back by then); one log line per dropped frame,
// no per-frame file (the log tee fsyncs each line)
#define SRN 16
static struct { uint64_t f; int n; const char *k[64]; double v[64]; } g_sr[SRN];
static int g_slow = -1;
static int slow_on(void) { if (g_slow < 0) g_slow = getenv("DM_SLOWFRAME") != NULL; return g_slow; }
static int frame_rec(void) { return g_tsv || slow_on(); }
static void trow(uint64_t f, const char *name, double v) {
    if (g_tsv) fprintf(g_tsv, "%llu\t%s\t%.3f\n", f, name, v);
    if (slow_on()) { __typeof__(g_sr[0]) *r = &g_sr[f % SRN]; if (r->f != f) { r->f = f; r->n = 0; } if (r->n < 64) { r->k[r->n] = name; r->v[r->n++] = v; } }
}
static double srow(uint64_t f, const char *name, double dflt) {
    __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
    if (r->f == f) for (int i = 0; i < r->n; i++) if (!strcmp(r->k[i], name)) return r->v[i];
    return dflt;
}
// dropped: on-screen interval > its hold × 1.5, or −1; without a window, the present call interval
static void slow_emit(uint64_t f) {
    static uint64_t judged, slow;
    double pm = srow(f, "@presented_ms", 0), fr = srow(f, "@frame_ms", 0), lim = (g_pres_exp_last > 0 ? g_pres_exp_last : 1000.0 / mtl_fps_target()) * 1.5;
    int has = mtl_has_window(), bad = has ? (pm > lim || pm < 0) : fr > lim;
    if (has ? pm != 0 : fr != 0) judged++;
    if (bad) {
        slow++;
        char b[4096]; size_t o = snprintf(b, sizeof b, "[slowframe]\t%llu", f);
        __typeof__(g_sr[0]) *r = &g_sr[f % SRN];
        for (int i = 0; i < r->n && o < sizeof b - 64; i++) o += snprintf(b + o, sizeof b - o, "\t%s=%.3f", r->k[i], r->v[i]);
        double m = 0; for (uint64_t p = f >= 3 ? f - 3 : 0; p < f; p++) { double x = srow(p, "@frame_ms", 0); if (x > m) m = x; }
        snprintf(b + o, sizeof b - o, "\t@prev3_max_frame_ms=%.3f", m);
        fprintf(stderr, "%s\n", b);
    }
    if (f % 300 == 0) fprintf(stderr, "[slowframe]\t#\tup to frame %llu\tjudged=%llu\tslow=%llu\tcriterion=%s\n", f, judged, slow, has ? "on-screen interval" : "present call interval (no window)");
}
// Time-constraint policies (period = constraint = the present period, computation = CPU per frame) tell the scheduler the deadline.
// Declared by the acquire thread, the queue replay and the encode thread.
static int rt_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_RT"); v = !(e && !strcmp(e, "0")); if (!v) LOG("[nvn] DM_RT=0: no thread gets a time-constraint policy (QoS only)"); } return v; }
static int rt_frame_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_RT_FRAME"); v = !(e && !strcmp(e, "0")); if (!v && rt_on()) LOG("[nvn] DM_RT_FRAME=0: only the acquire thread declares a time-constraint policy"); } return v; }
static int rt_comp_fixed_ms(void) { static int v = -2; if (v == -2) { const char *e = getenv("DM_RT_COMP"); v = e && *e && strcmp(e, "auto") ? atoi(e) : -1; } return v; }   // −1 = auto
#define RT_THREADS 8
typedef struct {
    const char *name; mach_port_t port; uint64_t tid; int acq;   // tid checked against the port before another thread acts on it; acq: an acquire thread
    nvn_rt r; uint64_t cyc;
    _Atomic uint64_t period_ns, comp_us;   // declared (0: none), read by the thread policy line
    unsigned ndecl; int retired;   // retired: an acquire thread that stopped acquiring, back on the standard policy; its slot may be reused
    _Atomic int ready, pri, base, pol;
    _Atomic uint64_t frames, demoted, cpu_ns, cycles;   // since the last policy line; demoted: declared but below real-time priority (the kernel's fail-safe for a real-time thread
                                                        // that runs about 1.5 s without blocking)
} rt_thr;
static rt_thr g_rtt[RT_THREADS]; static _Atomic int g_rtt_n;
static _Atomic uint64_t g_rt_frame_ns;   // declared by the acquire thread; frame threads follow it
static pthread_mutex_t g_rt_mu = PTHREAD_MUTEX_INITIALIZER;   // several threads may acquire, and one retires another's slot
static void rt_init(rt_thr *t, const char *name, int acq, uint64_t tid) {   // on the thread itself
    t->name = name; t->acq = acq; t->tid = tid; t->port = pthread_mach_thread_np(pthread_self());
    memset(&t->r, 0, sizeof t->r); t->cyc = 0; t->ndecl = 0; t->retired = 0; atomic_store(&t->period_ns, 0); atomic_store(&t->comp_us, 0);
    atomic_store(&t->frames, 0); atomic_store(&t->demoted, 0); atomic_store(&t->cpu_ns, 0); atomic_store(&t->cycles, 0); atomic_store(&t->ready, 1);
}
static uint64_t rt_self_tid(void) { uint64_t id = 0; pthread_threadid_np(NULL, &id); return id; }
static rt_thr *rt_register(const char *name, int acq) {   // on the thread itself
    int i = atomic_fetch_add(&g_rtt_n, 1); if (i >= RT_THREADS) { atomic_fetch_sub(&g_rtt_n, 1); return NULL; }
    rt_thr *t = &g_rtt[i]; rt_init(t, name, acq, rt_self_tid()); return t;
}
static int rt_allowed(const rt_thr *t) { return rt_on() && (t->acq || rt_frame_on()); }
static void rt_declare(rt_thr *t, uint64_t period_ns, double comp_ms) {
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    double cap = period_ns * 0.9 / 1e6; if (comp_ms > cap) comp_ms = cap; if (comp_ms < 1) comp_ms = 1;
    uint64_t comp_ns = (uint64_t)(comp_ms * 1e6);
    thread_time_constraint_policy_data_t p = {
        .period = (uint32_t)(period_ns * tb.denom / tb.numer), .computation = (uint32_t)(comp_ns * tb.denom / tb.numer),
        .constraint = (uint32_t)(period_ns * tb.denom / tb.numer), .preemptible = 1};
    kern_return_t kr = thread_policy_set(t->port, THREAD_TIME_CONSTRAINT_POLICY, (thread_policy_t)&p, THREAD_TIME_CONSTRAINT_POLICY_COUNT);
    if (kr == KERN_SUCCESS) { t->r.comp_ms = comp_ms; atomic_store(&t->period_ns, period_ns); atomic_store(&t->comp_us, comp_ns / 1000); if (t->acq) g_rt_comp_ms = comp_ms; }
    if (++t->ndecl <= 20 || kr != KERN_SUCCESS)
        LOG("[nvn] %s time-constraint policy (period %.3f ms, computation %.3f ms) %s%s", t->name, period_ns / 1e6, comp_ms, kr == KERN_SUCCESS ? "set" : "failed to set", t->ndecl == 20 ? "; further declarations of this thread only in the thread policy line" : "");
}
// on the thread itself, before each piece of its work: one sample a presented frame
static void rt_tick(rt_thr *t, uint64_t period_ns) {
    uint64_t f = atomic_load_explicit(&g_frame, memory_order_relaxed), declared = atomic_load_explicit(&t->period_ns, memory_order_relaxed);
    int period_moved = period_ns && period_ns != declared && (t->acq || t->r.comp_ms > 0);
    if (f == t->r.frame && !period_moved) return;
    if (f != t->r.frame) {
        uint64_t cpu = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID), ins, cyc; thr_counts(&ins, &cyc);
        if (t->r.frame && f > t->r.frame) {
            thread_extended_info_data_t xi; mach_msg_type_number_t xc = THREAD_EXTENDED_INFO_COUNT;
            if (thread_info(t->port, THREAD_EXTENDED_INFO, (thread_info_t)&xi, &xc) == KERN_SUCCESS) {
                atomic_store(&t->pri, xi.pth_curpri); atomic_store(&t->base, xi.pth_priority); atomic_store(&t->pol, xi.pth_policy);
                if (declared && xi.pth_curpri < 97) atomic_fetch_add(&t->demoted, 1);   // 97: BASEPRI_RTQUEUES, where a time-constraint thread runs
            }
            atomic_fetch_add(&t->frames, 1); atomic_fetch_add(&t->cpu_ns, cpu - t->r.cpu); atomic_fetch_add(&t->cycles, cyc - t->cyc);
        }
        t->cyc = cyc;
        double c = nvn_rt_sample(&t->r, f, cpu, (double)(period_ns ? period_ns : declared) / 1e6);
        if (c > 0 && period_ns && rt_allowed(t) && !(t->acq && rt_comp_fixed_ms() > 0) && (declared || !t->acq)) { rt_declare(t, period_ns, c); return; }
    }
    if (period_moved && rt_allowed(t)) {   // a new period: the acquire thread declares at once, a frame thread with what it measured
        int fx = t->acq ? rt_comp_fixed_ms() : -1;
        rt_declare(t, period_ns, fx > 0 ? fx : t->r.comp_ms > 0 ? t->r.comp_ms : 2);
    }
}
// Under g_rt_mu: another acquire thread idle for a whole window goes back to the standard policy, as it no longer carries the frame.
// Its port is used only while it still names that thread: an ended thread's port name may be reused.
static void rt_retire(const rt_thr *self) {
    uint64_t f = atomic_load_explicit(&g_frame, memory_order_relaxed); int nt = atomic_load(&g_rtt_n); if (nt > RT_THREADS) nt = RT_THREADS;
    for (int i = 0; i < nt; i++) { rt_thr *o = &g_rtt[i];
        if (o == self || !o->acq || o->retired || !atomic_load(&o->ready) || f < o->r.frame + RT_WIN) continue;
        thread_identifier_info_data_t ii; mach_msg_type_number_t ic = THREAD_IDENTIFIER_INFO_COUNT; int same = thread_info(o->port, THREAD_IDENTIFIER_INFO, (thread_info_t)&ii, &ic) == KERN_SUCCESS && ii.thread_id == o->tid;
        if (same && atomic_load(&o->period_ns)) { thread_standard_policy_data_t st = {0}; thread_policy_set(o->port, THREAD_STANDARD_POLICY, (thread_policy_t)&st, THREAD_STANDARD_POLICY_COUNT); }
        LOG("[nvn] acquire thread %llu stopped acquiring %llu frames ago%s", (unsigned long long)o->tid, (unsigned long long)(f - o->r.frame), same ? (atomic_load(&o->period_ns) ? ": back to the standard policy" : "") : " and has ended");
        o->retired = 1; atomic_store(&o->period_ns, 0); atomic_store(&o->comp_us, 0);
    }
}
// One slot per acquiring thread: CPU clocks and policies are per thread, so a game that acquires from a new thread gets that thread measured and declared
static void rt_acquire(uint64_t period_ns) {
    static _Thread_local rt_thr *t; static _Thread_local uint64_t tid; static int full;
    if (!tid) tid = rt_self_tid();
    atomic_store_explicit(&g_rt_frame_ns, period_ns, memory_order_relaxed);
    pthread_mutex_lock(&g_rt_mu);
    if (t && t->tid != tid) t = NULL;   // its slot went to another acquire thread
    if (!t) {   // first acquire, or first since its slot was reused
        int nt = atomic_load(&g_rtt_n); if (nt > RT_THREADS) nt = RT_THREADS;
        for (int i = 0; i < nt && !t; i++) if (g_rtt[i].acq && g_rtt[i].retired) { t = &g_rtt[i]; rt_init(t, "acquire thread", 1, tid); }
        if (!t && !(t = rt_register("acquire thread", 1)) && !full) { full = 1; LOG("[nvn] [RED] no time-constraint slot is left for another acquire thread (%d slots)", RT_THREADS); }
    } else if (t->retired) { double c = t->r.comp_ms; memset(&t->r, 0, sizeof t->r); t->r.comp_ms = c; t->retired = 0; }   // acquiring again: a new window, its computation declared at once
    if (t) { rt_tick(t, period_ns); rt_retire(t); }
    pthread_mutex_unlock(&g_rt_mu);
}
void *nvn_rt_join(const char *name) { return rt_register(name, 0); }
void nvn_rt_tick(void *h) { if (h) rt_tick(h, atomic_load_explicit(&g_rt_frame_ns, memory_order_relaxed)); }
// per thread: declaration, priority, policy, frames demoted, CPU per frame and mean clock (cycles per CPU second)
static void rt_line(char *b, size_t n) {
    size_t o = 0; int nt = atomic_load(&g_rtt_n); if (nt > RT_THREADS) nt = RT_THREADS; b[0] = 0;
    for (int i = 0; i < nt && o < n; i++) { rt_thr *t = &g_rtt[i]; if (!atomic_load(&t->ready)) continue;
        uint64_t fr = atomic_exchange(&t->frames, 0), de = atomic_exchange(&t->demoted, 0), cn = atomic_exchange(&t->cpu_ns, 0), cy = atomic_exchange(&t->cycles, 0);
        uint64_t pn = atomic_load(&t->period_ns), cu = atomic_load(&t->comp_us);
        int w = pn ? snprintf(b + o, n - o, "%s%s time-constraint computation %.2f ms / period %.2f ms", o ? "; " : "", t->name, cu / 1e3, pn / 1e6)
                             : snprintf(b + o, n - o, "%s%s no time-constraint policy", o ? "; " : "", t->name);
        if (w > 0) o += (size_t)w; if (o >= n) break;
        w = snprintf(b + o, n - o, ", priority %d (base %d, policy %d), demoted in %llu of %llu frames, CPU %.2f ms a frame at %.2f GHz", atomic_load(&t->pri), atomic_load(&t->base), atomic_load(&t->pol),
                     (unsigned long long)de, (unsigned long long)fr, fr ? cn / 1e6 / fr : 0, cn ? (double)cy / cn : 0);
        if (w > 0) o += (size_t)w;
    }
    if (!o) snprintf(b, n, "no thread registered");
}
static void sleep_until_ns(uint64_t dl) {   // absolute monotonic time
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    uint64_t now = now_ns(); if (dl <= now) return;
    mach_wait_until(mach_absolute_time() + (dl - now) * tb.denom / tb.numer);
}
// 60 Hz by default. DM_PRESENT_HZ=0 (uncapped) is a profiling switch only: games count frames, so uncapped runs them wrong
static int g_hz = -1, g_display_mode;
static _Atomic int g_game_itv = 1;   // the interval the game sets, when pacing by it (2 = 30 fps in game mode); nvn_game_fps reports it
void nvn_present_config(int hz, int display_mode) { g_hz = hz; g_display_mode = display_mode; }
static _Atomic uint64_t g_itv2_since;   // start of the current run of even intervals >= 2 (0 = 1 or odd now)
// The rate the game asks for: 30 in game mode once an even interval has held a full second, else 60; back at once on 1 or odd (an odd interval has no whole 30 Hz ticks).
// The hold keeps one interval-2 frame from flapping the display-link mode; with the frame step the host picks each hold.
__attribute__((weak)) uint8_t *dm_fps_state;   // graphics unit tests have no loader
// No engine frame limiter the host writes: the game reads whole 1/60 s vsyncs from dm_fps_state, so 1/120 s ticks (fps_high) are reached only by the unit tests that set this
static int g_fps_engine;
__attribute__((weak)) int dm_movie_hz(void) { return 0; }   // the playing movie's picture rate, 0 when none
// Thermal cap: at thermalState >= 2 the frame step holds 30 fps, back after THERMAL_CALM_NS at fair or better. DM_THERMAL_CAP=0 turns it off; the Mac reports no thermal state
#define THERMAL_CALM_NS 30000000000ull
static int thermal_capped(void) {
    // acquire, present and the display-link setup may ask from different threads: sample and recovery timer change as one, the clock taken inside the lock
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static int on = -1, capped; static uint64_t last, calm_since;
    pthread_mutex_lock(&mu);
    if (on < 0) { const char *e = getenv("DM_THERMAL_CAP"); on = !(e && !strcmp(e, "0")); }
    if (on) {
        uint64_t now = now_ns();
        if (now - last >= 1000000000ull) {
            last = now;
            int th = dm_thermal_now();
            if (th >= 2) { calm_since = 0; if (!capped) { capped = 1; LOG("[nvn] thermal state %d: the frame step holds 30 fps until the device is fair again for %llu s (DM_THERMAL_CAP=0 turns this off)", th, THERMAL_CALM_NS / 1000000000ull); } }
            else if (capped) { if (!calm_since) calm_since = now; else if (now - calm_since >= THERMAL_CALM_NS) { capped = 0; LOG("[nvn] thermal state %d for %llu s: back to the frame rate cap", th, THERMAL_CALM_NS / 1000000000ull); } }
        }
    }
    int result = capped;
    pthread_mutex_unlock(&mu);
    return result;
}
// 120 fps (fps_max 120): the frame step counts ticks of 1/120 s (hold 1 = 120 fps); 60 below a 120 Hz screen, during a movie and under the thermal cap.
// A game with a frame step state page is out: its stubs hand the game whole 1/60 s vsyncs
static int g_fps_high = -1;
static _Atomic int g_high_cap = 60;
static int fps_high(void) {
    int v = __atomic_load_n(&g_fps_high, __ATOMIC_RELAXED);
    if (v < 0) {
        const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
        v = t == 120 && DM_GAME_FPS_MAX >= 120 && !dm_fps_state && g_fps_engine;
        if (t == 120 && DM_GAME_FPS_MAX >= 120 && !v) LOG("[RED] [nvn] 120 fps: this main has %s, so it runs at 60", dm_fps_state ? "a frame step in whole vsyncs" : "no engine frame limit the host writes");
        __atomic_store_n(&g_fps_high, v, __ATOMIC_RELAXED);
    }
    return v;
}
static int fstep_hz(void) { return fps_high() ? 120 : 60; }   // the ticks the frame step counts in
// once per present: the cap the screen and game state allow, and a display link at a rate the screen can show (DM_PRESENT_LINK keeps its own)
static void high_cap_tick(void) {
    static int win = -1, link_env = -1;
    if (win < 0) { win = mtl_display_hz() != 0; link_env = getenv("DM_PRESENT_LINK") != NULL; }
    int ok = !win || front_screen_max_fps() >= 120, movie = dm_movie_hz(), cap = ok && !movie && !thermal_capped() ? 120 : 60;
    static int was; if (cap != was) { LOG("[nvn] 120 fps: cap %d → %d fps (%s)", was, cap, cap == 120 ? "the screen shows it" : !ok ? "the screen is below 120 Hz" : movie ? "movie playing" : "thermal state serious"); was = cap; }
    atomic_store(&g_high_cap, cap);
    if (!link_env) mtl_link_mode_set(ok ? 2 : 1);
}
static int fps_step_cap(void) {
    if (!dm_fps_state) return fps_high() ? atomic_load(&g_high_cap) : 0;
    uint32_t b = __atomic_load_n((uint32_t *)dm_fps_state, __ATOMIC_RELAXED); float t; memcpy(&t, &b, sizeof t);
    if (t <= 0) return 0;
    // movies and a 30 fps target still sample heat, so the calm interval resets and recovery counts
    int thermal = thermal_capped(), movie = dm_movie_hz();
    if (movie) return movie >= 50 && !thermal ? 60 : 30;   // a movie: its player takes a picture per game frame, so the step follows its pictures (the thermal cap aside)
    return t <= 30 || thermal ? 30 : 60;
}
static int fstep_on(void) { return fps_step_cap() != 0; }
// Without an engine limiter the host writes the game does not follow the cap: in game mode its own interval stays; display mode applies the cap as the present interval
static int g_present_cap = -1;
static int display_mode(void) {   // present_cfg's choice, also before its first call
    if (g_hz >= 0) return g_display_mode;
    const char *m = getenv("DM_PRESENT_INTERVAL"); return m ? !strcmp(m, "display") : DM_GAME_PRESENT_DISPLAY;
}
static int fps_present_cap(void) {
    if (dm_fps_state || fps_high()) return fps_step_cap();
    if (g_present_cap < 0) {
        const char *e=getenv("DM_FPS_TARGET");int t=e?atoi(e):DM_GAME_FPS_TARGET;
        g_present_cap=t==30||t==60?t:t==120?60:0;   // 120 here: not this game's (fps_high), so 60
        if (g_present_cap && !g_fps_engine && display_mode())
            LOG("[nvn] present cap %d fps (%s) applied by the present interval alone: display mode, and this main has no engine frame limit the host writes", g_present_cap, e ? "DM_FPS_TARGET" : "game profile fps_target");
        else if (g_present_cap && !g_fps_engine) {
            LOG("[nvn] present cap %d fps (%s) not applied: this main has neither a frame step nor an engine frame limit the host writes, so the game's own present interval stays", g_present_cap, e ? "DM_FPS_TARGET" : "game profile fps_target");
            g_present_cap=0;
        }
    }
    return g_present_cap;
}
static nvn_fstep g_fs;   // present thread only
static nvn_fgpu g_fg;   // GPU time at kmin and at the current hold
static _Atomic int g_fs_k;   // 0 = not decided yet
// GetDisplayVsyncEvent's rate: 60, or 120 at 120 fps with hold 1; a game waiting for each vblank is held to it, so the trial at 120 needs it at 120.
// nvn_game_fps: the display link runs at 30 Hz at a 30 cap, except while the game fixes its own step at 1
int nvn_vsync_hz(void) { return fps_high() && atomic_load(&g_fs_k) == 1 && atomic_load(&g_high_cap) == 120 ? 120 : 60; }
int nvn_game_fps(void) { int c = fps_present_cap(); if (c) return c == 30 && atomic_load(&g_fs_k) == 1 && fstep_on() ? 60 : c; uint64_t s = atomic_load(&g_itv2_since); return g_hz > 0 && !g_display_mode && s && now_ns() >= s + 1000000000ull ? 30 : 60; }
static void present_cfg(const win_t *o) {
    if (g_hz >= 0) return;
    // The profile's present_interval (default game): whether frames follow the game's interval depends on whether its logic counts frames; the environment overrides it
    int scr = mtl_display_hz();
    const char *e = getenv("DM_PRESENT_HZ"); g_hz = e ? atoi(e) : fstep_hz();
    if (!g_hz) LOG("[nvn] [WARNING] DM_PRESENT_HZ=0: frame rate uncapped, frame pacing unlike the device, UI logic may drift (for profiling only, not for verdicts / cross-checks)");
    // DM_PRESENT_INTERVAL=display (a deliberate deviation): one frame per display-link tick whatever interval the game sets, 60 fps at least
    const char *m = getenv("DM_PRESENT_INTERVAL"); g_display_mode = m ? !strcmp(m, "display") : DM_GAME_PRESENT_DISPLAY;
    LOG("[nvn] present tick %d Hz, interval mode %s (%s; present interval set by the game %d; %s)", g_hz, g_display_mode ? "display (one frame per tick, a cut)" : "game (original semantics)",
        m ? "DM_PRESENT_INTERVAL" : "game profile present_interval", o->interval, scr > 0 ? "window" : "no window");
}
static int present_itv(const win_t *o) { int c = fps_present_cap(), k = atomic_load(&g_fs_k); return c ? (k && fstep_on() ? k : fstep_hz() / c) : g_display_mode ? 1 : o->interval > 0 ? o->interval : 1; }   // vsyncs per frame that presentation follows
// Interval 0 swaps without a vsync wait on Horizon (the display shows the newest frame each vsync): taken as 1, a loading screen that draws as fast as it can would
// slow by minutes. One frame per display tick stays; a host frame cap or display mode keeps its own pacing.
static int present_unpaced(const win_t *o) { present_cfg(o); return o->interval == 0 && !g_display_mode && !fps_present_cap(); }
// in display mode, the interval the window presents at: the game's own would let it pace for that itself, under the host's cap
static int window_interval(const win_t *o) { present_cfg(o); return g_display_mode ? present_itv(o) : o->interval; }
// the present interval's 60 Hz vsyncs, or in display mode above 60 Hz one tick of the present rate, as many link ticks as that takes
static int link_wait(const win_t *o, uint64_t timeout_ns) {
    static int hz_was, rate_was, remainder;   // acquire thread only
    if (fps_high()) { int t = present_itv(o) * mtl_link_rate() / 120; return mtl_link_wait_ticks(t < 1 ? 1 : t, timeout_ns); }   // the hold in 1/120 s ticks, on a 120 or 60 Hz link
    int hz = g_display_mode && !fps_present_cap() && g_hz > 60 ? g_hz : 0, rate = mtl_link_rate();
    if (hz != hz_was || rate != rate_was) { remainder = 0; hz_was = hz; rate_was = rate; }
    if (hz) {
        int ticks = 1;
        // keep the fractional tick: 90 fps on a 120 Hz link takes 1, 1, 2 ticks; a rate change starts a new phase
        if (rate > hz) { remainder += rate; ticks = remainder / hz; remainder %= hz; }
        return mtl_link_wait_ticks(ticks, timeout_ns);
    }
    return mtl_link_wait_tick(present_itv(o), timeout_ns);
}
static uint64_t present_period(const win_t *o) {
    present_cfg(o);
    int was = atomic_load(&g_game_itv), want = o->interval > 0 ? o->interval : 1;
    if (want != was) {
        atomic_store(&g_game_itv, want);
        if (want < 2 || want % 2) atomic_store(&g_itv2_since, 0);   // an odd interval needs the 60 Hz link: 3 vsyncs on a 30 Hz link were 2 ticks
        else if (was < 2 || was % 2) atomic_store(&g_itv2_since, now_ns() | 1);   // | 1: the start is never 0
    }
    int itv = present_itv(o), hz = g_hz;
    return hz > 0 ? (uint64_t)itv * 1000000000ull / (uint64_t)hz : 0;
}
// The period declared to the scheduler: the present period, but a hold raised for late frames keeps the cap's deadline, or the controller parks the game on
// efficiency cores where the cap no longer fits. A step the game fixes itself is declared as it is.
static uint64_t rt_period_ns(const win_t *o, uint64_t period) {
    int c = fps_step_cap();
    if (!period || !c || !fstep_on() || (dm_fps_state && __atomic_load_n((uint32_t *)(dm_fps_state + 0x24), __ATOMIC_RELAXED))) return period;
    int kmin = fstep_hz() / c;
    return present_itv(o) > kmin ? (uint64_t)kmin * 1000000000ull / (uint64_t)g_hz : period;
}
// CPU slowed for reasons other than heat (allows probes): a game waiting out a long hold can be parked on efficiency cores, where the cost never fits k − 1 again
static int g_fs_pshare = -1;   // percent; −1 = not measured yet
static int fstep_cpu_slowed(uint64_t presents) {
    static int levels = -1, slowed, low_was; static uint64_t pc, pp;
    if (levels < 0) { size_t n = sizeof levels; if (sysctlbyname("hw.nperflevels", &levels, &n, 0, 0)) levels = 1; }
    if (levels < 2 || presents % 30) return levels >= 2 && slowed;
    task_power_info_v2_data_t pw; mach_msg_type_number_t n = TASK_POWER_INFO_V2_COUNT;
    if (task_info(mach_task_self(), TASK_POWER_INFO_V2, (task_info_t)&pw, &n) != KERN_SUCCESS) return slowed;
    uint64_t c = pw.cpu_energy.total_user + pw.cpu_energy.total_system, p = pw.task_ptime;   // mach time units, as in power_line
    if (pc && c > pc) g_fs_pshare = (int)(100 * (p - pp) / (c - pc));
    pc = c; pp = p;
    int low = g_fs_pshare >= 0 && g_fs_pshare < 30;
    slowed = low && low_was && dm_thermal_now() <= 1 && !dm_low_power_now(); low_was = low;
    return slowed;
}
// whether late frames may raise the hold above the cap (profile fps_hold, DM_FPS_HOLD): off for a game that steps by the cap whatever the hold, where holding only paces the screen
static int fstep_hold(void) {
    static int h = -1;
    if (h < 0) {
        const char *e = getenv("DM_FPS_HOLD"); h = e ? strcmp(e, "off") != 0 : DM_GAME_FPS_HOLD;
        if (!h) LOG("[nvn] frame step: frames are not held longer than the cap's vsyncs (%s)", e ? "DM_FPS_HOLD" : "game profile fps_hold");
    }
    return h;
}
// Once per present: the time since the previous frame's start and the frame's cost (max of CPU without the pacing wait and the latest GPU time, in vsyncs)
// decide this hold and the next step
static void fstep_tick(const win_t *o, int c) {
    static uint64_t prev, t0, v0, fixed_sum, held[FSTEP_MAX + 1], seen_focus, seen_settle; static int nlog;
    const double v = 1e9 / fstep_hz();
    uint64_t st = g_fs_start ? g_fs_start : now_ns(), pw = g_fs_wait;
    uint64_t f_at = g_fs_start ? g_fs_focus_at : dm_focus_in_since(), s_at = g_fs_start ? g_fs_settle_at : atomic_load(&g_fs_settle_seq);
    g_fs_start = g_fs_wait = 0;
    int settled = prev && (f_at != seen_focus || s_at != seen_settle);   // a pause ended, graphics resumed or the window changed within this frame's span
    seen_focus = f_at; seen_settle = s_at;
    // a drain of standing frames comes out of the frame's time as far as it made the frame longer than its hold
    static uint64_t drain_before;   // a drain in a later acquire of the previous frame: in this frame's time, not its pacing wait
    static uint64_t drain_n, drain_cut;   // for the 1800-present line
    uint64_t span = prev && st > prev ? st - prev : 0, raw = span;
    span = nvn_fstep_drain_cut(span, (uint64_t)((g_fs.k ? g_fs.k : fstep_hz() / c) * v), g_fs_drain, drain_before, &pw);
    if (span < raw) { drain_n++; drain_cut += raw - span; }
    drain_before = g_fs_drain_late; g_fs_drain = g_fs_drain_late = 0;
    double shown = prev ? span / v : 0, gpu = 0; prev = st; int gpu_on = 0;
    for (uint64_t f = g_frame; f-- > 0 && g_frame - f <= 8;) { double g = mtl_gpu_ms(f); if (g >= 0) { gpu = g * 1e6 / v; gpu_on = 1; break; } }
    double cpu = shown - pw / v, cost = cpu > gpu ? cpu : gpu;
    int kmin = fstep_hz() / c, kmax = fps_high() ? 2 : !fstep_hold() ? kmin : kmin >= 2 ? 2 : FSTEP_MAX, k0 = g_fs.k,   // at 120 fps a late frame steps down whatever fps_hold says: the game steps by the target
         kmin0 = g_fs.kmin, probe0 = g_fs.probe;
    int fixed = dm_fps_state ? (int)__atomic_load_n((uint32_t *)(dm_fps_state + 0x24), __ATOMIC_RELAXED) : 0;
    uint64_t probes0 = g_fs.probes, failed0 = g_fs.failed;
    // A present the game made no frame for (its thread held: frame advance), or the frame after one, goes neither to the frame step nor the frame time; prev still moves
    static nvn_fheld fh;
    int judged = !dm_fps_state || nvn_fheld_judged(&fh, __atomic_load_n((uint64_t *)(dm_fps_state + 0x30), __ATOMIC_RELAXED)) || fixed;
    if (!judged) { static int said; if (!said) { said = 1; LOG("[nvn] frame step: a present with no frame of the game behind it at present %llu (the game's thread held, as by the practice tools' frame advance): such presents and the frames let through between them are not judged, and step at the cap", (unsigned long long)o->presents); } }
    // A screen not a multiple of 60 Hz shows 1/60 s ticks on their nearest refreshes: a frame on time can show one refresh more than its hold
    int hz = front_screen_max_fps(); g_fs.shown_slack = hz > fstep_hz() && hz % fstep_hz() && mtl_link_active() ? (double)fstep_hz() / hz : 0;
    int cslow = judged && fstep_cpu_slowed(o->presents), gslow = judged && nvn_fstep_gpu_slowed(&g_fg, gpu, &g_fs, kmin) && dm_thermal_now() <= 1 && !dm_low_power_now();   // same gates as the CPU's
    // Present snapshots: the game runs up to a frame ahead, so a long frame is absorbed and its cost is no evidence. Lateness is the on-screen interval 8 presents back
    // against its hold (a quarter late, or dropped), or 12 of the last 16 starts late
    if (judged && mtl_snap_on() && mtl_link_active() && !mtl_link_stalled() && o->presents > 9) {   // frames the stall fallback drops say nothing: judged by cost then
        uint64_t pn = o->presents - 8; double a = mtl_presented_time(pn + g_present_base), b = mtl_presented_time(pn - 1 + g_present_base);
        if (a < 0) g_fs.screen = 2; else if (a > 0 && b > 0) g_fs.screen = (a - b) * 1e3 > 1.25 * pres_expect_ms(pn) ? 2 : 1;
        static uint16_t slow_bits; static int slow_k; int k = g_fs.k ? g_fs.k : kmin;
        if (k != slow_k) { slow_k = k; slow_bits = 0; }   // judged against the hold in force
        slow_bits = (uint16_t)(slow_bits << 1 | (shown > k + FSTEP_LATE));
        if (__builtin_popcount(slow_bits) >= 12) g_fs.screen = 2;
    }
    // a probe's grace lasts while its frames get cheaper: macOS raises the GPU clock over seconds, not within a probe's 30 frames
    g_fs.ramp = mtl_probe_ramp_on();
    g_fs.per_vsync = fstep_hz() / 60;
    g_fs.no_payback = fps_high();   // at 120 fps a late frame is slow motion: three raise the hold whatever the average
    g_fs.gpu_on = gpu_on; g_fs.gpu = gpu;   // an idle GPU's late frames do not step up; a hold the GPU does not explain is probed
    g_fs.settle = settled;   // this frame and its grace are not the scene's cost, and the pause is not time owed
    int n = judged ? nvn_fstep_next(&g_fs, shown, cost, kmin, kmax, fixed, cslow || gslow) : kmin;
    if (!judged) { g_fs.screen = 0; g_fs.gpu_on = 0; g_fs.gpu = 0; g_fs.settle = 0; }   // reset for a call that did not come
    static double measured_sum;   // vsyncs the game stepped beyond the host's
    if (dm_fps_state && !fixed && __atomic_load_n((uint32_t *)(dm_fps_state + 0x3c), __ATOMIC_RELAXED) && !dm_detclock_enabled()) {   // a stub that steps by the frame time when longer (state +0x38; the profile's patches.py)
        static int on = -1; static nvn_fdelta fd;
        if (on < 0) { const char *e = getenv("DM_FRAME_DELTA"); on = !e || strcmp(e, "0"); LOG("[nvn] game step from the measured frame time: %s", on ? "on" : "off (DM_FRAME_DELTA=0)"); }
        if (on) {
            // a held present or a frame across a pause stays out of the mean of the last 8 (nvn_fdelta_next); the next interval starts here
            double vs = 0;
            if (judged && !settled) vs = nvn_fstep_measured(&g_fs, nvn_fdelta_next(&fd, now_ns()) * 60.0);
            else fd.last = now_ns();
            if (judged) { n = g_fs.step; if (vs > n) measured_sum += vs - n; }
            float s = (float)(vs / 60); uint32_t b; memcpy(&b, &s, sizeof b);
            __atomic_store_n((uint32_t *)(dm_fps_state + 0x38), b, __ATOMIC_RELAXED);
        }
    }
    if (dm_fps_state) __atomic_store_n((uint32_t *)(dm_fps_state + 0x20), (uint32_t)n, __ATOMIC_RELAXED);
    atomic_store(&g_fs_k, g_fs.k);
    if (judged) held[g_fs.k]++;
    if (fixed) fixed_sum += (uint64_t)n;   // the stubs count only the host's steps
    static int fixed_was, nfix;
    if (fixed != fixed_was && nfix < 40) { nfix++; if (fixed) LOG("[nvn] frame step: the game fixes its own step at %d vsyncs at present %llu", fixed, (unsigned long long)o->presents); else LOG("[nvn] frame step: the game hands the step back to the host at present %llu", (unsigned long long)o->presents); }
    fixed_was = fixed;
    if (k0 && g_fs.k != k0 && nlog < 40) {
        char why[256], held[48] = ""; int w = snprintf(why, sizeof why, "probe");
        if (cslow) w += snprintf(why + w, sizeof why - w, ", CPU %d%% on performance cores", g_fs_pshare);
        double gr = nvn_fgpu_ratio(&g_fg);   // failed probes' ratio: the scene's time at kmin is cur over it
        if (gslow && gr > 0) w += snprintf(why + w, sizeof why - w, ", GPU %.2f at hold %d, %.2f at hold %d by the failed probes", g_fg.cur, g_fg.k, g_fg.cur / gr, kmin);
        else if (gslow) w += snprintf(why + w, sizeof why - w, ", GPU %.2f× its time at hold %d", g_fg.cur / g_fg.ref, kmin);
        if (w >= (int)sizeof why) w = sizeof why - 1;
        if (nvn_fstep_same(&g_fs)) w += snprintf(why + w, sizeof why - w, ", cost %.2f× the cost that held hold %d", g_fs.ccur / g_fs.cref, kmin);
        if (w >= (int)sizeof why) w = sizeof why - 1;
        if (g_fs.uprobe) snprintf(why + w, sizeof why - w, ", GPU fits a shorter hold, frames not waiting in the pacing");
        if (g_fs.k > k0 && g_fs.cref > 0) snprintf(held, sizeof held, "; hold %d held at %.2f", kmin, g_fs.cref);   // what a probe compares the longer hold's cost with
        LOG("[nvn] frame step: hold %d → %d %s at present %llu (%s; cost %.2f, GPU %.2f%s)%s", k0, g_fs.k, fps_high() ? "ticks of 1/120 s" : "vsyncs", (unsigned long long)o->presents,
            fixed ? "the game's own step" : kmin0 > FSTEP_MAX ? "the game's own step ended" : kmin != kmin0 ? "the cap changed" : g_fs.k > k0 ? (g_fs.up_idle ? "30 late frames, some with the GPU idle" : "3 late frames") : g_fs.probes != probes0 ? why : "trial", cost, gpu, held, ++nlog == 40 ? "; further changes only counted below" : "");
    }
    if (probe0 == 1 && g_fs.probe != 1 && !fixed && nlog < 40) {   // whether the clock rose for the shorter deadline, and the GPU time there
        char gm[96] = "";
        if (g_fg.pn >= FSTEP_GPU_PN) snprintf(gm, sizeof gm, "; GPU %.2f at hold %d after its grace", g_fg.post, kmin);   // against the longer hold before and after it
        LOG("[nvn] frame step: the probe %s at present %llu (CPU %d%% on performance cores%s)%s", g_fs.probe == 2 ? "held" : "failed", (unsigned long long)o->presents, g_fs_pshare, gm, ++nlog == 40 ? "; further changes only counted below" : "");
    }
    else if (probe0 == 2 && !g_fs.probe && g_fs.failed != failed0 && nlog < 40)
        LOG("[nvn] frame step: the run at hold %d after the probe lasted under %.0f s, counted as a failed probe (CPU %d%% on performance cores)%s", kmin, FSTEP_HOLD_MAX / 60, g_fs_pshare, ++nlog == 40 ? "; further changes only counted below" : "");
    if (!dm_fps_state) {   // no game clock at 120 fps: the frames at each hold tell how long it held 120
        if (o->presents % 1800 == 0) { LOG("[nvn] 120 fps: held 1 / 2 ticks %llu / %llu frames, %llu late, %llu up / %llu down (%llu probes, %llu failed trials), %llu stalls; standing-frame drains taken out of %llu frames (%.1f ticks)", (unsigned long long)held[1], (unsigned long long)held[2], (unsigned long long)g_fs.late, (unsigned long long)g_fs.ups, (unsigned long long)g_fs.downs, (unsigned long long)g_fs.probes, (unsigned long long)g_fs.failed, (unsigned long long)g_fs.stalls, (unsigned long long)drain_n, drain_cut / v); memset(held, 0, sizeof held); drain_n = drain_cut = 0; }
    } else if (o->presents % 1800 == 0) {   // game time against real time (1.000 = normal speed)
        uint64_t vs = __atomic_load_n((uint64_t *)(dm_fps_state + 0x30), __ATOMIC_RELAXED) + fixed_sum + (uint64_t)measured_sum, t = now_ns();
        if (t0) LOG("[nvn] frame step: game time %.2f s in %.2f s (%.3f of real time); held 1 / 2 / 3 vsyncs %llu / %llu / %llu frames, %llu late, %llu vsyncs paid back, %.0f dropped, %llu up / %llu down (%llu probes, %llu failed trials), %llu stalls; standing-frame drains taken out of %llu frames (%.1f vsyncs)",
                    (vs - v0) / 60.0, (t - t0) / 1e9, (vs - v0) / 60.0 / ((t - t0) / 1e9), (unsigned long long)held[1], (unsigned long long)held[2], (unsigned long long)held[3],
                    (unsigned long long)g_fs.late, (unsigned long long)g_fs.paid, g_fs.dropped, (unsigned long long)g_fs.ups, (unsigned long long)g_fs.downs, (unsigned long long)g_fs.probes, (unsigned long long)g_fs.failed, (unsigned long long)g_fs.stalls,
                    (unsigned long long)drain_n, drain_cut / v);
        t0 = t; v0 = vs; memset(held, 0, sizeof held); drain_n = drain_cut = 0;
    }
}
NVN(nvnQueuePresentTexture, void, NVNqueue *q, NVNwindow *w, int idx) {
    (void)q; win_t *o = (win_t *)w; o->presents++;
    if (fps_high()) high_cap_tick();
    int movie = dm_movie_hz();
    if (dm_fps_state) __atomic_store_n((uint32_t *)(dm_fps_state + 4), (uint32_t)(movie != 0), __ATOMIC_RELAXED);   // the stubs' copy of the movie state
    int c = fps_step_cap();
    if (dm_fps_state) { static int was; if (c != was) { char why[48] = ""; if (movie) snprintf(why, sizeof why, " (a movie of %d pictures a second)", movie); else if (c == 30 && was == 60 && thermal_capped()) snprintf(why, sizeof why, " (thermal state serious)");
        LOG("[nvn] frame step cap %d → %d fps at present %llu%s", was, c, (unsigned long long)o->presents, why); was = c; } }
    if (c) fstep_tick(o, c);
    static uint64_t prev_in, prev_out, prev_pass, prev_draw;
    present_cfg(o);
    {
        // Test switch DM_TEST_PRESENT_LAG=every N presents,ms[,presents in a row][;…]: sleeps before those presents, imitating slow frames or hitches. Not set in the product
        static int lag_k = -1, lag_n[4], lag_ms[4], lag_run[4];
        if (lag_k < 0) {
            const char *e = getenv("DM_TEST_PRESENT_LAG"); lag_k = 0;
            while (e && *e && lag_k < 4) {
                int n = 0, ms = 0, r = 1, c = sscanf(e, "%d,%d,%d", &n, &ms, &r);
                if (c < 2 || n <= 0) break;
                lag_n[lag_k] = n; lag_ms[lag_k] = ms; lag_run[lag_k] = c == 3 && r > 1 ? r : 1;
                LOG("DM_TEST_PRESENT_LAG: every %d presents, sleep an extra %d ms (%d presents in a row)", n, ms, lag_run[lag_k]); lag_k++;
                if ((e = strchr(e, ';'))) e++;
            }
        }
        int lag = 0; for (int i = 0; i < lag_k; i++) if (o->presents % (uint64_t)lag_n[i] < (uint64_t)lag_run[i]) lag += lag_ms[i];
        if (lag) usleep((useconds_t)lag * 1000);
    }
    gfx_enter();
    uint64_t t0 = now_ns();
    if (idx >= 0 && idx < o->ntex && idx < 4) {   // enqueue only, return at once
        const tex_t *t = CTX(o->tex[idx]);
        nvn_queue_present_release(o->ticket[idx]);
        o->ticket[idx] = nvn_queue_present((queue_t *)q,t,present_period(o) / 1e9,o->crop);
    }
    gfx_leave();
    // DM_NVN_PRESENT_FINISH=1 (diagnostic): a present returns once the GPU ran the frame
    { static int fin = -1; if (fin < 0) { const char *e = getenv("DM_NVN_PRESENT_FINISH"); fin = e && atoi(e); if (fin) LOG("DM_NVN_PRESENT_FINISH: every frame waited for at present (diagnostic)"); }
      if (fin) nvn_queue_finish((queue_t *)q); }
    atomic_fetch_add(&g_nvn_ns, now_ns() - t0);
    uint64_t f = g_frame, ps, dr; mtl_stats(&ps, &dr);
    if (nvn_real[0] || nvn_real[1]) {   // DM_NVN_PROF: record phase = implemented functions except submit; submit phase = QueueSubmitCommands
        static unsigned isub = ~0u; static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
        if (isub == ~0u) for (isub = 0; isub < NVN_NFUNC && strcmp(nvn_names[isub], "nvnQueueSubmitCommands"); isub++) {}
        uint64_t rec = 0, sub = 0;
        static uint64_t acc[NVN_NFUNC];
        for (unsigned i = 0; i < NVN_NFUNC; i++) { uint64_t t = atomic_exchange(&nvn_prof_ticks[i], 0); acc[i] += t; if (i == isub) sub = t; else rec += t; }
        if ((g_frame + 1) % TWIN == 0) {   // every 300 frames, the 5 costliest functions
            char line[1024]; size_t o = 0;
            for (int k = 0; k < 5; k++) { unsigned best = 0; for (unsigned i = 1; i < NVN_NFUNC; i++) if (acc[i] > acc[best]) best = i;
                if (!acc[best]) break; o += snprintf(line + o, sizeof line - o, " %s %.2f ms;", nvn_names[best] + 3, acc[best] * tb.numer / tb.denom / 1e6); acc[best] = 0; }
            LOG("[nvn] translation layer top 5 by time (sum over the last 300 frames):%s", line); memset(acc, 0, sizeof acc);
        }
        trow(g_frame, "@rec_ms", rec * tb.numer / tb.denom / 1e6); trow(g_frame, "@submit_ms", sub * tb.numer / tb.denom / 1e6);
        tpush(4, rec * tb.numer / tb.denom / 1e6); tpush(5, sub * tb.numer / tb.denom / 1e6);
    }
    if (prev_in) {
        double fr = (t0 - prev_in) / 1e6, wk = (t0 - prev_out) / 1e6, nv = atomic_exchange(&g_nvn_ns, 0) / 1e6;
        tpush(0, fr); tpush(1, wk); tpush(2, nv);
        trow(f, "@frame_ms", fr); trow(f, "@work_ms", wk); trow(f, "@nvn_ms", nv); trow(f, "@sync_ms", atomic_exchange(&g_sync_ns, 0) / 1e6); trow(f, "@itv", (double)present_itv(o));
        // slow-frame attribution (DM_NVN_TSV): present-thread CPU (−1 if the thread changed), process CPU, acquire wait, present call time (ms since process start)
        trow(f, "@wait_ms", g_acq_wait_ns / 1e6); trow(f, "@pq_wait_ms", g_pq_wait_ns / 1e6);
        if (frame_rec()) {
            static pthread_t pth; static uint64_t pcpu, pproc;
            uint64_t c = clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID); pthread_t th = pthread_self();
            trow(f, "@cpu_ms", pcpu && pthread_equal(th, pth) ? (c - pcpu) / 1e6 : -1); pth = th; pcpu = c;
            struct rusage ru; getrusage(RUSAGE_SELF, &ru);
            uint64_t pc = (uint64_t)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000000000ull + (uint64_t)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) * 1000ull;
            if (pproc) trow(f, "@proc_cpu_ms", (pc - pproc) / 1e6); pproc = pc;
            trow(f, "@call_ms", (t0 - g_t0_ns) / 1e6);
        }
        trow(f, "@passes", (double)(ps - prev_pass)); trow(f, "@draws", (double)(dr - prev_draw));
        static uint64_t pl, pst, pf, pudc; uint64_t l, st, fu; mtl_bw(&l, &st, &fu);
        trow(f, "@rt_load_mb", (l - pl) / 1048576.0); trow(f, "@rt_store_mb", (st - pst) / 1048576.0); trow(f, "@pass_full_rw", (double)(fu - pf));
        uint64_t udc = mtl_udc_saved_bytes(); trow(f, "@unwritten_dc_mb", (udc - pudc) / 1048576.0);
        pl = l; pst = st; pf = fu; pudc = udc;
        static uint64_t pae, par, pap; uint64_t ae_, ar_, ap_; mtl_args_stats(&ae_, &ar_, &ap_);
        trow(f, "@args_enc", (double)(ae_ - pae)); trow(f, "@args_reuse", (double)(ar_ - par)); trow(f, "@args_patch", (double)(ap_ - pap)); pae = ae_; par = ar_; pap = ap_;
        static uint64_t puc, pus; uint64_t uc_, us_; mtl_use_stats(&uc_, &us_);
        trow(f, "@use_call", (double)(uc_ - puc)); trow(f, "@use_skip", (double)(us_ - pus)); puc = uc_; pus = us_;
        static uint64_t pfa, pre_; uint64_t fa_, pr_, up_, re_; nvn_dirty_stats(&fa_, &pr_); mtl_pkg_stats(&up_, &re_);
        trow(f, "@faults", (double)(fa_ - pfa)); trow(f, "@reuploads", (double)(re_ - pre_)); pfa = fa_; pre_ = re_;
        static uint64_t pmc, pmp, pmn; uint64_t mc_, mpg_, mn_; nvn_dirty_mp_stats(&mc_, &mpg_, &mn_);
        trow(f, "@mprotect_calls", (double)(mc_ - pmc)); trow(f, "@mprotect_pages", (double)(mpg_ - pmp)); trow(f, "@mprotect_ms", (mn_ - pmn) / 1e6); pmc = mc_; pmp = mpg_; pmn = mn_;
        double up_ms = mtl_upload_ns_take() / 1e6; trow(f, "@upload_ms", up_ms);
        trow(f, "@upload_gpu_ms", mtl_upload_gpu_ns_take() / 1e6);
        // synchronous creations on the submit thread this frame, timestamped on the UIMSG clock
        static uint64_t plib, ppso; uint64_t lib_, pso_; mtl_create_stats(&lib_, &pso_);
        trow(f, "@lib_new", (double)(lib_ - plib)); trow(f, "@pso_new", (double)(pso_ - ppso));
        {   // long-frame log: recorded now, judged 8 frames later when GPU time arrives
            static uint64_t plns, ppns, pup, prup, pub, phy; uint64_t lns, pns, up, rup, ub, hy, hm_, hn_; mtl_create_ns(&lns, &pns); mtl_pkg_stats(&up, &rup); ub = mtl_upload_bytes(); mtl_hybrid_stats(&hy, &hm_, &hn_);
            lf_rec *r = &g_lf[f % LFN]; r->f = f; r->fr = fr; r->nv = nv; r->t_s = (now_ns() - g_t0_ns) / 1e9; r->pso = (unsigned)(pso_ - ppso); r->lib = (unsigned)(lib_ - plib);
            r->make_ms = ((lns - plns) + (pns - ppns)) / 1e6; r->up = (unsigned)(up - pup); r->rup = (unsigned)(rup - prup); r->up_mb = (ub - pub) / 1048576.0; r->up_ms = up_ms;
            r->hy = (unsigned)(hy - phy); plns = lns; ppns = pns; pup = up; prup = rup; pub = ub; phy = hy;
        }
        plib = lib_; ppso = pso_;
        trow(f, "@t_s", (now_ns() - g_t0_ns) / 1e9);
        trow(f, "@drawable_ms", mtl_drawable_ns_take() / 1e6);
        trow(f, "@sub_wall_ms", atomic_exchange(&g_sub_wall_ns, 0) / 1e6); trow(f, "@sub_cpu_ms", atomic_exchange(&g_sub_cpu_ns, 0) / 1e6); trow(f, "@sub_instr", (double)atomic_exchange(&g_sub_instr, 0));
    } else atomic_store(&g_nvn_ns, 0);
    g_acq_wait_ns = g_pq_wait_ns = 0;
    if (f >= 8) { double g = mtl_gpu_ms(f - 8); if (g >= 0) { tpush(3, g); trow(f - 8, "@gpu_ms", g); }
        { lf_rec *r = &g_lf[(f - 8) % LFN]; if (g >= 0 && r->f == f - 8) mtl_ab_gpu(f - 8, g, r->fr); }   // DM_AB phases
        { lf_rec *r = &g_lf[(f - 8) % LFN];   // DM_LONGFRAME (default on): a line only when the frame > 20 ms or GPU > 16.7 ms and well above recent frames
          static int on = -1; if (on < 0) { const char *e = getenv("DM_LONGFRAME"); on = !(e && !strcmp(e, "0")); }
          // relative to the last 16 frames: a steady 30 fps or a sustained overload must not flood the log
          static double ef = 16.7, eg = 8; int lf = r->fr > 20.0 && r->fr > 1.4 * ef, lg = g > 16.7 && g > 1.3 * eg;
          if (r->f == f - 8) { ef += ((r->fr < 3 * ef ? r->fr : 3 * ef) - ef) / 16; eg += ((g < 3 * eg ? g : 3 * eg) - eg) / 16; }
          if (on && r->f == f - 8 && f - 8 > 0 && (lf || lg)) { char b[512];
              snprintf(b, sizeof b, "frame %llu (%.2f s) whole frame %.1f ms, submit+present %.1f ms, GPU %.1f ms; new pipelines %u + shader libraries %u, %.1f ms in all; texture uploads %u first + %u re-uploads, %.1f MB / %.1f ms; draws with a hybrid pipeline %u",
                       (unsigned long long)r->f, r->t_s, r->fr, r->nv, g, r->pso, r->lib, r->make_ms, r->up, r->rup, r->up_mb, r->up_ms, r->hy);
              dm_longframe_note(b); } }
        uint64_t g0, g1; if (frame_rec() && mtl_gpu_span(f - 8, &g0, &g1)) { trow(f - 8, "@gpu_beg_ms", (g0 - g_t0_ns) / 1e6); trow(f - 8, "@gpu_end_ms", (g1 - g_t0_ns) / 1e6); } }
    prev_in = t0; prev_pass = ps; prev_draw = dr;
    dump_frame();
    if (g_frame % TWIN == 0) tsummary(g_frame);
    // Present only enqueues; the pacer schedules this frame's display, and the texture is acquired again only at display time + one period
    uint64_t period = present_period(o), now = dm_detclock_enabled() ? guest_ns() : now_ns();
    g_hold_ring[o->presents % 16] = (uint8_t)present_itv(o);
    nvn_pacer pc = {o->pace};
    // The screen wait released this frame's acquire: the pacer takes the screen's phase rather than paying the wait back with short frames
    { uint64_t r = atomic_exchange(&g_screen_at, 0); if (r > pc.target) pc.target = r; }
    uint64_t slot = now;   // a frame the display link paced, or nothing did, is shown from its present on (nvn_pace_follow)
    if (o->follow) nvn_pace_follow(&pc, now); else slot += nvn_pace(&pc, now, period);
    o->pace = pc.target;
    if (idx >= 0 && idx < 4) o->avail[idx] = period ? slot + period : 0;
    static uint64_t prev_slot; if (prev_slot && period) trow(f, "@display_ms", (slot - prev_slot) / 1e6); prev_slot = slot;   // on-screen interval between adjacent frames: the one to watch for drops
    // actual presentation interval, read 8 presents late; −1 = discarded
    if (o->presents > 9) { uint64_t n = o->presents - 8; double a_ = mtl_presented_time(n + g_present_base), b_ = mtl_presented_time(n - 1 + g_present_base);
        if (a_ < 0) trow(f - 8, "@presented_ms", -1); else if (a_ > 0 && b_ > 0) trow(f - 8, "@presented_ms", (a_ - b_) * 1e3);
        g_pres_exp[g_presn % TWIN] = g_pres_exp_last = pres_expect_ms(n);
        g_pres_iv[g_presn++ % TWIN] = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0);
        { double iv_ = a_ < 0 ? -1 : (a_ > 0 && b_ > 0 ? (a_ - b_) * 1e3 : 0); mtl_ub_present(iv_); }   // DM_NVN_UB tallies on-screen intervals per phase; off, one atomic read
        if (a_ > 0) trow(f - 8, "@pres_at_ms", a_ * 1e3 - g_t0_ns / 1e6);   // ms since process start; presentedTime shares mach_absolute_time's base
        // the link tick that took this frame: target time (as @pres_at_ms), the callback's lead, callbacks since the previous frame's and their longest gap
        double lt, ll, lg; int lk;
        if (g_tsv && mtl_link_tick(n, &lt, &ll, &lk, &lg)) { trow(f - 8, "@link_target_ms", lt * 1e3 - g_t0_ns / 1e6); trow(f - 8, "@link_lead_ms", ll); trow(f - 8, "@link_ticks", lk); trow(f - 8, "@link_gap_ms", lg); } }
    if (f >= 8 && slow_on()) slow_emit(f - 8);
    prev_out = now_ns();
}

// State-only interfaces of newer SDKs: they read and write the object itself and record nothing; a capability the host lacks logs once and returns normally
#define ONCE_LOG(...) do { static int once_; if (!once_++) LOG("[nvn] " __VA_ARGS__); } while (0)

// GPU addresses use the host pointer; CPU mappings need a CPU-accessible pool
typedef struct { NVNdevice *dev; const pool_t *pool; int64_t off; uint64_t size; } buf_builder_t;   FITS(buf_builder_t, NVNbufferBuilder);
// guest ring allocators write off / size directly at +0x18 / +0x20
typedef struct { uint32_t magic; const pool_t *pool; uint8_t reserved[8]; int64_t off; uint64_t size; } buf_t; FITS(buf_t, NVNbuffer);
_Static_assert(offsetof(buf_t, off) == 0x18, "NVNbuffer guest offset");
_Static_assert(offsetof(buf_t, size) == 0x20, "NVNbuffer guest size");
#define BB(b) ((buf_builder_t *)(b))
#define BF(b) ((const buf_t *)(b))
NVN(nvnBufferBuilderSetDefaults, void, NVNbufferBuilder *b) { NVNdevice *d = BB(b)->dev; memset(b, 0, sizeof(buf_builder_t)); BB(b)->dev = d; }
NVN(nvnBufferBuilderSetDevice, void, NVNbufferBuilder *b, NVNdevice *d) { BB(b)->dev = d; }
NVN(nvnBufferBuilderGetDevice, NVNdevice *, const NVNbufferBuilder *b) { return ((const buf_builder_t *)b)->dev; }
NVN(nvnBufferBuilderSetStorage, void, NVNbufferBuilder *b, NVNmemoryPool *p, ptrdiff_t off, size_t n) { BB(b)->pool = (const pool_t *)p; BB(b)->off = off; BB(b)->size = n; }
NVN(nvnBufferInitialize, NVNboolean, NVNbuffer *buf, const NVNbufferBuilder *b) {
    const buf_builder_t *bb = (const buf_builder_t *)b;
    memset(buf, 0, sizeof(buf_t));
    // physical pools supply pages to virtual pools only
    if (bb->pool && (bb->pool->flags & NVN_MEMORY_POOL_FLAGS_PHYSICAL)) return 0;
    if (!bb->pool || !bb->pool->mem) { LOG("[RED] BufferInitialize: pool has no storage (pool %p flags %#x offset %#llx size %#llx)", (const void *)bb->pool, bb->pool ? bb->pool->flags : 0, (unsigned long long)bb->off, (unsigned long long)bb->size); return 0; }
    // NVN does not check a buffer's range against its pool, and the host's GPU access goes by address and length: a buffer past its pool is one log line
    if ((uint64_t)bb->off + bb->size > bb->pool->size) ONCE_LOG("BufferInitialize: a buffer reaches past the end of its pool (offset %#llx size %#llx pool %#llx); only an access that leaves the pool is an error, and it is reported where it happens", (unsigned long long)bb->off, (unsigned long long)bb->size, (unsigned long long)bb->pool->size);
    *(buf_t *)buf = (buf_t){.magic = MAGIC('B', 'U', 'F', 0), .pool = bb->pool, .off = bb->off, .size = bb->size};
    pool_mark_addressed(bb->pool);
    OLOG("BufferInitialize %p pool=%p off=%#llx size=%#llx", (void *)buf, (const void *)bb->pool, (unsigned long long)bb->off, (unsigned long long)bb->size);
    return 1;
}
NVN(nvnBufferFinalize, void, NVNbuffer *buf) { ((buf_t *)buf)->magic = 0; }
NVN(nvnBufferSetDebugLabel, void, NVNbuffer *buf, const char *s) { (void)buf; (void)s; }
NVN(nvnBufferGetAddress, NVNbufferAddress, const NVNbuffer *buf) { vpool_taken(BF(buf)->pool); return (NVNbufferAddress)(BF(buf)->pool->mem + BF(buf)->off); }
// NULL for a CPU_NO_ACCESS buffer or pool, as in NVN, though the host's storage is CPU memory: games compute GPU addresses from Map's result
NVN(nvnBufferMap, void *, const NVNbuffer *buf) { uint8_t *base = pool_cpu_map(BF(buf)->pool); return base ? base + BF(buf)->off : NULL; }
NVN(nvnBufferGetSize, size_t, const NVNbuffer *buf) { return (size_t)BF(buf)->size; }
NVN(nvnBufferGetMemoryPool, NVNmemoryPool *, const NVNbuffer *buf) { return (NVNmemoryPool *)BF(buf)->pool; }
NVN(nvnBufferGetMemoryOffset, ptrdiff_t, const NVNbuffer *buf) { return (ptrdiff_t)BF(buf)->off; }
NVN(nvnBufferFlushMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) {
    // unified memory still needs the flush tracking when a Metal texture mirrors this storage
    h_nvnMemoryPoolFlushMappedRange((const NVNmemoryPool *)BF(buf)->pool, BF(buf)->off + off, n);
}
NVN(nvnBufferInvalidateMappedRange, void, const NVNbuffer *buf, ptrdiff_t off, size_t n) { (void)buf; (void)off; (void)n; }

NVN(nvnMemoryPoolGetFlags, NVNmemoryPoolFlags, const NVNmemoryPool *p) { return (NVNmemoryPoolFlags)((const pool_t *)p)->flags; }
NVN(nvnMemoryPoolMap, void *, const NVNmemoryPool *p) { return pool_cpu_map((const pool_t *)p); }
NVN(nvnMemoryPoolGetSize, size_t, const NVNmemoryPool *p) { return (size_t)((const pool_t *)p)->size; }
NVN(nvnMemoryPoolInvalidateMappedRange, void, const NVNmemoryPool *p, ptrdiff_t off, size_t n) { (void)p; (void)off; (void)n; }
NVN(nvnMemoryPoolSetDebugLabel, void, NVNmemoryPool *p, const char *s) { (void)p; (void)s; }
// for NVN debugging tools only
NVN(nvnWindowSetDebugLabel, void, NVNwindow *w, const char *s) { (void)w; (void)s; }
NVN(nvnTexturePoolSetDebugLabel, void, NVNtexturePool *p, const char *s) { (void)p; (void)s; }
NVN(nvnSamplerPoolSetDebugLabel, void, NVNsamplerPool *p, const char *s) { (void)p; (void)s; }
NVN(nvnQueueSetDebugLabel, void, NVNqueue *q, const char *s) { (void)q; (void)s; }
NVN(nvnProgramSetDebugLabel, void, NVNprogram *p, const char *s) { (void)p; (void)s; }
NVN(nvnCommandBufferSetDebugLabel, void, NVNcommandBuffer *c, const char *s) { (void)c; (void)s; }
// Virtual pool (flags 0x200): MapVirtual maps physical pages in; a request is {physical pool, physical offset, virtual offset, size, storage class}.
// Requests with one base (same physical pool and offset delta) alias the physical memory directly; others make the pool sparse
typedef struct { NVNmemoryPool *phys; int64_t poff, voff; uint64_t size; int32_t storage_class, _pad; } map_req;
_Static_assert(sizeof(map_req) == 0x28, "NVNmappingRequest");
// Sparse virtual pools: a reserved range (at most 64 GB) where mappings vm_remap physical pages; each mapped run is a replay pool. NVN maps at 4 KB, the host
// at 16 KB, so a 4 KB page table backs sparse texture reads. A first mapping over the whole pool keeps the alias unless an address of the range left the host.
enum { VPT_LEAF = 512 };   // a page-table leaf covers 2 MB
typedef struct { const uint8_t *phys; uint64_t gen; uint32_t live; } vpte;   // live: mapped, and may still be under a zero-copy buffer the GPU uses (vunmap_defer)
struct vpool { const pool_t *pool; uint8_t *base; uint64_t len, size, top; int nrun, taken; struct vrun { uint64_t lo, hi; } *run; vpte **pt; struct vlog_ent *log; _Atomic uint64_t nlog; };   // len: reserved; top: highest mapped end so far
struct vlog_ent { uint64_t off, len; };   // the pool's change log, kept once a placement sparse texture asks for it
enum { VLOG_N = 4096 };
static struct vpool **g_vp; static int g_nvp, g_cvp;   // under g_vp_mu
static pthread_mutex_t g_vp_mu = PTHREAD_MUTEX_INITIALIZER;
static _Atomic uint64_t g_vp_gen;
// each request's host range by generation, for the textures they overlap (nvn_sparse_notes)
enum { VNOTE_N = 8192 };
static struct { uint64_t gen, lo, hi; } g_vnote[VNOTE_N];   // under g_vp_mu
static struct vpool *vpool_of(const pool_t *o) { return o->vp; }
// an address of the pool's own range left the host before its first mapping: the range stays and the first mapping makes the pool sparse
static void vpool_taken(const pool_t *o) {
    if (!o || !o->vp || o->vmode != 0) return;
    pthread_mutex_lock(&g_vp_mu); if (o->vp && o->vmode == 0) o->vp->taken = 1; pthread_mutex_unlock(&g_vp_mu);
}
static uint8_t *vpool_reserve(pool_t *o) {
    if (!(o->flags & 0x200)) return NULL;
    uint64_t len = o->size < (64ull << 30) ? o->size : 64ull << 30;
    len = (len + (uint64_t)getpagesize() - 1) & ~((uint64_t)getpagesize() - 1);
    void *a = mmap(NULL, len, PROT_NONE, MAP_ANON | MAP_PRIVATE, -1, 0);
    if (a == MAP_FAILED) { LOG("[RED] virtual pool %p: cannot reserve %#llx bytes of address space (errno %d)", (const void *)o, (unsigned long long)len, errno); return NULL; }
    struct vpool *v = calloc(1, sizeof *v);
    if (!v || !(v->pt = calloc((o->size + (2u << 20) - 1) >> 21, sizeof *v->pt))) die("sparse pool allocation failed");
    v->pool = o; v->base = a; v->len = len; v->size = o->size;
    pthread_mutex_lock(&g_vp_mu);
    if (g_nvp == g_cvp) g_vp = dm_realloc(g_vp, (g_cvp = g_cvp ? 2 * g_cvp : 64) * sizeof *g_vp, "the sparse pool table");
    g_vp[g_nvp++] = v; o->vp = v;
    o->vmode = o->size > DM_GAME_APP_POOL ? 2 : 0;
    pthread_mutex_unlock(&g_vp_mu);
    if (o->vmode == 2) LOG("[nvn] virtual pool %p (%#llx bytes, flags %#x): sparse, own address range %p+%#llx, physical pages are remapped into it", (const void *)o, (unsigned long long)o->size, o->flags, a, (unsigned long long)len);
    return a;
}
static void vpool_drop(pool_t *o) {   // caller holds g_vp_mu
    struct vpool *v = o->vp; if (!v) return;
    for (int k = 0; k < v->nrun; k++) mtl_pool_del(v->base + v->run[k].lo);
    munmap(v->base, v->len);
    for (uint64_t l = 0; l < (v->size + (2u << 20) - 1) >> 21; l++) free(v->pt[l]);
    for (int i = 0; i < g_nvp; i++) if (g_vp[i] == v) { g_vp[i] = g_vp[--g_nvp]; break; }
    free(v->pt); free(v->log); free(v->run); free(v); o->vp = NULL;
}
static void vunmap_run(int all); static int g_nvun;
// the GPU finishes submitted work first: deferred release keeps the buffers alive but not the pages under them (a GPU page fault otherwise)
static void vpool_release(pool_t *o) { if (o->vp && o->vp->nrun) mtl_finish(); pthread_mutex_lock(&g_vp_mu); if (g_nvun) vunmap_run(0); vpool_drop(o); pthread_mutex_unlock(&g_vp_mu); }
// Mapped runs: sorted, disjoint, adjacent merged, each a buffer the GPU addresses; caller holds g_vp_mu. A change adds at most two runs, and the pools change
// in one step (mtl_pool_replace): the replay looks addresses up meanwhile, and one that stays mapped must stay in a pool
static void vrun_set(struct vpool *v, uint64_t lo, uint64_t hi, int mapped) {
    struct vrun *nl = malloc(((size_t)v->nrun + 2) * sizeof *nl); int nn = 0, nd = 0, na = 0; uint64_t mlo = lo, mhi = hi;
    const void **del = malloc(((size_t)v->nrun + 1) * sizeof *del), *add[2]; uint64_t asz[2];
    if (!nl || !del) die("sparse pool run list allocation failed");
    for (int k = 0; k < v->nrun; k++) {
        uint64_t a = v->run[k].lo, b = v->run[k].hi;
        if (b < lo || a > hi || (!mapped && (b == lo || a == hi))) { nl[nn++] = (struct vrun){a, b}; continue; }   // untouched (an unmap leaves adjacent runs alone)
        del[nd++] = v->base + a;
        if (mapped) { if (a < mlo) mlo = a; if (b > mhi) mhi = b; continue; }
        // only the first run starts before the unmap and only the last ends after it: two pieces at most
        if (a < lo) { nl[nn++] = (struct vrun){a, lo}; add[na] = v->base + a; asz[na++] = lo - a; }   // NOLINT(clang-analyzer-security.ArrayBound): two pieces at most (above)
        if (b > hi) { nl[nn++] = (struct vrun){hi, b}; add[na] = v->base + hi; asz[na++] = b - hi; }   // NOLINT(clang-analyzer-security.ArrayBound): two pieces at most (above)
    }
    if (mapped) { nl[nn++] = (struct vrun){mlo, mhi}; add[na] = v->base + mlo; asz[na++] = mhi - mlo; }
    if (nd || na) mtl_pool_replace(del, nd, add, asz, na);
    free(del);
    for (int a = 1; a < nn; a++) for (int b = a; b > 0 && nl[b].lo < nl[b - 1].lo; b--) { struct vrun t = nl[b]; nl[b] = nl[b - 1]; nl[b - 1] = t; }
    if (nn > v->nrun && nn >= 1024 && !(nn & (nn - 1))) LOG("[nvn] virtual pool %p: %d separate mapped runs", (const void *)v->pool, nn);
    free(v->run); v->run = nl; v->nrun = nn;
}
// caller holds g_vp_mu
static const vpte *vpt_get(const struct vpool *v, uint64_t o) { const vpte *l = v->pt[o >> 21]; return l ? &l[(o >> 12) & (VPT_LEAF - 1)] : NULL; }
// for the address notes; takes g_vp_mu only if free (the crash dump may run with it held)
void nvn_vpool_note(uint64_t a, char *note, size_t n) {
    if (!n) return; note[0] = 0;
    if (pthread_mutex_trylock(&g_vp_mu)) { snprintf(note, n, " (sparse pools busy)"); return; }
    for (int i = 0; i < g_nvp; i++) { const struct vpool *v = g_vp[i]; uint64_t b = (uint64_t)(uintptr_t)v->base;
        if (a < b || a >= b + v->len) continue;
        const vpte *e = vpt_get(v, a - b); int run = -1;
        for (int r = 0; r < v->nrun; r++) if (a - b >= v->run[r].lo && a - b < v->run[r].hi) { run = r; break; }
        snprintf(note, n, " sparse pool %p (+%#llx of %#llx reserved, %d runs): page %s%s%#llx, %s", (void *)v->pool, (unsigned long long)(a - b), (unsigned long long)v->len, v->nrun,
                 e && e->phys ? "mapped" : "not mapped", e && e->phys ? " from physical " : "", e && e->phys ? (unsigned long long)(uintptr_t)(e->phys + ((a - b) & 0xfff)) : 0ull,
                 run >= 0 ? "in a mapped run" : "in no run");   // the physical address: where the CPU writes what the GPU reads here
        break; }
    pthread_mutex_unlock(&g_vp_mu);
}
// an unmapped page of a sparse reservation: the GPU reads 0 and drops stores
int nvn_vpool_unmapped(uint64_t a) {
    int r = 0; pthread_mutex_lock(&g_vp_mu);
    for (int i = 0; i < g_nvp; i++) { const struct vpool *v = g_vp[i]; uint64_t b = (uint64_t)(uintptr_t)v->base;
        if (a < b || a - b >= v->size) continue;
        const vpte *e = vpt_get(v, a - b); r = !e || !e->phys; break; }
    pthread_mutex_unlock(&g_vp_mu); return r;
}
static void vpt_set(struct vpool *v, uint64_t voff, uint64_t size, const uint8_t *phys) {
    uint64_t gen = atomic_fetch_add(&g_vp_gen, 1) + 1;
    g_vnote[gen % VNOTE_N].gen = gen; g_vnote[gen % VNOTE_N].lo = (uint64_t)(uintptr_t)v->base + voff; g_vnote[gen % VNOTE_N].hi = (uint64_t)(uintptr_t)v->base + voff + size;
    if (v->log) { v->log[atomic_load_explicit(&v->nlog, memory_order_relaxed) % VLOG_N] = (struct vlog_ent){voff, size}; atomic_fetch_add_explicit(&v->nlog, 1, memory_order_release); }
    for (uint64_t o = 0; o < size; o += 0x1000) {
        vpte **l = &v->pt[(voff + o) >> 21];
        if (!*l && !phys) continue;   // never mapped: stays unmapped
        if (!*l && !(*l = calloc(VPT_LEAF, sizeof **l))) die("sparse pool page table allocation failed");
        vpte *e = &(*l)[((voff + o) >> 12) & (VPT_LEAF - 1)]; *e = (vpte){phys ? phys + o : NULL, gen, phys ? 1u : e->live};
    }
}
// No page under a live buffer changes: a run's zero-copy buffer keeps the pages it was made over until its deferred release, so an unmap keeps them until that
// fence completes (unless remapped since), and a mapping over mapped pages waits for the GPU first (vpool_overwrites). Caller holds g_vp_mu
enum { VUNMAP_N = 4096 };
static struct { struct vpool *v; uint64_t off, len, gen, fence; } g_vun[VUNMAP_N];
static _Atomic uint64_t g_vp_kept, g_vp_waits, g_vp_released, g_vp_unmap_lost;
static void vunmap_defer(struct vpool *v, uint64_t off, uint64_t len) {
    if (g_nvun == VUNMAP_N) { atomic_fetch_add(&g_vp_unmap_lost, 1); return; }   // full: these pages stay kept (a later mapping over them waits)
    g_vun[g_nvun++] = (typeof(g_vun[0])){v, off, len, atomic_load(&g_vp_gen), mtl_last_fence() + 1};
}
static void vunmap_run(int all) {   // caller holds g_vp_mu; all: the pool is going and the GPU has finished
    uint64_t done = mtl_completed_fence(), pg = (uint64_t)getpagesize();
    for (int i = 0; i < g_nvun; ) {
        if (!all && g_vun[i].fence > done) { i++; continue; }
        struct vpool *v = g_vun[i].v; uint64_t off = g_vun[i].off, len = g_vun[i].len, gen = g_vun[i].gen;
        int alive = 0; for (int k = 0; k < g_nvp && !alive; k++) alive = g_vp[k] == v;
        g_vun[i] = g_vun[--g_nvun];
        if (!alive) continue;
        for (uint64_t a = (off + pg - 1) & ~(pg - 1); a + pg <= off + len && a + pg <= v->len; a += pg) {   // whole host pages only, none mapped since
            int ok = 1; for (uint64_t q = 0; q < pg && ok; q += 0x1000) { const vpte *e = vpt_get(v, a + q); ok = !e || (!e->phys && e->gen <= gen); }
            if (!ok) continue;
            if (mmap(v->base + a, pg, PROT_NONE, MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED) continue;
            for (uint64_t q = 0; q < pg; q += 0x1000) { vpte *e = (vpte *)vpt_get(v, a + q); if (e) e->live = 0; }
            atomic_fetch_add(&g_vp_released, 1);
        }
    }
}
static int vpool_overwrites(const struct vpool *v, const map_req *r) {   // caller holds g_vp_mu
    if (!r->phys || r->voff < 0 || (uint64_t)r->voff + r->size > v->size) return 0;
    for (uint64_t o = 0; o < r->size; o += 0x1000) { const vpte *e = vpt_get(v, (uint64_t)r->voff + o); if (e && e->live) return 1; }
    return 0;
}
static int vpool_map(struct vpool *v, const map_req *r) {   // caller holds g_vp_mu; 0 on failure
    uint64_t pg = (uint64_t)getpagesize();
    if (r->voff < 0 || (uint64_t)r->voff + r->size > v->size) { LOG("[RED] virtual pool %p: request at %#llx+%#llx is beyond the pool's %#llx", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)v->size); return 0; }
    if (((uint64_t)r->voff | r->size | (uint64_t)r->poff) & 0xFFF) { LOG("[RED] virtual pool %p: request %#llx+%#llx (physical offset %#llx) is not 4 KB aligned", (const void *)v->pool, (unsigned long long)r->voff, (unsigned long long)r->size, (unsigned long long)r->poff); return 0; }
    const pool_t *ph = (const pool_t *)r->phys;
    if (ph && (!ph->mem || r->poff < 0 || (uint64_t)r->poff + r->size > ph->size)) { LOG("[RED] virtual pool %p: physical pool %p (%#llx bytes, storage %p) cannot supply %#llx+%#llx", (const void *)v->pool, (const void *)ph, (unsigned long long)ph->size, (void *)ph->mem, (unsigned long long)r->poff, (unsigned long long)r->size); return 0; }
    vpt_set(v, (uint64_t)r->voff, r->size, ph ? ph->mem + r->poff : NULL);
    vm_address_t to = (vm_address_t)(v->base + r->voff);
    // not aliased: only textures read through the page table see it
    if ((uint64_t)r->voff + r->size > v->len || ((uintptr_t)to | r->size | (uint64_t)r->voff) & (pg - 1) || (ph && (((uintptr_t)ph->mem + (uintptr_t)r->poff) & (pg - 1)))) {
        static _Atomic uint64_t n; if (!atomic_fetch_add(&n, 1)) LOG("[nvn] virtual pool %p: a request %s (%s %#llx+%#llx) is kept in the page table only: textures read it, the GPU cannot address it (later cases are not logged)",
                                                                   (const void *)v->pool, (uint64_t)r->voff + r->size > v->len ? "beyond the reservation" : "not aligned to the host page", r->phys ? "map" : "unmap", (unsigned long long)r->voff, (unsigned long long)r->size);
        return 1;
    }
    if (!r->phys) {   // unmap: pages kept until the work that may use them completes; the page table says unmapped at once
        atomic_fetch_add(&g_vp_kept, 1);
        vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, 0);
        vunmap_defer(v, (uint64_t)r->voff, r->size); return 1;
    }
    uintptr_t from = (uintptr_t)ph->mem + (uintptr_t)r->poff;
    vm_prot_t cur, max;
    kern_return_t kr = vm_remap(mach_task_self(), &to, r->size, 0, VM_FLAGS_FIXED | VM_FLAGS_OVERWRITE, mach_task_self(), (vm_address_t)from, FALSE, &cur, &max, VM_INHERIT_NONE);
    if (kr != KERN_SUCCESS || to != (vm_address_t)(v->base + r->voff)) { LOG("[RED] virtual pool remap %#lx → %p (%#llx bytes) failed: %d", (unsigned long)from, (void *)(v->base + r->voff), (unsigned long long)r->size, kr); return 0; }
    mprotect((void *)to, r->size, PROT_READ | PROT_WRITE);   // the source may be write-protected by the dirty tracker; the alias is plain memory
    vrun_set(v, (uint64_t)r->voff, (uint64_t)r->voff + r->size, 1);
    uint64_t end = (uint64_t)r->voff + r->size;
    if (end >> 27 > v->top >> 27) LOG("[nvn] virtual pool %p: mapped up to %#llx (%d runs)", (const void *)v->pool, (unsigned long long)end, v->nrun);
    if (end > v->top) v->top = end;
    return 1;
}
NVN(nvnMemoryPoolMapVirtual, NVNboolean, NVNmemoryPool *p, int n, const NVNmappingRequest *rq) {
    pool_t *o = (pool_t *)p; const map_req *r = (const map_req *)rq;
    pthread_mutex_lock(&g_vp_mu);
    if (o->vp && o->vmode == 0 && n > 0) {   // the first mapping decides
        int whole = r[0].phys && r[0].voff == 0 && r[0].size == o->size;
        if (whole && !o->vp->taken) { vpool_drop(o); o->mem = NULL; o->vmode = 1; }
        else {
            o->vmode = 2;
            if (whole) LOG("[nvn] MapVirtual: virtual pool %p (%#llx bytes) gave out addresses of its own range before this first mapping: sparse, its physical pool %p's pages remapped into that range, not aliased",
                           (void *)o, (unsigned long long)o->size, (const void *)r[0].phys);
        }
    }
    struct vpool *v = o->vmode == 2 ? vpool_of(o) : NULL; int ok = 1;
    if (g_nvun) vunmap_run(0);
    if (v) {   // new pages over mapped ones: submitted work finishes first
        int over = 0; for (int i = 0; i < n && !over; i++) over = vpool_overwrites(v, &r[i]);
        if (over) {
            pthread_mutex_unlock(&g_vp_mu); mtl_finish(); uint64_t w = atomic_fetch_add(&g_vp_waits, 1) + 1; pthread_mutex_lock(&g_vp_mu); v = o->vmode == 2 ? vpool_of(o) : NULL;
            if (g_nvun) vunmap_run(0);
            if (w == 1 || !(w & (w - 1))) LOG("[nvn] sparse pools: %llu mapping requests over pages that may be under a live buffer waited for the GPU first (%llu unmaps deferred, %llu host pages released after their work, %d still queued, %llu kept for good: queue full)",
                                             (unsigned long long)w, (unsigned long long)atomic_load(&g_vp_kept), (unsigned long long)atomic_load(&g_vp_released), g_nvun, (unsigned long long)atomic_load(&g_vp_unmap_lost));
        }
    }
    for (int i = 0; v && i < n && ok; i++) ok = vpool_map(v, &r[i]);
    pthread_mutex_unlock(&g_vp_mu);
    if (v) return (NVNboolean)ok;
    for (int i = 0; i < n; i++) {
        const pool_t *ph = (const pool_t *)r[i].phys;
        if (!ph) continue;   // unmap: the host keeps the storage as is
        if (!ph->mem) { LOG("[RED] MapVirtual: physical pool %p has no storage", (const void *)ph); return 0; }
        uint8_t *base = ph->mem + r[i].poff - r[i].voff;
        if (r[i].voff < 0 || (uint64_t)r[i].voff + r[i].size > o->size || (uint64_t)r[i].poff + r[i].size > ph->size) { LOG("[RED] MapVirtual: request out of range (virtual %#llx+%#llx / pool %#llx, physical %#llx / pool %#llx)", (unsigned long long)r[i].voff, (unsigned long long)r[i].size, (unsigned long long)o->size, (unsigned long long)r[i].poff, (unsigned long long)ph->size); return 0; }
        if (o->mem && o->mem != base) { LOG("[RED] MapVirtual: non-contiguous mapping (true sparse) not implemented: virtual pool %p already maps to %p, this request wants %p", (void *)o, (void *)o->mem, (void *)base); return 0; }
        if (!o->mem) LOG("[nvn] MapVirtual: the storage of virtual pool %p (%#llx bytes) points at physical pool %p + %#llx", (void *)o, (unsigned long long)o->size, (const void *)ph, (unsigned long long)(r[i].poff - r[i].voff));
        o->mem = base;
        // A CPU_NO_ACCESS pool whose address was never given out holds only textures whose content Metal keeps apart: it stays out of the raw pool table
        if (o->asked || !(o->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS)) mtl_pool_addressed(o->mem, o->size);
    }
    return 1;
}

// reduction maps to the Metal 26 sampler reduction mode
NVN(nvnSamplerBuilderSetReductionFilter, void, NVNsamplerBuilder *b, NVNsamplerReduction r) { SB(b)->reduction = (uint8_t)r; }
NVN(nvnSamplerSetDebugLabel, void, NVNsampler *s, const char *l) { (void)s; (void)l; }
#define SP(s) (&((const samp_t *)(s))->p)
NVN(nvnSamplerGetMinMagFilter, void, const NVNsampler *s, NVNminFilter *mi, NVNmagFilter *ma) { *mi = (NVNminFilter)SP(s)->minf; *ma = (NVNmagFilter)SP(s)->magf; }
NVN(nvnSamplerGetWrapMode, void, const NVNsampler *s, NVNwrapMode *a, NVNwrapMode *b, NVNwrapMode *c) { *a = (NVNwrapMode)SP(s)->ws; *b = (NVNwrapMode)SP(s)->wt; *c = (NVNwrapMode)SP(s)->wr; }
NVN(nvnSamplerGetLodClamp, void, const NVNsampler *s, float *lo, float *hi) { *lo = SP(s)->lod_min; *hi = SP(s)->lod_max; }
NVN(nvnSamplerGetLodBias, float, const NVNsampler *s) { return SP(s)->lod_bias; }
NVN(nvnSamplerGetCompare, void, const NVNsampler *s, NVNcompareMode *m, NVNcompareFunc *f) { *m = (NVNcompareMode)SP(s)->cmp_mode; *f = (NVNcompareFunc)SP(s)->cmp_func; }
NVN(nvnSamplerGetBorderColor, void, const NVNsampler *s, float *c) { memcpy(c, SP(s)->border, 16); }
NVN(nvnSamplerGetMaxAnisotropy, float, const NVNsampler *s) { return SP(s)->aniso; }
NVN(nvnSamplerGetReductionFilter, NVNsamplerReduction, const NVNsampler *s) { return (NVNsamplerReduction)SP(s)->reduction; }
// games use it to deduplicate sampler pool entries
NVN(nvnSamplerCompare, NVNboolean, const NVNsampler *a, const NVNsampler *b) {
    const samp_par *x = SP(a), *y = SP(b);
    return x->minf == y->minf && x->magf == y->magf && x->ws == y->ws && x->wt == y->wt && x->wr == y->wr && x->cmp_mode == y->cmp_mode && x->cmp_func == y->cmp_func && x->reduction == y->reduction &&
           x->lod_min == y->lod_min && x->lod_max == y->lod_max && x->lod_bias == y->lod_bias && x->aniso == y->aniso && !memcmp(x->border, y->border, sizeof x->border);
}

NVN(nvnTextureBuilderSetSize2D, void, NVNtextureBuilder *b, int w, int h) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = 1; }
NVN(nvnTextureBuilderSetSize3D, void, NVNtextureBuilder *b, int w, int h, int d) { TB(b)->d.width = w; TB(b)->d.height = h; TB(b)->d.depth = d; }
NVN(nvnTextureBuilderSetSwizzle, void, NVNtextureBuilder *b, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    tex_builder_t *o = (tex_builder_t *)b; o->set_swz = 1; o->swz = (uint16_t)((r & 7) | (g & 7) << 3 | (bl & 7) << 6 | (a & 7) << 9);
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureBuilderSetSwizzle not identity (%d %d %d %d): remapped with a Metal view when sampling", (int)r, (int)g, (int)bl, (int)a);
}
// STENCIL: reads the stencil of its depth-stencil storage, as a stencil-mode view
NVN(nvnTextureBuilderSetDepthStencilMode, void, NVNtextureBuilder *b, NVNtextureDepthStencilMode m) { TB(b)->stencil = m == NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL; }
NVN(nvnTextureBuilderGetStorageClass, NVNstorageClass, const NVNtextureBuilder *b) { (void)b; return 0; }
// identity R G B A when unset; storage class: the host has none (0)
NVN(nvnTextureGetSwizzle, void, const NVNtexture *t, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    unsigned w = tex_swz(CTX(t));
    if (r) *r = (NVNtextureSwizzle)(w & 7); if (g) *g = (NVNtextureSwizzle)(w >> 3 & 7); if (b) *b = (NVNtextureSwizzle)(w >> 6 & 7); if (a) *a = (NVNtextureSwizzle)(w >> 9 & 7);
}
NVN(nvnTextureGetStorageClass, NVNstorageClass, const NVNtexture *t) { (void)t; return 0; }
NVN(nvnTextureBuilderGetSwizzle, void, const NVNtextureBuilder *b, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *bl, NVNtextureSwizzle *a) {
    const tex_builder_t *o = (const tex_builder_t *)b; unsigned w = o->set_swz ? o->swz : 2u | 3u << 3 | 4u << 6 | 5u << 9;
    if (r) *r = (NVNtextureSwizzle)(w & 7); if (g) *g = (NVNtextureSwizzle)(w >> 3 & 7); if (bl) *bl = (NVNtextureSwizzle)(w >> 6 & 7); if (a) *a = (NVNtextureSwizzle)(w >> 9 & 7);
}
NVN(nvnTextureBuilderGetZCullStorageSize, size_t, const NVNtextureBuilder *b) { return zcull_size(&TB(b)->d); }
// NVN copies it into the storage at TextureInitialize (the CPU cannot write a compressible pool)
NVN(nvnTextureBuilderSetPackagedTextureData, void, NVNtextureBuilder *b, const void *data) { TB(b)->pkg_data = data; }
NVN(nvnTextureGetFormat, NVNformat, const NVNtexture *t) { return (NVNformat)CTX(t)->d.format; }
NVN(nvnTextureGetTarget, NVNtextureTarget, const NVNtexture *t) { return (NVNtextureTarget)CTX(t)->d.target; }
NVN(nvnTextureSetDebugLabel, void, NVNtexture *t, const char *s) { (void)t; (void)s; }
NVN(nvnTextureViewGetLevels, NVNboolean, const NVNtextureView *v, int *base, int *n) { const view_t *w = (const view_t *)v; *base = w->base_level; *n = w->nlevels; return w->set_levels; }
NVN(nvnTextureViewGetTarget, NVNboolean, const NVNtextureView *v, NVNtextureTarget *t) { const view_t *w = (const view_t *)v; *t = (NVNtextureTarget)w->target; return w->set_target; }
NVN(nvnTextureViewSetDepthStencilMode, void, NVNtextureView *v, NVNtextureDepthStencilMode m) { VW(v)->set_dsmode = 1; VW(v)->dsmode = (uint8_t)m; }   // STENCIL: the replay binds a stencil-only view
NVN(nvnTextureViewSetSwizzle, void, NVNtextureView *v, NVNtextureSwizzle r, NVNtextureSwizzle g, NVNtextureSwizzle bl, NVNtextureSwizzle a) {
    view_t *w = VW(v); w->set_swizzle = 1; w->swz[0] = (uint8_t)r; w->swz[1] = (uint8_t)g; w->swz[2] = (uint8_t)bl; w->swz[3] = (uint8_t)a;
    if (r != NVN_TEXTURE_SWIZZLE_R || g != NVN_TEXTURE_SWIZZLE_G || bl != NVN_TEXTURE_SWIZZLE_B || a != NVN_TEXTURE_SWIZZLE_A) ONCE_LOG("TextureViewSetSwizzle not identity (%d %d %d %d): remapped with a Metal view when sampling", (int)r, (int)g, (int)bl, (int)a);
}

NVN(nvnColorStateSetAlphaTest, void, NVNcolorState *s, NVNalphaFunc f) { ST(color_st, s)->alpha_func = (uint32_t)f; }
NVN(nvnColorStateSetLogicOp, void, NVNcolorState *s, NVNlogicOp op) { ST(color_st, s)->logic_op = (uint32_t)op; }
// games read their state objects and builders back to build pipeline states
NVN(nvnColorStateGetBlendEnable, NVNboolean, const NVNcolorState *s, int i) { return i >= 0 && i < 8 && (((const color_st *)s)->blend_en >> i & 1); }
NVN(nvnColorStateGetAlphaTest, NVNalphaFunc, const NVNcolorState *s) { return (NVNalphaFunc)((const color_st *)s)->alpha_func; }
NVN(nvnColorStateGetLogicOp, NVNlogicOp, const NVNcolorState *s) { return (NVNlogicOp)((const color_st *)s)->logic_op; }
NVN(nvnChannelMaskStateGetChannelMask, void, const NVNchannelMaskState *s, int i, NVNboolean *r, NVNboolean *g, NVNboolean *b, NVNboolean *a) {
    uint32_t m = i >= 0 && i < 8 ? ((const chmask_st *)s)->mask >> (4 * i) & 0xf : 0;
    if (r) *r = (m & 1) != 0; if (g) *g = (m & 2) != 0; if (b) *b = (m & 4) != 0; if (a) *a = (m & 8) != 0;
}
NVN(nvnBlendStateGetBlendTarget, int, const NVNblendState *s) { return (int)((const blend_st *)s)->target; }
NVN(nvnBlendStateGetBlendFunc, void, const NVNblendState *s, NVNblendFunc *sr, NVNblendFunc *dr, NVNblendFunc *sa, NVNblendFunc *da) {
    const blend_st *o = (const blend_st *)s;
    if (sr) *sr = (NVNblendFunc)o->src_rgb; if (dr) *dr = (NVNblendFunc)o->dst_rgb; if (sa) *sa = (NVNblendFunc)o->src_a; if (da) *da = (NVNblendFunc)o->dst_a;
}
NVN(nvnBlendStateGetBlendEquation, void, const NVNblendState *s, NVNblendEquation *er, NVNblendEquation *ea) {
    const blend_st *o = (const blend_st *)s; if (er) *er = (NVNblendEquation)o->eq_rgb; if (ea) *ea = (NVNblendEquation)o->eq_a;
}
NVN(nvnBlendStateGetAdvancedMode, NVNblendAdvancedMode, const NVNblendState *s) { return (NVNblendAdvancedMode)((const blend_st *)s)->adv; }
NVN(nvnBlendStateGetAdvancedPremultipliedSrc, NVNboolean, const NVNblendState *s) { return !((const blend_st *)s)->adv_npm; }
NVN(nvnBlendStateGetAdvancedNormalizedDst, NVNboolean, const NVNblendState *s) { return !((const blend_st *)s)->adv_dst_free; }
NVN(nvnBlendStateGetAdvancedOverlap, NVNblendAdvancedOverlap, const NVNblendState *s) { return (NVNblendAdvancedOverlap)((const blend_st *)s)->adv_ovl; }
NVN(nvnPolygonStateGetCullFace, NVNface, const NVNpolygonState *s) { return (NVNface)((const poly_st *)s)->cull; }
NVN(nvnPolygonStateGetFrontFace, NVNfrontFace, const NVNpolygonState *s) { return (NVNfrontFace)((const poly_st *)s)->front; }
NVN(nvnPolygonStateGetPolygonMode, NVNpolygonMode, const NVNpolygonState *s) { return (NVNpolygonMode)((const poly_st *)s)->mode; }
NVN(nvnPolygonStateGetPolygonOffsetEnables, NVNpolygonOffsetEnable, const NVNpolygonState *s) { return (NVNpolygonOffsetEnable)((const poly_st *)s)->offset_en; }
NVN(nvnDepthStencilStateGetDepthTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dt; }
NVN(nvnDepthStencilStateGetDepthWriteEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->dw; }
NVN(nvnDepthStencilStateGetDepthFunc, NVNdepthFunc, const NVNdepthStencilState *s) { return (NVNdepthFunc)((const ds_st *)s)->df; }
NVN(nvnDepthStencilStateGetStencilTestEnable, NVNboolean, const NVNdepthStencilState *s) { return ((const ds_st *)s)->st; }
NVN(nvnDepthStencilStateGetStencilFunc, NVNstencilFunc, const NVNdepthStencilState *s, NVNface f) { const ds_st *o = (const ds_st *)s; return (NVNstencilFunc)(f == NVN_FACE_BACK ? o->sf_b : o->sf_f); }
NVN(nvnDepthStencilStateGetStencilOp, void, const NVNdepthStencilState *s, NVNface f, NVNstencilOp *fail, NVNstencilOp *zfail, NVNstencilOp *zpass) {
    const ds_st *o = (const ds_st *)s; int b = f == NVN_FACE_BACK;
    if (fail) *fail = (NVNstencilOp)(b ? o->fail_b : o->fail_f); if (zfail) *zfail = (NVNstencilOp)(b ? o->zfail_b : o->zfail_f); if (zpass) *zpass = (NVNstencilOp)(b ? o->zpass_b : o->zpass_f);
}
// recorded only: the replay uses the sample count, multisample enable and alpha to coverage
NVN(nvnMultisampleStateSetAlphaToCoverageDither, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->a2c_dither = B(e) != 0; }
NVN(nvnMultisampleStateSetRasterSamples, void, NVNmultisampleState *s, int n) { ST(ms_st, s)->raster_samples = (uint8_t)n; }
NVN(nvnMultisampleStateSetCoverageModulationMode, void, NVNmultisampleState *s, NVNcoverageModulationMode m) { ST(ms_st, s)->cov_mod = (uint8_t)m; }
NVN(nvnMultisampleStateSetCoverageToColorEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->cov2color_en = B(e) != 0; }
NVN(nvnMultisampleStateSetCoverageToColorOutput, void, NVNmultisampleState *s, int i) { ST(ms_st, s)->cov2color_out = (uint8_t)i; }
NVN(nvnMultisampleStateSetSampleLocationsGridEnable, void, NVNmultisampleState *s, uint32_t e) { ST(ms_st, s)->sl_grid = B(e) != 0; }
NVN(nvnMultisampleStateGetAlphaToCoverageEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c; }
NVN(nvnMultisampleStateGetAlphaToCoverageDither, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->a2c_dither; }
NVN(nvnMultisampleStateGetRasterSamples, int, NVNmultisampleState *s) { return ((const ms_st *)s)->raster_samples; }
NVN(nvnMultisampleStateGetCoverageModulationMode, NVNcoverageModulationMode, const NVNmultisampleState *s) { return (NVNcoverageModulationMode)((const ms_st *)s)->cov_mod; }
NVN(nvnMultisampleStateGetCoverageToColorEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_en; }
NVN(nvnMultisampleStateGetCoverageToColorOutput, int, const NVNmultisampleState *s) { return ((const ms_st *)s)->cov2color_out; }
NVN(nvnMultisampleStateGetSampleLocationsEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_en; }
NVN(nvnMultisampleStateGetSampleLocationsGridEnable, NVNboolean, const NVNmultisampleState *s) { return ((const ms_st *)s)->sl_grid; }
NVN(nvnBufferBuilderGetSize, size_t, const NVNbufferBuilder *b) { return (size_t)((const buf_builder_t *)b)->size; }
NVN(nvnBufferBuilderGetMemoryOffset, ptrdiff_t, const NVNbufferBuilder *b) { return (ptrdiff_t)((const buf_builder_t *)b)->off; }
// the pool getters return the pool's address, as nvnTextureGetMemoryPool
NVN(nvnBufferBuilderGetMemoryPool, NVNmemoryPool *, const NVNbufferBuilder *b) { return (NVNmemoryPool *)((const buf_builder_t *)b)->pool; }
NVN(nvnTextureBuilderGetMemoryPool, NVNmemoryPool *, const NVNtextureBuilder *b) { return (NVNmemoryPool *)((const tex_builder_t *)b)->pool; }
NVN(nvnTextureBuilderGetMemoryOffset, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)((const tex_builder_t *)b)->off; }
NVN_NOPROTO(nvnEventBuilderGetMemoryPool, NVNmemoryPool *, const NVNeventBuilder *b) { return (NVNmemoryPool *)((const ev_builder_t *)b)->pool; }
NVN_NOPROTO(nvnEventBuilderGetMemoryOffset, int64_t, const NVNeventBuilder *b) { return ((const ev_builder_t *)b)->off; }
NVN(nvnTextureBuilderGetTarget, NVNtextureTarget, const NVNtextureBuilder *b) { return (NVNtextureTarget)((const tex_builder_t *)b)->d.target; }
NVN(nvnTextureBuilderGetWidth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.width; }
NVN(nvnTextureBuilderGetHeight, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.height; }
NVN(nvnTextureBuilderGetDepth, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.depth; }
NVN(nvnTextureBuilderGetLevels, int, const NVNtextureBuilder *b) { return (int)((const tex_builder_t *)b)->d.levels; }
NVN(nvnTextureBuilderGetFormat, NVNformat, const NVNtextureBuilder *b) { return (NVNformat)((const tex_builder_t *)b)->d.format; }
NVN(nvnTextureBuilderGetSamples, int, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->d.samples; }
NVN(nvnTextureBuilderGetStride, ptrdiff_t, const NVNtextureBuilder *b) { return (ptrdiff_t)((const tex_builder_t *)b)->d.stride; }
NVN(nvnTextureBuilderGetPackagedTextureData, const void *, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->pkg_data; }
NVN(nvnTextureViewGetFormat, NVNboolean, const NVNtextureView *v, NVNformat *f) { const view_t *w = (const view_t *)v; if (f) *f = (NVNformat)w->format; return w->set_format; }
NVN(nvnTextureViewGetDepthStencilMode, NVNboolean, const NVNtextureView *v, NVNtextureDepthStencilMode *m) { const view_t *w = (const view_t *)v; if (m) *m = (NVNtextureDepthStencilMode)w->dsmode; return w->set_dsmode; }
NVN(nvnTextureViewGetSwizzle, NVNboolean, const NVNtextureView *v, NVNtextureSwizzle *r, NVNtextureSwizzle *g, NVNtextureSwizzle *b, NVNtextureSwizzle *a) {
    const view_t *w = (const view_t *)v; NVNtextureSwizzle *o[4] = {r, g, b, a};
    for (int c = 0; c < 4; c++) if (o[c]) *o[c] = (NVNtextureSwizzle)(w->set_swizzle ? w->swz[c] : NVN_TEXTURE_SWIZZLE_R + c);
    return w->set_swizzle;
}
NVN(nvnTextureGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtexture *t) { return ((const tex_t *)t)->stencil ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; }
NVN(nvnTextureBuilderGetDepthStencilMode, NVNtextureDepthStencilMode, const NVNtextureBuilder *b) { return ((const tex_builder_t *)b)->stencil ? NVN_TEXTURE_DEPTH_STENCIL_MODE_STENCIL : NVN_TEXTURE_DEPTH_STENCIL_MODE_DEPTH; }
// the host has one Metal queue
NVN_NOPROTO(nvnQueueBuilderSetQueuePriority, void, NVNqueueBuilder *b, int prio) { (void)b; (void)prio; }
// games may fetch it with GetProcAddress(NULL, …) and call it without a null check; there is no debugger to attach
NVN_NOPROTO(nvnSetExternalDebuggerEnabledNVX, void, NVNboolean enable) { (void)enable; }
// texture id | sampler id << 20, as the separate handles
NVN(nvnDeviceGetTextureHandle, NVNtextureHandle, const NVNdevice *d, int tex, int samp) { (void)d; return (NVNtextureHandle)((uint64_t)(uint32_t)tex | (uint64_t)(uint32_t)samp << 20); }
// a fetch reads no sampler: sampler ID 0
NVN(nvnDeviceGetTexelFetchHandle, NVNtextureHandle, const NVNdevice *d, int tex) { (void)d; return (NVNtextureHandle)(uint64_t)(uint32_t)tex; }
// to ns on the guest clock, as GetCurrentTimestampInNanoseconds
NVN(nvnDeviceGetTimestampInNanoseconds, uint64_t, const NVNdevice *d, const NVNcounterData *c) { (void)d; return nvn_timestamp_to_ns(c->timestamp); }
// the guest clock, as the counter timestamps
NVN(nvnDeviceGetCurrentTimestampInNanoseconds, uint64_t, const NVNdevice *d) { (void)d; return guest_ns(); }
NVN(nvnDeviceRegisterFastClearColor, NVNboolean, NVNdevice *d, const float *c, NVNformat f) { (void)d; (void)c; (void)f; return 1; }
// a fast clear value is only a hint to the Tegra hardware
NVN(nvnDeviceRegisterFastClearDepth, NVNboolean, NVNdevice *d, float v) { (void)d; (void)v; return 1; }
// a compression hint as for colour and depth: every value is accepted
NVN(nvnDeviceRegisterFastClearStencil, NVNboolean, NVNdevice *d, int v) { (void)d; (void)v; return 1; }
// the host compiles no shaders at run time
NVN(nvnDeviceSetIntermediateShaderCache, void, NVNdevice *d, int n) { (void)d; (void)n; }

// a count outside 1 to the swap chain's own: red line, all of them
static int active_count(int n, int ntex, const char *who) {
    if (n < 1 || (ntex && n > ntex)) { LOG("[RED] %s %d of %d textures: all of them stay active", who, n, ntex); return 0; }
    if (!ntex) LOG("[nvn] %s %d", who, n); else if (n != ntex) LOG("[nvn] %s %d of %d textures", who, n, ntex);
    return n;
}
NVN_NOPROTO(nvnWindowBuilderSetNumActiveTextures, void, NVNwindowBuilder *b, int n) { WB(b)->nactive = active_count(n, WB(b)->ntex, "WindowBuilderSetNumActiveTextures"); }   // SetTextures may come after (ntex 0): window_active checks again
NVN_NOPROTO(nvnWindowBuilderGetNumActiveTextures, int, const NVNwindowBuilder *b) { return WB(b)->nactive > 0 ? WB(b)->nactive : WB(b)->ntex; }
NVN_NOPROTO(nvnWindowSetNumActiveTextures, void, NVNwindow *w, int n) { win_t *o = (win_t *)w; o->nactive = active_count(n, o->ntex, "WindowSetNumActiveTextures"); }
NVN_NOPROTO(nvnWindowGetNumActiveTextures, int, const NVNwindow *w) { return window_active((const win_t *)w); }

// Record bytes accumulated across segments (cur may be in a host segment, below the guest begin). As in NVN, counted from the memory the game last added:
// a game may take chunk start + used as the GPU's position in a ring of command memory
NVN(nvnCommandBufferGetCommandMemoryUsed, size_t, const NVNcommandBuffer *c) { const cmd_t *m = (const cmd_t *)c;
    return m->begin ? (size_t)m->used : 0; }
// entries written into the last block, and those still waiting in ctl_q
static size_t ctl_used_now(const cmd_t *o) { size_t u = o->ctl_used + 8u * (size_t)(!!o->ctl_q[0] + !!o->ctl_q[1]); return u < o->ctl_size ? u : o->ctl_size; }
NVN(nvnCommandBufferGetControlMemoryUsed, size_t, const NVNcommandBuffer *c) { return ctl_used_now((const cmd_t *)c); }
NVN(nvnCommandBufferGetControlMemoryFree, size_t, const NVNcommandBuffer *c) { const cmd_t *o = (const cmd_t *)c; return o->ctl_size - ctl_used_now(o); }
NVN(nvnCommandBufferGetControlMemorySize, size_t, const NVNcommandBuffer *c) { return ((const cmd_t *)c)->ctl_size; }
// what is left in the current segment
NVN(nvnCommandBufferGetCommandMemoryFree, size_t, const NVNcommandBuffer *c) { const cmd_t *m = (const cmd_t *)c; return m->cur && m->end > m->cur ? (size_t)(m->end - m->cur) : 0; }
// only the game's AddCommandMemory changes it (docs/reference/nvn-objects.md#getcommandmemorysize-read-while-another-thread-records)
NVN(nvnCommandBufferGetCommandMemorySize, size_t, const NVNcommandBuffer *c) { return __atomic_load_n(&((const cmd_t *)c)->cmd_size, __ATOMIC_RELAXED); }
// ignored: the host records more than the driver, so it still calls the game's callback when memory runs out rather than lend control memory never reclaimed
NVN_NOPROTO(nvnCommandBufferSetCommandMemoryCallbackEnabled, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }
NVN_NOPROTO(nvnCommandBufferSetControlMemoryCallbackEnabled, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }

// NVN 55: each stage is NVNshaderData + an 8-byte extension field the host does not use
typedef struct { NVNbufferAddress data; const void *control; uint64_t ext; } shader_data_ext;
NVN_NOPROTO(nvnProgramSetShadersExt, NVNboolean, NVNprogram *p, int n, const shader_data_ext *sd) {
    if (n < 0 || n > 6) { LOG("[RED] ProgramSetShadersExt stage count %d", n); return 0; }
    NVNshaderData plain[6];
    for (int i = 0; i < n; i++) { plain[i].data = sd[i].data; plain[i].control = sd[i].control; if (sd[i].ext) ONCE_LOG("ProgramSetShadersExt extension field nonzero (%#llx): unused by the host", (unsigned long long)sd[i].ext); }
    return h_nvnProgramSetShaders(p, n, plain);
}

// NVN2 program builders keep the caller's arrays until initialization; ray tracing settings are kept for the getters, and a builder with ray tracing modules makes
// no program. Signatures recovered from the SDK, absent from nvn_protos.h
typedef void (*NVNT_nvnProgramBuilderSetDefaults)(void *);
typedef void (*NVNT_nvnProgramBuilderSetDevice)(void *, NVNdevice *);
typedef NVNdevice *(*NVNT_nvnProgramBuilderGetDevice)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetShaders)(void *, int, const NVNshaderData *);
typedef int (*NVNT_nvnProgramBuilderGetShaderCount)(const void *);
typedef const NVNshaderData *(*NVNT_nvnProgramBuilderGetShaders)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetRayTracingModules)(void *, int, const NVNshaderData *);
typedef int (*NVNT_nvnProgramBuilderGetRayTracingModuleCount)(const void *);
typedef const NVNshaderData *(*NVNT_nvnProgramBuilderGetRayTracingModuleData)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetMaxTraceRecursionDepth)(void *, uint32_t);
typedef uint32_t (*NVNT_nvnProgramBuilderGetMaxTraceRecursionDepth)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetMaxCallableRecursionDepth)(void *, uint32_t);
typedef uint32_t (*NVNT_nvnProgramBuilderGetMaxCallableRecursionDepth)(const void *);
typedef void (*NVNT_nvnProgramBuilderSetProgramRayTracingFlags)(void *, uint32_t);
typedef uint32_t (*NVNT_nvnProgramBuilderGetProgramRayTracingFlags)(const void *);
typedef NVNboolean (*NVNT_nvnProgramInitializeFromBuilder)(NVNprogram *, const void *);
typedef struct {
    NVNdevice *dev;
    const NVNshaderData *sd, *rt;
    int n, nrt;
    uint32_t trace_depth, callable_depth, flags;
} prog_builder_t;
_Static_assert(sizeof(prog_builder_t)<=0x48,"program builder exceeds verified caller storage");
NVN(nvnProgramBuilderSetDefaults, void, void *b) {
    prog_builder_t *o=b;NVNdevice *dev=o->dev;memset(o,0,sizeof *o);o->dev=dev;
    o->trace_depth=2;o->callable_depth=2; // SDK defaults
}
NVN(nvnProgramBuilderSetDevice, void, void *b, NVNdevice *d) { ((prog_builder_t *)b)->dev=d; }
NVN(nvnProgramBuilderGetDevice, NVNdevice *, const void *b) { return ((const prog_builder_t *)b)->dev; }
NVN(nvnProgramBuilderSetShaders, void, void *b, int n, const NVNshaderData *sd) { ((prog_builder_t *)b)->n=n;((prog_builder_t *)b)->sd=sd; }
NVN(nvnProgramBuilderGetShaderCount, int, const void *b) { return ((const prog_builder_t *)b)->n; }
NVN(nvnProgramBuilderGetShaders, const NVNshaderData *, const void *b) { return ((const prog_builder_t *)b)->sd; }
NVN(nvnProgramBuilderSetRayTracingModules, void, void *b, int n, const NVNshaderData *sd) { ((prog_builder_t *)b)->nrt=n;((prog_builder_t *)b)->rt=sd; }
NVN(nvnProgramBuilderGetRayTracingModuleCount, int, const void *b) { return ((const prog_builder_t *)b)->nrt; }
NVN(nvnProgramBuilderGetRayTracingModuleData, const NVNshaderData *, const void *b) { return ((const prog_builder_t *)b)->rt; }
NVN(nvnProgramBuilderSetMaxTraceRecursionDepth, void, void *b, uint32_t depth) { ((prog_builder_t *)b)->trace_depth=depth; }
NVN(nvnProgramBuilderGetMaxTraceRecursionDepth, uint32_t, const void *b) { return ((const prog_builder_t *)b)->trace_depth; }
NVN(nvnProgramBuilderSetMaxCallableRecursionDepth, void, void *b, uint32_t depth) { ((prog_builder_t *)b)->callable_depth=depth; }
NVN(nvnProgramBuilderGetMaxCallableRecursionDepth, uint32_t, const void *b) { return ((const prog_builder_t *)b)->callable_depth; }
NVN(nvnProgramBuilderSetProgramRayTracingFlags, void, void *b, uint32_t flags) { ((prog_builder_t *)b)->flags=flags; }
NVN(nvnProgramBuilderGetProgramRayTracingFlags, uint32_t, const void *b) { return ((const prog_builder_t *)b)->flags; }
NVN(nvnProgramInitializeFromBuilder, NVNboolean, NVNprogram *p, const void *b) {
    const prog_builder_t *o = (const prog_builder_t *)b;
    if (!h_nvnProgramInitialize(p,o->dev)) return 0;
    if (o->n<0 || o->n>6 || o->nrt<0 || (o->n && !o->sd) || (o->nrt && !o->rt) || (o->n && o->nrt)) {
        LOG("[RED] invalid or mixed raster/RT program builder: shaders=%d modules=%d",o->n,o->nrt);return 0;
    }
    if (o->nrt) { LOG("[RED] nvnProgramInitializeFromBuilder with %d ray tracing modules: the host has no ray tracing (no shader of the game is a ray tracing module); no program", o->nrt); return 0; }
    return !o->n || h_nvnProgramSetShaders(p,o->n,o->sd);
}
// the host runs every queue's submits in order on one Metal queue: the tie needs no work
NVN_NOPROTO(nvnQueueBuilderSetPrimaryQueue, void, NVNqueueBuilder *b, NVNqueue *primary) { (void)b; (void)primary; }

// Metal's depth buffers keep their own compression
NVN_NOPROTO(nvnCommandBufferSetZCullZF32CompressionEnable, void, NVNcommandBuffer *c, uint32_t on) { (void)c; (void)on; }

// Native images: a decoded video picture as a linear RGBA8 texture over nn::codec's pool memory, so a new picture needs no upload. Imported by name, not in the proc table
__attribute__((weak)) int dm_native_image(uint32_t handle, uint8_t **mem, uint64_t *size, uint32_t *w, uint32_t *h, uint32_t *stride) { (void)handle; (void)mem; (void)size; (void)w; (void)h; (void)stride; return 0; }   // graphics unit tests have no decoder
typedef struct { NVNdevice *dev; uint32_t handle; NVNformat format; } native_builder_t;   // the game gives the builder 0x100 bytes
typedef struct { NVNdevice *dev; uint32_t handle; NVNformat format; uint32_t has_pool; NVNmemoryPool pool; } native_image_t;   // and the image 0x200
_Static_assert(sizeof(native_image_t) <= 0x200, "the game's NVNnativeImage");
HLE_FORCE("_Z32nvnNativeImageBuilderSetDefaultsP21NVNnativeImageBuilder", "native images are the host's", void, h_ni_defaults, native_builder_t *b) { NVNdevice *d = b->dev; memset(b, 0, sizeof *b); b->dev = d; }   // the game sets the device first
HLE_FORCE("_Z30nvnNativeImageBuilderSetDeviceP21NVNnativeImageBuilderP9NVNdevice", "native images are the host's", void, h_ni_device, native_builder_t *b, NVNdevice *d) { b->dev = d; }
HLE_FORCE("_Z41nvnNativeImageBuilderSetNativeImageHandleP21NVNnativeImageBuilderN2nn2gs17NativeImageHandleE", "native images are the host's", void, h_ni_handle, native_builder_t *b, uint32_t h) { b->handle = h; }
HLE_FORCE("_Z40nvnNativeImageBuilderSetMemoryPermissionP21NVNnativeImageBuilder30NVNnativeImageMemoryPermission", "native images are the host's: the permission has no meaning there", void, h_ni_perm, native_builder_t *b, int p) { (void)b; (void)p; }
HLE_FORCE("_Z30nvnNativeImageBuilderSetFormatP21NVNnativeImageBuilder9NVNformat", "native images are the host's", void, h_ni_format, native_builder_t *b, NVNformat f) { b->format = f; }
HLE_FORCE("_Z24nvnNativeImageInitializeP14NVNnativeImagePK21NVNnativeImageBuilder", "native images are the host's", NVNboolean, h_ni_init, native_image_t *i, const native_builder_t *b) {
    i->dev = b->dev; i->handle = b->handle; i->format = b->format; i->has_pool = 0;
    return 1;
}
HLE_FORCE("_Z22nvnNativeImageFinalizeP14NVNnativeImage", "native images are the host's", void, h_ni_fini, native_image_t *i) {
    if (i->has_pool) h_nvnMemoryPoolFinalize(&i->pool);
    i->has_pool = 0;
}
HLE_FORCE("_Z35nvnTextureInitializeFromNativeImageP10NVNtextureP14NVNnativeImage", "a linear texture over the picture's memory", NVNboolean, h_ni_texture, NVNtexture *t, native_image_t *i) {
    uint8_t *mem; uint64_t size; uint32_t w, h, stride;
    if (!dm_native_image(i->handle, &mem, &size, &w, &h, &stride)) { LOG("[RED] nvnTextureInitializeFromNativeImage: handle %#x names no picture", i->handle); return 0; }
    if (!i->has_pool) {
        NVNmemoryPoolBuilder pb; h_nvnMemoryPoolBuilderSetDefaults(&pb); h_nvnMemoryPoolBuilderSetDevice(&pb, i->dev); h_nvnMemoryPoolBuilderSetStorage(&pb, mem, size);
        h_nvnMemoryPoolBuilderSetFlags(&pb, NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_GPU_CACHED);
        if (!h_nvnMemoryPoolInitialize(&i->pool, &pb)) return 0;
        i->has_pool = 1;
    }
    NVNtextureBuilder tb; h_nvnTextureBuilderSetDefaults(&tb); h_nvnTextureBuilderSetDevice(&tb, i->dev);
    h_nvnTextureBuilderSetTarget(&tb, NVN_TEXTURE_TARGET_2D); h_nvnTextureBuilderSetFormat(&tb, i->format); h_nvnTextureBuilderSetSize2D(&tb, (int)w, (int)h); h_nvnTextureBuilderSetLevels(&tb, 1);
    h_nvnTextureBuilderSetFlags(&tb, NVN_TEXTURE_FLAGS_LINEAR); h_nvnTextureBuilderSetStride(&tb, stride); h_nvnTextureBuilderSetStorage(&tb, &i->pool, 0);
    return h_nvnTextureInitialize(t, &tb);
}

// NVN2's DLSS: the replay takes MetalFX's temporal scaler (dlss_eval). Builder and parameters are packed in place (the builder is 0x108 bytes of the game's stack);
// the network's texture IDs and storage are taken and dropped.
typedef struct { const NVNtexture *tex[DLSS_TEXTURES]; int32_t iw, ih, base[DLSS_TEXTURES][2]; float mv[2], jitter[2], pre_exposure; uint32_t reset; } dlss_params_t;
static inline int dlss_ceil_div(int v, int d) { return (v + d - 1) / d; }
// output / input by method (the game's choice: 2 and under is 0, 3 is 1, 1 is 2)
static int dlss_factor(int method) { return method == 1 ? 3 : method == 2 ? 1 : 2; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetDefaults, void, dlss_builder_t *b) { memset(b, 0, sizeof *b); }
NVN_NOPROTO(nvnDlssNetworkBuilderSetDevice, void, dlss_builder_t *b, NVNdevice *d) { (void)b; (void)d; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetOutputResolution, void, dlss_builder_t *b, int w, int h) { b->w = w; b->h = h; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetMethod, void, dlss_builder_t *b, int m) { b->method = m; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetPreset, void, dlss_builder_t *b, int p) { b->preset = p; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetFlags, void, dlss_builder_t *b, uint32_t f) { b->flags = f; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetMaxEvaluationEntries, void, dlss_builder_t *b, int n) { (void)b; (void)n; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetTextureIDs, void, dlss_builder_t *b, const void *pool, const int *ids, int n) { (void)b; (void)pool; (void)ids; (void)n; }
NVN_NOPROTO(nvnDlssNetworkBuilderSetStorage, void, dlss_builder_t *b, void *mem, size_t n) { (void)b; (void)mem; (void)n; }
NVN_NOPROTO(nvnDlssNetworkBuilderGetStorageSize, size_t, const dlss_builder_t *b) { (void)b; return 0x1000; }   // the scaler's memory is the host's; a page for a game that allocates it
// optimal, smallest and largest input, then sharpness (not NGX's order; open: one game's use). A game evaluates only inside the range: a third of the output up to it
NVN_NOPROTO(nvnDlssNetworkBuilderGetOptimalSettings, NVNboolean, const dlss_builder_t *b, int *ow, int *oh, int *minw, int *minh, int *maxw, int *maxh, float *sharp) {
    int f = dlss_factor(b->method);
    if (ow) *ow = dlss_ceil_div(b->w, f);
    if (oh) *oh = dlss_ceil_div(b->h, f);
    if (minw) *minw = dlss_ceil_div(b->w, 3);
    if (minh) *minh = dlss_ceil_div(b->h, 3);
    if (maxw) *maxw = b->w;
    if (maxh) *maxh = b->h;
    if (sharp) *sharp = 0;
    return 1;
}
NVN_NOPROTO(nvnDlssNetworkInitialize, NVNboolean, dlss_net_t *n, const dlss_builder_t *b) {
    n->b = *b; n->serial = nvn_new_serial();   // a network made again in the same memory is another network to the replay
    LOG("[nvn] DLSS network %dx%d, method %d (input 1/%d), preset %d, flags %#x: MetalFX's temporal scaler stands in", b->w, b->h, b->method, dlss_factor(b->method), b->preset, b->flags);
    return 1;
}
// no evaluation names the serial again, so the replay's cache reuses its slot
NVN_NOPROTO(nvnDlssNetworkFinalize, void, dlss_net_t *n) { (void)n; }
// the replay takes sizes from each evaluation, so the network keeps nothing of it
NVN_NOPROTO(nvnDlssNetworkResize, NVNboolean, dlss_net_t *n, int w, int h) {
    (void)n; static int once; if (!once++) LOG("[nvn] DLSS network resized to %dx%d (each evaluation's own sizes are used)", w, h);
    return 1;
}
NVN_NOPROTO(nvnDlssParametersSetDefaults, void, dlss_params_t *p) { memset(p, 0, sizeof *p); p->pre_exposure = 1; p->mv[0] = p->mv[1] = 1; }
static void dlss_set_tex(dlss_params_t *p, int k, const NVNtexture *t, const NVNtextureView *v) {
    p->tex[k] = t;
    if (v) ONCE_LOG("[RED] DLSS parameter %d given a texture view: the whole texture is used", k);
}
NVN_NOPROTO(nvnDlssParametersSetInputColor, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_COLOR, t, v); }
NVN_NOPROTO(nvnDlssParametersSetDepth, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_DEPTH, t, v); }
NVN_NOPROTO(nvnDlssParametersSetMotionVectors, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_MOTION, t, v); }
NVN_NOPROTO(nvnDlssParametersSetOutputColor, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_OUTPUT, t, v); }
NVN_NOPROTO(nvnDlssParametersSetBiasCurrentColorMask, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_BIAS, t, v); }
NVN_NOPROTO(nvnDlssParametersSetExposureTexture, void, dlss_params_t *p, const NVNtexture *t, const NVNtextureView *v) { dlss_set_tex(p, DLSS_EXPOSURE, t, v); }
NVN_NOPROTO(nvnDlssParametersSetInputSubrectangleSize, void, dlss_params_t *p, int w, int h) { p->iw = w; p->ih = h; }
NVN_NOPROTO(nvnDlssParametersSetInputColorSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_COLOR][0] = x; p->base[DLSS_COLOR][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetDepthSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_DEPTH][0] = x; p->base[DLSS_DEPTH][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetMotionVectorSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_MOTION][0] = x; p->base[DLSS_MOTION][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetOutputSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_OUTPUT][0] = x; p->base[DLSS_OUTPUT][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetBiasCurrentColorSubrectangleBase, void, dlss_params_t *p, int x, int y) { p->base[DLSS_BIAS][0] = x; p->base[DLSS_BIAS][1] = y; }
NVN_NOPROTO(nvnDlssParametersSetMotionVectorScale, void, dlss_params_t *p, float x, float y) { p->mv[0] = x; p->mv[1] = y; }
NVN_NOPROTO(nvnDlssParametersSetJitterOffset, void, dlss_params_t *p, float x, float y) { p->jitter[0] = x; p->jitter[1] = y; }
NVN_NOPROTO(nvnDlssParametersSetPreExposure, void, dlss_params_t *p, float e) { p->pre_exposure = e; }
NVN_NOPROTO(nvnDlssParametersSetHistoryReset, void, dlss_params_t *p, uint32_t on) { p->reset = on & 0xff; }   // NVNboolean: low 8 bits
NVN_NOPROTO(nvnDlssParametersSetEvaluationIndex, void, dlss_params_t *p, int i) { (void)p; (void)i; }   // the replay runs the network's frames in order
NVN_NOPROTO(nvnCommandBufferEvaluateDlss, void, NVNcommandBuffer *c, const dlss_net_t *n, const dlss_params_t *p) {
    p_dlss *o = REC_SNAP(c, OP_DLSS, p_dlss, DLSS_TEXTURES);
    for (int k = 0; k < DLSS_TEXTURES; k++) {
        o->tex[k] = tex_snap(o, SNAP_OFF(p_dlss), k, p->tex[k], (uint64_t)__builtin_return_address(0));
        memcpy(o->base[k], p->base[k], sizeof o->base[k]);
    }
    o->net = n->serial; o->iw = p->iw; o->ih = p->ih; o->ow = n->b.w; o->oh = n->b.h; o->flags = n->b.flags;
    memcpy(o->mv, p->mv, sizeof o->mv); memcpy(o->jitter, p->jitter, sizeof o->jitter); o->pre_exposure = p->pre_exposure; o->reset = p->reset != 0;
}
// NVN2 push constants (block, byte offset, size, data): a constant block the shaders read as c[24] (DMSH_PUSH_BANK). Block 1 + i holds stage i's buffer entries
// (DMSH_PUSH_BLOCKS); compute reads block 1 at DMSH_PUSH_SIZE in c[24]
static void push_const(NVNcommandBuffer *c, int compute, uint64_t w, uint64_t off, uint64_t n, const void *data) {
    const uint64_t blocks = DMSH_PUSH_BLOCKS;
    if (w >= blocks) { static int once; if (!once++) LOG("[RED] %s push constants into block %llu (%#llx + %#llx): the host keeps %llu, dropped", compute ? "compute" : "graphics", (unsigned long long)w, (unsigned long long)off, (unsigned long long)n, (unsigned long long)blocks); return; }
    if (off > DMSH_PUSH_SIZE || n > DMSH_PUSH_SIZE - off || !data) { static int k; if (k++ < 8) LOG("[RED] push constants at %#llx + %#llx: beyond the host's %#x-byte block, dropped", (unsigned long long)off, (unsigned long long)n, DMSH_PUSH_SIZE); return; }
    p_push_const *r = RECN(c, OP_PUSH_CONST, p_push_const, data, n); r->compute = (uint16_t)compute; r->offset = (uint16_t)(w * DMSH_PUSH_SIZE + off); r->size = (uint32_t)n; memcpy(r->data, data, n);
}
NVN_NOPROTO(nvnCommandBufferPushGraphicsConstants, void, NVNcommandBuffer *c, uint64_t w, uint64_t off, uint64_t n, const void *data) { push_const(c, 0, w, off, n, data); }
NVN_NOPROTO(nvnCommandBufferPushComputeConstants, void, NVNcommandBuffer *c, uint64_t w, uint64_t off, uint64_t n, const void *data) { push_const(c, 1, w, off, n, data); }
// NVN2 front-end modes (compute overlap, geometry processing, recording) change no result of an in-order Metal replay: logged once each (open: what they select).
// Provoking vertex: 0 FIRST, 1 LAST (3D method 0x1684); Metal's flat interpolation takes the first vertex, and LAST has no Metal setting
NVN_NOPROTO(nvnCommandBufferSetProvokingVertexMode, void, NVNcommandBuffer *c, int mode) {
    (void)c; if (mode) { static int once; if (!once++) LOG("[RED] nvnCommandBufferSetProvokingVertexMode %d (the last vertex): Metal's flat values come from the first vertex", mode); }
}
// 3D method 0x10b0 bit 0: an overflowing float blend result becomes infinity (0) or the format's largest finite value (1). Metal stores infinity (mode 0);
// mode 1 would take a target read in every blending shader. No frame seen to differ
NVN_NOPROTO(nvnCommandBufferSetFloatBlendClamp, void, NVNcommandBuffer *c, uint32_t to_max) {
    (void)c; if (to_max) { static int once; if (!once++) LOG("[RED] nvnCommandBufferSetFloatBlendClamp %u (a float blend result that overflows becomes the largest finite value): Metal stores infinity", to_max); }
}
NVN_NOPROTO(nvnCommandBufferSetRecordingMode, void, NVNcommandBuffer *c, int mode) { (void)c; ONCE_LOG("nvnCommandBufferSetRecordingMode %d: the host records every command buffer alike", mode); }
NVN_NOPROTO(nvnCommandBufferSetGeometryProcessingMode, void, NVNcommandBuffer *c, int mode) { (void)c; ONCE_LOG("nvnCommandBufferSetGeometryProcessingMode %d: no effect on Metal", mode); }
NVN_NOPROTO(nvnCommandBufferSetComputeWorkloadPriority, void, NVNcommandBuffer *c, int a, int b) { (void)c; ONCE_LOG("nvnCommandBufferSetComputeWorkloadPriority %d %d: Metal runs compute in submission order, with no priority", a, b); }
NVN_NOPROTO(nvnCommandBufferSetComputeOverlapMode, void, NVNcommandBuffer *c, int mode) { (void)c; ONCE_LOG("nvnCommandBufferSetComputeOverlapMode %d: the host runs compute in submission order", mode); }
// skipping fragments is allowed, never required: Metal running every fragment is valid for either setting
NVN_NOPROTO(nvnCommandBufferSetRepresentativeFragmentTest, void, NVNcommandBuffer *c, int enable) { (void)c; ONCE_LOG("nvnCommandBufferSetRepresentativeFragmentTest %d: Metal runs every fragment, which either setting allows", enable); }

// Metal runs a fragment function per sample only when it reads [[sample_id]] or interpolates at a sample, so the mode is not applied; on single-sample targets the two agree
NVN_NOPROTO(nvnProgramSetSampleShading, NVNboolean, NVNprogram *p, int mode) { (void)p; ONCE_LOG("nvnProgramSetSampleShading mode %d: Metal shades per sample only where the shader reads the sample index", mode); return 1; }

// Newer SDKs' commands, mapped onto existing records; state the replay lacks logs once and drawing continues (over-draw rather than drop)
#include "../tex/nvn_deswizzle.h"

// combined handle recorded as is (texture in the low 20 bits), laid out as the hardware's command: the handle 16 bytes in, where a game may patch it (p_bind_tex1)
NVN(nvnCommandBufferBindTexture, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNtextureHandle h) { st = st_host(st);
    if (!slots_bound("BindTexture", st, i, 1, NVN_SEPTEX_SLOT0 + DMSH_C2_SEP_SLOTS)) return;
    p_bind_tex1 *r = REC(c, OP_BIND_TEX1, p_bind_tex1); r->stage = st; r->slot = i; r->h = (uint64_t)h;
}
_Static_assert(offsetof(p_bind_tex1, h) == 8, "a single texture bind keeps its handle 16 bytes into the record (8 of header)");
NVN(nvnCommandBufferBindTextures, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { st = st_host(st); bind_handles(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h); }
// Deferred: the GPU reads the handles at h when it executes the command (recorded once, submitted repeatedly, handles rewritten); the replay reads them then (bh_fresh)
NVN_NOPROTO(nvnCommandBufferBindTexturesDeferred, void, NVNcommandBuffer *c, NVNshaderStage st, int first, int n, const NVNtextureHandle *h) { st = st_host(st);
    p_bind_handles *r = bind_handles_ex(c, OP_BIND_TEX, st, first, n, (const uint64_t *)h, 0);
    if (r && h) { r->defer = 1; r->src = (uint64_t)(uintptr_t)h; }
}
NVN(nvnCommandBufferBindImage, void, NVNcommandBuffer *c, NVNshaderStage st, int i, NVNimageHandle h) { st = st_host(st); uint64_t v = (uint64_t)h; bind_handles(c, OP_BIND_IMG, st, i, 1, &v); }
NVN(nvnCommandBufferBindVertexBuffer, void, NVNcommandBuffer *c, int i, NVNbufferAddress a, size_t n) { NVNbufferRange r = {a, n}; h_nvnCommandBufferBindVertexBuffers(c, i, 1, &r); }

NVN(nvnCommandBufferDrawElements, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, 0, 0, 1); }
NVN(nvnCommandBufferDrawElementsBaseVertex, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, int n, NVNbufferAddress ib, int bv) { h_nvnCommandBufferDrawElementsInstanced(c, pr, it, n, ib, bv, 0, 1); }
NVN(nvnCommandBufferDrawArraysInstanced, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, int first, int n, int bi, int inst) {
    if (!inst) return;   // no instance: nothing is drawn
    rec_draw(c, (p_draw){pr, first, n, inst, bi});
}
// Indirect-count multi-draws: Metal has no GPU-read draw count, so small ones become max indirect draws carrying the count's address and index, a large one
// one range record; the replay zeros argument records past the count on the GPU (mdx_run)
enum { MDI_INLINE = 1024 };  // larger calls keep the range in a single record
NVN(nvnCommandBufferMultiDrawElementsIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_INLINE) { *REC(c, OP_MULTI_DRAW, p_multi_draw) = (p_multi_draw){ind, cnt, ib, stride, max, pr, it}; return; }
    for (int i = 0; i < max; i++) rec_draw_idx(c, (p_draw_idx){pr, it, 0, 0, 0, 1, ib, ind + (uint64_t)i * (uint64_t)stride, cnt, i});
}
NVN(nvnCommandBufferMultiDrawArraysIndirectCount, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNbufferAddress ind, NVNbufferAddress cnt, int max, ptrdiff_t stride) {
    if (max > MDI_INLINE) { *REC(c, OP_MULTI_DRAW, p_multi_draw) = (p_multi_draw){ind, cnt, 0, stride, max, pr, -1}; return; }
    for (int i = 0; i < max; i++) rec_draw(c, (p_draw){pr, 0, 0, 1, 0, ind + (uint64_t)i * (uint64_t)stride, cnt, i});
}
// outside a sparse virtual pool always; inside one only where pages are mapped
int nvn_host_readable(uint64_t a, uint64_t n) {
    int ok = 1; pthread_mutex_lock(&g_vp_mu);
    for (int k = 0; k < g_nvp; k++) { struct vpool *v = g_vp[k]; if (a < (uint64_t)(uintptr_t)v->base || a >= (uint64_t)(uintptr_t)v->base + v->len) continue;
        uint64_t o = a - (uint64_t)(uintptr_t)v->base; ok = 0;
        for (int r = 0; r < v->nrun; r++) if (o >= v->run[r].lo && o + n <= v->run[r].hi) { ok = 1; break; }
        break; }
    pthread_mutex_unlock(&g_vp_mu); return ok;
}
// Sparse virtual addresses alias physical pools, so guest ranges alone cannot
// prove copy independence.
int nvn_sparse_alias(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return 0;
    int r = 0; pthread_mutex_lock(&g_vp_mu);
    for (int k = 0; k < g_nvp && !r; k++) { const struct vpool *v = g_vp[k];
        uint64_t base = (uint64_t)(uintptr_t)v->base;
        r = lo < base + v->len && base < hi;
    }
    pthread_mutex_unlock(&g_vp_mu); return r;
}
// Textures of a sparse pool, read through its page table: copies (unmapped = 0), change checks and the protection of the physical pages behind them
static struct vpool *vpool_at(const void *pool, uint64_t a) {   // caller holds g_vp_mu
    struct vpool *v = pool ? ((const pool_t *)pool)->vp : NULL;
    return v && a >= (uint64_t)(uintptr_t)v->base && a < (uint64_t)(uintptr_t)v->base + v->size ? v : NULL;
}
int nvn_sparse_pool(const void *pool) { return pool && ((const pool_t *)pool)->vp; }   // no lock: set at Initialize, cleared at the first (aliasing) mapping or Finalize
uint64_t nvn_sparse_gen(void) { return atomic_load(&g_vp_gen); }
// requests after *seen, which moves on: their count, or -1 past max or when the record no longer holds them (all sparse textures changed). Under g_vp_mu
int nvn_sparse_notes(uint64_t *seen, uint64_t (*out)[2], int max) {
    pthread_mutex_lock(&g_vp_mu);
    uint64_t now = atomic_load(&g_vp_gen); int n = 0;
    if (now - *seen > (uint64_t)max || now - *seen > VNOTE_N) n = -1;
    else for (uint64_t g = *seen + 1; g <= now; g++) {
        if (g_vnote[g % VNOTE_N].gen != g) { n = -1; break; }
        out[n][0] = g_vnote[g % VNOTE_N].lo; out[n][1] = g_vnote[g % VNOTE_N].hi; n++;
    }
    *seen = now; pthread_mutex_unlock(&g_vp_mu); return n;
}
uint64_t nvn_sparse_copy(const void *pool, void *dst, uint64_t a, uint64_t n) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a); uint64_t miss = 0;
    if (!v) { pthread_mutex_unlock(&g_vp_mu); memset(dst, 0, n); return n; }
    for (uint64_t p = 0, o = a - (uint64_t)(uintptr_t)v->base; p < n;) {
        uint64_t at = o + p, k = 0x1000 - (at & 0xFFF); if (k > n - p) k = n - p;
        const vpte *e = at < v->size ? vpt_get(v, at) : NULL;
        if (e && e->phys) memcpy((uint8_t *)dst + p, e->phys + (at & 0xFFF), k); else { memset((uint8_t *)dst + p, 0, k); miss += k; }
        p += k;
    }
    pthread_mutex_unlock(&g_vp_mu); return miss;
}
// fn on each run (nonzero stops and is returned), or 1 at the first page mapped after gen (gen ~0: no look)
static int vpt_walk_locked(const struct vpool *v, uint64_t a, uint64_t n, uint64_t gen, int (*fn)(const uint8_t *, uint64_t, uint64_t), uint64_t arg) {
    int r = 0;
    const uint8_t *rs = NULL, *re = NULL; uint64_t o = a - (uint64_t)(uintptr_t)v->base;
    for (uint64_t at = o & ~0xFFFull; at < o + n && at < v->size && !r; at += 0x1000) {
        const vpte *e = vpt_get(v, at);
        if (!e) { at |= (2u << 20) - 1; at -= 0xFFF; continue; }   // a leaf never mapped: skip its 2 MB
        if (gen != ~0ull && e->gen > gen) { r = 1; break; }
        if (!e->phys) continue;
        if (rs && e->phys == re) { re += 0x1000; continue; }
        if (rs) r = fn(rs, (uint64_t)(re - rs), arg);
        rs = e->phys; re = rs + 0x1000;
    }
    if (!r && rs) r = fn(rs, (uint64_t)(re - rs), arg);
    return r;
}
static int vpt_walk(const void *pool, uint64_t a, uint64_t n, uint64_t gen, int (*fn)(const uint8_t *, uint64_t, uint64_t), uint64_t arg) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a);
    int r = v ? vpt_walk_locked(v, a, n, gen, fn, arg) : 1;
    pthread_mutex_unlock(&g_vp_mu); return r;
}
static int run_buffer_range(const uint8_t *p, uint64_t n, uint64_t arg) {
    nvn_buffer_range_fn note = *(nvn_buffer_range_fn *)(uintptr_t)arg;
    note((uint64_t)(uintptr_t)p, n); return 0;
}
// attribution taken with the copy: an unmap drops the page-table entry at once while its Metal buffer keeps the old pages
void nvn_buffer_write_ranges(uint64_t a, uint64_t n, nvn_buffer_range_fn note) {
    if (!n) return;
    int sparse = 0;
    pthread_mutex_lock(&g_vp_mu);
    for (int i = 0; i < g_nvp; i++) {
        const struct vpool *v = g_vp[i]; uint64_t base = (uint64_t)(uintptr_t)v->base;
        if (a >= base && a - base < v->size) { sparse = 1; vpt_walk_locked(v, a, n, ~0ull, run_buffer_range, (uint64_t)(uintptr_t)&note); break; }
    }
    pthread_mutex_unlock(&g_vp_mu);
    if (!sparse) note(a, n);
}
static _Thread_local const void *t_rw_span;   // the dirty tracker's range of the last run (nvn_dirty_since_cached)
static int run_written(const uint8_t *p, uint64_t n, uint64_t stamp) { return nvn_dirty_since_cached(&t_rw_span, p, n, stamp); }
static int run_protect(const uint8_t *p, uint64_t n, uint64_t unused) { (void)unused; nvn_dirty_protect(p, n); return 0; }
int nvn_sparse_since(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp) { return vpt_walk(pool, a, n, gen, run_written, stamp); }
int nvn_sparse_mapped(const void *pool, uint64_t a, uint64_t n) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a); int ok = v != NULL;
    for (uint64_t o = v ? (a - (uint64_t)(uintptr_t)v->base) & ~0xFFFull : 0, end = v ? a - (uint64_t)(uintptr_t)v->base + n : 0; ok && o < end; o += 0x1000) { const vpte *e = o < v->size ? vpt_get(v, o) : NULL; ok = e && e->phys; }
    pthread_mutex_unlock(&g_vp_mu); return ok;
}
void nvn_sparse_protect(const void *pool, uint64_t a, uint64_t n) { vpt_walk(pool, a, n, ~0ull, run_protect, 0); }
// out[i]: the memory behind the page at a + i * 4 KB, NULL where unmapped (a, n multiples of 4 KB); 0 when a is in no sparse pool
int nvn_sparse_pages(const void *pool, uint64_t a, uint64_t n, const uint8_t **out) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a);
    if (!v) { pthread_mutex_unlock(&g_vp_mu); return 0; }
    for (uint64_t i = 0, o = a - (uint64_t)(uintptr_t)v->base; i < n >> 12; i++, o += 0x1000) { const vpte *e = o < v->size ? vpt_get(v, o) : NULL; out[i] = e ? e->phys : NULL; }
    pthread_mutex_unlock(&g_vp_mu); return 1;
}
static int run_protect_tile(const uint8_t *p, uint64_t n, uint64_t unused) { (void)unused; nvn_dirty_protect_sparse(p, n); return 0; }
void nvn_sparse_protect_tile(const void *pool, uint64_t a, uint64_t n) { vpt_walk(pool, a, n, ~0ull, run_protect_tile, 0); }
// its first page when all 16 are mapped and consecutive, NULL otherwise
const uint8_t *nvn_sparse_tile_phys(const void *pool, uint64_t a) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a); const uint8_t *p = NULL;
    uint64_t o = v ? a - (uint64_t)(uintptr_t)v->base : 0;
    if (v && !(o & 0xFFFF) && o + 65536 <= v->size) {
        const vpte *e = vpt_get(v, o); p = e ? e->phys : NULL;
        for (uint64_t k = 1; p && k < 16; k++) { const vpte *f = vpt_get(v, o + (k << 12)); if (!f || f->phys != p + (k << 12)) p = NULL; }
    }
    pthread_mutex_unlock(&g_vp_mu); return p;
}
// A sparse pool's mapping log, for placement sparse textures instead of a page walk per look (which held g_vp_mu against the game's mapping calls). No lock
uint64_t nvn_sparse_log_seq(const void *pool) {
    struct vpool *v = pool ? ((const pool_t *)pool)->vp : NULL; if (!v) return 0;
    if (!v->log) { pthread_mutex_lock(&g_vp_mu); if (!v->log && !(v->log = calloc(VLOG_N, sizeof *v->log))) die("sparse pool change log allocation failed"); pthread_mutex_unlock(&g_vp_mu); }
    return atomic_load_explicit(&v->nlog, memory_order_acquire);
}
// -1 when the log no longer holds them or there are more than max: scan the texture
int nvn_sparse_log_read(const void *pool, uint64_t *seq, uint64_t (*out)[2], int max) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = pool ? ((const pool_t *)pool)->vp : NULL; int n = 0;
    uint64_t end = v ? atomic_load_explicit(&v->nlog, memory_order_relaxed) : 0;
    if (!v || !v->log || end - *seq > VLOG_N || end - *seq > (uint64_t)max) n = -1;
    else for (uint64_t k = *seq; k < end; k++, n++) { const struct vlog_ent *e = &v->log[k % VLOG_N]; out[n][0] = (uint64_t)(uintptr_t)v->base + e->off; out[n][1] = out[n][0] + e->len; }
    *seq = end;
    pthread_mutex_unlock(&g_vp_mu); return n;
}
// bit 0: every page has memory behind it; bit 1: a mapping newer than gen or a page written after stamp. -1 outside a sparse pool
int nvn_sparse_tile_state(const void *pool, uint64_t a, uint64_t n, uint64_t gen, uint64_t stamp) {
    pthread_mutex_lock(&g_vp_mu); struct vpool *v = vpool_at(pool, a);
    if (!v) { pthread_mutex_unlock(&g_vp_mu); return -1; }
    int all = 1, chg = 0; const uint8_t *rs = NULL, *re = NULL;
    for (uint64_t o = (a - (uint64_t)(uintptr_t)v->base) & ~0xFFFull, end = a - (uint64_t)(uintptr_t)v->base + n; o < end; o += 0x1000) {
        const vpte *e = o < v->size ? vpt_get(v, o) : NULL;
        if (e && e->gen > gen) chg = 1;
        if (!e || !e->phys) { all = 0; continue; }
        if (rs && e->phys == re) { re += 0x1000; continue; }
        if (rs && !chg) chg = nvn_dirty_since(rs, (uint64_t)(re - rs), stamp);
        rs = e->phys; re = rs + 0x1000;
    }
    if (rs && !chg) chg = nvn_dirty_since(rs, (uint64_t)(re - rs), stamp);
    pthread_mutex_unlock(&g_vp_mu); return all | chg << 1;
}
NVN(nvnCommandBufferDrawElementsIndirect, void, NVNcommandBuffer *c, NVNdrawPrimitive pr, NVNindexType it, NVNbufferAddress ib, NVNbufferAddress ind) {
    rec_draw_idx(c, (p_draw_idx){pr, it, 0, 0, 0, 1, ib, ind});
}

NVN(nvnCommandBufferSetViewport, void, NVNcommandBuffer *c, int x, int y, int w, int h) { float v[4] = {(float)x, (float)y, (float)w, (float)h}; h_nvnCommandBufferSetViewports(c, 0, 1, v); }
NVN(nvnCommandBufferSetScissors, void, NVNcommandBuffer *c, int first, int n, const int *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetScissor(c, r[0], r[1], r[2], r[3]);
    if (first + n > 1) ONCE_LOG("SetScissors first=%d n=%d: the host uses only index 0", first, n);
}
NVN(nvnCommandBufferSetDepthRanges, void, NVNcommandBuffer *c, int first, int n, const float *r) {
    if (first == 0 && n >= 1) h_nvnCommandBufferSetDepthRange(c, r[0], r[1]);
    if (first + n > 1) ONCE_LOG("SetDepthRanges first=%d n=%d: the host uses only index 0", first, n);
}

// a 32-bit write when this point is reached, applied on completion
NVN(nvnCommandBufferReportValue, void, NVNcommandBuffer *c, uint32_t v, NVNbufferAddress a) {
    uint8_t *p = cmd_alloc(CB(c), 24); put_hdr(p, OP_SIGNAL_EVENT, 24);
    volatile uint32_t *val = (volatile uint32_t *)(uintptr_t)a; uint32_t mm = NVN_EVENT_SIGNAL_MODE_WRITE;
    memcpy(p + 8, &val, 8); memcpy(p + 16, &mm, 4); memcpy(p + 20, &v, 4);
}
// filled in submission order at replay
NVN(nvnCommandBufferClearBuffer, void, NVNcommandBuffer *c, NVNbufferAddress a, size_t n, uint32_t v) { *REC(c, OP_CLEAR_BUF, p_clear_buf) = (p_clear_buf){a, n, v, 0}; }

// evaluated at replay, as the GPU does
NVN(nvnCommandBufferSetRenderEnable, void, NVNcommandBuffer *c, uint32_t en) { *REC(c, OP_RENDER_COND, p_render_cond) = (p_render_cond){(en & 0xff) ? 2 : 3, 0, 0}; }
NVN(nvnCommandBufferSetRenderEnableConditional, void, NVNcommandBuffer *c, NVNconditionalRenderMode m, NVNbufferAddress a) {
    *REC(c, OP_RENDER_COND, p_render_cond) = (p_render_cond){(int)m & 1, 0, a};
}
NVN(nvnCommandBufferSetDepthClamp, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_DEPTH_CLAMP, p_depth_clamp)->on = (en & 0xff) != 0; }
// NEGATIVE_Y: clip y negated (how a GL-heritage renderer flips its picture)
NVN(nvnCommandBufferSetViewportSwizzles, void, NVNcommandBuffer *c, int first, int n, const NVNviewportSwizzle *s) {
    if (slots_fit(0, first, n, 16) != n) { LOG("[RED] SetViewportSwizzles first=%d n=%d", first, n); return; }
    p_vp_swizzle *r = REC(c, OP_VP_SWIZZLE, p_vp_swizzle); r->first = first; r->n = n;
    for (int i = 0; i < n; i++) for (int k = 0; k < 4; k++) r->sw[i][k] = (uint8_t)((s[4 * i + k] ^ 2 * k) & 7);
}
NVN(nvnCommandBufferSetAlphaRef, void, NVNcommandBuffer *c, float r) { REC(c, OP_ALPHA_REF, p_alpha_ref)->ref = r; }
NVN(nvnCommandBufferSetLineWidth, void, NVNcommandBuffer *c, float w) { REC(c, OP_LINE_WIDTH, p_line_width)->w = w; }
NVN(nvnCommandBufferSetPointSize, void, NVNcommandBuffer *c, float s) { (void)c; (void)s; ONCE_LOG("SetPointSize: the point size comes from the shader"); }
NVN(nvnCommandBufferSetPrimitiveRestart, void, NVNcommandBuffer *c, uint32_t en, int idx) {
    uint32_t on = (en & 0xff) != 0, ix = (uint32_t)idx;
    if (g_rec_compact && (ix == UINT32_MAX || ix < (1u << 22))) { crec(c, OPC_PRIM_RESTART, on | (ix == UINT32_MAX) << 1 | (ix == UINT32_MAX ? 0 : ix << 2), 0); return; }
    *REC(c, OP_PRIM_RESTART, p_prim_restart) = (p_prim_restart){on, ix};
}
// draws run their vertex stage only and rasterize nothing
NVN(nvnCommandBufferSetRasterizerDiscard, void, NVNcommandBuffer *c, uint32_t en) { REC(c, OP_RAST_DISCARD, p_rast_discard)->on = (en & 0xff) != 0; }
NVN(nvnCommandBufferSetSampleMask, void, NVNcommandBuffer *c, int m) { (void)c; if ((m & 0xffff) != 0xffff) ONCE_LOG("SetSampleMask %#x: ignored by the host for now", m); }
NVN(nvnCommandBufferSetConservativeRasterEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; if (en & 0xff) ONCE_LOG("SetConservativeRasterEnable on: ignored by the host for now"); }
NVN(nvnCommandBufferSetTiledCacheAction, void, NVNcommandBuffer *c, NVNtiledCacheAction a) { (void)c; (void)a; }        // Tegra tiled cache hint, no Metal equivalent
// discarded contents are undefined afterwards: keeping them is correct
NVN(nvnCommandBufferDiscardColor, void, NVNcommandBuffer *c, int i) { (void)c; (void)i; }
NVN(nvnCommandBufferDiscardDepthStencil, void, NVNcommandBuffer *c) { (void)c; }
NVN(nvnCommandBufferSetTiledCacheTileSize, void, NVNcommandBuffer *c, int w, int h) { (void)c; (void)w; (void)h; }
NVN_NOPROTO(nvnCommandBufferSetColorReductionEnable, void, NVNcommandBuffer *c, uint32_t en) { (void)c; (void)en; }   // colour compression hints
NVN_NOPROTO(nvnCommandBufferSetColorReductionThresholds, void, NVNcommandBuffer *c, int cls, const int *t) { (void)c; (void)cls; (void)t; }
// recorded as a texture copy with DM_COPY_RESOLVE; replayed as Metal's multisample resolve
static void rec_downsample(NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv, uint64_t ra) {
    p_copy_t2t *o = REC_SNAP(c, OP_COPY_T2T, p_copy_t2t, 2);
    o->src = tex_snap(o, SNAP_OFF(p_copy_t2t), 0, s, ra); o->dst = tex_snap(o, SNAP_OFF(p_copy_t2t), 1, d, ra);
    copy_view(&o->sv, &o->has_sv, sv); copy_view(&o->dv, &o->has_dv, dv);
    if (o->dst) mtl_tex_gpu_written(o->dst);
    if (o->src) o->sr = (NVNcopyRegion){0, 0, 0, (int)o->src->d.width, (int)o->src->d.height, 1};
    if (o->dst) o->dr = (NVNcopyRegion){0, 0, 0, (int)o->dst->d.width, (int)o->dst->d.height, 1};
    o->flags = DM_COPY_RESOLVE;
}
NVN(nvnCommandBufferDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { rec_downsample(c, s, NULL, d, NULL, (uint64_t)__builtin_return_address(0)); }
NVN(nvnCommandBufferDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    rec_downsample(c, s, sv, d, dv, (uint64_t)__builtin_return_address(0));
}
// tiled resolves differ only in Tegra cache scheduling, not the destination pixels
NVN(nvnCommandBufferTiledDownsample, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtexture *d) { rec_downsample(c, s, NULL, d, NULL, (uint64_t)__builtin_return_address(0)); }
NVN(nvnCommandBufferTiledDownsampleTextureView, void, NVNcommandBuffer *c, const NVNtexture *s, const NVNtextureView *sv, const NVNtexture *d, const NVNtextureView *dv) {
    rec_downsample(c, s, sv, d, dv, (uint64_t)__builtin_return_address(0));
}

// WriteTexels: only the region's bytes are written, by block-linear address (games may call it once per texel), after write protection is lifted and the range
// marked dirty. −1 out of range or unsupported: the whole-level path. slice: a 3D level's depth slice, 0 otherwise
static int write_region_bl(const tex_t *t, unsigned level, unsigned layer, unsigned slice, const NVNcopyRegion *r, const uint8_t *src, size_t row, int bw, int bh, int bpb) {
    uint32_t row_bytes, rows, slices, gbpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(&t->d, level, layer, &row_bytes, &rows, &slices, &base, &th, &td, &gbpb) || (int)gbpb != bpb || r->xoffset < 0 || r->yoffset < 0 || slice >= slices) return -1;
    uint32_t x0 = (uint32_t)(r->xoffset / bw) * (uint32_t)bpb, y0 = (uint32_t)(r->yoffset / bh);
    size_t wb = (size_t)((r->width + bw - 1) / bw) * (size_t)bpb, hb = (size_t)((r->height + bh - 1) / bh);
    if (x0 + wb > row_bytes || y0 + hb > rows) return -1;
    uint32_t lb = nvn_tex_level_gobs(&t->d, level) * 64; if (lb < row_bytes) lb = row_bytes;   // the row as laid out (padded in tiled levels)
    for (size_t y = 0; y < hb; y++)
        for (size_t x = 0; x < wb;) {   // 16-byte runs are contiguous within a GOB
            uint32_t bx = x0 + (uint32_t)x; size_t n = 16 - bx % 16; if (n > wb - x) n = wb - x;
            memcpy(t->mem + base + nvn_bl_offset(bx, y0 + (uint32_t)y, slice, lb, rows, th, td, nvn_gob_order(&t->d)), src + y * row + x, n); x += n;
        }
    return 0;
}
// a 3D region steps through its one layer's depth slices (a view's layers do not apply, as in the Metal copy), others through layers from the view's first
static void region_image(const tex_t *t, unsigned layer0, const NVNcopyRegion *r, int z, unsigned *layer, unsigned *slice) {
    if (t->d.target == NVN_TEXTURE_TARGET_3D) { *layer = 0; *slice = (unsigned)(r->zoffset + z); }
    else { *layer = layer0 + (unsigned)(r->zoffset + z); *slice = 0; }
}
static void write_texels(const tex_t *t, const view_t *v, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    if (!t->mem) { LOG("[RED] WriteTexels: texture has no storage"); return; }
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    if (!bpb) { ONCE_LOG("[RED] WriteTexels: unknown format %#x", t->d.format); return; }
    unsigned level = v && v->set_levels ? (unsigned)v->base_level : 0, layer0 = v && v->set_layers ? (unsigned)v->min_layer : 0;
    int is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = r->depth ? r->depth : 1;
    // a linear texture is one 2D level of rows of its stride: the block-linear writes refuse it, and the whole-level path would drop the write
    if (t->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) {
        size_t srow = tex_linear_row(t), wb = (size_t)r->width * (size_t)bpb, hb = (size_t)r->height, x0 = (size_t)r->xoffset * (size_t)bpb, y0 = (size_t)r->yoffset, row0 = row ? row : wb;
        if (t->d.target != NVN_TEXTURE_TARGET_2D || level || layer0 || r->zoffset || nz != 1 || bw != 1 || bh != 1 || r->xoffset < 0 || r->yoffset < 0 || r->width < 0 || r->height < 0
            || x0 + wb > srow || (y0 + hb) * srow > t->L.size) {
            static int once; if (!once++) LOG("[RED] WriteTexels into a linear texture outside its one level of rows (%ux%u format %#x target %d, stride %zu, region %d,%d,%d %dx%dx%d): not written",
                                              t->d.width, t->d.height, t->d.format, t->d.target, srow, r->xoffset, r->yoffset, r->zoffset, r->width, r->height, nz);
            return;
        }
        dm_guest_write_prepare(t->mem, t->L.size);
        for (size_t y = 0; y < hb; y++) memcpy(t->mem + (y0 + y) * srow + x0, src + y * row0, wb);
        mtl_tex_linear_cpu_written(t);
        return;
    }
    {
        size_t wb0 = (size_t)((r->width + bw - 1) / bw) * bpb, row0 = row ? row : wb0, img0 = img ? img : row0 * (size_t)((r->height + bh - 1) / bh);
        dm_guest_write_prepare(t->mem, t->L.size);
        int ok = 1;
        for (int z = 0; z < nz && ok; z++) { unsigned layer, slice; region_image(t, layer0, r, z, &layer, &slice); ok = !write_region_bl(t, level, layer, slice, r, src + (size_t)z * img0, row0, bw, bh, bpb); }
        if (ok) { mtl_tex_cpu_written(t); return; }
    }
    uint32_t lw = t->d.width >> level ? t->d.width >> level : 1, lh = t->d.height >> level ? t->d.height >> level : 1, lz = is3d ? (t->d.depth >> level ? t->d.depth >> level : 1) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (size_t)(r->height + bh - 1) / bh;
    if (!row) row = wb;
    if (!img) img = row * hb;
    uint8_t *lin = malloc(lp * lr * lz); if (!lin) die("WriteTexels allocation failed");
    dm_guest_write_prepare(t->mem, t->L.size);
    // the whole 2D level overwritten: no need to decode the old content
    int whole = !is3d && !r->xoffset && !r->yoffset && (uint32_t)r->width == lw && (uint32_t)r->height == lh;
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + (unsigned)(r->zoffset + z);
        if (((is3d && z == 0) || !is3d) && !whole) if (nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { ONCE_LOG("[RED] WriteTexels deswizzle failed (format %#x target %d)", t->d.format, t->d.target); free(lin); return; }
        uint8_t *dst = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0) + (size_t)(r->yoffset / bh) * lp + (size_t)(r->xoffset / bw) * bpb;
        for (size_t y = 0; y < hb; y++) memcpy(dst + y * lp, src + (size_t)z * img + y * row, wb);
        if ((!is3d || z == nz - 1) && nvn_swizzle(&t->d, level, layer, lin, lp, t->mem)) {
            static int once; if (!once++) LOG("[RED] WriteTexels swizzle failed (format %#x target %d flags %#x): not written", t->d.format, t->d.target, t->d.flags);
            free(lin); return;
        }
    }
    free(lin);
    mtl_tex_cpu_written(t);
}
NVN(nvnTextureWriteTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src) { write_texels(CTX(t), (const view_t *)v, r, src, 0, 0); }
// strides in bytes, 0 = tightly packed
NVN(nvnTextureWriteTexelsStrided, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r, const void *src, ptrdiff_t row, ptrdiff_t img) {
    write_texels(CTX(t), (const view_t *)v, r, src, (size_t)row, (size_t)img);
}
// offsets into the texture's storage; −1 out of range
int nvn_tex_level_range(const tex_t *t, unsigned level, unsigned layer, uint64_t *lo, uint64_t *hi) {
    uint64_t a = nvn_tex_level_offset(&t->d, level), b = level + 1 < t->d.levels ? nvn_tex_level_offset(&t->d, level + 1) : t->L.layer_size;
    if (b <= a || b > t->L.layer_size || (uint64_t)layer * t->L.layer_size + b > t->L.size) return -1;
    *lo = (uint64_t)layer * t->L.layer_size + a; *hi = (uint64_t)layer * t->L.layer_size + b;
    return 0;
}
NVN(nvnTextureFlushTexels, void, const NVNtexture *t, const NVNtextureView *v, const NVNcopyRegion *r) { (void)t; (void)v; (void)r; }   // unified memory + dirty page tracking: nothing to flush

// Reset: the NVN layer left as a new process finds it, content-keyed caches kept. Queued jobs dropped, the GPU drained, then each object the game left
// finalized by its own Finalize (pools last), skipping those whose magic is gone.
void mtl_reset_guest(void);
void nvn_queue_reset_drop(void);
static size_t live_take(int k, const void ***out) {
    pthread_mutex_lock(&g_live_mu);
    size_t n = g_live[k] ? (size_t)CFSetGetCount(g_live[k]) : 0;
    *out = n ? calloc(n, sizeof **out) : NULL;
    if (n) { CFSetGetValues(g_live[k], (const void **)*out); CFSetRemoveAllValues(g_live[k]); }
    pthread_mutex_unlock(&g_live_mu);
    return n;
}
void dm_gfx_reset(void) {
    uint64_t t0 = now_ns();
    mtl_hold_drain();   // a submission held at a wait for the game's signal ends first
    nvn_queue_reset_drop();
    dm_gfx_suspend(1); dm_gfx_suspend(0);
    static const uint32_t magic[LV_N] = {MAGIC('P', 'O', 'O', 'L'), MAGIC('T', 'E', 'X', 0), MAGIC('P', 'R', 'O', 'G'), MAGIC('S', 'Y', 'N', 'C'), MAGIC('Q', 'U', 'E', 'U'), MAGIC('W', 'I', 'N', 'D')};
    static const int order[] = {LV_WIN, LV_QUEUE, LV_SYNC, LV_PROG, LV_TEX, LV_POOL};
    void mtl_defer_batch(int on); mtl_defer_batch(1);
    size_t done[LV_N] = {0}, gone[LV_N] = {0}; uint64_t ms[LV_N] = {0};
    for (unsigned oi = 0; oi < sizeof order / sizeof *order; oi++) {
        int k = order[oi]; const void **v; size_t n = live_take(k, &v); uint64_t tk = now_ns();
        for (int pass = 0; pass < (k == LV_POOL ? 2 : 1); pass++)   // virtual pools (their storage is a physical pool's) before physical ones
            for (size_t i = 0; i < n; i++) {
                void *o = (void *)v[i]; if (!o) continue;
                if (*(const uint32_t *)o != magic[k]) { if (!pass) gone[k]++; v[i] = NULL; continue; }
                if (k == LV_POOL && !pass && !(((const pool_t *)o)->flags & 0x200)) continue;
                switch (k) {
                case LV_WIN: h_nvnWindowFinalize(o); break;
                case LV_QUEUE: h_nvnQueueFinalize(o); break;
                case LV_SYNC: h_nvnSyncFinalize(o); break;
                case LV_PROG: h_nvnProgramFinalize(o); break;
                case LV_TEX: h_nvnTextureFinalize(o); break;
                case LV_POOL: h_nvnMemoryPoolFinalize(o); break;
                }
                done[k]++; v[i] = NULL;
            }
        free(v); ms[k] = now_ns() - tk;
    }
    mtl_defer_batch(0);
    pthread_mutex_lock(&g_dpool_mu); memset(g_tpb, 0, sizeof g_tpb); memset(g_spb, 0, sizeof g_spb); g_ntpb = g_nspb = g_tnext = g_snext = g_tend = g_send = 0; pthread_mutex_unlock(&g_dpool_mu);
    mtl_set_tex_pool_n(0); mtl_set_samp_pool_n(0);
    pthread_mutex_lock(&g_pk_mu); if (g_pk) { CFIndex n = CFDictionaryGetCount(g_pk); const void **vals = calloc((size_t)n + 1, sizeof *vals); CFDictionaryGetKeysAndValues(g_pk, NULL, vals); for (CFIndex i = 0; i < n; i++) free((void *)vals[i]); free(vals); CFDictionaryRemoveAllValues(g_pk); } pthread_mutex_unlock(&g_pk_mu);
    pthread_mutex_lock(&g_hostseg_mu); for (int i = 0; i < g_nhostseg; i++) free(g_hostseg[i].mem); g_nhostseg = 0; g_hostseg_bytes = 0; atomic_store(&g_hostseg_held, 0); pthread_mutex_unlock(&g_hostseg_mu);
    pthread_mutex_lock(&g_ft_mu); size_t nft = g_nft; g_nft = 0; pthread_mutex_unlock(&g_ft_mu);
    pthread_mutex_lock(&g_fl_mu); g_nfl = 0; pthread_mutex_unlock(&g_fl_mu);
    pthread_mutex_lock(&g_sub_mu); mtl_reset_guest(); pthread_mutex_unlock(&g_sub_mu);
    atomic_fetch_add(&g_boot, 1);
    uint64_t mtl_present_tickets(void); g_present_base = mtl_present_tickets();
    dm_fstep_settle();
    LOG("reset: graphics: finalized %zu windows, %zu queues, %zu syncs, %zu programs (%.1f ms), %zu textures (%.1f ms), %zu memory pools (%.1f ms) (%zu objects already gone)%s in %.1f ms",
        done[LV_WIN], done[LV_QUEUE], done[LV_SYNC], done[LV_PROG], ms[LV_PROG] / 1e6, done[LV_TEX], ms[LV_TEX] / 1e6, done[LV_POOL], ms[LV_POOL] / 1e6,
        gone[LV_WIN] + gone[LV_QUEUE] + gone[LV_SYNC] + gone[LV_PROG] + gone[LV_TEX] + gone[LV_POOL], nft ? " (registered textures left to flush tracking)" : "", (now_ns() - t0) / 1e6);
}
