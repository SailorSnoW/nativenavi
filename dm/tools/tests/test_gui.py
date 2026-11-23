"""The NativeNavi app (dm/gui, packed by scripts/build_gui.py; docs/gui.md): its pinned downloads, the facts the packing script reads from the checkout (the tested Xcode and
Metal compiler, the profile's verified NUP containers, the Python packages, the host's Rust folder), the plist, the sources it packs, the game app's DMCommit from a copy of the
sources, and with Xcode a packed app. No game files needed."""
import importlib.util
import json
import plistlib
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/mac')); sys.path.insert(0, str(REPO / 'dm/gfx/tools')); sys.path.insert(0, str(REPO / 'dm/games'))
spec = importlib.util.spec_from_file_location('build_gui', REPO / 'scripts/build_gui.py')
if spec is None or spec.loader is None: raise ImportError('scripts/build_gui.py')
build_gui = importlib.util.module_from_spec(spec); spec.loader.exec_module(build_gui)
import game   # noqa: E402
import pack_app   # noqa: E402

SHA = re.compile(r'[0-9a-f]{64}')


def xcode():
    try: return subprocess.run(['xcrun', '--find', 'swiftc'], capture_output=True, text=True).returncode == 0 and shutil.which('codesign') is not None
    except OSError: return False


class Pins(unittest.TestCase):
    def test_downloads_are_pinned(self):
        s = json.loads((REPO / 'dm/gui/setup.json').read_text())
        for name in ('python', 'rustup'):
            pin = s[name]
            self.assertTrue(pin['url'].startswith('https://'), name)
            self.assertRegex(pin['sha256'], SHA, name)
            self.assertIn(pin['version'].replace('+', '%2B'), pin['url'], f'{name}: the version is the one the URL downloads')
        self.assertIn('aarch64-apple-darwin', s['python']['url'])
        self.assertGreater(s['first_build_gb'], 0)


class Facts(unittest.TestCase):
    def test_tested_xcode_and_metal(self):
        import own_compilers
        self.assertEqual(build_gui.xcode_version(), own_compilers.XCODE_VERSION)
        lock = dict(l.split('\t', 1) for l in (REPO / 'dm/gfx/tools/dmsh_chain.lock').read_text().splitlines() if '\t' in l and not l.startswith('#'))
        self.assertEqual(build_gui.metal_version(), lock['metal_macosx'])

    def test_paths_it_names_exist(self):
        self.assertEqual(build_gui.profile(), REPO / 'dm/games' / game.DEFAULT)
        self.assertTrue((REPO / build_gui.REQUIREMENTS).is_file())
        self.assertTrue((REPO / build_gui.RUST_DIR / 'rust-toolchain.toml').is_file())
        self.assertTrue((REPO / 'dm/mac/min_os.mk').is_file())

    def test_the_profile_lists_verified_nups(self):
        rows = build_gui.known_nups()
        self.assertTrue(rows)
        for r in rows:
            self.assertRegex(r['sha256'], SHA)
            self.assertGreater(r['bytes'], 0)
        self.assertEqual(len({r['sha256'] for r in rows}), len(rows))

    def test_nup_table_rows(self):
        with tempfile.TemporaryDirectory() as t:
            table = Path(t) / 'nup.tsv'
            with mock.patch.object(build_gui, 'profile', return_value=Path(t)):
                table.write_text('# header\n' + 'ab' * 32 + '\t25622267860\tv131072 (1.1.1)\n')
                self.assertEqual(build_gui.known_nups(), [{'sha256': 'ab' * 32, 'bytes': 25622267860, 'holds': 'v131072 (1.1.1)'}])
                for bad in ('AB' * 32 + '\t1\tx\n', 'ab' * 32 + '\tbig\tx\n', 'ab' * 32 + '\t1\n'):
                    table.write_text(bad)
                    with self.assertRaises(build_gui.Error): build_gui.known_nups()
                table.write_text('# only a header\n')
                with self.assertRaisesRegex(build_gui.Error, 'lists no NUP'): build_gui.known_nups()

    def test_setup_has_what_the_app_reads(self):
        s = build_gui.setup('0123456789ab')
        for key in ('python', 'rustup', 'nups', 'first_build_gb', 'commit', 'xcode', 'metal', 'xcode_download', 'requirements', 'rust_dir'):   # SetupConfig's keys (dm/gui/Setup.swift)
            self.assertIn(key, s)
        self.assertIn(f'Xcode {s["xcode"]}', s['xcode_download'].replace('%20', ' '))


