#!/usr/bin/env python3
# C and Objective-C lint of dm/host and dm/gfx, as CI runs it (.github/workflows/lint-c.yml).
#   compdb  dm/host/gen/compile_commands.json from dry runs of the host's, the tests' and the texture tests' Makefiles (the host's command first).
#   tidy    clang-tidy finding counts per file and check, a ratchet against dm/tools/lint_c_baseline.tsv as lint_python.py's: more fail, fewer fail too.
#             --update lowers counts and never adds one (fix a new finding, or `// NOLINT(<check>)` it with a reason); --rebaseline only with a raised
#             LLVM_VERSION in lint-c.yml; --against REV fails on entries or counts REV lacks. Sources named alone report no fixed findings.
# Needs LLVM's clang-tidy (Apple's toolchain has none): LLVM=<folder with bin/>, else Homebrew's llvm@23 or llvm, else PATH; the baseline names its version.
# Usage: lint_c.py compdb [--game ID] | tidy [--update | --rebaseline] [--against REV] [--report FILE] [-j N] [FILE...]
import argparse, collections, concurrent.futures, json, os, pathlib, re, shlex, shutil, subprocess, sys

REPO = pathlib.Path(__file__).resolve().parents[2]
DB_DIR = REPO / 'dm' / 'host' / 'gen'
BASELINE = REPO / 'dm' / 'tools' / 'lint_c_baseline.tsv'
WORKFLOW = '.github/workflows/lint-c.yml'
LINT_DIRS = ('dm/host', 'dm/gfx')
SKIP = ('dm/third_party/', 'dm/host/gen/', 'dm/gfx/nvn/include/')
SOURCES = ('.c', '.m', '.mm', '.cpp')
# Clang's analyzer starts path-sensitive checks only from the main file's functions (AnalysisConsumer::getModeForDecl): units made of parts
# get -analyzer-opt-analyze-headers. Not the tests that include them: each would take minutes
UNITS = ('dm/gfx/nvn/nvn_mtl.m',)
UNIT_FLAGS = ['-Xclang', '-analyzer-opt-analyze-headers']
DEFAULT_GAME = '0400486024DEC000'

def llvm_tool(name):
    if os.environ.get('LLVM'):
        p = pathlib.Path(os.environ['LLVM']) / 'bin' / name
        if p.exists(): return str(p)
        sys.exit(f'{p} not found (LLVM={os.environ["LLVM"]})')
    for f in ('llvm@23', 'llvm') if shutil.which('brew') else ():
        r = subprocess.run(['brew', '--prefix', f], capture_output=True, text=True)
        p = pathlib.Path(r.stdout.strip()) / 'bin' / name
        if r.returncode == 0 and p.exists(): return str(p)
    if shutil.which(name): return shutil.which(name)
    sys.exit(f'{name} not found: set LLVM=<LLVM folder> or brew install llvm@23')

def rel(p):
    try: return pathlib.Path(p).resolve().relative_to(REPO).as_posix()
    except ValueError: return None

def linted(r):
    return r is not None and r.startswith(LINT_DIRS) and not r.startswith(SKIP)

def make_targets(d):
    # without the sanitizer variants, whose sources are the plain tests'
    ts = re.findall(r'^(t_[a-z0-9_]+):', (REPO / d / 'Makefile').read_text(), re.M)
    return sorted({t for t in ts if not re.search(r'tsan|asan|sanitize', t)})

def dry_run(d, targets, game):
    r = subprocess.run(['make', '-n', '-B', '-k', '-w', f'GAME={game}', *targets], cwd=REPO / d, capture_output=True, text=True)
    dirs, out = [REPO / d], []   # make -w names the folder each recursive make enters and leaves
    for line in r.stdout.split('\n'):
        m = re.match(r"make(?:\[\d+\])?: (Entering|Leaving) directory [`']([^']+)'", line)
        if m:
            if m.group(1) == 'Entering': dirs.append(pathlib.Path(m.group(2)))
            elif len(dirs) > 1: dirs.pop()
            continue
        out.append((dirs[-1], line))
    return out

