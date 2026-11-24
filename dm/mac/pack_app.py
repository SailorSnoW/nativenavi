#!/usr/bin/env python3
# Packs the Mac app (build_app.py --platform mac-app; layout: docs/development.md, "A Mac app"): launcher, host, and the game's data in data bundles.
# Signed without the hardened runtime: the host maps the game's code executable, which it refuses without entitlements, so the app cannot be notarized.
# Usage: pack_app.py <out.app> --host <dmhost> --patched <dir> --romfs <image or dir> --msl <dir> --tables <profile dir> --font <dir> --title-id <id> --name <name> [options]
#   Reuses <out.app> while <out.app>.key matches, else each data bundle that still matches; builds <out.app>.partial and swaps it in. Prints "step: ..." and "app: <path>[, reused (inputs unchanged)]".
#   A failed pack leaves no <out.app>.key, only <out.app>.key.replaced, from which the next pack takes the data bundles of the app still in place.
import argparse, concurrent.futures, contextlib, ctypes, errno, hashlib, json, os, plistlib, re, shutil, stat, subprocess, sys, tempfile
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / 'tools'))
import nso_syms
import romfs as rfs
import shader_lib
import vol_space
MODULE_FILES = ('*.patched.bin', '*.fixups.tsv', '*.gates.tsv', '*.cuts.tsv', '*.obs.tsv', '*.changed.tsv')
PRACTICE_FILES = MODULE_FILES + ('*.ips', 'LICENSE')
PRACTICE_SETTING = 'DM_PRACTICE'   # offered by the settings only in an app that carries the practice tools
MSL_FILES = ('*.metallib', '*.json', '*.tsv')
# any may be absent
TABLES = ('shader_map.tsv', 'pso_manifest.tsv', 'c1_full.tsv', 'cb_extent.tsv', 'shader_pairs.tsv', 'pso_pairs.tsv')
PSO_FILES = ('*.mtl4a', '*-prewarm-*.keys', 'prebuilt.tsv', 'rest_pairs.tsv', 'archive.tsv', 'prewarm.tsv')
EXE = 'nativenavi'
ICON_SIZES = (16, 32, 128, 256, 512)
DETRITUS = ('.DS_Store',)


def step(msg): print(f'step: {msg}', flush=True)


def detritus(name):   # exFAT keeps extended attributes in a "._name" file beside each file: not an input, and the bundle may not carry it
    return name.startswith('._') or name in DETRITUS


def run(cmd):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'{Path(str(cmd[0])).name} failed ({r.returncode}): {(r.stderr or r.stdout).strip()[-1500:]}')
    return r.stdout


def first_file(d):
    for root, _, files in os.walk(d):
        if files: return Path(root) / files[0]
    return None


def can_clone(src, dst_dir):
    f = src if src.is_file() else first_file(src)
    if f is None: return False
    t = dst_dir / '.clone_test'
    try:
        if t.exists(): t.unlink()
        return ctypes.CDLL(None, use_errno=True).clonefile(str(f).encode(), str(t).encode(), 0) == 0
    finally:
        if t.exists(): t.unlink()


def tree_bytes(d):
    if d.is_file(): return d.stat().st_size
    return sum((Path(r) / f).stat().st_size for r, _, fs in os.walk(d) for f in fs)


def copy(srcs, dst, clone):   # -X keeps extended attributes out (cp rejects -c -X together)
    cmd = ['/bin/cp', '-R'] + (['-c'] if clone else ['-X'])
    pointer = ctypes.sizeof(ctypes.c_void_p)
    def arg_bytes(p): return len(os.fsencode(p)) + 1 + pointer
    # exec's limit counts the strings, their NULs and the pointers; 2 KiB more for alignment, as POSIX xargs reserves
    env_bytes = sum(arg_bytes(f'{k}={v}') for k, v in os.environ.items()) + pointer
    budget = os.sysconf('SC_ARG_MAX') - env_bytes - sum(arg_bytes(p) for p in cmd + [dst]) - pointer - 2048
    batch, used = [], 0
    for src in srcs:
        size = arg_bytes(src)
        if size > budget: raise SystemExit(f'copy arguments exceed ARG_MAX for {src}')
        if batch and used + size > budget:
            run(cmd + batch + [dst]); batch, used = [], 0
        batch.append(src); used += size
    if batch: run(cmd + batch + [dst])


