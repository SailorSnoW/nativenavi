// nv:: (the GPU driver glue layer beneath NVN: creates transfer memory, connects to the nvdrv service). NVN has been replaced wholesale with a host null device, so this layer has nothing to do.
// nv::SetGraphicsAllocator and nv::SetGraphicsDevtoolsAllocator are not taken over: the SDK's own setters only check their arguments and install the callbacks behind NvOsAlloc / Free / Realloc
// (on Switch 2 they also set up nn::gs's graphics allocator), which the SDK's movie extractor still allocates with (movies.md, Movie extractor allocations need the SDK graphics allocator)
#include "../host/hle.h"

// Whether the game has installed its graphics pool (nv::InitCheck asks). Nothing is allocated from the pool on the host: the graphics layer allocates its own Metal objects
static int g_nv_inited;

#define WHY "NVN has been replaced wholesale with a host null device, so the driver glue layer has nothing to do"
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY "; the initialisation is recorded for nv::InitCheck", void, h_nv_Init, void *mem, unsigned long size) {
    (void)mem; g_nv_inited = 1;
    LOG("nv::InitializeGraphics: pool %.2f MB, not used (the host's graphics layer allocates its own)", size / 1048576.0);
}
// nv::InitCheck aborts unless graphics are initialised. Its own implementation tests the SDK's flag, which nv::InitializeGraphics sets, and that runs on the host instead:
// Donkey Kong Bananza (SDK 20.5.19 Ounce) calls it from main+0xe19d04 once gameplay starts, and aborted at sdk+0x57582c. The host's record of the initialisation answers it
HLE_FORCE("_ZN2nv9InitCheckEv", WHY "; nv::InitializeGraphics recorded on the host", void, h_nv_InitCheck, void) {
    if (!g_nv_inited) die("nv::InitCheck: graphics not initialised (nv::InitializeGraphics was not called)");
}
// The Switch 2's graphics system (nn::gs) check: SDK 22.2.0's (sdk+0x476d60) aborts unless the SDK's own flag reads 1, which the SDK's graphics boot sets on the console and the host, whose
// graphics layer stands in for that system from the start, never runs. Mario Kart World 1.8.0 checks it before it makes its camera context (by pull request #1116)
HLE_FORCE("_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm", "the graphics system is the host's graphics layer, up from the start", void, h_gs_Check, const char *who, unsigned long n) { (void)who; (void)n; }
// The graphics server's application GPU scheduling time: how much of [begin, end] (TimeSpan ns on the system tick clock) the GPU was given to the application rather than to
// another client of the console's GPU (the system's own rendering). SDK 20.5.29's query (sdk+0x411dc0) asks the gsv session (command 27), which the host never opens: its
// null context faulted at sdk+0x151fb0. The callers time GPU work with their own counter timestamps: Welcome Tour's Bezel profiler passes each render pass's GPU region
// (main+0x30a8f4) and the whole frame's (main+0x30a808), where the frame load it feeds dynamic resolution is the frame's scheduled time + the time the application was not
// scheduled since the last frame + 0.36 ms. A busy-time answer would count the GPU's idle time between frames as taken and read every frame as full. The host's counter
// timestamps are its model of the console GPU's timeline (nvn_mtl.m, ts_mode), on which the application is the only client, so it holds the GPU for the whole interval
HLE_FORCE("_ZN2nn2gs3sdk27QueryApplicationGpuTimeSpanEPNS_8TimeSpanEmm", "application GPU scheduling time: the application is the only client of the host's GPU timeline",
          Result, h_gs_GpuTimeSpan, int64_t *out, uint64_t begin, uint64_t end) { *out = end > begin ? (int64_t)(end - begin) : 0; return RES_OK; }
