//! libc calls, float instructions core lacks, and the panic handler (a red line, then abort, as die()).
#![allow(unsafe_code)]

#[cfg(not(miri))]
use core::arch::asm;

// SAFETY: libSystem's abort, which does not return
unsafe extern "C" {
    #[cfg_attr(test, allow(dead_code))]
    safe fn abort() -> !;
}

// libSystem's fmaf / lrintf give the same bits at 14 times the cost (docs/development.md, "Rust in the host")

/// x * y + z rounded once (fmadd)
#[cfg(not(miri))]
#[inline(always)]
pub(crate) fn fma(x: f32, y: f32, z: f32) -> f32 {
    let r: f32;
    // SAFETY: registers only: no memory, stack or condition flags
    unsafe {
        asm!("fmadd {r:s}, {x:s}, {y:s}, {z:s}", r = lateout(vreg) r, x = in(vreg) x, y = in(vreg) y, z = in(vreg) z, options(pure, nomem, nostack, preserves_flags))
    };
    r
}

/// acc[j] + c × row[j] for 8 channels in two 4-lane fmla: the same bits as fma
#[cfg(not(miri))]
#[inline(always)]
pub(crate) fn fma8(acc: &mut [f32; 8], c: f32, row: &[i32; 8]) {
    use core::arch::aarch64::{vcvtq_f32_s32, vfmaq_n_f32, vld1q_f32, vld1q_s32, vst1q_f32};
    // SAFETY: NEON is baseline on arm64 Macs; the accesses stay inside the two disjoint 8-value arrays
    unsafe {
        let (x0, x1) = (vcvtq_f32_s32(vld1q_s32(row.as_ptr())), vcvtq_f32_s32(vld1q_s32(row.as_ptr().add(4))));
        let (a0, a1) = (vld1q_f32(acc.as_ptr()), vld1q_f32(acc.as_ptr().add(4)));
        vst1q_f32(acc.as_mut_ptr(), vfmaq_n_f32(a0, x0, c));
        vst1q_f32(acc.as_mut_ptr().add(4), vfmaq_n_f32(a1, x1, c));
    }
}

/// (int32_t)lrintf(v): frintx in the current rounding mode, fcvtzs to 64 bits, cut to 32
#[cfg(not(miri))]
#[inline(always)]
pub(crate) fn lrint(v: f32) -> i32 {
    let r: i64;
    // SAFETY: as fma: registers only
    unsafe {
        asm!("frintx {t:s}, {v:s}", "fcvtzs {r:x}, {t:s}", v = in(vreg) v, t = out(vreg) _, r = lateout(reg) r, options(pure, nomem, nostack, preserves_flags))
    };
    r as i32
}

// Miri: std, no C; round_ties_even is lrintf in the default rounding mode
#[cfg(miri)]
pub(crate) fn fma(x: f32, y: f32, z: f32) -> f32 {
    x.mul_add(y, z)
}

#[cfg(miri)]
pub(crate) fn lrint(v: f32) -> i32 {
    v.round_ties_even() as i64 as i32
}

#[cfg(miri)]
pub(crate) fn fma8(acc: &mut [f32; 8], c: f32, row: &[i32; 8]) {
    for (a, &x) in acc.iter_mut().zip(row) {
        *a = c.mul_add(x as f32, *a);
    }
}

#[cfg(not(test))]
#[panic_handler]
fn panic(info: &core::panic::PanicInfo<'_>) -> ! {
    match info.location() {
        Some(at) => crate::log::log!("[RED] FATAL rust: {} ({}:{})", info.message(), at.file(), at.line()),
        None => crate::log::log!("[RED] FATAL rust: {}", info.message()),
    }
    abort()
}

/// compiler_builtins' unwind tables name it even with panic = "abort"; nothing unwinds through here
#[cfg(not(test))]
#[unsafe(no_mangle)]
extern "C" fn rust_eh_personality() -> ! {
    abort()
}
