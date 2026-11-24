// Counter reports, occlusion queries, conditional rendering, and the writes queued in a submission (pend, sbw) that they and the uniform shadows look up.
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; void *vis; uint32_t seg; uint64_t copy_end; } post_op;   // vis: visibility slot table for kind 3; seg: the segment it was encoded in
static uint32_t g_seg;   // 0 until the first sub_cut
static uint64_t g_cbgen;
static int gsb_held(const void *mem, uint64_t size);
typedef struct seg_clk seg_clk;
static seg_clk *g_sclk;
static void seg_clk_put(seg_clk *c);
// Conditional rendering: Metal has no predication, so the condition is evaluated on the CPU at replay. A timestamp report writes 0 in the counter field (the time goes in the
// second field); a samples-passed result is unknown until its submission completes, and an unknown condition keeps drawing.
static int g_rc_skip;   // a render-enable block evaluated false is active
// The latest command-ordered writer of each report counter; completed reports are read from memory, so CPU rewrites stay visible.
typedef struct rc_report {
    uint64_t addr, fence, value, report_fence, gpu_epoch;
    uint8_t known, dirty, readable, reported, report_dirty, unreliable, gpu_domain, opaque_unreliable, opaque_later, conservative;
    struct rc_report *page_next, *dirty_next;
} rc_report;
static nvn_hmap g_rc_reports={.klen=8}, g_rc_pages={.klen=8};
static uint64_t *g_rc_page_order;
static uint32_t g_rc_page_n, g_rc_page_cap;
static uint32_t rc_page_lower(uint64_t page) {
    uint32_t lo=0, hi=g_rc_page_n;
    while (lo<hi) { uint32_t mid=lo+(hi-lo)/2; if (g_rc_page_order[mid]<page) lo=mid+1; else hi=mid; }
    return lo;
}
static void rc_page_add(uint64_t page) {
    uint32_t at=rc_page_lower(page);
    if (g_rc_page_n==g_rc_page_cap) {
        uint32_t cap=g_rc_page_cap ? g_rc_page_cap*2 : 64;
        uint64_t *p=realloc(g_rc_page_order,(size_t)cap*sizeof *p);
        if (!p) MDIE("no memory for the report page index");
        g_rc_page_order=p; g_rc_page_cap=cap;
    }
    memmove(g_rc_page_order+at+1,g_rc_page_order+at,(g_rc_page_n-at)*sizeof *g_rc_page_order);
    g_rc_page_order[at]=page; g_rc_page_n++;
}
static rc_report *g_rc_dirty;
static uint64_t g_rc_gpu_epoch, g_rc_gpu_fence;
static int g_rc_gpu_dirty;
// Published before an event can wake a CPU writer: g_done comes later and would briefly hide such a writer.
static _Atomic uint64_t g_rc_complete;
static rc_report *rc_find(uint64_t addr) { return nvn_hm_get(&g_rc_reports,&addr); }
static void rc_touch(rc_report *r) {
    r->fence=0; r->gpu_epoch=g_rc_gpu_epoch;
    if (!r->dirty) { r->dirty=1; r->dirty_next=g_rc_dirty; g_rc_dirty=r; }
}
static void rc_commit(uint64_t fence) {
    for (rc_report *r=g_rc_dirty,*next;r;r=next) {
        next=r->dirty_next; r->fence=fence; r->dirty=0; r->dirty_next=NULL;
        if (r->report_dirty) { r->report_fence=fence; r->report_dirty=0; }
    }
    g_rc_dirty=NULL;
    if (g_rc_gpu_dirty) { g_rc_gpu_fence=fence; g_rc_gpu_dirty=0; }
}
static void rc_forget(void) {
    g_rc_dirty=NULL; g_rc_gpu_epoch=g_rc_gpu_fence=0; g_rc_gpu_dirty=0;
    nvn_hm_clear(&g_rc_pages,NULL); nvn_hm_clear(&g_rc_reports,free);
    free(g_rc_page_order); g_rc_page_order=NULL; g_rc_page_n=g_rc_page_cap=0;
}
static rc_report *rc_ensure(uint64_t addr) {
    rc_report *r=rc_find(addr);
    if (!r) {
        r=calloc(1,sizeof *r);
        if (!r) MDIE("no memory for the report table");
        r->addr=addr; r->readable=1; r->gpu_epoch=g_rc_gpu_epoch;
        nvn_hm_put(&g_rc_reports,&addr,r);
        uint64_t page=addr>>12; r->page_next=nvn_hm_get(&g_rc_pages,&page);
        if (!r->page_next) rc_page_add(page);
        nvn_hm_put(&g_rc_pages,&page,r);
    }
    return r;
}
static void rc_query_operand(rc_report *r) {
    if (r->gpu_domain) return;
    r->gpu_domain=1;
    // an earlier opaque writer may have raced this timestamp: its CPU-post bytes are not exposed
    if (r->opaque_later) { r->known=0; r->opaque_later=0; }
    if (r->opaque_unreliable) r->unreliable=1;
}
static void rc_note(uint64_t addr, int kind) {
    rc_report *r=rc_ensure(addr);
    r->conservative=kind==3;
    // A condition compares adjacent 16-byte records; either operand can be the query.
    if (kind==2) {
        rc_query_operand(r);
        if (addr>=16) rc_query_operand(rc_ensure(addr-16));
        rc_query_operand(rc_ensure(addr+16));
    }
    // only timestamp and samples-passed reports have implemented counter values
    r->value=0; r->known=kind==1 ? 255 : 0; r->readable=kind!=3;
    r->reported=r->report_dirty=1; r->report_fence=0; r->unreliable=r->opaque_unreliable=r->opaque_later=0;
    rc_touch(r);
}
static int rc_gpu_unknown(rc_report *r, uint64_t done) {
    if (!r->gpu_domain || r->gpu_epoch==g_rc_gpu_epoch) return 0;
    // An unbounded shader writer may race a CPU-landed query.
    if (r->reported || g_rc_gpu_dirty || g_rc_gpu_fence>done) return 1;
    r->gpu_epoch=g_rc_gpu_epoch;
    return 0;
}
static uint8_t rc_source(uint64_t a, unsigned n, uint8_t *out, int *unreliable);
static int rc_value(uint64_t addr, uint64_t *v) {   // 0 = unknown at encode time
    rc_report *r=rc_find(addr);
    uint64_t done=atomic_load_explicit(&g_rc_complete,memory_order_acquire);
    // Replay ahead reaches a condition before the GPU ran its report, and an unknown condition draws its block: wait here for the game's signal.
    if (r && !r->conservative && r->fence>done && atomic_load(&g_hold_on) && on_sub_thread()) {
        hold_block(1);
        g_rc_ahead_waits++;
        done=atomic_load_explicit(&g_rc_complete,memory_order_acquire);
    }
    if (r) {
        if (r->conservative) return 0;
        if (rc_gpu_unknown(r,done)) return 0;
        if (r->dirty || r->fence>done) { *v=r->value; return r->known==255; }
        if (!r->readable || r->unreliable) return 0;
        *v=*(const volatile uint64_t *)addr;
        return 1;
    }
    // No report of its own: memory holds the operand only when nothing queued before the condition writes it later (a shader may write the visibility the condition compares).
    uint8_t b[8]; int unreliable=0;
    if (rc_source(addr,8,b,&unreliable)!=0xff || unreliable) return 0;
    memcpy(v,b,8);
    return 1;
}
// the previous seven bytes are included, so an unaligned counter crossing a page is covered
static rc_report *rc_containing(uint64_t addr) {
    uint64_t first=(addr>7 ? addr-7 : 0)>>12, last=addr>>12;
    for (uint64_t page=first;page<=last;page++)
        for (rc_report *r=nvn_hm_get(&g_rc_pages,&page);r;r=r->page_next)
            if (addr>=r->addr && addr-r->addr<8) return r;
    return NULL;
}
static void rc_overwrite(rc_report *r, uint64_t dst, uint64_t n, const void *data, unsigned period, uint64_t src, uint64_t done, int opaque) {
    uint64_t lo=MAX(dst,r->addr), hi=MIN(dst+n,r->addr+8);
    if (hi<=lo) return;
    if (opaque && !r->gpu_domain) {
        if (r->reported) r->opaque_unreliable=r->opaque_later=1;
        return;
    }
    int report_pending=r->reported && (r->report_dirty || r->report_fence>done);
    if (r->opaque_later) { r->known=0; r->opaque_later=0; }
    if (r->opaque_unreliable) r->unreliable=1;
    if ((r->reported && r->gpu_epoch!=g_rc_gpu_epoch) || rc_gpu_unknown(r,done)) {
        r->known=0;
        if (r->reported) r->unreliable=1;
    }
    if (opaque) {
        if (report_pending) r->unreliable=1;
        r->known=0;
        rc_touch(r);
        return;
    }
    // the preceding writer has landed: unaffected bytes keep the current CPU value
    if (!r->dirty && r->fence<=done) {
        if (r->readable && !r->unreliable && !rc_gpu_unknown(r,done)) { r->value=*(const volatile uint64_t *)r->addr; r->known=255; }
        else r->known=0;
    }
    int unreliable=report_pending;
    unsigned at=(unsigned)(lo-r->addr), bytes=(unsigned)(hi-lo);
    if (data) {
        for (unsigned b=0;b<bytes;b++) {
            uint64_t off=lo+b-dst;
            ((uint8_t *)&r->value)[at+b]=((const uint8_t *)data)[period ? off%period : off];
            r->known|=(uint8_t)(1u<<(at+b));
        }
    } else {
        uint8_t value[8], known=rc_source(src+lo-dst,bytes,value,&unreliable), mask=(uint8_t)(((1u<<bytes)-1)<<at);
        for (unsigned b=0;b<bytes;b++) if (known>>b&1) ((uint8_t *)&r->value)[at+b]=value[b];
        r->known=(r->known&~mask)|(known<<at);
    }
    if (r->known==255) r->readable=1;
    // Query reports land on the CPU after GPU execution, so a later GPU overwrite or copy can leave unreliable final bytes.
    if (unreliable) r->unreliable=1;
    else if (at==0 && bytes==8) r->unreliable=r->opaque_unreliable=r->opaque_later=0;
    rc_touch(r);
}
// A later explicit write replaces the report's command-ordered knowledge until it completes; shader-produced predicates stay unsupported.
static void rc_change(uint64_t dst, uint64_t n, const void *data, unsigned period, uint64_t src, int opaque) {
    if (!g_rc_reports.n || !n) return;
    uint64_t done=atomic_load_explicit(&g_rc_complete,memory_order_acquire);
    uint64_t first=(dst>7 ? dst-7 : 0)>>12, last=(dst+n-1)>>12;
    if (last-first>=8) {
        for (uint32_t i=rc_page_lower(first);i<g_rc_page_n && g_rc_page_order[i]<=last;i++)
            for (rc_report *r=nvn_hm_get(&g_rc_pages,&g_rc_page_order[i]);r;r=r->page_next) rc_overwrite(r,dst,n,data,period,src,done,opaque);
    } else {
        for (uint64_t page=first;page<=last;page++)
            for (rc_report *r=nvn_hm_get(&g_rc_pages,&page);r;r=r->page_next) rc_overwrite(r,dst,n,data,period,src,done,opaque);
    }
}
static void rc_write(uint64_t dst, uint64_t n, const void *data, unsigned period) { rc_change(dst,n,data,period,0,0); }
static void rc_copy(uint64_t src, uint64_t dst, uint64_t n) { if (src!=dst) rc_change(dst,n,NULL,0,src,0); }
static void rc_gpu_write(uint64_t dst, uint64_t n) {
    if (!n) { g_rc_gpu_epoch++; g_rc_gpu_dirty=1; }
    else rc_change(dst,n,NULL,0,0,1);
}
// Occlusion queries: NVN's samples passed is one cumulative counter on the queue; Metal counts into a visibility buffer slot per pass. The running total (g_vis_acc) is kept by
// post_run in submission order under g_post_mu; a slot is read once, while its buffer is still this submission's. With no reset, counting starts at the first report.
typedef struct { void *buf; uint32_t slot; } vis_slot;
typedef struct { int n; vis_slot s[]; } vis_list;
#define VIS_SLOTS 8192
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence; } vis_buf;
// One visibility buffer per submission in flight, kept resident and reused once its fence passes: a one-off buffer costs two residency commits.
enum { VIS_RING = SUB_FLIGHT_MAX };
static vis_buf g_vis_ring[VIS_RING];
static int g_vis_ring_n, g_vis_ring_i = -1;
static id<MTLBuffer> g_vis;
static uint32_t g_vis_n;
static void vis_take(void);
// g_vis is either the ring buffer or an overflow buffer of its own, retired after it
static int vis_in_ring(id<MTLBuffer> b) { return g_vis_ring_i >= 0 && g_vis_ring[g_vis_ring_i].buf == b; }
static int g_vis_on, g_vis_counting, g_vis_cur = -1;
static uint64_t g_vis_cur_pass;
// 0 after a samples-passed reset or report whose next samples-passed op in this submission is a reset
static int g_vis_need = 1;
static uint64_t g_vis_unseen;
static vis_slot *g_vs;   // slots since the last report or reset, in this submission
static int g_nvs, g_vscap;
enum { VIS_REPORT, VIS_FOLD, VIS_ZERO };   // kind 3 modes: add the slots and write the total / only add them / zero the total
static uint64_t g_vis_acc;   // under g_post_mu
static void vis_clear(void) {
    for (int i = 0; i < g_nvs; i++) CFRelease(g_vs[i].buf);
    g_nvs = 0;
    g_vis_cur = -1;
}
static vis_list *vis_snapshot(void) {
    vis_list *l = malloc(sizeof *l + g_nvs * sizeof(vis_slot)); l->n = g_nvs;
    for (int i = 0; i < g_nvs; i++) {
        l->s[i] = g_vs[i];
        CFRetain(g_vs[i].buf);
    }
    return l;
}
// Writes encoded in this submission but not yet run: the CPU reads old values there at encode time. Indexed by granule (PEND_LV sizes from 4 KB, each 8x the last).
// A full list makes the submission a write anywhere (g_pend_over), which refuses every partly covered uniform shadow of later submissions while it runs.
enum { PEND_HB_BITS = 13, PEND_HB = 1 << PEND_HB_BITS, PEND_SPAN = 8, PEND_LV = 16 };
static inline unsigned pend_shift(int lv) { return 12 + 3 * (unsigned)lv; }   // 4 KB << 3 lv
static uint32_t g_pend_lv;   // bit lv
static uint64_t *g_pend_lo, *g_pend_hi;
static int g_npend, g_pend_over, g_pend_cap;
static uint8_t *g_pend_kind;   // 1 inline update, 2 copy, 3 fill
static uint32_t *g_pend_upd;
static int32_t g_pend_head[PEND_HB];
static uint32_t g_pend_hgen[PEND_HB], g_pend_gen = 1;   // a bucket's chain is valid when its generation is this submission's
typedef struct { int32_t e, next; } pend_node;
static pend_node *g_pend_node;
static int g_npnode, g_pnode_cap;
typedef struct { uint64_t lo; size_t off; __unsafe_unretained id<MTLBuffer> buf; } upd_ent;   // the ring block lives until this submission completes
static upd_ent *g_upd;
static int g_nupd;
static int *g_ubp_idx;
static int pend_grow(void **p, size_t n, size_t each) {
    void *q = realloc(*p, n * each);
    if (!q) return 0;
    *p = q;
    return 1;
}
// 0 when memory runs out (the caller counts the write as one anywhere)
static int pend_room(int nodes) {
    if (g_npend == g_pend_cap) {
        if (g_pend_cap > INT_MAX / 2) return 0;
        int c = g_pend_cap ? g_pend_cap * 2 : 4096;
        if (!pend_grow((void **)&g_pend_lo, c, sizeof *g_pend_lo) || !pend_grow((void **)&g_pend_hi, c, sizeof *g_pend_hi) || !pend_grow((void **)&g_pend_kind, c, sizeof *g_pend_kind)
            || !pend_grow((void **)&g_pend_upd, c, sizeof *g_pend_upd) || !pend_grow((void **)&g_upd, c, sizeof *g_upd)) return 0;
        g_pend_cap = c;
    }
    if (!g_pnode_cap || g_npnode + nodes > g_pnode_cap) {
        int c = g_pnode_cap ? g_pnode_cap : 8192;
        while (c < g_npnode + nodes) {
            if (c > INT_MAX / 2) return 0;
            c *= 2;
        }
        if (!pend_grow((void **)&g_pend_node, c, sizeof *g_pend_node) || !pend_grow((void **)&g_ubp_idx, c, sizeof *g_ubp_idx)) return 0;
        g_pnode_cap = c;
    }
    return 1;
}
// Other guest memory writes of this submission, by 4 KB page. A counter report does not count against this submission's shadows (written on the CPU after it completes).
// A GPU write over more than SBW_WPAGES pages is kept as an interval.
enum { SBW_INIT = 16384, SBW_WPAGES = 64, SBW_WN = 256 };
typedef struct { uint64_t page; uint32_t gen; uint8_t src; } sbw_ent;
static sbw_ent *g_sbwp;   // src: SBW_* bits
static uint32_t *g_sbw_list;
static uint32_t g_sbw_cap, g_sbw_shift, g_nsbw;
static int g_sbw_over;   // a write could not be kept (out of memory, size not known, wide list full)
static uint64_t g_sbw_wlo[SBW_WN], g_sbw_whi[SBW_WN];
static int g_nsbw_w;
static int sbw_grow(void) {
    if (g_sbw_cap >= (1u << 24)) return 0;
    uint32_t ncap = g_sbw_cap ? g_sbw_cap * 2 : SBW_INIT;
    uint32_t nshift = g_sbw_cap ? g_sbw_shift - 1 : (uint32_t)(64 - __builtin_ctz(SBW_INIT));
    sbw_ent *np = calloc(ncap, sizeof *np);
    uint32_t *nl = malloc((size_t)ncap * sizeof *nl);
    if (!np || !nl) { free(np); free(nl); return 0; }
    for (uint32_t i = 0; g_sbwp && i < g_nsbw; i++) {
        sbw_ent e = g_sbwp[g_sbw_list[i]];
        unsigned h = (unsigned)((e.page * 0x9E3779B97F4A7C15ull) >> nshift) & (ncap - 1);
        while (np[h].gen == g_pend_gen) h = (h + 1) & (ncap - 1);
        np[h] = e; nl[i] = h;
    }
    free(g_sbwp); free(g_sbw_list);
    g_sbwp = np; g_sbw_list = nl; g_sbw_cap = ncap; g_sbw_shift = nshift;
    return 1;
}
static int sbw_slot(uint64_t pg, int add) {   // open addressing (load <= 50%); -1 = not there (or no room to add)
    if (!add && (!g_nsbw || !g_sbwp)) return -1;
    if (add && (!g_sbwp || g_nsbw >= g_sbw_cap / 2)) { if (!sbw_grow()) return -1; }
    unsigned h = (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> g_sbw_shift) & (g_sbw_cap - 1);
    for (;; h = (h + 1) & (g_sbw_cap - 1)) {
        if (g_sbwp[h].gen != g_pend_gen) {
            if (!add) return -1;
            g_sbwp[h].page = pg; g_sbwp[h].gen = g_pend_gen; g_sbwp[h].src = 0;
            g_sbw_list[g_nsbw++] = h;
            return (int)h;
        }
        if (g_sbwp[h].page == pg) return (int)h;
    }
}
enum { SBW_GPU = 1, SBW_REPORT = 2 };
static uint64_t *g_hw_sub;
static int g_nhw_sub, g_hw_cap;
static uint32_t g_sbw_gen;   // bumped by every write this submission's shadows must respect
static void drw_note(uint64_t lo, uint64_t n);
static void vk_gpu_write(uint64_t dst, uint64_t n);   // n = 0: a write anywhere
static void sbw_note(uint64_t lo, uint64_t n, int src) {
    if (src == SBW_GPU) {
        rc_gpu_write(lo, n);
        g_sbw_gen++;
        drw_note(lo, n);
        vk_gpu_write(lo, n);
    }
    if (!n) { if (src == SBW_GPU) g_sbw_over = 1; return; }
    uint64_t hi = lo + n;
    if (src == SBW_GPU && ((hi - 1) >> 12) - (lo >> 12) >= SBW_WPAGES) {   // a report is 16 bytes, never wide
        for (int i = 0; i < g_nsbw_w; i++)
            if (lo <= g_sbw_whi[i] && g_sbw_wlo[i] <= hi) { g_sbw_wlo[i] = MIN(lo, g_sbw_wlo[i]); g_sbw_whi[i] = MAX(hi, g_sbw_whi[i]); return; }
        if (g_nsbw_w == SBW_WN) { g_sbw_over = 1; return; }
        g_sbw_wlo[g_nsbw_w] = lo; g_sbw_whi[g_nsbw_w++] = hi;
        return;
    }
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) {
        int k = sbw_slot(pg, 1);
        if (k < 0) { g_sbw_over = 1; return; }
        g_sbwp[k].src |= (uint8_t)src;
    }
}
static int sbw_hits(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < g_nsbw_w; i++) if (lo < g_sbw_whi[i] && g_sbw_wlo[i] < hi) return 1;
    if (!g_nsbw) return 0;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) {
        int k = sbw_slot(pg, 0);
        if (k >= 0 && (g_sbwp[k].src & SBW_GPU)) return 1;
    }
    return 0;
}
// Ampere bindless constant buffer handle: bits 0..44 the address >> 4, 45..63 the size >> 4. Any handle has a bit at 45 or above, which no guest address has (below 2^40).
static uint64_t ucb_handle(uint64_t addr, uint64_t size) {
    uint64_t s = (size + 15) >> 4;
    return (addr >> 4 & ((1ull << 45) - 1)) | (s < (1u << 19) ? s : (1u << 19) - 1) << 45;
}
static uint64_t ucb_handle_addr(uint64_t v) { return v >> 45 ? (v & ((1ull << 45) - 1)) << 4 : v; }
// Size 0 or unknown: the rest of the Metal buffer (a device pointer past it is out of bounds on Metal anyway).
static uint64_t sbw_len(uint64_t size, void *mb, uint64_t mo) {
    if (size) return size;
    uint64_t len = ((__bridge id<MTLBuffer>)mb).length;
    return len > mo ? len - mo : 0;
}
static inline unsigned pend_bucket(uint64_t g, int lv) { return (unsigned)(((g * PEND_LV + (unsigned)lv) * 0x9E3779B97F4A7C15ull) >> (64 - PEND_HB_BITS)); }
static void pend_reset(void) {
    g_nhw_sub = 0;
    g_npend = g_pend_over = g_nupd = g_npnode = g_nsbw = g_nsbw_w = g_sbw_over = 0;
    g_pend_lv = 0;
    if (!++g_pend_gen) {
        memset(g_pend_hgen, 0, sizeof g_pend_hgen);
        if (g_sbwp) memset(g_sbwp, 0, (size_t)g_sbw_cap * sizeof *g_sbwp);
        g_pend_gen = 1;
    }
}
static void vc_gpu_write(uint64_t dst, uint64_t n);
static int pend_add(uint64_t dst, uint64_t n, int kind) {
    vc_gpu_write(dst, n);
    drw_note(dst, n ? n : 1);
    if (g_pend_over) return -1;
    uint64_t last = dst + (n ? n : 1) - 1; int lv = 0;
    while (lv < PEND_LV && (last >> pend_shift(lv)) - (dst >> pend_shift(lv)) >= PEND_SPAN) lv++;
    if (last < dst || lv == PEND_LV) { g_pend_over = 1; return -1; }
    uint64_t g0 = dst >> pend_shift(lv), g1 = last >> pend_shift(lv);
    if (!pend_room((int)(g1 - g0 + 1))) { g_pend_over = 1; return -1; }
    int e = g_npend;
    for (uint64_t g = g0; g <= g1; g++) {
        unsigned b = pend_bucket(g, lv);
        int k = g_npnode++;
        g_pend_node[k].e = e;
        g_pend_node[k].next = g_pend_hgen[b] == g_pend_gen ? g_pend_head[b] : -1;
        g_pend_head[b] = k;
        g_pend_hgen[b] = g_pend_gen;
    }
    g_pend_lv |= 1u << lv;
    g_pend_lo[e] = dst; g_pend_hi[e] = dst + n; g_pend_kind[e] = (uint8_t)kind; g_pend_upd[e] = 0;
    return g_npend++;
}
// a wider lookup scans every entry instead
static int pend_chains(uint64_t a, uint64_t last) {
    if (!g_pend_lv) return 1;
    unsigned s = pend_shift(__builtin_ctz(g_pend_lv));
    return (last >> s) - (a >> s) < PEND_SPAN;
}
// -1 for none, -2 when it cannot be told (overflow)
static int pend_last(uint64_t a, uint64_t n) {
    if (g_pend_over) return -2;
    uint64_t last = a + (n ? n : 1) - 1;
    if (!pend_chains(a, last)) {
        for (int e = g_npend - 1; e >= 0; e--) if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) return e;
        return -1;
    }
    int best = -1;
    for (uint32_t m = g_pend_lv; m; m &= m - 1) {
        int lv = __builtin_ctz(m);
        unsigned s = pend_shift(lv);
        for (uint64_t g = a >> s; g <= last >> s; g++) {
            unsigned b = pend_bucket(g, lv);
            if (g_pend_hgen[b] != g_pend_gen) continue;
            for (int k = g_pend_head[b]; k >= 0 && g_pend_node[k].e > best; k = g_pend_node[k].next) {
                int e = g_pend_node[k].e;
                if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) { best = e; break; }
            }
        }
    }
    return best;
}
static int pend_hits(uint64_t a, uint64_t n) { return pend_last(a, n) != -1; }
// → 1 when the last queued write is an inline update covering all n bytes; else 0 and *kind = what wrote them (0 = too many to tell)
static int pend_read(uint64_t a, uint64_t n, void *out, int *kind) {
    int e = pend_last(a, n);
    *kind = e >= 0 ? g_pend_kind[e] : 0;
    if (e < 0 || !g_pend_upd[e] || a < g_pend_lo[e] || a + n > g_pend_hi[e]) return 0;
    const upd_ent *u = &g_upd[g_pend_upd[e] - 1];
    memcpy(out, (const uint8_t *)u->buf.contents + u->off + (a - u->lo), n);
    return 1;
}
static id g_cb;
static id<MTL4ComputeCommandEncoder> g_blit;
// One allocation may have distinct Metal GPU addresses (overlapping pools, sparse mappings): encoder barriers need alias cache visibility.
static int g_blit_alias;
static inline MTL4VisibilityOptions blit_vis(void) { return resource_vis() | (g_blit_alias ? MTL4VisibilityOptionResourceAlias : 0); }
static NSMutableArray<dispatch_block_t> *g_commit_blocks;
// Uniform shadows taken once a shader with stores through loaded pointers has run, each with its submission's fence: a write log showing a pool the prediction left out
// checks the shadows that relied on its writes (gml_read).
enum { GSR_N = 16384 };
static struct { uint64_t fence, lo, hi; } g_gsr[GSR_N];
static uint64_t g_gsr_n;
static int g_gml_on;
static pthread_mutex_t g_gsr_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_alloc_fence[ALLOC_MAX];
static uint64_t g_alloc_waits, g_alloc_grown;
