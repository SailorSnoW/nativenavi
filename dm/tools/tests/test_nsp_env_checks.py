"""build_app.py's environment checks, all before the game is unpacked: a shader translator that does not start, a missing Python module, a Metal compiler that does
not run, and a Python under Rosetta each stop the build at once instead of at a shader step minutes later.
Every input (xcrun, sysctl, the translator, PATH) is mocked: no Xcode or Rosetta needed."""
import importlib.util
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import types
import unittest
from unittest import mock

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_app', REPO / 'scripts/build_app.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
sys.path.insert(0, str(REPO / 'dm/gfx/tools'))
import dmsh_chain  # noqa: E402
import tool_lock  # noqa: E402


def fake_run(metal_rc=0, xlate=(0, 'f21af1efa737a270\n', ''), translated='0', calls=None):
    def run(cmd, **kw):
        cmd = [str(c) for c in cmd]
        if calls is not None: calls.append(cmd)
        if cmd[0] == 'xcrun' and 'metal' in cmd: return subprocess.CompletedProcess(cmd, metal_rc, '', 'error: cannot execute tool \'metal\' due to missing Metal Toolchain' if metal_rc else '')
        if cmd[0] == 'sysctl': return subprocess.CompletedProcess(cmd, 0, translated + '\n', '')
        if cmd[-1] == '--source-sha':
            if isinstance(xlate, BaseException): raise xlate
            return subprocess.CompletedProcess(cmd, *xlate)
        return subprocess.CompletedProcess(cmd, 0, '', '')
    return run


def fake_capstone(major):   # the installed capstone's version does not matter to these tests
    return types.SimpleNamespace(__version__=f'{major}.0.0', cs_version=lambda: (major, 0, major << 8))


class ToolPath(unittest.TestCase):   # issue #1474: TOOL_PATHS are fallbacks, so the tools the caller's PATH selects are the ones that run
    def test_caller_tools_win_and_fallbacks_find_the_rest(self):
        with tempfile.TemporaryDirectory() as folder:
            caller, fallback = Path(folder) / 'Project Tools', Path(folder) / 'fallback'   # a space in the caller's folder
            for directory, names in ((caller, ('zstd', 'make')), (fallback, ('zstd', 'make', 'spirv-cross'))):
                directory.mkdir()
                for name in names: tool = directory / name; tool.write_text('#!/bin/sh\n'); tool.chmod(0o755)
            with mock.patch.object(nsp, 'TOOL_PATHS', [str(fallback)]):
                env = nsp.tool_env({'PATH': str(caller), 'HOME': '/h'})
            self.assertEqual(env, {'PATH': os.pathsep.join([str(caller), str(fallback)]), 'HOME': '/h'})
            for name in ('zstd', 'make'): self.assertEqual(shutil.which(name, path=env['PATH']), str(caller / name))
            self.assertEqual(shutil.which('spirv-cross', path=env['PATH']), str(fallback / 'spirv-cross'))

    def test_no_path_takes_the_system_path(self):   # unset or empty: an empty PATH entry would search the current folder
        for environ in ({}, {'PATH': ''}):
            with self.subTest(environ=environ), mock.patch.object(nsp, 'TOOL_PATHS', ['/fallback']):
                self.assertEqual(nsp.tool_env(environ)['PATH'], '/usr/bin:/bin:/usr/sbin:/sbin:/fallback')


