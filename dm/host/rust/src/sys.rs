//! The C library functions the Rust code calls, and what a panic does. The library is no_std (lib.rs): no allocator, no thread-local storage, no unwinding, and none of
//! std's panic machinery (its backtrace printer alone was 0.6 MB of code in the host). A panic, which only a bug can cause (an overflow, a read that a check before it
//! should have refused), logs a red line and aborts, as the C's die() does: the host's crash handler (crash.c) then writes the report.
#![allow(unsafe_code)]

// SAFETY: libSystem's abort, with its C prototype (stdlib.h: void abort(void), which does not return)
unsafe extern "C" {
    #[cfg_attr(test, allow(dead_code))]
    safe fn abort() -> !;
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
