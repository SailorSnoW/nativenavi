// Present crop regression: colored 8×8 texture, closed-form RGBA expectations; real Metal rendering, no dependence on the game or on cross-run determinism.
// nvn_mtl.m's nvn_game_fps is a weak default and hle_nvn.c gives the strong definition; in one translation unit that is a redefinition, so the weak one is renamed (this test does not look at the frame-rate target)
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle   // both files have a static g_ft (nvn_mtl.m's fragment argument table, hle_nvn.c's textures a flush can mark CPU-written), so hle_nvn.c's is renamed too
#include "../nvn/hle_nvn.c"
#undef g_ft
uint64_t g_t0_ns;   // process start time from host main.c (used by hle_nvn's per-frame @t_s); unit test stub
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
#ifdef CROP_LEGACY_HEADER
#include CROP_LEGACY_HEADER
#define ENCODE legacy_present_encode
#else
#define ENCODE present_encode
#endif
static int failed, cases;
#define EXPECT(n, w, v) do { int a=(w),b=(v); cases++; printf("%s expected %d → got %d %s\n",n,a,b,a==b?"✓":"✗");failed+=a!=b; } while(0)
static const uint8_t colors[4][4]={{255,0,0,255},{0,255,0,255},{0,0,255,255},{255,255,0,255}};
static void present_progress(uint64_t *queued, uint64_t *done) { pthread_mutex_lock(&g_pq_mu); *queued = g_pq_in; *done = g_pq_done; pthread_mutex_unlock(&g_pq_mu); }   // the display queue's counters

#ifndef CROP_LEGACY_HEADER
// Exercise the persistent scaler used by real presents, completing each command buffer before its allocator and output leave scope.
static int check_present_resize(id<MTLTexture> src, int w, int h) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModePrivate;d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> out=[g_dev newTextureWithDescriptor:d];res_add(out);res_commit();
    id<MTL4CommandAllocator> al=[g_dev newCommandAllocator];id<MTL4CommandBuffer> cb=[g_dev newCommandBuffer];id<MTLSharedEvent> ev=[g_dev newSharedEvent];
    [cb beginCommandBufferWithAllocator:al];int kind=present_encode_m4(cb,src,out,NULL,g_present_uv[0],&g_fx_present);[cb endCommandBuffer];
    [g_q4 commit:&cb count:1];[g_q4 signalEvent:ev value:1];
    EXPECT("resize present completes",1,[ev waitUntilSignaledValue:1 timeoutMS:10000]);res_remove(out);return kind;
}
#endif

static void check(id<MTLTexture> src, const char *name, const int crop[4], int w, int h, int color) {
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];
    d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> dst=[g_dev newTextureWithDescriptor:d];id<MTLCommandBuffer> cb=[g_q commandBuffer];
    EXPECT("encode succeeds",1,ENCODE(cb,src,dst,crop));[cb commit];[cb waitUntilCompleted];
    EXPECT("GPU done",MTLCommandBufferStatusCompleted,cb.status);
    uint8_t *out=malloc(w*h*4);[dst getBytes:out bytesPerRow:w*4 fromRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0];
    int bad=0;
    for(int y=0;y<h;y++)for(int x=0;x<w;x++) {
        int q=color;
        if(q==4) { // square valid area onto a 2:1 target: 4 black columns on each side, the middle 8 columns all red.
            static const uint8_t black[4]={0,0,0,255};
            const uint8_t *want=x<4||x>=12?black:colors[0];bad+=memcmp(out+4*(y*w+x),want,4)!=0;continue;
        }
        if(q==-2) { // crop=(1,1,4,4): 3×3 red, last column green, last row blue, bottom-right 1 cell yellow.
            q=(x+1>=4)+2*(y+1>=4);
        } else if(q<0) { // same-size default whole frame: the boundary is dead center.
            q=(x>=w/2)+2*(y>=h/2);
        }
        bad+=memcmp(out+4*(y*w+x),colors[q],4)!=0;
    }
    EXPECT(name,0,bad);free(out);
}

