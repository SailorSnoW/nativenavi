// The NVN resource owner: nvn_owner.h says what it holds, what a lookup returns and the lock order.
#import <Metal/Metal.h>
#include "nvn_owner.h"
#include "nvn_tex.h"
#include "../../host/dm.h"
#include <mach/mach.h>
#include <os/lock.h>

#define ODIE(...) do { dm_log("[dm] [nvn-mtl] FATAL " __VA_ARGS__); abort(); } while (0)

pthread_mutex_t g_smu = PTHREAD_MUTEX_INITIALIZER;

// Generation of what serials resolve to, +1 per change; caches record it and reuse what they fetched while unchanged. own_note names the serial (ser_sync, pool_gpu); a change
// without a note (own_epoch) or one the ring lost resyncs everything. Under g_snote_lk (taken under g_smu or alone, never the other way)
_Atomic uint64_t g_ser_gen = 1;
static os_unfair_lock g_snote_lk = OS_UNFAIR_LOCK_INIT;
static uint64_t g_snote[SER_NOTES];
static unsigned g_nsnote;
typedef struct { uint64_t gen, serial; } ser_lnote;
static ser_lnote g_ser_log[SER_LOG];
void own_note(uint64_t serial) {
    os_unfair_lock_lock(&g_snote_lk);
    if (g_nsnote < SER_NOTES) g_snote[g_nsnote++] = serial;
    uint64_t g = atomic_fetch_add(&g_ser_gen, 1) + 1;
    g_ser_log[g % SER_LOG] = (ser_lnote){g, serial};
    os_unfair_lock_unlock(&g_snote_lk);
}
void own_epoch(void) { atomic_fetch_add(&g_ser_gen, 1); }
unsigned own_notes_take(uint64_t out[SER_NOTES], uint64_t *gen) {
    os_unfair_lock_lock(&g_snote_lk);
    *gen = g_ser_gen;
    unsigned n = g_nsnote;
    memcpy(out, g_snote, n * sizeof *out);
    g_nsnote = 0;
    os_unfair_lock_unlock(&g_snote_lk);
    return n;
}
int own_note_at(uint64_t gen, uint64_t *serial) {
    os_unfair_lock_lock(&g_snote_lk);
    const ser_lnote *sn = &g_ser_log[gen % SER_LOG];
    int known = sn->gen == gen;
    if (known) *serial = sn->serial;
    os_unfair_lock_unlock(&g_snote_lk);
    return known;
}
static nvn_hmap g_by_serial = {.klen = 8};     // serial → MTLTexture / NvPkg / NvProg
// Entries of g_by_serial holding each object (a view's parents too), so alias_take_over asks in one lookup (docs/reference/rendering.md#a-storage-takeover-walked-the-serial-table-for-each-entry).
// An entry retains its object, so a counted address is not reused. Under g_smu
static nvn_hmap g_by_serial_held = {.klen = sizeof(uintptr_t)};
// the same for g_ghost
static nvn_hmap g_ghost_held = {.klen = sizeof(uintptr_t)};
// Serials of live NVN texture objects. A finalized serial resolves through g_ghost (records and registrations snapshot the object), kept by use: past NGHOST the least recently
// used goes once idle for GHOST_IDLE frames or GHOST_IDLE_SUBS submissions (docs/reference/nvn-objects.md#more-finalized-textures-in-use-than-their-table-held)
static nvn_hmap g_tex_live = {.klen = 8};   // serial → the texture object it was initialized in; under g_smu
// Live texture serials per pool, recorded at initialization (the game may free the object later), so a pool's Finalize ends exactly its textures. Under g_smu
static nvn_hmap g_tex_pool = {.klen = 8};
static nvn_hmap g_pool_tex = {.klen = sizeof(uintptr_t)};   // pool → CFMutableSetRef of serials (a serial is never 0)
static void pool_tex_del(uint64_t serial) {
    void *p = nvn_hm_del(&g_tex_pool, &serial);
    if (!p) return;
    uintptr_t pk = (uintptr_t)p;
    CFMutableSetRef set = nvn_hm_get(&g_pool_tex, &pk);
    if (!set) return;
    CFSetRemoveValue(set, (const void *)(uintptr_t)serial);
    if (!CFSetGetCount(set)) {
        nvn_hm_del(&g_pool_tex, &pk);
        CFRelease(set);
    }
}
static nvn_hmap g_ghost = {.klen = 8};
static uint64_t g_ghost_puts;
typedef struct ghost_use { struct ghost_use *prev, *next; uint64_t serial, frame, sub; } ghost_use;
static nvn_hmap g_ghost_use = {.klen = 8};    // the same serials as g_ghost
static ghost_use *g_ghost_lru, *g_ghost_mru;  // least recently used first
// Queued descriptors and saved bindings pin a serial, independently of the guest object and the ghost table
typedef struct { unsigned refs; void *object; } tex_pin;
static nvn_hmap g_tex_pins = {.klen = 8}, g_tex_pin_objects = {.klen = sizeof(uintptr_t)};

int serial_table_holds(const void *o) { uintptr_t k = (uintptr_t)o; return nvn_hm_get(&g_by_serial_held, &k) != NULL; }
int entry_holds(const void *o) { uintptr_t k = (uintptr_t)o; return serial_table_holds(o) || (g_ghost_held.n && nvn_hm_get(&g_ghost_held, &k)); }
int tex_object_pinned(id o) { uintptr_t key = (uintptr_t)(__bridge void *)o; return o && nvn_hm_get(&g_tex_pin_objects, &key) != NULL; }
int own_object_free(id o) {
    pthread_mutex_lock(&g_smu);
    int r = !entry_holds((__bridge void *)o);
    pthread_mutex_unlock(&g_smu);
    return r;
}
static CFMutableSetRef g_alias_set;   // non-owning: "is this shared"
// by address alone: gone_note may ask after the last reference went
int own_holds_locked(const void *o) { uintptr_t k = (uintptr_t)o; return o && (entry_holds(o) || nvn_hm_get(&g_tex_pin_objects, &k) || (g_alias_set && CFSetContainsValue(g_alias_set, o))); }
// Objects that left their last holder, for the view cache; past GONE_MAX the list says so and the taker checks everything. Under g_smu
enum { GONE_MAX = 65536 };
static const void **g_gone;
static unsigned g_gone_n, g_gone_cap;
static int g_gone_over;
static void gone_note(const void *o) {
    if (!o || g_gone_over || own_holds_locked(o)) return;
    if (g_gone_n == GONE_MAX) {
        g_gone_over = 1;
        return;
    }
    if (g_gone_n == g_gone_cap) {
        unsigned cap = g_gone_cap ? 2 * g_gone_cap : 256;
        const void **p = realloc(g_gone, cap * sizeof *p);
        if (!p) ODIE("the owner's released-object list");
        g_gone = p;
        g_gone_cap = cap;
    }
    g_gone[g_gone_n++] = o;
}
int own_gone_take_locked(const void *const **list, unsigned *n) {
    int over = g_gone_over;
    *list = g_gone;
    *n = g_gone_n;
    g_gone_n = 0;
    g_gone_over = 0;
    return over;
}

