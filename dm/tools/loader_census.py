#!/usr/bin/env python3
# Which entry points of a by-name loader (glad over eglGetProcAddress, NVN's over nvnDeviceGetProcAddress) the code really calls: each GOT slot the loader
# stores is named after its string, and the slots loaded elsewhere are the functions called.
# Usage: loader_census.py <NSO or decompressed image> [--sites name,name...]
import struct, sys, collections

def image(path):
    d = open(path, 'rb').read()
    if d[:4] != b'NSO0': return d, len(d)
    import os
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import nso
    try: m = nso.load(d, path)
    except nso.Error as e: sys.exit(f'[RED] {e}')
    return bytes(m.image), m.text_end

def adrp(w, pc):
    if (w & 0x9f000000) != 0x90000000: return None
    imm = ((((w >> 5) & 0x7ffff) << 2) | ((w >> 29) & 3)) << 12
    if imm & (1 << 32): imm -= 1 << 33
    return w & 31, (pc & ~0xfff) + imm
def addimm(w): return (w & 31, (w >> 5) & 31, (w >> 10) & 0xfff) if (w & 0xffc00000) == 0x91000000 else None
def ldr64(w): return (w & 31, (w >> 5) & 31, ((w >> 10) & 0xfff) * 8) if (w & 0xffc00000) == 0xf9400000 else None
def is_call(w): return (w & 0xfc000000) == 0x94000000 or (w & 0xfffffc1f) == 0xd63f0000   # bl, blr
def is_str_x0(w, base): return (w & 0xffc003ff) == 0xf9000000 | (base << 5) and ((w >> 10) & 0xfff) == 0   # str x0, [base]
def str_x0(w): return ((w >> 5) & 31, ((w >> 10) & 0xfff) * 8) if (w & 0xffc0001f) == 0xf9000000 else None   # str x0, [base, #off] → (base, off)

