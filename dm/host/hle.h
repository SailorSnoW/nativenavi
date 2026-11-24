// Host implementations registered by the Switch's mangled names.
// Switch code does not extend arguments narrower than 32 bits: declare them int / unsigned. Switch variadics are in registers: no C "...".
#pragma once
#include "dm.h"
#include "registration.h"
#include <stddef.h>
typedef struct { const char *name; void *fn; } hle_ent;
#define HLE(mangled, ret, cname, ...)                                                             \
    static ret cname(__VA_ARGS__);                                                                \
    DM_REGISTER("swhle", hle_ent, hle_ent_##cname, {mangled, (void *)cname}); \
    static ret cname(__VA_ARGS__)

typedef uint32_t Result;   // 0 = success
#define RES_OK 0u
#define RES_FAIL 0x2A2u
enum { DM_APPLET_RESOURCE_USER_ID = 0x80 };

// Call with the condition mutex held: 1 before waiting, 0 after reacquiring it.
void dm_thread_native_wait(int entering);
void dm_host_lock(pthread_mutex_t *m);  // releases the guest core while contended
void dm_thread_service_request(void);   // a blocking service request on Horizon: gives up the core
uint64_t dm_guest_frame(void);

// Forced takeover: every reference binds to the host, even where the SDK defines the symbol; an entry no module references fails the load.
// Runs on the thread's host stack (docs/reference/memory.md#host-calls-on-small-guest-stacks): ≤ 16 parameters (8 registers, 8 stack slots), no variadics.
typedef struct { const char *name; void *fn; const char *why; } hle_force_ent;
#define HLE_NARG(...) HLE_NARG_(__VA_ARGS__, 24, 23, 22, 21, 20, 19, 18, 17, 16, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0)
#define HLE_NARG_(a1, a2, a3, a4, a5, a6, a7, a8, a9, a10, a11, a12, a13, a14, a15, a16, a17, a18, a19, a20, a21, a22, a23, a24, n, ...) n
#define HLE_STR(x) HLE_STR_(x)
#define HLE_STR_(x) #x
// Past 8 parameters: x17 = stack argument bytes (8 each, rounded up to 16).
#define HLE_FORCE(mangled, why, ret, cname, ...) HLE_FORCE_IN("swforce", mangled, why, ret, cname, __VA_ARGS__)
// HLE_FORCE for an add-on module's imports only (the practice tools' subsdk9); unreferenced entries are not reported.
#define HLE_ADDON(mangled, why, ret, cname, ...) HLE_FORCE_IN("swaddon", mangled, why, ret, cname, __VA_ARGS__)
#define HLE_FORCE_IN(sect, mangled, why, ret, cname, ...)                                          \
    _Static_assert(HLE_NARG(__VA_ARGS__) <= 16, #cname ": more parameters than the big-stack entry carries over (hle.h)"); \
    static ret cname(__VA_ARGS__) __attribute__((used));                                          \
    extern const char cname##_bs[];                                                               \
    __asm__(".text\n.p2align 2\n_" #cname "_bs:\n" \
            "    adrp x16, _" #cname "@PAGE\n    add x16, x16, _" #cname "@PAGEOFF\n"              \
            ".if " HLE_STR(HLE_NARG(__VA_ARGS__)) " > 8\n    mov x17, #(((" HLE_STR(HLE_NARG(__VA_ARGS__)) " - 8) * 8 + 15) & ~15)\n" \
            "    b _dm_bigstack_enter_n\n.else\n    b _dm_bigstack_enter\n.endif\n");           \
    DM_REGISTER(sect, hle_force_ent, hle_force_##cname, {mangled, (void *)(uintptr_t)cname##_bs, why}); \
    static ret cname(__VA_ARGS__)
// HLE_FORCE on the caller's stack: only for a takeover running the game's own code (sized for its stack, may switch stacks). Say why.
#define HLE_FORCE_DIRECT(mangled, why, ret, cname, ...)                                            \
    static ret cname(__VA_ARGS__);                                                                \
    DM_REGISTER("swforce", hle_force_ent, hle_force_##cname, {mangled, (void *)cname, why}); \
    static ret cname(__VA_ARGS__)
// Weak pass-through for programs without hle_bigstack.c (unit tests).
__attribute__((weak, naked, visibility("hidden"))) void dm_bigstack_enter(void) { __asm__ volatile("br x16"); }
__attribute__((weak, naked, visibility("hidden"))) void dm_bigstack_enter_n(void) { __asm__ volatile("br x16"); }
// Pass __builtin_return_address(0) from a HLE_FORCE implementation.
void *dm_bigstack_caller(void *ra);
__attribute__((weak, visibility("hidden"))) void *dm_bigstack_caller(void *ra) { return ra; }

// 0 handheld / 1 docked
int dm_opmode(void);
void dm_set_opmode(int m);          // pushes notification 30 / 31
// 1 foreground / 2 focus lost / 3 background; a change pushes notification 15
void dm_focus_set(int st);
// First in a library applet stand-in: holds the game out of focus until it returns, as Horizon does. Not for stand-ins that fail.
void dm_applet_enter(const char *name);
void dm_applet_leave(void);
static inline void dm_applet_leave_(const char *const *name) { (void)name; dm_applet_leave(); }
#define DM_APPLET(name) const char *const dm_applet_ __attribute__((cleanup(dm_applet_leave_))) = (name); dm_applet_enter(dm_applet_)
extern uint8_t *dm_fps_state;   // +0 float target fps, +4 movie flag, +0x10 float render scale, +0x18 double seconds stepped; NULL when none
int dm_movie_hz(void);   // picture rate of the playing movie, 0 when none
void dm_movie_picture(const void *dec, int64_t ts_ns);
void dm_movie_stopped(const void *dec);
void dm_audio_suspend(int on);
// nn::audio::AudioOutBuffer in the SDK's layout (docs/reference/audio.md).
typedef struct { void *next; void *ptr; uint64_t cap, size, offset; } aout_buf;   // next, data, buffer_size, data_size, data_offset
_Static_assert(sizeof(aout_buf) == 40 && offsetof(aout_buf, ptr) == 0x08 && offsetof(aout_buf, cap) == 0x10 && offsetof(aout_buf, size) == 0x18 && offsetof(aout_buf, offset) == 0x20,
               "nn::audio::AudioOutBuffer layout");
int dm_audio_output_channels(void);   // 6 when the device takes six channels, else 2
int dm_audio_route(void);
// L R C LFE Ls Rs, then the stereo fold (made before clamping / resampling).
enum { DM_AUDIO_SINK_CHANNELS = 8 };
void *dm_pcmout_open_sink(void);
void dm_audio_device_changed(void);
