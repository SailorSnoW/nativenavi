#!/usr/bin/env python3
# Merges recorded prewarm lists (a player's pso_user.tsv, a DM_PSO_RECORD file) into the profile's pso_manifest.tsv (docs/development.md#prewarm-lists).
# Usage: python3 scripts/merge_prewarm_seeds.py <list.tsv>... [--apply] [--note TEXT]. Without --apply it only reports what it would add; the lists are only read.
#   Exit 1 when a list or the profile's files cannot be read, or the host's key parser (dm/tools/prewarm_skey.c) does not build.
import argparse, os, re, subprocess, sys, tempfile
from collections import Counter
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
NVN = REPO / 'dm/gfx/nvn'
PARSER = REPO / 'dm/tools/prewarm_skey.c'
sys.path.insert(0, str(REPO / 'dm/games'))
import game as gamemod

LIBRARY = r'[0-9a-f]{16}_b'
ROW = re.compile(rf'R\t[0-9a-f]+|M\t(?:-|{LIBRARY})\t[0-9a-f]+')
STAGES = {'vertex': '0', 'pixel': '1', 'compute': '5', 'task': '36', 'mesh': '47'}   # a task or mesh shader's stage is NVN2's or the host's slot
# S: a shader alone, which the host no longer prewarms; C: every compute shader of the map is prewarmed (DM_PSO_ALL_COMPUTE); G: nxnative's geometry pairs
IGNORED = ('S', 'C', 'G')


def read_rows(path):
    return [ln for ln in Path(path).read_text(encoding='utf-8').splitlines() if ln.strip() and not ln.startswith('#')]


def read_map(path):
    """{library name: stage}"""
    out = {}
    for ln in Path(path).read_text(encoding='utf-8').splitlines():
        c = ln.split('\t', 4)
        if not ln.startswith(('#', 'package\t')) and len(c) >= 4: out[c[3] + '_b'] = c[2]
    return out


def other_maps(profile):
    """[(versions, shader map)] of the derived profiles of this title in DM_GAMES_DIR (the packing script's <build folder>/profile)"""
    out = []
    for root in gamemod.extra_dirs():
        d = root / profile.id
        if not (d / 'game.tsv').is_file() or not (d / 'shader_map.tsv').is_file() or d.resolve() == profile.dir: continue
        g = gamemod.Game(d)
        if g.get('title_id', '').upper() != profile.id: continue
        builds = g.builds()
        label = ', '.join(builds[v]['display'] if v in builds else v for v in g.words('shader_versions', g.get('package', '')))
        out.append((label or str(d), read_map(d / 'shader_map.tsv')))
    return out


def parse(parser, rows):
    """{R or M row: [vertex key, its stage, pixel key, its stage, normalized hex]}, or ['-'] for a row the host skips"""
    queries = {r: r[0] + '\t' + r.rsplit('\t', 1)[1] for r in rows if ROW.fullmatch(r)}
    if not queries: return {}
    out = subprocess.run([str(parser)], input=''.join(q + '\n' for q in queries.values()), text=True, stdout=subprocess.PIPE, check=True).stdout.splitlines()
    if len(out) != len(queries): raise ValueError(f'the key parser gave {len(out):,} lines for {len(queries):,} rows')
    return {r: p.split('\t') for r, p in zip(queries, out)}


def row_key(row, parsed):
    """the pipeline as the host's prewarm tells rows apart (warm_parse): the normalized key, and an M row's task shader; () when the host skips it"""
    p = parsed.get(row)
    if not p or p == ['-']: return ()
    return ('R', p[4]) if row[0] == 'R' else ('M', row.split('\t')[1], p[4])


def check(row, p, shaders):
    vertex, vs_stage, pixel, fs_stage, _ = p
    refs = [(vertex, 'mesh' if row[0] == 'M' else 'vertex', vs_stage)] + ([(pixel, 'pixel', fs_stage)] if pixel else [])
    task = row.split('\t')[1] if row[0] == 'M' else '-'
    if task != '-': refs.append((task, 'task', ''))
    if any(k not in shaders for k, _, _ in refs): return 'missing shader'
    if any(shaders[k] != stage or (n and n not in STAGES.get(stage, '')) for k, stage, n in refs): return 'stage mismatch'
    return ''


