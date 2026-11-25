// Metal replay layer of the NVN device (nvn_mtl.h): shared state, command buffers, residency sets, recycled heaps and deferred release, then its parts in order
// (one translation unit; tests include this file for its static functions)
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <MetalFX/MetalFX.h>
#undef I   // MetalFX's <complex.h> I would shadow local tables named I
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <sys/file.h>
#include <IOKit/IOKitLib.h>
#include <os/os_sync_wait_on_address.h>
#include "nvn_mtl.h"
#include "nvn_queue.h"
#include "nvn_obj.h"
#include "../tex/nvn_fmt_mtl.h"
#include "../tex/nvn_deswizzle.h"
#include "../tex/nvn_deswizzle_gpu.h"
#include "host_msl.h"
#include "nvn_dirty.h"
#include "nvn_depth_clear.h"
#include "nvn_refl.h"
#include "nvn_border.h"
#include "nvn_primitive.h"
#include "nvn_link_drain.h"
#include "nvn_link_latch.h"
#include "nvn_present_history.h"
#include "nvn_timestamp.h"
#include "msl_image.h"
#include "dmsh_chain.h"   // DMSH_CHAIN: the shader library chain version this tree expects
#include "game_profile.h"
uint64_t nvn_ts_profile_hz = DM_GAME_GPU_TS_HZ;
// 0-127 combined handles, 128-255 separate textures (NVN_SEPTEX_SLOT0)
#define NTEX 256
#include "dmsh_abi.h"
_Static_assert(DMSH_C2_TEX + DMSH_C2_ROW * DMSH_C2_TEX_SLOTS == DMSH_C2_SEPTEX && DMSH_C2_SEPTEX + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS == DMSH_C2_SAMP && DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS <= DMSH_DRV_SIZE && NVN_SEPTEX_SLOT0 + DMSH_C2_SEP_SLOTS == NTEX, "NVN driver handle tables");
_Static_assert(DMSH_C0_CS_UBO + DMSH_C0_ROW * DMSH_C0_CS_UBO_SLOTS == DMSH_C0_SSBO_CS && DMSH_C0_SSBO_PS + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS <= DMSH_C0_POINT_SIZE && DMSH_C0_POINT_SIZE + 4 <= DMSH_C0_SAMPLE_TABLE &&
               DMSH_C0_SAMPLE_TABLE + DMSH_C0_SAMPLE_BYTES <= DMSH_C0_SAMPLE_INFO && DMSH_C0_SAMPLE_INFO + 16 <= DMSH_C0_BASE_INSTANCE && DMSH_C0_BASE_INSTANCE + 4 <= DMSH_C0_SSBO_TS && DMSH_C0_SSBO_TS + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS == DMSH_C0_SSBO_MS && DMSH_C0_SSBO_MS + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS <= DMSH_DRV_SIZE, "NVN driver c0 tables");
// NVN stages: 0 vertex, 1 pixel, 3 task, 4 mesh, 5 compute; no geometry shader (2)
static int c0_ssbo_base(int stage) { return stage == 0 ? DMSH_C0_SSBO_VS : stage == 1 ? DMSH_C0_SSBO_PS : stage == 5 ? DMSH_C0_SSBO_CS : stage == 3 ? DMSH_C0_SSBO_TS : stage == 4 ? DMSH_C0_SSBO_MS : -1; }
// as c2 holds it: a combined slot past 31 shares its word with an image slot (a size query of an image a compute shader writes)
static inline uint64_t tex_slot_h(const uint64_t *tex, const uint64_t *img, unsigned reg) {
    return reg >= 32 && reg - 32 < DMSH_C2_IMG_SLOTS && img[reg - 32] ? img[reg - 32] : reg < NTEX ? tex[reg] : 0;
}
static uint64_t c2_handle(int stage, unsigned b);
#include "nvn_hmap.h"
#include "nvn_owner.h"
#include "../../host/dm.h"
#import <CommonCrypto/CommonDigest.h>
#include <stdarg.h>
#include <mach-o/dyld.h>
#include <mach-o/fat.h>
#include <libkern/OSByteOrder.h>
#include <libgen.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <stdatomic.h>
#include <errno.h>
#include <execinfo.h>
#include <os/lock.h>
#include <objc/runtime.h>
#include <objc/message.h>
// Parameters are not retained again in callees (callers and tables hold them; defer_obj_release keeps them alive)
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)

#define MLOG(...) dm_log("[dm] [nvn-mtl] " __VA_ARGS__)   // one line under one lock, written on the host stack (recording runs on the game thread's stack)
#define MDIE(...) do { MLOG("FATAL " __VA_ARGS__); abort(); } while (0)

static void warm_start(void);
static void shmap_load(void);
static int prepare_on(void);
static int g_warm_started;
static void warm_claim(void);
#ifndef guest_clock_freeze_for
void guest_clock_freeze_for(int reason, int on);
#endif

