#!/usr/bin/env python3
# The pipeline archive made offline: the base of every shader pair as the host's prebuild makes it (nvn_mtl.m pre_desc / pre_pair), and a compute pipeline per
# compute shader, compiled by metal-tt into one Metal 4 archive per GPU architecture for the Mac app's bundle. What the host would leave out is left out; a pair
# whose functions declare constants other than HOST_CONSTANTS, or whose base metal-tt cannot compile, goes to the device (rest_pairs.tsv).
# Output: <arch>.mtl4a, prebuilt.tsv (the host's stamp), rest_pairs.tsv, archive.tsv (for people); the prewarm files there are the packing script's.
# An archive is made again only when its compiler inputs change (key.<arch>); large inputs compile in groups merged with metal-pack
# (docs/reference/shaders.md#bounded-groups-for-the-offline-pipeline-compiler).
import argparse, concurrent.futures, contextlib, filecmp, hashlib, json, os, shutil, struct, subprocess, sys, time
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..', 'tools'))
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import romfs, shader_lib   # metal-tt alone needs files of its own (real_libraries)
import dmsh_abi as abi, dmsh_side
import vol_space

# The constants the host sets (nvn_mtl.m pre_fn, fn_spec): dm_border_off true (almost no draw binds a border twin); dm_blend_off false, so one pulling
# base serves draws of both blend_live values as a hybrid stand-in; dm_mwa3_fc false, as the host's mwa_on leaves it unless DM_NVN_METALWA=1 (issue #3405)
HOST_CONSTANTS = {'dm_border_off': (abi.BORDER_FC, True), 'dm_blend_off': (abi.BLEND_OFF_FC, False), 'dm_mwa3_fc': (abi.MWA_FC, False)}
# A kernel in two passes (dmsh_ndsr.py) gets three compute pipelines: DMSH_SPLIT_FC undefined, false and true (nvn_mtl.m split_pipes)
SPLIT_PASSES = {'split0': False, 'split1': True}

PLATFORM, SDK = 'macos', 'macosx'   # as metal-tt's -macos-version-min takes it

