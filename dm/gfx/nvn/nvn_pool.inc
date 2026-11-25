// Device start (mtl_init) and memory pools: the address-to-pool table, zero-copy buffers, large pools in chunks, residency on demand.
// REACH_SHORT: a chunk that ends before its pool does, which answers only lookups that say how far they read
enum { REACH_WHOLE, REACH_END, REACH_SHORT };
typedef struct { uintptr_t lo, hi; uintptr_t base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; const void *key; int res; uint64_t born; uintptr_t end; int reach; uint32_t n_tex_reads, n_other; } pool_ent;
static pool_ent *g_pools;
static int g_npool, g_cap;
static int g_pidx_ok, g_plast = -1;
static NSMutableArray *g_keep;   // pool_ent's pointers do not own
static int g_pool_res_n;   // under g_mu
static __thread int t_tex_upload_lookup;

#define C1_BANK_SIZE (4u << 20)
#define C1_DECL_SIZE (64u << 10)
static NSMutableArray *g_c1_banks;
static size_t g_c1_bank_off;
static uint64_t g_c1_count;
static NSMutableSet<NSString *> *g_c1_full;
static int g_c1_full_valid;
static NSDictionary<NSString *, NSString *> *g_cb_ext;   // shader key → "reg:bytes ..." (how far into each uniform slot the shader can read); nil when stale or missing
static os_unfair_lock g_c1_lock = OS_UNFAIR_LOCK_INIT;

// DM_UDC_SELFTEST=1 (diagnostic): DontCare load / store without drawing, checked pixel by pixel
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
            if (depth) {
                rp.depthAttachment.texture = t; rp.depthAttachment.loadAction = la; rp.depthAttachment.storeAction = sa; rp.depthAttachment.clearDepth = 0.3125;
                rp.stencilAttachment.texture = t; rp.stencilAttachment.loadAction = la; rp.stencilAttachment.storeAction = sa; rp.stencilAttachment.clearStencil = 0x5a;
            } else {
                rp.colorAttachments[0].texture = t; rp.colorAttachments[0].loadAction = la; rp.colorAttachments[0].storeAction = sa;
                rp.colorAttachments[0].clearColor = MTLClearColorMake(0.2, 0.4, 0.6, 0.8);
            }
            [[cb renderCommandEncoderWithDescriptor:rp] endEncoding];
        }
        id<MTLBuffer> out = [g_dev newBufferWithLength:w * h * 4 options:MTLResourceStorageModeShared];
        id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:out destinationOffset:0
            destinationBytesPerRow:w * 4 destinationBytesPerImage:w * h * 4 options:depth ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        const uint32_t *px = out.contents;
        NSUInteger bad = 0;
        for (NSUInteger i = 0; i < w * h; i++) if (px[i] != px[0]) bad++;
        MLOG("UDC self-check %s %s %lux%lu first value %08x, values differing from it %lu / %lu", g_dev.name.UTF8String, depth ? "Depth32F_S8" : "RGBA8", (unsigned long)w, (unsigned long)h, px[0], (unsigned long)bad, (unsigned long)(w * h));
    }
}
static void mtl_init_once(void) {
    const char *vbc = getenv("DM_NVN_VBUF_CACHE");
    g_vbuf_cache = !(vbc && !strcmp(vbc, "0"));
    // DM_PRESENT_LINK: 1 (default) every tick of a 60 Hz display link; 2 every other tick of a 120 Hz link; 0 off
    const char *link = getenv("DM_PRESENT_LINK");
    g_dm_present_link = link ? atoi(link) : 1;
    if (g_dm_present_link < 0 || g_dm_present_link > 2) g_dm_present_link = 1;
    g_present_link_env = link != NULL;
    const char *latency = getenv("DM_PRESENT_LATENCY");
    if (latency && atoi(latency) > 0) g_present_latency = atoi(latency);
    g_res_check_on = getenv("DM_RES_CHECK") != NULL;
    g_dev = MTLCreateSystemDefaultDevice();
    if (!g_dev) MDIE("no Metal device");
    if (!g_dev.hasUnifiedMemory) MDIE("needs a unified-memory device (zero-copy memory pools)");
    mem_pressure_start();
    g_q = [g_dev newCommandQueue];
    // without Metal 4 (a virtual Mac's paravirtualized GPU) the NVN layer cannot run
    NSError *re = nil; const char *no4 = NULL;
    if (![g_dev supportsFamily:MTLGPUFamilyMetal4]) no4 = "no Metal 4 GPU family";
    else {
        MTLResidencySetDescriptor *rd = [MTLResidencySetDescriptor new];
        rd.initialCapacity = 4096;
        g_res = [g_dev newResidencySetWithDescriptor:rd error:&re];
        rd.initialCapacity = 8;
        if (g_res) g_snap_res = [g_dev newResidencySetWithDescriptor:rd error:&re];
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
    // BC textures are uploaded as BC with no decompression path (dm/gfx/tex/README.md): stop a device without BC at the start.
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
    g_vt = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]);
    if (!g_vt) MDIE("failed to create the vertex argument table: %s", ate.localizedDescription.UTF8String);
    g_vt_valid = 0;
    g_ft = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]);
    if (!g_ft) MDIE("failed to create the fragment argument table: %s", ate.localizedDescription.UTF8String);
    g_ct = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]);
    if (!g_ct) MDIE("failed to create the compute argument table: %s", ate.localizedDescription.UTF8String);
    g_ot = rec_table([g_dev newArgumentTableWithDescriptor:atd error:&ate]);
    if (!g_ot) MDIE("failed to create the object argument table: %s", ate.localizedDescription.UTF8String);
    g_up_ct = [g_dev newArgumentTableWithDescriptor:atd error:&ate];
    if (!g_up_ct) MDIE("failed to create the upload compute argument table: %s", ate.localizedDescription.UTF8String);
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
    // pool registration and the shutdown drain can arrive together on a cold device
    static dispatch_once_t once;
    dispatch_once(&once, ^{ mtl_init_once(); });
}

