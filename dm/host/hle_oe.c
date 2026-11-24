// nn::oe, answered directly. DM_OPMODE=0 handheld / 1 console (default: the profile's op_mode); DM_LANG=language code (default en-US).
#include "hle.h"
#include "front.h"

// DM_OPMODE is read once; DM_OE_SCRIPT and the front end change the mode
#include <stdatomic.h>
#include <unistd.h>
static _Atomic int g_opmode = -1;
int dm_opmode(void) {
    int m = g_opmode;
    if (m < 0) {
        const char *e = getenv("DM_OPMODE");
        int v = e ? atoi(e) : DM_GAME_OPMODE;
        int exp = -1;
        atomic_compare_exchange_strong(&g_opmode, &exp, v);
        m = g_opmode;
    }
    return m;
}
enum { MSG_FOCUS = 15, MSG_OPMODE = 30, MSG_PERFMODE = 31 };
#define MQ 32
static pthread_mutex_t g_mq_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_mq_cond = PTHREAD_COND_INITIALIZER;
static uint32_t g_mq[MQ];
static int g_mqh, g_mqn;
static _Atomic int g_ntf_opmode, g_ntf_perfmode;          // pushed only once the game enables them
static void mq_push(uint32_t m) {
    pthread_mutex_lock(&g_mq_lock);
    // 15 / 30 / 31 mean "query again": one still queued stands for a new one. Never merge an event of its own (Resume, Exit).
    if (m == MSG_FOCUS || m == MSG_OPMODE || m == MSG_PERFMODE)
        for (int i = 0; i < g_mqn; i++)
            if (g_mq[(g_mqh + i) % MQ] == m) {
                pthread_mutex_unlock(&g_mq_lock);
                return;
            }
    if (g_mqn == MQ) die("oe notification queue overflow (is the game not popping?)");
    g_mq[(g_mqh + g_mqn++) % MQ] = m;
    pthread_cond_broadcast(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_lock);
}
void dm_set_opmode(int m) {
    int old = dm_opmode();
    if (old == m) return;
    g_opmode = m;
    LOG("oe operation mode %d → %d", old, m);
    if (g_ntf_opmode) mq_push(MSG_OPMODE);
    if (g_ntf_perfmode) mq_push(MSG_PERFMODE);
}
// FocusState: 1 InFocus, 2 OutOfFocus, 3 Background
static _Atomic int g_focus = 1;
static _Atomic uint64_t g_focus_in_ns;
// -1: none. Only modes 1 and 2 get message 15 (docs/reference/services.md#focus-handling-mode-decides-whether-focus-changes-are-notified)
static _Atomic int g_focus_mode = -1;
static int focus_notifies(void) {
    int m = atomic_load(&g_focus_mode);
    return m != 0 && m != 3;
}
// Open library applets take the application's focus: the game sees 2 while one is open (docs/reference/services.md#library-applets-take-the-applications-focus)
static int g_applets;
static _Atomic int g_seen = 1;
static pthread_mutex_t g_seen_lock = PTHREAD_MUTEX_INITIALIZER;
static void focus_sync(int applets) {
    pthread_mutex_lock(&g_seen_lock);
    g_applets += applets;
    if (g_applets < 0) die("dm_applet_leave without dm_applet_enter");
    int w = atomic_load(&g_focus), s = w == 1 && g_applets ? 2 : w, changed = atomic_exchange(&g_seen, s) != s;
    pthread_mutex_unlock(&g_seen_lock);
    if (!changed) return;   // outside the lock: pushes need no order
    LOG("oe focus state → %d%s", s, applets > 0 ? " (library applet open)" : applets < 0 ? " (library applet closed)" : "");
    if (focus_notifies()) mq_push(MSG_FOCUS);
}
void dm_applet_enter(const char *name) {
    LOG("oe library applet: %s", name);
    focus_sync(1);
}
void dm_applet_leave(void) { focus_sync(-1); }
int dm_focus_now(void) { return atomic_load(&g_focus); }
uint64_t dm_focus_in_since(void) { return atomic_load(&g_focus_in_ns); }
void dm_focus_set(int st) {
    if (st < 1 || st > 3) die("dm_focus_set: invalid state %d", st);
    if (st == 1) atomic_store(&g_focus_in_ns, now_ns());
    if (atomic_exchange(&g_focus, st) == st) return;
    focus_sync(0);
}

