// friends service unit test: the calls that would reach the friends service fail at once, and each takeover is registered under the exact name the game's modules reference
// (a missing or misspelled overload would run the SDK's friends client instead). Expected results are hand-written, not taken from the implementation.
#include "../hle_friends.c"
#include <mach-o/getsect.h>
extern const struct mach_header_64 _mh_execute_header;
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok = !!(c); fails += !ok; printf("%s %s\n", ok ? "✓" : "✗", s); } while (0)
static void *hook(const char *name) {
    unsigned long bytes = 0;
    const hle_force_ent *entries = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &bytes);
    void *fn = NULL; unsigned count = 0;
    for (size_t i = 0; entries && i < bytes / sizeof *entries; i++) {
        if (strcmp(entries[i].name, name)) continue;
        count++; fn = entries[i].fn;
    }
    CHECK(count == 1 && fn, name);
    return count == 1 ? fn : NULL;
}
int main(void) {
    uint8_t obj[0x100], before[sizeof obj], uid[16] = {1}, ids[8] = {0};
    memset(before, 0xa5, sizeof before); memcpy(obj, before, sizeof obj);
    Result (*profiles)(void *, void *, const void *, const void *, int) = hook("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi");
    Result (*update)(void *, const void *, const void *, int) = hook("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi");
    Result (*queue)(void *, const void *) = hook("_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE");
    Result (*blocked)(int *, void *, const void *, int, int) = hook("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii");
    if (profiles) CHECK(profiles(NULL, obj, uid, ids, 1) == RES_FAIL && !memcmp(obj, before, sizeof obj), "GetProfileList fails and leaves the profiles untouched");
    if (update) CHECK(update(obj, uid, ids, 1) == RES_FAIL && !memcmp(obj, before, sizeof obj), "UpdateFriendInfo fails and leaves the friends untouched");
    if (queue) CHECK(queue(obj, uid) == RES_FAIL && !memcmp(obj, before, sizeof obj), "NotificationQueue::Initialize fails");
    int count = 7;
    if (blocked) CHECK(blocked(&count, obj, uid, 0, 4) == RES_FAIL && count == 0 && !memcmp(obj, before, sizeof obj), "GetBlockedUserList fails with an empty list");
    if (blocked) CHECK(blocked(NULL, obj, uid, 0, 4) == RES_FAIL, "GetBlockedUserList takes a null count");
    printf("t_friends: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
