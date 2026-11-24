// Clock snapshot arithmetic (hle_time.c): CalculateSpanBetween and CalculateStandardUserSystemClockDifferenceByUser looked up by their mangled names in the real swforce
// registrations, run on host snapshots and on edited copies; the C library's local-time functions, UTC as on a Switch, against nn::time's local conversion
#include "../hle_time.c"
#include <mach-o/getsect.h>

uint64_t guest_ns(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
static int checks, fails;
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) if (!strcmp(e[i].name, name)) { count++; fn = (void *)e[i].fn; }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
static void *section_fn(const char *sect, const char *name, unsigned *count) {
    unsigned long bytes = 0; void *fn = NULL; *count = 0;
    const hle_ent *e = (const hle_ent *)getsectiondata(&_mh_execute_header, "__DATA", sect, &bytes);   // hle_force_ent and hle_ent both begin {name, fn}
    size_t step = !strcmp(sect, "swforce") ? sizeof(hle_force_ent) : sizeof(hle_ent);
    for (size_t i = 0; e && i < bytes / step; i++) { const hle_ent *x = (const hle_ent *)((const char *)e + i * step); if (!strcmp(x->name, name)) { ++*count; fn = x->fn; } }
    return fn;
}
// The C library's local time is UTC (the SDK's own functions run; the pass-through table maps to the host's UTC ones), nn::time's is the host zone's
static void check_utc_libc(void) {
    static const char *names[] = {"localtime", "localtime_r", "mktime"}; unsigned n;
    for (int i = 0; i < 3; i++) { section_fn("swforce", names[i], &n); CHECK(n == 0, "the SDK's localtime / localtime_r / mktime are not taken over"); }
    setenv("TZ", "Asia/Seoul", 1); tzset();
    int64_t (*mk)(struct tm *) = section_fn("swhle", "mktime", &n);
    struct tm *(*lt)(const int64_t *, struct tm *) = section_fn("swhle", "localtime_r", &n);
    const int64_t save = 1794761043;   // a save header's time, 2026-11-15 16:44:03 UTC
    struct tm u; time_t sv = (time_t)save; gmtime_r(&sv, &u);
    CHECK(mk && mk(&u) == save, "mktime of a UTC struct tm gives the time back unchanged (TZ=Asia/Seoul)");
    struct tm l; CHECK(lt && lt(&save, &l) == &l && l.tm_hour == 16 && l.tm_min == 44 && l.tm_gmtoff == 0, "localtime_r is UTC (TZ=Asia/Seoul)");
    Result (*cal)(CalendarTime *, CalendarAdditionalInfo *, const int64_t *) = hook("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE");
    CalendarTime ct; CalendarAdditionalInfo ai;
    CHECK(cal && cal(&ct, &ai, &save) == RES_OK && ct.day == 16 && ct.hour == 1 && ct.minute == 44 && ai.offset == 9 * 3600, "ToCalendarTime converts in the host zone (01:44 KST)");
    setenv("TZ", "UTC", 1); tzset();
}
static void put64(uint8_t *s, int off, int64_t v) { memcpy(s + off, &v, 8); }
static int64_t get64(const uint8_t *s, int off) { int64_t v; memcpy(&v, s + off, 8); return v; }
int main(void) {
    Result (*snap)(uint8_t *) = hook("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_");
    unsigned (*span)(int64_t *, const uint8_t *, const uint8_t *) = hook("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_");
    int64_t (*udiff)(const uint8_t *, const uint8_t *) = hook("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_");
    check_utc_libc();
    if (!snap || !span || !udiff) { printf("time: %d checks, %d failed\n", checks, fails); return 1; }
    uint8_t a[0xd0], b[0xd0], c[0xd0]; int64_t ts = -1;
    // Before the first snapshot: h_time_GetCurrentTime reads DM_FIXED_TIME once and caches it
    setenv("DM_FIXED_TIME", "1700000000", 1);
    setenv("TZ", "UTC", 1); tzset();
    CHECK(snap(a) == RES_OK, "snapshot a");
    CHECK(a[0xcd] == 1, "standard snapshot type is 1");
    // The snapshot must stay inside the SDK's 0xd0-byte buffer, including the type near its end.
    uint8_t guarded[0xf0]; memset(guarded, 0xa5, sizeof guarded);
    uint8_t *second = guarded + 0x10;
    CHECK(snap(second) == RES_OK, "snapshot in guarded buffer");
    int guards_ok = 1;
    for (int i = 0; i < 0x10; i++) guards_ok &= guarded[i] == 0xa5 && guarded[0xe0 + i] == 0xa5;
    CHECK(guards_ok, "snapshot writes only its 0xd0 bytes");
    CHECK(get64(second, 0x40) == 1700000000, "snapshot user time follows the host clock");
    // StandardSteadyClock::now(), the C++ clock form (SDK 3.7: 0 without the time service session): the steady clock's seconds, as the time point getter gives them
    int64_t (*steady_now)(void) = hook("_ZN2nn4time19StandardSteadyClock3nowEv");
    Result (*steady_tp)(uint8_t *) = hook("_ZN2nn4time38GetStandardSteadyClockCurrentTimePointEPNS0_20SteadyClockTimePointE");
    uint8_t tp0[24]; int64_t sn = steady_now ? steady_now() : -1;
    CHECK(steady_tp && steady_tp(tp0) == RES_OK && sn > 0 && llabs(sn - get64(tp0, 0)) <= 1, "StandardSteadyClock::now() is the steady clock's seconds, not 0");
    Result (*net)(int64_t *) = hook("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE");
    int64_t nt = -1;
    CHECK(net && net(&nt) == RES_OK && nt == 1700000000, "the network clock reads the host clock, as the user clock (saves are dated by it)");
    const CalendarTime *cal = (const CalendarTime *)(second + 0x50);
    CHECK(cal->year == 2023 && cal->month == 11 && cal->day == 14 && cal->hour == 22 && cal->minute == 13 && cal->second == 20,
          "snapshot has the UTC calendar for the fixed time");
    CHECK(!memcmp(second + 0x20, second, 0x20) && get64(second, 0x48) == 1700000000 && !memcmp(second + 0x58, second + 0x50, 8) &&
          !memcmp(second + 0x78, second + 0x60, 0x18), "snapshot network fields are the user clock's (context, time, calendar, its info)");
    int64_t (*net_now)(void) = hook("_ZN2nn4time26StandardNetworkSystemClock3nowEv");
    CHECK(net_now && net_now() == 1700000000, "the network clock's now() reads the host clock, as GetCurrentTime does");
    CHECK(!memcmp(a + 0x98, second + 0x98, 16), "both snapshots use the same steady clock source");
    CHECK(!strcmp((const char *)second + 0xa8, "UTC"), "snapshot carries the host time zone");
    memcpy(b, a, sizeof b); b[0xcd] = 2;   // as the SDK marks a snapshot of the adjustable clocks
    CHECK(span(&ts, a, b) == 1 && ts == 0 && udiff(a, b) == 0, "changing only the type leaves the elapsed span at zero and reports no user clock change");
    unsetenv("DM_FIXED_TIME");
    CHECK(span(&ts, a, a) == 1 && ts == 0, "span of a snapshot with itself is 0");
    memcpy(b, a, sizeof b); put64(b, 0x90, get64(a, 0x90) + 90);   // b: 90 s of steady clock later
    CHECK(span(&ts, a, b) == 1 && ts == 90 * 1000000000LL, "span from the steady time points, in nanoseconds");
    CHECK(span(&ts, b, a) == 1 && ts == -90 * 1000000000LL, "span backwards is negative");
    memcpy(c, b, sizeof c); c[0x98] ^= 1; ts = 1234;   // c: another steady clock source, no network time
    put64(a, 0x48, 0); put64(c, 0x48, 0);   // as a snapshot made with no network clock
    CHECK(span(&ts, a, c) == 0 && ts == 1234, "different sources and no network time: false, output untouched");
    put64(a, 0x48, 1000); put64(c, 0x48, 1600);   // with network times the span comes from them
    CHECK(span(&ts, a, c) == 1 && ts == 600 * 1000000000LL, "different sources: span from the network times");
    CHECK(snap(a) == RES_OK && snap(b) == RES_OK && udiff(a, b) == 0, "two host snapshots in a row: the user did not move the clock");
    memcpy(b, a, sizeof b); put64(b, 0x00, get64(a, 0x00) + 3600);   // b: the user clock offset one hour later
    CHECK(udiff(a, b) == 3600 * 1000000000LL, "user clock moved an hour");
    b[0x10] ^= 1; CHECK(udiff(a, b) == 0, "user contexts of different steady sources: 0");
    b[0x10] ^= 1; a[0xcc] = b[0xcc] = 1; CHECK(udiff(a, b) == 0, "automatic correction on in both: 0");
    a[0xcc] = 0; CHECK(udiff(a, b) == 3600 * 1000000000LL, "automatic correction in one only: the difference");
    // The periodic benefit claim's two service questions: the location the host's snapshots carry, and time points of no clock (a source ID no snapshot has)
    Result (*auto_upd)(uint8_t *) = hook("_ZN2nn4time6detail56GetStandardUserSystemClockAutomaticCorrectionUpdatedTimeEPNS0_20SteadyClockTimePointE");
    Result (*loc_upd)(char *, uint8_t *) = hook("_ZN2nn4time6detail35GetDeviceLocationNameAndUpdatedTimeEPNS0_12LocationNameEPNS0_20SteadyClockTimePointE");
    uint8_t tp[0x18], none[0x18] = {0}; char loc[0x24];
    memset(tp, 0xaa, sizeof tp); CHECK(auto_upd && auto_upd(tp) == RES_OK && !memcmp(tp, none, sizeof tp), "automatic correction updated time: a cleared time point");
    memset(tp, 0xaa, sizeof tp); memset(loc, 0xaa, sizeof loc);
    CHECK(loc_upd && loc_upd(loc, tp) == RES_OK && !memcmp(loc, a + 0xa8, sizeof loc) && !memcmp(tp, none, sizeof tp), "device location: the snapshot's name, a cleared time point");
    CHECK(memcmp(tp + 8, a + 0x90 + 8, 16), "the cleared time point's source ID is not the snapshots' steady clock");
    // The contexts of the network clock and of the adjustable clocks: each is the user clock's, as the snapshot carries it, within the context's 0x20 bytes
    static const char *ctx_hooks[] = {
        "_ZN2nn4time26StandardNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE",
        "_ZN2nn4time25AdjustableUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE",
        "_ZN2nn4time28AdjustableNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE"};
    CHECK(snap(a) == RES_OK, "snapshot for the contexts");
    for (int h = 0; h < 3; h++) {
        Result (*get_ctx)(uint8_t *) = hook(ctx_hooks[h]);
        uint8_t g[0x40]; memset(g, 0xa5, sizeof g);
        int ok = get_ctx && get_ctx(g + 0x10) == RES_OK && !memcmp(g + 0x10, a, 0x20) && !memcmp(g + 0x10, a + 0x20, 0x20);
        for (int i = 0; i < 0x10; i++) ok &= g[i] == 0xa5 && g[0x30 + i] == 0xa5;
        CHECK(ok, ctx_hooks[h]);
    }
    printf("time: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