BATCH = 1000   # the largest compiler footprint at this size was 2 GiB on an 8 GB M2
MAX_FAILED = 8   # pipelines of one group metal-tt may fail on before the run counts as failed
COMPUTE_BATCH = 16   # one metal-tt keeps about 2-3 cores busy
JOBS = max(1, (os.cpu_count() or 2) // 2)
COLOUR = ('pixel_format', 'write_mask', 'blending_state', 'alpha_blend_operation', 'rgb_blend_operation', 'destination_alpha_blend_factor', 'destination_rgb_blend_factor',
          'source_alpha_blend_factor', 'source_rgb_blend_factor')


def arch_name(a):
    a = a.strip()
    return a if a.startswith('applegpu_') else f'applegpu_{a}'


def pair_text(paths):
    """shader_pairs.tsv, then pso_pairs.tsv when it exists, as the host reads them"""
    return b''.join(Path(p).read_bytes() for p in paths if p and Path(p).is_file())


def read_pairs(text):
    """as pso_prebuild reads them: in order, without empty lines, comments and repeats"""
    seen, out = set(), []
    for line in text.decode('utf-8').split('\n'):
        if not line or line.startswith('#') or line in seen: continue
        seen.add(line)
        c = line.split('\t')
        if len(c) >= 2: out.append((c[0], c[1]))
    return out


def read_computes(maps, manifests):
    """The prewarm lists' C rows in their order, then the shader maps' compute rows, each once; missing files skipped"""
    out = {}
    for path, kind in [*((p, 'C') for p in manifests or ()), *((p, 'map') for p in maps or ())]:
        if not path or not Path(path).is_file(): continue
        for line in Path(path).read_text(encoding='utf-8').split('\n'):
            c = line.rstrip('\r').split('\t')
            if kind == 'C' and len(c) >= 2 and c[0] == 'C' and c[1]: out.setdefault(c[1], None)
            elif kind == 'map' and len(c) > 3 and c[2] == 'compute' and c[3]: out.setdefault(c[3] + '_b', None)
    return list(out)


def stamp(chain, zero, text):
    """pso_prebuild's stamp: chain version, depth mode, the pair lists' byte length and SHA-256 (16 hex digits)"""
    return f'chain\t{chain}\ndepth\t{"near=0" if zero else "near=-w"}\npairs\t{len(text)}\t{hashlib.sha256(text).hexdigest()[:16]}\n'


def functions(path):
    """Raises ValueError on a file it cannot read"""
    return {k: bool(v) for k, v in constants(path).items()}


def _constant_entries(cnst):
    """a 16-bit count, then per constant its NUL-terminated name, a type byte, a 16-bit index and a byte"""
    n, p, out = struct.unpack_from('<H', cnst, 0)[0], 2, []
    for _ in range(n):
        end = cnst.index(b'\0', p)
        if end + 4 > len(cnst): raise ValueError('a truncated CNST tag')
        out.append((cnst[p:end].decode(), struct.unpack_from('<H', cnst, end + 2)[0]))
        p = end + 1 + 4
    return tuple(out)


def _constant_names(cnst): return tuple(n for n, _ in _constant_entries(cnst))


def split_constant(path, entry):
    return next((n for n, i in constants(path, indices=True).get(entry, ()) if i == abi.SPLIT_FC), None)


def constants(path, indices=False):
    """From the function list (NAME tags) and each function's CNST tag; with indices, (name, index) pairs. Raises ValueError on a file it cannot read"""
    with romfs.open(path, 'rb') as f: b = f.read(1 << 20)
    if b[:4] != b'MTLB' or len(b) < 0x58: raise ValueError(f'{path}: not a Metal library')
    fo, _, po, ps = struct.unpack_from('<QQQQ', b, 0x18)
    def tags(p, end):
        t = {}
        while p + 4 <= end:
            tag = b[p:p + 4]
            p += 4
            if tag == b'ENDT': return t
            n = struct.unpack_from('<H', b, p)[0]
            p += 2
            t[tag] = b[p:p + n]
            p += n
        raise ValueError(f'{path}: unterminated tag list')
    try:
        count = struct.unpack_from('<I', b, fo)[0]
        p = fo + 4
        names = []
        for _ in range(count):   # each entry: its size (counting the size field), then tags
            size = struct.unpack_from('<I', b, p)[0]
            names.append(tags(p + 4, p + size)[b'NAME'].rstrip(b'\0').decode())
            p += size
        consts, p = [], po
        for _ in range(count):   # each entry: its size (not counting the size field), then tags
            size = struct.unpack_from('<I', b, p)[0]
            t = tags(p + 4, p + 4 + size)
            consts.append((_constant_entries if indices else _constant_names)(t[b'CNST']) if b'CNST' in t else ())
            p += 4 + size
        if p != po + ps: raise ValueError(f'{path}: public metadata does not match the function list')
    except (struct.error, KeyError, UnicodeDecodeError) as e: raise ValueError(f'{path}: unreadable function list ({e})') from e
    return dict(zip(names, consts))


def vertex_entry(vj, zero):
    """`entry` / `entry_z` (near = 0) in <key>_b; in <key>_bp the name `pull` gives it, else the same, as the host takes it (side_entry)"""
    z = bool(zero and vj.get('entry_z'))
    own = vj['entry_z'] if z else vj.get('entry')
    pull = vj.get('pull')
    name = pull.get('entry_z' if z else 'entry') if isinstance(pull, dict) else None
    return name if isinstance(name, str) and name else own


def plan(lib_dir, chain, pairs, zero):
    """→ ([(vertex library, vertex entry, pixel library, pixel entry)], the pairs left to the device, counts of the pairs left out)"""
    lib_dir = romfs.Path(lib_dir)
    js, fns = {}, {}
    def refl(key):
        if key not in js:
            try: js[key] = json.loads((lib_dir / f'{key}_b.json').read_bytes())
            except (OSError, ValueError): js[key] = None
        return js[key]
    def fn(lib):
        if lib not in fns:
            try: fns[lib] = constants(lib_dir / f'{lib}.metallib')
            except (OSError, ValueError): fns[lib] = None
        return fns[lib]
    bases, rest, seen, left = [], [], set(), dict.fromkeys(('reflection', 'mesh', 'stage inputs', 'library', 'primitive ID', 'entry'), 0)
    for vk, fk in pairs:
        vj, fj = refl(vk), refl(fk)
        if not vj or not fj or vj.get('chain') != chain or fj.get('chain') != chain:
            left['reflection'] += 1
            continue
        if vj.get('mesh_stage'):   # mesh pipelines are built at the draw
            left['mesh'] += 1
            continue
        ventry = vertex_entry(vj, zero)
        fentry = fj.get('entry') or 'main0'
        pull = isinstance(vj.get('pull'), dict)
        if not pull and vj.get('vertex_inputs'):
            left['stage inputs'] += 1
            continue
        if (fj.get('raster') or {}).get('primitive_id'):
            left['primitive ID'] += 1
            continue
        vl, fl = vk + ('_bp' if pull else '_b'), fk + '_b'
        vf, ff = fn(vl), fn(fl)
        if vf is None or ff is None:
            left['library'] += 1
            continue
        if ventry not in vf or fentry not in ff:
            left['entry'] += 1
            continue
        if any(c not in HOST_CONSTANTS for c in vf[ventry] + ff[fentry]):
            rest.append((vk, fk))
            continue
        b = (vl, ventry, fl, fentry)
        if b not in seen:
            seen.add(b)
            bases.append(b)
    return bases, rest, left


def plan_compute(lib_dir, chain, shaders):
    """→ ([(library, entry)], counts of the shaders the host would not find a pipeline for)"""
    lib_dir = romfs.Path(lib_dir)
    pipes, seen = [], set()
    left = dict.fromkeys(('reflection', 'library', 'entry', 'constants'), 0)
    for pc in shaders:
        try: j = json.loads((lib_dir / f'{pc}.json').read_bytes())
        except (OSError, ValueError): j = None
        if not isinstance(j, dict) or j.get('chain') != chain or j.get('stage', 'cs') != 'cs':
            left['reflection'] += 1
            continue
        entry = j.get('entry') or 'main0'
        try: fs = constants(lib_dir / f'{pc}.metallib')
        except (OSError, ValueError):
            left['library'] += 1
            continue
        if entry not in fs:
            left['entry'] += 1
            continue
        split = j.get('split') if isinstance(j.get('split'), dict) and j['split'].get('fc') == abi.SPLIT_FC else None
        if any(c not in HOST_CONSTANTS and not (split and c == split_constant(lib_dir / f'{pc}.metallib', entry)) for c in fs[entry]):
            left['constants'] += 1
            continue
        for p in [(pc, entry)] + ([(pc, entry, x) for x in SPLIT_PASSES] if split else []):
            if p not in seen:
                seen.add(p)
                pipes.append(p)
    return pipes, left


# metal-tt specializes only a library's first function ("cannot find private metadata at offset '8'"): a later one comes from a library of its own, cut
# as metallib --function-name cuts; the host finds the base by its function (docs/reference/shaders.md#specialized-second-functions-in-the-offline-pipeline-archive)
def needs_cut(declared, entry):   # declared: constants() of the function's library
    return bool(declared.get(entry)) and next(iter(declared), entry) != entry


def cut_name(lib, entry): return f'{lib}@{entry}'


def cut_entries(lib_dir, bases, computes=()):
    want, found = {(lib, e) for vl, ve, fl, fe in bases for lib, e in ((vl, ve), (fl, fe))} | {tuple(c[:2]) for c in computes}, set()
    declared = {}
    for lib, entry in want:
        if lib not in declared:
            try: declared[lib] = constants(romfs.Path(lib_dir) / f'{lib}.metallib')
            except (OSError, ValueError): declared[lib] = {}
        if needs_cut(declared[lib], entry): found.add((lib, entry))
    return found


def script(lib_dir, bases, computes=()):
    """One render pipeline descriptor per base and one compute descriptor per (library, entry), naming the function alone as cps_bundle looks it up; a function
    with constants is named through its specialization by HOST_CONSTANTS"""
    libs, fns, specs, declared, pipes = {}, {}, {}, {}, []
    def fd(lib, entry, split=None):   # split: a pass of a kernel in two, its constant set as split_pipes sets it
        if lib not in declared:
            try: declared[lib] = constants(romfs.Path(lib_dir) / f'{lib}.metallib')
            except (OSError, ValueError): declared[lib] = {}   # plan read every base's libraries; one it could not read is not among them
        cs = declared[lib].get(entry, ())
        source = cut_name(lib, entry) if needs_cut(declared[lib], entry) else lib
        if source not in libs: libs[source] = {'label': source, 'path': str(Path(lib_dir).resolve() / f'{source}.metallib')}
        label = f'{lib}.{entry}'
        if label not in fns: fns[label] = {'label': label, 'library': f'lib:{source}', 'name': entry}
        if not cs: return f'fnd:{label}'
        spec = f'{label}.host' + (f'.{split}' if split else '')
        if spec not in specs:
            values = sorted([HOST_CONSTANTS[c] for c in cs if c in HOST_CONSTANTS] + ([(abi.SPLIT_FC, SPLIT_PASSES[split])] if split else []))
            specs[spec] = {'label': spec, 'function_descriptor': f'fnd:{label}',
                           'constant_values': [{'id_type': 'FunctionConstantIndex', 'id': {'data': i}, 'value_type': 'ConstantBool', 'value': {'data': v}} for i, v in values]}
        return f'fnd:{spec}'
    for vl, ve, fl, fe in bases:
        pipes.append({'vertex_function_descriptor': fd(vl, ve), 'fragment_function_descriptor': fd(fl, fe),
                      'color_attachments': [dict.fromkeys(COLOUR, 'Unspecialized') for _ in range(8)]})
    descriptors = {'render_pipeline_descriptors': pipes}
    if computes: descriptors['compute_pipeline_descriptors'] = [{'compute_function_descriptor': fd(*c)} for c in computes]
    functions_ = {'library_function_descriptors': list(fns.values())}
    if specs: functions_['specialized_function_descriptors'] = list(specs.values())   # none: no key, so the archive keys of scripts that specialize nothing hold
    return {'libraries': list(libs.values()), 'function_descriptors': functions_, 'pipeline_descriptors': descriptors}


def write_script(path, configuration):
    """Streamed to <name>.partial; replaces the previous script only when they differ, so an unchanged file keeps its time"""
    part = path.with_name(path.name + '.partial')
    try:
        with part.open('w', encoding='utf-8', newline='\n') as out: json.dump(configuration, out, indent=1)
        if not path.is_file() or not filecmp.cmp(path, part, shallow=False): os.replace(part, path)
    finally: part.unlink(missing_ok=True)


def write_changed(path, text):
    """An unchanged file keeps its times, which the Mac app's packer keys its inputs by"""
    try:
        if path.read_text() == text: return
    except (OSError, ValueError): pass
    path.write_text(text)


def run_key(a, archs, text, osmin):
    """What a run reads, by name and time, for the run that finds nothing to do (archive_key hashes every library instead). None for a library folder, or when
    the compiler cannot be asked"""
    image = shader_lib.image_of(a.msl)
    if os.path.isdir(shader_lib.folder_of(a.msl)) or not os.path.isfile(image): return None
    try: tv = tool_version()
    except SystemExit: return None
    st = os.stat(image)
    here = Path(__file__).resolve().parents[2]
    loaded = [Path(f).resolve() for f in (getattr(m, '__file__', None) for m in list(sys.modules.values())) if f]
    code = sorted({str(f) for f in loaded if f.is_relative_to(here)} | {str(Path(abi.HEADER).resolve())})
    # Inode and ctime too: a replacement or same-sized write can keep mtime.
    inputs = {'format': 2, 'image': [os.path.realpath(image), st.st_ino, st.st_size, st.st_mtime_ns, st.st_ctime_ns], 'pairs': hashlib.sha256(text).hexdigest(),
              'computes': None if a.no_compute else read_computes(a.shader_map, a.manifest), 'depth': a.depth, 'archs': archs, 'platform': PLATFORM, 'os_min': osmin, 'toolchain': tv,
              'batch': a.batch_size, 'code': [[os.path.relpath(f, here), hashlib.sha256(Path(f).read_bytes()).hexdigest()] for f in code]}
    return hashlib.sha256(json.dumps(inputs, sort_keys=True).encode()).hexdigest()


def outputs(out, work, archs):
    """Each file of the output folder that is its own (not the linked prewarm pipelines) by size and time, and each architecture's key"""
    files = {f.name: [f.stat().st_size, f.stat().st_mtime_ns] for f in sorted(out.iterdir()) if f.is_file() and '-prewarm-' not in f.name and f.name != 'prewarm.tsv'}
    return {'files': files, 'keys': {arch: (work / f'key.{arch}').read_text() if (work / f'key.{arch}').is_file() else None for arch in archs}}


def run_reused(work, key, out, archs):
    """→ the note to print for each architecture, or None"""
    try: last = json.loads((work / 'run.key').read_text())
    except (OSError, ValueError): return None
    try:
        if not key or not isinstance(last, dict) or last.get('key') != key or last.get('outputs') != outputs(out, work, archs): return None
        if any(k is None for k in last['outputs']['keys'].values()) or any(not (out / f'{arch}.mtl4a').is_file() for arch in archs): return None
        return [f'pso_offline: {arch}: reused ({(out / f"{arch}.mtl4a").stat().st_size / 2**20:.0f} MB), the library and the lists as at the last run' for arch in archs]
    except (OSError, KeyError, TypeError, AttributeError): return None


def library_digest(lib_dir, bases, computes=()):
    """The bytes metal-tt consumes: a profile/chain stamp does not change when the same shaders are rebuilt by another Metal compiler."""
    def digest(name):
        lib = hashlib.sha256()
        with romfs.open(os.path.join(lib_dir, f'{name}.metallib'), 'rb') as f:
            while chunk := f.read(1 << 20): lib.update(chunk)
        return lib.digest()
    names = sorted({lib for vl, _, fl, _ in bases for lib in (vl, fl)} | {c[0] for c in computes})
    h = hashlib.sha256()
    with concurrent.futures.ThreadPoolExecutor(8) as pool:   # digests taken in order: the same key
        for name, d in zip(names, pool.map(digest, names)):
            h.update(name.encode() + b'\0')
            h.update(d)
    return h.hexdigest()


def space_needed(bases):   # an estimate: metal-tt's temporaries and the archive
    return max(2 << 30, bases * (96 << 10))


def clear_temporaries(work):   # what a failed or killed metal-tt leaves in its TMPDIR
    for f in [*work.glob('translate-*'), *work.glob('*.airp'), *work.glob('multiple-functions-input-*.metallib')]:
        if f.is_dir(): shutil.rmtree(f)
        else: f.unlink(missing_ok=True)


def archive_key(configuration, libraries, arch, osmin, toolchain):
    """Identity of metal-tt's inputs, without chain labels and library locations. Version this format if a future input is not represented here."""
    config = dict(configuration, libraries=[dict(lib, path=lib['label'] + '.metallib') for lib in configuration['libraries']])
    inputs = {'format': 1, 'configuration': config, 'libraries': libraries,
              'arch': arch, 'platform': PLATFORM, 'os_min': osmin, 'toolchain': toolchain}
    digest = hashlib.sha256()
    for chunk in json.JSONEncoder(sort_keys=True, separators=(',', ':')).iterencode(inputs):
        digest.update(chunk.encode())
    return digest.hexdigest()


def tool_version():
    r = subprocess.run(['xcrun', '-sdk', SDK, 'metal-tt', '-version'], capture_output=True, text=True)
    if r.returncode: raise SystemExit(f'[RED] pso_offline: metal-tt is not available ({(r.stderr or r.stdout).strip()}); install the Metal toolchain: xcodebuild -downloadComponent MetalToolchain')
    return r.stdout.strip().splitlines()[0]


def os_min_default():   # the major version this Mac runs, not its SDK's: a Mac opens only an archive of its own major version
    v = subprocess.run(['sw_vers', '-productVersion'], capture_output=True, text=True, check=True).stdout.strip()
    return v.split('.')[0] + '.0'


def compile_archive(js, part, work, arch, osmin, count):
    """Temporaries on the work volume; a failed output is never published."""
    part.unlink(missing_ok=True)
    clear_temporaries(work)
    need, free = space_needed(count), vol_space.free_bytes(work)
    if free < need: sys.exit(f'[RED] pso_offline: {free / 2**30:.1f} GB free on the volume of {work}, metal-tt needs about {need / 2**30:.0f} GB for {count} bases: free some space and run again')
    cmd = ['xcrun', '-sdk', SDK, 'metal-tt', '-arch', arch, f'-{PLATFORM}-version-min', osmin, str(js), '-o', str(part)]
    log_path = work / f'metal-tt.{arch}.log'
    with log_path.open('w') as log:
        r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, TMPDIR=str(work)))
    if r.returncode or not part.is_file() or not part.stat().st_size:
        free = shutil.disk_usage(work).free   # what was really left as it failed, not what APFS could still free
        clear_temporaries(work)
        part.unlink(missing_ok=True)
        text = f'[RED] pso_offline: metal-tt failed for {arch} (exit {r.returncode})'
        if free < 1 << 30: sys.exit(f'{text} ({free / 2**30:.1f} GB free on the volume of {work} when it failed: likely out of disk space): {log_path.read_text()[-2000:]}')
        raise CompileFailed(f'{text}: {log_path.read_text()[-2000:]}')


