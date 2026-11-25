# Rewrites a Switch aarch64 NSO into an image that runs natively on Apple silicon (docs/reference/rewriter.md).
# Usage: patch_nso.py <NSO file> <output dir> <module name>
# Apple's kernel changes x18 and tpidr_el0 and faults on a misaligned sp; Horizon's SVCs, thread area and timers have no counterpart there.
# Outputs: <name>.patched.bin, .fixups.tsv, .gates.tsv, .sites.tsv, .cuts.tsv, .obs.tsv, .changed.tsv
import struct, re, sys, os, glob, shlex, types, importlib.util, multiprocessing, signal
from itertools import combinations
import capstone
if capstone.cs_version()[0] != 5:   # Capstone 6 renamed CS_ARCH_ARM64 / ARM64_OP_*: rewriter.md#capstone-api-version
    sys.exit(f'[RED] Capstone {capstone.__version__} is unsupported; the rewriter requires Capstone 5.x. Run: {shlex.join([sys.executable, "-m", "pip", "install", "capstone>=5,<6"])}')
from capstone import *
from capstone.arm64 import ARM64_OP_IMM, ARM64_OP_MEM, ARM64_OP_REG
import nso, nso_asm, nso_syms
from nso_asm import *

# build_app.py stops a rewrite with SIGTERM: exiting through the pool's context manager reaps the workers.
signal.signal(signal.SIGTERM, lambda *_: sys.exit(143))
try: WORKERS = int(os.environ.get('DM_REWRITE_J') or os.cpu_count() or 1)
except ValueError: sys.exit('[RED] DM_REWRITE_J must be a positive integer')
if WORKERS < 1: sys.exit('[RED] DM_REWRITE_J must be a positive integer')

src, outdir, NAME = sys.argv[1], sys.argv[2], sys.argv[3]
MOD = nso.cli_read(src)
bss = MOD.bss
# A B reaches ±128 MB: an appended patch area past 112 MB goes FRONT bytes before the base instead (FAR); the file then holds text + ro + data
# and the loader supplies bss and the key page, which stays after bss (reached by ADRP). rewriter.md#patch-area-before-the-module-base-for-a-large-bss
NEAR_PATCH_OFF = ((((len(MOD.image) + bss + 0x3FFF) & ~0x3FFF) + 0x4000 + 0xFFFF) & ~0xFFFF)
FAR, FRONT = NEAR_PATCH_OFF > 0x7000000, 0x2000000
# rewriter.md#patch-area-right-before-the-base-for-a-long-text; DM_PATCH_FRONT is set only by the rerun with the exact size needed
BIG_TEXT = FAR and MOD.text_end + FRONT > 0x8000000 - 0x10000
# rewriter.md#two-patch-areas-for-a-text-longer-than-a-branch-reaches and #split-point-within-the-back-areas-reach
TWO = BIG_TEXT and MOD.text_end > 0x8000000 - 0x10000
# GAP: rewriter.md#hops-for-sites-neither-patch-area-reaches, #distributing-branch-hops-between-patch-areas
BACK_MIN, GAP = 0, False
if TWO:
    BACK_MIN = ((((len(MOD.image) + bss + 0x3FFF) & ~0x3FFF) + 0x8000 + 0xFFFF) & ~0xFFFF) - 0x8000000 + 0x100000
    GAP = BACK_MIN >= 0x8000000 - 0x200000
    FRONT = int(os.environ.get('DM_PATCH_FRONT', '0'), 0) or (0x1000000 if GAP else min(0x1000000, (0x8000000 - 0x100000 - BACK_MIN) & ~0xFFFF))
elif BIG_TEXT: FRONT = int(os.environ.get('DM_PATCH_FRONT', '0'), 0) or (0x8000000 - 0x10000 - MOD.text_end) & ~0xFFFF
SPLIT = (0x8000000 - FRONT - 0x100000 if GAP else max(0x8000000 - FRONT - 0x100000, BACK_MIN)) if TWO else None
IMG = MOD.image
ORIG_TEXT = bytes(IMG[:MOD.text_end])
if not FAR: IMG.extend(bytes(bss))
TEXT_END = MOD.text_end
RO_OFF, DATA_OFF = MOD.segments[1][0], MOD.segments[2][0]
if FAR:
    FILE_IMAGE_LEN = (len(IMG) + 0x3FFF) & ~0x3FFF
    KEY_PAGE_OFF = (len(IMG) + bss + 0x3FFF) & ~0x3FFF
    IMAGE_SIZE, PATCH_OFF = KEY_PAGE_OFF + 0x4000, -FRONT
    BACK_OFF = (KEY_PAGE_OFF + 0x8000 + 0xFFFF) & ~0xFFFF if TWO else 0   # after the key page and the frame step state page
    assert not TWO or BACK_OFF - 0x8000000 + 0x100000 == BACK_MIN, 'BACK_MIN and BACK_OFF disagree'
else:
    KEY_PAGE_OFF = (len(IMG) + 0x3FFF) & ~0x3FFF
    IMG.extend(bytes(0x4000))
    PATCH_OFF = (len(IMG) + 0xFFFF) & ~0xFFFF
    IMAGE_SIZE = BACK_OFF = 0

# Registered patches, keyed by NSO build ID (sdk_patches.py, dm/games/<id>/patches.py; all are loaded, a build ID claimed twice stops the rewrite):
#   CUTS {build: [(offset, original, new, reason)]}, original bytes asserted; OBS {build: [(offset, OBS_* number of dm.h, original word, reason)]}, position-independent words only;
#   FRAME_STEP / STUBS {build: fn(ctx)}; CAPABILITIES {build: (name, ...)}; BINDING: read by docs_patches.py only.
#   MATCH_CUTS / MATCH_STUBS {module: [rule]}, MATCH_FRAME_STEP {'main': [rule]}: for builds without registered patches, the DM_GAME profile's only (sdk_patches.py's apply to every game).
SHARED = ('sdk_patches.py',)
PATCH_FILES = [os.path.join(os.path.dirname(os.path.abspath(__file__)), f) for f in SHARED] + sorted(glob.glob(os.path.join(glob.escape(os.path.dirname(os.path.abspath(__file__))), '../games/*/patches.py')))
# glob.escape: brackets in a folder name are a character class (rewriter.md#repository-patches-missing-under-bracketed-paths)
PATCH_FILES += [p for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d for p in sorted(glob.glob(os.path.join(glob.escape(d), '*', 'patches.py')))]
patch_paths = {}
for path in PATCH_FILES: patch_paths.setdefault(os.path.realpath(path), path)   # a symlink or a repeated root is the same file, not a second claim
PATCH_FILES = list(patch_paths.values())
def load_patches():
    tables, where = {'CUTS': {}, 'OBS': {}, 'FRAME_STEP': {}, 'STUBS': {}, 'MATCH_CUTS': {}, 'MATCH_FRAME_STEP': {}, 'MATCH_STUBS': {}, 'SDK_MATCH_CUTS': {}, 'CAPABILITIES': {}}, {}
    for n, path in enumerate(PATCH_FILES):
        spec = importlib.util.spec_from_file_location(f'patches_{n}', path)
        assert spec and spec.loader
        m = importlib.util.module_from_spec(spec); spec.loader.exec_module(m)
        for kind, table in tables.items():
            if kind == 'SDK_MATCH_CUTS': continue
            if kind == 'MATCH_CUTS' and os.path.basename(path) in SHARED:
                for mod, rules in getattr(m, kind, {}).items(): tables['SDK_MATCH_CUTS'].setdefault(mod, []).extend(rules)
                continue
            if kind in ('MATCH_CUTS', 'MATCH_FRAME_STEP', 'MATCH_STUBS'):
                if os.path.basename(path) in SHARED: continue
                profile = os.path.basename(os.path.dirname(path))
                roots = [os.path.join(os.path.dirname(os.path.abspath(__file__)), '../games')] + [d for d in os.environ.get('DM_GAMES_DIR', '').split(os.pathsep) if d]
                profile_dir = next((os.path.join(root, profile) for root in roots if os.path.isdir(os.path.join(root, profile))), None)
                if profile_dir and os.path.realpath(os.path.dirname(path)) != os.path.realpath(profile_dir): continue
                if getattr(m, kind, {}): table[profile] = getattr(m, kind)   # the repository's rules win for a derived profile; else the first external root's
                continue
            for bid, v in getattr(m, kind, {}).items():
                assert bid not in table, f'{kind} of build {bid} in both {where[kind, bid]} and {path}'
                table[bid], where[kind, bid] = v, path
    return tables, where
PATCHES, PATCH_WHERE = load_patches()
PROFILE = os.path.basename((os.environ.get('DM_GAME') or '0400486024DEC000').rstrip('/'))
BUILD_ID = MOD.build_id[:0x10].hex()
cuts = list(PATCHES['CUTS'].get(BUILD_ID, []))
if BUILD_ID not in PATCHES['CUTS']:
    from nso_match import resolve
    profile = PROFILE
    known, rows, own = [], [], set()
    try:
        if profile:
            with open(os.path.join(os.path.dirname(os.path.abspath(__file__)), '../games', profile, 'game.tsv'), encoding='utf-8') as f: rows = f.read().splitlines()
    except OSError: pass
    for row in rows:
        c = [x.strip() for x in row.split('\t')]
        if c[0] == 'build' and len(c) > 3:
            bid = dict(x.split('=', 1) for x in c[3:] if '=' in x).get(NAME)
            if bid: own.add(bid)
            if bid and bid != BUILD_ID and PATCHES['CUTS'].get(bid): known.append((bid, c[2], len(PATCHES['CUTS'][bid])))
    outcomes, sdk_outcomes = [], []
    cuts += resolve(IMG, TEXT_END, PATCHES['MATCH_CUTS'].get(profile, {}).get(NAME, []), outcomes=outcomes)
    cuts += resolve(IMG, TEXT_END, PATCHES['SDK_MATCH_CUTS'].get(NAME, []), outcomes=sdk_outcomes, red=False)
    if BUILD_ID not in own and (known or outcomes or sdk_outcomes):
        os.makedirs(outdir, exist_ok=True)
        with open(os.path.join(outdir, NAME + '.match.tsv'), 'w') as f:
            f.write(f'# {NAME} {BUILD_ID}: not a registered build. registered: build ID, display version, cuts; patch (the game profile\'s) or sdk (SDK-wide): name, cut offsets or -, implementation or why not\n')
            for bid, disp, n in known: f.write(f'registered\t{bid}\t{disp}\t{n}\n')
            for kind, got in (('patch', outcomes), ('sdk', sdk_outcomes)):
                for name, offs, why in got: f.write(f'{kind}\t{name}\t{",".join(f"{o:#x}" for o in offs) if offs else "-"}\t{why}\n')
FS_MATCH = None
if NAME == 'main' and BUILD_ID not in PATCHES['FRAME_STEP']:
    from nso_match import locate_stub
    FS_MATCH = locate_stub(IMG, TEXT_END, PATCHES['MATCH_FRAME_STEP'].get(PROFILE, {}).get(NAME, []))
STUB_RULES = PATCHES['MATCH_STUBS'].get(PROFILE, {}).get(NAME, [])
STUB_MATCH = None
if STUB_RULES and BUILD_ID not in PATCHES['STUBS']:
    from nso_match import locate_stub
    STUB_MATCH = locate_stub(IMG, TEXT_END, STUB_RULES)
# rewriter.md#a-frame-step-state-page-where-the-patch-area-starts
if (BUILD_ID in PATCHES['FRAME_STEP'] or FS_MATCH) and NAME == 'main' and not FAR and PATCH_OFF < KEY_PAGE_OFF + 0x4000 + 0x1000:
    PATCH_OFF += 0x10000
# Mod patches (DM_MOD_PATCHES, from mods_rewrite.py): applied after cuts and the frame step were found on the original code, before rewriting.
# One that overlaps a cut, frame step site or observation gate refuses its whole file (MOD_REFUSED, exit 4). rewriter.md#mod-code-patches-the-host-cannot-make-at-load-time
MOD_RECS = []
def mod_overlap(off, n): return sorted({label for o, data, label in MOD_RECS if o < off + n and off < o + len(data)})
def mod_refuse(found):
    for label, why in found: print(f'MOD_REFUSED\t{label}\t{why}')
    sys.stdout.flush(); sys.exit(4)
def mod_conflict(off, n, what):
    hit = mod_overlap(off, n)
    if hit: mod_refuse([(label, f'it patches {NAME}+{off:#x}, where the rewriter has {what}') for label in hit])
