// nn::account: local profiles and open sessions; online accounts cut.
#include "hle.h"
#include "account_users.h"

typedef struct { uint64_t v[2]; } Uid;
typedef struct { uint64_t v[3]; } UserHandle;
_Static_assert(sizeof(Uid) == 16 && sizeof(UserHandle) == 24, "account ABI");
static const Uid k_uid = {{DM_UID0, DM_UID1}};
static Uid user_uid(int i) { return (Uid){{DM_UID0 + (uint64_t)i, DM_UID1}}; }
#define RES_ACC_CANCELED (124u | 1u << 9)

// Each open gets a unique token in the third handle word: a stale or foreign handle cannot close another open.
typedef struct account_session { struct account_session *next; uint64_t token; int user; } account_session;
static pthread_mutex_t g_account_lock = PTHREAD_MUTEX_INITIALIZER;
static account_session *g_account_sessions;
static uint64_t g_account_token = UINT64_C(0x5553455200000000);
static unsigned opened_users(void) {   // g_account_lock held
    unsigned mask = 0;
    for (account_session *s = g_account_sessions; s; s = s->next) mask |= 1u << s->user;
    return mask;
}
static int list_users(Uid *out, int max, unsigned mask) {
    int n = 0;
    for (int i = 0; out && i < DM_USERS && n < max; i++)
        if (mask & (1u << i)) out[n++] = user_uid(i);
    return n;
}
static void user_nickname(char *nick, int i) {
    memset(nick, 0, 33);
    if (i) snprintf(nick, 33, "Player %d", i + 1);
    else {
        const char *e = getenv("DM_NICK");
        strncpy(nick, e ? e : "Player", 32);
    }
}
static Result open_user(UserHandle *h, const Uid *uid) {
    int i = dm_account_user(uid);
    if (i < 0) return RES_FAIL;
    account_session *s = calloc(1, sizeof *s);
    if (!s) return RES_FAIL;
    pthread_mutex_lock(&g_account_lock);
    s->user = i;
    s->token = ++g_account_token;
    s->next = g_account_sessions;
    g_account_sessions = s;
    h->v[0] = uid->v[0];
    h->v[1] = uid->v[1];
    h->v[2] = s->token;
    pthread_mutex_unlock(&g_account_lock);
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account10InitializeEv", "connects to the local account service", void, h_acc_Init, void) { (void)dm_account_users(); }
HLE_FORCE("_ZN2nn7account13ListOpenUsersEPiPNS0_3UidEi", "only users with a live account session", Result, h_acc_ListOpen, int *n, Uid *out, int max) {
    pthread_mutex_lock(&g_account_lock);
    *n = list_users(out, max, opened_users());
    pthread_mutex_unlock(&g_account_lock);
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account12ListAllUsersEPiPNS0_3UidEi", "registered local users, including closed users", Result, h_acc_ListAll, int *n, Uid *out, int max) {
    unsigned mask = dm_account_users();
    *n = list_users(out, max, mask);
    return mask ? RES_OK : RES_FAIL;
}

HLE_FORCE("_ZN2nn7account22TryOpenPreselectedUserEPNS0_10UserHandleE", "open a session for the launch user (returns bool)", unsigned, h_acc_TryOpenPre, UserHandle *h) { return open_user(h, &k_uid) == RES_OK; }
HLE_FORCE("_ZN2nn7account8OpenUserEPNS0_10UserHandleERKNS0_3UidE", "open a session for a registered local user", Result, h_acc_OpenUser, UserHandle *h, const Uid *uid) { return open_user(h, uid); }
HLE_FORCE("_ZN2nn7account9GetUserIdEPNS0_3UidERKNS0_10UserHandleE", "gets the Uid from any handle, as the SDK accessor does", Result, h_acc_GetUserId, Uid *out, const UserHandle *h) {
    // The SDK copies the first 16 bytes without checking the session word.
    out->v[0] = h->v[0];
    out->v[1] = h->v[1];
    return RES_OK;
}
HLE_FORCE("_ZN2nn7account11GetNicknameEPNS0_8NicknameERKNS0_3UidE", "DM_NICK for user 1, Player N for other registered users", Result, h_acc_GetNick, char *nick, const Uid *uid) {
    int i = dm_account_user(uid);
    if (i < 0) return RES_FAIL;
    user_nickname(nick, i);
    return RES_OK;
}

// docs/reference/services.md#the-network-account-check-is-cancelled-offline
HLE_FORCE("_ZN2nn7account36EnsureNetworkServiceAccountAvailableERKNS0_10UserHandleE", "offline network account check returns applet cancellation", Result, h_acc_EnsureNsa, const UserHandle *h) { (void)h; return RES_ACC_CANCELED; }
HLE_FORCE("_ZN2nn7account26GetNetworkServiceAccountIdEPNS0_23NetworkServiceAccountIdERKNS0_10UserHandleE", "network service account, out of scope; cut", Result, h_acc_GetNsaId, void *id, const UserHandle *h) { (void)id; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account37LoadNetworkServiceAccountIdTokenCacheEPmPcmRKNS0_10UserHandleE", "network service account token, out of scope; cut", Result, h_acc_LoadTok, uint64_t *n, char *buf, uint64_t sz, const UserHandle *h) {
    (void)buf; (void)sz; (void)h;
    if (n) *n = 0;
    return RES_FAIL;
}

// docs/reference/services.md#an-account-asynccontext-needs-its-sdk-constructor
HLE_FORCE("_ZN2nn7account44EnsureNetworkServiceAccountIdTokenCacheAsyncEPNS0_12AsyncContextERKNS0_10UserHandleE", "network service account token, out of scope; cut", Result, h_acc_EnsureTokAsync, void *ctx, const UserHandle *h) { (void)ctx; (void)h; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account12AsyncContext14GetSystemEventEPNS_2os11SystemEventE", "async completion event; fires immediately", Result, h_acc_AcGetEv, void *self, void *ev) { (void)self; hle_signal_system_event(ev); return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContext9GetResultEv", "async result; networking is cut, always fails", Result, h_acc_AcGetRes, void *self) { (void)self; return RES_FAIL; }
HLE_FORCE("_ZN2nn7account42NintendoAccountAuthorizationRequestContextD1Ev", "Nintendo Account authorization, out of scope", void, h_acc_NaDtor, void *self) { (void)self; }

HLE_FORCE("_ZN2nn7account12GetUserCountEPi", "number of registered local users", Result, h_acc_UserCount, int *n) {
    unsigned mask = dm_account_users();
    *n = __builtin_popcount(mask);
    return mask ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn7account18ListQualifiedUsersEPiPNS0_3UidEi", "local users qualified for offline play", Result, h_acc_ListQual, int *n, Uid *out, int max) { return h_acc_ListAll(n, out, max); }
HLE_FORCE("_ZN2nn7account16GetUserExistenceEPbRKNS0_3UidE", "whether the UID is registered", Result, h_acc_Exists, uint8_t *out, const Uid *uid) { *out = dm_account_user(uid) >= 0; return RES_OK; }
HLE_FORCE("_ZN2nn7account9CloseUserERKNS0_10UserHandleE", "close exactly this session; other opens remain live", void, h_acc_CloseUser, const UserHandle *h) {
    if (!h) return;
    int user = dm_uid_user(h);
    pthread_mutex_lock(&g_account_lock);
    account_session **p = &g_account_sessions;
    while (*p && ((*p)->token != h->v[2] || (*p)->user != user)) p = &(*p)->next;
    if (*p) {
        account_session *s = *p;
        *p = s->next;
        free(s);
    }
    pthread_mutex_unlock(&g_account_lock);
}
HLE_FORCE("_ZN2nn7account12AsyncContext6CancelEv", "async context cancel; no request", Result, h_acc_AcCancel, void *self) { (void)self; return RES_OK; }
HLE_FORCE("_ZN2nn7account12AsyncContextD1Ev", "async context destructor; no session to close", void, h_acc_AcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn7account12AsyncContextaSEOS1_", "async context move assignment; the object holds no host state", void *, h_acc_AcMove, void *self, void *other) { (void)other; return self; }

void dm_account_reset(void) {
    pthread_mutex_lock(&g_account_lock);
    for (account_session *s = g_account_sessions, *n; s; s = n) { n = s->next; free(s); }
    g_account_sessions = NULL;
    pthread_mutex_unlock(&g_account_lock);
}
