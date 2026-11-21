// Small draw passes take 16x16 tiles (tile_small): a pass whose 32x32 tiles, times its layers, number fewer than four per GPU core opens with 16x16 tiles, a larger one with
// Metal's own; and the picture is the same either way: 300 overlapping triangles blended in order (source over, so a reordered pair changes the bytes) into a 128x128 target,
// with and without the rule (DM_NVN_TILE_SMALL=ab, its two phases), read back and compared byte for byte.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { SIDE = 128, TRIS = 300 };
static NSString *const src = @"#include <metal_stdlib>\nusing namespace metal;\n"
    "struct VO { float4 p [[position]]; float4 c; };\n"
    "static float h(uint x) { x ^= x >> 16; x *= 0x7feb352dU; x ^= x >> 15; x *= 0x846ca68bU; x ^= x >> 16; return float(x & 0xffffff) / 16777215.0; }\n"
    "vertex VO v_tri(uint n [[vertex_id]], uint i [[instance_id]]) { uint k = i * 3 + n; VO o; o.p = float4(h(k * 2) * 2.4 - 1.2, h(k * 2 + 1) * 2.4 - 1.2, 0, 1);\n"
    "  o.c = float4(h(i * 7 + 1000), h(i * 7 + 2000), h(i * 7 + 3000), 0.3 + 0.6 * h(i * 7 + 4000)); return o; }\n"
    "fragment float4 f_col(VO in [[stage_in]]) { return in.c; }\n";

static tex_t target(uint32_t w, uint32_t h, uint32_t layers, id<MTLTexture> *mt) {
    tex_t t = {.magic = MAGIC('T', 'E', 'X', 0), .serial = nvn_new_serial()};
    t.d = (nvn_texdesc){.target = layers > 1 ? NVN_TEXTURE_TARGET_2D_ARRAY : NVN_TEXTURE_TARGET_2D, .format = NVN_FORMAT_RGBA8, .width = w, .height = h, .depth = layers, .levels = 1, .samples = 1};
    MTLTextureDescriptor *td = tex_desc(&t, 0); td.storageMode = MTLStorageModeShared;
    id<MTLTexture> o = [g_dev newTextureWithDescriptor:td]; res_add(o); mtl_tex_init(&t); serial_put(t.serial, o); *mt = o;
    return t;
}
static id<MTL4RenderCommandEncoder> pass(tex_t *t, int layered) {
    state_reset(); memset(&R, 0, sizeof R);
    R.has_rt = R.has_sc = 1; R.sc = (p_scissor){0, 0, (int)t->d.width, (int)t->d.height}; R.rt.n = 1; R.rt.color[0] = t; R.clear_c[0] = 1; R.cc[0] = MTLClearColorMake(0, 0, 0, 1);
    g_rp_layered = layered; id<MTL4RenderCommandEncoder> e = rp_begin_ex(0); g_rp_layered = 0;
    if (e) { enc_r_viewport(e, (MTLViewport){0, 0, t->d.width, t->d.height, 0, 1}); enc_r_scissor(e, (MTLScissorRect){0, 0, t->d.width, t->d.height}); enc_r_cull(e, MTLCullModeNone); }
    return e;
}

