// System service takeovers: oe, time, pl (shared fonts), nfp (amiibo), vi (vsync event), diag.
#include "hle.h"
#include <fcntl.h>
#include <pthread.h>
#include <pthread/qos.h>
#include <stdatomic.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

HLE_FORCE("_ZN2nn2oe15SetCpuBoostModeENS0_12CpuBoostModeE", "CPU boost mode; the host does not scale clocks", void, h_oe_CpuBoost, int m) { (void)m; }
// From the NACP of the build named by main's build ID (the profile's build rows); 1.0.0 for others
HLE_FORCE("_ZN2nn2oe17GetDisplayVersionEPNS0_14DisplayVersionE", "display version (by the main build ID)", void, h_oe_DispVer, char *v) {
    const struct { const char *build, *ver; } k[] = {DM_GAME_DISPLAY_VERSIONS};
    const char *ver = "1.0.0";
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main"))
        for (unsigned j = 0; j < sizeof k / sizeof *k; j++) if (k[j].build && !strcmp(g_mod[i].build_id, k[j].build)) ver = k[j].ver;
    memset(v, 0, 16); strlcpy(v, ver, 16);
}
HLE_FORCE("_ZN2nn2oe25GetProgramTotalActiveTimeEv", "total active time → time since the host started", int64_t, h_oe_ActiveTime, void) { return (int64_t)(now_ns() - g_t0_ns); }

// CalendarTime returned by value in x0
HLE_FORCE("_ZN2nn4time19ToCalendarTimeInUtcERKNS0_9PosixTimeE", "absolute seconds → UTC calendar (host gmtime_r)", uint64_t, h_time_ToCalUtc, const int64_t *t) {
    time_t v = (time_t)*t; struct tm tm; gmtime_r(&v, &tm);
    return (uint64_t)(uint16_t)(tm.tm_year + 1900) | (uint64_t)(uint8_t)(tm.tm_mon + 1) << 16 | (uint64_t)(uint8_t)tm.tm_mday << 24 |
           (uint64_t)(uint8_t)tm.tm_hour << 32 | (uint64_t)(uint8_t)tm.tm_min << 40 | (uint64_t)(uint8_t)tm.tm_sec << 48;
}
HLE_FORCE("_ZN2nn4time8FinalizeEv", "pairs with Initialize; the host has no session", Result, h_time_Finalize, void) { return RES_OK; }
// No request is sent: the event fires at once and the result is failure. The event is host static storage.
static _Alignas(16) uint8_t g_ev_netclock[0x100];
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskC1Ev", "network clock task constructor; the host opens no session", void, h_time_NcCtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTaskD1Ev", "network clock task destructor; no session to close", void, h_time_NcDtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask10InitializeENS_2os14EventClearModeE", "network clock task init (offline)", Result, h_time_NcInit, void *self, int mode) { (void)self; (void)mode; return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9StartTaskEv", "network clock task start: finishes at once with failure", Result, h_time_NcStart, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return RES_OK; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask14GetFinishEventEv", "network clock task finish event: already signalled", void *, h_time_NcEvent, void *self) { (void)self; hle_signal_system_event(g_ev_netclock); return g_ev_netclock; }
HLE_FORCE("_ZN2nn4time39AsyncEnsureNetworkClockAvailabilityTask9GetResultEv", "network clock task result: offline, failure", Result, h_time_NcResult, void *self) { (void)self; return RES_FAIL; }

