#!/usr/bin/env python3
# The Legend of Zelda: Ocarina of Time (game profile dm/games/0400486024DEC000) from its NUP to a build that runs on this Mac: unpack the NUP (scripts/pack_nup.py: the code modules, one RomFS
#   image per version, the DLC) → rewrite the code modules (patch_nso.py) → split out the shaders and check them against the profile's tables (shader_split.py) → the Mac shader libraries
#   (dmsh_build.py) → the Mac host and its fonts → the pipeline archive and the prewarm pipelines → the Mac app (dm/mac/pack_app.py).
# PLATFORM = 'mac-app' / --platform mac-app (the default): a self-contained, double-clickable Mac app of the game beside the build folder, <game name>.app (OUT_APP / -o names another path):
#   the launcher dm/mac/launcher.c, the Mac host, the RomFS (APFS clones on the build's volume), the Mac shader libraries, the DLC and the rewritten modules inside it; the save in
#   ~/Library/Application Support/nxnative/<title ID>/, the log in ~/Library/Logs/nxnative/<title ID>.log. Signed ad-hoc, or with MAC_SIGN / --mac-sign; no hardened runtime, no notarization.
# PLATFORM = 'mac' / --platform mac is the development build: the same steps up to the Mac host, then the pipeline prebuild; no app. Its last line is the command that runs the game in the
#   Mac host from the build folder, its save in <build folder>/run/save.
# SEEN_SHADERS / --seen-shaders: only the shaders the profile's shader_seen.tsv lists and the last runs' logs reported missing, in minutes instead of hours; the rest do not draw (build_shaders).
# Two ways to run it: open this file in Spyder and press F5 (it uses the values under "Settings" below; an empty NUP is asked for at run time);
#   or in a terminal: python3 scripts/build_app.py <NUP file or its folder> [options] (values given on the command line override "Settings", -h lists every option;
#   without keyboard input a missing value is not asked for but reported as an error; exit code 0 on success, 1 on error, 130 when interrupted, 143 when stopped by SIGTERM, 129 when the terminal closed (SIGHUP)).
#   Command-line options entered in Spyder's run configuration work the same way.
# Uses only two things: this repository and the game as one NUP container (a .nup file: base game, updates and DLC; scripts/pack_nup.py), given as the file or as the folder that holds it.
#   A NUP's files are not encrypted, so no keys are needed. The name, display version and icon come from the NACP and icon it carries, else the name from the profile (--icon / ICON gives the
#   app an icon when the NUP carries none; --fallback-icon / FALLBACK_ICON only then).
#   The newest version in the NUP is built. The version the profile registers (with every version in its shader_versions in the NUP, and the registered build IDs) is built from the profile as
#   it is; any other version (a later update of the game) from a profile derived from it in <build folder>/profile: the same settings, and the version's own shaders (derive_profile).
# Every intermediate file is under the build folder, <title ID>_build/ in the NUP's folder unless --work-dir / WORK_DIR names another: extract/ (the unpacked game files, kept: the game runs from
#   them and the app is made of clones of them), aoc/ (one RomFS image per DLC index), tools/ (the Mac host), patched_*/, shader/ (shader package, libraries and caches), pso/ (the pipeline
#   archive), profile/ (a derived profile), logs/ (each step's full output), tmp/, obsolete/ (what a build no longer uses: emptied of earlier builds' entries after the next successful build).
# The rewrite, the shader split, the shaders, the host and the app are reused when all their inputs are as at their last successful run (the game files, the tools, the profile, Xcode and the
#   in-repo spirv-cross: fingerprint() keys beside their output); the shaders' inputs are the shader package's content and the chain's own tools, so a split that runs again and writes the
#   same package leaves them reused.
#   The chain version header dm/gfx/nvn/dmsh_chain.h is written again when a chain change was committed without it (chain_header: it follows from the source alone), so a run may leave it
#   changed. The c1 / uniform extent tables (c1_full.tsv, cb_extent.tsv) are not in the repository: the shader step's dmsh_build.py makes them in the profile folder from this build's
#   shaders, where git ignores them, and a reused step makes them again when they do not name its libraries' stamp (chain_tables).
# The shader translator is the in-repo dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate, a native executable (its fingerprint must match dmsh_chain.lock: a rebuild has another one and dmsh_build
#   refuses it). SPIRV-Cross is the in-repo dm/gfx/tools/spirv_cross_bin and must match the lock too.
# The shader libraries are built through dm/gfx/tools/own_compilers.py. The Metal compiler must match the lock (Xcode 27.2): another one stops the build before its shader steps, unless
#   ALLOW_OTHER_METAL / --allow-other-metal.
# Needs a Mac with Apple silicon (the host runs the game's own arm64 code natively), Xcode with the Metal toolchain, and Python 3.12+ with lz4, capstone 5.x, fonttools and zbic
#   (and backports.zstd before Python 3.14).
# The console shows one line per stage ([k/n] stage, with a bar, done/total, percent, elapsed time and ETA where the work can be counted); the tools' own output goes only to logs/
#   (PROGRESS / --progress). --progress json writes newline-delimited JSON events to stdout.
import ast, collections, contextlib, errno, hashlib, json, os, re, shlex, shutil, signal, subprocess, sys, tempfile, threading, time, types
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "dm/tools"))
import romfs as rfs
import proc_group   # a tool's run in a process group of its own, stopped whole
import nso   # the code module loader: nso.Error is a damaged module (its file cut short or not what its header says)
import nso_syms   # a module's dynamic symbols (main_modules)
import tree_tar   # one file in place of a build's folders of many small files
import shader_lib   # the shader libraries as one file, read in place
import shader_cache   # the compile cache as one file, read in place
import vol_space   # the space a build may write, with what APFS frees on demand
sys.path.insert(0, str(Path(__file__).resolve().parent))
import pack_nup   # the game's NUP container

# ---- Settings ----
NUP = ''                                # the game's .nup file, or the folder that holds it (empty = ask at run time)
PACKAGE = ''                            # version built (empty = the newest in the NUP; 'v131072', or 'v0' for the base game)
WORK_DIR = ''                           # the build folder (empty = <title ID>_build in the NUP's folder); the unpacked files, shader cache and temporary files live there, and the app goes beside it unless OUT_APP names it
OUT_APP = ''                            # PLATFORM 'mac-app': the output app (empty = <game name>.app beside the build folder)
APP_NAME = ''                           # the app's name (empty = the game name in the NUP's control data, else the profile's)
BUNDLE_ID = ''                          # the app's bundle ID (empty = org.nxnative.<title ID in lowercase>)
ICON = ''                               # the app icon, a JPEG or PNG of any size (empty = the game's icon from its control data; a NUP may have none)
FALLBACK_ICON = ''                      # the app icon only when ICON is empty and the NUP carries none; empty = such an app's icon is blank
PLATFORM = 'mac-app'                    # 'mac-app' = a double-clickable Mac app of the game; 'mac' = development: the Mac shaders and the Mac host, then the command that runs the game from the build folder
MAC_SIGN = ''                           # PLATFORM 'mac-app': the codesign identity for the app, e.g. 'Developer ID Application: <name> (<team>)' (empty = ad-hoc); never with the hardened runtime
LANGUAGE = ''                           # game language, a code or a name: 'ja', 'fr', 'de', 'es', 'zh-Hans', 'zh-Hant', 'ko', 'Japanese', ... (empty = English); it must be one the game supports (the console line lists them)
DM_ENV_FILE = ''                        # optional: environment switches for a diagnostic build (one KEY=VALUE per line)
SEEN_SHADERS = False                    # build only the shaders the profile's playtests used (shader_seen.tsv) and those the last runs' logs reported missing, not the whole map; areas the list does not cover draw without the rest until this runs again
PREBAKE = True                          # PLATFORM 'mac-app': also make the prewarm list's pipelines here, on this Mac (the host's prebuild tool, DM_PSO_PREBUILD=tool), and bundle them with the app after the pipeline archive,
                                        #   so the app's first start builds none (40,000-line list: about 6,400 bases, 450-475 s on an M5 Max); minutes to hours when its inputs changed, about 1.3 GB more app; False = the app builds them on its first start
PSO_GPUS = ''                           # PLATFORM 'mac-app': the GPU architectures of the app's pipeline archive (Resources/pso), made on this Mac (metal-tt, comma-separated: 'g14s' for an M2 Max);
                                        #   empty = this Mac's GPU, 'none' = no archive: the app builds its pipeline bases on its first start
MOTION = True                           # True passes a gyro controller's motion to the game; False builds with DM_MOTION=0, for when motion misbehaves
PROFILE_OUT = ''                        # where a derived profile goes (empty = <build folder>/profile; the profile is <this>/<title ID>/)
ALLOW_DAMAGED = False                   # files whose bytes do not start as their names promise (a .zs that is no zstd stream: a NUP has no hashes) stop the build; True packs those files as they are and goes on
ALLOW_SHADER_REGRESSIONS = False        # shaders that translated in the previous build and fail with the current translation chain stop the build (logs/shader_regressions.log lists them);
                                        #   True packs the app without them
ALLOW_OTHER_METAL = False               # the shaders are built only with the Metal compiler they were tested with (dm/gfx/tools/dmsh_chain.lock); another one stops the build,
                                        #   as its shaders can render wrong pictures. True builds them with this Mac's Metal compiler anyway

PROGRESS = 'auto'                       # stage lines on the console: 'bar' (one line per stage, redrawn in place), 'lines' (a plain line every BEAT seconds), 'off' (only each stage's start and end); 'auto' = bar in a terminal or Spyder, lines when the output goes to a file; 'json' = one JSON event per line, for a program that shows the build
BEAT = 30                               # seconds between progress lines in 'lines' mode
TOOL_PATHS = ['/opt/homebrew/bin', '/usr/local/bin']   # searched after the caller's PATH (tool_env): GUI apps such as Spyder do not get the shell's PATH


class BuildError(Exception):
    pass


def clock():
    return time.strftime('%H:%M:%S')


def say(msg):
    PROG.say(f'[{clock()}] {msg}')


