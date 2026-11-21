// nn::hid (controller): on the real device shared memory is read through the hid service; the host takes the current state written by the front end (Mac keyboard / controller, iOS virtual gamepad / controller).
// Generic, game-independent version: player 1 is always connected, as the handheld controller (Npad id 0x20) with DM_OPMODE=0 (handheld), otherwise as the 1P full-key controller (id 0);
// players 2..8 are full-key controllers on No2..No8 (ids 1..7), in either mode. A controller on No1..No8 is shown as a dual Joy-Con pair instead when the game supports JoyDual but not
// full-key (pad_style); the Handheld npad stays Handheld. Players 2..8 are on No2..No8 while the front end reports a controller for them (dm_pad_players; the Mac gives each physical controller a player
// of its own, front_mac.m). A player whose npad the game does not support (SetSupportedNpadIdType) is not reported: in a one-player game it feeds player 1, otherwise it is not used.
// Six-axis sensor: one per connected controller (two for handheld), at rest lying flat, player 1's shaken side to side while the front end holds DM_BTN_SHAKE (see the six-axis section below).
// Vibration follows the player assignments through hle_haptics.c. Mouse and keyboard: not connected; the controller applet reports the connected players with player 1 selected, and leaves out those past the game's maximum.
#include "hle.h"
#include "hle_haptics.h"
#include "dm_input.h"
#include "front.h"
#include <stdatomic.h>
#include <unistd.h>
#include <strings.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include "motion_fusion.h"

enum { NPAD_NO1 = 0, NPAD_HANDHELD = 0x20 };
enum { STYLE_FULLKEY = 1u << 0, STYLE_HANDHELD = 1u << 1, STYLE_JOYDUAL = 1u << 2, STYLE_JOYLEFT = 1u << 3, STYLE_JOYRIGHT = 1u << 4 };
// The Joy-Con 2 forms of the Joy-Con styles (Switch 2 SDKs; inferred, not documented: Nintendo Switch 2 Welcome Tour's mouse exhibits support only 0x1000 and test a controller's style
// set with 0x1004 for a pair, 0x2008 for a left and 0x4000 beside JoyRight for a right one). A Joy-Con 2 pad (dm_joy_attach with DM_JOY_2) reports them beside the plain ones, to a game
// that supports them
enum { STYLE_JOYDUAL2 = 1u << 12, STYLE_JOYLEFT2 = 1u << 13, STYLE_JOYRIGHT2 = 1u << 14 };
enum { ATTR_CONNECTED = 1u << 0, ATTR_WIRED = 1u << 1, ATTR_LEFT_CONN = 1u << 2, ATTR_LEFT_WIRED = 1u << 3, ATTR_RIGHT_CONN = 1u << 4, ATTR_RIGHT_WIRED = 1u << 5 };
// Stick direction pseudo-buttons (NpadButton 16..23): derived from the stick values, threshold about half travel
enum { STICK_THRESH = 0x4000 };

// nn::hid::NpadFullKeyState / Handheld / JoyDual / JoyLeft / JoyRight share one layout (0x28)
typedef struct { int64_t sampling; uint64_t buttons; int32_t lx, ly, rx, ry; uint32_t attributes, _pad; } NpadState;
_Static_assert(sizeof(NpadState) == 0x28, "NpadState");

// Front-end publishing: the keyboard path (dm_input_set), the scripted path (script_set: DM_KEYS and DM_CTL) and player 1's physical controller path (dm_pad_set) are ORed;
// each further player has a controller path of its own (dm_pad_set_player). Press edges are latched until the game's next read so short presses are not lost. Scripted presses have
// their own path because a connected controller publishes its whole state on every change (a DualShock 4's sticks change all the time), which overwrote them on the shared one;
// the separate paths also keep scripted input out of DM_REC, which records the physical ones (rec_read)
static _Atomic uint64_t g_btn, g_axes, g_latch, g_sc_btn, g_sc_axes, g_sc_latch, g_pad_btn[DM_PLAYERS], g_pad_axes[DM_PLAYERS], g_pad_latch[DM_PLAYERS];
static _Atomic int64_t g_sampling = 1 << 16;   // starts high so the history entries below the newest keep positive sampling numbers
static uint64_t pack(int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    return (uint64_t)(uint16_t)lx | (uint64_t)(uint16_t)ly << 16 | (uint64_t)(uint16_t)rx << 32 | (uint64_t)(uint16_t)ry << 48;
}
// Shake (DM_BTN_SHAKE on any player 1 path): whole side-to-side cycles on the guest clock, so the controller always comes back to rest. A press starts a shake (or continues one still
// finishing), holding keeps shaking, and a release lets the current cycle finish: a tap of one frame is still one whole shake
#define SHAKE_HZ 6.0
static const uint64_t SHAKE_CYCLE_NS = (uint64_t)(1e9 / SHAKE_HZ);
static _Atomic uint64_t g_shake_t0, g_shake_end;   // the current shake's start and end; end = UINT64_MAX while held
static _Atomic int g_shake_on;
static void shake_note(void) {
    int on = ((atomic_load(&g_btn) | atomic_load(&g_sc_btn) | atomic_load(&g_pad_btn[0])) & DM_BTN_SHAKE) != 0;
    if (atomic_exchange(&g_shake_on, on) == on) return;
    uint64_t t = guest_ns();
    if (on) { if (t >= atomic_load(&g_shake_end)) atomic_store(&g_shake_t0, t); atomic_store(&g_shake_end, UINT64_MAX); }
    else { uint64_t t0 = atomic_load(&g_shake_t0); atomic_store(&g_shake_end, t0 + ((t - t0) / SHAKE_CYCLE_NS + 1) * SHAKE_CYCLE_NS); }
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_btn, buttons); atomic_fetch_or(&g_latch, buttons & ~old); atomic_store(&g_axes, pack(lx, ly, rx, ry));
    shake_note();
}
void dm_pad_set_player(int p, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    if (p < 0 || p >= DM_PLAYERS) return;
    uint64_t old = atomic_exchange(&g_pad_btn[p], buttons); atomic_fetch_or(&g_pad_latch[p], buttons & ~old); atomic_store(&g_pad_axes[p], pack(lx, ly, rx, ry));
    if (!p) shake_note();
}
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) { dm_pad_set_player(0, buttons, lx, ly, rx, ry); }
static void script_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry) {
    uint64_t old = atomic_exchange(&g_sc_btn, buttons); atomic_fetch_or(&g_sc_latch, buttons & ~old); atomic_store(&g_sc_axes, pack(lx, ly, rx, ry));
    shake_note();
}
// DM_KEYS scripted input (for tests): "seconds:key[+key][~hold seconds][*count/interval seconds],...", e.g. "8:A,12:A,15:Down~0.5,16:A+B,40:A*80/2" (A every 2 s from 40 s on, 80 times). Key names A B X Y LS RS L R ZL ZR (plus the sticks LLeft LUp LRight LDown and RLeft RUp RRight RDown) Plus Minus Left Up Right Down, left stick LLeft LUp LRight LDown,
//   L<x>/<y> and R<x>/<y> = a stick tilted part way (each -1 to 1, y up: a cursor that a full tilt sweeps past its target), and Shake = the controller shaken for the hold time (at least one whole shake);
//   held 0.15 s by default, at most KEY_STEPS steps (counted after repeats are expanded). Goes through player 1's scripted path (script_set), ORed with the keyboard and player 1's controller, none overrides another. Times are counted from host start.
//   An f before the time means a presented frame number ("f7600:A", and the repeat interval counts frames too): when the machine is busy seconds drift from the game's progress, frame numbers do not (used for the Zelda BotW opening route)
//   An f before the hold counts presented frames too ("f7600:A~f12"): held until 12 frames after the step's frame, and a step that begins by then follows without a release in between (the buttons
//   go straight to its state): DM_REC writes a run as such steps, one state after the other, and a list of only such steps is applied at the
//   game's reads (keys_sync) instead of by keys_main. Whitespace and newlines separate steps as commas do
typedef struct { double at, hold; uint64_t btn; int32_t lx, ly, rx, ry; int frame, fhold; } key_step;   // fhold: hold in frames
#define KEY_STEPS (1 << 20)
static key_step *g_steps; static int g_nsteps, g_cap_steps;
static int step_add(const key_step *st) {
    if (g_nsteps >= KEY_STEPS) return 0;
    if (g_nsteps == g_cap_steps) { int cap = g_cap_steps ? g_cap_steps * 2 : 64; key_step *n = realloc(g_steps, (size_t)cap * sizeof *n); if (!n) return 0; g_steps = n; g_cap_steps = cap; }
    g_steps[g_nsteps++] = *st; return 1;
}
static void *keys_main(void *arg) {
    // released: the frame the previous step released in (a frame step presses in a later frame, so the game reads the
    // release between two presses even when the replay runs late and the next step is already due: two taps of A stay two presses, not one long one)
    (void)arg; struct timespec t0; clock_gettime(CLOCK_MONOTONIC, &t0); int64_t released = -1;
    for (int i = 0; i < g_nsteps; i++) {
        const key_step *s = &g_steps[i];
        for (;;) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); double el = (t.tv_sec - t0.tv_sec) + (t.tv_nsec - t0.tv_nsec) / 1e9;
                   if (s->frame ? (double)dm_guest_frame() >= s->at && (int64_t)dm_guest_frame() > released : el >= s->at) break; usleep(5000); }
        LOG("DM_KEYS: %s%.2f press %#llx (hold %.2f %s)", s->frame ? "frame " : "", s->at, (unsigned long long)s->btn, s->hold, s->fhold ? "frames" : "s");
        script_set(s->btn, s->lx, s->ly, s->rx, s->ry);
        uint64_t end = 0;
        if (s->fhold) { end = (s->frame ? (uint64_t)s->at : dm_guest_frame()) + (uint64_t)s->hold; while (dm_guest_frame() < end) usleep(5000); }
        else usleep((useconds_t)(s->hold * 1e6));
        // Hand over without a release only to a step that begins where this hold ends (DM_REC's consecutive states), not to one that is merely due by now: a gap in the list is a release
        int next = s->fhold && i + 1 < g_nsteps && g_steps[i + 1].frame && g_steps[i + 1].at <= (double)end;
        if (!next) { script_set(0, 0, 0, 0, 0); released = (int64_t)dm_guest_frame(); }
    }
    return NULL;
}
// A list of frame steps that are all held in frames (what DM_REC writes) is not played by keys_main, whose thread presses within 5 ms after a present, before or after the game's read
// of that frame: player 1's reads (fill) call keys_sync first, which applies on the reading thread every step due in the frame the read is in. Every read
// in frame N then sees the state stamped on frame N, as DM_REC stamps a state with the frame of the reads that saw it (rec_read). A list with a step in seconds goes through keys_main
static _Atomic int g_sync_on; static pthread_mutex_t g_sync_mu = PTHREAD_MUTEX_INITIALIZER;
static int g_sync_next, g_sync_held;   // the next step to press; whether g_steps[g_sync_next - 1] is held
static int keys_framed(void) { for (int i = 0; i < g_nsteps; i++) if (!g_steps[i].frame || !g_steps[i].fhold) return 0; return g_nsteps > 0; }
static void keys_sync(void) {
    if (!atomic_load_explicit(&g_sync_on, memory_order_acquire)) return;
    pthread_mutex_lock(&g_sync_mu);
    uint64_t f = dm_guest_frame();
    for (;;) {
        if (g_sync_held) {
            const key_step *s = &g_steps[g_sync_next - 1]; uint64_t end = (uint64_t)s->at + (uint64_t)s->hold;
            if (end > f) break;
            if (g_sync_next < g_nsteps && g_steps[g_sync_next].at <= (double)end) {   // the next step begins where this hold ends: no release (the buttons go to its state)
                const key_step *n = &g_steps[g_sync_next++];
                script_set(n->btn, n->lx, n->ly, n->rx, n->ry); continue;
            }
            script_set(0, 0, 0, 0, 0);   // let go (a hold of 0 frames is pressed and let go before one read: the press latch hands it to that read)
            g_sync_held = 0; continue;
        }
        if (g_sync_next >= g_nsteps) { atomic_store(&g_sync_on, 0); LOG("DM_KEYS: the %d frame steps were played, the last let go at frame %llu", g_nsteps, (unsigned long long)f); break; }
        const key_step *s = &g_steps[g_sync_next];
        if (s->at > (double)f) break;
        script_set(s->btn, s->lx, s->ly, s->rx, s->ry); g_sync_next++; g_sync_held = 1;
    }
    pthread_mutex_unlock(&g_sync_mu);
}
// The key names of NpadButton bits 0..15 (dm_input.h): DM_KEYS and DM_CTL read them, DM_REC writes them
static const char *const key_names[16] = {"A", "B", "X", "Y", "LS", "RS", "L", "R", "ZL", "ZR", "Plus", "Minus", "Left", "Up", "Right", "Down"};
// The keys of one step, "key[+key]" (the hold and repeat already cut off), into st
static void parse_keys(char *spec, key_step *st) {
    for (char *k = spec; k && *k;) { char *plus = strchr(k, '+'); if (plus) *plus = 0;
        int found = 0; for (int b = 0; b < 16; b++) if (!strcasecmp(k, key_names[b])) { st->btn |= 1ull << b; found = 1; }
        // left stick fully pushed: LLeft LUp LRight LDown (for games that move on the map by stick only, such as Super Mario 3D World)
        static const char *const sticks[4] = {"LLeft", "LUp", "LRight", "LDown"};
        static const int sx[4] = {-32767, 0, 32767, 0}, sy[4] = {0, 32767, 0, -32767};
        for (int d = 0; d < 4; d++) if (!strcasecmp(k, sticks[d])) { if (sx[d]) st->lx = sx[d]; if (sy[d]) st->ly = sy[d]; found = 1; }
        // right stick the same way: RLeft RUp RRight RDown (camera; Super Mario Odyssey's first hint asks for it)
        static const char *const rsticks[4] = {"RLeft", "RUp", "RRight", "RDown"};
        for (int d = 0; d < 4; d++) if (!strcasecmp(k, rsticks[d])) { if (sx[d]) st->rx = sx[d]; if (sy[d]) st->ry = sy[d]; found = 1; }
        // a stick tilted part way: L<x>/<y> or R<x>/<y>, each from -1 to 1 (y up), e.g. "L-0.6/-0.4". OCTOPATH TRAVELER II's traveler selection opens a traveler's card only while
        // its quill cursor rests on them; a fully tilted stick swept the cursor past every traveler (a tilt of 0.3 is inside its dead zone)
        float fx, fy; if ((k[0] == 'L' || k[0] == 'R') && strchr(k, '/') && sscanf(k + 1, "%f/%f", &fx, &fy) == 2 && fabsf(fx) <= 1 && fabsf(fy) <= 1) {
            int32_t vx = (int32_t)lrintf(fx * 32767), vy = (int32_t)lrintf(fy * 32767);
            if (k[0] == 'L') { st->lx = vx; st->ly = vy; } else { st->rx = vx; st->ry = vy; } found = 1; }
        if (!strcasecmp(k, "Shake")) { st->btn |= DM_BTN_SHAKE; found = 1; }   // the six-axis sensor shaken (Super Mario Odyssey's cap throw)
        if (!found) LOG("[RED] DM_KEYS unknown key name: %s", k);
        k = plus ? plus + 1 : NULL; }
}
static int step_cmp(const void *a, const void *b) { double x = ((const key_step *)a)->at, y = ((const key_step *)b)->at; return (x > y) - (x < y); }
// A DM_KEYS list into g_steps, in time order
static void keys_parse(const char *spec) {
    char *buf = strdup(spec), *save = NULL; if (!buf) return;
    int full = 0;
    for (char *tok = strtok_r(buf, ", \t\r\n", &save); tok && !full; tok = strtok_r(NULL, ", \t\r\n", &save)) {
        char *c = strchr(tok, ':'); if (!c) { LOG("[RED] DM_KEYS step lacks 'seconds:': %s", tok); continue; }
        *c = 0; int fr = (tok[0] == 'f' || tok[0] == 'F'); key_step st = {atof(tok + fr), 0.15, 0}; st.frame = fr;
        int rep = 1; double every = 0;
        char *r = strchr(c + 1, '*'); if (r) { *r = 0; rep = atoi(r + 1); char *sl = strchr(r + 1, '/'); every = sl ? atof(sl + 1) : 1; if (rep < 1) rep = 1; }
        char *h = strchr(c + 1, '~'); if (h) { *h = 0; st.fhold = h[1] == 'f' || h[1] == 'F'; st.hold = atof(h + 1 + st.fhold); }
        parse_keys(c + 1, &st);
        for (int i = 0; i < rep && !full; i++) { key_step one = st; one.at = st.at + i * every; if (!step_add(&one)) { LOG("[RED] DM_KEYS: more than %d steps, the rest is dropped", KEY_STEPS); full = 1; } }
    }
    free(buf);
    // Steps run in time order, so repeated groups interleave ("290:A*200/3,291:Left*200/3,291.5:A*200/3" = A, Left, A every 3 s): they used to run in list order, one group after the
    // other, and a group listed after a long repeat waited for all of it. A stable sort keeps the listed order of steps at the same time; times and frame numbers are not comparable, so a list
    // that mixes them keeps its listed order
    int mixed = 0; for (int i = 1; i < g_nsteps; i++) mixed |= g_steps[i].frame != g_steps[0].frame;
    if (!mixed && g_nsteps > 1) mergesort(g_steps, (size_t)g_nsteps, sizeof *g_steps, step_cmp);   // stable (BSD libc), and not quadratic for long interleaved repeats
}
// DM_KEYS=@<file>: the steps are read from the file (a DM_REC recording as it is). An environment value is capped by ARG_MAX (1 MiB for arguments and environment together on macOS,
// about 40,000 recorded steps), and iOS's env.txt takes one line per switch; a relative path is from the working directory (Documents on iOS)
static char *keys_file(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    char *text = NULL; size_t len = 0, cap = 0, n;
    do { if (len + 65536 + 1 > cap) { cap = cap ? cap * 2 : 1 << 20; char *t = realloc(text, cap); if (!t) { free(text); fclose(f); return NULL; } text = t; }
         n = fread(text + len, 1, cap - len - 1, f); len += n; } while (n);
    fclose(f); text[len] = 0; return text;
}
static void ctl_start(void);
static void rec_start(void);
void hid_keys_init(void) {
    ctl_start();
    rec_start();
    const char *e = getenv("DM_KEYS"); if (!e || !*e) return;
    if (e[0] == '@') {
        char *text = keys_file(e + 1); if (!text) { LOG("[RED] DM_KEYS: cannot read %s", e + 1); return; }
        keys_parse(text); free(text);
    } else keys_parse(e);
    if (keys_framed()) { atomic_store(&g_sync_on, 1); LOG("DM_KEYS: %d steps, all in frames: applied at the game's input reads of their frames", g_nsteps); return; }
    pthread_t th; if (g_nsteps && !pthread_create(&th, NULL, keys_main, NULL)) { pthread_detach(th); LOG("DM_KEYS: %d steps", g_nsteps); }
}

