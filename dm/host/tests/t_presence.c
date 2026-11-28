// gamedata.c and presence.c: a GameDataMgr laid out as the profile's gamedata.tsv says, read back field by field; the activity's text; the place names' columns;
// Discord's socket protocol against a fake Discord (handshake, refusal, frames cut to the buffer). The expected values are written by hand.
#include "../gamedata.c"
#include "../presence.c"
#include <signal.h>

module_t *g_mod;
int g_nmod;
static int checks, fails;
#define CHECK(c, s) do { checks++; int ok_ = !!(c); fails += !ok_; printf("%s %s\n", ok_ ? "✓" : "✗", s); } while (0)

// ---- a fake manager: main's slot -> holder -> manager, a type entry per kind the profile lists ----
static uint64_t g_slot_value, g_holder;
static uint8_t g_mgr[0x1000];
static module_t g_main;
static struct { uint8_t *objects; uint32_t *table; uint32_t count, size; const gd_type *t; } g_ft[64];
static char g_texts[16][64];
static uint32_t murmur3_of(const char *s);
static int g_ntexts;

static const gd_build *build_10(void) {
    for (int i = 0; i < DM_GAME_GD_NBUILDS; i++) if (!strcmp(g_builds[i].build, "33fa6c2c23848282a77e8b88a0a93f48")) return &g_builds[i];
    return NULL;
}
static void fake_entry(const char *type, uint32_t count) {
    const gd_type *t = type_named(type);
    unsigned n = t->number;
    g_ft[n].t = t; g_ft[n].count = count; g_ft[n].size = 2 * count;
    g_ft[n].objects = calloc(count, t->stride); g_ft[n].table = calloc(g_ft[n].size, 8);
    uint8_t *e = g_mgr + DM_GAME_GD_ENTRY_FIRST + DM_GAME_GD_ENTRY_STEP * n;
    uint64_t vt = (uint64_t)(uintptr_t)g_main.base + build_10()->vtable + (uint64_t)DM_GAME_GD_VTABLE_STEP * n, o = (uint64_t)(uintptr_t)g_ft[n].objects, tb = (uint64_t)(uintptr_t)g_ft[n].table;
    memcpy(e, &vt, 8); memcpy(e + 0x08, &count, 4); memcpy(e + 0x10, &o, 8); memcpy(e + 0x18, &tb, 8); memcpy(e + 0x24, &g_ft[n].size, 4);
}
// the value's address of a new field of role's type; decoy: a foreign hash in its first slot, so the lookup has to probe on
static uint8_t *fake_field(const char *role, uint32_t index, int decoy) {
    const gd_field *f = field_for(role);
    unsigned n = type_named(f->type)->number;
    uint32_t at = f->hash % g_ft[n].size;
    if (decoy && !g_ft[n].table[2 * at]) { g_ft[n].table[2 * at] = f->hash ^ 0x5a5a5a5a; g_ft[n].table[2 * at + 1] = 0; }
    while (g_ft[n].table[2 * at]) at = (at + 1) % g_ft[n].size;
    g_ft[n].table[2 * at] = f->hash; g_ft[n].table[2 * at + 1] = index;
    return g_ft[n].objects + (size_t)index * g_ft[n].t->stride + g_ft[n].t->value;
}
static void put_text(uint8_t *desc, const char *s) {   // a text value: pointer and capacity
    char *t = g_texts[g_ntexts++];
    memset(t, 0, 64); strncpy(t, s, 63);
    uint64_t p = (uint64_t)(uintptr_t)t; uint32_t cap = 64;
    memcpy(desc, &p, 8); memcpy(desc + 8, &cap, 4);
}
static uint8_t *g_life, *g_menu, *g_paused, *g_place_text_desc;
static uint8_t g_place_elements[0x50 * 4];
static void fake_manager(void) {
    g_main.base = (uint8_t *)((uintptr_t)&g_slot_value - build_10()->slot);
    strcpy(g_main.name, "main"); strcpy(g_main.build_id, "33fa6c2c23848282a77e8b88a0a93f48");
    g_mod = &g_main; g_nmod = 1;
    g_slot_value = (uint64_t)(uintptr_t)&g_holder;
    g_holder = (uint64_t)(uintptr_t)g_mgr;
    for (int i = 0; i < DM_GAME_GD_NTYPES; i++) fake_entry(g_types[i].name, 8);
    put_text(fake_field("map", 3, 0), "HyruleField_Child");
    uint8_t *pl = fake_field("place", 1, 1);
    uint64_t cnt = 4, el = (uint64_t)(uintptr_t)g_place_elements;
    memcpy(pl, &cnt, 8); memcpy(pl + 8, &el, 8);
    g_place_text_desc = g_place_elements;
    put_text(g_place_text_desc, "LakeHylia");
    *fake_field("child", 5, 1) = 1;
    *(g_paused = fake_field("paused", 6, 0)) = 0;
    int32_t life = 11, max = 12, rupees = 1; uint32_t play = 3600, none = murmur3_of("None");
    memcpy(g_life = fake_field("life", 0, 1), &life, 4);
    memcpy(fake_field("max_life", 1, 0), &max, 4);
    memcpy(fake_field("rupees", 2, 1), &rupees, 4);
    memcpy(fake_field("play_time", 4, 0), &play, 4);
    memcpy(g_menu = fake_field("menu", 6, 1), &none, 4);
}