static id<MTLDevice> g_dev;
static void ser_compile_begin(void), ser_compile_end(void), plain_compile_begin(void), plain_compile_end(void);
static void arch_died(NSError *e);
// every compile reports service death before leaving the serialization gate
static id compile_done(id result, NSError *error, NSError **out) {
    if (!result) {
        arch_died(error);
        if (out) *out = error;
    }
    plain_compile_end();
    return result;
}
static id<MTLComputePipelineState> dev_cps(id<MTLFunction> f, NSError **out) {
    plain_compile_begin();
    NSError *e = nil;
    id ps = [g_dev newComputePipelineStateWithFunction:f error:&e];
    return compile_done(ps, e, out);
}
static id<MTLRenderPipelineState> dev_rps(MTLRenderPipelineDescriptor *d, NSError **out) {
    plain_compile_begin();
    NSError *e = nil;
    id ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e];
    return compile_done(ps, e, out);
}
static id<MTLRenderPipelineState> dev_mps(MTLMeshRenderPipelineDescriptor *d, NSError **out) {
    plain_compile_begin();
    NSError *e = nil;
    id ps = [g_dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&e];
    return compile_done(ps, e, out);
}
static id<MTLFunction> dev_fn(id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv, NSError **out) {
    plain_compile_begin();
    NSError *e = nil;
    id f = [lib newFunctionWithName:name constantValues:cv error:&e];
    return compile_done(f, e, out);
}
static id<MTLLibrary> dev_lib(NSString *source, NSError **out) {
    plain_compile_begin();
    NSError *e = nil;
    id lib = [g_dev newLibraryWithSource:source options:nil error:&e];
    return compile_done(lib, e, out);
}
// Host shaders (dm/gfx/msl) are compiled with the host and loaded by name; only diagnostic overrides compile from source
static id<MTLLibrary> host_lib_from(id<MTL4Compiler> compiler, const char *name, NSError **out) {
    plain_compile_begin();
    NSError *e = nil;
    id lib = host_msl_lib(g_dev, compiler, name, &e);
    return compile_done(lib, e, out);
}
static id<MTLLibrary> host_lib(const char *name, NSError **e) { return host_lib_from(nil, name, e); }
static int g_nvn_deswz_gpu;
static id<MTLCommandQueue> g_q;
static id<MTL4CommandQueue> g_q4;
static dispatch_queue_t g_feedback_q;
static _Atomic uint32_t g_present_qos;
#define ALLOC_MAX 8
enum { ALLOC_SHARE = 64 };   // submissions per allocator
// most submissions in flight; sizes POST_RING, VIS_RING
enum { SUB_FLIGHT_MAX = ALLOC_MAX * ALLOC_SHARE };
static id<MTL4CommandAllocator> g_alloc4[ALLOC_MAX];   // a finished allocator is reused, else a new one (wait only at the cap)
static int g_alloc_n, g_alloc_cur;
static id<MTL4CommandAllocator> g_up_alloc4[3];
static id<MTL4CommandAllocator> g_present_alloc4[3];
static id<MTL4CommandBuffer> g_present_cb[3];
static id<MTL4ArgumentTable> g_vt, g_ft, g_ct, g_up_ct, g_ot;   // g_ot: a task / mesh draw's object stage
static int g_vbuf_cache = 1;
static uint64_t g_vt_addr[31];
static uint32_t g_vt_valid;
static uint64_t g_ft_addr[31];   // as last set: a waiting run of mesh draws is drawn before one changes
static uint32_t g_ft_valid;
static uint64_t g_vbuf_resolve, g_vbuf_reuse, g_vbuf_set, g_vbuf_skip, g_vbuf_copies, g_vbuf_copy_bytes;
static id<MTL4ArgumentTable> g_present_args;
static id<MTLBuffer> g_present_uv[3];
static uint64_t g_present_alloc_fence[3];
// Under g_pq_mu: per present allocator, the newest ticket its feedback reported done; reset only once it reaches the ticket that last used it. Not g_pq_done, which runs
// ahead (a reset under an unrun present reads back black: t_present_alloc)
static uint64_t g_present_alloc_done[3];
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_link_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_link_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_link_tick_ns, g_link_tick_n;
static uint64_t g_link_dropped;
static uint64_t g_link_empty;   // Under g_pq_mu: ticks that found no new frame (not ticks minus presents)
// Standing-frame drain (nvn_link_drain.h): the next acquire waits g_link_drain_req more ticks once. g_link_blocked_ns: the acquire's last wait (0 = did not wait);
// g_link_drain_ns: drain time since mtl_link_drain_take_ns
static nvn_link_drain g_link_drain;   // display-link callback thread only
static _Atomic int g_link_drain_req;
static _Atomic uint64_t g_link_blocked_ns, g_link_drain_ns, g_link_drained;
// Late latch (nvn_link_latch.h): g_link_waiter, under g_link_mu, the tick an acquire blocks for (0 = none); g_link_waiter_end: it ends a drain into the latch;
// g_link_latch_exits: left for late GPU work or a skipped tick; g_link_latch_late_gpu: latched presents whose GPU work ended after targetTimestamp (feedback handler)
static uint64_t g_link_waiter, g_link_latched, g_link_latch_out, g_link_latch_exits;
static _Atomic uint64_t g_link_latch_late_gpu;
static nvn_link_latch_health g_link_latch_health;   // display-link callback thread only
static double g_present_gpu_s;   // under g_pq_mu: the present command buffer's GPU time, smoothed (seconds)
static unsigned g_link_latch_misses;
static int g_link_waiter_end;
// display-link callback thread only: the previous frame's hold (0 = none) and the ticks it kept empty
static double g_link_hold_until;
static uint64_t g_link_held;
static int g_pq_inflight;   // Under g_pq_mu
int g_dm_present_link;   // DM_PRESENT_LINK: 0 drawable acquired directly; 1 display link at 60 Hz; 2 at 120 Hz, a frame every other tick
static int g_present_latency;   // 0 = not set: link_latency picks it
static int g_present_link_env;   // set: its mode holds everywhere (no 120 Hz move in variable-refresh fullscreen)
static uint64_t g_pq_done;
static uint64_t g_pq_read;
static id<MTLResidencySet> g_res, g_snap_res;
// Residency changes queue in g_res_q (under g_res_mu) until a commit applies them under g_res_commit_mu alone: a commit can take tens of ms and would stall res_add /
// res_remove. Queue commits that must stay apart from the set's commit hold g_res_commit_mu too
typedef struct { CFTypeRef o; int add; } res_op;
static res_op *g_res_q;   // under g_res_mu
static size_t g_res_qn, g_res_qcap;
static pthread_mutex_t g_res_commit_mu = PTHREAD_MUTEX_INITIALIZER;   // taken before g_res_mu, never inside it
// Residency by command buffer, no set on a queue: a workaround, cause open (docs/reference/frame-rate.md#the-driver-wires-memory-tens-of-thousands-of-times-a-second-with-residency-sets-on-the-queues).
// Metal 4 command buffers name the sets in cb_end (a begin drops them), classic ones in cl_cb. Each naming holds g_res_commit_mu, apart from a set's commit
static _Atomic uint64_t g_res_names, g_res_name_ns, g_res_name_max;
static void res_name(id cb, id<MTLResidencySet> also) {   // a Metal 4 or classic command buffer
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_res_commit_mu);
    if (g_res) [cb useResidencySet:g_res];
    if (g_snap_res) [cb useResidencySet:g_snap_res];
    if (also) [cb useResidencySet:also];
    pthread_mutex_unlock(&g_res_commit_mu);
    uint64_t d = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, m = atomic_load(&g_res_name_max);   // reset apart: a naming can land in one line's time and the next line's count
    atomic_fetch_add(&g_res_name_ns, d);
    atomic_fetch_add(&g_res_names, 1);
    while (d > m && !atomic_compare_exchange_weak(&g_res_name_max, &m, d)) {}
}
static void cb_end(id cb, id<MTLResidencySet> also);   // every Metal 4 command buffer ends here
static id<MTLSharedEvent> g_ev;
static _Atomic uint64_t g_fence;                // incremented by the submit thread only
static _Atomic uint64_t g_sub_open;             // 0 with none open: from mtl_sub_begin until its commit took that fence and encoded everything
static void (*g_pool_gone_between)(void);       // test hook (t_pool_retire)
static int g_cut_copy;                          // a buffer → texture copy since the last commit: the next pass boundary commits
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_done;                 // GPU complete and all CPU-visible effects published
static _Atomic(uint32_t *) g_mesh_scr;         // post_complete publishes g_done there
static _Atomic uint64_t g_arena_made, g_arena_live, g_arena_live_bytes, g_arena_freed, g_arena_freed_bytes;
static uint64_t g_cm_mdraws;
static uint64_t g_cm_enc, g_cm_enc0, g_cm_mdraws0, g_cm_hist[7], g_cm_big_enc, g_cm_big_draws, g_cm_big_z;
static int g_allocprof = -1;
static uint64_t g_ap_bytes[64], g_ap_fn[8];
static uint64_t g_md_masked_calls, g_md_masked, g_md_counted_calls, g_md_counted;
static uint64_t g_cm_in[4], g_cm_out, g_cm_inn[4];
static int g_cm_kind = -1;
static void cm_enc_mark(int kind) {   // 0 / 1: an encoder begins; -1: it ends
    if (g_allocprof != 2 || !g_alloc4[g_alloc_cur]) return;
    static __unsafe_unretained id last; static uint64_t lz; id a = g_alloc4[g_alloc_cur]; uint64_t z = ((id<MTL4CommandAllocator>)a).allocatedSize;
    if (a == last && z >= lz) { if (g_cm_kind >= 0) g_cm_in[g_cm_kind] += z - lz; else g_cm_out += z - lz; }
    last = a; lz = z; g_cm_kind = kind; if (kind >= 0) g_cm_inn[kind]++;
}
// DM_NVN_ALLOCPROF=2: an allocator keeps its high-water size across resets, so a fresh one per submission measures it
static uint64_t g_cm_op[64], g_cm_opn[64], g_cm_sub_n, g_cm_sub_sum, g_cm_sub_max, g_cm_last;
static int allocprof_level(void) {
    if (g_allocprof < 0) {
        const char *e = getenv("DM_NVN_ALLOCPROF");
        g_allocprof = e ? (atoi(e) > 1 ? 2 : 1) : 0;
    }
    return g_allocprof;
}
// diagnostic: no clocks are read without DM_NVN_CPUPROF
typedef struct { uint64_t wall, cpu; } cp_stamp;
static _Thread_local uint64_t g_cp_wall[6], g_cp_cpu[6], g_cp_calls[6], g_cp_last;
static int cp_on(void) {
    static _Thread_local int on = -1;
    if (on < 0) {
        const char *e = getenv("DM_NVN_CPUPROF");
        on = e && atoi(e);
    }
    return on;
}
static cp_stamp cp_begin(void) {
    if (!cp_on()) return (cp_stamp){0};
    return (cp_stamp){clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW), clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID)};
}
static void cp_end(int phase, cp_stamp start) {
    if (!start.wall) return;
    g_cp_cpu[phase] += clock_gettime_nsec_np(CLOCK_THREAD_CPUTIME_ID) - start.cpu;
    g_cp_wall[phase] += clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW) - start.wall;
    g_cp_calls[phase]++;
}
static void cp_report(void) {
    if (!cp_on()) return;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC_RAW);
    if (!g_cp_last) { g_cp_last = now; return; }
    if (now - g_cp_last < 5000000000ull) return;
    static const char *names[6] = {"pass_end", "pass_begin", "pipeline", "arguments", "texture", "draw"};
    char line[1000] = {0}; size_t at = 0;
    for (int i = 0; i < 6; i++) if (g_cp_calls[i])
        at += snprintf(line + at, sizeof line - at, " %s %llu calls %.2f/%.2f ms CPU/wall;", names[i], (unsigned long long)g_cp_calls[i], g_cp_cpu[i] / 1e6, g_cp_wall[i] / 1e6);
    MLOG("[cpuprof] inclusive phases over %.2f s (nested phases overlap):%s", (now - g_cp_last) / 1e9, line);
    memset(g_cp_wall, 0, sizeof g_cp_wall);
    memset(g_cp_cpu, 0, sizeof g_cp_cpu);
    memset(g_cp_calls, 0, sizeof g_cp_calls);
    g_cp_last = now;
}
#define AP_B uint64_t _ap0 = g_allocprof > 0 ? g_dev.currentAllocatedSize : 0; cp_stamp _cp0 = cp_begin()
#define AP_E(i) do { if (g_allocprof > 0) { uint64_t _n = g_dev.currentAllocatedSize; if (_n > _ap0) g_ap_fn[i] += _n - _ap0; } cp_end(i, _cp0); } while (0)
static _Atomic int g_last_commit_error;
static _Atomic int g_up_pending;

