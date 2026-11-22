// hle_audren.c's Rust half (dm/host/rust/src/audren.rs, its C entry points in audren_ffi.rs): what reads data the game's SDK wrote. Linked from the host's static library (rust.mk)
#pragma once
#include <stddef.h>
#include <stdint.h>

// The work buffer Horizon's renderer service asks for the AudioRendererParameter at p (its first 0x2c bytes), of the given revision ("REV" and its number, 1 to 15);
// 0 for a revision it does not know or a count out of bounds, where the caller keeps the SDK's requirement
size_t dm_audren_work_size(const uint8_t *p, uint32_t revision);
