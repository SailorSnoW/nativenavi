"""The repository's C# tool is committed as a native universal executable (dm/tools/native_tools.sh, NativeAOT), so a build needs no .NET:
the shader translator dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate. It must hold both Mac architectures, and it must be built from the sources beside it, which it embeds and
names with --source-sha, and match dmsh_chain.lock. It may not import a function weakly: that is one newer than the executable's minimum macOS, which binds to 0 on a Mac without it.
It may link only macOS's own libraries: one from the machine that built it (a source-built .NET's /nix/store brotli) stops it from starting on any other Mac."""
from pathlib import Path
import hashlib
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
XLATE = REPO / 'dm/gfx/tools/dmsh_xlate_bin/dmsh_xlate'
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain   # noqa: E402


def macho_archs(data):
    magic, n = struct.unpack_from('>II', data, 0)
    assert magic == 0xCAFEBABE, 'not a universal binary'
    return sorted({0x01000007: 'x86_64', 0x0100000C: 'arm64'}.get(struct.unpack_from('>I', data, 8 + 20 * i)[0], '?') for i in range(n))


def weak_imports(data):
    """{architecture: the undefined symbols marked N_WEAK_REF} of a universal binary's 64-bit little-endian slices, as nm -m -u names them 'weak external'"""
    out = {}
    for i in range(struct.unpack_from('>I', data, 4)[0]):
        cpu, _, off, _, _ = struct.unpack_from('>5I', data, 8 + 20 * i)
        assert struct.unpack_from('<I', data, off)[0] == 0xFEEDFACF, 'not a 64-bit Mach-O slice'
        ncmds, weak, at = struct.unpack_from('<I', data, off + 16)[0], [], off + 32
        for _ in range(ncmds):
            cmd, size = struct.unpack_from('<II', data, at)
            if cmd == 2:   # LC_SYMTAB
                symoff, nsyms, stroff, _ = struct.unpack_from('<4I', data, at + 8)
                for k in range(nsyms):
                    strx, n_type, _, n_desc, _ = struct.unpack_from('<IBBHQ', data, off + symoff + 16 * k)
                    if n_type & 0xE0 == 0 and n_type & 0x0E == 0 and n_desc & 0x40:   # not a debug entry, N_UNDF, N_WEAK_REF
                        weak.append(data[off + stroff + strx:data.index(b'\0', off + stroff + strx)].decode())
            at += size
        out[{0x01000007: 'x86_64', 0x0100000C: 'arm64'}.get(cpu, '?')] = sorted(weak)
    return out


DYLIB_CMDS = {0xC, 0x20, 0x80000018, 0x8000001F, 0x80000023}   # LC_LOAD_DYLIB, LC_LAZY_LOAD_DYLIB, LC_LOAD_WEAK_DYLIB, LC_REEXPORT_DYLIB, LC_LOAD_UPWARD_DYLIB


def linked_libraries(data):
    """{architecture: the install names of the libraries each slice of a universal binary loads}, as otool -L lists them"""
    out = {}
    for i in range(struct.unpack_from('>I', data, 4)[0]):
        cpu, _, off, _, _ = struct.unpack_from('>5I', data, 8 + 20 * i)
        assert struct.unpack_from('<I', data, off)[0] == 0xFEEDFACF, 'not a 64-bit Mach-O slice'
        libs, at = [], off + 32
        for _ in range(struct.unpack_from('<I', data, off + 16)[0]):
            cmd, size = struct.unpack_from('<II', data, at)
            if cmd in DYLIB_CMDS:
                name = at + struct.unpack_from('<I', data, at + 8)[0]
                libs.append(data[name:data.index(b'\0', name)].decode())
            at += size
        out[{0x01000007: 'x86_64', 0x0100000C: 'arm64'}.get(cpu, '?')] = libs
    return out


