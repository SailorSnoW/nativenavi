"""Runner of the file-parser fuzz harnesses (dm/tools/fuzz/fuzz_*.py) and replay of their corpus. A malformed file must end in the parser's own rejection:
any other exception, an allocation over allowed() or an input over TIMEOUT is a finding. known.tsv's findings are skipped, and the replay expects them to fail.

Usage: python3 dm/tools/fuzz/fuzz_<parser>.py [--time S] [--engine atheris|random] [--findings DIR] [libFuzzer options]
  atheris (.github/requirements/fuzz.txt) guides mutations by coverage, else the random engine mutates blindly. A finding goes to <findings>/crash-<sha1>, exit 1.
"""
import argparse
import atexit
import fnmatch
import hashlib
import importlib
import os
from pathlib import Path
import random
import shutil
import signal
import struct
import sys
import tempfile
import time
import traceback
import zlib

REPO = Path(__file__).resolve().parents[3]
HERE = Path(__file__).resolve().parent
CORPUS = HERE / 'corpus'
KNOWN = HERE / 'known.tsv'
TIMEOUT = 10   # seconds
sys.path[:0] = [str(REPO / 'dm/tools'), str(REPO / 'dm/gfx/tools')]


class Oversized(BaseException):   # a BaseException, so a parser's own `except Exception` cannot swallow it
    pass


class Timeout(BaseException):
    pass


class Finding(Exception):
    def __init__(self, signature, text):
        super().__init__(f'{signature}\n{text}')
        self.signature, self.text = signature, text


_limit = [1 << 62]


def allowed(n):
    """16 MiB, or 256 bytes per input byte (LZ4's largest ratio is about 255). zlib's output too, though deflate reaches about 1032: a parser inflates no further than its format declares"""
    return max(16 << 20, 256 * n)


def _check_size(n, what):
    if n > _limit[0]: raise Oversized(f'{what} of {n:#x} bytes for an input that allows {_limit[0]:#x}')


def _guarded_bytearray(*args, **kwargs):
    if args and isinstance(args[0], int): _check_size(args[0], 'bytearray')
    return bytearray(*args, **kwargs)


def guard(*modules):
    """bytearray(n) in these modules, lz4.block.decompress (uncompressed_size is allocated up front) and zlib.decompress"""
    for m in modules: m.bytearray = _guarded_bytearray
    lz4_block = importlib.import_module('lz4.block')
    if not getattr(lz4_block.decompress, 'fuzz_guarded', False):
        real = lz4_block.decompress
        def decompress(source, uncompressed_size=-1, **kwargs):
            _check_size(uncompressed_size, 'lz4.block.decompress')
            return real(source, uncompressed_size=uncompressed_size, **kwargs)
        decompress.fuzz_guarded = True
        lz4_block.__dict__['decompress'] = decompress   # pyright does not know the attribute
    if not getattr(zlib.decompress, 'fuzz_guarded', False):
        real_zlib = zlib.decompress
        def zlib_decompress(data, wbits=zlib.MAX_WBITS, bufsize=zlib.DEF_BUF_SIZE):   # inflated first to one byte past the limit; then the real call
            try: _check_size(len(zlib.decompressobj(wbits).decompress(data, _limit[0] + 1)), 'zlib.decompress')
            except (zlib.error, ValueError): pass   # a bad wbits too: the real call raises its own error
            return real_zlib(data, wbits, bufsize)
        zlib_decompress.fuzz_guarded = True
        zlib.__dict__['decompress'] = zlib_decompress


class InputFile:
    """Rewritten for every input; its folder is removed at exit"""
    def __init__(self, name): self.name, self.path = name, None

    def write(self, data):
        if self.path is None:
            d = tempfile.mkdtemp(prefix='nxfuzz-')
            atexit.register(shutil.rmtree, d, True)
            self.path = os.path.join(d, self.name)
        with open(self.path, 'wb') as f: f.write(data)
        return self.path


