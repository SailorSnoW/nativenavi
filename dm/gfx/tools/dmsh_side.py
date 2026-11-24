# A pixel shader's side entry points from one Metal compile: the body once, as a static function with thin entry points around it, cut into <key>_b (plain),
# <key>_bx (_a, _l, main0_u) and <key>_bf (main0_f) (docs/reference/shaders.md#one-compile-for-a-pixel-shaders-side-entry-points). cut() takes functions out as
# `metallib --function-name` does; a change to what it gives needs a new dmsh_mtlcache.FORMAT.
# A vertex shader's libraries likewise (vs_unit): dm_vs_body always_inline in main0 / main0_z and their _p / _v versions, one out-of-line copy for the wide-line
# entries (never noinline: that changed results), main0_discard a whole copy (docs/reference/shaders.md#one-compile-for-a-vertex-shaders-libraries).
import re, struct
import dmsh_alpha, dmsh_logic, dmsh_pull

_ENTRY = re.compile(r'^((?:\[\[[^\]]*\]\]\s*)?)fragment\s+(\w+)\s+(\w+)\s*\(', re.M)   # an attribute may come first on the line ([[ early_fragment_tests ]])


def _close(src, i, o, c):
    """the index after the bracket closing the one opened just before i, -1 when there is none"""
    depth = 1
    for m in re.compile(re.escape(o) + '|' + re.escape(c)).finditer(src, i):
        depth += 1 if m.group() == o else -1
        if not depth: return m.end()
    return -1


def _params(s):   # commas inside <>, () or [] do not separate
    out, at, depth = [], 0, 0
    for i, ch in enumerate(s):
        depth += (ch in '<([') - (ch in '>)]')
        if ch == ',' and not depth:
            out.append(s[at:i].strip())
            at = i + 1
    return out + ([s[at:].strip()] if s[at:].strip() else [])


def _entry(msl, m):
    attr, ret, name = m.groups()
    head = _close(msl, m.end(), '(', ')')
    body = msl.find('{', head) if head >= 0 else -1
    end = _close(msl, body + 1, '{', '}') if body >= 0 else -1
    if end < 0: raise ValueError(f'no body for {name}')
    params = _params(msl[m.end():head - 1])
    decl, args = [], []
    for p in params:
        d = re.sub(r'\s*\[\[.*\]\]$', '', p)
        a = re.search(r'(\w+)$', d)
        if not a or 'function_constant' in p: raise ValueError(f'a parameter of {name}: {p[:80]}')
        decl.append(d)
        args.append(a.group(1))
    return {'attr': attr, 'ret': ret, 'name': name, 'params': params, 'decl': ', '.join(decl), 'args': ', '.join(args), 'start': m.start(), 'head': head, 'end': end}


def entries(msl, skip=()):
    """attr, ret, name, params, decl (without attributes), args, start, head, end. Raises ValueError on a shape it does not know"""
    return [_entry(msl, m) for m in _ENTRY.finditer(msl) if m.group(3) not in skip]


def entry(msl, name):   # the others are not read (a unit's _a and _l take function constants)
    return next((_entry(msl, m) for m in _ENTRY.finditer(msl) if m.group(3) == name), None)


# main0's body once, called by main0, main0_u, main0_d and main0_f. always_inline: the constants main0_f passes fold
_BODY = re.compile(r'static (__attribute__\(\(always_inline\)\) )?main0_out dm_body_main0\(')
BODY = 'static __attribute__((always_inline)) main0_out dm_body_main0('


def body(msl):   # → (its start, the end of its parameter list, the end of its body), None
    m = _BODY.search(msl)
    if not m: return None
    head = _close(msl, m.end(), '(', ')')
    end = _close(msl, msl.find('{', head) + 1, '{', '}')
    return (m.start(), head, end) if head >= 0 and end >= 0 else None


