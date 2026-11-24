// Multi-module loading in Horizon rtld's role. The code area is read-only+execute once copied in; from then on the loader only writes data.
#include "dm.h"
#include <fcntl.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <mach-o/getsect.h>
#include <mach-o/ldsyms.h>
#include "hle.h"

module_t *g_mod;
int g_nmod;
// Module addresses in g_mod are stable; lookup order follows activation.
pthread_key_t g_key18, g_keytls, g_keytlr, g_keygate;
enum { KEY_K18, KEY_KTLS, KEY_KTLR, KEY_KGATE, KEY_N };

uint64_t resolve_import(module_t *m, const char *name);
int hle_implements(const char *name);
void gate_register(uint64_t ret_addr, const char *kind, int arg, uint64_t orig);

static pthread_key_t key_value(unsigned i) {
    switch (i) {
    case KEY_K18: return g_key18; case KEY_KTLS: return g_keytls; case KEY_KTLR: return g_keytlr;
    default: return g_keygate;
    }
}
static void create_keys(void) {
    if (pthread_key_create(&g_key18, NULL) || pthread_key_create(&g_keytls, NULL) ||
        pthread_key_create(&g_keytlr, NULL) || pthread_key_create(&g_keygate, NULL)) die("cannot create the host key");
    LOG("key numbers K18=%lu KTLS=%lu KTLR=%lu KGATE=%lu", g_key18, g_keytls, g_keytlr, g_keygate);
}

// slot = tpidrro_el0 + 8 * key (fixed pthread TSD layout)
static void tsd_check(void) {
    pthread_setspecific(g_key18, (void *)0x1234);
    uint64_t base; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(base));
    uint64_t off = *(uint64_t *)(g_mod[0].base + g_mod[0].key_page_off + g_mod[0].key_idx[KEY_K18] * 8);
    if (((uint64_t *)(base & ~7ull))[off / 8] != 0x1234) die("TSD layout mismatch");
    pthread_setspecific(g_key18, 0);
}

static void build_symhash(module_t *m);
static void parse_dynamic(module_t *m) {
    uint32_t mod0 = *(uint32_t *)(m->base + 4);
    if (memcmp(m->base + mod0, "MOD0", 4)) die("%s: no MOD0", m->name);
    m->dyn = (const Elf64_Dyn *)(m->base + mod0 + *(int32_t *)(m->base + mod0 + 4));
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_SYMTAB: m->symtab = (const Elf64_Sym *)(m->base + d->d_un.d_ptr); break;
        case DT_STRTAB: m->strtab = (const char *)(m->base + d->d_un.d_ptr); break;
        case DT_INIT: m->init = d->d_un.d_ptr; break;
        case DT_INIT_ARRAY: m->init_array = d->d_un.d_ptr; break;
        case DT_INIT_ARRAYSZ: m->init_arraysz = d->d_un.d_val; break;
    }
    m->nsym = (unsigned)(((const uint8_t *)m->strtab - (const uint8_t *)m->symtab) / sizeof(Elf64_Sym));
    build_symhash(m);
}

static void *module_sym_linear(module_t *m, const char *name) {
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i];
        if (s->st_shndx && !strcmp(m->strtab + s->st_name, name)) return m->base + s->st_value;
    }
    return NULL;
}
// Inserted in index order, the first of a name kept: the same result as a linear scan (first definition).
static uint32_t sym_hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) h = (h ^ (uint8_t)*s++) * 16777619u;
    return h;
}
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;
        }
    }
}
static unsigned long g_symchk_n, g_symchk_bad;
void *module_sym(module_t *m, const char *name) {
    unsigned long nq; (void)nq;
    if (!m->symh) return module_sym_linear(m, name);
    void *r = NULL;
    for (unsigned j = sym_hash(name) & m->symh_mask; m->symh[j]; j = (j + 1) & m->symh_mask) {
        const Elf64_Sym *s = &m->symtab[m->symh[j]];
        if (!strcmp(m->strtab + s->st_name, name)) { r = m->base + s->st_value; break; }
    }
    static int chk = -1; if (chk < 0) chk = getenv("DM_SYMCHECK") != NULL;
    if (chk) { void *l = module_sym_linear(m, name); g_symchk_n++; nq = g_symchk_n;
               if (l != r) { g_symchk_bad++; LOG("[RED] DM_SYMCHECK: %s in %s hash %p ≠ linear %p", name, m->name, r, l); } }
    return r;
}
static void *loaded_sym(const char *name) {
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; }
    return NULL;
}
void *guest_sym(const char *name) {
    void *p = loaded_sym(name);
    return p;
}
// The patch area counts as the module's, before or after the image.
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && (const uint8_t *)p >= b + g_mod[i].patch_off && (const uint8_t *)p < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}

