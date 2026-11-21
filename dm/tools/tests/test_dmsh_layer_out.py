"""A vertex shader that writes the layer (dm/gfx/tools/dmsh_entry.py writes_layer): the chain and the host have no path for one (no vertex shader of Ocarina of Time writes it), so
dmsh_build.py fails such a shader by name. The test is main0's output struct, not the layer's name anywhere in the source. No game files, no Metal compiler."""
from pathlib import Path
import sys
from typing import Any
import unittest

TOOLS = Path(__file__).resolve().parents[3] / 'dm/gfx/tools'
sys.path.insert(0, str(TOOLS))
import dmsh_entry  # noqa: E402

ns: dict[str, Any] = dict(vars(dmsh_entry))
writes_layer, vs_depth = ns['writes_layer'], ns['vs_depth']

HEAD = '#include <metal_stdlib>\nusing namespace metal;\n\n'


def shader(outputs, body):
    return (HEAD + 'struct main0_out\n{\n' + ''.join(f'    {o};\n' for o in outputs) + '};\n\n'
            'vertex main0_out main0(uint gl_InstanceIndex [[instance_id]])\n{\n    main0_out out = {};\n' + body + '    return out;\n}\n')


PLAIN = shader(['float4 gl_Position [[position]]', 'float gl_PointSize [[point_size]]'], '    out.gl_Position = float4(0.0);\n    out.gl_PointSize = 1.0;\n')
LAYERED = shader(['float4 gl_Position [[position]]', 'uint gl_Layer [[render_target_array_index]]', 'float gl_PointSize [[point_size]]'],
                 '    out.gl_Position = float4(0.0);\n    out.gl_Layer = gl_InstanceIndex;\n    out.gl_PointSize = 1.0;\n')
SLICE = shader(['float4 gl_Position [[position]]', 'float4 m_1 [[user(locn1)]]', 'float gl_PointSize [[point_size]]'],
               '    out.gl_Position = float4(0.0);\n    out.m_1.x = as_type<float>(int(gl_InstanceIndex));\n    out.gl_PointSize = 1.0;\n')


class WritesLayer(unittest.TestCase):
    def test_main0_without_the_layer(self):
        self.assertFalse(writes_layer(PLAIN))
        self.assertFalse(writes_layer(SLICE))

    def test_main0_writing_the_layer(self):
        self.assertTrue(writes_layer(LAYERED))

    def test_the_depth_entry_points_keep_the_answer(self):
        for src, want in ((PLAIN, False), (LAYERED, True)):
            single, full = vs_depth(src)
            self.assertEqual(bool(writes_layer(single)), want); self.assertEqual(bool(writes_layer(full)), want)

    def test_the_name_outside_main0_out_does_not_count(self):
        other = PLAIN + 'struct other_out\n{\n    float4 gl_Position [[position]];\n    uint gl_Layer [[render_target_array_index]];\n};\n'
        self.assertFalse(writes_layer(other))

    def test_no_output_struct(self):
        self.assertFalse(writes_layer(HEAD + 'kernel void main0() {}\n'))

    def test_the_build_fails_such_a_shader_by_name(self):   # not skipped, not built without its layer
        build = (TOOLS / 'dmsh_build.py').read_text()
        self.assertRegex(build, r"if dmsh_entry\.writes_layer\(src\): return 'layer output', ")


if __name__ == '__main__':
    unittest.main()
