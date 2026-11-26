#pragma once
#include <stdlib.h>
#include <string.h>

// Workaround: an opaque CAMetalLayer in native fullscreen on a mirrored display flickers a purple band on M4 Pro and M5 Pro Macs, a defect of
// macOS's Direct-to-Display scanout; a non-opaque layer is composited by the window server instead (one frame more latency), so only there.
// DM_MAC_COMPOSITED=1 / 0 forces either: docs/reference/rendering.md#purple-band-flickers-in-native-fullscreen-on-mirrored-hdmi-displays
enum {
    FRONT_MAC_COMP_AUTO = 0,
    FRONT_MAC_COMP_OFF = -1,
    FRONT_MAC_COMP_ON = 1,
};

static inline int front_mac_composited_mode(const char *env) {
    if (!env || !*env || !strcmp(env, "auto")) return FRONT_MAC_COMP_AUTO;
    if (!strcmp(env, "0")) return FRONT_MAC_COMP_OFF;
    if (!strcmp(env, "1") || atoi(env) != 0) return FRONT_MAC_COMP_ON;
    return FRONT_MAC_COMP_AUTO;
}

static inline int front_mac_layer_opaque(int mode, int fullscreen, int mirrored) {
    if (mode == FRONT_MAC_COMP_ON) return 0;
    if (mode == FRONT_MAC_COMP_OFF) return 1;
    return !(fullscreen && mirrored);
}

static inline const char *front_mac_layer_opaque_why(int mode, int fullscreen, int mirrored) {
    if (mode == FRONT_MAC_COMP_ON) return "DM_MAC_COMPOSITED=1";
    if (mode == FRONT_MAC_COMP_OFF) return "DM_MAC_COMPOSITED=0";
    if (!fullscreen) return "window";
    return mirrored ? "fullscreen on a mirrored display" : "fullscreen on an unmirrored display";
}
