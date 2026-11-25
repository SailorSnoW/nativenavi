"""build_app.py's shader step (build_shaders): the Mac libraries with their reuse key, never reused without the outputs' stamp (inputs.tsv, which the one chain version's
c1_full.tsv / cb_extent.tsv name); the libraries of other chain versions and the unused cache entries go after a build that succeeded; the run command's environment file.
No game files or Xcode needed (temporary files only in a temporary folder)."""
import contextlib
import importlib.util
import io
from pathlib import Path
import shutil
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
SCRIPT = REPO / 'scripts/build_app.py'
spec = importlib.util.spec_from_file_location('build_app', SCRIPT)
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
CHAIN, INPUTS = '0123456789ab', 'aaaaaaaaaaaa'
LIVE, OLD, NEW = 'a' * 64, 'b' * 64, 'c' * 64


class Base(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp()); self.addCleanup(shutil.rmtree, self.tmp)
        self.sh, self.logs, self.ran = self.tmp / 'shader', self.tmp / 'logs', []
        self.cas_used = [LIVE]   # the cache entries a full dmsh_build.py run lists as used
        self.logs.mkdir()
        for cm in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', lambda m: None), mock.patch.object(nsp, 'run', self.fake_run)):
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def fake_run(self, cmd, log, env=None, cwd=None, on_line=None):   # stands in for the translators: writes the libraries a real run writes
        self.ran.append(([str(c) for c in cmd], dict(env or {})))
        tool = Path(cmd[1]).name
        if tool == 'own_compilers.py':   # a full dmsh_build.py run: the libraries, their stamp, and the profile's tables naming it
            d = Path(cmd[3]) / f'b-{CHAIN}'; shutil.rmtree(d, ignore_errors=True); d.mkdir(parents=True)
            (d / 'k_b.metallib').write_bytes(b'lib'); (d / 'k_b.metal').write_text('MSL'); (d / 'k_b.json').write_text('{}'); (d / '_b_cas.txt').write_text(''.join(x + '\n' for x in self.cas_used))
            (d / 'inputs.tsv').write_text(f'# stamp\ngame\t0400486024DEC000\ninputs\t{INPUTS}\n')
            for n in ('c1_full.tsv', 'cb_extent.tsv'): (self.tmp / n).write_text(f'# chain b-{CHAIN} inputs {INPUTS} 2026-10-23T00:00:00+00:00\n')
            log.write_text('shaders 1: ok 1\n')


