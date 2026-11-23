// Common header of the native Switch host (multi-module).
#pragma once
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // constants of the current game profile (generated from dm/games/<id>/game.tsv, chosen by the Makefile's GAME)

// Explicit host work does not occupy a guest core (Metal submission, or a blocked native codec lock). A contended host mutex must count as blocked: a signal's same-core handoff
// can otherwise wait forever for a peer that needs the signaller's native lock. Uncontended locks retain guest ordering; the counter nests inside existing host work.
extern __thread _Atomic int dm_host_work;
static inline int dm_host_mutex_lock(pthread_mutex_t *mutex) {
    int r = pthread_mutex_trylock(mutex);
    if (r != EBUSY) return r;
    dm_host_work++;
    r = pthread_mutex_lock(mutex);
    dm_host_work--;
    return r;
}
// Joining a native helper releases the guest core while that helper finishes; preserve any enclosing host-work scope.
static inline int dm_host_thread_join(pthread_t thread, void **result) {
    dm_host_work++;
    int r = pthread_join(thread, result);
    dm_host_work--;
    return r;
}

// One log line: the message and its newline under one stream lock (written apart, another thread's line could land between them and two messages share a log line).
// The host's dm_log (hle_bigstack.c) writes it on the thread's big host stack when the caller is on a guest stack: formatting one line takes about 2.7 KB of stack, and a take-over
// runs on the stack of the game thread that calls it, which can have less left (memory.md, Log lines on small guest stacks). The definition here is a weak one that writes on the
// caller's stack, for the programs that do not link hle_bigstack.c (unit tests, whose stacks are host stacks); the host's replaces it at link time
__attribute__((format(printf, 1, 2))) void dm_log(const char *fmt, ...);
#ifndef DM_LOG_IMPL
#include <stdarg.h>
__attribute__((weak)) void dm_log(const char *fmt, ...) {
    va_list ap; va_start(ap, fmt);
    flockfile(stderr); vfprintf(stderr, fmt, ap); fputc('\n', stderr); funlockfile(stderr);
    va_end(ap);
}
#endif
#define LOG(...) dm_log("[dm] " __VA_ARGS__)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
enum { DT_INIT = 12, DT_FINI = 13, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };
#define ELF64_ST_BIND(i) ((i) >> 4)            // symbol binding: 0 = STB_LOCAL, 2 = STB_WEAK
#define ELF64_ST_VISIBILITY(o) ((o) & 3)       // visibility: 0 = STV_DEFAULT
enum { STB_LOCAL = 0, STB_WEAK = 2, STV_DEFAULT = 0 };

// Run-time state of one NSO module
typedef struct {
    char name[16];
    char dir[1024];            // the folder holding its rewriter output (<name>.patched.bin, .fixups.tsv, ...): the modules folder, or an add-on's own
    int addon;                 // an add-on module (the practice tools' subsdk9, hle_practice.c): its imports also bind to HLE_ADDON takeovers (hle.h)
    uint8_t *base;             // Start address of the image (including the patch area) in the host address space
    size_t size;
    uint64_t text_end, ro_off, data_off, key_page_off, key_page_size;   // From the rewriter fixups header
    uint64_t fps_state_off;    // frame step state page (patch_nso.py FRAME_STEP: Tears of the Kingdom 1.4.3 and Breath of the Wild 1.9.0 main); 0 = none
    int64_t patch_off;         // patch area relative to the base; negative = before the base (image too large, see patch_nso.py FAR), and size does not include it
    uint64_t image_size, file_image_len, patch_len;   // only with the patch area before the base: whole image (with bss and key page), length of the image part of the file, patch area length
    uint64_t back_off, back_len;   // a second patch area after the image (patch_nso.py TWO: a text longer than a B reaches), inside size; 0 = none
    char build_id[33];         // First 16 bytes of the NSO build ID (hex); empty for older build outputs
    unsigned key_idx[4];       // key page index of each thread slot (K18, KTLS, KTLR, KGATE)
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh; unsigned symh_mask;   // Name hash table of defined symbols (open addressing, stores symbol indices; for a duplicated name only the smallest index is kept = the first hit of a linear scan)
    uint64_t init, init_array, init_arraysz;   // init = DT_INIT (the module's _init: registers the DSO and runs this module's init_array)
} module_t;

extern module_t *g_mod;         // allocated once from the module list; module addresses stay stable
extern int g_nmod;
// main's build ID (empty if none): code with fixed addresses in a game's module runs only on the build they were found in
static inline const char *dm_main_build(void) { for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) return g_mod[i].build_id; return ""; }
// Four thread-local slots: K18 = the Switch's x18, KTLS = tpidr_el0, KTLR = tpidrro_el0 (ThreadLocalRegion), KGATE = host gate entry; the actual key numbers are written into the key page by the loader
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;

