//! The host's log (dm.h LOG): a line formatted here into a buffer on the stack, so that logging allocates nothing, and written by the host's dm_log with the "[dm] "
//! prefix that LOG gives the C's lines. The text is the C's own, byte for byte where a line moved from C: the symptom index and the tests search for it.
#![allow(unsafe_code)]

use core::fmt::{self, Write};

/// Longer lines are cut at a character boundary (none of the host's comes near it)
const LINE: usize = 1024;

struct Line {
    buf: [u8; LINE],
    n: usize,
}

impl Write for Line {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let mut take = s.len().min(LINE - 1 - self.n); // the last byte stays for the NUL
        while !s.is_char_boundary(take) {
            take -= 1;
        }
        self.buf[self.n..self.n + take].copy_from_slice(&s.as_bytes()[..take]);
        self.n += take;
        Ok(())
    }
}

/// One line (no newline: dm_log ends it); `log!` is the way to call it
pub(crate) fn line(args: fmt::Arguments<'_>) {
    let mut l = Line { buf: [0; LINE], n: 0 };
    let _ = l.write_fmt(args); // Line never fails; a Display that fails leaves what it wrote
    #[cfg(not(test))]
    {
        use core::ffi::c_char;
        unsafe extern "C" {
            fn dm_log(fmt: *const c_char, ...);
        }
        l.buf[l.n] = 0;
        // SAFETY: dm_log is the host's printf-style logger (dm.h; hle_bigstack.c, or dm.h's weak one in the unit tests). "%s" takes one NUL-terminated string: buf is
        // NUL-terminated at n < LINE (a NUL inside the text only ends the line early)
        unsafe { dm_log(c"[dm] %s".as_ptr(), l.buf.as_ptr().cast::<c_char>()) }
    }
    #[cfg(test)]
    captured::push(&l.buf[..l.n]);
}

macro_rules! log {
    ($($arg:tt)*) => { $crate::log::line(format_args!($($arg)*)) };
}
pub(crate) use log;

/// C's `%#x`: 0x and the hex digits, but a plain 0 for zero
pub(crate) struct Hx<T>(pub T);

impl<T: fmt::LowerHex + Default + PartialEq> fmt::Display for Hx<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.0 == T::default() { f.write_str("0") } else { write!(f, "{:#x}", self.0) }
    }
}

/// Text in a fixed buffer of N bytes, the last kept free as a C string's NUL would be: a write that does not fit whole writes nothing and fails
pub(crate) struct Text<const N: usize> {
    buf: [u8; N],
    n: usize,
}

impl<const N: usize> Text<N> {
    pub(crate) fn new() -> Self {
        Self { buf: [0; N], n: 0 }
    }

    pub(crate) fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.n]).unwrap_or("") // only whole strs are ever written
    }

    /// s appended when it fits whole; false, and nothing written, when it does not
    pub(crate) fn push(&mut self, s: &str) -> bool {
        if self.n + s.len() >= N {
            return false;
        }
        self.buf[self.n..self.n + s.len()].copy_from_slice(s.as_bytes());
        self.n += s.len();
        true
    }
}

impl<const N: usize> Write for Text<N> {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        if self.push(s) { Ok(()) } else { Err(fmt::Error) }
    }
}

/// The lines the code under test logged, per test thread
#[cfg(test)]
pub(crate) mod captured {
    use std::cell::RefCell;

    thread_local! {
        static LINES: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    }

    pub(crate) fn push(line: &[u8]) {
        LINES.with(|l| l.borrow_mut().push(String::from_utf8_lossy(line).into_owned()));
    }

    /// What was logged since the last call, in order
    pub(crate) fn take() -> Vec<String> {
        LINES.with(|l| std::mem::take(&mut *l.borrow_mut()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_long_line_is_cut_at_a_character_boundary() {
        let long = "\u{d7}".repeat(LINE); // two bytes each: the cut falls inside one
        crate::log::log!("{long}");
        let got = captured::take();
        assert_eq!(got.len(), 1);
        assert!(got[0].len() < LINE && got[0].chars().all(|c| c == '\u{d7}'), "{}", got[0].len());
        assert_eq!(got[0].len(), (LINE - 1) / 2 * 2);
    }

    #[test]
    fn hx_is_cs_alternate_hex() {
        assert_eq!(format!("{} {} {}", Hx(0u32), Hx(0x14u8), Hx(0x2e7000usize)), "0 0x14 0x2e7000");
    }

    #[test]
    fn text_takes_whole_writes_only_and_keeps_a_byte_free() {
        let mut t = Text::<8>::new();
        assert!(t.push("abc") && t.push("defg"));
        assert!(!t.push("h")); // 8 bytes would leave none for the NUL
        assert_eq!(t.as_str(), "abcdefg");
        let mut t = Text::<8>::new();
        assert!(t.write_str("\u{d7}\u{d7}\u{d7}\u{d7}").is_err()); // 8 bytes
        assert_eq!(t.as_str(), "");
    }
}