def shared_body(msl):
    """Unchanged when it has that already; None when main0's shape is unknown"""
    m = _BODY.search(msl)
    if m: return msl if m.group(1) else msl[:m.start()] + BODY + msl[m.end():]
    try: e = entry(msl, 'main0')
    except ValueError: return None
    if not e or e['ret'] != 'main0_out': return None
    return msl[:e['start']] + f'{BODY}{e["decl"]})' + msl[e['head']:e['end']] + '\n' + calling(e, 'main0', 'main0_out', 'return out;') + msl[e['end']:]


def calling(e, name, ret, tail):
    return f'{e["attr"]}fragment {ret} {name}({", ".join(e["params"])})\n{{\n    main0_out out = dm_body_main0({e["args"]});\n    {tail}\n}}\n'


def unit(msl) -> tuple[str | None, dict]:
    # main0_u holds its own alpha test and logic operation, so main0 gets no _l beside it; main0's _a stays: it has no constant, so the common alpha-tested
    # draw needs no specialization
    typed = bool(re.search(r'\bfragment\s+main0_out_u\s+main0_u\s*\(', msl))
    try: es = entries(msl, skip=('main0_u',))
    except ValueError as e: return None, {'why': str(e)}
    if not es or es[0]['name'] != 'main0': return None, {'why': 'no fragment entry point main0'}
    no_alpha, no_logic = dmsh_alpha.refused(msl), dmsh_logic.refused(msl)
    alpha = {e['name'] + '_a': dmsh_alpha.entry(msl, e) for e in es} if not no_alpha else {}
    if not all(alpha.values()): alpha, no_alpha = {}, 'an entry point without a colour 0'
    logic = {e['name'] + '_l': o for e in es if not (typed and e['name'] == 'main0') and (o := None if no_logic else dmsh_logic.entry(msl, e))}
    if not alpha and not logic: return None, {'why': no_alpha or no_logic or 'no colour output'}
    thin = [a for a in alpha.values() if a] + [o[0] for o in logic.values()]
    targets = set().union(*(o[1] for o in logic.values()))
    src, at = [], es[0]['start']
    shared = bool(_BODY.search(msl))
    for e in es:
        src += [msl[at:e['start']]] + ([] if shared and e['name'] == 'main0' else [f'static {e["ret"]} dm_body_{e["name"]}({e["decl"]})', msl[e['head']:e['end']]])
        at = e['end']
    plain = [f'{e["attr"]}fragment {e["ret"]} {e["name"]}({", ".join(e["params"])})\n{{\n    return dm_body_{e["name"]}({e["args"]});\n}}\n' for e in es]
    helpers = ('' if 'struct dm_alpha_state ' in msl else dmsh_alpha.HELPER) + ('' if 'struct dm_logic_state ' in msl else dmsh_logic.HELPER)   # main0_u brings them when it uses them
    return msl[:es[0]['start']] + helpers + dmsh_logic.constants(targets) + ''.join(src) + msl[at:] + '\n' + ''.join(plain + thin), {'alpha': list(alpha), 'logic': list(logic)}


_VENTRY = re.compile(r'^vertex (\S+) (\w+)\((.*)\)\n\{\n', re.M)   # its header on one line
_VEND = re.compile(r'\n\}(?:\n|$)')
VS_DEPTH = 'out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);'   # what vs_depth puts before every `return out;` of main0
VS_BODY = 'static __attribute__((always_inline)) main0_out dm_vs_body('
VS_BODY_OUT = 'static main0_out dm_vs_body_o('   # no noinline (see above)
VS_PULLED = {'main0': 'main0_p', 'main0_z': 'main0_zp'}
VS_SHADOW = {'main0': 'main0_v', 'main0_z': 'main0_zv'}


def vs_entries(src):
    """{name: ret, params, start, body, end}"""
    out = {}
    for m in _VENTRY.finditer(src):
        e = _VEND.search(src, m.end() - 1)
        if not e: raise ValueError(f'no end of {m[2]}')
        out[m[2]] = {'ret': m[1], 'params': m[3], 'start': m.start(), 'body': m.end(), 'end': e.end()}
    return out


