//! nn::audio's AudioRenderer, the parts that read what the game's SDK wrote: the renderer parameter and each render's command list, out of
//! which the frame is taken. Safe code only: audren_ffi.rs turns the C's raw pointers into slices.

use crate::log::{Hx, Text, log};
use crate::sys;
use core::fmt::Write;

/// eleven 32-bit words from +0 (the voice-drop byte at +0x20 in place of the ninth)
pub(crate) const PARAM_BYTES: usize = 11 * 4;
const WORK_PAGE: usize = 0x1000;

const fn align(n: usize, a: usize) -> usize {
    (n + a - 1) & !(a - 1)
}

/// The work buffer Horizon's renderer service asks for, through revision 15, checked against its machine code; independent of the app's SDK.
/// 0 for an unknown revision or a count out of bounds: the caller keeps the SDK's requirement.
pub(crate) fn work_size(p: &[u8; PARAM_BYTES], revision: u32) -> usize {
    if revision & 0xff_ffff != 0x56_4552 || !(0x3156_4552..=0x3f56_4552).contains(&revision) {
        return 0;
    }
    let r = (revision >> 24) - 0x30;
    let mut n = [0usize; 11];
    for (i, v) in n.iter_mut().enumerate() {
        if i == 8 {
            *v = usize::from(p[0x20]); // a byte; the public struct's padding after it is not initialized
            continue;
        }
        let w = i32::from_le_bytes([p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]]);
        if !(0..=0x10_0000).contains(&w) {
            return 0; // bounds the arithmetic below
        }
        *v = w as usize;
    }
    let [_, samples, mix, sub, voices, sinks, effects, perf, _, splitters, dest] = n;
    let mixes = sub + 1;
    let mut size = align(((sinks + sub) * 0x3c0 + samples * 4) * (mix + 6), 64);
    size += align(mix * 4, 64) + sub * 0x400 + mixes * 0x940 + align(mixes * 8, 16);
    size += voices * 0x1d0 + align(voices * if r >= 15 { 0x238 } else { 0x220 }, 16) + align(voices * 8, 16);
    if r >= 2 {
        let graph = mixes * mixes * 4 + mixes * 12 + 2 * align(mixes, 64) / 8 + align(mixes * mixes, 64) / 8;
        size += align(graph, 16);
        size += splitters * 0x20 + align(dest * if r >= 12 { if r >= 15 { 0x130 } else { 8 } } else { 0xe0 }, 16);
        if r < 5 {
            size += align(dest * 4, 16);
        }
    }
    size += (effects + voices * 4) * 0x20 + 0x50;
    if r >= 12 && splitters != 0 && dest != 0 {
        size += align(dest * if r >= 14 { 0xe0 } else { 0x80 }, 64);
    }
    size = align(size + if r >= 9 { effects * 0x80 } else { 0 }, 64);
    size += (sinks + sub) * 0x280 + effects * if r >= 9 { 0x530 } else { 0x4b0 } + sinks * 0x170 + voices * 0x100 + 0x40;
    if perf != 0 {
        let frame = (sub + voices + sinks + effects + 1) * if r >= 5 { 0x18 } else { 0x10 } + if r >= 5 { 0x990 } else { 0x658 };
        size += align(frame * (perf + 1), 64) + 0xc0;
    }
    let commands = if r >= 5 {
        effects * 0x540
            + sub * 0x3f24
            + sinks * 0x130
            + dest * 0x360
            + voices * (splitters * 0x50 + if r >= 8 { 0x284 } else { 0x250 })
            + (voices + sub + effects + sinks + 0x65) * 0x28
            + 0x2c4
    } else {
        0x18000
    };
    align(size + commands + 0x7e, WORK_PAGE)
}

// Command list (SDK 21 on, CommandListProcessor::Setup sdk+0x25b3f0): header +0x04 count, +0x08 mix buffers, +0x10 their count, +0x14 samples, +0x18 rate, commands
// from +0x20: +0 enabled, +1 ID (u8), +2 size (s16). DeviceSink: ID 0x14, 0x140 bytes, +0x128 input count, +0x12c inputs (s16). Lists may be incomplete: bounds-checked reads.

/// 240 is 5 ms at 48 kHz
pub(crate) const FRAME_MAX: usize = 1024;
/// the six channels, then their stereo fold
pub(crate) const SINK_CHANNELS: usize = 8;
const TAPS: usize = 16;
const LIST_HEADER: usize = 0x20;
const CMD_HEADER: usize = 0x10;
const SINK_SIZE: usize = 0x140;
const SINK_UPSAMPLE: usize = 0x120;
const SINK_INPUTS: usize = 0x128;
const SINK_INDEX: usize = 0x12c;
/// the C makes Scan (DM_AUDREN_SCAN_INIT): only the tests name it
#[cfg(test)]
pub(crate) const SINK_ID: u8 = 0x14;
/// so the 3:2 output fits FRAME_MAX
const MAX_32K: usize = FRAME_MAX * 2 / 3 - 1;

