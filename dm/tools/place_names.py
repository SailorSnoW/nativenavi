#!/usr/bin/env python3
"""The game's place names in each of its languages, for the Discord presence (dm/host/presence.c): the messages of each language's archive that name the places,
labelled by the place IDs the game's data uses (KokiriForest, LakeHylia, ...). The profile names them: msg_archive (the archive's path, {code} its language),
msg_languages (language code = archive code) and place_messages (the MSBT in the archive).
Usage: place_names.py <RomFS folder or image> <out.tsv> [<profile id>]
Writes a comment, a header (id, then each language code whose archive the RomFS has) and a row per place; a name is its text without the message's tags."""
import fnmatch, os, re, struct, sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'games'))
import bfsha   # noqa: E402
import game as gamemod   # noqa: E402
import romfs as rfs   # noqa: E402


class Error(Exception): pass


def msbt(d, where):
    """{label: text} of a message file, its tags (0x0E, 0x0F) left out"""
    def field(fmt, at):
        if at < 0 or at + struct.calcsize(fmt) > len(d): raise Error(f'{where}: {struct.calcsize(fmt)} bytes at {at:#x} lie past its {len(d):#x}')
        return struct.unpack_from(fmt, d, at)
    if d[:8] != b'MsgStdBn': raise Error(f'{where}: not an MSBT message file')
    e = {b'\xff\xfe': '<', b'\xfe\xff': '>'}.get(d[8:10])
    if not e: raise Error(f'{where}: no byte order mark')
    enc, (nsec,) = d[0x0c], field(e + 'H', 0x0e)
    if enc not in (0, 1): raise Error(f'{where}: text encoding {enc}, neither UTF-8 (0) nor UTF-16 (1)')
    w, labels, texts, at = 1 if enc == 0 else 2, {}, None, 0x20
    for _ in range(nsec):
        magic, (size,) = d[at:at + 4], field(e + 'I', at + 4)
        b = at + 16
        if b + size > len(d): raise Error(f'{where}: section {magic!r} runs past the file')
        if magic == b'LBL1':
            (groups,) = field(e + 'I', b)
            for i in range(groups):
                n, off = field(e + 'II', b + 4 + 8 * i)
                p = b + off
                for _ in range(n):
                    ln = d[p]
                    name = d[p + 1:p + 1 + ln].decode('ascii', 'replace')
                    (labels[name],) = field(e + 'I', p + 1 + ln)
                    p += 5 + ln
        elif magic == b'TXT2':
            (n,) = field(e + 'I', b)
            offs = [field(e + 'I', b + 4 + 4 * i)[0] for i in range(n)] + [size]
            texts = []
            for i in range(n):
                raw, out, j = d[b + offs[i]:b + offs[i + 1]], bytearray(), 0
                while j + w <= len(raw):
                    c = raw[j] if w == 1 else struct.unpack_from(e + 'H', raw, j)[0]
                    if c == 0: break
                    if c == 0x0e:   # a tag: group, type, parameter size, parameters
                        if j + w + 6 > len(raw): raise Error(f'{where}: message {i}: a tag runs past its text')
                        j += w + 6 + struct.unpack_from(e + 'H', raw, j + w + 4)[0]
                    elif c == 0x0f: j += w + 4   # a tag's end: group, type
                    else: out += raw[j:j + w]; j += w
                texts.append(out.decode('utf-8' if w == 1 else 'utf-16-le' if e == '<' else 'utf-16-be', 'replace'))
        at = b + (size + 15 & ~15)
    if texts is None: raise Error(f'{where}: no TXT2 section')
    bad = [k for k, i in labels.items() if i >= len(texts)]
    if bad: raise Error(f'{where}: labels {", ".join(bad[:5])} name messages it does not have')
    return {k: texts[i] for k, i in sorted(labels.items(), key=lambda kv: kv[1])}


def clean(s):   # one TSV cell: no tabs or line breaks, no control characters
    return re.sub(r'\s+', ' ', ''.join(ch for ch in s if ch.isprintable() or ch.isspace())).strip()


def archives(romfs, pattern):
    """{archive code: its path in the RomFS}, the newest Product number when several"""
    folder, name = os.path.split(pattern)
    try:
        with rfs.scandir(os.path.join(romfs, folder)) as it: have = [e.name for e in it]
    except OSError: return {}
    out, glob = {}, name.replace('{code}', '*')
    for f in sorted(have, key=lambda f: [int(t) if t.isdigit() else t for t in re.split(r'(\d+)', f)]):
        if fnmatch.fnmatchcase(f, glob): out[f.split('.', 1)[0]] = f'{folder}/{f}'
    return out


def table(romfs, g):
    """(languages, {place ID: {language: name}}) from the RomFS; Error when the profile names none or the RomFS has none"""
    pattern, member = g.get('msg_archive', ''), g.get('place_messages', '')
    langs = g.pairs('msg_languages') if g.get('msg_languages', '') else []
    if not (pattern and member and langs): raise Error(f'game profile {g.id} names no place messages (msg_archive, msg_languages, place_messages)')
    have = archives(romfs, pattern)
    names, cols, read = {}, [], {}
    for lang, code in langs:
        if code not in have: continue
        if code not in read: read[code] = msbt(bfsha.read_member(str(romfs), f'{have[code]}/{member}'), f'{have[code]}/{member}')
        cols.append(lang)
        for place, text in read[code].items(): names.setdefault(place, {})[lang] = clean(text)
    if not cols: raise Error(f'{romfs}: none of the message archives msg_languages names ({pattern})')
    return cols, names


def write(path, cols, names, src):
    rows = [f'# The game\'s place names (dm/tools/place_names.py, from {src}): a place ID, then its name for each language code (DM_LANG)', '\t'.join(['id'] + cols)]
    rows += ['\t'.join([p] + [names[p].get(c, '') for c in cols]) for p in sorted(names)]
    tmp = Path(str(path) + '.part')
    tmp.write_text('\n'.join(rows) + '\n', encoding='utf-8')
    tmp.replace(path)


def main(argv):
    if len(argv) not in (2, 3): sys.exit(__doc__)
    g = gamemod.load(argv[2] if len(argv) == 3 else None)
    try: cols, names = table(argv[0], g)
    except (Error, bfsha.Unread) as e: sys.exit(f'[RED] place names: {e}')
    write(argv[1], cols, names, f'{g.get("msg_archive")} / {g.get("place_messages")}')
    print(f'{len(names)} places in {len(cols)} languages → {argv[1]}')


if __name__ == '__main__':
    main(sys.argv[1:])
