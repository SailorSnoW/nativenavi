// Bootstrap: after loading the modules, initialize the SDK runtime in the real order of nn::init::Start (sdk+0x2b0470) in the SDK, then enter nnMain.
#include "dm.h"
#include "front.h"
#include "../gfx/nvn/dmsh_chain.h"
#include <CoreFoundation/CoreFoundation.h>
#include <stdatomic.h>
#include <sys/resource.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <unistd.h>
void probe_install(void); void hid_keys_init(void); void stubs_report(void); void crash_install(void); void guest_thread_init(void); void guest_gate_stack(int own_stack); uint32_t svc_thread_main_init(void);
typedef void (*fn0)(void); typedef void (*fn2)(uint64_t, uint64_t); typedef void (*fnp)(void *);
static void *req(const char *n){ void *p=guest_sym(n); if(!p) die("missing symbol %s", n); return p; }

// Game arguments: argv[0] is fixed to "game", and whatever follows "--" is appended as is (on a real device nn::init takes them from the process argument area and hands them to the SDK through SetHostArgc/Argv)
static char *g_argv[64] = {"game"}; static int g_argc = 1;

static void guest_main(void) {
    dm_reset_main_base = (uintptr_t)__builtin_frame_address(0);   // the bottom of the game's frames on this thread: a reset ends it at a gate above them (hle_reset.c)
    guest_thread_init();
    guest_gate_stack(0);   // the guest code runs on this thread's own stack: its SVCs get a stack of their own
    uint32_t hmain = svc_thread_main_init();   // Main thread handle: x0 of nnosInitialize (stored in an sdk global, used by ThreadManagerImplByHorizon); x1 = pointer to the argument area
    // Copy Start step by step: _init_libc1 = __nnmusl_init_locks (initializes the stdio etc. locks as recursive locks; skipping it self-deadlocks on nested locking in putc) + __init_ssp.
    // The three callbacks passed in by rtld: A only sets an rtld flag (skipped), B = each module's DT_INIT (before nnMain), C = destructors (not done)
    ((fn0)req("_init_libc0"))();
    LOG(">>> nnosInitialize"); ((fn2)req("nnosInitialize"))(hmain, 0);
    ((fn0)req("_init_libc1"))();
    LOG(">>> nninitInitializeSdkModule"); ((fn0)req("nninitInitializeSdkModule"))();
    // The game arguments are in place before nninitStartup, as on hardware (nn::os takes them from the process argument area in nnosInitialize): Unreal Engine's nninitStartup
    // (Octopath Traveler 0, main+0x12afc) reads -SwitchVirtualAddressSpaceSize= and its other memory options there
    ((fnp)req("_ZN2nn2os11SetHostArgcEi"))((void *)(uint64_t)g_argc);
    ((fnp)req("_ZN2nn2os11SetHostArgvEPPc"))(g_argv);
    LOG(">>> nninitStartup"); ((fn0)req("nninitStartup"))();
    ((fn0)req("_init_libc2"))();
    run_module_inits();   // rtld callback B: each module's DT_INIT (global constructors)
    { void dm_guest_spawn_selftest(void); dm_guest_spawn_selftest(); }   // DM_GUEST_SPAWN_TEST self-test of host-created guest threads (hle_guestthread.c)
    LOG(">>> nnMain"); ((fn0)req("nnMain"))();
    LOG("nnMain returned");
}

struct dm_flags g_flag;
uint64_t g_t0_ns;
static void flags_init(void) {
    g_flag.thr = !!getenv("DM_THR"); g_flag.svc = !!getenv("DM_SVC"); g_flag.fs = !!getenv("DM_FS");
    g_flag.dbg = !!getenv("DM_DBG"); g_flag.imports = !!getenv("DM_IMPORTS");
    g_flag.bootprof = !!getenv("DM_BOOTPROF");
}

// Zero point of the boot timeline = process creation time (p_starttime from sysctl KERN_PROC_PID, wall clock) converted to the now_ns monotonic time base; same definition on Mac / iOS
static uint64_t proc_start_mono_ns(void) {
    struct kinfo_proc kp; size_t n = sizeof kp; int mib[4] = {CTL_KERN, KERN_PROC, KERN_PROC_PID, getpid()};
    uint64_t mono = now_ns(); struct timeval tv; gettimeofday(&tv, NULL);
    if (sysctl(mib, 4, &kp, &n, NULL, 0) || !n) return mono;
    int64_t age = (int64_t)(tv.tv_sec - kp.kp_proc.p_starttime.tv_sec) * 1000000000ll + (tv.tv_usec - kp.kp_proc.p_starttime.tv_usec) * 1000ll;
    return age > 0 ? mono - (uint64_t)age : mono;
}

