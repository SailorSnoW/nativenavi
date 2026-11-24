"""Storage-buffer words the translator split (LDG.E.64 / .128 as 32-bit loads, whose addresses Metal cannot tell are neighbours) read whole again (wide_loads).

Words of one read-only buffer at byte offsets k, k+4, ... from one value in straight-line code become one packed_uint2/3/4 read. A run ends at any line that may
write memory or change a value, starts on a 16-byte step and holds at most four words. A bounded run (MetalStorageBounds) is read whole when its first and last
words are inside, else word by word as before.
"""
from collections import Counter, defaultdict
import re

import dmsh_abi as abi

_IDENT = r'_[0-9A-Za-z_]+'
_BUF = r'[a-z]p_s\d+'   # storage buffer bindings: vp_ / fp_ / cp_ / gp_ ... s<n>
_ADD = re.compile(r'^\s*int (' + _IDENT + r') = (' + _IDENT + r') \+ (-?\d+);\s*$')
_IDX = re.compile(r'^\s*uint (' + _IDENT + r') = uint\(int\(uint\((' + _IDENT + r')\) >> uint\(2\)\)\);\s*$')
_LOAD = re.compile(r'\(\*spvDescriptorSet0\.(' + _BUF + r')\)\._m0\[int\((' + _IDENT + r')\)\]')
_RO = re.compile(r'^\s*const device ' + _IDENT + r'\* (' + _BUF + r') \[\[id\(\d+\)\]\];', re.M)
_OP = r'(?:[-+*/%|&^]|<<|>>)?=(?!=)'
_WRITE = re.compile(r'atomic_|_barrier\(|\.write\(|\]\s*' + _OP + r'|^\s*\*|^\s*' + _IDENT + r'\s*' + _OP)   # lines a run does not reach across
_VEC = {2: 'packed_uint2', 3: 'packed_uint3', 4: 'packed_uint4'}
_SIGNED = re.compile(r'^\s*int (' + _IDENT + r') = int\((' + _IDENT + r')\);\s*$')
_TABLE = r'\(\*spvDescriptorSet0\.[a-z]+_c%d\)\._m0\[\d+\]\.[xyzw]' % abi.SBLEN_BANK
_INSIDE = re.compile(r'^\s*bool (' + _IDENT + r') = \(uint\((' + _IDENT + r')\) \+ (' + _TABLE + r')\) < (' + _TABLE + r');\s*$')
_BLOAD = re.compile(r'select\(0u, \(\*spvDescriptorSet0\.(' + _BUF + r')\)\._m0\[(' + _IDENT + r') \? (' + _IDENT + r') : 0\], \2\)')   # as dmsh_rewrite.select_guards writes it


def _root(v, adds):   # (value, byte offset) the sums lead back to
    off = 0
    for _ in range(64):
        if v not in adds: break
        v, k = adds[v]
        off += k
    return v, off


