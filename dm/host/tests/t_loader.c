// The module loader on synthetic rewritten modules, written the way dm/tools/patch_nso.py writes them: the layout file, both patch-area layouts, the sealed pages,
// the key page, relocations and the order symbols bind in, gates, DT_INIT, the frame step page, snapshot and reset. No game files.
#include "../hle.h"
#include <mach-o/getsect.h>
#include <assert.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <unistd.h>

static int forced_target, hle_target;
static hle_ent entries[1] = {{"hle_only", &hle_target}};
static hle_force_ent forces[2] = {{"forced", &forced_target, "test"}, {"never_named", &forced_target, "test"}};
static uint8_t *test_section(const struct mach_header_64 *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; (void)seg; *size = 0;
    if (!strcmp(sect, "swhle")) { *size = sizeof entries; return (uint8_t *)entries; }
    if (!strcmp(sect, "swforce")) { *size = sizeof forces; return (uint8_t *)forces; }
    return NULL;
}
#define getsectiondata test_section
#include "../hle.c"
#include "../loader.c"
#undef getsectiondata

static int mods_patched;
void dm_mods_patch_module(module_t *m, const char *dir) { (void)m; (void)dir; mods_patched++; }
struct dm_flags g_flag;
uint64_t unimpl_stubs[MAX_STUB];
uint64_t now_ns(void) { return 0; }
uint64_t guest_ns(void) { return 0; }
uint64_t guest_ticks(void) { return 0; }
uint64_t guest_tick_hz(void) { return 31250000; }
void guest_clock_read_wait(void) {}
void hlecount_seal(void) {}
void thr_svc_enter(uint64_t *r, uint64_t a) { (void)r; (void)a; }
void thr_svc_leave(void) {}
void dm_spin_wait(uint64_t r, uint64_t a) { (void)r; (void)a; }
uint64_t svc_dispatch(int n, uint64_t *r, uint64_t a) { (void)n; (void)r; (void)a; return 0; }
void *hlecount_wrap(const char *kind, const char *name, void *fn) { (void)kind; (void)name; return fn; }

// One image: text, a page of tables and data, the key page, the frame step page, then the patch area (or, with the patch area before the base: bss and a back area)
enum { PG = 0x4000, INIT = 0x200, DATA = PG, DYN = DATA, SYM = DATA + 0x200, NSYM = 8, STR = SYM + NSYM * 24, RELA = DATA + 0x800, JMP = DATA + 0x900, RELR = DATA + 0xa00,
       SLOT = DATA + 0x1000, FN_SLOT = DATA + 0x1f00, KEYS = 2 * PG, FPS = 3 * PG, PATCH = 4 * PG, IMG = 5 * PG };
typedef struct { uint8_t b[IMG]; unsigned nsym, nstr, nrela, njmp, nrelr; } image;
static uint64_t *slot(image *im, unsigned off) { return (uint64_t *)(im->b + SLOT + off); }
static unsigned add_sym(image *im, const char *name, unsigned bind, unsigned other, int defined, uint64_t value) {
    Elf64_Sym *s = (Elf64_Sym *)(im->b + SYM) + im->nsym; assert(im->nsym < NSYM);
    *s = (Elf64_Sym){.st_name = im->nstr, .st_info = (uint8_t)(bind << 4), .st_other = (uint8_t)other, .st_shndx = defined ? 1 : 0, .st_value = value};
    strcpy((char *)im->b + STR + im->nstr, name); im->nstr += (unsigned)strlen(name) + 1;
    return im->nsym++;
}
static void add_rela(image *im, int jump, uint64_t off, unsigned type, unsigned sym, int64_t addend) {
    Elf64_Rela *r = (Elf64_Rela *)(im->b + (jump ? JMP : RELA)) + (jump ? im->njmp++ : im->nrela++);
    *r = (Elf64_Rela){off, (uint64_t)sym << 32 | type, addend};
}
// adr x16, FN_SLOT; ldr x16, [x16]; mov w0, #id; br x16: the module's DT_INIT tells the test it ran
static void add_init(image *im, int id) {
    uint32_t off = FN_SLOT - INIT, code[4] = {0x10000010u | (off & 3) << 29 | (off >> 2) << 5, 0xf9400210u, 0x52800000u | (uint32_t)id << 5, 0xd61f0200u};
    memcpy(im->b + INIT, code, sizeof code);
}
static void finish(image *im, int init) {
    uint32_t mod0 = 0x100; int32_t dyn = DYN - 0x100;
    memcpy(im->b + 4, &mod0, 4); memcpy(im->b + mod0, "MOD0", 4); memcpy(im->b + mod0 + 4, &dyn, 4);
    Elf64_Dyn d[] = {{DT_SYMTAB, {SYM}}, {DT_STRTAB, {STR}}, {DT_RELA, {RELA}}, {DT_RELASZ, {im->nrela * sizeof(Elf64_Rela)}}, {DT_JMPREL, {JMP}},
                     {DT_PLTRELSZ, {im->njmp * sizeof(Elf64_Rela)}}, {DT_RELR, {im->nrelr ? RELR : 0}}, {DT_RELRSZ, {im->nrelr * 8}}, {init ? DT_INIT : DT_FINI, {INIT}}, {0, {0}}};
    memcpy(im->b + DYN, d, sizeof d);
}
static char g_dir[1024];
static void put(const char *name, const char *suffix, const void *bytes, size_t n) {
    char path[1200]; snprintf(path, sizeof path, "%s/%s%s", g_dir, name, suffix);
    FILE *f = fopen(path, "wb"); assert(f && fwrite(bytes, 1, n, f) == n); fclose(f);
}
static const char k_keys[] = "# KEY_K18_IDX\t5\n# KEY_KTLS_IDX\t9\n# KEY_KTLR_IDX\t2\n# KEY_KGATE_IDX\t700\n";
static void put_layout(const char *name, const char *head) {
    char text[1024]; int n = snprintf(text, sizeof text, "%s%s", head, k_keys);
    put(name, ".fixups.tsv", text, (size_t)n);
}