def entries(cwd, line, sysroot):
    try: argv = shlex.split(line)
    except ValueError: return []
    if not argv or os.path.basename(argv[0]) not in ('clang', 'cc', 'clang++', 'gcc'): return []
    flags, srcs, i = [], [], 1
    while i < len(argv):
        a = argv[i]
        if a in ('-o', '-framework', '-MF', '-MT', '-arch'):
            i += 2
            continue
        if a in ('-c', '-Werror', '-MMD', '-MD', '-MP') or a.startswith(('-fsanitize', '-l', '-L', '-Wl,')):
            i += 1
            continue
        if a.startswith('-'):
            flags.append(a)
            if a in ('-I', '-D', '-include', '-isystem', '-x', '-isysroot', '-iquote'):
                flags.append(argv[i + 1])
                i += 1
        elif a.endswith(SOURCES): srcs.append(a)
        i += 1
    if sysroot and '-isysroot' not in flags: flags = ['-isysroot', sysroot] + flags
    return [{'directory': str(cwd), 'file': str((cwd / s).resolve()), 'arguments': ['clang', *flags, '-c', str((cwd / s).resolve())]} for s in srcs]

def compdb(game):
    # needed for the sources to parse: make writes the first two, dmsh_chain.h is committed
    subprocess.run(['make', '-s', '-C', str(REPO / 'dm/host'), f'GAME={game}', 'game_profile', 'host_msl'], check=True)
    sysroot = subprocess.run(['xcrun', '--show-sdk-path'], capture_output=True, text=True).stdout.strip() if sys.platform == 'darwin' else ''
    runs = [('dm/host', ['dmhost']), ('dm/gfx/tex/tests', make_targets('dm/gfx/tex/tests')), ('dm/gfx/tests', make_targets('dm/gfx/tests')),
            ('dm/host/tests', make_targets('dm/host/tests'))]
    db = merge_entries(e for d, targets in runs for cwd, line in dry_run(d, targets, game) for e in entries(cwd, line, sysroot))
    DB_DIR.mkdir(parents=True, exist_ok=True)
    (DB_DIR / 'compile_commands.json').write_text(json.dumps(db, indent=1) + '\n')
    print(f'{DB_DIR.relative_to(REPO)}/compile_commands.json: {len(db)} sources')
    return db

def merge_entries(es):
    db, seen = [], set()
    for e in es:
        r = rel(e['file'])
        if linted(r) and r not in seen:
            seen.add(r)
            if r in UNITS: e = {**e, 'arguments': [e['arguments'][0], *UNIT_FLAGS, *e['arguments'][1:]]}
            db.append(e)
    return sorted(db, key=lambda e: e['file'])

DIAG = re.compile(r'^(.+?):(\d+):(\d+): (warning|error): (.*?) \[([^\]]+)\]$')

def tidy_one(tool, f):
    r = subprocess.run([tool, '-p', str(DB_DIR), '--quiet', f], capture_output=True, text=True, cwd=REPO)
    if r.returncode < 0 or 'PLEASE submit a bug report' in r.stderr:   # a crash would otherwise read as a file without findings
        sys.exit(f'[RED] lint_c: clang-tidy crashed on {rel(f)} (exit {r.returncode}):\n{r.stderr[-4000:]}')
    return f, parse_diags(r.stdout)

def parse_diags(text):
    # a finding of several checks counts for each
    out = []
    for line in text.split('\n'):
        m = DIAG.match(line)
        if not m: continue
        path, ln, col, _, msg, checks = m.groups()
        r = rel(path)
        if not linted(r): continue
        for check in checks.split(','):
            # the compiler's own warnings are the build's; a parse error is kept (the file was not fully checked)
            if check.startswith('clang-diagnostic-') and check != 'clang-diagnostic-error': continue
            out.append((r, int(ln), int(col), check, msg))
    return out