// DM_CTL=<file> (Mac, for driving a game step by step from a shell): the host reads the lines appended to the file (polled every 100 ms) and acts on each at once:
//   "key[+key][~hold]" presses as a DM_KEYS step does (hold 0.15 s by default), "shot <path>" saves the next presented frame as a PNG (dm_gfx_shot_at), "passdump <n>" dumps the
//   render passes of the next n frames as DM_NVN_PASSDUMP does (to stderr, the run's log), "texdump <path with %d>" saves the colour targets of the next frame as DM_NVN_TEXDUMP does,
//   "env KEY=VALUE" sets a switch read at the time of use (DM_NVN_SNAPRAW for the next texdump),
//   "peek <address> <count>" logs that many floats of guest memory, "memdump <address> <bytes> <path>" saves guest memory
//   to a file (a buffer texture's storage, which the pass dump's tbuf lines print, holds what the GPU wrote: buffer textures are views of the pool), "texlist <NVN target>" logs the
//   registered textures of that target (2 = 3D) with storage address and block-linear layout (nvn_mtl.m dm_gfx_texlist), "texraw <serial> <path>"
//   saves a host texture's level 0 raw, every slice (a cube's six faces, a 3D texture's depth slices) (nvn_mtl.m dm_gfx_texraw), "addr <hex address>" logs what the host knows of a guest
//   address (nvn_mtl.m dm_gfx_addr_note: its memory pool, a texture's storage, a recorded GPU write, the raw memory table, a sparse pool's page), "dump" logs what each guest thread waits on and the graphics wait points
//   (crash_dump_now, as the iOS app's DM_LOG_PORT dump; scripts/regress.py asks for it before stopping a hung game), "#…" is ignored.
//   Look at a screen, decide, press, look again: menus whose timing moves from run to run (loading) need no blind timed script
static void imu_env_reread(void);   // below, at the IMU state
static void script_player(int p);   // below, at the players
static void *ctl_main(void *arg) {
    const char *path = arg; long off = 0; char line[1100];
    for (;;) {
        FILE *f = fopen(path, "r");
        if (f && !fseek(f, off, SEEK_SET)) {
            while (fgets(line, sizeof line, f)) {
                size_t n = strlen(line); if (!n || line[n - 1] != '\n') break;   // a line still being written: read it next time
                off += (long)n; line[--n] = 0; if (n && line[n - 1] == '\r') line[--n] = 0;
                if (!n || line[0] == '#') continue;
                if (!strncmp(line, "mod-on ", 7) || !strncmp(line, "mod-off ", 8)) {
                    int on = line[5] == 'n'; const char *name = line + (on ? 7 : 8), *why = "no such mod";
                    for (int i = 0; i < dm_mods_count(); i++) if (!strcmp(dm_mods_name(i), name)) { dm_mods_set(i, on); why = NULL; }
                    if (why) LOG("[RED] DM_CTL: %s: %s", line, why);
                    continue;
                }
                if (!strncmp(line, "amiibo ", 7)) { int dm_amiibo_scan(const char *path); dm_amiibo_scan(line + 7); continue; }
                if (!strcmp(line, "amiibo-remove")) { void dm_amiibo_remove(void); dm_amiibo_remove(); continue; }
                if (!strcmp(line, "dump")) { crash_dump_now("DM_CTL dump"); continue; }
                if (!strncmp(line, "atrace ", 7)) { void probe_atrace(uint64_t lo, uint64_t n); char *e2; uint64_t a = strtoull(line + 7, &e2, 16); probe_atrace(a, strtoull(e2, NULL, 0)); continue; }   // probe.c
                if (!strncmp(line, "shot ", 5)) { dm_gfx_shot_at(1, line + 5); LOG("DM_CTL: screenshot of the next frame → %s", line + 5); continue; }
                if (!strncmp(line, "texlist ", 8)) { void dm_gfx_texlist(int target); dm_gfx_texlist(atoi(line + 8)); continue; }
                if (!strcmp(line, "restop")) { void dm_gfx_restop(void); dm_gfx_restop(); continue; }   // what the residency set holds (nvn_mtl.m res_top)
                if (!strncmp(line, "addr ", 5)) { void dm_gfx_addr_note(uint64_t a, char *note, size_t n); char note[600]; uint64_t a = strtoull(line + 5, NULL, 16);   // what the host knows of a guest address
                    dm_gfx_addr_note(a, note, sizeof note); LOG("DM_CTL: addr %#llx:%s", (unsigned long long)a, note[0] ? note : " nothing known"); continue; }
                if (!strncmp(line, "texraw ", 7)) { void dm_gfx_texraw(uint64_t serial, const char *path); char *e2; uint64_t s = strtoull(line + 7, &e2, 10); while (*e2 == ' ') e2++; dm_gfx_texraw(s, e2); continue; }
                if (!strncmp(line, "bufsnap ", 8)) {   // bufsnap <shader key> <hex address> <bytes> <path>: guest memory as the GPU left it right after that shader's next dispatch (nvn_mtl.m dm_gfx_bufsnap)
                    void dm_gfx_bufsnap(const char *key, uint64_t a, size_t n, const char *path); char key[32] = {0}, path[512] = {0}; unsigned long long a = 0, n = 0;
                    if (sscanf(line + 8, "%31s %llx %lli %511s", key, &a, &n, path) == 4) dm_gfx_bufsnap(key, a, (size_t)n, path); else LOG("DM_CTL: bufsnap <shader key> <hex address> <bytes> <path>"); continue; }
                if (!strncmp(line, "peek ", 5)) { char *e2; uint64_t a = strtoull(line + 5, &e2, 16); dm_ctl_peek(a, (int)strtol(e2, NULL, 0)); continue; }
                if (!strncmp(line, "memdump ", 8)) { char *e2, *e3; uint64_t a = strtoull(line + 8, &e2, 16), n = strtoull(e2, &e3, 0);
                                                     while (*e3 == ' ') e3++; dm_ctl_memdump(a, n, e3); continue; }
                if (!strncmp(line, "env ", 4)) { char *eq = strchr(line + 4, '='); if (eq) { *eq = 0; setenv(line + 4, eq + 1, 1); LOG("DM_CTL: env %s=%s", line + 4, eq + 1);
                                                     if (!strncmp(line + 4, "DM_IMU_", 7) || !strcmp(line + 4, "DM_SIX_DELAY")) { imu_env_reread(); } } continue; }   // the IMU's switches are read again
                if (!strncmp(line, "texdump ", 8)) { void dm_gfx_texdump_next(const char *path); dm_gfx_texdump_next(line + 8); LOG("DM_CTL: colour targets of the next frame → %s (as DM_NVN_TEXDUMP)", line + 8); continue; }
                if (!strncmp(line, "passdump ", 9)) { void dm_gfx_passdump_next(long n); dm_gfx_passdump_next(atol(line + 9)); LOG("DM_CTL: passes of the next %ld frames (as DM_NVN_PASSDUMP)", atol(line + 9)); continue; }
                key_step st = {0, 0.15, 0}; char *h = strchr(line, '~'); if (h) { *h = 0; st.hold = atof(h + 1); }
                if (line[0] == 'P' && line[1] >= '2' && line[1] <= '0' + DM_PLAYERS && line[2] == ' ') {   // P<n> <keys>: player n's controller, for games that need a second player (Orbitals)
                    int p = line[1] - '1'; script_player(p);
                    parse_keys(line + 3, &st);
                    LOG("DM_CTL: player %d press %#llx (hold %.2f s)", p + 1, (unsigned long long)st.btn, st.hold);
                    dm_pad_set_player(p, st.btn, st.lx, st.ly, st.rx, st.ry); usleep((useconds_t)(st.hold * 1e6)); dm_pad_set_player(p, 0, 0, 0, 0, 0);
                    continue;
                }
                parse_keys(line, &st);
                LOG("DM_CTL: press %#llx (hold %.2f s)", (unsigned long long)st.btn, st.hold);
                script_set(st.btn, st.lx, st.ly, st.rx, st.ry); usleep((useconds_t)(st.hold * 1e6)); script_set(0, 0, 0, 0, 0);
            }
        }
        if (f) fclose(f);
        usleep(100000);
    }
    return NULL;
}
static void ctl_start(void) {
    const char *e = getenv("DM_CTL"); if (!e || !*e) return;
    pthread_t th; if (!pthread_create(&th, NULL, ctl_main, strdup(e))) { pthread_detach(th); LOG("DM_CTL: reading key presses and screenshot requests from %s", e); }
}

static uint32_t connected_id(void) { return dm_opmode() == 0 ? NPAD_HANDHELD : NPAD_NO1; }
uint32_t hle_hid_connected_npad(void) { return connected_id(); }   // nfp's NFC device sits on player 1's controller (hle_sdk16.c)
// Player n is on No n in handheld mode too, where No1 stays empty: games take Handheld and No1 both for player 1, so a second controller on No1 would act as player 1 again
uint32_t dm_player_npad(int p) { return p <= 0 ? connected_id() : (uint32_t)p; }
// The players with a controller (bit p = player p + 1, from the front end's dm_pad_players; player 1 always) and the npads the game supports (bit id for No1..No8, bit 8
// Handheld, bit 9 Other: all of them until SetSupportedNpadIdType, as the hid service starts)
static _Atomic uint32_t g_players = 1, g_ids_supported = 0x3ff;
// The players the controller applet left out (ctrl_support): those past the game's playerCountMax when it last opened, and whether that maximum was one. On a Switch the
// applet leaves no more controllers connected than the game takes; here such a player is not reported, as one on an npad the game does not support
static _Atomic uint32_t g_benched, g_bench_one;
static uint32_t id_bit(uint32_t id) { return id < DM_PLAYERS ? 1u << id : id == NPAD_HANDHELD ? 1u << 8 : id == 0x10 ? 1u << 9 : 0; }
static int reported(int p) { return p == 0 || (atomic_load(&g_players) >> p & 1 && atomic_load(&g_ids_supported) & id_bit((uint32_t)p) && !(atomic_load(&g_benched) >> p & 1)); }
// Players with a controller that the game does not see: their npad is not supported, or the controller applet left them out. A one-player game (no npad past No1 supported, or
// an applet opened for one player) gets them as player 1, so any controller plays it; in a game for fewer players than are connected (a fifth controller in a four-player game)
// they are not used, as on the Switch, where the system does not connect them
static uint32_t unsupported_players(void) {
    uint32_t m = 0; for (int p = 1; p < DM_PLAYERS; p++) if (atomic_load(&g_players) >> p & 1 && !reported(p)) m |= 1u << p;
    return m;
}
static int one_player(void) { return !(atomic_load(&g_ids_supported) & ((1u << DM_PLAYERS) - 2)); }
static uint32_t merged_players(void) { return one_player() ? unsupported_players() : atomic_load(&g_bench_one) ? atomic_load(&g_benched) & atomic_load(&g_players) : 0; }
static int player_of(uint32_t id) {   // the reported player on npad id; -1 when none
    if (id == connected_id()) return 0;
    return id > 0 && id < DM_PLAYERS && reported((int)id) ? (int)id : -1;
}
// Joy-Cons as devices (dm_joy_attach, dm_joy_set): a pad that holds Joy-Cons is shown to the game in the styles the game supports. Docked (console mode): both Joy-Cons of a pad make one dual npad
// (JoyDual: both buttons and both sticks, two six-axis sensors) unless the pair is split (single assignment: the controller applet splits a pair to reach the game's minimum of players, ctrl_support), when the left Joy-Con stays there
// (JoyLeft) and the right one takes the lowest free supported npad (JoyRight); one Joy-Con alone is JoyLeft or JoyRight. A game that supports none of these styles gets the pad's merged
// state as a full-key controller, as before. In handheld mode player 1 stays as it was (handheld).
enum { KIND_NONE = 0, KIND_PAD, KIND_DUAL, KIND_LEFT, KIND_RIGHT };
typedef struct { int pad, kind; } npad_ref;
static _Atomic uint32_t g_joy_sides[DM_PLAYERS];
static _Atomic uint32_t g_joy_two[DM_PLAYERS];   // the pad's Joy-Cons are Joy-Con 2s (DM_JOY_2)
static _Atomic uint32_t g_joy_paired[DM_PLAYERS];   // the pad has held both Joy-Cons since it last held none (six_stream: a half it lost lies flat)
static _Atomic uint32_t g_joy_reported[DM_PLAYERS];   // bit side: that Joy-Con has reported its own motion since it was attached (joy_own)
static _Atomic uint64_t g_joy_btn[DM_PLAYERS][2], g_joy_latch[DM_PLAYERS][2], g_joy_axes[DM_PLAYERS][2];
static _Atomic uint32_t g_single_mask;               // bit id: the pair on npad No(id + 1) is split, single assignment (joy_assign)
static _Atomic uint32_t g_styles_supported = 0xffffffffu;   // SetSupportedNpadStyleSet; read by the input threads too (npad_resolve, pad_style)
// The styles the game supports for pad p's Joy-Cons: a Joy-Con 2 style counts as its plain one
static uint32_t joy_supported(int p) { uint32_t s = atomic_load(&g_styles_supported); if (p >= 0 && p < DM_PLAYERS && atomic_load(&g_joy_two[p])) s |= (s >> 10 & (STYLE_JOYDUAL | STYLE_JOYLEFT | STYLE_JOYRIGHT)); return s; }
static int joy_active(int p) { return p >= 0 && p < DM_PLAYERS && atomic_load(&g_joy_sides[p]) && !(p == 0 && dm_opmode() == 0); }
static int joy_split(int p) { return atomic_load(&g_joy_sides[p]) == 3 && (atomic_load(&g_single_mask) >> p & 1) && (joy_supported(p) & (STYLE_JOYLEFT | STYLE_JOYRIGHT)) == (STYLE_JOYLEFT | STYLE_JOYRIGHT); }
// The npad the other Joy-Con of pad p takes when the pair is split: the lowest supported npad No2..No8 that has no player of its own and is not another split's; -1 for none
static int split_target(int p) {
    uint32_t taken = atomic_load(&g_players) | 1u; int mine = -1;
    for (int q = 0; q <= p && q < DM_PLAYERS; q++) if (joy_active(q) && joy_split(q)) {
        int t = -1; for (int i = 1; i < DM_PLAYERS; i++) if (!(taken >> i & 1) && (atomic_load(&g_ids_supported) & id_bit((uint32_t)i))) { t = i; break; }
        if (t >= 0) taken |= 1u << t;
        if (q == p) mine = t;
    }
    return mine;
}
static npad_ref npad_resolve(uint32_t id) {
    npad_ref r = {-1, KIND_NONE};
    int pl = player_of(id);
    if (pl >= 0) {
        r.pad = pl; r.kind = KIND_PAD;
        if (joy_active(pl)) {
            uint32_t s = atomic_load(&g_joy_sides[pl]);
            if (s == 3) r.kind = joy_split(pl) ? KIND_LEFT : (joy_supported(pl) & STYLE_JOYDUAL ? KIND_DUAL : KIND_PAD);
            else r.kind = s == 1 ? KIND_LEFT : KIND_RIGHT;
            if (r.kind == KIND_LEFT && !(joy_supported(pl) & STYLE_JOYLEFT)) r.kind = KIND_PAD;
            if (r.kind == KIND_RIGHT && !(joy_supported(pl) & STYLE_JOYRIGHT)) r.kind = KIND_PAD;
        }
    } else if (id > 0 && id < DM_PLAYERS) {   // the other half of a split pair
        for (int p = 0; p < DM_PLAYERS; p++) if (joy_active(p) && joy_split(p) && split_target(p) == (int)id) { r.pad = p; r.kind = KIND_RIGHT; break; }
    }
    return r;
}
// The style of a whole controller (KIND_PAD: the keyboard, a Pro-like pad, a Joy-Con pair the game supports no Joy-Con style for): Handheld on the Handheld npad, full-key elsewhere.
// The hid service connects a controller only in a style the game supports (SetSupportedNpadStyleSet), and games test the style before anything else: Nintendo Switch Sports Resort
// supports Handheld | JoyDual (0x6) in its lobby (main+0xa9e940), and its player check (main+0x58d0f0) found no player 1 on a full-key No1, so it opened the controller applet
// again every frame (main+0x3f9d264), 7,162 times in 3 minutes. On No1..No8 (docked, and players 2..8 in handheld mode), a full-key controller the game does not support is shown as a
// JoyDual pair instead, the supported style that has all its buttons and both sticks; no other style has them, so a game that supports neither keeps full-key, as before. The
// Handheld npad stays Handheld
static uint32_t pad_style(uint32_t id) {
    if (id == NPAD_HANDHELD) return STYLE_HANDHELD;
    uint32_t s = atomic_load(&g_styles_supported);
    return !(s & STYLE_FULLKEY) && (s & STYLE_JOYDUAL) ? STYLE_JOYDUAL : STYLE_FULLKEY;
}
static uint32_t kind_style(npad_ref r, uint32_t id) {
    uint32_t two = r.pad >= 0 && r.pad < DM_PLAYERS && atomic_load(&g_joy_two[r.pad]) ? atomic_load(&g_styles_supported) : 0;
    switch (r.kind) {
    case KIND_PAD: return pad_style(id);
    case KIND_DUAL: return STYLE_JOYDUAL | (two & STYLE_JOYDUAL2);
    case KIND_LEFT: return STYLE_JOYLEFT | (two & STYLE_JOYLEFT2);
    case KIND_RIGHT: return STYLE_JOYRIGHT | (two & STYLE_JOYRIGHT2);
    default: return 0;
    }
}
static uint32_t style_of(uint32_t id) { return kind_style(npad_resolve(id), id); }
static int32_t ax(uint64_t v, int i) { return (int16_t)(v >> (16 * i)); }
static int32_t pick(int32_t a, int32_t b) { return (a < 0 ? -a : a) >= (b < 0 ? -b : b) ? a : b; }   // whichever of two sources is deflected more