void load_modules(const char *dir, const char **names, const char *const *dirs, int n);   // dirs[i] (dirs may be NULL): an add-on module's own folder, NULL for one of dir
void run_module_inits(void);
void dm_ctl_peek(uint64_t a, int n); void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path);   // DM_CTL "peek" and "memdump": guest memory to the log or a file (probe.c)
void dm_device_share(const void *p, uint64_t size, int share);   // guest memory a device maps (share 1) or no longer maps (0): Horizon's DeviceShared attribute, which refuses unmapping it (hle_svc.c)
void *module_sym(module_t *m, const char *name);
int dm_sdk_major(void);   // SDK major version from the sdk module's version string (loader.c); 0 = unknown
int dm_sdk_ounce(void);   // 1 when the sdk module was built for the Switch 2 (its spec string "spec: Ounce", loader.c): the only kind this host runs; main.c reports another in red
int dm_sdk_nvn2_item(unsigned item, int *out);   // a constant NVN2's own nvnDeviceGetInteger answers for item, read from the sdk module (loader.c); 0 when it has none
int dm_game_imports(const char *prefix);   // main imports a symbol whose name starts with prefix (loader.c)
void *guest_sym(const char *name);
module_t *module_of(const void *p);
// Gate dispatch: rewritten svc / timer reads come in through the common entry, ret_addr = the return point of that gate block, regs points to the saved x0..x30
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
// Observation gates (rewriter OBS tables in the profiles' patches.py, applied by patch_nso.py; numbering matches): read-only callbacks, called on a gate hit once attached
// IDs 19 / 20 belonged to the removed movie request-ring workaround. Old rewritten modules may still contain them; without callbacks those gates are harmless passes through.
enum { OBS_REGION_ASK = 22, OBS_REGION_GOT = 23, OBS_REGION_FREE = 24,   // the SDK's address regions, gates the rewriter places by symbol in every sdk (patch_nso.py "Address regions", hle_svc.c)
       OBS_STDALLOC_INIT = 25,                                         // StandardAllocator::Initialize's entry, the same way: its callback writes the size (hle_svc.c)
       OBS_MAX = 26 };