// Retirement: after GPU completion an object an entry (live, ghost, pin, or a view's parent) still holds waits here, retained; its last count going hands it back to the
// deferred-release queue behind the submission in flight (retire_resume). One packaged texture can be held by many serials
// (docs/reference/nvn-objects.md#a-texture-shared-by-its-packaged-textures-serials-left-the-residency-set-with-one-of-them). Under g_smu
static nvn_hmap g_retire_wait = {.klen = sizeof(uintptr_t)};
static void retire_resume(const void *o) {
    uintptr_t k = (uintptr_t)o;
    if (!g_retire_wait.n || entry_holds(o)) return;
    void *v = nvn_hm_del(&g_retire_wait, &k);
    if (!v) return;
    id obj = CFBridgingRelease(v);
    if (!own_keeps_locked(obj)) own_hook_drop(obj, 0);
}
int own_retire_locked(id o) {
    if (!entry_holds((__bridge void *)o)) return 1;
    uintptr_t k = (uintptr_t)(__bridge void *)o;
    if (!nvn_hm_get(&g_retire_wait, &k)) nvn_hm_put(&g_retire_wait, &k, (void *)CFBridgingRetain(o));
    return 0;
}
static void count_one(nvn_hmap *m, const void *o, int d) {
    if (!o) return;
    uintptr_t k = (uintptr_t)o, n = (uintptr_t)nvn_hm_get(m, &k);
    if (d > 0) nvn_hm_put(m, &k, (void *)(n + 1));
    else if (n > 1) nvn_hm_put(m, &k, (void *)(n - 1));
    else if (n) {
        nvn_hm_del(m, &k);
        retire_resume(o);
        gone_note(o);
    }   // a slot a test wrote past these helpers was never counted
}
// An entry holding a view holds its parents too (docs/reference/nvn-objects.md#a-views-entry-did-not-hold-its-parent); a parent never changes
static void count_add(nvn_hmap *m, const void *o, int d) {
    count_one(m, o, d);
    id t = (__bridge id)o;
    if (t && [t respondsToSelector:@selector(parentTexture)]) for (id<MTLTexture> p = ((id<MTLTexture>)t).parentTexture; p; p = p.parentTexture) count_one(m, (__bridge void *)p, d);
}
static void held_add(const void *o, int d) { count_add(&g_by_serial_held, o, d); }
static void ghost_held_add(const void *o, int d) { count_add(&g_ghost_held, o, d); }

