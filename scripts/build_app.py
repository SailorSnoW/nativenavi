#!/usr/bin/env python3
# Packs Ocarina of Time from its NUP into a Mac app (--platform mac-app) or a development build (--platform mac): docs/development.md, "The packing script in detail".
# python3 scripts/build_app.py <NUP file or its folder> [options], or F5 in Spyder with the values under "Settings". Exit codes: 0, 1 error, 130 interrupted, 143 SIGTERM, 129 SIGHUP.
# Needs Apple silicon, Xcode 27.2 with the Metal toolchain dmsh_chain.lock names, and Python 3.12+ with .github/requirements/tools.txt.
import ast, collections, contextlib, errno, hashlib, json, os, re, shlex, shutil, signal, subprocess, sys, tempfile, threading, time, types
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dm/tools"))
import romfs as rfs
import proc_group
import nso
import nso_syms
import tree_tar
import shader_lib
import shader_cache
import vol_space
import pso_carry
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack_nup

# Settings
NUP = ''                                # the game's .nup file or its folder (empty = ask)
PACKAGE = ''                            # version: 'v131072', 'v0' for the base game (empty = the newest)
WORK_DIR = ''                           # build folder (empty = <title ID>_build beside the NUP)
OUT_APP = ''                            # mac-app: the app, or a folder for <game name>.app (empty = beside the build folder)
APP_NAME = ''                           # app name (empty = the NUP's game name, else the profile's)
BUNDLE_ID = ''                          # bundle ID (empty = org.nativenavi.<title ID>)
ICON = ''                               # app icon, JPEG or PNG (empty = the NUP's)
FALLBACK_ICON = ''                      # icon when ICON is empty and the NUP has none (empty = blank)
PLATFORM = 'mac-app'                    # 'mac-app' = a Mac app; 'mac' = development build run from the build folder
MAC_SIGN = ''                           # mac-app: codesign identity, e.g. 'Developer ID Application: <name> (<team>)' (empty = ad-hoc)
LANGUAGE = ''                           # game language, a code or a name: 'ja', 'fr', 'zh-Hant', 'Japanese' (empty = English)
DM_ENV_FILE = ''                        # environment switches for a diagnostic build, KEY=VALUE lines (empty = none)
SEEN_SHADERS = False                    # True = only shader_seen.tsv's shaders and those the last runs missed (others draw incomplete)
PREBAKE = True                          # mac-app: True = build the prewarm list's pipelines here and bundle them; False = the app's first start does
PSO_GPUS = ''                           # mac-app: GPU architectures of the pipeline archive ('g14s,...'); empty = this Mac's, 'none' = no archive
MOTION = True                           # False = DM_MOTION=0 (no gyro)
PRACTICE = True                         # True = practice tools in the build (off until DM_PRACTICE=1)
PROFILE_OUT = ''                        # derived profile folder (empty = <build folder>/profile)
ALLOW_DAMAGED = False                   # True = pack files whose bytes are not what their names promise
ALLOW_SHADER_REGRESSIONS = False        # True = pack without shaders that translated in the previous build and fail now
ALLOW_OTHER_METAL = False               # True = allow a Metal compiler other than dmsh_chain.lock's (pictures may be wrong)

PROGRESS = 'auto'                       # 'bar', 'lines', 'off' or 'json'; 'auto' = bar in a terminal or Spyder, lines into a file
BEAT = 30                               # seconds between lines in 'lines' mode
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin', os.path.expanduser('~/.cargo/bin')]   # searched after PATH: Spyder does not get the shell's PATH


class BuildError(Exception):
    pass


def clock():
    return time.strftime('%H:%M:%S')


def say(msg):
    PROG.say(f'[{clock()}] {msg}')


