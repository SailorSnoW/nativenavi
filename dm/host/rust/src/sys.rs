//! The C library functions the Rust code calls, and what a panic does. The library is no_std (lib.rs): no allocator, no thread-local storage, no unwinding, and none of
//! std's panic machinery (its backtrace printer alone was 0.6 MB of code in the host). A panic, which only a bug can cause (an overflow, a read that a check before it
//! should have refused), logs a red line and aborts, as the C's die() does: the host's crash handler (crash.c) then writes the report.
#![allow(unsafe_code)]

#[cfg(not(miri))]
use core::arch::asm;

// SAFETY: libSystem's abort, with its C prototype (stdlib.h: void abort(void), which does not return)
unsafe extern "C" {
    #[cfg_attr(test, allow(dead_code))]
    safe fn abort() -> !;
}

// The two float operations the resampler needs that core has not (mul_add and rounding are std's, and unstable in core as of Rust 1.95), as the instructions clang emitted
// for the C. libSystem's fmaf and lrintf, called instead, gave the same bits but made the resampler 14 times slower than the C (41.6 against 2.9 µs for a 160-sample frame
// of 8 channels): a call for every tap

/// x * y + z rounded once: fmadd, as clang fused the C's `v += c * x`
#[cfg(not(miri))]
#[inline(always)]
pub(crate) fn fma(x: f32, y: f32, z: f32) -> f32 {
    let r: f32;
    // SAFETY: fmadd reads the three registers named and writes the fourth: no memory, no stack, no condition flags (a float operation sets only the FPSR's cumulative
    // exception bits, as any of the compiler's own does)
    unsafe {
        asm!("fmadd {r:s}, {x:s}, {y:s}, {z:s}", r = lateout(vreg) r, x = in(vreg) x, y = in(vreg) y, z = in(vreg) z, options(pure, nomem, nostack, preserves_flags))
    };
    r
}

/// One tap of the resampler for 8 channels: acc[j] + c × row[j] for each, rounded once, in two 4-lane fused multiply-adds (fmla), as clang's vector code for the C had
/// it. The same bits as fma channel by channel; with fma alone the resampler took 7.3 µs a frame against the C's 2.8
#[cfg(not(miri))]
#[inline(always)]
pub(crate) fn fma8(acc: &mut [f32; 8], c: f32, row: &[i32; 8]) {
    use core::arch::aarch64::{vcvtq_f32_s32, vfmaq_n_f32, vld1q_f32, vld1q_s32, vst1q_f32};
    // SAFETY: NEON is in the baseline of every arm64 Mac (the aarch64-apple-darwin target enables it). The loads read 4 values at row and row + 4, at acc and acc + 4,
    // and the stores write 4 at acc and acc + 4: all inside the two 8-value arrays, which do not overlap (one is borrowed mutably)
    unsafe {
        let (x0, x1) = (vcvtq_f32_s32(vld1q_s32(row.as_ptr())), vcvtq_f32_s32(vld1q_s32(row.as_ptr().add(4))));
        let (a0, a1) = (vld1q_f32(acc.as_ptr()), vld1q_f32(acc.as_ptr().add(4)));
        vst1q_f32(acc.as_mut_ptr(), vfmaq_n_f32(a0, x0, c));
        vst1q_f32(acc.as_mut_ptr().add(4), vfmaq_n_f32(a1, x1, c));
    }
}

/// The C's (int32_t)lrintf(v): frintx rounds in the current rounding mode, fcvtzs converts the integral value to 64 bits (clang's code for lrintf), then cut to 32 bits
#[cfg(not(miri))]
#[inline(always)]
pub(crate) fn lrint(v: f32) -> i32 {
    let r: i64;
    // SAFETY: as fma: two instructions on the registers named (t a scratch register of its own), no memory, stack or condition flags
    unsafe {
        asm!("frintx {t:s}, {v:s}", "fcvtzs {r:x}, {t:s}", v = in(vreg) v, t = out(vreg) _, r = lateout(reg) r, options(pure, nomem, nostack, preserves_flags))
    };
    r as i32
}

// Miri runs the unit tests, which have std, and calls no C: std's fused multiply-add is the same function, and round_ties_even is lrintf in the default rounding mode
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

/// The unwinder's personality routine for Rust frames, which the precompiled compiler_builtins names in its unwind tables even with panic = "abort" (the link failed
/// without it: "_rust_eh_personality" undefined). Nothing unwinds through this library: its functions do not unwind (panic = "abort"), and no C++ exception or foreign
/// unwind crosses them (the host is C and its callers are the C and the game's SDK). An unwinder that asked anyway would be past saving, so it aborts
#[cfg(not(test))]
#[unsafe(no_mangle)]
extern "C" fn rust_eh_personality() -> ! {
    abort()
}
