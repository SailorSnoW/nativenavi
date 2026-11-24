// NVN2 raw timestamp ABI: nanoseconds, a profile's rate with known vectors, long uptimes.
#include "../nvn/nvn_timestamp.h"
uint64_t nvn_ts_profile_hz = 0;   // 0: NVN2's nanoseconds (nvn_mtl.m defines it from the profile)
#include <stdio.h>
#include <pthread.h>
#include <sched.h>
#include <stdatomic.h>

static _Atomic int ready, start;
static void *timestamp_reader(void *unused) {
    (void)unused;
    atomic_fetch_add(&ready, 1);
    while (!atomic_load(&start)) sched_yield();
    for (int i = 0; i < 1000; i++)
        if (nvn_timestamp_from_ns(1000000000) != NVN_TS_HZ_NS) return (void *)1;
    return NULL;
}

static int checks, fails;
#define CHECK(c, why) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", why); } } while (0)

int main(void) {
    pthread_t workers[8];
    for (int i = 0; i < 8; i++) if (pthread_create(&workers[i], NULL, timestamp_reader, NULL)) abort();
    while (atomic_load(&ready) != 8) sched_yield();
    atomic_store(&start, 1);
    for (int i = 0; i < 8; i++) { void *result = NULL; pthread_join(workers[i], &result); CHECK(!result, "concurrent first timestamp conversion"); }
    // NVN2 (the Switch 2): reports record nanoseconds. nn::gfx's NVN2 GetDuration is the raw difference, taken as a TimeSpan
    const uint64_t ns[] = {1, 2, 624, 626, 16666667, UINT64_C(1234567890123456789), UINT64_MAX};
    const uint64_t at = UINT64_C(988357000000000), frame = 16666667;
    for (unsigned i = 0; i < sizeof ns / sizeof *ns; i++)
        CHECK(nvn_timestamp_from_ns(ns[i]) == ns[i] && nvn_timestamp_to_ns(ns[i]) == ns[i], "NVN2: raw timestamps are nanoseconds both ways");
    CHECK(nvn_timestamp_from_ns(at + frame) - nvn_timestamp_from_ns(at) == frame, "NVN2: nn::gfx's raw difference reads a 60 fps frame as 16.67 ms");
    // A profile's gpu_ts_hz wins over NVN2's nanoseconds: Ocarina of Time reads its raw reports at the Switch 2's 31.25 MHz system counter rate (32 ns a tick)
    nvn_ts_profile_hz = 31250000;
    const struct { uint64_t ns, ticks; } exact[] = {
        {0, 0}, {32, 1}, {1000000, 31250}, {16000000, 500000}, {1000000000, 31250000},
        {UINT64_C(5000000000000000000), UINT64_C(156250000000000000)},
    };
    for (unsigned i = 0; i < sizeof exact / sizeof *exact; i++) {
        CHECK(nvn_timestamp_from_ns(exact[i].ns) == exact[i].ticks, "known ns -> 31.25 MHz ticks");
        CHECK(nvn_timestamp_to_ns(exact[i].ticks) == exact[i].ns, "known raw ticks -> ns");
    }
    CHECK(nvn_timestamp_from_ns(31) == 0 && nvn_timestamp_from_ns(63) == 1, "ns -> ticks rounds down");
    for (unsigned i = 0; i < sizeof ns / sizeof *ns; i++) {
        uint64_t back = nvn_timestamp_to_ns(nvn_timestamp_from_ns(ns[i]));
        CHECK(back <= ns[i] && ns[i] - back < 32, "ns round trip loses less than a tick, including long uptime");
    }
    CHECK(nvn_timestamp_from_ns(at + frame) - nvn_timestamp_from_ns(at) == 520833, "a 60 fps frame at the profile's 31.25 MHz is 520,833 ticks");
    nvn_ts_profile_hz = 0;
    printf("timestamp: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