// macOS revokes GPU access after GPU errors a process caused: every command buffer then fails with AccessRevoked without running and no fence is signalled. The first stops the
// game (post_complete raises the fences' event from the CPU, as for any error). A hang or fault alone is not enough: the GPU recovers from those
static _Atomic int g_gpu_lost;
void front_gpu_lost(void);
__attribute__((weak)) void front_gpu_lost(void) { MDIE("GPU access revoked: macOS runs none of this process's GPU work any more"); }
// Metal 4 reports a commit's error as MTL4CommandQueueErrorTimeout with the command buffer's error underneath; only this domain counts (a victim's IOGPU error has code 5 too)
static int gpu_err_has(NSError *e, NSInteger code, int depth) {
    if (!e || depth > 3) return 0;
    if (e.code == code && [e.domain isEqualToString:MTL4CommandQueueErrorDomain]) return 1;
    for (NSError *u in e.underlyingErrors) if (gpu_err_has(u, code, depth + 1)) return 1;
    return 0;
}
static void gpu_lost_check(NSError *e, const char *where) {
    if (!e || atomic_load(&g_gpu_lost) || !gpu_err_has(e, MTL4CommandQueueErrorAccessRevoked, 0) || atomic_exchange(&g_gpu_lost, 1)) return;
    MLOG("[RED] GPU access revoked: the %s command buffer was ignored (fences back %llu of %llu); macOS runs none of this process's GPU work any more, so the game stops", where,
         (unsigned long long)atomic_load(&g_done), (unsigned long long)atomic_load(&g_fence));
    front_gpu_lost();
}
// DM_TEST_GPU_LOST=n: from submission n, feedback imitates a revoked run and the fence signal is left out (the work still runs)
static int gpu_lost_test(void) {
    static int at = -2, n;
    if (at == -2) {
        const char *e = getenv("DM_TEST_GPU_LOST");
        at = e ? atoi(e) : -1;
    }
    return at > 0 && ++n >= at;
}
static NSError *gpu_lost_test_error(void) {
    NSError *revoked = [NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorAccessRevoked userInfo:@{NSLocalizedDescriptionKey: @"DM_TEST_GPU_LOST: Ignored (for causing prior/excessive GPU errors)",
                                                                                                                             NSUnderlyingErrorKey: [NSError errorWithDomain:@"IOGPUCommandQueueErrorDomain" code:4 userInfo:nil]}];
    return [NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorTimeout userInfo:@{NSUnderlyingErrorKey: revoked, NSMultipleUnderlyingErrorsKey: @[revoked]}];
}

static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;   // res_add is called with g_mu held (arena_new), so a lock of its own
// Wait ledger (DM_NVN_WAITS=1): CPU waits for the GPU or the encode thread, per frame; SyncWait split by how far its fence is behind the newest
enum { WS_SYNC, WS_SYNC_NEWEST, WS_SYNC_POLL, WS_ACQ, WS_FINISH, WS_ALLOC, WS_VIS, WS_MDI, WS_READTEX, WS_T2T, WS_DIAG, WS_SNAP, WS_UPZERO, WS_UPALLOC, WS_DRAIN, WS_UBC, WS_OLDQ, WS_N };
static const char *const ws_name[WS_N] = {"SyncWait (game, older fence)", "SyncWait (game, newest fence)", "SyncWait polls", "SyncWait on an acquire sync", "QueueFinish", "allocator", "occlusion ring",
    "indirect-count multi-draw", "texture readback", "texture copy t2t", "diagnostics", "snapshot ring", "upload batches", "upload slot", "encode thread drain", "uniform copy worker", "old-queue command buffer"};
