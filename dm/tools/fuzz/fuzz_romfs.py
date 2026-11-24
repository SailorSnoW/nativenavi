#!/usr/bin/env python3
"""Fuzz dm/tools/romfs.py's Image. Clean rejection: ValueError. An accepted image must hold to what its checks promise: every entry reachable once,
find() returns each by its path, every file range inside the image. Usage: see fuzzlib.py"""
import os
from pathlib import Path
import sys
import tempfile

import fuzzlib
import romfs
import romfs_image

HARNESS = 'romfs'


def seeds():
    out = {}
    trees = {'flat': {'a': b'one', 'b': b'', 'c': b'three' * 9},
             'nested': {'Data': {'archive': b'\x03\x02\x23\x07' * 8, 'empty': {}, 'z': {'deep': {'file.bin': b'x'}}}, 'A': b'upper', 'a': b'lower', 'top': b'top'},
             'empty': {}}
    with tempfile.TemporaryDirectory() as t:
        for name, tree in trees.items():
            src = Path(t, name)
            src.mkdir()
            def make(d, node):
                for k, v in node.items():
                    if isinstance(v, dict):
                        (d / k).mkdir()
                        make(d / k, v)
                    else: (d / k).write_bytes(v)
            make(src, tree)
            romfs_image.pack(src, Path(t, name + '.img'))
            out[name] = Path(t, name + '.img').read_bytes()
    return out


_file = fuzzlib.InputFile('RomFS.img')


def check(data):
    p = _file.write(data)
    try: img = romfs.Image(p)
    except ValueError: return
    size, seen, todo = os.path.getsize(p), {0}, [(0, '')]
    while todo:
        k, path = todo.pop()
        a, b, _, _, t, _ = img.entry(k)
        if path and img.find(path) != k: raise fuzzlib.broken('romfs.py:Image', f'find({path!r}) does not return entry {k}')
        if t:
            if a + b > size: raise fuzzlib.broken('romfs.py:Image', f'entry {k}: file range {a:#x}+{b:#x} past the image end {size:#x}')
            continue
        for name, j in img.children(k):
            if j in seen: raise fuzzlib.broken('romfs.py:Image', f'entry {j} is reached twice')
            seen.add(j)
            todo.append((j, f'{path}/{name}' if path else name))
    if len(seen) != img.count: raise fuzzlib.broken('romfs.py:Image', f'{img.count - len(seen)} entries are not reachable from the root')


if __name__ == '__main__':
    sys.exit(fuzzlib.main(HARNESS, check, seeds))