#ifndef CROP_LEGACY_HEADER
// Minification (present.metal): each minified axis filtered by a radius-1 tent stretched by min(step, 3), a non-minified one bilinear. The model weighs each texel
// itself, so the GPU's paired bilinear fetches are checked against the exact filter.
static double tent_ref(double x) { x = fabs(x); return x < 1.0 ? 1.0 - x : 0.0; }
static double bil_axis(const uint8_t *v, int w, int y, double cx) {
    double u = fmin(fmax(cx, 0.5), w - 0.5) - 0.5;
    int x0 = (int)u, x1 = x0 + 1 < w ? x0 + 1 : x0; double fx = u - x0;
    return v[4 * (y * w + x0)] * (1.0 - fx) + v[4 * (y * w + x1)] * fx;
}
static double tent_x(const uint8_t *v, int w, int y, double cx, double stepx) {
    if (stepx < 1.001) return bil_axis(v, w, y, cx);
    double sx = fmin(stepx, 3.0), sum = 0.0, ws = 0.0;
    int i0 = (int)floor(cx - 0.5 - sx) + 1, last = (int)ceil(cx - 0.5 + sx) - 1;
    for (int i = i0; i <= last; i++) {
        double wt = tent_ref((i + 0.5 - cx) / sx);
        int ci = i < 0 ? 0 : i >= w ? w - 1 : i;
        sum += wt * v[4 * (y * w + ci)]; ws += wt;
    }
    return ws > 0.0 ? sum / ws : bil_axis(v, w, y, cx);
}
static double minify_ref(const uint8_t *v, int w, int h, int ow, int oh, int x, int y) {
    double stepx = (double)w / ow, stepy = (double)h / oh, cx = (x + 0.5) * stepx, cy = (y + 0.5) * stepy;
    if (stepy < 1.001) {
        double t = fmin(fmax(cy, 0.5), h - 0.5) - 0.5;
        int y0 = (int)t, y1 = y0 + 1 < h ? y0 + 1 : y0; double fy = t - y0;
        return tent_x(v, w, y0, cx, stepx) * (1.0 - fy) + tent_x(v, w, y1, cx, stepx) * fy;
    }
    double sy = fmin(stepy, 3.0), sum = 0.0, ws = 0.0;
    int j0 = (int)floor(cy - 0.5 - sy) + 1, last = (int)ceil(cy - 0.5 + sy) - 1;
    for (int j = j0; j <= last; j++) {
        double wt = tent_ref((j + 0.5 - cy) / sy);
        int cj = j < 0 ? 0 : j >= h ? h - 1 : j;
        sum += wt * tent_x(v, w, cj, cx, stepx); ws += wt;
    }
    return ws > 0.0 ? sum / ws : tent_x(v, w, 0, cx, stepx);
}
// A w x h picture (or its crop, 255 around it) with red = pattern(x, y) presented into ow x oh: the red channel's largest difference from the model and the spread of
// the inner columns. m4: through present_offscreen_wait (Metal 4), else present_encode.
static void check_minify_ex(const char *name, int w, int h, const int crop[4], int ow, int oh, uint8_t (*pattern)(int, int), int m4, int *maxdiff, int *spread) {
    int flip = atomic_load(&g_origin_ll);   // lower-left origin: stored bottom-up, so output row y is model row oh - 1 - y
    int cx = crop ? crop[0] : 0, cy = crop ? crop[1] : 0, cw = crop ? crop[2] : w, ch = crop ? crop[3] : h;
    MTLTextureDescriptor *sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:w height:h mipmapped:NO];sd.storageMode=MTLStorageModeShared;sd.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> s=[g_dev newTextureWithDescriptor:sd];uint8_t *px=calloc((size_t)w*h,4), *sub=calloc((size_t)cw*ch,4);
    assert(px && sub);
    for(int y=0;y<h;y++)for(int x=0;x<w;x++){
        int in = x>=cx && x<cx+cw && y>=cy && y<cy+ch;
        uint8_t v = in ? pattern(x-cx, y-cy) : 255;
        px[4*(y*w+x)]=v; px[4*(y*w+x)+3]=255;
        if (in) { sub[4*((y-cy)*cw+(x-cx))]=v; sub[4*((y-cy)*cw+(x-cx))+3]=255; }
    }
    [s replaceRegion:MTLRegionMake2D(0,0,w,h) mipmapLevel:0 withBytes:px bytesPerRow:w*4];
    MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:ow height:oh mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
    id<MTLTexture> dst=[g_dev newTextureWithDescriptor:od];
    if (m4) {
        res_add(s); res_add(dst); res_commit();
        EXPECT(name,PRESENT_STANDARD,present_offscreen_wait(s,crop,dst));
        res_remove(s); res_remove(dst);
    } else {
        id<MTLCommandBuffer> cb=[g_q commandBuffer];
        EXPECT(name,1,ENCODE(cb,s,dst,crop));[cb commit];[cb waitUntilCompleted];
    }
    uint8_t *o=malloc((size_t)ow*oh*4); assert(o);
    [dst getBytes:o bytesPerRow:ow*4 fromRegion:MTLRegionMake2D(0,0,ow,oh) mipmapLevel:0];
    int md=0,lo=255,hi=0;
    for(int y=0;y<oh;y++)for(int x=0;x<ow;x++){int r=o[4*(y*ow+x)],d=abs(r-(int)lround(minify_ref(sub,cw,ch,ow,oh,x,flip?oh-1-y:y)));md=d>md?d:md;if(x>0&&x<ow-1){lo=r<lo?r:lo;hi=r>hi?r:hi;}}
    *maxdiff=md;*spread=hi-lo;free(px);free(sub);free(o);
}
static void check_minify(const char *name, int w, int h, int ow, int oh, uint8_t (*pattern)(int, int), int *maxdiff, int *spread) {
    check_minify_ex(name, w, h, NULL, ow, oh, pattern, 0, maxdiff, spread);
}
static uint8_t stripes(int x, int y) { (void)y; return x & 1 ? 255 : 0; }   // the finest detail a picture holds: one texel on, one off
static uint8_t ramp(int x, int y) { return (uint8_t)((x * 37 + y * 91) & 255); }
#endif

