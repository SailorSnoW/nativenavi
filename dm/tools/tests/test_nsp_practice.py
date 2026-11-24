"""The packing script's practice tools stage (scripts/build_app.py practice_tools; the game's note, "Practice tools"): the release the profile pins in practice.tsv, taken from
the build folder or downloaded (curl, here from a file:// URL) and refused with other bytes; its module rewritten, the IPS of main's build alone and the license in
<rewritten modules>/practice; the host's check (DM_MODS_CHECK with DM_PRACTICE=1, no mods folder) before they go in, a refusal or a red line the same check with DM_PRACTICE=0
does not log leaving the build without them (the game's own red lines do not);
reuse while the inputs are unchanged; off with --no-practice. No game files: a stand-in rewriter and a stand-in host (shell scripts) record what they were given."""
import hashlib
import importlib.util
import os
from pathlib import Path
import shutil
import tempfile
import types
import unittest
from unittest import mock
import zipfile

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('build_app', REPO / 'scripts/build_app.py')
assert spec and spec.loader
nsp = importlib.util.module_from_spec(spec); spec.loader.exec_module(nsp)
TEST_ROOT = Path(os.environ.get('NATIVENAVI_TEST_ROOT', tempfile.gettempdir()))
BID = 'e0f3ccfaf50065ae4ea01eb5aeb13b1a'   # main's build ID as build_id reads it: the first 16 of the 20 bytes the IPS names
IPS, OTHER = 'E0F3CCFAF50065AE4EA01EB5AEB13B1A45DA3348.ips', '33FA6C2C23848282A77E8B88A0A93F4887C55920.ips'

# patch_nso.py's stand-in: <py> <patch_nso.py> <module> <out> <name>; counts its runs in $CALLS
REWRITER = '''#!/bin/sh
echo rewrite >> "$CALLS"
printf 'code' > "$3/$4.patched.bin"; printf 'fixups' > "$3/$4.fixups.tsv"; printf 'gates' > "$3/$4.gates.tsv"; printf 'sites' > "$3/$4.sites.tsv"
'''
# the host's stand-in: answers DM_MODS_CHECK with the verdict $VERDICT on the practice folder's IPS, as hle_mods.c does; $RED prints a red line with the practice tools on, $GAME_RED
# one in either run (the game's own); $RC is its exit code
HOST = '''#!/bin/sh
echo "host $1 practice=$DM_PRACTICE mods=$DM_MODS window=${DM_WINDOW-unset}" >> "$CALLS"
[ -n "$GAME_RED" ] && echo "[dm] [RED] $GAME_RED at 0x$$"
[ "$DM_PRACTICE" = 1 ] || { echo "[dm] mods: DM_MODS_CHECK: the patch files' verdicts are written, the game is not started"; exit ${RC-0}; }
[ -f "$1/practice/subsdk9.fixups.tsv" ] || { echo "[dm] [RED] practice tools: none"; exit 0; }
for f in "$1"/practice/*.ips; do printf 'file\\tmain\\t%s\\t%s\\tpractice tools\\t%s\\tsha\\t%s\\n' "$BID" "${VERDICT-applied}" "$f" "${WHY--}" >> "$DM_MODS_CHECK"; done
[ -n "$RED" ] && echo "[dm] [RED] $RED"
echo "[dm] mods: DM_MODS_CHECK: the patch files' verdicts are written, the game is not started"
exit ${RC-0}
'''


