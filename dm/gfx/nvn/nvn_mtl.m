// Metal execution layer of the NVN null device, see nvn_mtl.h: the replay layer. One translation unit: this file holds the shared state (the device, the queues, the
// allocators), command buffers, the residency sets, recycled heaps and deferred release, then includes the rest in its order (the list below, after g_dual_want), each part after the parts it uses. The tests include this file
// to reach its static functions; the parts are not compiled on their own.
#import <Metal/Metal.h>
#import <QuartzCore/CAMetalLayer.h>
#import <QuartzCore/CAMetalDisplayLink.h>
#import <MetalFX/MetalFX.h>
#undef I   // MetalFX brings in <complex.h>, whose I (the imaginary unit) would shadow the local tables named I below
#import <ImageIO/ImageIO.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <dlfcn.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/sysctl.h>
#include <fcntl.h>
#include <sys/file.h>   // flock: the lock beside Metal's build folders (ga_claim)
#include <IOKit/IOKitLib.h>   // the GPU's core count (nvn_mesh.inc mesh_scratch)
#include <os/os_sync_wait_on_address.h>   // the uniform copy hand-off (nvn_rec.h ubc_wait)
#include <IOKit/IOKitLib.h>   // gpu_cores
#include "nvn_mtl.h"
#include "nvn_queue.h"
#include <mach/mach.h>
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
#include "dmsh_chain.h"   // DMSH_CHAIN: the translation chain version of the B shader library expected by this source tree, one for every game (beside this file, tools/dmsh_chain.py --header)
#include "game_profile.h"   // game profile constants (profile id, the profile's switches)
uint64_t nvn_ts_profile_hz = DM_GAME_GPU_TS_HZ;   // counter timestamp rate (nvn_timestamp.h)
// Texture slot table per stage: 0-127 the combined handles (nvnCommandBufferBindTexture), 128-255 the separate texture binds (NVN_SEPTEX_SLOT0, nvn_obj.h)
#define NTEX 256
// The NVN driver's constant segments c0 and c2 (byte offsets, slot counts) and the binding kinds of the shader reflection have one definition, which the shader build reads too (dmsh_build.py through dmsh_abi.py)
#include "dmsh_abi.h"
// c2 holds the handle tables (DMSH_C2_*), c0 the storage buffer table of each stage at DMSH_C0_SSBO_<stage> + 0x10 r (address 64 bits, size 32 bits). The replay fills one DMSH_DRV_SIZE buffer per segment a shader reads
_Static_assert(DMSH_C2_TEX + DMSH_C2_ROW * DMSH_C2_TEX_SLOTS == DMSH_C2_SEPTEX && DMSH_C2_SEPTEX + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS == DMSH_C2_SAMP && DMSH_C2_SAMP + DMSH_C2_ROW * DMSH_C2_SEP_SLOTS <= DMSH_DRV_SIZE && NVN_SEPTEX_SLOT0 + DMSH_C2_SEP_SLOTS == NTEX, "NVN driver handle tables");
_Static_assert(DMSH_C0_CS_UBO + DMSH_C0_ROW * DMSH_C0_CS_UBO_SLOTS == DMSH_C0_SSBO_CS && DMSH_C0_SSBO_PS + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS <= DMSH_C0_POINT_SIZE && DMSH_C0_POINT_SIZE + 4 <= DMSH_C0_SAMPLE_TABLE &&
               DMSH_C0_SAMPLE_TABLE + DMSH_C0_SAMPLE_BYTES <= DMSH_C0_SAMPLE_INFO && DMSH_C0_SAMPLE_INFO + 16 <= DMSH_C0_BASE_INSTANCE && DMSH_C0_BASE_INSTANCE + 4 <= DMSH_C0_SSBO_TS && DMSH_C0_SSBO_TS + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS == DMSH_C0_SSBO_MS && DMSH_C0_SSBO_MS + DMSH_C0_ROW * DMSH_C0_SSBO_SLOTS <= DMSH_DRV_SIZE, "NVN driver c0 tables");
// Geometry's counter/output addresses are c0+0x410/0x430 (NieR:Automata's MOVC/IADDC pairs); they use the logical NVN stage even when Metal executes a compute kernel.
static int c0_ssbo_base(int stage) { return stage == 0 ? DMSH_C0_SSBO_VS : stage == 1 ? DMSH_C0_SSBO_PS : stage == 5 ? DMSH_C0_SSBO_CS : stage == 3 ? DMSH_C0_SSBO_TS : stage == 4 ? DMSH_C0_SSBO_MS : -1; }   // NVN stages 0 vertex, 1 pixel, 5 compute, and NVN2's task and mesh in 3 and 4; no shader is a geometry shader (2)
// The handle a texture binding of slot reg reads: as c2 holds it (encode_args fills c2 the same way), a combined slot past 31 shares its word with an image slot, and a bound image
// is what lies there. A texture instruction on an image handle names such a slot: a size query of the image a compute shader writes (Unreal's GetDimensions on a RWTexture3D,
// dmsh_reflect.py decode_tex). DRAGON QUEST VII Reimagined's volumetric lightmap indirection patch read the empty texture slot 32 there, got a 1x1 size and wrote only cell (0, 0, 0)
static inline uint64_t tex_slot_h(const uint64_t *tex, const uint64_t *img, unsigned reg) {
    return reg >= 32 && reg - 32 < DMSH_C2_IMG_SLOTS && img[reg - 32] ? img[reg - 32] : reg < NTEX ? tex[reg] : 0;
}
static uint64_t c2_handle(int stage, unsigned b);   // the handle c2 holds at byte b (defined with the draw state, TEXA / SAMPA)
#include "nvn_hmap.h"
#include "nvn_owner.h"   // the serial tables (serial_put, tex_put, tex_get, the pins, the held counts) and their lock g_smu
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
// Function parameters in this file are never retained again in the callee (the caller / global tables already hold them, and lifetime is guaranteed by defer_obj_release): saves a retain / release pair per call on the hot path
#pragma clang attribute push(__attribute__((objc_externally_retained)), apply_to = function)

