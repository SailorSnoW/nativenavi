"""The shader package's records carry their keys, and dmsh_build.py finds a map row's record by its key, not by the row's index (issue #1841: a package from an older shader_split.py run
than the map translated every row after a dropped one from its neighbour's bytes; Ocarina of Time's present pass ran a terrain pixel shader and showed black). shader_split.py writes the
key at record +0x08; dmsh_build.py's record_index indexes a package by it and rec_of stops on a key the package does not hold. No game files."""
import ast
import os
from pathlib import Path
import re
import struct
import subprocess
import sys
import tempfile
import threading
from typing import Any
import unittest

REPO = Path(__file__).resolve().parents[3]
TOOLS = REPO / 'dm/gfx/tools'
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz  # noqa: E402


def record(key, body=b''):   # a record as shader_split.py writes it: the marker, the key at +0x08, a header to +0x38, then the shader's bytes
    return b'BTCDBTCD' + bytes.fromhex(key) + bytes(0x28) + body


def ndsr(stages):   # an NDSR pack (agl's upscaler pack) of minimal NVN2 shaders, one per SPH word 0
    data = bytearray(0x40); data[:4] = b'NDSR'
    for i, sph in enumerate(stages):
        ctl = bytearray(0x48); ctl[:8] = bytes.fromhex('34127698c9000000')
        code = struct.pack('<I', sph) + bytes(0x7c)
        struct.pack_into('<I', ctl, 0x44, len(code))
        struct.pack_into('<4I', data, 0x10 + 16 * i, len(data), len(ctl) << 4, len(data) + len(ctl), len(code) << 4)
        data += ctl + code
    return bytes(data)


class Lookup(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
        nodes: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name in ('record_index', 'rec_of')]
        self.assertEqual(len(nodes), 2)
        self.ns: dict[str, Any] = dict(os=os, re=re, sys=sys, mmap=__import__('mmap'), packs={}, packs_lock=threading.Lock(), dm_dir=self.tmp.name, loc={},
                                     check_record=lambda key, name, rec: self.checked.append(key))   # the record's form is CheckRecord's below
        self.checked = []
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), self.ns)
        self.addCleanup(lambda: [os.close(fd) for fd, _ in self.ns['packs'].values()])

    def package(self, recs, name='ps_lib.bin'):
        (Path(self.tmp.name) / name).write_bytes(b''.join(recs)); self.ns['packs'].clear()

    def test_records_are_read_from_the_file_at_their_offsets(self):   # record_index keeps no package in memory (GitHub issue #1036); a record runs from its marker to the next, the last to the end
        recs = [record('11' * 8, b'\x01' * 40), record('22' * 8, b'\x02' * 7), record('33' * 8, b'\x03' * 100)]
        path = Path(self.tmp.name) / 'pkg.bin'
        for data, want in ((b'header' + b''.join(recs), {'11' * 8: recs[0], '22' * 8: recs[1], '33' * 8: recs[2]}), (b'', {})):
            path.write_bytes(data)
            fd, index = self.ns['record_index'](str(path))
            try: self.assertEqual({k: os.pread(fd, e - o, o) for k, (o, e) in index.items()}, want)
            finally: os.close(fd)

    def test_a_row_finds_its_record_by_key_whatever_its_index(self):   # #1841: the map's rows renumbered after a dropped shader, the package from the split before
        recs = [record('aa' * 8, b'a'), record('bb' * 8, b'b'), record('cc' * 8, b'c')]
        self.package(recs)
        self.ns['loc'].update({'aa' * 8: ('ps_lib.bin', 0), 'cc' * 8: ('ps_lib.bin', 1)})
        self.assertEqual(self.ns['rec_of']('cc' * 8), ('ps_lib.bin', recs[2]))
        self.assertEqual(self.ns['rec_of']('aa' * 8), ('ps_lib.bin', recs[0]))
        self.assertEqual(self.checked, ['cc' * 8, 'aa' * 8])   # every record read is checked (check_record)

    def test_a_key_the_package_does_not_hold_stops_the_build(self):   # a map row added by a later split
        self.package([record('aa' * 8, b'a')])
        self.ns['loc']['dd' * 8] = ('ps_lib.bin', 0)
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] ps_lib\.bin holds no record of shader d{16}, which the shader map names: the package and the map come from different '
                                                r'shader_split\.py runs; split the shaders again') as cm:
            self.ns['rec_of']('dd' * 8)
        self.assertNotIn('carry no keys', str(cm.exception.code))

    def test_a_package_from_before_records_carried_keys_stops_the_build(self):   # the old layout: 0x30 zero bytes after the marker
        self.package([b'BTCDBTCD' + bytes(0x30) + b'a', b'BTCDBTCD' + bytes(0x30) + b'b'])
        self.ns['loc']['aa' * 8] = ('ps_lib.bin', 0)
        with self.assertRaisesRegex(SystemExit, r'come from different shader_split\.py runs \(its records carry no keys: it was split before they did\)'):
            self.ns['rec_of']('aa' * 8)


