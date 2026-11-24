//! The host's log (dm.h LOG), formatted on the stack. The text is the C's byte for byte: the symptom index and tests search for it.
#![allow(unsafe_code)]

use core::fmt::{self, Write};

/// Longer lines are cut at a character boundary
const LINE: usize = 1024;

struct Line {
    buf: [u8; LINE],
    n: usize,
}

impl Write for Line {
    fn write_str(&mut self, s: &str) -> fmt::Result {
        let mut take = s.len().min(LINE - 1 - self.n); // last byte for the NUL
        while !s.is_char_boundary(take) {
            take -= 1;
        }
        self.buf[self.n..self.n + take].copy_from_slice(&s.as_bytes()[..take]);
        self.n += take;
        Ok(())
    }
}

pub(crate) fn line(args: fmt::Arguments<'_>) {
    let mut l = Line { buf: [0; LINE], n: 0 };
    let _ = l.write_fmt(args);
    #[cfg(not(test))]
    {
        use core::ffi::c_char;
        unsafe extern "C" {
            fn dm_log(fmt: *const c_char, ...);
        }
        l.buf[l.n] = 0;
        // SAFETY: buf is NUL-terminated at n < LINE; "%s" takes one string
        unsafe { dm_log(c"[dm] %s".as_ptr(), l.buf.as_ptr().cast::<c_char>()) }
    }
    #[cfg(test)]
    captured::push(&l.buf[..l.n]);
}

macro_rules! log {
    ($($arg:tt)*) => { $crate::log::line(format_args!($($arg)*)) };
}
pub(crate) use log;

/// C's `%#x` (a plain 0 for zero)
pub(crate) struct Hx<T>(pub T);

impl<T: fmt::LowerHex + Default + PartialEq> fmt::Display for Hx<T> {
    fn fmt(&self, f: &mut fmt::Formatter<'_>) -> fmt::Result {
        if self.0 == T::default() { f.write_str("0") } else { write!(f, "{:#x}", self.0) }
    }
}

/// A write that does not fit whole in N - 1 bytes writes nothing and fails
pub(crate) struct Text<const N: usize> {
    buf: [u8; N],
    n: usize,
}

impl<const N: usize> Text<N> {
    pub(crate) fn new() -> Self {
        Self { buf: [0; N], n: 0 }
    }

    pub(crate) fn as_str(&self) -> &str {
        core::str::from_utf8(&self.buf[..self.n]).unwrap_or("") // only whole strs are written
    }

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

#[cfg(test)]
pub(crate) mod captured {
    use std::cell::RefCell;

    thread_local! {
        static LINES: RefCell<Vec<String>> = const { RefCell::new(Vec::new()) };
    }

    pub(crate) fn push(line: &[u8]) {
        LINES.with(|l| l.borrow_mut().push(String::from_utf8_lossy(line).into_owned()));
    }

    pub(crate) fn take() -> Vec<String> {
        LINES.with(|l| std::mem::take(&mut *l.borrow_mut()))
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn a_long_line_is_cut_at_a_character_boundary() {
        let long = "\u{d7}".repeat(LINE); // two bytes each
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
        assert!(!t.push("h"));
        assert_eq!(t.as_str(), "abcdefg");
        let mut t = Text::<8>::new();
        assert!(t.write_str("\u{d7}\u{d7}\u{d7}\u{d7}").is_err());
        assert_eq!(t.as_str(), "");
    }
}