def vs_unit(full, pulled=None, shadowed=None):
    es = vs_entries(full)
    if 'main0' not in es or 'main0_z' not in es: raise ValueError('no main0 and main0_z')
    e0, ez = es['main0'], es['main0_z']
    if e0['end'] > ez['start'] or (e0['ret'], ez['ret']) != ('main0_out', 'main0_out') or e0['params'] != ez['params']: raise ValueError('main0 and main0_z differ in more than their bodies')
    gl, z = full[e0['body']:e0['end']], full[ez['body']:ez['end']]
    if 'return out;' not in z or gl != z.replace('return out;', VS_DEPTH + '\n    return out;'): raise ValueError('main0 is not main0_z with the depth line')
    taken = [n for n in (*VS_PULLED.values(), *VS_SHADOW.values(), 'main0_wl', 'main0_z_wl') if n in es] + [n for n in ('dm_vs_body', 'dm_wl_in') if n + '(' in full]
    if taken: raise ValueError(f'the source has a {taken[0]} already')
    decl, args = [], []
    for p in _params(e0['params']):
        d = re.sub(r'\s*\[\[.*\]\]$', '', p)
        a = re.search(r'(\w+)$', d)
        if not a or 'function_constant' in p: raise ValueError(f'a parameter of main0: {p[:80]}')
        decl.append(d)
        args.append(a.group(1))
    decl, args = ', '.join(decl), ', '.join(args)
    def calling(text, fn, n_gl, n_z):
        if (text.count(gl), text.count(z)) != (n_gl, n_z): raise ValueError(f'{text.count(gl)} and {text.count(z)} copies of the body where {int(n_gl)} and {int(n_z)} belong')
        return text.replace(gl, f'    main0_out out = {fn}({args});\n    {VS_DEPTH}\n    return out;\n}}\n').replace(z, f'    return {fn}({args});\n}}\n')
    def renamed(variant, names):
        ve, text, got = vs_entries(variant), '', {}
        if 'main0' not in ve: raise ValueError('a variant without main0')
        for n, new in names.items():
            if n not in ve: continue
            one = variant[ve[n]['start']:ve[n]['end']]
            if not one.startswith(f'vertex main0_out {n}('): raise ValueError(f'the variant\'s {n} does not return main0_out')
            text += '\n' + calling(f'vertex main0_out {new}(' + one[len(f'vertex main0_out {n}('):], 'dm_vs_body', n == 'main0', n == 'main0_z')
            got['entry' if n == 'main0' else 'entry_z'] = new
        return text, got
    wide = bool(pulled) and '\nstatic inline main0_in dm_wl_in(' in pulled
    head = full[:e0['start']]
    if pulled:
        ui = head.find('using namespace metal;')
        if ui < 0: raise ValueError('no "using namespace metal;"')
        ui += len('using namespace metal;')
        head = head[:ui] + '\n\n' + dmsh_pull.FETCH + (dmsh_pull.WL_MSL if wide else '') + head[ui:]
    src = (head + f'{VS_BODY}{decl})\n{{\n' + z + '\n' + (f'{VS_BODY_OUT}{decl})\n{{\n    return dm_vs_body({args});\n}}\n\n' if wide else '')
           + calling(full[e0['start']:ez['end']], 'dm_vs_body', 1, 1) + full[ez['end']:])   # main0_discard, after them, stays a whole copy
    pn, vn = {}, {}
    if pulled:
        text, pn = renamed(pulled, VS_PULLED)
        src += text
        if wide:
            wl = pulled[pulled.index('\nstatic inline main0_in dm_wl_in('):]
            pn.update({k: n + '_wl' for n, k in (('main0', 'entry_wl'), ('main0_z', 'entry_z_wl')) if f'\nvertex main0_out {n}_wl(' in wl})
            src += calling(wl, 'dm_vs_body_o', 'entry_wl' in pn, 'entry_z_wl' in pn)
    if shadowed:
        st = re.search(r'\nstruct main0_in_v\n\{\n.*?\n\};', shadowed, re.S)
        if not st: raise ValueError('a shadow-input variant without main0_in_v')
        text, vn = renamed(shadowed, VS_SHADOW)
        src += '\n' + st[0] + '\n' + text
    return src, pn, vn


