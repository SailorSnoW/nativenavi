// The device's start (mtl_init) and memory pools: the address-to-pool table, zero-copy buffers, large pools in chunks, residency on demand.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---------- Memory pools ----------
// How far a pool entry's buffer reaches (BIG_MIN): REACH_WHOLE a large pool's whole-pool buffer; REACH_END to its pool's end (an ordinary pool, or a chunk that reaches the end); REACH_SHORT a chunk
// that ends before its pool does, which answers only lookups that say how far they read
enum { REACH_WHOLE, REACH_END, REACH_SHORT };
typedef struct { uintptr_t lo, hi; uintptr_t base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; const void *key; int res; uint64_t born; uintptr_t end; int reach; uint32_t n_tex_reads, n_other; } pool_ent;   // res: already in the residency set (on demand, see pool_res); born: when it was added (ns); end: where the buffer ends, reach: how far that is (a large pool's chunk, see BIG_MIN); n_tex_reads / n_other: lookups by texture uploads / by anything else (mem_census)
static pool_ent *g_pools; static int g_npool, g_cap;
static int g_pidx_ok, g_plast = -1;   // whether the address→pool index is valid, the last hit (pool_find)
static NSMutableArray *g_keep;                  // Holds the buffer references (pool_ent has non-owning pointers)
static int g_pool_res_n;   // registered pools already in the residency set (for the memory readout; under g_mu)
static __thread int t_tex_upload_lookup;   // set around a texture upload's read of its storage (pool lookups counted apart, mem_census)

#define C1_BANK_SIZE (4u << 20)
#define C1_DECL_SIZE (64u << 10)
static NSMutableArray *g_c1_banks;
static size_t g_c1_bank_off;
static uint64_t g_c1_count;
static NSMutableSet<NSString *> *g_c1_full;
static int g_c1_full_valid;
static NSDictionary<NSString *, NSString *> *g_cb_ext;   // cb_extent.tsv: shader key → "reg:bytes ..." (how far into each uniform slot the shader can read); nil when stale or missing
static os_unfair_lock g_c1_lock = OS_UNFAIR_LOCK_INIT;

