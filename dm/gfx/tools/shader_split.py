#!/usr/bin/env python3
# The game's shaders → dmsh_build.py's package directory, the shader map and the translation answer table, from a full scan of RomFS (zstd, Yaz0 and SARC opened
# whatever the name) and ExeFS main. Every shader is NVN2; anything else stops the split (refuse).
#   · BNSH: +0x7c variation count, +0x80 variation table (0x40 bytes each, program at +0x10); a program's stage pointers from +0x08 (vertex, hull, domain, geometry,
#     pixel, compute); a stage descriptor's control +0x08, code +0x10, code length +0x18, control length +0x1c
#   · BAHS: code region at +0x1c, length +0x18; 16-byte entries {code offset in the region, 0, control length, control's signed offset from the entry}. Control
#     sections are shared between programs, so pairing goes by entry
# Key = sha256(the control's first 0x48 bytes, +0x30 as zero, + code area)[:16], as the host's keys_hash: many shaders share a control section.
# Record: 'BTCDBTCD' + header (+0x08 key, +0x28 control length, +0x2c code length) padded to 0x38 + code area + control section, one per key.
# Usage: shader_split.py <RomFS dir> <ExeFS main> <output package dir> <output map> <output answer table> [<output pair table> [<pso_pairs.tsv> [<DLC RomFS dirs, comma-separated>]]]
#   RomFS and main take comma-separated lists for an update (base first); J=concurrency (default 8)
import collections, concurrent.futures as cf, hashlib, json, os, re, struct, sys, time
from typing import NoReturn
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '../../tools'))
from romfs import os, open, is_image
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '../../games'))
import game as _game
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
GAME = _game.load()
GROUPS, NOTE = GAME.pairs('shader_groups'), GAME.get('shader_note')

CTL_MAGIC = bytes.fromhex('34127698')
STAGES = ['vs', 'hs', 'ds', 'gs', 'ps', 'cs']
SPH_STAGE = {1: 'vs', 2: 'hs', 3: 'ds', 4: 'gs', 5: 'ps'}   # SPH word 0, bits 10..13
STAGE_NAME = {'vs': 'vertex', 'ps': 'pixel', 'cs': 'compute', 'ts': 'task', 'ms': 'mesh'}
GONE = {'hs': 'hull', 'ds': 'domain', 'gs': 'geometry', 'rt': 'ray tracing'}

def red(msg) -> NoReturn: sys.exit('[RED] ' + msg)

class Refused(SystemExit):
    """Unlike an unreadable file, this ends the split: a library without the shader could lack one the game asks for"""
def stop(msg) -> NoReturn: raise Refused('[RED] ' + msg)

def field(d, fmt, at, where):
    """red() naming where and the offset when it is not inside d"""
    if not 0 <= at <= len(d) - struct.calcsize(fmt): red(f'{where}: {struct.calcsize(fmt):#x} bytes at {at:#x} lie outside the {len(d):#x} bytes there are')
    return struct.unpack_from(fmt, d, at)

def span(d, at, n, where):
    """or red(): a slice would silently come out short"""
    if not (0 <= at and 0 <= n <= len(d) - at): red(f'{where}: {n:#x} bytes at {at:#x} lie outside the {len(d):#x} bytes there are')
    return d[at:at + n]

def refuse(where, st):
    """hull, domain, geometry, ray tracing: the game has none"""
    if st in GONE: stop(f'{where}: a {GONE[st]} shader: this chain translates only the vertex, pixel, compute, task and mesh shaders of an NVN2 game, as Ocarina of Time has no other')

def zstd_mod():
    """compression.zstd from Python 3.14, else backports.zstd"""
    try: from compression import zstd
    except ImportError:
        try: from backports import zstd
        except ImportError: red(f'Python {sys.version_info[0]}.{sys.version_info[1]} has no compression.zstd (3.14+): pip install backports.zstd')
    return zstd

NVN2_KEY_CTL = 0x48
def nvn2_ctl(ctl):
    """+0x30 holds the section's length in BNSH files and 0 in BAHS files and in memory, so the key takes it as zero"""
    return len(ctl) >= NVN2_KEY_CTL and ctl[:4] == CTL_MAGIC and ctl[4:8] == b'\xc9\0\0\0'

