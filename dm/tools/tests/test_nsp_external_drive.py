"""build_app.py: a NUP on an exFAT / FAT external drive. macOS writes an AppleDouble `._<name>` beside every file there: nup_file must not read one as the game's NUP, and rmtree
must treat an entry that vanished between the listing and its unlink (the `._x` that goes with `x`) as removed. No game files needed."""
import errno
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_app', REPO / 'scripts/build_app.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_app.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)


class ExternalDrive(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        cm = mock.patch.object(nsp, 'say', lambda m: None); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def test_the_folder_s_nup_skips_appledouble(self):
        nup = self.tmp / 'Game [0400486024dec000][v131072].nup'; nup.write_bytes(b'NUP\0' + bytes(60))
        (self.tmp / ('._' + nup.name)).write_bytes(b'\0\x05\x16\x07' + b'\0' * 4092)   # what macOS writes: 4 KB, no NUP
        self.assertEqual(nsp.nup_file(self.tmp), nup)   # with the `._` copy this would be a second .nup file
        self.assertEqual(nsp.nup_file(nup), nup)   # the file itself may be given

    def test_extension_in_any_letter_case(self):   # a download's `Game.NUP`: glob('*.nup') compares case-sensitively
        nup = self.tmp / 'Game.NUP'; nup.write_bytes(b'NUP\0' + bytes(60))
        self.assertEqual(nsp.nup_file(self.tmp), nup)

    def test_no_nup_or_several_stop(self):
        with self.assertRaises(nsp.BuildError) as e: nsp.nup_file(self.tmp)
        self.assertIn('no NUP', str(e.exception))
        other = self.tmp / 'notes.nup'; other.write_bytes(b'text')   # named like one, but not a NUP: does not count
        with self.assertRaises(nsp.BuildError): nsp.nup_file(self.tmp)
        with self.assertRaises(nsp.BuildError) as e: nsp.nup_file(other)
        self.assertIn('not a NUP file', str(e.exception))
        for n in ('a.nup', 'b.nup'): (self.tmp / n).write_bytes(b'NUP\0' + bytes(60))
        with self.assertRaises(nsp.BuildError) as e: nsp.nup_file(self.tmp)
        self.assertIn('more than one NUP', str(e.exception))

    def test_rmtree_tolerates_partner_vanishing(self):
        d = self.tmp / 'tree'; (d / 'sub').mkdir(parents=True)
        for n in ('a', '._a', 'sub/b', 'sub/._b'): (d / n).write_bytes(b'x')
        real = os.unlink

        def unlink(path, *a, **kw):   # deleting `x` takes `._x` with it (and the other way round), as on an exFAT volume
            real(path, *a, **kw)
            head, name = os.path.split(path); partner = os.path.join(head, name[2:] if name.startswith('._') else '._' + name)   # a bare name under dir_fd, a full path otherwise
            try: real(partner, *a, **kw)
            except FileNotFoundError: pass
        with mock.patch('os.unlink', unlink), mock.patch('os.remove', unlink):
            nsp.rmtree(d)
        self.assertFalse(d.exists())

    def test_rmtree_reports_other_errors(self):
        d = self.tmp / 'tree'; d.mkdir(); (d / 'a').write_bytes(b'x')
        def unlink(path, *a, **kw): raise PermissionError(errno.EACCES, 'denied', str(path))
        with mock.patch('os.unlink', unlink), mock.patch('os.remove', unlink):
            with self.assertRaises(PermissionError): nsp.rmtree(d)
        self.assertTrue(d.exists())


if __name__ == '__main__':
    unittest.main()
