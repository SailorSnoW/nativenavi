// Practice tools (DM_PRACTICE=1): PracticeROM's sloperinaGZ, the gz-style practice menu speedrunners use for this game on emulators and consoles, loaded as the add-on module subsdk9
// with its IPS patch of main, as Atmosphère's LayeredFS loads them. The modules folder's practice/ holds the release's subsdk9 rewritten by patch_nso.py and the IPS of main's build
// (the game's note, "Practice tools"). Off, the default, nothing of it is read: the modules, their bindings and main's code are the game's alone (DM_LOADHASH as without the folder).
// What the module needs beyond the game's own calls is the add-on takeovers (HLE_ADDON: the SD card mount in hle_fs.c, the hid detail readers in hle_hid.c), which bind for it only
#include "dm.h"
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

#define PRACTICE_MODULE "subsdk9"   // the name the release ships it under: Horizon loads an ExeFS's subsdk<N> in number order, after main and before sdk

static char g_dir[1100];
static int g_loaded;

static int practice_on(void) { const char *e = getenv("DM_PRACTICE"); return e && !strcmp(e, "1"); }

// The module list with the practice module put before sdk; names and dirs have room for n + 1 (main.c). Returns the new count. Before load_modules, after dm_mods_init
int dm_practice_modules(const char *dir, const char **names, const char **dirs, int n) {
    if (!practice_on()) return n;
    snprintf(g_dir, sizeof g_dir, "%s/practice", dir);
    char p[1200]; struct stat st;
    snprintf(p, sizeof p, "%s/%s.fixups.tsv", g_dir, PRACTICE_MODULE);
    if (stat(p, &st)) { LOG("[RED] practice tools: DM_PRACTICE=1, but this build has none (no %s): the game starts without them", p); return n; }
    for (int i = 0; i < n; i++)
        if (!strcmp(names[i], PRACTICE_MODULE)) { LOG("[RED] practice tools: the game has a module named %s already: they are not loaded", PRACTICE_MODULE); return n; }
    int at = n;
    for (int i = 0; i < n; i++) if (!strcmp(names[i], "sdk")) { at = i; break; }
    for (int i = n; i > at; i--) { names[i] = names[i - 1]; dirs[i] = dirs[i - 1]; }
    names[at] = PRACTICE_MODULE; dirs[at] = g_dir;
    dm_mods_add_builtin("practice tools", g_dir);
    g_loaded = 1;
    return n + 1;
}

// After load_modules: the patch of main's build has to be there, or the module stays idle (it checks main for the patch's marker and branch before it hooks anything)
void dm_practice_check(void) {
    if (!g_loaded) return;
    const char *bid = dm_main_build(); int have = 0;
    DIR *d = opendir(g_dir); struct dirent *e;
    while (d && (e = readdir(d)) && !have) {
        size_t n = strlen(e->d_name);
        have = n > 4 && !strcasecmp(e->d_name + n - 4, ".ips") && *bid && !strncasecmp(e->d_name, bid, strlen(bid));
    }
    if (d) closedir(d);
    if (have) LOG("practice tools: %s and its patch of main (build %s) from %s; in game, R + right stick down opens the menu", PRACTICE_MODULE, bid, g_dir);
    else LOG("[RED] practice tools: %s has no patch for main's build %s: the module is loaded but stays idle", g_dir, bid);
}