def parse_baseline(text):
    base = collections.Counter()
    for line in text.split('\n'):
        if line and not line.startswith('#'):
            f, check, n = line.split('\t')
            base[(f, check)] = int(n)
    return base

def baseline_llvm(text):
    m = re.search(r'^#.*Made with LLVM (\d+\.\d+\.\d+)', text, re.M)
    return m.group(1) if m else None

def read_baseline():
    return parse_baseline(BASELINE.read_text()) if BASELINE.exists() else collections.Counter()

def compare(counts, base, partial=False):
    # A partial run finds nothing fixed: a file's findings can come from other sources too (a header, a test that includes the file)
    worse = [(k, n, base.get(k, 0)) for k, n in sorted(counts.items()) if n > base.get(k, 0)]
    better = [] if partial else [(k, counts.get(k, 0), b) for k, b in sorted(base.items()) if counts.get(k, 0) < b]
    return worse, better

def shrink(base, counts):
    return collections.Counter({k: min(n, counts.get(k, 0)) for k, n in base.items() if min(n, counts.get(k, 0)) > 0})

def summary(text):
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f: f.write(text + '\n')

def write_baseline(counts, version):
    lines = ['# clang-tidy findings of dm/host and dm/gfx per file and check (dm/tools/lint_c.py tidy; checks in .clang-tidy), the ratchet CI holds pull requests to. It only',
             '# shrinks: after fixing findings, python3 dm/tools/lint_c.py compdb && python3 dm/tools/lint_c.py tidy --update; a new finding is fixed, or silenced on its line with',
             f'# // NOLINT(<check>) and a reason. Never edit by hand to add. Made with {version}.',
             '# file\tcheck\tfindings']
    lines += [f'{f}\t{c}\t{n}' for (f, c), n in sorted(counts.items()) if n > 0]
    BASELINE.write_text('\n'.join(lines) + '\n')

def git(*a): return subprocess.run(['git', *a], cwd=REPO, capture_output=True, text=True)

def llvm_pin(text):
    m = re.search(r'^\s+LLVM_VERSION: (\d+)\.(\d+)\.(\d+)$', text, re.M)
    return tuple(map(int, m.groups())) if m else None

def against(rev, base):
    old = git('show', f'{rev}:{BASELINE.relative_to(REPO).as_posix()}')
    if old.returncode:
        print(f'lint_c: no baseline at {rev}: nothing to compare')
        return 0
    before, after = llvm_pin(git('show', f'{rev}:{WORKFLOW}').stdout), llvm_pin((REPO / WORKFLOW).read_text())
    if after is None:
        print(f'[RED] lint_c: no LLVM_VERSION in {WORKFLOW}')
        return 1
    if before is not None and after > before:
        print(f'lint_c: LLVM_VERSION raised since {rev}: the baseline may be rewritten')
        return 0
    was = parse_baseline(old.stdout)
    grown = [(k, n, was.get(k, 0)) for k, n in sorted(base.items()) if n > was.get(k, 0)]
    for (f, c), n, w in grown: print(f'::error file={BASELINE.relative_to(REPO).as_posix()}::baseline grew: {f} {c} {w} -> {n}')
    if grown: print(f'lint_c: the baseline gained {len(grown)} entries over {rev}: fix the new findings instead, or silence a false positive with // NOLINT(<check>) (it only shrinks)')
    return 1 if grown else 0

