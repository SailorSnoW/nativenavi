#!/usr/bin/env python3
# agl's NDSR upscaler network recognised in its translated MSL, its HMMA core rewritten on simdgroup matrices, in two passes by a function constant
# (docs/reference/shaders.md#agls-ndsr-network-on-simdgroup-matrices-in-two-passes)
import re
from typing import Any
import dmsh_abi as abi, dmsh_rewrite

# SIMD-group sg runs 8 patches (bx), each three blocks of two groups g: H = W1_g . X, Y += H . W2_g (X the window for block 0, ReLU(Y) after; block 2 keeps n 0-7).
# Weights: 12 16x16-half matrices in PTX A-fragment order (lane L's words at 512 j + 16 L), W1_g = 4b + 2g, W2_g = 4b + 2g + 1
THREADS = 128
PATCHES = 8
BLOCKS = 3
WIN_ROW = 144
MATRIX = 512
CALLS = 176
FINAL_WORDS = PATCHES
ARGS = ['lane', 'blk', 'a0', 'a1', 'a2', 'a3', 'b0', 'b1', 'acc0', 'acc1', 'fl', 'aaddr', 'ao0', 'ao1', 'ao2', 'ao3', 'astr', 'd1']   # as dmsh_rewrite.HMMA_ARGS


class Refused(Exception):
    pass


# spirv-cross's straight-line statements, one a line
_TOKEN = re.compile(r'\s*(?:(?P<num>0x[0-9a-fA-F]+u?|\d+\.\d*(?:[eE][-+]?\d+)?[fh]?|\d+[eE][-+]?\d+[fh]?|\d+u?)|(?P<id>[A-Za-z_]\w*)|(?P<op><<|>>|<=|>=|==|!=|&&|\|\||::|[-+*/%&|^!~<>?:()\[\].,]))')


def tokens(text):
    out, at = [], 0
    while at < len(text):
        m = _TOKEN.match(text, at)
        if not m or m.end() == at:
            if text[at:].strip() == '': break
            raise Refused('unreadable expression: ' + text[at:at + 40])
        at = m.end()
        kind = m.lastgroup or 'op'
        out.append((kind, m.group(kind)))
    return out


class Expr:
    PREC = [('||',), ('&&',), ('|',), ('^',), ('&',), ('==', '!='), ('<', '>', '<=', '>='), ('<<', '>>'), ('+', '-'), ('*', '/', '%')]

    def __init__(self, text):
        self.t, self.i = tokens(text), 0

    def peek(self, k=0): return self.t[self.i + k][1] if self.i + k < len(self.t) else None

    def take(self, want=None):
        if self.i >= len(self.t): raise Refused('expression ends early')
        v = self.t[self.i][1]
        if want is not None and v != want: raise Refused(f'expected {want}, found {v}')
        self.i += 1
        return v

    def parse(self):
        e = self.cond()
        if self.i != len(self.t): raise Refused('trailing tokens: ' + ' '.join(v for _, v in self.t[self.i:self.i + 6]))
        return e

    def cond(self):
        c = self.binary(0)
        if self.peek() == '?':
            self.take()
            a = self.cond()
            self.take(':')
            b = self.cond()
            return ('sel', c, a, b)
        return c

    def binary(self, level):
        if level == len(self.PREC): return self.unary()
        e = self.binary(level + 1)
        while self.peek() in self.PREC[level]:
            op = self.take()
            e = ('bin', op, e, self.binary(level + 1))
        return e

    def unary(self):
        if self.peek() in ('-', '!', '~', '+'):
            op = self.take()
            return ('un', op, self.unary())
        return self.postfix(self.primary())

    def targs(self):
        depth, out = 0, []
        while True:
            v = self.take()
            depth += {'<': 1, '>': -1}.get(v, 0)
            if depth == 0: return ''.join(out)
            if not (depth == 1 and v == '<'): out.append(v)

    def primary(self):
        kind, v = self.t[self.i] if self.i < len(self.t) else (None, None)
        if v == '(':
            self.take()
            e = self.cond()
            self.take(')')
            return e
        if kind == 'num':
            self.take()
            return ('num', v)
        if kind == 'id':
            self.take()
            name = v
            while self.peek() == '::':
                self.take()
                name += '::' + self.take()
            ta = None
            if self.peek() == '<' and name in ('as_type', 'spvMulExtended'): ta = self.targs()
            if self.peek() == '(':
                self.take()
                args = []
                if self.peek() != ')':
                    args.append(self.cond())
                    while self.peek() == ',':
                        self.take()
                        args.append(self.cond())
                self.take(')')
                return ('call', name, ta, tuple(args))
            return ('name', name)
        raise Refused(f'unexpected token {v}')

    def postfix(self, e):
        while self.peek() in ('.', '['):
            if self.take() == '.': e = ('member', e, self.take())
            else:
                i = self.cond()
                self.take(']')
                e = ('index', e, i)
        return e


def parse(text) -> Any: return Expr(text).parse()


DECL = re.compile(r'^    (?:const )?([A-Za-z_][\w:]*(?:<[^;=]*>)?)\s+(_\w+)(?:\s*=\s*(.+))?;$')
U32 = 0xffffffff


def s32(v):
    v &= U32
    return v - (1 << 32) if v >> 31 else v


