// GameController names most pads' face buttons by position (Xbox), a Nintendo pad's by label: Nintendo pads map name for name, others by
// position (right A, bottom B, top X, left Y). Open: a single Joy-Con held sideways is untested.
#pragma once
#import <GameController/GameController.h>
#include <stdint.h>
#include "dm_input.h"

static inline int dm_pad_labelled(GCController *c) {
    NSString *k = c.productCategory;
    return [k isEqualToString:@"Switch Pro Controller"] || [k containsString:@"Joy-Con"];
}
static inline uint64_t dm_pad_face(GCExtendedGamepad *g) {
    GCControllerButtonInput *a = g.buttonA, *b = g.buttonB, *x = g.buttonX, *y = g.buttonY;
    if (!dm_pad_labelled(g.controller)) {   // by position
        GCControllerButtonInput *t = a; a = b; b = t;
        t = x; x = y; y = t;
    }
    return (a.isPressed ? DM_BTN_A : 0) | (b.isPressed ? DM_BTN_B : 0) | (x.isPressed ? DM_BTN_X : 0) | (y.isPressed ? DM_BTN_Y : 0);
}
