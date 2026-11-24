// nn::time over the host's system clock. PosixTime = int64 s; CalendarTime = {s16 year, s8 month/day/hour/minute/second};
// CalendarAdditionalInfo = {u32 wday, u32 yday, char tz[8], u32 DST, s32 UTC offset s}.
#include "hle.h"
#include <stdatomic.h>
#include <time.h>

typedef struct { int16_t year; int8_t month, day, hour, minute, second; int8_t pad; } CalendarTime;
typedef struct { uint32_t wday, yday; char tzname[8]; uint32_t dst; int32_t offset; } CalendarAdditionalInfo;

// DM_FIXED_TIME=seconds: the user clock returns this (main.c pins the zone to UTC).
static int64_t fixed_time(void) {
    static int64_t v = -2;
    if (v == -2) {
        const char *e = getenv("DM_FIXED_TIME");
        v = e ? strtoll(e, 0, 0) : -1;
        if (v >= 0) LOG("DM_FIXED_TIME=%lld: the user clock returns a fixed value", (long long)v);
    }
    return v;
}
HLE_FORCE("_ZN2nn4time10InitializeEv", "connect to the time service; clocks come from the host system time instead", Result, h_time_Initialize, void) { return RES_OK; }
HLE_FORCE("_ZN2nn4time13IsInitializedEv", "the time service is taken over, always initialized", int, h_time_IsInitialized, void) { return 1; }
// {s since boot, 16-byte source ID}; the SDK's GetSpanBetween subtracts only when source IDs match.
static Result h_time_SteadyNow(uint8_t *out) {
    int64_t s = (int64_t)(clock_gettime_nsec_np(CLOCK_MONOTONIC) / 1000000000ull);
    memcpy(out, &s, 8);
    memcpy(out + 8, "dmhost-steady-01", 16);
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time38GetStandardSteadyClockCurrentTimePointEPNS0_20SteadyClockTimePointE", "steady clock (the free function form, SDK 20) → host monotonic clock", Result, h_time_SteadyNowFn, uint8_t *out) { return h_time_SteadyNow(out); }
// The SDK's asks the steady-clock session the taken-over Initialize never makes.
HLE_FORCE("_ZN2nn4time19StandardSteadyClock3nowEv", "steady clock (std::chrono form) → host monotonic clock (seconds)", int64_t, h_time_SteadyClockNow, void) {
    uint8_t tp[24];
    h_time_SteadyNow(tp);
    int64_t s;
    memcpy(&s, tp, 8);
    return s;
}
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "user clock → host time()", Result, h_time_GetCurrentTime, int64_t *out) {
    int64_t ft = fixed_time();
    *out = ft >= 0 ? ft : time(NULL);
    return RES_OK;
}
// The host clock is network-set: the network clock is the user clock everywhere (GetCurrentTime, now(), snapshots).
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock14GetCurrentTimeEPNS0_9PosixTimeE", "network clock → host time(), as the user clock (the host's clock is network-set)", Result, h_time_NetNow, int64_t *out) {
    Result r = h_time_GetCurrentTime(out);
    static _Atomic int once;
    if (!atomic_exchange(&once, 1)) LOG("time: the network clock's first read: %lld (the host's clock)", (long long)*out);
    return r;
}
// CLOCK_REALTIME source (SDK 17): system_clock::now() throws when it fails.
static int h_time_timespec(int64_t *ts) {
    int64_t ft = fixed_time();
    if (ft >= 0) {
        ts[0] = ft;
        ts[1] = 0;
        return 0;
    }
    struct timespec t;
    clock_gettime(CLOCK_REALTIME, &t);
    ts[0] = t.tv_sec;
    ts[1] = t.tv_nsec;
    return 0;
}
HLE_FORCE("_ZN2nn4time41GetStandardUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the user clock → host real time", int, h_time_StdTimespec, int64_t *ts) { return h_time_timespec(ts); }
HLE_FORCE("_ZN2nn4time43GetAdjustableUserSystemClockCurrentTimespecEP8timespec", "C library real-time clock from the adjustable user clock → host real time", int, h_time_AdjTimespec, int64_t *ts) { return h_time_timespec(ts); }
// musl's CLOCK_MONOTONIC ifunc (SDK 22) reads time shared memory the host never maps; unbound, steady_clock::now() throws.
static int h_time_mono_timespec(int64_t *ts) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    ts[0] = t.tv_sec;
    ts[1] = t.tv_nsec;
    return 0;
}
HLE_FORCE("_ZN2nn4time37GetStandardSteadyClockCurrentTimespecEP8timespec", "C library monotonic clock from the steady clock → host monotonic time", int, h_time_MonoTimespec, int64_t *ts) { return h_time_mono_timespec(ts); }
// {s64 offset, steady time point}. Games compare contexts to notice a clock set: kept until the host clock moves > 1 s against the guest ticks.
HLE_FORCE("_ZN2nn4time23StandardUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "user clock context from the time service; the host's user clock over its steady clock, renewed when the host clock is set",
          Result, h_time_GetContext, uint8_t *out) {
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    static uint8_t ctx[0x20];
    static int64_t drift;
    static int made;
    uint8_t tp[0x18];
    int64_t now;
    h_time_SteadyNow(tp);
    h_time_GetCurrentTime(&now);
    int64_t d = now - (int64_t)(guest_ns() / 1000000000ull);
    pthread_mutex_lock(&mu);
    if (!made || (fixed_time() < 0 && llabs(d - drift) > 1)) {
        int64_t off = now - *(const int64_t *)tp;
        memcpy(ctx, &off, 8);
        memcpy(ctx + 8, tp, 0x18);
        if (made) LOG("time: host clock moved %+lld s against the guest ticks, new user clock context", (long long)(d - drift));
        drift = d;
        made = 1;
    }
    memcpy(out, ctx, 0x20);
    pthread_mutex_unlock(&mu);
    return RES_OK;
}
// The SDK's returns 0 (1970) without its initialized flag, which the taken-over Initialize never sets.
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock3nowEv", "network clock → host time(), as the user clock", int64_t, h_time_net_now, void) { int64_t ft = fixed_time(); return ft >= 0 ? ft : time(NULL); }
// The service's answer (0x74) would abort AdjustableNetworkSystemClock::Adjust.
HLE_FORCE("_ZN2nn4time26StandardNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "network clock context, as the user clock's (the host's clock is network-set)",
          Result, h_time_NetContext, uint8_t *out) { return h_time_GetContext(out); }