static int g_order[4], g_norder;
static void on_init(int id) { g_order[g_norder++] = id; }

// What must stop the process runs in a child: a write to a sealed page, a layout the loader refuses
static int child_signal(void (*fn)(void *), void *arg) {
    pid_t pid = fork(); assert(pid >= 0);
    if (!pid) {
        struct rlimit lim = {0, 0}; setrlimit(RLIMIT_CORE, &lim);
        freopen("/dev/null", "w", stderr);
        fn(arg); _exit(0);
    }
    int status; assert(waitpid(pid, &status, 0) == pid);
    return WIFSIGNALED(status) ? WTERMSIG(status) : 0;
}
static void poke(void *p) { *(volatile uint8_t *)p = 1; }
static void peek(void *p) { (void)*(volatile uint8_t *)p; }
static int faults(void (*fn)(void *), void *p) { int s = child_signal(fn, p); return s == SIGBUS || s == SIGSEGV; }
static void layout_of(void *head) {
    put_layout("bad", head);
    module_t m = {.name = "bad"}; read_layout(&m, g_dir);
}
static int refused(const char *head) { return child_signal(layout_of, (void *)head) == SIGABRT; }
static void map_short(void *unused) {
    (void)unused;
    static uint8_t bytes[PG];
    put("short", ".patched.bin", bytes, sizeof bytes);
    module_t m = {.name = "short", .patch_off = -2 * PG, .image_size = 3 * PG, .file_image_len = 2 * PG, .key_page_off = 2 * PG, .key_page_size = PG};
    map_image(&m, g_dir);
}