// an undefined dynamic symbol of main: one it calls and another module defines
int dm_game_imports(const char *prefix) {
    size_t n = strlen(prefix);
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        module_t *m = &g_mod[i];
        for (unsigned k = 1; k < m->nsym; k++) if (!m->symtab[k].st_shndx && !strncmp(m->strtab + m->symtab[k].st_name, prefix, n)) return 1;
    }
    return 0;
}

// From the sdk's version string ("NintendoSDK_nnSdk-10_4_0", "NintendoSdk_nnSdk-Generic_20_5_19"); 0 when none
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7) {
            if (p - b < 11 || strncasecmp(p - 11, "NintendoSDK", 11)) continue;
            const char *v = p + 7;
            while (v < e && ((*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z'))) v++;   // a variant name ("Generic") and its '_'
            if (v > p + 7 && v < e && *v == '_') v++;
            if (v < e && *v >= '0' && *v <= '9') {
                major = atoi(v);
                LOG("sdk: NintendoSDK nnSdk-%.20s (major %d)", p + 7, major);
                break;
            }
        }
    }
    if (!major) LOG("[RED] sdk: no SDK version string in the sdk module");
    return major;
}

// Its FS access log line names the spec ("spec: Ounce"; Switch 1: "spec: NX").
int dm_sdk_ounce(void) {
    static int ounce = -1;
    if (ounce >= 0) return ounce;
    ounce = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;
        if (memmem(b, (size_t)(e - b), "spec: Ounce", 11)) ounce = 1;
    }
    if (ounce) LOG("sdk: built for the Switch 2 (spec: Ounce)");
    return ounce;
}

// NVN2's nvnDeviceGetInteger constants move with the SDK, and games check them. Read off the driver's switch: mov w0, #231 / adr or adrp + add / ldrb /
// add, lsl #2 / br, after a cmp wN, #<last>; a constant case is mov w0, #v; b. 0 for a case that reads something else.
int dm_sdk_nvn2_item(unsigned item, int *out) {
    static const uint8_t *table;
    static const uint32_t *cases;
    static unsigned last;
    static int found = -1;
    if (found < 0) {
        found = 0;
        for (int i = 0; i < g_nmod && !found; i++) if (!strcmp(g_mod[i].name, "sdk")) {
            const uint32_t *t = (const uint32_t *)g_mod[i].base; size_t n = g_mod[i].ro_off / 4;
            for (size_t p = 8; p + 9 < n && !found; p++) if (t[p] == 0x52801CE0) {   // mov w0, #231
                const uint8_t *tb = NULL; const uint32_t *adr = NULL; int lim = -1, ldrb = 0, br = 0;
                for (size_t q = p - 8; q <= p + 8; q++) {
                    uint32_t w = t[q];
                    if ((w & 0xFFC0001F) == 0x7100001F) lim = (w >> 10) & 0xFFF;   // cmp wN, #imm
                    else if ((w & 0x9F000000) == 0x90000000 && (t[q + 1] & 0xFFC00000) == 0x91000000) {   // adrp + add
                        int64_t pg = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (pg & 0x100000) pg -= 0x200000;
                        tb = (const uint8_t *)(((uintptr_t)&t[q] & ~(uintptr_t)0xFFF) + (uintptr_t)(pg * 4096) + ((t[q + 1] >> 10) & 0xFFF));
                    } else if ((w & 0x9F000000) == 0x10000000) {   // adr
                        int64_t off = (int64_t)(((w >> 3) & 0x1FFFFC) | ((w >> 29) & 3)); if (off & 0x100000) off -= 0x200000;
                        adr = (const uint32_t *)((const char *)&t[q] + off);
                    } else if ((w & 0xFFE0FC00) == 0x38606800) ldrb = 1;   // ldrb wK, [xT, xI]
                    else if ((w & 0xFFFFFC1F) == 0xD61F0000) br = 1;       // br
                }
                if (!tb || !adr || lim <= 1 || !ldrb || !br) continue;
                const char *ro = (const char *)g_mod[i].base + g_mod[i].ro_off, *ro_end = (const char *)g_mod[i].base + g_mod[i].key_page_off;
                if ((const char *)tb < ro || (const char *)tb + lim >= ro_end) continue;
                table = tb; cases = adr; last = (unsigned)lim; found = 1;
            }
        }
        if (!found) LOG("[RED] sdk: NVN2's nvnDeviceGetInteger switch not found; the device answers the newest versions known");
    }
    if (!found || item > last) return 0;
    const uint32_t *c = cases + table[item];
    if ((c[0] & 0xFFE0001F) != 0x52800000 || (c[1] & 0xFC000000) != 0x14000000) return 0;   // mov w0, #imm16; b
    *out = (int)((c[0] >> 5) & 0xFFFF);
    return 1;
}

