# Logic operation entry points (<entry>_l): NVN's logic op combines the written bits with the target's (integer and normalized targets); Metal has none.
# A float output goes through its target's stored bits; a zero mask (float or sRGB target) passes it through. Each attachment read depends on DMSH_LOGIC_FC + n,
# as Metal refuses a pipeline reading an attachment the pass lacks. main0_d gets none: a logic operation replaces blending.
import re
import dmsh_abi

HELPER = '''
struct dm_logic_state { uint op; uint pad0, pad1, pad2; uint4 mask[8]; };   // NVN_LOGIC_OP_* and each colour target's channel masks (dmsh_logic.py)
static inline uint4 dm_logic(uint4 s, uint4 d, constant dm_logic_state& l, uint n)
{
    uint4 r;
    switch (l.op)
    {
        case 0u: r = uint4(0u); break;
        case 1u: r = s & d; break;
        case 2u: r = s & ~d; break;
        case 4u: r = ~s & d; break;
        case 5u: r = d; break;
        case 6u: r = s ^ d; break;
        case 7u: r = s | d; break;
        case 8u: r = ~(s | d); break;
        case 9u: r = ~(s ^ d); break;
        case 10u: r = ~d; break;
        case 11u: r = s | ~d; break;
        case 12u: r = ~s; break;
        case 13u: r = ~s | d; break;
        case 14u: r = ~(s & d); break;
        case 15u: r = uint4(~0u); break;
        default: r = s; break;
    }
    return r & l.mask[n];
}
static inline float4 dm_logic_norm(float4 s, float4 d, constant dm_logic_state& l, uint n)
{
    if (all(l.mask[n] == uint4(0u))) return s;   // not a normalized target: the operation does not apply
    float4 scale = float4(l.mask[n]);
    return float4(dm_logic(uint4(floor(saturate(s) * scale + 0.5f)), uint4(floor(saturate(d) * scale + 0.5f)), l, n)) / scale;
}
'''


def refused(msl):
    if f'[[buffer({dmsh_abi.LOGIC_BUFFER})]]' in msl: return f'fragment buffer {dmsh_abi.LOGIC_BUFFER} is taken'
    if re.search(r'\[\[color\(\d+\)\]\]\s*[,)]', msl): return 'an entry point already reads an attachment'
    return None


def constants(targets):
    return ''.join(f'constant bool dm_lfb{n} [[function_constant({dmsh_abi.LOGIC_FC + n})]];\n' for n in sorted(targets))


def parts(members, var, gate=None):
    """→ (the constants each read depends on, the fetch parameters, the statements, the colour outputs read), or None without a colour output.
    gate: a bool constant every read depends on too"""
    outs = [(ty, nm, int(n), fc) for ty, nm, n, fc in re.findall(r'^\s*((?:uint|float)[234]?)\s+(\w+)\s*\[\[color\((\d+)\)(?:, function_constant\((\w+)\))?\]\];', members, re.M)]
    if not outs: return None
    # main0_u's float and unsigned members read their attachment as their own type; a signed one has no bits to combine and passes through
    scalar = lambda ty: 'uint' if ty.startswith('uint') else 'float'
    on = lambda n, fc: f'dm_lfb{n}_{fc}' if fc else f'dm_lfb{n}_on' if gate else f'dm_lfb{n}'
    consts = ''.join(f'constant bool {on(n, fc)} = dm_lfb{n}{f" && {fc}" if fc else ""}{f" && {gate}" if gate else ""};\n' for _, _, n, fc in outs if fc or gate)
    fetch = [f'{scalar(ty)}4 {on(n, fc).replace("dm_lfb", "dm_fb")} [[color({n}), function_constant({on(n, fc)})]]' for ty, _, n, fc in outs]
    apply = []
    for ty, nm, n, fc in outs:
        s = scalar(ty)
        width = int(ty[len(s):] or 1)
        zero = '0u' if s == 'uint' else '0.0f'
        pad = f', {zero}' * (4 - width)
        sw = '.' + 'xyzw'[:width] if width < 4 else ''
        apply.append(f'if ({on(n, fc)}) {var}.{nm} = {ty}({"dm_logic" if s == "uint" else "dm_logic_norm"}({s}4({var}.{nm}{pad}), {on(n, fc).replace("dm_lfb", "dm_fb")}, dm_lop, {n}u){sw});')
    return consts, fetch, apply, {n for _, _, n, _ in outs}


def entry(msl, e):   # main0's, in a shader without main0_u (which holds its own)
    if e['name'] != 'main0': return None
    st = re.search(r'struct %s\s*\{(.*?)\};' % e['ret'], msl, re.S)
    p = parts(st.group(1), 'dm_r') if st else None
    if not p: return None
    consts, fetch, apply, targets = p
    params = ', '.join(e['params'] + fetch + [f'constant dm_logic_state& dm_lop [[buffer({dmsh_abi.LOGIC_BUFFER})]]'])
    return (f'{consts}{e["attr"]}fragment {e["ret"]} {e["name"]}_l({params})\n{{\n    {e["ret"]} dm_r = dm_body_{e["name"]}({e["args"]});\n    {" ".join(apply)}\n    return dm_r;\n}}\n',
            targets)
