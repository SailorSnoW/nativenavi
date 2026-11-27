// Hardware Opus forwarding (hle_codec.c) without a guest: a decoder initialized through the C interface keeps the software decoder's state in a host block (objmap), every call
// goes to the sdk's software function with that block, and a decoder the host did not initialize stops the host. The manager call the game's modules name is refused.
#include "../hle.h"
#undef HLE_FORCE
#define HLE_FORCE(mangled, why, ret, cname, ...) static ret cname(__VA_ARGS__)
#include <assert.h>
#include <setjmp.h>

static jmp_buf g_die_jmp;
static int g_die_catch, g_died;
#undef die
#define die(...) do { if (g_die_catch) { g_died = 1; longjmp(g_die_jmp, 1); } abort(); } while (0)

#include "../hle_codec.c"

static struct { const void *k; void *v; } g_om[8];
void *objmap_get(const void *k) { for (int i = 0; i < 8; i++) if (g_om[i].k == k) return g_om[i].v; return NULL; }
void objmap_put(const void *k, void *v) { for (int i = 0; i < 8; i++) if (!g_om[i].k) { g_om[i].k = k; g_om[i].v = v; return; } abort(); }
void *objmap_del(const void *k) { for (int i = 0; i < 8; i++) if (g_om[i].k == k) { void *v = g_om[i].v; g_om[i].k = g_om[i].v = NULL; return v; } return NULL; }

static void *g_last;
static int g_inits, g_finis, g_decodes, g_resets;
static size_t fake_size(int rate, int ch) { return (size_t)rate / 100 * (size_t)ch; }
static int fake_init(void *s, int rate, int ch, void *buf, size_t size) {
    (void)buf; (void)size;
    g_last = s; ((int *)s)[0] = rate; ((int *)s)[1] = ch;
    g_inits++;
    return 0;
}
void objmap_put_d(const void *k, void *v, void (*d)(void *)) { (void)d; objmap_put(k, v); }

static void fake_fini(void *s) { g_last = s; g_finis++; }
static void fake_reset(void *s) { g_last = s; g_resets++; }
static int fake_rate(const void *s) { return ((const int *)s)[0]; }
static int fake_ch(const void *s) { return ((const int *)s)[1]; }
static int fake_decode(void *s, size_t *c, int *smp, int16_t *o, size_t osz, const void *i, size_t isz) {
    (void)o; (void)osz; (void)i; (void)isz; g_last = s; *c = 12; *smp = 960; g_decodes++; return 0;
}
void *guest_sym(const char *n) {
    static const struct { const char *sym; void *fn; } tab[] = {
        {"_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii", fake_size},
        {"_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm", fake_init},
        {"_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE", fake_fini},
        {"_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE", fake_rate},
        {"_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE", fake_ch},
        {"_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE", fake_reset},
        {"_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm", fake_decode},
    };
    for (size_t i = 0; i < sizeof tab / sizeof tab[0]; i++) if (!strcmp(n, tab[i].sym)) return tab[i].fn;
    return NULL;
}

static int must_die(void *hw) {
    size_t c = 0; int smp = 0; int16_t out[4]; uint8_t in[4] = {0};
    g_died = 0; g_die_catch = 1;
    if (!setjmp(g_die_jmp)) (void)h_codec_HwOpusDecode(hw, &c, &smp, out, sizeof out, in, sizeof in);
    g_die_catch = 0;
    return g_died;
}

int main(void) {
    size_t c = 0; int smp = 0; int16_t out[4]; uint8_t in[4] = {0};
    uint64_t c_obj[8] = {0}, other[8] = {0};

    assert(h_codec_HwOpusWorkSize(48000, 2, 0) == 960);   // the software decoder's size; the option has no software counterpart
    assert(h_codec_HwOpusInit(c_obj, 48000, 2, in, sizeof in, 0) == 0 && g_inits == 1 && g_last != c_obj && g_last == objmap_get(c_obj));
    assert(h_codec_HwOpusRate(c_obj) == 48000 && h_codec_HwOpusCh(c_obj) == 2);
    assert(h_codec_HwOpusDecode(c_obj, &c, &smp, out, sizeof out, in, sizeof in) == 0 && g_decodes == 1 && g_last == objmap_get(c_obj) && c == 12 && smp == 960);
    void *state = objmap_get(c_obj);
    assert(h_codec_HwOpusInit(c_obj, 24000, 1, in, sizeof in, 0) == 0 && g_inits == 2 && objmap_get(c_obj) == state && h_codec_HwOpusRate(c_obj) == 24000);   // initialized again: the same host block
    g_last = NULL; h_codec_HwOpusReset(c_obj); assert(g_resets == 1 && g_last == state);
    h_codec_HwOpusFini(c_obj); assert(g_finis == 1 && g_last == state && objmap_get(c_obj) == NULL);
    h_codec_HwOpusFini(c_obj); assert(g_finis == 1);   // finalized twice, or never initialized: nothing to free

    // A decoder the host did not initialize, or one it has finalized, has no software state to decode with
    assert(must_die(other) && must_die(c_obj));

    // The manager's free I/O buffer count: the host has no manager, whatever the game's object holds
    uint64_t mgr[3] = {0}; assert(h_codec_HwOpusMgrFreeIo(mgr) == 0);
    mgr[0] = (uint64_t)(uintptr_t)other; assert(h_codec_HwOpusMgrFreeIo(mgr) == 0);

    puts("t_codec_opus: all checks passed");
    return 0;
}
