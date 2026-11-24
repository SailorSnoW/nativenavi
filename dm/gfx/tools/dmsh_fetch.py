# Readable-tile version main0_f: a draw that samples at its own pixel an attachment an earlier draw of the pass wrote reads the tile instead (framebuffer fetch).
# Function constants: FX_i = i (the colour index the i-th rewritable sample reads, -1: sample), FB_k = 32 + k (colour input k), OM_j_k = 64 + 8 j + k (output j writes attachment k).
# Rewritable: a 2D float .sample at an unchanged varying, a projective one ('proj', screen_varyings) or gl_FragCoord * uniform ('frag_pos'); the host proves at run time
# that the texel is this pixel's, else uses the original version
import collections
import re
import dmsh_abi as abi
import dmsh_side

NFB = 8

def _once(msl, name):   # assigned exactly once (its declaration): spirv-cross also declares first and assigns in branches
    # Found as text, then the character before checked: a leading lookbehind makes re try every character
    text, n = name + ' = ', 0
    i = msl.find(text)
    while i >= 0:
        if not i or not (msl[i - 1] in '._' or msl[i - 1].isalnum()): n += 1
        i = msl.find(text, i + len(text))
    return n == 1

def _proj(msl, var, comp, locn):
    """var = in.m_X.<comp> / in.m_X.w (a product with 1.0 / w or a quotient) → m_X, or None"""
    def src(n, c):   # float n = in.m_X.c; → m_X
        m = re.search(r'float %s = in\.(m_\d+)\.%s;' % (re.escape(n), c), msl)
        return m.group(1) if m and _once(msl, n) and m.group(1) in locn else None
    if not _once(msl, var): return None
    m = re.search(r'float %s = (_\d+) \* (_\d+);' % re.escape(var), msl)
    if m:
        for x, r in ((m.group(1), m.group(2)), (m.group(2), m.group(1))):
            d = re.search(r'float %s = 1\.0 / (_\d+);' % re.escape(r), msl)
            if d and _once(msl, r) and src(x, comp) and src(x, comp) == src(d.group(1), 'w'): return src(x, comp)
        return None
    m = re.search(r'float %s = (_\d+) / (_\d+);' % re.escape(var), msl)
    return src(m.group(1), comp) if m and src(m.group(1), comp) == src(m.group(2), 'w') else None

_UNI = re.compile(r'\(\*spvDescriptorSet0\.fp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])')

_SCALE = '(*spvDescriptorSet0.support_buffer)._m5[0].x'   # the support buffer's render scale, which the host fills with 1

def _frag_pos(msl, var, comp):
    """→ [uniform slot, vector, component] (fp_cN is slot N − 3), or None"""
    def one(n, pat):
        m = re.search(r'float %s = %s;' % (re.escape(n), pat), msl)
        return m if m and _once(msl, n) else None
    def pixel(n):
        if one(n, r'gl_FragCoord\.%s' % comp): return True
        q = one(n, r'(_\d+) / (_\d+)')
        return bool(q and one(q.group(1), r'gl_FragCoord\.%s' % comp) and one(q.group(2), re.escape(_SCALE)))
    m = one(var, r'(\S+) \* (\S+)')
    if not m: return None
    for a, u in ((m.group(1), m.group(2)), (m.group(2), m.group(1))):
        k = _UNI.fullmatch(u)
        if k and int(k.group(1)) >= 3 and re.fullmatch(r'_\d+', a) and pixel(a): return [int(k.group(1)) - 3, int(k.group(2)), 'xyzw'.index(k.group(3))]
    return None

# Screen varyings: locn K is the clip → screen varying when every vertex entry point sets .x = (P.x + P.w) / 2, .y = (P.w − P.y) / 2, .w = P.w (P the final
# gl_Position) at top level after any branch, from variables assigned once as linear forms of P. u runs from the left, v from the top
_TOK = re.compile(r'\s*(?:(out\.gl_Position\.[xyzw])|(_\d+)|(\d+\.\d*(?:e[+-]?\d+)?|\d+)f?|(fma)|([-+*/(),]))')

