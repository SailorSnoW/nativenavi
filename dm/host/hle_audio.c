// The host's PCM outputs in nn::audio AudioOut's shape: the in-process renderer, SDK 21's OutputStream and the guest's AudioOut.
// A CoreAudio AudioUnit with DM_AUDIO=1, else a silent sink that consumes in real time (the same pacing).
#include "hle.h"
#include "audio_input.h"
#include <AudioToolbox/AudioToolbox.h>
#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <unistd.h>

enum { FMT_PCM16 = 2, STATE_STARTED = 0, STATE_STOPPED = 1, RATE = 48000 };
enum { BK_NONE, BK_AU, BK_NULL };
enum { AU_IDLE, AU_STARTING, AU_CANCELED, AU_WAIT_CALLBACK };

#define QMAX 64
#define RELMAX QMAX                    // the game must take a buffer back before it appends again
typedef struct { uint64_t calls, count, qsum, underruns, zero_bytes; uint32_t fmin, fmax; } audio_stats;
typedef struct {
    pthread_mutex_t m;
    void *event;
    int ch;
    int sink_pcm;                      // six channels plus their pre-clamped stereo fold
    int dev_ch;                        // six-channel outputs follow the route, others keep their count
    _Atomic int state;
    aout_buf *q[QMAX]; _Atomic uint32_t q_head, q_tail;       // the game side writes, the output side reads
    aout_buf *rel[RELMAX]; _Atomic uint32_t rel_head, rel_tail; // the output side writes, the game side reads
    uint64_t pos;
    AudioUnit au;
    int start_phase, reset_pending, mute;   // reset_pending: replaced once the in-flight start returns
    int unit_lost;                          // no new unit could be made: the next route change retries
    int closed;                             // Close returned during a start: that start's worker frees the output
    _Atomic int hold_device;               // callbacks output silence until the silent sink has joined
    int backend;                       // changed only under g_aos_lock; act on it rather than re-deriving it
    pthread_t null_th; _Atomic int null_run;
    dispatch_semaphore_t notify_sem;
    pthread_t notify_th; _Atomic int notify_run, notify_pending, notify_armed;
    dispatch_semaphore_t rel_sem;      // host outputs only: the renderer blocks on it instead of polling
    int id;                            // #N in the log
    void *key;
    int nlog;
    _Atomic uint64_t under_n, under_b; // empty-queue callbacks / zero-filled bytes
    uint64_t st_calls, st_count, st_qsum, st_u0, st_ub0; uint32_t st_fmin, st_fmax;   // callback-owned
    audio_stats st_report; _Atomic int st_pending; // consumed by the watchdog or Close under g_aos_lock
    FILE *dump; int dump_tried, dump_ch;           // DM_AUDIO_DUMP: this output's file, opened at its first accepted block (audio_dump)
    _Atomic uint64_t cb_n;
    uint64_t wd_n, wd_t;               // under g_aos_lock
    int stalled, retry_ms; uint64_t retry_at;   // the silent sink stands in; the AudioUnit is retried at retry_at, backing off
} aout;

// g_aos_lock serializes every backend change with the started state: a suspend between them would start a second
// consumer on the single-consumer queues
#define AOMAX 8
static aout *g_aos[AOMAX];
static pthread_mutex_t g_aos_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_suspended;
static aout *ao(const void *g) {
    aout *a = objmap_get(g);
    if (!a) die("AudioOut %p was not created by the host", g);
    return a;
}
static const char *site(void *lr, char *buf, size_t n) {
    module_t *m = lr ? module_of(lr) : NULL;
    if (m) snprintf(buf, n, "%s+%#llx", m->name, (unsigned long long)((uint8_t *)lr - m->base)); else snprintf(buf, n, "host");
    return buf;
}
static void au_note(aout *a, const char *what, void *lr) {
    char b[64];
    if (a->nlog < 32) { a->nlog++; LOG("audio #%d %s (call site %s)%s", a->id, what, site(lr, b, sizeof b), a->nlog == 32 ? "; not logged from here on" : ""); }
}

static void notify_release(aout *a) {
    atomic_store_explicit(&a->notify_pending, 1, memory_order_release);
    if (atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
        dispatch_semaphore_signal(a->notify_sem);
}

void dm_host_helper_qos(qos_class_t q);
static void *event_notify(void *p) {
    aout *a = p; dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);
    for (;;) {
        dispatch_semaphore_wait(a->notify_sem, DISPATCH_TIME_FOREVER);
        if (atomic_exchange_explicit(&a->notify_pending, 0, memory_order_acq_rel) && a->event)
            hle_signal_system_event(a->event);
        // acq_rel: a release-only store allows pending=1 / armed=0 with both sides asleep
        atomic_exchange_explicit(&a->notify_armed, 0, memory_order_acq_rel);
        if (!atomic_load_explicit(&a->notify_run, memory_order_acquire)) break;
        if (atomic_load_explicit(&a->notify_pending, memory_order_acquire) &&
            atomic_exchange_explicit(&a->notify_armed, 1, memory_order_acq_rel) == 0)
            dispatch_semaphore_signal(a->notify_sem);
    }
    return NULL;
}

static void pull(aout *a, uint8_t *dst, uint64_t n) {
    int released = 0;
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed);
    while (n) {
        uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
        if (qh == qt || atomic_load_explicit(&a->state, memory_order_acquire) != STATE_STARTED) {
            if (qh == qt && atomic_load_explicit(&a->state, memory_order_acquire) == STATE_STARTED) { atomic_fetch_add(&a->under_n, 1); atomic_fetch_add(&a->under_b, n); }
            memset(dst, 0, n); break; }
        aout_buf *b = a->q[qh & (QMAX - 1)];
        uint64_t k = b->size - a->pos; if (k > n) k = n;
        memcpy(dst, (uint8_t *)b->ptr + a->pos, k);
        dst += k; n -= k; a->pos += k;
        if (a->pos >= b->size) {
            uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
            uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire);
            if (rw - rr == RELMAX) die("AudioOut released queue overflow");
            a->rel[rw & (RELMAX - 1)] = b;
            atomic_store_explicit(&a->rel_tail, rw + 1, memory_order_release);
            qh++;
            atomic_store_explicit(&a->q_head, qh, memory_order_release);
            a->pos = 0; released = 1;
        }
    }
    if (released) {
        if (a->event) notify_release(a);
        if (a->rel_sem) dispatch_semaphore_signal(a->rel_sem);
    }
}

// Six channels go as six when the route offers six, else as stereo (pull_device); others as the guest writes them
static int unit_channels(const aout *a, int route) { return a->sink_pcm || a->ch == 6 ? route : a->ch; }
// The renderer's default Q15 downmix: Core Audio's own 6 -> 2 keeps only the front pair. No allocation in the real-time callback.
static void pull_device(aout *a, uint8_t *dst, uint64_t bytes) {
    if (a->sink_pcm) {
        enum { BLOCK = 128 };
        int16_t scratch[BLOCK * DM_AUDIO_SINK_CHANNELS], *out = (int16_t *)dst;
        uint64_t frames = bytes / (sizeof *out * (unsigned)a->dev_ch);
        while (frames) {
            unsigned n = frames < BLOCK ? (unsigned)frames : BLOCK;
            pull(a, (uint8_t *)scratch, (uint64_t)n * sizeof scratch[0] * DM_AUDIO_SINK_CHANNELS);
            for (unsigned i = 0; i < n; i++) {
                const int16_t *src = scratch + i * DM_AUDIO_SINK_CHANNELS + (a->dev_ch == 6 ? 0 : 6);
                for (int j = 0; j < a->dev_ch; j++) *out++ = src[j];
            }
            frames -= n;
        }
        return;
    }
    if (a->ch != 6 || a->dev_ch == 6) { pull(a, dst, bytes); return; }
    enum { BLOCK = 128, CF = 32768, CC = 23167, CL = 8224, CB = 23167 };
    int16_t scratch[BLOCK * 6], *out = (int16_t *)dst;
    uint64_t frames = bytes / (2 * sizeof *out);
    while (frames) {
        uint64_t n = frames < BLOCK ? frames : BLOCK;
        pull(a, (uint8_t *)scratch, n * 6 * sizeof *scratch);
        for (uint64_t i = 0; i < n; i++) {
            const int16_t *p = scratch + 6 * i;
            int64_t c = (int64_t)p[2] * CC + (int64_t)p[3] * CL;
            int64_t l = ((int64_t)p[0] * CF + c + (int64_t)p[4] * CB) >> 15;
            int64_t r = ((int64_t)p[1] * CF + c + (int64_t)p[5] * CB) >> 15;
            *out++ = (int16_t)(l > 32767 ? 32767 : l < -32768 ? -32768 : l);
            *out++ = (int16_t)(r > 32767 ? 32767 : r < -32768 ? -32768 : r);
        }
        frames -= n;
    }
}