def hms(s):
    s = int(s); h, m = divmod(s // 60, 60)
    return f'{h}:{m:02d}:{s % 60:02d}' if h else f'{m}:{s % 60:02d}'


def size_unit(v):
    return next(((u, d) for u, d in (('GB', 2**30), ('MB', 2**20)) if v >= d), ('KB', 2**10))


def amount(v, unit=''):
    if unit == 'B':
        u, d = size_unit(v); return f'{v / d:.1f} {u}'
    return f'{v:,}' + (f' {unit}' if unit else '')


class Progress:
    """Modes: bar, lines (every BEAT s), off, json (counts at most every JSON_INTERVAL s)."""
    SPIN, BLOCKS, POLL, JSON_INTERVAL = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏', ' ▏▎▍▌▋▊▉', 1.0, 0.5

    def __init__(self, mode='off'):
        self.mode, self.lock = mode, threading.RLock()
        self.k, self.n = 0, 0
        self.cur = None
        self.shown = 0
        self.ticker, self.tstop = None, None

    def plan(self, n):
        with self.lock: self.k, self.n = 0, n

    def add(self, n):
        with self.lock: self.n += n

    def stage(self, name, poll=None, step='', **kw):
        with self.lock:
            self.end()
            self.k += 1; now = time.time()
            self.cur = types.SimpleNamespace(name=name, step=step, unit='', total=None, done=None, est=False, note='', t0=now, hist=collections.deque(), beat=now, pstop=None, jlast=None, jstate=None)
            if self.mode == 'json':
                self._json({'ev': 'stage', 'k': self.k, 'n': self.n, 'name': name, 'step': step, 't': round(now, 3)})
            elif self.mode != 'bar': self._out(f'[{clock()}] [{self.k}/{self.n}] {name}' + (f' · {step}' if step else '') + ' …')
            self.set(**kw)
            if self.mode != 'off' and not self.ticker:
                self.tstop = stop = threading.Event()
                self.ticker = threading.Thread(target=self._tick, args=(stop,), daemon=True); self.ticker.start()
        if poll: self.watch(poll)

    def watch(self, poll):
        with self.lock:
            st = self.cur
            if not st: return
            if st.pstop: st.pstop.set()
            st.pstop = stop = threading.Event()
        if poll: threading.Thread(target=self._poll, args=(st, poll, stop), daemon=True).start()

    def set(self, done=None, total=None, step=None, unit=None, est=None, note=None):
        """unit 'B' = bytes; est: an estimated total"""
        with self.lock:
            st = self.cur
            if not st: return
            if step is not None and step != st.step:
                st.step, st.done, st.total, st.unit, st.est, st.note, st.beat = step, None, None, '', False, '', time.time(); st.hist.clear()
                if self.mode == 'lines': self._out(f'[{clock()}] [{self.k}/{self.n}] {st.name} · {step} …')
                if self.mode == 'json': self._json({'ev': 'step', 'k': self.k, 'name': st.name, 'step': step})
            if unit is not None: st.unit = unit
            if total is not None: st.total = total
            if est is not None: st.est = est
            if note is not None: st.note = note
            if done is not None:
                st.done = done; now = time.time(); st.hist.append((now, done))
                while len(st.hist) > 2 and st.hist[1][0] < now - 60: st.hist.popleft()   # the ETA follows the last minute's rate
            if self.mode == 'json': self._json_progress(st)

    def _json_progress(self, st, force=False):   # the caller holds the lock
        state = (st.step, st.total, st.unit, st.est, st.note, st.done)
        if state == st.jstate: return
        now = time.monotonic()
        if not force and st.jstate is not None and state[:-1] == st.jstate[:-1] and now - st.jlast < self.JSON_INTERVAL: return
        st.jlast, st.jstate = now, state
        self._json({'ev': 'progress', 'k': self.k, 'name': st.name, 'step': st.step, 'done': st.done, 'total': st.total,
                    'unit': st.unit, 'est': st.est, 'note': st.note,
                    'frac': min(st.done / st.total, 0.99) if st.total and st.done is not None else None,
                    'eta': self._eta(st), 'elapsed': round(time.time() - st.t0, 3)})

    def _json(self, obj):
        obj.setdefault('t', round(time.time(), 3))
        print(json.dumps(obj, ensure_ascii=False, allow_nan=False), flush=True)

    def end(self, summary='', ok=True, warn=False):
        """warn: finished without what it makes"""
        with self.lock:
            st, self.cur = self.cur, None
            if not st: return
            if st.pstop: st.pstop.set()
            if self.mode == 'json':
                self._json_progress(st, force=True)
                self._json({'ev': 'stage_end', 'k': self.k, 'name': st.name, 'ok': ok, 'summary': summary, 'elapsed': round(time.time() - st.t0, 3), **({'warn': True} if warn else {})})
                return
            self._out(f'[{clock()}] {"⚠" if warn else "✓" if ok else "✗"} [{self.k}/{self.n}] {st.name} ({hms(time.time() - st.t0)})' + (f': {summary}' if summary else ''))

    def event(self, ev, **fields):
        with self.lock:
            if self.mode == 'json': self._json({'ev': ev, **fields})

    def say(self, msg):
        with self.lock:
            if self.mode == 'json': self._json({'ev': 'say', 'msg': msg})
            else: self._out(msg)

    def close(self, ok=True):   # the mode stays: a later message is still JSON in json mode
        with self.lock:
            self.end(ok=ok)
            if self.tstop: self.tstop.set()
            self.ticker = self.tstop = None

    def _count(self, st):
        if st.done is None: return ''
        if not st.total: return amount(st.done, st.unit)
        t = '~' if st.est else ''
        if st.unit == 'B':
            u, d = size_unit(st.total); return f'{st.done / d:.1f}/{t}{st.total / d:.1f} {u}'
        return f'{st.done:,}/{t}{st.total:,}' + (f' {st.unit}' if st.unit else '')

    def _eta(self, st):
        if not st.total or len(st.hist) < 2: return None
        (t1, d1), (t2, d2) = st.hist[0], st.hist[-1]
        if t2 - t1 < 3 or d2 <= d1: return None
        return max(st.total - st.done, 0) * (t2 - t1) / (d2 - d1)

    def _fields(self, st):
        head = f'[{self.k}/{self.n}] {st.name}' + (f' · {st.step}' if st.step else '')
        frac = min(st.done / st.total, 0.99) if st.total and st.done is not None else None
        eta = self._eta(st)
        return head, frac, self._count(st), st.note, hms(time.time() - st.t0) + (f' · ETA {hms(eta)}' if eta is not None else '')

    def _line(self, st, width):
        head, frac, count, note, times = self._fields(st)
        head = f'{self.SPIN[int(time.time() * 8) % len(self.SPIN)]} {head}'
        pct = f'{frac * 100:.0f}%' if frac is not None else ''
        for shown in (note, ''):
            rest = '  '.join(x for x in (' · '.join(x for x in (count, shown) if x), times) if x)
            barw = min(20, width - len(f'{head}  {pct:>4}  {rest}') - 3)
            if frac is None or barw >= 8: break
        if frac is not None and barw >= 8:
            cells = frac * barw; full = int(cells); part = int((cells - full) * 8)
            pct = ('█' * full + (self.BLOCKS[part] if part else '')).ljust(barw, '░') + f' {pct:>4}'
        return '  '.join(x for x in (head, pct, rest) if x)[:width - 1]

    def _out(self, text):   # the caller holds the lock
        if self.shown: sys.stdout.write('\r' + ' ' * self.shown + '\r'); self.shown = 0
        print(text, flush=True)
        if self.mode == 'bar' and self.cur: self._draw()

    def _draw(self):
        line = self._line(self.cur, shutil.get_terminal_size((80, 24)).columns)   # Spyder reports no size, and a wrapped line cannot be redrawn
        sys.stdout.write('\r' + line + ' ' * max(self.shown - len(line), 0)); sys.stdout.flush()
        self.shown = len(line)

    def _tick(self, stop):
        while not stop.wait(0.25 if self.mode == 'bar' and not in_ipython() else 1):
            with self.lock:
                if stop.is_set(): return   # close() stopped this ticker while it waited for the lock
                st = self.cur
                if not st: continue
                try:
                    if self.mode == 'bar': self._draw()
                    elif self.mode == 'json': self._json_progress(st)
                    elif self.mode == 'lines' and time.time() - st.beat >= BEAT:
                        st.beat = time.time(); head, frac, count, note, times = self._fields(st)
                        self._out(f'[{clock()}] {head}: ' + ', '.join(x for x in (f'{frac * 100:.0f}%' if frac is not None else '', count, note, times) if x))
                except Exception: pass   # progress never stops the build

    def _poll(self, st, poll, stop):
        while not stop.is_set():
            t, step = time.time(), st.step
            try: r = poll()
            except Exception: r = None
            with self.lock:   # a count read for a step that ended meanwhile is dropped
                if r and self.cur is st and not stop.is_set() and ('step' in r or st.step == step): self.set(**r)
            stop.wait(max(self.POLL, (time.time() - t) * 5))


PROG = Progress()


def progress_mode(want):   # IPython handles \r
    if want in ('bar', 'lines', 'off', 'json'): return want
    if want != 'auto': raise BuildError(f'PROGRESS / --progress must be auto, bar, lines, json or off: {want}')
    return 'bar' if in_ipython() or (sys.stdout and sys.stdout.isatty()) else 'lines'


def tree_size(d):
    n = b = 0; todo = [str(d)]
    while todo:
        try: it = rfs.os.scandir(todo.pop())
        except OSError: continue
        with it:
            for e in it:
                try:
                    if e.is_dir(follow_symlinks=False): todo.append(e.path)
                    else: n += 1; b += e.stat(follow_symlinks=False).st_size
                except OSError: pass
    return n, b


def in_ipython():   # Spyder runs this file inside IPython, where sys.exit must not be called
    import builtins
    return hasattr(builtins, '__IPYTHON__')


LANG_NEAR = {'en-US': 'en-GB', 'en-GB': 'en-US', 'fr': 'fr-CA', 'fr-CA': 'fr', 'es': 'es-419', 'es-419': 'es', 'pt': 'pt-BR', 'pt-BR': 'pt'}


def language_code(s):
    k = s.strip().lower().replace('_', '-')
    for c, n in nacp.LANGUAGES:
        if k in (c.lower(), n.lower()): return c
    return {'en': 'en-US', 'english': 'en-US', 'zh-cn': 'zh-Hans', 'zh-tw': 'zh-Hant'}.get(k)


def game_language(want, asked, have):
    """A language the game lacks stops the build when it was asked for; the default falls back to the game's first"""
    if not have or want in have: return want
    if LANG_NEAR.get(want) in have: return LANG_NEAR[want]
    if asked: raise BuildError(f'the game has no {want} ({dict(nacp.LANGUAGES)[want]}); its languages: {language_list(have)} (--language / LANGUAGE)')
    return have[0]


def languages_control(*dirs):
    """An update may carry no control data"""
    paths = [d / 'control.nacp' for d in dirs]
    return next((p for p in paths if nacp.languages(p)), paths[0])


def language_list(codes):
    names = dict(nacp.LANGUAGES)
    return ', '.join(f'{c} ({names[c]})' for c in codes)


def ask(prompt, what):
    if PROG.mode == 'json': raise BuildError(f'{what} not given: JSON mode does not prompt; give it on the command line (-h for usage)')
    if not in_ipython() and not (sys.stdin and sys.stdin.isatty()):
        raise BuildError(f'{what} not given and there is no keyboard input to ask for it: give it on the command line (-h for usage)')
    s = input(prompt).strip().strip('"').strip("'").replace('\\ ', ' ')   # dragged-in paths come quoted or with escaped spaces
    return os.path.expanduser(s)


def run(cmd, log, env=None, cwd=None, on_line=None, ok=(), fatal=True):
    """A group process still running proc_group.DRAIN s after the command exits is stopped, and the step fails."""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    tail = collections.deque(maxlen=40)
    with open(log, 'w') as f, proc_group.Group(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as g:
        for line in g.lines():
            f.write(line); tail.append(line)
            if on_line:
                try: on_line(line.rstrip('\n'))
                except Exception: pass
        rc = g.finish()
    bad = rc and rc not in ok
    if bad or g.stragglers:
        tool = Path(cmd[1] if cmd[0] in (sys.executable, 'bash') else cmd[0]).name
        why = f'{tool} exit code {rc}' if bad else f'{tool} exited, but {proc_group.describe(g.stragglers)} still ran {g.drain:g} s later and was stopped: what it writes may be incomplete'
        if fatal: PROG.end(ok=False)
        PROG.say(f'---- last lines of {log} ----\n' + ''.join(tail).rstrip('\n'))
        if not fatal:
            if not bad: PROG.say(f'[RED] {why}')
            return rc if bad else 1
        raise BuildError(f'{why} (full output: {log})')
    return 0


def build_id(path):
    with open(path, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def _gone(func, path, exc):
    if not (isinstance(exc, OSError) and exc.errno == errno.ENOENT): raise exc


OBSOLETE = 'obsolete'   # what a build or run no longer uses, in one folder to delete
STAMP = re.compile(r'\d{4}-\d{2}-\d{2} \d{6}')
BUILD_STARTED = None
BUILD_FOLDERS = set()


def obsolete_stamp(): return time.strftime('%Y-%m-%d %H%M%S')


def retire(work, p):
    """Deleted on another volume, where a move would copy"""
    p = Path(p); rel = p.relative_to(work) if p.is_relative_to(work) else Path(p.name)
    dest = work / OBSOLETE / f'{BUILD_STARTED or obsolete_stamp()} build' / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() or dest.is_symlink(): rmtree(dest)
    try: os.rename(p, dest)
    except OSError as e:
        if e.errno != errno.EXDEV: raise
        rmtree(p)


def empty_obsolete(work):
    root = work / OBSOLETE
    if not root.is_dir(): return
    for e in list(root.iterdir()):
        m = STAMP.match(e.name)
        if m and BUILD_STARTED and m.group(0) >= BUILD_STARTED: continue
        try: rmtree(e)
        except OSError as x: say(f'[RED] could not remove {e}: {x}')
    size = tree_size(root)[1]
    if size: say(f'no longer used: {amount(size, "B")} in {root}, removed after the next successful build (deleting the folder now costs nothing)')
    else:
        with contextlib.suppress(OSError): root.rmdir()


def rmtree(p):
    """On exFAT / FAT macOS drops `._x` when `x` is deleted, before the walk reaches it"""
    if p.is_symlink() or p.is_file(): p.unlink()
    elif p.exists(): shutil.rmtree(p, onexc=_gone)


def swap_in(tmp, dst):
    """dst is always the whole old output, the whole new one or absent, never half deleted (a reuse check could take that for finished)"""
    old = dst.with_name(dst.name + '.old.partial')
    here, aside = (lambda: dst.is_symlink() or dst.exists()), (lambda: old.is_symlink() or old.exists())
    if aside():
        if here(): rmtree(old)
        else: old.rename(dst)
    if not here(): tmp.rename(dst); return
    try:
        dst.rename(old)
        tmp.rename(dst)
    except BaseException as e:
        if aside() and not here():
            try: old.rename(dst)
            except OSError as back: raise BuildError(f'publishing {dst} failed ({e!r}), and the old output could not be put back ({back}): it is kept in {old}') from e
        raise
    rmtree(old)


def free_gb(p): return vol_space.free_bytes(p) / 2**30


def tool_env(environ=os.environ):
    return dict(environ, PATH=os.pathsep.join([environ.get('PATH') or '/usr/bin:/bin:/usr/sbin:/sbin'] + TOOL_PATHS))


def files_with_suffix(folder, *suffixes):
    """Path.glob is case-sensitive even on a case-insensitive volume; AppleDouble `._<name>` files left out"""
    return sorted(p for p in folder.iterdir() if p.suffix.lower() in suffixes and not p.name.startswith('._') and p.is_file())


def vnum(ver):
    return int(ver[1:])


REWRITER_FILES = ('patch_nso.py', 'jump_solver.py', 'nso.py', 'nso_code.py', 'nso_asm.py', 'nso_syms.py', 'nso_match.py')   # in the rewrite's key


EMPTY_TABLES = {   # until the split fills them: the inputs' digest and the host's build read them
    'shader_map.tsv': '# kind: evidence  derived profile: filled by the shader split\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n',
    'shader_facts.tsv': '# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> fact JSON. Derived profile: filled by the shader split\n',
    'shader_pairs.tsv': '# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n',
    'shader_map.tsv.conflicts.tsv': '# kind: evidence  control-section key conflict rulings: empty\ndm_ctl_sha16\tchosen_pc\tclass\tcandidates\trecords\n',
    'bf_want.tsv': '# generated\n',
    'pso_manifest.tsv': '# Prewarm seed list (shipped in the app, next to shader_map.tsv). Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex) / C<TAB>compute shader key / M<TAB>task shader key or -<TAB>native task / mesh pipeline key (pso_skey hex: the mesh shader in the vertex place).\n'
                        '# Empty until recorded on the Mac (DM_PSO_MANIFEST=off DM_PSO_RECORD).\n'}


def nup_versions(fam):
    out = {'v0': None} if fam['base'] is not None else {}
    for v, p in fam['updates']: out[f'v{v}'] = p
    return out


def pick_packages(fam, have, versions):
    if fam['base'] is not None: say(f'base: {fam["base"].name}')
    out = {}
    for ver in versions:
        if ver not in have: raise BuildError(f'no update {ver} in the NUP (the game profile needs these versions: {", ".join(versions)})')
        if have[ver]: say(f'update {ver}: {have[ver].name}')
        out[ver] = (fam['base'], have[ver])
    return out


def work_dir(args):
    w = args.work_dir or WORK_DIR
    return Path(os.path.expanduser(w)).resolve() if w else None


def build_folder(args, nup_dir, gid):
    return work_dir(args) or nup_dir / f'{gid}_build'


def own_profile_folders(args, nup_dir, app):
    """Left out of the profile lookup, so a profile is never derived from a derived one"""
    out = [nup_dir / f'{app:016X}_build' / 'profile']
    if work := work_dir(args): out.append(work / 'profile')
    if args.profile_out or PROFILE_OUT: out.append(Path(os.path.expanduser(args.profile_out or PROFILE_OUT)))
    return {p.resolve() for p in out}


def profile_folder(args, work):
    if args.profile_out or PROFILE_OUT: return Path(os.path.expanduser(args.profile_out or PROFILE_OUT)).resolve()
    return work / 'profile'


# Derived profile: a version the game's profile was not brought up on
DERIVED_SKIP = {'game.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv', 'shader_programs.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'patches.py'}   # made again from the version's files; patches.py holds the registered builds' cuts; shader_programs.tsv describes the registered version's files, which only the prewarm list's carry reads


def derived_key(profile_dir):
    return profile_dir.with_name(profile_dir.name + '.derived.key')


def derived_split_key(profile_dir):
    return profile_dir.with_name(profile_dir.name + '.derived.split.key')


def frame_step_builds(path, table='FRAME_STEP'):   # read without running the file
    try:
        for n in ast.parse(path.read_text(encoding='utf-8')).body:
            if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == table for t in n.targets) and isinstance(n.value, ast.Dict):
                return {k.value for k in n.value.keys if isinstance(k, ast.Constant)}
    except (OSError, UnicodeError, SyntaxError): pass
    return set()


def profiles_first(env, pdir):
    os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
    os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'


GPU_DRIVERS = {'NintendoSDK_OpenGL', 'NintendoSDK_Vulkan'}   # they program the GPU through NvRm, which the host does not have


def load_order(m):
    return (m != 'main', m == 'sdk', int(m[6:]) if m.startswith('subsdk') and m[6:].isdigit() else 0)


def nso_image(path):
    return bytes(nso.load(Path(path).read_bytes(), str(path)).image)


def nso_files(exefs, skip=()):
    out = []
    for p in sorted(Path(exefs).iterdir()):
        if p.name in skip or p.name == 'rtld' or not p.is_file(): continue
        with open(p, 'rb') as f:
            if f.read(4) == b'NSO0': out.append(p)
    return out


def module_kind(m):
    if m['comps']: return sorted(m['comps'])[0]
    if any(s.startswith('glslc') for s in m['exports']): return 'GLSLC'
    areas = collections.Counter(nso_syms.area(s) for s in m['exports'])
    return areas.most_common(1)[0][0] if areas else '?'


def main_modules(exefs, modules, hooks=None):
    """Modules the list leaves out that export what main imports and nothing else supplies (slots change between versions); never a GPU driver"""
    exefs = Path(exefs)
    if not nso_files(exefs, skip=modules): return {}
    if hooks is None: hooks = nso_syms.host_hooks(str(Path(__file__).resolve().parents[1] / 'dm/host'), forced=False)
    want = {s for s, defined, _ in nso_syms.dynsyms(nso_image(exefs / 'main')) if not defined}
    exp = {}
    for p in nso_files(exefs):
        img = nso_image(p)
        exp[p.name] = {'exports': {s for s, defined, bind in nso_syms.dynsyms(img) if defined and bind in (1, 2)},
                       'comps': {c.decode().rsplit('-', 1)[0] for c in re.findall(rb'NintendoSDK_(?!nnSdk)\w+-[0-9_]+', img)}}
    have = set(hooks)
    for m in modules: have |= exp.get(m, {}).get('exports', set())
    want -= have
    return {m: module_kind(e) for m, e in sorted(exp.items(), key=lambda x: load_order(x[0]))
            if m not in modules and want & e['exports'] and module_kind(e) not in GPU_DRIVERS}


def derive_profile(g, pdir, ver, d, mods, shader_only=False):
    """Frame step stubs for other builds only give fps_target 0: without the state page a 30 or 60 cap would run the game at double speed."""
    if pdir.resolve() == g.dir.resolve():   # the delete below would take the files the copy reads
        raise BuildError(f'game profile {g.id} would be derived onto its own folder {g.dir}')
    src = sorted(p for p in g.dir.iterdir() if p.is_file() and (p.name not in DERIVED_SKIP or shader_only and p.name == 'patches.py'))
    # '?' is what the game shows without a display version
    main = d / 'ExeFS/main'; bid = build_id(main) if main.exists() else None
    known = next((b['display'] for b in g.builds().values() if bid and b.get('main') == bid and b.get('display') not in (None, '', '?')), None)
    T, disp = '\t', nacp.version(d / 'control.nacp') or known or '?'
    add = main_modules(d / 'ExeFS', mods) if not shader_only and main.exists() else {}
    mods = sorted({*mods, *add}, key=load_order) if add else mods
    row = T.join(['build', ver, disp] + [f'{m}={build_id(d / "ExeFS" / m)}' for m in mods if (d / 'ExeFS' / m).exists()])
    # by ID, not folder: another copy of the repository keeps the derived folder
    out, put = [f'# Derived from game profile {g.id} by scripts/build_app.py for {ver} ({disp}), a version that profile was not brought up on:',
                '# its keys as they are, this version\'s build row, and shader tables made from this version\'s own files'], False
    if shader_only: out = [f'# Derived shader tables from game profile {g.id}; registered module builds and settings are preserved.']
    stubs = frame_step_builds(g.dir / 'patches.py')
    # where the rewrite does not match a frame step, the host keeps the game's own present interval
    matched = 'main' in frame_step_builds(g.dir / 'patches.py', 'MATCH_FRAME_STEP')
    own_step = shader_only or not stubs or matched or not main.exists() or bid in stubs
    for line in (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
        k = line.split(T, 1)[0]
        if not own_step and k == 'fps_target' and line.partition(T)[2].strip() != '0':
            out += ['# the profile has frame step stubs for other builds only: this version\'s main has none, so it keeps the game\'s own step', f'fps_target{T}0']; continue
        if not shader_only and k == 'modules' and (d / 'ExeFS').is_dir():
            want = line.partition(T)[2].split(); have = [m for m in want if (d / 'ExeFS' / m).exists()]
            if add: have = sorted({*have, *add}, key=load_order)
            if have != want:   # left in, the rewrite would stop on the missing file
                if gone := [m for m in want if m not in have]: out.append(f'# this version ships no {" ".join(gone)}')
                if add: out.append('# main imports from ' + ', '.join(f'{m} ({kind})' for m, kind in add.items()) + ', which the profile does not list')
                out.append(f'modules{T}{" ".join(have)}'); continue
        if not shader_only and k in ('build', 'package', 'shader_versions'):
            if not put: out += [row, f'package{T}{ver}', f'shader_versions{T}{ver}']; put = True
            continue
        out.append(line)
    text, kf, skf = '\n'.join(out) + '\n', derived_key(pdir), derived_split_key(pdir)   # beside the profile, so no tool reads them as tables
    key = fingerprint(('files', src), text)
    # when only other copied files changed, the tables the split and shader step wrote are kept
    skey = fingerprint(('files', [p for p in src if p.name in SPLIT_TABLES]), text)
    def rows(text):
        return [tuple(c.strip() for c in l.split(T)) for l in text.splitlines() if l.strip() and not l.lstrip().startswith('#')]
    def game_kept():   # later stages may have added their own rows
        expected = collections.Counter(rows(text)); keys = {r[0] for r in expected}
        try: kept = collections.Counter(r for r in rows((pdir / 'game.tsv').read_text(encoding='utf-8')) if r[0] in keys)
        except (OSError, UnicodeError): kept = None
        return kept == expected
    def copy_in(keep):
        for p in pdir.iterdir():
            if p.name != 'game.tsv' and not keep(p): rmtree(p)
        for p in src:
            # a link: patch_nso.py deduplicates real paths, so a copy would register the same cuts twice
            if shader_only and p.name == 'patches.py': (pdir / p.name).symlink_to(p.resolve())
            else: shutil.copyfile(p, pdir / p.name)
        kf.write_text(f'{key}\n'); skf.write_text(f'{skey}\n')
    why = 'no folder yet' if not (pdir / 'game.tsv').is_file() else 'game.tsv changed since' if not game_kept() else None
    if not why:
        if reused(kf, key) is not None: return disp
        if reused(skf, skey) is not None:
            copy_in(lambda p: p.name in DERIVED_SKIP and p.name != 'patches.py')
            say(f'derived profile {pdir}: copied files changed, the split\'s tables kept'); return disp
        why = 'no split key' if not skf.is_file() else 'the split\'s inputs changed'
    say(f'derived profile {pdir}: made again ({why})')
    rmtree(kf); rmtree(skf); pdir.mkdir(parents=True, exist_ok=True)
    atomic_text(pdir / 'game.tsv', text)
    copy_in(lambda p: False)
    return disp


# Stage reuse: a stage whose inputs are as at its last successful run keeps its output
def files_under(d):
    out, todo = [], [str(d)]
    while todo:
        try: it = rfs.os.scandir(todo.pop())
        except OSError: continue
        with it:
            for e in it:
                if e.is_dir(follow_symlinks=False): todo.append(e.path)
                else: out.append((os.path.relpath(e.path, d), e.stat(follow_symlinks=False)))
    return sorted(out, key=lambda x: x[0])


def fingerprint(*items, env=None):
    """('files', paths) = names and content; ('tree', d) = path and size; ('stat', d) = path, size and mtime; ('cmd', argv) = version output; else repr"""
    h = hashlib.sha256()
    for it in items:
        kind, v = it if isinstance(it, tuple) and len(it) == 2 and it[0] in ('files', 'tree', 'stat', 'cmd') else ('repr', it)
        h.update(f'\0{kind}\0'.encode())
        if kind == 'files':
            for p in map(rfs.Path, v):
                h.update(p.name.encode() + b'\0')
                try: h.update(hashlib.sha256(p.read_bytes()).digest())
                except OSError: h.update(b'missing')
        elif kind in ('tree', 'stat'):
            for rel, st in files_under(v): h.update(f'{rel}\0{st.st_size}\0{st.st_mtime_ns if kind == "stat" else ""}\n'.encode())
        elif kind == 'cmd':
            try:   # metal --version ends with InstalledDir:, the toolchain's mount, renamed at each boot (dmsh_mtlcache.compiler_id strips it too)
                r = subprocess.run(v, env=env, capture_output=True, text=True, timeout=120); h.update(re.sub(r'(?m)^InstalledDir:.*\n?', '', r.stdout + r.stderr).encode())
            except (OSError, subprocess.TimeoutExpired): h.update(b'missing')
        else: h.update(repr(v).encode())
    return h.hexdigest()


def reused(keyfile, key):   # None when the key differs or is missing
    try: k, _, note = keyfile.read_text().partition('\n')
    except OSError: return None
    return note.strip() if k == key else None


def python_tool_files(repo, entry, roots):
    dirs, todo, seen = [repo / d for d in roots], [repo / entry], set()
    while todo:
        p = todo.pop()
        if p in seen or not p.is_file(): continue
        seen.add(p)
        for n in ast.walk(ast.parse(p.read_bytes())):
            mods = [a.name for a in n.names] if isinstance(n, ast.Import) else [n.module] if isinstance(n, ast.ImportFrom) and n.module else []
            todo += [d / f'{m.split(".")[0]}.py' for m in mods for d in dirs]
    return sorted(seen)


def translator_key(xlate):   # mtime and path change with each checkout and worktree
    return ('files', [xlate])


def chain_files(repo):
    return python_tool_files(repo, 'dm/gfx/tools/dmsh_build.py', ('dm/gfx/tools', 'dm/games')) + [repo / 'dm/gfx/tools/dmsh_chain.lock']


SPLIT_TABLES = ('shader_facts_manual.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')


def split_profile_inputs(g):
    settings = [(name, g.get(name, '')) for name in ('modules', 'shader_groups', 'shader_note')]
    tables = [g.table(n) for n in SPLIT_TABLES]
    return [g.id, settings, ('files', tables)]


def split_output_key(pkg, g):
    return fingerprint(package_key(pkg), ('files', [pkg.with_name('pso_pairs.tsv')] if not g.table('pso_pairs.tsv').is_file() else []))


def package_key(pkg):
    fs = [rel for rel, _ in files_under(pkg)]
    return fingerprint(fs, ('files', [pkg / f for f in fs]))


def profile_files(g):   # not the chain tables the shader step makes, nor patches.py, which only the rewrite reads
    out = {name for name, _, _ in CHAIN_TABLES} | {'patches.py'}
    return sorted(p for p in g.dir.iterdir() if p.is_file() and p.name not in out)


def check_capstone():   # patch_nso.py checks the same, but only after the extraction
    import capstone
    if capstone.cs_version()[0] != 5:   # Capstone 6 renamed CS_ARCH_ARM64 / ARM64_OP_*
        cmd = shlex.join([sys.executable, '-m', 'pip', 'install', 'capstone>=5,<6'])
        raise BuildError(f'Capstone {capstone.__version__} is unsupported; the rewriter requires Capstone 5.x. Run: {cmd}')


def check_prereqs(env, xlate, rust=None):
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools', 'zbic') if not _importable(m)] + ([] if _importable('compression.zstd') or _importable('backports.zstd') else ['backports.zstd'])   # zstd is built in from Python 3.14
    if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)} (pip install lz4 "capstone>=5,<6" fonttools zbic backports.zstd; backports.zstd only before Python 3.14)')
    check_capstone()
    tools = {'xcodebuild': 'Xcode', 'make': 'Xcode', 'cargo': 'Rust: install rustup, https://rustup.rs'}
    gone = [f'{t} ({how})' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if gone: raise BuildError('tools not found: ' + ', '.join(gone))
    if rust: check_rust(env, rust)
    mtl = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '--version'], capture_output=True, text=True, env=env)   # a separate download since Xcode 26
    if mtl.returncode: raise BuildError(f'the Metal compiler does not run ({(mtl.stderr.strip().splitlines() or ["no output"])[-1]}): install it with xcodebuild -downloadComponent MetalToolchain')
    if not xlate.exists(): raise BuildError(f'the shader translator is missing from the repository: {xlate}')
    check_translator(xlate, env)
    sc = env.get('SPIRV_CROSS', str(xlate.parent.parent / 'spirv_cross_bin/spirv-cross'))
    if not shutil.which(sc, path=env.get('PATH')): raise BuildError(f'spirv-cross is missing or not executable: {sc}')


def check_rust(env, rust):
    """A missing toolchain stops the build here, not after the shaders"""
    cargo = shutil.which('cargo', path=env['PATH'])
    try: r = subprocess.run([cargo or 'cargo', '--version'], cwd=rust, env=env, capture_output=True, text=True, timeout=1800, errors='replace')
    except (OSError, subprocess.SubprocessError) as e: raise BuildError(f'cargo does not start ({e}): install Rust with rustup (https://rustup.rs)') from e
    if r.returncode == 0: return
    why = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or [f'exit code {r.returncode}'])[-1]
    raise BuildError(f'the Rust that {rust / "rust-toolchain.toml"} names is not available ({why}): run `rustup toolchain install` in {rust}')


