# Ocarina of Time (0400486024DEC000): the rewriter's patches of the game's own modules, in the format of dm/tools/patch_nso.py ("Registered patches").
from nso_asm import ADRP, B

# Frame ratio. Once a frame main+0x6e6be0 (v131072) divides its base rate (60.0, or 30.0 while bit 0 of [x21+0x24f] is set) by the int frame rate at [x21+0x2c8]
# (fdiv s8,s1,s0 at +0xe4) and steps the game's time by the result, whatever the frame took: a frame of two vsyncs at rate 60 plays at half speed.
# The stub makes the ratio the host's step × base / 60, and counts the vsyncs stepped. Below the target frames are not whole vsyncs long, so it takes the larger of that
# and base × the measured frame time (state +0x38, float seconds, 0 = at the target; hle_nvn.c fstep_tick; DM_FRAME_DELTA=0 leaves it 0). It marks state +0x3c so
# the host measures only for a main that reads the value. Until the host has decided a step, and at fps_target 0, the division runs.
# s0 is written again on every path before it is read and the function uses neither x16 nor x17; s1 keeps the base rate.
# The division's site came from Lichii's notes (a GPL emulator; only the fact was taken).
# v0 (1.0.0) compiled the same function (main+0x6b5900) with its globals in x21..x25: the division is at +0x108, the same checks hold (s0 dead after it: every path
# writes it or calls a function whose first call is nn::os::GetTlsValue or LockMutex; no x16 / x17), and the view-size update keeps its scale at +0xe54
def frame_step(ctx, site=0xe4, view=(0xce4, 0xd48, 0xe5c)):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = base + site
    fs_site(site, '2818201e')   # fdiv s8,s1,s0
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    for w in (0xb9400211,   # ldr w17,[x16]          target fps (0 = off)
              0x34000271,   # cbz w17,orig (+19)
              0xb9402211,   # ldr w17,[x16,#0x20]    step in vsyncs
              0x34000231,   # cbz w17,orig (+17)     not decided yet
              0x1e230220,   # ucvtf s0,w17
              0xf9401a10,   # ldr x16,[x16,#0x30]
              0x8b110210):  # add x16,x16,x17
        emit(w)
    emit(ADRP(17, here(), FS_STATE_OFF))
    for w in (0xf9001a30,   # str x16,[x17,#0x30]    vsyncs stepped
              0x1e210808,   # fmul s8,s0,s1          step × base rate
              0x52a84e10,   # mov w16,#0x42700000    60.0
              0x1e270200,   # fmov s0,w16
              0x1e201908):  # fdiv s8,s8,s0          / 60
        emit(w)
    emit(ADRP(16, here(), FS_STATE_OFF))   # the page again: x17, a scratch register, need not survive the store above
    for w in (0xbd403a00,   # ldr s0,[x16,#0x38]     seconds; 0 = at the target
              0x52800031,   # mov w17,#1
              0xb9003e11,   # str w17,[x16,#0x3c]    this main reads the frame time
              0x1e210800,   # fmul s0,s0,s1
              0x1e204908):  # fmax s8,s8,s0          never below the host's step
        emit(w)
    emit(B(here(), site + 4))
    emit(0x1e201828); emit(B(here(), site + 4))   # orig: fdiv s8,s1,s0; back
    fs_put(site, [B(site, start)], 'frame ratio: the host\'s step × base rate / 60, or base rate × the measured frame time when that is longer (state +0x38), instead of base rate / the game\'s frame rate (state page target fps; 0 = the game\'s ratio)')
    dynamic_resolution(ctx)
    render_scale(ctx, view)


def frame_step_v0(ctx):
    frame_step(ctx, 0x108, (0xcd4, 0xd38, 0xe54))

