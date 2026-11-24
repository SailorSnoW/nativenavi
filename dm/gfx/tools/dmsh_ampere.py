#!/usr/bin/env python3
"""Switch 2 (NVN2) shaders: what the build reads of their control section (the translator decodes the Ampere code itself).

+0x38 is the instructions' length (SPH included), +0x40 that rounded up to 0x100, +0x44 the code area's: the c1 segment lies between the last two."""
import struct

CTL_MAGIC, CTL_VERSION = 0x98761234, 0xc9


def is_nvn2(ctl):
    return len(ctl) >= 0x48 and struct.unpack_from('<II', ctl) == (CTL_MAGIC, CTL_VERSION)


def code_len(ctl):
    return struct.unpack_from('<I', ctl, 0x44)[0]


def inst_len(ctl):
    return struct.unpack_from('<I', ctl, 0x38)[0]


def c1_segment(ctl, code):
    a, e = struct.unpack_from('<II', ctl, 0x40)
    return code[a:e] if a <= e <= len(code) else b''


EARLY_FRAGMENT_TESTS = 0x5b


def early_fragment_tests(ctl):
    """Ampere's SPH has no such bit (docs/reference/shaders.md#early-fragment-tests-from-the-control-section)"""
    return len(ctl) > EARLY_FRAGMENT_TESTS and ctl[EARLY_FRAGMENT_TESTS] != 0