def min_os(host):   # LC_BUILD_VERSION minos
    m = re.search(r'cmd LC_BUILD_VERSION.*?minos (\S+)', run(['otool', '-l', host]), re.S)
    return m.group(1) if m else '14.0'


def swift_sources():
    return [HERE / n for n in ('Launcher.swift', 'LauncherSettings.swift', 'LauncherRow.swift')]


def build_launcher(out, minos, clang, swiftc, sdk):   # no Swift main: -parse-as-library
    with tempfile.TemporaryDirectory(dir=out.parents[3]) as t:
        obj = Path(t) / 'launcher.o'
        run([clang, '-isysroot', sdk, '-arch', 'arm64', f'-mmacosx-version-min={minos}', '-O2', '-Wall', '-I', HERE.parent / 'host', '-c', '-o', obj, HERE / 'launcher.c'])
        run([swiftc, '-sdk', sdk, '-target', f'arm64-apple-macos{minos}', '-O', '-swift-version', '5', '-parse-as-library', '-module-cache-path', Path(t) / 'module-cache',
             '-o', out, *swift_sources(), obj, '-framework', 'CoreFoundation', '-framework', 'AppKit', '-framework', 'SwiftUI', '-framework', 'AVFAudio', '-framework', 'CoreAudio'])


def make_icns(jpg, out):
    with tempfile.TemporaryDirectory(dir=out.parent) as t:
        s = Path(t) / 'AppIcon.iconset'; s.mkdir()
        for n in ICON_SIZES:
            for k, suffix in ((1, ''), (2, '@2x')):
                run(['sips', '-s', 'format', 'png', '-z', n * k, n * k, jpg, '--out', s / f'icon_{n}x{n}{suffix}.png'])
        run(['iconutil', '-c', 'icns', s, '-o', out])


def settings_rules(listed):
    rules = []
    for s in listed:
        if s['kind'] == 'range': rules.append({'env': s['env'], 'min': float(s['min']), 'max': float(s['max'])})
        elif s['kind'] == 'choice': rules.append({'env': s['env'], 'values': [o[0] for o in s['options']]})
        elif s['kind'] == 'toggle': rules.append({'env': s['env'], 'values': [s.get('on', '1'), s.get('off', '0')]})
    return rules


def info_plist(a, minos, icon):
    # LSSupportsGameMode off: a workaround (docs/reference/frame-rate.md, "macOS Game Mode holds the GPU at its lowest clock")
    p = {'CFBundleName': a.name, 'CFBundleDisplayName': a.name, 'CFBundleIdentifier': a.bundle_id or f'org.nativenavi.{a.title_id.lower()}',
         'CFBundleVersion': a.version or '1.0', 'CFBundleShortVersionString': a.version or '1.0', 'CFBundlePackageType': 'APPL', 'CFBundleExecutable': EXE,
         'CFBundleInfoDictionaryVersion': '6.0', 'LSMinimumSystemVersion': minos, 'LSArchitecturePriority': ['arm64'], 'NSHighResolutionCapable': True,
         'NSBluetoothAlwaysUsageDescription': 'Connect Switch 2 Joy-Con and Pro Controllers over Bluetooth.',
         'LSApplicationCategoryType': a.category or 'public.app-category.games', 'LSSupportsGameMode': False, 'DMTitleID': a.title_id.upper(), 'DMRomfsMarker': a.marker or '', 'DMLanguage': a.lang or 'en-US'}
    if icon: p['CFBundleIconFile'] = 'AppIcon'
    if a.settings:
        listed = json.loads(Path(a.settings).read_text(encoding='utf-8'))['settings']
        p['DMSettings'] = settings_rules(listed)
        lang = next((s['default'] for s in listed if s['env'] == 'DM_LANG'), None)
        if lang: p['DMLanguage'] = lang
    return p


def source_commit():
    repo = HERE.parent.parent
    try:
        head = subprocess.run(['git', '-C', repo, 'rev-parse', '--short=12', 'HEAD'], capture_output=True, text=True, check=True).stdout.strip()
        dirty = subprocess.run(['git', '-C', repo, 'status', '--porcelain', '--untracked-files=no'], capture_output=True, text=True, check=True).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        try: return (repo / '.source-commit').read_text().strip() or None
        except OSError: return None
    return head + ('+dirty' if dirty else '') if head else None


