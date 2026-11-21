// Public command recording and real queue/program ownership: the graphics and compute program bindings, a queue's own copies of them, finalization, and the push constant scopes.
// A program bind NVN2 would take as a ray tracing program (no stages, mask 0x100) binds nothing: the host has no ray tracing
#define nvn_game_fps nvn_game_fps_mtl_default
#include "../nvn/nvn_mtl.m"
#undef nvn_game_fps
#define g_ft g_ft_hle
#include "../nvn/hle_nvn.c"
#undef g_ft

void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks,failed;
static _Atomic int markers_released;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr,"FAIL: %s\n",label); } } while (0)
@interface ProgStateMarker : NSObject @end
@implementation ProgStateMarker
- (void)dealloc { atomic_fetch_add(&markers_released,1); }
@end
static NVNcommandBuffer commands;

static const void *marker_key=&marker_key;
static prog_t program(int marked) {   // marked: an object that counts its release rides on the program's host object
    prog_t p={.magic=MAGIC('P','R','O','G'),.nstage=1,.serial=nvn_new_serial()};
    NvProg *object=[NvProg new];
    if(marked) objc_setAssociatedObject(object,marker_key,[ProgStateMarker new],OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    serial_put(p.serial,object); return p;
}
static unsigned apply(NVNcommandHandle handle) {
    size_t length; uint8_t *bytes=nvn_rec_linear(handle,&length); unsigned records=0;
    for(size_t off=0;off<length;) {
        op_hdr h;memcpy(&h,bytes+off,sizeof h); if(h.op==OP_END)break;
        CHECK(h.size>=8&&h.size<=length-off,"expanded record is bounded");
        if(h.op==OP_PUSH_CONST) {
            const p_push_const *p=(const void *)(bytes+off+8);
            CHECK(p->offset<=sizeof S.push[0]&&p->size<=sizeof S.push[0]-p->offset,"push record is within its eight-bank group");
        }
        CHECK(state_op(h.op,bytes+off+8),"record replays through production state_op");
        off+=h.size;records++;
    }
    free(bytes);return records;
}
static void bind(const prog_t *p,int mask) {
    h_nvnCommandBufferBeginRecording(&commands);
    h_nvnCommandBufferBindProgram(&commands,(const NVNprogram *)p,mask);
    CHECK(apply(h_nvnCommandBufferEndRecording(&commands))==1,"public program bind records exactly once");
}
static void binding(void) {
    mtl_queue_activate(780101);
    prog_t graphics=program(0),compute=program(0),bare=program(0);bare.nstage=0;
    bind(&graphics,3);bind(&compute,0x20);
    CHECK(S.prog==&graphics&&S.cprog==&compute,"graphics and compute have separate bindings");
    for(unsigned i=0;i<NVN_RESOURCE_STAGES;i++)g_args_wrote[i]=g_args_seq;
    uint64_t graphics_gen=g_args_gen[0],compute_gen=g_args_gen[5];
    bind(&bare,0x100);bind(NULL,0x100);bind(&bare,0);
    CHECK(S.prog==&graphics&&S.cprog==&compute,"a bind with only NVN2's ray tracing bit, or no bit, changes neither binding");
    CHECK(g_args_gen[0]==graphics_gen&&g_args_gen[5]==compute_gen,"and leaves the argument generations alone");
    bind(&graphics,3);bind(&compute,0x20);
    CHECK(g_args_gen[0]==graphics_gen&&g_args_gen[5]==compute_gen,"binding the same programs again dirties nothing");
    mtl_queue_forget(780101);
    for(unsigned i=0;i<3;i++)mtl_prog_release(&(prog_t[]){graphics,compute,bare}[i]);
}
static void ownership(void) {
    mtl_queue_activate(780102);
    prog_t p[6];uint64_t serial[6];
    for(int i=0;i<6;i++) {p[i]=program(i>=4);serial[i]=p[i].serial;}
    for(int i=0;i<5;i++)S.bst[i]=&p[i];S.cprog=&p[5];
    queue_program_preserve();queue_state *first=g_active_queue;
    CHECK(S.cprog!=&p[5]&&S.cprog->serial==serial[5],"queue owns an immutable compute descriptor after replay");
    CHECK(S.bst[4]!=&p[4]&&S.bst[4]->serial==serial[4],"and one of each graphics stage's program");
    for(int i=0;i<6;i++) {
        CHECK(first->programs[i].serial==serial[i],"all six distinct bound program serials are preserved");
        mtl_prog_release(&p[i]);memset(&p[i],0xa5,sizeof p[i]);
        CHECK(serial_get(serial[i])!=nil,"queue pin survives guest finalization and descriptor reuse");
    }
    prog_t second=*S.cprog;
    queue_program_preserve(); // Already-owned descriptors must not add pins.
    mtl_queue_activate(780103);
    CHECK(!S.cprog&&!S.prog,"new queue starts with no compute or raster program");
    bind(&second,0x20);queue_program_preserve();
    CHECK(S.cprog!=&second&&S.cprog->serial==serial[5],"second queue independently owns the same compute serial");
    mtl_queue_activate(780102);
    CHECK(S.cprog->serial==serial[5]&&S.bst[4]->serial==serial[4],"queue restore keeps its bindings");
    mtl_queue_forget(780102);
    for(int i=0;i<5;i++)CHECK(serial_get(serial[i])==nil,"queue destruction releases every retired raster pin");
    CHECK(serial_get(serial[5])!=nil,"other queue still pins the retired compute program");
    mtl_queue_activate(780103);
    CHECK(S.cprog&&S.cprog->serial==serial[5],"remaining queue's snapshot survives original descriptor reuse");
    mtl_queue_forget(780103);
    CHECK(!S.cprog&&serial_get(serial[5])==nil,"last queue destruction releases the retired compute program");
}
static void forgotten(void) {
    prog_t extra=program(0),graphics=program(0),compute=program(0);
    mtl_queue_activate(780104);bind(&graphics,3);bind(&extra,0x20);
    mtl_queue_activate(780105);bind(&graphics,3);bind(&extra,0x20);
    h_nvnProgramFinalize((NVNprogram *)&extra);
    CHECK(!extra.magic,"public finalization invalidates the guest descriptor");
    CHECK(!S.cprog&&S.prog==&graphics,"finalize forgets the active queue's binding of it without changing the other scope");
    mtl_queue_activate(780104);
    CHECK(!S.cprog&&S.prog==&graphics,"finalize also forgets inactive queues' raw binding");
    bind(&compute,0x20);CHECK(S.cprog==&compute,"and the scope binds again");
    mtl_queue_forget(780104);mtl_queue_forget(780105);
    mtl_prog_release(&graphics);mtl_prog_release(&compute);
}
static void pushes(int compact) {
    g_rec_compact=compact;mtl_queue_activate(780106);
    uint8_t expected[2][8*256]={0},data[256];
    CHECK(DMSH_PUSH_BLOCKS==8&&sizeof S.push==sizeof expected,"both push scopes own exactly eight 256-byte banks");
    h_nvnCommandBufferBeginRecording(&commands);
    for(int scope=0;scope<2;scope++)for(int block=0;block<8;block++) {
        for(int byte=0;byte<256;byte++)data[byte]=(uint8_t)(scope*91+block*17+byte);
        memcpy(expected[scope]+block*256,data,256);
        if(scope)h_nvnCommandBufferPushComputeConstants(&commands,block,0,256,data);
        else h_nvnCommandBufferPushGraphicsConstants(&commands,block,0,256,data);
        memset(data,0xed,sizeof data); // The immediate API owns its recorded bytes.
    }
    CHECK(apply(h_nvnCommandBufferEndRecording(&commands))==16,"all eight blocks in both scopes record");
    CHECK(!memcmp(S.push,expected,sizeof expected),"full eight-bank patterns replay without merging graphics and compute");
    h_nvnCommandBufferBeginRecording(&commands);
    for(int scope=0;scope<2;scope++) {
        void (*push)(NVNcommandBuffer *,uint64_t,uint64_t,uint64_t,const void *)=scope?h_nvnCommandBufferPushComputeConstants:h_nvnCommandBufferPushGraphicsConstants;
        push(&commands,8,0,4,data);push(&commands,UINT64_MAX,0,4,data);
        push(&commands,0,UINT64_MAX,1,data);push(&commands,0,1,UINT64_MAX,data);
        push(&commands,0,0,257,data);push(&commands,0,255,2,data);
        push(&commands,0,257,0,data);push(&commands,0,0,4,NULL);
    }
    CHECK(apply(h_nvnCommandBufferEndRecording(&commands))==0,"invalid banks, overflowing ranges and null data never record");
    CHECK(!memcmp(S.push,expected,sizeof expected),"rejected push ranges preserve both scopes");
    mtl_queue_activate(780107);
    uint8_t zero[sizeof S.push]={0};CHECK(!memcmp(S.push,zero,sizeof zero),"new queue has its own empty push scopes");
    uint32_t graphics=0x12345678,compute=0xfedcba98;
    h_nvnCommandBufferBeginRecording(&commands);
    h_nvnCommandBufferPushGraphicsConstants(&commands,7,252,4,&graphics);
    h_nvnCommandBufferPushComputeConstants(&commands,7,252,4,&compute);
    apply(h_nvnCommandBufferEndRecording(&commands));
    memcpy(zero+7*256+252,&graphics,4);memcpy(zero+8*256+7*256+252,&compute,4);
    CHECK(!memcmp(S.push,zero,sizeof zero),"last word of bank7 is valid and separate in each scope");
    mtl_queue_activate(780106);CHECK(!memcmp(S.push,expected,sizeof expected),"queue restore preserves every push bank");
    mtl_queue_activate(780107);CHECK(!memcmp(S.push,zero,sizeof zero),"second queue restores its own bank7 values");
    mtl_queue_forget(780106);mtl_queue_forget(780107);
}
int main(void) { @autoreleasepool {
    mtl_init();CHECK(h_nvnCommandBufferInitialize(&commands,NULL),"command buffer initializes");
    @autoreleasepool {binding();ownership();forgotten();pushes(0);pushes(1);}
    // prog_get keeps its last lookup strongly cached independently of serial
    // ownership. Replace that cache with an ordinary program before observing
    // marker destruction, so this checks queue/owner references alone.
    @autoreleasepool {prog_t plain=program(0);prog_get(&plain);mtl_prog_release(&plain);}
    // Production retirement also holds objects until a real submission fence.
    // No draw or dispatch is needed to complete that ownership path.
    mtl_sub_begin();uint64_t fence=mtl_sub_commit();
    CHECK(mtl_wait(fence,5000000000ull)&&!mtl_last_commit_error(),"retirement fence completes");mtl_drain();
    CHECK(atomic_load(&markers_released)==2,"marked program objects die after the last owner releases their serial");
    h_nvnCommandBufferFinalize(&commands);
    printf("t_nvn_prog_state: %d checks, %d failed\n",checks,failed);return failed!=0;
} }
