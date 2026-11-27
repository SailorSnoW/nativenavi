// Dirty-page tracking of guest memory pools by write protection: NVN's GPU reads pool memory as it is now, and games write texture storage in use.
// The page-fault path uses only atomics and mprotect: no allocation, no locks.
#include "nvn_dirty.h"
#include "../../host/dm.h"
#include <stdatomic.h>
#include <pthread.h>
#include <sys/mman.h>
#include <unistd.h>
#include <mach/mach_time.h>
#include <mach/mach.h>
#include <sched.h>

// Page states: whoever changes protection sets BUSY, then the final state only after mprotect lands, so NONE is always writable and PROT always
// read-only (t_dirty_race). BUSY waits are capped at BUSY_WAIT_MS. OPEN: made writable by nvn_dirty_open, not counted as written.
enum { PG_NONE = 0, PG_PROT = 1, PG_BUSY = 2, PG_OPEN = 3 };
enum { BUSY_WAIT_MS = 2000 };
// One range per pool; blocks are never moved or freed, so the signal handler reads them without locks. A pool past MAX_RANGE is untracked (always dirty).
enum { BLK_SHIFT = 10, BLK = 1 << BLK_SHIFT, NBLK = 256, MAX_RANGE = BLK * NBLK };
// Ranges are rounded to 16 KB host pages but pools are 4 KB aligned: neighbours share pages (docs/reference/nvn-objects.md#dirty-page-ranges-of-pools-that-share-a-host-page).
// born: the clock at registration; an older stamp counts as written (docs/reference/nvn-objects.md#a-pool-registered-over-memory-an-earlier-pool-held-counts-older-stamps-as-written).
// pins: lock-free readers pin a slot before reading past base / end, which are atomic as they are read before the pin
typedef struct {
    _Atomic uintptr_t base, end;
    uintptr_t mem;
    _Atomic uint8_t *state;
    _Atomic uint64_t *gen;
    _Atomic(_Atomic uint8_t *) sparse;   // per page, protected for a placement sparse tile
    size_t cap;
    _Atomic int live, pins;
    _Atomic uint64_t born;
} range_t;
static inline uintptr_t rb(range_t *r) { return atomic_load_explicit(&r->base, memory_order_relaxed); }
static inline uintptr_t re(range_t *r) { return atomic_load_explicit(&r->end, memory_order_relaxed); }
static _Atomic(range_t *) g_blk[NBLK];
static inline range_t *rg(int i) { return &atomic_load_explicit(&g_blk[i >> BLK_SHIFT], memory_order_acquire)[i & (BLK - 1)]; }
static _Atomic int g_nr;
static uintptr_t g_pg, g_shift;
static _Atomic uint64_t g_now = 1;
static _Atomic uint64_t g_faults, g_preps;
static _Atomic int g_live, g_live_peak;
void nvn_dirty_stat(int *live, int *peak, int *cap) {
    *live = atomic_load(&g_live);
    *peak = atomic_load(&g_live_peak);
    *cap = MAX_RANGE;
}
static _Atomic int g_overflow_logged;
static pthread_mutex_t g_slot_mu = PTHREAD_MUTEX_INITIALIZER;   // serializes pool creation, retirement and protection; the fault path never takes it
// odd while descriptors change; it validates cached selections only, users still pin
static _Atomic uint64_t g_range_epoch;
static _Atomic uint64_t g_mp_calls, g_mp_pages, g_mp_ticks;   // mprotect calls, pages, mach ticks
static _Atomic uint64_t g_mp_begun, g_mp_done;   // on_fault tells from them whether its scan saw the protection as it is
static void mp(uintptr_t a, size_t len, int prot) {
    atomic_fetch_add(&g_mp_begun, 1);
    uint64_t t = mach_absolute_time();
    mprotect((void *)a, len, prot);
    atomic_fetch_add(&g_mp_done, 1);
    atomic_fetch_add(&g_mp_ticks, mach_absolute_time() - t);
    atomic_fetch_add(&g_mp_calls, 1);
    atomic_fetch_add(&g_mp_pages, len >> g_shift);
}