def strip_detritus(app):   # codesign reads each "._name" file as a code object and refuses the bundle
    for parent, _, files in os.walk(app):
        for n in files:
            if detritus(n):
                try: (Path(parent) / n).unlink()
                except OSError: pass   # a ._ file the volume maps to its file's attributes: gone with them


def sign(app, identity, codesign, nested=()):   # no --options runtime (see the top)
    strip_detritus(app)
    def refused(e): return any(s in str(e) for s in ('detritus', '._', 'not signed at all'))
    def first(target):
        try: run([codesign, '--force', '--sign', identity, target])
        except SystemExit as e: return e
    # at once: codesign reads a bundle on one core
    with concurrent.futures.ThreadPoolExecutor(max(1, len(nested))) as pool: failed = [(t, e) for t, e in zip(nested, pool.map(first, nested)) if e]
    for target, e in (*failed, (app / 'Contents/MacOS/dmhost', None), (app, None)):
        e = e or first(target)
        if e is None: continue
        if not refused(e): raise e
        subprocess.run(['xattr', '-cr', str(app)], capture_output=True)
        strip_detritus(app)
        run([codesign, '--force', '--sign', identity, target])
    strip_detritus(app)   # on exFAT codesign's attribute is a "._name" file again: the signature verifies without it, and a later --force would refuse it


# Data bundles: each large input in a code bundle of its own in Contents/PlugIns, linked into Contents/Resources: the app's seal takes a nested bundle's
# code directory hash alone, so a pack that changes only the host signs in a second. A bundle is cloned from the replaced app only when <out.app>.key says
# the same inputs made it and its files are as left (the seal does not read them). macOS moves change times of cloned files after the fact: a file whose
# change time alone differs is compared with its recorded SHA-256. docs/reference/packing.md#every-second-build-signs-the-whole-mac-app-again
DATA_FORMAT = 'NativeNavi data bundle 1'
SEAL_READ = 1 << 20


def data_bundle(contents, name): return contents / 'PlugIns' / f'{name}.bundle'


def data_reused(old_app, previous, name, key, dst):
    if not old_app or not isinstance(previous, dict) or not isinstance(previous.get('data'), dict) or previous['data'].get(name) != key or not isinstance(previous.get('app'), list): return False
    b = data_bundle(old_app / 'Contents', name); top = str(b.relative_to(old_app))
    want = {r[0]: r[1:] for r in previous['app'] if isinstance(r, list) and r and isinstance(r[0], str) and (r[0] == top or r[0].startswith(top + '/'))}
    try:
        have = {(top if r[0] == '.' else f'{top}/{r[0]}'): r[1:] for r in inventory(b)}
        if not want or not as_recorded(want, have, old_app, previous.get('seals')): return False
    except OSError: return False
    dst.parent.mkdir(parents=True, exist_ok=True)
    copy([b], dst, can_clone(b / 'Contents/Info.plist', dst.parent))
    return True


def data_shell(dst, name, bundle_id):
    (dst / 'Contents/Resources').mkdir(parents=True)
    (dst / 'Contents/Info.plist').write_bytes(plistlib.dumps({'CFBundleIdentifier': f'{bundle_id}.data.{name.lower()}', 'CFBundleName': name, 'CFBundlePackageType': 'BNDL',
                                                             'CFBundleInfoDictionaryVersion': '6.0'}))


def digest(path):
    h = hashlib.sha256()
    with Path(path).open('rb') as f:
        for b in iter(lambda: f.read(1 << 20), b''): h.update(b)
    return h.hexdigest()


def file_row(p, by_stat=False):
    """A large file by size and times: a write changes its ctime, which nothing sets back"""
    st = p.stat()
    return [str(p), st.st_mode, *([st.st_size, st.st_mtime_ns, st.st_ctime_ns] if by_stat else [digest(p)]), os.readlink(p) if p.is_symlink() else None]


