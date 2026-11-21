"""Packer/cache round trips and the index validator the host mounts with; synthetic files only."""
import importlib.util
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
from types import SimpleNamespace
import unittest
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location('romfs_image', REPO / 'dm/tools/romfs_image.py')
assert spec is not None and spec.loader is not None
image = importlib.util.module_from_spec(spec); spec.loader.exec_module(image)


class Images(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tools = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tools.cleanup)
        root = Path(cls.tools.name)
        source = root / 'validate.c'
        source.write_text('#include "romfs_image.h"\nint main(int argc, char **argv) {\n'
                          'if (argc != 2) return 2; romfs_img *m = romfs_image_open(argv[1]);\n'
                          'if (!m) return 1; romfs_image_close(m); return 0; }\n')
        cls.validator = root / 'validate'
        subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-I', str(REPO / 'dm/host'),
                        str(source), '-o', str(cls.validator)], check=True)

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory(); self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)
        self.src = self.root / 'RomFS'; self.src.mkdir()
        self.dst = self.root / 'romfs.img'

    def valid(self):
        return subprocess.run([str(self.validator), str(self.dst)]).returncode == 0

    def content(self, name):
        isdir, offset, size = image.read_index(self.dst)[name]
        self.assertFalse(isdir)
        with self.dst.open('rb') as f:
            f.seek(offset); return f.read(size)

    def test_empty_romfs_and_packed_files_use_the_host_validator(self):
        image.pack(self.src, self.dst)
        self.assertTrue(self.valid()); self.assertEqual(self.dst.stat().st_size, 56)
        (self.src / 'data/empty').mkdir(parents=True)
        (self.src / 'data/a').write_bytes(b'hello')
        (self.src / 'data/z').write_bytes(b'')
        image.pack(self.src, self.dst)
        self.assertTrue(self.valid()); self.assertEqual(self.content('data/a'), b'hello')
        self.assertEqual(self.content('data/z'), b'')
        self.assertTrue(image.read_index(self.dst)['data/empty'][0])

    def test_format_mismatches_name_zs_files_that_are_not_zstd(self):
        tex = self.src / 'Tex'; tex.mkdir()
        (tex / 'ok.bntx.zs').write_bytes(b'\x28\xb5\x2f\xfd' + b'frame')
        (tex / 'skip.bntx.zs').write_bytes(b'\x53\x2a\x4d\x18' + b'\0' * 8)   # a skippable frame (any of 0x184d2a50..5f)
        (tex / 'UPPER.ZS').write_bytes(b'\x28\xb5\x2f\xfd')
        (tex / 'zeros.bntx.zs').write_bytes(b'\0' * 12288 + b'\x28\xb5\x2f\xfd')   # as on a damaged NUP of Ocarina of Time
        (tex / 'short.zs').write_bytes(b'\x28\xb5')
        (tex / 'empty.zs').write_bytes(b'')   # no bytes, no promise broken: left out
        (tex / 'plain.bntx').write_bytes(b'\0' * 16)   # no format promised by the name
        image.pack(self.src, self.dst)
        seen = []
        bad = image.format_mismatches(self.dst, lambda done, total: seen.append((done, total)))
        self.assertEqual(sorted((p, size, head) for p, size, head in bad), [('Tex/short.zs', 2, b'\x28\xb5'), ('Tex/zeros.bntx.zs', 12292, b'\0\0\0\0')])
        self.assertEqual(seen, [(0, 5)])

    def test_reuse_rebuilds_changed_added_removed_and_renamed_files(self):
        (self.src / 'a').write_bytes(b'old')
        image.pack(self.src, self.dst, reuse=True)
        identity = image.file_identity(self.dst)
        image.pack(self.src, self.dst, reuse=True)
        self.assertEqual(image.file_identity(self.dst), identity)
        # Same length and a restored mtime: ctime still invalidates the cached image.
        times = (self.src / 'a').stat()
        (self.src / 'a').write_bytes(b'new')
        os.utime(self.src / 'a', ns=(times.st_atime_ns, times.st_mtime_ns))
        image.pack(self.src, self.dst, reuse=True)
        self.assertEqual(self.content('a'), b'new')
        (self.src / 'added').write_bytes(b'added')
        image.pack(self.src, self.dst, reuse=True); self.assertEqual(self.content('added'), b'added')
        (self.src / 'added').rename(self.src / 'renamed'); (self.src / 'a').unlink()
        image.pack(self.src, self.dst, reuse=True)
        self.assertEqual(set(image.read_index(self.dst)), {'renamed'})

    def test_changed_image_and_forced_repack_are_not_reused(self):
        image.pack(self.src, self.dst, reuse=True)
        self.dst.write_bytes(image.HEAD.pack(image.MAGIC, 32, 0, 0))
        image.pack(self.src, self.dst, reuse=True); self.assertTrue(self.valid())
        identity = image.file_identity(self.dst)
        image.pack(self.src, self.dst)
        self.assertNotEqual(image.file_identity(self.dst), identity)

    def test_reuse_survives_a_new_device_number(self):   # macOS numbers a volume at each mount: the Data volume went from 16777234 to 16777232 across a reboot, with the same inode, size and times
        (self.src / 'a').write_bytes(b'data')
        image.pack(self.src, self.dst, reuse=True)
        digest, identity = image.source_digest(self.src, image.entries(self.src)), image.file_identity(self.dst)
        real = os.stat
        def remounted(path, *args, **kw):
            s = real(path, *args, **kw)
            return SimpleNamespace(st_dev=s.st_dev + 2, st_ino=s.st_ino, st_size=s.st_size, st_mtime_ns=s.st_mtime_ns, st_ctime_ns=s.st_ctime_ns)
        with patch.object(image.os, 'stat', remounted):
            self.assertEqual(image.file_identity(self.dst), identity)
            self.assertEqual(image.source_digest(self.src, image.entries(self.src)), digest)
            self.assertTrue(image.reusable(str(self.dst), digest))

    def test_changed_source_during_pack_preserves_previous_image(self):
        (self.src / 'a').write_bytes(b'old'); image.pack(self.src, self.dst)
        original = self.dst.read_bytes()
        scan = image.entries
        calls = 0
        def changed(src):
            nonlocal calls
            calls += 1
            if calls == 2: (self.src / 'a').write_bytes(b'changed during packing')
            return scan(src)
        with patch.object(image, 'entries', side_effect=changed), self.assertRaisesRegex(RuntimeError, 'changed while packing'):
            image.pack(self.src, self.dst)
        self.assertEqual(self.dst.read_bytes(), original)
        self.assertFalse(Path(str(self.dst) + '.part').exists())

    def test_case_collision_guest_names(self):
        if not shutil.which('xattr'): self.skipTest('macOS xattr tool required')
        (self.src / 'a').write_bytes(b'lower')
        alias = self.src / '.dm-case-1'; alias.write_bytes(b'upper')
        subprocess.run(['xattr', '-w', image.GUEST_NAME, 'A', str(alias)], check=True)
        image.pack(self.src, self.dst)
        self.assertTrue(self.valid()); self.assertEqual(self.content('A'), b'upper')
        self.assertEqual(self.content('a'), b'lower')

    def test_dump_tool_listings_are_left_out(self):   # an FTP dump's .listing in every folder (Nintendo Switch Sports Resort's dump): not a game file
        (self.src / 'Parameter/P').mkdir(parents=True)
        (self.src / 'Parameter/P/Body00.bgyml').write_bytes(b'b')
        for d in ('', 'Parameter', 'Parameter/P'): (self.src / d / '.listing').write_bytes(b'-r--r--r-- 1 ftp ftp')
        (self.src / '.other').write_bytes(b'o')   # another dot file is a game file
        image.pack(self.src, self.dst)
        self.assertTrue(self.valid())
        self.assertEqual(sorted(image.read_index(self.dst)), ['.other', 'Parameter', 'Parameter/P', 'Parameter/P/Body00.bgyml'])

    def test_size_consistent_invalid_index_is_rejected(self):
        for raw in (image.HEAD.pack(image.MAGIC, 32, 0, 0),
                    image.HEAD.pack(image.MAGIC, 32, 24, 1) + image.ENT.pack(1, 0, 0, 0, 1, 0)):
            with self.subTest(raw=raw):
                self.dst.write_bytes(raw); self.assertFalse(self.valid())
        (self.src / 'a').write_bytes(b'content'); (self.src / 'b').write_bytes(b'other')
        image.pack(self.src, self.dst); complete = self.dst.read_bytes()
        _, index, _, _ = image.HEAD.unpack_from(complete)
        mutations = {
            'truncated': complete[:-1], 'trailing bytes': complete + b'x',
            'root cycle': (index, '<Q', 0), 'missing child': (index + 8, '<Q', 1),
            'bad name offset': (index + 24 + 16, '<I', 0xffffffff),
            'file in header': (index + 24, '<Q', 0), 'file into index': (index + 24 + 8, '<Q', index),
            'duplicate children': (index + 48 + 16, '<I', 0),
            'unsorted children': complete[:-2] + b'ba', 'path separator': complete[:-2] + b'a/',
        }
        for name, change in mutations.items():
            with self.subTest(name=name):
                raw = bytearray(complete)
                if isinstance(change, tuple):
                    off, fmt, value = change; struct.pack_into(fmt, raw, off, value)
                else: raw = change
                self.dst.write_bytes(raw); self.assertFalse(self.valid())


if __name__ == '__main__': unittest.main()