#define MLOG(...) dm_log("[dm] [nvn-mtl] " __VA_ARGS__)   // one line under one lock, written on the host stack when the caller is on a guest stack (dm.h dm_log): command recording runs on the game thread's stack
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
// Every compile reports service death before leaving the serialization gate, even when its caller ignores the error. Archive lookups do not compile.
static id compile_done(id result, NSError *error, NSError **out) {
    if (!result) { arch_died(error); if (out) *out = error; }
    plain_compile_end(); return result;
}
static id<MTLComputePipelineState> dev_cps(id<MTLFunction> f, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id ps = [g_dev newComputePipelineStateWithFunction:f error:&e]; return compile_done(ps, e, out);
}
static id<MTLRenderPipelineState> dev_rps(MTLRenderPipelineDescriptor *d, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id ps = [g_dev newRenderPipelineStateWithDescriptor:d error:&e]; return compile_done(ps, e, out);
}
static id<MTLRenderPipelineState> dev_mps(MTLMeshRenderPipelineDescriptor *d, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id ps = [g_dev newRenderPipelineStateWithMeshDescriptor:d options:MTLPipelineOptionNone reflection:nil error:&e]; return compile_done(ps, e, out);
}
static id<MTLFunction> dev_fn(id<MTLLibrary> lib, NSString *name, MTLFunctionConstantValues *cv, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id f = [lib newFunctionWithName:name constantValues:cv error:&e]; return compile_done(f, e, out);
}
static id<MTLLibrary> dev_lib(NSString *source, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id lib = [g_dev newLibraryWithSource:source options:nil error:&e]; return compile_done(lib, e, out);
}
// The shaders the host writes itself (clears, copies, index conversions, the present: dm/gfx/msl), compiled when the host is built and loaded by
// name (host_msl_lib), so none is compiled at run time. The diagnostic overrides (DM_NVN_HILITE, DM_NVN_MAGSCAN, DM_NVN_TEXDUMP) still compile theirs from source
static id<MTLLibrary> host_lib_from(id<MTL4Compiler> compiler, const char *name, NSError **out) {
    plain_compile_begin(); NSError *e = nil; id lib = host_msl_lib(g_dev, compiler, name, &e); return compile_done(lib, e, out);
}
static id<MTLLibrary> host_lib(const char *name, NSError **e) { return host_lib_from(nil, name, e); }
static int g_nvn_deswz_gpu;
static id<MTLCommandQueue> g_q;
static id<MTL4CommandQueue> g_q4;
static dispatch_queue_t g_feedback_q;
static _Atomic uint32_t g_present_qos;
#define ALLOC_MAX 8
enum { ALLOC_SHARE = 64 };   // submissions one allocator serves before the next takes another (see g_alloc_fence)
// The most submissions mtl_sub_begin lets stand uncompleted: ALLOC_MAX allocators of ALLOC_SHARE submissions each. It sizes what is held per submission in flight (POST_RING, VIS_RING)
enum { SUB_FLIGHT_MAX = ALLOC_MAX * ALLOC_SHARE };
static id<MTL4CommandAllocator> g_alloc4[ALLOC_MAX]; static int g_alloc_n, g_alloc_cur;   // Command allocator pool of the main submission: at start pick one the GPU has finished with, create a new one if there is none (wait only at the cap)
static id<MTL4CommandAllocator> g_up_alloc4[3];
static id<MTL4CommandAllocator> g_present_alloc4[3];
static id<MTL4CommandBuffer> g_present_cb[3];
static id<MTL4ArgumentTable> g_vt, g_ft, g_ct, g_up_ct, g_ot;   // g_ot: the object stage of a task / mesh draw (mesh_tasks_draw)
static int g_vbuf_cache = 1;   // DM_NVN_VBUF_CACHE=0 restores the uncached path for A/B measurements
static uint64_t g_vt_addr[31]; static uint32_t g_vt_valid;
static uint64_t g_ft_addr[31]; static uint32_t g_ft_valid;   // the fragment table's addresses as last set (enc_r_args): a waiting run of mesh draws is drawn before one changes
static uint64_t g_vbuf_resolve, g_vbuf_reuse, g_vbuf_set, g_vbuf_skip, g_vbuf_copies, g_vbuf_copy_bytes;   // copies: vertex streams at addresses not 4-byte aligned (vbuf_unaligned)
static id<MTL4ArgumentTable> g_present_args;
static id<MTLBuffer> g_present_uv[3];
static uint64_t g_present_alloc_fence[3];
// Under g_pq_mu: per present allocator, the newest ticket its commit feedback reported done on the GPU. present_now resets an allocator only once this reaches the ticket that
// last used it. g_pq_done is not that: every display-link callback sets it to ticket - 1 (the previous present was committed, not run) and a present that was not submitted sets
// it to its own ticket. Waiting on g_pq_done, a display-link present never waited here: with three presents in flight, or two after a dropped frame, the allocator was reset
// under a present the GPU had not run, and that present's drawable read back black (t_present_alloc; docs/reference/frame-rate.md)
static uint64_t g_present_alloc_done[3];
static pthread_mutex_t g_pq_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_pq_cv = PTHREAD_COND_INITIALIZER;
static pthread_cond_t g_link_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_link_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_link_tick_ns, g_link_tick_n;
static uint64_t g_link_dropped;
static uint64_t g_link_empty;   // Under g_pq_mu: ticks that found no new frame queued (counted here, not as ticks minus presents: presents before the link starts and while it is rescued are not on ticks)
// Frames standing in the present queue (nvn_link_drain.h): the callback asks the game's next acquire to wait g_link_drain_req more ticks, once (with LINK_DRAIN_LATCH: the
// callback that ends the drain latches that acquire's present), and mtl_link_wait_ticks does.
// g_link_blocked_ns: how long the acquire's last wait for the link blocked (0 = it was behind and did not wait), the callback's evidence that the game has room to spare;
// g_link_drain_ns: the time acquires spent on drains since mtl_link_drain_take_ns last asked (pacing, not a late frame: hle_nvn.c fstep_tick); g_link_drained: drains done
static nvn_link_drain g_link_drain;   // display-link callback thread only
static _Atomic int g_link_drain_req; static _Atomic uint64_t g_link_blocked_ns, g_link_drain_ns, g_link_drained;
// The late latch (nvn_link_latch.h): g_link_waiter, under g_link_mu, the tick an acquire blocks for in mtl_link_wait_ticks (0 = none), so the callback knows it woke one,
// and g_link_waiter_end, that this tick ends a drain into the latch (it latches through a give-up; a drain without LINK_DRAIN_LATCH sets no waiter);
// on the callback thread: latches that took the woken acquire's present, latches that timed out, and the timeouts in a row
static uint64_t g_link_waiter, g_link_latched, g_link_latch_out; static unsigned g_link_latch_misses; static int g_link_waiter_end;
static double g_link_hold_until; static uint64_t g_link_held;   // Changed only on the display-link callback thread: the time the previous frame must stay on screen until by its present interval (0 = no hold), and the ticks that took no new frame because of it
static int g_pq_inflight;   // Under g_pq_mu: number of presents committed to the GPU whose completion callback has not arrived yet
int g_dm_present_link;   // Present pacing mode (runtime value of DM_PRESENT_LINK, modified directly during same-process alternating measurement): 0 = acquire the drawable itself; 1 = display link at 60 Hz; 2 = display link at 120 Hz, the game presents a frame every other display-link tick
static int g_present_latency;   // DM_PRESENT_LATENCY: the display link's frame latency in a window and in fullscreen alike (0 = not set: link_latency picks it)
static int g_present_link_env;   // DM_PRESENT_LINK was set: its mode holds everywhere, and link_vrr_120 does not move a 60 Hz link to 120 Hz in variable-refresh fullscreen
static uint64_t g_pq_done;
static uint64_t g_pq_read;
static id<MTLResidencySet> g_res, g_snap_res;   // g_snap_res: the present snapshots' textures (snap_take)
// Residency changes wait in g_res_q (under g_res_mu, each holding its allocation) until a commit applies them to g_res and commits it, under g_res_commit_mu alone. A commit
// takes 10-25 ms when it adds hundreds of MB; while it held g_res_mu, as it did, every res_add and res_remove on any thread waited for it: the upload batches' completion
// handlers commit their staging's removal, and the submit thread's res_add calls of Switch Sports Resort's court load took 42-56 ms of that frame (1.5-1.7 ms queued).
// Queue commits that must stay apart from the set's commit (the present snapshot's, the encode thread's in nvn_rec.h) hold g_res_commit_mu too
typedef struct { CFTypeRef o; int add; } res_op;
static res_op *g_res_q; static size_t g_res_qn, g_res_qcap;   // under g_res_mu
static pthread_mutex_t g_res_commit_mu = PTHREAD_MUTEX_INITIALIZER;   // taken before g_res_mu, never inside it
// Residency by command buffer (PR #2715). No residency set is attached to a queue: every command buffer names the host's sets itself, a Metal 4 one in cb_end just before its end (a
// begin drops a command buffer's sets), a classic one in cl_cb when it is made, the present's also its drawable's layer set. With g_res attached to both queues instead, as it was,
// Ocarina of Time with guest files on fell in about half its launches into a state where the driver wired memory 53,000-93,000 times a second (IOGPUScheduler::freeToWireSysMemory
// under the queues' event signals, IOGPUMemory::prepare_memory_slow under their commits): 23-36 fps on a 16 GB M4 in 10 runs of 20, 13.6 fps median on a 16 GB M2 in 5 of 5; named
// per command buffer, in none of 20 and none of 4. Why the two differ is not known: Apple documents a queue's sets as named by every command buffer committed on it, so this is a
// choice of route around the driver's state, not a fix of its cause (docs/reference/frame-rate.md). A sparse mapping update (updateTextureMappings) is a queue operation and names no set.
// The naming holds g_res_commit_mu, apart from a set's commit, as the queue commits are made; another thread can hold it for a whole set commit, so each naming is timed with that wait
static _Atomic uint64_t g_res_names, g_res_name_ns, g_res_name_max;   // namings since the last residency line, their time with the wait for the lock, the longest one
static void res_name(id cb, id<MTLResidencySet> also) {   // cb: a Metal 4 or classic command buffer (both answer useResidencySet:)
    uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
    pthread_mutex_lock(&g_res_commit_mu);
    if (g_res) [cb useResidencySet:g_res];
    if (g_snap_res) [cb useResidencySet:g_snap_res];
    if (also) [cb useResidencySet:also];
    pthread_mutex_unlock(&g_res_commit_mu);
    uint64_t d = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0, m = atomic_load(&g_res_name_max);   // count, time and longest are reset apart: a naming can land in one line's time and the next line's count
    atomic_fetch_add(&g_res_name_ns, d); atomic_fetch_add(&g_res_names, 1);
    while (d > m && !atomic_compare_exchange_weak(&g_res_name_max, &m, d)) {}
}
static void cb_end(id cb, id<MTLResidencySet> also);   // every Metal 4 command buffer ends here (after nvn_rec.h)
static id<MTLSharedEvent> g_ev;
static _Atomic uint64_t g_fence;                // Highest committed fence value (incremented exclusively by the submit thread)
static _Atomic uint64_t g_sub_open;             // the open submission's fence, 0 with none open: from mtl_sub_begin until its commit has taken that fence and encoded everything (pool_gone_release)
static void (*g_pool_gone_between)(void);       // test hook (t_pool_retire): runs between pool_gone_release's two reads; the host never sets it
static int g_cut_copy;                          // a buffer → texture copy was encoded since the last commit: the next pass boundary commits the segment (sub_cut)
static pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_done;                 // GPU complete and all CPU-visible effects through this fence published
static _Atomic(uint32_t *) g_mesh_scr;         // the mesh scratch buffer's head words, once made (nvn_mesh.inc mesh_scratch): post_complete publishes g_done there
static _Atomic uint64_t g_arena_made, g_arena_live, g_arena_live_bytes, g_arena_freed, g_arena_freed_bytes;
static uint64_t g_cm_mdraws;
static uint64_t g_cm_enc, g_cm_enc0, g_cm_mdraws0, g_cm_hist[7], g_cm_big_enc, g_cm_big_draws, g_cm_big_z;   // per submission: encoders begun and Metal draws, by size, the largest (DM_NVN_ALLOCPROF=2)
static int g_allocprof = -1; static uint64_t g_ap_bytes[64], g_ap_fn[8];
static uint64_t g_md_masked_calls, g_md_masked, g_md_counted_calls, g_md_counted;   // indirect-count multi-draws: masked (every record up to the maximum encoded) and counted, with the draws each encoded
static uint64_t g_cm_in[4], g_cm_out, g_cm_inn[4]; static int g_cm_kind = -1;   // DM_NVN_ALLOCPROF=2: command memory added inside render [0] / compute [1] encoders, and between encoders
static void cm_enc_mark(int kind) {   // kind 0 / 1: an encoder begins; -1: it ends
    if (g_allocprof != 2 || !g_alloc4[g_alloc_cur]) return;
    static __unsafe_unretained id last; static uint64_t lz; id a = g_alloc4[g_alloc_cur]; uint64_t z = ((id<MTL4CommandAllocator>)a).allocatedSize;
    if (a == last && z >= lz) { if (g_cm_kind >= 0) g_cm_in[g_cm_kind] += z - lz; else g_cm_out += z - lz; }   // kinds 2 / 3: a render / compute encoder being made
    last = a; lz = z; g_cm_kind = kind; if (kind >= 0) g_cm_inn[kind]++;
}
// DM_NVN_ALLOCPROF=2: command memory. Each main submission takes a fresh command allocator (an allocator keeps its high-water size across resets), so its size is what that submission
// encoded; its growth (in chunks of about 2 MB) is charged to the replayed operation that caused it
static uint64_t g_cm_op[64], g_cm_opn[64], g_cm_sub_n, g_cm_sub_sum, g_cm_sub_max, g_cm_last;
static int allocprof_level(void) { if (g_allocprof < 0) { const char *e = getenv("DM_NVN_ALLOCPROF"); g_allocprof = e ? (atoi(e) > 1 ? 2 : 1) : 0; } return g_allocprof; }
// Diagnostic only: inclusive wall/CPU time on the replay thread. No GPU waits
// are added, and no clocks are read when DM_NVN_CPUPROF is absent or zero.
typedef struct { uint64_t wall, cpu; } cp_stamp;
static _Thread_local uint64_t g_cp_wall[6], g_cp_cpu[6], g_cp_calls[6], g_cp_last;
static int cp_on(void) { static _Thread_local int on = -1; if (on < 0) { const char *e = getenv("DM_NVN_CPUPROF"); on = e && atoi(e); } return on; }
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
    memset(g_cp_wall, 0, sizeof g_cp_wall); memset(g_cp_cpu, 0, sizeof g_cp_cpu); memset(g_cp_calls, 0, sizeof g_cp_calls); g_cp_last = now;
}
#define AP_B uint64_t _ap0 = g_allocprof > 0 ? g_dev.currentAllocatedSize : 0; cp_stamp _cp0 = cp_begin()
#define AP_E(i) do { if (g_allocprof > 0) { uint64_t _n = g_dev.currentAllocatedSize; if (_n > _ap0) g_ap_fn[i] += _n - _ap0; } cp_end(i, _cp0); } while (0)   // Per-frame arenas created so far (4 MiB each; for the memory report)
static _Atomic int g_last_commit_error;
static _Atomic int g_up_pending;

