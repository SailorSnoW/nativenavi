// Networking and peripheral services, all unavailable so the game takes its offline branch. "Saved / submitted" kinds return RES_OK so the game does not retry.
#include "hle.h"
#include "hle_net.h"

HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveERKNS_7account3UidE", "telemetry report save (user); nothing is sent, reports success", Result, h_prepo_SaveU, void *self, const void *uid) { (void)self; (void)uid; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo10PlayReport4SaveEv", "telemetry report save; nothing is sent, reports success", Result, h_prepo_Save, void *self) { (void)self; return RES_OK; }
// x8 is the hidden result pointer, which no C return type names. A null AsyncContext is the SDK's default: signaled event, result 0.
__attribute__((DM_NAKED)) static void h_prepo_Send(void) { __asm__("str xzr, [x8]\n    ret"); }
DM_REGISTER("swforce", hle_force_ent, hle_force_h_prepo_Send, {"_ZN2nn5prepo10PlayReport4SendERKNS_7account3UidE", (void *)h_prepo_Send, "telemetry report send; nothing is sent, a default context completes at once with success"});
HLE_FORCE("_ZN2nn5prepo27AddInternetConnectionStatusEPNS0_10PlayReportEPKcS4_S4_", "telemetry internet connection status: no service connection, nothing added, reports success", Result, h_prepo_AddNet, void *rep, const char *a, const char *b, const char *c) { (void)rep; (void)a; (void)b; (void)c; return RES_OK; }
HLE_FORCE("_ZN2nn5prepo28RequestImmediateTransmissionEv", "telemetry immediate upload; nothing is sent, reports success", Result, h_prepo_Tx, void) { return RES_OK; }

