#!/usr/bin/env python3
# Game name, display version and languages from control.nacp.
# Usage: nacp.py [--version | --languages] <control.nacp>; prints an empty line and exits 1 when none can be read.
import re, struct, sys, zlib
from pathlib import Path

# In SupportedLanguageFlag bit order. Polish and Thai are control-data bits 16 and 17; the SDK's language enum numbers them 18 and 19 (hle_sdk16.c).
LANGUAGES = [('en-US', 'American English'), ('en-GB', 'British English'), ('ja', 'Japanese'), ('fr', 'French'), ('de', 'German'), ('es-419', 'Latin American Spanish'), ('es', 'Spanish'), ('it', 'Italian'),
             ('nl', 'Dutch'), ('fr-CA', 'Canadian French'), ('pt', 'Portuguese'), ('ru', 'Russian'), ('ko', 'Korean'), ('zh-Hant', 'Traditional Chinese'), ('zh-Hans', 'Simplified Chinese'), ('pt-BR', 'Brazilian Portuguese'),
             ('pl', 'Polish'), ('th', 'Thai')]


def name(path):
    """0x300 bytes per language (name 0x200 + publisher 0x100): the first non-empty one; '' when none"""
    p = Path(path)
    if not p.is_file(): return ''
    b = p.read_bytes()
    t = b[:0x3000]
    if len(b) >= 2:
        try:   # newer SDKs: a u16 size + raw deflate, at most 32 × 0x300 (a crafted size could inflate about 1,000 times)
            d = zlib.decompressobj(-15)
            z = d.decompress(b[2:2 + struct.unpack_from('<H', b)[0]], 0x6000 + 1)
            if d.eof and len(z) == 0x6000: t = z
        except zlib.error: pass
    for i in range(len(t) // 0x300):
        n = t[i * 0x300:i * 0x300 + 0x200].split(b'\0', 1)[0]
        if not n: continue
        try: n = n.decode('utf-8')
        except UnicodeDecodeError: return ''
        return n if n.isprintable() else ''
    return ''


def version(path):
    """0x10 bytes at 0x3060, cut to what CFBundleVersion takes (up to three integers); '' when none"""
    p = Path(path)
    if not p.is_file(): return ''
    m = re.match(rb'\d+(\.\d+){0,2}', p.read_bytes()[0x3060:0x3070])
    return m.group().decode() if m else ''


def languages(path):
    """u32 SupportedLanguageFlag at 0x302C, in LANGUAGES order"""
    p = Path(path)
    if not p.is_file(): return []
    b = p.read_bytes()
    if len(b) < 0x3030: return []
    f = struct.unpack_from('<I', b, 0x302C)[0]
    return [c for i, (c, _) in enumerate(LANGUAGES) if f >> i & 1]


if __name__ == '__main__':
    a = sys.argv[1:]
    f = {'--version': version, '--languages': lambda p: ' '.join(languages(p))}.get(a[0] if a else '', name)
    if f is not name: a = a[1:]
    if len(a) != 1: sys.exit('usage: nacp.py [--version | --languages] <control.nacp>')
    n = f(a[0])
    print(n)
    sys.exit(0 if n else 1)