// DM_UDC_SELFTEST=1 (diagnostic, written to check DontCare behavior on a real A19 device): private-storage Depth32Float_Stencil8 / RGBA8 (1952x900) are first cleared to write known values,
// then Load + DontCare without drawing, read back and compared pixel by pixel, printing one result line
static void udc_selftest(void) {
    if (!getenv("DM_UDC_SELFTEST")) return;
    id<MTLCommandQueue> q = [g_dev newCommandQueue];
    for (int depth = 1; depth >= 0; depth--) {
        NSUInteger w = 1952, h = 900;
        MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:depth ? MTLPixelFormatDepth32Float_Stencil8 : MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        td.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; td.storageMode = MTLStorageModePrivate;
        id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
        id<MTLCommandBuffer> cb = [q commandBuffer];
        for (int pass = 0; pass < 2; pass++) {
            MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
            MTLLoadAction la = pass ? MTLLoadActionLoad : MTLLoadActionClear; MTLStoreAction sa = pass ? MTLStoreActionDontCare : MTLStoreActionStore;
            if (depth) { rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = la; rp.depthAttachment.storeAction = sa; rp.depthAttachment.clearDepth = 0.3125;
                         rp.stencilAttachment.texture = t; rp.stencilAttachment.loadAction = la; rp.stencilAttachment.storeAction = sa; rp.stencilAttachment.clearStencil = 0x5a; }
            else { rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = la; rp.colorAttachments[0].storeAction = sa; rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 0.8); }
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        id<MTLBuffer> out = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0
            destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4 options:depth ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *px = out.contents; NSUInteger bad = 0; for (NSUInteger i = 0; i < w * h; i++) if (px[i] != px[0]) bad++;
        MLOG("UDC self-check %s %s %lux%lu first value %08x, values differing from it %lu / %lu", g_dev.name.UTF8String, depth ? "Depth32F_S8" : "RGBA8", (unsigned long)w, (unsigned long)h, px[0], (unsigned long)bad, (unsigned long)(w * h));
    }
}
static void mtl_init_once(void) {
    const char *vbc = getenv("DM_NVN_VBUF_CACHE"); g_vbuf_cache = !(vbc && !strcmp(vbc, "0"));
    const char *link = getenv("DM_PRESENT_LINK");
    g_dm_present_link = link ? atoi(link) : 1; if (g_dm_present_link < 0 || g_dm_present_link > 2) g_dm_present_link = 1; g_present_link_env = link != NULL;   // Default 1 (real-device same-process alternation: when load exceeds one display-link period, long frames 15% → 0 and frame intervals even out; no difference from 0 at a steady 60); DM_PRESENT_LINK=0 turns it off   // 1 = present on every tick of a 60 Hz display link; 2 = 120 Hz display link, the game presents a frame every other display-link tick (when behind, it can present on the next 8.33 ms display-link tick)
    const char *latency = getenv("DM_PRESENT_LATENCY");
    if (latency && atoi(latency) > 0) g_present_latency = atoi(latency);
    g_res_check_on = getenv("DM_RES_CHECK") != NULL;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) MDIE("no Metal device");
    if (!g_dev.hasUnifiedMemory) MDIE("needs a unified-memory device (zero-copy memory pools)");
    mem_pressure_start();
    g_q = [g_dev newCommandQueue];
    // A device without Metal 4 (the paravirtualized GPU of a virtual Mac, such as GitHub's macos-26 runner: no residency set, no MTL4CommandQueue) cannot run the NVN layer
    NSError *re = nil; const char *no4 = NULL;
    if (![g_dev supportsFamily:MTLGPUFamilyMetal4]) no4 = "no Metal 4 GPU family";
    else {
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
        rd.initialCapacity = 4096;
        g_res = [g_dev newResidencySetWithDescriptor:rd error:&re];
        rd.initialCapacity = 8; if (g_res) g_snap_res = [g_dev newResidencySetWithDescriptor:rd error:&re];   // SNAP_MAX textures and the old ones they replaced
        if (!g_res || !g_snap_res) no4 = "failed to create the MTLResidencySet";
    }
    if (!no4) {
        dispatch_queue_attr_t feedback_attr = dispatch_queue_attr_make_with_qos_class(DISPATCH_QUEUE_SERIAL, QOS_CLASS_USER_INTERACTIVE, 0);
        g_feedback_q = dispatch_queue_create("dm.nvn.mtl4-feedback", feedback_attr);
        MTL4CommandQueueDescriptor *q4d = [MTL4CommandQueueDescriptor new]; q4d.feedbackQueue = g_feedback_q;
        g_q4 = [g_dev newMTL4CommandQueueWithDescriptor:q4d error:&re];
        if (!g_q4) no4 = "failed to create the MTL4CommandQueue";
    }
    if (no4) {
        const char *why = re ? re.localizedDescription.UTF8String : "";
        MDIE("the Metal device %s has no Metal 4 (%s%s%s), which the NVN layer needs; a virtual Mac's paravirtualized GPU has none", g_dev.name.UTF8String, no4, *why ? ": " : "", why);
    }
    // BC textures are created and uploaded as BC, with no decompression path (dm/gfx/tex/README.md); without BC the first one aborts in Metal's descriptor validation mid-game
    // (GitHub issue #704). Every Apple silicon Mac has BC; this stops any other device at the start, with the reason
    if (!g_dev.supportsBCTextureCompression) MDIE("the Metal device %s has no BC texture formats, which the game's textures use and the host does not decompress: an Apple silicon Mac is needed", g_dev.name.UTF8String);
    { void crash_set_wait_dump(void (*fn)(void)); void mtl_wait_state_dump(void); crash_set_wait_dump(mtl_wait_state_dump); }
    g_alloc_n = 3;
    for (int i = 0; i < 3; i++) {
        MTL4CommandAllocatorDescriptor *ad = [MTL4CommandAllocatorDescriptor new];
        ad.label = [NSString stringWithFormat:@"nvn-mtl allocator %d", i];
        NSError *ae = nil;
        g_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_alloc4[i]) MDIE("failed to create MTL4CommandAllocator[%d]: %s", i, ae.localizedDescription.UTF8String);
        g_up_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:ad error:&ae];
        if (!g_up_alloc4[i]) MDIE("failed to create upload MTL4CommandAllocator[%d]: %s", i, ae.localizedDescription.UTF8String);
        MTL4CommandAllocatorDescriptor *pad = [MTL4CommandAllocatorDescriptor new];
        pad.label = [NSString stringWithFormat:@"nvn-mtl present allocator %d", i];
        g_present_alloc4[i] = [g_dev newCommandAllocatorWithDescriptor:pad error:&ae];
        if (!g_present_alloc4[i]) MDIE("failed to create present MTL4CommandAllocator[%d]: %s", i, ae.localizedDescription.UTF8String);
        g_present_cb[i] = [g_dev newCommandBuffer];
        if (!g_present_cb[i]) MDIE("failed to create present MTL4CommandBuffer[%d]", i);
    }
    MTL4ArgumentTableDescriptor *atd = [MTL4ArgumentTableDescriptor new];
    atd.maxBufferBindCount = 31; atd.maxTextureBindCount = 1;
    NSError *ate = nil;
    g_vt = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]); if (!g_vt) MDIE("failed to create the vertex argument table: %s", ate.localizedDescription.UTF8String);
    g_vt_valid = 0;
    g_ft = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]); if (!g_ft) MDIE("failed to create the fragment argument table: %s", ate.localizedDescription.UTF8String);
    g_ct = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]); if (!g_ct) MDIE("failed to create the compute argument table: %s", ate.localizedDescription.UTF8String);
    g_ot = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]); if (!g_ot) MDIE("failed to create the object argument table: %s", ate.localizedDescription.UTF8String);
    g_up_ct = [g_dev newArgumentTableWithDescriptor:atd error:&ate]; if (!g_up_ct) MDIE("failed to create the upload compute argument table: %s", ate.localizedDescription.UTF8String);
    MTL4ArgumentTableDescriptor *pad = [MTL4ArgumentTableDescriptor new];
    pad.maxTextureBindCount = 1; pad.maxSamplerStateBindCount = 1; pad.maxBufferBindCount = 1;
    g_present_args = [g_dev newArgumentTableWithDescriptor:pad error:&ate];
    if (!g_present_args) MDIE("failed to create the present argument table: %s", ate.localizedDescription.UTF8String);
    for (int i = 0; i < 3; i++) {
        g_present_uv[i] = [g_dev newBufferWithLength:sizeof(float) * 8 options:MTLResourceStorageModeShared];
        if (!g_present_uv[i]) MDIE("failed to create present UV buffer[%d]", i);
        res_add(g_present_uv[i]);
    }
    g_ev = [g_dev newSharedEvent];
    g_rx_ev = [g_dev newSharedEvent];
    g_keep = [NSMutableArray array];
    g_nvn_deswz_gpu = nvn_deswz_gpu_init(g_dev) == 0;
    if (!g_nvn_deswz_gpu) MLOG("[RED] GPU deswizzle init failed; swizzled textures fall back to the CPU");
    MLOG("Metal device %s", g_dev.name.UTF8String);
    udc_selftest();
}

