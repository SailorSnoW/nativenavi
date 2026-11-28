#!/usr/bin/env python3
# One command for the repository's tests on this Mac (docs/development.md#running-the-tests). CI runs the suites that need no GPU;
# the graphics tests' GPU runs and the Python tests' Metal and GPU cases run only here.
#
# Usage: python3 scripts/test.py [SUITE ...] [--changed [REV]] [-j N] [--gpu-jobs N] [--opt 2] [--timeout S] [--coverage] [--logs DIR] [--lock FILE] [--list]
#   Suites (default: all of them but game):
#     host    dm/host/tests, built and run by its Makefile          tex     dm/gfx/tex/tests
#     rust    cargo test in dm/host/rust                            gfx     dm/gfx/tests: every test of its run list through its check_ target, on the GPU
#     pygpu   the Python tests' Metal and GPU cases, one at a time   python  every dm/tools/tests and dm/gfx/tests test_*.py, as CI runs them
#     docs    dm/tools/tests/test_docs.py alone                     game    scripts/regress.py check --quick (REGRESS_ROOT and REGRESS_GAMES or REGRESS_BUILD set)
#   --changed [REV]: the suites the files changed since REV need (default: the merge base with main; uncommitted and untracked files count), by RULES below.
#   -j N: parallel builds and CPU tests (default: the performance cores). --gpu-jobs N: graphics tests run at once (default 4); those the graphics Makefile names
#     in SERIAL_TESTS, which measure time, run one at a time after the others.
#   --opt N: the graphics tests' optimization, -ON (their Makefile's OPT, -O0 by default; 2 is the host's, for the run before a commit).
#   --timeout S: a test that runs longer is killed with its process group and fails (default 300; build steps get four times that).
#   --coverage: line coverage instead of a plain run (host, tex, gfx, python): each suite's coverage build, then dm/tools/coverage_report.py's summary.
#   --logs DIR: keep every test's output there (default: a temporary folder that keeps only the failing tests' output).
#   --lock FILE (DM_MEASURE_LOCK): bench.py's measuring lock, held for the whole run: a benchmark waits for the tests and the tests for a benchmark.
#   The Mac and its display are woken and kept awake while it runs (caffeinate). Exit status: 0 = every test passed, 1 = a test failed or timed out, 2 = a suite could not run.
import argparse, concurrent.futures as cf, importlib.util, os, re, shutil, signal, subprocess, sys, tempfile, threading, time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
GAME = '0400486024DEC000'
ALL = ('host', 'tex', 'rust', 'gfx', 'pygpu', 'python')
SUITES = ALL + ('docs', 'game')
# What a changed path needs, first match wins; a path that matches none needs everything. The Python tests read sources of the host and the graphics layer
# (test_architecture.py, test_env_switches.py), so they follow those too.
RULES = (
    (r'^dm/host/rust/', ('rust', 'host')),
    (r'^dm/host/tests/', ('host',)),
    (r'^dm/gfx/tex/tests/', ('tex',)),
    (r'^dm/gfx/tests/test_[^/]*\.py$', ('python', 'pygpu')),
    (r'^dm/gfx/tests/', ('gfx', 'python')),
    (r'^dm/gfx/(nvn|tex|msl)/', ('gfx', 'tex', 'host', 'python')),
    (r'^dm/host/|^dm/gfx/[^/]*\.[chm]$', ('host', 'gfx', 'python')),
    (r'^dm/gfx/tools/', ('python', 'pygpu', 'gfx')),
    (r'^dm/sanitizers/', ('host', 'tex', 'gfx', 'python')),
    (r'^docs/|^[^/]*\.md$|^dm/games/[^/]*/README\.md$', ('docs',)),
    (r'^dm/third_party/', ('host', 'pygpu', 'python')),   # dav1d is linked by the host's tests, the translator's sources are built by the tools'
    (r'^(dm/tools|dm/games|dm/gui|dm/mac|dm/nix|scripts|\.github|\.claude)/|^dm/(gfx/)?[^/]*$|^[^/]*$', ('python',)),
)


