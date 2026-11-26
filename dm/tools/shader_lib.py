#!/usr/bin/env python3
# A game's NVN shader libraries as one file, b-<chain>.img (RomFS image format), read in place by the host (msl_image.h) and romfs.py.
# Each file comes from the folder b-<chain>/ first: with a stamp it is a finished full build (pack replaces the older image); without one beside an
# image, a partial or stopped build (pack keeps the image's other files); alone, used as it is. A failed pack is one [RED] line.
# Usage: shader_lib.py pack <lib>                      the folder into the image
#        shader_lib.py path <lib>                      what to read: the folder or the image
#        shader_lib.py has <lib>                       exit status 0 when it holds a shader's library (<key>_b.metallib)
#        shader_lib.py cat <lib> <name>                one file's bytes on standard output
#        shader_lib.py subset <lib> <out.img>          without .metal sources and build reports, as the Mac app carries it
# <lib> is the path b-<chain>, with or without .img.
import argparse, builtins, collections, concurrent.futures, contextlib, os, shutil, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romfs

ALIGN = 16   # Metal is handed a pointer into the mapped file
STAMP = 'inputs.tsv'
APP_SUFFIXES = ('.metallib', '.json', '.tsv')
APP_LEAVE_OUT = ('_b_build.json',)
READERS = 8


def folder_of(lib):
    s = os.fspath(lib)
    return s[:-4] if s.endswith('.img') else s


def image_of(lib): return folder_of(lib) + '.img'
def name_of(lib): return os.path.basename(os.path.normpath(folder_of(lib)))


def view(lib):
    """The state table at the top of this file. A missing path gives the folder's, whose glob is empty and whose reads fail"""
    folder, image = folder_of(lib), image_of(lib)
    if os.path.isfile(image) and not os.path.isfile(os.path.join(folder, STAMP)): return romfs.Path(image)
    return romfs.Path(folder)


def exists(lib): return os.path.isdir(folder_of(lib)) or os.path.isfile(image_of(lib))


def read_stamp(lib):
    try: return dict(l.split('\t', 1) for l in (view(lib) / STAMP).read_text().splitlines() if '\t' in l and not l.startswith('#'))
    except OSError: return {}


def table_head(lib):
    return f'# chain {name_of(lib)} inputs {read_stamp(lib).get("inputs", "none")}'


def _names(folder):
    """Dot files (AppleDouble ._x, .DS_Store) and a stopped dmsh_build.py's <name>.tmp-<pid>-<thread> are detritus"""
    out = {}
    with os.scandir(folder) as it:
        for e in it:
            if e.name.startswith('.') or '.tmp' in e.name: continue
            if e.is_dir(follow_symlinks=False): raise ValueError(f'{e.path} is a folder: a shader library holds only files')
            st = e.stat()
            out[e.name] = (st.st_size, st.st_mtime_ns)
    return out


def write(files, dst, on_progress=None, share=None):
    """files: {name: path, bytes, or a function called only when its file is written, so no library is held in memory whole}.
    share: {name: key}; the names of one key take the first one's range, their own sources neither called nor read (the readers allow it: romfs.py, romfs_image.h).
    Written by way of dst.part, removed on failure so a full drive gets its space back: an image is whole or absent.
    on_progress(files written, files, bytes written): at the start and after each file"""
    part = dst + '.part'
    try:
        _write_part(files, part, on_progress, share)
        os.replace(part, dst)
    except BaseException:
        try: os.unlink(part)
        except OSError: pass
        raise


def _write_part(files, part, on_progress=None, share=None):
    names = sorted((n.encode() for n in files))
    share = share or {}
    first, alias = set(), {}
    for name in names:
        key = share.get(name.decode())
        if key is None: continue
        if key in first: alias[name] = key
        else: first.add(key)
    ents, off, placed = [], romfs.HEAD.size, {}
    if on_progress: on_progress(0, len(names), 0)
    # called one after the other, the sources held a library's write to the cache file's read speed
    readers = concurrent.futures.ThreadPoolExecutor(READERS) if any(name not in alias and callable(files[name.decode()]) for name in names) else None
    ahead, todo = collections.deque(), iter(names)
    def sources():
        while True:
            while len(ahead) < (4 * READERS if readers else 1) and (name := next(todo, None)) is not None:
                if name in alias:
                    ahead.append((name, None))
                    continue
                source = files[name.decode()]
                ahead.append((name, readers.submit(source) if readers and callable(source) else source))
            if not ahead: return
            name, source = ahead.popleft()
            yield name, source.result() if isinstance(source, concurrent.futures.Future) else source
    with builtins.open(part, 'wb') as out, contextlib.ExitStack() as done:
        if readers: done.callback(readers.shutdown, cancel_futures=True)   # after an error, the functions not yet called are not
        out.write(b'\0' * off)
        for name, source in sources():
            if name in alias:
                ents.append((name, *placed[alias[name]]))
                if on_progress: on_progress(len(ents), len(names), off)
                continue
            off += -off % ALIGN
            out.seek(off)
            n = 0
            if source is None: raise OSError(f'{name.decode()}: no bytes to write')
            if isinstance(source, bytes):
                out.write(source)
                n = len(source)
            elif not isinstance(source, (str, os.PathLike)): raise TypeError(f'{name.decode()}: {type(source).__name__} is not a path, bytes or a function')
            else:
                with romfs.open(source, 'rb') as f:
                    while b := f.read(8 << 20):
                        out.write(b)
                        n += len(b)
            ents.append((name, off, n))
            if (key := share.get(name.decode())) is not None: placed[key] = (off, n)
            off += n
            if on_progress: on_progress(len(ents), len(names), off)
        out.seek(off)
        pool, idx = bytearray(), bytearray(romfs.ENT.pack(1, len(names), 0, 0, 0, 0))
        for name, a, b in ents:
            idx += romfs.ENT.pack(a, b, len(pool), len(name), 1, 0)
            pool += name
        out.write(idx)
        out.write(pool)
        out.seek(0)
        out.write(romfs.HEAD.pack(romfs.MAGIC, off, len(idx) + len(pool), len(names) + 1))
    romfs.Image(part)   # as the host's check: nothing is published that a reader refuses