/// zeroed when the renderer opens
#[repr(C)]
pub(crate) struct State {
    up: [i32; SINK_CHANNELS * (TAPS - 1 + FRAME_MAX)],
    up_pos: u32,
    sink_logged: u8,
    _pad: [u8; 3],
}

/// used under hle_audren.c's g_render_mu
#[repr(C)]
pub(crate) struct Scan {
    mixed: [i32; FRAME_MAX * SINK_CHANNELS],
    walked: u8,
    hist_logged: u8,
    sink_id: u8,
    said: u8,
    _pad: [u8; 4],
}

const _: () = assert!(size_of::<State>() == 33256 && size_of::<Scan>() == 32776); // audren_rs.h asserts the same

const SAID_SKIP: u8 = 1;
const SAID_MALFORMED: u8 = 2;
const SAID_RATE: u8 = 4;
const SAID_SHORT: u8 = 8;
const SAID_MIX: u8 = 16;
const SAID_INPUT: u8 = 32;

impl Scan {
    fn once(&mut self, what: u8) -> bool {
        let first = self.said & what == 0;
        self.said |= what;
        first
    }
}

pub(crate) struct Frame {
    pub(crate) samples: usize,
    pub(crate) loud: bool,
}

fn bytes<const N: usize>(b: &[u8], at: usize) -> Option<[u8; N]> {
    b.get(at..)?.get(..N)?.try_into().ok()
}
fn u32_at(b: &[u8], at: usize) -> Option<u32> {
    bytes(b, at).map(u32::from_le_bytes)
}
fn u64_at(b: &[u8], at: usize) -> Option<u64> {
    bytes(b, at).map(u64::from_le_bytes)
}

/// Some(size) when the command is whole inside the list
fn command(list: &[u8], at: usize) -> Option<usize> {
    let rest = list.len().checked_sub(at)?;
    if rest < CMD_HEADER {
        return None;
    }
    let size = usize::try_from(i16::from_le_bytes(bytes(list, at + 2)?)).ok()?;
    (CMD_HEADER..=rest).contains(&size).then_some(size)
}

fn first_bad(list: &[u8], ncmd: u32) -> Option<(u32, usize)> {
    let mut at = LIST_HEADER;
    for i in 0..ncmd {
        match command(list, at) {
            Some(size) => at += size,
            None => return Some((i, at)),
        }
    }
    None
}

/// Per sample six channels, then their stereo fold, clamped to 16 bits. None when the list gives no frame (each case said once)
pub(crate) fn capture<'m>(scan: &mut Scan, st: &mut State, list: &[u8], dst: &mut [i16], mix: impl FnOnce(u64, usize) -> Option<&'m [i32]>) -> Option<Frame> {
    if list.len() < LIST_HEADER {
        return None;
    }
    let ncmd = u32_at(list, 4)?;
    if scan.walked == 0 && ncmd != 0 {
        // a list generated before the game's first update need not be whole: silent, nothing decided from it
        if first_bad(list, ncmd).is_some() {
            if scan.once(SAID_SKIP) {
                log!("audren: command list {ncmd} commands long does not walk (generated before the first update?): silent frame");
            }
            return None;
        }
        scan.walked = 1;
    }
    if scan.walked == 0 {
        return None;
    }
    // every command is checked before the sink scan or a sink gives PCM
    if let Some((i, at)) = first_bad(list, ncmd) {
        if scan.once(SAID_MALFORMED) {
            log!("[RED] audren: command {i} in the command list is malformed (offset {}, list {} bytes)", Hx(at), Hx(list.len()));
        }
        return None;
    }
    if scan.hist_logged == 0 && ncmd > 4 {
        scan.hist_logged = 1;
        histogram(scan, list, ncmd);
    }
    let mut at = LIST_HEADER;
    for _ in 0..ncmd {
        let size = command(list, at)?;
        let cmd = &list[at..at + size];
        if cmd[1] == scan.sink_id && cmd[0] & 1 != 0 {
            return sink(scan, st, list, cmd, dst, mix);
        }
        at += size;
    }
    None
}

/// With no command of the sink's ID, its one command of DeviceSink's size with 1 to 6 inputs is the sink
fn histogram(scan: &mut Scan, list: &[u8], ncmd: u32) {
    let mut hist = [0u32; 64];
    let (mut cand, mut ncand) = (0u8, 0u32);
    let mut at = LIST_HEADER;
    for _ in 0..ncmd {
        let Some(size) = command(list, at) else { return };
        let id = list[at + 1];
        hist[usize::from(id & 63)] += 1;
        if size == SINK_SIZE && u32_at(list, at + SINK_INPUTS).is_some_and(|n| n.wrapping_sub(1) < 6) {
            cand = id;
            ncand += 1;
        }
        at += size;
    }
    let mut line = Text::<512>::new(); // complete entries only
    for (k, &n) in hist.iter().enumerate().filter(|(_, n)| **n != 0) {
        let mut entry = Text::<32>::new();
        let _ = write!(entry, " {k}\u{d7}{n}");
        if !line.push(entry.as_str()) {
            break;
        }
    }
    log!("audren: first-frame commands {ncmd}, by command ID: {}", line.as_str());
    let id = scan.sink_id;
    if hist[usize::from(id & 63)] == 0 && ncand == 1 {
        log!("audren: no command with ID {} in the command list; the only {}-byte command is ID {}, taking it as DeviceSink", Hx(id), Hx(SINK_SIZE), Hx(cand));
        scan.sink_id = cand;
    } else if hist[usize::from(id & 63)] == 0 {
        log!("[RED] audren: no DeviceSink (ID {}) in the command list, {ncand} commands of {} bytes: the game will have no sound", Hx(id), Hx(SINK_SIZE));
    }
}