void mtl_init(void) {
    // Pool registration and the shutdown drain can arrive together on a cold device.
    // Returning a device alone is insufficient: the callers also need the queues and residency sets.
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mtl_init_once(); });
}

// No-copy buffers of finalized pools (weak references, under g_mu): the memory report counts how many are still alive and their length -- a buffer that outlives its pool is still strongly referenced by something else (a texture built directly on the pool, etc.)
static NSHashTable *g_dead_pools;
// Address → pool cache per thread (mtl_addr / mtl_gpu_addr), valid while g_pool_gen is unchanged (every pool add / removal bumps it under g_mu), holding only pools already in the residency set.
// Every buffer binding of every draw used to take g_mu, which game threads creating pools and the GPU completion handler also take, and search the pool index. Two levels: a table direct-mapped
// by 4 KB page, then the last 8 pools found, which also fill the table. Breath of the Wild binds from more pools a frame than 8 entries alone could hold (about 2300 of its 21000 lookups a frame
// took the lock), and its uniform rings move to a new page every few draws (the table alone missed as often)
enum { PC_BITS = 10, PC_RECENT = 8 };
static _Atomic uint64_t g_pool_gen = 1;
typedef struct { uintptr_t lo, hi, base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; uintptr_t end; int reach; } pc_ent;
static __thread struct { uint64_t gen; unsigned next; pc_ent last, r[PC_RECENT], e[1u << PC_BITS]; } t_pc;   // an empty entry has hi 0; last: the pool of the last hit, tried first (most lookups in a row land in the same pool, and that one entry stays in the L1 cache where the table does not)
static inline unsigned pc_slot(uint64_t addr) { return (unsigned)(((addr >> 12) * 0x9E3779B97F4A7C15ull) >> (64 - PC_BITS)); }
// A lookup of unknown length (1) may read on to its pool's end, so a chunk short of it does not answer one: big_lookup gives it the whole pool
static inline int pc_holds(const pc_ent *c, uint64_t addr, uint64_t len) { return len > 1 ? addr + len <= c->end : c->reach != REACH_SHORT; }
static inline const pc_ent *pc_get(uint64_t addr, uint64_t len) {
    if (t_pc.gen != atomic_load_explicit(&g_pool_gen, memory_order_acquire)) return NULL;
    const pc_ent *l = &t_pc.last;
    if (addr >= l->lo && addr < l->hi && pc_holds(l, addr, len)) return l;
    pc_ent *c = &t_pc.e[pc_slot(addr)];
    if (addr >= c->lo && addr < c->hi && pc_holds(c, addr, len)) { t_pc.last = *c; return c; }
    for (unsigned k = 0; k < PC_RECENT; k++) { const pc_ent *x = &t_pc.r[k]; if (addr >= x->lo && addr < x->hi && pc_holds(x, addr, len)) { *c = *x; t_pc.last = *x; return c; } }
    return NULL;
}
static void pc_put(int i, uint64_t addr) {   // caller holds g_mu, pool i is resident and holds addr
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_relaxed);
    if (t_pc.gen != gen) { memset(&t_pc.last, 0, sizeof t_pc.last); memset(t_pc.r, 0, sizeof t_pc.r); memset(t_pc.e, 0, sizeof t_pc.e); t_pc.gen = gen; }
    pc_ent c = {g_pools[i].lo, g_pools[i].hi, g_pools[i].base, g_pools[i].buf, g_pools[i].gpu, g_pools[i].end, g_pools[i].reach};
    t_pc.e[pc_slot(addr)] = c; t_pc.r[t_pc.next++ % PC_RECENT] = c; t_pc.last = c;
}
static _Atomic uint64_t g_reg_gen = 1;   // Change generation of memory pool / texture / sampler registration: one of the invalidation conditions for argument buffer reuse
// The changes behind g_reg_gen, by generation: a texture or sampler registration (under g_rmu) records the generation it made and its ID, so a pool table (pool_gpu) built at an earlier
// generation can take just those IDs again. One the ring has overwritten makes the table be built whole. A memory pool added or removed (mtl_pool_add / mtl_pool_del) records REG_POOL
// and no ID: it changes no registration, only which storage exists, so a table takes again only the IDs it found without storage. The IDs of a pool that Finalize removes are marked
// gone first (treg_gone), each with a note of its own. Ocarina of Time registers something between most of its draws, and its streamed textures map and unmap virtual pool runs
// (vrun_set) dozens of times a second: each of those walked its 33,025-entry texture table again. Pool changes come from threads that do not hold g_rmu, so a note is a sequence
// lock: its writer clears the generation, writes what changed and stores the generation last (release); a reader takes it only if the generation is the one asked for both before
// and after it reads what changed, so a note the ring overwrites meanwhile reads as overwritten
enum { REG_LOG = 4096, REG_POOL = 2 };
typedef struct { _Atomic uint64_t gen, what; } reg_note;   // what: the ID in the low 32 bits, above them 0 a texture ID, 1 a sampler ID, REG_POOL a memory pool added or removed (no ID)
static reg_note g_reg_log[REG_LOG];
static void reg_bump(int id_, int samp) {
    uint64_t g = atomic_fetch_add(&g_reg_gen, 1) + 1; reg_note *n = &g_reg_log[g % REG_LOG];
    atomic_store_explicit(&n->gen, 0, memory_order_relaxed); atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&n->what, (uint64_t)(uint32_t)id_ | (uint64_t)samp << 32, memory_order_relaxed);
    atomic_store_explicit(&n->gen, g, memory_order_release);
}
static int reg_read(uint64_t g, int *id_, int *samp) {   // → 0 when note g is not in the ring (not written yet, or overwritten)
    reg_note *n = &g_reg_log[g % REG_LOG];
    if (atomic_load_explicit(&n->gen, memory_order_acquire) != g) return 0;
    uint64_t w = atomic_load_explicit(&n->what, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&n->gen, memory_order_relaxed) != g) return 0;
    *id_ = (int)(uint32_t)w; *samp = (int)(w >> 32); return 1;
}
// The raw table admits pools noted by address getters (hle_nvn.c pool_mark_addressed) or reached by fixed pointers read before binding (gmem_discover). Other pools stay out, avoiding residency
// for texture-only storage that Metal keeps in textures of its own. With the original getter-only filter, Ocarina of Time excluded 172 pools of 3,721 MB of the 4,588 MB, its whole 2,432 MB
// graphics pool among them; with them in, the first scene passed 12 GB of Metal's 12.1 GB working set and took a 16 GB Mac into swap, and once into a kernel panic in IOGPUGroupMemory
static struct { uintptr_t lo, hi; } *g_addr; static int g_naddr, g_addr_cap;   // sorted, disjoint. Under g_mu
static int addr_first(uintptr_t lo) { int a = 0, b = g_naddr; while (a < b) { int m = (a + b) / 2; if (g_addr[m].hi <= lo) a = m + 1; else b = m; } return a; }   // the first range that ends after lo
static int addr_meets(uintptr_t lo, uintptr_t hi) { int i = addr_first(lo); return i < g_naddr && g_addr[i].lo < hi; }
void mtl_pool_addressed(const void *mem, uint64_t size) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    pthread_mutex_lock(&g_mu);
    int i = addr_first(lo);
    if (i == g_naddr || g_addr[i].lo > lo || g_addr[i].hi < hi) {
        if (i > 0 && g_addr[i - 1].hi == lo) i--;   // touching on the left
        int j = i;
        while (j < g_naddr && g_addr[j].lo <= hi) { lo = MIN(lo, g_addr[j].lo); hi = MAX(hi, g_addr[j].hi); j++; }
        if (i == j && g_naddr == g_addr_cap) { g_addr_cap = g_addr_cap ? g_addr_cap * 2 : 256; g_addr = dm_realloc(g_addr, (size_t)g_addr_cap * sizeof *g_addr, "the addressed range table"); }
        if (j != i + 1) memmove(g_addr + i + 1, g_addr + j, (size_t)(g_naddr - j) * sizeof *g_addr);
        g_naddr += 1 - (j - i); g_addr[i].lo = lo; g_addr[i].hi = hi;
        atomic_fetch_add(&g_pool_gen, 1);   // the table rebuilds before the next draw that uses it
    }
    pthread_mutex_unlock(&g_mu);
}
// On-demand residency of memory pools: putting a zero-copy buffer into the residency set makes the system read the whole pool into memory and pin it as wired, even a pool the game never uses.
// So a pool is not put into the residency set when created; it is put in, and the residency set committed on the spot, the first time the GPU gets its address through mtl_addr / mtl_gpu_addr -- these are the only two ways the GPU gets pool addresses,
// and committing on the spot is earlier than any command buffer commit that uses it. CPU-side reads of pool contents through mtl_addr trigger it too (conservative, never misses). Caller holds g_mu
static void pool_res(int i) {
    if (g_pools[i].res) return;
    res_add(g_pools[i].buf); res_commit(); g_pools[i].res = 1; g_pool_res_n++;
}
// A large pool in chunks. Metal wires a zero-copy buffer whole while it is in the residency set, so one buffer over a large pool pins all of it, whatever the GPU reads. Mario Kart World's 2,427 MB
// graphics pool: the GPU has touched 1,111 MB by the first race, 326 MB of it in the last 1,500 frames; on a 16 GB Mac the driver then spent half its time wiring and unwiring (3.2 s of a 6 s Metal System
// Trace) with 140 MB free, and frames took 100 ms and more every few frames. A pool of BIG_MIN or more therefore has no buffer of its own: a lookup gets a chunk, a zero-copy buffer over the BIG_S
// stride holding its address and on up to the end of what it asked (rounded up to the stride, plus a stride), made when first asked for. Only what lookups reach is wired. The chunks overlap, as adjacent
// pools' buffers do at shared pages; GPU addresses are not contiguous across them, so a lookup must say how far it reads. One that does not know (length 1: an indirect draw's index buffer, a storage
// buffer whose address the game keeps or pushes) may read on to the pool's end, and takes one buffer of the whole pool, as raw global memory does, whose pool table the shader walks by itself (big_full).
// A guess of 64 MB past the address made a chunk of 80 to 96 MB at each stride such a lookup started in, overlapping each other and each counted whole by Metal: Trails in the Sky 1st Chapter
// had 4,243 MB of shared buffers over its 1,507 MB pool and its command buffers failed out of memory on an M1 (issue #877). From 256 MB: Ocarina of Time's render
// targets live in virtual pools over one 305 MB physical pool, which the raw pool table no longer names (hle_nvn.c tex_bytes_elsewhere); one compute shader still binds a 200 KB
// storage buffer at an address in it every frame, and that lookup wired all 305 MB (docs/reference/memory.md)
enum { BIG_MIN = 256u << 20, BIG_S = 16u << 20, BIG_MAX = 32 };
typedef struct { uintptr_t lo, hi, a, b; const void *mem; int *chunk, full; NSMutableArray *old; } big_pool;   // chunk: stride → g_pools index + 1; full: the whole pool's entry + 1; old: buffers a longer chunk replaced, alive with the pool
static big_pool g_big[BIG_MAX]; static int g_nbig;
static id<MTLBuffer> pool_buf(uintptr_t a, uintptr_t b);   // a pool's zero-copy buffer over [a, b), made over writable pages (below)
static int big_of(uint64_t addr) { for (int i = 0; i < g_nbig; i++) if (addr >= g_big[i].lo && addr < g_big[i].hi) return i; return -1; }
// A pool to put into the table: its zero-copy buffer is made first, outside g_mu (pool_new_prep; it may retry for seconds), and the entry goes in under g_mu (pool_new_put), so a change
// of several pools can be one step for every lookup (mtl_pool_replace)
typedef struct { const void *mem; uint64_t size; uintptr_t lo, hi, a, b; __strong id<MTLBuffer> buf; } pool_new;   // buf nil: in chunks (a large pool)
static int pool_chunks(void) { static int c = -1; if (c < 0) { const char *e = getenv("DM_NVN_POOL_CHUNKS"); c = !e || atoi(e); } return c; }
static id<MTLBuffer> pool_buf_made(const void *mem, uint64_t size, uintptr_t a, uintptr_t b) {
    id<MTLBuffer> buf = pool_buf(a, b);
    // Pokémon Legends: Z-A: twice on the Mac a small pool's buffer was refused although its pages were one read-write VM region, both times while another heavy job (a shader compile,
    // a second game instance) took the machine's memory; every run on an idle machine created all of them. Wiring the pages may fail under memory pressure: retry for up to about 2 s
    for (int t = 0; !buf && t < 12; t++) {
        usleep(10000u << (t < 4 ? t : 4));
        buf = pool_buf(a, b);
        if (buf) MLOG("[RED] memory pool %p+%#llx: the zero-copy buffer was refused, created at retry %d", mem, (unsigned long long)size, t + 1);
    }
    if (!buf) {   // say what the pages looked like: every VM region over the range, with its protection (an unmapped page in the range is the usual cause)
        for (vm_address_t r = a; r < b; ) {
            vm_size_t rs = 0; vm_region_basic_info_data_64_t vi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
            if (vm_region_64(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj) != KERN_SUCCESS) { MLOG("[RED]   no VM region at or after %#llx", (unsigned long long)r); break; }
            MLOG("[RED]   VM region %#llx+%#llx protection %d (max %d)", (unsigned long long)r, (unsigned long long)rs, vi.protection, vi.max_protection);
            r += rs;
        }
        MDIE("memory pool %p+%#llx: failed to create the zero-copy buffer over %#lx..%#lx", mem, (unsigned long long)size, (unsigned long)a, (unsigned long)b);
    }
    return buf;
}
static void pool_new_prep(pool_new *n, const void *mem, uint64_t size) {
    uintptr_t pg = (uintptr_t)getpagesize(), lo = (uintptr_t)mem, hi = lo + size;
    *n = (pool_new){mem, size, lo, hi, lo & ~(pg - 1), (hi + pg - 1) & ~(pg - 1), nil};
    if (size >= BIG_MIN && pool_chunks()) { pthread_mutex_lock(&g_mu); int room = g_nbig < BIG_MAX; pthread_mutex_unlock(&g_mu); if (room) return; }
    n->buf = pool_buf_made(mem, size, n->a, n->b);
}
static void pool_new_put(pool_new *n) {   // caller holds g_mu
    if (!n->buf && g_nbig == BIG_MAX) n->buf = pool_buf_made(n->mem, n->size, n->a, n->b);   // the chunk table filled since pool_new_prep looked
    if (!n->buf) {
        int *ch = calloc(((n->b - n->a) >> 24) + 1, sizeof *ch); if (!ch) MDIE("pool chunk table");
        g_big[g_nbig++] = (big_pool){n->lo, n->hi, n->a, n->b, n->mem, ch, 0, [NSMutableArray array]};
        g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1); atomic_fetch_add(&g_big_n, 1);
        static int nl; if (nl++ < 8) MLOG("memory pool %p+%llu MB: in chunks, wired as far as lookups reach", n->mem, (unsigned long long)(n->size >> 20));
        return;
    }
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = dm_realloc(g_pools, g_cap * sizeof *g_pools, "the memory pool table"); }
    g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1);   // the address index and the per-thread caches are invalid (pool_find, pc_get)
    g_pools[g_npool++] = (pool_ent){n->lo, n->hi, n->a, n->buf, n->buf.gpuAddress, n->mem, 0, clock_gettime_nsec_np(CLOCK_MONOTONIC), n->hi, REACH_END};   // not in the residency set yet: put in the first time it is addressed (pool_res)
    [g_keep addObject:n->buf];
}
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init();   // the change is recorded once the pool can be found (reg_bump below): a reader that sees the new generation sees the pool
    pool_new n; pool_new_prep(&n, mem, size);
    pthread_mutex_lock(&g_mu); pool_new_put(&n); pthread_mutex_unlock(&g_mu);
    reg_bump(-1, REG_POOL);
}

