#!/usr/bin/env python3
# A game in a NUP container (https://github.com/a55uka/nup, SPEC.md v1) → the extraction layout build_app.py builds from (it runs this step itself).
# A container may hold no base game; the selected update is then taken as the whole game (NO_BASE; docs/reference/packing.md, "A NUP with complete standalone updates").
# Writes under the build folder: extract/v<version>/ExeFS/, RomFS.img (straight from the container), control.nacp, icon.jpg; aoc/<index>.img per DLC (DM_AOC_DIR).
# An update's RomFS is packed on its own, never laid over the base: a NUP's update holds the patched RomFS.
# Usage: pack_nup.py <game.nup> [--version N] [--work-dir DIR]   (exit 1 on error)
import argparse, hashlib, mmap, os, shutil, struct, sys
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / 'dm/tools'))
import romfs_image
try: from compression import zstd
except ImportError:
    try: from backports import zstd   # pyright: ignore[reportMissingImports]  # Python before 3.14
    except ImportError: zstd = None


class Error(Exception): pass


SUPER = struct.Struct('<4sHHQQ32sQQII16s32s')
TOC = struct.Struct('<4sIQQQQQ')
PKG = struct.Struct('<QIIIIBBHIQI20x')
DIR = struct.Struct('<IIIIQ')
FILE = struct.Struct('<IIQQQ32sBBH12x')
TLV = struct.Struct('<HHI')
NO_DIR = 0xFFFFFFFF
BASE, UPDATE, ADDON = 0, 1, 2
KINDS = ('base', 'update', 'addon', 'addon_update', 'system')
STANDALONE = 1   # package flags bit 0
T_NACP, T_ICON, T_LICONS = 2, 3, 10
ARTIFACTS = romfs_image.ARTIFACTS   # dump-tool files a producer may have kept (SPEC §6.3)
NO_BASE = 'no base game in this NUP: its update is taken as the whole game (if the game then stops on missing files, the update holds only what changed: pack a NUP with the base game)'


class Package:
    def __init__(self, nup, index, rec):
        self.nup, self.index = nup, index
        self.title_id, self.root, self.exefs, self.romfs, self.version, self.kind, self.flags, r1, r2, self.meta_off, self.meta_size = rec
        if self.kind >= len(KINDS): raise Error(f'{nup.path}: package {index} of unknown kind {self.kind}')
        if r1 or r2 or self.flags & ~STANDALONE: raise Error(f'{nup.path}: package {index} has reserved fields or flags set')
        self.name = f'{nup.path.name} ({KINDS[self.kind]}' + (f' {self.version}' if self.kind == UPDATE else f' {self.title_id:016X}' if self.kind == ADDON else '') + ')'

    def meta(self):
        out, b = {}, self.nup.mm[self.meta_off:self.meta_off + self.meta_size] if self.meta_size else b''
        p = 0
        while p + TLV.size <= len(b):
            t, _, n = TLV.unpack_from(b, p); p += TLV.size
            if p + n > len(b): raise Error(f'{self.nup.path}: package {self.index}: meta record {t:#x} runs past its block')
            out.setdefault(t, bytes(b[p:p + n])); p += n
        return out


