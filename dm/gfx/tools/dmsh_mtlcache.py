#!/usr/bin/env python3
# Compiled-shader cache keyed by the Metal source: one compile per (MSL, compile convention, compiler identity); the compiler is deterministic, which makes reuse safe.
# Libraries of earlier builds are taken only when their reflection names the current compiler and flags. Not in the chain version: change FORMAT when what it stores changes.
import os, re, sys, json, shutil, ctypes, hashlib, threading, subprocess, collections
from typing import Any
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
import romfs, shader_cache, shader_lib
STAMPS = 'metallib/filled.json'

FORMAT = b'dmsh_mtlcache 2'
OUTPUT = re.compile(r'[0-9a-f]{16}_b\.metal')
_clonefile = getattr(ctypes.CDLL(None, use_errno=True), 'clonefile', None)


def cut_from_unit(stem, names):
    return any(stem + c + '.metallib' in names for c in 'xfpv')


def clone(src, dst):
    tmp = f'{dst}.tmp-{os.getpid()}-{threading.get_ident()}'
    if _clonefile is None or _clonefile(os.fsencode(src), os.fsencode(tmp), 0) != 0: shutil.copyfile(src, tmp)
    os.replace(tmp, dst)


def compiler_id(sdk):
    def run(*a): return subprocess.run(['xcrun', '-sdk', sdk, *a], capture_output=True, text=True).stdout
    tools = ''.join(run(t, '--version') for t in ('metal', 'metallib'))
    return re.sub(r'(?m)^InstalledDir:.*\n?', '', tools) + run('--show-sdk-version') + run('--show-sdk-build-version')   # the install path changes with each toolchain mount