def check_translator(xlate, env):
    try: r = subprocess.run([str(xlate), '--source-sha'], capture_output=True, text=True, env=env, timeout=60, errors='replace')
    except (OSError, subprocess.SubprocessError) as e: raise BuildError(f'the shader translator does not start ({e}): {xlate}') from e
    if r.returncode == 0 and r.stdout.strip(): return
    why = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or [f'exit code {r.returncode}'])[0]
    raise BuildError(f'the shader translator does not run ({why}): {xlate} is the committed native executable; check out the repository again if it is missing or damaged')


def check_native_python():
    # tools started by an x86_64 Python run under Rosetta too, so the host would be built for x86_64
    if sys.platform != 'darwin' or subprocess.run(['sysctl', '-n', 'sysctl.proc_translated'], capture_output=True, text=True).stdout.strip() != '1': return
    raise BuildError(f'this Python ({sys.executable}) is an Intel one running under Rosetta, and the tools it starts would run as Intel code too: run the script with an arm64 Python (/usr/bin/python3, or Homebrew\'s in /opt/homebrew)')


def _importable(m):
    try: __import__(m); return True
    except ImportError: return False


def nup_file(path):
    if path.is_file():
        if not pack_nup.is_nup(path): raise BuildError(f'not a NUP file: {path}')
        return path
    nups = [p for p in files_with_suffix(path, '.nup') if pack_nup.is_nup(p)]
    if len(nups) > 1: raise BuildError(f'more than one NUP in {path}: {", ".join(p.name for p in nups)}; give the .nup file to build from in place of the folder')
    if not nups: raise BuildError(f'no NUP (a .nup file) in {path}')
    return nups[0]


def scan_nup(path):
    try:
        n = pack_nup.Nup(path)
        app, base, vers, dlc = pack_nup.scan(n)
    except (pack_nup.Error, OSError) as e: raise BuildError(str(e)) from e
    return app, {'base': base, 'updates': sorted(vers.items()), 'dlc': sorted(dlc.items()), 'nup': n}


def extract_nup(ex, name, k, base, app, n):
    dst = ex / name; part = dst / 'RomFS.img.part'
    def poll():
        try: return {'done': part.stat().st_size}
        except OSError: return None
    try:
        PROG.stage(f'unpack {name}', poll=poll, unit='B', total=pack_nup.romfs_bytes(n, k, base), est=True)
        how = pack_nup.pack_version(n, k, base, app, dst, say=lambda s: None, step=lambda s: PROG.set(step=s))
    except pack_nup.Error as e: raise BuildError(str(e)) from e
    PROG.end(amount((dst / 'RomFS.img').stat().st_size, 'B') + {'kept': ', reused (inputs unchanged)', 'verified': ', reused (image checked)'}.get(how, ''))
    return dst


def check_formats(dirs):
    """The game retries a damaged file on every pass of its background loader."""
    import romfs_image
    for ver, img in ((ver, d / 'RomFS.img') for ver, d in dirs.items()):
        stamp = img.with_name(img.name + '.formats')
        st = img.stat(); ident = f'{st.st_size} {st.st_mtime_ns}'
        try: lines = stamp.read_text().splitlines()
        except OSError: lines = []
        if lines and lines[0] == ident: bad = [ln.split('\t') for ln in lines[1:]]
        else:
            PROG.add(1); PROG.stage(f'check {ver} files', step='reading file heads', unit='files')
            bad = [(p, str(size), head.hex()) for p, size, head in romfs_image.format_mismatches(img, lambda done, total: PROG.set(done=done, total=total))]
            stamp.write_text('\n'.join([ident] + ['\t'.join(b) for b in bad]) + '\n')
            PROG.end(f'{len(bad)} damaged' if bad else 'ok')
        if not bad: continue
        rows = '\n'.join(f'  {p} ({int(size):,} bytes, starts {head or "(nothing)"})' for p, size, head in bad[:20]) + (f'\n  … {len(bad) - 20} more' if len(bad) > 20 else '')
        msg = f'{ver}: {len(bad)} file(s) whose bytes do not start as their names promise (a .zs file that is not a zstd stream), so the game files are damaged:\n{rows}'
        if not ALLOW_DAMAGED: raise BuildError(f'{msg}\nBuild again from a good copy of the game (--allow-damaged packs these files as they are)')
        say(f'[RED] {msg}\npacked as they are (--allow-damaged)')


def extract_aoc(work, app, fam):
    """<index> = title ID − the add-on content base ID; the host reports these as owned"""
    if not fam['dlc']: return None
    PROG.stage('unpack DLC', step='unpacking')
    try: pack_nup.pack_dlc(fam['nup'], app, dict(fam['dlc']), work / 'aoc', say=lambda s: None)
    except pack_nup.Error as e: raise BuildError(str(e)) from e
    PROG.end(f'{len(fam["dlc"])} DLC')
    return work / 'aoc'


def derived_reason(g, have, ver):
    """Why ver is built from a derived profile, or False: a registered version uses the profile when its shader tables are that version's and every version they came from is in the NUP.
    A registered version outside shader_versions has shaders of its own (Ocarina of Time's 1.0.0): its builds stay registered, with tables of its own files"""
    builds, need = g.builds(), g.words('shader_versions')
    if ver not in builds: return f'{ver} is not a version game profile {g.id} registers ({", ".join(v + " " + b["display"] for v, b in builds.items())})'
    if ver not in need: return f'{ver} ({builds[ver]["display"]}) has shaders of its own: game profile {g.id}\'s shader tables are those of {", ".join(need)}'
    if lost := [v for v in need if v not in have]: return f'the NUP has no {", ".join(lost)}, whose files game profile {g.id}\'s shader tables were made from'
    return False


def build_mismatch(d, name, g):   # [] = the registered build
    want = g.builds().get(name)
    if want is None: return [f'{name} has no build row']
    return [f'{m} {build_id(d / "ExeFS" / m)} (registered {bid})' for m, bid in want.items() if m != 'display' and build_id(d / 'ExeFS' / m) != bid]


def patch_files(repo, exe_dir, mods, game_id=None):
    ids = {build_id(exe_dir / m) for m in mods}
    selected = Path(game_id).name if game_id else None
    extra = [p for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d for p in sorted(Path(d).glob('*/patches.py'))]
    shared = [repo / 'dm/tools/sdk_patches.py']
    files = {}
    for p in shared + sorted((repo / 'dm/games').glob('*/patches.py')) + extra: files.setdefault(p.resolve(), p)
    return [p for p in files.values()
            if p in shared or p.parent.name == selected or any(i in p.read_text(errors='replace') for i in ids)]