if os.environ.get('DM_MOD_PATCHES'):
    with open(os.environ['DM_MOD_PATCHES'], encoding='utf-8') as f:
        for line in f:
            c = line.rstrip('\n').split('\t')
            if len(c) == 3: MOD_RECS.append((int(c[0], 16), bytes.fromhex(c[1]), c[2]))
    mod_end, refused = len(MOD.image) + (0 if FAR else bss), {}
    for o, data, label in MOD_RECS:
        if o + len(data) > mod_end: refused.setdefault(label, f'it patches {NAME}+{o:#x}, outside the module\'s image')
    for off, ob, nb, why in cuts:
        for label in mod_overlap(off, len(ob) // 2): refused.setdefault(label, f'it patches {NAME}+{off:#x}, where the rewriter has a cut ({why})')
    if refused: mod_refuse(sorted(refused.items()))
    for o, data, label in MOD_RECS: IMG[o:o + len(data)] = data
    ORIG_TEXT = bytes(IMG[:TEXT_END])
    print(f'{NAME}: {len(MOD_RECS)} mod patch(es) of {len({label for _, _, label in MOD_RECS})} file(s) applied before the rewrite')
for off, ob, nb, why in cuts:
    ob, nb = bytes.fromhex(ob), bytes.fromhex(nb)
    assert len(ob) == len(nb) and IMG[off:off + len(ob)] == ob, f'cut original bytes mismatch {off:#x}: {bytes(IMG[off:off + len(ob)]).hex()}'
    IMG[off:off + len(nb)] = nb
# Fixed indices; the loader writes "actual key number × 8" into the key page.
KEYS = {'K18': 508, 'KTLS': 509, 'KTLR': 510, 'KGATE': 511}
KEY_INDEX = {k: i for i, k in enumerate(('K18', 'KTLS', 'KTLR', 'KGATE'))}

md = Cs(CS_ARCH_ARM64, CS_MODE_ARM); md.detail = True
patch = bytearray(); fix = []; gates = []; stats = {}
patch_front, patch_back, PATCH_BASE = patch, bytearray(), PATCH_OFF
def use_area(a):
    """The patch area that reaches offset a (TWO: the back one from SPLIT on)"""
    global patch, PATCH_BASE
    patch, PATCH_BASE = (patch_back, BACK_OFF) if TWO and a >= SPLIT else (patch_front, PATCH_OFF)
def stat(k): stats[k] = stats.get(k, 0) + 1
def here(): return PATCH_BASE + len(patch)
def emit(w, kind=None):
    if kind: fix.append((len(patch), kind))
    patch.extend(struct.pack('<I', w & 0xFFFFFFFF))

def emit_key_addr(reg, kind):
    fix.append((len(patch), kind)); emit(ADRP(reg, here(), KEY_PAGE_OFF)); emit(LDRX(reg, reg, KEY_INDEX[kind]))

def emit_slot_addr(base, key_reg, kind):
    emit_key_addr(key_reg, kind); emit(ADDR(base, base, key_reg))

def dis(w, addr):
    l = list(md.disasm(struct.pack('<I', w), addr)); assert len(l) == 1, hex(w)
    return l[0]
def regset(ins):
    r, w = ins.regs_access()
    return {ins.reg_name(x) for x in r}, {ins.reg_name(x) for x in w}
def num(name):
    m = re.fullmatch(r'[xw](\d+)', name or ''); return int(m.group(1)) if m else None
def written_w(p):
    """Registers p writes, as wN names. Capstone calls x30 lr and x29 fp, and lists a compare's first operand as written"""
    names = ({'lr': 'x30', 'fp': 'x29'}.get(n, n) for n in map(p.reg_name, p.regs_access()[1]))
    return {f'w{num(n)}' for n in names if num(n) is not None}
def writes_reg(p, reg):
    """A wN write clears the upper half of xN; a compare only sets the flags"""
    return p.mnemonic not in ('cmp', 'cmn', 'tst') and 'w' + reg[1:] in written_w(p)
def has18(ins): return re.search(r'\b[xw]18\b', ins.op_str) is not None
def regs_in(ins):
    s = {int(n) for n in re.findall(r'\b[xw](\d+)\b', ins.op_str)}
    try:
        rd, wr = regset(ins); s |= {num(n) for n in rd | wr} - {None}
    except CsError: pass
    return s
def pick(avoid, n=2):
    out = [r for r in range(16) if r not in avoid and r != 18][:n]; assert len(out) == n; return out

def subst(ins, S):
    """Only the 4 register fields change, checked against the disassembly text."""
    w = struct.unpack('<I', ins.bytes)[0]
    want = re.sub(r'\b([xw])18\b', lambda m: f'{m.group(1)}{S}', ins.op_str)
    pos = [p for p in (0, 5, 10, 16) if (w >> p) & 31 == 18]
    for k in range(len(pos), 0, -1):
        for c in combinations(pos, k):
            nw = w
            for p in c: nw = (nw & ~(31 << p)) | (S << p)
            try: dd = dis(nw, ins.address)
            except AssertionError: continue
            if dd.mnemonic == ins.mnemonic and dd.op_str == want: return nw
    raise RuntimeError(f'subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}')

def sp_delta(ins):
    m = re.search(r'\[sp, #(-?(?:0x)?[0-9a-f]+)\]!', ins.op_str)
    if m: return int(m.group(1), 0)
    m = re.search(r'\[sp\], #(-?(?:0x)?[0-9a-f]+)$', ins.op_str)
    if m: return int(m.group(1), 0)
    assert not re.match(r'(sp|wsp),', ins.op_str), f'writes sp: {ins.address:x} {ins.op_str}'
    return 0

def site_plain(ins):
    S, T = pick(regs_in(ins))
    dlt = sp_delta(ins)
    save = -16 if dlt == 0 else -112          # instructions that modify sp write below it: the scratch save goes deep
    assert dlt == 0 or dlt >= -96, f'sp delta {dlt} at {ins.address:x}'
    start = here()
    emit(STP(S, T, 31, save)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(subst(ins, S))
    emit(STRX(S, T))                 # capstone can miss a write (adds w18)
    emit(LDP(S, T, 31, save - dlt)); emit(BX(here(), ins.address + 4))
    return start

def select_x18(ins):
    word = struct.unpack('<I', ins.bytes)[0]
    rd, rn, rm, cond = word & 31, (word >> 5) & 31, (word >> 16) & 31, (word >> 12) & 15
    if word & 0x7FE00C00 != 0x1A800000 or rd in (18, 31) or (rn == 18) == (rm == 18) or cond >= 14: return None
    return rd, (rm if rn == 18 else rn), (cond ^ (rn == 18)), bool(word >> 31)


# The first operand is a full definition unless also read; a wN definition zeros the high half. Stores, compares and NOP never end a lifetime.
SELECT_WRITES = set('adr adrp add adds and ands asr bic csel eor ldr ldrb ldrh ldrsb ldrsh ldrsw ldur lsl lsr mov movn movz mul orn orr sbfx sub subs ubfx'.split())
SELECT_READS = {'cmp', 'cmn', 'tst', 'nop', 'str', 'strb', 'strh', 'stur', 'sturb', 'sturh'}


def select_scratch(at, by_address, function_end, rd):
    """A dead AAPCS64 temporary (not x0–x8, callee-saved or LR), or None: overwritten before any read on every path, or reaching a return in the same function."""
    budget = 64

    def dead_after(pc, pending):
        nonlocal budget
        if not pending or budget == 0 or pc >= function_end or (ins := by_address.get(pc)) is None: return set()
        budget -= 1
        m = ins.mnemonic
        if m == 'ret' and not ins.op_str: return pending
        if m == 'b': return dead_after(ins.operands[-1].imm, pending)
        if m.startswith('b.') or m in ('cbz', 'cbnz', 'tbz', 'tbnz'):
            pending = pending - regs_in(ins)
            return dead_after(pc + 4, pending) & dead_after(ins.operands[-1].imm, pending)
        if m not in SELECT_WRITES and m not in SELECT_READS: return set()
        dead = set()
        if m in SELECT_WRITES and not ins.writeback:
            dst, _, sources = ins.op_str.partition(',')
            dst_n = num(dst)
            if dst_n in pending:
                reads, _ = regset(ins)
                if not any(num(name) == dst_n for name in reads | set(re.findall(r'\b[xw]\d+\b', sources))):
                    dead.add(dst_n)
        return dead | dead_after(pc + 4, pending - regs_in(ins))

    return min(dead_after(at + 4, set(range(9, 18)) - {rd}), default=None)


def site_select(ins, args, temp):
    """Only the arm selecting x18 loads K18, into the destination (dead there) and one dead temporary."""
    rd, other, other_cond, wide = args
    start, skip = here(), len(patch)
    emit(NOP)  # the conditional branch, once the other arm's address is known
    emit(MRS_TPIDRRO(temp)); emit_slot_addr(temp, rd, 'K18')
    emit(LDRX(rd, temp) if wide else 0xB9400000 | (temp << 5) | rd)  # ldr wD zero-extends
    emit(BX(here(), ins.address + 4))
    struct.pack_into('<I', patch, skip, BEQ(start, here()) | other_cond)
    if not wide or rd != other: emit(MOVX(rd, other) if wide else MOVX(rd, other) & 0x7FFFFFFF)
    emit(BX(here(), ins.address + 4))
    return start


def site_sp(ins):
    """sp = f(x18): site_plain's scratch would be restored relative to the new sp, so the old sp goes into T first."""
    S, T = pick(regs_in(ins))
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    emit(ADDI(T, 31, 0))             # mov T, sp (31 is sp in add immediate)
    emit(subst(ins, S))
    emit(LDP(S, T, T, -16)); emit(BX(here(), ins.address + 4))
    return start

def casp_pair18(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    return w & 0xBFA07C00 == 0x08207C00 and 18 in ((w >> 16) & 31, w & 31)

def site_casp(ins):
    """subst cannot rename x18 alone in a pair: the pair moves to a free even pair P, P + 1 (P from K18, P + 1 from x19) and back; the access stays one instruction"""
    w = struct.unpack('<I', ins.bytes)[0]
    rs, rn, rt = (w >> 16) & 31, (w >> 5) & 31, w & 31
    assert rn != 18, f'casp with x18 as its base and in a pair {ins.address:x} {ins.op_str}'
    used = regs_in(ins) | {18, 19}
    P = next(r for r in range(0, 16, 2) if not {r, r + 1} & used)
    T = next(r for r in range(16) if r not in used | {P, P + 1})
    nw = w
    if rs == 18: nw = (nw & ~(31 << 16)) | (P << 16)
    if rt == 18: nw = (nw & ~31) | P
    want = re.sub(r'\b([xw])(18|19)\b', lambda m: f'{m.group(1)}{P + (m.group(2) == "19")}', ins.op_str)
    dd = dis(nw, ins.address)
    if (dd.mnemonic, dd.op_str) != (ins.mnemonic, want): raise RuntimeError(f'casp subst fail {ins.address:x} {ins.mnemonic} {ins.op_str}: {dd.mnemonic} {dd.op_str}')
    start = here()
    emit(STP(P, P + 1, 31, -16)); emit(STUR(T, 31, -24))
    emit(MRS_TPIDRRO(T)); emit_slot_addr(T, P, 'K18'); emit(LDRX(P, T)); emit(MOVX(P + 1, 19))
    emit(nw)
    if rs == 18:
        emit(STRX(P, T))
        emit(MOVX(19, P + 1))
    emit(LDUR(T, 31, -24)); emit(LDP(P, P + 1, 31, -16)); emit(BX(here(), ins.address + 4))
    return start

def site_sp_align(ins, n):
    """Round xN up to 16: hand-written call bridges reserve argument sizes as they add up (rewriter.md#stack-arguments-reserved-with-sub-sp-sp-xn)"""
    start = here()
    emit(ADDI(n, n, 15)); emit(0x927CEC00 | n << 5 | n)   # add xN, xN, #15; and xN, xN, #~15
    emit(struct.unpack('<I', ins.bytes)[0]); emit(BX(here(), ins.address + 4))
    return start

def site_cond(ins):
    w = struct.unpack('<I', ins.bytes)[0]
    S, T = pick(regs_in(ins))
    tgt = ins.operands[-1].imm
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    m = ins.mnemonic
    taken = here() + 4 * 3
    off = (taken - here()) >> 2
    nw = ((w & 0xFF000000) | ((off & 0x7FFFF) << 5) | S) if m in ('cbz', 'cbnz') else ((w & 0xFFF80000) | ((off & 0x3FFF) << 5) | S)
    pc = here(); emit(nw)
    dd = dis(nw, pc); assert dd.mnemonic == m and dd.operands[-1].imm == taken
    emit(LDP(S, T, 31, -16)); emit(BX(here(), ins.address + 4))
    assert here() == taken
    emit(LDP(S, T, 31, -16)); emit(BX(here(), tgt))
    return start

def site_pcrel(ins):
    tgt = ins.operands[1].imm
    S, T = pick(set())
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18')
    pc = here(); emit(ADRP(S, pc, tgt))
    if ins.mnemonic == 'adr': emit(ADDI(S, S, tgt & 0xFFF))
    emit(STRX(S, T))
    emit(LDP(S, T, 31, -16)); emit(BX(here(), ins.address + 4))
    return start

def site_br(ins, base, count):
    """br x18 jump table (target = base + 4i): equally spaced landing pads, x16 saved in the red zone."""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))   # x16 / x17 can be live within a function: saved to the red zone
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(LDUR(17, 31, -24))
    padbase = start + 4 * 24
    delta = padbase - base
    op = ADDI if delta >= 0 else SUBI; dl = abs(delta)
    ops = []; hi, lo = dl >> 12, dl & 0xFFF
    while hi:
        h = min(hi, 0xFFF)
        ops.append(op(16, 16, h, 1))
        hi -= h
    if lo: ops.append(op(16, 16, lo, 0))
    assert len(ops) <= 12
    for o in ops: emit(o)
    emit(BR(16))
    while here() < padbase: emit(NOP)
    tails = padbase + 4 * count
    for i in range(count): emit(BX(here(), tails + 8 * i))
    for i in range(count):
        emit(0xF85F0000 | (31 << 5) | 16)
        emit(BX(here(), base + 4 * i))
    return start

def site_br_tab(ins, targets):
    """Offset-table br x18: the table is constant, so its targets are read now; no match → brk (the original bounds check stops other indices)"""
    start = here()
    emit(STUR(16, 31, -16)); emit(STUR(17, 31, -24))
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    uniq = sorted(set(targets))
    pads = here() + 4 * (4 * len(uniq) + 1)
    for k, t in enumerate(uniq):
        emit(ADRP(17, here(), t)); emit(ADDI(17, 17, t & 0xFFF)); emit(CMPR(16, 17)); emit(BEQ(here(), pads + 12 * k))
    emit(BRK(0x18))
    for t in uniq:
        emit(LDUR(16, 31, -16)); emit(LDUR(17, 31, -24)); emit(BX(here(), t))
    return start

def site_blr(ins):
    """x16 / x17 are free at a call (AAPCS64); x30 is the original return address, so unwinding never stops in the patch area"""
    start = here()
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16))
    ret = ins.address + 4
    emit(ADRP(30, here(), ret)); emit(ADDI(30, 30, ret & 0xFFF)); emit(BR(16))
    return start

def site_br_call(ins):
    """br x18 as an indirect tail call: x16 / x17 are free at a call, x30 keeps the caller's return address"""
    start = here()
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, 'K18'); emit(LDRX(16, 16)); emit(BR(16))
    return start

