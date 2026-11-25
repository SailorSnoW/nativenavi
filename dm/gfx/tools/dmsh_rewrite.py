#!/usr/bin/env python3
# The rewrites dmsh_build.py makes in a translated shader's MSL where Metal or the translator falls short of the Switch code (text functions only, so tests import them).
import bisect, collections, itertools, re
import dmsh_abi as abi, dmsh_side

# Maxwell's FMNMX returns the non-NaN operand, as precise:: (IEEE) does; fast:: leaves it to the GPU (docs/reference/shaders.md#min-max-and-clamp-with-a-nan-operand)
def precise(msl): return re.sub(r'\bfast::(min|max|clamp)\(', r'precise::\1(', msl)

PER_VERTEX_BASE = 64   # Declarations.PerVertexLocationBase: dm_pv<L>_<v> is at user(locn<64 + 3L + v>)
PER_VERTEX_IN = re.compile(r'\n    float4 dm_pv(\d+)_([012]) \[\[user\(locn(\d+)\)(?:, flat)?\]\];')
VERTEX_INDEX = ('first', 'second', 'third')
def per_vertex(msl):
    """LDTRAM's per-vertex reads (flat inputs dm_pv<L>_<v>) become one vertex_value<float4> at user(locn<L>), each read its get(vertex_index::...) (MSL 4.0).
    Raises ValueError when attribute L is also an ordinary input"""
    found = list(PER_VERTEX_IN.finditer(msl))
    if not found: return msl
    for m in found:
        if int(m[3]) != PER_VERTEX_BASE + 3 * int(m[1]) + int(m[2]): raise ValueError(f'per-vertex input dm_pv{m[1]}_{m[2]} at user(locn{m[3]}), not at the translator\'s location')
    locs = sorted({int(m[1]) for m in found})
    st = re.search(r'\nstruct main0_in\n\{(\n.*?)\n\};', msl, re.S)
    if not st or not all(st.start(1) <= m.start() < st.end(1) for m in found): raise ValueError('per-vertex inputs outside main0_in')
    both = [l for l in locs if re.search(rf'\[\[user\(locn{l}\)', st[1])]
    if both: raise ValueError(f'attribute {both[0]} read both per vertex and as an ordinary input')
    decls = ''.join(f'\n    vertex_value<float4> dm_pv{l} [[user(locn{l})]];' for l in locs)
    out = msl[:found[0].start()] + decls + ''.join(msl[a.end():b.start()] for a, b in zip(found, found[1:])) + msl[found[-1].end():]
    return re.sub(r'\.dm_pv(\d+)_([012])\b', lambda m: f'.dm_pv{m[1]}.get(vertex_index::{VERTEX_INDEX[int(m[2])]})', out)

BIT0_AND = re.compile(r'^[ \t]*(u?int) (_\d+) = ([^;\n]+) & 1u?;$', re.M)
BIT0_LOOSER = re.compile(r'\|\||&&|[|^?,]')   # `a | b & 1` is a | (b & 1), not bit 0
def bit0_operand(e):
    while True:
        f = re.sub(r'\([^()]*\)', '', e)
        if f == e: return not BIT0_LOOSER.search(e)
        e = f
BIT0_TEST = re.compile(r'^[ \t]*bool (_\d+) = (_\d+) != 0u?;$', re.M)
BIT0_MASK = re.compile(r'= (_\d+) \? \(-1\) : 0;')
BIT0_WRITE = re.compile(r'\b(_\d+)\b[ \t]*(?:<<|>>|[-+*/%&|^])?=(?!=)')
BIT0_OTHER = re.compile(r'(?:\+\+|--)[ \t]*(_\d+)\b|\b(_\d+)[ \t]*(?:\+\+|--)|&(_\d+)\b')
BIT0_BODY = re.compile(r'^\{\n.*?^\}$', re.M | re.S)   # a function's body: its braces alone in column 0
def bit0_mask(msl):
    """`-(x & 1)` for the translator's `b ? -1 : 0` of `bool b = (x & 1) != 0`, so the compare has its branch alone: Apple's M3+ GPU compiler can pick a bit-test jump it
    cannot encode (docs/reference/shaders.md#a-jump-apples-gpu-compiler-cannot-encode-a-tested-bit-0s-mask). Only single-assignment temporaries; other shaders come back
    as the same object"""
    if ' ? (-1) : 0;' not in msl: return msl
    def body(f):   # SPIRV-Cross numbers its temporaries by function
        src = f[0]
        if ' ? (-1) : 0;' not in src: return src
        bits = {m[2]: m[1] for m in BIT0_AND.finditer(src) if bit0_operand(m[3])}
        tests = {m[1]: m[2] for m in BIT0_TEST.finditer(src) if m[2] in bits}
        writes = collections.Counter(m[1] for m in BIT0_WRITE.finditer(src))
        other = {m[1] or m[2] or m[3] for m in BIT0_OTHER.finditer(src)}
        tests = {b: a for b, a in tests.items() if writes[a] == 1 and writes[b] == 1 and a not in other and b not in other}
        def mask(m):
            a = tests.get(m[1])
            if a is None: return m[0]
            return '= -%s;   // bit 0 as a mask (bit0_mask)' % (a if bits[a] == 'int' else 'int(%s)' % a)
        return BIT0_MASK.sub(mask, src) if tests else src
    out = BIT0_BODY.sub(body, msl)
    return out if out != msl else msl

TG_FENCE = 'atomic_thread_fence(mem_flags::mem_threadgroup, memory_order_seq_cst, thread_scope_simdgroup);'
TG_FENCE_FN = re.compile(r'\bvoid dmsh_tg_fence\(\)\s*\{')   # tg_fence also looks for the name 'dmsh_tg_fence': rename both
TG_FENCE_CALL = re.compile(r'^    dmsh_tg_fence\(\);$', re.M)   # spirv-cross writes every block in braces, 4 spaces a level
TG_BARRIER = 'simdgroup_barrier(mem_flags::mem_threadgroup);   // dmsh_tg_fence() in uniform control flow (tg_fence)'
TG_KERNEL = re.compile(r'\bkernel\s+void\s+main0\s*\(')
TG_LEFT = re.compile(r'\breturn\b|\bdiscard_fragment\b')
def tg_fence(src, kernel=False):
    """dmsh_tg_fence() (SharedMemoryFence.cs: Ampere code relies on lockstep warps) as a simdgroup-scope fence on threadgroup memory. A fence does not make another
    lane's store visible on Apple's GPUs, so in a kernel a call at main0's top level with no return before it is a SIMD-group barrier
    (docs/reference/shaders.md#threadgroup-stores-in-warp-synchronous-code)"""
    if 'dmsh_tg_fence' not in src: return src
    m = TG_FENCE_FN.search(src)
    if not m: return src
    src = src[:m.end()] + '\n    ' + TG_FENCE + '\n' + src[block_end(src, m.end() - 1):]
    k = TG_KERNEL.search(src) if kernel else None
    if not k: return src
    open_at = src.find('\n{\n', k.end())
    if open_at < 0: return src
    open_at += 1
    body_end = block_end(src, open_at)
    edits = []
    for c in TG_FENCE_CALL.finditer(src, open_at, body_end):
        depth = sum(1 if b.group() == '{' else -1 for b in BRACE.finditer(src, open_at, c.start()))
        if depth == 1 and not TG_LEFT.search(src, open_at, c.start()): edits.append(c.span())
    for a, b in reversed(edits): src = src[:a] + '    ' + TG_BARRIER + src[b:]
    return src

def tg_fence_counts(src):
    return len(re.findall(r'\bdmsh_tg_fence\(\);', src)), src.count(TG_BARRIER.split('   ')[1])

# RED.F32 reaches the MSL as a compare-and-swap loop (the translator's storage buffers are uint arrays): the exact loop spirv-cross writes becomes one
# atomic_fetch_add, its retry flag kept as false (docs/reference/shaders.md#float-atomics-in-storage-memory). spvFAdd too: one atomic add has nothing to contract
FLOAT_ADD = re.compile(r'(?m)^(?P<ind> *)bool (?P<again>_\d+);\n(?P=ind)do\n(?P=ind)\{\n'
                       r'(?P=ind)    float (?P<old>_\d+) = as_type<float>\((?P<addr>[^\n]*?)\);\n'
                       r'(?P=ind)    float (?P<sum>_\d+) = (?:(?P=old) \+ (?P<val>[^;\n]+)|spvFAdd\((?P=old), (?P<valp>[^;\n]+)\));\n'
                       r'(?P=ind)    uint (?P<want>_\d+) = as_type<uint>\((?P=old)\);\n(?P=ind)    uint (?P<seen>_\d+);\n(?P=ind)    do\n(?P=ind)    \{\n(?P=ind)        (?P=seen) = (?P=want);\n'
                       r'(?P=ind)    \} while \(!atomic_compare_exchange_weak_explicit\(\(device atomic_uint\*\)&(?P<addr2>[^\n]*?), &(?P=seen), as_type<uint>\((?P=sum)\), '
                       r'memory_order_relaxed, memory_order_relaxed\) && (?P=seen) == (?P=want)\);\n'
                       r'(?P=ind)    uint (?P<found>_\d+) = (?P=seen);\n(?P=ind)    (?P=again) = int\((?P=found)\) != as_type<int>\((?P=old)\);\n(?P=ind)\} while \((?P=again)\);\n')
