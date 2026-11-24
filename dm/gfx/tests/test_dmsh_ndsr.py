"""agl's NDSR network on simdgroup matrices, in two passes (dm/gfx/tools/dmsh_ndsr.py; shaders.md, "agl's NDSR network on simdgroup matrices, in two passes").
The game's shader is not here: a synthetic kernel computes the same graph (176 HMMA.16816 calls: 8 patches of three blocks over a 16x16 window, 12 weight matrices in PTX fragment
order, ReLU between blocks) in the shape the translator and spirv-cross write it (straight-line declarations, the helper's 18 operands, its flags), and goes through
dmsh_rewrite.hmma_msl as every translated shader does. On the CPU: the pass recognises it under two schedules of the calls (plain operands; TensorPass's chain slots, A reuse and
interleaved patches), and refuses every change of the graph, leaving the source as it was. On this Mac's GPU: the translated kernel against the rewritten one in one pass and in
two (DMSH_SPLIT_FC false, then true, the words at DMSH_SPLIT_SLOT), bit for bit on random weights and windows."""
from pathlib import Path
import random
import shutil
import struct
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
sys.path.insert(0, str(Path(__file__).resolve().parent))
import dmsh_abi as abi, dmsh_chain, dmsh_ndsr, dmsh_rewrite   # noqa: E402
from test_dmsh_hmma import HEAD, HELPER   # noqa: E402  (the helper as spirv-cross writes it, which hmma_msl rewrites)

GROUPS = 6   # threadgroups the GPU check runs