def inventory(root):
    """stat only, no multi-GB reads"""
    def walk_error(e): raise e
    paths = [root]
    for parent, dirs, files in (os.walk(root, onerror=walk_error) if root.is_dir() else []):
        paths.extend(Path(parent) / n for n in dirs + files)
    rows = []
    for p in sorted(paths):
        s = p.lstat()
        rows.append([str(p.relative_to(root)), s.st_mode, s.st_size, s.st_mtime_ns, s.st_ctime_ns,
                     os.readlink(p) if stat.S_ISLNK(s.st_mode) else ''])
    return rows


def seals(app):
    """The digest the bundle's signature records (CodeResources files2 hash2), else the file's own up to SEAL_READ; a larger one is reused only with its times as recorded"""
    out = {}
    plugins = app / 'Contents/PlugIns'
    for b in sorted(plugins.glob('*.bundle')) if plugins.is_dir() else []:
        try: signed = plistlib.loads((b / 'Contents/_CodeSignature/CodeResources').read_bytes()).get('files2', {})
        except Exception: signed = {}
        for parent, _, files in os.walk(b):
            for n in files:
                p = Path(parent) / n; st = p.lstat()
                if not stat.S_ISREG(st.st_mode): continue
                rule = signed.get(str(p.relative_to(b / 'Contents')))
                if isinstance(rule, dict) and isinstance(rule.get('hash2'), bytes): out[str(p.relative_to(app))] = rule['hash2'].hex()   # none is read
                elif st.st_size <= SEAL_READ: out[str(p.relative_to(app))] = digest(p)   # the bundle's Info.plist and the signature's own files
    return out


def as_recorded(want, have, root, sealed):
    """A file whose change time alone moved still counts as left when its bytes have the recorded SHA-256"""
    if want.keys() != have.keys(): return False
    for path, row in have.items():
        was = want[path]
        if list(row) == list(was): continue
        if not stat.S_ISREG(row[0]) or list(row[:3]) != list(was[:3]) or list(row[4:]) != list(was[4:]): return False
        if not isinstance(sealed, dict) or not sealed.get(path) or digest(Path(root) / path) != sealed[path]: return False
    return True


def as_packed(row, record, moved):
    """The last pack's row when only the change time differs and the bytes are the recorded ones; a change time seen before is taken without reading the file"""
    was = record.get('row') if isinstance(record, dict) else None
    if not (isinstance(was, list) and len(was) == len(row) == 6 and was[:4] == row[:4] and was[5] == row[5] and was[4] != row[4]): return row
    if record.get('ctime') != row[4]:
        try:
            if not record.get('sha256') or digest(row[0]) != record['sha256']: return row
        except OSError: return row
    moved[row[0]] = row[4]
    return was


def is_chain_library(msl): return shader_lib.name_of(msl).startswith('b-')


def msl_inputs(msl):
    if is_chain_library(msl) and str(shader_lib.view(msl)).endswith('.img'): return [Path(shader_lib.image_of(msl))] if any(n.endswith('.metallib') for n in shader_lib.app_files(msl)) else []
    files = sorted(p for pat in MSL_FILES for p in Path(msl).glob(pat) if p.is_file() and p.name != '_b_build.json' and not detritus(p.name))
    return files if any(p.suffix == '.metallib' for p in files) else []


def copy_msl(msl, msls, dst, clone):
    """The chain's library goes as the one file dst.img, which costs a copy's space, not a clone's"""
    if is_chain_library(msl): shader_lib.subset(msl, f'{dst}.img'); return
    dst.mkdir(parents=True); copy(msls, dst, clone)


