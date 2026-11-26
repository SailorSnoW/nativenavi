"""build_app.py's reuse across builds of a derived profile and of the registered one. Real fingerprints and mock tool runs; no game files or Xcode needed."""
import importlib.util
import os
from pathlib import Path
import shutil
import sys
import tempfile
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/games')); sys.path.insert(0, str(REPO / 'dm/tools')); sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import game, nacp
spec = importlib.util.spec_from_file_location('build_app', REPO / 'scripts/build_app.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
nsp.nacp = nacp   # main sets it

TID = 0x0100AAAA00000000
BID, BID2 = 'ab' * 16, 'cd' * 16


def nso(path, bid):   # an NSO header: the build ID at +0x40
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(b'NSO0' + bytes(0x3c) + bytes.fromhex(bid) + bytes(0x50))


def control(path, disp):   # control.nacp with the display version at 0x3060
    path.parent.mkdir(parents=True, exist_ok=True); path.write_bytes(bytes(0x3060) + disp.encode().ljust(0x10, b'\0') + bytes(0xa0))


def put(g, **rows):   # rows written into a derived profile's game.tsv after it was derived (a key's row replaced, or added); the profile read again
    p = g.table('game.tsv'); lines = [l for l in p.read_text().splitlines() if l.split('\t', 1)[0] not in rows]
    p.write_text('\n'.join(lines + [f'{k}\t{v}' for k, v in rows.items()]) + '\n')
    return game.Game(g.dir)


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()).resolve(); self.addCleanup(shutil.rmtree, self.tmp)
        self.src = self.tmp / 'games' / f'{TID:016X}'; self.src.mkdir(parents=True)
        (self.src / 'game.tsv').write_text('\n'.join([f'title_id\t{TID:016X}', f'program_id\t{TID:016X}', 'name\tTest Game', 'modules\tmain', 'app_category\tpublic.app-category.games',
                                                      'romfs_marker\ta/b', f'build\tv0\t1.0.0\tmain={BID}', 'package\tv0', 'shader_versions\tv0']) + '\n')
        (self.src / 'shader_map.tsv').write_text('# registered\npkg.bin\t0\tvertex\tregistered\tx\n')
        (self.src / 'shader_facts_manual.tsv').write_text('k\t{"manual": 1}\n')
        (self.src / 'pso_manifest.tsv').write_text('S\tk\tvertex\n')
        (self.src / 'dmsh_chain.h').write_text('#define DMSH_CHAIN "old"\n')
        self.ex = self.tmp / 'extract/v65536'; nso(self.ex / 'ExeFS/main', BID2); control(self.ex / 'control.nacp', '1.1.0')
        (self.ex / 'RomFS').mkdir(); (self.ex / 'RomFS/a.bin').write_bytes(b'romfs')
        self.pdir = self.tmp / 'derived' / f'{TID:016X}'
        self.imported = {}   # what main imports from modules the list leaves out (main_modules: these NSOs are headers only)
        for cm in (mock.patch.dict(os.environ, {'DM_GAMES_DIR': str(self.tmp / 'games')}), mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None),
                   mock.patch.object(nsp, 'main_modules', lambda exefs, mods: self.imported)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def derive(self, ver='v65536', mods=('main',)):
        return nsp.derive_profile(game.Game(self.src), self.pdir, ver, self.ex, list(mods))

    def redump(self, path):   # identical bytes and size, but a fresh extraction or capture timestamp
        before = path.stat()
        os.utime(path, ns=(before.st_atime_ns, before.st_mtime_ns + 1_000_000_000))