static void big_moved(int i) {   // caller holds g_mu: entry i has a new index
    const pool_ent *e = &g_pools[i];
    for (int k = 0; k < g_nbig; k++) { big_pool *b = &g_big[k]; if (b->mem != e->key) continue;
        if (e->reach == REACH_WHOLE) b->full = i + 1; else b->chunk[(e->lo - b->a) >> 24] = i + 1;
        return; }
}
// The no-copy buffer goes through deferred release (out of the residency set and unreferenced after submitted commands complete). It used to be removed only from g_keep while the residency set still held a strong reference: after the pool was freed the buffer stayed alive wrapping that guest memory,
// and a new pool at the same address wrapped it again. Engage's title demo cycles scenes; GPU allocated grew to 5.4 GB and resident memory to 3.6 GB in 850 s.
// In an autorelease pool of its caller's: this runs on the guest thread that finalizes the pool, whose implicit pool never drains, and adding to the weak table g_dead_pools left the buffer
// autoreleased there, so every finalized pool buffer stayed alive for good (Ghost Trick: Phantom Detective, 125 buffers and 6.6 GB of GPU allocations 300 frames into the boot, 0 with the pool)
static void pool_take_out(const void *mem, NSMutableArray<id<MTLBuffer>> *gone) {   // caller holds g_mu
    int bg = -1; for (int k = 0; k < g_nbig; k++) if (g_big[k].mem == mem) bg = k;
    for (int i = 0; i < g_npool; i++)
        if (g_pools[i].key == mem) { id<MTLBuffer> b = g_pools[i].buf; [g_keep removeObject:b]; [gone addObject:b]; if (g_pools[i].res) g_pool_res_n--;
            if (!g_dead_pools) g_dead_pools = [NSHashTable weakObjectsHashTable];
            [g_dead_pools addObject:b]; g_pools[i] = g_pools[--g_npool]; g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1);
            if (i < g_npool) big_moved(i);   // the entry moved into this place
            if (bg < 0) break;   // a large pool has an entry per chunk
            i--; }
    if (bg >= 0) {
        [gone addObjectsFromArray:g_big[bg].old];
        free(g_big[bg].chunk); g_big[bg] = g_big[--g_nbig]; g_pidx_ok = 0; g_plast = -1; atomic_fetch_add(&g_pool_gen, 1); atomic_fetch_sub(&g_big_n, 1);
    }
}
// Once out of the table and with g_pool_gen bumped, a buffer is reached only by what a submission already encoded: so with none open it waits for the last committed fence, not the
// next one, which a game that stops submitting never reaches (defer_kick). The open flag is read after the bump, and mtl_sub_begin sets it before its lookups, each side with a full
// fence: a submission either is seen open or sees the bump, and then neither the per-thread cache (pc_get, no lock) nor the table gives it this buffer.
// The open flag holds that submission's own fence and is read before g_fence: the commit clears it only after its ++g_fence, so a 0 read means the g_fence read after it counts that
// submission. As g_fence + (open ? 1 : 0), one expression, C left the order of the two reads open: g_fence read before a commit's ++g_fence and the flag after its clear gave the fence
// before that submission's, which may be done already, and defer_kick could free the buffer while the submission ran (PR #2328). A 0/1 flag read first is still one fence late when a
// commit lands between the reads, a fence a game that stops submitting never reaches
static void pool_gone_release(NSMutableArray<id<MTLBuffer>> *gone) {   // caller holds g_mu
    atomic_thread_fence(memory_order_seq_cst);
    uint64_t open = atomic_load(&g_sub_open);
    if (g_pool_gone_between) g_pool_gone_between();
    uint64_t fence = open ? open : atomic_load(&g_fence);
    for (id<MTLBuffer> b in gone) defer_obj_at(b, 0, fence);
}
void mtl_pool_del(const void *mem) {
    reg_bump(-1, REG_POOL);
    @autoreleasepool {
    NSMutableArray<id<MTLBuffer>> *gone = [NSMutableArray array];
    pthread_mutex_lock(&g_mu); pool_take_out(mem, gone); pool_gone_release(gone); pthread_mutex_unlock(&g_mu);
    }
}
// Pools that take the place of others, in one step under g_mu: a lookup finds the old ones or the new ones, never neither. A sparse pool's mapped run is a pool of its own, replaced
// whenever a mapping joins it to its neighbours or an unmap splits it (hle_nvn.c vrun_set), on the game's thread while the replay looks addresses up on its own. Removed and then
// added, an address mapped all along was in no pool between the two: PRAGMATA's mesh shaders 765e6d9d2588ce70 and 5aab38bb5ba67ef7 read a buffer whose address the game pushes,
// 0x3000000 into a sparse pool whose one run kept growing (0x8c5e304000..0x8c7b1a4000, then on), and got a dummy buffer at frames 2971 and 4484, the frames the run was replaced in
void mtl_pool_replace(const void *const *del, int ndel, const void *const *add, const uint64_t *size, int nadd) {
    mtl_init();
    pool_new *n = nadd ? (pool_new *)calloc((size_t)nadd, sizeof *n) : NULL; if (nadd && !n) MDIE("pool replacement");
    for (int i = 0; i < nadd; i++) pool_new_prep(&n[i], add[i], size[i]);
    reg_bump(-1, REG_POOL);
    @autoreleasepool {
    NSMutableArray<id<MTLBuffer>> *gone = [NSMutableArray array];
    pthread_mutex_lock(&g_mu);
    for (int i = 0; i < ndel; i++) pool_take_out(del[i], gone);
    for (int i = 0; i < nadd; i++) pool_new_put(&n[i]);
    pool_gone_release(gone);
    pthread_mutex_unlock(&g_mu);
    }
    for (int i = 0; i < nadd; i++) n[i].buf = nil;   // release the references before the memory goes (a strong member of a calloc'd array)
    free(n);
    reg_bump(-1, REG_POOL);
}

