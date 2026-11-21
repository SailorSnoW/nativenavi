#!/usr/bin/env python3
# Game profiles: dm/games/<base game title ID>/game.tsv (key <TAB> values..., lines starting with # are comments) + the shader tables (and patches.py, the rewriter's patches of the game's builds) in the same folder. The shader tools, the host build and the packing script take the game's constants from here.
# Usage: game.py list (list all profiles); game.py <id> <key> [<default>] (print that key's value, for shell; without a default a missing key is an error); game.py header <id> <out.h> (generate the host's game_profile.h);
#   game.py modules <id> <folder of rewritten modules> (the modules to load from it: the build row of its main's build, else the profile's modules);
#   game.py dir <id> (print the profile folder); game.py link <id> <link> (point a symlink at the profile folder: make cannot take paths with spaces)
#   From Python: import game; g = game.load() (takes DM_GAME, or Ocarina of Time, game.DEFAULT, when it is unset); game.by_title(title ID); g.get / g.words / g.int / g.builds() / g.table(file name)
# DM_GAMES_DIR (folders separated by ':') adds profile folders outside the repository, searched after dm/games: the packing script writes a derived profile for a version the profile does not register into
#   <build folder>/profile/<id>/ and points DM_GAMES_DIR at <build folder>/profile, so the shader tools and the host build find it.
#   DM_GAMES_FIRST=1 searches DM_GAMES_DIR before dm/games (a derived profile takes the place of the one in dm/games)
import os, re, sys
from pathlib import Path

HERE = Path(__file__).resolve().parent


