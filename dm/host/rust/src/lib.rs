//! The host's Rust code: parts of the host that read data the game's SDK wrote, where a bound the C did not check would read past it. One static library, linked into
//! the host (dm/host/rust.mk); the C side of each module is declared in its header (audren_rs.h for audren: the renderer's work size, and the frame out of each
//! command list). The rules are in docs/development.md, "Rust in the host":
//! unsafe code only in the modules that talk to C, each block with the reason it holds; nothing that allocates, takes a lock or uses thread-local storage (the library
//! is no_std, so it cannot), and no read that the game's data could make panic.
//!
//! The modules that talk to C, the only ones allowed unsafe code: audren_ffi, log, sys.
#![cfg_attr(not(test), no_std)] // the unit tests run with std (cargo test, cargo miri test)

mod audren;
mod audren_ffi;
mod log;
mod sys;
