"""Tools that search a folder with glob find its files when a parent folder's name has brackets (`Game [0400486024DEC000][v131072]`, a usual NSP folder name): glob reads
an unescaped `[...]` as a character class and finds nothing, silently. The rewriter's registry is checked in test_patches.py; these are the matcher's registered cuts and the
host hooks read from the sources. No game files needed (temporary files only in a temporary folder)."""
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import nso_match   # noqa: E402
import nso_syms   # noqa: E402


class BracketedParent(unittest.TestCase):
    def setUp(self):
        tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, tmp)
        self.root = tmp / 'Game [0400486024DEC000][v131072]'

    def write(self, rel, text=''):
        p = self.root / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_text(text)
        return p

    def test_matcher_finds_a_game_files_registered_cuts(self):
        bid = '6a' * 16
        tool = self.write('dm/tools/nso_match.py'); self.write('dm/tools/sdk_patches.py')
        self.write('dm/games/0100000000000000/patches.py', f"CUTS = {{'{bid}': [(0x10, 'aa', 'bb')]}}\n")
        with mock.patch.object(nso_match, '__file__', str(tool)):
            self.assertEqual(nso_match.registered_cuts(bid), [(0x10, 'aa', 'bb')])

    def test_host_hooks_are_read_from_every_source_folder(self):
        self.write('dm/host/hle_a.c', 'HLE("nnA")\n'); self.write('dm/gfx/b.c', 'HLE("nnB")\n')
        self.write('dm/gfx/nvn/c.c', 'HLE_FORCE("nnC")\n')
        self.assertEqual(nso_syms.host_hooks(str(self.root / 'dm/host')), {'nnA', 'nnB', 'nnC'})


if __name__ == '__main__':
    unittest.main()