fn sink<'m>(scan: &mut Scan, st: &mut State, list: &[u8], cmd: &[u8], dst: &mut [i16], mix: impl FnOnce(u64, usize) -> Option<&'m [i32]>) -> Option<Frame> {
    let (mix_at, buffers, samples, rate) = (u64_at(list, 8)?, u32_at(list, 0x10)?, u32_at(list, 0x14)?, u32_at(list, 0x18)?);
    let fields = (u32_at(cmd, SINK_INPUTS), u64_at(cmd, SINK_UPSAMPLE), bytes::<12>(cmd, SINK_INDEX));
    let (Some(nin), Some(up_at), Some(index)) = fields else {
        if scan.once(SAID_SHORT) {
            log!("[RED] audren: the DeviceSink command (ID {}) is {} bytes, too short for its inputs: no sound", Hx(scan.sink_id), Hx(cmd.len()));
        }
        return None;
    };
    let input: [i16; 6] = core::array::from_fn(|j| i16::from_le_bytes([index[2 * j], index[2 * j + 1]]));
    if st.sink_logged == 0 {
        st.sink_logged = 1;
        let first = |j: usize| if nin as usize > j { i32::from(input[j]) } else { -1 };
        log!(
            "audren: DeviceSink inputs {nin} ({} {} \u{2026}) rate {rate} samples per frame {samples} mix buffer {mix_at:#x} ({buffers} buffers) upsample buffer {up_at:#x}{}",
            first(0),
            first(1),
            if rate == 32000 { " (32 kHz: resampled to 48 kHz on the host)" } else { "" }
        );
    }
    let samples = samples as usize;
    if nin == 0 || samples > if rate == 48000 { FRAME_MAX } else { MAX_32K } {
        return None;
    }
    if rate != 48000 && rate != 32000 {
        if scan.once(SAID_RATE) {
            log!("[RED] audren: renderer sample rate {rate} (the SDK takes 32000 or 48000): no sound");
        }
        return None;
    }
    // at the renderer's rate: with CPU rendering the SDK makes no upsampler
    let Some(words) = (buffers as usize).checked_mul(samples).and_then(|w| mix(mix_at, w)) else {
        if scan.once(SAID_MIX) {
            log!("[RED] audren: the command list's {buffers} mix buffers of {samples} samples at {mix_at:#x} cannot be read: no sound");
        }
        return None;
    };
    let k = (nin as usize).min(6);
    let mut ch: [&[i32]; 6] = [&[]; 6];
    for j in 0..k {
        let Some(c) = usize::try_from(input[j]).ok().filter(|&b| b < buffers as usize).and_then(|b| words.get(b * samples..(b + 1) * samples)) else {
            if scan.once(SAID_INPUT) {
                log!("[RED] audren: DeviceSink input {j} names mix buffer {}, of {buffers}: no sound", input[j]);
            }
            return None;
        };
        ch[j] = c;
    }
    // FL FR C LFE BL BR; 6 -> 2 with the default coefficients (front 1.0, center 0.707, LFE 0.251, back 0.707, Q15). The fold is mixed
    // before the resampler and clamped last: folding clamped samples loses cancellation between channels.
    const CF: i64 = 32768;
    const CC: i64 = 23167;
    const CL: i64 = 8224;
    const CB: i64 = 23167;
    for (s, f) in scan.mixed.chunks_exact_mut(SINK_CHANNELS).take(samples).enumerate() {
        let x: [i64; 6] = core::array::from_fn(|j| if j < k { i64::from(ch[j][s]) } else { 0 });
        if k >= 6 {
            for j in 0..6 {
                f[j] = x[j] as i32;
            }
            let c = x[2] * CC + x[3] * CL;
            f[6] = ((x[0] * CF + c + x[4] * CB) >> 15) as i32;
            f[7] = ((x[1] * CF + c + x[5] * CB) >> 15) as i32;
        } else {
            f[0] = x[0] as i32;
            f[1] = (if k > 1 { x[1] } else { x[0] }) as i32;
            f[2..6].fill(0);
            f[6] = f[0];
            f[7] = f[1];
        }
    }
    let nout = if rate == 48000 { samples } else { up_3_2(st, &mut scan.mixed, samples, SINK_CHANNELS) };
    let out = dst.get_mut(..nout * SINK_CHANNELS)?;
    let mut loud = false;
    for (d, &v) in out.iter_mut().zip(scan.mixed.iter()) {
        let v = v.clamp(-32768, 32767);
        *d = v as i16;
        loud |= v != 0;
    }
    Some(Frame { samples: nout, loud })
}