// This fixture is the only present producer. A window holds a logical queue receipt; wait for replay, then for the display ticket (snapshots do not return that ticket).
static uint64_t display_ticket(uint64_t receipt, uint64_t before) {
    EXPECT("present returned a logical receipt",1,receipt!=0);
    if (!receipt) return 0;
    nvn_queue_present_wait(receipt);
    uint64_t queued, done; present_progress(&queued,&done);
    EXPECT("one frame reached the display queue",1,queued==before+1);
    if (queued!=before+1) return 0;
    mtl_present_wait(queued);
    return queued;
}

int main(int argc, char **argv) { @autoreleasepool {
    mtl_init();
#ifndef CROP_LEGACY_HEADER
    g_present_shape[0]=g_present_shape[1]=1;   // square fixtures: a square picture, so the checks below see the crop's texels and the bars of that shape; the display's own shape is checked at the end
#endif
    MTLTextureDescriptor *d=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];d.storageMode=MTLStorageModeShared;d.usage=MTLTextureUsageShaderRead;
    id<MTLTexture> src=[g_dev newTextureWithDescriptor:d];uint8_t px[8*8*4];
    for(int y=0;y<8;y++)for(int x=0;x<8;x++)memcpy(px+4*(y*8+x),colors[(x>=4)+2*(y>=4)],4);
    [src replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:px bytesPerRow:32];
    int zero[4]={0},tl[4]={0,0,4,4},tr[4]={4,0,4,4},bl[4]={0,4,4,4},br[4]={4,4,4,4},one[4]={3,3,1,1},inset[4]={1,1,4,4};
    check(src,"default whole frame 64 pixels",zero,8,8,-1);
    check(src,"NULL default whole frame 64 pixels",NULL,8,8,-1);
    check(src,"top-left crop scaled up, 256 pixels all red",tl,16,16,0);
    check(src,"non-zero x crop scaled up, 256 pixels all green",tr,16,16,1);
    check(src,"non-zero y crop scaled up, 256 pixels all blue",bl,16,16,2);
    check(src,"bottom-right crop at the edge, 256 pixels all yellow",br,16,16,3);
    check(src,"single texel scaled up without crossing edges, 256 pixels all red",one,16,16,0);
    check(src,"non-zero x and y offset, 16 pixels byte-identical to a hand-written sub-rectangle",inset,4,4,-2);
    check(src,"aspect of the valid area kept, 128 pixels red in the middle / black on both sides",tl,16,8,4);
#ifndef CROP_LEGACY_HEADER
    {   // minified 3:2, 4:3 (2560x1440 -> 1920x1080), 2:1, 5:1 (capped at 3:1), 1:1, one axis only, cropped, Metal 4, lower-left origin
        int md,sp;
        check_minify("3:2 stripes present",12,12,8,8,stripes,&md,&sp);
        EXPECT("3:2 stripes match the exact tent filter within 1",1,md<=1);
        // one bilinear tap gave columns of 64 and 191 here (spread 127): the stripes aliased into a coarser pattern of their own
        EXPECT("3:2 stripes: spread at most 30 (one tap: 127)",1,sp<=30);
        check_minify("4:3 ramp present",16,16,12,12,ramp,&md,&sp);
        EXPECT("4:3 ramp matches the exact tent filter within 1",1,md<=1);
        check_minify("2:1 ramp present",16,16,8,8,ramp,&md,&sp);
        EXPECT("2:1 ramp matches the exact tent filter within 1",1,md<=1);
        check_minify("5:1 ramp present",40,40,8,8,ramp,&md,&sp);
        EXPECT("5:1 ramp matches the tent filter at 3:1 width within 1",1,md<=1);
        check_minify("1:1 ramp present",8,8,8,8,ramp,&md,&sp);
        EXPECT("1:1 ramp: the texels as they are",0,md);
        check_minify("anisotropic ramp present (X upscaled 0.75x, Y minified 1.5x)",12,24,16,16,ramp,&md,&sp);
        EXPECT("anisotropic ramp: bilinear on X and exact tent on Y within 1",1,md<=1);
        const int midcrop[4]={6,6,12,12};
        check_minify_ex("cropped 3:2 ramp present",24,24,midcrop,8,8,ramp,0,&md,&sp);
        EXPECT("cropped 3:2 ramp: matches exact tent with no bleed outside the crop, within 1",1,md<=1);
        check_minify_ex("Metal 4 4:3 ramp present",16,16,NULL,12,12,ramp,1,&md,&sp);
        EXPECT("Metal 4 4:3 ramp matches the exact tent filter within 1",1,md<=1);
        atomic_store(&g_origin_ll,1);
        check_minify("3:2 ramp present, lower-left origin",24,24,16,16,ramp,&md,&sp);
        EXPECT("3:2 ramp with the lower-left origin is the exact tent picture upside down, within 1",1,md<=1);
        atomic_store(&g_origin_ll,0);
    }
    {
        // sRGB swapchain: present sends the raw bytes (as the Switch's scan-out does) without decoding first; decoded, 128/64/200 would come out as about 55/13/148
        MTLTextureDescriptor *sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:8 height:8 mipmapped:NO];sd.storageMode=MTLStorageModeShared;sd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> s8=[g_dev newTextureWithDescriptor:sd];uint8_t g[8*8*4];
        for(int i=0;i<64;i++){g[4*i]=128;g[4*i+1]=64;g[4*i+2]=200;g[4*i+3]=255;}
        [s8 replaceRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0 withBytes:g bytesPerRow:32];
        MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:od];id<MTLCommandBuffer> cb=[g_q commandBuffer];
        EXPECT("sRGB source encode succeeds",1,ENCODE(cb,s8,dst,zero));[cb commit];[cb waitUntilCompleted];
        uint8_t o[8*8*4];[dst getBytes:o bytesPerRow:32 fromRegion:MTLRegionMake2D(0,0,8,8) mipmapLevel:0];
        int bad=0;for(int i=0;i<64;i++)bad+=abs(o[4*i]-128)>1||abs(o[4*i+1]-64)>1||abs(o[4*i+2]-200)>1;
        EXPECT("sRGB swapchain present sends raw bytes, 64 pixels",0,bad);
    }
