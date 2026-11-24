# The Legend of Zelda: Ocarina of Time (0400486024DEC000): the rewriter's patches of this game's own code. dm/tools/patch_nso.py loads every profile's patches.py;
# MATCH_FRAME_STEP as described there ("Registered patches"). Patches of Nintendo SDK module builds are in dm/tools/sdk_patches.py
from nso_asm import ADRP, B

# The game's frame ratio. Once a frame the function at main+0x6e6be0 (v131072) divides its base rate (60.0, or 30.0 while bit 0 of [x21+0x24f] is set) by the int frame rate at
# [x21+0x2c8] (fdiv s8,s1,s0 at +0xe4) and hands the result to the time object (main+0x3b4e300: ratio at +0x10, 1 / ratio at +0x14, round(60 / ratio) at +0x1c), which the game
# steps by; the frame's own time never enters it, so a frame that takes two vsyncs at rate 60 plays at half speed. The stub gives the ratio the host's step instead: step × base / 60
# (the division's value when the host keeps frames 60 / rate vsyncs), and counts the vsyncs stepped. Until the host has decided a step, and at fps_target 0, the division runs.
# Below the target the frames are not whole vsyncs long (55-59.7 fps in the heavy areas of a GPU-bound Mac), so the stub then takes the larger of that ratio and base x the measured
# frame time (state +0x38, float seconds, 0 = at the target; hle_nvn.c fstep_tick, which pays back no whole vsync beside it). It marks state +0x3c so the host measures only for a
# main that reads the value (found by gunvalk, zortos293/Lichii PR #216; DM_FRAME_DELTA=0 leaves +0x38 at 0).
# s0 and x16 / x17 are free there (s0 is written again on every path before it is read; the function uses neither x16 nor x17); s1 keeps the base rate.
# Lichii (a GPL emulator; only the fact was taken) found the division (its notes: main+0x6E6CC4, 60 / [x21+0x2C8])
def frame_step(ctx):
    here, emit, fs_site, fs_put, FS_STATE_OFF, base = ctx.here, ctx.emit, ctx.site, ctx.put, ctx.state, ctx.base
    site = base + 0xe4
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
    emit(ADRP(16, here(), FS_STATE_OFF))   # the page again: x17, a scratch register, need not survive the store above (seedds/nxnative PR #1543: a fault at x17 + 0x3c with x17 = 0)
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
    render_scale(ctx)

