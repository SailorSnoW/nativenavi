#!/usr/bin/env python3
# Upstream nxnative's commits for the agents that port them (.claude/skills/upstream-port/SKILL.md) and the ledger of verdicts (upstream_ledger.tsv).
# Usage: upstream_sync.py fetch   [--mirror DIR] [--url URL]
#        upstream_sync.py list    [--mirror DIR] [--tree DIR] [--since REF] [--all] [--tsv]
#        upstream_sync.py show    <sha> [--mirror DIR] [path ...]
#        upstream_sync.py pick    <sha> [path ...] [--mirror DIR] [--tree DIR]
#        upstream_sync.py summary <log.txt> [--frames N,N]
#        upstream_sync.py record  <sha> <verdict> [--local SHA[,SHA]] [--why TEXT] [--mirror DIR]
# The mirror (blobless) is $NATIVENAVI_UPSTREAM_MIRROR, else ~/.cache/nativenavi/nxnative.git. pick merges three ways; exit 1 on conflict markers.
# Verdicts: ported, partial, docs, no-op, skip, deferred.
import argparse, collections, importlib.util, os, re, subprocess, sys, tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
LEDGER = REPO / 'dm/tools/upstream_ledger.tsv'
GAME_ID = '0400486024DEC000'
URL = 'https://github.com/seedds/nxnative.git'
VERDICTS = ('ported', 'partial', 'docs', 'no-op', 'skip', 'deferred')
REMOVED = (('dm/ios/', 'ios'), ('dm/gfx/gl/', 'gl'), ('dm/gfx/vk/', 'vk'), ('dm/gfx/vulkan/', 'vk'))
# regenerated after a port, never merged
GENERATED = {'docs/agents/symptoms.md': 'python3 dm/tools/docs_symptoms.py', 'docs/agents/game-patches.md': 'python3 dm/tools/docs_patches.py',
             'dm/gfx/nvn/dmsh_chain.h': 'python3 dm/gfx/tools/dmsh_chain.py --header'}
GAMES_TABLE = 'dm/games/README.md'   # one row here


def mirror_dir(arg: str | None) -> Path:
    return Path(arg or os.environ.get('NATIVENAVI_UPSTREAM_MIRROR') or Path.home() / '.cache/nativenavi/nxnative.git')


def git(repo: Path, *args: str, check: bool = True) -> str:
    r = subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True)
    if check and r.returncode: sys.exit(f'[RED] git {" ".join(args[:3])}: {r.stderr.strip()[-400:]}')
    return r.stdout


def blob(repo: Path, rev: str, path: str) -> bytes | None:
    r = subprocess.run(['git', '-C', str(repo), 'show', f'{rev}:{path}'], capture_output=True)
    return r.stdout if r.returncode == 0 else None


# The ledger: '# key<TAB>value' rows (url, base), then sha, date, verdict, local commits, subject, why
Row = collections.namedtuple('Row', 'sha date verdict local subject why')


def read_ledger(path: Path = LEDGER) -> tuple[dict[str, str], list[Row]]:
    meta: dict[str, str] = {}
    rows: list[Row] = []
    if not path.is_file(): return meta, rows
    for line in path.read_text().splitlines():
        if not line.strip(): continue
        if line.startswith('#'):
            m = re.match(r'#\s*([a-z]+)\t(\S+)$', line)
            if m: meta[m.group(1)] = m.group(2)
            continue
        f = line.split('\t')
        rows.append(Row(*(f + [''] * 6)[:6]))
    return meta, rows


def write_ledger(meta: dict[str, str], rows: list[Row], path: Path = LEDGER) -> None:
    head = ['# The upstream nxnative commits reviewed for this fork, one row each: sha, upstream date, verdict, the local commits that took it, subject, why',
            '# (dm/tools/upstream_sync.py record writes it; the upstream-port skill). Verdicts: ' + ', '.join(VERDICTS)]
    head += [f'# {k}\t{v}' for k, v in sorted(meta.items())]
    body = ['\t'.join(r) for r in sorted(rows, key=lambda r: (r.date, r.sha))]
    path.write_text('\n'.join(head + body) + '\n')