/// Windowed sinc (Blackman, cutoff 0.9 of the input Nyquist), three phases of 16 taps normalized to 1; fixed bits as clang computed them
/// (the golden PCM depends on them; coefficients_follow_the_formula recomputes them)
const H: [[f32; TAPS]; 3] = [
    [
        f32::from_bits(0x3a0d1547),
        f32::from_bits(0xbb5bb910),
        f32::from_bits(0x3c3380d8),
        f32::from_bits(0xbcd2ce4d),
        f32::from_bits(0x3d4310a2),
        f32::from_bits(0xbd9435ab),
        f32::from_bits(0x3dbd1166),
        f32::from_bits(0x3f6668c3),
        f32::from_bits(0x3dbd1166),
        f32::from_bits(0xbd9435ab),
        f32::from_bits(0x3d4310a2),
        f32::from_bits(0xbcd2ce4d),
        f32::from_bits(0x3c3380d8),
        f32::from_bits(0xbb5bb910),
        f32::from_bits(0x3a0d1547),
        f32::from_bits(0x20bf988b),
    ],
    [
        f32::from_bits(0x3988c7ab),
        f32::from_bits(0xbaeaac5e),
        f32::from_bits(0x3b9573f1),
        f32::from_bits(0xbbce65f7),
        f32::from_bits(0xa494f844),
        f32::from_bits(0x3cf30789),
        f32::from_bits(0xbe005484),
        f32::from_bits(0x3f446846),
        f32::from_bits(0x3ee210c6),
        f32::from_bits(0xbe23c82e),
        f32::from_bits(0x3d927cae),
        f32::from_bits(0xbceac167),
        f32::from_bits(0x3c1162ad),
        f32::from_bits(0xbad7f73c),
        f32::from_bits(0xa18bbcda),
        f32::from_bits(0x37a702f4),
    ],
    [
        f32::from_bits(0x37a702f4),
        f32::from_bits(0xa18bbcda),
        f32::from_bits(0xbad7f73c),
        f32::from_bits(0x3c1162ad),
        f32::from_bits(0xbceac167),
        f32::from_bits(0x3d927cae),
        f32::from_bits(0xbe23c82e),
        f32::from_bits(0x3ee210c6),
        f32::from_bits(0x3f446846),
        f32::from_bits(0xbe005484),
        f32::from_bits(0x3cf30789),
        f32::from_bits(0xa494f844),
        f32::from_bits(0xbbce65f7),
        f32::from_bits(0x3b9573f1),
        f32::from_bits(0xbaeaac5e),
        f32::from_bits(0x3988c7ab),
    ],
];

/// 3:2 upsampling in place (io holds FRAME_MAX samples), 8 input samples late so every tap is known; returns the output count
fn up_3_2(st: &mut State, io: &mut [i32], n: usize, nch: usize) -> usize {
    let hist = (TAPS - 1) * nch;
    st.up[hist..hist + n * nch].copy_from_slice(&io[..n * nch]); // copied first: the output goes over the input
    let mut pos = st.up_pos as usize; // in thirds of an input sample
    assert!(nch <= SINK_CHANNELS);
    let mut m = 0;
    while pos < 3 * n {
        let x = &st.up[nch * (pos / 3)..][..TAPS * nch];
        // one rounding per tap in order (the golden bits), channels side by side
        let mut v = [0f32; SINK_CHANNELS];
        for (row, &c) in x.chunks_exact(nch).zip(&H[pos % 3]) {
            match <&[i32; SINK_CHANNELS]>::try_from(row) {
                Ok(row) => sys::fma8(&mut v, c, row),
                Err(_) => {
                    for (v, &x) in v.iter_mut().zip(row) {
                        *v = sys::fma(c, x as f32, *v);
                    }
                }
            }
        }
        for (out, &v) in io[nch * m..nch * (m + 1)].iter_mut().zip(&v) {
            *out = sys::lrint(v);
        }
        pos += 2;
        m += 1;
    }
    st.up_pos = (pos - 3 * n) as u32;
    st.up.copy_within(nch * n..nch * n + hist, 0);
    m
}

#[cfg(test)]
pub(crate) mod tests {
    use super::*;

    const REV: u32 = 0x3056_4552; // "REV0"; + n << 24 is revision n