// Joy-Cons as devices (dm_input.h): which a pad holds, then each one's buttons and stick
void dm_joy_attach(int p, uint32_t sides) {
    if (p < 0 || p >= DM_PLAYERS) return;
    atomic_store(&g_joy_two[p], (sides & DM_JOY_2) && (sides & 3) ? 1u : 0u);
    sides &= 3;
    if (sides == 3 || !sides) atomic_store(&g_joy_paired[p], sides == 3);
    atomic_fetch_and(&g_joy_reported[p], sides);   // a Joy-Con taken away has reported nothing yet when it comes back
    if (atomic_exchange(&g_joy_sides[p], sides) != sides && !sides) for (int s = 0; s < 2; s++) { atomic_store(&g_joy_btn[p][s], 0); atomic_store(&g_joy_axes[p][s], 0); }
}
void dm_joy_set(int p, int side, uint64_t buttons, int32_t sx, int32_t sy) {
    if (p < 0 || p >= DM_PLAYERS || (side != 0 && side != 1)) return;
    uint64_t old = atomic_exchange(&g_joy_btn[p][side], buttons); atomic_fetch_or(&g_joy_latch[p][side], buttons & ~old);
    atomic_store(&g_joy_axes[p][side], pack(sx, sy, 0, 0));
}
static void note_merged(void) {   // logs the players the game does not see, when they change
    static _Atomic uint32_t last; uint32_t m = unsupported_players(), one = (uint32_t)one_player(), b = atomic_load(&g_benched) & m, bone = atomic_load(&g_bench_one), key = m | b << 8 | bone << 30 | one << 31;
    if (atomic_exchange(&last, key) == key) return;
    for (int p = 1; p < DM_PLAYERS; p++) if (m >> p & 1) {
        if (b >> p & 1 && atomic_load(&g_ids_supported) & id_bit((uint32_t)p)) LOG("hid: player %d's controller %s: the controller applet left it out (more players than the game asked for)", p + 1, one || bone ? "feeds player 1" : "is not used");
        else if (one) LOG("hid: player %d's controller feeds player 1: the game supports one player (no npad %d)", p + 1, p);
        else LOG("hid: player %d's controller is not used: the game supports no npad %d", p + 1, p);
    }
}
// Players a DM_CTL "P<n> <keys>" line connected (a scripted controller, kept connected from its first press on): ORed into the front end's mask, so a front end's change does not drop them
static _Atomic uint32_t g_script_players, g_front_players = 1;
static void script_player(int p) {
    if (p <= 0 || p >= DM_PLAYERS || atomic_fetch_or(&g_script_players, 1u << p) >> p & 1) return;
    LOG("DM_CTL: player %d's scripted controller connected", p + 1);
    dm_pad_players(atomic_load(&g_front_players));
}
void dm_pad_players(uint32_t mask) {
    atomic_store(&g_front_players, mask);
    mask = (mask | 1u | atomic_load(&g_script_players)) & ((1u << DM_PLAYERS) - 1);
    uint32_t old = atomic_exchange(&g_players, mask);
    atomic_fetch_and(&g_benched, mask);   // a controller that went away is no longer left out: the next one on its slot is a player until the applet says otherwise
    for (int p = 1; p < DM_PLAYERS; p++) if ((old ^ mask) >> p & 1 && !(mask >> p & 1)) dm_pad_set_player(p, 0, 0, 0, 0, 0);   // a controller that went away leaves nothing pressed
    note_merged();
}

// DM_REC=<file>: player 1's physical input (the keyboard, its controller and the controllers merged into it, as fill reads them) written as DM_KEYS steps, so a run plays again
// with DM_KEYS=@<file> from the same save and build: a bug report's path without the game's data, or a regress route recorded instead of written. DM_KEYS and DM_CTL have
// a path of their own (script_set) and are never recorded. Each step is one state of player 1, from the presented frame it began to the frame it ended:
// "f<frame>:<keys>~f<frames>," on a line of its own, the idle states between them left out. Frames, not seconds: under DM_DETCLOCK the guest clock and the game follow the
// presents, while seconds from host start drift with load. A state is stamped with the frame of the game's reads that saw it (rec_read, called by fill for player 1), not with the
// moment it arrived, and the replay applies it at the reads of that frame (keys_sync): frame N of the replay reads what frame N of the recording read. Within one frame the
// buttons of all its reads are ORed (a press that came and went between two reads reached the game through the press latch) and the sticks are the last read's.
// Sticks in hundredths of full tilt (L<x>/<y>, R<x>/<y>, what DM_KEYS reads back exactly), recorded again when an axis moves REC_STICK_STEP hundredths or leaves or comes back to the
// centre. The reading thread takes a mutex, compares and formats into memory; a writer thread appends that to the file every
// REC_FLUSH_MS (no file I/O on an input or guest thread). Off, a read costs one branch
enum { REC_STICK_STEP = 2, REC_FLUSH_MS = 250 };
typedef struct { uint64_t btn; int32_t q[4]; } rec_state;   // q: the sticks in hundredths
static _Atomic int g_rec_on;
static pthread_mutex_t g_rec_mu = PTHREAD_MUTEX_INITIALIZER, g_rec_io = PTHREAD_MUTEX_INITIALIZER;
static rec_state g_rec_cur; static uint64_t g_rec_from;   // the open state and the frame it began
static rec_state g_rec_frame; static uint64_t g_rec_at; static int g_rec_have;   // the state the reads of frame g_rec_at saw so far (g_rec_have: there were reads)
static char *g_rec_buf; static size_t g_rec_len, g_rec_cap;   // text not written yet
static FILE *g_rec_file;
static int rec_idle(const rec_state *s) { return !s->btn && !s->q[0] && !s->q[1] && !s->q[2] && !s->q[3]; }
static int rec_same(const rec_state *a, const rec_state *b) {   // within the recording's steps
    if (a->btn != b->btn) return 0;
    for (int i = 0; i < 4; i++) if (!a->q[i] != !b->q[i] || abs(a->q[i] - b->q[i]) >= REC_STICK_STEP) return 0;
    return 1;
}
// One state as DM_KEYS keys ("A+ZR+L0.50/-0.25"); returns the length
static int rec_keys(const rec_state *s, char *out, size_t cap) {
    int n = 0;
    #define PUT(fmt, ...) do { int w_ = snprintf(out + n, cap - (size_t)n, "%s" fmt, n ? "+" : "", ##__VA_ARGS__); if (w_ > 0 && (size_t)(n + w_) < cap) n += w_; } while (0)
    for (int b = 0; b < 16; b++) if (s->btn >> b & 1) PUT("%s", key_names[b]);
    if (s->btn & DM_BTN_SHAKE) PUT("Shake");
    if (s->q[0] || s->q[1]) PUT("L%.2f/%.2f", s->q[0] / 100.0, s->q[1] / 100.0);
    if (s->q[2] || s->q[3]) PUT("R%.2f/%.2f", s->q[2] / 100.0, s->q[3] / 100.0);
    #undef PUT
    return n;
}
static void rec_step(const rec_state *s, uint64_t from, uint64_t to) {   // appends one step to the text buffer (g_rec_mu held)
    char keys[160], line[200]; rec_keys(s, keys, sizeof keys);
    int n = snprintf(line, sizeof line, "f%llu:%s~f%llu,\n", (unsigned long long)from, keys, (unsigned long long)(to - from));
    if (n <= 0 || (size_t)n >= sizeof line) return;
    if (g_rec_len + (size_t)n > g_rec_cap) { size_t cap = g_rec_cap ? g_rec_cap * 2 : 4096; while (cap < g_rec_len + (size_t)n) cap *= 2; char *b = realloc(g_rec_buf, cap); if (!b) return; g_rec_buf = b; g_rec_cap = cap; }
    memcpy(g_rec_buf + g_rec_len, line, (size_t)n); g_rec_len += (size_t)n;
}
static void rec_frame_done(void) {   // the reads of frame g_rec_at are over: its state ends the open one when it differs (g_rec_mu held)
    if (!g_rec_have) return;
    if (!rec_same(&g_rec_frame, &g_rec_cur)) {
        if (!rec_idle(&g_rec_cur)) rec_step(&g_rec_cur, g_rec_from, g_rec_at);
        g_rec_cur = g_rec_frame; g_rec_from = g_rec_at;
    }
    g_rec_have = 0;
}
// A read of player 1 (fill): btn and a are what it composed from the physical sources, before the scripted path joins
static void rec_read(uint64_t btn, const int32_t a[4]) {
    rec_state s = {btn};
    for (int i = 0; i < 4; i++) { long q = lrint(a[i] * 100.0 / DM_STICK_MAX); s.q[i] = (int32_t)(q < -100 ? -100 : q > 100 ? 100 : q); }
    pthread_mutex_lock(&g_rec_mu);
    uint64_t f = dm_guest_frame();
    if (g_rec_have && f != g_rec_at) rec_frame_done();
    if (!g_rec_have) { g_rec_frame = s; g_rec_at = f; g_rec_have = 1; }
    else { s.btn |= g_rec_frame.btn; g_rec_frame = s; }   // buttons over all the frame's reads, the sticks as the last read saw them
    pthread_mutex_unlock(&g_rec_mu);
}
static void rec_flush(void) {   // the text so far to the file, outside the input lock
    pthread_mutex_lock(&g_rec_io);
    pthread_mutex_lock(&g_rec_mu); char *b = g_rec_buf; size_t n = g_rec_len; g_rec_buf = NULL; g_rec_len = g_rec_cap = 0; pthread_mutex_unlock(&g_rec_mu);
    if (n && g_rec_file) { fwrite(b, 1, n, g_rec_file); fflush(g_rec_file); }
    free(b);
    pthread_mutex_unlock(&g_rec_io);
}
// At exit (exit, SIGTERM through dm_host_shutdown): the open state ends at the current frame, or after the last frame read when that is later (a state its reads saw lasts through
// them), and everything goes to the file. A kill loses at most the last REC_FLUSH_MS
void dm_rec_finish(void) {
    if (!atomic_load(&g_rec_on)) return;
    pthread_mutex_lock(&g_rec_mu);
    uint64_t last = g_rec_have ? g_rec_at + 1 : 0; rec_frame_done();
    uint64_t f = dm_guest_frame(); if (f < last) f = last;
    if (!rec_idle(&g_rec_cur)) rec_step(&g_rec_cur, g_rec_from, f);
    g_rec_cur = (rec_state){0}; g_rec_from = f;
    pthread_mutex_unlock(&g_rec_mu);
    rec_flush();
}
static void *rec_main(void *arg) { (void)arg; for (;;) { usleep(REC_FLUSH_MS * 1000); rec_flush(); } return NULL; }
static int rec_open(const char *path) {   // a new recording (the file starts empty: one file, one run's timeline)
    if (!(g_rec_file = fopen(path, "w"))) { LOG("[RED] DM_REC: cannot write %s", path); return 0; }
    atomic_store(&g_rec_on, 1);
    return 1;
}
static void rec_start(void) {
    const char *e = getenv("DM_REC"); if (!e || !*e || !rec_open(e)) return;
    pthread_t th; if (!pthread_create(&th, NULL, rec_main, NULL)) pthread_detach(th);
    atexit(dm_rec_finish);
    LOG("DM_REC: recording player 1's input to %s as DM_KEYS steps in presented frames", e);
}

// Stick direction pseudo-buttons (NpadButton 16..23) from the sticks' values
static uint64_t stick_dirs(const int32_t a[4]) {
    uint64_t b = 0;
    if (a[0] < -STICK_THRESH) b |= 1u << 16;
    if (a[1] > STICK_THRESH) b |= 1u << 17;
    if (a[0] > STICK_THRESH) b |= 1u << 18;
    if (a[1] < -STICK_THRESH) b |= 1u << 19;
    if (a[2] < -STICK_THRESH) b |= 1u << 20;
    if (a[3] > STICK_THRESH) b |= 1u << 21;
    if (a[2] > STICK_THRESH) b |= 1u << 22;
    if (a[3] < -STICK_THRESH) b |= 1u << 23;
    return b;
}
// A Joy-Con style npad (JoyDual: both Joy-Cons of pad r.pad; JoyLeft / JoyRight: one): each Joy-Con's own buttons, rail buttons included, and its own stick, in the stick field of its side
static void fill_joy(NpadState *s, npad_ref r) {
    uint64_t b = 0; int32_t a[4] = {0}; int left = r.kind != KIND_RIGHT, right = r.kind != KIND_LEFT;
    for (int side = 0; side < 2; side++) if (side == 0 ? left : right) {
        b |= atomic_load(&g_joy_btn[r.pad][side]) | atomic_exchange(&g_joy_latch[r.pad][side], 0);
        uint64_t v = atomic_load(&g_joy_axes[r.pad][side]); a[2 * side] = ax(v, 0); a[2 * side + 1] = ax(v, 1);
    }
    s->lx = a[0]; s->ly = a[1]; s->rx = a[2]; s->ry = a[3];
    s->buttons = (b & 0x0f00ffffull) | stick_dirs(a);
    s->attributes = ATTR_CONNECTED | (left ? ATTR_LEFT_CONN : 0) | (right ? ATTR_RIGHT_CONN : 0);
}
static void fill(NpadState *s, uint32_t id, uint32_t style) {
    memset(s, 0, sizeof *s);
    s->sampling = atomic_fetch_add(&g_sampling, 1) + 1;
    npad_ref ref = npad_resolve(id);
    if (!(kind_style(ref, id) & style) || ref.pad < 0 || ref.pad >= DM_PLAYERS) return;   // no controller of this id / style: attributes = 0
    if (ref.kind >= KIND_DUAL) { fill_joy(s, ref); return; }
    int pl = ref.pad;
    // Player 1: keyboard, script, its controller and the players merged into it; another player: its own controller path
    uint32_t src = pl ? 1u << pl : 1u | merged_players();
    uint64_t b = 0; int32_t a[4] = {0};
    if (!pl) {
        keys_sync();   // DM_KEYS frame steps due in this frame, before the read
        b = atomic_load(&g_btn) | atomic_exchange(&g_latch, 0);
        uint64_t k = atomic_load(&g_axes); for (int i = 0; i < 4; i++) a[i] = ax(k, i);
    }
    for (int p = 0; p < DM_PLAYERS; p++) if (src >> p & 1) {
        b |= atomic_load(&g_pad_btn[p]) | atomic_exchange(&g_pad_latch[p], 0);
        uint64_t v = atomic_load(&g_pad_axes[p]); for (int i = 0; i < 4; i++) a[i] = pick(a[i], ax(v, i));
    }
    if (!pl) {   // the physical sources so far (DM_REC records them, DM_BTN_SHAKE only from player 1's own controller and the keyboard, as shake_note), then the scripted path
        if (atomic_load_explicit(&g_rec_on, memory_order_relaxed)) rec_read((b & 0xffffull) | ((atomic_load(&g_btn) | atomic_load(&g_pad_btn[0])) & DM_BTN_SHAKE), a);
        b |= atomic_load(&g_sc_btn) | atomic_exchange(&g_sc_latch, 0);
        uint64_t c = atomic_load(&g_sc_axes); for (int i = 0; i < 4; i++) a[i] = pick(a[i], ax(c, i));
    }
    s->lx = a[0]; s->ly = a[1]; s->rx = a[2]; s->ry = a[3];
    b &= 0xffffull;
    if (s->lx < -STICK_THRESH) b |= 1u << 16;
    if (s->ly > STICK_THRESH) b |= 1u << 17;
    if (s->lx > STICK_THRESH) b |= 1u << 18;
    if (s->ly < -STICK_THRESH) b |= 1u << 19;
    if (s->rx < -STICK_THRESH) b |= 1u << 20;
    if (s->ry > STICK_THRESH) b |= 1u << 21;
    if (s->rx > STICK_THRESH) b |= 1u << 22;
    if (s->ry < -STICK_THRESH) b |= 1u << 23;
    s->buttons = b;
    s->attributes = ATTR_CONNECTED | (id == NPAD_HANDHELD ? ATTR_WIRED | ATTR_LEFT_CONN | ATTR_LEFT_WIRED | ATTR_RIGHT_CONN | ATTR_RIGHT_WIRED : ATTR_LEFT_CONN | ATTR_RIGHT_CONN);
}
// GetNpadStates(out, n, id): newest first, n entries: the current state repeated with consecutive sampling numbers. The host keeps no history, but on hardware the
// 16-entry history is always full, and games read all of it whatever count comes back: Super Mario Odyssey (main+0x93fc20) polls every controller slot each frame, which advances
// the shared sampling number by about 16 per frame, and with only entry 0 written neither stick did anything in play (the camera did not turn at its first hint) while buttons
// and menus (the stick's direction bits) worked; with every entry written the camera turns
static int states(NpadState *out, int n, uint32_t id, uint32_t style) {
    if (n <= 0) return 0;
    fill(out, id, style);
    for (int i = 1; i < n; i++) { out[i] = out[0]; out[i].sampling = out[0].sampling - i; }
    return n;
}