// No-copy buffers of finalized pools (weak references, under g_mu), for the memory report.
static NSHashTable *g_dead_pools;
// Per-thread address → pool cache, valid while g_pool_gen is unchanged, holding only resident pools: a lookup takes no g_mu.
// The last pool found, a table direct-mapped by 64 KB region (w), one by 4 KB page (e), then the last PC_RECENT pools found. The region table finds a large
// pool from any page of a region already seen (docs/reference/frame-rate.md, "Pool lookups by page missed with thousands of pools").
enum { PC_BITS = 10, PC_RECENT = 8 };
static _Atomic uint64_t g_pool_gen = 1;
typedef struct { uintptr_t lo, hi, base; __unsafe_unretained id<MTLBuffer> buf; uint64_t gpu; uintptr_t end; int reach; } pc_ent;
static __thread struct { uint64_t gen; unsigned next; pc_ent last, r[PC_RECENT], e[1u << PC_BITS], w[1u << PC_BITS]; } t_pc;
static inline unsigned pc_slot(uint64_t addr) { return (unsigned)(((addr >> 12) * 0x9E3779B97F4A7C15ull) >> (64 - PC_BITS)); }
static inline unsigned pc_slot_w(uint64_t addr) { return (unsigned)(((addr >> 16) * 0x9E3779B97F4A7C15ull) >> (64 - PC_BITS)); }
// a lookup of unknown length (1) may read on to its pool's end, so a short chunk does not answer it
static inline int pc_holds(const pc_ent *c, uint64_t addr, uint64_t len) { return len > 1 ? addr + len <= c->end : c->reach != REACH_SHORT; }
static inline const pc_ent *pc_get(uint64_t addr, uint64_t len) {
    if (t_pc.gen != atomic_load_explicit(&g_pool_gen, memory_order_acquire)) return NULL;
    const pc_ent *l = &t_pc.last;
    if (addr >= l->lo && addr < l->hi && pc_holds(l, addr, len)) return l;
    pc_ent *w = &t_pc.w[pc_slot_w(addr)];
    if (addr >= w->lo && addr < w->hi && pc_holds(w, addr, len)) { t_pc.last = *w; return w; }
    pc_ent *c = &t_pc.e[pc_slot(addr)];
    if (addr >= c->lo && addr < c->hi && pc_holds(c, addr, len)) { t_pc.last = *c; return c; }
    for (unsigned k = 0; k < PC_RECENT; k++) {
        const pc_ent *x = &t_pc.r[k];
        if (addr >= x->lo && addr < x->hi && pc_holds(x, addr, len)) { *c = *x; *w = *x; t_pc.last = *x; return c; }
    }
    return NULL;
}
static void pc_put(int i, uint64_t addr) {   // caller holds g_mu, pool i is resident and holds addr
    uint64_t gen = atomic_load_explicit(&g_pool_gen, memory_order_relaxed);
    if (t_pc.gen != gen) {
        memset(&t_pc.last, 0, sizeof t_pc.last);
        memset(t_pc.r, 0, sizeof t_pc.r);
        memset(t_pc.e, 0, sizeof t_pc.e);
        memset(t_pc.w, 0, sizeof t_pc.w);
        t_pc.gen = gen;
    }
    pc_ent c = {g_pools[i].lo, g_pools[i].hi, g_pools[i].base, g_pools[i].buf, g_pools[i].gpu, g_pools[i].end, g_pools[i].reach};
    t_pc.e[pc_slot(addr)] = c; t_pc.w[pc_slot_w(addr)] = c; t_pc.r[t_pc.next++ % PC_RECENT] = c; t_pc.last = c;
}
static _Atomic uint64_t g_reg_gen = 1;
// The changes behind g_reg_gen, by generation, so a pool table (pool_gpu) can take just those IDs again; an overwritten note makes it rebuild whole.
// A note is a sequence lock (pool changes come without g_rmu): the writer stores the generation last (release); a reader checks it before and after.
enum { REG_LOG = 4096, REG_POOL = 2 };
typedef struct { _Atomic uint64_t gen, what; } reg_note;   // what: the ID in the low 32 bits; above them 0 texture, 1 sampler, REG_POOL a pool added or removed (no ID)
static reg_note g_reg_log[REG_LOG];
static void reg_bump(int id_, int samp) {
    uint64_t g = atomic_fetch_add(&g_reg_gen, 1) + 1;
    reg_note *n = &g_reg_log[g % REG_LOG];
    atomic_store_explicit(&n->gen, 0, memory_order_relaxed);
    atomic_thread_fence(memory_order_release);
    atomic_store_explicit(&n->what, (uint64_t)(uint32_t)id_ | (uint64_t)samp << 32, memory_order_relaxed);
    atomic_store_explicit(&n->gen, g, memory_order_release);
}
static int reg_read(uint64_t g, int *id_, int *samp) {   // 0 when note g is not in the ring
    reg_note *n = &g_reg_log[g % REG_LOG];
    if (atomic_load_explicit(&n->gen, memory_order_acquire) != g) return 0;
    uint64_t w = atomic_load_explicit(&n->what, memory_order_relaxed);
    atomic_thread_fence(memory_order_acquire);
    if (atomic_load_explicit(&n->gen, memory_order_relaxed) != g) return 0;
    *id_ = (int)(uint32_t)w; *samp = (int)(w >> 32);
    return 1;
}
// The raw table admits only pools noted by address getters (pool_mark_addressed) or reached by fixed pointers (gmem_discover): every other pool in it would be wired resident.
static struct { uintptr_t lo, hi; } *g_addr;   // sorted, disjoint; under g_mu
static int g_naddr, g_addr_cap;
static int addr_first(uintptr_t lo) {
    int a = 0, b = g_naddr;
    while (a < b) {
        int m = (a + b) / 2;
        if (g_addr[m].hi <= lo) a = m + 1; else b = m;
    }
    return a;
}
static int addr_meets(uintptr_t lo, uintptr_t hi) { int i = addr_first(lo); return i < g_naddr && g_addr[i].lo < hi; }
void mtl_pool_addressed(const void *mem, uint64_t size) {
    uintptr_t lo = (uintptr_t)mem, hi = lo + size;
    pthread_mutex_lock(&g_mu);
    int i = addr_first(lo);
    if (i == g_naddr || g_addr[i].lo > lo || g_addr[i].hi < hi) {
        if (i > 0 && g_addr[i - 1].hi == lo) i--;
        int j = i;
        while (j < g_naddr && g_addr[j].lo <= hi) { lo = MIN(lo, g_addr[j].lo); hi = MAX(hi, g_addr[j].hi); j++; }
        if (i == j && g_naddr == g_addr_cap) {
            g_addr_cap = g_addr_cap ? g_addr_cap * 2 : 256;
            g_addr = dm_realloc(g_addr, (size_t)g_addr_cap * sizeof *g_addr, "the addressed range table");
        }
        if (j != i + 1) memmove(g_addr + i + 1, g_addr + j, (size_t)(g_naddr - j) * sizeof *g_addr);
        g_naddr += 1 - (j - i); g_addr[i].lo = lo; g_addr[i].hi = hi;
        atomic_fetch_add(&g_pool_gen, 1);
    }
    pthread_mutex_unlock(&g_mu);
}
// A zero-copy buffer in the residency set wires its whole pool, so a pool goes in on the first mtl_addr / mtl_gpu_addr of it, the only ways the GPU gets pool addresses.
// Caller holds g_mu.
static void pool_res(int i) {
    if (g_pools[i].res) return;
    res_add(g_pools[i].buf);
    res_commit();
    g_pools[i].res = 1;
    g_pool_res_n++;
}
// Metal wires a resident zero-copy buffer whole: a pool of BIG_MIN or more gets chunks from the BIG_S stride to as far as a lookup asks, so only what is reached is wired
// (docs/reference/memory.md). GPU addresses are not contiguous across chunks; a lookup of unknown length takes one buffer of the whole pool (big_full).
enum { BIG_MIN = 256u << 20, BIG_S = 16u << 20, BIG_MAX = 32 };
typedef struct { uintptr_t lo, hi, a, b; const void *mem; int *chunk, full; NSMutableArray *old; } big_pool;   // chunk: stride → g_pools index + 1; full: the whole pool's entry + 1; old: replaced chunks, alive with the pool
static big_pool g_big[BIG_MAX];
static int g_nbig;
static id<MTLBuffer> pool_buf(uintptr_t a, uintptr_t b);
static int big_of(uint64_t addr) {
    for (int i = 0; i < g_nbig; i++) if (addr >= g_big[i].lo && addr < g_big[i].hi) return i;
    return -1;
}
static void pool_table_room(void) {
    if (g_npool == g_cap) { g_cap = g_cap ? g_cap * 2 : 64; g_pools = dm_realloc(g_pools, g_cap * sizeof *g_pools, "the memory pool table"); }
}
static void pool_index_stale(void) {
    g_pidx_ok = 0;
    g_plast = -1;
    atomic_fetch_add(&g_pool_gen, 1);
}
// The buffer is made outside g_mu (it may retry for seconds) and the entry goes in under it, so a change of several pools is one step (mtl_pool_replace).
typedef struct { const void *mem; uint64_t size; uintptr_t lo, hi, a, b; __strong id<MTLBuffer> buf; } pool_new;
static int pool_chunks(void) {
    static int c = -1;
    if (c < 0) {
        const char *e = getenv("DM_NVN_POOL_CHUNKS");
        c = !e || atoi(e);
    }
    return c;
}
static id<MTLBuffer> pool_buf_made(const void *mem, uint64_t size, uintptr_t a, uintptr_t b) {
    id<MTLBuffer> buf = pool_buf(a, b);
    // wiring may fail while another job takes the machine's memory: retry for about 2 s
    for (int t = 0; !buf && t < 12; t++) {
        usleep(10000u << (t < 4 ? t : 4));
        buf = pool_buf(a, b);
        if (buf) MLOG("[RED] memory pool %p+%#llx: the zero-copy buffer was refused, created at retry %d", mem, (unsigned long long)size, t + 1);
    }
    if (!buf) {   // an unmapped page in the range is the usual cause
        for (vm_address_t r = a; r < b; ) {
            vm_size_t rs = 0; vm_region_basic_info_data_64_t vi; mach_msg_type_number_t cnt = VM_REGION_BASIC_INFO_COUNT_64; mach_port_t obj = MACH_PORT_NULL;
            if (vm_region_64(mach_task_self(), &r, &rs, VM_REGION_BASIC_INFO_64, (vm_region_info_t)&vi, &cnt, &obj) != KERN_SUCCESS) {
                MLOG("[RED]   no VM region at or after %#llx", (unsigned long long)r);
                break;
            }
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
    if (size >= BIG_MIN && pool_chunks()) {
        pthread_mutex_lock(&g_mu);
        int room = g_nbig < BIG_MAX;
        pthread_mutex_unlock(&g_mu);
        if (room) return;
    }
    n->buf = pool_buf_made(mem, size, n->a, n->b);
}
static void pool_new_put(pool_new *n) {   // caller holds g_mu
    if (!n->buf && g_nbig == BIG_MAX) n->buf = pool_buf_made(n->mem, n->size, n->a, n->b);
    if (!n->buf) {
        int *ch = calloc(((n->b - n->a) >> 24) + 1, sizeof *ch);
        if (!ch) MDIE("pool chunk table");
        g_big[g_nbig++] = (big_pool){n->lo, n->hi, n->a, n->b, n->mem, ch, 0, [NSMutableArray array]};
        pool_index_stale();
        atomic_fetch_add(&g_big_n, 1);
        static int nl;
        if (nl++ < 8) MLOG("memory pool %p+%llu MB: in chunks, wired as far as lookups reach", n->mem, (unsigned long long)(n->size >> 20));
        return;
    }
    pool_table_room();
    pool_index_stale();
    g_pools[g_npool++] = (pool_ent){n->lo, n->hi, n->a, n->buf, n->buf.gpuAddress, n->mem, 0, clock_gettime_nsec_np(CLOCK_MONOTONIC), n->hi, REACH_END};   // resident only once addressed (pool_res)
    [g_keep addObject:n->buf];
}
void mtl_pool_add(const void *mem, uint64_t size) {
    mtl_init();   // recorded once the pool can be found: a reader that sees the new generation sees the pool
    pool_new n;
    pool_new_prep(&n, mem, size);
    pthread_mutex_lock(&g_mu);
    pool_new_put(&n);
    pthread_mutex_unlock(&g_mu);
    reg_bump(-1, REG_POOL);
}

static void big_moved(int i) {   // caller holds g_mu
    const pool_ent *e = &g_pools[i];
    for (int k = 0; k < g_nbig; k++) {
        big_pool *b = &g_big[k];
        if (b->mem != e->key) continue;
        if (e->reach == REACH_WHOLE) b->full = i + 1; else b->chunk[(e->lo - b->a) >> 24] = i + 1;
        return;
    }
}
// Deferred release: a buffer only dropped from g_keep stays resident over the freed guest memory. In the caller's autorelease pool: the guest thread's implicit pool never drains.
static void pool_take_out(const void *mem, NSMutableArray<id<MTLBuffer>> *gone) {   // caller holds g_mu
    int bg = -1;
    for (int k = 0; k < g_nbig; k++) if (g_big[k].mem == mem) bg = k;
    for (int i = 0; i < g_npool; i++) {
        if (g_pools[i].key != mem) continue;
        id<MTLBuffer> b = g_pools[i].buf;
        [g_keep removeObject:b];
        [gone addObject:b];
        if (g_pools[i].res) g_pool_res_n--;
        if (!g_dead_pools) g_dead_pools = [NSHashTable weakObjectsHashTable];
        [g_dead_pools addObject:b];
        g_pools[i] = g_pools[--g_npool];
        pool_index_stale();
        if (i < g_npool) big_moved(i);
        if (bg < 0) break;
        i--;
    }
    if (bg >= 0) {
        [gone addObjectsFromArray:g_big[bg].old];
        free(g_big[bg].chunk);
        g_big[bg] = g_big[--g_nbig];
        pool_index_stale();
        atomic_fetch_sub(&g_big_n, 1);
    }
}
// Out of the table and past the g_pool_gen bump, a buffer is reached only by encoded submissions: with none open it waits for the last committed fence (defer_kick).
// Read the open flag (after the bump, full fence) before g_fence, in separate statements: the commit clears it only after its ++g_fence, and one expression leaves the order open.
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
    pthread_mutex_lock(&g_mu);
    pool_take_out(mem, gone);
    pool_gone_release(gone);
    pthread_mutex_unlock(&g_mu);
    }
}
// Replaced in one step under g_mu, so a lookup finds the old pools or the new, never neither: a sparse pool's mapped run is replaced on the game's thread while the replay looks up.
void mtl_pool_replace(const void *const *del, int ndel, const void *const *add, const uint64_t *size, int nadd) {
    mtl_init();
    pool_new *n = nadd ? (pool_new *)calloc((size_t)nadd, sizeof *n) : NULL;
    if (nadd && !n) MDIE("pool replacement");
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
    for (int i = 0; i < nadd; i++) n[i].buf = nil;   // a strong member of a calloc'd array
    free(n);
    reg_bump(-1, REG_POOL);
}

