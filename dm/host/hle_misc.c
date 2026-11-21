// Miscellaneous service layer: add-on content / shop service accessor / error applet / profanity filter / server name resolution / parental controls / streamplay.
// On the real device each goes through IPC or a system applet; the host gives results as single-player offline. Pure-computation SDK parts (hash, util, software Opus) are not taken over, the original implementation is used directly.
#include "hle.h"
#include <dirent.h>

// ---------- aoc add-on content (the DLC NSPs in the player's folder) / ec shop (excluded) ----------
// Owned add-on content indices (ascending), fixed at the first query:
//   numeric subdirectories of DM_AOC_DIR: each is one DLC's RomFS, which the packing script extracts only from DLC NSPs really in the player's folder (title ID = base + 0x1000 + index).
//     Engage: F001 → 1 (E0), F002 → 2 (H0); the content itself is in the 2.0.0 update's RomFS and each DLC package holds one empty marker file; the game calls MountAddOnContent for each index ListAddOnContent returns (hle_fs.c).
//   No DM_AOC_DIR: none (on a Switch 2 Mario 1.2.1 uses it to tell whether the upgrade pack is owned, and the user has no Mario add-on content files).
//   DM_AOC_OWNED=0: always none (test switch, for runs without the DLC).
// Room for every index the scan accepts (1..2000): Super Smash Bros. Ultimate has 99 DLC, and a cap below the folder count dropped whichever readdir returned last (fighters among them).
static int g_aoc[2000], g_naoc = -1;
static int aoc_int_cmp(const void *a, const void *b) { return *(const int *)a - *(const int *)b; }
static void aoc_load(void) {
    if (g_naoc >= 0) return;
    g_naoc = 0;
    const char *e = getenv("DM_AOC_OWNED"), *dir = getenv("DM_AOC_DIR"); const char *why = "no DM_AOC_DIR";
    if (e && !strcmp(e, "0")) why = "DM_AOC_OWNED=0";
    else if (dir && *dir) {
        why = dir; DIR *d = opendir(dir); struct dirent *ent;
        while (d && (ent = readdir(d)) && g_naoc < (int)(sizeof g_aoc / sizeof *g_aoc)) {
            char *end; long i = strtol(ent->d_name, &end, 10);
            if (ent->d_name[0] >= '1' && ent->d_name[0] <= '9' && (!*end || !strcmp(end, ".img")) && i <= 2000) g_aoc[g_naoc++] = (int)i;
        }
        if (d) closedir(d);
        qsort(g_aoc, g_naoc, sizeof *g_aoc, aoc_int_cmp);
        int unique = 0; for (int i = 0; i < g_naoc; i++) if (!i || g_aoc[i] != g_aoc[i - 1]) g_aoc[unique++] = g_aoc[i];
        g_naoc = unique;   // a legacy directory and its replacement image report the same DLC once
    }
    static char list[2000 * 5 + 1]; list[0] = 0; for (int i = 0; i < g_naoc; i++) snprintf(list + strlen(list), sizeof list - strlen(list), "%s%d", i ? "," : "", g_aoc[i]);
    LOG("aoc: reporting %d add-on contents {%s} (%s)", g_naoc, list, why);
}
// On the big host stack: the first query lists DM_AOC_DIR and logs, a few KB, and Digimon Story Cyber Sleuth asks from a thread with little stack left (SIGBUS in __vfprintf
// under aoc_load, a store right below a page of the guest thread's stack)
HLE_FORCE("_ZN2nn3aoc17CountAddOnContentEv", "add-on content count: length of the owned index list (DM_AOC_DIR)", int, h_aoc_Count, void) { aoc_load(); return g_naoc; }
// ListAddOnContent(out, start index, max count): writes out from the owned index list starting at the start index, returns the number written
HLE_FORCE("_ZN2nn3aoc16ListAddOnContentEPiii", "add-on content list: the owned index list", int, h_aoc_List, int *out, int off, int n) {
    int w = 0; aoc_load();
    for (int i = off < 0 ? 0 : off; i < g_naoc && w < n; i++) out[w++] = g_aoc[i];
    return w;
}
// The shop service accessor's request and its async response: the SDK's versions go through an ec service session, which the host does not open.
// No request is ever sent: Request fails, and a response is complete at once and holds nothing
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor7RequestEPNS1_13AsyncResponseERKNS_7account3UidENS0_11ShopService6MethodEPKcRKNS1_8PostDataENS_8TimeSpanE", "shop request: never sent (cut)",
          Result, h_ec_SsaRequest, void *self, void *resp, const void *uid, int method, const char *path, const void *post, int64_t timeout) {
    (void)self; (void)resp; (void)uid; (void)method; (void)path; (void)post; (void)timeout; return RES_FAIL;
}
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7TryWaitEv", "shop async response: complete at once (cut)", unsigned, h_ec_ArTryWait, void *self) { (void)self; return 1; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse7GetSizeEPm", "shop async response: no data (cut)", Result, h_ec_ArGetSize, void *self, size_t *n) { (void)self; if (n) *n = 0; return RES_FAIL; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse3GetEPvm", "shop async response: no data (cut)", Result, h_ec_ArGet, void *self, void *buf, size_t n) { (void)self; (void)buf; (void)n; return RES_FAIL; }
HLE_FORCE("_ZN2nn2ec19ShopServiceAccessor13AsyncResponse6CancelEv", "shop async response: nothing to cancel (cut)", void, h_ec_ArCancel, void *self) { (void)self; }
HLE_FORCE("_ZNK2nn2ec19ShopServiceAccessor13AsyncResponse12GetErrorCodeEv", "shop async response: no error code (cut)", uint64_t, h_ec_ArErr, const void *self) { (void)self; return 0; }

// ---------- err error applet: printed by the host ----------
HLE_FORCE("_ZN2nn3err9ShowErrorENS_6ResultE", "error display applet; the host prints the module / description number", void, h_err_ShowError, Result r) { DM_APPLET("error");
    LOG("err::ShowError Result=%#x (module %u, description %u)", r, r & 0x1ff, (r >> 9) & 0x1fff);
}

// ---------- ngc profanity filter: the word list is in a system data save (mounted via IPC); single-player has no text visible to others, always judged as no profanity ----------
// (Initialize and the char16_t check are in hle_offline.c)
static Result ngc_mask(void *self, int *masked, void *text, uint32_t region) { (void)self; (void)text; (void)region; if (masked) *masked = 0; return RES_OK; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilterD1Ev", "profanity filter", void, h_ngc_Dtor, void *self) { (void)self; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter17GetContentVersionEv", "word list version; the host returns 0", uint32_t, h_ngc_Ver, void *self) { (void)self; return 0; }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter19CheckProfanityWordsEPjjPPKcm", "profanity check; never a hit", Result, h_ngc_Check, void *self, uint32_t *masks, uint32_t pat, const char **words, uint64_t n) {
    (void)self; (void)pat; (void)words;
    for (uint64_t i = 0; i < n; i++) masks[i] = 0;
    return RES_OK;
}
// The SDK's versions read the word list the host never mounted
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPcj", "profanity masking (UTF-8); nothing is masked", Result, h_ngc_Mask8, void *self, int *masked, char *text, uint32_t region) { return ngc_mask(self, masked, text, region); }
// The UTF-16 forms: Dark Souls Remastered (SDK 4) masks the character name with the unsigned short one right after the name entry; the SDK's version allocated from a heap the host never set up
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPtj", "profanity masking (UTF-16); nothing is masked", Result, h_ngc_Mask16, void *self, int *masked, uint16_t *text, uint32_t region) { return ngc_mask(self, masked, text, region); }
HLE_FORCE("_ZN2nn3ngc15ProfanityFilter24MaskProfanityWordsInTextEPiPDsj", "profanity masking (char16_t); nothing is masked", Result, h_ngc_Mask16c, void *self, int *masked, uint16_t *text, uint32_t region) { return ngc_mask(self, masked, text, region); }