def hms(s):   # seconds → m:ss, or h:mm:ss from an hour on
    s = int(s); h, m = divmod(s // 60, 60)
    return f'{h}:{m:02d}:{s % 60:02d}' if h else f'{m}:{s % 60:02d}'


def size_unit(v):   # (unit, divisor) for a byte count
    return next(((u, d) for u, d in (('GB', 2**30), ('MB', 2**20)) if v >= d), ('KB', 2**10))


def amount(v, unit=''):   # a count, or a byte size when unit is 'B'
    if unit == 'B':
        u, d = size_unit(v); return f'{v / d:.1f} {u}'
    return f'{v:,}' + (f' {unit}' if unit else '')


# ---- Progress on the console ----
class Progress:
    """The console view of the build: one line per stage, "[k/n] stage · step", with a bar, done/total, percent, elapsed time and ETA when the work can be counted, else a spinner and the elapsed time.
    Modes: bar = the line is redrawn in place (a terminal, Spyder's console) and replaced by a ✓ line when the stage ends; lines = a plain line when a stage or step starts, every BEAT seconds and at the end
    (for output piped to a file); off = only each stage's start and end lines. Counters come from set(), or from a poll function that runs in a thread of its own (it may walk a big folder) and returns set()'s arguments.
    Messages go through say(), which prints them above the live line. json (for a program that shows the build) = one JSON object per stdout line instead: the events stage, step, progress, stage_end, say, shaders_stale and game;
    a count change goes out at most every JSON_INTERVAL seconds (the ticker sends one held back), any other change at once"""
    SPIN, BLOCKS, POLL, JSON_INTERVAL = '⠋⠙⠹⠸⠼⠴⠦⠧⠇⠏', ' ▏▎▍▌▋▊▉', 1.0, 0.5

    def __init__(self, mode='off'):
        self.mode, self.lock, self.k, self.n, self.cur, self.shown, self.ticker, self.tstop = mode, threading.RLock(), 0, 0, None, 0, None, None

    def plan(self, n):   # the next n stages are numbered 1..n
        with self.lock: self.k, self.n = 0, n

    def add(self, n):   # n more stages than planned (one that turns out to be needed: the check of an image's files, the pipeline archive)
        with self.lock: self.n += n

    def stage(self, name, poll=None, step='', **kw):
        """Start the next stage, ending the current one. kw: set()'s arguments"""
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
        """Call poll() over and over until the stage ends or watch() is called again (None: stop); poll returns set()'s arguments, or None to leave the line as it is"""
        with self.lock:
            st = self.cur
            if not st: return
            if st.pstop: st.pstop.set()
            st.pstop = stop = threading.Event()
        if poll: threading.Thread(target=self._poll, args=(st, poll, stop), daemon=True).start()

    def set(self, done=None, total=None, step=None, unit=None, est=None, note=None):
        """done / total: the count of the current step (unit 'B' = bytes); est: total is an estimate (shown with ~); note: shown after the count;
        step: the step within the stage (a new step starts with no count and restarts the ETA)"""
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
                while len(st.hist) > 2 and st.hist[1][0] < now - 60: st.hist.popleft()   # the ETA follows the rate of the last minute
            if self.mode == 'json': self._json_progress(st)

    def _json_progress(self, st, force=False):   # a progress event when anything changed; a count change alone waits JSON_INTERVAL unless forced (the caller holds the lock)
        state = (st.step, st.total, st.unit, st.est, st.note, st.done)
        if state == st.jstate: return
        now = time.monotonic()
        if not force and st.jstate is not None and state[:-1] == st.jstate[:-1] and now - st.jlast < self.JSON_INTERVAL: return
        st.jlast, st.jstate = now, state
        self._json({'ev': 'progress', 'k': self.k, 'name': st.name, 'step': st.step, 'done': st.done, 'total': st.total,
                    'unit': st.unit, 'est': st.est, 'note': st.note,
                    'frac': min(st.done / st.total, 0.99) if st.total and st.done is not None else None,
                    'eta': self._eta(st), 'elapsed': round(time.time() - st.t0, 3)})

    def _json(self, obj):   # one JSON object per stdout line (a message's newlines are escaped)
        obj.setdefault('t', round(time.time(), 3))
        print(json.dumps(obj, ensure_ascii=False, allow_nan=False), flush=True)

    def end(self, summary='', ok=True, warn=False):
        """End the current stage with a ✓ (or ✗) line: its elapsed time and a summary. warn: it finished without what it makes (a ⚠ line; the JSON event has ok true and warn true)"""
        with self.lock:
            st, self.cur = self.cur, None
            if not st: return
            if st.pstop: st.pstop.set()
            if self.mode == 'json':
                self._json_progress(st, force=True)   # the last count, if it was held back
                self._json({'ev': 'stage_end', 'k': self.k, 'name': st.name, 'ok': ok, 'summary': summary, 'elapsed': round(time.time() - st.t0, 3), **({'warn': True} if warn else {})})
                return
            self._out(f'[{clock()}] {"⚠" if warn else "✓" if ok else "✗"} [{self.k}/{self.n}] {st.name} ({hms(time.time() - st.t0)})' + (f': {summary}' if summary else ''))

    def event(self, ev, **fields):   # a build event beside the stage lines (json mode only: the human modes get the message that goes with it from say())
        with self.lock:
            if self.mode == 'json': self._json({'ev': ev, **fields})

    def say(self, msg):
        with self.lock:
            if self.mode == 'json': self._json({'ev': 'say', 'msg': msg})
            else: self._out(msg)

    def close(self, ok=True):   # end the current stage and stop the ticker (the mode stays: a message after it is still JSON in json mode)
        with self.lock:
            self.end(ok=ok)
            if self.tstop: self.tstop.set()
            self.ticker = self.tstop = None

    def _count(self, st):   # "1.9/~5.0 GB", "1,203/19,592 shaders", "2,301 files" (no total); '' without a count
        if st.done is None: return ''
        if not st.total: return amount(st.done, st.unit)
        t = '~' if st.est else ''
        if st.unit == 'B':
            u, d = size_unit(st.total); return f'{st.done / d:.1f}/{t}{st.total / d:.1f} {u}'
        return f'{st.done:,}/{t}{st.total:,}' + (f' {st.unit}' if st.unit else '')

    def _eta(self, st):   # seconds left at the rate of the last minute; None while unknown
        if not st.total or len(st.hist) < 2: return None
        (t1, d1), (t2, d2) = st.hist[0], st.hist[-1]
        if t2 - t1 < 3 or d2 <= d1: return None
        return max(st.total - st.done, 0) * (t2 - t1) / (d2 - d1)

    def _fields(self, st):   # (head, fraction done or None, count, note, times)
        head = f'[{self.k}/{self.n}] {st.name}' + (f' · {st.step}' if st.step else '')
        frac = min(st.done / st.total, 0.99) if st.total and st.done is not None else None   # 100 % only when the stage ends
        eta = self._eta(st)
        return head, frac, self._count(st), st.note, hms(time.time() - st.t0) + (f' · ETA {hms(eta)}' if eta is not None else '')

    def _line(self, st, width):
        head, frac, count, note, times = self._fields(st)
        head = f'{self.SPIN[int(time.time() * 8) % len(self.SPIN)]} {head}'
        pct = f'{frac * 100:.0f}%' if frac is not None else ''
        for shown in (note, ''):   # a narrow console loses the note first, then the bar
            rest = '  '.join(x for x in (' · '.join(x for x in (count, shown) if x), times) if x)
            barw = min(20, width - len(f'{head}  {pct:>4}  {rest}') - 3)   # the bar takes what the console's width leaves, up to 20 cells; none below 8
            if frac is None or barw >= 8: break
        if frac is not None and barw >= 8:
            cells = frac * barw; full = int(cells); part = int((cells - full) * 8)
            pct = ('█' * full + (self.BLOCKS[part] if part else '')).ljust(barw, '░') + f' {pct:>4}'
        return '  '.join(x for x in (head, pct, rest) if x)[:width - 1]

    def _out(self, text):   # a whole line above the live one (the caller holds the lock)
        if self.shown: sys.stdout.write('\r' + ' ' * self.shown + '\r'); self.shown = 0
        print(text, flush=True)
        if self.mode == 'bar' and self.cur: self._draw()

    def _draw(self):
        line = self._line(self.cur, shutil.get_terminal_size((80, 24)).columns)   # Spyder has no size to ask, and a line that wraps there cannot be redrawn: 80 columns
        sys.stdout.write('\r' + line + ' ' * max(self.shown - len(line), 0)); sys.stdout.flush()
        self.shown = len(line)

    def _tick(self, stop):
        while not stop.wait(0.25 if self.mode == 'bar' and not in_ipython() else 1):   # Spyder's console gets fewer redraws
            with self.lock:
                if stop.is_set(): return   # close() stopped this ticker while it waited for the lock
                st = self.cur
                if not st: continue
                try:
                    if self.mode == 'bar': self._draw()
                    elif self.mode == 'json': self._json_progress(st)   # a count that set() held back
                    elif self.mode == 'lines' and time.time() - st.beat >= BEAT:
                        st.beat = time.time(); head, frac, count, note, times = self._fields(st)
                        self._out(f'[{clock()}] {head}: ' + ', '.join(x for x in (f'{frac * 100:.0f}%' if frac is not None else '', count, note, times) if x))
                except Exception: pass   # progress never stops the build, and one bad line does not stop the next

    def _poll(self, st, poll, stop):
        while not stop.is_set():
            t, step = time.time(), st.step
            try: r = poll()
            except Exception: r = None   # progress never stops the build
            with self.lock:   # a count read for a step that ended meanwhile is dropped
                if r and self.cur is st and not stop.is_set() and ('step' in r or st.step == step): self.set(**r)
            stop.wait(max(self.POLL, (time.time() - t) * 5))   # a slow poll (a walk over tens of thousands of files) runs less often


PROG = Progress()


def progress_mode(want):   # auto: redraw in a terminal or in Spyder (IPython handles \r), plain lines when the output goes to a file or a pipe
    if want in ('bar', 'lines', 'off', 'json'): return want
    if want != 'auto': raise BuildError(f'PROGRESS / --progress must be auto, bar, lines, json or off: {want}')
    return 'bar' if in_ipython() or (sys.stdout and sys.stdout.isatty()) else 'lines'


def tree_size(d):   # (files, bytes) under d; (0, 0) when it does not exist yet
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


def in_ipython():   # Spyder (and Jupyter) run this file inside IPython: input works there, and sys.exit must not be called (it piles an error onto the console)
    import builtins
    return hasattr(builtins, '__IPYTHON__')


LANG_NEAR = {'en-US': 'en-GB', 'en-GB': 'en-US', 'fr': 'fr-CA', 'fr-CA': 'fr', 'es': 'es-419', 'es-419': 'es', 'pt': 'pt-BR', 'pt-BR': 'pt'}   # the same language for another region


def language_code(s):
    """A game language as typed (a code such as ja / zh-Hans, any case, _ for -, or its name such as Japanese) → its language code (nacp.LANGUAGES); None when it is none of them"""
    k = s.strip().lower().replace('_', '-')
    for c, n in nacp.LANGUAGES:
        if k in (c.lower(), n.lower()): return c
    return {'en': 'en-US', 'english': 'en-US', 'zh-cn': 'zh-Hans', 'zh-tw': 'zh-Hant'}.get(k)   # the settings' older codes for Chinese


def game_language(want, asked, have):
    """The language code to pack: want when the game supports it, else the same language for another region (en-GB for en-US, es for es-419, ...); have: the game's languages (empty: unknown, want as is).
    A language the game lacks stops the build when it was asked for; English, the default, then falls back to the game's first language"""
    if not have or want in have: return want
    if LANG_NEAR.get(want) in have: return LANG_NEAR[want]
    if asked: raise BuildError(f'the game has no {want} ({dict(nacp.LANGUAGES)[want]}); its languages: {language_list(have)} (--language / LANGUAGE)')
    return have[0]


def languages_control(*dirs):
    """The control.nacp that lists the game's languages: the first of the folders' (the packed version's, then the base game's) that lists any, else the first one's path. An update, a NUP's
    especially, may carry no control data: the language check and the launcher settings then read the base game's, the same file"""
    paths = [d / 'control.nacp' for d in dirs]
    return next((p for p in paths if nacp.languages(p)), paths[0])


def language_list(codes):
    names = dict(nacp.LANGUAGES)
    return ', '.join(f'{c} ({names[c]})' for c in codes)


def ask(prompt, what):   # what: what is missing, for the error message
    if PROG.mode == 'json': raise BuildError(f'{what} not given: JSON mode does not prompt; give it on the command line (-h for usage)')
    if not in_ipython() and not (sys.stdin and sys.stdin.isatty()):
        raise BuildError(f'{what} not given and there is no keyboard input to ask for it: give it on the command line (-h for usage)')
    s = input(prompt).strip().strip('"').strip("'").replace('\\ ', ' ')   # dragged-in paths come quoted or with escaped spaces
    return os.path.expanduser(s)


def run(cmd, log, env=None, cwd=None, on_line=None, ok=(), fatal=True):
    """Run a subprocess with all its output written to log (the console shows the stage line instead: the shader build alone emits tens of thousands of compiler warnings);
    stop on a non-zero exit not in ok, printing the log's last lines. on_line: called with every output line (pack_app.py's step lines move its stage on).
    fatal False: such an exit only prints those lines and returns its code (the caller ends the stage and goes on without what the command makes).
    The command and everything it starts run in a process group of their own (proc_group): an interrupt, a SIGTERM (cli) or an error here (a log that cannot be written)
    stops the whole group, not only the command; a process of the group still running proc_group.DRAIN s after the command's own exit is stopped, and the step fails
    as for an exit code (what it writes may be incomplete; the code is 1 when the command's own was 0)"""
    cmd = [str(c) for c in cmd]
    log.parent.mkdir(parents=True, exist_ok=True)
    tail = collections.deque(maxlen=40)
    with open(log, 'w') as f, proc_group.Group(cmd, env=env, cwd=cwd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT) as g:
        for line in g.lines():
            f.write(line); tail.append(line)
            if on_line:
                try: on_line(line.rstrip('\n'))
                except Exception: pass   # progress never stops the build (an interrupt raised in it does)
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


def build_id(nso):
    with open(nso, 'rb') as f:
        f.seek(0x40); return f.read(16).hex()


def _gone(func, path, exc):   # shutil.rmtree's onexc hook (Python 3.12+, the script's floor)
    if not (isinstance(exc, OSError) and exc.errno == errno.ENOENT): raise exc   # anything but "already gone" is a real failure


OBSOLETE = 'obsolete'   # <build folder>/obsolete: what a build or a run of the game no longer uses, in one folder to delete (retire, empty_obsolete)
STAMP = re.compile(r'\d{4}-\d{2}-\d{2} \d{6}')   # the start of the build or run that retired an entry, as its folder there is named: <date> <HHMMSS> <what>
BUILD_STARTED = None   # this build's start, so named (cli)
BUILD_FOLDERS = set()   # the build folders this build used (main), whose obsolete folders it empties once it succeeds


def obsolete_stamp(): return time.strftime('%Y-%m-%d %H%M%S')


def retire(work, p):
    """p, under the build folder work, no longer used: moved into work/obsolete/<this build's start> build/ at its place in the build folder (a rename, at once however large), so that
    what a build left behind is one folder to look at or delete; the next successful build empties it (empty_obsolete). On another volume (a move would be a copy) it is deleted"""
    p = Path(p); rel = p.relative_to(work) if p.is_relative_to(work) else Path(p.name)
    dest = work / OBSOLETE / f'{BUILD_STARTED or obsolete_stamp()} build' / rel
    dest.parent.mkdir(parents=True, exist_ok=True)
    if dest.exists() or dest.is_symlink(): rmtree(dest)
    try: os.rename(p, dest)
    except OSError as e:
        if e.errno != errno.EXDEV: raise
        rmtree(p)


def empty_obsolete(work):
    """After a successful build: the obsolete folder's entries of earlier builds and runs go (those this build retired stay until the next one, so the previous chain's files can still be
    taken back), and one line says what is left. A run of the game retires what its pipeline archive no longer uses there too (DM_OBSOLETE, mac_run)"""
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
    """Make p gone. An entry that vanishes between the listing and its unlink is the goal reached, not an error: on an exFAT / FAT drive macOS keeps each AppleDouble `._x` beside `x`
    and drops it when `x` is deleted, so the walk's later unlink of `._x` raised FileNotFoundError and left the folder half deleted"""
    if p.is_symlink() or p.is_file(): p.unlink()
    elif p.exists(): shutil.rmtree(p, onexc=_gone)   # Python 3.13 and later pass these by themselves


def swap_in(tmp, dst):
    """Publish the finished output tmp (a sibling of dst, so the same volume) as dst. dst is always the whole old output, the whole new one, or absent; never half deleted, which a
    reuse check could take for a finished output: the old one is renamed aside first (<dst>.old.partial: as every unfinished folder, a name the build ignores), the new one renamed in,
    and only then the old one deleted. Recoverable, not atomic: two renames. What stops it before the new one is in (a failure, an interrupt or SIGTERM, also between the two
    renames) puts the old one back, judged by what is on the disk rather than by which step ran; if that rename fails too, BuildError says where the old output is kept. An aside
    folder an earlier run left is put back when nothing is in place (a hard kill between the renames: the old output, whole) and deleted only when the new output went in (the old
    one's remains, cut short while being deleted)"""
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
    """The environment the build's tools run in: the caller's PATH first, so the tools it selects (a venv's python3, mise's zstd) are the ones that run,
    then TOOL_PATHS for the tools it lacks (a GUI app such as Spyder gets only the system PATH, or none)."""
    return dict(environ, PATH=os.pathsep.join([environ.get('PATH') or '/usr/bin:/bin:/usr/sbin:/sbin'] + TOOL_PATHS))


# ---- The NUP's versions ----
def files_with_suffix(folder, *suffixes):
    """The files of folder whose extension is one of suffixes in any letter case (`Game [id].NUP` from a download): Path.glob('*.nup') compares case-sensitively on every
    POSIX system, a case-insensitive Mac volume included. An AppleDouble file does not count: on an exFAT / FAT drive macOS keeps a file's extended attributes (a download's quarantine
    flag) in `._<name>`, one beside each game file"""
    return sorted(p for p in folder.iterdir() if p.suffix.lower() in suffixes and not p.name.startswith('._') and p.is_file())


def vnum(ver):   # 'v655360' → 655360
    return int(ver[1:])


# ---- The build folder and the profile folders ----
REWRITER_FILES = ('patch_nso.py', 'jump_solver.py', 'nso.py', 'nso_code.py', 'nso_asm.py', 'nso_syms.py', 'nso_match.py')   # the rewriter and the modules it imports: inputs of the rewrite's key


EMPTY_TABLES = {   # a derived profile's shader tables before the shader split fills them: the profile inputs' digest (dmsh_chain.py --inputs) and the host's build read them
    'shader_map.tsv': '# kind: evidence  derived profile: filled by the shader split\npackage\tindex\tstage\tdm_ctl_sha16\tsource\n',
    'shader_facts.tsv': '# Answer table for translation queries (generated by dm/gfx/tools/shader_split.py, do not edit): key <TAB> fact JSON. Derived profile: filled by the shader split\n',
    'shader_pairs.tsv': '# kind: evidence  vertex / pixel shader pairs within BNSH variations (generated by dm/gfx/tools/shader_split.py, do not edit): vertex key <TAB> pixel key\n',
    'shader_map.tsv.conflicts.tsv': '# kind: evidence  control-section key conflict rulings: empty\ndm_ctl_sha16\tchosen_pc\tclass\tcandidates\trecords\n',
    'bf_want.tsv': '# generated\n',
    'pso_manifest.tsv': '# Prewarm seed list (shipped in the app, next to shader_map.tsv). Rows: S<TAB>shader key<TAB>stage / R<TAB>render pipeline key (pso_skey hex) / C<TAB>compute shader key / M<TAB>task shader key or -<TAB>native task / mesh pipeline key (pso_skey hex: the mesh shader in the vertex place).\n'
                        '# Empty until recorded on the Mac (DM_PSO_MANIFEST=off DM_PSO_RECORD).\n'}


def nup_versions(fam):
    """The versions the NUP holds: {version: its update package, or None for the base game alone}. The base game is v0, an update v<its version number>; a NUP may have no base game"""
    out = {'v0': None} if fam['base'] is not None else {}
    for v, p in fam['updates']: out[f'v{v}'] = p
    return out


def pick_packages(fam, have, versions):
    """For each version to unpack, (base game, update or None): v0 = the base game alone, v<N> = the base game + the update with version number N (nup_versions).
    A NUP may have no base game: (None, update)"""
    if fam['base'] is not None: say(f'base: {fam["base"].name}')
    out = {}
    for ver in versions:
        if ver not in have: raise BuildError(f'no update {ver} in the NUP (the game profile needs these versions: {", ".join(versions)})')
        if have[ver]: say(f'update {ver}: {have[ver].name}')
        out[ver] = (fam['base'], have[ver])
    return out


def work_dir(args):   # the build folder --work-dir / WORK_DIR names, used as given; None = the default, <title ID>_build in the NUP's folder
    w = args.work_dir or WORK_DIR
    return Path(os.path.expanduser(w)).resolve() if w else None


def build_folder(args, nup_dir, gid):   # the build folder of the game profile gid: --work-dir / WORK_DIR as given, else <NUP folder>/<gid>_build
    return work_dir(args) or nup_dir / f'{gid}_build'


def own_profile_folders(args, nup_dir, app):
    """The folders this script writes a derived profile to (profile_folder's choices: <NUP folder>/<title ID>_build/profile, <work dir>/profile, --profile-out). A profile there is this
    script's own output, never a registered one, also when DM_GAMES_DIR reaches it (a derived build's DM_GAMES_FIRST): main leaves these folders out of the lookup, so a derived
    profile is derived again from its registered profile, never onto itself"""
    out = [nup_dir / f'{app:016X}_build' / 'profile']
    if work := work_dir(args): out.append(work / 'profile')
    if args.profile_out or PROFILE_OUT: out.append(Path(os.path.expanduser(args.profile_out or PROFILE_OUT)))
    return {p.resolve() for p in out}


def profile_folder(args, work):   # where the derived profile goes: --profile-out / PROFILE_OUT, else <build folder>/profile
    if args.profile_out or PROFILE_OUT: return Path(os.path.expanduser(args.profile_out or PROFILE_OUT)).resolve()
    return work / 'profile'


# ---- Derived profile (a version the game's profile was not brought up on) ----
DERIVED_SKIP = {'game.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'patches.py'}   # made again from the built version's own files (patches.py: the rewriter's patches of the registered builds)


def derived_key(profile_dir):
    return profile_dir.with_name(profile_dir.name + '.derived.key')


def derived_split_key(profile_dir):   # the split's inputs among a derived profile's (derive_profile): a match keeps the tables it wrote
    return profile_dir.with_name(profile_dir.name + '.derived.split.key')


def frame_step_builds(path, table='FRAME_STEP'):   # the keys of a table of a patches.py (FRAME_STEP: build IDs; MATCH_FRAME_STEP: module names), read without running the file (empty: no table)
    try:
        for n in ast.parse(path.read_text(encoding='utf-8')).body:
            if isinstance(n, ast.Assign) and any(isinstance(t, ast.Name) and t.id == table for t in n.targets) and isinstance(n.value, ast.Dict):
                return {k.value for k in n.value.keys if isinstance(k, ast.Constant)}
    except (OSError, UnicodeError, SyntaxError): pass
    return set()


def profiles_first(env, pdir):
    """Put pdir, the folder of a derived profile, first on the search path of game.py, which the shader tools, make and the packers all use (DM_GAMES_DIR), before the repository's own
    profile of the same ID (DM_GAMES_FIRST): in this process and in env, the tools' environment"""
    os.environ['DM_GAMES_DIR'] = env['DM_GAMES_DIR'] = os.pathsep.join([str(pdir)] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d and d != str(pdir)])
    os.environ['DM_GAMES_FIRST'] = env['DM_GAMES_FIRST'] = '1'


GPU_DRIVERS = {'NintendoSDK_OpenGL', 'NintendoSDK_Vulkan'}   # NVIDIA's drivers program the GPU through NvRm, which the host does not have: never loaded


def load_order(m):   # the modules' load order on Horizon: main, the subsdks by number, sdk
    return (m != 'main', m == 'sdk', int(m[6:]) if m.startswith('subsdk') and m[6:].isdigit() else 0)


def nso_image(path):   # an NSO module's three segments decompressed (dm/tools/nso.py); nso.Error (a ValueError) on a damaged file
    return bytes(nso.load(Path(path).read_bytes(), str(path)).image)


def nso_files(exefs, skip=()):   # the code modules in a folder, in name order
    out = []
    for p in sorted(Path(exefs).iterdir()):
        if p.name in skip or p.name == 'rtld' or not p.is_file(): continue
        with open(p, 'rb') as f:
            if f.read(4) == b'NSO0': out.append(p)
    return out


def module_kind(m):   # a short name for what a module is: its NintendoSDK component, GLSLC, or the nn:: namespace most of its exports are in
    if m['comps']: return sorted(m['comps'])[0]
    if any(s.startswith('glslc') for s in m['exports']): return 'GLSLC'
    areas = collections.Counter(nso_syms.area(s) for s in m['exports'])
    return areas.most_common(1)[0][0] if areas else '?'


def main_modules(exefs, modules, hooks=None):
    """The modules a profile's list leaves out that main imports from, {module: kind}, in load order: a module's slot is not fixed across a game's versions, so an update may ship one
    the registered version does not have. A module counts when it exports a symbol main imports that no listed module and no host hook supplies (hooks: the host's own functions for
    imports, by default dm/host's, without the HLE_FORCE wrappers that still call the module's function); never a GPU driver. Reads nothing with no other module there"""
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
    """The game's profile for a version it was not brought up on (the NUP has another update, no update, or files of other builds), in pdir (outside the repository): its keys as they
    are, with this version's build row, package and shader versions. The shader tables and the chain tables are left out: the build makes them again from this version's own files.
    The hand-written tables (the prewarm list, shader_facts_manual.tsv) are copied: they name shaders by content key. A folder derived from the same inputs (the files copied and the
    game.tsv written, kept in <pdir>.derived.key) is kept as it is, with what later stages wrote into it (the split's tables), so their own reuse holds. shader_only preserves all
    registered game.tsv rows and links its existing cuts: only the shader tables change with installed DLC. The modules are the profile's that this version ships and those main imports
    from that the list leaves out (main_modules). A profile with frame step stubs (FRAME_STEP) and no function-matched ones (MATCH_FRAME_STEP) for other builds only gets fps_target 0
    for this version: its main has no state page, so a cap of 30 or 60 would present every vsync while the game steps 1/30 s per frame (double speed). Returns the version's display version"""
    if pdir.resolve() == g.dir.resolve():   # the delete below would take the files the copy reads; main never takes this script's own profiles as registered ones (own_profile_folders)
        raise BuildError(f'game profile {g.id} would be derived onto its own folder {g.dir}')
    src = sorted(p for p in g.dir.iterdir() if p.is_file() and (p.name not in DERIVED_SKIP or shader_only and p.name == 'patches.py'))
    # The display version: the package's control data, else the one the profile registers for this main build. A NUP update may carry no control data, and the game asks the host for
    # the version by its main's build ID (hle_sdk16.c GetDisplayVersion, the build rows): a row with ? is what the game then shows
    main = d / 'ExeFS/main'; bid = build_id(main) if main.exists() else None
    known = next((b['display'] for b in g.builds().values() if bid and b.get('main') == bid and b.get('display') not in (None, '', '?')), None)
    T, disp = '\t', nacp.version(d / 'control.nacp') or known or '?'
    add = main_modules(d / 'ExeFS', mods) if not shader_only and main.exists() else {}   # a module's slot can change between versions: what main imports and the profile's list leaves out. The host loads what the row names
    mods = sorted({*mods, *add}, key=load_order) if add else mods
    row = T.join(['build', ver, disp] + [f'{m}={build_id(d / "ExeFS" / m)}' for m in mods if (d / 'ExeFS' / m).exists()])
    # the header names the profile by its ID, not its folder: the text is part of the derived key, so the same profile in another checkout keeps the derived folder and the split
    out, put = [f'# Derived from game profile {g.id} by scripts/build_app.py for {ver} ({disp}), a version that profile was not brought up on:',
                '# its keys as they are, this version\'s build row, and shader tables made from this version\'s own files'], False
    if shader_only: out = [f'# Derived shader tables from game profile {g.id}; registered module builds and settings are preserved.']
    stubs = frame_step_builds(g.dir / 'patches.py')
    # a frame step placed by its functions (MATCH_FRAME_STEP) may serve this main; whether it does is known only when the rewrite matches it, and where it does not the main
    # has no state page and the host keeps the game's own present interval anyway (fps_present_cap: a cap needs a frame step or an engine limit the host writes)
    matched = 'main' in frame_step_builds(g.dir / 'patches.py', 'MATCH_FRAME_STEP')
    own_step = shader_only or not stubs or matched or not main.exists() or bid in stubs
    for line in (g.dir / 'game.tsv').read_text(encoding='utf-8').splitlines():
        k = line.split(T, 1)[0]
        if not own_step and k == 'fps_target' and line.partition(T)[2].strip() != '0':
            out += ['# the profile has frame step stubs for other builds only: this version\'s main has none, so it keeps the game\'s own step', f'fps_target{T}0']; continue
        if not shader_only and k == 'modules' and (d / 'ExeFS').is_dir():
            want = line.partition(T)[2].split(); have = [m for m in want if (d / 'ExeFS' / m).exists()]
            if add: have = sorted({*have, *add}, key=load_order)
            if have != want:   # a module this version does not ship: left in, the rewrite would stop on the missing file
                if gone := [m for m in want if m not in have]: out.append(f'# this version ships no {" ".join(gone)}')
                if add: out.append('# main imports from ' + ', '.join(f'{m} ({kind})' for m, kind in add.items()) + ', which the profile does not list')
                out.append(f'modules{T}{" ".join(have)}'); continue
        if not shader_only and k in ('build', 'package', 'shader_versions'):
            if not put: out += [row, f'package{T}{ver}', f'shader_versions{T}{ver}']; put = True
            continue
        out.append(line)
    text, kf, skf = '\n'.join(out) + '\n', derived_key(pdir), derived_split_key(pdir)   # beside the profile, not in it: no tool reads them as tables
    key = fingerprint(('files', src), text)
    # What the split reads of the derived profile besides the tables it writes (split_profile_inputs): game.tsv's settings and the registered tables of SPLIT_TABLES the folder
    # copies (the hand-written facts). When only other copied files changed (the game's note, the prewarm list, geo_mesh.tsv, the patches.py link), the copies are made again and
    # the tables the split and the shader step wrote are kept, so the split's reuse holds: wiping them made the split run again on unchanged game files
    skey = fingerprint(('files', [p for p in src if p.name in SPLIT_TABLES]), text)
    def rows(text):   # the same whitespace and comment rules as game.Game
        return [tuple(c.strip() for c in l.split(T)) for l in text.splitlines() if l.strip() and not l.lstrip().startswith('#')]
    def game_kept():   # game.tsv holds the rows written for these inputs (later stages may have added their own)
        expected = collections.Counter(rows(text)); keys = {r[0] for r in expected}
        try: kept = collections.Counter(r for r in rows((pdir / 'game.tsv').read_text(encoding='utf-8')) if r[0] in keys)
        except (OSError, UnicodeError): kept = None
        return kept == expected
    def copy_in(keep):   # the copies of src, made again; the folder's other files go, but for game.tsv and keep
        for p in pdir.iterdir():
            if p.name != 'game.tsv' and not keep(p): rmtree(p)
        for p in src:
            # Registered cuts stay the same file: patch_nso.py deduplicates real paths, so an external profile must not register copied claims for the same builds.
            if shader_only and p.name == 'patches.py': (pdir / p.name).symlink_to(p.resolve())
            else: shutil.copyfile(p, pdir / p.name)
        kf.write_text(f'{key}\n'); skf.write_text(f'{skey}\n')
    why = 'no folder yet' if not (pdir / 'game.tsv').is_file() else 'game.tsv changed since' if not game_kept() else None
    if not why:
        if reused(kf, key) is not None: return disp
        if reused(skf, skey) is not None:   # the tables later stages wrote (DERIVED_SKIP's names, but the patches.py link) stay
            copy_in(lambda p: p.name in DERIVED_SKIP and p.name != 'patches.py')
            say(f'derived profile {pdir}: copied files changed, the split\'s tables kept'); return disp
        why = 'no split key' if not skf.is_file() else 'the split\'s inputs changed'
    say(f'derived profile {pdir}: made again ({why})')
    rmtree(kf); rmtree(skf); pdir.mkdir(parents=True, exist_ok=True)
    atomic_text(pdir / 'game.tsv', text)
    copy_in(lambda p: False)
    return disp


# ---- Stage reuse: a stage whose inputs are all as at its last successful run keeps its output ----
def files_under(d):   # (relative path, stat) of every file under d, sorted; [] when it does not exist
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
    """sha256 over a stage's inputs. Items: ('files', paths) = names and content (a missing file counts too); ('tree', folder) = relative path and size of every file under it
    (a RomFS: check_build pins its version, so names and sizes stand for its content); ('stat', folder) = relative path, size and mtime of every file under it (unpacked game files, too large to read for a key);
    ('cmd', argv) = a tool's version output (run with env); anything else = its repr"""
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
            try: r = subprocess.run(v, env=env, capture_output=True, text=True, timeout=120); h.update((r.stdout + r.stderr).encode())
            except (OSError, subprocess.TimeoutExpired): h.update(b'missing')
        else: h.update(repr(v).encode())
    return h.hexdigest()


def reused(keyfile, key):   # the note the stage's last successful run wrote beside its key ('' when none), or None when the key differs or is missing
    try: k, _, note = keyfile.read_text().partition('\n')
    except OSError: return None
    return note.strip() if k == key else None


def python_tool_files(repo, entry, roots):   # transitive repository imports, including imports inside reader functions; unrelated tools are not inputs
    dirs, todo, seen = [repo / d for d in roots], [repo / entry], set()
    while todo:
        p = todo.pop()
        if p in seen or not p.is_file(): continue
        seen.add(p)
        for n in ast.walk(ast.parse(p.read_bytes())):
            mods = [a.name for a in n.names] if isinstance(n, ast.Import) else [n.module] if isinstance(n, ast.ImportFrom) and n.module else []
            todo += [d / f'{m.split(".")[0]}.py' for m in mods for d in dirs]
    return sorted(seen)


def translator_key(xlate):   # the translator by its bytes alone: a checkout that rewrites the same bytes (a commit switched and back) gives them a new mtime, and each worktree has its own path; either rebuilt every shader
    return ('files', [xlate])


def chain_files(repo):   # what the shader build runs: not the split's readers (shader_split.py)
    return python_tool_files(repo, 'dm/gfx/tools/dmsh_build.py', ('dm/gfx/tools', 'dm/games')) + [repo / 'dm/gfx/tools/dmsh_chain.lock']


SPLIT_TABLES = ('shader_facts_manual.tsv', 'shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')   # the profile tables the split reads or is checked against (derive_profile keys on them too)


def split_profile_inputs(g):   # shader_split.py's GAME reads, plus the tables the packer checks or writes; renderer settings, notes and translation tables are not extraction inputs
    settings = [(name, g.get(name, '')) for name in ('modules', 'shader_groups', 'shader_note', 'shader_key_skip')]
    tables = [g.table(n) for n in SPLIT_TABLES]
    return [g.id, settings, ('files', tables)]


def split_output_key(pkg, g):   # detect deleted/edited packages and supplemental pairs before reusing a successful split
    return fingerprint(package_key(pkg), ('files', [pkg.with_name('pso_pairs.tsv')] if not g.table('pso_pairs.tsv').is_file() else []))


def package_key(pkg):   # the shader package's content (file names and bytes): what the shader libraries are built from
    fs = [rel for rel, _ in files_under(pkg)]
    return fingerprint(fs, ('files', [pkg / f for f in fs]))


def profile_files(g):   # every file of the game profile (game.tsv and its tables) but the c1 / uniform extent tables, which the shader step makes (chain_tables): no step reads them,
    out = {name for name, _, _ in CHAIN_TABLES} | {'patches.py'}   # and the rewriter's patches, which only the rewrite step reads (patch_files)
    return sorted(p for p in g.dir.iterdir() if p.is_file() and p.name not in out)


# ---- Steps ----
def check_capstone():   # patch_nso.py checks the same, but only at the rewrite step, after the extraction
    import capstone
    if capstone.cs_version()[0] != 5:   # the rewriter uses Capstone 5's names (CS_ARCH_ARM64, ARM64_OP_*), which Capstone 6 renamed
        cmd = shlex.join([sys.executable, '-m', 'pip', 'install', 'capstone>=5,<6'])
        raise BuildError(f'Capstone {capstone.__version__} is unsupported; the rewriter requires Capstone 5.x. Run: {cmd}')


def check_prereqs(env, xlate):
    missing = [m for m in ('lz4.block', 'capstone', 'fontTools', 'zbic') if not _importable(m)] + ([] if _importable('compression.zstd') or _importable('backports.zstd') else ['backports.zstd'])   # zstd: built in from Python 3.14. zbic: a system 22.0.0+ module's segments, which only the rewrite reads, after the whole game is unpacked
    if missing: raise BuildError(f'the current Python ({sys.executable}) is missing modules: {", ".join(missing)} (pip install lz4 "capstone>=5,<6" fonttools zbic backports.zstd; backports.zstd only before Python 3.14)')
    check_capstone()
    tools = {'xcodebuild': 'Xcode', 'make': 'Xcode'}
    gone = [f'{t} ({how})' for t, how in tools.items() if not shutil.which(t, path=env['PATH'])]
    if gone: raise BuildError('tools not found: ' + ', '.join(gone))
    mtl = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '--version'], capture_output=True, text=True, env=env)   # Xcode 26 and later install the Metal toolchain separately
    if mtl.returncode: raise BuildError(f'the Metal compiler does not run ({(mtl.stderr.strip().splitlines() or ["no output"])[-1]}): install it with xcodebuild -downloadComponent MetalToolchain')
    if not xlate.exists(): raise BuildError(f'the shader translator is missing from the repository: {xlate}')
    check_translator(xlate, env)
    sc = env.get('SPIRV_CROSS', str(xlate.parent.parent / 'spirv_cross_bin/spirv-cross'))
    if not shutil.which(sc, path=env.get('PATH')): raise BuildError(f'spirv-cross is missing or not executable: {sc}')