def site_slot_copy(ins, src, dst):
    """mrs / msr with x18: a copy between thread slots through x15, never through x18 (the kernel may change it at any instruction); x15–x17 saved in the red zone"""
    start = here()
    emit(STP(16, 17, 31, -16)); emit(STUR(15, 31, -24))
    if isinstance(src, str):
        emit(MRS_TPIDRRO(16))
        emit_slot_addr(16, 17, src)
        emit(LDRX(15, 16))
    else: emit(src)
    emit(MRS_TPIDRRO(16)); emit_slot_addr(16, 17, dst); emit(STRX(15, 16))
    emit(LDUR(15, 31, -24)); emit(LDP(16, 17, 31, -16))
    emit(BX(here(), ins.address + 4))
    return start

def site_sysreg_read(ins, kind):
    N = ins.bytes[0] & 31
    if N == 18: return site_slot_copy(ins, kind, 'K18')
    tmp = 16 if N != 16 else 17
    start = here()
    emit(STUR(tmp, 31, -16)); emit(MRS_TPIDRRO(N)); emit_slot_addr(N, tmp, kind); emit(LDRX(N, N)); emit(LDUR(tmp, 31, -16))
    emit(BX(here(), ins.address + 4))
    return start

def site_tpidr_write(ins):
    N = struct.unpack('<I', ins.bytes)[0] & 31
    if N == 18: return site_slot_copy(ins, 'K18', 'KTLS')
    (T,) = pick({N}, 1)
    start = here()
    emit(0xF81F0000 | (31 << 5) | T)
    tmp = next(x for x in (16, 17) if x not in (N, T))
    emit(STUR(tmp, 31, -24)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, tmp, 'KTLS'); emit(STRX(N, T)); emit(LDUR(tmp, 31, -24))
    emit(0xF85F0000 | (31 << 5) | T)
    emit(BX(here(), ins.address + 4))
    return start

def emit_gate_call():
    """x29 is borrowed and restored at once: _dm_gate must save the guest's x29, which the crash backtrace walks."""
    emit(MRS_TPIDRRO(30)); emit_slot_addr(30, 29, 'KGATE'); emit(LDRX(30, 30)); emit(LDRX(29, 31))

def site_gate(ins, kind, arg):
    """The gate finds its gates.tsv row by return point (this block's ldp)."""
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(BX(here(), ins.address + 4))
    gates.append((ret, kind, arg, ins.address))
    return start

def site_obs(off, oid, word):
    """dm_gate saves x0–x28 and the flags: registers keep their values unless the callback writes one (OBS_STDALLOC_INIT's x2)"""
    ins = dis(word, off)
    assert ins.mnemonic not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') and not ins.mnemonic.startswith('b.') \
        and not (ins.mnemonic.startswith('ldr') and '[' not in ins.op_str) and not has18(ins), f'observation gate original instruction cannot be relocated {off:#x} {ins.mnemonic} {ins.op_str}'
    start = here()
    emit(STP_PRE(29, 30, 31, -16))
    emit_gate_call()
    emit(BLR(30))
    ret = here()
    emit(LDP_POST(29, 30, 31, 16))
    emit(word)
    emit(BX(here(), off + 4))
    gates.append((ret, 'obs', oid, off))
    return start

EXCL_LD = ('ldxr', 'ldaxr', 'ldxp', 'ldaxp', 'ldxrb', 'ldaxrb', 'ldxrh', 'ldaxrh')
EXCL_ST = ('stxr', 'stlxr', 'stxp', 'stlxp', 'stxrb', 'stlxrb', 'stxrh', 'stlxrh')
def find_windows(insl):
    out = []
    for i, ins in enumerate(insl):
        if ins.mnemonic not in EXCL_LD: continue
        j = i
        while j < len(insl) and insl[j].mnemonic not in EXCL_ST: j += 1
        if j >= len(insl) or j - i >= 16: continue
        # another exclusive load before the store has no store of its own (a retry block): spanning both would merge two sequences
        if any(x.mnemonic in EXCL_LD for x in insl[i + 1:j]): continue
        if j + 1 < len(insl) and insl[j + 1].mnemonic in ('cbz', 'cbnz'): j += 1
        seg = insl[i:j + 1]
        if any(has18(x) for x in seg): out.append(seg)
    return out

def window_adrp(x, S, pc):
    """An adrp inside a window, recomputed for the copy's pc (x18's stand-in S)."""
    rd = num(x.reg_name(x.operands[0].reg))
    return ADRP(S if rd == 18 else rd, pc, x.operands[1].imm)

def check_window_branches(seg):
    """Windows with a B: reject exits that carry a reservation through a stub's store."""
    if not any(x.mnemonic == 'b' for x in seg): return
    assert all(b.address == a.address + 4 for a, b in zip(seg, seg[1:])), f'noncontiguous window {seg[0].address:x}'
    by_addr = {x.address: x for x in seg}
    pending, seen = [(seg[0].address, False)], set()
    while pending:
        addr, live = pending.pop()
        if addr not in by_addr:
            assert not live, f'branch leaves window with live reservation {addr:x}'
            continue
        if (addr, live) in seen: continue
        seen.add((addr, live))
        x = by_addr[addr]; m = x.mnemonic
        if m in EXCL_LD: live = True
        elif m in EXCL_ST or m == 'clrex': live = False
        if m == 'b':
            tgt = x.operands[-1].imm
            pending.append((tgt, live))
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            pending.extend(((x.operands[-1].imm, live), (addr + 4, live)))
        else: pending.append((addr + 4, live))

def site_window(seg):
    check_window_branches(seg)
    used = set()
    for x in seg: used |= regs_in(x)
    S, T = pick(used)
    lo, hi = seg[0].address, seg[-1].address + 4
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    copy0 = here(); n = len(seg)
    # The fall-through stub must come first: a successful stxr falls into it (rewriter.md#exit-stub-order-in-exclusive-windows).
    exits = copy0 + 4 * n + 16
    ex_targets = []; words = []
    for k, x in enumerate(seg):
        w = struct.unpack('<I', x.bytes)[0]
        if has18(x): w = subst(x, S)
        m = x.mnemonic
        if m in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
            tgt = x.operands[-1].imm
            if lo <= tgt < hi: newt = copy0 + (tgt - lo)
            else:
                ex_targets.append(tgt)
                newt = exits + 16 * (len(ex_targets) - 1)
            pc = copy0 + 4 * k; off = (newt - pc) >> 2
            if m == 'b': w = B(pc, newt)
            elif m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
            else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
            dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
        elif m == 'adrp': w = window_adrp(x, S, copy0 + 4 * k)
        else:
            assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
        words.append(w)
    for w in words: emit(w)
    for t in [hi] + ex_targets:
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(BX(here(), t)); emit(NOP)
    return start, lo, hi

def retry_block(insl, k, wseg):
    """Accept a branch into window wseg only as an out-of-line retry block re-entering it at its store-exclusive → (block, window from the store on), or None"""
    t = insl[k].operands[-1].imm
    tail = [y for y in wseg if y.address >= t]
    if not tail or tail[0].mnemonic not in EXCL_ST: return None
    for p in range(k - 1, max(-1, k - 8), -1):
        q = insl[p]
        if q.mnemonic in EXCL_LD: return insl[p:k + 1], tail
        if q.mnemonic in ('b', 'bl', 'br', 'blr', 'ret', 'cbz', 'cbnz', 'tbz', 'tbnz') or q.mnemonic.startswith('b.') or q.mnemonic in EXCL_ST: return None
    return None

def site_retry_window(parts):
    """A retry block's own copy: it cannot jump into the window's copy, as x18's stand-in would have to be loaded between its exclusive load and store"""
    used = set()
    for seg, _ in parts:
        for x in seg: used |= regs_in(x)
    S, T = pick(used)
    start = here()
    emit(STP(S, T, 31, -16)); emit(MRS_TPIDRRO(T)); emit_slot_addr(T, S, 'K18'); emit(LDRX(S, T))
    pos, at = {}, here()
    for seg, _ in parts:
        for x in seg:
            pos[x.address] = at
            at += 4
        at += 16
    exits, ex_targets = at, []
    for seg, ft in parts:
        for x in seg:
            w = struct.unpack('<I', x.bytes)[0]
            if has18(x): w = subst(x, S)
            m, pc = x.mnemonic, here()
            if m in ('cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'):
                tgt = x.operands[-1].imm
                if tgt not in pos and tgt not in ex_targets: ex_targets.append(tgt)
                newt = pos[tgt] if tgt in pos else exits + 16 * ex_targets.index(tgt)
                off = (newt - pc) >> 2
                if m in ('tbz', 'tbnz'): w = (w & 0xFFF8001F) | ((off & 0x3FFF) << 5)
                else: w = (w & 0xFF00001F) | ((off & 0x7FFFF) << 5)
                dd = dis(w, pc); assert dd.operands[-1].imm == newt and dd.mnemonic == m
            elif m == 'adrp': w = window_adrp(x, S, pc)
            else:
                assert m not in ('adr', 'adrp', 'b', 'bl', 'br', 'blr', 'ret') and not (m == 'ldr' and '[' not in x.op_str), hex(x.address)
            emit(w)
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(BX(here(), ft)); emit(NOP)
    for t in ex_targets:
        emit(STRX(S, T)); emit(LDP(S, T, 31, -16)); emit(BX(here(), t)); emit(NOP)
    return start

# A module without unwind tables has no FDE: func_of takes its text as one function.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import nso_code
starts = sorted({a for a, _ in nso_code.fde_table(IMG) if a < TEXT_END})

# words in data blocks of the text that read as sites are left alone
DATA_BLOCKS = nso_code.data_blocks(IMG, TEXT_END, md)
CODE_RANGES = list(zip([0] + [e for _, e in DATA_BLOCKS], [s for s, _ in DATA_BLOCKS] + [TEXT_END]))

# Hops: a b written into alignment padding no path runs, for branches neither end reaches (GAP only; rewriter.md#hops-for-sites-neither-patch-area-reaches).
REACH = (1 << 27) - 4
_holes, _hops = None, {}
def hop_holes():
    out, data = [], [(s, e) for s, e in DATA_BLOCKS]
    for s in starts:
        if s < 8 or s & 15: continue
        align, k = s & -s, s
        while k - 4 >= 0 and struct.unpack_from('<I', ORIG_TEXT, k - 4)[0] in (0, 0xD503201F): k -= 4
        if k == s or s - k >= align or k < 4: continue
        p = struct.unpack_from('<I', ORIG_TEXT, k - 4)[0]
        if not (p & 0xFC000000 == 0x14000000 or p & 0xFFFFFC1F in (0xD61F0000, 0xD65F0000) or p & 0xFFE0001F == 0xD4200000): continue
        if any(a < s and k < b for a, b in data): continue
        out.extend(range(k, s, 4))
    return out
def reach_via(frm, to):
    """to when a b at frm reaches it, else a hop that does"""
    global _holes
    if -REACH - 4 <= to - frm <= REACH: return to
    for h in _hops.get(to, ()):
        if abs(h - frm) <= REACH: return h
    if _holes is None: _holes = hop_holes()
    lo, hi = max(frm, to) - REACH, min(frm, to) + REACH
    i = bisect.bisect_left(_holes, lo)
    assert i < len(_holes) and _holes[i] <= hi, f'no padding word for a hop from {frm:#x} to {to:#x} (between {lo:#x} and {hi:#x})'
    h = _holes.pop(i)
    assert IMG[h:h + 4] == ORIG_TEXT[h:h + 4], f'hop {h:#x} was written by something else'
    struct.pack_into('<I', IMG, h, B(h, to)); _hops.setdefault(to, []).append(h); stat('hop')
    return h
def BX(frm, to): return B(frm, reach_via(frm, to))
def exclusive_order_sites(img, ranges):
    """Unordered exclusive loads / stores; bit 15 makes each the acquire / release form. Apple cores reorder around them where the Switch's did not (threads.md#exclusive-accesses-without-ordering)"""
    out = []
    for s, e in ranges:
        for m in re.finditer(rb'[\x08\x48\x88\xC8]', img[s:e]):   # top byte of every form: size << 6 | 0x08
            o = s + m.start() - 3
            if o & 3 or o < s: continue
            w = struct.unpack_from('<I', img, o)[0]
            if (w & 0x3FFFFC00 == 0x085F7C00 or w & 0xBFFF8000 == 0x887F0000           # ldxr* (Rs, Rt2 all ones), ldxp (Rs all ones)
                    or w & 0x3FE0FC00 == 0x08007C00 or w & 0xBFE08000 == 0x88200000):   # stxr* (Rt2 all ones), stxp
                out.append(o)
    return out
ORDERED = set(exclusive_order_sites(IMG, CODE_RANGES))   # a mod patch over one of these runs its own instruction, which no site depends on
for o in ORDERED:
    struct.pack_into('<I', IMG, o, struct.unpack_from('<I', IMG, o)[0] | 0x8000)
if ORDERED: stats['exclusive_ordered'] = len(ORDERED)
def odd_sp_frames(img, ranges, md):
    """(sub, add, size) of frames whose size is not 16-aligned, taken only when nothing between reaches past the frame (rewriter.md#stack-frames-of-an-odd-size-sub-sp-sp-imm)"""
    from capstone import CS_GRP_CALL, CS_GRP_INT, CS_GRP_JUMP, CS_GRP_RET
    from capstone.arm64 import ARM64_OP_MEM, ARM64_OP_REG, ARM64_REG_SP
    WIN = 64
    NARROW = {'ldrb': 1, 'strb': 1, 'ldrsb': 1, 'ldurb': 1, 'sturb': 1, 'ldursb': 1, 'ldrh': 2, 'strh': 2, 'ldrsh': 2, 'ldurh': 2, 'sturh': 2, 'ldursh': 2, 'ldrsw': 4, 'ldursw': 4}
    REG_BYTES = {'x': 8, 'w': 4, 'q': 16, 'd': 8, 's': 4, 'h': 2, 'b': 1}
    def inside(ins, size):
        if ins.group(CS_GRP_RET) or ins.group(CS_GRP_JUMP) and not ins.group(CS_GRP_CALL): return False
        if ins.group(CS_GRP_CALL) or ins.group(CS_GRP_INT): return True
        r, w = ins.regs_access()
        if ARM64_REG_SP not in r and ARM64_REG_SP not in w: return True
        mems = [op.mem for op in ins.operands if op.type == ARM64_OP_MEM]
        regs = [ins.reg_name(op.reg) for op in ins.operands if op.type == ARM64_OP_REG]
        if ARM64_REG_SP in w or ins.writeback or len(mems) != 1 or mems[0].base != ARM64_REG_SP or mems[0].index or not regs or len(regs) > 2: return False
        width = NARROW.get(ins.mnemonic) or REG_BYTES.get(regs[0][0])
        return width is not None and 0 <= mems[0].disp and mems[0].disp + width * len(regs) <= size
    out = []
    for s, e in ranges:
        for m in re.finditer(rb'\xD1', img[s:e]):   # top byte of sub (immediate, 64-bit)
            o = s + m.start() - 3
            if o & 3 or o < s or o + 4 > e: continue
            w = struct.unpack_from('<I', img, o)[0]
            if w & 0xFFC003FF != 0xD10003FF or not w >> 10 & 15: continue   # sub sp, sp, #imm (64-bit, no shift)
            size = w >> 10 & 0xFFF
            add = 0x910003FF | size << 10
            for q in range(o + 4, min(e, o + 4 + 4 * WIN), 4):
                if struct.unpack_from('<I', img, q)[0] == add:
                    out.append((o, q, size)); break
                ins = next(md.disasm(bytes(img[q:q + 4]), q), None)
                if ins is None or not inside(ins, size): break
    return out
def sp_adjust(op, n):
    """A frame of 0xff1–0xfff rounds up to 0x1000, which only the shifted form (#1, lsl #12) holds"""
    if n <= 0xFFF: return op | n << 10
    assert not n & 0xFFF and n >> 12 <= 0xFFF, hex(n)
    return op | 1 << 22 | n >> 12 << 10
for s, a, size in odd_sp_frames(IMG, CODE_RANGES, md):
    r = (size + 15) & ~15
    struct.pack_into('<I', IMG, s, sp_adjust(0xD10003FF, r)); struct.pack_into('<I', IMG, a, sp_adjust(0x910003FF, r))
    stat('sp_frame')
md_lite = Cs(CS_ARCH_ARM64, CS_MODE_ARM)
R18 = re.compile(r'\b[xw]18\b')
def x18_words(lo, hi):
    """Decoded 16 KB at a time without details, going on after a word capstone cannot decode"""
    out, a = set(), lo
    while a < hi:
        end = min(hi, a + 0x4000); last = a - 4
        for addr, size, mn, op in md_lite.disasm_lite(bytes(IMG[a:end]), a):
            last = addr
            if R18.search(op): out.add(addr)
        a = last + 4 if last + 4 >= end else last + 8
    return out
# The x18 scan and the spin wait search read only the original image, so they run in workers.
def in_parallel(fn, jobs):
    """[fn(*job) for job in jobs] in workers forked now. SIGTERM stays blocked across the fork until a worker has its default action back, and acts here only
    while waiting (rewriter.md#a-pools-shutdown-hangs-on-a-worker-that-has-just-started)"""
    if WORKERS < 2 or len(jobs) < 2:
        return [fn(*job) for job in jobs]
    def worker_signals():
        signal.signal(signal.SIGTERM, signal.SIG_DFL)
        signal.pthread_sigmask(signal.SIG_UNBLOCK, {signal.SIGTERM})
    mask = signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM})   # inherited by the pool's threads, so a replacement worker starts the same way
    try:
        with multiprocessing.get_context('fork').Pool(min(WORKERS, len(jobs)), initializer=worker_signals) as pool:
            signal.pthread_sigmask(signal.SIG_SETMASK, mask)   # a SIGTERM pending meanwhile acts here, inside the with, which reaps the workers
            try: results = pool.starmap(fn, jobs)
            finally: signal.pthread_sigmask(signal.SIG_BLOCK, {signal.SIGTERM})
            pool.close(); pool.join()   # workers leave by the pool's sentinels: a finished rewrite sends no SIGTERM
            return results
    finally:
        pool = None   # its destructors run here, still blocked
        signal.pthread_sigmask(signal.SIG_SETMASK, mask)