// Stand-in shared fonts, one per type in $DM_FONT_DIR (dm/tools/make_shared_fonts.py), cut to fit the game's font engine memory
// (docs/reference/services.md#shared-font-stand-ins-sized-for-the-games-font-engines); a type with no file takes the standard font.
// BFTTF = 8-byte header + TTF: Decode leaves the data alone when the first word is the decoded marker 0x18029a7f; the second is bswap(length ^ 0x49621806).
typedef struct { uint8_t *p; size_t n; } font_t;
static font_t font_map(const char *p) {
    font_t f = {0}; int fd = open(p, O_RDONLY); struct stat st;
    if (fd < 0 || fstat(fd, &st)) { if (fd >= 0) close(fd); return f; }
    size_t n = (size_t)st.st_size;
    uint8_t *m = mmap(NULL, n + 8, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m == MAP_FAILED) {
        LOG("[RED] shared font memory allocation failed (%zu bytes)", n + 8);
        close(fd);
        return f;
    }
    size_t got = 0; while (got < n) { ssize_t r = pread(fd, m + 8 + got, n - got, (off_t)got); if (r <= 0) break; got += (size_t)r; }
    close(fd);
    if (got != n) {
        LOG("[RED] shared font read short: %s (%zu / %zu bytes)", p, got, n);
        munmap(m, n + 8);
        return f;
    }
    uint32_t done = 0x18029a7fu, len = __builtin_bswap32((uint32_t)n ^ 0x49621806u);
    memcpy(m, &done, 4); memcpy(m + 4, &len, 4);
    f.p = m; f.n = n + 8; return f;
}
static font_t font_open(const char *path, const char *file) {   // path (an override), else $DM_FONT_DIR/file
    char buf[1024]; const char *dir = getenv("DM_FONT_DIR");
    if ((!path || !*path) && file && dir && *dir) { snprintf(buf, sizeof buf, "%s/%s", dir, file); path = buf; }
    if (!path || !*path) return (font_t){0};
    font_t f = font_map(path);
    if (f.p) LOG("shared font (%s): %s (%zu bytes, BFTTF 'decrypted' header + TTF as is)", file ? file : "fallback", path, f.n - 8);
    return f;
}
static pthread_mutex_t font_mu = PTHREAD_MUTEX_INITIALIZER;
#define FONT_TYPES 6
static const char *const font_file[FONT_TYPES] = {"standard.ttf", "chinese_simplified.ttf", "chinese_simplified_ext.ttf", "chinese_traditional.ttf", "korean.ttf", "nintendo_ext.ttf"};
static font_t font_by_type[FONT_TYPES];   // its own file, or the standard font's mapping
static font_t font_open_type(int type) {   // font_mu held; empty: the standard font
    if (type == 5) {
        font_t f = font_open(getenv("DM_SHARED_FONT_EXT"), font_file[5]);
        if (!f.p) LOG("[YELLOW] Nintendo extended shared font missing (dm/tools/make_shared_fonts.py makes nintendo_ext.ttf): button icons will be drawn as '?'");
        return f;
    }
    const char *one = getenv("DM_SHARED_FONT");   // one font for types 0-4
    if (type) return one && *one ? (font_t){0} : font_open(NULL, font_file[type]);
    font_t f = font_open(one, font_file[0]);
    if (!f.p && (f = font_open("/System/Library/Fonts/Supplemental/Arial Unicode.ttf", NULL)).p)
        LOG("[YELLOW] no standard.ttf in DM_FONT_DIR: the shared fonts are macOS's Arial Unicode as it is, which some games' font engines cannot load (dm/tools/make_shared_fonts.py makes the set)");
    if (!f.p) LOG("[RED] cannot open the standard shared font (DM_FONT_DIR with dm/tools/make_shared_fonts.py's fonts, or DM_SHARED_FONT naming a TTF)");
    return f;
}
static font_t font_get(int type) {
    static int done[FONT_TYPES];
    if (type < 0 || type >= FONT_TYPES) type = 0;
    pthread_mutex_lock(&font_mu);
    if (!done[0]) { done[0] = 1; font_by_type[0] = font_open_type(0); }
    if (!done[type]) {
        done[type] = 1;
        font_t f = font_open_type(type);
        font_by_type[type] = f.p ? f : font_by_type[0];
    }
    font_t f = font_by_type[type];
    pthread_mutex_unlock(&font_mu);
    return f;
}
// docs/reference/services.md#shared-font-address-bfttf-header-or-bare-ttf
static size_t font_skip(void) {
    static int skip = -1;
    if (skip < 0) {
        int dec = dm_game_imports("_ZN2nn6fontll24ScalableFontEngineHelper6Decode");
        skip = dec ? 0 : 8;
        LOG("shared font: %s (main %s ScalableFontEngineHelper::Decode)", skip ? "the TTF itself" : "BFTTF header + TTF", dec ? "imports" : "does not import");
    }
    return (size_t)skip;
}
HLE_FORCE("_ZN2nn2pl22GetSharedFontLoadStateENS0_14SharedFontTypeE", "shared font load state: loaded (1)", int, h_pl_LoadState, int type) { return font_get(type).p ? 1 : 0; }
HLE_FORCE("_ZN2nn2pl20GetSharedFontAddressENS0_14SharedFontTypeE", "shared font address → BFTTF header, or the TTF itself for a main that imports no Decode", const void *, h_pl_Addr, int type) { const uint8_t *p = font_get(type).p; return p ? p + font_skip() : NULL; }
// pl is not served and any failure aborts; the stand-ins live in host memory, so the buffer stays as it is.
HLE_FORCE("_ZN2nn2pl17RequestLoadNxFontEPvm", "load the shared fonts into the game's buffer; the stand-in fonts need none of it", void, h_pl_ReqNxFont, void *buf, size_t size) { (void)buf; (void)size; }
HLE_FORCE("_ZN2nn2pl17GetSharedFontSizeENS0_14SharedFontTypeE", "shared font size → 8-byte header + stand-in font file size, or without the header as the address", size_t, h_pl_Size, int type) { font_t f = font_get(type); return f.p ? f.n - font_skip() : 0; }
// Unhooked, the SDK aborts on pl:u. Types 0-5 are the shared font types; the address has GetSharedFontAddress's form.
typedef struct { uint8_t type, flag, pad[6]; const void *address; uint64_t size; } pl_builtin_font_t;
static unsigned pl_builtin_font(pl_builtin_font_t *out, int type) {
    font_t f = font_get(type); if (!f.p) return 0;
    *out = (pl_builtin_font_t){ .type = (uint8_t)type, .address = f.p + font_skip(), .size = f.n - font_skip() };
    return 1;
}
HLE_FORCE("_ZN2nn2pl18GetBuiltInFontDataEPNS0_20BuiltInFontDataOunceENS0_20BuiltInFontTypeOunceEb", "one built-in font (Switch 2 type): the stand-in TTFs", unsigned, h_pl_BuiltInOunce, pl_builtin_font_t *out, int type, unsigned b) { (void)b; return pl_builtin_font(out, type); }
// At most 6 types, filled in order as above; a type with no stand-in is left out of the count.
HLE_FORCE("_ZN2nn2pl21GetBuiltInFontDataSetEPiPNS0_20BuiltInFontDataOunceEPKNS0_20BuiltInFontTypeOunceEiNS_8settings12LanguageCodeEb", "built-in fonts by a list of types (Switch 2 types, at most 6): the stand-in TTFs", unsigned, h_pl_BuiltInSetOunce, int *count, pl_builtin_font_t *out, const uint8_t *types, int n, uint64_t lang, unsigned b) {
    (void)lang; (void)b; int k = 0;
    for (int i = 0; i < n && i < 6; i++) k += (int)pl_builtin_font(&out[k], types[i]);
    *count = k; return k > 0;
}

