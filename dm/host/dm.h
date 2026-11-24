// Common header of the host.
#pragma once
#include <pthread.h>
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game_profile.h"   // generated from dm/games/<id>/game.tsv

// Host work does not occupy a guest core. A contended host mutex counts as blocked, or a same-core signal handoff can
// wait forever for a peer that needs the signaller's lock. Nests.
extern __thread _Atomic int dm_host_work;
static inline int dm_host_mutex_lock(pthread_mutex_t *mutex) {
    int r = pthread_mutex_trylock(mutex);
    if (r != EBUSY) return r;
    dm_host_work++;
    r = pthread_mutex_lock(mutex);
    dm_host_work--;
    return r;
}
static inline int dm_host_thread_join(pthread_t thread, void **result) {
    dm_host_work++;
    int r = pthread_join(thread, result);
    dm_host_work--;
    return r;
}

// Message and newline under one stream lock. The host's (hle_bigstack.c) formats on the big host stack
// (docs/reference/memory.md#log-lines-on-small-guest-stacks); this weak one is for tests.
__attribute__((format(printf, 1, 2))) void dm_log(const char *fmt, ...);
#ifndef DM_LOG_IMPL
#include <stdarg.h>
__attribute__((weak)) void dm_log(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    flockfile(stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    funlockfile(stderr);
    va_end(ap);
}
#endif
#define LOG(...) dm_log("[dm] " __VA_ARGS__)
#define die(...) do { LOG("FATAL " __VA_ARGS__); abort(); } while (0)
// dies instead of returning NULL; what names the table
static inline void *dm_realloc(void *p, size_t bytes, const char *what) {
    void *q = realloc(p, bytes);
    if (!q && bytes) die("out of memory for %s (%zu bytes)", what, bytes);
    return q;
}

typedef struct { uint32_t st_name; uint8_t st_info, st_other; uint16_t st_shndx; uint64_t st_value, st_size; } Elf64_Sym;
typedef struct { int64_t d_tag; union { uint64_t d_val, d_ptr; } d_un; } Elf64_Dyn;
typedef struct { uint64_t r_offset, r_info; int64_t r_addend; } Elf64_Rela;
#define ELF64_R_SYM(i) ((i) >> 32)
#define ELF64_R_TYPE(i) ((i) & 0xffffffff)
enum { DT_INIT = 12, DT_FINI = 13, DT_RELA = 7, DT_RELASZ = 8, DT_JMPREL = 23, DT_PLTRELSZ = 2, DT_SYMTAB = 6, DT_STRTAB = 5,
       DT_INIT_ARRAY = 25, DT_INIT_ARRAYSZ = 27, DT_RELRSZ = 35, DT_RELR = 36 };
enum { R_AARCH64_ABS64 = 257, R_AARCH64_GLOB_DAT = 1025, R_AARCH64_JUMP_SLOT = 1026, R_AARCH64_RELATIVE = 1027 };
#define ELF64_ST_BIND(i) ((i) >> 4)
#define ELF64_ST_VISIBILITY(o) ((o) & 3)
enum { STB_LOCAL = 0, STB_WEAK = 2, STV_DEFAULT = 0 };

typedef struct {
    char name[16];
    char dir[1024];            // rewriter output folder: the modules folder, or an add-on's own
    int addon;                 // the practice tools' subsdk9: its imports also bind to HLE_ADDON takeovers
    uint8_t *base;
    size_t size;
    uint64_t text_end, ro_off, data_off, key_page_off, key_page_size;
    uint64_t fps_state_off;    // patch_nso.py FRAME_STEP state page; 0 = none
    int64_t patch_off;         // relative to base; negative = before it (patch_nso.py FAR), then not in size
    uint64_t image_size, file_image_len, patch_len;   // only when patch_off < 0
    uint64_t back_off, back_len;   // a second patch area after the image (patch_nso.py TWO), inside size; 0 = none
    char build_id[33];         // first 16 bytes, hex; may be empty
    unsigned key_idx[4];       // K18, KTLS, KTLR, KGATE
    const Elf64_Dyn *dyn;
    const Elf64_Sym *symtab;
    const char *strtab;
    unsigned nsym;
    uint32_t *symh; unsigned symh_mask;   // open addressing; a duplicated name keeps its smallest index
    uint64_t init, init_array, init_arraysz;
} module_t;

extern module_t *g_mod;         // allocated once: module addresses are stable
extern int g_nmod;
// fixed module addresses run only on the build they were found in
static inline const char *dm_main_build(void) {
    for (int i = 0; i < g_nmod; i++)
        if (!strcmp(g_mod[i].name, "main")) return g_mod[i].build_id;
    return "";
}
// K18 = x18, KTLS = tpidr_el0, KTLR = tpidrro_el0, KGATE = gate entry; the loader writes the key numbers into the key page
extern pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;

void load_modules(const char *dir, const char **names, const char *const *dirs, int n);
void run_module_inits(void);
void dm_ctl_peek(uint64_t a, int n);
void dm_ctl_memdump(uint64_t a, uint64_t n, const char *path);
void dm_device_share(const void *p, uint64_t size, int share);   // Horizon's DeviceShared attribute: unmapping it is refused
void *module_sym(module_t *m, const char *name);
int dm_sdk_major(void);
int dm_sdk_ounce(void);   // "spec: Ounce": the only kind this host runs
int dm_sdk_nvn2_item(unsigned item, int *out);   // 0 when the sdk has no constant for item
int dm_game_imports(const char *prefix);
void *guest_sym(const char *name);
module_t *module_of(const void *p);
// ret_addr = the gate block's return point, regs = saved x0..x30
uint64_t gate_dispatch(uint64_t ret_addr, uint64_t *regs);
// Observation gates, numbered as patches.py's OBS tables; 19 and 20 are retired (no callback, they pass through).
enum { OBS_REGION_ASK = 22, OBS_REGION_GOT = 23, OBS_REGION_FREE = 24,   // placed by symbol in every sdk (patch_nso.py "Address regions")
       OBS_STDALLOC_INIT = 25,
       OBS_MAX = 26 };
// Bits 8 and up are the callback's parameter; its writes to x0-x29 take effect when the gate returns
void gate_obs_set(int id, void (*fn)(uint64_t *regs, int param));
uint64_t now_ns(void);
uint64_t guest_ns(void);
uint64_t guest_ns_at_host_time(uint64_t host_ns);
int guest_clock_pinned(void);  // frozen or DM_DETCLOCK: every host time maps to one guest time
uint64_t guest_ticks(void);           // 31.25 MHz
uint64_t guest_tick_hz(void);
int dm_detclock_enabled(void);
void dm_fstep_settle(void);   // window size, visibility, screen or fullscreen changed, or graphics resumed
void guest_clock_freeze_for(int reason, int on);   // reason: 0 tick hold, 1 background, 2 system keyboard, 3 NVN prepare, 4 NVN archive init
void guest_clock_read_wait(void);     // counts dm_clock_reads; waits while backgrounded or preparing
extern __thread uint64_t dm_clock_reads;
void dm_guest_frame_tick(void);        // every present: one fixed step under DM_DETCLOCK
uint64_t dm_nvn_frame_count(void);
void dm_backtrace(uint64_t fp);
void crash_watch_frame(void);
void crash_dump_now(const char *why);
// Diagnostic switches are read once at start: getenv takes libc's environment lock
extern uint64_t g_t0_ns;
extern struct dm_flags { int thr, svc, fs, dbg, imports, bootprof; } g_flag;
void *objmap_get(const void *guest);
void objmap_put(const void *guest, void *host);
void *objmap_del(const void *guest);
void hle_signal_system_event(void *g);
void hle_bind_system_event(void *g, int autoclear);   // guest thread only
void hle_clear_system_event(void *g);
uint64_t hle_system_event_binding(void *g);   // 0 if unbound; a rebind gets a new id even with the same address and handle
int hle_signal_system_event_binding(uint64_t binding);   // any thread; 0 after its write handle closes
uint32_t hle_new_transfer_memory(void);   // 0: table full
uint32_t hle_dup_readable_handle(uint32_t h);   // 0 if h is no read end
void dm_fault_hook_register(int (*fn)(void *addr, int is_write, void *uc));
void dm_guest_write_prep_register(void (*fn)(void *addr, size_t len));
void dm_guest_write_prepare(void *addr, size_t len);
void dm_guest_heap_range(uint64_t *base, size_t *open_len);   // guest addresses; 0 / 0 when none
void dm_mods_init(void);              // before load_modules: puts the mods' romfs folders into DM_ROMFS_OVERLAY
void dm_mods_patch_module(module_t *m, const char *dir);   // while the module is mapped and writable
void dm_mods_add_builtin(const char *name, const char *dir);   // highest priority, not in the Mods menu
void dm_mods_check_done(void);
void dm_mods_report_unmatched(void);
const char *dm_mods_dir_path(void);   // "" when neither DM_MODS nor DM_ROOT is set
int dm_mods_count(void);
const char *dm_mods_name(int i);
int dm_mods_on(int i);
void dm_mods_set(int i, int on);      // code patches wait for the next start (dm_mods_edited)
void dm_fs_overlay_set(const char *list);
int dm_mods_edited(void);
int dm_mods_exclusive(int i);         // a pack variant: turning it on turns the other one off
int dm_amiibo_scan(const char *path); // 0 when the file is no amiibo dump
void dm_amiibo_remove(void);
const char *dm_amiibo_name(void);
// on=1: submit / present block, in-flight GPU work drained before it returns; on=0 resumes
void dm_gfx_suspend(int on);
void dm_gfx_quit(int wait_ms);   // writes what the pipeline archive still holds, waiting up to wait_ms
void dm_gfx_quit_begin(void);   // stops the prewarm and starts the archive write without waiting
enum { DM_QUIT_ARCH_MS = 10000 };   // the archive write at a controlled exit; 900 bases took up to 4 s
void dm_gfx_enter(void);   // the gate dm_gfx_suspend closes
void dm_gfx_shot_at(long k, const char *path);   // k frames after the current one
void dm_host_shutdown_then(const char *why, void (*then)(void *), void *arg);   // then(arg) replaces the last _exit(0) (oe::ExecuteProgram's execve)
void dm_host_shutdown(const char *why);   // stops threads, audio and the prewarm, drains graphics, writes the archive, _exit(0)
// The game's reset (Home > close > start again) inside this process; any thread, one at a time
void dm_guest_boot(void (*boot)(void));      // a reset brings the main guest thread back here
void dm_reset_set_argv(char **argv);
extern uintptr_t dm_reset_main_base;
// Weak stand-ins for tests, where no reset happens
#ifndef DM_RESET_IMPL
__attribute__((weak)) _Atomic int dm_reset_stop;
__attribute__((weak)) void dm_reset_gate(const uint64_t *regs) { (void)regs; }   // ends the guest thread if every frame below it is the game's
__attribute__((weak)) int dm_reset_gate_can_end(const uint64_t *regs, int role) { (void)regs; (void)role; return 0; }
__attribute__((weak)) void dm_reset_entry(uintptr_t sp) { (void)sp; }
__attribute__((weak)) void dm_reset_request(const char *why) { (void)why; }
#else
extern _Atomic int dm_reset_stop;
void dm_reset_gate(const uint64_t *regs);
int dm_reset_gate_can_end(const uint64_t *regs, int role);
void dm_reset_entry(uintptr_t sp);
void dm_reset_request(const char *why);
#endif