def selected_inputs(a):
    """Resolved once: the same lists make the reuse key and the copies"""
    for name in ('patched', 'romfs', 'msl', 'tables', 'font', 'aoc', 'pso', 'practice'):
        p = getattr(a, name)
        if p and not (rfs.Path(p).is_dir() if name == 'romfs' else shader_lib.exists(p) if name == 'msl' else Path(p).is_dir()): raise SystemExit(f'no {name} directory at {p}')
    if a.marker and not (rfs.Path(a.romfs) / a.marker).exists(): raise SystemExit(f'no RomFS at {a.romfs} (missing {a.marker})')
    mods = sorted({p for pat in MODULE_FILES for p in Path(a.patched).glob(pat) if not detritus(p.name)})
    if not any(p.name.endswith('.patched.bin') for p in mods): raise SystemExit(f'no rewritten module in {a.patched}')
    for p in mods:
        if p.name.endswith('.patched.bin') and not p.with_name(p.name.removesuffix('.patched.bin') + '.fixups.tsv').is_file():
            raise SystemExit(f'no rewriter fixups for {p}')
    practice = sorted({p for pat in PRACTICE_FILES for p in Path(a.practice).glob(pat) if not detritus(p.name)}) if a.practice else []
    if a.practice and not (any(p.name.endswith('.patched.bin') and p.with_name(p.name.removesuffix('.patched.bin') + '.fixups.tsv').is_file() for p in practice)
                           and any(p.suffix == '.ips' for p in practice)):
        raise SystemExit(f'the practice tools\' folder {a.practice} needs a rewritten module with its fixups and an IPS')
    msls = msl_inputs(a.msl)
    if not msls: raise SystemExit(f'no shader libraries at {a.msl}')
    pso = sorted({p for pat in PSO_FILES for p in Path(a.pso).glob(pat) if not detritus(p.name)}) if a.pso else []
    if a.pso and (not any(p.suffix == '.mtl4a' for p in pso) or not (Path(a.pso) / 'prebuilt.tsv').is_file()):
        raise SystemExit(f'no pipeline archive with its prebuilt.tsv in {a.pso}')
    tables = [Path(a.tables) / n for n in TABLES if (Path(a.tables) / n).is_file()]
    pairs = Path(a.pso_pairs) if a.pso_pairs and not any(p.name == 'pso_pairs.tsv' for p in tables) else None
    fonts = sorted(p for p in Path(a.font).glob('*.ttf') if not detritus(p.name))
    files = [Path(a.host)] + mods + practice + msls + pso + tables + fonts + ([pairs] if pairs else [])
    files += [Path(p) for p in (a.env_file, a.settings, a.icon) if p]
    for p in files:
        if not p.is_file(): raise SystemExit(f'no input file at {p}')
    if a.settings and not practice and any(s.get('env') == PRACTICE_SETTING for s in json.loads(Path(a.settings).read_text(encoding='utf-8'))['settings']):
        raise SystemExit(f'the settings list {a.settings} offers {PRACTICE_SETTING}, and the app would not carry the practice tools (--practice)')
    return mods, practice, msls, pso, tables, pairs, fonts, files


def replace_app(tmp, out):
    if out.exists():
        # RENAME_SWAP exchanges complete directories atomically; a failed swap leaves the old app at out.
        swap = ctypes.CDLL(None, use_errno=True).renamex_np
        swap.argtypes = (ctypes.c_char_p, ctypes.c_char_p, ctypes.c_uint)
        swap.restype = ctypes.c_int
        if swap(os.fsencode(tmp), os.fsencode(out), 2):
            err = ctypes.get_errno()
            if err not in (errno.ENOTSUP, errno.EOPNOTSUPP): raise OSError(err, os.strerror(err), str(out))
            swap_by_moves(tmp, out)   # Mac OS Extended has no RENAME_SWAP ([Errno 45])
    else:
        tmp.rename(out)


def swap_by_moves(tmp, out):
    """RENAME_SWAP's result by plain renames, undone on failure. Not atomic: a kill between the first two renames leaves the old app at <out>.old, which the next replacement clears"""
    aside = out.with_name(out.name + '.old')
    shutil.rmtree(aside, ignore_errors=True)
    out.rename(aside)
    try:
        tmp.rename(out)
    except BaseException:
        aside.rename(out)
        raise
    try:
        aside.rename(tmp)
    except BaseException:
        out.rename(tmp); aside.rename(out)
        raise


def write_key(key_file, value):
    key_tmp = key_file.with_name(key_file.name + '.partial')
    try:
        key_tmp.write_text(json.dumps(value, sort_keys=True) + '\n')
        key_tmp.replace(key_file)
    finally:
        key_tmp.unlink(missing_ok=True)