# The game's dynamic resolution (game__gfx__DynamicResolutionParam: LoadThreshold, ReduceCounterMax, IncreaseCounterMax and a list of levels, each a ResolutionReduceScale and a
# Priority; on v131072 levels 0..9 at 0.75..0.333 of 2048x1152, 1536x864 to 683x384). Its update (main+0x805aa0) reads the frame's GPU load (about 0.2 ms a unit) into +0x8, subtracts
# its small model of the fixed costs and divides the rest by the square of the current scale: it takes the rest of the frame's cost to grow with the pixel count, as on the Switch 2
# its constants were tuned for. The level function (main+0x8065e0) adds that back times each level's scale squared and returns the best level under the threshold (+0x10, 83 = about
# 16.4 ms). On Apple GPUs this game's cost hardly depends on the resolution (mesh, compute and the upscaler's fixed 2048x1152 dominate): 683x384 cost 14.3 ms and 1536x864 14.7 ms on
# an M4 Pro, yet at 683x384 the extrapolation put 1536x864 at about 300 units against the 83 it allows, so the controller sat at its floor for good (game note, Dynamic resolution).
# The stub replaces the level function. With DM_DYNAMIC_RESOLUTION 0 (the default; state +0x40 0) it returns 0: full resolution. With 1 it climbs on what was measured at each
# level, with no model of how the cost grows: one level down when the load (+0x8) has been over the threshold for 8 calls in a row, and that level is marked as not fitting; one level
# up when the load fits and the level above was never measured, fitted when it was last measured, or was measured more than 600 calls (about 10 s at 60 fps, 20 s at 30) ago; else the current level
# (+0x174). Nothing is decided or recorded until a level has held 8 calls: the load is read a few frames late, so before that it may be the previous level's. The 8 calls over in a
# row keep a single slow frame, or the first frames at a new size (its render targets), from marking a level that fits (on the full library at level 1: one burst to 90 against loads
# of 77-80 at levels 2 and 1). The game's counters still pace the steps (down after ReduceCounterMax frames, up after IncreaseCounterMax); its level count (+0x1a0) is the floor.
# The memory is in the state page: +0x200 calls, +0x204 the last call's level, +0x208 calls at it, +0x20c calls over in a row, +0x210 the load measured at each level (16 floats),
# +0x250 the call it was measured at (0 = never). Run against a model of the update (a level moves after the stub asked 5 times up or 2 times down; the load two frames late): a load
# of 70 at every level climbs from the floor to full size in 9 steps, as does Ocarina's measured 72-80 with a spike every 37 frames and a burst after each change; a load that grows
# with the pixel count settles at the highest level that fits and tries the one above once per 600 calls; a scene that gets lighter climbs again, a heavier one steps down.
# The threshold is the game's at 60 fps, scaled by 60 / the host's target fps (state +0: 83 -> 166 at 30, the GPU time a frame of that rate has), and at most one frame of the target
# rate: 5000 / target fps units (an integer division: 83 at 60 fps, 166 at 30); at target 0 (the game's own frame step) the game's alone. Without the scaling a player's 30 fps cap kept
# the docked 83 and the controller sat at its 854x480 floor wherever the full size took over 16.4 ms but fitted 30 fps (upstream nxnative c12000da, PR #1954; an M2 Air in handheld
# mode at 30 fps: 100% of frames at 1440x810, load 134 under 166). The update makes the game's from LoadThreshold (+0xc, or its override at +0x14 when set) plus an offset for the mode in its argument (+0x44: -3, -12 or -16):
# 83 docked, one 60 Hz frame, and 105 in handheld mode (read in play), 21 ms, which a screen with a variable refresh rate can show as it comes. The host keeps every frame
# for whole vsyncs (nvn_fstep.h), so a level that fits 105 and not 83 holds 30 fps: on a 16 GB M2 Pro the handheld game stayed at the full 1440x810 with loads of 90-104 and
# 31 fps, and with the cap it settles at levels 2 to 4 with loads of 77-82 and holds 60 (game note, Dynamic resolution).
# The function is a leaf: x2..x11, x16, x17 and s0..s2 are free, w0 its only result (the original also stores its predicted cost at +0x18, which only it and the update's own
# decision read). The update's fingerprint is required too: +0x8 and +0x174 are its fields
drs_controller = (   # drs_stub.s, assembled with clang; x0 the controller, x16 the state page (the ADRP before it)
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
# Under a host render scale below 1.0 (state +0x10, float bits in 1..0x3f7fffff), return level 0 (the game's full size, which render_scale then multiplies) so
# DM_DYNAMIC_RESOLUTION=1 does not fight DM_RENDER_SCALE. Self-contained (no jump into drs_controller), emitted only when render_scale also matches
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

# Render scale (DM_RENDER_SCALE, state +0x10, dm/host/loader.c): the 3D views at that fraction of the game's own size, the dynamic resolution held at its top level (as Tears of the
# Kingdom's and Breath of the Wild's). The views' sizes come from the scale the engine stores at +0xd48 of its view-size update (str s0,[x20,#0xe5c]; v131072 main+0x31d3200, found by
# its fingerprint; the only float store to #0xe5c in main): the level's ResolutionReduceScale (0.75 at the top level) normalized by [x20+0xe58]/[x20+0xe50]. That store and the size
# loop after it (+0xdd8, which clears the "scale changed" bit 3 of [x20+0xe2c]) run only in a frame where the dynamic-resolution update (main+0x805aa0, which always returns the
# level's float scale in w0) answers status 0, a new scale: at a steady frame rate it answers status 2, so a stub at the store alone changes nothing until the game comes under load.
# Two stubs: at the update's answer (+0xce4, ubfx of its status byte) a host scale in (0.0, 1.0) whose last stored value (state +0x290, private area after drs_controller) does not
# match [x20+0xe5c] makes any non-1 status 0 so that frame recomputes the sizes (and re-applies if [x20+0xe5c] is ever reset); at the store (+0xd48) the freshly computed s0 is
# multiplied by the host's scale and recorded at state +0x290. The level stub above returns level 0 under such a scale, so the size is fixed; 1.0 (the loader's default) runs the
# game's own code. Registers and flags are preserved except x8 (the status read next) and s0 (stored, dead after +0xd48: reloaded at +0xd84)
def _bcond(frm, to, cond): return 0x54000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | cond
def _cbz_w(frm, to, rt): return 0x34000000 | (((to - frm) >> 2) & 0x7ffff) << 5 | rt


def render_scale_status_body(state_adrp):   # 19 words; state_adrp is word 4 (ADRP x16, state) or MOV x16, x1 in unit tests
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
        0xb94e5e91,                       # ldr w17,[x20,#0xe5c]   current view scale
        0x6b11021f,                       # cmp w16,w17
        _bcond(at(13), at(16), 0x0),      # b.eq done              already applied and intact
        0x7100051f,                       # cmp w8,#1
        0x1a9f0108,                       # csel w8,w8,wzr,eq      keep status 1; any other status -> 0 (recompute sizes)
        0xf9400bf0,                       # done: ldr x16,[sp,#16]
        0xd51b4210,                       # msr nzcv,x16
        0xa8c247f0)                       # ldp x16,x17,[sp],#32