class Threads:
    """Per-thread values of one 128-thread threadgroup; one that depends on anything but the thread raises Refused"""
    INT = {'int', 'uint', 'bool', 'short', 'ushort'}

    def __init__(self, defs, types):
        self.defs, self.types, self.memo = defs, types, {}

    def var(self, n):
        if n in self.memo: return self.memo[n]
        if n not in self.defs: raise Refused(f'{n} has no definition the analysis reads')
        self.memo[n] = None   # a cycle
        v = self.conv(self.types[n], self.ev(parse(self.defs[n])))
        self.memo[n] = v
        return v

    @staticmethod
    def conv(ty, v):   # int <-> uint keep the bits; float holds bits only through as_type
        t, xs = v
        if ty in ('int', 'uint', 'short', 'ushort'):
            if t == 'float': raise Refused('a float converted to an integer')
            if t == 'bool': return (ty, tuple(int(x) for x in xs))
            return (ty, tuple(x & (0xffff if ty in ('short', 'ushort') else U32) for x in xs))
        if ty == 'bool': return ('bool', tuple(int(x != 0) for x in xs))
        if ty == 'float' and t == 'float': return v
        if ty == 'float': raise Refused('a float value conversion')
        raise Refused(f'a value of type {ty}')

    def ev(self, e):
        k = e[0]
        if k == 'num':
            s = e[1]
            if '.' in s or s[-1] in 'fh' or ('e' in s and not s.startswith('0x')): raise Refused('a float literal in an address')
            u = s.endswith('u')
            v = int(s.rstrip('u'), 0)
            return ('uint' if u else 'int', (v & U32,) * THREADS)
        if k == 'name':
            if e[1] in self.defs: return self.var(e[1])
            if e[1] == 'gl_SubgroupInvocationID': return ('uint', tuple(x % 32 for x in range(THREADS)))   # a 1D threadgroup's SIMD-groups are 32 consecutive threads
            raise Refused(f'{e[1]} is not a thread value')
        if k == 'member':
            base = e[1]
            if base == ('name', 'gl_LocalInvocationID'): return ('uint', tuple(range(THREADS)) if e[2] == 'x' else (0,) * THREADS)
            if base[0] == 'call' and base[1] == 'spvMulExtended' and e[2] in ('_m0', '_m1') and len(base[3]) == 2:   # OpUMulExtended / OpSMulExtended: the low or the high word
                signed = (base[2] or '').replace(' ', '').endswith(',int')
                (_, a), (_, b) = self.ev(base[3][0]), self.ev(base[3][1])
                full = tuple((s32(x) * s32(y) if signed else x * y) for x, y in zip(a, b))
                return ('int' if signed else 'uint', tuple((v >> (32 if e[2] == '_m1' else 0)) & U32 for v in full))
            raise Refused('a member read in an address: ' + e[2])
        if k == 'call':
            fn, ta, args = e[1], e[2], e[3]
            if fn == 'as_type':
                t, xs = self.ev(args[0])
                if ta not in ('int', 'uint', 'float'): raise Refused(f'as_type<{ta}>')
                return (ta, xs)
            if fn in ('int', 'uint', 'bool') and len(args) == 1: return self.conv(fn, self.ev(args[0]))
            raise Refused(f'a call of {fn} in an address')
        if k == 'un':
            t, xs = self.ev(e[2])
            if t == 'float': raise Refused('float arithmetic in an address')
            if e[1] == '-': return (t, tuple(-x & U32 for x in xs))
            if e[1] == '~': return (t, tuple(~x & U32 for x in xs))
            if e[1] == '!': return ('bool', tuple(int(not x) for x in xs))
            return (t, xs)
        if k == 'bin':
            (ta, a), (tb, b) = self.ev(e[2]), self.ev(e[3])
            if 'float' in (ta, tb): raise Refused('float arithmetic in an address')
            op = e[1]
            t = 'uint' if 'uint' in (ta, tb) else 'int'
            sg = t == 'int'
            if op in ('==', '!=', '<', '>', '<=', '>='):
                f = {'==': lambda x, y: x == y, '!=': lambda x, y: x != y, '<': lambda x, y: x < y, '>': lambda x, y: x > y, '<=': lambda x, y: x <= y, '>=': lambda x, y: x >= y}[op]
                return ('bool', tuple(int(f(s32(x) if sg else x, s32(y) if sg else y)) for x, y in zip(a, b)))
            if op in ('&&', '||'): return ('bool', tuple(int((x and y) if op == '&&' else (x or y)) for x, y in zip(a, b)))
            if op == '<<': return (ta, tuple((x << (y & 31)) & U32 for x, y in zip(a, b)))
            if op == '>>': return (ta, tuple(((s32(x) if ta == 'int' else x) >> (y & 31)) & U32 for x, y in zip(a, b)))
            if op in ('/', '%'):
                if any(y == 0 for y in b): raise Refused('a division by zero')
                def div(x, y, rem):
                    if sg:
                        x, y = s32(x), s32(y)
                        q = abs(x) // abs(y) * (1 if (x < 0) == (y < 0) else -1)
                    else:
                        q = x // y
                    return (x - q * y if rem else q) & U32
                return (t, tuple(div(x, y, op == '%') for x, y in zip(a, b)))
            f = {'+': lambda x, y: x + y, '-': lambda x, y: x - y, '*': lambda x, y: x * y, '&': lambda x, y: x & y, '|': lambda x, y: x | y, '^': lambda x, y: x ^ y}[op]
            return (t, tuple(f(x, y) & U32 for x, y in zip(a, b)))
        if k == 'sel':
            c, (ta, a), (tb, b) = self.ev(e[1])[1], self.ev(e[2]), self.ev(e[3])
            return (ta, tuple(x if s else y for s, x, y in zip(c, a, b)))
        raise Refused(f'an expression of kind {k} in an address')