// The sharing table: textures by pool + offset + description, as an object rebuilt over the same storage is the same texture
// (docs/reference/rendering.md#transient-render-targets-shared-by-storage). It keeps them resident; the serial table only drops its reference. Under g_smu
static nvn_hmap g_alias = {.klen = sizeof(alias_key)};
// {pool, 64 KB cell} → the shared entries reaching into it, with their ends (docs/reference/rendering.md#a-storage-takeover-walked-the-sharing-table-for-each-texture-made).
// Kept by alias_put and alias_del, the only writers of g_alias's keys
typedef struct { alias_key k; int64_t end; } alias_ent;
typedef struct { uint32_t n, cap; alias_ent e[]; } alias_cell;
static nvn_hmap g_alias_cells = {.klen = 16};
static void alias_index(const alias_key *k, int add) {
    nvn_texlayout L;
    nvn_tex_layout(&k->d, &L);
    int64_t end = k->off + (int64_t)L.size, c1 = (end > k->off ? end - 1 : k->off) >> ALIAS_CELL;
    for (int64_t c = k->off >> ALIAS_CELL; c <= c1; c++) {
        uint64_t ck[2] = {k->pool, (uint64_t)c};
        alias_cell *v = nvn_hm_get(&g_alias_cells, ck);
        uint32_t i = 0;
        if (v) for (; i < v->n; i++) if (!memcmp(&v->e[i].k, k, sizeof *k)) break;
        if (add) {
            if (v && i < v->n) continue;
            if (!v || v->n == v->cap) {
                uint32_t cap = v ? v->cap * 2 : 4;
                alias_cell *nv = realloc(v, sizeof *nv + cap * sizeof nv->e[0]);
                if (!nv) abort();
                if (!v) nv->n = 0;
                nv->cap = cap;
                v = nv;
                nvn_hm_put(&g_alias_cells, ck, v);
            }
            v->e[v->n].k = *k;
            v->e[v->n++].end = end;
        } else if (v && i < v->n) {
            v->e[i] = v->e[--v->n];
            if (!v->n) {
                nvn_hm_del(&g_alias_cells, ck);
                free(v);
            }
        }
    }
}
static nvn_hmap g_alias_seen = {.klen = sizeof(alias_key)};   // key → submission of its last lookup or creation (takeovers spare recent ones)
static uint64_t g_alias_evicted, g_alias_taken, g_alias_pool_entries, g_alias_looks;
int own_shared_locked(id o) { return g_alias_set && o && CFSetContainsValue(g_alias_set, (__bridge const void *)o); }
int own_keeps_locked(id o) { return tex_object_pinned(o) || own_shared_locked(o); }
static void obj_drop(id o) { own_hook_drop(o, own_keeps_locked(o)); }   // caller holds g_smu
static void alias_seen(const alias_key *k, uint64_t now) { nvn_hm_put(&g_alias_seen, k, (void *)(uintptr_t)now); }
// The only writers: an entry goes in and out with its cells, g_alias_set and g_alias_seen. Caller holds g_smu
static int alias_del(const alias_key *k) {
    void *old = nvn_hm_del(&g_alias, k);
    if (!old) return 0;
    alias_index(k, 0);
    nvn_hm_del(&g_alias_seen, k);
    id o = CFBridgingRelease(old);
    if (g_alias_set) CFSetRemoveValue(g_alias_set, (__bridge const void *)o);
    gone_note((__bridge const void *)o);
    own_hook_drop(o, 0);
    return 1;
}
static void alias_put(const alias_key *k, id o, uint64_t seen) {   // an old entry is retired first: replaced in the map alone, it would stay in g_alias_set and be released at once
    if (nvn_hm_get(&g_alias, k)) alias_del(k);
    if (!g_alias_set) g_alias_set = CFSetCreateMutable(NULL, 0, NULL);
    hm_put_id(&g_alias, k, o);
    alias_index(k, 1);
    CFSetAddValue(g_alias_set, (__bridge const void *)o);
    if (seen) alias_seen(k, seen);
}
id own_alias_get_locked(const alias_key *k) { return hm_get_id(&g_alias, k); }
id own_alias_peek(const alias_key *k) {
    pthread_mutex_lock(&g_smu);
    id o = hm_get_id(&g_alias, k);
    pthread_mutex_unlock(&g_smu);
    return o;
}
void own_alias_put_locked(const alias_key *k, id o, uint64_t seen) { alias_put(k, o, seen); }
int own_alias_del_locked(const alias_key *k) { return alias_del(k); }
// A new shared texture takes over storage: overlapping entries of its pool go unless a live object resolves to them or a lookup found them within ALIAS_IDLE submissions
// (docs/reference/rendering.md#shared-storage-entries-taken-over-by-a-new-texture). No serial changes, so no note (own_note(0)). Caller holds g_smu
static uint32_t alias_take_over(const alias_key *nk, uint64_t now) {
    nvn_texlayout L;
    nvn_tex_layout(&nk->d, &L);
    if (!L.size) return 0;
    int64_t lo = nk->off, hi = nk->off + (int64_t)L.size;
    uint32_t n = 0;
    alias_key ks[16];
    for (int64_t c = lo >> ALIAS_CELL; c <= (hi - 1) >> ALIAS_CELL && n < 16; c++) {   // entries reaching into this one's cells
        uint64_t ck[2] = {nk->pool, (uint64_t)c};
        const alias_cell *v = nvn_hm_get(&g_alias_cells, ck);
        for (uint32_t i = 0; v && i < v->n && n < 16; i++) {
            const alias_key *k = &v->e[i].k;
            if (k->off >= hi || v->e[i].end <= lo) continue;
            // met in each cell of the overlap: looked at in the first only (docs/reference/frame-rate.md#a-storage-takeover-looked-at-a-spared-entry-once-per-64-kb-cell)
            if (c != (k->off > lo ? k->off : lo) >> ALIAS_CELL) continue;
            if (!memcmp(k, nk, sizeof *k)) continue;
            g_alias_looks++;
            void *val = nvn_hm_get(&g_alias, k);
            if (!val) continue;
            uint64_t seen = (uint64_t)(uintptr_t)nvn_hm_get(&g_alias_seen, k);
            if ((seen && now - seen < ALIAS_IDLE) || serial_table_holds(val)) continue;
            ks[n++] = *k;
        }
    }
    for (uint32_t j = 0; j < n; j++) alias_del(&ks[j]);
    if (n) {
        own_note(0);
        g_alias_taken += n;
    }
    return n;
}
uint32_t own_alias_take_over_locked(const alias_key *k) { return alias_take_over(k, own_hook_submission()); }
void own_alias_at_locked(const void *pool, int64_t off, void (^fn)(const alias_key *k, id o)) {
    uint64_t ck[2] = {(uint64_t)(uintptr_t)pool, (uint64_t)(off >> ALIAS_CELL)};
    const alias_cell *cell = nvn_hm_get(&g_alias_cells, ck);
    for (uint32_t i = 0; cell && i < cell->n; i++) {
        const alias_key *k = &cell->e[i].k;
        if (k->off != off) continue;
        id o = hm_get_id(&g_alias, k);
        if (o) fn(k, o);
    }
}
void own_alias_each_locked(void (^fn)(const alias_key *k, id o)) {
    for (uint32_t i = 0; i < g_alias.cap; i++) if (g_alias.vals[i]) fn((const alias_key *)(const void *)(g_alias.keys + (size_t)i * g_alias.klen), (__bridge id)g_alias.vals[i]);
}
own_alias_stats own_alias_stats_locked(void) { return (own_alias_stats){.n = g_alias.n, .cap = g_alias.cap, .cells = g_alias_cells.n, .evicted = g_alias_evicted, .taken = g_alias_taken, .pool_entries = g_alias_pool_entries, .looks = g_alias_looks}; }

const void *own_live_ref_locked(uint64_t k) { return nvn_hm_get(&g_by_serial, &k); }
const void *own_live_ref(uint64_t k) {
    pthread_mutex_lock(&g_smu);
    const void *o = nvn_hm_get(&g_by_serial, &k);
    pthread_mutex_unlock(&g_smu);
    return o;
}
void serial_put(uint64_t k, id o) {
    pthread_mutex_lock(&g_smu);
    id old = hm_get_id(&g_by_serial, &k);
    if (old) {
        held_add((__bridge void *)old, -1);
        obj_drop(old);
    }
    hm_put_id(&g_by_serial, &k, o);
    held_add((__bridge void *)o, 1);
    own_note(k);
    pthread_mutex_unlock(&g_smu);
}
static void live_del_locked(uint64_t k) {
    id old = hm_get_id(&g_by_serial, &k);
    if (!old) return;
    held_add((__bridge void *)old, -1);
    obj_drop(old);
    hm_del_id(&g_by_serial, &k);
    own_note(k);
}
void serial_del(uint64_t k) {
    pthread_mutex_lock(&g_smu);
    live_del_locked(k);
    pthread_mutex_unlock(&g_smu);
}
void own_tex_live_put_locked(const tex_t *t) {
    nvn_hm_put(&g_tex_live, &t->serial, (void *)t);
    pool_tex_del(t->serial);
    if (!t->pool) return;
    uintptr_t pk = (uintptr_t)t->pool;
    CFMutableSetRef set = nvn_hm_get(&g_pool_tex, &pk);
    if (!set) {
        set = CFSetCreateMutable(NULL, 0, NULL);
        if (!set) ODIE("the pool's texture index");
        nvn_hm_put(&g_pool_tex, &pk, (void *)set);
    }
    // NOLINTBEGIN(clang-analyzer-osx.cocoa.RetainCount): g_pool_tex owns set until pool_tex_del or own_pool_finalize_locked
    CFSetAddValue(set, (const void *)(uintptr_t)t->serial);
    nvn_hm_put(&g_tex_pool, &t->serial, (void *)t->pool);
    // NOLINTEND(clang-analyzer-osx.cocoa.RetainCount)
}
int own_tex_live_locked(uint64_t serial) { return nvn_hm_get(&g_tex_live, &serial) != NULL; }
int own_tex_live(uint64_t serial) {
    pthread_mutex_lock(&g_smu);
    int r = own_tex_live_locked(serial);
    pthread_mutex_unlock(&g_smu);
    return r;
}
int own_tex_end_locked(tex_t *t) {
    // The game finalizes copies of texture objects that live on: a copy's Finalize ends only the copy (docs/reference/nvn-objects.md#finalizing-a-copy-of-a-texture-object)
    const tex_t *own = nvn_hm_get(&g_tex_live, &t->serial);
    if (own && own != t) {   // read through the kernel: the game may have unmapped that object
        tex_t seen;
        vm_size_t got = 0;
        if (vm_read_overwrite(mach_task_self(), (vm_address_t)(uintptr_t)own, sizeof seen, (vm_address_t)(uintptr_t)&seen, &got) == KERN_SUCCESS && got == sizeof seen && seen.magic == MAGIC('T', 'E', 'X', 0) && seen.serial == t->serial) return -1;
    }
    pool_tex_del(t->serial);
    return nvn_hm_del(&g_tex_live, &t->serial) != NULL;
}

