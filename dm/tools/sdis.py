# Disassembly with symbol names.
# Usage: sdis.py <original image .orig.bin> <symbol name|0xoffset> [count=200]
#        sdis.py <image> --sym 0xoffset...      function+offset
#        sdis.py <image> --xref 0xaddr...       adrp+add that build it
#        sdis.py <image> --fn 0xaddr...         function start
#        sdis.py <image> --find <substring>     defined symbols
#        sdis.py <image> --callers <symbol>     its direct calls
import struct, sys, bisect
from capstone import *
from capstone.arm64 import ARM64_OP_IMM

img = open(sys.argv[1], 'rb').read()
md = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
md.detail = True
m = struct.unpack_from('<I', img, 4)[0]
dyn = m + struct.unpack_from('<i', img, m + 4)[0]
t = {}
o = dyn
while True:
    tag, v = struct.unpack_from('<qQ', img, o)
    o += 16
    if tag == 0: break
    t.setdefault(tag, v)
SYM, STR = t[6], t[5]
def cstr(off): return img[off:img.index(b'\0', off)].decode()
nsym = (STR - SYM) // 24
defs = {}
byaddr = {}
for i in range(1, nsym):
    nm, info, oth, shn, val, sz = struct.unpack_from('<IBBHQQ', img, SYM + i * 24)
    if shn and val:
        n = cstr(STR + nm)
        defs[n] = (val, sz)
        byaddr.setdefault(val, n)
funcs = sorted(byaddr.items())
addrs = [a for a, _ in funcs]
# PLT: adrp x16 / ldr x17,[x16,#o] / add / br x17 → JUMP_SLOT name
got = {}
for i in range(0, t.get(2, 0), 24):
    off, info, add = struct.unpack_from('<QQq', img, t[23] + i)
    got[off] = cstr(STR + struct.unpack_from('<I', img, SYM + (info >> 32) * 24)[0])
plt = {}
def plt_scan():
    for o in range(0, len(img) - 16, 4):
        w = struct.unpack_from('<I', img, o)[0]
        if w & 0x9F00001F != 0x90000010: continue
        ins = list(md.disasm(img[o:o + 16], o))
        if len(ins) == 4 and ins[1].mnemonic == 'ldr' and ins[3].mnemonic == 'br':
            a = ins[0].operands[1].imm + ins[1].operands[1].mem.disp
            if a in got: plt[o] = got[a]
def name(a):
    if a in plt: return 'PLT:' + plt[a]
    i = bisect.bisect_right(addrs, a) - 1
    if i < 0: return hex(a)
    return funcs[i][1] + ('' if a == funcs[i][0] else f'+{a - funcs[i][0]:#x}')

def fn_starts():
    m0 = struct.unpack_from('<I', img, 4)[0]
    ehs = m0 + struct.unpack_from('<i', img, m0 + 16)[0]
    ver, penc, cenc, tenc = img[ehs:ehs + 4]
    def rd(o, enc):
        fmt, app = enc & 0xf, enc & 0x70
        v, sz = {0x3: (struct.unpack_from('<I', img, o)[0], 4), 0xb: (struct.unpack_from('<i', img, o)[0], 4),
                 0x4: (struct.unpack_from('<Q', img, o)[0], 8)}[fmt]
        if app == 0x10: v += o
        if app == 0x30: v += ehs
        return v, sz
    o = ehs + 4
    _, k = rd(o, penc)
    o += k
    n, k = rd(o, cenc)
    o += k
    out = []
    for _ in range(n):
        a, k = rd(o, tenc)
        o += k
        _, k = rd(o, tenc)
        o += k
        out.append(a)
    return sorted(set(out))

def resolve(s):
    if s.startswith('0x'): return int(s, 16)
    if s in defs: return defs[s][0]
    if not plt: plt_scan()
    for a, n in plt.items():
        if n == s: return a
    sys.exit(f'{s}: neither defined nor imported through the PLT')

def text_end():   # a main exports too few symbols: the PLT ends the text
    end = max((v + s for v, s in defs.values() if v < 0x10000000 and s), default=0)
    if not plt: plt_scan()
    return max(end, max(plt) + 16) if plt else end

if sys.argv[2] == '--sym':
    plt_scan()
    for a in sys.argv[3:]: print(a, name(int(a, 16)))
    sys.exit()
if sys.argv[2] == '--xref':
    want = {int(a, 16) for a in sys.argv[3:]}
    for o in range(0, min(len(img), text_end()), 4):
        w = struct.unpack_from('<I', img, o)[0]
        if w & 0x9F000000 != 0x90000000: continue
        rd = w & 31
        imm = (((w >> 5) & 0x7FFFF) << 2) | ((w >> 29) & 3)
        if imm & (1 << 20): imm -= 1 << 21
        page = (o & ~0xFFF) + (imm << 12)
        for i in md.disasm(img[o + 4:o + 24], o + 4):
            ops = i.operands
            if i.mnemonic == 'add' and len(ops) == 3 and ops[1].reg and i.reg_name(ops[1].reg) == f'x{rd}' and ops[2].type == ARM64_OP_IMM:
                if page + ops[2].imm in want: print(f'{i.address:#x} {name(i.address)} -> {page + ops[2].imm:#x}')
                break
    sys.exit()
if sys.argv[2] == '--fn':
    st = fn_starts()
    for a in sys.argv[3:]:
        a = int(a, 16)
        k = bisect.bisect_right(st, a) - 1
        if k < 0 or k + 1 >= len(st):   # the patch area: reported alone, the batch goes on
            print(f'{a:#x} not in the function table')
            continue
        print(f'{a:#x} function {st[k]:#x} +{a - st[k]:#x}  next function {st[k + 1]:#x}')
    sys.exit()
if sys.argv[2] == '--find':
    for n, (v, sz) in sorted(defs.items(), key=lambda x: x[1]):
        if sys.argv[3] in n: print(f'{v:#x} {sz:#x} {n}')
    sys.exit()
if sys.argv[2] == '--callers':
    tgt = resolve(sys.argv[3])
    for o in range(0, min(len(img), text_end()), 4):
        w = struct.unpack_from('<I', img, o)[0]
        if w >> 26 in (0x25, 0x05) and o + ((((w & 0x3ffffff) ^ 0x2000000) - 0x2000000) * 4) == tgt:
            print(f'{o:#x} {name(o)}')
    sys.exit()
plt_scan()
a = resolve(sys.argv[2])
n = int(sys.argv[3]) if len(sys.argv) > 3 else 200
for ins in md.disasm(img[a:a + n * 4], a):
    ann = ''
    if ins.mnemonic in ('bl', 'b') or ins.mnemonic.startswith('b.') or ins.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz', 'adr'):
        op = ins.operands[-1]
        if op.type == ARM64_OP_IMM: ann = '  ; ' + name(op.imm)
    print(f'{ins.address:#9x}  {ins.mnemonic:8s} {ins.op_str}{ann}')
    if ins.mnemonic == 'ret' and n > 60 and ins.address > a + 8 and len(sys.argv) <= 3: break
