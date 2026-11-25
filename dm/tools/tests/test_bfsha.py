"""dm/tools/bfsha.py: a shader archive's (FSHA) programs named by their options, the same name in another version of the archive whose programs, variations and key bits
are in another order; an archive of another version or a program pointing at no variation refused; the profile's table naming every shader map key from a shader
archive. The archives are synthetic."""
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('bfsha', REPO / 'dm/tools/bfsha.py')
assert spec and spec.loader
bf = importlib.util.module_from_spec(spec); spec.loader.exec_module(bf)
PROFILE = REPO / 'dm/games/0400486024DEC000'


class Blob:
    def __init__(self): self.d = bytearray()

    def put(self, b, align=8):
        while len(self.d) % align: self.d.append(0)
        at = len(self.d); self.d += b
        return at

    def string(self, s): return self.put(struct.pack('<H', len(s)) + s.encode() + b'\0', 2)

    def names(self, ns):   # the root node, then one per name
        nodes = struct.pack('<IHHQ', 0xffffffff, 1, 0, self.string(''))
        for i, n in enumerate(ns): nodes += struct.pack('<IHHQ', 0, i + 1, i + 1, self.string(n))
        return self.put(struct.pack('<II', 0, len(ns)) + nodes)


def archive(options, programs, nvar=None, version=bf.VERSION):
    """options: [(name, choices, word, shift)] (all static, one key word); programs: [(variation, {name: choice})] in program order"""
    b = Blob(); b.put(b'FSHA    ' + struct.pack('<I', version) + bytes(0x14))
    arc_at = b.put(bytes(0x48)); struct.pack_into('<Q', b.d, 0x20, arc_at)
    opts = b''
    for name, choices, word, shift in options:
        opts += struct.pack('<QQQ', b.string(name), b.names(choices), 0) + struct.pack('<HHHBB', len(choices), 0, 0, 0, 2) + struct.pack('<IBBH', 0xf << shift, word, shift, 0)
    opt_at = b.put(opts)
    keys = b''
    for _, vals in programs:
        w = [0, 0]
        for name, choices, word, shift in options: w[word] |= choices.index(vals[name]) << shift
        keys += struct.pack('<2I', *w)
    key_at = b.put(keys)
    nvar = len(programs) if nvar is None else nvar
    bnsh = b.put(b'BNSH' + bytes(0x78) + struct.pack('<IQ', nvar, 0xc0) + bytes(0x38) + bytes(0x40 * nvar), 0x1000)
    progs = b''.join(bytes(0x20) + struct.pack('<Q', bnsh + 0xc0 + 0x40 * v) + bytes(0x18) for v, _ in programs)
    prog_at = b.put(progs)
    model = bytearray(0x100)
    struct.pack_into('<QQ', model, 0, b.string('model'), opt_at)
    struct.pack_into('<Q', model, 0x18, opt_at + 0x28 * len(options))
    struct.pack_into('<QQ', model, 0x88, prog_at, key_at)
    struct.pack_into('<Q', model, 0xa8, bnsh)
    struct.pack_into('<iHHHBB', model, 0xe0, -1, len(options), 0, len(programs), 2, 0)
    model_at = b.put(bytes(model))
    struct.pack_into('<Q', b.d, arc_at + 0x10, model_at); struct.pack_into('<H', b.d, arc_at + 0x40, 1)
    return bytes(b.d), bnsh


LIGHT = ['off', 'on']; FOG = ['none', 'near', 'far']
OLD = [('light', LIGHT, 0, 0), ('fog', FOG, 0, 4)]
NEW = [('fog', FOG, 1, 8), ('light', LIGHT, 0, 12)]   # the options listed in another order, in other key bits


class Programs(unittest.TestCase):
    def test_the_same_options_name_the_same_program(self):
        old, _ = archive(OLD, [(0, {'light': 'off', 'fog': 'none'}), (1, {'light': 'on', 'fog': 'far'}), (2, {'light': 'on', 'fog': 'near'})])
        new, bnsh = archive(NEW, [(2, {'light': 'on', 'fog': 'far'}), (0, {'light': 'off', 'fog': 'none'}), (1, {'light': 'off', 'fog': 'far'})])
        [(name, _, a)], [(_, at, b)] = bf.programs(old), bf.programs(new)
        self.assertEqual((name, at), ('model', bnsh))
        self.assertEqual(sorted(b), [0, 1, 2])
        self.assertEqual(a[0], b[0])                       # light off, no fog: variation 0 in both
        self.assertEqual(a[1], b[2])                       # light on, far fog: variation 1, then 2
        self.assertNotIn(a[2], b.values())                 # light on, near fog: not in the new archive
        self.assertNotIn(b[1], a.values())

    def test_refused(self):
        d, _ = archive(OLD, [(0, {'light': 'off', 'fog': 'none'})], version=0x00080000)
        with self.assertRaisesRegex(bf.Unread, 'FSHA version 0x00080000'): bf.programs(d)
        d, _ = archive(OLD, [(3, {'light': 'off', 'fog': 'none'})], nvar=2)
        with self.assertRaisesRegex(bf.Unread, 'program 0 points at no variation'): bf.programs(d)
        with self.assertRaisesRegex(bf.Unread, 'not a shader archive'): bf.programs(b'BNSH' + bytes(0x100))

    def test_table_round_trip(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t) / 'programs.tsv'
            p.write_text(bf.HEAD + 'key\tmodel\toptions\n0000000000000001\tmodel\t00000000000000aa\n')
            self.assertEqual(bf.read_table(str(p)), {'0000000000000001': ('model', '00000000000000aa')})
            self.assertEqual(bf.read_table(str(Path(t) / 'missing.tsv')), {})


class Profile(unittest.TestCase):
    def test_the_table_names_every_shader_archive_key_of_the_map(self):
        want = {key for keys in bf.sources(str(PROFILE / 'shader_map.tsv')).values() for key, *_ in keys}
        self.assertTrue(want)
        self.assertEqual(set(bf.read_table(str(PROFILE / 'shader_programs.tsv'))), want,
                         'shader_programs.tsv is not the shader map\'s: python3 dm/tools/bfsha.py <RomFS of the registered version> <shader_map.tsv> <shader_programs.tsv>')


if __name__ == '__main__': unittest.main()