static OSStatus au_render(void *ctx, AudioUnitRenderActionFlags *f, const AudioTimeStamp *ts, UInt32 bus, UInt32 frames, AudioBufferList *bl) {
    (void)f; (void)ts; (void)bus; (void)frames;
    aout *a = ctx;
    atomic_fetch_add_explicit(&a->cb_n, 1, memory_order_relaxed);
    {
        // LOG takes a stdio lock: never here. A full mailbox keeps accumulating rather than waiting or overwriting.
        uint32_t qd = atomic_load(&a->q_tail) - atomic_load(&a->q_head);
        if (!a->st_calls || frames < a->st_fmin) a->st_fmin = frames;
        if (frames > a->st_fmax) a->st_fmax = frames;
        a->st_qsum += qd;
        a->st_count++;
        if (++a->st_calls % 2000 == 0 && !atomic_load_explicit(&a->st_pending, memory_order_acquire)) {
            uint64_t un = atomic_load(&a->under_n), ubb = atomic_load(&a->under_b);
            a->st_report = (audio_stats){a->st_calls, a->st_count, a->st_qsum, un - a->st_u0, ubb - a->st_ub0, a->st_fmin, a->st_fmax};
            atomic_store_explicit(&a->st_pending, 1, memory_order_release);
            a->st_fmin = ~0u; a->st_fmax = 0; a->st_qsum = a->st_count = 0; a->st_u0 = un; a->st_ub0 = ubb;
        } }
    if (atomic_load_explicit(&a->hold_device, memory_order_acquire)) {
        memset(bl->mBuffers[0].mData, 0, bl->mBuffers[0].mDataByteSize);
    } else {
        pull_device(a, bl->mBuffers[0].mData, bl->mBuffers[0].mDataByteSize);
        if (a->mute) memset(bl->mBuffers[0].mData, 0, bl->mBuffers[0].mDataByteSize);
    }
    return noErr;
}
// once per burst: the flag is cleared before the check reads the route
static _Atomic int g_route_queued;
static void dm_audio_route_changed(void);
static void route_check(void *unused) {
    (void)unused;
    atomic_store(&g_route_queued, 0);
    dm_audio_route_changed();
}
static void route_queue(void) { if (!atomic_exchange(&g_route_queued, 1)) dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), NULL, route_check); }
// DefaultOutput's 512 frames can exceed what a game queued (zero-filled gaps): ask about 5 ms; DM_DBG_AU_FRAMES=N overrides
static void au_buffer(AudioUnit au, const char *why) {
    AudioDeviceID device = kAudioObjectUnknown; UInt32 sz = sizeof device;
    if (AudioUnitGetProperty(au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &device, &sz) || device == kAudioObjectUnknown) return;
    AudioStreamBasicDescription df; sz = sizeof df;
    OSStatus st = AudioUnitGetProperty(au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Output, 0, &df, &sz);
    if (st || !(df.mSampleRate > 0)) { LOG("[RED] audio device buffer: cannot read the output sample rate (status %d)", (int)st); return; }
    const char *e = getenv("DM_DBG_AU_FRAMES");
    UInt32 fr = e ? (UInt32)atoi(e) : (UInt32)(df.mSampleRate / 200 + 0.5);
    AudioValueRange range; sz = sizeof range;
    if (!e && !AudioUnitGetProperty(au, kAudioDevicePropertyBufferFrameSizeRange, kAudioUnitScope_Global, 0, &range, &sz)) {
        if (fr < range.mMinimum) fr = (UInt32)range.mMinimum;
        if (fr > range.mMaximum) fr = (UInt32)range.mMaximum;
    }
    // changing the buffer can notify the stream-format listener: never write an unchanged value
    UInt32 actual = 0; sz = sizeof actual;
    st = AudioUnitGetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &actual, &sz);
    if (st || actual != fr) st = AudioUnitSetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &fr, sizeof fr);
    if (!st) { sz = sizeof actual; st = AudioUnitGetProperty(au, kAudioDevicePropertyBufferFrameSize, kAudioUnitScope_Global, 0, &actual, &sz); }
    LOG("%saudio device buffer: %u frames at %.0f Hz%s (requested %u, status %d)", st ? "[RED] " : "", actual, df.mSampleRate, why ? why : "", fr, (int)st);
}
static void au_device_changed(void *ctx, AudioUnit au, AudioUnitPropertyID id, AudioUnitScope scope, AudioUnitElement el) {
    (void)ctx;
    if (el == 0 && ((id == kAudioOutputUnitProperty_CurrentDevice && scope == kAudioUnitScope_Global) ||
                   (id == kAudioUnitProperty_StreamFormat && scope == kAudioUnitScope_Output))) {
        au_buffer(au, ", after output changed");
        // a unit must not be replaced inside its own notification: queued, once per burst
        route_queue();
    }
}
static void au_dispose(AudioUnit au) {
    // disposal clears CurrentDevice and notifies: remove the listeners first
    AudioUnitRemovePropertyListenerWithUserData(au, kAudioOutputUnitProperty_CurrentDevice, au_device_changed, NULL);
    AudioUnitRemovePropertyListenerWithUserData(au, kAudioUnitProperty_StreamFormat, au_device_changed, NULL);
    AudioUnitUninitialize(au); AudioComponentInstanceDispose(au);
}
// DM_AUDIO_DEVICE=<part of a device name>; kAudioObjectUnknown when unset or unmatched
static AudioDeviceID env_out_device(void) {
    const char *want = getenv("DM_AUDIO_DEVICE");
    if (!want || !*want) return kAudioObjectUnknown;
    AudioObjectPropertyAddress pa = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = 0; AudioDeviceID found = kAudioObjectUnknown;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &pa, 0, NULL, &sz) || !sz) return kAudioObjectUnknown;
    AudioDeviceID *ids = malloc(sz);
    if (!ids || AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, ids)) { free(ids); return kAudioObjectUnknown; }
    for (UInt32 i = 0; i < sz / sizeof *ids && found == kAudioObjectUnknown; i++) {
        AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        CFStringRef name = NULL; UInt32 nsz = sizeof(CFStringRef); char buf[256] = {0};
        if (!AudioObjectGetPropertyData(ids[i], &na, 0, NULL, &nsz, &name) && name) {
            if (CFStringGetCString(name, buf, sizeof buf, kCFStringEncodingUTF8) && strstr(buf, want)) found = ids[i];
            CFRelease(name);
        }
    }
    free(ids);
    static _Atomic int told;
    if (found == kAudioObjectUnknown && !atomic_exchange(&told, 1)) LOG("[RED] audio: DM_AUDIO_DEVICE=\"%s\" names no output device: the default output plays", want);
    return found;
}
static AudioDeviceID out_device(void) {
    AudioDeviceID d = env_out_device();
    if (d != kAudioObjectUnknown) return d;
    AudioObjectPropertyAddress pa = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 sz = sizeof d; d = kAudioObjectUnknown;
    AudioObjectGetPropertyData(kAudioObjectSystemObject, &pa, 0, NULL, &sz, &d);
    return d;
}
// 0 when unknown
static int device_out_channels(AudioDeviceID d, char *name, size_t namesz) {
    if (name) name[0] = 0;
    if (d == kAudioObjectUnknown) return 0;
    AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    CFStringRef cn = NULL; UInt32 nsz = sizeof(CFStringRef);
    if (name && !AudioObjectGetPropertyData(d, &na, 0, NULL, &nsz, &cn) && cn) { CFStringGetCString(cn, name, namesz, kCFStringEncodingUTF8); CFRelease(cn); }
    AudioObjectPropertyAddress pa = {kAudioDevicePropertyStreamFormat, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    AudioStreamBasicDescription f; UInt32 sz = sizeof f;
    if (AudioObjectGetPropertyData(d, &pa, 0, NULL, &sz, &f)) return 0;
    return (int)f.mChannelsPerFrame;
}
// Surround only when Core Audio maps MPEG_5_1_A's L R C Ls Rs (the LFE is optional: 5.0) onto the device's first `channels` outputs through its
// preferred speaker layout (Configure Speakers in Audio MIDI Setup; an HDMI sink's own): an audio interface's many outputs are discrete
static int device_has_surround(AudioDeviceID d, int channels) {
    if (d == kAudioObjectUnknown || channels < 5) return 0;
    AudioObjectPropertyAddress pa = {kAudioDevicePropertyPreferredChannelLayout, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    const UInt32 head = (UInt32)offsetof(AudioChannelLayout, mChannelDescriptions);
    UInt32 sz = 0;
    if (AudioObjectGetPropertyDataSize(d, &pa, 0, NULL, &sz) || sz < head) return 0;
    AudioChannelLayout *l = malloc(sz);
    if (!l) return 0;
    int surround = 0;
    if (!AudioObjectGetPropertyData(d, &pa, 0, NULL, &sz, l) && sz >= head &&
        (l->mChannelLayoutTag != kAudioChannelLayoutTag_UseChannelDescriptions ||
         l->mNumberChannelDescriptions <= (sz - head) / sizeof(AudioChannelDescription))) {
        AudioChannelLayout in = {.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A};
        const AudioChannelLayout *pair[2] = {&in, l};
        UInt32 msz = 0;
        if (!AudioFormatGetPropertyInfo(kAudioFormatProperty_ChannelMap, sizeof pair, pair, &msz) && msz >= sizeof(SInt32)) {
            SInt32 *map = malloc(msz);
            if (map && !AudioFormatGetProperty(kAudioFormatProperty_ChannelMap, sizeof pair, pair, &msz, map)) {
                UInt32 count = msz / (UInt32)sizeof *map;
                if (count > (UInt32)channels) count = (UInt32)channels;
                unsigned routed = 0;
                for (UInt32 i = 0; i < count; i++)
                    if (map[i] >= 0 && map[i] < 6) routed |= 1u << (unsigned)map[i];
                surround = (routed & 0x37u) == 0x37u;   // bits 0 L, 1 R, 2 C, 4 Ls, 5 Rs; 3 (LFE) optional
            }
            free(map);
        }
    }
    free(l);
    return surround;
}
static _Atomic int g_route;
static pthread_mutex_t g_route_mu = PTHREAD_MUTEX_INITIALIZER;
static void route_notify(void *unused) { (void)unused; dm_audio_device_changed(); }
static AudioDeviceID g_route_device;
static OSStatus route_device_changed(AudioObjectID id, UInt32 n, const AudioObjectPropertyAddress *pa, void *ctx) {
    (void)id; (void)n; (void)pa; (void)ctx; route_queue(); return noErr;
}
static void route_listen(void) {
    AudioObjectPropertyAddress pa = {kAudioHardwarePropertyDefaultOutputDevice, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    OSStatus st = AudioObjectAddPropertyListener(kAudioObjectSystemObject, &pa, route_device_changed, NULL);
    if (st) LOG("[RED] audio: cannot listen for the default output device (status %d)", (int)st);
}
int dm_audio_output_channels(void) {
    pthread_mutex_lock(&g_route_mu);
    int device_changed = 0;
    static pthread_once_t listener = PTHREAD_ONCE_INIT; pthread_once(&listener, route_listen);
    AudioDeviceID device = out_device();
    if (device != g_route_device) {
        static const AudioObjectPropertySelector watched[] = {kAudioDevicePropertyStreamFormat, kAudioDevicePropertyPreferredChannelLayout};
        for (size_t i = 0; i < sizeof watched / sizeof *watched; i++) {
            AudioObjectPropertyAddress pa = {watched[i], kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
            if (g_route_device) AudioObjectRemovePropertyListener(g_route_device, &pa, route_device_changed, NULL);
            if (device) {
                OSStatus st = AudioObjectAddPropertyListener(device, &pa, route_device_changed, NULL);
                if (st && !(i && st == kAudioHardwareUnknownPropertyError))
                    LOG("[RED] audio: cannot listen for output %s (status %d)", i ? "speaker layout" : "channels", (int)st);
            }
        }
        device_changed = g_route_device != 0; g_route_device = device;
    }
    char name[256]; int n = device_out_channels(device, name, sizeof name);
    int ch = device_has_surround(device, n) ? 6 : 2;
    int old = atomic_exchange(&g_route, ch);
    if (ch != old || device_changed) LOG("audio output route: \"%s\" has %d channels, the host plays %s", name, n, ch == 6 ? "six (5.1)" : n >= 5 ? "stereo (no 5.1 speaker layout: Audio MIDI Setup, Configure Speakers)" : "stereo");
    // Signaling a kernel event takes the guest scheduler lock: off the AudioUnit / registry locks, after the cache is published
    if (old && (old != ch || device_changed)) dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), NULL, route_notify);
    pthread_mutex_unlock(&g_route_mu);
    return ch;
}
int dm_audio_route(void) {
    int r = atomic_load(&g_route);
    return r ? r : dm_audio_output_channels();
}
// the guest's order (FL FR C LFE BL BR) is Core Audio's MPEG_5_1_A
static OSStatus au_set_layout6(AudioUnit au) {
    AudioChannelLayout l = {.mChannelLayoutTag = kAudioChannelLayoutTag_MPEG_5_1_A};
    return AudioUnitSetProperty(au, kAudioUnitProperty_AudioChannelLayout, kAudioUnitScope_Input, 0, &l, sizeof l);
}
static OSStatus au_create(aout *a, int route) {
    AudioComponentDescription d = {.componentType = kAudioUnitType_Output,
                                   .componentSubType = kAudioUnitSubType_DefaultOutput,
                                   .componentManufacturer = kAudioUnitManufacturer_Apple};
    AudioComponent c = AudioComponentFindNext(NULL, &d);
    a->au = NULL; if (!c || AudioComponentInstanceNew(c, &a->au)) { a->au = NULL; return -1; }
    a->dev_ch = unit_channels(a, route);
    unsigned ch = (unsigned)a->dev_ch;
    AudioStreamBasicDescription f = {.mSampleRate = RATE, .mFormatID = kAudioFormatLinearPCM,
                                     .mFormatFlags = kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
                                     .mBytesPerPacket = 2u * ch, .mFramesPerPacket = 1, .mBytesPerFrame = 2u * ch,
                                     .mChannelsPerFrame = ch, .mBitsPerChannel = 16};
    AURenderCallbackStruct cb = {au_render, a};
    AudioDeviceID pinned = env_out_device();
    if (pinned != kAudioObjectUnknown) {
        OSStatus st = AudioUnitSetProperty(a->au, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &pinned, sizeof pinned);
        if (st) LOG("[RED] audio: cannot use the DM_AUDIO_DEVICE output (status %d): the default output plays", (int)st);
    }
    const AudioUnitPropertyID props[] = {kAudioOutputUnitProperty_CurrentDevice, kAudioUnitProperty_StreamFormat};
    for (unsigned i = 0; i < sizeof props / sizeof *props; i++) {
        OSStatus st = AudioUnitAddPropertyListener(a->au, props[i], au_device_changed, NULL);
        if (st) LOG("[RED] audio: cannot listen for output property %u (status %d)", (unsigned)props[i], (int)st);
    }
    au_buffer(a->au, NULL);
    OSStatus st = AudioUnitSetProperty(a->au, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f);
    if (!st && ch == 6) {
        OSStatus ls = au_set_layout6(a->au);   // a failure leaves the default mapping
        if (ls) LOG("audio #%d: no 5.1 channel layout on the AudioUnit (status %d): its default mapping stands", a->id, (int)ls);
    }
    if (!st) st = AudioUnitSetProperty(a->au, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb);
    if (!st) st = AudioUnitInitialize(a->au);
    if (st) { au_dispose(a->au); a->au = NULL; }
    return st;
}
// Making an AudioUnit takes about 24 KB of stack, more than some game threads have left: on a host thread
static void *au_create_th(void *a) { return (void *)(intptr_t)au_create(a, dm_audio_output_channels()); }
static void au_open(aout *a) {
    pthread_t t; void *r = NULL;
    if (pthread_create(&t, NULL, au_create_th, a) || dm_host_thread_join(t, &r)) die("AudioUnit setup: cannot start its thread");
    OSStatus st = (OSStatus)(intptr_t)r; if (st) die("AudioUnit setup failed (status %d)", (int)st);
}
// After a stop of the whole process (> 100 ms behind) restart the count: making up the time would release every buffer at once
_Static_assert(DM_AUDIO_SINK_CHANNELS >= 6, "the silent sink's block holds the widest output: the renderer's, and no fewer than a guest output's six (au_channels_ok)");
static void *null_sink(void *p) {
    aout *a = p;
    uint8_t scratch[RATE / 200 * 2 * DM_AUDIO_SINK_CHANNELS];
    dm_host_helper_qos(QOS_CLASS_USER_INTERACTIVE);
    uint64_t bytes = (uint64_t)RATE / 200 * 2 * a->ch, t = now_ns();
    while (atomic_load_explicit(&a->null_run, memory_order_acquire)) {
        t += 5000000; uint64_t now = now_ns();
        if (t > now) usleep((useconds_t)((t - now) / 1000)); else if (now - t > 100000000) t = now;
        pull(a, scratch, bytes);
    }
    return NULL;
}
// callers hold g_aos_lock; pull() takes no lock, so joining a sink under it cannot deadlock
static void null_start(aout *a) {
    atomic_store_explicit(&a->hold_device, 1, memory_order_release);
    atomic_store_explicit(&a->null_run, 1, memory_order_release);
    pthread_create(&a->null_th, NULL, null_sink, a);
    a->backend = BK_NULL;
}
static void null_stop(aout *a) {
    atomic_store_explicit(&a->null_run, 0, memory_order_release);
    pthread_join(a->null_th, NULL);
    a->backend = BK_NONE;
}
static void au_halt(aout *a) {
    if (a->start_phase == AU_STARTING) a->start_phase = AU_CANCELED;
    if (a->backend == BK_AU || a->start_phase == AU_WAIT_CALLBACK) {
        AudioOutputUnitStop(a->au);
        a->start_phase = AU_IDLE;
        if (a->backend == BK_AU) a->backend = BK_NONE;
    }
    a->stalled = 0; a->retry_ms = 0; a->retry_at = 0;
}
static int real_audio(void) { const char *e = getenv("DM_AUDIO"); return e ? atoi(e) != 0 : 0; }
static void au_retry(aout *a, uint64_t now) {
    a->stalled = 1;
    a->retry_ms = a->retry_ms ? (a->retry_ms < 8000 ? a->retry_ms * 2 : 8000) : 1000;
    a->retry_at = now + (uint64_t)a->retry_ms * 1000000;
}
static void au_replace(aout *a, int route) {
    au_dispose(a->au);
    OSStatus st = au_create(a, route);
    if (st) LOG("[RED] audio #%d: no AudioUnit after the route change (status %d): this output is silent until the next route change", a->id, (int)st);
    a->unit_lost = st != 0;
    a->reset_pending = 0;
}
static void aout_free(aout *a) {
    if (a->au) au_dispose(a->au);
    dispatch_release(a->notify_sem);
    if (a->rel_sem) dispatch_release(a->rel_sem);
    if (a->dump) fclose(a->dump);
    pthread_mutex_destroy(&a->m); free(a);
}
// A HAL start can block for seconds: the silent sink plays meanwhile; Stop / suspend cancel without waiting, Close leaves the output to this worker.
static void au_start_device(void *p) {
    aout *a = p;
    OSStatus st = AudioOutputUnitStart(a->au);
    pthread_mutex_lock(&g_aos_lock);
    if (a->closed) {   // only Close reaches this: stop and free outside the lock
        pthread_mutex_unlock(&g_aos_lock);
        if (!st) AudioOutputUnitStop(a->au);
        aout_free(a);
        return;
    }
    if (a->start_phase == AU_CANCELED) {
        if (!st) AudioOutputUnitStop(a->au);
        a->start_phase = AU_IDLE;
        if (a->reset_pending) au_replace(a, dm_audio_output_channels());
    } else if (st) {
        if (!a->stalled) LOG("[RED] audio #%d: the AudioUnit did not start (status %d): the silent sink stands in, retrying", a->id, (int)st);
        a->start_phase = AU_IDLE; au_retry(a, now_ns());
    } else {
        a->wd_n = atomic_load_explicit(&a->cb_n, memory_order_relaxed);
        a->start_phase = AU_WAIT_CALLBACK; a->wd_t = now_ns();
    }
    pthread_mutex_unlock(&g_aos_lock);
}
static void au_request_start(aout *a) {
    if (!a->au || g_suspended || a->start_phase != AU_IDLE) return;
    a->start_phase = AU_STARTING;
    dispatch_async_f(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), a, au_start_device);
}
// The system can stop calling back without telling the AudioUnit, and the game would wait for releases for good: after WD_STALL_MS the
// silent sink takes over and the unit is retried after 1, 2, 4, 8 s. The time is read under g_aos_lock, where wd_t is stamped.
enum { WD_TICK_MS = 50, WD_STALL_MS = 200 };
typedef struct { audio_stats stats; int id, ch; } audio_report;
// a local copy owns every log argument: a slow log holds no lock
static int stats_take(aout *a, audio_report *r) {
    if (!atomic_load_explicit(&a->st_pending, memory_order_acquire)) return 0;
    *r = (audio_report){a->st_report, a->id, a->ch};
    atomic_store_explicit(&a->st_pending, 0, memory_order_release);
    return 1;
}
static void stats_log(const audio_report *r) {
    const audio_stats *s = &r->stats;
    LOG("audio #%d device callbacks %llu: %u–%u frames each, %.1f blocks queued on average at callback; in these %llu, %llu underruns, %.1f ms zero-padded", r->id,
        (unsigned long long)s->calls, s->fmin, s->fmax, (double)s->qsum / s->count, (unsigned long long)s->count,
        (unsigned long long)s->underruns, s->zero_bytes / (2.0 * r->ch) / (RATE / 1000.0));
}
static void stats_drain(void) {
    audio_report reports[AOMAX]; int n = 0;
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] && stats_take(g_aos[i], &reports[n])) n++;
    pthread_mutex_unlock(&g_aos_lock);
    for (int i = 0; i < n; i++) stats_log(&reports[i]);
}
static void *au_watchdog(void *p) {
    (void)p; uint64_t last = now_ns();
    for (;;) {
        usleep(WD_TICK_MS * 1000);
        pthread_mutex_lock(&g_aos_lock);
        uint64_t t = now_ns(), gap = t - last; last = t;
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i]; if (!a || !a->au) continue;
            uint64_t n = atomic_load_explicit(&a->cb_n, memory_order_relaxed);
            if (a->start_phase == AU_WAIT_CALLBACK) {
                if (n != a->wd_n) {
                    null_stop(a);
                    atomic_store_explicit(&a->hold_device, 0, memory_order_release);
                    if (a->stalled) LOG("audio #%d: AudioUnit restarted", a->id);
                    a->backend = BK_AU; a->start_phase = AU_IDLE; a->stalled = a->retry_ms = 0;
                    a->wd_n = n; a->wd_t = t;
                } else if (gap > 4ull * WD_TICK_MS * 1000000) a->wd_t = t;
                else if (t - a->wd_t >= 1000000000ull) {
                    if (!a->stalled) LOG("[RED] audio #%d: no device callback after start; continuing on the silent sink", a->id);
                    AudioOutputUnitStop(a->au); a->start_phase = AU_IDLE; au_retry(a, t);
                }
                continue;
            }
            if (a->backend == BK_NULL) {
                if (t >= a->retry_at) au_request_start(a);
                continue;
            }
            if (a->backend != BK_AU) continue;
            if (gap > 4ull * WD_TICK_MS * 1000000 || n != a->wd_n) {
                a->wd_n = n; a->wd_t = t;
            } else if (t - a->wd_t > WD_STALL_MS * 1000000ull) {
                LOG("[RED] audio #%d: the device has not called back for %.0f ms while playing: the silent sink stands in, retrying the AudioUnit", a->id, (t - a->wd_t) / 1e6);
                AudioOutputUnitStop(a->au); a->backend = BK_NONE; null_start(a);
                au_retry(a, t);
            }
        }
        pthread_mutex_unlock(&g_aos_lock);
        stats_drain();
    }
    return NULL;
}
static void au_watchdog_start(void) { pthread_t t; if (!pthread_create(&t, NULL, au_watchdog, NULL)) pthread_detach(t); }

