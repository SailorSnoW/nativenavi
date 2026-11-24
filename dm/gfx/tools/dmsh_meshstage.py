"""NVN2 task and mesh shaders (control +0x56: 6 task, 7 mesh) as Metal object and mesh functions wrapping the translator's compute kernel.

A mesh shader with no task also gets an object function main0_o: Metal caps a mesh grid at METAL_MAX_GRID threadgroups, so each one-thread object threadgroup
launches a block of up to that many. Task and object functions read the draw's {count, first} at MESH_DRAW_SLOT.

Stage memory (memory(), in shared memory), zero when the body starts:
  word 0      the workgroup's number (ISBERD mode 0x78)
  INPUT       the task's payload (ISBERD 0x38); its word 0 the mesh workgroup's number among its task's launches, as the hardware puts it
  outputs     a task's payload (0xb8: word 1 the mesh workgroups to launch, records from +0x10), or a mesh shader's attributes (0xf8): vertex blocks of 32, then
              primitive blocks of 32 (system values first; each keeps its room, or the primitive id would lie over the count byte)
  primitives  the count at +3 (0x88), index bytes from +4 (0x80, control +0x5f per primitive)
"""
import re
import struct
import dmsh_abi as abi

WORKGROUP, INPUT = 0, 0x10
BLOCK = 32
TOPOLOGY = {1: 'point', 2: 'line', 3: 'triangle'}
METAL_MAX_VERTICES, METAL_MAX_PRIMITIVES = 256, 512
METAL_MAX_GRID = abi.MESH_GRID   # mesh threadgroups an object threadgroup may launch
METAL_MAX_SHARED = 0x8000
SPILL_MIN = 6000   # stage memory bytes from which a mesh shader's array is a device slice (faster from 6.5 KB, no gain at 5.3 KB)


def is_stage(ctl):
    return {6: 'ts', 7: 'ms'}.get(ctl[0x56]) if len(ctl) > 0x56 else None