def select(paths):
    """The suites a set of changed paths needs, in ALL's order (docs only when nothing else asks for the Python tests)"""
    want = set()
    for p in paths:
        for pattern, suites in RULES:
            if re.search(pattern, p):
                want.update(suites); break
        else: return list(ALL)
    if 'python' in want: want.discard('docs')
    return [s for s in SUITES if s in want]


def changed_paths(rev):
    def git(*args):
        r = subprocess.run(['git', *args], cwd=REPO, capture_output=True, text=True)
        if r.returncode: raise SystemExit(f'[RED] test.py: git {" ".join(args)} failed: {r.stderr.strip()}')
        return r.stdout
    base = rev or git('merge-base', 'HEAD', 'main').strip()
    return sorted(set(git('diff', '--name-only', base).split('\n') + git('ls-files', '--others', '--exclude-standard').split('\n')) - {''})


class Job:
    def __init__(self, suite, name, cmd, cwd=REPO, env=None, timeout=300.0, serial=False):
        self.suite, self.name, self.cmd, self.cwd, self.env, self.timeout, self.serial = suite, name, [str(c) for c in cmd], cwd, env, timeout, serial
        self.ok, self.secs, self.why, self.log = False, 0.0, '', None


_live, _live_mu, _stopping = set(), threading.Lock(), threading.Event()


def run_job(job, logs):
    """Runs one job in a process group of its own, its output in <logs>/<suite>/<name>.log; a timeout kills the group"""
    if _stopping.is_set():
        job.why = 'not run'; return job
    d = logs / job.suite; d.mkdir(parents=True, exist_ok=True)
    job.log = d / (re.sub(r'[^A-Za-z0-9_.-]+', '_', job.name) + '.log')
    t0 = time.monotonic()
    with open(job.log, 'wb') as out:
        try: p = subprocess.Popen(job.cmd, cwd=job.cwd, env=job.env, stdout=out, stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, start_new_session=True)
        except OSError as e:
            job.why = f'could not start: {e}'; return job
        with _live_mu: _live.add(p.pid)
        try:
            rc = p.wait(timeout=job.timeout)
            job.ok, job.why = rc == 0, '' if rc == 0 else f'exit {rc}'
        except subprocess.TimeoutExpired:
            job.why = f'timed out after {job.timeout:.0f} s'
        finally:
            # the group, not the child alone: a test's own children (make's, a fixture's) would otherwise outlive it
            try: os.killpg(p.pid, signal.SIGKILL)
            except (ProcessLookupError, PermissionError): pass
            p.wait()
            with _live_mu: _live.discard(p.pid)
    job.secs = time.monotonic() - t0
    return job


def run_jobs(jobs, logs, n, label):
    """The parallel jobs n at a time, then the serial ones one by one; prints each failure as it comes"""
    if not jobs: return
    t0, done = time.monotonic(), 0

    def report(j):
        nonlocal done
        done += 1
        if not j.ok: print(f'  FAILED {j.suite}: {j.name} ({j.why}, {j.secs:.1f} s)', flush=True)
        elif sys.stdout.isatty(): print(f'  {label}: {done}/{len(jobs)}', end='\r', flush=True)
    with cf.ThreadPoolExecutor(max(1, n)) as ex:
        for j in ex.map(lambda j: run_job(j, logs), [j for j in jobs if not j.serial]): report(j)
    for j in jobs:
        if j.serial: report(run_job(j, logs))
    print(f'  {label}: {len(jobs)} run, {sum(not j.ok for j in jobs)} failed, {time.monotonic() - t0:.0f} s', flush=True)


def make_words(folder, target):
    r = subprocess.run(['make', '--no-print-directory', '-C', str(folder), target], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'[RED] test.py: make -C {folder} {target} failed: {r.stderr.strip()[-300:]}')
    return r.stdout.split()


HOST_RUN = re.compile(r'(?:[A-Z_]+=(?:"[^"]*"|\S+) )*(?:build/(t_\w+)|python3 (\S+))(.*)')