class Stage(unittest.TestCase):
    def setUp(self):
        TEST_ROOT.mkdir(parents=True, exist_ok=True)
        self.t = t = Path(tempfile.mkdtemp(prefix='practice-', dir=TEST_ROOT)).resolve()
        self.addCleanup(shutil.rmtree, t)
        self.zip = t / 'release/sloperinaGZ-9.9.9.zip'; self.zip.parent.mkdir()
        with zipfile.ZipFile(self.zip, 'w') as z:
            z.writestr('rel/files/exefs/subsdk9', b'NSO0 module'); z.writestr(f'rel/files/exefs/{IPS}', b'PATCH this build')
            z.writestr(f'rel/files/exefs/{OTHER}', b'PATCH another build'); z.writestr(f'rel/files/{IPS}', b'PATCH a copy elsewhere'); z.writestr('rel/LICENSE', b'GPL-3.0')
        self.profile = t / 'profile'; self.profile.mkdir()
        self.pin(hashlib.sha256(self.zip.read_bytes()).hexdigest())
        self.work, self.game, self.patched = t / 'work', t / 'game', t / 'work/patched_v1'
        (self.game / 'ExeFS').mkdir(parents=True); self.patched.mkdir(parents=True); (self.work / 'tmp').mkdir()
        (self.game / 'ExeFS/main').write_bytes(b'NSO0'.ljust(0x40, b'\0') + bytes.fromhex(BID) + bytes(16))
        for n in ('main.patched.bin', 'main.fixups.tsv', 'sdk.patched.bin', 'sdk.fixups.tsv'): (self.patched / n).write_text(n)
        self.py, self.host, self.calls_file = t / 'py', t / 'dmhost', t / 'calls.txt'
        self.py.write_text(REWRITER); self.host.write_text(HOST)
        for p in (self.py, self.host): p.chmod(0o755)
        self.env = dict(os.environ, CALLS=str(self.calls_file), BID=BID, DM_GAME='0400486024DEC000', DM_WINDOW='1')
        self.said = []
        for p in (mock.patch.object(nsp, 'PROG', mock.Mock()), mock.patch.object(nsp, 'say', self.said.append)):
            p.start(); self.addCleanup(p.stop)

    def pin(self, sha, **rows):
        rows = {'release': '9.9.9', 'url': self.zip.as_uri(), 'sha256': sha, 'module': 'rel/files/exefs/subsdk9', 'patches': 'rel/files/exefs', 'license': 'rel/LICENSE', **rows}
        (self.profile / 'practice.tsv').write_text('# a comment\n' + ''.join(f'{k}\t{v}\n' for k, v in rows.items()))

    def stage(self, on=True, env=None):   # env: the stand-in host's switches for this run
        g = types.SimpleNamespace(table=lambda n: self.profile / n)
        return nsp.practice_tools(self.py, REPO, self.work, self.game, self.patched, self.host, dict(self.env, **(env or {})), self.work / 'logs', g, on)

    def ran(self): return self.calls_file.read_text().splitlines() if self.calls_file.is_file() else []

    def test_rewritten_checked_and_reused(self):
        out, red = self.stage()
        self.assertEqual((out, red), (self.patched / 'practice', None))
        self.assertEqual(sorted(p.name for p in out.iterdir()), sorted([IPS, 'LICENSE', 'subsdk9.fixups.tsv', 'subsdk9.gates.tsv', 'subsdk9.patched.bin', 'subsdk9.sites.tsv']))
        self.assertEqual((out / IPS).read_bytes(), b'PATCH this build')   # the patches folder's IPS of main's build, not the other build's nor a copy elsewhere in the zip
        self.assertEqual((out / 'LICENSE').read_bytes(), b'GPL-3.0')
        self.assertTrue((self.work / 'practice/sloperinaGZ-9.9.9.zip').is_file())   # downloaded once into the build folder
        rewrite, host, base = self.ran()
        self.assertEqual(rewrite, 'rewrite')
        chk = self.work / 'tmp/practice-check'
        self.assertEqual(host, f'host {chk} practice=1 mods={self.work / "tmp/practice-no-mods"} window=unset')   # the player's mods are not the check's; no window
        self.assertEqual(base, f'host {chk} practice=0 mods={self.work / "tmp/practice-no-mods"} window=unset')   # the same check without them: the game's own red lines
        self.assertFalse((self.work / 'tmp/practice-no-mods').exists())
        self.assertEqual(sorted(p.name for p in chk.iterdir()), ['main.fixups.tsv', 'main.patched.bin', 'practice', 'sdk.fixups.tsv', 'sdk.patched.bin'])
        self.assertFalse((self.patched / 'practice.partial').exists())
        self.calls_file.unlink()
        self.assertEqual(self.stage(), (out, None))
        self.assertEqual(self.ran(), [])   # reused: nothing rewritten or checked again
        (self.host).write_text(HOST + '\n')   # another host checks them again
        self.assertEqual(self.stage(), (out, None))
        self.assertEqual(len(self.ran()), 3)

    def test_the_zip_in_the_build_folder_is_taken(self):   # no download: the pin's file:// URL is gone, the zip is where the build keeps it
        (self.work / 'practice').mkdir(); shutil.copy(self.zip, self.work / 'practice')
        self.pin(hashlib.sha256(self.zip.read_bytes()).hexdigest(), url='file:///nonexistent/sloperinaGZ-9.9.9.zip')
        self.assertEqual(self.stage()[1], None)

    def test_other_bytes_are_refused(self):
        self.pin('0' * 64)
        out, red = self.stage()
        self.assertIsNone(out)
        self.assertRegex(red, r'^\[RED\] practice tools: sloperinaGZ 9\.9\.9 is not in this build: .* has the SHA-256 [0-9a-f]{64}, not the profile\'s 0{64}')
        self.assertEqual(self.ran(), []); self.assertFalse((self.work / 'practice/sloperinaGZ-9.9.9.zip').exists())
        self.assertFalse((self.patched / 'practice').exists())

    def test_a_zip_of_other_bytes_in_the_build_folder_goes_to_obsolete(self):
        (self.work / 'practice').mkdir(); (self.work / 'practice/sloperinaGZ-9.9.9.zip').write_bytes(b'not the release'); (self.work / 'practice/sloperinaGZ-0.0.1.zip').write_bytes(b'old')
        self.assertEqual(self.stage()[1], None)   # downloaded again
        self.assertEqual(hashlib.sha256((self.work / 'practice/sloperinaGZ-9.9.9.zip').read_bytes()).digest(), hashlib.sha256(self.zip.read_bytes()).digest())
        kept = sorted(p.name for p in (self.work / 'obsolete').rglob('*.zip'))
        self.assertEqual(kept, ['sloperinaGZ-0.0.1.zip', 'sloperinaGZ-9.9.9.zip'])

    def test_a_failed_download_goes_on_without_them(self):
        self.pin('0' * 64, url='file:///nonexistent/sloperinaGZ-9.9.9.zip')
        out, red = self.stage()
        self.assertIsNone(out); self.assertIn('the download of file:///nonexistent/sloperinaGZ-9.9.9.zip failed', red)
        self.assertIn(str(self.work / 'practice/sloperinaGZ-9.9.9.zip'), red)   # where to put the zip by hand

    def test_the_hosts_refusal_or_red_line_leaves_them_out(self):
        out, _ = self.stage()
        for env, why in (({'VERDICT': 'refused', 'WHY': 'replaces an instruction the rewriter already moved out of line'}, f'the host refused {IPS} (refused: replaces an instruction'),
                         ({'RED': 'sdk: something'}, 'the host\'s check logged 1 red line(s) the game without the practice tools does not, the first: [dm] [RED] sdk: something'),
                         ({'RC': '3'}, 'the host\'s check failed (exit code 3')):
            with self.subTest(env=env):
                self.host.write_text(HOST + f'# {env}\n')   # a new host: checked again
                out, red = self.stage(env=env)
                self.assertIsNone(out); self.assertIn(why, red)
                self.assertFalse((self.patched / 'practice').exists()); self.assertFalse((self.patched / 'practice.key').exists())
                self.assertFalse((self.patched / 'practice.partial').exists())

    def test_the_games_own_red_lines_do_not(self):   # 1.0.0: the host's forced takeovers its main references none of, in the check with the practice tools and without
        out, red = self.stage(env={'GAME_RED': 'forced takeover entry no module references: _ZN2nn3aoc17CountAddOnContentEv'})
        self.assertEqual((out, red), (self.patched / 'practice', None))
        self.host.write_text(HOST + '# again\n')
        out, red = self.stage(env={'GAME_RED': 'aoc', 'RC': '0'})
        self.assertIsNone(red)

    def test_no_patch_for_this_main(self):   # another version of the game: no red line, no folder
        (self.game / 'ExeFS/main').write_bytes(b'NSO0'.ljust(0x40, b'\0') + bytes(32))
        self.assertEqual(self.stage(), (None, None))
        self.assertEqual(self.ran(), [])
        self.assertTrue(any('has no patch for this version\'s main (build 000000' in s for s in self.said))

    def test_off(self):   # --no-practice: what an earlier build put there goes, nothing is downloaded
        self.stage(); self.calls_file.unlink(); shutil.rmtree(self.work / 'practice')
        self.assertEqual(self.stage(on=False), (None, None))
        self.assertFalse((self.patched / 'practice').exists()); self.assertFalse((self.patched / 'practice.key').exists())
        self.assertFalse((self.work / 'practice').exists()); self.assertEqual(self.ran(), [])

    def test_a_profile_without_the_file(self):
        (self.profile / 'practice.tsv').unlink()
        self.assertEqual(self.stage(), (None, None))

    def test_a_pin_without_a_row(self):
        (self.profile / 'practice.tsv').write_text('release\t1\n')
        with self.assertRaisesRegex(nsp.BuildError, 'no url, sha256, module, patches, license row'): self.stage()


class Profile(unittest.TestCase):
    def test_the_game_pins_one_release(self):   # one zip, by digest, with the files the stage takes inside the release's own folder
        g = types.SimpleNamespace(table=lambda n: REPO / 'dm/games/0400486024DEC000' / n)
        pin = nsp.practice_pin(g)
        self.assertRegex(pin['url'], rf'^https://github\.com/PracticeROM/sloperina/releases/download/v{pin["release"]}/sloperinaGZ-{pin["release"]}\.zip$')
        top = f'sloperinaGZ-{pin["release"]}/'
        self.assertTrue(all(pin[k].startswith(top) for k in ('module', 'patches', 'license')))
        self.assertEqual(pin['module'].rsplit('/', 1)[-1], nsp.PRACTICE_MODULE)
        self.assertIn(f'#define PRACTICE_MODULE "{nsp.PRACTICE_MODULE}"', (REPO / 'dm/host/hle_practice.c').read_text())
        self.assertIn(f'"%s/{nsp.PRACTICE_DIR}"', (REPO / 'dm/host/hle_practice.c').read_text())


if __name__ == '__main__': unittest.main()
