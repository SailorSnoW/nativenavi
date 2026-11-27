#!/usr/bin/env python3
"""Carry a game profile's prewarm seed list (pso_manifest.tsv) to the shader keys of another version of the game, for a derived profile (scripts/build_app.py;
docs/reference/packing.md#a-derived-profiles-prewarm-list-names-another-versions-shaders).

An update that recompiles a shader gives it another key, so a list recorded on one version names shaders another version does not have. A shader is found again by
where it sits in the game's files: the shader map's source column names a container (a BNSH, BAHS, NDSR or NDVE entry, by its byte offset in a file) and what is in it.
Offsets move between versions, so containers are aligned in order, file by file: keys both versions share anchor the alignment, and the containers between two anchors
pair in order only when both versions have as many. What is in a container (its variations and stages) is carried only when both hold as many keys and no anchor in
them moved: a container whose variations were renumbered carries nothing. A file whose name differs only in its numbers (a table named by version) pairs with the one
other file of that name. A pipeline row is kept only when its carried shader pair is a pair of the new version's files (shader_pairs.tsv, pso_pairs.tsv); rows whose
shaders the new version has under the same keys are kept as they are. A shader archive's (BFSHA) variations, which an update renumbers, are found again by their
program's options (dm/tools/bfsha.py): the old version's table of them, and the new version's archives read from its RomFS. An effect set's (a .vsetb file's) BNSH
variations are found again within their container: the containers of one file pair in order, the variations whose keys both versions share anchor the order
within a paired container, and the variations between two anchors pair in order only when both sides hold as many.

python3 dm/tools/pso_carry.py [--programs <from shader_programs.tsv> --romfs <to RomFS>] <from shader_map.tsv> <to shader_map.tsv> <from pso_manifest.tsv> <out pso_manifest.tsv> <to pairs.tsv>...
"""
USAGE = ('python3 dm/tools/pso_carry.py [--programs <from shader_programs.tsv> --romfs <to RomFS>] <from shader_map.tsv> <to shader_map.tsv> <from pso_manifest.tsv> '
         '<out pso_manifest.tsv> <to pairs.tsv>...')
import collections
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bfsha

POS = re.compile(r'^(.*?)#(BNSH|BAHS|NDSR|NDVE)@(0x[0-9a-f]+)(.*)$')
VAR = re.compile(r'^#var(\d+)$')
DIGITS = re.compile(r'\d+')


def read_map(path):
    """key → (file, container kind, container offset, what in it, stage), or None for a source without a container"""
    out = {}
    with open(path, encoding='utf-8') as f:
        for ln in f:
            if ln.startswith(('#', 'package\t')): continue
            c = ln.rstrip('\n').split('\t')
            if len(c) < 5: continue
            m = POS.match(c[4])
            out[c[3]] = (m[1], m[2], int(m[3], 16), m[4], c[2]) if m else None
    return out


def read_pairs(paths):
    out = set()
    for p in paths:
        try:
            with open(p, encoding='utf-8') as f:
                for ln in f:
                    c = ln.split()
                    if len(c) >= 2 and not ln.startswith('#'): out.add((c[0], c[1]))
        except FileNotFoundError: pass
    return out


def file_names(a, b):
    """the old map's file names under the new map's: the same name, or the one name of the other side that differs only in its numbers"""
    fa, fb = {v[0] for v in a.values() if v}, {v[0] for v in b.values() if v}
    out = {n: n for n in fa & fb}
    loose_a, loose_b = collections.defaultdict(list), collections.defaultdict(list)
    for n in fa - fb: loose_a[DIGITS.sub('#', n)].append(n)
    for n in fb - fa: loose_b[DIGITS.sub('#', n)].append(n)
    for k, ns in loose_a.items():
        if len(ns) == 1 and len(loose_b.get(k, ())) == 1: out[ns[0]] = loose_b[k][0]
    return out


def key_map(a, b):
    """old key → new key: the same key, or the key at the aligned place"""
    names = file_names(a, b)
    a = {k: (names[v[0]],) + v[1:] if v and v[0] in names else None for k, v in a.items()}
    at_b = {v: k for k, v in b.items() if v}
    cont_a, cont_b = collections.defaultdict(set), collections.defaultdict(set)
    for v in a.values():
        if v: cont_a[v[:2]].add(v[2])
    for v in b.values():
        if v: cont_b[v[:2]].add(v[2])
    anchors, moved = collections.defaultdict(dict), set()
    for k, v in a.items():
        w = b.get(k)
        if not v or not w or w[:2] != v[:2]: continue
        anchors[v[:2]].setdefault(v[2], w[2])
        if w[3:] != v[3:]: moved.add((v[:2], v[2]))
    place = {}
    for fk, offs in cont_a.items():
        A, B = sorted(offs), sorted(cont_b.get(fk, ()))
        if not B: continue
        ia, ib = {o: i for i, o in enumerate(A)}, {o: i for i, o in enumerate(B)}
        bounds, last = [(-1, -1)], -1
        for oa, ob in sorted(anchors.get(fk, {}).items()):   # in order on both sides
            if ib[ob] > last: bounds.append((ia[oa], ib[ob])); last = ib[ob]
        bounds.append((len(A), len(B)))
        for (a0, b0), (a1, b1) in zip(bounds, bounds[1:]):
            if a0 >= 0: place[(fk, A[a0])] = B[b0]
            if a1 - a0 == b1 - b0:
                for d in range(1, a1 - a0): place[(fk, A[a0 + d])] = B[b0 + d]
    na = collections.Counter((v[:2], v[2]) for v in a.values() if v)
    nb = collections.Counter((v[:2], v[2]) for v in b.values() if v)
    out = {}
    for k, v in a.items():
        if k in b: out[k] = k; continue
        if not v or (v[:2], v[2]) in moved: continue
        ob = place.get((v[:2], v[2]))
        if ob is None or na[(v[:2], v[2])] != nb[(v[:2], ob)]: continue
        t = at_b.get(v[:2] + (ob,) + v[3:])
        if t and t not in a: out[k] = t
    return out


