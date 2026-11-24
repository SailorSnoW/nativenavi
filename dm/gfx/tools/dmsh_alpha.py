# Alpha test entry points (<entry>_a): NVN's alpha test drops a fragment whose colour 0 alpha fails against a reference, before blending; Metal has none.
# Function and reference are data (DMSH_ALPHA_BUFFER), so one pipeline serves every draw. [[early_fragment_tests]] stays: Maxwell writes depth first too.
import re
import dmsh_abi

HELPER = '''
struct dm_alpha_state { uint func; float ref; };   // NVN_ALPHA_FUNC_* and the reference (dmsh_alpha.py)
static inline bool dm_alpha_pass(float a, constant dm_alpha_state& s)
{
    switch (s.func)
    {
        case 1u: return false;
        case 2u: return a < s.ref;
        case 3u: return a == s.ref;
        case 4u: return a <= s.ref;
        case 5u: return a > s.ref;
        case 6u: return a != s.ref;
        case 7u: return a >= s.ref;
        default: return true;
    }
}
'''


def _colour0(msl, struct):
    st = re.search(r'struct %s\s*\{(.*?)\};' % struct, msl, re.S)
    return [(ty, nm, fc or None) for ty, nm, fc in re.findall(r'^\s*(float4|half4|uint4|int4)\s+(\w+)\s*\[\[color\(0\)(?:, index\(0\))?(?:, function_constant\((\w+)\))?\]\];', st.group(1), re.M)] if st else []


def refused(msl):
    st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    if not st: return 'no colour output'
    if [ty for ty, _, _ in _colour0(msl, 'main0_out')] in ([], ['uint4'], ['int4']):   # no float alpha to test
        return 'colour 0 has no float alpha' if re.search(r'\[\[color\(0\)\]\]', st.group(1)) else 'no colour 0 output'
    if f'[[buffer({dmsh_abi.ALPHA_BUFFER})]]' in msl: return f'fragment buffer {dmsh_abi.ALPHA_BUFFER} is taken'
    return None


def test(msl, var):
    """main0_out held in var; None when the shader gets none"""
    if refused(msl): return None
    cols = [(ty, nm) for ty, nm, fc in _colour0(msl, 'main0_out') if not fc]
    if len(cols) != 1 or cols[0][0] not in ('float4', 'half4'): return None
    return f'if (!dm_alpha_pass(float({var}.{cols[0][1]}.w), dm_alpha)) discard_fragment();'


def entry(msl, e):
    cols = _colour0(msl, e['ret'])
    if not cols: return None
    # main0_u's integer colour 0 members hold the float's bits (the host leaves an integer colour 0 untested)
    read = lambda ty, nm: f'as_type<float>(dm_r.{nm}.w)' if ty in ('uint4', 'int4') else f'float(dm_r.{nm}.w)'
    alpha = ' '.join(f'if ({fc}) dm_a = {read(ty, nm)};' if fc else f'dm_a = {read(ty, nm)};' for ty, nm, fc in cols)
    params = ', '.join(e['params'] + [f'constant dm_alpha_state& dm_alpha [[buffer({dmsh_abi.ALPHA_BUFFER})]]'])
    return (f'{e["attr"]}fragment {e["ret"]} {e["name"]}_a({params})\n{{\n    {e["ret"]} dm_r = dm_body_{e["name"]}({e["args"]});\n'
            f'    float dm_a = 1.0f; {alpha}\n    if (!dm_alpha_pass(dm_a, dm_alpha)) discard_fragment();\n    return dm_r;\n}}\n')
