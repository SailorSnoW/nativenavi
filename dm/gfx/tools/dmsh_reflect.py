#!/usr/bin/env python3
# Reflection of a translated shader's MSL for dmsh_build.py (text functions and dmsh_abi.py only, so tests import them).
import re
import dmsh_abi as abi, dmsh_fetch

# NVN's driver segments (dmsh_abi.h): c2 C2_TEX + 8 r combined slot r (any handle below the separate texture table: games bind past slot 32), C2_IMG + 8 r
# image r, C2_SEPTEX + 8 r separate texture (reported as SEPTEX_SLOT0 + r), C2_SAMP + 8 r separate sampler; c0 C0_SSBO_<stage> + 0x10 r storage buffer r
TEX_BASE, IMG_BASE, IMG_SLOTS, SEPTEX_BASE, SEPTEX_SLOT0, SAMP_BASE = abi.C2_TEX, abi.C2_IMG, abi.C2_IMG_SLOTS, abi.C2_SEPTEX, abi.SEPTEX_SLOT0, abi.C2_SAMP

SSBO = {'vs': abi.C0_SSBO_VS, 'cs': abi.C0_SSBO_CS, 'ps': abi.C0_SSBO_PS, 'ts': abi.C0_SSBO_TS, 'ms': abi.C0_SSBO_MS}

def attributes(src):
    """Searched only between the first and the last declaration's lines: the pattern is slow on a whole source"""
    i, j = src.find('[[attribute('), src.rfind('[[attribute(')
    if i < 0: return []
    end = src.find('\n', j)
    return list(re.finditer(r'(\w+)\s+(\w+)\s*\[\[attribute\((\d+)\)\]\]', src[src.rfind('\n', 0, i) + 1:end if end >= 0 else len(src)]))

def in_word(src, i):
    return i > 0 and (src[i - 1] == '_' or src[i - 1].isalnum())

def has_word(src, text):
    """re.search(r'\\b' + text, src) found as text (a leading \\b has no literal prefix to skip to)"""
    i = src.find(text)
    while i >= 0:
        if not in_word(src, i): return True
        i = src.find(text, i + 1)
    return False

def far_plane(body):
    """z = w in the last stores to gl_Position (one value, no branches): every vertex on the far value, which the host draws with a bias and clamping
    (docs/reference/rendering.md#depth-on-the-far-plane). vs_depth's z' = z/2 + w/2 keeps z = w, so it is skipped"""
    if re.search(r'\b(if|for|while|switch|do)\b', body) or re.search(r'out\.gl_Position(?:\.[xyzw]{2,})?\s*=[^=]', body): return False
    last = {}
    for m in re.finditer(r'out\.gl_Position\.([zw])\s*=\s*([^;]+);', body):
        v = m.group(2).strip()
        if m.group(1) == 'z' and v == 'fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5)': continue
        last[m.group(1)] = v
    v = last.get('z')
    if not v or v != last.get('w') or v in ('0.0', '-0.0'): return False
    if re.fullmatch(r'_\d+', v): return len(re.findall(r'(?<![\w.])%s\s*=[^=]' % re.escape(v), body)) == 1
    return re.fullmatch(r'-?\d+\.\d*(?:e[+-]?\d+)?', v) is not None

def raster_facts(stage, src):
    """Facts for the run-time proof that a first draw covers its target (nvn_mtl.m first_draw_covers): pos_attr, the attribute the position is taken from
    unchanged, else -1; a fragment shader's discard, sample_mask and colour writes. Anything not recognized counts as unprovable."""
    if stage == 'ps':
        out = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
        mem = out.group(1) if out else ''
        return {'discard': int('discard_fragment' in src), 'sample_mask': int('sample_mask' in mem), 'primitive_id': int('[[primitive_id]]' in src),
                'colors': sorted({int(x) for x in re.findall(r'\[\[color\((\d+)\)\]\]', mem)}),
                # bitmaps: a pipeline whose target takes another class takes main0_u
                **{k: sum(1 << int(c) for ty, c in re.findall(r'^\s*(\w+)\s+\w+\s*\[\[color\((\d+)\)[^\]]*\]\];', mem, re.M) if ty.startswith(p))
                   for k, p in (('float_colors', ('float', 'half')), ('uint_colors', ('uint',)), ('sint_colors', ('int',)))}}
    if stage != 'vs': return {}
    scr: dict[str, object] = {'screen': dmsh_fetch.screen_varyings(src)}
    body = re.search(r'\bvertex\s+\w+\s+\w+\s*\(.*?\)\s*\{(.*)\}\s*$', src, re.S)   # the main function is the last in the file
    if body and far_plane(body.group(1)): scr['far'] = 1
    if not body or re.search(r'\b(if|for|while|switch|do)\b|clip_distance|ClipDistance', body.group(1)): return {'pos_attr': -1, **scr}
    b = body.group(1)
    attr = {m.group(2): int(m.group(3)) for m in attributes(src)}
    var = {m.group(1): (m.group(2), m.group(3)) for m in re.finditer(r'float\s+(_\w+)\s*=\s*in\.(\w+)\.([xyzw]);', b)}
    last = {}
    for m in re.finditer(r'out\.gl_Position\.([xyzw])\s*=\s*([^;]+);', b): last[m.group(1)] = m.group(2).strip()
    src_of = {c: var.get(v) for c, v in last.items()}
    m0 = src_of.get('x')
    if not m0 or m0[0] not in attr or any(src_of.get(c) != (m0[0], c) for c in 'xyz'): return {'pos_attr': -1, **scr}
    if last.get('w') == '1.0': w = 'one'
    elif src_of.get('w') == (m0[0], 'w'): w = 'attr'
    else: return {'pos_attr': -1, **scr}
    # uv pass-through: locn K's .x / .y taken unchanged from attribute A's → uv_from_attr[K] = A
    ost = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    locn = {m.group(1): int(m.group(2)) for m in re.finditer(r'(m_\d+) \[\[user\(locn(\d+)\)\]\]', ost.group(1) if ost else '')}
    olast = {}
    for m in re.finditer(r'out\.(m_\d+)\.([xyzw])\s*=\s*([^;]+);', b): olast[(m.group(1), m.group(2))] = m.group(3).strip()
    uv, aff = {}, {}
    for mem, k in locn.items():
        sx, sy = var.get(olast.get((mem, 'x'), '')), var.get(olast.get((mem, 'y'), ''))
        if sx and sy and sx[0] == sy[0] and sx[0] in attr and sx[1] == 'x' and sy[1] == 'y':
            uv[str(k)] = attr[sx[0]]
            continue
        # vp_cN is uniform slot N − 3
        def fm(v_):
            m = re.search(r'float %s = fma\((_\w+), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw]), \(\*spvDescriptorSet0\.vp_c(\d+)\)\._m0\[(\d+)\]\.([xyzw])\);' % re.escape(v_), b)
            if not m or m.group(1) not in var: return None
            return var[m.group(1)], [int(m.group(2)) - 3, int(m.group(3)), 'xyzw'.index(m.group(4))], [int(m.group(5)) - 3, int(m.group(6)), 'xyzw'.index(m.group(7))]
        fx, fy = fm(olast.get((mem, 'x'), '')), fm(olast.get((mem, 'y'), ''))
        if fx and fy and fx[0][0] == fy[0][0] and fx[0][0] in attr and fx[0][1] == 'x' and fy[0][1] == 'y' and min(fx[1][0], fx[2][0], fy[1][0], fy[2][0]) >= 0:
            aff[str(k)] = {'attr': attr[fx[0][0]], 'sx': fx[1], 'ox': fx[2], 'sy': fy[1], 'oy': fy[2]}
    return {'pos_attr': attr[m0[0]], 'pos_w': w, 'uv_from_attr': uv, 'uv_affine': aff, **scr}