// The SDK's AudioOut front: rate, channels, format, state at +0 / +4 / +8 / +0xc. Middleware checks the first word before it closes:
// left zero, it frees buffers the host still reads.
static void au_info(void *g, int ch, int state) {
    uint32_t *f = g;
    f[0] = RATE;
    f[1] = (uint32_t)ch;
    f[2] = FMT_PCM16;
    f[3] = (uint32_t)state;
}
static void route_sync(int route);
// audout takes 1, 2 or 6 (0: the default 2); the sink's and the downmix's blocks are sized for these
static int au_channels_ok(int ch) { return ch == 1 || ch == 2 || ch == 6; }
// param: AudioOutParameter { int32 sampleRate; uint16 channelCount; ... }, 0 = default (48 kHz / 2)
static Result au_open_at(void *g, void *ev, const char *name, const uint8_t *param, void *lr, int sink_pcm) {
    static _Atomic int nopen;
    int ch = *(const uint16_t *)(param + 4); int rate = *(const int32_t *)param;
    if (!ch) ch = 2;
    if (rate && rate != RATE) {   // ResultInvalidSampleRate (153 / 3)
        char sb[64]; LOG("[RED] AudioOut sample rate %d: audout takes 48 kHz only, the open fails with 0x699 (call site %s)", rate, site(lr, sb, sizeof sb));
        return 0x699u;
    }
    if (!sink_pcm && !au_channels_ok(ch)) {   // ResultInvalidChannelCount (153 / 10)
        char sb[64]; LOG("[RED] AudioOut channel count %d: audout takes 1, 2 or 6, the open fails with 0x1499 (call site %s)", ch, site(lr, sb, sizeof sb));
        return 0x1499u;
    }
    aout *a = calloc(1, sizeof *a);
    const char *mute = getenv("DM_AUDIO_MUTE"); a->mute = mute && atoi(mute);
    pthread_mutex_init(&a->m, NULL);
    a->event = ev;
    a->ch = ch;
    a->dev_ch = ch;
    a->sink_pcm = sink_pcm;
    a->key = g;
    atomic_init(&a->state, STATE_STOPPED);
    atomic_init(&a->q_head, 0); atomic_init(&a->q_tail, 0);
    atomic_init(&a->rel_head, 0); atomic_init(&a->rel_tail, 0);
    atomic_init(&a->null_run, 0); atomic_init(&a->notify_run, 1);
    atomic_init(&a->notify_pending, 0); atomic_init(&a->notify_armed, 0);
    atomic_init(&a->st_pending, 0);
    a->notify_sem = dispatch_semaphore_create(0); a->id = atomic_fetch_add(&nopen, 1);
    pthread_create(&a->notify_th, NULL, event_notify, a);
    if (ev) hle_bind_system_event(ev, 1);
    char sb[64]; int use_au = real_audio();
    LOG("audio #%d OpenAudioOut %s channels %d %s%s (call site %s)", a->id, name ? name : "(default)", ch, use_au ? "real output" : "silent sink", ev ? "" : ", no release event (the game polls)", site(lr, sb, sizeof sb));
    // the helper is joined before readers can find this output; outside g_aos_lock: joining can yield the guest core
    if (use_au) au_open(a);
    // before publication: a DeviceSink dump keeps the opening route's channels (its 6, or its stereo fold) across a route change
    a->dump_ch = a->sink_pcm ? (a->au && a->dev_ch == 6 ? 6 : 2) : a->ch;
    objmap_put(g, a);
    pthread_mutex_lock(&g_aos_lock);
    int slot = -1; for (int i = 0; i < AOMAX; i++) if (!g_aos[i]) { slot = i; break; }
    if (slot < 0) die("more than %d AudioOuts open at once", AOMAX);
    g_aos[slot] = a;
    if (a->au) route_queue();   // a route change during construction found no output to replace
    pthread_mutex_unlock(&g_aos_lock);
    if (use_au) { static pthread_once_t wd = PTHREAD_ONCE_INIT; pthread_once(&wd, au_watchdog_start); }
    au_info(g, ch, STATE_STOPPED);
    return RES_OK;
}
// SDK 22's overload takes no release event; unbound, it reaches audout, fails with 0xe15 and the game aborts
HLE_FORCE("_ZN2nn5audio12OpenAudioOutEPNS0_8AudioOutEPKcRKNS0_17AudioOutParameterE", "connect to the audout service, no release event (SDK 22 overload)",
          Result, h_au_Open4, void *g, const char *name, const uint8_t *param) { return au_open_at(g, NULL, name, param, dm_bigstack_caller(__builtin_return_address(0)), 0); }
