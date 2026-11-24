#!/usr/bin/env python3
# Game profiles: dm/games/<title ID>/game.tsv (key <TAB> values; keys: dm/games/README.md).
# Usage: game.py list | <id> <key> [<default>] | header <id> <out.h> | modules <id> <rewritten modules folder> | dir <id> | link <id> <link> (make cannot take paths with spaces)
# DM_GAMES_DIR (':'-separated) adds profile folders searched after dm/games (the packing script's derived profiles); DM_GAMES_FIRST=1 searches them first.
import os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


class Game:
    def __init__(self, d):
        self.dir = Path(d).resolve()
        self.id = self.dir.name
        self.rows = []
        for line in (self.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.lstrip().startswith('#'): continue
            c = line.split('\t')
            self.rows.append((c[0].strip(), [v.strip() for v in c[1:]]))

    def values(self, key):   # keys such as build repeat
        return [v for k, v in self.rows if k == key]

    def get(self, key, default=None):
        r = self.values(key)
        if r: return '\t'.join(r[0])
        if default is None: raise KeyError(f'{self.dir / "game.tsv"} has no key {key}')
        return default

    def words(self, key, default=None): return self.get(key, default).split()
    def int(self, key): return int(self.get(key), 0)
    def table(self, name): return self.dir / name

    @property
    def title_id(self): return int(self.get('title_id'), 16)

    def builds(self):
        """{version: {'display': display version, module name: build ID, ...}}"""
        out = {}
        for ver, disp, *mods in self.values('build'):
            out[ver] = dict([('display', disp)] + [tuple(m.split('=', 1)) for m in mods])
        return out

    def build_modules(self):
        """{main build ID: [modules]}, in the load order of modules; a row's modules that modules does not list stay unloaded"""
        order, out = self.words('modules'), {}
        for b in self.builds().values():
            if 'main' in b: out[b['main']] = [m for m in order if m in b]
        return out

    def modules_in(self, folder):   # versions may differ in modules
        folder = Path(folder)
        return [m for m in self.words('modules', 'main') if (folder / m).is_file() or (folder / f'{m}.patched.bin').is_file()]

    def pairs(self, key):
        return [tuple(w.split('=', 1)) for w in self.words(key)]


def extra_dirs():
    return [Path(p) for p in os.environ.get('DM_GAMES_DIR', '').split(':') if p]


def search_dirs():
    return extra_dirs() + [HERE] if os.environ.get('DM_GAMES_FIRST') else [HERE] + extra_dirs()


def find_dir(name):
    if os.sep in str(name): return Path(name)
    return next((d / name for d in search_dirs() if (d / name / 'game.tsv').exists()), HERE / name)


DEFAULT = '0400486024DEC000'


def load(name=None):
    name = name or os.environ.get('DM_GAME') or DEFAULT
    d = find_dir(name)
    if not (d / 'game.tsv').exists():
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] no game profile {d / "game.tsv"} (DM_GAME / GAME take the base game title ID: {have})')
    return Game(d)


def all_games(skip=()):   # skip: the packing script's own derived profiles, never taken as registered ones
    out, seen, skip = [], set(), {Path(d).resolve() for d in skip}
    for root in search_dirs():
        if not root.is_dir() or root.resolve() in skip: continue
        for d in sorted(root.iterdir()):
            if (d / 'game.tsv').exists() and d.name not in seen:
                seen.add(d.name)
                out.append(Game(d))
    return out


def by_title(tid, skip=()):
    return next((g for g in all_games(skip) if g.title_id == tid), None)


def by_build(main_id, skip=()):   # a release of the same build under another title ID
    return next((g for g in all_games(skip) if any(b.get('main') == main_id for b in g.builds().values())), None)


# NDSR pack sources (shader_split.py ndsr_shaders), under whatever name ships them; a shader whose colour the host does not know runs as the game's
UPSCALER_SOURCE = re.compile(r'#NDSR@0x[0-9a-f]+$')
# agl's variable-ratio NDSR: a chain of compute dispatches a frame, told apart by what they bind (up_chain_dispatch)
UPSCALER_CHAIN_SOURCE = re.compile(r'/agl_ndsr_vr(_light)?\.pck#')


def upscaler_keys(g, source=UPSCALER_SOURCE):
    """Sorted 16-digit keys from the shader map's source column; none without a map"""
    p = g.table('shader_map.tsv')
    if not p.exists(): return []
    keys = set()
    for line in p.read_text(encoding='utf-8').splitlines():
        c = line.split('\t')
        if len(c) >= 5 and c[2] == 'compute' and source.search(c[4]) and re.fullmatch(r'[0-9a-f]{16}', c[3]): keys.add(c[3])
    return sorted(keys)


def header(g):
    return header_one(g)


