// Front end (keyboard and controllers) -> nn::hid input state.
#pragma once
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
// nn::hid::NpadButton bit order (DM_KEYS key names follow it)
enum {
    DM_BTN_A = 1u << 0, DM_BTN_B = 1u << 1, DM_BTN_X = 1u << 2, DM_BTN_Y = 1u << 3,
    DM_BTN_LS = 1u << 4, DM_BTN_RS = 1u << 5, DM_BTN_L = 1u << 6, DM_BTN_R = 1u << 7,
    DM_BTN_ZL = 1u << 8, DM_BTN_ZR = 1u << 9, DM_BTN_PLUS = 1u << 10, DM_BTN_MINUS = 1u << 11,
    DM_BTN_LEFT = 1u << 12, DM_BTN_UP = 1u << 13, DM_BTN_RIGHT = 1u << 14, DM_BTN_DOWN = 1u << 15,
    DM_BTN_LSL = 1u << 24, DM_BTN_LSR = 1u << 25, DM_BTN_RSL = 1u << 26, DM_BTN_RSR = 1u << 27,
};
#define DM_BTN_SHAKE (1ull << 63)   // not an NpadButton: shakes the six-axis sensor while set
#define DM_STICK_MAX 32767
// A round dead zone of `dead` (a share of the full deflection) around the stick's centre, the rest rescaled to start at 0,
// each axis clamped after, so square gates reach their corners
static inline void dm_stick_apply(float dead, float x, float y, int32_t *ox, int32_t *oy) {
    float r = sqrtf(x * x + y * y);
    if (r <= dead) { *ox = *oy = 0; return; }
    float k = (r - dead) / (1 - dead) / r;
    x *= k;
    y *= k;
    *ox = (int32_t)((x < -1 ? -1 : x > 1 ? 1 : x) * DM_STICK_MAX);
    *oy = (int32_t)((y < -1 ? -1 : y > 1 ? 1 : y) * DM_STICK_MAX);
}
// DM_PAD_DEADZONE: one round dead zone for every controller's sticks, as a share of the full deflection (0.05 to 0.9).
// 0, unset or not a number: no override, each input path keeps its own default (raw HID 0.12, GameController none, Switch 2 its calibration)
static inline float dm_pad_deadzone(void) {
    const char *e = getenv("DM_PAD_DEADZONE");
    if (!e) return 0;
    char *end;
    double v = strtod(e, &end);
    return *e && !*end && v > 0 && v < 0.95 ? (float)v : 0;
}
void dm_input_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
// Player 1 merges controller and keyboard / DM_KEYS input: buttons ORed, each stick axis from the larger deflection.
// A player whose npad the game does not support feeds player 1.
#define DM_PLAYERS 8   // player 1 is the Handheld npad in handheld mode
void dm_pad_set(uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void dm_pad_set_player(int player, uint64_t buttons, int32_t lx, int32_t ly, int32_t rx, int32_t ry);
void dm_pad_players(uint32_t mask);   // player 1 always counts as connected (the keyboard)
int dm_players_reported(void);   // as the controller applet counts them (a split Joy-Con pair as two); any thread
// acc in G ((0, 0, -1) face up), rot in rad/s, in Apple's controller frame
void dm_motion_set(int player, const float acc[3], const float rot[3]);
void dm_motion_clear(int player);
// Switch frame; sample_ns is the host time of the sample; GameController motion wins while fresh
void dm_motion_set_hid(int player, const float acc[3], const float rot[3], uint64_t sample_ns);
void dm_motion_clear_hid(int player);
// Pad p holds a left (sides bit 0) and / or right (bit 1) Joy-Con; sides 0 is a Pro-style pad again.
// A pair is one npad unless the controller applet splits it.
void dm_joy_attach(int pad, uint32_t sides);   // | DM_JOY_2: Joy-Con 2s
#define DM_JOY_2 4u
void dm_joy_set(int pad, int side, uint64_t buttons, int32_t sx, int32_t sy);   // side 0 left, 1 right
void dm_motion_set_joy(int pad, int side, const float acc[3], const float rot[3], uint64_t sample_ns);
void dm_motion_clear_joy(int pad, int side);
uint32_t dm_player_npad(int player);  // Handheld (0x20) or No1 (0) for player 1; No n is id n - 1