class Nup:
    """Validated on open (SPEC §6.1); file content is checked against its SHA-256 as it is read"""
    def __init__(self, path):
        self.path = Path(path)
        self.application, self.aoc_base = None, None
        with open(self.path, 'rb') as f:   # the map keeps its own descriptor
            try: self.mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
            except ValueError: raise Error(f'{self.path}: empty file') from None
        try: self._read()
        except BaseException:
            self.close(); raise

    def _read(self):
        if len(self.mm) < SUPER.size: raise Error(f'{self.path}: not a NUP (too short)')
        magic, hsize, _, toc_off, toc_size, toc_sha, self.blob, fsize, npkg, flags, self.uuid, _ = SUPER.unpack_from(self.mm)
        if magic[:3] != b'NUP': raise Error(f'{self.path}: not a NUP')
        if magic[3] != 1: raise Error(f'{self.path}: NUP format version {magic[3]}; this reads version 1')
        if hsize < SUPER.size or toc_off != hsize or flags: raise Error(f'{self.path}: unsupported NUP header')
        if fsize != len(self.mm): raise Error(f'{self.path}: {len(self.mm):,} bytes, its header says {fsize:,} (copied or downloaded partly?)')
        if self.blob < toc_off + toc_size or self.blob % 16 or self.blob > fsize: raise Error(f'{self.path}: TOC and data regions overlap')
        toc = self.mm[toc_off:toc_off + toc_size]
        self.toc_sha = toc_sha
        if hashlib.sha256(toc).digest() != toc_sha: raise Error(f'{self.path}: table of contents does not match its SHA-256 (damaged file)')
        tmagic, tpkg, self.ndirs, nfiles, pool_off, pool_size, _ = TOC.unpack_from(toc)
        need = TOC.size + tpkg * PKG.size + self.ndirs * DIR.size + nfiles * FILE.size
        if tmagic != b'TOC\x01' or tpkg != npkg or not npkg or len(toc) < need or pool_off < need or pool_off + pool_size > len(toc) or (pool_off - need) % 4:
            raise Error(f'{self.path}: malformed table of contents')
        pool = toc[pool_off:pool_off + pool_size]
        def name(off, n):
            if off + n > len(pool): raise Error(f'{self.path}: name out of bounds')
            return pool[off:off + n]
        p = TOC.size
        self.packages = [Package(self, i, PKG.unpack_from(toc, p + i * PKG.size)) for i in range(npkg)]; p += npkg * PKG.size
        self.dirs = []
        for _ in range(self.ndirs):
            o, n, first, count, _ = DIR.unpack_from(toc, p); p += DIR.size
            self.dirs.append((name(o, n), first, count))
        self.files = []
        for _ in range(nfiles):
            o, n, size, eoff, esz, sha, codec, cflags, _ = FILE.unpack_from(toc, p); p += FILE.size
            if codec not in (0, 1) or cflags: raise Error(f'{self.path}: file {name(o, n)!r} uses codec {codec}; this reads raw and zstd')
            if size and (eoff < self.blob or eoff + esz > fsize or eoff % 16 or not esz or codec == 0 and esz != size): raise Error(f'{self.path}: file {name(o, n)!r} out of bounds')
            if not size and (eoff or esz or codec): raise Error(f'{self.path}: empty file {name(o, n)!r} with data')
            self.files.append((name(o, n), size, sha, eoff, esz, codec))
        self.slots = struct.unpack_from(f'<{(pool_off - p) // 4}I', toc, p)
        for n, first, count in self.dirs:
            if first + count > len(self.slots): raise Error(f'{self.path}: folder {n!r} out of bounds')
        for s in self.slots:
            if s >= self.ndirs + nfiles: raise Error(f'{self.path}: child {s} out of bounds')
        self._acyclic()
        for k in self.packages:
            if k.root >= self.ndirs or any(d != NO_DIR and d >= self.ndirs for d in (k.exefs, k.romfs)) or k.meta_size and k.meta_off + k.meta_size > fsize:
                raise Error(f'{self.path}: package {k.index} out of bounds')

    def _acyclic(self):
        """The bounds checks pass a folder that contains itself, and every walk of the tree would loop for ever"""
        kids = [[s for s in self.slots[first:first + count] if s < self.ndirs] for _, first, count in self.dirs]
        state = bytearray(self.ndirs)   # 0 not seen, 1 on the current path, 2 done
        for top in range(self.ndirs):
            if state[top]: continue
            state[top], stack = 1, [(top, 0)]
            while stack:   # a container's depth is not bounded by Python's recursion limit
                d, i = stack[-1]
                if i == len(kids[d]): state[d] = 2; stack.pop(); continue
                stack[-1] = (d, i + 1); c = kids[d][i]
                if state[c] == 1: raise Error(f'{self.path}: folder {self.dirs[c][0]!r} (index {c}) contains itself (damaged file)')
                if not state[c]: state[c] = 1; stack.append((c, 0))

    def close(self):
        self.mm.close()

    def __enter__(self): return self
    def __exit__(self, *a): self.close()

    def children(self, d):
        _, first, count = self.dirs[d]
        out = []
        for s in self.slots[first:first + count]:
            if s < self.ndirs: out.append((self.dirs[s][0], True, s))
            elif self.files[s - self.ndirs][0] not in ARTIFACTS: out.append((self.files[s - self.ndirs][0], False, s - self.ndirs))
        return sorted(out)

    def child(self, d, name):
        return next((i for n, isdir, i in self.children(d) if n == name and not isdir), None)

    def tree_bytes(self, d):
        return sum(self.files[i][1] if not isdir else self.tree_bytes(i) for _, isdir, i in self.children(d))

    def copy(self, i, out):
        name, size, sha, eoff, esz, codec = self.files[i]
        h, n = hashlib.sha256(), 0
        def put(b):
            nonlocal n
            h.update(b); out.write(b); n += len(b)
        with memoryview(self.mm) as view:
            if codec == 0:
                for p in range(eoff, eoff + esz, 8 << 20):
                    with view[p:min(p + (8 << 20), eoff + esz)] as chunk:
                        put(chunk)
            elif size:
                if zstd is None: raise Error('reading a NUP needs the zstd module: compression.zstd (Python 3.14+) or pip install backports.zstd')
                d = zstd.ZstdDecompressor()
                try:
                    for p in range(eoff, eoff + esz, 4 << 20):
                        with view[p:min(p + (4 << 20), eoff + esz)] as chunk:
                            data = d.decompress(chunk)
                        put(data)
                        if n > size: break
                except zstd.ZstdError as e: raise Error(f'{self.path}: {name.decode(errors="replace")}: {e}') from e
                if not d.eof: n = -1
        if n != size or h.digest() != sha: raise Error(f'{self.path}: {name.decode(errors="replace")} does not match its size or SHA-256 (damaged file)')
        return n

    def read(self, i):
        class Buf(bytearray):
            write = bytearray.extend
        b = Buf(); self.copy(i, b); return bytes(b)


