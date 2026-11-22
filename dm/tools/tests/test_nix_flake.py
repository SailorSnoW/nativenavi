"""What flake.nix and dm/nix/shell_hook.sh read from other files still matches their patterns (CI has no Nix to evaluate the flake)."""
from pathlib import Path
import re
import shutil
import subprocess
import sys
import tempfile
import tomllib
import unittest

REPO = Path(__file__).resolve().parents[3]


class Flake(unittest.TestCase):
    def setUp(self):
        self.flake = (REPO / 'flake.nix').read_text()

    def test_lint_c_has_the_lines_the_flake_reads(self):
        lines = (REPO / '.github/workflows/lint-c.yml').read_text().split('\n')
        for key, value in (('LLVM_VERSION', r'\d+\.\d+\.\d+'), ('LLVM_SHA256', r'[0-9a-f]{64}')):
            self.assertIn(f'workflowEnv lintC "{key}"', self.flake)
            found = [m[1] for line in lines if (m := re.fullmatch(f' *{key}: *([^ #]+).*', line))]
            self.assertTrue(found, f'lint-c.yml has no {key} line for flake.nix')
            self.assertRegex(found[0], f'^{value}$', f'lint-c.yml: {key} as flake.nix reads it')

    def test_requirement_files_exist(self):
        files = [f for group in re.findall(r'requirements = \[([^\]]*)\]', self.flake) for f in re.findall(r'"([^"]+)"', group)]
        self.assertTrue(files, 'flake.nix names no requirement files')
        for f in files: self.assertTrue((REPO / '.github/requirements' / f).is_file(), f'flake.nix names .github/requirements/{f}, which does not exist')

    def test_fuzz_shell_installs_the_documented_atheris(self):
        self.assertRegex((REPO / '.github/requirements/fuzz.txt').read_text(), r'(?m)^atheris==')
        self.assertIn('NXNATIVE_PIP_SKIP = "atheris"', self.flake)
        commit = re.search(r'git\+https://github\.com/google/atheris@([0-9a-f]{40})', self.flake)
        if commit is None: self.fail('flake.nix names no atheris commit')
        self.assertIn(f'github.com/google/atheris@{commit[1]}', (REPO / 'docs/development.md').read_text(), 'flake.nix and docs/development.md name other atheris commits')

    def test_hook_reads_the_xcode_version(self):
        self.assertRegex((REPO / 'dm/gfx/tools/own_compilers.py').read_text(), r"(?m)^XCODE_VERSION = '[0-9.]+'")

    def test_rust_toolchain_has_what_the_flake_reads(self):   # the flake takes rust-overlay's stable.<channel>, the version rustup takes outside Nix
        self.assertIn('lib.importTOML ./dm/host/rust/rust-toolchain.toml', self.flake)
        toolchain = tomllib.loads((REPO / 'dm/host/rust/rust-toolchain.toml').read_text())['toolchain']
        self.assertRegex(toolchain['channel'], r'^\d+\.\d+\.\d+$', 'rust-toolchain.toml: a version, which rust-bin.stable has, not "stable"')
        self.assertIn('aarch64-apple-darwin', toolchain['targets'])

    def test_hook_without_requirements_makes_no_venv(self):   # the miri shell: shasum with no files would have waited on the terminal
        bash = shutil.which('bash')
        if bash is None: self.skipTest('no bash')
        with tempfile.TemporaryDirectory() as d:
            env = {'PATH': '/usr/bin:/bin', 'HOME': d, 'NXNATIVE_SHELL': 'miri', 'NXNATIVE_REQUIREMENTS': '', 'NXNATIVE_NO_XCODE_CHECK': '1', 'NXNATIVE_TOOLS': '',
                   'NXNATIVE_STDENV_PATH': '', 'NXNATIVE_PYTHON': sys.executable, 'NXNATIVE_VENVS': d}
            p = subprocess.Popen([bash, '-c', f'source {REPO / "dm/nix/shell_hook.sh"}; printf "%s\\n" "$CARGO_HOME"'], cwd=REPO, env=env, text=True,
                                 stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.PIPE)   # stdin stays open: a read of it would hang
            try: p.wait(timeout=30)
            except subprocess.TimeoutExpired: p.kill(); self.fail('the hook read stdin')
            out, err = p.stdout.read(), p.stderr.read(); p.stdin.close(); p.stdout.close(); p.stderr.close()
            self.assertEqual((p.returncode, out.strip()), (0, f'{d}/cargo'), err)   # a CARGO_HOME of the shells' own: rustup's proxies in ~/.cargo/bin are not cargo's subcommands
            self.assertFalse((Path(d) / 'miri').exists(), 'no venv for a shell without requirements')

    def test_hook_parses(self):
        bash = shutil.which('bash')
        if bash is None: self.skipTest('no bash')
        r = subprocess.run([bash, '-n', str(REPO / 'dm/nix/shell_hook.sh')], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
