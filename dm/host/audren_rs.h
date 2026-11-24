// Rust half of hle_audren.c (rust/src/audren.rs): parses what the game's SDK wrote.
#pragma once
#include <stddef.h>
#include <stdint.h>

// Work buffer size for the AudioRendererParameter at p; 0 when unknown (the caller keeps the SDK's).
size_t dm_audren_work_size(const uint8_t *p, uint32_t revision);

// Must match audren.rs FRAME_MAX, SINK_CHANNELS, TAPS.
enum { DM_AUDREN_FRAME_MAX = 1024, DM_AUDREN_CHANNELS = 8, DM_AUDREN_TAPS = 16 };
typedef struct {
    int32_t up[DM_AUDREN_CHANNELS * (DM_AUDREN_TAPS - 1 + DM_AUDREN_FRAME_MAX)];
    uint32_t up_pos;
    uint8_t sink_logged, pad[3];
} dm_audren_state;
typedef struct {
    int32_t mixed[DM_AUDREN_FRAME_MAX * DM_AUDREN_CHANNELS];
    uint8_t walked, hist_logged, sink_id, said, pad[4];
} dm_audren_scan;
#define DM_AUDREN_SCAN_INIT { .sink_id = 0x14 }   // DeviceSink command ID (SDK 21+)
_Static_assert(sizeof(dm_audren_state) == 33256 && sizeof(dm_audren_scan) == 32776, "audren.rs State and Scan are these sizes");

// The frame's sample count, -1 when the list has no DeviceSink. The caller holds the lock over scan and st.
int dm_audren_capture(dm_audren_scan *scan, dm_audren_state *st, const uint8_t *cl, size_t len, int16_t *dst, size_t dst_n, int *loud);