// macOS revokes a process's GPU access after GPU hangs or faults it caused: from then on every command buffer comes back MTL4CommandQueueErrorAccessRevoked ("Ignored (for causing
// prior/excessive GPU errors)", kIOGPUCommandBufferCallbackErrorSubmissionsIgnored) without running, the GPU signals no fence again and nothing is drawn; the game in #1326 ran about
// 70 s like that before it aborted, and in full screen WindowServer froze. The first one stops the game: presents stop, the front end closes the game, and the shared event a game's
// SyncWait waits on (for up to an hour, mtl_wait_at) is raised from the CPU to each fence whose feedback has come back. The feedback still comes, promptly, so g_done moves as
// before and no command allocator is reset before Metal has handed its command buffer back. A hang or a fault alone is not enough: the GPU recovers from those, and a command buffer
// discarded as the victim of another's error comes back with a timeout too while the game goes on, as it did in #1326's second run
static _Atomic int g_gpu_lost;
void front_gpu_lost(void);
__attribute__((weak)) void front_gpu_lost(void) { MDIE("GPU access revoked: macOS runs none of this process's GPU work any more"); }   // the Mac front end leaves full screen, says why and quits
static void gpu_lost_release(void) {
    static os_unfair_lock lk = OS_UNFAIR_LOCK_INIT;   // never lower it
    os_unfair_lock_lock(&lk); uint64_t f = atomic_load(&g_done); if (g_ev && g_ev.signaledValue < f) g_ev.signaledValue = f; os_unfair_lock_unlock(&lk);
}
// Metal 4 reports a commit's error as MTL4CommandQueueErrorTimeout with no text, and the command buffer's own error underneath (NSUnderlyingErrorKey, NSMultipleUnderlyingErrorsKey):
// every command buffer error in both #1326 logs has that shape. Only this domain counts: an innocent victim's IOGPU error underneath has code 5 too
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
    gpu_lost_release();
    front_gpu_lost();
}
// DM_TEST_GPU_LOST=n (for testing): from the n-th submission on, imitate the revoked access without faulting the GPU: each submission's feedback reports it, in the shape the #1326
// logs show, and the GPU's signal of its fence is left out (its work still runs)
static int gpu_lost_test(void) {
    static int at = -2, n; if (at == -2) { const char *e = getenv("DM_TEST_GPU_LOST"); at = e ? atoi(e) : -1; }
    return at > 0 && ++n >= at;
}
static NSError *gpu_lost_test_error(void) {
    NSError *revoked = [NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorAccessRevoked userInfo:@{NSLocalizedDescriptionKey: @"DM_TEST_GPU_LOST: Ignored (for causing prior/excessive GPU errors)",
                                                                                                                             NSUnderlyingErrorKey: [NSError errorWithDomain:@"IOGPUCommandQueueErrorDomain" code:4 userInfo:nil]}];
    return [NSError errorWithDomain:MTL4CommandQueueErrorDomain code:MTL4CommandQueueErrorTimeout userInfo:@{NSUnderlyingErrorKey: revoked, NSMultipleUnderlyingErrorsKey: @[revoked]}];
}

static pthread_mutex_t g_res_mu = PTHREAD_MUTEX_INITIALIZER;   // Dedicated lock for the residency set: res_add is called on paths that hold g_mu (arena_new), so it cannot share g_mu
// Wait ledger (DM_NVN_WAITS=1): every place a thread of the host or the game waits on the CPU for the GPU or for the encode thread, counted per frame (calls, calls that blocked,
// the time they blocked) and logged every 300 frames with the frame's GPU idle. The game's own SyncWait is split by how far its fence is behind the newest one (0: the last submission)
enum { WS_SYNC, WS_SYNC_NEWEST, WS_SYNC_POLL, WS_ACQ, WS_FINISH, WS_ALLOC, WS_VIS, WS_MDI, WS_READTEX, WS_T2T, WS_DIAG, WS_SNAP, WS_UPZERO, WS_UPALLOC, WS_DRAIN, WS_UBC, WS_OLDQ, WS_N };
static const char *const ws_name[WS_N] = {"SyncWait (game, older fence)", "SyncWait (game, newest fence)", "SyncWait polls", "SyncWait on an acquire sync", "QueueFinish", "allocator", "occlusion ring",
    "indirect-count multi-draw", "texture readback", "texture copy t2t", "diagnostics", "snapshot ring", "upload batches", "upload slot", "encode thread drain", "uniform copy worker", "old-queue command buffer"};
