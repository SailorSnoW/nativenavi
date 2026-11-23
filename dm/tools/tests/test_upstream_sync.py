# dm/tools/upstream_sync.py on synthetic repositories: what list tags and leaves out, pick's three-way merges (clean, conflict, missing, added, generated), the ledger, a log's summary
import contextlib, importlib.util, io, subprocess, tempfile, unittest
from collections.abc import Mapping
from pathlib import Path
from typing import Any

HERE = Path(__file__).resolve().parent
spec = importlib.util.spec_from_file_location('upstream_sync', HERE.parent / 'upstream_sync.py')
assert spec is not None and spec.loader is not None
us: Any = importlib.util.module_from_spec(spec)
spec.loader.exec_module(us)


def git(repo: Path, *args: str) -> str:
    return subprocess.run(['git', '-C', str(repo), *args], capture_output=True, text=True, check=True).stdout


def commit(repo: Path, files: Mapping[str, str | None], msg: str) -> str:
    for p, text in files.items():
        f = repo / p
        if text is None: f.unlink(); continue
        f.parent.mkdir(parents=True, exist_ok=True); f.write_text(text)
    git(repo, 'add', '-A'); git(repo, '-c', 'user.name=t', '-c', 'user.email=t@t', 'commit', '-q', '-m', msg)
    return git(repo, 'rev-parse', 'HEAD').strip()


def run(*argv: str) -> tuple[str, int]:
    out = io.StringIO(); code = 0
    with contextlib.redirect_stdout(out), contextlib.redirect_stderr(io.StringIO()):
        try: us.main(list(argv))
        except SystemExit as e: code = e.code if isinstance(e.code, int) else 1
    return out.getvalue(), code


