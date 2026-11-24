# Pulling vertex inputs (<key>_bp): the vertex shader without [[stage_in]], decoding its inputs through a per-draw layout block (dmsh_pull.metal), bit-identical to
# Metal's vertex fetch, so one pipeline serves every layout while the layout's own builds. main0 / main0_z; main0_discard keeps the vertex descriptor.
# Usage: variant(msl) → (new MSL, {'entry', 'entry_z', 'slot', 'inputs'[, 'entry_wl', 'entry_z_wl']}) or (None, reason)
import os, re

SLOT = 16   # the vertex buffers' first slot, which a pipeline without a vertex descriptor does not use
WL_LINE = 15   # beside the vertex stage's 1-13 and 16-30
# Wide lines (<entry>_wl): Metal draws 1-pixel lines only, so a segment is a quad of 6 vertices (nvn_wline.inc). The body runs for both endpoints; the position moves half
# the width along the minor axis in window space (OpenGL's non-antialiased wide line; with multisampling, rect, a rectangle), scaled by w, after the segment is clipped to
# w >= 1e-6 of its larger |w| and z >= 0 (dm_wl_cut). The body runs 12 times a segment, and so would its stores: no line-drawing shader has any
WL_MSL = '''struct dm_wline { device const uchar* ib; float2 vp; float width; uint prim; uint count; int base; uint isz; uint restart; uint ri; uint zclip; uint rect; };   // nvn_wline.inc wl_block
static inline uint dm_wl_vid(constant dm_wline& L, uint i, thread bool& cut)
{
    if (L.isz == 0u) return uint(L.base) + i;
    uint v = L.isz == 1u ? uint(L.ib[i]) : L.isz == 2u ? uint(((device const ushort*)L.ib)[i]) : ((device const uint*)L.ib)[i];
    cut = cut || (L.restart != 0u && v == L.ri);
    return v + uint(L.base);
}
static inline uint3 dm_wl_ends(constant dm_wline& L, uint c, thread bool& cut)   // corner c: (its endpoint's vertex ID, the other endpoint's, its side | 2 at the second endpoint)
{
    uint s = c / 6u, k = c - 6u * s;
    uint i0 = L.prim == 1u ? 2u * s : s, i1 = L.prim == 2u && s + 1u == L.count ? 0u : i0 + 1u;
    uint a = dm_wl_vid(L, i0, cut), b = dm_wl_vid(L, i1, cut);
    bool at_b = k == 1u || k == 2u || k == 4u;
    return uint3(at_b ? b : a, at_b ? a : b, (k == 2u || k == 4u || k == 5u ? 1u : 0u) | (at_b ? 2u : 0u));
}
static inline float2 dm_wl_cut(float4 p, float4 q, constant dm_wline& L)   // how far each endpoint moves towards the other to enter w >= e (and z >= 0); 2: none of it is in
{
    float e = 1e-6f * max(abs(p.w), abs(q.w));
    float2 t = float2(0.0f);
    for (uint k = 0u; k < (L.zclip != 0u ? 2u : 1u); k++)
    {
        float dp = k != 0u ? p.z : p.w - e, dq = k != 0u ? q.z : q.w - e;
        if (dp < 0.0f && dq < 0.0f) return float2(2.0f);
        if (dp < 0.0f) t.x = max(t.x, dp / (dp - dq));
        if (dq < 0.0f) t.y = max(t.y, dq / (dq - dp));
    }
    return t.x + t.y >= 1.0f ? float2(2.0f) : t;
}
static inline float4 dm_wl_offset(float4 p, float4 q, constant dm_wline& L, uint side)
{
    float2 d = (q.xy / q.w - p.xy / p.w) * L.vp * ((side & 2u) != 0u ? -1.0f : 1.0f);   // from the first endpoint to the second at both ends
    float s = (side & 1u) != 0u ? 0.5f : -0.5f;
    float2 o = abs(d.x) >= abs(d.y) ? float2(0.0f, s * L.width) : float2(s * L.width, 0.0f);
    if (L.rect != 0u) o = all(d == 0.0f) ? float2(0.0f) : normalize(float2(-d.y, d.x)) * (s * L.width);   // multisampled: across the segment (a point draws nothing)
    p.xy += o / L.vp * p.w;
    return p;
}
'''
WL_ATTRS = ('buffer', 'texture', 'sampler', 'vertex_id', 'instance_id', 'base_instance', 'base_vertex')