def _tags(b, p, end):
    out = []
    while p + 4 <= end:
        tag = b[p:p + 4]
        p += 4
        if tag == b'ENDT': return out
        n = struct.unpack_from('<H', b, p)[0]
        p += 2
        out.append((tag, b[p:p + n]))
        p += n
    raise ValueError('an unterminated tag list')


def _sized(b, p, n):   # n entries, each its size (without the size field) then its bytes
    out = []
    for _ in range(n):
        size = struct.unpack_from('<I', b, p)[0]
        out.append(b[p:p + 4 + size])
        p += 4 + size
    return out, p


def _take(b, keep, fns, tail, sections):
    fo, fs, po, ps, qo, qs, bo, bs = sections
    pub, _ = _sized(b, po, len(fns))
    priv, _ = _sized(b, qo, len(fns))
    entries_, newpub, newpriv, code = [], b'', b'', b''
    for i in keep:
        t = dict(fns[i])
        offs = struct.unpack('<QQQ', t[b'OFFT'])
        size = struct.unpack('<Q', t[b'MDSZ'])[0]
        body = b''.join(tag + struct.pack('<H', 24 if tag == b'OFFT' else len(v)) + (struct.pack('<QQQ', len(newpub), len(newpriv), len(code)) if tag == b'OFFT' else v) for tag, v in fns[i]) + b'ENDT'
        entries_.append(struct.pack('<I', len(body) + 4) + body)
        newpub += pub[i]
        newpriv += priv[i]
        code += b[bo + offs[2]:bo + offs[2] + size]
    listing = struct.pack('<I', len(keep)) + b''.join(entries_)
    all_entries = po - len(tail) - fo - 4
    nfo, npo = fo, fo + len(listing) + len(tail)
    nqo = npo + len(newpub)
    nbo = nqo + len(newpriv)
    head = bytearray(b[:fo])
    struct.pack_into('<Q', head, 0x10, nbo + len(code))
    struct.pack_into('<QQQQQQQQ', head, 0x18, nfo, fs - all_entries + len(listing) - 4, npo, len(newpub), nqo, len(newpriv), nbo, len(code))
    return bytes(head) + listing + tail + newpub + newpriv + code


def cut(lib, *groups):
    """(the base library, each group's library), from the layout pso_offline.functions reads. Raises ValueError on an unknown file, a missing name or a name in two groups"""
    try:
        if lib[:4] != b'MTLB' or struct.unpack_from('<Q', lib, 0x10)[0] != len(lib): raise ValueError('not a whole Metal library')
        sections = struct.unpack_from('<QQQQQQQQ', lib, 0x18)
        fo = sections[0]
        po = sections[2]
        bo, bs = sections[6:8]
        if bo + bs != len(lib): raise ValueError('its bitcode is not last')
        n = struct.unpack_from('<I', lib, fo)[0]
        p = fo + 4
        fns, names = [], []
        for _ in range(n):
            size = struct.unpack_from('<I', lib, p)[0]
            t = _tags(lib, p + 4, p + size)
            fns.append(t)
            names.append(dict(t)[b'NAME'].rstrip(b'\0').decode())
            p += size
        groups = [set(g) for g in groups]
        named = set().union(*groups)
        missing = named - set(names)
        if missing: raise ValueError(f'no function {sorted(missing)[0]}')
        if sum(map(len, groups)) != len(named): raise ValueError('a function in two groups')
        tail = lib[p:po]   # what lies between the function list and the public metadata stays
        return tuple(_take(lib, [i for i, x in enumerate(names) if keep(x)], fns, tail, sections) for keep in [lambda x: x not in named] + [g.__contains__ for g in groups])
    except (struct.error, KeyError) as e: raise ValueError(f'an unreadable library ({e})') from e
