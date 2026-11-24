#!/usr/bin/env python3
"""The one loader of the game's code modules: an NSO → its memory image of text + ro + data (bss is only a size).
Checks what Horizon's loader checks (Atmosphere's ldr_process_creation.cpp), before anything the header claims is allocated, and raises Error on a damaged
file: a short LZ4 stream decompresses without an error, so a segment must decompress to exactly its size, and with the check-hash flag its SHA-256 must match.
A compressed segment can claim no more than LZ4 produces from its stored bytes. ZBIC (flag bit 7, system 22.0.0+) is Nintendo's zstd variant.
Usage on its own: nso.py <module> <out.bin> (the whole memory image, bss included)"""
import hashlib, struct, sys
from typing import NamedTuple

PAGE = 0x1000
LZ4_MAX_RATIO = 255   # a match length extension byte per stored byte
SIZE_MAX = 0x7FFFFFFF   # Horizon reads segment sizes as s32
NSO_ZSTD = 1 << 7
NAMES = ('text', 'ro', 'data')


class Error(ValueError):
    pass


def zstd_segment(raw, size, what):
    """zbic's own checks catch a damaged stream; a wrong length still gets this loader's Error"""
    try:
        import zbic   # optional (tools.txt): only system 22.0.0+ modules' ZBIC segments need it
    except ImportError:
        raise Error(f'{what}: ZBIC (zstd) segments need the zbic package (python3 -m pip install zbic)') from None
    try:
        out = zbic.decompress(bytes(raw))
    except Exception as e:
        raise Error(f'{what}: ZBIC does not decompress ({e})') from None
    if len(out) != size: raise Error(f'{what}: ZBIC decompresses to {len(out):#x} bytes, not the {size:#x} of the header: the file is damaged')
    return out


class Module(NamedTuple):
    image: bytearray
    segments: tuple
    bss: int
    build_id: bytes     # the build ID tools key on is its first 0x10

    @property
    def text_end(self): return self.segments[0][1]


def load(d, name='module'):
    if d[:4] == b'NSO0':
        if len(d) < 0x100: raise Error(f'{name}: the NSO header is cut short ({len(d)} bytes)')
        flags = struct.unpack_from('<I', d, 0xc)[0]
        stored = [struct.unpack_from('<III', d, 0x10 + i * 0x10) for i in range(3)]
        csz = [c if flags >> i & 1 else sz for i, ((_, _, sz), c) in enumerate(zip(stored, struct.unpack_from('<III', d, 0x60)))]
        bss, build_id = struct.unpack_from('<I', d, 0x3c)[0], bytes(d[0x40:0x60])
        for i, ((fo, mo, sz), c) in enumerate(zip(stored, csz)):
            what = f'{name}: {NAMES[i]} segment'
            if mo % PAGE: raise Error(f'{what} is at {mo:#x}, not on a page')
            if sz > SIZE_MAX: raise Error(f'{what} claims {sz:#x} bytes, more than Horizon loads')
            if fo + c > len(d): raise Error(f'{what} is stored at {fo:#x}..{fo + c:#x}, past the end of the file ({len(d):#x} bytes): the file is cut short or damaged')
            if flags >> i & 1 and c > sz: raise Error(f'{what} is stored compressed in {c:#x} bytes, more than its {sz:#x}')
            if flags >> i & 1 and not flags & NSO_ZSTD and sz > c * LZ4_MAX_RATIO: raise Error(f'{what} claims {sz:#x} bytes from {c:#x} stored, more than LZ4 can produce')
        if stored[0][1]: raise Error(f'{name}: text segment is at {stored[0][1]:#x}, not at 0')
        for i in (1, 2):
            if stored[i][1] < stored[i - 1][1] + stored[i - 1][2]: raise Error(f'{name}: {NAMES[i]} segment at {stored[i][1]:#x} overlaps the {NAMES[i - 1]} segment before it')
    else:
        raise Error(f'{name}: not an NSO (magic {bytes(d[:4])!r})')
    end = stored[2][1] + stored[2][2]
    if end > len(d) * LZ4_MAX_RATIO: raise Error(f'{name}: the segments span {end:#x} bytes, more than a file of {len(d):#x} bytes can fill (a gap of zeros between them)')
    img = bytearray(end)
    for i, ((fo, mo, sz), c) in enumerate(zip(stored, csz)):
        raw = d[fo:fo + c]
        if flags >> i & 1:
            if flags & NSO_ZSTD:
                raw = zstd_segment(raw, sz, f'{name}: {NAMES[i]} segment')
            else:
                import lz4.block
                try: raw = lz4.block.decompress(raw, uncompressed_size=sz)
                except lz4.block.LZ4BlockError as e: raise Error(f'{name}: {NAMES[i]} segment does not decompress ({e})') from None
        if len(raw) != sz: raise Error(f'{name}: {NAMES[i]} segment decompresses to {len(raw):#x} bytes, not the {sz:#x} of the header: the file is damaged')
        if flags >> (3 + i) & 1 and hashlib.sha256(raw).digest() != d[0xa0 + 0x20 * i:0xc0 + 0x20 * i]:
            raise Error(f'{name}: {NAMES[i]} segment does not match its SHA-256 in the header: the file is damaged')
        img[mo:mo + sz] = raw
    return Module(img, tuple((mo, sz) for _, mo, sz in stored), bss, build_id)


def read(path):
    with open(path, 'rb') as f: return load(f.read(), str(path))


def cli_read(path):
    """A damaged module stops the script with a [RED] line, not a traceback"""
    try: return read(path)
    except Error as e: sys.exit(f'[RED] {e}')


if __name__ == '__main__':
    m = cli_read(sys.argv[1])
    with open(sys.argv[2], 'wb') as f:
        f.write(m.image)
        f.truncate(len(m.image) + m.bss)   # a sparse zero tail
    print([hex(x) for s in m.segments for x in s], hex(m.bss), 'moduleid', m.build_id.hex())
