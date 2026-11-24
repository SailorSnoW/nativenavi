// Counter reports, occlusion queries and conditional rendering, and the writes queued in a submission (pend, sbw) that they and the uniform shadows look up.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---------- Submission ----------
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; uint32_t type; void *vis; uint32_t seg; uint64_t copy_end; } post_op;   // vis: the visibility slot table for kind 3; seg: the segment it was encoded in (sub_cut)
static uint32_t g_seg;   // Index of the segment being encoded in this submission (0 until the first sub_cut)
static uint64_t g_cbgen; static int gsb_held(const void *mem, uint64_t size);   // main command buffers committed; whether storage overlaps what the one being encoded writes on the GPU (gsb_store)
typedef struct seg_clk seg_clk; static seg_clk *g_sclk; static void seg_clk_put(seg_clk *c);   // Counter timestamps by segment (see gpu_ticks)
// ---------- Conditional rendering ----------
// SetRenderEnableConditional(mode, rec) enables rendering if the 64-bit counter of the report at rec equals (RENDER_IF_EQUAL) or differs from (RENDER_IF_NOT_EQUAL) the one at
// rec + 16, until the next SetRenderEnable(true). Tears of the Kingdom wraps nearly every ClearColor / ClearDepthStencil this way: rec is set to 1 from the CPU when the target
// needs a clear, each clear is followed by ReportCounter(TIMESTAMP_TOP, rec), and rec + 16 is never written. A timestamp report puts 0 in the counter field (the time goes in the
// second field), so after one clear the two compare equal and later conditional clears of that target are skipped. The host used to ignore the condition and write the time
// into both fields, so every clear ran: the first tile-lighting round, the half-resolution scene depth its light volumes test against and the light accumulation were cleared
// before the composite read them, and the 3D scene came out black.
// Metal has no predication, so the condition is evaluated here at replay, on the CPU, from the records in memory. A report in an earlier unfinished GPU prefix is not written yet
// (that happens when the GPU completes): a timestamp one counts with its known counter 0; a samples-passed result (an occlusion query, written from the visibility buffers) is
// unknown until its submission completes, and a condition on it keeps drawing (occlusion culling only saves work). Once it has landed the condition is evaluated from it, as the
// GPU would at that point in the queue: Ocarina of Time reports its queries a submission ahead of the conditional draws, so every one has landed by then (1.48 million
// conditions on its Kokiri Forest route, 42% false; the host drew them all, GPU p50 17.06 → 16.44 ms a frame on an M4 Pro). Inside a block evaluated false, draws and clears are dropped
static int g_rc_skip;   // logical queue state: a render-enable block evaluated false is active
// Latest command-ordered writer of each report counter. Completed reports are read
// from memory, so CPU rewrites remain visible. Unfinished queries stay unknown;
// timestamp counters and explicit writes can supply known bytes before completion.
// Address lookup is constant-time. The dirty list stamps only this prefix's writers,
// and the page index keeps unrelated buffer updates out of the report table.
typedef struct rc_report {
    uint64_t addr, fence, value, report_fence, gpu_epoch;
    uint8_t known, dirty, readable, reported, report_dirty, unreliable, gpu_domain, opaque_unreliable, opaque_later, conservative;
    struct rc_report *page_next, *dirty_next;
} rc_report;
static nvn_hmap g_rc_reports={.klen=8}, g_rc_pages={.klen=8};
static uint64_t *g_rc_page_order; static uint32_t g_rc_page_n, g_rc_page_cap;
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
// Published after the CPU post effects land, before an event can wake a CPU writer.
// g_done is later than event publication and would briefly hide such a writer.
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
        r=calloc(1,sizeof *r); if (!r) MDIE("no memory for the report table");
        r->addr=addr; r->readable=1; r->gpu_epoch=g_rc_gpu_epoch; nvn_hm_put(&g_rc_reports,&addr,r);
        uint64_t page=addr>>12; r->page_next=nvn_hm_get(&g_rc_pages,&page);
        if (!r->page_next) rc_page_add(page);
        nvn_hm_put(&g_rc_pages,&page,r);
    }
    return r;
}
static void rc_query_operand(rc_report *r) {
    if (r->gpu_domain) return;
    r->gpu_domain=1;
    // An earlier opaque writer may have raced this timestamp before the address
    // first participated in a query comparison. Do not expose its CPU-post bytes.
    if (r->opaque_later) { r->known=0; r->opaque_later=0; }
    if (r->opaque_unreliable) r->unreliable=1;
}
static void rc_note(uint64_t addr, int kind) {
    rc_report *r=rc_ensure(addr);
    r->conservative=kind==3;   // counter kinds the host does not implement; a samples-passed result is read once it has landed (rc_value)
    // A condition compares adjacent 16-byte records. Either operand can be the
    // query; track its unreported neighbor before later explicit buffer writers.
    // Existing entries keep their own report type, pending fence and known bytes.
    if (kind==2) { rc_query_operand(r); if (addr>=16) rc_query_operand(rc_ensure(addr-16)); rc_query_operand(rc_ensure(addr+16)); }
    // Only timestamp and samples-passed reports have implemented counter values.
    // Other counter kinds retain the conservative fallback after their placeholder lands.
    r->value=0; r->known=kind==1 ? 255 : 0; r->readable=kind!=3;
    r->reported=r->report_dirty=1; r->report_fence=0; r->unreliable=r->opaque_unreliable=r->opaque_later=0; rc_touch(r);
}
static int rc_gpu_unknown(rc_report *r, uint64_t done) {
    if (!r->gpu_domain || r->gpu_epoch==g_rc_gpu_epoch) return 0;
    // An unbounded shader writer may race a CPU-landed query. A fresh report or
    // explicit full write can establish that counter again. Unreported reference
    // words have no deferred CPU report, so they become readable after the writer.
    if (r->reported || g_rc_gpu_dirty || g_rc_gpu_fence>done) return 1;
    r->gpu_epoch=g_rc_gpu_epoch; return 0;
}
static uint8_t rc_source(uint64_t a, unsigned n, uint8_t *out, int *unreliable);
static int rc_value(uint64_t addr, uint64_t *v) {   // 0 = unknown at encode time
    rc_report *r=rc_find(addr);
    uint64_t done=atomic_load_explicit(&g_rc_complete,memory_order_acquire);
    // Replay ahead (nvn_rec.h) reaches a condition before the GPU has run the report it reads, and an unknown condition draws its block: Ocarina of Time's Kakariko Village had
    // 360-480 more draws a frame (2,930 to 3,290-3,410). The translation waits here for the game's signal, as it does without the hold; what it has translated by then goes to
    // the GPU at the signal. Waiting for the report's fence as well cost more than it saved (57.4 to 55.2 fps, the translation stalled 0.8 times a frame)
    if (r && !r->conservative && r->fence>done && atomic_load(&g_hold_on) && on_sub_thread()) { hold_block(1); g_rc_ahead_waits++; done=atomic_load_explicit(&g_rc_complete,memory_order_acquire); }
    if (r) {
        if (r->conservative) return 0;
        if (rc_gpu_unknown(r,done)) return 0;
        if (r->dirty || r->fence>done) { *v=r->value; return r->known==255; }
        if (!r->readable || r->unreliable) return 0;
        *v=*(const volatile uint64_t *)addr; return 1;
    }
    // No report of its own: memory holds the operand only when nothing queued before the condition writes it later (rc_source: an inline update of this submission is known by its
    // bytes; a copy, fill or shader write of this submission, or a GPU write of one still running, is not). Portal 2's D3D9 layer has a shader write each mesh's visibility into a
    // storage buffer and draws the mesh if that word differs from the next record's; read from memory before that shader ran, both were 0 and the vault's furniture was dropped
    uint8_t b[8]; int unreliable=0;
    if (rc_source(addr,8,b,&unreliable)!=0xff || unreliable) return 0;
    memcpy(v,b,8); return 1;
}
// Counter records need not share a page with the buffer write. Include the previous
// seven bytes so even an unaligned eight-byte counter crossing a page is covered.
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
        // Keep ordinary timestamp forwarding unchanged, but retain this history
        // in case a later query makes the address a comparison operand.
        if (r->reported) r->opaque_unreliable=r->opaque_later=1;
        return;
    }
    int report_pending=r->reported && (r->report_dirty || r->report_fence>done);
    if (r->opaque_later) { r->known=0; r->opaque_later=0; }
    if (r->opaque_unreliable) r->unreliable=1;
    if ((r->reported && r->gpu_epoch!=g_rc_gpu_epoch) || rc_gpu_unknown(r,done)) { r->known=0; if (r->reported) r->unreliable=1; }
    if (opaque) {
        if (report_pending) r->unreliable=1;
        r->known=0; rc_touch(r); return;
    }
    // The preceding writer has landed: keep unaffected bytes from the current CPU
    // value, including a legal CPU rewrite after that writer completed.
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
    // Query reports still land on the CPU after GPU execution. A following GPU
    // overwrite/copy can therefore leave unreliable final bytes. Preserve the
    // existing conservative fallback for this unsupported ordering pattern;
    // pending command-known values remain valid, but completed memory is not used.
    if (unreliable) r->unreliable=1;
    else if (at==0 && bytes==8) r->unreliable=r->opaque_unreliable=r->opaque_later=0;
    rc_touch(r);
}
// A later explicit buffer write replaces the report's command-ordered knowledge,
// including partial writes. Its bytes stay live until that writer completes.
// This does not make CPU-landed query reports ordered GPU sources: arbitrary
// shader-produced predicates and GPU reads of an unfinished report remain unsupported.
static void rc_change(uint64_t dst, uint64_t n, const void *data, unsigned period, uint64_t src, int opaque) {
    if (!g_rc_reports.n || !n) return;
    uint64_t done=atomic_load_explicit(&g_rc_complete,memory_order_acquire);
    uint64_t first=(dst>7 ? dst-7 : 0)>>12, last=(dst+n-1)>>12;
    // Large writable bindings are common on every draw. Search the recorded page
    // addresses, then visit only intersecting pages; scanning every report made
    // unrelated storage buffers cost several milliseconds per frame.
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
// ---------- Occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED) ----------
// NVN semantics: samples passed is a cumulative counter on the queue - ResetCounter zeroes it, every draw afterwards adds to it, and ReportCounter writes out the cumulative value at that moment (+ timestamp) when the GPU completes.
// Metal: every render pass attaches a visibility result buffer; draws during counting use counting mode, with a new slot per pass (no reliance on accumulation across passes). The counter is one running total
// (g_vis_acc), kept by post_run in submission order under g_post_mu: a report hands the slots since the previous report (retained) to its submission's completion, which adds them to the total and writes it out;
// the slots left at a submission's end are added at its completion too (VIS_FOLD), so a slot is read once, while its buffer is still this submission's (the ring hands it to a later one once the fence passes);
// a reset drops the slots before it and zeroes the total there (VIS_ZERO). After a report a new slot is used, so later draws are not counted into the reported value.
// Counting starts at the first reset or, for a game that never resets, at the first report: the GPU's counter runs from the queue's creation, so such a game reads end - begin of two reports around each query
// (Red Dead Redemption's character occlusion queries: counting used to start only at a reset, so every report was 0 and the bodies of characters a few metres away were dropped).
// (Previously always wrote 0: occlusion query = 'fully occluded', campfires / light sources got culled - the character select screen calls ResetCounter 21 times per frame)
typedef struct { void *buf; uint32_t slot; } vis_slot;
typedef struct { int n; vis_slot s[]; } vis_list;
#define VIS_SLOTS 8192
typedef struct { __strong id<MTLBuffer> buf; uint64_t fence; } vis_buf;
// One visibility buffer per submission in flight, as many as command allocators: with 4, Age of Calamity (about twelve submissions a frame) waited in mtl_sub_begin for the previous frame's GPU work; its first battle went from 18.0 to 16.9 ms a frame on the Mac
// The ring grows on demand (vis_take), each buffer kept resident and reused once its submission's fence passes. Since a frame's submissions share a command allocator, more
// counting submissions are in flight than allocators: with a ring of 8, a GPU running behind left none free and each such submission made a one-off buffer, added to the
// residency set and removed after it, two residency commits through the kernel's GPU driver per submission (issue #2535).
// So the ring holds as many as can be in flight, and never runs dry: it grows only to the most counting submissions in flight at once (SUB_FLIGHT_MAX x 64 KiB = 32 MiB at most)
enum { VIS_RING = SUB_FLIGHT_MAX };
static vis_buf g_vis_ring[VIS_RING]; static int g_vis_ring_n, g_vis_ring_i = -1;
static id<MTLBuffer> g_vis; static uint32_t g_vis_n;   // This submission's visibility buffer and number of slots used
static void vis_take(void);
// Whether b is this submission's ring buffer: g_vis is either that or an overflow buffer of its own, which is retired after it
static int vis_in_ring(id<MTLBuffer> b) { return g_vis_ring_i >= 0 && g_vis_ring[g_vis_ring_i].buf == b; }
static int g_vis_on, g_vis_counting, g_vis_cur = -1; static uint64_t g_vis_cur_pass;
// Whether a report can still see the draws encoded now (vis_need_scan): 0 from a samples-passed reset or report whose next samples-passed op in this submission is a reset
static int g_vis_need = 1; static uint64_t g_vis_unseen;   // and the draws encoded with counting off for that reason (tests, diagnostics)
static vis_slot *g_vs; static int g_nvs, g_vscap;      // Slots since the last report or reset, in this submission
enum { VIS_REPORT, VIS_FOLD, VIS_ZERO };               // post_op mode for kind 3: add the slots and write the total / only add them / zero the total
static uint64_t g_vis_acc;                              // The counter's running total (post_run, under g_post_mu)
static void vis_clear(void) { for (int i = 0; i < g_nvs; i++) CFRelease(g_vs[i].buf); g_nvs = 0; g_vis_cur = -1; }
static vis_list *vis_snapshot(void) {
    vis_list *l = malloc(sizeof *l + g_nvs * sizeof(vis_slot)); l->n = g_nvs;
    for (int i = 0; i < g_nvs; i++) { l->s[i] = g_vs[i]; CFRetain(g_vs[i].buf); }
    return l;
}
// Target ranges of buffer writes (copies / inline uniform updates) already encoded in this submission but not yet executed on the GPU: reading these addresses on the CPU side at encode time yields old values (uv_proof avoids them when reading uniforms)
// Inline updates keep where their data sits in the update ring (g_pend_upd: index + 1 into g_upd), so a bindless handle they wrote can be read as the GPU will see it (pend_read).
// The writes are indexed by granule (a chain per hash bucket, newest first). Granules come in PEND_LV sizes, 4 KB and each eight times the last (pend_shift); a write goes in at the
// finest over which it spans at most PEND_SPAN granules, one node per granule, and a lookup walks the chains of the granules it touches at each size this submission used (g_pend_lv).
// Writes over more than PEND_SPAN pages had a list of their own instead, which every lookup scanned and which stopped at 256: Mortal Kombat 11 passed it (a write anywhere, below), and
// its bindless handles were read from memory under updates still queued (RED 47). Merely grown, that scan would cost each lookup about 0.25 us per thousand such writes
// The list grows as a submission needs it and keeps its size for the next (pend_room). It used to stop at a fixed count, and past it the submission counted as a write anywhere
// (g_pend_over), which gw_commit carries past the submission: every partly covered uniform shadow of later submissions was refused while it ran, so every draw that read one ended its
// pass. Monster Hunter Rise passed 256 writes in a submission, and Splatoon Raiders' third boss stage about 11,000 (7 uniform updates before each of about 1,550 ink-map queries; 2,800
// in its other fights): past 8,192 each query took a pass of its own on its 6400x6400 map, the GPU fell behind, the submissions kept running, and the stage stayed at 1-2 fps.
// A write anywhere is left for a list that cannot grow (no memory) and a write past the end of the address space
enum { PEND_HB_BITS = 13, PEND_HB = 1 << PEND_HB_BITS, PEND_SPAN = 8, PEND_LV = 16 };
static inline unsigned pend_shift(int lv) { return 12 + 3 * (unsigned)lv; }   // granule size 4 KB << 3 lv: the coarsest, 2^57 bytes, holds any write within PEND_SPAN granules
static uint32_t g_pend_lv;   // the granule sizes holding writes this submission (bit lv)
static uint64_t *g_pend_lo, *g_pend_hi; static int g_npend, g_pend_over, g_pend_cap; static uint8_t *g_pend_kind; static uint32_t *g_pend_upd;   // kind: 1 inline update, 2 copy, 3 fill
static int32_t g_pend_head[PEND_HB]; static uint32_t g_pend_hgen[PEND_HB], g_pend_gen = 1;   // a bucket's chain is valid when its generation is this submission's
typedef struct { int32_t e, next; } pend_node;
static pend_node *g_pend_node; static int g_npnode, g_pnode_cap;
typedef struct { uint64_t lo; size_t off; __unsafe_unretained id<MTLBuffer> buf; } upd_ent;   // the ring block lives until this submission completes (mtl_sub_update)
static upd_ent *g_upd; static int g_nupd;   // one per entry at most: sized with the entries
static int *g_ubp_idx;   // every entry a lookup can meet (pend_since): at most one per node of the chains it walks, or every entry (each has a node): sized with the nodes
static int pend_grow(void **p, size_t n, size_t each) { void *q = realloc(*p, n * each); if (!q) return 0; *p = q; return 1; }
// Room for one more entry and nodes more nodes: 0 when memory runs out (the caller counts the write as one anywhere)
static int pend_room(int nodes) {
    if (g_npend == g_pend_cap) { if (g_pend_cap > INT_MAX / 2) return 0; int c = g_pend_cap ? g_pend_cap * 2 : 4096;
        if (!pend_grow((void **)&g_pend_lo, c, sizeof *g_pend_lo) || !pend_grow((void **)&g_pend_hi, c, sizeof *g_pend_hi) || !pend_grow((void **)&g_pend_kind, c, sizeof *g_pend_kind)
            || !pend_grow((void **)&g_pend_upd, c, sizeof *g_pend_upd) || !pend_grow((void **)&g_upd, c, sizeof *g_upd)) return 0;   // a grown array kept at its new size is only spare room
        g_pend_cap = c; }
    if (!g_pnode_cap || g_npnode + nodes > g_pnode_cap) { int c = g_pnode_cap ? g_pnode_cap : 8192; while (c < g_npnode + nodes) { if (c > INT_MAX / 2) return 0; c *= 2; }
        if (!pend_grow((void **)&g_pend_node, c, sizeof *g_pend_node) || !pend_grow((void **)&g_ubp_idx, c, sizeof *g_ubp_idx)) return 0;
        g_pnode_cap = c; }
    return 1;
}
// Other writes to guest memory in this submission, which the pend list does not hold: writable storage buffers bound to a shader, texture→buffer copies, counter reports (ubp_get).
// Kept by 4 KB page (Breath of the Wild's particle dispatches bind about ten storage buffers each, hundreds a frame): a set of pages tagged with the submission (g_pend_gen) and with what
// wrote them. A counter report does not count against this submission's shadows: the host writes reports on the CPU when the whole submission has completed, so no draw of it reads one
// (Fire Emblem Engage writes its query reports into the page of a uniform block); for later submissions it counts by its own 16 bytes while this one runs (hw_note, hw_live).
// A GPU write over more than SBW_WPAGES pages is kept as an interval instead (overlapping ones merge): a storage buffer bound with size 0 counts as the rest of its Metal buffer (sbw_len)
enum { SBW_INIT = 16384, SBW_WPAGES = 64, SBW_WN = 256 };   // g_sbwp grows on demand from 16,384 slots (large Switch 2 scenes such as Donkey Kong Bananza's 3D overview map touch >16,384 unique pages per submission)
typedef struct { uint64_t page; uint32_t gen; uint8_t src; } sbw_ent;
static sbw_ent *g_sbwp; static uint32_t *g_sbw_list; static uint32_t g_sbw_cap, g_sbw_shift, g_nsbw; static int g_sbw_over;   // src: SBW_* bits; pages in the set this submission; over: a write could not be kept (out of memory, size not known, wide list full)
static uint64_t g_sbw_wlo[SBW_WN], g_sbw_whi[SBW_WN]; static int g_nsbw_w;   // the wide writes of this submission
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
static int sbw_slot(uint64_t pg, int add) {   // open addressing (load <= 50%); −1 = not there (or no room to add)
    if (!add && (!g_nsbw || !g_sbwp)) return -1;
    if (add && (!g_sbwp || g_nsbw >= g_sbw_cap / 2)) { if (!sbw_grow()) return -1; }
    unsigned h = (unsigned)((pg * 0x9E3779B97F4A7C15ull) >> g_sbw_shift) & (g_sbw_cap - 1);
    for (;; h = (h + 1) & (g_sbw_cap - 1)) {
        if (g_sbwp[h].gen != g_pend_gen) { if (!add) return -1; g_sbwp[h].page = pg; g_sbwp[h].gen = g_pend_gen; g_sbwp[h].src = 0; g_sbw_list[g_nsbw++] = h; return (int)h; }
        if (g_sbwp[h].page == pg) return (int)h; }
}
enum { SBW_GPU = 1, SBW_REPORT = 2 };
static uint64_t *g_hw_sub; static int g_nhw_sub, g_hw_cap;   // the 16-byte records this submission's completion writes (hw_note), in order
static uint32_t g_sbw_gen;   // bumped by every write noted that this submission's shadows must respect (a cached shadow made before it is checked again)
static void drw_note(uint64_t lo, uint64_t n);   // the writes since the last drain (below g_wr_epoch)
static void vk_gpu_write(uint64_t dst, uint64_t n);   // kept vertex conversions of the written range are dropped (vk_get); n = 0: a write anywhere
static void sbw_note(uint64_t lo, uint64_t n, int src) {
    if (src==SBW_GPU) rc_gpu_write(lo,n);
    if (src == SBW_GPU) { g_sbw_gen++; drw_note(lo, n); vk_gpu_write(lo, n); }
    if (!n) { if (src == SBW_GPU) g_sbw_over = 1; return; }
    uint64_t hi = lo + n;
    if (src == SBW_GPU && ((hi - 1) >> 12) - (lo >> 12) >= SBW_WPAGES) {   // a report is 16 bytes, never wide
        for (int i = 0; i < g_nsbw_w; i++) if (lo <= g_sbw_whi[i] && g_sbw_wlo[i] <= hi) { g_sbw_wlo[i] = MIN(lo, g_sbw_wlo[i]); g_sbw_whi[i] = MAX(hi, g_sbw_whi[i]); return; }
        if (g_nsbw_w == SBW_WN) { g_sbw_over = 1; return; }
        g_sbw_wlo[g_nsbw_w] = lo; g_sbw_whi[g_nsbw_w++] = hi; return;
    }
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { int k = sbw_slot(pg, 1); if (k < 0) { g_sbw_over = 1; return; } g_sbwp[k].src |= (uint8_t)src; }
}
static int sbw_hits(uint64_t lo, uint64_t hi) {
    for (int i = 0; i < g_nsbw_w; i++) if (lo < g_sbw_whi[i] && g_sbw_wlo[i] < hi) return 1;
    if (!g_nsbw) return 0;
    for (uint64_t pg = lo >> 12; pg <= (hi - 1) >> 12; pg++) { int k = sbw_slot(pg, 0); if (k >= 0 && (g_sbwp[k].src & SBW_GPU)) return 1; }
    return 0;
}
// The bytes a writable storage binding at Metal buffer offset mo can write: its size, else (bound with size 0, as Breath of the Wild's GPU particle dispatches bind their outputs, or a
// size not known) the rest of that buffer. A device pointer that runs past its buffer is out of bounds on Metal, which the host could not give the Switch's meaning anyway. Before, such
// a binding counted as a write anywhere, and every partly covered uniform shadow after it in the submission was refused (BotW shrines: about 240 more passes a frame, GitHub issue #534)
// A bindless constant buffer handle in the hardware's layout (Ampere; as NVK's nvk_bindless_cbuf): bits 0..44 the address >> 4, 45..63 the size >> 4, which the shaders
// decode (InstEmitAmpere.cs ABindlessConstant). PRAGMATA builds its own: b12078fe3bce1273 pushes 0x5a0083055d124, 32 bytes at 0x83055d1240. ucb_handle_addr reads a buffer
// word that holds either one or a plain address: a handle of any size has a bit at 45 or above, which no guest address has (they lie below 2^40)
static uint64_t ucb_handle(uint64_t addr, uint64_t size) { uint64_t s = (size + 15) >> 4; return (addr >> 4 & ((1ull << 45) - 1)) | (s < (1u << 19) ? s : (1u << 19) - 1) << 45; }
static uint64_t ucb_handle_addr(uint64_t v) { return v >> 45 ? (v & ((1ull << 45) - 1)) << 4 : v; }
static uint64_t sbw_len(uint64_t size, void *mb, uint64_t mo) { if (size) return size; uint64_t len = ((__bridge id<MTLBuffer>)mb).length; return len > mo ? len - mo : 0; }
static inline unsigned pend_bucket(uint64_t g, int lv) { return (unsigned)(((g * PEND_LV + (unsigned)lv) * 0x9E3779B97F4A7C15ull) >> (64 - PEND_HB_BITS)); }   // granule g of size lv
static void pend_reset(void) { g_nhw_sub = 0; g_npend =g_pend_over = g_nupd = g_npnode = g_nsbw = g_nsbw_w = g_sbw_over = 0; g_pend_lv = 0; if (!++g_pend_gen) { memset(g_pend_hgen, 0, sizeof g_pend_hgen); if (g_sbwp) memset(g_sbwp, 0, (size_t)g_sbw_cap * sizeof *g_sbwp); g_pend_gen = 1; } }
static void vc_gpu_write(uint64_t dst, uint64_t n);   // kept index ranges under the written range are not trusted again (ix_range)
static int pend_add(uint64_t dst, uint64_t n, int kind) {
    vc_gpu_write(dst, n); drw_note(dst, n ? n : 1);
    if (g_pend_over) return -1;
    uint64_t last = dst + (n ? n : 1) - 1; int lv = 0;
    while (lv < PEND_LV && (last >> pend_shift(lv)) - (dst >> pend_shift(lv)) >= PEND_SPAN) lv++;
    if (last < dst || lv == PEND_LV) { g_pend_over = 1; return -1; }
    uint64_t g0 = dst >> pend_shift(lv), g1 = last >> pend_shift(lv);
    if (!pend_room((int)(g1 - g0 + 1))) { g_pend_over = 1; return -1; }
    int e = g_npend;
    for (uint64_t g = g0; g <= g1; g++) { unsigned b = pend_bucket(g, lv); int k = g_npnode++;
        g_pend_node[k].e = e; g_pend_node[k].next = g_pend_hgen[b] == g_pend_gen ? g_pend_head[b] : -1; g_pend_head[b] = k; g_pend_hgen[b] = g_pend_gen; }
    g_pend_lv |= 1u << lv;
    g_pend_lo[e] = dst; g_pend_hi[e] = dst + n; g_pend_kind[e] = (uint8_t)kind; g_pend_upd[e] = 0; return g_npend++;
}
// Whether a lookup of [a, last] walks chains: it touches at most PEND_SPAN granules of every size in use (the finest decides). A wider one scans every entry instead
static int pend_chains(uint64_t a, uint64_t last) { if (!g_pend_lv) return 1; unsigned s = pend_shift(__builtin_ctz(g_pend_lv)); return (last >> s) - (a >> s) < PEND_SPAN; }
// The newest queued write overlapping [a, a+n): its index, -1 for none, -2 when it cannot be told (overflow)
static int pend_last(uint64_t a, uint64_t n) {
    if (g_pend_over) return -2;
    uint64_t last = a + (n ? n : 1) - 1;
    if (!pend_chains(a, last)) { for (int e = g_npend - 1; e >= 0; e--) if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) return e; return -1; }
    int best = -1;
    for (uint32_t m = g_pend_lv; m; m &= m - 1) { int lv = __builtin_ctz(m); unsigned s = pend_shift(lv);
        for (uint64_t g = a >> s; g <= last >> s; g++) { unsigned b = pend_bucket(g, lv); if (g_pend_hgen[b] != g_pend_gen) continue;
            for (int k = g_pend_head[b]; k >= 0 && g_pend_node[k].e > best; k = g_pend_node[k].next) { int e = g_pend_node[k].e; if (a < g_pend_hi[e] && a + n > g_pend_lo[e]) { best = e; break; } } } }
    return best;
}
static int pend_hits(uint64_t a, uint64_t n) { return pend_last(a, n) != -1; }
// The n bytes at a as the last write queued in this submission leaves them, when that write is an inline update covering them all → 1; otherwise → 0 and *kind = what wrote them (0 = too many writes to tell)
static int pend_read(uint64_t a, uint64_t n, void *out, int *kind) {
    int e = pend_last(a, n); *kind = e >= 0 ? g_pend_kind[e] : 0;
    if (e < 0 || !g_pend_upd[e] || a < g_pend_lo[e] || a + n > g_pend_hi[e]) return 0;
    const upd_ent *u = &g_upd[g_pend_upd[e] - 1]; memcpy(out, (const uint8_t *)u->buf.contents + u->off + (a - u->lo), n); return 1;
}
static id g_cb;
static id<MTL4ComputeCommandEncoder> g_blit;
// Buffer operations may use distinct Metal GPU addresses for one allocation
// (overlapping pools or sparse mappings). Their encoder barriers need alias cache visibility.
static int g_blit_alias;
static inline MTL4VisibilityOptions blit_vis(void) { return resource_vis() | (g_blit_alias ? MTL4VisibilityOptionResourceAlias : 0); }
static NSMutableArray<dispatch_block_t> *g_commit_blocks;
// The uniform shadows taken once a shader with stores through loaded pointers has run (g_gml_on), each with the fence of the submission that took it: every shadow made or checked
// again after a GPU write was noted (ubp_get). A write log that shows a pool the prediction left out checks the shadows of its own submission and of the later ones encoded before
// it completed, the ones that relied on its writes (gml_read): the miss matters only if one of them covers a page the stores wrote
enum { GSR_N = 16384 };
static struct { uint64_t fence, lo, hi; } g_gsr[GSR_N]; static uint64_t g_gsr_n; static int g_gml_on;
static pthread_mutex_t g_gsr_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_alloc_fence[ALLOC_MAX]; static uint64_t g_alloc_waits, g_alloc_grown;