# Dynamic resolution (game note, "Dynamic resolution"). The update (main+0x805aa0 on v131072) reads the frame's GPU load (about 0.2 ms a unit) into +0x8, takes off its
# small fixed-cost model and divides the rest by the current scale squared; the level function (main+0x8065e0) multiplies that back by each level's scale squared and
# returns the best level under the threshold (+0x10, 83 = about 16.4 ms). This game's cost on Apple GPUs hardly follows the resolution, so that model held the controller
# at its floor for good.
# The stub replaces the level function. With DM_DYNAMIC_RESOLUTION 0 (state +0x40 0, the default) it returns 0: full resolution. With 1 it climbs on the load measured at
# each level, with no model of how the cost grows: one level down when the load (+0x8) has been over the threshold for 8 calls in a row, and that level is marked as not
# fitting; one level up when the load fits and the level above was never measured, fitted when it was last measured, or was measured more than 600 calls ago; else the
# current level (+0x174). Nothing is decided or recorded until a level has held 8 calls: the load is read a few frames late. The 8 calls over in a row keep a single slow
# frame, or the first frames at a new size, from marking a level that fits. The game's counters still pace the steps; its level count (+0x1a0) is the floor.
# The memory is in the state page: +0x200 calls, +0x204 the last call's level, +0x208 calls at it, +0x20c calls over in a row, +0x210 the load measured at each level
# (16 floats), +0x250 the call it was measured at (0 = never).
# The threshold is the game's at 60 fps (LoadThreshold +0xc, or its override +0x14, plus the mode's offset: 83 docked, 105 handheld), scaled by 60 / the host's target
# fps (state +0) and at most one frame of the target rate, 5000 / target fps units (an integer division); at target 0 the game's alone. The host keeps every frame for
# whole vsyncs, so a level that fits 105 and not 83 would hold 30 fps.
# The function is a leaf: x2..x11, x16, x17 and s0..s2 are free, w0 its only result (the original also stores its predicted cost at +0x18, which only it and the update's
# own decision read). The update's fingerprint is required too: +0x8 and +0x174 are its fields
drs_controller = (   # x0 the controller, x16 the state page (the ADRP before it)
    0xb9404211, 0x35000071, 0x2a1f03e0, 0xd65f03c0,                                                   # off (state +0x40 0): level 0
    0xbd401001, 0xbd400202, 0x1e380049, 0x34000129, 0x52a84e0a, 0x1e270140, 0x1e200821, 0x1e221821,   # threshold × 60 / target fps (state +0; target 0: the game's)
    0x5282710a, 0x1ac90949, 0x1e230122, 0x1e227821, 0xbd400800,                                       # at most 5000 / target fps; then the load
    0xb9417402, 0xb941a003, 0x7100005f, 0x1a9fa042, 0x528001e9, 0x6b09005f,                           # level (0..15), count
    0x1a89d042, 0xb9420204, 0x11000484, 0xb9020204, 0xb9420605, 0xb9420a06, 0x6b0200bf, 0x1a8617e6,   # calls + 1; calls at this level
    0xb9020602, 0xb9020a06, 0x35000046, 0xb9020e1f, 0x91084207, 0x91094208, 0x710020df, 0x5400040b,   # a new level: none over; under 8 calls: stay
    0x1e212000, 0x540001ad, 0xb9420e0b, 0x1100056b, 0xb9020e0b, 0x7100217f, 0x5400032b,               # over: under 8 in a row, stay
    0xbc2258e0, 0xb8225904, 0x51000469, 0x1100044a, 0x6b09015f, 0x1a89d140, 0xd65f03c0,               # 8 in a row: record it, down one (to the floor)
    0xb9020e1f, 0xbc2258e0, 0xb8225904, 0x34000182, 0x51000449, 0xb869590a, 0x340000ea, 0x4b0a008a,   # fits: record it; at 0 stay; the level above's record
    0x7109615f, 0x54000088, 0xbc6958e2, 0x1e212040, 0x540000aa, 0x2a0903e0, 0xd65f03c0,               # never, stale or fitting: up one
    0x2a1f03e0, 0xd65f03c0, 0x2a0203e0, 0xd65f03c0)                                                   # 0; or stay
# Under a host render scale below 1.0 (state +0x10, float bits in 1..0x3f7fffff), level 0 (the game's full size, which render_scale then multiplies), so that
# DM_DYNAMIC_RESOLUTION=1 does not fight DM_RENDER_SCALE. Emitted only when render_scale also matches
drs_render_scale_guard = (   # x16 the state page; w17, w2 free (same leaf contract as drs_controller)
    0xb9401211,   # ldr w17, [x16, #0x10]   the host render scale's bits (1.0 = the game's own size)
    0x340000d1,   # cbz w17, +6             unset (0): fall through to drs_controller
    0x52a7f002,   # mov w2, #0x3f800000     1.0
    0x6b02023f,   # cmp w17, w2
    0x54000062,   # b.hs +3                 1.0 or more, negative, or NaN: fall through to drs_controller
    0x2a1f03e0,   # mov w0, wzr             level 0
    0xd65f03c0)   # ret