def _lin(expr, env):
    """{symbol: coefficient} ('1' the constant), or None"""
    toks, pos = [], 0
    while pos < len(expr):
        m = _TOK.match(expr, pos)
        if not m or m.end() == pos: return None
        toks.append(m.group(1) or m.group(2) or ('#' + m.group(3) if m.group(3) else None) or m.group(4) or m.group(5))
        pos = m.end()
    toks = [t for t in toks if t]
    i = [0]
    def add(a, b, s=1.0):
        r = dict(a)
        for k_, v in b.items(): r[k_] = r.get(k_, 0.0) + s * v
        return {k_: v for k_, v in r.items() if v}
    def const(a): return set(a) <= {'1'}
    def scale(a, c): return {k_: v * c for k_, v in a.items() if v * c}
    def peek(): return toks[i[0]] if i[0] < len(toks) else None
    def take(t=None):
        x = peek()
        if x is None or (t and x != t): raise ValueError
        i[0] += 1
        return x
    def atom():
        t = take()
        if t == '-': return scale(unary(), -1.0)
        if t == '(':
            v = add_()
            take(')')
            return v
        if t == 'fma':
            take('(')
            a = add_()
            take(',')
            b = add_()
            take(',')
            c = add_()
            take(')')
            return add(mul(a, b), c)
        if t.startswith('#'): return {'1': float(t[1:])} if float(t[1:]) else {}
        v = env(t)
        if v is None: raise ValueError
        return v
    def unary(): return atom()
    def mul(a, b):
        if const(a): return scale(b, a.get('1', 0.0))
        if const(b): return scale(a, b.get('1', 0.0))
        raise ValueError
    def mul_():
        v = unary()
        while peek() in ('*', '/'):
            op = take()
            w = unary()
            if op == '*': v = mul(v, w)
            elif const(w) and w.get('1'): v = scale(v, 1.0 / w['1'])
            else: raise ValueError
        return v
    def add_():
        v = mul_()
        while peek() in ('+', '-'):
            op = take()
            v = add(v, mul_(), 1.0 if op == '+' else -1.0)
        return v
    try:
        v = add_()
        return v if peek() is None else None
    except (ValueError, ZeroDivisionError):
        return None

def _entry_screen(body, locn):
    if len(re.findall(r'\breturn\b', body)) != 1 or not re.search(r'\n    return out;\n$', body + '\n'): return set()
    depth, stmts = 0, []
    lines = [ln.strip() for ln in body.split('\n')]
    for n, s in enumerate(lines):   # spirv-cross braces every block on lines of its own: a control statement without one is not read
        if re.match(r'(if|else|for|while|do|switch)\b', s) and (n + 1 >= len(lines) or lines[n + 1] != '{'): return set()
    for s in lines:
        if s == '{':
            depth += 1
            continue
        if s == '}':
            depth -= 1
            continue
        if s: stmts.append((depth, s))
    decl, last = {}, {}   # variable → (statement index, expression); output component → (statement index, depth, expression)
    nassign = collections.Counter(re.findall(r'(?<![\w.])(_\d+) = ', body))
    for n, (dp, s) in enumerate(stmts):
        m = re.fullmatch(r'(?:float|int|uint|bool|half) (_\d+) = (.+);', s)
        if m:
            decl[m.group(1)] = (n, dp, m.group(2))
            continue
        m = re.fullmatch(r'out\.(gl_Position|m_\d+)\.([xyzw]) = (.+);', s)
        if m:
            last[(m.group(1), m.group(2))] = (n, dp, m.group(3))
            continue
        m = re.fullmatch(r'out\.(gl_Position|m_\d+) = float4\((.+)\);', s)
        if m:
            parts = [p.strip() for p in m.group(2).split(',')]
            for c, p in zip('xyzw', parts if len(parts) == 4 else [None] * 4): last[(m.group(1), c)] = (n, dp, p)
    pos = {}
    for c in 'xyw':
        p = last.get(('gl_Position', c))
        if p is None or p[1] != 0 or p[2] is None: return set()
        pos[c] = p
    sym = {p[2]: 'P' + c for c, p in pos.items() if re.fullmatch(r'_\d+', p[2])}
    def env_at(at):
        memo = {}
        def env(name):
            if name.startswith('out.gl_Position.'):
                c = name[-1]
                p = pos.get(c)
                return {'P' + c: 1.0} if p and p[0] < at else None
            if name in sym: return {sym[name]: 1.0}
            if name in memo: return memo[name]
            d = decl.get(name)
            if not d or d[0] >= at or nassign[name] != 1: return None
            memo[name] = None
            memo[name] = _lin(d[2], env_at(d[0]))
            return memo[name]
        return env
    want = {'x': {'Px': 0.5, 'Pw': 0.5}, 'y': {'Py': -0.5, 'Pw': 0.5}, 'w': {'Pw': 1.0}}
    got = set()
    for mem, k in locn.items():
        ok = True
        for c, w in want.items():
            a = last.get((mem, c))
            if not a or a[1] != 0 or a[2] is None or _lin(a[2], env_at(a[0])) != w:
                ok = False
                break
        if ok: got.add(k)
    return got

