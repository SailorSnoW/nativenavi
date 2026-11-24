//! hle_audren.c's entry points (audren_rs.h): raw pointers become references and slices for audren.rs.
#![allow(unsafe_code)]

use crate::audren::{self, Scan, State};
use core::ffi::c_int;
use core::{ptr, slice};

/// # Safety
/// p is null or points to at least audren::PARAM_BYTES readable bytes
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dm_audren_work_size(p: *const u8, revision: u32) -> usize {
    // SAFETY: hle_audren.c's 0x40-byte parameter copy; a byte array needs no alignment
    match unsafe { p.cast::<[u8; audren::PARAM_BYTES]>().as_ref() } {
        Some(param) => audren::work_size(param, revision),
        None => 0,
    }
}

/// # Safety
/// scan and st are valid and unshared during the call; cl..cl+len readable, dst..dst+dst_n writable, no overlaps
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dm_audren_capture(scan: *mut Scan, st: *mut State, cl: *const u8, len: usize, dst: *mut i16, dst_n: usize, loud: *mut c_int) -> c_int {
    // SAFETY: every caller holds g_render_mu, so nothing else touches scan or st; loud is the caller's local
    let (Some(scan), Some(st), Some(loud)) = (unsafe { scan.as_mut() }, unsafe { st.as_mut() }, unsafe { loud.as_mut() }) else { return -1 };
    if cl.is_null() || dst.is_null() {
        return -1;
    }
    // SAFETY: the list is rewritten only under g_render_mu; dst is the caller's output block
    let (list, dst) = unsafe { (slice::from_raw_parts(cl, len), slice::from_raw_parts_mut(dst, dst_n)) };
    let frame = audren::capture(scan, st, list, dst, |addr, words| {
        let p = ptr::with_exposed_provenance::<i32>(usize::try_from(addr).ok()?);
        if p.is_null() || !p.is_aligned() || words > isize::MAX as usize / size_of::<i32>() {
            return None;
        }
        // SAFETY: the SDK clears and mixes these words every frame from open to close; the frame's commands have run, and the next generation waits for g_render_mu
        Some(unsafe { slice::from_raw_parts(p, words) })
    });
    match frame {
        Some(f) => {
            *loud = c_int::from(f.loud);
            c_int::try_from(f.samples).unwrap_or(-1)
        }
        None => -1,
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::audren::tests::{build, new_scan, new_state};
    use crate::audren::{FRAME_MAX, SINK_CHANNELS};
    use crate::log::captured;

    #[test]
    fn work_size_through_the_c_entry_point() {
        // revision 8 at +0x34
        let mut p = [0u8; 0x40];
        for (i, w) in [48000u32, 240, 132, 19, 138, 1, 16, 0, 0, 85, 1800, 0, 0, 0x3856_4552].iter().enumerate() {
            p[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
        }
        // SAFETY: p is 0x40 readable bytes
        unsafe {
            assert_eq!(dm_audren_work_size(p.as_ptr(), 0x3856_4552), 0x61d000);
            assert_eq!(dm_audren_work_size(p.as_ptr().add(1), 0x3856_4552), audren::work_size(&p[1..1 + audren::PARAM_BYTES].try_into().unwrap(), 0x3856_4552)); // any alignment
            assert_eq!(dm_audren_work_size(core::ptr::null(), 0x3856_4552), 0);
        }
    }

    fn call(scan: &mut Scan, st: &mut State, list: &[u8], dst: &mut [i16]) -> (c_int, c_int) {
        let mut loud = -7;
        // SAFETY: live Rust objects of the lengths given, none overlapping
        let n = unsafe { dm_audren_capture(scan, st, list.as_ptr(), list.len(), dst.as_mut_ptr(), dst.len(), &mut loud) };
        (n, loud)
    }

    #[test]
    fn capture_through_the_c_entry_point() {
        // an integer, as the SDK's: its writer exposed the provenance
        let mix: Vec<i32> = (0..8 * 240).map(|w| [3000, -3000][(w / 240).min(1)]).collect();
        let at = mix.as_ptr().expose_provenance() as u64;
        let (mut scan, mut st, mut out) = (new_scan(), new_state(), vec![0i16; FRAME_MAX * SINK_CHANNELS]);
        assert_eq!(call(&mut scan, &mut st, &build(6, 240, 48000, &[0, 1], at), &mut out), (240, 1));
        assert_eq!((out[6], out[7], out[8 * 239 + 6], out[8 * 239 + 7]), (3000, -3000, 3000, -3000));
        let quiet = vec![0i32; 8 * 240];
        let at = quiet.as_ptr().expose_provenance() as u64;
        assert_eq!(call(&mut scan, &mut st, &build(6, 240, 48000, &[0, 1], at), &mut out), (240, 0));
        captured::take();
        for bad in [0, at + 2] {
            let (mut scan, mut st) = (new_scan(), new_state());
            assert_eq!(call(&mut scan, &mut st, &build(6, 240, 48000, &[0, 1], bad), &mut out).0, -1, "{bad:#x}");
        }
        assert!(captured::take().iter().filter(|l| l.contains("cannot be read")).count() == 2);
        assert_eq!(call(&mut scan, &mut st, &build(6, 240, 48000, &[0, 1], at), &mut out[..240 * SINK_CHANNELS - 1]).0, -1);
    }

    #[test]
    fn null_pointers_give_no_frame() {
        let (mut scan, mut st, mut out, mut loud) = (new_scan(), new_state(), vec![0i16; 8], 0);
        let list = build(6, 240, 48000, &[0, 1], 0x1000);
        // SAFETY: the entry point checks each pointer for null first
        unsafe {
            assert_eq!(dm_audren_capture(ptr::null_mut(), &mut *st, list.as_ptr(), list.len(), out.as_mut_ptr(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, ptr::null_mut(), list.as_ptr(), list.len(), out.as_mut_ptr(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, &mut *st, ptr::null(), list.len(), out.as_mut_ptr(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, &mut *st, list.as_ptr(), list.len(), ptr::null_mut(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, &mut *st, list.as_ptr(), list.len(), out.as_mut_ptr(), 8, ptr::null_mut()), -1);
        }
    }
}
