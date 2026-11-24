#!/usr/bin/env python3
"""Where an NSO's text holds code: function ranges from .eh_frame, and data blocks no function covers (at least DATA_MIN bytes where over a quarter
of the words do not decode): a text may start with megabytes of data whose words read as x18 instructions. Usage on its own: nso_code.py <module> (lists the data blocks)"""
import struct

DATA_MIN = 0x1000


def eh_frame_hdr(img):
    m0 = struct.unpack_from('<I', img, 4)[0]
    assert img[m0:m0 + 4] == b'MOD0', 'no MOD0'
    return m0 + struct.unpack_from('<i', img, m0 + 16)[0], m0 + struct.unpack_from('<i', img, m0 + 20)[0]


def _read(img, o, enc, ehs):
    fmt, app = enc & 0xF, enc & 0x70
    if fmt == 0x1:
        v, end = _uleb(img, o)
        sz = end - o
    elif fmt == 0x3: v, sz = struct.unpack_from('<I', img, o)[0], 4
    elif fmt == 0xB: v, sz = struct.unpack_from('<i', img, o)[0], 4
    elif fmt in (0x0, 0x4): v, sz = struct.unpack_from('<Q', img, o)[0], 8
    elif fmt == 0xC: v, sz = struct.unpack_from('<q', img, o)[0], 8
    else: raise ValueError(f'pointer encoding {enc:#x} at {o:#x}')
    if app == 0x10: v += o
    elif app == 0x30: v += ehs
    return v, sz


def _uleb(img, o):
    r = sh = 0
    while True:
        b = img[o]
        o += 1
        r |= (b & 0x7F) << sh
        sh += 7
        if not b & 0x80: return r, o


def fde_table(img):
    """Empty when the module has no valid eh_frame_hdr (a 32-bit module)"""
    ehs, ehe = eh_frame_hdr(img)
    if ehe - ehs < 12 or img[ehs] != 1 or img[ehs + 3] in (0xFF, 0): return []
    _, penc, cenc, tenc = img[ehs:ehs + 4]
    o = ehs + 4
    _, s = _read(img, o, penc, ehs); o += s
    n, s = _read(img, o, cenc, ehs); o += s
    out = []
    for _ in range(n):
        a, s = _read(img, o, tenc, ehs); o += s
        f, s = _read(img, o, tenc, ehs); o += s
        out.append((a, f))
    return out


def _cie(img, c, ehs):
    """(FDE pointer encoding, LSDA encoding or None, whether it names a personality routine)"""
    p = c + 8
    ver = img[p]
    p += 1
    aug = img[p:img.index(b'\0', p)]
    p += len(aug) + 1
    _, p = _uleb(img, p)
    while img[p] & 0x80: p += 1
    p += 1
    p = p + 1 if ver == 1 else _uleb(img, p)[1]
    enc, lenc = 0, None
    if aug.startswith(b'z'):
        _, p = _uleb(img, p)
        for ch in aug[1:]:
            if ch == ord('R'):
                enc = img[p]
                p += 1
            elif ch == ord('P'):
                pe = img[p]
                p += 1
                p += _read(img, p, pe, ehs)[1]
            elif ch == ord('L'):
                lenc = img[p]
                p += 1
    return enc, lenc, aug.startswith(b'z') and b'P' in aug


def fde_ranges(img):
    ehs, _ = eh_frame_hdr(img)
    cies = {}
    out = []
    for _, f in fde_table(img):
        cptr = f + 4
        c = cptr - struct.unpack_from('<I', img, cptr)[0]
        if c not in cies: cies[c] = _cie(img, c, ehs)
        enc = cies[c][0]
        st, s = _read(img, f + 8, enc, ehs)
        ln, _ = _read(img, f + 8 + s, enc & 0xF, ehs)
        out.append((st, st + ln))
    return sorted(out)