def tidy(args):
    tool = llvm_tool('clang-tidy')
    version = re.search(r'LLVM version (\S+)', subprocess.run([tool, '--version'], capture_output=True, text=True).stdout)
    version = f'LLVM {version.group(1)}' if version else 'an unknown LLVM'
    dbf = DB_DIR / 'compile_commands.json'
    if not dbf.exists(): sys.exit(f'{dbf.relative_to(REPO)} is missing: run lint_c.py compdb first')
    if (args.update or args.rebaseline) and args.files: sys.exit('--update and --rebaseline write the whole baseline: run them without files')
    files = [e['file'] for e in json.loads(dbf.read_text())]
    if args.files: files = [str((REPO / f).resolve()) for f in args.files]
    found = set()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for i, (f, out) in enumerate(ex.map(lambda f: tidy_one(tool, f), files)):
            found.update(out)   # a header's finding comes once from every source that includes it
            print(f'[{i + 1}/{len(files)}] {rel(f)}: {sum(1 for o in out if o[0] == rel(f))} findings', flush=True)
    found = sorted(found)
    counts = collections.Counter((f, c) for f, _, _, c, _ in found)
    if args.report:
        pathlib.Path(args.report).write_text(''.join(f'{f}:{l}:{c}: {msg} [{chk}]\n' for f, l, c, chk, msg in found))
    print(f'{version}: {len(found)} findings in {len({f for f, _ in counts})} files, {len(counts)} file/check pairs')
    if args.rebaseline:
        write_baseline(counts, version)
        print(f'wrote {BASELINE.relative_to(REPO)}')
        return 0
    base = read_baseline()
    made = baseline_llvm(BASELINE.read_text()) if BASELINE.exists() else None
    if made and made.split('.')[0] != version.removeprefix('LLVM ').split('.')[0]:
        print(f'::warning::the baseline was made with LLVM {made}, this is {version}: another major version finds other things')
    if args.update:
        shrunk = shrink(base, counts)
        write_baseline(shrunk, f'LLVM {made}' if made else version)
        print(f'lint_c: baseline {sum(base.values())} -> {sum(shrunk.values())} findings')
        base = shrunk
    worse, better = compare(counts, base, partial=bool(args.files))
    md = []
    for (f, c), n, b in worse:
        print(f'{f}: {c}: {n} findings, the baseline has {b}')
        md.append(f'- `{f}` {c}: {n} findings, the baseline has {b}')
        for ff, l, col, cc, msg in found:   # counts cannot tell which finding is new
            if ff == f and cc == c:
                print(f'::error file={f},line={l},col={col}::{msg} [{c}] ({n} in this file, the baseline has {b})')
                md.append(f'  - line {l}: {msg}')
    for (f, c), n, b in better:
        print(f'::error file={f}::{c}: {n} findings, the baseline still has {b}: shrink it (python3 dm/tools/lint_c.py tidy --update)')
    line = f'baseline {sum(base.values())} findings; {len(worse)} file/check pairs above it, {len(better)} below it'
    print(line)
    summary(f'### clang-tidy ({version})\n\n{len(found)} findings, {line}.'
            + ('\n\nNew findings (fix them, or silence a false positive on its line with `// NOLINT(<check>)` and a reason):\n\n' + '\n'.join(md) if worse else '')
            + (f'\n\n{len(better)} file/check pairs are below the baseline: `python3 dm/tools/lint_c.py tidy --update` shrinks it.' if better else ''))
    rc = 1 if worse or better else 0
    if args.against: rc |= against(args.against, base)
    return rc

def main():
    ap = argparse.ArgumentParser(description='C and Objective-C lint of dm/host and dm/gfx (clang-tidy with a ratchet baseline)')
    sub = ap.add_subparsers(dest='cmd', required=True)
    p = sub.add_parser('compdb')
    p.add_argument('--game', default=os.environ.get('GAME') or DEFAULT_GAME)
    p = sub.add_parser('tidy')
    g = p.add_mutually_exclusive_group()
    g.add_argument('--update', action='store_true', help='shrink the baseline to the findings the code still has')
    g.add_argument('--rebaseline', action='store_true', help='write every current finding (only after raising LLVM_VERSION)')
    p.add_argument('--against', metavar='REV', help='also fail when the baseline grew over the one at REV')
    p.add_argument('--report')
    p.add_argument('-j', '--jobs', type=int, default=os.cpu_count())
    p.add_argument('files', nargs='*')
    a = ap.parse_args()
    if a.cmd == 'compdb':
        compdb(a.game)
        return 0
    return tidy(a)

if __name__ == '__main__':
    sys.exit(main())