// nfp: the SDK would abort without sm. One NFC device on the connected controller; with no amiibo scanned, detection finds no tag.
// A virtual amiibo is a raw NTAG215 dump (UID and model block plain); for its encrypted owner and area: the default Mii and <DM_ROOT>/amiibo/<UID>.area.
// Open: reading a dump's own application area (docs/reference/services.md).
#define NFP_RES(d) (115u | (uint32_t)(d) << 9)
#define NFP_RES_TAG_NOT_FOUND NFP_RES(97)
#define NFP_RES_WRONG_AREA_SIZE NFP_RES(68)
#define NFP_RES_WRONG_STATE NFP_RES(73)
#define NFP_RES_AREA_NOT_CREATED NFP_RES(128)
#define NFP_RES_WRONG_AREA_ID NFP_RES(152)
#define NFP_RES_AREA_EXISTS NFP_RES(168)
enum { NFP_STATE_INIT = 0, NFP_STATE_SEARCH = 1, NFP_STATE_FOUND = 2, NFP_STATE_REMOVED = 3, NFP_STATE_MOUNTED = 4 };
#define NFP_AREA_SIZE 0xD8
typedef struct { uint32_t id; uint8_t reserved[4]; } NfpDeviceHandle;
#define NFP_DEVICE_ID 0x4e465030u   // 'NFP0'
static pthread_mutex_t g_nfp_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_nfp_state;
static struct {
    int present; char name[128]; uint8_t dump[0x60];
    int area_made, area_open, area_dirty; uint32_t area_id; uint8_t area[NFP_AREA_SIZE];
} g_tag;
uint32_t hle_hid_connected_npad(void);
void dm_mii_default_charinfo(void *out);

