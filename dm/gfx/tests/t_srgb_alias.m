// Two NVN textures over the same storage that differ only by the formats of an sRGB / linear pair, or by two 32-bit formats (R32 integers among them), share their bytes: what
// is written through one reads back through the other (an RGBA8 render target sampled through an RGBA8_SRGB texture; an RGB9E5 picture written through an R32_UI image).
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)
int main(void) { @autoreleasepool {
    mtl_init(); mtl_sub_begin();
    tex_t lin = {.magic=MAGIC('T','E','X',0), .serial=99501};
    lin.d = (nvn_texdesc){.target=NVN_TEXTURE_TARGET_2D, .format=NVN_FORMAT_RGBA8, .width=4, .height=4, .depth=1, .levels=1, .samples=1};
    nvn_tex_layout(&lin.d, &lin.L);
    tex_t srgb = lin; srgb.serial++; srgb.d.format = NVN_FORMAT_RGBA8_SRGB;
    tex_t other = lin; other.serial += 2; other.d.format = NVN_FORMAT_RGBA16F;   // another size of texel: storage of its own
    mtl_tex_init(&lin); mtl_tex_init(&srgb); mtl_tex_init(&other);
    id<MTLTexture> base = tex_mtl(&lin), view = tex_mtl(&srgb);
    CHECK(base && view && view.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "the sRGB texture has its own format");
    CHECK(view.parentTexture == base, "the sRGB texture is a view of the linear texture's storage");
    CHECK(tex_mtl(&other) != base && tex_mtl(&other).parentTexture == nil, "a format of another texel size keeps its own texture");
    uint32_t pixels[16]; for (int i = 0; i < 16; i++) pixels[i] = 0x44332211u + (uint32_t)i;
    id<MTLBuffer> src = [g_dev newBufferWithBytes:pixels length:sizeof pixels options:MTLResourceStorageModeShared]; res_add(src);
    id<MTLBuffer> dst = [g_dev newBufferWithLength:sizeof pixels options:MTLResourceStorageModeShared]; res_add(dst);
    id<MTL4ComputeCommandEncoder> encoder = blit();
    enc_b_copy_buffer_texture(encoder, src, 0, 16, sizeof pixels, MTLSizeMake(4,4,1), base, 0, 0, MTLOriginMake(0,0,0));
    encoder = blit(); enc_b_copy_texture_buffer(encoder, view, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), dst, 0, 16, sizeof pixels);
    mtl_sub_commit(); mtl_drain();
    CHECK(!memcmp(dst.contents, pixels, sizeof pixels), "pixels written through the linear texture read back through the sRGB one");
    // The linear texture upgraded after the view was made (image writes): the view follows the upgraded storage
    id<MTLTexture> up = tex_upgrade(&lin, base, MTLTextureUsageShaderWrite, base.pixelFormat), view2 = tex_mtl(&srgb);
    CHECK(up != base && tex_mtl(&lin) == up, "the linear texture is upgraded");
    CHECK(view2.parentTexture == up && view2.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB, "the sRGB texture is a view of the upgraded storage");
    for (int i = 0; i < 16; i++) ((uint32_t *)src.contents)[i] = 0x88776655u + (uint32_t)i;
    mtl_sub_begin(); encoder = blit();
    enc_b_copy_buffer_texture(encoder, src, 0, 16, sizeof pixels, MTLSizeMake(4,4,1), up, 0, 0, MTLOriginMake(0,0,0));
    encoder = blit(); enc_b_copy_texture_buffer(encoder, view2, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(4,4,1), dst, 0, 16, sizeof pixels);
    mtl_sub_commit(); mtl_drain();
    CHECK(!memcmp(dst.contents, src.contents, sizeof pixels), "pixels written through the upgraded linear texture read back through the sRGB view");
    // An upgrade asked through the view upgrades the parent; the view is made again on it
    id<MTLTexture> up2 = tex_upgrade(&srgb, view2, MTLTextureUsageRenderTarget, view2.pixelFormat);
    CHECK(up2.pixelFormat == MTLPixelFormatRGBA8Unorm_sRGB && up2.parentTexture == tex_mtl(&lin) && tex_mtl(&lin) != up && tex_mtl(&srgb) == up2, "an upgrade through the view upgrades the parent and keeps the pair sharing");
    {   // A finalized sRGB texture resolved again (a replayed record): its view goes to the ghost ring. Its parent then leaves the sharing table: the next resolution takes the stale
        // view's ghost entry (own_view_drop_stale) and makes the texture's own, and the held counts match the tables throughout
        tex_t l2 = lin; l2.serial = 99511; l2.d.width = l2.d.height = 8; nvn_tex_layout(&l2.d, &l2.L);
        tex_t s2 = l2; s2.serial = 99512; s2.d.format = NVN_FORMAT_RGBA8_SRGB;
        mtl_tex_init(&l2); mtl_tex_init(&s2);
        id<MTLTexture> p = tex_mtl(&l2), v = tex_mtl(&s2);
        CHECK(p && v.parentTexture == p && serial_get(s2.serial) == v, "a live sRGB texture is a view of the shared linear one");
        mtl_tex_release(&s2);
        CHECK(!tex_get(s2.serial), "finalized: its live entry goes");
        id<MTLTexture> gv = tex_mtl(&s2);
        CHECK(gv.parentTexture == p && tex_get(s2.serial) == gv && !serial_get(s2.serial), "resolved again after its finalize: a view of the shared texture, in the ghost ring");
        char why[256] = ""; pthread_mutex_lock(&g_smu); int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
        CHECK(ok, why);
        alias_key ak = tex_alias_key(&l2);
        pthread_mutex_lock(&g_smu); int gone = own_alias_del_locked(&ak); pthread_mutex_unlock(&g_smu);   // as a pool's finalize evicts it
        CHECK(gone && tex_get(s2.serial) == gv, "the parent leaves the sharing table; the ghost entry still names the view");
        id<MTLTexture> own = tex_mtl(&s2);
        pthread_mutex_lock(&g_smu); int held = entry_holds((__bridge void *)gv); pthread_mutex_unlock(&g_smu);
        CHECK(own && own != gv && !own.parentTexture && tex_get(s2.serial) == own && !held, "the next resolution drops the stale view and makes the texture's own, in its ghost entry");
        pthread_mutex_lock(&g_smu); ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
        CHECK(ok, why);
        mtl_tex_release(&l2);
    }
    {   // 32-bit formats over one storage, either made first: Ocarina of Time's upscaler writes its RGB9E5 picture through an R32_UI image and samples it as RGB9E5
        void *mem = NULL; posix_memalign(&mem, 16384, 1 << 20); memset(mem, 0, 1 << 20);
        pool_t pool = {.magic = MAGIC('P','O','O','L'), .flags = NVN_MEMORY_POOL_FLAGS_CPU_NO_ACCESS | NVN_MEMORY_POOL_FLAGS_GPU_CACHED, .mem = mem, .size = 1 << 20};
        tex_t e5 = lin; e5.serial = 99521; e5.pool = &pool; e5.off = 0x10000; e5.mem = pool.mem + e5.off; e5.d.width = e5.d.height = 8; e5.d.format = NVN_FORMAT_RGB9E5F; nvn_tex_layout(&e5.d, &e5.L);
        tex_t u32 = e5; u32.serial = 99522; u32.d.format = NVN_FORMAT_R32UI;
        tex_t u2 = e5; u2.serial = 99523; u2.off = 0x20000; u2.mem = pool.mem + u2.off; u2.d.format = NVN_FORMAT_R32UI;
        tex_t f11 = u2; f11.serial = 99524; f11.d.format = NVN_FORMAT_R11G11B10F;
        tex_t r32f = f11; r32f.serial = 99525; r32f.d.format = NVN_FORMAT_R32F;
        tex_t loose = u2; loose.serial = 99526; loose.pool = NULL; loose.mem = NULL; loose.off = 0x30000; tex_t loose2 = loose; loose2.serial = 99527; loose2.d.format = NVN_FORMAT_RGB9E5F;
        mtl_tex_init(&e5); mtl_tex_init(&u32); mtl_tex_init(&u2); mtl_tex_init(&f11); mtl_tex_init(&r32f); mtl_tex_init(&loose); mtl_tex_init(&loose2);
        id<MTLTexture> pe = tex_mtl(&e5), vu = tex_mtl(&u32);
        CHECK(pe && vu.pixelFormat == MTLPixelFormatR32Uint && vu.parentTexture == pe, "an R32_UI texture over an RGB9E5 texture's storage is a view of it");
        id<MTLTexture> pu = tex_mtl(&u2);
        CHECK(pu && !(pu.usage & MTLTextureUsagePixelFormatView) && !pu.parentTexture, "an R32_UI texture made first has no format-view usage");
        id<MTLTexture> vf = tex_mtl(&f11), pu2 = tex_mtl(&u2);
        CHECK(vf.pixelFormat == MTLPixelFormatRG11B10Float && vf.parentTexture == pu2 && pu2 != pu && (pu2.usage & MTLTextureUsagePixelFormatView),
              "an R11G11B10F texture made after it is a view of its storage, upgraded with the format-view usage under every name");
        id<MTLTexture> vr = tex_mtl(&r32f);
        CHECK(vr.pixelFormat == MTLPixelFormatR32Float && vr.parentTexture == pu2, "a third 32-bit format over the storage is a view of the same parent");
        id<MTLTexture> l1 = tex_mtl(&loose), l2 = tex_mtl(&loose2);
        CHECK(l1 && l2 && l1 != l2 && !l2.parentTexture, "textures in no memory pool share no storage across formats");
        uint32_t words[64]; for (int i = 0; i < 64; i++) words[i] = 0x3c0ff00fu + (uint32_t)i * 0x01010101u;
        id<MTLBuffer> s2 = [g_dev newBufferWithBytes:words length:sizeof words options:MTLResourceStorageModeShared]; res_add(s2);
        id<MTLBuffer> d1 = [g_dev newBufferWithLength:sizeof words options:MTLResourceStorageModeShared]; res_add(d1);
        id<MTLBuffer> d2 = [g_dev newBufferWithLength:sizeof words options:MTLResourceStorageModeShared]; res_add(d2);
        mtl_sub_begin(); encoder = blit();
        enc_b_copy_buffer_texture(encoder, s2, 0, 32, sizeof words, MTLSizeMake(8,8,1), vu, 0, 0, MTLOriginMake(0,0,0));
        encoder = blit(); enc_b_copy_buffer_texture(encoder, s2, 0, 32, sizeof words, MTLSizeMake(8,8,1), pu2, 0, 0, MTLOriginMake(0,0,0));
        encoder = blit(); enc_b_copy_texture_buffer(encoder, pe, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(8,8,1), d1, 0, 32, sizeof words);
        encoder = blit(); enc_b_copy_texture_buffer(encoder, vf, 0, 0, MTLOriginMake(0,0,0), MTLSizeMake(8,8,1), d2, 0, 32, sizeof words);
        mtl_sub_commit(); mtl_drain();
        CHECK(!memcmp(d1.contents, words, sizeof words), "words written through the R32_UI view read back through the RGB9E5 texture");
        CHECK(!memcmp(d2.contents, words, sizeof words), "words written through the upgraded R32_UI texture read back through the R11G11B10F view");
        char why[256] = ""; pthread_mutex_lock(&g_smu); int ok = own_check_locked(why, sizeof why); pthread_mutex_unlock(&g_smu);
        CHECK(ok, why);
        mtl_tex_release(&e5); mtl_tex_release(&u32); mtl_tex_release(&u2); mtl_tex_release(&f11); mtl_tex_release(&r32f); mtl_tex_release(&loose); mtl_tex_release(&loose2);
    }
    printf("sRGB / linear and 32-bit aliases: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
