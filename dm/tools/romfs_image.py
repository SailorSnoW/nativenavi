#!/usr/bin/env python3
# Packs an extracted RomFS folder into one image the host reads in place (hle_fs.c, "RomFS image"; DM_ROMFS may name it).
# Layout (little-endian): header 32 bytes (magic "DMROMFS1", u64 index offset, u64 index size, u64 entry count), the files' data, then the index:
#   24-byte entries (u64 a, u64 b, u32 name offset, u16 name length, u8 type 0 dir / 1 file, u8 flags = 0) and the name pool. A dir: a = first child, b = child count;
#   a file: a = data offset, b = size. Entry 0 is the root; children are consecutive, sorted by name bytes, numbered breadth-first.
# The index comes last: a copy cut off partway has none. Names are guest names (hle_fs.c guest_name).
# Usage: romfs_image.py [--reuse] <RomFS folder> <out image>   (written to <out>.part, renamed at the end)
import argparse, hashlib, json, os, struct, subprocess
from pathlib import Path

MAGIC = b'DMROMFS1'
HEAD = struct.Struct('<8sQQQ')
ENT = struct.Struct('<QQIHBB')
GUEST_NAME = 'org.d2rmobile.guest-name'
# Dump-tool files in every folder (an FTP dump's .listing, NUP spec §6.3): packed, a game listing a folder would see one more entry
ARTIFACTS = {b'.listing'}


def guest(parent, name):
    if not name.startswith('.dm-case-'): return name
    r = subprocess.run(['xattr', '-p', GUEST_NAME, os.path.join(parent, name)], capture_output=True, text=True)
    return r.stdout.rstrip('\n') if r.returncode == 0 else name


def children(path):
    out = []
    with os.scandir(path) as it:
        for e in it:
            name, isdir = guest(path, e.name).encode(), e.is_dir(follow_symlinks=True)
            if isdir or name not in ARTIFACTS: out.append((name, e.path, isdir))
    out.sort()
    return out


def entries(src):
    ents = [[b'', src, True, 0, 0]]   # name, host path, is dir, a, b
    i = 0
    while i < len(ents):
        name, path, isdir, _, _ = ents[i]
        if isdir:
            kids = children(path)
            ents[i][3], ents[i][4] = len(ents), len(kids)
            ents.extend([n, p, d, 0, 0] for n, p, d in kids)
        i += 1
    return ents


def file_identity(path):   # not st_dev: macOS numbers volumes at mount
    s = os.stat(path)
    return [s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns]


def source_digest(src, ents):
    h = hashlib.sha256(Path(__file__).read_bytes())
    h.update(os.fsencode(os.path.realpath(src)) + b'\0')
    for name, path, isdir, _, _ in ents:
        h.update(struct.pack('<I?', len(name), isdir))
        h.update(name)
        h.update(json.dumps(file_identity(path)).encode() + b'\0')
    return h.hexdigest()


def reusable(dst, digest):
    try: return json.loads(Path(dst + '.inputs.json').read_text()) == {'source': digest, 'image': file_identity(dst)}
    except (OSError, ValueError): return False


def layout(ents, end):
    """write() and the check of an existing image both use these bytes, so the two cannot disagree on the format."""
    pool, idx = bytearray(), bytearray()
    for name, _, isdir, a, b in ents:
        idx += ENT.pack(a, b, len(pool), len(name), 0 if isdir else 1, 0)
        pool += name
    idx += pool
    return HEAD.pack(MAGIC, end, len(idx), len(ents)), bytes(idx)


def write(ents, dst, copy):
    """copy(source, out) writes one file's bytes and returns how many; fills in each file's a and b"""
    part = dst + '.part'
    with open(part, 'wb') as out:
        out.write(b'\0' * HEAD.size)
        off = HEAD.size
        for e in ents:
            if e[2]: continue
            e[3], e[4] = off, copy(e[1], out)
            off += e[4]
        head, idx = layout(ents, off)
        out.write(idx)
        out.seek(0)
        out.write(head)
    return part


def stamp(dst, digest, image):
    Path(dst + '.inputs.json.part').write_text(json.dumps({'source': digest, 'image': image}))
    os.replace(dst + '.inputs.json.part', dst + '.inputs.json')


def publish(part, dst, digest):
    os.replace(part, dst)
    stamp(dst, digest, file_identity(dst))


def copy_file(path, out):
    n = 0
    with open(path, 'rb') as f:
        while b := f.read(8 << 20):
            out.write(b)
            n += len(b)
    return n


def pack(src, dst, reuse=False):
    src, dst = os.fspath(src), os.fspath(dst)
    ents = entries(src)
    digest = source_digest(src, ents)
    if reuse and reusable(dst, digest): return len(ents)
    part = write(ents, dst, copy_file)
    # a changing source must publish no mixed image or stale stamp
    if source_digest(src, entries(src)) != digest:
        os.unlink(part)
        raise RuntimeError('RomFS changed while packing; run the packer again when its files are stable')
    publish(part, dst, digest)
    return len(ents)


def read_index(img):
    with open(img, 'rb') as f:
        magic, ioff, isz, n = HEAD.unpack(f.read(HEAD.size))
        assert magic == MAGIC
        f.seek(ioff)
        raw = f.read(isz)
    pool = raw[n * ENT.size:]
    e = [ENT.unpack_from(raw, k * ENT.size) for k in range(n)]
    out, todo = {}, [(0, '')]
    while todo:
        k, path = todo.pop()
        a, b, no, nl, t, _ = e[k]
        if t == 0:
            if path: out[path] = (True, 0, 0)
            todo.extend((c, (path + '/' if path else '') + pool[e[c][2]:e[c][2] + e[c][3]].decode()) for c in range(a, a + b))
        else: out[path] = (False, a, b)
    return out


# A NUP carries no NCA hashes: a file starting with none of its suffix's heads is damaged (zstd: a frame or a skippable frame).
FORMAT_HEADS = {'.zs': ((b'\x28\xb5\x2f\xfd', b'\xff\xff\xff\xff'), (b'\x50\x2a\x4d\x18', b'\xf0\xff\xff\xff'))}   # suffix: ((bytes, mask), ...)


def format_mismatches(img, progress=None):
    files = sorted((off, size, p) for p, (d, off, size) in read_index(img).items() if not d and size and os.path.splitext(p)[1].lower() in FORMAT_HEADS)
    bad = []
    with open(img, 'rb') as f:
        for k, (off, size, p) in enumerate(files):   # one pass forward over the disk
            if progress and not k % 4096: progress(k, len(files))
            f.seek(off)
            head = f.read(min(size, 4))
            if not any(len(head) == 4 and all(h & m == w & m for h, w, m in zip(head, want, mask)) for want, mask in FORMAT_HEADS[os.path.splitext(p)[1].lower()]):
                bad.append((p, size, head))
    return bad


if __name__ == '__main__':
    ap = argparse.ArgumentParser(description='Pack a RomFS folder into an indexed image')
    ap.add_argument('--reuse', action='store_true', help='keep the image only while its source tree, packer and image are unchanged')
    ap.add_argument('source')
    ap.add_argument('image')
    args = ap.parse_args()
    n = pack(args.source, args.image, reuse=args.reuse)
    print(f'{args.image}: {n} entries, {os.path.getsize(args.image)} bytes')