// MurmurHash3 x86 32, seed 0, for the menu values (game.py's, checked there against the game's)
static uint32_t murmur3_of(const char *s) {
    uint32_t h = 0, n = (uint32_t)strlen(s), i = 0, k;
    for (; i + 4 <= n; i += 4) {
        memcpy(&k, s + i, 4);
        k *= 0xcc9e2d51; k = k << 15 | k >> 17; k *= 0x1b873593;
        h ^= k; h = h << 13 | h >> 19; h = h * 5 + 0xe6546b64;
    }
    k = 0;
    switch (n & 3) { case 3: k ^= (uint32_t)(uint8_t)s[i + 2] << 16; /* fall through */ case 2: k ^= (uint32_t)(uint8_t)s[i + 1] << 8; /* fall through */
        case 1: k ^= (uint8_t)s[i]; k *= 0xcc9e2d51; k = k << 15 | k >> 17; k *= 0x1b873593; h ^= k; }
    h ^= n; h ^= h >> 16; h *= 0x85ebca6b; h ^= h >> 13; h *= 0xc2b2ae35; h ^= h >> 16;
    return h;
}

static void test_reader(void) {
    dm_gd_state s;
    const char *why = "";
    g_nmod = 0;
    CHECK(!dm_gd_read(&s, &why) && strstr(why, "profile"), "no main module: refused, the profile named as the reason");
    fake_manager();
    uint64_t keep = g_slot_value;
    g_slot_value = 0;
    CHECK(!dm_gd_read(&s, &why) && strstr(why, "no GameDataMgr"), "an empty slot: no manager yet");
    g_slot_value = keep;
    int ok = dm_gd_read(&s, &why);
    CHECK(ok && !strcmp(s.place, "LakeHylia") && !strcmp(s.map, "HyruleField_Child"), "place (an array's first text) and map read");
    CHECK(ok && s.child == 1 && s.paused == 0 && s.life == 11 && s.max_life == 12 && s.rupees == 1 && s.play_time == 3600 && s.menu == DM_GD_MENU_NONE, "age, hearts, rupees, play time and menu read, through probed slots");
    uint32_t title = murmur3_of("Title"), other = murmur3_of("ItemMenu"), over = murmur3_of("GameOver");
    memcpy(g_menu, &title, 4); dm_gd_read(&s, &why);
    CHECK(s.menu == DM_GD_MENU_TITLE, "menu Title");
    memcpy(g_menu, &over, 4); dm_gd_read(&s, &why);
    CHECK(s.menu == DM_GD_MENU_GAME_OVER, "menu GameOver");
    memcpy(g_menu, &other, 4); *g_paused = 1; dm_gd_read(&s, &why);
    CHECK(s.menu == DM_GD_MENU_OTHER && s.paused == 1, "another menu value: other; the pause read");
    *g_paused = 0;
    char *t = (char *)(uintptr_t)*(uint64_t *)g_place_text_desc;
    memcpy(t, "Lake\x01Hylia", 11);
    dm_gd_read(&s, &why);
    CHECK(!s.place[0] && !strcmp(s.map, "HyruleField_Child"), "a text that is not an identifier (a torn read): unread, the map still read");
    memset(t, 'A', 64);
    dm_gd_read(&s, &why);
    CHECK(!s.place[0], "a text with no end inside its 64 bytes: unread");
    strcpy(t, "LakeHylia");
    uint8_t *e = g_mgr + DM_GAME_GD_ENTRY_FIRST + DM_GAME_GD_ENTRY_STEP * type_named("Int")->number;
    uint64_t vt; memcpy(&vt, e, 8); vt += 8; memcpy(e, &vt, 8);
    CHECK(!dm_gd_read(&s, &why) && strstr(why, "Int entry"), "an Int entry with another vtable: refused");
    vt -= 8; memcpy(e, &vt, 8);
    uint8_t *b = g_mgr + DM_GAME_GD_ENTRY_FIRST + DM_GAME_GD_ENTRY_STEP * type_named("Bool")->number;
    uint64_t bt; memcpy(&bt, b + 0x18, 8); uint64_t bad = 0x10; memcpy(b + 0x18, &bad, 8);
    ok = dm_gd_read(&s, &why);
    CHECK(ok && s.child == -1 && s.paused == -1 && s.life == 11, "a hash table at an unmapped address: that field unread, no fault, the others read");
    memcpy(b + 0x18, &bt, 8);
    memcpy(g_menu, &(uint32_t){murmur3_of("None")}, 4);
}

