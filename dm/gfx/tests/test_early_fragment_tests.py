"""Early fragment tests come from control +0x5b of an NVN2 pixel shader, unless its SPH writes depth (dmsh_build.py early_fragment_tests)."""
import ast
import os
from pathlib import Path
import struct
import sys
import tempfile
import unittest

path = Path(__file__).resolve().parents[1] / 'tools/dmsh_build.py'
nodes = [n for n in ast.parse(path.read_text()).body if (isinstance(n, ast.FunctionDef) and n.name in ('early_fragment_tests', 'job_for'))
         or (isinstance(n, ast.Assign) and getattr(n.targets[0], 'id', None) == 'NVN2_STAGE_INDEX')]
sys.path.insert(0, str(path.parent)); import dmsh_ampere, dmsh_abi
scope = {'struct': struct, 'os': os, 'dmsh_ampere': dmsh_ampere, 'abi': dmsh_abi, 'FACTS': {}}
exec(compile(ast.Module(body=nodes, type_ignores=[]), str(path), 'exec'), scope)
early = scope['early_fragment_tests']; job_for = scope['job_for']


def control2(flags=b'', size=0xe0, sub=0x11):   # an NVN2 control section (magic, version 0xc9, sub-version at +0x08), the pixel shader's flag bytes from +0x58
    c = bytearray(size); struct.pack_into('<III', c, 0, 0x98761234, 0xc9, sub)
    c[0x58:0x58 + len(flags)] = flags[:max(0, size - 0x58)]
    return bytes(c)


def sph2(word19=0):   # NVN2's 0x80-byte SPH (version 4)
    return struct.pack('<32I', *([0] * 19 + [word19] + [0] * 12)) + bytes(0x40)


class EarlyFragmentTestsNvn2(unittest.TestCase):
    def test_flag_set(self):   # +0x5b set, no depth
        self.assertTrue(early(control2(bytes([0, 0, 0, 1])), sph2()))
        self.assertTrue(early(control2(bytes([0, 0, 1, 1])), sph2()))   # +0x5a beside it

    def test_flag_clear(self):
        self.assertFalse(early(control2(), sph2()))
        for o in (0, 1, 2, 4, 5, 6, 7):   # the other fragment bytes are not the flag
            self.assertFalse(early(control2(bytes(o) + b'\1'), sph2()), o)

    def test_nvn1_offset_ignored(self):   # a long section's +0x718 (where NVN1 kept the flag) is not read
        c = bytearray(control2(size=0x880)); c[0x718] = 1
        self.assertFalse(early(bytes(c), sph2()))

    def test_depth_writer_left_late(self):   # +0x58 and SPH word 19 bit 1 go together; the SPH decides
        self.assertFalse(early(control2(bytes([1, 1, 0, 1, 0, 0, 0, 1])), sph2(2)))
        self.assertTrue(early(control2(bytes([0, 0, 0, 1])), sph2(1)))   # a sample mask output (bit 0) does not keep it late (the console tests early too)

    def test_sub_versions(self):   # the driver reads +0x5b in every control section version: 11, 0x11, 0x14 (Ocarina of Time) and the older ones
        for sub in (0, 2, 7, 11, 0x11, 0x14):
            self.assertTrue(early(control2(bytes([0, 0, 0, 1]), sub=sub), sph2()), sub)

    def test_short_control_section(self):
        self.assertFalse(early(control2(bytes([0, 0, 0, 1]), size=0x5b), sph2()))
        self.assertTrue(early(control2(bytes([0, 0, 0, 1]), size=0x5c), sph2()))

    def test_no_sph(self):
        self.assertFalse(early(control2(bytes([0, 0, 0, 1])), bytes(0x20)))


class JobFor(unittest.TestCase):   # the job a pixel shader record gives: the flag reaches it
    def job(self, code, ctl, stage='ps'):
        with tempfile.TemporaryDirectory() as w:
            return job_for('k', stage + '_k', bytes(0x38) + code + ctl, w)[0]

    def nvn2(self, flags, word19=0):
        code = sph2(word19)
        c = bytearray(control2(flags)); struct.pack_into('<III', c, 0x38, len(code), len(code), len(code))
        return code, bytes(c)

    def test_nvn2(self):
        self.assertTrue(self.job(*self.nvn2(bytes([0, 0, 0, 1])))['early_fragment_tests'])
        self.assertNotIn('early_fragment_tests', self.job(*self.nvn2(b'')))
        self.assertNotIn('early_fragment_tests', self.job(*self.nvn2(bytes([1, 0, 0, 1]), word19=2)))   # depth writer
        self.assertNotIn('early_fragment_tests', self.job(*self.nvn2(bytes([0, 0, 0, 1])), stage='vs'))


if __name__ == '__main__':
    unittest.main()
