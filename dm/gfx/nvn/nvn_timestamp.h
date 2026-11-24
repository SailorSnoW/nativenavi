// Raw counter-report timestamps: NVN2 records ns; the profile's gpu_ts_hz or DM_NVN_TS_HZ=<Hz> sets another rate
// (docs/reference/frame-rate.md#a-game-that-reads-raw-timestamp-reports-at-another-rate). Wide products avoid overflow on long uptimes.
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

#define NVN_TS_HZ_NS 1000000000ull
extern uint64_t nvn_ts_profile_hz;   // 0 = NVN2's ns
static inline uint64_t nvn_ts_env_hz(void) {
    static _Atomic uint64_t hz;   // the rate + 1 once read
    uint64_t value = atomic_load_explicit(&hz, memory_order_relaxed);
    if (!value) {
        const char *e = getenv("DM_NVN_TS_HZ");
        uint64_t v = e && *e ? strtoull(e, NULL, 10) : 0;
        value = (v < UINT64_MAX ? v : 0) + 1;
        uint64_t unset = 0;
        if (!atomic_compare_exchange_strong_explicit(&hz, &unset, value, memory_order_relaxed, memory_order_relaxed)) value = unset;
    }
    return value - 1;
}
static inline uint64_t nvn_ts_hz(void) {
    uint64_t env = nvn_ts_env_hz();
    return env ? env : nvn_ts_profile_hz ? nvn_ts_profile_hz : NVN_TS_HZ_NS;
}
static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) {
    uint64_t hz = nvn_ts_hz();
    return hz == NVN_TS_HZ_NS ? ns : (uint64_t)((__uint128_t)ns * hz / NVN_TS_HZ_NS);
}
static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) {
    uint64_t hz = nvn_ts_hz();
    return hz == NVN_TS_HZ_NS ? ticks : (uint64_t)((__uint128_t)ticks * NVN_TS_HZ_NS / hz);
}