def varno(s):
    m = VAR.match(s)
    return int(m.group(1)) if m else None


def vsetb_keys(a, b):
    """old key → new key for keys in an effect set's BNSH (a .vsetb file) whose variations an update renumbered: the containers of one file pair in order,
    and within a paired container the variations whose keys both versions share anchor the order; the variations between two anchors pair in order only
    when both sides hold as many. A container with a different number of BNSHs in the new version pairs with none."""
    names = file_names(a, b)
    at = {}   # (file, BNSH offset, variation, stage) → key of the new map
    for k, v in b.items():
        n = varno(v[3]) if v and v[1] == 'BNSH' else None
        if n is not None: at[(v[0], v[2], n, v[4])] = k
    cont = collections.defaultdict(list)   # (file, BNSH offset) → old keys
    for k, v in a.items():
        n = varno(v[3]) if v and v[0] in names and v[0].endswith('.vsetb.byml.zs') and v[1] == 'BNSH' else None
        if n is not None: cont[(names[v[0]], v[2])].append(k)
    files = collections.defaultdict(list)
    for f, off in cont: files[f].append(off)
    out = {}
    for f, offs in sorted(files.items()):
        nb = sorted({v[2] for v in b.values() if v and v[0] == f and v[1] == 'BNSH'})
        if len(nb) != len(offs): continue   # a vsetb holds one graphics and one compute BNSH; another count pairs with none
        for oa, ob in zip(sorted(offs), nb):
            old = {k: a[k] for k in cont[(f, oa)]}
            anchor = {}   # old variation → new variation, for keys both versions hold in the paired containers
            for k, v in old.items():
                w = b.get(k)
                if w and w[0] == f and w[2] == ob:
                    mv, ov = varno(w[3]), varno(v[3])
                    if mv is not None and ov is not None: anchor.setdefault(ov, mv)
            ovs = sorted({n for n in (varno(v[3]) for v in old.values()) if n is not None})
            nvs = sorted({n for v in b.values() if v and v[0] == f and v[2] == ob for n in (varno(v[3]),) if n is not None})
            if not ovs or not nvs: continue
            oi, ni = {v: i for i, v in enumerate(ovs)}, {v: i for i, v in enumerate(nvs)}
            pairs = {}
            bounds = [(-1, -1)] + [(oi[x], ni[anchor[x]]) for x in sorted(anchor) if x in oi and anchor[x] in ni] + [(len(ovs), len(nvs))]
            for (a0, b0), (a1, b1) in zip(bounds, bounds[1:]):
                if a0 >= 0: pairs[ovs[a0]] = nvs[b0]
                if a1 - a0 == b1 - b0:
                    for d in range(1, a1 - a0):
                        if a0 + d < len(ovs) and b0 + d < len(nvs): pairs[ovs[a0 + d]] = nvs[b0 + d]
            for k, v in old.items():
                ov = varno(v[3])
                if ov is None: continue
                nv = pairs.get(ov)
                t = nv is not None and at.get((f, ob, nv, v[4]))
                if t and t not in a: out[k] = t
    return out


def program_keys(a, b, km, programs, romfs, warn=print):
    """old key → new key for keys km lacks whose old variation is a shader archive's program (dm/tools/bfsha.py's table of the old version): the new version's
    program of the same shading model with the same options, read from its archive in romfs, gives the key of the same stage"""
    names, want = file_names(a, b), collections.defaultdict(list)
    for k, (model, digest) in programs.items():
        v = a.get(k)
        if k not in km and v and v[1] == 'BNSH' and v[0] in names: want[names[v[0]]].append((k, v[4], model, digest))
    out = {}
    for path, keys in sorted(want.items()):
        try:
            d = bfsha.read_member(romfs, path)
            at = {(name, digest): (bnsh, var) for name, bnsh, ids in bfsha.programs(d, path) for var, digest in ids.items()}
            per = {bnsh: bfsha.variation_keys(d, bnsh, path) for bnsh in {at[m, g][0] for _, _, m, g in keys if (m, g) in at}}
        except bfsha.Unread as e:
            warn(f'[RED] {e}: the prewarm list\'s rows of its {len(keys):,} keys are not carried'); continue
        for k, st, model, digest in keys:
            if (model, digest) not in at: continue
            bnsh, var = at[model, digest]
            t = per[bnsh].get(var, {}).get(bfsha.STAGE.get(st))
            if t and t not in a and b.get(t) and b[t][4] == st: out[k] = t
    return out