def got_values(img):
    """RELA R_AARCH64_RELATIVE, and RELR, whose addend is in place"""
    out = {}
    mod0 = struct.unpack_from('<I', img, 4)[0]
    if img[mod0:mod0 + 4] != b'MOD0': return out
    dyn = mod0 + struct.unpack_from('<i', img, mod0 + 4)[0]
    tags = {}
    for k in range(4096):
        t, v = struct.unpack_from('<qQ', img, dyn + k * 16)
        if t == 0: break
        tags.setdefault(t, v)
    for k in range(tags.get(8, 0) // 24):   # DT_RELA / DT_RELASZ
        off, info, add = struct.unpack_from('<QQq', img, tags[7] + k * 24)
        if info & 0xffffffff == 1027: out[off] = add
        elif info & 0xffffffff in (257, 1025) and 6 in tags:
            shndx, value = struct.unpack_from('<HQ', img, tags[6] + (info >> 32) * 24 + 6)
            if shndx: out[off] = value + add
    where = 0
    for k in range(tags.get(35, 0) // 8):   # DT_RELR / DT_RELRSZ
        e = struct.unpack_from('<Q', img, tags[36] + k * 8)[0]
        if not e & 1:
            where = e
            out[where] = struct.unpack_from('<Q', img, where)[0]
            where += 8
        else:
            for b in range(63):
                if e >> (b + 1) & 1:
                    a = where + b * 8
                    out[a] = struct.unpack_from('<Q', img, a)[0]
            where += 63 * 8
    return out

def known_loads(words, got=None):
    """(index, address) of every 64-bit load whose base register holds a known address, followed in a straight line: a return forgets them all, a call or
    branch keeps only x19–x28. A load from a GOT slot gives its register the address the slot holds."""
    got = got or {}
    val = {}   # register → the addresses it may hold
    for i, w in enumerate(words):
        r = adrp(w, i * 4)
        if r:
            val[r[0]] = (r[1],)
            continue
        a = addimm(w)
        if a:
            if a[1] in val: val[a[0]] = tuple(x + a[2] for x in val[a[1]])
            else: val.pop(a[0], None)
            continue
        l = ldr64(w)
        if l:
            ats = tuple(x + l[2] for x in val.get(l[1], ()))
            for at in ats: yield i, at
            held = tuple(got[at] for at in ats if at in got)   # the next load off a GOT slot reads the global
            if held: val[l[0]] = held
            else: val.pop(l[0], None)
            continue
        mov, csel = (w & 0xffe0ffe0) == 0xaa0003e0, (w & 0xffe00c00) == 0x9a800000
        if mov or csel:
            src = [(w >> 16) & 31] + ([(w >> 5) & 31] if csel else [])
            if all(k in val for k in src): val[w & 31] = tuple(x for k in src for x in val[k])
            else: val.pop(w & 31, None)
            continue
        if (w & 0xfc000000) == 0x94000000 or (w & 0xfffffc1f) == 0xd63f0000:   # x0–x18 and x30 are the callee's
            for k in [*range(19), 30]: val.pop(k, None)
            continue
        if (w & 0xfc000000) == 0x14000000:   # reached from elsewhere: x19–x28 still hold hoisted pointers
            for k in [*range(19), 29, 30]: val.pop(k, None)
            continue
        if w == 0xd65f03c0 or (w & 0xfffffc1f) == 0xd61f0000:
            val.clear()
            continue
        if (w & 0x0a000000) == 0x08000000:
            if (w & 0x3a000000) == 0x28000000:   # pairs: a load writes both
                if (w >> 22) & 1:
                    val.pop(w & 31, None)
                    val.pop((w >> 10) & 31, None)
                if (w >> 23) & 1 or not (w >> 24) & 1: val.pop((w >> 5) & 31, None)
            else:
                if (w & 0x3b000000) == 0x39000000: load = (w >> 22) & 3 != 0
                else:
                    load = (w >> 22) & 3 != 0 or (w & 0x3f000000) in (0x08000000, 0x18000000, 0x58000000, 0x98000000)
                    if (w & 0x3b200000) == 0x38000000 and (w >> 10) & 1: val.pop((w >> 5) & 31, None)
                if load: val.pop(w & 31, None)
            continue
        if (w & 0x1c000000) == 0x14000000 and (w & 0xfff00000) != 0xd5300000: continue   # branches, compares, system (but mrs) write no register
        val.pop(w & 31, None)

def main():
    a = sys.argv[1:]
    want = []
    if '--sites' in a:
        i = a.index('--sites')
        want = a[i + 1].split(',')
        del a[i:i + 2]
    if len(a) != 1: sys.exit('usage: loader_census.py <NSO or image> [--sites name,...]')
    img, text = image(a[0])
    n = text // 4
    words = struct.unpack_from(f'<{n}I', img, 0)
    def cstr(p):
        e = img.find(b'\0', p, p + 256)
        try: return img[p:e].decode('ascii') if e > p else None
        except UnicodeDecodeError: return None
    slot, loader, page = {}, set(), {}
    for i in range(4, n - 3):
        r = adrp(words[i], i * 4)
        a = addimm(words[i])
        if r: page[r[0]] = r[1]   # kept in x19–x28 across a loader
        elif a and a[1] in page: page[a[0]] = page[a[1]] + a[2]
        if not is_call(words[i]): continue
        name = None
        for k in range(i - 4, i - 1):   # adrp + add into x0 or x1 just before the call
            r = adrp(words[k], k * 4)
            s = addimm(words[k + 1])
            if r and s and r[0] in (0, 1) and s[1] == r[0] and s[0] == r[0]: name = cstr(r[1] + s[2])
        if not name: continue
        r = adrp(words[i + 1], (i + 1) * 4)
        l = ldr64(words[i + 2])
        s = str_x0(words[i + 1])
        s2 = str_x0(words[i + 2])
        if r and l and l[1] == r[0] and is_str_x0(words[i + 3], l[0]): at = r[1] + l[2]
        elif r and s2 and s2[0] == r[0]: at = r[1] + s2[1]
        elif s and s[0] in page and s[0] not in (0, 1): at = page[s[0]] + s[1]
        else: continue
        if name not in slot.setdefault(at, []): slot[at].append(name)
        loader.update(range((i - 4) * 4, (i + 4) * 4, 4))
    slot = {at: ' / '.join(sorted(names, key=len)) for at, names in slot.items()}
    refs, sites = collections.Counter(), collections.defaultdict(list)
    for i, at in known_loads(words, got_values(img)):
        if at in slot and i * 4 not in loader:
            refs[slot[at]] += 1
            sites[slot[at]].append(i * 4)
    fams = collections.Counter(nm[:2] if nm.startswith('gl') else nm[:3] for nm in slot.values())
    print(f'{len(slot)} slots stored by a loader ({", ".join(f"{k}* {v}" for k, v in fams.most_common(6))}); {len(refs)} loaded by other code:')
    for nm in sorted(refs): print(f'  {refs[nm]:4d}  {nm}')
    for nm in want: print(f'{nm}: ' + ' '.join(hex(x) for x in sites.get(nm, [])))

if __name__ == '__main__': main()