int main(void) {
    strcpy(g_dir, "/tmp/t_loader.XXXXXX"); assert(mkdtemp(g_dir));
    void (*fn)(int) = on_init;

    // main: the patch area after the image, a frame step page, every relocation kind
    static image a;
    add_sym(&a, "", 0, 0, 0, 0);
    unsigned a_value = add_sym(&a, "app_value", 1, 0, 1, SLOT + 0x100), a_shared = add_sym(&a, "shared", 1, 0, 1, SLOT + 0x108), a_fn = add_sym(&a, "lib_fn", 1, 0, 0, 0),
             a_forced = add_sym(&a, "forced", 1, 0, 1, SLOT + 0x110), a_hle = add_sym(&a, "hle_only", 1, 0, 0, 0), a_weak = add_sym(&a, "nowhere_weak", STB_WEAK, 0, 0, 0),
             a_none = add_sym(&a, "nowhere", 1, 0, 0, 0);
    add_rela(&a, 0, SLOT + 0x00, R_AARCH64_RELATIVE, 0, 0x1234);
    add_rela(&a, 0, SLOT + 0x08, R_AARCH64_ABS64, a_value, 4);
    add_rela(&a, 0, SLOT + 0x10, R_AARCH64_GLOB_DAT, a_shared, 0);
    add_rela(&a, 0, SLOT + 0x18, R_AARCH64_ABS64, a_forced, 0);
    add_rela(&a, 0, SLOT + 0x20, R_AARCH64_GLOB_DAT, a_hle, 0);
    add_rela(&a, 0, SLOT + 0x28, R_AARCH64_GLOB_DAT, a_weak, 0); *slot(&a, 0x28) = 0xdead;
    add_rela(&a, 0, SLOT + 0x30, R_AARCH64_GLOB_DAT, a_none, 0);
    add_rela(&a, 1, SLOT + 0x38, R_AARCH64_JUMP_SLOT, a_fn, 0x99);   // a jump slot takes no addend
    // DT_RELR: the slot at +0x40, then a bitmap for the 63 slots after it: the first and the third
    uint64_t relr[2] = {SLOT + 0x40, (1ull << 0 | 1ull << 2) << 1 | 1}; memcpy(a.b + RELR, relr, sizeof relr); a.nrelr = 2;
    *slot(&a, 0x40) = 0x10; *slot(&a, 0x48) = 0x20; *slot(&a, 0x50) = 0x77; *slot(&a, 0x58) = 0x30;
    memcpy(a.b + FN_SLOT, &fn, 8); add_init(&a, 1); finish(&a, 1);
    memset(a.b + PATCH, 0xa5, 0x100);
    put("main", ".patched.bin", a.b, IMG);
    put_layout("main", "# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x3f00\n# RO_OFF\t0x4000\n# DATA_OFF\t0x5000\n# FPS_STATE_OFF\t0xc000\n"
                       "# BUILD_ID\t00112233445566778899aabbccddeeff\n");
    static const char gates[] = "1000 svc 0 2000\n1010 cntpct 3 2010\nnot a gate line\n";
    put("main", ".gates.tsv", gates, sizeof gates - 1);

    // lib: defines what main imports, and names main's symbols again as global, local and hidden ones
    static image b;
    add_sym(&b, "", 0, 0, 0, 0);
    add_sym(&b, "lib_fn", 1, 0, 1, INIT);
    unsigned b_shared = add_sym(&b, "shared", 1, 0, 1, SLOT + 0x200), b_local = add_sym(&b, "forced_not", STB_LOCAL, 0, 1, SLOT + 0x208),
             b_hidden = add_sym(&b, "app_value", 1, 2, 1, SLOT + 0x210);   // STV_HIDDEN
    add_rela(&b, 0, SLOT + 0x00, R_AARCH64_GLOB_DAT, b_shared, 0);
    add_rela(&b, 0, SLOT + 0x08, R_AARCH64_ABS64, b_local, 0);
    add_rela(&b, 0, SLOT + 0x10, R_AARCH64_ABS64, b_hidden, 0);
    memcpy(b.b + FN_SLOT, &fn, 8); add_init(&b, 2); finish(&b, 1);
    put("lib", ".patched.bin", b.b, 4 * PG + 0x123);   // not a whole page: the size is rounded up
    put_layout("lib", "# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x4000\n");

    // pre: the patch area before the base and a back patch area; the file holds the image up to FILE_IMAGE_LEN, the patch area, the back area. No DT_INIT.
    static image c; static uint8_t file[2 * PG + 0x1000 + PG];
    add_sym(&c, "", 0, 0, 0, 0);
    add_rela(&c, 0, SLOT, R_AARCH64_RELATIVE, 0, 0x40);
    finish(&c, 0);
    memcpy(file, c.b, 2 * PG); memset(file + 2 * PG, 0xc3, 0x1000); memset(file + 2 * PG + 0x1000, 0x5a, PG);
    put("pre", ".patched.bin", file, sizeof file);
    put_layout("pre", "# PATCH_OFF\t-0x8000\n# IMAGE_SIZE\t0x14000\n# FILE_IMAGE_LEN\t0x8000\n# BACK_OFF\t0x10000\n# BACK_LEN\t0x4000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n# TEXT_END\t0x4000\n");

    const char *names[] = {"main", "lib", "pre"};
    load_modules(g_dir, names, NULL, 3);
    module_t *ma = &g_mod[0], *mb = &g_mod[1], *mc = &g_mod[2];
    assert(g_nmod == 3 && mods_patched == 3);

    // The layout file
    assert(ma->patch_off == 0x10000 && ma->size == IMG && ma->text_end == 0x3f00 && ma->ro_off == 0x4000 && ma->data_off == 0x5000 && ma->fps_state_off == 0xc000);
    assert(!strcmp(ma->build_id, "00112233445566778899aabbccddeeff") && !mb->build_id[0]);
    assert(mb->size == IMG);
    assert(mc->patch_off == -0x8000 && mc->size == 0x14000 && mc->patch_len == PG && mc->back_off == 0x10000 && mc->back_len == PG);
    assert(refused("# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\nPATCH_OFF 0x10000\n"));   // a line of another format: an old rewriter's output
    assert(refused("# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n"));                                               // no patch area
    assert(refused("# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x8100\n# KEY_PAGE_SIZE\t0x4000\n"));                         // the key page is not on a host page
    assert(refused("# PATCH_OFF\t-0x8000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n"));                         // before the base, without the image's sizes
    assert(refused("# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n# BACK_OFF\t0x10000\n# BACK_LEN\t0x4000\n"));   // a back area needs the patch area before the base
    assert(!refused("# PATCH_OFF\t0x10000\n# KEY_PAGE_OFF\t0x8000\n# KEY_PAGE_SIZE\t0x4000\n"));
    assert(child_signal(map_short, NULL) == SIGABRT);   // a file shorter than FILE_IMAGE_LEN

    // The key page holds each host key's TSD offset at the index the rewriter chose, and is sealed; so are the code and the patch areas
    const uint64_t *keys = (const uint64_t *)(ma->base + KEYS);
    assert(keys[5] == g_key18 * 8 && keys[9] == g_keytls * 8 && keys[2] == g_keytlr * 8 && keys[700] == g_keygate * 8 && keys[0] == 0);
    assert(faults(poke, ma->base + KEYS) && faults(poke, ma->base + 8) && faults(poke, ma->base + PATCH) && !faults(poke, ma->base + SLOT));
    assert(ma->base[PATCH] == 0xa5 && mc->base[-0x8000] == 0xc3 && mc->base[-0x8000 + 0xfff] == 0xc3 && mc->base[-0x8000 + 0x1000] == 0);
    assert(faults(poke, mc->base - 0x8000) && faults(peek, mc->base - PG) && faults(poke, mc->base + 0x10000) && !faults(poke, mc->base + 3 * PG));   // patch, gap, back, bss
    assert(mc->base[0x10000] == 0x5a && mc->base[0x13fff] == 0x5a && mc->base[3 * PG] == 0 && ((const uint64_t *)(mc->base + KEYS))[5] == g_key18 * 8);
    assert(module_of(ma->base) == ma && module_of(ma->base + IMG - 1) == ma && module_of(ma->base + PATCH) == ma && module_of(mc->base - 0x8000) == mc
           && module_of(mc->base - PG - 1) == mc && !module_of(mc->base - PG) && !module_of(&forced_target));

    // Relocations
    const uint64_t *sa = (const uint64_t *)(ma->base + SLOT), *sb = (const uint64_t *)(mb->base + SLOT), A = (uint64_t)ma->base, B = (uint64_t)mb->base;
    assert(sa[0] == A + 0x1234);                                  // RELATIVE
    assert(sa[1] == A + SLOT + 0x100 + 4);                        // ABS64: the symbol and the addend
    assert(sa[2] == A + SLOT + 0x108);                            // main's own `shared`: it is the first module to define it
    assert(sa[3] == (uint64_t)&forced_target);                    // a forced takeover wins over the module's own definition
    assert(sa[4] == (uint64_t)&hle_target);                       // an import the host implements
    assert(sa[5] == 0);                                           // an undefined weak nothing defines binds to 0, as Horizon's rtld binds it
    assert(sa[6] == (uint64_t)&unimpl_stubs[0] && g_nstub == 1);  // an undefined strong one gets a reporting stub
    assert(sa[7] == B + INIT);                                    // JUMP_SLOT: lib's function, without the addend
    assert(sa[8] == A + 0x10 && sa[9] == A + 0x20 && sa[10] == 0x77 && sa[11] == A + 0x30);   // DT_RELR: the address entry and the bitmap's two slots
    assert(sb[0] == A + SLOT + 0x108);                            // lib's `shared` binds to main's: the earlier module's definition
    assert(sb[1] == B + SLOT + 0x208 && sb[2] == B + SLOT + 0x210);   // a local and a hidden symbol bind to their own module, whatever main defines
    assert(*(const uint64_t *)(mc->base + SLOT) == (uint64_t)mc->base + 0x40);
    assert(g_force_hits[0] == 1 && g_force_hits[1] == 0);         // force_check reports the entry no module names

    // Lookups
    assert(module_sym(ma, "app_value") == ma->base + SLOT + 0x100 && module_sym(mb, "app_value") == mb->base + SLOT + 0x210 && !module_sym(ma, "lib_fn") && !module_sym(mc, "shared"));
    assert(guest_sym("lib_fn") == mb->base + INIT && guest_sym("shared") == ma->base + SLOT + 0x108 && !guest_sym("nowhere"));
    assert(module_sym_linear(mb, "shared") == module_sym(mb, "shared"));
    assert(dm_game_imports("lib_") && dm_game_imports("nowhere") && !dm_game_imports("app_") && !dm_game_imports("absent"));

    // Gates: keyed by their absolute return address
    assert(g_ngate == 2);
    const gate_t *g0 = &g_gate[gate_h(A + 0x1000)], *g1 = &g_gate[gate_h(A + 0x1010)];
    assert(g0->ret == A + 0x1000 && g0->kind == G_SVC && g0->orig == 0x2000 && g1->ret == A + 0x1010 && g1->kind == G_CNTPCT && g1->arg == 3);

    loader_snapshot();

    // DT_INIT in reverse load order (rtld's InitModules); a module without one is passed over
    run_module_inits();
    assert(g_norder == 2 && g_order[0] == 2 && g_order[1] == 1);

    // The frame step page: the profile's target unless the environment names one; values out of range fall back
    assert(dm_fps_state == ma->base + FPS);
    float f; uint32_t dyn;
    unsetenv("DM_FPS_TARGET"); unsetenv("DM_RENDER_SCALE"); unsetenv("DM_DYNAMIC_RESOLUTION"); dm_fps_state_apply();
    memcpy(&f, dm_fps_state, 4); assert(f == (float)DM_GAME_FPS_TARGET);
    memcpy(&f, dm_fps_state + 0x10, 4); memcpy(&dyn, dm_fps_state + 0x40, 4); assert(f == 1.0f && dyn == 0);
    setenv("DM_FPS_TARGET", "30", 1); setenv("DM_RENDER_SCALE", "0.75", 1); setenv("DM_DYNAMIC_RESOLUTION", "1", 1); dm_fps_state_apply();
    memcpy(&f, dm_fps_state, 4); assert(f == 30.0f);
    memcpy(&f, dm_fps_state + 0x10, 4); memcpy(&dyn, dm_fps_state + 0x40, 4); assert(f == 0.75f && dyn == 1);
    setenv("DM_FPS_TARGET", "45", 1); setenv("DM_RENDER_SCALE", "0.2", 1); setenv("DM_DYNAMIC_RESOLUTION", "yes", 1); dm_fps_state_apply();
    memcpy(&f, dm_fps_state, 4); assert(f == 0.0f);
    memcpy(&f, dm_fps_state + 0x10, 4); memcpy(&dyn, dm_fps_state + 0x40, 4); assert(f == 1.0f && dyn == 0);

    // Reset: data and bss as at the first start (the relocated values, zero pages zero again); code, patch areas and key pages stay
    setenv("DM_FPS_TARGET", "30", 1); dm_fps_state_apply();
    *(uint64_t *)(ma->base + SLOT) = 1; *(uint64_t *)(ma->base + SLOT + 0x100) = 2; mc->base[3 * PG + 5] = 3; *(uint64_t *)(mc->base + SLOT) = 4;
    loader_reset();
    assert(sa[0] == A + 0x1234 && sa[0x100 / 8] == 0 && mc->base[3 * PG + 5] == 0 && *(const uint64_t *)(mc->base + SLOT) == (uint64_t)mc->base + 0x40);
    memcpy(&f, dm_fps_state, 4); assert(f == 0.0f);   // written after the snapshot: the host applies it again after a reset
    assert(keys[5] == g_key18 * 8 && faults(poke, ma->base + KEYS) && ma->base[PATCH] == 0xa5 && mc->base[0x10000] == 0x5a && !memcmp(ma->base + 0x100, "MOD0", 4));

    static const char *const left[] = {"main.patched.bin", "main.fixups.tsv", "main.gates.tsv", "lib.patched.bin", "lib.fixups.tsv", "pre.patched.bin", "pre.fixups.tsv",
                                       "bad.fixups.tsv", "short.patched.bin"};
    for (unsigned i = 0; i < sizeof left / sizeof *left; i++) { char path[1200]; snprintf(path, sizeof path, "%s/%s", g_dir, left[i]); unlink(path); }
    rmdir(g_dir);
    puts("Loader: layout file and refusals, both patch-area layouts sealed, key page, relocations and binding order, lookups, gates, DT_INIT order, frame step page, snapshot and reset: OK");
    return 0;
}