def decode_tex(name):
    """fp_t_cb{slots}_{handle} / fp_t_tcb_{handle} → texture slot, sampler slot (Ryujinx TextureHandle). Type 3 carries a constant sampler ID instead of a second offset"""
    m = re.match(r'[a-z]+_[ti]\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    slots = int(m.group(1)) if m.group(1) else -1
    h = int(m.group(2), 16)
    tb = (slots & 0xffff) if slots >= 0 else 2
    sb = ((slots >> 16) - 1) if slots >= 0 and (slots >> 16) else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    def slot(word, base, cb): return (word * 4 - base) // abi.C2_ROW if cb == 2 and word * 4 >= base and (word * 4 - base) % abi.C2_ROW == 0 else None
    words = [o0] + ([o1] if ty else [])
    if re.match(r'[a-z]+_i', name):
        return next((slot(x, IMG_BASE, tb) for x in words if IMG_BASE <= x * 4 < IMG_BASE + abi.C2_ROW * IMG_SLOTS and slot(x, IMG_BASE, tb) is not None), None), None, ty
    tw = [o0] if ty in (2, 3) else words
    t = next((slot(x, TEX_BASE, tb) for x in tw if slot(x, TEX_BASE, tb) is not None and x * 4 < SEPTEX_BASE), None)
    if t is None: t = next((SEPTEX_SLOT0 + slot(x, SEPTEX_BASE, tb) for x in tw if slot(x, SEPTEX_BASE, tb) is not None and x * 4 < SAMP_BASE), None)
    s = next((slot(x, SAMP_BASE, sb) for x in words if x * 4 >= SAMP_BASE and slot(x, SAMP_BASE, sb) is not None), None) if ty != 3 else None
    return t, s, ty

def const_sampler(name):
    """fp_t_cb65538_3041005C: the texture handle at c2 word 0x5C ORed with sampler 0x104"""
    m = re.match(r'[a-z]+_t\w*?_(?:cb(\d+)|tcb)_([0-9A-F]+)', name)
    if not m: return None
    h = int(m.group(2), 16)
    return (h >> 14) & 0x3fff if h >> 28 == 3 else None

def decode_ubo_tex(name):
    """fp_t_cb7_2A = c[7] word 0x2A → (texture: (uniform slot, byte offset), sampler: (slot, offset)). Handle type 0: one word, low 20 bits texture, high 12 sampler;
    1: the sampler in another word (slot: high 16 bits of slots − 1), or ('c2', byte offset) in the driver table; 3: ('id', ID)"""
    m = re.match(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name)
    if not m: return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    tb = slots & 0xffff
    sb = (slots >> 16) - 1 if slots >> 16 else tb
    o0, o1, ty = h & 0x3fff, (h >> 14) & 0x3fff, h >> 28
    if not 3 <= tb <= 16 or ty not in (0, 1, 3) or (ty == 1 and sb != 2 and not 3 <= sb <= 16): return None
    return (tb - 3, o0 * 4), (('id', o1) if ty == 3 else ('c2', o1 * 4) if ty and sb == 2 else (sb - 3, o1 * 4) if ty else (tb - 3, o0 * 4))

def decode_ubo_samp_id(name):
    """Handle type 2 (texture word | sampler ID << 20): texture in c2, the ID in a uniform buffer (fp_t_cb720898_2000005A: c2 word 0x5A, the ID at c[10] word 0)
    → (uniform slot, byte offset), or None"""
    m = re.fullmatch(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name)
    if not m: return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    sb = (slots >> 16) - 1
    return (sb - 3, ((h >> 14) & 0x3fff) * 4) if h >> 28 == 2 and slots & 0xffff == 2 and 3 <= sb <= 16 else None

def decode_ubo_img(name):
    """<stage>_i_cb<n>_<word>_<format> → (uniform slot, byte offset)"""
    m = re.match(r'[a-z]+_i\w*?_cb(\d+)_([0-9A-F]+)', name)
    return (int(m.group(1)) - 3, int(m.group(2), 16) * 4) if m and 3 <= int(m.group(1)) <= 16 else None

def buffer_image_bias(name, res):
    """The binding's index in the offset table (DMSH_TBIAS_BANK), "tbx" in the translator's images or textures, found by its name's slot and handle; an array's is its first element's"""
    m = re.match(r'[a-z]+_([ti])[a-z0-9]*_(?:cb(\d+)|tcb)_([0-9A-F]+)(?:_|$)', name)
    if not m: return None
    slots, h = int(m.group(2)) if m.group(2) else None, int(m.group(3), 16)
    return next((i['tbx'] for i in res.get('images' if m.group(1) == 'i' else 'textures', ()) if 'tbx' in i and i['handle'] == h and (i['cbuf_slot'] == slots if slots is not None else i['cbuf_slot'] < 0)), None)

def buffer_bias_entries(res):
    """every entry given out, an array's one per element"""
    n = max((i['tbx'] + max(i.get('array', 1), 1) * (2 if i.get('row_bias') else 1) for k in ('textures', 'images') for i in res.get(k, ()) if 'tbx' in i), default=0)
    return (n + 3) // 4 * 4

def c2_array(name, ty):
    """An array of c2 handles indexed at run time (<stage>_t<type>_cb<slots>_<handle>, index = byte offset ÷ 8; type 1 ORs one sampler word into each)
    → (c2 byte offset of element 0, of the sampler word or None), or None"""
    m = re.fullmatch(r'[a-z]+_t\w*?_cb(\d+)_([0-9A-F]+)', name[:-5] if name.endswith('Smplr') else name)
    if not m or not ty.startswith('array<'): return None
    slots, h = int(m.group(1)), int(m.group(2), 16)
    if h >> 28 == 0 and slots == 2: return (h & 0x3fff) * 4, None
    if h >> 28 == 1 and slots & 0xffff == 2 and (slots >> 16) - 1 == 2: return (h & 0x3fff) * 4, ((h >> 14) & 0x3fff) * 4
    return None

POOL_MIN = 33   # arrays at least this long are pools: the vendored BindlessElimination keeps 32-entry slot arrays

# A plain load: an atomic one goes to the shared cache every time, and a pass's pixels queue on a few words. A stale 0 costs an OR that changes nothing:
# a bit only goes from 0 to 1 while a submission runs
POOL_MARK = """template<typename P> static inline __attribute__((always_inline)) uint dm_pool_mark(constant P* p, uint i)
{
    device uint* w = reinterpret_cast<device uint*>(p->seen) + (i >> 5);
    uint b = 1u << (i & 31u);
    if (!(*w & b)) atomic_fetch_or_explicit(reinterpret_cast<device atomic_uint*>(w), b, memory_order_relaxed);
    return i;
}

"""

def _buffer_pool_operation(src, start, base, index):
    call = re.match(r'\.(read|get_width)\(', src[start:])
    if not call: raise ValueError(f'{base}: unsupported BUFFER table operation')
    first, depth = start + call.end(), 1
    end = first
    while depth:
        if end >= len(src): raise ValueError(f'{base}: unbalanced BUFFER operation')
        depth += {'(': 1, ')': -1}.get(src[end], 0)
        end += 1
    arg = src[first:end - 1]
    out = f'dm_buffer_read({base}, {index}, {arg})' if call[1] == 'read' else f'dm_buffer_width({base}, {index})'
    return end, out


def _callable_pool_arrays(src, pools):
    """Whole pools may only be forwarded unchanged to matching parameters; other aliases fail explicitly."""
    code = re.sub(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"',
                  lambda m: ''.join('\n' if c == '\n' else ' ' for c in m.group()), src, flags=re.S)

    def closing(start, left, right):
        depth = 1
        for i in range(start + 1, len(code)):
            depth += (code[i] == left) - (code[i] == right)
            if not depth: return i
        raise ValueError('pool callable: unbalanced delimiters')

    def arguments(start, end, types=False):
        result, first, stack = [], start, []
        pairs = {')': '(', ']': '[', '}': '{', **({'>': '<'} if types else {})}
        for i in range(start, end):
            c = code[i]
            if c in '([{' + ('<' if types else ''): stack.append(c)
            elif c in pairs:
                if not stack or stack.pop() != pairs[c]: raise ValueError('pool callable: unbalanced argument')
            elif c == ',' and not stack:
                result.append((first, i))
                first = i + 1
        if stack: raise ValueError('pool callable: unbalanced argument')
        if code[first:end].strip(): result.append((first, end))
        return result

    calls, definitions = [], {}
    for match in re.finditer(r'\b(\w+)\s*\(', code):
        if match.group(1) in ('if', 'for', 'while', 'switch', 'sizeof', 'alignof'): continue
        end = closing(match.end() - 1, '(', ')')
        following = re.match(r'\s*(.)', code[end + 1:])
        definition = following is not None and following.group(1) == '{'
        args = arguments(match.end(), end, types=definition)
        call = {'name': match.group(1), 'args': args}
        if definition and following is not None:
            call['body'] = end + following.end()
            call['body_end'] = closing(call['body'], '{', '}')
            call['pools'] = {}
            definitions.setdefault(call['name'], []).append(call)
        else: calls.append(call)

    allowed, edits = [], []
    member = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', code, re.S)
    if member is None: raise ValueError('pool callable: missing descriptor definition')
    for name, (element, length, _) in pools.items():
        pattern = r'constant\s+array<(.+),\s*(\d+)>\s*&\s*' + re.escape(name) + r'\s*$'
        for functions in definitions.values():
            for function in functions:
                for index, (start, end) in enumerate(function['args']):
                    text = code[start:end].strip()
                    if not re.search(r'\b' + re.escape(name) + r'\b', text): continue
                    parameter = re.fullmatch(pattern, text)
                    if not parameter or re.sub(r'\s', '', parameter.group(1)) != re.sub(r'\s', '', element) or int(parameter.group(2)) != length:
                        raise ValueError(f'{name}: unsupported callable pool parameter')
                    if start < member.end(): raise ValueError(f'{name}: pool callable precedes its descriptor definition')
                    function['pools'][index] = name
                    allowed.append((start, end))
                    edits.append((start, end, f'constant spvPool_{name}* {name}'))
        for match in re.finditer(r'\b' + re.escape(name) + r'\b', code[member.start():member.end()]):
            allowed.append((member.start() + match.start(), member.start() + match.end()))

    def source_pool(start, end, name):
        text = code[start:end].strip()
        if text != name: return bool(re.fullmatch(r'[\w.]+\.' + re.escape(name), text))
        return any(function['body'] < start < function['body_end'] and name in function['pools'].values()
                   for functions in definitions.values() for function in functions)

    for call in calls:
        for index, (start, end) in enumerate(call['args']):
            for name in pools:
                if not re.fullmatch(r'(?:[\w.]+\.)?' + re.escape(name), code[start:end].strip()): continue
                targets = definitions.get(call['name'], [])
                if len(targets) != 1 or targets[0]['pools'].get(index) != name or len(call['args']) != len(targets[0]['args']) or not source_pool(start, end, name):
                    raise ValueError(f'{name}: unsupported callable pool forwarding')
                allowed.append((start, end))

    for name, (_, _, marked) in pools.items():
        for match in re.finditer(r'\b' + re.escape(name) + r'\b', code):
            if any(a <= match.start() < b for a, b in allowed): continue
            prefix = re.search(r'([\w.]+)\.$', code[:match.start()])
            start = prefix.start() if prefix else match.start()
            end = match.end()
            bracket = re.match(r'\s*\[', code[end:])
            if not bracket or not source_pool(start, end, name):
                raise ValueError(f'{name}: unsupported callable pool use or alias')
            if code[:start].rstrip().endswith('&'):
                raise ValueError(f'{name}: taking a pool element address is unsupported')
            left = end + bracket.end() - 1
            right = closing(left, '[', ']')
            base = src[start:end]
            index = f'min(uint({src[left + 1:right]}), {base}->n)'
            use = f'dm_pool_mark({base}, {index})' if marked else index
            edits.append((start, right + 1, f'(&{base}->e[0])[{use}]'))
            allowed.append((start, right + 1))

    edits.sort()
    if any(a[1] > b[0] for a, b in zip(edits, edits[1:])):
        raise ValueError('pool callable: overlapping uses are unsupported')
    for start, end, replacement in reversed(edits): src = src[:start] + replacement + src[end:]
    return src

BUFFER_POOL_READ = """template<typename P> static inline __attribute__((always_inline)) auto dm_buffer_read(constant P* p, uint i, uint c)
{
    uint b = reinterpret_cast<constant uint*>(reinterpret_cast<constant uchar*>(p) + p->bias)[i];
    uint m = c + b;
    return (&p->e[0])[i].read(m < b ? c : m);
}
template<typename P> static inline __attribute__((always_inline)) uint dm_buffer_width(constant P* p, uint i)
{
    uint b = reinterpret_cast<constant uint*>(reinterpret_cast<constant uchar*>(p) + p->bias)[i];
    return (&p->e[0])[i].get_width() - b;
}

"""

def pool_arrays(src):
    """Ryujinx's arrays for handles traced to no slot (<stage>_<t|s>_cb0_40000000, sized to the whole ID range) → a pointer to a pool the replay keeps per element type,
    struct spvPool_<name> { uint n; ulong seen; array<element, 1> e; } read as (&e[0])[min(index, n)]; a texture read marks its ID in seen (dm_pool_mark), so the host makes
    only those. One declared element: Metal's argument encoder walks every element of a declared array. A table of a uniform buffer's handles takes the same form, unmarked.
    ValueError on a use the rewrite does not cover."""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', src, re.S)
    if not body: return src
    pools = {}
    def to_pointer(m):
        if int(m.group(3)) < POOL_MIN or not (m.group(4).endswith('_cb0_40000000') or decode_ubo_tex(m.group(4).removesuffix('Smplr'))): return m.group(0)
        pools[m.group(4)] = (m.group(2), int(m.group(3)), m.group(2) != 'sampler' and m.group(4).endswith('_cb0_40000000'))   # element, length, marked
        return f'{m.group(1)}constant spvPool_{m.group(4)}* {m.group(4)} [[id('
    members = re.sub(r'^(\s*)array<(.+), (\d+)> (\w+) \[\[id\(', to_pointer, body.group(1), flags=re.M)
    if not pools: return src
    callable_pools = any(re.search(r'array<[^;\n]+>\s*&\s*' + re.escape(name) + r'\b', src) for name in pools)
    if callable_pools:
        src = _callable_pool_arrays(src, pools)
        body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', src, re.S)
        if body is None: raise ValueError('pool callable: descriptor definition was lost')
        members = re.sub(r'^(\s*)array<(.+), (\d+)> (\w+) \[\[id\(', to_pointer, body.group(1), flags=re.M)
    aid = 0
    def renumber(m):
        nonlocal aid
        a = re.fullmatch(r'array<.+, (\d+)>', m.group(2).strip())
        out = f'{m.group(1)}{m.group(2)} {m.group(3)} [[id({aid}){m.group(5)}]];'
        aid += int(a.group(1)) if a else 1
        return out
    members = re.sub(r'^(\s*)(.+?)\s+(\w+)\s*\[\[id\((\d+)\)((?:\s*,\s*\w+\(\d+\))*)\]\];', renumber, members, flags=re.M)
    structs = ''.join(f'struct spvPool_{nm}\n{{\n    uint n;\n' + ('    uint bias;\n' if 'texture_buffer<' in el else '') + f'    ulong seen;\n    array<{el}, 1> e;\n}};\n\n' for nm, (el, n, _) in pools.items())
    if any('texture_buffer<' in el for el, _, _ in pools.values()): structs = BUFFER_POOL_READ + structs
    marked = any(mk for _, _, mk in pools.values())
    if marked: structs = POOL_MARK + structs
    i = body.start()
    src = src[:i] + structs + src[i:body.start(1)] + members + src[body.end(1):]
    for nm in (() if callable_pools else pools):
        out, pos = [], 0
        for m in re.finditer(r'([\w.]+)\.%s\[' % re.escape(nm), src):
            if m.start() < pos: continue
            depth, j = 1, m.end()
            while depth:
                if j >= len(src): raise ValueError(f'{nm}: unbalanced index')
                depth += {'[': 1, ']': -1}.get(src[j], 0)
                j += 1
            base = f'{m.group(1)}.{nm}'
            index = f'min(uint({src[m.end():j - 1]}), {base}->n)'
            selected = f"dm_pool_mark({base}, {index})" if pools[nm][2] else index
            replacement = f'(&{base}->e[0])[{selected}]'
            out.append(src[pos:m.start()] + replacement)
            pos = j
        src = ''.join(out) + src[pos:]
        names = 3 if pools[nm][2] else 2   # each read names it twice, and once more in a mark; the member once
        left = len(re.findall(r'\b%s\b' % re.escape(nm), src)) - names * len(re.findall(r'\.%s->e\[0\]\)\[' % re.escape(nm), src)) - 1
        if left: raise ValueError(f'{nm}: {left} uses of the pool array the rewrite does not cover')
    # Last access first: a nested read's replacement is already inside its caller's argument.
    for access in reversed(list(re.finditer(r'\(&([\w.]+)->e\[0\]\)\[', src))):
        base = access[1]
        name = base.rsplit('.', 1)[-1]
        if name not in pools or 'texture_buffer<' not in pools[name][0]: continue
        first, end, depth = access.end(), access.end(), 1
        while depth:
            if end >= len(src): raise ValueError(f'{name}: unbalanced BUFFER index')
            depth += {'[': 1, ']': -1}.get(src[end], 0)
            end += 1
        index = src[first:end - 1]
        end, replacement = _buffer_pool_operation(src, end, base, index)
        src = src[:access.start()] + replacement + src[end:]
    # The mark stores to memory, so Metal would test depth late (an EQUAL pass fails along contours); the mark is the host's, so the test stays early
    # unless the shader discards or writes depth
    own = src.replace(POOL_MARK, '')
    if marked and not _DISCARD.search(own) and not _POOL_WRITES.search(own):
        src = re.sub(r'^(?<!\]\] )fragment\s', '[[early_fragment_tests]] fragment ', src, flags=re.M)
    return src

_DISCARD = re.compile(r'\bdiscard_fragment\b')
_POOL_WRITES = re.compile(r'\[\[\s*depth\(|\[\[\s*sample_mask\s*\]\]|\[\[\s*stencil\s*\]\]|\.write\(|\batomic_\w+_explicit\b|^\s*device\s', re.M)
# SPIRV-Cross's spvUnsafeArray defines it whether or not the shader makes a device array of it
_ARRAY_DEVICE_ACCESSOR = re.compile(r'^ +device T& operator \[\] \(size_t pos\) device\n *\{\n *return elements\[pos\];\n *\}', re.M)

def pool_early_variant(src):
    """A pixel shader pool_arrays left with late tests although its only store is the mark → 'read_only' when it discards (early tests only while depth is
    read-only: nvn_pso.inc pso_early), 'standard' when the array accessor alone kept them late; None otherwise"""
    if POOL_MARK not in src or '[[early_fragment_tests]]' in src: return None
    own, removed = _ARRAY_DEVICE_ACCESSOR.subn('', src.replace(POOL_MARK, '').replace(BUFFER_POOL_READ, ''))
    if _POOL_WRITES.search(own) or re.search(r'\bdevice\b', re.sub(r'\bconst\s+device\b', '', own)): return None
    if _DISCARD.search(own): return 'read_only'
    return 'standard' if removed else None

def reflect(key, stage, msl, res):
    """kind (as nvn_refl.h) + NVN slot + argument buffer member index"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    binds = []
    # SPIRV-Cross puts attributes after the id ([[id(n), raster_order_group(0)]]); a skipped member would make the argument buffer longer than the reflection
    members = list(re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', body.group(1) if body else '', re.M))
    if body and len(members) != body.group(1).count('[[id('): raise ValueError(f'{key}: an argument buffer member the reflection cannot read')
    for m in members:
        ty, nm, aid = m.group(1).strip(), m.group(2), int(m.group(3))
        e = {'msl_name': nm, 'msl_type': ty, 'arg_id': aid}
        pool = re.search(r'struct spvPool_%s\s*\{\s*uint n;\s*(?:uint bias;\s*)?ulong seen;\s*array<(.+), \d+> e;' % re.escape(nm), msl) if ty == f'constant spvPool_{nm}*' else None
        if ty == 'sampler' and nm.endswith('Smplr' + abi.BORDER_TWIN):   # a sampler's twin (border_emu)
            nm = nm[:-len(abi.BORDER_TWIN)]
            e['border_bit'] = sum(1 for b in binds if 'border_bit' in b)
        if nm == abi.BORDER_MEMBER: e.update(kind=abi.KIND_BORDER)
        elif pool and not nm.endswith('_cb0_40000000'):
            ut, us = decode_ubo_tex(nm.removesuffix('Smplr'))   # pyright: ignore[reportGeneralTypeIssues]  (pool_arrays made it from a name it decodes)
            e.update(kind=abi.KIND_SAMP_HTAB if pool.group(1) == 'sampler' else abi.KIND_TEX_HTAB, elem=pool.group(1), reg=ut[0], off=ut[1], **({'soff': us[1], 'sreg': abi.SREG_C2} if us[0] == 'c2' else {}))
        elif pool: e.update(kind=abi.KIND_SAMP_POOL if pool.group(1) == 'sampler' else abi.KIND_TEX_POOL, elem=pool.group(1))
        elif nm == 'support_buffer': e.update(kind=abi.KIND_SUPPORT)
        elif c := re.fullmatch(r'[a-z]+_c(\d+)', nm):
            n = int(c.group(1))
            if 3 <= n <= 16:   # fixed: the bytes read at fixed offsets, which the host makes readable
                e.update(kind=abi.KIND_CBUF, reg=n - 3, fixed=next((b['fixed'] for b in res['cbufs'] if b['slot'] == n), 0))
            elif n == abi.CBSIZE_BANK: e.update(kind=abi.KIND_CBSIZE)
            elif n == abi.TBIAS_BANK: e.update(kind=abi.KIND_TBIAS, n=buffer_bias_entries(res))
            elif n == abi.SBLEN_BANK: e.update(kind=abi.KIND_SBLEN)
            else: e.update(kind=abi.KIND_C1 if n == 1 else abi.KIND_DRVCB, bank=n)
        elif sb := re.fullmatch(r'[a-z]+_s(\d+)', nm):
            e.update(kind=abi.KIND_SBUF, index=int(sb.group(1)))
        elif ty == 'sampler' and nm.endswith('Smplr') and (si := decode_ubo_samp_id(nm[:-5])):   # handle type 2: the replay shifts the sampler ID into a handle
            e.update(kind=abi.KIND_SAMP_UBO, reg=si[0], off=si[1], raw_id=True)
        elif u := decode_ubo_tex(nm[:-5] if nm.endswith('Smplr') else nm):   # bindless: the handle is in a uniform buffer
            ut, us = u
            other = () if us[0] in ('id', 'c2') or us == ut else (ut, us)   # handle type 1: the host ORs in the other half's word
            if nm.endswith('Smplr') and us[0] == 'c2': e.update(kind=abi.KIND_SAMP_UBO, reg=ut[0], off=ut[1], soff=us[1], sreg=abi.SREG_C2)
            elif nm.endswith('Smplr'): e.update(kind=abi.KIND_SAMP_ID, reg=us[1]) if us[0] == 'id' else e.update(kind=abi.KIND_SAMP_UBO, reg=us[0], off=us[1], **({'soff': ut[1], 'sreg': ut[0]} if other else {}))
            elif 'texture_buffer' in ty:
                e.update(kind=abi.KIND_TBUF_UBO, reg=ut[0], off=ut[1], **({'soff': us[1], 'sreg': us[0]} if other else {}))
                if not ty.startswith('array<') and (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x
            else: e.update(kind=abi.KIND_TEX_UBO, reg=ut[0], off=ut[1], **({'soff': us[1], 'sreg': us[0]} if other else {}))
        elif decode_ubo_img(nm):
            e.update(kind=abi.KIND_IMG_UBO, reg=decode_ubo_img(nm)[0], off=decode_ubo_img(nm)[1])   # pyright: ignore[reportOptionalSubscript]
            if (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x
            if has_word(msl, nm + '.atomic_'): e['atomic'] = True
        elif c2_array(nm, ty) is not None:   # element k from the slot whose c2 word is at off + 8k
            off, soff = c2_array(nm, ty)   # pyright: ignore[reportGeneralTypeIssues]
            e.update(kind=abi.KIND_SAMP_C2A if nm.endswith('Smplr') else abi.KIND_TEX_C2A, off=off, **({'soff': soff} if soff is not None else {}))
            if not nm.endswith('Smplr') and 'texture_buffer' in ty and (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x
        elif nm.endswith('Smplr'):   # handle type 0: the sampler index from the high bits of that slot's handle
            t, s, ty = decode_tex(nm[:-5]); e.update(  # pyright: ignore[reportGeneralTypeIssues]  (an undecodable sampler stops the shader here)
                kind=abi.KIND_SAMP_ID, reg=const_sampler(nm[:-5])) if ty == 3 else e.update(kind=abi.KIND_SAMP, reg=s) if ty else e.update(kind=abi.KIND_SAMP_TEX, reg=t)
        else:
            d = decode_tex(nm)
            if not d: e.update(kind=abi.KIND_UNKNOWN)
            else: e.update(kind=abi.KIND_IMG if re.match(r'[a-z]+_i', nm) else (abi.KIND_TBUF if 'texture_buffer' in ty else abi.KIND_TEX), reg=d[0])
            # Metal allows texture atomics only on an R32Uint / R32Sint texture with the shader-atomic usage, which the host checks
            if e.get('kind') == abi.KIND_IMG and has_word(msl, nm + '.atomic_'): e['atomic'] = True
            if e.get('kind') in (abi.KIND_IMG, abi.KIND_TBUF) and (x := buffer_image_bias(nm, res)) is not None: e['tbx'] = x
            # a 1D integer fetch the translator read as 2D rows of 16384 texels (Program.cs RecordTexelFetch1D)
            if e.get('kind') == abi.KIND_TEX and e.get('reg') in res.get('fetch1d', ()):
                e['fetch1d'] = True
                if has_word(msl, nm + '.get_width('): e['fetch1d_size'] = True   # also size-queried: a texture buffer of more than 16384 elements reports 16384 there
        if e.get('kind') in (abi.KIND_TEX, abi.KIND_TEX_UBO, abi.KIND_TEX_C2A):
            if (x := buffer_image_bias(nm, res)) is not None and any(t.get('row_bias') and t.get('tbx') == x for t in res.get('textures', ())):
                e['tbx'] = x
                e['fetch1d'] = True
        binds.append(e)
    base = SSBO.get(stage)   # a stage with no table here gets no slot
    sb_sorted = sorted(res['sbufs'], key=lambda b: b['binding'])
    for e in binds:
        if e['kind'] == abi.KIND_SBUF:
            b = sb_sorted[e['index']] if e['index'] < len(sb_sorted) else None
            off = b['cb_offset'] * 4 if b and b['cb_slot'] == 0 else None
            if b and b['cb_slot'] == abi.GMEM_MAP_CB_SLOT:   # not a game buffer: the host's pool table or the write log
                if b['cb_offset'] == abi.GMEM_LOG_CB_OFFSET:
                    e.update(kind=abi.KIND_GMEM_LOG, rw=True)
                    continue
                e.update(kind=abi.KIND_GMEM_MAP, rw=any(re.search(r'operation "(Store|Atomic\w*)" traced to no storage buffer', n) for n in res.get('notes', ())), pointers=gptr_sites(stage, res))
                continue
            if stage == 'cs' and off is not None and abi.C0_CS_UBO <= off < abi.C0_CS_UBO + abi.C0_ROW * abi.C0_CS_UBO_SLOTS and (off - abi.C0_CS_UBO) % abi.C0_ROW == 0:
                e.update(kind=abi.KIND_UBO_PTR, reg=(off - abi.C0_CS_UBO) // abi.C0_ROW)
            elif b and b['cb_slot'] == abi.PUSH_BANK:   # the address is one the game pushed (c[24])
                e.update(kind=abi.KIND_SBUF_PUSH, off=b['cb_offset'] * 4)
            elif b and b['cb_slot'] >= 3:   # c[n] is uniform slot n - 3
                e.update(kind=abi.KIND_SBUF_UBO, reg=b['cb_slot'] - 3, off=b['cb_offset'] * 4)
            else: e['reg'] = (off - base) // abi.C0_ROW if off is not None and base is not None and 0 <= off - base < abi.C0_ROW * abi.C0_SSBO_SLOTS and (off - base) % abi.C0_ROW == 0 else None
            e['rw'] = bool(b and b['flags'] & 1)   # BufferUsageFlags.Write (atomics too)
            if b and b['flags'] & 2:   # BufferUsageFlags.Bounded
                if e['kind'] not in (abi.KIND_SBUF_UBO, abi.KIND_SBUF_PUSH) or not 0 <= b['slot'] < abi.SBLEN_SLOTS:
                    raise ValueError(f"{key}: storage buffer {e['msl_name']} is bounded but is a {e['kind']} binding, slot {b['slot']}")
                e['sbx'] = b['slot']
    if any('sbx' in e for e in binds) and not any(e['kind'] == abi.KIND_SBLEN for e in binds):
        raise ValueError(f'{key}: bounded storage buffers without their length table')
    return binds

_GPTR_SITE = re.compile(r'translator: gptr site (\d+): c(\d+)\[(\d+)\] \+(\d+)$')

def gptr_sites(stage, res):
    """metadata for the raw pool table, not GPU site entries."""
    sites = {}
    base = SSBO.get(stage)
    for note in res.get('notes', ()):
        m = _GPTR_SITE.match(note)
        if not m: continue
        k, cb_slot, cb_word, byte = map(int, m.groups())
        off = cb_word * 4
        reg = (off - base) // abi.C0_ROW if cb_slot == 0 and base is not None and 0 <= off - base < abi.C0_ROW * abi.C0_SSBO_SLOTS and (off - base) % abi.C0_ROW == 0 else -1
        sites[k] = [reg, byte]
    if sorted(sites) != list(range(len(sites))) or len(sites) > abi.GPTR_MAX: raise ValueError(f'gptr sites {sorted(sites)} are not 0..n-1 within {abi.GPTR_MAX}')
    return [sites[k] for k in range(len(sites)) if sites[k][0] >= 0]

DRV_FILL = {0: [(b, b + abi.C0_ROW * abi.C0_SSBO_SLOTS) for k, b in sorted(SSBO.items(), key=lambda kv: kv[1]) if k not in ('ts', 'ms')],
            2: [(TEX_BASE, SEPTEX_BASE), (IMG_BASE, IMG_BASE + abi.C2_ROW * IMG_SLOTS), (SEPTEX_BASE, SAMP_BASE), (SAMP_BASE, SAMP_BASE + abi.C2_ROW * abi.C2_SEP_SLOTS)]}

INDEX_BUILTINS = {'gl_SampleID': (0, abi.C0_SAMPLE_BYTES - 1)}   # the sample index, below the sample count
INDEX_DEF = re.compile(r'^\s*(?:int|uint)\s+(_\d+)\s*=\s*([^;\n]+);', re.M)
INDEX_SET = re.compile(r'^\s*(_\d+)\s*=', re.M)

def index_range(src, expr):
    """[lo, hi] of an integer expression of literals and INDEX_BUILTINS through single-assignment temporaries, with + - * & | ^ << >> on non-negative values; else None"""
    defs, sets = {}, set(INDEX_SET.findall(src))   # a temporary assigned again (a loop's phi) has no single value
    for name, e in INDEX_DEF.findall(src): defs.setdefault(name, []).append(e)
    float_defs = dict(re.findall(r'^\s*float\s+(_\d+)\s*=\s*as_type<float>\(([\w.]+)\);', src, re.M))
    import ast as _ast
    def ev(n, depth):
        if depth > 64: return None
        if isinstance(n, _ast.Constant) and isinstance(n.value, int): return (n.value, n.value)
        if isinstance(n, _ast.Name):
            if n.id in INDEX_BUILTINS: return INDEX_BUILTINS[n.id]
            if n.id in float_defs and float_defs[n.id] in INDEX_BUILTINS and n.id not in sets: return INDEX_BUILTINS[float_defs[n.id]]
            if len(defs.get(n.id, ())) != 1 or n.id in sets: return None
            return parse(defs[n.id][0], depth + 1)
        if isinstance(n, _ast.Call) and isinstance(n.func, _ast.Name) and n.func.id == '_c' and len(n.args) == 1 and not n.keywords: return ev(n.args[0], depth + 1)
        if not isinstance(n, _ast.BinOp): return None
        a, b = ev(n.left, depth + 1), ev(n.right, depth + 1)
        if a is None or b is None: return None
        op = type(n.op)
        if op is _ast.Add: r = (a[0] + b[0], a[1] + b[1])
        elif op is _ast.Sub: r = (a[0] - b[1], a[1] - b[0])
        elif op is _ast.Mult: r = (a[0] * b[0], a[1] * b[1])
        elif op is _ast.RShift and b[0] == b[1] and b[0] < 32: r = (a[0] >> b[0], a[1] >> b[0])
        elif op is _ast.LShift and b[0] == b[1] and b[0] < 32: r = (a[0] << b[0], a[1] << b[0])
        elif op is _ast.BitAnd: r = (0, min(a[1], b[1]))
        elif op in (_ast.BitOr, _ast.BitXor): r = (0, (1 << max(a[1], b[1]).bit_length()) - 1)
        else: return None
        return r if 0 <= r[0] <= r[1] < 1 << 31 else None
    def parse(e, depth):
        e = re.sub(r'\b(?:as_type<\s*u?int\s*>|u?int)\s*\(', '_c(', e)
        e = re.sub(r'\b(\d+)u\b', r'\1', e)
        try: tree = _ast.parse(e, mode='eval')
        except SyntaxError: return None
        return ev(tree.body, depth)
    return parse(expr, 0)

def drv_reads(src):
    """constant index → byte range; a computed index of known range → its rows; else dynamic"""
    out = {}
    for bank in (0, 2):
        offs, dyn = set(), False
        for m in re.finditer(r'fp_c%d\)\._m0\[([^\]]+)\]' % bank, src):
            idx = m.group(1).rstrip('u')
            if idx.isdigit():
                offs.add(int(idx) * 16)
                continue
            r = index_range(src, m.group(1))
            if r and r[1] - r[0] < 16: offs.update(range(r[0] * 16, r[1] * 16 + 16, 16))
            else: dyn = True
        if offs or dyn: out[str(bank)] = {'vec4_offsets': sorted(offs), 'dynamic': dyn}
    return out

SAMPLE_ROWS = [(abi.C0_SAMPLE_TABLE, abi.C0_SAMPLE_TABLE + abi.C0_SAMPLE_BYTES), (abi.C0_SAMPLE_INFO, abi.C0_SAMPLE_INFO + abi.C0_ROW)]

def unfillable(dr, stage):
    bad = []
    for bank, d in dr.items():
        fill = DRV_FILL[int(bank)] + (SAMPLE_ROWS if bank == '0' and stage == 'ps' else []) + ([(SSBO[stage], SSBO[stage] + abi.C0_ROW * abi.C0_SSBO_SLOTS)] if bank == '0' and stage in ('ts', 'ms') else [])
        if bank == '0' and stage == 'cs': fill = fill + [(abi.C0_CS_UBO, abi.C0_CS_UBO + abi.C0_ROW * abi.C0_CS_UBO_SLOTS)]
        # The replay fills c2's handle tables whole, c0 only in its tables; the one computed c0 index known is GLSLC's sample position lookup (0x620, at most 15)
        if d['dynamic'] and bank == '0' and not (stage == 'ps' and abi.C0_SAMPLE_INFO in d['vec4_offsets']): bad.append(f'c{bank} dynamic index')
        for o in d['vec4_offsets']:
            # a read is a 16-byte row: overlapping any table is enough (the c2+0x160 row holds texture slot 0 at 0x168)
            if not any(o < b and a < o + 16 for a, b in fill): bad.append(f'c{bank}+{o:#x}')
    return bad