static void nfp_uid(uint8_t uid[7]) { memcpy(uid, g_tag.dump, 3); memcpy(uid + 3, g_tag.dump + 4, 4); }
static int nfp_area_path(char *out, size_t n) {
    const char *root = getenv("DM_ROOT");
    if (!root || !*root) return 0;
    uint8_t u[7]; nfp_uid(u);
    snprintf(out, n, "%s/amiibo", root); mkdir(out, 0755);
    snprintf(out, n, "%s/amiibo/%02X%02X%02X%02X%02X%02X%02X.area", root, u[0], u[1], u[2], u[3], u[4], u[5], u[6]);
    return 1;
}
static void nfp_area_load(void) {
    g_tag.area_made = g_tag.area_open = g_tag.area_dirty = 0; memset(g_tag.area, 0, sizeof g_tag.area);
    char p[1100]; uint8_t raw[4 + NFP_AREA_SIZE];
    if (!nfp_area_path(p, sizeof p)) return;
    FILE *f = fopen(p, "rb");
    if (!f) return;
    if (fread(raw, 1, sizeof raw, f) == sizeof raw) {
        memcpy(&g_tag.area_id, raw, 4);
        memcpy(g_tag.area, raw + 4, NFP_AREA_SIZE);
        g_tag.area_made = 1;
    }
    fclose(f);
}
static void nfp_area_save(void) {
    char p[1100], tmp[1110]; uint8_t raw[4 + NFP_AREA_SIZE];
    if (!g_tag.area_dirty) return;
    g_tag.area_dirty = 0;
    if (!nfp_area_path(p, sizeof p)) { LOG("[RED] nfp: DM_ROOT is not set: the amiibo's application area is kept for this run only"); return; }
    memcpy(raw, &g_tag.area_id, 4); memcpy(raw + 4, g_tag.area, NFP_AREA_SIZE);
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    FILE *f = fopen(tmp, "wb");
    if (!f || fwrite(raw, 1, sizeof raw, f) != sizeof raw || fclose(f) || rename(tmp, p)) LOG("[RED] nfp: cannot write %s: %s", p, strerror(errno));
}
static void nfp_found(void) {
    g_nfp_state = NFP_STATE_FOUND;
    LOG("nfp: amiibo %s found by the game", g_tag.name);
}

int dm_amiibo_scan(const char *path) {
    uint8_t raw[0x60]; FILE *f = fopen(path, "rb");
    size_t n = f ? fread(raw, 1, sizeof raw, f) : 0; long size = 0;
    if (f) { fseek(f, 0, SEEK_END); size = ftell(f); fclose(f); }
    if (n != sizeof raw || size < 520 || size > 4096 || raw[0x0C] != 0xF1 || raw[0x0D] != 0x10 || raw[0x0E] != 0xFF || raw[0x0F] != 0xEE || raw[0x5B] != 0x02) {
        LOG("[RED] nfp: %s is not an amiibo dump (a raw NTAG215 image of 520 to 572 bytes)", path); return 0;
    }
    pthread_mutex_lock(&g_nfp_mu);
    memcpy(g_tag.dump, raw, sizeof raw); g_tag.present = 1;
    const char *base = strrchr(path, '/'); snprintf(g_tag.name, sizeof g_tag.name, "%s", base ? base + 1 : path);
    char *dot = strrchr(g_tag.name, '.'); if (dot) *dot = 0;
    nfp_area_load();
    LOG("nfp: amiibo %s on the reader (model %02x%02x%02x%02x %02x%02x%02x%02x%s)", g_tag.name, raw[0x54], raw[0x55], raw[0x56], raw[0x57], raw[0x58], raw[0x59], raw[0x5A], raw[0x5B],
        g_tag.area_made ? ", with a saved application area" : "");
    if (g_nfp_state != NFP_STATE_INIT) nfp_found();
    pthread_mutex_unlock(&g_nfp_mu);
    return 1;
}
void dm_amiibo_remove(void) {
    pthread_mutex_lock(&g_nfp_mu);
    if (g_tag.present) {
        nfp_area_save(); g_tag.present = 0;
        LOG("nfp: amiibo %s taken off the reader", g_tag.name);
        if (g_nfp_state == NFP_STATE_FOUND || g_nfp_state == NFP_STATE_MOUNTED) g_nfp_state = NFP_STATE_REMOVED;
    }
    pthread_mutex_unlock(&g_nfp_mu);
}
const char *dm_amiibo_name(void) { return g_tag.present ? g_tag.name : NULL; }

static Result nfp_no_tag(const char *what) {
    static _Atomic int logged;
    if (!atomic_exchange(&logged, 1)) LOG("nfp: %s and other operations that need an amiibo tag → no tag found (none is on the reader)", what);
    return NFP_RES_TAG_NOT_FOUND;
}
// 0 with the lock held when the tag is there; else the result, the lock released
static Result nfp_need(const char *what, int mounted) {
    pthread_mutex_lock(&g_nfp_mu);
    int have = g_nfp_state == NFP_STATE_FOUND || g_nfp_state == NFP_STATE_MOUNTED;
    if (have && (!mounted || g_nfp_state == NFP_STATE_MOUNTED)) return RES_OK;
    pthread_mutex_unlock(&g_nfp_mu);
    return have ? NFP_RES_WRONG_STATE : nfp_no_tag(what);
}
static void nfp_today(uint8_t *out) {
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm);
    uint16_t y = (uint16_t)(tm.tm_year + 1900);
    memcpy(out, &y, 2);
    out[2] = (uint8_t)(tm.tm_mon + 1);
    out[3] = (uint8_t)tm.tm_mday;
}
static void nfp_register_info(uint8_t *out) {
    // 10 characters at most
    memset(out, 0, 0x100);
    dm_mii_default_charinfo(out);
    nfp_today(out + 0x58);
    strcpy((char *)out + 0x5C, "NativeNavi");
}

