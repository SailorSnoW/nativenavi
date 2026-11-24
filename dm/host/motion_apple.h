// GCMotion to the six-axis sensor: acceleration in G (CoreMotion axes, face up = (0, 0, -1)), rotation in rad/s.
#pragma once
#import <GameController/GameController.h>
#include <stdlib.h>
#include "dm_input.h"

static inline void dm_motion_attach(GCController *c, int player) {
    const char *e = getenv("DM_MOTION");
    if (e && !atoi(e)) return;
    dm_motion_clear(player);
    GCMotion *mo = c.motion;
    if (!mo || !mo.hasRotationRate) return;
    if (mo.sensorsRequireManualActivation) mo.sensorsActive = YES;   // DualSense / DualShock 4: off until asked for
    __block int live = 0, primed = 0;
    __block GCRotationRate first;
    mo.valueChangedHandler = ^(GCMotion *m) {
        float acc[3], rot[3];
        if (m.hasGravityAndUserAcceleration) {
            acc[0] = (float)(m.gravity.x + m.userAcceleration.x);
            acc[1] = (float)(m.gravity.y + m.userAcceleration.y);
            acc[2] = (float)(m.gravity.z + m.userAcceleration.z);
        } else {
            acc[0] = (float)m.acceleration.x;
            acc[1] = (float)m.acceleration.y;
            acc[2] = (float)m.acceleration.z;
        }
        rot[0] = (float)m.rotationRate.x;
        rot[1] = (float)m.rotationRate.y;
        rot[2] = (float)m.rotationRate.z;
        // Some controllers report a gyroscope they lack: publish once a raw rotation differs from the first report.
        if (!live) {
            if (!primed) { primed = 1; first = m.rotationRate; return; }
            if (m.rotationRate.x == first.x && m.rotationRate.y == first.y && m.rotationRate.z == first.z) return;
            live = 1;
        }
        dm_motion_set(player, acc, rot);
    };
}
static inline void dm_motion_detach(GCController *c, int player) {
    GCMotion *mo = c.motion;
    if (mo) {
        mo.valueChangedHandler = nil;
        if (mo.sensorsRequireManualActivation) mo.sensorsActive = NO;
    }
    dm_motion_clear(player);
}
