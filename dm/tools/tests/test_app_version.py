"""The app's version (scripts/build_app.py app_display_version): the packed version's control data first, then the display version the profile registers for its
build, then the base game's control data; a profile row that is not a number gives nothing. And this game's v131072 row is 1.1.1. No game files needed."""
import importlib.util
from pathlib import Path
import sys
import tempfile
import types
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/games')]
import game
import nacp
spec = importlib.util.spec_from_file_location('build_app', REPO / 'scripts/build_app.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_app.py')
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
vars(nsp)['nacp'] = nacp   # main sets it


def folder(root: Path, name: str, version: str | None) -> Path:   # a version's unpacked folder, with a control.nacp whose display version is this, or none
    d = root / name; d.mkdir()
    if version is not None:
        data = bytearray(0x4000); data[0x3060:0x3060 + len(version)] = version.encode(); (d / 'control.nacp').write_bytes(data)
    return d


def profile(display: str) -> types.SimpleNamespace:
    return types.SimpleNamespace(builds=lambda: {'v131072': {'display': display, 'main': 'e0f3'}})


class AppVersion(unittest.TestCase):
    def test_order(self) -> None:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            upd, upd_bare, base, base_bare = folder(root, 'u', '1.2.0'), folder(root, 'ub', None), folder(root, 'b', '1.0.0'), folder(root, 'bb', None)
            v = nsp.app_display_version
            self.assertEqual(v(upd, base, profile('1.1.1'), 'v131072'), '1.2.0')        # the packed version's own control data
            self.assertEqual(v(upd_bare, base, profile('1.1.1'), 'v131072'), '1.1.1')   # no control data in the update: the registered row, not the base game's
            self.assertEqual(v(upd_bare, base, profile('v131072'), 'v131072'), '1.0.0')   # a row that is no number: the base game's
            self.assertEqual(v(upd_bare, base, profile('?'), 'v0'), '1.0.0')            # no row for the packed version
            self.assertEqual(v(upd_bare, base_bare, profile('?'), 'v131072'), '')       # nothing: the app says 1.0

    def test_this_game_registers_1_1_1(self) -> None:
        g = game.Game(REPO / 'dm/games/0400486024DEC000')
        self.assertEqual(g.builds()['v131072']['display'], '1.1.1')


if __name__ == '__main__':
    unittest.main()