    fn param(words: &[u32]) -> [u8; PARAM_BYTES] {
        let mut p = [0; PARAM_BYTES];
        for (i, w) in words.iter().enumerate() {
            p[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
        }
        p
    }

    /// t_audren_work.c's parameter
    const GAME: [u32; 11] = [48000, 240, 132, 19, 138, 1, 16, 0, 0, 85, 1800];

    #[test]
    fn sizes_found_by_running_the_services_sizing() {
        // expected results from Horizon's own sizing routine (audio build 3a75b4a6)
        let cases = [
            (8, 4, 138, 0x61d000),
            (8, 4, 137, 0x61b000),
            (8, 3, 18, 0x5f7000),
            (8, 10, 1799, 0x61c000),
            (8, 7, 2, 0x622000),
            (9, 4, 138, 0x61e000),
            (12, 4, 138, 0x5f7000),
            (14, 4, 138, 0x621000),
            (15, 4, 138, 0x6a4000),
        ];
        for (revision, word, value, expected) in cases {
            let mut w = GAME;
            w[word] = value;
            assert_eq!(work_size(&param(&w), REV + (revision << 24)), expected, "revision {revision}, word {word} = {value}");
        }
        let mut w = GAME;
        w[9] = 0;
        w[10] = 0;
        assert_eq!(work_size(&param(&w), REV + (4 << 24)), 0x308000);
    }

    #[test]
    fn the_padding_after_voice_drop_is_not_read() {
        let mut p = param(&GAME);
        p[0x21..0x24].fill(0xff);
        assert_eq!(work_size(&p, REV + (8 << 24)), 0x61d000);
    }

    #[test]
    fn unknown_revisions_and_counts_out_of_bounds_give_0() {
        let p = param(&GAME);
        for revision in [0, REV, REV + (16 << 24), 0x3156_4553, 0x3156_4452, u32::MAX] {
            assert_eq!(work_size(&p, revision), 0, "{revision:#x}");
        }
        for word in (0..11).filter(|&w| w != 8) {
            for value in [0x10_0001, 0x8000_0000, u32::MAX] {
                let mut w = GAME;
                w[word] = value;
                assert_eq!(work_size(&param(&w), REV + (15 << 24)), 0, "word {word} = {value:#x}");
            }
        }
    }

    #[test]
    fn the_largest_counts_do_not_overflow() {
        // overflow-checks are on in every profile
        for revision in 1..=15 {
            let mut p = param(&[0x10_0000; 11]);
            p[0x20] = 0xff;
            assert!(work_size(&p, REV + (revision << 24)) > 0);
        }
    }

    use crate::log::captured;

    const SAMPLES: usize = 240;
    const NMIX: usize = 8;
    const NCMD: usize = 6;

    pub(crate) fn new_scan() -> Box<Scan> {
        Box::new(Scan { mixed: [0; FRAME_MAX * SINK_CHANNELS], walked: 0, hist_logged: 0, sink_id: SINK_ID, said: 0, _pad: [0; 4] })
    }
    pub(crate) fn new_state() -> Box<State> {
        Box::new(State { up: [0; SINK_CHANNELS * (TAPS - 1 + FRAME_MAX)], up_pos: 0, sink_logged: 0, _pad: [0; 3] })
    }
    fn out() -> Vec<i16> {
        vec![0; FRAME_MAX * SINK_CHANNELS]
    }

    fn put(list: &mut [u8], at: usize, id: u8, size: usize) -> usize {
        list[at..at + size].fill(0);
        list[at] = 1;
        list[at + 1] = id;
        list[at + 2..at + 4].copy_from_slice(&(size as i16).to_le_bytes());
        at + size
    }

    pub(crate) fn build(ncmd: usize, samples: usize, rate: u32, inputs: &[i16], mix_at: u64) -> Vec<u8> {
        let mut l = vec![0u8; LIST_HEADER + (ncmd - 1) * 0x40 + SINK_SIZE];
        l[4..8].copy_from_slice(&(ncmd as u32).to_le_bytes());
        l[8..16].copy_from_slice(&mix_at.to_le_bytes());
        l[0x10..0x14].copy_from_slice(&(NMIX as u32).to_le_bytes());
        l[0x14..0x18].copy_from_slice(&(samples as u32).to_le_bytes());
        l[0x18..0x1c].copy_from_slice(&rate.to_le_bytes());
        let mut at = LIST_HEADER;
        for _ in 0..ncmd - 1 {
            at = put(&mut l, at, 6, 0x40);
        }
        let sink = at;
        put(&mut l, at, SINK_ID, SINK_SIZE);
        l[sink + SINK_INPUTS..sink + SINK_INPUTS + 4].copy_from_slice(&(inputs.len() as u32).to_le_bytes());
        for (j, i) in inputs.iter().enumerate() {
            l[sink + SINK_INDEX + 2 * j..sink + SINK_INDEX + 2 * j + 2].copy_from_slice(&i.to_le_bytes());
        }
        l
    }
    fn stereo(ncmd: usize) -> Vec<u8> {
        build(ncmd, SAMPLES, 48000, &[0, 1], 0x1000)
    }

    fn run(scan: &mut Scan, st: &mut State, list: &[u8], mix: &[i32], dst: &mut [i16]) -> Option<usize> {
        capture(scan, st, list, dst, |_, words| mix.get(..words)).map(|f| f.samples)
    }
    fn mix_of(per_buffer: impl Fn(usize, usize) -> i32) -> Vec<i32> {
        (0..NMIX * SAMPLES).map(|w| per_buffer(w / SAMPLES, w % SAMPLES)).collect()
    }
    fn said(lines: &[String], text: &str) -> bool {
        lines.iter().any(|l| l.contains(text))
    }

    #[test]
    fn commands_are_whole_or_refused() {
        let mut wire = vec![0u8; 0x41];
        put(&mut wire, 1, 0x14, 0x40); // an odd offset: no typed load
        let q = &wire[1..];
        assert_eq!(command(q, 0), Some(0x40));
        for n in 0..0x40 {
            assert_eq!(command(&q[..n], 0), None, "truncated to {n}");
        }
        for bad in [0i16, -1, 4, 0xf, 0x41] {
            wire[3..5].copy_from_slice(&bad.to_le_bytes());
            assert_eq!(command(&wire[1..], 0), None, "size {bad}");
        }
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        let list = stereo(NCMD);
        for n in 0..LIST_HEADER {
            assert_eq!(run(&mut scan, &mut st, &list[..n], &[], &mut o), None);
        }
    }

    #[test]
    fn startup_lists_decide_nothing_until_one_walks() {
        let mix = mix_of(|b, _| [1000, -1000][b.min(1)]);
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        let mut empty = stereo(NCMD);
        empty[4..8].fill(0);
        assert_eq!(run(&mut scan, &mut st, &empty[..LIST_HEADER], &mix, &mut o), None);
        // as before the game's first update: silent, not malformed
        let mut incomplete = stereo(NCMD);
        incomplete[LIST_HEADER + 2 * 0x40..LIST_HEADER + 3 * 0x40].fill(0);
        captured::take();
        for _ in 0..3 {
            assert_eq!(run(&mut scan, &mut st, &incomplete, &mix, &mut o), None);
        }
        let lines = captured::take();
        assert!(said(&lines, "does not walk") && !said(&lines, "[RED]") && !said(&lines, "by command ID:"), "{lines:?}");
        assert_eq!(lines.len(), 1);
        // the SDK starts with short valid lists
        assert_eq!(run(&mut scan, &mut st, &stereo(2), &mix, &mut o), Some(SAMPLES));
        assert_eq!(scan.sink_id, SINK_ID);
        for s in 0..SAMPLES {
            assert_eq!((o[8 * s + 6], o[8 * s + 7]), (1000, -1000));
        }
        assert_eq!(run(&mut scan, &mut st, &stereo(NCMD), &mix, &mut o), Some(SAMPLES));
    }

    #[test]
    fn a_sink_under_another_id_is_found_by_its_size() {
        let mix = mix_of(|b, _| [1000, -1000][b.min(1)]);
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        let mut list = stereo(NCMD);
        list[LIST_HEADER + (NCMD - 1) * 0x40 + 1] = 0x15;
        captured::take();
        assert_eq!(run(&mut scan, &mut st, &list, &mix, &mut o), Some(SAMPLES));
        let lines = captured::take();
        assert!(scan.sink_id == 0x15 && said(&lines, "taking it as DeviceSink") && !said(&lines, "[RED]"), "{lines:?}");
        assert_eq!((o[6], o[7]), (1000, -1000));
    }

    #[test]
    fn malformed_lists_after_one_walks_give_nothing() {
        for bad in 0..3 {
            let mix = mix_of(|b, _| [700, -900][b.min(1)]);
            let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
            let mut first = stereo(NCMD);
            first[4..8].copy_from_slice(&1u32.to_le_bytes());
            assert_eq!(run(&mut scan, &mut st, &first[..LIST_HEADER + 0x40], &mix, &mut o), None);
            let mut list = stereo(NCMD);
            if bad < 2 {
                list[LIST_HEADER + bad * 0x40..LIST_HEADER + (bad + 1) * 0x40].fill(0);
            } else {
                list[4..8].copy_from_slice(&(NCMD as u32 + 1).to_le_bytes());
                list.extend([0u8; 0x40]);
            }
            o.fill(1234);
            captured::take();
            assert_eq!(run(&mut scan, &mut st, &list, &mix, &mut o), None, "bad {bad}");
            let lines = captured::take();
            assert!(said(&lines, "malformed") && !said(&lines, "by command ID:") && scan.sink_id == SINK_ID, "{lines:?}");
            assert!(o.iter().all(|&v| v == 1234));
            assert_eq!(run(&mut scan, &mut st, &stereo(NCMD), &mix, &mut o), Some(SAMPLES));
            assert_eq!((o[6], o[7]), (700, -900));
            let mut tail = stereo(NCMD);
            tail[4..8].copy_from_slice(&(NCMD as u32 + 1).to_le_bytes());
            tail.extend([0u8; 0x40]);
            assert_eq!(run(&mut scan, &mut st, &tail, &mix, &mut o), None);
        }
    }

    #[test]
    #[cfg_attr(miri, ignore)] // slow under Miri
    fn the_histogram_keeps_whole_entries_within_its_buffer() {
        // 64 IDs × 1000 commands overflow the 512-byte histogram text
        const COUNT: usize = 64000;
        let mut l = vec![0u8; LIST_HEADER + COUNT * 0x10];
        l[4..8].copy_from_slice(&(COUNT as u32).to_le_bytes());
        for i in 0..COUNT {
            let at = put(&mut l, LIST_HEADER + i * 0x10, (i / 1000) as u8, 0x10) - 0x10;
            l[at] = 0;
        }
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        captured::take();
        assert_eq!(run(&mut scan, &mut st, &l, &[], &mut o), None);
        let lines = captured::take();
        let hist = lines.iter().find_map(|l| l.split_once("by command ID:")).map(|(_, h)| h.to_owned()).unwrap();
        assert!(hist.len() <= 512 && hist.starts_with("  0\u{d7}1000 1\u{d7}1000"), "{} bytes: {hist}", hist.len());
    }

    #[test]
    fn six_channels_in_order_and_a_mono_sink_on_the_front_pair() {
        let mut mix = mix_of(|b, _| 1000 * (b as i32 + 1));
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, SAMPLES, 48000, &[0, 1, 2, 3, 4, 5], 0x1000), &mix, &mut o), Some(SAMPLES));
        for s in 0..SAMPLES {
            for j in 0..6 {
                assert_eq!(o[8 * s + j], 1000 * (j as i16 + 1));
            }
        }
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, SAMPLES, 48000, &[0], 0x1000), &mix, &mut o), Some(SAMPLES));
        for s in 0..SAMPLES {
            assert_eq!(&o[8 * s..8 * s + 8], &[1000, 1000, 0, 0, 0, 0, 1000, 1000]);
        }
        // a center above 16 bits must cancel the front before the fold is clamped
        mix = mix_of(|b, _| [-30000, 0, 60000, 0, 0, 0, 0, 0][b]);
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, SAMPLES, 48000, &[0, 1, 2, 3, 4, 5], 0x1000), &mix, &mut o), Some(SAMPLES));
        assert_eq!((o[0], o[2], o[6], o[7]), (-30000, 32767, 12420, 32767));
    }

    /// t_audren_layout.c test_stereo_unchanged's golden PCM, at 48 and 32 kHz
    #[test]
    fn stereo_matches_the_golden_output() {
        let golden = [0xb2c9_1fe3_77e1_ac44u64, 0x9dfe_f1ca_9df2_4ea6];
        for (r, (rate, count)) in [(48000u32, 240usize), (32000, 160)].into_iter().enumerate() {
            let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
            let mut hash = 14_695_981_039_346_656_037u64;
            for frame in 0..3 {
                let mut mix = vec![0i32; NMIX * SAMPLES];
                for c in 0..6 {
                    for i in 0..count {
                        mix[c * count + i] = ((frame * 977 + c * 7919 + i * 613) % 120_001) as i32 - 60000;
                    }
                }
                let list = build(NCMD, count, rate, &[0, 1, 2, 3, 4, 5], 0x1000);
                assert_eq!(run(&mut scan, &mut st, &list, &mix, &mut o), Some(240));
                for i in 0..240 {
                    for j in 6..8 {
                        let v = o[i * 8 + j] as u16;
                        hash = (hash ^ u64::from(v & 255)).wrapping_mul(1_099_511_628_211);
                        hash = (hash ^ u64::from(v >> 8)).wrapping_mul(1_099_511_628_211);
                    }
                }
            }
            assert_eq!(hash, golden[r], "{rate} Hz");
        }
    }

    #[test]
    fn each_renderer_resamples_with_its_own_history() {
        let (mut a, mut b) = (new_state(), new_state());
        let mut io = vec![20000i32; FRAME_MAX * SINK_CHANNELS];
        assert_eq!(up_3_2(&mut a, &mut io, 160, 2), 240);
        io.fill(0);
        assert_eq!(up_3_2(&mut b, &mut io, 160, 2), 240);
        assert!(io[..2 * 240].iter().all(|&v| v == 0));
        let mut c = new_state();
        let mut six = vec![0i32; FRAME_MAX * SINK_CHANNELS];
        for i in 0..160 {
            six[6 * i + 3] = 20000;
        }
        assert_eq!(up_3_2(&mut c, &mut six, 160, 6), 240);
        assert!((0..240).all(|m| (0..6).all(|j| j == 3 || six[6 * m + j] == 0)));
        assert_eq!(six[6 * 100 + 3], 20000);
    }

    #[test]
    fn a_sink_input_outside_the_mix_buffers_gives_no_sound() {
        let mix = mix_of(|_, _| 1000);
        for bad in [NMIX as i16, -1, i16::MAX] {
            let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
            captured::take();
            assert_eq!(run(&mut scan, &mut st, &build(NCMD, SAMPLES, 48000, &[0, bad], 0x1000), &mix, &mut o), None, "index {bad}");
            let lines = captured::take();
            assert!(said(&lines, &format!("[RED] audren: DeviceSink input 1 names mix buffer {bad}, of 8: no sound")), "{lines:?}");
            assert_eq!(run(&mut scan, &mut st, &build(NCMD, SAMPLES, 48000, &[0, bad], 0x1000), &mix, &mut o), None);
            assert!(!said(&captured::take(), "[RED]"));
        }
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        captured::take();
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, SAMPLES, 48000, &[0, 1], 0x1000), &mix[..NMIX * SAMPLES - 1], &mut o), None);
        assert!(said(&captured::take(), "mix buffers of 240 samples at 0x1000 cannot be read"));
    }

    #[test]
    fn a_command_with_the_sinks_id_too_short_for_its_fields_is_not_read() {
        let mix = mix_of(|_, _| 1000);
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        let mut l = vec![0u8; LIST_HEADER + 0x40];
        l[4..8].copy_from_slice(&1u32.to_le_bytes());
        l[0x10..0x14].copy_from_slice(&(NMIX as u32).to_le_bytes());
        l[0x14..0x18].copy_from_slice(&(SAMPLES as u32).to_le_bytes());
        l[0x18..0x1c].copy_from_slice(&48000u32.to_le_bytes());
        put(&mut l, LIST_HEADER, SINK_ID, 0x40);
        captured::take();
        assert_eq!(run(&mut scan, &mut st, &l, &mix, &mut o), None);
        assert!(said(&captured::take(), "[RED] audren: the DeviceSink command (ID 0x14) is 0x40 bytes, too short for its inputs: no sound"));
    }

    #[test]
    fn rates_and_sample_counts_out_of_range_give_no_frame() {
        let mix = vec![0i32; NMIX * FRAME_MAX];
        let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
        captured::take();
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, 240, 44100, &[0, 1], 0x1000), &mix, &mut o), None);
        assert!(said(&captured::take(), "[RED] audren: renderer sample rate 44100 (the SDK takes 32000 or 48000): no sound"));
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, FRAME_MAX + 1, 48000, &[0, 1], 0x1000), &mix, &mut o), None);
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, MAX_32K + 1, 32000, &[0, 1], 0x1000), &mix, &mut o), None);
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, FRAME_MAX, 48000, &[0, 1], 0x1000), &mix, &mut o), Some(FRAME_MAX));
        assert!(run(&mut scan, &mut st, &build(NCMD, MAX_32K, 32000, &[0, 1], 0x1000), &mix, &mut o).is_some_and(|n| n <= FRAME_MAX));
        assert_eq!(run(&mut scan, &mut st, &build(NCMD, 240, 48000, &[], 0x1000), &mix, &mut o), None);
    }

    /// random lists and edits: capture refuses or gives a frame, never panics
    #[test]
    fn random_lists_never_panic() {
        let mut seed = 0x9e37_79b9_7f4a_7c15u64;
        let mut next = move || {
            seed ^= seed << 13;
            seed ^= seed >> 7;
            seed ^= seed << 17;
            seed
        };
        let mix = vec![0x7fff_ffffi32; NMIX * FRAME_MAX];
        let rounds = if cfg!(miri) { 40 } else { 20000 };
        for round in 0..rounds {
            let (mut scan, mut st, mut o) = (new_scan(), new_state(), out());
            scan.walked = (round & 1) as u8;
            let rate = [48000, 32000, 44100, 0][next() as usize % 4];
            let mut l = build(1 + next() as usize % 8, next() as usize % (FRAME_MAX + 2), rate, &[0, 1, 2, 3, 4, 5], 0x1000);
            for _ in 0..next() % 16 {
                let at = next() as usize % l.len();
                l[at] = next() as u8;
            }
            let cut = next() as usize % (l.len() + 1);
            if round % 3 == 0 {
                l.truncate(cut);
            }
            let _ = run(&mut scan, &mut st, &l, &mix, &mut o);
            let noise: Vec<u8> = (0..next() % 0x400).map(|_| next() as u8).collect();
            let _ = run(&mut scan, &mut st, &noise, &mix, &mut o);
        }
        captured::take();
    }

    /// recomputes H, with the two fused multiply-adds clang made in the window
    #[test]
    #[cfg_attr(miri, ignore)] // Miri's sin and cos are not the system's
    fn coefficients_follow_the_formula() {
        use std::f64::consts::PI;
        for (p, phase) in H.iter().enumerate() {
            let (mut w, mut sum) = ([0f64; TAPS], 0f64);
            for (k, wk) in w.iter_mut().enumerate() {
                let x = p as f64 / 3.0 + (TAPS / 2) as f64 - 1.0 - k as f64;
                let fc = 0.9;
                let s = if x == 0.0 { fc } else { (PI * fc * x).sin() / (PI * x) };
                let t = (x + (TAPS / 2) as f64) / TAPS as f64;
                let win = (4.0 * PI * t).cos().mul_add(0.08, (2.0 * PI * t).cos().mul_add(-0.5, 0.42));
                *wk = s * win;
                sum += *wk;
            }
            for (k, (wk, h)) in w.iter().zip(phase).enumerate() {
                assert_eq!(((wk / sum) as f32).to_bits(), h.to_bits(), "phase {p} tap {k}");
            }
        }
    }
}