HLE_FORCE("_ZN2nn2oe17FinishStartupLogoEv", "dismiss the system boot screen (through the am application proxy); the host has no boot screen", void, h_oe_FinishLogo, void) {}
HLE_FORCE("_ZN2nn2oe38SetUserInactivityDetectionTimeExtendedEb", "inactivity extension mode 1 or 0; the host never turns the screen off", void, h_oe_InactivityExtSafe, unsigned on) { (void)on; }
// the SDK's getter returns 0 without am, and OpenAudioRenderer aborts on 0 (docs/reference/services.md#applet-resource-user-id)
HLE_FORCE("_ZN2nn6applet23GetAppletResourceUserIdEv", "applet resource user ID; the host returns a fixed non-zero value", uint64_t, h_applet_Aruid, void) { return DM_APPLET_RESOURCE_USER_ID; }

// OperationMode: 0 Handheld, 1 Console; PerformanceMode: 0 Normal, 1 Boost
HLE_FORCE("_ZN2nn2oe16GetOperationModeEv", "operation mode query; a fixed value from DM_OPMODE", int, h_oe_GetOpMode, void) { return dm_opmode(); }
// a combination a Switch 1 never reports
HLE_FORCE("_ZN2nn2oe18GetPerformanceModeEv", "performance mode query; same as the operation mode (profile perf_mode boost: Boost)", int, h_oe_GetPerfMode, void) { return DM_GAME_PERF_BOOST ? 1 : dm_opmode(); }
// The scale tier is GPU budget, not the operation mode: 0 in both (docs/reference/services.md#the-graphics-scale-does-not-follow-the-operation-mode); DM_GFX_SCALE sets one
static int gfx_scale(void) {
    const char *e = getenv("DM_GFX_SCALE");
    int v = e ? atoi(e) : 0;
    return v >= 0 && v <= 2 ? v : 0;
}
HLE_FORCE("_ZN2nn2oe23GetCurrentGraphicsScaleEv", "graphics scale (Switch 2, through am): 0, the whole GPU (DM_GFX_SCALE sets 1 or 2)", int, h_oe_GfxScale, void) { return gfx_scale(); }
HLE_FORCE("_ZN2nn2oe42SetGraphicsScaleChangedNotificationEnabledEb", "graphics scale change notification (Switch 2, through am); the host's scale never changes",
          void, h_oe_GfxScaleNotify, unsigned on) { (void)on; }
// The SDK's sends the mode to the gs server the host lacks; the host paces by the present interval (docs/reference/services.md#the-target-frame-rate-graphics-frame-length)
HLE_FORCE("_ZN2nn2gs3sdk26SetGraphicsFrameLengthModeENS1_23GraphicsFrameLengthModeE", "target frame length (Switch 2, gs): noted; the host paces by the present interval",
          void, h_gs_FrameLength, int mode) {
    static int last = -1;
    if (mode != last) {
        LOG("gs: graphics frame length mode %d (0 auto, 1 30 fps, 2 60 fps, 3 120 fps)", mode);
        last = mode;
    }
}
HLE_FORCE("_ZN2nn2oe24IsInStoreDemoModeEnabledEv", "retail store demo mode (through am); never", unsigned, h_oe_StoreDemo, void) { return 0; }