static void tex_pin_set(tex_pin *pin, id object) {   // g_smu held
    id old = (__bridge id)pin->object;
    if (old == object) return;
    if (old) {
        uintptr_t key = (uintptr_t)pin->object, count = (uintptr_t)nvn_hm_get(&g_tex_pin_objects, &key);
        if (count > 1) nvn_hm_put(&g_tex_pin_objects, &key, (void *)(count - 1));
        else nvn_hm_del(&g_tex_pin_objects, &key);
        held_add(pin->object, -1);
        own_hook_drop(old, own_keeps_locked(old) || serial_table_holds((__bridge void *)old));
        CFRelease(pin->object);
        pin->object = NULL;
    }
    if (object) {
        pin->object = (void *)CFBridgingRetain(object);
        uintptr_t key = (uintptr_t)pin->object;
        uintptr_t count = (uintptr_t)nvn_hm_get(&g_tex_pin_objects, &key);
        nvn_hm_put(&g_tex_pin_objects, &key, (void *)(count + 1));
        held_add(pin->object, 1);
    }
}
void mtl_tex_pin(uint64_t serial) {
    if (!serial) return;
    pthread_mutex_lock(&g_smu);
    tex_pin *pin = nvn_hm_get(&g_tex_pins, &serial);
    if (!pin) {
        pin = calloc(1, sizeof *pin);
        if (!pin) ODIE("texture retention allocation failed");
        nvn_hm_put(&g_tex_pins, &serial, pin);
    }
    pin->refs++;
    if (!pin->object) {
        id o = hm_get_id(&g_by_serial, &serial);
        if (!o) o = hm_get_id(&g_ghost, &serial);
        tex_pin_set(pin, o);
    }
    pthread_mutex_unlock(&g_smu);
}
void mtl_tex_unpin(uint64_t serial) {
    if (!serial) return;
    pthread_mutex_lock(&g_smu);
    tex_pin *pin = nvn_hm_get(&g_tex_pins, &serial);
    if (pin && !--pin->refs) {
        nvn_hm_del(&g_tex_pins, &serial);
        tex_pin_set(pin, nil);
        free(pin);
    }
    pthread_mutex_unlock(&g_smu);
}
// caller holds g_smu
static void use_unlink(ghost_use *u) {
    if (u->prev) u->prev->next = u->next; else g_ghost_lru = u->next;
    if (u->next) u->next->prev = u->prev; else g_ghost_mru = u->prev;
    u->prev = u->next = NULL;
}
static void ghost_used(uint64_t k) {
    ghost_use *u = nvn_hm_get(&g_ghost_use, &k);
    if (!u) {
        u = calloc(1, sizeof *u);
        if (!u) ODIE("ghost table allocation failed");
        u->serial = k;
        nvn_hm_put(&g_ghost_use, &k, u);
    } else if (u != g_ghost_mru) {
        use_unlink(u);
    }
    u->frame = own_hook_frame();
    u->sub = own_hook_submission();
    if (u != g_ghost_mru) {
        u->prev = g_ghost_mru;
        if (g_ghost_mru) g_ghost_mru->next = u;
        else g_ghost_lru = u;
        g_ghost_mru = u;
    }
}
static void ghost_del_locked(uint64_t k) {   // the serial resolves anew (a note)
    ghost_use *u = nvn_hm_del(&g_ghost_use, &k);
    if (u) {
        use_unlink(u);
        free(u);
    }
    id old = hm_get_id(&g_ghost, &k);
    if (!old) return;
    ghost_held_add((__bridge void *)old, -1);
    obj_drop(old);
    hm_del_id(&g_ghost, &k);
    own_note(k);
}
// at most `budget`; the first entry in use ends the walk
static void ghost_trim(unsigned budget) {
    uint64_t frame = own_hook_frame(), sub = own_hook_submission();
    for (; budget && g_ghost.n > NGHOST && g_ghost_lru; budget--) {
        const ghost_use *u = g_ghost_lru;
        if (frame - u->frame < GHOST_IDLE && sub - u->sub < GHOST_IDLE_SUBS) break;
        ghost_del_locked(u->serial);
    }
}
void own_ghost_trim(void) {
    pthread_mutex_lock(&g_smu);
    ghost_trim(GHOST_TRIM);
    pthread_mutex_unlock(&g_smu);
}
const void *own_resolve_ref(uint64_t k) {
    pthread_mutex_lock(&g_smu);
    const void *o = nvn_hm_get(&g_by_serial, &k);
    if (!o && g_ghost.n && (o = nvn_hm_get(&g_ghost, &k))) ghost_used(k);
    if (!o) {
        tex_pin *pin = nvn_hm_get(&g_tex_pins, &k);
        if (pin) o = pin->object;
    }
    pthread_mutex_unlock(&g_smu);
    return o;
}
static void tex_put_locked(uint64_t k, id o) {   // serials from 2^62 up are external textures (mtl_ext_tex)
    nvn_hmap *m = k >= (1ull << 62) || nvn_hm_get(&g_tex_live, &k) ? &g_by_serial : &g_ghost;
    id old = hm_get_id(m, &k);
    if (old) count_add(m == &g_by_serial ? &g_by_serial_held : &g_ghost_held, (__bridge void *)old, -1);
    if (old) obj_drop(old);
    else if (m == &g_ghost) g_ghost_puts++;
    hm_put_id(m, &k, o);
    tex_pin *pin = nvn_hm_get(&g_tex_pins, &k);
    if (pin) tex_pin_set(pin, o);
    own_note(k);
    count_add(m == &g_by_serial ? &g_by_serial_held : &g_ghost_held, (__bridge void *)o, 1);
    if (m == &g_ghost) {
        ghost_used(k);
        ghost_trim(2);   // two per put: idle entries drain faster than new ones come
    }
}
void tex_put(uint64_t k, id o) {
    pthread_mutex_lock(&g_smu);
    tex_put_locked(k, o);
    pthread_mutex_unlock(&g_smu);
}