// ---------- nsd server name resolution (networking, out of scope) / pctl parental controls ----------
HLE_FORCE("_ZN2nn3nsd9ResolveExEPNS0_4FqdnERKS1_", "server name resolution, networking out of scope; cut", Result, h_nsd_ResolveEx, void *out, const void *in) { (void)out; (void)in; return RES_FAIL; }
HLE_FORCE("_ZN2nn4pctl33CheckAgcPermissionUnderMinimumAgeERKNS_7account3UidE", "parental controls · a user's permission under the minimum age (SDK 22), networking out of scope; never", unsigned, h_pctl_CheckAgc, const void *uid) { (void)uid; return 0; }
// The permission for stereoscopic 3D, which a game asks before it turns on a VR mode (returns bool; the argument = whether to show UI, a bool of which only the low 8 bits count).
// The host has no VR display: not permitted
HLE_FORCE("_ZN2nn4pctl27CheckStereoVisionPermissionEb", "parental controls · stereoscopic 3D permission; the host has no VR display, not permitted", unsigned, h_pctl_Stereo, unsigned show_ui) { (void)show_ui; return 0; }

// ---------- streamplay: GameShare / GameChat streaming (Switch 2 SDKs): the host streams to nobody, so no guest is connected ----------
HLE_FORCE("_ZN2nn10streamplay27GetGuestVibrationDeviceInfoEPNS_3hid19VibrationDeviceInfoERKNS0_26GuestVibrationDeviceHandleE", "guest vibration device info: none (no guest)",
          void, h_stpl_VibInfo, uint64_t *info, const void *h) { (void)h; *info = 0; }
