// Switch 2 controllers over Bluetooth LE (pad_switch2_apple.m; protocol in switch2_proto.h). Off unless DM_SW2=1, so no Bluetooth prompt appears otherwise;
// the Info.plist needs NSBluetoothAlwaysUsageDescription, without which macOS kills the process at its first scan.
// A pad is one player: a Pro Controller 2, a pair of Joy-Con 2, or one held sideways (s2_gesture).
#pragma once
#include "switch2_proto.h"
#include "hle_haptics.h"

typedef struct s2_pad { s2_pad_state st; char name[48]; struct s2_pad *next; } s2_pad;

// All calls come on the main queue.
typedef struct {
    int (*attach)(s2_pad *p);    // 1: kept (player() -1 until its first button press); 0: no free player, the controller is let go
    void (*detach)(s2_pad *p);   // already off the list; freed after the call
    void (*read)(s2_pad *p);
    int (*player)(s2_pad *p);    // 0-based, -1 without one
    int (*blocked)(void);        // no controller input now (focus lost, Home prompt); NULL: never
} dm_switch2_front;

int dm_switch2_start(const dm_switch2_front *front);   // 0: off (DM_SW2 not 1, or no Bluetooth usage description)
void dm_switch2_active(int on);                         // 0: the sensors stop driving the game's
s2_pad *dm_switch2_pads(void);                          // a list through next
// Each value the game sets plays once in its own 5 ms sample, three to a packet every 15 ms (switch2_proto.h, Rumble timing).
void dm_switch2_rumble(const dm_haptic_state *state);
