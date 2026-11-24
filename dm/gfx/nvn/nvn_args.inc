// Argument buffers: image slots, raw global memory, handles in uniform memory, sample positions, the bounds of uniform and storage slots, hang reports, encode_args, and the shared index buffers of quads and fans.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// The texture of an image slot (a view of the shader's class and type when the texture's differ, img_fit), else the write sink. Caller holds g_rmu
static id<MTLTexture> img_binding(int tid, const nvn_rbind *b, uint64_t *rid, int *real) {
    void *tv = NULL; *rid = tid ? tex_fast_rid(tid, &tv) : 0;
    id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
    int swz_only = t && !(t.usage & MTLTextureUsageShaderWrite) && (t.parentTexture.usage & MTLTextureUsageShaderWrite);
    if (t && (b->rw || b->atom) && !(t.usage & MTLTextureUsageShaderWrite) && !swz_only) {
        const tex_t *tt = tid > 0 && tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid].t : NULL;
        RED_ONCE(32, "texture bound to an image slot lacks the shader-write usage (texture ID %d, serial %llu, target %d, format %#x, flags %#x, Metal usage %#lx, parent usage %#lx, slot %u, writes %u)",
                 tid, tt ? (unsigned long long)tt->serial : 0ull, tt ? (int)tt->d.target : -1, tt ? (unsigned)tt->d.format : 0u, tt ? (unsigned)tt->d.flags : 0u,
                 (unsigned long)t.usage, (unsigned long)t.parentTexture.usage, (unsigned)b->reg, (unsigned)b->rw);
        t = nil;
    }
    if (t && (swz_only || t.textureType != (MTLTextureType)b->dtt || img_needs_class_view(t, b->dpf) || (pf_srgb_linear(t.pixelFormat) != MTLPixelFormatInvalid && t.textureType != MTLTextureTypeTextureBuffer))) {   // img_fit: a view of the shader's class, type, or of an sRGB's linear twin
        uint64_t r2 = 0; id<MTLTexture> v = img_fit(tid, t, b->dpf, (MTLTextureType)b->dtt, &r2);
        if (v) { t = v; *rid = r2; } else if (swz_only) t = nil;
    }
    *real = t != nil;
    if (!t) { t = dummy_img(b->dtt, b->dpf); *rid = g_dummy_img_rid[b->dtt & 7][b->dpf & 3]; }
    return t;
}
// Resolve shader image writes before an encoder is opened. Snowdrop's cf5ba107e29b51f4 writes a COMPRESSIBLE-only R16F texture registered with RegisterTexture, not RegisterImage.
// An image bind may follow rendering into a texture created without IMAGE; upgrading it inside encode_args would
// copy out of an attachment while its render pass was still open. Close that pass first, then replace the shared backing (all registered sampled views follow g_ser_gen).
static uint32_t ubo_handle(int stage, int reg, int off); static int ubo_tex_id(uint32_t h);
static void image_prepare_shader(NvShader *sh, int st) {
    if (!sh) return;
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i]; if ((b->kind != NVN_RB_IMGU && (b->kind != NVN_RB_IMG || b->reg >= 32)) || (!b->rw && !b->atom)) continue;
        int tid = b->kind == NVN_RB_IMGU ? ubo_tex_id(ubo_handle(st, b->reg, b->off)) : (int)(bound_img(st, b->reg) & 0xfffff); if (tid <= 0) continue;
        os_unfair_lock_lock(&g_rmu);
        if (tid >= g_ntreg || !g_treg[tid].ok || g_treg[tid].gone) { os_unfair_lock_unlock(&g_rmu); continue; }
        tex_t image = g_treg[tid].t; image.d.flags |= NVN_TEXTURE_FLAGS_IMAGE; mtl_tex_gpu_written(&image);
        id<MTLTexture> base = tex_mtl_replay(&image); sparse_gpu_wrote(&image); tu_wrote(base); while (base.parentTexture) base = base.parentTexture;
        int reinterpret = b->atom && b->dpf != NVN_DPF_FLOAT && tex_atomic_alias(base);
        int upgrade = base && (!(base.usage & MTLTextureUsageShaderWrite) || reinterpret);
        os_unfair_lock_unlock(&g_rmu);
        bcp_wrote(&image, base);   // propagate image stores to a compressed-alias pair
        if (!upgrade) continue;
        rp_end();
        os_unfair_lock_lock(&g_rmu);
        // A registration can change while the pass ends; resolve it again under the descriptor lock.
        if (tid < g_ntreg && g_treg[tid].ok && !g_treg[tid].gone) {
            image = g_treg[tid].t; image.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
            base = tex_mtl_replay(&image); while (base.parentTexture) base = base.parentTexture;
            reinterpret = b->atom && b->dpf != NVN_DPF_FLOAT && tex_atomic_alias(base);
            if (base && (!(base.usage & MTLTextureUsageShaderWrite) || reinterpret)) {
                if (reinterpret) tex_upgrade(&image, base, img_usage(&image, base.pixelFormat) | MTLTextureUsageShaderAtomic, b->dpf == NVN_DPF_UINT ? MTLPixelFormatR32Uint : MTLPixelFormatR32Sint);
                else tex_upgrade(&image, base, img_usage(&image, base.pixelFormat), base.pixelFormat);
                args_dirty_all();
            }
        }
        os_unfair_lock_unlock(&g_rmu);
    }
}
static void image_prepare(NvProg *pr, unsigned stages) {
    if (!pr) return;
    if (!pr->c_img) { int any = 0;   // no written or atomic image slot in any stage: nothing to resolve, every draw
        for (int st = 0; st < 6 && !any; st++) { __unsafe_unretained NvShader *sh = pr->sh[st]; if (!sh) continue;
            for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; if (((b->kind == NVN_RB_IMG && b->reg < 32) || b->kind == NVN_RB_IMGU) && (b->rw || b->atom)) { any = 1; break; } } }
        pr->c_img = (int8_t)(any + 1); }
    if (pr->c_img == 1) return;
    for (int st = 0; st < 6; st++) {
        if (!(stages & (1u << st))) continue;
        NvShader *sh = pr->sh[st]; if (!sh) continue;
        image_prepare_shader(sh, st);
    }
}
// Image atomics run natively on the image's own texture (dmsh_atomic.py), so they and every other access of the image see the same texels: a block-linear image's Metal texture as
// much as a linear one's pool memory. They used to go through a raw pointer beside the texture, which for a block-linear image (a deswizzled copy) could only be a scratch buffer
// nothing read, and a 3D image's did not compile. Metal allows texture atomics only on an R32Uint / R32Sint texture made with the shader-atomic usage (img_usage); any other
// texture in an atomic slot is replaced by an atomic-capable write sink, logged red, rather than left to the GPU's undefined behaviour
static id<MTLTexture> dummy_img_atomic(int dtt, int dpf, int wide) {   // wide: a 64-bit atomic's sink (RG32Uint; nvn_rbind atom 2)
    static __strong id<MTLTexture> cache[8][3];
    int s = wide && dev_atomic64() ? 2 : dpf == NVN_DPF_SINT; id<MTLTexture> t = cache[dtt & 7][s]; if (t) return t;
    MTLPixelFormat pf = s == 2 ? MTLPixelFormatRG32Uint : s ? MTLPixelFormatR32Sint : MTLPixelFormatR32Uint;
    MTLTextureUsage u = MTLTextureUsageShaderRead | MTLTextureUsageShaderWrite | MTLTextureUsageShaderAtomic;
    if ((MTLTextureType)dtt == MTLTextureTypeTextureBuffer) {
        MTLTextureDescriptor *d = [MTLTextureDescriptor textureBufferDescriptorWithPixelFormat:pf width:16 resourceOptions:MTLResourceStorageModePrivate usage:u];
        id<MTLBuffer> b = [g_dev newBufferWithLength:256 options:MTLResourceStorageModePrivate]; res_add(b);
        t = [b newTextureWithDescriptor:d offset:0 bytesPerRow:256];
    } else {
        MTLTextureDescriptor *d = [MTLTextureDescriptor new];
        d.textureType = (MTLTextureType)dtt; d.pixelFormat = pf; d.width = d.height = d.depth = 1; d.storageMode = MTLStorageModePrivate; d.usage = u;
        t = [g_dev newTextureWithDescriptor:d];
    }
    cache[dtt & 7][s] = t; res_add(t); return t;
}
static int img_atomic_ok(id<MTLTexture> t, const nvn_rbind *b) {
    id<MTLTexture> base = t; while (base.parentTexture) base = base.parentTexture;   // a view of a view: a descriptor of R32 over a texture moved to R32 storage has its colour view as parent (tex_upgrade)
    int fmt = b->atom == 2 ? t.pixelFormat == MTLPixelFormatRG32Uint && dev_atomic64() : pf_r32i(t.pixelFormat);   // atom 2: 64-bit atomics (texture2d<ulong>), see pf_atomic
    return fmt && tex_atomic_capable(base) && t.textureType == (MTLTextureType)b->dtt && t.sampleCount == 1;
}
// Snapshot substitution for same-pass feedback sampling (registered by fb_prepare, used by encode_args; see the notes before draw)
static uint64_t g_draw_seq, g_fb_seq, g_fb_count; static int g_fb_n, g_fb_rebind;
static __unsafe_unretained id<MTLTexture> g_fb_src[4]; static __strong id<MTLTexture> g_fb_use[4]; static uint64_t g_fb_rid[4];
static inline int fb_snapshot_slot(int stage, id<MTLTexture> texture) {
    if (stage == 1 && g_fb_n && g_fb_seq == g_draw_seq)
        for (int i = 0; i < g_fb_n; i++) if (g_fb_src[i] == texture) return i;
    return -1;
}
// Called with g_rmu held after img_binding found a real texture. If the shader reinterprets the registered pixel class, keep its normal image binding.
static int fb_image_view(int tid, const nvn_rbind *b, id<MTLTexture> t) {
    __unsafe_unretained id<MTLTexture> registered = (__bridge id<MTLTexture>)g_treg[tid].view;
    return b->dtt == MTLTextureType2D && t.textureType == MTLTextureType2D && t.mipmapLevelCount == 1 &&
           (b->dpf == NVN_DPF_FLOAT || mtl_pf_dpf(registered.pixelFormat) == b->dpf);
}
static uint64_t g_args_reg_seen, g_args_dirty_seen, g_args_ser_seen;
// A reused block holds resource IDs: it is stale after a registration change, a guest write, or a serial's Metal object replaced (g_ser_gen: an upgraded or re-uploaded packaged texture,
// an alias taken or evicted) with the same program and bindings (MacNativeGames e49aa55)
static inline void args_sync_dirty(void) {
    uint64_t reg = g_reg_gen, dirty = nvn_dirty_now(), ser = g_ser_gen;
    if (reg != g_args_reg_seen || dirty != g_args_dirty_seen || ser != g_args_ser_seen) {
        g_args_reg_seen = reg; g_args_dirty_seen = dirty; g_args_ser_seen = ser; args_dirty_all();
    }
}
static void bind_args(id<MTL4CommandEncoder> en, int nvn_stage, int metal_vertex, id<MTLBuffer> ab, uint64_t gpu, size_t off, NSUInteger ai) {
    if (nvn_stage == 3) { za_check(gpu, "object table slot", (long)ai); if (rec_on()) rec_table_op(RO_T_ADDR, g_ot, gpu, ai); else [g_ot setAddress:gpu atIndex:ai]; return; }   // a task shader: the object stage's table (mesh_tasks_draw)
    if (nvn_stage == 5) enc_c_args(en, gpu, ai);
    else enc_r_args((id<MTL4RenderCommandEncoder>)en, metal_vertex ? MTLRenderStageVertex : MTLRenderStageFragment, gpu, ai);
}
// Static constant buffers of route B: the Ryujinx support_buffer (layout in Ryujinx.Graphics.Shader/SupportBuffer.cs: alpha_test u32, is_bgra u32[8],
// viewport_inverse f4, viewport_size f4, frag_scale_count i32, render_scale f32[73]..., each field aligned to 16 bytes) and the all-0 driver constant segment (64 KB = the upper bound declared by shaders)
static id<MTLBuffer> support_buf(void) {
    static __strong id<MTLBuffer> b; if (b) return b;
    b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b);   // All 0 when newly created
    const int render_scale_off = 192;   // Measured MSL layout: alpha_test 0, is_bgra[8] 16, viewport_inverse 144, viewport_size 160, frag_scale_count 176, render_scale float4[73] 192 (take .x)
    for (int i = 0; i < 73; i++) *(float *)((uint8_t *)b.contents + render_scale_off + 16 * i) = 1.0f;
    return b;
}
static uint64_t support_gpu(void) { static uint64_t gpu; if (!gpu) gpu = support_buf().gpuAddress; return gpu; }
// Raw global memory (binding kind DMSH_KIND_GMEM_MAP): a shader that loads or stores at an address it read from memory, which the translator could tie to no storage buffer
// (Ryujinx GlobalToStorage.TryGenerateRawStorageOp), finds the pool holding the address in this table and goes through a pointer to the pool's GPU address. Donkey Kong Bananza: gsys's
// vertex shader writes {count 0, instances 1} into each indirect draw record whose address its storage buffer lists, every frame before the terrain's culling refills them; with the
// stores dropped, the terrain's walls and floor were never drawn (instance count 0) and the counts grew frame after frame.
// Layout (dmsh_abi.h DMSH_GMEM_MAP_*): sorted by start, entry 0 empty, then the pools as disjoint ranges (a pool overlapping an earlier one keeps only its part past that one's end: every
// pool wraps the guest's own pages, so any pool holding an address gives its GPU address), unused entries starting at ~0. Rebuilt when the pools change; the replaced table goes through
// deferred release, so submissions encoded with it keep it. Only pools something gave an address in are listed (mtl_pool_addressed), each made resident, as the shader may reach any of them
static id<MTLBuffer> g_gmem_buf; static uint64_t g_gmem_gen;
static id<MTLBuffer> gmem_map(void) {
    uint64_t gen = atomic_load(&g_pool_gen);
    if (g_gmem_buf && g_gmem_gen == gen) return g_gmem_buf;
    id<MTLBuffer> b = [g_dev newBufferWithLength:DMSH_GMEM_MAP_ENTRIES * DMSH_GMEM_MAP_ENTRY options:MTLResourceStorageModeShared];
    uint8_t *t = (uint8_t *)b.contents; memset(t, 0, b.length);
    for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES; k++) memset(t + (size_t)k * DMSH_GMEM_MAP_ENTRY, 0xff, 8);
    int n = 1, over = 0; uint64_t end = 0;
    pthread_mutex_lock(&g_mu);
    for (int k = 0; k < g_nbig; k++) if (!g_big[k].full && addr_meets(g_big[k].lo, g_big[k].hi)) big_full(&g_big[k], "raw global memory's pool table");   // the pool table reaches all of a large pool
    gen = atomic_load(&g_pool_gen); pidx_sort();
    for (int k = 0; k < g_npool; k++) {
        pool_ent *p = &g_pools[g_pidx[k]];
        if (!addr_meets(p->lo, p->hi)) continue;
        if (!p->res) { res_add(p->buf); p->res = 1; g_pool_res_n++; }
        uint64_t lo = MAX((uint64_t)p->lo, end), hi = p->hi;
        if (lo >= hi) continue;
        if (n == DMSH_GMEM_MAP_ENTRIES) { over++; continue; }
        uint8_t *entry = t + (size_t)n++ * DMSH_GMEM_MAP_ENTRY;
        uint64_t e[3] = {lo, hi, p->gpu - p->base};
        memcpy(entry, e, sizeof e);
        if (hi - lo <= UINT32_MAX && (lo >> 32) == ((hi - 1) >> 32)) {
            uint32_t span = (uint32_t)(hi - lo);
            memcpy(entry + 28, &span, sizeof span);
        }
        end = hi;
    }
    pthread_mutex_unlock(&g_mu);
    // Each 2 MiB chunk wholly inside an entry gets a hint, so most accesses skip the binary search. A colliding chunk replaces the hint and the shader's tag check sends the other to the search
    for (int k = 1; k < n; k++) {
        const uint64_t *e = (const uint64_t *)(t + (size_t)k * DMSH_GMEM_MAP_ENTRY);
        uint64_t size = 1ull << DMSH_GMEM_HINT_SHIFT, first = (e[0] + size - 1) >> DMSH_GMEM_HINT_SHIFT, last = e[1] >> DMSH_GMEM_HINT_SHIFT;
        for (uint64_t c = first; c < last && c < (1ull << (32 - 13)); c++) {
            uint32_t hint = (uint32_t)(c << 13 | (unsigned)k);
            memcpy(t + (size_t)(c % DMSH_GMEM_MAP_ENTRIES) * DMSH_GMEM_MAP_ENTRY + 24, &hint, sizeof hint);
        }
    }
    if (over) RED_ONCE(140, "raw global memory: %d more memory pools than the %d entries of the pool table; addresses in them are not reached", over, DMSH_GMEM_MAP_ENTRIES - 1);
    res_add(b); res_commit();
    if (g_gmem_buf) defer_obj_release(g_gmem_buf);
    g_gmem_buf = b; g_gmem_gen = gen;
    { static int said; if (!said++) MLOG("raw global memory: pool table of %d ranges (pool generation %llu), rebuilt whenever a pool comes or goes", n - 1, (unsigned long long)gen); }
    return b;
}
static uint64_t __attribute__((unused)) gmem_map_gpu(void) { return gmem_map().gpuAddress; }   // the table's address (t_gmem_words, t_pool_chunks)
// A raw pointer read from a fixed storage-buffer word can reach a pool whose address no NVN getter handed out. Discover it before the draw, including when its argument block is
// reused; gmem_commit_patch then gives every block the complete table. Record the logical pool/run, not each interior pointer or the Metal buffer's page-rounded padding.
// The CPU sees only pointer values available now (or a complete queued inline update). GPU-generated values still rely on the table; this does not fix task/mesh ordering hangs.
static void gmem_discover(NvShader *sh, int stage) {
    g_gptr_bindings++;
    for (int q = 0; q < sh->rf.ngptr; q++) {
        const p_bind_buf *u = &S.ssbo[stage][sh->rf.gptr[q].reg]; uint64_t off = sh->rf.gptr[q].off, ptr = 0;
        if (!u->addr || (u->size && (off > u->size || u->size - off < 8))) continue;
        uint64_t at = u->addr + off;
        if (at < u->addr || at > UINT64_MAX - 8) continue;
        int kind;
        if (!pend_read(at, 8, &ptr, &kind)) {
            // A declared size does not prove the source is mapped: sparse holes read zero on the GPU and may be PROT_NONE on the CPU.
            if (!nvn_host_readable(at, 8) || !mtl_addr_known(at, 8)) continue;
            memcpy(&ptr, (const void *)(uintptr_t)at, 8);
        }
        g_gptr_reads++;
        if (!ptr) continue;
        uintptr_t lo = 0, hi = 0;
        pthread_mutex_lock(&g_mu);
        int b = big_of(ptr), p = b < 0 ? pool_held(ptr, 1) : -1;
        if (b >= 0) { lo = g_big[b].lo; hi = g_big[b].hi; }
        else if (p >= 0) { lo = g_pools[p].lo; hi = g_pools[p].hi; }
        int fresh = hi > lo && !addr_meets(lo, hi);
        pthread_mutex_unlock(&g_mu);
        if (fresh) { g_gptr_added++; g_gptr_bytes += hi - lo; }
        if (hi > lo) mtl_pool_addressed((const void *)lo, hi - lo);
    }
}
// An argument block holds the pool table that was current when it was encoded. A pool made after that, later in the submission or on another thread while it is encoded, is not in it, and a raw
// read of that pool gets 0: the failure that hung agl's task shaders (docs/reference/shaders.md, More memory pools than the raw pool table had entries). The GPU has not read the blocks of the
// command buffer yet, so at each commit, whole, segment or before a count wait (mdi_wait), their table words are pointed at a table of the pools as they are then (as #1407 does). A new table is built when the pools changed,
// not refilled in place, as earlier segments may be reading the old one. A block's write log is a bitmap of the entries of the table the shader searches: its word follows its table, and the log is
// read against that table (NvGmlLog), else a bit of the newer table would be decoded against the older one. Counted in the memory report: the words, those that held an older table than the
// current one, and those that held the current one behind the pools
@interface NvGmlLog : NSObject { @public id<MTLBuffer> b, map; } @end   // a log and the table its bits index, as gml_read reads them: gmem_commit_patch may change the table before the GPU starts
@implementation NvGmlLog @end
static __unsafe_unretained NvGmlLog *g_gml_last;   // gml_take's: the submission's commit blocks hold it until it completes
typedef struct { uint64_t *w; void *log; } gmem_slot;   // a block's table word, and the NvGmlLog of its write log (NULL: the shader has none)
static gmem_slot *g_gmem_slots; static int g_gmem_nslots, g_gmem_cslots; static uint64_t g_gmem_words, g_gmem_older, g_gmem_behind;
static void gmem_slot_note(uint64_t *w, void *log) {
    if (g_gmem_nslots == g_gmem_cslots) { g_gmem_cslots = g_gmem_cslots ? 2 * g_gmem_cslots : 256; g_gmem_slots = dm_realloc(g_gmem_slots, (size_t)g_gmem_cslots * sizeof *g_gmem_slots, "the raw pool table's word list"); }
    g_gmem_slots[g_gmem_nslots++] = (gmem_slot){w, log};
}
void mtl_gmem_words(uint64_t *words, uint64_t *older, uint64_t *behind) { *words = g_gmem_words; *older = g_gmem_older; *behind = g_gmem_behind; }
static void gmem_commit_patch(void) {
    int n = g_gmem_nslots; g_gmem_nslots = 0; if (!n || !g_gmem_buf) return;
    uint64_t was = g_gmem_buf.gpuAddress; int lag = atomic_load(&g_pool_gen) != g_gmem_gen;
    id<MTLBuffer> map = gmem_map(); uint64_t cur = map.gpuAddress;
    g_gmem_words += (uint64_t)n;
    for (int i = 0; i < n; i++) { const gmem_slot *k = &g_gmem_slots[i]; if (*k->w == cur) continue;
        if (*k->w != was) g_gmem_older++; else if (lag) g_gmem_behind++;
        *k->w = cur;
        if (k->log) ((__bridge NvGmlLog *)k->log)->map = map; }
}
// Where those stores land is known only once the GPU has run them, while the host's uniform shadows and its CPU reads of guest memory must not miss a GPU write (sbw / gw). Counted as a
// write anywhere, they refused every shadow of the submission and of the next ones: Donkey Kong Bananza's tunnel ran at 70 ms a frame, 4,533 passes a frame ended to land deferred updates.
// So each store also sets the bit of its pool table entry in a write log (DMSH_KIND_GMEM_LOG, a bitmap), each draw or dispatch with such stores gets its own log from a small pool, and the
// host learns the pools from it: until the learned set has stayed the same over GML_STABLE logs in a row, the stores count as a write anywhere; then as writes over the learned pools. A log
// read on completion that shows an entry in no learned pool adds that pool (before the set has settled, it starts the count again); if that submission had used the prediction, the shadows
// that relied on it are checked against the pool it missed (g_gsr): [RED] only when one lies in it. The missed pools are old ones the stores reach for the first time as the level streams in,
// not new ones (Donkey Kong Bananza: added 3.5-294 s before; no shadow covered a missed page in 7 misses when the log held pages). Pools, not pages: the game allocates new records as the
// level streams in, in the same pools (learned page by page, Donkey Kong Bananza's tunnel still met a new page 213 logs in), and a shader that writes a whole frame ran out of slots in a set
// of pages (Ocarina of Time's movie compute)
// GML_STABLE: 2 logs. While the set is learned every draw after the stores that reads guest memory on the CPU waits for the GPU (wl_cpu_hazard; nxnative's geometry stage lowering then): 8 logs gave Donkey Kong Bananza 9 frames of 300-540 ms
// when gameplay began. A pool found later is checked against the shadows that relied on it (g_gsr)
enum { GML_WORDS = DMSH_GMEM_MAP_ENTRIES / 32, GML_BYTES = GML_WORDS * 4, GML_MAX = 4096, GML_STABLE = 2 };
static pthread_mutex_t g_gml_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_gml_lo[GML_MAX], g_gml_hi[GML_MAX]; static int g_gml_n, g_gml_stable, g_gml_over;   // the learned ranges; over: the set filled: a write anywhere from then on
static uint64_t g_gml_logs, g_gml_miss;
static NSMutableArray<id<MTLBuffer>> *g_gml_free;
static void gml_read(id<MTLBuffer> b, id<MTLBuffer> map, int predicted, uint64_t fence) {   // map: the table the shader used, whose entries the bits name
    const uint32_t *w = (const uint32_t *)b.contents; const uint8_t *t = (const uint8_t *)map.contents;
    int added = 0, shadowed = 0; double age_lo = 1e30, age_hi = -1;
    uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    int nr = 0, unchecked = 0; static uint64_t rlo[GSR_N], rhi[GSR_N];   // the shadows that relied on this submission's writes (completion handlers run one at a time)
    if (predicted) { pthread_mutex_lock(&g_gsr_mu);
        uint64_t n = g_gsr_n, first = n > GSR_N ? n - GSR_N : 0;
        if (first && g_gsr[first % GSR_N].fence >= fence) unchecked = 1;   // the ring no longer holds all of them
        for (uint64_t i = first; i < n; i++) if (g_gsr[i % GSR_N].fence >= fence) { rlo[nr] = g_gsr[i % GSR_N].lo; rhi[nr++] = g_gsr[i % GSR_N].hi; }
        pthread_mutex_unlock(&g_gsr_mu); }
    pthread_mutex_lock(&g_gml_mu);
    g_gml_logs++;
    for (int i = 0; i < GML_WORDS && !g_gml_over; i++) for (uint32_t bits = w[i]; bits && !g_gml_over; bits &= bits - 1) {
        uint64_t e[2]; memcpy(e, t + (size_t)(i * 32 + __builtin_ctz(bits)) * DMSH_GMEM_MAP_ENTRY, sizeof e);
        int known = 0;
        for (int k = 0; k < g_gml_n && !known; k++) known = e[0] >= g_gml_lo[k] && e[1] <= g_gml_hi[k];
        if (known) continue;
        if (g_gml_n == GML_MAX) { g_gml_over = 1; break; }
        if (predicted) for (int j = 0; j < nr; j++) if (e[0] < rhi[j] && rlo[j] < e[1]) { shadowed++; break; }   // under a shadow? The whole pool counts: the log does not say where in it
        pthread_mutex_lock(&g_mu); int p = pool_peek(e[0]); double age = p >= 0 ? (double)(now - g_pools[p].born) / 1e6 : -1; pthread_mutex_unlock(&g_mu);
        if (age >= 0) { if (age < age_lo) age_lo = age; if (age > age_hi) age_hi = age; }
        g_gml_lo[g_gml_n] = e[0]; g_gml_hi[g_gml_n++] = e[1]; added++;
        if (g_gml_n <= 64) MLOG("raw global memory: stores reach %#llx..%#llx (pool table entry %d)", (unsigned long long)e[0], (unsigned long long)e[1], i * 32 + __builtin_ctz(bits));   // which pools a store went to: a game's CPU data in one (PRAGMATA's draw items) means a misplaced store
    }
    // Once the prediction has begun, a pool found late is added and the prediction goes on: going back to a write anywhere until more logs agreed (8 then) made every draw that read guest memory on the CPU (nxnative's geometry draws)
    // after the stores wait for the GPU (as wl_cpu_hazard does now), 400-2,000 ms frames for a second or more after each miss in Donkey Kong Bananza, and bought nothing, as the next late
    // pool is missed the same way and every miss is checked (g_gsr)
    if (added && g_gml_stable < GML_STABLE) g_gml_stable = 0; else if (g_gml_stable < INT_MAX) g_gml_stable++;
    if (predicted && added) g_gml_miss += added;
    uint64_t logs = g_gml_logs, miss = g_gml_miss; int learned = g_gml_n, over = g_gml_over;
    [g_gml_free addObject:b];
    pthread_mutex_unlock(&g_gml_mu);
    if (over) RED_ONCE(141, "raw global memory: the stores wrote in more than %d pools: they count as a write anywhere from now on", GML_MAX);
    if (predicted && added) {   // learned after the fact: harmful only if a shadow that relied on this submission's writes lies in a pool they missed
        static int lines; int bad = shadowed || unchecked;
        if (bad || lines++ < 8) MLOG("%sraw global memory: the stores wrote in %d pool(s) the host had not learned (log %llu, %llu such pools so far, %d learned; the pools were added %.0f-%.0f ms before): "
            "%d of them hold one of the %d uniform shadows taken from that submission on%s", bad ? "[RED] " : "", added, (unsigned long long)logs,
            (unsigned long long)miss, learned, age_lo < 1e30 ? age_lo : -1.0, age_hi, shadowed, nr,
            shadowed ? ": those shadows may have missed the write" : unchecked ? "; the shadow record had already wrapped, so not all could be checked" : ""); }
}
static uint64_t gml_take(id<MTLBuffer> map) {
    pthread_mutex_lock(&g_gml_mu);
    if (!g_gml_free) g_gml_free = [NSMutableArray array];
    id<MTLBuffer> b = g_gml_free.lastObject; if (b) [g_gml_free removeLastObject];
    int predict = !g_gml_over && g_gml_stable >= GML_STABLE;
    { static int said; if (predict && !said++) MLOG("raw global memory: the stores' writes are predicted from now on: %d pool(s) learned over %llu logs", g_gml_n, (unsigned long long)g_gml_logs); }
    if (predict) for (int i = 0; i < g_gml_n; i++) sbw_note(g_gml_lo[i], g_gml_hi[i] - g_gml_lo[i], SBW_GPU);
    pthread_mutex_unlock(&g_gml_mu);
    if (!predict) sbw_note(0, 0, SBW_GPU);
    if (!b) { b = [g_dev newBufferWithLength:GML_BYTES options:MTLResourceStorageModeShared]; res_add(b); res_commit(); }
    memset(b.contents, 0, GML_BYTES);
    g_gml_on = 1; uint64_t fence = g_fence + 1;   // this submission's fence (mtl_sub_commit)
    NvGmlLog *log = [NvGmlLog new]; log->b = b; log->map = map; g_gml_last = log;
    [g_commit_blocks addObject:^{ gml_read(log->b, log->map, predict, fence); }];
    return b.gpuAddress;
}
static id<MTLBuffer> zero_bank(void) { static __strong id<MTLBuffer> b; if (!b) { b = [g_dev newBufferWithLength:65536 options:MTLResourceStorageModeShared]; res_add(b); } return b; }
static uint64_t zero_bank_gpu(void) { static uint64_t gpu; if (!gpu) gpu = zero_bank().gpuAddress; return gpu; }
// Bindless handle (TEXU / SAMPU): the 32-bit word at byte offset off of uniform slot reg = the low 32 bits of the NVN handle the game wrote there (low 20 bits texture ID, high 12 bits sampler ID).
// Read on the CPU side at replay time (the memory at the same moment a real GPU would read it at execution); slot unbound / out of range / unreadable = 0 (empty slot: dummy texture / dummy sampler).
// A location with a GPU-side write queued in this submit (pend_hits) is still read at its current value; the first case is reported red.
// The same read serves storage buffer addresses kept in uniform memory (SBUFU: 8 bytes)
static void ubo_read(int stage, int reg, int off, size_t n, void *out) {
    memset(out, 0, n);
    if ((unsigned)stage >= NVN_RESOURCE_STAGES || (unsigned)reg >= 16) return;
    const p_bind_buf *u = &S.ubo[stage][reg];
    if (!u->addr || (uint64_t)off + n > u->size) return;
    if (g_nubd && ubd_hits(u->addr + off, u->addr + off + n) && ubd_covers(u->addr + off, u->addr + off + n)) { ubd_read(u->addr + off, n, out); return; }   // written by an inline update deferred in this pass
    { uint64_t g; const uint8_t *c; if (pend_hits(u->addr + off, n) && ubp_get(u->addr + off, u->addr + off + n, &g, &c)) { ubc_wait(g_ubp_ready); memcpy(out, c, n); return; } }   // memory and the inline updates queued over it
    if (pend_hits(u->addr + off, n)) { int kind; if (pend_read(u->addr + off, n, out, &kind)) return;   // written by an inline update queued in this submission: its data
        RED_ONCE(47, "the uniform buffer holding bindless handles (stage %d slot %d +%#x) has GPU-side writes in this submission (%s): the current value is read", stage, reg, off,
                 kind == 2 ? "a buffer copy" : kind == 3 ? "a fill" : kind == 1 ? "an inline update covering part of it" : "more writes than the host tracks"); }
    // A pool's buffer wraps guest memory without a copy (mtl_pool_add), so its contents at mo are the guest bytes at the address itself: read there, once a pool is found to hold
    // them. Asking the buffer for its contents cost a message, and, contents being an inner pointer, a retain and autorelease of the buffer at every handle read (Fire Emblem:
    // Fortune's Weave: about 4% of the submit thread's samples, and as much again draining the autorelease pool at commit)
    void *mb; uint64_t mo;
    if (!mtl_addr(u->addr + off, n, &mb, &mo)) return;
    memcpy(out, (const void *)(uintptr_t)(u->addr + off), n);
}
static uint32_t ubo_handle(int stage, int reg, int off) { uint32_t h; ubo_read(stage, reg, off, 4, &h); return h; }
// The handle a TEXU / SAMPU binding reads: a word, or two the shader ORs (handle type 1), which hold the texture and the sampler in either order. Unreal Engine 5.4's lighting shaders on the Switch 2 put the
// shared sampler's word first, so its G-buffer textures read as ID 0 from the first word alone: the dummy, and a black scene
static uint32_t ubo_bind_handle(int stage, const nvn_rbind *b) { uint32_t h = ubo_handle(stage, b->reg, b->off); if (b->soff) h |= ubo_handle(stage, b->sreg, b->soff - 1); return h; }
// A handle in uniform memory is relative to the pools active when the draw runs, just like an explicitly bound handle (0 = none).
static int ubo_tex_id(uint32_t h) { int t = (int)(h & 0xfffff); return t ? t + S.tex_base : 0; }
static int ubo_samp_id(uint32_t h) { int s = (int)(h >> 20); return s ? s + S.samp_base : 0; }
// A SAMPU binding's sampler ID: from its handle, or the separate sampler word of the driver table (sreg DMSH_SREG_C2, whose IDs are moved already, as the words TEXA / SAMPA read)
static int sampu_id(int stage, const nvn_rbind *b, int k) {
    if (b->soff && b->sreg == DMSH_SREG_C2) return (int)(c2_handle(stage, b->soff - 1u) >> 20);
    uint32_t h = k < 0 ? ubo_bind_handle(stage, b) : ubo_handle(stage, b->reg, b->off + 8 * k);
    return ubo_samp_id(b->raw_id ? h << 20 : h);   // raw_id: the word is a SeparateSamplerId (the ID itself), not a handle with the sampler in its high 12 bits
}
// The sampler ID a sampler binding reads in the current state: SAMPT the sampler part of a combined handle (texture ID | sampler ID << 20), SAMPU the same with the handle in a uniform buffer,
// SAMPI a sampler ID the shader names as a constant
static int bind_sid(int nvn_stage, const nvn_rbind *b) {
    int reg = b->reg;
    return b->kind == NVN_RB_SAMPU ? sampu_id(nvn_stage, b, -1) :
           b->kind == NVN_RB_SAMPI ? ubo_samp_id((uint32_t)reg << 20) : b->kind == NVN_RB_SAMPT ? (reg < NTEX ? (int)(bound_handle(S.tex[nvn_stage][reg]) >> 20) : 0) : reg < 128 ? (int)(bound_handle(S.samp[nvn_stage][reg]) >> 20) : 0;
}
// Whether a draw of sh binds a sampler with a twin (dmsh_rewrite.py border_emu, nvn_border.h), whose samples it must correct: then its pipeline keeps the corrections (pso_key.border),
// otherwise it is specialized without them (fn_off). Inspect the registered parameters: the pipeline is chosen before encode_args creates Metal samplers, including their twins
// DM_NVN_BORDER_KEEP=1 keeps the corrections in every pipeline (diagnostic: a wrong border colour with the specialization, or its cost: ab with DM_AB)
static int border_need(NvShader *sh, int nvn_stage) {
    static int keep = -1; if (keep < 0) keep = sw_mode("DM_NVN_BORDER_KEEP", 0);
    if (sh && sh->rf.border && sw_on(keep)) return 1;
    if (!sh || !sh->rf.border || !atomic_load_explicit(&g_border_any, memory_order_relaxed)) return 0;
    int need = 0;
    os_unfair_lock_lock(&g_rmu);
    for (int i = 0; i < sh->rf.nb && !need; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        if (!b->bw || (b->kind != NVN_RB_SAMP && b->kind != NVN_RB_SAMPT && b->kind != NVN_RB_SAMPU && b->kind != NVN_RB_SAMPI)) continue;
        int sid = bind_sid(nvn_stage, b);
        if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid]) {
            const samp_par *p = &g_sreg[sid]; int bn, bw; float k[4]; const uint8_t wrap[3] = {p->ws, p->wt, p->wr};
            need = nvn_border(p->border, wrap, p->reduction, &bn, &bw, k);
        }
    }
    os_unfair_lock_unlock(&g_rmu);
    return need;
}
// pso_key.border of a draw of pr in the current state (pso_key_fill): the draw fetches its pipeline again when this changes (draw_)
static int prog_border(NvProg *pr) { NvShader *vs = pr->sh[0], *fs = pr->sh[1]; return (vs && border_need(vs, vs->stage)) || border_need(fs, 1); }
// A table of the handles in one of the game's uniform buffers, which a shader indexes at run time (element k = the handle at off + 8k): nothing tells how many the game keeps there, and the argument
// buffer has one pointer for it, so the table is as long as the last handle the buffer holds. Kirby Air Riders keeps 197 handles up to element 392 in a 4,096-element buffer, and 34,000 textures
// registered: only those a table names are made here, where pool_gpu's walk over every ID took 16 GB and most of the frame. A table is looked at again in each pass, in each submission (a compute-only
// submission keeps the last pass's serial), and after a write queued in this submission over its bytes (htab_written: an inline update an open pass defers, a copy, a fill), and rebuilt when the handles
// or the registered IDs have changed: into a new buffer, as commands already encoded hold the old one. The read bitmap's address stays 0: the translator marks no read of a table (dmsh_reflect.py
// pool_arrays). Caller holds g_rmu, as for pool_gpu
#define HTAB_SNAPS 64
typedef struct { __strong id<MTLBuffer> buf; uint64_t gpu, reg, sgen, pass, sub, addr; int npend; uint8_t *copy; uint64_t *ser; uint32_t bytes, cap, n; int samp, dtt, dpf, tb, sb, c2s; } htab_snap;   // ser: each element's texture serial, for fb_texu_reads; sub, npend: the submission and its queued writes (g_npend) when the handles were last read
// A write queued since the table's handles were last read lands on [a, b) (or cannot be told). Only the writes since are scanned (pend_since), so games that update another uniform block before
// every draw keep the table without reading it again
static int htab_written(const htab_snap *p, uint64_t a, uint64_t b) {
    if (g_pend_over) return 1;
    if (p->npend == g_npend || b <= a) return 0;
    return pend_since(a, b, p->npend, g_ubp_idx) != 0;
}
static htab_snap g_htab[HTAB_SNAPS], *g_htab_last; static unsigned g_htab_next;
static uint64_t g_stat_htab_builds, g_stat_htab_reads; static double g_stat_htab_ms;   // reads: the handles read again (a new pass or submission, or a write over them)
static uint64_t htab_gpu(int stage, const nvn_rbind *b, __unsafe_unretained id<MTLBuffer> *out) {
    int samp = b->kind == NVN_RB_SAMPH, reg = b->reg;
    const p_bind_buf *u = reg < 16 ? &S.ubo[stage][reg] : NULL;
    uint64_t addr = u ? u->addr + b->off : 0, rg = atomic_load(&g_reg_gen), sg = atomic_load(&g_ser_gen);
    uint32_t bytes = u && u->addr && u->size > b->off ? (uint32_t)MIN(u->size - b->off, 65536u) : 0;
    int c2s = samp && b->soff && b->sreg == DMSH_SREG_C2 ? sampu_id(stage, b, 0) : -1;
    htab_snap *p = NULL, *free_ = NULL;
    for (int i = 0; i < HTAB_SNAPS && !p; i++) { htab_snap *q = &g_htab[i];
        if (!q->buf) { if (!free_) free_ = q; }
        else if (q->addr == addr && q->samp == samp && q->dtt == b->dtt && q->dpf == b->dpf && q->tb == S.tex_base && q->sb == S.samp_base && q->c2s == c2s) p = q; }
    if (p && p->pass == g_pass.serial && p->sub == g_sub_serial && p->reg == rg && p->sgen == sg && p->bytes == bytes && !htab_written(p, addr, addr + bytes)) { p->npend = g_npend; g_htab_last = p; *out = p->buf; return p->gpu; }
    static uint8_t now[65536]; g_stat_htab_reads++;
    bytes &= ~7u; memset(now, 0, bytes); for (uint32_t i = 0; i < bytes; i += 4096) ubo_read(stage, reg, b->off + i, MIN(bytes - i, 4096u), now + i);
    if (p && p->reg == rg && p->sgen == sg && p->bytes == bytes && !memcmp(p->copy, now, bytes)) {
        // Kept for a new pass: its handles are uses as a binding's are (tu_pass), and no shader marks a read of a table
        if (!samp) { const uint32_t *hw = (const uint32_t *)now; for (uint32_t k = 0; k < p->n; k++) { int tid = ubo_tex_id(hw[k * 2]); if (tid > 0 && tid < g_ntreg) g_treg[tid].used = g_tu_now; } }
        p->pass = g_pass.serial; p->sub = g_sub_serial; p->npend = g_npend; g_htab_last = p; *out = p->buf; return p->gpu; }
    uint64_t t0 = mach_absolute_time();
    if (!p) { p = free_ ? free_ : &g_htab[g_htab_next++ % HTAB_SNAPS]; if (p->buf) defer_obj_release(p->buf); uint8_t *keep = p->copy; uint64_t *ks = p->ser; uint32_t cap = p->cap; *p = (htab_snap){0}; p->copy = keep; p->ser = ks; p->cap = cap; }
    uint32_t n = 0; const uint32_t *w = (const uint32_t *)now;
    for (uint32_t k = bytes / 8; k > 0 && !n; k--) if (w[(k - 1) * 2]) n = k;
    uint64_t dummy = samp ? dummy_samp_rid() : (dummy_tex(b->dtt, b->dpf) ? g_dummy_tex_rid[b->dtt % DMY_NT][b->dpf & 3] : 0);
    if (p->cap < bytes) { free(p->copy); free(p->ser); p->copy = malloc(bytes ? bytes : 8); p->ser = malloc(bytes ? bytes : 8); p->cap = bytes; }
    static uint64_t e[8193]; static uint32_t lead[8193]; memset(lead, 0, sizeof lead);
    for (uint32_t k = 0; k < n; k++) { uint64_t r = 0; p->ser[k] = 0;
        if (samp) { int sid = c2s >= 0 ? c2s : ubo_samp_id(w[k * 2]); if (sid > 0 && sid < g_nsreg && g_sreg_ok[sid] && samp_fast(sid)) r = g_srid[sid]; }
        else { int tid = ubo_tex_id(w[k * 2]); if (tid > 0 && tid < g_ntreg && g_treg[tid].ok && (g_treg[tid].view || (!g_treg[tid].gone && tex_storage_live(&g_treg[tid].t)))) {   // as for the samplers: other data, not a handle
                void *tv = NULL; uint64_t tr = tex_fast_rid(tid, &tv); __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
                if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; t = tex_fit_elem(tid, t, (MTLTextureType)b->dtt, &r2); tr = t ? r2 : 0; }   // a handle the shader does not pick here may be another kind
                if (t && b->dtt == (uint8_t)MTLTextureTypeTextureBuffer) lead[k] = tbuf_lead(tid, t);
                if (t) { r = tr; p->ser[k] = g_treg[tid].t.serial; } } }
        e[k] = r ? r : dummy; }
    e[n] = dummy;
    memcpy(p->copy, now, bytes);
    // Registrations come many times a frame and rarely touch a table: the same elements keep the buffer, and argument blocks that hold it
    size_t metadata = !samp && b->dtt == MTLTextureTypeTextureBuffer ? 4 * (size_t)(n + 1) : 0;
    if (p->buf && p->n == n && !memcmp((uint8_t *)p->buf.contents + DMSH_POOL_HEAD, e, 8 * (size_t)(n + 1))
        && (!metadata || !memcmp((uint8_t *)p->buf.contents + DMSH_POOL_HEAD + 8 * (size_t)(n + 1), lead, metadata))) { p->reg = rg; p->sgen = sg; p->pass = g_pass.serial; p->sub = g_sub_serial; p->npend = g_npend; p->bytes = bytes; g_htab_last = p; *out = p->buf; return p->gpu; }
    id<MTLBuffer> buf = [g_dev newBufferWithLength:DMSH_POOL_HEAD + 8 * (size_t)(n + 1) + metadata options:MTLResourceStorageModeShared];
    uint8_t *c = buf.contents; *(uint32_t *)c = n; memcpy(c + DMSH_POOL_HEAD, e, 8 * (size_t)(n + 1));
    *(uint32_t *)(void *)(c + DMSH_POOL_BIAS) = metadata ? DMSH_POOL_HEAD + 8u * (n + 1) : 0;
    if (metadata) memcpy(c + DMSH_POOL_HEAD + 8 * (size_t)(n + 1), lead, metadata);
    res_add(buf);
    if (p->buf) defer_obj_release(p->buf);
    p->buf = buf; p->gpu = buf.gpuAddress; p->reg = rg; p->sgen = sg; p->pass = g_pass.serial; p->sub = g_sub_serial; p->npend = g_npend; p->addr = addr; p->bytes = bytes; p->n = n; p->samp = samp; p->dtt = b->dtt; p->dpf = b->dpf; p->tb = S.tex_base; p->sb = S.samp_base; p->c2s = c2s;
    g_psnap_gen++;
    static mach_timebase_info_data_t tb; if (!tb.denom) mach_timebase_info(&tb);
    double ms = (double)(mach_absolute_time() - t0) * tb.numer / tb.denom / 1e6; g_stat_htab_ms += ms;
    if (g_stat_htab_builds++ < 16 || g_stat_htab_builds % 1000 == 0)
        MLOG("table of a uniform buffer's handles: %s of %u elements from slot %d +%#x, built in %.3f ms (%llu builds, %.0f ms in all, %llu reads)", samp ? "samplers" : "textures", n, reg, b->off, ms, (unsigned long long)g_stat_htab_builds, g_stat_htab_ms, (unsigned long long)g_stat_htab_reads);
    g_htab_last = p; *out = buf; return p->gpu;
}
static uint32_t g_dispatch_dim[3];   // the current dispatch's workgroup counts (driver segment c0 + 0x200 for compute, and c0 + 4, where NVN2's compute shaders read them)
static uint32_t g_draw_binst;   // the current draw's base instance (draw), for c0 + 0x644
static __unsafe_unretained id<MTLBuffer> g_c0_dim_buf; static size_t g_c0_dim_off;   // where encode_args put them for this dispatch (nil: the shader reads no c0); an indirect dispatch copies its GPU-written counts there
// The pixel stage's sample position rows of c0, for the current pass's sample count. GLSLC lowers gl_SamplePosition (and interpolation at a sample) to a byte of a table at 0x620,
// indexed by the sample index (PIXLD.MYINDEX) and the pixel's place in a location grid: index = ((((y & w) << z) + (x & z)) << log2 samples) + (sample & mask), with
// {mask, log2 samples, z, w} at 0x630; each byte holds the sample's x (low nibble) and y (high nibble) in 1/16 pixel. Metroid Prime Remastered's G-buffer shaders pack it into an
// integer target (30 of its pixel shaders read it); left at 0, every sample sat in the pixel's corner instead of its centre (0x88 for one sample). Metal rasterizes at its default
// positions, the same for every pixel, so the grid masks stay 0 and the table holds those positions. Only one sample was met: the nibble order for several is the hardware's
// (nouveau's sample locations), not seen in a game.
// NVN2's table, the one written here, holds each nibble as a signed offset from the pixel centre (the corner form with bit 3 flipped, so 0x00 for one sample): its shaders either
// flip it back before use (PRAGMATA's 154 sample position readers: (byte ^ 0x88) nibbles / 16 - 0.5, added to the pixel centre) or pass the nibbles as they are to IPA's
// offset, which is signed from the centre (its pixel shader 4a9d4507eea6c837; Mesa NAK's load_barycentric_offset subtracts the half pixel from its corner table for the same
// operand). With the corner form there, every sample position read half a pixel up and left
static void c0_sample_rows(uint8_t *d, uint32_t samples) {
    static uint8_t table[17][DMSH_C0_SAMPLE_BYTES]; static uint32_t made;   // per sample count, made on first use
    if (samples < 1 || samples > DMSH_C0_SAMPLE_BYTES) samples = 1;
    if (!(made >> samples & 1)) {
        uint8_t *t = table[samples];
        if (samples == 1) t[0] = 0x88;
        else { MTLSamplePosition pos[DMSH_C0_SAMPLE_BYTES]; [g_dev getDefaultSamplePositions:pos count:samples];
               for (uint32_t i = 0; i < samples; i++) t[i] = (uint8_t)(((int)(pos[i].x * 16) & 15) | ((int)(pos[i].y * 16) & 15) << 4); }
        for (uint32_t i = 0; i < samples; i++) t[i] ^= 0x88;
        made |= 1u << samples;
    }
    uint32_t info[4] = { samples - 1, (uint32_t)__builtin_ctz(samples), 0, 0 };
    memcpy(d + DMSH_C0_SAMPLE_TABLE, table[samples], DMSH_C0_SAMPLE_BYTES); memcpy(d + DMSH_C0_SAMPLE_INFO, info, sizeof info);
}
// Which game shaders a failed command buffer encoded. A GPU fault's error names no pass or shader (Metal 4's commit feedback has only the error and two times), and a fault does not always stop
// the game (Splatoon Raiders, issue #1289: survivable page faults every minute or so before the fatal one), so what the failures have in common narrows the suspects. Each command buffer keeps the
// list of the shaders it encoded, each once. On an error this command buffer caused, the list is printed, then the shaders of all such failures so far, most often failed first, each as
// `key failed/ran`: in how many failed command buffers, of how many that encoded it. A shader that runs everywhere has a low ratio. Counts rather than an intersection: one failure with another
// cause would empty an intersection for good. A victim of another's fault and a submission refused after one ran nothing of their own and do not count. The list leaves out the host's own passes
// so a fault there shows no shader standing out. Only the submit thread adds to the list; the feedback handlers print, under g_cbs_mu
enum { CBS_KEY = sizeof ((NvShader *)0)->key, CBS_SHOW = 200, CBS_RANK = 30, CBS_REPORTS = 16 };
typedef struct { char key[CBS_KEY]; uint32_t runs; } cbs_ent;   // runs: command buffers that had encoded it, this one included
struct cbs_list { uint32_t n, nz; char z[ZA_SHOW][ZA_TEXT]; cbs_ent e[]; };   // nz, z: its zero GPU addresses (za_note), the first ZA_SHOW of them
typedef struct { char key[CBS_KEY]; uint32_t failed, runs; } cbs_tally_ent;
static cbs_ent *g_cbs; static uint32_t g_cbs_n, g_cbs_cap; static uint64_t g_cbs_serial = 1;
static pthread_mutex_t g_cbs_mu = PTHREAD_MUTEX_INITIALIZER; static cbs_tally_ent *g_cbs_tally; static uint32_t g_cbs_tally_n, g_cbs_tally_cap; static uint64_t g_cbs_failures;
static inline void cbs_note(NvShader *sh) {
    if (sh->cbs_seen == g_cbs_serial) return;
    sh->cbs_seen = g_cbs_serial; sh->cbs_runs++;
    if (g_cbs_n == g_cbs_cap) { g_cbs_cap = g_cbs_cap ? g_cbs_cap * 2 : 256; g_cbs = dm_realloc(g_cbs, g_cbs_cap * sizeof *g_cbs, "the command buffer's shader list"); }
    cbs_ent *e = &g_cbs[g_cbs_n++]; memcpy(e->key, sh->key, CBS_KEY); e->runs = sh->cbs_runs;
}
static cbs_list *cbs_take(void) {   // the list of the command buffer about to be committed; the next one starts empty
    cbs_list *l = malloc(sizeof *l + g_cbs_n * sizeof *g_cbs);
    if (l) { l->n = g_cbs_n; if (g_cbs_n) memcpy(l->e, g_cbs, g_cbs_n * sizeof *g_cbs); l->nz = g_za_n; memcpy(l->z, g_za, (g_za_n < ZA_SHOW ? g_za_n : ZA_SHOW) * sizeof g_za[0]); }
    g_cbs_n = 0; g_cbs_serial++; g_za_n = 0;
    return l;
}
static int cbs_tally_cmp(const void *pa, const void *pb) {   // most often failed first, then the rarer
    const cbs_tally_ent *x = pa, *y = pb;
    return x->failed != y->failed ? (x->failed > y->failed ? -1 : 1) : x->runs != y->runs ? (x->runs < y->runs ? -1 : 1) : strcmp(x->key, y->key);
}
static void cbs_tally_add(const cbs_list *l) {   // caller holds g_cbs_mu
    for (uint32_t i = 0; i < l->n; i++) { cbs_tally_ent *t = NULL;
        for (uint32_t k = 0; k < g_cbs_tally_n && !t; k++) if (!strcmp(g_cbs_tally[k].key, l->e[i].key)) t = &g_cbs_tally[k];
        if (!t) {
            if (g_cbs_tally_n == g_cbs_tally_cap) { g_cbs_tally_cap = g_cbs_tally_cap ? g_cbs_tally_cap * 2 : 256; g_cbs_tally = dm_realloc(g_cbs_tally, g_cbs_tally_cap * sizeof *g_cbs_tally, "the failed command buffers' shader counts"); }
            t = &g_cbs_tally[g_cbs_tally_n++]; memcpy(t->key, l->e[i].key, CBS_KEY); t->failed = 0; }
        t->failed++; t->runs = l->e[i].runs; }
}
static void cbs_report(const cbs_list *l, const char *where, uint64_t nth) {   // caller holds g_cbs_mu
    uint32_t show = l->n < CBS_SHOW ? l->n : CBS_SHOW, rank = g_cbs_tally_n < CBS_RANK ? g_cbs_tally_n : CBS_RANK;
    size_t room = (show + rank) * (CBS_KEY + 24) + 64; char *b = malloc(room), *e = b; if (!b) return;
    *b = 0; for (uint32_t i = 0; i < show; i++) e += snprintf(e, room - (size_t)(e - b), "%s%s", i ? " " : "", l->e[i].key);
    if (l->n > show) snprintf(e, room - (size_t)(e - b), " and %u more", l->n - show);
    MLOG("[RED] game shaders of failed command buffer #%llu (%s): %u: %s", (unsigned long long)nth, where, l->n, l->n ? b : "none");
    qsort(g_cbs_tally, g_cbs_tally_n, sizeof *g_cbs_tally, cbs_tally_cmp);
    e = b; *b = 0; for (uint32_t i = 0; i < rank; i++) e += snprintf(e, room - (size_t)(e - b), "%s%s %u/%u", i ? ", " : "", g_cbs_tally[i].key, g_cbs_tally[i].failed, g_cbs_tally[i].runs);
    MLOG("[RED] game shaders by failed/ran over %llu failed of %llu command buffers: %s", (unsigned long long)nth, (unsigned long long)g_cbs_serial - 1, rank ? b : "none");
    free(b);
    // what the host handed it as a GPU address below ZA_LOW (za_check): none says the zero base a fault at a low address read was not one of those (a table slot the host never set, a host pass's own table, or an address the shader computed)
    MLOG("[RED] zero GPU addresses in failed command buffer #%llu: %u%s%s%s%s%s%s%s%s (%llu in the run)", (unsigned long long)nth, l->nz, l->nz ? ": " : " (none)",
         l->nz > 0 ? l->z[0] : "", l->nz > 1 ? "; " : "", l->nz > 1 ? l->z[1] : "", l->nz > 2 ? "; " : "", l->nz > 2 ? l->z[2] : "", l->nz > 3 ? "; " : "", l->nz > 3 ? l->z[3] : "", (unsigned long long)g_za_total);
}
static void cbs_feedback(cbs_list *l, NSError *err, const char *where) {   // from a command buffer's feedback handler; owns l
    if (err && l) { NSString *d = err.description;
        if (![d containsString:@"InnocentVictim"] && ![d containsString:@"SubmissionsIgnored"]) {
            pthread_mutex_lock(&g_cbs_mu);
            uint64_t nth = ++g_cbs_failures;
            cbs_tally_add(l);
            if (nth <= CBS_REPORTS) cbs_report(l, where, nth);
            pthread_mutex_unlock(&g_cbs_mu);
        } }
    free(l);
}
// A uniform slot bound shorter than its shader reads it at fixed offsets (nvn_rbind.fixw, the translator's count): Maxwell gives 0 for a read past a constant buffer's size, and games
// rely on it. Metal has no bounds, so the binding's bytes go into an arena block with zeros up to the end of those reads, and the slot points there. Checked once a binding, where the
// translator used to compare every read with the size (GitHub issue #2066: a fifth of the GPU's time in Donkey Kong Bananza); a read at a computed offset is still compared in the
// shader (NVN_RB_CBSIZE). The bytes are the ones the slot would have pointed at: the deferred updates' when they cover the binding, a partly updated block's copy, else the memory.
// Paper Mario: The Thousand-Year Door binds 16 bytes to a slot read up to byte 0x930, and the GPU hung on the light count it read at 0x900 (GitHub issue #1860)
// The size that bounds a slot is the one the console's driver gives the GPU, the bound size rounded up to 256 bytes (ub_bound): nvnCommandBufferBindUniformBuffer writes
// (size + 0xff) & ~0xff in NVN1's driver (method 0x8e0) and ((size + 0xff) >> 8) << 20 in NVN2's (method 0xe02), so a read between the bound size and the next multiple of 256
// gives the memory there, not 0. Ocarina of Time binds 16, 64, 144, 192, 464, 976, 16,960 and 41,600 bytes (GitHub issue #2218). live: the bytes of the binding that are
// readable, its rounded size where the pool reaches that far
static inline uint64_t ub_bound(uint64_t size) { return (size + 255) & ~UINT64_C(255); }
static uint64_t g_ub_short;
static uint64_t ub_padded(NvShader *sh, int st, int reg, const p_bind_buf *u, uint64_t live, uint32_t need, const uint8_t *mem, __strong id<MTLBuffer> *buf) {
    arena_at at = arena_take(need, buf); uint64_t gpu = at.gpu; uint8_t *d = at.cpu; uint64_t lo = u->addr, hi = lo + u->size, g; const uint8_t *c;
    if (!mem) mem = (const uint8_t *)(uintptr_t)lo;
    if (g_nubd && ubd_hits(lo, hi) && ubd_covers(lo, hi)) {   // as ubd_shadow_: the updates in order
        for (int i = 0; i < g_nubd; i++) { const ubd_ent *e = &g_ubd[i]; uint64_t a = e->lo > lo ? e->lo : lo, b = e->hi < hi ? e->hi : hi;
            if (a < b) memcpy(d + (a - lo), (const uint8_t *)e->buf.contents + e->off + (a - e->lo), (size_t)(b - a)); } }
    else memcpy(d, g_nubd && ubd_hits(lo, hi) && ubp_get(lo, hi, &g, &c) ? c : mem, (size_t)u->size);
    if (live > u->size) memcpy(d + u->size, mem + u->size, (size_t)(live - u->size));   // the rest of the last 256 bytes, which no deferred update of the binding covers
    memset(d + live, 0, need - (size_t)live);
    uint64_t n = ++g_ub_short;
    if (!(sh->logged & 16)) { sh->logged |= 16; static int nlog; if (nlog++ < 16) MLOG("uniform slot %d of shader %s (stage %d) is bound with %llu bytes (%llu readable, a multiple of 256) and read up to byte %u at fixed offsets: it is encoded with zeros past those (%llu such encodes so far)", reg, sh->key, st, (unsigned long long)u->size, (unsigned long long)live, need, (unsigned long long)n); }
    return gpu;
}
// A CBUF slot's GPU address, and in *use the buffer it points into: the bound uniform buffer; the shadow of inline updates deferred in this pass; an arena copy padded with zeros
// when the binding is shorter than the shader reads it at fixed offsets (ub_padded); or the all-zero bank when nothing usable is bound. The full encode and the patch take it from here
static uint64_t cbuf_slot(NvShader *sh, int nvn_stage, const nvn_rbind *b, __strong id<MTLBuffer> *use) {
    int reg = b->reg; const p_bind_buf *u = reg < 16 ? &S.ubo[nvn_stage][reg] : NULL;
    void *mb; uint64_t mo, gpu;
    if (!u || !u->addr || !mtl_gpu_addr(u->addr, u->size ? u->size : 1, &mb, &mo, &gpu)) {   // reads 0 (the all-zero bank), as an unbound texture slot does, instead of through a null pointer
        // A slot the game bound empty itself (address 0, size 0): every read is past the binding's size, which reads 0 on Maxwell as for a short binding (ub_padded), so the zero bank
        // is what it reads. Resident Evil 4 binds compute slot 1 so before every dispatch of 4e06df4bbdf9b7c9, whose cb1 reads it does not use. Noted once; a slot never bound and
        // an address in no pool stay red
        if (u && !u->addr && !u->size && (S.ubo_set[nvn_stage] >> reg & 1)) {
            static int noted; if (!noted++) MLOG("stage %d cb%d is bound empty by the game (address 0, size 0): shader %s reads it as 0, as a read past a binding's size is (frame %llu)", nvn_stage, reg, sh->key, (unsigned long long)atomic_load(&g_cur_frame));
            *use = zero_bank(); return zero_bank_gpu(); }
        if (!g_red_once[16]) { char note[400] = ""; if (u && u->addr) dm_gfx_addr_note(u->addr, note, sizeof note);   // which shader, and what the address is (unbound, or in no pool)
            RED_ONCE(16, "stage %d cb%d has no uniform buffer bound: shader %s, address %#llx size %#llx (frame %llu)%s", nvn_stage, reg, sh->key, u ? (unsigned long long)u->addr : 0ull, u ? (unsigned long long)u->size : 0ull,
                     (unsigned long long)atomic_load(&g_cur_frame), note); }
        *use = zero_bank(); return zero_bank_gpu(); }
    if (__builtin_expect(ub_bound(u->size) < 4u * b->fixw, 0)) {   // bound shorter than the shader reads it at fixed offsets: those reads give 0, as on Maxwell
        void *rb; uint64_t ro, rg, live = u->size && mtl_gpu_addr(u->addr, ub_bound(u->size), &rb, &ro, &rg) && rb == mb ? ub_bound(u->size) : u->size;   // the pool may end before the rounded size
        return ub_padded(sh, nvn_stage, reg, u, live, 4u * b->fixw, (const uint8_t *)[(__bridge id<MTLBuffer>)mb contents] + mo, use); }
    *use = (__bridge id<MTLBuffer>)mb;
    uint64_t ul = cb_len(sh, reg, u->size);   // what the shader can read of it
    if (g_nubd && ubd_hits(u->addr, u->addr + ul)) {   // inline updates deferred in this pass (see mtl_sub_update); ubd_conflict made sure they cover the binding or the rest can be read from memory
        uint64_t g; const uint8_t *c; if (ubd_covers(u->addr, u->addr + ul)) gpu = ubd_shadow(u->addr, u->addr + ul);
        else if (ubp_get(u->addr, u->addr + ul, &g, &c)) gpu = g;
        else RED_ONCE(92, "a uniform update deferred inside the pass covers only part of the binding (stage %d cb%d): the content before the update is read", nvn_stage, reg); }
    return gpu;
}
// The uniform slots' bound sizes (DMSH_CBSIZE_BANK), which the translator's reads at computed offsets compare with: one past its slot's size, rounded up to 256 bytes (ub_bound), gives 0, as on Maxwell. 0 for an unbound slot,
// whose reads go to the zero bank (cbuf_slot) and get 0 here too
static uint64_t cbsize_table(int nvn_stage, __strong id<MTLBuffer> *use) {
    arena_at at = arena_take(16 * ((DMSH_CBSIZE_SLOTS + 3) / 4), use); uint32_t *d = (uint32_t *)(void *)at.cpu;
    memset(d, 0, 16 * ((DMSH_CBSIZE_SLOTS + 3) / 4));
    for (int r = 0; r < DMSH_CBSIZE_SLOTS; r++) { const p_bind_buf *u = &S.ubo[nvn_stage][r]; if (u->addr) d[r] = u->size < 65536 ? (uint32_t)ub_bound(u->size) : 65536; }   // rounded up to 256, as the driver gives it to the GPU (ub_bound)
    return at.gpu;
}
// A storage buffer by address (SBUFU / SBUFQ) has no size: on the console the shader reads the GPU's page table as far as it indexes from the address, the pages mapped there and 0 for an
// unmapped one. The host binds the Metal buffer that holds the address, a memory pool, a large pool's chunk or a sparse pool's mapped run, and a read past its end reached whatever lay
// after it in the GPU's address space: PRAGMATA's GI voxeliser ef4ad01151918469 reads 0x150000 bytes and more from the base of a 1 GB sparse pool with one 0xc0000-byte run mapped
// there, and the other memory's bytes became hot surfel colours that the GI spread through the R53 hangar (GitHub issue #2618). So the translator compares each access with the memory
// bound (Ryujinx TranslationFlags.MetalStorageBounds), from the shader's length table (dmsh_abi.h DMSH_SBLEN_BANK): the binding's words before the address, then those plus the
// words from it to the buffer's end. Out of it a load gives 0 and a store or an atomic is dropped, as on an unmapped page. buf nil: a dummy, nothing readable
static void sblen_put(uint32_t *t, const nvn_rbind *b, id<MTLBuffer> buf, uint64_t off) {
    if (!t || !b->sbx) return;
    uint64_t len = buf ? buf.length : 0, before = off <= len ? off >> 2 : 0, total = off <= len ? before + ((len - off) >> 2) : 0;
    if (total > UINT32_MAX) { total = UINT32_MAX; if (before > total) before = total; }   // a binding is far smaller than 16 GB; kept inside the u32 the comparison wraps at
    t[2 * (b->sbx - 1)] = (uint32_t)before; t[2 * (b->sbx - 1) + 1] = (uint32_t)total;
}
// Whether a block of sh can be patched when only what the uniform bindings point at changed: every binding that reads through a uniform buffer is a plain CBUF slot (no handles or
// addresses read out of one: TEXU, SAMPU, IMGU, TBUFU, TEXH, SAMPH, SBUFU, UBOP), or the slots' size table (CBSIZE, made from the bindings alone), or the buffer images' offsets (TBIAS, made from the images, which such a change leaves as they are), and no slot is registered by its
// location (GMEM / GMEML: gmem_slot_note). An allow-list, so a new kind is never patched
static int args_cbuf_only(NvShader *sh) {
    if (__builtin_expect(g_res_check_on, 0)) return 0;   // the resource check (USE) sees every slot of a full encode
    if (sh->ab_cbuf_only) return sh->ab_cbuf_only == 1;
    int ok = 1, cb = 0;
    for (int i = 0; i < sh->rf.nb && ok; i++) switch (sh->rf.b[i].kind) {
        case NVN_RB_CBUF: cb = 1; break;
        case NVN_RB_SAMP: case NVN_RB_SAMPT: case NVN_RB_SAMPI: case NVN_RB_TEX: case NVN_RB_IMG: case NVN_RB_SBUFP: case NVN_RB_SUPPORT: case NVN_RB_C1:
        case NVN_RB_DRV0: case NVN_RB_DRV2: case NVN_RB_PUSH: case NVN_RB_BORDER: case NVN_RB_CBSIZE: case NVN_RB_TBIAS: break;   // TBIAS: made from the images alone
        default: ok = 0;
    }
    sh->ab_cbuf_only = ok && cb ? 1 : 2;
    return sh->ab_cbuf_only == 1;
}
// The shader lists of the last CBS_FLY committed command buffers, by fence: a hang report names what the command buffer it waits for runs (mtl_wait_at)
#define CBS_FLY 64
static struct { uint64_t fence; cbs_list *l; } g_cbs_fly[CBS_FLY]; static uint32_t g_cbs_fly_i;
static void cbs_fly_put(uint64_t fence, const cbs_list *l) {
    if (!l) return;
    cbs_list *c = malloc(sizeof *c + l->n * sizeof *l->e); if (!c) return;
    memcpy(c, l, sizeof *c + l->n * sizeof *l->e);
    pthread_mutex_lock(&g_cbs_mu); uint32_t i = g_cbs_fly_i++ % CBS_FLY; free(g_cbs_fly[i].l); g_cbs_fly[i].fence = fence; g_cbs_fly[i].l = c; pthread_mutex_unlock(&g_cbs_mu);
}
// The last IND_FLY indirect dispatches (fence of their command buffer, shader, where the GPU reads the group counts): a hang report prints the counts as they are now
#define IND_FLY 64
static struct { uint64_t fence, ind, off; void *buf; char key[CBS_KEY]; } g_ind_fly[IND_FLY]; static uint32_t g_ind_fly_i;
static void ind_fly_put(const char *key, uint64_t ind, uint64_t off, void *buf) {
    pthread_mutex_lock(&g_cbs_mu); uint32_t i = g_ind_fly_i++ % IND_FLY;
    g_ind_fly[i].fence = g_fence + 1; g_ind_fly[i].ind = ind; g_ind_fly[i].off = off; g_ind_fly[i].buf = buf; strlcpy(g_ind_fly[i].key, key, CBS_KEY);
    pthread_mutex_unlock(&g_cbs_mu);
}
static void cbs_fly_report(uint64_t fence) {
    pthread_mutex_lock(&g_cbs_mu);
    for (uint32_t i = 0; i < CBS_FLY; i++) if (g_cbs_fly[i].l && g_cbs_fly[i].fence == fence) {
        const cbs_list *l = g_cbs_fly[i].l; char b[CBS_SHOW * (CBS_KEY + 1) + 32], *e = b; *b = 0;
        for (uint32_t k = 0; k < l->n && k < CBS_SHOW; k++) e += snprintf(e, sizeof b - (size_t)(e - b), "%s%s", k ? " " : "", l->e[k].key);
        MLOG("[RED] game shaders of the command buffer of fence %llu: %u: %s%s", (unsigned long long)fence, l->n, l->n ? b : "none", l->n > CBS_SHOW ? " and more" : "");
    }
    for (uint32_t i = 0; i < IND_FLY; i++) if (g_ind_fly[i].buf && g_ind_fly[i].fence == fence) {
        const uint32_t *g = (const uint32_t *)((uint8_t *)[(__bridge id<MTLBuffer>)g_ind_fly[i].buf contents] + g_ind_fly[i].off);
        MLOG("[RED]   its indirect dispatch of %s: %u x %u x %u groups now at %#llx", g_ind_fly[i].key, g[0], g[1], g[2], (unsigned long long)g_ind_fly[i].ind);
    }
    pthread_mutex_unlock(&g_cbs_mu);
}
static int encode_args_(id<MTL4CommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex);
#define ZA_K(k) (1ull << NVN_RB_##k)
static const uint64_t ZA_ADDR_KINDS = ZA_K(CBUF) | ZA_K(SBUFP) | ZA_K(SUPPORT) | ZA_K(C1) | ZA_K(DRV0) | ZA_K(DRV2) | ZA_K(UBOP) | ZA_K(SBUFU) | ZA_K(TEXP) | ZA_K(SAMPP) | ZA_K(TEXH) |
                                      ZA_K(SAMPH) | ZA_K(BORDER) | ZA_K(PUSH) | ZA_K(GMEM) | ZA_K(GMEML) | ZA_K(CBSIZE) | ZA_K(SBUFQ) | ZA_K(TBIAS) | ZA_K(SBLEN);   // binding kinds whose slot holds a GPU address