def find_x18_words():
    pieces = []
    for start, end in CODE_RANGES:
        for lo in range(start, end, 0x100000):
            pieces.append((lo, min(lo + 0x100000, end)))
    words = set()
    for found in in_parallel(x18_words, pieces):
        words |= found
    return words

x18 = find_x18_words()
kinds = {}
# Spin wait candidates: backward direct branches at most SPIN_WIN bytes back, by target
SPIN_WIN, SPIN_TOPS = 512, {0x16, 0x17, 0x54, 0x34, 0x35, 0xB4, 0xB5, 0x36, 0x37, 0xB6, 0xB7}
spin_cand = {}
for o in (o for s, e in CODE_RANGES for o in range(s, e, 4)):
    w = struct.unpack_from('<I', IMG, o)[0]
    if w >> 24 in SPIN_TOPS:
        t = w >> 24
        if t in (0x16, 0x17): d = ((w & 0x3FFFFFF) - (1 << 26)) * 4
        elif t & 0x7E == 0x36: d = (((w >> 5) & 0x3FFF) - (1 << 14)) * 4 if w & 0x40000 else 4
        elif t == 0x54: d = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 and not w & 0x10 else 4
        else: d = (((w >> 5) & 0x7FFFF) - (1 << 19)) * 4 if w & 0x800000 else 4
        if -SPIN_WIN <= d <= 0: spin_cand.setdefault(o + d, []).append(o)
    if w & 0xFFE0001F == 0xD4000001: kinds[o] = ('svc', (w >> 5) & 0xFFFF); continue
    if w & 0xFFFFFFE0 == 0xD53BD040: kinds[o] = ('tpidr_r', None); continue
    if w & 0xFFFFFFE0 == 0xD51BD040: kinds[o] = ('tpidr_w', None); continue
    if w & 0xFFFFFFE0 == 0xD53BD060: kinds[o] = ('tpidrro_r', None); continue
    if w & 0xFFFFFFE0 == 0xD53BE020: kinds[o] = ('cntpct', w & 31); continue
    if w & 0xFFFFFFE0 == 0xD53BE040: kinds[o] = ('cntvct', w & 31); continue
    # cntfrq_el0 read raw is the Mac's counter rate, not the cntpct gate's 31.25 MHz (rewriter.md#the-counter-frequency-read-raw)
    if w & 0xFFFFFFE0 == 0xD53BE000: kinds[o] = ('cntfrq', w & 31); continue
    # ctr_el0 traps in user mode on macOS (SIGILL); SDK 16's FlushDataCache reads it
    if w & 0xFFFFFFE0 == 0xD53B0020: kinds[o] = ('ctr', w & 31); continue
    # sub sp, sp, xN: rewriter.md#stack-arguments-reserved-with-sub-sp-sp-xn; with x18 it is site_sp
    if w & 0xFFE0FFFF == 0xCB2063FF and (w >> 16) & 31 != 18: kinds[o] = ('sp_align', (w >> 16) & 31); continue
    if o in x18: kinds[o] = ('x18', None)

import bisect
def func_of(a):
    """From the FDE at or before a to the next. Code before the first FDE, or a module without FDEs, is one function from the end of the data block before it"""
    i = bisect.bisect_right(starts, a) - 1
    if i >= 0: return (starts[i], starts[i + 1] if i + 1 < len(starts) else TEXT_END)
    return (max((e for s, e in DATA_BLOCKS if e <= a), default=0), starts[0] if starts else TEXT_END)
done = set(); windows = []; jumps = {}; tabjumps = {}; tailcalls = set(); retries = []
def tail_call_at_end(insl, j):
    """x18's last write is a plain pointer load and the br ends the function"""
    if j + 1 != len(insl): return False
    for p in reversed(insl[:j]):
        if 'x18' not in [p.reg_name(r) for r in p.regs_access()[1]]: continue
        return p.mnemonic == 'ldr' and bool(re.fullmatch(r'x18, \[(x\d+|sp)(, #(0x[0-9a-f]+|\d+))?\]', p.op_str))
    return False
def tail_call_padded(insl, j):
    if j + 1 >= len(insl) or any(not (p.mnemonic == 'nop' or p.mnemonic == 'udf' and p.op_str == '#0') for p in insl[j + 1:]): return False
    if any(b.address != a.address + 4 for a, b in zip(insl, insl[1:])): return False
    for p in insl[:j]:
        if p.mnemonic not in ('mov', 'ldr', 'ldrh', 'ldp', 'add', 'str', 'stp'): return False
        if {p.reg_name(r) for r in p.regs_access()[1]} & {'sp', 'wsp', 'fp', 'lr', 'x29', 'w29', 'x30', 'w30'}: return False
    for p in reversed(insl[:j]):
        if not {'x18', 'w18'} & {p.reg_name(r) for r in p.regs_access()[1]}: continue
        return p.mnemonic in ('ldr', 'ldp') and bool(re.fullmatch(r'x18, (x\d+, )?\[(x\d+|sp)(, #(0x[0-9a-f]+|\d+))?\]', p.op_str))
    return False
def tail_call(insl, j):
    """A br x18 with code after it is a tail call only when the frame is provably gone: a wrong accept is silent, so anything unproven is rejected
    (rewriter.md#indirect-tail-calls-through-x18-in-il2cpp-thunks)"""
    if j + 1 == len(insl): return tail_call_at_end(insl, j)
    if tail_call_padded(insl, j): return True
    if j < 4 or any(b.address != a.address + 4 for a, b in zip(insl, insl[1:])): return False
    writes = lambda p: {p.reg_name(r) for r in p.regs_access()[1]}
    cond = lambda p: p.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') or p.mnemonic.startswith('b.')
    load = next((k for k in range(j - 1, -1, -1) if writes(insl[k]) & {'x18', 'w18'}), None)
    if load is None or insl[load].mnemonic not in ('ldr', 'ldp') or not re.fullmatch(r'x18, (x\d+, )?\[(x\d+|sp)(, #(0x[0-9a-f]+|\d+))?\]', insl[load].op_str): return False
    # disp: the saved pair's offset from sp
    thunk = insl[0].mnemonic == 'stp' and insl[0].op_str == 'x29, x30, [sp, #-0x10]!'
    if thunk:
        if insl[1].mnemonic != 'mov' or insl[1].op_str != 'x29, sp': return False
        if insl[j - 1].mnemonic != 'ldp' or insl[j - 1].op_str != 'x29, x30, [sp], #0x10': return False
        restore, start, disp, saved, fp_set = j - 1, 2, 0, True, True
    else:
        sub, add = (re.fullmatch(r'sp, sp, #(0x[0-9a-f]+|\d+)', p.op_str) if p.mnemonic == m else None for p, m in ((insl[0], 'sub'), (insl[j - 1], 'add')))
        if not sub or not add or int(sub[1], 0) != int(add[1], 0): return False
        restore = next((k for k in range(j - 2, 0, -1) if insl[k].mnemonic == 'ldp' and re.fullmatch(r'x29, x30, \[sp(, #(0x[0-9a-f]+|\d+))?\]', insl[k].op_str)), None)
        if restore is None: return False
        disp = insl[restore].operands[-1].mem.disp
        if disp < 0 or disp + 16 > int(sub[1], 0): return False
        start, saved, fp_set = 1, False, False
    tail = min(load, restore)
    after_br = insl[j].address
    setup_end = insl[start].address
    targets = []
    guarded = False
    for k in range(start, j - 1):   # j - 1 is the frame's last restore or add sp
        p = insl[k]
        if k == restore: continue
        if cond(p) or p.mnemonic == 'b':
            t = p.operands[-1].imm
            if not saved or k >= restore or p.mnemonic == 'b' and k >= tail: return False
            if t > after_br: guarded = True
            elif thunk: return False
            targets.append(t)
            continue
        if p.mnemonic in ('bl', 'blr'):
            if thunk or not saved or k >= tail: return False
            continue
        if p.mnemonic in ('br', 'ret', 'udf', 'brk', 'hlt', 'svc', 'hvc', 'smc', 'msr', 'sys', 'dc', 'ic'): return False
        w = writes(p)
        if w & {'sp', 'wsp', 'lr', 'x30', 'w30'}: return False
        if w & {'fp', 'x29', 'w29'}:
            sets_fp = p.op_str == 'x29, sp' and not disp if p.mnemonic == 'mov' else p.mnemonic == 'add' and p.op_str.startswith('x29, sp, #') and p.operands[2].imm == disp
            if k > restore or fp_set or not saved or not sets_fp: return False
            fp_set = True
            setup_end = insl[k + 1].address
            continue
        if not saved and p.mnemonic == 'stp' and p.op_str == insl[restore].op_str:
            saved = True
            setup_end = insl[k + 1].address
            continue
        if '[' not in p.op_str or p.mnemonic.startswith(('ldr', 'ldur', 'ldp', 'ldar', 'prfm')): continue
        # regs_access does not report memory writes: stack stores are checked against the saved pair. Only the thunk can rule out stores through another base.
        if k > restore or p.mnemonic not in ('str', 'strb', 'strh', 'stur', 'sturb', 'sturh', 'stp') or p.writeback: return False
        mem = p.operands[-1].mem
        base = p.reg_name(mem.base)
        if base not in ('sp', 'fp', 'x29'):
            if thunk: return False
            continue
        if mem.index or base != 'sp' and not fp_set: return False
        at = mem.disp + (disp if base != 'sp' else 0)
        size = {'b': 1, 'h': 2}.get(p.mnemonic[-1]) or {'x': 8, 'w': 4, 'q': 16, 'd': 8, 's': 4, 'h': 2, 'b': 1}.get(p.reg_name(p.operands[0].reg)[0], 8)
        if p.mnemonic == 'stp': size *= 2
        if at < 0 or at < disp + 16 and at + size > disp: return False
    if not saved: return False
    suffix = insl[j + 1:]
    leaves = False
    for p in suffix:
        if p.mnemonic == 'bl':
            if insl[0].address <= p.operands[-1].imm <= insl[-1].address: return False
            leaves = True
        elif (cond(p) or p.mnemonic == 'b') and p.operands[-1].imm <= after_br: return False
    if thunk and (not guarded or not leaves or suffix[-1].mnemonic not in ('bl', 'udf', 'b') or any(p.mnemonic in ('ret', 'br', 'blr') for p in suffix)): return False
    return not any(t < setup_end or insl[restore].address < t <= after_br for t in targets)