static _Atomic uint64_t g_ws_n[WS_N], g_ws_blk[WS_N], g_ws_ns[WS_N];
static int ws_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_WAITS");
        v = e && atoi(e) > 0;
    }
    return v;
}
static inline uint64_t ws_t0(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static inline void ws_put(int site, uint64_t t0, int blocked) {
    atomic_fetch_add_explicit(&g_ws_n[site], 1, memory_order_relaxed);
    if (blocked) {
        atomic_fetch_add_explicit(&g_ws_blk[site], 1, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_ws_ns[site], ws_t0() - t0, memory_order_relaxed);
    }
}
// each fence's GPU end and feedback time: a blocked wait's wake-up delay
static struct { _Atomic uint64_t fence, wfence; double gpu_end, fb, wake; _Atomic int done; } g_fend[64];
static _Atomic uint64_t g_ws_wake_n, g_ws_wake_ns, g_ws_fb_ns;
static void ws_wake_pair(unsigned k) {
    if (atomic_load(&g_fend[k].fence) != atomic_load(&g_fend[k].wfence) || atomic_exchange(&g_fend[k].done, 1)) return;
    atomic_fetch_add(&g_ws_wake_n, 1);
    atomic_fetch_add(&g_ws_wake_ns, (uint64_t)(fmax(0, g_fend[k].wake - g_fend[k].gpu_end) * 1e9));
    atomic_fetch_add(&g_ws_fb_ns, (uint64_t)(fmax(0, g_fend[k].fb - g_fend[k].gpu_end) * 1e9));
}
static void ws_wake(uint64_t fence) {
    unsigned k = fence % 64;
    g_fend[k].wake = CACurrentMediaTime();
    atomic_store(&g_fend[k].wfence, fence);
    ws_wake_pair(k);
    static int once;
    if (!once++) {
        char nm[64] = "";
        pthread_getname_np(pthread_self(), nm, sizeof nm);
        MLOG("[waits] first blocking SyncWait on the newest fence: thread '%s'", nm);
    }
}
static void ws_fb(uint64_t fence, double gpu_end) {
    unsigned k = fence % 64;
    if (atomic_load(&g_fend[k].fence) != fence) {
        atomic_store(&g_fend[k].done, 0);
        g_fend[k].gpu_end = gpu_end;
        g_fend[k].fb = CACurrentMediaTime();
        atomic_store(&g_fend[k].fence, fence);
    }
    ws_wake_pair(k);
}
static _Atomic uint64_t g_ws_post[5];
static void ws_report(uint64_t frame, uint64_t frames) {
    char line[2048]; size_t o = 0;
    { uint64_t e = atomic_exchange(&g_ws_post[1], 0), sp = atomic_exchange(&g_ws_post[2], 0), ts = atomic_exchange(&g_ws_post[3], 0), oc = atomic_exchange(&g_ws_post[4], 0);
      if (e + sp + ts + oc) o += snprintf(line + o, sizeof line - o, " CPU-side writes at GPU end: events %.2f, samples-passed reports %.2f, timestamp reports %.2f, other counter reports %.2f;", (double)e / frames, (double)sp / frames, (double)ts / frames, (double)oc / frames); }
    uint64_t wn = atomic_exchange(&g_ws_wake_n, 0), wns = atomic_exchange(&g_ws_wake_ns, 0), fns = atomic_exchange(&g_ws_fb_ns, 0);
    if (wn) o += snprintf(line + o, sizeof line - o, " newest-fence SyncWait wakes %.3f ms after the GPU end (feedback handler at %.3f ms);", wns / 1e6 / wn, fns / 1e6 / wn);
    for (int i = 0; i < WS_N; i++) { uint64_t n = atomic_exchange(&g_ws_n[i], 0), b = atomic_exchange(&g_ws_blk[i], 0), ns = atomic_exchange(&g_ws_ns[i], 0);
        if (n && o < sizeof line - 160) o += snprintf(line + o, sizeof line - o, " %s %.2f/frame (blocked %.2f, %.3f ms);", ws_name[i], (double)n / frames, (double)b / frames, ns / 1e6 / frames); }
    MLOG("[waits] up to frame %llu, per frame:%s", (unsigned long long)frame, o ? line : " none");
}
static _Atomic uint64_t g_sub_cmt;   // with no encode thread, the last committed (mtl_sub_commit)
static _Atomic int g_seg_inflight;
static int cut_busy_limit(void);     // 0 unbounded, 1 GPU-idle (default), N bounded depth
// kept as blocks: several merged submissions' feedback can ride on one commit
@interface DmCommitFB : NSObject { @public NSMutableArray *blocks; }
- (void)addFeedbackHandler:(void (^)(id<MTL4CommitFeedback>))b;
@end
@implementation DmCommitFB
- (instancetype)init { if ((self = [super init])) blocks = [NSMutableArray array]; return self; }
- (void)addFeedbackHandler:(void (^)(id<MTL4CommitFeedback>))b { [blocks addObject:[b copy]]; }
@end
// one merged submission's share: the error and its part of the GPU time
@interface DmPartFB : NSObject { @public double t0, t1; NSError *err; }
@end
@implementation DmPartFB
- (CFTimeInterval)GPUStartTime { return t0; }
- (CFTimeInterval)GPUEndTime { return t1; }
- (NSError *)error { return err; }
@end
static MTL4CommitOptions *commit_opts(DmCommitFB *fb) {
    if (!fb) return nil;
    MTL4CommitOptions *o = [MTL4CommitOptions new];
    NSArray *bl = fb->blocks;
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { for (void (^b)(id<MTL4CommitFeedback>) in bl) b(f); }];
    return o;
}
static id<MTL4CounterHeap> g_ts_heap;
#include "nvn_rec.h"
// Names the residency sets first; a recorded one names them on the encode thread at RO_CB_END. Nothing else calls endCommandBuffer (test_residency_naming.py)
static void cb_end(id cb, id<MTLResidencySet> also) {
    if (![cb isKindOfClass:[DmRecCB class]]) res_name(cb, also);
    [(id<MTL4CommandBuffer>)cb endCommandBuffer];
}
// nothing else calls commandBuffer on g_q (the same test)
static id<MTLCommandBuffer> cl_cb(void) { id<MTLCommandBuffer> cb = [g_q commandBuffer]; res_name(cb, nil); return cb; }
static NSHashTable *g_res_objects;
static _Atomic unsigned g_image_heap_n, g_big_n, g_pair_n;   // g_big_n: large pools in overlapping chunks (BIG_MIN); g_pair_n: storage pairs
static inline MTL4VisibilityOptions resource_vis(void) { return MTL4VisibilityOptionDevice | (g_image_heap_n || g_big_n || g_pair_n ? MTL4VisibilityOptionResourceAlias : 0); }
// residency churn by call site (file:line) for the power line
enum { RS_SITES = 16 };
static struct { const char *file; int line; uint64_t n, bytes; } g_rs_site[RS_SITES];
static uint64_t g_rs_rm, g_rs_commits, g_rs_commit_ns;
#define res_add(r) res_add_at((r), __FILE_NAME__, __LINE__)
// Guest threads' implicit autorelease pools never drain, and weak NSHashTable lookups autorelease the object: so a pool each
static void res_q_put(id<MTLAllocation> r, int add) {   // caller holds g_res_mu
    if (g_res_qn == g_res_qcap) {
        g_res_qcap = g_res_qcap ? 2 * g_res_qcap : 256;
        g_res_q = dm_realloc(g_res_q, g_res_qcap * sizeof *g_res_q, "the residency changes");
    }
    g_res_q[g_res_qn++] = (res_op){CFBridgingRetain(r), add};
}
static void res_add_at(id<MTLAllocation> allocation, const char *file, int line) { @autoreleasepool {
    if (!allocation) return;
    // a placed texture's views also need the parent texture: both retire through the usual paths
    if ([(id)allocation conformsToProtocol:@protocol(MTLResource)] && ((id<MTLResource>)allocation).heap)
        res_add_at(((id<MTLResource>)allocation).heap, file, line);
    id<MTLAllocation> r = allocation;
    pthread_mutex_lock(&g_res_mu);
    if (!g_res_objects) g_res_objects = [NSHashTable weakObjectsHashTable];
    if ([g_res_objects containsObject:r]) {   // already resident: marking the set dirty would cost an empty commit
        pthread_mutex_unlock(&g_res_mu);
        return;
    }
    int k = 0;
    while (k < RS_SITES - 1 && g_rs_site[k].line && (g_rs_site[k].line != line || strcmp(g_rs_site[k].file, file))) k++;
    g_rs_site[k].file = file;
    g_rs_site[k].line = line;
    g_rs_site[k].n++;
    g_rs_site[k].bytes += r.allocatedSize;
    res_q_put(r, 1);
    [g_res_objects addObject:r];
    pthread_mutex_unlock(&g_res_mu);
} }
static void res_remove(id<MTLAllocation> r) { @autoreleasepool {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    if (![g_res_objects containsObject:r]) {
        pthread_mutex_unlock(&g_res_mu);
        return;
    }
    res_q_put(r, 0);
    [g_res_objects removeObject:r];
    g_rs_rm++;
    pthread_mutex_unlock(&g_res_mu);
} }
// Recycled texture memory: Metal lays textures out unlike NVN, so GPU-only targets cannot share the pool's memory, but a freed texture's placement heap waits here, resident,
// for the next texture it fits: made an eighth larger, it serves one up to a third smaller (a dynamic resolution's next size). Only textures the GPU renders or writes; each
// is filled as a new allocation is and the old use completed, so no alias flush is needed. Heaps idle RH_IDLE_NS, or past RH_CAP, go at a submission's start (rh_trim)
enum { RH_CAP = 256u << 20 };
static const uint64_t RH_IDLE_NS = 3000000000ull;
typedef struct { void *heap; uint64_t size, t; } rh_ent;   // heap retained; t: when it began to wait
static os_unfair_lock g_rh_lk = OS_UNFAIR_LOCK_INIT;
static rh_ent *g_rh;
static unsigned g_rh_n, g_rh_cap;
static uint64_t g_rh_bytes;
static _Atomic uint64_t g_rh_made, g_rh_reused, g_rh_trimmed;
static int sw_mode(const char *name, int dflt);
static inline int sw_on(int m);
static int rh_mode(void) {
    static int m = -1;
    if (m < 0) m = sw_mode("DM_NVN_RT_RECYCLE", 1);
    return m;
}
// runs where the texture's last reference goes (defer_drain: submit thread or completion handler)
static void rh_wait(id<MTLHeap> heap) {
    if (!heap) return;
    if (!sw_on(rh_mode())) {
        res_remove(heap);
        return;
    }
    rh_ent e = {(void *)CFBridgingRetain(heap), heap.size, clock_gettime_nsec_np(CLOCK_MONOTONIC)};
    os_unfair_lock_lock(&g_rh_lk);
    if (g_rh_n == g_rh_cap) {
        g_rh_cap = g_rh_cap ? 2 * g_rh_cap : 64;
        g_rh = dm_realloc(g_rh, g_rh_cap * sizeof *g_rh, "the recycled texture heaps");
    }
    g_rh[g_rh_n++] = e;
    g_rh_bytes += e.size;
    os_unfair_lock_unlock(&g_rh_lk);
}
@interface NvRecycledHeap : NSObject { @public id<MTLHeap> heap; } @end
@implementation NvRecycledHeap
- (void)dealloc { rh_wait(heap); }
@end
@interface NvImageHeap : NSObject { @public id<MTLHeap> heap; } @end
@implementation NvImageHeap
- (void)dealloc {
    rh_wait(heap);
    atomic_fetch_sub(&g_image_heap_n, 1);
}
@end
static char g_image_heap_owner_key, g_rh_owner_key;
// cap a new heap's extra space so one large target does not keep tens of MB for reuse
enum { RH_SLACK = 4u << 20 };
static id<MTLHeap> rh_heap(uint64_t size, MTLStorageMode sm, MTLCPUCacheMode cm, MTLHazardTrackingMode hm, int *reused) {
    *reused = 0;
    if (!size) return nil;
    int on = sw_on(rh_mode());
    uint64_t top = size + size / 2;
    void *got = NULL;
    if (on) {
        os_unfair_lock_lock(&g_rh_lk);
        unsigned best = g_rh_n;
        for (unsigned i = 0; i < g_rh_n; i++) {
            if (g_rh[i].size < size || g_rh[i].size > top || (best < g_rh_n && g_rh[i].size >= g_rh[best].size)) continue;
            id<MTLHeap> h = (__bridge id<MTLHeap>)g_rh[i].heap;
            if (h.storageMode != sm || h.cpuCacheMode != cm) continue;   // a placed texture takes its heap's hazard tracking (placement heaps report untracked)
            best = i;
        }
        if (best < g_rh_n) {
            got = g_rh[best].heap;
            g_rh_bytes -= g_rh[best].size;
            g_rh[best] = g_rh[--g_rh_n];
        }
        os_unfair_lock_unlock(&g_rh_lk);
    }
    if (got) {
        *reused = 1;
        atomic_fetch_add(&g_rh_reused, 1);
        return CFBridgingRelease(got);
    }
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new];
    hd.type = MTLHeapTypePlacement;
    hd.size = on ? size + MIN(size / 8, (uint64_t)RH_SLACK) : size;
    hd.storageMode = sm;
    hd.cpuCacheMode = cm;
    hd.hazardTrackingMode = hm;
    id<MTLHeap> h = [g_dev newHeapWithDescriptor:hd];
    if (h && on) atomic_fetch_add(&g_rh_made, 1);
    return h;
}
// nil when off or unplaceable; a heap the texture does not go into waits again
static id<MTLTexture> rh_place(MTLTextureDescriptor *d) {
    if (!sw_on(rh_mode()) || d.storageMode != MTLStorageModePrivate || d.textureType == MTLTextureTypeTextureBuffer) return nil;
    MTLSizeAndAlign need = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    int reused = 0;
    id<MTLHeap> heap = rh_heap(need.size, d.storageMode, d.cpuCacheMode, d.hazardTrackingMode, &reused);
    id<MTLTexture> mt = heap ? [heap newTextureWithDescriptor:d offset:0] : nil;
    if (!mt) {
        if (heap && reused) rh_wait(heap);
        return nil;
    }
    NvRecycledHeap *owner = [NvRecycledHeap new];
    owner->heap = heap;
    objc_setAssociatedObject(mt, &g_rh_owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return mt;
}
// NVN asks no flag of a render target, so the test is the Metal usage; block-compressed assets keep exact allocations
static int rh_fits(const tex_t *t, MTLTextureUsage usage) {
    return t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) && (usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite))
        && !(t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
}
static id<MTLTexture> rh_texture(const tex_t *t, MTLTextureDescriptor *d) { return rh_fits(t, d.usage) ? rh_place(d) : nil; }
// Shared render targets drawn over (nvn_owner.m own_alias_dead_locked): per texture, its last use and last GPU write in g_tu_now's clock. A pool walk stamps nothing
// (it resolves every ID). Under g_tu_lk, a leaf lock
typedef struct { uint32_t use, wr, pass; } tu_rec;
static os_unfair_lock g_tu_lk = OS_UNFAIR_LOCK_INIT;
static nvn_hmap g_tu = {.klen = sizeof(uintptr_t)};
static uint32_t g_tu_now = 1, g_tu_pass_no;   // deciseconds since the first submission + 1 (0 = none)
static int g_pool_walk;   // under g_rmu: lookups are not uses
static nvn_hmap g_tu_gone = {.klen = 8};   // under g_tu_lk
static uint64_t g_tu_remade;
static tu_rec *tu_find(id o) {   // caller holds g_tu_lk; a view's parent's record
    for (id x = o; x; x = [x respondsToSelector:@selector(parentTexture)] ? ((id<MTLTexture>)x).parentTexture : nil) {
        uintptr_t k = (uintptr_t)(__bridge void *)x;
        tu_rec *r = nvn_hm_get(&g_tu, &k);
        if (r) return r;
    }
    return NULL;
}
static void tu_stamp(id o, int wrote) {
    if (!o || !g_tu.n) return;   // n read unlocked: a record made meanwhile is stamped at its next use
    uint32_t now = g_tu_now;
    os_unfair_lock_lock(&g_tu_lk);
    tu_rec *r = tu_find(o);
    if (r) {
        r->use = now;
        if (wrote) r->wr = now;
    }
    os_unfair_lock_unlock(&g_tu_lk);
}
static void tu_wrote(id o) { tu_stamp(o, 1); }
static void tu_register(id<MTLTexture> t, uint64_t serial) {
    uintptr_t k = (uintptr_t)(__bridge void *)t;
    os_unfair_lock_lock(&g_tu_lk);
    tu_rec *r = nvn_hm_get(&g_tu, &k);
    if (!r) {
        r = malloc(sizeof *r);
        if (!r) MDIE("the render target use records");
        nvn_hm_put(&g_tu, &k, r);
    }
    *r = (tu_rec){g_tu_now, 0, g_tu_pass_no};
    if (g_tu_gone.n && nvn_hm_del(&g_tu_gone, &serial)) g_tu_remade++;
    os_unfair_lock_unlock(&g_tu_lk);
}
// all of them when memory is short (mem_trim). → bytes freed
static uint64_t rh_trim_n(int all, unsigned *n_out) {
    if (!g_rh_n) {
        if (n_out) *n_out = 0;
        return 0;
    }
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), bytes = 0;
    unsigned n = 0;
    for (;;) {
        void *drop[64];
        unsigned nd = 0;
        os_unfair_lock_lock(&g_rh_lk);
        while (g_rh_n && nd < 64) {
            unsigned old = 0;
            for (unsigned i = 1; i < g_rh_n; i++) if (g_rh[i].t < g_rh[old].t) old = i;
            if (!all && now - g_rh[old].t < RH_IDLE_NS && g_rh_bytes <= RH_CAP) break;
            drop[nd++] = g_rh[old].heap;
            bytes += g_rh[old].size;
            g_rh_bytes -= g_rh[old].size;
            g_rh[old] = g_rh[--g_rh_n];
        }
        int more = all && g_rh_n;
        os_unfair_lock_unlock(&g_rh_lk);
        for (unsigned i = 0; i < nd; i++) {
            id<MTLHeap> h = CFBridgingRelease(drop[i]);
            res_remove(h);
        }
        n += nd;
        if (!more || !nd) break;
    }
    if (n) atomic_fetch_add(&g_rh_trimmed, n);
    if (n_out) *n_out = n;
    return bytes;
}
static void rh_trim(void) { rh_trim_n(0, NULL); }
// system memory in MB (host_statistics64): free and wired memory the process's figures miss
typedef struct { uint64_t total, free, wired, compressed, file, purgeable; int ok; } sys_mem_t;
static sys_mem_t sys_mem(void) {
    sys_mem_t m = {0};
    vm_statistics64_data_t v;
    mach_msg_type_number_t n = HOST_VM_INFO64_COUNT;
    uint64_t total = 0;
    size_t tl = sizeof total;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &n) != KERN_SUCCESS) return m;
    sysctlbyname("hw.memsize", &total, &tl, NULL, 0);
    uint64_t pg = vm_kernel_page_size;
    m.total = total >> 20;
    m.free = ((uint64_t)v.free_count + v.speculative_count) * pg >> 20;
    m.wired = (uint64_t)v.wire_count * pg >> 20;
    m.compressed = (uint64_t)v.compressor_page_count * pg >> 20;
    m.file = (uint64_t)v.external_page_count * pg >> 20;
    m.purgeable = (uint64_t)v.purgeable_count * pg >> 20;
    m.ok = 1;
    return m;
}
// Memory pressure: each event asks for a trim at the next submission's start (mem_trim), no GPU wait there. The Mac trims at critical only, since it can swap
static _Atomic int g_mp_want;   // 1 warn, 2 critical; taken by mem_trim
static _Atomic uint64_t g_mp_n[3], g_mp_trims, g_mp_freed;
static _Atomic uint64_t g_sweep_n, g_sweep_bytes;
static int mp_act_level(void) {
    static int lv = -1;
    if (lv < 0) {
        const char *e = getenv("DM_NVN_MEM_PRESSURE");
        lv = e && *e ? atoi(e) : 2;
        if (lv < 0 || lv > 2) lv = 2;
    }
    return lv;
}
static void mem_pressure_said(char *b, size_t n) {
    snprintf(b, n, "memory pressure events warn %llu, critical %llu, back to normal %llu; trims %llu, %llu MB given back; idle uploaded textures freed without pressure %llu, %llu MB",
             (unsigned long long)atomic_load(&g_mp_n[1]), (unsigned long long)atomic_load(&g_mp_n[2]), (unsigned long long)atomic_load(&g_mp_n[0]),
             (unsigned long long)atomic_load(&g_mp_trims), (unsigned long long)(atomic_load(&g_mp_freed) >> 20),
             (unsigned long long)atomic_load(&g_sweep_n), (unsigned long long)(atomic_load(&g_sweep_bytes) >> 20));
}
static void mem_pressure_event(int lv) {   // 0 normal, 1 warn, 2 critical
    atomic_fetch_add(&g_mp_n[lv], 1);
    sys_mem_t m = sys_mem();
    int act = lv && mp_act_level() && lv >= mp_act_level();
    MLOG("[mem] memory pressure %s: system free %llu MB, wired %llu MB, compressed %llu MB, file-backed %llu MB; residency set %llu MB; %s", lv == 2 ? "critical" : lv ? "warn" : "back to normal",
         (unsigned long long)m.free, (unsigned long long)m.wired, (unsigned long long)m.compressed, (unsigned long long)m.file, (unsigned long long)(g_res ? g_res.allocatedSize >> 20 : 0),
         act ? "trimming at the next submission" : lv ? "no trim at this level here" : "");
    if (act) {
        int cur = atomic_load(&g_mp_want);
        while (lv > cur && !atomic_compare_exchange_weak(&g_mp_want, &cur, lv)) {}
    }
}
static void mem_pressure_start(void) {
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!src) {
        MLOG("[RED] memory pressure: no dispatch source; memory is not trimmed on pressure");
        return;
    }
    dispatch_source_set_event_handler(src, ^{ @autoreleasepool {
        unsigned long d = dispatch_source_get_data(src);
        mem_pressure_event(d & DISPATCH_MEMORYPRESSURE_CRITICAL ? 2 : d & DISPATCH_MEMORYPRESSURE_WARN ? 1 : 0);
    } });
    dispatch_resume(src);
}
// R32 storage under a 32-bit colour texture with image atomics, resident while the colour view lives
@interface NvAtomicRaw : NSObject { @public id<MTLTexture> raw; } @end
@implementation NvAtomicRaw
- (void)dealloc { res_remove(raw); }
@end
static char g_atomic_raw_key;
// DM_RES_CHECK=1: argument-block resources missing from the residency set (the first 40)
static int g_res_check_on;
// and the submission that last bound each, for defer_drain: an object retired before its binding's fence would fault or read a reused resource ID
static NSMapTable *g_res_last_use;
static uint64_t g_res_late;
static int g_res_missing;
static void res_use_note(id o, const char *what) {
    if (!g_res_last_use) g_res_last_use = [NSMapTable weakToStrongObjectsMapTable];
    [g_res_last_use setObject:@[@(g_fence + 1), @(what)] forKey:o];
}
static void res_check(id r, const char *what, int reg) {
    if (!g_res_check_on || !r) return;
    pthread_mutex_lock(&g_res_mu);
    res_use_note(r, what);
    pthread_mutex_unlock(&g_res_mu);
    if (g_res_missing >= 40) return;
    int is_tex = [r conformsToProtocol:@protocol(MTLTexture)], is_buf = [r conformsToProtocol:@protocol(MTLBuffer)];
    if (!is_tex && !is_buf) return;
    id<MTLResource> a = r;
    int hops = 0;
    if (is_tex) {
        id<MTLTexture> t = (id<MTLTexture>)a;
        while (t.parentTexture) {
            t = t.parentTexture;
            hops++;
        }
        a = t.buffer ? (id<MTLResource>)t.buffer : (id<MTLResource>)t;
    }
    if (a != r) {
        pthread_mutex_lock(&g_res_mu);
        res_use_note(a, what);
        pthread_mutex_unlock(&g_res_mu);
    }
    // a placement heap's textures are in the set one by one (the heap stays while a removed one waits to be drained); an automatic heap's are not
    pthread_mutex_lock(&g_res_mu);
    id<MTLHeap> h = a.heap;
    int ok = h ? [g_res_objects containsObject:h] && (h.type != MTLHeapTypePlacement || [g_res_objects containsObject:a]) : [g_res_objects containsObject:a];
    pthread_mutex_unlock(&g_res_mu);
    if (!ok) {
        g_res_missing++;
        id<MTLTexture> t = is_tex ? (id<MTLTexture>)r : nil;
        MLOG("[RED] missing residency #%d: %s slot %d %s %p (parent chain %d deep) allocated %lu B storage mode %lu%s", g_res_missing, what, reg, is_tex ? "texture" : "buffer", (__bridge void *)a, hops, (unsigned long)a.allocatedSize, (unsigned long)a.storageMode,
             t ? [NSString stringWithFormat:@" format %lu %lux%lu type %lu usage %#lx", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.textureType, (unsigned long)t.usage].UTF8String : ""); }
}
// Commits every change made before the call, one another thread's commit took included (it holds g_res_commit_mu until done and its removals are released); a removed
// allocation lives until it left the set
static void res_commit(void) { @autoreleasepool {
    pthread_mutex_lock(&g_res_commit_mu);
    pthread_mutex_lock(&g_res_mu);
    res_op *q = g_res_q;
    size_t n = g_res_qn;
    g_res_q = NULL;
    g_res_qn = g_res_qcap = 0;
    pthread_mutex_unlock(&g_res_mu);
    if (n) {
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (size_t i = 0; i < n; i++) {
            if (q[i].add) [g_res addAllocation:(__bridge id<MTLAllocation>)q[i].o];
            else [g_res removeAllocation:(__bridge id<MTLAllocation>)q[i].o];
        }
        [g_res commit];
        uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        pthread_mutex_lock(&g_res_mu);
        g_rs_commits++;
        g_rs_commit_ns += dt;
        pthread_mutex_unlock(&g_res_mu);
    }
    // released before the unlock, outside g_res_mu: a heap a removed texture held is back in the recycler before another thread's res_commit returns
    @autoreleasepool { for (size_t i = 0; i < n; i++) CFRelease(q[i].o); }
    free(q);
    pthread_mutex_unlock(&g_res_commit_mu);
} }
static int res_pending(void) {
    pthread_mutex_lock(&g_res_mu);
    int n = g_res_qn != 0;
    pthread_mutex_unlock(&g_res_mu);
    return n;
}
// The residency set's totals by kind and largest objects: at the first GPU out-of-memory error and for DM_CTL restop
static void res_top(const char *why) { @autoreleasepool {
    enum { N = 24 }; id<MTLAllocation> top[N] = {nil}; uint64_t total = 0, n = 0, kind[4] = {0}, count[4] = {0};
    pthread_mutex_lock(&g_res_mu);
    for (id<MTLAllocation> o in g_res_objects.allObjects) { uint64_t z = o.allocatedSize; total += z; n++;
        int k = [(id)o conformsToProtocol:@protocol(MTLTexture)] ? 0 : [(id)o conformsToProtocol:@protocol(MTLHeap)] ? 1 : ((id<MTLBuffer>)o).storageMode == MTLStorageModeShared ? 2 : 3;
        kind[k] += z; count[k]++;
        for (int q = 0; q < N; q++) if (!top[q] || z > top[q].allocatedSize) { for (int j = N - 1; j > q; j--) top[j] = top[j - 1]; top[q] = o; break; } }
    pthread_mutex_unlock(&g_res_mu);
    MLOG("%s: the residency set holds %llu objects, %llu MB (Metal's recommended working set %llu MB, GPU allocated %llu MB): textures %llu, %llu MB; heaps %llu, %llu MB; shared buffers %llu, %llu MB; private buffers %llu, %llu MB; the largest:", why,
         (unsigned long long)n, (unsigned long long)(total >> 20), (unsigned long long)(g_dev.recommendedMaxWorkingSetSize >> 20), (unsigned long long)(g_dev.currentAllocatedSize >> 20),
         (unsigned long long)count[0], (unsigned long long)(kind[0] >> 20), (unsigned long long)count[1], (unsigned long long)(kind[1] >> 20), (unsigned long long)count[2], (unsigned long long)(kind[2] >> 20), (unsigned long long)count[3], (unsigned long long)(kind[3] >> 20));
    for (int k = 0; k < N && top[k]; k++) { id o = top[k];
        if ([o conformsToProtocol:@protocol(MTLTexture)]) { id<MTLTexture> t = o; MLOG("  texture %lux%lux%lu format %lu levels %lu%s: %llu MB", (unsigned long)t.width, (unsigned long)t.height, (unsigned long)MAX(t.depth, t.arrayLength), (unsigned long)t.pixelFormat, (unsigned long)t.mipmapLevelCount, t.isSparse ? " (sparse)" : "", (unsigned long long)(t.allocatedSize >> 20)); }
        else if ([o conformsToProtocol:@protocol(MTLHeap)]) MLOG("  heap: %llu MB", (unsigned long long)(((id<MTLHeap>)o).size >> 20));
        else { id<MTLBuffer> b = o; int sh = b.storageMode == MTLStorageModeShared; MLOG("  buffer %p+%#lx (%s): %llu MB", sh ? b.contents : NULL, (unsigned long)b.length, sh ? "shared" : "private", (unsigned long long)(b.allocatedSize >> 20)); } }
} }
void mem_census(const char *why);
void dm_gfx_restop(void) {
    res_top("DM_CTL restop");
    mem_census("DM_CTL restop");
}
void dm_gfx_memtrim(void) {
    int cur = atomic_load(&g_mp_want);
    while (cur < 1 && !atomic_compare_exchange_weak(&g_mp_want, &cur, 1)) {}
    MLOG("DM_CTL memtrim: a memory pressure trim at the next submission's start");
}
// 32 failures within a minute end the game: failing command buffers keep wiring more memory (a kernel panic at 14.7 GB wired)
static void res_top_once(NSError *err) {
    if (!err || !strstr(err.description.UTF8String, "OutOfMemory")) return;
    static _Atomic int done, fails;
    static _Atomic uint64_t since;
    if (!atomic_exchange(&done, 1)) res_top("[RED] GPU out of memory");
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (now - atomic_load(&since) > 60000000000ull) {
        atomic_store(&since, now);
        atomic_store(&fails, 0);
    }
    if (atomic_fetch_add(&fails, 1) >= 32) MDIE("the GPU is out of memory: 32 command buffers failed within a minute (GPU allocated %llu MB, recommended working set %llu MB)", (unsigned long long)(g_dev.currentAllocatedSize >> 20), (unsigned long long)(g_dev.recommendedMaxWorkingSetSize >> 20));
}
void mtl_res_stats(char *out, size_t n) {
    pthread_mutex_lock(&g_res_commit_mu);   // the set's counts, which a commit changes
    unsigned long set_n = g_res ? (unsigned long)g_res.allocationCount : 0ul;
    double set_mb = g_res ? g_res.allocatedSize / 1048576.0 : 0;
    pthread_mutex_unlock(&g_res_commit_mu);
    pthread_mutex_lock(&g_res_mu);
    uint64_t tn = 0, tb = 0;
    for (int k = 0; k < RS_SITES; k++) {
        tn += g_rs_site[k].n;
        tb += g_rs_site[k].bytes;
    }
    size_t o = snprintf(out, n, "commits %llu / %.1f ms, added %llu, %.0f MB (", (unsigned long long)g_rs_commits, g_rs_commit_ns / 1e6, (unsigned long long)tn, tb / 1048576.0);
    for (int i = 0; i < 4 && o < n; i++) {
        int best = -1;
        for (int k = 0; k < RS_SITES; k++) if (g_rs_site[k].n && (best < 0 || g_rs_site[k].bytes > g_rs_site[best].bytes)) best = k;
        if (best < 0) break;
        o += snprintf(out + o, n - o, "%s%s:%d: %llu, %.0f MB", i ? ", " : "", g_rs_site[best].file, g_rs_site[best].line, (unsigned long long)g_rs_site[best].n, g_rs_site[best].bytes / 1048576.0);
        g_rs_site[best].n = 0;
    }
    if (o < n) o += snprintf(out + o, n - o, "), removed %llu, %lu allocations in the set (%.0f MB)", (unsigned long long)g_rs_rm, set_n, set_mb);
    if (o < n) snprintf(out + o, n - o, ", named on %llu command buffers in %.2f ms, longest %.2f ms", (unsigned long long)atomic_exchange(&g_res_names, 0), atomic_exchange(&g_res_name_ns, 0) / 1e6, atomic_exchange(&g_res_name_max, 0) / 1e6);
    memset(g_rs_site, 0, sizeof g_rs_site);
    g_rs_rm = g_rs_commits = g_rs_commit_ns = 0;
    pthread_mutex_unlock(&g_res_mu);
}