static _Atomic uint64_t g_ws_n[WS_N], g_ws_blk[WS_N], g_ws_ns[WS_N];
static int ws_on(void) { static int v = -1; if (v < 0) { const char *e = getenv("DM_NVN_WAITS"); v = e && atoi(e) > 0; } return v; }
static inline uint64_t ws_t0(void) { return clock_gettime_nsec_np(CLOCK_UPTIME_RAW); }
static inline void ws_put(int site, uint64_t t0, int blocked) {
    atomic_fetch_add_explicit(&g_ws_n[site], 1, memory_order_relaxed);
    if (blocked) { atomic_fetch_add_explicit(&g_ws_blk[site], 1, memory_order_relaxed); atomic_fetch_add_explicit(&g_ws_ns[site], ws_t0() - t0, memory_order_relaxed); }
}
// For the game's waits: when each fence's command buffer ended on the GPU and when its feedback handler ran (CACurrentMediaTime), so a wait that blocked tells its wake-up delay
static struct { _Atomic uint64_t fence, wfence; double gpu_end, fb, wake; _Atomic int done; } g_fend[64];
static _Atomic uint64_t g_ws_wake_n, g_ws_wake_ns, g_ws_fb_ns;
static void ws_wake_pair(unsigned k) {   // both halves known: the wake-up delay after the GPU end, counted once
    if (atomic_load(&g_fend[k].fence) != atomic_load(&g_fend[k].wfence) || atomic_exchange(&g_fend[k].done, 1)) return;
    atomic_fetch_add(&g_ws_wake_n, 1); atomic_fetch_add(&g_ws_wake_ns, (uint64_t)(fmax(0, g_fend[k].wake - g_fend[k].gpu_end) * 1e9)); atomic_fetch_add(&g_ws_fb_ns, (uint64_t)(fmax(0, g_fend[k].fb - g_fend[k].gpu_end) * 1e9));
}
static void ws_wake(uint64_t fence) {   // a blocking SyncWait on this fence has woken
    unsigned k = fence % 64; g_fend[k].wake = CACurrentMediaTime(); atomic_store(&g_fend[k].wfence, fence); ws_wake_pair(k);
    static int once; if (!once++) { char nm[64] = ""; pthread_getname_np(pthread_self(), nm, sizeof nm); MLOG("[waits] first blocking SyncWait on the newest fence: thread '%s'", nm); }
}
static void ws_fb(uint64_t fence, double gpu_end) {   // the fence's feedback handler
    unsigned k = fence % 64; if (atomic_load(&g_fend[k].fence) != fence) { atomic_store(&g_fend[k].done, 0); g_fend[k].gpu_end = gpu_end; g_fend[k].fb = CACurrentMediaTime(); atomic_store(&g_fend[k].fence, fence); }
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
static _Atomic uint64_t g_sub_cmt;   // the last submission whose commit the encode thread has made (its fence; with no encode thread, the last committed: mtl_sub_commit)
static _Atomic int g_seg_inflight;   // main command buffers committed and not reported back yet (DM_NVN_SUB_IDLE_FB and test drain)
static int cut_busy_limit(void);     // maximum in-flight commits for sub_cut (0 = unbounded, 1 = default GPU-idle, N = bounded depth)
// A main command buffer's commit feedback, kept as blocks: the feedback of several submissions can then ride on one commit (nvn_rec.h, submissions merged into one command buffer)
@interface DmCommitFB : NSObject { @public NSMutableArray *blocks; }
- (void)addFeedbackHandler:(void (^)(id<MTL4CommitFeedback>))b;
@end
@implementation DmCommitFB
- (instancetype)init { if ((self = [super init])) blocks = [NSMutableArray array]; return self; }
- (void)addFeedbackHandler:(void (^)(id<MTL4CommitFeedback>))b { [blocks addObject:[b copy]]; }
@end
// The feedback one merged submission's blocks get: the command buffer's error and that submission's own part of its GPU time
@interface DmPartFB : NSObject { @public double t0, t1; NSError *err; }
@end
@implementation DmPartFB
- (CFTimeInterval)GPUStartTime { return t0; }
- (CFTimeInterval)GPUEndTime { return t1; }
- (NSError *)error { return err; }
@end
static MTL4CommitOptions *commit_opts(DmCommitFB *fb) {
    if (!fb) return nil;
    MTL4CommitOptions *o = [MTL4CommitOptions new]; NSArray *bl = fb->blocks;
    [o addFeedbackHandler:^(id<MTL4CommitFeedback> f) { for (void (^b)(id<MTL4CommitFeedback>) in bl) b(f); }];
    return o;
}
static id<MTL4CounterHeap> g_ts_heap;   // tentative: defined with the segment timestamps below
#include "nvn_rec.h"   // deferred encoding (DM_NVN_ENC_THREAD)
// Ends a Metal 4 command buffer, naming the residency sets first (see res_name). A recorded one (DM_NVN_ENC_THREAD) records its end, and the encode thread names them at RO_CB_END.
// Nothing else calls endCommandBuffer (dm/tools/tests/test_residency_naming.py)
static void cb_end(id cb, id<MTLResidencySet> also) {
    if (![cb isKindOfClass:[DmRecCB class]]) res_name(cb, also);
    [(id<MTL4CommandBuffer>)cb endCommandBuffer];
}
// A classic command buffer on g_q, the sets named (nothing else calls commandBuffer on g_q: the same test)
static id<MTLCommandBuffer> cl_cb(void) { id<MTLCommandBuffer> cb = [g_q commandBuffer]; res_name(cb, nil); return cb; }
static NSHashTable *g_res_objects;
static _Atomic unsigned g_image_heap_n, g_big_n, g_pair_n;   // g_big_n: large pools in chunks, whose buffers overlap (BIG_MIN); g_pair_n: storage pairs (pair_texture)
static inline MTL4VisibilityOptions resource_vis(void) { return MTL4VisibilityOptionDevice | (g_image_heap_n || g_big_n || g_pair_n ? MTL4VisibilityOptionResourceAlias : 0); }
// Residency churn by call site (the power line every 300 frames, mtl_res_stats): each commit with changes is a kernel call whose cost grows with what was added or removed.
// A site is a file and a line: the replay layer is nvn_mtl.m and the parts it includes
enum { RS_SITES = 16 }; static struct { const char *file; int line; uint64_t n, bytes; } g_rs_site[RS_SITES]; static uint64_t g_rs_rm, g_rs_commits, g_rs_commit_ns;
#define res_add(r) res_add_at((r), __FILE_NAME__, __LINE__)
// Both run on guest threads too (pool_res from mtl_addr, uploads), whose implicit autorelease pools never drain; containsObject: / removeObject: on a weak NSHashTable leave the object
// autoreleased in the calling thread's pool (measured in a standalone test: the object outlived its last strong reference until the thread ended), so each has a pool of its own
static void res_q_put(id<MTLAllocation> r, int add) {   // Caller holds g_res_mu
    if (g_res_qn == g_res_qcap) { g_res_qcap = g_res_qcap ? 2 * g_res_qcap : 256; g_res_q = dm_realloc(g_res_q, g_res_qcap * sizeof *g_res_q, "the residency changes"); }
    g_res_q[g_res_qn++] = (res_op){CFBridgingRetain(r), add};
}
static void res_add_at(id<MTLAllocation> allocation, const char *file, int line) { @autoreleasepool {
    if (!allocation) return;
    // A placed texture's heap covers storage; fragment sampling through its
    // views also needs the parent texture descriptor. Retire both through the
    // existing ownership and queued-residency paths, not a second heap queue.
    if ([(id)allocation conformsToProtocol:@protocol(MTLResource)] && ((id<MTLResource>)allocation).heap)
        res_add_at(((id<MTLResource>)allocation).heap, file, line);
    id<MTLAllocation> r = allocation;
    pthread_mutex_lock(&g_res_mu);
    if (!g_res_objects) g_res_objects = [NSHashTable weakObjectsHashTable];
    if ([g_res_objects containsObject:r]) { pthread_mutex_unlock(&g_res_mu); return; }   // already resident (a shared texture resolved again): marking the set dirty would cost a commit that changes nothing
    { int k = 0; while (k < RS_SITES - 1 && g_rs_site[k].line && (g_rs_site[k].line != line || strcmp(g_rs_site[k].file, file))) k++;
      g_rs_site[k].file = file; g_rs_site[k].line = line; g_rs_site[k].n++; g_rs_site[k].bytes += r.allocatedSize; }
    res_q_put(r, 1);
    [g_res_objects addObject:r];
    pthread_mutex_unlock(&g_res_mu);
} }
static void res_remove(id<MTLAllocation> r) { @autoreleasepool {
    if (!r) return;
    pthread_mutex_lock(&g_res_mu);
    if (![g_res_objects containsObject:r]) { pthread_mutex_unlock(&g_res_mu); return; }
    res_q_put(r, 0);
    [g_res_objects removeObject:r];
    g_rs_rm++;
    pthread_mutex_unlock(&g_res_mu);
} }
// Recycled texture memory (GitHub issues #1831, #1823). An engine that keeps its render targets in a GPU-only pool and moves or resizes them with its dynamic resolution names new
// storage or a new size every few frames, and each (pool, offset, description) gets a Metal texture of its own: Ocarina of Time's 305 MB target pool made 3,468 textures in about
// 1,000 frames of its village, 450-575 MB added to the residency set every 300 frames, while the storage takeover (nvn_owner.m's alias_take_over) let as many go. Metal does not lay a texture out
// as NVN does, so these targets cannot share the pool's memory as on the console: a heap that mirrors the pool corrupts the picture, as the game keeps live targets in the part of
// the full-size scene target its current resolution leaves unused (PR #1453). What can be kept is the memory of a texture the host has already let go. Each such texture is made in
// a placement heap of its own; when its texture is freed (after the submissions that used it completed, defer_drain), the heap waits here, still resident, for the next texture it fits.
// Every texture whose memory the host makes for the GPU alone draws from the same list: single-description targets in a GPU-only pool (rh_texture), the heaps of resolution families
// (image_texture), and same-pass feedback snapshots (fb_snap). A heap is made an eighth larger than its first texture, and a waiting heap serves a texture up to a third smaller
// than itself (the smallest that fits is taken), so a target that grows by a step of Metal's sizes (about 10% near 2 MB) fits the heap it freed the frame before: Mario Kart World's
// dynamic resolution moves its targets every frame (1480x832, 1544x868, 1548x868, 1488x836 and their halves and quarters), and with exact heaps taken only up to an eighth larger,
// family heaps freed for good and only COMPRESSIBLE targets recycled, its race made about 3,500 textures and heaps and added 5-6 GB to the residency set every 300 frames on an
// M6, with a residency commit for nearly every one; alternating that policy with this one in a run, 970 -> 272 commits and 1.8 -> 0.34 GB per 300 frames, replay CPU 6.94 -> 6.36 ms
// a frame (issue #1823). Only textures the GPU can render or write to are taken (block-compressed assets keep exact allocations of their own, without the headroom). The new
// texture is filled as a new allocation is (tex_init_fill or zeros; a family heap is zeroed through its largest description, a snapshot is copied into), so nothing it reads
// depends on the reuse; the memory was the old texture's only up to that texture's last use, which completed, as with memory Metal frees and hands out again, so no alias flush is
// added (resource_vis). Heaps idle for RH_IDLE_NS, and the oldest past
// RH_CAP bytes waiting, leave the residency set when a submission begins (rh_trim). DM_NVN_RT_RECYCLE=0 makes each texture its own allocation and each family heap exact and freed
// with its last texture, as before; ab alternates it with the DM_AB phases
enum { RH_CAP = 256u << 20 }; static const uint64_t RH_IDLE_NS = 3000000000ull;
typedef struct { void *heap; uint64_t size, t; } rh_ent;   // heap: retained (CFBridgingRetain); t: when it began to wait
static os_unfair_lock g_rh_lk = OS_UNFAIR_LOCK_INIT; static rh_ent *g_rh; static unsigned g_rh_n, g_rh_cap; static uint64_t g_rh_bytes;
static _Atomic uint64_t g_rh_made, g_rh_reused, g_rh_trimmed;   // for the memory readout
static int sw_mode(const char *name, int dflt); static inline int sw_on(int m);
static int rh_mode(void) { static int m = -1; if (m < 0) m = sw_mode("DM_NVN_RT_RECYCLE", 1); return m; }
// A heap whose texture was freed waits for the next texture it fits (with the switch off it is freed). Runs where the texture's last reference goes (defer_drain: the submit thread or a
// completion handler)
static void rh_wait(id<MTLHeap> heap) {
    if (!heap) return;
    if (!sw_on(rh_mode())) { res_remove(heap); return; }
    rh_ent e = {(void *)CFBridgingRetain(heap), heap.size, clock_gettime_nsec_np(CLOCK_MONOTONIC)};
    os_unfair_lock_lock(&g_rh_lk);
    if (g_rh_n == g_rh_cap) { g_rh_cap = g_rh_cap ? 2 * g_rh_cap : 64; g_rh = dm_realloc(g_rh, g_rh_cap * sizeof *g_rh, "the recycled texture heaps"); }
    g_rh[g_rh_n++] = e; g_rh_bytes += e.size;
    os_unfair_lock_unlock(&g_rh_lk);
}
@interface NvRecycledHeap : NSObject { @public id<MTLHeap> heap; } @end
@implementation NvRecycledHeap
- (void)dealloc { rh_wait(heap); }
@end
@interface NvImageHeap : NSObject { @public id<MTLHeap> heap; } @end   // a resolution family's heap (image_texture)
@implementation NvImageHeap
- (void)dealloc { rh_wait(heap); atomic_fetch_sub(&g_image_heap_n, 1); }
@end
static char g_image_heap_owner_key, g_rh_owner_key;
// A waiting heap can cover a nearby dynamic-resolution size. For new heaps, cap the extra space so a single large target does not keep tens of MB resident for reuse.
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
            if (h.storageMode != sm || h.cpuCacheMode != cm) continue;   // (a placed texture takes its heap's hazard tracking: a placement heap reports untracked, a descriptor default)
            best = i;
        }
        if (best < g_rh_n) { got = g_rh[best].heap; g_rh_bytes -= g_rh[best].size; g_rh[best] = g_rh[--g_rh_n]; }
        os_unfair_lock_unlock(&g_rh_lk);
    }
    if (got) { *reused = 1; atomic_fetch_add(&g_rh_reused, 1); return CFBridgingRelease(got); }
    MTLHeapDescriptor *hd = [MTLHeapDescriptor new]; hd.type = MTLHeapTypePlacement; hd.size = on ? size + MIN(size / 8, (uint64_t)RH_SLACK) : size;
    hd.storageMode = sm; hd.cpuCacheMode = cm; hd.hazardTrackingMode = hm;
    id<MTLHeap> h = [g_dev newHeapWithDescriptor:hd];
    if (h && on) atomic_fetch_add(&g_rh_made, 1);
    return h;
}
// A texture of this descriptor in recycled memory, at the start of its heap. nil when the switch is off or the descriptor cannot be placed. A heap taken from the list that the texture
// does not go into waits again; a new one goes
static id<MTLTexture> rh_place(MTLTextureDescriptor *d) {
    if (!sw_on(rh_mode()) || d.storageMode != MTLStorageModePrivate || d.textureType == MTLTextureTypeTextureBuffer) return nil;
    MTLSizeAndAlign need = [g_dev heapTextureSizeAndAlignWithDescriptor:d];
    int reused = 0; id<MTLHeap> heap = rh_heap(need.size, d.storageMode, d.cpuCacheMode, d.hazardTrackingMode, &reused);
    id<MTLTexture> mt = heap ? [heap newTextureWithDescriptor:d offset:0] : nil;
    if (!mt) { if (heap && reused) rh_wait(heap); return nil; }
    NvRecycledHeap *owner = [NvRecycledHeap new]; owner->heap = heap;
    objc_setAssociatedObject(mt, &g_rh_owner_key, owner, OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return mt;
}
// A shared texture in a GPU-only pool (tex_mtl_) that the GPU can render or write to, in recycled memory: the CPU never writes its storage, and the new texture is filled from what
// the host holds of it (tex_init_fill) or zeroed, as a new allocation is. NVN asks no flag of a render target (Mario Kart World's resized targets include some without COMPRESSIBLE),
// so the test is the Metal usage the host gives it: a texture of a block-compressed format, which is an asset the game copied in, keeps an allocation of its own and so its exact size
static int rh_fits(const tex_t *t, MTLTextureUsage usage) {
    return t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS) && (usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite))
        && !(t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_VIDEO_DECODE | NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_SPARSE | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET));
}
static id<MTLTexture> rh_texture(const tex_t *t, MTLTextureDescriptor *d) { return rh_fits(t, d.usage) ? rh_place(d) : nil; }
// Shared render targets another one drew over (tu_pass; nvn_owner.m's own_alias_dead_locked says why they go). A texture of the sharing table that the GPU alone writes (rh_fits,
// tex_mtl_) and that a pool walk reads as the dummy until a pass writes it (COMPRESSIBLE, pool_unwritten_target) has a record here, keyed by its address, with two stamps in
// g_tu_now's clock: its last use and its last write by the GPU. Uses: a lookup outside a pool walk (tex_mtl_: attachments, copies, clears, bindings by texture object), a binding of
// one of its texture IDs (tex_fast_rid, tex_by_id; each render pass encodes its bindings again, so a texture read every frame is stamped every frame) and a read the shaders mark in
// a pool (folded by the pass). A pool walk resolves every ID of the pool, read or not, so it stamps nothing. Writes: attach, image bindings, copy and clear destinations, DLSS output
// (tu_wrote). The record goes at the pass after its texture left the sharing table; a texture made at a freed one's address resets it (tu_register). Under g_tu_lk, a leaf lock
typedef struct { uint32_t use, wr, pass; } tu_rec;
static os_unfair_lock g_tu_lk = OS_UNFAIR_LOCK_INIT; static nvn_hmap g_tu = {.klen = sizeof(uintptr_t)};
static uint32_t g_tu_now = 1, g_tu_pass_no;   // deciseconds since the first submission, plus 1 (a stamp of 0 is none), set when a submission begins; the passes so far
static int g_pool_walk;   // a pool walk is resolving its IDs (pool_gpu, under g_rmu): not uses
static nvn_hmap g_tu_gone = {.klen = 8};   // serials whose texture a pass released, until it is made again (the count of those made again) or the set is full. Under g_tu_lk
static uint64_t g_tu_remade;
static tu_rec *tu_find(id o) {   // o's record, or its parent's for a view. Caller holds g_tu_lk
    for (id x = o; x; x = [x respondsToSelector:@selector(parentTexture)] ? ((id<MTLTexture>)x).parentTexture : nil) {
        uintptr_t k = (uintptr_t)(__bridge void *)x; tu_rec *r = nvn_hm_get(&g_tu, &k); if (r) return r; }
    return NULL;
}
static void tu_stamp(id o, int wrote) {
    if (!o || !g_tu.n) return;   // (n read unlocked: a record made meanwhile is stamped at its next use)
    uint32_t now = g_tu_now;
    os_unfair_lock_lock(&g_tu_lk); tu_rec *r = tu_find(o); if (r) { r->use = now; if (wrote) r->wr = now; } os_unfair_lock_unlock(&g_tu_lk);
}
static void tu_wrote(id o) { tu_stamp(o, 1); }
static void tu_register(id<MTLTexture> t, uint64_t serial) {
    uintptr_t k = (uintptr_t)(__bridge void *)t;
    os_unfair_lock_lock(&g_tu_lk);
    tu_rec *r = nvn_hm_get(&g_tu, &k); if (!r) { r = malloc(sizeof *r); if (!r) MDIE("the render target use records"); nvn_hm_put(&g_tu, &k, r); }
    *r = (tu_rec){g_tu_now, 0, g_tu_pass_no};
    if (g_tu_gone.n && nvn_hm_del(&g_tu_gone, &serial)) g_tu_remade++;
    os_unfair_lock_unlock(&g_tu_lk);
}
// When a submission begins: heaps that waited RH_IDLE_NS, and the oldest past RH_CAP bytes, leave the residency set and are freed; all of them when memory is short (mem_trim). → bytes freed
static uint64_t rh_trim_n(int all, unsigned *n_out) {
    if (!g_rh_n) { if (n_out) *n_out = 0; return 0; }
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), bytes = 0; unsigned n = 0;
    for (;;) {
        void *drop[64]; unsigned nd = 0;
        os_unfair_lock_lock(&g_rh_lk);
        while (g_rh_n && nd < 64) {
            unsigned old = 0; for (unsigned i = 1; i < g_rh_n; i++) if (g_rh[i].t < g_rh[old].t) old = i;
            if (!all && now - g_rh[old].t < RH_IDLE_NS && g_rh_bytes <= RH_CAP) break;
            drop[nd++] = g_rh[old].heap; bytes += g_rh[old].size; g_rh_bytes -= g_rh[old].size; g_rh[old] = g_rh[--g_rh_n];
        }
        int more = all && g_rh_n;
        os_unfair_lock_unlock(&g_rh_lk);
        for (unsigned i = 0; i < nd; i++) { id<MTLHeap> h = CFBridgingRelease(drop[i]); res_remove(h); }
        n += nd;
        if (!more || !nd) break;
    }
    if (n) atomic_fetch_add(&g_rh_trimmed, n);
    if (n_out) *n_out = n;
    return bytes;
}
static void rh_trim(void) { rh_trim_n(0, NULL); }
// The whole system's memory (host_statistics64), in MB: what the process's own figures miss (free
// and wired memory, docs/reference/memory.md). file: file-backed pages in memory
typedef struct { uint64_t total, free, wired, compressed, file, purgeable; int ok; } sys_mem_t;
static sys_mem_t sys_mem(void) {
    sys_mem_t m = {0}; vm_statistics64_data_t v; mach_msg_type_number_t n = HOST_VM_INFO64_COUNT; uint64_t total = 0; size_t tl = sizeof total;
    if (host_statistics64(mach_host_self(), HOST_VM_INFO64, (host_info64_t)&v, &n) != KERN_SUCCESS) return m;
    sysctlbyname("hw.memsize", &total, &tl, NULL, 0);
    uint64_t pg = vm_kernel_page_size;
    m.total = total >> 20; m.free = ((uint64_t)v.free_count + v.speculative_count) * pg >> 20; m.wired = (uint64_t)v.wire_count * pg >> 20;
    m.compressed = (uint64_t)v.compressor_page_count * pg >> 20; m.file = (uint64_t)v.external_page_count * pg >> 20; m.purgeable = (uint64_t)v.purgeable_count * pg >> 20; m.ok = 1;
    return m;
}
// Memory pressure (DISPATCH_SOURCE_TYPE_MEMORYPRESSURE): the system says when its free memory runs low, warn then critical, before it ends processes. Each event is logged with the
// system's figures, and asks the submit thread for a trim at the next submission's start (mem_trim): what no frame needs any more goes, and nothing waits on the GPU there.
// The Mac trims at critical only, since it can swap. DM_NVN_MEM_PRESSURE=0 logs only, 1 trims at warn, 2 at critical
static _Atomic int g_mp_want;   // the level a trim was asked at (1 warn, 2 critical), taken by mem_trim
static _Atomic uint64_t g_mp_n[3], g_mp_trims, g_mp_freed;   // events by level (0 back to normal), trims made, bytes they gave back
static _Atomic uint64_t g_sweep_n, g_sweep_bytes;   // idle uploaded textures freed without memory pressure (pkg_sweep)
static int mp_act_level(void) {
    static int lv = -1;
    if (lv < 0) { const char *e = getenv("DM_NVN_MEM_PRESSURE"); lv = e && *e ? atoi(e) : 2; if (lv < 0 || lv > 2) lv = 2; }
    return lv;
}
static void mem_pressure_said(char *b, size_t n) {
    snprintf(b, n, "memory pressure events warn %llu, critical %llu, back to normal %llu; trims %llu, %llu MB given back; idle uploaded textures freed without pressure %llu, %llu MB",
             (unsigned long long)atomic_load(&g_mp_n[1]), (unsigned long long)atomic_load(&g_mp_n[2]), (unsigned long long)atomic_load(&g_mp_n[0]),
             (unsigned long long)atomic_load(&g_mp_trims), (unsigned long long)(atomic_load(&g_mp_freed) >> 20),
             (unsigned long long)atomic_load(&g_sweep_n), (unsigned long long)(atomic_load(&g_sweep_bytes) >> 20));
}
static void mem_pressure_event(int lv) {   // lv: 0 normal, 1 warn, 2 critical (the source's handler; a test calls it directly)
    atomic_fetch_add(&g_mp_n[lv], 1);
    sys_mem_t m = sys_mem();
    int act = lv && mp_act_level() && lv >= mp_act_level();
    MLOG("[mem] memory pressure %s: system free %llu MB, wired %llu MB, compressed %llu MB, file-backed %llu MB; residency set %llu MB; %s", lv == 2 ? "critical" : lv ? "warn" : "back to normal",
         (unsigned long long)m.free, (unsigned long long)m.wired, (unsigned long long)m.compressed, (unsigned long long)m.file, (unsigned long long)(g_res ? g_res.allocatedSize >> 20 : 0),
         act ? "trimming at the next submission" : lv ? "no trim at this level here" : "");
    if (act) { int cur = atomic_load(&g_mp_want); while (lv > cur && !atomic_compare_exchange_weak(&g_mp_want, &cur, lv)) {} }
}
static void mem_pressure_start(void) {
    static dispatch_source_t src;
    src = dispatch_source_create(DISPATCH_SOURCE_TYPE_MEMORYPRESSURE, 0, DISPATCH_MEMORYPRESSURE_NORMAL | DISPATCH_MEMORYPRESSURE_WARN | DISPATCH_MEMORYPRESSURE_CRITICAL,
                                 dispatch_get_global_queue(QOS_CLASS_UTILITY, 0));
    if (!src) { MLOG("[RED] memory pressure: no dispatch source; memory is not trimmed on pressure"); return; }
    dispatch_source_set_event_handler(src, ^{ @autoreleasepool {
        unsigned long d = dispatch_source_get_data(src);
        mem_pressure_event(d & DISPATCH_MEMORYPRESSURE_CRITICAL ? 2 : d & DISPATCH_MEMORYPRESSURE_WARN ? 1 : 0);
    } });
    dispatch_resume(src);
}
// The R32 storage under a 32-bit colour texture that takes image atomics (tex_upgrade): resident as long as the colour view over it lives, which is what the game's names hold
@interface NvAtomicRaw : NSObject { @public id<MTLTexture> raw; } @end
@implementation NvAtomicRaw
- (void)dealloc { res_remove(raw); }
@end
static char g_atomic_raw_key;
// DM_RES_CHECK=1 (diagnostic): if textures / buffers the shader accesses through the argument block are not in the residency set, print the first 40 (textures are attributed to the actual allocation via parent texture / underlying buffer)
static int g_res_check_on;
// It also keeps the submission that last bound each texture or buffer, for defer_drain: an object retired behind an earlier fence than a binding's goes while that binding is on the GPU, which this
// check cannot see at the binding (it is still in the set then). A texture gone that way faults or, with its resource ID reused, reads another texture, measured on an M4 Pro: a freed texture's resource ID went to the next texture made
static NSMapTable *g_res_last_use; static uint64_t g_res_late;
static void res_use_note(id o, const char *what) {
    if (!g_res_last_use) g_res_last_use = [NSMapTable weakToStrongObjectsMapTable];
    [g_res_last_use setObject:@[@(g_fence + 1), @(what)] forKey:o];
}
static void res_check(id r, const char *what, int reg) {
    static int nbad;
    if (!g_res_check_on || !r) return;
    pthread_mutex_lock(&g_res_mu); res_use_note(r, what); pthread_mutex_unlock(&g_res_mu);
    if (nbad >= 40) return;
    int is_tex = [r conformsToProtocol:@protocol(MTLTexture)], is_buf = [r conformsToProtocol:@protocol(MTLBuffer)];
    if (!is_tex && !is_buf) return;   // Samplers etc. are not allocations
    id<MTLResource> a = r; int hops = 0;
    if (is_tex) { id<MTLTexture> t = (id<MTLTexture>)a; while (t.parentTexture) { t = t.parentTexture; hops++; } a = t.buffer ? (id<MTLResource>)t.buffer : (id<MTLResource>)t; }
    if (a != r) { pthread_mutex_lock(&g_res_mu); res_use_note(a, what); pthread_mutex_unlock(&g_res_mu); }
    pthread_mutex_lock(&g_res_mu); int ok = [g_res_objects containsObject:a.heap ? (id)a.heap : a]; pthread_mutex_unlock(&g_res_mu);
    if (!ok) { nbad++; id<MTLTexture> t = is_tex ? (id<MTLTexture>)r : nil;
        MLOG("[RED] missing residency #%d: %s slot %d %s %p (parent chain %d deep) allocated %lu B storage mode %lu%s", nbad, what, reg, is_tex ? "texture" : "buffer", (__bridge void *)a, hops, (unsigned long)a.allocatedSize, (unsigned long)a.storageMode,
             t ? [NSString stringWithFormat:@" format %lu %lux%lu type %lu usage %#lx", (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.textureType, (unsigned long)t.usage].UTF8String : ""); }
}
// Applies and commits every change made before the call. A change another thread's commit took is in the set when this returns too: that commit holds g_res_commit_mu until it is
// done. Each change holds its allocation until it is committed, so a removed one stays alive until it has left the set
static void res_commit(void) { @autoreleasepool {
    pthread_mutex_lock(&g_res_commit_mu);
    pthread_mutex_lock(&g_res_mu);
    res_op *q = g_res_q; size_t n = g_res_qn; g_res_q = NULL; g_res_qn = g_res_qcap = 0;
    pthread_mutex_unlock(&g_res_mu);
    if (n) {
        uint64_t t0 = clock_gettime_nsec_np(CLOCK_UPTIME_RAW);
        for (size_t i = 0; i < n; i++) { if (q[i].add) [g_res addAllocation:(__bridge id<MTLAllocation>)q[i].o]; else [g_res removeAllocation:(__bridge id<MTLAllocation>)q[i].o]; }
        [g_res commit];
        uint64_t dt = clock_gettime_nsec_np(CLOCK_UPTIME_RAW) - t0;
        pthread_mutex_lock(&g_res_mu); g_rs_commits++; g_rs_commit_ns += dt; pthread_mutex_unlock(&g_res_mu);
    }
    pthread_mutex_unlock(&g_res_commit_mu);
    for (size_t i = 0; i < n; i++) CFRelease(q[i].o);
    free(q);
} }
static int res_pending(void) { pthread_mutex_lock(&g_res_mu); int n = g_res_qn != 0; pthread_mutex_unlock(&g_res_mu); return n; }
// What the residency set holds: totals by kind and the largest objects. At the first GPU out-of-memory error (the error names none of them), and for DM_CTL "restop" (hle_hid.c). The set holds
// the game's pools (shared buffers over guest memory), textures, heaps and staging alike
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
void dm_gfx_restop(void) { res_top("DM_CTL restop"); mem_census("DM_CTL restop"); }
void dm_gfx_memtrim(void) { int cur = atomic_load(&g_mp_want); while (cur < 1 && !atomic_compare_exchange_weak(&g_mp_want, &cur, 1)) {} MLOG("DM_CTL memtrim: a memory pressure trim at the next submission's start"); }
// Going on helps nobody once command buffers keep failing: no frame reaches the window, and each attempt asks the driver to wire more. A host left running like that on a 16 GB Mac took the whole
// system down, hung for hours and then a kernel panic in IOGPUGroupMemory at 14.7 GB wired. So 32 failures within a minute end the game
static void res_top_once(NSError *err) {
    if (!err || !strstr(err.description.UTF8String, "OutOfMemory")) return;
    static _Atomic int done, fails; static _Atomic uint64_t since;
    if (!atomic_exchange(&done, 1)) res_top("[RED] GPU out of memory");
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    if (now - atomic_load(&since) > 60000000000ull) { atomic_store(&since, now); atomic_store(&fails, 0); }
    if (atomic_fetch_add(&fails, 1) >= 32) MDIE("the GPU is out of memory: 32 command buffers failed within a minute (GPU allocated %llu MB, recommended working set %llu MB)", (unsigned long long)(g_dev.currentAllocatedSize >> 20), (unsigned long long)(g_dev.recommendedMaxWorkingSetSize >> 20));
}
// Since the last call: residency commits (count, time), additions by source line (file:line, count, MB; the four largest by bytes) and removals
void mtl_res_stats(char *out, size_t n) {
    pthread_mutex_lock(&g_res_commit_mu);   // the set's counts, which a commit changes
    unsigned long set_n = g_res ? (unsigned long)g_res.allocationCount : 0ul; double set_mb = g_res ? g_res.allocatedSize / 1048576.0 : 0;
    pthread_mutex_unlock(&g_res_commit_mu);
    pthread_mutex_lock(&g_res_mu);
    uint64_t tn = 0, tb = 0; for (int k = 0; k < RS_SITES; k++) { tn += g_rs_site[k].n; tb += g_rs_site[k].bytes; }
    size_t o = snprintf(out, n, "commits %llu / %.1f ms, added %llu, %.0f MB (", (unsigned long long)g_rs_commits, g_rs_commit_ns / 1e6, (unsigned long long)tn, tb / 1048576.0);
    for (int i = 0; i < 4 && o < n; i++) { int best = -1; for (int k = 0; k < RS_SITES; k++) if (g_rs_site[k].n && (best < 0 || g_rs_site[k].bytes > g_rs_site[best].bytes)) best = k;
        if (best < 0) break; o += snprintf(out + o, n - o, "%s%s:%d: %llu, %.0f MB", i ? ", " : "", g_rs_site[best].file, g_rs_site[best].line, (unsigned long long)g_rs_site[best].n, g_rs_site[best].bytes / 1048576.0); g_rs_site[best].n = 0; }
    if (o < n) o += snprintf(out + o, n - o, "), removed %llu, %lu allocations in the set (%.0f MB)", (unsigned long long)g_rs_rm, set_n, set_mb);
    if (o < n) snprintf(out + o, n - o, ", named on %llu command buffers in %.2f ms, longest %.2f ms", (unsigned long long)atomic_exchange(&g_res_names, 0), atomic_exchange(&g_res_name_ns, 0) / 1e6, atomic_exchange(&g_res_name_max, 0) / 1e6);
    memset(g_rs_site, 0, sizeof g_rs_site); g_rs_rm = g_rs_commits = g_rs_commit_ns = 0;
    pthread_mutex_unlock(&g_res_mu);
}

@interface NvDeferredRelease : NSObject { @public id object; uint64_t fence; int keep_resident; } @end
@implementation NvDeferredRelease @end
static NSMutableArray<NvDeferredRelease *> *g_deferred;
static pthread_mutex_t g_deferred_mu = PTHREAD_MUTEX_INITIALIZER;
// Deferred release has two meanings that must be kept apart (the root of the GPU memory fault on the first frame on entering Travincal):
//   defer_obj_release = this object is retired: drop the reference after the submissions that used it complete, and remove it from the residency set once no serial entry holds it (the owner's own_retire_locked);
//   defer_obj_unref   = only the reference I hold is no longer wanted, the object itself may still be in use (the view cache of a texture ID may hold the base texture itself): only drop the reference, leave the residency set alone.
// Previously only the former existed: when the view cache was invalidated, a texture that 'is still the current storage of some serial' was also removed from the residency set. Invalidation used to happen only when the base texture was really replaced, so it was never hit;
// after the transparency chain's in-place composite swapped the storage of two textures, each of the two live textures was removed once this way, and afterwards they are used as attachments and sampled every frame as usual - the GPU accesses non-resident memory.
static void defer_kick(void);
// A reset finalizes thousands of pools at once (hle_nvn.c dm_gfx_reset), each of whose buffers is ready to release at once: one drain at the end instead of one per object, each of
// which walked the whole queue the earlier ones had filled while the next object waited for its lock (4,488 pools in Kokiri Forest took 1.3-1.7 s, nearly all in that wait)
static _Atomic int g_defer_batch;
void mtl_defer_batch(int on) { atomic_store(&g_defer_batch, on); if (!on) defer_kick(); }
// Most objects wait for the next submission's fence. A caller that knows no submission still to come can reach the object passes an earlier one (mtl_pool_del); a fence the GPU has
// already passed is then drained at once (defer_kick), as no feedback handler is left to do it
static void defer_obj_at(id obj, int keep_resident, uint64_t fence) {
    if (!obj) return;
    NvDeferredRelease *d = [NvDeferredRelease new]; d->object = obj; d->fence = fence; d->keep_resident = keep_resident;
    pthread_mutex_lock(&g_deferred_mu);
    if (!g_deferred) g_deferred = [NSMutableArray array];
    [g_deferred addObject:d];
    pthread_mutex_unlock(&g_deferred_mu);
    // After the entry is in: a feedback handler stores g_done before it drains, so either its drain finds the entry or this load sees its fence
    if (fence <= atomic_load(&g_done) && !atomic_load(&g_defer_batch)) defer_kick();
}
static void defer_obj_release(id obj) { defer_obj_at(obj, 0, g_fence + 1); }
static void defer_obj_unref(id obj) { defer_obj_at(obj, 1, g_fence + 1); }
// Its own autorelease pool: mtl_sub_begin drains before it pushes the submission's pool, on the guest submit thread, whose implicit pool is never drained. The weak-table lookups and the residency-set removal
// autorelease the retired object, so whatever that thread drained (instead of the completion handler, which wins the race only when the CPU is idle) stayed alive for good: in Metroid Dread every
// re-upload of its map mask kept the old texture and its 32 MB upload staging block, and GPU memory grew by several GB within minutes under CPU load.
// → how many finished entries it handed back to the queue (g_smu busy)
static int defer_drain(void) { int back = 0; @autoreleasepool {
    NSMutableArray<NvDeferredRelease *> *ready = [NSMutableArray array];
    pthread_mutex_lock(&g_deferred_mu);
    // One pass that keeps the rest: removing each from the middle moved the whole tail, quadratic in the queue, which a scene load fills (PR 1444 saw minutes in memmove)
    uint64_t done = g_done; NSMutableArray<NvDeferredRelease *> *keep = nil;
    for (NvDeferredRelease *d in g_deferred) if (d->fence <= done) { [ready addObject:d]; }
    if (ready.count) { keep = [NSMutableArray arrayWithCapacity:g_deferred.count - ready.count]; for (NvDeferredRelease *d in g_deferred) if (d->fence > done) [keep addObject:d]; g_deferred = keep; }
    pthread_mutex_unlock(&g_deferred_mu);
    // A retired resource a serial entry still holds keeps its residency: the owner decides (own_retire_locked: eligibility, after the fence here gave completion) and keeps
    // what an entry holds until its last entry goes. g_smu is only tried: this runs on the completion handler's thread too, which must not wait on
    // the submit thread's long holds; when it is busy, those entries wait for the next drain
    // Each entry is sorted once, here, into the resources that may leave the residency set and the rest. When g_smu is busy, the rest are drained now and the resources go back to
    // g_deferred: from then on they belong to whichever drain takes them next (the submit thread or a completion handler), which releases the object and clears d->object. So nothing
    // here may read an entry it has handed back: sorting them again after the hand-back sent conformsToProtocol: to an object another drain had just freed (a crash in
    // class_conformsToProtocol from a completion handler, ROMANCE OF THE THREE KINGDOMS XIV in its main menu)
    NSMutableArray<NvDeferredRelease *> *res_ready = nil, *rest = [NSMutableArray arrayWithCapacity:ready.count];
    for (NvDeferredRelease *d in ready) {
        if (!d->keep_resident && [d->object conformsToProtocol:@protocol(MTLResource)]) { if (!res_ready) res_ready = [NSMutableArray array]; [res_ready addObject:d]; }
        else [rest addObject:d];
    }
    if (res_ready && !pthread_mutex_trylock(&g_smu)) {
        for (NvDeferredRelease *d in res_ready) if (!own_retire_locked(d->object)) d->keep_resident = 1;
        pthread_mutex_unlock(&g_smu);
    } else if (res_ready) {
        ready = rest; back = (int)res_ready.count;
        pthread_mutex_lock(&g_deferred_mu); [g_deferred addObjectsFromArray:res_ready]; pthread_mutex_unlock(&g_deferred_mu);
    }
    for (NvDeferredRelease *d in ready) {
        id obj = d->object;
        if (g_res_check_on) { pthread_mutex_lock(&g_res_mu); NSArray *u = [g_res_last_use objectForKey:obj]; pthread_mutex_unlock(&g_res_mu);
            CFIndex refs = CFGetRetainCount((__bridge CFTypeRef)obj);   // d, obj and the array's enumeration hold some; a released object has nobody else
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
// A drain off the submit thread for entries whose fence had passed when they were queued. Between submissions no feedback handler comes: Ocarina of Time out of focus waits in
// oe::PopNotificationMessage and submits nothing, while its streaming threads remade a run of about 10 MB of a virtual pool 70 times a second (vrun_set). Each old buffer waited for
// the next submission's fence, and GPU allocated went from 11 to 20 GB in 12 s, with 6.7 GB of swap in 20 s (PR #1392). Entries handed back while g_smu was busy are tried again 10 ms later.
// The drains run on a serial queue of their own, at the utility QoS of the global queue they used to run on, so that they can be waited for (defer_settle)
static dispatch_queue_t kick_q(void) {
    static dispatch_queue_t q; static dispatch_once_t once;
    dispatch_once(&once, ^{ q = dispatch_queue_create("dm.nvn.defer-kick", dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_UTILITY, 0)); });
    return q;
}
static void defer_kick(void) {
    static _Atomic int queued;
    if (atomic_exchange(&queued, 1)) return;
    dispatch_async(kick_q(), ^{ @autoreleasepool {
        atomic_store(&queued, 0);   // before the drain: an entry queued during it kicks again
        int back = defer_drain(); res_commit();
        if (back) dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), kick_q(), ^{ defer_kick(); });
    } });
}
// Where everything retired so far has been released, for the tests (the host never waits for it): the feedback handlers already queued, the drains they or a retirement kicked
// (defer_kick), then a drain here. Waiting for the feedback queue alone was not enough: a kicked drain releases what it takes after its residency commit, past g_res_commit_mu
// (res_commit), so a commit on the waiting thread could return while the kicked one still released a family's textures (t_image_alias failed 18 runs in 20 without the
// encode thread, 13 with it, on an M2 Pro). A drain kicked again 10 ms later, for entries handed back while g_smu was busy, is not waited for
static void __attribute__((unused)) defer_settle(void) { dispatch_sync(g_feedback_q, ^{}); dispatch_sync(kick_q(), ^{}); defer_drain(); }
// What a drain takes out of the residency set leaves it, and is released, at the set's next commit, which the next submission makes. With none open none may come: the drain is
// committed off this thread then (a feedback handler's, or the submit thread closing its submission). Each side writes, then reads the other's state behind a full fence, so one of
// them sees what is left
static void defer_idle_commit(void) {
    atomic_thread_fence(memory_order_seq_cst);
    if (atomic_load(&g_sub_open)) return;
    if (res_pending()) defer_kick();
}

// A profile list of shader keys the host asks the next shader build for (bf_want.tsv, dual_want.tsv): each key appended once to the file the environment names. A shader's key is
// its 16 hex digits, with or without the route suffix (NvShader keys are "<key>_b"), as dmsh_build.py reads the list
typedef struct { const char *env; pthread_mutex_t mu; char **keys; size_t n, cap; int loaded; } want_list;
static void want_note(want_list *w, const char *key) {
    const char *path = getenv(w->env);
    if (!path || !key || strlen(key) < 16 || (key[16] && key[16] != '_')) return;
    for (int i = 0; i < 16; i++) if (!((key[i] >= '0' && key[i] <= '9') || (key[i] >= 'a' && key[i] <= 'f') || (key[i] >= 'A' && key[i] <= 'F'))) return;
    char k16[17]; memcpy(k16, key, 16); k16[16] = 0;
    pthread_mutex_lock(&w->mu);
    if (!w->loaded) {
        FILE *f = fopen(path, "r"); char line[64];
        if (f) { while (fgets(line, sizeof line, f)) { char *e = strpbrk(line, "\r\n"); if (e) *e = 0; if (strlen(line) != 16) continue;
                if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 64; w->keys = dm_realloc(w->keys, w->cap * sizeof *w->keys, "the shader key list"); }
                w->keys[w->n++] = strdup(line); } fclose(f); }
        w->loaded = 1;
    }
    for (size_t i = 0; i < w->n; i++) if (!strcmp(w->keys[i], k16)) { pthread_mutex_unlock(&w->mu); return; }
    if (w->n == w->cap) { w->cap = w->cap ? w->cap * 2 : 64; w->keys = dm_realloc(w->keys, w->cap * sizeof *w->keys, "the shader key list"); }
    w->keys[w->n++] = strdup(k16);
    FILE *f = fopen(path, "a"); if (f) { fprintf(f, "%s\n", k16); fclose(f); }
    pthread_mutex_unlock(&w->mu);
}
static want_list g_dual_want = {"DM_NVN_DUAL_WANT", PTHREAD_MUTEX_INITIALIZER};

// The rest of the replay layer, in its order
#include "nvn_pool.inc"          // the device, memory pools
#include "nvn_query.inc"         // conditional rendering, occlusion queries, writes queued in a submission
#include "nvn_submit.inc"        // allocators, command buffers, barriers, the copy encoder
#include "nvn_ubuf.inc"          // inline uniform updates, uniform shadows, GPU write pages
#include "nvn_fence.inc"         // GPU timing, completion, the game's waits
#include "nvn_texture.inc"       // texture lookups, packaged textures, uploads
#include "nvn_sparse.inc"        // placement sparse textures
#include "nvn_storage.inc"       // texture storage, tex_mtl
#include "nvn_pass.inc"          // render passes
#include "nvn_shader.inc"        // the shader map, libraries, functions
#include "nvn_descpool.inc"      // texture and sampler IDs, argument block arenas
#include "nvn_state.inc"         // draw state, program bindings
#include "nvn_vertex.inc"        // vertex inputs and conversions
#include "nvn_pso_desc.inc"      // colour state, pso_desc
#include "nvn_archive.inc"       // the pipeline archive
#include "nvn_pso.inc"           // pipeline builds, precompile, hybrid pipelines
#include "nvn_prewarm.inc"       // prebuild, pso_get, prewarm, the prebuild tool
#include "nvn_pooltab.inc"       // dummies, texture pool tables, stand-ins
#include "nvn_args.inc"          // argument buffers: encode_args
#include "nvn_scan.inc"          // the lookaheads of the replay position
#include "nvn_feedback.inc"      // a draw that samples its own attachment
#include "nvn_indirect.inc"      // indirect draws, 8-bit indices, face-split draws, multi-draws
#include "nvn_mesh.inc"          // NVN2 task and mesh shaders: DrawMeshTasks
#include "nvn_wline.inc"         // lines wider than a pixel: a quad per segment
#include "nvn_draw.inc"          // lazy clears, compute hoisting, draw
#include "nvn_dispatch.inc"      // compute pipelines, dispatch
#include "nvn_copy.inc"          // copies, compressed-alias pairs
#include "nvn_dlss.inc"          // NVN2's DLSS on MetalFX
#include "nvn_exec.inc"          // clears, segments, mtl_exec
#include "nvn_present.inc"       // the present, screenshots, texture dumps
#include "nvn_upscaler.inc"      // a game's upscaler on MetalFX
#include "nvn_display.inc"       // the present pass, the display link, the present thread
#include "nvn_capture.inc"       // DM_NVN_CAPTURE (one frame for dm/gfx/tests/nvn_replay)
#include "nvn_ndve.inc"          // DM_NDVE_ENGINE (nn::ndve's movie network)

#pragma clang attribute pop

uint64_t mtl_present_tickets(void) { pthread_mutex_lock(&g_pq_mu); uint64_t n = g_pq_in; pthread_mutex_unlock(&g_pq_mu); return n; }   // the presents queued so far (hle_nvn.c g_present_base)
// A reset (hle_nvn.c dm_gfx_reset), with g_sub_mu held, after every object of the game's has been finalized: the tables keyed by the game's addresses that no Finalize empties
void mtl_reset_guest(void) {
    rc_forget();   // counter reports, by guest address
}