// DM_OE_SCRIPT="focus=3@10,focus=1@20,opmode=0@30" (seconds from the first notification fetch); bg=1 / bg=0: the background round trip
static void script_bg(int on) {
    if (on) {
        dm_focus_set(2);
        dm_focus_set(3);
        dm_audio_suspend(1);
        guest_clock_freeze_for(1, 1);
        dm_gfx_suspend(1);
    } else {
        dm_gfx_suspend(0);
        guest_clock_freeze_for(1, 0);
        dm_audio_suspend(0);
        dm_focus_set(2);
        dm_focus_set(1);
    }
}
static void *oe_script(void *arg) {
    char *buf = arg;
    uint64_t t0 = now_ns();
    for (char *save, *tok = strtok_r(buf, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        char key[16];
        int v;
        double at;
        if (sscanf(tok, "%15[a-z]=%d@%lf", key, &v, &at) != 3) die("DM_OE_SCRIPT entry '%s' should be key=value@seconds", tok);
        uint64_t due = t0 + (uint64_t)(at * 1e9), now = now_ns();
        if (due > now) usleep((useconds_t)((due - now) / 1000));
        if (!strcmp(key, "focus")) dm_focus_set(v);
        else if (!strcmp(key, "opmode")) dm_set_opmode(v);
        else if (!strcmp(key, "bg")) script_bg(v);
        else die("DM_OE_SCRIPT unknown key '%s' (focus / opmode / bg)", key);
    }
    return NULL;
}
static void oe_script_start(void) {
    const char *e = getenv("DM_OE_SCRIPT");
    if (!e) return;
    pthread_t th;
    if (pthread_create(&th, NULL, oe_script, strdup(e))) die("DM_OE_SCRIPT thread");
    pthread_detach(th);
}
static pthread_once_t g_oe_once = PTHREAD_ONCE_INIT;

HLE_FORCE("_ZN2nn2oe20GetCurrentFocusStateEv", "foreground / background state; the front end's dm_focus_set, out of focus while a library applet is open", int, h_oe_GetFocus, void) { return atomic_load(&g_seen); }
HLE_FORCE("_ZN2nn2oe20SetFocusHandlingModeENS0_17FocusHandlingModeE", "focus handling mode; 1 and 2 get focus notifications, 0 and 3 none (see g_focus_mode)", void, h_oe_SetFocusMode, int m) {
    atomic_store(&g_focus_mode, m);
    LOG("oe::SetFocusHandlingMode %d (%s)", m, focus_notifies() ? "focus notifications" : "suspended out of focus, no focus notifications");
}

// returns bool
HLE_FORCE("_ZN2nn2oe25TryPopNotificationMessageEPj", "system notification queue; front end state changes are queued", unsigned, h_oe_TryPopMsg, uint32_t *msg) {
    pthread_once(&g_oe_once, oe_script_start);
    pthread_mutex_lock(&g_mq_lock);
    int ok = g_mqn > 0;
    if (ok) {
        *msg = g_mq[g_mqh];
        g_mqh = (g_mqh + 1) % MQ;
        g_mqn--;
    }
    pthread_mutex_unlock(&g_mq_lock);
    if (ok) LOG("oe notification popped by the game: %u", *msg);
    return ok;
}
// The SDK's waits on oe::Initialize's system event, which the host never makes: wait on the host queue
HLE_FORCE("_ZN2nn2oe22PopNotificationMessageEv", "system notification queue (blocking); waits on the host queue", uint32_t, h_oe_PopMsg, void) {
    pthread_once(&g_oe_once, oe_script_start);
    uint32_t m = 0;
    for (;;) {
        pthread_mutex_lock(&g_mq_lock);
        while (!g_mqn && !atomic_load(&dm_reset_stop)) {
            dm_thread_native_wait(1);
            pthread_cond_wait(&g_mq_cond, &g_mq_lock);
            dm_thread_native_wait(0);
        }
        pthread_mutex_unlock(&g_mq_lock);
        if (atomic_load(&dm_reset_stop)) return 0;   // reset: the caller's next gate ends its thread
        if (h_oe_TryPopMsg(&m)) return m;   // taken meanwhile: wait again
    }
}
void dm_oe_reset_wake(void) {
    pthread_mutex_lock(&g_mq_lock);
    pthread_cond_broadcast(&g_mq_cond);
    pthread_mutex_unlock(&g_mq_lock);
}
// bool: only the low 8 bits count
HLE_FORCE("_ZN2nn2oe28SetResumeNotificationEnabledEb", "resume notification switch; the host expresses resume as focus notification 15", void, h_oe_SetResumeNtf, unsigned on) { (void)on; }
HLE_FORCE("_ZN2nn2oe42SetOperationModeChangedNotificationEnabledEb", "operation mode change notification switch", void, h_oe_SetOpModeNtf, unsigned on) { g_ntf_opmode = on & 0xff; }
HLE_FORCE("_ZN2nn2oe44SetPerformanceModeChangedNotificationEnabledEb", "performance mode change notification switch", void, h_oe_SetPerfModeNtf, unsigned on) { g_ntf_perfmode = on & 0xff; }

// LanguageCode { char string[8]; }, returned in x0
HLE_FORCE("_ZN2nn2oe18GetDesiredLanguageEv", "system language query; value from DM_LANG", uint64_t, h_oe_GetLang, void) {
    const char *e = getenv("DM_LANG");
    char code[8] = {0};
    strncpy(code, e && *e ? e : "en-US", sizeof code - 1);
    uint64_t v;
    memcpy(&v, code, 8);
    return v;
}

// Reset drops the game's notifications, switches, focus mode and applets; the front end's focus and operation mode stay
void dm_oe_reset(void) {
    pthread_mutex_lock(&g_mq_lock);
    g_mqh = g_mqn = 0;
    pthread_mutex_unlock(&g_mq_lock);
    atomic_store(&g_ntf_opmode, 0);
    atomic_store(&g_ntf_perfmode, 0);
    atomic_store(&g_focus_mode, -1);
    pthread_mutex_lock(&g_seen_lock);
    g_applets = 0;
    atomic_store(&g_seen, atomic_load(&g_focus));
    pthread_mutex_unlock(&g_seen_lock);
}