def jump_bytes_far(insl, j, half):
    """A byte / halfword table whose add is up to 32 instructions before the br, x18 unchanged between → (base, width) or None"""
    ctx = insl[max(0, j - 32):j]
    for k in range(len(ctx) - 1, -1, -1):
        p = ctx[k]
        if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str: break
        if writes_reg(p, 'x18'): return None
    else: return None
    rn = p.reg_name(p.operands[2].reg); idx = {'lr': 30, 'fp': 29}.get(rn, num(rn))
    base = next((q.operands[1].imm for q in reversed(ctx[:k]) if q.mnemonic == 'adr' and q.op_str.startswith('x18,')), None)
    ld = next((q for q in reversed(ctx[:k]) if q.mnemonic in ('ldrb', 'ldrsb', 'ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx), None)
    if base is None or ld is None: return None
    return base, 256 if ld.mnemonic in ('ldrb', 'ldrsb') else half(base) or 65536
def disasm_fn(fa, fb):
    """Capstone stops at an undecodable word: go on after it (a trap word can hide a jump table after it)"""
    out, a = [], fa
    while a < fb:
        l = list(md.disasm(bytes(IMG[a:fb]), a))
        out += l; a = (l[-1].address + 4 if l else a) + 4
    return out
# Text without unwind entries can be tens of MB: functions over BIG_FN are analysed in pieces (rewriter.md#long-functions-analysed-in-pieces).
BIG_FN, PIECE, PIECE_B = 0x40000, 0x40000, 0x10000
def pieces(fa, fb):
    from array import array
    n = (fb - fa) // 4
    w = memoryview(bytes(IMG[fa:fa + 4 * n])).cast('I')
    cross = array('i', bytes(4 * (n + 2))); far = []
    for i in range(n):
        x = w[i]; t = x >> 24
        if t == 0x54 and not x & 0x10 or t & 0x7E == 0x34:   # b.cond, cbz / cbnz
            d = (x >> 5) & 0x7FFFF
            d -= (d & 0x40000) << 1
        elif t & 0x7E == 0x36:   # tbz / tbnz
            d = (x >> 5) & 0x3FFF
            d -= (d & 0x2000) << 1
        elif t & 0x7C == 0x14:                                                                                # b, bl
            d = x & 0x3FFFFFF; d -= (d & 0x2000000) << 1
            if t & 0x80 or 4 * abs(d) > PIECE_B:
                far.append((fa + 4 * i, fa + 4 * (i + d)))
                continue
        else: continue
        lo, hi = max(min(i, i + d), -1), min(max(i, i + d), n)   # crosses the cut before instruction k when lo < k <= hi
        cross[lo + 1] += 1; cross[hi + 1] -= 1
    cand, c = [], 0
    for k in range(n):
        c += cross[k]
        x = w[k - 1] if k else 0
        if k and not c and (x & 0xFFFFFC1F in (0xD65F0000, 0xD61F0000) or x >> 26 == 0x05): cand.append(k)
    cuts, s = [], 0
    while s + PIECE // 4 < n:
        j = bisect.bisect_right(cand, s + PIECE // 4) - 1
        if j < 0 or cand[j] <= s: j = bisect.bisect_right(cand, s + PIECE // 4)
        if j >= len(cand): break
        s = cand[j]; cuts.append(s)
    b = [0] + cuts + [n]
    return [(fa + 4 * p, fa + 4 * q) for p, q in zip(b, b[1:])], far
funcs = sorted({func_of(a) for a, (k, _) in kinds.items() if k == 'x18'})
far_into, rmid_all, newly_accepted = [], set(), set()
if any(fb - fa > BIG_FN for fa, fb in funcs):
    sites18 = sorted(a for a, (k, _) in kinds.items() if k == 'x18'); split = []
    for fa, fb in funcs:
        if fb - fa <= BIG_FN:
            split.append((fa, fb))
            continue
        ps, far = pieces(fa, fb); far_into += far
        split += [(p, q) for p, q in ps if bisect.bisect_left(sites18, p) < bisect.bisect_left(sites18, q)]
        print(f'function {fa:#x}..{fb:#x} ({(fb - fa) / 2**20:.2f} MB, no unwind entries) analysed in {len(ps)} pieces')
    funcs = split
# rewriter.md#jump-tables-proved-over-control-flow
import jump_solver
def branch_entries(ranges):
    """Direct branches into each range from outside it: {range: {target}}. A data word that decodes as a branch only makes a proof stricter"""
    out = {r: set() for r in ranges}
    if not ranges: return out
    lo_ = [a for a, _ in ranges]
    def marks(tops):
        b = bytearray(256)
        for t in tops: b[t] = 1
        return bytes(b)
    far, near = marks([*range(0x14, 0x18), *range(0x94, 0x98)]), marks([0x54, 0x34, 0x35, 0xB4, 0xB5, 0x36, 0x37, 0xB6, 0xB7])
    def scan(start, end, mark):
        n = (end - start) // 4
        if n <= 0: return
        data = bytes(IMG[start:start + 4 * n]); words = memoryview(data).cast('I')
        for m_ in re.finditer(b'\x01', data[3::4].translate(mark)):
            i = m_.start(); w = words[i]; t = w >> 24
            if t == 0x54 or t & 0x7E == 0x34:
                if t == 0x54 and w & 0x10: continue
                d = (w >> 5) & 0x7FFFF; d -= (d & 0x40000) << 1
            elif t & 0x7E == 0x36:
                d = (w >> 5) & 0x3FFF
                d -= (d & 0x2000) << 1
            else:
                d = w & 0x3FFFFFF
                d -= (d & 0x2000000) << 1
            at = start + 4 * i; dest = at + 4 * d
            k = bisect.bisect_right(lo_, dest) - 1
            if k >= 0 and dest < ranges[k][1] and not ranges[k][0] <= at < ranges[k][1]: out[ranges[k]].add(dest)
    windows = []
    for a, b in ranges:
        a, b = max(0, a - (1 << 20)) & ~3, b + (1 << 20)
        if windows and a <= windows[-1][1]: windows[-1][1] = max(windows[-1][1], b)
        else: windows.append([a, b])
    for start, end in CODE_RANGES:
        scan(start, end, far)
        for a, b in windows:
            if max(start, a) < min(end, b): scan(max(start, a), min(end, b), near)
    return out
def code_ok(a): return any(s_ <= a < e_ for s_, e_ in CODE_RANGES)
solved, entries_in, pad_starts, pads_at = {}, {}, [], {}
fde_starts = set(starts)
def solve_fn(fa, fb, insl, outside=()):
    global pad_starts
    if not solved and not entries_in:
        entries_in.update(branch_entries(funcs))
        pads_at.update(nso_code.landing_pads(IMG)); pad_starts = sorted(pads_at)
    i, j = bisect.bisect_left(pad_starts, fa), bisect.bisect_left(pad_starts, fb)
    landing = {a: pads_at[a] for a in pad_starts[i:j]}
    unwound = (fa, fb) == func_of(fa) and fa in fde_starts
    solved[(fa, fb)] = jump_solver.solve(insl, IMG, code_ok, entries_in.get((fa, fb), ()), outside, landing, unwound)
    return solved[(fa, fb)]
def table_at(fa, fb, insl, at):
    found, failed = solved[(fa, fb)] if (fa, fb) in solved else solve_fn(fa, fb, insl)
    return found.get(at), failed.get(at, 'not a dispatch')
def half_pads(fa, fb, insl, at, base):
    """A halfword table's pads: its largest entry + 1, or None (all 65,536 pads, which can put the patch area out of a B's reach)"""
    tgt, _ = table_at(fa, fb, insl, at)
    if not tgt or any(t < base or (t - base) % 4 for t in tgt): return None
    return max(t - base for t in tgt) // 4 + 1
selects = {}
function_ends = dict(nso_code.fde_ranges(IMG))
for fa, fb in funcs:
    insl = disasm_fn(fa, fb)
    by_address = None
    ws = find_windows(insl)
    for wseg in ws:
        windows.append(wseg); done |= {x.address for x in wseg}
        if any(x.mnemonic == 'b' for x in wseg): newly_accepted.add(wseg[0].address)
    for j, ins in enumerate(insl):
        if ins.mnemonic == 'csel' and fa in function_ends and ins.address < function_ends[fa] <= fb and ins.address not in done:
            if args := select_x18(ins):
                if by_address is None: by_address = {p.address: p for p in insl}
                temp = select_scratch(ins.address, by_address, function_ends[fa], args[0])
                if temp is not None: selects[ins.address] = args, temp
        if ins.mnemonic == 'br' and ins.op_str == 'x18':
            base = width = None
            # vector moves between the add and the br may put the adr x18 nine back
            ctx = insl[max(0, j - 16):j]
            for p in reversed(ctx):
                if p.mnemonic == 'adr' and p.op_str.startswith('x18,'):
                    base = p.operands[1].imm
                    break
            for p in reversed(ctx):
                if p.mnemonic == 'add' and p.op_str.startswith('x18, x18,') and 'lsl #2' in p.op_str:
                    # capstone names x30 'lr' and x29 'fp'
                    rn = p.reg_name(p.operands[2].reg); idx = {'lr': 30, 'fp': 29}.get(rn, num(rn))
                    for q in reversed(ctx):
                        if q.mnemonic in ('ldrb', 'ldrsb') and num(q.reg_name(q.operands[0].reg)) == idx: width = 256
                        if q.mnemonic in ('ldrh', 'ldrsh') and num(q.reg_name(q.operands[0].reg)) == idx: width = (half_pads(fa, fb, insl, ins.address, base) if q.mnemonic == 'ldrh' else None) or 65536 if base else 65536
                        if width: break
                    break
            if not (base and width):
                if (far := jump_bytes_far(insl, j, lambda b, fa=fa, fb=fb, insl=insl, at=ins.address: half_pads(fa, fb, insl, at, b))):
                    jumps[ins.address] = far
                    continue
                tgt, why = table_at(fa, fb, insl, ins.address)
                if not tgt and tail_call(insl, j):
                    tailcalls.add(ins.address)
                    continue
                assert tgt, f'jump table {ins.address:x}: {why}'
                tabjumps[ins.address] = tgt; continue
            jumps[ins.address] = (base, width)
    # No jump may land inside a window, except a retry block's branch onto its store-exclusive (retry_block)
    inwin = {x.address for wseg in ws for x in wseg}; heads = {wseg[0].address for wseg in ws}
    rmid = set()
    for k, x in enumerate(insl):
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            t = x.operands[-1].imm
            if t in inwin and t not in heads:
                wseg = next(w for w in ws if any(y.address == t for y in w))
                if x.mnemonic != 'bl' and any(y.address == x.address for y in wseg):
                    newly_accepted.add(wseg[0].address)
                    continue
                rb = retry_block(insl, k, wseg) if x.mnemonic not in ('b', 'bl') else None
                assert rb, f'branch into window {x.address:x}'
                assert not any(y.address in done for y in rb[0]), f'retry block overlaps a window {rb[0][0].address:x}'
                tail_addrs = {y.address for y in rb[1]}
                for y in rb[1]:
                    if y.mnemonic in ('cbz', 'cbnz', 'tbz', 'tbnz') or y.mnemonic.startswith('b.'):
                        dest = y.operands[-1].imm
                        assert dest not in inwin or dest in heads or dest in tail_addrs, f'retry copy branches into window {y.address:x}'
                retries.append([(rb[0], x.address + 4), (rb[1], wseg[-1].address + 4)])
                done |= {y.address for y in rb[0]}; rmid |= {y.address for y in rb[0][1:]}
    for x in insl:
        if x.mnemonic in ('b', 'bl', 'cbz', 'cbnz', 'tbz', 'tbnz') or x.mnemonic.startswith('b.'):
            assert x.operands[-1].imm not in rmid, f'branch into retry block {x.address:x}'
    for a_, tgt in tabjumps.items():
        if fa <= a_ < fb: assert not any(t in inwin and t not in heads for t in tgt), f'jump table target into window {a_:x}'
    rmid_all |= rmid
if far_into:   # far b / bl of pieces and every table's targets may not land inside a window or a retry block
    win_mid = {x.address for wseg in windows for x in wseg[1:]}
    for at, to in far_into: assert to not in win_mid and to not in rmid_all, f'far branch into a window or retry block {at:x}'
    for at, tgt in tabjumps.items(): assert not any(t in win_mid or t in rmid_all for t in tgt), f'jump table target into a window or retry block {at:x}'

for w in windows:
    if w[0].address in newly_accepted:
        assert all(b.address == a.address + 4 for a, b in zip(w, w[1:])), f'noncontiguous window {w[0].address:x}'
new_mid = {x.address for w in windows if w[0].address in newly_accepted for x in w[1:]}
# a table's targets in another function are entries its proof did not see: prove that function again
fstarts = [fa for fa, _ in funcs]
def range_of(a):
    i = bisect.bisect_right(fstarts, a) - 1
    return funcs[i] if i >= 0 and a < funcs[i][1] else None
outside = {}
for at, tgt in tabjumps.items():
    for t in tgt:
        if (r := range_of(t)) is not None and r != range_of(at): outside.setdefault(r, set()).add(t)
for at, (b, width) in jumps.items():
    if (r0 := range_of(at)) is None: continue
    for t in range(b, b + 4 * width, 4):
        if (r := range_of(t)) is not None and r != r0: outside.setdefault(r, set()).add(t)
for (fa, fb), ts in outside.items():
    if (fa, fb) not in solved: continue
    before = solved[(fa, fb)][0]
    found, _ = solve_fn(fa, fb, disasm_fn(fa, fb), ts)
    for at in before:
        if at in tabjumps: assert set(found.get(at) or ()) == set(tabjumps[at]), f'jump table {at:x}: an entry from another function is not proved'
if new_mid:
    for start, end in CODE_RANGES:
        for at in range(start, end - 3, 4):
            w = struct.unpack_from('<I', IMG, at)[0]; t = w >> 24
            if t == 0x54 and not w & 0x10 or t & 0x7E == 0x34:
                d = (w >> 5) & 0x7FFFF
                d -= (d & 0x40000) << 1
            elif t & 0x7E == 0x36:
                d = (w >> 5) & 0x3FFF
                d -= (d & 0x2000) << 1
            elif t & 0x7C == 0x14:
                d = w & 0x3FFFFFF
                d -= (d & 0x2000000) << 1
            else: continue
            dest = at + 4 * d
            if dest in new_mid: assert func_of(at) == func_of(dest), f'branch into window from another function {at:x}'

if GAP:
    _holes = hop_holes()
    front_holes = bisect.bisect_right(_holes, PATCH_OFF + REACH)
    back_holes = len(_holes) - bisect.bisect_left(_holes, BACK_MIN - 0x100000)
    gap_sites = sorted(a for a in (
        *(w[0].address for w in windows), *(p[0][0][0].address for p in retries),
        *(a for a in kinds if a not in done),
    ) if SPLIT <= a < BACK_MIN)
    if gap_sites and front_holes:
        toward_front = len(gap_sites) * front_holes // (front_holes + back_holes)
        SPLIT = gap_sites[toward_front] if toward_front < len(gap_sites) else BACK_MIN
        print(f'{NAME}: {len(gap_sites)} sites in the branch gap, split at {SPLIT:#x} ({front_holes} front and {back_holes} back padding words)', flush=True)


def put_branch(at, to): struct.pack_into('<I', IMG, at, BX(at, to))
for wseg in windows:
    use_area(wseg[0].address); st, lo, hi = site_window(wseg); stat('window'); put_branch(lo, st)
for parts in retries:
    use_area(parts[0][0][0].address); st = site_retry_window(parts); stat('retry_window'); put_branch(parts[0][0][0].address, st)
for a in sorted(kinds):
    if a in done: continue
    use_area(a)
    k, arg = kinds[a]
    ins = dis(struct.unpack_from('<I', IMG, a)[0], a)
    if k == 'x18':
        m = ins.mnemonic
        if m == 'br' and a in tailcalls: st = site_br_call(ins); stat('br_call')
        elif m == 'br': st = site_br_tab(ins, tabjumps[a]) if a in tabjumps else site_br(ins, *jumps[a]); stat('br')
        elif m in ('cbz', 'cbnz', 'tbz', 'tbnz'): st = site_cond(ins); stat('cond')
        elif m in ('adr', 'adrp'): st = site_pcrel(ins); stat('pcrel')
        elif m == 'blr': st = site_blr(ins); stat('blr')
        elif re.match(r'(sp|wsp),', ins.op_str): st = site_sp(ins); stat('sp')
        elif casp_pair18(ins): st = site_casp(ins); stat('casp')
        elif a in selects: st = site_select(ins, *selects[a]); stat('select')
        elif m in ('ret', 'b', 'bl') or (m == 'ldr' and '[' not in ins.op_str): raise RuntimeError(f'{a:x} {m} {ins.op_str}')
        else: st = site_plain(ins); stat('plain')
    elif k == 'tpidr_r': st = site_sysreg_read(ins, 'KTLS'); stat(k)
    elif k == 'tpidrro_r': st = site_sysreg_read(ins, 'KTLR'); stat(k)
    elif k == 'tpidr_w': st = site_tpidr_write(ins); stat(k)
    elif k == 'svc': st = site_gate(ins, 'svc', arg); stat(k)
    elif k in ('cntpct', 'cntvct', 'cntfrq'): st = site_gate(ins, k, arg); stat(k)
    elif k == 'sp_align': st = site_sp_align(ins, arg); stat(k)
    elif k == 'ctr':   # DminLine = 4 (64-byte lines): movz xN, #4, lsl #16, or into K18 for x18
        if arg == 18: st = site_slot_copy(ins, 0xD2A00080 | 15, 'K18'); stat(k)
        else: struct.pack_into('<I', IMG, a, 0xD2A00080 | arg); stat(k); continue
    put_branch(a, st)

obs = list(PATCHES['OBS'].get(BUILD_ID, []))
for off, oid, ow, why in obs:
    mod_conflict(off, 4, f'an observation gate ({why})')
    w = struct.unpack_from('<I', IMG, off)[0]
    assert f'{w:08x}' == ow, f'observation gate original instruction mismatch {off:#x}: {w:08x}'
    # dm_gate saves and restores x0–x28 and the flags (dm/host/entry.S), so any relocatable instruction can take a gate
    assert off < TEXT_END and off not in kinds and not any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]), f'observation gate conflicts with a rewrite site {off:#x}'
    use_area(off); put_branch(off, site_obs(off, oid, w)); stat('obs')

# Address regions (hle_svc.c "Address regions"): gates found by the SDK's symbols in any build, all or none. AllocateAddressRegion's entry (x2 size)
# and its store of the region's address through out (register A in the gate number's upper bits); FreeAddressRegion's entry.
OBS_REGION_ASK, OBS_REGION_GOT, OBS_REGION_FREE, OBS_STDALLOC_INIT = 22, 23, 24, 25   # dm/host/dm.h
def region_gates():
    from nso_syms import defined
    sym = defined(IMG)
    ask, free = sym.get('_ZN2nn2os6detail11VammManager21AllocateAddressRegionEPmm'), sym.get('_ZN2nn2os6detail11VammManager17FreeAddressRegionEm')
    if not ask or not free: return [], None
    from nso_code import out_store
    try: got, reg = out_store(IMG, ask[0], ask[1], md)
    except ValueError as e: return [], str(e)
    sites = [(ask[0], OBS_REGION_ASK, 'VammManager::AllocateAddressRegion entry: the size in x2'),
             (got, OBS_REGION_GOT | reg << 8, f'VammManager::AllocateAddressRegion: the new region\'s address in x{reg}, stored through the out pointer'),
             (free[0], OBS_REGION_FREE, 'VammManager::FreeAddressRegion entry: the address in x1')]
    for off, _, _ in sites:
        if off >= TEXT_END or off in kinds or any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]):
            return [], f'no observation gate possible at {off:#x}'
    return [(off, oid, f'{struct.unpack_from("<I", IMG, off)[0]:08x}', why) for off, oid, why in sites], None
region_obs, why_not = region_gates()
if why_not: print(f'{NAME}: no address region gates: {why_not}')
# StandardAllocator::Initialize: the host clamps a null address's size to the alias region's room (hle_svc.c "StandardAllocator's address space"); writes x2. Both overloads.
def stdalloc_gates():
    from nso_syms import defined
    sym = defined(IMG)
    out = []
    for name in ('_ZN2nn3mem17StandardAllocator10InitializeEPvm', '_ZN2nn3mem17StandardAllocator10InitializeEPvmb'):
        if name not in sym: continue
        off = sym[name][0]
        if off >= TEXT_END or off in kinds or any(lo <= off < hi for wseg in windows for lo, hi in [(wseg[0].address, wseg[-1].address + 4)]):
            print(f'{NAME}: no StandardAllocator gate at {off:#x}: a rewrite site'); continue
        out.append((off, OBS_STDALLOC_INIT, f'{struct.unpack_from("<I", IMG, off)[0]:08x}', f'StandardAllocator::Initialize(void *, size_t{", bool" if name.endswith("b") else ""}) entry: the size in x2, capped when the address in x1 is null'))
    return out
region_obs += stdalloc_gates()
for off, oid, ow, why in region_obs:
    use_area(off)
    put_branch(off, site_obs(off, oid, int(ow, 16)))
    stat('obs')
obs += region_obs

# Frame step in whole vsyncs (docs/reference/frame-rate.md#frame-step). State page, after the key page (layout shared with the loader, the host and the stubs):
# +0 f32 target fps (0 = original code), +4 movie flag, +0x10 f32 render scale, +0x20 u32 step in vsyncs (host), +0x24 u32 step the game fixes,
# +0x30 u64 vsyncs stepped, +0x38 f32 frame seconds, +0x3c u32 stub-read mark, +0x40 u32 dynamic resolution on, +0x200..+0x293 Ocarina of Time's stubs.
# ctx: here / emit / site / put / address / state / others / found / capabilities.add.
FRAME_STEP = PATCHES['FRAME_STEP']
FS_STATE_OFF = 0
FS_CAPABILITIES = set()
if (BUILD_ID in FRAME_STEP or FS_MATCH) and NAME == 'main':
    FS_STATE_OFF = KEY_PAGE_OFF + 0x4000
    if FAR:
        # A front patch area's loader supplies the key page; reserve the state page after it too.
        IMAGE_SIZE = FS_STATE_OFF + 0x4000
    assert FS_STATE_OFF % 0x1000 == 0 and (FAR or FS_STATE_OFF + 0x40 <= PATCH_OFF) and not any(IMG[FS_STATE_OFF:]), 'frame step state page'
def stub_site(off, orig, what):
    ob = bytes.fromhex(orig)
    mod_conflict(off, len(ob), f'a {what} site')
    assert IMG[off:off + len(ob)] == ob, f'{what} original bytes mismatch {off:#x}: {bytes(IMG[off:off + len(ob)]).hex()}'
    for o in range(off, off + len(ob), 4):
        assert o not in kinds and not any(seg[0].address <= o < seg[-1].address + 4 for seg in windows), f'{what} site is a rewrite site {o:#x}'
def stub_put(off, words, why, kind):
    mod_conflict(off, 4 * len(words), f"a {kind.replace('_', ' ')} site ({why})")
    old = bytes(IMG[off:off + 4 * len(words)])
    for i, w in enumerate(words): struct.pack_into('<I', IMG, off + 4 * i, w)
    cuts.append((off, old.hex(), bytes(IMG[off:off + 4 * len(words)]).hex(), why)); stat(kind)
def stub_address(off, what, lo_off=None):   # adrp and its low instruction (adjacent unless lo_off), in the original text
    lo_off = off + 4 if lo_off is None else lo_off
    assert lo_off > off and not (off | lo_off) & 3, f'{what}: invalid address instruction offsets'
    a, b = struct.unpack_from('<I', ORIG_TEXT, off)[0], struct.unpack_from('<I', ORIG_TEXT, lo_off)[0]
    assert a & 0x9f000000 == 0x90000000, f'{what}: no adrp at {off:#x}'
    imm = (((a >> 5) & 0x7ffff) << 2 | (a >> 29) & 3) << 12
    page = (off & ~0xfff) + (imm - (1 << 33) if imm & 1 << 32 else imm)
    assert (b >> 5) & 31 == a & 31, f'{what}: the low instruction at {lo_off:#x} does not use the adrp register'
    if b & 0xffc00000 == 0x91000000: return page + ((b >> 10) & 0xfff)
    assert b & 0x3b000000 == 0x39000000, f'{what}: no add, load or store after the adrp at {off:#x}'
    return page + ((b >> 10) & 0xfff) * (1 << (b >> 30)) * (16 if b & 0x04800000 == 0x04800000 and b >> 30 == 0 else 1)   # scaled by the access size (q: 16)
if FS_STATE_OFF:
    use_area(0)
    fs = types.SimpleNamespace(here=here, emit=emit, site=lambda off, orig: stub_site(off, orig, 'frame step'),
                               put=lambda off, words, why: stub_put(off, words, why + (' [function match]' if FS_MATCH else ''), 'frame_step'),
                               state=FS_STATE_OFF, base=FS_MATCH[1] if FS_MATCH else 0, others=FS_MATCH[2] if FS_MATCH else {},
                               found=FS_MATCH[3] if FS_MATCH else None, capabilities=set(), address=lambda off, lo_off=None: stub_address(off, 'frame step', lo_off))
    (FS_MATCH[0]['stub'] if FS_MATCH else FRAME_STEP[BUILD_ID])(fs)
    FS_CAPABILITIES = fs.capabilities

# STUBS: a fix that needs more instructions than its sites hold, with the frame step's ctx and no state page. Not scanned: it must hold nothing the rewriter would rewrite.
STUBS = PATCHES['STUBS']
if BUILD_ID in STUBS or STUB_MATCH:
    use_area(0)
    st = types.SimpleNamespace(here=here, emit=emit, site=lambda off, orig: stub_site(off, orig, 'stub'),
                              put=lambda off, words, why: stub_put(off, words, why + (' [function match]' if STUB_MATCH else ''), 'stub'),
                              address=lambda off, lo_off=None: stub_address(off, 'stub', lo_off),
                              base=STUB_MATCH[1] if STUB_MATCH else 0, others=STUB_MATCH[2] if STUB_MATCH else {},
                              found=STUB_MATCH[3] if STUB_MATCH else None)
    (STUB_MATCH[0]['stub'] if STUB_MATCH else STUBS[BUILD_ID])(st)

# Spin waits (docs/reference/threads.md#yields-and-spin-waits): a loop that provably cannot leave by itself, only another thread ends it, gets a copy whose back-edges pass a spin gate
# (hle_thread.c dm_spin_wait; the gate keeps every register and the flags). The original back-edges enter the copy, so a single retry costs nothing.
SPIN_PATHS, SPIN_STEPS, SPIN_STORES = 256, 4096, 4   # complete paths back to T; search steps
# At most SPIN_STORES stores of fixed values to fixed addresses: such a loop's passes repeat after a few hundred; the gate polls 2 ms before sleeping.
SPIN_LSE = ('ldadd', 'ldclr', 'ldeor', 'ldset', 'ldsmax', 'ldsmin', 'ldumax', 'ldumin')
SPIN_BAD = {'bl', 'blr', 'br', 'ret', 'eret', 'svc', 'hvc', 'smc', 'brk', 'hlt', 'udf', 'mrs', 'msr', 'sys', 'sysl', 'wfe', 'wfi', 'sev', 'sevl', 'clrex', 'dc', 'ic', 'at',
            'tlbi', 'drps', 'dcps1', 'dcps2', 'dcps3', 'blraa', 'blrab', 'blraaz', 'blrabz', 'braa', 'brab', 'braaz', 'brabz', 'retaa', 'retab', 'eretaa', 'eretab'}
# One head's decodes (spin_region clears it): kept across heads, a worker's held every instruction near every candidate it tried
spin_dec = {}
def spin_ins(a):
    if a not in spin_dec: spin_dec[a] = next(md.disasm(bytes(IMG[a:a + 4]), a), None)
    return spin_dec[a]
def spin_norm(n):
    n = {'fp': 'x29', 'lr': 'x30', 'wsp': 'sp'}.get(n, n)
    if n in ('wzr', 'xzr'): return None
    m = re.fullmatch(r'[wx](\d+)', n)
    if m: return 'x' + m.group(1)
    m = re.fullmatch(r'[bhsdqv](\d+)', n)
    return 'v' + m.group(1) if m else n
def spin_rw(ins):
    """(reads, writes), w / x / vector views merged. Capstone's lists miss writes (adds) and add false ones (a compare), so every register operand counts too"""
    m = ins.mnemonic
    regs = [spin_norm(ins.reg_name(o.reg)) for o in ins.operands if o.type == ARM64_OP_REG]
    ops = set(regs) - {None}
    if m in ('cbz', 'cbnz', 'tbz', 'tbnz'): return ops, set()
    if m.startswith('b.'): return {'nzcv'}, set()
    if m == 'b': return set(), set()
    if m in ('cmp', 'cmn', 'tst', 'fcmp', 'fcmpe'): return ops, {'nzcv'}
    if m in ('ccmp', 'ccmn', 'fccmp', 'fccmpe'): return ops | {'nzcv'}, {'nzcv'}
    r, w = ins.regs_access()
    r, w = {spin_norm(ins.reg_name(x)) for x in r} - {None}, {spin_norm(ins.reg_name(x)) for x in w} - {None}
    if m.startswith(('ld', 'prfm', 'prfum')): return r, w | ops
    return r | (set(regs[1:]) - {None}), w | (set(regs[:1]) - {None})
def spin_kind(ins):
    if ins is None: return None
    m = ins.mnemonic
    if m in ('b', 'cbz', 'cbnz', 'tbz', 'tbnz') or m.startswith('b.'): return 'br'
    if m in ('adr', 'adrp'): return 'pc'
    if m in SPIN_BAD or m.startswith(('st', 'cas', 'swp') + SPIN_LSE): return None
    mem = any(o.type == ARM64_OP_MEM for o in ins.operands)
    if m.startswith(('ld', 'prfm', 'prfum')): return 'ok' if mem else None   # no memory operand: a pc-relative literal load
    return None if mem else 'ok'
def spin_rmw(a):
    """ld(a)xr; add|sub #k; st(l)xr; cbnz <the load> at a → (xN, ±k, registers written, the four instructions), or None"""
    l = [spin_ins(a + 4 * i) for i in range(4)]
    if None in l or l[0].mnemonic not in ('ldxr', 'ldaxr') or l[1].mnemonic not in ('add', 'sub') or l[2].mnemonic not in ('stxr', 'stlxr') or l[3].mnemonic != 'cbnz': return None
    r = lambda x, i: x.reg_name(x.operands[i].reg)
    if len(l[1].operands) != 3 or l[1].operands[2].type != ARM64_OP_IMM or 'lsl' in l[1].op_str: return None
    base = l[0].operands[1].mem
    if base.disp or base.index or l[2].operands[2].mem.base != base.base or l[2].operands[2].mem.disp: return None
    if r(l[1], 1) != r(l[0], 0) or r(l[2], 1) != r(l[1], 0) or r(l[3], 0) != r(l[2], 0) or l[3].operands[1].imm != a: return None
    k = l[1].operands[2].imm * (1 if l[1].mnemonic == 'add' else -1)
    return spin_norm(l[0].reg_name(base.base)), k, {spin_norm(r(l[0], 0)), spin_norm(r(l[1], 0)), spin_norm(r(l[2], 0))} - {None}, l
def spin_lock(a):
    """A lock sequence or test loop at a → (kind, xN, instructions, count before a, count from a on, reads, writes).
    'acq' test-and-set retried until its store succeeds, 'rel' compare-and-clear or stlr wzr, 'tst' a load-compare-branch loop; with or without clrex"""
    x0 = spin_ins(a)   # the words after it only for the forms its mnemonic starts
    if x0 is None: return None
    r = lambda x, i: x.reg_name(x.operands[i].reg)
    plain = lambda x, i: x.operands[i].type == ARM64_OP_MEM and not x.operands[i].mem.index and not x.operands[i].mem.disp
    m0 = x0.mnemonic
    if m0 == 'stlr' and len(x0.operands) == 2 and r(x0, 0) in ('wzr', 'xzr') and plain(x0, 1):
        bn = spin_norm(x0.reg_name(x0.operands[1].mem.base)); return ('rel', bn, [x0], 0, 1, {bn}, set())
    if m0 in ('ldxr', 'ldaxr') and r(x0, 0).startswith('w') and plain(x0, 1):
        l = [x0] + [spin_ins(a + 4 * i) for i in range(1, 6)]
        if None not in l:
            base = l[0].operands[1].mem.base; bn = spin_norm(l[0].reg_name(base))
            if (l[1].mnemonic == 'cbnz' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].imm == a
                    and l[2].mnemonic in ('stxr', 'stlxr') and plain(l[2], 2) and l[2].operands[2].mem.base == base and r(l[2], 1) not in ('wzr', 'xzr')
                    and l[3].mnemonic == 'cbnz' and r(l[3], 0) == r(l[2], 0) and l[3].operands[1].imm == a):
                return ('acq', bn, l[:4], 0, 4, {bn, spin_norm(r(l[2], 1))}, {spin_norm(r(l[0], 0)), spin_norm(r(l[2], 0))})
            if (l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
                    and l[2].mnemonic == 'b.ne' and l[2].operands[0].imm == a and l[3].mnemonic in ('stxr', 'stlxr') and r(l[3], 1) == 'wzr'
                    and plain(l[3], 2) and l[3].operands[2].mem.base == base and l[4].mnemonic == 'cbnz' and r(l[4], 0) == r(l[3], 0) and l[4].operands[1].imm == a):
                return ('rel', bn, l[:5], 0, 5, {bn}, {spin_norm(r(l[0], 0)), spin_norm(r(l[3], 0)), 'nzcv'})
            if (l[1].mnemonic == 'cbz' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].imm == a + 16 and l[2].mnemonic == 'clrex' and l[3].mnemonic == 'b' and l[3].operands[0].imm == a
                    and l[4].mnemonic in ('stxr', 'stlxr') and plain(l[4], 2) and l[4].operands[2].mem.base == base and r(l[4], 1) not in ('wzr', 'xzr')
                    and l[5].mnemonic == 'cbnz' and r(l[5], 0) == r(l[4], 0) and l[5].operands[1].imm == a):
                return ('acq', bn, l, 0, 6, {bn, spin_norm(r(l[4], 1))}, {spin_norm(r(l[0], 0)), spin_norm(r(l[4], 0))})
            p = spin_ins(a - 4)
            if (p is not None and p.mnemonic == 'clrex' and l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
                    and l[2].mnemonic == 'b.ne' and l[2].operands[0].imm == a - 4 and l[3].mnemonic in ('stxr', 'stlxr') and r(l[3], 1) == 'wzr'
                    and plain(l[3], 2) and l[3].operands[2].mem.base == base and l[4].mnemonic == 'cbnz' and r(l[4], 0) == r(l[3], 0) and l[4].operands[1].imm == a):
                return ('rel', bn, [p] + l[:5], 1, 5, {bn}, {spin_norm(r(l[0], 0)), spin_norm(r(l[3], 0)), 'nzcv'})
    if (m0 in ('ldr', 'ldrb', 'ldrh') and len(x0.operands) == 2 and r(x0, 0).startswith('w') and x0.operands[1].type == ARM64_OP_MEM
            and not x0.operands[1].mem.index and not x0.writeback):
        l = [x0, spin_ins(a + 4), spin_ins(a + 8)]
        if (None not in l and l[1].mnemonic == 'cmp' and r(l[1], 0) == r(l[0], 0) and l[1].operands[1].type == ARM64_OP_IMM
                and l[2].mnemonic in ('b.eq', 'b.ne') and l[2].operands[0].imm == a):
            bn = spin_norm(l[0].reg_name(l[0].operands[1].mem.base)); return ('tst', bn, l, 0, 3, {bn}, {spin_norm(r(l[0], 0)), 'nzcv'})
    return None