static mach_timebase_info_data_t g_tb;   // set at load; the fault path only multiplies and divides
static _Atomic int g_busy_red;
// 0 = waited past the limit (reported red once). Signal-safe: atomics, mach_absolute_time, sched_yield
static int busy_wait(range_t *r, uintptr_t p) {
    uint64_t t0 = 0;
    for (unsigned n = 1; atomic_load(&r->state[p]) == PG_BUSY; n++) {
        if (n & 0x3ff) continue;
        uint64_t now = mach_absolute_time();
        if (!t0) t0 = now;
        if ((now - t0) * g_tb.numer / g_tb.denom > (uint64_t)BUSY_WAIT_MS * 1000000ull) {
            if (!atomic_exchange(&g_busy_red, 1)) LOG("[RED] dirty-page tracking: page %p stuck 'busy' for over %d ms (the thread changing its protection has not finished), no longer waiting", (void *)(rb(r) + (p << g_shift)), BUSY_WAIT_MS);
            return 0;
        }
        sched_yield();
    }
    return 1;
}
static int live_at(int i) { return atomic_load_explicit(&rg(i)->live, memory_order_acquire); }
// docs/reference/nvn-objects.md#a-memory-pool-retired-while-another-thread-uses-its-tracking-slot
// seq_cst both sides: the pin counts before live is read, remove_range clears live before add_range reads the count. 0 = not live. Signal-safe
static int pin(range_t *r) {
    atomic_fetch_add(&r->pins, 1);
    if (atomic_load(&r->live)) return 1;
    atomic_fetch_sub_explicit(&r->pins, 1, memory_order_release);
    return 0;
}
static void unpin(range_t *r) { atomic_fetch_sub_explicit(&r->pins, 1, memory_order_release); }
static range_t *pin_holding(range_t *r, uintptr_t a, size_t len) {
    if (!r || !pin(r)) return NULL;
    if (a >= rb(r) && a + len <= re(r)) return r;
    unpin(r);
    return NULL;
}
typedef struct {
    range_t *range;
    uintptr_t base;
    uintptr_t end;
    // exclusive bounds that keep earlier slots from containing a hit: the first containing slot wins
    uintptr_t start_upper_bound;
    uintptr_t end_lower_bound;
    uint64_t epoch;
} cached_span_t;

// power of two: the insertion counter wraps
enum { SPAN_CACHE_SIZE = 8, SPAN_CACHE_MASK = SPAN_CACHE_SIZE - 1 };

