// Inline uniform updates, pages the GPU writes, uniform shadows made from memory (ubp_get), buffer fills, events and counter reports, visibility buffers, counter timestamps.
// An inline update's data is copied into the host's ring at encode time: the game may recycle its command memory before the GPU runs.
static id<MTLBuffer> g_ubs_buf;
static size_t g_ubs_off, g_ubs_size;
// Inline updates inside a pass are deferred instead of ending it (Metal cannot copy into a buffer inside a pass); NVN applies them in command order, so a draw whose
// binding overlaps one reads a shadow. The pass still ends before a draw that would need more (ubd_conflict).
static size_t arena_alloc(size_t n, __strong id<MTLBuffer> *buf);
static inline void args_dirty_all(void);
static void ubd_dirty(uint64_t lo, uint64_t hi);
typedef struct { uint64_t lo, hi, gpu; size_t off; __unsafe_unretained id<MTLBuffer> buf; } ubd_ent;   // a ring block replaced meanwhile lives until this submission completes
enum { UBD_MAX = 512 };
static ubd_ent g_ubd[UBD_MAX];
static int g_nubd;
static uint64_t g_ubd_deferred, g_ubd_conflicts;
static int ubd_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_UB_DEFER");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_UB_DEFER=0: uniform updates inside a pass end the pass as before");
    }
    return v;
}
void mtl_ubd_stats(uint64_t *deferred, uint64_t *conflicts) { *deferred = g_ubd_deferred; *conflicts = g_ubd_conflicts; }
// Deferred updates are landed only when something needs the memory itself (a copy or dispatch, a non-uniform read, the submission end, more than n of them).
static int ubd_lazy(void) {   // 0 = off
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_UB_LAZY");
        v = e ? atoi(e) : 160;
        if (v < 0) v = 0;
    }
    return v;
}
enum { UBD_PF = 1024 };
static uint64_t g_ubd_pf[UBD_PF];   // pages (address >> 12, +1), open addressing; cleared when the list is landed
static int g_ubd_wide;              // an update spans more than 16 pages: the filter is skipped
// the span of every update listed since the list was last emptied
static uint64_t g_ubd_env_lo = UINT64_MAX, g_ubd_env_hi;
static inline unsigned ubd_pf_slot(uint64_t pg) { return (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> 54) & (UBD_PF - 1); }
// Pages inside the guest address space (0x70_0000_0000) are in a bitmap as well; pages outside it use the table only.
enum { UBD_BM_SHIFT = 21 };
static const uint64_t UBD_BM_BASE = 0x7000000000ull;
static uint64_t g_ubd_bm[(1u << UBD_BM_SHIFT) / 64];
static inline int ubd_bm_in(uint64_t lo, uint64_t hi) { return lo >= UBD_BM_BASE && ((hi - 1 - UBD_BM_BASE) >> 12) < (1u << UBD_BM_SHIFT); }
static inline int ubd_bm_page(uint64_t a, uint64_t *pg) {   // a: address >> 12
    if (a < UBD_BM_BASE >> 12 || a - (UBD_BM_BASE >> 12) >= (1u << UBD_BM_SHIFT)) return 0;
    *pg = a - (UBD_BM_BASE >> 12);
    return 1;
}
static void ubd_pf_add(uint64_t lo, uint64_t hi) {
    if (lo < g_ubd_env_lo) g_ubd_env_lo = lo;
    if (hi > g_ubd_env_hi) g_ubd_env_hi = hi;
    if (((hi - 1) >> 12) - (lo >> 12) > 16) { g_ubd_wide = 1; return; }
    for (uint64_t a = lo >> 12, pg; a <= (hi - 1) >> 12; a++)   // a lookup inside the window asks only the bitmap
        if (ubd_bm_page(a, &pg)) g_ubd_bm[pg >> 6] |= 1ull << (pg & 63);
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) {
        unsigned k = ubd_pf_slot(pg);
        while (g_ubd_pf[k] && g_ubd_pf[k] != pg + 1) k = (k + 1) & (UBD_PF - 1);
        g_ubd_pf[k] = pg + 1;
    }
}
static inline int ubd_pf_hit(uint64_t lo, uint64_t hi) {
    if (hi <= g_ubd_env_lo || lo >= g_ubd_env_hi) return 0;
    if (g_ubd_wide || ((hi - 1) >> 12) - (lo >> 12) > 16) return 1;
    if (ubd_bm_in(lo, hi)) {
        for (uint64_t pg = (lo - UBD_BM_BASE) >> 12; pg <= (hi - 1 - UBD_BM_BASE) >> 12; pg++) if (g_ubd_bm[pg >> 6] >> (pg & 63) & 1) return 1;
        return 0;
    }
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) {
        unsigned k = ubd_pf_slot(pg);
        while (g_ubd_pf[k]) {
            if (g_ubd_pf[k] == pg + 1) return 1;
            k = (k + 1) & (UBD_PF - 1);
        }
    }
    return 0;
}
static void ubd_bm_clear(void) {   // before the list is emptied
    for (int i = 0; i < g_nubd; i++) {
        uint64_t lo = g_ubd[i].lo, hi = g_ubd[i].hi;
        if (hi <= lo || ((hi - 1) >> 12) - (lo >> 12) > 16) continue;
        for (uint64_t a = lo >> 12, pg; a <= (hi - 1) >> 12; a++)
            if (ubd_bm_page(a, &pg)) g_ubd_bm[pg >> 6] &= ~(1ull << (pg & 63));
    }
}
static int ubd_hits_scan(uint64_t lo, uint64_t hi) {
    if (hi <= lo || !ubd_pf_hit(lo, hi)) return 0;
    for (int i = 0; i < g_nubd; i++) if (lo < g_ubd[i].hi && g_ubd[i].lo < hi) return 1;
    return 0;
}
// any order of overlap
static int ubd_covers_scan(uint64_t lo, uint64_t hi) {
    for (uint64_t at = lo; at < hi;) {
        uint64_t next = at;
        for (int i = 0; i < g_nubd; i++) if (g_ubd[i].lo <= at && g_ubd[i].hi > next) next = g_ubd[i].hi;
        if (next == at) return 0;
        at = next;
    }
    return 1;
}
// g_ubd_ver changes with every update deferred and every landing.
static uint64_t g_ubd_ver = 1;
typedef struct { uint64_t lo, hi, ver, shadow; int8_t hits, covers; } ubd_memo_ent;   // shadow: 0 = not made
enum { UBD_MEMO = 512 };
static ubd_memo_ent g_ubd_memo[UBD_MEMO];
__attribute__((noinline)) static int ubd_memo_init(void) {
    const char *e = getenv("DM_NVN_UBD_MEMO");
    int v = !(e && !strcmp(e, "0"));
    if (!v) MLOG("DM_NVN_UBD_MEMO=0: deferred update lookups scan the list every time");
    return v;
}
static int g_ubd_memo_on = -1;
static inline int ubd_memo_on(void) { if (__builtin_expect(g_ubd_memo_on < 0, 0)) g_ubd_memo_on = ubd_memo_init(); return g_ubd_memo_on; }
static inline ubd_memo_ent *ubd_memo(uint64_t lo, uint64_t hi) {
    ubd_memo_ent *m = &g_ubd_memo[(unsigned)(((lo * 0x9E3779B97F4A7C15ull) ^ (hi * 0xC2B2AE3D27D4EB4Full)) >> 55) & (UBD_MEMO - 1)];
    if (m->ver != g_ubd_ver || m->lo != lo || m->hi != hi) {
        m->lo = lo; m->hi = hi; m->ver = g_ubd_ver;
        m->hits = m->covers = -1; m->shadow = 0;
    }
    return m;
}
static int ubd_hits(uint64_t lo, uint64_t hi) {
    if (hi <= lo || !ubd_pf_hit(lo, hi)) return 0;
    if (!ubd_memo_on()) return ubd_hits_scan(lo, hi);
    ubd_memo_ent *m = ubd_memo(lo, hi);
    if (m->hits < 0) m->hits = (int8_t)ubd_hits_scan(lo, hi);
    return m->hits;
}
static int ubd_covers(uint64_t lo, uint64_t hi) {
    if (hi <= lo) return 1;
    if (!ubd_memo_on()) return ubd_covers_scan(lo, hi);
    ubd_memo_ent *m = ubd_memo(lo, hi);
    if (m->covers < 0) m->covers = (int8_t)ubd_covers_scan(lo, hi);
    return m->covers;
}
// One deferred update holding all of it and suitably aligned is used in place; otherwise the updates are laid into the arena in order.
static uint64_t ubd_shadow_(uint64_t lo, uint64_t hi);
static uint64_t ubd_shadow(uint64_t lo, uint64_t hi) {
    if (!ubd_memo_on()) return ubd_shadow_(lo, hi);
    ubd_memo_ent *m = ubd_memo(lo, hi);
    if (!m->shadow) m->shadow = ubd_shadow_(lo, hi);
    return m->shadow;
}
static uint64_t ubd_shadow_(uint64_t lo, uint64_t hi) {
    for (int i = g_nubd - 1; i >= 0; i--) {
        const ubd_ent *e = &g_ubd[i];
        if (e->lo <= lo && hi <= e->hi) {
            int later = 0;
            for (int j = i + 1; j < g_nubd; j++) later |= lo < g_ubd[j].hi && g_ubd[j].lo < hi;
            uint64_t g = e->gpu + (lo - e->lo);
            if (!later && !(g & 15)) return g;
            break;
        }
    }
    id<MTLBuffer> ab;
    size_t o = arena_alloc((size_t)(hi - lo), &ab);
    uint8_t *d = (uint8_t *)ab.contents + o;
    for (int i = 0; i < g_nubd; i++) {
        const ubd_ent *e = &g_ubd[i];
        uint64_t a = e->lo > lo ? e->lo : lo, b = e->hi < hi ? e->hi : hi;
        if (a < b) memcpy(d + (a - lo), (const uint8_t *)e->buf.contents + e->off + (a - e->lo), (size_t)(b - a));
    }
    return ab.gpuAddress + o;
}
static void ubd_flush(void) {
    ubd_bm_clear();
    int n = g_nubd;
    g_nubd = 0; g_ubd_ver++;
    memset(g_ubd_pf, 0, sizeof g_ubd_pf); g_ubd_wide = 0; g_ubd_env_lo = UINT64_MAX; g_ubd_env_hi = 0;
    for (int i = 0; i < n; i++) {
        const ubd_ent *e = &g_ubd[i];
        void *db;
        uint64_t dof;
        if (!mtl_addr(e->lo, e->hi - e->lo, &db, &dof)) continue;
        enc_b_copy_buffer(blit_buf(0, 0, e->lo, e->hi), e->buf, e->off, (__bridge id<MTLBuffer>)db, dof, e->hi - e->lo);
        g_gp_ubd++;
    }
}
static void ubd_read(uint64_t lo, size_t n, void *out) {
    for (int i = 0; i < g_nubd; i++) {
        const ubd_ent *e = &g_ubd[i];
        uint64_t a = e->lo > lo ? e->lo : lo, b = e->hi < lo + n ? e->hi : lo + n;
        if (a < b) memcpy((uint8_t *)out + (a - lo), (const uint8_t *)e->buf.contents + e->off + (a - e->lo), (size_t)(b - a));
    }
}
// A partly covered binding: the shadow starts from memory at encode time and lays over every inline update queued in this submission, in queue order. Made only when
// nothing else can have written the range by then (pend kinds 2–4, sbw, gw); otherwise the pass ends.
static int ub_partial_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_UB_PARTIAL");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_UB_PARTIAL=0: a binding the deferred uniform updates cover only in part ends the pass as before");
    }
    return v;
}
// 7-way set-associative, a set one 128-byte cache line, with a per-set overflow fence: a direct-mapped slot would stay taken while either of two colliding pages is written.
enum { GW_BITS = 15, GW_SETS = 1 << GW_BITS, GW_WAYS = 7, GW_PAGES = SBW_WPAGES, GWR_BITS = 10, GWR_SETS = 1 << GWR_BITS };
typedef struct { _Alignas(128) uint64_t page[GW_WAYS]; uint64_t fence[GW_WAYS], over_fence, pad; } gw_set;
_Static_assert(sizeof(gw_set) == 128 && _Alignof(gw_set) == 128, "gw_set fits one 128-byte cache line");
static gw_set g_gwx[GW_SETS];   // GPU writes of running submissions except inline updates remembered in g_um
static uint64_t g_gw_all;                            // every page counts as written until this fence completes
static uint64_t g_ubp_made, g_ubp_reused, g_ubp_refused;
static uint64_t g_ubp_why[6];   // 0 too large or off, 1 sbw, 2 gw, 3 a copy, fill or feedback write queued, 4 unreadable or too many writes, 5 hw_live
static inline unsigned gw_slot_b(uint64_t pg, unsigned bits) { return (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> (64 - bits)) & ((1u << bits) - 1); }
static inline unsigned gw_slot(uint64_t pg) { return gw_slot_b(pg, GW_BITS); }
// Writes of GW_PAGES pages or more, kept as intervals: expanded they would fill the page table, and as a write anywhere refuse every shadow of the next submission.
enum { GWW_N = 128 };
static struct { uint64_t lo, hi, fence; } g_gww[GWW_N];
static int g_gww_used;
static void gww_put(uint64_t lo, uint64_t hi, uint64_t fence, uint64_t done) {
    int free_slot = -1;
    for (int i = 0; i < g_gww_used; i++) {
        if (g_gww[i].lo == lo && g_gww[i].hi == hi) {
            if (fence > g_gww[i].fence) g_gww[i].fence = fence;
            return;
        }
        if (free_slot < 0 && g_gww[i].fence <= done) free_slot = i;
    }
    if (free_slot < 0 && g_gww_used < GWW_N) free_slot = g_gww_used++;
    if (free_slot >= 0) {
        g_gww[free_slot].lo = lo; g_gww[free_slot].hi = hi; g_gww[free_slot].fence = fence;
    }
    else if (fence > g_gw_all) g_gw_all = fence;
}
static void gw_put_in(gw_set *t, unsigned bits, uint64_t lo, uint64_t hi, uint64_t fence, uint64_t done) {
    if (hi <= lo) return;
    uint64_t p0 = lo >> 12, p1 = (hi - 1) >> 12;
    if (p1 - p0 >= GW_PAGES) {
        uint64_t last = (hi - 1) | 4095ull;
        gww_put(p0 << 12, last == UINT64_MAX ? last : last + 1, fence, done);
        return;
    }
    for (uint64_t pg = p0; pg <= p1; pg++) {
        gw_set *s = &t[gw_slot_b(pg, bits)]; int free_i = -1, hit = 0;
        for (int w = 0; w < GW_WAYS; w++) {
            if (s->page[w] == pg) { if (fence > s->fence[w]) s->fence[w] = fence; hit = 1; break; }
            if (free_i < 0 && s->fence[w] <= done) free_i = w;
        }
        if (hit) continue;
        if (free_i >= 0) { s->page[free_i] = pg; s->fence[free_i] = fence; }
        else if (fence > s->over_fence) s->over_fence = fence;
    }
}
static void gw_put(uint64_t lo, uint64_t hi, uint64_t fence, uint64_t done) { gw_put_in(g_gwx, GW_BITS, lo, hi, fence, done); }
static int gw_set_live(const gw_set *t, unsigned bits, uint64_t lo, uint64_t hi, uint64_t done) {
    if (hi <= lo) return 0;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) {
        const gw_set *s = &t[gw_slot_b(pg, bits)];
        if (s->over_fence > done) return 1;
        for (int w = 0; w < GW_WAYS; w++) if (s->page[w] == pg && s->fence[w] > done) return 1;
    }
    return 0;
}
static int gw_live_in(const gw_set *t, unsigned bits, uint64_t lo, uint64_t hi) {
    uint64_t done = g_done;   // read without the lock: a stale value only refuses more
    if (g_gw_all > done) return 1;
    for (int i = 0; i < g_gww_used; i++) if (g_gww[i].fence > done && lo < g_gww[i].hi && g_gww[i].lo < hi) return 1;
    return gw_set_live(t, bits, lo, hi, done);
}
// Counter reports the host writes into guest memory at completion, kept by their own 16 bytes: by page, a report sharing a page with uniform blocks would refuse every
// partly covered shadow over it while its submission runs.
enum { HW_N = 4096 };
static struct { uint64_t lo, fence; } g_hwr[HW_N];   // fences grow along the ring
static uint64_t g_nhwr;
static gw_set g_gwr[GWR_SETS];   // fast-reject filter
static int hw_exact_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_REPORT_EXACT");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_REPORT_EXACT=0: a counter report of a running submission refuses uniform shadows over its whole page");
    }
    return v;
}
static void hw_note(uint64_t addr) {
    if (g_nhw_sub == g_hw_cap) {
        int c = g_hw_cap ? g_hw_cap * 2 : 256;
        uint64_t *p = realloc(g_hw_sub, (size_t)c * sizeof *p);
        if (!p) { g_sbw_over = 1; return; }   // over: every page counts as written
        g_hw_sub = p; g_hw_cap = c;
    }
    g_hw_sub[g_nhw_sub++] = addr;
}
// a report's page holds other words no report writes
static int hw_sub_hits(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < g_nhw_sub; i++) if (lo < g_hw_sub[i] + 16 && g_hw_sub[i] < hi) return 1;
    return 0;
}
static int hw_live(uint64_t lo, uint64_t hi) {
    uint64_t done = g_done;
    if (!gw_set_live(g_gwr, GWR_BITS, lo, hi, done)) return 0;
    if (!hw_exact_on()) return 1;
    uint64_t n0 = g_nhwr > HW_N ? g_nhwr - HW_N : 0;
    for (uint64_t j = g_nhwr; j-- > n0;) {   // newest first: the older ones have landed
        unsigned q = (unsigned)(j % HW_N);
        if (g_hwr[q].fence <= done) break;
        if (lo < g_hwr[q].lo + 16 && g_hwr[q].lo < hi) return 1;
    }
    return 0;
}
static int um_page_live(uint64_t lo, uint64_t hi, uint64_t done);
static int gw_live(uint64_t lo, uint64_t hi) { uint64_t done = g_done; return gw_live_in(g_gwx, GW_BITS, lo, hi) || um_page_live(lo, hi, done) || hw_live(lo, hi); }
// Inline updates of running submissions, remembered by byte: the CPU knows them, so a partly covered shadow lays them over memory instead of refusing. A byte counts while its fence runs.
enum { UM_N = 256 };
typedef struct { uint64_t page, fence; uint64_t f[4096]; uint8_t val[4096]; } um_ent;   // f: each byte's fence; fence: the highest of them
static um_ent *g_um;
static inline unsigned um_slot(uint64_t pg) { return (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> 40) & (UM_N - 1); }
static int um_on(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_UBP_RUNNING");
        v = !(e && !strcmp(e, "0"));
        if (!v) MLOG("DM_NVN_UBP_RUNNING=0: inline updates of a running submission refuse partly covered uniform shadows");
    }
    return v;
}
static int um_page_live(uint64_t lo, uint64_t hi, uint64_t done) {
    if (!g_um || hi <= lo) return 0;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) {
        const um_ent *m = &g_um[um_slot(pg)];
        if (m->page == pg && m->fence > done) return 1;
    }
    return 0;
}
static void um_put(uint64_t lo, uint64_t hi, const uint8_t *src, uint64_t fence, uint64_t done) {
    if (!g_um) g_um = calloc(UM_N, sizeof *g_um);   // only the pages a game updates are touched
    if (!g_um) { gw_put_in(g_gwx, GW_BITS, lo, hi, fence, done); return; }
    for (uint64_t at = lo, end; at < hi; at = end) {
        uint64_t pg = at >> 12;
        end = MIN((pg + 1) << 12, hi);
        um_ent *m = &g_um[um_slot(pg)];
        if (m->page != pg && m->fence > done) { gw_put_in(g_gwx, GW_BITS, at, end, fence, done); continue; }
        if (m->page != pg || fence > m->fence) m->fence = fence;   // a page taken over: all its bytes' fences have finished
        m->page = pg;
        unsigned o = (unsigned)(at & 4095), n = (unsigned)(end - at);
        memcpy(&m->val[o], src + (at - lo), n);
        for (unsigned i = 0; i < n; i++) m->f[o + i] = fence;
    }
}
static void gw_commit(uint64_t fence) {
    uint64_t done = g_done;
    if ((g_pend_over || g_sbw_over) && fence > g_gw_all) g_gw_all = fence;
    for (int e = 0; e < g_npend; e++) {
        if (g_pend_kind[e] == 1 && g_pend_upd[e] && um_on()) {
            const upd_ent *u = &g_upd[g_pend_upd[e] - 1];
            um_put(g_pend_lo[e], g_pend_hi[e], (const uint8_t *)u->buf.contents + u->off + (g_pend_lo[e] - u->lo), fence, done);
        } else gw_put_in(g_gwx, GW_BITS, g_pend_lo[e], g_pend_hi[e], fence, done);
    }
    for (uint32_t i = 0; i < g_nsbw; i++) {
        unsigned h = g_sbw_list[i];
        if (g_sbwp[h].src & SBW_GPU) gw_put(g_sbwp[h].page << 12, (g_sbwp[h].page + 1) << 12, fence, done);
    }
    for (int i = 0; i < g_nsbw_w; i++) gw_put(g_sbw_wlo[i], g_sbw_whi[i], fence, done);
    int ring = g_nhw_sub <= HW_N && (!g_nhw_sub || g_hwr[(g_nhwr + (uint64_t)g_nhw_sub - 1) % HW_N].fence <= done);   // the slots they take have finished (the farthest is the newest)
    for (int i = 0; i < g_nhw_sub; i++) {
        uint64_t a = g_hw_sub[i];
        if (ring) {
            unsigned q = (unsigned)(g_nhwr++ % HW_N);
            g_hwr[q].lo = a; g_hwr[q].fence = fence;
            gw_put_in(g_gwr, GWR_BITS, a, a + 16, fence, done);
        } else gw_put(a, a + 16, fence, done);
    }
}
// ubp_get then copies that shadow here, since the overlay waits for its copy
static int um_hits(uint64_t lo, uint64_t hi) {
    if (!g_um) return 0;
    uint64_t done = g_done;
    for (uint64_t at = lo, end; at < hi; at = end) {
        uint64_t pg = at >> 12;
        end = MIN((pg + 1) << 12, hi);
        const um_ent *m = &g_um[um_slot(pg)];
        if (m->page != pg || m->fence <= done) continue;
        for (uint64_t x = at; x < end; x++) if (m->f[x & 4095] > done) return 1;
    }
    return 0;
}
// d holds [lo, hi); ready: the copy operations filling it, which come first. Not left to the copy worker: the next commit changes these bytes.
static void um_overlay(uint64_t lo, uint64_t hi, uint8_t *d, uint64_t ready) {
    if (!g_um) return;
    uint64_t done = g_done;
    int waited = 0;
    for (uint64_t at = lo, end; at < hi; at = end) {
        uint64_t pg = at >> 12;
        end = MIN((pg + 1) << 12, hi);
        const um_ent *m = &g_um[um_slot(pg)];
        if (m->page != pg || m->fence <= done) continue;
        for (uint64_t x = at; x < end; x++) {
            if (m->f[x & 4095] <= done) continue;
            if (!waited) { ubc_wait(ready); waited = 1; }
            d[x - lo] = m->val[x & 4095];
        }
    }
}
// -1 when they cannot all be told
static int pend_since(uint64_t a, uint64_t b, int since, int *out) {
    if (g_pend_over) return -1;
    int n = 0;
    uint64_t last = b - 1;
    if (!pend_chains(a, last)) {
        for (int e = g_npend - 1; e >= since; e--) if (a < g_pend_hi[e] && g_pend_lo[e] < b) out[n++] = e;
        return n;
    }
    for (uint32_t m = g_pend_lv; m; m &= m - 1) {
        int lv = __builtin_ctz(m);
        unsigned s = pend_shift(lv);
        for (uint64_t g = a >> s; g <= last >> s; g++) {
            unsigned bk = pend_bucket(g, lv);
            if (g_pend_hgen[bk] != g_pend_gen) continue;
            for (int k = g_pend_head[bk]; k >= 0 && g_pend_node[k].e >= since; k = g_pend_node[k].next) {   // chains run newest first
                int e = g_pend_node[k].e;
                if (a < g_pend_hi[e] && g_pend_lo[e] < b) out[n++] = e;
            }
        }
    }
    if (n > 1) {
        for (int i = 1; i < n; i++) {
            int v = out[i], j = i;
            while (j > 0 && out[j - 1] < v) { out[j] = out[j - 1]; j--; }
            out[j] = v;
        }
        int m = 1;
        for (int i = 1; i < n; i++) if (out[i] != out[m - 1]) out[m++] = out[i];
        n = m;
    }
    return n;
}
int nvn_host_readable(uint64_t a, uint64_t n);
// A copy source byte is read only where the command stream proves it, or from memory no unfinished GPU write covers.
static uint8_t rc_source(uint64_t a, unsigned n, uint8_t *out, int *unreliable) {
    uint8_t known=0; uint64_t done=atomic_load_explicit(&g_rc_complete,memory_order_acquire);
    for (unsigned b=0;b<n;b++) {
        uint64_t p=a+b; rc_report *r=rc_containing(p);
        if (r) {
            unsigned off=(unsigned)(p-r->addr);
            if (r->unreliable || r->opaque_unreliable || (r->reported && (r->report_dirty || r->report_fence>done))) *unreliable=1;
            if (r->opaque_later) continue;
            if ((r->reported && r->gpu_epoch!=g_rc_gpu_epoch) || rc_gpu_unknown(r,done)) { *unreliable=1; continue; }
            if (!r->dirty && r->fence<=done) {
                if (r->readable && !r->unreliable && !r->opaque_unreliable) { out[b]=*(const volatile uint8_t *)p; known|=(uint8_t)(1u<<b); }
            } else if (r->known>>off&1) { out[b]=((const uint8_t *)&r->value)[off]; known|=(uint8_t)(1u<<b); }
            continue;
        }
        int kind, page=sbw_slot(p>>12,0);
        if (pend_read(p,1,&out[b],&kind)) known|=(uint8_t)(1u<<b);
        else if (!pend_hits(p,1) && !g_sbw_over && !sbw_hits(p,p+1) && !(page>=0 && (g_sbwp[page].src & SBW_REPORT) && hw_sub_hits(p,p+1)) && !gw_live(p,p+1) && nvn_host_readable(p,1)) {
            out[b]=*(const uint8_t *)p; known|=(uint8_t)(1u<<b);
        }
    }
    return known;
}
// One kept while no write is queued after it; the next for the same range starts from it and lays over only the writes queued since.
enum { UBP_CACHE = 16, UBP_MAX_BYTES = 65536 };
typedef struct { uint64_t lo, hi, gpu; uint8_t *cpu; int npend; uint32_t gen, sbw; uint64_t ready; } ubp_ent;   // sbw: g_sbw_gen when last checked; ready: copy ops that fill it
static uint64_t g_ubp_ready;   // a CPU read of it waits for that (ubc_wait)
static ubp_ent g_ubpc[UBP_CACHE];
static unsigned g_ubpc_i;
// 0 when they cannot be known (the pass ends instead)
static int ubp_get(uint64_t lo, uint64_t hi, uint64_t *gpu, const uint8_t **cpu) {
    if (!ub_partial_on() || hi <= lo || hi - lo > UBP_MAX_BYTES) { g_ubp_why[0]++; return 0; }
    ubp_ent *c = NULL;
    for (int i = 0; i < UBP_CACHE; i++) if (g_ubpc[i].lo == lo && g_ubpc[i].hi == hi && g_ubpc[i].gen == g_pend_gen) { c = &g_ubpc[i]; break; }
    if (!c || c->sbw != g_sbw_gen) {
        if (g_sbw_over || sbw_hits(lo, hi)) { g_ubp_why[1]++; return 0; }
        if (c) c->sbw = g_sbw_gen;
        if (g_gml_on) {
            pthread_mutex_lock(&g_gsr_mu);
            uint64_t i = g_gsr_n++ % GSR_N;
            g_gsr[i].fence = g_fence + 1; g_gsr[i].lo = lo; g_gsr[i].hi = hi;
            pthread_mutex_unlock(&g_gsr_mu);
        }
    }
    if (c && c->npend == g_npend) {
        g_ubp_reused++;
        *gpu = c->gpu; *cpu = c->cpu; g_ubp_ready = c->ready;
        return 1;
    }
    int since = c ? c->npend : 0;
    if (!c && gw_live_in(g_gwx, GW_BITS, lo, hi)) { g_ubp_why[2]++; return 0; }
    if (!c && hw_live(lo, hi)) { g_ubp_why[5]++; return 0; }
    if (!c && !nvn_host_readable(lo, hi - lo)) { g_ubp_why[4]++; return 0; }
    int n = pend_since(lo, hi, since, g_ubp_idx);
    if (n < 0) { g_ubp_why[4]++; return 0; }
    // a write only newer ones cover is never read
    enum { COVN = 64 };
    uint64_t cl[COVN], ch[COVN];   // sorted and disjoint; exact = 0 once they no longer fit
    int nc = 0, nk = 0, exact = 1;
    for (int i = 0; i < n; i++) {
        int e = g_ubp_idx[i];
        uint64_t x = g_pend_lo[e] > lo ? g_pend_lo[e] : lo, y = g_pend_hi[e] < hi ? g_pend_hi[e] : hi;
        int j = 0;
        while (j < nc && ch[j] < y) j++;
        if (exact && j < nc && cl[j] <= x && y <= ch[j]) continue;
        if (g_pend_kind[e] != 1 || !g_pend_upd[e]) { g_ubp_why[3]++; return 0; }   // a copy, fill or feedback write the CPU cannot see
        g_ubp_idx[nk++] = e;
        if (!exact) continue;
        int k = 0;
        while (k < nc && ch[k] < x) k++;
        int m = k;
        uint64_t nl = x, nh = y;
        while (m < nc && cl[m] <= y) { nl = MIN(nl, cl[m]); nh = MAX(nh, ch[m]); m++; }
        if (nc - (m - k) + 1 > COVN) { exact = 0; continue; }
        memmove(&cl[k + 1], &cl[m], (nc - m) * sizeof *cl);
        memmove(&ch[k + 1], &ch[m], (nc - m) * sizeof *ch);
        nc = nc - (m - k) + 1;
        cl[k] = nl; ch[k] = nh;
        if (nc == 1 && cl[0] == lo && ch[0] == hi) break;
    }
    uint64_t len = hi - lo;
    id<MTLBuffer> ab;
    size_t o = arena_alloc((size_t)len, &ab);
    uint8_t *d = (uint8_t *)ab.contents + o;
    int defer = len >= 512 && ubc_on() && (c || !um_hits(lo, hi));   // small ones (bindless handles read on the CPU), and those um_overlay writes into, are copied here
    uint64_t ready = 0;
    if (defer) ready = ubc_push(d, c ? c->cpu : (const uint8_t *)(uintptr_t)lo, (size_t)len);   // the previous shadow's own copies are queued before these
    else {
        if (c) ubc_wait(c->ready);
        memcpy(d, c ? c->cpu : (const uint8_t *)(uintptr_t)lo, (size_t)len);
    }
    if (!c) um_overlay(lo, hi, d, ready);   // a shadow made from the previous one has them already
    for (int i = nk - 1; i >= 0; i--) {   // oldest first: each byte ends with the newest write
        int e = g_ubp_idx[i];
        const upd_ent *u = &g_upd[g_pend_upd[e] - 1];
        uint64_t x = g_pend_lo[e] > lo ? g_pend_lo[e] : lo, y = g_pend_hi[e] < hi ? g_pend_hi[e] : hi;
        const uint8_t *src = (const uint8_t *)u->buf.contents + u->off + (x - u->lo);
        if (defer) ready = ubc_push(d + (x - lo), src, (size_t)(y - x));
        else memcpy(d + (x - lo), src, (size_t)(y - x));
    }
    g_ubp_made++;
    if (!c) c = &g_ubpc[g_ubpc_i++ % UBP_CACHE];
    *c = (ubp_ent){lo, hi, ab.gpuAddress + o, d, g_npend, g_pend_gen, g_sbw_gen, ready};
    *gpu = c->gpu; *cpu = d; g_ubp_ready = ready;
    return 1;
}
void mtl_ubp_stats(uint64_t *made, uint64_t *reused, uint64_t *refused) { *made = g_ubp_made; *reused = g_ubp_reused; *refused = g_ubp_refused; }
void mtl_ubp_why(uint64_t w[6]) { memcpy(w, g_ubp_why, sizeof g_ubp_why); }
void mtl_sub_update(const void *data, uint64_t dst, uint64_t n) {
    if (!n) return;
    rc_write(dst,n,data,0);
    int pi = pend_add(dst, n, 1);
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("uniform update target is in no memory pool dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    int defer = (enc_open() || (ubd_lazy() && !g_blit)) && g_nubd < UBD_MAX && ubd_on();
    size_t need = (n + 255u) & ~(size_t)255u;   // 256-aligned: a deferred update is read in place as a uniform buffer
    if (!g_ubs_buf || g_ubs_off + need > g_ubs_size) {
        if (g_ubs_buf) defer_obj_release(g_ubs_buf);   // copies already encoded still reference the old block
        g_ubs_size = MAX((size_t)1 << 20, need);
        g_ubs_buf = [g_dev newBufferWithLength:g_ubs_size options:MTLResourceStorageModeShared];
        if (!g_ubs_buf) MDIE("uniform update ring allocation failed %#zx", g_ubs_size);
        res_add(g_ubs_buf); g_ubs_off = 0;
    }
    memcpy((uint8_t *)g_ubs_buf.contents + g_ubs_off, data, n);
    if (pi >= 0) { g_upd[g_nupd] = (upd_ent){dst, g_ubs_off, g_ubs_buf}; g_pend_upd[pi] = (uint32_t)++g_nupd; }
    if (defer) {
        // A deferred update this one covers whole never needs to land: the draws that read it hold its ring copy.
        int k = 0;
        for (int i = 0; i < g_nubd; i++) if (!(dst <= g_ubd[i].lo && g_ubd[i].hi <= dst + n)) g_ubd[k++] = g_ubd[i];
        g_nubd = k;
        g_ubd[g_nubd++] = (ubd_ent){dst, dst + n, g_ubs_buf.gpuAddress + g_ubs_off, g_ubs_off, g_ubs_buf};
        ubd_pf_add(dst, dst + n);
        g_ubd_ver++; g_ubd_deferred++;
        ubd_dirty(dst, dst + n);
    }
    else enc_b_copy_buffer(blit_buf(0, 0, dst, dst + n), g_ubs_buf, g_ubs_off, (__bridge id<MTLBuffer>)db, dof, n);
    g_ubs_off += need;
}

void mtl_sub_fill(uint64_t dst, uint64_t n, uint32_t value) {
    if (!n) return;
    rc_write(dst,n,&value,4);
    pend_add(dst, n, 3); g_mdx_epoch++;
    void *db; uint64_t dof;
    if (!mtl_addr(dst, n, &db, &dof)) MDIE("buffer fill address is in no memory pool dst=%#llx n=%#llx", (unsigned long long)dst, (unsigned long long)n);
    uint8_t b0 = (uint8_t)value;
    if (value == (uint32_t)b0 * 0x01010101u) { [blit_buf(0, 0, dst, dst + n) fillBuffer:(__bridge id<MTLBuffer>)db range:NSMakeRange(dof, n) value:b0]; return; }
    enum { PAT = 64 << 10 };
    static __strong id<MTLBuffer> pat;
    static uint32_t pat_v;
    static int pat_ok;
    if (!pat_ok || pat_v != value) {
        uint32_t *tmp = malloc(PAT);
        for (unsigned i = 0; i < PAT / 4; i++) tmp[i] = value;
        pat = [g_dev newBufferWithBytes:tmp length:PAT options:MTLResourceStorageModeShared];
        free(tmp);
        pat_v = value; pat_ok = 1;
        res_add(pat);
    }
    for (uint64_t off = 0; off < n; off += PAT) {
        uint64_t k = n - off < PAT ? n - off : PAT;
        enc_b_copy_buffer(blit_buf(0, 0, dst + off, dst + off + k), pat, 0, (__bridge id<MTLBuffer>)db, dof + off, k);
    }
}

static void post(post_op o) {
    if (g_npost == g_postcap) {
        g_postcap = g_postcap ? g_postcap * 2 : 64;
        g_post = dm_realloc(g_post, g_postcap * sizeof *g_post, "the deferred operations");
    }
    g_post[g_npost++] = o;
}
// Taken at the first pass that counts samples (most submissions count none). The ring holds one per submission in flight, so one is always free.
static void vis_take(void) {
    if (g_vis_ring_i >= 0) return;
    int pick = -1;
    for (int i = 0; i < g_vis_ring_n; i++) if (g_vis_ring[i].fence <= g_done) { pick = i; break; }
    if (pick < 0 && g_vis_ring_n < VIS_RING) pick = g_vis_ring_n++;
    if (pick < 0) return;
    g_vis_ring_i = pick; g_vis_ring[pick].fence = UINT64_MAX;
    if (!g_vis_ring[pick].buf) { g_vis_ring[pick].buf = [g_dev newBufferWithLength:VIS_SLOTS * 8 options:MTLResourceStorageModeShared]; res_add(g_vis_ring[pick].buf); }
    g_vis = g_vis_ring[pick].buf; g_vis_n = 0;
}
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){1, val, mode, v, 0, 0, NULL}); }
static void ts_clk(void);
void mtl_sub_counter(uint64_t addr, uint32_t type) {
    sbw_note(addr, 16, SBW_REPORT);
    hw_note(addr);
    post((post_op){2, NULL, 0, 0, addr, type, NULL, g_seg});
    ts_clk();
}