def dynamic_resolution(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state
    site = ctx.others.get('dynamic resolution')
    if site is None or ctx.others.get('dynamic resolution update') is None: return   # not both found in this build (a red line from the locator): the game's own dynamic resolution
    fs_site(site, '017c40bd')   # ldr s1,[x0,#0x7c]
    has_render_scale = ctx.others.get('render scale') is not None
    start = here(); emit(ADRP(16, here(), FS_STATE_OFF))
    if has_render_scale:
        for w in drs_render_scale_guard: emit(w)
    for w in drs_controller: emit(w)
    fs_put(site, [B(site, start)], 'dynamic resolution level: 0 (full resolution)' + (' also under a render scale below 1.0' if has_render_scale else '') +
           ', or with state +0x40 (DM_DYNAMIC_RESOLUTION) a climb on the load measured at each level, against the game\'s threshold or one frame of the target rate, whichever is lower')
    ctx.capabilities.add('DYNAMIC_RESOLUTION')

# Render scale (DM_RENDER_SCALE, state +0x10, written by dm/host/loader.c): the 3D views at that fraction of the game's own size, the dynamic resolution held at its top
# level. The view-size update (main+0x31d3200 on v131072) stores the views' scale at +0xd48 (str s0,[x20,#0xe5c], the only float store to #0xe5c in main): the level's
# ResolutionReduceScale normalized by [x20+0xe58]/[x20+0xe50]. That store and the size loop after it (+0xdd8, which clears the "scale changed" bit 3 of [x20+0xe2c]) run
# only in a frame where the dynamic-resolution update (main+0x805aa0) answers status 0, a new scale: at a steady frame rate it answers 2, so a stub at the store alone
# would change nothing until the game came under load. Two stubs: at the update's answer (+0xce4, ubfx of its status byte) a host scale in (0.0, 1.0) whose last stored
# value (state +0x290) does not match [x20+0xe5c] makes any non-1 status 0, so that frame recomputes the sizes (again if [x20+0xe5c] is ever reset); at the store (+0xd48)
# s0 is multiplied by the host's scale and recorded at state +0x290. The level stub returns level 0 under such a scale; 1.0 (the loader's default) runs the game's own code.
# Registers and flags are preserved except x8 (the status read next) and s0 (stored, dead after +0xd48: reloaded at +0xd84)
def _bcond(frm, to, cond): return 0x54000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | cond
def _cbz_w(frm, to, rt): return 0x34000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | rt


def _ldr_w17_x20(off): return 0xb9400291 | (off // 4) << 10
def _str_s0_x20(off): return 0xbd000280 | (off // 4) << 10


def render_scale_status_body(state_adrp, scale=0xe5c):   # 19 words; state_adrp is word 4 (ADRP x16, state) or MOV x16, x1 in unit tests; scale: the views' scale field (v0 0xe54)
    at = lambda k: 4 * k
    return (
        0xd3609c08,                       # ubfx x8,x0,#32,#8      the original
        0xa9be47f0,                       # stp x16,x17,[sp,#-32]!
        0xd53b4210,                       # mrs x16,nzcv
        0xf9000bf0,                       # str x16,[sp,#16]
        state_adrp,                       # x16 = state page
        0xb9401211,                       # ldr w17,[x16,#0x10]    the host scale's bits
        _cbz_w(at(6), at(16), 17),        # cbz w17,done           unset
        0x53177e31,                       # lsr w17,w17,#23        sign + exponent (< 0x7f iff positive finite < 1.0)
        0x7101fe3f,                       # cmp w17,#0x7f
        _bcond(at(9), at(16), 0x2),       # b.hs done              1.0 or more, negative or NaN: the game's own
        0xb9429210,                       # ldr w16,[x16,#0x290]   last scaled value stored to [x20,#0xe5c]
        _ldr_w17_x20(scale),              # ldr w17,[x20,#0xe5c]   current view scale
        0x6b11021f,                       # cmp w16,w17
        _bcond(at(13), at(16), 0x0),      # b.eq done              already applied and intact
        0x7100051f,                       # cmp w8,#1
        0x1a9f0108,                       # csel w8,w8,wzr,eq      keep status 1; any other status -> 0 (recompute sizes)
        0xf9400bf0,                       # done: ldr x16,[sp,#16]
        0xd51b4210,                       # msr nzcv,x16
        0xa8c247f0)                       # ldp x16,x17,[sp],#32


def render_scale_store_body(state_adrp, scale=0xe5c):   # 16 words; state_adrp is word 2 (ADRP x16, state) or MOV x16, x1 in unit tests
    at = lambda k: 4 * k
    return (
        0xadbf7ffe,                       # stp q30,q31,[sp,#-32]!
        0xa9bf47f0,                       # stp x16,x17,[sp,#-16]!
        state_adrp,                       # x16 = state page
        0xd53b4211,                       # mrs x17,nzcv
        0xbd40121e,                       # ldr s30,[x16,#0x10]    the host scale
        0x1e2023c8,                       # fcmp s30,#0.0
        _bcond(at(6), at(12), 0xd),       # b.le original          unset, negative or NaN
        0x1e2e101f,                       # fmov s31,#1.0
        0x1e3f23c0,                       # fcmp s30,s31
        _bcond(at(9), at(12), 0xa),       # b.ge original          1.0 or more
        0x1e3e0800,                       # fmul s0,s0,s30         the game's scale x the host's
        0xbd029200,                       # str s0,[x16,#0x290]    applied value (checked by the status stub)
        _str_s0_x20(scale),               # original: str s0,[x20,#0xe5c]
        0xd51b4211,                       # msr nzcv,x17
        0xa8c147f0,                       # ldp x16,x17,[sp],#16
        0xacc17ffe)                       # ldp q30,q31,[sp],#32


def render_scale(ctx, view=(0xce4, 0xd48, 0xe5c)):   # view: the status ubfx's and the scale store's offsets in the function, the scale's field (v0: 0xcd4, 0xd38, 0xe54)
    here, emit = ctx.here, ctx.emit
    fn = ctx.others.get('render scale')
    if fn is None: return   # not found in this build (a red line from the locator): the game's own sizes
    status, store, scale = fn + view[0], fn + view[1], view[2]
    ctx.site(status, '089c60d3')   # ubfx x8,x0,#32,#8: the update's status byte
    s = here()
    for w in render_scale_status_body(ADRP(16, s + 4 * 4, ctx.state), scale): emit(w)
    emit(B(here(), status + 4))
    ctx.put(status, [B(status, s)], 'dynamic-resolution status: a host render scale not yet stored asks for new 3D sizes (state +0x10)')
    ctx.site(store, _str_s0_x20(scale).to_bytes(4, 'little').hex())    # str s0,[x20,#0xe5c]
    s = here()
    for w in render_scale_store_body(ADRP(16, s + 4 * 2, ctx.state), scale): emit(w)
    emit(B(here(), store + 4))
    ctx.put(store, [B(store, s)], 'final 3D view scale x the host render scale below 1.0 (1.0 = the game\'s own)')
    ctx.capabilities.add('RENDER_SCALE')   # the loader writes DM_RENDER_SCALE (0.5 to 1.0) to state +0x10; the launcher offers it (settings.tsv, fixups:RENDER_SCALE)

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']},   # main+0x805aa0
                'render scale': {'size': 0xfc8, 'fingerprints': ['2c1804f08798ceee012ae30a1ba6b177a6b6a61ce732185bad28e9989d05e165']}}},   # main+0x31d3200, the 3D view size update
    {'name': 'frame ratio from the host\'s step', 'size': 0x9a8, 'fingerprints': ['777177e79e876b03bb01e602619c9fbde6b80741d9a256fca0dddb3cabb760d8'], 'stub': frame_step_v0,   # v0 main+0x6b5900
     'others': {'dynamic resolution': {'size': 0x2a8, 'fingerprints': ['92b72f56e192953782067be61941ecccc148cdc86b5775cc568c3f999877d478']},   # v0 main+0x7e8920
                'dynamic resolution update': {'size': 0xb30, 'fingerprints': ['07ef7eb8ba36414cd381174d378c640288e3b82b81fe58ca230f5889556f933b']},   # main+0x7e7de0
                'render scale': {'size': 0x1000, 'fingerprints': ['6b1bd58d4c7f951832da5516b9051ae5e299e11e17c19e376167f86bbc941bb9']}}},   # main+0x317be20
]}