// The modules to load when the command line names none: those of the profile's build row for main's build (its build ID from the rewriter's main.fixups.tsv), else all the profile's
// modules. A game's versions may differ in modules: Mario Kart World's base game has main subsdk0 (a Mono runtime) sdk, its update 1.8.0 main subsdk0 (the movie player) subsdk1 sdk
static int profile_modules(const char *dir, const char **names) {
    const char *all[] = DM_GAME_MODULES;
    const struct { const char *build, *mods; } rows[] = {DM_GAME_BUILD_MODULES};
    char path[1024], line[256], bid[33] = "";
    snprintf(path, sizeof path, "%s/main.fixups.tsv", dir);
    FILE *f = fopen(path, "r");
    while (f && fgets(line, sizeof line, f)) if (sscanf(line, "# BUILD_ID %32s", bid) == 1) break;
    if (f) fclose(f);
    for (size_t r = 0; *bid && r < sizeof rows / sizeof *rows; r++) {
        if (!rows[r].build || strcmp(rows[r].build, bid)) continue;
        int n = 0;
        for (int k = 0; k < DM_GAME_NMOD; k++) {   // in the profile's load order; build_modules (game.py) made the row from it
            const char *s = strstr(rows[r].mods, all[k]); size_t l = strlen(all[k]);
            while (s && !((s == rows[r].mods || s[-1] == ' ') && (!s[l] || s[l] == ' '))) s = strstr(s + 1, all[k]);
            if (s) names[n++] = all[k];
        }
        if (n < DM_GAME_NMOD) LOG("modules of main's build %.16s: %s (the profile lists %d)", bid, rows[r].mods, DM_GAME_NMOD);
        return n;
    }
    for (int k = 0; k < DM_GAME_NMOD; k++) names[k] = all[k];
    return DM_GAME_NMOD;
}

