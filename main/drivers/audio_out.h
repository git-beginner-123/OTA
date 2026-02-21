#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool AudioOut_Init(void);
void AudioOut_Deinit(void);

bool AudioOut_SetSampleRateHz(uint32_t sample_rate_hz, uint32_t timeout_ms);
uint32_t AudioOut_GetSampleRateHz(void);

void AudioOut_SetVolumePercent(int volume_pct);
int AudioOut_GetVolumePercent(void);

bool AudioOut_Write(const int16_t* samples, size_t sample_count, uint32_t timeout_ms);
