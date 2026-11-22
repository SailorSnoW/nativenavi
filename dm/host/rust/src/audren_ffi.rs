//! What hle_audren.c calls (declared in dm/host/audren_rs.h): the raw pointers it passes become references and slices here, and the safe code in audren.rs does the work.
#![allow(unsafe_code)]

use crate::audren::{self, Scan, State};
use core::ffi::c_int;
use core::{ptr, slice};

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

/// int dm_audren_capture(dm_audren_scan *scan, dm_audren_state *st, const uint8_t *cl, size_t len, int16_t *dst, size_t dst_n, int *loud): audren::capture of the command
/// list cl..cl+len into dst (dst_n samples of 16 bits). The frame's sample count, *loud 1 when any of them is not 0; -1 when the list gives no frame
///
/// # Safety
/// scan and st are valid, and nothing else uses them during the call; cl..cl+len is readable and dst..dst+dst_n writable, neither overlapping the other or scan and st
#[unsafe(no_mangle)]
pub unsafe extern "C" fn dm_audren_capture(scan: *mut Scan, st: *mut State, cl: *const u8, len: usize, dst: *mut i16, dst_n: usize, loud: *mut c_int) -> c_int {
    // SAFETY: scan is hle_audren.c's g_scan and st the renderer's own ar_t.rs; capture runs under g_render_mu, which every caller holds (render_once), so nothing else
    // touches either during the call. loud is the caller's local
    let (Some(scan), Some(st), Some(loud)) = (unsafe { scan.as_mut() }, unsafe { st.as_mut() }, unsafe { loud.as_mut() }) else { return -1 };
    if cl.is_null() || dst.is_null() {
        return -1;
    }
    // SAFETY: cl..cl+len is the command list GenerateCommand returned (its Span) in the AudioRenderSystem's command buffer, which the next generation rewrites only under
    // g_render_mu: the list stays as it is during the call. dst..dst+dst_n is the caller's output block (the renderer's frame or a catch-up slot of FRAME_MAX samples)
    let (list, dst) = unsafe { (slice::from_raw_parts(cl, len), slice::from_raw_parts_mut(dst, dst_n)) };
    let frame = audren::capture(scan, st, list, dst, |addr, words| {
        let p = ptr::with_exposed_provenance::<i32>(usize::try_from(addr).ok()?);
        if p.is_null() || !p.is_aligned() || words > isize::MAX as usize / size_of::<i32>() {
            return None;
        }
        // SAFETY: the list's header names the renderer's mix buffers, `buffers` of `samples` words at addr, which the SDK exposed when it wrote the address: its
        // ClearMixBufferCommand zeroes that many words there every frame (Ocarina of Time's sdk 22.2.8, +0x259970: memset(mix, 0, count × samples × 4)) and its
        // commands mix into them, so the region is the renderer's memory from its open to its close. Nothing writes it during the call: the frame's commands have run
        // (CommandListProcessor::Process, before capture) and the next generation waits for g_render_mu
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

    /// The C's call: g_scan, the renderer's state, the list, a frame of FRAME_MAX samples
    fn call(scan: &mut Scan, st: &mut State, list: &[u8], dst: &mut [i16]) -> (c_int, c_int) {
        let mut loud = -7;
        // SAFETY: every pointer is to a live Rust object of the length given, none overlapping another
        let n = unsafe { dm_audren_capture(scan, st, list.as_ptr(), list.len(), dst.as_mut_ptr(), dst.len(), &mut loud) };
        (n, loud)
    }

    #[test]
    fn capture_through_the_c_entry_point() {
        // The mix buffers' address goes through the list as the SDK's does: an integer, whose provenance its writer exposed
        let mix: Vec<i32> = (0..8 * 240).map(|w| [3000, -3000][(w / 240).min(1)]).collect();
        let at = mix.as_ptr().expose_provenance() as u64;
        let (mut scan, mut st, mut out) = (new_scan(), new_state(), vec![0i16; FRAME_MAX * SINK_CHANNELS]);
        assert_eq!(call(&mut scan, &mut st, &build(6, 240, 48000, &[0, 1], at), &mut out), (240, 1));
        assert_eq!((out[6], out[7], out[8 * 239 + 6], out[8 * 239 + 7]), (3000, -3000, 3000, -3000));
        let quiet = vec![0i32; 8 * 240];
        let at = quiet.as_ptr().expose_provenance() as u64;
        assert_eq!(call(&mut scan, &mut st, &build(6, 240, 48000, &[0, 1], at), &mut out), (240, 0));
        // A list without the mix buffers' address, or one that is not word-aligned, or an output too small for the frame: no frame
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
        // SAFETY: the entry point checks each pointer for null before it uses any
        unsafe {
            assert_eq!(dm_audren_capture(ptr::null_mut(), &mut *st, list.as_ptr(), list.len(), out.as_mut_ptr(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, ptr::null_mut(), list.as_ptr(), list.len(), out.as_mut_ptr(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, &mut *st, ptr::null(), list.len(), out.as_mut_ptr(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, &mut *st, list.as_ptr(), list.len(), ptr::null_mut(), 8, &mut loud), -1);
            assert_eq!(dm_audren_capture(&mut *scan, &mut *st, list.as_ptr(), list.len(), out.as_mut_ptr(), 8, ptr::null_mut()), -1);
        }
    }
}