class CompileFailed(Exception): pass


def pair_of(base):
    return base[0].rsplit('_', 1)[0], base[2].rsplit('_', 1)[0]


def failed_file(work, arch): return Path(work) / f'failed.{arch}.tsv'


def read_failed(work, arch):
    """([base], [compute pipeline]) a compile left out, kept beside its key so that a reused archive still names them"""
    f = failed_file(work, arch)
    bases, computes = [], []
    for row in (ln.split('\t') for ln in f.read_text().splitlines()) if f.is_file() else ():
        (bases if row[0] == 'base' else computes).append(tuple(row[1:]))
    return bases, computes


def write_failed(work, arch, bases, computes):
    f = failed_file(work, arch)
    if bases or computes: f.write_text(''.join('\t'.join(('base', *b)) + '\n' for b in bases) + ''.join('\t'.join(('compute', *c)) + '\n' for c in computes))
    else: f.unlink(missing_ok=True)


def compile_isolating(lib_dir, bases, computes, js, part, work, arch, osmin):
    """One pipeline the GPU compiler stops on would cost the app its whole archive, so with room on the volume the failures are found (isolate) and left out.
    → (bases left out, compute pipelines left out)"""
    def run(items):
        write_script(js, script(real_libraries(lib_dir, [x for k, x in items if k == 'b'], work, [x for k, x in items if k == 'c']), [x for k, x in items if k == 'b'], [x for k, x in items if k == 'c']))
        try:
            compile_archive(js, part, work, arch, osmin, len(items))
            return None
        except CompileFailed as e: return str(e)
        finally: clear_real_libraries(work)
    items = [('b', b) for b in bases] + [('c', c) for c in computes]
    try: bad = isolate(items, run)
    except IsolateFailed as e: sys.exit(str(e))
    for (kind, x), why in bad:
        what = f'the base of {" + ".join(pair_of(x))} ({x[0]}.{x[1]}, {x[2]}.{x[3]})' if kind == 'b' else f'the compute pipeline of {".".join(x)}'
        print(f'[RED] pso_offline: {arch}: metal-tt cannot compile {what} ({compiler_line(why)}): left out of the archive, the device builds it', flush=True)
    return [x for (k, x), _ in bad if k == 'b'], [x for (k, x), _ in bad if k == 'c']