def import_shader_split():
    return importlib.import_module('shader_split')


class Broken(AssertionError):
    def __init__(self, where, msg):
        super().__init__(msg)
        self.where = where


def broken(where, msg): return Broken(where, msg)


def signature(e):
    """Of the two innermost frames in the parser's file, else of the innermost frame"""
    if isinstance(e, Broken): return f'AssertionError@{e.where}'
    frames = traceback.extract_tb(e.__traceback__)
    inside = [f for f in frames if f.filename.startswith(str(REPO / 'dm')) and not f.filename.startswith(str(HERE))]
    last = (inside or frames)[-2:]   # a shared helper fails differently for each reader that calls it
    if len(last) == 2 and last[0].filename != last[1].filename: last = last[1:]
    where = f'{os.path.basename(last[-1].filename)}:' + '>'.join(f.name for f in last) if last else '?'
    t = type(e)
    name = t.__qualname__ if t.__module__ in ('builtins', __name__) else f'{t.__module__.lstrip("_")}.{t.__qualname__}'
    return f'{name}@{where}'


def known(harness):
    """known.tsv: harness, signature pattern (fnmatch, one cause), corpus file, issue, what"""
    out = {}
    for line in KNOWN.read_text().splitlines():
        if not line.strip() or line.startswith('#'): continue
        h, sig, name = line.split('\t')[:3]
        if h == harness: out[sig] = name.split(',')
    return out


def matches(sig, pattern):
    """A pattern that leaves the exception open (*@...) never covers a hang or an oversized allocation"""
    if '|' in pattern: return any(matches(sig, p) for p in pattern.split('|'))
    if sig.startswith(('Oversized@', 'Timeout@')) and not pattern.startswith(sig.split('@')[0] + '@'): return False
    return fnmatch.fnmatchcase(sig, pattern)


def _alarm(signum, frame): raise Timeout(f'one input ran longer than {TIMEOUT} s')


def run(check, data, skip=(), alarm=False):
    _limit[0] = allowed(len(data))
    if alarm:
        old = signal.signal(signal.SIGALRM, _alarm)
        signal.alarm(TIMEOUT)
    try: check(data)
    except (Exception, SystemExit, Oversized, Timeout) as e:
        if isinstance(e, MemoryError): e = Oversized(f'MemoryError: {e}').with_traceback(e.__traceback__)
        sig = signature(e)
        if any(matches(sig, k) for k in skip): return
        raise Finding(sig, ''.join(traceback.format_exception(e))) from None
    finally:
        _limit[0] = 1 << 62
        if alarm:
            signal.alarm(0)
            signal.signal(signal.SIGALRM, old)


def replay(harness, check, data):
    """Strictly: known findings fail too"""
    run(check, data, alarm=True)


# the random engine, without atheris
INTERESTING = (0, 1, 2, 0x7f, 0x80, 0xff, 0x100, 0x7fff, 0x8000, 0xffff, 0x10000, 0x7fffffff, 0x80000000, 0xffffffff)