def reviewed(rows: list[Row], sha: str) -> Row | None:
    return next((r for r in rows if sha.startswith(r.sha) or r.sha.startswith(sha)), None)


def chain_files() -> set[str]:   # ledger: '# key<TAB>value' rows (url, base), then sha, date, verdict, local commits, subject, why
    spec = importlib.util.spec_from_file_location('dmsh_chain_files', REPO / 'dm/gfx/tools/dmsh_chain.py')
    if spec is None or spec.loader is None: return set()
    m = importlib.util.module_from_spec(spec)
    try: spec.loader.exec_module(m)
    except Exception: return set()
    return {os.path.normpath(os.path.join('dm/gfx', f)) for f in getattr(m, 'FILES', [])}


def file_tags(status: str, path: str, tree: Path, chain: set[str]) -> list[str]:
    tags = []
    m = re.match(r'dm/games/([0-9A-F]{16})/', path)
    if m: tags.append('oot' if m.group(1) == GAME_ID else f'game:{m.group(1)}')
    tags += [t for p, t in REMOVED if path.startswith(p)]
    if re.search(r'(^|/|_)ios(_|/|\.|$)', path) and 'ios' not in tags: tags.append('ios')
    if status == 'A': tags.append('add')
    elif not (tree / path).exists(): tags.append('gone')
    if path in chain: tags.append('chain')
    if path in GENERATED: tags.append('generated')
    if path == GAMES_TABLE: tags.append('games-table')
    return tags


def commit_files(mirror: Path, sha: str) -> list[tuple[str, str]]:
    out = git(mirror, 'diff-tree', '--no-commit-id', '-r', '--no-renames', '--name-status', sha)
    return [(ln.split('\t')[0][0], ln.split('\t')[-1]) for ln in out.splitlines() if '\t' in ln]


def triage(mirror: Path, sha: str, tree: Path, chain: set[str]) -> dict[str, object]:
    files = [(st, p, file_tags(st, p, tree, chain)) for st, p in commit_files(mirror, sha)]
    away = lambda t: 'gone' in t or any(x.startswith('game:') or x in ('ios', 'gl', 'vk', 'generated', 'games-table') for x in t)
    hints = []
    if files and all(away(t) for _, _, t in files): hints.append('skip?')
    if any('oot' in t for _, _, t in files): hints.append('oot')
    if any('chain' in t for _, _, t in files): hints.append('chain')
    return {'files': files, 'hints': hints}


def cmd_fetch(a: argparse.Namespace) -> None:
    m = mirror_dir(a.mirror)
    if not (m / 'HEAD').is_file():
        m.parent.mkdir(parents=True, exist_ok=True)
        r = subprocess.run(['git', 'clone', '--bare', '--filter=blob:none', a.url, str(m)], capture_output=True, text=True)
        if r.returncode: sys.exit(f'[RED] clone {a.url}: {r.stderr.strip()[-400:]}')
    else:
        git(m, 'fetch', '--filter=blob:none', a.url, '+refs/heads/main:refs/heads/main')
    print(f'{m}: main at {git(m, "log", "-1", "--format=%h %ad %s", "--date=short", "main").strip()}')


