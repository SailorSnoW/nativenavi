// hle_audren.c's Rust half (dm/host/rust/src/audren.rs, its C entry points in audren_ffi.rs): what reads data the game's SDK wrote. Linked from the host's static library (rust.mk)
#pragma once
#include <stddef.h>
#include <stdint.h>

// The work buffer Horizon's renderer service asks for the AudioRendererParameter at p (its first 0x2c bytes), of the given revision ("REV" and its number, 1 to 15);
// 0 for a revision it does not know or a count out of bounds, where the caller keeps the SDK's requirement
size_t dm_audren_work_size(const uint8_t *p, uint32_t revision);

// The frame out of a command list (audren.rs FRAME_MAX, SINK_CHANNELS, TAPS): at most DM_AUDREN_FRAME_MAX samples of DM_AUDREN_CHANNELS 16-bit values each, the sink's
// six channels then their stereo fold (hle.h DM_AUDIO_SINK_CHANNELS)
enum { DM_AUDREN_FRAME_MAX = 1024, DM_AUDREN_CHANNELS = 8, DM_AUDREN_TAPS = 16 };
// One renderer's state, zeroed when it opens (audren.rs State): the 3:2 resampler's last samples and position, and whether its sink was logged
typedef struct {
    int32_t up[DM_AUDREN_CHANNELS * (DM_AUDREN_TAPS - 1 + DM_AUDREN_FRAME_MAX)];
    uint32_t up_pos;
    uint8_t sink_logged, pad[3];
} dm_audren_state;
// What capture keeps for all renderers (audren.rs Scan), used under one lock: the frame being mixed, whether a list has walked yet, the sink's command ID, the lines said once
typedef struct {
    int32_t mixed[DM_AUDREN_FRAME_MAX * DM_AUDREN_CHANNELS];
    uint8_t walked, hist_logged, sink_id, said, pad[4];
} dm_audren_scan;
#define DM_AUDREN_SCAN_INIT { .sink_id = 0x14 }   // DeviceSink's command ID in SDK 21 on
_Static_assert(sizeof(dm_audren_state) == 33256 && sizeof(dm_audren_scan) == 32776, "audren.rs State and Scan are these sizes");

// The DeviceSink's output out of the command list cl..cl+len into dst (dst_n values): the frame's sample count, with *loud 1 when any value is not 0; -1 when the list gives
// none. scan and st are used by this call alone (the caller holds the lock over them)
int dm_audren_capture(dm_audren_scan *scan, dm_audren_state *st, const uint8_t *cl, size_t len, int16_t *dst, size_t dst_n, int *loud);