def check_translator(xlate, env):
    """The shader translator is a committed native executable (dm/tools/native_tools.sh, universal): one short run of `--source-sha`, which translates nothing, shows before the extraction
    that it starts (not missing, executable, not held back by macOS), where dmsh_build.py would stop at the shader step (tool_lock.py xlate_embedded reports the same failure there)"""
    try: r = subprocess.run([str(xlate), '--source-sha'], capture_output=True, text=True, env=env, timeout=60, errors='replace')
    except (OSError, subprocess.SubprocessError) as e: raise BuildError(f'the shader translator does not start ({e}): {xlate}') from e
    if r.returncode == 0 and r.stdout.strip(): return
    why = (r.stderr.strip().splitlines() or r.stdout.strip().splitlines() or [f'exit code {r.returncode}'])[0]
    raise BuildError(f'the shader translator does not run ({why}): {xlate} is the committed native executable; check out the repository again if it is missing or damaged')


def check_native_python():
    # An x86_64 Python (an Intel Homebrew in /usr/local) runs under Rosetta on Apple silicon, and the tools it starts run as x86_64 too: make and clang build the Mac host, which runs
    # the game's own arm64 code, for x86_64, and every universal tool runs translated. sysctl.proc_translated is 1 under Rosetta, 0 or missing otherwise
    if sys.platform != 'darwin' or subprocess.run(['sysctl', '-n', 'sysctl.proc_translated'], capture_output=True, text=True).stdout.strip() != '1': return
    raise BuildError(f'this Python ({sys.executable}) is an Intel one running under Rosetta, and the tools it starts would run as Intel code too: run the script with an arm64 Python (/usr/bin/python3, or Homebrew\'s in /opt/homebrew)')


