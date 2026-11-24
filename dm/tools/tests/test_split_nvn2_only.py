"""shader_split: the chain translates only an NVN2 game's vertex, pixel, compute, task and mesh shaders (Ocarina of Time has no other). A shader of another form or stage met in a
file stops the split with a red line that names it (an NVN1 / Maxwell shader, a hull, domain or geometry shader, a ray tracing module): none is left out as if the file held no
shader, and none reaches the map. All inputs are synthetic."""
import contextlib
import io
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz  # noqa: E402

ss = fz.ss
GFX = bytes.fromhex('78563412')   # an NVN1 code area's header magic


def nvn1(sph_type):   # (control section, code area) as NVN1 has them: version 1, 0x880 bytes, its own length at +0x20, the code area's at +0x704; a 0x30-byte code header before the SPH
    code = GFX + bytes(0x2c) + struct.pack('<I', sph_type) + bytes(0x4c) + bytes.fromhex('0f00000000000000') * 8
    ctl = bytearray(0x880); ctl[:4] = ss.CTL_MAGIC; struct.pack_into('<I', ctl, 4, 1); struct.pack_into('<I', ctl, 0x20, 0x880); struct.pack_into('<I', ctl, 0x704, len(code))
    return bytes(ctl), code


def staged(kind):   # an NVN2 shader whose control section names its stage (+0x56)
    ctl, code = fz.stage(0x0461); ctl = bytearray(ctl); ctl[0x56] = kind
    return bytes(ctl), code


def bahs(shaders):   # an agl BAHS of the shaders given, one program entry each (fuzz_shader_split.bahs, with any control length)
    d = bytearray(0x100); d[:4] = b'BAHS'
    region = 0x100 + sum(len(ctl) for ctl, _ in shaders); c = 0x100
    for i, (ctl, code) in enumerate(shaders):
        ent = 0x58 + 0x20 * i
        struct.pack_into('<IIIi', d, ent, 0x100 * i, 0, len(ctl), c - ent); c += len(ctl)
    for ctl, _ in shaders: d += ctl
    area = bytearray(0x100 * len(shaders))
    for i, (_, code) in enumerate(shaders): area[0x100 * i:0x100 * i + len(code)] = code
    struct.pack_into('<II', d, 0x18, len(area), region)
    return bytes(d + area)


class Nvn2Only(unittest.TestCase):
    def setUp(self): self.addCleanup(ss.BAHS_VAR.clear)

    def test_the_games_stages_are_read(self):
        data, _ = fz.bnsh_of([[(8, *fz.VS), (0x28, *fz.PS), (0x40, *staged(6)), (0x48, *staged(7))]])
        self.assertEqual([r[1] for r in ss.bnsh_shaders(data, 0, '/a.bfsha')], ['vs', 'ps', 'ts', 'ms'])
        self.assertEqual([r[1] for r in ss.bahs_shaders(bahs([fz.VS, fz.PS, staged(5)]), '/a.sharcb')], ['vs', 'ps', 'cs'])
        self.assertEqual(set(ss.STAGE_NAME), {'vs', 'ps', 'cs', 'ts', 'ms'})

    def test_an_nvn1_shader_in_a_bnsh_stops_the_split(self):
        data, _ = fz.bnsh_of([[(8, *fz.VS)], [(8, *nvn1(0x0461))]])
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /old\.bnsh BNSH @0x0 variation 1 vs: not an NVN2 control section \(version 0xc9\): an NVN1 \(Maxwell\) shader'):
            ss.bnsh_shaders(data, 0, '/old.bnsh')

    def test_an_nvn1_entry_in_a_bahs_stops_the_split(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /old\.sharcb#BAHS@0x78: an NVN1 \(Maxwell\) shader entry: this chain translates only NVN2'):
            ss.bahs_shaders(bahs([fz.VS, nvn1(0x1462)]), '/old.sharcb')

    def test_hull_domain_and_geometry_stop_the_split(self):
        for slot, sph, word in ((0x10, 0x0861, 'hull'), (0x18, 0x0c61, 'domain'), (0x20, 0x1061, 'geometry')):
            with self.subTest(stage=word):
                data, _ = fz.bnsh_of([[(8, *fz.VS), (slot, *fz.stage(sph))]])
                with self.assertRaisesRegex(SystemExit, rf'^\[RED\] /t\.bnsh BNSH @0x0 variation 0: a {word} shader: this chain translates only'): ss.bnsh_shaders(data, 0, '/t.bnsh')
                with self.assertRaisesRegex(SystemExit, rf'^\[RED\] /t\.sharcb#BAHS@0x78: a {word} shader'): ss.bahs_shaders(bahs([fz.VS, fz.stage(sph)]), '/t.sharcb')

    def test_a_ray_tracing_module_stops_the_split(self):
        data, _ = fz.bnsh_of([[(8, *fz.VS), (0x38, *staged(8))]])
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /rt\.bnsh BNSH @0x0 variation 0: a ray tracing shader'): ss.bnsh_shaders(data, 0, '/rt.bnsh')
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] /rt\.sharcb#BAHS@0x78: a ray tracing shader'): ss.bahs_shaders(bahs([fz.VS, staged(8)]), '/rt.sharcb')

    def test_a_file_scan_stops_where_an_unreadable_file_is_left_out(self):   # scan_file's net (a file no reader reads is left out, test_split_unreadable.py) does not take these
        with tempfile.TemporaryDirectory() as t:
            for name, data, why in (('old.bnsh', fz.bnsh_of([[(8, *nvn1(0x0461))]])[0], r'an NVN1 \(Maxwell\) shader'), ('old.sharcb', bahs([nvn1(0x0461)]), r'an NVN1 \(Maxwell\) shader entry'),
                                    ('geo.bnsh', fz.bnsh_of([[(8, *fz.VS), (0x20, *fz.stage(0x1061))]])[0], 'a geometry shader')):
                path = Path(t) / name; path.write_bytes(data)
                with self.subTest(name=name), self.assertRaisesRegex(ss.Refused, r'^\[RED\] /' + re.escape(name) + '.*' + why): ss.scan_file((str(path), name))
            cut = Path(t) / 'cut.bnsh'   # a damaged file: left out with its red line
            cut.write_bytes((REPO / 'dm/tools/fuzz/corpus/shader_split/bnsh-far-pointer.bin').read_bytes())
            with contextlib.redirect_stdout(io.StringIO()) as said: got = ss.scan_file((str(cut), 'cut.bnsh'))
            self.assertEqual(got[3][0], 'unreadable'); self.assertIn('[RED] ', said.getvalue())

    def test_the_split_ends_with_the_red_line(self):   # the whole tool, the file read in a pool worker
        with tempfile.TemporaryDirectory() as t:
            t = Path(t); rom = t / 'RomFS'; rom.mkdir(); (rom / 'old.bnsh').write_bytes(fz.bnsh_of([[(8, *fz.VS)], [(8, *nvn1(0x0461))]])[0])
            main = t / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))
            out = [t / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *out], capture_output=True, text=True, env=dict(os.environ, DM_GAME='0400486024DEC000', J='2'))
            self.assertNotEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertRegex(r.stdout + r.stderr, r'\[RED\] /old\.bnsh BNSH @0x0 variation 1 vs: not an NVN2 control section \(version 0xc9\): an NVN1 \(Maxwell\) shader')
            self.assertFalse(out[1].exists(), 'no map is written from a split that met such a shader')


if __name__ == '__main__':
    unittest.main()
