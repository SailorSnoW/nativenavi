#!/usr/bin/env python3
"""Ampere memory and image instructions through the offline translator (dmsh_xlate, sm86); needs DMSH_TEST_XLATE."""
import json, os, re, struct, subprocess, sys, tempfile, unittest
from pathlib import Path

TOOLS = Path(__file__).resolve().parents[1] / 'tools'
sys.path.insert(0, str(TOOLS)); import dmsh_chain   # noqa: E402  (the chain's MSL version)
EXIT = (0x000fea0003800000, 0x000000000000794d)
# SUATOM.ADD.U32 R12, [R3], R12 on the 1D image at c0 word 12 (image slot 1)
SUATOM = (0x002ea200001ee000, 0x00000c0c030c7593)


def inst(hi, lo): return struct.pack('<QQ', lo, hi)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class Translated(unittest.TestCase):
    def translate(self, code, atomics=False, **extra):
        with tempfile.TemporaryDirectory(prefix='ampere_mem_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(code)
            # c0 (QueryAmpereComputeC0Layout): 3 texture handles from 0x10, image handles from 0x28, 0x40 bytes
            job = [dict(key='k', stage='cs', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], debug=True, nvn2_cs=extra.pop('nvn2_cs', [0, 0, 3, 0x40, 0]), **extra)]
            (work / 'jobs.json').write_text(json.dumps(job))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
            self.assertTrue((work / 'k.spv').exists(), r.stdout + r.stderr)
            self.res = json.loads((work / 'k.res.json').read_text())
            sys.path.insert(0, str(TOOLS))
            r = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), *dmsh_chain.CROSS_MSL, str(work / 'k.spv')], capture_output=True, text=True)
            import dmsh_atomic
            self.assertEqual(r.returncode, 0, r.stderr)
            return dmsh_atomic.check_msl(r.stdout) if atomics else r.stdout

    def refused(self, code):
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / 'k.bin').write_bytes(code)
            (Path(d) / 'jobs.json').write_text(json.dumps([dict(key='k', stage='cs', arch='sm86', code=str(Path(d) / 'k.bin'), tex={}, img={}, attr={}, fout={}, local=[32, 1, 1], nvn2_cs=[0, 0, 3, 0x40, 0])]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(Path(d) / 'jobs.json'), d], capture_output=True, text=True)
            self.assertFalse((Path(d) / 'k.spv').exists(), r.stdout + r.stderr)
            return r.stdout + r.stderr