# Movie pictures a frame (docs/reference/movies.md#a-player-limited-to-one-picture-a-frame-falls-behind-its-sound). The player's update (main+0x3b5ca60 on v131072)
# adds each frame's time to a count of pictures due (+0x8) and advances that many, at most the player's +0x20 (csel at main+0x3b5cb2c). The constructor (main+0x3b5cec0)
# sets +0x20 to 1, or to 2 when its last argument is 2 or more (cmp w27, #2; cinc w8, w28, ge at +0x1c4, w28 = 1), and the queue's size at +0x50 to twice that; the game's
# one caller passes 1. So a frame of two vsyncs showed one picture where the movie had two, and the picture fell behind the movie's sound for good. The cut gives the
# constructor's own value for 2 (+0x20 = 2, +0x50 = 4): a movie that fell behind catches up at two pictures a frame.
movie_pictures = 'movie pictures a frame: up to 2, the constructor\'s value for an argument of 2 or more, whatever its caller passes (1)'
# The update's cap, 3: the frame step's largest hold. The limit itself cannot be 3, as it also sizes the decoder's pipeline: a picture pool of twice it (registered from a
# four-slot array at the player's +0x168) and as many inputs in flight (two slots at +0x188 and +0x190). The update reads the limit only for its cap (ldr w9, [x0, #0x20]
# at +0xc0, before the csel), so the cut puts 3 there and leaves the limit, the pool and the inputs at 2. The advance stops when its queue is empty, so a frame takes a
# third picture only when the decoder has one.
movie_cap = 'movie update cap: 3 pictures a frame, the frame step\'s largest hold, in place of the limit (2), which also sizes the decoder pipeline'
# The movie's frame (docs/reference/movies.md#a-movies-frame-counts-frames-drawn-not-pictures). The event that plays a movie (main+0x3b3e9a0 on v131072) moves its
# timeline (subtitles, the narrator's voice, rumble, the title logo) up to the movie's frame (vtable +0x78: the movie object's int at +0x2f8). The movie's draws
# (main+0x3b5c710, and main+0x3b5c870 for its first draw) add 1 to it each frame a picture is on screen, so it counts frames drawn, not pictures. The cut sets it to
# the pictures the player has taken (the player's +0xc, reset when a movie starts, as +0x2f8 is). The player is the object at +0x10, which both draws have just read.
movie_frame = 'movie frame: the pictures the player has taken (its +0xc), not 1 more for each frame drawn'
MATCH_CUTS = {'main': [
    {'name': movie_pictures, 'size': 828, 'fingerprints': ['f61ad468b6b4e1d574fe8678f5a12dfc60e1aab71be7375142a4b48b0d73ce19'],   # v131072 main+0x3b5cec0
     'cuts': [(452, '88b79c1a', '48008052', movie_pictures + ' (cinc w8, w28, ge → mov w8, #2; as v131072 main+0x3b5d084)')]},
    {'name': movie_cap, 'size': 368, 'fingerprints': ['6a29b27559214b3b89cc31a5b85f14bda4a55685f1ae4fe486d53eda66855867'],   # v131072 main+0x3b5ca60, the update
     'cuts': [(192, '092040b9', '69008052', movie_cap + ' (ldr w9, [x0, #0x20] → mov w9, #3; as v131072 main+0x3b5cb20)')]},
    {'name': movie_frame, 'functions': [
        {'size': 344, 'fingerprints': ['cc1ca04feb1ca4e553a6c7a4f36f99b13f56977185eea8ef5cd8dd3dbbdcdbcd',   # v131072 main+0x3b5c710, the draw
                                       '1037a5ea5e8b3d8ba145dfb46364b094d9e9af2e1ae446ae4658937752ada8a6'],  # v0 main+0x3aff0c0: the same body, callees of its draw setup changed
         'cuts': [(180, '88fa42b908050011', '880a40f9080d40b9', movie_frame + ' (ldr w8, [x20, #0x2f8]; add w8, w8, #1 → ldr x8, [x20, #0x10]; ldr w8, [x8, #0xc]; as v131072 main+0x3b5c7c4)')]},
        {'size': 196, 'fingerprints': ['8fe123ca6fcd016cd9c718e5e66035c807b27f63408861235765e47b4ec34da3',   # v131072 main+0x3b5c870, the first draw
                                       '7189ba8d60a684a4bd8b929dbeeec5afd8004e766ca098f89de680614dd7c5c3'],  # v0 main+0x3aff220
         'cuts': [(152, '88fa42b908050011', '880a40f9080d40b9', movie_frame + ' (the same; as v131072 main+0x3b5c908)')]},
    ]},
]}

