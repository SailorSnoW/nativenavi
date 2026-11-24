#!/usr/bin/env python3
"""docs/env_switches.tsv: the DM_* switches the code reads (class and meaning kept by hand).
Usage: env_switches.py [--check]   (--check: exit 1 when the table and the sources disagree, or a row lacks a meaning or a known class)"""
from pathlib import Path
import re
import subprocess
import sys

REPO = Path(__file__).resolve().parents[2]
TABLE = REPO / 'docs/env_switches.tsv'
HEAD = 'name\tclass\tmeaning\n'
CLASSES = ('player', 'path', 'switch', 'diagnostic', 'test')
CODE = ('.c', '.m', '.h', '.inc', '.swift', '.py', '.sh', '.S')
NAME = r'DM_[A-Z0-9_]*[A-Z0-9]'
READ = r'(?:getenv\(|environ(?:\.get\(|\[))\s*["\'](' + NAME + r')["\']'


def tracked():
    out = subprocess.run(['git', 'ls-files', 'dm', 'scripts', 'docs', 'README.md'], cwd=REPO, capture_output=True, text=True, check=True).stdout.split('\n')
    return [p for p in out if p and 'third_party' not in p.split('/')]


def scan():
    reads, docs, macros, direct = {}, {}, set(), set()
    for p in tracked():
        path = REPO / p
        if not path.is_file(): continue
        text = path.read_text(errors='replace')
        if p.endswith('.md'):
            for n in set(re.findall(r'`(' + NAME + r')', text)): docs.setdefault(n, set()).add(p)
            continue
        if not p.endswith(CODE) and not p.startswith('scripts/'): continue
        # macros (the profile header's DM_GAME_*) are not switches unless something also reads them from the environment (DM_GAME_ENV)
        macros |= set(re.findall(r'#\s*define\s+(' + NAME + r')', text))
        for body in re.findall(r'\benum\s*\w*\s*\{([^}]*)\}', text): macros |= set(re.findall(r'\b(' + NAME + r')\s*=', body))
        if '/tests/' in p:   # a test's own switches only; the names in its fixtures are data
            found = re.findall(READ, text)
        elif p.endswith('.sh'): found = re.findall(r'\$\{?(' + NAME + r')\b', text) + re.findall(r'["\'](' + NAME + r')["\']', text)
        else: found = re.findall(r'["\'](' + NAME + r')=?["\']', text)   # any quoted name: a helper such as envdir() reads it for its caller
        direct |= set(re.findall(READ, text))
        for n in set(found): reads.setdefault(n, set()).add(p)
    for n in macros - direct:
        reads.pop(n, None)
        docs.pop(n, None)
    return reads, docs


def guess(name, files, player):
    if name in player: return 'player'
    if all('/tests/' in f for f in files): return 'test'
    if re.search(r'_(DIR|PATH|FILE|ROOT)$', name): return 'path'
    return 'diagnostic'


def rows():
    reads, docs = scan()
    player = {l.split('\t')[0] for l in (REPO / 'dm/games/settings.tsv').read_text().split('\n') if l.startswith('DM_')}
    kept = {}
    if TABLE.exists():
        for l in TABLE.read_text().split('\n'):
            f = l.split('\t')
            if len(f) == 3 and f[0].startswith('DM_'): kept[f[0]] = f[1:]
    out, bad = [], []
    for n in sorted(reads):
        cls, meaning = kept.get(n, [guess(n, reads[n], player), ''])
        if cls not in CLASSES or not meaning.strip(): bad.append(n)
        out.append(f'{n}\t{cls}\t{meaning}\n')
    unread = sorted(n for n in docs if n not in reads and not n.endswith('_'))
    return HEAD + ''.join(out), bad, unread


if __name__ == '__main__':
    text, bad, unread = rows()
    if unread: print('quoted in the docs but read nowhere (check that each mention says it was removed): ' + ' '.join(unread))
    if sys.argv[1:] == ['--check']:
        problems = []
        if not TABLE.exists() or TABLE.read_text() != text: problems.append('docs/env_switches.tsv does not list the switches the code reads: run dm/tools/env_switches.py, then write the new rows\' meanings')
        if bad: problems.append('switches without a meaning or a class (one of ' + ', '.join(CLASSES) + '): ' + ' '.join(bad))
        print('\n'.join(problems))
        sys.exit(1 if problems else 0)
    TABLE.write_text(text)
    print(f'{TABLE.relative_to(REPO)}: {text.count(chr(10)) - 1} switches, {len(bad)} without a meaning')