// lookup and publication in one hold: a pool Finalize between them would give the serial an evicted texture
id own_alias_take(uint64_t serial, const alias_key *k) {
    pthread_mutex_lock(&g_smu);
    id o = hm_get_id(&g_alias, k);
    if (o) {
        alias_seen(k, own_hook_submission());
        tex_put_locked(serial, o);
    }
    pthread_mutex_unlock(&g_smu);
    return o;
}
// one hold; a texture another thread registered under the key since the lookup wins, and the caller retires its own
id own_alias_publish(uint64_t serial, const alias_key *k, id o) {
    pthread_mutex_lock(&g_smu);
    uint64_t now = own_hook_submission();
    id cur = hm_get_id(&g_alias, k);
    if (cur) {
        alias_seen(k, now);
        o = cur;
    } else {
        alias_take_over(k, now);
        alias_put(k, o, now);
    }
    tex_put_locked(serial, o);
    pthread_mutex_unlock(&g_smu);
    return o;
}

// sRGB / linear views share bytes only while their parent is in the sharing table; checked in the same g_smu hold as the view's entry changes
static id view_parent(id o) { return [o respondsToSelector:@selector(parentTexture)] ? ((id<MTLTexture>)o).parentTexture : nil; }
int own_view_put(uint64_t k, const alias_key *pk, id<MTLTexture> view) {
    pthread_mutex_lock(&g_smu);
    id p = view_parent(view);
    int ok = p && hm_get_id(&g_alias, pk) == p;
    if (ok) {
        alias_seen(pk, own_hook_submission());
        tex_put_locked(k, view);
    }
    pthread_mutex_unlock(&g_smu);
    return ok;
}
int own_view_drop_stale(uint64_t k, id view) {
    pthread_mutex_lock(&g_smu);
    id p = view_parent(view);
    int stale = p && !own_keeps_locked(view) && !own_keeps_locked(p);
    if (stale) {
        id live = hm_get_id(&g_by_serial, &k);
        if (live == view) live_del_locked(k);
        else if (!live && hm_get_id(&g_ghost, &k) == view) ghost_del_locked(k);   // changed by another thread meanwhile: not this view's
    }
    pthread_mutex_unlock(&g_smu);
    return stale;
}

int own_rename_locked(id from, id to, uint64_t serial) {
    nvn_hmap *tables[] = {&g_by_serial, &g_ghost};
    int named = 0;
    for (unsigned j = 0; j < 2; j++) for (uint32_t i = 0; i < tables[j]->cap; i++) {
        if (tables[j]->vals[i] != (__bridge void *)from) continue;
        uint64_t k;
        memcpy(&k, tables[j]->keys + (size_t)i * tables[j]->klen, sizeof k);
        void *old = tables[j]->vals[i];
        tables[j]->vals[i] = (void *)CFBridgingRetain(to);
        nvn_hmap *cnt = j == 0 ? &g_by_serial_held : &g_ghost_held;
        count_add(cnt, old, -1);
        count_add(cnt, (__bridge void *)to, 1);
        CFRelease(old);
        named |= k == serial;
    }
    for (uint32_t i = 0; i < g_tex_pins.cap; i++) {
        tex_pin *pin = g_tex_pins.vals[i];
        if (pin && pin->object == (__bridge void *)from) tex_pin_set(pin, to);
    }
    int shared = 0;   // sharing table values replaced in place
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (g_alias.vals[i] != (__bridge void *)from) continue;
        void *old = g_alias.vals[i];
        g_alias.vals[i] = (void *)CFBridgingRetain(to);
        CFRelease(old);
        shared = 1;
    }
    if (shared) {
        CFSetRemoveValue(g_alias_set, (__bridge const void *)from);
        CFSetAddValue(g_alias_set, (__bridge const void *)to);
    }
    gone_note((__bridge const void *)from);
    return named;
}
// in one hold: published later, a reader would find the old view of a parent no longer shared
int own_upgrade(id from, id to, uint64_t serial, id put) {
    pthread_mutex_lock(&g_smu);
    int named = own_rename_locked(from, to, serial);
    own_epoch();   // every serial that named `from` changed: no single note
    if (put && !(named && put == to)) tex_put_locked(serial, put);
    pthread_mutex_unlock(&g_smu);
    return named;
}

