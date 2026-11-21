// pctl offline policy (hle_misc.c): the hooks are looked up by their mangled names in the real swforce registrations, so a missing or misspelled
// takeover fails; no guest module, IPC session or system applet exists in this test
#include "../hle_misc.c"
void dm_applet_enter(const char *name) { (void)name; }   // hle_oe.c: the error stand-in holds the game out of focus
void dm_applet_leave(void) {}
#include <mach-o/getsect.h>

static int checks, fails;
extern const struct mach_header_64 _mh_execute_header;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *e = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; e && i < bytes / sizeof *e; i++) {
        if (strcmp(e[i].name, name)) continue;
        count++; fn = (void *)e[i].fn;
        CHECK(e[i].why && e[i].why[0], "takeover has a reason");
    }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
int main(void) {
    // bool arguments are widened as hle.h requires (only the low 8 bits count); the returns are bools in w0, not nn::Result
    unsigned (*stereo)(unsigned) = hook("_ZN2nn4pctl27CheckStereoVisionPermissionEb");
    unsigned (*agc)(const void *) = hook("_ZN2nn4pctl33CheckAgcPermissionUnderMinimumAgeERKNS_7account3UidE");
    const uint64_t agc_uid[2] = {1, 2};
    if (agc) CHECK(agc(agc_uid) == 0, "a user under the minimum age: not permitted");
    const unsigned args[] = {0, 1, 0x100, 0x101, ~0u};
    for (size_t i = 0; i < sizeof args / sizeof *args; i++)
        if (stereo) CHECK(stereo(args[i]) == 0, "stereoscopic 3D not permitted, with or without UI: no VR display");
    printf("pctl: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