static uint64_t resolve_sym(module_t *m, const char *name) {
    void *p = loaded_sym(name); if (p) return (uint64_t)p;
    return resolve_import(m, name);
}

// HLE_FORCE (__DATA,swforce): a hit overrides the module's own definition
static const hle_force_ent *g_force;
static unsigned g_nforce;
static unsigned *g_force_hits;
static unsigned *g_fidx, g_fmask;   // hashed: looked up for every named relocation
static void force_init(void) {
    unsigned long sz; g_force = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &sz);
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    unsigned cap = 16; while (cap < g_nforce * 2) cap <<= 1;
    g_fidx = calloc(cap, sizeof *g_fidx); g_fmask = cap - 1;
    for (unsigned i = 0; i < g_nforce; i++) {   // doubles as the duplicate check
        for (unsigned j = sym_hash(g_force[i].name) & g_fmask;; j = (j + 1) & g_fmask) {
            if (!g_fidx[j]) { g_fidx[j] = i + 1; break; }
            if (!strcmp(g_force[g_fidx[j] - 1].name, g_force[i].name)) die("duplicate entry in the forced takeover list: %s", g_force[i].name);
        }
    }
}
static int force_lookup_quiet(const char *name) {
    if (!g_fmask) return 0;
    for (unsigned j = sym_hash(name) & g_fmask; g_fidx[j]; j = (j + 1) & g_fmask) if (!strcmp(g_force[g_fidx[j] - 1].name, name)) return 1;
    return 0;
}
static void *force_lookup(const char *name) {
    if (!g_fmask) return NULL;
    for (unsigned j = sym_hash(name) & g_fmask; g_fidx[j]; j = (j + 1) & g_fmask) {
        unsigned i = g_fidx[j] - 1;
        if (!strcmp(g_force[i].name, name)) {
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  (%s)", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);
        }
    }
    return NULL;
}
// HLE_ADDON (__DATA,swaddon): an add-on module's imports only
static const hle_force_ent *g_addon;
static unsigned g_naddon, g_addon_hits;
static void addon_init(void) {
    unsigned long sz; g_addon = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swaddon", &sz);
    g_naddon = g_addon ? (unsigned)(sz / sizeof *g_addon) : 0;
    for (unsigned i = 0; i < g_naddon; i++) for (unsigned j = 0; j < i; j++)
        if (!strcmp(g_addon[i].name, g_addon[j].name)) die("duplicate entry in the add-on takeover list: %s", g_addon[i].name);
    for (unsigned i = 0; i < g_naddon; i++) if (force_lookup_quiet(g_addon[i].name)) die("%s is both a forced and an add-on takeover", g_addon[i].name);
}
static void *addon_lookup(const char *name) {
    for (unsigned i = 0; i < g_naddon; i++) if (!strcmp(g_addon[i].name, name)) {
        g_addon_hits++;
        if (g_flag.imports) LOG("add-on takeover %s  (%s)", name, g_addon[i].why);
        void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("ADDON", name, g_addon[i].fn);
    }
    return NULL;
}
// An entry no module names is a leftover or a typo: reported
static void force_check(void) {
    unsigned unused = 0;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { LOG("[RED] forced takeover entry no module references: %s", g_force[i].name); unused++; }
    LOG("force: %u entries hit, %u not referenced", g_nforce - unused, unused);
}

