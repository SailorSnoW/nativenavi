// Multi-module loading (playing the role of Horizon's rtld): mapping, claiming thread slots, gate registration, cross-module relocation, take-over by name, segment permissions.
// The code area (original text and patch area) is set to read-only+execute as soon as it is copied in, and from then on the loader only writes data: on iOS these two blocks must be embedded as is as signed code segments.
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
// Stable module addresses live in g_mod; lookup order follows activation, including unload/reload.
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

// K slot check: the pthread TSD layout is fixed, slot = tpidrro_el0 + 8*key
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
// Symbol name hash table: inserted in ascending index order, not overwritten if the same name is already there ⇒ lookup results are identical, entry by entry, to a linear scan (the first defined symbol with that name).
// Of the 91–99 ms of ① host loading, relocation + forced take-over took 77–86 ms, mainly because every by-name relocation scanned the symbol tables of two modules linearly
static uint32_t sym_hash(const char *s) { uint32_t h = 2166136261u; while (*s) h = (h ^ (uint8_t)*s++) * 16777619u; return h; }
static void build_symhash(module_t *m) {
    unsigned cap = 16; while (cap < m->nsym * 2) cap <<= 1;
    m->symh = calloc(cap, sizeof *m->symh); m->symh_mask = cap - 1;
    for (unsigned i = 1; i < m->nsym; i++) {
        const Elf64_Sym *s = &m->symtab[i]; if (!s->st_shndx) continue;
        const char *nm = m->strtab + s->st_name;
        for (unsigned j = sym_hash(nm) & m->symh_mask;; j = (j + 1) & m->symh_mask) {
            if (!m->symh[j]) { m->symh[j] = i; break; }
            if (!strcmp(m->strtab + m->symtab[m->symh[j]].st_name, nm)) break;   // The same name already exists with a smaller index: keep that one
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
static void *loaded_sym(const char *name) {   // the module list is fixed once load_modules has mapped them
    for (int i = 0; i < g_nmod; i++) { void *p = module_sym(&g_mod[i], name); if (p) return p; }   // in load order
    return NULL;
}
void *guest_sym(const char *name) {
    void *p = loaded_sym(name);
    return p;
}
// For modules with the patch area before the base (patch_off < 0) addresses in the patch area count too, as for modules whose patch area follows the image (crash reports, the GC pause's "in guest code" test)
module_t *module_of(const void *p) {
    for (int i = 0; i < g_nmod; i++) {
        const uint8_t *b = g_mod[i].base;
        if ((const uint8_t *)p >= b && (const uint8_t *)p < b + g_mod[i].size) return &g_mod[i];
        if (g_mod[i].patch_off < 0 && (const uint8_t *)p >= b + g_mod[i].patch_off && (const uint8_t *)p < b + g_mod[i].patch_off + g_mod[i].patch_len) return &g_mod[i];
    }
    return NULL;
}

// Whether main imports a symbol whose name starts with prefix (an undefined dynamic symbol: one main calls but another module defines), for behaviour that follows what the game uses
int dm_game_imports(const char *prefix) {
    size_t n = strlen(prefix);
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "main")) {
        module_t *m = &g_mod[i];
        for (unsigned k = 1; k < m->nsym; k++) if (!m->symtab[k].st_shndx && !strncmp(m->strtab + m->symtab[k].st_name, prefix, n)) return 1;
    }
    return 0;
}

// SDK major version, from the version string the SDK's libraries keep in the sdk module's read-only data ("NintendoSDK_nnSdk-10_4_0"; SDK 12 spells it NintendoSdk_;
// Switch 2 SDKs name a variant first: "NintendoSdk_nnSdk-Generic_20_5_19", Donkey Kong Bananza).
// 0 = no sdk module or no version string
int dm_sdk_major(void) {
    static int major = -1;
    if (major >= 0) return major;
    major = 0;
    for (int i = 0; i < g_nmod; i++) if (!strcmp(g_mod[i].name, "sdk")) {
        const char *b = (const char *)g_mod[i].base + g_mod[i].ro_off, *e = (const char *)g_mod[i].base + g_mod[i].key_page_off;   // read-only data, data and bss
        for (const char *p = b; (p = memmem(p, (size_t)(e - p), "_nnSdk-", 7)); p += 7) {
            if (p - b < 11 || strncasecmp(p - 11, "NintendoSDK", 11)) continue;
            const char *v = p + 7;
            while (v < e && ((*v >= 'A' && *v <= 'Z') || (*v >= 'a' && *v <= 'z'))) v++;   // a variant name ("Generic") and its '_'
            if (v > p + 7 && v < e && *v == '_') v++;
            if (v < e && *v >= '0' && *v <= '9') { major = atoi(v); LOG("sdk: NintendoSDK nnSdk-%.20s (major %d)", p + 7, major); break; }
        }
    }
    if (!major) LOG("[RED] sdk: no SDK version string in the sdk module");
    return major;
}

// Whether the sdk module was built for the Switch 2 ("Ounce"): its file system access log line names the build spec ("FS_ACCESS: { sdk_version: 20.5.19, spec: Ounce }";
// Switch 1 SDKs say "spec: NX"). The host has only the Switch 2's side: main.c reports an sdk that is not
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

// The constants NVN2's own nvnDeviceGetInteger answers: the driver is compiled into the game's sdk, and its API and GPU code versions move with the SDK (20.5.19: API 231.30, GPU code minor 17;
// 21.4.4: 231.39 and 20). Games check them: Splatoon Raiders requires minor >= 39 and otherwise creates no graphics system (main+0x31b86b0). The driver's switch over the items loads w0 with the
// major first, then jumps through a byte table: mov w0, #231 / adr / adrp + add (the table) / ldrb / add, lsl #2 / br, with cmp wN, #<last item> before it. A case that is a constant is mov w0, #v; b.
// Returns 1 with *out set when item has such a case; 0 for a case that reads something else (most items go through the driver's table lookup or read the GPU's description), or no such switch
int dm_sdk_nvn2_item(unsigned item, int *out) {
    static const uint8_t *table; static const uint32_t *cases; static unsigned last; static int found = -1;
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

// Cross-module symbol resolution: first look for a definition in the loaded modules, and only if not found hand over to HLE / stubs
static uint64_t resolve_sym(module_t *m, const char *name) {
    void *p = loaded_sym(name); if (p) return (uint64_t)p;
    return resolve_import(m, name);
}

// Forced take-over table (hle.h HLE_FORCE, the __DATA,swforce section): a hit overrides the definition inside the module
static const hle_force_ent *g_force; static unsigned g_nforce; static unsigned *g_force_hits;
static unsigned *g_fidx, g_fmask;   // name hash over the table: force_lookup runs for every named relocation, and each lookup used to strcmp its way through all 722 entries (the swhle table's scan is hashed the same way, hle.c)
static void force_init(void) {
    unsigned long sz; g_force = (const hle_force_ent *)getsectiondata(&_mh_execute_header, "__DATA", "swforce", &sz);
    g_nforce = g_force ? (unsigned)(sz / sizeof *g_force) : 0;
    g_force_hits = calloc(g_nforce + 1, sizeof *g_force_hits);
    unsigned cap = 16; while (cap < g_nforce * 2) cap <<= 1;
    g_fidx = calloc(cap, sizeof *g_fidx); g_fmask = cap - 1;
    for (unsigned i = 0; i < g_nforce; i++) {   // insertion doubles as the duplicate check (a wrong list must still fail loudly, at once)
        for (unsigned j = sym_hash(g_force[i].name) & g_fmask;; j = (j + 1) & g_fmask) {
            if (!g_fidx[j]) { g_fidx[j] = i + 1; break; }
            if (!strcmp(g_force[g_fidx[j] - 1].name, g_force[i].name)) die("duplicate entry in the forced takeover list: %s", g_force[i].name);
        }
    }
}
static void *force_lookup(const char *name) {
    if (!g_fmask) return NULL;
    for (unsigned j = sym_hash(name) & g_fmask; g_fidx[j]; j = (j + 1) & g_fmask) {
        unsigned i = g_fidx[j] - 1;
        if (!strcmp(g_force[i].name, name)) {
            if (!g_force_hits[i]++ && g_flag.imports) LOG("force %s  (%s)", name, g_force[i].why);
            void *hlecount_wrap(const char *, const char *, void *); return hlecount_wrap("FORCE", name, g_force[i].fn);   // A counting stub when DM_HLECOUNT is set
        }
    }
    return NULL;
}
// The list is this game's: an entry no module names is a leftover or a mistyped name, and is reported
static void force_check(void) {
    unsigned unused = 0;
    for (unsigned i = 0; i < g_nforce; i++)
        if (!g_force_hits[i]) { LOG("[RED] forced takeover entry no module references: %s", g_force[i].name); unused++; }
    LOG("force: %u entries hit, %u not referenced", g_nforce - unused, unused);
}

// Read the rewriter table header (segment boundaries, slot numbers); any row beyond the header means an old-format build output (code modified at load time), which is rejected
static void read_layout(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.fixups.tsv", dir, m->name);
    FILE *f = fopen(path, "r");
    if (!f) die("open %s", path);
    static const char *kn[KEY_N] = {"K18", "KTLS", "KTLR", "KGATE"};
    char line[256], key[48]; unsigned long v; unsigned seen = 0;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "# %47s %lx", key, &v) != 2) die("%s: the output comes from an old rewriter; rewrite the modules again with dm/tools/patch_nso.py", path);
        if (!strcmp(key, "PATCH_OFF")) m->patch_off = strtoll(line + 2 + strlen(key), NULL, 16);   // may be negative (patch area before the base)
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

// Frame step state (the page after the key page, read-write; layout in patch_nso.py FRAME_STEP): the rewriter's stubs step the game by the host's whole vsyncs capped at this
// target, 0 = the game's own interval logic. The profile's fps_target, overridden by DM_FPS_TARGET (0, 30 or 60); the graphics layer decides the steps and paces presents
// (hle_nvn.c fstep_tick)
uint8_t *dm_fps_state;
static const char *g_fps_mod;   // the module holding the state page (for the log)
static void install_fps_state(module_t *m) {
    if (m->fps_state_off + 0x40 > m->size || m->fps_state_off < m->key_page_off + m->key_page_size) die("%s: frame step state page out of range", m->name);
    dm_fps_state = m->base + m->fps_state_off; g_fps_mod = m->name;
}
// The settings in the state page, written when the game starts (main.c, after host loading): nothing reads the page before the game runs
void dm_fps_state_apply(void) {
    if (!dm_fps_state) return;
    const char *e = getenv("DM_FPS_TARGET"); int t = e ? atoi(e) : DM_GAME_FPS_TARGET;
    if (t != 0 && t != 30 && t != 60) { LOG("[RED] DM_FPS_TARGET=%s: must be 0, 30 or 60; using the game's own frame step", e); t = 0; }
    float f = (float)t; memcpy(dm_fps_state, &f, sizeof f);
    // The game's own dynamic resolution, which the level function's stub reads (the DYNAMIC_RESOLUTION capability of the frame step stubs): 1 lets it run, 0 (the default) holds
    // full resolution. A build without that stub ignores the field
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

// DT_RELR (from SDK 20; all three modules of The Legend of Zelda: Breath of the Wild 1.9.0 have it): a compact table of relative relocations with the addend in the slot (*slot += base).
// Each 8-byte entry: even = a new start offset (relocate it, the pointer moves to the next slot); odd = a bitmap whose bits 1-63 stand for the 63 slots after the pointer, which then moves on by 63 slots.
// Without it main's GOT and vtables hold module offsets (main's RELA keeps only symbol relocations)
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

// The address a symbol relocation of module m binds to, as Horizon's rtld would with the modules loaded now (forced take-over, then the loaded modules in load order, then HLE / stub).
// *other: the module's own definition was taken over by an earlier module's; *weak0: an undefined weak symbol nothing defines, bound to 0
static uint64_t bind_sym(module_t *m, const Elf64_Sym *s, int *other, int *weak0) {
    uint64_t v = (uint64_t)force_lookup(m->strtab + s->st_name);
    *other = *weak0 = 0;
    // Symbols the module defines itself are also looked up globally in load order (the ELF default; Horizon's rtld does the same): the definition of an earlier-loaded module overrides a later one; only local / non-default visibility binds to itself.
    // Engage's main defines its own operator new / delete (Unity's memory manager) and sdk has a copy too (forwarding to malloc / free): previously sdk bound its own delete,
    // so blocks from main's new were freed by sdk's libc++ string code into the SDK heap, and CentralHeap::Free reported NotNMallocPtr
    if (!v && s->st_shndx) {
        uint64_t self = (uint64_t)m->base + s->st_value;
        v = ELF64_ST_BIND(s->st_info) == STB_LOCAL || ELF64_ST_VISIBILITY(s->st_other) != STV_DEFAULT ? self : resolve_sym(m, m->strtab + s->st_name);
        if (!v) v = self;
        *other = v != self;
    }
    // An undefined weak symbol that no module defines and the host does not implement is 0, as Horizon's rtld binds it: the code tests its address before calling it.
    // Octopath Traveler 0's main imports the thread_local init function _ZTHN13LowLevelTasks5FTask10ActiveTaskE weakly; a named stub made it look defined and the game called it
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

// DM_LOADHASH=1 (diagnostics): after loading, output an address-independent sha256 per module, to prove that the load results of "early start / no early start" are byte-identical.
// Module base addresses and host ASLR differ every run, and only the values of the relocation slots change with them: copy the image and, using the same relocation table, replace every slot with an address-independent token --
// RELATIVE → (0, addend); symbol target falls in some module → (1 + module number, offset within the module); otherwise (host forced take-over / host symbol) → (name FNV, addend); all other bytes as is.
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
        // DT_RELR slots: subtracting the base makes them address-independent (done on the copy, reusing relr_apply's walk)
        if (relr) { uint8_t *b = m->base; m->base = cp; relr_apply(m, relr, relrsz, -(uint64_t)b); m->base = b; }
        const Elf64_Rela *tabs[2] = {(const Elf64_Rela *)(m->base + rela), (const Elf64_Rela *)(m->base + jmprel)};
        uint64_t sizes[2] = {relasz, pltsz}; unsigned nslot = 0;
        for (int t = 0; t < 2; t++)
            for (uint64_t i = 0; i < sizes[t] / sizeof(Elf64_Rela); i++) {
                const Elf64_Rela *r = &tabs[t][i]; uint64_t tok[1] = {0};
                if (ELF64_R_TYPE(r->r_info) != R_AARCH64_RELATIVE) {
                    uint64_t v = *(const uint64_t *)(m->base + r->r_offset); int hit = 0;
                    for (int j = 0; j < g_nmod; j++) if (v >= (uint64_t)g_mod[j].base && v < (uint64_t)g_mod[j].base + g_mod[j].size) { tok[0] = 1 + j; hit = 1; break; }
                    if (!hit) { const char *nm = m->strtab + m->symtab[ELF64_R_SYM(r->r_info)].st_name; uint64_t h = 0xcbf29ce484222325ull;
                                for (; *nm; nm++) h = (h ^ (uint8_t)*nm) * 0x100000001b3ull; tok[0] = h | 1ull << 63; }
                }
                memcpy(cp + r->r_offset, &tok[0], 8);                // The 8 slot bytes are replaced by the token head; the token tail is appended below in slot order
                nslot++;
            }
        CC_SHA256_CTX c; CC_SHA256_Init(&c); CC_SHA256_Update(&c, cp, (CC_LONG)m->size);
        for (int t = 0; t < 2; t++)                                // Token tails (addend / offset) are appended in slot order, and together with the slot head uniquely determine the slot value
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

// Seal the code area immediately after copying into anonymous pages (16 KB pages): [0, text_end) and the patch area are read-only+execute, and from then on any write to the code area crashes on the spot;
// what lies between (ro and data on the same page can only go together) is writable
static void map_image(module_t *m, const char *dir) {
    char path[1024]; snprintf(path, sizeof path, "%s/%s.patched.bin", dir, m->name);
    size_t sz; uint8_t *raw = slurp(path, &sz);
    if (m->patch_off < 0) {
        // Patch area before the base: file = the first part of the image (text + ro + data) + patch area; bss and the key page are anonymous zero pages. The gap from the patch area's end to the base is left inaccessible (PROT_NONE)
        // With a back patch area (patch_nso.py TWO) the file ends with it, BACK_LEN bytes, copied to BACK_OFF in the image
        uint64_t pre = (uint64_t)-m->patch_off, fl = m->file_image_len, bl = m->back_len;
        if (sz < fl + bl) die("%s is shorter than FILE_IMAGE_LEN %#llx + BACK_LEN %#llx", path, (unsigned long long)fl, (unsigned long long)bl);
        m->size = m->image_size; m->patch_len = (sz - bl - fl + 0x3FFF) & ~0x3FFFull;
        if (m->patch_len > pre) die("%s patch area of %#llx bytes does not fit in the %#llx before the base", path, (unsigned long long)m->patch_len, (unsigned long long)pre);
        uint8_t *a = mmap(NULL, pre + m->size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
        if (a == MAP_FAILED) die("%s mmap %#llx", m->name, (unsigned long long)(pre + m->size));
        m->base = a + pre;
        memcpy(m->base, raw, fl); memcpy(a, raw + fl, sz - bl - fl); if (bl) memcpy(m->base + m->back_off, raw + sz - bl, bl);
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


void load_modules(const char *dir, const char **names, int n) {
    // Map everything and parse the symbol tables first, then relocate all at once (cross-module symbols must all be in place first)
    // DM_BOOTPROF: breakdown of ① host loading (milliseconds), to set the upper bound for ③ pre-relocation / mmap
    uint64_t t0 = now_ns(), tm = 0, tp = 0, tg = 0, tr = 0, t;
    if (n <= 0) die("no modules to load");
    if (!(g_mod = calloc((size_t)n, sizeof *g_mod))) die("cannot allocate %d module slots", n);   // once: module addresses stay stable
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        strlcpy(m->name, names[i], sizeof m->name);
        read_layout(m, dir);
    }
    create_keys();
    for (int i = 0; i < n; i++) {
        module_t *m = &g_mod[i];
        t = now_ns(); map_image(m, dir); tm += now_ns() - t;   // On return the code area is already read-only+execute; from here on the loader only writes data
        if (!i) tsd_check();
        t = now_ns(); parse_dynamic(m); tp += now_ns() - t;
        t = now_ns(); register_gates(m, dir); tg += now_ns() - t;
        LOG("%s: base %p size %#zx", names[i], m->base, m->size);
        g_nmod++;
    }
    force_init();
    t = now_ns(); for (int i = 0; i < g_nmod; i++) relocate(&g_mod[i]); tr = now_ns() - t;
    force_check();
    { void hlecount_seal(void); hlecount_seal(); }   // DM_HLECOUNT: relocation done, seal the counting stub pages as read-only executable
    if (getenv("DM_SYMCHECK")) LOG("DM_SYMCHECK: %lu symbol lookups at load time, hash and linear disagree %lu", g_symchk_n, g_symchk_bad);
    if (g_flag.bootprof) LOG("① breakdown: mapping (read + copy + seal pages) %.1f ms, dynamic section parsing %.1f ms, gate registration %.1f ms, relocation + forced takeover %.1f ms, total %.1f ms",
                             tm / 1e6, tp / 1e6, tg / 1e6, tr / 1e6, (now_ns() - t0) / 1e6);
}

// Plays the role of rtld's InitModules (rtld+0x830): call each module's DT_INIT. The order is reverse of the load list:
// modules later in the list are the ones the earlier ones depend on, so the sdk module's DT_INIT, which constructs the
// libc++ globals (std::cout and friends), runs before main's static initializers, which already write to them. Forward
// order crashed Little Busters! Converted Edition at its first log line (std::ostream::write on an unconstructed cout).
// The timing is decided by nn::init::Start: after nninitStartup and _init_libc2, before nnMain (the 4th argument callback of Start).
void run_module_inits(void) {
    for (int i = g_nmod - 1; i >= 0; i--) {
        module_t *m = &g_mod[i];
        if (!m->init) continue;
        LOG("%s: DT_INIT +%#llx (init_array %llu entries)", m->name, m->init, m->init_arraysz / 8);
        ((void (*)(void))(m->base + m->init))();
    }
}