def pack(lib, say=print):
    """Returns whether an image now stands for the library"""
    folder, image = folder_of(lib), image_of(lib)
    try:
        before = _names(folder) if os.path.isdir(folder) else None
        if before is not None and not before:
            os.rmdir(folder)
            before = None
        if before is None: return os.path.isfile(image)
        files, share = {n: os.path.join(folder, n) for n in before}, {}
        if os.path.isfile(image) and STAMP not in before:
            old = romfs.Image(image)
            for name, k in old.children(0):
                if name not in files:
                    files[name] = os.path.join(image, name)
                    share[name] = old.entry(k)[:2]
        if share: write(files, image, share=share)
        else: write(files, image)
        if _names(folder) != before:   # a build wrote to it meanwhile: the folder is the newer one
            say(f'{folder} changed while it was packed: it stays a folder')
            return True
        shutil.rmtree(folder)
    except (OSError, ValueError) as e:
        say(f'[RED] could not pack {folder}, it stays a folder: {e}')
        return False
    return True


def remove_image(lib):   # a stopped full build must not take old shaders for the ones it had not reached
    for p in (image_of(lib), image_of(lib) + '.part'):
        try: os.unlink(p)
        except FileNotFoundError: pass


def expand(lib):
    """The state a stopped build leaves, which the packing script's tests start from; no build or run needs it"""
    folder, image = folder_of(lib), image_of(lib)
    if not os.path.isfile(image): return
    work = folder + '.expand'
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)
    for p in romfs.Path(image).iterdir():
        with builtins.open(os.path.join(work, p.name), 'wb') as out: out.write(p.read_bytes())
    if os.path.isdir(folder):
        for n in _names(folder): os.replace(os.path.join(folder, n), os.path.join(work, n))
        shutil.rmtree(folder)
    os.rename(work, folder)
    os.unlink(image)


def app_files(lib):
    return {p.name: str(p) for p in view(lib).iterdir() if p.name.endswith(APP_SUFFIXES) and p.name not in APP_LEAVE_OUT and not p.name.startswith('.') and '.tmp' not in p.name}


def subset(lib, dst):
    files = app_files(lib)
    if not any(n.endswith('.metallib') for n in files): raise ValueError(f'no shader libraries at {lib}')
    src, share = view(lib), {}
    if os.path.isfile(src):
        img = romfs.Image(src)
        share = {name: img.entry(k)[:2] for name, k in img.children(0) if name in files}
    def whole(path):
        with romfs.open(path, 'rb') as f: return f.read()
    write({n: (lambda p=p: whole(p)) for n, p in files.items()}, os.fspath(dst), share=share)
    return len(files)


def has_library(lib): return next(view(lib).glob('*_b.metallib'), None) is not None


def main():
    ap = argparse.ArgumentParser(description='A game\'s shader libraries as one file, read in place')
    ap.add_argument('cmd', choices=('pack', 'path', 'has', 'cat', 'subset'))
    ap.add_argument('lib')
    ap.add_argument('arg', nargs='?')
    a = ap.parse_args()
    if a.cmd == 'pack': return 0 if pack(a.lib) else 1
    if a.cmd == 'path':
        print(view(a.lib))
        return 0
    if a.cmd == 'has': return 0 if has_library(a.lib) else 1
    if a.cmd == 'cat':
        try: sys.stdout.buffer.write((view(a.lib) / a.arg).read_bytes())
        except OSError as e: sys.exit(f'[RED] {e}')
        return 0
    try: print(f'{a.arg}: {subset(a.lib, a.arg)} files, {os.path.getsize(a.arg)} bytes')
    except (OSError, ValueError) as e: sys.exit(f'[RED] {e}')
    return 0


if __name__ == '__main__': sys.exit(main())
