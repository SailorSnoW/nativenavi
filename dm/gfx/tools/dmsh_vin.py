# Shadow vertex inputs (<key>_bv): an input declared integer (int_inputs) may still be fed a float-class format or an integer of the other sign, which Metal's
# fetch reads only into an input of its own class. Each integer input is also a float4 and the other sign at indices 16-30, the three ORed.
# Usage: variant(msl) → (new MSL, [{'location', 'float', 'sint' | 'uint'}]) or (None, reason)
import re

SLOT0, SLOT_END = 16, 31

def variant(msl):
    m = re.search(r'\nstruct main0_in\n\{\n(.*?)\n\};', msl, re.S)
    if not m: return None, ('no main0_in struct' if '[[stage_in]]' in msl else 'no integer input')
    members = []
    for ln in m.group(1).split('\n'):
        mm = re.fullmatch(r'    (float4|uint4|int4) (m_\d+(?:_\d+)*) \[\[attribute\((\d+)\)\]\];', ln)   # m_28_0: an input array split per element
        if not mm: return None, 'an input declaration not understood: ' + ln.strip()[:60]
        members.append((mm.group(1), mm.group(2), int(mm.group(3))))
    ints = [x for x in members if x[0] != 'float4']
    if not ints: return None, 'no integer input'
    if any(l >= SLOT0 for _, _, l in members): return None, 'an input at attribute 16 or above'
    slot, shadows, extra = SLOT0, [], []
    for t, n, l in ints:   # float4 shadows first (the common case) ...
        if slot >= SLOT_END: break
        extra.append(f'    float4 {n}_vf [[attribute({slot})]];')
        shadows.append({'location': l, 'float': slot})
        slot += 1
    for e, (t, n, l) in zip(shadows, ints):   # ... then the other signedness while indices last
        if slot >= SLOT_END: break
        o = 'int4' if t == 'uint4' else 'uint4'
        extra.append(f'    {o} {n}_vo [[attribute({slot})]];')
        e['sint' if o == 'int4' else 'uint'] = slot
        slot += 1
    covered = {e['location']: e for e in shadows}
    head = msl[:m.end()] + '\n\nstruct main0_in_v\n{\n' + m.group(1) + '\n' + '\n'.join(extra) + '\n};'
    body = msl[m.end():]
    pro = '    main0_in in = {};\n'
    for t, n, l in members:
        e = covered.get(l)
        ors = '' if not e else f' | as_type<{t}>(dm_in.{n}_vf)' + (f' | as_type<{t}>(dm_in.{n}_vo)' if ('sint' in e or 'uint' in e) else '')
        pro += f'    in.{n} = dm_in.{n}{ors};\n'
    for entry in ('main0', 'main0_z'):
        hdr = re.search(r'\nvertex main0_out %s\(main0_in in \[\[stage_in\]\]([^\n]*)\)\n\{\n' % entry, body)
        if not hdr:
            if entry == 'main0': return None, 'main0 signature not recognized'
            continue
        new = f'\nvertex main0_out {entry}(main0_in_v dm_in [[stage_in]]{hdr.group(1)})\n{{\n' + pro
        body = body[:hdr.start()] + new + body[hdr.end():]
    return head + body, shadows