def render_scale_store_body(state_adrp):   # 16 words; state_adrp is word 2 (ADRP x16, state) or MOV x16, x1 in unit tests
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
        0xbd0e5e80,                       # original: str s0,[x20,#0xe5c]
        0xd51b4211,                       # msr nzcv,x17
        0xa8c147f0,                       # ldp x16,x17,[sp],#16
        0xacc17ffe)                       # ldp q30,q31,[sp],#32


def render_scale(ctx):
    here, emit = ctx.here, ctx.emit
    fn = ctx.others.get('render scale')
    if fn is None: return   # not found in this build (a red line from the locator): the game's own sizes
    status, store = fn + 0xce4, fn + 0xd48
    ctx.site(status, '089c60d3')   # ubfx x8,x0,#32,#8: the update's status byte
    s = here()
    for w in render_scale_status_body(ADRP(16, s + 4 * 4, ctx.state)): emit(w)
    emit(B(here(), status + 4))
    ctx.put(status, [B(status, s)], 'dynamic-resolution status: a host render scale not yet stored asks for new 3D sizes (state +0x10)')
    ctx.site(store, '805e0ebd')    # str s0,[x20,#0xe5c]
    s = here()
    for w in render_scale_store_body(ADRP(16, s + 4 * 2, ctx.state)): emit(w)
    emit(B(here(), store + 4))
    ctx.put(store, [B(store, s)], 'final 3D view scale x the host render scale below 1.0 (1.0 = the game\'s own)')
    ctx.capabilities.add('RENDER_SCALE')   # the loader writes DM_RENDER_SCALE (0.5 to 1.0) to state +0x10; the launcher offers it (settings.tsv, fixups:RENDER_SCALE)

MATCH_FRAME_STEP = {'main': [
    {'name': 'frame ratio from the host\'s step', 'size': 0x9b4, 'fingerprints': ['6ae2aff5344d14568e4dbaf93455e2d8278384bf892ed6c6eca87fef98fbcd1d'], 'stub': frame_step,   # v131072 main+0x6e6be0
     'others': {'dynamic resolution': {'size': 0x294, 'fingerprints': ['7af6ceb0aedf8e0c17dc08ce17fcd6a7cba6d394a0d09e3b504c6d668dc418e7']},   # v131072 main+0x8065e0
                'dynamic resolution update': {'size': 0xb24, 'fingerprints': ['5a423697838976baf286cffa5e2ceef0d4e037abbcd9d371e90e9e702d61b3d4']},   # main+0x805aa0
                'render scale': {'size': 0xfc8, 'fingerprints': ['2c1804f08798ceee012ae30a1ba6b177a6b6a61ce732185bad28e9989d05e165']}}},   # main+0x31d3200, the 3D view size update
]}