class UpstreamSync(unittest.TestCase):
    def setUp(self) -> None:
        self.td = tempfile.TemporaryDirectory(); root = Path(self.td.name)
        self.up, self.tree = root / 'up', root / 'tree'
        self.up.mkdir(); git(self.up, 'init', '-q', '-b', 'main')
        base = {'a.txt': 'one\ntwo\nthree\n', 'gone.txt': 'x\n', 'dm/games/README.md': '| table |\n', 'dm/games/0400486024DEC000/README.md': 'note\n',
                'dm/games/0100000000000001/game.tsv': 'k\tv\n', 'docs/agents/symptoms.md': 'index\n'}
        self.base = commit(self.up, base, 'base')
        self.c1 = commit(self.up, {'a.txt': 'one\ntwo\nthree\nfour\n', 'gone.txt': 'y\n', 'new.txt': 'added\n', 'docs/agents/symptoms.md': 'index 2\n'}, 'host: a change')
        self.c2 = commit(self.up, {'dm/games/0100000000000001/game.tsv': 'k\tw\n', 'dm/games/README.md': '| table 2 |\n'}, 'game: another game')
        self.c3 = commit(self.up, {'a.txt': 'ONE\ntwo\nthree\nfour\n'}, 'host: the first line')
        # the fork: the base less gone.txt and the other game, a line of its own in a.txt
        self.tree.mkdir(); git(self.tree, 'init', '-q')
        commit(self.tree, {'a.txt': 'one\ntwo (fork)\nthree\n', 'dm/games/README.md': '| table |\n', 'dm/games/0400486024DEC000/README.md': 'note\n', 'docs/agents/symptoms.md': 'index\n'}, 'fork')
        self.ledger = root / 'ledger.tsv'
        self.ledger.write_text(f'# base\t{self.base[:8]}\n')
        self._ledger = us.LEDGER; us.LEDGER = self.ledger
        self._defaults = us.read_ledger.__defaults__, us.write_ledger.__defaults__
        us.read_ledger.__defaults__ = (self.ledger,); us.write_ledger.__defaults__ = (self.ledger,)

    def tearDown(self) -> None:
        us.LEDGER = self._ledger
        us.read_ledger.__defaults__, us.write_ledger.__defaults__ = self._defaults
        self.td.cleanup()

    def test_list_tags_and_hints(self) -> None:
        out, code = run('list', '--mirror', str(self.up), '--tree', str(self.tree))
        self.assertEqual(code, 0)
        self.assertIn('gone.txt  (gone)', out)
        self.assertIn('new.txt  (add)', out)
        self.assertIn('docs/agents/symptoms.md  (generated)', out)
        self.assertRegex(out, r'\[skip\?\] game: another game')   # another game's profile and the games table only
        self.assertNotRegex(out, r'\[skip\?\] host: a change')
        self.assertIn('dm/games/0100000000000001/game.tsv  (game:0100000000000001, gone)', out)

    def test_record_hides_a_commit_and_deferred_comes_back(self) -> None:
        self.assertEqual(run('record', self.c2, 'skip', '--why', 'another game', '--mirror', str(self.up))[1], 0)
        self.assertEqual(run('record', self.c3, 'deferred', '--why', 'waits', '--mirror', str(self.up))[1], 0)
        out, _ = run('list', '--mirror', str(self.up), '--tree', str(self.tree))
        self.assertNotIn('game: another game', out)
        self.assertIn('host: the first line [ledger: deferred]', out)
        meta, rows = us.read_ledger(self.ledger)
        self.assertEqual(meta['base'], self.base[:8])
        self.assertEqual({r.verdict for r in rows}, {'skip', 'deferred'})
        run('record', self.c3, 'ported', '--local', 'abc1234', '--mirror', str(self.up))   # a second record replaces the row
        rows = us.read_ledger(self.ledger)[1]
        self.assertEqual(sorted((r.verdict, r.local) for r in rows), [('ported', 'abc1234'), ('skip', '-')])
        self.assertEqual(run('record', self.c1, 'maybe', '--mirror', str(self.up))[1], 1)

    def test_pick_merges_adds_and_reports(self) -> None:
        out, code = run('pick', self.c1, '--mirror', str(self.up), '--tree', str(self.tree))
        self.assertEqual(code, 0, out)
        self.assertEqual((self.tree / 'a.txt').read_text(), 'one\ntwo (fork)\nthree\nfour\n')   # upstream's line and the fork's both kept
        self.assertEqual((self.tree / 'new.txt').read_text(), 'added\n')
        self.assertIn('MISSING     gone.txt', out)
        self.assertIn('REGENERATE  docs/agents/symptoms.md', out)
        self.assertEqual((self.tree / 'docs/agents/symptoms.md').read_text(), 'index\n')   # left for its generator

    def test_pick_conflict_and_paths(self) -> None:
        (self.tree / 'a.txt').write_text('one (fork)\ntwo\nthree\nfour\n')
        out, code = run('pick', self.c3, '--mirror', str(self.up), '--tree', str(self.tree))
        self.assertEqual(code, 1)
        self.assertIn('CONFLICT(1) a.txt', out)
        self.assertIn('<<<<<<< ours', (self.tree / 'a.txt').read_text())
        out, code = run('pick', self.c1, 'new.txt', '--mirror', str(self.up), '--tree', str(self.tree))   # only the paths named
        self.assertEqual((code, out.strip()), (0, 'added       new.txt'))

    def test_summary_of_a_log(self) -> None:
        log = '\n'.join([
            '[dm] [nvn-mtl] [RED] texture type mismatch: shader 9b310b3349825e9a_b (stage 1) declares slot 7',
            '[dm] [nvn-mtl] [RED] texture type mismatch: shader 0123456789abcdef_b (stage 1) declares slot 7',
            '[dm] [nvn] power up to frame 300: CPU energy 4.1 W, 68.3 mJ per frame (59.8 fps); CPU 181%',
            '[dm] [nvn] GPU by process up to frame 300: the game 877 ms/s (14.62 ms per frame), other processes 404 ms/s: dmhost 300, WindowServer 104',
            '[dm] [nvn] timing up to frame 300: frame time p50 16.67 / p95 17.1 / max 20 ms (300 frames); GPU p50 14.20 / p95 15.10 / max 16 ms (300 frames);',
            '[dm] [nvn] residency set up to frame 300: commits 4 / 0.1 ms, added 2, 5 MB, removed 1, 5873 allocations in the set (4332 MB), named on 10'])
        text = '\n'.join(us.summary(log, [300]))
        self.assertIn('red lines: 2', text)
        self.assertIn('2 × texture type mismatch: shader <key>_b', text)
        self.assertIn('fps per 300 frames: 59.8', text)
        self.assertIn('another dmhost used the GPU: 300', text)
        self.assertIn('GPU p50 per window: 300:14.20', text)
        self.assertIn('residency set at frame 300: 5873 allocations, 4332 MB', text)


if __name__ == '__main__':
    unittest.main()