def program_id(npdm, where):   # the META header is 0x80 bytes; the ACI0 it names follows it, 0x18 bytes at least, within the file
    if len(npdm) < 0x80 or npdm[:4] != b'META': raise Error(f'{where}: not an NPDM')
    off, size = struct.unpack_from('<II', npdm, 0x70)
    if off < 0x80 or size < 0x18 or off + size > len(npdm) or npdm[off:off + 4] != b'ACI0': raise Error(f'{where}: no ACI0 section')
    return struct.unpack_from('<Q', npdm, off + 0x10)[0]


def is_nup(path):
    try:
        with open(path, 'rb') as f: return f.read(3) == b'NUP'
    except OSError: return False


NACP_AOC_BASE, NACP_SAVE_OWNER = 0x3070, 0x3078   # ApplicationControlProperty offsets


def nacp_ids(k):
    b = k.meta().get(T_NACP, b'')
    if len(b) < NACP_SAVE_OWNER + 8: return None, None
    aoc, owner = struct.unpack_from('<QQ', b, NACP_AOC_BASE)
    return owner or None, aoc or None


def scan(n):
    """Packages of several programs are refused: the host runs one program and has no nn::fs::MountRom by program index."""
    bases = [k for k in n.packages if k.kind == BASE]
    updates = [k for k in n.packages if k.kind == UPDATE]
    if not bases and (not updates or any(not k.flags & STANDALONE for k in updates)):
        raise Error(f'{n.path}: no base game: pack the base game and its update into one NUP, or use updates marked standalone_update with complete exefs/ and romfs/')
    real = [k for k in bases if n.tree_bytes(k.root)]   # a base record with no files is no base game
    progs, loose = {}, []
    for k in real + updates:
        i = n.child(k.exefs, b'main.npdm') if k.exefs != NO_DIR else None
        if i is None:
            if k.kind == BASE: loose.append(k)
            continue
        progs.setdefault(program_id(n.read(i), f'{k.name} exefs/main.npdm'), []).append(k)
    if not progs: raise Error(f'{n.path}: no exefs/main.npdm in the base or updates')
    if len(progs) > 1: raise Error(f'{n.path}: packages of several programs (' + ', '.join(f'{pid:016X}' for pid in sorted(progs)) + '): one program with its updates is packed')
    (app, ks), = progs.items()
    if len(loose) > 1: raise Error(f'{n.path}: {len(loose)} base packages without exefs/main.npdm')
    if loose: ks.insert(0, loose[0])
    bases = [k for k in ks if k.kind == BASE]
    if len(bases) > 1: raise Error(f'{n.path}: {len(bases)} base packages of program {app:016X}')
    base, vers = bases[0] if bases else None, {}
    for k in ks:
        if k.kind != UPDATE: continue
        if k.version in vers: raise Error(f'{n.path}: two updates of version {k.version}')
        if not k.version: raise Error(f'{n.path}: an update of version 0')
        vers[k.version] = k
    newest = [vers[v] for v in sorted(vers, reverse=True)] + ([base] if base else [])
    n.application = next((a for k in ks if (a := nacp_ids(k)[0]) is not None), app & ~0xFFF)
    n.aoc_base = next((a for k in newest if (a := nacp_ids(k)[1]) is not None), n.application + 0x1000)
    dlc = {}
    for k in n.packages:
        if k.kind != ADDON: continue
        if not 0 < k.title_id - n.aoc_base <= 2000: raise Error(f'{n.path}: {k.title_id:016X} is not a DLC of {n.application:016X} (its add-on content IDs start at {n.aoc_base:016X})')
        if k.romfs == NO_DIR: raise Error(f'{n.path}: DLC {k.title_id:016X} has no romfs/')
        if k.title_id in dlc: raise Error(f'{n.path}: DLC {k.title_id:016X} more than once')
        dlc[k.title_id] = k
    return app, base, vers, dlc