class DeriveProfile(Base):
    def later_stages(self):   # what comes into the derived profile after it was derived: the shader split's tables, a row added by hand
        put(game.Game(self.pdir), mem_used='0x4800000')
        (self.pdir / 'shader_map.tsv').write_text('# split\npkg.bin\t0\tvertex\tderived\tx\n')

    def assert_kept(self):
        self.assertEqual(game.Game(self.pdir).get('mem_used'), '0x4800000')
        self.assertIn('derived', (self.pdir / 'shader_map.tsv').read_text())

    def assert_fresh(self):
        self.assertNotIn('mem_used', [k for k, _ in game.Game(self.pdir).rows])
        self.assertFalse((self.pdir / 'shader_map.tsv').exists())

    def test_unchanged_inputs_keep_later_stages(self):
        self.assertEqual(self.derive(), '1.1.0')
        self.later_stages()
        self.assertEqual(self.derive(), '1.1.0')
        self.assert_kept()
        self.assertEqual(game.Game(self.pdir).builds()['v65536']['main'], BID2)
        self.assertEqual(self.derive(), '1.1.0'); self.assert_kept()   # still kept on a third run

    def test_a_module_the_version_does_not_ship_is_left_out(self):   # a version without the subsdk1 the profile's build loads
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text().replace('modules\tmain\n', 'modules\tmain subsdk1 sdk\n'))
        nso(self.ex / 'ExeFS/sdk', BID)
        self.derive(mods=('main', 'subsdk1', 'sdk'))
        g = game.Game(self.pdir)
        self.assertEqual(g.words('modules'), ['main', 'sdk'])
        self.assertIn('# this version ships no subsdk1', (self.pdir / 'game.tsv').read_text())
        self.assertNotIn('subsdk1', g.builds()['v65536'])
        nso(self.ex / 'ExeFS/subsdk1', BID2); self.derive(mods=('main', 'subsdk1', 'sdk'))   # a version that ships it keeps the profile's list
        self.assertEqual(game.Game(self.pdir).words('modules'), ['main', 'subsdk1', 'sdk'])

    def test_a_module_main_imports_that_the_list_leaves_out_is_added(self):   # a module renamed in another version: subsdk2 shipped as subsdk0, and no subsdk2
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text().replace('modules\tmain\n', 'modules\tmain subsdk2 sdk\n'))
        nso(self.ex / 'ExeFS/sdk', BID); nso(self.ex / 'ExeFS/subsdk0', 'ee' * 16); nso(self.ex / 'ExeFS/subsdk1', 'ff' * 16)
        self.imported = {'subsdk0': 'NintendoSDK_movie'}
        self.derive(mods=('main', 'subsdk2', 'sdk'))
        g = game.Game(self.pdir); text = (self.pdir / 'game.tsv').read_text()
        self.assertEqual(g.words('modules'), ['main', 'subsdk0', 'sdk'])
        self.assertEqual(g.build_modules()[BID2], ['main', 'subsdk0', 'sdk'])   # the host loads what the row names; subsdk1 stays out
        self.assertIn('# this version ships no subsdk2', text)
        self.assertIn('# main imports from subsdk0 (NintendoSDK_movie), which the profile does not list', text)
        self.assertEqual(g.builds()['v65536']['subsdk0'], 'ee' * 16)
        self.assertNotIn('subsdk1', g.builds()['v65536'])

    def test_nothing_imported_keeps_the_list(self):
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text().replace('modules\tmain\n', 'modules\tmain sdk\n'))
        nso(self.ex / 'ExeFS/sdk', BID); nso(self.ex / 'ExeFS/subsdk0', 'ee' * 16)
        self.derive(mods=('main', 'sdk'))
        g = game.Game(self.pdir)
        self.assertEqual(g.words('modules'), ['main', 'sdk'])
        self.assertNotIn('# main imports', (self.pdir / 'game.tsv').read_text())
        self.assertNotIn('subsdk0', g.builds()['v65536'])

    def test_shader_tables_alone_add_no_module(self):
        self.imported = {'subsdk0': 'NintendoSDK_movie'}
        nsp.derive_profile(game.Game(self.src), self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        self.assertEqual(game.Game(self.pdir).words('modules'), ['main'])

    def test_a_profile_is_never_derived_onto_its_own_folder(self):
        own = self.tmp / 'profile' / f'{TID:016X}'   # this script's own profile, where its derived copy would go
        shutil.copytree(self.src, own)
        before = {p.name: p.read_bytes() for p in own.iterdir()}
        with self.assertRaises(nsp.BuildError):   # deriving onto itself would delete the folder's files, then copy them from themselves
            nsp.derive_profile(game.Game(own), own, 'v65536', self.ex, ['main'], shader_only=True)
        self.assertEqual({p.name: p.read_bytes() for p in own.iterdir()}, before)

    def test_the_same_profile_in_another_checkout_keeps_later_stages(self):
        self.derive(); self.later_stages()
        other = self.tmp / 'checkout2' / 'games' / f'{TID:016X}'   # the repository cloned again, or a front end's own copy of it
        shutil.copytree(self.src, other)
        nsp.derive_profile(game.Game(other), self.pdir, 'v65536', self.ex, ['main'])
        self.assert_kept()
        self.assertNotIn(str(self.tmp), (self.pdir / 'game.tsv').read_text())

    def test_changed_inputs_derive_again(self):
        changes = {
            'source game.tsv': lambda: (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text() + 'core_preempt\toff\n'),
            'source manual table': lambda: (self.src / 'shader_facts_manual.tsv').write_text('k\t{"manual": 2}\n'),
            'main build': lambda: nso(self.ex / 'ExeFS/main', 'ef' * 16),
            'display version': lambda: control(self.ex / 'control.nacp', '1.2.0'),
        }
        for what, change in changes.items():
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                change(); self.derive(); self.assert_fresh()
                for p in self.src.iterdir():
                    if p.name not in nsp.DERIVED_SKIP: self.assertEqual((self.pdir / p.name).read_bytes(), p.read_bytes())
        self.assertEqual(game.Game(self.pdir).builds()['v65536'], {'display': '1.2.0', 'main': 'ef' * 16})

    def test_changed_arguments_derive_again(self):
        nso(self.ex / 'ExeFS/sdk', BID)   # in the build row only when the modules name it
        for what, kw in (('version', {'ver': 'v131072'}), ('modules', {'mods': ('main', 'sdk')})):
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                self.derive(**kw); self.assert_fresh()   # pyright: ignore[reportArgumentType]  (each keyword set has its own value type)

    def test_a_file_the_split_does_not_read_refreshes_the_copies_and_keeps_later_stages(self):   # an edit of a table the split does not read keeps the split's tables
        changes = {
            'source chain header': lambda: (self.src / 'dmsh_chain.h').write_text('#define DMSH_CHAIN "new"\n'),
            'new source file': lambda: (self.src / 'bf_want.tsv').write_text('# new\n'),
            'edited source file': lambda: (self.src / 'pso_manifest.tsv').write_text('S\tk3\tpixel\n'),
        }
        for what, change in changes.items():
            with self.subTest(what):
                self.derive(); self.later_stages(); self.derive(); self.assert_kept()
                change(); self.derive(); self.assert_kept()
                for p in self.src.iterdir():
                    if p.name not in nsp.DERIVED_SKIP: self.assertEqual((self.pdir / p.name).read_bytes(), p.read_bytes())
                self.assertEqual(self.derive(), '1.1.0'); self.assert_kept()   # and the next run reuses the folder as it is

    def test_removed_source_file_goes(self):
        self.derive(); self.later_stages(); self.derive(); self.assert_kept()
        (self.src / 'pso_manifest.tsv').unlink(); self.derive()
        self.assertFalse((self.pdir / 'pso_manifest.tsv').exists()); self.assert_kept()

    def test_no_split_key_derives_again(self):   # a folder derived before the split key, or one whose split key is gone
        self.derive(); self.later_stages()
        nsp.derived_split_key(self.pdir).unlink()
        (self.src / 'pso_manifest.tsv').write_text('S\tk3\tpixel\n'); self.derive(); self.assert_fresh()

    def test_edited_copy_is_kept_until_inputs_change(self):   # the stored digest is of the inputs, not of the derived files
        self.derive(); (self.pdir / 'pso_manifest.tsv').write_text('S\tk2\tpixel\n'); self.derive()
        self.assertEqual((self.pdir / 'pso_manifest.tsv').read_text(), 'S\tk2\tpixel\n')

    def test_shader_only_derivation_preserves_builds_and_registered_patch_file(self):
        source = self.src / 'game.tsv'
        source.write_text(source.read_text() + f'build\tv65536\t1.1.0\tmain={BID2}\nshader_versions\tv0 v65536\n')
        patch = self.src / 'patches.py'; patch.write_text(f'CUTS = {{"{BID}": []}}\n')
        g = game.Game(self.src)
        nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        derived = game.Game(self.pdir)
        self.assertEqual(derived.rows, g.rows)
        self.assertEqual(derived.builds(), g.builds())
        self.assertTrue((self.pdir / 'patches.py').is_symlink())
        self.assertEqual((self.pdir / 'patches.py').resolve(), patch.resolve())
        derived.table('shader_map.tsv').write_text('generated DLC shader rows\n')
        nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
        self.assertEqual(derived.table('shader_map.tsv').read_text(), 'generated DLC shader rows\n')

    def test_derived_main_without_frame_step_stubs_keeps_the_games_step(self):
        source = self.src / 'game.tsv'; source.write_text(source.read_text() + 'fps_target\t60\n')
        (self.src / 'patches.py').write_text(f'def frame_step(ctx): pass\nFRAME_STEP = {{"{BID}": frame_step}}\n')   # stubs for the registered main only, never run
        self.derive()
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '0')
        self.assertIn('frame step stubs for other builds only', (self.pdir / 'game.tsv').read_text())
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1, "{BID2}": 2}}\n')   # the derived main has stubs too
        self.derive(); self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text('CUTS = {}\n'); self.derive()   # no frame step in the profile
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1}}\n'); self.derive()
        nsp.derive_profile(game.Game(self.src), self.pdir, 'v65536', self.ex, ['main'], shader_only=True)   # shader tables only: the settings stay
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')
        (self.src / 'patches.py').write_text(f'FRAME_STEP = {{"{BID}": 1}}\nMATCH_FRAME_STEP = {{"main": []}}\n'); self.derive()   # stubs placed by function may serve it
        self.assertEqual(game.Game(self.pdir).get('fps_target'), '60')

    def test_missing_corrupt_and_duplicate_override_rows_derive_again(self):
        for change in ('missing', 'corrupt', 'override', 'indented override', 'truncated'):
            with self.subTest(change):
                self.derive(); p = self.pdir / 'game.tsv'; original = p.read_text()
                if change == 'missing': broken = original.replace('romfs_marker\ta/b\n', '')
                elif change == 'corrupt': broken = original.replace('romfs_marker\ta/b', 'romfs_marker\tx/y')
                elif change == 'override': broken = 'romfs_marker\tx/y\n' + original
                elif change == 'indented override': broken = ' romfs_marker\tx/y\n' + original
                else: broken = original[:original.index('romfs_marker')]
                p.write_text(broken)
                self.derive()
                self.assertEqual(p.read_text(), original)

    def test_repeated_source_rows_and_computed_additions_are_kept(self):
        p = self.src / 'game.tsv'; p.write_text(p.read_text() + 'extra\tone\nextra\tone\n')
        self.derive(); self.later_stages(); self.derive(); self.assert_kept()
        p = self.pdir / 'game.tsv'; p.write_text(p.read_text().replace('extra\tone\n', '', 1))
        self.derive()
        self.assertEqual(game.Game(self.pdir).values('extra'), [['one'], ['one']])

    def test_derived_replace_failure_preserves_original(self):
        self.derive(); p = self.pdir / 'game.tsv'; before = p.read_bytes()
        with mock.patch.object(nsp.os, 'replace', side_effect=OSError('interrupted replace')):
            with self.assertRaisesRegex(OSError, 'interrupted replace'): self.derive(ver='v131072')
        self.assertEqual(p.read_bytes(), before)

