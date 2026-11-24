#!/usr/bin/env python3
# Python lint (ruff) and type check (pyright basic) of scripts/ and dm/ with a ratchet against dm/tools/lint_python_baseline.tsv (.github/workflows/lint-python.yml):
# more findings than a key's count fail, and so do fewer. --update lowers counts, never adds one (fix a new finding, or noqa / `# pyright: ignore[<rule>]` it
# with a reason); --rebaseline only with a raised pin in .github/requirements/lint.txt; --against REV fails on entries or counts REV lacks.
# Run with the pinned tools (nix develop .#lint): other versions find other things.
import argparse, collections, json, os, re, subprocess, sys, pathlib

REPO = pathlib.Path(__file__).resolve().parents[2]
BASELINE = REPO / 'dm/tools/lint_python_baseline.tsv'
REQS = '.github/requirements/lint.txt'
HEADER = '# tool\tfile\trule\tcount\tmessage   (dm/tools/lint_python.py; shrink with --update, never edit by hand to add)\n'

def rel(p): return pathlib.Path(p).resolve().relative_to(REPO).as_posix()
def norm(msg): return re.sub(r'\s+', ' ', msg).strip()   # pyright's messages span lines and use no-break spaces

def run(cmd):
    r = subprocess.run(cmd, cwd=REPO, capture_output=True, text=True)
    if not r.stdout.strip(): sys.exit(f'[RED] lint_python: {" ".join(cmd[:3])} printed nothing (exit {r.returncode}):\n{r.stderr}')
    return r.stdout

def ruff():
    out = json.loads(run([sys.executable, '-m', 'ruff', 'check', '--no-cache', '--exit-zero', '--output-format', 'json', '.']))
    return [('ruff', rel(f['filename']), f['code'] or 'syntax', norm(f['message']), f['location']['row']) for f in out]

def pyright():
    out = json.loads(run([sys.executable, '-m', 'pyright', '--outputjson', '--pythonpath', sys.executable]))
    return [('pyright', rel(d['file']), d.get('rule') or d['severity'], norm(d['message']), d['range']['start']['line'] + 1)
            for d in out['generalDiagnostics'] if d['severity'] in ('error', 'warning')]

def load(text):
    base = collections.Counter()
    for line in text.splitlines():
        if not line or line.startswith('#'): continue
        tool, path, rule, n, msg = line.split('\t', 4)
        base[(tool, path, rule, msg)] = int(n)
    return base

def write(base):
    rows = sorted(f'{k[0]}\t{k[1]}\t{k[2]}\t{n}\t{k[3]}\n' for k, n in base.items() if n > 0)
    BASELINE.write_text(HEADER + ''.join(rows))

def git(*a): return subprocess.run(['git', *a], cwd=REPO, capture_output=True, text=True)

def versions(text):
    # reject missing, duplicate or unsupported pins instead of authorizing a baseline rewrite
    pins = {}
    for line in text.splitlines():
        name, _, version = line.split('#', 1)[0].strip().partition('==')
        name, version = name.strip().lower(), version.strip()
        if name not in ('ruff', 'pyright'): continue
        if name in pins or not re.fullmatch(r'\d+\.\d+\.\d+', version):
            raise ValueError(f'{REQS}: {name} must have one numeric release pin (name==X.Y.Z)')
        pins[name] = tuple(map(int, version.split('.')))
    if set(pins) != {'ruff', 'pyright'}: raise ValueError(f'{REQS}: both ruff and pyright need numeric release pins (name==X.Y.Z)')
    return pins

