#!/usr/bin/env python3
# The NativeNavi app (dm/gui, docs/gui.md): a Mac app for players who do not use Terminal. It takes the player's NUP, downloads what the build needs into
#   ~/Library/Developer/NativeNavi (a Python with the repository's packages, Rust through rustup; and Apple's Metal toolchain for the tested Xcode), runs
#   scripts/build_app.py --progress json (docs/frontends.md) and shows the app it made.
# This script packs it from this checkout:
#   Contents/MacOS/NativeNavi         dm/gui/*.swift, compiled with the selected Xcode's swiftc for macOS MACOS_MIN (dm/mac/min_os.mk), as the game app's launcher is
#   Contents/Resources/setup.json      dm/gui/setup.json (the downloads, pinned by SHA-256) with this checkout's facts: its commit, the Xcode and Metal compiler the shaders were tested with
#                                      (dm/gfx/tools/own_compilers.py XCODE_VERSION, dmsh_chain.lock metal_macosx), the Python packages' list, the host's Rust folder, and the NUP
#                                      containers of the game the app builds from (the profile's nup.tsv: each one's SHA-256, size and what it holds; the app refuses any other file)
#   Contents/Resources/source.tar.gz   this checkout's files as they are on disk: the tracked ones and the new ones git does not ignore (any change makes the commit
#                                      <hash>+dirty.<digest of the packed files>, so that each changed copy is a commit of its own to the app).
#                                      The app unpacks it into ~/Library/Developer/NativeNavi/Source and builds the game from there
#   Contents/Resources/Assets.car, AppIcon.icns   dm/gui/AppIcon.icon, compiled with actool
#   Contents/Info.plist                the .nup type it opens, and why it asks for the folders a NUP may be in
# Signed ad-hoc, or with --sign <identity>, the hardened runtime and a secure timestamp, for notarization (this app runs none of the game's code, unlike the game's own app, which cannot
# have the hardened runtime). --zip also writes <app>.zip beside it, for notarytool and a download page.
# Usage: python3 scripts/build_gui.py [-o <path>.app] [--sign "Developer ID Application: <name> (<team ID>)"] [--zip]   (exit code 0 on success, 1 on error, 2 for invalid arguments)
import argparse
import ast
import hashlib
import json
import os
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
GUI = REPO / 'dm/gui'
NAME = 'NativeNavi'
BUNDLE_ID = 'org.nativenavi.NativeNavi'
VERSION = '1.0'
REQUIREMENTS = '.github/requirements/tools.txt'   # what the app's Python installs (pip, wheels only)
RUST_DIR = 'dm/host/rust'   # where the app runs cargo once, so that rustup installs the Rust its rust-toolchain.toml names
NUP_TYPE = 'org.nativenavi.nup'
WHY_FILES = 'NativeNavi reads the game file you chose there to build the game\'s app.'


class Error(Exception):
    pass


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise Error(f'{Path(str(cmd[0])).name} {" ".join(str(c) for c in cmd[1:3])} failed (exit code {r.returncode}): {(r.stderr or r.stdout).strip()[-3000:]}')
    return r.stdout


def min_os():   # the Mac host's minimum macOS, which the game app also has
    m = re.search(r'^MACOS_MIN\s*:=\s*(\S+)', (REPO / 'dm/mac/min_os.mk').read_text(), re.M)
    if not m: raise Error('dm/mac/min_os.mk has no MACOS_MIN')
    return m.group(1)


def constant(path, name):   # a module's constant, read without importing it (this script runs on any Python 3, the tools need 3.12)
    for node in ast.parse((REPO / path).read_text()).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return ast.literal_eval(node.value)
    raise Error(f'{path} has no {name}')


def xcode_version():   # the Xcode whose Metal compiler the shaders were tested with
    return constant('dm/gfx/tools/own_compilers.py', 'XCODE_VERSION')


def profile():   # the profile this repository builds (game.DEFAULT)
    return REPO / 'dm/games' / constant('dm/games/game.py', 'DEFAULT')


def metal_version():   # the Metal compiler the shaders were tested with, as tool_lock.py records it
    for line in (REPO / 'dm/gfx/tools/dmsh_chain.lock').read_text().splitlines():
        key, _, value = line.partition('\t')
        if key == 'metal_macosx': return value
    raise Error('dm/gfx/tools/dmsh_chain.lock has no metal_macosx')