def _field(h, i):
    return bytes.fromhex(h[48 * i:48 * i + 48]).split(b'\0', 1)[0].decode('ascii', 'replace')


def _put(h, i, name):
    return h[:48 * i] + name.encode().ljust(24, b'\0').hex() + h[48 * i + 48:]


def carry(manifest_lines, km, pairs):
    """→ (carried lines, Counter of (row kind, 'all' / 'kept' / 'carried'))"""
    def new(name):   # '<key>_b' or ''
        if not name: return ''
        k, sfx = name[:16], name[16:]
        return km[k] + sfx if k in km else None
    out, seen, st = [], set(), collections.Counter()
    for ln in manifest_lines:
        if not ln.strip() or ln.startswith('#'): continue
        c = ln.rstrip('\n').split('\t')
        kind = c[0]; st[kind, 'all'] += 1
        names = []
        if kind in ('S', 'C') and len(c) >= 2: names = [c[1]]
        elif kind == 'R' and len(c) >= 2: names = [_field(c[1], 0), _field(c[1], 1)]
        elif kind == 'M' and len(c) == 3: names = [_field(c[2], 0), _field(c[2], 1)] + ([] if c[1] == '-' else [c[1]])
        else: continue
        maybe = [new(n) for n in names]
        got = [g for g in maybe if g is not None]
        if len(got) != len(names) or not got[0]: continue
        same = got == names
        if kind in ('R', 'M') and names[1] and not same and (got[0][:16], got[1][:16]) not in pairs: continue
        if kind in ('S', 'C'): c[1] = got[0]
        elif kind == 'R': c[1] = _put(_put(c[1], 0, got[0]), 1, got[1])
        else:
            c[2] = _put(_put(c[2], 0, got[0]), 1, got[1])
            if c[1] != '-': c[1] = got[2]
        row = '\t'.join(c)
        if row in seen: continue
        seen.add(row); out.append(row)
        st[kind, 'kept' if same else 'carried'] += 1
    return out, st


def carry_file(from_map, to_map, from_manifest, out_manifest, pair_paths, origin='', programs=None, romfs=None, warn=print):
    """programs: the old version's dm/tools/bfsha.py table, romfs: the new version's RomFS; with both, a shader archive's keys are found again by their program"""
    a, b = read_map(from_map), read_map(to_map)
    km = key_map(a, b)
    vsetb = {k: v for k, v in vsetb_keys(a, b).items() if k not in km}
    km.update(vsetb)
    found = {}
    if programs and romfs:
        found = program_keys(a, b, km, bfsha.read_table(programs), romfs, warn)
        km.update(found)
    with open(from_manifest, encoding='utf-8') as f: lines = f.readlines()
    rows, st = carry(lines, km, read_pairs(pair_paths))
    total = sum(n for (k, w), n in st.items() if w == 'all')
    kept = sum(n for (k, w), n in st.items() if w == 'kept')
    moved = sum(n for (k, w), n in st.items() if w == 'carried')
    by = ', '.join(f'{k} {st[k, "kept"] + st[k, "carried"]:,} of {st[k, "all"]:,}' for k in 'SRCM' if st[k, 'all'])
    note = (f'{kept + moved:,} of {total:,} rows ({kept:,} with the same keys, {moved:,} carried to this version\'s keys; {by}'
            + (f'; {len(vsetb):,} effect-set (vsetb) keys found again by their variation\'s anchored place' if vsetb else '')
            + (f'; {len(found):,} shader archive keys found again by their program\'s options' if programs and romfs else '') + ')')
    head = [f'# Prewarm seed list carried by dm/tools/pso_carry.py from {origin or from_manifest}: {note}. The original list\'s comments follow.\n']
    with open(out_manifest + '.partial', 'w', encoding='utf-8') as f:
        f.writelines(head + [ln for ln in lines if ln.startswith('#')] + [r + '\n' for r in rows])
    os.replace(out_manifest + '.partial', out_manifest)
    return note


if __name__ == '__main__':
    a, opt = sys.argv[1:], {}
    while a[:1] in (['--programs'], ['--romfs']) and len(a) > 1: opt[a[0][2:]] = a[1]; a = a[2:]
    if len(a) < 5 or len(opt) == 1: sys.exit(USAGE)
    print(carry_file(a[0], a[1], a[2], a[3], a[4:], programs=opt.get('programs'), romfs=opt.get('romfs')))