class CheckRecord(unittest.TestCase):
    """dmsh_build.py check_record: a record the chain does not translate stops the build with a red line that names it; none is skipped"""
    def setUp(self):
        tree = ast.parse((TOOLS / 'dmsh_build.py').read_text())
        nodes: list[ast.stmt] = [n for n in tree.body if isinstance(n, ast.FunctionDef) and n.name == 'check_record']
        self.assertEqual(len(nodes), 1)
        sys.path.insert(0, str(TOOLS)); import dmsh_ampere
        self.ns: dict[str, Any] = dict(sys=sys, struct=struct, dmsh_ampere=dmsh_ampere)
        exec(compile(ast.Module(body=nodes, type_ignores=[]), str(TOOLS / 'dmsh_build.py'), 'exec'), self.ns)
        self.check = self.ns['check_record']

    def rec(self, sph=0, kind=0, version=0xc9, size=0xe0):
        code = struct.pack('<I', sph) + bytes(0x7c) + bytes.fromhex('0f00000000000000') * 2
        ctl = bytearray(size); struct.pack_into('<II', ctl, 0, 0x98761234, version); struct.pack_into('<I', ctl, 0x44, len(code))
        if size > 0x56: ctl[0x56] = kind
        return record('ab' * 8, code + bytes(ctl))

    def test_the_games_stages_pass(self):
        for name, kw in (('vs_lib.bin', dict(sph=0x0461)), ('ps_lib.bin', dict(sph=0x1462)), ('cs_lib.bin', dict(kind=5)), ('ts_lib.bin', dict(sph=0x0461, kind=6)), ('ms_lib.bin', dict(sph=0x0461, kind=7))):
            with self.subTest(name=name): self.assertIsNone(self.check('ab' * 8, name, self.rec(**kw)))

    def test_an_nvn1_record_stops_the_build(self):   # Maxwell: control version 1, 0x880 bytes, a code header before the SPH
        nvn1 = record('ab' * 8, bytes.fromhex('78563412') + bytes(0x2c) + struct.pack('<I', 0x0461) + bytes(0x4c) + bytes.fromhex('34127698') + struct.pack('<I', 1) + bytes(0x878))
        for rec in (nvn1, self.rec(sph=0x0461, version=1), record('ab' * 8, b'no control section')):
            with self.assertRaisesRegex(SystemExit, r'^\[RED\] vs_lib\.bin: shader (ab){8} has no NVN2 control section .*is not skipped but stops the build'): self.check('ab' * 8, 'vs_lib.bin', rec)

    def test_a_ray_tracing_module_stops_the_build(self):
        with self.assertRaisesRegex(SystemExit, r'^\[RED\] cs_lib\.bin: shader (ab){8} is an NVN2 ray tracing module'): self.check('ab' * 8, 'cs_lib.bin', self.rec(kind=8))

    def test_a_stage_the_chain_has_not_stops_the_build(self):   # hull 2, domain 3, geometry 4 in SPH word 0, under any package name
        for sph in (0x0861, 0x0c61, 0x1061):
            for name in ('vs_lib.bin', 'ps_lib.bin', 'gs_lib.bin', 'hs_lib.bin', 'ds_lib.bin'):
                with self.subTest(sph=sph, name=name), self.assertRaisesRegex(SystemExit, r'^\[RED\] .*is not a .. shader \(control stage byte 0, SPH shader type [234]\)'): self.check('ab' * 8, name, self.rec(sph=sph))

    def test_a_record_in_another_stages_package_stops_the_build(self):
        for name, kw in (('vs_lib.bin', dict(sph=0x1462)), ('ps_lib.bin', dict(sph=0x0461)), ('vs_lib.bin', dict(sph=0x0461, kind=7)), ('cs_lib.bin', dict(sph=0x0461)), ('ms_lib.bin', dict(kind=6)), ('ts_lib.bin', dict(kind=5))):
            with self.subTest(name=name, kw=kw), self.assertRaisesRegex(SystemExit, r'^\[RED\] .*is not a .. shader'): self.check('ab' * 8, name, self.rec(**kw))


class Split(unittest.TestCase):
    def test_each_record_carries_the_key_of_its_map_row(self):   # the whole split of one NDSR pack (a vertex and a pixel shader)
        with tempfile.TemporaryDirectory() as t:
            t = Path(t); rom = t / 'RomFS'; (rom / 'Lib').mkdir(parents=True)
            (rom / 'Lib/agl_ndsr.pck').write_bytes(ndsr([0x0461, 0x1462]))
            main = t / 'main'; main.write_bytes(b'NSO0' + bytes(0xfc))   # an NSO header of three empty segments
            out = [t / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
            r = subprocess.run([sys.executable, TOOLS / 'shader_split.py', rom, main, *out], capture_output=True, text=True, env=dict(os.environ, J='1'))
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertNotIn('[RED]', r.stdout)
            rows = [ln.split('\t') for ln in out[1].read_text().splitlines() if not ln.startswith('#') and ln.split('\t')[0].endswith('.bin')]
            self.assertEqual(len(rows), 2)
            for name, k, _, key, _ in rows:
                recs = re.split(b'(?=BTCDBTCD)', (out[0] / name).read_bytes())[1:]
                rec = recs[int(k)]
                self.assertEqual(rec[8:16].hex(), key)
                self.assertEqual(rec.find(fz.ss.CTL_MAGIC, 0x38), 0x38 + struct.unpack_from('<I', rec, 0x2c)[0])   # the control section right after the code area, as before


if __name__ == '__main__':
    unittest.main()
