// The game's own state, read from its GameDataMgr by the profile's map of it (gamedata.tsv, DM_GAME_GD_FIELDS and the others); for the Discord presence (presence.c).
#pragma once
#include <stdint.h>

enum { DM_GD_MENU_UNKNOWN = -1, DM_GD_MENU_NONE, DM_GD_MENU_TITLE, DM_GD_MENU_GAME_OVER, DM_GD_MENU_OTHER };
typedef struct {
    char place[65], map[65];   // the game's IDs ("KokiriForest", "KokiriForest_Child"), "" when unset or unread
    int child;                 // 1 child, 0 adult, -1 unread
    int paused;                // 1 the game's own pause, 0 not, -1 unread
    int life, max_life;        // quarter hearts, -1 unread
    int rupees;                // -1 unread
    int64_t play_time;         // seconds, -1 unread
    int menu;                  // DM_GD_MENU_*
} dm_gd_state;

// 1: the manager was found and its type entries checked (a field may still be unread); 0: not, *why says why
int dm_gd_read(dm_gd_state *s, const char **why);
// a guest read that fails instead of faulting (gamedata.c, mach_vm_read_overwrite)
int dm_gd_peek(uint64_t a, void *out, uint64_t n);
