#!/usr/bin/env python3
# scripts/test.py: which suites a change needs, the jobs it plans from the Makefiles' own lists, a job's timeout and process group, and its exit status.
# No GPU, game files or compiler: the plans are read with make -n and print- targets, and the jobs run here are small shell commands.
import argparse, contextlib, importlib.util, io, os, re, subprocess, tempfile, time, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('test_runner', REPO / 'scripts/test.py')
assert spec and spec.loader
tr = importlib.util.module_from_spec(spec); spec.loader.exec_module(tr)
GFX = REPO / 'dm/gfx/tests'


def args(**kw):
    return argparse.Namespace(**{'opt': '', 'jobs': 4, 'gpu_jobs': 2, 'timeout': 300.0, 'coverage': False, **kw})


class Select(unittest.TestCase):
    def test_a_change_needs_the_suites_that_read_it(self):
        self.assertEqual(tr.select(['dm/host/hle_svc.c']), ['host', 'gfx', 'python'])
        self.assertEqual(tr.select(['dm/gfx/hle_vi.c']), ['host', 'gfx', 'python'])
        self.assertEqual(tr.select(['dm/gfx/nvn/nvn_present.inc']), ['host', 'tex', 'gfx', 'python'])
        self.assertEqual(tr.select(['dm/gfx/tests/t_alpha.m']), ['gfx', 'python'])
        self.assertEqual(tr.select(['dm/gfx/tests/test_dmsh_ndsr.py']), ['pygpu', 'python'])
        self.assertEqual(tr.select(['dm/host/rust/src/lib.rs']), ['host', 'rust'])
        self.assertEqual(tr.select(['dm/tools/patch_nso.py', 'scripts/bench.py', 'flake.nix']), ['python'])
        self.assertEqual(tr.select(['dm/gfx/tools/dmsh_build.py']), ['gfx', 'pygpu', 'python'])

    def test_documentation_alone_needs_its_own_test_only(self):
        self.assertEqual(tr.select(['docs/reference/memory.md', 'AGENTS.md', f'dm/games/{tr.GAME}/README.md']), ['docs'])
        self.assertEqual(tr.select(['docs/reference/memory.md', 'dm/tools/game.py']), ['python'])

    def test_an_unknown_path_needs_everything_and_nothing_needs_nothing(self):
        self.assertEqual(tr.select(['dm/new_folder/x.c']), list(tr.ALL))
        self.assertEqual(tr.select([]), [])

    def test_every_tracked_file_matches_a_rule(self):
        # a new top-level area is given a rule here, not left to run every suite by accident
        files = subprocess.run(['git', 'ls-files'], cwd=REPO, capture_output=True, text=True, check=True).stdout.split('\n')
        loose = sorted({f.split('/')[0] if '/' not in f else '/'.join(f.split('/')[:2]) for f in files if f and not any(re.search(p, f) for p, _ in tr.RULES)})
        self.assertEqual(loose, [], 'no rule of scripts/test.py RULES names these')


