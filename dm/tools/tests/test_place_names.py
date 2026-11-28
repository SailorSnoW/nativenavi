"""dm/tools/place_names.py: an MSBT's texts by label, tags left out, in UTF-8 and UTF-16; a damaged file refused; the archive of each language code found (the newest
Product number), its place messages read through zstd and SARC, one column per language the RomFS has. The messages and the RomFS are synthetic."""
import struct
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools'))
sys.path.insert(0, str(REPO / 'dm/tools/fuzz'))
import fuzz_shader_split as fz   # noqa: E402  its SARC builder
import place_names as pn   # noqa: E402
import game as gamemod   # noqa: E402  (place_names put dm/games on the path)


def msbt(entries, utf8=True, big=False):
    """An MSBT: LBL1 (one group per label) and TXT2; a text is a list of str and ('tag', bytes) / ('end',) items"""
    e = '>' if big else '<'
    enc = (lambda s: s.encode('utf-8')) if utf8 else (lambda s: s.encode('utf-16-be' if big else 'utf-16-le'))
    unit = (lambda c: bytes([c])) if utf8 else (lambda c: struct.pack(e + 'H', c))
    texts = []
    for _, parts in entries:
        b = b''
        for p in parts:
            if isinstance(p, str): b += enc(p)
            elif p[0] == 'tag': b += unit(0x0e) + struct.pack(e + 'HHH', 201, 0, len(p[1])) + p[1]
            else: b += unit(0x0f) + struct.pack(e + 'HH', 201, 0)
        texts.append(b + unit(0))
    n = len(entries)
    lbl = struct.pack(e + 'I', n)
    names = b''
    off = 4 + 8 * n
    for i, (label, _) in enumerate(entries):
        lbl += struct.pack(e + 'II', 1, off + len(names))
        names += bytes([len(label)]) + label.encode() + struct.pack(e + 'I', i)
    lbl += names
    txt = struct.pack(e + 'I', n)
    at = 4 + 4 * n
    for t in texts: txt += struct.pack(e + 'I', at); at += len(t)
    txt += b''.join(texts)
    def section(magic, body): return magic + struct.pack(e + 'I', len(body)) + bytes(8) + body + bytes(-len(body) % 16)
    secs = section(b'LBL1', lbl) + section(b'TXT2', txt)
    head = b'MsgStdBn' + (b'\xfe\xff' if big else b'\xff\xfe') + bytes(2) + bytes([0 if utf8 else 1, 3]) + struct.pack(e + 'H', 2) + bytes(2) + struct.pack(e + 'I', 0x20 + len(secs)) + bytes(10)
    return head + secs


PLACES = [('KokiriForest', [('tag', b'\x01\x00\x06\x01'), 'Forêt Kokiri']), ('LakeHylia', ['Lac', ('end',), ' Hylia\t\n'])]


class Parse(unittest.TestCase):
    def test_utf8_tags_left_out(self):
        self.assertEqual(pn.msbt(msbt(PLACES), 'x'), {'KokiriForest': 'Forêt Kokiri', 'LakeHylia': 'Lac Hylia\t\n'})

    def test_utf16_both_orders(self):
        for big in (False, True):
            self.assertEqual(pn.msbt(msbt(PLACES, utf8=False, big=big), 'x')['KokiriForest'], 'Forêt Kokiri')

    def test_clean_makes_one_cell(self):
        self.assertEqual(pn.clean('Lac Hylia\t\n'), 'Lac Hylia')
        self.assertEqual(pn.clean(' A\x07 B '), 'A B')

    def test_damaged_refused(self):
        good = msbt(PLACES)
        for bad in (b'NotAMsbt' + good[8:], good[:0x30], good[:8] + b'\0\0' + good[10:]):
            with self.assertRaises(pn.Error): pn.msbt(bad, 'x')
        cut = bytearray(good)
        cut[0x0e] = 1   # one section: the labels name texts it does not have
        with self.assertRaises(pn.Error): pn.msbt(bytes(cut), 'x')


class Table(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        root = Path(self.tmp.name)
        self.romfs = root / 'romfs'
        (self.romfs / 'Mals').mkdir(parents=True)
        zstd = fz.ss.zstd_mod()
        def archive(name, places):
            (self.romfs / 'Mals' / name).write_bytes(zstd.compress(fz.sarc([(b'LocationMsg/Location.msbt', msbt(places)), (b'Other/A.msbt', msbt([('A', ['a'])]))])))
        archive('EUfr.Product.100.sarc.zs', [('KokiriForest', ['old'])])
        archive('EUfr.Product.110.sarc.zs', PLACES)
        archive('USen.Product.100.sarc.zs', [('KokiriForest', ['Kokiri Forest']), ('Market', ['Hyrule Castle Town'])])
        self.profile = root / 'profile' / '0400486024DEC000'
        self.profile.mkdir(parents=True)
        (self.profile / 'game.tsv').write_text('title_id\t0400486024DEC000\nmsg_archive\tMals/{code}.Product.*.sarc.zs\nplace_messages\tLocationMsg/Location.msbt\n'
                                               'msg_languages\ten-US=USen fr=EUfr fr-CA=USfr ja=JPja\n')
        self.g = gamemod.Game(self.profile)

    def tearDown(self): self.tmp.cleanup()

    def test_newest_archive_per_code(self):
        self.assertEqual(pn.archives(self.romfs, 'Mals/{code}.Product.*.sarc.zs'), {'EUfr': 'Mals/EUfr.Product.110.sarc.zs', 'USen': 'Mals/USen.Product.100.sarc.zs'})

    def test_columns_and_names(self):
        cols, names = pn.table(self.romfs, self.g)
        self.assertEqual(cols, ['en-US', 'fr'])   # fr-CA's and ja's archives are not there
        self.assertEqual(names, {'KokiriForest': {'en-US': 'Kokiri Forest', 'fr': 'Forêt Kokiri'}, 'LakeHylia': {'fr': 'Lac Hylia'}, 'Market': {'en-US': 'Hyrule Castle Town'}})
        out = Path(self.tmp.name) / 'places.tsv'
        pn.write(out, cols, names, 'test')
        rows = out.read_text(encoding='utf-8').splitlines()
        self.assertTrue(rows[0].startswith('#'))
        self.assertEqual(rows[1:], ['id\ten-US\tfr', 'KokiriForest\tKokiri Forest\tForêt Kokiri', 'LakeHylia\t\tLac Hylia', 'Market\tHyrule Castle Town\t'])

    def test_no_keys_or_no_archive_refused(self):
        bare = gamemod.Game(self.profile)
        bare.rows = [r for r in bare.rows if r[0] != 'place_messages']
        with self.assertRaises(pn.Error): pn.table(self.romfs, bare)
        none = gamemod.Game(self.profile)
        none.rows = [r if r[0] != 'msg_languages' else (r[0], ['ja=JPja']) for r in none.rows]
        with self.assertRaises(pn.Error): pn.table(self.romfs, none)


if __name__ == '__main__':
    unittest.main()
