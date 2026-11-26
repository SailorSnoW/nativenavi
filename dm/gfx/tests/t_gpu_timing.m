// Completion times that are missing, unordered, not finite or in the future (a faulted command buffer's) add nothing to a frame's GPU time or span, and still retire it.
// No GPU: the production helpers are called directly
#include "../nvn/nvn_mtl.m"
void *front_metal_layer(void) { return NULL; }
module_t *module_of(const void *p) { (void)p; return NULL; }
module_t *g_mod; int g_nmod;
void svc_thread_dump(void) {}
uint64_t now_ns(void) { return bp_now(); }
static int checks, failed;
#define CHECK(c, label) do { checks++; if (!(c)) { failed++; fprintf(stderr, "FAIL: %s\n", label); } } while (0)

int main(void) { @autoreleasepool {
    double now = CACurrentMediaTime(), start = now - 2, end = now - 1;
    uint64_t frame = 1000, t0 = 0, t1 = 0;
    CHECK(gpu_time_valid(start, end) == 1, "elapsed ordered times are valid");
    const double bad[][2] = {
        {0, end}, {-1, end}, {end, start}, {end, end},
        {NAN, end}, {start, NAN}, {INFINITY, INFINITY}, {start, INFINITY},
        {start, now + 3600}, {start, start + 6148483371.4372},   // the last: an unwritten UINT64_MAX tick end (issue #1737)
    };
    for (unsigned i = 0; i < sizeof bad / sizeof bad[0]; i++, frame++) {
        CHECK(!gpu_time_valid(bad[i][0], bad[i][1]), "invalid times rejected");
        gpu_tag_begin(frame);
        CHECK(mtl_gpu_ms(frame) == -1, "a pending completion leaves the frame unknown");
        gpu_tag_done(frame, bad[i][0], bad[i][1]);
        CHECK(mtl_gpu_ms(frame) == 0, "invalid times add no duration");
        CHECK(!mtl_gpu_span(frame, &t0, &t1), "invalid times add no span");
        CHECK(atomic_load(&g_gpu[frame % GRING].pending) == 0, "an invalid completion still retires");
    }
    gpu_tag_begin(frame); gpu_tag_begin(frame); gpu_tag_begin(frame);
    gpu_tag_done(frame, start, end);
    gpu_tag_done(frame, start, now + 3600);
    CHECK(mtl_gpu_ms(frame) == -1, "an invalid completion retires no other work");
    gpu_tag_done(frame, start - 1, start);
    CHECK(fabs(mtl_gpu_ms(frame) - 2000) < 0.001, "a mixed frame keeps only the valid work");
    CHECK(mtl_gpu_span(frame, &t0, &t1), "the valid work keeps its span");
    CHECK(t0 == (uint64_t)((start - 1) * 1e9) && t1 == (uint64_t)(end * 1e9), "the span leaves the invalid end out");
    frame++;
    gpu_tag_begin(frame); gpu_tag_begin(frame + GRING);
    gpu_tag_done(frame, start, end);
    CHECK(mtl_gpu_ms(frame + GRING) == -1, "a late completion does not retire a reused ring slot");
    gpu_tag_done(frame + GRING, start, end);
    CHECK(fabs(mtl_gpu_ms(frame + GRING) - 1000) < 0.001, "the reused slot keeps its own duration");
    printf("t_gpu_timing: %d checks, %d failed\n", checks, failed);
    return failed != 0;
} }