def plan(lists, profile, parser, note=None, say=print):
    """→ (manifest path, its bytes as read, its new text or None)"""
    shaders, others = read_map(profile.table('shader_map.tsv')), other_maps(profile)
    path = profile.table('pso_manifest.tsv')
    original = path.read_bytes()
    text = original.decode('utf-8')
    listed = [ln for ln in text.splitlines() if ROW.fullmatch(ln)]
    parsed = parse(parser, listed)
    seen = {row_key(r, parsed) for r in listed} - {()}
    added, kinds, names = [], Counter(), []
    for f in lists:
        rows = read_rows(f)
        parsed, counts = parse(parser, rows), Counter()
        for row in rows:
            kind = row.split('\t', 1)[0]
            if kind in IGNORED: counts[f'ignored {kind}'] += 1; continue
            if not ROW.fullmatch(row): counts['invalid row'] += 1; continue
            key = row_key(row, parsed)
            if not key: counts['invalid pipeline'] += 1; continue
            if key in seen: counts['duplicate'] += 1; continue
            seen.add(key)
            why = check(row, parsed[row], shaders)
            other = next((label for label, m in others if not check(row, parsed[row], m)), None) if why else None
            if not why: added.append(row); kinds[kind] += 1; counts['added'] += 1
            elif other: counts[f'only in {other}'] += 1   # the packing script carries the list to that version's keys
            else: counts[why] += 1
        if counts['added']: names.append(Path(f).name)
        say(f'{f}: {len(rows):,} rows: ' + (', '.join(f'{n:,} {k}' for k, n in sorted(counts.items())) or 'none'))
    if not added: return path, original, None
    breakdown = ', '.join(f'{n:,} {k}' for k, n in sorted(kinds.items()))
    head = note or f'The rows of {", ".join(names)} that this list did not have ({breakdown}), merged by scripts/merge_prewarm_seeds.py.'
    say(f'{path}: {len(added):,} new rows ({breakdown})')
    if text and not text.endswith('\n'): text += '\n'
    return path, original, text + ('' if head.startswith('#') else '# ') + head + '\n' + ''.join(r + '\n' for r in added)


def write(path, original, content):
    if path.is_symlink() or path.read_bytes() != original: raise ValueError(f'{path}: changed while the lists were read, run it again')
    fd, name = tempfile.mkstemp(dir=path.parent, prefix='.pso_manifest.')
    tmp = Path(name)
    try:
        with os.fdopen(fd, 'w', encoding='utf-8') as out: out.write(content)
        tmp.chmod(path.stat().st_mode & 0o777)
        tmp.replace(path)
    finally:
        tmp.unlink(missing_ok=True)


def main():
    ap = argparse.ArgumentParser(description='Merge recorded prewarm lists into the profile\'s pso_manifest.tsv: rows the host would build that the list lacks, '
                                 'checked with the host\'s key parser against the profile\'s shader map. DM_GAMES_DIR names the derived profiles of other versions '
                                 '(<build folder>/profile), whose rows are counted and left out.')
    ap.add_argument('lists', nargs='+', type=Path, help='recorded lists, such as ~/Library/Application Support/NativeNavi/<title ID>/pso_user.tsv')
    ap.add_argument('--apply', action='store_true', help='append the new rows to the profile\'s pso_manifest.tsv (without it, only report)')
    ap.add_argument('--note', help='the comment line above the new rows (default: the lists\' names and the counts)')
    a = ap.parse_args()
    if a.note and '\n' in a.note: ap.error('--note takes one line')
    try:
        profile = gamemod.Game(REPO / 'dm/games' / gamemod.DEFAULT)
        with tempfile.TemporaryDirectory(prefix='prewarm-skey-') as work:
            parser = Path(work) / 'prewarm_skey'
            subprocess.run(['clang', '-std=c11', '-I', str(NVN), '-I', str(NVN / 'include'), str(PARSER), '-o', str(parser)], check=True)
            path, original, content = plan(a.lists, profile, parser, a.note)
        if content is None: print('No new rows.')
        elif a.apply: write(path, original, content); print(f'Updated {path}.')
        else: print('Preview: --apply appends them.')
    except (OSError, UnicodeDecodeError, ValueError, subprocess.CalledProcessError) as e:
        print(f'[RED] merge_prewarm_seeds.py: {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main())
