#!/usr/bin/env python3
# The shader build's compile cache: one SQLite file per chain version, <cache folder>/cas-<chain>.db, entries read and written whole by name, bytes stored once by hash,
# as a Zstandard frame when that is smaller.
# A build writes only its chain's file; Metal compiles reused from older ones are copied in (for_chain).
# Only saves work: a damaged file is made anew, a full drive is one [RED] line. Not part of the translation chain: it decides no output.
import contextlib, fcntl, hashlib, json, multiprocessing, os, re, sqlite3, threading, time
from pathlib import Path

try: from compression import zstd
except ImportError:
    try: from backports import zstd   # pyright: ignore[reportMissingImports]  # before Python 3.14
    except ImportError: zstd = None

# v2: (codec, payload), where v1 had raw data. A checkout from before fails on "no such column: b.data" instead of reading a frame as a library.
# A v1 file stays v1, read and written raw in place; a new chain's file is v2 and takes v1's blobs compressed.
SCHEMA = ('CREATE TABLE IF NOT EXISTS blob(id INTEGER PRIMARY KEY, hash BLOB UNIQUE NOT NULL, codec INTEGER NOT NULL, payload BLOB NOT NULL)',
          'CREATE TABLE IF NOT EXISTS entry(name TEXT PRIMARY KEY, blob INTEGER NOT NULL, mtime REAL NOT NULL) WITHOUT ROWID')
RAW, ZSTD, ZSTD_LEVEL, ZSTD_MIN = 0, 1, 1, 64
_TLS = threading.local()   # a compressor per thread
SHADER_ENTRY = re.compile(r'shader/([0-9a-f]{64})/')
WAIT = 600   # seconds another process's write may hold this one up
_DAMAGED = ('malformed', 'not a database', 'disk image')
DBMOVED = 1032
READ_ONLY = None   # set only by --run
GENERATION = re.compile(r'cas-([0-9a-f]{12})\.db')
LEGACY = 'cas.db'   # every chain's entries, from before generations


def _encode(data):   # (codec, payload): raw when short, incompressible or without zstd
    if zstd is None or len(data) < ZSTD_MIN: return RAW, data
    comp = getattr(_TLS, 'zstd', None)
    if comp is None: comp = _TLS.zstd = zstd.ZstdCompressor(level=ZSTD_LEVEL)
    packed = comp.compress(data, mode=zstd.ZstdCompressor.FLUSH_FRAME)
    return (ZSTD, packed) if len(packed) < len(data) else (RAW, data)


def _decode(codec, payload):
    if codec == RAW: return bytes(payload)
    if codec != ZSTD: raise RuntimeError(f'unsupported shader compile cache blob codec {codec}')
    if zstd is None: raise RuntimeError('reading a compressed shader compile cache needs Python 3.14+ or backports.zstd (pip install backports.zstd)')
    return zstd.decompress(payload)


def _bad_frame(e): return zstd is not None and isinstance(e, zstd.ZstdError)


def _blob_cols(db, schema='main'): return {r[1] for r in db.execute(f'PRAGMA {schema}.table_info(blob)')}


def _get_sql(v2, with_hash=False):
    cols = ('b.hash, b.codec, b.payload' if with_hash else 'b.codec, b.payload') if v2 else ('b.hash, 0, b.data' if with_hash else '0, b.data')
    return f'SELECT {cols} FROM entry e JOIN blob b ON b.id=e.blob WHERE e.name=?'


def generations(folder):
    """Newest first: cas-<chain>.db, then cas.db"""
    folder = os.fspath(folder)
    try: names = os.listdir(folder)
    except OSError: return []
    def when(p):
        try: return max(os.path.getmtime(p + s) for s in ('', '-wal') if os.path.exists(p + s))
        except (OSError, ValueError): return 0
    gens = sorted((os.path.join(folder, n) for n in names if GENERATION.fullmatch(n)), key=when, reverse=True)
    return gens + ([os.path.join(folder, LEGACY)] if LEGACY in names else [])


