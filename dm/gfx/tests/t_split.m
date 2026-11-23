// A compute shader in two passes (dmsh_ndsr.py's split, docs/reference/shaders.md "agl's NDSR network on simdgroup matrices, in two passes"): dispatch runs pass 0 (DMSH_SPLIT_FC false),
// a barrier, then pass 1 (true) on the same grid, the words between them in frame scratch at DMSH_SPLIT_SLOT (nvn_mtl.m split_pipes, split_dispatch). The kernel's pass 0 writes WORDS
// words a thread; its pass 1 reads those of a thread GROUPS / 2 + 1 workgroups away, which a pass 1 that started before pass 0 had finished would find unwritten, and marks the output 2;
// the one pass (the constant undefined) computes the same value and marks it 1. Direct dispatches take two passes, an indirect one and DM_NVN_SPLIT=0 (--one) take one
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

enum { GROUPS = 512, THREADS = 128, WORDS = 8, N = GROUPS * THREADS };

static uint32_t value(uint32_t i) { return i * 2654435761u ^ 0x5a5a5a5au; }   // the kernel's own: what pass 0 writes for thread i, and the one pass computes

int main(int argc, char **argv) { @autoreleasepool {
    const int one = argc > 1 && !strcmp(argv[1], "--one");   // run with DM_NVN_SPLIT=0: every dispatch in one pass
    mtl_init();
    NSError *error = nil;
    NSString *src = [NSString stringWithFormat:@"#include <metal_stdlib>\nusing namespace metal;\n"
        "constant bool p1 [[function_constant(%d)]];\nconstant bool split = is_function_constant_defined(p1);\n"
        "struct Args { device uint *out [[id(0)]]; };\n"
        "static uint value(uint i) { return i * 2654435761u ^ 0x5a5a5a5au; }\n"
        "kernel void k(constant Args &a [[buffer(0)]], device uint *w [[buffer(%d), function_constant(split)]], uint3 g3 [[threadgroup_position_in_grid]], uint3 n3 [[threadgroups_per_grid]],\n"
        "              uint t [[thread_index_in_threadgroup]]) {\n"
        " const uint i = ((g3.z * n3.y + g3.y) * n3.x + g3.x) * %du + t, n = n3.x * n3.y * n3.z * %du, j = (i + (n3.x / 2u + 1u) * %du) %% n;\n"
        " if (split && !p1) { for (uint k = 0; k < %du; k++) w[i * %du + k] = value(i) + k; return; }\n"
        " uint s = 0; for (uint k = 0; k < %du; k++) s += split ? w[j * %du + k] : value(j) + k;\n"
        " a.out[2 * i] = s; a.out[2 * i + 1] = split ? 2u : 1u; }\n",
        DMSH_SPLIT_FC, DMSH_SPLIT_SLOT, THREADS, THREADS, THREADS, WORDS, WORDS, WORDS, WORDS];
    id<MTLLibrary> lib = [g_dev newLibraryWithSource:src options:nil error:&error];
    CHECK(lib != nil, "split kernel compiles"); if (!lib) { fprintf(stderr, "%s\n", error.localizedDescription.UTF8String); return 1; }
    prog_t pg = {.serial = 961101}; NvProg *program = [NvProg new]; NvShader *cs = [NvShader new];
    program->sh[5] = cs; cs->stage = 5; cs->fn = fn_spec(lib, @"k", [MTLFunctionConstantValues new], &error);   // as shader_load specializes a function with constants
    cs->ae = [cs->fn newArgumentEncoderWithBufferIndex:0];
    cs->rf.tg[0] = THREADS; cs->rf.tg[1] = cs->rf.tg[2] = 1;
    cs->rf.nb = 1; cs->rf.b[0] = (nvn_rbind){.kind = NVN_RB_SBUFP, .rw = 1, .reg = 0, .aid = 0};
    cs->rf.split_words = WORDS;
    serial_put(pg.serial, program);
    const size_t out_bytes = (size_t)N * 8, pool = out_bytes + 0x4000;
    uint8_t *mem = NULL; if (posix_memalign((void **)&mem, getpagesize(), pool)) return 1;
    memset(mem, 0, pool); mtl_pool_add(mem, pool);
    uint32_t *args = (uint32_t *)(mem + out_bytes);
    for (int indirect = 0; indirect < 2; indirect++) {
        for (int run = 0; run < 3; run++) {
            mtl_sub_begin(); state_reset(); memset(&R, 0, sizeof R);
            S.cprog = &pg; S.ssbo[5][0] = (p_bind_buf){.addr = (uint64_t)mem, .size = out_bytes};
            mtl_sub_fill((uint64_t)mem, out_bytes, 0xffffffffu);
            p_dispatch dp = {.x = GROUPS, .y = 1, .z = 1};
            if (indirect) { args[0] = GROUPS; args[1] = args[2] = 1; dp = (p_dispatch){.x = GROUPS, .y = 1, .z = 1, .ind = (uint64_t)args}; }
            dispatch(&dp);
            uint64_t fence = mtl_sub_commit(); CHECK(mtl_wait(fence, 5000000000ull), "dispatch completes"); mtl_drain();
            CHECK(!mtl_last_commit_error(), "no GPU error");
            const uint32_t *out = (const uint32_t *)mem; long wrong = 0, passes[3] = {0, 0, 0};
            for (uint32_t i = 0; i < N; i++) {
                uint32_t j = (i + (GROUPS / 2 + 1) * THREADS) % N, s = 0;
                for (uint32_t k = 0; k < WORDS; k++) s += value(j) + k;
                wrong += out[2 * i] != s; passes[out[2 * i + 1] < 3 ? out[2 * i + 1] : 0]++;
            }
            const int want = one || indirect ? 1 : 2;
            if (wrong || passes[want] != N) fprintf(stderr, "%s run %d: %ld values wrong, %ld threads in one pass, %ld in two, %ld neither\n", indirect ? "indirect" : "direct", run, wrong, passes[1], passes[2], passes[0]);
            CHECK(!wrong, "pass 1 reads every word pass 0 wrote, of other workgroups too");
            CHECK(passes[want] == N, one ? "DM_NVN_SPLIT=0: one pass" : indirect ? "an indirect dispatch: one pass" : "a direct dispatch: two passes");
        }
    }
    serial_del(pg.serial);
    printf("t_split%s: %d checks, %d failed\n", one ? " (DM_NVN_SPLIT=0)" : "", checks, failed); return failed != 0;
} }