def spin_store(ins):
    """A plain store of a w register or zr to [xN{,#imm}] → (value register or None for zr, base), or None"""
    if ins is None or ins.mnemonic not in ('str', 'strb', 'strh', 'stur', 'sturb', 'sturh') or ins.writeback or len(ins.operands) != 2: return None
    v, o = ins.operands
    if v.type != ARM64_OP_REG or o.type != ARM64_OP_MEM or o.mem.index: return None
    vn = ins.reg_name(v.reg)
    if not (vn.startswith('w') or vn in ('xzr',)): return None
    return (spin_norm(vn), spin_norm(ins.reg_name(o.mem.base)))
def spin_rebranch(ins, pc, tgt):
    w = struct.unpack('<I', ins.bytes)[0]; off = tgt - pc
    if ins.mnemonic == 'b': return BX(pc, tgt)
    if ins.mnemonic in ('tbz', 'tbnz'):
        assert -(1 << 15) <= off < (1 << 15) and off % 4 == 0, f'spin wait tbz out of range {pc:#x}'
        return (w & ~(0x3FFF << 5)) | (((off >> 2) & 0x3FFF) << 5)
    assert -(1 << 20) <= off < (1 << 20) and off % 4 == 0, f'spin wait branch out of range {pc:#x}'
    return (w & ~(0x7FFFF << 5)) | (((off >> 2) & 0x7FFFF) << 5)
