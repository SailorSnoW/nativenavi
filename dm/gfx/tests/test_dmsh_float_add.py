#!/usr/bin/env python3
"""dmsh_rewrite.py float_add_msl: a float add to storage memory (RED.F32's compare-and-swap loop, InstEmitMemory EmitAtomicAddF32) as one native atomic_fetch_add on a device
atomic_float. The loop as spirv-cross writes it is replaced and any other loop is left alone; a later read of its retry flag still gets false. On this Mac's GPU 8,192 threads
adding into the same four words, through the loop and through the rewrite, reach the exact sums. The add is a call of a noinline leaf, dm_atomic_add_f32, which keeps
Apple's AGX3 compiler (M5, macOS 26.0-26.6) from hoisting it into its constant program; with DMSH_TEST_METAL=1, metal-tt shows the inline add failing for applegpu_g17g and the
rewrite compiling for g17g and g17s. test_dmsh_ampere_alu.py FloatRed checks that the translator's own loop is the one rewritten."""
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
import dmsh_chain
import dmsh_rewrite
float_add_msl = dmsh_rewrite.float_add_msl
HEAD = 'using namespace metal;\n'
HELPER = dmsh_rewrite.FLOAT_ADD_HELPER


def has_metal_tt():
    return sys.platform == 'darwin' and bool(shutil.which('xcrun')) and subprocess.run(['xcrun', '-sdk', 'macosx', 'metal-tt', '-version'], capture_output=True).returncode == 0


def compile_tt(msl, flags, arch, osmin):
    """(ok, log) of kernel k through metal, metallib and metal-tt for a GPU and a minimum macOS"""
    with tempfile.TemporaryDirectory() as w:
        w = Path(w); (w / 'k.metal').write_text(msl)
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-w', *flags, str(w / 'k.metal'), '-o', str(w / 'k.air')], capture_output=True, text=True)
        if r.returncode: raise AssertionError('metal: ' + r.stderr)
        subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / 'k.air'), '-o', str(w / 'k.metallib')], check=True, capture_output=True)
        js = {'libraries': [{'label': 'L', 'path': str(w / 'k.metallib')}],
              'function_descriptors': {'library_function_descriptors': [{'label': 'L.k', 'library': 'lib:L', 'name': 'k'}]},
              'pipeline_descriptors': {'compute_pipeline_descriptors': [{'compute_function_descriptor': 'fnd:L.k'}]}}
        (w / 'k.mtl4-json').write_text(json.dumps(js))
        r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal-tt', '-arch', arch, '-macos-version-min', osmin, str(w / 'k.mtl4-json'), '-o', str(w / 'k.mtl4a')], capture_output=True, text=True)
        return r.returncode == 0, r.stdout + r.stderr


# The loop of 6e28359aa3558f61 (Ocarina of Time), on word w of buffer b, adding v
def loop(b, w, v, n):
    return f"""    bool _{n};
    do
    {{
        float _{n + 1} = as_type<float>({b}._m0[{w}]);
        float _{n + 2} = _{n + 1} + {v};
        uint _{n + 3} = as_type<uint>(_{n + 1});
        uint _{n + 4};
        do
        {{
            _{n + 4} = _{n + 3};
        }} while (!atomic_compare_exchange_weak_explicit((device atomic_uint*)&{b}._m0[{w}], &_{n + 4}, as_type<uint>(_{n + 2}), memory_order_relaxed, memory_order_relaxed) && _{n + 4} == _{n + 3});
        uint _{n + 5} = _{n + 4};
        _{n} = int(_{n + 5}) != as_type<int>(_{n + 1});
    }} while (_{n});
"""

KERNEL = """#include <metal_stdlib>
using namespace metal;
struct B { uint _m0[1]; };
kernel void k(device B& b [[buffer(0)]], uint id [[thread_position_in_grid]])
{
    float v = float(id & 7u) * 0.25f, w = 1.0f;
%s}
"""


