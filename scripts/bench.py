#!/usr/bin/env python3
# A repeatable benchmark of the game in the Mac host along a route of dm/games/<id>/bench.tsv (docs/development.md#a-benchmark-on-the-mac).
# Usage: python3 scripts/bench.py <id>_build folder [--route NAME] [--save DIR] [--variant 'label:KEY=VALUE,…'] [--runs N] [--fps N] [--env KEY=VALUE] [--headless] [--out DIR]
#   Exit code 0 when every run gave frames, 1 otherwise. Compare variants of one invocation, not invocations: load on the Mac moves the numbers.
import argparse, datetime, json, math, os, re, shutil, signal, statistics, subprocess, sys, time
from typing import NoReturn
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO / 'dm/tools'))
import romfs as rfs


def fail(msg) -> NoReturn:
    print(f'error: {msg}', file=sys.stderr); sys.exit(1)


def newest(paths):
    vs = sorted((p for p in paths if re.fullmatch(r'v\d+', p.name.removeprefix('patched_'))), key=lambda p: int(p.name.removeprefix('patched_')[1:]))
    return vs[-1] if vs else None


def routes(gid):
    f = REPO / 'dm/games' / gid / 'bench.tsv'
    if not f.is_file(): return {}
    out = {}
    for ln in f.read_text().splitlines():
        if not ln.strip() or ln.startswith('#'): continue
        c = ln.split('\t')
        if len(c) < 4: fail(f'{f}: a route line needs name, keys, window start and window length: {ln!r}')
        by_frame = c[2].startswith('f')
        if by_frame != c[3].startswith('f'): fail(f'{f}: the window\'s start and length are both seconds or both frames (f<n>): {ln!r}')
        keys = c[1]
        if keys.startswith('@'):
            kf = f.parent / keys[1:]
            if not kf.is_file(): fail(f'{f}: route {c[0]!r}: no keys file {kf}')
            keys = '@' + str(kf)
        env = dict(kv.split('=', 1) for kv in c[5].split()) if len(c) > 5 and c[5].strip() else {}
        out[c[0]] = {'keys': keys, 'from': float(c[2].lstrip('f')), 'secs': float(c[3].lstrip('f')), 'about': c[4] if len(c) > 4 else '', 'frames': by_frame, 'env': env}
    return out


def pct(xs, p):   # nearest-rank percentile
    if not xs: return None
    s = sorted(xs); return s[min(len(s) - 1, max(0, math.ceil(p / 100 * len(s)) - 1))]


def window_rows(tsv, t0, t1, by_frame=False):
    """with by_frame, t0 and t1 are frame numbers"""
    rows = {}
    with open(tsv, errors='replace') as fh:
        for ln in fh:
            c = ln.rstrip('\n').split('\t')
            if len(c) != 3 or not c[1].startswith('@'): continue
            try: rows.setdefault(int(c[0]), {})[c[1]] = float(c[2])
            except ValueError: continue
    return [r for f, r in sorted(rows.items()) if '@t_s' in r and t0 <= (f if by_frame else r['@t_s']) < t1]


def frames(tsv, t0, t1, by_frame=False):
    sel = window_rows(tsv, t0, t1, by_frame)
    ft = [r['@frame_ms'] for r in sel if r.get('@frame_ms', 0) > 0]
    shown = [r['@presented_ms'] for r in sel if r.get('@presented_ms', 0) > 0]
    gpu = [r['@gpu_ms'] for r in sel if r.get('@gpu_ms', -1) >= 0]
    dropped = sum(1 for r in sel if r.get('@presented_ms') == -1)
    return ft, shown, gpu, dropped, len(sel)


def summary(ft, shown, gpu, dropped, n, secs, budget):
    d = {'frames': n, 'fps': round(n / secs, 1) if secs else None, 'dropped': dropped}
    for name, xs in (('frame', ft), ('shown', shown), ('gpu', gpu)):
        for p in (50, 95, 99): d[f'{name}_p{p}'] = round(pct(xs, p), 2) if xs else None
        d[f'{name}_max'] = round(max(xs), 2) if xs else None
    d['over_budget'] = sum(1 for x in ft if x > budget * 1.5)   # the spikes the Metal HUD shows
    return d