class _ReadOnly:
    """mode=ro sees a writer's committed WAL and never migrates, repairs or writes. An older generation serves only its Metal compiles."""
    def __init__(self, paths, say, prefixes=('shader/', 'variant/', 'metallib/')):
        self.say, self.lock, self.prefixes, self.dbs, self._v2 = say, threading.RLock(), prefixes, [], {}
        for p in map(os.fspath, paths):
            if not os.path.isfile(p): continue
            db = None
            try:
                db = sqlite3.connect(Path(p).resolve().as_uri() + '?mode=ro', uri=True, timeout=WAIT, check_same_thread=False, isolation_level=None)
                cols = _blob_cols(db)
                self._v2[db] = True if 'payload' in cols else False if 'data' in cols else None   # None: no table yet
                self.dbs.append((p, db))
            except sqlite3.Error as e:
                if db is not None:
                    with contextlib.suppress(sqlite3.Error): db.close()
                self.say(f'[RED] the read-only shader cache {p} cannot be read and is ignored: {e}')

    def _failed(self, i, e):
        p, db = self.dbs.pop(i)
        self._v2.pop(db, None)
        self.say(f'[RED] the read-only shader cache {p} cannot be read and is ignored: {e}')
        with contextlib.suppress(sqlite3.Error): db.close()

    def shared(self, name): return name.startswith(self.prefixes) and name != 'metallib/filled.json'

    def _each(self, sql, *args):
        with self.lock:
            i = 0
            while i < len(self.dbs):
                try: rows = self.dbs[i][1].execute(sql, args).fetchall()
                except sqlite3.Error as e:
                    self._failed(i, e)
                    continue
                i += 1
                yield rows

    def _fetch(self, name, seen):   # (row, db, v2) from the first file not in seen that holds name, or (None, None, False)
        with self.lock:
            i = 0
            while i < len(self.dbs):
                db = self.dbs[i][1]
                if db in seen:
                    i += 1
                    continue
                seen.add(db)
                try:
                    v2 = self._v2.get(db)
                    if v2 is None:
                        cols = _blob_cols(db)
                        v2 = 'data' not in cols
                        if cols: self._v2[db] = v2
                    rows = db.execute(_get_sql(v2, True), (name,)).fetchall()
                except sqlite3.Error as e:
                    self._failed(i, e)
                    continue
                if rows: return rows[0], db, v2
                i += 1
        return None, None, False

    def get_blob(self, name):   # (hash, codec, payload, from_v2), not decoded, or None
        if not self.shared(name): return None
        row, _, v2 = self._fetch(name, set())
        return (bytes(row[0]), row[1], bytes(row[2]), v2) if row is not None else None

    def get_verified_blob(self, name):   # (hash, codec, payload, from_v2, data) once it decodes; a file whose blob does not is dropped and the next one asked
        if not self.shared(name): return None
        seen = set()
        while True:
            row, db, v2 = self._fetch(name, seen)
            if row is None: return None
            digest, codec, payload = bytes(row[0]), row[1], bytes(row[2])
            try: data = _decode(codec, payload)
            except Exception as e:
                if not (isinstance(e, RuntimeError) or _bad_frame(e)): raise
                with self.lock:
                    for i, (_, d) in enumerate(self.dbs):
                        if d is db:
                            self._failed(i, e)
                            break
                continue
            return digest, codec, payload, v2, data

    def get(self, name):
        got = self.get_verified_blob(name)
        return got[4] if got is not None else None

    def has(self, name): return self.shared(name) and any(rows for rows in self._each('SELECT 1 FROM entry WHERE name=?', name))

    def names(self, prefix):
        return {n for rows in self._each('SELECT name FROM entry WHERE name>=? AND name<?', prefix, prefix + '\U0010ffff') for n, in rows if self.shared(n)}

    def close(self):
        with self.lock:
            for _, db in self.dbs:
                with contextlib.suppress(sqlite3.Error): db.close()
            self.dbs, self._v2 = [], {}