# The add through a noinline leaf: inline on a dispatch-uniform address, Apple's AGX3 compiler (AIR-NT 32023.922; M5 and M5 Pro, macOS 26.0-26.6) hoists
# @llvm.agx3.disable.smashing.float into agc.main.constant_program and MTLCompilerService aborts (docs/reference/shaders.md#uniform-float-atomic-adds-crash-apples-agx3-compiler)
FLOAT_ADD_HELPER = ('#ifndef DM_ATOMIC_ADD_F32\n#define DM_ATOMIC_ADD_F32\n'
                    'static __attribute__((noinline)) void dm_atomic_add_f32(device atomic_float* p, float v) { atomic_fetch_add_explicit(p, v, memory_order_relaxed); }\n#endif\n')
def float_add_msl(msl):
    if 'atomic_compare_exchange_weak_explicit' not in msl: return msl
    out = FLOAT_ADD.sub(lambda m: m.group(0) if m.group('addr') != m.group('addr2') else
                        f"{m.group('ind')}dm_atomic_add_f32((device atomic_float*)&{m.group('addr')}, {m.group('val') or m.group('valp')});\n"
                        f"{m.group('ind')}bool {m.group('again')} = false;\n", msl)
    if out == msl or '#define DM_ATOMIC_ADD_F32' in out: return out
    head = re.search(r'^using namespace metal;\n', out, re.M)
    if not head: raise ValueError('float_add_msl: no "using namespace metal;" to put dm_atomic_add_f32 after')
    return out[:head.end()] + FLOAT_ADD_HELPER + out[head.end():]

# spvMulExtended is optnone, so every product stays an unfolded call; scalar operands get overloads that take the 64-bit product's halves, vectors keep the template
# (docs/reference/shaders.md#extended-products-behind-spirv-crosss-optnone-helper)
def mul_extended(msl):
    helper = re.compile(r'template<typename T, typename U, typename V>\s*\[\[clang::optnone\]\] T spvMulExtended\(V l, V r\)\s*\{\s*return T\{U\(l \* r\), U\(mulhi\(l, r\)\)\};\s*\}\n')
    found = list(helper.finditer(msl)) if 'spvMulExtended' in msl else []
    if len(found) != 1 or 'spvMulExtended(uint l, uint r)' in msl: return msl
    scalar = ''.join(f'template<typename T, typename U> static inline __attribute__((always_inline)) T spvMulExtended({t} l, {t} r) {{ {w} p = {w}(l) * {w}(r); return T{{U(p), U(p >> 32)}}; }}\n'
                     for t, w in (('uint', 'ulong'), ('int', 'long')))
    return msl[:found[0].end()] + scalar + msl[found[0].end():]

# Bounded uniform reads (MetalUniformBounds): spirv-cross writes the translator's load-and-select as cond ? read : 0, a branch to Metal (which an M1's compiler gets
# wrong); select_guards writes each as select() or raises ValueError (docs/reference/shaders.md#bounded-uniform-reads-written-as-select)
_GSIZE = r'\(\(\*spvDescriptorSet0\.[a-z]+_c%d\)\._m0\[\d\]\.[xyzw] >> (?:2|4)u\)' % abi.CBSIZE_BANK   # a slot's size in words or vec4 rows
_GZERO = r'0(?:\.0|u)?'
def _gchoice(cond, read):
    return re.compile('|'.join(form % (n, cond, n, read % n, n, _GZERO) for n, form in
                               ((1, r'(?<![\w>])\((?P<q%d>%s) \? (?P<r%d>%s) : (?P<z%d>%s)\)'), (2, r'(?P<q%d>%s) \? (?P<r%d>%s) : (?P<z%d>%s)(?![\w.])'))))
# Starts with the text "bool ", which re skips to; the lookbehind is the \b (test_shader_rewrites.py GuardScans)
_GBOOL = re.compile(r'bool (?<!\wbool )(_\d+) = [^;\n]* < %s;' % _GSIZE)   # the condition: the read's index and its choice both use it
_GROW = _gchoice(r'_\d+', r'\(\*spvDescriptorSet0\.[a-z]+_c\d+\)\._m0\[(?P<i%s>_\d+) \? [^\]\n]+ : 0\](?:\[[^\]\n]+\]|\.[xyzw])')
_GLEFT = re.compile(r'%s\) \?' % _GSIZE)
# A bounded storage buffer read (MetalStorageBounds): the same choice, also a select, so its words stay plain for wide_loads
_STABLE = r'\(\*spvDescriptorSet0\.[a-z]+_c%d\)\._m0\[\d+\]\.[xyzw]' % abi.SBLEN_BANK
_SBOOL = re.compile(r'bool (?<!\wbool )(_\d+) = \(uint\(_\d+\) \+ %s\) < %s;' % (_STABLE, _STABLE))
_SROW = _gchoice(r'_\d+', r'\(\*spvDescriptorSet0\.[a-z]+_s\d+\)\._m0\[(?P<i%s>_\d+) \? [^\]\n]+ : 0\]')
_GNAME = re.compile(r'(?<!\[)\b(_\d+) \?')   # a choice on a name; a read's index has its own, after the bracket
# These have no literal prefix: each is tried only where text every match holds is found, with the same matches as sub / finditer (GuardScans)
_GREAD = ' ? (*spvDescriptorSet0.'   # follows the condition of every choice of a read

def _gsub(pat, repl, msl):
    """a match starts at its condition, the last _ before a _GREAD, or at the parenthesis before that"""
    k = msl.find(_GREAD)
    if k < 0: return msl
    out, pos, low = [], 0, 0
    while k >= 0:
        s = msl.rfind('_', low, k)
        m = (pat.match(msl, s - 1) if s > pos and msl[s - 1] == '(' else None) or (pat.match(msl, s) if s >= 0 else None)
        if m:
            out += (msl[pos:m.start()], repl(m))
            pos = low = m.end()
        else: low = k + 1
        k = msl.find(_GREAD, low)
    out.append(msl[pos:])
    return ''.join(out)

def _gleft(msl, conds):
    """a match ends with " ?" and starts at the last _ before it"""
    k, low = msl.find(' ?'), 0
    while k >= 0:
        s = msl.rfind('_', low, k)
        m = _GNAME.match(msl, s) if s >= 0 else None
        if m and m.group(1) in conds: return m
        low = k + 2
        k = msl.find(' ?', low)
    return None

def select_guards(msl):
    if 'spvDescriptorSet0' not in msl: return msl
    bools, sbools = set(_GBOOL.findall(msl)), set(_SBOOL.findall(msl))
    def select(m, conds):
        n = '1' if m.group('q1') else '2'
        c = m.group('q' + n)
        if c not in conds or m.group('i' + n) != c: return m.group(0)
        return 'select(%s, %s, %s)' % (m.group('z' + n), m.group('r' + n), c)
    if bools: msl = _gsub(_GROW, lambda m: select(m, bools), msl)
    if sbools: msl = _gsub(_SROW, lambda m: select(m, sbools), msl)
    left = _GLEFT.search(msl)
    conds = bools | sbools
    if not left and conds: left = _gleft(msl, conds)
    if left: raise ValueError('a bounded uniform or storage read is not written as a select: ' + msl[msl.rfind('\n', 0, left.start()) + 1:left.end() + 100].split('\n')[0].strip()[:160])
    return msl

# A game uniform word tested against constants more than once (the translator's GLSL switch) becomes a volatile load per test (dm_uword), so the front end folds
# no switch (docs/reference/shaders.md#tests-of-one-uniform-word-folded-into-switches). The driver banks c0 to c2 stay as written: later rewrites look for them
_UTEST_READ = r'(u?int)>\((\(\*spvDescriptorSet0\.[a-z]+_c(?:[3-9]|\d\d)\)\._m0\[\d+\])\.([xyzw])\)'   # game slots c3 and up, not the driver's banks
# The literal prefix is written three times (the start and both lookbehinds): change all three (test_shader_rewrites.py UniformMatchScan)
_UTEST = re.compile(r'as_type<(?:(?<![\w.]as_type<)%s(?= [!=]= -?\d+u?(?![\w.]))|(?<=[!=]= as_type<)%s)' %
                   (_UTEST_READ, _UTEST_READ.replace('(u?int)', '(?P<t2>u?int)').replace(r'(\(\*', r'(?P<r2>\(\*').replace('([xyzw])', '(?P<c2>[xyzw])')))
