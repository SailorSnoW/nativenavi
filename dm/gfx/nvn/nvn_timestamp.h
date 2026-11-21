// NVN counter timestamps, the raw value a counter report records: on the Switch (NVN, the X1) 614.4 MHz ticks (384 ticks per 625 ns), not the CPU's 19.2 MHz system counter;
// on the Switch 2 (NVN2) nanoseconds. nn::gfx shows both: its GetDuration subtracts two raw timestamps and converts the difference, by 625 / 384 in its NVN backend (ApiVersion 8,
// Mario Golf: Super Rush main+0xaad0, tail to main+0x7210) and not at all in its NVN2 backend (ApiVersion 200, Welcome Tour main+0x613610, tail to a bare ret).
// nvn_timestamp_ns is set with the device (nvnDeviceInitialize: dm_sdk_ounce). Keep the guest clock's epoch in both directions. Wide products avoid overflow on long host uptimes.
// A profile's gpu_ts_hz (nvn_ts_profile_hz, defined in nvn_mtl.m from game_profile.h; 0 = by the game's NVN, as above) sets another rate; DM_NVN_TS_HZ=<Hz> overrides both.
// The Legend of Zelda: Ocarina of Time (Switch 2) turns the difference of two raw reports into its wind and particle time step at 31.25 MHz, the
// Switch 2's system counter rate: at 614.4 MHz its foliage, falling leaves and hanging plants moved 19.66 times too fast (an agl pass's time input
// advanced 0.3456 in a 17.88 ms frame, = 17.58 ms of GPU time read at 31.25 MHz); in NVN2's nanoseconds they would move 32 times too fast
#pragma once
#include <stdint.h>
#include <stdlib.h>
#include <stdatomic.h>

#define NVN_TS_HZ_DEFAULT 614400000ull   // NVN's (the X1's) rate
#define NVN_TS_HZ_NS 1000000000ull       // NVN2's: the nanosecond
__attribute__((weak)) int nvn_timestamp_ns;   // 1: the raw unit is the nanosecond (NVN2)
extern uint64_t nvn_ts_profile_hz;   // the profile's gpu_ts_hz (nvn_mtl.m), 0 = by the game's NVN
static inline uint64_t nvn_ts_env_hz(void) {   // DM_NVN_TS_HZ, read once; 0 = not set
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
static inline uint64_t nvn_ts_hz(void) {   // the rate of the raw counter timestamps
    uint64_t env = nvn_ts_env_hz();
    return env ? env : nvn_ts_profile_hz ? nvn_ts_profile_hz : nvn_timestamp_ns ? NVN_TS_HZ_NS : NVN_TS_HZ_DEFAULT;
}
static inline uint64_t nvn_timestamp_from_ns(uint64_t ns) {
    uint64_t hz = nvn_ts_hz();
    return hz == NVN_TS_HZ_NS ? ns : hz == NVN_TS_HZ_DEFAULT ? (uint64_t)((__uint128_t)ns * 384 / 625) : (uint64_t)((__uint128_t)ns * hz / NVN_TS_HZ_NS);
}
static inline uint64_t nvn_timestamp_to_ns(uint64_t ticks) {
    uint64_t hz = nvn_ts_hz();
    return hz == NVN_TS_HZ_NS ? ticks : hz == NVN_TS_HZ_DEFAULT ? (uint64_t)((__uint128_t)ticks * 625 / 384) : (uint64_t)((__uint128_t)ticks * NVN_TS_HZ_NS / hz);
}
