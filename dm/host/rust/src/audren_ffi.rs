//! What hle_audren.c calls (declared in dm/host/audren_rs.h): the raw pointers it passes become references and slices here, and the safe code in audren.rs does the work.
#![allow(unsafe_code)]

use crate::audren;

/// size_t dm_audren_work_size(const uint8_t *p, uint32_t revision): audren::work_size for the game's parameter at p (0 for a null p)
///
/// # Safety
/// p is null or points to at least audren::PARAM_BYTES readable bytes
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dm_audren_work_size(p: *const u8, revision: u32) -> usize {
    // SAFETY: the callers pass hle_audren.c's copy of the AudioRendererParameter (P_SIZE, 0x40 bytes, more than PARAM_BYTES); a byte array has no alignment to keep
    match unsafe { p.cast::<[u8; audren::PARAM_BYTES]>().as_ref() } {
        Some(param) => audren::work_size(param, revision),
        None => 0,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn work_size_through_the_c_entry_point() {
        // hle_audren.c's 0x40-byte copy of t_audren_work.c's parameter, revision 8 at +0x34
        let mut p = [0u8; 0x40];
        for (i, w) in [48000u32, 240, 132, 19, 138, 1, 16, 0, 0, 85, 1800, 0, 0, 0x3856_4552].iter().enumerate() {
            p[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
        }
        // SAFETY: p is 0x40 readable bytes; null is the other case the entry point takes
        unsafe {
            assert_eq!(dm_audren_work_size(p.as_ptr(), 0x3856_4552), 0x61d000);
            assert_eq!(dm_audren_work_size(p.as_ptr().add(1), 0x3856_4552), audren::work_size(&p[1..1 + audren::PARAM_BYTES].try_into().unwrap(), 0x3856_4552)); // any alignment
            assert_eq!(dm_audren_work_size(core::ptr::null(), 0x3856_4552), 0);
        }
    }
}