#endif
#ifndef CROP_LEGACY_HEADER
    {   // DM_PRESENT_UPSCALE: which content MetalFX scales, and to which size (the picture's rectangle on the canvas)
        const struct { unsigned cw,ch,dw,dh,w,h; int upscale; } sizes[] = {
            {1280,720,2868,1320,2347,1320,1},   // handheld, letterboxed on a wide canvas
            {1952,900,2868,1320,2868,1320,1},   // ratios within 1 % fill the canvas
            {1280,720,2560,1440,2560,1440,1},   // Retina window
            {1280,720,1280,720,1280,720,0},     // native size
            {1600,900,1280,720,1280,720,0},     // downscale
            {8,8,16,8,8,8,0},                  // larger canvas, native-size picture
        };
        for (unsigned i=0;i<sizeof sizes/sizeof *sizes;i++) {
            MTLViewport vp=present_viewport(sizes[i].cw,sizes[i].ch,sizes[i].dw,sizes[i].dh);
            EXPECT("MetalFX only upscales the picture",sizes[i].upscale,fx_wanted(vp,sizes[i].cw,sizes[i].ch));
            EXPECT("picture width",sizes[i].w,(int)vp.width);EXPECT("picture height",sizes[i].h,(int)vp.height);
        }
        {   // 1600 * (1710 / 1600) is 1710.0000000000002: centred on the unrounded size, the picture began at x = -1
            MTLViewport vp=present_viewport(1600,900,1710,1112);
            EXPECT("size that rounds up a hair keeps the picture inside the canvas: x",0,(int)vp.originX);EXPECT("  y",75,(int)vp.originY);
            EXPECT("  width",1710,(int)vp.width);EXPECT("  height",962,(int)vp.height);
            vp=present_viewport(1280,720,3456,2234);   // 720 * 2.7 is 1944.0000000000002: 145 rows from the top, not 144
            EXPECT("size that rounds down a hair is centred on the rounded size: y",145,(int)vp.originY);EXPECT("  height",1944,(int)vp.height);
        }
        EXPECT("empty content is not upscaled",0,fx_wanted((MTLViewport){0,0,2868,1320,0,1},0,720));
    }
    if([MTLFXSpatialScalerDescriptor supportsMetal4FX:g_dev]) {   // the present pass through MetalFX into an offscreen canvas (present_offscreen_m4, as DM_NVN_SHOT_OUT uses it)
        g_fx_mode=1;
        const int N=64;MTLTextureDescriptor *bd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm_sRGB width:N height:N mipmapped:NO];bd.storageMode=MTLStorageModeShared;bd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> big=[g_dev newTextureWithDescriptor:bd];uint8_t *bp=malloc(N*N*4);
        for(int y=0;y<N;y++)for(int x=0;x<N;x++)memcpy(bp+4*(y*N+x),colors[(x>=N/2)+2*(y>=N/2)],4);
        for(int y=N/2;y<N;y++)for(int x=0;x<N/2;x++){uint8_t g[4]={128,64,200,255};memcpy(bp+4*(y*N+x),g,4);}   // lower-left quadrant: a mid grey-violet the sRGB bytes must keep
        [big replaceRegion:MTLRegionMake2D(0,0,N,N) mipmapLevel:0 withBytes:bp bytesPerRow:N*4];free(bp);
        res_add(big);res_commit();
        const int W=4*N,H=2*N;MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:W height:H mipmapped:NO];
        od.storageMode=MTLStorageModePrivate;od.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead;id<MTLTexture> out=[g_dev newTextureWithDescriptor:od];
        const int crops[4][4]={{0},{8,8,48,48},{8,8,48,48},{8,8,48,48}};
        for (int mode=0;mode<4;mode++) {
            g_fx_mode=mode==3?0:1;atomic_store(&g_origin_ll,mode==2);
            EXPECT("offscreen present reports the scaler actually used",mode==3?PRESENT_STANDARD:PRESENT_METALFX,present_offscreen_wait(big,crops[mode],out));
            id<MTLBuffer> rb=[g_dev newBufferWithLength:W*H*4 options:MTLResourceStorageModeShared];id<MTLCommandBuffer> cb=[g_q commandBuffer];id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];
            [bl copyFromTexture:out sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(W,H,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:W*4 destinationBytesPerImage:W*H*4];
            [bl endEncoding];[cb commit];[cb waitUntilCompleted];
            const uint8_t *o=rb.contents;   // BGRA
        #define PX(x,y,c) o[4*((y)*W+(x))+(c)]
            int bars=0;for(int y=0;y<H;y++)for(int x=0;x<W;x++)if(x<64||x>=192)bars+=PX(x,y,0)||PX(x,y,1)||PX(x,y,2);
            EXPECT("  black bars beside the square picture, 0 lit pixels",0,bars);
            int bad=0;const int cxs[2]={64+32,64+96},cys[2]={32,96};
            for(int q=0;q<4;q++){int x=cxs[q&1],y=cys[q>>1],sq=mode==2?q^2:q;const uint8_t *want=sq==2?(const uint8_t[4]){128,64,200,255}:colors[sq];
                bad+=abs(PX(x,y,2)-want[0])>3||abs(PX(x,y,1)-want[1])>3||abs(PX(x,y,0)-want[2])>3;}
            EXPECT("  quadrant centres keep their colours, raw bytes of the sRGB source (4 checked)",0,bad);
        #undef PX
        }
        atomic_store(&g_origin_ll,0);g_fx_mode=1;
        MTLTextureDescriptor *smallD=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:32 height:32 mipmapped:NO];
        smallD.storageMode=MTLStorageModePrivate;smallD.usage=MTLTextureUsageRenderTarget;
        id<MTLTexture> small=[g_dev newTextureWithDescriptor:smallD];int whole[4]={0},invalidCrop[4]={63,0,2,2};
        EXPECT("MetalFX requested but downscaling uses standard",PRESENT_STANDARD,present_offscreen_wait(big,whole,small));
        EXPECT("offscreen rejects an invalid crop before submission",PRESENT_FAILED,present_offscreen_wait(big,invalidCrop,out));

        uint64_t savedPresent=g_present_n;g_present_n=1;
        EXPECT("initial present makes its scaler",PRESENT_METALFX,check_present_resize(big,W,H));
        id<MTL4FXSpatialScaler> first=g_fx_present.sc;
        g_present_n=2;
        EXPECT("resizing within 30 presents falls back to standard",PRESENT_STANDARD,check_present_resize(big,192,96));
        EXPECT("resizing keeps the previous scaler until replacement",1,g_fx_present.sc==first);
        g_present_n=31;
        EXPECT("stable new size makes its scaler",PRESENT_METALFX,check_present_resize(big,192,96));
        EXPECT("replacement has the new picture size",96,(int)g_fx_present.sc.outputWidth);
        EXPECT("exactly two scalers made",2,g_fx_present.made);
        g_present_n=32;
        EXPECT("same size reuses its scaler",PRESENT_METALFX,check_present_resize(big,192,96));
        EXPECT("reuse makes no new scaler",2,g_fx_present.made);
        // DM_FX_INPLACE and DM_FX_DIRECT: the scaler reads a private window texture in place and writes a canvas it can write, while the frame that draws that texture is
        // still running on the queue (committed just before, as a present follows its frame). The scaler must still see the whole frame: the encoder before it orders it after
        // the queue's earlier work only around work of its own (an encoder of barriers alone orders nothing)
        {
            const int S=256,ROUNDS=8;
            MTLTextureDescriptor *wd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:S height:S mipmapped:NO];
            wd.storageMode=MTLStorageModePrivate;wd.usage=MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;id<MTLTexture> win=[g_dev newTextureWithDescriptor:wd];
            MTLTextureDescriptor *cd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatBGRA8Unorm width:2*S height:2*S mipmapped:NO];
            cd.storageMode=MTLStorageModePrivate;cd.usage=MTLTextureUsageRenderTarget|MTLTextureUsageShaderRead|MTLTextureUsageShaderWrite;id<MTLTexture> canvas=[g_dev newTextureWithDescriptor:cd];
            id<MTLBuffer> args=[g_dev newBufferWithLength:16 options:MTLResourceStorageModeShared],rb=[g_dev newBufferWithLength:4*S*S*4 options:MTLResourceStorageModeShared];
            res_add(win);res_add(canvas);res_add(args);res_commit();
            NSError *err=nil;id<MTL4Compiler> comp=[g_dev newCompilerWithDescriptor:[MTL4CompilerDescriptor new] error:&err];
            id<MTLLibrary> lib=[g_dev newLibraryWithSource:@"#include <metal_stdlib>\nusing namespace metal;\nstruct A { uint iters; uint tag; };\n"
                "kernel void frame(constant A &a [[buffer(0)]], texture2d<float, access::write> t [[texture(0)]], uint2 i [[thread_position_in_grid]]) {\n"
                "  float x = float(i.x); for (uint k = 0; k < a.iters; k++) x = fma(x, 1.0000001f, 0.5f); float g = x > -1.0f ? float(a.tag) / 255.0f : 0.0f; t.write(float4(g, g, g, 1), i); }\n" options:nil error:&err];
            MTL4LibraryFunctionDescriptor *fd=[MTL4LibraryFunctionDescriptor new];fd.name=@"frame";fd.library=lib;
            MTL4ComputePipelineDescriptor *pd=[MTL4ComputePipelineDescriptor new];pd.computeFunctionDescriptor=fd;
            id<MTLComputePipelineState> ps=lib&&comp?[comp newComputePipelineStateWithDescriptor:pd compilerTaskOptions:nil error:&err]:nil;
            EXPECT("the slow frame kernel compiles",1,ps!=nil);
            MTL4ArgumentTableDescriptor *ad=[MTL4ArgumentTableDescriptor new];ad.maxBufferBindCount=1;ad.maxTextureBindCount=1;
            id<MTL4ArgumentTable> at=[g_dev newArgumentTableWithDescriptor:ad error:&err];
            id<MTL4CommandAllocator> fal=[g_dev newCommandAllocator],pal=[g_dev newCommandAllocator];id<MTLSharedEvent> ev=[g_dev newSharedEvent];
            g_present_n=1000;int stale=0,kinds=0;
            for (int round=1;ps&&round<=ROUNDS;round++) {
                uint32_t tag=40+round*23;((uint32_t *)args.contents)[0]=50000;((uint32_t *)args.contents)[1]=tag;
                [at setAddress:args.gpuAddress atIndex:0];[at setTexture:win.gpuResourceID atIndex:0];[fal reset];[pal reset];
                id<MTL4CommandBuffer> fcb=[g_dev newCommandBuffer];[fcb beginCommandBufferWithAllocator:fal];
                id<MTL4ComputeCommandEncoder> ce=[fcb computeCommandEncoder];[ce setComputePipelineState:ps];[ce setArgumentTable:at];
                [ce dispatchThreads:MTLSizeMake(S,S,1) threadsPerThreadgroup:MTLSizeMake(16,16,1)];[ce endEncoding];[fcb endCommandBuffer];
                id<MTL4CommandBuffer> pcb=[g_dev newCommandBuffer];[pcb beginCommandBufferWithAllocator:pal];
                kinds+=present_encode_m4(pcb,win,canvas,NULL,g_present_uv[0],&g_fx_present)==PRESENT_METALFX;[pcb endCommandBuffer];
                [g_q4 commit:&fcb count:1];[g_q4 commit:&pcb count:1];[g_q4 signalEvent:ev value:round];
                if (![ev waitUntilSignaledValue:round timeoutMS:10000]) { EXPECT("in-place present completes",1,0); break; }
                id<MTLCommandBuffer> cb=[g_q commandBuffer];id<MTLBlitCommandEncoder> bl=[cb blitCommandEncoder];
                [bl copyFromTexture:canvas sourceSlice:0 sourceLevel:0 sourceOrigin:MTLOriginMake(0,0,0) sourceSize:MTLSizeMake(2*S,2*S,1) toBuffer:rb destinationOffset:0 destinationBytesPerRow:2*S*4 destinationBytesPerImage:4*S*S*4];
                [bl endEncoding];[cb commit];[cb waitUntilCompleted];
                const uint8_t *o=rb.contents;int bad=0;
                for (int y=4;y<2*S-4;y+=9) for (int x=4;x<2*S-4;x+=9) bad+=abs(o[4*(y*2*S+x)+1]-(int)tag)>3;
                stale+=bad!=0;
            }
            EXPECT("in-place presents go through MetalFX",ROUNDS,kinds);
            EXPECT("the scaler read the window texture in place: no input copy made",1,g_fx_present.in==nil&&g_fx_present.in_place_n>=ROUNDS);
            EXPECT("the scaler wrote the canvas: no present pass, no output texture made",1,g_fx_present.out==nil&&g_fx_present.direct_n>=ROUNDS);
            EXPECT("the scaler saw the whole frame still drawing at the present, rounds stale",0,stale);
            res_remove(win);res_remove(canvas);res_remove(args);
        }
        g_present_n=savedPresent;

        // Whole-window capture uses the same upscale and remains asynchronous; its completion includes the final BGRA readback.
        dispatch_semaphore_t grabbed=dispatch_semaphore_create(0);
        mtl_grab_next_present(^(CGImageRef img) {
            EXPECT("whole-window capture produces an image",1,img!=NULL);
            if (img) {
                EXPECT("whole-window image width",W,(int)CGImageGetWidth(img));
                CFDataRef data=CGDataProviderCopyData(CGImageGetDataProvider(img));const uint8_t *p=CFDataGetBytePtr(data);
                size_t off=96*CGImageGetBytesPerRow(img)+96*4;
                EXPECT("whole-window capture preserves sRGB bytes",1,abs(p[off]-200)<=3&&abs(p[off+1]-64)<=3&&abs(p[off+2]-128)<=3);
                CFRelease(data);
            }
            dispatch_semaphore_signal(grabbed);
        });
        grab_present(big,whole,CGSizeMake(W,H));
        EXPECT("whole-window capture callback completes",0,(int)dispatch_semaphore_wait(grabbed,dispatch_time(DISPATCH_TIME_NOW,10*NSEC_PER_SEC)));
        res_remove(big);g_fx_mode=0;
    } else printf("no Metal 4 MetalFX spatial scaler on this GPU: the MetalFX present is not checked\n");
