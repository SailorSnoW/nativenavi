#!/usr/bin/env python3
# A folder of many small files nothing reads for now as one uncompressed <folder>.tar. unpack handles every state a stop leaves: a folder wins over a tar;
# a tar beside <folder>.gone wins; .unpack and .part are unfinished. unpack's lock on the parent, released by pack, keeps two builds from packing one folder.
import fcntl, os, shutil, subprocess, time
from pathlib import Path

QUIET_DAYS, QUIET_MIN_FILES, QUIET_FREE = 14, 100, 20 * 2**30
_locks = {}   # a second unpack in this process takes what it already holds


def tar_of(folder): return Path(f'{folder}.tar')
def exists(folder): return Path(folder).is_dir() or tar_of(folder).is_file()


def _remove(p):
    def gone(f, path, e):   # an exFAT or FAT drive drops ._x with x
        if not isinstance(e, FileNotFoundError): raise e
    if p.is_dir() and not p.is_symlink(): shutil.rmtree(p, onexc=gone)
    else: p.unlink(missing_ok=True)


def _lock(folder, say=None):   # without say, it does not wait
    parent = str(Path(folder).parent)
    if parent in _locks or not os.path.isdir(parent): return True
    fd = os.open(parent, os.O_RDONLY)
    try:
        try: fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            if not say:
                os.close(fd)
                return False
            say(f'waiting for another build to finish with {folder}')
            fcntl.flock(fd, fcntl.LOCK_EX)
    except OSError:
        os.close(fd)
        return True
    _locks[parent] = fd
    return True


def _unlock(folder):
    fd = _locks.pop(str(Path(folder).parent), None)
    if fd is not None: os.close(fd)


def unpack(folder, say=print):
    """False when there is nothing yet: the build then makes it. A damaged tar costs only the cache it held"""
    folder = Path(folder)
    tar = tar_of(folder)
    work = Path(f'{folder}.unpack')
    gone = Path(f'{folder}.gone')
    _lock(folder, say)
    _remove(work)
    _remove(Path(f'{tar}.part'))
    if folder.is_dir():
        tar.unlink(missing_ok=True)
        _remove(gone)
        return True
    _remove(gone)
    if not tar.is_file(): return False
    work.mkdir()
    r = subprocess.run(['tar', '-xf', tar, '-C', work], capture_output=True, text=True, errors='replace')
    if r.returncode:
        say(f'[RED] could not unpack {tar}, the build goes on without it: {r.stderr.strip()[:300]}')
        _remove(work)
        tar.unlink()
        return False
    os.rename(work, folder)
    tar.unlink()
    return True


def pack(folder, say=print, xattrs=False):
    """A folder another build is using is left to it. A failed pack is one [RED] line. xattrs: a RomFS folder's case-collision names (hle_fs.c guest_name)"""
    folder = Path(folder)
    tar = tar_of(folder)
    part = Path(f'{tar}.part')
    gone = Path(f'{folder}.gone')
    if not _lock(folder): return
    try:
        if not folder.is_dir(): return
        try:
            subprocess.run(['tar', '--xattrs' if xattrs else '--no-xattrs', '--format=pax', '-cf', part, '-C', folder, '.'], check=True, capture_output=True, env=dict(os.environ, COPYFILE_DISABLE='1'))
            os.replace(part, tar)
            os.rename(folder, gone)
            _remove(gone)
        except (OSError, subprocess.CalledProcessError) as e:
            say(f'[RED] could not pack {folder}, it stays a folder: {getattr(e, "stderr", b"")[:300] if isinstance(e, subprocess.CalledProcessError) else e}')
            try: part.unlink(missing_ok=True)
            except OSError: pass
    finally: _unlock(folder)


def _quiet_size(folder, since):   # None as soon as anything changed at or after since
    stack, n, size = [folder], 0, 0
    while stack:
        with os.scandir(stack.pop()) as it:
            for e in it:
                st = e.stat(follow_symlinks=False)
                if max(st.st_mtime, st.st_ctime) >= since: return None   # a folder just unpacked has old times and a recent ctime
                if e.is_dir(follow_symlinks=False): stack.append(e.path)
                else:
                    n += 1
                    size += st.st_size
    return n, size


def pack_quiet(work, say=print, days: float = QUIET_DAYS, min_files=QUIET_MIN_FILES):
    """Top-level folders of <NUP folder>/<id>-work untouched for days become <name>.tar (`tar --xattrs -xf` brings one back). regress/, small folders and
    Python virtual environments stay; an existing tar is never overwritten; the tar must fit beside the folder with QUIET_FREE to spare."""
    done, since = [], time.time() - days * 86400
    try:
        with os.scandir(work) as it: entries = sorted((e for e in it if e.is_dir(follow_symlinks=False)), key=lambda e: e.name)
    except OSError: return done
    for e in entries:
        if e.name == 'regress' or e.name.endswith(('.gone', '.unpack')) or tar_of(e.path).exists() or os.path.isfile(os.path.join(e.path, 'pyvenv.cfg')): continue
        try:
            if os.stat(e.path).st_ctime >= since or not (size := _quiet_size(e.path, since)) or size[0] < min_files: continue
            if shutil.disk_usage(work).free < size[1] + QUIET_FREE:   # not vol_space: housekeeping nobody asked for makes macOS purge nothing
                say(f'{e.path} stays a folder: no room for its {size[1] / 2**30:.1f} GB tar')
                continue
        except OSError: continue
        say(f'packing {e.path}, untouched for {days} days: {size[0]:,} files into one file')
        pack(e.path, say, xattrs=True)
        if tar_of(e.path).is_file() and not os.path.isdir(e.path): done.append(e.name)
    return done