HLE_FORCE("_ZN2nn3nfp10InitializeEv", "amiibo library init; the host has one NFC device, which finds the amiibo file the player scans", void, h_nfp_Init, void) {
    LOG("nfp::Initialize: one NFC device, which finds a scanned amiibo file");
    static _Atomic int env;
    const char *e = getenv("DM_AMIIBO");
    if (e && *e && !atomic_exchange(&env, 1)) dm_amiibo_scan(e);
}
HLE_FORCE("_ZN2nn3nfp29AttachAvailabilityChangeEventEPNS_2os15SystemEventTypeE", "NFC availability change event; bound to a kernel event, never signalled", void, h_nfp_AttachAvail, void *ev) { hle_bind_system_event(ev, 0); }
HLE_FORCE("_ZN2nn3nfp11ListDevicesEPNS0_12DeviceHandleEPii", "NFC device list: one device", Result, h_nfp_List, NfpDeviceHandle *out, int *n, int max) {
    *n = 0; if (max >= 1) { memset(&out[0], 0, sizeof out[0]); out[0].id = NFP_DEVICE_ID; *n = 1; }
    return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp9GetNpadIdEPjRKNS0_12DeviceHandleE", "controller the NFC device is on: the one connected now", Result, h_nfp_NpadId, uint32_t *out, const NfpDeviceHandle *h) { (void)h; *out = hle_hid_connected_npad(); return RES_OK; }
HLE_FORCE("_ZN2nn3nfp14StartDetectionERKNS0_12DeviceHandleE", "start tag detection: finds the scanned amiibo, if one is on the reader", Result, h_nfp_Start, const NfpDeviceHandle *h) { (void)h;
    pthread_mutex_lock(&g_nfp_mu);
    g_nfp_state = NFP_STATE_SEARCH; if (g_tag.present) nfp_found();
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
// The next detection needs a new scan, as a player lifts the figure off.
HLE_FORCE("_ZN2nn3nfp13StopDetectionERKNS0_12DeviceHandleE", "stop tag detection", Result, h_nfp_Stop, const NfpDeviceHandle *h) { (void)h;
    pthread_mutex_lock(&g_nfp_mu);
    if (g_nfp_state >= NFP_STATE_FOUND && g_tag.present) { nfp_area_save(); g_tag.present = 0; }
    g_nfp_state = NFP_STATE_INIT;
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp14GetDeviceStateERKNS0_12DeviceHandleE", "device state: initialized / searching / tag found / removed / mounted", int, h_nfp_State, const NfpDeviceHandle *h) { (void)h;
    pthread_mutex_lock(&g_nfp_mu); int s = g_nfp_state; pthread_mutex_unlock(&g_nfp_mu); return s;
}
HLE_FORCE("_ZN2nn3nfp10GetTagInfoEPNS0_7TagInfoERKNS0_12DeviceHandleE", "tag info: the dump's UID, an NFC-A type 2 tag", Result, h_nfp_TagInfo, uint8_t *out, const NfpDeviceHandle *h) { (void)h;
    Result r = nfp_need("GetTagInfo", 0); if (r) return r;
    memset(out, 0, 0x58);
    nfp_uid(out);
    out[0x0A] = 7;
    uint32_t protocol = 1, type = 2;
    memcpy(out + 0x20, &protocol, 4);
    memcpy(out + 0x24, &type, 4);
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
// character ID (as on the tag), variant, figure type, model number (big-endian), series
HLE_FORCE("_ZN2nn3nfp12GetModelInfoEPNS0_9ModelInfoERKNS0_12DeviceHandleE", "model info: the dump's model block", Result, h_nfp_ModelInfo, uint8_t *out, const NfpDeviceHandle *h) { (void)h;
    Result r = nfp_need("GetModelInfo", 1); if (r) return r;
    const uint8_t *m = g_tag.dump + 0x54;
    memset(out, 0, 0x40); memcpy(out, m, 4); out[4] = m[5]; out[5] = m[4]; out[6] = m[6];
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp5MountERKNS0_12DeviceHandleENS0_9ModelTypeENS0_11MountTargetE", "mount the scanned amiibo", Result, h_nfp_Mount, const NfpDeviceHandle *h, int model, int target) { (void)h; (void)model; (void)target;
    Result r = nfp_need("Mount", 0); if (r) return r;
    g_nfp_state = NFP_STATE_MOUNTED; g_tag.area_open = 0;
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
// last write date (encrypted on the tag: today), write counter (big-endian at 0x11), version, area size
HLE_FORCE("_ZN2nn3nfp13GetCommonInfoEPNS0_10CommonInfoERKNS0_12DeviceHandleE", "common info: the dump's write counter, today's date", Result, h_nfp_CommonInfo, uint8_t *out, const NfpDeviceHandle *h) { (void)h;
    Result r = nfp_need("GetCommonInfo", 1); if (r) return r;
    memset(out, 0, 0x40);
    nfp_today(out);
    out[4] = g_tag.dump[0x12];
    out[5] = g_tag.dump[0x11];
    uint32_t size = NFP_AREA_SIZE;
    memcpy(out + 8, &size, 4);
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp15GetRegisterInfoEPNS0_12RegisterInfoERKNS0_12DeviceHandleE", "register info: the default Mii as the owner (the dump's own is encrypted)", Result, h_nfp_RegInfo, uint8_t *out, const NfpDeviceHandle *h) { (void)h;
    Result r = nfp_need("GetRegisterInfo", 1); if (r) return r;
    nfp_register_info(out);
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp8FinalizeEv", "amiibo library finalize; the host has no session", void, h_nfp_Fini, void) {}
HLE_FORCE("_ZN2nn3nfp5FlushERKNS0_12DeviceHandleE", "flush: the application area goes to its file", Result, h_nfp_Flush, const NfpDeviceHandle *h) { (void)h;
    Result r = nfp_need("Flush", 1); if (r) return r;
    nfp_area_save();
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp19OpenApplicationAreaERKNS0_12DeviceHandleEj", "open the amiibo's application area, if a game made one with this access ID", Result, h_nfp_OpenArea, const NfpDeviceHandle *h, uint32_t id) { (void)h;
    Result r = nfp_need("OpenApplicationArea", 1); if (r) return r;
    r = !g_tag.area_made ? NFP_RES_AREA_NOT_CREATED : g_tag.area_id != id ? NFP_RES_WRONG_AREA_ID : RES_OK;
    g_tag.area_open = r == RES_OK;
    LOG("nfp: OpenApplicationArea(%#x) on %s → %s", id, g_tag.name, r == RES_OK ? "opened" : g_tag.area_made ? "another game's" : "none yet (a dump's own is encrypted, the host starts each amiibo empty)");
    pthread_mutex_unlock(&g_nfp_mu); return r;
}
HLE_FORCE("_ZN2nn3nfp18GetApplicationAreaEPvPmRKNS0_12DeviceHandleEm", "read the application area", Result, h_nfp_GetArea, void *buf, uint64_t *n, const NfpDeviceHandle *h, uint64_t sz) { (void)h;
    if (n) *n = 0;
    Result r = nfp_need("GetApplicationArea", 1); if (r) return r;
    if (!g_tag.area_open) { pthread_mutex_unlock(&g_nfp_mu); return NFP_RES_AREA_NOT_CREATED; }
    if (sz > NFP_AREA_SIZE) sz = NFP_AREA_SIZE;
    memcpy(buf, g_tag.area, sz); if (n) *n = sz;
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp18SetApplicationAreaERKNS0_12DeviceHandleEPKvm", "write the application area", Result, h_nfp_SetArea, const NfpDeviceHandle *h, const void *buf, uint64_t sz) { (void)h;
    Result r = nfp_need("SetApplicationArea", 1); if (r) return r;
    if (!g_tag.area_open) r = NFP_RES_AREA_NOT_CREATED;
    else if (sz > NFP_AREA_SIZE) r = NFP_RES_WRONG_AREA_SIZE;
    else { memcpy(g_tag.area, buf, sz); g_tag.area_dirty = 1; }
    pthread_mutex_unlock(&g_nfp_mu); return r;
}
// access ID, initial data at +8, its size at +0x10
static Result nfp_create_area(const uint8_t *info, int again, const char *what) {
    Result r = nfp_need(what, 1); if (r) return r;
    uint32_t id, size;
    const void *data;
    memcpy(&id, info, 4);
    memcpy(&data, info + 8, 8);
    memcpy(&size, info + 0x10, 4);
    if (g_tag.area_made && !again) r = NFP_RES_AREA_EXISTS;
    else if (size > NFP_AREA_SIZE) r = NFP_RES_WRONG_AREA_SIZE;
    else {
        memset(g_tag.area, 0, sizeof g_tag.area); if (data && size) memcpy(g_tag.area, data, size);
        g_tag.area_id = id; g_tag.area_made = g_tag.area_open = g_tag.area_dirty = 1; nfp_area_save();
        LOG("nfp: %s(%#x) on %s: %u bytes", what, id, g_tag.name, size);
    }
    pthread_mutex_unlock(&g_nfp_mu); return r;
}
HLE_FORCE("_ZN2nn3nfp21CreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "create the application area in the amiibo's file", Result, h_nfp_CreateArea, const NfpDeviceHandle *h, const uint8_t *info) { (void)h; return nfp_create_area(info, 0, "CreateApplicationArea"); }
HLE_FORCE("_ZN2nn3nfp23RecreateApplicationAreaERKNS0_12DeviceHandleERKNS0_25ApplicationAreaCreateInfoE", "replace the application area in the amiibo's file", Result, h_nfp_RecreateArea, const NfpDeviceHandle *h, const uint8_t *info) { (void)h; return nfp_create_area(info, 1, "RecreateApplicationArea"); }
static Result nfp_nothing_to_do(const char *what) {
    Result r = nfp_need(what, 0);
    if (r) return r;
    pthread_mutex_unlock(&g_nfp_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp13StartRestorerEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo restorer applet: nothing to restore", Result, h_nfp_Restorer, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_nothing_to_do("StartRestorer"); }
HLE_FORCE("_ZN2nn3nfp19StartGameDataEraserEPNS0_12DeviceHandleERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo game data eraser applet: the host has none, the data stays", Result, h_nfp_Eraser, NfpDeviceHandle *h, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_nothing_to_do("StartGameDataEraser"); }
static Result nfp_owner_applet(uint8_t *changed, uint8_t *reg) {
    if (changed) *changed = 0;
    Result r = nfp_need("StartNicknameAndOwnerSettings", 0); if (r) return r;
    if (reg) nfp_register_info(reg);
    pthread_mutex_unlock(&g_nfp_mu); return RES_OK;
}
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoE", "amiibo nickname and owner settings applet: the host's owner stays", Result, h_nfp_Nick, NfpDeviceHandle *h, uint8_t *changed, uint8_t *reg, const void *p, const void *tag) { (void)h; (void)p; (void)tag; return nfp_owner_applet(changed, reg); }
HLE_FORCE("_ZN2nn3nfp29StartNicknameAndOwnerSettingsEPNS0_12DeviceHandleEPbPNS0_12RegisterInfoERKNS0_24AmiiboSettingsStartParamERKNS0_7TagInfoERKS4_", "amiibo nickname and owner settings applet (with the old register info): the host's owner stays", Result, h_nfp_Nick2, NfpDeviceHandle *h, uint8_t *changed, uint8_t *reg, const void *p, const void *tag, const void *old) { (void)h; (void)p; (void)tag; (void)old; return nfp_owner_applet(changed, reg); }

// vi vsync event: 60 Hz (120 while 120 fps holds), on the display link's ticks while it presents (a clock of its own would drift against vsync).
// One thread per binding lifetime; a destroyed binding retires its worker even if its storage and handle are reused before the next tick.
void dm_host_helper_qos(qos_class_t q);
typedef struct vsync_event { void *ev; uint64_t binding; struct vsync_event *next; } vsync_event_t;
static pthread_mutex_t g_vs_mu = PTHREAD_MUTEX_INITIALIZER;
static vsync_event_t *g_vs_events;
static int vsync_tick(vsync_event_t *v) {   // 0: the event is gone and this thread has left the table
    if (hle_signal_system_event_binding(v->binding)) return 1;
    pthread_mutex_lock(&g_vs_mu);
    vsync_event_t **p = &g_vs_events; while (*p && *p != v) p = &(*p)->next;
    if (*p) *p = v->next;
    pthread_mutex_unlock(&g_vs_mu);
    LOG("vi: vsync event %p destroyed by the game, its thread ends", v->ev);
    return 0;
}
__attribute__((weak)) int mtl_link_wait_vsync(uint64_t *seen, uint64_t timeout_ns) { (void)seen; (void)timeout_ns; return 0; }
__attribute__((weak)) int nvn_vsync_hz(void) { return 60; }
static void *vsync_thread(void *arg) {
    vsync_event_t *v = arg;
    dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);   // the game paces frames on this event
    const char *e = getenv("DM_VSYNC_HZ"); double hz = e ? atof(e) : 60.0; if (hz <= 0) hz = 60.0;
    uint64_t period = (uint64_t)(1e9 / hz), next = now_ns() + period, seen = 0;
    for (;;) {
        if (!e) period = nvn_vsync_hz() == 120 ? 1000000000ull / 120 : 1000000000ull / 60;
        if (!e && mtl_link_wait_vsync(&seen, 2 * period)) {
            if (!vsync_tick(v)) break;
            next = now_ns() + period;
            continue;
        }
        uint64_t t = now_ns(); if (next > t) { struct timespec ts = {(time_t)((next - t) / 1000000000ull), (long)((next - t) % 1000000000ull)}; nanosleep(&ts, NULL); }
        if (!vsync_tick(v)) break;
        next += period; if (now_ns() > next + 4 * period) next = now_ns() + period;   // no catch-up when far behind
    }
    free(v); return NULL;
}
HLE_FORCE("_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", "vsync event: a host thread signals it at 60 Hz", Result, h_vi_VsyncEvent, void *ev, void *disp) {
    (void)disp; hle_bind_system_event(ev, 1);
    uint64_t binding = hle_system_event_binding(ev);
    pthread_mutex_lock(&g_vs_mu);
    int running = 0; for (vsync_event_t *v = g_vs_events; v; v = v->next) running |= v->binding == binding;
    if (!running) {
        vsync_event_t *v = calloc(1, sizeof *v); if (!v) die("cannot allocate a vsync event");
        v->ev = ev; v->binding = binding; v->next = g_vs_events; g_vs_events = v;
        pthread_t th;
        if (pthread_create(&th, NULL, vsync_thread, v)) die("cannot start the vsync thread");
        pthread_detach(th);
    }
    pthread_mutex_unlock(&g_vs_mu);
    LOG("vi::GetDisplayVsyncEvent: %p signalled by the host at 60 Hz%s", ev, running ? " (its thread already runs)" : ""); return RES_OK;
}

HLE_FORCE("_ZN2nn4diag6detail7PutImplERKNS0_11LogMetaDataEPKcm", "game log → host log", void, h_diag_Put, const void *meta, const char *s, size_t n) {
    (void)meta;
    while (n && (s[n - 1] == '\n' || s[n - 1] == '\0')) n--;
    if (n) LOG("[game] %.*s", (int)n, s);
}
__attribute__((noinline)) static void diag_abort(uint32_t res, void *lr) {
    module_t *m = module_of(lr);
    LOG("game called nn::diag abort: Result %#x, call site %s+%#llx", res, m ? m->name : "?", m ? (unsigned long long)((uint8_t *)lr - m->base) : (unsigned long long)(uintptr_t)lr);
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wframe-address"   // the caller's frame on purpose (noinline keeps it one up)
    dm_backtrace((uint64_t)__builtin_frame_address(1));
#pragma clang diagnostic pop
    die("game aborted");
}
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEv", "game aborted", void, h_diag_Abort0, void) { diag_abort(0, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn4diag6detail9AbortImplEPKNS_6ResultE", "game aborted (with Result)", void, h_diag_AbortR, const uint32_t *r) { diag_abort(r ? *r : 0, dm_bigstack_caller(__builtin_return_address(0))); }
HLE_FORCE("_ZN2nn4diag12GetBacktraceEPmi", "backtrace; the host provides none (0 frames)", int, h_diag_Bt, uint64_t *out, int n) { (void)out; (void)n; return 0; }
// Only the FORMAT is printed: the host cannot read a __va_list the guest built (its save area holds guest registers).
HLE_FORCE("_ZN2nn4diag6detail10VAbortImplEPKcS3_S3_iPKNS_6ResultEPKNS_2os17UserExceptionInfoES3_RSt9__va_list", "SDK abort message", void, h_diag_VAbort,
          const char *fn, const char *expr, const char *file, int line, const void *res, const void *info, const char *fmt, void *ap) {
    (void)info; (void)ap;
    LOG("[diag] abort at %s: %s (%s:%d) message format %s", fn ? fn : "?", expr ? expr : "?", file ? file : "?", line, fmt ? fmt : "?");
    diag_abort(res ? *(const uint32_t *)res : 0, dm_bigstack_caller(__builtin_return_address(0)));
}

// Reset: the NFC device returns to its pre-Initialize state (area written out first); vsync threads end once the bindings are gone, so the new boot's binding gets one.
void dm_sdk16_reset(void) {
    memset(g_ev_netclock, 0, sizeof g_ev_netclock);
    pthread_mutex_lock(&g_nfp_mu);
    if (g_tag.area_dirty) nfp_area_save();
    g_tag.area_made = g_tag.area_open = g_tag.area_dirty = 0;
    g_nfp_state = NFP_STATE_INIT;
    pthread_mutex_unlock(&g_nfp_mu);
}
void dm_sdk16_reset_wait(void) {
    uint64_t t0 = now_ns(); int n = 0;
    for (;;) {
        pthread_mutex_lock(&g_vs_mu);
        n = 0;
        for (vsync_event_t *v = g_vs_events; v; v = v->next) n++;
        pthread_mutex_unlock(&g_vs_mu);
        if (!n || now_ns() - t0 > 200000000ull) break;
        usleep(1000);
    }
    if (n) LOG("[RED] reset: %d vsync threads did not end within 200 ms", n);
}