// Rows beyond the header are an old-format build output (code modified at load time): rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[KEY_N] = {"K18", "KTLS", "KTLR", "KGATE"};
    char line[256], key[48]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %47s %lx", key, &v) != 2) die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);   // negative: patch area before the base
        else if (!strcmp(key, "IMAGE_SIZE")) m->image_size = v;
        else if (!strcmp(key, "FILE_IMAGE_LEN")) m->file_image_len = v;
        else if (!strcmp(key, "BACK_OFF")) m->back_off = v;
        else if (!strcmp(key, "BACK_LEN")) m->back_len = v;
        else if (!strcmp(key, "KEY_PAGE_OFF")) m->key_page_off = v;
        else if (!strcmp(key, "KEY_PAGE_SIZE")) m->key_page_size = v;
        else if (!strcmp(key, "TEXT_END")) m->text_end = v;
        else if (!strcmp(key, "RO_OFF")) m->ro_off = v;
        else if (!strcmp(key, "DATA_OFF")) m->data_off = v;
        else if (!strcmp(key, "FPS_STATE_OFF")) m->fps_state_off = v;
        else if (!strcmp(key, "BUILD_ID")) sscanf(line, "# %*s %32s", m->build_id);
        else for (int k = 0; k < KEY_N; k++) {
            size_t n = strlen(kn[k]);
            if (!strncmp(key, "KEY_", 4) && !strncmp(key + 4, kn[k], n) && !strcmp(key + 4 + n, "_IDX")) {
                m->key_idx[k] = (unsigned)strtoul(strrchr(line, '\t') + 1, 0, 10); seen |= 1u << k;
            }
        }
    }
    fclose(f);
    if (!m->patch_off || m->key_page_off % 0x4000 || m->key_page_size != 0x4000 || seen != (1u << KEY_N) - 1)
        die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
    if (m->patch_off < 0 && (!m->image_size || !m->file_image_len || m->file_image_len % 0x4000 || m->patch_off % 0x4000 || m->key_page_off + m->key_page_size > m->image_size))
        die("%s: patch area before the base, but IMAGE_SIZE / FILE_IMAGE_LEN missing or wrong", path);
    if ((m->back_off || m->back_len) && (m->patch_off >= 0 || !m->back_len || m->back_off % 0x4000 || m->back_len % 0x4000 || m->back_off < m->key_page_off + m->key_page_size
                                         || m->back_off + m->back_len != m->image_size))
        die("%s: back patch area, but BACK_OFF / BACK_LEN missing or wrong", path);
}