def _params(rest):
    """a template argument list has commas of its own"""
    out, depth, cur = [], 0, ''
    for ch in rest:
        if ch in '<(': depth += 1
        elif ch in '>)': depth -= 1
        if ch == ',' and depth == 0:
            out.append(cur.strip())
            cur = ''
        else:
            cur += ch
    if cur.strip(): out.append(cur.strip())
    return out

def _wl_mix(msl):
    """floating-point members interpolated, arrays element by element; flat and integer ones keep the first's. None when it cannot"""
    m = re.search(r'\nstruct main0_out\n\{\n(.*?)\n\};', msl, re.S)
    if not m: return None
    body = ''
    for ln in m.group(1).split('\n'):
        mm = re.fullmatch(r'    (\w+) (\w+)(?: \[\[([^\]]*)\]\])?(?: \[(\d+)\])?;', ln)
        if not mm: return None
        t, n, attrs, count = mm.groups()
        if not t.startswith(('float', 'half')) or 'flat' in (attrs or ''): continue
        body += (f'    for (int i = 0; i < {count}; i++) r.{n}[i] = mix(a.{n}[i], b.{n}[i], t);\n' if count else f'    r.{n} = mix(a.{n}, b.{n}, t);\n')
    return '\nstatic inline main0_out dm_wl_mix(main0_out a, main0_out b, float t)\n{\n    main0_out r = a;\n' + body + '    return r;\n}\n'

def _wide_line(entry, params, fbody, pos, inn, bn):
    """None when a parameter is not one it can pass on"""
    decl, args = [], []
    for p in params:
        m = re.fullmatch(r'(.*?)(\w+) \[\[(\w+)(?:\(\d+\))?\]\]', p)
        if not m or m.group(3) not in WL_ATTRS: return None
        decl.append(m.group(1) + m.group(2))
        args.append({'vertex_id': '{v}', 'base_vertex': 'uint(dm_wl.base)'}.get(m.group(3), m.group(2)))
    wparams = [p for p in params if not p.endswith(('[[vertex_id]]', '[[base_vertex]]'))] + ['uint dm_wcorner [[vertex_id]]', f'constant dm_wline& dm_wl [[buffer({WL_LINE})]]']
    call = lambda v: f'dm_wlb_{entry}(dm_wl_in(dm_lay, {v}, {inn}, {bn}), ' + ', '.join(a.replace('{v}', v) for a in args) + ')'
    return (f'\nstatic inline main0_out dm_wlb_{entry}(main0_in in, ' + ', '.join(decl) + ')\n{\n' + fbody +
            f'\nvertex main0_out {entry}_wl(' + ', '.join(wparams) + ')\n{\n'
            '    bool dm_cut = false; uint3 dm_e = dm_wl_ends(dm_wl, dm_wcorner, dm_cut);\n'
            '    if (dm_cut) return main0_out{};   // a segment with a restart index: nothing drawn\n'
            f'    main0_out out = {call("dm_e.x")}, dm_o = {call("dm_e.y")};\n'
            f'    float4 dm_p = out.{pos}, dm_q = dm_o.{pos};\n'
            '    float2 dm_t = dm_wl_cut(dm_p, dm_q, dm_wl);\n'
            '    if (dm_t.x > 1.0f) return main0_out{};   // the segment is wholly behind the eye (or the near plane)\n'
            '    if (dm_t.x > 0.0f) out = dm_wl_mix(out, dm_o, dm_t.x);\n'
            f'    out.{pos} = dm_wl_offset(out.{pos}, mix(dm_q, dm_p, dm_t.y), dm_wl, dm_e.z);\n'
            '    return out;\n}\n')

FETCH = open(os.path.join(os.path.dirname(os.path.abspath(__file__)), 'dmsh_pull.metal')).read()

def _drop_entries(msl, keep):
    """a function runs from a line 'vertex ...' to the next line '}'"""
    out, lines, i = [], msl.split('\n'), 0
    while i < len(lines):
        m = re.match(r'vertex \S+ (\w+)\(', lines[i])
        if m and m.group(1) not in keep:
            j = i
            while j < len(lines) and lines[j] != '}': j += 1
            i = j + 1
            continue
        out.append(lines[i])
        i += 1
    return '\n'.join(out)