def host_commands():
    """What dm/host/tests' `make run` runs, one command a test (its recipe, read with make -n), so a failing test does not hide the ones after it"""
    d = REPO / 'dm/host/tests'
    r = subprocess.run(['make', '-n', '-C', str(d), 'run'], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'[RED] test.py: make -n -C {d} run failed: {r.stderr.strip()[-300:]}')
    out, seen = [], {}
    for line in r.stdout.splitlines():
        m = HOST_RUN.fullmatch(line.strip())
        if not m or (m[2] and not (d / m[2]).is_file()): continue   # a sub-make's own python3 line is not a test
        name = ((m[1] or m[2]) + re.sub(r'\s*\d?>\s*\S+', '', m[3])).strip()
        seen[name] = seen.get(name, 0) + 1
        out.append((name if seen[name] == 1 else f'{name} ({seen[name]})', line.strip(), m[1]))
    if not out: raise SystemExit(f'[RED] test.py: make -n -C {d} run names no test')
    return out


def python_files():
    return sorted((REPO / 'dm/tools/tests').glob('test_*.py')) + sorted((REPO / 'dm/gfx/tests').glob('test_*.py'))


def plan(suite, a):
    """A suite's phases, each (how many jobs at once, jobs), run one after the other (a build before its tests)"""
    gfx, opt, big = REPO / 'dm/gfx/tests', [f'OPT=-O{a.opt}'] if a.opt else [], a.timeout * 4
    py_env = dict(os.environ, DM_GAME=GAME, DMSH_TEST_XLATE=str(REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'))
    if suite == 'host':
        d = REPO / 'dm/host/tests'
        if a.coverage: return [(1, [Job('host', 'coverage', ['make', '-C', d, f'-j{a.jobs}', 'coverage'], timeout=big)])]
        cmds = host_commands()
        build = Job('host', 'build', ['make', '-C', d, '-k', f'-j{a.jobs}', *sorted({f'build/{b}' for _, _, b in cmds if b})], timeout=big)
        # one at a time: several play on the audio device or measure the scheduler
        return [(1, [build]), (1, [Job('host', name, ['sh', '-c', line], cwd=d, timeout=a.timeout) for name, line, _ in cmds])]
    if suite == 'tex':
        return [(1, [Job('tex', 'coverage' if a.coverage else 'build and run', ['make', '-C', 'dm/gfx/tex/tests', 'coverage' if a.coverage else 'run'], timeout=big)])]
    if suite == 'rust':
        return [(1, [Job('rust', 'cargo test', ['cargo', 'test', '--locked', '--offline'], cwd=REPO / 'dm/host/rust', timeout=big)])]
    if suite == 'gfx':
        if a.coverage: return [(1, [Job('gfx', 'coverage', ['make', '-C', gfx, f'-j{a.jobs}', *opt, 'coverage'], timeout=a.timeout * 20)])]
        tests, serial = make_words(gfx, 'print-run'), set(make_words(gfx, 'print-serial'))
        build = Job('gfx', 'build', ['make', '-C', gfx, '-k', f'-j{a.jobs}', *opt, *tests, 'nvn_replay'], timeout=big)
        check = [Job('gfx', t, ['make', '--no-print-directory', '-C', gfx, *opt, f'check_{t}'], timeout=a.timeout, serial=t in serial) for t in tests]
        check += [Job('gfx', t, ['make', '--no-print-directory', '-C', gfx, *opt, f'check_{t}'], timeout=a.timeout) for t in ('test_dmsh_pull', 'test_tbdr_budget')]
        return [(1, [build]), (a.gpu_jobs, check)]
    if suite == 'pygpu':
        # test_dmsh_compute_loops.py's fixture links the graphics tests' shared objects: made first, by one make
        build = Job('pygpu', 'build', ['make', '-C', gfx, f'-j{a.jobs}', *opt, 't_compute_loops'], timeout=big)
        # one at a time: beside other work on the GPU a kernel's legitimate loop budget ran out (test_dmsh_compute_loops.py), and two dotnet builds at once lost a
        # build node (MSB4166, test_dmsh_native_functions.py)
        return [(1, [build]), (1, [Job('pygpu', f, ['make', '--no-print-directory', '-C', gfx, *opt, f'pygpu_{f}'], timeout=a.timeout * 2) for f in make_words(gfx, 'print-py-gpu')])]
    if suite in ('python', 'docs'):
        files = [REPO / 'dm/tools/tests/test_docs.py'] if suite == 'docs' else python_files()
        run = [sys.executable, '-m', 'coverage', 'run', '-p', '-m', 'unittest'] if a.coverage and suite == 'python' else [sys.executable, '-m', 'unittest', '-v']
        return [(a.jobs, [Job(suite, f.name, [*run, f.relative_to(REPO)], env=py_env, timeout=a.timeout * 2) for f in files])]
    if suite == 'game':
        if not (os.environ.get('REGRESS_ROOT') and (os.environ.get('REGRESS_GAMES') or os.environ.get('REGRESS_BUILD'))):
            raise SystemExit('[RED] test.py: the game suite needs REGRESS_ROOT and REGRESS_GAMES or REGRESS_BUILD (docs/development.md#the-regression-harness)')
        return [(1, [Job('game', 'regress.py check --quick', [sys.executable, 'scripts/regress.py', 'check', '--quick'], timeout=3600)])]
    raise SystemExit(f'[RED] test.py: no suite {suite!r} (one of {", ".join(SUITES)})')


def coverage_summary(suites, logs):
    """coverage_report.py over what the coverage runs wrote; the Python data files are combined first"""
    args = []
    for suite, flag, path in (('host', '--host', 'dm/host/tests/build/cov/coverage.lcov'), ('tex', '--textures', 'dm/gfx/tex/tests/cov/coverage.lcov'), ('gfx', '--gfx', 'dm/gfx/tests/coverage.lcov')):
        if suite in suites and (REPO / path).is_file(): args += [flag, path]
    if 'python' in suites:
        out = logs / 'python-coverage.json'
        if subprocess.run([sys.executable, '-m', 'coverage', 'combine', '-q'], cwd=REPO).returncode == 0 and \
           subprocess.run([sys.executable, '-m', 'coverage', 'json', '-q', '-o', str(out)], cwd=REPO).returncode == 0: args += ['--python', str(out)]
    if args: subprocess.run([sys.executable, 'dm/tools/coverage_report.py', *args, '--holes', '15'], cwd=REPO)


def tail(path, n=12):
    try: lines = Path(path).read_text(errors='replace').rstrip().split('\n')
    except OSError: return ''
    return '\n'.join('      ' + line[:300] for line in lines[-n:])


def perf_cores():
    r = subprocess.run(['sysctl', '-n', 'hw.perflevel0.physicalcpu'], capture_output=True, text=True)
    return int(r.stdout) if r.returncode == 0 and r.stdout.strip().isdigit() else os.cpu_count() or 4


def wake():
    """Keeps the Mac and its display awake until this process ends. -u first: a Mac that woke for maintenance with nobody at it (a dark wake) runs no audio device
    and no display link, so t_audio's device restart and the present tests fail there (docs/reference/audio.md#tests-of-the-audio-device-fail-on-a-mac-in-dark-wake)"""
    if not shutil.which('caffeinate'): return None
    subprocess.run(['caffeinate', '-u', '-t', '2'])
    return subprocess.Popen(['caffeinate', '-dis', '-w', str(os.getpid())])


def stop(sig, frame):
    _stopping.set()
    with _live_mu: pids = list(_live)
    for pid in pids:
        try: os.killpg(pid, signal.SIGKILL)
        except (ProcessLookupError, PermissionError): pass


def main(argv=None):
    ap = argparse.ArgumentParser(description="The repository's tests on this Mac: builds in parallel, each test under a timeout, a summary at the end")
    ap.add_argument('suites', nargs='*', metavar='SUITE', help=f'{", ".join(SUITES)} (default: {" ".join(ALL)})')
    ap.add_argument('--changed', nargs='?', const='', default=None, metavar='REV', help='only the suites the files changed since REV need (default REV: the merge base with main)')
    ap.add_argument('-j', '--jobs', type=int, default=perf_cores(), help='parallel builds and CPU tests (default: the performance cores)')
    ap.add_argument('--gpu-jobs', type=int, default=4, help='graphics tests run at once (default 4)')
    ap.add_argument('--opt', default='', choices=('0', '1', '2', '3', 's'), help="the graphics tests' optimization, -ON: their Makefile's OPT (default: the Makefile's, -O0)")
    ap.add_argument('--timeout', type=float, default=300.0, help='seconds a test may run (default 300)')
    ap.add_argument('--coverage', action='store_true', help='line coverage of host, tex, gfx and python instead of a plain run')
    ap.add_argument('--logs', default='', help="keep every test's output in this folder")
    ap.add_argument('--lock', default=os.environ.get('DM_MEASURE_LOCK', ''), help="bench.py's measuring lock, held while the tests run (DM_MEASURE_LOCK)")
    ap.add_argument('--list', action='store_true', help='print the suites that would run, and stop')
    a = ap.parse_args(argv)
    for s in a.suites:
        if s not in SUITES: ap.error(f'no suite {s!r} (one of {", ".join(SUITES)})')
    if a.changed is not None:
        paths = changed_paths(a.changed)
        suites = [s for s in select(paths) if not a.suites or s in a.suites]
        print(f'{len(paths)} changed files need: {" ".join(suites) or "nothing"}', flush=True)
    else: suites = a.suites or list(ALL)
    if a.coverage: suites = [s for s in suites if s in ('host', 'tex', 'gfx', 'python')]
    if a.list or not suites:
        print(' '.join(suites)); return 0
    logs = Path(a.logs).resolve() if a.logs else Path(tempfile.mkdtemp(prefix='nativenavi-tests-'))
    logs.mkdir(parents=True, exist_ok=True)
    signal.signal(signal.SIGINT, stop); signal.signal(signal.SIGTERM, stop)
    awake = wake()
    games = subprocess.run(['pgrep', '-x', 'dmhost'], capture_output=True, text=True).stdout.split()
    if games and set(suites) & {'gfx', 'pygpu', 'game'}: print(f'note: a game is running (dmhost pid {" ".join(games)}): the tests that measure time may fail beside it', flush=True)
    done, t0 = [], time.monotonic()

    def run_all():
        for s in suites:
            if _stopping.is_set(): break
            print(f'{s}:', flush=True)
            for width, jobs in plan(s, a):
                run_jobs(jobs, logs, width, 'tests' if len(jobs) > 1 else jobs[0].name)
                done.extend(jobs)
    try:
        if a.lock:
            spec = importlib.util.spec_from_file_location('bench', REPO / 'scripts/bench.py')
            if spec is None or spec.loader is None: raise SystemExit('[RED] test.py: cannot load scripts/bench.py for --lock')
            bench = importlib.util.module_from_spec(spec); spec.loader.exec_module(bench)
            with bench.hold_lock(a.lock, 'scripts/test.py'): run_all()
        else: run_all()
    finally:
        if awake: awake.terminate()
    if a.coverage and not _stopping.is_set(): coverage_summary(suites, logs)
    failed = [j for j in done if not j.ok]
    print(f'\n{"suite":8} {"run":>5} {"failed":>6} {"seconds":>8}')
    for s in suites:
        js = [j for j in done if j.suite == s]
        print(f'{s:8} {len(js):5} {sum(not j.ok for j in js):6} {sum(j.secs for j in js):8.0f}')
    print(f'{"total":8} {len(done):5} {len(failed):6} {time.monotonic() - t0:8.0f} (wall)')
    for j in failed:
        print(f'\nFAILED {j.suite}: {j.name} ({j.why}): {j.log}')
        print(tail(j.log))
    if not a.logs:
        for j in done:
            if j.ok and j.log: j.log.unlink(missing_ok=True)
        if not failed: shutil.rmtree(logs, ignore_errors=True)
    if _stopping.is_set():
        print('stopped'); return 130
    could_not_start = any(j.why.startswith('could not start') for j in failed)
    return 2 if could_not_start else 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