KERNEL = re.compile(r'\nkernel void main0\(([^\n]*)\)\n\{\n')
CALL = re.compile(r'^    int (_\d+) = dmsh_hmma16816_f16\((.*)\);$')
BARRIER = '    threadgroup_barrier(mem_flags::mem_threadgroup);'
WLOAD = re.compile(r'^\*\(const device packed_uint([24])\*\)&(.+)\._m0\[int\((_\w+)\)\]$')
ASSIGN = re.compile(r'(?<![\w.])(_\w+)\s*(?:<<|>>|[-+*/%&|^])?=(?!=)')
NAME = re.compile(r'\b_\w+\b')


class Graph:
    """Interned terms: ('Z',), ('W', j, w) weight j word w, ('S', off) window word at base + off bytes, ('R', x) ReLU of both halves,
    ('H', a0..a3, b0, b1, c0, c1) one HMMA.16816, ('D', h, i) its D word i"""

    def __init__(self):
        self.ids, self.terms = {}, []

    def __call__(self, t):
        i = self.ids.get(t)
        if i is None:
            i = self.ids[t] = len(self.terms)
            self.terms.append(t)
        return i

    def show(self, i, depth=3):
        t = self.terms[i]
        if t[0] in ('Z', 'W', 'S'): return t[0] + ''.join(f'.{x}' for x in t[1:])
        if depth == 0: return '…'
        if t[0] == 'R': return f'relu({self.show(t[1], depth)})'
        if t[0] == 'D': return f'{self.show(t[1], depth - 1)}.d{t[2]}'
        return 'H(' + ' '.join(self.show(x, depth) for x in t[1:]) + ')'


