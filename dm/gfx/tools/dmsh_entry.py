#!/usr/bin/env python3
# Entry points dmsh_build.py adds to, or rewrites in, a translated shader's MSL (text functions only, so tests import them).
import re, sys
import dmsh_abi, dmsh_side, dmsh_alpha, dmsh_logic
from dmsh_reflect import attributes

def vs_writes(src, binds):
    """Storage buffer writes or image stores, which a draw with rasterizer discard still runs"""
    return any(b.get('rw') for b in binds) or bool(re.search(r'access::(read_)?write\b', src))

def vs_discard(src):
    """the void vertex entry Metal requires with rasterization disabled: side effects and early returns kept, outputs dropped"""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\((.*?)\)\s*\{', src, re.S)
    if not m: raise ValueError('vertex shader has no main0 entry for rasterizer discard')
    return '\nvertex void main0_discard(' + m.group(1) + ')\n{' + src[m.end():].replace('return out;', 'return;')

_CAST = ('as_type<int>(', 'as_type<uint>(')

def int_inputs(src):
    """Inputs only used as integers → {location: 'uint'}. Maxwell puts an integer format's bits in the attribute register unconverted; declared float, Metal would convert them"""
    ins = {m.group(2): int(m.group(3)) for m in attributes(src)}
    i = src.find('vertex main0_out main0(')
    if i < 0: return {}
    body = src[i:]
    out = {}
    for name, loc_ in ins.items():
        uses = list(re.finditer(r'in\.%s\b' % re.escape(name), body))
        nint = 0
        bad = False
        for u in uses:
            ls = body.rfind('\n', 0, u.start()) + 1
            le = body.find('\n', u.end())
            m = re.match(r'\s*float\s+(_\w+)\s*=\s*in\.%s\.[xyzw];$' % re.escape(name), body[ls:le])
            if not m:
                if body[:u.start()].endswith(_CAST):
                    nint += 1
                    continue
                bad = True
                break
            v = m.group(1)
            for o in list(re.finditer(r'\b%s\b' % re.escape(v), body))[1:]:
                if body[:o.start()].endswith(_CAST):
                    nint += 1
                    continue
                l2s = body.rfind('\n', 0, o.start()) + 1
                l2e = body.find('\n', o.end())
                if re.match(r'\s*[\w.\[\]]+\s*=\s*%s;$' % re.escape(v), body[l2s:l2e]): continue   # the bit pattern is the same under either declaration
                bad = True
                break
            if bad: break
        if not bad and nint: out[str(loc_)] = 'uint'
    return out

def vs_init(src):
    """As an unwritten component reads on the hardware (the translator's out = {} is all zeros): an output written only in xyz would divide by w = 0"""
    i = src.find('struct main0_out\n{\n')
    j = src.find('\n};', i)
    k = src.find('main0_out out = {};')
    if i < 0 or j < 0 or k < 0: return src
    outs = re.findall(r'\n    float4 (\w+) \[\[user\(locn\d+\)\]\];', src[i:j])
    k += len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.{n} = float4(0.0, 0.0, 0.0, 1.0);' for n in outs) + src[k:]

VP_BASE_INSTANCE = '(*spvDescriptorSet0.vp_c0)._m0[100].y'   # c0 + 0x644, the driver's base-instance word

def vs_base_instance(src):
    """GLSLC adds c0 + 0x644 to the instance ID; an indirect draw's base instance is in its argument record, which the GPU may write in the same submission, so it
    comes from [[base_instance]] (as the float with its bits)"""
    if VP_BASE_INSTANCE not in src: return src
    src = src.replace(VP_BASE_INSTANCE, 'as_type<float>(gl_BaseInstance)')
    def add(m):
        return m.group(0) if '[[base_instance]]' in m.group(0) else m.group(0)[:-1] + ', uint gl_BaseInstance [[base_instance]])'
    return re.sub(r'\bvertex\s+\w+\s+main0\s*\((?:[^()]|\([^()]*\))*\)', add, src)   # attributes hold parentheses ([[buffer(0)]])

def writes_layer(src):
    i = src.find('struct main0_out\n{\n')
    j = src.find('\n};', i)
    return i >= 0 and j >= 0 and '[[render_target_array_index]]' in src[i:j]

def vs_depth(src):
    """main0 converts to the GL convention (near = -w: z' = z/2 + w/2), main0_z keeps NVN's near = 0; the host picks one by nvnDeviceSetDepthMode.
    → (the source with only main0, for reflection and raster facts; the full source)"""
    m = re.search(r'\bvertex\s+main0_out\s+main0\s*\(', src)
    if not m: sys.exit('[RED] vertex shader without a main0 main function, cannot add the depth entry points')
    body = src[m.start():]   # the main function is the last in the file
    gl = body.replace('return out;', 'out.gl_Position.z = fma(out.gl_Position.z, 0.5, out.gl_Position.w * 0.5);\n    return out;')
    single = src[:m.start()] + gl
    return single, single + '\n' + re.sub(r'\bmain0\s*\(', 'main0_z(', body, count=1)

def fs_uint(src):
    """main0_u: main0's body returning each colour output n as a float, unsigned or signed vector of the same bits, by function constants DMSH_UINT_FC + n /
    DMSH_SINT_FC + n (Metal refuses a float output on an integer attachment); also NVN's alpha test and logic operation, each under its constant.
    → main0_u, or None when main0's shape is unknown or it has no colour output"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    try: e = dmsh_side.entry(src, 'main0')
    except ValueError: e = None
    if not e or not st or e['ret'] != 'main0_out': return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    col = {}   # member → (scalar type, width suffix, colour)
    for ty, nm, at in mem:
        c, t = re.fullmatch(r'\[\[color\(([0-7])\)\]\]', at or ''), re.fullmatch(r'(float|uint|int)([234]?)', ty)
        if c and t: col[nm] = (t.group(1), t.group(2), int(c.group(1)))
    if not col: return None
    ns = sorted({n for _, _, n in col.values()})
    consts = ''.join(f'constant bool dm_ui{n} [[function_constant({dmsh_abi.UINT_FC + n})]];\nconstant bool dm_si{n} [[function_constant({dmsh_abi.SINT_FC + n})]];\n'
                     f'constant bool dm_uf{n} = !dm_ui{n} && !dm_si{n};\n' for n in ns)
    kinds = (('float', '', 'dm_uf'), ('uint', '_ui', 'dm_ui'), ('int', '_si', 'dm_si'))
    def member(ty, nm, at):
        if nm not in col: return f'    {ty} {nm} {at};\n' if at else f'    {ty} {nm};\n'
        _, w, n = col[nm]
        return ''.join(f'    {k}{w} {nm}{sfx} [[color({n}), function_constant({fc}{n})]];\n' for k, sfx, fc in kinds)
    def store(nm):
        if nm not in col: return f'    r.{nm} = out.{nm};\n'
        k0, w, n = col[nm]
        put = lambda k, sfx: f'r.{nm}{sfx} = ' + (f'out.{nm};' if k == k0 else f'as_type<{k}{w}>(out.{nm});')
        return f'    if (dm_ui{n}) {put("uint", "_ui")} else if (dm_si{n}) {put("int", "_si")} else {put("float", "")}\n'
    members = ''.join(member(ty, nm, at) for ty, nm, at in mem)
    alpha = dmsh_alpha.test(src, 'out')
    logic = None if dmsh_logic.refused(src) else dmsh_logic.parts(members, 'r', 'dm_lo')
    helpers, params, pre, post = '', list(e['params']), '', ''
    if alpha:
        helpers += dmsh_alpha.HELPER
        consts += f'constant bool dm_at [[function_constant({dmsh_abi.TYPED_ALPHA_FC})]];\n'
        params.append(f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER}), function_constant(dm_at)]]')
        pre = f'    if (dm_at) {{ {alpha} }}\n'
    if logic:
        lc, fetch, apply, targets = logic
        helpers += dmsh_logic.HELPER
        consts += f'constant bool dm_lo [[function_constant({dmsh_abi.TYPED_LOGIC_FC})]];\n' + dmsh_logic.constants(targets) + lc
        params += fetch + [f'constant dm_logic_state& dm_lop [[buffer({dmsh_abi.LOGIC_BUFFER}), function_constant(dm_lo)]]']
        post = f'    {" ".join(apply)}\n'
    struct = 'struct main0_out_u\n{\n' + members + '};\n\n'
    return ('\n' + helpers + consts + '\n' + struct + f'{e["attr"]}fragment main0_out_u main0_u({", ".join(params)})\n{{\n    main0_out out = dm_body_main0({e["args"]});\n'
            + pre + '    main0_out_u r = {};\n' + ''.join(store(nm) for _, nm, _ in mem) + post + '    return r;\n}\n')


def typed_ops(msl):
    return {'alpha': f'dm_at [[function_constant({dmsh_abi.TYPED_ALPHA_FC})]]' in msl, 'logic': f'dm_lo [[function_constant({dmsh_abi.TYPED_LOGIC_FC})]]' in msl}


def fs_dual(src):
    """main0_d: output 1 as [[color(0), index(1)]], the only second source Metal reads (its compiler crashes on a pipeline blending with an absent one), for
    dual_want.tsv shaders that write exactly colours 0 and 1. → main0_d, or None"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', src, re.S)
    if not st: return None
    try: e = dmsh_side.entry(src, 'main0')
    except ValueError: e = None
    if not e: return None
    mem = re.findall(r'^\s*(\w+)\s+(\w+)\s*(\[\[[^\]]*\]\])?;', st.group(1), re.M)
    if sorted(at for _, _, at in mem if 'color(' in at) != ['[[color(0)]]', '[[color(1)]]']: return None
    struct = 'struct main0_out_d\n{\n' + ''.join(f'    {ty} {nm} {"[[color(0), index(1)]]" if at == "[[color(1)]]" else at};\n' for ty, nm, at in mem) + '};\n\n'
    return '\n' + struct + dmsh_side.calling(e, 'main0_d', 'main0_out_d', 'return main0_out_d{' + ', '.join(f'out.{nm}' for _, nm, _ in mem) + '};')