// NVN2's raw timestamp unit is ns, from the guest clock (same epoch as GetSystemTick), not CLOCK_MONOTONIC, which runs on while the Mac sleeps.
uint64_t guest_ns(void);
static uint64_t gpu_ticks(void) { return nvn_timestamp_from_ns(guest_ns()); }
// A timestamp report takes the GPU time of the segment it was encoded in: TIMESTAMP_TOP (0x10) its GPU start, others its end. With one time per submission,
// a game timing its GPU work by two reports (agl's dynamic resolution) reads 0.
#define SEG_CLK_N 1024
// t: guest ns of each cut segment's GPU start / end (0 = no feedback yet); b: with ts_on, heap slots of the boundaries; cm: segments that ended a command buffer
struct seg_clk {
    _Atomic int refs;
    _Atomic uint64_t t[SEG_CLK_N][2];
    uint32_t nb, ncm; uint16_t b[SEG_CLK_N + 1], cm[SEG_CLK_N];
    uint64_t begin;
};
static int ts_mode(void) {   // 0 submission, 1 segment (the default), 2 segment with idle gaps kept, 3 segment with the waits before submissions kept
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("DM_NVN_GPU_TS");
        v = 1;
        if (e && *e) {
            if (!strcmp(e, "submission")) v = 0;
            else if (!strcmp(e, "segment_gaps")) v = 2;
            else if (!strcmp(e, "segment_submit_gaps")) v = 3;
            else if (strcmp(e, "segment")) MLOG("[RED] DM_NVN_GPU_TS=%s: not segment, segment_submit_gaps, segment_gaps or submission; segment stands", e);
        }
        MLOG("counter timestamps: %s (DM_NVN_GPU_TS=segment / segment_submit_gaps / segment_gaps / submission)", v == 0 ? "one time for the whole submission" : v == 1 ? "GPU start / end time of their segment, without the GPU's idle time the host's translation caused (between command buffers and between submissions)" : v == 3 ? "GPU start / end time of their segment, without the GPU's idle time between command buffers" : "GPU start / end time of their segment, the GPU's idle time between command buffers included");
    }
    return v;
}
static int seg_ts(void) { return ts_mode() > 0; }
static void seg_clk_put(seg_clk *c) { if (c && atomic_fetch_sub(&c->refs, 1) == 1) free(c); }
static uint64_t gpu_s_guest_ns(double s) {   // 0 when unknown
    uint64_t now = now_ns();
    uint64_t g = guest_ns_at_host_time(now);
    // The age is taken in integer ns: `(double)now - s * 1e9` contracts into a fused multiply-subtract that can land in the future for a time read in the same tick.
    if (!(s > 0) || !(s < 1e10)) return 0;   // NaN, infinity or negative; 1e10 s stays within the integer range
    uint64_t t = (uint64_t)llround(s * 1e9), slack = (uint64_t)((nextafter(s, INFINITY) - s) * 1e9) + 1;
    if (t > now + slack) return 0;
    uint64_t age = t < now ? now - t : 0;
    return age < g ? g - age : 0;
}
// Reports read where a Switch GPU would have run each command buffer: here the GPU also waits while the host translates, which agl's dynamic resolution would read as GPU time.
// Each report moves back by how much later the real GPU ran its command buffer, never before its submission was ready.
// A gap counts once the time is past its middle: a boundary's stamp is written before the commit or at the next start, never inside the gap.
static uint64_t seg_idle_before(seg_clk *sc, uint64_t ns, uint64_t c0) {
    uint64_t idle = 0;
    for (uint32_t i = 0; i < sc->ncm; i++) {
        uint64_t e = atomic_load(&sc->t[sc->cm[i]][1]), s = i + 1 < sc->ncm ? atomic_load(&sc->t[sc->cm[i + 1]][0]) : c0;
        if (e && s > e && ns >= e + (s - e) / 2) idle += s - e;
    }
    return idle;
}
static uint64_t place_back(seg_clk *sc, uint64_t ns, uint64_t c0, uint64_t off) {   // off: its submission's (sub_place)
    int m = ts_mode(); uint64_t back = off;
    if (sc && (m == 1 || m == 3)) back += seg_idle_before(sc, ns, c0);
    return back < ns ? back : 0;
}
// c0 / c1: the last segment's GPU start / end on the guest clock
static uint64_t rep_ticks(const post_op *o, seg_clk *sc, uint32_t last, uint64_t c0, uint64_t c1, uint64_t off) {
    int top = o->type == 0x10;
    uint64_t ns = 0;
    uint32_t bi = o->seg + !top;
    if (sc && bi < sc->nb && g_ts_heap) {   // reading the guest clock later would add CPU scheduling delay
        NSData *d = [g_ts_heap resolveCounterRange:NSMakeRange(sc->b[bi], 1)];
        const MTL4TimestampHeapEntry *e = d.bytes;
        MTLTimestamp cpu = 0, gpu = 0;
        [g_dev sampleTimestamps:&cpu gpuTimestamp:&gpu];
        static double scale;
        if (!scale) {
            uint64_t hz = [g_dev queryTimestampFrequency];
            scale = hz ? 1e9 / (double)hz : 1;
        }
        uint64_t g = cpu ? guest_ns_at_host_time(cpu) : 0;
        if (e && d.length >= sizeof *e && e->timestamp && g) {   // an entry counts at queryTimestampFrequency, a sample in ns
            double at = (double)e->timestamp * scale, ago = (double)gpu - at;
            if (ago >= 0 && ago < 2e9 && ago < (double)g) ns = g - (uint64_t)ago;
        }
        // A stamp from before the submission began is a slot it did not write (an encoder with no work writes no relaxed timestamp): its segment's feedback time stands instead.
        if (ns && sc->begin && ns + 500000 < sc->begin) ns = 0;
    }
    if (!ns && sc && o->seg < last && o->seg < SEG_CLK_N) ns = atomic_load(&sc->t[o->seg][!top]);
    if (!ns) ns = top ? c0 : c1;
    if (ns) ns -= place_back(sc, ns, c0, off);
    return ns ? nvn_timestamp_from_ns(ns) : gpu_ticks();
}