def window_base(x):
    sg, lane = x // 32, x % 32
    return WIN_ROW * (6 * sg + lane // 4) + 4 * (lane % 4)


def expected(T):
    """d0 of each patch's last D"""
    Z = T(('Z',))
    W = lambda j, w: T(('W', j, w))   # noqa: E731
    D = lambda h, i: T(('D', h, i))   # noqa: E731
    H = lambda A, B, C: T(('H',) + tuple(A) + tuple(B) + tuple(C))   # noqa: E731
    out = []
    for p in range(PATCHES):
        X = [(T(('S', 12 * p + 1152 * nb)), T(('S', 12 * p + 1152 * nb + 16))) for nb in (0, 1)]
        for b in range(BLOCKS):
            Y = []
            for nh in ((0, 1) if b < BLOCKS - 1 else (0,)):
                c = (Z, Z)
                for g in (0, 1):
                    G = [H((W(4 * b + 2 * g, w) for w in range(4)), X[nb], (Z, Z)) for nb in (0, 1)]
                    y = H((D(G[0], 0), D(G[0], 1), D(G[1], 0), D(G[1], 1)), (W(4 * b + 2 * g + 1, 2 * nh), W(4 * b + 2 * g + 1, 2 * nh + 1)), c)
                    c = (D(y, 0), D(y, 1))
                Y.append(c)
            if b < BLOCKS - 1: X = [(T(('R', Y[0][nb])), T(('R', Y[1][nb]))) for nb in (0, 1)]   # D word (m g / g+8, n 2t) as B word (k 2t / 2t+8, n g): the transposed tile
        out.append(Y[0][0])
    return out


def relu_source(defs, e):
    """spirv-cross's ReLU of a D word's two halves (+-0 to +0) → x, else None"""
    d = lambda n: parse(defs[n]) if n in defs else None   # noqa: E731
    if not (e[0] == 'call' and e[1] == 'as_type' and e[2] == 'uint' and len(e[3]) == 1): return None
    h = e[3][0]
    if not (h[0] == 'call' and h[1] == 'half2' and len(h[3]) == 1 and h[3][0][0] == 'call' and h[3][0][1] == 'float2' and len(h[3][0][3]) == 2): return None
    src = None
    for half, arg in zip('xy', h[3][0][3]):
        if arg[0] != 'name': return None
        s = d(arg[1])
        if not (s and s[0] == 'sel' and s[1][0] == 'name' and s[2] == ('num', '0.0') and s[3][0] == 'name'): return None
        c, m = d(s[1][1]), d(s[3][1])
        if not (c and c[0] == 'bin' and c[1] == '==' and c[3] == ('num', '0') and c[2][0] == 'name'): return None
        sh = d(c[2][1])
        if not (sh and sh[0] == 'bin' and sh[1] == '<<' and sh[3] == ('num', '1') and sh[2][0] == 'call' and sh[2][1] == 'as_type' and sh[2][2] == 'int' and sh[2][3][0][0] == 'name'): return None
        v = sh[2][3][0][1]
        if not (m and m[0] == 'call' and m[1] == 'precise::max' and m[3] == (('name', v), ('num', '0.0'))): return None
        f = d(v)
        if not (f and f[0] == 'member' and f[2] == half and f[1][0] == 'call' and f[1][1] == 'float2' and len(f[1][3]) == 1): return None
        a = f[1][3][0]
        if not (a[0] == 'call' and a[1] == 'as_type' and a[2] == 'half2' and a[3][0][0] == 'call' and a[3][0][1] == 'uint' and a[3][0][3][0][0] == 'name'): return None
        x = a[3][0][3][0][1]
        if src not in (None, x): return None
        src = x
    return src


def analyse(msl, local):
    """None when the kernel calls no HMMA; raises Refused with the reason"""
    k = KERNEL.search(msl)
    if not k or 'dmsh_hmma16816_f16(' not in msl[k.end():]: return None
    open_at = k.end() - 2
    body_end = dmsh_rewrite.block_end(msl, open_at)
    lines = msl[k.end():body_end].split('\n')
    calls = [i for i, ln in enumerate(lines) if CALL.match(ln)]
    if not calls: return None
    if list(local or ()) != [THREADS, 1, 1]: raise Refused(f'local size {local}, not {THREADS}x1x1')
    if len(calls) != CALLS: raise Refused(f'{len(calls)} HMMA calls, not {CALLS}')
    if sum('dmsh_hmma16816_f16(' in ln for ln in lines) != CALLS: raise Refused('an HMMA call that is not a statement of its own')
    core0 = next((i for i, ln in enumerate(lines) if ln == BARRIER), None)
    if core0 is None or core0 > calls[0]: raise Refused('no threadgroup barrier before the first HMMA call')
    core1 = calls[-1]
    defs, types, writes = {}, {}, {}
    for i, ln in enumerate(lines):
        for m in ASSIGN.finditer(ln): writes[m.group(1)] = writes.get(m.group(1), 0) + 1
        if i > core1: continue
        m = DECL.match(ln)
        if m and m.group(3) is not None:
            defs[m.group(2)] = m.group(3)
            types[m.group(2)] = m.group(1)
        elif m: types[m.group(2)] = m.group(1)
    for i in range(core0 + 1, core1 + 1):   # straight-line declarations, barriers and the calls, nothing that writes memory
        ln = lines[i]
        if i in calls or ln == BARRIER: continue
        m = DECL.match(ln)
        if not m or '{' in ln or '}' in ln: raise Refused('a statement in the core the pass does not read: ' + ln.strip()[:80])
        if m.group(3) and re.search(r'\b(?:atomic_\w+|write|dmsh_\w+|simdgroup_\w+|threadgroup_barrier)\s*\(', m.group(3)): raise Refused('a call with effects in the core: ' + ln.strip()[:80])
    th = Threads(defs, types)
    once = lambda n: writes.get(n, 0) <= 1   # noqa: E731  (a declaration with its value counts once)
    T = Graph()
    Z = T(('Z',))
    res, outs = {}, {}
    parsed = []
    matches = [m for c in calls if (m := CALL.match(lines[c]))]
    for m in matches:
        args = dmsh_rewrite.split_params(m.group(2))
        if len(args) < len(ARGS) + 5 or args[len(ARGS) + 1:len(ARGS) + 4] != ['dm_acc', 'dm_am', 'dm_bm']: raise Refused('an HMMA call without hmma_msl\'s arguments')
        a = dict(zip(ARGS, args))
        if not re.fullmatch(r'-?\d+', args[len(ARGS) + 4]): raise Refused('an HMMA call whose B mode is not a literal')
        res[m.group(1)] = len(parsed)
        outs[a['d1']] = len(parsed)
        parsed.append((m.group(1), a, int(args[len(ARGS) + 4])))
    weight_buf, window = set(), set()
    words = {}

    def term(e, n, depth):
        if e == ('num', '0'): return Z
        if e[0] == 'name': return word(e[1], depth + 1)
        if (x := relu_source(defs, e)) is not None: return T(('R', word(x, depth + 1)))
        if e[0] == 'call' and e[1] == 'as_type' and e[2] in ('int', 'uint', 'float') and len(e[3]) == 1:
            x = e[3][0]
            if x[0] == 'index' and x[1][0] == 'name':   # the window
                arr = x[1][1]
                if not re.search(r'^    threadgroup spvUnsafeArray<uint, \d+> %s;$' % re.escape(arr), msl[k.end():body_end], re.M): raise Refused(f'{n}: a read of {arr}, not a threadgroup array')
                window.add(arr)
                ix = th.ev(x[2])[1]
                off = {4 * ix[i] - window_base(i) for i in range(THREADS)}
                if len(off) != 1: raise Refused(f'{n}: a window word at no fixed offset from the lane\'s base')
                return T(('S', off.pop()))
            if x[0] == 'member' and x[1][0] == 'name' and x[2] in 'xyzw' and x[1][1] in defs and (wl := WLOAD.match(defs[x[1][1]])):
                weight_buf.add(wl.group(2))
                comp = 'xyzw'.index(x[2])
                if comp >= int(wl.group(1)): raise Refused(f'{n}: a component past its load')
                ix = th.var(wl.group(3))[1]
                jw = set()
                for i in range(THREADS):
                    b = 4 * (ix[i] + comp)
                    j = b // MATRIX
                    r = b - MATRIX * j - 16 * (i % 32)
                    jw.add((j, r // 4) if 0 <= r < 16 and r % 4 == 0 else None)
                if len(jw) != 1 or None in jw: raise Refused(f'{n}: a weight word not in the PTX fragment order')
                return T(('W',) + jw.pop())
            return term(x, n, depth + 1)
        if e[0] == 'call' and e[1] in ('int', 'uint') and e[2] is None and len(e[3]) == 1 and e[3][0][0] == 'name' and types.get(e[3][0][1]) in ('int', 'uint'): return word(e[3][0][1], depth + 1)
        raise Refused(f'{n}: an operand of unknown origin ({defs.get(n, "")[:60]})')

    def word(n, depth=0):
        if n in words: return words[n]
        if depth > 64: raise Refused('a copy chain too long')
        if not once(n): raise Refused(f'{n} is written more than once')
        if n in res or n in outs:
            j = res.get(n, outs.get(n))
            h = D_of.get(j)
            if h is None: raise Refused(f'{n}: an HMMA result read before its call')
            words[n] = h[0 if n in res else 1]
            return words[n]
        if n not in defs: raise Refused(f'{n}: an operand with no definition')
        t = term(parse(defs[n]), n, depth)
        words[n] = t
        return t

    D_of, acc, bm, prev_a = {}, {}, {}, None
    for j, (r, a, mode) in enumerate(parsed):   # as dmsh_rewrite.HMMA_BODY computes it
        f = set(th.var(a['fl'])[1]) if a['fl'] in defs else None
        if not f or len(f) != 1: raise Refused(f'call {j}: flags that are not one constant')
        f = f.pop()
        if set(th.var(a['lane'])[1]) != set(range(32)) or th.var(a['lane'])[1] != tuple(x % 32 for x in range(THREADS)): raise Refused(f'call {j}: a lane argument that is not the lane')
        if f & 16: raise Refused(f'call {j}: A read from shared memory (FlagADirect)')
        kk = (f >> 8) * 2
        A = prev_a if f & 1 else tuple(word(a[x]) for x in ('a0', 'a1', 'a2', 'a3'))
        if A is None: raise Refused(f'call {j}: A reused before any was loaded')
        btake, bfill = mode >= 0 and mode & 1, mode >= 0 and not mode & 1
        B = bm.get(mode >> 1) if btake else tuple(word(a[x]) for x in ('b0', 'b1'))
        if B is None: raise Refused(f'call {j}: a cached B taken before it was filled')
        if bfill: bm[mode >> 1] = B
        C = (Z, Z) if f & 8 else acc.get(kk) if f & 2 else (word(a['acc0']), word(a['acc1']))
        if C is None: raise Refused(f'call {j}: an accumulator slot read before it was written')
        h = T(('H',) + A + B + C)
        prev_a = A
        if f & 4:
            acc[kk] = (T(('D', h, 0)), T(('D', h, 1)))
            D_of[j] = (Z, Z)
        else: D_of[j] = (T(('D', h, 0)), T(('D', h, 1)))
    scratch = {dmsh_rewrite.split_params(m.group(2))[len(ARGS)] for m in matches}
    sdecl = [re.fullmatch(r'    threadgroup spvUnsafeArray<uint, (\d+)> (%s);' % re.escape(x), ln) for x in scratch for ln in lines[:core0]]
    sdecl = [m for m in sdecl if m]
    if len(scratch) != 1 or len(sdecl) != 1: raise Refused('the calls\' scratch is not one array of the kernel')
    body = '\n'.join(lines)
    if len(re.findall(r'\b%s\b' % re.escape(sdecl[0].group(2)), body)) != CALLS + 1: raise Refused('the calls\' scratch is used outside them')
    mats = [ln.strip() for ln in lines[:core0] if re.fullmatch(r'    simdgroup_half8x8 dm_acc\[16\], dm_am\[4\], dm_bm\[\d+\];', ln)]
    if len(mats) != 1: raise Refused('no matrices of hmma_msl in the kernel')
    if len(weight_buf) != 1: raise Refused(f'weights from {len(weight_buf)} buffers')
    if len(window) != 1: raise Refused(f'the window in {len(window)} arrays')
    # what the epilogue reads of the core: the 8 final words, and values that do not depend on a call (kept)
    tainted = set(res) | set(outs)
    for i in range(core0 + 1, core1 + 1):
        m = DECL.match(lines[i])
        if m and (m.group(2) in tainted or (m.group(3) and set(NAME.findall(m.group(3))) & tainted)): tainted.add(m.group(2))
    later = set(NAME.findall('\n'.join(lines[core1 + 1:])))
    want = {x: p for p, x in enumerate(expected(T))}
    finals = {}
    for n in sorted(tainted & later):
        t = word(n)
        if t not in want: raise Refused(f'the epilogue reads {n}, which is not a final word of the network ({T.show(t)})')
        finals[n] = want[t]
    if sorted(set(finals.values())) != list(range(FINAL_WORDS)): raise Refused(f'the epilogue reads final words of patches {sorted(set(finals.values()))}')
    arr = window.pop()
    pro = lines[:core0]
    for ln in pro:   # gathers and window stores, nothing else that writes memory (pass 1 drops the stores)
        if re.search(r'\]\s*=(?!=)', ln) and not re.match(r'^\s+%s\[[^\n]*\] = [^;]+;$' % re.escape(arr), ln): raise Refused('a store in the prologue other than the window\'s: ' + ln.strip()[:80])
        if re.search(r'\b(?:atomic_\w+|write)\s*\(', ln): raise Refused('a store in the prologue: ' + ln.strip()[:80])
    return {'lines': lines, 'start': k.end(), 'end': body_end, 'core0': core0, 'core1': core1, 'tainted': tainted, 'finals': finals, 'types': types,
            'window': arr, 'weights': weight_buf.pop(), 'params': k.group(1), 'sig': (k.start(), k.end(1)), 'graph': T, 'scratch': (int(sdecl[0].group(1)), sdecl[0].group(2)), 'matrices': mats[0]}


SPLIT_FC = abi.SPLIT_FC
SPLIT_SLOT = abi.SPLIT_SLOT
HELPERS = '''
// agl's NDSR network on simdgroup matrices (dm/gfx/tools/dmsh_ndsr.py). One 8-row half of an HMMA.16816 as dmsh_hmma16816_f16 computes it: C + a0 . b0 (k 0-7) in f32,
// rounded to f16 (RNE), then + a1 . b1 (k 8-15), rounded
static inline __attribute__((always_inline)) simdgroup_half8x8 dm_ndsr_round(simdgroup_float8x8 f) { simdgroup_half8x8 h; h.thread_elements()[0] = half(f.thread_elements()[0]); h.thread_elements()[1] = half(f.thread_elements()[1]); return h; }
static inline __attribute__((always_inline)) simdgroup_half8x8 dm_ndsr_mma(simdgroup_half8x8 c, simdgroup_half8x8 a0, simdgroup_half8x8 a1, simdgroup_half8x8 b0, simdgroup_half8x8 b1) {
    simdgroup_float8x8 f; simdgroup_multiply_accumulate(f, a0, b0, c); const simdgroup_half8x8 r = dm_ndsr_round(f); simdgroup_multiply_accumulate(f, a1, b1, r); return dm_ndsr_round(f); }
// the translated ReLU of a half: +-0 -> +0, else precise::max(v, 0)
static inline __attribute__((always_inline)) half dm_ndsr_relu1(half h) { const float v = float(h); return half((as_type<int>(v) << 1) == 0 ? 0.0 : precise::max(v, 0.0)); }
static inline __attribute__((always_inline)) simdgroup_half8x8 dm_ndsr_relu(simdgroup_half8x8 a) { simdgroup_half8x8 r; r.thread_elements()[0] = dm_ndsr_relu1(a.thread_elements()[0]); r.thread_elements()[1] = dm_ndsr_relu1(a.thread_elements()[1]); return r; }
// The split (DMSH_SPLIT_FC): undefined, one pass; false, pass 0 (prologue and network: each thread's 8 final words to buffer DMSH_SPLIT_SLOT); true, pass 1 (the epilogue from them)
constant bool dm_ndsr_p1 [[function_constant(%(fc)d)]];
constant bool dm_ndsr_split = is_function_constant_defined(dm_ndsr_p1);
constant bool dm_ndsr_net = !dm_ndsr_split || !dm_ndsr_p1;
'''

CORE = '''    // ---- agl's NDSR network on simdgroup matrices (dmsh_ndsr.py): the HMMA calls above computed this, in this order of products and roundings ----
    threadgroup half dm_ndsr_wa[6][256];      // the A matrices (weights 0, 2, .., 10): 16x16 row-major
    threadgroup half dm_ndsr_wb[6][2][128];   // the B matrices (weights 1, 3, .., 11): per n half (words x, y / z, w) 16x8 (k x n) row-major
    int dm_ndsr_fin[8];
    const uint dm_ndsr_t = ((%(wg)s.z * %(ng)s.y + %(wg)s.y) * %(ng)s.x + %(wg)s.x) * %(threads)du + %(lid)s.x;
    if (dm_ndsr_net)
    {
        const uint L = %(lane)s, g = L >> 2, t = L & 3, sg = %(lid)s.x >> 5;
        for (uint jj = 0; jj < 3; jj++)   // constant bounds, every loop of the network: nothing to contain (compute_loops)
        {
            const uint j = sg + 4 * jj;
            const packed_uint4 w = *(const device packed_uint4*)&%(weights)s._m0[(j * %(matrix)du + L * 16u) >> 2];
            const uint ws[4] = {w.x, w.y, w.z, w.w};
            for (uint q = 0; q < 4; q++) for (uint e = 0; e < 2; e++)
            {
                const half v = as_type<half>(ushort(ws[q] >> (16u * e)));
                if ((j & 1u) == 0u) dm_ndsr_wa[j >> 1][(g + 8u * (q & 1u)) * 16u + 2u * t + 8u * (q >> 1) + e] = v;
                else dm_ndsr_wb[j >> 1][q >> 1][(2u * t + 8u * (q & 1u) + e) * 8u + g] = v;
            }
        }
    }
    if (dm_ndsr_net) threadgroup_barrier(mem_flags::mem_threadgroup);
    if (dm_ndsr_net)
    {
        const uint L = %(lane)s, g = L >> 2, t = L & 3, sg = %(lid)s.x >> 5;
        threadgroup half *win = (threadgroup half *)&%(window)s[0];
        const simdgroup_half8x8 z = make_filled_simdgroup_matrix<half, 8, 8>(0.0h);
        for (uint bx = 0; bx < 8; bx++)
        {
            simdgroup_half8x8 X[2][2], Y[2][2];
            for (uint kb = 0; kb < 2; kb++) for (uint nb = 0; nb < 2; nb++)
                simdgroup_load(X[kb][nb], win + (%(sgrows)du * sg + %(nbrows)du * nb) * %(row)du + %(px)du * bx + 8u * kb, %(row)d, ulong2(0, 0), true);
            // block 0: per group H = W1 . X, then at once Y += H . W2 (the calls' order: group 0, then 1)
            Y[0][0] = z; Y[0][1] = z; Y[1][0] = z; Y[1][1] = z;
            for (uint gr = 0; gr < 2; gr++)
            {
                simdgroup_half8x8 H[2][2];
                {   threadgroup half *A = dm_ndsr_wa[gr]; simdgroup_half8x8 a00, a01, a10, a11;
                    simdgroup_load(a00, A, 16, ulong2(0, 0)); simdgroup_load(a01, A, 16, ulong2(8, 0)); simdgroup_load(a10, A, 16, ulong2(0, 8)); simdgroup_load(a11, A, 16, ulong2(8, 8));
                    for (uint nb = 0; nb < 2; nb++) { H[nb][0] = dm_ndsr_mma(z, a00, a01, X[0][nb], X[1][nb]); H[nb][1] = dm_ndsr_mma(z, a10, a11, X[0][nb], X[1][nb]); } }
                for (uint nh = 0; nh < 2; nh++)
                {
                    threadgroup half *Bm = dm_ndsr_wb[gr][nh]; simdgroup_half8x8 b0, b1; simdgroup_load(b0, Bm, 8, ulong2(0, 0)); simdgroup_load(b1, Bm, 8, ulong2(0, 8));
                    Y[nh][0] = dm_ndsr_mma(Y[nh][0], H[0][0], H[1][0], b0, b1); Y[nh][1] = dm_ndsr_mma(Y[nh][1], H[0][1], H[1][1], b0, b1);
                }
            }
            // block 1 transposed (the same products in the same k order for each element): R = relu(Y) as A; per group H^T = R . W1^T, then Y^T += W2^T . H^T
            simdgroup_half8x8 R[2][2];
            for (uint a = 0; a < 2; a++) for (uint b = 0; b < 2; b++) R[a][b] = dm_ndsr_relu(Y[b][a]);
            simdgroup_half8x8 YT[2][2]; YT[0][0] = z; YT[0][1] = z; YT[1][0] = z; YT[1][1] = z;
            for (uint gr = 0; gr < 2; gr++)
            {
                simdgroup_half8x8 HT[2][2];
                {   threadgroup half *A = dm_ndsr_wa[2 + gr]; simdgroup_half8x8 t00, t01, t10, t11;
                    simdgroup_load(t00, A, 16, ulong2(0, 0), true); simdgroup_load(t01, A, 16, ulong2(0, 8), true); simdgroup_load(t10, A, 16, ulong2(8, 0), true); simdgroup_load(t11, A, 16, ulong2(8, 8), true);
                    for (uint p = 0; p < 2; p++) { HT[p][0] = dm_ndsr_mma(z, R[p][0], R[p][1], t00, t10); HT[p][1] = dm_ndsr_mma(z, R[p][0], R[p][1], t01, t11); } }
                for (uint nh = 0; nh < 2; nh++)
                {
                    threadgroup half *Bm = dm_ndsr_wb[2 + gr][nh]; simdgroup_half8x8 u0, u1;
                    simdgroup_load(u0, Bm, 8, ulong2(0, 0), true); simdgroup_load(u1, Bm, 8, ulong2(0, 8), true);
                    YT[nh][0] = dm_ndsr_mma(YT[nh][0], u0, u1, HT[0][0], HT[1][0]); YT[nh][1] = dm_ndsr_mma(YT[nh][1], u0, u1, HT[0][1], HT[1][1]);
                }
            }
            // block 2: X = relu(Y), n 0-7 only
            for (uint kb = 0; kb < 2; kb++) for (uint nb = 0; nb < 2; nb++) X[kb][nb] = dm_ndsr_relu(YT[kb][nb]);
            simdgroup_half8x8 c = z;
            for (uint gr = 0; gr < 2; gr++)
            {
                simdgroup_half8x8 H0, H1;
                {   threadgroup half *A = dm_ndsr_wa[4 + gr]; simdgroup_half8x8 a00, a01;
                    simdgroup_load(a00, A, 16, ulong2(0, 0)); simdgroup_load(a01, A, 16, ulong2(8, 0));
                    H0 = dm_ndsr_mma(z, a00, a01, X[0][0], X[1][0]); H1 = dm_ndsr_mma(z, a00, a01, X[0][1], X[1][1]); }
                threadgroup half *Bm = dm_ndsr_wb[4 + gr][0]; simdgroup_half8x8 b0, b1; simdgroup_load(b0, Bm, 8, ulong2(0, 0)); simdgroup_load(b1, Bm, 8, ulong2(0, 8));
                c = dm_ndsr_mma(c, H0, H1, b0, b1);
            }
            // the last D's word d0 (rows 0-7, columns 2t, 2t+1 of lane (g, t)) from the lane that holds those elements of c
            {   const uint r = g, cc = 2u * t, src = ((r >> 2) & 1u) << 4 | ((cc >> 2) & 1u) << 3 | (r & 3u) << 1 | ((cc >> 1) & 1u);
                dm_ndsr_fin[bx] = as_type<int>(simd_shuffle(as_type<uint>(half2(c.thread_elements()[0], c.thread_elements()[1])), ushort(src))); }
        }
    }
    else
    {
        const uint4 w0 = dm_ndsr_words[2 * dm_ndsr_t], w1 = dm_ndsr_words[2 * dm_ndsr_t + 1];
        for (uint i = 0; i < 4; i++) { dm_ndsr_fin[i] = as_type<int>(w0[i]); dm_ndsr_fin[4 + i] = as_type<int>(w1[i]); }
    }
    if (dm_ndsr_split && !dm_ndsr_p1)
    {
        dm_ndsr_words[2 * dm_ndsr_t] = uint4(as_type<uint>(dm_ndsr_fin[0]), as_type<uint>(dm_ndsr_fin[1]), as_type<uint>(dm_ndsr_fin[2]), as_type<uint>(dm_ndsr_fin[3]));
        dm_ndsr_words[2 * dm_ndsr_t + 1] = uint4(as_type<uint>(dm_ndsr_fin[4]), as_type<uint>(dm_ndsr_fin[5]), as_type<uint>(dm_ndsr_fin[6]), as_type<uint>(dm_ndsr_fin[7]));
        return;
    }
'''
PARAM = re.compile(r'\buint3 (\w+) \[\[(thread_position_in_threadgroup|threadgroup_position_in_grid|threadgroups_per_grid)\]\]|\buint (\w+) \[\[thread_index_in_simdgroup\]\]')


def rewrite(msl, a):
    names = {}
    for m in PARAM.finditer(a['params']): names[m.group(2) or 'lane'] = m.group(1) or m.group(3)
    if 'thread_position_in_threadgroup' not in names or 'lane' not in names or 'threadgroup_position_in_grid' not in names: raise Refused('the kernel lacks its local id, lane or workgroup parameter')
    if 'dm_ndsr' in msl: raise Refused('rewritten already')
    extra = ''
    if 'threadgroups_per_grid' not in names:
        names['threadgroups_per_grid'] = 'dm_ndsr_ng'
        extra += ', uint3 dm_ndsr_ng [[threadgroups_per_grid]]'
    extra += f', device uint4* dm_ndsr_words [[buffer({SPLIT_SLOT}), function_constant(dm_ndsr_split)]]'
    if re.search(r'\[\[buffer\(%d\)' % SPLIT_SLOT, a['params']): raise Refused(f'buffer {SPLIT_SLOT} is the kernel\'s already')
    lines, core0, core1, arr = a['lines'], a['core0'], a['core1'], a['window']
    store = re.compile(r'^(\s+)(%s\[[^\n]*\] = [^;]+;)$' % re.escape(arr))
    gone = {'    ' + a['matrices'], '    threadgroup spvUnsafeArray<uint, %d> %s;' % a['scratch']}
    out = [store.sub(r'\1if (dm_ndsr_net) \2', ln) for ln in lines[:core0] if ln not in gone]   # pass 1 fills no window
    out.append('    if (dm_ndsr_net) threadgroup_barrier(mem_flags::mem_threadgroup);')
    out += (CORE % {'wg': names['threadgroup_position_in_grid'], 'ng': names['threadgroups_per_grid'], 'lid': names['thread_position_in_threadgroup'], 'lane': names['lane'],
                    'threads': THREADS, 'weights': a['weights'], 'matrix': MATRIX, 'window': arr, 'row': WIN_ROW // 2, 'sgrows': 6, 'nbrows': 1152 // WIN_ROW, 'px': 12 // 2}).rstrip('\n').split('\n')
    out += [ln for i, ln in enumerate(lines[core0 + 1:core1 + 1], core0 + 1)
            if not ((m := DECL.match(ln)) and m.group(2) in a['tainted']) and 'dmsh_hmma16816_f16(' not in ln]
    for n, p in sorted(a['finals'].items(), key=lambda x: x[1]):
        ty = a['types'][n]
        out.append(f'    {ty} {n} = as_type<{ty}>(dm_ndsr_fin[{p}]);')
    out += lines[core1 + 1:]
    k0, k1 = a['sig']
    head = msl[:k0] + '\n' + HELPERS.strip('\n') % {'fc': SPLIT_FC} + '\n'
    return head + msl[k0:k1] + extra + msl[k1:a['start']] + '\n'.join(out) + msl[a['end']:]


def network(msl, local, spill=0):
    """-> (msl, info): None without HMMA calls, {'refused': reason}, or {'split': {fc, buffer, words}}"""
    try:
        a = analyse(msl, local)
        if a is None: return msl, None
        if spill: raise Refused('threadgroup memory in a device buffer (tg_spill)')
        return rewrite(msl, a), {'split': {'fc': SPLIT_FC, 'buffer': SPLIT_SLOT, 'words': FINAL_WORDS}}
    except Refused as e:
        return msl, {'refused': str(e)}