class Prereqs(unittest.TestCase):
    def check(self, which=lambda *a, **k: '/x', capstone=5, **kw):
        calls = []
        with tempfile.TemporaryDirectory() as d, mock.patch.object(nsp, '_importable', lambda m: True), mock.patch.object(nsp.shutil, 'which', which), \
                mock.patch.object(nsp.subprocess, 'run', fake_run(calls=calls, **kw)), mock.patch.dict(sys.modules, capstone=fake_capstone(capstone)):
            xlate = Path(d) / 'dmsh_xlate'; xlate.write_text('')
            try: nsp.check_prereqs({'PATH': '/usr/bin'}, xlate)
            finally: self.calls = calls

    def test_only_the_mac_sdk_is_asked_for(self):
        self.check()
        self.assertTrue(any(c[:3] == ['xcrun', '-sdk', 'macosx'] and 'metal' in c for c in self.calls))
        self.assertFalse(any('iphoneos' in c for c in self.calls))

    def test_metal_compiler_that_does_not_run_stops(self):   # Xcode installs the Metal toolchain separately
        with self.assertRaisesRegex(nsp.BuildError, r'Metal compiler does not run \(.*missing Metal Toolchain\).*downloadComponent MetalToolchain'): self.check(metal_rc=1)

    def test_missing_tools_are_named(self):
        with self.assertRaisesRegex(nsp.BuildError, r'tools not found: make \(Xcode\)$'): self.check(which=lambda tool, **k: None if tool == 'make' else '/x')
        for gone in ('xcodegen', 'glslangValidator', 'zstd'):   # what the removed paths ran: no longer asked for
            with self.subTest(gone=gone): self.check(which=lambda tool, gone=gone, **k: None if tool == gone else '/x')

    def test_missing_cargo_names_rustup(self):   # the host has a Rust library (dm/host/rust)
        with self.assertRaisesRegex(nsp.BuildError, r'tools not found: cargo \(Rust: install rustup, https://rustup\.rs\)$'):
            self.check(which=lambda tool, **k: None if tool == 'cargo' else '/x')

    def test_rust_toolchain_is_resolved_in_the_rust_folder(self):   # where rustup reads rust-toolchain.toml (and installs the version it names)
        run = mock.Mock(side_effect=fake_run())
        with mock.patch.object(nsp.subprocess, 'run', run), mock.patch.object(nsp.shutil, 'which', lambda *a, **k: '/home/.cargo/bin/cargo'):
            nsp.check_rust({'PATH': '/bin'}, Path('/repo/dm/host/rust'))
        self.assertEqual(run.call_args.args[0], ['/home/.cargo/bin/cargo', '--version'])
        self.assertEqual(run.call_args.kwargs['cwd'], Path('/repo/dm/host/rust'))

    def test_rust_toolchain_that_cannot_be_had_stops(self):
        failed = mock.Mock(return_value=subprocess.CompletedProcess([], 1, '', 'error: toolchain 1.95.0 is not installed\n'))
        with mock.patch.object(nsp.subprocess, 'run', failed), self.assertRaisesRegex(nsp.BuildError, r'is not available \(error: toolchain 1\.95\.0 is not installed\).*rustup toolchain install'):
            nsp.check_rust({'PATH': '/bin'}, Path('/repo/dm/host/rust'))

    def test_translator_runs_once_with_the_scripts_environment(self):
        run = mock.Mock(side_effect=fake_run())
        with tempfile.TemporaryDirectory() as d, mock.patch.object(nsp.subprocess, 'run', run):
            xlate = Path(d) / 'dmsh_xlate'; nsp.check_translator(xlate, {'PATH': '/bin', 'TMPDIR': '/work/tmp/'})
        self.assertEqual(run.call_args.args[0], [str(xlate), '--source-sha'])
        self.assertEqual(run.call_args.kwargs['env']['TMPDIR'], '/work/tmp/'); self.assertTrue(run.call_args.kwargs['timeout'])

    def test_translator_that_does_not_run_stops(self):   # its first error line, not an empty `xlate_source` at the shader step
        with self.assertRaises(nsp.BuildError) as e: self.check(xlate=(137, '', 'Killed: 9\nmore'))
        self.assertIn('does not run (Killed: 9)', str(e.exception)); self.assertIn('check out the repository again', str(e.exception))
        with self.assertRaisesRegex(nsp.BuildError, r'does not run \(exit code 0\)'): self.check(xlate=(0, ' \n', ''))

    def test_translator_that_cannot_start_or_hangs_stops(self):
        for exc in (OSError('Exec format error'), subprocess.TimeoutExpired('dmsh_xlate', 60)):
            with self.subTest(exc=exc), self.assertRaisesRegex(nsp.BuildError, 'does not start'): self.check(xlate=exc)

    def test_zbic_is_a_prerequisite(self):   # README installs it; a game with ZBIC modules would otherwise stop only at the rewrite, after its whole RomFS is unpacked (Ocarina of Time: 23 minutes)
        with mock.patch.object(nsp, '_importable', lambda m: m != 'zbic'), self.assertRaisesRegex(nsp.BuildError, r'missing modules: zbic \(pip install .*zbic'):
            nsp.check_prereqs({'PATH': '/usr/bin'}, Path('/nonexistent'))

    def test_capstone_5_passes(self):
        with mock.patch.dict(sys.modules, capstone=fake_capstone(5)): nsp.check_capstone()

    def test_other_capstone_stops_with_the_install_command(self):   # Capstone 6 renamed CS_ARCH_ARM64 and the operand constants patch_nso.py uses
        for major in (4, 6):
            with self.subTest(major=major), mock.patch.dict(sys.modules, capstone=fake_capstone(major)):
                with self.assertRaisesRegex(nsp.BuildError, rf'Capstone {major}\.0\.0 is unsupported.*-m pip install .capstone>=5,<6'): nsp.check_capstone()
        with self.assertRaisesRegex(nsp.BuildError, 'Capstone 6'): self.check(capstone=6, which=lambda *a, **k: None)   # before the tools and the Metal check


