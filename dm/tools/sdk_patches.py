# The rewriter's patches of the game's Nintendo SDK module (sdk): dm/tools/patch_nso.py applies them to the module of that name. A patch of the game's own modules goes in its
# profile's patches.py (dm/games/0400486024DEC000/patches.py). CUTS as described in patch_nso.py ("Registered patches")
CUTS = {}   # none: no cut is bound to an sdk build; the sdk takes the function-matched rule below

# The function-matched rule (dm/tools/nso_match.py: found by the function's fingerprint, so an update of the game that keeps the function gets it without a cut for its build).
# The AddressRegionManager constructor reserves a node area at the start of the alias region: 0x3fe00000 (about 1 GB), which does not matter in the Switch 2's 64 GB alias region
# and takes a quarter of the host's (the profile's mem_alias, 4 GB). The rule makes it 0x3e00000 (62 MB, about 1.35 million 0x30-byte nodes; a few hundred regions are live in
# measurements). Both instructions change together: the limit, and the length registered as an occupied region. The host's StandardAllocator cap counts on the 62 MB (alias_room,
# dm/host/hle_svc.c): with 1 GB the capped region did not fit the alias region (found on Fire Emblem Fortune's Weave, whose sdk has the same constructor)
node_area = 'AddressRegionManager node area 1 GB → 62 MB'
def mov(old, value):   # the original mov / orr of a constant, as the cut's reason names it (its destination register: the word's low 5 bits)
    rd = int(old[:2], 16) & 31
    return f'orr w{rd},wzr,#{value:#x}' if int(old[6:8], 16) == 0x32 else f'mov w{rd},#{value:#x}'
def node(size, fingerprints, limit, limit_old, limit_new, occupied, occupied_old, occupied_new):
    return {'name': node_area, 'size': size, 'fingerprints': fingerprints, 'cuts': [
        (limit, limit_old, limit_new, f'AddressRegionManager constructor: node area limit {mov(limit_old, 0x3fe00000)} → {mov(limit_new, 0x3e00000)}'),
        (occupied, occupied_old, occupied_new,
         f'AddressRegionManager constructor: length of the node area registered as an occupied region {mov(occupied_old, 0x3fe00000)} → {mov(occupied_new, 0x3e00000)}')]}
MATCH_CUTS = {'sdk': [
    # SDK 22.2 built for the Switch 2 (Ocarina of Time v131072: sdk 1643a838…, the constructor at sdk+0x16ba30, the movs at +0x16ba48 and +0x16bad4), with return address signing and
    # its own frame
    node(328, ['5f2f7a70fa5c25aeb7020f75dc785fd6ffe6c2f5c72d8fcb30e0ea9a0bb4bbc5'], 0x18, '08fca752', '087ca052', 0xa4, '09fca752', '097ca052'),
]}

# How general the patch is and could be (dm/tools/docs_patches.py "Binding", listed in docs/agents/game-patches.md): what, its binding now, the loosest it could have (goal),
# purpose, why; blocked or kept: why the gap stays open (cannot close yet, or left open on purpose); checked: what was checked on game files (else a judgement)
BINDING = [
    {'what': 'node area 1 GB → 62 MB: the function-matched rule', 'now': 'sdk', 'goal': 'sdk', 'purpose': 'address space',
     'checked': 'Ocarina of Time v131072: the rule resolves to the two cuts at sdk+0x16ba48 and +0x16bad4 (sdk.cuts.tsv), 2026-11-21',
     'why': 'one rule for the reviewed implementation; an sdk build that keeps the function gets it'},
]