static void test_activity(void) {
    dm_gd_state s = {.place = "LakeHylia", .map = "HyruleField_Child", .child = 1, .paused = 0, .life = 11, .max_life = 12, .rupees = 1, .play_time = 3600, .menu = DM_GD_MENU_NONE};
    activity a;
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strcmp(a.details, "Lac Hylia"), "details: the place in the game's language");
    CHECK(!strcmp(a.state, "Child · ♥ 2¾/3 · 1 rupee"), "state: age, hearts in quarters, one rupee");
    CHECK(a.start == 100000 - 3600, "start: the file's play time before now");
    s.menu = DM_GD_MENU_OTHER;
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strstr(a.state, "Paused"), "a menu value other than the title or Game Over, without the pause (a load): not paused");
    s.child = 0; s.life = 2; s.max_life = 40; s.rupees = 250; s.paused = 1; s.menu = DM_GD_MENU_NONE;
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strcmp(a.state, "Adult · ♥ ½/10 · 250 rupees · Paused"), "adult, half a heart, rupees, paused");
    s.life = 50;
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(strstr(a.state, "♥ 10/10") != NULL, "life above max shows as full");
    s.menu = DM_GD_MENU_GAME_OVER;
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strcmp(a.state, "Game Over"), "Game Over");
    s.menu = DM_GD_MENU_NONE; s.place[0] = 0; strcpy(s.map, "Market_Child");
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strcmp(a.details, "Citadelle d'Hyrule"), "no place: the map's, without its age");
    strcpy(s.map, "GohmaStage");
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strcmp(a.details, "Gohma Stage"), "a map no name is known for: its ID's words");
    s.menu = DM_GD_MENU_TITLE;
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strcmp(a.details, "Title screen") && !a.state[0] && a.start == 5, "title: no state, the session's start");
    activity_of(&s, 0, 100000, 5, &a);
    CHECK(!strcmp(a.details, "Title screen"), "no manager: title screen");
    s.menu = DM_GD_MENU_NONE; s.play_time = -1; s.child = -1; s.paused = -1; s.life = -1; s.rupees = -1; strcpy(s.place, "KokiriForest");
    activity_of(&s, 1, 100000, 5, &a);
    CHECK(!strcmp(a.details, "Forêt Kokiri") && !a.state[0] && a.start == 5, "nothing but the place read: no state, the session's start");
    char json[800];
    activity q = {.details = "A \"quoted\" \\ name", .state = "", .start = 7};
    CHECK(activity_json(&q, 42, 3, json, sizeof json) && !strcmp(json, "{\"cmd\":\"SET_ACTIVITY\",\"args\":{\"pid\":42,\"activity\":{\"name\":\"The Legend of Zelda: Ocarina of Time\",\"type\":0,\"details\":\"A \\\"quoted\\\" \\\\ name\",\"timestamps\":{\"start\":7000},\"assets\":{\"large_image\":\"oot\",\"large_text\":\"The Legend of Zelda: Ocarina of Time\"}}},\"nonce\":\"3\"}"),
          "SET_ACTIVITY's JSON: escapes, no empty state, milliseconds, the profile's image with the game's name");
    CHECK(!activity_json(&q, 42, 3, json, 60), "a JSON that does not fit is refused");
}

static void test_places(const char *dir) {
    char p[600];
    snprintf(p, sizeof p, "%s/places.tsv", dir);
    FILE *f = fopen(p, "w");
    if (!f) { CHECK(0, "the test's places.tsv written"); return; }
    fprintf(f, "# test\nid\tja\ten-US\tfr\tfr-CA\nLakeHylia\t\xe3\x83\x8f\tLake Hylia\tLac Hylia\tLac Hylia (CA)\nMarket\t\xe3\x83\x8f\tHyrule Castle Town\tCitadelle d'Hyrule\tCitadelle\nKokiriForest\t\xe3\x82\xb3\tKokiri Forest\tFor\xc3\xaat Kokiri\t\n");
    fclose(f);
    char out[128];
    g_nplaces = 0; places_load(dir, "fr-CA"); place_name("KokiriForest", out, sizeof out);
    CHECK(g_nplaces == 2 && !strcmp(out, "Kokiri Forest"), "fr-CA: its own column; a place it has no name for is named from its ID");
    g_nplaces = 0; places_load(dir, "de"); place_name("LakeHylia", out, sizeof out);
    CHECK(!strcmp(out, "Lake Hylia"), "a language with no column: en-US's");
    g_nplaces = 0; places_load(dir, "fr-BE"); place_name("LakeHylia", out, sizeof out);
    CHECK(!strcmp(out, "Lac Hylia"), "a regional code with no column: its language's");
    g_nplaces = 0; places_load(dir, "fr");
}