// Frame step page (patch_nso.py FRAME_STEP): the stubs step by whole vsyncs capped at the target, 0 = the game's own logic.
uint8_t *dm_fps_state;
static const char *g_fps_mod;
static void install_fps_state(module_t *m) {
    if (m->fps_state_off + 0x40 > m->size || m->fps_state_off < m->key_page_off + m->key_page_size) die("%s: frame step state page out of range", m->name);
    dm_fps_state = m->base + m->fps_state_off; g_fps_mod = m->name;
}
// Nothing reads the page before the game runs.
void dm_fps_state_apply(void) {
    if (!dm_fps_state) return;
    const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; using the game's own frame step", e); t = 0; }
    float f = (float)t; memcpy(dm_fps_state, &f, sizeof f);
    // RENDER_SCALE: 1.0 keeps the game's size and dynamic resolution; 0.5 to under 1.0 renders the 3D views at that fraction, dynamic resolution held at top.
    const char *rs = getenv("DM_RENDER_SCALE"); float sc = 1.0f;
    if (rs) { char *end; sc = strtof(rs, &end); if (end == rs || *end || !(sc >= 0.5f && sc <= 1.0f)) { LOG("[RED] DM_RENDER_SCALE=%s: must be 0.5 to 1.0; keeping the game's size", rs); sc = 1.0f; }
              else if (sc < 1.0f) LOG("%s: 3D views rendered at %.2f of the game's size (DM_RENDER_SCALE)", g_fps_mod, sc); }
    memcpy(dm_fps_state + 0x10, &sc, sizeof sc);
    // DYNAMIC_RESOLUTION: 1 lets the game's run, 0 holds full resolution; ignored by a build without that stub.
    const char *dr = getenv("DM_DYNAMIC_RESOLUTION"); uint32_t dyn = dr && !strcmp(dr, "1");
    if (dr && strcmp(dr, "0") && strcmp(dr, "1")) LOG("[RED] DM_DYNAMIC_RESOLUTION=%s: must be 0 or 1; keeping full resolution", dr);
    memcpy(dm_fps_state + 0x40, &dyn, sizeof dyn);
    char cap[32]; snprintf(cap, sizeof cap, ", capped at %d fps", t);
    LOG("%s: frame step %s%s (%s)", g_fps_mod, !t ? "as the game sets it" : "in whole vsyncs decided by the host", t ? cap : "", e ? "DM_FPS_TARGET" : "game profile fps_target");
}
static void install_key_page(module_t *m) {
    if (m->key_page_off + m->key_page_size > m->size) die("%s: key page out of range", m->name);
    uint64_t *page = (uint64_t *)(m->base + m->key_page_off);
    memset(page, 0, m->key_page_size);
    for (unsigned i = 0; i < KEY_N; i++) page[m->key_idx[i]] = (uint64_t)key_value(i) * 8;
    for (int k = 0; k < KEY_N; k++) if (!page[m->key_idx[k]]) die("%s: key page entry %d is 0 (host key not created) -- the game would read TSD slot 0", m->name, k);
    if (mprotect(m->base + m->key_page_off, m->key_page_size, PROT_READ)) die("%s key page mprotect", m->name);
    if (m->fps_state_off) install_fps_state(m);
}

static void register_gates(module_t *m, const char *dir) {
    char path[1024], line[256]; snprintf(path, sizeof path, "%s/%s.gates.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    int ng = 0;
    if (f) {
        unsigned long ret, orig; char kind[16]; int arg;
        while (fgets(line, sizeof line, f))
            if (sscanf(line, "%lx %15s %d %lx", &ret, kind, &arg, &orig) == 4) { gate_register((uint64_t)m->base + ret, kind, arg, orig); ng++; }
        fclose(f);
    }
    LOG("%s: gates %d patch_off %#llx keys %lu/%lu/%lu/%lu", m->name, ng, m->patch_off, g_key18, g_keytls, g_keytlr, g_keygate);
}

// DT_RELR (SDK 20+): even entry = offset to relocate (*slot += base); odd = bitmap of the 63 slots after the pointer. Main's RELA holds only symbol relocations.
static unsigned relr_apply(module_t *m, uint64_t relr, uint64_t relrsz, uint64_t delta) {
    const uint64_t *e = (const uint64_t *)(m->base + relr); uint64_t *where = NULL; unsigned n = 0;
    for (uint64_t i = 0; i < relrsz / 8; i++) {
        uint64_t v = e[i];
        if (!(v & 1)) { where = (uint64_t *)(m->base + v); *where++ += delta; n++; continue; }
        if (!where) die("%s: DT_RELR bitmap entry with no start before it", m->name);
        for (unsigned b = 0; (v >>= 1); b++) if (v & 1) { where[b] += delta; n++; }
        where += 63;
    }
    return n;
}

// As Horizon's rtld: forced takeover, then the loaded modules in load order, then HLE / stub. *weak0: an undefined weak nothing defines, bound to 0
static uint64_t bind_sym(module_t *m, const Elf64_Sym *s, int *other, int *weak0) {
    uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
    if (!v && m->addon) v = (uint64_t)addon_lookup(m->strtab + s->st_name);
    *other = *weak0 = 0;
    // Own definitions are also looked up globally (ELF default, as Horizon's rtld): a main defining operator new / delete gets sdk's frees too.
    if (!v && s->st_shndx) {
        uint64_t self = (uint64_t)m->base + s->st_value;
        v = ELF64_ST_BIND(s->st_info) == STB_LOCAL || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT ? self : resolve_sym(m, m->strtab + s->st_name);
        if (!v) v = self;
        *other = v != self;
    }
    // 0, as Horizon's rtld binds it: the code tests the address, and a stub would look defined.
    const char *nm = m->strtab + s->st_name;
    *weak0 = !v && !s->st_shndx && ELF64_ST_BIND(s->st_info) == STB_WEAK && !loaded_sym(nm) && !hle_implements(nm);
    if (!v && !*weak0) v = resolve_sym(m, nm);
    return v;
}

typedef struct { const Elf64_Rela *tab[2]; uint64_t n[2]; uint64_t relr, relrsz; } relocs_t;
static relocs_t relocs_of(const module_t *m) {
    uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0; relocs_t r = {0};
    for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
        case DT_RELA: rela = d->d_un.d_ptr; break;
        case DT_RELASZ: relasz = d->d_un.d_val; break;
        case DT_JMPREL: jmprel = d->d_un.d_ptr; break;
        case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
        case DT_RELR: r.relr = d->d_un.d_ptr; break;
        case DT_RELRSZ: r.relrsz = d->d_un.d_val; break;
    }
    r.tab[0] = (const Elf64_Rela *)(m->base + rela); r.tab[1] = (const Elf64_Rela *)(m->base + jmprel);
    r.n[0] = relasz / sizeof(Elf64_Rela); r.n[1] = pltsz / sizeof(Elf64_Rela);
    return r;
}