class NativeTools(unittest.TestCase):
    def test_universal(self):
        for exe in (XLATE,):
            with self.subTest(exe=exe.name):
                self.assertEqual(macho_archs(exe.read_bytes()), ['arm64', 'x86_64'])
                self.assertTrue(exe.stat().st_mode & 0o111, f'{exe} is not executable')

    def test_no_weak_imports(self):
        for exe in (XLATE,):
            with self.subTest(exe=exe.name):
                self.assertEqual(weak_imports(exe.read_bytes()), {'arm64': [], 'x86_64': []},
                                 f"{exe.name} imports functions its minimum macOS does not have: rebuild it with Microsoft's .NET SDK (dm/tools/native_tools.sh {exe.name})")

    def test_links_only_system_libraries(self):
        for exe in (XLATE,):
            with self.subTest(exe=exe.name):
                libs = linked_libraries(exe.read_bytes())
                self.assertEqual(sorted(libs), ['arm64', 'x86_64'])
                foreign = {a: [l for l in v if not l.startswith(('/usr/lib/', '/System/Library/'))] for a, v in libs.items()}
                self.assertEqual(foreign, {'arm64': [], 'x86_64': []},
                                 f"{exe.name} links libraries a player's Mac does not have: rebuild it with Microsoft's .NET SDK (dm/tools/native_tools.sh {exe.name})")

    def test_only_the_executable_is_committed(self):   # no dlls, runtimeconfig or satellite folders of a framework-dependent build left beside it
        for exe in (XLATE,):
            with self.subTest(exe=exe.name): self.assertEqual(sorted(p.name for p in exe.parent.iterdir() if not p.name.startswith('.')), [exe.name])

    def test_projects_publish_native(self):
        for proj in ('dm/gfx/tools/dmsh_xlate/dmsh_xlate.csproj',):
            with self.subTest(proj=proj): self.assertIn('<PublishAot>true</PublishAot>', (REPO / proj).read_text())

    def test_translator_matches_the_lock(self):   # dmsh_build.py refuses another translator (tool_lock.py); this says so in CI, before any game
        with open(dmsh_chain.LOCK) as f: lock = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
        self.assertEqual(hashlib.sha256(XLATE.read_bytes()).hexdigest()[:16], lock.get('dmsh_xlate'), 'rebuilt translator: tool_lock.py --update-lock and dmsh_chain.py --header')
        self.assertEqual(lock.get('xlate_source'), dmsh_chain.xlate_source_sha(), 'dmsh_xlate sources changed: rebuild it with dm/tools/native_tools.sh dmsh_xlate')

    @unittest.skipUnless(sys.platform == 'darwin', 'runs the Mac executables')
    def test_built_from_the_sources_beside_them(self):
        want = {XLATE: dmsh_chain.xlate_source_sha()}
        for exe, sha in want.items():
            with self.subTest(exe=exe.name):
                got = subprocess.run([str(exe), '--source-sha'], capture_output=True, text=True, timeout=60).stdout.strip()
                self.assertEqual(got, sha, f'{exe.name} was not built from its in-repo sources: rebuild it with dm/tools/native_tools.sh {exe.name}')

    def test_xlate_source_sha_covers_the_vendored_ryujinx_sources(self):   # an edit there without a rebuild passed tool_lock.py
        files = {Path(p).relative_to(REPO / 'dm').as_posix() for p in dmsh_chain.xlate_source_files()}
        for f in ('gfx/tools/dmsh_xlate/Program.cs', 'gfx/tools/dmsh_xlate/dmsh_xlate.csproj', 'third_party/ryujinx/Directory.Build.props',
                  'third_party/ryujinx/src/Ryujinx.Graphics.Shader/CodeGen/Spirv/SpirvGenerator.cs', 'third_party/ryujinx/src/Ryujinx.Graphics.Shader/Translation/ResourceManager.cs'):
            self.assertIn(f, files)
        with tempfile.TemporaryDirectory() as tmp:
            dm = Path(tmp) / 'dm'; gfx = dm / 'gfx'; (gfx / 'tools/dmsh_xlate').mkdir(parents=True)
            (gfx / 'tools/dmsh_xlate/Program.cs').write_text('// a\n'); (gfx / 'tools/dmsh_xlate/dmsh_xlate.csproj').write_text('<Project />\n')
            ryu = dm / 'third_party/ryujinx/src/Ryujinx.Graphics.Shader'; ryu.mkdir(parents=True)
            spv = ryu / 'SpirvGenerator.cs'; spv.write_text('class SpirvGenerator {}\n')
            base = dmsh_chain.xlate_source_sha(str(gfx))
            (dm / 'third_party/ryujinx/README.md').write_text('not compiled\n')
            for d in ('obj', 'bin', '.git'): (ryu / d).mkdir(); (ryu / d / 'out.cs').write_text('build output\n')
            self.assertEqual(dmsh_chain.xlate_source_sha(str(gfx)), base)
            spv.write_text('class SpirvGenerator { /* edited */ }\n')
            self.assertNotEqual(dmsh_chain.xlate_source_sha(str(gfx)), base)


if __name__ == '__main__': unittest.main()