class Synthetic:
    """The network's kernel in the translator's shape. schedule: 'plain' (every operand a word, C = 0 by the flag) or 'chained' (TensorPass's flags: a chain's first call keeps D in
    accumulator slot k, its second takes C from there; a call with the previous call's A reuses it; two patches' chains interleaved in slots 0 and 2). mutate: one change of the
    graph the pass must refuse"""

    def __init__(self, schedule='plain', mutate=None, calls_out=0):
        self.n, self.lines, self.mutate, self.dropped = 1000, [], mutate, calls_out
        self.schedule = schedule

    def v(self, ty, expr):
        self.n += 1; name = f'_{self.n}'; self.lines.append(f'    {ty} {name} = {expr};'); return name

    def word_of(self, ty_expr):
        f = self.v('float', ty_expr)
        return self.v('int', f'as_type<int>({f})')

    def call(self, A, B, C, fl):
        args = [self.v('int', f'as_type<int>({self.lane})'), self.v('int', f'int({self.sg}) << 8')]
        args += [self.v('int', x) for x in A] + [self.v('int', x) for x in B] + [self.v('int', x) for x in C] + [self.v('int', str(fl))] + [self.v('int', '0') for _ in range(6)]
        self.n += 1; d1 = f'_{self.n}'; self.lines.append(f'    int {d1};')
        self.n += 1; r = f'_{self.n}'
        self.lines.append(f'    int {r} = dmsh_hmma16816_f16({", ".join(args)}, {d1}, _34);')
        self.calls += 1
        return r, d1

    def source(self):
        L = self.lines
        L.append('    threadgroup spvUnsafeArray<uint, 1408> _29;')
        L.append('    threadgroup spvUnsafeArray<uint, 1024> _34;')
        lid = self.v('uint', 'gl_LocalInvocationID.x')
        for i in range(11):   # the prologue: the window, 1408 words, 11 a thread
            idx = self.v('uint', f'{lid} + {128 * i}u')
            val = self.v('uint', f'win_in._m0[gl_WorkGroupID.x * 1408u + {idx}]')
            L.append(f'    _29[int({idx})] = {val};')
        self.lane = self.v('uint', 'gl_SubgroupInvocationID') if self.mutate != 'lane' else lid
        self.sg = self.v('uint', f'{lid} >> 5u')
        g = self.v('uint', f'{self.lane} >> 2u'); t = self.v('uint', f'{self.lane} & 3u')
        base = self.v('int', f'(int({self.sg}) * 6 + int({g})) * 144 + int({t}) * 4')
        W = {}
        def weights(j):   # matrix j's four words of this lane, one 16-byte load
            if j not in W:
                ix = self.v('uint', f'uint((int({self.lane}) * 16 + {dmsh_ndsr.MATRIX * j}) >> 2)')
                L.append(f'    packed_uint4 _dmw{j} = *(const device packed_uint4*)&wts._m0[int({ix})];')
                W[j] = [self.word_of(f'as_type<float>(_dmw{j}.{c})') for c in 'xyzw']
            return W[j]
        for j in range(2): weights(j)   # some loads before the barrier, the rest where the core first needs them
        L.append(dmsh_ndsr.BARRIER)
        self.calls = 0
        S = {}
        for p in range(dmsh_ndsr.PATCHES):   # block 0's window words, read before the second barrier (the epilogue reuses the array after it)
            for nb in (0, 1):
                for k in (0, 16):
                    off = 12 * p + 1152 * nb + k + (4 if self.mutate == 'window' and p == 3 and nb == 1 and k == 16 else 0)
                    a = self.v('uint', f'uint(int(uint({base} + {off}) >> uint(2)))')
                    S[p, nb, k] = self.word_of(f'as_type<float>(_29[int({a})])')
        L.append(dmsh_ndsr.BARRIER)
        gid = None
        X = {p: [(S[p, nb, 0], S[p, nb, 16]) for nb in (0, 1)] for p in range(dmsh_ndsr.PATCHES)}
        finals = {}
        pairs = [(p, p + 1) for p in range(0, dmsh_ndsr.PATCHES, 2)] if self.schedule == 'chained' else [(p,) for p in range(dmsh_ndsr.PATCHES)]
        for ps in pairs:
            for b in range(dmsh_ndsr.BLOCKS):
                if b == 1 and gid is None: gid = self.v('uint', 'gl_WorkGroupID.x * 128u + gl_LocalInvocationID.x')   # a value of the epilogue's, in the core: kept
                Gd = {}
                for p in ps:
                    for gr in (0, 1):
                        j = 4 * b + 2 * gr + (1 if self.mutate == 'weight' and p == 5 and b == 1 and gr == 1 else 0)
                        for nb in (0, 1):
                            reuse = self.schedule == 'chained' and nb == 1
                            Gd[p, gr, nb] = self.call(weights(j), X[p][nb], ('0', '0'), 8 | (1 if reuse else 0))
                Y = {p: [] for p in ps}
                for nh in ((0, 1) if b < dmsh_ndsr.BLOCKS - 1 else (0,)):
                    order = (1, 0) if self.mutate == 'order' and b == 2 else (0, 1)
                    if self.schedule == 'chained':   # first calls of both patches into slots 0 and 2, then the second calls from them
                        for p in ps:
                            gr = order[0]; k = 2 * ps.index(p)
                            G0, G1 = Gd[p, gr, 0], Gd[p, gr, 1]
                            self.call((G0[0], G0[1], G1[0], G1[1]), weights(4 * b + 2 * gr + 1)[2 * nh:2 * nh + 2], ('0', '0'), 8 | 4 | (k // 2) << 8)
                        for p in ps:
                            gr = order[1]; k = 2 * ps.index(p)
                            G0, G1 = Gd[p, gr, 0], Gd[p, gr, 1]
                            Y[p].append(self.call((G0[0], G0[1], G1[0], G1[1]), weights(4 * b + 2 * gr + 1)[2 * nh:2 * nh + 2], ('0', '0'), 2 | (k // 2) << 8))
                    else:
                        for p in ps:
                            c = None
                            for gr in order:
                                G0, G1 = Gd[p, gr, 0], Gd[p, gr, 1]
                                c = self.call((G0[0], G0[1], G1[0], G1[1]), weights(4 * b + 2 * gr + 1)[2 * nh:2 * nh + 2], c or ('0', '0'), 0 if c else 8)
                            Y[p].append(c)
                for p in ps:
                    if b < dmsh_ndsr.BLOCKS - 1:
                        X[p] = [(self.relu(Y[p][0][nb], p == 2 and b == 1 and nb == 0), self.relu(Y[p][1][nb])) for nb in (0, 1)]
                        if b == 1: finals.setdefault('block1', Y[p][0][0])
                    else: finals[p] = Y[p][0][0]
        copies = {p: self.v('int', finals[p]) for p in range(0, dmsh_ndsr.PATCHES, 2)}   # some finals copied in the core, the others read straight from their call
        if self.dropped: self.lines = self.drop_call(self.lines)
        L = self.lines
        epi = [f'    _29[int({lid})] = uint({finals[0]});', dmsh_ndsr.BARRIER]   # the epilogue reuses the window's array, after the second barrier
        for p in range(dmsh_ndsr.PATCHES):
            w = copies.get(p, finals[p])
            if self.mutate == 'final' and p == 6: w = finals['block1']
            epi.append(f'    out._m0[{gid} * 9u + {p}u] = uint({w});')
        epi.append(f'    out._m0[{gid} * 9u + 8u] = _29[int(({lid} + 1u) & 127u)];')
        params = ('device const W& wts [[buffer(0)]], device const W& win_in [[buffer(1)]], device W& out [[buffer(2)]], uint3 gl_LocalInvocationID [[thread_position_in_threadgroup]], '
                  'uint3 gl_WorkGroupID [[threadgroup_position_in_grid]], uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]]')
        return (HEAD + 'struct W { uint _m0[1]; };\n' + HELPER + f'\nkernel void main0({params})\n{{\n' + '\n'.join(L + epi) + '\n}\n')

    def relu(self, d, bad=False):   # the translated ReLU of a D word's two halves, as spirv-cross writes it
        h = []
        for c in 'xy':
            v = self.v('float', f'float2(as_type<half2>(uint({d}))).{c}')
            sh = self.v('int', f'as_type<int>({v}) << 1')
            z = self.v('bool', f'{sh} == 0')
            m = self.v('float', f'precise::max({v}, {"0.5" if bad and self.mutate == "relu" else "0.0"})')
            h.append(self.v('float', f'{z} ? 0.0 : {m}'))
        u = self.v('uint', f'as_type<uint>(half2(float2({h[0]}, {h[1]})))')
        return self.v('int', f'int({u})')

    @staticmethod
    def drop_call(lines):
        i = max(i for i, ln in enumerate(lines) if 'dmsh_hmma16816_f16(' in ln)
        r = lines[i].split()[1]
        return lines[:i] + [f'    int {r} = 0;'] + lines[i + 1:]


def translated(**kw): return dmsh_rewrite.hmma_msl(Synthetic(**kw).source())


class Recognition(unittest.TestCase):
    def test_both_schedules_are_the_network(self):
        for schedule in ('plain', 'chained'):
            with self.subTest(schedule):
                src = translated(schedule=schedule)
                self.assertEqual(src.count('dmsh_hmma16816_f16(') - 1, dmsh_ndsr.CALLS)
                out, info = dmsh_ndsr.network(src, [128, 1, 1])
                self.assertEqual(info, {'split': {'fc': abi.SPLIT_FC, 'buffer': abi.SPLIT_SLOT, 'words': 8}}, info)
                body = out[out.index('kernel void main0'):]
                self.assertNotIn('dmsh_hmma16816_f16(', body)
                self.assertNotRegex(body, r'\b_34\b')   # the helper's scratch and matrices go with the calls
                self.assertNotIn('dm_acc', body)
                self.assertIn(f'[[buffer({abi.SPLIT_SLOT}), function_constant(dm_ndsr_split)]]', body)
                self.assertIn(f'dm_ndsr_p1 [[function_constant({abi.SPLIT_FC})]]', out)
                self.assertEqual(body.count('if (dm_ndsr_net) _29['), 11)   # pass 1 fills no window
                self.assertEqual(dmsh_ndsr.network(out, [128, 1, 1]), (out, None))   # its kernel calls no HMMA any more

    def test_changes_of_the_graph_are_refused(self):
        for kw, why in ((dict(mutate='weight'), 'not a final word'), (dict(mutate='window'), 'not a final word'), (dict(mutate='relu'), 'unknown origin'),
                        (dict(mutate='final'), 'not a final word'), (dict(mutate='order'), 'not a final word'), (dict(mutate='lane'), 'lane'),
                        (dict(calls_out=1), 'HMMA calls, not 176'), (dict(schedule='chained', mutate='order'), 'not a final word')):
            with self.subTest(**kw):
                src = translated(**kw)
                out, info = dmsh_ndsr.network(src, [128, 1, 1])
                self.assertEqual(out, src)
                self.assertIn(why, info.get('refused', ''), info)

    def test_other_kernels(self):
        src = translated()
        self.assertIn('local size', dmsh_ndsr.network(src, [64, 1, 1])[1]['refused'])
        self.assertIn('tg_spill', dmsh_ndsr.network(src, [128, 1, 1], spill=40000)[1]['refused'])
        plain = HEAD + 'kernel void main0(device uint* o [[buffer(0)]])\n{\n    o[0] = 1u;\n}\n'
        self.assertEqual(dmsh_ndsr.network(plain, [128, 1, 1]), (plain, None))

    def test_the_evaluator(self):   # the integer forms of spirv-cross the addresses use, per thread
        th = dmsh_ndsr.Threads({'_1': 'as_type<float>(gl_LocalInvocationID.x)', '_2': '(spvMulExtended<_9, uint>(as_type<uint>(_1), 2369637129u))._m1',
                                '_3': 'uint(int(uint(int(_2)) >> uint(4)))', '_4': 'int(_3) * (-29)', '_5': '6 & (as_type<int>(_1) * 2)', '_6': '-5 >> 1'},
                               {'_1': 'float', '_2': 'uint', '_3': 'uint', '_4': 'int', '_5': 'int', '_6': 'int'})
        x = list(range(dmsh_ndsr.THREADS))
        self.assertEqual(th.var('_3')[1], tuple(v // 29 for v in x))   # the compiler's division by 29
        self.assertEqual(th.var('_4')[1], tuple((-29 * (v // 29)) & 0xffffffff for v in x))
        self.assertEqual(th.var('_5')[1], tuple(6 & (2 * v) for v in x))
        self.assertEqual(th.var('_6')[1], (0xfffffffd,) * dmsh_ndsr.THREADS)   # arithmetic shift of an int
        with self.assertRaises(dmsh_ndsr.Refused): dmsh_ndsr.Threads({'_1': 'float(gl_LocalInvocationID.x) + 1.0'}, {'_1': 'float'}).var('_1')


RUNNER = r'''
#import <Metal/Metal.h>
#include <stdio.h>
#include <string.h>
// run <orig.metallib> <new.metallib> <weights> <window> <groups>: the outputs of the translated kernel, the rewritten one in one pass and in two, compared word for word
int main(int argc, char **argv) {
    @autoreleasepool {
        id<MTLDevice> dev = MTLCreateSystemDefaultDevice(); id<MTLCommandQueue> q = [dev newCommandQueue];
        NSData *wd = [NSData dataWithContentsOfFile:@(argv[3])], *xd = [NSData dataWithContentsOfFile:@(argv[4])]; int G = atoi(argv[5]);
        id<MTLBuffer> wb = [dev newBufferWithBytes:wd.bytes length:wd.length options:0], xb = [dev newBufferWithBytes:xd.bytes length:xd.length options:0];
        size_t on = (size_t)G * 128 * 9 * 4; id<MTLBuffer> out[3], words = [dev newBufferWithLength:(size_t)G * 128 * 32 options:MTLResourceStorageModePrivate];
        id<MTLComputePipelineState> ps[4];
        for (int v = 0; v < 4; v++) {
            NSError *e = nil; id<MTLLibrary> lib = [dev newLibraryWithURL:[NSURL fileURLWithPath:@(argv[v ? 2 : 1])] error:&e]; if (!lib) { printf("library: %s\n", e.description.UTF8String); return 2; }
            MTLFunctionConstantValues *cv = [MTLFunctionConstantValues new]; bool p1 = v == 3; if (v >= 2) [cv setConstantValue:&p1 type:MTLDataTypeBool atIndex:SPLIT_FC];
            id<MTLFunction> f = [lib newFunctionWithName:@"main0" constantValues:cv error:&e]; if (!f) { printf("function: %s\n", e.description.UTF8String); return 2; }
            ps[v] = [dev newComputePipelineStateWithFunction:f error:&e]; if (!ps[v]) { printf("pipeline: %s\n", e.description.UTF8String); return 2; }
        }
        for (int r = 0; r < 3; r++) { out[r] = [dev newBufferWithLength:on options:MTLResourceStorageModeShared]; memset(out[r].contents, 0xcd, on); }
        id<MTLCommandBuffer> cb = [q commandBuffer]; id<MTLComputeCommandEncoder> en = [cb computeCommandEncoder];
        for (int v = 0; v < 4; v++) {
            [en setComputePipelineState:ps[v]]; [en setBuffer:wb offset:0 atIndex:0]; [en setBuffer:xb offset:0 atIndex:1]; [en setBuffer:out[v < 3 ? v : 2] offset:0 atIndex:2];
            [en setBuffer:words offset:0 atIndex:SPLIT_SLOT];
            [en dispatchThreadgroups:MTLSizeMake(G, 1, 1) threadsPerThreadgroup:MTLSizeMake(128, 1, 1)];
            [en memoryBarrierWithScope:MTLBarrierScopeBuffers];
        }
        [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        if (cb.error) { printf("GPU: %s\n", cb.error.description.UTF8String); return 2; }
        const uint32_t *a = out[0].contents, *b = out[1].contents, *c = out[2].contents; long d1 = 0, d2 = 0, unwritten = 0, zero = 0;
        for (size_t i = 0; i < on / 4; i++) { d1 += a[i] != b[i]; d2 += a[i] != c[i]; unwritten += a[i] == 0xcdcdcdcd; zero += (a[i] & 0x7fff7fff) == 0; }
        printf("words %zu one-pass %ld two-pass %ld unwritten %ld\nzero %ld\n", on / 4, d1, d2, unwritten, zero);
        return 0;
    }
}
'''


def halves(values): return b''.join(struct.pack('<e', v) for v in values)


@unittest.skipUnless(shutil.which('xcrun') and subprocess.run(['xcrun', '-sdk', 'macosx', '-f', 'metal'], capture_output=True).returncode == 0, 'no Metal compiler')
class OnTheGPU(unittest.TestCase):
    def test_bit_exact_with_the_translated_calls(self):
        rnd = random.Random(1937)
        weights = halves(rnd.choice((0.0, -0.0)) if rnd.random() < 0.05 else rnd.uniform(-0.25, 0.25) for _ in range(12 * 256))
        window = halves(rnd.uniform(0.0, 1.2) if rnd.random() > 0.02 else rnd.choice((0.0, -0.0, -0.5)) for _ in range(GROUPS * 1408 * 2))
        for schedule in ('plain', 'chained'):
            with self.subTest(schedule), tempfile.TemporaryDirectory(prefix='ndsr_') as d:
                d = Path(d); src = translated(schedule=schedule)
                new, info = dmsh_ndsr.network(src, [128, 1, 1])
                self.assertIn('split', info)
                for name, text in (('orig', src), ('new', new)):
                    (d / f'{name}.metal').write_text(text)
                    r = subprocess.run(dmsh_chain.MAC_METAL + [str(d / f'{name}.metal'), '-o', str(d / f'{name}.air')], capture_output=True, text=True)
                    self.assertEqual(r.returncode, 0, r.stderr[-3000:])
                    subprocess.run(['xcrun', '-sdk', 'macosx', 'metallib', str(d / f'{name}.air'), '-o', str(d / f'{name}.metallib')], check=True)
                (d / 'w.bin').write_bytes(weights); (d / 'x.bin').write_bytes(window)
                (d / 'run.m').write_text(RUNNER.replace('SPLIT_FC', str(abi.SPLIT_FC)).replace('SPLIT_SLOT', str(abi.SPLIT_SLOT)))
                subprocess.run(['clang', '-fobjc-arc', '-O1', str(d / 'run.m'), '-framework', 'Metal', '-framework', 'Foundation', '-o', str(d / 'run')], check=True)
                r = subprocess.run([str(d / 'run'), str(d / 'orig.metallib'), str(d / 'new.metallib'), str(d / 'w.bin'), str(d / 'x.bin'), str(GROUPS)], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
                head, zero = r.stdout.split('\n')[:2]
                self.assertEqual(head.split(), ['words', str(GROUPS * 128 * 9), 'one-pass', '0', 'two-pass', '0', 'unwritten', '0'], r.stdout)
                self.assertLess(int(zero.split()[1]), GROUPS * 128 * 9 // 4, r.stdout)   # the network's output is not mostly zeros (ReLU of everything)


if __name__ == '__main__':
    unittest.main()
