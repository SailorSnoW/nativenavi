// nn::friends (friends / presence / invitations, out of scope): every interface that connects to the friends service is cut to "unavailable": its Result is RES_FAIL.
// hle_offline.c has the friend list and play history calls.
#include "hle.h"

HLE_FORCE("_ZN2nn7friends14GetProfileListEPNS0_12AsyncContextEPNS0_7ProfileERKNS_7account3UidEPKNS5_23NetworkServiceAccountIdEi", "profile query fails (cut)",
          Result, h_fr_GetProfileList, void *ctx, void *out, const void *uid, const void *ids, int n) { (void)ctx; (void)out; (void)uid; (void)ids; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends16UpdateFriendInfoEPNS0_6FriendERKNS_7account3UidEPKNS3_23NetworkServiceAccountIdEi", "friend info update fails (cut)",
          Result, h_fr_UpdateFriendInfo, void *f, const void *uid, const void *ids, int n) { (void)f; (void)uid; (void)ids; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends17NotificationQueue10InitializeERKNS_7account3UidE", "notification queue init fails (cut)", Result, h_fr_NqInit, void *self, const void *uid) { (void)self; (void)uid; return RES_FAIL; }
HLE_FORCE("_ZN2nn7friends18GetBlockedUserListEPiPNS_7account23NetworkServiceAccountIdERKNS2_3UidEii", "blocked user list is empty (cut)",
          Result, h_fr_GetBlocked, int *count, void *out, const void *uid, int off, int n) { (void)out; (void)uid; (void)off; (void)n; if (count) *count = 0; return RES_FAIL; }
