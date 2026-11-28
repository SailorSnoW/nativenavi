// The game's own state from its GameDataMgr: the manager through main's slot, a type entry per kind, a field by its hash (the profile's gamedata.tsv describes the layout).
// Every read is mach_vm_read_overwrite into a local copy: the game frees and moves what its pointers name, and a reset gives its memory back, so a read fails and is
// answered "unread" instead of faulting. The game writes while the host reads, so a text that is not a whole identifier is taken as unread too.
#include "dm.h"
#include "gamedata.h"
#include <mach/mach.h>
#include <mach/mach_vm.h>

typedef struct { const char *build; uint64_t slot, vtable; } gd_build;
typedef struct { const char *name; unsigned number; uint32_t stride, value; } gd_type;
typedef struct { const char *role, *type; uint32_t hash; } gd_field;
typedef struct { const char *role; uint32_t value; } gd_menu;
static const gd_build g_builds[] = {DM_GAME_GD_BUILDS};
static const gd_type g_types[] = {DM_GAME_GD_TYPES};
static const gd_field g_fields[] = {DM_GAME_GD_FIELDS};
static const gd_menu g_menus[] = {DM_GAME_GD_MENUS};

// sanity bounds on what the game's memory claims: Ocarina of Time has 10,411 Bool fields in a table of 20,822
#define GD_MAX_COUNT (1u << 20)
#define GD_MAX_PROBES 4096

int dm_gd_peek(uint64_t a, void *out, uint64_t n) {
    mach_vm_size_t got = 0;
    return a >= 0x1000 && n && mach_vm_read_overwrite(mach_task_self(), a, n, (mach_vm_address_t)(uintptr_t)out, &got) == KERN_SUCCESS && got == n;
}
static int peek64(uint64_t a, uint64_t *v) { return dm_gd_peek(a, v, 8); }

typedef struct { uint64_t main, mgr; const gd_build *b; } gd_ctx;
typedef struct { uint64_t objects, table; uint32_t count, size; const gd_type *t; } gd_entry;

