// The present: screenshots, DM_NVN_TEXDUMP, the crash dump's address lookup, presentation times, the picture's rectangle, upscaling in the present.
// Part of the replay layer: nvn_mtl.m includes it in its place, in one translation unit, after the parts it uses.
// ---------- Present: draw the swapchain texture to the CAMetalLayer; optional screenshot ----------
static id<MTLRenderPipelineState> g_blit_pso; static MTLPixelFormat g_blit_fmt;
static id<MTLSamplerState> g_blit_samp;
static uint64_t g_present_n;
// Screenshot on request (DM_CTL's "shot <path>", hle_hid.c): dm_gfx_shot_at(k, path) = take a screenshot at the kth frame after the current present frame; due requests are checked at every frame's present
#define SHOTQ_MAX 8
static struct { uint64_t frame; char path[1024]; } g_shotq[SHOTQ_MAX]; static int g_nshotq;
static pthread_mutex_t g_shotq_mu = PTHREAD_MUTEX_INITIALIZER;
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]);
static int present_crop_rect(id<MTLTexture> src, const int crop[4], NSUInteger r[4]);
enum { PRESENT_FAILED, PRESENT_STANDARD, PRESENT_METALFX };
static int present_offscreen_m4(id<MTLTexture> src, const int crop[4], id<MTLTexture> out, void (^done)(int));
static int present_offscreen_wait(id<MTLTexture> src, const int crop[4], id<MTLTexture> out);
// Game picture needed by the whole-window screenshot (front end window_shot; no longer does drawViewHierarchy on a Metal layer with the display link attached): at the next present, the present thread draws the same frame
// with the same encoding as the present into an offscreen texture of the view's size, and after the GPU finishes hands out a CGImage in the completion callback. Guarded by g_shotq_mu
static void (^g_grab_cb)(CGImageRef img);
void mtl_grab_next_present(void (^cb)(CGImageRef img)) { pthread_mutex_lock(&g_shotq_mu); g_grab_cb = [cb copy]; pthread_mutex_unlock(&g_shotq_mu); }
static void grab_present(id<MTLTexture> src, const int crop[4], CGSize sz) {
    pthread_mutex_lock(&g_shotq_mu); void (^cb)(CGImageRef) = g_grab_cb; g_grab_cb = nil; pthread_mutex_unlock(&g_shotq_mu);
    if (!cb) return;
    size_t w = (size_t)sz.width, h = (size_t)sz.height, row = w * 4;
    MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
    id<MTLTexture> out = w && h ? [g_dev newTextureWithDescriptor:d] : nil; id<MTLBuffer> buf = out ? [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared] : nil;
    if (!buf) { MLOG("[RED] full-window screenshot: cannot create output (%zux%zu)", w, h); cb(NULL); return; }
    // GPU completion starts the readback, keeping the present thread free. Each capture owns its scaler and textures until completion.
    void (^readback)(int) = ^(int ok) {
        if (!ok) { cb(NULL); return; }
        id<MTLCommandBuffer> cb2 = q_cb(); id<MTLBlitCommandEncoder> b = [cb2 blitCommandEncoder];
        [b copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
        [b endEncoding];
        [cb2 addCompletedHandler:^(id<MTLCommandBuffer> c) { (void)c;
            CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
            CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little);
            CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL; cb(img);
            CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs); }];
        [cb2 commit];
    };
    if (g_q4) { if (!present_offscreen_m4(src, crop, out, readback)) cb(NULL); }
    else {
        id<MTLCommandBuffer> draw = q_cb();
        if (!present_encode(draw, src, out, crop)) { cb(NULL); return; }
        [draw addCompletedHandler:^(id<MTLCommandBuffer> c) { readback(c.status == MTLCommandBufferStatusCompleted); }]; [draw commit];
    }
}
void dm_gfx_shot_at(long k, const char *path) {
    pthread_mutex_lock(&g_shotq_mu);
    if (g_nshotq < SHOTQ_MAX) { g_shotq[g_nshotq].frame = g_present_n + (uint64_t)(k > 0 ? k : 0); strlcpy(g_shotq[g_nshotq].path, path, sizeof g_shotq[0].path); g_nshotq++; }
    else MLOG("[RED] more than %d screenshot requests; dropping %s", SHOTQ_MAX, path);
    pthread_mutex_unlock(&g_shotq_mu);
}
// DM_NVN_SHOT_OUT=WIDTHxHEIGHT (for picture quality metrics): the present-frame screenshot instead captures the result "upscaled to this resolution by the present pass", i.e. the picture the player sees on a screen
// of that size (with DM_PRESENT_UPSCALE=metalfx through MetalFX, on the Metal 4 queue: present_offscreen_m4); screen=0 (texture dump) is unaffected.
// Without it, a window the game crops (nvnWindowSetCrop) is captured the same way at the window texture's own size: what is outside the crop is never presented and holds
// whatever the texture held before (Bastion crops its 1920x1080 window textures to 1600x900; the strip outside was black in some runs and purple in others, which regress.py
// took for a rendering change). An uncropped window is read as it is, so its shots are the texture's own pixels
static void shot(id<MTLTexture> src, const char *path, int screen, const int crop[4]) {
    static int out_w = -1, out_h;
    if (out_w < 0) {
        out_w = 0;
        const char *e = getenv("DM_NVN_SHOT_OUT"); int w = 0, h = 0; char sep = 0, extra = 0;
        if (e && sscanf(e, "%d%c%d%c", &w, &sep, &h, &extra) == 3 && sep == 'x' && w > 0 && h > 0) { out_w = w; out_h = h; }
    }
    id<MTLTexture> read = src;
    NSUInteger r[4]; int cropped = screen && !out_w && present_crop_rect(src, crop, r) && (r[0] || r[1] || r[2] != src.width || r[3] != src.height);
    if ((out_w || cropped) && screen) {
        NSUInteger ow = out_w ? (NSUInteger)out_w : src.width, oh = out_w ? (NSUInteger)out_h : src.height;
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:ow height:oh mipmapped:NO];
        d.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate;
        id<MTLTexture> out = [g_dev newTextureWithDescriptor:d];
        if (!out) { MLOG("[RED] cannot create the screenshot output texture %lux%lu", (unsigned long)ow, (unsigned long)oh); return; }
        if (g_q4) { if (!present_offscreen_wait(src, crop, out)) return; }
        else { id<MTLCommandBuffer> cb = q_cb(); if (!present_encode(cb, src, out, crop)) return; [cb commit]; [cb waitUntilCompleted]; }
        read = out;
    }
    if (read.pixelFormat != MTLPixelFormatRGBA8Unorm && read.pixelFormat != MTLPixelFormatRGBA8Unorm_sRGB &&
        read.pixelFormat != MTLPixelFormatBGRA8Unorm && read.pixelFormat != MTLPixelFormatBGRA8Unorm_sRGB) { MLOG("[RED] screenshot does not support format %lu", (unsigned long)read.pixelFormat); return; }
    size_t w = read.width, h = read.height, row = w * 4;
    id<MTLBuffer> buf = [g_dev newBufferWithLength:row * h options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb();
    id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    [b copyFromTexture:read sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, 1)
              toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:row * h];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    if (cb.status != MTLCommandBufferStatusCompleted) { MLOG("[RED] screenshot %s: Metal readback did not complete", path); return; }
    if (screen && read == src && atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) {   // lower-left origin: the window image is stored bottom-up (see g_origin_ll)
        uint8_t *p = buf.contents, *tmp = malloc(row);
        if (tmp) { for (size_t y = 0; y < h / 2; y++) { memcpy(tmp, p + y * row, row); memcpy(p + y * row, p + (h - 1 - y) * row, row); memcpy(p + (h - 1 - y) * row, tmp, row); } free(tmp); } }
    int bgra = read.pixelFormat == MTLPixelFormatBGRA8Unorm || read.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB;
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGBitmapInfo bi = bgra ? (kCGImageAlphaNoneSkipFirst | kCGBitmapByteOrder32Little) : (CGBitmapInfo)kCGImageAlphaNoneSkipLast;
    CGContextRef ctx = CGBitmapContextCreate(buf.contents, w, h, 8, row, cs, bi);
    CGImageRef img = ctx ? CGBitmapContextCreateImage(ctx) : NULL;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)path, strlen(path), false);
    CGImageDestinationRef dst = url ? CGImageDestinationCreateWithURL(url, (__bridge CFStringRef)UTTypePNG.identifier, 1, NULL) : NULL;
    // If any step yields null (directory does not exist / not writable, bitmap context cannot be created), report a failure and return; previously it went straight to CFRelease(NULL) and crashed in CFRelease.cold (hit during a 1952-wide TEXDUMP)
    int ok = 0;
    if (!ctx || !img || !dst) MLOG("[RED] screenshot %s failed: %s", path, !ctx ? "cannot create the bitmap context" : !img ? "cannot get the image" : "cannot write (directory missing or not writable)");
    else { CGImageDestinationAddImage(dst, img, NULL); ok = CGImageDestinationFinalize(dst); }
    if (dst) CFRelease(dst); if (url) CFRelease(url); CGImageRelease(img); CGContextRelease(ctx); CGColorSpaceRelease(cs);
    MLOG("screenshot %zux%zu format %lu → %s %s", w, h, (unsigned long)read.pixelFormat, path, ok ? "ok" : "failed");
}
// ---------- DM_NVN_TEXDUMP=frame number:path (%d in the path is replaced with "index*100+mip level"): save each color render target used by that frame as an image ----------
// Diagnostic: before the first render pass of the next frame is opened, wait for the GPU to finish, then draw each one (the mip level used by that frame) by full-screen sampling into RGBA8 and save as PNG (float formats are clamped to [0,1])
static long g_td_frame = -2; static const char *g_td_path; static NSMutableArray *g_td_list; static int g_td_done;
// DM_NVN_TEXDUMP=layers=<n>:path: the first frame with at least n draws into single layers of small (at most 64x64) array or cube texture levels, counted per pass or slice-merged layer
// (Breath of the Wild's cube map filter frame, every tenth: about 150 against 20): its targets are kept frame by frame and dumped when such a frame ends, the same frame with or without DM_NVN_SLICE_MERGE
static int g_td_layers, g_td_lcount; static long g_td_lframe = -1;
static void td_layer_count(id<MTLTexture> t, NSUInteger lv) { if (t && (t.textureType == MTLTextureType2DArray || t.textureType == MTLTextureTypeCube) && MAX(t.width >> lv, 1ul) <= 64) g_td_lcount++; }
static void td_dump(void);
// DM_NVN_TEXDUMP=size=WxH:path instead picks the frame by content: the first frame that opens a render pass whose first color attachment level is WxH (for one-off work whose frame differs between runs)
static unsigned g_td_w, g_td_h;
void td_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    if (g_td_frame == -2) { const char *e = getenv("DM_NVN_TEXDUMP"), *c = e ? strchr(e, ':') : NULL; g_td_frame = c ? atol(e) : -1; g_td_path = c ? c + 1 : NULL; g_td_list = [NSMutableArray array];
        if (c && !strncmp(e, "size=", 5) && sscanf(e + 5, "%ux%u", &g_td_w, &g_td_h) == 2) g_td_frame = LONG_MAX;
        if (c && !strncmp(e, "layers=", 7)) { g_td_layers = atoi(e + 7); g_td_frame = (long)atomic_load(&g_cur_frame); } }
    if (g_td_frame < 0 || g_td_done) return;
    long f = (long)atomic_load(&g_cur_frame);
    if (g_td_layers) {
        if (f != g_td_lframe) { if (g_td_lframe >= 0 && g_td_lcount >= g_td_layers) { MLOG("[texdump] frame %ld: %d draws into small array / cube layers", g_td_lframe, g_td_lcount); g_td_frame = g_td_lframe; td_dump(); return; }
            g_td_lframe = f; g_td_frame = f; g_td_lcount = 0; [g_td_list removeAllObjects]; }
        if (nc) td_layer_count(pd.colorAttachments[0].texture, pd.colorAttachments[0].level); }
    if (g_td_frame == LONG_MAX) { id<MTLTexture> t0 = nc ? pd.colorAttachments[0].texture : nil; NSUInteger lv = pd.colorAttachments[0].level;
        if (t0 && MAX(t0.width >> lv, 1ul) == g_td_w && MAX(t0.height >> lv, 1ul) == g_td_h) { g_td_frame = f; MLOG("[texdump] first %ux%u render target at frame %ld: dumping this frame", g_td_w, g_td_h, f); } }
    if (f > g_td_frame) { td_dump(); return; }
    if (f != g_td_frame) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) {
        id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        uint64_t ser = i < (NSUInteger)R.rt.n && R.rt.color[i] ? R.rt.color[i]->serial : 0;
        NSUInteger lv = pd.colorAttachments[i].level, sl = t.textureType == MTLTextureType2DArray ? pd.colorAttachments[i].slice : 0;
        BOOL seen = NO; for (NSArray *e in g_td_list) if (e.count < 5 && [e[1] unsignedLongLongValue] == ser && [e[2] unsignedIntegerValue] == lv && [e[3] unsignedIntegerValue] == sl) seen = YES;
        if (!seen) [g_td_list addObject:@[t, @(ser), @(lv), @(sl)]];
    }
}
void td_note_layer(id<MTLTexture> t, uint64_t ser, NSUInteger lv, NSUInteger sl) {   // a slice-merged pass's other layers (see g_sm_plan)
    if (g_td_frame < 0 || g_td_done || g_td_frame == LONG_MAX || (long)atomic_load(&g_cur_frame) != g_td_frame || !t) return;
    if (g_td_layers) td_layer_count(t, lv);
    if (t.textureType != MTLTextureType2DArray) sl = 0;
    for (NSArray *e in g_td_list) if (e.count < 5 && [e[1] unsignedLongLongValue] == ser && [e[2] unsignedIntegerValue] == lv && [e[3] unsignedIntegerValue] == sl) return;
    [g_td_list addObject:@[t, @(ser), @(lv), @(sl)]];
}
// The next frame's targets, on request (the Mac's DM_CTL "texdump <path>", hle_hid.c): the path is kept (strdup), the frame number is the one after the current
// The Mac's DM_CTL "texraw <serial> <path>" (hle_hid.c): level 0 of every slice (a cube's six faces) of the host texture behind a serial, raw, after the GPU finishes (header line: width height Metal format bytes per row, then the slice count when above one);
// for a texture that is no render target (a default texture, a lookup table) whose content a shader depends on
void dm_gfx_texraw(uint64_t serial, const char *path) {
    __unsafe_unretained id o = tex_get(serial);
    if ([o isKindOfClass:[NvPkg class]]) o = ((NvPkg *)o)->tex;   // a texture from the game's files
    if (!o || ![o conformsToProtocol:@protocol(MTLTexture)]) { MLOG("[texraw] ser=%llu: no host texture", (unsigned long long)serial); return; }
    id<MTLTexture> t = o; mtl_finish_gpu();
    if (t.storageMode == MTLStorageModeMemoryless || t.pixelFormat == MTLPixelFormatInvalid) return;
    NSUInteger w = t.width, h = t.height, row = w * 16, sz = row * h;   // 16 bytes a texel: a multiple of every uncompressed texel size
    int d3 = t.textureType == MTLTextureType3D;   // a 3D texture: every depth slice, as slices
    NSUInteger ns = d3 ? MAX(t.depth, 1u) : (t.textureType == MTLTextureTypeCube || t.textureType == MTLTextureTypeCubeArray ? 6 : 1) * MAX(t.arrayLength, 1u);   // every slice (a cube's six faces), one after another
    id<MTLBuffer> buf = [g_dev newBufferWithLength:sz * ns options:MTLResourceStorageModeShared];
    id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
    MTLBlitOption bo = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;
    for (NSUInteger s = 0; s < ns; s++)
        [b copyFromTexture:t sourceSlice:d3 ? 0 : s sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, d3 ? s : 0) sourceSize:MTLSizeMake(w, h, 1) toBuffer:buf destinationOffset:sz * s destinationBytesPerRow:row destinationBytesPerImage:sz options:bo];
    [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
    FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %lu", (unsigned long)w, (unsigned long)h, (unsigned long)t.pixelFormat, (unsigned long)row); if (ns > 1) fprintf(f, " %lu", (unsigned long)ns); fputc('\n', f); fwrite(buf.contents, 1, sz * ns, f); fclose(f); }
    const uint8_t *p = buf.contents;
    MLOG("[texraw] ser=%llu %lux%lu format %lu type %lu → %s; first bytes %02x %02x %02x %02x %02x %02x %02x %02x", (unsigned long long)serial, (unsigned long)w, (unsigned long)h, (unsigned long)t.pixelFormat, (unsigned long)t.textureType, path, p[0], p[1], p[2], p[3], p[4], p[5], p[6], p[7]);
}
// The Mac's DM_CTL "texlist <NVN target>" (hle_hid.c): the registered textures of that target (2 = 3D), with their storage (guest address, size) and block-linear layout, for "memdump"
void dm_gfx_texlist(int target) {
    os_unfair_lock_lock(&g_rmu);
    for (int i = 1; i < g_ntreg; i++) { const tex_t *t = &g_treg[i].t; if (!g_treg[i].ok || t->d.target != target) continue;
        MLOG("[texlist] tid=%d ser=%llu target %d %ux%ux%u format %#x levels %u flags %#x storage %p size %#llx block height/depth log2 %u/%u", i, (unsigned long long)t->serial, t->d.target,
             t->d.width, t->d.height, t->d.depth, t->d.format, t->d.levels, t->d.flags, (void *)t->mem, (unsigned long long)t->L.size, t->L.tileH, t->L.tileD); }
    os_unfair_lock_unlock(&g_rmu);
}
// For the crash dump (crash.c): what is known of an address. Read without the locks (the process is going), so a table being changed at that moment may print nonsense, never fault
// on a freed table: g_pools and g_treg are only reallocated, and both are read through their current pointer and count
void dm_gfx_addr_note(uint64_t a, char *note, size_t n) {
    size_t k = 0; note[0] = 0;
    #define NOTE(...) do { if (k < n) { int w = snprintf(note + k, n - k, __VA_ARGS__); if (w > 0) k += (size_t)w; } } while (0)
    const pool_ent *pl = g_pools; int np = g_npool;
    for (int i = 0; pl && i < np; i++) if (a >= pl[i].lo && a < pl[i].hi) { NOTE(" memory pool %#lx..%#lx (+%#llx)", (unsigned long)pl[i].lo, (unsigned long)pl[i].hi, (unsigned long long)(a - pl[i].lo)); break; }
    const treg_t *tr = g_treg; int nt = g_ntreg, shown = 0;
    for (int i = 1; tr && i < nt && shown < 2; i++) { const tex_t *t = &tr[i].t; uint64_t m = (uint64_t)(uintptr_t)t->mem;
        if (tr[i].ok && m && a >= m && a < m + t->L.size) { NOTE(" texture ser=%llu %ux%u format %#x storage +%#llx", (unsigned long long)t->serial, t->d.width, t->d.height, t->d.format, (unsigned long long)(a - m)); shown++; } }
    uint64_t pg = a >> 12, done = atomic_load(&g_done); const gw_set *s = &g_gwx[gw_slot(pg)];
    uint64_t wf = 0; int shared = 0;
    for (int w = 0; w < GW_WAYS; w++) if (s->page[w] == pg && s->fence[w] > wf) wf = s->fence[w];
    if (g_um) { const um_ent *m = &g_um[um_slot(pg)]; if (m->page == pg && m->fence > wf) wf = m->fence; }
    if (s->over_fence > wf) { wf = s->over_fence; shared = 1; }
    if (wf) NOTE(" GPU write recorded on the page%s: fence %llu (completed %llu, last %llu)", shared ? " (shared set)" : "", (unsigned long long)wf, (unsigned long long)done, (unsigned long long)mtl_last_fence());
    for (int i = 0; i < GWW_N; i++) if (g_gww[i].fence && a >= g_gww[i].lo && a < g_gww[i].hi) NOTE(" in a large GPU write %#llx..%#llx: fence %llu", (unsigned long long)g_gww[i].lo, (unsigned long long)g_gww[i].hi, (unsigned long long)g_gww[i].fence);
    id<MTLBuffer> gm = g_gmem_buf;   // the raw global memory table (gmem_map): what a raw load or store of the address reaches
    if (gm) { const uint64_t *t = (const uint64_t *)gm.contents; int hit = 0;
        for (int k = 1; k < DMSH_GMEM_MAP_ENTRIES && t[(size_t)k * (DMSH_GMEM_MAP_ENTRY / 8)] != ~0ull; k++) { const uint64_t *e = t + (size_t)k * (DMSH_GMEM_MAP_ENTRY / 8); if (a >= e[0] && a < e[1]) { hit = k; break; } }
        if (hit) NOTE(" raw memory table entry %d", hit);
        else NOTE(" in no raw memory table entry (a raw load reads 0, a store is dropped)%s", addr_meets((uintptr_t)a, (uintptr_t)a + 1) ? "" : "; no pool address given in it"); }
    { char vn[200]; void nvn_vpool_note(uint64_t, char *, size_t); nvn_vpool_note(a, vn, sizeof vn); NOTE("%s", vn); }
    #undef NOTE
}
void dm_gfx_texdump_next(const char *path) {
    if (g_td_frame == -2) td_note(nil, 0);   // read DM_NVN_TEXDUMP first, so it does not override the request
    g_td_path = strdup(path); g_td_list = [NSMutableArray array]; g_td_done = 0; g_td_frame = (long)atomic_load(&g_cur_frame) + 1;
}
static void td_dump(void) {
    g_td_done = 1;
    rp_end(); if (g_blit) { enc_b_end(g_blit); g_blit = nil; }
    mtl_finish_gpu();
    static id<MTLRenderPipelineState> ps; static id<MTLSamplerState> sp;
    if (!ps) {
        static const char *src_msl =
            "#include <metal_stdlib>\nusing namespace metal;\n"
            "struct V { float4 p [[position]]; float2 uv; };\n"
            "vertex V vs(uint id [[vertex_id]]) { float2 uv = float2((id << 1) & 2, id & 2); V o; o.p = float4(uv * float2(2, -2) + float2(-1, 1), 0, 1); o.uv = uv; return o; }\n"
            "fragment float4 fs(V in [[stage_in]], texture2d<float> t [[texture(0)]], sampler s [[sampler(0)]]) { float4 c = t.sample(s, in.uv); return float4(c.rgb, 1); }\n";
        NSError *e = nil; id<MTLLibrary> lib = dev_lib(@(src_msl), &e);
        MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new];
        d.vertexFunction = [lib newFunctionWithName:@"vs"]; d.fragmentFunction = [lib newFunctionWithName:@"fs"];
        d.colorAttachments[0].pixelFormat = MTLPixelFormatRGBA8Unorm;
        ps = dev_rps(d, &e);
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sp = [g_dev newSamplerStateWithDescriptor:sd];
        if (!ps) { MLOG("[RED] TEXDUMP pipeline failed %s", e.localizedDescription.UTF8String); return; }
    }
    MLOG("[texdump] frame %ld: %lu color targets", g_td_frame, (unsigned long)g_td_list.count);
    // DM_NVN_TEXRAW=index,...: save raw bytes by texture index (including 3D; file: %d in the path is replaced with the index, suffix .raw, first-line text header: width height depth format bytes-per-row)
    for (const char *q = getenv("DM_NVN_TEXRAW"); q && *q; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) {
        if (!strncmp(q, "info", 4)) {   // DM_NVN_TEXRAW=info: the textures remembered by DM_NVN_TEXINFO
            static char buf[256]; size_t o = 0; for (int i = 0; i < g_texinfo_n; i++) o += snprintf(buf + o, sizeof buf - o, "%s%llu", i ? "," : "", (unsigned long long)g_texinfo_ser[i]);
            if (o) { static char env[300]; snprintf(env, sizeof env, "%s", buf); setenv("DM_NVN_TEXRAW", env, 1); q = getenv("DM_NVN_TEXRAW"); MLOG("[texraw] info → %s", q); } else continue;
        }
        uint64_t ser = (uint64_t)atoll(q); id o = tex_get(ser);
        id<MTLTexture> t = [o isKindOfClass:[NvPkg class]] ? ((NvPkg *)o)->tex : o;
        if (!t) { MLOG("[texraw] ser=%llu not in the serial table", ser); continue; }
        int bw, bh; NSUInteger w = t.width, h = t.height, dz = t.depth;
        size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : 4; (void)bw; (void)bh;
        size_t row = w * bpp, img = row * h;
        id<MTLBuffer> buf = [g_dev newBufferWithLength:img * dz options:MTLResourceStorageModeShared];
        id<MTLCommandBuffer> cb = q_cb(); id<MTLBlitCommandEncoder> b = [cb blitCommandEncoder];
        MTLBlitOption bo = t.pixelFormat == MTLPixelFormatDepth32Float_Stencil8 ? MTLBlitOptionDepthFromDepthStencil : MTLBlitOptionNone;   // depth-stencil: the 32-bit float depth plane only
        [b copyFromTexture:t sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(w, h, dz) toBuffer:buf destinationOffset:0 destinationBytesPerRow:row destinationBytesPerImage:img options:bo];
        [b endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)ser); strlcat(path, ".raw", sizeof path);
        FILE *f = fopen(path, "wb"); if (f) { fprintf(f, "%lu %lu %lu %lu %zu\n", (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, row); fwrite(buf.contents, 1, img * dz, f); fclose(f); }
        MLOG("[texraw] ser=%llu %lux%lux%lu format %lu → %s", ser, (unsigned long)w, (unsigned long)h, (unsigned long)dz, (unsigned long)t.pixelFormat, path);
    }
    for (NSArray *e in g_td_list) {
        id<MTLTexture> t = e[0]; uint64_t ser = [e[1] unsignedLongLongValue]; NSUInteger lv = [e[2] unsignedIntegerValue], sl = [e[3] unsignedIntegerValue];
        // 2D arrays are saved by the attached layer (file index ser*100 + layer*10 + level; layer 0 is named like 2D)
        if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray) { MLOG("[texdump] ser=%llu type %lu skipped", ser, (unsigned long)t.textureType); continue; }
        if (getenv("DM_NVN_SNAPRAW")) {   // Also save the raw bytes (converting an HDR target to an 8-bit image truncates; first-line text header: width height format bytes-per-row)
            NSUInteger rw = MAX(t.width >> lv, 1u), rh = MAX(t.height >> lv, 1u);
            size_t bpp = t.pixelFormat == MTLPixelFormatRGBA16Float ? 8 : t.pixelFormat == MTLPixelFormatRGBA32Float ? 16 : t.pixelFormat == MTLPixelFormatR16Float || t.pixelFormat == MTLPixelFormatR16Unorm ? 2 : 4;
            id<MTLBuffer> rb = [g_dev newBufferWithLength:rw * rh * bpp options:MTLResourceStorageModeShared];
            id<MTLCommandBuffer> rc = q_cb(); id<MTLBlitCommandEncoder> bb = [rc blitCommandEncoder];
            [bb copyFromTexture:t sourceSlice:0 sourceLevel:lv sourceOrigin:MTLOriginMake(0, 0, 0) sourceSize:MTLSizeMake(rw, rh, 1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:rw * bpp destinationBytesPerImage:rw * rh * bpp];
            [bb endEncoding]; [rc commit]; [rc waitUntilCompleted];
            char rp_[1024]; snprintf(rp_, sizeof rp_, g_td_path, (int)(ser * 100 + lv));
            if (e.count > 4) { char tail[64]; snprintf(tail, sizeof tail, "_s%03d", [e[4] intValue]); strlcat(rp_, tail, sizeof rp_); }   // TEXSNAP copies of one target: one file per pass
            strlcat(rp_, ".raw", sizeof rp_);
            FILE *f = fopen(rp_, "wb"); if (f) { fprintf(f, "%lu %lu %lu %zu\n", (unsigned long)rw, (unsigned long)rh, (unsigned long)t.pixelFormat, rw * bpp); fwrite(rb.contents, 1, rw * rh * bpp, f); fclose(f); }
        }
        id<MTLTexture> v = [t newTextureViewWithPixelFormat:t.pixelFormat textureType:MTLTextureType2D levels:NSMakeRange(lv, 1) slices:NSMakeRange(sl, 1)];
        NSUInteger w = MAX(t.width >> lv, 1u), h = MAX(t.height >> lv, 1u);
        MTLTextureDescriptor *od = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
        od.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead; od.storageMode = MTLStorageModePrivate;
        id<MTLTexture> o = [g_dev newTextureWithDescriptor:od];
        id<MTLCommandBuffer> cb = q_cb();
        MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
        rp.colorAttachments[0].texture = o; rp.colorAttachments[0].loadAction = MTLLoadActionDontCare; rp.colorAttachments[0].storeAction = MTLStoreActionStore;
        id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
        [en setRenderPipelineState:ps]; [en setFragmentTexture:v atIndex:0]; [en setFragmentSamplerState:sp atIndex:0];
        [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3]; [en endEncoding]; [cb commit]; [cb waitUntilCompleted];
        char path[1024]; snprintf(path, sizeof path, g_td_path, (int)(ser * 100 + sl * 10 + lv));
        if (e.count > 4) { snprintf(path, sizeof path, g_td_path, 0); char *dot = strrchr(path, '.'); char ext[16] = ""; if (dot) { strlcpy(ext, dot, sizeof ext); *dot = 0; }
            char tail[64]; snprintf(tail, sizeof tail, "_s%03d_%llu_%lu%s", [e[4] intValue], (unsigned long long)ser, (unsigned long)lv, ext); strlcat(path, tail, sizeof path); }   // TEXSNAP copy
        MLOG("[texdump] ser=%llu lv=%lu layer %lu format %lu %lux%lu", ser, (unsigned long)lv, (unsigned long)sl, (unsigned long)t.pixelFormat, (unsigned long)w, (unsigned long)h);
        shot(o, path, 0, NULL);
    }
    [g_td_list removeAllObjects];
}
// DM_NVN_TEXSNAP=in-frame index,... (together with the frame number and path of DM_NVN_TEXDUMP): as soon as these render passes end, copy their color attachments and save them together with the end-of-frame dump
// (the same target is often cleared and reused within a frame, so the end-of-frame state cannot reflect the output of intermediate passes); saved as <TEXDUMP path with %d = 0>_s<in-frame index>_<serial>_<level>.png
static int g_ts_idx[64], g_ts_n = -1, g_ts_on; static uint64_t g_ts_first, g_ts_curf = ~0ull;
static __strong id<MTLTexture> g_ts_att[8]; static uint64_t g_ts_ser[8]; static NSUInteger g_ts_lv[8]; static int g_ts_na;
void ts_note(MTL4RenderPassDescriptor *pd, NSUInteger nc) {
    if (g_ts_n < 0) { g_ts_n = 0; const char *e = getenv("DM_NVN_TEXSNAP");
        for (const char *q = e; q && *q && g_ts_n < 64; q = strchr(q, ',') ? strchr(q, ',') + 1 : q + strlen(q)) g_ts_idx[g_ts_n++] = atoi(q); }
    g_ts_on = 0; if (!g_ts_n || g_td_frame < 0 || g_td_done) return;
    uint64_t f = atomic_load(&g_cur_frame); if (f != g_ts_curf) { g_ts_curf = f; g_ts_first = g_stat_pass; }
    if ((long)f != g_td_frame) return;
    int idx = (int)(g_stat_pass - g_ts_first); for (int i = 0; i < g_ts_n; i++) if (g_ts_idx[i] == idx) g_ts_on = idx + 1;
    g_ts_na = 0; if (!g_ts_on) return;
    for (NSUInteger i = 0; i < nc && i < 8; i++) { id<MTLTexture> t = pd.colorAttachments[i].texture; if (!t) continue;
        g_ts_att[g_ts_na] = t; g_ts_ser[g_ts_na] = R.rt.color[i] ? R.rt.color[i]->serial : 0; g_ts_lv[g_ts_na] = pd.colorAttachments[i].level; g_ts_na++; }
}
void ts_after(void) {
    if (!g_ts_on) return;
    // The copy goes into the replay stream (Metal 4 copy encoder, after the pass that just ended). A command buffer of the classic queue, committed here, ran before the
    // Metal 4 command buffer holding the pass was even committed, so every snapshot showed the attachments as they were before the pass (found on Tears of the Kingdom)
    for (int i = 0; i < g_ts_na; i++) {
        id<MTLTexture> t = g_ts_att[i]; if (t.textureType != MTLTextureType2D && t.textureType != MTLTextureType2DArray) continue;   // 2D arrays take slice 0
        MTLTextureDescriptor *d = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:t.pixelFormat width:t.width height:t.height mipmapped:t.mipmapLevelCount > 1];
        d.usage = MTLTextureUsageShaderRead; d.storageMode = MTLStorageModePrivate; d.mipmapLevelCount = t.mipmapLevelCount;
        id<MTLTexture> c = [g_dev newTextureWithDescriptor:d]; res_add(c);
        NSUInteger lv = g_ts_lv[i], w = MAX((NSUInteger)1, t.width >> lv), h = MAX((NSUInteger)1, t.height >> lv);
        enc_b_copy_texture(blit(), t, 0, lv, MTLOriginMake(0, 0, 0), MTLSizeMake(w, h, 1), c, 0, lv, MTLOriginMake(0, 0, 0));
        [g_td_list addObject:@[c, @(g_ts_ser[i]), @(g_ts_lv[i]), @0, @(g_ts_on - 1)]];   // the end-of-frame fields (texture, serial, level, layer) plus the in-frame pass index: saved as <path with %d = 0>_s<index>_<serial>_<level>.png
    }
    g_ts_on = 0;
}
// Ring of actual presentation times: present sequence number -> presentedTime (seconds; -1 = dropped by the system, not presented)
#define PRING NVN_PRESENT_HISTORY_CAP
static nvn_present_history g_present_history = NVN_PRESENT_HISTORY_INIT;
// Get the actual presentation time of the nth present (returns 0 if not ready or overwritten, -1 if dropped)
double mtl_presented_time(uint64_t n) { return nvn_present_history_get(&g_present_history, n); }
// The screen's progress, for the acquire's wait without the display link (mtl_present_screen_wait, DM_PRESENT_DEPTH) and the queue-to-screen statistics: by ticket, when the game
// queued the present (CACurrentMediaTime, the presentedTime base), and the newest ticket the system has reported shown or dropped (the drawable's presented handler, on a system thread)
static double g_pres_tin[PRING];
static _Atomic uint64_t g_pres_seen, g_sdw_n, g_sdw_ns, g_sdw_to;   // the newest ticket seen; the acquires that waited for the screen, their time, and those that timed out
static int g_sdw_depth = -1;
static uint64_t g_pres_frm[PRING];   // by ticket: the game frame its present was recorded in (pq_item.frame)
static _Atomic uint64_t g_rec_frame, g_seen_frame;   // the frame of the game's newest recorded present (mtl_present_recorded, the game's thread) and of the newest present shown or dropped
void mtl_present_recorded(uint64_t frame) { uint64_t w = atomic_load(&g_rec_frame); while (frame > w && !atomic_compare_exchange_weak(&g_rec_frame, &w, frame)) {} }
static void pres_seen(uint64_t pn, double tt) {
    uint64_t was = atomic_load(&g_pres_seen);
    while (pn > was && !atomic_compare_exchange_weak(&g_pres_seen, &was, pn)) {}
    { uint64_t fr = g_pres_frm[pn % PRING], w = atomic_load(&g_seen_frame); while (fr > w && !atomic_compare_exchange_weak(&g_seen_frame, &w, fr)) {} }
    pthread_mutex_lock(&g_pq_mu); pthread_cond_broadcast(&g_pq_cv); pthread_mutex_unlock(&g_pq_mu);
    double lagms = tt > 0 ? (CACurrentMediaTime() - tt) * 1e3 : -1;   // how late the presented handler runs after the frame reached the screen
    double tin = g_pres_tin[pn % PRING];
    if (!(tt > 0) || !(tin > 0) || tt < tin) return;
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER; static double lat[300], lag[300]; static int n; static uint64_t n0, ns0, to0;
    pthread_mutex_lock(&mu);
    lag[n] = lagms; lat[n++] = (tt - tin) * 1e3;
    if (n == 300) {
        for (int i = 1; i < n; i++) { double x = lat[i]; int j = i - 1; while (j >= 0 && lat[j] > x) { lat[j + 1] = lat[j]; j--; } lat[j + 1] = x; }
        for (int i = 1; i < n; i++) { double x = lag[i]; int j = i - 1; while (j >= 0 && lag[j] > x) { lag[j + 1] = lag[j]; j--; } lag[j + 1] = x; }
        uint64_t wn = atomic_load(&g_sdw_n), wns = atomic_load(&g_sdw_ns), wto = atomic_load(&g_sdw_to);
        MLOG("present: game enqueue to screen p50 %.1f ms, p95 %.1f, max %.1f over 300 frames shown (ticket %llu); waits for the screen at acquire (DM_PRESENT_DEPTH %d): %llu, %.2f ms a frame, %llu timed out; presented handlers run %.1f ms (p50) / %.1f (p95) after the frame reached the screen",
             lat[149], lat[284], lat[299], (unsigned long long)pn, g_sdw_depth, (unsigned long long)(wn - n0), (wns - ns0) / 1e6 / 300, (unsigned long long)(wto - to0), lag[149], lag[284]);
        n0 = wn; ns0 = wns; to0 = wto; n = 0;
    }
    pthread_mutex_unlock(&mu);
}
// Ring of the display-link tick that took each present (written on the display-link callback thread, read 8 presents later for DM_NVN_TSV, as the presentation history): its target display time
// (seconds, CACurrentMediaTime base), how long before it the callback ran, the callbacks since the previous present's (1 = no tick without a frame) and the longest interval between them
int mtl_link_tick(uint64_t n, double *target, double *lead_ms, int *ticks, double *gap_ms) {
    nvn_link_time e; if (!nvn_link_history_get(&g_present_history, n, &e)) return 0;
    *target = e.target; *lead_ms = e.lead_ms; *ticks = e.ticks; *gap_ms = e.gap_ms; return 1;
}
static _Atomic uint64_t g_drawable_ns;   // Accumulated wall clock waiting for nextDrawable in present (timing instrumentation)
uint64_t mtl_drawable_ns_take(void) { return atomic_exchange(&g_drawable_ns, 0); }
// Maximum refresh rate (Hz) of the screen when there is a window; returns 0 without a window. Mac uses CoreGraphics display modes (thread-safe C interface; the present configuration is read on the game thread)
int mtl_display_hz(void) {
    if (!front_metal_layer()) return 0;
    CGDisplayModeRef m = CGDisplayCopyDisplayMode(CGMainDisplayID());
    double hz = m ? CGDisplayModeGetRefreshRate(m) : 0; if (m) CGDisplayModeRelease(m);
    return (int)(hz + 0.5);
}
static _Atomic uint64_t g_present_stall_ns;   // For unit tests: an extra pause before each present on the present thread (simulating a drawable that stays unavailable)
void mtl_present_test_stall(uint64_t ns) { g_present_stall_ns = ns; }
static void present_pipeline(MTLPixelFormat format) {
    if (!g_blit_pso || g_blit_fmt != format) {
        NSError *e = nil;
        id<MTLLibrary> lib = host_lib("present", &e);
        if (!lib) MDIE("present shader compile failed %s", e.localizedDescription.UTF8String);
        MTLRenderPipelineDescriptor *pd = [MTLRenderPipelineDescriptor new];
        pd.vertexFunction = [lib newFunctionWithName:@"vs"]; pd.fragmentFunction = [lib newFunctionWithName:@"fs"];
        pd.colorAttachments[0].pixelFormat = format;
        g_blit_pso = dev_rps(pd, &e);
        if (!g_blit_pso) MDIE("present pipeline failed %s", e.localizedDescription.UTF8String);
        g_blit_fmt = format;
        MTLSamplerDescriptor *sd = [MTLSamplerDescriptor new]; sd.minFilter = sd.magFilter = MTLSamplerMinMagFilterLinear; sd.supportArgumentBuffers = YES;
        g_blit_samp = [g_dev newSamplerStateWithDescriptor:sd];
    }
}
// Present samples only the valid source rectangle declared by NVN; kept separate from the raw swapchain readback screenshot. Edges are clamped to the texel centers of the valid area, so bilinear does not read the black texels outside.
// Present samples the raw bytes stored in the swapchain texture (the Switch's scan-out sends the stored bytes as they are): an sRGB source is replaced by a non-sRGB view of the same storage,
// otherwise sampling decodes to linear first, the BGRA8Unorm layer does not encode again, and the whole picture comes out dark. Other formats are returned as they are (Mario's swapchain is RGBA8 and does not come here).
// View cache of 4 entries: swapchain textures live long and are few; a view holds its parent, so an equal parent pointer means the same object. A replaced view is released after the submissions that used it complete (Metal 4 does not retain resources)
static id<MTLTexture> present_raw_src(id<MTLTexture> src) {
    MTLPixelFormat pf = src.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB ? MTLPixelFormatRGBA8Unorm : src.pixelFormat == MTLPixelFormatBGRA8Unorm_sRGB ? MTLPixelFormatBGRA8Unorm : MTLPixelFormatInvalid;
    if (pf == MTLPixelFormatInvalid) return src;
    static __strong id<MTLTexture> views[4]; static int next; static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    pthread_mutex_lock(&mu);
    id<MTLTexture> v = nil;
    for (int i = 0; i < 4 && !v; i++) if (views[i] && views[i].parentTexture == src) v = views[i];
    if (!v && (v = [src newTextureViewWithPixelFormat:pf])) { if (views[next]) defer_obj_unref(views[next]); views[next] = v; next = (next + 1) & 3; }
    pthread_mutex_unlock(&mu);
    if (!v) { RED_ONCE(69, "present: cannot create a non-sRGB view of the sRGB swapchain texture (format %lu); presenting the decoded values", (unsigned long)src.pixelFormat); return src; }
    return v;
}
// The rectangle of the canvas (dw x dh) the picture (pw x ph) is drawn into: the picture's aspect ratio kept and centred, black bars in the rest (no stretching when the screen's ratio
// differs from the game's, e.g. a 16:10 screen against the game's 16:9); when the ratios differ by under 1 % (the remainder from a game width that is a multiple of 8 in the
// full-screen setup, e.g. 1952x900 against 2868x1320 differs by 0.17 %) the whole canvas, leaving no border of a few pixels
static MTLViewport present_viewport(double pw, double ph, double dw, double dh) {
    double s = fmin(dw / pw, dh / ph);
    MTLViewport vp = {floor((dw - pw * s) / 2), floor((dh - ph * s) / 2), round(pw * s), round(ph * s), 0, 1};
    if (fabs((dw / dh) / (pw / ph) - 1) < 0.01) vp = (MTLViewport){0, 0, dw, dh, 0, 1};
    return vp;
}
// The shape the picture is shown in, whatever part of the texture it comes from. The Switch's compositor stretches a buffer's crop to the layer, so a crop only picks texels: Super
// Mario Galaxy 2 draws its storybook cutscenes anamorphically into 1200x960 of a 1920x1080 swapchain (800x640 handheld). The layer fills the display, 16:9; the host has no
// other shape (no producer of a sized layer is left), and only the graphics tests whose canvases are square write this
static double g_present_shape[2] = {1920, 1080};
static MTLViewport present_fit(double dw, double dh) { return present_viewport(g_present_shape[0], g_present_shape[1], dw, dh); }
// The valid source rectangle of a present (x, y, width, height; the crop of nvnWindowSetCrop, zero = the whole texture); 0 when it lies outside the texture
static int present_crop_rect(id<MTLTexture> src, const int crop[4], NSUInteger r[4]) {
    r[0] = 0; r[1] = 0; r[2] = src.width; r[3] = src.height;
    if (!crop || !(crop[2] || crop[3])) return 1;
    if (crop[0] < 0 || crop[1] < 0 || crop[2] <= 0 || crop[3] <= 0 ||
        (uint64_t)crop[0] + crop[2] > src.width || (uint64_t)crop[1] + crop[3] > src.height) {
        MLOG("[RED] present crop out of bounds: %d,%d %dx%d / source texture %lux%lu", crop[0], crop[1], crop[2], crop[3], (unsigned long)src.width, (unsigned long)src.height); return 0;
    }
    r[0] = crop[0]; r[1] = crop[1]; r[2] = crop[2]; r[3] = crop[3]; return 1;
}
static int present_encode(id<MTLCommandBuffer> cb, id<MTLTexture> src, id<MTLTexture> dst, const int crop[4]) {
    NSUInteger r[4]; if (!present_crop_rect(src, crop, r)) return 0;
    NSUInteger cx = r[0], cy = r[1], cw = r[2], ch = r[3];
    static uint64_t last_crop[8]; uint64_t meta[8] = {src.width, src.height, cx, cy, cw, ch, dst.width, dst.height};
    if (memcmp(meta, last_crop, sizeof meta)) { memcpy(last_crop, meta, sizeof meta);
        MLOG("present crop: source texture %lux%lu, valid %lu,%lu %lux%lu → target %lux%lu", (unsigned long)src.width, (unsigned long)src.height,
             (unsigned long)cx, (unsigned long)cy, (unsigned long)cw, (unsigned long)ch, (unsigned long)dst.width, (unsigned long)dst.height); }
    present_pipeline(dst.pixelFormat);
    MTLRenderPassDescriptor *rp = [MTLRenderPassDescriptor renderPassDescriptor];
    double dw = dst.width, dh = dst.height;
    MTLViewport vp = present_fit(dw, dh);
    int full = vp.originX == 0 && vp.originY == 0 && vp.width == dw && vp.height == dh;
    static double last_vp[4];   // Log one line when the viewport changes (to check the aspect ratio)
    if (memcmp(last_vp, &vp, sizeof last_vp)) { memcpy(last_vp, &vp, sizeof last_vp);
        MLOG("present viewport %.0f,%.0f %.0fx%.0f / canvas %.0fx%.0f (game image %lux%lu)", vp.originX, vp.originY, vp.width, vp.height, dw, dh, (unsigned long)src.width, (unsigned long)src.height); }
    rp.colorAttachments[0].texture = dst; rp.colorAttachments[0].loadAction = full ? MTLLoadActionDontCare : MTLLoadActionClear;
    rp.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 1);
    rp.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> en = [cb renderCommandEncoderWithDescriptor:rp];
    [en setViewport:vp];
    [en setRenderPipelineState:g_blit_pso]; [en setFragmentTexture:present_raw_src(src) atIndex:0]; [en setFragmentSamplerState:g_blit_samp atIndex:0];
    float uv[8] = {(float)cx / src.width, (float)cy / src.height, (float)cw / src.width, (float)ch / src.height,
                   (cx + 0.5f) / src.width, (cy + 0.5f) / src.height, (cx + cw - 0.5f) / src.width, (cy + ch - 0.5f) / src.height};
    if (atomic_load_explicit(&g_origin_ll, memory_order_relaxed)) { uv[1] += uv[3]; uv[3] = -uv[3]; }   // lower-left origin: the window image is stored bottom-up
    [en setFragmentBytes:uv length:sizeof uv atIndex:0];
    [en drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:3];
    [en endEncoding];
    return 1;
}
// ---------- Upscaling in the present (DM_PRESENT_UPSCALE) ----------
// Opt-in spatial scaling of the valid content to its letterboxed viewport, in perceptual mode (present_raw_src supplies gamma bytes). The scaler reads the presented texture itself
// when the picture is all of it (DM_FX_INPLACE), else a copy of the crop at the origin of a texture of its own (MetalFX reads from the origin); it writes the canvas itself when
// the picture fills it unflipped and the canvas takes the scaler's output (DM_FX_DIRECT), else a texture of its own that the present pass draws 1:1 with the usual origin and output
// format. Each texture of its own is made when a present first needs it. Queue barriers serialize both the frame's rendering and reuse of the scaler's textures across presents.
// Replacing the scaler waits for earlier presents; throttle replacement during resizing, and keep standard scaling for refused sizes. Offscreen captures own separate state.
#define FX_REMAKE_PRESENTS 30
// in, out: the scaler's own input and output (nil until needed); in_place_n, direct_n: presents that read the presented texture, wrote the canvas
typedef struct { id<MTL4FXSpatialScaler> sc; id<MTLTexture> in, out; uint64_t made_at, in_place_n, direct_n; int made; NSUInteger refused[6]; } fx_state;
static fx_state g_fx_present;   // only the present thread encodes or replaces the scaler
static id<MTL4Compiler> g_fx_comp;
static int g_fx_mode = -1;   // 1 = metalfx, 0 = off
static int fx_mode(void) {
    if (g_fx_mode >= 0) return g_fx_mode;
    const char *e = getenv("DM_PRESENT_UPSCALE"); int v = 0;
    if (e && (!strcmp(e, "metalfx") || !strcmp(e, "1"))) v = 1;
    else if (e && (!strcmp(e, "off") || !strcmp(e, "0"))) v = 0;
    else if (e && *e) MLOG("[RED] DM_PRESENT_UPSCALE=%s: metalfx or off; keeping %s", e, v ? "metalfx" : "off");
    if (v && !(g_q4 && [MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev])) { MLOG("present upscaling: this GPU has no Metal 4 MetalFX spatial scaler; the present scales bilinearly"); v = 0; }
    MLOG("present upscaling (DM_PRESENT_UPSCALE): %s", v ? "metalfx, MetalFX's spatial scaler where the picture is shown larger than the game renders it" : "off, bilinear");
    return g_fx_mode = v;
}
// MetalFX scales the content (cw x ch) to its rectangle on the canvas (vp) when the rectangle is at least as large in both directions and larger in one
static int fx_wanted(MTLViewport vp, NSUInteger cw, NSUInteger ch) {
    NSUInteger w = (NSUInteger)vp.width, h = (NSUInteger)vp.height;
    return cw && ch && w >= cw && h >= ch && (w > cw || h > ch);
}
// A texture of the scaler's own (private, resident), made on first use; nil when it cannot be made
static id<MTLTexture> fx_tex(__strong id<MTLTexture> *slot, MTLPixelFormat pf, NSUInteger w, NSUInteger h, MTLTextureUsage usage, NSString *label) {
    if (*slot) return *slot;
    MTLTextureDescriptor *td = [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:pf width:w height:h mipmapped:NO];
    td.storageMode = MTLStorageModePrivate; td.usage = usage;
    id<MTLTexture> t = [g_dev newTextureWithDescriptor:td];
    if (!t) { MLOG("[RED] present upscaling: cannot create the %s (%lux%lu format %lu)", label.UTF8String, (unsigned long)w, (unsigned long)h, (unsigned long)pf); return nil; }
    t.label = label; res_add(t); res_commit();
    return *slot = t;
}
// Encodes the upscale of the rectangle r of raw into cb (see above). Returns the scaled picture, which the present pass draws 1:1 into vp; or the canvas, when the scaler wrote it
// (*wrote = 1: no pass); or nil (the bilinear pass). canvas: the present's target when the picture fills it unflipped (else nil), written by the scaler where it takes it.
// remake_wait: fx is the present thread's, whose textures presents still on the GPU may read: a new scaler waits for them and for FX_REMAKE_PRESENTS presents since the last.
static id<MTLTexture> fx_encode(fx_state *fx, id<MTL4CommandBuffer> cb, id<MTLTexture> raw, const NSUInteger r[4], MTLViewport vp, MTLPixelFormat out_pf, int remake_wait, id<MTLTexture> canvas, int *wrote) {
    NSUInteger cw = r[2], ch = r[3], ow = (NSUInteger)vp.width, oh = (NSUInteger)vp.height;
    if (fx_mode() != 1 || !fx_wanted(vp, cw, ch)) return nil;
    NSUInteger key[6] = {cw, ch, raw.pixelFormat, ow, oh, out_pf};
    id<MTL4FXSpatialScaler> sc = fx->sc;
    if (!sc || sc.inputWidth != cw || sc.inputHeight != ch || sc.colorTextureFormat != raw.pixelFormat || sc.outputWidth != ow || sc.outputHeight != oh || sc.outputTextureFormat != out_pf) {
        if (!memcmp(key, fx->refused, sizeof key)) return nil;
        if (remake_wait && fx->made && g_present_n < fx->made_at + FX_REMAKE_PRESENTS) return nil;
        if (remake_wait) { pthread_mutex_lock(&g_pq_mu); while (g_pq_inflight) pthread_cond_wait(&g_pq_cv, &g_pq_mu); pthread_mutex_unlock(&g_pq_mu); }
        res_remove(fx->in); res_remove(fx->out); fx->sc = nil; fx->in = fx->out = nil;
        uint64_t t0 = now_ns(); NSError *e = nil;
        if (!g_fx_comp) g_fx_comp = [g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&e];
        MTLFXSpatialScalerDescriptor *d = [MTLFXSpatialScalerDescriptor new];
        d.colorTextureFormat = raw.pixelFormat; d.outputTextureFormat = out_pf; d.inputWidth = cw; d.inputHeight = ch; d.outputWidth = ow; d.outputHeight = oh;
        d.colorProcessingMode = MTLFXSpatialScalerColorProcessingModePerceptual;
        sc = g_fx_comp ? [d newSpatialScalerWithDevice:g_dev compiler:g_fx_comp] : nil;
        fx->made_at = g_present_n; fx->made++;
        if (!sc) { memcpy(fx->refused, key, sizeof key);
            MLOG("present upscaling: MetalFX makes no spatial scaler for %lux%lu format %lu → %lux%lu format %lu%s; this size is scaled bilinearly", (unsigned long)cw, (unsigned long)ch, (unsigned long)raw.pixelFormat,
                 (unsigned long)ow, (unsigned long)oh, (unsigned long)out_pf, g_fx_comp ? "" : " (no Metal 4 compiler)"); return nil; }
        fx->sc = sc;
        MLOG("present upscaling: MetalFX spatial scaler %lux%lu → %lux%lu (format %lu → %lu, scaler #%d, made in %.1f ms)", (unsigned long)cw, (unsigned long)ch, (unsigned long)ow, (unsigned long)oh,
             (unsigned long)raw.pixelFormat, (unsigned long)out_pf, fx->made, (now_ns() - t0) / 1e6);
    }
    // The picture is the whole presented texture, which the scaler can read: it reads that texture (the copy only moved the rectangle to the origin)
    static int in_place_ok = -1; if (in_place_ok < 0) { const char *e = getenv("DM_FX_INPLACE"); in_place_ok = !(e && !strcmp(e, "0")); }
    int in_place = in_place_ok && !r[0] && !r[1] && raw.width == cw && raw.height == ch && (raw.usage & sc.colorTextureUsage) == sc.colorTextureUsage && raw.storageMode == MTLStorageModePrivate;
    // The canvas has the picture's size and the scaler can write it: the scaler writes it (the present pass would only copy its output 1:1)
    int direct = canvas && canvas.width == ow && canvas.height == oh && canvas.pixelFormat == out_pf && (canvas.usage & sc.outputTextureUsage) == sc.outputTextureUsage;
    id<MTLTexture> in = in_place ? raw : fx_tex(&fx->in, raw.pixelFormat, cw, ch, sc.colorTextureUsage | MTLTextureUsageShaderRead, @"present upscale input");
    id<MTLTexture> out = direct ? canvas : fx_tex(&fx->out, out_pf, ow, oh, sc.outputTextureUsage | MTLTextureUsageShaderRead, @"present upscale output");
    if (!in || !out) { memcpy(fx->refused, key, sizeof key); return nil; }
    // The scaler after the queue's earlier work (the frame's rendering, the last present's scaler): behind the copy of the crop, or reading in place behind one word (mfx_order)
    mfx_order(cb, raw, MTLOriginMake(r[0], r[1], 0), MTLSizeMake(cw, ch, 1), in_place ? nil : in);
    sc.colorTexture = in; sc.inputContentWidth = cw; sc.inputContentHeight = ch; sc.outputTexture = out;
    [sc encodeToCommandBuffer:cb];
    if (in_place && !fx->in_place_n++ && remake_wait) MLOG("present upscaling: MetalFX reads the presented texture itself (%lux%lu), no copy (DM_FX_INPLACE=0 copies it)", (unsigned long)cw, (unsigned long)ch);
    if (direct && !fx->direct_n++ && remake_wait) MLOG("present upscaling: MetalFX writes the drawable itself (%lux%lu), no present pass after it (DM_FX_DIRECT=0 keeps the pass)", (unsigned long)ow, (unsigned long)oh);
    *wrote = direct;
    return out;
}
