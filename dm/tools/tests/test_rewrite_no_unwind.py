"""patch_nso.py on a module built without unwind tables (an empty eh_frame_hdr, as GCC's -fno-unwind-tables leaves it: sloperinaGZ's subsdk9, the practice tools),
and its rewrite of mrs cntfrq_el0 to a host gate, on a synthetic NSO without game files"""
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest

REPO = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(REPO / 'dm/tools/tests'))
from test_nso import make_nso

MRS_CNTFRQ_X0, MRS_CNTPCT_X1, RET, NOP = 0xD53BE000, 0xD53BE021, 0xD65F03C0, 0xD503201F


def words(*w): return b''.join(struct.pack('<I', x) for x in w)


def module():
    """text: the module header (0, MOD0's offset), then mrs x0, cntfrq_el0; mrs x1, cntpct_el0; ret. ro: MOD0 with an empty eh_frame_hdr. data: an empty dynamic section"""
    text = words(0, 0x1000, MRS_CNTFRQ_X0, MRS_CNTPCT_X1, RET, NOP, NOP, NOP)
    ro = bytearray(0x40); ro[:4] = b'MOD0'
    struct.pack_into('<6i', ro, 4, 0x2000 - 0x1000, 0x2010 - 0x1000, 0x2020 - 0x1000, 0x40, 0x40, 0x2010 - 0x1000)   # dynamic, bss start / end, eh_frame_hdr start = end, module object
    return make_nso((text, bytes(ro), bytes(16)), compress=(0, 0, 0), bss=0x10)


class NoUnwindTables(unittest.TestCase):
    def test_rewrite(self):
        with tempfile.TemporaryDirectory() as tmp:
            src = Path(tmp) / 'subsdk9.nso'; src.write_bytes(module())
            r = subprocess.run([sys.executable, str(REPO / 'dm/tools/patch_nso.py'), str(src), tmp, 'subsdk9'], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr[-2000:])
            gates = [l.split('\t') for l in (Path(tmp) / 'subsdk9.gates.tsv').read_text().splitlines() if l and not l.startswith('#')]
            self.assertEqual(sorted((k, a, o) for _, k, a, o in gates), [('cntfrq', '0', '0x8'), ('cntpct', '1', '0xc')], 'both timer reads go to the host gate, their registers kept')
            img = (Path(tmp) / 'subsdk9.patched.bin').read_bytes()
            for off in (8, 12):
                self.assertEqual(struct.unpack_from('<I', img, off)[0] >> 26, 0x05, f'+{off:#x} is a b to its gate block, not the raw mrs')


if __name__ == '__main__':
    unittest.main()