static const gd_type *type_named(const char *name) {
    for (int i = 0; i < DM_GAME_GD_NTYPES; i++) if (!strcmp(g_types[i].name, name)) return &g_types[i];
    return NULL;
}
static int entry_of(const gd_ctx *c, const gd_type *t, gd_entry *e) {
    uint8_t b[0x28];
    if (!t || !dm_gd_peek(c->mgr + DM_GAME_GD_ENTRY_FIRST + (uint64_t)DM_GAME_GD_ENTRY_STEP * t->number, b, sizeof b)) return 0;
    uint64_t vt;
    memcpy(&vt, b, 8); memcpy(&e->count, b + 0x08, 4); memcpy(&e->objects, b + 0x10, 8); memcpy(&e->table, b + 0x18, 8); memcpy(&e->size, b + 0x24, 4);
    e->t = t;
    return vt == c->main + c->b->vtable + (uint64_t)DM_GAME_GD_VTABLE_STEP * t->number && e->count && e->count <= GD_MAX_COUNT && e->size >= e->count && e->size <= 4 * GD_MAX_COUNT;
}
// the object of field hash, probed as the game's own getters probe
static int object_of(const gd_entry *e, uint32_t hash, uint64_t *obj) {
    uint32_t at = hash % e->size;
    for (int k = 0; k < GD_MAX_PROBES && k < (int)e->size; k++) {
        uint32_t pair[2];
        if (!dm_gd_peek(e->table + 8ull * at, pair, sizeof pair) || !pair[0]) return 0;
        if (pair[0] == hash) {
            if (pair[1] >= e->count) return 0;
            *obj = e->objects + (uint64_t)pair[1] * e->t->stride;
            return 1;
        }
        if (++at == e->size) at = 0;
    }
    return 0;
}
// a text value: a pointer and a capacity; only a whole identifier counts
static int text_at(uint64_t desc, char out[65]) {
    uint64_t p; uint32_t cap; char s[64];
    if (!peek64(desc, &p) || !dm_gd_peek(desc + 8, &cap, 4) || !p || cap < sizeof s || !dm_gd_peek(p, s, sizeof s)) return 0;
    size_t n = 0;
    while (n < sizeof s && s[n]) {
        char ch = s[n++];
        if (!((ch >= 'A' && ch <= 'Z') || (ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '_')) return 0;
    }
    if (n == sizeof s) return 0;
    memcpy(out, s, n + 1);
    return 1;
}
static const gd_field *field_for(const char *role) {
    for (int i = 0; i < DM_GAME_GD_NFIELDS; i++) if (!strcmp(g_fields[i].role, role)) return &g_fields[i];
    return NULL;
}
// the value's address of role's field, 0 when the field or its entry cannot be read; *t its type
static uint64_t value_of(const gd_ctx *c, const char *role, const gd_type **t) {
    const gd_field *f = field_for(role);
    gd_entry e;
    uint64_t obj;
    if (!f || !entry_of(c, type_named(f->type), &e) || !object_of(&e, f->hash, &obj)) return 0;
    *t = e.t;
    return obj + e.t->value;
}
static int read_int(const gd_ctx *c, const char *role, int64_t *v) {
    const gd_type *t = NULL;
    uint64_t a = value_of(c, role, &t);
    if (!a) return 0;
    if (!strcmp(t->name, "Bool")) { uint8_t b; if (!dm_gd_peek(a, &b, 1)) return 0; *v = b != 0; return 1; }
    uint32_t w;
    if (!dm_gd_peek(a, &w, 4)) return 0;
    *v = !strcmp(t->name, "Int") ? (int64_t)(int32_t)w : (int64_t)w;
    return 1;
}
static int read_text(const gd_ctx *c, const char *role, char out[65]) {
    const gd_type *t = NULL;
    uint64_t a = value_of(c, role, &t), n, el;
    if (!a) return 0;
    if (!strcmp(t->name, "String64")) return text_at(a, out);
    if (!peek64(a, &n) || !peek64(a + 8, &el)) return 0;
    if (!n) { out[0] = 0; return 1; }
    return text_at(el, out);
}

int dm_gd_read(dm_gd_state *s, const char **why) {
    *s = (dm_gd_state){.child = -1, .paused = -1, .life = -1, .max_life = -1, .rupees = -1, .play_time = -1, .menu = DM_GD_MENU_UNKNOWN};
    gd_ctx c = {0};
    const char *bid = dm_main_build();
    for (int i = 0; i < DM_GAME_GD_NBUILDS; i++) if (!strcmp(g_builds[i].build, bid)) c.b = &g_builds[i];
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) c.main = (uint64_t)(uintptr_t)g_mod[i].base;
    if (!c.b || !c.main) { *why = "the profile's gamedata.tsv has no row for main's build"; return 0; }
    uint64_t holder;
    if (!peek64(c.main + c.b->slot, &holder) || !holder || !peek64(holder, &c.mgr) || !c.mgr) { *why = "the game has no GameDataMgr yet"; return 0; }
    gd_entry e;
    if (!entry_of(&c, type_named("Int"), &e)) { *why = "the manager's Int entry is not the one the profile describes"; return 0; }
    int64_t v;
    if (!read_text(&c, "place", s->place)) s->place[0] = 0;
    if (!read_text(&c, "map", s->map)) s->map[0] = 0;
    if (read_int(&c, "child", &v)) s->child = (int)v;
    if (read_int(&c, "paused", &v)) s->paused = (int)v;
    if (read_int(&c, "life", &v)) s->life = (int)v;
    if (read_int(&c, "max_life", &v)) s->max_life = (int)v;
    if (read_int(&c, "rupees", &v)) s->rupees = (int)v;
    if (read_int(&c, "play_time", &v)) s->play_time = v;
    if (read_int(&c, "menu", &v)) {
        s->menu = DM_GD_MENU_OTHER;
        for (int i = 0; i < DM_GAME_GD_NMENUS; i++)
            if ((uint32_t)v == g_menus[i].value)
                s->menu = !strcmp(g_menus[i].role, "none") ? DM_GD_MENU_NONE : !strcmp(g_menus[i].role, "title") ? DM_GD_MENU_TITLE
                        : !strcmp(g_menus[i].role, "game_over") ? DM_GD_MENU_GAME_OVER : DM_GD_MENU_OTHER;
    }
    return 1;
}