class FloatAdd(unittest.TestCase):
    def test_loop_becomes_one_atomic_add(self):
        src = HEAD + loop('(*spvDescriptorSet0.cp_s4)', 0, '_2580', 10) + loop('(*spvDescriptorSet0.cp_s4)', 'int(_7)', '_2572', 20)
        out = float_add_msl(src)
        self.assertEqual(out, HEAD + HELPER
                              + '    dm_atomic_add_f32((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[0], _2580);\n'
                              + '    bool _10 = false;\n'
                              + '    dm_atomic_add_f32((device atomic_float*)&(*spvDescriptorSet0.cp_s4)._m0[int(_7)], _2572);\n'
                              + '    bool _20 = false;\n')
        self.assertEqual(float_add_msl(out), out)   # idempotent

    def test_exact_add_too(self):   # the translator's exact form (XLATE_REDUCED=0): spvFAdd(old, v)
        src = HEAD + loop('b', 0, 'v', 10).replace('_11 + v;', 'spvFAdd(_11, v);')
        self.assertEqual(float_add_msl(src), HEAD + HELPER + '    dm_atomic_add_f32((device atomic_float*)&b._m0[0], v);\n    bool _10 = false;\n')

    def test_live_retry_flag_is_false_after_add(self):
        tail = '    b._m0[0] = uint(_10);\n'
        for add in ('_11 + v', 'spvFAdd(_11, v)'):
            with self.subTest(add=add):
                src = HEAD + loop('b', 0, 'v', 10).replace('_11 + v', add) + tail
                out = float_add_msl(src)
                self.assertNotIn('atomic_compare_exchange_weak_explicit', out)
                self.assertIn('    bool _10 = false;\n', out)   # the do-while can only finish with this value
                self.assertTrue(out.endswith(tail))

    def test_other_words_and_shapes_left_alone(self):
        other = loop('b', 0, 'v', 10).replace('&b._m0[0], &', '&b._m0[1], &')   # the swap on another word than the read
        self.assertEqual(float_add_msl(other), other)

    def test_without_using_namespace_metal_it_fails(self):
        with self.assertRaises(ValueError): float_add_msl(loop('b', 0, 'v', 10))

    @unittest.skipUnless(shutil.which('xcrun') and sys.platform == 'darwin', 'needs Metal')
    def test_sums_on_gpu(self):
        body = loop('b', 0, 'v', 10) + loop('b', 1, 'w', 20) + loop('b', 2, 'v', 30) + loop('b', 3, 'w', 40)
        body += '    if (_10 || _20 || _30 || _40) { b._m0[0] = as_type<uint>(-1.0f); }\n'
        runner = r'''#import <Metal/Metal.h>
#include <stdio.h>
int main(int ac, char **av) { @autoreleasepool {
    id<MTLDevice> d = MTLCreateSystemDefaultDevice(); NSError *e = nil;
    for (int k = 1; k < ac; k++) {
        id<MTLLibrary> l = [d newLibraryWithURL:[NSURL fileURLWithPath:@(av[k])] error:&e]; if (!l) { puts(e.description.UTF8String); return 2; }
        id<MTLComputePipelineState> ps = [d newComputePipelineStateWithFunction:[l newFunctionWithName:@"k"] error:&e]; if (!ps) { puts(e.description.UTF8String); return 2; }
        id<MTLBuffer> b = [d newBufferWithLength:16 options:MTLResourceStorageModeShared]; memset(b.contents, 0, 16);
        id<MTLCommandQueue> q = [d newCommandQueue]; id<MTLCommandBuffer> c = [q commandBuffer]; id<MTLComputeCommandEncoder> ce = [c computeCommandEncoder];
        [ce setComputePipelineState:ps]; [ce setBuffer:b offset:0 atIndex:0]; [ce dispatchThreads:MTLSizeMake(8192, 1, 1) threadsPerThreadgroup:MTLSizeMake(256, 1, 1)]; [ce endEncoding]; [c commit]; [c waitUntilCompleted];
        const float *f = b.contents; printf("%g %g %g %g\n", f[0], f[1], f[2], f[3]);
    }
    return 0;
} }
'''
        with tempfile.TemporaryDirectory() as w:
            w = Path(w); libs = []
            for name, src in (('loop', KERNEL % body), ('native', float_add_msl(KERNEL % body))):
                (w / f'{name}.metal').write_text(src)
                subprocess.run([*dmsh_chain.mac_metal(), str(w / f'{name}.metal'), '-o', str(w / f'{name}.air')], check=True, capture_output=True)   # the chain's flags: its deployment target is its language version
                subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(w / f'{name}.air'), '-o', str(w / f'{name}.metallib')], check=True)
                libs.append(str(w / f'{name}.metallib'))
            self.assertIn('dm_atomic_add_f32((device atomic_float*)', (w / 'native.metal').read_text())
            (w / 'r.m').write_text(runner)
            subprocess.run(['clang', '-fobjc-arc', '-framework', 'Metal', '-framework', 'Foundation', str(w / 'r.m'), '-o', str(w / 'r')], check=True, capture_output=True)
            out = subprocess.run([str(w / 'r')] + libs, capture_output=True, text=True)
            want = f'{sum((i & 7) * 0.25 for i in range(8192)):g} 8192 {sum((i & 7) * 0.25 for i in range(8192)):g} 8192'
            self.assertEqual(out.stdout.split('\n')[:2], [want, want], out.stdout + out.stderr)


    @unittest.skipUnless(os.environ.get('DMSH_TEST_METAL') and has_metal_tt(), 'DMSH_TEST_METAL=1 compiles for M5 GPUs with metal-tt')
    def test_m5_compiler_takes_the_helper(self):   # AIR-NT 32023.922: the inline add fails on applegpu_g17g at macOS 26.6 with disable.smashing.float
        flags = dmsh_chain.mac_metal()[5:]
        ok, log = compile_tt(KERNEL % '    atomic_fetch_add_explicit((device atomic_float*)&b._m0[0], v, memory_order_relaxed);\n', flags, 'applegpu_g17g', '26.6')
        if ok: self.skipTest('this toolchain compiles the inline add for applegpu_g17g')
        self.assertIn('disable.smashing.float', log)
        rewritten = float_add_msl(KERNEL % loop('b', 0, 'v', 10))
        for arch in ('applegpu_g17g', 'applegpu_g17s'):
            ok, log = compile_tt(rewritten, flags, arch, '26.6')
            self.assertTrue(ok, f'{arch}: {log}')


if __name__ == '__main__':
    unittest.main()