// Host loading (independent of AppKit / Metal): read the NSOs, relocate, forced take-over, probes, keys, start prefetch
static pthread_key_t g_race_keys[512];
static unsigned g_race_n, g_race_want;
static void *keyrace_thread(void *unused);
static void host_load(int argc, char **argv) {
    g_t0_ns = proc_start_mono_ns();
    flags_init();
    { void dm_diag_samplers_start(void); dm_diag_samplers_start(); }   // DM_MEMSTAT / DM_THRCPU (memstat.m)
    if (argc < 2) die("usage: dmhost <dir> [mod...] [-- game arguments...]");
    // Time zone: a game gets local time from nn::time (ToCalendarTime / ToPosixTime, hle_time.c), which converts with the host's zone, so its screens show this machine's time.
    // The guest C library's localtime / mktime stay UTC, as on a Switch. Only deterministic comparison (DM_FIXED_TIME) pins the host to UTC: the same replay must convert to the same
    // calendar time on this machine (UTC+8) and on the second Mac (UTC+9)
    if (getenv("DM_FIXED_TIME")) setenv("TZ", "UTC", 1);
    // Open file limit: the soft limit starts at 256 in a default macOS shell, while the game's file handles, the RomFS archives, save transactions and Metal caches share it. Raise it to 4096 (as far as the hard limit allows)
    struct rlimit rl; if (!getrlimit(RLIMIT_NOFILE, &rl) && rl.rlim_cur < 4096) {
        rlim_t was = rl.rlim_cur; rl.rlim_cur = rl.rlim_max < 4096 ? rl.rlim_max : 4096;
        if (setrlimit(RLIMIT_NOFILE, &rl)) LOG("open file limit %llu could not be raised", (unsigned long long)was);
    }
    crash_install();
    {   // Which build this is: the commit the packer stamped into the app (DMCommit; a run outside an app has none), the shader chain the host loads, the profile
        CFBundleRef b = CFBundleGetMainBundle(); CFTypeRef c = b ? CFBundleGetValueForInfoDictionaryKey(b, CFSTR("DMCommit")) : NULL; char cs[64] = "not recorded";
        if (c && CFGetTypeID(c) == CFStringGetTypeID()) CFStringGetCString((CFStringRef)c, cs, sizeof cs, kCFStringEncodingUTF8);
        LOG("build: commit %s, shader chain %s, profile %s", cs, DMSH_CHAIN, DM_GAME_ID);
    }
    const char *race = getenv("DM_TEST_KEYRACE");
    if (race && *race) {
        g_race_want = (unsigned)strtoul(race, NULL, 10);
        if (g_race_want > 512) g_race_want = 512;
        pthread_t th;
        if (pthread_create(&th, NULL, keyrace_thread, NULL)) die("DM_TEST_KEYRACE cannot start its thread");
        pthread_detach(th);
    }
    { void dm_term_install(void); dm_term_install(); }   // SIGTERM graceful exit (hle_lifecycle.c)
    { void crash_set_dump_hook(void (*)(void)); void svc_thread_dump(void); crash_set_dump_hook(svc_thread_dump); }   // Dump guest thread state on a crash
    const char *names[argc + DM_GAME_NMOD + 1], *dirs[argc + DM_GAME_NMOD + 1]; int n = 0;   // + 1: the practice tools' module
    memset(dirs, 0, sizeof dirs);   // NULL: a module of the modules folder
    int i = 2;
    for (; i < argc && strcmp(argv[i], "--"); i++) names[n++] = argv[i];
    if (i < argc && !strcmp(argv[i], "--")) for (i++; i < argc && g_argc < 63; i++) g_argv[g_argc++] = argv[i];
    else if (*DM_GAME_ARGS) {   // no "--" on the command line (the Mac app, the packing script's runs): the game profile's game_args, split at spaces
        char *s = strdup(DM_GAME_ARGS);
        for (char *t = strtok(s, " "); t && g_argc < 63; t = strtok(NULL, " ")) g_argv[g_argc++] = t;
        LOG("game arguments from the game profile: %s", DM_GAME_ARGS);
    }
    if (!n) n = profile_modules(argv[1], names);
    dm_mods_init();
    { int dm_practice_modules(const char *, const char **, const char **, int); n = dm_practice_modules(argv[1], names, dirs, n); }   // DM_PRACTICE=1 (hle_practice.c)
    load_modules(argv[1], names, dirs, n);
    { void dm_practice_check(void); dm_practice_check(); }
    dm_mods_report_unmatched();
    dm_mods_check_done();
    dm_sdk_major();   // logs the SDK version (loader.c)
    if (!dm_sdk_ounce()) LOG("[RED] sdk: the sdk module was not built for the Switch 2 (no \"spec: Ounce\"): this host has only the Switch 2's side (NVN2's API and counter reports, the 31.25 MHz system counter)");
    uint64_t ta = now_ns();
    probe_install();   // DM_PROBE probes (for diagnostics)
    hid_keys_init();   // DM_KEYS scripted input
    uint64_t tb = now_ns();
    stubs_report();
    if (g_flag.bootprof) LOG("① breakdown (after loading): probes / keys %.1f ms, stub report %.1f ms", (tb - ta) / 1e6, (now_ns() - tb) / 1e6);
    { void loader_hash_log(void); loader_hash_log(); }   // DM_LOADHASH=1: address-independent hash of the load result (loader.c)
}
static void *keyrace_thread(void *unused) {
    (void)unused;
    for (unsigned i = 0; i < g_race_want; i++) if (pthread_key_create(&g_race_keys[g_race_n++], NULL)) break;
    return NULL;
}
static void guest_entry(void) { dm_guest_boot(guest_main); }   // a reset of the game brings the main guest thread back here (hle_reset.c)
int main(int argc, char **argv) {
    dm_reset_set_argv(argv);
    { int mtl_pso_prebuild_tool(void); int r = mtl_pso_prebuild_tool(); if (r) return r == 2; }   // DM_PSO_PREBUILD=tool: what a first start would build, made with no game (nvn_mtl.m)
    host_load(argc, argv);
    { void dm_fps_state_apply(void); dm_fps_state_apply(); }   // the frame rate target and the dynamic resolution switch into the frame step state page (loader.c)
    { void loader_snapshot(void); loader_snapshot(); }   // the modules' writable parts as the game first starts with them, which a reset copies back (loader.c, hle_reset.c)
    front_run(guest_entry);
    return 0;
}