def landing_pads(img):
    """{landing pad: [(start, end) of the calls it catches]} from the LSDAs. Only a CIE with a personality routine ('P') has such an LSDA ('L' alone: the runtime's own data)"""
    ehs, _ = eh_frame_hdr(img)
    cies, out = {}, {}
    for _, f in fde_table(img):
        try:
            cptr = f + 4
            c = cptr - struct.unpack_from('<I', img, cptr)[0]
            if c not in cies: cies[c] = _cie(img, c, ehs)
            enc, lenc, z = cies[c]
            if lenc is None or lenc == 0xFF or not z: continue
            st, s = _read(img, f + 8, enc, ehs)
            p = f + 8 + s
            ln, s = _read(img, p, enc & 0xF, ehs)
            p += s
            _, p = _uleb(img, p)
            lsda, _ = _read(img, p, lenc, ehs)
            if not lsda: continue
            p = lsda
            lpe = img[p]
            p += 1
            lpstart = st
            if lpe != 0xFF:
                lpstart, s = _read(img, p, lpe, ehs)
                p += s
            tte = img[p]
            p += 1
            if tte != 0xFF: _, p = _uleb(img, p)
            cse = img[p]
            p += 1
            n, p = _uleb(img, p)
            end = p + n
            got = []
            while p < end:
                cs, s = _read(img, p, cse & 0xF, ehs); p += s
                cl, s = _read(img, p, cse & 0xF, ehs); p += s
                lp, s = _read(img, p, cse & 0xF, ehs); p += s
                _, p = _uleb(img, p)
                if cs + cl > ln or (lp and not st <= lpstart + lp < st + ln): raise ValueError('a call site outside its function')
                if lp: got.append((lpstart + lp, (st + cs, st + cs + cl)))
            for pad, r in got: out.setdefault(pad, []).append(r)
        except (ValueError, IndexError, struct.error): continue   # an unknown table: its pads stay unseen, as without exception tables
    return out


def data_blocks(img, text_end, md):
    gaps, last = [], 0
    for s, e in fde_ranges(img):
        if s >= text_end: break
        if s > last: gaps.append((last, s))
        last = max(last, e)
    if last < text_end: gaps.append((last, text_end))
    out = []
    for s, e in gaps:
        s, e = (s + 3) & ~3, e & ~3
        if e - s < DATA_MIN: continue
        undecodable = sum(1 for a in range(s, e, 4) if next(md.disasm_lite(bytes(img[a:a + 4]), a), None) is None)
        if undecodable * 4 > (e - s) // 4: out.append((s, e))
    return out


def out_store(img, start, size, md):
    """The one store through an out pointer copied from x1 in the prologue (patch_nso.py "Address regions"); ValueError when that shape is not there"""
    import re
    ins = list(md.disasm(bytes(img[start:start + size]), start))
    out = [i for i in ins[:16] if i.mnemonic == 'mov' and re.fullmatch(r'x\d+, x1', i.op_str)]
    if len(out) != 1: raise ValueError('no single copy of the out pointer in the prologue')
    xb = out[0].op_str.split(',')[0]
    st = [i for i in ins if i.mnemonic == 'str' and re.fullmatch(rf'x\d+, \[{xb}\]', i.op_str)]
    if len(st) != 1: raise ValueError(f'{len(st)} stores through the out pointer')
    return st[0].address, int(st[0].op_str.split(',')[0][1:])


if __name__ == '__main__':
    import sys
    from capstone import Cs, CS_ARCH_ARM64, CS_MODE_ARM
    p = sys.argv[1]
    raw = open(p, 'rb').read()
    if raw[:4] == b'NSO0':
        import nso
        try: m = nso.load(raw, p)
        except nso.Error as e: sys.exit(f'[RED] {e}')
        img, text_end = m.image, m.text_end
    else:
        img = raw
        text_end = int(sys.argv[2], 0)
    blocks = data_blocks(img, text_end, Cs(CS_ARCH_ARM64, CS_MODE_ARM))
    print(f'{p}: {len(blocks)} data blocks in text, {sum(e - s for s, e in blocks)} bytes')
    for s, e in blocks: print(f'  {s:#x}..{e:#x} ({e - s} bytes)')
