"""dm/tools/pso_carry.py: a prewarm list carried to another version's shader keys, without game files. Shaders are found again by their place in the files: the keys both
versions share anchor the containers' order; containers between anchors pair only when both sides have as many; a container's variations only when both hold as many
keys and no anchor in it moved; a file renamed by its version number pairs with its one twin; a pipeline row only when its carried pair is a pair of the new files."""
import importlib.util
from pathlib import Path
import tempfile
import unittest

spec = importlib.util.spec_from_file_location('pso_carry', Path(__file__).resolve().parents[1] / 'pso_carry.py')
assert spec and spec.loader
pc = importlib.util.module_from_spec(spec); spec.loader.exec_module(pc)


def k(n): return f'{n:016x}'


def smap(rows):   # (key, stage, source)
    return {key: pc.POS.match(src) and (pc.POS.match(src)[1], pc.POS.match(src)[2], int(pc.POS.match(src)[3], 16), pc.POS.match(src)[4], st) for key, st, src in rows}


def skey_hex(vs, fs):   # an R row's pso_skey: vs[24], fs[24], then the rest
    return (vs + '_b').encode().ljust(24, b'\0').hex() + ((fs + '_b').encode().ljust(24, b'\0').hex() if fs else '00' * 24) + '01' * 40


OLD = [(k(1), 'vertex', '/a.bin#BNSH@0x100#var0'), (k(2), 'pixel', '/a.bin#BNSH@0x100#var0'),        # an anchor: the same keys in both
       (k(3), 'vertex', '/a.bin#BNSH@0x900#var0'), (k(4), 'pixel', '/a.bin#BNSH@0x900#var0'),        # recompiled, between two anchors, as many containers on both sides
       (k(5), 'vertex', '/a.bin#BNSH@0x2000#var0'),                                                    # an anchor
       (k(6), 'pixel', '/b.Product.110.bin#BNSH@0x40#var3'), (k(7), 'vertex', '/b.Product.110.bin#BNSH@0x40#var3'),   # its file renamed by version
       (k(8), 'vertex', '/c.bin#BNSH@0x40#var0'), (k(9), 'vertex', '/c.bin#BNSH@0x40#var1'),         # its variations renumbered: one more key in the new file
       (k(10), 'compute', '/d.bin#BAHS@0x80'), (k(11), 'compute', '/d.bin#BAHS@0x100'), (k(12), 'compute', '/d.bin#BAHS@0x200')]
NEW = [(k(1), 'vertex', '/a.bin#BNSH@0x180#var0'), (k(2), 'pixel', '/a.bin#BNSH@0x180#var0'),
       (k(103), 'vertex', '/a.bin#BNSH@0xa00#var0'), (k(104), 'pixel', '/a.bin#BNSH@0xa00#var0'),
       (k(5), 'vertex', '/a.bin#BNSH@0x2400#var0'),
       (k(106), 'pixel', '/b.Product.100.bin#BNSH@0x40#var3'), (k(107), 'vertex', '/b.Product.100.bin#BNSH@0x40#var3'),
       (k(108), 'vertex', '/c.bin#BNSH@0x40#var0'), (k(109), 'vertex', '/c.bin#BNSH@0x40#var1'), (k(110), 'vertex', '/c.bin#BNSH@0x40#var2'),
       (k(111), 'compute', '/d.bin#BAHS@0x60'), (k(10), 'compute', '/d.bin#BAHS@0x90'), (k(112), 'compute', '/d.bin#BAHS@0x120'), (k(113), 'compute', '/d.bin#BAHS@0x220')]


class Carry(unittest.TestCase):
    def test_key_map(self):
        km = pc.key_map(smap(OLD), smap(NEW))
        self.assertEqual(km[k(1)], k(1)); self.assertEqual(km[k(5)], k(5))
        self.assertEqual((km[k(3)], km[k(4)]), (k(103), k(104)))          # between the anchors, one container each side
        self.assertEqual((km[k(6)], km[k(7)]), (k(106), k(107)))          # Product.110 ↔ Product.100
        self.assertNotIn(k(8), km); self.assertNotIn(k(9), km)            # 2 keys against 3: its variations are not carried
        # d.bin: the anchor 0x80 ↔ 0x90; before it 0 against 1 container, after it 2 against 2: 0x100 ↔ 0x120, 0x200 ↔ 0x220
        self.assertEqual((km[k(11)], km[k(12)]), (k(112), k(113)))

    def test_a_moved_anchor_keeps_its_container_out(self):
        old = [(k(1), 'vertex', '/e.bin#BNSH@0x40#var0'), (k(2), 'vertex', '/e.bin#BNSH@0x40#var1')]
        new = [(k(1), 'vertex', '/e.bin#BNSH@0x40#var1'), (k(102), 'vertex', '/e.bin#BNSH@0x40#var0')]
        self.assertNotIn(k(2), pc.key_map(smap(old), smap(new)))

    def test_rows(self):
        km = pc.key_map(smap(OLD), smap(NEW))
        lines = ['# a comment\n', f'S\t{k(3)}_b\t0\n', f'S\t{k(8)}_b\t0\n', f'C\t{k(11)}_b\n', f'S\t{k(1)}_b\t0\n',
                 f'R\t{skey_hex(k(3), k(4))}\n',      # carried, a pair of the new files
                 f'R\t{skey_hex(k(3), k(2))}\n',      # carried, not a pair of the new files: left out
                 f'R\t{skey_hex(k(1), k(2))}\n',      # the same keys: kept as it is
                 f'M\t-\t{skey_hex(k(7), k(6))}\n', f'S\t{k(3)}_b\t0\n']
        rows, st = pc.carry(lines, km, {(k(103), k(104)), (k(107), k(106))})
        self.assertEqual(rows[:3], [f'S\t{k(103)}_b\t0', f'C\t{k(112)}_b', f'S\t{k(1)}_b\t0'])
        self.assertEqual(rows[3], f'R\t{skey_hex(k(103), k(104))}')
        self.assertEqual(rows[4], f'R\t{skey_hex(k(1), k(2))}')
        self.assertEqual(rows[5], f'M\t-\t{skey_hex(k(107), k(106))}')
        self.assertEqual(len(rows), 6)                                     # the duplicate S row once
        self.assertEqual((st['R', 'all'], st['R', 'carried'], st['R', 'kept']), (3, 1, 1))

    def test_file(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t)
            for name, rows in (('from.tsv', OLD), ('to.tsv', NEW)):
                (p / name).write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(f'x.bin\t0\t{st}\t{key}\t{src}\n' for key, st, src in rows))
            (p / 'list.tsv').write_text(f'# recorded on v131072\nS\t{k(3)}_b\t0\nS\t{k(999)}_b\t0\n')
            (p / 'pairs.tsv').write_text('# pairs\n')
            note = pc.carry_file(str(p / 'from.tsv'), str(p / 'to.tsv'), str(p / 'list.tsv'), str(p / 'out.tsv'), [str(p / 'pairs.tsv'), str(p / 'missing.tsv')], 'the test')
            self.assertIn('1 of 2 rows (0 with the same keys, 1 carried', note)
            out = (p / 'out.tsv').read_text().splitlines()
            self.assertTrue(out[0].startswith('# Prewarm seed list carried by dm/tools/pso_carry.py from the test'))
            self.assertEqual(out[1:], ['# recorded on v131072', f'S\t{k(103)}_b\t0'])


if __name__ == '__main__': unittest.main()