class GuardedWrites(Translated):
    P0 = (0x000fda0003f06070, 0x00c0060005007a0c)   # P0 = R5 >= c3[0x18]
    MOV_R5 = (0x000fe20000000f00, 0x0100000000057a02)

    def test_a_guarded_load_reads_its_address_before_the_false_guards_zero(self):
        # UR4 = c3[0x10]; LDG.!P0 R5, [R5 + UR4]: zeroing R5 first would read at the table's start
        ur4, ldg = (0x000fe20000000a00, 0x00c0040000047ab9), (0x000ea200081e890f, 0x0000000405057981)
        waits = (0x004ea200001ee000, 0x00000c05050c7593)
        msl = self.translate(inst(*self.MOV_R5) + inst(*ur4) + inst(*self.P0) + inst(*ldg) + inst(*waits) + inst(*EXIT))
        self.assertNotIn('_m0[0]);', msl.split('kernel')[1].replace('_m0[0].x', ''))
        self.assertRegex(msl, r'_m0\[(int\()?\w+')

    def test_a_deferred_write_under_a_false_guard_writes_nothing(self):
        # MUFU.EX2 R17 under P0; R17 = R29 * R29 under !P0; R17 = R36 * R17 after the wait, which must not copy the old R17 over !P0's
        ex2, other, used = (0x000ee20000000800, 0x0000002000170308), (0x000fe20000410000, 0x0000001d1d178220), (0x008fe20000410000, 0x0000001724177220)
        msl = self.translate(inst(*self.MOV_R5) + inst(*self.P0) + inst(*ex2) + inst(*other) + inst(*used) + inst(SUATOM[0], 0x00000c17030c7593) + inst(*EXIT))
        self.assertRegex(msl, r'\? 1065353216 : 0;')
        self.assertRegex(msl, r'bool \w+ = \w+ != 0;\s+float \w+ = \w+;\s+if \(\w+\)')

    def test_uniform_handle_forms_translate(self):
        for code in ((0x000f4200081e0f04, 0x3000040702067f63), (0x000142000c1e03ff, 0x2000040400167f6c), (0x00014200001e0f12, 0x3800001002101364)):   # TLD4 and TXD through a uniform register, TLD4 through a register
            self.translate(inst(*code) + inst(*EXIT))

    def test_a_computed_handle_reads_the_pools(self):
        # TLD4 through c4[0] & 0x7fff | 0x10800000: a handle no table holds, which BindlessElimination must keep
        mov, mask, sampler = (0x000fe20000000f00, 0x0100000000107a02), (0x000fe400078ec0ff, 0x00007fff10107812), (0x000fe400078efcff, 0x1080000010107812)
        tld4 = (0x00014200001e0f12, 0x3800001002101364)
        msl = self.translate(inst(*mov) + inst(*mask) + inst(*sampler) + inst(*tld4) + inst(0x020ea200001ee000, 0x00000c10030c7593) + inst(*EXIT))
        self.assertIn('cb0_40000000', msl)
        self.assertIn('gather(', msl)

    def test_size_query_through_a_uniform_register_translates(self):   # TXQ R10, UR6, R10 (0xf6f)
        msl = self.translate(inst(0x000f4200080001ff, 0x000006000a0a7f6f) + inst(SUATOM[0], 0x00000c0a030c7593) + inst(*EXIT))
        self.assertIn('get_width(', msl)

    def test_lod_query_handle_forms_translate(self):
        for code in ((0x000f4200080002ff, 0x600004ff20277f69), (0x000f4200000002ff, 0x68000018141d736a)):
            self.translate(inst(*code) + inst(*EXIT))

    def test_gradients_through_a_uniform_register_keep_their_handle(self):
        # TXD through UR4 = c3[0x10]: the translator takes source 0 as the handle (else it deletes the read)
        ur4, txd, used = (0x000fe20000000a00, 0x00c0040000047ab9), (0x000142000c1e03ff, 0x2000040400167f6c), (0x020ea200001ee000, 0x00000c16030c7593)
        msl = self.translate(inst(*ur4) + inst(*txd) + inst(*used) + inst(*EXIT))
        self.assertIn('gradient', msl)

    def test_handle_under_a_uniform_vote_of_the_reads_guard(self):
        # UP0 = vote.all(P0); if (!UP0) UR4 |= UR8; TEX.!P0 through UR4
        code = [(0x000fe20003f05070, 0x01000500ff007a0c), (0x000fe20000000800, 0x00c0180000087ab9), (0x000fe20000000800, 0x00c0140000047ab9), (0x000fe40000000000, 0x00000000003f7886),
                (0x000fe2000f8efc3f, 0x0000000408048292), (0x000f42000c9e0f16, 0xb000042420188f60), (0x020ea200001ee000, 0x00000c16030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.sample(', msl)
        self.assertIn('cp_t_cb262147_10050018', msl)

    def test_handle_under_a_guard_the_reads_guard_implies(self):
        # if (P4) R16 = c3[0x28] | c3[0x70]; TEX.P5 through R16
        code = [(0x040fe20003f85070, 0x000000070800780c), (0x000fc40003f25070, 0x000000040800780c), (0x000fe40000fa8170, 0x000000000000781c), (0x000fc60000000f00, 0x00c00a0000104a02),
                (0x000fc600078efcff, 0x00c01c0010104a12), (0x00014200049e03ff, 0x380000100e125361), (0x020ea200001ee000, 0x00000c12030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.sample(', msl)
        self.assertIn('cp_t_cb262147_1007000A', msl)

    def test_handle_with_only_its_first_word_under_a_guard(self):
        # R16 = c3[0x28] under P4; R16 |= c3[0x70]; TEX through R16
        code = [(0x040fe20003f85070, 0x000000070800780c), (0x000fc60000000f00, 0x00c00a0000104a02), (0x000fc600078efcff, 0x00c01c0010107a12), (0x00014200049e03ff, 0x380000100e127361),
                (0x020ea200001ee000, 0x00000c12030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.sample(', msl)
        self.assertIn('cp_t_cb262147_1007000A', msl)

    def test_first_word_of_a_handle_is_not_read_as_a_handle_of_its_own(self):
        code = [(0x040fe20003f85070, 0x000000070800780c), (0x000fc40003f25070, 0x000000040800780c), (0x000fe40000fa8170, 0x000000000000781c), (0x000fc60000000f00, 0x00800a0000104a02),
                (0x000fc600078efcff, 0x00801c0010104a12), (0x00014200049e03ff, 0x380000100e125361), (0x020ea200001ee000, 0x00000c12030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertEqual(msl.count('.sample('), 1)
        self.assertRegex(msl, r'_1007000A\b')

    def test_gather_through_a_register_past_a_branch_on_the_guard(self):
        # BRA.!P6 past the read; TLD4 through R49 (.SCR)
        code = [(0x000fe20003fc5070, 0x01000500ff007a0c), (0x000fe20000000f00, 0x00c0060000316a02), (0x000fe200078efcff, 0x00c01a0031316a12), (0x000fea0003800000, 0x000000200000e947),
                (0x000f4200001e0f0c, 0x38000031020e7364), (0x020ea200001ee000, 0x00000c0c030c7593), EXIT]
        msl = self.translate(b''.join(inst(*c) for c in code))
        self.assertIn('.gather(', msl)
        self.assertIn('cp_t_cb262147_10068006', msl)


class Suatom(Translated):
    def test_handle_in_the_games_uniform_buffer(self):
        msl = self.translate(inst(SUATOM[0], SUATOM[1] | 3 << 54) + inst(*EXIT))
        self.assertIn('cp_i_cb3_C_r32ui', msl)
        self.assertIn('atomic_fetch_add', msl)

    # the register forms (0x394, 0x396, 0x3a0; SULD / SUST 0x998..0x99e): the handle in Rc (64..72)
    MOV_R12_C3 = (0x003fde0000000f00, 0x00c00000000c7a02)   # MOV R12, c[3][0]

    def test_handle_in_a_register(self):   # SUATOM.ADD.U32 R12, [R12], R24 on a 1D buffer image
        msl = self.translate(inst(*self.MOV_R12_C3) + inst(0x00321e00001ee00c, 0x200000180c0c7394) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)
        self.assertIn('cp_i_cb3_', msl)

    def test_reduction_with_its_handle_in_a_register(self):
        msl = self.translate(inst(*self.MOV_R12_C3) + inst(0x000fe2000310e00c, 0xa00000010800759f - 0x59f + 0x3a0) + inst(*EXIT))
        self.assertIn('atomic_fetch_or', msl)
        self.assertIn('cp_i_cb3_', msl)

    def test_sured_takes_its_operation(self):
        self.assertIn('atomic_fetch_or', self.translate(inst(0x000fe2000310e000, 0xa0c002010800759f) + inst(*EXIT)))

    def test_add_u32_on_a_1d_image(self):
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)
        self.assertIn('cp_i_tcb_4A_r32ui', msl)

    def test_images_come_after_the_separate_handles(self):
        msl = self.translate(inst(*SUATOM) + inst(*EXIT), nvn2_cs=[0, 0, 1, 0x40, 0, 1, 1])
        self.assertIn('cp_i_tcb_4A_r32ui', msl)

    def test_result_kept(self):
        msl = self.translate(inst(*SUATOM) + inst(*EXIT))
        self.assertRegex(msl, r'uint \w+ = [^;]*atomic_fetch_add')


class MufuTurns(Translated):
    """MUFU.SIN / COS take turns: t - rint(t) before the 2pi multiply (Metal's fast sin / cos return 0 past 2^20 turns); GPU values in t_mufu_turns.m"""
    COS = (0x000fe60000000000, 0x0000000c000c7308)   # MUFU.COS R12, R12 (op 0, bits 74..80)
    SIN = (0x000fe60000000400, 0x0000000c000c7308)   # MUFU.SIN R12, R12 (op 1)

    def lowered(self, mufu, fn):
        msl = self.translate(inst(*mufu) + inst(*SUATOM) + inst(*EXIT))
        m = re.search(r'float (_\d+) = rint\(([\w.]+)\);\s*float (_\d+) = (?:spvFSub\(\2, \1\)|\2 - \1);\s*float (_\d+) = (?:spvFMul\(\3, 6\.283185482025146484375\)|\3 \* 6\.283185482025146484375);'
                      r'\s*float _\d+ = ' + fn + r'\(\4\);', msl)
        self.assertTrue(m, msl)

    def test_cos(self): self.lowered(self.COS, 'cos')
    def test_sin(self): self.lowered(self.SIN, 'sin')


class Bmsk(Translated):
    MOVS = inst(0x000fe20000000f00, 0x0100000000037a02) + inst(0x000fe20000000f00, 0x0100010000047a02)
    BMSK = (0x000fe40000000000, 0x000000040303721b)   # BMSK R3, R3, R4: clamping
    WRAP = (0x000fe40000000800, 0x000000040303721b)

    def msl(self, op):
        return self.translate(self.MOVS + inst(*op) + inst(*SUATOM) + inst(*EXIT))

    def test_a_clamping_mask_tests_its_operands(self):
        self.assertGreaterEqual(self.msl(self.BMSK).count('>= 32u'), 3)

    def test_a_wrapping_mask_tests_only_their_sum(self):
        self.assertEqual(self.msl(self.WRAP).count('>= 32u'), 1)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class PixelCovered(unittest.TestCase):
    COVERED = (0x040e0200000e4000, 0x0000000000007925)
    OTHER = (0x040e0200000e8000, 0x0000000000007925)
    SAMPLE = (0x040e0200000ec000, 0x0000000000007925)   # bits 78..80 = 3: the sample index
    SPH = struct.pack('<32I', *([0x000c5482] + [0] * 4 + [0xb0000000] + [0] * 12 + [0xf] + [0] * 13))

    def run_job(self, *ops):
        with tempfile.TemporaryDirectory(prefix='ampere_px_') as d:
            work = Path(d); (work / 'k.bin').write_bytes(self.SPH + b''.join(inst(*op) for op in ops) + inst(*EXIT))
            (work / 'jobs.json').write_text(json.dumps([dict(key='k', stage='ps', arch='sm86', code=str(work / 'k.bin'), tex={}, img={}, attr={}, fout={}, debug=True)]))
            r = subprocess.run([os.environ['DMSH_TEST_XLATE'], str(work / 'jobs.json'), d], capture_output=True, text=True)
            if not (work / 'k.spv').exists(): return None, r.stdout + r.stderr
            sys.path.insert(0, str(TOOLS))
            m = subprocess.run([str(TOOLS / 'spirv_cross_bin/spirv-cross'), '--msl', '--msl-version', dmsh_chain.MSL_VERSION, '--msl-argument-buffers', str(work / 'k.spv')], capture_output=True, text=True)
            self.assertEqual(m.returncode, 0, m.stderr)
            return m.stdout, r.stdout + r.stderr

    def test_reads_the_helper_invocation(self):
        msl, log = self.run_job(self.COVERED)
        self.assertIsNotNone(msl, log)
        self.assertIn('simd_is_helper_thread()', msl)
        self.assertRegex(msl, r'\? 0 : 1;')

    def test_another_form_is_refused(self):
        msl, log = self.run_job(self.OTHER)
        self.assertIsNone(msl)
        self.assertIn('0x925 form 2', log)

    def test_form_3_reads_the_sample_index(self):
        msl, log = self.run_job(self.SAMPLE)
        self.assertIsNotNone(msl, log)
        self.assertIn('[[sample_id]]', msl)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class IpaAtOffset(unittest.TestCase):
    """IPA at an offset (location 2): the centre's value plus the offset times its screen derivatives"""
    run_job = PixelCovered.run_job
    SPH = PixelCovered.SPH
    SAMPLE_R5 = (0x040e0200000ec000, 0x0000000000057925)
    BARY_AT = (0x000ee200000e20ed | 0x3f << 52, 0x0000000500007326)
    BARY = (0x000ee200000e00ed, 0x000000ff00007326)

    def test_reads_the_centre_value_and_its_derivatives(self):
        msl, log = self.run_job(self.SAMPLE_R5, self.BARY_AT)
        self.assertIsNotNone(msl, log)
        self.assertIn('[[barycentric_coord', msl)
        self.assertRegex(msl, r'float (_\d+) = in\.gl_BaryCoordEXT\.x;\n\s*float (_\d+) = gl_FragCoord\.w;\n\s*float _\d+ = spvFMul\(\1, \2\);')
        self.assertRegex(msl, r'float (_\d+) = dfdx\((_\d+)\);\n\s*float (_\d+) = dfdy\(\2\);')
        self.assertRegex(msl, r'int (_\d+) = as_type<int>\(_\d+\) << 20;\n\s*int _\d+ = \1 >> 28;')   # x: bits 8..11, signed
        self.assertRegex(msl, r'int (_\d+) = as_type<int>\(_\d+\) << 4;\n\s*int _\d+ = \1 >> 28;')    # y: bits 24..27
        self.assertIn('0.0625', msl)

    def test_a_centre_read_takes_no_derivative(self):
        msl, log = self.run_job(self.BARY)
        self.assertIsNotNone(msl, log)
        self.assertIn('[[barycentric_coord', msl)
        self.assertNotIn('dfdx(', msl)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class ShadingRate(unittest.TestCase):
    """S2UR of SR 0x84, the fragment size (X bits 8..15, Y 16..23): the host shades every pixel, so 1 x 1, not 0 (a colour divided by the area would be NaN)"""
    run_job = PixelCovered.run_job
    SPH = PixelCovered.SPH
    S2UR = (0x000e220000008400, 0x00000000000479c3)   # S2UR UR4, SR 0x84
    I2F_R0 = (0x03ffe20008201400, 0x0000000400007c45)   # I2F R0, UR4: 65792 for 1 x 1

    def test_reads_one_by_one(self):
        msl, log = self.run_job(self.S2UR, self.I2F_R0)
        self.assertIsNotNone(msl, log)
        self.assertIn('float(65792)', msl)


@unittest.skipUnless(os.environ.get('DMSH_TEST_XLATE'), 'set DMSH_TEST_XLATE to the offline translator')
class DepthRegister(unittest.TestCase):
    """depth follows the colours at four registers per enabled target: OmapTargets 0x10f puts it in R9, not R6 where a component count would"""
    run_job = PixelCovered.run_job
    SPH = struct.pack('<32I', *([0x000c5482] + [0] * 4 + [0xb0000000] + [0] * 12 + [0x10f, 0x2] + [0] * 12))   # colours 0x10f, depth written (+0x4c bit 1)

    @staticmethod
    def mov(reg, value): return (0x000fe20000000f00, value << 32 | reg << 16 | 0x7802)

    def test_depth_comes_after_the_last_target_block(self):
        msl, log = self.run_job(self.mov(6, 0x3e800000), self.mov(9, 0x3f400000))
        self.assertIsNotNone(msl, log)
        self.assertRegex(msl, r'gl_FragDepth = 0\.75')


class MufuF16(Translated):
    """MUFU.F16: the source half from bits 60..61 (bit 62 its abs), the result the low half"""
    LOG2_HIGH = (0x000fe60000000e00, 0x5000000c000c7308)   # MUFU.F16.LOG2 R12, |R12.H1|
    LOG2_LOW = (0x0002a20000000e00, 0x4000000c000c7308)    # MUFU.F16.LOG2 R12, |R12.H0|

    def msl(self, mufu):
        return self.translate(inst(*mufu) + inst(*SUATOM) + inst(*EXIT))

    def test_high_half(self):
        msl = self.msl(self.LOG2_HIGH)
        self.assertRegex(msl, r'as_type<half2>\(.*\)\)\.y;')
        self.assertIn('log2(abs', re.sub(r'float _\d+ = abs\((_\d+)\);\s*float _\d+ = log2\(_\d+\);', 'log2(abs', msl))

    def test_result_is_the_low_half(self):
        self.assertRegex(self.msl(self.LOG2_HIGH), r'as_type<uint>\(half2\(float2\(\w+, 0\.0\)\)\)')

    def test_low_half_translates_apart(self):
        self.assertNotRegex(self.msl(self.LOG2_LOW), r'as_type<half2>\(.*\)\)\.y;')


class Sured(Translated):
    FORM = (0x000fe2000010e000, 0x60000c000400759f)

    def test_adds_at_the_coordinates(self):
        msl = self.translate(inst(*self.FORM) + inst(*EXIT))
        self.assertIn('atomic_fetch_add', msl)
        self.assertIn('texture2d', msl)

    def test_another_form_is_refused(self):
        with self.assertRaises(AssertionError) as e: self.translate(inst(0x000fe200001ee000, self.FORM[1]) + inst(*EXIT))
        self.assertIn('SURED form', str(e.exception))


class StageMemory(Translated):
    MEMORY = dict(stage_memory=[0, 0x10, 0x120, 0x400], shared=0x800)
    WORKGROUP = (0x000fe20000007800, 0x00000000ff007923)   # ISBERD R0, mode 0x78
    INPUT = (0x000fe20000003800, 0x00002000ff137923)       # ISBERD R19, [RZ + 0x20], mode 0x38
    PAYLOAD = (0x0041e8000000b800, 0x0000100201007927)     # 0x927 [R1 + 0x10], R2, mode 0xb8 (a task's record)
    KEEP_R0 = (0x0041e8000000b800, 0x00001000ff007927)
    KEEP_R19 = (0x0041e8000000b800, 0x00001013ff007927)
    ATTRIBUTE = (0x0001e2000000f800, 0x0000800307007927)   # 0x927 [R7 + 0x80], R3, mode 0xf8 (a vertex's y)
    COUNT_ZERO = (0x000fe20000008800, 0x000003ffff007927)  # 0x927 [RZ + 3], RZ, mode 0x88: every thread clears the count
    COUNT = (0x0001e20000008800, 0x00000307ff007927)
    INDEX = (0x0001e20000008000, 0x0000040605057927)       # 0x927 [R5 + 4], R6, mode 0x80: an index byte

    def msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in code) + inst(*EXIT), **self.MEMORY)

    def test_workgroup_number_reads_word_0(self):
        self.assertRegex(self.msl(self.WORKGROUP, self.KEEP_R0), r'as_type<float>\(_\d+\[0\]\)')

    def test_input_is_at_its_offset(self):
        self.assertRegex(self.msl(self.INPUT, self.KEEP_R19), r'as_type<float>\(_\d+\[12\]\)')

    def test_subword_inputs_are_unsigned_and_keep_the_byte_offset(self):
        for mode, offset, bits in ((0x34, 0x22, 16), (0x30, 0x23, 8)):
            read = (self.INPUT[0] & ~0xff00 | mode << 8, self.INPUT[1] & ~(0xffffff << 40) | offset << 40)
            src = self.msl(read, self.KEEP_R19)
            self.assertIn('[12]', src)
            self.assertRegex(src, rf'uint\(as_type<int>\(\w+\)\) >> uint\({(offset & 3) * 8}\)')
            self.assertIn(f'& {(1 << bits) - 1};', src)

    def test_subword_payload_stores_use_their_width(self):
        for mode, bits in ((0xb4, 16), (0xb0, 8)):
            write = (self.PAYLOAD[0] & ~0xff00 | mode << 8, self.PAYLOAD[1])
            self.assertRegex(self.msl(write), rf'void dmsh_shared_store{bits}_\d+\(')

    def test_subword_vertex_records_keep_the_offset_within_the_word(self):
        read = (self.READBACK[0] & ~0xff00 | 0xb400, self.READBACK[1] & ~(0xffffff << 40) | 0x202 << 40)
        src = self.translate(inst(*read) + inst(*self.KEEP_R8) + inst(*EXIT), stage_memory=[0, 0x10, 0x120, 0x400, 0x200, 6, 128 * 24], shared=0x800)
        self.assertIn('[72]', src)
        self.assertIn('>> uint(16)', src)
        self.assertIn('& 65535;', src)

    def test_word_store_goes_to_the_outputs(self):
        self.assertIn('0 + 416', self.msl(self.ATTRIBUTE))

    READBACK = (0x000fe6000000f800, 0x00018000ff087923)   # ISBERD R8, [RZ + 0x180], mode 0xf8
    KEEP_R8 = (0x0041e8000000b800, 0x00001008ff007927)

    def test_an_attribute_word_is_read_back_from_the_outputs(self):
        self.assertRegex(self.msl(self.READBACK, self.KEEP_R8), r'as_type<float>\(_\d+\[168\]\)')

    def test_the_zero_count_is_left_out(self):   # threads are not in step on Metal: it could land after the real count
        without = self.msl(self.COUNT)
        with_zero = self.msl(self.COUNT_ZERO, self.COUNT)
        self.assertEqual(without.count('atomic_compare_exchange'), with_zero.count('atomic_compare_exchange'))

    def test_a_byte_goes_through_its_word_atomically(self):
        self.assertIn('atomic_compare_exchange', self.msl(self.INDEX))

    def test_vertex_records_go_to_their_soa_word(self):
        mesh = self.translate(inst(*self.PAYLOAD) + inst(*EXIT), stage_memory=[0, 0x10, 0x120, 0x400, 0x200, 6, 128 * 24], shared=0x800)
        self.assertRegex(mesh, r'< 3072u')
        self.assertRegex(mesh, r'\* 43691u?\b')   # v = w * ceil(2^18 / 6) >> 18
        self.assertRegex(mesh, r'\* 768u?\b')
        self.assertNotRegex(self.msl(self.PAYLOAD), r'43691')

    def test_vertex_records_of_many_components_divide_exactly(self):   # vertex 109 for word 5609: 110 * 51 = 5610 > 5609 >= 109 * 51 + 50
        mesh = self.translate(inst(*self.PAYLOAD) + inst(*EXIT), stage_memory=[0, 0x10, 0x120, 0x400, 0x200, 51, 160 * 4 * 51], shared=0x8000)
        self.assertRegex(mesh, r'< 32640u')
        self.assertNotRegex(mesh, r'\* %du?\b' % (((1 << 18) + 50) // 51))
        self.assertRegex(mesh, r'/ 51\b')

    def test_outside_a_task_or_mesh_shader_it_is_refused(self):
        with self.assertRaises(AssertionError) as e: self.translate(inst(*self.PAYLOAD) + inst(*EXIT))
        self.assertIn('outside a task or mesh shader', str(e.exception))


class BarrierMoves(Translated):
    """BMOV to and from convergence barriers does nothing per thread"""
    SAVE = (0x000fe20000000000, 0x000000001a0f7f55)
    RESTORE = (0x000fe40000100000, 0x000000001a0f7f56)
    RESTORE_PLAIN = (0x000fe40000000000, 0x000000001a0f7f56)

    def test_no_effect_on_the_shader(self):
        plain = self.translate(inst(*SUATOM) + inst(*EXIT))
        moved = self.translate(inst(*self.SAVE) + inst(*self.RESTORE) + inst(*SUATOM) + inst(*self.RESTORE_PLAIN) + inst(*EXIT))
        self.assertEqual(moved, plain)


class Yield(Translated):
    """YIELD (0x946) does nothing per thread"""
    YIELD = (0x000fde0003800000, 0x0000000000007946)

    def test_no_effect_on_the_shader(self):
        plain = self.translate(inst(*SUATOM) + inst(*EXIT))
        yielded = self.translate(inst(*SUATOM) + inst(*self.YIELD) + inst(*EXIT))
        self.assertEqual(yielded, plain)


class LdcIndexedBank(Translated):
    """LDC.64 .IS (address mode 2): bank = the instruction's + Ra's upper half, offset = Ra's lower half"""
    LDC_IS = (0x000e620000008a00, 0x00000000030c7b82)

    def test_reads_the_bank_chosen_at_run_time(self):
        msl = self.translate(inst(*self.LDC_IS) + inst(*SUATOM) + inst(*EXIT))
        self.assertIn('cp_c1)._m0[', msl)
        self.assertRegex(msl, r'>> 16u|>> uint\(16\)|>> 16')
        self.assertRegex(msl, r'& 65535\b')
        self.assertRegex(msl, r'== 1;')
        self.assertNotIn('cp_c0)._m0[', msl)   # never the driver segment at a run-time index, which the host cannot fill


def mov(rd, value):
    return inst(0x000fc60000000f00, value << 32 | rd << 16 | 0x7802)


def sts(offset, rb):
    return inst(0x000fe80000000800, offset << 40 | rb << 32 | 0xff << 24 | 0x7388)


class Shared(Translated):
    STS_SCALED_4 = 0x000fe80000004800, 0x0000000302027388   # STS.U32 [R2 * 4], R3
    STS_SCALED_8 = 0x000fe80000008a00, 0x0000000302027388   # STS.U64 [R2 * 8], R3:R4
    STS_BYTES = 0x000fe80000000800, 0x0000000302027388
    IMAD_HI = 0x000fc80007820008, 0x0000000b07087227        # IMAD.HI R8, R7, R11, {R9:R8}
    ATOMS_CAS = 0x000e640001800001, 0x00000400ff01738d      # ATOMS.CAS R1, [RZ + 4], R0, R1

    def msl(self, *code, shared=0x400):
        msl = self.translate(b''.join(code) + inst(*EXIT), shared=shared)
        return msl[msl.index('kernel void'):]

    def test_a_32_bit_access_scales_its_register_by_4(self):
        self.assertIn('[3] = 77u', self.msl(mov(2, 3), mov(3, 77), inst(*self.STS_SCALED_4)))

    def test_a_64_bit_access_scales_its_register_by_8(self):
        msl = self.msl(mov(2, 3), mov(3, 77), mov(4, 88), inst(*self.STS_SCALED_8))
        self.assertIn('[6] = 77u', msl)
        self.assertIn('[7] = 88u', msl)

    def test_no_scale_keeps_the_byte_address(self):
        self.assertIn('[3] = 77u', self.msl(mov(2, 12), mov(3, 77), inst(*self.STS_BYTES)))

    def test_imad_hi_adds_the_pair_to_the_whole_product(self):
        # 0xffffffff * 1 + {4000:1}: the low word carries
        msl = self.msl(mov(7, 0xffffffff), mov(11, 1), mov(8, 1), mov(9, 4000), inst(*self.IMAD_HI), sts(0x20, 8))
        self.assertIn('._m1', msl)
        self.assertRegex(msl, r'\+ 4000\b')
        self.assertRegex(msl, r'\+ 1\b')

    ISETP_P0 = 0x000fe20003f06070, 0x000000200200780c
    IMAD_X = 0x000fe400000e0605, 0x000000ffff057224       # IMAD.X R5, RZ, RZ, R5 + P0
    IMAD_X_NEG = 0x000fe400000e0e07, 0x000000ffff067224   # IMAD.X R6, RZ, RZ, -R7 + P0: with .X a negated addend is its bitwise not

    def test_imad_x_adds_its_carry(self):
        self.assertIn('[8] = 101u', self.msl(mov(2, 0x40), mov(5, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X), sts(0x20, 5)))

    def test_imad_x_without_the_carry_adds_nothing(self):
        self.assertIn('[8] = 100u', self.msl(mov(2, 1), mov(5, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X), sts(0x20, 5)))

    def test_imad_x_negates_by_bitwise_not(self):   # ~100 + 1 is -100; a two's complement negation would give -99
        self.assertIn('[8] = 4294967196u', self.msl(mov(2, 0x40), mov(7, 100), inst(*self.ISETP_P0), inst(*self.IMAD_X_NEG), sts(0x20, 6)))

    def test_atoms_cas_takes_its_expected_value_from_rb_and_its_new_value_from_rc(self):
        msl = self.msl(mov(0, 5), mov(1, 9), inst(*self.ATOMS_CAS), sts(0x20, 1))
        self.assertIn('atomic_compare_exchange', msl)
        self.assertRegex(msl, r'= 5u;')
        self.assertRegex(msl, r', 9u,')

    # the stride field is a scale, not the access size
    STS_SCALED_16 = 0x0001e4000000c800, 0x0000000302027388

    def test_stride_3_scales_its_register_by_16(self):
        self.assertIn('[12] = 77u', self.msl(mov(2, 3), mov(3, 77), inst(*self.STS_SCALED_16)))

    STS64_SCALED_4 = 0x0009ee0000004a00, 0x000900182c007388     # STS.U64 [R44 * 4 + 0x900], R24:R25
    STS_SCALED_16_AT_8 = 0x000fe8000000c800, 0x0000080609008388   # STS.U32 [R9 * 16 + 8], R6
    STS64_SCALED_16 = 0x000fe8000000ca00, 0x0000000209008388
    STS128_SCALED_16 = 0x0001e2000000cc00, 0x000000ff26007388   # STS.128 [R38 * 16], RZ

    def test_a_64_bit_access_with_stride_1_scales_its_register_by_4(self):
        msl = self.msl(mov(44, 3), mov(24, 77), mov(25, 88), inst(*self.STS64_SCALED_4), shared=0x1000)
        self.assertIn('[579] = 77u', msl)
        self.assertIn('[580] = 88u', msl)

    def test_stride_3_adds_its_offset_after_the_scale(self):
        self.assertIn('[14] = 77u', self.msl(mov(9, 3), mov(6, 77), inst(*self.STS_SCALED_16_AT_8)))

    def test_a_64_bit_access_with_stride_3_scales_its_register_by_16(self):
        msl = self.msl(mov(9, 3), mov(2, 77), mov(3, 88), inst(*self.STS64_SCALED_16))
        self.assertIn('[12] = 77u', msl)
        self.assertIn('[13] = 88u', msl)

    def test_a_128_bit_access_with_stride_3_scales_its_register_by_16(self):
        msl = self.msl(mov(38, 3), inst(*self.STS128_SCALED_16))
        self.assertIn('[12] = 0u', msl)
        self.assertIn('[15] = 0u', msl)

    # LDSM keeps its mode at 78..79, where LDS has its stride: 1 is a transposed read (MT88), not a scale
    LDSM_MT88 = 0x000fe20000004200, 0x000000000314783b   # LDSM.16.MT88.4 R20, [R3]

    def test_ldsm_mode_is_not_a_scale(self):
        msl = self.msl(mov(3, 0x40), inst(*self.LDSM_MT88), sts(0x20, 20))
        self.assertIn('spvSubgroupShuffle(8.96831', msl)

    # LDSM reads rows other lanes stored, often with no BAR before it: SIMD-group barriers before and after its reads
    def test_ldsm_reads_between_subgroup_barriers(self):
        msl = self.msl(mov(3, 0x40), inst(*self.LDSM_MT88), sts(0x20, 20))
        barriers = [m.start() for m in re.finditer(r'simdgroup_barrier\(mem_flags::mem_threadgroup\)', msl)]
        reads = [m.start() for m in re.finditer(r'spvSubgroupShuffle\(', msl)]
        store = msl.index('[8] = ')
        self.assertEqual(len(barriers), 2, msl)
        self.assertTrue(reads and barriers[0] < reads[0] and reads[-1] < barriers[1] < store, msl)


class Pokopia(Translated):
    MERGE_FIRST = (0x000fe200000040ff, 0x00c00600ff0c7a3e)   # F2FP.MERGE_C R12, RZ, c[3][word 6], RZ
    MERGE_SECOND = (0x000fe4000000400c, 0x00c00500ff0c7a3e)   # F2FP.MERGE_C R12, RZ, c[3][word 5], R12
    F2I_LOW = (0x000fe2000010f100, 0x0000000c000c7305)   # F2I.S32.F16.TRUNC R12, R12.H0
    F2I_HIGH = (0x000fe2000010f100, 0x1000000c000c7305)
    # TLD.1D.LZ R12, R12, texture slot 0 (c0 word 4)
    TLD_1D = (0x000fe200009e01ff, 0x100004ff0c0c7b66)
    ATOMS_ADD = (0x004e680000000000, 0x0007fc0cff0c738c)   # ATOMS.ADD.U32 R12, [RZ + 0x7fc], R12
    MEMBAR = (0x000fec0000005000, 0x0000000000007992)
    ERRBAR = (0x000fc00000000000, 0x00000000000079ab)

    def msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in code) + inst(*SUATOM) + inst(*EXIT))

    def test_fixed_uniform_reads_are_bare_and_counted(self):   # MetalUniformBounds: no bounds check for reads at fixed offsets; the host gets how far they go
        sys.path.insert(0, str(TOOLS)); import dmsh_abi
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND)
        self.assertNotIn('_c%d' % dmsh_abi.CBSIZE_BANK, msl)
        self.assertEqual([c['fixed'] for c in self.res['cbufs'] if c['slot'] == 3], [28])

    def test_merge_shifts_c_into_the_high_half(self):
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND)
        high = re.search(r'uint (_\d+) = as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z, 0\.0\)\)\);', msl)
        low = re.search(r'uint (_\d+) = as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.y, 0\.0\)\)\);', msl)
        self.assertTrue(high and low, msl)
        shifted = re.search(rf'int (_\d+) = int\({high.group(1)}\) << 16;', msl)
        self.assertTrue(shifted, msl)
        self.assertRegex(msl, rf'{shifted.group(1)} \| int\({low.group(1)}\)')

    def test_merge_with_rz_in_c_is_the_low_half_alone(self):
        msl = self.msl(self.MERGE_FIRST)
        self.assertRegex(msl, r'as_type<uint>\(half2\(float2\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z, 0\.0\)\)\)')
        self.assertNotIn('<< 16', msl)

    def test_f2i_reads_the_selected_half(self):
        self.assertIn('trunc(0.0)', self.msl(self.MERGE_FIRST, self.F2I_HIGH))
        self.assertRegex(self.msl(self.MERGE_FIRST, self.F2I_LOW), r'trunc\(\(\*spvDescriptorSet0\.cp_c3\)\._m0\[1\]\.z\)')

    def test_1d_fetch_wraps_into_rows(self):   # a texture buffer is bound as rows of 16384 texels (tbuf_as_2d)
        msl = self.msl(self.MERGE_FIRST, self.TLD_1D)
        self.assertRegex(msl, r'& 16383')
        self.assertRegex(msl, r'>> uint\(14\)')
        self.assertRegex(msl, r'cp_t_tcb_8\.read\(uint2\(')
        self.assertNotIn('texture1d', msl)

    def test_atoms_add_on_shared_memory(self):   # 0x7fc is word 511 of the workgroup's 0x800 bytes
        msl = self.translate(inst(*self.ATOMS_ADD) + inst(*SUATOM) + inst(*EXIT), shared=0x800)
        self.assertRegex(msl, r'atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&_\d+\[511\]')

    # HADD2 / HMUL2 with an immediate (form 2): the second source is the immediate
    HADD2_IMM = (0x000fe20000000900, 0x3e003e00ff0c7430)   # HADD2 R12, RZ, (1.5, 1.5)
    HADD2_NEG_IMM = (0x000fe20000000900, 0xb800b800ff0c7430)   # HADD2 R12, RZ, (-0.5, -0.5): bits 63 and 62 are the constant's, not modifiers
    HMUL2_IMM = (0x000fe2000000080c, 0x400040000c0c7432)   # HMUL2 R12, R12, (2.0, 2.0)

    def test_half_add_takes_the_immediate(self):
        msl = self.msl(self.HADD2_IMM)
        self.assertIn('1.5', msl)
        self.assertNotRegex(msl, r'as_type<half2>\(\s*_?\w*r0')

    def test_half_immediate_keeps_its_sign(self):
        self.assertIn('-0.5', self.msl(self.HADD2_NEG_IMM))

    def test_half_multiply_takes_the_immediate(self):
        self.assertIn('2.0', self.msl(self.MERGE_FIRST, self.HMUL2_IMM))

    # HFMA2 .H0_NH1 (bit 86): low lane times B.hi negated, high lane times B.lo
    HFMA2_NH1 = (0x000fe200004000ff, 0x0000000c0c0c7231)   # HFMA2 R12, R12, R12.H0_NH1, RZ
    HFMA2_PLAIN = (0x000fe200000000ff, 0x0000000c0c0c7231)

    def hfma2_lanes(self, hfma2):
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND, hfma2)
        lanes = re.findall(r'float (_\d+) = float2\(as_type<half2>\(uint\(_\d+\)\)\)\.[xy];', msl)
        self.assertEqual(len(lanes), 4, msl)
        return (msl, *lanes)

    def test_hfma2_nh1_rotates_b(self):
        msl, a_lo, a_hi, b_lo, b_hi = self.hfma2_lanes(self.HFMA2_NH1)
        negated = re.search(rf'float (_\d+) = -{b_hi};', msl)
        self.assertTrue(negated, msl)
        self.assertIn(f'fma(half({a_lo}), half({negated.group(1)}), half(0.0))', msl)
        self.assertIn(f'fma(half({a_hi}), half({b_lo}), half(0.0))', msl)

    def test_hfma2_without_nh1_keeps_the_lanes(self):
        msl, a_lo, a_hi, b_lo, b_hi = self.hfma2_lanes(self.HFMA2_PLAIN)
        self.assertIn(f'fma(half({a_lo}), half({b_lo}), half(0.0))', msl)
        self.assertIn(f'fma(half({a_hi}), half({b_hi}), half(0.0))', msl)

    FSEL_NEG_B = (0x000fe40000000000, 0x8000000c0c0c7208)   # FSEL R12, P0, R12, -R12

    def test_fsel_negates_b(self):
        msl = self.msl(self.MERGE_FIRST, self.MERGE_SECOND, self.FSEL_NEG_B)
        self.assertRegex(msl, r'float _\d+ = -')

    def test_up2ur_takes_any_predicate_by_its_mask_bit(self):
        for mask in (0x40, 0x08): self.msl((0x000fe20008000000, mask << 32 | 0x3f067883))

    def test_up2ur_with_a_merge_source_is_refused(self):
        with self.assertRaises(AssertionError) as e: self.msl((0x000fe20008000000, 0x40 << 32 | 0x05067883))
        self.assertIn('UP2UR with a merge source', str(e.exception))

    def test_up2ur_reads_up0(self):
        self.msl((0x000fc60008000000, 0x000000013f067883))

    # stride 1: Ra * 4 + offset; the index comes from F2I so RZ cannot fold the scale away
    F2I_R3 = (0x000e24000021f000, 0x0000000c00037305)   # F2I.S32 R3, R12
    HIST_INC = (0x0011e4000d00403f, 0x0000000003ff7f8c)   # 0xf8c: hist[R3]++
    LDS_SCALED = (0x000fe20000004800, 0x00000000030c7984)   # LDS R12, [R3 * 4]
    LDS_PLAIN = (0x000fe20000000800, 0x00000000030c7984)

    def shared_msl(self, *code):
        return self.translate(b''.join(inst(*c) for c in (self.MERGE_FIRST, self.F2I_R3) + code) + inst(*SUATOM) + inst(*EXIT), shared=0x800)

    def test_histogram_increment_adds_one_at_the_scaled_index(self):
        msl = self.shared_msl(self.HIST_INC)
        self.assertRegex(msl, r'atomic_fetch_add_explicit\(\(threadgroup atomic_uint\*\)&_\d+\[[^\]]*\], 1u')
        self.assertRegex(msl, r'<< 2')

    def test_shared_load_scales_only_with_bit_78(self):
        self.assertRegex(self.shared_msl(self.LDS_SCALED), r'<< 2')
        self.assertNotRegex(self.shared_msl(self.LDS_PLAIN), r'<< 2')

    def test_membar_orders_memory(self):
        self.assertRegex(self.msl(self.MEMBAR, self.ERRBAR), r'threadgroup_barrier|atomic_thread_fence')


# SURED.D.2D.MAX.U64.STRONG.GPU.IGN [R0], R2 at c0 word 12: coordinate R0, R1; value R2 (low), R3 (high)
SURED_MAX_U64 = (0x0001e2000110e400, 0x60000c020000759f)
SURED_MIN_U64 = (SURED_MAX_U64[0] & ~(0x1f << 23) | 1 << 23, SURED_MAX_U64[1])   # operation 87..91: 1 = MIN
SURED_ADD_U64 = (SURED_MAX_U64[0] & ~(0x1f << 23), SURED_MAX_U64[1])            # 0 = ADD: Metal has no 64-bit texture add
SURED64 = (mov(0, 5), mov(1, 2), mov(2, 0xffffffff), mov(3, 1), inst(*SURED_MAX_U64))


class Sured64(Translated):
    """64-bit image max / min: one texture2d<ulong> atomic on the rg32ui image (wide_atomics, pf_atomic)"""
    def msl(self, *code):
        return self.translate(b''.join(code) + inst(*EXIT), atomics=True)

    def test_max_is_one_64_bit_texture_atomic(self):
        msl = self.msl(*SURED64)
        self.assertIn('texture2d<ulong, access::read_write> cp_i_tcb_4A_rg32ui', msl)
        self.assertEqual(len(re.findall(r'\.atomic_max\(uint2\(\w+\), ulong4\(as_type<ulong>\(uint2\(4294967295u, 1u\)\)\)\);', msl)), 1)
        self.assertNotIn('atomic_fetch', msl)

    def test_bounds_guard_stays(self):
        self.assertRegex(self.msl(*SURED64), r'if \(all\(uint2\(\w+\) < uint2\(spvDescriptorSet0\.cp_i_tcb_4A_rg32ui\.get_width\(\), spvDescriptorSet0\.cp_i_tcb_4A_rg32ui\.get_height\(\)\)\)\)')

    def test_min(self):
        msl = self.msl(mov(0, 5), mov(1, 2), mov(2, 6), mov(3, 7), inst(*SURED_MIN_U64))
        self.assertRegex(msl, r'\.atomic_min\(uint2\(\w+\), ulong4\(as_type<ulong>\(uint2\(6u, 7u\)\)\)\);')

    def test_add_is_refused(self):
        self.assertIn('Metal 64-bit image atomics', self.refused(b''.join((mov(0, 5), mov(1, 2), inst(*SURED_ADD_U64), inst(*EXIT)))))


class WideAtomics(unittest.TestCase):
    DECL = 'struct spvDescriptorSetBuffer0\n{\n    texture2d<uint, access::read_write> cp_i_tcb_4A_rg32ui [[id(0)]];\n};\n'
    GUARD = '    if (all(uint2(_29) < uint2(spvDescriptorSet0.cp_i_tcb_4A_rg32ui.get_width(), spvDescriptorSet0.cp_i_tcb_4A_rg32ui.get_height())))\n    {\n'
    PAIR = ('        uint _40 = spvDescriptorSet0.cp_i_tcb_4A_rg32ui.atomic_fetch_max(uint2(_29), uint(_12)).x;\n'
            '        uint _41 = spvDescriptorSet0.cp_i_tcb_4A_rg32ui.atomic_fetch_max(uint2(_29), as_type<uint>(f(_13, 2))).x;\n')

    def wide(self, body):
        sys.path.insert(0, str(TOOLS)); import dmsh_atomic
        return dmsh_atomic.wide_atomics(self.DECL + self.GUARD + body + '    }\n')

    def test_pair(self):
        out = self.wide(self.PAIR)
        self.assertIn('texture2d<ulong, access::read_write> cp_i_tcb_4A_rg32ui', out)
        self.assertIn('spvDescriptorSet0.cp_i_tcb_4A_rg32ui.atomic_max(uint2(_29), ulong4(as_type<ulong>(uint2(uint(_12), as_type<uint>(f(_13, 2))))));', out)
        self.assertNotIn('atomic_fetch', out)

    def test_a_lone_32_bit_atomic_fails(self):
        with self.assertRaisesRegex(ValueError, 'not the translator'):
            self.wide(self.PAIR.split('\n')[0] + '\n')

    def test_halves_on_different_texels_fail(self):
        with self.assertRaisesRegex(ValueError, 'different texels'):
            self.wide(self.PAIR.replace('(uint2(_29), as_type', '(uint2(_30), as_type'))

    def test_another_access_fails(self):
        with self.assertRaisesRegex(ValueError, 'accesses of a 64-bit atomic image'):
            self.wide(self.PAIR + '        uint4 _50 = spvDescriptorSet0.cp_i_tcb_4A_rg32ui.read(uint2(_29));\n')

    def test_32_bit_images_are_left_alone(self):
        sys.path.insert(0, str(TOOLS)); import dmsh_atomic
        src = self.DECL.replace('rg32ui', 'r32ui') + '        uint _40 = spvDescriptorSet0.cp_i_tcb_4A_r32ui.atomic_fetch_max(uint2(_29), 1u).x;\n'
        self.assertEqual(dmsh_atomic.wide_atomics(src), src)


if __name__ == '__main__':
    unittest.main()