static void relocate(module_t *m) {
    relocs_t rl = relocs_of(m);
    int cnt[3] = {0}, nint = 0, nweak0 = 0;
    unsigned nrelr = rl.relr ? relr_apply(m, rl.relr, rl.relrsz, (uint64_t)m->base) : 0;
    for (int t = 0; t < 2; t++)
        for (uint64_t i = 0; i < rl.n[t]; i++) {
            const Elf64_Rela *r = &rl.tab[t][i];
            uint64_t *where = (uint64_t *)(m->base + r->r_offset);
            unsigned type = ELF64_R_TYPE(r->r_info), si = ELF64_R_SYM(r->r_info);
            if (type == R_AARCH64_RELATIVE) { *where = (uint64_t)m->base + r->r_addend; cnt[0]++; continue; }
            const Elf64_Sym *s = &m->symtab[si]; int other, weak0;
            uint64_t v = bind_sym(m, s, &other, &weak0);
            if (other && nint++ < 8) LOG("%s: %s bound to an earlier module's definition (%s)", m->name, m->strtab + s->st_name, module_of((void *)v) ? module_of((void *)v)->name : "?");
            if (weak0 && nweak0++ < 8) LOG("%s: undefined weak %s bound to 0", m->name, m->strtab + s->st_name);
            if (type == R_AARCH64_JUMP_SLOT) { *where = v; cnt[2]++; }
            else { *where = v + r->r_addend; cnt[1]++; }       // ABS64 / GLOB_DAT
        }
    LOG("%s: reloc relative=%d relr=%u abs/glob=%d jump=%d%s", m->name, cnt[0], nrelr, cnt[1], cnt[2], nint ? " (some bound to an earlier module's definition)" : "");
    if (nint > 8) LOG("%s: %d bound to an earlier module's definition in all", m->name, nint);
}