class IsolateFailed(Exception): pass


def compiler_line(why):
    lines = why.splitlines()
    return next((ln for ln in lines if 'ERROR' in ln or 'error' in ln), lines[-1] if lines else '').split('): ', 1)[-1].strip()[:200]


def isolate(items, run):
    """Found by halving: run(items) → None when they compiled, else why. After it returns, the last run was of every item but those. Raises IsolateFailed when a
    failure does not repeat alone, more than MAX_FAILED fail, or nothing compiles"""
    first = run(items)
    if first is None: return []
    bad, compiled = [], [False]
    def search(sub, why):
        if len(sub) == 1:
            why = run([items[sub[0]]])
            if why is None: raise IsolateFailed(first + '\n[RED] pso_offline: the failure does not repeat on one pipeline alone')
            bad.append((sub[0], why))
            if len(bad) > MAX_FAILED: raise IsolateFailed(first + f'\n[RED] pso_offline: more than {MAX_FAILED} pipelines of one group fail')
            return
        mid = len(sub) // 2
        left = run([items[i] for i in sub[:mid]])
        if left is None:
            compiled[0] = True
            search(sub[mid:], why)
            return
        search(sub[:mid], left)
        right = run([items[i] for i in sub[mid:]])
        if right is None: compiled[0] = True
        else: search(sub[mid:], right)
    if len(items) > 1: search(list(range(len(items))), first)
    if not compiled[0]: raise IsolateFailed(first)
    out = {i for i, _ in bad}
    again = run([x for i, x in enumerate(items) if i not in out])
    if again is not None: raise IsolateFailed(again)
    return [(items[i], why) for i, why in bad]