def means(sel):
    """Steadier than the percentiles for a change of a few percent; span: the GPU's first start to last end, which overlapping command buffers do not inflate"""
    def mean(k, ok=lambda x: x >= 0):
        xs = [r[k] for r in sel if k in r and ok(r[k])]
        return round(statistics.fmean(xs), 2) if xs else None
    span = [r['@gpu_end_ms'] - r['@gpu_beg_ms'] for r in sel if r.get('@gpu_end_ms', 0) > r.get('@gpu_beg_ms', 0) > 0]
    sp = pct(span, 50)
    return {'frame_mean': mean('@frame_ms', lambda x: x > 0), 'gpu_mean': mean('@gpu_ms'), 'span_p50': round(sp, 2) if sp is not None else None,
            'sub_cpu_mean': mean('@sub_cpu_ms'), 'passes': mean('@passes'), 'draws': mean('@draws')}


def submit_metrics(sel):
    """The submission's medians a frame and a draw (@draws, @sub_cpu_ms, @sub_instr): a change of a few percent of the replay's cost shows per draw when draw counts differ"""
    draws = [r['@draws'] for r in sel if r.get('@draws', 0) > 0]
    sub_cpu = [r['@sub_cpu_ms'] for r in sel if r.get('@sub_cpu_ms', -1) >= 0]
    sub_minstr = [r['@sub_instr'] / 1e6 for r in sel if r.get('@sub_instr', 0) > 0]
    us_draw = [r['@sub_cpu_ms'] * 1000.0 / r['@draws'] for r in sel if r.get('@draws', 0) > 0 and r.get('@sub_cpu_ms', -1) >= 0]
    kinstr_draw = [r['@sub_instr'] / r['@draws'] / 1e3 for r in sel if r.get('@draws', 0) > 0 and r.get('@sub_instr', 0) > 0]
    out = {}
    for k, xs, digits in (('draws_p50', draws, 0), ('sub_cpu_p50', sub_cpu, 2), ('sub_us_draw', us_draw, 2), ('sub_minstr_p50', sub_minstr, 2), ('sub_kinstr_draw', kinstr_draw, 2)):
        v = pct(xs, 50)
        if v is not None: out[k] = int(round(v)) if digits == 0 else round(v, digits)
    return out


def last_frame(tsv):
    try:
        with open(tsv, 'rb') as fh:
            fh.seek(0, 2); fh.seek(max(0, fh.tell() - 4096)); lines = fh.read().decode(errors='replace').splitlines()
    except OSError: return -1
    for ln in reversed(lines[1:] or lines):
        c = ln.split('\t')
        if len(c) == 3 and c[0].isdigit(): return int(c[0])
    return -1