#endif
    int invalid[4][4]={{-1,0,4,4},{0,-1,4,4},{0,0,0,4},{7,0,4,4}};
    for(int i=0;i<4;i++) {
        MTLTextureDescriptor *dd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:8 height:8 mipmapped:NO];dd.usage=MTLTextureUsageRenderTarget;dd.storageMode=MTLStorageModeShared;
        id<MTLTexture> dst=[g_dev newTextureWithDescriptor:dd];
        EXPECT("invalid crop is not encoded and does not fake success by stretching the whole image",0,ENCODE([g_q commandBuffer],src,dst,invalid[i]));
    }
    // Real QueuePresent entry: set crop and enqueue first, then immediately change the window; the present ticket must keep that frame's copy.
    PFNNVNDEVICEGETPROCADDRESSPROC get=(PFNNVNDEVICEGETPROCADDRESSPROC)h_nvnBootstrapLoader("nvnDeviceGetProcAddress");
    NVNT_nvnWindowSetCrop set=(NVNT_nvnWindowSetCrop)get(NULL,"nvnWindowSetCrop");
    NVNT_nvnQueuePresentTexture present=(NVNT_nvnQueuePresentTexture)get(NULL,"nvnQueuePresentTexture");
    NVNqueueBuilder builder={0}; NVNqueue queue={0};
    h_nvnQueueBuilderSetDefaults(&builder);
    EXPECT("fixture queue initialized",1,h_nvnQueueInitialize(&queue,&builder));
    uint64_t before, done;
    tex_t tex={.magic=MAGIC('T', 'E', 'X', 0), .serial =10001};tex.d.width=tex.d.height=8;tex.d.depth=tex.d.levels=1;tex.d.format=NVN_FORMAT_RGBA8;tex.d.target=NVN_TEXTURE_TARGET_2D;serial_put(tex.serial,src);
    win_t win={.ntex=1};win.tex[0]=(NVNtexture *)&tex;set((NVNwindow *)&win,4,4,4,4);
    present_progress(&before,&done);
    // Replay is held until the window changes: QueuePresent must take this frame's crop when it queues the frame, not when replay presents it
    nvn_queue_test_hold(1);
    present(&queue,(NVNwindow *)&win,0);set((NVNwindow *)&win,0,0,8,8);
    {   // without a working hold the crop check below would pass whenever replay ran first
        uint64_t held; usleep(20000); present_progress(&held,&done);
        EXPECT("the held replay queues no frame for the display",1,held==before);
    }
    nvn_queue_test_hold(0);
    uint64_t tk=display_ticket(win.ticket[0],before);
