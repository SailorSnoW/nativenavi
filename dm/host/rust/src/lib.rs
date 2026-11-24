//! The host's Rust code: the parts that read data the game's SDK wrote, with every read bounded. One static library (dm/host/rust.mk); each module's C side is
//! declared in its header (audren_rs.h). Rules: docs/development.md, "Rust in the host".
//!
//! The modules that talk to C, the only ones allowed unsafe code: audren_ffi, log, sys.
#![cfg_attr(not(test), no_std)] // the unit tests run with std (cargo test, cargo miri test)

mod audren;
mod audren_ffi;
mod log;
mod sys;