# The movie player's pictures per frame. Once a frame the player's update (main+0x3b5ca60 on v131072) adds the frame's time (the time object's +0x18, the frame ratio / 60 s, times
# 60) to a count at +0x8, takes its whole pictures (the movie's rate at the player's +0x324; the title's and the opening's are 60 fps) and advances that many (main+0x3b61ea0 pops them from its
# queue and shows the last), but never more than the player's +0x20 (csel at main+0x3b5cb2c), and the count keeps what was left. The constructor (main+0x3b5cec0) sets +0x20 to
# 1, or to 2 when its last argument is 2 or more (cmp w27, #2; cinc w8, w28, ge at +0x1c4, w28 = 1), and the queue's size at +0x50 to twice that; the game's one caller passes 1
# (main+0x3b5c55c). So a frame of two vsyncs shows one picture where the movie had two, the picture falls behind for good (the count only grows), and the movie's sound, a stream of
# the sound engine on real time, runs ahead and ends before the picture: at a 30 fps frame step a movie played at exactly 0.500 of its speed (58.00 s of movie in 115.95 s) with
# the count at 1,350 pictures after 45 s. The console holds one vsync a frame in its movies, so the limit never shows there; here the frame step hands the game steps of 2 and 3.
# The cut gives the constructor's own value for 2 (+0x20 = 2, +0x50 = 4), so a frame of two vsyncs shows every second picture and a movie that fell behind catches up at two
# pictures a frame: at the 30 fps frame step 1.000 (95.00 s in 94.97 s).
movie_pictures = 'movie pictures a frame: up to 2, the constructor\'s value for an argument of 2 or more, whatever its caller passes (1)'
# The update's cap, 3. The constructor's limit also sizes the decoder's pipeline: the picture pool is twice it (video-frame-count, registered from a four-slot array at the player's
# +0x168, which the teardown frees slot by slot) and the inputs in flight are as many as it (their buffers two slots at +0x188 and +0x190: main+0x3b5ff30 takes slot 0 for an id of
# 2 or more). So 3 cannot be the limit: with the game's pool of 6 the registration wrote over the two input buffers (the game stopped at the movie's start with a copy to a null
# pointer in sdk), and with the pool kept at 4 the third input reused slot 0 while in flight (a VideoToolbox decode error after 150 pictures of the title movie). The frame step's
# largest hold is 3 vsyncs, and with a cap of 2 each such frame left the picture one behind for as long as the step stayed at 2. The update reads the limit only for its cap
# (ldr w9, [x0, #0x20] at +0xc0, before the csel); the cut puts 3 there and leaves the limit, the pool and the inputs at the constructor's 2. The advance pops up to that many from
# its own queue and stops when the queue is empty, so a frame takes a third picture only when the decoder has one.
movie_cap = 'movie update cap: 3 pictures a frame, the frame step\'s largest hold, in place of the limit (2), which also sizes the decoder pipeline'
# The movie's frame, which its event follows. The event that plays a movie (its update main+0x3b3e9a0 on v131072) reads the movie's frame (its vtable's +0x78, main+0x3b3ec10: the
# movie object's int at +0x2f8) and moves its timeline up to it when the timeline is behind (main+0x122d520), then measures the movie's sound stream against it (main+0x3b3ea50).
# The opening's timeline carries its subtitles, the narrator's voice lines (EventFlowMsg/Dm_Kokiri_0000, played with them), its rumble and the title logo
# (EventOpenOpeningTitle). The movie's draw (main+0x3b5c710, and main+0x3b5c870 for its first draw) adds 1 to that frame each frame a picture is on screen (ldr, add #1,
# str at +0xb4 and +0x98), so the frame counts frames drawn, not pictures shown: the same while the player takes one picture a frame, half of them when it takes two. With the
# cut above, a frame of two vsyncs showed the picture in real time but the subtitles and voice at half speed, and the logo, due after the movie's end, never showed. The cut
# sets the frame to the pictures the player has taken (the player's +0xc: its update adds what it advances each frame; reset when a movie starts, as +0x2f8 is), so the
# timeline follows the picture whatever the frame takes. The player is the object at +0x10, already read by both draws (main+0x3b5ce80 on it) just before.
movie_frame = 'movie frame: the pictures the player has taken (its +0xc), not 1 more for each frame drawn'
MATCH_CUTS = {'main': [
    {'name': movie_pictures, 'size': 828, 'fingerprints': ['f61ad468b6b4e1d574fe8678f5a12dfc60e1aab71be7375142a4b48b0d73ce19'],   # v131072 main+0x3b5cec0
     'cuts': [(452, '88b79c1a', '48008052', movie_pictures + ' (cinc w8, w28, ge → mov w8, #2; as v131072 main+0x3b5d084)')]},
    {'name': movie_cap, 'size': 368, 'fingerprints': ['6a29b27559214b3b89cc31a5b85f14bda4a55685f1ae4fe486d53eda66855867'],   # v131072 main+0x3b5ca60, the update
     'cuts': [(192, '092040b9', '69008052', movie_cap + ' (ldr w9, [x0, #0x20] → mov w9, #3; as v131072 main+0x3b5cb20)')]},
    {'name': movie_frame, 'functions': [
        {'size': 344, 'fingerprints': ['cc1ca04feb1ca4e553a6c7a4f36f99b13f56977185eea8ef5cd8dd3dbbdcdbcd'],   # v131072 main+0x3b5c710, the draw
         'cuts': [(180, '88fa42b908050011', '880a40f9080d40b9', movie_frame + ' (ldr w8, [x20, #0x2f8]; add w8, w8, #1 → ldr x8, [x20, #0x10]; ldr w8, [x8, #0xc]; as v131072 main+0x3b5c7c4)')]},
        {'size': 196, 'fingerprints': ['8fe123ca6fcd016cd9c718e5e66035c807b27f63408861235765e47b4ec34da3'],   # v131072 main+0x3b5c870, the first draw
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
     'checked': 'v131072 main: one function with each fingerprint, each with one caller, the event update that reads the frame (vtable +0x78), the timeline and sound syncs and the player\'s +0xc read by disassembly; the opening timed with frames forced to two vsyncs (2026-11-16)'},
    {'what': 'frame ratio from the host\'s step, or from the measured frame time when that is longer', 'now': 'game', 'goal': 'game', 'purpose': 'frame rate',
     'why': "the game's own frame-ratio division, found by its function's fingerprint on any build that keeps the function",
     'checked': 'v131072 main: one function with this fingerprint, the site and its free registers read by disassembly (2026-11-08); the stub executed with and without a measured time (test_oot_real_speed.py, 2026-11-10)'},
    {'what': 'dynamic resolution: full size, or a climb on the load measured at each level', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own dynamic-resolution level function and update, found by their functions' fingerprints on any build that keeps both; switchable (DM_DYNAMIC_RESOLUTION)",
     'checked': 'v131072 main: one function with each fingerprint (the level function a leaf, x2..x11 / x16 / x17 / s0..s2 free), its callers, the update and the controller fields read by disassembly, the fields read in play (2026-11-08); the climb run natively against a model of the update and in play to full size (2026-11-09); the threshold at 60 and 30 fps, docked and handheld, run natively in test_ocarina_drs.py (2026-11-23), in play at 30 fps on an M2 Air (upstream PR #1954, 2026-11-16)'},
    {'what': 'render scale: the 3D views at a fraction of the game\'s own size (DM_RENDER_SCALE)', 'now': 'game', 'goal': 'game', 'purpose': 'rendering',
     'why': "the game's own view-size update (its test of the dynamic-resolution status and its scale store), found by its function's fingerprint on any build that keeps it; the level function's stub above holds level 0 under a scale",
     'checked': 'v131072 main: one function with this fingerprint (main+0x31d3200, 0xfc8 bytes, one caller main+0x31c00cc); the status ubfx at +0xce4, the store at +0xd48 (the only float store to #0xe5c in main; s0 computed from the update\'s w0 at +0xd34, never reread from +0xe5c) and the size loop clearing bit 3 of +0xe2c read by disassembly (2026-11-09, 2026-11-23); guard, status and store stubs executed natively on ARM64 in test_ocarina_drs.py'},
]