// Shared entries another covered entry of their pool drew over after their last use, unused for `idle`, go with their serial entries: the content is dead on the console too
// (docs/reference/rendering.md#render-targets-drawn-over-stayed-while-their-texture-objects-lived). A pin keeps one. *serials: malloc'ed, the caller frees it. Caller holds g_smu
typedef struct { nvn_hmap *m; uintptr_t n; } held_sum_ctx;
static void held_sum_one(const void *v, void *c) {
    held_sum_ctx *x = c;
    uintptr_t k = (uintptr_t)v;
    x->n += (uintptr_t)nvn_hm_get(x->m, &k);
}
static uintptr_t held_sum(nvn_hmap *m, CFSetRef set) {
    held_sum_ctx c = {m, 0};
    if (m->n) CFSetApplyFunction(set, held_sum_one, &c);
    return c.n;
}
static void drop_entries_naming_locked(CFSetRef gone, int live, int ghost, uint64_t **serials, uint32_t *nserials) {
    nvn_hmap *tables[] = {&g_by_serial, &g_ghost}, *held[] = {&g_by_serial_held, &g_ghost_held};
    int want[] = {live, ghost};
    uint64_t *ks = NULL;
    uint32_t nk = 0, kcap = 0, nlive = 0;
    for (unsigned t = 0; t < 2; t++) {
        uintptr_t named = want[t] && tables[t]->n ? held_sum(held[t], gone) : 0, found = 0;
        for (uint32_t i = 0; named && found < named && i < tables[t]->cap; i++) {
            void *o = tables[t]->vals[i];
            if (!o) continue;
            int hit = 0;
            for (id x = (__bridge id)o; x && !hit; x = view_parent(x)) hit = CFSetContainsValue(gone, (__bridge const void *)x);
            if (!hit) continue;
            for (id x = (__bridge id)o; x; x = view_parent(x)) found += CFSetContainsValue(gone, (__bridge const void *)x);
            if (nk == kcap) {
                kcap = kcap ? kcap * 2 : 64;
                uint64_t *n = realloc(ks, kcap * sizeof *ks);
                if (!n) ODIE("the released textures' serials");
                ks = n;
            }
            memcpy(&ks[nk++], tables[t]->keys + (size_t)i * tables[t]->klen, sizeof *ks);
        }
        if (!t) nlive = nk;
    }
    for (uint32_t i = 0; i < nk; i++) {
        if (i < nlive) live_del_locked(ks[i]);
        else ghost_del_locked(ks[i]);
    }
    *serials = ks;
    *nserials = nk;
}
// A pool's Finalize ends every texture made on its storage: its sharing-table entries, its live objects' entries
// (docs/reference/memory.md#a-texture-object-finalized-after-its-pool-kept-the-pools-memory) and ghost entries naming them. `also` (may be NULL): the replay layer's other objects
// of the pool. Releases wait for the submission in flight. → sharing-table entries gone. Caller holds g_smu
static void gone_add(CFMutableSetRef gone, id o) {
    if (!o) return;
    CFSetAddValue(gone, (__bridge const void *)o);
    own_hook_parts(o, ^(id part) { if (part) CFSetAddValue(gone, (__bridge const void *)part); });
}
uint32_t own_pool_finalize_locked(const void *pool, CFSetRef also) {
    uint32_t n = 0;
    alias_key *ks = NULL;
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        alias_key k;
        memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        if (k.pool != (uint64_t)(uintptr_t)pool) continue;
        alias_key *nk = realloc(ks, (n + 1) * sizeof *ks);
        if (!nk) ODIE("the finalized pool's shared textures");
        ks = nk;
        ks[n++] = k;
    }
    CFMutableSetRef gone = CFSetCreateMutable(NULL, 0, NULL);
    if (!gone) ODIE("the finalized pool's textures");
    for (uint32_t j = 0; j < n; j++) gone_add(gone, hm_get_id(&g_alias, &ks[j]));
    if (also) {
        CFIndex na = CFSetGetCount(also);
        const void **vs = malloc((size_t)(na ? na : 1) * sizeof *vs);
        if (!vs) ODIE("the finalized pool's textures");
        CFSetGetValues(also, vs);
        for (CFIndex j = 0; j < na; j++) gone_add(gone, (__bridge id)vs[j]);
        free(vs);
    }
    uintptr_t pk = (uintptr_t)pool;
    CFMutableSetRef mine = nvn_hm_del(&g_pool_tex, &pk);
    CFIndex nm = mine ? CFSetGetCount(mine) : 0;
    const void **ms = NULL;
    if (nm) {
        ms = malloc((size_t)nm * sizeof *ms);
        if (!ms) ODIE("the finalized pool's textures");
        CFSetGetValues(mine, ms);
    }
    uint64_t dropped = 0;
    for (CFIndex j = 0; j < nm; j++) {
        uint64_t serial = (uint64_t)(uintptr_t)ms[j];
        nvn_hm_del(&g_tex_pool, &serial);
        id o = hm_get_id(&g_by_serial, &serial);
        if (!o) continue;
        gone_add(gone, o);
        live_del_locked(serial);
        dropped++;
    }
    free(ms);
    if (mine) CFRelease(mine);
    if (CFSetGetCount(gone)) {
        uint64_t *sers = NULL;
        uint32_t nsers = 0;
        drop_entries_naming_locked(gone, 1, 1, &sers, &nsers);
        free(sers);
        dropped += nsers;
    }
    CFRelease(gone);
    for (uint32_t j = 0; j < n; j++) alias_del(&ks[j]);
    free(ks);
    g_alias_evicted += n;
    g_alias_pool_entries += dropped;
    if (n) own_epoch();   // no note: the pool's IDs are many and its finalize marks them gone
    return n;
}
own_dead_stats own_alias_dead_locked(uint32_t now, uint32_t idle, own_use_fn use_of, uint32_t max, uint64_t **serials, uint32_t *nserials) {
    own_dead_stats st = {0};
    *serials = NULL;
    *nserials = 0;
    if (!g_alias.n) return st;
    uint64_t sub = own_hook_submission();
    CFMutableSetRef pinned = CFSetCreateMutable(NULL, 0, NULL), dead = CFSetCreateMutable(NULL, 0, NULL);   // a pin holds a view's parents too
    for (uint32_t i = 0; i < g_tex_pins.cap; i++) {
        tex_pin *pin = g_tex_pins.vals[i];
        if (!pin || !pin->object) continue;
        for (id o = (__bridge id)pin->object; o; o = view_parent(o)) CFSetAddValue(pinned, (__bridge const void *)o);
    }
    alias_key *vk = NULL;
    uint32_t nv = 0, cap = 0;
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        void *val = g_alias.vals[i];
        if (!val) continue;
        const alias_key *k = (const alias_key *)(const void *)(g_alias.keys + (size_t)i * g_alias.klen);
        own_use u;
        if (!use_of((__bridge id)val, &u) || nv >= max || !u.use || now - u.use < idle) continue;
        uint64_t seen = (uint64_t)(uintptr_t)nvn_hm_get(&g_alias_seen, k);
        if (seen && sub - seen < ALIAS_IDLE) continue;
        nvn_texlayout L;
        nvn_tex_layout(&k->d, &L);
        if (!L.size) continue;
        int64_t lo = k->off, hi = k->off + (int64_t)L.size;
        int over = 0;
        for (int64_t c = lo >> ALIAS_CELL; c <= (hi - 1) >> ALIAS_CELL && !over; c++) {
            uint64_t ck[2] = {k->pool, (uint64_t)c};
            const alias_cell *v = nvn_hm_get(&g_alias_cells, ck);
            for (uint32_t j = 0; v && j < v->n && !over; j++) {
                if (!memcmp(&v->e[j].k, k, sizeof *k) || v->e[j].k.off >= hi || v->e[j].end <= lo) continue;
                void *x = nvn_hm_get(&g_alias, &v->e[j].k);
                own_use xu;
                over = x && x != val && use_of((__bridge id)x, &xu) && xu.wr > u.use;
            }
        }
        if (!over) continue;
        if (CFSetContainsValue(pinned, val)) {
            st.pinned++;
            continue;
        }
        if (nv == cap) {
            cap = cap ? cap * 2 : 64;
            alias_key *n = realloc(vk, cap * sizeof *vk);
            if (!n) ODIE("the released shared textures");
            vk = n;
        }
        vk[nv++] = *k;
        CFSetAddValue(dead, val);
    }
    CFRelease(pinned);
    if (nv) {
        uint64_t *ks = NULL;
        uint32_t nk = 0;
        drop_entries_naming_locked(dead, 1, 1, &ks, &nk);
        for (uint32_t j = 0; j < nv; j++) {
            id o = hm_get_id(&g_alias, &vk[j]);
            if ([o respondsToSelector:@selector(allocatedSize)]) st.bytes += ((id<MTLTexture>)o).allocatedSize;
            alias_del(&vk[j]);
        }
        st.n = nv;
        st.serials = nk;
        *serials = ks;
        *nserials = nk;
    }
    free(vk);
    CFRelease(dead);
    return st;
}

