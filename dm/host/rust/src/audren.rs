//! nn::audio's AudioRenderer, the parts of hle_audren.c that read what the game's SDK wrote: the renderer parameter the game opens with. Safe code only: the raw pointers
//! the C hands over become slices in audren_ffi.rs.

/// The AudioRendererParameter fields the service's sizing reads: eleven 32-bit words from +0 (the voice-drop byte at +0x20 in place of the ninth)
pub(crate) const PARAM_BYTES: usize = 11 * 4;
const WORK_PAGE: usize = 0x1000;

const fn align(n: usize, a: usize) -> usize {
    (n + a - 1) & !(a - 1)
}

/// The work buffer Horizon's renderer service asks of an application, through renderer revision 15, for the parameter `p` of revision `revision` ("REV" and its
/// number). Its command and state representation does not depend on the application's SDK: an older client still uses the service's compact commands, while the SDK's
/// local server sizes its own CPU command structures, so forwarding that size inflates the guest's requirement (hle_audren.c GetAudioRendererWorkBufferSize). Derived
/// from the service's sizing and revision gates and checked against its machine code run in isolation; no firmware code is needed at run time. 0 for a revision this
/// does not know or a count out of bounds: the caller keeps the SDK's requirement.
pub(crate) fn work_size(p: &[u8; PARAM_BYTES], revision: u32) -> usize {
    if revision & 0xff_ffff != 0x56_4552 || !(0x3156_4552..=0x3f56_4552).contains(&revision) {
        return 0;
    }
    let r = (revision >> 24) - 0x30;
    let mut n = [0usize; 11];
    for (i, v) in n.iter_mut().enumerate() {
        if i == 8 {
            *v = usize::from(p[0x20]); // voice drop is a byte; the public struct's padding after it is not initialized
            continue;
        }
        let w = i32::from_le_bytes([p[4 * i], p[4 * i + 1], p[4 * i + 2], p[4 * i + 3]]);
        if !(0..=0x10_0000).contains(&w) {
            return 0; // bounds the arithmetic below, even for a parameter the caller did not check
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

#[cfg(test)]
mod tests {
    use super::*;

    const REV: u32 = 0x3056_4552; // "REV0"; + n << 24 is revision n

    fn param(words: &[u32]) -> [u8; PARAM_BYTES] {
        let mut p = [0; PARAM_BYTES];
        for (i, w) in words.iter().enumerate() {
            p[4 * i..4 * i + 4].copy_from_slice(&w.to_le_bytes());
        }
        p
    }

    /// t_audren_work.c's parameter: 48 kHz, 240 samples, 132 mix buffers, 19 submixes, 138 voices, 1 sink, 16 effects, no performance frames, 85 splitters, 1800 destinations
    const GAME: [u32; 11] = [48000, 240, 132, 19, 138, 1, 16, 0, 0, 85, 1800];

    #[test]
    fn sizes_found_by_running_the_services_sizing() {
        // As t_audren_work.c: expected results were obtained by executing Horizon audio build 3a75b4a6's sizing routine, independently of this implementation
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
        assert_eq!(work_size(&param(&w), REV + (4 << 24)), 0x308000); // fixed command buffer and performance format 1
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
        // overflow-checks are on in every profile: this would stop the test where a sum overflowed
        for revision in 1..=15 {
            let mut p = param(&[0x10_0000; 11]);
            p[0x20] = 0xff;
            assert!(work_size(&p, REV + (revision << 24)) > 0);
        }
    }
}