def screen_varyings(msl):
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'float4 (m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    entries = list(re.finditer(r'\nvertex main0_out \w+\(.*?\)\n\{\n(.*?)\n\}\n', msl, re.S))
    if not locn or not entries: return []
    got = set(locn.values())
    for e in entries: got &= _entry_screen(e.group(1), locn)
    return sorted(got)

def fold(msl, bindings, om_only_ok=False):
    """Each rewritable sample in main0's shared body becomes (fx_i == k ? fb_k : the sample); other callers pass -1 and zeros, which the always-inline body folds back,
    so they need no function constants. → (source, {'calls', 'mixed_regs', 'outputs'}), or (None, why)"""
    src = dmsh_side.shared_body(msl)
    if src is None: return None, 'main function signature not recognized'
    try: where, e = dmsh_side.body(src), dmsh_side.entry(src, 'main0')
    except ValueError as x: return None, str(x)
    if not where or not e: return None, 'main function signature not recognized'
    start, head, end = where
    text = src[head:end]
    tex = {m.group(1) for m in re.finditer(r'texture2d<float> (\w+) \[\[id\(\d+\)\]\];', src)}
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'float4 (m_\d+) \[\[user\(locn(\d+)\)\]\];', src.split('struct main0_in', 1)[-1].split('};', 1)[0])}
    reg = {b['msl_name']: b['reg'] for b in bindings if b.get('kind') == abi.KIND_TEX and b.get('msl_name') and b.get('reg') is not None}
    # a tile read replaces a border-corrected sample (border_emu) whole
    pat = re.compile(r'(dm_bs\()?spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+)Smplr, select\((_\d+), float2\(0\.0\), isnan\(\4\)\)\)'
                     r'(?(1), spvDescriptorSet0\.%s, \d+u, spvDescriptorSet0\.\2, spvDescriptorSet0\.\3Smplr%s, select\(\4, float2\(0\.0\), isnan\(\4\)\)\))' % (abi.BORDER_MEMBER, abi.BORDER_TWIN))
    calls, out_parts, pos = [], [], 0
    for m in pat.finditer(text):
        member, var = m.group(2), m.group(4)
        if member not in tex or member != m.group(3) or member not in reg: continue
        d = re.search(r'float2 %s = float2\((_\d+), (_\d+)\);' % var, text)
        a = d and re.search(r'float %s = in\.(m_\d+)\.x;' % d.group(1), text)
        b = d and re.search(r'float %s = in\.(m_\d+)\.y;' % d.group(2), text)
        if a and b and a.group(1) == b.group(1) and a.group(1) in locn: how = {'varying_locn': locn[a.group(1)]}
        else:
            pa, pb = (_proj(text, d.group(1), 'x', locn), _proj(text, d.group(2), 'y', locn)) if d else (None, None)
            fx, fy = (_frag_pos(text, d.group(1), 'x'), _frag_pos(text, d.group(2), 'y')) if d else (None, None)
            if pa and pa == pb: how = {'varying_locn': locn[pa], 'proj': 1}
            elif fx and fy: how = {'frag_pos': [fx, fy]}
            else: continue
        i = len(calls)
        if i >= 32: break
        sel = ''.join(f'fx{i} == {k} ? fb{k} : ' for k in range(NFB))
        out_parts.append(text[pos:m.start()])
        out_parts.append(f'({sel}{m.group(0)})')
        pos = m.end()
        calls.append({'fc': i, 'tex_reg': reg[member], **how})
    if not calls and not om_only_ok:
        return None, 'no rewritable sample'
    # Mixed-use slots stay off tile reads: the two kinds of read would not see the same data
    conv = collections.Counter(c['tex_reg'] for c in calls)
    mixed = sorted({reg[mb] for mb in reg if mb in tex and text.count(f'spvDescriptorSet0.{mb}.') > conv[reg[mb]]})
    om = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not om: return None, 'no output struct'
    outs = [(t, n, int(c)) for t, n, c in re.findall(r'(\w+) (m_\d+) \[\[color\((\d+)\)\]\];', om.group(1))]
    if not outs or len(outs) > 4 or re.sub(r'\w+ m_\d+ \[\[color\(\d+\)\]\];', '', om.group(1)).strip(): return None, 'the output struct is not plain color outputs (or has more than 4)'
    rest = src[end:]
    if calls:
        params = [f'int fx{i}' for i in range(len(calls))] + [f'float4 fb{k}' for k in range(NFB)]
        off = ', '.join(['-1'] * len(calls) + ['float4(0.0)'] * NFB)
        sig = src[start:head - 1]
        sig += (', ' if not sig.endswith('(') else '') + ', '.join(params) + ')'
        parts, at = [], 0
        for m in re.finditer(r'\bdm_body_main0\(', rest):
            close = dmsh_side._close(rest, m.end(), '(', ')') - 1
            parts += [rest[at:close], (', ' if rest[m.end():close].strip() else '') + off]
            at = close
        rest = ''.join(parts) + rest[at:]
    else: sig = src[start:head]
    consts = ''.join(f'constant int FX_{c["fc"]} [[function_constant({c["fc"]})]];\n' for c in calls) + \
             ''.join(f'constant bool FB_{k} [[function_constant({32 + k})]];\n' for k in range(NFB)) + \
             ''.join(f'constant bool OM_{j}_{k} [[function_constant({64 + 8 * j + k})]];\n' for j in range(len(outs)) for k in range(NFB))
    lead = src[:start]
    if 'using namespace metal;\n' not in lead: return None, 'no using namespace metal'
    lead = lead.replace('using namespace metal;\n', 'using namespace metal;\n\n' + consts, 1)
    # a colour the pipeline does not read (FB_k false) is not passed to the body
    members = ''.join(f'    {t} {n}_at{k} [[color({k}), function_constant(OM_{j}_{k})]];\n' for j, (t, n, c) in enumerate(outs) for k in range(NFB))
    fbs = [f'float4 fb{k} [[color({k}), function_constant(FB_{k})]]' for k in range(NFB)]
    args = ', '.join(([e['args']] if e['args'] else []) + ([f'FX_{i}' for i in range(len(calls))] + [f'_fb{k}' for k in range(NFB)] if calls else []))
    load = ''.join(f'    float4 _fb{k} = float4(0.0);\n    if (FB_{k}) _fb{k} = fb{k};\n' for k in range(NFB)) if calls else ''
    put = ''.join(f'    if (OM_{j}_{k}) out.{n}_at{k} = r.{n};\n' for j, (t, n, c) in enumerate(outs) for k in range(NFB))
    main0_f = (f'\nstruct main0_out_f\n{{\n{members}}};\n\n{e["attr"]}fragment main0_out_f main0_f({", ".join(e["params"] + fbs)})\n{{\n'
               f'{load}    main0_out r = dm_body_main0({args});\n    main0_out_f out = {{}};\n{put}    return out;\n}}\n')
    return lead + sig + ''.join(out_parts) + text[pos:] + rest + main0_f, {'calls': calls, 'mixed_regs': mixed, 'outputs': [{'color': c, 'type': t} for t, n, c in outs]}