def romfs_bytes(n, k, base): return n.tree_bytes(romfs_of(k, base))


def exefs(n, k, dst):
    files = []
    for name, isdir, i in n.children(k.exefs):
        if isdir: continue
        # NUP names are labels (§6.1), not safe host paths
        filename = os.fsdecode(name)
        if filename in ('', '.', '..') or '\0' in filename or Path(filename).name != filename:
            raise Error(f'{n.path}: invalid ExeFS name {name!r}')
        if not name.startswith(b'.'): files.append((name, i))
    def same():
        if not dst.is_dir() or {os.fsencode(p.name) for p in dst.iterdir()} != {name for name, _ in files}: return False
        def sha(f):
            with open(f, 'rb') as h: return hashlib.file_digest(h, 'sha256').digest()
        return all(sha(dst / os.fsdecode(name)) == n.files[i][2] for name, i in files)
    if same(): return
    tmp = dst.with_name(dst.name + '.partial')
    shutil.rmtree(tmp, ignore_errors=True); tmp.mkdir(parents=True)
    for name, i in files:
        with open(tmp / os.fsdecode(name), 'wb') as f: n.copy(i, f)
    shutil.rmtree(dst, ignore_errors=True); tmp.rename(dst)


def entries(n, d):
    ents, i = [[b'', d, True, 0, 0]], 0
    while i < len(ents):
        if ents[i][2]:
            kids = n.children(ents[i][1])
            ents[i][3], ents[i][4] = len(ents), len(kids)
            ents.extend([name, j, isdir, 0, 0] for name, isdir, j in kids)
        i += 1
    return ents


def verified(n, ents, dst):
    """Reads the image, not the container's file data; None unless it is byte for byte what write() makes of ents now"""
    ents = [e.copy() for e in ents]
    off = romfs_image.HEAD.size
    for e in ents:
        if not e[2]: e[3], e[4] = off, n.files[e[1]][1]; off += e[4]
    head, idx = romfs_image.layout(ents, off)
    try:
        before = romfs_image.file_identity(dst)
        if before[1] != off + len(idx): return None   # [inode, size, mtime, ctime]
        with open(dst, 'rb') as f:
            if f.read(len(head)) != head: return None
            f.seek(off)
            if f.read(len(idx)) != idx: return None
            f.seek(len(head))
            for _, i, isdir, _, size in ents:
                if isdir: continue
                h, left = hashlib.sha256(), size
                while left:
                    chunk = f.read(min(left, 8 << 20))
                    if not chunk: return None
                    h.update(chunk); left -= len(chunk)
                if h.digest() != n.files[i][2]: return None
        return before if romfs_image.file_identity(dst) == before else None
    except OSError:
        return None


def pack(n, d, img, say=print, step=lambda s: None):
    """Returns 'kept', 'verified' (another packer's stamp, but the bytes check against the container's hashes) or 'packed'"""
    img.parent.mkdir(parents=True, exist_ok=True)
    ents, dst = entries(n, d), os.fspath(img)
    digest = hashlib.sha256(Path(romfs_image.__file__).read_bytes() + Path(__file__).read_bytes() + n.toc_sha + struct.pack('<I', d)).hexdigest()   # the TOC hash covers every name and file hash
    if romfs_image.reusable(dst, digest): return 'kept'
    if img.is_file():   # a packer update changes the digest, not the image: check its bytes before unpacking again
        step('checking the image'); say(f'checking {img} against {n.path.name}')
        if identity := verified(n, ents, dst):
            romfs_image.stamp(dst, digest, identity)
            say('  kept: the image matches the container')
            return 'verified'
    step('unpacking'); say(f'packing {n.path.name} folder {d} → {img}')
    part = dst + '.part'
    try: romfs_image.publish(romfs_image.write(ents, dst, n.copy), dst, digest)
    except BaseException:
        Path(part).unlink(missing_ok=True); raise
    say(f'  {len(ents):,} entries, {img.stat().st_size / 2**30:.2f} GB')
    return 'packed'