def variant(msl) -> 'tuple[str, dict] | tuple[None, str]':
    m = re.search(r'\nstruct main0_in\n\{\n(.*?)\n\};', msl, re.S)
    if not m: return None, 'no stage input'   # reads no vertex attribute: no vertex descriptor to vary
    if f'[[buffer({SLOT})]]' in msl: return None, f'buffer {SLOT} is taken'
    members = []
    for ln in m.group(1).split('\n'):
        mm = re.fullmatch(r'    (float4|uint4|int4) (m_\d+(?:_\d+)*) \[\[attribute\((\d+)\)\]\];', ln)   # m_28_0: an input array split per element
        if not mm: return None, 'an input declaration not understood: ' + ln.strip()[:60]
        members.append((mm.group(1), mm.group(2), int(mm.group(3))))
    if any(l >= 32 for _, _, l in members): return None, 'an input at attribute 32 or above'   # NVN2 has 32 attributes
    head = msl[:m.start()] + '\nstruct main0_in\n{\n' + '\n'.join(f'    {t} {n};' for t, n, _ in members) + '\n};'
    body = _drop_entries(msl[m.end():], ('main0', 'main0_z'))
    entries = {}
    wl = []
    wle = set()
    posm = re.search(r'struct main0_out\n\{[^}]*?float4 (\w+) \[\[position', msl)
    pos = posm.group(1) if posm else None
    for entry in ('main0', 'main0_z'):
        hdr = re.search(r'\nvertex main0_out %s\(main0_in in \[\[stage_in\]\]((?:, [^\n]*)?)\)\n\{\n' % entry, body)
        if not hdr:
            if entry == 'main0': return None, 'main0 signature not recognized'
            continue
        rest = hdr.group(1)
        vid = re.search(r'uint (\w+) \[\[vertex_id\]\]', rest)
        iid = re.search(r'uint (\w+) \[\[instance_id\]\]', rest)
        bi = re.search(r'uint (\w+) \[\[base_instance\]\]', rest)
        params = _params(rest[2:]) if rest else []
        params.append(f'constant dm_vlayout& dm_lay [[buffer({SLOT})]]')
        if not vid: params.append('uint dm_vid [[vertex_id]]')
        if not iid: params.append('uint dm_iid [[instance_id]]')
        if not bi: params.append('uint dm_bi [[base_instance]]')
        vn, inn, bn = vid.group(1) if vid else 'dm_vid', iid.group(1) if iid else 'dm_iid', bi.group(1) if bi else 'dm_bi'
        pro = '    main0_in in;\n' + ''.join(f'    in.{n} = as_type<{t}>(dm_fetch(dm_lay, {l}u, {vn}, {inn}, {bn}));\n' for t, n, l in members)
        fn_end = body.find('\n}\n', hdr.end())
        if fn_end < 0: return None, 'end of ' + entry + ' not found'
        fbody = body[hdr.end():fn_end + 3]
        body = body[:hdr.start()] + f'\nvertex main0_out {entry}(' + ', '.join(params) + ')\n{\n' + pro + body[hdr.end():]
        entries[entry] = 1
        w = _wide_line(entry, params, fbody, pos, inn, bn) if pos else None
        if w:
            wl.append(w)
            wle.add(entry)
    ui = head.find('using namespace metal;')
    if ui < 0: return None, 'no "using namespace metal;"'
    ui += len('using namespace metal;')
    mix = _wl_mix(msl) if wl else None
    if not mix: wl, wle = [], set()   # an output it cannot interpolate: no wide-line entries
    head = head[:ui] + '\n\n' + FETCH + (WL_MSL if wl else '') + head[ui:]
    if wl and mix:
        wl.insert(0, '\nstatic inline main0_in dm_wl_in(constant dm_vlayout& dm_lay, uint dm_v, uint dm_i, uint dm_b)\n{\n    main0_in in;\n'
                  + ''.join(f'    in.{n} = as_type<{t}>(dm_fetch(dm_lay, {l}u, dm_v, dm_i, dm_b));\n' for t, n, l in members) + '    return in;\n}\n' + mix)
    return head + body + ''.join(wl), {'entry': 'main0', 'entry_z': 'main0_z' if 'main0_z' in entries else None, 'slot': SLOT, 'inputs': [l for _, _, l in members],
                                       **({'entry_wl': 'main0_wl', 'entry_z_wl': 'main0_z_wl' if 'main0_z' in wle else None} if 'main0' in wle else {})}
