#!/usr/bin/env python3
"""Make the host's stand-ins for the six system shared fonts (DM_FONT_DIR): types 0-4 as unhinted cuts of one source font, type 5 from make_ext_font.py
(docs/reference/services.md, "Shared font stand-ins sized for the game's font engines").
Usage: make_shared_fonts.py out_dir [source.ttf]; needs fontTools (pip install fonttools)"""
import os, sys
from fontTools import subset
from fontTools.ttLib import TTFont

SOURCE = '/System/Library/Fonts/Supplemental/Arial Unicode.ttf'   # STD_FONT / the second argument names another
LETTERS = '/System/Library/Fonts/Supplemental/Arial Rounded Bold.ttf'
# the name is the one the font engine reports (LoadFont); short, as Nintendo's are
TYPES = [(0, 'standard.ttf', 'DM Standard'), (1, 'chinese_simplified.ttf', 'DM Chinese Simplified'), (2, 'chinese_simplified_ext.ttf', 'DM Chinese Simplified Ext'),
         (3, 'chinese_traditional.ttf', 'DM Chinese Traditional'), (4, 'korean.ttf', 'DM Korean')]
EXT = 'nintendo_ext.ttf'

def rng(*pairs): return {c for a, b in pairs for c in range(a, b + 1)}

def legacy(codec, lead, trail=None):
    out, (lo, hi) = set(), trail or lead
    for a in range(lead[0], lead[1] + 1):
        for b in range(lo, hi + 1):
            try: s = bytes([a, b]).decode(codec)
            except UnicodeDecodeError: continue
            if len(s) == 1: out.add(ord(s))
    return out

# so a game that puts a Chinese or Korean font first still has digits, Latin and CJK punctuation
COMMON = rng((0x20, 0x7e), (0xa0, 0xff), (0x2000, 0x206f), (0x3000, 0x303f), (0xff00, 0xffef))

def charsets():
    gb2312 = legacy('gb2312', (0xa1, 0xfe))
    gbk_han = {c for c in legacy('gbk', (0x81, 0xfe), (0x40, 0xff)) if 0x4e00 <= c <= 0x9fff} - gb2312
    return {
        # as Nintendo's standard font: cp932, Latin, Greek, Cyrillic and the symbols games use
        0: COMMON | rng((0x100, 0x24f), (0x370, 0x3ff), (0x400, 0x4ff), (0x20a0, 0x20cf), (0x2100, 0x22ff), (0x2300, 0x23ff), (0x2460, 0x27bf), (0x3040, 0x30ff), (0x31f0, 0x31ff))
           | legacy('cp932', (0x81, 0xfc), (0x40, 0xfd)),
        1: COMMON | gb2312,
        2: COMMON | gbk_han,
        3: COMMON | legacy('big5', (0xa1, 0xf9), (0x40, 0xff)),
        4: COMMON | rng((0x1100, 0x11ff), (0x3130, 0x318f), (0xac00, 0xd7a3)) | legacy('euc_kr', (0xa1, 0xfe)),
    }

def cut(src, out, chars, name):
    f = TTFont(src, fontNumber=0)
    have = sorted(c for c in chars if c in (f.getBestCmap() or {}))
    # no hinting: its program sizes the engine's interpreter memory; of the layout tables only kerning (GSUB's forms grow the tables)
    o = subset.Options(hinting=False, layout_features=['kern'], name_IDs=['*'], name_languages=['*'], notdef_outline=True, recalc_bounds=True)
    s = subset.Subsetter(o)
    s.populate(unicodes=have)
    s.subset(f)
    ps = name.replace(' ', '-')
    for rec in f['name'].names:       # one font each: the engine knows fonts by name
        if rec.nameID in (1, 4, 16): rec.string = name
        elif rec.nameID in (3, 6): rec.string = ps
    f.save(out)
    return len(have), f['maxp'].numGlyphs

def build(out_dir, src=SOURCE, letters=LETTERS):
    os.makedirs(out_dir, exist_ok=True)
    sets = charsets()
    for t, fn, name in TYPES:
        chars, glyphs = cut(src, os.path.join(out_dir, fn), sets[t], name)
        print('%s: type %d, %d characters, %d glyphs, %d bytes' % (fn, t, chars, glyphs, os.path.getsize(os.path.join(out_dir, fn))))
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import make_ext_font
    make_ext_font.build(os.path.join(out_dir, EXT), letters)

if __name__ == '__main__':
    if len(sys.argv) < 2: sys.exit(__doc__ or 'usage: make_shared_fonts.py out_dir [source.ttf]')
    build(sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else SOURCE)