def mutate(d, rng, pool):
    d = bytearray(d)
    for _ in range(rng.choice((1, 1, 2, 4, 8))):
        op, n = rng.randrange(7), len(d)
        if op == 0 and n: d[rng.randrange(n)] ^= 1 << rng.randrange(8)
        elif op == 1 and n: d[rng.randrange(n)] = rng.choice((0, 0xff, 0x7f, 0x80, rng.randrange(256)))
        elif op == 2 and n >= 4:   # headers are word-aligned
            o = rng.randrange(n // 4) * 4
            v = rng.choice(INTERESTING + (n, n - 1, n + 1, rng.randrange(1 << 32)))
            d[o:o + 4] = struct.pack('<I', v & 0xffffffff)
        elif op == 3 and n: del d[rng.randrange(n):]
        elif op == 4: d += bytes(rng.randrange(256) for _ in range(rng.randrange(1, 64)))
        elif op == 5 and n:
            a = rng.randrange(n)
            b = rng.randrange(a, min(n, a + 256) + 1)
            d[a:a] = d[a:b]
        elif op == 6:
            o = rng.choice(pool)
            a = rng.randrange(len(o) + 1)
            b = rng.randrange(a, len(o) + 1)
            at = rng.randrange(n + 1)
            d[at:at + (b - a)] = o[a:b]
    return bytes(d)


def random_engine(check, pool, seconds, findings, skip, seed):
    rng, start, runs = random.Random(seed), time.monotonic(), 0
    while time.monotonic() - start < seconds:
        data = mutate(rng.choice(pool), rng, pool)
        try: run(check, data, skip, alarm=True)
        except Finding as f:
            path = save(findings, data)
            print(f.text, file=sys.stderr)
            print(f'==finding== {f.signature}: input written to {path}', file=sys.stderr)
            return 1
        runs += 1
    t = time.monotonic() - start
    print(f'Done {runs} runs in {t:.0f} second(s): {runs / max(t, 1e-9):.0f} exec/s (random engine)')
    return 0


def save(findings, data):
    os.makedirs(findings, exist_ok=True)
    path = os.path.join(findings, 'crash-' + hashlib.sha1(data).hexdigest())
    with open(path, 'wb') as f: f.write(data)
    return path


def main(harness, check, seeds):
    ap = argparse.ArgumentParser(description=f'Fuzz the {harness} parser (dm/tools/fuzz/fuzzlib.py)')
    ap.add_argument('--time', type=int, default=60, help='seconds to fuzz (default 60)')
    ap.add_argument('--engine', choices=('auto', 'atheris', 'random'), default='auto', help='atheris when installed (auto), or the blind random engine')
    ap.add_argument('--findings', default=f'fuzz-findings/{harness}', help='folder for crashing inputs (default fuzz-findings/<parser>)')
    ap.add_argument('--seed', type=int, default=None, help='random engine: the RNG seed (default: time)')
    args, rest = ap.parse_known_args()
    pool = list(seeds().values())
    corpus = CORPUS / harness
    if corpus.is_dir(): pool += [p.read_bytes() for p in sorted(corpus.iterdir()) if p.is_file()]
    skip = set(known(harness))
    engine = args.engine
    if engine == 'auto':
        try:
            importlib.import_module('atheris')
            engine = 'atheris'
        except ImportError: engine = 'random'
    print(f'{harness}: {len(pool)} seeds and corpus inputs, engine {engine}, {args.time} s, {len(skip)} known findings skipped', flush=True)
    if sys.platform == 'linux':   # RLIMIT_AS, which macOS does not enforce: a MemoryError backstops what guard() misses
        import resource
        resource.setrlimit(resource.RLIMIT_AS, (4 << 30, resource.getrlimit(resource.RLIMIT_AS)[1]))
    if engine == 'random':
        return random_engine(check, pool, args.time, args.findings, skip, args.seed if args.seed is not None else time.time_ns())
    atheris = importlib.import_module('atheris')
    work = tempfile.mkdtemp(prefix=f'nxfuzz-{harness}-')   # libFuzzer adds to its first corpus folder: never the repository
    for i, d in enumerate(pool): Path(work, f'seed-{i:03d}').write_bytes(d)
    os.makedirs(args.findings, exist_ok=True)
    argv = [sys.argv[0], f'-max_total_time={args.time}', f'-timeout={TIMEOUT}', '-rss_limit_mb=2048', f'-max_len={max(4096, 2 * max(map(len, pool)))}',
            f'-artifact_prefix={args.findings.rstrip("/")}/', *rest, work]
    atheris.instrument_all()
    def one(data):
        try: run(check, data, skip)
        except Finding as f:
            print(f'==finding== {f.signature}', file=sys.stderr)
            raise
    try:
        atheris.Setup(argv, one)
        atheris.Fuzz()
    finally: shutil.rmtree(work, ignore_errors=True)
    return 0


if __name__ == '__main__':
    sys.exit(__doc__)