// DM_LOADHASH=1: an address-independent sha256 per module (relocation slots replaced by tokens), to compare load results between runs.
#include <CommonCrypto/CommonDigest.h>
void loader_hash_log(void) {
    if (!getenv("DM_LOADHASH")) return;
    for (int k = 0; k < g_nmod; k++) {
        module_t *m = &g_mod[k];
        uint8_t *cp = malloc(m->size); memcpy(cp, m->base, m->size);
        uint64_t rela = 0, relasz = 0, jmprel = 0, pltsz = 0, relr = 0, relrsz = 0;
        for (const Elf64_Dyn *d = m->dyn; d->d_tag; d++) switch (d->d_tag) {
            case DT_RELA: rela = d->d_un.d_ptr; break; case DT_RELASZ: relasz = d->d_un.d_val; break;
            case DT_JMPREL: jmprel = d->d_un.d_ptr; break; case DT_PLTRELSZ: pltsz = d->d_un.d_val; break;
            case DT_RELR: relr = d->d_un.d_ptr; break; case DT_RELRSZ: relrsz = d->d_un.d_val; break;
        }
        if (relr) {
            uint8_t *b = m->base;
            m->base = cp;
            relr_apply(m, relr, relrsz, -(uint64_t)b);
            m->base = b;
        }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) {
                        tok[0] = 1 + j;
                        hit = 1;
                        break;
                    }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);
                nslot++;
            }
        CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, cp, (CC_LONG)m->size);
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tail = (uint64_t)r->r_addend;
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset);
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tail = v - (uint64_t)g_mod[j].base; break; }
                }
                CC_SHA256_Update(&c, &tail, 8);
            }
        uint8_t d[32]; CC_SHA256_Final(d, &c); free(cp);
        char hx[65]; for (int i = 0; i < 32; i++) sprintf(hx + 2 * i, "%02x", d[i]);
        LOG("DM_LOADHASH %s size %#zx relocation slots %u sha256 %s", m->name, m->size, nslot, hx);
    }
}

static uint8_t *slurp(const char *path, size_t *n) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) die("open %s", path);
    struct stat st; fstat(fd, &st);
    uint8_t *p = malloc(st.st_size);
    if (read(fd, p, st.st_size) != st.st_size) die("read %s", path);
    close(fd); *n = st.st_size; return p;
}

