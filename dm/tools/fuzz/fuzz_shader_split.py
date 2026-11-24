#!/usr/bin/env python3
"""Fuzz shader_split.py's in-memory container readers as scan_file dispatches a loose file (scan_blob, uniq_keys, var_pairs). Clean rejection: red()
or a result. Any other exception is a finding: scan_file's net would turn it into an 'unreadable' line that names no layout problem. Usage: see fuzzlib.py"""
import struct
import sys

import fuzzlib

ss = fuzzlib.import_shader_split()

HARNESS = 'shader_split'
fuzzlib.guard(ss)
CTL = ss.CTL_MAGIC


def stage(sph_type):
    """Control: version 0xc9 at +4, +0x38 the instructions' length, +0x40 / +0x44 the code area's; code: the 0x80-byte SPH (word 0: stage in bits 10..13), then instructions"""
    code = bytearray(struct.pack('<I', sph_type) + bytes(0x7c))
    code += bytes.fromhex('0f00000000000000') * 8
    ctl = bytearray(0xe0)
    ctl[:4] = CTL
    struct.pack_into('<I', ctl, 4, 0xc9)
    struct.pack_into('<IIII', ctl, 0x38, len(code), 0, len(code), len(code))
    return bytes(ctl), bytes(code)


VS, PS = stage(0x0461), stage(0x1462)


def bnsh():
    d = bytearray(0x300)
    d[:8] = b'BNSH\0\0\0\0'
    d[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', d, 0x7c, 1, 0x100)
    struct.pack_into('<Q', d, 0x110, 0x140)
    for si, desc, (ctl, code) in ((0, 0x200, VS), (4, 0x240, PS)):
        struct.pack_into('<Q', d, 0x148 + 8 * si, desc)     # vertex 0, pixel 4
        at = len(d)
        d += ctl + code
        struct.pack_into('<QQII', d, desc + 8, at, at + len(ctl), len(code), len(ctl))
    struct.pack_into('<I', d, 0x1c, len(d))
    return bytes(d)


def bnsh_of(variations):
    """Program pointers: vertex at 8, pixel at 0x28, task at 0x40, mesh at 0x48"""
    programs = 0x100 + len(variations) * 0x40
    descriptors = programs + len(variations) * 0x50
    data = bytearray(descriptors + sum(map(len, variations)) * 0x20)
    data[:8] = b'BNSH\0\0\0\0'
    data[0x60:0x64] = b'grsc'
    struct.pack_into('<IQ', data, 0x7c, len(variations), 0x100)
    marks = []
    descriptor = descriptors
    for i, variation in enumerate(variations):
        program = programs + i * 0x50
        struct.pack_into('<Q', data, 0x110 + i * 0x40, program)
        for slot, control, code in variation:
            struct.pack_into('<Q', data, program + slot, descriptor)
            pctl = len(data)
            pcode = pctl + len(control)
            data.extend(control + code)
            struct.pack_into('<QQII', data, descriptor + 8, pctl, pcode, len(code), len(control))
            marks.append(dict(program=program, descriptor=descriptor, control=pctl, code=pcode))
            descriptor += 0x20
    struct.pack_into('<I', data, 0x1c, len(data))
    return bytes(data), marks


def bahs():
    d = bytearray(0x100)
    d[:4] = b'BAHS'
    struct.pack_into('<II', d, 0x40, 8 + 2 * 0x20, 2)       # two 0x20-byte records, the entry at +0x10
    region = 0x100 + sum(len(ctl) for ctl, _ in (VS, PS))
    for i, (ctl, code) in enumerate((VS, PS)):
        rec = 0x48 + 0x20 * i
        ent = rec + 0x10
        c = 0x100 + len(VS[0]) * i
        struct.pack_into('<II', d, rec, 0x20, i)            # size, kind (0 vertex, 1 fragment)
        struct.pack_into('<IIIi', d, ent, 0x100 * i, 0, len(ctl), c - ent)
    struct.pack_into('<II', d, 0x88, 8 + 16, 1)
    struct.pack_into('<IIII', d, 0x90, 16, 0, 0x11, 0)
    d += VS[0] + PS[0]
    code = bytearray(0x200)
    code[:len(VS[1])] = VS[1]
    code[0x100:0x100 + len(PS[1])] = PS[1]
    struct.pack_into('<II', d, 0x18, len(code), region)
    d += code
    return bytes(d)


def yaz0(b):   # every byte a literal: header 0xff per 8 bytes
    out = bytearray(b'Yaz0' + len(b).to_bytes(4, 'big') + bytes(8))
    for i in range(0, len(b), 8): out += b'\xff' + b[i:i + 8]
    return bytes(out)


def sarc(members):
    names = b''.join(n + b'\0' + bytes(-(len(n) + 1) % 4) for n, _ in members)
    sfat = b'SFAT' + struct.pack('<HHI', 0xc, len(members), 0x65)
    data, at, offs = b'', 0, []
    for n, m in members:
        offs.append(at)
        data += m + bytes(-len(m) % 0x100)
        at = len(data)
    noff = 0
    for (n, m), o in zip(members, offs):
        sfat += struct.pack('<IIII', 0, 0x01000000 | noff // 4, o, o + len(m))
        noff += len(n) + 1 + (-(len(n) + 1) % 4)
    head = 0x14 + len(sfat) + 8 + len(names)
    doff = (head + 0xff) & ~0xff
    d = b'SARC' + struct.pack('<HHII', 0x14, 0xfeff, doff + len(data), doff) + bytes(4) + sfat + b'SFNT' + struct.pack('<HH', 8, 0) + names
    return d + bytes(doff - len(d)) + data


def seeds():
    b = bnsh()
    return {'bnsh': b, 'bahs': bahs(), 'sarc-yaz0': yaz0(sarc([(b'shader.bnsh', b)]))}


def check(data):
    try:
        found = []
        ss.scan_blob(data, '/fuzz', found)
        ss.uniq_keys(found)
        ss.var_pairs(found)
    except SystemExit as e:
        if not str(e).startswith('[RED] '): raise
    finally: ss.BAHS_VAR.clear()


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