HLE_FORCE("_ZN2nn3hid14InitializeNpadEv", "connect to the hid service; the host takes input from the front-end state", void, h_hid_InitNpad, void) {}
// Supported npad ids: players 2..8 are reported only on supported npads; a one-player game ({No1, Handheld}) gets every controller as player 1 (merged_players); a four-player
// game ({No1..No4, Handheld}) gets players 1..4, and a fifth controller is not used
HLE_FORCE("_ZN2nn3hid22SetSupportedNpadIdTypeEPKjm", "supported controller ids; the host records them and reports further players only on supported ids", void, h_hid_SetIds, const uint32_t *ids, uint64_t n) {
    uint32_t m = 0; for (uint64_t i = 0; ids && i < n; i++) m |= id_bit(ids[i]);
    if (atomic_exchange(&g_ids_supported, m) == m) return;
    atomic_store(&g_benched, 0);   // the game names its npads anew: whoever the controller applet left out counts again
    for (int p = 0; p < DM_PLAYERS; p++) dm_haptics_clear(p);
    LOG("hid supported controller ids %#x (bit n: No n+1, bit 8 Handheld, bit 9 Other)", m);
    note_merged();
}
HLE_FORCE("_ZN2nn3hid24SetSupportedNpadStyleSetENS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "supported controller styles; the host records them and picks the reported style from them", void, h_hid_SetStyles, uint32_t styles) {
    if (styles == atomic_load(&g_styles_supported)) return;
    LOG("hid supported controller styles %#x", styles); atomic_store(&g_styles_supported, styles);   // Joy-Cons show as the styles the game supports (npad_resolve), a controller in another style (pad_style) as the hid service reports it
}
HLE_FORCE("_ZN2nn3hid24GetSupportedNpadStyleSetEv", "supported controller styles: all", uint32_t, h_hid_GetStyles, void) { return atomic_load(&g_styles_supported); }
HLE_FORCE("_ZN2nn3hid15GetNpadStyleSetERKj", "controller style query; follows DM_OPMODE", uint32_t, h_hid_GetStyle, const uint32_t *id) { return style_of(*id); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadFullKeyStateEiRKj", "full-key controller states", int, h_hid_StatesFullKey, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_FULLKEY); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadHandheldStateEiRKj", "handheld controller states", int, h_hid_StatesHandheld, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_HANDHELD); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyDualStateEiRKj", "dual Joy-Con states (not connected on the host)", int, h_hid_StatesJoyDual, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYDUAL); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_16NpadJoyLeftStateEiRKj", "left Joy-Con states (not connected on the host)", int, h_hid_StatesJoyLeft, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYLEFT); }
HLE_FORCE("_ZN2nn3hid13GetNpadStatesEPNS0_17NpadJoyRightStateEiRKj", "right Joy-Con states (not connected on the host)", int, h_hid_StatesJoyRight, NpadState *s, int n, const uint32_t *id) { return states(s, n, *id, STYLE_JOYRIGHT); }
HLE_FORCE("_ZN2nn3hid14DisconnectNpadERKj", "disconnect a controller; ignored by the host", void, h_hid_Disconnect, const uint32_t *id) { (void)id; }
static uint32_t g_hold_type;
HLE_FORCE("_ZN2nn3hid18SetNpadJoyHoldTypeENS0_15NpadJoyHoldTypeE", "Joy-Con hold orientation; the host only records it", void, h_hid_HoldType, int t) { g_hold_type = (uint32_t)t; }
HLE_FORCE("_ZN2nn3hid18GetNpadJoyHoldTypeEv", "Joy-Con hold orientation: returns the value that was set", int, h_hid_GetHold, void) { return (int)g_hold_type; }
// How a pair of Joy-Cons on an npad is used (npad_resolve): as one dual npad, or split into two single ones, the left Joy-Con staying on the npad
static void joy_assign(uint32_t id, int single) {
    if (id >= DM_PLAYERS) return;
    uint32_t was = atomic_load(&g_single_mask), now = single ? was | 1u << id : was & ~(1u << id);
    if (atomic_exchange(&g_single_mask, now) != now) LOG("hid: npad %u: %s Joy-Con assignment", id, single ? "single" : "dual");
}
// The npad ids the SDK's own checks accept (No1..No8, Other, Handheld), as an index 0..9; -1 otherwise
static int npad_id_slot(uint32_t id) { return id < 8 ? (int)id : id == 0x10 ? 8 : id == 0x20 ? 9 : -1; }
#define RES_INVALID_NPAD_ID 0x58acau   // the SDK's Result for any other id
// Controller applet (system UI): report the connected players, with player 1's controller selected. ControllerSupportResultInfo = { int playerCount; uint32_t selectedId; };
// ControllerSupportArg starts with { int8 playerCountMin, playerCountMax }: the count is capped at the maximum. Fewer players than the minimum: on a Switch the applet stays open
// until enough controllers are there or the player closes it, and the SDK's ShowControllerSupportCore (Mario Kart 8 Deluxe 4.0.0 sdk+0x24eeb0) turns the applet's result into
// 0 success, 2 → 2202-3102, anything else → ResultControllerSupportCanceled (2202-3101). With a window the front end does the same (front_ctrl_support, ctrl_prompt.h): its prompt
// stays up on the game's thread while controllers join (a first button press with DM_PAD_PRESS_TO_JOIN, a connection without it) until enough are in or the player closes it
// (GitHub issue #1640: Mario Kart World's player screen closed when the second controller had not pressed yet). Without a window, or with scripted input (DM_KEYS, DM_CTL), nobody
// is at the controllers: the host answers at once, as the player closing it. An OK with fewer players is an answer a Switch never gives: Mario Kart 8 Deluxe asks for two in 2P
// and opened the applet again every few frames on it (GitHub issue #550)
// More players than the maximum: on a Switch the applet leaves no more controllers connected than the game takes. The host answered with the count capped and left every npad
// connected, so a game that takes one player saw two and opened the applet again, 1,671 times in one run of Ocarina of Time, with no input (GitHub issue #2237). The players
// past the maximum, the highest first, are now left out (g_benched) until the applet opens for more players, the game sets its supported npads again or their controller goes
// away; with a maximum of one their controllers feed player 1, as in a game that supports one npad, so whichever controller the player holds plays. No prompt asks which
// controller stays: a Switch's does, and with a maximum above one the lowest players stay here
// It runs on the big host stack, as every HLE_FORCE take-over: with too few players the front end's prompt runs on the game's thread, and Drag x Drive opens the applet from a
// thread with less than a log line's worth of stack left (PR #1943; memory.md, Host calls on small guest stacks)
#define RES_CTRL_CANCELED (202u | 3101u << 9)
int dm_players_reported(void) {   // a pair of Joy-Cons split into two npads is two players
    int n = 0; for (int p = 0; p < DM_PLAYERS; p++) n += reported(p) + (reported(p) && joy_active(p) && joy_split(p) && split_target(p) >= 0);
    return n;
}
static Result ctrl_support(int32_t *info, const int8_t *arg) {
    int count = dm_players_reported(), canceled = 0;
    uint32_t bench_was = atomic_load(&g_benched);
    // The game takes more players than it sees: those left out before count again, the lowest first
    for (int p = 1; arg && p < DM_PLAYERS && (arg[1] <= 0 || count < arg[1]); p++)
        if (atomic_load(&g_benched) >> p & 1) { atomic_fetch_and(&g_benched, ~(1u << p)); count = dm_players_reported(); }
    // Too few players, but a pair of Joy-Cons is two when each is held sideways (single assignment): the applet splits the pair, as the player does on a Switch, to reach the minimum
    for (int p = 0; arg && count < arg[0] && p < DM_PLAYERS; p++)
        if (reported(p) && joy_active(p) && atomic_load(&g_joy_sides[p]) == 3 && !(atomic_load(&g_single_mask) >> p & 1)) {
            joy_assign((uint32_t)p, 1);
            if (joy_split(p) && split_target(p) >= 0) count++; else joy_assign((uint32_t)p, 0);
        }
    if (arg && arg[0] > count) {
        const char *keys = getenv("DM_KEYS"), *ctl = getenv("DM_CTL");
        int r = (keys && *keys) || (ctl && *ctl) ? -1 : front_ctrl_support(arg[0], arg[1]);   // blocks until the prompt closes; -1: no window
        if (r < 0) LOG("hid controller applet: %d to %d players wanted, %d connected: too few, closed as the player would (canceled)", arg[0], arg[1], count);
        else {
            int was = count; count = dm_players_reported();
            LOG("hid controller applet: %d to %d players wanted, %d connected: the prompt waited, %d now (%s)", arg[0], arg[1], was, count, r && arg[0] <= count ? "OK" : "canceled");
        }
        canceled = r <= 0 || arg[0] > count;
    } else if (arg) LOG("hid controller applet: %d to %d players wanted, %d connected", arg[0], arg[1], count);
    for (int p = DM_PLAYERS - 1; arg && arg[1] > 0 && p > 0 && count > arg[1]; p--)
        if (reported(p)) { atomic_fetch_or(&g_benched, 1u << p); count = dm_players_reported(); }
    // The lowest players stay: one left out before takes the place of a higher player that connected since
    for (int q = 1; arg && q < DM_PLAYERS; q++) if (atomic_load(&g_benched) >> q & 1)
        for (int p = DM_PLAYERS - 1; p > q; p--) if (reported(p)) { atomic_store(&g_benched, (atomic_load(&g_benched) | 1u << p) & ~(1u << q)); break; }
    if (arg) {
        atomic_store(&g_bench_one, arg[1] == 1);
        if (atomic_load(&g_benched) != bench_was) note_merged();   // their npads connect or disconnect
    }
    if (arg && arg[1] > 0 && count > arg[1]) count = arg[1];   // a split pair of Joy-Cons on player 1 can still be one too many
    if (info) { info[0] = count; info[1] = (int32_t)connected_id(); }
    return canceled ? RES_CTRL_CANCELED : RES_OK;
}
HLE_FORCE("_ZN2nn3hid21ShowControllerSupportEPNS0_27ControllerSupportResultInfoERKNS0_20ControllerSupportArgE", "controller applet; the host reports the connected players (fewer than the minimum: the front end's prompt waits for more, canceled without a window; more than the maximum: the highest are left out)",
          Result, h_hid_ShowCtrl, int32_t *info, const int8_t *arg) {
    DM_APPLET("controller support");
    return ctrl_support(info, arg);
}
HLE_FORCE("_ZN2nn3hid28ShowControllerFirmwareUpdateERKNS0_27ControllerFirmwareUpdateArgE", "controller firmware update applet; not shown by the host", Result, h_hid_FwUpd, const void *arg) { DM_APPLET("controller firmware update"); (void)arg; return RES_OK; }
// Six-axis sensor (accelerometer + gyroscope) of the connected controller. SixAxisSensorState (0x60): acceleration in G, angular velocity in revolutions / s, angle in revolutions,
// direction = the controller's three axes. At rest it lies flat: gravity (0, 0, -1), no rotation, the identity direction. While shaking (DM_BTN_SHAKE: Space on the
// keyboard; DM_KEYS Shake) it moves side to side, x acceleration ±SHAKE_G with a yaw of ±SHAKE_REV, in whole cycles that end where they began. Samples are 5 ms apart on the
// guest clock, as the sensor's 200 Hz, and numbered by time, so a history read returns the samples at those times. Super Mario Odyssey reads all 16 entries of GetSixAxisSensorStates
// whatever count comes back (main+0x93fd84), takes the newest entry's acceleration for its swing detection (main+0x626244) and its angular velocity, angle and direction (main+0x5d72d4)
typedef struct { int64_t delta_ns, sampling; float acc[3], gyro[3], angle[3], dir[9]; uint32_t attributes, _pad; } SixAxisState;
_Static_assert(sizeof(SixAxisState) == 0x60, "SixAxisState");
enum { SIX_PERIOD_NS = 5000000 };
#define SHAKE_G 3.0
#define SHAKE_REV 1.5
static int shaking(uint64_t t) { return t >= atomic_load(&g_shake_t0) && t < atomic_load(&g_shake_end); }
// Live motion (dm_motion_set: a controller's gyro and accelerometer, front_mac.m through motion_apple.h): while a player's samples keep arriving (within
// MOTION_STALE_NS on the host clock) their six-axis sensor reports them (motion_fusion.h) instead of lying flat; a shake (DM_BTN_SHAKE) still adds its side-to-side acceleration.
// Each stream keeps its last LIVE_RING samples, stamped on the guest clock (live_ring): a history read or an IMU answer gives each 5 ms tick the sample at that tick's time,
// interpolated between the two around it, the IMU samples played back a sample interval late (DM_IMU_DELAY, six_source) so the ticks up to the clock's (answer_tick) lie between samples
// that have arrived, the newest held past its time only when a report stalls, so the SDK's sensor fusion gets a 200 Hz stream
// whatever the source's rate (GameController 60 to 1000 Hz, a Pro or Joy-Con 2 report's samples) and nothing made up beyond the newest sample. Before, every tick of an answer carried the newest
// sample, so the fusion saw a staircase of repeated samples. DM_MOTION=0 ignores live motion; DM_MOTION_AXES / DM_MOTION_ACC_SIGN map Apple's axes (motion_fusion.h);
// DM_MOTION_GAIN sets the tilt correction (default 0.5); DM_MOTION_LOG=1 logs player 1's mapped sample once a second, for checking the axes
enum { MOTION_STALE_NS = 300000000 };
#define MOTION_REST_GAP_NS 100000000ull   // motion_set_to: samples more than 100 ms apart break the rest window
// LIVE_RING samples cover 128 ms at 1 kHz and 1.28 s at 100 Hz, more than the 16 ticks (80 ms) an IMU answer reaches back at any source rate the host sees; 80 bytes each,
// about 340 KB for every stream of every player
enum { LIVE_RING = 128 };
#define LIVE_BLEND_MAX_NS 50000000ull   // live_at: past 50 ms between two samples, no blend of their directions
typedef struct { uint64_t t; float acc[3], gyro[3], angle[3], dir[9]; } motion_sample;   // t: the sample's host ns in the ring, guest ns once copied out (live_ring_copy); the SixAxisSensorState fields (motion_fill)
typedef struct { pthread_mutex_t mu; motion_state st; uint64_t last_ns, received_ns, logged; motion_sample ring[LIVE_RING]; unsigned head, count; motion_rest rest; motion_lag lag; } live_motion;
static live_motion g_motion[DM_PLAYERS] = {[0 ... DM_PLAYERS - 1] = {.mu = PTHREAD_MUTEX_INITIALIZER}};
static live_motion g_hid_motion[DM_PLAYERS] = {[0 ... DM_PLAYERS - 1] = {.mu = PTHREAD_MUTEX_INITIALIZER}};
static live_motion g_joy_motion[DM_PLAYERS][2] = {[0 ... DM_PLAYERS - 1] = {[0 ... 1] = {.mu = PTHREAD_MUTEX_INITIALIZER}}};   // each Joy-Con's own sensor (dm_motion_set_joy)
static pthread_once_t g_motion_once = PTHREAD_ONCE_INIT;
static motion_map g_motion_map; static float g_motion_gain = 0.5f; static int g_motion_off, g_motion_log;
static void motion_env(void) {
    const char *e = getenv("DM_MOTION"); g_motion_off = e && !atoi(e);
    if (!motion_map_parse(&g_motion_map, getenv("DM_MOTION_AXES"))) LOG("[hid] DM_MOTION_AXES='%s' is not like \"x,-z,y\": using x,y,z", getenv("DM_MOTION_AXES"));
    if ((e = getenv("DM_MOTION_ACC_SIGN")) && atof(e) < 0) g_motion_map.acc_sign = -1;
    if ((e = getenv("DM_MOTION_GAIN"))) g_motion_gain = (float)atof(e);
    g_motion_log = (e = getenv("DM_MOTION_LOG")) && atoi(e);
}
// A sample into its stream. hid: it is from a controller's HID reports (dm_motion_set_hid, dm_motion_set_joy), in the Switch frame and with its gyro offset taken off; else GameController's (dm_motion_set)
static void motion_set_to(live_motion *m, int p, const float acc[3], const float rot[3], int hid, uint64_t sample_ns);
static void motion_set(int p, const float acc[3], const float rot[3], int hid, uint64_t sample_ns) {
    if (p < 0 || p >= DM_PLAYERS) return;
    motion_set_to(hid ? &g_hid_motion[p] : &g_motion[p], p, acc, rot, hid, sample_ns);
}
// DM_IMU_LOG=<n> (diagnostic, for checking a real controller against the host's synthetic samples): every n-th raw motion sample as the host receives it, per stream (the source,
// player, the sample's host time, acceleration in G and rotation in rad/s as the front end gave them, before any axis map), and every n-th GetNpadImuSamples answer (the handle,
// how many, their sampling numbers, the newest one's values in the units the game gets: m/s^2 and rad/s, and whether live motion drove it). One line each
static _Atomic int g_imu_log = -1;   // -1 unread, 0 off, else n
static int imu_log_every(void) {
    int n = atomic_load(&g_imu_log);
    if (n < 0) { const char *e = getenv("DM_IMU_LOG"); n = e ? atoi(e) : 0; if (n < 0) n = 0; atomic_store(&g_imu_log, n); }
    return n;
}
static int imu_log_due(_Atomic uint64_t *count) { int n = imu_log_every(); return n > 0 && atomic_fetch_add(count, 1) % (uint64_t)n == 0; }
static void motion_set_to(live_motion *m, int p, const float acc[3], const float rot[3], int hid, uint64_t sample_ns) {
    static _Atomic uint64_t raw_count[4];   // per kind of stream: gc, hid, joyL, joyR (a shared count logged every n-th of the HID and Joy-Con streams together, always one of them)
    int kind = hid ? (m == &g_joy_motion[p][0] ? 2 : m == &g_joy_motion[p][1] ? 3 : 1) : 0;
    if (imu_log_due(&raw_count[kind]))
        LOG("imu raw %s p%d t=%.4f (%.1f ms before arrival) acc %+.4f %+.4f %+.4f G rot %+.4f %+.4f %+.4f rad/s", hid ? (m == &g_joy_motion[p][0] ? "joyL" : m == &g_joy_motion[p][1] ? "joyR" : "hid") : "gc",
            p + 1, (double)(sample_ns ? sample_ns : now_ns()) / 1e9, sample_ns ? (double)((int64_t)(now_ns() - sample_ns)) / 1e6 : 0.0, acc[0], acc[1], acc[2], rot[0], rot[1], rot[2]);
    pthread_once(&g_motion_once, motion_env); if (g_motion_off) return;
    // A stream's first sample of all zeros is no reading yet (a sensor at rest reads 1 G): a Joy-Con 2's first report carries one, which put a 0 G sample into the history. Later
    // zeros can be a real free fall (motion_apple.h), so only a stream with nothing fresh drops them
    if (!acc[0] && !acc[1] && !acc[2] && !rot[0] && !rot[1] && !rot[2]) {
        pthread_mutex_lock(&m->mu); int fresh = m->last_ns && now_ns() - m->received_ns <= MOTION_STALE_NS; pthread_mutex_unlock(&m->mu);
        if (!fresh) return;
    }
    static motion_map identity = {{0, 1, 2}, {1, 1, 1}, 1, 1};
    // Each transport owns its orientation. HID batches carry three sample times; arrival time controls freshness, never their integration intervals.
    uint64_t now = now_ns(), t = sample_ns ? sample_ns : now;
    if (!t || t > now || now - t > MOTION_STALE_NS) return;
    pthread_mutex_lock(&m->mu);
    if (!m->last_ns || now - m->received_ns > MOTION_STALE_NS) { motion_reset(&m->st); m->last_ns = 0; m->count = 0; motion_lag_reset(&m->lag); if (m->received_ns) motion_rest_reset(&m->rest); }   // stale: forget the offset too
    else if (t - m->last_ns > MOTION_REST_GAP_NS) motion_rest_break(&m->rest);   // a gap: the rest window starts again
    if (m->last_ns && t <= m->last_ns) { pthread_mutex_unlock(&m->mu); return; }
    float dt = m->last_ns && t > m->last_ns ? (float)((double)(t - m->last_ns) / 1e9) : 0;
    // GameController reports the gyro with its rest offset (a DualSense: 0.34, 0.20, -0.66 deg/s): the host learns it while the controller rests and takes it off (motion_rest),
    // as the Joy-Con 2 front end does for its own (s2_rest); the HID sources come corrected
    float rc[3]; if (!hid) { motion_rest_update(&m->rest, acc, rot, (double)t / 1e9); motion_rest_apply(&m->rest, rot, (double)t / 1e9, rc); rot = rc; }
    motion_update(&m->st, hid ? &identity : &g_motion_map, acc, rot, dt, g_motion_gain);
    if (g_motion_log && !p && t - m->logged > 1000000000ull) {
        m->logged = t;
        LOG("[hid] motion player 1 (%s): acc %+.2f %+.2f %+.2f G, gyro %+.3f %+.3f %+.3f rev/s (flat and still should read acc 0 0 -1)",
            hid ? "controller HID" : "controller", m->st.acc[0], m->st.acc[1], m->st.acc[2], m->st.gyro[0], m->st.gyro[1], m->st.gyro[2]);
    }
    if (m->last_ns) motion_lag_add(&m->lag, now - m->last_ns);   // how long after the last sample's time this one arrived: the delay the stream needs (six_source)
    m->last_ns = t; m->received_ns = now;
    if (!m->count) m->head = 0;   // a new stream (reset above) starts its history again
    motion_sample *r = &m->ring[(m->head + m->count) % LIVE_RING];
    r->t = t; motion_fill(&m->st, r->acc, r->gyro, r->angle, r->dir);   // host time: the guest clock's lag (a freeze, DM_DETCLOCK) is applied when read
    if (m->count < LIVE_RING) m->count++; else m->head = (m->head + 1) % LIVE_RING;
    pthread_mutex_unlock(&m->mu);
}
void dm_motion_set(int p, const float acc[3], const float rot[3]) { motion_set(p, acc, rot, 0, 0); }
void dm_motion_set_hid(int p, const float acc[3], const float rot[3], uint64_t t) { if (t) motion_set(p, acc, rot, 1, t); }
void dm_motion_set_joy(int p, int side, const float acc[3], const float rot[3], uint64_t t) {
    if (t && p >= 0 && p < DM_PLAYERS && (side == 0 || side == 1)) { atomic_fetch_or(&g_joy_reported[p], 1u << side); motion_set_to(&g_joy_motion[p][side], p, acc, rot, 1, t); }
}
void dm_motion_clear_joy(int p, int side) {
    if (p < 0 || p >= DM_PLAYERS || (side != 0 && side != 1)) return;
    pthread_mutex_lock(&g_joy_motion[p][side].mu); g_joy_motion[p][side].last_ns = 0; pthread_mutex_unlock(&g_joy_motion[p][side].mu);
}
void dm_motion_clear(int p) {
    if (p < 0 || p >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_motion[p].mu); g_motion[p].last_ns = 0; motion_rest_reset(&g_motion[p].rest); pthread_mutex_unlock(&g_motion[p].mu);   // another controller may take the player
}
void dm_motion_clear_hid(int p) {
    if (p < 0 || p >= DM_PLAYERS) return;
    pthread_mutex_lock(&g_hid_motion[p].mu); g_hid_motion[p].last_ns = 0; pthread_mutex_unlock(&g_hid_motion[p].mu);
}
static uint64_t motion_fresh(live_motion *m) {   // a source's timestamp while it is fresh (0 when not)
    pthread_mutex_lock(&m->mu);
    uint64_t t = m->last_ns && now_ns() - m->received_ns <= MOTION_STALE_NS ? m->last_ns : 0;
    pthread_mutex_unlock(&m->mu);
    return t;
}
// The live stream behind player p (its controller's GameController stream, else its HID one; in a one-player game the freshest of every merged player's); NULL while none is fresh
static live_motion *motion_pick(int p) {
    if (p < 0 || p >= DM_PLAYERS) return NULL;
    // Match the button path: in a one-player game, a controller in another slot also drives player 1.
    uint32_t src = p ? 1u << p : 1u | merged_players(); uint64_t newest = 0; live_motion *best = NULL;
    for (int i = 0; i < DM_PLAYERS; i++) if (src >> i & 1) {
        live_motion *m = &g_motion[i]; uint64_t t = motion_fresh(m);
        if (!t) { m = &g_hid_motion[i]; t = motion_fresh(m); }
        if (t > newest) { newest = t; best = m; }
    }
    return best;
}
// One Joy-Con's sensor: its own stream while fresh; NULL (the host's flat sensor) once it has reported its own motion and is still attached but its stream went quiet (a
// Joy-Con 2 skipping reports, for any length of time), as the pad's merged stream holds the controller in another frame (a sideways Joy-Con's, a quarter turn away); else
// the pad's merged one (a controller that reports no per-Joy-Con motion, or a Joy-Con not attached)
static int joy_own(int p, int side) {   // 1: its own stream is fresh, 0: it went quiet, -1: none
    pthread_once(&g_motion_once, motion_env); if (g_motion_off) return -1;
    if (motion_fresh(&g_joy_motion[p][side])) return 1;
    return atomic_load(&g_joy_reported[p]) >> side & atomic_load(&g_joy_sides[p]) >> side & 1 ? 0 : -1;
}
static live_motion *motion_joy_pick(int p, int side) {
    if (p < 0 || p >= DM_PLAYERS || (side != 0 && side != 1)) return motion_pick(p);
    int own = joy_own(p, side);
    return own > 0 ? &g_joy_motion[p][side] : own == 0 ? NULL : motion_pick(p);
}
// A stream's history from guest time `from` on, copied under its lock while the stream is still fresh (the input thread holds that lock only to add a sample): the newest samples
// back to the first at or before `from` (one before the window, for interpolating its start), oldest first, their times turned into guest ns with the guest clock's lag now;
// *n of them, 0 when the stream went stale, and *lag the playout delay the stream needs (motion_lag). The times only shift by the current lag, so they stay in order through
// a change of lag. A frozen or DM_DETCLOCK guest clock (guest_clock_pinned) maps every sample to one time, so only the newest is copied: the state now
static void live_ring_copy(live_motion *m, uint64_t from, motion_sample *out, unsigned *n, uint64_t *lag) {
    int pinned = guest_clock_pinned();
    pthread_mutex_lock(&m->mu);
    unsigned k = 0; *lag = m->lag.delay_ns;
    if (m->last_ns && now_ns() - m->received_ns <= MOTION_STALE_NS) {
        while (k < m->count) { k++; if (pinned || guest_ns_at_host_time(m->ring[(m->head + m->count - k) % LIVE_RING].t) <= from) break; }
        for (unsigned i = 0; i < k; i++) out[i] = m->ring[(m->head + m->count - k + i) % LIVE_RING];
    }
    pthread_mutex_unlock(&m->mu);
    for (unsigned i = 0; i < k; i++) out[i].t = guest_ns_at_host_time(out[i].t);
    *n = k;
}
// The sample at guest time t from a history (oldest first, n >= 1): interpolated between the samples around t, the oldest before it, the newest held after it (never
// extrapolated)
// Returns 1 when the tick carries no change of its own: held (before the oldest or past the newest sample, or a single sample) or between two samples that read alike
static int live_at(const motion_sample *r, unsigned n, uint64_t t, motion_sample *o) {
    if (t <= r[0].t) { *o = r[0]; o->t = t; return 1; }
    if (t >= r[n - 1].t) { *o = r[n - 1]; o->t = t; return 1; }
    unsigned lo = 0, hi = n - 1;   // r[lo].t < t < r[hi].t: bisect
    while (hi - lo > 1) { unsigned mid = (lo + hi) / 2; if (r[mid].t <= t) lo = mid; else hi = mid; }
    const motion_sample *a = &r[lo], *b = &r[hi];
    float f = (float)((double)(t - a->t) / (double)(b->t - a->t));
    o->t = t;
    for (int i = 0; i < 3; i++) { o->acc[i] = a->acc[i] + (b->acc[i] - a->acc[i]) * f; o->gyro[i] = a->gyro[i] + (b->gyro[i] - a->gyro[i]) * f; o->angle[i] = a->angle[i] + (b->angle[i] - a->angle[i]) * f; }
    // direction: the two rotations blended and made a rotation again (normalized lerp; a 5 ms step turns far less than the angle where that departs from a slerp). Samples more
    // than LIVE_BLEND_MAX_NS apart, or a blend that nearly vanishes (rotations about half a turn apart: a column shorter than 0.5), take the nearer sample's direction instead,
    // so the direction is always a rotation
    const motion_sample *near = f < 0.5f ? a : b;
    memcpy(o->dir, near->dir, sizeof o->dir);
    int same = !memcmp(a->acc, b->acc, sizeof a->acc) && !memcmp(a->gyro, b->gyro, sizeof a->gyro);   // NOLINT(bugprone-suspicious-memory-comparison): a held sample is the same floats bit for bit, which is what "same" means here
    if (b->t - a->t > LIVE_BLEND_MAX_NS) return same;
    float R[3][3]; for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) R[i][j] = a->dir[3 * i + j] + (b->dir[3 * i + j] - a->dir[3 * i + j]) * f;
    for (int j = 0; j < 3; j++) if (R[0][j] * R[0][j] + R[1][j] * R[1][j] + R[2][j] * R[2][j] < 0.25f) return same;
    m3_orthonormalize(R);
    for (int i = 0; i < 3; i++) for (int j = 0; j < 3; j++) o->dir[3 * i + j] = R[i][j];
    return same;
}
// The newest tick an answer reaches: the guest clock's current tick, live or not (a tick past the newest sample holds that sample: a source's latency of a few ticks), and never
// below the last tick answered for that handle (a high-water mark per handle and kind of answer), so the sampling numbers a game reads never go back: not when another thread
// read the clock a moment earlier, nor when the source changes. Before, a live answer ended at the newest sample's tick: a first sample stamped a few ticks late, or a switch
// between a controller's GameController and HID streams with different latencies, took the numbers back, and a game polling faster than its controller reported read the same
// numbers again, then a jump when the controller stopped. HWM_SLOTS handles per kind; an unknown handle past them goes by the clock alone
enum { HWM_SLOTS = 64, HWM_BACK_TICKS = 200 };   // 200 ticks: 1 s
// Each slot also keeps the playout delay last applied to that handle and when (answer_delay)
typedef struct { _Atomic uint32_t key[HWM_SLOTS]; _Atomic int64_t tick[HWM_SLOTS]; _Atomic uint64_t delay[HWM_SLOTS], delay_at[HWM_SLOTS]; } tick_hwm;
static tick_hwm g_hwm_states, g_hwm_imu;
static int hwm_slot(tick_hwm *w, uint32_t h) {   // the handle's slot, taken on first use; -1 when all are taken
    uint32_t key = h | 1u << 31;   // never 0 (a free slot)
    for (unsigned i = 0, at = (h * 2654435761u) % HWM_SLOTS; i < HWM_SLOTS; i++, at = (at + 1) % HWM_SLOTS) {
        uint32_t k = atomic_load(&w->key[at]);
        if (!k) { uint32_t z = 0; if (!atomic_compare_exchange_strong(&w->key[at], &z, key) && z != key) continue; k = key; }
        if (k == key) return (int)at;
    }
    return -1;
}
static int64_t answer_tick(tick_hwm *w, uint32_t h, int64_t now_tick) {
    int at = hwm_slot(w, h);
    if (at >= 0) {
        int64_t prev = atomic_load(&w->tick[at]);
        // a mark more than HWM_BACK_TICKS ahead of the clock means the clock went back (a reset, a restored state): start again from the clock, not pinned to the old mark
        while ((prev < now_tick || prev - now_tick > HWM_BACK_TICKS) && !atomic_compare_exchange_weak(&w->tick[at], &prev, now_tick)) {}
        return prev > now_tick && prev - now_tick <= HWM_BACK_TICKS ? prev : now_tick;
    }
    return now_tick;
}
// The playout delay an answer applies to a live stream's data (not to its sampling numbers): the target (DM_IMU_DELAY / DM_SIX_DELAY, else the stream's motion_lag) reached at
// no more than LIVE_DELAY_SLEW of the guest time passed since the handle's last answer. A delay that jumped between answers would replay or skip the stream's data (the numbers
// stay monotone, but data time minus tick time jumps: 25 ms at 300 deg/s is 7.5 degrees); slewed under 1, the data time of the ticks a game takes keeps going forward, 5% faster
// or slower while the delay settles (a new stream, a stale restart, another stream behind the handle). The first answer, one after a second without any, or one after the clock
// went back takes the target at once: nothing was fed just before it to jump from
#define LIVE_DELAY_SLEW 0.05
#define LIVE_DELAY_MAX_NS MOTION_LAG_MAX_NS   // the most either path delays (DM_IMU_DELAY / DM_SIX_DELAY are bounded by it too)
// DM_SIX_DELAY / DM_IMU_DELAY (imu_env_read): each path's delay in ns, -1 for the stream's own (motion_lag). GetSixAxisSensorStates [0] defaults to none: a Switch 1 game
// reads the newest state each frame (aiming, steering), so a delay only adds latency, and a few held ticks cost it nothing. GetNpadImuSamples [1] defaults to the stream's own:
// the SDK's sensor fusion integrates every tick
static _Atomic int64_t g_live_delay_cfg[2] = {0, -1};
#define LIVE_DELAY_IDLE_NS 1000000000ull
static uint64_t answer_delay(tick_hwm *w, uint32_t h, uint64_t target, uint64_t now) {
    int at = hwm_slot(w, h);
    if (at < 0) return target;
    uint64_t prev = atomic_load(&w->delay[at]), when = atomic_load(&w->delay_at[at]), d = target;
    if (when && now >= when && now - when <= LIVE_DELAY_IDLE_NS) {
        uint64_t step = (uint64_t)((double)(now - when) * LIVE_DELAY_SLEW);
        if (target > prev + step) d = prev + step;
        else if (target + step < prev) d = prev - step;
    }
    atomic_store(&w->delay[at], d); atomic_store(&w->delay_at[at], now ? now : 1);
    return d;
}
static void six_sample_live(SixAxisState *s, int64_t samp, const motion_sample *ms, int shakes) {
    memset(s, 0, sizeof *s);
    uint64_t t = (uint64_t)samp * SIX_PERIOD_NS;
    s->delta_ns = SIX_PERIOD_NS; s->sampling = samp; s->attributes = 1;
    memcpy(s->acc, ms->acc, sizeof s->acc); memcpy(s->gyro, ms->gyro, sizeof s->gyro); memcpy(s->angle, ms->angle, sizeof s->angle); memcpy(s->dir, ms->dir, sizeof s->dir);
    if (shakes && shaking(t)) s->acc[0] += (float)(SHAKE_G * sin(2 * M_PI * SHAKE_HZ * (double)(t - atomic_load(&g_shake_t0)) / 1e9));
}
static void six_sample(SixAxisState *s, int64_t samp, int shakes) {
    memset(s, 0, sizeof *s);
    uint64_t t = (uint64_t)samp * SIX_PERIOD_NS;
    s->delta_ns = SIX_PERIOD_NS; s->sampling = samp; s->attributes = 1;   // IsConnected
    s->acc[2] = -1;
    double yaw = 0;
    if (shakes && shaking(t)) {
        double ph = 2 * M_PI * SHAKE_HZ * (double)(t - atomic_load(&g_shake_t0)) / 1e9;
        s->acc[0] = (float)(SHAKE_G * sin(ph));
        s->gyro[2] = (float)(SHAKE_REV * sin(ph));
        yaw = SHAKE_REV / (2 * M_PI * SHAKE_HZ) * (1 - cos(ph)); s->angle[2] = (float)yaw;
    }
    float c = (float)cos(2 * M_PI * yaw), sn = (float)sin(2 * M_PI * yaw);
    s->dir[0] = c; s->dir[1] = sn; s->dir[3] = -sn; s->dir[4] = c; s->dir[8] = 1;
}
// SixAxisSensorHandle, as the console's SDK makes it (nn::hid::utility::GetSixAxisSensorHandle, sdk+0x6d8490 in Nintendo Switch Sports Resort's SDK 22; its GetSixAxisSensorHandles,
// GetNpadImuHandles and GetSensorFusionHandles all call it): byte 0 the NpadStyleIndex (3 full key or GameCube, 4 handheld, 5 Joy-Con pair, 6 left Joy-Con, 7 right Joy-Con), byte 1
// the npad id, byte 2 the device (0 the left and 1 the right sensor of two; SDK 22 gives a full-key or handheld controller's single sensor device 2). The host's own handles
// (GetSixAxisSensorHandles below) use the same numbering, so a handle from either reads the same everywhere. They used to carry the style's bit number in byte 0, and the SDK's IMU
// handles that the fusion passes to GetNpadImuSamples (0x5 / 0x10005 for a pair on No1) read as style bit 5, never connected: no samples, and Nintendo Switch Sports Resort's
// fusion never ran. six_style_bit: the style bit of a handle's style index, -1 for an index the host has no style for or a device past 2 (any style takes devices 0 to 2, as the
// host's check always did: SDKs differ in which device they give a style, and six_source picks a pair's side from bit 0 of the device). The styles made of two Joy-Cons (handheld, dual) have two sensors in the host's handles, the others one. A handle reads as connected while its controller is connected
// in that style; otherwise its entries are zero (not connected) and the count 0. Only player 1's controller shakes (DM_BTN_SHAKE comes from the keyboard and DM_KEYS,
// both player 1)
enum { SIX_IDX_FULLKEY = 3, SIX_IDX_HANDHELD = 4, SIX_IDX_JOYDUAL = 5, SIX_IDX_JOYLEFT = 6, SIX_IDX_JOYRIGHT = 7 };
static int six_style_bit(uint32_t h) {
    uint32_t idx = h & 0xff, dev = h >> 16 & 0xff;
    if (idx < SIX_IDX_FULLKEY || idx > SIX_IDX_JOYRIGHT || dev > 2) return -1;
    return (int)(idx - SIX_IDX_FULLKEY);   // FullKey .. JoyRight are style bits 0 .. 4
}
static int six_connected(uint32_t h) { int b = six_style_bit(h); return b >= 0 && (style_of(h >> 8 & 0xff) >> b & 1); }
// The style index of a style set as the SDK's utility picks it: the first of FullKey (or GameCube, bit 5), Handheld, JoyDual (or its Joy-Con 2 form, bit 12), JoyLeft (13), JoyRight (14);
// 0 (no sensor) for none of them
static uint32_t six_style_index(uint32_t style) {
    if (style & (STYLE_FULLKEY | 1u << 5)) return SIX_IDX_FULLKEY;
    if (style & STYLE_HANDHELD) return SIX_IDX_HANDHELD;
    if (style & (STYLE_JOYDUAL | STYLE_JOYDUAL2)) return SIX_IDX_JOYDUAL;
    if (style & (STYLE_JOYLEFT | STYLE_JOYLEFT2)) return SIX_IDX_JOYLEFT;
    if (style & (STYLE_JOYRIGHT | STYLE_JOYRIGHT2)) return SIX_IDX_JOYRIGHT;
    return 0;
}
// The motion behind a handle: a Joy-Con npad's own Joy-Con, else its player's controller. Fills src with that stream's history (live_ring_copy) and returns its count: 0 when no
// live stream is fresh (the sensor lies flat). Also whether it shakes
typedef struct { motion_sample r[LIVE_RING]; unsigned n; uint64_t delay; } live_hist;   // delay: the playout delay its times carry (six_source)
static pthread_once_t g_imu_once; static void imu_env_read(void);   // DM_IMU_DELAY and the rest (below, at the IMU state)
// The stream behind a handle (NULL: the host's flat sensor). A Joy-Con npad reads its own Joy-Con, a real L + R pair the game takes as a pair (KIND_DUAL) each handle its own
// half. A whole controller reads its player's merged stream, except Joy-Cons the game is shown as a pair (pad_style: a game such as Nintendo Switch Sports Resort supports JoyDual
// but no single Joy-Con style), which read the Joy-Cons' own streams in the frame of a Joy-Con of a pair (upright), as the game takes a JoyDual sensor to be; the merged stream
// holds a single Joy-Con in the frame of one held sideways, a quarter turn away. Both halves there (a pair in another player's slot of a one-player game, merged_players): each
// handle its own half. A single Joy-Con from the start: both handles read it, not one of them a flat sensor, as Switch Sports Resort read device 0 (the left half's) 50 times as
// often as device 1 with a right Joy-Con alone, so a flat device 0 would leave its racket still. A pair that lost a half: the missing half's handle lies flat, as its sensor is
// gone, rather than taking the other half's stream mid-game (a jump to another controller's frame)
static live_motion *six_stream(uint32_t h, npad_ref ref, int pl) {
    int dev = (int)(h >> 16 & 1);
    if (ref.kind >= KIND_DUAL) return motion_joy_pick(ref.pad, ref.kind == KIND_LEFT ? 0 : ref.kind == KIND_RIGHT ? 1 : dev);
    if (pl >= 0 && pl < DM_PLAYERS && kind_style(ref, h >> 8 & 0xff) == STYLE_JOYDUAL) {
        uint32_t src = 1u << pl | (pl ? 0 : merged_players());
        pthread_once(&g_motion_once, motion_env);
        for (int q = 0; q < DM_PLAYERS; q++) if (src >> q & 1 && joy_active(q)) {
            uint32_t sides = atomic_load(&g_joy_sides[q]); int side = sides == 3 ? dev : sides == 2;
            if (sides != 3 && side != dev && atomic_load(&g_joy_paired[q])) return NULL;
            int own = joy_own(q, side);
            if (own > 0) return &g_joy_motion[q][side];
            if (!own) return NULL;   // it went quiet: flat, not the merged stream's frame
        }
    }
    return motion_pick(pl);
}
// The history behind a handle for an answer (w: g_hwm_states or g_hwm_imu, whose handle slots keep the delay), played back late by the path's delay (answer_delay): every
// time moved d later, so a tick at the present lies between two samples that have arrived. None on a pinned guest clock (live_ring_copy keeps only the newest sample)
static unsigned six_source(uint32_t h, uint64_t from, live_hist *src, int *shakes, tick_hwm *w) {   // from: the oldest tick's guest time the answer needs
    npad_ref ref = npad_resolve(h >> 8 & 0xff);
    int pl = ref.kind >= KIND_DUAL ? ref.pad : player_of(h >> 8 & 0xff);
    *shakes = pl == 0 && ref.kind < KIND_DUAL;
    live_motion *m = six_stream(h, ref, pl);
    src->n = 0; src->delay = 0;
    if (!m) return 0;
    pthread_once(&g_imu_once, imu_env_read);
    uint64_t lag; live_ring_copy(m, from > LIVE_DELAY_MAX_NS ? from - LIVE_DELAY_MAX_NS : 0, src->r, &src->n, &lag);
    if (!src->n) return 0;
    int64_t cfg = atomic_load(&g_live_delay_cfg[w == &g_hwm_imu]);
    uint64_t d = 0;
    if (guest_clock_pinned()) answer_delay(w, h, 0, 0);   // none, and the first answer after takes its target at once (nothing to slew from)
    else d = answer_delay(w, h, cfg >= 0 ? (uint64_t)cfg : lag, guest_ns());
    for (unsigned i = 0; i < src->n; i++) src->r[i].t += d;
    src->delay = d;
    return src->n;
}
// Whether the sensor behind a handle reads without a gyro offset: the host's own (no live source), a HID or Joy-Con 2 stream (their front ends take off the offset they
// learn or read from the controller), or a GameController stream once the host has learned its rest offset (motion_rest)
static int six_offset_known(uint32_t h) {
    npad_ref ref = npad_resolve(h >> 8 & 0xff);
    int pl = ref.kind >= KIND_DUAL ? ref.pad : player_of(h >> 8 & 0xff);
    live_motion *m = six_stream(h, ref, pl);
    if (!m || m < g_motion || m >= g_motion + DM_PLAYERS) return 1;
    pthread_mutex_lock(&m->mu); int have = m->rest.have; pthread_mutex_unlock(&m->mu);
    return have;
}
// The state at a tick: from the live history (live_at) while there is one, else the host's own (flat or shaken)
static int six_at(SixAxisState *s, int64_t samp, const live_hist *src, int shakes) {   // returns live_at's "no change of its own" (0 for the host's own samples)
    if (src->n) { motion_sample ms; int held = live_at(src->r, src->n, (uint64_t)samp * SIX_PERIOD_NS, &ms); six_sample_live(s, samp, &ms, shakes); return held; }
    six_sample(s, samp, shakes); return 0;
}
static int six_states(SixAxisState *out, int n, uint32_t h) {
    if (n <= 0) return 0;
    if (!six_connected(h)) { memset(out, 0, sizeof *out * (size_t)n); return 0; }
    static _Thread_local live_hist src;   // 10 KB: off the guest thread's stack
    int64_t samp = answer_tick(&g_hwm_states, h, (int64_t)(guest_ns() / SIX_PERIOD_NS)), first = samp - n + 1;
    int shakes; six_source(h, first > 0 ? (uint64_t)first * SIX_PERIOD_NS : 0, &src, &shakes, &g_hwm_states);
    for (int i = 0; i < n; i++) six_at(&out[i], samp - i, &src, shakes);
    return n;
}
// A style set of several styles (FullKey | JoyDual, as a game that takes either passes) gets the style the npad's controller is shown in, when it is one of them (else the first, as
// the SDK's utility); two handles when the set holds a style made of two Joy-Cons (handheld, dual), as the host always answered; none for a set with no style that has a sensor
HLE_FORCE("_ZN2nn3hid23GetSixAxisSensorHandlesEPNS0_19SixAxisSensorHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "six-axis sensor handles: two for handheld / dual Joy-Cons, one otherwise",
          int, h_hid_SixHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) {
    uint32_t shown = six_style_index(style & style_of(*id & 0xff)), idx = shown ? shown : six_style_index(style);
    if (!idx || n <= 0) return 0;
    int k = style & (STYLE_HANDHELD | STYLE_JOYDUAL | STYLE_JOYDUAL2) ? 2 : 1; if (k > n) k = n;
    uint32_t dev0 = idx == SIX_IDX_JOYRIGHT;   // a right Joy-Con's sensor is device 1, as the SDK's (0x10007)
    for (int i = 0; i < k; i++) h[i] = idx | (*id & 0xff) << 8 | ((uint32_t)i + dev0) << 16;
    return k;
}
HLE_FORCE("_ZN2nn3hid18StartSixAxisSensorERKNS0_19SixAxisSensorHandleE", "six-axis sensor; always sampling on the host", void, h_hid_SixStart, const uint32_t *h) { (void)h; }
// The Switch 2's mouse: the Joy-Con 2's mouse sensor (JCMS) and the pointer on top of it (Nmouse). The SDK opens the hid service
// itself in InitializeJcms and InitializeNmouse, which the host does not serve; this game references neither, so it never sets the sensors up and no Joy-Con's sensor reports. The calls
// it does reference answer as a console with no such sensor: no pointer handles and no mouse types (a game skips its mouse code on those answers), nothing to start, stop or release.
// A Jcms handle is the npad id in byte 0 and the JcmsType in byte 1 (0 the left Joy-Con, 1 the right; sdk hid::utility::MakeJcmsHandle)
HLE_FORCE("_ZN2nn3hid14FinalizeNmouseEv", "merged mouse: nothing to release", void, h_hid_NmouseFini, void) {}
HLE_FORCE("_ZN2nn3hid16GetNmouseTypeSetEPNS_4util10BitFlagSetILi32ENS0_10NmouseTypeEEENS0_12NmouseHandleE", "mouse types: none", int, h_hid_NmouseTypes, uint32_t *out, uint64_t h) { (void)h; *out = 0; return 1; }
HLE_FORCE("_ZN2nn3hid16GetNmouseHandlesEPNS0_12NmouseHandleEij", "Joy-Con mouse pointer handles: none, no Joy-Con's sensor reports", int, h_hid_NmouseHandles, void *out, int n, uint32_t id) { (void)out; (void)n; (void)id; return 0; }
HLE_FORCE("_ZN2nn3hid12FinalizeJcmsEv", "Joy-Con mouse sensor: nothing to release", void, h_hid_JcmsFini, void) {}
HLE_FORCE("_ZN2nn3hid13GetJcmsHandleERKjRKNS0_8JcmsTypeE", "Joy-Con mouse sensor handle: npad id | type << 8, as the SDK makes it",
          uint32_t, h_hid_JcmsHandle, const uint32_t *id, const uint32_t *type) { return (*id & 0xff) | (*type & 0xff) << 8; }