#undef ZA_K
static void args_check_addresses(const nvn_refl *rf, const uint64_t *slots, size_t arg_len, int nvn_stage) {
    for (int i = 0; i < rf->nb; i++) { const nvn_rbind *b = &rf->b[i];
        if ((ZA_ADDR_KINDS >> b->kind & 1) && (size_t)b->aid * 8 < arg_len && slots[b->aid] < ZA_LOW) { char w[64]; snprintf(w, sizeof w, "stage %d argument block (binding kind %d) slot", nvn_stage, b->kind); za_note(w, b->aid, slots[b->aid]); }
    }
}
static int encode_args(id<MTL4CommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) { cbs_note(sh); za_shader((__bridge const void *)sh, sh->key[0] ? sh->key : "?"); AP_B; int r = encode_args_(en, sh, nvn_stage, metal_vertex); AP_E(3); return r; }
static int encode_args_(id<MTL4CommandEncoder> en, NvShader *sh, int nvn_stage, int metal_vertex) {
    if ((unsigned)nvn_stage >= NVN_RESOURCE_STAGES) MDIE("argument encoding has unsupported resource stage %d", nvn_stage);
    if (!sh->ae) return 1;
    if (sh->rf.ngptr) gmem_discover(sh, nvn_stage);
    args_sync_dirty();
    int reusable = !passdump() && sh->ab_gen == g_args_gen[nvn_stage] && sh->ab_pass == g_pass.serial && sh->ab_buf && nvn_stage != 5 && !(sh->rf.drv_read[0] && sh->ab_binst != g_draw_binst) && !((sh->rf.pool || sh->rf.htab) && sh->ab_pgen != g_psnap_gen);
    if (reusable && sh->ab_ugen == g_args_ugen[nvn_stage]) {
        bind_args(en, nvn_stage, metal_vertex, sh->ab_buf, sh->ab_gpu, sh->ab_off, sh->rf.abi);
        g_stat_args_reuse++;
        return 1;
    }
    NSUInteger arg_len = sh->arg_len ? sh->arg_len : sh->ae.encodedLength;
    if (reusable && args_cbuf_only(sh)) {   // only what the uniform bindings point at changed since this shader's block: copy it and write its uniform slots and their size table again
        id<MTLBuffer> ab; arena_at at = arena_take(arg_len, &ab); size_t off = at.off;
        uint64_t ab_gpu = at.gpu;   // cbuf_slot / cbsize_table below allocate too (arena_take)
        uint64_t *p = (uint64_t *)(void *)at.cpu;
        memcpy(p, (const uint8_t *)sh->ab_buf.contents + sh->ab_off, arg_len);
        for (int i = 0; i < sh->rf.nb; i++) { const nvn_rbind *b = &sh->rf.b[i]; id<MTLBuffer> use;
            if (b->kind == NVN_RB_CBUF) p[b->aid] = cbuf_slot(sh, nvn_stage, b, &use);
            else if (b->kind == NVN_RB_CBSIZE) p[b->aid] = cbsize_table(nvn_stage, &use);
            else continue;
            za_check(p[b->aid], "argument block address slot", b->aid); }
        bind_args(en, nvn_stage, metal_vertex, ab, ab_gpu, off, sh->rf.abi);
        sh->ab_buf = ab; sh->ab_gpu = ab_gpu; sh->ab_off = off; sh->ab_ugen = g_args_ugen[nvn_stage]; g_args_wrote[nvn_stage] = g_args_seq;   // pass, base instance, pool and binding generations are the ones the copied block was encoded for
        g_stat_args_patch++;
        return 1;
    }
    int gmem_aid = -1; id<MTLBuffer> raw_map = nil; int gml_taken = 0;   // the slot that holds the raw pool table (gmem_slot_note), and the table this block's log and table slot share
    uint64_t slots[NVN_REFL_MAXARG]; memset(slots, 0, arg_len < sizeof slots ? arg_len : sizeof slots);   // only the bytes that are copied out (they used to be zeroed in full, before the reuse check too)
    #define USE(r, u) do { (void)(u); if (__builtin_expect(g_res_check_on, 0)) res_check((r), sh->key, reg); } while (0)
    // DM_NVN_PASSDUMP: on the first new encode per pass per stage, records the shader key (K) and the actual source of each binding (B: UBO address / length / first 4 floats, texture ID and serial, or "dummy")
    static uint64_t bl_pass[NVN_RESOURCE_STAGES]; static int bl_all = -1; if (bl_all < 0) bl_all = getenv("DM_NVN_PASSDUMP_ALL") != NULL;   // Record on every new encode (otherwise only the first time per pass per stage)
    int bl = passdump() && (bl_all || g_kd_force || bl_pass[nvn_stage] != g_stat_pass);
    if (bl) { bl_pass[nvn_stage] = g_stat_pass; PD("K %llu st%d key=%s nb=%d", g_stat_pass, nvn_stage, sh->key[0] ? sh->key : "?", sh->rf.nb); }
    uint32_t bmask = 0; float bk[DMSH_BORDER_SAMPLERS][4];   // the border colour block (see nvn_border.h): the twins that correct, their factors
    // The offset table (dmsh_abi.h DMSH_TBIAS_BANK), as long as the shader declares it: each buffer image's and buffer texture's element offset (tbuf_lead), an array's one per element;
    // 0 for an aligned texture, an unbound slot or a texture of another kind
    uint32_t *tbias = NULL; uint64_t tbias_gpu = 0; __strong id<MTLBuffer> tbias_buf = nil;
    if (sh->rf.tbias_n) { arena_at at = arena_take(sh->rf.tbias_n * 4u, &tbias_buf); tbias = (uint32_t *)(void *)at.cpu; tbias_gpu = at.gpu; memset(tbias, 0, sh->rf.tbias_n * 4u); }
    // The length table of the bounded storage buffers (dmsh_abi.h DMSH_SBLEN_BANK, sblen_put), filled as they are encoded; 0 for a slot with no buffer, which reads nothing
    uint32_t *sblen = NULL; uint64_t sblen_gpu = 0; __strong id<MTLBuffer> sblen_buf = nil;
    if (sh->rf.sblen) { arena_at at = arena_take(DMSH_SBLEN_SLOTS * 8u, &sblen_buf); sblen = (uint32_t *)(void *)at.cpu; sblen_gpu = at.gpu; memset(sblen, 0, DMSH_SBLEN_SLOTS * 8u); }
    os_unfair_lock_lock(&g_rmu);   // The texture / sampler registration tables may be being grown or rewritten by the game thread
    for (int i = 0; i < sh->rf.nb; i++) {
        const nvn_rbind *b = &sh->rf.b[i];
        int reg = b->reg, aid = b->aid;
        if (b->kind == NVN_RB_CBUF) {
            const p_bind_buf *u = reg < 16 ? &S.ubo[nvn_stage][reg] : NULL;
            id<MTLBuffer> use; slots[aid] = cbuf_slot(sh, nvn_stage, b, &use);
            USE(use, MTLResourceUsageRead);
            void *mb; uint64_t mo, g_;
            if (bl && u && u->addr && mtl_gpu_addr(u->addr, u->size ? u->size : 1, &mb, &mo, &g_)) {
                PD("B %llu st%d cb reg%d addr=%#llx size=%llu pool=%#llx+%#llx f=%g,%g,%g,%g", g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr, (unsigned long long)u->size,
                   (unsigned long long)(u->addr - mo), (unsigned long long)mo, ((float *)u->addr)[0], ((float *)u->addr)[1], ((float *)u->addr)[2], ((float *)u->addr)[3]);
                // DM_NVN_CBDUMP=directory: also saves the whole uniform block to disk as-is (F<frame>_p<pass>_st<stage>_cb<slot>_<address>.bin), for the ground-truth side to compare byte by byte by guest address
                // DM_NVN_CBDUMP_KEY=shader key: only save for draws using that shader (any stage)
                static const char *cbd, *cbk; static int cbd_init; if (!cbd_init) { cbd = getenv("DM_NVN_CBDUMP"); cbk = getenv("DM_NVN_CBDUMP_KEY"); cbd_init = 1; }
                if (cbd && (!cbk || !strcmp(sh->key, cbk))) {
                    char p[1024]; snprintf(p, sizeof p, "%s/F%llu_p%llu_st%d_cb%d_%llx.bin", cbd, (unsigned long long)atomic_load(&g_cur_frame), g_stat_pass, nvn_stage, reg, (unsigned long long)u->addr);
                    FILE *f = fopen(p, "wb"); if (f) { fwrite((const void *)u->addr, 1, u->size, f); fclose(f); } else RED_ONCE(20, "CBDUMP cannot write %s", p);
                }
            }
        } else if (b->kind == NVN_RB_SAMPU && b->n) {   // the samplers of an array of handles in a uniform buffer: element k = the sampler ID of the handle at the array's base + 8k (see the textures below)
            for (int k = 0; k < b->n; k++) {   // only registered IDs are looked up: most elements are other data the shader never picks as a handle
                int sid = sampu_id(nvn_stage, b, k); if (sid >= g_nsreg || !g_sreg_ok[sid]) sid = 0;
                if (bl && (sid || (b->soff && b->sreg == DMSH_SREG_C2 && !k))) PD("B %llu st%d sampu [%d] sid=%d%s", g_stat_pass, nvn_stage, k, sid, b->soff && b->sreg == DMSH_SREG_C2 ? " from c2" : "");
                __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
                slots[aid + k] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
                USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_TEXU && b->n) {   // an array of handles in a uniform buffer that the shader indexes at run time (Ryujinx's texture array from a buffer): element k = the handle
            uint64_t real = 0;                         // at the array's base + 8k (BindlessToArray: 0, or a table's constant offset), the index the shader computes from the handle's offset (an NVN handle is 64-bit)
            for (int k = 0; k < b->n; k++) {
                int tid = ubo_tex_id(ubo_handle(nvn_stage, reg, b->off + 8 * k)); if (tid >= g_ntreg || !g_treg[tid].ok) tid = 0;   // as for the samplers: other data, not a handle
                void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
                if (t && t.textureType == MTLTextureTypeTextureBuffer && b->dtt == MTLTextureType2D && b->f1d) { uint64_t r2 = 0; id<MTLTexture> v = tbuf_rows_binding(tid, t, b, k, tbias, &r2); if (v) { t = v; trid = r2; } }
                if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit_elem(tid, t, (MTLTextureType)b->dtt, &r2); t = v; trid = v ? r2 : 0; }   // a handle the shader does not pick here may be another kind
                if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt % DMY_NT][b->dpf & 3]; } else real |= 1ull << k;
                if (bl && tid) PD("B %llu st%d texu [%d] tid=%d %s", g_stat_pass, nvn_stage, k, tid, tv ? "real" : "dummy");
                if (b->dtt == (uint8_t)MTLTextureTypeTextureBuffer) t = tbuf_shadow(tid, t, &trid);   // see tbuf_shadow
                slots[aid + k] = trid;
                USE(t, MTLResourceUsageRead);
            }
            if (!(sh->logged & 4)) { sh->logged |= 4; static int nlog; if (nlog++ < 32) MLOG("shader %s (stage %d): an array of %d handles in uniform slot %d, elements %#llx hold textures", sh->key, nvn_stage, b->n, reg, (unsigned long long)real); }
        } else if (b->kind == NVN_RB_SAMP || b->kind == NVN_RB_SAMPT || b->kind == NVN_RB_SAMPU || b->kind == NVN_RB_SAMPI) {   // SAMPT: the sampler part of a combined handle (texture ID | sampler ID << 20); SAMPU: the same, with the handle in a uniform buffer
            int sid = bind_sid(nvn_stage, b);
            __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
            if (b->bw && ss && g_sview_w[sid]) {   // a twin (dmsh_rewrite.py border_emu) of a sampler whose border colour Metal lacks: the shader corrects its samples
                ss = (__bridge id<MTLSamplerState>)g_sview_w[sid]; slots[aid] = g_srid_w[sid];
                bmask |= 1u << (b->bw - 1); memcpy(bk[b->bw - 1], g_sbk[sid], sizeof bk[0]);
            } else slots[aid] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();   // a twin of any other sampler is never sampled
            USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            if (bl && sid > 0) { int ok = sid < g_nsreg && g_sreg_ok[sid]; samp_par sp = ok ? g_sreg[sid] : (samp_par){0};   // g_rmu is already held in this section
                if (ok) PD("B %llu st%d samp reg%d sid=%d mag=%d min=%d (0 nearest / 1 linear; 2–5 with mips) wrap=%d,%d,%d border=%g,%g,%g,%g lod=%g..%g bias=%g aniso=%g cmp=%d/%d %s", g_stat_pass, nvn_stage, reg, sid, (int)sp.magf, (int)sp.minf,
                           (int)sp.ws, (int)sp.wt, (int)sp.wr, sp.border[0], sp.border[1], sp.border[2], sp.border[3], sp.lod_min, sp.lod_max, sp.lod_bias, sp.aniso, (int)sp.cmp_mode, (int)sp.cmp_func, ss ? "real" : "failed"); }
        } else if (b->kind == NVN_RB_TEX || b->kind == NVN_RB_TEXU) {
            int tid = b->kind == NVN_RB_TEXU ? ubo_tex_id(ubo_bind_handle(nvn_stage, b)) : (int)(bound_tex(nvn_stage, reg) & 0xfffff);
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            int snap = fb_snapshot_slot(nvn_stage, t);
            if (snap >= 0) { t = g_fb_use[snap]; trid = g_fb_rid[snap]; }
            if (t && passdump() && tid < g_ntreg) PD("R %llu st%d ser=%llu", g_stat_pass, nvn_stage, g_treg[tid].t.serial);
            if (bl) PD("B %llu st%d tex reg%d tid=%d %s ser=%llu fmt=%lu %lux%lux%lu view_lv=%lu+%lu view_sl=%lu array=%lu target=%lu swz=%d%d%d%d mtl=%p", g_stat_pass, nvn_stage, reg, tid, t ? "real" : "dummy", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                       (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)t.depth,
                       (unsigned long)(t.parentTexture ? t.parentRelativeLevel : 0), (unsigned long)t.mipmapLevelCount,
                       (unsigned long)(t.parentTexture ? t.parentRelativeSlice : 0), (unsigned long)t.arrayLength, (unsigned long)t.textureType, t ? (int)t.swizzle.red : 0, t ? (int)t.swizzle.green : 0, t ? (int)t.swizzle.blue : 0, t ? (int)t.swizzle.alpha : 0, (__bridge void *)(t.parentTexture ? t.parentTexture : t));
            // A handle in a uniform buffer has no descriptor at build time, so a buffer texture there is declared 2D and read in rows like any 1D integer fetch (Unreal Engine 5.4's vertex factories on the Switch 2: UVs and colours)
            if (t && (b->f1d || b->kind == NVN_RB_TEXU) && t.textureType == MTLTextureTypeTextureBuffer && (MTLTextureType)b->dtt == MTLTextureType2D) { uint64_t r2 = 0; id<MTLTexture> v = tbuf_rows_binding(tid, t, b, 0, tbias, &r2); if (v) { t = v; trid = r2; } }   // see tbuf_as_2d
            if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit(tid, t, (MTLTextureType)b->dtt, &r2); if (v) { t = v; trid = r2; }
                // no view fits, and one side is a buffer texture (Octopath Traveler 0: a buffer texture left in a slot a shader reads as a cube): reads of it are undefined, so the slot reads 0
                else if (t.textureType == MTLTextureTypeTextureBuffer || (MTLTextureType)b->dtt == MTLTextureTypeTextureBuffer) { tex_mis_note(sh, nvn_stage, b->kind == NVN_RB_TEX ? reg : -1, tid, t, (MTLTextureType)b->dtt, "a buffer texture: the slot reads 0 (dummy)"); t = nil; }
                else tex_mis_note(sh, nvn_stage, b->kind == NVN_RB_TEX ? reg : -1, tid, t, (MTLTextureType)b->dtt, "bound as is"); }
            if (t && b->dpf == NVN_DPF_FLOAT && mtl_pf_dpf(t.pixelFormat) != NVN_DPF_FLOAT) { uint64_t r2 = 0; id<MTLTexture> v = tex_float_fit(tid, t, &r2); if (v) { t = v; trid = r2; } }   // see tex_float_fit
            if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt % DMY_NT][b->dpf & 3]; }
            else if (b->dtt == (uint8_t)MTLTextureTypeTextureBuffer) t = tbuf_shadow(tid, t, &trid);   // see tbuf_shadow
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_IMG || b->kind == NVN_RB_IMGU) {   // Storage image: NVN image slot r (BindImages) ↔ D3D u_r, or its handle in a uniform buffer (IMGU)
            int tid = b->kind == NVN_RB_IMGU ? ubo_tex_id(ubo_handle(nvn_stage, reg, b->off)) : reg < 32 ? (int)(bound_img(nvn_stage, reg) & 0xfffff) : 0;
            uint64_t trid; int real;
            id<MTLTexture> t = img_binding(tid, b, &trid, &real);
            int snap = real && g_fb_n && !b->rw && !b->atom && fb_image_view(tid, b, t) ? fb_snapshot_slot(nvn_stage, t) : -1;
            if (snap >= 0) { t = g_fb_use[snap]; trid = g_fb_rid[snap]; }
            else if (real && t.textureType == MTLTextureTypeTextureBuffer) {   // a buffer image: its view starts before its offset, and the shader moves its coordinates by the elements in between (tbuf_lead)
                uint32_t lead = tbuf_lead(tid, t);
                if (b->tbx) { if (tbias) tbias[b->tbx - 1] = lead; }
                else if (lead) RED_ONCE(119, "buffer image whose view starts %u elements before it (below Metal's view alignment), in shader %s (stage %d) with no offset table entry for it (an array of images, or past %d of them): its accesses land %u elements early",
                                        lead, sh->key, nvn_stage, DMSH_TBIAS_SLOTS, lead);
            }
            if (b->atom && !img_atomic_ok(t, b)) {   // see dummy_img_atomic; an unbound slot's write sink is not atomic-capable either
                if (real) { id<MTLTexture> base = t; while (base.parentTexture) base = base.parentTexture;
                    RED_ONCE(93, "image atomics on a texture Metal cannot run them on (shader %s stage %d slot %d texture ID %d: Metal format %lu of base format %lu, type %lu for %u, %s): they go to a write sink",
                             sh->key, nvn_stage, reg, tid, (unsigned long)t.pixelFormat, (unsigned long)base.pixelFormat, (unsigned long)t.textureType, b->dtt, base.usage & MTLTextureUsageShaderWrite ? "shader-writable" : "not shader-writable"); }
                t = dummy_img_atomic(b->dtt, b->dpf, b->atom == 2); trid = t.gpuResourceID._impl; real = 0;
            } else if (b->atom && !(sh->logged & 2)) { sh->logged |= 2; static int nlog; const tex_t *tt = tid < g_ntreg && g_treg[tid].ok ? &g_treg[tid].t : NULL;
                if (tt && nlog++ < 8) MLOG("shader %s (stage %d): image atomics on image slot %d, texture ID %d (target %d, %s, %llu bytes)", sh->key, nvn_stage, reg, tid, (int)tt->d.target,
                                           tt->d.target == NVN_TEXTURE_TARGET_BUFFER || (tt->d.flags & (NVN_TEXTURE_FLAGS_LINEAR | NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET)) ? "linear" : "block-linear", (unsigned long long)tt->L.size); }
            if (passdump()) PD("W %llu st%d reg%d tid=%d %s ser=%llu fmt=%lu %lux%lu view_sl=%lu array=%lu target=%lu shader wants target %u", g_stat_pass, nvn_stage, reg, tid, real ? "real" : "dummy", real && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                                  (unsigned long)t.pixelFormat, (unsigned long)t.width, (unsigned long)t.height, (unsigned long)(t.parentTexture ? t.parentRelativeSlice : 0), (unsigned long)t.arrayLength, (unsigned long)t.textureType, (unsigned)b->dtt);
            slots[aid] = trid;
            USE(t, snap >= 0 ? MTLResourceUsageRead : MTLResourceUsageRead | MTLResourceUsageWrite);
        } else if (b->kind == NVN_RB_TBUF || b->kind == NVN_RB_TBUFU) {   // Typed buffer: the buffer texture (target BUFFER) in texture slot r, or its handle in a uniform buffer (TBUFU)
            // A type-1 handle ORs two words, in either order, even for a samplerless fetch. Civilization VI's glyph vertices take the texture ID from the second word;
            // reading only the first (sampler) word bound the dummy buffer and collapsed the text. Use the same complete handle as ordinary bindless textures.
            int tid = b->kind == NVN_RB_TBUFU ? ubo_tex_id(ubo_bind_handle(nvn_stage, b)) : (int)(bound_tex(nvn_stage, reg) & 0xfffff);
            void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
            if (t && t.textureType != MTLTextureTypeTextureBuffer) { tex_mis_note(sh, nvn_stage, reg, tid, t, MTLTextureTypeTextureBuffer, "a buffer slot: it reads 0 (dummy)"); t = nil; }
            if (bl) PD("B %llu st%d tbuf reg%d tid=%d %s ser=%llu fmt=%#x n=%u mem=%p", g_stat_pass, nvn_stage, reg, tid, t ? "real" : "dummy", t && tid < g_ntreg ? g_treg[tid].t.serial : 0ull,
                       tid < g_ntreg ? g_treg[tid].t.d.format : 0, tid < g_ntreg ? g_treg[tid].t.d.width : 0, tid < g_ntreg ? (void *)g_treg[tid].t.mem : NULL);
            if (!t) { t = dummy_tbuf(b->dpf); trid = g_dummy_tbuf_rid[b->dpf & 3]; }
            else if (b->tbx) { if (tbias) tbias[b->tbx - 1] = tbuf_lead(tid, t); }   // read in place, the shader moving its coordinates by the elements before it (tbuf_lead)
            else t = tbuf_shadow(tid, t, &trid);   // see tbuf_shadow
            slots[aid] = trid;
            USE(t, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_TEXD) {   // dynamic handle array (profile key tex_dyn_slots): element k = the texture in slot k (combined, else separate), since c2 holds slot indices for this shader (see DRV2 below)
            uint64_t real = 0;   // the elements that got a texture of the declared kind (logged on the shader's first encode: logged bit 0)
            for (int k = 0; k < b->n; k++) {
                uint64_t hk = k < 128 ? (S.tex[nvn_stage][k] ? S.tex[nvn_stage][k] : S.tex[nvn_stage][NVN_SEPTEX_SLOT0 + k]) : 0; int tid = (int)(bound_handle(hk) & 0xfffff);
                void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
                if (b->dtt == (uint8_t)MTLTextureTypeTextureBuffer) {
                    if (t && t.textureType != MTLTextureTypeTextureBuffer) t = nil;   // a slot the shader does not pick here may hold another kind of texture
                    if (!t) { t = dummy_tbuf(b->dpf); trid = g_dummy_tbuf_rid[b->dpf & 3]; }
                    else if (b->tbx) { if (tbias) tbias[b->tbx - 1 + k] = tbuf_lead(tid, t); }   // in place, element k's entry (tbuf_lead)
                    else t = tbuf_shadow(tid, t, &trid);   // see tbuf_shadow
                } else {
                    if (t && t.textureType == MTLTextureTypeTextureBuffer && b->dtt == MTLTextureType2D && b->f1d) { uint64_t r2 = 0; id<MTLTexture> v = tbuf_rows_binding(tid, t, b, k, tbias, &r2); if (v) { t = v; trid = r2; } }
                    if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit_elem(tid, t, (MTLTextureType)b->dtt, &r2); t = v; trid = v ? r2 : 0; }
                    if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt % DMY_NT][b->dpf & 3]; }
                }
                if (bl && tid) PD("B %llu st%d texd [%d] tid=%d %s", g_stat_pass, nvn_stage, k, tid, tv ? "real" : "dummy");
                if (tv && trid && trid != g_dummy_tbuf_rid[b->dpf & 3] && trid != g_dummy_tex_rid[b->dtt % DMY_NT][b->dpf & 3]) real |= 1ull << k;
                slots[aid + k] = trid;
                USE(t, MTLResourceUsageRead);
            }
            if (!(sh->logged & 1)) { sh->logged |= 1; static int nlog; if (nlog++ < 32) MLOG("shader %s (stage %d): dynamic texture handles, slots %#llx of its %d-entry array hold %s", sh->key, nvn_stage,
                (unsigned long long)real, b->n, b->dtt == (uint8_t)MTLTextureTypeTextureBuffer ? "buffer textures" : "textures"); }
        } else if (b->kind == NVN_RB_TEXP || b->kind == NVN_RB_SAMPP) {   // a pool indexed by the handle's ID (see pool_gpu)
            __unsafe_unretained id<MTLBuffer> pb = nil;
            slots[aid] = b->kind == NVN_RB_SAMPP ? pool_gpu(1, 0, 0, S.samp_base, &pb) : pool_gpu(0, b->dtt, b->dpf, S.tex_base, &pb);
            USE(pb, MTLResourceUsageRead);
            if (bl) PD("B %llu st%d %s pool base %d", g_stat_pass, nvn_stage, b->kind == NVN_RB_SAMPP ? "sampler" : "texture", b->kind == NVN_RB_SAMPP ? S.samp_base : S.tex_base);
        } else if (b->kind == NVN_RB_TEXH || b->kind == NVN_RB_SAMPH) {   // a table of a uniform buffer's handles (see htab_gpu)
            __unsafe_unretained id<MTLBuffer> pb = nil;
            slots[aid] = htab_gpu(nvn_stage, b, &pb);
            USE(pb, MTLResourceUsageRead);
            if (bl) PD("B %llu st%d %s table of uniform slot %d +%#x", g_stat_pass, nvn_stage, b->kind == NVN_RB_SAMPH ? "sampler" : "texture", reg, b->off);
        } else if (b->kind == NVN_RB_TEXA) {   // an array of the driver table's handles that the shader indexes at run time (nvn_refl.h): element k = the texture of the c2 word at off + 8k
            uint64_t real = 0;                  // (elements past 63 are not in the log's mask)
            for (int k = 0; k < b->n; k++) {
                int tid = (int)(c2_handle(nvn_stage, b->off + 8u * k) & 0xfffff);   // a separate sampler word ORed in (soff) holds only a sampler ID
                void *tv = NULL; uint64_t trid = tid ? tex_fast_rid(tid, &tv) : 0; __unsafe_unretained id<MTLTexture> t = (__bridge id<MTLTexture>)tv;
                if (t && t.textureType == MTLTextureTypeTextureBuffer && b->dtt == MTLTextureType2D && b->f1d) { uint64_t r2 = 0; id<MTLTexture> v = tbuf_rows_binding(tid, t, b, k, tbias, &r2); if (v) { t = v; trid = r2; } }
                if (t && t.textureType != (MTLTextureType)b->dtt) { uint64_t r2 = 0; id<MTLTexture> v = tex_fit_elem(tid, t, (MTLTextureType)b->dtt, &r2); t = v; trid = v ? r2 : 0; }   // an element the shader does not pick here may be another kind
                if (!t) { t = dummy_tex(b->dtt, b->dpf); trid = g_dummy_tex_rid[b->dtt % DMY_NT][b->dpf & 3]; } else if (k < 64) real |= 1ull << k;
                if (bl && tid) PD("B %llu st%d texa [%d] tid=%d %s", g_stat_pass, nvn_stage, k, tid, tv ? "real" : "dummy");
                if (b->dtt == (uint8_t)MTLTextureTypeTextureBuffer) {
                    if (b->tbx) { if (tbias) tbias[b->tbx - 1 + k] = tbuf_lead(tid, t); }   // in place, element k's entry (tbuf_lead; 0 for a dummy)
                    else t = tbuf_shadow(tid, t, &trid);   // see tbuf_shadow
                }
                slots[aid + k] = trid;
                USE(t, MTLResourceUsageRead);
            }
            if (!(sh->logged & 8)) { sh->logged |= 8; static int nlog; if (nlog++ < 32) MLOG("shader %s (stage %d): an array of %d driver-table handles from c2 byte %#x, elements %#llx hold textures", sh->key, nvn_stage, b->n, b->off, (unsigned long long)real); }
        } else if (b->kind == NVN_RB_SAMPA) {   // its samplers: element k = the sampler of the handle at c2 byte off + 8k, ORed with the separate sampler word at soff (handle type 1) as the GPU does
            uint64_t sw = b->soff ? c2_handle(nvn_stage, b->soff) : 0;
            for (int k = 0; k < b->n; k++) {
                int sid = (int)((c2_handle(nvn_stage, b->off + 8u * k) | sw) >> 20);
                __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
                slots[aid + k] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
                USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_SAMPD) {   // its samplers: element k = the separate sampler in slot k, else the sampler of a combined handle in texture slot k
            for (int k = 0; k < b->n; k++) {
                int sid = k < 128 ? (int)(bound_handle(S.samp[nvn_stage][k]) >> 20) : 0;
                if (!sid && k < NTEX) sid = (int)(bound_handle(S.tex[nvn_stage][k]) >> 20);
                __unsafe_unretained id<MTLSamplerState> ss = sid ? (__bridge id<MTLSamplerState>)samp_fast(sid) : nil;
                slots[aid + k] = sid && sid < g_nsreg && g_srid[sid] ? g_srid[sid] : dummy_samp_rid();
                USE(ss ? ss : dummy_samp(), MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_SBUFP || b->kind == NVN_RB_UBOP) {   // B: storage buffers / compute-stage uniforms are device pointers; the address is taken by NVN slot
            const p_bind_buf *u = reg < 16 ? (b->kind == NVN_RB_UBOP ? &S.ubo[nvn_stage][reg] : &S.ssbo[nvn_stage][reg]) : NULL;
            void *mb; uint64_t mo, gpu;
            if (u && u->addr && mtl_gpu_addr(u->addr, u->size ? u->size : 1, &mb, &mo, &gpu)) { slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); if (b->rw && b->kind == NVN_RB_SBUFP && !(reg < 32 && (sh->sb_ro >> reg & 1))) sbw_note(u->addr, sbw_len(u->size, mb, mo), SBW_GPU); }   // cb_extent.tsv: slots the shader never writes
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); }
            if (bl) PD("B %llu st%d sbufp reg%d %s addr=%#llx size=%llu %s", g_stat_pass, nvn_stage, reg, u && u->addr ? "real" : "dummy", u ? (unsigned long long)u->addr : 0ull, u ? (unsigned long long)u->size : 0ull, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_SBUFU) {   // B: a storage buffer whose GPU address the game keeps in its own uniform buffer (Monster Hunter Rise's compute shaders: c3 + 0x38); read like a bindless handle
            uint64_t addr; ubo_read(nvn_stage, reg, b->off, 8, &addr); addr = ucb_handle_addr(addr);
            void *mb; uint64_t mo, gpu;
            if (addr && mtl_gpu_addr(addr, 1, &mb, &mo, &gpu)) { slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); if (b->rw) sbw_note(addr, sbw_len(0, mb, mo), SBW_GPU);
                                                                 sblen_put(sblen, b, (__bridge id<MTLBuffer>)mb, mo); }   // size not known: bounded by the buffer that holds it
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); sblen_put(sblen, b, nil, 0); }
            if (bl) PD("B %llu st%d sbufu reg%d+%#x addr=%#llx %s", g_stat_pass, nvn_stage, reg, b->off, (unsigned long long)addr, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_SBUFQ) {   // B: a storage buffer whose GPU address the game pushed (c[24] at b->off, both blocks: PRAGMATA's compute buffer entries); resolved like NVN_RB_SBUFU
            uint64_t addr = 0; const uint8_t *pb = S.push[nvn_stage == 5 ? 1 : 0]; if (b->off + 8u <= sizeof S.push[0]) memcpy(&addr, pb + b->off, 8);
            addr = ucb_handle_addr(addr);
            void *mb; uint64_t mo, gpu;
            if (addr && mtl_gpu_addr(addr, 1, &mb, &mo, &gpu)) { slots[aid] = gpu; USE((__bridge id<MTLBuffer>)mb, b->rw ? MTLResourceUsageRead | MTLResourceUsageWrite : MTLResourceUsageRead); if (b->rw) sbw_note(addr, sbw_len(0, mb, mo), SBW_GPU);
                                                                 sblen_put(sblen, b, (__bridge id<MTLBuffer>)mb, mo); }
            else { slots[aid] = dummy_buf_gpu(); USE(dummy_buf(), MTLResourceUsageRead); sblen_put(sblen, b, nil, 0);
                   // An unmapped page of a sparse pool's reservation reads 0 on the GPU, as the dummy does: Resident Evil 4's 0ee0c5f004239ce2 at frame 5 is pushed an address 0x4400000 into
                   // a 1 GB sparse pool, on a page the game has not mapped by then. Noted once, not red; any other address is
                   static int sp_noted; int nvn_vpool_unmapped(uint64_t); int sp = (!sp_noted || !g_red_once[165]) && nvn_vpool_unmapped(addr);
                   if (sp && !sp_noted) { sp_noted = 1; MLOG("shader %s: the buffer address the game pushed at c[24] + %#x, %#llx, is an unmapped page of a sparse pool: bound to the zero buffer, as the GPU reads such a page as 0 (frame %llu)",
                                                             sh->key, b->off, (unsigned long long)addr, (unsigned long long)atomic_load(&g_cur_frame)); }
                   else if (!sp && !g_red_once[165]) { char note[400]; dm_gfx_addr_note(addr, note, sizeof note);   // what is known of it: a sparse pool's page, a texture's storage
                       RED_ONCE(165,"shader %s: the buffer address the game pushed at c[24] + %#x is %#llx, in no memory pool: bound to a dummy buffer (frame %llu;%s)", sh->key, b->off, (unsigned long long)addr, (unsigned long long)atomic_load(&g_cur_frame), note); } }
            if (bl) PD("B %llu st%d sbufq push+%#x addr=%#llx %s", g_stat_pass, nvn_stage, b->off, (unsigned long long)addr, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_C1) {      // B: the shader's own constant segment, built when the shader is fetched
            id<MTLBuffer> cb = sh->c1buf ? sh->c1buf : zero_bank();   // c1 has length 0 (600 of them in the whole library) yet the shader still declares it: reads 0
            slots[aid] = (sh->c1buf ? sh->c1_gpu : zero_bank_gpu()) + (sh->c1buf ? sh->c1off : 0);
            USE(cb, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_GMEM || b->kind == NVN_RB_GMEML) {   // B: the memory pool table for addresses traced to no storage buffer (gmem_map), and the write log of those stores (gml_take)
            if (!raw_map) raw_map = gmem_map();   // the log and the shader use the same table even if a pool comes in during the encode
            slots[aid] = b->kind == NVN_RB_GMEM ? raw_map.gpuAddress : gml_take(raw_map);
            if (b->kind == NVN_RB_GMEM) gmem_aid = aid; else gml_taken = 1;
            if (bl && b->kind == NVN_RB_GMEM) PD("B %llu st%d gmem %s", g_stat_pass, nvn_stage, b->rw ? "rw" : "ro");
        } else if (b->kind == NVN_RB_SUPPORT) {   // support_buffer: scale 1.0, BGRA swap 0, viewport transform not disabled (viewport fields are not read)
            slots[aid] = support_gpu();
            USE(support_buf(), MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_CBSIZE) {   // the uniform slots' bound sizes (cbsize_table)
            id<MTLBuffer> db; slots[aid] = cbsize_table(nvn_stage, &db); USE(db, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_TBIAS) {   // the buffer textures' element offsets (tbias above), filled as the bindings are encoded
            slots[aid] = tbias_gpu; USE(tbias_buf, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_SBLEN) {   // the bounded storage buffers' lengths (sblen above), filled as they are encoded
            slots[aid] = sblen_gpu; USE(sblen_buf, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_PUSH) {   // NVN2 push constants (c[24]): the stage group's current block
            id<MTLBuffer> db; arena_at at = arena_take(sizeof S.push[0], &db);
            memcpy(at.cpu, S.push[nvn_stage == 5 ? 1 : 0], sizeof S.push[0]);
            slots[aid] = at.gpu; USE(db, MTLResourceUsageRead);
        } else if (b->kind == NVN_RB_DRV0 || b->kind == NVN_RB_DRV2) {
            // NVN driver constant segments: those proven unread by static analysis at packaging time are supplied as 0; those read are filled on the spot from the current bindings - c0 = storage buffer table (base + 0x10 r: address 64 bits, size 32 bits),
            // c2 = the handle tables (DMSH_C2_*), the values being the NVN handles the game gave at bind time; positions read outside the tables are registered at packaging time and reported a failure at load time
            int bank = b->kind == NVN_RB_DRV2 ? 2 : 0;
            if (!sh->rf.drv_read[bank]) { slots[aid] = zero_bank_gpu(); USE(zero_bank(), MTLResourceUsageRead); }
            else {
                id<MTLBuffer> db; arena_at at = arena_take(DMSH_DRV_SIZE, &db); size_t doff = at.off; uint8_t *d = at.cpu; memset(d, 0, DMSH_DRV_SIZE);
                if (bank == 0) {
                    int base = c0_ssbo_base(nvn_stage);
                    if (base < 0) RED_ONCE(21, "stage %d shader reads driver segment c0; the storage buffer table base was never measured", nvn_stage);
                    else for (int r = 0; r < DMSH_C0_SSBO_SLOTS; r++) { uint64_t a = S.ssbo[nvn_stage][r].addr; uint32_t n = (uint32_t)S.ssbo[nvn_stage][r].size; memcpy(d + base + DMSH_C0_ROW * r, &a, 8); memcpy(d + base + DMSH_C0_ROW * r + 8, &n, 4); }
                    // Compute also has the dispatch size at 0x200 (x, y, z) and the uniform buffer table at 0x210 + 16 r (address 64 bits, size 32 bits), ending where the storage buffer table
                    // starts (0x310). Measured on Link's Awakening's light culling (light_culling_compute.bfsha): tile index = workgroup y × c0[0x200] + workgroup x, and c0[0x280] is
                    // the address of uniform buffer 7 (the light parameters, 144 bytes per light); Breath of the Wild's compute shaders read 0x280 / 0x290 / 0x2d0 of the same table
                    // Graphics stages: 0x610, right after the pixel stage's storage buffer table, holds the largest point size, to which GLSLC clamps a shader-written
                    // gl_PointSize (Super Mario Odyssey's agl point sprites, alPointSpriteShader: gl_PointSize = min(size attribute, c0[0x610])). It is not SetPointSize: the game
                    // sets that to 1 and sizes its sprites itself (the fog splats ask for 3-18 px). Left at 0, every sprite had size 0, so the splats that push the Cap Kingdom
                    // fog aside never reached its velocity fields and the fog did not part around Mario. 2047 is Maxwell's point size limit (Metal clamps to its own)
                    if (nvn_stage != 5) { const float max_point = 2047.0f; memcpy(d + DMSH_C0_POINT_SIZE, &max_point, 4); }
                    // Graphics stages: 0x644 holds the draw's base instance. Maxwell's instance ID leaves it out (GL's gl_InstanceID), and GLSLC adds this word where a shader needs the
                    // absolute instance (gl_InstanceID + gl_BaseInstance): Pokémon Legends: Z-A's occlusion boxes (a793f436dcabd350, one box per draw at base instance 61, 62, …) pick their
                    // transform this way, and 1,550 reads in 793 of its vertex shaders. Left at 0, every box was box 0: all six queries came back empty, and the game, taking its characters for
                    // hidden, posed their legs straight back from the hips. 0x640 (read by 58 shaders, added to the vertex ID) stays 0: what the driver puts there is not established
                    if (nvn_stage != 5) memcpy(d + DMSH_C0_BASE_INSTANCE, &g_draw_binst, 4);
                    if (nvn_stage == 1) c0_sample_rows(d, g_pass.samples);
                    if (nvn_stage == 5) {
                        memcpy(d + DMSH_C0_DISPATCH, g_dispatch_dim, 12); g_c0_dim_buf = db; g_c0_dim_off = doff + DMSH_C0_DISPATCH;
                        // NVN2 compute also reads them below its per-shader layout: a light culling shader's cluster is (z * c0[8] + y) * c0[4] + x, and at zero every cluster of a column wrote the same entry
                        memcpy(d + DMSH_C0_DISPATCH_NVN2, g_dispatch_dim, 12);
                    }
                    // Compute's uniform slots at 0x210 + 16 r (address 64 bits, size 32 bits). NVN2's compute shaders read a slot through its first 8 bytes as a bindless constant buffer
                    // handle too (cx[UR], InstEmitAmpere.cs ABindlessConstant), which NVN2's driver writes there in the hardware's layout (ucb_handle).
                    // NVN2's graphics stages read no such table: their bank holds the entries the game pushes (dmsh_abi.h DMSH_NVN2_STAGE_BANK)
                    if (nvn_stage == 5) for (int r = 0; r < DMSH_C0_CS_UBO_SLOTS; r++) { uint64_t a = S.ubo[nvn_stage][r].addr; uint32_t n = (uint32_t)S.ubo[nvn_stage][r].size; a = ucb_handle(a, n); memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r, &a, 8); memcpy(d + DMSH_C0_CS_UBO + DMSH_C0_ROW * r + 8, &n, 4); }
                } else {
                    // A shader with dynamic handle arrays (TEXD / SAMPD, profile key tex_dyn_slots) picks a table slot at run time and ORs a texture word with a sampler word into a handle,
                    // whose texture / sampler IDs index the arrays. For it the tables hold slot indices instead of handles (texture slot r → r, and r << 20 when a combined handle carries
                    // a sampler; sampler slot r → r << 20), and the arrays hold the bound slots, so the shader's own arithmetic picks the binding (Octopath Traveler 0: separate texture slots 0-10, samplers 0-4)
                    // Pool shaders index relative descriptors; shaders whose tables name host resources take the active bases, as do ordinary slot bindings.
                    const uint64_t *tx = S.tex[nvn_stage], *im = S.img[nvn_stage]; int dyn = sh->rf.dyn;
                    #define C2_HANDLE(h) ((uint32_t)(sh->rf.pool ? (h) : bound_handle(h)))
                    for (int r = 0; r < DMSH_C2_IMG_SLOTS; r++) { uint32_t ih = C2_HANDLE(im[r]); memcpy(d + DMSH_C2_IMG + DMSH_C2_ROW * r, &ih, 4); }
                    for (int r = 0; r < DMSH_C2_TEX_SLOTS; r++) {   // a combined slot past 31 is written when it is bound and the image slot it shares is not (Hades II binds texture slot 34, no image)
                        if (r >= 32 && (!tx[r] || (r - 32 < DMSH_C2_IMG_SLOTS && im[r - 32]))) continue;
                        uint32_t v = !dyn ? C2_HANDLE(tx[r]) : !tx[r] ? 0 : (uint32_t)r | (tx[r] >> 20 ? (uint32_t)r << 20 : 0); memcpy(d + DMSH_C2_TEX + DMSH_C2_ROW * r, &v, 4); }
                    for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint64_t h = tx[NVN_SEPTEX_SLOT0 + r]; uint32_t v = !dyn ? C2_HANDLE(h) : h ? (uint32_t)r : 0; memcpy(d + DMSH_C2_SEPTEX + DMSH_C2_ROW * r, &v, 4); }
                    for (int r = 0; r < DMSH_C2_SEP_SLOTS; r++) { uint64_t h = S.samp[nvn_stage][r]; uint32_t v = !dyn ? C2_HANDLE(h) : h ? (uint32_t)r << 20 : 0; memcpy(d + DMSH_C2_SAMP + DMSH_C2_ROW * r, &v, 4); }
                    #undef C2_HANDLE
                }
                slots[aid] = at.gpu;
                USE(db, MTLResourceUsageRead);
            }
        } else if (b->kind == NVN_RB_BORDER) continue;   // filled below, once every twin has been seen
        else RED_ONCE(46, "binding kind %d not implemented", b->kind);
    }
    if (sh->rf.border) {   // the border colour block: all zero (the shared zero bank) unless a twin corrects, then its mask and factors (dmsh_abi.h DMSH_BORDER_*)
        int reg = 0, aid = sh->rf.b[sh->rf.border - 1].aid;
        if (!bmask) { slots[aid] = zero_bank_gpu(); USE(zero_bank(), MTLResourceUsageRead); }
        else {
            int n = 32 - __builtin_clz(bmask); id<MTLBuffer> db; arena_at at = arena_take(DMSH_BORDER_K + 16 * n, &db); uint8_t *d = at.cpu;
            memset(d, 0, DMSH_BORDER_K); memcpy(d, &bmask, 4); memcpy(d + DMSH_BORDER_K, bk, 16 * n);
            slots[aid] = at.gpu; USE(db, MTLResourceUsageRead);
            if (bl) PD("B %llu st%d border mask=%#x", g_stat_pass, nvn_stage, bmask);
        }
    }
    #undef USE
    os_unfair_lock_unlock(&g_rmu);
    args_check_addresses(&sh->rf, slots, arg_len, nvn_stage);
    id<MTLBuffer> ab; arena_at at = arena_take(arg_len, &ab); size_t off = at.off;
    memcpy(at.cpu, slots, arg_len);
    if (gmem_aid >= 0) gmem_slot_note((uint64_t *)(void *)at.cpu + gmem_aid, gml_taken ? (__bridge void *)g_gml_last : NULL);   // the arena's own CPU address (no contents message: see ubo_read)
    bind_args(en, nvn_stage, metal_vertex, ab, at.gpu, off, sh->rf.abi);
    sh->ab_buf = ab; sh->ab_gpu = at.gpu; sh->ab_off = off; sh->ab_pass = g_pass.serial; sh->ab_binst = g_draw_binst; sh->ab_pgen = g_psnap_gen; sh->ab_gen = g_args_gen[nvn_stage]; sh->ab_ugen = g_args_ugen[nvn_stage]; g_args_wrote[nvn_stage] = g_args_seq;
    g_stat_args_enc++;
    return 1;
}
static id<MTLBuffer> g_quad_ib; static uint64_t g_quad_gpu;
static id<MTLBuffer> g_fan_ib; static uint64_t g_fan_gpu;   // non-indexed triangle fans: shared indices (0, i, i+1), base vertex = first
#define SHARED_IB_N 65536   // quads / fan triangles the shared index buffers cover
static void quad_ib_make(void) {   // Quads: shared indices 0,1,2,0,2,3
    if (g_quad_ib) return;
    uint32_t n = SHARED_IB_N; uint32_t *ix = malloc(n * 6 * 4);
    for (uint32_t q = 0; q < n; q++) { uint32_t b = q * 4; uint32_t v[6] = {b, b + 1, b + 2, b, b + 2, b + 3}; memcpy(ix + q * 6, v, 24); }
    g_quad_ib = [g_dev newBufferWithBytes:ix length:n * 24 options:MTLResourceStorageModeShared]; g_quad_gpu = g_quad_ib.gpuAddress; res_add(g_quad_ib); free(ix);
}
static void fan_ib_make(void) {
    if (g_fan_ib) return;
    uint32_t n = SHARED_IB_N; uint32_t *ix = malloc(n * 12);
    for (uint32_t i = 0; i < n; i++) { uint32_t v[3] = {0, i + 1, i + 2}; memcpy(ix + i * 3, v, 12); }
    g_fan_ib = [g_dev newBufferWithBytes:ix length:n * 12 options:MTLResourceStorageModeShared]; g_fan_gpu = g_fan_ib.gpuAddress; res_add(g_fan_ib); free(ix);
}
static id<MTL4RenderCommandEncoder> rp_begin(void);