spin_touched = set(kinds) | done | {o for off, oid, ow, why in obs for o in (off,)} | {o for off, ob, nb, why in cuts for o in range(off, off + len(ob) // 2, 4)}
# Words that end every path through them by their encoding alone: bl; br / blr / ret; svc / brk; plain and vector stores; stp / stnp; LSE atomics.
SPIN_STOP = ((0xFC000000, 0x94000000), (0xFE000000, 0xD6000000), (0xFF000000, 0xD4000000), (0x3FC00000, 0x39000000), (0x3FC00000, 0x3D000000), (0x3FC00000, 0x3D800000),
             (0x3FE00000, 0x38000000), (0x3FE00C00, 0x38200800), (0x3FE00000, 0x3C000000), (0x3FE00000, 0x3C800000), (0x3A400000, 0x28000000), (0x3F200C00, 0x38200000))
def spin_quick(T, hi):
    """a back-edge to T reached without a word of SPIN_STOP (only such heads get the full check)"""
    seen, todo = set(), [T]
    while todo:
        a = todo.pop()
        if a in seen or not T <= a <= hi: continue
        seen.add(a); w = struct.unpack_from('<I', IMG, a)[0]; t = w >> 24
        if 0x14 <= t <= 0x17: succ = (a + (((w & 0x3FFFFFF) ^ 0x2000000) - 0x2000000) * 4,)
        elif t == 0x54 or t & 0x7E == 0x34: succ = (a + ((((w >> 5) & 0x7FFFF) ^ 0x40000) - 0x40000) * 4, a + 4)
        elif t & 0x7E == 0x36: succ = (a + ((((w >> 5) & 0x3FFF) ^ 0x2000) - 0x2000) * 4, a + 4)
        elif any(w & m == v for m, v in SPIN_STOP) and not (w & 0xBFC00000 in (0xB9000000, 0x39000000, 0x79000000) or w & 0xBFE00C00 in (0xB8000000, 0x38000000, 0x78000000)): continue   # str / strb / strh may store the same on every pass
        else: succ = (a + 4,)
        if T in succ: return True
        todo.extend(succ)
    return False
class SpinNo(Exception): pass
def spin_region(T, bes):
    """Region with head T → dict, or None. A store whose value or address a pass changes ends its paths; a lock sequence at T is one node of the poll it heads"""
    hi = max(bes)
    if any(s < hi + 4 and T < e for s, e in DATA_BLOCKS) or not spin_quick(T, hi): return None   # before any decode
    try:
        lk = spin_lock(T)
        for head in (True, False) if lk and lk[0] in ('acq', 'rel') else (True,):
            ban = set()
            for _ in range(8):
                g = spin_region1(T, bes, ban, True, head)
                if not isinstance(g, set): break
                ban |= g
            old = spin_region1(T, bes, set(), False, head)
            g = g if isinstance(g, dict) and (not old or old['valid'] <= g['valid']) else old
            if g: break
        return g
    finally:
        spin_dec.clear()   # a region keeps the instructions it holds; spin_emit decodes nothing
def spin_region1(T, bes, ban, stores, head):
    hi = max(bes)
    nodes, back, bad, inner = {}, set(), set(), set()
    todo = [T]
    while todo:
        a = todo.pop()
        if a in nodes or a in bad: continue
        if a in ban:
            bad.add(a)
            continue
        rmw = spin_rmw(a) if a + 12 <= hi and struct.unpack_from('<I', IMG, a)[0] & 0x3FE00000 == 0x08400000 else None   # ldxr / ldaxr
        lk = None if rmw else spin_lock(a)
        if lk and a == T and (lk[0] == 'tst' or not head): lk = None
        if rmw:
            n = {'kind': 'rmw', 'base': rmw[0], 'k': rmw[1], 'ins': rmw[3], 'uses': {rmw[0]}, 'defs': rmw[2], 'succ': [a + 16]}
        elif lk:
            n = {'kind': lk[0], 'base': lk[1], 'k': {'acq': 1, 'rel': -1, 'tst': 0}[lk[0]], 'ins': lk[2], 'pre': lk[3], 'uses': lk[5], 'defs': lk[6], 'succ': [a + 4 * lk[4]]}
        elif stores and (st := spin_store(x := spin_ins(a))):
            n = {'kind': 'stc', 'st': st, 'ins': [x], 'uses': {st[1]} | ({st[0]} - {None}), 'defs': set(), 'succ': [a + 4]}
        else:
            x = spin_ins(a); k = spin_kind(x)
            if k is None:
                bad.add(a)
                continue
            if k == 'br':
                t = x.operands[-1].imm
                succ = [t] if x.mnemonic == 'b' else [t, a + 4]
            else: succ = [a + 4]
            n = {'kind': k, 'ins': [x], 'succ': succ}
        pre = n.get('pre', 0)
        if any(a + 4 * (i - pre) in spin_touched for i in range(len(n['ins']))):
            bad.add(a)
            continue
        nodes[a] = n
        if n['kind'] == 'rmw': inner |= {a + 4, a + 8, a + 12}
        if n['kind'] in ('acq', 'rel', 'tst'): inner |= {a + 4 * (i - pre) for i in range(len(n['ins']))} - {a}
        for s in n['succ']:
            if s == T: back.add(a)
            elif T < s <= hi: todo.append(s)
    if not back or inner & set(nodes) or any(s in inner for n in nodes.values() for s in n['succ']): return None
    def prune(ends):
        keep, more = set(ends), True
        while more:
            more = False
            for a, n in nodes.items():
                if a not in keep and any(s in keep and s != T for s in n['succ']):
                    keep.add(a)
                    more = True
        return keep
    R = prune(back)
    for a in R:
        if 'uses' not in nodes[a]: nodes[a]['uses'], nodes[a]['defs'] = spin_rw(nodes[a]['ins'][0])
    # every path from T to a back-edge: no inner cycle, and the exclusive adds on each address sum to 0
    bad_back, cnt = set(), [0, 0]
    def walk(a, onpath, ver, delta):
        cnt[1] += 1
        if cnt[1] > SPIN_STEPS: raise SpinNo
        n = nodes[a]
        if n['kind'] in ('rmw', 'acq', 'rel'):
            key = (n['kind'] != 'rmw', n['base'], ver.get(n['base'], 0)); delta = dict(delta); delta[key] = delta.get(key, 0) + n['k']
            if n['kind'] != 'rmw' and not 0 <= delta[key] <= 1: raise SpinNo   # a release before its acquire or a second acquire: not a lock taken once a pass
        if n['defs']:
            ver = dict(ver)
            ver.update({r: ver.get(r, 0) + 1 for r in n['defs']})
        for s in n['succ']:
            if s == T:
                cnt[0] += 1
                if cnt[0] > SPIN_PATHS: raise SpinNo
                if any(delta.values()): bad_back.add(a)
            elif s in R:
                if s in onpath: raise SpinNo
                walk(s, onpath | {s}, ver, delta)
    try: walk(T, {T}, {}, {})
    except SpinNo: return None
    valid = back - bad_back
    if not valid: return None
    R = prune(valid)
    # no state carried from one pass to the next
    live, more = {a: set() for a in R}, True
    while more:
        more = False
        for a in sorted(R, reverse=True):
            n, out = nodes[a], set()
            for s in n['succ']:
                if s == T: out |= live[T] if a in valid else set()
                elif s in R: out |= live[s]
            new = n['uses'] | (out - n['defs'])
            if new != live[a]:
                live[a] = new
                more = True
    defs = set().union(*(nodes[a]['defs'] for a in R))
    if any(nodes[a]['kind'] == 'stc' for a in R) and not any(nodes[a]['kind'] in ('rmw', 'acq', 'rel', 'tst') or nodes[a]['ins'][0].mnemonic.startswith('ld') for a in R): return None
    vary = {a for a in R if nodes[a]['kind'] == 'stc' and nodes[a]['uses'] & defs}
    if vary: return vary
    if sum(nodes[a]['kind'] == 'stc' for a in R) > SPIN_STORES: return None
    if live[T] & defs: return None
    return {'T': T, 'R': sorted(R), 'nodes': nodes, 'valid': valid}
def spin_emit(g):
    T, R, nodes, valid = g['T'], g['R'], g['nodes'], g['valid']
    use_area(T); cpy, fixes, tramp = {}, [], {}
    for i, a in enumerate(R):
        n = nodes[a]; cpy[a] = here(); x = n['ins'][0]
        if n['kind'] == 'rmw':
            for y in n['ins'][:3]: emit(struct.unpack('<I', y.bytes)[0])
            fixes.append((len(patch), n['ins'][3], a)); emit(0)         # retry: the copy's exclusive load
        elif n['kind'] in ('acq', 'rel'):   # their branches stay inside the sequence
            cpy[a] = here() + 4 * n['pre']
            for y in n['ins']: emit(struct.unpack('<I', y.bytes)[0])
        elif n['kind'] == 'tst':
            for y in n['ins'][:2]: emit(struct.unpack('<I', y.bytes)[0])
            fixes.append((len(patch), n['ins'][2], ('orig', a))); emit(0)
        elif n['kind'] == 'pc':
            rd, tgt = num(spin_norm(x.reg_name(x.operands[0].reg))), x.operands[1].imm
            emit(ADRP(rd, here(), tgt))
            if x.mnemonic == 'adr': emit(ADDI(rd, rd, tgt & 0xFFF))
        elif n['kind'] == 'br':
            fixes.append((len(patch), x, None))
            emit(0)
        else: emit(struct.unpack('<I', x.bytes)[0])
        if not (n['kind'] == 'br' and x.mnemonic == 'b'):
            ft = a + 4 * (len(n['ins']) - n.get('pre', 0))
            if not (i + 1 < len(R) and R[i + 1] == ft):
                fixes.append((len(patch), None, ft))
                emit(0)
    pause = here()
    emit(STP_PRE(29, 30, 31, -16)); emit_gate_call(); emit(BLR(30)); ret = here(); emit(LDP_POST(29, 30, 31, 16))
    gates.append((ret, 'spin', 0, T)); emit(BX(here(), cpy[T]))
    def out(t):
        if t not in tramp:
            tramp[t] = here()
            emit(BX(here(), t))
        return tramp[t]
    for idx, x, info in fixes:
        pc = PATCH_BASE + idx
        if x is None: w = BX(pc, cpy.get(info, info))
        elif isinstance(info, tuple): w = spin_rebranch(x, pc, out(info[1]))
        elif info is not None: w = spin_rebranch(x, pc, cpy[info])
        else:
            t, cond = x.operands[-1].imm, x.mnemonic != 'b'
            d = pause if t == T and x.address in valid else cpy[t] if t in cpy and t != T else out(t) if cond else t
            w = spin_rebranch(x, pc, d)
        struct.pack_into('<I', patch, idx, w)
    for b in sorted(valid):
        x = nodes[b]['ins'][0]
        if x.mnemonic == 'b': put_branch(b, cpy[T])
        else:
            st = here()
            emit(spin_rebranch(x, here(), cpy[T]))
            emit(BX(here(), b + 4))
            put_branch(b, st)
    stat('spin')
spin_heads = sorted(spin_cand)

def spin_heads_with_region(first, step):
    return [T for T in spin_heads[first::step] if spin_region(T, spin_cand[T])]

def find_spin_regions():
    """Capstone objects cannot leave a worker: workers say which heads have a region, built again here"""
    step = min(8 * WORKERS, len(spin_heads))
    heads = []
    for found in in_parallel(spin_heads_with_region, [(first, step) for first in range(step)]):
        heads += found
    return [g for T in sorted(heads) if (g := spin_region(T, spin_cand[T]))]

spin_regions = find_spin_regions()   # all found on the unchanged image first: emitting patches back-edges
spin_sites = [(g['T'], sorted(g['valid']), sum(len(g['nodes'][a]['ins']) for a in g['R'])) for g in spin_regions]
for g in spin_regions: spin_emit(g)
if os.environ.get('DM_SPIN_LIST'):
    for T, bes, n in spin_sites: print(f'{NAME} spin wait {T:#x} ({n} instructions), back-edges ' + ' '.join(f'{b:#x}' for b in bes))

os.makedirs(outdir, exist_ok=True)
P = lambda ext: os.path.join(outdir, f'{NAME}.{ext}')
patch = patch_front
BACK_LEN = (len(patch_back) + 0x3FFF) & ~0x3FFF
if FAR:   # file = text + ro + data (16K-padded) + patch area (TWO: front area 16K-padded, then back area)
    if BIG_TEXT and len(patch) > FRONT - 0x4000 and 'DM_PATCH_FRONT' not in os.environ:
        need = (len(patch) + 0x4000 + 0xFFFF) & ~0xFFFF
        print(f'{NAME}: patch area of {len(patch):#x} bytes does not fit in {FRONT:#x} before the base; rewriting again with {need:#x}', flush=True)
        os.execve(sys.executable, [sys.executable] + sys.argv, dict(os.environ, DM_PATCH_FRONT=hex(need)))
    assert len(patch) <= FRONT - 0x4000, f'patch area of {len(patch):#x} bytes does not fit in the {FRONT:#x} before the base'
    if TWO:
        assert IMAGE_SIZE <= BACK_OFF, 'back patch area overlaps the key page or the frame step page'
        IMAGE_SIZE = BACK_OFF + BACK_LEN
        patch = patch + bytearray(-len(patch) & 0x3FFF) + patch_back + bytearray(BACK_LEN - len(patch_back))
    open(P('patched.bin'), 'wb').write(IMG + bytearray(FILE_IMAGE_LEN - len(IMG)) + patch)
else:
    open(P('patched.bin'), 'wb').write(IMG + bytearray(PATCH_OFF - len(IMG)) + patch)
import hashlib
with open(os.path.join(outdir, 'build_src.sha256'), 'w') as f:
    for src_ in (os.path.abspath(__file__), nso.__file__, nso_code.__file__, nso_asm.__file__, nso_syms.__file__, *PATCH_FILES): f.write(hashlib.sha256(open(src_, 'rb').read()).hexdigest() + f'  {os.path.relpath(os.path.abspath(src_), os.path.dirname(os.path.abspath(__file__)))}\n')
with open(P('fixups.tsv'), 'w') as f:
    f.write(f'# PATCH_OFF\t{PATCH_OFF:#x}\n# KEY_PAGE_OFF\t{KEY_PAGE_OFF:#x}\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t{TEXT_END:#x}\n# RO_OFF\t{RO_OFF:#x}\n# DATA_OFF\t{DATA_OFF:#x}\n')
    if FAR: f.write(f'# IMAGE_SIZE\t{IMAGE_SIZE:#x}\n# FILE_IMAGE_LEN\t{FILE_IMAGE_LEN:#x}\n')
    if TWO: f.write(f'# BACK_OFF\t{BACK_OFF:#x}\n# BACK_LEN\t{BACK_LEN:#x}\n')
    for k in KEY_INDEX: f.write(f'# KEY_{k}_IDX\t{KEY_INDEX[k]}\n')
    if FS_STATE_OFF: f.write(f'# FPS_STATE_OFF\t{FS_STATE_OFF:#x}\n')
    if NAME == 'main':
        for capability in sorted(set(PATCHES['CAPABILITIES'].get(BUILD_ID, ())) | FS_CAPABILITIES):
            assert re.fullmatch(r'[A-Z][A-Z0-9_]*', capability), f'invalid capability {capability}'
            f.write(f'# {capability}\t1\n')
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
with open(P('gates.tsv'), 'w') as f:
    for ret, k, arg, orig in gates: f.write(f'{ret:#x}\t{k}\t{arg}\t{orig:#x}\n')
with open(P('sites.tsv'), 'w') as f:
    for a in sorted(kinds):
        if a not in done: f.write(f'{a:#x}\t{kinds[a][0]}\n')
    for wseg in windows: f.write(f'{wseg[0].address:#x}\tW\t{wseg[-1].address + 4:#x}\n')
    for a, (b, c) in jumps.items(): f.write(f'{a:#x}\tJ\t{b:#x}\t{c}\n')
    for a, tgt in tabjumps.items(): f.write(f'{a:#x}\tT\t' + ' '.join(f'{t:#x}' for t in tgt) + '\n')
    for t, bes, n in spin_sites: f.write(f'{t:#x}\tS\t' + ' '.join(f'{b:#x}' for b in bes) + '\n')
with open(P('cuts.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, ob, nb, why in cuts: f.write(f'{off:#x}\t{ob}\t{nb}\t{why}\n')
# Runs of text words that no longer run as the file has them (not those that only gained ordering): the host refuses a mod's code patch over them.
def changed_runs():
    words = set()
    for o in range(0, TEXT_END, 0x1000):
        if IMG[o:o + 0x1000] != ORIG_TEXT[o:o + 0x1000]:
            words.update(w for w in range(o, min(o + 0x1000, TEXT_END), 4) if IMG[w:w + 4] != ORIG_TEXT[w:w + 4]
                         and not (w in ORDERED and struct.unpack_from('<I', IMG, w)[0] == struct.unpack_from('<I', ORIG_TEXT, w)[0] | 0x8000))
    for wseg in windows: words.update(range(wseg[0].address, wseg[-1].address + 4, 4))
    for g in spin_regions:
        for a in g['R']:
            n = g['nodes'][a]; pre = n.get('pre', 0)
            words.update(a + 4 * (i - pre) for i in range(len(n['ins'])))
    runs = []
    for w in sorted(words):
        if runs and runs[-1][0] + runs[-1][1] == w: runs[-1][1] += 4
        else: runs.append([w, 4])
    return runs
with open(P('changed.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, n in changed_runs(): f.write(f'{off:#x}\t{n:#x}\n')
with open(P('obs.tsv'), 'w') as f:
    f.write(f'# BUILD_ID\t{BUILD_ID}\n')
    for off, oid, ow, why in obs: f.write(f'{off:#x}\t{oid}\t{ow}\t{why}\n')
print(NAME, stats, 'cuts', len(cuts), 'obs', len(obs), 'patch', len(patch_front) + len(patch_back), 'PATCH_OFF', hex(PATCH_OFF), *(('BACK_OFF', hex(BACK_OFF), 'back', len(patch_back)) if TWO else ()), 'slot_refs', len(fix), 'gates', len(gates))
if DATA_BLOCKS: print(NAME, 'data blocks left alone:', ', '.join(f'{s:#x}..{e:#x}' for s, e in DATA_BLOCKS))