def cmd_list(a: argparse.Namespace) -> None:
    m = mirror_dir(a.mirror)
    meta, rows = read_ledger()
    base = a.since or meta.get('base')
    if not base: sys.exit('[RED] no base: the ledger has no "# base" row and --since names none')
    shas = git(m, 'rev-list', '--reverse', f'{base}..main').split()
    chain = chain_files()
    tree = Path(a.tree).resolve()
    todo = []
    for s in shas:
        r = reviewed(rows, s)
        if r and r.verdict != 'deferred' and not a.all: continue
        todo.append((s, r))
    print(f'upstream main has {len(shas)} commits after {base}; {len(todo)} to review' + ('' if a.all else ' (the ledger has the others)'), file=sys.stderr)
    for s, r in todo:
        date, subject = git(m, 'log', '-1', '--format=%ad\t%s', '--date=short', s).strip().split('\t', 1)
        t = triage(m, s, tree, chain)
        files = t['files']
        hints = t['hints']
        assert isinstance(files, list) and isinstance(hints, list)
        mark = f' [ledger: {r.verdict}]' if r else ''
        if a.tsv:
            print('\t'.join([s[:12], date, ','.join(hints), subject + mark, ' '.join(f'{p}({"/".join(tg)})' if tg else p for _, p, tg in files)]))
            continue
        print(f'{s[:12]} {date} {" ".join(f"[{h}]" for h in hints)} {subject}{mark}'.replace('  ', ' '))
        for st, p, tg in files[:14]: print(f'    {st} {p}' + (f'  ({", ".join(tg)})' if tg else ''))
        if len(files) > 14: print(f'    … {len(files) - 14} more')


def cmd_show(a: argparse.Namespace) -> None:
    m = mirror_dir(a.mirror)
    sys.stdout.write(git(m, 'show', '--stat', '--format=%H%n%an %ad%n%n%B', '--date=short', a.sha))
    if a.paths: sys.stdout.write(git(m, 'show', '--format=', a.sha, '--', *a.paths))


def cmd_pick(a: argparse.Namespace) -> None:
    m = mirror_dir(a.mirror)
    tree = Path(a.tree).resolve()
    conflicts = 0
    for st, p in commit_files(m, a.sha):
        if a.paths and not any(p == q or p.startswith(q.rstrip('/') + '/') for q in a.paths): continue
        dst = tree / p
        if p in GENERATED:
            print(f'REGENERATE  {p} ({GENERATED[p]})')
            continue
        if st == 'A':
            if dst.exists():
                print(f'EXISTS-ADD  {p}')
                continue
            data = blob(m, a.sha, p)
            if data is None:
                print(f'?           {p}')
                continue
            dst.parent.mkdir(parents=True, exist_ok=True)
            dst.write_bytes(data)
            print(f'added       {p}')
            continue
        if st == 'D':
            print(('UPSTREAM-DELETES ' if dst.exists() else 'gone        ') + p)
            continue
        if not dst.exists():
            print(f'MISSING     {p}')
            continue
        base, theirs = blob(m, f'{a.sha}^', p), blob(m, a.sha, p)
        if base is None or theirs is None:
            print(f'?           {p}')
            continue
        if b'\0' in base[:8192] or b'\0' in theirs[:8192]:
            print(f'BINARY      {p} (not merged)')
            continue
        with tempfile.TemporaryDirectory() as td:
            bp, tp = Path(td) / 'base', Path(td) / 'theirs'
            bp.write_bytes(base)
            tp.write_bytes(theirs)
            r = subprocess.run(['git', 'merge-file', '-L', 'ours', '-L', 'base', '-L', 'upstream', str(dst), str(bp), str(tp)], capture_output=True, text=True)
        if r.returncode == 0: print(f'merged      {p}')
        elif r.returncode > 0:
            conflicts += 1
            print(f'CONFLICT({r.returncode}) {p}')
        else: sys.exit(f'[RED] git merge-file {p}: {r.stderr.strip()}')
    if conflicts: sys.exit(1)