BINDING = [
    {'what': 'movie pictures a frame: up to 2', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie player constructor, found by its function's fingerprint on any build that keeps the function; no other game has it: the fingerprint matches no function in the mains of Donkey Kong Bananza, Splatoon Raiders, Pokopia, Mario Tennis Fever, Switch Sports, Yoshi and the Mysterious Book, Mario Golf, Xenoblade Chronicles 2 or Super Mario 3D All-Stars, and Breath of the Wild's and Tears of the Kingdom's movies go through subsdk0's movie::Decoder, not a player in main",
     'checked': 'v131072 main: one function with this fingerprint, the constructor, its one caller, the update and the advance read by disassembly; the limit and the count read in play with DM_PROBE, the opening movie timed at a 30 fps frame step without and with the cut (2026-11-12)'},
    {'what': 'movie update cap: 3 pictures a frame', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie player update, found by its function's fingerprint on any build that keeps the function; it goes with the player's two-picture cut and has no wider binding",
     'checked': 'v131072 main: one function with this fingerprint, the cap\'s only read of the limit, the pool and input slots the limit sizes and the advance read by disassembly; the opening with frames forced to three vsyncs at cap 2, limit 3 with pool 4 (decode error) and cap 3 (2026-11-16)'},
    {'what': 'movie frame: the pictures the player has taken', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own movie draw functions, found by their functions' fingerprints on any build that keeps them; it is the counter the same game's player keeps, so it goes with the player's two-picture cut and has no wider binding",
     'checked': 'v131072 main: one function with each fingerprint, each with one caller, the event update that reads the frame (vtable +0x78), the timeline and sound syncs and the player\'s +0xc read by disassembly; the opening timed with frames forced to two vsyncs (2026-11-16); v0 main: both draws byte-identical after normalization (other callees), the player, its update and the event that reads the frame found exact by nso_fnmap (2026-11-24)'},
    {'what': 'frame ratio from the host\'s step, or from the measured frame time when that is longer', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08); the stub executed with and without a measured time (test_oot_real_speed.py, 2026-11-10); v0 main: one function with its fingerprint (main+0x6b5900), the same code with other registers for its globals, the division at +0x108, s0 dead after it and no x16 / x17, by disassembly (2026-11-24)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09); the threshold at 60 and 30 fps, docked and handheld, run natively in test_ocarina_drs.py (2026-11-23), in play at 30 fps on an M2 Air (upstream PR #1954, 2026-11-16); v0 main: the level function (main+0x7e8920) and the update (main+0x7e7de0) the same code over the same controller fields, only the addresses of their constants moved, by disassembly (2026-11-24)'},
    {'what': 'render scale: the 3D views at a fraction of the game\'s own size (DM_RENDER_SCALE)', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own view-size update (its test of the dynamic-resolution status and its scale store), found by its function's fingerprint on any build that keeps it; the level function's stub above holds level 0 under a scale",
     'checked': 'v131072 main: one function with this fingerprint (main+0x31d3200, 0xfc8 bytes, one caller main+0x31c00cc); the status ubfx at +0xce4, the store at +0xd48 (the only float store to #0xe5c in main; s0 computed from the update\'s w0 at +0xd34, never reread from +0xe5c) and the size loop clearing bit 3 of +0xe2c read by disassembly (2026-11-09, 2026-11-23); guard, status and store stubs executed natively on ARM64 in test_ocarina_drs.py; v0 main: main+0x317be20 (0x1000 bytes), the status ubfx at +0xcd4, the store at +0xd38 to the scale at +0xe54 (its flags at +0xe28), s0 reloaded from +0xe54 before any read, by disassembly (2026-11-24)'},
]
