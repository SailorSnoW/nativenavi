"""dm/tools/pso_carry.py: a prewarm list carried to another version's shader keys, without game files. Shaders are found again by their place in the files: the keys both
versions share anchor the containers' order; containers between anchors pair only when both sides have as many; a container's variations only when both hold as many
keys and no anchor in it moved; a file renamed by its version number pairs with its one twin; a pipeline row only when its carried pair is a pair of the new files. An
effect set's (a .vsetb file's) BNSH variations are found again within their container, anchored on the shared keys and paired in order between anchors that hold as
many. A shader archive's keys are found again by their program's options (dm/tools/bfsha.py), its reading faked here."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
from unittest import mock

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
        lines = ['# a comment\n', f'S\t{k(3)}_b\t0\n', f'C\t{k(8)}_b\n', f'C\t{k(11)}_b\n', f'C\t{k(10)}_b\n',
                 f'R\t{skey_hex(k(3), k(4))}\n',      # carried, a pair of the new files
                 f'R\t{skey_hex(k(3), k(2))}\n',      # carried, not a pair of the new files: left out
                 f'R\t{skey_hex(k(1), k(2))}\n',      # the same keys: kept as it is
                 f'M\t-\t{skey_hex(k(7), k(6))}\n', f'C\t{k(11)}_b\n']
        rows, st = pc.carry(lines, km, {(k(103), k(104)), (k(107), k(106))})
        self.assertEqual(rows[:2], [f'C\t{k(112)}_b', f'C\t{k(10)}_b'])
        self.assertEqual(rows[2], f'R\t{skey_hex(k(103), k(104))}')
        self.assertEqual(rows[3], f'R\t{skey_hex(k(1), k(2))}')
        self.assertEqual(rows[4], f'M\t-\t{skey_hex(k(107), k(106))}')
        self.assertEqual(len(rows), 5)                                     # the S row left out, the duplicate C row once
        self.assertNotIn(('S', 'all'), st)
        self.assertEqual((st['R', 'all'], st['R', 'carried'], st['R', 'kept']), (3, 1, 1))

    def test_file(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t)
            for name, rows in (('from.tsv', OLD), ('to.tsv', NEW)):
                (p / name).write_text('# map\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(f'x.bin\t0\t{st}\t{key}\t{src}\n' for key, st, src in rows))
            (p / 'list.tsv').write_text(f'# recorded on v131072\nC\t{k(11)}_b\nC\t{k(999)}_b\nS\t{k(3)}_b\t0\n')
            (p / 'pairs.tsv').write_text('# pairs\n')
            note = pc.carry_file(str(p / 'from.tsv'), str(p / 'to.tsv'), str(p / 'list.tsv'), str(p / 'out.tsv'), [str(p / 'pairs.tsv'), str(p / 'missing.tsv')], 'the test')
            self.assertIn('1 of 2 rows (0 with the same keys, 1 carried', note)
            out = (p / 'out.tsv').read_text().splitlines()
            self.assertTrue(out[0].startswith('# Prewarm seed list carried by dm/tools/pso_carry.py from the test'))
            self.assertEqual(out[1:], ['# recorded on v131072', f'C\t{k(112)}_b'])


V_OLD = [(k(30), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x1000#var0'), (k(31), 'pixel', '/Effect/static.vsetb.byml.zs#BNSH@0x1000#var0'),   # anchors: the same keys
         (k(32), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x1000#var1'), (k(33), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x1000#var2'),   # recompiled, between the anchors, as many variations on both sides
         (k(35), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x1000#var3'),   # the second anchor, moved to var7 in the new file
         (k(36), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x1000#var5'), (k(37), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x1000#var6'),   # 2 variations against 1: not paired
         (k(40), 'compute', '/Effect/static.vsetb.byml.zs#BNSH@0x9000#var0')]  # the compute BNSH
V_NEW = [(k(30), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x3000#var0'), (k(31), 'pixel', '/Effect/static.vsetb.byml.zs#BNSH@0x3000#var0'),
         (k(130), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x3000#var2'), (k(131), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x3000#var3'),
         (k(35), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x3000#var7'),
         (k(132), 'vertex', '/Effect/static.vsetb.byml.zs#BNSH@0x3000#var9'),
         (k(140), 'compute', '/Effect/static.vsetb.byml.zs#BNSH@0xa000#var0')]


class Vsetb(unittest.TestCase):
    def test_variations_pair_by_their_anchors(self):
        a, b = smap(V_OLD), smap(V_NEW)
        got = pc.vsetb_keys(a, b)
        self.assertEqual(got, {k(32): k(130), k(33): k(131), k(40): k(140)})   # the keys both versions have are key_map's (k→k), not these
        # k(32)/k(33): between the anchors var0 and var3→var7, as many variations on both sides, paired in order
        # k(36)/k(37): the last segment holds 2 old against 1 new: not paired; k(40): the compute BNSH pairs with the compute BNSH

    def test_a_container_count_mismatch_pairs_nothing(self):
        old = [(k(42), 'vertex', '/Effect/x.vsetb.byml.zs#BNSH@0x100#var0'), (k(43), 'vertex', '/Effect/x.vsetb.byml.zs#BNSH@0x200#var0')]
        new = [(k(142), 'vertex', '/Effect/x.vsetb.byml.zs#BNSH@0x800#var0')]
        self.assertEqual(pc.vsetb_keys(smap(old), smap(new)), {})

    def test_a_non_vsetb_file_is_left_to_the_place_carry(self):
        old = [(k(50), 'vertex', '/Shader/x.bin#BNSH@0x100#var0'), (k(51), 'vertex', '/Shader/x.bin#BNSH@0x100#var1')]
        new = [(k(150), 'vertex', '/Shader/x.bin#BNSH@0x800#var0'), (k(151), 'vertex', '/Shader/x.bin#BNSH@0x800#var1')]
        self.assertEqual(pc.vsetb_keys(smap(old), smap(new)), {})


ARC = '/m.Product.110.bfsha.zs#BNSH@0x1000#var'
P_OLD = [(k(20), 'vertex', ARC + '0'), (k(21), 'pixel', ARC + '0'),
         (k(22), 'pixel', ARC + '1'),                 # its program is not in the new archive
         (k(23), 'vertex', ARC + '2'),                # its program's new vertex key is a pixel shader of the new map: not taken
         (k(24), 'vertex', ARC + '3')]                # not in the table
P_NEW = [(k(120), 'vertex', '/m.Product.100.bfsha.zs#BNSH@0x2000#var5'), (k(121), 'pixel', '/m.Product.100.bfsha.zs#BNSH@0x2000#var5'),
         (k(123), 'pixel', '/m.Product.100.bfsha.zs#BNSH@0x2000#var7')]
P_TABLE = {k(20): ('model', 'aa'), k(21): ('model', 'aa'), k(22): ('model', 'bb'), k(23): ('model', 'cc')}


def fake_archive(read=None):
    return (mock.patch.object(pc.bfsha, 'read_member', side_effect=read or (lambda romfs, path: path.encode())),
            mock.patch.object(pc.bfsha, 'programs', return_value=[('other', 0x1000, {5: 'aa'}), ('model', 0x2000, {5: 'aa', 6: 'cc'})]),
            mock.patch.object(pc.bfsha, 'variation_keys', return_value={5: {'vs': k(120), 'ps': k(121)}, 6: {'vs': k(123), 'ps': k(121)}}))


class Programs(unittest.TestCase):
    def test_program_keys(self):
        a, b = smap(P_OLD), smap(P_NEW)
        r, p, v = fake_archive()
        with r as read, p, v:
            got = pc.program_keys(a, b, pc.key_map(a, b), P_TABLE, '/romfs')
        self.assertEqual(got, {k(20): k(120), k(21): k(121)})
        read.assert_called_once_with('/romfs', '/m.Product.100.bfsha.zs')   # the file renamed by version, read from the new version's RomFS

    def test_an_unread_archive_carries_nothing(self):
        a, b, said = smap(P_OLD), smap(P_NEW), []
        def unread(romfs, path): raise pc.bfsha.Unread(f'{path}: FSHA version 0x00080000, only 0x00090101 is read')
        r, p, v = fake_archive(unread)
        with r, p, v:
            self.assertEqual(pc.program_keys(a, b, {}, P_TABLE, '/romfs', said.append), {})
        self.assertEqual(len(said), 1); self.assertTrue(said[0].startswith('[RED] /m.Product.100.bfsha.zs: FSHA version'))

    def test_file(self):
        with tempfile.TemporaryDirectory() as t:
            p = Path(t)
            for name, rows in (('from.tsv', P_OLD), ('to.tsv', P_NEW)):
                (p / name).write_text('package\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(f'x.bin\t0\t{st}\t{key}\t{src}\n' for key, st, src in rows))
            (p / 'programs.tsv').write_text('key\tmodel\toptions\n' + ''.join(f'{key}\t{m}\t{g}\n' for key, (m, g) in P_TABLE.items()))
            (p / 'list.tsv').write_text(f'R\t{skey_hex(k(20), k(21))}\nR\t{skey_hex(k(23), k(22))}\n')
            (p / 'pairs.tsv').write_text(f'{k(120)}\t{k(121)}\n')
            r, pr, v = fake_archive()
            with r, pr, v:
                note = pc.carry_file(str(p / 'from.tsv'), str(p / 'to.tsv'), str(p / 'list.tsv'), str(p / 'out.tsv'), [str(p / 'pairs.tsv')],
                                     programs=str(p / 'programs.tsv'), romfs='/romfs')
            self.assertIn('1 of 2 rows', note); self.assertIn('2 shader archive keys found again by their program', note)
            self.assertEqual((p / 'out.tsv').read_text().splitlines()[1:], [f'R\t{skey_hex(k(120), k(121))}'])


if __name__ == '__main__': unittest.main()