int main(void) { @autoreleasepool {
    setenv("DM_NVN_TILE_SMALL", "ab", 1); setenv("DM_AB", "1000", 1); mtl_init();
    NSUInteger cores = gpu_cores(); printf("GPU cores: %lu\n", (unsigned long)cores);
    CHECK(cores >= 1 && cores <= 1024, "a GPU core count");
    NSError *error = nil; id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 2; }
    MTLRenderPipelineDescriptor *d = [MTLRenderPipelineDescriptor new]; MTLRenderPipelineColorAttachmentDescriptor *c = d.colorAttachments[0];
    c.pixelFormat = MTLPixelFormatRGBA8Unorm; c.blendingEnabled = YES; c.sourceRGBBlendFactor = c.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    c.destinationRGBBlendFactor = c.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    d.vertexFunction = [lib newFunctionWithName:@"v_tri"]; d.fragmentFunction = [lib newFunctionWithName:@"f_col"];
    id<MTLRenderPipelineState> pso = [g_dev newRenderPipelineStateWithDescriptor:d error:&error];
    CHECK(pso != nil, "the pipeline compiles"); if (!pso) return 2;

    // The choice: (width, height, layers) → 16x16 when the 32x32 tiles times layers are fewer than 4 per core
    static const uint32_t sizes[][3] = {{128, 128, 1}, {16, 16, 1}, {256, 256, 1}, {683, 384, 1}, {1366, 768, 1}, {128, 128, 6}, {64, 64, 4}};
    atomic_store(&g_ab_phase, 1);   // the rule on
    for (size_t i = 0; i < sizeof sizes / sizeof *sizes; i++) {
        id<MTLTexture> mt; tex_t t = target(sizes[i][0], sizes[i][1], sizes[i][2], &mt);
        uint64_t n0 = g_tile_small_n; mtl_sub_begin(); id<MTL4RenderCommandEncoder> e = pass(&t, sizes[i][2] > 1); CHECK(e != nil, "the pass opens"); rp_end();
        mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "the submission completes without Metal errors");
        NSUInteger tiles = ((sizes[i][0] + 31) / 32) * ((sizes[i][1] + 31) / 32) * sizes[i][2]; int want = tiles < 4 * cores, got = g_tile_small_n != n0;
        printf("%4ux%-4u x%u: %4lu tiles of 32x32, 16x16 tiles %s (expected %s)\n", sizes[i][0], sizes[i][1], sizes[i][2], (unsigned long)tiles, got ? "yes" : "no", want ? "yes" : "no");
        CHECK(got == want, "16x16 tiles exactly for passes with fewer 32x32 tiles than four per core");
        mtl_tex_release(&t);
    }
    CHECK(4 * cores > 16, "a 128x128 pass has fewer tiles than four per core on every Apple GPU (5 cores or more)");
    atomic_store(&g_ab_phase, 0); { id<MTLTexture> mt; tex_t t = target(SIDE, SIDE, 1, &mt); uint64_t n0 = g_tile_small_n;
        mtl_sub_begin(); pass(&t, 0); rp_end(); mtl_sub_commit(); mtl_drain(); CHECK(g_tile_small_n == n0, "with the rule off a small pass keeps Metal's tiles"); mtl_tex_release(&t); }

    // The picture: the same 300 blended triangles with the rule off (phase 0) and on (phase 1)
    static uint8_t px[2][SIDE * SIDE * 4];
    for (int on = 0; on < 2; on++) {
        atomic_store(&g_ab_phase, on); id<MTLTexture> mt; tex_t t = target(SIDE, SIDE, 1, &mt); uint64_t n0 = g_tile_small_n;
        mtl_sub_begin(); id<MTL4RenderCommandEncoder> e = pass(&t, 0); enc_r_pso(e, pso);
        enc_r_draw_instanced(e, MTLPrimitiveTypeTriangle, 0, 3, TRIS, 0); rp_end();
        mtl_sub_commit(); mtl_drain(); CHECK(!mtl_last_commit_error(), "the drawing completes without Metal errors");
        CHECK((g_tile_small_n != n0) == on, "the drawing pass took the tiles its phase gives");
        [mt getBytes:px[on] bytesPerRow:SIDE * 4 fromRegion:MTLRegionMake2D(0, 0, SIDE, SIDE) mipmapLevel:0];
        mtl_tex_release(&t);
    }
    int lit = 0, differ = 0; for (int i = 0; i < SIDE * SIDE; i++) { lit += px[0][i * 4] || px[0][i * 4 + 1] || px[0][i * 4 + 2]; differ += memcmp(px[0] + i * 4, px[1] + i * 4, 4) != 0; }
    printf("picture: %d of %d pixels drawn, %d differ between Metal's tiles and 16x16\n", lit, SIDE * SIDE, differ);
    CHECK(lit > SIDE * SIDE / 2, "the triangles cover most of the target");
    CHECK(differ == 0, "16x16 tiles draw the same picture, byte for byte");
    printf("== %d checks, %d failed ==\n", checks, failed); return failed != 0;
} }