// Address → pool: the last hit first, then a binary search over indices sorted by start address, and a full linear scan only when that fails (pools overlapping).
// The Legend of Zelda: Breath of the Wild keeps about 1760 memory pools alive and every buffer binding of every draw looks one up; the old full scan took about 4% of the submit thread's samples (mtl_gpu_addr 222 / 6 s). Adding / removing pools invalidates the index, which is re-sorted when next used. Caller holds g_mu
static int *g_pidx; static int g_pidx_cap;
static int pidx_cmp(const void *a, const void *b) { uintptr_t x = g_pools[*(const int *)a].lo, y = g_pools[*(const int *)b].lo; return x < y ? -1 : x > y; }
static void pidx_sort(void) {
    if (g_pidx_ok) return;
    if (g_pidx_cap < g_npool) { g_pidx_cap = g_npool * 2; g_pidx = dm_realloc(g_pidx, (size_t)g_pidx_cap * sizeof *g_pidx, "the memory pool index"); }
    for (int i = 0; i < g_npool; i++) g_pidx[i] = i;
    qsort(g_pidx, (size_t)g_npool, sizeof *g_pidx, pidx_cmp); g_pidx_ok = 1;
}
static int big_new(big_pool *b, uintptr_t lo, uintptr_t hi, uintptr_t end, int reach) {   // caller holds g_mu: a new entry for [lo, end) of the pool, looked up for addresses in [lo, hi)
    id<MTLBuffer> buf = pool_buf(lo, end);
    if (!buf) { MLOG("[RED] memory pool %p: the zero-copy buffer over %#lx..%#lx was refused", b->mem, (unsigned long)lo, (unsigned long)end); return -1; }
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = dm_realloc(g_pools, g_cap * sizeof *g_pools, "the memory pool table"); }
    g_pidx_ok = 0; atomic_fetch_add(&g_pool_gen, 1);
    g_pools[g_npool++] = (pool_ent){lo < b->lo ? b->lo : lo, hi > b->hi ? b->hi : hi, lo, buf, buf.gpuAddress, b->mem, 0, clock_gettime_nsec_np(CLOCK_MONOTONIC), end > b->hi ? b->hi : end, reach};
    [g_keep addObject:buf];
    return g_npool - 1;
}
// The entry of a large pool for [addr, addr + len): the chunk of its stride, made or lengthened to reach as far as asked, or the whole pool for a lookup of unknown length; -1 past the pool's end.
// The chunks made before the whole pool's buffer stay, resident: what was encoded with them still reads through them
static int big_full(big_pool *b, const char *why) {
    int n = big_new(b, b->a, b->b, b->b, REACH_WHOLE);
    if (n < 0) return -1;
    b->full = n + 1;
    static int nl; if (nl++ < 16) { int c = 0; uint64_t mb = 0; for (int i = 0; i < g_npool; i++) if (i != n && g_pools[i].key == b->mem) { c++; mb += g_pools[i].buf.length; }
        for (id<MTLBuffer> o in b->old) { c++; mb += o.length; }
        MLOG("memory pool %p+%llu MB: one buffer of the whole pool (%s); the chunks made before it stay: %d, %llu MB", b->mem, (unsigned long long)((b->hi - b->lo) >> 20), why, c, (unsigned long long)(mb >> 20)); }
    return n;
}
static int big_lookup(big_pool *b, uint64_t addr, uint64_t len) {
    if (addr + len > b->hi) return -1;
    if (b->full) return b->full - 1;
    if (len <= 1) return big_full(b, "a lookup of unknown length");
    uintptr_t k = (addr - b->a) >> 24, lo = b->a + (k << 24);
    uintptr_t end = (addr + len + BIG_S - 1) & ~(uintptr_t)(BIG_S - 1); end += BIG_S; if (end > b->b || end < lo) end = b->b;
    int i = b->chunk[k] - 1;
    uintptr_t have = i >= 0 ? g_pools[i].end : 0;
    if (i >= 0 && (have >= b->hi || have >= addr + len)) return i;
    int n = big_new(b, lo, lo + BIG_S, end, end >= b->b ? REACH_END : REACH_SHORT); if (n < 0) return -1;
    if (i >= 0) {   // lengthened: what is encoded with the shorter chunk keeps it, resident, until the pool goes
        [b->old addObject:g_pools[i].buf]; [g_keep removeObject:g_pools[i].buf]; g_pools[n].res = 0; if (g_pools[i].res) g_pool_res_n--;
        g_pools[i] = g_pools[n]; g_npool--; n = i;
    }
    b->chunk[k] = n + 1;
    return n;
}
static int pool_held(uint64_t addr, uint64_t len) {   // the entry of g_pools holding [addr, addr + len), made by no lookup: an ordinary pool, or an existing chunk
    if (len > UINT64_MAX - addr) return -1;   // an overflowing exclusive end cannot lie in a pool, including the cached one
    if (g_plast >= 0 && g_plast < g_npool && addr >= g_pools[g_plast].lo && addr + len <= g_pools[g_plast].hi) return g_plast;
    pidx_sort();
    int lo = 0, hi = g_npool - 1, hit = -1;   // the last one whose start ≤ addr
    while (lo <= hi) { int m = (lo + hi) / 2; if (g_pools[g_pidx[m]].lo <= addr) { hit = m; lo = m + 1; } else hi = m - 1; }
    if (hit >= 0 && addr + len <= g_pools[g_pidx[hit]].hi) return g_plast = g_pidx[hit];
    for (int i = 0; i < g_npool; i++) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) return g_plast = i;
    return -1;
}
// The entry for a GPU use of [addr, addr + len): a large pool's chunk is made or lengthened to reach as far as asked (big_lookup)
static int pool_find(uint64_t addr, uint64_t len) {
    if (g_nbig) { int b = big_of(addr); if (b >= 0) return big_lookup(&g_big[b], addr, len); }
    return pool_held(addr, len);
}
// The entry that holds addr, or -1, making no buffer: a large pool answers with its stride's chunk if it has one. For questions no GPU use follows: through pool_find the pool walk's log
// asked about every texture's storage and made a chunk of 80 MB and more for each stride (then 64 MB past the address), or lengthened its chunk to that, 77 of them, 6.7 GB counted
// by Metal, in Ocarina of Time's first area load. Caller holds g_mu
static int pool_peek(uint64_t addr) {
    if (g_nbig) { int b = big_of(addr); if (b >= 0) { const big_pool *bp = &g_big[b]; return bp->full ? bp->full - 1 : bp->chunk[(addr - bp->a) >> 24] - 1; } }
    return pool_held(addr, 1);
}
// Whether [addr, addr + len) lies in a pool, with no side effect: mtl_addr makes the pool resident, for a GPU use to follow. A large pool is answered from its range, since its lookup
// (big_lookup) makes the chunk that holds the range. hle_nvn.c keys_hash asks this whether a shader's code is mapped: through mtl_addr every pool holding shader code went into the
// residency set, though the GPU never reads it (the shaders run translated): The Great Ace Attorney Chronicles' three chunks, about 5 MB
int mtl_addr_known(uint64_t addr, uint64_t len) {
    pthread_mutex_lock(&g_mu); int b = big_of(addr), ok = b >= 0 ? addr + len <= g_big[b].hi : pool_find(addr, len) >= 0; pthread_mutex_unlock(&g_mu);
    return ok;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    const pc_ent *c = pc_get(addr, len);
    if (c) { *buf = (__bridge void *)c->buf; *off = addr - c->base; return 1; }
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    int i = pool_find(addr, len);
    if (i >= 0) { if (t_tex_upload_lookup) g_pools[i].n_tex_reads++; else g_pools[i].n_other++; }
    if (i >= 0) { pool_res(i); pc_put(i, addr); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) {
    const pc_ent *c = pc_get(addr, len);
    if (c) { *buf = (__bridge void *)c->buf; *off = addr - c->base; *gpu = c->gpu + *off; return 1; }
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    int i = pool_find(addr, len);
    if (i >= 0) { if (t_tex_upload_lookup) g_pools[i].n_tex_reads++; else g_pools[i].n_other++; }
    if (i >= 0) { pool_res(i); pc_put(i, addr); *buf = (__bridge void *)g_pools[i].buf; *off = addr - g_pools[i].base; *gpu = g_pools[i].gpu + *off; ok = 1; }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
