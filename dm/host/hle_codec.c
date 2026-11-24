// nn::codec's hardware Opus decoder on the sdk's bundled libopus (no hwopus service); state in a host block per decoder (objmap).
#include "hle.h"

#define SOFT_OPUS_STATE 0x200   // bytes; larger than the SDK's OpusDecoderType
static void *opus_fn(const char *name) {
    void *f = guest_sym(name);
    if (!f) die("sdk lacks %s", name);
    return f;
}
static void *opus_soft(const void *hw) {
    void *s = objmap_get(hw);
    if (!s) die("Opus decoder %p was not initialized by the host", hw);
    return s;
}
HLE_FORCE("_ZN2nn5codec36GetHardwareOpusDecoderWorkBufferSizeEiii", "hwopus service; work buffer computed for the software decoder", size_t, h_codec_HwOpusWorkSize, int rate, int ch, int option) {
    (void)option;
    size_t (*f)(int, int) = opus_fn("_ZN2nn5codec28GetOpusDecoderWorkBufferSizeEii");
    return f(rate, ch);
}
HLE_FORCE("_ZN2nn5codec29InitializeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeEiiPvmi", "hwopus service; replaced by the software OpusDecoder", int, h_codec_HwOpusInit,
          void *hw, int rate, int ch, void *buf, size_t size, int option) {
    (void)option;
    int (*f)(void *, int, int, void *, size_t) = opus_fn("_ZN2nn5codec21InitializeOpusDecoderEPNS0_15OpusDecoderTypeEiiPvm");
    void *s = objmap_get(hw);
    if (!s) {
        s = calloc(1, SOFT_OPUS_STATE);
        objmap_put(hw, s);
    }
    return f(s, rate, ch, buf, size);
}
HLE_FORCE("_ZN2nn5codec27FinalizeHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusFini, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec19FinalizeOpusDecoderEPNS0_15OpusDecoderTypeE");
    void *s = objmap_del(hw);
    if (s) {
        f(s);
        free(s);
    }
}
HLE_FORCE("_ZN2nn5codec32GetHardwareOpusDecoderSampleRateEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusRate, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec24GetOpusDecoderSampleRateEPKNS0_15OpusDecoderTypeE");
    return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec34GetHardwareOpusDecoderChannelCountEPKNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", int, h_codec_HwOpusCh, const void *hw) {
    int (*f)(const void *) = opus_fn("_ZN2nn5codec26GetOpusDecoderChannelCountEPKNS0_15OpusDecoderTypeE");
    return f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec24ResetHardwareOpusDecoderEPNS0_23HardwareOpusDecoderTypeE", "hwopus service; software OpusDecoder", void, h_codec_HwOpusReset, void *hw) {
    void (*f)(void *) = opus_fn("_ZN2nn5codec16ResetOpusDecoderEPNS0_15OpusDecoderTypeE");
    f(opus_soft(hw));
}
HLE_FORCE("_ZN2nn5codec33DecodeOpusInterleavedWithHardwareEPNS0_23HardwareOpusDecoderTypeEPmPiPsmPKvm", "hwopus service; software OpusDecoder", int, h_codec_HwOpusDecode,
          void *hw, size_t *consumed, int *samples, int16_t *out, size_t out_size, const void *in, size_t in_size) {
    int (*f)(void *, size_t *, int *, int16_t *, size_t, const void *, size_t) = opus_fn("_ZN2nn5codec21DecodeOpusInterleavedEPNS0_15OpusDecoderTypeEPmPiPsmPKvm");
    return f(opus_soft(hw), consumed, samples, out, out_size, in, in_size);
}

// The game names this one call of SDK 21's async interface and none that makes a manager
HLE_FORCE("_ZN2nn5codec46GetHardwareOpusDecoderManagerFreeIoBufferCountEPKNS0_30HardwareOpusDecoderManagerTypeE", "hwopus service; refused: the host has no decoder manager", int, h_codec_HwOpusMgrFreeIo, const void *gm) {
    LOG("[RED] codec: GetHardwareOpusDecoderManagerFreeIoBufferCount(%p): the host has no hardware Opus decoder manager, no free I/O buffer", gm);
    return 0;
}