class Plist(unittest.TestCase):
    def test_info_plist(self):
        p = build_gui.info_plist(build_gui.min_os(), 7)
        self.assertEqual(p['LSMinimumSystemVersion'], build_gui.min_os())
        self.assertEqual(p['CFBundleExecutable'], build_gui.NAME)
        nup = p['UTImportedTypeDeclarations'][0]
        self.assertEqual(nup['UTTypeTagSpecification']['public.filename-extension'], ['nup'])
        self.assertEqual(p['CFBundleDocumentTypes'][0]['LSItemContentTypes'], [nup['UTTypeIdentifier']])
        for k in ('NSDownloadsFolderUsageDescription', 'NSRemovableVolumesUsageDescription'): self.assertTrue(p[k])
        plistlib.dumps(p)


class Sources(unittest.TestCase):
    def test_packs_the_checkout(self):
        files = build_gui.source_files()
        for f in ('scripts/build_app.py', 'dm/gui/App.swift', 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate', '.github/requirements/tools.txt'):
            self.assertIn(f, files)
        self.assertFalse([f for f in files if '__pycache__' in f or f.startswith('dist/')])

    def test_no_space_in_the_apps_folders(self):   # the host's Makefiles take none (dm/host/rust.mk)
        src = (REPO / 'dm/gui/Setup.swift').read_text()
        root = re.search(r'static let root = home\.appending\(path: "([^"]+)"', src)
        assert root, 'Paths.root not found in dm/gui/Setup.swift'
        self.assertNotIn(' ', root.group(1))
        for name in re.findall(r'appending\(path: "([^"]+)", directoryHint: \.isDirectory\)', src.split('enum Paths', 1)[1].split('}', 1)[0]):
            self.assertNotIn(' ', name)


class SourceCommit(unittest.TestCase):
    def test_a_changed_copy_has_a_commit_of_its_own(self):   # the app unpacks its sources once per commit (Setup.swift prepareSource): two copies of other changes must differ
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            (root / 'a.txt').write_text('one')
            out = {'rev-parse': '0123456789ab\n', 'status': ' M a.txt\n'}
            with mock.patch.object(build_gui, 'REPO', root), mock.patch.object(build_gui, 'git', lambda *a: out[a[0]]):
                first = build_gui.commit(['a.txt'])
                self.assertRegex(first, r'^0123456789ab\+dirty\.[0-9a-f]{8}$')
                self.assertEqual(build_gui.commit(['a.txt']), first)
                (root / 'a.txt').write_text('two')
                self.assertNotEqual(build_gui.commit(['a.txt']), first)
                out['status'] = ''
                self.assertEqual(build_gui.commit(['a.txt']), '0123456789ab')

    def test_a_copy_of_the_sources_names_its_commit(self):
        with tempfile.TemporaryDirectory() as t:
            root = Path(t)
            (root / 'dm/mac').mkdir(parents=True)
            (root / '.source-commit').write_text('0123456789ab+dirty.0a1b2c3d\n')
            with mock.patch.object(pack_app, 'HERE', root / 'dm/mac'):
                self.assertEqual(pack_app.source_commit(), '0123456789ab+dirty.0a1b2c3d')
                (root / '.source-commit').unlink()
                self.assertIsNone(pack_app.source_commit())


@unittest.skipUnless(xcode(), 'needs Xcode (swiftc, actool, codesign)')
class PackedApp(unittest.TestCase):
    def test_pack(self):
        with tempfile.TemporaryDirectory() as t:
            out = Path(t) / f'{build_gui.NAME}.app'
            build_gui.build(out, say=lambda s: None)
            contents = out / 'Contents'
            info = plistlib.loads((contents / 'Info.plist').read_bytes())
            self.assertEqual(info['CFBundleIdentifier'], build_gui.BUNDLE_ID)
            exe = contents / 'MacOS' / build_gui.NAME
            self.assertIn('arm64', subprocess.run(['file', exe], capture_output=True, text=True).stdout)
            setup = json.loads((contents / 'Resources/setup.json').read_text())
            self.assertEqual(setup['nups'], build_gui.known_nups())
            self.assertTrue((contents / 'Resources/Assets.car').is_file())
            with tarfile.open(contents / 'Resources/source.tar.gz') as tar:
                names = set(tar.getnames())
            self.assertIn('scripts/build_app.py', names)
            self.assertIn('dm/games/0400486024DEC000/nup.tsv', names)
            self.assertEqual(subprocess.run(['codesign', '--verify', '--strict', out], capture_output=True).returncode, 0)
            self.assertFalse(out.with_name(out.name + '.partial').exists())


if __name__ == '__main__':
    unittest.main()
