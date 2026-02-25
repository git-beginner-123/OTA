#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    kAudioEngineVoice = 0,
    kAudioEngineSfx,
    kAudioEngineMusic,
    kAudioEngineStream,
} AudioEngineSource;

typedef struct {
    bool open;
    AudioEngineSource source;
    int volume_pct;
    volatile bool* stop_flag;
} AudioEngineSession;

bool AudioEngine_Init(void);
void AudioEngine_Deinit(void);
void AudioEngine_SetMasterVolumePercent(int volume_pct);
int AudioEngine_GetMasterVolumePercent(void);

bool AudioEngine_Open(AudioEngineSession* session, AudioEngineSource source,
                      int volume_pct, uint32_t lock_timeout_ms,
                      volatile bool* stop_flag);
void AudioEngine_Close(AudioEngineSession* session);

void AudioEngine_SetSessionVolume(AudioEngineSession* session, int volume_pct);
int AudioEngine_GetSessionVolume(const AudioEngineSession* session);
bool AudioEngine_SetOutputSampleRateHz(uint32_t sample_rate_hz, uint32_t timeout_ms);
uint32_t AudioEngine_GetOutputSampleRateHz(void);

bool AudioEngine_WriteS16(AudioEngineSession* session, const int16_t* samples,
                          size_t sample_count, uint32_t timeout_ms);

bool AudioEngine_PlaySilenceMs(AudioEngineSession* session, uint32_t sample_rate,
                               uint32_t dur_ms, uint32_t timeout_ms);

bool AudioEngine_PlaySquareMs(AudioEngineSession* session, uint32_t sample_rate,
                              uint32_t freq_hz, uint32_t dur_ms, int16_t amp,
                              uint32_t timeout_ms);

bool AudioEngine_PlayPcmS16Le(AudioEngineSession* session, const uint8_t* pcm_bytes,
                              size_t pcm_len_bytes, size_t chunk_bytes,
                              const volatile int32_t* gain_q15,
                              uint32_t timeout_ms);