def key_ctl(ctl):
    """Only called on an NVN2 section (check)"""
    c = ctl[:NVN2_KEY_CTL]
    return c[:0x30] + bytes(4) + c[0x34:]

def skey(ctl, code):
    return hashlib.sha256(key_ctl(ctl) + code).hexdigest()[:16]

def same_shader(a, b):
    """Equal code areas and the control bytes the key takes (past them, +0x30 among them, BNSH and BAHS copies differ)"""
    (ca, ka), (cb, kb) = a, b
    return ka == kb and key_ctl(ca) == key_ctl(cb)

NVN1 = 'not an NVN2 control section (version 0xc9): an NVN1 (Maxwell) shader, which this chain does not translate'
def check(where, st, ctl, code):
    if ctl[:4] != CTL_MAGIC or len(ctl) < 0x24: return 'bad control section'
    if not nvn2_ctl(ctl): return NVN1
    return None if struct.unpack_from('<I', ctl, 0x44)[0] == len(code) else 'control +0x44 does not match the code area length'

def bnsh_shaders(d, base, label):
    where = f'{label} BNSH @{base:#x}'
    if d[base + 0x60:base + 0x64] != b'grsc': red(f'{where} has no grsc block')
    nvar, pvar = field(d, '<IQ', base + 0x7c, f'{where} grsc')
    out = []
    for v in range(nvar):
        prog = field(d, '<Q', base + pvar + 0x40 * v + 0x10, f'{where} variation {v}')[0]
        if not prog: continue
        for si, st in enumerate(STAGES):
            sc = field(d, '<Q', base + prog + 8 + 8 * si, f'{where} variation {v} program')[0]
            if not sc: continue
            pctl, pcode, ncode, nctl = field(d, '<QQII', base + sc + 8, f'{where} variation {v} {st}')
            ctl, code = span(d, base + pctl, nctl, f'{where} variation {v} {st} control section'), span(d, base + pcode, ncode, f'{where} variation {v} {st} code area')
            why = check(label, st, ctl, code)
            if why: (stop if why is NVN1 else red)(f'{label} BNSH @{base:#x} variation {v} {st}: {why}')
            refuse(f'{where} variation {v}', st)
            out.append((f'{label}#BNSH@{base:#x}#var{v}', st, ctl, code))
        # Older programs end at +0x38; an NVN2 stage 8 pointer there is a ray tracing module: refused, not left out
        if base + prog + 0x40 <= len(d):
            sc = struct.unpack_from('<Q', d, base + prog + 0x38)[0]
            if sc and base + sc + 0x20 <= len(d):
                pctl = struct.unpack_from('<Q', d, base + sc + 8)[0]
                prefix = d[base + pctl:base + pctl + 0x57]
                if nvn2_ctl(prefix) and len(prefix) == 0x57 and prefix[0x56] == 8: refuse(f'{where} variation {v}', 'rt')
        # NVN2's task and mesh stages at +0x40 / +0x48; only a pointer to a control section of that stage counts
        for off, st, stage in ((0x40, 'ts', 6), (0x48, 'ms', 7)):
            if base + prog + off + 8 > len(d): continue
            sc = struct.unpack_from('<Q', d, base + prog + off)[0]
            if not sc or base + sc + 0x20 > len(d): continue
            pctl, pcode, ncode, nctl = struct.unpack_from('<QQII', d, base + sc + 8)
            if not 0x48 <= nctl < 0x10000 or base + pctl + nctl > len(d) or base + pcode + ncode > len(d): continue
            ctl, code = d[base + pctl:base + pctl + nctl], d[base + pcode:base + pcode + ncode]
            if not nvn2_ctl(ctl) or len(ctl) <= 0x56 or ctl[0x56] != stage: continue
            why = check(label, st, ctl, code)
            if why: (stop if why is NVN1 else red)(f'{label} BNSH @{base:#x} variation {v} {st}: {why}')
            out.append((f'{label}#BNSH@{base:#x}#var{v}', st, ctl, code))
    return out

