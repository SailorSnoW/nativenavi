"""shader_split: a file no reader can read is left out with a red line, and the split goes on instead of ending with nothing written. The damaged file here is a SARC
whose header is cut short, with a BAHS magic so that the scan opens it. No game files needed."""
import contextlib
import io
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import shader_split as ss

CUT = b'SARC' + bytes(4) + b'BAHS'   # a SARC header that ends after 8 of its 16 bytes
PACK = b'BAHS' + bytes(0x40)        # opened by bahs_shaders


class ScanFile(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(lambda: __import__('shutil').rmtree(self.tmp))

    def scan(self, path, rel):
        out = io.StringIO()
        with contextlib.redirect_stdout(out): r = ss.scan_file((str(path), rel))
        return r, out.getvalue()

    def test_red_line_of_a_reader(self):
        p = self.tmp / 'a.sarc'; p.write_bytes(CUT)
        (rel, keys, pairs, extra), out = self.scan(p, 'sub/a.sarc')
        self.assertEqual((rel, keys, pairs, extra[0]), ('sub/a.sarc', {}, set(), 'unreadable'))
        self.assertTrue(extra[1].startswith('/sub/a.sarc SARC header: '), extra[1])
        self.assertEqual(out.count('/sub/a.sarc'), 1); self.assertTrue(out.startswith('[RED] /sub/a.sarc SARC header: ') and out.endswith('(its shaders are left out)\n'))

    def test_exception_of_a_reader(self):
        p = self.tmp / 'a.sharcb'; p.write_bytes(PACK)
        with mock.patch.object(ss, 'bahs_shaders', side_effect=IndexError('list index out of range')):
            (_, _, _, extra), out = self.scan(p, 'a.sharcb')
        self.assertEqual(extra, ('unreadable', 'a.sharcb: IndexError: list index out of range', 1))
        self.assertEqual(out, '[RED] a.sharcb: IndexError: list index out of range (its shaders are left out)\n')

    def test_interrupt_is_not_taken(self):
        p = self.tmp / 'a.sharcb'; p.write_bytes(PACK)
        with mock.patch.object(ss, 'bahs_shaders', side_effect=KeyboardInterrupt), self.assertRaises(KeyboardInterrupt): self.scan(p, 'a.sharcb')

    def test_split_goes_on(self):   # the whole split: the unreadable archive is reported once more at the end, the tables are written and the split exits 0
        rom = self.tmp / 'RomFS'; (rom / 'sub').mkdir(parents=True)
        (rom / 'sub/a.sarc').write_bytes(CUT); (rom / 'b.bin').write_bytes(b'no shaders')
        main = self.tmp / 'main'
        main.write_bytes(b'NSO0' + bytes(0xfc))   # an NSO header of three empty segments
        out = [self.tmp / n for n in ('pkg', 'map.tsv', 'facts.tsv', 'pairs.tsv', 'pso.tsv')]
        r = subprocess.run([sys.executable, REPO / 'dm/gfx/tools/shader_split.py', rom, main, *out], capture_output=True, text=True, env=dict(os.environ, J='2'))
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertIn('\n[RED] 1 file(s) could not be read, their shaders are left out (first: /sub/a.sarc SARC header: ', '\n' + r.stdout)
        self.assertIn('shaders: 0 (', r.stdout)
        for p in out: self.assertTrue(p.exists(), p)


if __name__ == '__main__': unittest.main()