@interface NvDeferredRelease : NSObject { @public id object; uint64_t fence; int keep_resident; } @end
@implementation NvDeferredRelease @end
static NSMutableArray<NvDeferredRelease *> *g_deferred;
static pthread_mutex_t g_deferred_mu = PTHREAD_MUTEX_INITIALIZER;
// defer_obj_release: retired, dropped after its submissions complete and out of the residency set once no serial holds it; defer_obj_unref: only my reference goes, the object
// may still be in use. A texture still some serial's storage released as retired would be drawn non-resident
static void defer_kick(void);
// a reset finalizes thousands of pools: one drain at the end, not one per object
static _Atomic int g_defer_batch;
void mtl_defer_batch(int on) {
    atomic_store(&g_defer_batch, on);
    if (!on) defer_kick();
}
// a caller that knows no later submission can reach obj passes an earlier fence; one already passed drains at once
static void defer_obj_at(id obj, int keep_resident, uint64_t fence) {
    if (!obj) return;
    NvDeferredRelease *d = [NvDeferredRelease new];
    d->object = obj;
    d->fence = fence;
    d->keep_resident = keep_resident;
    pthread_mutex_lock(&g_deferred_mu);
    if (!g_deferred) g_deferred = [NSMutableArray array];
    [g_deferred addObject:d];
    pthread_mutex_unlock(&g_deferred_mu);
    // a feedback handler stores g_done before it drains: its drain finds the entry or this sees its fence
    if (fence <= atomic_load(&g_done) && !atomic_load(&g_defer_batch)) defer_kick();
}
static void defer_obj_release(id obj) { defer_obj_at(obj, 0, g_fence + 1); }
static void defer_obj_unref(id obj) { defer_obj_at(obj, 1, g_fence + 1); }
// Its own autorelease pool: on the guest submit thread the implicit pool never drains, and the retired object is autoreleased. → entries handed back (g_smu busy)
static int defer_drain(void) { int back = 0; @autoreleasepool {
    NSMutableArray<NvDeferredRelease *> *ready = [NSMutableArray array];
    pthread_mutex_lock(&g_deferred_mu);
    // one pass keeping the rest (removing from the middle is quadratic)
    uint64_t done = g_done;
    NSMutableArray<NvDeferredRelease *> *keep = nil;
    for (NvDeferredRelease *d in g_deferred) if (d->fence <= done) [ready addObject:d];
    if (ready.count) {
        keep = [NSMutableArray arrayWithCapacity:g_deferred.count - ready.count];
        for (NvDeferredRelease *d in g_deferred) if (d->fence > done) [keep addObject:d];
        g_deferred = keep;
    }
    pthread_mutex_unlock(&g_deferred_mu);
    // g_smu is only tried (this runs on completion handlers too); when busy, the rest go back to g_deferred and belong to the next drain: nothing here may read them after
    NSMutableArray<NvDeferredRelease *> *res_ready = nil, *rest = [NSMutableArray arrayWithCapacity:ready.count];
    for (NvDeferredRelease *d in ready) {
        if (!d->keep_resident && [d->object conformsToProtocol:@protocol(MTLResource)]) {
            if (!res_ready) res_ready = [NSMutableArray array];
            [res_ready addObject:d];
        } else {
            [rest addObject:d];
        }
    }
    if (res_ready && !pthread_mutex_trylock(&g_smu)) {
        for (NvDeferredRelease *d in res_ready) if (!own_retire_locked(d->object)) d->keep_resident = 1;
        pthread_mutex_unlock(&g_smu);
    } else if (res_ready) {
        ready = rest;
        back = (int)res_ready.count;
        pthread_mutex_lock(&g_deferred_mu);
        [g_deferred addObjectsFromArray:res_ready];
        pthread_mutex_unlock(&g_deferred_mu);
    }
    for (NvDeferredRelease *d in ready) {
        id obj = d->object;
        if (g_res_check_on) { pthread_mutex_lock(&g_res_mu); NSArray *u = [g_res_last_use objectForKey:obj]; pthread_mutex_unlock(&g_res_mu);
            CFIndex refs = CFGetRetainCount((__bridge CFTypeRef)obj);   // d, obj and the enumeration hold some
            if (u && [u[0] unsignedLongLongValue] > d->fence && (!d->keep_resident || refs <= 3) && g_res_late++ < 40) {
                int is_tex = [obj conformsToProtocol:@protocol(MTLTexture)]; id<MTLTexture> t = is_tex ? obj : nil;
                MLOG("[RED] released too early #%llu: %s %p (%lux%lu format %lu) retired behind fence %llu, bound by shader %s in submission %llu; %s, references %ld", (unsigned long long)g_res_late, is_tex ? "texture" : "object", (__bridge void *)obj, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.pixelFormat, (unsigned long long)d->fence, [u[1] UTF8String], [u[0] unsignedLongLongValue], d->keep_resident ? "reference dropped" : "out of the residency set", (long)refs); } }
        if (!d->keep_resident && [obj conformsToProtocol:@protocol(MTLResource)]) {
            pthread_mutex_lock(&g_res_mu);
            int registered = [g_res_objects containsObject:obj];
            pthread_mutex_unlock(&g_res_mu);
            if (registered) res_remove((id<MTLResource>)obj);
        }
        d->object = nil;
    }
} return back; }
// A drain off the submit thread for entries whose fence passed when queued: between submissions no feedback handler comes. A serial queue at utility QoS (defer_settle waits for it)
static dispatch_queue_t kick_q(void) {
    static dispatch_queue_t q;
    static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("dm.nvn.defer-kick", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q;
}
static void defer_kick(void) {
    static _Atomic int queued;
    if (atomic_exchange(&queued, 1)) return;
    dispatch_async(kick_q(), ^{ @autoreleasepool {
        atomic_store(&queued, 0);   // before the drain: an entry queued during it kicks again
        int back = defer_drain();
        res_commit();
        if (back) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), kick_q(), ^{ defer_kick(); });
    } });
}
// For tests: waits for queued feedback handlers, kicked drains, then drains (a 10 ms re-kick is not waited for)
static void __attribute__((unused)) defer_settle(void) {
    dispatch_sync(g_feedback_q, ^{});
    dispatch_sync(kick_q(), ^{});
    defer_drain();
}
// With no submission open no commit comes: the drain commits off this thread. Each side writes, then reads the other behind a full fence
static void defer_idle_commit(void) {
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load(&g_sub_open)) return;
    if (res_pending()) defer_kick();
}