class Rosetta(unittest.TestCase):
    def check(self, translated, platform='darwin'):
        with mock.patch.object(nsp.sys, 'platform', platform), mock.patch.object(nsp.subprocess, 'run', fake_run(translated=translated)):
            nsp.check_native_python()

    def test_translated_stops(self):
        with self.assertRaisesRegex(nsp.BuildError, r'running under Rosetta.*arm64 Python'): self.check('1')

    def test_native_or_intel_mac_passes(self):   # an Intel Mac has no sysctl.proc_translated
        self.check('0'); self.check('')

    def test_other_systems_are_not_asked(self):
        with mock.patch.object(nsp.sys, 'platform', 'linux'), mock.patch.object(nsp.subprocess, 'run', side_effect=AssertionError): nsp.check_native_python()


class ChainCheck(unittest.TestCase):   # dmsh_build.py run directly (tool_lock.check_tools) names the translator that does not run, not an empty `xlate_source`
    def embedded(self, xlate):
        with mock.patch.object(tool_lock.subprocess, 'run', fake_run(xlate=xlate)): return tool_lock.xlate_embedded()

    def test_embedded(self):
        self.assertEqual(self.embedded((0, 'f21af1efa737a270\n', '')), 'f21af1efa737a270')
        self.assertEqual(self.embedded((137, '', 'Killed: 9\nmore')), 'does not run (Killed: 9)')
        self.assertEqual(self.embedded((0, ' \n', '')), 'does not run (exit code 0)')
        for exc in (OSError('Exec format error'), subprocess.TimeoutExpired('dmsh_xlate', 60)):
            with self.subTest(exc=exc): self.assertTrue(self.embedded(exc).startswith('does not run ('))

    def check(self, xlate_source):
        with open(dmsh_chain.LOCK) as f: lock = dict(l.rstrip('\n').split('\t', 1) for l in f if '\t' in l and not l.startswith('#'))
        with mock.patch.object(tool_lock, 'tools_now', lambda: {**lock, 'xlate_source': xlate_source}): return tool_lock.check_tools()

    def test_checks_are_outside_the_chain(self):   # a fix to a check changes neither V nor any shader's cache key (dmsh_build.py CODE_FP takes dmsh_chain.py's code form)
        self.assertNotIn('tools/tool_lock.py', dmsh_chain.FILES)
        for name in ('tools_now', 'check_tools', 'xlate_embedded'): self.assertFalse(hasattr(dmsh_chain, name), name)

    def test_check_tools(self):
        bad = self.check('does not run (Killed: 9)')
        self.assertEqual(len(bad), 1, bad); self.assertIn('the translator does not run (Killed: 9)', bad[0]); self.assertIn('native_tools.sh', bad[0])
        bad = self.check('0123456789abcdef')   # a translator that runs but was built from other source: the lock line and the rebuild line
        self.assertEqual(len(bad), 2, bad); self.assertIn('xlate_source: lock', bad[0]); self.assertIn('native_tools.sh dmsh_xlate', bad[1])


if __name__ == '__main__': unittest.main()
