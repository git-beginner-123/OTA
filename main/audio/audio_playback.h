#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Application-layer audio gate:
// - Init at boot
// - Begin/End around playback to avoid concurrent multi-app output.
bool AudioPlayback_Init(void);
void AudioPlayback_Deinit(void);
bool AudioPlayback_Begin(uint32_t lock_timeout_ms);
void AudioPlayback_End(void);

bool AudioPlayback_WriteRetry(const int16_t* samples, size_t sample_count,
                              uint32_t timeout_ms, volatile bool* stop_flag);

bool AudioPlayback_PlaySilenceMs(uint32_t sample_rate, uint32_t dur_ms,
                                 uint32_t timeout_ms, volatile bool* stop_flag);

bool AudioPlayback_PlaySquareMs(uint32_t sample_rate, uint32_t freq_hz, uint32_t dur_ms,
                                int16_t amp, uint32_t timeout_ms, volatile bool* stop_flag);

bool AudioPlayback_PlayPcmS16Le(const uint8_t* pcm_bytes, size_t pcm_len_bytes,
                                size_t chunk_bytes, const volatile int32_t* gain_q15,
                                uint32_t timeout_ms, volatile bool* stop_flag);