def known_nups():   # the profile's nup.tsv: sha256 <TAB> bytes <TAB> what the container holds, '#' comments
    table = profile() / 'nup.tsv'
    rows = []
    for n, line in enumerate(table.read_text().splitlines() if table.is_file() else [], 1):
        if not line.strip() or line.startswith('#'): continue
        f = line.split('\t')
        if len(f) != 3 or not re.fullmatch(r'[0-9a-f]{64}', f[0]) or not f[1].isdigit() or not f[2].strip():
            raise Error(f'{table}:{n}: not a row of sha256 <TAB> bytes <TAB> what it holds: {line!r}')
        rows.append({'sha256': f[0], 'bytes': int(f[1]), 'holds': f[2].strip()})
    if not rows: raise Error(f'{table} lists no NUP container: the app would refuse every game file')
    return rows


def git(*args):
    return run(['git', '-C', REPO, *args])


def source_files():   # the checkout's files as the app packs them: tracked and new, not ignored, that are on disk
    names = git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split('\0')
    return sorted({n for n in names if n and ((REPO / n).is_file() or (REPO / n).is_symlink())})


def commit(files):   # the checkout's commit; when its files differ from it, +dirty and a digest of the files packed (their names and bytes). The app unpacks its copy once per
    # commit (Setup.swift prepareSource) and offers to build the game again when it changes (updateAvailable): with +dirty alone, a second app of other changes kept the first one's files
    head = git('rev-parse', '--short=12', 'HEAD').strip()
    if not git('status', '--porcelain').strip(): return head
    h = hashlib.sha256()
    for rel in files:
        path = REPO / rel
        h.update(rel.encode() + b'\0')
        h.update((b'L' + os.readlink(path).encode()) if path.is_symlink() else (b'F' + path.read_bytes()))
        h.update(b'\0')
    return f'{head}+dirty.{h.hexdigest()[:8]}'


def setup(source_commit):
    s = json.loads((GUI / 'setup.json').read_text())
    xcode = xcode_version()
    s.update(commit=source_commit, xcode=xcode, metal=metal_version(), requirements=REQUIREMENTS, rust_dir=RUST_DIR, nups=known_nups(),
             xcode_download=f'https://developer.apple.com/download/all/?q=Xcode%20{xcode}')
    return s


def info_plist(minos, build_number):
    return {
        'CFBundleName': NAME, 'CFBundleDisplayName': NAME, 'CFBundleIdentifier': BUNDLE_ID, 'CFBundleExecutable': NAME,
        'CFBundlePackageType': 'APPL', 'CFBundleInfoDictionaryVersion': '6.0', 'CFBundleDevelopmentRegion': 'en',
        'CFBundleShortVersionString': VERSION, 'CFBundleVersion': str(build_number),
        'CFBundleIconFile': 'AppIcon', 'CFBundleIconName': 'AppIcon',
        'LSMinimumSystemVersion': minos, 'LSArchitecturePriority': ['arm64'], 'LSApplicationCategoryType': 'public.app-category.utilities',
        'NSPrincipalClass': 'NSApplication', 'NSHighResolutionCapable': True,
        'NSHumanReadableCopyright': 'MIT License. Not affiliated with Nintendo.',
        'UTImportedTypeDeclarations': [{'UTTypeIdentifier': NUP_TYPE, 'UTTypeDescription': 'NUP Container', 'UTTypeConformsTo': ['public.data'],
                                        'UTTypeTagSpecification': {'public.filename-extension': ['nup']}}],
        'CFBundleDocumentTypes': [{'CFBundleTypeName': 'NUP Container', 'CFBundleTypeRole': 'Viewer', 'LSHandlerRank': 'Alternate', 'LSItemContentTypes': [NUP_TYPE]}],
        'NSDesktopFolderUsageDescription': WHY_FILES, 'NSDocumentsFolderUsageDescription': WHY_FILES, 'NSDownloadsFolderUsageDescription': WHY_FILES,
        'NSRemovableVolumesUsageDescription': WHY_FILES, 'NSNetworkVolumesUsageDescription': WHY_FILES,
    }


