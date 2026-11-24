#!/usr/bin/env python3
# A precompiled header of the Metal texture class instantiations translated shaders use, most of a compile's front end. It declares no name and the output is
# byte-identical (checked on every kind in make()), so it is outside the compile records and the chain version. DMSH_PCH=0 compiles without it.
import atexit, os, shutil, subprocess, sys, tempfile, threading

SAMPLED = ('texture2d', 'texture2d_array', 'texture3d', 'texturecube', 'texturecube_array', 'depth2d', 'depth2d_array', 'depthcube', 'depthcube_array')
WRITTEN = ('texture2d', 'texture2d_array', 'texture3d')
CLASSES = ([f'metal::{k}<float, metal::access::sample, metal::memory_coherence_{c}>' for k in SAMPLED for c in ('threadgroup', 'device')] +
           [f'metal::{k}<float, metal::access::write, metal::memory_coherence_threadgroup>' for k in WRITTEN])   # what translated shaders instantiate: float, sampled or written
HEADER = '#include <metal_stdlib>\n#include <simd/simd.h>\n' + ''.join(f'static_assert(sizeof({c}) > 0);\n' for c in CLASSES)
PROBE = '''#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;
struct set0 { texture2d<float> a [[id(0)]]; texture2d_array<float> b [[id(1)]]; texture3d<float> c [[id(2)]]; texturecube<float> d [[id(3)]]; depth2d_array<float> e [[id(4)]]; sampler s [[id(5)]]; };
struct main0_in { float4 uv [[user(locn0)]]; };
fragment float4 main0(main0_in in [[stage_in]], constant set0& t [[buffer(0)]])
{
    return t.a.sample(t.s, in.uv.xy) + t.b.sample(t.s, in.uv.xy, 0u) + t.c.sample(t.s, in.uv.xyz) + t.d.sample(t.s, in.uv.xyz) + t.e.sample(t.s, in.uv.xy, 0u);
}
'''
_lock, _headers = threading.Lock(), {}


def make(metal):
    """→ its path, or (None, why it is not used)"""
    d = tempfile.mkdtemp(prefix='pch_', dir=os.environ.get('WORK'))   # not dmsh_*: the packing script's progress looks for those
    atexit.register(shutil.rmtree, d, ignore_errors=True)
    h, pch, src = os.path.join(d, 'prefix.h'), os.path.join(d, 'prefix.pch'), os.path.join(d, 'probe.metal')
    with open(h, 'w') as f: f.write(HEADER)
    with open(src, 'w') as f: f.write(PROBE)
    r = subprocess.run([*(a for a in metal if a != '-c'), '-x', 'metal-header', h, '-o', pch], capture_output=True, text=True)
    if r.returncode: return None, f'it does not build: {r.stderr.strip()[:200]}'
    outs = []
    for extra, name in (((), 'plain.air'), (('-include-pch', pch), 'header.air')):
        out = os.path.join(d, name)
        r = subprocess.run([*metal, *extra, src, '-o', out], capture_output=True, text=True)
        if r.returncode: return None, f'a test shader does not compile: {r.stderr.strip()[:200]}'
        with open(out, 'rb') as f: outs.append(f.read())
    if outs[0] != outs[1]: return None, 'a test shader compiled with it differs'
    return pch, None


def header(metal):
    if os.environ.get('DMSH_PCH') == '0': return None
    with _lock:   # compiling without it would be slower than the wait
        if tuple(metal) not in _headers:
            pch, why = make(metal)
            if why: print(f'note: no precompiled Metal header for {" ".join(metal)}, so each compile takes longer: {why}', flush=True)
            _headers[tuple(metal)] = pch
        return _headers[tuple(metal)]


def _run(metal, msl, air, **kw):
    # The source on stdin: clang names a static initializer after the file (_GLOBAL__sub_I_<name>), so one text under two names would give two libraries
    with open(msl, 'rb') as f: return subprocess.run([*metal, '-x', 'metal', '-', '-o', air], stdin=f, **kw)


def compile(metal, msl, air):
    """A compile that fails with the header runs again without it, so a failure is the plain compile's"""
    pch = header(metal)
    if pch:
        r = _run([*metal, '-include-pch', pch], msl, air, capture_output=True)
        if not r.returncode:
            sys.stderr.buffer.write(r.stderr)
            sys.stderr.flush()
            return
    try: _run(metal, msl, air, check=True)
    except subprocess.CalledProcessError:
        print(f'{msl}: does not compile (the messages above say <stdin>)', file=sys.stderr, flush=True)
        raise
