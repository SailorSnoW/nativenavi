#!/usr/bin/env python3
# scripts/merge_prewarm_seeds.py on a fake repository: rows parsed and normalized by the host's own key parser (built with clang), checked against the
# profile's shader map, another version's rows counted and left out, the recorded lists only read.
import os, shutil, subprocess, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
SCRIPT = Path('scripts/merge_prewarm_seeds.py')
TITLE = '0400486024DEC000'
VERTEX, PIXEL, COMPUTE, MESH, TASK = (f'{n:016x}_b' for n in range(1, 6))
OLD_VERTEX = f'{6:016x}_b'   # 1.0.0's alone


def key(vs, fs='', vs_stage=0, fs_stage=1):
    """pso_skey's hex: the shader names, their stages, the render state zero"""
    return (vs.encode().ljust(24, b'\0') + fs.encode().ljust(24, b'\0') + bytes([vs_stage, fs_stage if fs else 0]) + bytes(462)).hex()


def shader_map(rows):
    return 'package\tindex\tstage\tdm_ctl_sha16\tsource\n' + ''.join(f'shaders.bin\t{i}\t{st}\t{k[:-2]}\tx\n' for i, (st, k) in enumerate(rows))


@unittest.skipUnless(shutil.which('clang'), 'the key parser needs clang')
class Merge(unittest.TestCase):
    def setUp(self):
        t = tempfile.TemporaryDirectory(); self.addCleanup(t.cleanup)
        self.root = Path(t.name); self.repo = self.root / 'repo'
        for p in (SCRIPT, Path('dm/games/game.py'), Path('dm/tools/prewarm_skey.c')):
            (self.repo / p).parent.mkdir(parents=True, exist_ok=True); shutil.copyfile(REPO / p, self.repo / p)
        shutil.copytree(REPO / 'dm/gfx/nvn', self.repo / 'dm/gfx/nvn', ignore=shutil.ignore_patterns('*.m', '*.c', '*.inc', '*.S'))
        self.profile = self.repo / 'dm/games' / TITLE; self.profile.mkdir()
        (self.profile / 'game.tsv').write_text(f'title_id\t{TITLE}\nbuild\tv131072\t1.1.1\tmain={"a" * 32}\nbuild\tv0\t1.0.0\tmain={"b" * 32}\n'
                                               'package\tv131072\nshader_versions\tv131072\n')
        (self.profile / 'shader_map.tsv').write_text(shader_map([('vertex', VERTEX), ('pixel', PIXEL), ('compute', COMPUTE), ('mesh', MESH), ('task', TASK)]))
        self.manifest = self.profile / 'pso_manifest.tsv'
        self.original = f'# Prewarm seed list\nR\t{key(VERTEX)}\n'
        self.manifest.write_text(self.original)
        self.env = dict(os.environ, PYTHONDONTWRITEBYTECODE='1'); self.env.pop('DM_GAMES_DIR', None)

    def derived(self):   # the packing script's 1.0.0 profile, in its build folder
        d = self.root / 'build/profile' / TITLE; d.mkdir(parents=True)
        (d / 'game.tsv').write_text(f'title_id\t{TITLE}\nbuild\tv0\t1.0.0\tmain={"b" * 32}\npackage\tv0\nshader_versions\tv0\n')
        (d / 'shader_map.tsv').write_text(shader_map([('vertex', VERTEX), ('vertex', OLD_VERTEX), ('pixel', PIXEL)]))
        self.env['DM_GAMES_DIR'] = str(d.parent)

    def recorded(self, name, *rows):
        p = self.root / name; p.write_text(''.join(r + '\n' for r in rows)); return p

    def merge(self, *args, check=True):
        r = subprocess.run([sys.executable, str(self.repo / SCRIPT), *map(str, args)], env=self.env, text=True, capture_output=True)
        if check: self.assertEqual(r.returncode, 0, r.stderr)
        return r

    def test_preview_then_apply(self):
        pair, mesh = f'R\t{key(VERTEX, PIXEL)}', f'M\t{TASK}\t{key(MESH, vs_stage=7)}'
        user = self.recorded('pso_user.tsv', f'R\t{key(VERTEX)[:-16]}',   # the listed row in the layout before border: the same pipeline
                             pair, mesh, f'M\t{TASK}\t{key(MESH, vs_stage=4)}', f'S\t{VERTEX}\t0', f'C\t{COMPUTE}', f'G\t{VERTEX[:-2]}\t{PIXEL[:-2]}',
                             'R\t00', f'R\t{key(f"{99:016x}_b")}', f'R\t{key(COMPUTE)}', '# a comment', 'X', pair)
        before = user.stat().st_mtime_ns, user.read_bytes()
        out = self.merge(user).stdout
        for want in ('2 added', '3 duplicate', '1 ignored C', '1 ignored G', '1 ignored S', '1 invalid pipeline', '1 invalid row', '1 missing shader',
                     '1 stage mismatch', '2 new rows (1 M, 1 R)', 'Preview'):
            self.assertIn(want, out)
        self.assertEqual(self.manifest.read_text(), self.original)
        self.assertIn('Updated', self.merge(user, '--apply').stdout)
        self.assertEqual(self.manifest.read_text(), self.original + '# The rows of pso_user.tsv that this list did not have (1 M, 1 R), merged by '
                         f'scripts/merge_prewarm_seeds.py.\n{pair}\n{mesh}\n')
        self.assertEqual((user.stat().st_mtime_ns, user.read_bytes()), before)
        self.assertIn('No new rows.', self.merge(user, '--apply').stdout)

    def test_another_versions_rows_are_left_out(self):
        old = f'R\t{key(OLD_VERTEX, PIXEL)}'
        user = self.recorded('pso_user.tsv', old, f'R\t{key(VERTEX, PIXEL)}')
        self.assertIn('1 added, 1 missing shader', self.merge(user).stdout)
        self.derived()
        self.assertIn('1 added, 1 only in 1.0.0', self.merge(user, '--apply').stdout)
        self.assertNotIn(old, self.manifest.read_text())

    def test_lists_in_order_and_a_note(self):
        a = self.recorded('a.tsv', f'R\t{key(VERTEX, PIXEL)}')
        b = self.recorded('b.tsv', f'R\t{key(VERTEX, PIXEL)}', f'M\t-\t{key(MESH, vs_stage=4)}')
        out = self.merge(a, b, '--apply', '--note', 'Rows of a test.').stdout
        self.assertIn('b.tsv: 2 rows: 1 added, 1 duplicate', out)
        self.assertEqual(self.manifest.read_text(), self.original + f'# Rows of a test.\nR\t{key(VERTEX, PIXEL)}\nM\t-\t{key(MESH, vs_stage=4)}\n')

    def test_a_list_it_cannot_read(self):
        r = self.merge(self.root / 'missing.tsv', '--apply', check=False)
        self.assertEqual(r.returncode, 1)
        self.assertIn('[RED]', r.stderr)
        self.assertEqual(self.manifest.read_text(), self.original)


if __name__ == '__main__':
    unittest.main()