def rewrite(py, repo, exe_dir, out, env, logs, g):
    import capstone
    try: workers = int(env['DM_REWRITE_J']) if env.get('DM_REWRITE_J') else None
    except ValueError: raise BuildError('DM_REWRITE_J must be a positive integer') from None
    if workers is not None and workers < 1: raise BuildError('DM_REWRITE_J must be a positive integer')
    PROG.stage('rewrite', step='checking the inputs')
    mods = g.modules_in(exe_dir)
    src = {m: exe_dir / m for m in mods}
    kf = out.with_name(out.name + '.key')
    # the build rows decide which builds are the game's own (SDK-wide patches skip those)
    try: own = [l for l in (repo / 'dm/games' / Path(env.get('DM_GAME') or '-').name / 'game.tsv').read_text(encoding='utf-8').splitlines() if l.startswith('build\t')]
    except OSError: own = []
    key = fingerprint(('files', [repo / 'dm/tools' / f for f in REWRITER_FILES] + patch_files(repo, exe_dir, mods, env.get('DM_GAME')) + list(src.values())), mods, capstone.__version__,
                      {k: env.get(k) for k in ('DM_PATCH_FRONT', 'DM_GAME')}, own)
    if all((out / f'{m}.patched.bin').is_file() for m in mods) and reused(kf, key) is not None:
        PROG.end(f'{", ".join(mods)}, reused (inputs unchanged)'); return
    tmp = out.with_name(out.name + '.partial'); rmtree(tmp); tmp.mkdir(parents=True); logs.mkdir(parents=True, exist_ok=True)
    PROG.set(step=', '.join(mods), done=0, total=len(mods), unit='modules')
    groups, todo, cores, stop_sig = {}, list(mods), max(2, os.cpu_count() or 4), signal.SIGTERM
    jobs = max(1, min(cores, 2 * cores // max(1, len(mods))))   # so parallel rewrites don't oversubscribe
    rewrite_env = dict(env, DM_REWRITE_J=str(workers if workers is not None else jobs))

    def stop_remaining():   # each rewrite's worker pool is in its group: a failed sibling or an interrupt stops them all
        try: proc_group.stop_all([grp for grp, _ in groups.values()], stop_sig)
        finally:
            for _, fh in groups.values(): fh.close()
            groups.clear()

    def fail(m, why):
        stop_remaining()
        log = logs / f'rewrite_{m}.log'
        PROG.end(ok=False)
        with open(log, errors='replace') as lf: tail = collections.deque(lf, maxlen=40)
        PROG.say(f'---- last lines of {log} ----\n' + ''.join(tail).rstrip('\n'))
        raise BuildError(f'{why} (full output: {log})')
    try:
        while todo or groups:
            while todo and len(groups) < cores:
                m = todo.pop(0); f = open(logs / f'rewrite_{m}.log', 'w')
                try: grp = proc_group.Group([str(py), str(repo / 'dm/tools/patch_nso.py'), str(src[m]), str(tmp), m], env=rewrite_env, stdout=f, stderr=subprocess.STDOUT)
                except BaseException:
                    f.close(); raise
                groups[m] = (grp, f)
            for m in [m for m, (grp, _) in groups.items() if grp.p.poll() is not None]:
                grp, f = groups[m]
                if grp.p.returncode: fail(m, f'patch_nso.py exit code {grp.p.returncode} on {m}')
                groups.pop(m)
                try:
                    rc = grp.finish()
                    if grp.stragglers:
                        os.write(f.fileno(), f'[RED] patch_nso.py exited, but {proc_group.describe(grp.stragglers)} still ran {grp.drain:g} s later and was stopped: what it writes may be incomplete\n'.encode())
                except BaseException:
                    groups[m] = (grp, f); raise
                finally:
                    if m not in groups: f.close()
                PROG.set(done=len(mods) - len(todo) - len(groups))
                if rc: fail(m, f'patch_nso.py exit code {rc} on {m}')
                if grp.stragglers: fail(m, f'patch_nso.py exited on {m}, but {proc_group.describe(grp.stragglers)} still ran {grp.drain:g} s later and was stopped: what it writes may be incomplete')
            if todo or groups: time.sleep(0.2)
    except KeyboardInterrupt:
        stop_sig = signal.SIGINT; raise
    finally:
        stop_remaining()
    rmtree(kf); swap_in(tmp, out); kf.write_text(f'{key}\n')
    PROG.end(', '.join(mods))


def apple_silicon():   # 1 under Rosetta too
    return subprocess.run(['sysctl', '-n', 'hw.optional.arm64'], capture_output=True, text=True).stdout.strip() == '1'


HOST_DIRS = ('dm/host', 'dm/gfx', 'dm/gfx/nvn', 'dm/gfx/nvn/include', 'dm/gfx/tex')   # dm/host/Makefile SRC, GFX
RUST_DIR = 'dm/host/rust'


def rust_files(repo):
    r = repo / RUST_DIR
    return [r / 'Cargo.toml', r / 'Cargo.lock', r / 'rust-toolchain.toml', repo / 'dm/host/rust.mk', *sorted((r / 'src').rglob('*.rs'))]


def build_mac_host(repo, out, env, logs, g):
    """make takes no target path with spaces, and dm/host/dmhost may be another build's"""
    host = repo / 'dm/host'
    run(['make', '-s', f'GAME={g.id}', f'gen/{g.id}/game_profile.h', 'gen/dav1d/libdav1d.a', 'host_msl'], logs / 'mac_host_make.log', env=env, cwd=host)
    built = host / f'gen/{g.id}/dmhost'
    dry = subprocess.run(['make', '-n', '-B', f'GAME={g.id}', f'gen/{g.id}/dmhost'], env=env, cwd=host, capture_output=True, text=True)
    cmd = [l for l in dry.stdout.splitlines() if l.startswith('clang') and f' -o gen/{g.id}/' in l]
    if not any(f' -o gen/{g.id}/dmhost ' in l for l in cmd): raise BuildError(f'make -n gave no link line for the Mac host: {dry.stderr.strip()[-500:]}')
    kf, tp = out.with_name(out.name + '.key'), repo / 'dm/third_party'
    srcs = [p for d in HOST_DIRS if (repo / d).is_dir() for p in sorted((repo / d).iterdir()) if p.is_file() and p.suffix in ('.c', '.m', '.h', '.S', '.inc')]
    key = fingerprint(fingerprint(cmd, ('files', [host / f'gen/{g.id}/game_profile.h', host / 'gen/msl-macosx/host_msl_gen.h', host / 'gen/dav1d/libdav1d.a']),
                                  ('tree', tp / 'dav1d/include'), ('cmd', ['clang', '--version']), ('cmd', ['xcrun', '--show-sdk-build-version']), env=env),
                      ('files', srcs), ('files', rust_files(repo)))
    if out.is_file() and reused(kf, key) is not None: return out
    rmtree(kf); out.parent.mkdir(parents=True, exist_ok=True)
    run(['make', '-s', f'GAME={g.id}', f'gen/{g.id}/dmhost'], logs / 'mac_host_build.log', env=env, cwd=host)
    part, dsym = out.with_name(out.name + '.partial'), Path(f'{out}.dSYM')
    shutil.copy2(built, part); os.replace(part, out)
    rmtree(dsym)
    if Path(f'{built}.dSYM').is_dir(): shutil.copytree(f'{built}.dSYM', dsym)   # crash reports and lldb read it beside the program
    kf.write_text(f'{key}\n')
    return out


def atomic_text(path, text):
    f = tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, prefix=f'.{path.name}.', delete=False)
    try:
        with f: f.write(text)
        os.replace(f.name, path)
    finally: Path(f.name).unlink(missing_ok=True)


CHAIN_H = 'dm/gfx/nvn/dmsh_chain.h'


def chain_of(repo):
    try: m = re.search(r'#define DMSH_CHAIN "(.*)"', (repo / CHAIN_H).read_text())
    except OSError: return None
    return m.group(1) if m else None


def empty_tables(g):
    for n, text in EMPTY_TABLES.items():
        if not g.table(n).exists(): g.table(n).write_text(text)


def chain_header(py, repo, env, logs):
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py')], env=env, capture_output=True, text=True)
    if r.returncode or not r.stdout.split(): raise BuildError(f'dmsh_chain.py exit code {r.returncode}: {(r.stderr.strip() or "no version")[-500:]}')
    want, have = r.stdout.split()[-1], chain_of(repo)
    if have == want: return
    run([py, repo / 'dm/gfx/tools/dmsh_chain.py', '--header'], logs / 'dmsh_chain.log', env=env)
    say(f'chain version header rewritten: {repo / CHAIN_H} held {have or "no version"}, the source tree gives {want}')


CHAIN_TABLES = (('c1_full.tsv', 'c1_guard.py', 'c1 guard'), ('cb_extent.tsv', 'cb_extent.py', 'uniform read extents'))   # the host takes each only when its header names its library's stamp


def lib_head(lib):
    stamp = shader_lib.read_stamp(lib)
    return f'# chain {shader_lib.name_of(lib)} inputs {stamp["inputs"]}' if stamp.get('inputs') else None


def chain_tables(py, repo, sh, env, logs, g, chain):
    """The host ignores tables that do not name the library's stamp."""
    lib = sh / 'cache' / f'b-{chain}'
    want = lib_head(lib)
    if not want: return False
    for name, tool, step in CHAIN_TABLES:
        p = g.table(name)
        try: head = p.read_text(errors='replace').partition('\n')[0]
        except OSError: head = ''
        if head == want or head.startswith(want + ' '): continue   # older tables carry a time after it
        PROG.set(step=step)
        run([py, repo / 'dm/gfx/tools' / tool, lib, p], logs / f'{tool[:-3]}.log', env=dict(env, DMSH_TABLE_MEMO=str(sh / 'cache')))
        say(f'{name} ' + (f'named {" ".join(head.split()[2:5])}' if head.startswith('# chain ') else 'names no library' if head else 'missing') + f': made for {want[2:]} ({p})')
    return True


def data_rows(b):   # the comment lines change with the shader_split.py version, not the game files
    return [l for l in b.splitlines() if not l.startswith(b'#')]


def table_difference(name, profile, split):
    """Compared as multisets, so one row added early does not read as every later row changed"""
    a, b = collections.Counter(data_rows(profile)), collections.Counter(data_rows(split))
    only_profile, only_split = list((a - b).elements()), list((b - a).elements())
    if not only_profile and not only_split: return f'{name}: the same rows in another order'
    out = [f'{name}: {len(only_profile):,} row(s) only in the profile, {len(only_split):,} only in the split']
    if only_profile: out.append('  profile: ' + only_profile[0].decode(errors='replace'))
    if only_split: out.append('  split:   ' + only_split[0].decode(errors='replace'))
    return '\n'.join(out)


def dlc_only_shader_change(g, tables):
    def rows(path):
        out = {}
        for line in path.read_text().splitlines():
            if not line.strip() or line.startswith(('#', 'package\t')): continue
            c = line.split('\t')
            if len(c) != 5 or not c[0].endswith('.bin') or c[3] in out: return None
            out[c[3]] = (c[0], c[2], c[4])
        return out
    try:
        old, new = rows(g.table('shader_map.tsv')), rows(tables / 'shader_map.tsv')
        if old is None or new is None: return False
        old_dlc = {k: v for k, v in old.items() if v[2].startswith('/aoc/')}
        new_dlc = {k: v for k, v in new.items() if v[2].startswith('/aoc/')}
        if old_dlc == new_dlc or {k: v for k, v in old.items() if k not in old_dlc} != {k: v for k, v in new.items() if k not in new_dlc}: return False
        dlc = old_dlc.keys() | new_dlc.keys()
        def facts(path):
            out, seen = [], set()
            for line in path.read_text().splitlines():
                if not line.strip() or line.startswith('#'): continue
                c = line.split('\t', 1)
                if len(c) != 2 or c[0] in seen: raise ValueError('invalid facts')
                seen.add(c[0])
                if c[0] not in dlc: out.append(line)
            return out
        return facts(g.table('shader_facts.tsv')) == facts(tables / 'shader_facts.tsv')
    except (OSError, UnicodeError, ValueError): return False


def split_shaders(py, repo, roms, mains, pkg, env, logs, g, own=False, dlc_roms=(), derive_dlc=None):
    """own: a derived profile takes the tables as they come"""
    names = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv')
    PROG.stage('shader split', step='checking the inputs')
    kf, outputkf = pkg.with_name(pkg.name + '.key'), pkg.with_name(pkg.name + '.outputs.key')
    inputs = [('files', python_tool_files(repo, 'dm/gfx/tools/shader_split.py', ('dm/gfx/tools', 'dm/tools', 'dm/games'))), bool(own), 'split-v2']
    # a repaired copy keeps every path and size, so a damaged one is keyed by 'stat' and the damage list
    for r, m in zip(roms, mains):
        inputs += [('files', [m] + [m.parent / n for n in g.modules_in(m.parent) if n != 'main']), ('stat' if own or ALLOW_DAMAGED else 'tree', r)]
        if ALLOW_DAMAGED: inputs += [('files', [rfs.Path(r).with_name('RomFS.img.formats')])]
    inputs += [item for r in dlc_roms for item in (('dlc', Path(r).name), ('stat', r))]   # no registered build pins DLC contents
    key = fingerprint(*inputs, *split_profile_inputs(g))
    dlckf, dlckey, promoted = pkg.with_name(pkg.name + '.dlc.key'), None, False
    def promote():
        nonlocal g, own, env, promoted
        assert derive_dlc is not None
        g, own, promoted = derive_dlc(), True, True; inputs[1] = True
        env = dict(env, DM_GAMES_DIR=os.pathsep.join([str(g.dir.parent), env.get('DM_GAMES_DIR', '')]), DM_GAMES_FIRST='1')
    if not own and derive_dlc:
        dlckey = fingerprint(key, ('files', [Path(__file__)]))
        if reused(dlckf, dlckey) is not None:
            promote()
            key = fingerprint(*inputs, *split_profile_inputs(g))
    have_pairs = g.table('pso_pairs.tsv').is_file() or pkg.with_name('pso_pairs.tsv').is_file()
    if pkg.is_dir() and have_pairs and (note := reused(kf, key)) is not None and reused(outputkf, split_output_key(pkg, g)) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return package_key(pkg)
    PROG.set(step='splitting')
    tmp = pkg.with_name(pkg.name + '.partial'); rmtree(tmp); (tmp / 'tables').mkdir(parents=True)
    def counted(ln):
        if m := re.fullmatch(r'progress: (\d+)/(\d+)', ln): PROG.set(done=int(m[1]), total=int(m[2]), unit='files')
    run([py, repo / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), tmp / 'pkg'] + [tmp / 'tables' / n for n in names + ('pso_pairs.tsv',)] + ([','.join(map(str, dlc_roms))] if dlc_roms else []),
        logs / 'shader_split.log', env=env, on_line=counted)
    text = (logs / 'shader_split.log').read_text(errors='replace')
    m = re.search(r'shaders: (\d+) \(', text)
    bad = re.search(r'^\[RED\] (\d+) file\(s\) could not be read.*$', text, re.M)
    if bad and not own:   # the profile's tables were split from every file: one unreadable now is a reader change
        raise BuildError(f'{bad.group(0).removeprefix("[RED] ")}; the profile\'s tables hold every file\'s shaders (each file in {logs / "shader_split.log"})')
    if bad: say(f'{bad.group(0)}; the build goes without them (each file in {logs / "shader_split.log"})')
    if m and m.group(1) == '0':   # dmsh_build.py would make no library
        raise BuildError('the shader split found no shaders in main or the RomFS: the game keeps them in a form shader_split.py does not read')
    if not own and derive_dlc and dlc_only_shader_change(g, tmp / 'tables'): promote()
    # kept beside the package, where dmsh_build.py and the pipeline prebuild read it
    if own or g.table('pso_pairs.tsv').is_file(): names += ('pso_pairs.tsv',)
    else: shutil.copyfile(tmp / 'tables' / 'pso_pairs.tsv', pkg.with_name('pso_pairs.tsv'))
    if own:
        for n in names: shutil.copyfile(tmp / 'tables' / n, g.table(n))
    else:
        diff = [n for n in names if data_rows((tmp / 'tables' / n).read_bytes()) != data_rows(g.table(n).read_bytes())]
        if diff:
            rows = '\n'.join(table_difference(n, g.table(n).read_bytes(), (tmp / 'tables' / n).read_bytes()) for n in diff)
            raise BuildError(f'the split shader tables differ from game profile {g.id}\'s:\n{rows}\nEither these game files are not the versions the profile registers, or shader_split.py now reads them differently and the profile\'s tables were not split again (the split\'s tables: {tmp / "tables"})')
    rmtree(kf); rmtree(outputkf); swap_in(tmp / 'pkg', pkg); rmtree(tmp)
    note = (f'{int(m.group(1)):,} shaders' if m else '') + (f', {bad.group(1)} file(s) unreadable' if bad else '')
    key = fingerprint(*inputs, *split_profile_inputs(g))   # again: a derived profile's tables were just written
    outputkf.write_text(split_output_key(pkg, g) + '\n')
    kf.write_text(f'{key}\n{note}\n')
    if promoted and dlckey: dlckf.write_text(f'{dlckey}\nDLC-only shader content\n')
    else: rmtree(dlckf)
    PROG.end(note)
    return package_key(pkg)


def shader_keys(g):
    keys = set()
    for ln in g.table('shader_map.tsv').read_text(errors='replace').splitlines():
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[0].endswith('.bin'): keys.add(c[3])
    return keys


SHADER_MISSING = re.compile(r'\[RED\] (?:shader ([0-9a-f]{16}) \([^)]*\) failed to load:|failed to load shader ([0-9a-f]{16})_b:)')


def seen_listed(g):
    return set(re.findall(r'^[0-9a-f]{16}\b', g.table('shader_seen.tsv').read_text(errors='replace'), re.M))


def seen_logs(work, app):   # quitting in the settings window starts a new log
    logs = Path(os.path.expanduser('~/Library/Logs/NativeNavi'))
    return work / 'run/host.log', logs / f'{app:016X}.log', logs / f'{app:016X}.prev.log'


def seen_keys(g, sh, logs):
    """None stops the build: the builder takes an empty list for the whole map."""
    listed, known, done = seen_listed(g), shader_keys(g), sh / 'partial-keys.txt'
    try: earlier = set(done.read_text().split())
    except OSError: earlier = set()
    found = set()
    for log in logs:
        try: found.update(a or b for a, b in SHADER_MISSING.findall(log.read_text(errors='replace')))
        except OSError: pass
    keys = sorted((listed | earlier | found) & known)
    if not keys: raise BuildError(f'none of the {len(listed):,} shaders in {g.table("shader_seen.tsv")} is in this version\'s shader map: build the whole map, without --seen-shaders')
    sh.mkdir(parents=True, exist_ok=True); atomic_text(done, ''.join(k + '\n' for k in keys))
    extra = len(set(keys) - listed)
    say(f'partial shader library: {len(keys):,} of {len(known):,} shaders ({len(listed & known):,} from the profile\'s playtests' + (f', {extra:,} that earlier runs reported missing' if extra else '')
        + '); the rest will not draw, so areas the list does not cover draw incomplete until this script runs again')
    return keys


