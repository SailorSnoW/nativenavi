# The build folder (build_app.py build_folder): <title ID>_build in the NUP's folder, or --work-dir / WORK_DIR used as given
import importlib.util
from pathlib import Path
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_app', REPO / 'scripts/build_app.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
NUP_DIR = Path('/games/oot')


def args(work_dir=None, **kw):
    return types.SimpleNamespace(work_dir=work_dir, **kw)


class BuildFolder(unittest.TestCase):
    def setUp(self):   # the Spyder settings block's WORK_DIR stays out of it
        patch = mock.patch.object(nsp, 'WORK_DIR', ''); patch.start(); self.addCleanup(patch.stop)

    def test_default_is_beside_the_nup(self):
        self.assertEqual(nsp.build_folder(args(), NUP_DIR, '0400486024DEC000'), NUP_DIR / '0400486024DEC000_build')

    def test_work_dir_is_used_as_given(self):
        self.assertEqual(nsp.build_folder(args('/b/w'), NUP_DIR, '0400486024DEC000'), Path('/b/w').resolve())

    def test_the_setting_names_it_too(self):
        with mock.patch.object(nsp, 'WORK_DIR', '/b/setting'):
            self.assertEqual(nsp.build_folder(args(), NUP_DIR, '0400486024DEC000'), Path('/b/setting').resolve())
            self.assertEqual(nsp.build_folder(args('/b/w'), NUP_DIR, '0400486024DEC000'), Path('/b/w').resolve())   # the command line wins


if __name__ == '__main__':
    unittest.main()