def _importable(m):
    try: __import__(m); return True
    except ImportError: return False


def nup_file(path):   # the NUP to build from: path when it is a .nup file, else the one .nup in the folder
    if path.is_file():
        if not pack_nup.is_nup(path): raise BuildError(f'not a NUP file: {path}')
        return path
    nups = [p for p in files_with_suffix(path, '.nup') if pack_nup.is_nup(p)]
    if len(nups) > 1: raise BuildError(f'more than one NUP in {path}: {", ".join(p.name for p in nups)}; give the .nup file to build from in place of the folder')
    if not nups: raise BuildError(f'no NUP (a .nup file) in {path}')
    return nups[0]


def scan_nup(path):
    """What a NUP holds: (title ID, {'base': the base game's package or None, 'updates': [(version, package)], 'dlc': [(title ID, package)], 'nup': the open container})"""
    try:
        n = pack_nup.Nup(path)
        app, base, vers, dlc = pack_nup.scan(n)
    except (pack_nup.Error, OSError) as e: raise BuildError(str(e)) from e
    return app, {'base': base, 'updates': sorted(vers.items()), 'dlc': sorted(dlc.items()), 'nup': n}


def extract_nup(ex, name, k, base, app, n):   # one version of a NUP (k: its base or an update package) into ex/<name>: ExeFS/, RomFS.img, the control data; the code modules and the image are kept while unchanged
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
    """Each unpacked image's files whose bytes do not start as their names promise (romfs_image.format_mismatches: a .zs file that is not a zstd stream) stop the build: a NUP carries
    no hashes, so this is where its damage shows. ALLOW_DAMAGED lists them and goes on. A NUP of the game held three .zs textures beginning with 12,288 zero bytes; the game asked for
    one again on every pass of its background loader (412% CPU on the Mac). The result is kept beside the image (RomFS.img.formats) with its size and time, so an unchanged image is
    not read again (102,185 .zs files, 16 s from a USB SSD)"""
    import romfs_image
    for ver, img in ((ver, img) for ver, d in dirs.items() for img in [d / 'RomFS.img', *sorted((d / pack_nup.PROGRAMS).glob('*.img'))]):   # the other programs' RomFS too, when a NUP has any
        stamp = img.with_name(img.name + '.formats')
        if img.parent.name == pack_nup.PROGRAMS: ver = f'{ver} program {img.stem}'
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
    """The NUP's DLC → <build folder>/aoc/<index>.img (the RomFS of each one; index = title ID − the application's add-on content base ID). The host reports these indices as owned
    and mounts them (DM_AOC_DIR). pack_nup.pack_dlc keeps each image while its inputs are unchanged and removes those of DLC no longer in the NUP. Returns None without DLC"""
    if not fam['dlc']: return None
    PROG.stage('unpack DLC', step='unpacking')
    try: pack_nup.pack_dlc(fam['nup'], app, dict(fam['dlc']), work / 'aoc', say=lambda s: None)
    except pack_nup.Error as e: raise BuildError(str(e)) from e
    PROG.end(f'{len(fam["dlc"])} DLC')
    return work / 'aoc'


def build_mismatch(d, name, g):   # the modules of an extraction whose build ID is not the one the profile's build row of that version registers ([] = the registered build)
    want = g.builds().get(name)
    if want is None: return [f'{name} has no build row']
    return [f'{m} {build_id(d / "ExeFS" / m)} (registered {bid})' for m, bid in want.items() if m != 'display' and build_id(d / 'ExeFS' / m) != bid]


def patch_files(repo, exe_dir, mods, game_id=None):
    """The patch files that name one of the modules' builds (dm/tools/sdk_patches.py, dm/games/*/patches.py): patch_nso.py loads them all but applies only those of a module's
    build ID. The selected game's file also counts for unknown builds: it may supply MATCH_CUTS. sdk_patches.py always counts: its MATCH_CUTS are SDK-wide and apply to builds it does
    not name. The patches.py of profiles in DM_GAMES_DIR (a derived profile's link) count too, as patch_nso.py loads them"""
    ids = {build_id(exe_dir / m) for m in mods}
    selected = Path(game_id).name if game_id else None
    extra = [p for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d for p in sorted(Path(d).glob('*/patches.py'))]
    shared = [repo / 'dm/tools/sdk_patches.py']   # its MATCH_CUTS apply to every sdk module
    files = {}
    for p in shared + sorted((repo / 'dm/games').glob('*/patches.py')) + extra: files.setdefault(p.resolve(), p)
    return [p for p in files.values()
            if p in shared or p.parent.name == selected or any(i in p.read_text(errors='replace') for i in ids)]