def sph_outputs(sph):
    """[('psiz', 0)] point size (byte 0x35 bit 3), [(None, c)] position (0x35 bits 4..7), then [(n, c)] generic attribute n (0x36 on, a nibble each)."""
    out = ([('psiz', 0)] if sph[0x35] >> 3 & 1 else []) + [(None, c) for c in range(4) if sph[0x35] >> (4 + c) & 1]
    for n in range(32):
        out += [(n, c) for c in range(4) if sph[0x36 + n // 2] >> (4 * (n % 2) + c) & 1]
    return out


def sph_system(sph):
    """byte 0x34, byte 0x35 bits 0..3, lowest bit first."""
    bits = sph[0x34] | (sph[0x35] & 0xf) << 8
    return [b for b in range(12) if bits >> b & 1]


def info(ctl, area):
    """Control +0x58 threads, +0x5a payload bytes, +0x5e has a task, +0x5f vertices a primitive, +0x60 / +0x62 most vertices / primitives, +0x64 the
    per-primitive SPH's offset where +0x68 bit 7 is set."""
    kind = is_stage(ctl)
    threads, payload = struct.unpack_from('<2H', ctl, 0x58)
    out = {'kind': kind, 'threads': threads, 'payload': payload}
    if kind == 'ms':
        max_v, max_p = struct.unpack_from('<2H', ctl, 0x60)
        at = struct.unpack_from('<I', ctl, 0x64)[0] if ctl[0x68] & 0x80 else 0
        prim = sph_outputs(area[at:at + 0x80]) if at else []
        out.update(task=bool(ctl[0x5e]), verts=ctl[0x5f] or 3, max_v=max_v, max_p=max_p, vertex=sph_outputs(area[:0x80]), primitive=[c for c in prim if isinstance(c[0], int)],
                   primitive_system=len(sph_system(area[at:at + 0x80])) if at else 0, primitive_builtins=sph_system(area[at:at + 0x80]) if at else [])
    return out


def memory(i):
    """→ (offsets of the workgroup's number, input, outputs, primitive bytes; shared memory bytes)."""
    out = INPUT + (i['payload'] + 15 & ~15)
    if i['kind'] == 'ts':
        return [WORKGROUP, INPUT, out, out + i['payload']], out + (i['payload'] + 3 & ~3)
    v_blocks, p_blocks = blocks_of(i['max_v']), blocks_of(i['max_p'])
    prim = out + max(v_blocks * len(i['vertex']) + p_blocks * per_primitive(i), 1) * BLOCK * 4
    # Every lane of every primitive block writes index bytes (a count of 0 wraps count - 1), so they are reserved for whole blocks of 32.
    # [4..6]: the vertex records the non-skew attribute addresses (ISBEWR / ISBERD .O.ATTR.32, mode 0xb8) name, after the SoA area (AosToSoa)
    n = max(len(i['vertex']), 1)
    return [WORKGROUP, INPUT, out, prim, prim - out, n, i['max_v'] * 4 * n], prim + (4 + i.get('verts', 3) * p_blocks * BLOCK + 3 & ~3)


# Per-primitive system values handed on, by OMAP bit (attribute 0x40 + 4 * bit): the primitive id (0x60). Layer and viewport keep their room, not handed on
PRIMITIVE_BUILTINS = {8: ('dm_pid', 'primitive_id')}


def primitive_builtins(i):
    """[(member name, Metal attribute, component in a primitive block)]"""
    return [PRIMITIVE_BUILTINS[b] + (k,) for k, b in enumerate(i.get('primitive_builtins', [])) if b in PRIMITIVE_BUILTINS]


def blocks_of(n):
    """at least one"""
    return (max(n, 1) + BLOCK - 1) // BLOCK


def per_primitive(i):
    """system values, then attributes"""
    return i.get('primitive_system', 0) + len(i['primitive'])


def own_shared(code):
    """LDS, STS, ATOMS: it would lie over the stage memory, and the control section does not give its size."""
    return any(struct.unpack_from('<H', code, o)[0] & 0xfff in (0x984, 0x388, 0x988, 0x38c, 0x98c, 0xf8c) for o in range(0, len(code) - 15, 16))


# SPIRV-Cross can declare a private variable's initializer ahead of the stage array: plain declarations before it are taken along (group 2)
_KERNEL = re.compile(r'kernel\s+void\s+main0\s*\(((?:[^()]|\([^()]*\))*)\)\s*\{[ \t]*\n((?:(?![ \t]*threadgroup\b)[ \t]+[^\n{}]*;[ \t]*\n)*?)'
                     r'[ \t]*threadgroup\s+spvUnsafeArray<uint,\s*(\d+)>\s+(\w+);\n')
_NAME = re.compile(r'(\w+)\s*(?:\[\[.*\]\])?$')
_THREAD = re.compile(r'(uint3?)\s+(\w+)\s*\[\[(thread_position_in_threadgroup|threadgroup_position_in_grid)\]\]$')


def _params(text):
    out, depth, cur = [], 0, ''
    for ch in text:
        if ch == ',' and not depth:
            out.append(cur.strip())
            cur = ''
            continue
        depth += ch in '([<'
        depth -= ch in ')]>'
        cur += ch
    return out + [cur.strip()] if cur.strip() else out


def _name(param):
    m = _NAME.search(param)
    if not m: raise ValueError(f'no name in the kernel parameter {param}')
    return m[1]


def _members(comps):
    """{n: [index in comps or None, for x, y, z, w]} (n None for the position)"""
    return {n: [comps.index((n, c)) if (n, c) in comps else None for c in range(4)] for n in dict.fromkeys(n for n, _ in comps)}


def pixel_reads(sph):
    """byte 0x18 + n of an Ampere pixel SPH, two bits a component (0 unused)"""
    return {n: sum(1 << c for c in range(4) if sph[0x18 + n] >> (2 * c) & 3) for n in range(32) if sph[0x18 + n]}


def _width(written, read):
    """The written components, and a w its pixel shaders read but it does not write (the hardware's 1); a wider Metal fragment input reads 0 in x, y, z, the hardware's default"""
    hi = max((c + 1 for c in range(4) if written[c] is not None), default=0)
    return max(hi, 4 if (0xf if read is None else read) & 8 and written[3] is None else 0, 1)


def _value(width, base):
    """base[c] None: unwritten (0, 0, 0, 1 as a vertex output starts)."""
    parts = [f'as_type<float>({base[k]})' if base[k] else ('1.0' if k == 3 else '0.0') for k in range(width)]
    return parts[0] if width == 1 else f'float{width}(' + ', '.join(parts) + ')'


def _type(width):
    return 'float' if width == 1 else f'float{width}'


def _device(msl):
    """As dmsh_build.py tg_spill does a kernel's (keep the two in sync); the array template's own threadgroup overloads stay."""
    out = []
    for ln in msl.split('\n'):
        if 'operator [] (size_t pos)' not in ln:
            ln = re.sub(r'\bthreadgroup\b(?=\s)', 'device', ln)
            ln = re.sub(r'(?<!mem_device \| )mem_flags::mem_threadgroup\b', 'mem_flags::mem_device | mem_flags::mem_threadgroup', ln)
        out.append(ln)
    return '\n'.join(out)


def to_stage(msl, i, reads=None):
    """main0 (kind 'ts') or main0 and main0_z (kind 'ms', the two depth conventions). reads: {location: component mask} the paired pixel shaders read, None when
    not known; a location no SPH enables becomes (0,0,0,1), since Metal links no pixel input the mesh stage does not write."""
    kind = i['kind']
    m = _KERNEL.search(msl)
    if not m: raise ValueError(f'{kind}: no kernel main0 starting with its stage memory')
    params, local, words, mem = _params(m[1]), m[2].strip(), int(m[3]), m[4]
    names = [_name(p) for p in params]
    plain = [re.sub(r'\s*\[\[.*\]\]$', '', p) for p in params]
    # the body's early returns come back to the wrapper's copy of the outputs; one call a thread
    src = (msl[:m.start()] + f'static inline __attribute__((always_inline)) void dm_stage_body({", ".join(plain + [f"threadgroup spvUnsafeArray<uint, {words}>& {mem}"])})\n{{\n' +
           (f'    {local}\n' if local else '') + msl[m.end():])
    spill = kind == 'ms' and i['threads'] == 32 and SPILL_MIN <= words * 4 <= abi.MESH_SCRATCH_STRIDE
    if spill:
        i['spill'] = 1
        src = _device(src)
    call = f'dm_stage_body({", ".join(names + [mem])});'
    # the kernel's parameters where it has them: an attribute may appear once in a function
    own = {t[3]: t[2] + ('.x' if t[1] == 'uint3' else '') for t in map(_THREAD.match, params) if t}
    lid, tg = own.get('thread_position_in_threadgroup', 'dm_lid'), own.get('threadgroup_position_in_grid', 'dm_tg')
    extra = [f'uint {v} [[{a}]]' for a, v in (('thread_position_in_threadgroup', 'dm_lid'), ('threadgroup_position_in_grid', 'dm_tg')) if a not in own]
    lay, _ = memory(i)
    thr, pay = i['threads'], i['payload'] + 3 >> 2
    wg, inp, out, prim = lay[0] >> 2, lay[1] >> 2, lay[2] >> 2, lay[3]
    bar = 'mem_flags::mem_device | mem_flags::mem_threadgroup' if spill else 'mem_flags::mem_threadgroup'   # the device slice is ordered by the same barriers
    if spill:
        # Lane 0 claims the slice at ticket & mask by compare-and-swap of its flag (free, or of a finished submission); after MESH_SCRATCH_TRIES tickets the group counts itself
        # in the overflow word and draws nothing. The entry frees the flag after its last read. The slice starts zero, as the threadgroup array does
        w4 = words // 4
        h = '(device atomic_uint*)dm_scr'
        flag = f'{h} + {abi.MESH_SCRATCH_FLAGS // 4}u + dm_t'
        fence = '    atomic_thread_fence(mem_flags::mem_device, memory_order_seq_cst, thread_scope_device);\n'
        start = ('    uint dm_t = ~0u;\n'
                 f'    if ({lid} == 0u)\n    {{\n'
                 f'        uint dm_m = atomic_load_explicit({h} + {abi.MESH_SCRATCH_MASK}, memory_order_relaxed), dm_tag = atomic_load_explicit({h} + {abi.MESH_SCRATCH_TAG}, memory_order_relaxed);\n'
                 f'        uint dm_done = atomic_load_explicit({h} + {abi.MESH_SCRATCH_DONE}, memory_order_relaxed);\n'
                 f'        for (uint dm_k = 0u; dm_k < {abi.MESH_SCRATCH_TRIES}u; dm_k++)\n        {{\n'
                 f'            uint dm_s = atomic_fetch_add_explicit({h} + {abi.MESH_SCRATCH_TICKET}, 1u, memory_order_relaxed) & dm_m;\n'
                 f'            device atomic_uint* dm_f = {h} + {abi.MESH_SCRATCH_FLAGS // 4}u + dm_s;\n'
                 '            uint dm_v = atomic_load_explicit(dm_f, memory_order_relaxed);\n'
                 '            if ((dm_v == 0u || int(dm_done - dm_v) >= 0) && atomic_compare_exchange_weak_explicit(dm_f, &dm_v, dm_tag, memory_order_relaxed, memory_order_relaxed)) { dm_t = dm_s; break; }\n'
                 '        }\n'
                 f'        if (dm_t == ~0u) atomic_fetch_add_explicit({h} + {abi.MESH_SCRATCH_OVER}, 1u, memory_order_relaxed);\n    }}\n'
                 '    dm_t = simd_broadcast_first(dm_t);\n'
                 f'    if (dm_t == ~0u)\n    {{\n        if ({lid} == 0u) dm_out.set_primitive_count(0u);\n        return;\n    }}\n' + fence +
                 f'    device spvUnsafeArray<uint, {words}>& {mem} = *(device spvUnsafeArray<uint, {words}>*)(dm_scr + {abi.MESH_SCRATCH_HEAD}ul + ulong(dm_t) * {abi.MESH_SCRATCH_STRIDE}ul);\n'
                 f'    for (uint i = {lid}; i < {w4}u; i += {thr}u) ((device uint4*)&{mem}[0])[i] = uint4(0u);\n' +
                 (f'    for (uint i = {w4 * 4}u + {lid}; i < {words}u; i += {thr}u) {mem}[i] = 0u;\n' if words % 4 else '') +
                 f'    threadgroup_barrier({bar});\n')
        release = (f'    threadgroup_barrier({bar});\n' + fence +   # every lane has read its outputs
                   f'    if ({lid} == 0u) atomic_store_explicit({flag}, 0u, memory_order_relaxed);\n')
    else:
        start = (f'    threadgroup spvUnsafeArray<uint, {words}> {mem};\n'
                 f'    for (uint i = {lid}; i < {words}u; i += {thr}u) {mem}[i] = 0u;\n'
                 f'    threadgroup_barrier({bar});\n')
        release = ''
    head = f'struct dm_payload_t {{ uint w[{max(pay, 1)}]; }};\n'
    sig = ', '.join(params + extra)
    draw = f'constant uint* dm_draw [[buffer({abi.MESH_DRAW_SLOT})]]'
    scr = f', device uchar* dm_scr [[buffer({abi.MESH_SCRATCH_SLOT})]]' if spill else ''
    if kind == 'ts':
        return src + '\n' + head + (
            f'[[object]] void main0({sig}, object_data dm_payload_t& dm_payload [[payload]], mesh_grid_properties dm_grid, {draw})\n{{\n{start}'
            f'    if ({lid} == 0u) {mem}[{wg}] = dm_draw[1] + {tg};\n'
            f'    threadgroup_barrier({bar});\n    {call}\n    threadgroup_barrier({bar});\n'
            f'    for (uint i = {lid}; i < {pay}u; i += {thr}u) dm_payload.w[i] = {mem}[{out}u + i];\n'
            f'    if ({lid} == 0u) dm_grid.set_threadgroups_per_grid(uint3(min({mem}[{out + 1}], {METAL_MAX_GRID}u), 1u, 1u));\n}}\n')
    vertex, primitive = i['vertex'], i['primitive']
    max_v, max_p = i['max_v'], i['max_p']
    verts = i.get('verts', 3)
    if verts not in TOPOLOGY: raise ValueError(f'ms: primitive vertex count {verts} is not 1, 2 or 3')
    if max_v > METAL_MAX_VERTICES or max_p > METAL_MAX_PRIMITIVES: raise ValueError(f'ms: {max_v} vertices and {max_p} primitives exceed a Metal mesh')
    v_blocks = max(max(max_v, 1) + BLOCK - 1, 0) // BLOCK
    nsys = i.get('primitive_system', 0)
    v_stride, p_stride = max(len(vertex), 1) * BLOCK, max(per_primitive(i), 1) * BLOCK
    p_out = out + v_blocks * len(vertex) * BLOCK
    word = lambda comps: [f'{mem}[b + {comps[k] * BLOCK}u]' if comps[k] is not None else None for k in range(4)]
    vm, pm = _members(vertex), {n: [c + nsys if c is not None else None for c in v] for n, v in _members(primitive).items()}
    pb = primitive_builtins(i)
    # Each output as wide as its pixel shaders can see (_width): Metal counts the declared outputs against a mesh's 32 KB
    read = (lambda n: reads.get(n, 0)) if reads is not None else (lambda n: None)
    miss = sorted(set(reads or ()) - {n for n in vm if isinstance(n, int)} - set(pm))
    vw = {n: _width(c, read(n)) for n, c in vm.items() if isinstance(n, int)}
    pw = {n: _width(c, read(n)) for n, c in pm.items()}
    mw = {n: _width([None] * 4, read(n)) for n in miss}
    # Invariant, as a vertex shader's: a mesh depth pre-pass and a mesh colour pass test EQUAL
    head += ('struct dm_vertex_t {\n    float4 position [[position, invariant]];\n' +
             ('    float point_size [[point_size]];\n' if 'psiz' in vm else '') +
             ''.join(f'    {_type(w)} a{n} [[user(locn{n})]];\n' for n, w in vw.items()) +
             ''.join(f'    {_type(w)} dm_pad{n} [[user(locn{n})]];\n' for n, w in mw.items()) + '};\n' +
             ('struct dm_prim_t {\n' + ''.join(f'    uint {name} [[{attr}]];\n' for name, attr, _ in pb) + ''.join(f'    {_type(w)} p{n} [[user(locn{n})]];\n' for n, w in pw.items()) + '};\n'
              if pm or pb else '') +
             f'using dm_mesh_t = metal::mesh<dm_vertex_t, {"dm_prim_t" if pm or pb else "void"}, {max_v}, {max_p}, metal::topology::{TOPOLOGY[verts]}>;\n')
    v_block = lambda v: f'{out}u + ({v} / {BLOCK}u) * {v_stride}u + {v} % {BLOCK}u'
    p_block = lambda p: f'{p_out}u + ({p} / {BLOCK}u) * {p_stride}u + {p} % {BLOCK}u'
    sets = ''.join(f'        v.point_size = as_type<float>({word(c)[0]});\n' if n == 'psiz' else
                   f'        v.position = {_value(4, word(c))};\n' if n is None else
                   f'        v.a{n} = {_value(vw[n], word(c))};\n' for n, c in vm.items()) + \
        ''.join(f'        v.dm_pad{n} = {_value(w, [None] * 4)};\n' for n, w in mw.items())
    psets = ''.join(f'        p.{name} = {mem}[b + {k * BLOCK}u];\n' for name, _, k in pb) + ''.join(f'        p.p{n} = {_value(pw[n], word(c))};\n' for n, c in pm.items())
    if None not in vm: raise ValueError('ms: its SPH enables no position component')

    def entry(name, gl):
        # With a task the payload is the input, word 0 the workgroup's number among the task's launches as the hardware leaves it (the task writes from +4)
        fill = (f'    for (uint i = {lid}; i < {pay}u; i += {thr}u) {mem}[{inp}u + i] = dm_payload.w[i];\n'
                f'    threadgroup_barrier({bar});\n'
                f'    if ({lid} == 0u) {{ {mem}[{wg}] = {tg}; {mem}[{inp}] = {tg}; }}\n') if i['task'] else f'    if ({lid} == 0u) {mem}[{wg}] = dm_block.first + {tg};\n'
        return (f'[[mesh]] void {name}({sig}, dm_mesh_t dm_out' + (', const object_data dm_payload_t& dm_payload [[payload]]' if i['task'] else ', const object_data dm_block_t& dm_block [[payload]]') +
                scr + f')\n{{\n{start}{fill}'
                f'    threadgroup_barrier({bar});\n    {call}\n    threadgroup_barrier({bar});\n'
                f'    uint np = min({mem}[{prim >> 2}] >> 24, {max_p}u);\n'
                f'    if ({lid} == 0u) dm_out.set_primitive_count(np);\n'
                f'    for (uint n = {lid}; n < {max_v}u; n += {thr}u)\n    {{\n        uint b = {v_block("n")};\n        dm_vertex_t v;\n{sets}' +
                ('        v.position.z = fma(v.position.z, 0.5, v.position.w * 0.5);\n' if gl else '') +
                '        dm_out.set_vertex(n, v);\n    }\n' +
                (f'    for (uint n = {lid}; n < np; n += {thr}u)\n    {{\n        uint b = {p_block("n")};\n        dm_prim_t p;\n{psets}        dm_out.set_primitive(n, p);\n    }}\n' if pm or pb else '') +
                f'    for (uint n = {lid}; n < np * {verts}u; n += {thr}u)\n    {{\n        uint at = {prim + 4}u + n;\n        dm_out.set_index(n, ({mem}[at >> 2] >> ((at & 3u) * 8u)) & 0xffu);\n    }}\n' + release + '}\n')
    if i['task']: return src + '\n' + head + entry('main0', True) + '\n' + entry('main0_z', False)
    # main0_o: one thread a block of METAL_MAX_GRID workgroups
    head += 'struct dm_block_t { uint first; };\n'
    obj = (f'[[object]] void main0_o(object_data dm_block_t& dm_block [[payload]], mesh_grid_properties dm_grid, uint dm_otg [[threadgroup_position_in_grid]], {draw})\n{{\n'
           f'    uint b = dm_otg * {METAL_MAX_GRID}u;\n    dm_block.first = dm_draw[1] + b;\n'
           f'    dm_grid.set_threadgroups_per_grid(uint3(min(dm_draw[0] - b, {METAL_MAX_GRID}u), 1u, 1u));\n}}\n')
    return src + '\n' + head + entry('main0', True) + '\n' + entry('main0_z', False) + '\n' + obj