def nvn2_stage(ctl, code):   # compute, task, mesh, ray tracing: control +0x56 (5..8); graphics: SPH word 0 bits 10..13
    if len(ctl) > 0x56 and ctl[0x56] in (5, 6, 7, 8): return {5: 'cs', 6: 'ts', 7: 'ms', 8: 'rt'}[ctl[0x56]]
    return SPH_STAGE.get(struct.unpack_from('<I', code)[0] >> 10 & 0xf) if len(code) >= 4 else None


def bahs_shaders(d, label):
    rsz, region = field(d, '<II', 0x18, f'{label} BAHS header')
    if region + rsz > len(d): red(f'{label} BAHS code region out of bounds')
    found = []
    for a in range(0, region - 16, 4):
        off, z, csz, rel = struct.unpack_from('<IIIi', d, a)
        c = a + rel
        nvn2 = 0x48 <= csz < 0x10000 and 0 <= c and c + csz <= region and nvn2_ctl(d[c:c + csz])
        if z or off % 0x100: continue
        if not nvn2:
            # An NVN1 entry (control length 0x880 / 0x88c, or 0x760..0x87f): not passed over as if the file held no shader
            if (csz in (0x880, 0x88c) or 0x760 <= csz < 0x880) and 0 <= c and c + csz <= region and d[c:c + 4] == CTL_MAGIC and d[c + 4:c + 8] != b'\xc9\0\0\0':
                n = struct.unpack_from('<I', d, c + 0x704)[0]
                if off + n <= rsz and d[region + off:region + off + 4] in (bytes.fromhex('78563412'), bytes.fromhex('79563412')):
                    stop(f'{label}#BAHS@{a:#x}: an NVN1 (Maxwell) shader entry: this chain translates only NVN2 (Switch 2) shaders')
            continue
        n = struct.unpack_from('<I', d, c + 0x44)[0]
        if off + n > rsz: continue
        code, ctl = d[region + off:region + off + n], d[c:c + csz]
        st = nvn2_stage(ctl, code)
        if not st or check(label, st, ctl, code): continue
        found.append((a, c, (f'{label}#BAHS@{a:#x}', st, ctl, code)))
    # A program entry never lies inside a control section: an NVN2 section's +0x38..+0x47 reads as a false entry. Each section counts with its own NVN2_KEY_CTL bytes
    spans = {(c, c + NVN2_KEY_CTL) for _, c, s in found}
    candidates = [(a, s) for a, _, s in found if not any(lo <= a < hi for lo, hi in spans)]
    # A validated binary array is authoritative: descriptor-shaped words also occur inside a record's other payload
    offsets = {a for a, _ in candidates}
    array = bahs_binary_records(d, region, offsets)
    if array is not None:
        records, end = array
        offsets.intersection_update(a for a, _ in records)
        bahs_variations(d, label, offsets, records, end)
    out = [s for a, s in candidates if a in offsets]
    for where, st, _, _ in out: refuse(where, st)
    return out