// The code area is sealed right after copying: any later write to it crashes. What shares a 16 KB page with ro and data stays writable.
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {
        // Patch area before the base: the file holds image + patch area; bss and the key page are zero pages; the gap to the base is PROT_NONE.
        // A back patch area (patch_nso.py TWO) ends the file, BACK_LEN bytes copied to BACK_OFF.
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len, bl = m->back_len;
        if (sz < fl + bl) die("%s is shorter than FILE_IMAGE_LEN %#llx + BACK_LEN %#llx", path, (unsigned long long)fl, (unsigned long long)bl);
        m->size = m->image_size; m->patch_len = (sz - bl - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s patch area of %#llx bytes does not fit in the %#llx before the base", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
        uint8_t *a = mmap(NULL, pre + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
        m->base = a + pre;
        memcpy(m->base, raw, fl);
        memcpy(a, raw + fl, sz - bl - fl);
        if (bl) memcpy(m->base + m->back_off, raw + sz - bl, bl);
        free(raw); install_key_page(m); dm_mods_patch_module(m, dir);
        if (bl && mprotect(m->base + m->back_off, bl, PROT_READ | PROT_EXEC)) die("%s mprotect back patch", m->name);
        uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
        if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
        if (mprotect(a, m->patch_len, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
        if (pre > m->patch_len && mprotect(a + m->patch_len, pre - m->patch_len, PROT_NONE)) die("%s mprotect gap", m->name);
        return;
    }
    m->size = (sz + 0x3FFF) & ~0x3FFFull;
    m->base = mmap(NULL, m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (m->base == MAP_FAILED) die("%s mmap", m->name);
    memcpy(m->base, raw, sz); free(raw); install_key_page(m); dm_mods_patch_module(m, dir);
    uint64_t rx = (m->text_end + 0x3FFF) & ~0x3FFFull;
    if (mprotect(m->base, rx, PROT_READ | PROT_EXEC)) die("%s mprotect text", m->name);
    uint64_t po = m->patch_off & ~0x3FFFull;
    if (mprotect(m->base + po, m->size - po, PROT_READ | PROT_EXEC)) die("%s mprotect patch", m->name);
}


void load_modules(const char *dir, const char **names, const char *const *dirs, int n) {
    // Map all and parse their symbols first: cross-module symbols must be in place before relocating.
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    if (n <= 0) die("no modules to load");
    if (!(g_mod = calloc((size_t)n, sizeof *g_mod))) die("cannot allocate %d module slots", n);   // once: module addresses stay stable
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        m->addon = dirs && dirs[i];
        strlcpy(m->dir, m->addon ? dirs[i] : dir, sizeof m->dir);
        read_layout(m, m->dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, m->dir); tm += now_ns() - t;
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, m->dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx%s%s", names[i], m->base, m->size, m->addon ? ", an add-on from " : "", m->addon ? m->dir : "");
        g_nmod++;
    }
    force_init(); addon_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    for (int i = 0; i < g_nmod; i++) if (g_mod[i].addon) {
        LOG("add-on takeovers: %u imports of the add-ons bound to them (%u entries)", g_addon_hits, g_naddon);
        break;
    }
    { void hlecount_seal(void); hlecount_seal(); }
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK: %lu symbol lookups at load time, hash and linear disagree %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① breakdown: mapping (read + copy + seal pages) %.1f ms, dynamic section parsing %.1f ms, gate registration %.1f ms, relocation + forced takeover %.1f ms, total %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}

// Reset: each module's writable part as it was at first start, copied back; code, patch areas and key pages stay, so gates and looked-up functions stay valid.
typedef struct { uint8_t *lo; size_t n; uint8_t **page; } mod_snap;   // page[i]: a copy of 16 KB page i, NULL for zeros
static mod_snap *g_snap;
static size_t g_snap_bytes;
static void mod_writable(const module_t *m, uint8_t **lo, size_t *n) {
    uint64_t a = (m->text_end + 0x3FFF) & ~0x3FFFull, b = m->patch_off > 0 ? ((uint64_t)m->patch_off & ~0x3FFFull) : m->back_len ? m->back_off : m->size;
    *lo = m->base + a; *n = b > a ? b - a : 0;
}
static int in_key_page(const module_t *m, const uint8_t *p) { return p >= m->base + m->key_page_off && p < m->base + m->key_page_off + m->key_page_size; }
void loader_snapshot(void) {
    static const uint8_t zero[0x4000];
    if (!(g_snap = calloc((size_t)g_nmod, sizeof *g_snap))) die("module snapshot");
    for (int i = 0; i < g_nmod; i++) {
        mod_snap *s = &g_snap[i]; mod_writable(&g_mod[i], &s->lo, &s->n);
        if (!(s->page = calloc(s->n / 0x4000 + 1, sizeof *s->page))) die("module snapshot");
        for (size_t o = 0; o < s->n; o += 0x4000) {
            uint8_t *p = s->lo + o;
            if (in_key_page(&g_mod[i], p) || !memcmp(p, zero, 0x4000)) continue;
            if (!(s->page[o / 0x4000] = malloc(0x4000))) die("module snapshot");
            memcpy(s->page[o / 0x4000], p, 0x4000); g_snap_bytes += 0x4000;
        }
    }
}
void loader_reset(void) {
    for (int i = 0; i < g_nmod; i++) {
        mod_snap *s = &g_snap[i]; const module_t *m = &g_mod[i];
        for (size_t o = 0; o < s->n; o += 0x4000) {
            uint8_t *p = s->lo + o;
            if (in_key_page(m, p)) continue;
            if (mprotect(p, 0x4000, PROT_READ | PROT_WRITE)) die("reset: %s+%#lx cannot be made writable (errno %d)", m->name, (unsigned long)(p - m->base), errno);
            if (s->page[o / 0x4000]) memcpy(p, s->page[o / 0x4000], 0x4000); else memset(p, 0, 0x4000);
        }
    }
    LOG("reset: the modules' data and bss as at the first start (%.1f MB kept of it)", g_snap_bytes / 1048576.0);
}
// rtld's InitModules: DT_INIT in reverse load order, so the sdk's libc++ globals exist before main's static initializers. Called by nn::init::Start before nnMain.
void run_module_inits(void) {
    for (int i = g_nmod - 1; i >= 0; i--) {
        module_t *m = &g_mod[i];
        if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx (init_array %llu entries)", m->name, m->init, m->init_arraysz / 8);
        ((void (*)(void))(m->base + m->init))();
    }
}