_UTEST_LEFT = re.compile(r'(?<![\w.])-?\d+u? [!=]= $')
UWORD = ('#ifndef DM_UWORD\n#define DM_UWORD\ntemplate<typename T, int N> static inline __attribute__((always_inline)) T dm_uword(const constant vec<T, N>& v, int i) '
         '{ return ((const volatile constant T*)&v)[i]; }\n#endif\n')
def uniform_tests(msl):
    if 'spvDescriptorSet0' not in msl or 'as_type<' not in msl: return msl
    def parts(m):
        if m.group(1): return m.group(1), m.group(2), m.group(3)
        if not _UTEST_LEFT.search(msl, max(0, m.start() - 16), m.start()): return None
        return m.group('t2'), m.group('r2'), m.group('c2')
    found = [(m, p) for m in _UTEST.finditer(msl) for p in [parts(m)] if p]
    tested = collections.Counter(r + c for _, (_, r, c) in found)
    chains = {w for w, n in tested.items() if n > 1}
    if not chains: return msl
    out, at = [], 0
    for m, (t, r, c) in found:
        if r + c not in chains: continue
        out += [msl[at:m.start()], 'as_type<%s>(dm_uword(%s, %d))' % (t, r, 'xyzw'.index(c))]
        at = m.end()
    body = ''.join(out) + msl[at:]
    head = re.search(r'^using namespace metal;\n', body, re.M)
    if not head: raise ValueError('uniform tests: no "using namespace metal;" to put dm_uword after')
    return body[:head.end()] + UWORD + body[head.end():]

# Ampere HMMA.16816 f16 (dmsh_hmma16816_f16(lane, block, A0..A3, B0, B1, C0, C1, flags, A's LDSM address, A0..A3's offsets, A's row stride, out D1) → D0) as simdgroup_matrix
# code over the same scratch block: A row-major (halves 0..255), B transposed (256..383), C / D row-major (384..511). TensorPass's flags keep a chain's matrices in
# registers (docs/reference/shaders.md#hmma-chains-keep-their-matrices-in-the-simd-group)
HMMA_BODY = '''{
    threadgroup uint* s = &%(scr)s[uint(%(blk)s)];
    threadgroup half* h = (threadgroup half*)s;
    const int f = int(%(fl)s), k = (f >> 8) * 2;
    const bool areuse = f & 1, cslot = f & 2, dslot = f & 4, czero = f & 8, adirect = %(adirect)s;
    const bool btake = bmode >= 0 && (bmode & 1), bfill = bmode >= 0 && !(bmode & 1); const int bk = (bmode >> 1) * 2;
    uint g = uint(%(lane)s) >> 2, t = uint(%(lane)s) & 3;
    if (!areuse && !adirect) { s[g * 8 + t] = uint(%(a0)s); s[(g + 8) * 8 + t] = uint(%(a1)s); s[g * 8 + 4 + t] = uint(%(a2)s); s[(g + 8) * 8 + 4 + t] = uint(%(a3)s); }
    if (!btake) { s[128 + g * 8 + t] = uint(%(b0)s); s[128 + g * 8 + 4 + t] = uint(%(b1)s); }
    if (!cslot && !czero) { s[192 + g * 4 + t] = uint(%(acc0)s); s[192 + (g + 8) * 4 + t] = uint(%(acc1)s); }
    if (!btake || (!areuse && !adirect) || (!cslot && !czero)) simdgroup_barrier(mem_flags::mem_threadgroup);
    if (!areuse && adirect) {
        threadgroup half* x = %(shm_base)s + (int(simd_broadcast(uint(%(aaddr)s), ushort(0))) >> 1);
        const ulong st = ulong(uint(%(astr)s) >> 1); const bool tr = f & 32;
        simdgroup_load(am[0], x + (int(%(ao0)s) >> 1), st, ulong2(0, 0), tr); simdgroup_load(am[1], x + (int(%(ao2)s) >> 1), st, ulong2(0, 0), tr);
        simdgroup_load(am[2], x + (int(%(ao1)s) >> 1), st, ulong2(0, 0), tr); simdgroup_load(am[3], x + (int(%(ao3)s) >> 1), st, ulong2(0, 0), tr);
    } else if (!areuse) {
        simdgroup_load(am[0], h, 16, ulong2(0, 0)); simdgroup_load(am[1], h, 16, ulong2(8, 0)); simdgroup_load(am[2], h, 16, ulong2(0, 8)); simdgroup_load(am[3], h, 16, ulong2(8, 8));
    }
    // C and D in the call's own matrices: a slot is read only for a chain's C (cslot) and written only for its D (dslot). A call outside every chain has slot 0 in its
    // flags (no slot bits); working in acc[0..1] it overwrote the accumulator a chain held there between its two calls (Ocarina of Time's agl_ndsr_vr networks: 2,515 such
    // calls in 30 shaders, dotted rows across fog and light, GitHub issue #1537). The flags are constants at each inlined call, so the copies cost nothing
    // Ampere adds each 8 products to the accumulator and rounds once to f16, k 0-7 then k 8-15 (N_FMA 8 in Khattak and Mikaitis's A100 model, which matches recorded
    // A100 output in 5,000 of 5,000 cases), where simdgroup_half8x8 rounds after every product (2,420 of them). So each block runs in f32 (each product of two halves
    // exact, the sum in order) and its result is rounded to f16 (RNE): an f16 C, the first block's result and D are the only roundings
    // simdgroup_multiply_accumulate takes a float result from half operands and a half C. A lane's thread_elements() are the same two places in a half and a
    // float matrix, so the rounding is two conversions a lane. Rounding through a multiply-accumulate with zero A and B into a half result gives the same bits but
    // made the network twice as slow (NDSR's fa3033d05c0a41d6: 0.59 ms against 0.32 on an M5 Max). dm/gfx/msl/ndve_engine.metal's hmma() rounds the same way
    simdgroup_half8x8 b0, b1, c0, c1;
    simdgroup_float8x8 f0, f1;
    if (btake) { b0 = bm[bk]; b1 = bm[bk + 1]; }
    else { simdgroup_load(b0, h + 256, 16, ulong2(0, 0), true); simdgroup_load(b1, h + 256, 16, ulong2(8, 0), true); if (bfill) { bm[bk] = b0; bm[bk + 1] = b1; } }
    if (czero) { c0 = make_filled_simdgroup_matrix<half, 8, 8>(0.0h); c1 = c0; }
    else if (cslot) { c0 = acc[k]; c1 = acc[k + 1]; }
    else { simdgroup_load(c0, h + 384, 8, ulong2(0, 0)); simdgroup_load(c1, h + 384, 8, ulong2(0, 8)); }
    simdgroup_multiply_accumulate(f0, am[0], b0, c0); simdgroup_multiply_accumulate(f1, am[2], b0, c1);
    for (int i = 0; i < 2; i++) { c0.thread_elements()[i] = half(f0.thread_elements()[i]); c1.thread_elements()[i] = half(f1.thread_elements()[i]); }   // the first block's rounding
    simdgroup_multiply_accumulate(f0, am[1], b1, c0); simdgroup_multiply_accumulate(f1, am[3], b1, c1);
    for (int i = 0; i < 2; i++) { c0.thread_elements()[i] = half(f0.thread_elements()[i]); c1.thread_elements()[i] = half(f1.thread_elements()[i]); }
    if (dslot) { acc[k] = c0; acc[k + 1] = c1; simdgroup_barrier(mem_flags::mem_threadgroup); %(d1)s = 0; return 0; }
    simdgroup_store(c0, h + 384, 8, ulong2(0, 0)); simdgroup_store(c1, h + 384, 8, ulong2(0, 8));
    simdgroup_barrier(mem_flags::mem_threadgroup);
    uint d0 = s[192 + g * 4 + t], d1 = s[192 + (g + 8) * 4 + t];
    simdgroup_barrier(mem_flags::mem_threadgroup);
    %(d1)s = as_type<%(dt)s>(d1);
    return as_type<%(rt)s>(d0);
}'''

HMMA_ARGS = ['lane', 'blk', 'a0', 'a1', 'a2', 'a3', 'b0', 'b1', 'acc0', 'acc1', 'fl', 'aaddr', 'ao0', 'ao1', 'ao2', 'ao3', 'astr', 'd1']

HMMA_CALL = re.compile(r'\bdmsh_hmma16816_f16\(([^()]*)\)')

TG_ARRAY = re.compile(r'^[ \t]+threadgroup (spvUnsafeArray<uint, \d+>) (\w+);[ \t]*$', re.M)