class ShaderWatch:
    def __init__(self, path): self.path = path

    def __call__(self):
        try: p = json.loads(self.path.read_text())
        except (OSError, ValueError): return None
        step, cached, misses = p.get('step') or 'compiling', p.get('cached', 0), p.get('misses')
        if step == 'cache check':
            return {'step': step, 'done': p['done'], 'total': p['total'], 'unit': 'shaders', 'note': f'{cached:,} cached'}
        if step == 'compiling':
            return {'step': step, 'done': p.get('done', 0), 'total': p.get('total', 0), 'unit': 'shaders', 'note': f'{misses:,} not cached' if misses else ''}
        out = {'step': step, **{k: p[k] for k in ('done', 'total') if k in p}}   # the library image (files, bytes written), the c1 guard and uniform read extent tables
        if p.get('unit'): out['unit'] = p['unit']
        if p.get('bytes'): out['note'] = amount(p['bytes'], 'B')
        return out


def spirv_cross(repo, env=None):
    env = os.environ if env is None else env
    selected = env.get('SPIRV_CROSS', str(repo / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'))
    return Path(shutil.which(selected, path=env.get('PATH')) or selected)


def toolchain(repo, env=None):   # Metal is a separate Xcode download
    return (('cmd', ['xcodebuild', '-version']), ('files', [spirv_cross(repo, env)]),
            *(('cmd', ['xcrun', '-sdk', 'macosx', *args]) for args in (['metal', '--version'], ['metallib', '--version'], ['--show-sdk-version'], ['--show-sdk-build-version'])))


def prune_stale_chains(sh, chain):
    """Only once this chain's are in place, so a failed build keeps the old ones."""
    def listing(d, pat): return [d / n for n in (os.listdir(d) if d.is_dir() else []) if re.fullmatch(pat, n) and n not in (f'b-{chain}', f'b-{chain}.img', f'mac-b-{chain}.key')]
    old = [p for p in listing(sh / 'cache', r'b-[0-9a-f]{12}') if p.is_dir()]
    old += [p for d, pat in ((sh, r'mac-b-[0-9a-f]{12}\.key'), (sh / 'cache', r'b-[0-9a-f]{12}\.img(\.part)?')) for p in listing(d, pat) if p.is_file()]
    gone, size = [], 0
    for p in old:
        try: n = tree_size(p)[1] if p.is_dir() else p.stat().st_size; retire(sh.parent, p)
        except OSError as e: say(f'[RED] could not remove old shader libraries {p}: {e}'); continue
        gone.append(p); size += n
    if not gone: return []
    names = sorted({re.sub(r'\.(key|img(\.part)?)$', '', p.name).removeprefix('mac-') for p in gone})
    say(f'removed old shader libraries {", ".join(names)} ({amount(size, "B")}) to {OBSOLETE}/')
    return names


def prune_shader_cache(sh, chain):
    """a cache hit does not rewrite index/, so _b_cas.txt decides; newer entries stay"""
    folder = sh / 'cache'
    shader_cache.drop_generations(folder, chain, say, remove=lambda p: retire(sh.parent, p))
    used = shader_lib.view(folder / f'b-{chain}') / '_b_cas.txt'
    try: keep, since = set(used.read_text().split()), used.stat().st_mtime
    except OSError: return 0
    if not shader_cache.exists(shader_cache.chain_file(folder, chain)): return 0
    store = shader_cache.Store(shader_cache.chain_file(folder, chain), say)
    try: n, size = store.prune_shaders(keep, since)
    finally: store.close()
    if n: say(f'removed old shader cache entries: {n:,} ({amount(size, "B")})')
    return n


def shader_profile_key(py, repo, env, g):
    """So an edit of the note or prewarm list does not rewrite the libraries"""
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py'), '--inputs'], env=env, capture_output=True, text=True)
    words = r.stdout.split() if r.returncode == 0 else []
    if words: return ('shader inputs', g.id, words[-1])
    say(f'[RED] dmsh_chain.py --inputs gave no digest ({(r.stderr.strip().splitlines() or ["exit code " + str(r.returncode)])[-1]}): the shader step is keyed on every profile file, so any profile edit rebuilds it')
    return ('files', profile_files(g))


def stale_shaders(py, repo, env, lib_dir, chain, g):
    """'chain': another chain and no stamped current build (a partial one must not hide them); 'inputs': other profile inputs"""
    cur = lib_dir / f'b-{chain}'
    cached = shader_lib.has_library(cur)
    if not (cached and lib_head(cur)):
        other = [old for old in sorted({Path(shader_lib.folder_of(p)) for p in lib_dir.glob('b-*')}) if old.name != cur.name and shader_lib.has_library(old)]
        if other: return ('chain', ', '.join(sorted({d.name for d in other})), f'b-{chain}', len(shader_keys(g)))
    if not cached: return None
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py'), '--inputs'], env=env, capture_output=True, text=True)
    new = r.stdout.split()[-1] if r.returncode == 0 and r.stdout.split() else None
    have = lib_head(cur)
    if new and have and have.split()[-1] != new: return ('inputs', have.split()[-1], new, len(shader_keys(g)))
    return None


def build_shaders(py, repo, sh, xlate, env, logs, g, pkg_key, seen=None):
    """The key is sh/mac-b-<chain>.key: in sh/cache dmsh_build.py would read it as a library folder. A partial (seen) build gets no stamp and no
    chain tables on purpose: the host would take their silence on a shader it never saw as proof that it is safe."""
    log = logs / 'dmsh_build.log'
    PROG.stage('Mac shaders')
    chain = chain_of(repo)
    cache, misplaced = sh / 'cache', sh / 'partial-cache.db'
    if cache.is_file():   # a partial build on a fresh folder opened the path as its database (DMSH_CAS)
        shader_cache.Store(cache, say).close(); cache.rename(misplaced)   # its WAL folded in before the move
    cache.mkdir(parents=True, exist_ok=True)
    if misplaced.is_file(): misplaced.rename(cache / shader_cache.LEGACY)   # an older generation the next builds read
    out = sh / 'partial' if seen else cache
    libs, kf = out / f'b-{chain}', out / 'key' if seen else sh / f'mac-b-{chain}.key'
    pso_pairs = g.table('pso_pairs.tsv')
    if not pso_pairs.is_file(): pso_pairs = sh / 'pso_pairs.tsv'
    PROG.set(step='checking the inputs')
    key = fingerprint(pkg_key, ('files', chain_files(repo)), translator_key(xlate), shader_profile_key(py, repo, env, g), ('files', [pso_pairs]), *toolchain(repo, env),
                      {k: env.get(k) for k in ('XLATE_DEBUG', 'XLATE_REDUCED')}, *((seen,) if seen else ()), env=env)
    note = reused(kf, key)
    # a missing or stale chain table is made from the last build's outputs; without them the step runs again
    if shader_lib.has_library(libs) and note is not None and (seen or chain_tables(py, repo, sh, env, logs, g, chain)):
        if not seen: prune_stale_chains(sh, chain); prune_shader_cache(sh, chain); shader_lib.pack(libs, say)
        PROG.end(f'{note}, reused (inputs unchanged)'); return out
    rmtree(kf)
    if seen: rmtree(out)   # its keys are in partial-keys.txt
    benv = dict(env, XLATE=str(xlate))
    if ALLOW_OTHER_METAL: benv['DM_ALLOW_OTHER_METAL'] = '1'
    if seen: benv['DMSH_CAS'] = str(cache)
    benv['DMSH_TABLE_MEMO'] = str(sh / 'cache')
    prog = sh / 'progress.json'; rmtree(prog); benv['DMSH_PROGRESS'] = str(prog)
    try: PROG.watch(ShaderWatch(prog))
    except Exception: pass
    try: stale = None if seen else stale_shaders(py, repo, env, sh / 'cache', chain, g)
    except Exception: stale = None
    if stale:
        reason, old, new, n = stale
        PROG.event('shaders_stale', reason=reason, old=old, new=new, count=n)
        say(f'[NOTE] cached shader libraries cannot be reused ({old} -> {new}): ' + ('the translation chain changed' if reason == 'chain' else 'the game profile\'s shader inputs changed')
            + f'; {n:,} base libraries scheduled (including cache restores; later builds reuse as before)')
    if not seen and g.table('shader_seen.tsv').is_file():
        say(f'building the whole shader map, {len(shader_keys(g)):,} shaders: hours the first time. --seen-shaders builds only the {len(seen_listed(g)):,} the profile\'s playtests used, in minutes; areas they do not cover then draw incomplete')
    if not seen: shader_lib.remove_image(libs)   # a stopped build keeps no old shaders under the same chain
    run([py, repo / 'dm/gfx/tools/own_compilers.py', sh / 'pkg', out, *(seen or ())], log, env=benv)
    PROG.watch(None)
    if not seen and not chain_tables(py, repo, sh, env, logs, g, chain): raise BuildError(f'no stamped Mac shader outputs in {libs} (inputs.tsv): dmsh_build.py did not finish a full build')
    n = sum(1 for _ in shader_lib.view(libs).glob('*_b.metallib')) if shader_lib.exists(libs) else 0
    if n == 0: raise BuildError(f'no Mac shader libraries in {libs}')
    say(f'{n} Mac shader libraries ({libs})')
    t = log.read_text(errors='replace')
    if own := re.search(r'^note: (built with this Mac\'s compilers.*)$', t, re.M): say(own.group(1))
    if lost := re.search(r'^\[RED\] lost without failing: .*$', t, re.M): say(f'{lost.group(0)}; each is named in {log}, and the build goes on: a shader the game never reaches costs nothing')
    m, c = re.search(r'shaders (\d+): ok (\d+)', t), re.search(r'shader cache: hit (\d+)', t)
    note = ''
    if m:
        total, ok = int(m.group(1)), int(m.group(2))
        failed = f'; {total - ok:,} failed, reasons at the end of {log}' if ok < total else ''
        note = f'{ok:,} of {total:,} shaders ready{failed}'
        PROG.end(f'{ok:,} of {total:,} shaders ready' + (f' ({int(c.group(1)):,} from the cache)' if c else '') + failed)
    else: PROG.end()
    if not seen: shader_regressions(py, repo, sh / 'cache', chain, logs)   # before the key: a stopped build is checked again on the next run
    kf.write_text(f'{key}\n{note}\n')
    if not seen:   # last: a build that stopped before here keeps the old libraries and cache entries
        prune_stale_chains(sh, chain); prune_shader_cache(sh, chain); shader_lib.pack(libs, say)
        rmtree(sh / 'partial')
    return out


def check_metal(py, repo, env):
    if ALLOW_OTHER_METAL: return
    r = subprocess.run([py, str(repo / 'dm/gfx/tools/own_compilers.py'), '--check-metal'], capture_output=True, text=True, env=env, errors='replace')
    if r.returncode == 0: return
    msg = (r.stdout.strip() or r.stderr.strip() or f'own_compilers.py --check-metal exit code {r.returncode}').removeprefix('[RED] ')
    raise BuildError(re.sub(r'set DM_ALLOW_OTHER_METAL=1 \([^)]*\)', 'ALLOW_OTHER_METAL = True (--allow-other-metal)', msg))


def shader_regressions(py, repo, lib_dir, chain, logs):
    log = logs / 'shader_regressions.log'
    if not (shader_lib.view(lib_dir / f'b-{chain}') / '_b_build.json').is_file(): return
    r = subprocess.run([py, str(repo / 'dm/gfx/tools/shader_regressions.py'), str(lib_dir), chain], capture_output=True, text=True, errors='replace')
    log.write_text(r.stdout + r.stderr)
    if r.returncode == 0: return
    if r.returncode != 1: raise BuildError(f'shader_regressions.py exit code {r.returncode} (full output: {log})')
    lines = r.stdout.splitlines()
    prev = re.search(r'against (b-\w+)', r.stdout)
    msg = f'{len(lines) - 1} shaders that translated in {prev.group(1) if prev else "the previous build"} fail in b-{chain}: a change to the translation chain broke them'
    PROG.say('\n'.join(lines[1:11]) + (f'\n... {len(lines) - 11} more in {log}' if len(lines) > 11 else ''))
    if ALLOW_SHADER_REGRESSIONS: say(f'[RED] {msg}; going on (ALLOW_SHADER_REGRESSIONS)'); return
    raise BuildError(f'{msg} (list: {log}). ALLOW_SHADER_REGRESSIONS = True (--allow-shader-regressions) packs the app without those shaders')


def env_file_pairs(path):   # read as the launcher reads them
    out = {}
    try: text = Path(os.path.expanduser(path)).read_text(errors='replace')
    except OSError as e: raise BuildError(f'cannot read the environment file (--env-file / DM_ENV_FILE): {e}') from e
    for ln in text.splitlines():
        k, eq, v = ln.strip().partition('=')
        if k.strip() and eq and not k.startswith('#'): out[k.strip()] = v
    return out


def build_env_file(work, env_file, motion):
    if motion: return env_file
    pairs = env_file_pairs(env_file) if env_file else {}
    pairs['DM_MOTION'] = '0'
    f = work / 'build_env.txt'
    f.write_text(''.join(f'{k}={v}\n' for k, v in pairs.items()))
    return str(f)


def mac_host(py, repo, work, env, logs, g):
    PROG.stage('Mac host', step='building')
    host = build_mac_host(repo, work / 'tools/dmhost', env, logs, g)
    PROG.set(step='shared fonts')
    font, kf = work / 'font', work / 'font.key'
    src = Path(os.environ.get('STD_FONT') or '/System/Library/Fonts/Supplemental/Arial Unicode.ttf')
    st = src.stat() if src.is_file() else None
    key = fingerprint(('files', [repo / 'dm/tools/make_shared_fonts.py', repo / 'dm/tools/make_ext_font.py']), ('font', str(src), st and st.st_size, st and int(st.st_mtime)))
    if not ((font / 'korean.ttf').is_file() and reused(kf, key) is not None):
        rmtree(kf); rmtree(font); font.mkdir(parents=True, exist_ok=True)
        run([py, repo / 'dm/tools/make_shared_fonts.py', font, src], logs / 'make_shared_fonts.log', env=env)
        kf.write_text(f'{key}\n')
    PROG.end(str(host))
    return host, font


# Practice tools (the profile's practice.tsv; the game's note, "Practice tools")
PRACTICE_DIR = 'practice'   # the host reads it there with DM_PRACTICE=1
PRACTICE_MODULE = 'subsdk9'   # hle_practice.c's PRACTICE_MODULE
PRACTICE_KEYS = ('release', 'url', 'sha256', 'module', 'patches', 'license')


def practice_pin(g):
    p = g.table('practice.tsv')
    try: text = p.read_text(encoding='utf-8')
    except FileNotFoundError: return None
    pin = {}
    for ln in text.splitlines():
        if ln.strip() and not ln.lstrip().startswith('#'): k, _, v = ln.partition('\t'); pin[k.strip()] = v.strip()
    if missing := [k for k in PRACTICE_KEYS if not pin.get(k)]: raise BuildError(f'{p}: no {", ".join(missing)} row')
    if not re.fullmatch(r'[0-9a-f]{64}', pin['sha256']): raise BuildError(f'{p}: sha256 is not a SHA-256 in lowercase hex: {pin["sha256"]}')
    return pin


def file_sha256(p):
    h = hashlib.sha256()
    with open(p, 'rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def practice_zip(work, pin, env, logs):
    d = work / PRACTICE_DIR; d.mkdir(parents=True, exist_ok=True)
    z = d / pin['url'].rstrip('/').rsplit('/', 1)[-1]
    for p in d.glob('*.zip'):
        if p != z or file_sha256(p) != pin['sha256']: retire(work, p)
    if z.is_file(): return z, None
    part = z.with_name(z.name + '.partial'); rmtree(part)
    PROG.set(step=f'downloading {pin["url"]}')
    if run(['curl', '--fail', '--silent', '--show-error', '--location', '--retry', '2', '--output', part, pin['url']], logs / 'practice_download.log', env=env, fatal=False):
        rmtree(part); return None, f'the download of {pin["url"]} failed ({logs / "practice_download.log"}); with the zip put at {z} the next build takes it'
    if (got := file_sha256(part)) != pin['sha256']:
        rmtree(part); return None, f'{pin["url"]} has the SHA-256 {got}, not the profile\'s {pin["sha256"]}'
    os.replace(part, z)
    return z, None


def practice_check(host, patched, tmp, ips, work, env, logs):
    """DM_MODS_CHECK: the host checks the patch without starting the game"""
    chk = work / 'tmp/practice-check'; rmtree(chk); chk.mkdir(parents=True)
    for p in patched.iterdir():
        if p.name != PRACTICE_DIR and not p.name.startswith(PRACTICE_DIR + '.'): (chk / p.name).symlink_to(p)
    (chk / PRACTICE_DIR).symlink_to(tmp)
    verdicts, log = work / 'tmp/practice-check.tsv', logs / 'practice_check.log'
    rmtree(verdicts)
    cenv = {k: v for k, v in env.items() if k not in ('DM_WINDOW', 'DM_CTL', 'DM_KEYS', 'DM_REC')}
    cenv.update(DM_PRACTICE='1', DM_MODS=str(work / 'tmp/practice-no-mods'), DM_MODS_CHECK=str(verdicts))   # a missing folder: none of the player's mods
    rc = run([host, chk], log, env=cenv, cwd=work / 'tmp', fatal=False)
    if rc or not verdicts.is_file(): return f'the host\'s check failed (exit code {rc}, {log})'
    rows = [r for r in (l.split('\t') for l in verdicts.read_text(errors='replace').splitlines()) if r[0] == 'file' and len(r) >= 8]
    mine = [r for r in rows if r[1] == 'main' and Path(r[5]).name == ips]
    if not mine: return f'the host did not apply {ips} to main ({log})'
    if mine[0][3] != 'applied': return f'the host refused {ips} ({mine[0][3]}: {mine[0][7]})'
    # the game's own red lines are not the practice tools' (1.0.0's main references none of four SDK entries the host takes over): only those the same check without them lacks
    base_log, base_verdicts = logs / 'practice_check_base.log', work / 'tmp/practice-check-base.tsv'
    rc = run([host, chk], base_log, env=dict(cenv, DM_PRACTICE='0', DM_MODS_CHECK=str(base_verdicts)), cwd=work / 'tmp', fatal=False)
    rmtree(base_verdicts)
    if rc: return f'the host\'s check without the practice tools failed (exit code {rc}, {base_log})'
    def reds(p): return [re.sub(r'0x[0-9a-f]+', '0x?', l.strip()) for l in p.read_text(errors='replace').splitlines() if '[RED]' in l]
    known = set(reds(base_log))
    if new := [l for l in reds(log) if l not in known]: return f'the host\'s check logged {len(new)} red line(s) the game without the practice tools does not, the first: {new[0]} ({log})'
    return None


def practice_tools(py, repo, work, game, patched, host, env, logs, g, on=True):
    """(folder, None) or (None, a [RED] line), the build going on without them; (None, None) when off or not available"""
    out, kf = patched / PRACTICE_DIR, patched / f'{PRACTICE_DIR}.key'
    pin = practice_pin(g)
    def without():
        rmtree(out)
        rmtree(kf)
    if not on or pin is None:
        without()
        if not on and pin: say('practice tools: off (PRACTICE / --no-practice), not in this build')
        return None, None
    PROG.add(1); PROG.stage('practice tools', step=f'sloperinaGZ {pin["release"]}')
    def failed(why):
        without()
        red = f'[RED] practice tools: sloperinaGZ {pin["release"]} is not in this build: {why}'
        PROG.end('not in this build', warn=True)
        say(red)
        return None, red
    z, why = practice_zip(work, pin, env, logs)
    if not z: return failed(why)
    import capstone, zipfile
    bid = build_id(game / 'ExeFS/main')
    src = work / 'tmp/practice-src'; rmtree(src); src.mkdir(parents=True)
    try:
        with zipfile.ZipFile(z) as zf:
            names = set(zf.namelist())
            folder = pin['patches'].rstrip('/') + '/'
            ips = sorted(n for n in names if n.startswith(folder) and '/' not in n[len(folder):]
                         and n.lower().endswith('.ips') and Path(n).name.lower().startswith(bid))   # build_id reads 16 of the 20 bytes
            if not ips:
                without(); PROG.end(f'none: the release has no patch for main\'s build {bid}', warn=True)
                say(f'practice tools: sloperinaGZ {pin["release"]} has no patch for this version\'s main (build {bid}): this build goes without them'); return None, None
            if missing := [n for n in (pin['module'], pin['license']) if n not in names]: return failed(f'the zip has no {", ".join(missing)}')
            for n, to in ((pin['module'], PRACTICE_MODULE), (ips[0], Path(ips[0]).name), (pin['license'], 'LICENSE')): (src / to).write_bytes(zf.read(n))
    except (zipfile.BadZipFile, OSError) as e: return failed(f'{z}: {e}')
    ipsname = Path(ips[0]).name
    key = fingerprint(pin['sha256'], bid, ('files', [repo / 'dm/tools' / f for f in REWRITER_FILES] + patch_files(repo, src, [PRACTICE_MODULE], env.get('DM_GAME')) + [host]),
                      capstone.__version__, {k: env.get(k) for k in ('DM_PATCH_FRONT', 'DM_GAME')})
    note = f'sloperinaGZ {pin["release"]}, {PRACTICE_MODULE} and {ipsname}'
    if (out / f'{PRACTICE_MODULE}.fixups.tsv').is_file() and (out / ipsname).is_file() and reused(kf, key) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return out, None
    rmtree(kf)
    tmp = patched / f'{PRACTICE_DIR}.partial'; rmtree(tmp); tmp.mkdir()
    PROG.set(step=f'rewriting {PRACTICE_MODULE}')
    rc = run([py, repo / 'dm/tools/patch_nso.py', src / PRACTICE_MODULE, tmp, PRACTICE_MODULE], logs / f'rewrite_{PRACTICE_MODULE}.log', env=env, fatal=False)
    if rc: rmtree(tmp); return failed(f'patch_nso.py exit code {rc} on {PRACTICE_MODULE} ({logs / f"rewrite_{PRACTICE_MODULE}.log"})')
    for n in (ipsname, 'LICENSE'): shutil.copyfile(src / n, tmp / n)
    PROG.set(step=f'the host checks {ipsname}')
    if why := practice_check(host, patched, tmp, ipsname, work, env, logs): rmtree(tmp); return failed(why)
    swap_in(tmp, out); kf.write_text(f'{key}\n{note}\n')
    PROG.end(note)
    return out, None


def mac_run(work, game, patched, host, font, msl, aoc, lang, env, logs, g, env_file='', partial=False):
    """A host outside dm/host does not find the shader map beside itself."""
    rd = work / 'run'
    for d in ('save', 'temp'): (rd / d).mkdir(parents=True, exist_ok=True)
    renv = {'DM_ROOT': work, 'DM_ROMFS': rfs.source(game), 'DM_SAVE': rd / 'save', 'DM_TEMP': rd / 'temp', 'DM_MSL_DIR': msl, 'DM_FONT_DIR': font, 'DM_OBSOLETE': work / OBSOLETE}
    if aoc: renv['DM_AOC_DIR'] = aoc
    renv['DM_NVN_SHMAP'] = g.table('shader_map.tsv')
    if partial:
        renv['DM_PSO_ARCHIVE'] = rd / 'pso_archive_partial'
        (rd / 'pso_archive_partial/prebuilt.tsv').unlink(missing_ok=True)   # the keys may have grown: the prebuild walks the pairs again
    renv.update(DM_LANG=lang, DM_WINDOW=1, DM_AUDIO=1)
    if env_file: renv.update(env_file_pairs(env_file))
    if g.table('shader_pairs.tsv').is_file():
        if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file(): renv['DM_PSO_PAIRS'] = work / 'shader/pso_pairs.tsv'
        # docs/development.md, "Hybrid pipelines"
        PROG.add(1); PROG.stage('Pipeline prebuild', step='building')
        penv = dict(env, **{k: str(v) for k, v in renv.items()}, DM_PSO_PREBUILD='tool')
        if g.get('depth_mode', 'gl') == 'zero': penv['DM_PSO_PREBUILD_DEPTH'] = 'zero'   # the host learns the depth mode only at run time
        run([host], logs / 'pso_prebuild.log', env=penv, cwd=rd)
        PROG.end()
    q = lambda v: shlex.quote(str(v))
    return f'cd {q(rd)} && ' + ' '.join(f'{k}={q(v)}' for k, v in renv.items()) + f' {q(host)} {q(patched)}' + (f' 2>&1 | tee {q(rd / "host.log")}' if partial else '')


MTL_ARCH_M = '''#import <Metal/Metal.h>
int main(void) { @autoreleasepool { id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) return 1; printf("%s\\n", d.architecture.name.UTF8String); return 0; } }
'''


def mac_gpu_arch(work, env, logs):
    src, exe = work / 'tools/mtl_arch.m', work / 'tools/mtl_arch'
    if not (exe.is_file() and src.is_file() and src.read_text() == MTL_ARCH_M):
        src.parent.mkdir(parents=True, exist_ok=True); src.write_text(MTL_ARCH_M)
        run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', src, '-o', exe], logs / 'mtl_arch.log', env=env)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    arch = r.stdout.strip()
    if r.returncode or not re.fullmatch(r'applegpu_\w+', arch): raise BuildError(f'cannot tell this Mac\'s GPU architecture ({(r.stderr or r.stdout).strip() or r.returncode}); --gpu names it, none builds the app without a pipeline archive')
    return arch


def mac_pso(py, repo, work, lib, g, gpus, env, logs):
    if gpus == 'none': say('pipeline archive: none (--gpu none): the app builds the pipeline bases on its first start'); return None
    if gpus and not re.fullmatch(r'(applegpu_)?g\w+(,(applegpu_)?g\w+)*', gpus): raise BuildError(f'--gpu / PSO_GPUS takes GPU architectures such as g14s, or none: {gpus}')
    try: here = mac_gpu_arch(work, env, logs)
    except BuildError:
        if not gpus: raise
        here = None
    if gpus and here and here not in {x if x.startswith('applegpu_') else 'applegpu_' + x for x in gpus.split(',')}:
        say(f'pipeline archive: --gpu / PSO_GPUS ({gpus}) does not name this Mac\'s GPU ({here}): on this Mac the app builds its pipeline bases on its first start (empty = this Mac\'s GPU)')
    arch = gpus or here
    PROG.add(1); PROG.stage('Pipeline archive', step=f'{arch} (metal-tt, minutes when its inputs changed)')
    out = work / 'pso/mac'
    cmd = [py, repo / 'dm/gfx/tools/pso_offline.py', '--game', g.id, '--msl', lib, '--out', out, '--work', work / 'pso/mac-work', '--gpu', arch]
    if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file():
        cmd += ['--pairs', g.table('shader_pairs.tsv'), '--pairs', work / 'shader/pso_pairs.tsv']
    if run(cmd, logs / 'pso_offline_mac.log', env=env, fatal=False):   # optional: the app builds the bases on its first start without it
        shutil.rmtree(out, ignore_errors=True)
        PROG.end('failed, the app is packed without one', warn=True)
        say(f'[RED] pipeline archive: pso_offline.py failed (see {logs / "pso_offline_mac.log"}); the app is packed without one and builds its pipeline bases on its first start (--gpu none skips this step)')
        return None
    if not any(out.glob('*.mtl4a')):   # every eligible pair has function constants (the host specializes those on the device)
        PROG.end('no base to bundle'); say('pipeline archive: none (no shader pair, or every eligible one has function constants): the app builds what it needs on its first start'); return None
    PROG.end(f'{arch}, {amount(sum(f.stat().st_size for f in out.iterdir() if f.is_file() and "-prewarm-" not in f.name), "B")}')
    return out


def drop_prewarm(pso):
    for old in pso.glob('*-prewarm-*'): old.unlink()
    (pso / 'prewarm.tsv').unlink(missing_ok=True)


def mac_prebake(py, repo, work, msl, g, host, pso, env, logs):
    """One file per flush period: Metal's serializer fills past about 2,000 captures."""
    if pso is None: return pso
    manifest = g.table('pso_manifest.tsv')
    text = manifest.read_text() if manifest.is_file() else ''
    rows = sum(ln.startswith(('R\t', 'M\t')) for ln in text.splitlines())
    def none(why):
        say(why)
        drop_prewarm(pso)
        return pso
    if not rows: return none('prewarm pipelines: none (the profile\'s pso_manifest.tsv has no R or M rows)')
    try: arch = mac_gpu_arch(work, env, logs)
    except BuildError as e: return none(f'[RED] prewarm pipelines: {e}; the app builds them on its first start')
    if not (pso / f'{arch}.mtl4a').is_file(): return none(f'prewarm pipelines: the pipeline archive is not for this Mac\'s GPU ({arch}): the app builds them on its first start')
    PROG.add(1); PROG.stage('Prewarm pipelines', step=f'{arch}, {rows:,} rows (host, minutes to hours when its inputs changed)')
    out, stage = work / 'pso/mac-prewarm', work / 'pso/mac-prewarm-work'
    zero = g.get('depth_mode', 'gl') == 'zero'
    others = sorted(f.stem for f in pso.glob('applegpu_*.mtl4a') if '-prewarm-' not in f.name and f.stem != arch)
    lib = shader_lib.view(msl / f'b-{chain_of(repo)}')
    digest = hashlib.sha256()
    for part in (arch, chain_of(repo), 'zero' if zero else 'gl', text, Path(host).read_bytes() if Path(host).is_file() else str(host),
                 (lib / 'inputs.tsv').read_text() if (lib / 'inputs.tsv').is_file() else '',
                 (work / f'pso/mac-work/key.{arch}').read_text() if (work / f'pso/mac-work/key.{arch}').is_file() else '',
                 subprocess.run(['sysctl', '-n', 'kern.osversion'], capture_output=True, text=True).stdout):
        digest.update(part if isinstance(part, bytes) else str(part).encode()); digest.update(b'\0')
    key = digest.hexdigest()
    keyf, stage_key = work / 'pso/mac-prewarm.key', stage / 'key'
    if keyf.is_file() and keyf.read_text().split() == [key, ','.join(others) or '-'] and list(out.glob(f'{arch}-prewarm-*.mtl4a')):
        say('prewarm pipelines: reused (inputs unchanged)')
    else:
        keyf.unlink(missing_ok=True)
        if not (out.is_dir() and (out / 'key').is_file() and (out / 'key').read_text().strip() == key and list(out.glob(f'{arch}-prewarm-*.mtl4a'))):
            shutil.rmtree(out, ignore_errors=True)
            if not (stage_key.is_file() and stage_key.read_text().strip() == key): shutil.rmtree(stage, ignore_errors=True)
            stage.mkdir(parents=True, exist_ok=True); stage_key.write_text(key + '\n')
            renv = dict(env, DM_PSO_PREBUILD='tool', DM_PSO_ARCHIVE=str(stage), DM_PSO_PREBUILD_OUT=str(out), DM_PSO_BUNDLE=str(pso), DM_PSO_MANIFEST=str(manifest),
                        DM_MSL_DIR=str(msl), DM_NVN_SHMAP=str(g.table('shader_map.tsv')))
            if zero: renv['DM_PSO_PREBUILD_DEPTH'] = 'zero'
            if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file(): renv['DM_PSO_PAIRS'] = str(work / 'shader/pso_pairs.tsv')
            for k in ('DM_WINDOW', 'DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_SAVE', 'DM_PSO_USER', 'DM_PSO_PREBUILD_WARM', 'DM_NVN_METALWA'): renv.pop(k, None)   # the profile's list alone, not a player's; KK3 by this macOS, as the archive's
            if run([host], logs / 'pso_prewarm_mac.log', env=renv, fatal=False) or not list(out.glob(f'{arch}-prewarm-*.mtl4a')):
                shutil.rmtree(out, ignore_errors=True)
                PROG.end('failed, the app is packed without them', warn=True)
                say(f'[RED] prewarm pipelines: the host\'s prebuild tool failed (see {logs / "pso_prewarm_mac.log"}); the app is packed without them and builds them on its first start (--no-prebake skips this step)')
                drop_prewarm(pso)
                return pso
            (out / 'key').write_text(key + '\n')
            shutil.rmtree(stage, ignore_errors=True)
        if other_gpu_prewarm(repo, out, arch, others, logs, lib if lib.is_dir() else lib.with_name(lib.name + '.img'), env):
            keyf.write_text(f'{key} {",".join(others) or "-"}\n')   # else no key: the next build tries those GPUs again
    sys.path.insert(0, str(repo / 'dm/gfx/tools')); import pso_offline as po
    po.publish_prewarm(pso, {f.name: f for f in out.iterdir() if f.suffix in ('.mtl4a', '.keys', '.tsv')})
    PROG.end(f'{len(list(out.glob("*.mtl4a")))} files, {amount(sum(f.stat().st_size for f in out.glob("*.mtl4a")), "B")}')
    return pso


def prewarm_script_libraries(out, scripts, lib_dir, po):
    try: return po.script_libraries(scripts, lib_dir, out / 'libraries')
    except LookupError as e: raise BuildError(str(e)) from e


def other_gpu_prewarm(repo, out, arch, others, logs, lib_dir, env):
    """Returns False when a period was left out for a GPU, so the next build tries again."""
    for old in out.glob('applegpu_*-prewarm-*'):
        if not old.name.startswith(f'{arch}-'): old.unlink()
    if not others: return True
    scripts = sorted(out.glob('prewarm-*.mtl4-json'))
    if not scripts: say(f'[RED] prewarm pipelines for {", ".join(others)}: the host wrote no pipelines script; those GPUs build them on their first start'); return False
    sys.path.insert(0, str(repo / 'dm/gfx/tools')); import pso_offline as po
    osmin = po.os_min_default()
    try: prewarm_script_libraries(out, scripts, lib_dir, po)
    except (BuildError, OSError) as e: say(f'[RED] prewarm pipelines for {", ".join(others)}: {e}; those GPUs build them on their first start'); return False
    ok = True
    for gpu in others:
        made, failed = [], []
        for js in scripts:
            dst = out / f'{gpu}-{js.stem}.mtl4a'
            if run(['xcrun', '-sdk', 'macosx', 'metal-tt', '-arch', gpu, '-macos-version-min', osmin, js, '-o', dst], logs / f'pso_prewarm_{gpu}_{js.stem}.log', env=env, fatal=False):
                dst.unlink(missing_ok=True); failed.append(js.name); ok = False; continue
            made.append(dst)
            keys = out / f'{arch}-{js.stem}.keys'
            if keys.is_file():   # with its index, the file wins the lookup over a device file an earlier build left
                k = dst.with_suffix('.keys'); k.unlink(missing_ok=True); os.link(keys, k)
        if failed: say(f'[RED] prewarm pipelines for {gpu}: metal-tt failed on {", ".join(failed)} (see {logs}/pso_prewarm_{gpu}_*.log); that GPU builds those periods\' pipelines on its first start')
        if made: say(f'prewarm pipelines for {gpu}: {len(made)} of {len(scripts)} files, {amount(sum(f.stat().st_size for f in made), "B")} (metal-tt)')
    shutil.rmtree(out / 'libraries', ignore_errors=True)
    return ok


def mac_app_name(name):   # Finder shows ':' and '/' otherwise
    return re.sub(r'\s*[:/]\s*', ' - ', name).strip(' .-') or 'NativeNavi'


def mac_app(py, repo, out, app, work, game, patched, host, font, msl, aoc, lang, g, name, version, bundle_id, icon, sign, env, logs, env_file='', gpus='', control=None, prebake=False, practice=None):
    lib = msl / f'b-{chain_of(repo)}'
    pso = mac_pso(py, repo, work, lib, g, gpus, env, logs) if g.table('shader_pairs.tsv').is_file() else None
    if prebake and pso: pso = mac_prebake(py, repo, work, msl, g, host, pso, env, logs)
    elif pso: drop_prewarm(pso)
    PROG.stage('Mac app', step='starting')
    # docs/frontends.md
    settings = work / 'app/launcher.json'; settings.parent.mkdir(parents=True, exist_ok=True)
    run([py, repo / 'dm/games/settings.py', g.id, patched, control or game / 'control.nacp', settings], logs / 'mac_settings.log', env=dict(env, DM_LANG=lang))
    cmd = [py, repo / 'dm/mac/pack_app.py', out, '--host', host, '--patched', patched, '--romfs', rfs.source(game), '--msl', lib, '--tables', g.table('shader_map.tsv').parent,
           '--font', font, '--title-id', f'{app:016X}', '--name', name, '--bundle-id', bundle_id, '--lang', lang, '--sign', sign or '-']
    split_pairs = work / 'shader/pso_pairs.tsv'
    for opt, v in (('--version', version), ('--category', g.get('app_category')), ('--marker', g.get('romfs_marker')), ('--aoc', aoc), ('--icon', icon), ('--practice', practice),
                   ('--pso', pso), ('--pso-pairs', split_pairs if not g.table('pso_pairs.tsv').is_file() and split_pairs.is_file() else None),
                   ('--env-file', Path(os.path.expanduser(env_file)).resolve() if env_file else None), ('--settings', settings)):
        if v: cmd += [opt, v]
    unchanged = False
    def line(ln):
        nonlocal unchanged
        if ln.startswith('step: '): PROG.set(step=ln[6:].strip())
        if ln.startswith('app: ') and ln.endswith(', reused (inputs unchanged)'): unchanged = True
    run(cmd, logs / 'mac_app.log', env=env, on_line=line)
    if not (out / 'Contents/MacOS/dmhost').is_file(): raise BuildError(f'no app at {out}')
    PROG.end(f'{out.name}, ' + ('reused (inputs unchanged)' if unchanged else amount(tree_size(out)[1], 'B')))


def find_repo(here):
    for d in here.parents:
        if (d / 'dm/tools/patch_nso.py').exists(): return d
    raise BuildError(f'cannot find the repository root above {here} (no dm/tools/patch_nso.py)')


def parse_args(argv):
    """Options given override "Settings"; F5 in Spyder passes none"""
    import argparse
    ap = argparse.ArgumentParser(prog='build_app.py',
        description='Build The Legend of Zelda: Ocarina of Time for this Mac from its NUP: a Mac app, or a development build with the command that runs it.',
        epilog='example: python3 scripts/build_app.py ~/games/oot/"Ocarina of Time [0400486024dec000][v131072].nup"')
    ap.add_argument('nup', nargs='?', help='the game\'s NUP container (a .nup file), or the folder that holds it (default: the NUP setting, else asked)')
    ap.add_argument('-p', '--package', help='version built, e.g. v131072, or v0 for the base game (default: the newest in the NUP)')
    ap.add_argument('-o', '--out', help='--platform mac-app: the output app, a path ending in .app, or an existing folder to put <game name>.app in, such as /Applications (default: <game name>.app beside the build folder)')
    ap.add_argument('--app-name', help='the app\'s name (default: the game name in the NUP\'s control data, else the profile\'s)')
    ap.add_argument('--bundle-id', help='the app\'s bundle ID (default: org.nativenavi.<title ID in lowercase>)')
    ap.add_argument('--platform', choices=('mac', 'mac-app'), help='mac-app = a self-contained, double-clickable Mac app of the game (<game name>.app beside the build folder, -o names another); '
                    'mac = for development: the Mac shaders and the Mac host, then the command that runs the game from the build folder (default: the PLATFORM setting, mac-app)')
    ap.add_argument('--icon', help='the app icon, a JPEG or PNG of any size (default: the ICON setting, else the game\'s icon from its control data; a NUP may have none)')
    ap.add_argument('--fallback-icon', help='the app icon only when --icon / ICON is not given and the NUP carries none, a JPEG or PNG of any size: a game with its own icon '
                    'keeps it, so a front end can pass a stand-in without knowing what the NUP carries (default: the FALLBACK_ICON setting, else such an app\'s icon is blank)')
    ap.add_argument('--mac-sign', help='--platform mac-app: the codesign identity for the app, e.g. "Developer ID Application: <name> (<team>)" (default: the MAC_SIGN setting, else ad-hoc)')
    ap.add_argument('-l', '--language', help='game language, a code or a name: ja, fr, de, es, it, nl, pt, ru, ko, zh-Hans, zh-Hant, en-GB, fr-CA, es-419, pt-BR, pl, th, Japanese, ... (default: the LANGUAGE setting, else English); '
                    'it must be one the game supports, which the console lists')
    ap.add_argument('--work-dir', help='the build folder, used as it is (default: the WORK_DIR setting, else <title ID>_build in the NUP\'s folder); the unpacked files, shader cache and temporary files live there, and the Mac app defaults to the folder holding it')
    ap.add_argument('--env-file', help='environment switches for a diagnostic build (one KEY=VALUE per line)')
    ap.add_argument('--gpu', help='--platform mac-app: the GPU architectures of the app\'s pipeline archive, made on this Mac with metal-tt, comma-separated, e.g. g14s (default: the PSO_GPUS setting, else this Mac\'s GPU); '
                    'none = no archive, the app builds the pipeline bases on its first start')
    ap.add_argument('--prebake', action=argparse.BooleanOptionalAction, help='--platform mac-app: also make the pipelines of the prewarm list (pso_manifest.tsv) here on this Mac with the host\'s prebuild tool, '
                    'and bundle them with the app after the pipeline archive, so its first start builds none (about 6,400 bases, 450-475 s on an M5 Max at the first start); minutes to hours when its inputs changed and '
                    'about 1.3 GB more app, made again only when its inputs change; needs the pipeline archive for this Mac\'s GPU; the other GPUs --gpu names get theirs from the host\'s pipelines scripts through metal-tt; '
                    '--no-prebake: the app builds them on its first start (default: the PREBAKE setting, on)')
    ap.add_argument('--seen-shaders', action=argparse.BooleanOptionalAction, help='build only the shaders the profile\'s playtests used (its shader_seen.tsv) and those the last runs\' logs reported missing, not the whole map: '
                    'minutes instead of hours, but areas the list does not cover draw without the rest until this runs again (default: the SEEN_SHADERS setting, off)')
    ap.add_argument('--motion', action=argparse.BooleanOptionalAction, help='pass a gyro controller\'s motion to the game; --no-motion builds with DM_MOTION=0, for when motion misbehaves (default: the MOTION setting, on)')
    ap.add_argument('--practice', action=argparse.BooleanOptionalAction, help='put the practice tools in the build: sloperinaGZ, the release the profile\'s practice.tsv pins, downloaded once into <build folder>/practice/ and checked by its SHA-256, '
                    'off until DM_PRACTICE=1 turns them on (the app\'s settings window: Practice tools); --no-practice: a build without them, which downloads nothing (default: the PRACTICE setting, on)')
    ap.add_argument('--allow-damaged', action='store_true', help='files whose bytes do not start as their names promise (a .zs file that is no zstd stream): pack them as they are and go on instead of stopping')
    ap.add_argument('--allow-shader-regressions', action='store_true', help='shaders that translated in the previous build and fail with the current translation chain: '
                    'pack the app without them instead of stopping')
    ap.add_argument('--allow-other-metal', action='store_true', help='build the shaders with this Mac\'s Metal compiler when it is not the one they were tested with '
                    '(dm/gfx/tools/dmsh_chain.lock) instead of stopping; such shaders can render wrong pictures')
    ap.add_argument('--profile-out', help='where a derived profile goes, for a version the game\'s profile does not register (default: <build folder>/profile)')
    ap.add_argument('--progress', choices=('auto', 'bar', 'lines', 'json', 'off'), help='stage lines on the console: bar = one line per stage, redrawn in place; lines = a plain line every BEAT seconds; json = newline-delimited JSON events (no prompts); off = only each stage\'s start and end '
                    '(default: the PROGRESS setting; auto = bar in a terminal or Spyder, lines when the output goes to a file or a pipe)')
    return ap.parse_args(argv)


def icon_file(value, option):
    if not value: return None
    p = Path(os.path.expanduser(value)).resolve()
    if not p.is_file(): raise BuildError(f'no such icon file ({option}): {p}')
    return p


def app_icon(given, spare, *dirs):
    own = next((d / 'icon.jpg' for d in dirs if (d / 'icon.jpg').exists()), None)
    return given or own or spare, bool(spare and not (given or own))


def app_display_version(game, base, g, pkg_ver):
    """The host answers GetDisplayVersion from the same row; '' when none has one (the app then says 1.0)"""
    registered = re.match(r'\d+(\.\d+){0,2}$', g.builds().get(pkg_ver, {}).get('display', '') or '')
    return nacp.version(game / 'control.nacp') or (registered.group() if registered else '') or nacp.version(base / 'control.nacp')


def say_game(app, name, version, bundle_id, out, icon):
    PROG.event('game', title_id=f'{app:016X}', name=name, version=version, bundle_id=bundle_id, out=str(out) if out else None, icon=str(icon) if icon else None)
    say(f'app name: {name}; version: {version}; bundle ID: {bundle_id}' + (f'; app: {out}' if out else ''))


def report_fix_coverage(patched, mods, reg):
    """SDK-wide patches are listed without red: a game without virtual address memory needs none."""
    for m in mods:
        try: rows = [l.split('\t') for l in (patched / f'{m}.match.tsv').read_text().splitlines() if l and not l.startswith('#')]
        except OSError: continue
        known = [r[1:] for r in rows if r[0] == 'registered' and len(r) == 4]
        tried = [r[1:] for r in rows if r[0] == 'patch' and len(r) == 4]
        sdk = [r[1:] for r in rows if r[0] == 'sdk' and len(r) == 4]
        lost = '; '.join(f'{disp} ({bid[:8]}…) has {n} cuts' for bid, disp, n in known)
        if sdk:
            applied, absent = [n for n, offs, _ in sdk if offs != '-'], [n for n, offs, _ in sdk if offs == '-']
            say(f'{m}: SDK-wide patches' + (f' applied: {", ".join(map(repr, applied))}' if applied else '') + ('; ' if applied and absent else '')
                + (f' with no reviewed implementation in this build, so not applied: {", ".join(map(repr, absent))}' if absent else ''))
        if not tried:
            if known and not any(offs != '-' for _, offs, _ in sdk):
                say(f'[RED] {m}: no function patch carries the registered builds\' fixes to this build ({lost}), and the game may stop where they were needed; see {reg.dir / "README.md"}')
            continue
        missing = [(name, why) for name, offs, why in tried if offs == '-']
        if missing: say(f'[RED] {m}: function patches not applied, and the game may stop where they were needed: ' + '; '.join(f'{name!r} ({why})' for name, why in missing)
                        + (f' (registered: {lost})' if lost else ''))
        if len(missing) < len(tried): say(f'{m}: function patches applied: ' + ', '.join(repr(name) for name, offs, _ in tried if offs != '-'))


def report_profile_status(g, reg, pkg_ver, derived, artifact, patched=None):
    if not derived: return
    disp = g.builds()[pkg_ver]['display']
    known = {(m, b) for row in reg.builds().values() for m, b in row.items() if m != 'display'}
    other = [m for m, b in g.builds()[pkg_ver].items() if m != 'display' and (m, b) not in known]
    registered_builds = ", ".join(f'{v} {b["display"]}' for v, b in reg.builds().items())
    if g.builds() == reg.builds() or not other and pkg_ver in reg.builds():
        say(f'this {artifact} uses the registered {pkg_ver} ({disp}) builds and every fix for them, with derived shader tables ({derived})')
    else: say(f'[RED] this {artifact} uses {pkg_ver} ({disp}), which game profile {reg.id} was not brought up on ({registered_builds}): '
        'its settings, with this version\'s own shaders' + (f'; {", ".join(other)} {"is a build" if len(other) == 1 else "are builds"} the profile does not register, so the fixes made for '
        'the registered builds\' fixed addresses do not apply there; opt-in function patches apply only on a verified code match (see rewrite logs), and the game may not run as well' if other else '; every module is a registered build'))
    if other and patched: report_fix_coverage(patched, other, reg)


def main(args=None):
    args = args or parse_args([])
    PROG.mode = progress_mode(args.progress or PROGRESS)
    global ALLOW_DAMAGED; ALLOW_DAMAGED = args.allow_damaged or ALLOW_DAMAGED
    global ALLOW_SHADER_REGRESSIONS; ALLOW_SHADER_REGRESSIONS = args.allow_shader_regressions or ALLOW_SHADER_REGRESSIONS
    global ALLOW_OTHER_METAL; ALLOW_OTHER_METAL = args.allow_other_metal or ALLOW_OTHER_METAL
    here = Path(globals().get('__file__') or sys.argv[0]).resolve()
    if sys.version_info < (3, 12): raise BuildError('Direct RomFS image builds require Python 3.12 or newer')
    repo = find_repo(here)
    platform = args.platform or PLATFORM
    if platform not in ('mac', 'mac-app'): raise BuildError(f'PLATFORM / --platform must be mac or mac-app: {platform}')
    mac_pack = platform == 'mac-app'
    seen_shaders = SEEN_SHADERS if args.seen_shaders is None else args.seen_shaders
    if args.mac_sign and not mac_pack: raise BuildError('--mac-sign signs the Mac app: it needs --platform mac-app')
    if args.prebake and not mac_pack: raise BuildError('--prebake makes the Mac app\'s prewarm pipelines: it needs --platform mac-app')
    if not apple_silicon(): raise BuildError('the game runs in the Mac host, which runs the game\'s own arm64 code natively: it needs a Mac with Apple silicon, and this Mac has an Intel processor')
    check_native_python()
    given = Path(os.path.expanduser(args.nup or NUP or ask('NUP file, or the folder that holds it: ', 'the NUP'))).resolve()
    if not given.exists(): raise BuildError(f'no such file or folder: {given}')
    nup = nup_file(given)
    nup_dir = nup.parent
    global NUP_FOLDER; NUP_FOLDER = nup_dir
    user_icon = icon_file(args.icon or ICON, '--icon / ICON'); spare_icon = icon_file(args.fallback_icon or FALLBACK_ICON, '--fallback-icon / FALLBACK_ICON')
    sys.path.insert(0, str(repo / 'dm/games')); import game as gamemod
    sys.path.insert(0, str(repo / 'dm/tools')); global nacp; import nacp
    lang_in = args.language or LANGUAGE   # checked against the game's languages once its control data is unpacked
    want_lang = language_code(lang_in) if lang_in else 'en-US'
    if not want_lang: raise BuildError(f'unknown game language {lang_in!r}; give one of {language_list([c for c, _ in nacp.LANGUAGES])}, by code or by name')
    env = tool_env()
    env.setdefault('J', str(os.cpu_count() or 8))
    py = sys.executable
    app, fam = scan_nup(nup)
    say(f'a NUP: {nup.name}, {app:016X}, {len(fam["updates"])} update(s), {len(fam["dlc"])} DLC')
    if fam['base'] is None: say(pack_nup.NO_BASE)
    own = own_profile_folders(args, nup_dir, app)
    g = gamemod.by_title(app, own)
    if g is None: raise BuildError(f'no game profile for {app:016X}: this repository builds The Legend of Zelda: Ocarina of Time ({gamemod.DEFAULT}) only')
    say(f'game profile: {g.id}')
    if seen_shaders and not g.table('shader_seen.tsv').is_file(): raise BuildError(f'--seen-shaders: game profile {g.id} has no list of the shaders its playtests used ({g.table("shader_seen.tsv")}): build the whole map, without this option')
    need, have = g.words('shader_versions'), nup_versions(fam)
    pkg_ver = args.package or PACKAGE or max(have, key=vnum)
    if pkg_ver not in have: raise BuildError(f'the version to build (-p / PACKAGE) is not in the NUP, which holds {", ".join(sorted(have, key=vnum))}')
    derived = derived_reason(g, have, pkg_ver)
    if derived: say(f'{derived}: building {pkg_ver} from a profile derived from it')
    versions = [pkg_ver] if derived else need + ([pkg_ver] if pkg_ver not in need else [])
    work = build_folder(args, nup_dir, g.id); BUILD_FOLDERS.add(work)
    out_app = Path(os.path.expanduser(args.out or OUT_APP)).resolve() if mac_pack and (args.out or OUT_APP) else None
    out_dir = out_app if out_app and out_app.suffix != '.app' and out_app.is_dir() else None   # a front end knows no name before the unpack
    if out_dir: out_app = None
    if out_app and out_app.suffix != '.app': raise BuildError(f'the Mac app (-o / OUT_APP) must end in .app, or be a folder that exists: {out_app}')
    say('Mac app: the Mac shaders and the Mac host, packed into a Mac app' if mac_pack else 'development build: the Mac shaders and the Mac host, then the command that runs the game')
    say(f'build folder (the unpacked game files in extract/, kept for the next build): {work}')
    xlate = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'))
    logs = work / 'logs'
    check_prereqs(env, xlate, repo / RUST_DIR)
    picks = pick_packages(fam, have, versions)

    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    gb = sum(pack_nup.romfs_bytes(fam['nup'], u or b, b) for b, u in picks.values()) / 2**30 + 6   # plus 6 GB for the shader cache and slack
    if free_gb(work) < gb and not all((ex / v).exists() for v in versions): raise BuildError(f'less than {gb:.0f} GB free on the volume of {work}')
    PROG.plan(len(versions) + bool(fam['dlc']) + 4 + mac_pack)
    dirs = {}
    for ver in versions:
        b, u = picks[ver]
        dirs[ver] = extract_nup(ex, ver, u or b, b, app, fam['nup'])
    check_formats(dirs)
    bad = [] if derived else [f'{v}: {x}' for v in versions for x in build_mismatch(dirs[v], v, g)]
    if bad:   # other builds under a registered version's tag: built like any other version
        derived = f'not the builds game profile {g.id} registers: {"; ".join(bad)}'
        say(f'{derived}: building {pkg_ver} from a profile derived from it')
        versions, dirs = [pkg_ver], {pkg_ver: dirs[pkg_ver]}
    reg = g
    pdir = profile_folder(args, work)
    if derived:
        disp = derive_profile(g, pdir / g.id, pkg_ver, dirs[pkg_ver], g.words('modules'))
        profiles_first(env, pdir)
        g = gamemod.load(g.id)
        say(f'derived game profile for {pkg_ver} ({disp}): {g.dir}')
    sv = versions if derived else need
    game = dirs[pkg_ver]
    check_metal(py, repo, env)   # before the shader split, which takes minutes
    aoc = extract_aoc(work, app, fam)
    dlc_roms = sorted(p for p in aoc.iterdir() if p.is_dir() or p.suffix == '.img') if aoc else []
    base = next((dirs[v] for v in versions if picks[v][1] is None), dirs[versions[0]])
    app_name = args.app_name or APP_NAME or nacp.name(base / 'control.nacp') or g.get('name', '')
    if not app_name and mac_pack: raise BuildError(f'no game name in the NUP\'s control data ({base / "control.nacp"}) or the profile: give --app-name')
    bundle_id = args.bundle_id or BUNDLE_ID or f'org.nativenavi.{app:016x}'
    app_version = app_display_version(game, base, g, pkg_ver)
    icon, stand_in = app_icon(user_icon, spare_icon, game, base)
    if mac_pack:
        out_app = out_app or (out_dir or work.parent) / f'{mac_app_name(app_name)}.app'
        if stand_in: say(f'no game icon: the app takes the fallback icon {spare_icon} (--fallback-icon / FALLBACK_ICON)')
        say_game(app, app_name, app_version or '1.0', bundle_id, out_app, icon)
    # Horizon gives the game a language its control data lists, which an update may extend
    control = languages_control(game, base)
    langs = nacp.languages(control)
    lang = game_language(want_lang, bool(lang_in), langs)
    say(f'game language: {lang} ({dict(nacp.LANGUAGES)[lang]})' + (f', the game has no {want_lang}' if lang != want_lang else '')
        + (f'; the game\'s languages: {" ".join(langs)}' if langs else '; the control data lists no languages (not checked)') + ' (DM_LANG=<code> in the run command changes it)')

    patched = work / f'patched_{pkg_ver}'
    rewrite(py, repo, game / 'ExeFS', patched, env, logs, g)
    chain_header(py, repo, env, logs)
    if derived: empty_tables(g)
    sh = work / 'shader'
    def derive_shader_tables():   # called only once dlc_only_shader_change passed
        nonlocal g, derived
        assert g is not None
        derived = 'the installed DLC shader content differs from the registered tables'
        derive_profile(g, pdir / g.id, pkg_ver, game, g.words('modules'), shader_only=True)
        profiles_first(env, pdir)
        g = gamemod.load(g.id)
        say(f'{derived}: using a shader-only derived profile at {g.dir}; registered module fixes are preserved')
        return g
    pkey = split_shaders(py, repo, [rfs.source(dirs[v]) for v in sv], [dirs[v] / 'ExeFS/main' for v in sv], sh / 'pkg', env, logs, g,
                         bool(derived), dlc_roms, None if derived else derive_shader_tables)
    if derived and g.dir != reg.dir and reg.table('pso_manifest.tsv').is_file():   # another version's list names its shaders by their keys there
        note = pso_carry.carry_file(str(reg.table('shader_map.tsv')), str(g.table('shader_map.tsv')), str(reg.table('pso_manifest.tsv')), str(g.table('pso_manifest.tsv')),
                                    [str(g.table('shader_pairs.tsv')), str(g.table('pso_pairs.tsv'))], f'game profile {reg.id}\'s list for {", ".join(reg.words("shader_versions"))}',
                                    programs=str(reg.table('shader_programs.tsv')), romfs=str(rfs.source(game)), warn=say)
        say(f'prewarm list: {note}')
    seen = seen_keys(g, sh, seen_logs(work, app)) if seen_shaders else None
    msl = build_shaders(py, repo, sh, xlate, env, logs, g, pkey, seen)
    motion = MOTION if args.motion is None else args.motion
    env_file = build_env_file(work, args.env_file or DM_ENV_FILE, motion)
    if not motion: say('motion (gyro): off, DM_MOTION=0 is built in')
    host, font = mac_host(py, repo, work, env, logs, g)
    practice, practice_red = practice_tools(py, repo, work, game, patched, host, env, logs, g, PRACTICE if args.practice is None else args.practice)
    if mac_pack:
        if not icon: say('no game icon: the NUP carries none; the app\'s icon will be blank; --icon / ICON or --fallback-icon / FALLBACK_ICON gives it one')
        mac_app(py, repo, out_app, app, work, game, patched, host, font, msl, aoc, lang, g, app_name, app_version, bundle_id, icon, args.mac_sign or MAC_SIGN, env, logs, env_file,
                (args.gpu or PSO_GPUS).strip(), control, PREBAKE if args.prebake is None else args.prebake, practice)
        rmtree(work / 'tmp')
        say(f'done: {out_app} ({tree_size(out_app)[1] / 2**30:.1f} GB); '
            f'open it from Finder. Its save is in ~/Library/Application Support/NativeNavi/{app:016X}/, its log in ~/Library/Logs/NativeNavi/{app:016X}.log')
        if practice: say('practice tools: in the app, off until Practice tools in its settings window turns them on')
        if practice_red: say(practice_red)
        if seen_shaders: say('a partial shader library: where a shader is missing, the log says so in red and the draw goes without it. Run this script again with --seen-shaders to add what the log names, or without it for the whole map')
        report_profile_status(g, reg, pkg_ver, derived, 'Mac app', patched)
        return
    cmd = mac_run(work, game, patched, host, font, msl, aoc, lang, env, logs, g, env_file, partial=seen_shaders)
    rmtree(work / 'tmp')
    say(f'done: the game runs on the Mac with this command (its save in {work / "run/save"}, kept from run to run; DM_WINDOW=1 opens a window, DM_AUDIO=1 plays sound'
        + ('; DM_PRACTICE=1 turns the practice tools on' if practice else '') + '):')
    PROG.say(cmd)
    if practice_red: say(practice_red)
    if seen_shaders: say(f'a partial shader library: where a shader is missing, the log says so in red and the draw goes without it. Run this script again with --seen-shaders to add what {work / "run/host.log"} names, or without it for the whole map')
    report_profile_status(g, reg, pkg_ver, derived, 'Mac build', patched)


class Stopped(BaseException):   # not an Exception, so no `except Exception` keeps it from cli
    def __init__(self, sig=signal.SIGTERM):
        super().__init__(sig)
        self.sig = sig


def _sigterm(sig, frame): raise Stopped(sig)


STOPS = (signal.SIGTERM, signal.SIGHUP)


NUP_FOLDER = None


def cli(argv):
    # Spyder runs the next F5 in this process, where the derived profile on DM_GAMES_DIR would be found first: the environment is put back
    environ = dict(os.environ)
    # SIGTERM (a front end's cancel) or SIGHUP (the tool's process group is its own session, so the shell's signal misses it) ends the build as an interrupt does.
    # Python sets handlers in the main thread only; Spyder's kernel gets its own back
    main_thread = threading.current_thread() is threading.main_thread()
    saved = {s: signal.signal(s, _sigterm) for s in STOPS} if main_thread else {}
    global BUILD_STARTED
    BUILD_STARTED = obsolete_stamp(); BUILD_FOLDERS.clear()
    try:
        main(parse_args(argv))
        for w in sorted(BUILD_FOLDERS): empty_obsolete(w)
        if NUP_FOLDER:
            for w in NUP_FOLDER.glob('*-work'):
                if re.fullmatch(r'[0-9A-Fa-f]{16}-work', w.name): tree_tar.pack_quiet(w, say)
    except (BuildError, nso.Error, pack_nup.Error) as e:
        PROG.close(ok=False); PROG.say(f'[RED] {e}'); return 1
    except KeyboardInterrupt:
        PROG.close(ok=False); PROG.say('[RED] interrupted'); return 130
    except Stopped as e:
        PROG.close(ok=False); PROG.say(f'[RED] stopped ({signal.Signals(e.sig).name})'); return 128 + int(e.sig)
    except Exception as e:   # end the stage line before the traceback
        PROG.close(ok=False)
        if PROG.mode == 'json': PROG.say(f'[RED] {type(e).__name__}: {e}')
        raise
    finally:
        for s, h in saved.items(): signal.signal(s, signal.SIG_DFL if h is None else h)   # first: a signal during this cleanup is the caller's again (None: not set from Python)
        PROG.close(ok=False)
        for k in os.environ.keys() - environ.keys(): del os.environ[k]
        os.environ.update(environ)
    return 0


if __name__ == '__main__':
    code = cli(sys.argv[1:])
    if not in_ipython(): sys.exit(code)