// The first range containing the span; the cache holds descriptors, never generations or results
// (docs/reference/nvn-objects.md#cached-dirty-range-lookups-with-overlapping-pools)
static range_t *find_span(uintptr_t start, size_t length) {
    static _Thread_local cached_span_t recent_spans[SPAN_CACHE_SIZE];
    static _Thread_local unsigned next_cache_slot;
    uintptr_t end = start + length;
    uint64_t epoch = atomic_load_explicit(&g_range_epoch, memory_order_acquire);

    if (!(epoch & 1)) {
        for (unsigned offset = 0; offset < SPAN_CACHE_SIZE; offset++) {
            unsigned slot = (next_cache_slot - 1 - offset) & SPAN_CACHE_MASK;
            const cached_span_t *entry = &recent_spans[slot];
            if (!entry->range || entry->epoch != epoch) {
                continue;
            }
            if (start < entry->base || end > entry->end) {
                continue;
            }
            // every earlier slot must still fail to contain the span, or we could read the wrong generation
            if (start >= entry->start_upper_bound || end <= entry->end_lower_bound) {
                continue;
            }
            if (atomic_load_explicit(&g_range_epoch, memory_order_acquire) == epoch) {
                return entry->range;
            }
        }
    }

    uintptr_t start_upper_bound = UINTPTR_MAX;
    uintptr_t end_lower_bound = 0;
    int range_count = atomic_load(&g_nr);
    for (int slot = 0; slot < range_count; slot++) {
        range_t *range = rg(slot);
        if (!live_at(slot)) {
            continue;
        }
        uintptr_t base = rb(range);
        uintptr_t limit = re(range);
        if (start < base) {
            if (base < start_upper_bound) {
                start_upper_bound = base;
            }
            continue;
        }
        if (end > limit) {
            if (limit > end_lower_bound) {
                end_lower_bound = limit;
            }
            continue;
        }

        // only a selection made in one stable epoch
        if (!(epoch & 1) && atomic_load_explicit(&g_range_epoch, memory_order_acquire) == epoch) {
            recent_spans[next_cache_slot & SPAN_CACHE_MASK] = (cached_span_t){
                .range = range,
                .base = base,
                .end = limit,
                .start_upper_bound = start_upper_bound,
                .end_lower_bound = end_lower_bound,
                .epoch = epoch,
            };
            next_cache_slot++;
        }
        return range;
    }
    return NULL;
}
// for readers outside g_slot_mu: pinned (the caller unpins); NULL = untracked, or retired at every try
static range_t *pin_span(range_t *kept, uintptr_t a, size_t len) {
    range_t *r = pin_holding(kept, a, len);
    for (int tries = 0; !r && tries < 4; tries++) {
        range_t *f = find_span(a, len);
        if (!f) return NULL;
        r = pin_holding(f, a, len);
    }
    return r;
}
static void stamp_page(range_t *r, uintptr_t p) {
    atomic_store(&r->gen[p], atomic_fetch_add(&g_now, 1) + 1);
    // the clock moves again after the generation: a check that cached "clean" in between must miss
    atomic_fetch_add(&g_now, 1);
}
// The replaying thread's writes are ordered with its commands; any other thread's race them. Not those nvn_dirty_close finds.
static _Atomic(pthread_t) g_own;
static _Atomic uint64_t g_own_writes;
static void own_note(void) {
    pthread_t o = atomic_load_explicit(&g_own, memory_order_relaxed);
    if (o && pthread_equal(o, pthread_self())) atomic_fetch_add(&g_own_writes, 1);
}
void nvn_dirty_own_thread(void) { atomic_store(&g_own, pthread_self()); }
uint64_t nvn_dirty_own_writes(void) { return atomic_load(&g_own_writes); }
// replay_ahead (docs/reference/frame-rate.md#the-gpu-waits-at-a-frames-start-for-commands-the-game-queued-long-before) translates a parked frame before the game's
// signal and holds its commits: the copies it makes at translation read guest memory ahead of the signal, so a write another thread makes in between can be
// missed. While a hold is open (nvn_dirty_ahead_watch, set by hold_begin and cleared when the hold ends) every page a write unprotects is counted here; the
// replay thread's own writes are ordered with the commands it replays and are not counted. A page not re-protected since its last write is not seen.
static _Atomic int g_ahead_watch;
static _Atomic uint64_t g_ahead_pages, g_ahead_bytes, g_ahead_holds;
void nvn_dirty_ahead_watch(int on) {
    if (atomic_exchange(&g_ahead_watch, on) != on) atomic_fetch_add(&g_ahead_holds, on);
}
void nvn_dirty_ahead_stats(uint64_t *pages, uint64_t *bytes, uint64_t *holds) {
    if (pages) *pages = atomic_load(&g_ahead_pages);
    if (bytes) *bytes = atomic_load(&g_ahead_bytes);
    if (holds) *holds = atomic_load(&g_ahead_holds);
}
static void ahead_note(uint64_t npages) {
    if (!npages || !atomic_load_explicit(&g_ahead_watch, memory_order_relaxed)) return;
    pthread_t o = atomic_load_explicit(&g_own, memory_order_relaxed);
    if (o && pthread_equal(o, pthread_self())) return;
    atomic_fetch_add(&g_ahead_pages, npages);
    atomic_fetch_add(&g_ahead_bytes, npages * (uint64_t)g_pg);
}
static _Atomic uint64_t g_sparse_writes;
static void sparse_note(range_t *r, uintptr_t p) {
    _Atomic uint8_t *sp = atomic_load(&r->sparse);
    if (sp && atomic_exchange(&sp[p], 0)) atomic_fetch_add(&g_sparse_writes, 1);
}
static void unprotect_page(range_t *r, uintptr_t p) {   // the caller already set PROT -> BUSY
    mp(rb(r) + (p << g_shift), g_pg, PROT_READ | PROT_WRITE);
    stamp_page(r, p);
    sparse_note(r, p);
    own_note();
    ahead_note(1);
    atomic_store(&r->state[p], PG_NONE);
}
// 1 = taken (-> BUSY); 0 with the state found in *st
static int take_page(range_t *r, uintptr_t p, uint8_t *st) {
    *st = PG_PROT;
    if (atomic_compare_exchange_strong(&r->state[p], st, PG_BUSY)) return 1;
    return *st == PG_OPEN && atomic_compare_exchange_strong(&r->state[p], st, PG_BUSY);
}
// A page BUSY elsewhere is waited out and looked at again: a protection in progress ends PROT, and an unprotect-before-write never faults.
// 1 = it was this module's and is writable now
static int release_page(range_t *r, uintptr_t p, _Atomic uint64_t *count) {
    for (;;) {
        uint8_t st;
        if (take_page(r, p, &st)) {
            unprotect_page(r, p);
            if (count) atomic_fetch_add(count, 1);
            return 1;
        }
        if (st != PG_BUSY) return 0;   // NONE, or a read-only protection not ours (claiming it would loop forever)
        if (!busy_wait(r, p)) return 0;
    }
}
// one vm_region call, only for a fault no range claimed
static int page_writable(uintptr_t a) {
    vm_address_t pg = a & ~(g_pg - 1), va = pg;
    vm_size_t vs = 0;
    vm_region_basic_info_data_64_t bi;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    if (vm_region_64(mach_task_self(), &va, &vs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj) != KERN_SUCCESS || va > pg) return 0;
    return (bi.protection & (VM_PROT_WRITE | VM_PROT_EXECUTE)) == VM_PROT_WRITE;
}
// A fault no range claims is re-executed if the page is writable by now (a pool retired meanwhile), and the scan repeats if a protection change ran during it;
// only an unchanged scan hands it on as not ours (t_dirty_reuse)
static int on_fault(void *addr, int is_write, void *uc) {
    (void)uc;
    if (!is_write) return 0;
    uintptr_t a = (uintptr_t)addr;
    for (int tries = 0; tries < 1000; tries++) {
        uint64_t done = atomic_load(&g_mp_done);
        int ours = 0, n = atomic_load(&g_nr);
        for (int i = 0; i < n; i++) {   // every range sharing the page: one left PROT would miss later writes
            range_t *r = rg(i);
            if (!live_at(i) || a < rb(r) || a >= re(r) || !pin(r)) continue;
            if (a >= rb(r) && a < re(r)) ours |= release_page(r, (a - rb(r)) >> g_shift, &g_faults);
            unpin(r);
        }
        if (ours) return 1;
        if (page_writable(a)) return 1;
        if (atomic_load(&g_mp_begun) == done) return 0;   // none begun since the scan began, none in flight then: not ours
    }
    return 0;
}
// one mprotect per run; a page another thread is changing is taken after the run so far, so no BUSY page is held while waiting
static void release_pages(range_t *r, uintptr_t p, uintptr_t q, _Atomic uint64_t *count) {
    uintptr_t run = 0;
    int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && take_page(r, i, &st);
        if (took) {
            if (!open) {
                run = i;
                open = 1;
            }
            continue;
        }
        if (open) {
            mp(rb(r) + (run << g_shift), (i - run) << g_shift, PROT_READ | PROT_WRITE);
            for (uintptr_t k = run; k < i; k++) {
                stamp_page(r, k);
                sparse_note(r, k);
                atomic_store(&r->state[k], PG_NONE);
            }
            if (count) atomic_fetch_add(count, i - run);
            ahead_note(i - run);
            open = 0;
        }
        if (i <= q && st == PG_BUSY) release_page(r, i, count);
    }
}
static void prep_write(void *addr, size_t len) {
    uintptr_t a = (uintptr_t)addr, e = a + len;
    int n = atomic_load(&g_nr), in = 0;
    if (!len) return;
    for (int i = 0; i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || e <= rb(r) || a >= re(r) || !pin(r)) continue;
        uintptr_t base = rb(r), end = re(r);   // pinned: they hold until the unpin
        if (e > base && a < end) {
            uintptr_t lo = a > base ? a : base, hi = e < end ? e : end;
            in = 1;
            release_pages(r, (lo - base) >> g_shift, (hi - 1 - base) >> g_shift, &g_preps);
        }
        unpin(r);
    }
    if (in) own_note();   // whatever the pages' state: one another thread wrote first is writable already
}