def rewrite(py, repo, exe_dir, out, env, logs, g):
    """patch_nso.py on every code module of the profile (each writes its own <module>.* files, all the same build_src.sha256 line), each with its log, as many at once as the Mac has cores.
    Reused when the modules, patch_nso.py (with the modules it imports and the selected profile's function rules / the patch files of their builds), its switches and capstone are as
    at the last successful rewrite"""
    import capstone
    try: workers = int(env['DM_REWRITE_J']) if env.get('DM_REWRITE_J') else None
    except ValueError: raise BuildError('DM_REWRITE_J must be a positive integer') from None
    if workers is not None and workers < 1: raise BuildError('DM_REWRITE_J must be a positive integer')
    PROG.stage('rewrite', step='checking the inputs')
    mods = g.modules_in(exe_dir)   # a version may lack a module of the profile another version has (game.py build_modules)
    src = {m: exe_dir / m for m in mods}
    kf = out.with_name(out.name + '.key')
    # the repository profile's build rows decide which builds the rewriter treats as the game's own (SDK-wide function patches skip those), so they are inputs too
    try: own = [l for l in (repo / 'dm/games' / Path(env.get('DM_GAME') or '-').name / 'game.tsv').read_text(encoding='utf-8').splitlines() if l.startswith('build\t')]
    except OSError: own = []
    key = fingerprint(('files', [repo / 'dm/tools' / f for f in REWRITER_FILES] + patch_files(repo, exe_dir, mods, env.get('DM_GAME')) + list(src.values())), mods, capstone.__version__,
                      {k: env.get(k) for k in ('DM_PATCH_FRONT', 'DM_GAME')}, own)
    if all((out / f'{m}.patched.bin').is_file() for m in mods) and reused(kf, key) is not None:
        PROG.end(f'{", ".join(mods)}, reused (inputs unchanged)'); return
    tmp = out.with_name(out.name + '.partial'); rmtree(tmp); tmp.mkdir(parents=True); logs.mkdir(parents=True, exist_ok=True)
    PROG.set(step=', '.join(mods), done=0, total=len(mods), unit='modules')
    procs, logf, todo, cores = {}, {}, list(mods), max(2, os.cpu_count() or 4)
    jobs = max(1, min(cores, 2 * cores // max(1, len(mods))))   # patch_nso.py workers per module, so parallel rewrites don't oversubscribe
    rewrite_env = dict(env, DM_REWRITE_J=str(workers if workers is not None else jobs))
    try:
        while todo or procs:
            while todo and len(procs) < cores:
                m = todo.pop(0); logf[m] = open(logs / f'rewrite_{m}.log', 'w')
                procs[m] = subprocess.Popen([str(py), str(repo / 'dm/tools/patch_nso.py'), str(src[m]), str(tmp), m], env=rewrite_env, stdout=logf[m], stderr=subprocess.STDOUT)
            for m in [m for m in procs if procs[m].poll() is not None]:
                p = procs.pop(m); logf[m].close(); PROG.set(done=len(mods) - len(todo) - len(procs))
                if p.returncode:
                    log = logs / f'rewrite_{m}.log'
                    PROG.end(ok=False)
                    PROG.say(f'---- last lines of {log} ----\n' + ''.join(collections.deque(open(log, errors='replace'), maxlen=40)).rstrip('\n'))
                    raise BuildError(f'patch_nso.py exit code {p.returncode} on {m} (full output: {log})')
            time.sleep(0.2)
    finally:   # an error or an interrupt stops the other rewrites
        for p in procs.values():
            if p.poll() is None: p.terminate(); p.wait()
        for f in logf.values(): f.close()
    rmtree(kf); swap_in(tmp, out); kf.write_text(f'{key}\n')
    PROG.end(', '.join(mods))


def apple_silicon():   # hw.optional.arm64 is 1 on Apple silicon (for a Python under Rosetta too); an Intel Mac has no such key
    return subprocess.run(['sysctl', '-n', 'hw.optional.arm64'], capture_output=True, text=True).stdout.strip() == '1'


HOST_DIRS = ('dm/host', 'dm/gfx', 'dm/gfx/nvn', 'dm/gfx/nvn/include', 'dm/gfx/tex')   # the folders the host's compile lines take their sources and headers from (dm/host/Makefile SRC, GFX)


def build_mac_host(repo, out, env, logs, g):
    """The Mac host for the profile, built into out: dm/host's per-game host (make gen/<id>/dmhost: one object per source on every core, kept for the next build) copied with its dSYM,
    since make takes no target path with spaces and dm/host/dmhost may be another build's.
    Reused when the compile lines and their inputs (the source files of HOST_DIRS, the chain header dm/gfx/nvn/dmsh_chain.h among them, the profile header make generates, dav1d and its headers, the compiler
    and the macOS SDK) are as at the last build into out"""
    host = repo / 'dm/host'
    run(['make', '-s', f'GAME={g.id}', f'gen/{g.id}/game_profile.h', 'gen/dav1d/libdav1d.a', 'host_msl'], logs / 'mac_host_make.log', env=env, cwd=host)
    built = host / f'gen/{g.id}/dmhost'
    dry = subprocess.run(['make', '-n', '-B', f'GAME={g.id}', f'gen/{g.id}/dmhost'], env=env, cwd=host, capture_output=True, text=True)
    cmd = [l for l in dry.stdout.splitlines() if l.startswith('clang') and f' -o gen/{g.id}/' in l]   # the host's compiles and its link, not the libraries'
    if not any(f' -o gen/{g.id}/dmhost ' in l for l in cmd): raise BuildError(f'make -n gave no link line for the Mac host: {dry.stderr.strip()[-500:]}')
    kf, tp = out.with_name(out.name + '.key'), repo / 'dm/third_party'
    srcs = [p for d in HOST_DIRS if (repo / d).is_dir() for p in sorted((repo / d).iterdir()) if p.is_file() and p.suffix in ('.c', '.m', '.h', '.S', '.inc')]
    key = fingerprint(fingerprint(cmd, ('files', [host / f'gen/{g.id}/game_profile.h', host / 'gen/msl-macosx/host_msl_gen.h', host / 'gen/dav1d/libdav1d.a']),
                                  ('tree', tp / 'dav1d/include'), ('cmd', ['clang', '--version']), ('cmd', ['xcrun', '--show-sdk-build-version']), env=env),
                      ('files', srcs))
    if out.is_file() and reused(kf, key) is not None: return out
    rmtree(kf); out.parent.mkdir(parents=True, exist_ok=True)
    run(['make', '-s', f'GAME={g.id}', f'gen/{g.id}/dmhost'], logs / 'mac_host_build.log', env=env, cwd=host)
    part, dsym = out.with_name(out.name + '.partial'), Path(f'{out}.dSYM')
    shutil.copy2(built, part); os.replace(part, out)
    rmtree(dsym)
    if Path(f'{built}.dSYM').is_dir(): shutil.copytree(f'{built}.dSYM', dsym)   # crash reports and lldb read it beside the program
    kf.write_text(f'{key}\n')
    return out


# ---- The chain version and its tables ----
def atomic_text(path, text):
    f = tempfile.NamedTemporaryFile(mode='w', encoding='utf-8', dir=path.parent, prefix=f'.{path.name}.', delete=False)
    try:
        with f: f.write(text)
        os.replace(f.name, path)
    finally: Path(f.name).unlink(missing_ok=True)


CHAIN_H = 'dm/gfx/nvn/dmsh_chain.h'   # the chain version the host is compiled with, one for every game


def chain_of(repo):   # the chain version dm/gfx/nvn/dmsh_chain.h holds (None when it has none)
    try: m = re.search(r'#define DMSH_CHAIN "(.*)"', (repo / CHAIN_H).read_text())
    except OSError: return None
    return m.group(1) if m else None


def empty_tables(g):
    """What a derived profile needs before a Mac host can be built for it: the shader tables (empty until the shader split fills them; DERIVED_SKIP leaves them out of the copy),
    which the host's build and the chain's input digest read"""
    for n, text in EMPTY_TABLES.items():
        if not g.table(n).exists(): g.table(n).write_text(text)


def chain_header(py, repo, env, logs):
    """The chain version header, written again when it no longer holds the version the source tree gives (a chain change committed without dmsh_chain.py --header): dmsh_build.py stops
    on a stale one, and the host is compiled with it. The version follows from the source alone (dmsh_chain.py)"""
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py')], env=env, capture_output=True, text=True)
    if r.returncode or not r.stdout.split(): raise BuildError(f'dmsh_chain.py exit code {r.returncode}: {(r.stderr.strip() or "no version")[-500:]}')
    want, have = r.stdout.split()[-1], chain_of(repo)
    if have == want: return
    run([py, repo / 'dm/gfx/tools/dmsh_chain.py', '--header'], logs / 'dmsh_chain.log', env=env)
    say(f'chain version header rewritten: {repo / CHAIN_H} held {have or "no version"}, the source tree gives {want}')


CHAIN_TABLES = (('c1_full.tsv', 'c1_guard.py', 'c1 guard'), ('cb_extent.tsv', 'cb_extent.py', 'uniform read extents'))   # made from the shader build's Mac outputs: the host takes each only when its header names its library's stamp


def lib_head(lib):
    """What c1_full.tsv / cb_extent.tsv start with to be taken beside the library directory lib (shader_lib.table_head): its chain version and the profile inputs its stamp (inputs.tsv,
    written by a full dmsh_build.py run) names. None when it has no stamp"""
    stamp = shader_lib.read_stamp(lib)
    return f'# chain {shader_lib.name_of(lib)} inputs {stamp["inputs"]}' if stamp.get('inputs') else None


def chain_tables(py, repo, sh, env, logs, g, chain):
    """c1_full.tsv (c1_guard.py: shaders whose c1 reads are not proven bounded) and cb_extent.tsv (cb_extent.py: how far each shader reads into its uniform blocks) from the Mac outputs of the
    shader build (sh/cache/b-<chain>/), into the profile folder. They are not in the repository (git ignores them): they follow from the shaders this Mac built, so every pack makes its own.
    A full dmsh_build.py run makes both; this makes those missing or not naming the library's stamp again, which the host would ignore (every c1 its own 64 KiB, whole uniform bindings
    copied; GitHub issue #78). True when both name it; False when the library has no stamp (no full build's Mac outputs to make them from)"""
    lib = sh / 'cache' / f'b-{chain}'
    want = lib_head(lib)
    if not want: return False
    for name, tool, step in CHAIN_TABLES:
        p = g.table(name)
        try: head = p.read_text(errors='replace').partition('\n')[0]
        except OSError: head = ''
        if head.startswith(want + ' '): continue
        PROG.set(step=step)
        run([py, repo / 'dm/gfx/tools' / tool, lib, p], logs / f'{tool[:-3]}.log', env=dict(env, DMSH_TABLE_MEMO=str(sh / 'cache')))   # the shader build's memo (c1_guard.py)
        say(f'{name} ' + (f'named {" ".join(head.split()[2:5])}' if head.startswith('# chain ') else 'names no library' if head else 'missing') + f': made for {want[2:]} ({p})')
    return True


def data_rows(b):   # a table's data rows (without # comment lines: the header changes with the shader_split.py version, not with the game files)
    return [l for l in b.splitlines() if not l.startswith(b'#')]


def table_difference(name, profile, split):
    """How a split table differs from the profile's, for the build error: the row counts on each side and the first such row of each. Rows are compared as sets of lines, so one row added early does not read as every later row changed."""
    a, b = collections.Counter(data_rows(profile)), collections.Counter(data_rows(split))
    only_profile, only_split = list((a - b).elements()), list((b - a).elements())
    if not only_profile and not only_split: return f'{name}: the same rows in another order'
    out = [f'{name}: {len(only_profile):,} row(s) only in the profile, {len(only_split):,} only in the split']
    if only_profile: out.append('  profile: ' + only_profile[0].decode(errors='replace'))
    if only_split: out.append('  split:   ' + only_split[0].decode(errors='replace'))
    return '\n'.join(out)


def dlc_only_shader_change(g, tables):
    """A different DLC shader union, with the registered non-DLC shaders and facts unchanged. Package indices may be re-numbered when an add-on is removed.
    Source ownership is the split's /aoc/<index>/ prefix; a module or base/update shader difference never qualifies for this shader-only derivation."""
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
    """The shader split into pkg, checked against the profile's tables (own: a derived profile, which takes the tables as they come). The package is reused when the extractor and the
    modules it imports, the profile settings and tables it reads or is checked against, the DLC RomFS roots, each version's modules and RomFS (paths and sizes), and the package's own
    contents are as at the last successful split. Returns the package's content key, the shader build's input: a split that runs again because a reader changed and writes the same
    package leaves the shaders reused.
    derive_dlc: for the registered profile, called when the split's tables differ from the profile's only by the DLC's shaders (dlc_only_shader_change); returns a shader-only derived
    profile, which then takes the tables"""
    names = ('shader_map.tsv', 'shader_facts.tsv', 'shader_pairs.tsv')
    PROG.stage('shader split', step='checking the inputs')   # then the count of files and archive ranges scanned (shader_split.py's progress lines); the package and tables come at the end
    kf, outputkf = pkg.with_name(pkg.name + '.key'), pkg.with_name(pkg.name + '.outputs.key')
    inputs = [('files', python_tool_files(repo, 'dm/gfx/tools/shader_split.py', ('dm/gfx/tools', 'dm/tools', 'dm/games'))), bool(own), 'split-v2']
    # A damaged copy (ALLOW_DAMAGED) is reused too, but only as the same extraction: a repaired copy keeps every path and size, all 'tree' sees, so its RomFS is keyed by 'stat' (the
    # image's time) and by the damage list
    for r, m in zip(roms, mains):
        inputs += [('files', [m] + [m.parent / n for n in g.modules_in(m.parent) if n != 'main']), ('stat' if own or ALLOW_DAMAGED else 'tree', r)]
        if ALLOW_DAMAGED: inputs += [('files', [rfs.Path(r).with_name('RomFS.img.formats')])]   # check_formats' list of the damaged files
    inputs += [item for r in dlc_roms for item in (('dlc', Path(r).name), ('stat', r))]   # DLC has no registered module build to pin its contents: include its index and re-extraction times, even when file sizes stay the same
    key = fingerprint(*inputs, *split_profile_inputs(g))
    dlckf, dlckey, promoted = pkg.with_name(pkg.name + '.dlc.key'), None, False
    def promote():
        nonlocal g, own, env, promoted
        assert derive_dlc is not None   # promote() is called only with one
        g, own, promoted = derive_dlc(), True, True; inputs[1] = True
        env = dict(env, DM_GAMES_DIR=os.pathsep.join([str(g.dir.parent), env.get('DM_GAMES_DIR', '')]), DM_GAMES_FIRST='1')
    if not own and derive_dlc:
        dlckey = fingerprint(key, ('files', [Path(__file__)]))   # proof pins the comparison code as well as the registered tables and every split input
        if reused(dlckf, dlckey) is not None:
            promote()
            key = fingerprint(*inputs, *split_profile_inputs(g))
    have_pairs = g.table('pso_pairs.tsv').is_file() or pkg.with_name('pso_pairs.tsv').is_file()
    if pkg.is_dir() and have_pairs and (note := reused(kf, key)) is not None and reused(outputkf, split_output_key(pkg, g)) is not None:
        PROG.end(f'{note}, reused (inputs unchanged)'); return package_key(pkg)
    PROG.set(step='splitting')
    tmp = pkg.with_name(pkg.name + '.partial'); rmtree(tmp); (tmp / 'tables').mkdir(parents=True)
    def counted(ln):   # shader_split.py's 'progress: <done>/<total>'
        if m := re.fullmatch(r'progress: (\d+)/(\d+)', ln): PROG.set(done=int(m[1]), total=int(m[2]), unit='files')
    run([py, repo / 'dm/gfx/tools/shader_split.py', ','.join(map(str, roms)), ','.join(map(str, mains)), tmp / 'pkg'] + [tmp / 'tables' / n for n in names + ('pso_pairs.tsv',)] + ([','.join(map(str, dlc_roms))] if dlc_roms else []),
        logs / 'shader_split.log', env=env, on_line=counted)
    text = (logs / 'shader_split.log').read_text(errors='replace')
    m = re.search(r'shaders: (\d+) \(', text)
    bad = re.search(r'^\[RED\] (\d+) file\(s\) could not be read.*$', text, re.M)   # the split leaves out a file no reader reads (its own red line per file in the log) and goes on
    if bad and not own:   # the profile's tables were split from every file: one that does not read now is a reader change
        raise BuildError(f'{bad.group(0).removeprefix("[RED] ")}; the profile\'s tables hold every file\'s shaders (each file in {logs / "shader_split.log"})')
    if bad: say(f'{bad.group(0)}; the build goes without them (each file in {logs / "shader_split.log"})')
    if m and m.group(1) == '0':   # dmsh_build.py would make no library
        raise BuildError('the shader split found no shaders in main or the RomFS: the game keeps them in a form shader_split.py does not read')
    if not own and derive_dlc and dlc_only_shader_change(g, tmp / 'tables'): promote()
    # pso_pairs.tsv (supplemental BAHS variations): checked like the others where the profile has one; else kept beside the package, where dmsh_build.py reads it too, so padding and
    # the pipeline prebuild use the same pairs
    if own or g.table('pso_pairs.tsv').is_file(): names += ('pso_pairs.tsv',)
    else: shutil.copyfile(tmp / 'tables' / 'pso_pairs.tsv', pkg.with_name('pso_pairs.tsv'))
    if own:   # a derived profile takes the tables as they come
        for n in names: shutil.copyfile(tmp / 'tables' / n, g.table(n))
    else:
        # Data rows byte-identical to the profile's = the same game files the tables were built from; if not, stop (the profile's tables are left alone). Only data rows are compared: a header changes with the shader_split.py version
        diff = [n for n in names if data_rows((tmp / 'tables' / n).read_bytes()) != data_rows(g.table(n).read_bytes())]
        if diff:
            rows = '\n'.join(table_difference(n, g.table(n).read_bytes(), (tmp / 'tables' / n).read_bytes()) for n in diff)
            raise BuildError(f'the split shader tables differ from game profile {g.id}\'s:\n{rows}\nEither these game files are not the versions the profile registers, or shader_split.py now reads them differently and the profile\'s tables were not split again (the split\'s tables: {tmp / "tables"})')
    rmtree(kf); rmtree(outputkf); swap_in(tmp / 'pkg', pkg); rmtree(tmp)
    note = (f'{int(m.group(1)):,} shaders' if m else '') + (f', {bad.group(1)} file(s) unreadable' if bad else '')
    key = fingerprint(*inputs, *split_profile_inputs(g))   # a derived profile's tables are outputs too; the next split checks the profile after they were written
    outputkf.write_text(split_output_key(pkg, g) + '\n')
    kf.write_text(f'{key}\n{note}\n')
    if promoted and dlckey: dlckf.write_text(f'{dlckey}\nDLC-only shader content\n')
    else: rmtree(dlckf)
    PROG.end(note)
    return package_key(pkg)


def shader_keys(g):   # the base shaders dmsh_build.py builds, shared by progress and the stale-library event
    keys = set()
    for ln in g.table('shader_map.tsv').read_text(errors='replace').splitlines():
        c = ln.rstrip('\n').split('\t')
        if len(c) > 4 and c[0].endswith('.bin'): keys.add(c[3])
    return keys


SHADER_MISSING = re.compile(r'\[RED\] (?:shader ([0-9a-f]{16}) \([^)]*\) failed to load:|failed to load shader ([0-9a-f]{16})_b:)')   # what the host logs for a shader its library lacks (nvn_mtl.m)


def seen_listed(g):   # the keys of the profile's shader_seen.tsv
    return set(re.findall(r'^[0-9a-f]{16}\b', g.table('shader_seen.tsv').read_text(errors='replace'), re.M))


def seen_logs(work, app):   # the logs a --seen-shaders build reads: the run command's, the app's and the one before it (only opening the app and quitting in its settings window starts a new log)
    logs = Path(os.path.expanduser('~/Library/Logs/nxnative'))
    return work / 'run/host.log', logs / f'{app:016X}.log', logs / f'{app:016X}.prev.log'


def seen_keys(g, sh, logs):
    """The keys of a partial Mac library: the profile's shader_seen.tsv, those of the last partial build (sh/partial-keys.txt) and those the logs report missing. The file keeps what earlier runs added, as a
    shader that loads no longer shows in the next log. Only keys of the shader map go in: a list or a log of another game version may name others, and the builder knows only the map's. No key at all stops
    the build, as the builder takes no keys for the whole map. Returns the keys, sorted"""
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
    """Progress of dmsh_build.py, from the small JSON file it rewrites at most every half second (DMSH_PROGRESS): the cache check over every key, then the compile, which finishes the keys
    in batches (cache hits and misses alike) and counts them. It used to be read from the files dmsh_build.py wrote, one per shader output, which it no longer leaves on disk"""
    def __init__(self, path): self.path = path

    def __call__(self):
        try: p = json.loads(self.path.read_text())
        except (OSError, ValueError): return None
        cached, misses = p.get('cached', 0), p.get('misses')
        if p.get('step') == 'cache check':
            return {'step': 'cache check', 'done': p['done'], 'total': p['total'], 'unit': 'shaders', 'note': f'{cached:,} cached'}
        return {'step': 'compiling', 'done': p.get('done', 0), 'total': p.get('total', 0), 'unit': 'shaders', 'note': f'{misses:,} not cached' if misses else ''}


def spirv_cross(repo, env=None):   # resolve the same selection the shader tools run, including an executable name on PATH
    env = os.environ if env is None else env
    selected = env.get('SPIRV_CROSS', str(repo / 'dm/gfx/tools/spirv_cross_bin/spirv-cross'))
    return Path(shutil.which(selected, path=env.get('PATH')) or selected)


def toolchain(repo, env=None):   # Metal is a separate Xcode download: its version and SDK must invalidate reuse too
    return (('cmd', ['xcodebuild', '-version']), ('files', [spirv_cross(repo, env)]),
            *(('cmd', ['xcrun', '-sdk', 'macosx', *args]) for args in (['metal', '--version'], ['metallib', '--version'], ['--show-sdk-version'], ['--show-sdk-build-version'])))


def prune_stale_chains(sh, chain):
    """Remove the shader libraries of other chain versions, and their keys, once the build for this one succeeded: sh/cache/b-<old>, sh/cache/b-<old>.img and sh/mac-b-<old>.key.
    A chain change translates every shader again into a new b-<chain>/, so each chain change left another library. The host and the packers read only b-<chain>/,
    shader_regressions.py compares this build with the one before it (already done, and the next chain change compares with this one), and the Metal compile cache (metallib/ in
    sh/cache/cas-<chain>.db, keyed by the MSL) holds what the old libraries compiled. Called only after the libraries, their tables and the key are in place, so a stopped or failed build
    keeps the old ones. Only names of the exact form b-<12 hex digits> go (the key files with .key and a mac- prefix), never another entry. A removal that fails is a [RED] line, not
    an error: the build is done, and the next one tries again. Returns the chain versions removed"""
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
    """Remove the compile cache entries the current chain's last full build did not use, with prune_stale_chains and under the same conditions (issue #1033). Returns the number of shaders removed.
    The other generations of sh/cache go whole (shader_cache.drop_generations): the files of older chains (cas-<chain>.db, a development build's too) and cas.db, the one file of every chain from
    before them, once a target this build compiled nothing for has taken the newest of their Metal compiles. Ocarina of Time's cas.db held about seven chains' compiles: 52 GB, 10 GB of them in use.
    In the chain's own file, the per-shader entries (shader/<digest>) the build did not list go: a shader's digest covers the profile's inputs too, so an edit of the shader map within one chain
    leaves old ones. The digests in use are listed by dmsh_build.py's full build in b-<chain>/_b_cas.txt beside the Mac outputs (every full build writes them); index/ cannot tell, since a cache
    hit does not rewrite it. No list (libraries from before the list): no entry goes. An entry newer than the list stays. Removals that fail are one [RED] line, as for the libraries.
    Logs the space of the sources and reflection: an entry's libraries are the Metal compiles'"""
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
    """The profile's part of the shader step's key: the game and the digest of its shader inputs (dmsh_chain.py --inputs: the shader tables, geo_mesh.tsv, bf_want.tsv, dual_want.tsv and the
    nvn_* / shader_* keys), which the libraries' stamp records and every per-shader cache key already assumes are all a shader build reads of the profile. Hashing every profile file
    instead restored every library from the cache and rewrote the image after an edit of the game's note or prewarm list. Without the digest: every file, as before"""
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py'), '--inputs'], env=env, capture_output=True, text=True)
    words = r.stdout.split() if r.returncode == 0 else []
    if words: return ('shader inputs', g.id, words[-1])
    say(f'[RED] dmsh_chain.py --inputs gave no digest ({(r.stderr.strip().splitlines() or ["exit code " + str(r.returncode)])[-1]}): the shader step is keyed on every profile file, so any profile edit rebuilds it')
    return ('files', profile_files(g))


def stale_shaders(py, repo, env, lib_dir, chain, g):
    """Why cached shader libraries cannot be reused by the build that starts: (reason, old, new, count) or None. 'chain': library folders b-<V> of another translation chain in lib_dir
    have libraries without a completed current build; old is those folders' names, new b-<chain>. A full build writes its stamp only at the end, so a partial current-chain output must
    not hide the old libraries. 'inputs': the current chain's stamp names different profile inputs (dmsh_chain.py --inputs).
    count: base libraries scheduled, once per current shader key, including cache restores, never copies retained in older chains"""
    cur = lib_dir / f'b-{chain}'
    cached = shader_lib.has_library(cur)
    if not (cached and lib_head(cur)):
        other = [old for old in sorted({Path(shader_lib.folder_of(p)) for p in lib_dir.glob('b-*')}) if old.name != cur.name and shader_lib.has_library(old)]
        if other: return ('chain', ', '.join(sorted({d.name for d in other})), f'b-{chain}', len(shader_keys(g)))
    if not cached: return None
    r = subprocess.run([str(py), str(repo / 'dm/gfx/tools/dmsh_chain.py'), '--inputs'], env=env, capture_output=True, text=True)
    new = r.stdout.split()[-1] if r.returncode == 0 and r.stdout.split() else None
    have = lib_head(cur)   # '# chain b-<V> inputs <digest>', None without a stamp
    if new and have and have.split()[-1] != new: return ('inputs', have.split()[-1], new, len(shader_keys(g)))
    return None


def build_shaders(py, repo, sh, xlate, env, logs, g, pkg_key, seen=None):
    """The Mac shader libraries (dmsh_build.py through own_compilers.py) into sh/cache/b-<chain>. Returns the folder the host takes them from (sh/cache, or sh/partial with seen).
    Reused when the shader package (pkg_key, its content) and the chain's tools, the translator's bytes, profile and compilers are as at the last successful build: dmsh_build.py
    would only restore every shader from its cache. The key is sh/mac-b-<chain>.key (not in sh/cache, whose b-* entries dmsh_build.py reads as library folders), taken only while
    the sources and reflection carry a full build's stamp (inputs.tsv) for c1_full.tsv and cb_extent.tsv to name (chain_tables).
    After success the libraries of other chain versions and the cache entries this chain's build did not use are removed (prune_stale_chains, prune_shader_cache), the library becomes
    one file, b-<chain>.img beside the folder's name, read in place (shader_lib.py), and the compile cache is one file per chain, sh/cache/cas-<chain>.db (shader_cache.py), read in
    place and into which a cache folder from before it moves: a reused step touches none of their files.
    seen (--seen-shaders): only those keys, into sh/partial/b-<chain>, emptied first, with its key inside; the full build's libraries, its key and the cache are left as they are, and the
    per-shader cache (DMSH_CAS) is the full build's, so a later full build reuses this work. It gets no stamp and no c1_full.tsv or cb_extent.tsv, on purpose: beside a stamp the host
    would take the tables' silence on a shader the partial build never saw as proof it is safe (no chain_tables, no shader_regressions, no pruning; the host logs the missing stamp
    in red at each start)"""
    log = logs / 'dmsh_build.log'
    PROG.stage('Mac shaders')
    chain = chain_of(repo)
    shader_cache.adopt(sh / 'cache/cas', shader_cache.chain_file(sh / 'cache', chain), say)
    out = sh / 'partial' if seen else sh / 'cache'
    libs, kf = out / f'b-{chain}', out / 'key' if seen else sh / f'mac-b-{chain}.key'
    pso_pairs = g.table('pso_pairs.tsv')
    if not pso_pairs.is_file(): pso_pairs = sh / 'pso_pairs.tsv'   # the split's copy is a shader input too (vs_pad)
    PROG.set(step='checking the inputs')
    key = fingerprint(pkg_key, ('files', chain_files(repo)), translator_key(xlate), shader_profile_key(py, repo, env, g), ('files', [pso_pairs]), *toolchain(repo, env),
                      {k: env.get(k) for k in ('XLATE_DEBUG', 'XLATE_REDUCED')}, *((seen,) if seen else ()), env=env)
    note = reused(kf, key)
    # the libraries are the last build's, so are tables that name their stamp; a missing or stale one is made from that build's outputs, and without them the step runs again
    if shader_lib.has_library(libs) and note is not None and (seen or chain_tables(py, repo, sh, env, logs, g, chain)):
        if not seen: prune_stale_chains(sh, chain); prune_shader_cache(sh, chain); shader_lib.pack(libs, say)   # a folder from before this becomes the one file here
        PROG.end(f'{note}, reused (inputs unchanged)'); return out
    rmtree(kf)
    if seen: rmtree(out)   # what a run leaves is what it built; its keys are in partial-keys.txt
    benv = dict(env, XLATE=str(xlate))
    if ALLOW_OTHER_METAL: benv['DM_ALLOW_OTHER_METAL'] = '1'
    if seen: benv['DMSH_CAS'] = str(sh / 'cache')   # the full build's folder: this chain's file there
    benv['DMSH_TABLE_MEMO'] = str(sh / 'cache')   # c1_guard.py / cb_extent.py keep each shader's answer there: a build that restores every shader makes the tables in seconds
    prog = sh / 'progress.json'; rmtree(prog); benv['DMSH_PROGRESS'] = str(prog)   # dmsh_build.py's progress (ShaderWatch)
    try: PROG.watch(ShaderWatch(prog))
    except Exception: pass   # progress never stops the build
    try: stale = None if seen else stale_shaders(py, repo, env, sh / 'cache', chain, g)
    except Exception: stale = None   # a note never stops the build
    if stale:
        reason, old, new, n = stale
        PROG.event('shaders_stale', reason=reason, old=old, new=new, count=n)
        say(f'[NOTE] cached shader libraries cannot be reused ({old} -> {new}): ' + ('the translation chain changed' if reason == 'chain' else 'the game profile\'s shader inputs changed')
            + f'; {n:,} base libraries scheduled (including cache restores; later builds reuse as before)')
    if not seen and g.table('shader_seen.tsv').is_file():
        say(f'building the whole shader map, {len(shader_keys(g)):,} shaders: hours the first time. --seen-shaders builds only the {len(seen_listed(g)):,} the profile\'s playtests used, in minutes; areas they do not cover then draw incomplete')
    if not seen: shader_lib.remove_image(libs)   # the run's folder replaces the library: a stopped build keeps no old shaders under the same chain, as dmsh_build.py removes a folder's
    run([py, repo / 'dm/gfx/tools/own_compilers.py', sh / 'pkg', out, *(seen or ())], log, env=benv)
    PROG.watch(None)
    if not seen and not chain_tables(py, repo, sh, env, logs, g, chain): raise BuildError(f'no stamped Mac shader outputs in {libs} (inputs.tsv): dmsh_build.py did not finish a full build')
    n = sum(1 for _ in shader_lib.view(libs).glob('*_b.metallib')) if shader_lib.exists(libs) else 0   # the image dmsh_build.py writes (a folder from an older chain)
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
        rmtree(sh / 'partial')   # the whole library replaces a partial one; partial-keys.txt stays
    return out


def check_metal(py, repo, env):
    """The shaders are built only with the Metal compiler they were tested with (dmsh_chain.lock), unless ALLOW_OTHER_METAL: checked before the shader steps, so a Mac with
    another Xcode stops in seconds instead of after the shader split"""
    if ALLOW_OTHER_METAL: return   # the shader step says which compiler it used (own_compilers.py's note line)
    r = subprocess.run([py, str(repo / 'dm/gfx/tools/own_compilers.py'), '--check-metal'], capture_output=True, text=True, env=env, errors='replace')
    if r.returncode == 0: return
    msg = (r.stdout.strip() or r.stderr.strip() or f'own_compilers.py --check-metal exit code {r.returncode}').removeprefix('[RED] ')
    raise BuildError(re.sub(r'set DM_ALLOW_OTHER_METAL=1 \([^)]*\)', 'ALLOW_OTHER_METAL = True (--allow-other-metal)', msg))


def shader_regressions(py, repo, lib_dir, chain, logs):
    """dm/gfx/tools/shader_regressions.py on the Mac libraries (dmsh_build.py writes its build report there): shaders that translated in the previous build and fail with this chain
    stop the build, unless ALLOW_SHADER_REGRESSIONS"""
    log = logs / 'shader_regressions.log'
    if not (shader_lib.view(lib_dir / f'b-{chain}') / '_b_build.json').is_file(): return   # a full dmsh_build.py run always writes it
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


def env_file_pairs(path):   # an environment file's switches (one KEY=VALUE per line, # starts a comment), read as the launcher reads them
    out = {}
    try: text = Path(os.path.expanduser(path)).read_text(errors='replace')
    except OSError as e: raise BuildError(f'cannot read the environment file (--env-file / DM_ENV_FILE): {e}') from e
    for ln in text.splitlines():
        k, eq, v = ln.strip().partition('=')
        if k.strip() and eq and not k.startswith('#'): out[k.strip()] = v
    return out


def build_env_file(work, env_file, motion):
    """The environment file the build bakes in (the Mac app's, the Mac run command's switches): the --env-file / DM_ENV_FILE one as it is, or, for a build
    without motion (MOTION / --no-motion), its switches plus DM_MOTION=0 in <build folder>/build_env.txt"""
    if motion: return env_file
    pairs = env_file_pairs(env_file) if env_file else {}
    pairs['DM_MOTION'] = '0'
    f = work / 'build_env.txt'
    f.write_text(''.join(f'{k}={v}\n' for k, v in pairs.items()))
    return str(f)


def mac_host(py, repo, work, env, logs, g):
    """The stage "Mac host": the Mac host for the profile (build_mac_host, into <build folder>/tools/dmhost; DM_GAMES_DIR / DM_GAMES_FIRST in env pick a derived profile) and the
    button-icon font the host gives the game for the Nintendo extension shared font and the shared-font stand-ins (make_shared_fonts.py: one per type, into <build folder>/font, made
    again when the tools or the source font changed). Returns (host, font folder)"""
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


def mac_run(work, game, patched, host, font, msl, aoc, lang, env, logs, g, env_file='', partial=False):
    """The command that runs the game in a window with sound after a development build (PLATFORM 'mac'): its save and temporary storage in <build folder>/run (kept from run to run),
    the unpacked RomFS, the Mac shader libraries (msl), the DLC, the profile's shader map (a host outside dm/host does not find it beside itself; c1_full.tsv and cb_extent.tsv are
    read beside it), the game language, and the switches of an environment file (DM_ENV_FILE / --env-file). partial (--seen-shaders): the pipeline bases go into an archive of their
    own, as the prebuild's stamp does not name the library (the whole library would find its bases done), and the command keeps its log, which the next partial build reads"""
    rd = work / 'run'
    for d in ('save', 'temp'): (rd / d).mkdir(parents=True, exist_ok=True)
    renv = {'DM_ROOT': work, 'DM_ROMFS': rfs.source(game), 'DM_SAVE': rd / 'save', 'DM_TEMP': rd / 'temp', 'DM_MSL_DIR': msl, 'DM_FONT_DIR': font, 'DM_OBSOLETE': work / OBSOLETE}   # the pipeline archive's unused files go there
    if aoc: renv['DM_AOC_DIR'] = aoc
    renv['DM_NVN_SHMAP'] = g.table('shader_map.tsv')
    if partial:
        renv['DM_PSO_ARCHIVE'] = rd / 'pso_archive_partial'
        (rd / 'pso_archive_partial/prebuilt.tsv').unlink(missing_ok=True)   # the keys may have grown: the prebuild walks the pairs again, and skips what the archive holds
    renv.update(DM_LANG=lang, DM_WINDOW=1, DM_AUDIO=1)
    if env_file: renv.update(env_file_pairs(env_file))
    if g.table('shader_pairs.tsv').is_file():
        if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file(): renv['DM_PSO_PAIRS'] = work / 'shader/pso_pairs.tsv'   # split_shaders' copy
        # The pipeline prebuild (docs/development.md, Hybrid pipelines): the pulling base of every shader pair into run/pso_archive, so the first run's draws have stand-ins instead
        # of waiting for the Metal compiler. The host's tool mode skips it when pso_archive/prebuilt.tsv matches (chain version, depth mode, pair list). Then the prewarm lists'
        # pipelines (the profile's and run/save's pso_user.tsv), so the first run's prepare step finds them in the archive (seconds once they are there). It takes the depth mode
        # from the profile's depth_mode key (default gl, NVN's); with a wrong one the game builds its bases again when it runs
        PROG.add(1); PROG.stage('Pipeline prebuild', step='building')
        penv = dict(env, **{k: str(v) for k, v in renv.items()}, DM_PSO_PREBUILD='tool')
        if g.get('depth_mode', 'gl') == 'zero': penv['DM_PSO_PREBUILD_DEPTH'] = 'zero'   # a game that sets the near = 0 depth mode (the profile says so: the host learns it only at run time)
        run([host], logs / 'pso_prebuild.log', env=penv, cwd=rd)
        PROG.end()
    q = lambda v: shlex.quote(str(v))
    return f'cd {q(rd)} && ' + ' '.join(f'{k}={q(v)}' for k, v in renv.items()) + f' {q(host)} {q(patched)}' + (f' 2>&1 | tee {q(rd / "host.log")}' if partial else '')


MTL_ARCH_M = '''#import <Metal/Metal.h>
int main(void) { @autoreleasepool { id<MTLDevice> d = MTLCreateSystemDefaultDevice(); if (!d) return 1; printf("%s\\n", d.architecture.name.UTF8String); return 0; } }
'''


def mac_gpu_arch(work, env, logs):
    """This Mac's GPU architecture as Metal names it (MTLDevice.architecture, e.g. applegpu_g14s for an M2 Max): the archive file the host opens in an app's pso/ (pso_bundle_open)"""
    src, exe = work / 'tools/mtl_arch.m', work / 'tools/mtl_arch'
    if not (exe.is_file() and src.is_file() and src.read_text() == MTL_ARCH_M):
        src.parent.mkdir(parents=True, exist_ok=True); src.write_text(MTL_ARCH_M)
        run(['xcrun', 'clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', src, '-o', exe], logs / 'mtl_arch.log', env=env)
    r = subprocess.run([str(exe)], capture_output=True, text=True)
    arch = r.stdout.strip()
    if r.returncode or not re.fullmatch(r'applegpu_\w+', arch): raise BuildError(f'cannot tell this Mac\'s GPU architecture ({(r.stderr or r.stdout).strip() or r.returncode}); --gpu names it, none builds the app without a pipeline archive')
    return arch


def mac_pso(py, repo, work, lib, g, gpus, env, logs):
    """PLATFORM 'mac-app', an NVN game with shader pairs: its pipeline archive made offline for this Mac's GPU (dm/gfx/tools/pso_offline.py --platform macos, metal-tt), which the app carries as
    Resources/pso and the launcher hands to the host as DM_PSO_BUNDLE (docs/development.md, The pipeline archive made offline): the app's first start builds no base,
    and the bases do not go into the player's ~/Library/Application Support. gpus: --gpu / PSO_GPUS (empty: this Mac's GPU; none: no archive, the app
    builds the bases on its first start). Made again only when its inputs change (pso_offline.py's key). Returns the folder, or None"""
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
    cmd = [py, repo / 'dm/gfx/tools/pso_offline.py', '--game', g.id, '--msl', lib, '--out', out, '--work', work / 'pso/mac-work', '--gpu', arch, '--platform', 'macos']
    if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file():   # the split's copy, as the app gets it (--pso-pairs)
        cmd += ['--pairs', g.table('shader_pairs.tsv'), '--pairs', work / 'shader/pso_pairs.tsv']
    if run(cmd, logs / 'pso_offline_mac.log', env=env, fatal=False):   # an optimization: the app builds the bases on its first start without it
        shutil.rmtree(out, ignore_errors=True)
        PROG.end('failed, the app is packed without one', warn=True)
        say(f'[RED] pipeline archive: pso_offline.py failed (see {logs / "pso_offline_mac.log"}); the app is packed without one and builds its pipeline bases on its first start (--gpu none skips this step)')
        return None
    if not any(out.glob('*.mtl4a')):   # no pair (a header-only table, #338), or every eligible pair declares function constants (the host specializes them on the device, rest_pairs.tsv)
        PROG.end('no base to bundle'); say('pipeline archive: none (no shader pair, or every eligible one has function constants): the app builds what it needs on its first start'); return None
    PROG.end(f'{arch}, {amount(sum(f.stat().st_size for f in out.iterdir() if f.is_file() and "-prewarm-" not in f.name), "B")}')   # the archives, not the prewarm pipelines an earlier build linked there
    return out


def drop_prewarm(pso):   # the prewarm pipelines in an archive folder, with their indexes (.keys) and stamp: an earlier build's, when this one makes none
    for old in pso.glob('*-prewarm-*'): old.unlink()
    (pso / 'prewarm.tsv').unlink(missing_ok=True)


def mac_prebake(py, repo, work, msl, g, host, pso, env, logs):
    """PLATFORM 'mac-app', --prebake (on by default): what the app's first start would still build after the pipeline archive, made here and added to the archive folder pso, so the first
    start finds it (nvn_mtl.m pso_bundle_open_prewarm) instead of building it before the first frame: the prewarm list's pipelines (pso_manifest.tsv: the R rows' bases with a vertex layout,
    the M rows' mesh pipelines) and the pairs pso_offline.py left to the device (rest_pairs.tsv). They need the descriptor the host builds from a row's pipeline key and the shader's reflection,
    which only the host knows, so the host makes them itself: its prebuild tool in the bundle form (DM_PSO_PREBUILD=tool with DM_PSO_PREBUILD_OUT; depth mode from the profile's depth_mode,
    as pso_offline.py gets it). The tool captures into a work folder, one file per flush period (Metal's serializer store fills past about 2,000 captures), which is kept for the next build only
    while the inputs are the same (an interrupted run goes on from there), and puts <GPU>-prewarm-<NN>.mtl4a, each with its index <GPU>-prewarm-<NN>.keys, and prewarm.tsv in the output folder;
    those are linked into pso, where pack_app.py takes them. The files are for this Mac's GPU, so for every other GPU --gpu names (pso holds a pair archive for it) the tool also writes each
    period as a pipelines script (prewarm-<NN>.mtl4-json), which other_gpu_prewarm compiles with metal-tt into <GPU>-prewarm-<NN>.mtl4a. Made again only when its key changes (the list,
    the libraries, the pair archive, the GPUs, the host, the depth mode, the system build). Returns pso; an optimization, so a failure leaves the app without them (it builds them on its first start)"""
    if pso is None: return pso   # no pair archive (none for this Mac's GPU, or --gpu none): the app has no bundle to put them in
    manifest = g.table('pso_manifest.tsv')
    text = manifest.read_text() if manifest.is_file() else ''
    rows = sum(ln.startswith(('R\t', 'M\t')) for ln in text.splitlines())
    def none(why):   # the app goes without them: none of an earlier build's stay in pso (pso_offline.py leaves an architecture's there)
        say(why); drop_prewarm(pso); return pso
    if not rows: return none('prewarm pipelines: none (the profile\'s pso_manifest.tsv has no R or M rows)')
    try: arch = mac_gpu_arch(work, env, logs)
    except BuildError as e: return none(f'[RED] prewarm pipelines: {e}; the app builds them on its first start')
    if not (pso / f'{arch}.mtl4a').is_file(): return none(f'prewarm pipelines: the pipeline archive is not for this Mac\'s GPU ({arch}): the app builds them on its first start')
    PROG.add(1); PROG.stage('Prewarm pipelines', step=f'{arch}, {rows:,} rows (host, minutes to hours when its inputs changed)')
    out, stage = work / 'pso/mac-prewarm', work / 'pso/mac-prewarm-work'
    zero = g.get('depth_mode', 'gl') == 'zero'
    others = sorted(f.stem for f in pso.glob('applegpu_*.mtl4a') if '-prewarm-' not in f.name and f.stem != arch)   # the other GPUs --gpu named: their pipelines come from the scripts (metal-tt)
    lib = shader_lib.view(msl / f'b-{chain_of(repo)}')   # the full build packs this folder: its input stamp remains inside the image
    digest = hashlib.sha256()
    for part in (arch, chain_of(repo), 'zero' if zero else 'gl', text, Path(host).read_bytes() if Path(host).is_file() else str(host),
                 (lib / 'inputs.tsv').read_text() if (lib / 'inputs.tsv').is_file() else '',
                 (work / f'pso/mac-work/key.{arch}').read_text() if (work / f'pso/mac-work/key.{arch}').is_file() else '',
                 subprocess.run(['sysctl', '-n', 'kern.osversion'], capture_output=True, text=True).stdout):
        digest.update(part if isinstance(part, bytes) else str(part).encode()); digest.update(b'\0')
    key = digest.hexdigest()   # this Mac's files; the other GPUs' are keyed by it and their list (keyf)
    keyf, stage_key = work / 'pso/mac-prewarm.key', stage / 'key'
    if keyf.is_file() and keyf.read_text().split() == [key, ','.join(others) or '-'] and list(out.glob(f'{arch}-prewarm-*.mtl4a')):
        say('prewarm pipelines: reused (inputs unchanged)')
    else:
        keyf.unlink(missing_ok=True)
        if not (out.is_dir() and (out / 'key').is_file() and (out / 'key').read_text().strip() == key and list(out.glob(f'{arch}-prewarm-*.mtl4a'))):   # this Mac's files are not this key's
            shutil.rmtree(out, ignore_errors=True)
            if not (stage_key.is_file() and stage_key.read_text().strip() == key): shutil.rmtree(stage, ignore_errors=True)   # an unfinished run of other inputs: its captures are not this build's
            stage.mkdir(parents=True, exist_ok=True); stage_key.write_text(key + '\n')
            renv = dict(env, DM_PSO_PREBUILD='tool', DM_PSO_ARCHIVE=str(stage), DM_PSO_PREBUILD_OUT=str(out), DM_PSO_BUNDLE=str(pso), DM_PSO_MANIFEST=str(manifest),
                        DM_MSL_DIR=str(msl), DM_NVN_SHMAP=str(g.table('shader_map.tsv')))
            if zero: renv['DM_PSO_PREBUILD_DEPTH'] = 'zero'
            if not g.table('pso_pairs.tsv').is_file() and (work / 'shader/pso_pairs.tsv').is_file(): renv['DM_PSO_PAIRS'] = str(work / 'shader/pso_pairs.tsv')
            for k in ('DM_WINDOW', 'DM_GPU_CAPTURE', 'MTL_CAPTURE_ENABLED', 'DM_SAVE', 'DM_PSO_USER', 'DM_PSO_PREBUILD_WARM'): renv.pop(k, None)   # the profile's list alone, not a player's
            if run([host], logs / 'pso_prewarm_mac.log', env=renv, fatal=False) or not list(out.glob(f'{arch}-prewarm-*.mtl4a')):
                shutil.rmtree(out, ignore_errors=True)
                PROG.end('failed, the app is packed without them', warn=True)
                say(f'[RED] prewarm pipelines: the host\'s prebuild tool failed (see {logs / "pso_prewarm_mac.log"}); the app is packed without them and builds them on its first start (--no-prebake skips this step)')
                drop_prewarm(pso)
                return pso
            (out / 'key').write_text(key + '\n')
            shutil.rmtree(stage, ignore_errors=True)   # the work folder is kept only while a run is unfinished
        if other_gpu_prewarm(repo, out, arch, others, logs, lib if lib.is_dir() else lib.with_name(lib.name + '.img'), env):
            keyf.write_text(f'{key} {",".join(others) or "-"}\n')   # a period left out for a GPU: no key, so the next build tries those GPUs again (this Mac's files are kept)
    sys.path.insert(0, str(repo / 'dm/gfx/tools')); import pso_offline as po
    po.publish_prewarm(pso, {f.name: f for f in out.iterdir() if f.suffix in ('.mtl4a', '.keys', '.tsv')})   # the scripts and their libraries stay in the work folder
    PROG.end(f'{len(list(out.glob("*.mtl4a")))} files, {amount(sum(f.stat().st_size for f in out.glob("*.mtl4a")), "B")}')
    return pso


def prewarm_script_libraries(out, scripts, lib_dir, po):
    """The libraries the prewarm pipelines scripts name, as files for metal-tt, in out/libraries, and the scripts' paths pointed at them (po.script_libraries,
    share: pso_offline.py --prewarm-host). po: the pso_offline module. Returns that folder, or raises BuildError naming a library not found"""
    try: return po.script_libraries(scripts, lib_dir, out / 'libraries')
    except LookupError as e: raise BuildError(str(e)) from e


def other_gpu_prewarm(repo, out, arch, others, logs, lib_dir, env):
    """The prewarm pipelines for the GPUs --gpu named besides this Mac's (arch): the host's tool wrote each flush period also as a pipelines script (prewarm-<NN>.mtl4-json, prewarm_script), which
    metal-tt builds for each of them as <GPU>-prewarm-<NN>.mtl4a, as pso_offline.py builds the pairs' bases, with this Mac's index of the same period (the same pipelines, so the same keys).
    A period whose build fails for a GPU is left out for it (that GPU's first start builds those pipelines), the others are kept. Returns False when one was left out, so the stage keeps
    no key and the next build tries again"""
    for old in out.glob('applegpu_*-prewarm-*'):   # the other GPUs' files of an earlier build, made again here
        if not old.name.startswith(f'{arch}-'): old.unlink()
    if not others: return True
    scripts = sorted(out.glob('prewarm-*.mtl4-json'))
    if not scripts: say(f'[RED] prewarm pipelines for {", ".join(others)}: the host wrote no pipelines script; those GPUs build them on their first start'); return False
    sys.path.insert(0, str(repo / 'dm/gfx/tools')); import pso_offline as po
    osmin = po.os_min_default('macos')
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
            if keys.is_file():   # with its index, the file leads the lookup for them over a device file an earlier build left (pso_bundle_open_prewarm)
                k = dst.with_suffix('.keys'); k.unlink(missing_ok=True); os.link(keys, k)
        if failed: say(f'[RED] prewarm pipelines for {gpu}: metal-tt failed on {", ".join(failed)} (see {logs}/pso_prewarm_{gpu}_*.log); that GPU builds those periods\' pipelines on its first start')
        if made: say(f'prewarm pipelines for {gpu}: {len(made)} of {len(scripts)} files, {amount(sum(f.stat().st_size for f in made), "B")} (metal-tt)')
    shutil.rmtree(out / 'libraries', ignore_errors=True)
    return ok


def mac_app_name(name):   # the app's file name: the game name with the characters Finder shows otherwise (':' and '/') as ' - '
    return re.sub(r'\s*[:/]\s*', ' - ', name).strip(' .-') or 'nxnative'


def mac_app(py, repo, out, app, work, game, patched, host, font, msl, aoc, lang, g, name, version, bundle_id, icon, sign, env, logs, env_file='', gpus='', control=None, prebake=False):
    """The last stage of PLATFORM 'mac-app': pack the Mac app (dm/mac/pack_app.py) from what the build made: the Mac host and font (mac_host), the built version's RomFS and rewritten
    modules, the Mac shader libraries (msl: <shader>/cache, whose b-<chain> goes in), the DLC, the profile's tables (with the shader pairs the pipeline prebuild reads), the pipeline
    archive made offline for this Mac (mac_pso; gpus: --gpu / PSO_GPUS), the launcher settings and an environment file.
    The save and the log go to the player's Library, keyed by the title ID (app); a new app replaces the old one only when it is complete. control: the control.nacp that lists the
    game's languages (languages_control), by default the built version's"""
    lib = msl / f'b-{chain_of(repo)}'
    pso = mac_pso(py, repo, work, lib, g, gpus, env, logs) if g.table('shader_pairs.tsv').is_file() else None   # a stage of its own, before the app's
    if prebake and pso: pso = mac_prebake(py, repo, work, msl, g, host, pso, env, logs)   # --prebake: a stage of its own, after the archive
    elif pso: drop_prewarm(pso)   # what an earlier build with --prebake left
    PROG.stage('Mac app', step='starting')
    # The launcher settings (dm/games/settings.py): the app's settings window and Mac front ends show them (docs/frontends.md)
    settings = work / 'app/launcher.json'; settings.parent.mkdir(parents=True, exist_ok=True)
    run([py, repo / 'dm/games/settings.py', g.id, patched, control or game / 'control.nacp', settings], logs / 'mac_settings.log', env=dict(env, DM_LANG=lang))
    cmd = [py, repo / 'dm/mac/pack_app.py', out, '--host', host, '--patched', patched, '--romfs', rfs.source(game), '--msl', lib, '--tables', g.table('shader_map.tsv').parent,
           '--font', font, '--title-id', f'{app:016X}', '--name', name, '--bundle-id', bundle_id, '--lang', lang, '--sign', sign or '-']
    split_pairs = work / 'shader/pso_pairs.tsv'   # the split's copy of pso_pairs.tsv, for a profile without the table (as mac_run's DM_PSO_PAIRS)
    for opt, v in (('--version', version), ('--category', g.get('app_category')), ('--marker', g.get('romfs_marker')), ('--aoc', aoc), ('--icon', icon),
                   ('--pso', pso), ('--pso-pairs', split_pairs if not g.table('pso_pairs.tsv').is_file() and split_pairs.is_file() else None),
                   ('--env-file', Path(os.path.expanduser(env_file)).resolve() if env_file else None), ('--settings', settings),
                   ('--control-nacp', next((p for p in (control, game / 'control.nacp') if p and Path(p).is_file()), None))):
        if v: cmd += [opt, v]
    unchanged = False
    def line(ln):
        nonlocal unchanged
        if ln.startswith('step: '): PROG.set(step=ln[6:].strip())
        if ln.startswith('app: ') and ln.endswith(', reused (inputs unchanged)'): unchanged = True
    run(cmd, logs / 'mac_app.log', env=env, on_line=line)
    if not (out / 'Contents/MacOS/dmhost').is_file(): raise BuildError(f'no app at {out}')
    PROG.end(f'{out.name}, ' + ('reused (inputs unchanged)' if unchanged else amount(tree_size(out)[1], 'B')))


def find_repo(here):   # this file lives in the repository's scripts/; walk up to the level that has dm/tools/patch_nso.py
    for d in here.parents:
        if (d / 'dm/tools/patch_nso.py').exists(): return d
    raise BuildError(f'cannot find the repository root above {here} (no dm/tools/patch_nso.py)')


def parse_args(argv):
    """Command-line options: when given they override "Settings"; F5 in Spyder passes none, so "Settings" is used as is. -h shows the help"""
    import argparse
    ap = argparse.ArgumentParser(prog='build_app.py',
        description='Build The Legend of Zelda: Ocarina of Time for this Mac from its NUP: a Mac app, or a development build with the command that runs it.',
        epilog='example: python3 scripts/build_app.py ~/games/oot/"Ocarina of Time [0400486024dec000][v131072].nup"')
    ap.add_argument('nup', nargs='?', help='the game\'s NUP container (a .nup file), or the folder that holds it (default: the NUP setting, else asked)')
    ap.add_argument('-p', '--package', help='version built, e.g. v131072, or v0 for the base game (default: the newest in the NUP)')
    ap.add_argument('-o', '--out', help='--platform mac-app: the output app, a path ending in .app (default: <game name>.app beside the build folder)')
    ap.add_argument('--app-name', help='the app\'s name (default: the game name in the NUP\'s control data, else the profile\'s)')
    ap.add_argument('--bundle-id', help='the app\'s bundle ID (default: org.nxnative.<title ID in lowercase>)')
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
    """The image an icon option or setting names, None when it is empty. Checked when the build starts, not after the game is unpacked"""
    if not value: return None
    p = Path(os.path.expanduser(value)).resolve()
    if not p.is_file(): raise BuildError(f'no such icon file ({option}): {p}')
    return p


def app_icon(given, spare, *dirs):
    """The image the app is given, and whether it is the fallback: --icon / ICON (given), else the game icon pack_nup.py saved from the NUP (icon.jpg; in dirs, the built version's
    first, then the base game's), which a NUP may lack, else --fallback-icon / FALLBACK_ICON (spare), which never replaces a game's own. None without any"""
    own = next((d / 'icon.jpg' for d in dirs if (d / 'icon.jpg').exists()), None)
    return given or own or spare, bool(spare and not (given or own))


def say_game(app, name, version, bundle_id, out, icon):
    """Which game the app is, once its name, version and bundle ID are settled (before the shader stages): the event game for a front end (json mode; the title ID as DMTitleID has it,
    out = the Mac app; icon = the image the app is given: --icon / ICON, else the unpacked icon.jpg, else --fallback-icon / FALLBACK_ICON; None without one), then the message for people"""
    PROG.event('game', title_id=f'{app:016X}', name=name, version=version, bundle_id=bundle_id, out=str(out) if out else None, icon=str(icon) if icon else None)
    say(f'app name: {name}; version: {version}; bundle ID: {bundle_id}' + (f'; app: {out}' if out else ''))


def report_fix_coverage(patched, mods, reg):
    """What the modules of builds the profile does not register kept of their registered builds' cuts: the rewriter's <module>.match.tsv (patch_nso.py) lists the cuts of those
    builds and each function patch's result; a game patch not applied, or registered cuts with no function patch at all, is a red line (the game may stop where they were needed).
    SDK-wide patches (sdk_patches.py) are listed as applied or not, without red: a module of a game without virtual address memory needs none"""
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
    """Keep a derived build's warnings visible at the end: what it uses of the registered profile reg, and what it lacks"""
    if not derived: return
    disp = g.builds()[pkg_ver]['display']
    known = {(m, b) for row in reg.builds().values() for m, b in row.items() if m != 'display'}   # unknown builds do not inherit fixed addresses; opt-in function matches are reported by the rewriter
    other = [m for m, b in g.builds()[pkg_ver].items() if m != 'display' and (m, b) not in known]
    registered_builds = ", ".join(f'{v} {b["display"]}' for v, b in reg.builds().items())
    if g.builds() == reg.builds():
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
    mac_pack = platform == 'mac-app'   # the Mac app (mac_app); else the development build, which ends with the run command (mac_run)
    seen_shaders = SEEN_SHADERS if args.seen_shaders is None else args.seen_shaders
    if args.mac_sign and not mac_pack: raise BuildError('--mac-sign signs the Mac app: it needs --platform mac-app')
    if args.prebake and not mac_pack: raise BuildError('--prebake makes the Mac app\'s prewarm pipelines: it needs --platform mac-app')
    if not apple_silicon(): raise BuildError('the game runs in the Mac host, which runs the game\'s own arm64 code natively: it needs a Mac with Apple silicon, and this Mac has an Intel processor')
    check_native_python()
    given = Path(os.path.expanduser(args.nup or NUP or ask('NUP file, or the folder that holds it: ', 'the NUP'))).resolve()
    if not given.exists(): raise BuildError(f'no such file or folder: {given}')
    nup = nup_file(given)
    nup_dir = nup.parent   # the build folder goes there by default
    global NUP_FOLDER; NUP_FOLDER = nup_dir
    user_icon = icon_file(args.icon or ICON, '--icon / ICON'); spare_icon = icon_file(args.fallback_icon or FALLBACK_ICON, '--fallback-icon / FALLBACK_ICON')
    sys.path.insert(0, str(repo / 'dm/games')); import game as gamemod   # game profiles
    sys.path.insert(0, str(repo / 'dm/tools')); global nacp; import nacp   # the game name in the NUP's control data
    lang_in = args.language or LANGUAGE   # checked against the game's languages once its control data is unpacked
    want_lang = language_code(lang_in) if lang_in else 'en-US'
    if not want_lang: raise BuildError(f'unknown game language {lang_in!r}; give one of {language_list([c for c, _ in nacp.LANGUAGES])}, by code or by name')
    env = tool_env()
    env.setdefault('J', str(os.cpu_count() or 8))   # shader_split.py's and dmsh_build.py's workers (their default is 8)
    py = sys.executable
    app, fam = scan_nup(nup)
    say(f'a NUP: {nup.name}, {app:016X}, {len(fam["updates"])} update(s), {len(fam["dlc"])} DLC')
    if fam['base'] is None: say(pack_nup.NO_BASE)
    own = own_profile_folders(args, nup_dir, app)   # this script's own derived profiles: never taken as registered ones, wherever DM_GAMES_DIR puts them
    g = gamemod.by_title(app, own)
    if g is None: raise BuildError(f'no game profile for {app:016X}: this repository builds The Legend of Zelda: Ocarina of Time ({gamemod.DEFAULT}) only')
    say(f'game profile: {g.id}')
    if seen_shaders and not g.table('shader_seen.tsv').is_file(): raise BuildError(f'--seen-shaders: game profile {g.id} has no list of the shaders its playtests used ({g.table("shader_seen.tsv")}): build the whole map, without this option')
    # The newest version in the NUP, or the one asked for. The profile is used as it is for a version it registers when every version its shader tables were made from is there;
    # any other version (no update, another update) is built from a profile derived from it (derive_profile), with its own shaders
    builds, need = g.builds(), g.words('shader_versions')
    have, derived = nup_versions(fam), False
    pkg_ver = args.package or PACKAGE or max(have, key=vnum)
    if pkg_ver not in have: raise BuildError(f'the version to build (-p / PACKAGE) is not in the NUP, which holds {", ".join(sorted(have, key=vnum))}')
    if pkg_ver not in builds:
        registered_builds = ", ".join(f'{v} {b["display"]}' for v, b in builds.items())
        derived = f'{pkg_ver} is not a version game profile {g.id} registers ({registered_builds})'
    elif not all(v in have for v in need): derived = f'the NUP has no {", ".join(v for v in need if v not in have)}, whose files game profile {g.id}\'s shader tables were made from'
    if derived: say(f'{derived}: building {pkg_ver} from a profile derived from it')
    versions = [pkg_ver] if derived else need + ([pkg_ver] if pkg_ver not in need else [])
    work = build_folder(args, nup_dir, g.id); BUILD_FOLDERS.add(work)
    out_app = Path(os.path.expanduser(args.out or OUT_APP)).resolve() if mac_pack and (args.out or OUT_APP) else None   # by default beside the build folder, named after the game (below)
    if out_app and out_app.suffix != '.app': raise BuildError(f'the Mac app (-o / OUT_APP) must end in .app: {out_app}')
    say('Mac app: the Mac shaders and the Mac host, packed into a Mac app' if mac_pack else 'development build: the Mac shaders and the Mac host, then the command that runs the game')
    say(f'build folder (the unpacked game files in extract/, kept for the next build): {work}')
    xlate = repo / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
    # DM_GAME picks the profile; DM_ROOT / WORK point at the build folder: the tools' default paths and temporary directories all stay inside it
    env.update(DM_GAME=g.id, DM_ROOT=str(work), WORK=str(work / 'tmp'))
    logs = work / 'logs'
    check_prereqs(env, xlate)
    picks = pick_packages(fam, have, versions)

    ex = work / 'extract'; ex.mkdir(parents=True, exist_ok=True); (work / 'tmp').mkdir(exist_ok=True)
    gb = sum(pack_nup.romfs_bytes(fam['nup'], u or b, b) for b, u in picks.values()) / 2**30 + 6   # every image is complete; shader cache + slack
    if free_gb(work) < gb and not all((ex / v).exists() for v in versions): raise BuildError(f'less than {gb:.0f} GB free on the volume of {work}')
    PROG.plan(len(versions) + bool(fam['dlc']) + 4 + mac_pack)   # unpack (each version, the DLC), rewrite, shader split, Mac shaders, Mac host, and the Mac app; the stages a build may skip add themselves
    dirs = {}
    for ver in versions:   # each version streams directly into its own image
        b, u = picks[ver]
        dirs[ver] = extract_nup(ex, ver, u or b, b, app, fam['nup'])
    check_formats(dirs)
    bad = [] if derived else [f'{v}: {x}' for v in versions for x in build_mismatch(dirs[v], v, g)]   # every module's build ID must be the one the profile registers for its version
    if bad:   # files of other builds under a registered version's tag: built like any other version
        derived = f'not the builds game profile {g.id} registers: {"; ".join(bad)}'
        say(f'{derived}: building {pkg_ver} from a profile derived from it')
        versions, dirs = [pkg_ver], {pkg_ver: dirs[pkg_ver]}
    reg = g   # the profile as registered (a derived build reports what it lacks against it)
    pdir = profile_folder(args, work)
    if derived:   # every tool (game.py, make, the packers) takes the derived profile first (DM_GAMES_FIRST)
        disp = derive_profile(g, pdir / g.id, pkg_ver, dirs[pkg_ver], g.words('modules'))
        profiles_first(env, pdir)
        g = gamemod.load(g.id)
        say(f'derived game profile for {pkg_ver} ({disp}): {g.dir}')
    sv = versions if derived else need   # the versions whose files the shaders come from
    game = dirs[pkg_ver]
    check_metal(py, repo, env)   # before the shader split, which takes minutes
    aoc = extract_aoc(work, app, fam)
    dlc_roms = sorted(p for p in aoc.iterdir() if p.is_dir() or p.suffix == '.img') if aoc else []
    base = next((dirs[v] for v in versions if picks[v][1] is None), dirs[versions[0]])   # the folder the base game was unpacked to alone
    # the app name comes from the game's control data (a NUP may carry none: the profile's name then), the bundle ID from its title ID; a setting / the command line overrides them
    app_name = args.app_name or APP_NAME or nacp.name(base / 'control.nacp') or g.get('name', '')
    if not app_name and mac_pack: raise BuildError(f'no game name in the NUP\'s control data ({base / "control.nacp"}) or the profile: give --app-name')
    bundle_id = args.bundle_id or BUNDLE_ID or f'org.nxnative.{app:016x}'
    # the app version is the built game's display version: the update's control data when an update is built, else the base game's
    app_version = nacp.version(game / 'control.nacp') or nacp.version(base / 'control.nacp')
    icon, stand_in = app_icon(user_icon, spare_icon, game, base)
    if mac_pack:
        out_app = out_app or work.parent / f'{mac_app_name(app_name)}.app'
        if stand_in: say(f'no game icon: the app takes the fallback icon {spare_icon} (--fallback-icon / FALLBACK_ICON)')
        say_game(app, app_name, app_version or '1.0', bundle_id, out_app, icon)
    # the game language: Horizon gives the game one of the languages its control data lists (the built version's, which an update may extend), so the one built in must be among them
    control = languages_control(game, base)
    langs = nacp.languages(control)
    lang = game_language(want_lang, bool(lang_in), langs)
    say(f'game language: {lang} ({dict(nacp.LANGUAGES)[lang]})' + (f', the game has no {want_lang}' if lang != want_lang else '')
        + (f'; the game\'s languages: {" ".join(langs)}' if langs else '; the control data lists no languages (not checked)') + ' (DM_LANG=<code> in the run command changes it)')

    patched = work / f'patched_{pkg_ver}'
    rewrite(py, repo, game / 'ExeFS', patched, env, logs, g)
    chain_header(py, repo, env, logs)   # before the host is built
    if derived: empty_tables(g)
    sh = work / 'shader'
    def derive_shader_tables():   # called only after the strict DLC-only comparison passed (split_shaders)
        nonlocal g, derived
        assert g is not None
        derived = 'the installed DLC shader content differs from the registered tables'
        derive_profile(g, pdir / g.id, pkg_ver, game, g.words('modules'), shader_only=True)
        profiles_first(env, pdir)
        g = gamemod.load(g.id)
        say(f'{derived}: using a shader-only derived profile at {g.dir}; registered module fixes are preserved')
        return g
    pkey = split_shaders(py, repo, [rfs.source(dirs[v]) for v in sv], [dirs[v] / 'ExeFS/main' for v in sv], sh / 'pkg', env, logs, g,
                         bool(derived), dlc_roms, None if derived else derive_shader_tables)   # a derived profile takes its shader tables from this split (the registered profile's are checked)
    seen = seen_keys(g, sh, seen_logs(work, app)) if seen_shaders else None
    msl = build_shaders(py, repo, sh, xlate, env, logs, g, pkey, seen)
    motion = MOTION if args.motion is None else args.motion
    env_file = build_env_file(work, args.env_file or DM_ENV_FILE, motion)
    if not motion: say('motion (gyro): off, DM_MOTION=0 is built in')
    host, font = mac_host(py, repo, work, env, logs, g)
    if mac_pack:   # the app packed from the host and the build's files
        if not icon: say('no game icon: the NUP carries none; the app\'s icon will be blank; --icon / ICON or --fallback-icon / FALLBACK_ICON gives it one')
        mac_app(py, repo, out_app, app, work, game, patched, host, font, msl, aoc, lang, g, app_name, app_version, bundle_id, icon, args.mac_sign or MAC_SIGN, env, logs, env_file,
                (args.gpu or PSO_GPUS).strip(), control, PREBAKE if args.prebake is None else args.prebake)
        rmtree(work / 'tmp')
        say(f'done: {out_app} ({tree_size(out_app)[1] / 2**30:.1f} GB); '
            f'open it from Finder. Its save is in ~/Library/Application Support/nxnative/{app:016X}/, its log in ~/Library/Logs/nxnative/{app:016X}.log')
        if seen_shaders: say('a partial shader library: where a shader is missing, the log says so in red and the draw goes without it. Run this script again with --seen-shaders to add what the log names, or without it for the whole map')
        report_profile_status(g, reg, pkg_ver, derived, 'Mac app', patched)
        return
    cmd = mac_run(work, game, patched, host, font, msl, aoc, lang, env, logs, g, env_file, partial=seen_shaders)   # the development build ends with the command that runs the game
    rmtree(work / 'tmp')
    say(f'done: the game runs on the Mac with this command (its save in {work / "run/save"}, kept from run to run; DM_WINDOW=1 opens a window, DM_AUDIO=1 plays sound):')
    PROG.say(cmd)
    if seen_shaders: say(f'a partial shader library: where a shader is missing, the log says so in red and the draw goes without it. Run this script again with --seen-shaders to add what {work / "run/host.log"} names, or without it for the whole map')
    report_profile_status(g, reg, pkg_ver, derived, 'Mac build', patched)


class Stopped(BaseException):   # a SIGTERM or SIGHUP, raised by cli's handler: not an Exception, so no `except Exception` on the way keeps it from cli
    def __init__(self, sig=signal.SIGTERM): super().__init__(sig); self.sig = sig


def _sigterm(sig, frame): raise Stopped(sig)


STOPS = (signal.SIGTERM, signal.SIGHUP)   # the signals cli turns into Stopped


NUP_FOLDER = None   # main's: the folder whose work folders cli tidies after a build that succeeded


def cli(argv):   # returns the exit code: 0 done, 1 error, 130 interrupted, 143 stopped by SIGTERM, 129 by SIGHUP
    # main points DM_GAMES_DIR (and DM_GAMES_FIRST) at this run's derived profile for its tools. Spyder runs the next F5 in the same process, where the game lookup
    # (game.py by_title) would then find that profile first: the environment is put back at the end
    environ = dict(os.environ)
    # A SIGTERM (a front end's cancel) ends the build as an interrupt does: the running tool's process group stopped (run), the stage line ended, exit code 143. So does a SIGHUP
    # (the terminal closed): the shell sends it to its job, which the tool's group, a session of its own (proc_group), is not in, and its default action ended this script
    # with no cleanup and left the tool running. The handlers are cli's only while it runs, and in the main thread only (Python's rule): Spyder's kernel, which runs the next
    # F5 in the same process, gets its own back
    main_thread = threading.current_thread() is threading.main_thread()
    saved = {s: signal.signal(s, _sigterm) for s in STOPS} if main_thread else {}
    global BUILD_STARTED
    BUILD_STARTED = obsolete_stamp(); BUILD_FOLDERS.clear()
    try:
        main(parse_args(argv))
        for w in sorted(BUILD_FOLDERS): empty_obsolete(w)
        if NUP_FOLDER:   # a developer's work folder beside the NUP (tree_tar.py)
            for w in NUP_FOLDER.glob('*-work'):
                if re.fullmatch(r'[0-9A-Fa-f]{16}-work', w.name): tree_tar.pack_quiet(w, say)
    except (BuildError, nso.Error, pack_nup.Error) as e:   # a damaged module, read in this process (derive_profile), or a NUP's version it cannot unpack (no RomFS, read for the disk check) stops the build the same way
        PROG.close(ok=False); PROG.say(f'[RED] {e}'); return 1
    except KeyboardInterrupt:
        PROG.close(ok=False); PROG.say('[RED] interrupted'); return 130
    except Stopped as e:
        PROG.close(ok=False); PROG.say(f'[RED] stopped ({signal.Signals(e.sig).name})'); return 128 + int(e.sig)
    except Exception as e:   # an unexpected error: end the stage line before the traceback (which goes to stderr; json mode also sends the error as a message)
        PROG.close(ok=False)
        if PROG.mode == 'json': PROG.say(f'[RED] {type(e).__name__}: {e}')
        raise
    finally:
        for s, h in saved.items(): signal.signal(s, signal.SIG_DFL if h is None else h)   # first: a SIGTERM or SIGHUP in this cleanup is the caller's again (None: one not set from Python)
        PROG.close(ok=False)   # stop the ticker
        for k in os.environ.keys() - environ.keys(): del os.environ[k]
        os.environ.update(environ)
    return 0


if __name__ == '__main__':
    code = cli(sys.argv[1:])
    if not in_ipython(): sys.exit(code)   # give an exit code in a terminal; do not exit in Spyder (keeps the console clean)