class Game:
    def __init__(self, d):
        self.dir = Path(d).resolve(); self.id = self.dir.name; self.rows = []
        for line in (self.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
            if not line.strip() or line.lstrip().startswith('#'): continue
            c = line.split('\t'); self.rows.append((c[0].strip(), [v.strip() for v in c[1:]]))

    def values(self, key):   # every row of this key (keys such as build repeat)
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
        """{main build ID: [modules]} for each build row with a main: the modules that build has, in the load order of modules (Mario Kart World: the base game's main subsdk0 sdk,
        1.8.0's main subsdk0 subsdk1 sdk). A row may name modules the profile does not load (Hades' subsdk0 and subsdk1): they stay unloaded"""
        order, out = self.words('modules'), {}
        for b in self.builds().values():
            if 'main' in b: out[b['main']] = [m for m in order if m in b]
        return out

    def modules_in(self, folder):   # the modules of the profile a version's ExeFS (or its rewritten modules' folder) holds, in load order: a game's versions may differ in modules
        folder = Path(folder)
        return [m for m in self.words('modules', 'main') if (folder / m).is_file() or (folder / f'{m}.patched.bin').is_file()]

    def pairs(self, key):   # "a=b c=d" → [(a, b), (c, d)]
        return [tuple(w.split('=', 1)) for w in self.words(key)]


def extra_dirs():   # profile folders outside the repository (DM_GAMES_DIR), searched after dm/games
    return [Path(p) for p in os.environ.get('DM_GAMES_DIR', '').split(':') if p]


def search_dirs():   # dm/games, then DM_GAMES_DIR; DM_GAMES_FIRST=1 the other way round
    return extra_dirs() + [HERE] if os.environ.get('DM_GAMES_FIRST') else [HERE] + extra_dirs()


def find_dir(name):   # the profile folder of a title ID (the first of search_dirs that has it); a path is taken as is
    if os.sep in str(name): return Path(name)
    return next((d / name for d in search_dirs() if (d / name / 'game.tsv').exists()), HERE / name)


DEFAULT = '0400486024DEC000'   # The Legend of Zelda: Ocarina of Time, the one game of this repository: what load() takes when neither a name nor DM_GAME is given


def load(name=None):
    name = name or os.environ.get('DM_GAME') or DEFAULT
    d = find_dir(name)
    if not (d / 'game.tsv').exists():   # profile folders are named after the base game's title ID (the old names smbw / fe_engage are gone)
        have = ', '.join(f'{g.id} ({g.get("name", "")})' for g in all_games())
        raise SystemExit(f'[RED] no game profile {d / "game.tsv"} (DM_GAME / GAME take the base game title ID: {have})')
    return Game(d)


def all_games(skip=()):   # skip: profile folders left out (the packing script's own derived profiles, which it never takes as registered ones)
    out, seen, skip = [], set(), {Path(d).resolve() for d in skip}
    for root in search_dirs():
        if not root.is_dir() or root.resolve() in skip: continue
        for d in sorted(root.iterdir()):
            if (d / 'game.tsv').exists() and d.name not in seen: seen.add(d.name); out.append(Game(d))
    return out


def by_title(tid, skip=()):
    return next((g for g in all_games(skip) if g.title_id == tid), None)


def by_build(main_id, skip=()):   # the profile whose build rows register this main build ID (a release of the same build under another title ID)
    return next((g for g in all_games(skip) if any(b.get('main') == main_id for b in g.builds().values())), None)


# The upscaler network (NDSR), by where the shader map found each key: the compute shaders of an NDSR pack (shader_split.py ndsr_shaders: a source ending in #NDSR@<offset>), whatever
# ships it. The pack is a library several engines carry under their own names: agl's agl_ndsr.pck (Donkey Kong Bananza, Splatoon Raiders, Mario Kart World, Nintendo Switch Sports Resort)
# and its siblings agl_ndsr_vr.pck and agl_ndsr_vr_light.pck (Ocarina of Time), Kirby Air Riders' and Fast Fusion's .ndsr files (the same 20 networks), Star Fox's nested in a VIPR container
# (Splatoon Raiders' 20), and the pack inside nn::ndve's movie file. Being named only lets the host read a shader's colour from its code; one whose colour it does not know, or a dispatch
# it cannot draw, runs as the game's (nvn_mtl.m up_mfx_color, up_mfx_encode)
UPSCALER_SOURCE = re.compile(r'#NDSR@0x[0-9a-f]+$')
# agl's variable-ratio NDSR, a chain of compute dispatches a frame (a tile list, edges, a pack, then the network its ratio picks) instead of one: the compute shaders of agl_ndsr_vr.pck
# and agl_ndsr_vr_light.pck (Ocarina of Time on the Switch 2: 37). The host tells the chain's dispatches apart by what they bind (nvn_mtl.m up_chain_dispatch)
UPSCALER_CHAIN_SOURCE = re.compile(r'/agl_ndsr_vr(_light)?\.pck#')


def upscaler_keys(g, source=UPSCALER_SOURCE):
    """the game's own upscaler shaders, from its shader map (shader_split.py's source column): sorted 16-digit keys, none without a map; source: the pack they come from"""
    p = g.table('shader_map.tsv')
    if not p.exists(): return []
    keys = set()
    for line in p.read_text(encoding='utf-8').splitlines():
        c = line.split('\t')
        if len(c) >= 5 and c[2] == 'compute' and source.search(c[4]) and re.fullmatch(r'[0-9a-f]{16}', c[3]): keys.add(c[3])
    return sorted(keys)


def header(g):
    """game_profile.h of a profile: its constants"""
    return header_one(g)


def header_one(g):
    """the host's game_profile.h: the profile constants the host uses, as macros"""
    q = lambda s: '"' + s.replace('\\', '\\\\').replace('"', '\\"') + '"'
    mods = g.words('modules')
    vers = ', '.join('{' + q(b['main']) + ', ' + q(b['display']) + '}' for b in g.builds().values() if 'main' in b)
    pi = g.get('present_interval', 'game')   # present interval: game = as the game sets it (as on the Switch), display = one frame per display tick
    if pi not in ('game', 'display'): sys.exit(f'{g.id}: present_interval must be game or display: {pi}')
    ft = g.get('fps_target', '0')   # frame rate cap of the rewriter's frame step (FRAME_STEP or MATCH_FRAME_STEP in the profile's patches.py): 0 = the game's own step, 30 / 60
    if ft not in ('0', '30', '60'): sys.exit(f'{g.id}: fps_target must be 0, 30 or 60: {ft}')
    fm = g.get('fps_max', '60')   # the highest rate fps_target and the player's Frame rate may take (hle_nvn.c fps_high): 120 needs an engine frame limit the host writes, of which this tree has none
    if fm not in ('60', '120'): sys.exit(f'{g.id}: fps_max must be 60 or 120: {fm}')
    fh = g.get('fps_hold', 'on')   # whether the frame step may hold a frame longer than the cap's vsyncs when frames come late (hle_nvn.c fstep_tick): on / off
    if fh not in ('on', 'off'): sys.exit(f'{g.id}: fps_hold must be on or off: {fh}')
    ps = g.get('present_snap', 'off')   # present snapshots by default (nvn_mtl.m mtl_snap_on; DM_PRESENT_SNAP and the launcher's Frame buffering override it): on / off
    if ps not in ('on', 'off'): sys.exit(f'{g.id}: present_snap must be on or off: {ps}')
    et = g.get('enc_thread', 'off')   # Metal encoding on its own thread by default (nvn_rec.h rec_on; DM_NVN_ENC_THREAD overrides): on / off
    if et not in ('on', 'off'): sys.exit(f'{g.id}: enc_thread must be on or off: {et}')
    cp = g.get('core_preempt', 'off')   # same-core preemption approximation (hle_thread.c): on / off
    if cp not in ('on', 'off'): sys.exit(f'{g.id}: core_preempt must be on or off: {cp}')
    if g.get('shader_key_skip', ''): print(f'{g.id}: shader_key_skip is ignored: an NVN2 shader key never left control bytes out, and the host and the shader chain read no other', file=sys.stderr)   # a profile written before the key went (a build folder's derived one)
    if g.get('render_cond', ''): print(f'{g.id}: render_cond is ignored: the host evaluates NVN conditional rendering for every game', file=sys.stderr)   # a profile written before the key went (a build folder's derived one)
    rah = g.get('replay_ahead', 'off')   # a queue parked on an event the game's CPU signals is translated meanwhile, its commits held until the signal (nvn_queue.inc nq_worker, nvn_rec.h hold_*; DM_NVN_REPLAY_AHEAD overrides; needs enc_thread): on / off
    if rah not in ('on', 'off'): sys.exit(f'{g.id}: replay_ahead must be on or off: {rah}')
    if rah == 'on' and et != 'on': sys.exit(f'{g.id}: replay_ahead needs enc_thread on (the encode thread holds the commits)')
    tfu = g.get('tex_flush_upload', 'off')   # textures written by the CPU into a pool and published with nvnMemoryPoolFlushMappedRange are uploaded like packaged ones (hle_nvn.c): on / off
    if tfu not in ('on', 'off'): sys.exit(f'{g.id}: tex_flush_upload must be on or off: {tfu}')
    tdu = g.get('tex_dirty_upload', 'off')   # with tex_flush_upload: a registered texture the CPU writes after init without any flush is uploaded too (dirty pages, hle_nvn.c nvn_ft_written): on / off
    if tdu not in ('on', 'off') or (tdu == 'on' and tfu != 'on'): sys.exit(f'{g.id}: tex_dirty_upload must be on or off, and needs tex_flush_upload on: {tdu}')
    tds = g.get('tex_dyn_slots', 'off')   # shaders whose texture handle the translator could not trace to one slot: slot indices in c2, the bound slots in the fallback arrays (nvn_mtl.m): on / off
    if tds not in ('on', 'off'): sys.exit(f'{g.id}: tex_dyn_slots must be on or off: {tds}')
    vcr = g.get('vconv_range', 'off')   # vertex attributes converted on the CPU only over the vertices each draw reads (nvn_mtl.m vc_draw_range): on / off
    if vcr not in ('on', 'off'): sys.exit(f'{g.id}: vconv_range must be on or off: {vcr}')
    mml = g.get('mem_map_limit', 'off')   # MapPhysicalMemory refused beyond the application pool, as on Horizon (hle_svc.c): on / off
    if mml not in ('on', 'off'): sys.exit(f'{g.id}: mem_map_limit must be on or off: {mml}')
    ush = g.get('upscaler_sharpness', '1.0')   # DM_UPSCALER=metalfx: the share of MetalFX's own picture, the rest a bilinear one (nvn_mtl.m up_mfx_sharpness; DM_UPSCALER_SHARPNESS overrides): 0.0-1.0
    if not re.fullmatch(r'[01]\.\d{1,2}', ush) or float(ush) > 1: sys.exit(f'{g.id}: upscaler_sharpness must be 0.0 to 1.0: {ush}')
    ups = upscaler_keys(g)   # the game's own upscaler shaders, which DM_UPSCALER=metalfx (the launcher's 3D upscaler) replaces with MetalFX's spatial scaler (nvn_mtl.m up_mfx_encode)
    upc = upscaler_keys(g, UPSCALER_CHAIN_SOURCE)   # the shaders of its upscaler chain, which the same choice replaces as a whole, before the single dispatch (nvn_mtl.m up_chain_dispatch)
    dbr = g.get('draw_barriers', 'full')   # how draw passes are ordered (nvn_mtl.m barrier_game): full = each waits for all earlier GPU work, game = by the game's own barriers, so vertex work overlaps the previous pass's fragments
    if dbr not in ('full', 'game'): sys.exit(f'{g.id}: draw_barriers must be full or game: {dbr}')
    tsh = g.get('gpu_ts_hz', '0')   # the counter timestamps' tick rate (nvn_timestamp.h): 0 = NVN2's nanoseconds, 31250000 for a Switch 2 game that reads raw reports at its system counter rate
    if not tsh.isdigit(): sys.exit(f'{g.id}: gpu_ts_hz must be a rate in Hz, or 0 for the game\'s NVN: {tsh}')
    om = g.get('op_mode', 'console')   # the operation mode reported at start (hle_oe.c dm_opmode; DM_OPMODE overrides): console = docked, handheld
    if om not in ('console', 'handheld'): sys.exit(f'{g.id}: op_mode must be console or handheld: {om}')
    pm = g.get('perf_mode', 'follow')   # the performance mode reported (hle_oe.c): follow = the operation mode's (Boost docked, Normal handheld), boost = Boost always
    if pm not in ('follow', 'boost'): sys.exit(f'{g.id}: perf_mode must be follow or boost: {pm}')
    dmo = g.get('depth_mode', 'gl')   # the NVN depth mode the game sets, for the pipeline archive made offline (dm/gfx/tools/pso_offline.py; the host follows the game at run time): gl / zero
    if dmo not in ('gl', 'zero'): sys.exit(f'{g.id}: depth_mode must be gl or zero: {dmo}')
    src = f'dm/games/{g.id}' if g.dir.parent == HERE else str(g.dir)
    L = [f'// Generated by dm/games/game.py from {src}/game.tsv; do not edit',
         '#pragma once',
         f'#define DM_GAME_ID {q(g.id)}',
         f'#define DM_GAME_NAME {q(g.get("name"))}',
         f'#define DM_GAME_PROGRAM_ID 0x{int(g.get("program_id"), 16):016X}ull',
         f'#define DM_GAME_MODULES {{{", ".join(map(q, mods))}}}',
         f'#define DM_GAME_NMOD {len(mods)}',
         # each build's own modules (main.c picks the row of main's build ID, else DM_GAME_MODULES): a version may lack a module another has
         f'#define DM_GAME_BUILD_MODULES {", ".join("{" + q(bid) + ", " + q(" ".join(ms)) + "}" for bid, ms in g.build_modules().items()) or "{0}"}',
         f'#define DM_GAME_ROMFS_MARKER {q(g.get("romfs_marker"))}',
         f'#define DM_GAME_MEM_RESERVE {g.int("mem_reserve"):#x}ull',
         f'#define DM_GAME_MEM_ALIAS {g.int("mem_alias"):#x}ull',
         f'#define DM_GAME_MEM_STACK {g.int("mem_stack"):#x}ull',
         f'#define DM_GAME_APP_POOL {g.int("app_pool"):#x}ull',
         f'#define DM_GAME_MEM_USED {int(g.get("mem_used", "0x4000000"), 0):#x}ull',   # used memory reported beside the heap (hle_svc.c GetInfo UsedMemorySize)
         f'#define DM_GAME_SYS_RESOURCE {int(g.get("sys_resource", "0x2000000"), 0):#x}ull',   # the NPDM's system resource size (hle_svc.c GetInfo TotalSystemResourceSize; 0 = no virtual address memory)
         f'#define DM_GAME_ARGS {q(g.get("game_args", ""))}',   # game arguments when the command line gives none (main.c; made for Octopath Traveler 0: Unreal's -SwitchVirtualAddressSpaceSize=)
         f'#define DM_GAME_ENV {q(g.get("game_env", ""))}',   # KEY=VALUE pairs getenv answers (hle_os.c; made for Absolum: .NET's DOTNET_GCRegionRange)
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
        out, text = Path(a[2]), header(load(a[1])); out.parent.mkdir(parents=True, exist_ok=True)
        if not (out.exists() and out.read_text() == text):   # written only when it changes: make runs this every time (the profile folder can change under the same GAME), and an unchanged header must not rebuild the host
            tmp = out.with_name(out.name + '.tmp'); tmp.write_text(text); tmp.replace(out)
    elif len(a) == 3 and a[0] == 'modules':   # the modules to load from a folder of rewritten modules: its main build's row, else all the profile's (main.c profile_modules)
        g, bid = load(a[1]), re.search(r'^# BUILD_ID\s+(\w+)', (Path(a[2]) / 'main.fixups.tsv').read_text(errors='replace'), re.M)
        print(' '.join(g.build_modules().get(bid.group(1) if bid else '', g.words('modules'))))
    elif len(a) == 2 and a[0] == 'dir':
        print(load(a[1]).dir)
    elif len(a) == 3 and a[0] == 'link':   # link → the profile folder (replaced when it points elsewhere)
        d, ln = load(a[1]).dir, Path(a[2]); ln.parent.mkdir(parents=True, exist_ok=True)
        if not (ln.is_symlink() and Path(os.readlink(ln)) == d):
            if ln.is_symlink() or ln.exists(): ln.unlink()
            ln.symlink_to(d)
    elif len(a) in (2, 3):
        print(load(a[0]).get(*a[1:]))
    else:
        sys.exit(__doc__ or 'usage: game.py list | game.py <id> <key> [<default>] | game.py header <id> <out.h> | game.py modules <id> <rewritten modules> | game.py dir <id> | game.py link <id> <link>')