class DlcOnlyShaders(Base):
    def test_only_dlc_rows_and_facts_can_change(self):
        tables = self.tmp / 'new-tables'; tables.mkdir()
        base = 'pkg.bin\t8\tcompute\tbase\t/base.shader\n'
        dlc = 'pkg.bin\t0\tvertex\tdlc\t/aoc/5/new.shader\n'
        old_facts = 'base\t{"local":[8,1,1]}\ndlc\t{"other":1}\n'
        self.src.joinpath('shader_map.tsv').write_text(base + dlc)
        self.src.joinpath('shader_facts.tsv').write_text(old_facts)
        g = game.Game(self.src)
        cases = [
            ('removed DLC and reindexed base', base.replace('\t8\t', '\t0\t'), 'base\t{"local":[8,1,1]}\n', True),
            ('added DLC', base + dlc + 'pkg.bin\t9\tpixel\tnew-dlc\t/aoc/7/new.shader\n', old_facts, True),
            ('shaderless DLC', base + dlc, old_facts, False),
            ('changed base key', base.replace('\tbase\t', '\tother\t'), old_facts, False),
            ('changed base source', base.replace('/base.shader', '/different.shader'), old_facts, False),
            ('changed base facts', base, old_facts.replace('[8,1,1]', '[4,1,1]'), False),
            ('no DLC difference', base + dlc, old_facts.replace('[8,1,1]', '[4,1,1]'), False),
            ('invalid map', 'not a shader map\n', old_facts, False),
        ]
        for name, shader_map, facts, want in cases:
            with self.subTest(name=name):
                tables.joinpath('shader_map.tsv').write_text(shader_map)
                tables.joinpath('shader_facts.tsv').write_text(facts)
                self.assertEqual(nsp.dlc_only_shader_change(g, tables), want)

    def test_a_shader_the_dlc_shares_with_the_game(self):   # the split gives a shared shader the game's row (merge_keys), so both directions are DLC-only changes
        tables = self.tmp / 'new-tables'; tables.mkdir()
        game_rows = 'vs_sdf.bin\t0\tvertex\tbase\t/re_chunk_000.pak#1#SDF#var0\nvs_sdf.bin\t1\tvertex\tshared\t/re_chunk_000.pak#1#SDF#var619\n'
        dlc_rows = 'vs_other.bin\t0\tvertex\tdlc\t/aoc/1/re_dlc_000.pak#2#SDF#var1\n'
        facts = 'base\t{}\n'
        for name, profile, split, want in (
            ('registered with the DLC, packed without it', game_rows + dlc_rows, game_rows, True),
            ('registered without the DLC, packed with it', game_rows, game_rows + dlc_rows, True),
            ('registered under the old crediting (the shared shader as the DLC\'s), packed without it',   # such a profile is split again, not accepted
             game_rows.splitlines(True)[0] + dlc_rows + 'vs_other.bin\t1\tvertex\tshared\t/aoc/1/re_dlc_000.pak#2#SDF#var619\n', game_rows, False)):
            with self.subTest(name=name):
                self.src.joinpath('shader_map.tsv').write_text(profile); self.src.joinpath('shader_facts.tsv').write_text(facts)
                tables.joinpath('shader_map.tsv').write_text(split); tables.joinpath('shader_facts.tsv').write_text(facts)
                self.assertEqual(nsp.dlc_only_shader_change(game.Game(self.src), tables), want)