// Shader keys appended once to the list file the environment names; 16 hex digits, with or without the route suffix
typedef struct { const char *env; pthread_mutex_t mu; char **keys; size_t n, cap; int loaded; } want_list;
static void want_add(want_list *w, const char *key) {   // w->mu held
    if (w->n == w->cap) {
        w->cap = w->cap ? w->cap * 2 : 64;
        w->keys = dm_realloc(w->keys, w->cap * sizeof *w->keys, "the shader key list");
    }
    w->keys[w->n++] = strdup(key);
}
static void want_note(want_list *w, const char *key) {
    const char *path = getenv(w->env);
    if (!path || !key || strlen(key) < 16 || (key[16] && key[16] != '_')) return;
    for (int i = 0; i < 16; i++) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f') || (key[i] >= 'A' && key[i] <= 'F'))) return;
    char k16[17];
    memcpy(k16, key, 16);
    k16[16] = 0;
    pthread_mutex_lock(&w->mu);
    if (!w->loaded) {
        FILE *f = fopen(path, "r");
        char line[64];
        if (f) {
            while (fgets(line, sizeof line, f)) {
                char *e = strpbrk(line, "\r\n");
                if (e) *e = 0;
                if (strlen(line) != 16) continue;
                want_add(w, line);
            }
            fclose(f);
        }
        w->loaded = 1;
    }
    for (size_t i = 0; i < w->n; i++) if (!strcmp(w->keys[i], k16)) { pthread_mutex_unlock(&w->mu); return; }
    want_add(w, k16);
    FILE *f = fopen(path, "a");
    if (f) {
        fprintf(f, "%s\n", k16);
        fclose(f);
    }
    pthread_mutex_unlock(&w->mu);
}
static want_list g_dual_want = {"DM_NVN_DUAL_WANT", PTHREAD_MUTEX_INITIALIZER};