def real_libraries(lib_dir, bases, work, computes=()):
    """metal-tt takes file paths: one-file libraries and cut functions get a temporary folder (clear_real_libraries removes it); a reused archive gets none"""
    cuts = cut_entries(lib_dir, bases, computes)
    folder = os.path.isdir(lib_dir)
    if folder and not cuts: return lib_dir
    real = Path(work) / 'libraries'
    shutil.rmtree(real, ignore_errors=True)
    real.mkdir(parents=True)
    for name in sorted({lib for vl, _, fl, _ in bases for lib in (vl, fl)} | {c[0] for c in computes}):
        if folder:
            (real / f'{name}.metallib').symlink_to(Path(lib_dir).resolve() / f'{name}.metallib')
            continue
        with romfs.open(os.path.join(lib_dir, f'{name}.metallib'), 'rb') as f, open(real / f'{name}.metallib', 'wb') as o: shutil.copyfileobj(f, o)
    for lib, entry in sorted(cuts):
        (real / f'{cut_name(lib, entry)}.metallib').write_bytes(dmsh_side.cut((real / f'{lib}.metallib').read_bytes(), {entry})[1])
    return real


def clear_real_libraries(work): shutil.rmtree(Path(work) / 'libraries', ignore_errors=True)


def compile_batch(lib_dir, bases, work, arch, osmin, toolchain, computes=()):
    """One bounded group, cached by its actual libraries and descriptors; its left-out pipelines are in read_failed(work, arch)"""
    work.mkdir(parents=True, exist_ok=True)
    configuration = script(lib_dir, bases, computes)
    js = work / 'bases.mtl4-json'
    write_script(js, configuration)
    key = archive_key(configuration, library_digest(lib_dir, bases, computes), arch, osmin, toolchain)
    del configuration
    dst, kf = work / f'{arch}.mtl4a', work / f'key.{arch}'
    if dst.is_file() and dst.stat().st_size and kf.is_file() and kf.read_text().strip() == key: return dst, True
    kf.unlink(missing_ok=True)
    part = work / f'{arch}.mtl4a.partial'
    write_failed(work, arch, *compile_isolating(lib_dir, bases, computes, js, part, work, arch, osmin))
    os.replace(part, dst)
    kf.write_text(key + '\n')
    return dst, False