// a fake Discord on <dir>/discord-ipc-0: answers the handshake with READY or a close, then reads one frame
static int g_lfd, g_answer_close;
static char g_got_hs[256], g_got_frame[1024];
static void *fake_discord(void *arg) {
    (void)arg;
    int c = accept(g_lfd, NULL, NULL);
    uint32_t op;
    if (c < 0 || frame_read(c, &op, g_got_hs, sizeof g_got_hs, 5000) < 0 || op != OP_HANDSHAKE) return NULL;
    const char *ans = g_answer_close ? "{\"code\":4000,\"message\":\"Invalid Client ID\"}" : "{\"cmd\":\"DISPATCH\",\"data\":{\"v\":1},\"evt\":\"READY\",\"nonce\":null}";
    frame_write(c, g_answer_close ? OP_CLOSE : OP_FRAME, ans, (uint32_t)strlen(ans));
    if (!g_answer_close) frame_read(c, &op, g_got_frame, sizeof g_got_frame, 5000);
    close(c);
    return NULL;
}
static void test_socket(const char *dir) {
    struct sockaddr_un a = {.sun_family = AF_UNIX};
    snprintf(a.sun_path, sizeof a.sun_path, "%s/discord-ipc-0", dir);
    int refused = 0;
    CHECK(ipc_connect("123456789012345678", dir, &refused) < 0 && !refused, "no socket: not connected, not refused");
    for (int pass = 0; pass < 2; pass++) {
        g_answer_close = pass;
        unlink(a.sun_path);
        g_lfd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (g_lfd < 0 || bind(g_lfd, (struct sockaddr *)&a, sizeof a) || listen(g_lfd, 1)) { CHECK(0, "the fake Discord listens"); return; }
        pthread_t th;
        pthread_create(&th, NULL, fake_discord, NULL);
        refused = 0;
        int fd = ipc_connect("123456789012345678", dir, &refused);
        if (!pass) {
            CHECK(fd >= 0 && !refused && !strcmp(g_got_hs, "{\"v\":1,\"client_id\":\"123456789012345678\"}"), "handshake: version 1 and the application ID, READY answered");
            const char *j = "{\"cmd\":\"SET_ACTIVITY\"}";
            CHECK(fd >= 0 && frame_write(fd, OP_FRAME, j, (uint32_t)strlen(j)), "a frame written");
            pthread_join(th, NULL);
            CHECK(!strcmp(g_got_frame, j), "the frame read back whole");
            if (fd >= 0) close(fd);
        } else {
            pthread_join(th, NULL);
            CHECK(fd < 0 && refused, "a close for the handshake: refused (an unknown application ID)");
        }
        close(g_lfd);
    }
    unlink(a.sun_path);
    int sv[2];
    socketpair(AF_UNIX, SOCK_STREAM, 0, sv);
    char big[3000], small[16];
    memset(big, 'x', sizeof big);
    uint32_t op = 0;
    frame_write(sv[0], OP_PING, big, sizeof big);
    frame_write(sv[0], OP_FRAME, "ok", 2);
    int n1 = frame_read(sv[1], &op, small, sizeof small, 1000);
    CHECK(n1 == 15 && op == OP_PING && strlen(small) == 15, "a frame longer than the buffer: cut, NUL-terminated");
    int n2 = frame_read(sv[1], &op, small, sizeof small, 1000);
    CHECK(n2 == 2 && op == OP_FRAME && !strcmp(small, "ok"), "the next frame after a cut one: whole (the rest was read past)");
    CHECK(frame_read(sv[1], &op, small, sizeof small, 0) == -2, "nothing waiting: -2");
    close(sv[0]);
    CHECK(frame_read(sv[1], &op, small, sizeof small, 1000) == -1, "the other end closed: -1");
    close(sv[1]);
}

int main(void) {
    signal(SIGPIPE, SIG_IGN);
    const char *tmp = getenv("TMPDIR");
    char dir[200];
    snprintf(dir, sizeof dir, "%s/t_presence.XXXXXX", tmp && *tmp && strlen(tmp) < 80 ? tmp : "/tmp");
    if (!mkdtemp(dir)) { perror("mkdtemp"); return 1; }
    test_places(dir);
    test_reader();
    test_activity();
    test_socket(dir);
    char p[300];
    snprintf(p, sizeof p, "%s/places.tsv", dir); unlink(p);
    rmdir(dir);
    printf("t_presence: %d checks, %d failed\n", checks, fails);
    return fails != 0;
}