def run_one(a, gid, build, var, route, out, lock):
    build, out = build.resolve(), out.expanduser().resolve()
    var = {k: str(Path(v).expanduser().resolve()) if k in ('host', 'patched', 'msl', 'shmap') else v for k, v in var.items()}
    env = dict(os.environ)
    host = Path(var.get('host') or build / 'tools/dmhost')
    patched = Path(var.get('patched') or newest(build.glob('patched_v*')) or fail(f'no rewritten modules (patched_v*) in {build}'))
    ver = patched.name.removeprefix('patched_')
    romfs = rfs.source(build / 'extract' / ver)
    if not romfs.is_dir(): romfs = rfs.source(newest(build.glob('extract/v*')))
    prof = build / 'profile' / gid if (build / 'profile' / gid / 'game.tsv').is_file() else REPO / 'dm/games' / gid
    out.mkdir(parents=True, exist_ok=True); save, temp = out / 'save', out / 'temp'
    shutil.rmtree(save, ignore_errors=True); shutil.copytree(a.save, save); temp.mkdir(exist_ok=True)
    env.update(DM_ROOT=str(build), DM_ROMFS=str(romfs), DM_SAVE=str(save), DM_TEMP=str(temp), DM_MSL_DIR=str(var.get('msl') or build / 'shader/cache'),
               DM_FONT_DIR=str(build / 'font'), DM_NVN_SHMAP=str(var.get('shmap') or prof / 'shader_map.tsv'), DM_AUDIO='0', DM_KEYS=route['keys'],
               DM_NVN_TSV=str(out / 'frames.tsv'), DM_NVN_TSV_CALLS='0', DM_CTL=str(out / 'ctl'), DM_METAL_HUD='0' if a.no_hud else '1')
    headless = getattr(a, 'headless', False)
    if headless: env.pop('DM_WINDOW', None); env['DM_METAL_HUD'] = '0'
    else: env['DM_WINDOW'] = '1'
    env.update(route.get('env') or {})   # --env and the variants override the route's own switches
    if (build / 'pso/mac').is_dir(): env.setdefault('DM_PSO_BUNDLE', str(build / 'pso/mac'))   # as the Mac app's launcher sets it; without it every base is rebuilt before the first frame
    if (build / 'aoc').is_dir(): env['DM_AOC_DIR'] = str(build / 'aoc')
    archive = var.get('pso') or a.pso_archive
    if archive: env['DM_PSO_ARCHIVE'] = archive if archive == 'off' else str(Path(archive).expanduser().resolve())
    if a.fps: env['DM_FPS_TARGET'] = str(a.fps)
    if a.fps and a.fps > 60: env.update(DM_PRESENT_HZ=str(a.fps), DM_PRESENT_LINK='2')   # at 60 or 30 the host matches the display link to the cap itself
    for kv in a.env: k, _, v = kv.partition('='); env[k] = v
    for k, v in var.items():
        if k not in ('host', 'msl', 'shmap', 'patched', 'pso'): env[k] = v
    (out / 'ctl').write_text('')
    by_frame = route.get('frames', False)
    end = route['from'] + route['secs'] + 2
    # a capture can stall a frame: the default shot is after the window
    shots = sorted(a.shots) if a.shots else [route['from'] + route['secs'] + (20 if by_frame else 0.5)]
    if by_frame:
        env['DM_NVN_SHOT'] = ','.join(str(int(f)) for f in shots) + f':{out}/shot_f%d.png'
        last_shot, end_frame, shots = int(shots[-1]), int(max(shots[-1], route['from'] + route['secs'])), []
        end = getattr(a, 'timeout', None) or 600
    p = None
    if lock: wait_lock(lock)
    try:
        with open(out / 'log.txt', 'w') as log:
            p = subprocess.Popen([str(host), str(patched)], cwd=out, env=env, stdout=log, stderr=subprocess.STDOUT)
        t0 = time.monotonic()
        while time.monotonic() - t0 < end and p.poll() is None:
            if by_frame and last_frame(out / 'frames.tsv') > end_frame and (out / f'shot_f{last_shot}.png').is_file(): break
            el = time.monotonic() - t0
            while shots and el >= shots[0]:
                with open(out / 'ctl', 'a') as c: c.write(f'shot {out}/shot_{int(shots.pop(0))}s.png\n')
            time.sleep(0.2)
    finally:
        if p is not None and p.poll() is None:
            p.send_signal(signal.SIGTERM)
            try: p.wait(5)
            except subprocess.TimeoutExpired: p.kill(); p.wait()
        if lock: lock.unlink(missing_ok=True)
    budget = 1000.0 / (a.fps or 60)
    s = {'frames': 0}
    if (out / 'frames.tsv').is_file():
        sel = window_rows(out / 'frames.tsv', route['from'], route['from'] + route['secs'], by_frame)
        secs = route['secs']
        if by_frame:
            secs = (sel[-1]['@t_s'] - sel[0]['@t_s'] + sel[-1].get('@frame_ms', 0) / 1000) if len(sel) > 1 else 0
            if len(sel) < route['secs']: print(f'  [RED] the run stopped before the window\'s end: {len(sel)} of {route["secs"]:g} frames (see {out / "log.txt"})', flush=True)
        s = summary(*frames(out / 'frames.tsv', route['from'], route['from'] + route['secs'], by_frame), secs, budget)
        s.update(means(sel)); s.update(submit_metrics(sel))
    log_text = (out / 'log.txt').read_text(errors='replace')
    m = re.findall(r'frame step: game time [\d.]+ s in [\d.]+ s \(([\d.]+) of real time\)', log_text)
    if m: s['game_speed'] = float(m[-1])
    s['red_lines'] = sum(1 for ln in log_text.splitlines() if '[RED]' in ln)
    return s