def ndsr_shaders(d, label):
    """'NDSR', then from +0x10 16-byte records up to a zero one: u32 control offset, u32 (control length << 4 | type), u32 code offset, u32 (code length / 16 << 8 | type);
    those pointing at an NVN2 control section are shaders. A pack whose header has another word starts at +0x14"""
    if d[:4] != b'NDSR' or len(d) < 0x20: return []
    def records_from(o):
        count = min(16, (len(d) - o) // 16)
        records = struct.iter_unpack('<4I', d[o:o + 16 * count])
        return sum(1 for c, _, _, _ in records if c + 0x48 <= len(d) and nvn2_ctl(d[c:c + 0x48]))
    out = []
    o = max((0x10, 0x14), key=records_from)
    prog = None
    while o + 16 <= len(d):
        c, cl, k, _ = struct.unpack_from('<4I', d, o)
        if not (c or cl or k or _): break
        # Entries are 8 bytes, a shader two of them, so data entries can shift the next shader by 8
        if not (c + 0x48 <= len(d) and nvn2_ctl(d[c:c + 0x48])):
            o += 8
            continue
        o += 16
        ctl = d[c:c + max(cl >> 4, 0x48)]
        code = d[k:k + struct.unpack_from('<I', ctl, 0x44)[0]]
        st = nvn2_stage(ctl, code)
        if st is None or check(label, st, ctl, code): continue
        where = f'{label}#NDSR@{c:#x}'
        refuse(where, st)
        out.append((where, st, ctl, code))
        if st == 'vs': prog = where
        elif st == 'ps' and prog:
            BAHS_VAR[prog] = BAHS_VAR[where] = f'{label}#NDSR#{prog.rsplit("@", 1)[1]}'
            prog = None
    return out

def ndve_shaders(d, label):
    """'NDVE', +0xc u32 the NDSR pack's length, +0x14 u32 count, count 8-byte model records, then the NDSR pack (offsets from its own start), then the networks,
    whose control sections name their own and their code area's length (+0x30, +0x44), the code behind"""
    if len(d) < 0x18: return []
    pack = 0x18 + 8 * struct.unpack_from('<I', d, 0x14)[0]
    nets = pack + struct.unpack_from('<I', d, 0xc)[0]
    out = ndsr_shaders(d[pack:nets], label)
    c = d.find(CTL_MAGIC, nets)
    while c >= 0:
        if not c & 3 and c + 0x48 <= len(d) and nvn2_ctl(d[c:c + 0x48]):
            own, n = struct.unpack_from('<I', d, c + 0x30)[0], struct.unpack_from('<I', d, c + 0x44)[0]
            ctl, code = d[c:c + own], d[c + own:c + own + n]
            st = nvn2_stage(ctl, code)
            if st and len(code) == n and not check(label, st, ctl, code):
                refuse(f'{label}#NDVE@{c:#x}', st)
                out.append((f'{label}#NDVE@{c:#x}', st, ctl, code))
        c = d.find(CTL_MAGIC, c + 4)
    return out

BAHS_VAR = {}
def bahs_binary_records(d, region, taken):
    """Validated binary array: (entry-offset/kind records, array end), or None for
    an unrecognized layout. Used both to identify shaders and pair variations."""
    if not taken: return
    arr = min(taken) - 0x18
    if arr < 0x20: return
    asz, acnt = struct.unpack_from('<II', d, arr)
    end = arr + asz
    strays = max(1, len(taken) // 100)
    if not acnt or end > region or acnt < len(taken) - strays: return
    o, recs = arr + 8, []
    for _ in range(acnt):
        if o + 0x20 > end: return
        rs, kind = struct.unpack_from('<II', d, o)
        if rs < 0x20: return
        recs.append((o + 0x10, kind))
        o += rs
    # Every candidate is a record's entry, but for one stray scan match, or up to 1 % in larger arrays; more leaves the layout unrecognized.
    if o != end or len(taken - {a for a, _ in recs}) > strays: return
    return recs, end

def bahs_variations(d, label, taken, recs, o):
    """A binary array (u32 byte size, u32 count, per binary: u32 record size, u32 kind (0 vertex, 1 fragment, 3 compute), two u32, the 16-byte entry), then a program
    array (u32 byte size, u32 count, per program: u32 record size, u32 name length, u32 stage mask, u32 first binary index, name, macros). The n-th vertex and n-th
    fragment binaries of a program's range are one variation"""
    acnt = len(recs)
    if o + 8 > len(d): return
    psz, pcnt = struct.unpack_from('<II', d, o)
    if o + psz > len(d) or not pcnt: return
    q, bases = o + 8, []
    for _ in range(pcnt):
        if q + 16 > o + psz: return
        rs, _nl, _mask, base = struct.unpack_from('<IIII', d, q)
        if rs < 16 or base > acnt or (bases and base < bases[-1]): return
        bases.append(base)
        q += rs
    if q != o + psz: return
    bases.append(acnt)
    for p in range(pcnt):
        seen = [0, 0]
        for i in range(bases[p], bases[p + 1]):
            a, k = recs[i]
            if k > 1: continue
            if a in taken: BAHS_VAR[f'{label}#BAHS@{a:#x}'] = f'{label}#BAHS#prog{p}#var{seen[k]}'
            seen[k] += 1


ZSTD_MAGIC = bytes.fromhex('28b52ffd')


def yaz0(s, label='Yaz0'):
    """Group header bits: 1 = a literal byte; 0 = back-reference, 2 bytes {length high nibble, 12-bit distance} (distance + 1); length 0 reads one more byte + 0x12,
    else + 2. Overlapping copies expand by their period"""
    n = int.from_bytes(s[4:8], 'big')
    out = bytearray()
    si = 16
    try:
        while len(out) < n:
            h = s[si]
            si += 1
            for _ in range(8):
                if h & 0x80:
                    out.append(s[si])
                    si += 1
                else:
                    b1, b2 = s[si], s[si + 1]
                    si += 2
                    dist, ln = ((b1 & 15) << 8 | b2) + 1, b1 >> 4
                    if ln:
                        ln += 2
                    else:
                        ln = s[si] + 0x12
                        si += 1
                    st = len(out) - dist
                    if st < 0: red(f'{label}: a back-reference at {si:#x} reaches {-st:#x} bytes before the start')
                    out += out[st:st + ln] if dist >= ln else (out[st:] * (ln // dist + 1))[:ln]
                if len(out) >= n: break
                h <<= 1
    except IndexError: red(f'{label}: the Yaz0 stream ends at {len(s):#x} with {len(out):#x} of its {n:#x} bytes decoded')
    return bytes(out[:n])

def sarc_members(d, label='SARC'):
    """A member range outside the data (one spanning the whole archive would be scanned again without end) is a damaged archive"""
    hl, bom, size, doff = field(d, '<HHII', 4, f'{label} SARC header')
    n = field(d, '<H', hl + 6, f'{label} SFAT header')[0]
    names = hl + 12 + 16 * n + 8
    if not names <= doff <= len(d): red(f'{label}: SARC data at {doff:#x}, its {n} entries and the names end at {names:#x}, the archive at {len(d):#x}')
    for i in range(n):
        h, attr, s, e = struct.unpack_from('<IIII', d, hl + 12 + 16 * i)
        if not s <= e <= len(d) - doff: red(f'{label}: SARC member {i} at {s:#x}..{e:#x} of the data lies outside its {len(d) - doff:#x} bytes')
        if attr >> 24:   # up to the name's own NUL: splitting the rest of the archive per member is quadratic
            o = names + (attr & 0xffffff) * 4
            z = d.find(b'\0', o)
            nm = d[o:z if z >= 0 else len(d)].decode('utf-8', 'replace')
        else: nm = f'#{i}'
        yield nm, d[doff + s:doff + e]

ZDICTS, _ZD = {}, {}

def _pool_init(zdicts={}):
    global ZDICTS
    ZDICTS = zdicts

def zs_dicts(romfs):
    """Pack/ZsDic.pack.zs: a SARC of *.zsdic zstd dictionaries (magic 0xEC30A437, then the ID). → {ID: dictionary}; {} without it"""
    p = os.path.join(romfs, 'Pack/ZsDic.pack.zs')
    if not os.path.exists(p): return {}
    d = zstd_mod().decompress(open(p, 'rb').read())
    return {struct.unpack_from('<I', m, 4)[0]: m for _, m in sarc_members(d, '/Pack/ZsDic.pack.zs') if m[:4] == bytes.fromhex('37a430ec')}

def zs_dict_id(d):
    """The zstd dictionary a frame names (0: none)"""
    fhd = d[4] if len(d) > 5 and d[:4] == ZSTD_MAGIC else 0
    n, o = (0, 1, 2, 4)[fhd & 3], 6 - (fhd >> 5 & 1)   # dictionary ID size; it follows the window descriptor, which single-segment frames omit
    return int.from_bytes(d[o:o + n], 'little')

def unzs_dict(d, did, where):
    zstd = zstd_mod()
    if did not in ZDICTS: red(f'{where}: a frame needs zstd dictionary {did}, which Pack/ZsDic.pack.zs does not have')
    if did not in _ZD: _ZD[did] = zstd.ZstdDict(ZDICTS[did])
    return zstd.decompress(d, zstd_dict=_ZD[did])

def unzs(path):
    """every frame, each with the dictionary it names"""
    d = open(path, 'rb').read()
    did = zs_dict_id(d)
    return unzs_dict(d, did, path) if did else zstd_mod().decompress(d)


NEST = 16   # archives in archives
            # Two budgets of NEST times the file bound a damaged or crafted archive's work: left[0] the members its archives open, by stored size, left[1] what its Yaz0
            # members expand to
def scan_blob(d, label, out, depth=0, left=None):
    if depth > NEST: red(f'{label}: archives nested more than {NEST} deep')
    stored = len(d)
    if d[:4] == b'Yaz0':
        if left is not None:
            left[1] -= int.from_bytes(d[4:8], 'big')
            if left[1] < 0: red(f'{label}: the Yaz0 archive members expand to more than {NEST} times the file')
        d = yaz0(d, label)
    if left is None: left = [NEST * len(d)] * 2
    else:
        left[0] -= stored
        if left[0] < 0: red(f'{label}: the archive members add up to more than {NEST} times the file (members that overlap)')
    if d[:4] == b'SARC':
        for nm, m in sarc_members(d, label): scan_blob(m, f'{label}/{nm}', out, depth + 1, left)
        return
    if d[:4] == b'BAHS':
        out.extend(bahs_shaders(d, label))
        return
    if d[:4] == b'NDSR':
        out.extend(ndsr_shaders(d, label))
        return
    if d[:4] == b'NDVE':
        out.extend(ndve_shaders(d, label))
        return
    for m in re.finditer(b'BNSH\0\0\0\0', d):
        if m.start() + 0x20 > len(d): continue
        size = struct.unpack_from('<I', d, m.start() + 0x1c)[0]
        if m.start() + size <= len(d) and d[m.start() + 0x60:m.start() + 0x64] == b'grsc': out.extend(bnsh_shaders(d, m.start(), label))

def merge_keys(results):
    """A key found more than once keeps its first copy, but the game's own copy displaces an add-on's (/aoc/<index>/), so the game's rows do not depend on the DLC set"""
    keys, pairs, per_src = {}, set(), collections.Counter()
    for rel, uniq, pr, _ in results:
        pairs |= pr
        for key, v in uniq.items():
            if key in keys:
                old = keys[key]
                if not same_shader(old[2:4], v[2:4]): red(f'key {key} is the same but the control section or code area differs ({v[0]} / {old[0]})')
                if old[0].startswith('/aoc/') and not v[0].startswith('/aoc/'):
                    per_src[group(old[0])] -= 1
                    keys[key] = v
                    per_src[group(v[0])] += 1
                continue
            keys[key] = v
            per_src[group(v[0])] += 1
    return keys, pairs, per_src

def group(label):
    """source group (second half of the package file name)"""
    if label.startswith('main'): return 'main'
    for k, g in GROUPS:   # first match wins
        if k in label: return g
    return 'other'

def var_pairs(found):
    """(vertex or mesh key, pixel key) per BNSH variation; (vertex key, pixel key, 'prog') per BAHS program variation. A mesh shader is the stage before the pixel
    shader as a vertex shader is"""
    var = collections.defaultdict(dict)
    for where, st, ctl, code in found:
        g = where if '#var' in where else BAHS_VAR.get(where)
        if g: var[g][st] = skey(ctl, code)
    return {(v[st], v['ps']) + (('prog',) if '#BAHS#prog' in g else ()) for g, v in var.items() for st in ('vs', 'ms') if st in v and 'ps' in v}

def unreadable_why(e, name):
    why = str(e).removeprefix('[RED] ') if isinstance(e, SystemExit) else f'{type(e).__name__}: {e}'
    return why if why.lstrip('/').startswith(name.lstrip('/')) else f'{name}: {why}'

def scan_file(args):
    """→ (relative path, {key: …}, pairs, extra); extra {} or ('unreadable', reason, files left out): the split goes on, main reports them"""
    try: return scan_file_in(args)
    except Refused: raise
    except (Exception, SystemExit) as e:   # red() is a SystemExit
        why = unreadable_why(e, args[1])
        print(f'[RED] {why} (its shaders are left out)', flush=True)
        return args[1], {}, set(), ('unreadable', why, 1)


def scan_file_in(args):
    path, rel = args[:2]
    with open(path, 'rb') as f: head = f.read(4)
    if head == ZSTD_MAGIC: d = unzs(path)
    else:
        with open(path, 'rb') as f: d = f.read()
    if d[:4] == b'Yaz0': d = yaz0(d, '/' + rel)
    if b'BNSH\0\0\0\0' not in d and b'BAHS' not in d and d[:4] != b'NDSR' and d[:4] != b'NDVE' and not (d[:4] == b'SARC' and b'Yaz0' in d): return rel, {}, set(), {}   # a SARC may hold Yaz0 members
    found = []
    scan_blob(d, '/' + rel, found)
    return rel, uniq_keys(found), var_pairs(found), {}


def uniq_keys(found):
    uniq = {}
    for where, st, ctl, code in found:
        key = skey(ctl, code)
        if key in uniq and not same_shader(uniq[key][2:4], (ctl, code)): red(f'key {key} is the same but the control section or code area differs ({where})')
        uniq.setdefault(key, (where, st, ctl, code))
    return uniq

def nso_image(path):
    import nso
    with open(path, 'rb') as f: return bytes(nso.load(f.read(), path).image)

def scan_batch(jobs):
    return [scan_file(j) for j in jobs]


def pooled(ex, jobs, total, start=0, chunk=1):
    """Results in job order; 'progress: <done>/<total>' at most twice a second counts jobs finished in any order, so it does not stall behind one slow range"""
    batches = [jobs[i:i + chunk] for i in range(0, len(jobs), chunk)]
    futures = {ex.submit(scan_batch, b): len(b) for b in batches}
    done, pending, shown = start, set(futures), 0.0
    while pending:
        finished, pending = cf.wait(pending, timeout=0.5)
        done += sum(futures[f] for f in finished)
        if not pending or time.monotonic() - shown >= 0.5:
            print(f'progress: {done}/{total}', flush=True)
            shown = time.monotonic()
    return [r for f in futures for r in f.result()]


def main():
    romfs_l, main_l, pkg, mapout, factsout = sys.argv[1:6]
    pairsout = sys.argv[6] if len(sys.argv) > 6 else None
    psopairsout = sys.argv[7] if len(sys.argv) > 7 else None
    dlc_v = sys.argv[8].split(',') if len(sys.argv) > 8 and sys.argv[8] else []
    # Base + update, paired one to one: later versions' sources are prefixed with their folder name (/v327680/...) and sort after, so rows keep their package indices.
    romfs_v, main_v = romfs_l.split(','), main_l.split(',')
    if len(romfs_v) != len(main_v): red(f'{len(romfs_v)} RomFS and {len(main_v)} main: they must pair one to one')
    files, results = [], []
    for i, (romfs, mainnso) in enumerate(zip(romfs_v, main_v)):
        tag = os.path.basename(os.path.dirname(os.path.abspath(romfs))) if i else ''
        files += sorted((os.path.join(dp, f), (tag + '/' if tag else '') + os.path.relpath(os.path.join(dp, f), romfs)) for dp, _, fs in os.walk(romfs) for f in fs)
        found = []
        scan_blob(nso_image(mainnso), 'main' + ('@' + tag if tag else ''), found)
        results.append(('main', uniq_keys(found), var_pairs(found), {}))
    for romfs in dlc_v:
        name = os.path.basename(os.path.normpath(romfs))
        tag = 'aoc/' + (name.removesuffix('.img') if is_image(romfs) else name)
        files += sorted((os.path.join(dp, f), tag + '/' + os.path.relpath(os.path.join(dp, f), romfs)) for dp, _, fs in os.walk(romfs) for f in fs)
    zdicts = {k: v for r in romfs_v + dlc_v for k, v in zs_dicts(r).items()}
    J = int(os.environ.get('J', '8'))
    with cf.ProcessPoolExecutor(J, initializer=_pool_init, initargs=(zdicts,)) as ex:
        results += pooled(ex, files, len(files), chunk=16)
    bad = [extra for *_, extra in results if isinstance(extra, tuple)]
    if bad: print(f'[RED] {sum(e[2] for e in bad)} file(s) could not be read, their shaders are left out (first: {bad[0][1]})', flush=True)   # build_app.py reads this line
    keys, pairs, per_src = merge_keys(results)
    prog = {(a, b) for a, b, *t in pairs if t} - pairs
    pairs = {x for x in pairs if len(x) == 2}
    os.makedirs(pkg, exist_ok=True)
    for f in os.listdir(pkg):
        if f.endswith('.bin'): os.unlink(os.path.join(pkg, f))
    recs = collections.defaultdict(list)
    rows = []
    facts = {}
    stat = collections.Counter()
    for key in sorted(keys, key=lambda k: (group(keys[k][0]), keys[k][0], k)):   # the same input gives byte-identical output
        where, st, ctl, code = keys[key]
        name = f'{st}_{group(where)}.bin'
        rec = bytearray(b'BTCDBTCD' + bytes.fromhex(key) + bytes(0x28))
        struct.pack_into('<2I', rec, 0x28, len(ctl), len(code))
        rec += code + ctl
        if rec.find(CTL_MAGIC, 0x38) != 0x38 + len(code): red(f'key {key}: the control magic appears in the code area, dmsh_build would split the record wrongly')
        if rec.count(b'BTCDBTCD') != 1: red(f'key {key}: the record separator appears inside the record')
        rows.append((name, len(recs[name]), STAGE_NAME[st], key, where))
        recs[name].append(bytes(rec))
        stat[st] += 1
        if st == 'cs':
            # shared memory from +0x64: not yet seen nonzero (unverified)
            at = (0x58, 0x64)
            x, y, z = struct.unpack_from('<3I', ctl, at[0])
            sh = struct.unpack_from('<I', ctl, at[1])[0]
            if not x * y * z: red(f'compute shader {key}: control +{at[0]:#x} thread group is 0 ({where})')
            facts[key] = {'local': [x, y, z], 'shared': (sh + 15) // 16 * 16}
    for name, rr in recs.items():
        with open(os.path.join(pkg, name), 'wb') as f: f.write(b''.join(rr))
    with open(mapout, 'w') as f:
        f.write(f'# kind: evidence  {NOTE} (BNSH / BAHS in RomFS + main) → control section + code area keys (dm/gfx/tools/shader_split.py)\n')
        f.write('# column 5 records where each key first appears, a copy in the game before one in an add-on; no PC shader pairing supplied\n')
        f.write('package\tindex\tstage\tdm_ctl_sha16\tsource\n')
        for r in rows: f.write('\t'.join(map(str, r)) + '\n')
    # merged over the generated rows, so the table stays reproducible
    manual = GAME.table('shader_facts_manual.tsv')
    if os.path.exists(manual):
        for ln in open(manual):
            if ln.strip() and not ln.startswith('#'):
                k, v = ln.rstrip('\n').split('\t', 1)
                facts[k] = {**facts.get(k, {}), **json.loads(v)}
    with open(factsout, 'w') as f:
        f.write('# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> fact JSON. No PC shader pairing supplied: only the thread group / shared memory of compute shaders (taken from the control section); other queries get the translator defaults\n')
        for k in sorted(facts): f.write(k + '\t' + json.dumps(facts[k], sort_keys=True, separators=(',', ':')) + '\n')
    if pairsout:
        with open(pairsout, 'w') as f:
            f.write('# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n')
            for a, b in sorted(pairs): f.write(f'{a}\t{b}\n')
    if psopairsout:
        with open(psopairsout, 'w') as f:
            f.write('# kind: evidence  supplemental vertex / pixel shader pairs (BAHS program variations) for vertex output padding and the pipeline prebuild (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n')
            for a, b in sorted(prog - pairs): f.write(f'{a}\t{b}\n')
    print(f'shaders: {len(rows)} (' + ', '.join(f'{STAGE_NAME[s]} {n}' for s, n in sorted(stat.items())) + '); sources ' + ', '.join(f'{g} {n}' for g, n in per_src.most_common()) + f'; compute shader answers {len(facts)} rows' + (f'; variation pairs {len(pairs)}' if pairsout else ''))

if __name__ == '__main__':
    import nso
    try: main()
    except nso.Error as e: red(str(e))