def pack(a):
    out = Path(a.out).resolve()
    if out.suffix != '.app': raise SystemExit(f'the output must end in .app: {out}')
    key_file, set_aside = out.with_name(out.name + '.key'), out.with_name(out.name + '.key.replaced')
    try:
        previous = json.loads(key_file.read_text())
    except (OSError, ValueError): previous = None
    try: left = None if previous is not None else json.loads(set_aside.read_text())
    except (OSError, ValueError): left = None
    last = next((r['sources'] for r in (previous, left) if isinstance(r, dict) and isinstance(r.get('sources'), dict)), {})
    moved = {}
    def aside():
        """No failed attempt may leave a valid reuse key: the record goes aside, where only its data bundles are taken from it"""
        with contextlib.suppress(OSError):
            if key_file.is_file(): os.replace(key_file, set_aside)
    try:
        a = argparse.Namespace(**vars(a))
        for name in ('out', 'host', 'patched', 'practice', 'romfs', 'msl', 'tables', 'font', 'aoc', 'pso', 'pso_pairs', 'env_file', 'settings', 'icon'):
            if getattr(a, name): setattr(a, name, str(Path(getattr(a, name)).resolve()))
        mods, practice, msls, pso, tables, pairs, fonts, files = selected_inputs(a)
        clang = Path(run(['xcrun', '--find', 'clang']).strip()).resolve()
        swiftc = Path(run(['xcrun', '--find', 'swiftc']).strip())   # not resolved: the driver is a link to swift-frontend, which acts by its name
        sdk = Path(run(['xcrun', '--sdk', 'macosx', '--show-sdk-path']).strip()).resolve()
        if not clang.is_file() or not swiftc.is_file() or not sdk.is_dir(): raise SystemExit(f'no compiler or SDK at {clang}, {swiftc}, {sdk}')
        codesign = shutil.which('codesign')
        if not codesign: raise SystemExit('codesign is not available')
        codesign = Path(codesign).resolve()
        minos = min_os(a.host)
        sources = [Path(__file__).resolve(), HERE / 'launcher.c', HERE.parent / 'host/romfs_image.h', HERE.parent / 'tools/romfs.py', HERE.parent / 'tools/nso_syms.py'] + swift_sources()
        large = {*msls, *pso}
        def tree(p):
            rows = inventory(Path(p))
            return [['.', *as_packed([p, *rows[0][1:]], last.get(p), moved)[1:]]] if Path(p).is_file() and len(rows) == 1 else rows
        romfs_rows = tree(a.romfs)
        inputs = {'options': vars(a), 'files': [as_packed(file_row(p, True), last.get(str(p)), moved) if p in large else file_row(p) for p in files + sources],
                  'trees': [(p, romfs_rows if p == a.romfs else inventory(Path(p))) for p in (a.romfs, a.aoc) if p and Path(p).exists()],
                  'tools': [[str(c), c.stat().st_mode, c.stat().st_size, c.stat().st_mtime_ns, c.stat().st_ctime_ns, run([c, '--version']).strip()] for c in (clang, swiftc)] + [
                            str(sdk), run(['xcrun', '--sdk', 'macosx', '--show-sdk-version']).strip(),
                            str(codesign), codesign.stat().st_mode, digest(codesign), minos]}
        key = hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()
        if out.is_dir() and isinstance(previous, dict) and previous.get('inputs') == key and isinstance(previous.get('app'), list):
            now = inventory(out)
            rows = lambda inv: {r[0]: r[1:] for r in inv if isinstance(r, list) and r and isinstance(r[0], str)}
            if previous['app'] == now or (len(rows(previous['app'])) == len(previous['app']) and as_recorded(rows(previous['app']), rows(now), out, previous.get('seals'))):
                if previous['app'] != now or any(last[k].get('ctime') != v for k, v in moved.items()):
                    with contextlib.suppress(OSError): write_key(key_file, dict(previous, app=now, sources={k: dict(v, ctime=moved[k]) if k in moved else v for k, v in last.items()}))
                print(f'app: {out}, reused (inputs unchanged)', flush=True)
                return
    except BaseException:
        aside(); key_file.unlink(missing_ok=True)
        raise
    aside(); key_file.unlink(missing_ok=True)
    if previous is None: previous = left
    tmp = out.with_name(out.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True)
    c = tmp / 'Contents'; mac, res = c / 'MacOS', c / 'Resources'; dm = res / 'dm'
    for d in (mac, dm / 'font'): d.mkdir(parents=True)
    romfs, msl, aoc = Path(a.romfs), Path(a.msl), Path(a.aoc) if a.aoc else None
    # cp -c copies what it cannot clone: a probe decides only the free-space check and the step line
    trees, lists = {'romfs': romfs, 'aoc': aoc}, {'mods': mods, 'practice': practice, 'msl': msls, 'pso': pso}
    clone = {k: bool(d) and can_clone(d, res) for k, d in trees.items()} | {k: bool(ps) and can_clone(ps[0], res) for k, ps in lists.items()}
    if is_chain_library(msl): clone['msl'] = False
    need = sum(tree_bytes(d) for k, d in trees.items() if d and not clone[k]) + sum(p.stat().st_size for k, ps in lists.items() if not clone[k] for p in ps)
    if need:
        free = vol_space.free_bytes(res)
        if need + (1 << 30) > free: raise SystemExit(f'the app needs {need / 2**30:.1f} GB on the volume of {out.parent} (no APFS clones from the build there) and {free / 2**30:.1f} GB are free')
    step(f'launcher and host ({"clones" if clone["romfs"] else "copies"} of the game files)')
    build_launcher(mac / EXE, minos, clang, swiftc, sdk)
    shutil.copy(a.host, mac / 'dmhost')   # copy2 would carry the build's extended attributes into the app
    copy(mods, dm, clone['mods'])
    if practice: (dm / 'practice').mkdir(); copy(practice, dm / 'practice', clone['practice'])
    if tables: copy(tables, dm, False)
    if pairs: shutil.copyfile(pairs, dm / 'pso_pairs.tsv')
    if fonts: copy(fonts, dm / 'font', False)
    if a.env_file: shutil.copyfile(a.env_file, dm / 'env.txt')
    if a.settings: shutil.copyfile(a.settings, dm / 'launcher.json')
    msl_dst = res / 'msl' / (a.msl_name or shader_lib.name_of(msl))
    old_app = out if out.is_dir() else None
    took = {f[0]: f[1:] for f in inputs['files']}
    common = [DATA_FORMAT, a.sign, str(codesign), digest(codesign), a.bundle_id or a.title_id]
    data, fresh = {}, []
    def place(name, top, label, inputs, fill):
        b = data_bundle(c, name); link = res / top
        data[name] = hashlib.sha256(json.dumps(common + [name, top, inputs], sort_keys=True).encode()).hexdigest()
        if data_reused(old_app, previous, name, data[name], b): step(f'{label} (reused, signed)')
        else:
            step(label); data_shell(b, name, a.bundle_id or f'org.nativenavi.{a.title_id.lower()}'); (b / 'Contents/Resources' / top).parent.mkdir(parents=True, exist_ok=True)
            fill(b / 'Contents/Resources' / top); fresh.append(b)
        link.parent.mkdir(parents=True, exist_ok=True); link.symlink_to(os.path.relpath(b / 'Contents/Resources' / top, link.parent))
    image = msls[0] if is_chain_library(msl) and len(msls) == 1 and msls[0].suffix == '.img' else None
    read = {}
    def fill_msl(d):
        with concurrent.futures.ThreadPoolExecutor(1) as pool:
            whole = pool.submit(digest, image) if image else None
            d.mkdir(parents=True); copy_msl(msl, msls, d / msl_dst.name, clone['msl'])
            if whole: read[str(image)] = whole.result()
    lib_code = [digest(Path(shader_lib.__file__)), digest(Path(rfs.__file__))]
    place('ShaderLibraries', 'msl', 'shader libraries', [lib_code, msl_dst.name, [took[str(p)] for p in msls]], fill_msl)
    if aoc: place('AddOnContent', 'aoc', 'add-on content', inventory(aoc), lambda d: copy([aoc], d, clone['aoc']))
    if pso: place('PipelineArchive', 'pso', 'pipeline archive', [took[str(p)] for p in pso], lambda d: (d.mkdir(parents=True), copy(pso, d, clone['pso'])))
    rtop = 'romfs.img' if romfs.is_file() else 'romfs'
    place('GameData', rtop, 'game files', romfs_rows, lambda d: copy([romfs], d, clone['romfs']))
    icon = bool(a.icon)
    if icon: step('icon'); make_icns(a.icon, res / 'AppIcon.icns')
    info = info_plist(a, minos, icon)
    info['DMModuleFiles'] = [p.name for p in mods]
    if nso_syms.audio_input_capable(mods):
        info['NSMicrophoneUsageDescription'] = f'{a.name} uses the microphone for in-game audio input.'
    commit = source_commit()
    if commit: info['DMCommit'] = commit
    if msl_dst.name.startswith('b-'): info['DMChain'] = msl_dst.name[2:]
    (c / 'Info.plist').write_bytes(plistlib.dumps(info))
    (c / 'PkgInfo').write_text('APPL????')
    step(f'signing ({"ad-hoc" if a.sign == "-" else a.sign})'); sign(tmp, a.sign, codesign, fresh)
    had_old = out.exists()
    replace_app(tmp, out)
    sealed, sources = seals(out), {}
    def note(path, row, sha):
        if sha: sources[str(path)] = {'row': row, 'ctime': moved.get(str(path), row[4]), 'sha256': sha}
    bundled = lambda name, rel: sealed.get(str(data_bundle(Path('Contents'), name) / 'Contents/Resources' / rel))
    if romfs.is_file(): note(romfs, [str(romfs), *romfs_rows[0][1:]], bundled('GameData', rtop))
    for f in pso: note(f, [str(f), *took[str(f)]], bundled('PipelineArchive', f'pso/{f.name}'))
    if image: note(image, [str(image), *took[str(image)]], read.get(str(image)) or (last.get(str(image)) or {}).get('sha256'))
    try: write_key(key_file, {'inputs': key, 'data': data, 'app': inventory(out), 'seals': sealed, 'sources': sources})
    except BaseException:
        if had_old: replace_app(tmp, out)
        else: out.rename(tmp)
        raise
    set_aside.unlink(missing_ok=True)
    shutil.rmtree(tmp, ignore_errors=True)
    print(f'app: {out}', flush=True)