#ifdef CROP_LEGACY_HEADER
    EXPECT("old queue struct has no room for a four-value copy (struct check)",1,sizeof(pq_item)>=sizeof(tex_t)+sizeof(double)+sizeof(uint64_t)+sizeof br);
#else
    pthread_mutex_lock(&g_pq_mu);
    int saved=tk && memcmp(g_pq[(tk-1)%PQ].crop,br,sizeof br)==0;pthread_mutex_unlock(&g_pq_mu);
    EXPECT("real QueuePresent: the frame's crop copy does not change with the window",1,saved);
#endif
    h_nvnWindowFinalize((NVNwindow *)&win);
#ifndef CROP_LEGACY_HEADER
    {   // The crop only picks texels; the picture has the layer's shape (a 1200x960 crop of a 1920x1080 swapchain is shown at 16:9)
        const int W=64,H=36,TW=64,TH=36;MTLTextureDescriptor *td=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:TW height:TH mipmapped:NO];td.storageMode=MTLStorageModeShared;td.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> sw=[g_dev newTextureWithDescriptor:td];uint8_t *sp=malloc(TW*TH*4);
        const int crop54[4]={0,0,40,32};   // 5:4 in the top-left corner: red inside, green outside
        for(int y=0;y<TH;y++)for(int x=0;x<TW;x++)memcpy(sp+4*(y*TW+x),colors[x>=crop54[2]||y>=crop54[3]],4);
        [sw replaceRegion:MTLRegionMake2D(0,0,TW,TH) mipmapLevel:0 withBytes:sp bytesPerRow:TW*4];free(sp);res_add(sw);res_commit();
        g_present_shape[0]=1920;g_present_shape[1]=1080;   // the display's shape, as the host has it
        for(int m4=0;m4<2;m4++) {
            int outH[2]={H,40};   // 16:9 fills; a 16:10 canvas keeps the bars above and below
            for(int k=0;k<2;k++) {
                MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:W height:outH[k] mipmapped:NO];od.storageMode=MTLStorageModeShared;od.usage=MTLTextureUsageRenderTarget;
                id<MTLTexture> ot=[g_dev newTextureWithDescriptor:od];res_add(ot);res_commit();
                if(m4){EXPECT("Metal 4 present of a 5:4 crop",1,present_offscreen_wait(sw,crop54,ot)!=PRESENT_FAILED);}
                else{id<MTLCommandBuffer> cb=[g_q commandBuffer];EXPECT("classic present of a 5:4 crop",1,present_encode(cb,sw,ot,crop54));[cb commit];[cb waitUntilCompleted];}
                uint8_t *o=malloc(W*outH[k]*4);[ot getBytes:o bytesPerRow:W*4 fromRegion:MTLRegionMake2D(0,0,W,outH[k]) mipmapLevel:0];
                int bad=0,top=(outH[k]-H)/2;static const uint8_t black[4]={0,0,0,255};
                for(int y=0;y<outH[k];y++)for(int x=0;x<W;x++)bad+=memcmp(o+4*(y*W+x),y<top||y>=top+H?black:colors[0],4)!=0;
                EXPECT(k?"5:4 crop on a 16:10 canvas: 16:9 of red, black above and below":"5:4 crop on a 16:9 canvas: all red, no side bars",0,bad);free(o);res_remove(ot);
            }
        }
        res_remove(sw);
    }
    {   // Output alpha is 1 (255) for a swapchain of alpha 0, as a non-opaque layer needs: bilinear and tent paths, classic and Metal 4
        g_present_shape[0]=g_present_shape[1]=1;   // square fixtures: the picture fills the output, no bars
        MTLTextureDescriptor *sd=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:16 height:16 mipmapped:NO];
        sd.storageMode=MTLStorageModeShared; sd.usage=MTLTextureUsageShaderRead;
        id<MTLTexture> zsrc=[g_dev newTextureWithDescriptor:sd];
        uint8_t sp[16*16*4];
        for(int i=0;i<16*16;i++){sp[4*i+0]=200;sp[4*i+1]=100;sp[4*i+2]=50;sp[4*i+3]=0;}
        [zsrc replaceRegion:MTLRegionMake2D(0,0,16,16) mipmapLevel:0 withBytes:sp bytesPerRow:16*4];
        res_add(zsrc); res_commit();
        const int sizes[2]={16,8};   // 16: bilinear (step 1.0), 8: minified tent (step 2.0)
        for(int k=0;k<2;k++) for(int m4=0;m4<2;m4++) {
            int ow=sizes[k];
            MTLTextureDescriptor *od=[MTLTextureDescriptor texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm width:ow height:ow mipmapped:NO];
            od.storageMode=MTLStorageModeShared; od.usage=MTLTextureUsageRenderTarget;
            id<MTLTexture> ot=[g_dev newTextureWithDescriptor:od]; res_add(ot); res_commit();
            if(m4) EXPECT(k?"Metal 4 minified zero-alpha present":"Metal 4 bilinear zero-alpha present",PRESENT_STANDARD,present_offscreen_wait(zsrc,NULL,ot));
            else { id<MTLCommandBuffer> cb=[g_q commandBuffer]; EXPECT(k?"classic minified zero-alpha present":"classic bilinear zero-alpha present",1,present_encode(cb,zsrc,ot,NULL)); [cb commit]; [cb waitUntilCompleted]; }
            uint8_t o[16*16*4]; [ot getBytes:o bytesPerRow:ow*4 fromRegion:MTLRegionMake2D(0,0,ow,ow) mipmapLevel:0];
            int bad=0;
            for(int i=0;i<ow*ow;i++) bad += abs((int)o[4*i+0]-200)>1 || abs((int)o[4*i+1]-100)>1 || abs((int)o[4*i+2]-50)>1 || o[4*i+3]!=255;
            EXPECT(k?(m4?"Metal 4 minified zero-alpha output is opaque":"classic minified zero-alpha output is opaque"):(m4?"Metal 4 bilinear zero-alpha output is opaque":"classic bilinear zero-alpha output is opaque"),0,bad);
            res_remove(ot);
        }
        g_present_shape[0]=1920;g_present_shape[1]=1080;
        res_remove(zsrc);
    }
#endif
    h_nvnQueueFinalize(&queue);
    printf("== %d checks, %d failed ==\n",cases,failed);return failed!=0;
} }