class _Files:
    def get(self, name) -> 'bytes | None': raise NotImplementedError
    def put(self, name, data, when=None): raise NotImplementedError

    def put_file(self, name, path):
        with open(path, 'rb') as f: self.put(name, f.read())

    def to_file(self, name, path):   # whole or not at all; False when there is none
        data = self.get(name)
        if data is None: return False
        tmp = f'{path}.tmp-{os.getpid()}-{threading.get_ident()}'
        try:
            with open(tmp, 'wb') as f: f.write(data)
            os.replace(tmp, path)
        except BaseException:
            try: os.unlink(tmp)
            except OSError: pass
            raise
        return True


class Store(_Files):
    def __init__(self, path, say=print, *, readonly=None, older=()):
        """readonly: read, never copied (regress.py's fallback to the player's); older: earlier generations, copied in when read"""
        self.path, self.say = os.fspath(path), say
        self.lock = threading.RLock()   # one connection for dmsh_build.py's threads: a read or write is cheap next to a compile
        self.readers, self.wal, self.opener, self.v2 = {}, False, threading.get_ident(), True   # other threads read through their own read-only connections (_reader)
        self.full = False   # after a failed write (a full drive) no write is tried, and its batch is rolled back
        self.outer = False   # SQLite may end the transaction itself on a failed write, so its own flag cannot tell
        source = readonly if readonly is not None else READ_ONLY
        if source and os.path.realpath(source) == os.path.realpath(self.path): raise ValueError('the writable and read-only shader caches must differ')
        self.use_lock = None
        if source:
            # a killed harness may leave its builder and its workers alive: the writer keeps its own lease
            os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
            self.use_lock = open(self.path + '.lock', 'a')
            fcntl.flock(self.use_lock, fcntl.LOCK_SH)
        own = os.path.realpath(self.path)
        sources = generations(source) if source and os.path.isdir(source) else [source] if source else []
        self.readonly = _ReadOnly([p for p in sources if os.path.realpath(p) != own], say) if source else None
        older = [p for p in map(os.fspath, older) if os.path.realpath(p) != own]
        self.older = _ReadOnly(older, say, ('metallib/',)) if older else None
        self._open()

    def _connect(self): return sqlite3.connect(self.path, timeout=WAIT, check_same_thread=False, isolation_level=None)

    def _open(self):
        os.makedirs(os.path.dirname(os.path.abspath(self.path)), exist_ok=True)
        self.db = self._connect()
        try:
            try: self._prepare()
            except sqlite3.OperationalError as e:
                # Workaround: on exFAT a new file took another inode at SQLite's first write and SQLite refused writes (SQLITE_READONLY_DBMOVED); the file is opened
                # again once. Why SQLite's guard missed it is open (docs/reference/packing.md).
                if getattr(e, 'sqlite_errorcode', None) != DBMOVED: raise
                with contextlib.suppress(sqlite3.Error): self.db.close()
                self.db = self._connect()
                self._prepare()
        except sqlite3.DatabaseError as e:
            if not self._damage(e): raise

    def _prepare(self):   # each step does nothing to a file that has it, so a reopened file takes it again
        self.db.execute('PRAGMA page_size=16384')   # only before the first table
        self.db.execute('PRAGMA auto_vacuum=INCREMENTAL')
        # WAL: readers in other processes do not wait for the writer, and a commit needs no fsync. A drive without shared memory stays on the unsynced rollback journal.
        self.wal = self.db.execute('PRAGMA journal_mode=WAL').fetchone()[0].lower() == 'wal'
        self.db.execute('PRAGMA synchronous=NORMAL' if self.wal else 'PRAGMA synchronous=OFF')
        self.v2 = 'data' not in _blob_cols(self.db)
        for s in SCHEMA if self.v2 else SCHEMA[1:]: self.db.execute(s)

    def _damage(self, e, force=False):
        if not force and not any(w in str(e) for w in _DAMAGED): return False
        self.say(f'[RED] the shader compile cache {self.path} is damaged, and is made anew: {e}')
        self._close_readers()
        with contextlib.suppress(sqlite3.Error): self.db.close()
        for suffix in ('', '-wal', '-shm', '-journal'):
            with contextlib.suppress(FileNotFoundError): os.unlink(self.path + suffix)
        self._open()
        return True

    def _run(self, sql, *args):
        with self.lock:
            try: return self.db.execute(sql, args).fetchall()
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise
                return []

    def has(self, name):
        return bool(self._run('SELECT 1 FROM entry WHERE name=?', name)) or any(r is not None and r.has(name) for r in (self.older, self.readonly))

    @contextlib.contextmanager
    def _reader(self):
        """None when the read belongs on the one connection: the opening thread, inside a batch, or without WAL. Taking the lock to ask lets another thread's
        batch finish first. A connection per thread lets shader_lib.py's image write read several libraries at once."""
        me, reader = threading.get_ident(), None
        with self.lock:
            if me != self.opener and self.wal and not self.outer and not self.db.in_transaction:
                reader = self.readers.get(me)
                if reader is None:
                    try: db = sqlite3.connect(Path(self.path).resolve().as_uri() + '?mode=ro', uri=True, timeout=WAIT, check_same_thread=False, isolation_level=None)
                    except sqlite3.Error: pass
                    else: reader = self.readers[me] = (db, threading.RLock())
                if reader: reader[1].acquire()   # under the registry lock: recovery cannot retire it before the query begins
        try: yield reader[0] if reader else None
        finally:
            if reader: reader[1].release()   # before a failed query takes self.lock again through _run

    def _local(self, name):   # (codec, payload) of this file alone, or None
        rows = None
        with self._reader() as db:
            if db is not None:
                try: rows = db.execute(_get_sql(self.v2), (name,)).fetchall()
                except sqlite3.Error: pass   # asked again on the one connection, which also finds a damaged file
        if rows is None: rows = self._run(_get_sql(self.v2), name)
        return (rows[0][0], bytes(rows[0][1])) if rows else None

    def _adopt_older(self, name):
        """(codec, payload, data) of an older generation's entry, copied into this file first so it stays when that generation goes; a v2 blob is copied as it is"""
        if not self.older or (got := self.older.get_verified_blob(name)) is None: return None
        digest, codec, payload, from_v2, data = got
        if not self.v2: codec, payload = RAW, data
        elif not from_v2: codec, payload = _encode(data)
        self.put_blob(name, digest, codec, payload)
        return codec, payload, data

    def get_raw(self, name):   # (codec, payload), not decoded: a worker decodes it (Remote)
        if (raw := self._local(name)) is not None: return raw
        if (adopted := self._adopt_older(name)) is not None: return adopted[:2]
        if self.readonly and (got := self.readonly.get_blob(name)) is not None: return got[1], got[2]
        return None

    def get(self, name):
        if (raw := self._local(name)) is not None:
            try: return _decode(*raw)
            except Exception as e:
                if not _bad_frame(e): raise   # a missing zstd module or an unknown codec leaves the file as it is
                with self.lock: self._damage(e, force=True)
                return None
        if (adopted := self._adopt_older(name)) is not None: return adopted[2]
        return self.readonly.get(name) if self.readonly else None

    def names(self, prefix):
        local = {r[0] for r in self._run('SELECT name FROM entry WHERE name>=? AND name<?', prefix, prefix + '\U0010ffff')}
        for r in (self.older, self.readonly):
            if r is not None: local |= r.names(prefix)
        return local

    def blob_ids(self, prefix):   # {name: blob id} of this file's entries under prefix: dmsh_build.py's staged outputs, which share a blob when their bytes are the same
        return dict(self._run('SELECT name, blob FROM entry WHERE name>=? AND name<?', prefix, prefix + '\U0010ffff'))

    def put_link(self, name, digest, when=None):
        """Points name at the blob of that digest: False when this file has none and the bytes must come (put_blob)"""
        if self.full: return True
        with self.lock:
            try:
                row = self.db.execute('SELECT id FROM blob WHERE hash=?', (digest,)).fetchone()
                if not row: return False
                with self.batch(): self.db.execute('INSERT OR REPLACE INTO entry(name, blob, mtime) VALUES(?, ?, ?)', (name, row[0], time.time() if when is None else when))
                return True
            except sqlite3.OperationalError as e:
                self._unwritable(e)
                return True
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise
                return False

    def put_blob(self, name, digest, codec, payload, when=None):   # put of bytes already hashed and encoded
        self._put(name, digest, when, lambda: (codec, payload))

    def put(self, name, data, when=None):
        """A full drive is one [RED] line: the build goes on without the cache, and this write's batch is rolled back"""
        if self.full: return   # SQLite may have rolled the failed batch back already
        digest, encoded = hashlib.blake2b(data, digest_size=16).digest(), None
        if self.v2 and not self.outer:
            # compressed before the lock, so that dmsh_build.py's threads compress at once
            with self._reader() as db:
                if db is not None:
                    try:
                        if not db.execute('SELECT 1 FROM blob WHERE hash=?', (digest,)).fetchone(): encoded = _encode(data)
                    except sqlite3.Error: pass
        self._put(name, digest, when, lambda: (encoded or _encode(data)) if self.v2 else (RAW, data))

    def _put(self, name, digest, when, encoded):   # encoded() is asked only when this file has no blob of that digest
        if self.full: return
        with self.lock:
            try:
                with self.batch():
                    row = self.db.execute('SELECT id FROM blob WHERE hash=?', (digest,)).fetchone()
                    if row: blob = row[0]
                    else:
                        codec, payload = encoded()
                        if self.v2: blob = self.db.execute('INSERT INTO blob(hash, codec, payload) VALUES(?, ?, ?)', (digest, codec, payload)).lastrowid
                        else: blob = self.db.execute('INSERT INTO blob(hash, data) VALUES(?, ?)', (digest, _decode(codec, payload))).lastrowid
                    self.db.execute('INSERT OR REPLACE INTO entry(name, blob, mtime) VALUES(?, ?, ?)', (name, blob, time.time() if when is None else when))
            except sqlite3.OperationalError as e: self._unwritable(e)
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise

    def _unwritable(self, e):
        if any(w in str(e) for w in _DAMAGED): self._damage(e)
        elif not self.full:
            self.full = True
            self.say(f'[RED] the shader compile cache {self.path} cannot be written, the build goes on without it: {e}')

    @contextlib.contextmanager
    def batch(self):
        """All or none (a shader's outputs and the index naming them), one commit; nests. A failed write or commit rolls it back and sets full, which dmsh_build.py checks after."""
        with self.lock:
            if self.outer or self.db.in_transaction:
                yield
                return
            was = self.full
            self.db.execute('BEGIN IMMEDIATE')
            self.outer = True
            try: yield
            except BaseException:
                if self.db.in_transaction: self.db.rollback()
                raise
            else:
                if self.full and not was:
                    if self.db.in_transaction: self.db.rollback()
                else:
                    try: self.db.commit()
                    except sqlite3.OperationalError as e:
                        with contextlib.suppress(sqlite3.Error):
                            if self.db.in_transaction: self.db.rollback()
                        self._unwritable(e)
            finally: self.outer = False

    def prune_shaders(self, keep, since):
        """Returns (shaders removed, bytes of their sources and reflection)"""
        # sizes only for what goes: reading them with the names walks the blob table too
        rows = self._run("SELECT name, mtime FROM entry WHERE name>='shader/' AND name<'shader0'")
        newest, names = {}, {}
        for name, mtime in rows:
            m = SHADER_ENTRY.match(name)
            if not m: continue
            d = m.group(1)
            newest[d] = max(newest.get(d, 0), mtime)
            names.setdefault(d, []).append(name)
        gone = [d for d in newest if d not in keep and newest[d] < since]
        if not gone: return 0, 0
        size, col = {}, 'payload' if self.v2 else 'data'   # the bytes stored, compressed or not
        for d in gone:
            size[d] = sum(n for name, n in self._run(f'SELECT e.name, length(b.{col}) FROM entry e JOIN blob b ON b.id=e.blob WHERE e.name>=? AND e.name<?', f'shader/{d}/', f'shader/{d}0')
                          if not name.endswith('.metallib'))
        with self.lock, self.batch():
            self.db.executemany('DELETE FROM entry WHERE name=?', ((n,) for d in gone for n in names[d]))
            self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
        self._run('PRAGMA incremental_vacuum')
        return len(gone), sum(size.get(d, 0) for d in gone)

    def delete_prefix(self, prefix):
        with self.lock:
            try:
                with self.batch():
                    self.db.execute('DELETE FROM entry WHERE name>=? AND name<?', (prefix, prefix + '\U0010ffff'))
                    self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
            except sqlite3.DatabaseError as e:
                if not self._damage(e): raise
        self._run('PRAGMA incremental_vacuum')

    def _close_readers(self, thread=None):
        with self.lock:
            readers = [self.readers.pop(thread)] if thread in self.readers else [] if thread is not None else list(self.readers.values())
            if thread is None: self.readers = {}
            for db, reading in readers:   # no connection to the old file may be published during recovery
                with reading, contextlib.suppress(sqlite3.Error): db.close()   # an active execute/fetchall finishes before its connection is closed

    def forget(self): self._close_readers(threading.get_ident())

    def close(self):   # also folds the write-ahead log into the file
        with self.lock:
            self._close_readers()   # first: the last connection to close folds the log
            with contextlib.suppress(sqlite3.Error): self.db.close()
        for r in (self.readonly, self.older):
            if r: r.close()
        if self.use_lock:
            self.use_lock.close()
            self.use_lock = None

    def trim(self, limit):
        """Oldest-written groups first, each whole. Only with no builder using this regression cache."""
        if limit < 1 << 20: raise ValueError('the shader cache limit must be at least 1 MiB')
        self._run('PRAGMA wal_checkpoint(TRUNCATE)')
        self._run('PRAGMA incremental_vacuum')
        before = os.path.getsize(self.path)
        if before <= limit: return 0
        with self.batch(): self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
        self._run('PRAGMA incremental_vacuum')
        self._run('PRAGMA wal_checkpoint(TRUNCATE)')
        rows = self._run(f'SELECT e.name,e.blob,e.mtime,length(b.{"payload" if self.v2 else "data"}) FROM entry e JOIN blob b ON b.id=e.blob')
        groups, refs, sizes = {}, {}, {}
        for name, blob, when, size in rows:
            parts = name.split('/')
            group = '/'.join(parts[:2]) if parts[0] in ('shader', 'variant', 'stage') else re.sub(r'_x\d*(?=\.metallib$)', '', name)
            ns, newest = groups.setdefault(group, ([], when))
            ns.append((name, blob))
            groups[group] = ns, max(newest, when)
            refs[blob] = refs.get(blob, 0) + 1
            sizes[blob] = size
        # a stopped build's stage is not a reusable result
        ordered = iter(sorted(groups, key=lambda g: (g != 'metallib/filled.json' and not g.startswith('stage/'), groups[g][1], g)))
        removed = 0
        while os.path.getsize(self.path) > limit:
            names, freed = [], 0
            target = max(os.path.getsize(self.path) - limit, before // 10)
            for group in ordered:
                for name, blob in groups[group][0]:
                    names.append((name,))
                    refs[blob] -= 1
                    if not refs[blob]: freed += sizes[blob]
                if freed >= target: break
            if not names: break
            with self.batch():
                self.db.executemany('DELETE FROM entry WHERE name=?', names)
                self.db.execute('DELETE FROM blob WHERE id NOT IN (SELECT blob FROM entry)')
            removed += len(names)
            self._run('PRAGMA incremental_vacuum')
            self._run('PRAGMA wal_checkpoint(TRUNCATE)')
        self.say(f'regression shader cache: removed {removed:,} entries, {before / (1 << 30):.2f} → {os.path.getsize(self.path) / (1 << 30):.2f} GiB (limit {limit / (1 << 30):.2f} GiB)')
        return removed


@contextlib.contextmanager
def bounded(path, limit, say=print):
    """Only an idle cache is trimmed; active builds may exceed the limit for a while."""
    path = Path(path)
    path.parent.mkdir(parents=True, exist_ok=True)
    with open(str(path) + '.lock', 'a') as lock:
        def trim():
            try: fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
            except BlockingIOError: return
            try:
                if path.is_file():
                    store = Store(path, say)
                    try: store.trim(limit() if callable(limit) else limit)
                    finally: store.close()
            finally: fcntl.flock(lock, fcntl.LOCK_UN)
        trim()
        fcntl.flock(lock, fcntl.LOCK_SH)
        try: yield
        finally:
            fcntl.flock(lock, fcntl.LOCK_UN)
            trim()


class _Gone(Exception): pass
class _Abort(Exception): pass


class Served:
    """Forked workers must not open the file: Apple's SQLite calls os_signpost at open, which in a fork of a process that opened one reads the parent's logging
    state (SIGSEGV in _os_log_preferences_refresh). So every call goes through a pipe to a thread here, on the one connection; a worker's batch is rolled back when it dies.
    Made before the fork; start() once every worker is forked; worker() in each worker gives its store."""
    OPS = ('get', 'get_raw', 'has', 'names', 'blob_ids', 'put', 'put_link', 'put_blob', 'delete_prefix')

    def __init__(self, store, n):
        ctx = multiprocessing.get_context('fork')
        self.store, self.pipes, self.taken = store, [ctx.Pipe() for _ in range(n)], ctx.Value('i', 0)

    def start(self):
        for here, there in self.pipes:
            there.close()   # so that a dead worker ends its thread's read
            threading.Thread(target=self._serve, args=(here,), daemon=True).start()

    def worker(self):
        with self.taken.get_lock():
            i = self.taken.value
            self.taken.value += 1
        for j, (here, there) in enumerate(self.pipes):
            here.close()
            if j != i: there.close()
        return Remote(self.pipes[i][1], self.store.v2)

    def _serve(self, conn):
        try:
            with contextlib.suppress(_Gone, EOFError, OSError): self._calls(conn, False)
        finally: self.store.forget()

    def _calls(self, conn, inside):
        s = self.store
        while True:
            try: op, args = conn.recv()
            except (EOFError, OSError):
                if inside: raise _Gone from None
                return
            if op == 'end' and inside: return
            if op == 'abort' and inside: raise _Abort
            try:
                if op == 'begin' and not inside:
                    try:
                        with s.batch():
                            conn.send((True, None, s.full))
                            self._calls(conn, True)
                    except _Abort: pass
                    value = None
                elif op in self.OPS: value = getattr(s, op)(*args)
                else: raise ValueError(f'shader cache: no call {op!r}')
            except _Gone: raise
            except Exception as e: conn.send((False, e, s.full))
            else: conn.send((True, value, s.full))


class Remote(_Files):
    """Hashes, compresses and decompresses in the worker: a blob the file holds is linked by its digest, without its bytes"""
    def __init__(self, conn, v2=True): self.conn, self.v2, self.lock, self.inside, self.full = conn, v2, threading.RLock(), False, False

    def _call(self, op, *args):
        with self.lock:
            self.conn.send((op, args))
            ok, value, self.full = self.conn.recv()
        if not ok: raise value
        return value

    def get(self, name):
        if (raw := self._call('get_raw', name)) is None: return None
        try: return _decode(*raw)
        except Exception as e:
            if _bad_frame(e): return self._call('get', name)   # the process that opened the file makes it anew, or drops the read-only one
            raise

    def has(self, name): return self._call('has', name)
    def names(self, prefix): return self._call('names', prefix)
    def blob_ids(self, prefix): return self._call('blob_ids', prefix)

    def put(self, name, data, when=None):
        if self.full: return
        digest = hashlib.blake2b(data, digest_size=16).digest()
        if self._call('put_link', name, digest, when): return
        codec, payload = _encode(data) if self.v2 else (RAW, data)
        self._call('put_blob', name, digest, codec, payload, when)

    def delete_prefix(self, prefix): self._call('delete_prefix', prefix)

    @contextlib.contextmanager
    def batch(self):
        with self.lock:
            if self.inside:
                yield
                return
            self._call('begin')
            self.inside = True
            try: yield
            except BaseException:
                self.inside = False
                self._call('abort')
                raise
            else:
                self.inside = False
                self._call('end')


def exists(path): return os.path.isfile(os.fspath(path))


def chain_file(folder, chain): return os.path.join(os.fspath(folder), f'cas-{chain}.db')


def for_chain(folder, chain, say=print, **kw):
    return Store(chain_file(folder, chain), say, older=generations(folder), **kw)


def _file_size(p): return sum(os.path.getsize(p + s) for s in ('', '-wal') if os.path.isfile(p + s))


def drop_generations(folder, chain, say=print, remove=None):
    """The build copied every Metal compile it reused, so the others are not needed; nothing goes without the chain's own file. remove: e.g. into obsolete/"""
    cur = chain_file(folder, chain)
    others = [p for p in generations(folder) if os.path.realpath(p) != os.path.realpath(cur)]
    if not others or not os.path.isfile(cur): return []
    gone, size = [], 0
    for p in others:
        n = _file_size(p)
        try:
            for s in ('', '-wal'):
                if os.path.exists(p + s): (remove or os.unlink)(p + s)
            for s in ('-shm', '-journal', '.lock'):
                with contextlib.suppress(FileNotFoundError): os.unlink(p + s)
        except OSError as e:
            say(f'[RED] could not remove the old shader compile cache {p}: {e}')
            continue
        gone.append(os.path.basename(p))
        size += n
    if gone: say(f'removed old shader compile caches {", ".join(gone)} ({size / 1e9:.2f} GB){" to obsolete/" if remove else ""}')
    return gone


def main():
    import sys
    if sys.argv[1:2] == ['--run']:
        # the cache format decides no shader output, so older builders take this reader without changing their chain identity
        import argparse, runpy
        ap = argparse.ArgumentParser(description='Run a shader builder with read-only fallback to another compile cache.')
        ap.add_argument('--readonly', required=True)
        ap.add_argument('script')
        ap.add_argument('args', nargs=argparse.REMAINDER)
        a = ap.parse_args(sys.argv[2:])
        global READ_ONLY
        READ_ONLY = a.readonly
        sys.modules['shader_cache'] = sys.modules[__name__]
        sys.path.insert(0, os.path.dirname(os.path.abspath(a.script)))
        sys.argv = [a.script, *a.args]
        runpy.run_path(a.script, run_name='__main__')
        return
    if len(sys.argv) != 2: sys.exit('usage: shader_cache.py <cas.db>: what it holds')
    s = Store(sys.argv[1])
    kinds = {}
    for name, in s._run('SELECT name FROM entry'): kinds[name.split('/', 1)[0]] = kinds.get(name.split('/', 1)[0], 0) + 1
    print(json.dumps({'entries': kinds, 'blobs': s._run('SELECT count(*) FROM blob')[0][0], 'bytes': os.path.getsize(sys.argv[1]), 'codec': 'zstd' if s.v2 else 'raw'}))
    s.close()


if __name__ == '__main__': main()
