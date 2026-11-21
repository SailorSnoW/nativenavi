#!/usr/bin/env python3
"""A subgroup shuffle of a literal is the literal (dmsh_rewrite.py shuffle_literal): every lane holds it, so the lane read gives it back. Ocarina of Time's mesh shaders
hold 32 such shuffles of the constant 2 in a row, and Apple's M2 mesh compiler dies on one of them (shaders.md, A shuffle of a constant crashes Apple's M2 mesh compiler).
The pass folds a literal's shuffle whatever the lane argument, leaves a shuffle of a value alone, and spirv-cross's helper definitions too; it is idempotent, and with
xcrun the folded shader compiles as a mesh function."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS))
from dmsh_rewrite import shuffle_literal   # noqa: E402

HELPERS = '''#include <metal_stdlib>
using namespace metal;

template<typename T>
inline T spvSubgroupShuffle(T value, ushort lane)
{
    return simd_shuffle(value, lane);
}

template<>
inline ulong spvSubgroupShuffle(ulong value, ushort lane)
{
    return as_type<ulong>(spvSubgroupShuffle(as_type<uint2>(value), lane));
}
'''
# The mesh shaders' count of what the lanes before this one put out: the translator's constant propagation left the per-lane value as a literal
BODY = '''
struct dm_vertex_t { float4 pos [[position]]; };
[[mesh]] void main0(uint gl_SubgroupInvocationID [[thread_index_in_simdgroup]], mesh<dm_vertex_t, void, 64, 32, topology::triangle> out)
{
    uint _748 = uint((simd_vote::vote_t)simd_ballot(true));
    float _750 = spvSubgroupShuffle(2.8025969286496341418474591665798e-45, 0u);
    int _756 = ((int(_748) & 1) != 0) ? as_type<int>(_750) : 0;
    float _758 = spvSubgroupShuffle(2.8025969286496341418474591665798e-45, 1u);
    int _760 = _756 + as_type<int>(_758);
    float _6144 = spvSubgroupShuffle(as_type<float>(_760), uint(gl_SubgroupInvocationID));
    out.set_primitive_count(uint(_760) + as_type<uint>(_6144));
}
'''
LITERAL = '2.8025969286496341418474591665798e-45'


class ShuffleLiteral(unittest.TestCase):
    def test_a_literal_is_folded_whatever_the_lane(self):
        for call, want in (('spvSubgroupShuffle(%s, 0u)' % LITERAL, LITERAL), ('spvSubgroupShuffle(1.0, uint(_6140))', '1.0'), ('spvSubgroupShuffle(7u, _x)', '7u'),
                           ('spvSubgroupShuffle(0x7fu, 31u)', '0x7fu'), ('spvSubgroupShuffle(-3, 0u)', '(-3)')):
            self.assertEqual(shuffle_literal(f'int a = b + {call};'), f'int a = b + {want};', call)

    def test_a_value_and_the_helpers_stay(self):
        for text in ('spvSubgroupShuffle(as_type<float>(_6112), uint(_6140))', 'spvSubgroupShuffle(_12, 3u)', 'spvSubgroupShuffle(value.x, lane)', HELPERS, 'float4 x = y;'):
            self.assertEqual(shuffle_literal(text), text)

    def test_the_shader_keeps_only_the_shuffle_of_a_value(self):
        out = shuffle_literal(HELPERS + BODY)
        self.assertEqual(out.count('spvSubgroupShuffle(' + LITERAL), 0)
        self.assertEqual(out.count(f'float _750 = {LITERAL};'), 1)
        self.assertEqual(out.count('spvSubgroupShuffle(as_type<float>(_760), uint(gl_SubgroupInvocationID))'), 1)
        self.assertEqual(shuffle_literal(out), out)

    @unittest.skipUnless(shutil.which('xcrun'), 'needs the Metal compiler')
    def test_the_folded_shader_compiles(self):
        with tempfile.TemporaryDirectory() as d:
            for name, text in (('plain', HELPERS + BODY), ('folded', shuffle_literal(HELPERS + BODY))):
                src = Path(d) / f'{name}.metal'; src.write_text(text)
                r = subprocess.run(['xcrun', '-sdk', 'macosx', 'metal', '-c', '-O1', str(src), '-o', str(Path(d) / f'{name}.air')], capture_output=True, text=True)
                self.assertEqual(r.returncode, 0, r.stderr)


if __name__ == '__main__':
    unittest.main()