static Result au_start_at(void *g, void *lr) {
    aout *a = ao(g);
    pthread_mutex_lock(&g_aos_lock);
    int was = atomic_exchange_explicit(&a->state, STATE_STARTED, memory_order_acq_rel);
    au_note(a, was == STATE_STARTED ? "Start (already playing)" : g_suspended ? "Start (app in the background: the silent sink takes over for now)" : "Start", lr);
    if (was != STATE_STARTED) {
        null_start(a); au_request_start(a);
    }
    pthread_mutex_unlock(&g_aos_lock);
    return RES_OK;
}
// Core Audio needs far more stack than a game thread has: these run on the big host stack (the first AudioOutputUnitStart overflows a guest stack).

// Once the backend stops, return every buffer, even a partly played one: a sound engine may wait for them before joining its thread.
static void au_release_pending(aout *a) {
    pthread_mutex_lock(&a->m);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_relaxed), qt = atomic_load_explicit(&a->q_tail, memory_order_acquire);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_acquire), rw = atomic_load_explicit(&a->rel_tail, memory_order_relaxed);
    int released = qh != qt;
    while (qh != qt) {
        if (rw - rr >= RELMAX) die("AudioOut released queue overflow while stopping");
        a->rel[rw++ & (RELMAX - 1)] = a->q[qh++ & (QMAX - 1)];
    }
    atomic_store_explicit(&a->rel_tail, rw, memory_order_release);
    atomic_store_explicit(&a->q_head, qh, memory_order_release); a->pos = 0;
    pthread_mutex_unlock(&a->m);
    if (released) { if (a->event) notify_release(a); if (a->rel_sem) dispatch_semaphore_signal(a->rel_sem); }
}
static void au_stop_at(void *g, void *lr) {   // lr NULL: Close's stop, not logged
    aout *a = ao(g);
    pthread_mutex_lock(&g_aos_lock);
    int was = atomic_exchange_explicit(&a->state, STATE_STOPPED, memory_order_acq_rel);
    if (lr) au_note(a, was == STATE_STARTED ? "Stop" : "Stop (already stopped)", lr);
    au_halt(a);
    if (a->backend == BK_NULL) null_stop(a);
    if (was == STATE_STARTED) au_release_pending(a);
    pthread_mutex_unlock(&g_aos_lock);
}
static void au_close(void *g, void *lr) {
    au_note(ao(g), "Close", lr);
    au_stop_at(g, NULL);
    aout *a = objmap_del(g);
    atomic_store_explicit(&a->notify_run, 0, memory_order_release);
    dispatch_semaphore_signal(a->notify_sem);
    pthread_join(a->notify_th, NULL);
    if (a->dump) { fclose(a->dump); a->dump = NULL; }
    audio_report report;
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i] == a) g_aos[i] = NULL;
    int have_report = stats_take(a, &report), ch = a->ch;
    // a start the driver still holds keeps the unit: hand the output to that worker rather than wait
    int in_flight = a->start_phase == AU_CANCELED;
    a->closed = in_flight; a->reset_pending = 0;
    pthread_mutex_unlock(&g_aos_lock);
    if (have_report) stats_log(&report);
    if (!in_flight) aout_free(a);
    au_info(g, ch, STATE_STOPPED);
}
// DM_AUDIO_DUMP=<file>: each block an output accepts (16-bit, 48 kHz, dump_ch channels), output #0 to <file>, #N to <file>.<N>; a DeviceSink block's
// eight channels (six and their stereo fold) unpacked to the representation latched at open. Under a->m, before q_tail publishes the block.
static void audio_dump(aout *a, const aout_buf *b) {
    if (!a->dump_tried) {
        a->dump_tried = 1; const char *e = getenv("DM_AUDIO_DUMP"); char p[1024];
        if (e && *e) {
            int n = a->id ? snprintf(p, sizeof p, "%s.%d", e, a->id) : snprintf(p, sizeof p, "%s", e);
            if (n < 0 || (size_t)n >= sizeof p) LOG("[RED] DM_AUDIO_DUMP path too long for audio #%d", a->id);
            else if (!(a->dump = fopen(p, "wb"))) LOG("[RED] DM_AUDIO_DUMP cannot open %s", p);
            else LOG("audio #%d DM_AUDIO_DUMP: its blocks (%d channels, 16-bit, 48 kHz) go to %s", a->id, a->dump_ch, p);
        }
    }
    if (!a->dump || !b || !b->ptr || !b->size) return;
    if (!a->sink_pcm) { fwrite(b->ptr, 1, (size_t)b->size, a->dump); return; }
    enum { BLOCK = 128 };
    const unsigned ch = a->dump_ch == 6 ? 6 : 2, off = ch == 6 ? 0 : 6;
    const int16_t *src = b->ptr;
    size_t frames = (size_t)b->size / (sizeof *src * DM_AUDIO_SINK_CHANNELS);
    int16_t buf[BLOCK * 6];
    while (frames) {
        size_t n = frames < BLOCK ? frames : BLOCK;
        for (size_t i = 0; i < n; i++)
            memcpy(buf + i * ch, src + i * DM_AUDIO_SINK_CHANNELS + off, ch * sizeof *buf);
        fwrite(buf, sizeof *buf, n * ch, a->dump);
        src += n * DM_AUDIO_SINK_CHANNELS; frames -= n;
    }
}
static unsigned h_au_Append(void *g, aout_buf *b) {
    aout *a = ao(g);
    pthread_mutex_lock(&a->m);
    uint32_t qt = atomic_load_explicit(&a->q_tail, memory_order_relaxed);
    uint32_t qh = atomic_load_explicit(&a->q_head, memory_order_acquire);
    int ok = qt - qh < QMAX;
    if (ok) { audio_dump(a, b); a->q[qt & (QMAX - 1)] = b; atomic_store_explicit(&a->q_tail, qt + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return ok;
}
static aout_buf *h_au_Released(void *g) {
    aout *a = ao(g); aout_buf *b = NULL;
    pthread_mutex_lock(&a->m);
    uint32_t rr = atomic_load_explicit(&a->rel_head, memory_order_relaxed);
    uint32_t rw = atomic_load_explicit(&a->rel_tail, memory_order_acquire);
    if (rr != rw) { b = a->rel[rr & (RELMAX - 1)]; atomic_store_explicit(&a->rel_head, rr + 1, memory_order_release); }
    pthread_mutex_unlock(&a->m);
    return b;
}

// The host serves no audin device: an AudioIn does not open
static Result ai_open(const char *name, void *lr) {
    static _Atomic int once; char sb[64];
    if (!atomic_exchange(&once, 1)) LOG("audio input %s: the host has no microphone, the open fails (call site %s)", name ? name : "(default)", site(lr, sb, sizeof sb));
    return RES_FAIL;
}
HLE_FORCE("_ZN2nn5audio11OpenAudioInEPNS0_7AudioInEPKcRKNS0_16AudioInParameterE", "connect to the audin service (no buffer event: Patrick's Parabox imports this one); no microphone on the host: fails",
          Result, h_ai_OpenNoEv, void *g, const char *name, const void *param) { (void)g; (void)param; return ai_open(name, dm_bigstack_caller(__builtin_return_address(0))); }

// SDK 21's OutputStream (docs/reference/audio.md#sdk-21s-outputstream-on-a-virtual-output-device): float frames, each write one 16-bit
// block on an AudioOut. Unhooked, the SDK connects to auddmg:u and aborts with 0xa99.
enum { OS_NONE = 0, OS_STOPPED = 1, OS_STARTED = 2, OS_FLOAT = 5, OS_PCM16 = 2 };
typedef struct { uint8_t key[64]; pthread_mutex_t m; int ch, fmt, cap, queued, nblk, state; } ostream;   // key: the AudioOut's object
typedef struct { aout_buf b; int frames; int16_t pcm[]; } oblock;
static ostream *os_of(const void *g) {
    ostream *o = objmap_get(g);
    if (!o) die("OutputStream %p was not opened by the host", g);
    return o;
}
static void os_reclaim(ostream *o) {   // under o->m
    aout_buf *b; while ((b = h_au_Released(o->key))) { oblock *k = (oblock *)b; o->queued -= k->frames; o->nblk--; free(k); }
}
enum { VDEV_NAME = 0x100 };
static const char VDEV_OUT[] = "AudioVirtualOutput";
HLE_FORCE("_ZN2nn5audio29GetDefaultVirtualOutputDeviceEPNS0_15AudioDeviceNameE", "connect to auddmg:u; the host has one output: its name", void, h_os_DefaultDev, char *name) {
    memset(name, 0, VDEV_NAME); strlcpy(name, VDEV_OUT, VDEV_NAME);
}
// 0x110 bytes: the name, the shortest stream in ns at +0x100 (Open refuses less with 0x899), +0x108 open (0):
// docs/reference/audio.md#the-virtual-output-devices-properties-and-the-outputstream-open-without-flags
#define OS_MIN_NS 10000000LL   // one 10 ms period: the host's AudioOut takes any block size
typedef struct { char name[VDEV_NAME]; int64_t min_ns; uint32_t word_108, pad; } vdev_props;
HLE_FORCE("_ZN2nn5audio32GetVirtualOutputDevicePropertiesEPNS0_23VirtualDevicePropertiesERKNS0_15AudioDeviceNameE", "connect to auddmg:u; the host's one output device",
          Result, h_os_Props, vdev_props *p, const char *name) {
    if (strncmp(name, VDEV_OUT, VDEV_NAME)) { LOG("[RED] virtual output device %.64s: the host has only %s, no properties", name, VDEV_OUT); return RES_FAIL; }
    memset(p, 0, sizeof *p); memcpy(p->name, name, sizeof p->name);
    p->min_ns = OS_MIN_NS;
    return RES_OK;
}
static Result os_open(void *g, void *ev, const int32_t *fmt, int frames, void *lr) {
    int f = fmt[0], ch = fmt[1], rate = fmt[2];
    if (f != OS_FLOAT && f != OS_PCM16) { LOG("[RED] OutputStream sample format %d: not supported, the open fails", f); return RES_FAIL; }
    if (rate && rate != RATE) { LOG("[RED] OutputStream sample rate %d: not supported (48 kHz only), the open fails", rate); return RES_FAIL; }
    // what the SDK answers to another count is not known
    if (!au_channels_ok(ch)) { LOG("[RED] OutputStream channel count %d: not supported (1, 2 or 6), the open fails", ch); return RES_FAIL; }
    if (frames <= 0) return RES_FAIL;
    ostream *o = calloc(1, sizeof *o); if (!o) die("OutputStream calloc");
    pthread_mutex_init(&o->m, NULL); o->ch = ch; o->fmt = f; o->cap = frames; o->state = OS_STOPPED;
    uint8_t param[8] = {0}; *(int32_t *)param = RATE; *(uint16_t *)(param + 4) = (uint16_t)ch;
    Result r = au_open_at(o->key, ev, "OutputStream", param, lr, 0);
    if (r) { free(o); return r; }
    objmap_put(g, o);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEiiNS_4util10BitFlagSetILi32ENS0_16OutputStreamFlagEEE",
          "connect to auddmg:u; an AudioOut of the host's", Result, h_os_Open, void *g, void *ev, void *buf, size_t size, const char *dev, const int32_t *fmt, int frames, int threshold, uint32_t flags) {
    (void)buf; (void)size; (void)dev; (void)threshold; (void)flags;
    return os_open(g, ev, fmt, frames, dm_bigstack_caller(__builtin_return_address(0)));
}
HLE_FORCE("_ZN2nn5audio16OpenOutputStreamEPNS0_16OutputStreamTypeEPNS_2os11SystemEventEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEii",
          "connect to auddmg:u (no flags); an AudioOut of the host's", Result, h_os_OpenNoFlags, void *g, void *ev, void *buf, size_t size, const char *dev, const int32_t *fmt, int frames, int threshold) {
    (void)buf; (void)size; (void)dev; (void)threshold;
    return os_open(g, ev, fmt, frames, dm_bigstack_caller(__builtin_return_address(0)));
}
HLE_FORCE("_ZN2nn5audio17StartOutputStreamEPNS0_16OutputStreamTypeE", "start the stream's output", Result, h_os_Start, void *g) {
    ostream *o = os_of(g);
    o->state = OS_STARTED;
    return au_start_at(o->key, dm_bigstack_caller(__builtin_return_address(0)));
}
HLE_FORCE("_ZN2nn5audio16StopOutputStreamEPNS0_16OutputStreamTypeE", "stop the stream's output", void, h_os_Stop, void *g) {
    ostream *o = os_of(g);
    au_stop_at(o->key, dm_bigstack_caller(__builtin_return_address(0)));
    o->state = OS_STOPPED;
}
HLE_FORCE("_ZN2nn5audio17PurgeOutputStreamEPNS0_16OutputStreamTypeE", "drop the frames not yet played", void, h_os_Purge, void *g) {
    ostream *o = os_of(g);
    pthread_mutex_lock(&o->m); os_reclaim(o); pthread_mutex_unlock(&o->m);
    pthread_mutex_lock(&g_aos_lock); au_release_pending(ao(o->key)); pthread_mutex_unlock(&g_aos_lock);
    pthread_mutex_lock(&o->m); os_reclaim(o); pthread_mutex_unlock(&o->m);
}
HLE_FORCE("_ZN2nn5audio17CloseOutputStreamEPNS0_16OutputStreamTypeE", "close the stream's output", void, h_os_Close, void *g) {
    ostream *o = objmap_get(g); if (!o) return;
    au_stop_at(o->key, NULL);
    pthread_mutex_lock(&o->m); os_reclaim(o); pthread_mutex_unlock(&o->m);
    au_close(o->key, dm_bigstack_caller(__builtin_return_address(0)));
    objmap_del(g); pthread_mutex_destroy(&o->m); free(o);
}
HLE_FORCE("_ZN2nn5audio20GetOutputStreamStateEPKNS0_16OutputStreamTypeE", "stream state: 0 not open, 1 stopped, 2 started", int, h_os_State, const void *g) {
    ostream *o = objmap_get(g); return o ? o->state : OS_NONE;
}
HLE_FORCE("_ZN2nn5audio29GetOutputStreamFreeFrameCountEPKNS0_16OutputStreamTypeE", "frames the stream can take: the capacity less the frames queued", int, h_os_Free, const void *g) {
    ostream *o = os_of(g);
    pthread_mutex_lock(&o->m); os_reclaim(o);
    int n = o->nblk >= QMAX - 1 ? 0 : o->cap - o->queued;
    pthread_mutex_unlock(&o->m);
    return n < 0 ? 0 : n;
}
HLE_FORCE("_ZN2nn5audio17WriteOutputStreamEPNS0_16OutputStreamTypeEPKvi", "frames to play: one 16-bit interleaved block on the stream's AudioOut", int, h_os_Write, void *g, const void *data, int frames) {
    ostream *o = os_of(g);
    if (frames <= 0 || !data) return 0;
    pthread_mutex_lock(&o->m); os_reclaim(o);
    if (o->nblk >= QMAX - 1 || frames > o->cap - o->queued) { pthread_mutex_unlock(&o->m); return 0; }   // the game writes only what the free count allows
    oblock *k = malloc(sizeof *k + (size_t)frames * o->ch * 2); if (!k) die("OutputStream block malloc");
    size_t n = (size_t)frames * o->ch;
    if (o->fmt == OS_FLOAT) {
        const float *in = data;
        for (size_t i = 0; i < n; i++) {
            float v = in[i] * 32767.0f;
            k->pcm[i] = (int16_t)(v > 32767.0f ? 32767 : v < -32768.0f ? -32768 : v);
        }
    } else memcpy(k->pcm, data, n * 2);
    k->b.ptr = k->pcm; k->b.cap = k->b.size = (uint64_t)frames * o->ch * 2; k->frames = frames;
    if (!h_au_Append(o->key, &k->b)) { pthread_mutex_unlock(&o->m); free(k); return 0; }
    o->queued += frames; o->nblk++;
    pthread_mutex_unlock(&o->m);
    return frames;
}
HLE_FORCE("_ZN2nn5audio31GetOutputStreamFilledFrameCountEPKNS0_16OutputStreamTypeE", "frames written and not yet played", int, h_os_Filled, const void *g) {
    ostream *o = os_of(g);
    pthread_mutex_lock(&o->m); os_reclaim(o); int n = o->queued; pthread_mutex_unlock(&o->m);
    return n;
}
// exactly 24 bytes: a game keeps them on its stack before the canary. +0x10: glitches, none
typedef struct { int64_t a, b, c; } os_stats;
HLE_FORCE("_ZN2nn5audio25GetOutputStreamStatisticsEPNS0_16OutputStreamTypeEb", "stream statistics: none kept", os_stats, h_os_Stats, void *g, bool reset) {
    (void)g; (void)reset; os_stats s = {0}; return s;
}
typedef struct { int32_t format, channels, rate; } os_format;   // returned in registers
// A capture queue opens only with the microphone enabled and macOS's access granted
HLE_FORCE("_ZN2nn5audio23ListVirtualInputDevicesEPNS0_15AudioDeviceNameEi", "list an input device without prompting; the Mac launcher requests permission before play", int, h_is_List, void *names, int n) {
    if (!names || n <= 0 || !dm_audio_input_available()) return 0;
    dm_audio_input_device_name(names);
    LOG("audio input: ListVirtualInputDevices returns 1 virtual device");
    return 1;
}
HLE_FORCE("_ZN2nn5audio26GetTrueRawAudioInputDeviceEPNS0_15AudioDeviceNameE", "return the host default microphone name", void, h_is_RawDevice, char *name) {
    if (!name) return;
    if (dm_audio_input_available()) dm_audio_input_device_name(name);
    else memset(name, 0, DM_AUDIO_INPUT_NAME_SIZE);
}
HLE_FORCE("_ZN2nn5audio15OpenInputStreamEPNS0_15InputStreamTypeEPvmRKNS0_15AudioDeviceNameERKNS0_12StreamFormatEi", "open a polling queue; hardware start is deferred until StartInputStream", Result, h_is_Open,
          void *g, void *buf, size_t size, const char *dev, const void *fmt, int frames) {
    (void)buf; (void)size;
    if (!g || !dev || !fmt || strncmp(dev, DM_AUDIO_INPUT_DEVICE, sizeof DM_AUDIO_INPUT_DEVICE) || objmap_get(g)) return RES_FAIL;
    os_format f; memcpy(&f, fmt, sizeof f);
    dm_audio_input_stream *s = dm_audio_input_open(f.format, f.channels, f.rate, frames);
    if (!s) { LOG("audio input: OpenInputStream failed (format=%d channels=%d rate=%d capacity=%d)", f.format, f.channels, f.rate, frames); return RES_FAIL; }
    objmap_put(g, s);
    LOG("audio input: OpenInputStream accepted (format=%d channels=%d rate=%d capacity=%d)", f.format, f.channels, f.rate, frames);
    return RES_OK;
}
HLE_FORCE("_ZN2nn5audio16CloseInputStreamEPNS0_15InputStreamTypeE", "close the host microphone stream", void, h_is_Close, void *g) {
    dm_audio_input_stream *s = objmap_del(g); if (s) dm_audio_input_close(s);
}
HLE_FORCE("_ZN2nn5audio16StartInputStreamEPNS0_15InputStreamTypeE", "start the host microphone stream", Result, h_is_Start, void *g) {
    dm_audio_input_stream *s = objmap_get(g); int result = s ? dm_audio_input_start(s) : -1;
    LOG("audio input: StartInputStream %s", result == 0 ? "started" : "failed; capture unavailable");
    return result == 0 ? RES_OK : RES_FAIL;
}
HLE_FORCE("_ZN2nn5audio15StopInputStreamEPNS0_15InputStreamTypeE", "stop the host microphone stream", void, h_is_Stop, void *g) { dm_audio_input_stop(objmap_get(g)); }
HLE_FORCE("_ZN2nn5audio15ReadInputStreamEPNS0_15InputStreamTypeEPvi", "drain captured PCM16 frames from the host microphone", int, h_is_Read, void *g, void *buf, int frames) {
    dm_audio_input_stream *s = objmap_get(g);
    return dm_audio_input_read(s, buf, frames);
}
HLE_FORCE("_ZN2nn5audio30GetInputStreamFilledFrameCountEPKNS0_15InputStreamTypeE", "frames captured and not yet read", int, h_is_Filled, const void *g) { return dm_audio_input_filled(objmap_get(g)); }
// The SDK calls through Impl, null for a stream never opened (SIGSEGV at 0x340); the game queries and purges its unopened microphone
// streams (main+0x336ee7c, playing a song), so an unopened stream answers closed and zero.
HLE_FORCE("_ZN2nn5audio19GetInputStreamStateEPKNS0_15InputStreamTypeE", "input stream state: closed, stopped or started", int, h_is_State, const void *g) { return dm_audio_input_state(objmap_get(g)); }
HLE_FORCE("_ZN2nn5audio16PurgeInputStreamEPNS0_15InputStreamTypeE", "discard captured frames", void, h_is_Purge, void *g) { dm_audio_input_purge(objmap_get(g)); }
HLE_FORCE("_ZN2nn5audio24GetInputStreamStatisticsEPNS0_15InputStreamTypeEb", "input stream statistics: none returned to the guest", os_stats, h_is_Stats, void *g, bool reset) { (void)g; (void)reset; os_stats s = {0}; return s; }

// for the in-process renderer, keyed by a host-allocated dummy object
void *dm_pcmout_open_sink(void) {
    void *key = calloc(1, 64); uint8_t param[8] = {0}; *(uint16_t *)(param + 4) = DM_AUDIO_SINK_CHANNELS;
    if (au_open_at(key, NULL, "AudioRenderer", param, NULL, 1)) die("host PCM output cannot be opened");
    ao(key)->rel_sem = dispatch_semaphore_create(0);
    return key;
}
// spurious returns are fine: the caller checks again
void dm_pcmout_wait(void *k, int ms) { dispatch_semaphore_wait(ao(k)->rel_sem, dispatch_time(DISPATCH_TIME_NOW, (int64_t)ms * 1000000)); }
void dm_pcmout_start(void *k) { au_start_at(k, NULL); }
void dm_pcmout_close(void *k) { au_close(k, NULL); free(k); }
int dm_pcmout_append(void *k, void *b) { return (int)h_au_Append(k, b); }
void *dm_pcmout_released(void *k) { return h_au_Released(k); }

// Reset: close every output still open before the game's memory goes (callbacks read its blocks). Streams are not tracked: the game opens none.
void dm_audio_reset(void) {
    aout *open[AOMAX]; int n = 0;
    pthread_mutex_lock(&g_aos_lock);
    for (int i = 0; i < AOMAX; i++) if (g_aos[i]) open[n++] = g_aos[i];
    pthread_mutex_unlock(&g_aos_lock);
    for (int i = 0; i < n; i++) {
        void *k = open[i]->key;
        int host = open[i]->sink_pcm;
        au_close(k, NULL);
        if (host) free(k);
    }
    if (n) LOG("reset: %d audio outputs closed", n);
}
// The silent sink keeps the real-time pace while suspended
void dm_audio_suspend(int on) {
    pthread_mutex_lock(&g_aos_lock);
    if (g_suspended != on) {
        g_suspended = on;
        if (!on) route_sync(dm_audio_output_channels());   // first: the outputs restart at the route's count
        for (int i = 0; i < AOMAX; i++) {
            aout *a = g_aos[i];
            if (!a || !a->au) continue;
            // otherwise buffers are not released and the game's audio pacing stalls
            if (on) {
                au_halt(a);
                if (a->backend == BK_NONE && atomic_load(&a->state) == STATE_STARTED) null_start(a);
            } else if (a->backend == BK_NULL) au_request_start(a);
        }
        LOG("audio %s", on ? "suspended (background)" : "resumed (foreground)");
    }
    pthread_mutex_unlock(&g_aos_lock);
}
// Under g_aos_lock; a start the driver holds keeps the old unit until it returns. The callback reads dev_ch, set once the old unit is disposed.
static void au_reopen(aout *a, int route) {
    au_halt(a);
    if (a->backend == BK_NONE && atomic_load(&a->state) == STATE_STARTED) null_start(a);
    if (a->start_phase == AU_CANCELED) a->reset_pending = 1;
    else { au_replace(a, route); if (a->backend == BK_NULL) au_request_start(a); }
}
// under g_aos_lock
static void au_remake(aout *a, int route) {
    if (au_create(a, route)) return;
    a->unit_lost = 0;
    LOG("audio #%d: the output has an AudioUnit again after the route change (%d channels)", a->id, a->dev_ch);
    if (a->backend == BK_NULL) au_request_start(a);
}
// A six-channel output whose unit's count differs from the route's gets a new unit (Core Audio's 6 -> 2 drops center and back).
// Under g_aos_lock with the route read under it; not while suspended (resume syncs). Guest outputs of other counts keep theirs.
static void route_sync(int route) {
    if (g_suspended) return;
    for (int i = 0; i < AOMAX; i++) {
        aout *a = g_aos[i]; if (!a || a->reset_pending) continue;
        if (!a->au) { if (a->unit_lost) au_remake(a, route); continue; }
        int want = unit_channels(a, route);
        if (want == a->dev_ch) continue;
        LOG("audio #%d: the output route takes %d channels now: the AudioUnit is replaced (%d → %d channels%s)", a->id, route, a->dev_ch, want, want == 2 && (a->sink_pcm || a->ch == 6) ? ", stereo fold by the host" : "");
        au_reopen(a, route);
    }
}
static void dm_audio_route_changed(void) {
    pthread_mutex_lock(&g_aos_lock);
    int route = dm_audio_output_channels();
    route_sync(route);
    pthread_mutex_unlock(&g_aos_lock);
}
