"""build_app.py --work-dir / WORK_DIR: the build folder is the folder given, as it is; without it the build folder is <title ID>_build in the NUP's folder.
A derived profile goes in <build folder>/profile, or where --profile-out / PROFILE_OUT says. The run stops at the prerequisite check; no game files or Xcode needed."""
import importlib.util
import os
from pathlib import Path
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games'))
import game
spec = importlib.util.spec_from_file_location('build_app', REPO / 'scripts/build_app.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_app.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)

TID = 0x0400486024DEC000   # the game's profile in dm/games
VER = 131072   # the version it registers


class Stop(Exception): pass


class WorkDir(unittest.TestCase):
    def setUp(self):
        td = tempfile.TemporaryDirectory(); self.addCleanup(td.cleanup)
        self.tmp = Path(td.name).resolve()
        self.nups = self.tmp / 'nups'; self.nups.mkdir()
        self.nup = self.nups / 'game.nup'; self.nup.write_bytes(b'NUP\0' + bytes(60))

    def plan(self, *opts, setting='', given=None):
        """What main decides before it unpacks anything: DM_ROOT (the build folder) and what the console said"""
        fam = {'base': None, 'updates': [(VER, types.SimpleNamespace(name='update'))], 'dlc': [], 'nup': None}
        args = nsp.parse_args([str(given or self.nup), *opts])
        said, seen = [], {}
        def stop(env, *a): seen.update(env); raise Stop()
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'scan_nup', return_value=(TID, fam)), \
             mock.patch.object(nsp, 'apple_silicon', return_value=True), mock.patch.object(nsp, 'check_native_python'), \
             mock.patch.object(nsp, 'WORK_DIR', setting), mock.patch.object(nsp, 'check_prereqs', side_effect=stop), \
             mock.patch.object(nsp, 'say', side_effect=said.append):
            with self.assertRaises(Stop): nsp.main(args)
        return Path(seen['DM_ROOT']), said

    def test_default(self):
        self.assertEqual(self.plan()[0], self.nups / f'{TID:016X}_build')
        self.assertEqual(self.plan(given=self.nups)[0], self.nups / f'{TID:016X}_build')   # the folder that holds the NUP, given in its place

    def test_option_moves_the_build_folder(self):
        out = self.tmp / 'builds' / 'mine'
        work, said = self.plan('--work-dir', str(out))
        self.assertEqual(work, out)
        self.assertIn(f'build folder (the unpacked game files in extract/, kept for the next build): {out}', said)

    def test_setting_does_the_same(self):
        out = self.tmp / 'elsewhere'
        self.assertEqual(self.plan(setting=str(out))[0], out)

    def test_out_must_be_an_app(self):
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'apple_silicon', return_value=True), mock.patch.object(nsp, 'check_native_python'), \
             mock.patch.object(nsp, 'scan_nup', return_value=(TID, {'base': None, 'updates': [(VER, types.SimpleNamespace(name='update'))], 'dlc': [], 'nup': None})), \
             mock.patch.object(nsp, 'check_prereqs', side_effect=Stop), mock.patch.object(nsp, 'say'):
            with self.assertRaises(nsp.BuildError) as e: nsp.main(nsp.parse_args([str(self.nup), '-o', str(self.tmp / 'x.ipa')]))
            self.assertIn('must end in .app, or be a folder that exists', str(e.exception))
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(self.nup), '-o', str(self.tmp / 'x.app')]))
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(self.nup), '-o', str(self.nups)]))   # a folder that exists: the app goes in it, named after the game
            with self.assertRaises(nsp.BuildError): nsp.main(nsp.parse_args([str(self.nup), '-o', str(self.tmp / 'no such folder')]))
            with self.assertRaises(Stop): nsp.main(nsp.parse_args([str(self.nup), '--platform', 'mac', '-o', str(self.tmp / 'x.ipa')]))   # a development build makes no app: not looked at

    def test_another_game_stops(self):
        with mock.patch.object(nsp, 'find_repo', return_value=REPO), mock.patch.object(nsp, 'apple_silicon', return_value=True), mock.patch.object(nsp, 'check_native_python'), \
             mock.patch.object(nsp, 'scan_nup', return_value=(0x0100000000010000, {'base': None, 'updates': [], 'dlc': [], 'nup': None})), mock.patch.object(nsp, 'say'):
            with self.assertRaises(nsp.BuildError) as e: nsp.main(nsp.parse_args([str(self.nup)]))
        self.assertIn('no game profile for 0100000000010000', str(e.exception))

    def test_profile_folder(self):
        args = lambda *o: nsp.parse_args([str(self.nup), *o])
        work = self.tmp / 'w'
        with mock.patch.object(nsp, 'WORK_DIR', ''), mock.patch.object(nsp, 'PROFILE_OUT', ''):
            self.assertEqual(nsp.profile_folder(args(), work), work / 'profile')
            self.assertFalse(work.exists())   # nothing is made
            elsewhere = self.tmp / 'p'
            self.assertEqual(nsp.profile_folder(args('--profile-out', str(elsewhere)), work), elsewhere)
            with mock.patch.object(nsp, 'PROFILE_OUT', str(elsewhere)):   # the setting does the same as the option
                self.assertEqual(nsp.profile_folder(args(), work), elsewhere)

    def test_the_builds_own_profile_is_not_taken_as_registered(self):
        # a derived profile in the folder this script writes them to, reached through DM_GAMES_DIR (DM_GAMES_FIRST as a derived build sets it): main would take it for a
        # registered one, and derive it onto itself
        own = self.nups / f'{TID:016X}_build' / 'profile'
        (own / f'{TID:016X}').mkdir(parents=True)
        mine = game.by_title(TID); assert mine, 'no profile for the game'
        text = (mine.dir / 'game.tsv').read_text(encoding='utf-8')
        (own / f'{TID:016X}' / 'game.tsv').write_text(text + 'own_copy\tyes\n', encoding='utf-8')
        with mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(own), 'DM_GAMES_FIRST': '1'}), mock.patch.object(nsp, 'PROFILE_OUT', ''), mock.patch.object(nsp, 'WORK_DIR', ''):
            found = game.by_title(TID); assert found
            self.assertEqual(found.get('own_copy', ''), 'yes')   # what the lookup finds without the rule
            folders = nsp.own_profile_folders(nsp.parse_args([str(self.nup)]), self.nups, TID)
            self.assertIn(own, folders)
            self.assertIn(own, nsp.own_profile_folders(nsp.parse_args([str(self.nup), '--work-dir', str(self.tmp / 'w')]), self.nups, TID))   # a profile from before --work-dir too
            self.assertIn((self.tmp / 'w' / 'profile').resolve(), nsp.own_profile_folders(nsp.parse_args([str(self.nup), '--work-dir', str(self.tmp / 'w')]), self.nups, TID))
            kept = game.by_title(TID, folders); assert kept
            self.assertEqual(kept.get('own_copy', ''), '')   # dm/games' profile, not the own copy
            self.assertEqual(kept.dir.resolve(), (REPO / 'dm/games' / f'{TID:016X}').resolve())
            _, said = self.plan()
            self.assertIn(f'game profile: {TID:016X}', said)


if __name__ == '__main__':
    unittest.main()