// A gate number's bits from 8 up are a parameter for its callback (OBS_REGION_GOT: the register that holds the address). Callbacks read the registers; a write to x0–x29 takes
// effect when the gate returns (entry.S restores them from the frame), which OBS_STDALLOC_INIT does
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param));
uint64_t now_ns(void);
uint64_t guest_ns(void);              // Guest clock (hle_os.c; does not advance while frozen, guest_clock_freeze_for)
uint64_t guest_ns_at_host_time(uint64_t host_ns);  // Host Mach-absolute ns projected into the current guest clock
int guest_clock_pinned(void);  // the guest clock is frozen or DM_DETCLOCK's: guest_ns_at_host_time maps every host time to one
uint64_t guest_ticks(void);           // The guest's system counter on that clock: the Switch 2's 31.25 MHz (hle_os.c)
uint64_t guest_tick_hz(void);         // That counter's rate, what a rewritten mrs cntfrq_el0 reads (hle_os.c)
int dm_detclock_enabled(void);
void guest_clock_freeze_for(int reason, int on);   // Freeze by reason (0 tick hold, 1 background, 2 system keyboard, 3 NVN prepare, 4 NVN archive init); see hle_os.c
void guest_clock_read_wait(void);     // A guest read of the system counter: counts it in dm_clock_reads and waits while backgrounded or preparing (hle_os.c)
extern __thread uint64_t dm_clock_reads;   // the calling thread's guest reads of the system counter (hle_os.c; thr_yield tells clock waits by them)
void dm_guest_frame_tick(void);        // Called on every present (hle_nvn.c): when DM_DETCLOCK is on, advances the guest clock by one fixed step
uint64_t dm_nvn_frame_count(void);
void dm_backtrace(uint64_t fp);
void crash_watch_frame(void);
void crash_dump_now(const char *why);   // Dump state on demand (threads + graphics wait points), without exiting
// Diagnostic switches: read once from environment variables when main starts (getenv takes the libc global environment lock, and on a hot path it would make all guest threads queue up)
extern uint64_t g_t0_ns;   // Host start time (now_ns), used for relative times in the log
extern struct dm_flags { int thr, svc, fs, dbg, imports, bootprof; } g_flag;
void *objmap_get(const void *guest); void objmap_put(const void *guest, void *host); void *objmap_del(const void *guest);
void hle_signal_system_event(void *g);   // hle_thread.c: signal a service-layer SystemEvent (any thread)
void hle_bind_system_event(void *g, int autoclear);   // On a guest thread, bind a guest SystemEvent to a kernel event
void hle_clear_system_event(void *g);    // hle_thread.c: clear a bound service-layer SystemEvent (any thread)
uint64_t hle_system_event_binding(void *g);   // Opaque lifetime ID of a binding, 0 if unbound; a rebind gets a new ID even if its address and handle are reused
int hle_signal_system_event_binding(uint64_t binding);   // Signal only this binding lifetime (any thread); 0 after its write handle closes
uint32_t hle_new_transfer_memory(void);   // hle_thread.c: the handle CreateTransferMemory gives the game (0: the table is full); closed like any other
uint32_t hle_dup_readable_handle(uint32_t h);   // hle_thread.c: a second read end of a kernel event (the copy a service hands its client), 0 if h is no read end
// Page fault hook / hook before the host writes guest memory (crash.c; used for write-protect dirty page tracking)
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_guest_write_prepare(void *addr, size_t len);
void dm_guest_heap_range(uint64_t *base, size_t *open_len);   // hle_svc.c: the guest heap's base and opened length (guest addresses), 0 / 0 when none
void dm_mods_init(void);              // Before load_modules (main.c): finds the mods folder's mods, in order, and puts their romfs folders into DM_ROMFS_OVERLAY (hle_mods.c)
void dm_mods_patch_module(module_t *m, const char *dir);   // A mapped, still writable module: the mods' .pchtxt / .ips patches for its build (loader.c map_image)
void dm_mods_add_builtin(const char *name, const char *dir);   // A built-in set of code patches (the practice tools' IPS): dir's .ips / .pchtxt at the highest priority, not in the Mods menu (hle_mods.c)
void dm_mods_check_done(void);        // DM_MODS_CHECK: writes the verdicts and ends the host (after dm_mods_report_unmatched)
void dm_mods_report_unmatched(void);  // After load_modules: the patch files no loaded module's build matched
const char *dm_mods_dir_path(void);   // The mods folder ("" when neither DM_MODS nor DM_ROOT is set); it need not exist
int dm_mods_count(void); const char *dm_mods_name(int i); int dm_mods_on(int i);   // Every mod folder found at the start, in priority order, on or off (the Mac window's Mods menu)
void dm_mods_set(int i, int on);      // Turn a mod on or off: its romfs folder at once, and in order.txt; dm_mods_edited() is 1 once a mod with code patches changed (those wait for the next start)
void dm_fs_overlay_set(const char *list);   // Replace the RomFS overlay folders while the game runs (hle_fs.c)
int dm_mods_edited(void);
int dm_mods_exclusive(int i);         // 1 for a pack's variant that replaces the same files as another of the pack: turning it on turns that one off
int dm_amiibo_scan(const char *path); // Put an amiibo dump on the virtual NFC reader (hle_sdk16.c); 0 when the file is not one
void dm_amiibo_remove(void);          // Take it off
const char *dm_amiibo_name(void);     // The amiibo on the reader, NULL when none
// Graphics lifecycle (hle_nvn.c): on=1 suspends (afterwards submit / present block, and in-flight GPU work is drained before returning), on=0 resumes; called by the iOS front end when entering and leaving the background
void dm_gfx_suspend(int on);
void dm_gfx_quit(int wait_ms);   // hle_nvn.c: the process is ending (the graceful exit): what the pipeline archive has not written yet, written, waiting up to wait_ms
void dm_gfx_quit_begin(void);   // hle_nvn.c: the process is about to end (the graceful exit, oe::ExecuteProgram's hand-over): the pipeline prewarm stops and the archive starts writing what it holds, without waiting
enum { DM_QUIT_ARCH_MS = 10000 };   // what any end of the process the host controls gives the pipeline archive's write (atexit, the graceful exit): a file of 900 bases took 1.8-4 s on Macs
void dm_gfx_enter(void); void dm_gfx_leave(void);   // hle_nvn.c: the gate dm_gfx_suspend closes, for callers outside the NVN layer
void dm_gfx_shot_at(long k, const char *path);   // graphics side: screenshot of the k-th frame after the current presented frame (nvn_mtl.m)
void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg);   // The same, then(arg) in place of the last _exit(0) (oe::ExecuteProgram: execve of the next program)
void dm_host_shutdown(const char *why);   // Graceful exit (hle_lifecycle.c): stop background threads and audio, stop the pipeline prewarm, drain graphics, write the pipeline archive, then _exit(0); used by SIGTERM and oe::ExecuteProgram