class Cache:
    def __init__(self, store: 'shader_cache.Store | shader_cache.Remote', targets, work):
        """targets: {name: (sdk, record, compile)}; work: a folder for temporary .air files"""
        self.store, self.targets, self.work = store, targets, work
        self.ident = {t: hashlib.sha256(FORMAT + b'\0' + rec.encode() + b'\0' + compiler_id(sdk).encode()).digest() for t, (sdk, rec, _) in targets.items()}
        self.stats, self.lock = collections.Counter(), threading.Lock()

    def path(self, target, src):
        d = hashlib.sha256(self.ident[target] + b'\0' + src).hexdigest()
        return f'metallib/{target}/{d}.metallib'

    def with_compiler(self, target, refl) -> dict[str, Any]:
        return dict(refl, chain_metal=self.targets[target][1], chain_compiler=self.ident[target].hex())

    def count(self, what, n=1):
        with self.lock: self.stats[what] += n

    def _compile(self, target, msl, lib):
        air = os.path.join(self.work, f'mtlcache-{target}-{os.getpid()}-{threading.get_ident()}.air')
        try: self.targets[target][2](msl, air, lib)
        finally:
            if os.path.exists(air): os.unlink(air)

    def compile(self, target, msl, lib):
        p = self.path(target, open(msl, 'rb').read())
        if self.store.to_file(p, lib):
            self.count((target, 'reused'))
            return
        tmp = f'{lib}.tmp-{os.getpid()}-{threading.get_ident()}'
        self._compile(target, msl, tmp)
        os.replace(tmp, lib)
        self.store.put_file(p, lib)
        self.count((target, 'compiled'))

    def compile_split(self, target, msl, libs, split):
        """Pieces are cached by their place (<digest>.metallib, _x, _x2): one source always splits the same way"""
        p = self.path(target, open(msl, 'rb').read())
        paths = [p] + [f'{p[:-len(".metallib")]}_x{i if i > 1 else ""}.metallib' for i in range(1, len(libs))]
        if all(self.store.has(x) for x in paths):
            for src, dst in zip(paths, libs): self.store.to_file(src, dst)
            self.count((target, 'reused'))
            return
        whole = f'{libs[0]}.tmp-{os.getpid()}-{threading.get_ident()}'
        self._compile(target, msl, whole)
        try:
            with open(whole, 'rb') as f: pieces = split(f.read())
        finally: os.unlink(whole)
        if len(pieces) != len(libs): raise ValueError(f'the split gave {len(pieces)} libraries, not {len(libs)}')
        for lib, data in zip(libs, pieces):
            with open(whole, 'wb') as f: f.write(data)
            os.replace(whole, lib)
        with self.store.batch():
            for path, lib in zip(paths, libs): self.store.put_file(path, lib)
        self.count((target, 'compiled'))

    def _check(self, target, d, stems, mac_dir):
        rec, picks = self.targets[target][1], collections.defaultdict(list)
        eligible = set()
        for stem in stems:
            try: j = json.loads(romfs.open(os.path.join(d, stem + '.json'), 'rb').read())
            except (OSError, ValueError): return set(), f'no readable {stem}.json'
            if j.get('chain_metal_cut'): continue   # the source on disk is only the standalone MSL
            if j.get('chain_metal') != rec: return set(), f'built with "{j.get("chain_metal")}"'
            if j.get('chain_compiler') != self.ident[target].hex(): return set(), f'{stem} has a different or unrecorded Metal compiler'
            eligible.add(stem)
            if len(picks[j.get('stage')]) < 2: picks[j.get('stage')].append(stem)
        for stem in (s for v in picks.values() for s in v):
            tmp = os.path.join(self.work, f'mtlcache-check-{target}.metallib')
            msl = os.path.join(mac_dir, stem + '.metal')
            inside = mac_dir.endswith('.img')   # the compiler then gets a file of its own
            if inside:
                with romfs.open(msl, 'rb') as f, open(os.path.join(self.work, f'mtlcache-check-{stem}.metal'), 'wb') as o: shutil.copyfileobj(f, o)
                msl = os.path.join(self.work, f'mtlcache-check-{stem}.metal')
            try: self._compile(target, msl, tmp)
            except subprocess.CalledProcessError: return set(), f'{stem}.metal does not compile now'
            finally:
                if inside: os.unlink(msl)
            same = open(tmp, 'rb').read() == romfs.open(os.path.join(d, stem + '.metallib'), 'rb').read()
            os.unlink(tmp)
            if not same: return set(), f'a fresh compile of {stem}.metal differs from its library'
        return eligible, None

    def fill(self, mac_dir):
        """mac_dir: an earlier build's b-<chain>/. → the lines to print"""
        mac_dir = str(shader_lib.view(mac_dir))
        if not romfs.os.path.isdir(mac_dir): return []
        try: stamps = json.loads(self.store.get(STAMPS) or b'{}')
        except ValueError: stamps = {}
        names = set(romfs.os.listdir(mac_dir))
        stems = sorted(n[:-6] for n in names if OUTPUT.fullmatch(n) and not cut_from_unit(n[:-6], names))   # not an AppleDouble ._<name> (exFAT / FAT drives)
        todo, notes = {}, []
        for target, d in (('mac', mac_dir),):
            if target not in self.targets or not d or not romfs.os.path.isdir(d): continue
            libs = names if d == mac_dir else set(romfs.os.listdir(d))
            have = [s for s in stems if s + '.metallib' in libs and not cut_from_unit(s, libs)]
            key = self.ident[target].hex() + ':' + os.path.realpath(d)
            if not have or stamps.get(key) == len(have): continue
            eligible, why = self._check(target, d, have, mac_dir)
            if why: notes.append(f'Metal cache: not filled from {d}: {why}')
            elif eligible: todo[target] = (d, eligible)
            stamps[key] = len(have)
        added = collections.Counter()
        for stem in stems:
            ts = [t for t, (d, have) in todo.items() if stem in have]
            if not ts: continue
            src = romfs.open(os.path.join(mac_dir, stem + '.metal'), 'rb').read()
            for t in ts:
                p = self.path(t, src)
                if self.store.has(p): continue
                self.store.put(p, romfs.open(os.path.join(todo[t][0], stem + '.metallib'), 'rb').read())
                added[t] += 1
        for t, (d, have) in todo.items():
            self.count((t, 'filled'), added[t])
            notes.append(f'Metal cache: {added[t]} {t} libraries added from {d} ({len(have)} there)')
        self.store.put(STAMPS, json.dumps(stamps, indent=1).encode())
        return notes

    def summary(self):
        s = self.stats
        return 'Metal cache: ' + '; '.join(f'{t} {s[t, "reused"]} reused, {s[t, "compiled"]} compiled' + (f', {s[t, "filled"]} filled from earlier builds' if s[t, 'filled'] else '')
                                          for t in self.targets)