def lock_holder(lock):
    """0 for a lock naming no ID or a dead one while a game runs; None for a free or stale lock"""
    try: text = Path(lock).read_text()
    except OSError: return None
    m = re.search(r'(?:^|pid )(\d+)\s*$', text.strip())
    if not m: return 0
    pid = int(m.group(1))
    try: os.kill(pid, 0); return pid
    except PermissionError: return pid
    except ProcessLookupError: pass
    return 0 if subprocess.run(['pgrep', '-x', 'dmhost'], capture_output=True).returncode == 0 else None


def wait_lock(lock, owner=''):   # taken atomically; a stale one is removed
    while True:
        try:
            fd = os.open(lock, os.O_CREAT | os.O_EXCL | os.O_WRONLY)
            os.write(fd, (f'{owner} pid {os.getpid()}' if owner else str(os.getpid())).encode())
            os.close(fd)
            return
        except FileExistsError:
            if lock_holder(lock) is None:
                print(f'removing the stale measuring lock {lock} ({Path(lock).read_text(errors="replace").strip()[:80]}: its process is gone, no game runs)', flush=True)
                Path(lock).unlink(missing_ok=True); continue
            time.sleep(2)


def table(results, keys):
    head = ['variant', 'runs'] + keys
    rows = ['| ' + ' | '.join(head) + ' |', '|' + '---|' * len(head)]
    for label, rs in results.items():
        ok = [r for r in rs if r.get('frames')]
        def cell(k):
            values = [r[k] for r in ok if r.get(k) is not None]
            if not values: return '–'
            return f'{statistics.median(values):g}' + (f' ({min(values):g}–{max(values):g})' if len(values) > 1 else '')
        rows.append('| ' + ' | '.join([label, str(len(ok))] + [cell(k) for k in keys]) + ' |')
    return '\n'.join(rows)


