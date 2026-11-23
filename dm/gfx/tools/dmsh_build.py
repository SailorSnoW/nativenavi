#!/usr/bin/env python3
# Route B offline build: original Switch shaders → SPIR-V (dmsh_xlate, the Ryujinx translator) → spirv-cross MSL → metallib + replay reflection JSON.
# No translation at runtime: build output goes into the shader cache directory, file name = Switch control segment key (dm_ctl_sha16, same as column 4 of shader_map.tsv) + "_b".
# Source of query answers: the answer table in the game profile, dm/games/<id>/shader_facts.tsv (dmsh_facts.py) - texture types, vertex attribute / fragment output component types, compute thread group size and shared memory.
# Usage: dmsh_build.py <Switch package dir> <output cache dir> [key...] (no keys = all); build output is the image <output cache dir>/b-<chain version>.img (dm/tools/shader_lib.py), for the chain version see dmsh_chain.py.
# The folder b-<chain version>/ beside it holds only the shaders in flight: a finished shader's files go into the shader cache (cas-<chain>.db, under stage/), and the image is written from there
# at the end (publish_library), so a build, stopped or not, leaves a few files and not one per shader output (Octopath Traveler 0, stopped in its compile: 68,230 files). The translator's
# working files go into a folder per batch of shaders, removed when the batch is done; a working folder whose build is gone is removed at the next start (sweep_work).
# A full build (no keys) also stamps the directory with the game and the digest of its profile inputs (inputs.tsv) and writes the profile's c1_full.tsv and cb_extent.tsv (c1_guard.py, cb_extent.py)
# from its Mac outputs. One output directory per game: the chain version is the same for every game   Environment: DM_GAME=another game profile than Ocarina of Time's (a derived one), XLATE=path to the translation tool (default the in-repo dmsh_xlate_bin), DMSH_CAS=the compile cache (shader_cache.py): a folder, whose cas-<chain>.db this build writes (default the output cache dir), or one file, DMSH_PREFILL=library roots (separated by :, their b-*/ go into the MSL-keyed compile cache first; sibling b-*/ always do), J=concurrency, DMSH_PROCS=processes that finish shaders (default J; 0 = threads of this process, see fin_worker)
# Build output: <key>_b.metallib, <key>_b.json (replay reflection, including drv_reads, the runtime read offsets into driver segments c0 / c2), <key>_b.metal (MSL)
# Final summary: success / failure counts and the distribution of reasons; distribution of driver segment read offsets (those outside the range the replay layer can fill are listed separately);
# a [RED] line per shader that built but lost something (shader_regressions.report_losses), which does not change the exit status
import atexit, errno, inspect, multiprocessing, os, re, sys, json, mmap, struct, hashlib, shutil, subprocess, tempfile, time, collections, threading, concurrent.futures as cf
import dmsh_atomic, dmsh_wide, dmsh_meshstage, dmsh_rewrite, dmsh_reflect, dmsh_entry, dmsh_abi as abi   # abi: the driver segment layout and the binding kinds the host shares (dm/gfx/nvn/dmsh_abi.h)
DM_ROOT = os.environ.get('DM_ROOT') or os.path.expanduser('~/dm-work')   # Workspace root (game files, build outputs and run artifacts are all under it)
HOME = os.path.expanduser('~')
dm_dir, out = sys.argv[1:3]; want = set(sys.argv[3:]); cache_root = out
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__))); import dmsh_chain, dmsh_fetch, dmsh_facts, dmsh_mtlcache, dmsh_pch, dmsh_vin, dmsh_pull, dmsh_side, dmsh_ampere, shader_regressions, shader_cache, tool_lock   # translation chain version: verify the tools against the lock file first; build output goes into b-<V>/
_bad = tool_lock.check_tools()
if _bad: sys.exit('\n'.join('[RED] translation chain tools do not match the lock file: ' + b for b in _bad) + '\n(if the tools changed, run tool_lock.py --update-lock and commit the lock file; if the translator just was not rebuilt, rebuild it)')
CHAIN = dmsh_chain.version(); out = os.path.join(out, 'b-' + CHAIN)
GAME = dmsh_chain.load_game()   # the current game profile (DM_GAME): the tables come from here
if f'"{CHAIN}"' not in open(dmsh_chain.HEADER).read(): sys.exit(f'[RED] {dmsh_chain.HEADER} does not hold the current translation chain version {CHAIN}: run dmsh_chain.py --header first and commit it (otherwise the host cannot find these libraries)')
INPUTS = dmsh_chain.inputs(GAME)   # the digest of the profile's inputs, which a full build stamps on its libraries (b-<V>/inputs.tsv)
XLATE = os.environ.get('XLATE', os.path.join(dmsh_chain.XLATE_DIR, 'dmsh_xlate'))
SPIRV_CROSS = dmsh_chain.SPIRV_CROSS   # the in-repo build the lock records
FACTS = dmsh_facts.load()
if not os.path.exists(dmsh_facts.TABLE): sys.exit(f'[RED] no answer table {dmsh_facts.TABLE} (it comes with the game profile; an empty table = every query answered with defaults)')
import shader_lib, romfs   # dm/tools (on the path shader_regressions puts it): the library's image, which this build writes (publish_library)
_g = shader_lib.read_stamp(out).get('game')   # one chain version serves every game, so b-<V>/ alone does not keep two games' libraries apart
if _g and _g != GAME.id: sys.exit(f'[RED] {out} holds the shader libraries of game {_g}, not {GAME.id}: give each game its own output directory')
MAP = str(GAME.table('shader_map.tsv'))
BF_WANT_FILE = str(GAME.table('bf_want.tsv'))
BF_WANT = {line.strip() for line in open(BF_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(BF_WANT_FILE) else set()
DUAL_WANT_FILE = str(GAME.table('dual_want.tsv'))   # pixel shaders a pipeline blends with their output 1 as the second source (see fs_dual), produced by the host (DM_NVN_DUAL_WANT)
DUAL_WANT = {line.split()[0] for line in open(DUAL_WANT_FILE) if line.strip() and not line.lstrip().startswith('#')} if os.path.exists(DUAL_WANT_FILE) else set()
XC = [*dmsh_chain.CROSS_MSL, '--msl-pad-fragment-output']   # MSL 3.2 (see dmsh_chain.CROSS_MSL)

_cas = os.environ.get('DMSH_CAS') or cache_root   # a cache folder: this chain's own file over the folder's older generations (shader_cache.for_chain); a file: that one alone (regress.py's)
STORE: 'shader_cache.Store | shader_cache.Remote' = shader_cache.for_chain(_cas, CHAIN) if os.path.isdir(_cas) or _cas == cache_root else shader_cache.Store(_cas)
atexit.register(STORE.close)   # folds its log into the one file
CAS_LOCKS, CAS_LOCKS_GUARD = {}, threading.Lock()
PRE_STATS = collections.Counter()
# The Mac compile is dmsh_chain.MAC_METAL (each flag's reason there)
MAC_RECORD = ' '.join(dmsh_chain.MAC_METAL) + '; xcrun -sdk macosx metallib'
# The compiles run the tools xcrun names, found once: each xcrun launch looks them up again, about 4 ms of a compile's two launches (8 ms a shader). -isysroot is what xcrun -sdk passes
# (as SDKROOT), and the outputs are the same bytes (.air and .metallib, checked on Odyssey's shaders); without it the driver targets the running macOS, not the SDK, and they differ.
# MAC_RECORD names the xcrun command, which runs the same compiler
def xcrun_out(*args): return subprocess.run(['xcrun', '-sdk', 'macosx', *args], capture_output=True, text=True, check=True).stdout.strip()
MAC_RUN = [xcrun_out('-f', 'metal'), '-isysroot', xcrun_out('--show-sdk-path'), *dmsh_chain.MAC_METAL[4:]]
MAC_LINK = [xcrun_out('-f', 'metallib')]
# -fbracket-depth: the front end stops at 256 nested brackets ("bracket nesting level exceeded maximum of 256"). Ocarina of Time's compute shaders 1f78730053feb036 and e172bd7ec218ab8e
# (3.5 MB of MSL each) nest 259 deep in one expression and were left out. A source within the default limit compiles to the same bytes with it (checked), so, like the
# precompiled header, the records do not name it and the Metal cache keeps its libraries
METAL_DEPTH = ['-fbracket-depth=1024']
# The outer shader cache restores compiled libraries directly, before the MSL compile cache can check their compiler. Keep override builds separate there too.
METAL_FP = dmsh_mtlcache.compiler_id('macosx')
def mac_compile(msl, air, lib):   # as dmsh_mtlcache.Cache calls it; the precompiled header and METAL_DEPTH change no output, so MAC_RECORD does not name them (dmsh_pch.py)
    dmsh_pch.compile(MAC_RUN + METAL_DEPTH, msl, air)
    subprocess.run([*MAC_LINK, air, '-o', lib], check=True)

def record_index(path):   # → (an open descriptor, {a record's key: its bytes' (start, end)}): shader_split.py writes each record's key at +0x08, its bytes from its marker to the next one
    # Scanned through a read-only mapping that is closed again, so no package stays in this process. Pokopia's ps_rdb.bin (4.25 GB) read and cut into its 105,118
    # records left the process at 8.7 GB, and each fork (every spirv-cross and Metal compile) then took 75 ms instead of 3 with the GIL held: one shader worker at a time (GitHub issue #1036)
    fd = os.open(path, os.O_RDONLY); n = os.fstat(fd).st_size
    if not n: return fd, {}
    with mmap.mmap(fd, n, access=mmap.ACCESS_READ) as m:
        offs = [x.start() for x in re.finditer(b'BTCDBTCD', m)] + [n]
        return fd, {m[o + 8:o + 16].hex(): (o, e) for o, e in zip(offs, offs[1:])}

def clone(src, dst):   # APFS copy-on-write, else a plain copy; the clonefile call, not a cp: a fork of this large process costs about 6 ms
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    dmsh_mtlcache.clone(src, dst)

def atomic_json(path, value):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    tmp = path + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
    with open(tmp, 'w') as f: json.dump(value, f, ensure_ascii=False, indent=1)
    os.replace(tmp, path)

def source_without_rule_bodies():   # this file's code form (dmsh_chain.code_form): no comments, docstrings or formatting, and the rule functions' bodies blanked
    return dmsh_chain.code_form(open(__file__, 'r').read(), {f.__name__ for f, _ in RULES})

def shader_rule_source(name):
    return b''.join(inspect.getsource(f).encode() for f, applies in RULES if applies(name))

def tool_fingerprint():
    sc = subprocess.run([SPIRV_CROSS, '--version'], capture_output=True, text=True, timeout=30)
    return ('lock\0' + open(dmsh_chain.LOCK).read() + '\0options\0' + dmsh_chain.options() +
            '\0xlate\0' + dmsh_chain.xlate_source_sha() + '\0spirv-cross\0' + sc.stdout + sc.stderr +
            '\0xc\0' + repr(XC) + '\0mac\0' + MAC_RECORD + '\0metal\0' + METAL_FP)

# The profile keys that change how a shader is built (nvn_*, shader_*), as dmsh_chain.inputs takes them: part of every shader's cache key
PROFILE_ROWS = dmsh_chain.profile_rows(GAME)
def shader_key(key, name, rec):
    """The per-shader cache key. It covers the profile's build keys and the shader's pairs; a vertex shader with pairs also takes its pixel shaders' keys (pair_keyed), since vs_pad reads their
    MSL. Before, these were only in the chain version, so a build after changing only them hit the old entries and filed them under the new version (with an empty DMSH_CAS as the workaround)"""
    pc = dmsh_facts.row(key, FACTS[key]).encode() if key in FACTS else 'missing'.encode()
    map_row = MAP_ROWS.get(key, 'missing'.encode()) + MAP_CONFLICT_ROWS.get(key, b'')
    payload = (b'record\0' + rec + b'\0map\0' + map_row + b'\0pc\0' + pc +
               b'\0tools\0' + TOOL_FP + b'\0code\0' + CODE_FP.encode() +
               b'\0rule\0' + shader_rule_source(name) + b'\0profile\0' + PROFILE_ROWS + b'\0pairs\0' + ','.join(sorted(PAIRS.get(key, ()))).encode() +
               b'\0dual\0' + (b'1' if key in DUAL_WANT else b'0'))   # fs_dual emits another entry point for listed shaders; the cache must track that choice
    return hashlib.sha256(payload).hexdigest()
def pair_keyed(digest, pixel_digests):   # a vertex shader's key with its pixel shaders' keys (vs_pad)
    return hashlib.sha256((digest + '\0' + ','.join(pixel_digests)).encode()).hexdigest()

def cache_meta_path(key): return f'index/{key}.json'
def cache_path(digest): return f'shader/{digest}'
CAS_USED = '_b_cas.txt'   # in the Mac library directory b-<V>/: the cache_path digests of its last full build, one per line

# A finished shader's outputs are staged in the cache, under a name of this output folder's, and leave the disk (commit); the image is written from them at the end (publish_library).
# A stopped build leaves them there, and the next build of this folder clears them first: its shaders come back from the cache entries publish_cache wrote
STAGE = 'stage/' + hashlib.sha256(os.path.realpath(out).encode()).hexdigest()[:16]
def output(name):
    """An output file's bytes: on disk while its shader is in flight, else staged; for a partial build, a shader it does not build is read from the image it builds on. None when there is none"""
    try:
        with open(os.path.join(out, name), 'rb') as f: return f.read()
    except FileNotFoundError: pass
    data = STORE.get(f'{STAGE}/mac/{name}')   # put before its file is removed (commit), so a file gone meanwhile is found here
    if data is not None or not want: return data
    m = OUT_KEY.match(name)
    if OLD_IMAGE is None or not m or owned_output(m.group(1)): return None   # a product this build makes has no older outputs: the start removed them
    try: return (OLD_IMAGE / name).read_bytes()
    except OSError: return None

def output_of(name) -> bytes:   # an output its shader has just written or restored: there is one
    data = output(name)
    if data is None: raise FileNotFoundError(f'{name}: no such output')
    return data

def commit(key='', names=None):
    """A finished shader's files (<key>_*, or the names given) into the cache's stage, then off the disk once the stage has them: a cache that cannot take one (a full drive) rolls the whole batch back,
    and all of them stay on disk, where publish_library finds them too"""
    staged = []
    with STORE.batch():
        for n in (names if names is not None else [n for n in os.listdir(out) if n.startswith(key + '_')]):
            p = os.path.join(out, n)
            if n.startswith('.') or '.tmp' in n or not os.path.isfile(p): continue
            STORE.put_file(f'{STAGE}/mac/{n}', p); staged.append(p)
    if not STORE.full:
        for p in staged: os.unlink(p)

def publish_library():
    """The library's image <dir>.img from the staged outputs and the files left on disk (these win), and for a partial build the image it builds on, less the outputs of the keys it built
    (and its stamp, when the profile's inputs changed). Then the folder and the staged entries go. When the image cannot be written, a full build whose drive is out of space stops at once
    without unpacking loose files back onto the full drive; otherwise the staged outputs go back into the folder, which works as it is, and a folder restore that fails too removes what it
    wrote and stops the build"""
    d = out; image = shader_lib.image_of(d); prefix = f'{STAGE}/mac/'
    files = {}
    if OLD_IMAGE is not None:
        for p in OLD_IMAGE.iterdir():
            m = OUT_KEY.match(p.name)
            if (m and owned_output(m.group(1))) or (p.name == dmsh_chain.STAMP and STAMP_BEFORE != INPUTS): continue
            files[p.name] = str(p)
    staged = STORE.names(prefix)
    for n in staged: files[n[len(prefix):]] = (lambda n=n: STORE.get(n))
    for n in os.listdir(d):
        if not n.startswith('.') and '.tmp' not in n and os.path.isfile(os.path.join(d, n)): files[n] = os.path.join(d, n)
    try: shader_lib.write(files, image)
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
        except OSError as e2:   # a drive that cannot take the loose files either: give back what this restore wrote and stop with one red line
            for p in written:
                try: os.unlink(p)
                except OSError: pass
            stop(f'could not write {image} ({e}) or restore the shaders into {d} ({e2})', e2)
        print(f'[RED] could not write {image}, the library stays the folder {d}: {e}', flush=True)
        return
    STORE.delete_prefix(prefix)
    shutil.rmtree(d, ignore_errors=True)

def cache_outputs(key, digest, cached=False):   # → (the cache entry's root, the shader's outputs it holds, or the output folder does), None when one every shader has is missing
    root = cache_path(digest)
    required = [key + '_b.json', key + '_b.metal', key + '_b.metallib']   # fin(), paired vertex padding and the profile tables need the source and the reflection
    have = STORE.names(f'{root}/mac/') if cached else set()
    if not all(f'{root}/mac/{n}' in have if cached else os.path.exists(os.path.join(out, n)) for n in required): return None
    return root, required

def restore_cache(key, digest):
    found = cache_outputs(key, digest, cached=True)
    if not found: return False
    root, names = found
    for name in names:
        src = f'{root}/mac/{name}'; dst = os.path.join(out, name)
        if name.endswith('.json'):
            data = STORE.get(src)
            if data is None: return False   # pruned by another build since the look above: a miss, which builds it
            tmp = dst + '.tmp-' + str(os.getpid()) + '-' + str(threading.get_ident())
            with open(tmp, 'wb') as f: f.write(re.sub(rb'("chain"\s*:\s*")[^"]*"', rb'\g<1>' + CHAIN.encode() + b'"', data, count=1))
            os.replace(tmp, dst)
        elif not STORE.to_file(src, dst): return False
    return True

def cache_lookup(key, digest, rule_fp, code_fp):   # whether the cache holds the shader's outputs; they are restored when the shader is finished (fin), so they are on disk only while it is in flight
    with CAS_LOCKS_GUARD: lock = CAS_LOCKS.setdefault(('shader', key), threading.Lock())
    with lock:
        if cache_outputs(key, digest, cached=True): PRE_STATS['hit'] += 1; return True, 'hit'
        old = json.loads(STORE.get(cache_meta_path(key)) or b'null')
        if old and old.get('code_fp') != code_fp: reason = 'code fingerprint changed: all rebuilt'
        elif old and old.get('rule_fp') != rule_fp: reason = 'rule changed'
        else: reason = 'new key'
        PRE_STATS['miss'] += 1; PRE_STATS[reason] += 1; return False, reason

def publish_cache(key, digest, rule_fp, code_fp):
    # Paired pixel MSL is an input to vs_pad; a diagnostic build without it must not cache an incomplete vertex shader under a complete dependency key.
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
                    value.pop(field, None)   # fin regenerates the side libraries and records how the base was built
                STORE.put(f'{root}/mac/{name}', json.dumps(value, ensure_ascii=False, indent=1).encode())
            else: STORE.put_file(f'{root}/mac/{name}', src)
        STORE.put(cache_meta_path(key), json.dumps({'digest': digest, 'rule_fp': rule_fp, 'code_fp': code_fp}).encode())

def write_refl(key, refl, cut=False):   # cut: the base library was cut from one compile with its side libraries (vs_libraries)
    recorded = MTL.with_compiler('mac', refl)
    if cut: recorded['chain_metal_cut'] = True
    atomic_json(os.path.join(out, key + '_b.json'), recorded)

VIN_STATS = collections.Counter()
PULL_STATS = collections.Counter()
SIDE_STATS = collections.Counter()
BF_STATS = collections.Counter()

def pull_finish(key, full, refl):
    """The pulling library <key>_bp of a vertex shader with stage inputs (dmsh_pull.py: no [[stage_in]], the inputs read through the per-draw layout block), compiled through the
    MSL-keyed Metal cache with the chain's flags; reflection 'pull' names its entry points, the layout block's buffer index and the inputs. Shaders without stage inputs get none
    (their pipelines have no vertex descriptor anyway), nor does a shader the variant does not fit (stale libraries removed). Changes refl; the caller writes it"""
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
        drop(); return refl
    msl = os.path.join(w, key + '_bp.metal'); open(msl, 'w').write(v)
    try:
        MTL.compile('mac', msl, lib)
    except subprocess.CalledProcessError as e:
        PULL_STATS['failed'] += 1; print(f'[RED] {key}: the pulling library does not compile ({str(e)[:120]}): its pipelines are always built for the draw\'s vertex layout', flush=True)
        drop(); return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    PULL_STATS['generated'] += 1
    refl['pull'] = {k: v_ for k, v_ in info.items() if v_ is not None}
    return refl

def vin_finish(key, full, refl):
    """The shadow-input library <key>_bv of a vertex shader with integer inputs (dmsh_vin.py), compiled through the MSL-keyed Metal cache; reflection 'vin' names its entry points and
    the shadow attribute indices. Other shaders get none (stale libraries removed). Changes refl; the caller writes it"""
    if refl['stage'] != 'vs': return refl
    lib = os.path.join(out, key + '_bv.metallib')
    refl.pop('vin', None)
    def drop():
        try: os.unlink(lib)
        except FileNotFoundError: pass
    v, shadows = dmsh_vin.variant(full)
    if not v:
        if shadows != 'no integer input': VIN_STATS['not generated'] += 1; print(f'[RED] {key}: no shadow-input library ({shadows}): its integer inputs keep the CPU conversion', flush=True)
        drop(); return refl
    VIN_STATS['generated'] += 1
    msl = os.path.join(w, key + '_bv.metal'); open(msl, 'w').write(v)
    try:
        MTL.compile('mac', msl, lib)
    except subprocess.CalledProcessError as e:
        VIN_STATS['failed'] += 1; print(f'[RED] {key}: the shadow-input library does not compile ({str(e)[:120]}): its integer inputs keep the CPU conversion', flush=True)
        drop(); return refl
    finally:
        try: os.unlink(msl)
        except FileNotFoundError: pass
    refl['vin'] = {'entry': 'main0', 'entry_z': 'main0_z', 'shadow': shadows}
    VIN_STATS['inputs'] += len(shadows); VIN_STATS['float only'] += sum(1 for s in shadows if 'sint' not in s and 'uint' not in s)
    return refl

def vs_libraries(key, msl, full):
    """A vertex shader's libraries: <key>_b with its own entry points, its pulling library <key>_bp (pull_finish) and its shadow-input library <key>_bv (vin_finish). A shader with
    a pulling library gets them cut from one compile through the MSL-keyed Metal cache (dmsh_side.vs_unit, dmsh_side.cut), which holds the body once where the three sources held it
    nine or twelve times.
    A shader whose unit does not build, or whose source is not of the shape vs_unit knows, gets the three compiles as before, each library from its own source, with a red line; so
    does one without a pulling library, with none. Returns the 'pull' and 'vin' reflection and whether the base library was cut from a unit. Raises CalledProcessError when the
    plain source does not compile."""
    cut = False
    pv, pinfo = dmsh_pull.variant(full); why = None
    if pv and isinstance(pinfo, dict):
        vv, shadows = dmsh_vin.variant(full)
        try:
            whole = dmsh_side.vs_unit(full, pv, vv)
            src, pn, vn = whole
            names = ['_b'] + (['_bp'] if pn else []) + (['_bv'] if vn else []); groups = [set(n.values()) for n in (pn, vn) if n]
            u = os.path.join(w, f'{key}_bu_mac.metal'); open(u, 'w').write(src)
            try: MTL.compile_split('mac', u, [os.path.join(out, key + n + '.metallib') for n in names], lambda lib, groups=groups: dmsh_side.cut(lib, *groups))
            finally: os.unlink(u)
            cut = True
        except (subprocess.CalledProcessError, ValueError) as e: why = str(e)[:120]
        else:
            PULL_STATS['generated'] += 1; PULL_STATS['unit'] += cut
            refl = {'stage': 'vs', 'pull': {k: v for k, v in {**pinfo, **whole[1]}.items() if v is not None}}
            if not vv: vin_finish(key, full, refl)   # none: its reason, and a stale library removed
            else:
                refl['vin'] = {'entry': 'main0', 'entry_z': 'main0_z', **whole[2], 'shadow': shadows}
                VIN_STATS['generated'] += 1; VIN_STATS['inputs'] += len(shadows); VIN_STATS['float only'] += sum(1 for s in shadows if 'sint' not in s and 'uint' not in s)
            return {k: refl[k] for k in ('pull', 'vin') if k in refl}, cut
    MTL.compile('mac', msl, os.path.join(out, key + '_b.metallib'))
    if why: PULL_STATS['unit failed'] += 1; print(f'[RED] {key}: its libraries do not build from one compile ({why}): each is compiled from its own source', flush=True)   # after the plain source built: a source that does not compile at all is the caller's failure, not the unit's
    refl = pull_finish(key, full, vin_finish(key, full, {'stage': 'vs'}))
    return {k: refl[k] for k in ('pull', 'vin') if k in refl}, False

def ps_libraries(key, msl, full, bindings):
    """A pixel shader's libraries, cut from one compile through the MSL-keyed Metal cache (dmsh_side.cut): <key>_b with its own entry points, <key>_bx with its alpha test and logic
    operation ones (dmsh_side.unit) and its typed-output entry point main0_u (dmsh_entry.fs_uint, which every pixel shader of a known shape has and few pipelines take, with its own alpha
    test and logic operation), <key>_bf with its readable-tile version main0_f (dmsh_fetch.fold; a shader without a rewritable sample only when bf_want.tsv lists it). The readable-tile
    version was a source and a Metal compile of its own (13,666 of Mario Kart 8 Deluxe's 29,651 shaders). msl: the file of its plain source, full; bindings: its reflection's.
    What does not build is left out, and only that: a unit whose readable-tile version does not build is compiled again without it, one that does not build at all gives <key>_b from the
    plain source, each with a red line. A library a shader does not get is removed (a stale one of an earlier build), and the host draws without it. → what the reflection says of them.
    Raises CalledProcessError when the plain source does not compile"""
    def drop(*suffixes):
        for s in suffixes:
            try: os.unlink(os.path.join(out, key + s + '.metallib'))
            except FileNotFoundError: pass
    src, info = dmsh_side.unit(full)
    typed = {'main0_u'} if re.search(r'\bfragment\s+main0_out_u\s+main0_u\s*\(', full) else set()
    side = (set(info['alpha'] + info['logic']) if src else set()) | typed
    folded, fmeta = dmsh_fetch.fold(src or full, bindings, om_only_ok=key in BF_WANT)   # also without side entry points: main0_f then shares the compile with main0 alone
    for unit, fetch in ([(folded, fmeta)] if folded else []) + ([(src or full, None)] if src or typed else []):
        names = ['_b'] + (['_bx'] if side else []) + (['_bf'] if fetch else [])
        groups = ([side] if side else []) + ([{'main0_f'}] if fetch else [])
        u = os.path.join(w, key + '_bu.metal'); open(u, 'w').write(unit)
        try:
            MTL.compile_split('mac', u, [os.path.join(out, key + n + '.metallib') for n in names], lambda lib, groups=groups: dmsh_side.cut(lib, *groups))
        except (subprocess.CalledProcessError, ValueError) as e:
            if fetch: BF_STATS['failed'] += 1; print(f'[RED] {key}: the readable-tile version does not build ({str(e)[:120]}): its draws go without the tile read', flush=True)
            else: SIDE_STATS['failed'] += 1; print(f'[RED] {key}: the side library (alpha test, logic operation, typed outputs) does not build ({str(e)[:120]}): its draws go without the alpha test and logic operation, its typed outputs come from <key>_b', flush=True)
            continue
        finally:
            try: os.unlink(u)
            except FileNotFoundError: pass
        drop(*{'_bx', '_bf'} - set(names))
        SIDE_STATS['generated'] += bool(side - typed); BF_STATS['generated'] += bool(fetch)
        return {**{f: {'entries': info[f]} for f in ('alpha', 'logic') if src and side and info[f]}, **({'fetch': fetch} if fetch else {}), **({'entry_u_side': True} if typed else {})}
    SIDE_STATS['not generated'] += not src
    drop('_bx', '_bf')
    MTL.compile('mac', msl, os.path.join(out, key + '_b.metallib'))
    return {}

pc_of, loc = {}, {}
# shader_map.tsv column 3 → the stage, which is also the prefix of the package file (shader_split.py). The game's 42,847 shaders are these five and nothing else: no hull, domain or geometry
# stage, no ray tracing module, and every one an NVN2 (Switch 2, Ampere) shader. The chain translates only them: a map row of another stage, or a record that is not NVN2 (check_record),
# stops the build, since nothing here would translate it
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
        pc_of.setdefault(c[3], c[4]); loc.setdefault(c[3], (c[0], int(c[1])))
for ln in open(MAP + '.conflicts.tsv'):
    c = ln.rstrip('\n').split('\t')
    if len(c) > 1 and not ln.startswith('#') and not ln.startswith('dm_') and c[0] in pc_of: pc_of[c[0]] = c[1]

def check_record(key, name, rec):
    """Stops the build at a record this chain does not translate: one whose control section is not NVN2's (an NVN1 / Maxwell shader: its code header, VertexA / VertexB programs,
    transform feedback layout and +0x700 fields are not read here any more), an NVN2 ray tracing module (control stage 8), or one whose stage is not its package's (a hull, domain
    or geometry shader under another name: the control's stage byte +0x56 is 5 compute, 6 task, 7 mesh; a graphics stage's is in its SPH word 0, bits 10..13: 1 vertex, 5 pixel).
    Every record is read through rec_of, which calls this, so none is translated or hashed unchecked"""
    ci = rec.find(bytes.fromhex('34127698'), 0x38); ctl = rec[ci:] if ci >= 0 else b''; code = rec[0x38:ci] if ci >= 0 else b''
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
    """Early fragment tests: control +0x5b of an NVN2 pixel shader (dmsh_ampere.early_fragment_tests: the driver's control accessor reads that byte beside the other fragment
    flags). On the console such a shader is tested against depth before it runs; Metal otherwise tests a shader that stores to memory after it, and the late depth differs by an
    ulp from a prepass's along contours, so an EQUAL pass fails there (MacNativeGames ae8f4f4: Tears of the Kingdom's dotted lattice on the Great Sky Island; 1,677 of its 13,646
    pixel shaders set it, none writes depth). A shader whose SPH (the first 0x80 bytes of the code) writes depth (word 19 bit 1, OmapDepth) is left as it is: tested early, the
    depth it computes would not be the one tested"""
    return dmsh_ampere.early_fragment_tests(ctl) and len(code) >= 0x50 and not struct.unpack_from('<I', code, 19 * 4)[0] & 2

def ps_omap(code):
    """A pixel shader's output map: SPH word 18 (of NVN2's 0x80-byte SPH, at the start of the job's code; Ryujinx ShaderHeader OmapTargets),
    4 bits per colour output, R G B A from the low bit. The GPU writes only these components of a bound target and leaves the others as they were; the translator stores only
    them, and Metal writes the whole attachment under its write mask, so the host masks each target with its nibble (nvn_mtl.m). Lichii (a Switch 2 emulator) traced a band of
    NaN above Ocarina of Time's horizon to the missing per-component mask (their commit 79c1516; Ocarina's agl_common depth pass has map 0x10f: output 0 RGBA, output 2 R only)"""
    return struct.unpack_from('<I', code, 18 * 4)[0] if len(code) >= 0x50 else None

NVN2_STAGE_INDEX = {'vs': 0, 'ms': 1, 'ps': 4}   # NVN2 graphics stage indices of the stages the game has (PRAGMATA's banks: vertex c[25], mesh c[26], pixel c[29]; hull shares 1, domain is 2, geometry 3)

def job_for(key, name, rec, w):
    ci = rec.find(bytes.fromhex('34127698'), 0x38); code = rec[0x38:ci]; stage = name[:2]
    extra = {}
    # NVN2 (Switch 2), the only form (check_record): Ampere code (a 0x80-byte SPH for the graphics stages, then the instructions, control +0x38 long), which the translator's
    # Ampere frontend reads (job arch sm86); its c1 segment lies between control +0x40 and +0x44; early fragment tests are at +0x5b (early_fragment_tests)
    c1 = dmsh_ampere.c1_segment(rec[ci:], code)
    code = code[:dmsh_ampere.inst_len(rec[ci:])]
    extra['arch'] = 'sm86'
    # compute lays out its driver constants in c[0] per shader: the control section gives its push constants' words (+0xa9), counts its uniform buffer, storage
    # buffer, texture, separate texture and separate sampler entries (+0xb1, +0xb2, +0xb3, +0xb4, +0xb5; images, +0xb6, come last) and gives the size (+0xba);
    # the translator maps them to c[24] and NVN's fixed tables (InstEmitAmpere.cs AMapC0). Graphics keeps NVN's tables.
    # +0xaa counts the words of push block 1 (NVN2's own accessor reads +0xa9 + block): what the game pushes there (PRAGMATA: {address, size} buffer entries, or 8-byte handles),
    # laid out right after push block 0, before the uniform entries; the translator maps it to c[24] + DMSH_PUSH_SIZE (dmsh_abi.h DMSH_PUSH_BLOCKS). With it every one of
    # PRAGMATA's 6,856 compute shaders adds up to its size (+0xba); Donkey Kong Bananza's and Star Fox's sizes add up without it, as +0xaa is 0 there
    if stage == 'cs': extra['nvn2_cs'] = [rec[ci + 0xb1], rec[ci + 0xb2], rec[ci + 0xb3], struct.unpack_from('<H', rec, ci + 0xba)[0], rec[ci + 0xa9] * 4, rec[ci + 0xb4], rec[ci + 0xb5], rec[ci + 0xaa] * 4]
    # a graphics stage reads the buffer entries the game pushes into its push block 1 + stage index from a bank of its own, c[25 + stage index]; the translator maps
    # that bank onto the block in c[24] (dmsh_abi.h DMSH_NVN2_STAGE_BANK). A task shader's bank has not been seen
    if stage in NVN2_STAGE_INDEX: extra['stage_bank'] = abi.NVN2_STAGE_BANK + NVN2_STAGE_INDEX[stage]
    if stage in ('ts', 'ms'):
        # A task or mesh shader: translated as compute, its SPH taken off and its stage memory in shared memory, then wrapped as the Metal stage function (dmsh_meshstage.py).
        # Graphics stages' c[0] and c[2] tables stay: the translator reads their addresses out of the code as for a vertex shader
        mesh = dmsh_meshstage.info(rec[ci:], rec[0x38:ci])
        lay, size = dmsh_meshstage.memory(mesh)
        if dmsh_meshstage.own_shared(code[0x80:]) or size > dmsh_meshstage.METAL_MAX_SHARED: mesh['refused'] = 'shared memory of its own, or stage memory past Metal\'s limit'
        extra.update(mesh_stage=mesh, stage_memory=lay, local=[mesh['threads'], 1, 1], shared=size); code = code[0x80:]; stage = 'cs'
    if stage == 'ps' and early_fragment_tests(rec[ci:], code): extra['early_fragment_tests'] = True
    cp = os.path.join(w, key + '.bin'); open(cp, 'wb').write(code)
    j = {'key': key, 'stage': stage, 'code': cp, 'tex': {}, 'img': {}, 'attr': {}, 'fout': {}, 'c1': c1.hex(), **extra,
         'debug': os.environ.get('XLATE_DEBUG', '1') != '0', 'reduced': os.environ.get('XLATE_REDUCED', '1') == '1'}   # translation options (see dmsh_xlate/Program.cs); relaxed precision is on by default (GPU float last-bit error does not enter game state, whole frame −26% on the second Mac)   # c1: used by the translator to read indirect jump tables
    if extra.get('mesh_stage'): return j, c1   # its threads and memory come from its control section (above)
    if key not in FACTS: j['no_facts'] = True; return j, c1   # not in the answer table: queries use defaults (listed separately in the summary)
    j.update(FACTS[key])
    return j, c1

RULES = []   # MSL rewrite rules attached by shader name (function, applicability test); the former dark-range remap toe has been removed (back to the original Switch brightness)
# CODE_FP hashes the code form of this file and its helper modules (dmsh_chain.code_form): editing a comment, a docstring or the layout does not change any shader's key.
# The library ABI enters as its values (dmsh_abi.VALUES), so a comment in dmsh_abi.h does not change a key either.
# It covers what runs only when a shader misses the cache, since a hit restores the output and skips it: the MSL rewrites (dmsh_rewrite.py), the reflection (dmsh_reflect.py) and the entry points (dmsh_entry.py) too.
# dmsh_side.py is in it: dmsh_entry's fs_uint and fs_dual write main0's shared body (shared_body).
# dmsh_fetch.py is not, but for screen_varyings (raster_facts), nor are the other side libraries' modules: they are made again from the restored MSL on every build (ps_libraries;
# test_dmsh_code_fp.py checks this list).
CODE_FP = hashlib.sha256(source_without_rule_bodies() + b''.join(b'\0' + dmsh_chain.code_form(open(m.__file__, 'r').read()) for m in (dmsh_chain, dmsh_atomic, dmsh_wide, dmsh_meshstage, dmsh_rewrite, dmsh_reflect, dmsh_entry, dmsh_ampere, dmsh_side, abi))
                         + b'\0' + inspect.getsource(dmsh_fetch.screen_varyings).encode()
                         + b'\0abi\0' + repr(sorted(abi.VALUES.items())).encode()).hexdigest()
TOOL_FP = tool_fingerprint().encode()

# Both pair tables feed pipelines, so both must feed padding, build order and shader cache keys.
def load_pairs(game, package):
    pairs = collections.defaultdict(set)
    for table in ('shader_pairs.tsv', 'pso_pairs.tsv'):
        path = str(game.table(table))
        if table == 'pso_pairs.tsv' and not os.path.isfile(path):
            path = os.path.join(os.path.dirname(os.path.abspath(package)), table)   # the split's copy for profiles predating this table
        if table == 'pso_pairs.tsv' and not os.path.isfile(path): continue
        with open(path) as f:
            for ln in f:
                if ln.strip() and not ln.startswith('#'): v, p = ln.split(); pairs[v].add(p)
    return pairs

PAIRS = load_pairs(GAME, dm_dir)

def build_keys(want, locations, pairs):
    """A partial vertex build needs its available paired pixel shaders before padding; a full build keeps every shader."""
    return sorted(set(want) | {ps for vs in want for ps in pairs.get(vs, ()) if ps in locations}) if want else sorted(locations)

def partner_inputs(key):
    """The user(locnN) locations the pixel shaders paired with a vertex shader read (vs_pad); pairs whose pixel MSL cannot be read are skipped."""
    want = set()
    for ps in PAIRS.get(key, ()):
        data = output(ps + '_b.metal')   # finished before this shader (fin's phases), so staged in the cache by now
        if data is not None: want |= {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', data.decode())}
    return want

def partner_reads(key):
    """{location: component mask} the pixel shaders paired with a mesh shader read (dmsh_meshstage.to_stage's reads), from their SPH input maps (dmsh_meshstage.pixel_reads), None without
    pairs. A location a partner's MSL declares that its input map does not name (a record not found, a fixed-function input the translator moved to a user location) counts as all four."""
    if not PAIRS.get(key): return None
    reads = collections.defaultdict(int)
    for ps in PAIRS[key]:
        got = {}
        if ps in loc:
            rec = rec_of(ps)[1]; ci = rec.find(bytes.fromhex('34127698'), 0x38); code = rec[0x38:ci]
            got = dmsh_meshstage.pixel_reads(code[:0x80])   # the Ampere SPH leads the code
        data = output(ps + '_b.metal')
        for n in ({int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', data.decode())} if data is not None else set()) - got.keys(): got[n] = 0xf
        for n, m in got.items(): reads[n] |= m
    return dict(reads)

def vs_pad(key, src):
    """Give a vertex shader the outputs its paired pixel shaders read but it does not write, with the value (0,0,0,1).
    Maxwell attributes are untyped 32-bit slots, and a pixel shader reading a location the vertex shader did not write only gets a default; Ryujinx also fills them with (0,0,0,1) when it links stages. Here every shader is translated
    offline on its own without knowing what the next stage reads, and Metal requires every pixel shader input to be a vertex output: 331 of 9043 pairs failed to link and their draws were skipped
    (over 10,000 "Fragment input(s) user(locnN) ... not written by vertex shader" in one iPhone session after the opening course). Returns (new source, padded locations); pairs whose pixel MSL cannot be read are skipped."""
    miss = sorted(partner_inputs(key) - {int(x) for x in re.findall(r'\[\[user\(locn(\d+)\)', src)})
    i = src.find('struct main0_out\n{\n'); j = src.find('\n};', i); k = src.find('main0_out out = {};')
    if not miss or i < 0 or j < 0 or k < 0: return src, []
    src = src[:j] + ''.join(f'\n    float4 dm_pad{l} [[user(locn{l})]];' for l in miss) + src[j:]
    k = src.find('main0_out out = {};') + len('main0_out out = {};')
    return src[:k] + ''.join(f'\n    out.dm_pad{l} = float4(0.0, 0.0, 0.0, 1.0);' for l in miss) + src[k:], miss

SG_OPS = {333: 'true', 334: 'pred', 335: 'pred', 336: 'true', 337: 'value', 338: 'value', 339: 'ballot', 345: 'value', 346: 'value', 347: 'value', 348: 'value'}   # OpGroupNonUniform* → its one-lane value
SG_BUILTINS = {36: 'one', 41: 'zero', 4416: 'one4', 4417: 'one4', 4418: 'zero4', 4419: 'one4', 4420: 'zero4'}   # SubgroupSize, SubgroupLocalInvocationId, Eq / Ge / Gt / Le / Lt masks
def spv_single_lane(path):
    """Metal has no SIMD-group functions in vertex functions, and SPIRV-Cross's subgroup emulation covers only Elect. Monster Hunter Rise's vertex shaders (about 4%) scalarize uniform loads:
    ballot the active lanes, shuffle the first lane's value to all, and take a fast path when all agree. With a subgroup of one lane that is exact: the ballot is the lane's own bit, shuffles and
    broadcasts return the lane's own value, all / any are the predicate, all-equal and elect are true, the lane ID is 0, the size 1 and the masks follow. The SPIR-V is rewritten that way
    (the builtin variables, their decorations and the subgroup capabilities removed). → True when rewritten, False when there is nothing to do or an operation it does not cover"""
    with open(path, 'rb') as f: d = f.read()
    words = list(struct.unpack(f'<{len(d) // 4}I', d)); hdr, ins, i = words[:5], [], 5
    while i < len(words): n = words[i] >> 16; ins.append(words[i:i + n]); i += n
    op = lambda x: x[0] & 0xffff
    if not any(op(x) in SG_OPS or (op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS) for x in ins): return False
    if any(340 <= op(x) <= 366 and op(x) not in SG_OPS for x in ins): return False   # other subgroup operations (bit counts, arithmetic): not covered
    bvar = {x[1]: SG_BUILTINS[x[3]] for x in ins if op(x) == 71 and len(x) > 3 and x[2] == 11 and x[3] in SG_BUILTINS}
    # A mask component can be loaded through an input pointer (Xenoblade Chronicles 3's prefix ballot), rather than loading the whole vector first. Keep its index and extract
    # from the constant vector at the load; removing only the builtin variable would leave a dangling pointer. Other access-chain forms remain unsupported.
    bptr = {}
    for x in ins:
        if op(x) in (65, 66) and len(x) > 3 and x[3] in bvar:
            if len(x) != 5 or not bvar[x[3]].endswith('4'): return False
            bptr[x[2]] = (bvar[x[3]], x[4])
    if any(op(x) in (65, 66) and len(x) > 3 and x[3] in bptr for x in ins): return False
    uint = next((x[1] for x in ins if op(x) == 21 and x[2:4] == [32, 0]), None); boolt = next((x[1] for x in ins if op(x) == 20), None)
    uvec4 = next((x[1] for x in ins if op(x) == 23 and x[2] == uint and x[3] == 4), None)
    # A type the module lacks is declared with the constants (PRAGMATA's domain shaders 859225c91a5fe2b2 and f1ad8f78c6c1551e read only the lane ID and have no uvec4)
    bound = hdr[3]; c = {}; types = []
    if uint is None: uint = bound; bound += 1; types.append([21 | 4 << 16, uint, 32, 0])
    if boolt is None: boolt = bound; bound += 1; types.append([20 | 2 << 16, boolt])
    if uvec4 is None: uvec4 = bound; bound += 1; types.append([23 | 4 << 16, uvec4, uint, 4])
    for nm in ('zero', 'one', 'zero4', 'one4', 'true'): c[nm] = bound; bound += 1
    consts = types + [[43 | 4 << 16, uint, c['zero'], 0], [43 | 4 << 16, uint, c['one'], 1], [44 | 7 << 16, uvec4, c['zero4'], c['zero'], c['zero'], c['zero'], c['zero']],
              [44 | 7 << 16, uvec4, c['one4'], c['one'], c['zero'], c['zero'], c['zero']], [41 | 3 << 16, boolt, c['true']]]
    out, placed = [], False
    for x in ins:
        o = op(x)
        if o == 17 and x[1] in (61, 62, 63, 64, 65, 66, 67, 68, 4423, 4431): continue   # GroupNonUniform* / SubgroupBallotKHR / SubgroupVoteKHR capabilities
        if o == 10 and (b'ballot' in struct.pack(f'<{len(x) - 1}I', *x[1:]) or b'subgroup' in struct.pack(f'<{len(x) - 1}I', *x[1:])): continue
        if o in (71, 5) and x[1] in bvar: continue   # the builtin's decoration and name
        if o == 59 and x[2] in bvar: continue        # the builtin variable itself
        if o == 15: x = _ep_drop(x, bvar)
        if o == 54 and not placed: out += consts; placed = True
        if o == 61 and x[3] in bvar: out.append([83 | 4 << 16, x[1], x[2], c[bvar[x[3]]]]); continue   # a load of a builtin → its constant
        if o in (65, 66) and x[2] in bptr: continue
        if o == 61 and x[3] in bptr:
            nm, index = bptr[x[3]]
            out.append([77 | 5 << 16, x[1], x[2], c[nm], index]); continue   # OpVectorExtractDynamic, including a constant index
        if o in SG_OPS:
            kind = SG_OPS[o]
            if kind == 'ballot': out.append([169 | 6 << 16, x[1], x[2], x[4], c['one4'], c['zero4']]); continue   # OpSelect(pred, own bit, 0)
            out.append([83 | 4 << 16, x[1], x[2], c['true'] if kind == 'true' else x[4]]); continue
        out.append(x)
    if not placed: return False
    hdr[3] = bound
    flat = hdr + [v for x in out for v in x]
    with open(path, 'wb') as f: f.write(struct.pack(f'<{len(flat)}I', *flat))
    return True
def _ep_drop(x, ids):   # OpEntryPoint without the given interface IDs: model, function, the name (a nul-terminated string in whole words), then the interface list
    k = 3
    while k < len(x):
        k += 1
        if any((x[k - 1] >> (8 * b)) & 0xff == 0 for b in range(4)): break   # the word holding the string's nul ends the name
    y = x[:k] + [v for v in x[k:] if v not in ids]
    y[0] = len(y) << 16 | (x[0] & 0xffff)
    return y

def finish(key, w, j, c1):
    name, k = loc[key]
    msl = os.path.join(w, key + '.metal'); lib = os.path.join(out, key + '_b.metallib')
    if j['stage'] == 'vs': spv_single_lane(os.path.join(w, key + '.spv'))   # one-lane subgroups (see spv_single_lane)
    r = subprocess.run([SPIRV_CROSS, *XC, os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
    if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
    src = open(msl).read()
    if j['stage'] == 'vs':   # second pass: re-translate with inputs that are only used as integers reported as uint (see int_inputs)
        extra = {l: t for l, t in dmsh_entry.int_inputs(src).items() if l not in j['attr']}
        if extra:
            j['attr'].update(extra); jp = os.path.join(w, f'job1_{key}.json'); json.dump([j], open(jp, 'w'))
            rx = subprocess.run([XLATE, jp, w], capture_output=True, text=True)
            if rx.returncode or not os.path.exists(os.path.join(w, key + '.spv')): return 'translation failed', ('integer-input retranslation: ' + rx.stderr.strip())[:160], None
            spv_single_lane(os.path.join(w, key + '.spv'))
            r = subprocess.run([SPIRV_CROSS, *XC, os.path.join(w, key + '.spv'), '--output', msl], capture_output=True, text=True)
            if r.returncode: return 'MSL failed', r.stderr.strip()[:160], None
            src = open(msl).read()
    try: src = dmsh_atomic.check_msl(src)
    except ValueError as e: return 'image atomic MSL failed', str(e), None
    if j['stage'] == 'ps':
        try: src = dmsh_rewrite.per_vertex(src)   # LDTRAM's per-vertex attribute reads as vertex_value (see per_vertex)
        except ValueError as e: return 'per-vertex input rewrite failed', str(e)[:160], None
    src = dmsh_rewrite.precise(src)   # min / max / clamp with Maxwell's NaN results (see precise)
    src = dmsh_rewrite.bit0_mask(src)   # a tested bit 0's 0 / -1 mask without the compare, which Apple's GPU compiler can fail to encode a jump on (see bit0_mask)
    try: src = dmsh_rewrite.select_guards(src)   # bounded uniform reads as a load and a select, as the translator means them, not a branch for each (see dmsh_rewrite.select_guards)
    except ValueError as e: return 'uniform bounds rewrite failed', str(e)[:160], None
    src = dmsh_rewrite.mul_extended(src)   # scalar extended products without the optnone wrapper (see mul_extended)
    src = dmsh_rewrite.float_add_msl(src)   # a float add's compare-and-swap loop as one native atomic add (see float_add_msl)
    try: src = dmsh_rewrite.gather_offsets(src)   # four gathers instead of a wrapper's loop over their offsets (see gather_offsets)
    except ValueError as e: return 'gather offsets rewrite failed', str(e)[:160], None
    try: src = dmsh_rewrite.hmma_msl(src)   # Ampere HMMA through simdgroup_matrix (see hmma_msl)
    except ValueError as e: return 'HMMA rewrite failed', str(e)[:160], None
    try: src = dmsh_rewrite.shared_store_msl(src)   # shared memory byte and half-word stores as Metal byte stores (see shared_store_msl)
    except ValueError as e: return 'shared store rewrite failed', str(e)[:160], None
    try: src = dmsh_reflect.pool_arrays(src)   # handles traced to no slot read the pools the replay keeps (see pool_arrays)
    except ValueError as e: return 'pool rewrite failed', str(e)[:160], None
    src = dmsh_wide.wide_loads(src)   # storage words the translator split, read whole again (see dmsh_wide.py)
    res = json.load(open(os.path.join(w, key + '.res.json')))
    # The MSL s<n> number indexes the buffers in binding order, as in dmsh_reflect.reflect. The raw pool table is host data.
    read_only = {i for i, b in enumerate(sorted(res['sbufs'], key=lambda b: b['binding']))
                 if not b['flags'] & 1 and b['cb_slot'] != abi.GMEM_MAP_CB_SLOT}   # BufferUsageFlags.Write (includes atomics)
    src = dmsh_wide.constant_reads(src, read_only, dmsh_chain.storage_constant_mode())
    src = dmsh_rewrite.tg_fence(src, kernel=j['stage'] == 'cs' and not j.get('mesh_stage'))   # warp-synchronous shared memory: the translator's dmsh_tg_fence() as a simdgroup-scope fence, a compute kernel's in uniform control flow a SIMD-group barrier (see tg_fence)
    src = dmsh_rewrite.border_emu(src)   # border colours Metal lacks (see border_emu)
    if j['stage'] == 'ps': src = dmsh_rewrite.frag_z_snap(src)[0]   # gl_FragCoord.z against a depth of its own surface (see frag_z_snap)
    try: src = dmsh_rewrite.uniform_tests(src)   # a uniform word tested against constants read by a volatile load per test, so the front end folds no switch (see uniform_tests)
    except ValueError as e: return 'uniform tests rewrite failed', str(e)[:160], None
    blend0 = 0
    if j['stage'] == 'ps': src, blend0 = dmsh_rewrite.blend_zero(src)   # a zero blend weight makes 0 of a NaN colour, as NVIDIA's blender does; before the entry points that share main0's body (see blend_zero)
    open(msl, 'w').write(src)
    first, spill, loop_sites = [], 0, []
    mesh = j.get('mesh_stage')
    if mesh:   # the kernel as the Metal object or mesh function (dmsh_meshstage.py)
        if mesh.get('refused'): return 'mesh stage failed', mesh['refused'], None
        # outputs its paired pixel shaders read but its SPHs do not enable get (0,0,0,1), as vs_pad gives a vertex shader's (Ocarina of Time's foliage meshes); each output as wide as they read
        try: src = dmsh_meshstage.to_stage(src, mesh, partner_reads(key)); open(msl, 'w').write(src)
        except ValueError as e: return 'mesh stage failed', str(e)[:160], None
    elif j['stage'] == 'cs':
        src, first = dmsh_rewrite.first_store(src, j.get('local')); open(msl, 'w').write(src)   # the first invocation's store, for the host (see first_store)
        try: src, spill = dmsh_rewrite.tg_spill(src)   # shared memory past Metal's 32 KB in a device buffer (see tg_spill)
        except ValueError as e: return 'shared memory rewrite failed', str(e)[:160], None
        if spill: open(msl, 'w').write(src)
        try: src, loop_sites = dmsh_rewrite.compute_loops(src); open(msl, 'w').write(src)
        except ValueError as e: return 'compute loop containment failed', str(e)[:160], None
        src = dmsh_rewrite.max_threads(src, j.get('local')); open(msl, 'w').write(src)   # the local size as the kernel's thread limit, so its pipeline allows the dispatch (see max_threads)
    if 'dmsh_raw_find' in src:
        src = dmsh_rewrite.raw_noinline(src, mesh['kind'] if mesh else j['stage']); open(msl, 'w').write(src)   # native stages were translated as cs; task and mesh searches stay inline (see raw_noinline)
    if 'spvSubgroupShuffle(' in src:
        folded = dmsh_rewrite.shuffle_literal(src)   # a shuffle of a literal is the literal (see shuffle_literal)
        if folded != src: src = folded; open(msl, 'w').write(src)
    pad = []
    if j['stage'] == 'vs':   # outputs start as (0,0,0,1) (see vs_init); the base instance from Metal (see vs_base_instance)
        # No vertex shader of the game writes the layer (render_target_array_index: none of its 15,850), so neither this chain nor the host has a layered vertex path: one that did
        # would need its point size taken out and an input primitive class on its pipelines, and fails here instead of drawing every layer into the first
        if dmsh_entry.writes_layer(src): return 'layer output', 'the vertex shader writes the layer (render_target_array_index), which this chain and the host have no path for', None
        src = dmsh_entry.vs_base_instance(dmsh_entry.vs_init(src)); open(msl, 'w').write(src)
    if j['stage'] == 'vs' and PAIRS.get(key):   # outputs the pixel shader reads but the vertex shader does not write get (0,0,0,1) (see vs_pad)
        src, pad = vs_pad(key, src)
        if pad: open(msl, 'w').write(src)
    discard = False
    if j['stage'] == 'vs':   # the two depth-convention entry points (see vs_depth); the analysis below uses the source with only main0
        src, full = dmsh_entry.vs_depth(src)
        discard = dmsh_entry.vs_writes(src, dmsh_reflect.reflect(key, 'vs', src, res))
        if discard: full += dmsh_entry.vs_discard(src)
        open(msl, 'w').write(full)
    fu = dmsh_entry.fs_uint(src) if j['stage'] == 'ps' else None   # the typed-output entry point (see fs_uint); the analysis below uses the source with only main0
    fd = dmsh_entry.fs_dual(src) if j['stage'] == 'ps' and key in DUAL_WANT else None   # the dual-source entry point (see fs_dual), from the source with only main0
    shared = dmsh_side.shared_body(src) if fu or fd else None   # they call main0's body; fs_uint and fs_dual give none for a main0 whose shape it does not know
    if shared: plain = src; open(msl, 'w').write(shared + (fu or '') + (fd or ''))
    else: fu = fd = None
    try: binds = dmsh_reflect.reflect(key, mesh['kind'] if mesh else j['stage'], src, res)   # before the compile: a pixel shader's readable-tile version needs its texture slots (ps_libraries)
    except ValueError as e: return 'reflection failed', str(e)[:160], None
    lost = []
    def compile_libraries():   # MSL identical to one compiled before takes that library (dmsh_mtlcache.py)
        if j['stage'] == 'ps': return ps_libraries(key, msl, open(msl).read(), binds), False
        if j['stage'] == 'vs': return vs_libraries(key, msl, open(msl).read())
        MTL.compile('mac', msl, lib)
        return {}, False
    try: side, cut = compile_libraries()
    except subprocess.CalledProcessError as e:
        if not fu and not fd: return 'metal failed', str(e)[:160], None
        lost = [name for name, extra in (('integer output', fu), ('dual source', fd)) if extra]   # kept in the reflection, as a cache hit compiles nothing: every build reports it (shader_regressions.report_losses)
        open(msl, 'w').write(plain); fu = fd = None   # the typed-output and dual-source entry points are extras: a library that does not build with them is built without
        try: side, cut = compile_libraries()
        except subprocess.CalledProcessError as e: return 'metal failed', str(e)[:160], None
    clone(msl, os.path.join(out, key + '_b.metal'))
    entry = re.search(r'(vertex|fragment|kernel)\s+\S+\s+(\w+)\s*\(', src)
    dr = dmsh_reflect.drv_reads(src)
    tgf = dmsh_rewrite.tg_fence_counts(src)
    refl = {'key': key + '_b', 'route': 'B', 'chain': CHAIN, 'dm_ctl': key, 'source': f'{name}#{k}', 'pc_paired': pc_of[key], 'stage': mesh['kind'] if mesh else j['stage'],
            **({'mesh_stage': {'kind': mesh['kind'], 'threads': mesh['threads'], 'payload': mesh['payload'], 'task': mesh.get('task', True)}, **({'entry_z': 'main0_z'} if mesh['kind'] == 'ms' else {}),
               **({'entry_object': 'main0_o'} if mesh['kind'] == 'ms' and not mesh.get('task', True) else {})} if mesh else {}),
            **({'mesh_spill': 1} if mesh and mesh.get('spill') else {}),   # the mesh stage's array is a device slice: the host binds the scratch buffer at DMSH_MESH_SCRATCH_SLOT (dmsh_meshstage.py to_stage)
            'entry': entry.group(2) if entry else 'main0', 'msl_entry_kind': entry.group(1) if entry else None, **({'entry_z': 'main0_z'} if j['stage'] == 'vs' else {}),
            **({'entry_discard': 'main0_discard'} if discard else {}),
            **({'entry_u': 'main0_u', **{f'entry_u_{k}': True for k, v in dmsh_entry.typed_ops(fu).items() if v}} if fu else {}), **({'entry_d': 'main0_d'} if fd else {}), **({'blend_zero': True} if blend0 else {}), **({'entries_lost': lost} if lost else {}),
            **({'first_store': [{'reg': next(b['reg'] for b in binds if b['msl_name'] == n), 'word': w, 'value': v} for n, w, v in first]} if first else {}),
            **({'tg_spill': spill} if spill else {}),
            **({'tg_fence': dict(zip(('fence', 'barrier'), tgf))} if any(tgf) else {}),   # the build's report (tg_fence)
            **({'loop_sites': loop_sites} if j['stage'] == 'cs' and loop_sites else {}),
            'threadgroup_size': j.get('local'), 'c1': c1.hex(), 'notes': res['notes'] + (['not in the answer table, queries use defaults'] if j.get('no_facts') else []), 'drv_reads': dr, 'drv_unfillable': dmsh_reflect.unfillable(dr, mesh['kind'] if mesh else j['stage']),
            'bindings': binds,
            'vertex_inputs': [{'location': int(m.group(3)), 'msl_type': m.group(1)} for m in dmsh_reflect.attributes(src)],
            'raster': {**dmsh_reflect.raster_facts(j['stage'], src), **({'omap': ps_omap(open(j['code'], 'rb').read())} if j['stage'] == 'ps' else {})}, 'vs_pad': pad}
    refl.update(side)   # the alpha test and logic operation entry points and the readable-tile version (ps_libraries); the pulling and shadow-input libraries (vs_libraries)
    write_refl(key, refl, cut)
    return 'ok', '', refl

keys = build_keys(want, loc, PAIRS)
if want: want = set(keys)   # dependency outputs are cleaned too, so a failed pixel build cannot leave old MSL for padding
NJ = int(os.environ.get('J', '8'))
PROCS = int(os.environ.get('DMSH_PROCS', NJ))   # processes that finish shaders (fin_worker); 0: NJ threads of this one
BATCH = 4 * NJ   # shaders translated and finished together: the next batch is translated while this one is finished, and a shader's working files go when it is (fin).
# Minecraft Dungeons II, 15,826 shaders: at most 3,279 files under the build folder with batches of 256 removed whole, 68,230 before batches (Octopath Traveler 0's stopped build)
# A full build starts from no shader outputs, a partial one from none of the keys it builds, so what a run leaves is what it built: every output is restored from the cache anyway.
# The chain version is the same whatever the profile's tables, so a key that left the map, a shader that no longer builds or a variant no longer made would keep an old library.
# The stamp goes too, and a full build writes it at the end; a partial build keeps it only while it names the profile's current inputs.
# A full build also removes the image (as the packing script does before it): a stopped build leaves no library, rather than one with old shaders for those it had not reached. A partial build
# builds on the image (OLD_IMAGE), less the outputs of the keys it builds, which publish_library leaves out
OUT_KEY = re.compile(r'([0-9a-f]{16})_b')
def owned_output(key): return key in want
STAMP_BEFORE = shader_lib.read_stamp(out).get('inputs')
OLD_IMAGE = None
if not want:
    shutil.rmtree(out, ignore_errors=True); shader_lib.remove_image(out)
elif os.path.isfile(shader_lib.image_of(out)): OLD_IMAGE = romfs.Path(shader_lib.image_of(out))
os.makedirs(out, exist_ok=True)
for _n in os.listdir(out):   # a partial build over a folder (an older chain's, or one a stopped build left)
    _m = OUT_KEY.match(_n)
    if _m and owned_output(_m.group(1)): os.unlink(os.path.join(out, _n))
if STAMP_BEFORE != INPUTS and os.path.exists(os.path.join(out, dmsh_chain.STAMP)): os.unlink(os.path.join(out, dmsh_chain.STAMP))
STORE.delete_prefix(STAGE + '/')   # what a stopped build of this folder staged

def sweep_work(root):
    """The working folders (dmsh_*) a build left that is no longer running: each holds the process ID of its build (pid), one from before that is taken after a day.
    Octopath Traveler 0's build, stopped in its compile, left 38,376 files there, and every later build made another folder beside it"""
    for d in (os.path.join(root, n) for n in os.listdir(root) if n.startswith('dmsh_')) if os.path.isdir(root) else ():
        try:
            with open(os.path.join(d, 'pid')) as f: pid = int(f.read())
            try: os.kill(pid, 0); continue   # its build runs
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
# first take the libraries earlier chain versions left on disk: the b-*/ and b-*.img beside the output and the roots DMSH_PREFILL lists
for _root in [cache_root] + [x for x in os.environ.get('DMSH_PREFILL', '').split(os.pathsep) if x]:
    for _b in sorted(os.listdir(_root)) if os.path.isdir(_root) else []:
        if _b.startswith('b-') and os.path.realpath(shader_lib.folder_of(os.path.join(_root, _b))) != os.path.realpath(out):
            for _ln in MTL.fill(os.path.join(_root, _b)): print(_ln, flush=True)

# Progress for the packing script (DMSH_PROGRESS: a small JSON file it reads, build_app.py ShaderWatch), written at most every half second
PROGRESS = os.environ.get('DMSH_PROGRESS')
_prog = {'t': 0.0, 'lock': threading.Lock()}
def progress(step, done, total, force=False, **extra):
    if not PROGRESS: return
    with _prog['lock']:
        now = time.time()
        if not force and now - _prog['t'] < 0.5: return
        _prog['t'] = now
        try:
            with open(PROGRESS + '.tmp', 'w') as f: json.dump({'step': step, 'done': done, 'total': total, **extra}, f)
            os.replace(PROGRESS + '.tmp', PROGRESS)
        except OSError: pass

packs, jobs, c1s = {}, {}, {}
packs_lock = threading.Lock()
digests, rule_fps, cache_reasons, cache_hits = {}, {}, {}, set()
def rec_of(key):
    """→ (package name, the shader's record), found by its key and read from the package each time, not kept (see record_index). Not by the map row's index: a package from another
    shader_split.py run than the map translated every row after a dropped or added one from its neighbour's bytes (issue #1841: Ocarina of Time's present pass ran a terrain pixel
    shader and showed black). Every key is looked up before the first job starts, so a package that does not hold one stops the build here"""
    name = loc[key][0]
    with packs_lock:
        if name not in packs: packs[name] = record_index(os.path.join(dm_dir, name))
    fd, recs = packs[name]
    if key not in recs:
        old = ' (its records carry no keys: it was split before they did)' if recs and set(recs) == {'0' * 16} else ''
        sys.exit(f'[RED] {name} holds no record of shader {key}, which the shader map names: the package and the map come from different shader_split.py runs{old}; '
                 f'split the shaders again (the packing script does when {dm_dir} is removed)')
    o, e = recs[key]
    rec = os.pread(fd, e - o, o); check_record(key, name, rec)
    return name, rec
for key in keys: digests[key] = shader_key(key, *rec_of(key))
def plain_digest(k):   # a paired pixel shader's key, also when it is not built this time
    if k in digests: return digests[k]
    return shader_key(k, *rec_of(k)) if k in loc else 'missing'
for key in keys:
    if PAIRS.get(key): digests[key] = pair_keyed(digests[key], [plain_digest(p) for p in sorted(PAIRS[key])])
for i, key in enumerate(keys):
    name = loc[key][0]
    rule_fps[key] = hashlib.sha256(shader_rule_source(name) + ','.join(sorted(PAIRS.get(key, ()))).encode()).hexdigest()   # the vertex shader must be rebuilt when its pairs change (vs_pad)
    hit, cache_reasons[key] = cache_lookup(key, digests[key], rule_fps[key], CODE_FP)
    if hit: cache_hits.add(key)
    progress('cache check', i + 1, len(keys), cached=len(cache_hits))
NMISS = sum(1 for k in keys if k not in cache_hits)
xerr = {}
def xlate_errors(r):
    for ln in r.stderr.splitlines():
        m = re.match(r'\[RED\] (\w+): (.*)', ln)
        if m: xerr[m.group(1)] = m.group(2)[:160]
def translate(batch):
    """A batch's misses translated into a working folder of its own (removed once the batch is finished) → that folder"""
    wb = tempfile.mkdtemp(prefix='batch_', dir=w)
    miss = [k for k in batch if k not in cache_hits]
    for k in miss: jobs[k], c1s[k] = job_for(k, loc[k][0], rec_of(k)[1], wb)
    def xl(ch):
        jp = os.path.join(wb, f'job_{abs(hash(ch[0]))}.json'); json.dump([jobs[k] for k in ch], open(jp, 'w'))
        return subprocess.run([XLATE, jp, wb], capture_output=True, text=True)
    with cf.ThreadPoolExecutor(NJ) as ex:
        for r in ex.map(xl, [c for c in (miss[i::NJ] for i in range(NJ)) if c]): xlate_errors(r)
    return wb
def restored(key, wb):
    """A cache hit's outputs back on disk while it is finished. When another build pruned them since the look (cache_lookup), the shader is translated now, in the batch's folder: False"""
    if restore_cache(key, digests[key]): return True
    PRE_STATS['hit'] -= 1; PRE_STATS['miss'] += 1; PRE_STATS['pruned meanwhile'] += 1
    jobs[key], c1s[key] = job_for(key, loc[key][0], rec_of(key)[1], wb)
    jp = os.path.join(wb, f'job1_{key}.json'); json.dump([jobs[key]], open(jp, 'w'))
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
                refl.update(side)   # stored without the side-library reflection (publish_cache)
                write_refl(key, refl, cut)
            if refl['stage'] == 'ps': refl.update(ps_libraries(key, os.path.join(out, key + '_b.metal'), src, refl['bindings'])); write_refl(key, refl)   # stored without them (publish_cache)
            return key, 'ok', '', refl
        if key in xerr or not os.path.exists(os.path.join(wb, key + '.spv')): return key, 'translation failed', xerr.get(key, 'no spv'), None
        result = (key,) + finish(key, wb, jobs[key], c1s[key])
        if result[1] == 'ok': publish_cache(key, digests[key], rule_fps[key], CODE_FP)
        return result
    finally:
        commit(key)
        with os.scandir(wb) as it:   # its working files (<key>.bin / .spv / .res.json / .metal, job1_<key>.json)
            for e in it:
                if e.name.startswith((key, 'job1_' + key)) and e.is_file(): os.unlink(e.path)
        finished()
def finished(*_):   # one more shader finished, for the progress
    with _prog['lock']: DONE['n'] += 1; n = DONE['n']
    progress('compiling', n, len(keys), cached=len(cache_hits), misses=NMISS)
# The shaders are finished in worker processes (PROCS of them): fin's own Python, the rules that rewrite the MSL above all, holds the GIL, and with threads one core ran it for all of them
# while the Metal compiles they waited on ran at about a third of the machine (Ocarina of Time, 16 cores: 104% CPU for this process, 4-5 cores busy in all). A worker is a fork of this
# process, made before any thread starts, so it has every table and fingerprint read above; spawn would run this script again, its start removing the outputs. A worker does not open
# the cache: this process makes its calls on the one connection (shader_cache.Served: Apple's SQLite crashes when a fork opens a database, GitHub issue #2354), so its outputs and
# cache entries go into the file as a thread's did; what fin returns and what it counted come back here,
# in the batch's order, so the results, the summary and every file written are those of the threads (DMSH_PROCS=0). The precompiled Metal headers (dmsh_pch.py) are made here before the
# fork: a worker would make its own (three compiles for each target in each worker) in a folder that nothing removes, as a worker process ends without atexit. A worker that dies
# (killed, out of memory) stops the build with one line
COUNTERS = (PRE_STATS, VIN_STATS, PULL_STATS, SIDE_STATS, BF_STATS, MTL.stats)
def worker_start():
    global PROGRESS, STORE
    assert SERVED is not None
    STORE = MTL.store = SERVED.worker()
    PROGRESS = None   # this build writes it, as the workers' shaders finish
def fin_worker(key, wb, job, err):
    """fin in a worker process, given what translate left for the shader in this process (its job and c1, its translator error) → (fin's result, what it added to the counters)"""
    jobs.clear(); c1s.clear(); xerr.clear()
    if job: jobs[key], c1s[key] = job
    if err is not None: xerr[key] = err
    for c in COUNTERS: c.clear()
    return fin(key, wb), [dict(c) for c in COUNTERS]
def submit(batch, wb):   # a translated batch's shaders to finish → their futures
    if not px: return [ex.submit(fin, k, wb) for k in batch]
    fs = [px.submit(fin_worker, k, wb, (jobs.pop(k), c1s.pop(k)) if k in jobs else None, xerr.get(k)) for k in batch]
    for f in fs: f.add_done_callback(finished)
    return fs
def results(fs):   # → what fin returned for them, in their order (a worker's counts added to this process's)
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
    assert SERVED is not None; SERVED.start()
# Two phases: paired vertex shaders read their pixel shaders' MSL (vs_pad), so they go last, in batches of their own, once every shader before them is finished. The next batch is
# translated while this one is finished, and handed out before this one is done, so that no worker waits for the slowest shader of a batch; a batch's working folder goes when it is done
batches = [b[i:i + BATCH] for b in ([k for k in keys if k not in PAIRS], [k for k in keys if k in PAIRS]) for i in range(0, len(b), BATCH)]
res, busy = [], []   # busy: the batches handed out, oldest first: (working folder, futures)
def collect(left):   # the results of the oldest batches handed out, until left are
    while len(busy) > left:
        wb, fs = busy.pop(0); res.extend(results(fs)); shutil.rmtree(wb, ignore_errors=True)
try:
    with cf.ThreadPoolExecutor(1) as tx, cf.ThreadPoolExecutor(NJ) as ex:
        nxt = tx.submit(translate, batches[0]) if batches else None
        for i, batch in enumerate(batches):
            assert nxt is not None
            wb = nxt.result()
            nxt = tx.submit(translate, batches[i + 1]) if i + 1 < len(batches) else None
            if batch[0] in PAIRS and i and batches[i - 1][0] not in PAIRS: collect(0)   # the first paired batch
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
tm = collections.Counter(nn[len('translator: '):] for x in ok for nn in x['notes'] if nn.startswith('translator: '))   # the translator's own messages (dmsh_xlate Log): bindless reads resolved or removed, invalid encodings
for e, n in tm.most_common(12): print(f'  translator {n:6d} in {sum(e in [nn[len("translator: "):] for nn in x["notes"]] for x in ok):5d} shaders  {e}')
shader_regressions.report_losses(ok)
json.dump([{'key': r[0], 'st': r[1], 'err': r[2]} for r in res], open(os.path.join(out, '_b_build.json'), 'w'), ensure_ascii=False)
if not want:   # a full build: the stamp, then the c1 guard and uniform read extent tables from these Mac outputs, into the profile folder (the host takes them only beside this stamp)
    # The per-shader cache entries this build used (cas/shader/<digest>), which the packing script keeps when it prunes the others (prune_shader_cache). Before the stamp, so a stamped folder always has its list
    with open(os.path.join(out, CAS_USED + '.tmp'), 'w') as _f: _f.write(''.join(d + '\n' for d in sorted(set(digests.values()))))
    os.replace(os.path.join(out, CAS_USED + '.tmp'), os.path.join(out, CAS_USED))
# The libraries' images, from what this build staged (and for a partial build the images it builds on): the folders go, and what reads a library from here on reads the image
if not want: dmsh_chain.write_stamp(out, GAME.id, INPUTS)   # just before publishing, so that a build that stops on a full drive leaves no stamp beside a library it could not write
publish_library()
if not want: print(f'library stamp: game {GAME.id}, inputs {INPUTS}', flush=True)
if PROGRESS:
    try: os.unlink(PROGRESS)
    except FileNotFoundError: pass
if not want:
    for _tool, _table in (('c1_guard.py', 'c1_full.tsv'), ('cb_extent.py', 'cb_extent.tsv')):
        _r = subprocess.run([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), _tool), out, str(GAME.table(_table))], capture_output=True, text=True)
        if _r.returncode: sys.exit(f'[RED] {_tool} failed ({_r.returncode}): {_r.stderr.strip()[-500:]}')
        print(f'{_table}: {(_r.stdout.strip().splitlines() or [""])[-1]}', flush=True)
elif STAMP_BEFORE != INPUTS: print(f'no library stamp: the profile\'s inputs changed since the last full build ({STAMP_BEFORE} → {INPUTS}), and a partial build does not rebuild the rest', flush=True)
