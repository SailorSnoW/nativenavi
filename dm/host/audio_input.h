#ifndef DM_AUDIO_INPUT_H
#define DM_AUDIO_INPUT_H

#include <stdint.h>

typedef struct dm_audio_input_stream dm_audio_input_stream;

enum { DM_AUDIO_INPUT_PCM16 = 2, DM_AUDIO_INPUT_CHANNELS = 1, DM_AUDIO_INPUT_RATE = 48000, DM_AUDIO_INPUT_NAME_SIZE = 0x100 };

#define DM_AUDIO_INPUT_DEVICE "AudioVirtualInput"

// Platform hooks (audio_input_mac.m; the tests have their own).
int dm_audio_input_platform_device_available(void); // Checks hardware and granted permission without prompting.
int dm_audio_input_platform_prepare(void);     // Prepares capture only when launcher permission is granted; never prompts.
int dm_audio_input_platform_start(void);
void dm_audio_input_platform_close(void);
int dm_audio_input_platform_failed(void);
int dm_audio_input_platform_route_changed(void);
void dm_audio_input_platform_info(char name[0x100], char uid[0x100], double *sample_rate, int *channels);

int dm_audio_input_available(void);
void dm_audio_input_device_name(char out[DM_AUDIO_INPUT_NAME_SIZE]);
dm_audio_input_stream *dm_audio_input_open(int format, int channels, int rate, int capacity_frames);
void dm_audio_input_close(dm_audio_input_stream *stream);
int dm_audio_input_start(dm_audio_input_stream *stream);
void dm_audio_input_stop(dm_audio_input_stream *stream);
void dm_audio_input_purge(dm_audio_input_stream *stream);
int dm_audio_input_state(const dm_audio_input_stream *stream);
int dm_audio_input_filled(const dm_audio_input_stream *stream);
int dm_audio_input_read(dm_audio_input_stream *stream, void *dst, int frames);

// The platform callback supplies planar Float32 samples at the device sample rate.
// Conversion and fan-out use preallocated queues and do not take locks.
void dm_audio_input_process(const float *const *channels, int channel_count, uint32_t frames, double sample_rate);
void dm_audio_input_route_changed(void);

#endif