def main():
    ap = argparse.ArgumentParser(description='Benchmark a game in the Mac host along a fixed route (see the comment at the top of this file).')
    ap.add_argument('build', help='the <id>_build folder of a --platform mac build')
    ap.add_argument('--route', help='a route name from dm/games/<id>/bench.tsv (default: its first)')
    ap.add_argument('--save', help='the save folder the route starts from (default <id>_build/run/save)')
    ap.add_argument('--variant', action='append', default=[], help="'label:KEY=VALUE,…' (repeatable); keys host, msl, shmap, patched, pso take paths")
    ap.add_argument('--runs', type=int, default=2)
    ap.add_argument('--fps', type=int, help='DM_FPS_TARGET for every variant (above 60 with the display link at that rate)')
    ap.add_argument('--env', action='append', default=[], help='KEY=VALUE for every variant (repeatable)')
    ap.add_argument('--pso-archive', help='DM_PSO_ARCHIVE for every variant (the pipeline archive; default the host\'s own)')
    ap.add_argument('--lock', default='', help='a lock file to hold while a game runs, shared with other measuring runs on this Mac')
    ap.add_argument('--shots', type=lambda v: [float(x.lstrip('f')) for x in v.split(',')], help='screenshot times in seconds from host start, or presented frames for a frame route, comma-separated (default: after the measurement window; in-window captures perturb timing)')
    ap.add_argument('--no-hud', action='store_true', help='no Metal performance HUD')
    ap.add_argument('--headless', action='store_true', help='no window: no display link or compositing, nothing on the screen')
    ap.add_argument('--timeout', type=float, default=600, help='seconds a run of a frame route may take before it is stopped (default 600)')
    ap.add_argument('--out', help='the output folder (default <id>_build/bench/<date-time>)')
    a = ap.parse_args()
    build = Path(a.build).expanduser().resolve()
    m = re.match(r'([0-9A-Fa-f]{16})_build$', build.name)
    if not m or not build.is_dir(): fail(f'{build} is not an <id>_build folder of the packing script')
    gid = m.group(1).upper()
    rs = routes(gid)
    if a.route: route = rs.get(a.route) or fail(f'no route {a.route!r} in dm/games/{gid}/bench.tsv (routes: {", ".join(rs) or "none"})')
    elif rs: a.route, route = next(iter(rs.items()))
    else: fail(f'dm/games/{gid}/bench.tsv has no route')
    a.save = Path(a.save).expanduser() if a.save else build / 'run/save'
    if not a.save.is_dir(): fail(f'no save folder {a.save}')
    variants = {}
    for v in a.variant or ['base:']:
        label, _, kvs = v.partition(':')
        variants[label] = dict(kv.split('=', 1) for kv in kvs.split(',') if kv)
    out = Path(a.out).expanduser().resolve() if a.out else build / 'bench' / datetime.datetime.now().strftime('%Y%m%d-%H%M%S')
    lock = Path(a.lock) if a.lock else None
    unit = 'frames' if route.get('frames') else 's'
    print(f'{gid}, route {a.route!r} ({route["about"]}): window {route["from"]:g}–{route["from"] + route["secs"]:g} {unit}; {len(variants)} variant(s) × {a.runs} run(s) → {out}', flush=True)
    results = {k: [] for k in variants}
    for i in range(a.runs):
        for label, var in variants.items():
            d = out / f'{label}-{i + 1}'
            s = run_one(a, gid, build, var, route, d, lock); results[label].append(s)
            sub_note = f', submission {s["sub_cpu_p50"]} ms ({s["sub_us_draw"]} us a draw)' if s.get('sub_us_draw') is not None else ''
            print(f'  {label} run {i + 1}: ' + (f'{s["fps"]} fps, frame p50 {s["frame_p50"]} / p99 {s["frame_p99"]} / max {s["frame_max"]} ms, GPU p50 {s["gpu_p50"]} / mean {s.get("gpu_mean")} ms, span p50 {s.get("span_p50")} ms{sub_note}' if s.get('frames') else f'no frames (see {d / "log.txt"})'), flush=True)
    keys = ['fps', 'frame_p50', 'frame_p95', 'frame_p99', 'frame_max', 'frame_mean', 'shown_p99', 'shown_max', 'dropped', 'over_budget', 'gpu_p50', 'gpu_p95', 'gpu_mean', 'span_p50',
            'sub_cpu_mean', 'passes', 'draws']
    sub_keys = ['draws_p50', 'sub_cpu_p50', 'sub_us_draw', 'sub_minstr_p50', 'sub_kinstr_draw']
    if any(r.get(k) is not None for rs_ in results.values() for r in rs_ for k in sub_keys): keys += sub_keys
    keys += ['game_speed', 'red_lines']
    md = (f'{gid}, route `{a.route}` ({route["about"]}), window {route["secs"]:g} {unit}, fps target {a.fps or "profile"}; '
          f'medians over runs (range); times in ms; over_budget = frames longer than 1.5 frame times\n\n' + table(results, keys) + '\n')
    (out / 'results.md').write_text(md)
    options = {k: v for k, v in vars(a).items() if k not in ('build', 'save')}
    (out / 'results.json').write_text(json.dumps({'game': gid, 'route': a.route, **options, 'save': str(a.save), 'variants': variants, 'results': results}, indent=1, default=str))
    print('\n' + md)
    sys.exit(0 if all(r.get('frames') for rs_ in results.values() for r in rs_) else 1)


if __name__ == '__main__':
    main()