def header_one(g):
    q = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    mods = g.words('modules')
    vers = ', '.join('{' + q(b['main']) + ', ' + q(b['display']) + '}' for b in g.builds().values() if 'main' in b)
    pi = g.get('present_interval', 'game')
    if pi not in ('game', 'display'): sys.exit(f'{g.id}: present_interval must be game or display: {pi}')
    ft = g.get('fps_target', '0')
    if ft not in ('0', '30', '60'): sys.exit(f'{g.id}: fps_target must be 0, 30 or 60: {ft}')
    fm = g.get('fps_max', '60')
    if fm not in ('60', '120'): sys.exit(f'{g.id}: fps_max must be 60 or 120: {fm}')
    fh = g.get('fps_hold', 'on')
    if fh not in ('on', 'off'): sys.exit(f'{g.id}: fps_hold must be on or off: {fh}')
    ps = g.get('present_snap', 'off')
    if ps not in ('on', 'off'): sys.exit(f'{g.id}: present_snap must be on or off: {ps}')
    et = g.get('enc_thread', 'off')
    if et not in ('on', 'off'): sys.exit(f'{g.id}: enc_thread must be on or off: {et}')
    cp = g.get('core_preempt', 'off')
    if cp not in ('on', 'off'): sys.exit(f'{g.id}: core_preempt must be on or off: {cp}')
    # keys a derived profile in a build folder may still carry
    if g.get('shader_key_skip', ''): print(f'{g.id}: shader_key_skip is ignored: an NVN2 shader key never left control bytes out, and the host and the shader chain read no other', file=sys.stderr)
    if g.get('render_cond', ''): print(f'{g.id}: render_cond is ignored: the host evaluates NVN conditional rendering for every game', file=sys.stderr)
    rah = g.get('replay_ahead', 'off')
    if rah not in ('on', 'off'): sys.exit(f'{g.id}: replay_ahead must be on or off: {rah}')
    if rah == 'on' and et != 'on': sys.exit(f'{g.id}: replay_ahead needs enc_thread on (the encode thread holds the commits)')
    tfu = g.get('tex_flush_upload', 'off')
    if tfu not in ('on', 'off'): sys.exit(f'{g.id}: tex_flush_upload must be on or off: {tfu}')
    tdu = g.get('tex_dirty_upload', 'off')
    if tdu not in ('on', 'off') or (tdu == 'on' and tfu != 'on'): sys.exit(f'{g.id}: tex_dirty_upload must be on or off, and needs tex_flush_upload on: {tdu}')
    tds = g.get('tex_dyn_slots', 'off')
    if tds not in ('on', 'off'): sys.exit(f'{g.id}: tex_dyn_slots must be on or off: {tds}')
    vcr = g.get('vconv_range', 'off')
    if vcr not in ('on', 'off'): sys.exit(f'{g.id}: vconv_range must be on or off: {vcr}')
    mml = g.get('mem_map_limit', 'off')
    if mml not in ('on', 'off'): sys.exit(f'{g.id}: mem_map_limit must be on or off: {mml}')
    ush = g.get('upscaler_sharpness', '1.0')
    if not re.fullmatch(r'[01]\.\d{1,2}', ush) or float(ush) > 1: sys.exit(f'{g.id}: upscaler_sharpness must be 0.0 to 1.0: {ush}')
    ups = upscaler_keys(g)
    upc = upscaler_keys(g, UPSCALER_CHAIN_SOURCE)
    dbr = g.get('draw_barriers', 'full')
    if dbr not in ('full', 'game'): sys.exit(f'{g.id}: draw_barriers must be full or game: {dbr}')
    tsh = g.get('gpu_ts_hz', '0')
    if not tsh.isdigit(): sys.exit(f'{g.id}: gpu_ts_hz must be a rate in Hz, or 0 for the game\'s NVN: {tsh}')
    om = g.get('op_mode', 'console')
    if om not in ('console', 'handheld'): sys.exit(f'{g.id}: op_mode must be console or handheld: {om}')
    pm = g.get('perf_mode', 'follow')
    if pm not in ('follow', 'boost'): sys.exit(f'{g.id}: perf_mode must be follow or boost: {pm}')
    dmo = g.get('depth_mode', 'gl')
    if dmo not in ('gl', 'zero'): sys.exit(f'{g.id}: depth_mode must be gl or zero: {dmo}')
    src = f'dm/games/{g.id}' if g.dir.parent == HERE else str(g.dir)
    L = [f'// Generated by dm/games/game.py from {src}/game.tsv; do not edit',
         '#pragma once',
         f'#define DM_GAME_ID {q(g.id)}',
         f'#define DM_GAME_NAME {q(g.get("name"))}',
         f'#define DM_GAME_PROGRAM_ID 0x{int(g.get("program_id"), 16):016X}ull',
         f'#define DM_GAME_MODULES {{{", ".join(map(q, mods))}}}',
         f'#define DM_GAME_NMOD {len(mods)}',
         # a version may lack a module another has
         f'#define DM_GAME_BUILD_MODULES {", ".join("{" + q(bid) + ", " + q(" ".join(ms)) + "}" for bid, ms in g.build_modules().items()) or "{0}"}',
         f'#define DM_GAME_ROMFS_MARKER {q(g.get("romfs_marker"))}',
         f'#define DM_GAME_MEM_RESERVE {g.int("mem_reserve"):#x}ull',
         f'#define DM_GAME_MEM_ALIAS {g.int("mem_alias"):#x}ull',
         f'#define DM_GAME_MEM_STACK {g.int("mem_stack"):#x}ull',
         f'#define DM_GAME_APP_POOL {g.int("app_pool"):#x}ull',
         f'#define DM_GAME_MEM_USED {int(g.get("mem_used", "0x4000000"), 0):#x}ull',
         f'#define DM_GAME_SYS_RESOURCE {int(g.get("sys_resource", "0x2000000"), 0):#x}ull',   # 0 = no virtual address memory
         f'#define DM_GAME_ARGS {q(g.get("game_args", ""))}',
         f'#define DM_GAME_ENV {q(g.get("game_env", ""))}',
         f'#define DM_GAME_MEM_MAP_LIMIT {int(mml == "on")}',
         f'#define DM_GAME_DISPLAY_VERSIONS {vers}',
         f'#define DM_GAME_PRESENT_DISPLAY {int(pi == "display")}',
         f'#define DM_GAME_CORE_PREEMPT {int(cp == "on")}',
         f'#define DM_GAME_FPS_TARGET {int(ft)}',
         f'#define DM_GAME_FPS_MAX {int(fm)}',
         f'#define DM_GAME_FPS_HOLD {int(fh == "on")}',
         f'#define DM_GAME_PRESENT_SNAP {int(ps == "on")}',
         f'#define DM_GAME_ENC_THREAD {int(et == "on")}',
         f'#define DM_GAME_OPMODE {int(om == "console")}',
         f'#define DM_GAME_PERF_BOOST {int(pm == "boost")}',
         f'#define DM_GAME_TEX_FLUSH_UPLOAD {int(tfu == "on")}',
         f'#define DM_GAME_REPLAY_AHEAD {int(rah == "on")}',
         f'#define DM_GAME_TEX_DIRTY_UPLOAD {int(tdu == "on")}',
         f'#define DM_GAME_TEX_DYN_SLOTS {int(tds == "on")}',
         f'#define DM_GAME_DRAW_BARRIERS_GAME {int(dbr == "game")}',
         f'#define DM_GAME_GPU_TS_HZ {int(tsh)}ull',
         f'#define DM_GAME_UPSCALER_KEYS {q(",".join(ups))}', f'#define DM_GAME_UPSCALER_N {len(ups)}',
         f'#define DM_GAME_UPSCALER_SHARPNESS {ush}',
         f'#define DM_GAME_UPSCALER_CHAIN_KEYS {q(",".join(upc))}', f'#define DM_GAME_UPSCALER_CHAIN_N {len(upc)}',
         f'#define DM_GAME_VCONV_RANGE {int(vcr == "on")}',
         '']
    return '\n'.join(L)