// The last hit, then a binary search over indices sorted by start, a linear scan only when pools overlap. Caller holds g_mu.
static int *g_pidx;
static int g_pidx_cap;
static int pidx_cmp(const void *a, const void *b) {
    uintptr_t x = g_pools[*(const int *)a].lo, y = g_pools[*(const int *)b].lo;
    return x < y ? -1 : x > y;
}
static void pidx_sort(void) {
    if (g_pidx_ok) return;
    if (g_pidx_cap < g_npool) {
        g_pidx_cap = g_npool * 2;
        g_pidx = dm_realloc(g_pidx, (size_t)g_pidx_cap * sizeof *g_pidx, "the memory pool index");
    }
    for (int i = 0; i < g_npool; i++) g_pidx[i] = i;
    qsort(g_pidx, (size_t)g_npool, sizeof *g_pidx, pidx_cmp);
    g_pidx_ok = 1;
}
static int big_new(big_pool *b, uintptr_t lo, uintptr_t hi, uintptr_t end, int reach) {   // caller holds g_mu
    id<MTLBuffer> buf = pool_buf(lo, end);
    if (!buf) {
        MLOG("[RED] memory pool %p: the zero-copy buffer over %#lx..%#lx was refused", b->mem, (unsigned long)lo, (unsigned long)end);
        return -1;
    }
    pool_table_room();
    g_pidx_ok = 0; atomic_fetch_add(&g_pool_gen, 1);
    g_pools[g_npool++] = (pool_ent){lo < b->lo ? b->lo : lo, hi > b->hi ? b->hi : hi, lo, buf, buf.gpuAddress, b->mem, 0, clock_gettime_nsec_np(CLOCK_MONOTONIC), end > b->hi ? b->hi : end, reach};
    [g_keep addObject:buf];
    return g_npool - 1;
}
// -1 past the pool's end. Chunks made before the whole pool's buffer stay resident: what was encoded with them still reads through them.
static int big_full(big_pool *b, const char *why) {
    int n = big_new(b, b->a, b->b, b->b, REACH_WHOLE);
    if (n < 0) return -1;
    b->full = n + 1;
    static int nl;
    if (nl++ < 16) {
        int c = 0;
        uint64_t mb = 0;
        for (int i = 0; i < g_npool; i++) if (i != n && g_pools[i].key == b->mem) { c++; mb += g_pools[i].buf.length; }
        for (id<MTLBuffer> o in b->old) { c++; mb += o.length; }
        MLOG("memory pool %p+%llu MB: one buffer of the whole pool (%s); the chunks made before it stay: %d, %llu MB", b->mem, (unsigned long long)((b->hi - b->lo) >> 20), why, c, (unsigned long long)(mb >> 20));
    }
    return n;
}
static int big_lookup(big_pool *b, uint64_t addr, uint64_t len) {
    if (addr + len > b->hi) return -1;
    if (b->full) return b->full - 1;
    if (len <= 1) return big_full(b, "a lookup of unknown length");
    uintptr_t k = (addr - b->a) >> 24, lo = b->a + (k << 24);
    uintptr_t end = (addr + len + BIG_S - 1) & ~(uintptr_t)(BIG_S - 1);
    end += BIG_S;
    if (end > b->b || end < lo) end = b->b;
    int i = b->chunk[k] - 1;
    uintptr_t have = i >= 0 ? g_pools[i].end : 0;
    if (i >= 0 && (have >= b->hi || have >= addr + len)) return i;
    int n = big_new(b, lo, lo + BIG_S, end, end >= b->b ? REACH_END : REACH_SHORT);
    if (n < 0) return -1;
    if (i >= 0) {   // what is encoded with the shorter chunk keeps it until the pool goes
        [b->old addObject:g_pools[i].buf];
        [g_keep removeObject:g_pools[i].buf];
        g_pools[n].res = 0;
        if (g_pools[i].res) g_pool_res_n--;
        g_pools[i] = g_pools[n];
        g_npool--;
        n = i;
    }
    b->chunk[k] = n + 1;
    return n;
}
static int pool_held(uint64_t addr, uint64_t len) {
    if (len > UINT64_MAX - addr) return -1;   // an overflowing exclusive end cannot lie in a pool
    if (g_plast >= 0 && g_plast < g_npool && addr >= g_pools[g_plast].lo && addr + len <= g_pools[g_plast].hi) return g_plast;
    pidx_sort();
    int lo = 0, hi = g_npool - 1, hit = -1;
    while (lo <= hi) {
        int m = (lo + hi) / 2;
        if (g_pools[g_pidx[m]].lo <= addr) { hit = m; lo = m + 1; } else hi = m - 1;
    }
    if (hit >= 0 && addr + len <= g_pools[g_pidx[hit]].hi) return g_plast = g_pidx[hit];
    for (int i = 0; i < g_npool; i++) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) return g_plast = i;
    return -1;
}
static int pool_find(uint64_t addr, uint64_t len) {
    if (g_nbig) {
        int b = big_of(addr);
        if (b >= 0) return big_lookup(&g_big[b], addr, len);
    }
    return pool_held(addr, len);
}
// Makes no buffer, for questions no GPU use follows (a chunk per texture's stride would be counted by Metal). Caller holds g_mu.
static int pool_peek(uint64_t addr) {
    if (g_nbig) {
        int b = big_of(addr);
        if (b >= 0) {
            const big_pool *bp = &g_big[b];
            return bp->full ? bp->full - 1 : bp->chunk[(addr - bp->a) >> 24] - 1;
        }
    }
    return pool_held(addr, 1);
}
// No side effect (mtl_addr makes the pool resident); a large pool is answered from its range.
int mtl_addr_known(uint64_t addr, uint64_t len) {
    pthread_mutex_lock(&g_mu);
    int b = big_of(addr), ok = b >= 0 ? addr + len <= g_big[b].hi : pool_find(addr, len) >= 0;
    pthread_mutex_unlock(&g_mu);
    return ok;
}
// gpu may be NULL
static int pool_lookup(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) {
    const pc_ent *c = pc_get(addr, len);
    if (c) {
        *buf = (__bridge void *)c->buf;
        *off = addr - c->base;
        if (gpu) *gpu = c->gpu + *off;
        return 1;
    }
    int ok = 0;
    pthread_mutex_lock(&g_mu);
    int i = pool_find(addr, len);
    if (i >= 0) {
        if (t_tex_upload_lookup) g_pools[i].n_tex_reads++; else g_pools[i].n_other++;
        pool_res(i);
        pc_put(i, addr);
        *buf = (__bridge void *)g_pools[i].buf;
        *off = addr - g_pools[i].base;
        if (gpu) *gpu = g_pools[i].gpu + *off;
        ok = 1;
    }
    pthread_mutex_unlock(&g_mu);
    return ok;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { return pool_lookup(addr, len, buf, off, NULL); }
int mtl_gpu_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off, uint64_t *gpu) { return pool_lookup(addr, len, buf, off, gpu); }
