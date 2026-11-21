// Shared fonts: one stand-in per type from the font folder, and the built-in font takeovers that hand them out; no game module is needed.
#include "../hle_sdk16.c"
void dm_mii_default_charinfo(void *out) { memset(out, 0, 0x58); }
#include <sys/wait.h>

static int checks, fails;
#define CHECK(c, msg) do { checks++; if (!(c)) { fails++; fprintf(stderr, "FAIL: %s\n", msg); } } while (0)
module_t *g_mod;
int g_nmod;
uint64_t g_t0_ns;
uint64_t now_ns(void) { return 0; }
int dm_game_imports(const char *prefix) { (void)prefix; return 1; }
module_t *module_of(const void *p) { (void)p; return NULL; }
void dm_backtrace(uint64_t fp) { (void)fp; }
void hle_bind_system_event(void *ev, int autoclear) { (void)ev; (void)autoclear; }
void hle_signal_system_event(void *ev) { (void)ev; }
uint64_t hle_system_event_binding(void *ev) { (void)ev; return 1; }
int hle_signal_system_event_binding(uint64_t binding) { (void)binding; return 1; }
uint32_t hle_hid_connected_npad(void) { return 0; }
void dm_host_helper_qos(qos_class_t q) { (void)q; }
// One font per type from DM_FONT_DIR (a child each, as the fonts are opened once): a type's own file, else the standard font; DM_SHARED_FONT is one font for types 0-4
static int fonts_from_folder(const char *dir, const char *one) {
    pid_t child = fork();
    if (child) { int s = 0; waitpid(child, &s, 0); return WIFEXITED(s) ? WEXITSTATUS(s) : 99; }
    setenv("DM_FONT_DIR", dir, 1); unsetenv("DM_SHARED_FONT_EXT");
    if (one) setenv("DM_SHARED_FONT", one, 1); else unsetenv("DM_SHARED_FONT");
    int bad = 0; font_t f[FONT_TYPES];
    for (int t = 0; t < FONT_TYPES; t++) f[t] = font_get(t);
    const char *want[FONT_TYPES] = {one ? "one" : "std", "std", "std", "std", "kor", "ext"};
    if (one) want[1] = want[2] = want[3] = want[4] = "one";
    for (int t = 0; t < FONT_TYPES; t++) bad += !f[t].p || f[t].n != 8 + 3 || memcmp(f[t].p + 8, want[t], 3);
    bad += f[1].p != f[0].p || f[2].p != f[0].p;                   // no file of its own: the standard font's mapping itself
    bad += font_get(-1).p != f[0].p || font_get(FONT_TYPES).p != f[0].p;   // an unknown type: the standard font
    _exit(bad);
}
static void put(const char *dir, const char *file, const char *text) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", dir, file);
    FILE *f = fopen(p, "wb"); if (f) { fputs(text, f); fclose(f); }
}
int main(void) {
    char dir[] = "/tmp/t_shared_font.XXXXXX";
    if (mkdtemp(dir)) {
        put(dir, "standard.ttf", "std"); put(dir, "korean.ttf", "kor"); put(dir, "nintendo_ext.ttf", "ext");
        char one[1100]; snprintf(one, sizeof one, "%s/one.ttf", dir); put(dir, "one.ttf", "one");
        CHECK(fonts_from_folder(dir, NULL) == 0, "a type's own file, else the standard font");
        CHECK(fonts_from_folder(dir, one) == 0, "DM_SHARED_FONT: one font for types 0-4, the extension font still its own");
        const char *files[] = {"standard.ttf", "korean.ttf", "nintendo_ext.ttf", "one.ttf"};
        for (unsigned i = 0; i < 4; i++) { char p[1100]; snprintf(p, sizeof p, "%s/%s", dir, files[i]); unlink(p); }
        rmdir(dir);
    } else CHECK(0, "temporary font folder");
    uint8_t standard[64] = {0}, extension[64] = {0}, traditional[64] = {0};
    // GetBuiltInFontDataSet (Switch 2 types): one entry per type asked, in order, at most 6; the address and size are GetBuiltInFontData's
    for (int t = 0; t < FONT_TYPES; t++) (void)font_get(t);   // the fonts are opened once: what follows sets them (types 1, 2 and 4 as having no file of their own)
    for (int t = 0; t < FONT_TYPES; t++) font_by_type[t] = (font_t){standard, sizeof standard};
    font_by_type[3] = (font_t){traditional, sizeof traditional}; font_by_type[5] = (font_t){extension, sizeof extension};
    pl_builtin_font_t set[8]; memset(set, 0xa5, sizeof set);
    const uint8_t types[8] = {5, 0, 3, 5, 1, 2, 4, 0};
    int got = -1;
    CHECK(h_pl_BuiltInSetOunce(&got, set, types, 3, 0, 0) == 1 && got == 3, "font set: three types, three entries");
    CHECK(set[0].type == 5 && set[0].address == extension + font_skip() && set[0].size == sizeof extension - font_skip(), "font set: the extension font for type 5");
    CHECK(set[1].type == 0 && set[1].address == standard + font_skip() && set[2].type == 3 && set[2].address == traditional + font_skip(), "font set: each type's own font, in order");
    pl_builtin_font_t one; CHECK(h_pl_BuiltInOunce(&one, 3, 0) == 1 && !memcmp(&one, &set[2], sizeof one), "font set: an entry as GetBuiltInFontData fills it");
    CHECK(set[3].type == 0xa5, "font set: nothing past the count");
    memset(set, 0, sizeof set);
    CHECK(h_pl_BuiltInSetOunce(&got, set, types, 8, 0, 0) == 1 && got == 6 && set[5].type == 2 && set[6].type == 0 && set[6].address == NULL, "font set: at most 6, as the SDK sends");
    memset(font_by_type, 0, sizeof font_by_type);
    CHECK(h_pl_BuiltInSetOunce(&got, set, types, 3, 0, 0) == 0 && got == 0, "font set: no fonts, no entries");
    printf("shared font: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