#include "nvn_pool.inc"
#include "nvn_query.inc"
#include "nvn_submit.inc"
#include "nvn_ubuf.inc"
#include "nvn_fence.inc"
#include "nvn_texture.inc"
#include "nvn_sparse.inc"
#include "nvn_storage.inc"
#include "nvn_pass.inc"
#include "nvn_shader.inc"
#include "nvn_descpool.inc"
#include "nvn_state.inc"
#include "nvn_vertex.inc"
#include "nvn_pso_desc.inc"
#include "nvn_archive.inc"
#include "nvn_pso.inc"
#include "nvn_prewarm.inc"
#include "nvn_pooltab.inc"
#include "nvn_args.inc"
#include "nvn_scan.inc"
#include "nvn_feedback.inc"
#include "nvn_indirect.inc"
#include "nvn_mesh.inc"
#include "nvn_wline.inc"
#include "nvn_draw.inc"
#include "nvn_dispatch.inc"
#include "nvn_copy.inc"
#include "nvn_dlss.inc"
#include "nvn_exec.inc"
#include "nvn_present.inc"
#include "nvn_upscaler.inc"
#include "nvn_display.inc"
#include "nvn_capture.inc"
#include "nvn_ndve.inc"

#pragma clang attribute pop

uint64_t mtl_present_tickets(void) {
    pthread_mutex_lock(&g_pq_mu);
    uint64_t n = g_pq_in;
    pthread_mutex_unlock(&g_pq_mu);
    return n;
}
// A reset (dm_gfx_reset), g_sub_mu held, all game objects finalized: tables keyed by guest addresses that no Finalize empties
void mtl_reset_guest(void) {
    rc_forget();
}