class Nvn(Base):
    def setUp(self):
        super().setUp()
        self.g = types.SimpleNamespace(table=lambda n: self.tmp / n, dir=self.tmp, id='0400486024DEC000')
        for cm in (mock.patch.object(nsp, 'fingerprint', lambda *a, **k: 'K'), mock.patch.object(nsp, 'chain_of', lambda repo: CHAIN)):   # one chain version for every game
            cm.__enter__(); self.addCleanup(cm.__exit__, None, None, None)

    def build(self):
        self.ran.clear()
        return nsp.build_shaders(sys.executable, REPO, self.sh, REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate', {'WORK': str(self.tmp)}, self.logs, self.g, 'P')

    def test_mac_libraries(self):   # into the cache folder, the key beside it, not in it; no switch of another platform's build
        self.assertEqual(self.build(), self.sh / 'cache')
        self.assertEqual(self.ran[0][0][1:], [str(REPO / 'dm/gfx/tools/own_compilers.py'), str(self.sh / 'pkg'), str(self.sh / 'cache')])
        for k in ('IOSOUT', 'DMSH_TARGETS', 'DMSH_IOS_PULL'): self.assertNotIn(k, self.ran[0][1])
        self.assertEqual(self.ran[0][1]['XLATE'], str(REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
        self.assertTrue((self.sh / f'mac-b-{CHAIN}.key').is_file()); self.assertEqual([p.name for p in (self.sh / 'cache').iterdir()], [f'b-{CHAIN}.img'])
        self.assertEqual(self.build(), self.sh / 'cache'); self.assertEqual(self.ran, [])

    def stale(self):   # two older chain versions' libraries and keys
        for old in ('157e1b96c8da', 'a78f966a7b38'):
            d = self.sh / 'cache' / f'b-{old}'; d.mkdir(parents=True); (d / 'k_b.metallib').write_bytes(b'x' * 100)
            (self.sh / f'mac-b-{old}.key').write_text('K\n')

    def left(self):
        return sorted(str(p.relative_to(self.sh)) for p in self.sh.rglob('*') if p.parent.name in ('cache', 'shader') and (p.name.startswith(('b-', 'mac-b-'))))

    def test_prune_stale_chains_after_success(self):   # the old chain versions go with their keys, the current one stays, the size is logged once
        self.stale()
        said = []
        with mock.patch.object(nsp, 'say', said.append):
            self.assertEqual(self.build(), self.sh / 'cache')
        self.assertEqual(self.left(), [f'cache/b-{CHAIN}.img', f'mac-b-{CHAIN}.key'])
        self.assertTrue(nsp.shader_lib.has_library(self.sh / 'cache' / f'b-{CHAIN}'))
        self.assertEqual([m for m in said if m.startswith('removed old shader libraries')], ['removed old shader libraries b-157e1b96c8da, b-a78f966a7b38 (0.2 KB) to obsolete/'])
        moved = list((self.sh.parent / 'obsolete').iterdir())   # into this build's folder there, at their places in the build folder
        self.assertEqual(len(moved), 1); self.assertTrue(nsp.STAMP.match(moved[0].name) and moved[0].name.endswith(' build'), moved[0].name)
        self.assertTrue((moved[0] / 'shader/cache/b-157e1b96c8da').exists() and (moved[0] / 'shader/mac-b-a78f966a7b38.key').exists())
        self.stale(); said.clear()   # a reused build prunes too
        with mock.patch.object(nsp, 'say', said.append):
            self.build()
        self.assertEqual(self.ran, []); self.assertEqual(len(self.left()), 2)

    def test_failed_build_keeps_stale_chains(self):   # nothing goes before the new libraries exist
        self.stale(); before = self.left()
        def failing(cmd, log, env=None, cwd=None, on_line=None): raise nsp.BuildError('translator failed')
        with mock.patch.object(nsp, 'run', failing), self.assertRaises(nsp.BuildError): self.build()
        self.assertEqual(self.left(), before)
        with mock.patch.object(nsp, 'run', lambda *a, **k: None), self.assertRaises(nsp.BuildError): self.build()   # no libraries written
        self.assertEqual(self.left(), before)

    def test_failed_prune_keeps_the_build(self):   # a folder that cannot be removed is a [RED] line; the others go and the build still returns its libraries
        self.stale()
        real, said = nsp.retire, []
        def stuck(work, p):
            if p.name == 'b-157e1b96c8da' and p.parent.name == 'cache': raise PermissionError(13, 'Permission denied', str(p))
            real(work, p)
        with mock.patch.object(nsp, 'retire', stuck), mock.patch.object(nsp, 'say', said.append):
            self.assertEqual(self.build(), self.sh / 'cache')
        self.assertEqual(self.left(), sorted(['cache/b-157e1b96c8da', f'cache/b-{CHAIN}.img', f'mac-b-{CHAIN}.key']))
        self.assertEqual(len([m for m in said if m.startswith('[RED] could not remove old shader libraries')]), 1)
        self.assertEqual(len([m for m in said if m.startswith('removed old shader libraries b-157e1b96c8da, b-a78f966a7b38 (')]), 1)

    def test_prune_ignores_other_names(self):   # only b-<12 hex> folders and their keys, never another folder
        self.build()
        for n in ('cas', 'b-notahexchain', 'b-0123456789abcd', 'mine'): (self.sh / 'cache' / n).mkdir()
        (self.sh / 'cache' / 'b-ffffffffffff').write_text('a file, not a library folder')
        self.build()
        self.assertEqual(sorted(p.name for p in (self.sh / 'cache').iterdir()), sorted(['b-0123456789abcd', 'b-ffffffffffff', 'b-notahexchain', f'b-{CHAIN}.img', 'cas', 'mine']))

    def test_a_folder_from_before_becomes_one_file_without_a_build(self):   # a reused step: nothing is translated again
        self.build(); lib = self.sh / 'cache' / f'b-{CHAIN}'
        nsp.shader_lib.expand(lib); self.assertTrue(lib.is_dir())
        self.assertEqual(self.build(), self.sh / 'cache'); self.assertEqual(self.ran, [])
        self.assertFalse(lib.exists()); self.assertTrue(lib.with_name(lib.name + '.img').is_file())

    def test_a_build_starts_without_the_old_image_and_ends_with_a_new_one(self):   # a stopped build must not read as the old library with a few new shaders
        self.build(); image = self.sh / 'cache' / f'b-{CHAIN}.img'; before = image.stat().st_ino
        (self.sh / f'mac-b-{CHAIN}.key').unlink()
        real, there = nsp.run, []
        def spy(cmd, *a, **k): there.append(image.exists()); return real(cmd, *a, **k)
        with mock.patch.object(nsp, 'run', spy): self.build()
        self.assertFalse(there[0]); self.assertTrue(image.is_file()); self.assertNotEqual(image.stat().st_ino, before)
        self.assertFalse((self.sh / 'cache' / f'b-{CHAIN}').exists())

    def test_old_chain_images_go_too(self):
        self.build()
        for n in ('b-157e1b96c8da.img', 'b-a78f966a7b38.img.part'): (self.sh / 'cache' / n).write_bytes(b'x')
        self.build()
        self.assertEqual(sorted(p.name for p in (self.sh / 'cache').iterdir() if p.name.startswith('b-')), [f'b-{CHAIN}.img'])

    def test_stale_chain_note_names_a_packed_library(self):
        self.build(); lib = self.sh / 'cache' / f'b-{CHAIN}'
        (lib.with_name(lib.name + '.img')).rename(self.sh / 'cache' / 'b-ba9876543210.img')
        with mock.patch.object(nsp, 'shader_keys', lambda g: ['k']):
            self.assertEqual(nsp.stale_shaders(sys.executable, REPO, {}, self.sh / 'cache', CHAIN, self.g)[:3], ('chain', 'b-ba9876543210', f'b-{CHAIN}'))

    def test_cli_tidies_the_work_folder_after_a_build_that_succeeded(self):
        folder = self.sh.parent / 'nup'; (folder / '0100000000000000-work').mkdir(parents=True); (folder / 'notes-work').mkdir()
        def failed(args): raise nsp.BuildError('stopped')
        with mock.patch.object(nsp, 'NUP_FOLDER', folder), mock.patch.object(nsp, 'parse_args'), mock.patch.object(nsp.PROG, 'say'), mock.patch.object(nsp.tree_tar, 'pack_quiet') as tidy:   # NUP_FOLDER as main leaves it
            with mock.patch.object(nsp, 'main', failed): self.assertEqual(nsp.cli([]), 1)
            tidy.assert_not_called()
            with mock.patch.object(nsp, 'main'): self.assertEqual(nsp.cli([]), 0)
        self.assertEqual([c.args[0].name for c in tidy.call_args_list], ['0100000000000000-work'])

    def store(self):
        s = nsp.shader_cache.Store(self.sh / f'cache/cas-{CHAIN}.db'); self.addCleanup(s.close)
        return s

    def entries(self):   # the shaders the cache holds, as the next build that runs would read them
        return {n.split('/')[1] for n in self.store().names('shader/')}

    def cas(self, *names, when=0):   # cache entries with a source and a library, last changed at when
        s = self.store()
        for n in names:
            s.put(f'shader/{n}/mac/k_b.metal', b'm' * 100 + n.encode()[:0], when); s.put(f'shader/{n}/mac/k_b.metallib', b'l' * 1000, when)
        s.close()

    def test_prune_shader_cache(self):   # entries the build did not list go once it succeeded; a listed one, one newer than the list and other names stay; only the sources count
        self.cas(LIVE, OLD, 'notadigest')
        said = []
        with mock.patch.object(nsp, 'say', said.append): self.build()
        self.cas(NEW, when=nsp.time.time() + 3600)   # published by another build sharing the cache after this one's list
        self.assertEqual(self.entries(), {LIVE, NEW, 'notadigest'})
        self.assertEqual([m for m in said if 'shader cache entries' in m], ['removed old shader cache entries: 1 (0.1 KB)'])
        self.cas(OLD); said.clear()   # a reused build prunes too
        with mock.patch.object(nsp, 'say', said.append): self.build()
        self.assertEqual(self.ran, []); self.assertNotIn(OLD, self.entries()); self.assertIn(NEW, self.entries())

    def test_older_compile_caches_go_with_a_build(self):   # other chains' files and the one file from before generations, once this chain has its own; a failed build keeps them
        old = nsp.shader_cache.Store(self.sh / 'cache/cas.db'); old.put('metallib/mac/x.metallib', b'old'); old.close()
        other = nsp.shader_cache.Store(self.sh / 'cache/cas-fedcba987654.db'); other.put('metallib/mac/m.metallib', b'mac'); other.close()
        self.build()
        self.assertTrue((self.sh / 'cache/cas.db').exists())   # the chain has no file of its own yet
        self.store().put('metallib/mac/a.metallib', b'a')
        (self.sh / f'mac-b-{CHAIN}.key').unlink()   # not reused: the step runs, and fails
        def failing(cmd, log, env=None, cwd=None, on_line=None): raise nsp.BuildError('translator failed')
        with mock.patch.object(nsp, 'run', failing), self.assertRaises(nsp.BuildError): self.build()
        self.assertTrue((self.sh / 'cache/cas-fedcba987654.db').exists())
        said = []
        with mock.patch.object(nsp, 'say', said.append): self.build()
        self.assertEqual(sorted(p.name for p in (self.sh / 'cache').glob('cas*.db')), [f'cas-{CHAIN}.db'])
        self.assertEqual(self.store().names('metallib/'), {'metallib/mac/a.metallib'})   # its own compiles: what a build reuses of the others it copies as it reads them
        self.assertTrue(any(m.startswith('removed old shader compile caches ') and m.endswith(' to obsolete/') for m in said), said)
        self.assertEqual(sorted(p.name for p in (self.sh.parent / 'obsolete').glob('* build/shader/cache/cas*.db')), ['cas-fedcba987654.db', 'cas.db'])

    def test_obsolete_emptied_by_the_next_successful_build(self):   # what earlier builds and runs moved there goes; what this build moved stays, and one line says how much
        ob = self.sh.parent / 'obsolete'
        for name in ('2026-01-02 030405 build', '2026-01-02 030406 pso_archive', 'loose file'):
            (ob / name).mkdir(parents=True); (ob / name / 'x').write_bytes(b'x' * 10)
        (ob / '2026-02-01 000000 build').mkdir(); (ob / '2026-02-01 000000 build' / 'y').write_bytes(b'y' * 2048)
        said = []
        with mock.patch.object(nsp, 'BUILD_STARTED', '2026-02-01 000000'), mock.patch.object(nsp, 'say', said.append): nsp.empty_obsolete(self.sh.parent)
        self.assertEqual([p.name for p in ob.iterdir()], ['2026-02-01 000000 build'])
        self.assertEqual(said, [f'no longer used: 2.0 KB in {ob}, removed after the next successful build (deleting the folder now costs nothing)'])
        said.clear()
        with mock.patch.object(nsp, 'BUILD_STARTED', '2026-03-01 000000'), mock.patch.object(nsp, 'say', said.append): nsp.empty_obsolete(self.sh.parent)
        self.assertFalse(ob.exists()); self.assertEqual(said, [])

    def test_prune_shader_cache_needs_success_and_the_list(self):   # a failed build, or libraries from before the list, keep every entry
        self.build(); self.cas(OLD)
        (self.sh / f'mac-b-{CHAIN}.key').unlink()   # not reused: the step runs, and fails
        def failing(cmd, log, env=None, cwd=None, on_line=None): raise nsp.BuildError('translator failed')
        with mock.patch.object(nsp, 'run', failing), self.assertRaises(nsp.BuildError): self.build()
        self.assertIn(OLD, self.entries())
        self.build(); self.assertNotIn(OLD, self.entries())
        nsp.shader_lib.expand(self.sh / 'cache' / f'b-{CHAIN}'); (self.sh / 'cache' / f'b-{CHAIN}' / '_b_cas.txt').unlink(); self.cas(OLD)
        self.build()
        self.assertEqual(self.ran, []); self.assertIn(OLD, self.entries())

    def test_no_stamp(self):   # Mac libraries no full build stamped (an interrupted one): no table can name them, so the key does not reuse them
        self.build()
        nsp.shader_lib.expand(self.sh / 'cache' / f'b-{CHAIN}'); (self.sh / 'cache' / f'b-{CHAIN}' / 'inputs.tsv').unlink()
        self.assertEqual(self.build(), self.sh / 'cache'); self.assertEqual(len(self.ran), 1)
        self.assertEqual(nsp.shader_lib.read_stamp(self.sh / 'cache' / f'b-{CHAIN}')['game'], '0400486024DEC000')

    def test_stale_tables(self):   # a reused build makes a table again when it does not name the Mac libraries' stamp
        self.build()
        (self.tmp / 'cb_extent.tsv').write_text(f'# chain b-{CHAIN} inputs bbbbbbbbbbbb 2026-10-23T00:00:00+00:00\n')
        self.assertEqual(self.build(), self.sh / 'cache'); self.assertEqual([Path(c[1]).name for c, _ in self.ran], ['cb_extent.py'])

    def test_the_build_hands_the_table_memo(self):   # c1_guard.py / cb_extent.py keep their answers in the shader cache (DMSH_TABLE_MEMO)
        self.build()
        self.assertEqual(self.ran[0][1]['DMSH_TABLE_MEMO'], str(self.sh / 'cache'))

    def test_split_pairs_invalidate_shader_reuse(self):
        # A registered profile may have no pso_pairs.tsv: the split's copy feeds vs_pad.
        def fingerprint(*args, **kwargs):
            path = next(x[1][0] for x in args if isinstance(x, tuple) and x[0] == 'files' and x[1] and x[1][0].name == 'pso_pairs.tsv')
            return str(path.read_bytes() if path.is_file() else b'missing')
        with mock.patch.object(nsp, 'fingerprint', side_effect=fingerprint):
            self.build(); self.assertEqual(len(self.ran), 1)
            self.build(); self.assertEqual(self.ran, [])
            (self.sh / 'pso_pairs.tsv').write_text('v\tf1\n')
            self.build(); self.assertEqual(len(self.ran), 1)
            (self.sh / 'pso_pairs.tsv').write_text('v\tf2\n')
            self.build(); self.assertEqual(len(self.ran), 1)
            self.build(); self.assertEqual(self.ran, [])

    def test_shader_watch_reads_the_progress_file(self):   # dmsh_build.py DMSH_PROGRESS
        p = self.tmp / 'progress.json'; watch = nsp.ShaderWatch(p)
        self.assertIsNone(watch())   # not written yet
        p.write_text('{"step": "cache check", "done": 3, "total": 10, "cached": 2}')
        self.assertEqual(watch(), {'step': 'cache check', 'done': 3, 'total': 10, 'unit': 'shaders', 'note': '2 cached'})
        p.write_text('{"step": "compiling", "done": 7, "total": 10, "cached": 6, "misses": 4}')
        self.assertEqual(watch(), {'step': 'compiling', 'done': 7, 'total': 10, 'unit': 'shaders', 'note': '4 not cached'})
        p.write_text('{"step": "comp')   # half written: no progress this time, not an error
        self.assertIsNone(watch())

    def test_shader_watch_follows_the_steps_after_the_compile(self):   # the library image and the two tables, as dmsh_build.py writes them
        p = self.tmp / 'progress.json'; watch = nsp.ShaderWatch(p)
        p.write_text('{"step": "compiling", "done": 42847, "total": 42847, "cached": 42847, "misses": 0}')
        self.assertEqual(watch(), {'step': 'compiling', 'done': 42847, 'total': 42847, 'unit': 'shaders', 'note': ''})
        p.write_text('{"step": "writing the library image", "done": 0, "total": 100, "unit": "files", "bytes": 0}')
        self.assertEqual(watch(), {'step': 'writing the library image', 'done': 0, 'total': 100, 'unit': 'files'})
        p.write_text('{"step": "writing the library image", "done": 50, "total": 100, "unit": "files", "bytes": 2097152}')
        self.assertEqual(watch(), {'step': 'writing the library image', 'done': 50, 'total': 100, 'unit': 'files', 'note': '2.0 MB'})
        for step in ('c1 guard', 'uniform read extents'):
            p.write_text(f'{{"step": "{step}"}}')
            self.assertEqual(watch(), {'step': step})
        with contextlib.redirect_stdout(io.StringIO()):
            prog = nsp.Progress('off'); prog.plan(1); prog.stage('Mac shaders')
            for payload in ('{"step": "writing the library image", "done": 50, "total": 100, "unit": "files", "bytes": 2097152}', '{"step": "c1 guard"}'):
                p.write_text(payload); prog.set(**watch())
            self.assertEqual((prog.cur.step, prog.cur.done, prog.cur.total, prog.cur.unit, prog.cur.note), ('c1 guard', None, None, '', ''))   # no count left from the image
            prog.close()


class EnvFile(Base):
    def test_pairs(self):
        p = self.tmp / 'env.txt'; p.write_text('# a comment\nDM_A=1\n  DM_B = x=y\n=nokey\nnoeq\n\nDM_C=\n')
        self.assertEqual(nsp.env_file_pairs(p), {'DM_A': '1', 'DM_B': ' x=y', 'DM_C': ''})

    def test_missing(self):
        with self.assertRaises(nsp.BuildError): nsp.env_file_pairs(self.tmp / 'none.txt')


if __name__ == '__main__':
    unittest.main()