HLE_FORCE("_ZN2nn3hid9StartJcmsERKNS0_10JcmsHandleE", "Joy-Con mouse sensor: no sensor on the host", void, h_hid_JcmsStart, const uint32_t *h) { (void)h; }
HLE_FORCE("_ZN2nn3hid8StopJcmsERKNS0_10JcmsHandleE", "Joy-Con mouse sensor: no sensor on the host", void, h_hid_JcmsStop, const uint32_t *h) { (void)h; }

// Npad IMU (Switch 2 SDKs): the six-axis samples the SDK's own sensor fusion reads (Enable/UpdateSensorFusion → GetNpadImuSamples → nn::imu::FullSensorState). EnableNpadImu gives the
// hid service the game's buffer as transfer memory, which the host does not serve: Nintendo Switch Sports Resort aborted in InitializeSensorFusion (Result 0xe15, sdk+0x6d6948). These four
// calls are the only users of the SDK's IMU state (sdk+0x1056c48), so the host keeps that state here: on with the game's buffer, which no service writes; GetNpadImuSamples writes the
// samples straight into the caller's array instead (below)
#define RES_NPAD_IMU_ENABLED 0x2908cau   // what the SDK's EnableNpadImu answers while it is already on (sdk+0x6ca600)
static pthread_mutex_t g_imu_lock = PTHREAD_MUTEX_INITIALIZER;
static void *g_imu_buf;
static int g_imu_on;   // its own flag: the state does not hang on the buffer pointer
HLE_FORCE("_ZN2nn3hid13EnableNpadImuEPvm", "npad IMU on with the game's buffer; no hid service writes it", Result, h_hid_ImuEnable, void *buf, uint64_t size) {
    (void)size; pthread_mutex_lock(&g_imu_lock);
    Result r = g_imu_on ? RES_NPAD_IMU_ENABLED : 0;
    if (!r) { g_imu_buf = buf; g_imu_on = 1; }
    pthread_mutex_unlock(&g_imu_lock);
    return r;
}
HLE_FORCE("_ZN2nn3hid14DisableNpadImuEv", "npad IMU off", Result, h_hid_ImuDisable, void) {
    pthread_mutex_lock(&g_imu_lock); g_imu_buf = NULL; g_imu_on = 0; pthread_mutex_unlock(&g_imu_lock);
    return 0;
}
HLE_FORCE("_ZN2nn3hid16IsNpadImuEnabledEv", "npad IMU state as the host keeps it", int, h_hid_ImuIs, void) {
    pthread_mutex_lock(&g_imu_lock); int on = g_imu_on; pthread_mutex_unlock(&g_imu_lock);
    return on;
}
// The handle is the six-axis sensor handle (UpdateSensorFusion passes its SensorFusionHandle to both, sdk+0x6d6ea0 and +0x6d7078): byte 1 the npad id, byte 2 the device index. The SDK
// accepts npads No1..No8, Other and Handheld (else 0x58aca) and devices 0..2 (else 0xd6ca: VerifyValidSixAxisSensorHandle, sdk+0x6d8440). A refused handle gets a red line naming the
// call (who), except from GetNpadImuSamples, which the fusion calls each update only after that check of its own (sdk+0x6d6ff0)
#define RES_INVALID_SIX_DEVICE 0xd6cau
enum { SIX_DEVICES = 3, SIX_NPADS = 10 };   // npad_id_slot's ten npads
static Result six_handle_slot(const char *who, uint32_t h, int *slot, int *dev) {
    *slot = npad_id_slot(h >> 8 & 0xff); *dev = (int)(h >> 16 & 0xff);
    Result r = *slot < 0 ? RES_INVALID_NPAD_ID : *dev >= SIX_DEVICES ? RES_INVALID_SIX_DEVICE : RES_OK;
    if (r != RES_OK && who) LOG("[RED] hid %s: six-axis handle %#x refused (%#x)", who, h, r);
    return r;
}
// NpadImuSample (0x28, sdk+0x6ca8b8) as UpdateSensorFusion reads it: sampling number, ns since the previous sample, acceleration in m/s^2 at +0x10, angular velocity in rad/s at
// +0x1c. UpdateSensorFusion copies +0x10..+0x18 negated (fneg at sdk+0x6d7334, +0x6d734c, +0x6d7350) to nn::imu's SixAxisSensorSample +0x24 and +0x1c..+0x24 unchanged to its +0x18
// (sdk+0x6d7320..+0x6d7354); nerd's FeedSixAxisSensorValues (sdk+0x6ffac0) passes sample +0x24 on as the acceleration (GyroPointing::FeedImuSampleNoUpdate, sdk+0x700e90, takes off
// its offset in cm/s^2) and +0x18 as the angular velocity (sdk+0x700f84, less the bias). So the fusion's acceleration is the sample's negated, a specific force (+9.8 m/s^2 up for a
// controller lying face up), and its angular velocity the sample's own; it reports them as m/s^2 x (-1 / 9.80665) G (sdk+0x6d74a0) and rad/s x 57.2958 deg/s (sdk+0x6d74c8), the
// six-axis state's own signs. The host's six-axis state (acceleration in G, (0, 0, -1) at rest; angular velocity in revolutions / s, right-handed in the same axes) becomes acc =
// G x 9.80665 and gyro = rev/s x 2 pi. Before, the host wrote the angular velocity at +0x10 and the acceleration at +0x1c, each negated: the fusion took gravity (9.8 m/s^2) for an
// angular velocity of 9.8 rad/s, so its stable orientation turned about the vertical at 562 deg/s (probed at sdk+0x6d7118, GetUnbiasedAngularVelocity z = 561.9 deg/s with a flat
// controller) and the calibration never completed. DM_IMU_ACC_SIGN / DM_IMU_GYRO_SIGN = -1 negate either, for a test
// Samples are the sensor's 5 ms ticks on the guest clock (SIX_PERIOD_NS), each the six-axis state at that time (live motion, else flat with player 1's shake). As the console's ring in
// shared memory, the answer keeps no per-reader state: a call returns the newest min(n, 16) ticks up to the current one (16: the SDK's ring, EnableNpadImu's buffer holds 16 x 0x28 per
// device), oldest first, numbered by absolute tick + 1 (always >= 1). A tick a reader already took comes again in its next call and the fusion's skip rule drops it; ticks it read too
// late for are skipped numbers, a gap the fusion bridges or resets on. So several readers, or a fusion created again (or the IMU switched off and on), each get every tick, and a reader
// that read the clock before another (an older current tick) only gets numbers the fusion has taken or will skip. Off, or for a handle whose controller is not connected in that style,
// there are none. A device index of 2 (accepted by the SDK's check) reads the motion of index 0 for a Joy-Con pair, as six_source masks it to the pair's two sides
typedef struct { int64_t sampling, delta_ns; float acc[3], gyro[3]; } NpadImuSample;
_Static_assert(sizeof(NpadImuSample) == 0x28, "NpadImuSample");
enum { IMU_RING = 16 };
#define STANDARD_GRAVITY 9.80665
static float g_imu_acc_sign = 1, g_imu_gyro_sign = 1;
#define IMU_NOISE_DPS 0.1      // the default noise (see imu_sample)
#define IMU_NOISE_G 0.00005
#define IMU_DITHER_SCALE 0.1     // held live ticks: a tenth of the host noise (0.01 deg/s, 0.005 mG), under a real controller's floor; DM_IMU_NOISE scales or removes it too
static motion_map g_imu_axes = {{0, 1, 2}, {1, 1, 1}, 1, 1};   // DM_IMU_AXES (imu_sample)
static double g_imu_noise_g = -1, g_imu_noise_dps = -1;   // DM_IMU_NOISE (imu_sample): the spreads in use; set by imu_env_read
static pthread_once_t g_imu_once = PTHREAD_ONCE_INIT;
static int64_t live_delay_env(const char *name, int64_t dflt) {   // a delay switch: -1 auto, else ns; dflt when unset or not valid (with a log line)
    const char *e = getenv(name); double f; char tail;
    if (!e || !*e) return dflt;
    if (!strcmp(e, "auto")) return -1;
    if (sscanf(e, "%lf%c", &f, &tail) == 1 && f >= 0 && f * 1e6 <= (double)LIVE_DELAY_MAX_NS) return (int64_t)(f * 1e6);
    LOG("[RED] %s=%s is neither auto nor 0 to %llu ms: %s", name, e, (unsigned long long)(LIVE_DELAY_MAX_NS / 1000000), dflt < 0 ? "auto" : "none");
    return dflt;
}
static void imu_env_read(void) {
    const char *e;
    g_imu_acc_sign = (e = getenv("DM_IMU_ACC_SIGN")) && atof(e) < 0 ? -1 : 1;
    g_imu_gyro_sign = (e = getenv("DM_IMU_GYRO_SIGN")) && atof(e) < 0 ? -1 : 1;
    // DM_IMU_NOISE: "<deg/s>,<mG>" sets both spreads (for tuning); a single number scales the defaults (0 none, 1 the defaults); anything else keeps the defaults, with a log line
    double nd = IMU_NOISE_DPS, ng = IMU_NOISE_G * 1000, f; char tail;
    if ((e = getenv("DM_IMU_NOISE")) && *e) {
        double a, b;
        if (sscanf(e, "%lf,%lf%c", &a, &b, &tail) == 2 && a >= 0 && a < 100 && b >= 0 && b < 100) { nd = a; ng = b; }
        else if (!strchr(e, ',') && sscanf(e, "%lf%c", &f, &tail) == 1 && f >= 0 && f < 1000) { nd *= f; ng *= f; }
        else LOG("[RED] DM_IMU_NOISE=%s is neither <deg/s>,<mG> nor a scale (0 to 1000): the default noise (%g deg/s, %g mG)", e, nd, ng);
    }
    g_imu_noise_dps = nd; g_imu_noise_g = ng / 1000;
    // DM_IMU_AXES=<map> ("x,-z,y"): a signed permutation of the axes of every IMU sample the game gets, acceleration and rotation alike (rotation times its determinant, so
    // a mirror stays a proper rotation), for trying another frame with a real controller without a rebuild; default x,y,z
    if (!motion_map_parse(&g_imu_axes, getenv("DM_IMU_AXES"))) LOG("[RED] DM_IMU_AXES=%s is not like \"x,-z,y\": x,y,z", getenv("DM_IMU_AXES"));
    // DM_IMU_DELAY (GetNpadImuSamples, default auto) and DM_SIX_DELAY (GetSixAxisSensorStates, default 0): a live stream's playout delay (six_source); "auto" the stream's
    // own (motion_lag), 0 none (ticks past the newest sample hold it), <ms> fixed
    atomic_store(&g_live_delay_cfg[1], live_delay_env("DM_IMU_DELAY", -1));
    atomic_store(&g_live_delay_cfg[0], live_delay_env("DM_SIX_DELAY", 0));
}
// Sensor noise. A real IC's samples are never all alike, and the SDK's fusion (NERD's gyro_pointing, sdk+0x700e60) is built for that. With samples that do not change at all
// (the host's flat controller), the fusion reset itself several times a second (its initialization, which asks GetSixAxisSensorIcInformation, ran 612 times in one Nintendo Switch
// Sports Resort session) and never calibrated its gyro bias (StoreSixAxisSensorCalibrationParameter never called), so the game's "place the Joy-Con on a flat surface" calibration
// failed. The noise has two bounds. The gyro's must be there: the fusion takes its bias only once the Kalman filter's bias deviation is under 0.01 rad/s on x and z
// (GyroPointingKalmanFilter::GetCalibrationState, thresholds 0.002 / 0.01 at sdk+0xbb85b8 / +0xbb7df8; IsGyroBiasCalibrationReady with axes 5, sdk+0x6d745c). The acceleration's
// must stay small: the stillness detector (GyroPointingStateDetector::UpdateMovingDetection, sdk+0x736260) calls the controller moving when the lag-1 autocovariance of the last 64
// accelerations on x and z (m/s^2) is beyond 1e-6 (threshold 0.001 squared, detector +0x11ec), and with 2 mG of white noise it read 5e-5: never still, never calibrated. So the gyro
// carries 0.1 deg/s (about one step of the IC at 2000 deg/s) and the acceleration 0.05 mG (under one step at 8 G, 0.24 mG), Gaussian, per axis: IMU_NOISE_DPS and IMU_NOISE_G. It is
// a function of the tick, the device and the axis (a hash), so a tick delivered again reads the same, and two Joy-Cons differ. DM_IMU_NOISE=0 leaves it out.
// A live controller carries its own noise, so its interpolated ticks are left as they are; a tick with no change of its own (live_at: held past the newest sample or before the
// oldest, a single sample, or between two samples that read alike) gets a tenth of it (IMU_DITHER_SCALE: 0.01 deg/s, 0.005 mG), so the game never reads a run of identical ticks.
// Held ticks are most of them: the game reads up to now and a sample lands only every 30 ms (Joy-Con 2) or 15 ms (DualSense), so in the first real-controller session 76 to 79% of
// the Joy-Con 2's ticks were held, in runs of 4 to 9, and 58% of the DualSense's
// (IMU_NOISE_DPS / IMU_NOISE_G are defined above, at the IMU state)
// On by default: the fusion is the SDK's own code, so every Switch 2 title on this SDK runs into it, and the noise touches only GetNpadImuSamples (GetSixAxisSensorStates keeps exact values)
static uint64_t imu_mix(uint64_t x) {   // splitmix64's finalizer
    x += 0x9e3779b97f4a7c15ull; x = (x ^ (x >> 30)) * 0xbf58476d1ce4e5b9ull; x = (x ^ (x >> 27)) * 0x94d049bb133111ebull; return x ^ (x >> 31);
}
static double imu_gauss(int64_t tick, uint32_t device, int channel) {   // a standard normal value, fixed for (tick, device, channel)
    uint64_t h = imu_mix((uint64_t)tick * 6 + (uint64_t)channel ^ imu_mix(device));
    double u1 = ((h >> 11) + 1) * (1.0 / 9007199254740993.0), u2 = (imu_mix(h) >> 11) * (1.0 / 9007199254740992.0);
    return sqrt(-2 * log(u1)) * cos(2 * M_PI * u2);
}
static void imu_env_reread(void) { pthread_once(&g_imu_once, imu_env_read); imu_env_read(); }   // DM_CTL "env DM_IMU_…" (a race with a sample being made only mixes two settings for one tick)
static int imu_sample(NpadImuSample *o, int64_t tick, const live_hist *src, int shakes, uint32_t device) {   // returns whether the tick held a live sample
    SixAxisState s; int held = six_at(&s, tick, src, shakes);
    o->sampling = tick + 1; o->delta_ns = SIX_PERIOD_NS;
    for (int i = 0; i < 3; i++) {
        // The host's own samples get the full noise. A live controller's carry their own (a Joy-Con 2 at rest: 0.4..0.7 mG, 0.04..0.06 deg/s; a DualSense 3..5 mG, 0.05..0.2
        // deg/s) and keep it untouched where they are interpolated; a tick that holds a sample (past the newest: a stalled report, or DM_IMU_DELAY=0, when a 33 Hz Joy-Con 2
        // holds about six ticks; a single-sample history) or lies between two that read alike would repeat the same values, which the fusion is not made for: it gets a dither
        double fg = !src->n ? 1 : held ? IMU_DITHER_SCALE : 0;
        double na = fg > 0 && g_imu_noise_g > 0 ? fg * g_imu_noise_g * imu_gauss(tick, device, i) : 0, ng = fg > 0 && g_imu_noise_dps > 0 ? fg * g_imu_noise_dps / 360 * imu_gauss(tick, device, 3 + i) : 0;
        o->acc[i] = (float)(g_imu_acc_sign * (s.acc[i] + na) * STANDARD_GRAVITY);
        o->gyro[i] = (float)(g_imu_gyro_sign * (s.gyro[i] + ng) * 2 * M_PI);
    }
    float a[3] = {o->acc[0], o->acc[1], o->acc[2]}, g[3] = {o->gyro[0], o->gyro[1], o->gyro[2]};
    motion_map_apply(&g_imu_axes, a, o->acc, 1); motion_map_apply(&g_imu_axes, g, o->gyro, g_imu_axes.det);
    return src->n && held;
}
// The IMU handle is the SDK's own SixAxisSensorHandle (GetNpadImuHandles → utility::GetSixAxisSensorHandle), in the numbering six_style_bit reads
HLE_FORCE("_ZN2nn3hid17GetNpadImuSamplesEPNS0_13NpadImuSampleEiNS0_13NpadImuHandleE", "npad IMU samples: the device's six-axis state at the newest 5 ms ticks, oldest first",
          int, h_hid_ImuSamples, void *out, int n, uint32_t imu_handle) {
    pthread_once(&g_imu_once, imu_env_read);
    int slot, dev;
    if (!out || n <= 0 || six_handle_slot(NULL, imu_handle, &slot, &dev) != RES_OK || !six_connected(imu_handle) || !h_hid_ImuIs()) return 0;
    static _Thread_local live_hist src;
    int64_t now = answer_tick(&g_hwm_imu, imu_handle, (int64_t)(guest_ns() / SIX_PERIOD_NS)), k = n < IMU_RING ? n : IMU_RING;
    if (k > now + 1) k = now + 1;   // no tick before the guest clock's start
    if (k <= 0) return 0;
    int shakes; unsigned live = six_source(imu_handle, (uint64_t)(now - k + 1) * SIX_PERIOD_NS, &src, &shakes, &g_hwm_imu);
    NpadImuSample *o = out;
    int held = 0; for (int64_t i = 0; i < k; i++) held += imu_sample(&o[i], now - k + 1 + i, &src, shakes, imu_handle >> 8 & 0xffff);
    static _Atomic uint64_t out_count;
    if (k > 0 && imu_log_due(&out_count))
        LOG("imu out h=%#x k=%lld s=%lld..%lld acc %+.4f %+.4f %+.4f m/s2 gyro %+.4f %+.4f %+.4f rad/s %s (%d held, delay %.1f ms)", imu_handle, (long long)k, (long long)o[0].sampling, (long long)o[k - 1].sampling,
            o[k - 1].acc[0], o[k - 1].acc[1], o[k - 1].acc[2], o[k - 1].gyro[0], o[k - 1].gyro[1], o[k - 1].gyro[2], live ? "live" : "synthetic", held, (double)src.delay / 1e6);
    return (int)k;
}
// The fusion's initialization (sdk+0x6d7db0) asks GetSixAxisSensorIcInformation through the hid service (IHidServer +0x178, sdk+0x6c3768), which the host does not serve: on a failure it
// latches its error flag (+0x579, sdk+0x6d7e40) and never reads a sample. It copies the 0xc8 bytes (sdk+0x6d7e58) and builds nn::imu::SixAxisSensorInformation from them
// (sdk+0x6d7f18, built at sdk+0x6fed30): for each sensor its range, a zero-offset bound and sensitivity bounds (diagonal 1 ± e, cross-axis ± c), read at +0x00 / +0x64 (range, also the
// limit: a sample past twice the range resets the fusion), +0x10 / +0x74 (the offset's upper bound), +0x40 / +0xa4 (1 + e) and +0x44 / +0xa8 (c). The layout matches yuzu's
// SixAxisSensorIcInformation, whose values the host answers: 2000 deg/s and 8 G (the full scale of the Switch's six-axis IC), offsets within ±10 deg/s and ±0.0612 G, sensitivity
// 0.95..1.05 with ±0.003 cross-axis. The ranges are the Switch's; the bounds are yuzu's guesses, not read from a Switch 2 controller
typedef struct { float range, offset_min[3], offset_max[3], scale_min[9], scale_max[9]; } SixIcHalf;
typedef struct { SixIcHalf gyro, acc; } SixIcInformation;
_Static_assert(sizeof(SixIcInformation) == 0xc8, "SixAxisSensorIcInformation");
#define SIX_IC_SCALE_MIN {0.95f, -0.003f, -0.003f, -0.003f, 0.95f, -0.003f, -0.003f, -0.003f, 0.95f}
#define SIX_IC_SCALE_MAX {1.05f, 0.003f, 0.003f, 0.003f, 1.05f, 0.003f, 0.003f, 0.003f, 1.05f}
static const SixIcInformation SIX_IC_INFO = {
    .gyro = {2000.0f, {-10.0f, -10.0f, -10.0f}, {10.0f, 10.0f, 10.0f}, SIX_IC_SCALE_MIN, SIX_IC_SCALE_MAX},
    .acc = {8.0f, {-0.0612f, -0.0612f, -0.0612f}, {0.0612f, 0.0612f, 0.0612f}, SIX_IC_SCALE_MIN, SIX_IC_SCALE_MAX},
};
HLE_FORCE("_ZN2nn3hid29GetSixAxisSensorIcInformationEPNS0_26SixAxisSensorIcInformationERKNS0_19SixAxisSensorHandleE", "six-axis sensor IC: 2000 deg/s, 8 G (bounds as yuzu's)",
          Result, h_hid_SixIcInfo, void *out, const uint32_t *h) {
    int slot, dev; Result r = six_handle_slot("GetSixAxisSensorIcInformation", *h, &slot, &dev);
    if (r == RES_OK) memcpy(out, &SIX_IC_INFO, sizeof SIX_IC_INFO);
    return r;
}
// UpdateSensorFusion asks each update whether a new controller took the device (sdk+0x6d6f48), which resets the fusion; the SDK reads that from hid shared memory, which the host does
// not map (IsSixAxisSensorDeviceNewlyAssignedFromSharedMemory, sdk+0x6c1330), and clears it through the hid service (IHidServer +0x180, sdk+0x6c3870). The host's controllers keep
// their npads, so the answer is no and there is nothing to clear
HLE_FORCE("_ZN2nn3hid34IsSixAxisSensorDeviceNewlyAssignedERKNS0_19SixAxisSensorHandleE", "six-axis device newly assigned: no", unsigned, h_hid_SixNewly, const uint32_t *h) { (void)h; return 0; }
HLE_FORCE("_ZN2nn3hid39ResetIsSixAxisSensorDeviceNewlyAssignedERKNS0_19SixAxisSensorHandleE", "six-axis device newly assigned: nothing to clear", void, h_hid_SixNewlyReset, const uint32_t *h) { (void)h; }
// The fusion's gyro bias calibration (0x744 bytes, cleared at sdk+0x6d7f4c and +0x6d766c), kept by the hid service per device (IHidServer +0x170 Load, +0x168 Store). The fusion
// stores it once it has calibrated the bias at rest (sdk+0x6d7690, result unused) and loads it at initialization (sdk+0x6d7f64): on success it takes the parameter's first three floats
// as its initial gyro bias (FullSensorState::SetGyroscopeBiasInitialGuess, sdk+0x6d7f7c) and marks the bias calibrated (+0x538), which UpdateSensorFusion then reports as the
// calibration being ready (sdk+0x6d7464) and IsSensorFusionCalibrated reads. A console's controllers come with a stored calibration (their factory one, or the last the fusion
// stored); the host's controllers are ideal sensors whose bias is zero, so a device with nothing stored this run answers that: zeros, a zero bias. It used to fail (0xd8ca), and
// the fusion had to learn the bias itself, which it never finished on the host: its Kalman filter's parameter test kept failing and resetting the bias estimate
// (GyroPointingCore::Recover, sdk+0x712aa0, counted at core +0x3cc8: 10 to 25 times a minute), so Nintendo Switch Sports Resort's "place the Joy-Con on a flat surface"
// calibration failed after 30 s every time and the game warned that controls may lag. It still does: the fusion's stable orientation diverges at rest every 8 to 20 s, and that reset
// (sdk+0x6d7700) initializes it again without loading this (sdk+0x6d773c), and once the controller is still the flag is overwritten by the Kalman filter's own readiness
// (sdk+0x6d7460), which never comes; why the filter does not converge on the host's samples is open (docs/reference/services.md)
#define RES_SIX_CAL_NONE (202u | 108u << 9)   // hid 202-108: nothing stored for that device
enum { SIX_CAL_SIZE = 0x744 };
static pthread_mutex_t g_six_cal_mu = PTHREAD_MUTEX_INITIALIZER;
static uint8_t g_six_cal[SIX_NPADS][SIX_DEVICES][SIX_CAL_SIZE], g_six_cal_have[SIX_NPADS][SIX_DEVICES];
// One sensor, one stored parameter: the SDK names a full-key or handheld controller's sensor device 2 where the host's handles name it device 0, so device 2 keeps its parameter
// under device 0 (a store through either handle is what a load through the other reads)
static int six_cal_dev(int dev) { return dev == 2 ? 0 : dev; }
HLE_FORCE("_ZN2nn3hid37LoadSixAxisSensorCalibrationParameterEPNS0_33SixAxisSensorCalibrationParameterERKNS0_19SixAxisSensorHandleE",
          "six-axis calibration: what the game stored this run, else zero bias for a sensor whose offset is taken off (else none)", Result, h_hid_SixCalLoad, void *out, const uint32_t *h) {
    int slot, dev; Result r = six_handle_slot("LoadSixAxisSensorCalibrationParameter", *h, &slot, &dev);
    if (r != RES_OK) return r;
    int known = six_offset_known(*h);
    memset(out, 0, SIX_CAL_SIZE);   // written whatever the answer; the SDK's only caller (fusion init, sdk+0x6d7f64) tests the Result for 0 and reads nothing on a failure
    pthread_mutex_lock(&g_six_cal_mu);
    dev = six_cal_dev(dev);
    if (g_six_cal_have[slot][dev]) memcpy(out, g_six_cal[slot][dev], SIX_CAL_SIZE);
    else if (known) {}   // nothing stored: a sensor whose offset is taken off reads with zero bias (the zeros above)
    else r = RES_SIX_CAL_NONE;   // a GameController whose rest offset is not known yet: the fusion has to learn it (Switch Sports Resort then asks to calibrate). The host
    // answered this Result for every device before (Switch Sports Resort ran with it); the fusion's init only takes the bias on 0 (sdk+0x6d7f68: cset eq, cbnz skips it)
    pthread_mutex_unlock(&g_six_cal_mu);
    return r;
}
HLE_FORCE("_ZN2nn3hid38StoreSixAxisSensorCalibrationParameterERKNS0_19SixAxisSensorHandleERKNS0_33SixAxisSensorCalibrationParameterE",
          "six-axis calibration: kept for the run", Result, h_hid_SixCalStore, const uint32_t *h, const void *in) {
    int slot, dev; Result r = six_handle_slot("StoreSixAxisSensorCalibrationParameter", *h, &slot, &dev);
    if (r != RES_OK) return r;
    pthread_mutex_lock(&g_six_cal_mu);
    dev = six_cal_dev(dev);
    memcpy(g_six_cal[slot][dev], in, SIX_CAL_SIZE); g_six_cal_have[slot][dev] = 1;
    pthread_mutex_unlock(&g_six_cal_mu);
    return RES_OK;
}
HLE_FORCE("_ZN2nn3hid22GetSixAxisSensorStatesEPNS0_18SixAxisSensorStateEiRKNS0_19SixAxisSensorHandleE", "six-axis sensor states: every requested entry, newest first",
          int, h_hid_SixStates, SixAxisState *out, int n, const uint32_t *h) { return six_states(out, n, *h); }