def against(rev, base):
    # only an upgraded checker may gain entries or counts
    old = git('show', f'{rev}:{rel(BASELINE)}')
    if old.returncode:
        print(f'lint_python: no baseline at {rev}: nothing to compare')
        return 0
    old_reqs = git('show', f'{rev}:{REQS}')
    if old_reqs.returncode:
        print(f'[RED] lint_python: cannot read {REQS} at {rev}: {old_reqs.stderr.strip()}')
        return 1
    try:
        before, after = versions(old_reqs.stdout), versions((REPO / REQS).read_text())
    except (OSError, ValueError) as e:
        print(f'[RED] lint_python: {e}')
        return 1
    upgraded = {t for t in before if after[t] > before[t]}
    for t in sorted(upgraded): print(f'lint_python: {t} upgraded since {rev}: its baseline may be rewritten')
    was = load(old.stdout)
    grown = [(k, n, was[k]) for k, n in base.items() if n > was[k] and k[0] not in upgraded]
    for (tool, path, rule, msg), n, w in grown: print(f'::error file={rel(BASELINE)}::baseline grew: {tool} {path} {rule} {w} -> {n}: {msg}')
    if grown: print(f'lint_python: the baseline gained {len(grown)} entries over {rev}: fix the new findings instead (it only shrinks)')
    return 1 if grown else 0

def summary(md):
    if os.environ.get('GITHUB_STEP_SUMMARY'):
        with open(os.environ['GITHUB_STEP_SUMMARY'], 'a') as f: f.write(md + '\n')

def check(args):
    found = ruff() + pyright()
    cur = collections.Counter(f[:4] for f in found)
    base = load(BASELINE.read_text()) if BASELINE.exists() else collections.Counter()
    if args.rebaseline:
        write(cur)
        print(f'lint_python: wrote {sum(cur.values())} findings to {rel(BASELINE)}')
        return 0
    new = {k: n - base[k] for k, n in cur.items() if n > base[k]}
    fixed = {k: base[k] - cur[k] for k in base if base[k] > cur[k]}
    if args.update:
        shrunk = collections.Counter({k: min(n, cur[k]) for k, n in base.items()})
        write(shrunk)
        print(f'lint_python: baseline {sum(base.values())} -> {sum(shrunk.values())} findings')
        base, fixed = shrunk, {}
    gha = os.environ.get('GITHUB_ACTIONS') == 'true'
    for tool, path, rule, msg, line in found:
        k = (tool, path, rule, msg)
        if new.get(k):   # with several of one key, which one is new is not known
            print(f'::error file={path},line={line},title={tool} {rule}::{msg}' if gha else f'{path}:{line}: {tool} {rule}: {msg}')
    for (tool, path, rule, msg), n in sorted(fixed.items()):
        print(f'fixed, still in the baseline: {tool} {path} {rule} x{n}: {msg}')
    n_new, n_fixed = sum(new.values()), sum(fixed.values())
    rows = ['| tool | findings | in the baseline | new | fixed, still listed |', '|---|---:|---:|---:|---:|']
    for t in ('ruff', 'pyright'):
        rows.append(f'| {t} | {sum(n for k, n in cur.items() if k[0] == t)} | {sum(n for k, n in base.items() if k[0] == t)} '
                    f'| {sum(n for k, n in new.items() if k[0] == t)} | {sum(n for k, n in fixed.items() if k[0] == t)} |')
    verdict = ('New findings: fix them (or silence one on its line with `# noqa: <rule>` / `# pyright: ignore[<rule>]` and a reason).' if n_new else
               'Fixed findings are still in the baseline: run `python3 dm/tools/lint_python.py --update` and commit it.' if n_fixed else
               'No new findings.')
    summary('### Python lint and types (ruff, pyright basic)\n\n' + '\n'.join(rows) + f'\n\n{verdict}\n')
    print(f'lint_python: {sum(cur.values())} findings, {sum(base.values())} in the baseline, {n_new} new, {n_fixed} fixed but still listed')
    if n_fixed: print('lint_python: run python3 dm/tools/lint_python.py --update to shrink the baseline')
    rc = 1 if n_new or n_fixed else 0
    if args.against: rc |= against(args.against, load(BASELINE.read_text()) if BASELINE.exists() else collections.Counter())
    return rc

def main():
    ap = argparse.ArgumentParser(description='ruff and pyright with a ratchet baseline')
    g = ap.add_mutually_exclusive_group()
    g.add_argument('--update', action='store_true', help='shrink the baseline to the findings the code still has')
    g.add_argument('--rebaseline', action='store_true', help='write every current finding (only after raising a pinned tool version)')
    ap.add_argument('--against', metavar='REV', help='also fail when the baseline grew over the one at REV')
    args = ap.parse_args()
    return check(args)

if __name__ == '__main__':
    sys.exit(main())
