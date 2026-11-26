#!/usr/bin/env python3
# Offline shader build: Switch shaders → SPIR-V (dmsh_xlate) → spirv-cross MSL → metallib + replay reflection JSON, into <output cache dir>/b-<chain version>.img.
# Usage: dmsh_build.py <Switch package dir> <output cache dir> [key...] (no keys: a full build, which also stamps the inputs and writes c1_full.tsv and cb_extent.tsv)
# Environment: DM_GAME (a derived profile), XLATE (the translator), DMSH_CAS (compile cache folder or file), DMSH_PREFILL (library roots, :-separated),
#   J (concurrency), DMSH_PROCS (processes that finish shaders, default J; 0 = threads)
import atexit, errno, inspect, multiprocessing, os, re, sys, json, mmap, struct, hashlib, shutil, subprocess, tempfile, time, collections, threading, concurrent.futures as cf
import dmsh_atomic, dmsh_wide, dmsh_meshstage, dmsh_rewrite, dmsh_ndsr, dmsh_reflect, dmsh_entry, dmsh_metalwa, dmsh_abi as abi
dm_dir, out = sys.argv[1:3]
want = set(sys.argv[3:])
cache_root = out
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import dmsh_chain, dmsh_fetch, dmsh_facts, dmsh_mtlcache, dmsh_pch, dmsh_vin, dmsh_pull, dmsh_side, dmsh_ampere, shader_regressions, shader_cache, tool_lock
_bad = tool_lock.check_tools()
if _bad: sys.exit('\n'.join('[RED] translation chain tools do not match the lock file: ' + b for b in _bad) + '\n(if the tools changed, run tool_lock.py --update-lock and commit the lock file; if the translator just was not rebuilt, rebuild it)')
CHAIN = dmsh_chain.version()
out = os.path.join(out, 'b-' + CHAIN)
GAME = dmsh_chain.load_game()
if f'"{CHAIN}"' not in open(dmsh_chain.HEADER).read(): sys.exit(f'[RED] {dmsh_chain.HEADER} does not hold the current translation chain version {CHAIN}: run dmsh_chain.py --header first and commit it (otherwise the host cannot find these libraries)')
INPUTS = dmsh_chain.inputs(GAME)
XLATE = os.environ.get('XLATE', os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate'))
SPIRV_CROSS = dmsh_chain.SPIRV_CROSS
FACTS = dmsh_facts.load()
if not os.path.exists(dmsh_facts.TABLE): sys.exit(f'[RED] no answer table {dmsh_facts.TABLE} (it comes with the game profile; an empty table = every query answered with defaults)')
import shader_lib, romfs
_g = shader_lib.read_stamp(out).get('game')   # one chain version serves every game: the stamp keeps two games apart
if _g and _g != GAME.id: sys.exit(f'[RED] {out} holds the shader libraries of game {_g}, not {GAME.id}: give each game its own output directory')
MAP = str(GAME.table('shader_map.tsv'))
BF_WANT_FILE = str(GAME.table('bf_want.tsv'))
BF_WANT = {line.strip() for line in open(BF_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(BF_WANT_FILE) else set()
DUAL_WANT_FILE = str(GAME.table('dual_want.tsv'))
DUAL_WANT = {line.split()[0] for line in open(DUAL_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(DUAL_WANT_FILE) else set()
XC = [*dmsh_chain.CROSS_MSL, '--msl-pad-fragment-output']

_cas = os.environ.get('DMSH_CAS') or cache_root
STORE: 'shader_cache.Store | shader_cache.Remote' = shader_cache.for_chain(_cas, CHAIN) if os.path.isdir(_cas) or _cas == cache_root else shader_cache.Store(_cas)
atexit.register(STORE.close)
CAS_LOCKS, CAS_LOCKS_GUARD = {}, threading.Lock()
PRE_STATS = collections.Counter()
MAC_RECORD = ' '.join(dmsh_chain.MAC_METAL) + '; xcrun -sdk macosx metallib'
# The tools xcrun names, found once (each xcrun launch looks them up again). -isysroot is what xcrun -sdk passes: without it the driver targets the running macOS
def xcrun_out(*args): return subprocess.run(['xcrun', '-sdk', 'macosx', *args], capture_output=True, text=True, check=True).stdout.strip()
MAC_RUN = [xcrun_out('-f', 'metal'), '-isysroot', xcrun_out('--show-sdk-path'), *dmsh_chain.MAC_METAL[4:]]
MAC_LINK = [xcrun_out('-f', 'metallib')]
# -fbracket-depth: some compute shaders nest 259 deep; a source within the default 256 compiles to the same bytes, so the records do not name it
METAL_DEPTH = ['-fbracket-depth=1024']
# The outer shader cache restores libraries before the MSL compile cache checks their compiler: override builds stay separate there too.
METAL_FP = dmsh_mtlcache.compiler_id('macosx')
def mac_compile(msl, air, lib):   # the precompiled header and METAL_DEPTH change no output, so MAC_RECORD does not name them
    dmsh_pch.compile(MAC_RUN + METAL_DEPTH, msl, air)
    subprocess.run([*MAC_LINK, air, '-o', lib], check=True)

def record_index(path):   # shader_split.py writes each record's key at +0x08, its bytes from its marker to the next one
    # a mapping closed again: a process holding the packages makes every fork (each compile) slow
    fd = os.open(path, os.O_RDONLY)
    n = os.fstat(fd).st_size
    if not n: return fd, {}
    with mmap.mmap(fd, n, access=mmap.ACCESS_READ) as m:
        offs = [x.start() for x in re.finditer(b'BTCDBTCD', m)] + [n]
        return fd, {m[o + 8:o + 16].hex(): (o, e) for o, e in zip(offs, offs[1:])}

def clone(src, dst):   # clonefile, not a cp: forks of this process are slow
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    dmsh_mtlcache.clone(src, dst)

def atomic_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    with open(tmp, 'w') as f: json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)

def source_without_rule_bodies():
    return dmsh_chain.code_form(open(__file__, 'r').read(), {f.__name__ for f, _ in RULES})

def shader_rule_source(name):
    return b''.join(inspect.getsource(f).encode() for f, applies in RULES if applies(name))

def tool_fingerprint():
    sc = subprocess.run([SPIRV_CROSS, '--version'], capture_output=True, text=True, timeout=30)
    return ('lock\0' + open(dmsh_chain.LOCK).read() + '\0options\0' + dmsh_chain.options() +
            '\0xlate\0' + dmsh_chain.xlate_source_sha() + '\0spirv-cross\0' + sc.stdout + sc.stderr +
            '\0xc\0' + repr(XC) + '\0mac\0' + MAC_RECORD + '\0metal\0' + METAL_FP)

PROFILE_ROWS = dmsh_chain.profile_rows(GAME)
def shader_key(key, name, rec):
    """Covers the profile's build keys and the shader's pairs; a paired vertex shader also takes its pixel shaders' keys, since vs_pad reads their MSL"""
    pc = dmsh_facts.row(key, FACTS[key]).encode() if key in FACTS else 'missing'.encode()
    map_row = MAP_ROWS.get(key, 'missing'.encode()) + MAP_CONFLICT_ROWS.get(key, b'')
    payload = (b'record\0' + rec + b'\0map\0' + map_row + b'\0pc\0' + pc +
               b'\0tools\0' + TOOL_FP + b'\0code\0' + CODE_FP.encode() +
               b'\0rule\0' + shader_rule_source(name) + b'\0profile\0' + PROFILE_ROWS + b'\0pairs\0' + ','.join(sorted(PAIRS.get(key, ()))).encode() +
               b'\0dual\0' + (b'1' if key in DUAL_WANT else b'0'))
    return hashlib.sha256(payload).hexdigest()
def pair_keyed(digest, pixel_digests):
    return hashlib.sha256((digest + '\0' + ','.join(pixel_digests)).encode()).hexdigest()

def cache_meta_path(key): return f'index/{key}.json'
def cache_path(digest): return f'shader/{digest}'
CAS_USED = '_b_cas.txt'   # the cache_path digests of the last full build, one per line

# Finished outputs are staged in the cache under this folder's name and leave the disk (commit); the next build of the folder clears a stopped one's stage
STAGE = 'stage/' + hashlib.sha256(os.path.realpath(out).encode()).hexdigest()[:16]
def output(name):
    """On disk while in flight, else staged; for a partial build, from the image it builds on. None when there is none"""
    try:
        with open(os.path.join(out, name), 'rb') as f: return f.read()
    except FileNotFoundError: pass
    data = STORE.get(f'{STAGE}/mac/{name}')
    if data is not None or not want: return data
    m = OUT_KEY.match(name)
    if OLD_IMAGE is None or not m or owned_output(m.group(1)): return None
    try: return (OLD_IMAGE / name).read_bytes()
    except OSError: return None

def output_of(name) -> bytes:
    data = output(name)
    if data is None: raise FileNotFoundError(f'{name}: no such output')
    return data

def commit(key='', names=None):
    """A cache that cannot take one rolls the whole batch back, and the files stay on disk, where publish_library finds them"""
    staged = []
    with STORE.batch():
        for n in (names if names is not None else [n for n in os.listdir(out) if n.startswith(key + '_')]):
            p = os.path.join(out, n)
            if n.startswith('.') or '.tmp' in n or not os.path.isfile(p): continue
            STORE.put_file(f'{STAGE}/mac/{n}', p)
            staged.append(p)
    if not STORE.full:
        for p in staged: os.unlink(p)

def publish_library(on_progress=None):   # on_progress: shader_lib.write's
    """Files left on disk win over staged ones. When the image cannot be written, a full build out of space stops without unpacking onto the full drive; otherwise
    the staged outputs go back into the folder"""
    d = out
    image = shader_lib.image_of(d)
    prefix = f'{STAGE}/mac/'
    files, share = {}, {}   # share: the outputs of one cache blob, or of one range of the old image, written once (shader_lib.write)
    if OLD_IMAGE is not None:
        old = romfs.Image(OLD_IMAGE)
        for name, k in old.children(0):
            m = OUT_KEY.match(name)
            if (m and owned_output(m.group(1))) or (name == dmsh_chain.STAMP and STAMP_BEFORE != INPUTS): continue
            files[name] = str(OLD_IMAGE / name)
            share[name] = ('old', *old.entry(k)[:2])
    staged = STORE.blob_ids(prefix)
    for n, blob in staged.items():
        files[n[len(prefix):]] = (lambda n=n: STORE.get(n))
        share[n[len(prefix):]] = ('blob', blob)
    for n in os.listdir(d):
        if not n.startswith('.') and '.tmp' not in n and os.path.isfile(os.path.join(d, n)):
            files[n] = os.path.join(d, n)
            share.pop(n, None)
    try: shader_lib.write(files, image, on_progress=on_progress, share=share)
    except (OSError, ValueError) as e:
        cache_full = getattr(STORE, 'full', False)
        def stop(why, err2=None):
            if not want:
                try: os.unlink(os.path.join(d, dmsh_chain.STAMP))
                except OSError: pass
            hint = ('The shader cache filled up earlier in this build, so only the shaders cached before that will be reused' if cache_full
                    else 'The shaders compiled so far are in the shader cache, and a build again in this work folder takes them from there')
            action = 'free space on the drive and build again' if errno.ENOSPC in (getattr(e, 'errno', None), getattr(err2, 'errno', None)) else 'fix the error and build again'
            sys.exit(f'[RED] {why}. {hint}: {action}')
        if isinstance(e, OSError) and e.errno == errno.ENOSPC and OLD_IMAGE is None and not cache_full and staged:
            stop(f'could not write {image}: {e}')
        written = []
        try:
            for n in staged:
                dst = os.path.join(d, n[len(prefix):])
                if not os.path.exists(dst) and STORE.to_file(n, dst): written.append(dst)
        except OSError as e2:
            for p in written:
                try: os.unlink(p)
                except OSError: pass
            stop(f'could not write {image} ({e}) or restore the shaders into {d} ({e2})', e2)
        print(f'[RED] could not write {image}, the library stays the folder {d}: {e}', flush=True)
        return
    STORE.delete_prefix(prefix)
    shutil.rmtree(d, ignore_errors=True)

def cache_outputs(key, digest, cached=False):
    root = cache_path(digest)
    required = [key + '_b.json', key + '_b.metal', key + '_b.metallib']
    have = STORE.names(f'{root}/mac/') if cached else set()
    if not all(f'{root}/mac/{n}' in have if cached else os.path.exists(os.path.join(out, n)) for n in required): return None
    return root, required

def restore_cache(key, digest):
    found = cache_outputs(key, digest, cached=True)
    if not found: return False
    root, names = found
    for name in names:
        src = f'{root}/mac/{name}'
        dst = os.path.join(out, name)
        if name.endswith('.json'):
            data = STORE.get(src)
            if data is None: return False
            tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
            with open(tmp, 'wb') as f: f.write(re.sub(rb'("chain"\s*:\s*")[^"]*"', rb'\g<1>' + CHAIN.encode() + b'"', data, count=1))
            os.replace(tmp, dst)
        elif not STORE.to_file(src, dst): return False
    return True

def cache_lookup(key, digest, rule_fp, code_fp):
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('shader', key), threading.Lock())
    with lock:
        if cache_outputs(key, digest, cached=True):
            PRE_STATS['hit'] += 1
            return True, 'hit'
        old = json.loads(STORE.get(cache_meta_path(key)) or b'null')
        if old and old.get('code_fp') != code_fp: reason = 'code fingerprint changed: all rebuilt'
        elif old and old.get('rule_fp') != rule_fp: reason = 'rule changed'
        else: reason = 'new key'
        PRE_STATS['miss'] += 1
        PRE_STATS[reason] += 1
        return False, reason

def publish_cache(key, digest, rule_fp, code_fp):
    # A build without the paired pixel MSL must not cache an incomplete vertex shader under a complete key.
    if any(output(ps + '_b.metal') is None for ps in PAIRS.get(key, ())): return
    found = cache_outputs(key, digest)
    if not found: return
    root, names = found
    with STORE.batch():   # a stop leaves the whole entry or none of it
        for name in names:
            src = os.path.join(out, name)
            if name.endswith('.json'):
                value = json.load(open(src))
                for field in ('fetch', 'vin', 'pull', 'alpha', 'logic', 'chain_metal_cut'):
                    value.pop(field, None)   # fin regenerates the side libraries
                STORE.put(f'{root}/mac/{name}', json.dumps(value, ensure_ascii=False, indent=1).encode())
            else: STORE.put_file(f'{root}/mac/{name}', src)
        STORE.put(cache_meta_path(key), json.dumps({'digest': digest, 'rule_fp': rule_fp, 'code_fp': code_fp}).encode())

def write_refl(key, refl, cut=False):
    recorded = MTL.with_compiler('mac', refl)
    if cut: recorded['chain_metal_cut'] = True
    atomic_json(os.path.join(out, key + '_b.json'), recorded)

VIN_STATS = collections.Counter()
PULL_STATS = collections.Counter()
SIDE_STATS = collections.Counter()
BF_STATS = collections.Counter()

def pull_finish(key, full, refl):
    """<key>_bp of a vertex shader with stage inputs (dmsh_pull.py); none without them or when the variant does not fit. Changes refl; the caller writes it"""
    if refl['stage'] != 'vs': return refl
    lib = os.path.join(out, key + '_bp.metallib')
    refl.pop('pull', None)
    def drop():
        try: os.unlink(lib)
        except FileNotFoundError: pass
    v, info = dmsh_pull.variant(full)
    if not v:
        PULL_STATS['no stage input' if info == 'no stage input' else 'not generated'] += 1
        if info != 'no stage input': print(f'[RED] {key}: no pulling library ({info}): its pipelines are always built for the draw\'s vertex layout', flush=True)
        drop()
        return refl
    msl = os.path.join(w, key + '_bp.metal')
    open(msl, 'w').write(v)
    try:
        MTL.compile('mac', msl, lib)
    except subprocess.CalledProcessError as e:
        PULL_STATS['failed'] += 1
        print(f'[RED] {key}: the pulling library does not compile ({str(e)[:120]}): its pipelines are always built for the draw\'s vertex layout', flush=True)
        drop()
        return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    PULL_STATS['generated'] += 1
    refl['pull'] = {k: v_ for k, v_ in info.items() if v_ is not None}
    return refl

def vin_finish(key, full, refl):
    """<key>_bv of a vertex shader with integer inputs (dmsh_vin.py). Changes refl; the caller writes it"""
    if refl['stage'] != 'vs': return refl
    lib = os.path.join(out, key + '_bv.metallib')
    refl.pop('vin', None)
    def drop():
        try: os.unlink(lib)
        except FileNotFoundError: pass
    v, shadows = dmsh_vin.variant(full)
    if not v:
        if shadows != 'no integer input':
            VIN_STATS['not generated'] += 1
            print(f'[RED] {key}: no shadow-input library ({shadows}): its integer inputs keep the CPU conversion', flush=True)
        drop()
        return refl
    VIN_STATS['generated'] += 1
    msl = os.path.join(w, key + '_bv.metal')
    open(msl, 'w').write(v)
    try:
        MTL.compile('mac', msl, lib)
    except subprocess.CalledProcessError as e:
        VIN_STATS['failed'] += 1
        print(f'[RED] {key}: the shadow-input library does not compile ({str(e)[:120]}): its integer inputs keep the CPU conversion', flush=True)
        drop()
        return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    refl['vin'] = {'entry': 'main0', 'entry_z': 'main0_z', 'shadow': shadows}
    VIN_STATS['inputs'] += len(shadows)
    VIN_STATS['float only'] += sum(1 for s in shadows if 'sint' not in s and 'uint' not in s)
    return refl

def vs_libraries(key, msl, full):
    """<key>_b, _bp and _bv cut from one compile (dmsh_side.vs_unit); three compiles, with a red line, when the unit does not build. → ('pull', 'vin' reflection, cut).
    Raises CalledProcessError when the plain source does not compile."""
    cut = False
    pv, pinfo = dmsh_pull.variant(full)
    why = None
    if pv and isinstance(pinfo, dict):
        vv, shadows = dmsh_vin.variant(full)
        try:
            whole = dmsh_side.vs_unit(full, pv, vv)
            src, pn, vn = whole
            names = ['_b'] + (['_bp'] if pn else []) + (['_bv'] if vn else [])
            groups = [set(n.values()) for n in (pn, vn) if n]
            u = os.path.join(w, f'{key}_bu_mac.metal')
            open(u, 'w').write(src)
            try: MTL.compile_split('mac', u, [os.path.join(out, key + n + '.metallib') for n in names], lambda lib, groups=groups: dmsh_side.cut(lib, *groups))
            finally: os.unlink(u)
            cut = True
        except (subprocess.CalledProcessError, ValueError) as e: why = str(e)[:120]
        else:
            PULL_STATS['generated'] += 1
            PULL_STATS['unit'] += cut
            refl = {'stage': 'vs', 'pull': {k: v for k, v in {**pinfo, **whole[1]}.items() if v is not None}}
            if not vv: vin_finish(key, full, refl)
            else:
                refl['vin'] = {'entry': 'main0', 'entry_z': 'main0_z', **whole[2], 'shadow': shadows}
                VIN_STATS['generated'] += 1
                VIN_STATS['inputs'] += len(shadows)
                VIN_STATS['float only'] += sum(1 for s in shadows if 'sint' not in s and 'uint' not in s)
            return {k: refl[k] for k in ('pull', 'vin') if k in refl}, cut
    MTL.compile('mac', msl, os.path.join(out, key + '_b.metallib'))
    if why:   # a source that does not compile at all is the caller's failure, not the unit's
        PULL_STATS['unit failed'] += 1
        print(f'[RED] {key}: its libraries do not build from one compile ({why}): each is compiled from its own source', flush=True)
    refl = pull_finish(key, full, vin_finish(key, full, {'stage': 'vs'}))
    return {k: refl[k] for k in ('pull', 'vin') if k in refl}, False

def ps_libraries(key, msl, full, bindings):
    """<key>_b, _bx (alpha test, logic op, main0_u) and _bf (readable tile, main0_f) cut from one compile (dmsh_side.cut). What does not build is left out with a
    red line, its stale library removed. → the reflection's view. Raises CalledProcessError when the plain source does not compile"""
    def drop(*suffixes):
        for s in suffixes:
            try: os.unlink(os.path.join(out, key + s + '.metallib'))
            except FileNotFoundError: pass
    src, info = dmsh_side.unit(full)
    typed = {'main0_u'} if re.search(r'\bfragment\s+main0_out_u\s+main0_u\s*\(', full) else set()
    side = (set(info['alpha'] + info['logic']) if src else set()) | typed
    folded, fmeta = dmsh_fetch.fold(src or full, bindings, om_only_ok=key in BF_WANT)
    for unit, fetch in ([(folded, fmeta)] if folded else []) + ([(src or full, None)] if src or typed else []):
        names = ['_b'] + (['_bx'] if side else []) + (['_bf'] if fetch else [])
        groups = ([side] if side else []) + ([{'main0_f'}] if fetch else [])
        u = os.path.join(w, key + '_bu.metal')
        open(u, 'w').write(unit)
        try:
            MTL.compile_split('mac', u, [os.path.join(out, key + n + '.metallib') for n in names], lambda lib, groups=groups: dmsh_side.cut(lib, *groups))
        except (subprocess.CalledProcessError, ValueError) as e:
            if fetch:
                BF_STATS['failed'] += 1
                print(f'[RED] {key}: the readable-tile version does not build ({str(e)[:120]}): its draws go without the tile read', flush=True)
            else:
                SIDE_STATS['failed'] += 1
                print(f'[RED] {key}: the side library (alpha test, logic operation, typed outputs) does not build ({str(e)[:120]}): its draws go without the alpha test and logic operation, its typed outputs come from <key>_b', flush=True)
            continue
        finally:
            try: os.unlink(u)
            except FileNotFoundError: pass
        drop(*{'_bx', '_bf'} - set(names))
        SIDE_STATS['generated'] += bool(side - typed)
        BF_STATS['generated'] += bool(fetch)
        return {**{f: {'entries': info[f]} for f in ('alpha', 'logic') if src and side and info[f]}, **({'fetch': fetch} if fetch else {}), **({'entry_u_side': True} if typed else {})}
    SIDE_STATS['not generated'] += not src
    drop('_bx', '_bf')
    MTL.compile('mac', msl, os.path.join(out, key + '_b.metallib'))
    return {}

pc_of, loc = {}, {}
# shader_map.tsv column 3 → stage and package prefix. The game has only these five, all NVN2: anything else stops the build (check_record)
STAGES = {'vertex': 'vs', 'pixel': 'ps', 'compute': 'cs', 'task': 'ts', 'mesh': 'ms'}
MAP_ROWS, MAP_CONFLICT_ROWS = {}, {}
for raw in open(MAP, 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 4: MAP_ROWS.setdefault(c[3].decode(), raw)
for raw in open(MAP + '.conflicts.tsv', 'rb'):
    c = raw.rstrip(b'\n').split(b'\t')
    if len(c) > 1 and not raw.startswith((b'#', b'dm_')): MAP_CONFLICT_ROWS.setdefault(c[0].decode(), raw)
for ln in open(MAP):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 4 and c[0].endswith('.bin'):
        if STAGES.get(c[2]) != c[0][:2]:
            sys.exit(f'[RED] {MAP}: shader {c[3]} is a {c[2]} shader in package {c[0]}: this chain translates only the vertex, pixel, compute, task and mesh shaders of an NVN2 game '
                     f'(each in the package of its stage), as Ocarina of Time has no other')
        pc_of.setdefault(c[3], c[4])
        loc.setdefault(c[3], (c[0], int(c[1])))
for ln in open(MAP + '.conflicts.tsv'):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 1 and not ln.startswith('#') and not ln.startswith('dm_') and c[0] in pc_of: pc_of[c[0]] = c[1]

def check_record(key, name, rec):
    """Stops at a record this chain does not translate: not NVN2, a ray tracing module (control stage 8), or a stage not its package's (control +0x56: 5 compute,
    6 task, 7 mesh; graphics: SPH word 0 bits 10..13, 1 vertex, 5 pixel). rec_of calls it for every record"""
    ci = rec.find(bytes.fromhex('34127698'), 0x38)
    ctl = rec[ci:] if ci >= 0 else b''
    code = rec[0x38:ci] if ci >= 0 else b''
    if not dmsh_ampere.is_nvn2(ctl):
        sys.exit(f'[RED] {name}: shader {key} has no NVN2 control section (magic 0x98761234, version 0xc9): this chain translates only NVN2 (Switch 2, Ampere) shaders, '
                 f'and an NVN1 (Maxwell) record is not skipped but stops the build')
    kind = ctl[0x56] if len(ctl) > 0x56 else None
    if kind == 8: sys.exit(f'[RED] {name}: shader {key} is an NVN2 ray tracing module (control stage 8): this chain has no ray tracing path, as the game has no such shader')
    stage = name[:2]
    sph = struct.unpack_from('<I', code)[0] >> 10 & 0xf if len(code) >= 4 else None
    if (kind != {'cs': 5, 'ts': 6, 'ms': 7}[stage]) if stage in ('cs', 'ts', 'ms') else (kind in (5, 6, 7) or sph != {'vs': 1, 'ps': 5}.get(stage)):
        sys.exit(f'[RED] {name}: shader {key} is not a {stage} shader (control stage byte {kind}, SPH shader type {sph}): this chain translates only vertex, pixel, compute, '
                 f'task and mesh shaders, each from the package of its stage')

def early_fragment_tests(ctl, code):
    """Control +0x5b: the console tests such a shader against depth before it runs, Metal after one that stores to memory, which breaks EQUAL passes. A shader
    whose SPH writes depth (word 19 bit 1) stays late"""
    return dmsh_ampere.early_fragment_tests(ctl) and len(code) >= 0x50 and not struct.unpack_from('<I', code, 19 * 4)[0] & 2

def ps_omap(code):
    """SPH word 18: 4 bits per colour output, R G B A from the low bit. The GPU writes only these components; Metal writes the whole attachment, so the host masks
    each target (without it, a band of NaN above the horizon)"""
    return struct.unpack_from('<I', code, 18 * 4)[0] if len(code) >= 0x50 else None

NVN2_STAGE_INDEX = {'vs': 0, 'ms': 1, 'ps': 4}

def job_for(key, name, rec, w):
    ci = rec.find(bytes.fromhex('34127698'), 0x38)
    code = rec[0x38:ci]
    stage = name[:2]
    extra = {}
    c1 = dmsh_ampere.c1_segment(rec[ci:], code)
    code = code[:dmsh_ampere.inst_len(rec[ci:])]
    extra['arch'] = 'sm86'
    # compute's c[0] layout from its control section: push words +0xa9, push block 1 words +0xaa, ubuf / sbuf / texture / separate texture / sampler
    # counts +0xb1..+0xb5 (images +0xb6, last), size +0xba; the translator maps them (InstEmitAmpere.cs AMapC0)
    if stage == 'cs': extra['nvn2_cs'] = [rec[ci + 0xb1], rec[ci + 0xb2], rec[ci + 0xb3], struct.unpack_from('<H', rec, ci + 0xba)[0], rec[ci + 0xa9] * 4, rec[ci + 0xb4], rec[ci + 0xb5], rec[ci + 0xaa] * 4]
    # a graphics stage reads push block 1 + stage index from a bank of its own, c[25 + stage index]
    if stage in NVN2_STAGE_INDEX: extra['stage_bank'] = abi.NVN2_STAGE_BANK + NVN2_STAGE_INDEX[stage]
    if stage in ('ts', 'ms'):
        # task / mesh: translated as compute, then wrapped as the Metal stage function (dmsh_meshstage.py)
        mesh = dmsh_meshstage.info(rec[ci:], rec[0x38:ci])
        lay, size = dmsh_meshstage.memory(mesh)
        if dmsh_meshstage.own_shared(code[0x80:]) or size > dmsh_meshstage.METAL_MAX_SHARED: mesh['refused'] = 'shared memory of its own, or stage memory past Metal\'s limit'
        extra.update(mesh_stage=mesh, stage_memory=lay, local=[mesh['threads'], 1, 1], shared=size)
        code = code[0x80:]
        stage = 'cs'
    if stage == 'ps' and early_fragment_tests(rec[ci:], code): extra['early_fragment_tests'] = True
    cp = os.path.join(w, key + '.bin')
    open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), **extra,
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # relaxed precision by default; c1 holds the indirect jump tables
    if extra.get('mesh_stage'): return j, c1
    if key not in FACTS:   # queries use defaults
        j['no_facts'] = True
        return j, c1
    j.update(FACTS[key])
    return j, c1

RULES = []
# CODE_FP: the code form (no comments, docstrings, layout) of this file and of the modules that run only on a cache miss; test_dmsh_code_fp.py checks the list.
CODE_FP = hashlib.sha256(source_without_rule_bodies() + b''.join(b'\0' + dmsh_chain.code_form(open(m.__file__, 'r').read()) for m in (dmsh_chain, dmsh_atomic, dmsh_wide, dmsh_meshstage, dmsh_rewrite, dmsh_ndsr, dmsh_reflect, dmsh_entry, dmsh_ampere, dmsh_side, dmsh_metalwa, abi))
                         + b'\0' + inspect.getsource(dmsh_fetch.screen_varyings).encode()
                         + b'\0abi\0' + repr(sorted(abi.VALUES.items())).encode()).hexdigest()
TOOL_FP = tool_fingerprint().encode()

# Both pair tables feed pipelines, so both feed padding, build order and cache keys.
def load_pairs(game, package):
    pairs = collections.defaultdict(set)
    for table in ('shader_pairs.tsv', 'pso_pairs.tsv'):
        path = str(game.table(table))
        if table == 'pso_pairs.tsv' and not os.path.isfile(path):
            path = os.path.join(os.path.dirname(os.path.abspath(package)), table)
        if table == 'pso_pairs.tsv' and not os.path.isfile(path): continue
        with open(path) as f:
            for ln in f:
                if ln.strip() and not ln.startswith('#'):
                    v, p = ln.split()
                    pairs[v].add(p)
    return pairs

PAIRS = load_pairs(GAME, dm_dir)

def build_keys(want, locations, pairs):
    """A partial vertex build needs its available paired pixel shaders before padding; a full build keeps every shader."""
    return sorted(set(want) | {ps for vs in want for ps in pairs.get(vs, ()) if ps in locations}) if want else sorted(locations)

def partner_inputs(key):
    """pairs whose pixel MSL cannot be read are skipped"""
    want = set()
    for ps in PAIRS.get(key, ()):
        data = output(ps + '_b.metal')   # finished before this shader (fin's phases), so staged by now
        if data is not None: want |= {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', data.decode())}
    return want

def partner_reads(key):
    """{location: component mask} from the partners' SPH input maps; None without pairs. A location a partner's MSL declares but its map does not name counts as all four."""
    if not PAIRS.get(key): return None
    reads = collections.defaultdict(int)
    for ps in PAIRS[key]:
        got = {}
        if ps in loc:
            rec = rec_of(ps)[1]
            ci = rec.find(bytes.fromhex('34127698'), 0x38)
            code = rec[0x38:ci]
            got = dmsh_meshstage.pixel_reads(code[:0x80])
        data = output(ps + '_b.metal')
        for n in ({int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', data.decode())} if data is not None else set()) - got.keys(): got[n] = 0xf
        for n, m in got.items(): reads[n] |= m
    return dict(reads)

def vs_pad(key, src):
    """Outputs the paired pixel shaders read but the vertex shader does not write get (0,0,0,1), as Maxwell's untyped slots do; Metal does not link a pixel
    input that is no vertex output. → (source, padded locations)"""
    miss = sorted(partner_inputs(key) - {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', src)})
    i = src.find('struct main0_out\n{\n')
    j = src.find('\n};', i)
    k = src.find('main0_out out = {};')
    if not miss or i < 0 or j < 0 or k < 0: return src, []
    src = src[:j] + ''.join(f'\n    float4 dm_pad{l} [[user(locn{l})]];' for l in miss) + src[j:]
    k = src.find('main0_out out = {};') + len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.dm_pad{l} = float4(0.0, 0.0, 0.0, 1.0);' for l in miss) + src[k:], miss

SG_OPS = {333: 'true', 334: 'pred', 335: 'pred', 336: 'true', 337: 'value', 338: 'value', 339: 'ballot', 345: 'value', 346: 'value', 347: 'value', 348: 'value'}   # OpGroupNonUniform* → its one-lane value
SG_BUILTINS = {36: 'one', 41: 'zero', 4416: 'one4', 4417: 'one4', 4418: 'zero4', 4419: 'one4', 4420: 'zero4'}   # SubgroupSize, SubgroupLocalInvocationId, Eq / Ge / Gt / Le / Lt masks
def spv_single_lane(path):
    """Metal has no SIMD-group functions in vertex functions: the SPIR-V's subgroup operations become a one-lane subgroup's, which is exact. → True when rewritten"""
    with open(path, 'rb') as f: d = f.read()
    words = list(struct.unpack(f'<{len(d) // 4}I', d))
    hdr, ins, i = words[:5], [], 5
    while i < len(words):
        n = words[i] >> 16
        ins.append(words[i:i + n])
        i += n
    op = lambda x: x[0] & 0xffff
    if not any(op(x) in SG_OPS or (op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS) for x in ins): return False
    if any(340 <= op(x) <= 366 and op(x) not in SG_OPS for x in ins): return False   # bit counts, arithmetic: not covered
    bvar = {x[1]: SG_BUILTINS[x[3]] for x in ins if op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS}
    # A mask component loaded through an input pointer (a prefix ballot) keeps its index and extracts from the constant vector at the load.
    bptr = {}
    for x in ins:
        if op(x) in (65, 66) and len(x) > 3 and x[3] in bvar:
            if len(x) != 5 or not bvar[x[3]].endswith('4'): return False
            bptr[x[2]] = (bvar[x[3]], x[4])
    if any(op(x) in (65, 66) and len(x) > 3 and x[3] in bptr for x in ins): return False
    uint = next((x[1] for x in ins if op(x) == 21 and x[2:4] == [32, 0]), None)
    boolt = next((x[1] for x in ins if op(x) == 20), None)
    uvec4 = next((x[1] for x in ins if op(x) == 23 and x[2] == uint and x[3] == 4), None)
    # a type the module lacks is declared with the constants
    bound = hdr[3]
    c = {}
    types = []
    if uint is None:
        uint = bound
        bound += 1
        types.append([21 | 4 << 16, uint, 32, 0])
    if boolt is None:
        boolt = bound
        bound += 1
        types.append([20 | 2 << 16, boolt])
    if uvec4 is None:
        uvec4 = bound
        bound += 1
        types.append([23 | 4 << 16, uvec4, uint, 4])
    for nm in ('zero', 'one', 'zero4', 'one4', 'true'):
        c[nm] = bound
        bound += 1
    consts = types + [[43 | 4 << 16, uint, c['zero'], 0], [43 | 4 << 16, uint, c['one'], 1], [44 | 7 << 16, uvec4, c['zero4'], c['zero'], c['zero'], c['zero'], c['zero']],
              [44 | 7 << 16, uvec4, c['one4'], c['one'], c['zero'], c['zero'], c['zero']], [41 | 3 << 16, boolt, c['true']]]
    out, placed = [], False
    for x in ins:
        o = op(x)
        if o == 17 and x[1] in (61, 62, 63, 64, 65, 66, 67, 68, 4423, 4431): continue   # GroupNonUniform* / SubgroupBallotKHR / SubgroupVoteKHR capabilities
        if o == 10 and (b'ballot' in struct.pack(f'<{len(x) - 1}I', *x[1:]) or b'subgroup' in struct.pack(f'<{len(x) - 1}I', *x[1:])): continue
        if o in (71, 5) and x[1] in bvar: continue   # the builtin's decoration and name
        if o == 59 and x[2] in bvar: continue        # the builtin variable
        if o == 15: x = _ep_drop(x, bvar)
        if o == 54 and not placed:
            out += consts
            placed = True
        if o == 61 and x[3] in bvar:   # a load of a builtin → its constant
            out.append([83 | 4 << 16, x[1], x[2], c[bvar[x[3]]]])
            continue
        if o in (65, 66) and x[2] in bptr: continue
        if o == 61 and x[3] in bptr:
            nm, index = bptr[x[3]]
            out.append([77 | 5 << 16, x[1], x[2], c[nm], index])   # OpVectorExtractDynamic
            continue
        if o in SG_OPS:
            kind = SG_OPS[o]
            if kind == 'ballot':
                out.append([169 | 6 << 16, x[1], x[2], x[4], c['one4'], c['zero4']])   # OpSelect(pred, own bit, 0)
                continue
            out.append([83 | 4 << 16, x[1], x[2], c['true'] if kind == 'true' else x[4]])
            continue
        out.append(x)
    if not placed: return False
    hdr[3] = bound
    flat = hdr + [v for x in out for v in x]
    with open(path, 'wb') as f: f.write(struct.pack(f'<{len(flat)}I', *flat))
    return True
def _ep_drop(x, ids):   # OpEntryPoint: model, function, nul-terminated name in whole words, then the interface list
    k = 3
    while k < len(x):
        k += 1
        if any((x[k - 1] >> (8 * b)) & 0xff == 0 for b in range(4)): break
    y = x[:k] + [v for v in x[k:] if v not in ids]
    y[0] = len(y) << 16 | (x[0] & 0xffff)
    return y

def finish(key, w, j, c1):
    name, k = loc[key]
    msl = os.path.join(w, key + '.metal')
    lib = os.path.join(out, key + '_b.metallib')
    if j['stage'] == 'vs': spv_single_lane(os.path.join(w, key + '.spv'))
    r = subprocess.run([SPIRV_CROSS, *XC, os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
    if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
    src = open(msl).read()
    if j['stage'] == 'vs':   # second pass: inputs used only as integers reported as uint
        extra = {l: t for l, t in dmsh_entry.int_inputs(src).items() if l not in j['attr']}
        if extra:
            j['attr'].update(extra)
            jp = os.path.join(w, f'job1_{key}.json')
            json.dump([j], open(jp, 'w'))
            rx = subprocess.run([XLATE, jp, w], capture_output=True, text=True)
            if rx.returncode or not os.path.exists(os.path.join(w, key + '.spv')): return 'translation failed', ('integer-input retranslation: ' + rx.stderr.strip())[:160], None
            spv_single_lane(os.path.join(w, key + '.spv'))
            r = subprocess.run([SPIRV_CROSS, *XC, os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
            if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
            src = open(msl).read()
    try: src = dmsh_atomic.check_msl(src)
    except ValueError as e: return 'image atomic MSL failed', str(e), None
    if j['stage'] == 'ps':
        try: src = dmsh_rewrite.per_vertex(src)
        except ValueError as e: return 'per-vertex input rewrite failed', str(e)[:160], None
    src = dmsh_rewrite.precise(src)
    src = dmsh_rewrite.bit0_mask(src)
    try: src = dmsh_rewrite.select_guards(src)
    except ValueError as e: return 'uniform bounds rewrite failed', str(e)[:160], None
    src = dmsh_rewrite.mul_extended(src)
    try: src = dmsh_rewrite.float_add_msl(src)
    except ValueError as e: return 'float atomic add rewrite failed', str(e)[:160], None
    try: src = dmsh_rewrite.gather_offsets(src)
    except ValueError as e: return 'gather offsets rewrite failed', str(e)[:160], None
    try: src = dmsh_rewrite.hmma_msl(src)
    except ValueError as e: return 'HMMA rewrite failed', str(e)[:160], None
    try: src = dmsh_rewrite.shared_store_msl(src)
    except ValueError as e: return 'shared store rewrite failed', str(e)[:160], None
    try: src = dmsh_reflect.pool_arrays(src)
    except ValueError as e: return 'pool rewrite failed', str(e)[:160], None
    src = dmsh_wide.wide_loads(src)
    res = json.load(open(os.path.join(w, key + '.res.json')))
    # The MSL s<n> number indexes the buffers in binding order, as dmsh_reflect.reflect does.
    read_only = {i for i, b in enumerate(sorted(res['sbufs'], key=lambda b: b['binding']))
                 if not b['flags'] & 1 and b['cb_slot'] != abi.GMEM_MAP_CB_SLOT}   # BufferUsageFlags.Write (includes atomics)
    src = dmsh_wide.constant_reads(src, read_only, dmsh_chain.storage_constant_mode())
    src = dmsh_rewrite.tg_fence(src, kernel=j['stage'] == 'cs' and not j.get('mesh_stage'))
    src = dmsh_rewrite.border_emu(src)
    if j['stage'] == 'ps': src = dmsh_rewrite.frag_z_snap(src)[0]
    try: src = dmsh_rewrite.uniform_tests(src)
    except ValueError as e: return 'uniform tests rewrite failed', str(e)[:160], None
    blend0 = 0
    if j['stage'] == 'ps': src, blend0 = dmsh_rewrite.blend_zero(src)   # before the entry points that share main0's body
    open(msl, 'w').write(src)
    if j['stage'] != 'vs':   # Apple GPU compiler workarounds (KK3 ballots behind DMSH_MWA_FC, KK9 loop breaks), before the mesh stage, compute_loops and the side libraries
        try: t = dmsh_metalwa.apply(src)[0]
        except ValueError as e: return 'metal workaround rewrite failed', str(e)[:160], None
        if t != src:
            src = t
            open(msl, 'w').write(src)
    first, spill, loop_sites, ndsr = [], 0, [], None
    mesh = j.get('mesh_stage')
    if mesh:
        if mesh.get('refused'): return 'mesh stage failed', mesh['refused'], None
        # outputs its paired pixel shaders read but its SPHs do not enable get (0,0,0,1), each as wide as they read
        try:
            src = dmsh_meshstage.to_stage(src, mesh, partner_reads(key))
            open(msl, 'w').write(src)
        except ValueError as e: return 'mesh stage failed', str(e)[:160], None
    elif j['stage'] == 'cs':
        src, first = dmsh_rewrite.first_store(src, j.get('local'))
        open(msl, 'w').write(src)
        try: src, spill = dmsh_rewrite.tg_spill(src)
        except ValueError as e: return 'shared memory rewrite failed', str(e)[:160], None
        if spill: open(msl, 'w').write(src)
        try:
            src, loop_sites = dmsh_rewrite.compute_loops(src)
            open(msl, 'w').write(src)
        except ValueError as e: return 'compute loop containment failed', str(e)[:160], None
        src = dmsh_rewrite.max_threads(src, j.get('local'))
        open(msl, 'w').write(src)
        src, ndsr = dmsh_ndsr.network(src, j.get('local'), spill)   # last
        if ndsr and 'split' in ndsr: open(msl, 'w').write(src)
    if 'dmsh_raw_find' in src:
        src = dmsh_rewrite.raw_noinline(src, mesh['kind'] if mesh else j['stage'])
        open(msl, 'w').write(src)
    if 'spvSubgroupShuffle(' in src:
        folded = dmsh_rewrite.shuffle_literal(src)
        if folded != src:
            src = folded
            open(msl, 'w').write(src)
    pad = []
    if j['stage'] == 'vs':
        # No vertex shader of the game writes the layer, so there is no layered vertex path: one that did fails here rather than draw every layer into the first
        if dmsh_entry.writes_layer(src): return 'layer output', 'the vertex shader writes the layer (render_target_array_index), which this chain and the host have no path for', None
        src = dmsh_entry.vs_base_instance(dmsh_entry.vs_init(src))
        open(msl, 'w').write(src)
    if j['stage'] == 'vs' and PAIRS.get(key):
        src, pad = vs_pad(key, src)
        if pad: open(msl, 'w').write(src)
    discard = False
    if j['stage'] == 'vs':   # the analysis below uses the source with only main0
        src, full = dmsh_entry.vs_depth(src)
        discard = dmsh_entry.vs_writes(src, dmsh_reflect.reflect(key, 'vs', src, res))
        if discard: full += dmsh_entry.vs_discard(src)
        open(msl, 'w').write(full)
    fu = dmsh_entry.fs_uint(src) if j['stage'] == 'ps' else None
    fd = dmsh_entry.fs_dual(src) if j['stage'] == 'ps' and key in DUAL_WANT else None
    shared = dmsh_side.shared_body(src) if fu or fd else None
    if shared:
        plain = src
        open(msl, 'w').write(shared + (fu or '') + (fd or ''))
    else: fu = fd = None
    try: binds = dmsh_reflect.reflect(key, mesh['kind'] if mesh else j['stage'], src, res)   # before the compile: the readable-tile version needs the texture slots
    except ValueError as e: return 'reflection failed', str(e)[:160], None
    lost = []
    def compile_libraries():
        if j['stage'] == 'ps': return ps_libraries(key, msl, open(msl).read(), binds), False
        if j['stage'] == 'vs': return vs_libraries(key, msl, open(msl).read())
        MTL.compile('mac', msl, lib)
        return {}, False
    try: side, cut = compile_libraries()
    except subprocess.CalledProcessError as e:
        if not fu and not fd: return 'metal failed', str(e)[:160], None
        lost = [name for name, extra in (('integer output', fu), ('dual source', fd)) if extra]   # kept in the reflection, as a cache hit compiles nothing
        open(msl, 'w').write(plain)   # extras: a library that does not build with them is built without
        fu = fd = None
        try: side, cut = compile_libraries()
        except subprocess.CalledProcessError as e: return 'metal failed', str(e)[:160], None
    clone(msl, os.path.join(out, key + '_b.metal'))
    entry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', src)
    dr = dmsh_reflect.drv_reads(src)
    tgf = dmsh_rewrite.tg_fence_counts(src)
    refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': mesh['kind'] if mesh else j['stage'],
            **({'mesh_stage': {'kind': mesh['kind'], 'threads': mesh['threads'], 'payload': mesh['payload'], 'task': mesh.get('task', True)}, **({'entry_z': 'main0_z'} if mesh['kind'] == 'ms' else {}),
               **({'entry_object': 'main0_o'} if mesh['kind'] == 'ms' and not mesh.get('task', True) else {})} if mesh else {}),
            **({'mesh_spill': 1} if mesh and mesh.get('spill') else {}),
            'entry': entry.group(2) if entry else 'main0', 'msl_entry_kind': entry.group(1) if entry else None, **({'entry_z': 'main0_z'} if j['stage'] == 'vs' else {}),
            **({'entry_discard': 'main0_discard'} if discard else {}),
            **({'entry_u': 'main0_u', **{f'entry_u_{k}': True for k, v in dmsh_entry.typed_ops(fu).items() if v}} if fu else {}), **({'entry_d': 'main0_d'} if fd else {}), **({'blend_zero': True} if blend0 else {}), **({'entries_lost': lost} if lost else {}),
            **({'first_store': [{'reg': next(b['reg'] for b in binds if b['msl_name'] == n), 'word': w, 'value': v} for n, w, v in first]} if first else {}),
            **({'tg_spill': spill} if spill else {}),
            **({'tg_fence': dict(zip(('fence', 'barrier'), tgf))} if any(tgf) else {}),
            **({'loop_sites': loop_sites} if j['stage'] == 'cs' and loop_sites else {}),
            **({'split': ndsr['split']} if ndsr and 'split' in ndsr else {'ndsr_refused': ndsr['refused']} if ndsr else {}),
            'threadgroup_size': j.get('local'), 'c1': c1.hex(), 'notes': res['notes'] + (['not in the answer table, queries use defaults'] if j.get('no_facts') else []), 'drv_reads': dr, 'drv_unfillable': dmsh_reflect.unfillable(dr, mesh['kind'] if mesh else j['stage']),
            'bindings': binds,
            'vertex_inputs': [{'location': int(m.group(3)), 'msl_type': m.group(1)} for m in dmsh_reflect.attributes(src)],
            'raster': {**dmsh_reflect.raster_facts(j['stage'], src), **({'omap': ps_omap(open(j['code'], 'rb').read())} if j['stage'] == 'ps' else {})}, 'vs_pad': pad}
    refl.update(side)
    write_refl(key, refl, cut)
    return 'ok', '', refl

keys = build_keys(want, loc, PAIRS)
if want: want = set(keys)   # so a failed pixel build cannot leave old MSL for padding
NJ = int(os.environ.get('J', '8'))
PROCS = int(os.environ.get('DMSH_PROCS', NJ))
BATCH = 4 * NJ
# A run starts from no outputs of the keys it builds (the chain version does not change with the tables, so old ones would linger); a full build
# also removes the stamp and the image, a partial one builds on OLD_IMAGE less those keys
OUT_KEY = re.compile(r'([0-9a-f]{16})_b')
def owned_output(key): return key in want
STAMP_BEFORE = shader_lib.read_stamp(out).get('inputs')
OLD_IMAGE = None
if not want:
    shutil.rmtree(out, ignore_errors=True)
    shader_lib.remove_image(out)
elif os.path.isfile(shader_lib.image_of(out)): OLD_IMAGE = romfs.Path(shader_lib.image_of(out))
os.makedirs(out, exist_ok=True)
for _n in os.listdir(out):
    _m = OUT_KEY.match(_n)
    if _m and owned_output(_m.group(1)): os.unlink(os.path.join(out, _n))
if STAMP_BEFORE != INPUTS and os.path.exists(os.path.join(out, dmsh_chain.STAMP)): os.unlink(os.path.join(out, dmsh_chain.STAMP))
STORE.delete_prefix(STAGE + '/')

def sweep_work(root):
    """dmsh_* working folders whose build (its pid file) is gone; one without a pid after a day"""
    for d in (os.path.join(root, n) for n in os.listdir(root) if n.startswith('dmsh_')) if os.path.isdir(root) else ():
        try:
            with open(os.path.join(d, 'pid')) as f: pid = int(f.read())
            try:
                os.kill(pid, 0)
                continue
            except ProcessLookupError: pass
            except PermissionError: continue
        except (OSError, ValueError):
            try:
                if time.time() - os.stat(d).st_mtime < 86400: continue
            except OSError: continue
        shutil.rmtree(d, ignore_errors=True)
_work = os.environ.get('WORK') or tempfile.gettempdir()
sweep_work(_work)
w = tempfile.mkdtemp(prefix='dmsh_', dir=_work)
with open(os.path.join(w, 'pid'), 'w') as _f: _f.write(str(os.getpid()))
MTL = dmsh_mtlcache.Cache(STORE, {'mac': ('macosx', MAC_RECORD, mac_compile)}, w)
# first the libraries earlier chain versions left (b-*/ and b-*.img here, DMSH_PREFILL's roots)
for _root in [cache_root] + [x for x in os.environ.get('DMSH_PREFILL', '').split(os.pathsep) if x]:
    for _b in sorted(os.listdir(_root)) if os.path.isdir(_root) else []:
        if _b.startswith('b-') and os.path.realpath(shader_lib.folder_of(os.path.join(_root, _b))) != os.path.realpath(out):
            for _ln in MTL.fill(os.path.join(_root, _b)): print(_ln, flush=True)

# Progress for the packing script (a JSON file, build_app.py ShaderWatch): at most every half second, and at once for a new step or a forced count
PROGRESS = os.environ.get('DMSH_PROGRESS')
_prog = {'t': 0.0, 'step': '', 'lock': threading.Lock()}
def progress(step, done=None, total=None, force=False, **extra):
    if not PROGRESS: return
    with _prog['lock']:
        now = time.time()
        if not force and step == _prog['step'] and now - _prog['t'] < 0.5: return
        _prog['t'], _prog['step'] = now, step
        payload = {'step': step, **({'done': done} if done is not None else {}), **({'total': total} if total is not None else {}), **extra}
        try:
            with open(PROGRESS + '.tmp', 'w') as f: json.dump(payload, f)
            os.replace(PROGRESS + '.tmp', PROGRESS)
        except OSError: pass

packs, jobs, c1s = {}, {}, {}
packs_lock = threading.Lock()
digests, rule_fps, cache_reasons, cache_hits = {}, {}, {}, set()
def rec_of(key):
    """Found by its key, not the map row's index: a package from another split run would shift every row after a dropped one. Every key is looked up before the first job"""
    name = loc[key][0]
    with packs_lock:
        if name not in packs: packs[name] = record_index(os.path.join(dm_dir, name))
    fd, recs = packs[name]
    if key not in recs:
        old = ' (its records carry no keys: it was split before they did)' if recs and set(recs) == {'0' * 16} else ''
        sys.exit(f'[RED] {name} holds no record of shader {key}, which the shader map names: the package and the map come from different shader_split.py runs{old}; '
                 f'split the shaders again (the packing script does when {dm_dir} is removed)')
    o, e = recs[key]
    rec = os.pread(fd, e - o, o)
    check_record(key, name, rec)
    return name, rec
for key in keys: digests[key] = shader_key(key, *rec_of(key))
def plain_digest(k):
    if k in digests: return digests[k]
    return shader_key(k, *rec_of(k)) if k in loc else 'missing'
for key in keys:
    if PAIRS.get(key): digests[key] = pair_keyed(digests[key], [plain_digest(p) for p in sorted(PAIRS[key])])
for i, key in enumerate(keys):
    name = loc[key][0]
    rule_fps[key] = hashlib.sha256(shader_rule_source(name) + ','.join(sorted(PAIRS.get(key, ()))).encode()).hexdigest()   # rebuilt when its pairs change (vs_pad)
    hit, cache_reasons[key] = cache_lookup(key, digests[key], rule_fps[key], CODE_FP)
    if hit: cache_hits.add(key)
    progress('cache check', i + 1, len(keys), force=i + 1 == len(keys), cached=len(cache_hits))
NMISS = sum(1 for k in keys if k not in cache_hits)
xerr = {}
def xlate_errors(r):
    for ln in r.stderr.splitlines():
        m = re.match(r'\[RED\] (\w+): (.*)', ln)
        if m: xerr[m.group(1)] = m.group(2)[:160]
def translate(batch):
    wb = tempfile.mkdtemp(prefix='batch_', dir=w)
    miss = [k for k in batch if k not in cache_hits]
    for k in miss: jobs[k], c1s[k] = job_for(k, loc[k][0], rec_of(k)[1], wb)
    def xl(ch):
        jp = os.path.join(wb, f'job_{abs(hash(ch[0]))}.json')
        json.dump([jobs[k] for k in ch], open(jp, 'w'))
        return subprocess.run([XLATE, jp, wb], capture_output=True, text=True)
    with cf.ThreadPoolExecutor(NJ) as ex:
        for r in ex.map(xl, [c for c in (miss[i::NJ] for i in range(NJ)) if c]): xlate_errors(r)
    return wb
def restored(key, wb):
    """False when another build pruned them since cache_lookup: the shader is then translated now"""
    if restore_cache(key, digests[key]): return True
    PRE_STATS['hit'] -= 1
    PRE_STATS['miss'] += 1
    PRE_STATS['pruned meanwhile'] += 1
    jobs[key], c1s[key] = job_for(key, loc[key][0], rec_of(key)[1], wb)
    jp = os.path.join(wb, f'job1_{key}.json')
    json.dump([jobs[key]], open(jp, 'w'))
    xlate_errors(subprocess.run([XLATE, jp, wb], capture_output=True, text=True))
    return False
DONE = collections.Counter()
def fin(key, wb):
    try:
        if key in cache_hits and restored(key, wb):
            refl = json.loads(output_of(key + '_b.json'))
            src = output_of(key + '_b.metal').decode() if refl['stage'] in ('ps', 'vs') else ''
            if refl['stage'] == 'vs':
                side, cut = vs_libraries(key, os.path.join(out, key + '_b.metal'), src)
                refl.update(side)
                write_refl(key, refl, cut)
            if refl['stage'] == 'ps':
                refl.update(ps_libraries(key, os.path.join(out, key + '_b.metal'), src, refl['bindings']))   # stored without the side-library reflection (publish_cache)
                write_refl(key, refl)
            return key, 'ok', '', refl
        if key in xerr or not os.path.exists(os.path.join(wb, key + '.spv')): return key, 'translation failed', xerr.get(key, 'no spv'), None
        result = (key,) + finish(key, wb, jobs[key], c1s[key])
        if result[1] == 'ok': publish_cache(key, digests[key], rule_fps[key], CODE_FP)
        return result
    finally:
        commit(key)
        with os.scandir(wb) as it:
            for e in it:
                if e.name.startswith((key, 'job1_' + key)) and e.is_file(): os.unlink(e.path)
        finished()
def finished(*_):
    with _prog['lock']:
        DONE['n'] += 1
        n = DONE['n']
    progress('compiling', n, len(keys), force=n == len(keys), cached=len(cache_hits), misses=NMISS)
# Shaders finish in forked worker processes (fin holds the GIL), forked before any thread starts so they inherit every table. A worker does not open the cache (Apple's
# SQLite crashes when a fork opens a database): results come back here in batch order, as with threads. The Metal PCH is made before the fork.
COUNTERS = (PRE_STATS, VIN_STATS, PULL_STATS, SIDE_STATS, BF_STATS, MTL.stats)
def worker_start():
    global PROGRESS, STORE
    assert SERVED is not None
    STORE = MTL.store = SERVED.worker()
    PROGRESS = None
def fin_worker(key, wb, job, err):
    """→ (fin's result, what it added to the counters)"""
    jobs.clear()
    c1s.clear()
    xerr.clear()
    if job: jobs[key], c1s[key] = job
    if err is not None: xerr[key] = err
    for c in COUNTERS: c.clear()
    return fin(key, wb), [dict(c) for c in COUNTERS]
def submit(batch, wb):
    if not px: return [ex.submit(fin, k, wb) for k in batch]
    fs = [px.submit(fin_worker, k, wb, (jobs.pop(k), c1s.pop(k)) if k in jobs else None, xerr.get(k)) for k in batch]
    for f in fs: f.add_done_callback(finished)
    return fs
def results(fs):
    out = []
    for f in fs:
        r = f.result()
        if px:
            r, counts = r
            for c, n in zip(COUNTERS, counts): c.update(n)
        out.append(r)
    return out
SERVED = shader_cache.Served(STORE, PROCS) if PROCS and keys else None
px = cf.ProcessPoolExecutor(PROCS, mp_context=multiprocessing.get_context('fork'), initializer=worker_start) if PROCS and keys else None
if px:
    dmsh_pch.header(MAC_RUN + METAL_DEPTH)
    px.submit(int).result()   # forks every worker now, while this process has no other thread
    assert SERVED is not None
    SERVED.start()
# Paired vertex shaders read their pixel shaders' MSL (vs_pad), so they go last, once every shader before them is finished
batches = [b[i:i + BATCH] for b in ([k for k in keys if k not in PAIRS], [k for k in keys if k in PAIRS]) for i in range(0, len(b), BATCH)]
res, busy = [], []
def collect(left):
    while len(busy) > left:
        wb, fs = busy.pop(0)
        res.extend(results(fs))
        shutil.rmtree(wb, ignore_errors=True)
try:
    with cf.ThreadPoolExecutor(1) as tx, cf.ThreadPoolExecutor(NJ) as ex:
        nxt = tx.submit(translate, batches[0]) if batches else None
        for i, batch in enumerate(batches):
            assert nxt is not None
            wb = nxt.result()
            nxt = tx.submit(translate, batches[i + 1]) if i + 1 < len(batches) else None
            if batch[0] in PAIRS and i and batches[i - 1][0] not in PAIRS: collect(0)
            busy.append((wb, submit(batch, wb)))
            collect(1)
        collect(0)
except cf.process.BrokenProcessPool:
    sys.exit(f'[RED] a shader build worker process stopped (killed, or out of memory) with {len(keys) - len(res)} of {len(keys)} shaders unfinished; run the build again, '
             f'with fewer workers (DMSH_PROCS) if memory ran out, or with DMSH_PROCS=0 (threads) if it stops again: what it finished is in the cache')
if px: px.shutdown()
shutil.rmtree(w, ignore_errors=True)
st = collections.Counter(r[1] for r in res)
print(f'shaders {len(res)}: ok {st["ok"]}, ' + ', '.join(f'{k} {v}' for k, v in st.items() if k != 'ok'))
print(f'shader cache: hit {PRE_STATS["hit"]}, rebuilt {PRE_STATS["miss"]}')
for reason in ('new key', 'rule changed', 'code fingerprint changed: all rebuilt'):
    if PRE_STATS[reason]: print(f'  {reason} {PRE_STATS[reason]}')
print(f'readable-tile versions (<key>_bf, dmsh_fetch.py): {BF_STATS["generated"]} generated, {BF_STATS["failed"]} failed to compile')
print(f'shadow-input libraries (<key>_bv, dmsh_vin.py): {VIN_STATS["generated"]} generated, {VIN_STATS["failed"]} failed to compile, {VIN_STATS["not generated"]} not generated; '
      f'{VIN_STATS["inputs"]} integer inputs shadowed, {VIN_STATS["float only"]} of them with no other-sign shadow (attribute indices ran out)')
print(f'alpha test and logic operation libraries (<key>_bx, dmsh_side.py): {SIDE_STATS["generated"]} generated, {SIDE_STATS["failed"]} failed to compile, {SIDE_STATS["not generated"]} pixel shaders without either')
print(f'pulling libraries (<key>_bp, dmsh_pull.py): {PULL_STATS["generated"]} generated, {PULL_STATS["failed"]} failed to compile, {PULL_STATS["not generated"]} not generated, '
      f'{PULL_STATS["no stage input"]} vertex shaders without stage inputs (none needed); '
      f'{PULL_STATS["unit"]} vertex shaders\' libraries cut from one compile (dmsh_side.vs_unit), {PULL_STATS["unit failed"]} compiled each from its own source instead')
print(MTL.summary())
for lvl in [k for k in st if k != 'ok']:
    for e, n in collections.Counter(r[2] for r in res if r[1] == lvl).most_common(10): print(f'  [{lvl}] {n:5d}  {e}')
ok = [r[3] for r in res if r[1] == 'ok']
rd = [x for x in ok if x['drv_reads']]
print(f'shaders reading driver segments at run time {len(rd)} (c0 {sum("0" in x["drv_reads"] for x in rd)}, c2 {sum("2" in x["drv_reads"] for x in rd)}); reading what the replay layer cannot fill {sum(bool(x["drv_unfillable"]) for x in ok)}')
for e, n in collections.Counter(u for x in ok for u in x['drv_unfillable']).most_common(15): print(f'  cannot fill {n:5d}  {e}')
nd = [x for x in ok if x.get('split')]
print(f'agl\'s NDSR network on simdgroup matrices in two passes (dmsh_ndsr.py): {len(nd)} kernels ({" ".join(x["dm_ctl"] for x in nd)}); '
      f'{sum(1 for x in ok if x.get("ndsr_refused"))} other kernels with HMMA calls left as translated')
for e, n in collections.Counter(x['ndsr_refused'] for x in ok if x.get('ndsr_refused')).most_common(6): print(f'  {n:5d}  {e}')
tgf = [x['tg_fence'] for x in ok if x.get('tg_fence')]
print(f'warp-synchronous shared memory (dmsh_tg_fence, dmsh_rewrite.tg_fence): {sum(t.get("fence", 0) for t in tgf)} calls left as the simdgroup fence, '
      f'{sum(t.get("barrier", 0) for t in tgf)} made a SIMD-group barrier (a kernel\'s, in uniform control flow), in {len(tgf)} shaders')
f1 = [b for x in ok for b in x['bindings'] if b.get('fetch1d')]
print(f'1D integer fetches read as 2D rows of 16384 texels (a texture buffer there is bound as rows, nvn_mtl.m tbuf_as_2d): {len(f1)} bindings in {sum(any(b.get("fetch1d") for b in x["bindings"]) for x in ok)} shaders, '
      f'{sum(1 for b in f1 if b.get("fetch1d_size"))} of them also size-queried')
SLOTK = {abi.KIND_CBUF, abi.KIND_SAMP, abi.KIND_SAMP_TEX, abi.KIND_TEX, abi.KIND_IMG, abi.KIND_TBUF, abi.KIND_SBUF, abi.KIND_UBO_PTR, abi.KIND_SBUF_UBO, abi.KIND_TEX_UBO, abi.KIND_SAMP_UBO, abi.KIND_SAMP_ID, abi.KIND_IMG_UBO, abi.KIND_TBUF_UBO}
noreg = [x for x in ok if any(b['kind'] in SLOTK and b.get('reg') is None for b in x['bindings'])]
unk = [x for x in ok if any(b['kind'] == abi.KIND_UNKNOWN for b in x['bindings'])]
print(f'shaders with a binding whose slot was not decoded {len(noreg)}, with an unrecognized binding kind {len(unk)} (loading them at run time prints [RED] lines)')
for e, n in collections.Counter(f"{b['kind']} {b['msl_name'][:28]}" for x in noreg + unk for b in x['bindings'] if b['kind'] == abi.KIND_UNKNOWN or (b['kind'] in SLOTK and b.get('reg') is None)).most_common(8): print(f'  {n:5d}  {e}')
nt = collections.Counter(nn.split(' (')[0] for x in ok for nn in x['notes'] if not nn.startswith('translator: '))
for e, n in nt.most_common(8): print(f'  queries without an answer {n:5d}  {e}')
tm = collections.Counter(nn[len('translator: '):] for x in ok for nn in x['notes'] if nn.startswith('translator: '))
for e, n in tm.most_common(12): print(f'  translator {n:6d} in {sum(e in [nn[len("translator: "):] for nn in x["notes"]] for x in ok):5d} shaders  {e}')
shader_regressions.report_losses(ok)
json.dump([{'key': r[0], 'st': r[1], 'err': r[2]} for r in res], open(os.path.join(out, '_b_build.json'), 'w'), ensure_ascii=False)
if not want:
    # Before the stamp, so a stamped folder always has its list (prune_shader_cache keeps these entries)
    with open(os.path.join(out, CAS_USED + '.tmp'), 'w') as _f: _f.write(''.join(d + '\n' for d in sorted(set(digests.values()))))
    os.replace(os.path.join(out, CAS_USED + '.tmp'), os.path.join(out, CAS_USED))
if not want: dmsh_chain.write_stamp(out, GAME.id, INPUTS)   # a build stopped on a full drive leaves no stamp beside a library it could not write
publish_library(lambda done, total, size: progress('writing the library image', done, total, force=done == total, unit='files', bytes=size))
if not want: print(f'library stamp: game {GAME.id}, inputs {INPUTS}', flush=True)
if not want:
    for _tool, _table, _step in (('c1_guard.py', 'c1_full.tsv', 'c1 guard'), ('cb_extent.py', 'cb_extent.tsv', 'uniform read extents')):
        progress(_step, force=True)
        _r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), _tool), out, str(GAME.table(_table))], capture_output=True, text=True)
        if _r.returncode: sys.exit(f'[RED] {_tool} failed ({_r.returncode}): {_r.stderr.strip()[-500:]}')
        print(f'{_table}: {(_r.stdout.strip().splitlines() or [""])[-1]}', flush=True)
elif STAMP_BEFORE != INPUTS: print(f'no library stamp: the profile\'s inputs changed since the last full build ({STAMP_BEFORE} → {INPUTS}), and a partial build does not rebuild the rest', flush=True)
if PROGRESS:
    try: os.unlink(PROGRESS)
    except FileNotFoundError: pass