class SplitShaders(Base):
    NAMES = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')

    def setUp(self):
        super().setUp()
        self.repo = self.tmp / 'repo'
        for f in ('dm/gfx/tools/shader_split.py', 'dm/gfx/tools/dmsh_chain.lock', 'dm/games/game.py'):
            p = self.repo / f; p.parent.mkdir(parents=True, exist_ok=True); p.write_text('v1')
        self.pkg = self.tmp / 'work/shader/pkg'; self.logs = self.tmp / 'logs'; self.logs.mkdir()
        self.splits, self.rows, self.log = 0, 'derived', 'shaders: 3 (3 vertex)\n'
        cm = mock.patch.object(nsp, 'run', self.split); cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def split(self, cmd, log, env=None, cwd=None, on_line=None):   # shader_split.py: the package and the four tables, deterministic for the same rows
        self.splits += 1; self.last_cmd = cmd; out = Path(cmd[4]); out.mkdir(parents=True); (out / 'main.bin').write_bytes(self.rows.encode())
        for n, t in zip(self.NAMES, cmd[5:]): Path(t).write_text(self.table(n))
        Path(log).write_text(self.log)

    def table(self, n): return self.tables[n] if hasattr(self, 'tables') else f'# split header\n{n}\t{self.rows}\n'

    def go(self, g, own=True, dlc_roms=(), derive_dlc=None, env=None):
        return nsp.split_shaders(sys.executable, self.repo, [self.ex / 'RomFS'], [self.ex / 'ExeFS/main'], self.pkg, env or {}, self.logs, g, own, dlc_roms, derive_dlc)

    def test_dlc_only_derivation_is_incremental_and_keeps_registered_tables(self):
        base = 'other.bin\t1\tvertex\tbase\t/base.shader\n'
        old = {'shader_map.tsv': 'other.bin\t0\tvertex\tdlc\t/aoc/7/new.shader\n' + base,
               'shader_facts.tsv': 'base\t{"fact":1}\ndlc\t{"fact":2}\n',
               'shader_pairs.tsv': 'base\tdlc\n', 'pso_pairs.tsv': '# none\n'}
        for n in self.NAMES: (self.src / n).write_text(old[n])
        g = game.Game(self.src)
        self.tables = {'shader_map.tsv': base.replace('\t1\t', '\t0\t'), 'shader_facts.tsv': 'base\t{"fact":1}\n',
                       'shader_pairs.tsv': '# none\n', 'pso_pairs.tsv': '# none\n'}
        def derive():
            nsp.derive_profile(g, self.pdir, 'v65536', self.ex, ['main'], shader_only=True)
            return game.Game(self.pdir)
        promote = mock.Mock(side_effect=derive)
        first = self.go(g, own=False, derive_dlc=promote)
        self.assertEqual((self.splits, promote.call_count), (1, 1))
        self.assertEqual(self.go(g, own=False, derive_dlc=promote), first)
        self.assertEqual((self.splits, promote.call_count), (1, 2))   # proof of the strict non-DLC comparison is reused
        self.assertEqual(game.Game(self.pdir).rows, g.rows)
        for n in self.NAMES:
            self.assertEqual(g.table(n).read_text(), old[n])
            self.assertEqual(game.Game(self.pdir).table(n).read_text(), self.tables[n])
        # A new DLC extraction, even of the same file sizes, must perform the strict comparison again.
        dlc = self.tmp / 'aoc/7'
        dlc.mkdir(parents=True)
        asset = dlc / 'shader'
        asset.write_bytes(b'bytes')
        self.go(g, own=False, dlc_roms=[dlc], derive_dlc=promote); self.assertEqual(self.splits, 2)
        self.redump(asset)
        self.go(g, own=False, dlc_roms=[dlc], derive_dlc=promote); self.assertEqual(self.splits, 3)
        moved = dlc.with_name('8'); dlc.rename(moved)
        self.go(g, own=False, dlc_roms=[moved], derive_dlc=promote); self.assertEqual(self.splits, 4)   # index changes count even if every file statistic is identical
        verifier = self.tmp / 'comparison-v2.py'; verifier.write_text('changed comparison code')
        with mock.patch.object(nsp, '__file__', str(verifier)):
            self.go(g, own=False, dlc_roms=[moved], derive_dlc=promote); self.assertEqual(self.splits, 5)

    def test_base_shader_mismatch_never_derives_for_dlc(self):
        (self.src / 'shader_map.tsv').write_text('p.bin\t0\tvertex\tbase\t/base\np.bin\t1\tvertex\tdlc\t/aoc/7/shader\n')
        (self.src / 'shader_facts.tsv').write_text('# none\n')
        (self.src / 'shader_pairs.tsv').write_text('# none\n')
        self.tables = {'shader_map.tsv': 'p.bin\t0\tvertex\twrong-base\t/base\n', 'shader_facts.tsv': '# none\n',
                       'shader_pairs.tsv': '# none\n', 'pso_pairs.tsv': '# none\n'}
        promote = mock.Mock()
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'):
            self.go(game.Game(self.src), own=False, derive_dlc=promote)
        promote.assert_not_called()

    def test_dlc_shaders_are_passed_and_invalidate_the_split(self):
        g = self.own(); self.go(g); self.assertEqual(self.splits, 1)
        dlc = self.tmp / 'work/aoc/7'; dlc.mkdir(parents=True)
        asset = dlc / 'shader.archive'; asset.write_bytes(b'new shader')
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 2)
        self.assertEqual(self.last_cmd[-1], str(dlc))
        # Extra content has no main: the base RomFS/main pairing remains unchanged.
        self.assertEqual(self.last_cmd[2:4], [str(self.ex / 'RomFS'), str(self.ex / 'ExeFS/main')])
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 2)
        asset.write_bytes(b'updated DLC shader')
        self.go(g, dlc_roms=[dlc]); self.assertEqual(self.splits, 3)
        self.go(g); self.assertEqual(self.splits, 4)   # removing the DLC removes its shaders from the union

    def own(self):   # a derived profile with the empty tables, as main makes it
        self.derive(); g = game.Game(self.pdir); nsp.empty_tables(g); return g

    def test_own_profile_reuses_and_returns_the_package_key(self):
        g = self.own()
        k1 = self.go(g)
        self.assertEqual((self.splits, k1), (1, nsp.package_key(self.pkg)))
        for n in self.NAMES: self.assertEqual(g.table(n).read_text(), self.table(n))
        k2 = self.go(g)
        self.assertEqual((self.splits, k2), (1, k1))   # the key stored after the tables were written: the second run sees the same inputs

    def test_translation_and_profile_metadata_changes_keep_extraction(self):
        g = self.own(); before = self.go(g)
        for name in ('dmsh_build.py', 'dmsh_chain.py', 'pso_offline.py', 'unrelated_reader.py'):
            (self.repo / 'dm/gfx/tools' / name).write_text('version = 2\n')
            self.assertEqual(self.go(g), before)
            self.assertEqual(self.splits, 1, name)
        for name in ('README.md', 'bf_want.tsv', 'shader_facts.tsv.notes', 'dmsh_chain.h'):
            g.table(name).write_text('changed\n')
            self.assertEqual(self.go(g), before)
            self.assertEqual(self.splits, 1, name)
        g = put(g, mem_used='0x4800000', nvn_all_pulling='1')   # runtime settings
        self.assertEqual(self.go(g), before)
        self.assertEqual(self.splits, 1)

    def test_transitive_reader_dependency_changes_reextract(self):
        (self.repo / 'dm/gfx/tools/shader_split.py').write_text('import reader\n')
        (self.repo / 'dm/gfx/tools/reader.py').write_text('from reader_bits import FORMAT\n')
        helper = self.repo / 'dm/tools/reader_bits.py'; helper.parent.mkdir(parents=True)
        helper.write_text('FORMAT = 1\n')
        g = self.own(); self.go(g)
        helper.write_text('FORMAT = 2\n')
        self.go(g); self.assertEqual(self.splits, 2)
        helper.unlink()
        self.go(g); self.assertEqual(self.splits, 3)

    def test_extraction_settings_and_auxiliary_modules_reextract(self):
        g = self.own(); self.go(g)
        for i, (key, value) in enumerate((('shader_groups', 'foo=bar'), ('shader_note', 'Other sources'), ('modules', 'main sdk')), 2):
            g = put(g, **{key: value})
            self.go(g); self.assertEqual(self.splits, i)
        nso(self.ex / 'ExeFS/sdk', BID)
        self.go(g); self.assertEqual(self.splits, 5)
        nso(self.ex / 'ExeFS/sdk', BID2)
        self.go(g); self.assertEqual(self.splits, 6)

    def test_missing_or_modified_package_is_not_reused(self):
        g = self.own(); self.go(g)
        output = self.pkg / 'main.bin'
        output.unlink()
        self.go(g); self.assertEqual(self.splits, 2)
        self.assertEqual(output.read_bytes(), b'derived')
        output.write_bytes(b'corrupt')
        self.go(g); self.assertEqual(self.splits, 3)
        self.assertEqual(output.read_bytes(), b'derived')

    def test_redumped_inputs_invalidate_only_own_split(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        for own in (False, True):
            g = self.own() if own else game.Game(self.src)
            self.go(g, own=own)
            for p in [self.ex / 'RomFS/a.bin']:
                with self.subTest(own=own, path=p):
                    before = self.splits; self.redump(p); self.go(g, own=own)
                    self.assertEqual(self.splits, before + int(own))
                    self.go(g, own=own); self.assertEqual(self.splits, before + int(own))

    def test_own_profile_changes_split_again(self):
        changes = {'manual table': lambda g: g.table('shader_facts_manual.tsv').write_text('k\t{"manual": 3}\n'),
                   'main': lambda g: nso(self.ex / 'ExeFS/main', 'ef' * 16),
                   'RomFS': lambda g: (self.ex / 'RomFS/b.bin').write_bytes(b'more'),
                   'tool': lambda g: (self.repo / 'dm/gfx/tools/shader_split.py').write_text('v2')}
        g = self.own(); self.go(g)
        for i, (what, change) in enumerate(changes.items(), 2):
            with self.subTest(what):
                change(g); self.assertEqual(self.go(g), nsp.package_key(self.pkg)); self.assertEqual(self.splits, i)
                self.go(g); self.assertEqual(self.splits, i)

    def test_own_generated_tables_are_checked(self):   # an edited or missing table of the split is never taken as is: the split's output comes back (split again, or restored)
        g = self.own(); self.go(g)
        for n in self.NAMES:
            with self.subTest(edited=n):
                g.table(n).write_text('# edited\nstale\trow\n'); self.go(g)
                self.assertEqual(g.table(n).read_text(), self.table(n))
            with self.subTest(missing=n):
                g.table(n).unlink(); self.go(g)
                self.assertEqual(g.table(n).read_text(), self.table(n))
        runs = self.splits; self.go(g); self.assertEqual(self.splits, runs)

    def test_own_new_output_replaces_tables(self):   # a changed input whose split writes other rows: the profile takes them
        g = self.own(); self.go(g)
        self.rows = 'other'; (self.ex / 'RomFS/b.bin').write_bytes(b'more'); self.go(g)
        self.assertEqual(g.table('shader_map.tsv').read_text(), self.table('shader_map.tsv'))
        self.go(g); self.assertEqual(self.splits, 2)

    def test_unreadable_files_are_reported(self):   # the split leaves out a file no reader reads and goes on: a derived profile's build says so and goes on, the registered profile stops
        self.log = '[RED] 2 file(s) could not be read, their shaders are left out (first: /a.dict: not in the layout)\nshaders: 3 (3 vertex)\n'
        g, said = self.own(), []
        with mock.patch.object(nsp, 'say', said.append): self.go(g)
        self.assertEqual(len([m for m in said if m.startswith('[RED] 2 file(s) could not be read')]), 1)
        self.go(g); self.assertEqual(self.splits, 1)
        self.assertIn('2 file(s) unreadable', nsp.PROG.end.call_args[0][0])   # the reused split still names them
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        with self.assertRaisesRegex(nsp.BuildError, r'^2 file\(s\) could not be read'): self.go(game.Game(self.src), own=False)

    def test_mode_is_in_the_key(self):   # the same files split as an own profile, then checked against them as a registered one
        g = self.own(); self.go(g)
        self.assertEqual(self.go(g, own=False), nsp.package_key(self.pkg)); self.assertEqual(self.splits, 2)
        self.go(g, own=False); self.assertEqual(self.splits, 2)
        self.go(g); self.assertEqual(self.splits, 3)

    def test_registered_tables_are_compared(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(f'# older header\n{n}\t{self.rows}\n')
        g = game.Game(self.src)
        k = self.go(g, own=False)
        self.assertEqual((self.splits, k), (1, nsp.package_key(self.pkg)))
        self.assertEqual(self.go(g, own=False), k); self.assertEqual(self.splits, 1)
        self.assertEqual((self.src / 'shader_map.tsv').read_text(), '# older header\nshader_map.tsv\tderived\n')   # a registered profile's tables are not written
        self.rows = 'other'; (self.ex / 'RomFS/b.bin').write_bytes(b'more')
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'): self.go(g, own=False)
        self.assertEqual((self.src / 'shader_map.tsv').read_text(), '# older header\nshader_map.tsv\tderived\n')
        with self.assertRaisesRegex(nsp.BuildError, 'differ from game profile'): self.go(g, own=False)   # a failed check is not reused
        self.assertEqual(self.splits, 3)

    def test_damaged_reuses_only_the_same_extraction(self):
        # --allow-damaged: a repaired copy keeps every path and size, which is all 'tree' sees, so a damaged RomFS is keyed by its files' times as well ('stat': an extraction writes
        # them again) and by the damage check's list, so a damaged build reuses its split and its shaders (Ocarina of Time's three damaged textures would otherwise cost
        # every build the split and the restore of every shader)
        g = self.own()
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True):
            k = self.go(g); self.assertEqual(k, nsp.package_key(self.pkg))   # the shader step's key: its libraries are reused while the package is the same
            self.assertEqual(self.go(g), k); self.assertEqual(self.splits, 1)
            self.redump(self.ex / 'RomFS/a.bin')   # a repaired copy: the same size, extracted again
            self.go(g); self.assertEqual(self.splits, 2)
            self.go(g); self.assertEqual(self.splits, 2)
            (self.ex / 'RomFS.img.formats').write_text('1 2\nTex/a.bntx.zs\t10\t00000000\n')   # check_formats' list beside the image
            self.go(g); self.assertEqual(self.splits, 3)
        self.go(g); self.assertEqual(self.splits, 4)   # an undamaged run takes nothing a damaged one keyed, and the other way round
        with mock.patch.object(nsp, 'ALLOW_DAMAGED', True): self.go(g)
        self.assertEqual(self.splits, 5)

    def test_missing_split_pairs_are_generated_again(self):
        for n in self.NAMES[:3]: (self.src / n).write_text(self.table(n))
        g = game.Game(self.src)
        self.go(g, own=False)
        pairs = self.pkg.with_name('pso_pairs.tsv'); pairs.unlink()
        self.go(g, own=False)
        self.assertEqual(self.splits, 2)
        self.assertEqual(pairs.read_text(), self.table('pso_pairs.tsv'))
        pairs.write_text('corrupt\tpair\n')
        self.go(g, own=False)
        self.assertEqual(self.splits, 3)
        self.assertEqual(pairs.read_text(), self.table('pso_pairs.tsv'))

    def test_derived_rerun_is_incremental(self):   # the derived path of a second run: derive, the empty tables, a row written since, split
        g = self.own(); g = put(g, mem_used='0x4800000'); k = self.go(g)
        g = self.own(); self.assertEqual(g.get('mem_used'), '0x4800000')
        self.assertEqual(self.go(g), k); self.assertEqual(self.splits, 1)


class RewriteWorkers(Base):
    def test_explicit_workers_override_the_automatic_module_budget(self):
        g = game.Game(self.src)
        for value, expected in (('1', '1'), ('3', '3'), ('', '14')):
            with self.subTest(value=value):
                out = self.tmp / f'patched-{value or "auto"}'
                env = {'DM_GAME': str(self.src), 'DM_REWRITE_J': value}
                with mock.patch.object(nsp, 'patch_files', return_value=[]), \
                     mock.patch.object(nsp.os, 'cpu_count', return_value=14), mock.patch.object(nsp.time, 'sleep'), \
                     mock.patch.object(nsp.proc_group, 'Group', return_value=mock.Mock(p=mock.Mock(returncode=0, poll=lambda: 0), finish=lambda: 0, stragglers=[])) as popen:
                    nsp.rewrite(Path(sys.executable), REPO, self.ex / 'ExeFS', out, env, self.tmp / 'logs', g)
                self.assertEqual(popen.call_args.kwargs['env']['DM_REWRITE_J'], expected)
                self.assertEqual(env['DM_REWRITE_J'], value)

    def test_invalid_workers_fail_before_reading_the_modules(self):
        for value in ('0', '-1', 'many'):
            with self.subTest(value=value), mock.patch.object(nsp, 'patch_files') as read:
                with self.assertRaisesRegex(nsp.BuildError, 'DM_REWRITE_J must be a positive integer'):
                    nsp.rewrite(Path(sys.executable), REPO, self.ex / 'ExeFS', self.tmp / 'patched',
                                {'DM_REWRITE_J': value}, self.tmp / 'logs', game.Game(self.src))
                read.assert_not_called()


class MacApp(Base):
    def test_packer_reports_reuse_and_first_pack_steps(self):
        t = self.tmp; out = t / 'Game.app'; packs = []
        def pack(cmd, log, env=None, on_line=None):
            if not on_line: return   # the launcher settings (dm/games/settings.py)
            packs.append(cmd)
            (out / 'Contents/MacOS').mkdir(parents=True, exist_ok=True)
            (out / 'Contents/MacOS/dmhost').write_bytes(b'host')
            if len(packs) == 1: on_line('step: launcher and host (clones of the game files)')
            on_line(f'app: {out}' + (', reused (inputs unchanged)' if len(packs) > 1 else ''))
        with mock.patch.object(nsp, 'run', side_effect=pack):
            for i in range(2):
                result = nsp.mac_app(sys.executable, REPO, out, TID, t, self.ex, t / 'patched', t / 'host', t / 'font', t / 'shader', None, 'en-US',
                                     game.Game(self.src), 'Game', '1.0.0', 'org.nativenavi.test', None, '', {}, t)
                self.assertIsNone(result)
                if i: nsp.PROG.end.assert_called_with('Game.app, reused (inputs unchanged)')
                else: nsp.PROG.set.assert_called_with(step='launcher and host (clones of the game files)')
        self.assertEqual(len(packs), 2)   # pack_app owns the reuse decision and sidecar key


class ShaderProfileKey(Base):   # the shader step's key takes the profile's shader inputs (dmsh_chain.py --inputs), not every file's bytes
    def key(self):
        env = dict(os.environ, DM_GAME=f'{TID:016X}', DM_GAMES_DIR=str(self.tmp / 'games'))
        return nsp.fingerprint(nsp.shader_profile_key(sys.executable, REPO, env, game.Game(self.src)))

    def test_files_no_shader_build_reads_keep_the_key(self):   # a note edit restores no library
        before = self.key()
        (self.src / 'README.md').write_text('# a note\n')
        (self.src / 'pso_manifest.tsv').write_text('S\tk2\tpixel\n')
        (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text() + 'core_preempt\toff\n')
        self.assertEqual(self.key(), before)

    def test_shader_inputs_change_the_key(self):
        changes = {
            'shader map': lambda: (self.src / 'shader_map.tsv').write_text('# registered\npkg.bin\t0\tvertex\tother\tx\n'),
            'bf_want.tsv': lambda: (self.src / 'bf_want.tsv').write_text('a\n'),
            'an nvn_ key': lambda: (self.src / 'game.tsv').write_text((self.src / 'game.tsv').read_text() + 'nvn_test\ton\n'),
        }
        for what, change in changes.items():
            with self.subTest(what):
                before = self.key(); change(); self.assertNotEqual(self.key(), before)


class Route(Base):   # which versions build from the profile as it is, and what the end of the build says of the others
    def profile(self, *rows):
        (self.src / 'game.tsv').write_text('\n'.join([f'title_id\t{TID:016X}', 'name\tTest Game', 'modules\tmain', *rows]) + '\n')
        return game.Game(self.src)

    def test_a_registered_version_with_shaders_of_its_own_is_derived(self):   # Ocarina of Time's 1.0.0: its shader tables are not the update's
        g = self.profile(f'build\tv131072\t1.1.1\tmain={BID}', f'build\tv0\t1.0.0\tmain={BID2}', 'shader_versions\tv131072')
        both = {'v0': None, 'v131072': Path('u')}
        self.assertFalse(nsp.derived_reason(g, both, 'v131072'))
        self.assertIn('v0 (1.0.0) has shaders of its own', nsp.derived_reason(g, both, 'v0'))
        self.assertIn('has shaders of its own', nsp.derived_reason(g, {'v0': None}, 'v0'))
        self.assertIn('is not a version', nsp.derived_reason(g, {**both, 'v196608': Path('n')}, 'v196608'))

    def test_a_version_whose_tables_need_another(self):
        g = self.profile(f'build\tv65536\t1.1.0\tmain={BID}', 'shader_versions\tv0 v65536')
        self.assertFalse(nsp.derived_reason(g, {'v0': None, 'v65536': Path('u')}, 'v65536'))
        self.assertIn('the NUP has no v0', nsp.derived_reason(g, {'v65536': Path('u')}, 'v65536'))

    def test_its_derived_profile_keeps_the_display_version(self):   # a NUP without control data: the registered row's
        g = self.profile(f'build\tv0\t1.0.0\tmain={BID2}', 'shader_versions\tv65536')
        (self.ex / 'control.nacp').unlink()
        nsp.derive_profile(g, self.pdir, 'v0', self.ex, ['main'])
        self.assertEqual(game.Game(self.pdir).builds(), {'v0': {'display': '1.0.0', 'main': BID2}})

    def test_the_end_of_the_build(self):
        reg = self.profile(f'build\tv131072\t1.1.1\tmain={BID}', f'build\tv0\t1.0.0\tmain={BID2}', 'shader_versions\tv131072')
        d = self.tmp / 'd'; d.mkdir()
        def said(row, ver):
            (d / 'game.tsv').write_text(f'title_id\t{TID:016X}\nname\tTest Game\nmodules\tmain\n{row}\n')
            with mock.patch.object(nsp, 'say') as say: nsp.report_profile_status(game.Game(d), reg, ver, 'why', 'Mac app')
            return [c.args[0] for c in say.call_args_list]
        ok = said(f'build\tv0\t1.0.0\tmain={BID2}', 'v0')
        self.assertEqual(len(ok), 1); self.assertNotIn('[RED]', ok[0]); self.assertIn('registered v0 (1.0.0) builds', ok[0])
        self.assertIn('[RED]', said(f'build\tv196608\t?\tmain={"ef" * 16}', 'v196608')[0])


if __name__ == '__main__': unittest.main()
