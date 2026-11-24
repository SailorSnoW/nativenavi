// Service layer initialization called by nninitInitializeSdkModule: on the device each connects to a Horizon system service (IPC), which the host does not have. The takeovers
// open no session; each service's later calls are taken over where they are used (hle_*.c). nn::os::InitializeVirtualAddressMemory (the SDK's os layer) and
// fs::SetLocalApplicationAccessLog / SetResultHandledByApplication (SDK-internal switches) stay the SDK's.
#include "hle.h"

static void noop_init(const char *what) { LOG("init takeover: %s (no-op)", what); }

HLE_FORCE("nndiagStartup", "connect to the lm log service; the host prints diagnostic logs itself", void, h_nndiagStartup, void) { noop_init("nndiagStartup"); }
HLE_FORCE("_ZN2nn2oe10InitializeEv", "connect to the am/oe environment services; environment queries are wired one by one to host defaults", void, h_oe_Initialize, void) { noop_init("oe::Initialize"); }
HLE_FORCE("_ZN2nn3aoc10InitializeEv", "add-on content (DLC) service: no session opened (no service process); the owned list comes from the aoc interface in hle_misc.c (Reign of the Warlock, non-empty)", void, h_aoc_Initialize, void) { noop_init("aoc::Initialize"); }
HLE_FORCE("_ZN2nn3err26InitializeContextWriterApiEv", "connect to the err error context service; the host does not report", void, h_err_InitCtx, void) { noop_init("err::InitializeContextWriterApi"); }
HLE_FORCE("_ZN2nn2fs24InitializeForApplicationEv", "connect to the fsp-srv file service; the file interface goes to the host file system + RomFS", void, h_fs_InitApp, void) { noop_init("fs::InitializeForApplication"); }
HLE_FORCE("_ZN2nn4pctl10InitializeEv", "connect to the pctl parental controls service; single-player has no parental controls", void, h_pctl_Initialize, void) { noop_init("pctl::Initialize"); }
HLE_FORCE("_ZN2nn4diag34InitializeApplicationAbortObserverEv", "register with the system's abort observer; host crashes are printed by the signal handler", void, h_diag_AbortObs, void) { noop_init("diag::InitializeApplicationAbortObserver"); }