class Plan(unittest.TestCase):
    def test_graphics_tests_are_the_makefile_run_list_with_its_serial_ones_last(self):
        run, serial = tr.make_words(GFX, 'print-run'), set(tr.make_words(GFX, 'print-serial'))
        self.assertLessEqual(serial, set(run), 'SERIAL_TESTS names a test that is not run')
        (w1, build), (w2, checks) = tr.plan('gfx', args(opt='2'))
        self.assertEqual((w1, w2), (1, 2))
        self.assertIn('OPT=-O2', build[0].cmd)
        self.assertEqual([j.name for j in checks], run + ['test_dmsh_pull', 'test_tbdr_budget'])
        self.assertEqual({j.name for j in checks if j.serial}, serial)
        self.assertTrue(all(j.cmd[-1] == f'check_{j.name}' for j in checks))

    def test_gpu_python_files_are_those_that_name_a_switch(self):
        names = tr.make_words(GFX, 'print-py-gpu')
        want = sorted(f.stem for f in tr.python_files() if re.search(r'DMSH_TEST_(GPU|METAL)', f.read_text()) and f.name != Path(__file__).name)
        self.assertEqual(sorted(names), want)
        self.assertEqual([j.name for j in tr.plan('pygpu', args())[1][1]], names)

    def test_host_tests_are_the_run_recipe_one_job_each(self):
        cmds = tr.host_commands()
        names = [n for n, _, _ in cmds]
        self.assertEqual(len(names), len(set(names)))
        built = {b for _, _, b in cmds if b}
        sources = {p.stem for p in (REPO / 'dm/host/tests').glob('t_*.c')} | {p.stem for p in (REPO / 'dm/host/tests').glob('t_*.m')}
        self.assertLessEqual({'t_svc_memory', 't_audio', 't_condvar'}, built)
        self.assertLessEqual({b for b in built if b in sources}, sources)
        self.assertTrue(any(line.startswith('DM_TEST_BUILD=build python3 test_memory_profile.py') for _, line, _ in cmds))
        self.assertFalse(any('game.py' in line for _, line, _ in cmds), "a sub-make's line taken for a test")
        (_, build), (width, tests) = tr.plan('host', args())
        self.assertEqual(width, 1)
        self.assertEqual(len(tests), len(cmds))
        self.assertLessEqual({f'build/{b}' for b in built}, set(build[0].cmd))

    def test_python_runs_every_test_file_with_the_profile_and_the_translator(self):
        (width, jobs), = tr.plan('python', args(jobs=3))
        self.assertEqual(width, 3)
        self.assertEqual(len(jobs), len(tr.python_files()))
        self.assertIn(Path(__file__).name, [j.name for j in jobs])
        self.assertEqual(jobs[0].env['DM_GAME'], tr.GAME)
        self.assertTrue(jobs[0].env['DMSH_TEST_XLATE'].endswith('dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
        (_, cov), = tr.plan('python', args(coverage=True))
        self.assertEqual(cov[0].cmd[1:7], ['-m', 'coverage', 'run', '-p', '-m', 'unittest'])

    def test_the_game_suite_refuses_without_its_folders(self):
        with mock.patch.dict(os.environ, {'REGRESS_ROOT': '/r', 'REGRESS_GAMES': '', 'REGRESS_BUILD': ''}), self.assertRaises(SystemExit) as e: tr.plan('game', args())
        self.assertIn('[RED]', str(e.exception))
        with mock.patch.dict(os.environ, {'REGRESS_ROOT': '/r', 'REGRESS_GAMES': '', 'REGRESS_BUILD': '/b'}): (_, jobs), = tr.plan('game', args())
        self.assertEqual(jobs[0].cmd[1:], ['scripts/regress.py', 'check', '--quick'])


class Run(unittest.TestCase):
    def test_a_job_passes_fails_and_keeps_its_output(self):
        with tempfile.TemporaryDirectory() as d:
            ok = tr.run_job(tr.Job('s', 'fine', ['sh', '-c', 'echo out; echo err >&2']), Path(d))
            bad = tr.run_job(tr.Job('s', 'a b/c', ['sh', '-c', 'echo why; exit 3']), Path(d))
            gone = tr.run_job(tr.Job('s', 'none', [str(Path(d) / 'no-such-program')]), Path(d))
            self.assertTrue(ok.ok); self.assertEqual(sorted(ok.log.read_text().split()), ['err', 'out'])
            self.assertFalse(bad.ok); self.assertEqual(bad.why, 'exit 3'); self.assertEqual(bad.log.name, 'a_b_c.log')
            self.assertIn('why', tr.tail(bad.log))
            self.assertFalse(gone.ok); self.assertTrue(gone.why.startswith('could not start'))

    def test_a_timeout_kills_the_whole_process_group(self):
        with tempfile.TemporaryDirectory() as d:
            mark = Path(d) / 'pid'
            t0 = time.monotonic()
            j = tr.run_job(tr.Job('s', 'slow', ['sh', '-c', f'sleep 60 & echo $! > {mark}; wait'], timeout=0.5), Path(d))
            self.assertFalse(j.ok); self.assertEqual(j.why, 'timed out after 0 s'); self.assertLess(time.monotonic() - t0, 10)
            child = int(mark.read_text())
            for _ in range(50):
                try: os.kill(child, 0)
                except ProcessLookupError: break
                time.sleep(0.1)
            else: self.fail(f'the job\'s child {child} outlived its timeout')

    def test_serial_jobs_run_after_the_parallel_ones(self):
        with tempfile.TemporaryDirectory() as d:
            log = Path(d) / 'order'
            jobs = [tr.Job('s', 'last', ['sh', '-c', f'echo last >> {log}'], serial=True)] + [tr.Job('s', f'p{i}', ['sh', '-c', f'echo p >> {log}']) for i in range(4)]
            with contextlib.redirect_stdout(io.StringIO()): tr.run_jobs(jobs, Path(d), 4, 'tests')
            self.assertEqual(log.read_text().split(), ['p', 'p', 'p', 'p', 'last'])
            self.assertTrue(all(j.ok for j in jobs))


class Main(unittest.TestCase):
    def run_main(self, argv, plans):
        out = io.StringIO()
        with mock.patch.object(tr, 'plan', lambda s, a: plans[s]), mock.patch.object(tr, 'wake', lambda: None), contextlib.redirect_stdout(out):
            return tr.main(argv), out.getvalue()

    def test_list_and_unknown_suite(self):
        rc, out = self.run_main(['--list'], {})
        self.assertEqual((rc, out.split()), (0, list(tr.ALL)))
        rc, out = self.run_main(['docs', 'tex', '--coverage', '--list'], {})
        self.assertEqual(out.split(), ['tex'])
        with self.assertRaises(SystemExit), contextlib.redirect_stderr(io.StringIO()): tr.main(['nothing'])

    def test_exit_status_and_summary(self):
        good = [(1, [tr.Job('tex', 'one', ['true'])])]
        rc, out = self.run_main(['tex'], {'tex': good})
        self.assertEqual(rc, 0); self.assertRegex(out, r'tex\s+1\s+0\s')
        rc, out = self.run_main(['tex', 'rust'], {'tex': [(1, [tr.Job('tex', 'one', ['sh', '-c', 'echo the reason; false'])])], 'rust': [(1, [tr.Job('rust', 'two', ['true'])])]})
        self.assertEqual(rc, 1)
        self.assertIn('FAILED tex: one (exit 1)', out); self.assertIn('the reason', out); self.assertRegex(out, r'rust\s+1\s+0\s')   # a failing suite does not stop the next
        rc, out = self.run_main(['tex'], {'tex': [(1, [tr.Job('tex', 'one', ['/no/such/program'])])]})
        self.assertEqual(rc, 2)

    def test_changed_takes_the_suites_of_the_changed_files(self):
        with mock.patch.object(tr, 'changed_paths', lambda rev: ['docs/development.md']):
            rc, out = self.run_main(['--changed', '--list'], {})
        self.assertEqual((rc, out.split('\n')[1]), (0, 'docs'))
        self.assertIn('1 changed files need: docs', out)


if __name__ == '__main__':
    unittest.main()