def groups_of(bases, computes, batch_size, compute_size=COMPUTE_BATCH):
    """Bases in groups of batch_size (00001, ...), then compute pipelines in groups of compute_size (c00001, ...), so a change to either leaves the other's. 0: one group each"""
    groups = []
    for prefix, items, size in (('', bases, batch_size), ('c', computes, compute_size)):
        size = size or len(items) or 1
        for index, start in enumerate(range(0, len(items), size), 1):
            group = items[start:start + size]
            groups.append((f'{prefix}{index:05d}', group if not prefix else [], group if prefix else []))
    return groups


def compile_batches(lib_dir, bases, work, part, arch, osmin, toolchain, batch_size, computes=(), jobs=1, compute_size=COMPUTE_BATCH):
    """Bounds the input count, not memory: a single pipeline can still exceed the compiler's resources. Base groups run in sequence, compute groups jobs at a time
    (each its own TMPDIR)."""
    groups = groups_of(bases, computes, batch_size, compute_size)
    total = len(groups)
    names = {name for name, _, _ in groups}
    failed = ([], [])
    for old in [*(work / f'batches.{arch}').glob('[0-9]*'), *(work / f'batches.{arch}').glob('c[0-9]*')]:
        if old.name not in names: shutil.rmtree(old)
    def one(index, name, group, cgroup):
        print(f'pso_offline: {arch}: batch {index}/{total}: {f"{len(group)} bases" if group else f"{len(cgroup)} compute pipelines"}, compiling or checking cache', flush=True)
        piece, reused = compile_batch(lib_dir, group, work / f'batches.{arch}' / name, arch, osmin, toolchain, cgroup)
        print(f'pso_offline: {arch}: batch {index}/{total}: ready ({"reused" if reused else "compiled"})', flush=True)
        fb, fc = read_failed(work / f'batches.{arch}' / name, arch)
        failed[0].extend(fb)
        failed[1].extend(fc)
        return piece
    pieces = [one(i, *g) for i, g in enumerate(groups, 1) if g[1]]
    with concurrent.futures.ThreadPoolExecutor(max(1, jobs)) as pool:   # a failed group stops the build once the others under way have finished (their pieces stay cached)
        futures = [pool.submit(one, i, *g) for i, g in enumerate(groups, 1) if not g[1]]
        pieces += [f.result() for f in futures]
    part.unlink(missing_ok=True)
    need, free = max(2 << 30, sum(p.stat().st_size for p in pieces)), vol_space.free_bytes(work)
    if free < need: sys.exit(f'[RED] pso_offline: {free / 2**30:.1f} GB free before merging; need at least {need / 2**30:.1f} GB')
    print(f'pso_offline: {arch}: merging {total} batches', flush=True)
    cmd = ['xcrun', '-sdk', SDK, 'metal-pack', '-nthreads', '1', *map(str, pieces), '-o', str(part)]
    log_path = work / f'metal-pack.{arch}.log'
    with log_path.open('w') as log:
        r = subprocess.run(cmd, stdout=log, stderr=subprocess.STDOUT, env=dict(os.environ, TMPDIR=str(work)))
    if r.returncode or not part.is_file() or not part.stat().st_size:
        part.unlink(missing_ok=True)
        sys.exit(f'[RED] pso_offline: metal-pack failed for {arch} (exit {r.returncode}): {log_path.read_text()[-2000:]}')
    return failed