if __name__ == '__main__':
    a = sys.argv[1:]
    if a == ['list']:
        for g in all_games(): print(f'{g.id}\t{g.title_id:016X}\t{g.get("name", "")}')
    elif len(a) == 3 and a[0] == 'header':
        out, text = Path(a[2]), header(load(a[1]))
        out.parent.mkdir(parents=True, exist_ok=True)
        if not (out.exists() and out.read_text() == text):   # make runs this every time, and an unchanged header must not rebuild the host
            tmp = out.with_name(out.name + '.tmp')
            tmp.write_text(text)
            tmp.replace(out)
    elif len(a) == 3 and a[0] == 'modules':
        g, bid = load(a[1]), re.search(r'^# BUILD_ID\s+(\w+)', (Path(a[2]) / 'main.fixups.tsv').read_text(errors='replace'), re.M)
        print(' '.join(g.build_modules().get(bid.group(1) if bid else '', g.words('modules'))))
    elif len(a) == 2 and a[0] == 'dir':
        print(load(a[1]).dir)
    elif len(a) == 3 and a[0] == 'link':
        d, ln = load(a[1]).dir, Path(a[2])
        ln.parent.mkdir(parents=True, exist_ok=True)
        if not (ln.is_symlink() and Path(os.readlink(ln)) == d):
            if ln.is_symlink() or ln.exists(): ln.unlink()
            ln.symlink_to(d)
    elif len(a) in (2, 3):
        print(load(a[0]).get(*a[1:]))
    else:
        sys.exit(__doc__ or 'usage: game.py list | game.py <id> <key> [<default>] | game.py header <id> <out.h> | game.py modules <id> <rewritten modules> | game.py dir <id> | game.py link <id> <link>')