def summary(text: str, frames: list[int]) -> list[str]:
    out = []
    red = collections.Counter(re.sub(r'\d+(\.\d+)?', 'N', re.sub(r'\b[0-9a-f]{16}(?=\b|_)', '<key>', ln.split('[RED]', 1)[1].strip()))[:180]   # keys first, then numbers
                              for ln in text.splitlines() if '[RED]' in ln)
    out.append(f'red lines: {sum(red.values())}')
    out += [f'  {n} × {t}' for t, n in red.most_common(8)]
    out += [f'  {ln.strip()[:200]}' for ln in text.splitlines() if re.search(r'\bCRASH\b|nn::diag abort|SIGSEGV|SIGBUS', ln)][:3]
    fps = re.findall(r'power up to frame \d+: .*?\(([0-9.]+) fps\)', text)
    out.append('fps per 300 frames: ' + ' '.join(fps))
    gpu = re.findall(r'GPU by process up to frame \d+: the game \d+ ms/s \(([0-9.]+) ms per frame', text)
    out.append('game GPU ms per frame: ' + ' '.join(gpu))
    other = re.findall(r'other processes \d+ ms/s: [^\n]*?\bdmhost (\d+)', text)
    if other: out.append('[another dmhost used the GPU: ' + ' '.join(other) + ' ms/s] the frame figures are not comparable')
    gp = re.findall(r'timing up to frame (\d+):.*?GPU p50 ([0-9.]+) / p95 ([0-9.]+)', text)
    out.append('GPU p50 per window: ' + ' '.join(f'{f}:{p}' for f, p, _ in gp))
    for f in frames:
        r = re.search(rf'residency set up to frame {f}: .*?(\d+) allocations in the set \((\d+) MB\)', text)
        if r: out.append(f'residency set at frame {f}: {r.group(1)} allocations, {r.group(2)} MB')
    mem = re.findall(r'memory up to frame \d+: resident \d+ MB, GPU allocated \d+ MB', text)
    if mem: out.append('last memory line: ' + mem[-1])
    step = re.findall(r'frame step: game time [^;]*of real time\)', text)
    if step: out.append('last frame step line: ' + step[-1])
    return out


def cmd_summary(a: argparse.Namespace) -> None:
    text = Path(a.log).read_text(errors='replace')
    print('\n'.join(summary(text, [int(x) for x in a.frames.split(',') if x] if a.frames else [])))


def cmd_record(a: argparse.Namespace) -> None:
    if a.verdict not in VERDICTS: sys.exit(f'[RED] verdict {a.verdict}: one of {", ".join(VERDICTS)}')
    m = mirror_dir(a.mirror)
    meta, rows = read_ledger()
    full = git(m, 'rev-parse', a.sha).strip()
    date, subject = git(m, 'log', '-1', '--format=%ad\t%s', '--date=short', full).strip().split('\t', 1)
    why = (a.why or '').replace('\t', ' ').replace('\n', ' ')
    rows = [r for r in rows if not (full.startswith(r.sha) or r.sha.startswith(full))]
    rows.append(Row(full[:12], date, a.verdict, a.local or '-', subject.replace('\t', ' '), why))
    meta.setdefault('url', URL)
    write_ledger(meta, rows)
    print(f'{full[:12]} {a.verdict}: {subject}')


def main(argv: list[str] | None = None) -> None:
    ap = argparse.ArgumentParser(description='upstream nxnative commits for this fork')
    sub = ap.add_subparsers(dest='cmd', required=True)
    def add(name: str) -> argparse.ArgumentParser:
        p = sub.add_parser(name)
        p.add_argument('--mirror')
        return p
    p = add('fetch')
    p.add_argument('--url', default=URL)
    p.set_defaults(fn=cmd_fetch)
    p = add('list')
    p.add_argument('--tree', default=str(REPO))
    p.add_argument('--since')
    p.add_argument('--all', action='store_true')
    p.add_argument('--tsv', action='store_true')
    p.set_defaults(fn=cmd_list)
    p = add('show')
    p.add_argument('sha')
    p.add_argument('paths', nargs='*')
    p.set_defaults(fn=cmd_show)
    p = add('pick')
    p.add_argument('sha')
    p.add_argument('paths', nargs='*')
    p.add_argument('--tree', default=str(REPO))
    p.set_defaults(fn=cmd_pick)
    p = sub.add_parser('summary')
    p.add_argument('log')
    p.add_argument('--frames', default='')
    p.set_defaults(fn=cmd_summary)
    p = add('record')
    p.add_argument('sha')
    p.add_argument('verdict')
    p.add_argument('--local')
    p.add_argument('--why')
    p.set_defaults(fn=cmd_record)
    a = ap.parse_args(argv)
    a.fn(a)


if __name__ == '__main__':
    main()
