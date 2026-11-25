#!/usr/bin/env python3
# Packs the NativeNavi app (dm/gui, docs/gui.md) with setup.json and source.tar.gz of this checkout. --sign adds the hardened runtime: this app runs none of the game's code.
# Usage: python3 scripts/build_gui.py [-o <path>.app] [--sign "Developer ID Application: <name> (<team ID>)"] [--zip]
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
REQUIREMENTS = '.github/requirements/tools.txt'
RUST_DIR = 'dm/host/rust'   # where the app runs cargo once, so rustup installs the Rust rust-toolchain.toml names
NUP_TYPE = 'org.nativenavi.nup'
WHY_FILES = 'NativeNavi reads the game file you chose there to build the game\'s app.'


class Error(Exception):
    pass


def run(cmd, **kw):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True, **kw)
    if r.returncode:
        raise Error(f'{Path(str(cmd[0])).name} {" ".join(str(c) for c in cmd[1:3])} failed (exit code {r.returncode}): {(r.stderr or r.stdout).strip()[-3000:]}')
    return r.stdout


def min_os():
    m = re.search(r'^MACOS_MIN\s*:=\s*(\S+)', (REPO / 'dm/mac/min_os.mk').read_text(), re.M)
    if not m: raise Error('dm/mac/min_os.mk has no MACOS_MIN')
    return m.group(1)


def constant(path, name):   # read without importing: this script runs on any Python 3, the tools need 3.12
    for node in ast.parse((REPO / path).read_text()).body:
        if isinstance(node, ast.Assign) and any(isinstance(t, ast.Name) and t.id == name for t in node.targets):
            return ast.literal_eval(node.value)
    raise Error(f'{path} has no {name}')


def xcode_version():
    return constant('dm/gfx/tools/own_compilers.py', 'XCODE_VERSION')


def profile():
    return REPO / 'dm/games' / constant('dm/games/game.py', 'DEFAULT')


def metal_version():
    for line in (REPO / 'dm/gfx/tools/dmsh_chain.lock').read_text().splitlines():
        key, _, value = line.partition('\t')
        if key == 'metal_macosx': return value
    raise Error('dm/gfx/tools/dmsh_chain.lock has no metal_macosx')


def game_versions():   # {package: display version} of the profile's build rows
    rows = [l.split('\t') for l in (profile() / 'game.tsv').read_text(encoding='utf-8').splitlines() if l.startswith('build\t')]
    return {r[1]: r[2] for r in rows if len(r) > 2}


def known_nups():   # sha256 <TAB> bytes <TAB> versions it builds, the default first <TAB> what the container holds
    table, versions = profile() / 'nup.tsv', game_versions()
    rows = []
    for n, line in enumerate(table.read_text().splitlines() if table.is_file() else [], 1):
        if not line.strip() or line.startswith('#'): continue
        f = line.split('\t')
        if len(f) != 4 or not re.fullmatch(r'[0-9a-f]{64}', f[0]) or not f[1].isdigit() or not f[2].split() or not f[3].strip():
            raise Error(f'{table}:{n}: not a row of sha256 <TAB> bytes <TAB> versions <TAB> what it holds: {line!r}')
        if lost := [v for v in f[2].split() if v not in versions]:
            raise Error(f'{table}:{n}: {", ".join(lost)} has no build row in the profile\'s game.tsv: the app builds only a registered version')
        rows.append({'sha256': f[0], 'bytes': int(f[1]), 'versions': [{'package': v, 'display': versions[v]} for v in f[2].split()], 'holds': f[3].strip()})
    if not rows: raise Error(f'{table} lists no NUP container: the app would refuse every game file')
    return rows


def git(*args):
    return run(['git', '-C', REPO, *args])


def source_files():
    names = git('ls-files', '-z', '--cached', '--others', '--exclude-standard').split('\0')
    return sorted({n for n in names if n and ((REPO / n).is_file() or (REPO / n).is_symlink())})


def commit(files):   # the app unpacks its copy once per commit, so two changed copies must not share a name
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
