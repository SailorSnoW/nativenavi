// The MetalFX scalers of the game's upscaler (nvn_upscaler.inc up_mfx_scaler_for; docs/reference/rendering.md, A scaler the cache gave up left the residency set at once):
// a recently used scaler stays when a new size comes (least recently used eviction), an evicted scaler stays alive and its own input and output textures in the residency set
// while the submission is open, and they leave the set and the scaler goes once it has completed. No game shader runs: the "upscaler" reads one texture and writes one image.
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
static tex_t texture(uint64_t serial, unsigned w, unsigned h, int format) {
    tex_t t = {.magic = MAGIC('T','E','X',0), .serial = serial, .off = serial << 24};
    t.d = (nvn_texdesc){.target = NVN_TEXTURE_TARGET_2D, .format = format, .width = w, .height = h, .depth = 1, .levels = 1, .samples = 1};
    nvn_tex_layout(&t.d, &t.L); return t;
}
static NvShader *upscaler(const char *key) {
    NvShader *s = [NvShader new]; snprintf(s->key, sizeof s->key, "%s", key); s->up_color = UP_COLOR_ROOT;
    s->rf.nb = 2; s->rf.b[0] = (nvn_rbind){.kind = NVN_RB_TEX, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 0};
    s->rf.b[1] = (nvn_rbind){.kind = NVN_RB_IMG, .dtt = MTLTextureType2D, .dpf = NVN_DPF_FLOAT, .reg = 1, .rw = 1};
    return s;
}
static int resident(id o) { pthread_mutex_lock(&g_res_mu); int r = o && [g_res_objects containsObject:o]; pthread_mutex_unlock(&g_res_mu); return r; }
int main(void) { @autoreleasepool {
    if (!DM_GAME_UPSCALER_N) { printf("t_mfx_evict: this profile has no upscaler shaders, nothing to check\n"); return 0; }
    setenv("DM_UPSCALER", "metalfx", 1);
    mtl_init();
    if (![MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) { printf("t_mfx_evict: no Metal 4 MetalFX on this GPU, skipped\n"); return 0; }
    // square-rooted R11G11B10F into R11G11B10F, as the game's upscaler: input (the squared copy) and output (drawn back) are the scaler's own RGBA16F textures
    tex_t in = texture(77, 960, 540, NVN_FORMAT_R11G11B10F), out = texture(78, 1920, 1080, NVN_FORMAT_R11G11B10F);
    out.d.flags |= NVN_TEXTURE_FLAGS_IMAGE;
    mtl_tex_init(&in); mtl_tex_init(&out);
    mtl_reg_tex(701, &in, NULL); mtl_reg_image(702, &out, NULL);
    char key[24]; snprintf(key, sizeof key, "%.16s_b", DM_GAME_UPSCALER_KEYS);   // the profile's first upscaler
    NvShader *named = upscaler(key);
    memset(&S, 0, sizeof S); S.tex[5][0] = 701; S.img[5][1] = 702;
    { const uint32_t rects[8] = {0, 0, 959, 539, 0, 0, 1919, 1079}; memcpy(S.push[1], rects, sizeof rects); }

    // 1. One frame's scale, completed: its scaler has input and output textures of its own
    mtl_sub_begin();
    CHECK(up_mfx_encode(named), "the first scale is drawn by MetalFX");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the first submission completes");
    int slot = -1; for (int i = 0; i < UP_MFX_SCALERS; i++) if (g_up_mfx[i].out) slot = i;
    CHECK(slot >= 0 && g_up_mfx[slot].in && g_up_mfx[slot].out, "the scaler keeps both input and output textures of its own");
    if (slot < 0 || !g_up_mfx[slot].in || !g_up_mfx[slot].out) { printf("t_mfx_evict: %d checks, %d failed\n", checks, failed); return 1; }
    id<MTLTexture> own_in = g_up_mfx[slot].in, own_out = g_up_mfx[slot].out;
    __weak id own_scaler = g_up_mfx[slot].sc;
    CHECK(resident(own_in) && resident(own_out), "the scaler's own textures are in the residency set");

    // 2. The other slots filled, the first scaler used again, one more size: a colder slot goes
    mtl_sub_begin();
    int made = 0;
    for (int i = 0; i < UP_MFX_SCALERS - 1; i++) { made++;
        up_mfx_scaler_for(640 + 16 * (NSUInteger)made, 360 + 16 * (NSUInteger)made, 1280 + 32 * (NSUInteger)made, 720 + 32 * (NSUInteger)made, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float); }
    CHECK(up_mfx_scaler_for(960, 540, 1920, 1080, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float) == &g_up_mfx[slot], "touching the first scaler hits its slot");
    made++;
    up_mfx_scaler_for(640 + 16 * (NSUInteger)made, 360 + 16 * (NSUInteger)made, 1280 + 32 * (NSUInteger)made, 720 + 32 * (NSUInteger)made, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float);
    CHECK(g_up_mfx[slot].out == own_out && g_up_mfx[slot].in == own_in, "LRU keeps the recently used scaler when a new size arrives");

    // 3. New sizes until the first scaler is evicted, the submission still open: it stays alive and its textures in the set
    int extra = 0;
    while (g_up_mfx[slot].out == own_out && extra < 2 * UP_MFX_SCALERS) { made++; extra++;
        up_mfx_scaler_for(640 + 16 * (NSUInteger)made, 360 + 16 * (NSUInteger)made, 1280 + 32 * (NSUInteger)made, 720 + 32 * (NSUInteger)made, 0, MTLPixelFormatRGBA16Float, MTLPixelFormatRGBA16Float); }
    CHECK(g_up_mfx[slot].out != own_out, "cold sizes eventually take the first scaler's slot");
    CHECK(own_scaler != nil, "the evicted scaler stays alive until the open submission has completed: a Metal 4 command buffer does not retain it");
    int in_ok = resident(own_in), out_ok = resident(own_out);
    CHECK(in_ok && out_ok, "both textures of the evicted scaler stay in the residency set until the open submission, behind whose fence they were retired, has completed");
    CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "the second submission completes");

    // 4. Once the submissions that used them completed and drained: out of the set, and the scaler released
    if (in_ok && out_ok) {
        mtl_sub_begin(); CHECK(mtl_wait(mtl_sub_commit(), 5000000000ull) && !mtl_last_commit_error(), "an empty submission completes");
        for (int i = 0; i < 100 && (resident(own_in) || resident(own_out) || own_scaler != nil); i++) { defer_drain(); res_commit(); usleep(10000); }
        CHECK(!resident(own_in) && !resident(own_out), "after the submissions that used them, both evicted textures leave the residency set");
        CHECK(own_scaler == nil, "after the submissions that used it, the evicted scaler is released");
    }
    printf("t_mfx_evict: %d checks, %d failed\n", checks, failed);
    return failed != 0;
}}
