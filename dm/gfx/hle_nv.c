// nv:: (GPU driver glue beneath NVN): nothing to do under the host's NVN. nv::SetGraphicsAllocator stays the SDK's: its movie extractor allocates through it
// (docs/reference/movies.md#movie-extractor-allocations-need-the-sdk-graphics-allocator)
#include "../host/hle.h"

static int g_nv_inited;

#define WHY "NVN has been replaced wholesale with a host null device, so the driver glue layer has nothing to do"
HLE_FORCE("_ZN2nv18InitializeGraphicsEPvm", WHY "; the initialisation is recorded for nv::InitCheck", void, h_nv_Init, void *mem, unsigned long size) {
    (void)mem; g_nv_inited = 1;
    LOG("nv::InitializeGraphics: pool %.2f MB, not used (the host's graphics layer allocates its own)", size / 1048576.0);
}
HLE_FORCE("_ZN2nv9InitCheckEv", WHY "; nv::InitializeGraphics recorded on the host", void, h_nv_InitCheck, void) {
    if (!g_nv_inited) die("nv::InitCheck: graphics not initialised (nv::InitializeGraphics was not called)");
}
HLE_FORCE("_ZN2nn2gs3sdk30CheckGraphicsSystemInitializedEPKcm", "the graphics system is the host's graphics layer, up from the start", void, h_gs_Check, const char *who, unsigned long n) { (void)who; (void)n; }
// Callers count the unscheduled part of [begin, end] as frame load; the application is the only client of the host's GPU timeline, so it holds all of it
HLE_FORCE("_ZN2nn2gs3sdk27QueryApplicationGpuTimeSpanEPNS_8TimeSpanEmm", "application GPU scheduling time: the application is the only client of the host's GPU timeline",
          Result, h_gs_GpuTimeSpan, int64_t *out, uint64_t begin, uint64_t end) { *out = end > begin ? (int64_t)(end - begin) : 0; return RES_OK; }