enum { SK_EAI_FAIL = 4 };
HLE_FORCE("_ZN2nn6socket10InitializeEPvmmi", "connects to the bsd socket service; offline, creates no session (cut)", Result, h_sk_Init4, void *mem, uint64_t sz, uint64_t alloc, int conc) { (void)mem; (void)sz; (void)alloc; (void)conc; return RES_OK; }
HLE_FORCE("_ZN2nn6socket8FinalizeEv", "pairs with Initialize (cut)", Result, h_sk_Fini, void) { return RES_OK; }
HLE_FORCE("_ZN2nn6socket12GetLastErrnoEv", "local UDP (offline BSD service)", int, h_sk_Errno, void) { return dm_sk_errno(); }
HLE_FORCE("_ZN2nn6socket6SocketEiii", "local UDP (offline BSD service)", int, h_sk_Socket3i, int a, int b, int c) { return dm_sk_socket(a, b, c); }
HLE_FORCE("_ZN2nn6socket8ShutdownEii", "local UDP (offline BSD service)", int, h_sk_Shutdown, int s, int h) { return dm_sk_shutdown(s, h); }
HLE_FORCE("_ZN2nn6socket4BindEiPK8sockaddrj", "local UDP (offline BSD service)", int, h_sk_Bind, int s, const void *a, unsigned n) { return dm_sk_bind(s, a, n); }
HLE_FORCE("_ZN2nn6socket4PollEP6pollfdmi", "waits on process-local UDP and event descriptors", int, h_sk_Poll, void *p, uint64_t n, int t) { return dm_sk_poll(p, n, t); }
HLE_FORCE("_ZN2nn6socket6SelectEiP6fd_setS2_S2_P7timeval", "Poll over process-local UDP and event descriptor sets", int, h_sk_Select2, int n, void *r, void *w, void *e, void *t) { return dm_sk_select(n, r, w, e, t); }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmiPK8sockaddrj", "local UDP (offline BSD service)", long, h_sk_SendTo, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return dm_sk_sendto(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket6SendToEiPKvmNS0_7MsgFlagEPKNS0_8SockAddrEj", "local UDP (offline BSD service)", long, h_sk_SendTo2, int s, const void *b, unsigned long n, int f, const void *a, unsigned al) { return dm_sk_sendto(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket8RecvFromEiPvmiP8sockaddrPj", "local UDP (offline BSD service)", long, h_sk_RecvFrom2, int s, void *b, unsigned long n, int f, void *a, unsigned *al) { return dm_sk_recvfrom(s, b, n, f, a, al); }
HLE_FORCE("_ZN2nn6socket12FreeAddrInfoEP8addrinfo", "pairs with GetAddrInfo (cut)", void, h_sk_FreeAi2, void *ai) { (void)ai; }
HLE_FORCE("_ZN2nn6socket11GAIStrErrorENS0_7AiErrnoE", "resolver error text", const char *, h_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
HLE_FORCE("_ZN2nn6socket7ConnectEiPK8sockaddrj", "local UDP (offline BSD service)", int, h_sk_Connect2, int s, const void *a, unsigned n) { return dm_sk_connect(s, a, n); }
HLE_FORCE("_ZN2nn6socket6AcceptEiP8sockaddrPj", "offline (cut)", int, h_sk_Accept2, int s, void *a, unsigned *n) { (void)s; (void)a; (void)n; return -1; }
HLE_FORCE("_ZN2nn6socket11GetSockNameEiP8sockaddrPj", "local UDP (offline BSD service)", int, h_sk_GetSockName2, int s, void *a, unsigned *n) { return dm_sk_name(s, a, n, 0); }
HLE_FORCE("_ZN2nn6socket10GetSockOptEiiiPvPj", "local UDP (offline BSD service)", int, h_sk_GetSockOpt2, int s, int l, int o, void *v, unsigned *n) { return dm_sk_getopt(s, l, o, v, n); }
HLE_FORCE("_ZN2nn6socket10SetSockOptEiiiPKvj", "local UDP (offline BSD service)", int, h_sk_SetSockOpt2, int s, int l, int o, const void *v, unsigned n) { return dm_sk_setopt(s, l, o, v, n); }
HLE_FORCE("_ZN2nn6socket5FcntlEiiz", "local UDP (offline BSD service)", int, h_sk_Fcntl2, int s, int c, int v) { return dm_sk_fcntl(s, c, v); }
HLE_FORCE("_ZN2nn6socket4RecvEiPvmi", "local UDP (offline BSD service)", long, h_sk_Recv2, int s, void *b, unsigned long n, int f) { return dm_sk_recvfrom(s, b, n, f, NULL, NULL); }
HLE_FORCE("_ZN2nn6socket4SendEiPKvmi", "local UDP (offline BSD service)", long, h_sk_Send2, int s, const void *b, unsigned long n, int f) { return dm_sk_sendto(s, b, n, f, NULL, 0); }

HLE_FORCE("_ZN2nn8irsensor15RunHandAnalysisERKNS0_14IrCameraHandleERKNS0_18HandAnalysisConfigE", "IR camera (cut)", Result, h_irs_RunHand, const void *h, const void *c) { (void)h; (void)c; return RES_FAIL; }

HLE_FORCE("_ZN2nn3ssl7Context15ImportClientPkiEPmPKcS4_jj", "SSL (cut)", Result, h_ssl_ImpCli, void *self, uint64_t *id, const char *a, const char *b, unsigned x, unsigned y) { (void)self; (void)a; (void)b; (void)x; (void)y; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context15ImportServerPkiEPmPKcjNS0_17CertificateFormatE", "SSL (cut)", Result, h_ssl_ImpSrv, void *self, uint64_t *id, const char *a, unsigned n, int f) { (void)self; (void)a; (void)n; (void)f; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl7Context9ImportCrlEPmPKcj", "SSL (cut)", Result, h_ssl_ImpCrl, void *self, uint64_t *id, const char *a, unsigned n) { (void)self; (void)a; (void)n; if (id) *id = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn3ssl14BuiltInManager22GetBuiltInCertificatesEPPNS0_26BuiltInCertificateInfoBaseEPhjPNS0_15CaCertificateIdEj", "SSL (cut)", Result, h_ssl_Certs,
          void **out, uint8_t *buf, unsigned bn, void *ids, unsigned n) { (void)buf; (void)bn; (void)ids; (void)n; if (out) *out = NULL; return RES_FAIL; }

HLE_FORCE("_ZN2nn4nifm27SubmitNetworkRequestAndWaitEv", "offline: the network request is not submitted (returns at once, IsNetworkAvailable is always false)", void, h_nifm_SubmitWait, void) {}
HLE_FORCE("_ZN2nn3nsd24GetEnvironmentIdentifierEPNS0_21EnvironmentIdentifierE", "server environment name: retail lp1", void, h_nsd_EnvId, char *out) { memset(out, 0, 8); memcpy(out, "lp1", 3); }
// Also answers oe::GetServerEnvironmentType (it branches here through the PLT); a request to the unserved nsd would abort.
HLE_FORCE("_ZN2nn3nsd35GetApplicationServerEnvironmentTypeEv", "server environment type: retail lp1 (1), as GetEnvironmentIdentifier; nsd is not served", int, h_nsd_AppSrvEnv, void) { return 1; }

HLE_FORCE("_ZN2nn7friends13GetFriendListEPiPNS0_6FriendERKNS_7account3UidEiiRKNS0_12FriendFilterE", "friend list is empty (cut)", Result, h_fr_List2,
          int *count, void *out, const void *uid, int off, int n, const void *filt) { (void)out; (void)uid; (void)off; (void)n; (void)filt; if (count) *count = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends25EnsureFriendListAvailableEPNS0_12AsyncContextERKNS_7account3UidE", "friend list (cut): the request fails at once", Result, h_fr_ListEnsure, void *ctx, const void *uid) { (void)ctx; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends14AddPlayHistoryERKNS_7account3UidERKNS0_26PlayHistoryRegistrationKeyERKNS0_15InAppScreenNameESA_", "play history is not registered (cut)",
          Result, h_fr_PlayHist2, const void *uid, const void *key, const void *a, const void *b) { (void)uid; (void)key; (void)a; (void)b; return RES_FAIL; }

HLE_FORCE("_ZN2nn3ngc15ProfanityFilter10InitializeEPvmb", "profanity filter init; the host has no word list", Result, h_ngc_Init3, void *self, void *work, uint64_t sz, unsigned check) { (void)self; (void)work; (void)sz; (void)check; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKDsm", "profanity check: no profanity in anything", Result, h_ngc_Check16, void *self, uint32_t *out, unsigned mask, const void **words, uint64_t n) {
    (void)self; (void)mask; (void)words;
    for (uint64_t i = 0; out && i < n; i++) out[i] = 0;
    return RES_OK;
}

HLE_FORCE("_ZN2nn3err9ShowErrorENS0_9ErrorCodeE", "error display applet; the host only logs it", void, h_err_ShowCode, uint64_t code) {
    DM_APPLET("error");
    LOG("game asks to show an error: %04u-%04u", (unsigned)(code & 0xffffffff), (unsigned)(code >> 32));
}

// The game constructs the return value itself.
HLE_FORCE("_ZN2nn3web11ShowWebPageEPNS0_18WebPageReturnValueERKNS0_14ShowWebPageArgE", "web page applet (cut)", Result, h_web_Page, void *rv, const void *arg) {
    (void)rv; (void)arg;
    LOG("web::ShowWebPage (cut)");
    return RES_FAIL;
}

// The arguments are all in registers and unread.
HLE_FORCE("nnsocketAccept", "offline (cut)", int, h_c_sk_Accept, void) { return -1; }
HLE_FORCE("nnsocketListen", "offline (cut)", int, h_c_sk_Listen, void) { return -1; }
HLE_FORCE("nnsocketRecvMsg", "offline (cut)", int, h_c_sk_RecvMsg, void) { return -1; }
HLE_FORCE("nnsocketSendMsg", "offline (cut)", int, h_c_sk_SendMsg, void) { return -1; }
HLE_FORCE("nnsocketShutdown", "local UDP (offline BSD service)", int, h_c_sk_Shutdown, int s, int h) { return dm_sk_shutdown(s, h); }
HLE_FORCE("nnsocketGetAddrInfoWithOptions", "name resolution with resolver options (cut): EAI_FAIL", int, h_c_sk_GaiOpt, const char *h, const char *s, const void *hint, void **res, const void *o, unsigned long n) { (void)h; (void)s; (void)hint; (void)o; (void)n; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketGetAddrInfo", "name resolution (cut): EAI_FAIL", int, h_c_sk_Gai, const char *h, const char *s, const void *hint, void **res) { (void)h; (void)s; (void)hint; if (res) *res = NULL; return SK_EAI_FAIL; }
HLE_FORCE("nnsocketFreeAddrInfo", "pairs with GetAddrInfo (cut)", void, h_c_sk_FreeAi, void *ai) { (void)ai; }
HLE_FORCE("nnsocketGAIStrError", "resolver error text", const char *, h_c_sk_GaiStr, int e) { (void)e; return "network unavailable"; }