// set before main, so every registration, fault and host pre-write sees them complete
__attribute__((constructor)) static void dirty_init(void) {
    g_pg = (uintptr_t)getpagesize();
    g_shift = __builtin_ctzl(g_pg);
    mach_timebase_info(&g_tb);
    dm_fault_hook_register(on_fault);
    dm_guest_write_prep_register(prep_write);
}

void nvn_dirty_add_range(const void *mem, size_t size) {
    pthread_mutex_lock(&g_slot_mu);
    int n = atomic_load(&g_nr);
    int slot = -1;
    for (int i = 0; i < n; i++) if (!live_at(i)) { slot = i; break; }
    if (slot < 0) {
        if (n >= MAX_RANGE) {
            int expected = 0;
            if (atomic_compare_exchange_strong(&g_overflow_logged, &expected, 1)) LOG("[RED] dirty-page tracking ranges are full (%d), pool %p not tracked", MAX_RANGE, mem);
            pthread_mutex_unlock(&g_slot_mu);
            return;
        }
        slot = n;
        if (!atomic_load_explicit(&g_blk[n >> BLK_SHIFT], memory_order_acquire)) {   // published before g_nr counts the slot (scans read g_nr first)
            range_t *b = calloc(BLK, sizeof *b);
            if (!b) die("dirty-page tracking range table allocation failed (block %d)", n >> BLK_SHIFT);
            atomic_store_explicit(&g_blk[n >> BLK_SHIFT], b, memory_order_release);
        }
    }
    atomic_fetch_add(&g_range_epoch, 1);
    int lv = atomic_fetch_add(&g_live, 1) + 1, pk = atomic_load(&g_live_peak);
    while (lv > pk && !atomic_compare_exchange_weak(&g_live_peak, &pk, lv)) {}
    range_t *r = rg(slot);
    while (atomic_load(&r->pins)) sched_yield();   // a pinned reader may be waiting out a BUSY page
    uintptr_t base = (uintptr_t)mem & ~(g_pg - 1), end = ((uintptr_t)mem + size + g_pg - 1) & ~(g_pg - 1);
    size_t np = (end - base) >> g_shift;
    _Atomic uint8_t *state = r->state;
    _Atomic uint64_t *gen = r->gen;
    if (r->cap < np) {
        free((void *)state);   // no reader holds them: none is pinned, and readers pin before reading the pointers
        free((void *)gen);
        free((void *)atomic_load(&r->sparse));
        state = calloc(np, 1);
        gen = calloc(np, sizeof *gen);
        if (!state || !gen) die("dirty-page table allocation failed (%zu pages)", np);
        atomic_store(&r->sparse, NULL);   // sized for the old cap
        r->cap = np;
    } else {
        memset((void *)state, 0, np);
        memset((void *)gen, 0, np * sizeof *gen);
        _Atomic uint8_t *sp = atomic_load(&r->sparse);
        if (sp) memset((void *)sp, 0, np);
    }
    atomic_store_explicit(&r->base, base, memory_order_relaxed);
    atomic_store_explicit(&r->end, end, memory_order_relaxed);
    r->mem = (uintptr_t)mem;
    r->state = state;
    r->gen = gen;
    atomic_store_explicit(&r->born, atomic_fetch_add(&g_now, 1) + 1, memory_order_relaxed);   // published by live's release store below
    atomic_fetch_add(&g_now, 1);
    if (slot == n) atomic_store(&g_nr, n + 1);
    atomic_store_explicit(&r->live, 1, memory_order_release);
    atomic_fetch_add(&g_range_epoch, 1);
    pthread_mutex_unlock(&g_slot_mu);
}
// pages back to writable first: the game may reuse the memory
void nvn_dirty_remove_range(const void *mem) {
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = NULL;
    int n = atomic_load(&g_nr);
    for (int i = 0; i < n && !r; i++) if (live_at(i) && rg(i)->mem == (uintptr_t)mem) r = rg(i);   // exact start: a neighbour may share the rounded base
    if (r) {
        atomic_fetch_add(&g_range_epoch, 1);
        prep_write((void *)rb(r), re(r) - rb(r));
        atomic_store(&r->live, 0);   // seq_cst: against pin
        atomic_fetch_sub(&g_live, 1);
        atomic_fetch_add(&g_range_epoch, 1);
    }
    pthread_mutex_unlock(&g_slot_mu);
}
// A zero-copy Metal buffer made while any of its pages is read-only silently drops every GPU write through it, so pages are opened around its creation
// (docs/reference/nvn-objects.md#gpu-writes-dropped-by-a-pool-buffer-made-over-write-protected-pages); the close counts as written only pages whose hash changed.
// No protection starts between open and close (g_slot_mu); the fault path is not held up.
typedef struct { range_t *r; uintptr_t p; uint64_t h; } open_pg;
static open_pg *g_open;   // by range then page; g_slot_mu held
static size_t g_nopen, g_copen;
static _Atomic uint64_t g_opened, g_open_changed;
static uint64_t page_hash(const void *page) {   // xxHash64's rounds over a page's four lanes
    const uint64_t P1 = 0x9E3779B185EBCA87ull, P2 = 0xC2B2AE3D27D4EB4Full, *w = page;
    uint64_t a = P1 + P2, b = P2, c = 0, d = -P1;
    for (size_t i = 0, n = g_pg / 8; i < n; i += 4) {
        a = __builtin_rotateleft64(a + w[i] * P2, 31) * P1;
        b = __builtin_rotateleft64(b + w[i + 1] * P2, 31) * P1;
        c = __builtin_rotateleft64(c + w[i + 2] * P2, 31) * P1;
        d = __builtin_rotateleft64(d + w[i + 3] * P2, 31) * P1;
    }
    uint64_t h = __builtin_rotateleft64(a, 1) + __builtin_rotateleft64(b, 7) + __builtin_rotateleft64(c, 12) + __builtin_rotateleft64(d, 18);
    h ^= h >> 33;
    h *= P2;
    h ^= h >> 29;
    h *= P1;
    return h ^ (h >> 32);
}
static void open_add(range_t *r, uintptr_t p, uint64_t h) {
    if (g_nopen == g_copen) {
        size_t cap = g_copen ? 2 * g_copen : 1024;
        open_pg *grown = realloc(g_open, cap * sizeof *g_open);
        if (!grown) die("dirty-page tracking: out of memory for %zu open pages", cap);
        g_open = grown;
        g_copen = cap;
    }
    g_open[g_nopen++] = (open_pg){r, p, h};
}
void nvn_dirty_open(const void *addr, size_t len) {
    pthread_mutex_lock(&g_slot_mu);
    g_nopen = 0;
    uintptr_t a = (uintptr_t)addr, e = a + len;
    int n = atomic_load(&g_nr);
    // Every range's pages are hashed and taken before any is opened, so each hash is of a read-only page (t_dirty_race open_shared_page).
    // Taken pages stay BUSY: a fault or host write waits for the open, holding no BUSY page of its own
    for (int k = 0; len && k < n; k++) {
        range_t *r = rg(k);
        if (!live_at(k) || e <= rb(r) || a >= re(r)) continue;
        uintptr_t lo = a > rb(r) ? a : rb(r), hi = e < re(r) ? e : re(r), p = (lo - rb(r)) >> g_shift, q = (hi - 1 - rb(r)) >> g_shift;
        for (uintptr_t i = p; i <= q; i++) {
            uint8_t st = atomic_load(&r->state[i]);
            if (st == PG_PROT) {
                uint64_t h = page_hash((const void *)(rb(r) + (i << g_shift)));
                if (atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY)) open_add(r, i, h);
            }
            if (st == PG_BUSY) busy_wait(r, i);   // another thread's unprotect finishes without g_slot_mu or another page
        }
    }
    for (size_t j = 0; j < g_nopen; ) {
        range_t *r = g_open[j].r;
        size_t s = j++;
        while (j < g_nopen && g_open[j].r == r && g_open[j].p == g_open[s].p + (j - s)) j++;
        mp(rb(r) + (g_open[s].p << g_shift), (j - s) << g_shift, PROT_READ | PROT_WRITE);
        for (size_t k = s; k < j; k++) atomic_store(&r->state[g_open[k].p], PG_OPEN);
    }
    atomic_fetch_add(&g_opened, g_nopen);
}
void nvn_dirty_close(void) {
    size_t changed = 0;
    for (size_t j = 0; j < g_nopen; ) {
        // A run: one range's consecutive pages still open; a page a host write took meanwhile ends it and is left as it is
        range_t *r = g_open[j].r;
        size_t s = j;
        for (uint8_t st = PG_OPEN; j < g_nopen && g_open[j].r == r && g_open[j].p == g_open[s].p + (j - s) && atomic_compare_exchange_strong(&r->state[g_open[j].p], &st, PG_BUSY); st = PG_OPEN) j++;
        if (j == s) {
            j++;
            continue;
        }
        mp(rb(r) + (g_open[s].p << g_shift), (j - s) << g_shift, PROT_READ);   // protected again first, so the bytes stay as hashed below
        for (size_t k = s; k < j; k++) atomic_store(&r->state[g_open[k].p], PG_PROT);
        for (size_t k = s; k < j; k++)
            if (page_hash((const void *)(rb(r) + (g_open[k].p << g_shift))) != g_open[k].h) {
                stamp_page(r, g_open[k].p);
                sparse_note(r, g_open[k].p);
                ahead_note(1);
                changed++;
            }
    }
    atomic_fetch_add(&g_open_changed, changed);
    g_nopen = 0;
    pthread_mutex_unlock(&g_slot_mu);
}
void nvn_dirty_open_stats(uint64_t *opened, uint64_t *changed) {
    *opened = atomic_load(&g_opened);
    *changed = atomic_load(&g_open_changed);
}
int nvn_dirty_tracked(const void *addr, size_t len) {
    return find_span((uintptr_t)addr, len) != NULL;
}
// a handle the caller keeps; NULL = untracked
const void *nvn_dirty_span(const void *addr, size_t len) { return len ? find_span((uintptr_t)addr, len) : NULL; }
void nvn_dirty_protect(const void *addr, size_t len) {
    // held through the last mprotect: retirement restores RW under it (docs/reference/nvn-objects.md#a-retired-pool-remains-read-only)
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (!r) {
        pthread_mutex_unlock(&g_slot_mu);
        return;
    }
    uintptr_t a = (uintptr_t)addr, p = (a - rb(r)) >> g_shift, q = (a + len - 1 - rb(r)) >> g_shift;
    // Runs of pages merged into one mprotect; PROT / BUSY pages break a run. A BUSY page is waited out (the run so far protected first) and looked at again:
    // skipped, an unprotect in progress would leave it writable under the caller's stamp, its later writes never faulting
    uintptr_t run = 0;
    int open = 0;
    for (uintptr_t i = p; i <= q + 1; i++) {
        uint8_t st = PG_NONE;
        int took = i <= q && atomic_compare_exchange_strong(&r->state[i], &st, PG_BUSY);
        if (took && !open) {
            run = i;
            open = 1;
        }
        if (!took && open) {
            mp(rb(r) + (run << g_shift), (i - run) << g_shift, PROT_READ);
            for (uintptr_t k = run; k < i; k++) atomic_store(&r->state[k], PG_PROT);
            open = 0;
        }
        if (!took && i <= q && st == PG_BUSY && busy_wait(r, i)) i--;
    }
    pthread_mutex_unlock(&g_slot_mu);
}
// For a placement sparse tile's pages: a write moves nvn_dirty_sparse_writes, so its textures need no tile look until one is written
void nvn_dirty_protect_sparse(const void *addr, size_t len) {
    nvn_dirty_protect(addr, len);
    pthread_mutex_lock(&g_slot_mu);
    range_t *r = len ? find_span((uintptr_t)addr, len) : NULL;
    if (r) {
        _Atomic uint8_t *sp = atomic_load(&r->sparse);
        if (!sp) {
            sp = calloc(r->cap, 1);
            atomic_store(&r->sparse, sp);
        }
        if (sp) for (uintptr_t i = ((uintptr_t)addr - rb(r)) >> g_shift, q = ((uintptr_t)addr + len - 1 - rb(r)) >> g_shift; i <= q; i++) atomic_store(&sp[i], 1);
    }
    pthread_mutex_unlock(&g_slot_mu);
}
uint64_t nvn_dirty_sparse_writes(void) { return atomic_load(&g_sparse_writes); }
uint64_t nvn_dirty_now(void) { return atomic_load(&g_now); }
// A GPU blit does not fault on CPU protection: the protection stays for the next CPU write
void nvn_dirty_gpu_written(uintptr_t addr, size_t len) {
    if (!len) return;
    pthread_mutex_lock(&g_slot_mu);
    for (int i = 0, n = atomic_load(&g_nr); i < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i)) continue;
        uintptr_t base = rb(r), end = re(r);
        uintptr_t lo = addr > base ? addr : base, hi = addr + len < end ? addr + len : end;
        if (hi <= lo) continue;
        for (uintptr_t p = (lo - base) >> g_shift; p <= (hi - 1 - base) >> g_shift; p++) {
            // a CPU writer that took a protected page publishes its own stamp: serialize so the generation cannot move back
            uint8_t state = PG_PROT;
            if (atomic_compare_exchange_strong(&r->state[p], &state, PG_BUSY)) {
                stamp_page(r, p);
                sparse_note(r, p);
                atomic_store(&r->state[p], PG_PROT);
            }
        }
    }
    pthread_mutex_unlock(&g_slot_mu);
    own_note();   // ordered with this submission's texture reads, unlike a concurrent CPU writer
}
void nvn_dirty_touch(void) { atomic_fetch_add(&g_now, 1); }
// As nvn_dirty_since, keeping the caller's last range in *span (scanned again only if it no longer holds the span)
int nvn_dirty_since_cached(const void **span, const void *addr, size_t len, uint64_t stamp) {
    if (!len) return 0;
    uintptr_t a = (uintptr_t)addr;
    range_t *r = pin_span((range_t *)*span, a, len);
    *span = r;
    if (!r) return 1;   // untracked: dirty
    int w = stamp < atomic_load_explicit(&r->born, memory_order_relaxed);   // a stamp from before this pool
    uintptr_t p = (a - rb(r)) >> g_shift, q = (a + len - 1 - rb(r)) >> g_shift;
    for (uintptr_t i = p; i <= q && !w; i++) w = atomic_load(&r->gen[i]) > stamp;
    unpin(r);
    return w;
}
int nvn_dirty_since_span(const void *span, const void *addr, size_t len, uint64_t stamp) { return nvn_dirty_since_cached(&span, addr, len, stamp); }
int nvn_dirty_since(const void *addr, size_t len, uint64_t stamp) { return nvn_dirty_since_span(NULL, addr, len, stamp); }
// Pages holding a byte of an excepted range do not count: the caller accounts for those writes itself
size_t nvn_dirty_since_except(const void *span, const void *addr, size_t len, uint64_t stamp, const uintptr_t (*ex)[2], int nex, uintptr_t *first) {
    if (!len) return 0;
    uintptr_t a = (uintptr_t)addr;
    range_t *r = pin_span((range_t *)span, a, len);
    if (!r) return SIZE_MAX;
    uintptr_t p = (a - rb(r)) >> g_shift, q = (a + len - 1 - rb(r)) >> g_shift;
    size_t n = 0;
    int before = stamp < atomic_load_explicit(&r->born, memory_order_relaxed);   // every page counts (born)
    for (uintptr_t i = p; i <= q; i++) {
        if (!before && atomic_load(&r->gen[i]) <= stamp) continue;
        uintptr_t lo = rb(r) + (i << g_shift), hi = lo + g_pg;
        int mine = 0;
        for (int k = 0; k < nex && !mine; k++) mine = ex[k][0] < hi && ex[k][1] > lo;
        if (mine) continue;
        if (!n++ && first) *first = lo < a ? a : lo;
    }
    unpin(r);
    return n;
}
// Held PROT but writable: lost behind the tracker; read-only with no newer generation: a non-faulting writer (GPU, another mapping)
void nvn_dirty_describe(const void *addr, const void *span, char *out, size_t n) {
    static const char *const st[] = {"none", "prot", "busy", "open"};
    uintptr_t a = (uintptr_t)addr;
    int k = atomic_load(&g_nr), found = 0;
    size_t o = 0;
    if (!n) return;
    out[0] = 0;
    for (int i = 0; i < k && o < n; i++) {
        range_t *r = rg(i);
        if (!live_at(i) || a < rb(r) || a >= re(r) || !pin(r)) continue;
        if (a < rb(r) || a >= re(r)) {
            unpin(r);
            continue;
        }
        uintptr_t p = (a - rb(r)) >> g_shift;
        uint8_t s = atomic_load(&r->state[p]);
        int w = snprintf(out + o, n - o, "%sslot %d pool %#lx%s %s gen %llu born %llu", found++ ? "; " : "", i, (unsigned long)r->mem, (const void *)r == span ? " (kept)" : "",
                         s < 4 ? st[s] : "?", (unsigned long long)atomic_load(&r->gen[p]), (unsigned long long)atomic_load(&r->born));
        unpin(r);
        if (w > 0) o += (size_t)w;
    }
    if (!found && o < n) {
        int w = snprintf(out + o, n - o, "no live range");
        if (w > 0) o += (size_t)w;
    }
    vm_address_t va = a & ~(g_pg - 1);
    vm_size_t vs = 0;
    vm_region_basic_info_data_64_t bi;
    mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64;
    mach_port_t obj = MACH_PORT_NULL;
    kern_return_t kr = vm_region_64(mach_task_self(), &va, &vs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&bi, &cnt, &obj);
    if (o < n) {
        if (kr != KERN_SUCCESS || va > (a & ~(g_pg - 1))) snprintf(out + o, n - o, "; kernel: not mapped");
        else snprintf(out + o, n - o, "; kernel: %s%s%s%s", bi.protection & VM_PROT_READ ? "r" : "-", bi.protection & VM_PROT_WRITE ? "w" : "-", bi.shared ? " shared" : "",
                      bi.reserved ? " reserved" : "");
    }
}
void nvn_dirty_stats(uint64_t *faults, uint64_t *preps) {
    *faults = atomic_load(&g_faults);
    *preps = atomic_load(&g_preps);
}
void nvn_dirty_mp_stats(uint64_t *calls, uint64_t *pages, uint64_t *ns) {
    static mach_timebase_info_data_t tb;
    if (!tb.denom) mach_timebase_info(&tb);
    *calls = atomic_load(&g_mp_calls);
    *pages = atomic_load(&g_mp_pages);
    *ns = atomic_load(&g_mp_ticks) * tb.numer / tb.denom;
}