// On the host each adjustable clock is its standard clock; Adjust runs as is and sets only the SDK's copy.
HLE_FORCE("_ZN2nn4time25AdjustableUserSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "adjustable user clock context, as the user clock's",
          Result, h_time_AdjContext, uint8_t *out) { return h_time_GetContext(out); }
HLE_FORCE("_ZN2nn4time28AdjustableNetworkSystemClock21GetSystemClockContextEPNS0_18SystemClockContextE", "adjustable network clock context, as the network clock's",
          Result, h_time_AdjNetContext, uint8_t *out) { return h_time_GetContext(out); }
HLE_FORCE("_ZN2nn4time14ToCalendarTimeEPNS0_12CalendarTimeEPNS0_22CalendarAdditionalInfoERKNS0_9PosixTimeE",
          "time zone rules of the time service; uses the host localtime_r instead", Result, h_time_ToCalendarTime, CalendarTime *ct, CalendarAdditionalInfo *ai, const int64_t *pt) {
    time_t t = *pt;
    struct tm tm;
    localtime_r(&t, &tm);
    static int once;
    if (!once++) LOG("ToCalendarTime: first conversion %lld → %04d-%02d-%02d %02d:%02d (time zone %s, offset %ld s)", (long long)t, tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_zone ? tm.tm_zone : "?", (long)tm.tm_gmtoff);
    if (ct) *ct = (CalendarTime){(int16_t)(tm.tm_year + 1900), (int8_t)(tm.tm_mon + 1), (int8_t)tm.tm_mday, (int8_t)tm.tm_hour, (int8_t)tm.tm_min, (int8_t)tm.tm_sec, 0};
    if (ai) {
        memset(ai, 0, sizeof *ai);
        ai->wday = tm.tm_wday;
        ai->yday = tm.tm_yday;
        strlcpy(ai->tzname, tm.tm_zone ? tm.tm_zone : "UTC", sizeof ai->tzname);
        ai->dst = tm.tm_isdst > 0;
        ai->offset = (int32_t)tm.tm_gmtoff;
    }
    return RES_OK;
}
// 0xD0 bytes, as Ryujinx's ClockSnapshot: +0 / +0x20 user / network context, +0x40 / +0x48 times, +0x50 / +0x58 calendars, +0x60 / +0x78 their info,
// +0x90 steady time point, +0xa8 location name, +0xcc automatic correction, +0xcd type. Network fields = the user clock's.
HLE_FORCE("_ZN2nn4time13ClockSnapshot29CreateWithStandardSystemClockEPS1_", "snapshot from the time service; the host fills it on the spot (network clock = user clock)", Result, h_time_Snapshot, uint8_t *s) {
    memset(s, 0, 0xd0);
    uint8_t tp[0x18];
    h_time_SteadyNow(tp);
    int64_t now;
    h_time_GetCurrentTime(&now);
    h_time_GetContext(s);   // one context until the host clock is set (per snapshot it would differ by a second at random)
    memcpy(s + 0x40, &now, 8);
    h_time_ToCalendarTime((CalendarTime *)(s + 0x50), (CalendarAdditionalInfo *)(s + 0x60), &now);
    memcpy(s + 0x20, s, 0x20);
    memcpy(s + 0x48, &now, 8);
    memcpy(s + 0x58, s + 0x50, 8);
    memcpy(s + 0x78, s + 0x60, 0x18);
    memcpy(s + 0x90, tp, 0x18);
    tzset();
    strlcpy((char *)s + 0xa8, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24);
    s[0xcd] = 1;   // 1 standard, 2 adjustable, 0 uninitialized
    return RES_OK;
}
// What the time service computes (as Ryujinx's). TimeSpan is ns. CalculateSpanBetween returns bool (true when stored), not a Result.
static int64_t snap64(const uint8_t *s, int off) { int64_t v; memcpy(&v, s + off, 8); return v; }
static int same_source(const uint8_t *a, const uint8_t *b, int tp) { return !memcmp(a + tp + 8, b + tp + 8, 16); }
HLE_FORCE("_ZN2nn4time20CalculateSpanBetweenEPNS_8TimeSpanERKNS0_13ClockSnapshotES5_", "span between two snapshots (time service): their steady time points, else their network times",
          unsigned, h_time_SpanBetween, int64_t *out, const uint8_t *a, const uint8_t *b) {
    int64_t s;
    if (same_source(a, b, 0x90)) s = snap64(b, 0x90) - snap64(a, 0x90);
    else if (snap64(a, 0x48) && snap64(b, 0x48)) s = snap64(b, 0x48) - snap64(a, 0x48);
    else return 0;   // no common clock: *out untouched, as the SDK leaves it
    *out = s * 1000000000;
    return 1;
}
// 0 when the steady sources differ or both have automatic correction on.
HLE_FORCE("_ZN2nn4time48CalculateStandardUserSystemClockDifferenceByUserERKNS0_13ClockSnapshotES3_", "user clock change between two snapshots (time service): from their user clock offsets",
          int64_t, h_time_UserDiff, const uint8_t *a, const uint8_t *b) {
    if (!same_source(a, b, 0x08) || (a[0xcc] && b[0xcc])) return 0;
    return (snap64(b, 0x00) - snap64(a, 0x00)) * 1000000000;
}
// The host knows neither moment: a cleared time point matches no snapshot's source, so the claim uses only the snapshots.
HLE_FORCE("_ZN2nn4time6detail56GetStandardUserSystemClockAutomaticCorrectionUpdatedTimeEPNS0_20SteadyClockTimePointE",
          "when automatic correction was last switched (time service); not known on the host: a time point of no clock", Result, h_time_AutoCorrUpdated, uint8_t *tp) {
    memset(tp, 0, 0x18);
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time6detail35GetDeviceLocationNameAndUpdatedTimeEPNS0_12LocationNameEPNS0_20SteadyClockTimePointE",
          "device location and when it was set (time service): the host's zone, as in its snapshots, and a time point of no clock", Result, h_time_LocUpdated, char *name, uint8_t *tp) {
    memset(name, 0, 0x24);
    tzset();
    strlcpy(name, tzname[0] && *tzname[0] ? tzname[0] : "UTC", 0x24);
    memset(tp, 0, 0x18);
    return RES_OK;
}
// Around a DST switch one local time maps to 0 or 2 instants; mktime gives one.
HLE_FORCE("_ZN2nn4time11ToPosixTimeEPiPNS0_9PosixTimeEiRKNS0_12CalendarTimeE",
          "time zone rules of the time service; uses the host mktime instead", Result, h_time_ToPosixTime, int *count, int64_t *out, int n, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second, .tm_isdst = -1};
    time_t t = mktime(&tm);
    if (count) *count = n > 0;
    if (n > 0 && out) out[0] = (int64_t)t;
    return RES_OK;
}
HLE_FORCE("_ZN2nn4time18ToPosixTimeFromUtcERKNS0_12CalendarTimeE", "UTC calendar → absolute seconds; host timegm", int64_t, h_time_ToPosixUtc, const CalendarTime *ct) {
    struct tm tm = {.tm_year = ct->year - 1900, .tm_mon = ct->month - 1, .tm_mday = ct->day, .tm_hour = ct->hour, .tm_min = ct->minute, .tm_sec = ct->second};
    return (int64_t)timegm(&tm);
}
// localtime / mktime stay the SDK's: UTC on a Switch (a fixed musl zone stub); taking them over would shift a game's UTC struct tm.