def split_params(text):   # commas inside template arguments (spvUnsafeArray<uint, N>) do not separate parameters
    params, depth, cur = [], 0, ''
    for ch in text + ',':
        depth += {'<': 1, '>': -1}.get(ch, 0)
        if ch == ',' and depth == 0:
            params.append(cur.strip())
            cur = ''
        else:
            cur += ch
    return params

BRACE = re.compile(r'[{}]')

FUNC_OPEN = re.compile(r'(?=\n\{\n)')   # overlapping ones included

def block_end(src, start):   # the brace closing the block opened at src[start] (start when it is no brace)
    if start < len(src) and src[start] not in '{}': return start
    depth = 0
    for b in BRACE.finditer(src, start):
        depth += 1 if b.group() == '{' else -1
        if depth == 0: return b.start()
    raise ValueError('dmsh_hmma16816_f16: block not closed')

# Compute loop containment: a platform workaround, not a fix of a runaway loop's cause: committed GPU work can survive process exit and wedge the Mac
# (docs/reference/shaders.md#compute-loops-that-outlive-the-process). A loop holding a barrier has a private budget and never reads the abort word.
LOOP_CAP = 1 << 18
LOOP_ABORT_EVERY = 1024
_LOOP = re.compile(r'\b(for|while)\s*\(')
_LOOP_COMMENT = re.compile(r'//[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"', re.S)


def compute_loops(msl):
    """→ (MSL, loop sites). Other stages and loop-free compute shaders are unchanged."""
    if not re.search(r'^kernel\s+void\s+main0\s*\(', msl, re.M): return msl, []
    fns = []
    for m in FUNC_OPEN.finditer(msl):
        close = m.start() - 1
        if close < 0 or msl[close] != ')': continue
        depth, op = 1, close
        while depth and op:
            op -= 1
            depth += (msl[op] == ')') - (msl[op] == '(')
        name = re.search(r'(\w+)\s*$', msl[:op])
        if not name: continue
        start = msl.rfind('\n', 0, op) + 1
        body = m.end() + 2
        end = block_end(msl, body - 1)
        fns.append((name[1], start, op, close, body, end))
    names = {f[0] for f in fns}
    calls = {n: set() for n in names}
    bodies = {n: '' for n in names}
    def code(text): return _LOOP_COMMENT.sub(lambda m: re.sub(r'[^\n]', ' ', m[0]), text)
    for n, _, _, _, b, e in fns: bodies[n] += code(msl[b:e])
    def uses(text, n): return re.search(rf'\b{re.escape(n)}\s*(?:<[^;\n()]*>)?\s*\(', text)
    for n in names: calls[n] = {c for c in names if uses(bodies[n], c)}
    def grow(seed, edges):
        found = set(seed)
        while True:
            more = {n for n in names if edges(n, found)} - found
            if not more: return found
            found |= more
    reach = grow({'main0'}, lambda n, s: any(n in calls[c] for c in s))
    loops = {n for n in reach if _LOOP.search(bodies[n])}
    guarded = grow(loops, lambda n, s: bool(calls[n] & s)) & reach
    if not loops: return msl, []
    if re.search(rf'\[\[buffer\({abi.LOOP_SLOT}\)\]\]', msl): raise ValueError(f'compute buffer {abi.LOOP_SLOT} is taken')
    barriers = grow({n for n in names if re.search(r'\b(?:threadgroup|simdgroup)_barrier\s*\(', bodies[n])}, lambda n, s: bool(calls[n] & s))
    edits, sites = [], []
    def call_edits(text):
        out = []
        for c in guarded:
            for call in re.finditer(rf'\b{re.escape(c)}\s*(?:<[^;\n()]*>)?\s*\(', code(text)):
                tail = dmsh_side._close(text, call.end(), '(', ')')
                if tail < 0: raise ValueError(f'loop containment: unclosed call to {c}')
                out.append((tail - 1, (', ' if text[call.end():tail - 1].strip() else '') + 'dm_loop'))
        return out
    for n, _, op, close, b, e in fns:
        if n not in guarded: continue
        param = 'device dm_loop_state& dm_loop' + (f' [[buffer({abi.LOOP_SLOT})]]' if n == 'main0' else '')
        edits.append((close, close, (', ' if msl[op + 1:close].strip() else '') + param))
        body, conditions = msl[b:e], []
        masked = code(body)
        if n in loops: edits.append((b, b, '\n    uint dm_lg = 0u;'))
        for loop in _LOOP.finditer(masked):
            close = dmsh_side._close(masked, loop.end(), '(', ')') - 1
            if close < 0: raise ValueError(f'loop containment: unclosed condition in {n}')
            lo, hi = loop.end(), close
            if loop[1] == 'for':
                semis = [m.start() for m in re.finditer(';', masked[lo:hi])]
                if len(semis) != 2: raise ValueError(f'loop containment: unknown loop in {n}')
                lo, hi = lo + semis[0] + 1, lo + semis[1]
            after = close + 1
            while after < len(masked) and masked[after].isspace(): after += 1
            before = masked[:loop.start()].rstrip()
            begin = loop.start()
            if loop[1] == 'while' and after < len(masked) and masked[after] == ';' and before.endswith('}'):
                # A do/while: its body found backwards.
                depth, start = 1, len(before) - 1
                while depth and start:
                    start -= 1
                    depth += (masked[start] == '}') - (masked[start] == '{')
                do = re.search(r'\bdo\s*$', masked[:start])
                if not do: raise ValueError(f'loop containment: no do in {n}')
                begin, end = do.start(), after + 1
                inside = masked[start:len(before)]
            elif after < len(masked) and masked[after] == '{':
                end = block_end(masked, after) + 1
                inside = masked[after:end]
            else:
                end = masked.find(';', after)
                if end < 0: raise ValueError(f'loop containment: no loop body in {n}')
                inside = masked[after:end]
                end += 1
            context = masked[loop.end():close] + inside
            barrier = bool(re.search(r'\b(?:threadgroup|simdgroup)_barrier\s*\(', context)) or any(uses(context, c) for c in barriers)
            site = len(sites) + 1
            sites.append({'site': site, 'function': n, 'line': msl.count('\n', 0, b + loop.start()) + 1, 'barrier': barrier})
            counter = f'dm_lg_b{site}' if barrier else 'dm_lg'
            if barrier:
                edits.extend(((b + begin, b + begin, f'{{ uint {counter} = 0u; '), (b + end, b + end, ' }')))
            check = f'dm_lg_{"cap" if barrier else "go"}({counter}, dm_loop, {site}u)'
            out = body[lo:hi].strip() or 'true'
            for at, text in sorted(call_edits(out), reverse=True): out = out[:at] + text + out[at:]
            edits.append((b + lo, b + hi, f'({out}) && {check} /* dm_loop_site {site}: {n} */'))
            conditions.append((lo, hi))
        for at, text in call_edits(body):
            if not any(lo <= at <= hi for lo, hi in conditions): edits.append((b + at, b + at, text))
    for a, b, text in sorted(edits, reverse=True): msl = msl[:a] + text + msl[b:]
    head = re.search(r'^using namespace metal;\s*\n', msl, re.M)
    if not head: raise ValueError('loop containment: no metal namespace')
    helper = f'''
struct dm_loop_state
{{
    device atomic_uint* abort;
    atomic_uint hit;
}};
static inline __attribute__((always_inline)) bool dm_lg_cap(thread uint& n, device dm_loop_state& s, uint site)
{{
    if (++n < {LOOP_CAP}u) return true;
    if (n == {LOOP_CAP}u)
    {{
        atomic_fetch_min_explicit(&s.hit, site, memory_order_relaxed);
        atomic_store_explicit(s.abort, 1u, memory_order_relaxed);
    }}
    return false;
}}
static inline __attribute__((always_inline)) bool dm_lg_go(thread uint& n, device dm_loop_state& s, uint site)
{{
    if (!dm_lg_cap(n, s, site)) return false;
    return (n & {LOOP_ABORT_EVERY - 1}u) != 0u || atomic_load_explicit(s.abort, memory_order_relaxed) == 0u;
}}
'''
    return msl[:head.end()] + helper + msl[head.end():], sites

# B kept in the SIMD-group (hmma_bslots): the matrix hardware fills its matrices only from memory, so B's trip through the block can only be left out where it repeats
HMMA_COPY = re.compile(r'(?:as_type<\w+>|int|uint|float|half|short|ushort)?\((\w+)\)|(\w+)')

HMMA_DECL = re.compile(r'^[ \t]*(?:const[ \t]+)?[A-Za-z_]\w*(?:<[^<>;]*>)?[ \t]+(\w+)[ \t]*=[ \t]*([^;\n]+);', re.M)