def wide_loads(msl):
    if ' >> uint(2)' not in msl: return msl
    ro = set(_RO.findall(msl))
    if not ro: return msl
    lines = msl.split('\n')
    adds, idx, signed, inside = {}, {}, {}, {}
    for ln in lines:
        m = _ADD.match(ln)
        if m:
            adds[m.group(1)] = (m.group(2), int(m.group(3)))
            continue
        m = _IDX.match(ln)
        if m:
            idx[m.group(1)] = m.group(2)
            continue
        m = _SIGNED.match(ln)
        if m:
            signed[m.group(1)] = m.group(2)
            continue
        m = _INSIDE.match(ln)
        if m: inside[m.group(1)] = m.group(2, 3, 4)   # (signed index, words before, words in all)
    words, block = {}, 0   # (block, buffer, value, bound) → {byte offset: (line, match, bounded read's (signed index, before, total) or None)}
    for i, ln in enumerate(lines):
        if '{' in ln or '}' in ln or _WRITE.search(ln):
            block += 1
            continue
        ms, bs = list(_LOAD.finditer(ln)), list(_BLOAD.finditer(ln))
        if len(ms) + len(bs) != 1: continue
        if ms:
            if ms[0].group(1) not in ro or ms[0].group(2) not in idx: continue
            m, at, bound = ms[0], idx[ms[0].group(2)], None
        else:
            m = bs[0]
            bound = inside.get(m.group(2))
            if m.group(1) not in ro or not bound or bound[0] != m.group(3) or signed.get(m.group(3)) not in idx: continue
            at = idx[signed[m.group(3)]]
        v, off = _root(at, adds)
        if off % 4 == 0: words.setdefault((block, m.group(1), v, bound[1:] if bound else None), {}).setdefault(off, (i, m, bound))
    edits, n = {}, 0
    for (_, buf, _, _), by_off in words.items():
        for base in sorted({off - off % 16 for off in by_off}):
            run = []
            while len(run) < 4 and base + 4 * len(run) in by_off: run.append(by_off[base + 4 * len(run)])
            if len(run) < 2 or any(i < run[0][0] for i, _, _ in run): continue
            name, vec = f'_dmw{n}', _VEC[len(run)]
            n += 1
            src = f'(*spvDescriptorSet0.{buf})._m0'
            if run[0][2] is None: decl = f'{vec} {name} = *(const device {vec}*)&{src}[int({run[0][1].group(2)})];'
            else:
                first, before, total = run[0][2]
                ins = [f'(uint({first} + {k}) + {before}) < {total}' for k in range(len(run))]
                lanes = ', '.join(f'select(0u, {src}[{ins[k]} ? {first} + {k} : 0], {ins[k]})' for k in range(len(run)))
                decl = (f'bool {name}_in = {ins[0]} && {ins[-1]};\n'
                        f'{vec} {name} = {name}_in ? *(const device {vec}*)&{src}[{first}] : {vec}({lanes});')
            for w, (i, m, _) in enumerate(run): edits.setdefault(i, []).append((m.start(), m.end(), f'{name}.{"xyzw"[w]}', decl if w == 0 else None))
    for i, es in edits.items():
        ln, pre = lines[i], []
        for s, e, rep, decl in sorted(es, reverse=True):
            ln = ln[:s] + rep + ln[e:]
            if decl: pre.append(decl)
        indent = lines[i][:len(lines[i]) - len(lines[i].lstrip())]
        lines[i] = ''.join(indent + d.replace('\n', '\n' + indent) + '\n' for d in pre) + ln
    return '\n'.join(lines)


# constant_reads: reads at uniform addresses of the constant address space can stay in constant registers; only buffers whose every use is a word read of either form
_RO_DECL = re.compile(r'^\s*(const device) ' + _IDENT + r'\* (' + _BUF + r') \[\[id\(\d+\)\]\];', re.M)
_STORAGE_NAME = re.compile(r'\b' + _BUF + r'\b')
_WORD_READ = re.compile(r'(?:\*\((?P<cast>const device) packed_uint[234]\*\)(?=&\(\*))?'
                        r'(?P<address>&\s*(?:\(\s*)*)?\(\*spvDescriptorSet0\.(?P<buf>' + _BUF + r')\)\._m0\[')
_DEVICE_REFERENCE = re.compile(r'\bdevice\s+[^;{}()]*&')


def constant_reads(msl, read_only, mode='fp'):
    """Promote translator-confirmed read-only slots with no escaping pointer or reference."""
    if mode in ('0', 'off', '') or not read_only or not _RO.search(msl): return msl
    # A helper taking a device reference can borrow an array element without an address-of operator.
    if _DEVICE_REFERENCE.search(msl): return msl
    stages = {stage.strip() for stage in mode.split(',')}
    mentions = Counter(_STORAGE_NAME.findall(msl))
    reads = defaultdict(list)
    for access in _WORD_READ.finditer(msl):
        reads[access['buf']].append(access)
    edits = []
    for decl in _RO_DECL.finditer(msl):
        buf = decl[2]
        if int(buf[4:]) not in read_only or (mode != 'all' and buf[:2] not in stages): continue
        accesses = reads[buf]
        if not accesses or len(accesses) + 1 != mentions[buf]: continue
        if any(access['address'] and not access['cast'] for access in accesses): continue
        edits.append(decl.span(1))
        edits.extend(access.span('cast') for access in accesses if access['cast'])
    for start, end in sorted(edits, reverse=True):
        msl = msl[:start] + 'constant' + msl[end:]
    return msl