# The prewarm pipelines are the packing script's (mac_prebake): it compiles each prewarm period's script for every other GPU and calls these two functions
def script_libraries(scripts, lib_dir, real):
    """Scripts name each library by the SHA-256 of its bytes (the host loads them from memory): the shader folder's libraries are hashed until every one named is
    found, written to real, and the scripts pointed at them. Raises LookupError naming a library not found"""
    want, data = set(), {}
    for js in scripts:
        data[js] = json.loads(Path(js).read_text())
        want |= {lib['label'].upper() for lib in data[js].get('libraries', []) if not Path(lib.get('path', '')).is_file()}
    real = Path(real)
    shutil.rmtree(real, ignore_errors=True)
    real.mkdir(parents=True)
    found = {}
    for root, _, files in romfs.walk(str(lib_dir)):
        for f in files:
            if not f.endswith('.metallib') or len(found) == len(want): continue
            with romfs.open(os.path.join(root, f), 'rb') as fh: b = fh.read()
            h = hashlib.sha256(b).hexdigest().upper()
            if h in want and h not in found:
                (real / f'{h}.metallib').write_bytes(b)
                found[h] = real / f'{h}.metallib'
    missing = want - set(found)
    if missing: raise LookupError(f'{len(missing)} of the libraries the prewarm scripts name are not in {lib_dir} (one is {sorted(missing)[0]})')
    cuts = {}
    for js, d in data.items():
        for lib in d.get('libraries', []):
            if lib['label'].upper() in found: lib['path'] = str(found[lib['label'].upper()])
        fds = d.get('function_descriptors', {})
        spec = {s['function_descriptor'].removeprefix('fnd:') for s in fds.get('specialized_function_descriptors', [])}
        for fn in fds.get('library_function_descriptors', []):
            h = fn.get('library', '').upper()
            # a later function of its library comes from a library of its own (needs_cut)
            if fn['label'] not in spec or h not in found: continue
            if h not in cuts:
                try: cuts[h] = constants(found[h])
                except (OSError, ValueError): cuts[h] = {}
            if not needs_cut(cuts[h], fn['name']): continue
            cut = cut_name(h, fn['name'])
            path = real / f'{cut}.metallib'
            if not path.exists(): path.write_bytes(dmsh_side.cut(found[h].read_bytes(), {fn['name']})[1])
            if not any(lib['label'] == cut for lib in d['libraries']): d['libraries'].append({'label': cut, 'path': str(path)})
            fn['library'] = cut
        Path(js).write_text(json.dumps(d, indent=1))
    return real


def publish_prewarm(out, sources):
    """Only changed files: the Mac app's packer keys its inputs by file identity, ctime included"""
    out = Path(out)
    pending = dict(sources)
    for old in [*out.glob('*-prewarm-*'), out / 'prewarm.tsv']:
        src = pending.get(old.name)
        if src and old.is_file() and (os.path.samefile(src, old) or (src.stat().st_size == old.stat().st_size and filecmp.cmp(src, old, shallow=False))): del pending[old.name]
        else: old.unlink(missing_ok=True)
    for name, src in pending.items():
        try: os.link(src, out / name)
        except OSError: shutil.copyfile(src, out / name)


def save_run(work, quick, out, archs):
    if quick:
        with contextlib.suppress(OSError): (work / 'run.key').write_text(json.dumps({'key': quick, 'outputs': outputs(out, work, archs)}, sort_keys=True) + '\n')