HMMA_WRITE = re.compile(r'\b(\w+)[ \t]*(?:<<|>>|[-+*/%&|^])?=(?!=)|\+\+[ \t]*(\w+)|\b(\w+)[ \t]*\+\+|--[ \t]*(\w+)|\b(\w+)[ \t]*--|&[ \t]*(\w+)')

def hmma_bslots(msl, open_at, end, fcalls, writers):
    """→ {call start: slot * 2 (keeps its B) or slot * 2 + 1 (takes it)}. Same B: the same variables after copies, down to one written once at the function's top
    level (a call's out argument counts as a write); a top-level call's B is kept for the calls after it in the text"""
    body = msl[open_at:end]
    writes = collections.Counter(next(g for g in w.groups() if g) for w in HMMA_WRITE.finditer(body))
    for c, args in fcalls: writes[args[HMMA_ARGS.index('d1')]] += 1
    for name in writers:
        for w in re.finditer(r'\b%s\(' % name, body):
            paren_depth, j = 1, w.end()
            while paren_depth and j < len(body):
                paren_depth += {'(': 1, ')': -1}.get(body[j], 0)
                j += 1
            writes.update(re.findall(r'\b[A-Za-z_]\w*\b', body[w.end():j]))
    braces = [(b.start() + open_at, 1 if b.group() == '{' else -1) for b in BRACE.finditer(body)]
    at, depth_after = [p for p, _ in braces], list(itertools.accumulate(d for _, d in braces))
    def depth(pos):   # 1: the function's top level
        k = bisect.bisect_left(at, pos) - 1
        return depth_after[k] if k >= 0 else 0
    decls = {}
    for d in HMMA_DECL.finditer(body): decls.setdefault(d.group(1), (d.group(2).strip(), d.start(1) + open_at))
    def canon(name):
        for _ in range(64):
            if writes[name] != 1 or name not in decls: return None
            init, pos = decls[name]
            m = HMMA_COPY.fullmatch(init)
            inner = m and (m.group(1) or m.group(2))
            if not inner or inner[0].isdigit(): return name if depth(pos) == 1 else None
            name = inner
        return None
    by_b = collections.defaultdict(list)
    for c, args in fcalls:
        k = (canon(args[HMMA_ARGS.index('b0')]), canon(args[HMMA_ARGS.index('b1')]))
        if None not in k: by_b[k].append(c.start())
    modes, slot = {}, 0
    for starts in by_b.values():
        first = next((i for i, st in enumerate(starts) if depth(st) == 1), None)
        if first is None or first == len(starts) - 1: continue
        modes[starts[first]] = slot * 2
        modes.update((st, slot * 2 + 1) for st in starts[first + 1:])
        slot += 1
    return modes