// Pinned by the program's immutable serial: no serial aliases (they would invalidate texture tables and composite-program keys)
typedef struct { unsigned refs, retired; } prog_pin;
static nvn_hmap g_prog_pins = {.klen = 8};   // under the submission lock
void mtl_prog_release(prog_t *p) {
    if (!p->serial) return;
    prog_pin *pin = nvn_hm_get(&g_prog_pins, &p->serial);
    if (pin) pin->retired = 1;
    else serial_del(p->serial);
}
void mtl_prog_unpin(prog_t *p) {
    prog_pin *pin = nvn_hm_get(&g_prog_pins, &p->serial);
    if (pin && !--pin->refs) {
        nvn_hm_del(&g_prog_pins, &p->serial);
        if (pin->retired) serial_del(p->serial);
        free(pin);
    }
}
void own_prog_pin(uint64_t serial) {
    prog_pin *pin = nvn_hm_get(&g_prog_pins, &serial);
    if (!pin) {
        pin = calloc(1, sizeof *pin);
        if (!pin) ODIE("program retention allocation failed");
        nvn_hm_put(&g_prog_pins, &serial, pin);
    }
    pin->refs++;
}

own_alias_unheld own_alias_unheld_locked(void) {
    own_alias_unheld s = {0};
    uint64_t now = own_hook_submission();
    CFMutableSetRef sized = CFSetCreateMutable(NULL, 0, NULL);
    for (uint32_t i = 0; i < g_alias.cap; i++) {
        void *v = g_alias.vals[i];
        id o = (__bridge id)v;
        if (!v || entry_holds(v) || tex_object_pinned(o)) continue;
        s.entries++;
        alias_key k;
        memcpy(&k, g_alias.keys + (size_t)i * g_alias.klen, sizeof k);
        uint64_t seen = (uint64_t)(uintptr_t)nvn_hm_get(&g_alias_seen, &k);
        if (seen && now > seen && now - seen > s.oldest_idle) s.oldest_idle = now - seen;
        if (CFSetContainsValue(sized, v) || ![o respondsToSelector:@selector(allocatedSize)]) continue;
        CFSetAddValue(sized, v);
        uint64_t b = ((id<MTLResource>)o).allocatedSize;
        s.bytes += b;
        if ([o respondsToSelector:@selector(usage)] && (((id<MTLTexture>)o).usage & (MTLTextureUsageRenderTarget | MTLTextureUsageShaderWrite))) s.writable_bytes += b;
    }
    CFRelease(sized);
    return s;
}
own_stats own_stats_locked(void) { return (own_stats){.live = g_by_serial.n, .live_cap = g_by_serial.cap, .ghost = g_ghost.n, .retire_wait = g_retire_wait.n, .ghost_puts = g_ghost_puts}; }
void own_each_locked(void (^fn)(uint64_t serial, id o, int ghost)) {
    const nvn_hmap *tables[] = {&g_by_serial, &g_ghost};
    for (int j = 0; j < 2; j++) for (uint32_t i = 0; i < tables[j]->cap; i++) {
        if (!tables[j]->vals[i]) continue;
        uint64_t k;
        memcpy(&k, tables[j]->keys + (size_t)i * 8, 8);
        fn(k, (__bridge id)tables[j]->vals[i], j);
    }
}
uint32_t own_drop_where_locked(int (^gone)(id o), void (^each)(uint64_t serial)) {
    uint32_t n = 0;
    for (int j = 0; j < 2; j++) {   // keys first, then deletes (a delete moves entries)
        nvn_hmap *t = j ? &g_ghost : &g_by_serial;
        uint64_t *ks = NULL;
        uint32_t nk = 0;
        for (uint32_t i = 0; i < t->cap; i++) {
            if (!t->vals[i] || !gone((__bridge id)t->vals[i])) continue;
            uint64_t *nks = realloc(ks, (nk + 1) * sizeof *ks);
            if (!nks) ODIE("the dropped serials");
            ks = nks;
            memcpy(&ks[nk++], t->keys + (size_t)i * 8, 8);
        }
        for (uint32_t i = 0; i < nk; i++) {
            if (j) ghost_del_locked(ks[i]);
            else live_del_locked(ks[i]);
            if (each) each(ks[i]);
        }
        n += nk;
        free(ks);
    }
    return n;
}
static void expect_one(CFMutableDictionaryRef want, id o) {
    for (; o; o = view_parent(o))
        CFDictionarySetValue(want, (__bridge const void *)o, (const void *)((uintptr_t)CFDictionaryGetValue(want, (__bridge const void *)o) + 1));
}
static int counts_match(const char *what, CFMutableDictionaryRef want, const nvn_hmap *held, char *why, size_t n) {
    for (uint32_t i = 0; i < held->cap; i++) {
        if (!held->vals[i]) continue;
        uintptr_t k; memcpy(&k, held->keys + (size_t)i * held->klen, sizeof k);
        uintptr_t w = (uintptr_t)CFDictionaryGetValue(want, (const void *)k);
        if (w != (uintptr_t)held->vals[i]) { snprintf(why, n, "%s count of %#lx: %lu, the tables say %lu", what, (unsigned long)k, (unsigned long)(uintptr_t)held->vals[i], (unsigned long)w); return 0; }
    }
    if ((CFIndex)held->n != CFDictionaryGetCount(want)) { snprintf(why, n, "%s counts: %u objects, the tables hold %ld", what, held->n, (long)CFDictionaryGetCount(want)); return 0; }
    return 1;
}
int own_check_locked(char *why, size_t n) {
    CFMutableDictionaryRef live = CFDictionaryCreateMutable(NULL, 0, NULL, NULL), ghost = CFDictionaryCreateMutable(NULL, 0, NULL, NULL);
    for (uint32_t i = 0; i < g_by_serial.cap; i++) if (g_by_serial.vals[i]) expect_one(live, (__bridge id)g_by_serial.vals[i]);
    for (uint32_t i = 0; i < g_tex_pins.cap; i++) { tex_pin *pin = g_tex_pins.vals[i]; if (pin && pin->object) expect_one(live, (__bridge id)pin->object); }
    for (uint32_t i = 0; i < g_ghost.cap; i++) if (g_ghost.vals[i]) expect_one(ghost, (__bridge id)g_ghost.vals[i]);
    int ok = counts_match("live", live, &g_by_serial_held, why, n) && counts_match("ghost", ghost, &g_ghost_held, why, n);
    // Each ghost serial has one place in the use list and back, links agree, uses go oldest to newest
    uint32_t places = 0; const ghost_use *prev = NULL;
    for (const ghost_use *u = g_ghost_lru; ok && u; prev = u, u = u->next) {
        if (++places > g_ghost_use.n) { snprintf(why, n, "the ghost list is longer than its %u records (a cycle)", g_ghost_use.n); ok = 0; break; }
        if (u->prev != prev) { snprintf(why, n, "ghost list: serial %llu's back link is wrong", (unsigned long long)u->serial); ok = 0; }
        else if (nvn_hm_get(&g_ghost_use, &u->serial) != u) { snprintf(why, n, "ghost list: serial %llu's record is not the one its table names", (unsigned long long)u->serial); ok = 0; }
        else if (!nvn_hm_get(&g_ghost, &u->serial)) { snprintf(why, n, "ghost list: serial %llu has no ghost entry", (unsigned long long)u->serial); ok = 0; }
        else if (prev && (u->frame < prev->frame || u->sub < prev->sub)) { snprintf(why, n, "ghost list: serial %llu (frame %llu, submission %llu) used before the one ahead of it", (unsigned long long)u->serial, (unsigned long long)u->frame, (unsigned long long)u->sub); ok = 0; }
    }
    if (ok && prev != g_ghost_mru) { snprintf(why, n, "the ghost list does not end at its most recently used entry"); ok = 0; }
    if (ok && (places != g_ghost.n || g_ghost_use.n != g_ghost.n)) { snprintf(why, n, "%u places in the ghost list, %u records, %u ghost serials", places, g_ghost_use.n, g_ghost.n); ok = 0; }
    uint64_t in_sets = 0;
    for (uint32_t i = 0; ok && i < g_pool_tex.cap; i++) {
        CFSetRef set = g_pool_tex.vals[i]; if (!set) continue;
        uintptr_t pool; memcpy(&pool, g_pool_tex.keys + (size_t)i * g_pool_tex.klen, sizeof pool);
        CFIndex m = CFSetGetCount(set); in_sets += (uint64_t)m;
        if (!m) { snprintf(why, n, "pool %#lx has an empty set of live textures", (unsigned long)pool); ok = 0; break; }
        const void **vs = malloc((size_t)m * sizeof *vs); if (!vs) ODIE("the check's pool index"); CFSetGetValues(set, vs);
        for (CFIndex j = 0; ok && j < m; j++) {
            uint64_t serial = (uint64_t)(uintptr_t)vs[j];
            if ((uintptr_t)nvn_hm_get(&g_tex_pool, &serial) != pool) { snprintf(why, n, "serial %llu is in pool %#lx's set but indexed under another", (unsigned long long)serial, (unsigned long)pool); ok = 0; }
            else if (!nvn_hm_get(&g_tex_live, &serial)) { snprintf(why, n, "serial %llu is in pool %#lx's set but not live", (unsigned long long)serial, (unsigned long)pool); ok = 0; }
        }
        free(vs);
    }
    if (ok && in_sets != g_tex_pool.n) { snprintf(why, n, "%llu serials in the pools' sets, %u indexed", (unsigned long long)in_sets, g_tex_pool.n); ok = 0; }
    CFMutableSetRef vals = CFSetCreateMutable(NULL, 0, NULL); uint64_t want_ents = 0;
    for (uint32_t i = 0; ok && i < g_alias.cap; i++) {
        if (!g_alias.vals[i]) continue;
        const alias_key *k = (const alias_key *)(const void *)(g_alias.keys + (size_t)i * g_alias.klen);
        CFSetAddValue(vals, g_alias.vals[i]);
        if (!own_shared_locked((__bridge id)g_alias.vals[i])) { snprintf(why, n, "shared texture %p (pool %#llx offset %#llx) is not in the set of shared textures", g_alias.vals[i], (unsigned long long)k->pool, (unsigned long long)k->off); ok = 0; break; }
        nvn_texlayout L; nvn_tex_layout(&k->d, &L);
        int64_t end = k->off + (int64_t)L.size, c1 = (end > k->off ? end - 1 : k->off) >> ALIAS_CELL;
        for (int64_t c = k->off >> ALIAS_CELL; ok && c <= c1; c++) {
            uint64_t ck[2] = {k->pool, (uint64_t)c}; const alias_cell *v = nvn_hm_get(&g_alias_cells, ck); uint32_t j = 0;
            if (v) for (; j < v->n; j++) if (!memcmp(&v->e[j].k, k, sizeof *k)) break;
            if (!v || j == v->n) { snprintf(why, n, "shared entry (pool %#llx offset %#llx) is missing from cell %lld of the index", (unsigned long long)k->pool, (unsigned long long)k->off, (long long)c); ok = 0; }
            want_ents++;
        }
    }
    uint64_t ents = 0;
    for (uint32_t i = 0; ok && i < g_alias_cells.cap; i++) if (g_alias_cells.vals[i]) ents += ((const alias_cell *)g_alias_cells.vals[i])->n;
    if (ok && ents != want_ents) { snprintf(why, n, "%llu index entries for the %llu cells the shared entries reach", (unsigned long long)ents, (unsigned long long)want_ents); ok = 0; }
    CFIndex nset = g_alias_set ? CFSetGetCount(g_alias_set) : 0;
    if (ok && nset != CFSetGetCount(vals)) { snprintf(why, n, "%ld textures in the set of shared textures, %ld in the table", (long)nset, (long)CFSetGetCount(vals)); ok = 0; }
    for (uint32_t i = 0; ok && i < g_alias_seen.cap; i++)
        if (g_alias_seen.vals[i] && !nvn_hm_get(&g_alias, g_alias_seen.keys + (size_t)i * g_alias_seen.klen)) { snprintf(why, n, "a last lookup kept for a key with no shared entry"); ok = 0; }
    CFRelease(vals);
    for (uint32_t i = 0; ok && i < g_retire_wait.cap; i++)
        if (g_retire_wait.vals[i] && !entry_holds(g_retire_wait.vals[i])) { snprintf(why, n, "retired object %p waits with no entry holding it", g_retire_wait.vals[i]); ok = 0; }
    CFRelease(live); CFRelease(ghost);
    return ok;
}