HLE_FORCE("_ZN2nn3hid20FinalizeSensorFusionEv", "end of sensor fusion (SDK 22): the host keeps no IMU state to disable", void, h_hid_FusionFin, void) {}
HLE_FORCE("_ZN2nn3hid25SetGyroscopeZeroDriftModeERKNS0_19SixAxisSensorHandleERKNS0_22GyroscopeZeroDriftModeE", "gyroscope zero-drift mode; ignored by the host", void, h_hid_GyroSet, const uint32_t *h, const int *m) { (void)h; (void)m; }
HLE_FORCE("_ZN2nn3hid41IsFirmwareUpdateAvailableForSixAxisSensorERKNS0_19SixAxisSensorHandleE", "six-axis sensor firmware update: none", unsigned, h_hid_SixFw, const uint32_t *h) { (void)h; return 0; }
// Vibration handles: byte 0 = SDK style index (FullKey 3, Handheld 4), byte 1 = npad id, byte 2 = motor.
// See libnx hid.c _hidGetVibrationDeviceHandles. Only the styles this host connects are exposed.
// A Joy-Con style's handle (type 5 JoyDual, 6 JoyLeft, 7 JoyRight) reaches the pad's motor of its Joy-Con: the left one's is motor 0, the right one's motor 1, whichever npad the right one is on
// The players a vibration handle names, before its haptics output is asked about: the handle bytes are
// checked and the npad's style decides the rest. A game asks whether a channel is mounted once, often
// at boot: on the console the controller is there then, while a Bluetooth pad here may connect seconds
// later, so mounted and GetVibrationDeviceInfo answer from this; sends keep only the players that have
// a routed output (vib_targets).
static uint32_t vib_valid(uint32_t h, int *motor) {
    unsigned type = h & 0xff, id = h >> 8 & 0xff, dev = h >> 16 & 0xff;
    *motor = (int)(dev & 1);
    if (h >> 24 || dev > 1 || type < 3 || type > 7) return 0;
    npad_ref r = npad_resolve(id);
    int p = player_of(id);
    uint32_t pad_players = p == 0 ? 1u | merged_players() : p > 0 ? 1u << p : 0;   // a whole controller's motors: player 1's merged controllers too
    if (type >= 5) {
        if (type == 5 && r.kind == KIND_PAD && kind_style(r, id) == STYLE_JOYDUAL) return pad_players;   // a controller shown as a pair (pad_style)
        if (r.kind != (type == 5 ? KIND_DUAL : type == 6 ? KIND_LEFT : KIND_RIGHT) || (type != 5 && dev)) return 0;
        if (type != 5) *motor = type == 7;
        return 1u << r.pad;
    }
    if (type > 4) return 0;
    // A full-key handle on an npad that now holds Joy-Cons still reaches them: a game keeps the handles it got before the Joy-Cons connected (Super Mario Odyssey asks for the
    // JoyDual handles when the pair connects, but keeps sending to its full-key ones). Its motor is the Joy-Con of that side; a single Joy-Con takes the handle of its side
    if (p >= 0 && r.kind >= KIND_DUAL && type == 3 && id != NPAD_HANDHELD) {
        if (r.kind == KIND_LEFT && dev) return 0;
        if (r.kind == KIND_RIGHT && !dev) return 0;
        return 1u << r.pad;
    }
    // A controller shown as a pair because the game supports no full-key style keeps the full-key handles it got before the game changed its styles, as a pair's Joy-Cons do
    uint32_t st = kind_style(r, id);
    if (p < 0 || r.kind != KIND_PAD || (st != (1u << (type - 3)) && !(type == 3 && st == STYLE_JOYDUAL))) return 0;
    return pad_players;
}
static uint32_t vib_targets(uint32_t h, int *motor) {
    return vib_valid(h, motor) & dm_haptics_available();
}
static void vib_send(const uint32_t *handles, const dm_vibration *values, int n) {
    if (!handles || !values || n <= 0) return;
    dm_vibration_update updates[DM_PLAYERS * 2];
    uint32_t used = 0; int unrouted = 0;
    for (int i = 0; i < n; i++) {
        int m; uint32_t targets = vib_targets(handles[i], &m);
        if (!targets) unrouted++;
        for (int p = 0; p < DM_PLAYERS; p++) if (targets >> p & 1) {
            int k = p * 2 + m;
            updates[k] = (dm_vibration_update){p, m, values[i]}; used |= 1u << k;
        }
    }
    int count = 0;
    for (int k = 0; k < DM_PLAYERS * 2; k++) if (used >> k & 1) updates[count++] = updates[k];
    if (getenv("DM_RUMBLE_LOG")) {   // Count input values before deduplicating motors, including partially routed batches.
        static pthread_mutex_t log_lock = PTHREAD_MUTEX_INITIALIZER;
        static uint64_t sends, dropped, t0; static float peak; struct timespec ts;
        pthread_mutex_lock(&log_lock);   // SDK sends can arrive from several game threads.
        clock_gettime(CLOCK_MONOTONIC, &ts);
        uint64_t now = ts.tv_sec; sends += n; dropped += unrouted;
        for (int i = 0; i < n; i++) peak = fmaxf(peak, fmaxf(values[i].amplitude_low, values[i].amplitude_high));
        if (now != t0) { LOG("rumble: %llu values sent, %llu with no output, peak amplitude %.2f, handle %#x", sends, dropped, peak, handles[0]); sends = dropped = 0; peak = 0; t0 = now; }
        pthread_mutex_unlock(&log_lock);
    }
    dm_haptics_send(updates, count);
}
// Games can cache handles once at startup, before GameController routes the pad's haptics (Unicorn Overlord kept an empty answer and sent every value to handle 0).
// On Horizon handles are computed from npad id and style (libnx _hidGetVibrationDeviceHandles); vib_send checks for an output at each send. With rumble off they are handed out too,
// as on a Switch whose vibration setting is off (IsVibrationPermitted answers false and nothing is felt), so turning it on while the game runs works in a game that asked only once.
HLE_FORCE("_ZN2nn3hid25GetVibrationDeviceHandlesEPNS0_21VibrationDeviceHandleEiRKjNS_4util10BitFlagSetILi32ENS0_12NpadStyleTagEEE", "vibration devices of the connected controller",
          int, h_hid_VibHandles, uint32_t *h, int n, const uint32_t *id, uint32_t style) {
    if (!h || !id || n <= 0 || !(style & style_of(*id)) || !dm_haptics_has_output()) {
        if (getenv("DM_RUMBLE_LOG") && id) LOG("rumble: game asked for vibration handles of npad %u style %#x: none (connected style %#x, %s)", *id, style, style_of(*id), dm_haptics_has_output() ? "output ready" : "no output yet");
        return 0;
    }
    npad_ref r = npad_resolve(*id);
    uint32_t st = kind_style(r, *id);   // the handle's type follows the reported style (a whole controller may be shown as a pair, pad_style)
    uint32_t type = st & STYLE_JOYDUAL ? 5u : st & STYLE_JOYLEFT ? 6u : st & STYLE_JOYRIGHT ? 7u : st & STYLE_HANDHELD ? 4u : 3u;
    uint32_t base = (*id << 8) | type;
    int count = n < (r.kind == KIND_LEFT || r.kind == KIND_RIGHT ? 1 : 2) ? n : (r.kind == KIND_LEFT || r.kind == KIND_RIGHT ? 1 : 2);
    for (int i = 0; i < count; i++) h[i] = base | (uint32_t)i << 16;
    if (getenv("DM_RUMBLE_LOG")) LOG("rumble: game asked for vibration handles of npad %u style %#x: %d, first %#x", *id, style, count, h[0]);
    return count;
}
HLE_FORCE("_ZN2nn3hid25InitializeVibrationDeviceERKNS0_21VibrationDeviceHandleE", "vibration: initialize one channel", void, h_hid_InitVib, const uint32_t *h) {
    const dm_vibration idle = {0, 160, 0, 320}; vib_send(h, &idle, 1);
}
HLE_FORCE("_ZN2nn3hid18SendVibrationValueERKNS0_21VibrationDeviceHandleERKNS0_14VibrationValueE", "vibration: update one channel", void, h_hid_SendVib, const uint32_t *h, const dm_vibration *v) { vib_send(h, v, 1); }
// Values queued by the vibration graph (VibrationNode::Update) reach it from the sdk's internal FlushEnqueuedVibrationValues through the PLT, into the hid service proxy (sm abort). Metroid Dread
// sets handles on its VibrationTargets even when the host reports 0 devices, so values get queued; return success and the flush clears its count as usual
HLE_FORCE("_ZN2nn3hid6detail19SendVibrationValuesEPKNS0_21VibrationDeviceHandleEPKNS0_14VibrationValueEi", "vibration batch: update all channels atomically", Result, h_hid_SendVibs, const uint32_t *h, const dm_vibration *v, int n) { vib_send(h, v, n); return RES_OK; }
// Device info and mounting describe a valid channel of the npad's reported controller, independently of the rumble setting or a routed haptics output (vib_valid)
HLE_FORCE("_ZN2nn3hid22GetVibrationDeviceInfoEPNS0_19VibrationDeviceInfoERKNS0_21VibrationDeviceHandleE", "vibration: linear resonant actuator, left or right", void, h_hid_VibInfo, uint32_t *info, const uint32_t *h) {
    int m = 0, on = h && vib_valid(*h, &m) != 0;
    info[0] = on ? 1 : 0; info[1] = on ? 1 + m : 0;
    if (getenv("DM_RUMBLE_LOG")) { static _Atomic unsigned n; if (atomic_fetch_add(&n, 1) < 8) LOG("rumble: GetVibrationDeviceInfo %#x -> mounted %u, actuator %u", h ? *h : 0, info[0], info[1]); }
}
// The Switch 2's form of IsVibrationDeviceMounted (Kirby Air Riders asks soon after boot in a run with a window, main+0x10709b0; the SDK's own call reads the null hid service object)
HLE_FORCE("_ZN2nn3hid29IsOunceVibrationDeviceMountedERKNS0_21VibrationDeviceHandleE", "vibration (Switch 2): device mounted when the handle names a valid npad channel", unsigned, h_hid_VibMountedOunce, const uint32_t *h) {
    int m; return h && vib_valid(*h, &m);
}
// GetActual returns the sanitized guest value accepted by the host, before output-device adaptation.
HLE_FORCE("_ZN2nn3hid23GetActualVibrationValueEPNS0_14VibrationValueERKNS0_21VibrationDeviceHandleE", "vibration: last accepted channel value", void, h_hid_VibActual, dm_vibration *v, const uint32_t *h) {
    int m = 0; uint32_t targets = h ? vib_targets(*h, &m) : 0;
    *v = targets ? dm_haptics_actual(__builtin_ctz(targets), m) : (dm_vibration){0, 160, 0, 320};
}
// Mouse / keyboard: not connected
HLE_FORCE("_ZN2nn3hid15InitializeMouseEv", "mouse; not provided by the host", void, h_hid_InitMouse, void) {}
HLE_FORCE("_ZN2nn3hid14GetMouseStatesEPNS0_10MouseStateEi", "mouse states: 0 entries", int, h_hid_MouseStates, void *out, int n) { (void)out; (void)n; return 0; }
HLE_FORCE("_ZN2nn3hid18InitializeKeyboardEv", "keyboard; not provided by the host", void, h_hid_InitKbd, void) {}
HLE_FORCE("_ZN2nn3hid16GetKeyboardStateEPNS0_13KeyboardStateE", "keyboard state: not connected (KeyboardState 0x30 bytes zeroed)", void, h_hid_KbdState, void *s) {
    memset(s, 0, 0x30); ((int64_t *)s)[0] = atomic_fetch_add(&g_sampling, 1) + 1;
}