def hmma_msl(msl):
    m = re.search(r'\n(\w+) dmsh_hmma16816_f16\(([^)]*)\)\n\{', msl)
    if not m: return msl
    params = split_params(m.group(2))
    scr = [x for x in params if x.startswith('threadgroup ')]
    plain = [x for x in params if not x.startswith('threadgroup ')]
    if len(scr) != 1 or len(plain) != len(HMMA_ARGS): raise ValueError('dmsh_hmma16816_f16: unexpected parameters ' + m.group(2)[:200])
    def group1(pattern, text, how=re.search):
        found = how(pattern, text)
        if not found: raise ValueError('dmsh_hmma16816_f16: unexpected parameter ' + text[:100])
        return found.group(1)
    name = lambda x: group1(r'(\w+)$', x)
    v = dict(zip(HMMA_ARGS, [name(x) for x in plain]), scr=name(scr[0]), dt=group1(r'thread (\w+)&', plain[-1], re.match), rt=m.group(1))
    body_end = block_end(msl, m.end() - 1)
    # Functions' openings and ends are found once: a network's function holds hundreds of calls in megabytes of MSL. A call's function opens at the last brace line before it
    opens = [o.start() for o in FUNC_OPEN.finditer(msl)]
    calls, callers, ends = [], {}, {}
    for c in HMMA_CALL.finditer(msl):
        if m.start() <= c.start() <= body_end: continue
        k = bisect.bisect_right(opens, c.start() - 3) - 1
        open_at = opens[k] + 1 if k >= 0 else 0
        if open_at > 0 and open_at not in ends: ends[open_at] = block_end(msl, open_at)
        if open_at <= 0 or ends[open_at] < c.start(): raise ValueError('dmsh_hmma16816_f16: a call outside a function body')
        if open_at not in callers:
            scratch = split_params(c.group(1))[-1]
            arrays = [(t, n) for t, n in TG_ARRAY.findall(msl, open_at, ends[open_at]) if n != scratch]
            callers[open_at] = arrays[0] if len(arrays) == 1 else None
        calls.append((c, open_at))
    if not calls: return msl
    shared = set(callers.values())
    direct = len(shared) == 1 and None not in shared
    writers = {f.group(1) for f in re.finditer(r'\b(\w+)\(([^()]*)\)\s*\{', msl) if re.search(r'\bthread (?!const\b)[\w<>, ]+&', f.group(2)) and f.group(1) != 'dmsh_hmma16816_f16'}
    modes = {}
    for open_at in callers:
        modes.update(hmma_bslots(msl, open_at, ends[open_at], [(c, split_params(c.group(1))) for c, o in calls if o == open_at], writers))
    nb = max([2] + [(v // 2 + 1) * 2 for v in modes.values()])
    extra = (', thread simdgroup_half8x8 (&acc)[16], thread simdgroup_half8x8 (&am)[4], thread simdgroup_half8x8 (&bm)[%d], const int bmode' % nb
             + (', threadgroup %s& dm_shm' % next(iter(shared))[0] if direct else ''))
    v.update(adirect='(f & 16) != 0' if direct else 'false', shm_base='(threadgroup half*)&dm_shm[0]' if direct else '(threadgroup half*)nullptr')
    # Edits of the original text, applied from its end
    edits = [(m.start(), body_end + 1, '\n%s dmsh_hmma16816_f16(%s%s)\n%s' % (m.group(1), m.group(2), extra, HMMA_BODY % v))]   # spirv-cross has it inlined (always_inline, the line before)
    edits += [(c.end() - 1, c.end() - 1, ', dm_acc, dm_am, dm_bm, %d' % modes.get(c.start(), -1) + (', ' + callers[open_at][1] if direct else '')) for c, open_at in calls]
    edits += [(open_at + 2, open_at + 2, '    simdgroup_half8x8 dm_acc[16], dm_am[4], dm_bm[%d];\n' % nb) for open_at in callers]
    for a, b, text in sorted(edits, key=lambda e: e[0], reverse=True):
        msl = msl[:a] + text + msl[b:]
    return msl

# A store only a dispatch's first invocation makes (`if (-local.x == workgroup.x << log2(local size))`, an append counter's reset): Apple's GPUs start workgroups in
# no order, so the host makes it before the dispatch. A guarded store not understood leaves the shader as it is. → (shader, [(binding, word, value)])
FIRST_STORE = re.compile(r'float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*int (\w+) = as_type<int>\(\1\) << (\d+);\s*'
                         r'int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \5 \+ 1;\s*bool (\w+) = \6 != \3;\s*')

# A local size with rows (8x8): the same test on y and x, and workgroup z zero
FIRST_STORE_2D = re.compile(r'float (\w+) = as_type<float>\(gl_WorkGroupID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.y\);\s*float (\w+) = as_type<float>\(gl_LocalInvocationID\.x\);\s*'
                            r'float (\w+) = as_type<float>\(gl_WorkGroupID\.x\);\s*float (\w+) = as_type<float>\(gl_WorkGroupID\.z\);\s*'
                            r'int (\w+) = as_type<int>\(\1\) << (\d+);\s*int (\w+) = ~as_type<int>\(\2\);\s*int (\w+) = \8 \+ 1;\s*bool (\w+) = \9 != \6;\s*'
                            r'int (\w+) = as_type<int>\(\4\) << (\d+);\s*int (\w+) = ~as_type<int>\(\3\);\s*int (\w+) = \13 \+ 1;\s*bool (\w+) = \14 == \11;\s*'
                            r'bool (\w+) = !\10;\s*bool (\w+) = \15 && \16;\s*bool (\w+) = (?:as_type<int>\(\5\) == 0|0 == as_type<int>\(\5\));\s*bool (\w+) = \18 && \17;\s*(?:bool (\w+) = !\19;\s*)?')

def first_store(msl, local):
    if not local or local[2:] != [1]: return msl, []
    m = FIRST_STORE.search(msl)
    if m:
        if local[1] != 1 or 1 << int(m.group(4)) != local[0]: return msl, []
        guard = r'!' + re.escape(m.group(7))
    else:
        m = FIRST_STORE_2D.search(msl)
        if not m or 1 << int(m.group(7)) != local[1] or 1 << int(m.group(12)) != local[0]: return msl, []
        guard = r'!' + re.escape(m.group(20)) if m.group(20) else re.escape(m.group(19))
    held = re.compile(r'int (\w+) = \w+;\s*if \(%s\)\s*\{\s*\1 = (\d+);\s*\}\s*int (\w+) = \1;\s*' % guard)   # a local that holds a constant in the first invocation
    block = re.compile(r'if \(%s\)\s*\{\s*((?:\(\*\w+\.\w+\)\._m0\[\d+\] = (?:\d+u|uint\(\w+\));\s*)+)\}\s*' % guard)
    plain = re.compile(r'int \w+ = [^;{}]*;\s*')   # arithmetic the compiler left between them
    at, out, stores, values = m.end(), msl[:m.end()], [], {}
    while True:
        h = held.match(msl, at)
        if h:
            values[h.group(3)] = int(h.group(2))
            out += msl[at:h.end()]
            at = h.end()
            continue
        b = block.match(msl, at)
        if b:
            for n, w, v in re.findall(r'\(\*\w+\.(\w+)\)\._m0\[(\d+)\] = (\d+u|uint\(\w+\));', b.group(1)):
                if v.endswith('u'): stores.append((n, int(w), int(v[:-1])))
                elif v[5:-1] in values: stores.append((n, int(w), values[v[5:-1]]))
                else: return msl, []
            at = b.end()
            continue
        p = plain.match(msl, at)
        if not p or held.match(msl, p.start()): break
        out += msl[at:p.end()]
        at = p.end()
    rest = msl[at:]
    if not stores or len(stores) > 4 or re.search(r'if \(%s\)' % guard, rest): return msl, []
    return out + rest, stores

# STS.U8 / STS.U16 (dmsh_shared_store8/16_<id>) as one byte or half-word store, as the console stores (no atomic), not a compare-and-swap loop
# (docs/reference/shaders.md#stage-memory-byte-stores-through-a-compare-and-swap-loop). The narrow pointer aliases the uint array, so the word loads keep their order
SHARED_STORE = re.compile(r'\nvoid (dmsh_shared_store(8|16)_\d+)\(([^)]*)\)\n\{')

SHARED_ALIAS = 'typedef ushort __attribute__((may_alias)) dm_ushort_alias;   // a half-word that may alias the shared memory\'s words (see shared_store_msl)\n'

def shared_store_msl(msl):
    if 'dmsh_shared_store' not in msl: return msl
    found = list(SHARED_STORE.finditer(msl))
    if not found: raise ValueError('dmsh_shared_store: no definition of the expected form')
    out, at = [], 0
    for m in found:
        params = split_params(m.group(3))
        scalar = [re.fullmatch(r'thread const u?int& (\w+)', p) for p in params[:2]]
        arr = re.fullmatch(r'(threadgroup|device) spvUnsafeArray<uint, \d+>& (\w+)', params[2]) if len(params) == 3 else None
        if not arr or not scalar[0] or not scalar[1]: raise ValueError('dmsh_shared_store: unexpected parameters ' + m.group(3)[:160])
        off, val = scalar[0].group(1), scalar[1].group(1)
        space, mem = arr.groups()
        store = (f'(({space} uchar*)&{mem}[0])[uint({off})] = uchar(uint({val}));' if m.group(2) == '8' else
                 f'(({space} dm_ushort_alias*)&{mem}[0])[uint({off}) >> 1u] = ushort(uint({val}));')   # a half-word store is aligned (Maxwell faults on a misaligned one)
        out.append(msl[at:m.end()] + '\n    ' + store + '\n')
        at = block_end(msl, m.end() - 1)
    out.append(msl[at:])
    src = ''.join(out)
    if any(m.group(2) == '16' for m in found):
        u = src.find('using namespace metal;\n')
        if u < 0: raise ValueError('dmsh_shared_store: no using namespace metal')
        u += len('using namespace metal;\n')
        src = src[:u] + SHARED_ALIAS + src[u:]
    return src

# The local size as [[max_total_threads_per_threadgroup]]: without Dynamic Caching (M1, M2) a pipeline's thread limit falls with the kernel's registers, and a larger
# dispatch runs nothing (docs/reference/shaders.md#compute-dispatches-past-the-pipelines-thread-limit). The line goes before the kernel's, which tools match at a line's start
MAX_THREADS = 1024   # Metal's limit for a compute threadgroup on every Apple GPU
ALWAYS_FITS = 384   # threads a pipeline allows at the most registers a thread can take (128) on g13 / g14

def max_threads(msl, local):
    if not local or len(local) != 3 or any(not isinstance(v, int) or v < 1 for v in local): return msl
    n = local[0] * local[1] * local[2]
    if n <= ALWAYS_FITS or n > MAX_THREADS or '[[max_total_threads_per_threadgroup(' in msl: return msl
    return re.sub(r'^(kernel void main0\()', '[[max_total_threads_per_threadgroup(%d)]]\n\\1' % n, msl, count=1, flags=re.M)

# Threadgroup memory past Metal's 32 KB from a device buffer at abi.TG_SLOT, one slice per workgroup of the grid (docs/reference/shaders.md#compute-shared-memory-past-32-kb);
# barriers then order device memory, threadgroup atomics become device ones. → (shader, bytes per workgroup; 0: unchanged)
TG_DECL = re.compile(r'^([ \t]*)threadgroup (spvUnsafeArray<(\w+), (\d+)>|\w+) (\w+)(?:\[(\d+)\])?;[ \t]*$', re.M)

TG_BYTES = {'uint': 4, 'int': 4, 'float': 4, 'uint2': 8, 'int2': 8, 'float2': 8, 'uint3': 16, 'int3': 16, 'float3': 16, 'uint4': 16, 'int4': 16, 'float4': 16,
            'half': 2, 'short': 2, 'ushort': 2, 'half2': 4, 'half4': 8, 'char': 1, 'uchar': 1}

def tg_spill(msl):
    m = re.search(r'\bkernel\s+void\s+main0\s*\(', msl)
    if not m: return msl, 0
    decls, total = [], 0
    for d in TG_DECL.finditer(msl, m.end()):
        elem = d.group(3) or d.group(2)
        n = int(d.group(4) or d.group(6) or 1)
        if elem not in TG_BYTES: raise ValueError(f'threadgroup array of {elem}')
        decls.append((d, total))
        total += (TG_BYTES[elem] * n + 15) & ~15
    if total <= abi.TG_MAX: return msl, 0
    depth = 1   # the parameter list's end: its attributes hold parentheses
    i = m.end()
    while depth:
        depth += {'(': 1, ')': -1}.get(msl[i], 0)
        i += 1
    sig_end = i - 1
    params = msl[m.end():sig_end]
    g = re.search(r'uint3 (\w+) \[\[threadgroup_position_in_grid\]\]', params)
    gid = g.group(1) if g else 'dm_tgid'
    add = ('' if g else ', uint3 dm_tgid [[threadgroup_position_in_grid]]') + f', uint3 dm_ntg [[threadgroups_per_grid]], device uchar* dm_tg [[buffer({abi.TG_SLOT})]]'
    out, at = [], 0
    for k, (d, off) in enumerate(decls):
        out.append(msl[at:d.start()])
        t = d.group(2) + (f'[{d.group(6)}]' if d.group(6) else '')
        if k == 0: out.append(f'{d.group(1)}ulong dm_tgbase = ((ulong({gid}.z) * dm_ntg.y + {gid}.y) * dm_ntg.x + {gid}.x) * {total}ul;\n')   # 64 bits: large grids of 48 KB groups pass 32
        if d.group(6): out.append(f'{d.group(1)}device {d.group(2)} (&{d.group(5)})[{d.group(6)}] = *(device {d.group(2)} (*)[{d.group(6)}])(dm_tg + dm_tgbase + {off});')
        else: out.append(f'{d.group(1)}device {t}& {d.group(5)} = *(device {t}*)(dm_tg + dm_tgbase + {off});')
        at = d.end()
    out.append(msl[at:])
    src = ''.join(out)
    e = src.index(params, m.start()) + len(params)
    src = src[:e] + add + src[e:]
    lines = src.split('\n')
    for n, ln in enumerate(lines):   # the array template's own threadgroup overloads stay
        if 'operator [] (size_t pos)' in ln: continue
        ln = re.sub(r'\bthreadgroup\b(?=\s)', 'device', ln)
        ln = re.sub(r'(?<!mem_device \| )mem_flags::mem_threadgroup\b', 'mem_flags::mem_device | mem_flags::mem_threadgroup', ln)
        lines[n] = ln
    return '\n'.join(lines), total

# spvGatherConstOffsets passes its offsets from a loop, which Metal takes only as constants (texels then differ between lanes, with no error): four gathers, or one
# at b for a 2x2 square b + (0..1, 0..1) (Metal's order: x (i0, j1), y (i1, j1), z (i1, j0), w (i0, j0))
GATHER_QUAD = {(0, 0): 'w', (1, 0): 'z', (0, 1): 'x', (1, 1): 'y'}

def gather_quad(off):
    """→ (the square's corner b as MSL, the swizzle that orders its texels), or None"""
    xy = []
    for o in off:
        m = re.fullmatch(r'int2\(\s*(-?\d+)\s*(?:,\s*(-?\d+)\s*)?\)', o)
        if not m: return None
        xy.append((int(m.group(1)), int(m.group(2) if m.group(2) is not None else m.group(1))))
    bx, by = min(x for x, _ in xy), min(y for _, y in xy)
    rel = [(x - bx, y - by) for x, y in xy]
    if sorted(rel) != sorted(GATHER_QUAD): return None
    return f'int2({bx}, {by})', ''.join(GATHER_QUAD[r] for r in rel)

# dmsh_raw_find written noinline: spirv-cross writes every function always_inline, so each access built its own copy of the search
# (docs/reference/shaders.md#raw-global-lookups-built-once-for-every-call). Workaround, cause open: not in task or mesh functions, where M2s lock up and M1 / M2 mesh
# compilers crash (docs/reference/shaders.md#a-noinline-call-in-a-task-or-mesh-function-locks-up-an-m2s-gpu)
RAW_FIND = re.compile(r'\nstatic (?:inline __attribute__\(\(always_inline\)\)|__attribute__\(\(noinline\)\))\n(\w+ dmsh_raw_find\w*\()')
RAW_INLINE_STAGES = ('ts', 'ms')
def raw_noinline(msl, stage='cs'):
    attr = 'inline __attribute__((always_inline))' if stage in RAW_INLINE_STAGES else '__attribute__((noinline))'
    return RAW_FIND.sub(lambda m: '\nstatic ' + attr + '\n' + m.group(1), msl) if 'dmsh_raw_find' in msl else msl

# A shuffle of a literal is the literal. Workaround, cause in Apple's M2 mesh compiler open: it crashes on some shuffles of a constant
# (docs/reference/shaders.md#a-shuffle-of-a-constant-crashes-apples-m2-mesh-compiler)
SHUFFLE_LITERAL = re.compile(r'\bspvSubgroupShuffle\(\s*(-?(?:0x[0-9a-fA-F]+|\d[\d.]*(?:[eE][+-]?\d+)?)[uUfFhH]?)\s*,\s*(?:\w+|\w+\(\w+\))\s*\)')
def shuffle_literal(msl):
    def lit(m): return '(' + m.group(1) + ')' if m.group(1).startswith('-') else m.group(1)
    return SHUFFLE_LITERAL.sub(lit, msl) if 'spvSubgroupShuffle(' in msl else msl

GATHER4 = 'template<typename T> static inline __attribute__((always_inline)) vec<T, 4> dm_gather4(T a, T b, T c, T d) { return vec<T, 4>(a, b, c, d); }\n'

def gather_offsets(msl):
    if 'spvGather' not in msl: return msl
    if not re.search(r'\bspvGather(Compare)?ConstOffsets\(spvDesc|= spvGather(Compare)?ConstOffsets\(', msl): return msl
    arrays = {}
    for m in re.finditer(r'\bconstant spvUnsafeArray<int2, 4> (\w+) = spvUnsafeArray<int2, 4>\(\{ (.*?) \}\);', msl):
        off = re.findall(r'int2\([^()]*\)', m.group(2))
        if len(off) != 4: raise ValueError('gather offsets: unexpected array ' + m.group(0)[:120])
        arrays[m.group(1)] = off
    out, i = [], 0
    for m in re.finditer(r'\bspvGather(Compare)?ConstOffsets\((?=[^)]*?\w)', msl):
        if m.start() < i or msl[m.end():m.end() + 6] == 'const ': continue   # the wrappers' own definitions
        depth, args, cur, j = 1, [], '', m.end()
        while depth:
            ch = msl[j]
            j += 1
            depth += {'(': 1, ')': -1}.get(ch, 0)
            if depth == 0 or (ch == ',' and depth == 1):
                args.append(cur.strip())
                cur = ''
            else:
                cur += ch
        cmp = bool(m.group(1))
        base = 3 if cmp else 4
        if len(args) <= base or args[2] not in arrays: raise ValueError('gather offsets: unexpected call ' + msl[m.start():j][:160])
        tex, smp, off, rest = args[0], args[1], arrays[args[2]], ', '.join(args[base:])
        tail = '' if cmp else ', ' + args[3]
        fn = 'gather_compare' if cmp else 'gather'
        quad = gather_quad(off)
        if quad: out += [msl[i:m.start()], f'({tex}.{fn}({smp}, {rest}, {quad[0]}{tail}).{quad[1]})']
        else: out += [msl[i:m.start()], 'dm_gather4(' + ', '.join(f'{tex}.{fn}({smp}, {rest}, {o}{tail}).w' for o in off) + ')']
        i = j
    out.append(msl[i:])
    msl = ''.join(out)
    if 'dm_gather4(' not in msl: return msl
    k = msl.index('\n', msl.index('using namespace metal;')) + 1
    return msl[:k] + GATHER4 + msl[k:]

# Border colours Metal lacks: a filtered sample is linear in the border colour, so with the sampler's Metal colour Bn and its twin's Bw, r = s_n + (s_w - s_n)·k,
# k = (B - Bn) / (Bw - Bn) (docs/reference/rendering.md#border-colours-metal-has-no-sampler-state-for)
BORDER_HELPERS = f'''
struct dm_border_t {{ uint mask; float4 k[{abi.BORDER_SAMPLERS}]; }};
static inline __attribute__((always_inline)) float4 dm_bmix(float4 r, float4 w, float4 k) {{ return r + (w - r) * k; }}
static inline __attribute__((always_inline)) float dm_bmix(float r, float w, float4 k) {{ return r + (w - r) * k.x; }}
template<typename V, typename T, typename... A> static inline __attribute__((always_inline)) V dm_bs(V r, constant dm_border_t* b, uint i, T t, sampler w, A... a)
{{
    if (DM_BORDER_LIVE && (b->mask >> i & 1u)) r = dm_bmix(r, t.sample(w, a...), b->k[i]);
    return r;
}}
'''

# abi.BORDER_FC (true: off) compiles the twins out, as a uniform branch still keeps their registers live; unspecialized, every sample corrects as its bit says
BORDER_LIVE_FC = (f'constant bool dm_border_off [[function_constant({abi.BORDER_FC})]];\n'
                  'constant bool dm_border_live = !is_function_constant_defined(dm_border_off) || !dm_border_off;\n#define DM_BORDER_LIVE dm_border_live\n')

def border_emu(msl):
    """Float textures only: an integer one is not filtered, and keeps Metal's nearest border colour"""
    body = re.search(r'struct spvDescriptorSetBuffer0\s*\{(.*?)\};', msl, re.S)
    if not body: return msl
    # members with attributes after their id too (see reflect)
    members = [(m.group(1).strip(), m.group(2), int(m.group(3))) for m in re.finditer(r'^\s*(.+?)\s+(\w+)\s*\[\[id\((\d+)\)(?:\s*,\s*\w+\(\d+\))*\]\];', body.group(1), re.M)]
    samplers = [n for ty, n, _ in members if ty == 'sampler']
    ftex = {n for ty, n, _ in members if re.fullmatch(r'(texture|depth)\w*<float(,.*)?>', ty)}
    call = re.compile(r'spvDescriptorSet0\.(\w+)\.sample\(spvDescriptorSet0\.(\w+), ')
    used = {m.group(2) for m in call.finditer(msl) if m.group(1) in ftex}
    bit = {n: i for i, n in enumerate([n for n in samplers if n in used][:abi.BORDER_SAMPLERS])}
    if not bit: return msl
    def width(ty):
        a = re.fullmatch(r'array<.*,\s*(\d+)>', ty)
        return int(a.group(1)) if a else 1
    nid = max(aid + width(ty) for ty, _, aid in members)   # the new members go after every entry spirv-cross gave out
    add = ''.join(f'    sampler {n}{abi.BORDER_TWIN} [[id({nid + i})]];\n' for n, i in bit.items()) + f'    constant dm_border_t* {abi.BORDER_MEMBER} [[id({nid + len(bit)})]];\n'
    end = body.end(1)
    msl = msl[:end] + ('' if msl[end - 1] == '\n' else '\n') + add + msl[end:]
    for m in reversed([m for m in call.finditer(msl) if m.group(2) in bit and m.group(1) in ftex]):   # the last first: a nested sample is rewritten before the outer one copies it
        depth, i = 1, m.end()
        while depth:
            depth += {'(': 1, ')': -1}.get(msl[i], 0)
            i += 1
        args = msl[m.end():i - 1]
        twin = f'spvDescriptorSet0.{m.group(2)}{abi.BORDER_TWIN}'
        msl = msl[:m.start()] + f'dm_bs({msl[m.start():i]}, spvDescriptorSet0.{abi.BORDER_MEMBER}, {bit[m.group(2)]}u, spvDescriptorSet0.{m.group(1)}, {twin}, {args})' + msl[i:]
    return msl.replace('using namespace metal;\n', 'using namespace metal;\n' + BORDER_LIVE_FC + BORDER_HELPERS, 1)

# Apple's GPU stores a depth up to an ulp plus a fraction of fwidth(z) below the [[position]].z its fragment shader reads; NVIDIA stores that z. Where gl_FragCoord.z
# and a texture value meet in one comparison, z takes the value when it lies in [z - ulp(z) - 2^-20 fwidth(z), z)
# (docs/reference/shaders.md#gl_fragcoordz-against-the-depth-stored-for-its-own-surface)
ZSNAP = ('static inline __attribute__((always_inline)) float dm_zsnap(float z, float d, float zw)   // see frag_z_snap\n'
         '{\n    float lo = as_type<float>(as_type<int>(z) - 1) - zw * 0x1p-20f;\n    return (z > 0.0f && d < z && d >= lo) ? d : z;\n}\n')

ZSNAP_WIDTH = 'dm_zw'

def frag_z_snap(msl):
    """→ (msl, the number of reads)"""
    if 'gl_FragCoord.z' not in msl: return msl, 0
    shared = dmsh_side.body(msl)
    try: e = None if shared else dmsh_side.entry(msl, 'main0')
    except ValueError: return msl, 0
    if shared:
        head, end = shared[1:]
        params = re.findall(r'[^,]+', msl[shared[0]:head])
    elif e: head, end, params = e['head'], e['end'], e['params']
    else: return msl, 0
    if not any(re.search(r'\bfloat4 gl_FragCoord( \[\[position\]\])?\)?$', p.strip()) for p in params): return msl, 0
    first = msl.find('{', head) + 1
    lines = msl[first:end - 1].split('\n')
    zr, xr, scope, line_of = {}, {}, {}, {}
    stack, blocks, pairs = [0], 0, {}
    for i, ln in enumerate(lines):
        m = re.match(r'\s*(?:const )?(float|bool|int|uint)\s+(_\d+) = (.+);$', ln)
        if m:
            ty, v, ex = m.groups()
            refs = re.findall(r'\b_\d+\b', ex)
            z = set().union(*(zr.get(r, set()) for r in refs))
            x = set().union(*(xr.get(r, set()) for r in refs))
            if ex == 'gl_FragCoord.z': z = {v}
            elif ty == 'float' and re.search(r'\.(sample|read)\(', ex) and not z: x = {v}   # a scalar read from a texture; not sample_compare or gather
            zr[v], xr[v], scope[v], line_of[v] = z, x, tuple(stack), i
            c = re.fullmatch(r'(_\d+) (?:<=|>=|<|>|==|!=) (_\d+)', ex) if ty == 'bool' else None
            if c:
                a, b = c.groups()
                for zs, xs in ((a, b), (b, a)):
                    if zr.get(zs) and xr.get(xs) and not xr.get(zs) and not zr.get(xs):
                        for zv in zr[zs]: pairs.setdefault(zv, set()).update(xs_ for xs_ in xr[xs])
        for ch in re.findall(r'[{}]', ln):
            if ch == '{':
                blocks += 1
                stack.append(blocks)
            elif len(stack) > 1: stack.pop()
    n = 0
    for zv in sorted(pairs, key=lambda v: line_of[v]):
        xs = [x for x in sorted(pairs[zv], key=lambda v: line_of[v]) if line_of[x] < line_of[zv] and scope[zv][:len(scope[x])] == scope[x]]   # declared before the read, in a block it sees
        if not xs: continue
        expr = 'gl_FragCoord.z'
        for x in xs: expr = f'dm_zsnap({expr}, {x}, {ZSNAP_WIDTH})'
        i = line_of[zv]
        lines[i] = lines[i].replace(f'{zv} = gl_FragCoord.z;', f'{zv} = {expr};')
        n += 1
    if not n: return msl, 0
    body = f'\n    float {ZSNAP_WIDTH} = fwidth(gl_FragCoord.z);' + '\n'.join(lines)   # in uniform control flow, before anything the body does
    msl = msl[:first] + body + msl[end - 1:]
    return msl.replace('using namespace metal;\n', 'using namespace metal;\n\n' + ZSNAP, 1), n


# NVIDIA's blender has "zero times anything is zero" on (NVN sets SET_BLEND_FLOAT_OPTION); Apple's stores 0 x NaN. Where the host's word at DMSH_BLEND_BUFFER says
# an output's source weight is zero, a non-finite rgb is returned as 0 (docs/reference/shaders.md#a-nan-colour-under-a-zero-blend-weight). A call, which fs_uint
# reads past; it also rounds an unblended normalized target's colour as NVIDIA does (round, DMSH_BLEND_ROUND_BITS)
BLEND_ZERO = '''struct dm_blend_state { uint zero; uint round; };   // see dmsh_rewrite.py blend_zero
static inline __attribute__((always_inline)) float4 dm_bz(float4 c, uint m)
{
    m &= %du;
    if (!((m == 1u && c.w == 0.0f) || (m == 2u && c.w == 1.0f))) return c;
    return float4(select(c.xyz, float3(0.0f), (as_type<uint3>(c.xyz) & uint3(0x7f800000u)) == uint3(0x7f800000u)), c.w);   // NaN or Inf by its bits: fast math may assume there is none
}
static inline __attribute__((always_inline)) float4 dm_rop(float4 c, uint m)
{
    m &= %du;
    if (m == 0u) return c;
    float4 s = m == %du ? float4(255.0f) : (m == %du ? float4(65535.0f) : float4(1023.0f, 1023.0f, 1023.0f, 3.0f));
    return rint(precise::clamp(c, 0.0f, 1.0f) * s) / s;   // precise: NaN goes to 0, as both store it; the product is float32's, rint rounds it to nearest even
}
constant bool dm_blend_off [[function_constant(%d)]];   // abi.BLEND_OFF_FC: the pipeline's blend words are both 0 (nvn_mtl.m fn_off), so nothing to fix up
static inline __attribute__((always_inline)) void dm_blend0(thread main0_out& dm_o, constant dm_blend_state& dm_s)
{
    if (is_function_constant_defined(dm_blend_off) && dm_blend_off) return;
    uint m = dm_s.zero, r = dm_s.round;
    if ((m | r) == 0u) return;
%s}
'''
BLEND_ZERO_PARAM = f'constant dm_blend_state& dm_blend [[buffer({abi.BLEND_BUFFER})]]'


def blend_zero(msl):
    """→ (msl, returns guarded). Unchanged (0) when main0 has no float4 colour output, an unknown shape, a shared body, or a return not on a line of its own"""
    st = re.search(r'struct main0_out\s*\{(.*?)\};', msl, re.S)
    cols = [(nm, int(c)) for ty, nm, c in re.findall(r'^\s*(\w+)\s+(\w+)\s*\[\[color\((\d+)\)\]\];', st.group(1), re.M) if ty == 'float4'] if st else []   # not index(1): a second blend source
    if not cols or 'dm_blend0(' in msl or f'[[buffer({abi.BLEND_BUFFER})]]' in msl or dmsh_side.body(msl): return msl, 0
    try: e = dmsh_side.entry(msl, 'main0')
    except ValueError: return msl, 0
    if not e or e['ret'] != 'main0_out': return msl, 0
    body = msl[e['head']:e['end']]
    n = len(re.findall(r'\breturn\b', body))
    body, k = re.subn(r'^([ \t]*)return out;$', lambda m: f'{m.group(1)}dm_blend0(out, dm_blend);\n{m.group(1)}return out;', body, flags=re.M)
    if not k or k != n: return msl, 0
    bits, rb = abi.BLEND_ZERO_BITS, abi.BLEND_ROUND_BITS
    helper = BLEND_ZERO % ((1 << bits) - 1, (1 << rb) - 1, abi.ROUND_8, abi.ROUND_16, abi.BLEND_OFF_FC,
                           ''.join(f'    dm_o.{nm} = dm_rop(dm_bz(dm_o.{nm}, m >> {bits * c}u), r >> {rb * c}u);\n' for nm, c in cols))
    return msl[:e['start']] + helper + '\n' + msl[e['start']:e['head'] - 1] + (', ' if e['params'] else '') + BLEND_ZERO_PARAM + ')' + body + msl[e['end']:], k