def parse(argv):
    ap = argparse.ArgumentParser(prog='pack_app.py', description='Pack a self-contained Mac app of a game')
    ap.add_argument('out', help='the app (<name>.app)')
    ap.add_argument('--host', required=True, help='the Mac host built for the profile')
    ap.add_argument('--patched', required=True, help='the rewritten modules (patch_nso.py output)')
    ap.add_argument('--practice', help='the practice tools: their rewritten module, the IPS of main\'s build and their license (the packing script\'s practice_tools folder; Resources/dm/practice)')
    ap.add_argument('--romfs', required=True, help='the RomFS of the packed version')
    ap.add_argument('--msl', required=True, help='the Mac shader libraries: <shader>/cache/b-<chain>')
    ap.add_argument('--msl-name', help='its folder name in the app (default: its own)')
    ap.add_argument('--tables', required=True, help='the game profile folder (shader_map.tsv and the tables beside it)')
    ap.add_argument('--font', required=True, help='the folder of the shared-font stand-ins (nintendo_ext.ttf)')
    ap.add_argument('--aoc', help='the DLC RomFS images or folders by index')
    ap.add_argument('--pso', help='the pipeline archive made offline for this Mac (the output of pso_offline.py: <arch>.mtl4a, prebuilt.tsv)')
    ap.add_argument('--pso-pairs', help='pso_pairs.tsv for a profile that has none (the shader split\'s copy)')
    ap.add_argument('--env-file', help='environment switches baked in (Resources/dm/env.txt)')
    ap.add_argument('--settings', help='the game\'s launcher settings (dm/games/settings.py output; Resources/dm/launcher.json, which the Mac front end reads)')
    ap.add_argument('--title-id', required=True); ap.add_argument('--name', required=True)
    ap.add_argument('--version'); ap.add_argument('--bundle-id'); ap.add_argument('--category'); ap.add_argument('--lang'); ap.add_argument('--marker', help='the profile\'s romfs_marker')
    ap.add_argument('--icon', help='the game icon (icon.jpg from the control data)')
    ap.add_argument('--sign', default='-', help='codesign identity (default: - = ad-hoc)')
    return ap.parse_args(argv)


if __name__ == '__main__':
    pack(parse(sys.argv[1:]))