def main(argv=None):
    ap = argparse.ArgumentParser(description='Build the pipeline archive of a game\'s shader pairs offline (metal-tt)')
    ap.add_argument('--msl', required=True, help='the shader libraries, b-<chain version> (with their reflection <key>_b.json)')
    ap.add_argument('--game', help='the game profile: its pair lists and depth_mode key')
    ap.add_argument('--pairs', action='append', help='shader_pairs.tsv, then pso_pairs.tsv (missing files are skipped; default: the profile\'s)')
    ap.add_argument('--out', required=True, help='the folder the app bundles (archives and stamp)')
    ap.add_argument('--work', required=True, help='the script, keys and logs')
    ap.add_argument('--gpu', required=True, help='GPU architectures as MTLDevice.architecture names them, comma-separated, with or without the applegpu_ prefix (g14s: an M2 Pro or M2 Max)')
    ap.add_argument('--depth', choices=('gl', 'zero'), help='the game\'s NVN depth mode (default: the profile\'s depth_mode, else gl: near = -w)')
    ap.add_argument('--os-min', help='minimum macOS of the archives (default: the major version this Mac runs)')
    ap.add_argument('--script-only', action='store_true', help='write the script and the stamp, run no metal-tt')
    ap.add_argument('--batch-size', type=int, default=BATCH, help=f'compile sequential groups of at most N bases and merge with metal-pack, when there are more (default {BATCH}; 0: one compiler invocation)')
    ap.add_argument('--shader-map', action='append', help='shader_map.tsv: its compute shaders get pipelines (default: the profile\'s)')
    ap.add_argument('--manifest', action='append', help='pso_manifest.tsv: its C rows first (default: the profile\'s)')
    ap.add_argument('--no-compute', action='store_true', help='no compute pipelines, the bases alone')
    ap.add_argument('--jobs', type=int, default=JOBS, help=f'metal-tt runs at a time for the compute pipelines (default {JOBS}, half the logical CPUs)')
    a = ap.parse_args(argv)
    if a.batch_size < 0: ap.error('--batch-size must be zero or positive')
    if a.game:
        sys.path.insert(0, str(Path(__file__).resolve().parents[2] / 'games'))
        import game
        g = game.load(a.game)
        a.pairs = a.pairs or [str(g.table('shader_pairs.tsv')), str(g.table('pso_pairs.tsv'))]
        a.shader_map = a.shader_map or [str(g.table('shader_map.tsv'))]
        a.manifest = a.manifest or [str(g.table('pso_manifest.tsv'))]
        a.depth = a.depth or g.get('depth_mode', 'gl')
        if a.depth not in ('gl', 'zero'): sys.exit(f'[RED] pso_offline: {g.id}: depth_mode must be gl or zero: {a.depth}')
    if not a.pairs: sys.exit('[RED] pso_offline: no pair lists (--pairs or --game)')
    a.depth = a.depth or 'gl'
    lib_dir, out, work = shader_lib.view(a.msl), Path(a.out), Path(a.work)
    if not shader_lib.name_of(lib_dir).startswith('b-'): sys.exit(f'[RED] pso_offline: {lib_dir} is not a b-<chain version> folder')
    chain = shader_lib.name_of(lib_dir)[2:]
    archs = sorted({arch_name(x) for x in a.gpu.split(',') if x.strip()})
    if not archs: sys.exit('[RED] pso_offline: no GPU architecture (--gpu)')
    text = pair_text(a.pairs)
    if not text: sys.exit(f'[RED] pso_offline: no pair list ({", ".join(a.pairs)})')
    zero = a.depth == 'zero'
    t0 = time.time()
    osmin = a.os_min or os_min_default()
    quick = None if a.script_only else run_key(a, archs, text, osmin)
    if quick and (notes := run_reused(work, quick, out, archs)):
        print('\n'.join(notes))
        save_run(work, quick, out, archs)
        return 0
    (work / 'run.key').unlink(missing_ok=True)   # written again at the end of a run that succeeds
    pairs = read_pairs(text)
    bases, rest, left = plan(lib_dir, chain, pairs, zero)
    shaders = [] if a.no_compute else read_computes(a.shader_map, a.manifest)
    computes, cleft = plan_compute(lib_dir, chain, shaders)
    work.mkdir(parents=True, exist_ok=True)
    out.mkdir(parents=True, exist_ok=True)
    js = work / 'bases.mtl4-json'   # metal-tt takes the format from the extension
    configuration = script(lib_dir, bases, computes)
    write_script(js, configuration)
    st = stamp(chain, zero, text)
    print(f'pso_offline: {len(pairs)} pairs: {len(bases)} bases, {len(rest)} left to the device (function constants), left out as the host leaves them: '
          + ', '.join(f'{k} {v}' for k, v in left.items()) + f' ({time.time() - t0:.1f} s)')
    if shaders: print(f'pso_offline: {len(shaders)} compute shaders: {len(computes)} compute pipelines, left out: '
                      + ', '.join(f'{k} {v}' for k, v in cleft.items()))
    if a.script_only:
        write_changed(out / 'prebuilt.tsv', st)
        return 0
    if pairs and not bases and not rest: sys.exit('[RED] pso_offline: no base to build')
    if not pairs: print('pso_offline: no pairs (only a header): nothing to build, no archive')
    gpus = archs
    if not bases and not computes: archs = []
    tv = tool_version() if bases or computes else 'not used (function constants only)'
    libraries = library_digest(lib_dir, bases, computes)
    keys = {arch: archive_key(configuration, libraries, arch, osmin, tv) for arch in archs}
    del configuration
    failed_bases, failed_computes = [], []
    for arch, key in keys.items():
        dst, kf = out / f'{arch}.mtl4a', work / f'key.{arch}'
        if dst.is_file() and kf.is_file() and kf.read_text().strip() == key:
            print(f'pso_offline: {arch}: reused ({dst.stat().st_size / 2**20:.0f} MB)')
            fb, fc = read_failed(work, arch)
            failed_bases += fb
            failed_computes += fc
            continue
        kf.unlink(missing_ok=True)
        dst.unlink(missing_ok=True)
        part = work / f'{arch}.mtl4a.partial'
        part.unlink(missing_ok=True)
        t1 = time.time()
        print(f'pso_offline: {arch}: metal-tt, {len(bases)} bases, {len(computes)} compute pipelines (minutes) ...', flush=True)
        if a.batch_size and len(groups_of(bases, computes, a.batch_size)) > 1:
            fb, fc = compile_batches(lib_dir, bases, work, part, arch, osmin, tv, a.batch_size, computes, a.jobs)
        else:
            fb, fc = compile_isolating(lib_dir, bases, computes, js, part, work, arch, osmin)
        write_failed(work, arch, fb, fc)
        failed_bases += fb
        failed_computes += fc
        os.replace(part, dst)
        kf.write_text(key + '\n')
        print(f'pso_offline: {arch}: {dst.stat().st_size / 2**20:.0f} MB in {time.time() - t1:.0f} s')
    for f in out.glob('*.mtl4a'):   # an architecture no longer asked for loses its archive and prewarm files
        keep = f.stem.split('-prewarm-')[0] in gpus if '-prewarm-' in f.name else f.stem in archs
        if not keep: f.unlink()
    write_changed(out / 'prebuilt.tsv', st)
    rest = list(dict.fromkeys([*rest, *map(pair_of, failed_bases)]))
    rp = out / 'rest_pairs.tsv'
    if rest: write_changed(rp, ''.join(f'{v}\t{f}\n' for v, f in rest))
    else: rp.unlink(missing_ok=True)
    write_changed(out / 'archive.tsv', f'# the pipeline archive made offline (dm/gfx/tools/pso_offline.py); the host reads prebuilt.tsv, not this file\narch\t{" ".join(archs)}\n'
                                     f'platform\t{PLATFORM}\t{osmin}\ntoolchain\t{tv}\nbases\t{len(bases)}\nrest\t{len(rest)}\ncompute\t{len(computes)}\n'
                                     f'failed\t{len(set(failed_bases))} bases, {len(set(failed_computes))} compute pipelines (metal-tt could not compile them: left to the device)\n')
    save_run(work, quick, out, archs)
    return 0


if __name__ == '__main__':
    sys.exit(main())