def localized(payload):   # TLV 0x000A
    out, p = {}, 0
    while p < len(payload):
        ln = payload[p]; tag = payload[p + 1:p + 1 + ln].decode(errors='replace').lower(); p += 1 + ln
        if p + 4 > len(payload): break
        dn = struct.unpack_from('<I', payload, p)[0]; p += 4
        out.setdefault(tag, payload[p:p + dn]); p += dn
    return out


def pick(tags, code):
    code = code.lower()
    return tags.get(code) or tags.get(code.split('-')[0])


def app_package(n):   # SPEC §5.1: packers attach metadata to a base record with no files
    return next((k for k in n.packages if k.kind == BASE), n.packages[0])


def control(k, out, fallback=None):
    meta = fallback.meta() if fallback is not None else {}
    meta.update(k.meta())
    nacp_bytes = meta.get(T_NACP)
    icons = localized(meta.get(T_LICONS, b''))
    icon = meta.get(T_ICON) or pick(icons, 'en-US') or next(iter(icons.values()), None)
    for name, data in (('control.nacp', nacp_bytes), ('icon.jpg', icon)):
        if data: (out / name).write_bytes(data)
        else: (out / name).unlink(missing_ok=True)


def romfs_of(k, base):
    d = k.romfs if k.romfs != NO_DIR else base.romfs if base is not None else NO_DIR
    if d == NO_DIR: raise Error(f'{k.name}: no romfs/ in this package or the base game')
    return d


def pack_version(n, k, base, app, out, say=print, step=lambda s: None):
    if k.exefs == NO_DIR or n.child(k.exefs, b'main') is None or n.child(k.exefs, b'main.npdm') is None: raise Error(f'{k.name}: no exefs/main and exefs/main.npdm')
    if (pid := program_id(n.read(n.child(k.exefs, b'main.npdm')), f'{k.name} exefs/main.npdm')) != app: raise Error(f'{k.name}: program ID {pid:016X}, the base is {app:016X}')
    d = romfs_of(k, base)
    out.mkdir(parents=True, exist_ok=True)
    exefs(n, k, out / 'ExeFS')
    how = pack(n, d, out / 'RomFS.img', say, step)
    control(k, out, app_package(n) if base is None else None)
    return how


def pack_dlc(n, app, dlc, aoc, say=print):
    first = n.aoc_base if n.aoc_base is not None else app + 0x1000
    want = {f'{tid - first}.img': k for tid, k in dlc.items()}
    if aoc.is_dir():
        for f in aoc.iterdir():
            if f.name.removesuffix('.inputs.json') not in want: f.unlink()
    for name, k in want.items(): pack(n, k.romfs, aoc / name, say)


def main():
    ap = argparse.ArgumentParser(description='Pack a game in a NUP container into the extraction layout of build_app.py')
    ap.add_argument('nup', type=Path, help='the .nup file')
    ap.add_argument('--version', type=int, help='the update version to pack (default: the newest in the container; 0 = the base game)')
    ap.add_argument('--work-dir', type=Path, help='the build folder (default: <folder of the .nup>/<title ID>_build)')
    a = ap.parse_args()
    path = a.nup.resolve()
    try:
        with Nup(path) as n:
            app, base, vers, dlc = scan(n)
            ver = a.version if a.version is not None else max(vers, default=0)
            if ver and ver not in vers: raise Error(f'no update {ver} in {path.name} (have: {", ".join(map(str, sorted(vers))) or "none"})')
            k = vers[ver] if ver else base
            if k is None: raise Error(f'{path.name}: no base game (version 0); choose a standalone update version')
            if base is None: print(NO_BASE, flush=True)
            work = (a.work_dir or path.parent / f'{app:016X}_build').resolve()
            out = work / 'extract' / f'v{ver}'
            print(f'{app:016X} version {ver}: {k.name}', flush=True)
            say = lambda s: print(s, flush=True)
            pack_version(n, k, base, app, out, say)
            pack_dlc(n, app, dlc, work / 'aoc', say)
    except (Error, OSError) as e: sys.exit(f'[RED] {e}')
    print(f'done: {out}' + (f', {len(dlc)} DLC in {work / "aoc"}' if dlc else ''))


if __name__ == '__main__':
    main()