def pack_source(dest, files):
    def plain(ti):   # no owner names from this Mac in the archive
        ti.uid = ti.gid = 0; ti.uname = ti.gname = ''
        return ti
    with tarfile.open(dest, 'w:gz', compresslevel=6) as t:
        for rel in files: t.add(REPO / rel, arcname=rel, recursive=False, filter=plain)


def build(out, identity=None, zip_it=False, say=print):
    if out.suffix != '.app': raise Error(f'the app (-o) must end in .app: {out}')
    if not (GUI / 'AppIcon.icon').is_dir(): raise Error(f'no icon at {GUI / "AppIcon.icon"}')
    minos, files = min_os(), source_files()
    source_commit = commit(files)
    if '+dirty' in source_commit: say(f'note: the checkout has changes; the app packs its files as they are on disk ({source_commit})')
    swiftc = run(['xcrun', '--find', 'swiftc']).strip()
    sdk = run(['xcrun', '--sdk', 'macosx', '--show-sdk-path']).strip()
    out.parent.mkdir(parents=True, exist_ok=True)
    partial = out.with_name(out.name + '.partial')
    shutil.rmtree(partial, ignore_errors=True)
    contents = partial / 'Contents'; macos = contents / 'MacOS'; res = contents / 'Resources'
    macos.mkdir(parents=True); res.mkdir()
    with tempfile.TemporaryDirectory() as tmp:
        tmp = Path(tmp)
        say(f'compiling {len(list(GUI.glob("*.swift")))} Swift files for macOS {minos}')
        run([swiftc, '-sdk', sdk, '-target', f'arm64-apple-macos{minos}', '-O', '-swift-version', '6', '-parse-as-library', '-module-name', 'NativeNavi',
             '-module-cache-path', tmp / 'module-cache', '-o', macos / NAME, *sorted(GUI.glob('*.swift'))])
        run(['xcrun', 'actool', GUI / 'AppIcon.icon', '--compile', res, '--platform', 'macosx', '--minimum-deployment-target', minos, '--app-icon', 'AppIcon',
             '--output-partial-info-plist', tmp / 'icon.plist', '--output-format', 'human-readable-text'])
    say(f'packing the sources: {len(files):,} files of {source_commit}')
    pack_source(res / 'source.tar.gz', files)
    (res / 'setup.json').write_text(json.dumps(setup(source_commit), indent=2) + '\n')
    count = git('rev-list', '--count', 'HEAD').strip()
    with open(contents / 'Info.plist', 'wb') as f: plistlib.dump(info_plist(minos, count), f)
    sign = ['codesign', '--force', '--sign', identity or '-', '--options', 'runtime'] + (['--timestamp'] if identity else [])
    run([*sign, partial])
    run(['codesign', '--verify', '--strict', partial])
    if out.exists(): shutil.rmtree(out)
    os.replace(partial, out)
    if zip_it:
        z = out.with_suffix('.zip')
        z.unlink(missing_ok=True)
        run(['ditto', '-c', '-k', '--sequesterRsrc', '--keepParent', out, z])
        say(f'zip: {z}')
    size = sum(p.stat().st_size for p in out.rglob('*') if p.is_file())
    say(f'done: {out} ({size / 2**20:.0f} MB, {"signed by " + identity if identity else "signed ad-hoc"}, builds {source_commit})')
    return out


def main(argv):
    ap = argparse.ArgumentParser(description='Pack the NativeNavi app, which builds the game for players who do not use Terminal (docs/gui.md).')
    ap.add_argument('-o', '--out', default=str(REPO / 'dist' / f'{NAME}.app'), help='the app to write, a path ending in .app (default: dist/NativeNavi.app in the repository)')
    ap.add_argument('--sign', help='the codesign identity, e.g. "Developer ID Application: <name> (<team ID>)" (default: ad-hoc)')
    ap.add_argument('--zip', action='store_true', help='also write <app>.zip beside the app, for notarytool and a download page')
    args = ap.parse_args(argv)
    try:
        build(Path(os.path.expanduser(args.out)).resolve(), args.sign, args.zip)
    except Error as e:
        print(f'[RED] {e}', file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
