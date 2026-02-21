#include "audio/audio_engine.h"

#include "drivers/audio_out.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include <limits.h>

static SemaphoreHandle_t s_engine_lock = NULL;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;

static inline bool should_stop(const AudioEngineSession* session)
{
    return (session && session->stop_flag && *session->stop_flag);
}

static int clamp_pct(int v)
{
    if (v < 0) return 0;
    if (v > 100) return 100;
    return v;
}

bool AudioEngine_Init(void)
{
    if (s_engine_lock) return true;

    taskENTER_CRITICAL(&s_init_mux);
    if (!s_engine_lock) {
        s_engine_lock = xSemaphoreCreateRecursiveMutex();
    }
    taskEXIT_CRITICAL(&s_init_mux);
    return (s_engine_lock != NULL);
}

void AudioEngine_Deinit(void)
{
    taskENTER_CRITICAL(&s_init_mux);
    SemaphoreHandle_t lock = s_engine_lock;
    s_engine_lock = NULL;
    taskEXIT_CRITICAL(&s_init_mux);

    if (lock) {
        vSemaphoreDelete(lock);
    }
}

bool AudioEngine_Open(AudioEngineSession* session, AudioEngineSource source,
                      int volume_pct, uint32_t lock_timeout_ms,
                      volatile bool* stop_flag)
{
    if (!session) return false;
    if (!AudioEngine_Init() || !s_engine_lock) return false;

    TickType_t tmo = (lock_timeout_ms == UINT32_MAX)
                         ? portMAX_DELAY
                         : pdMS_TO_TICKS(lock_timeout_ms);
    if (xSemaphoreTakeRecursive(s_engine_lock, tmo) != pdTRUE) return false;

    if (!AudioOut_Init()) {
        (void)xSemaphoreGiveRecursive(s_engine_lock);
        return false;
    }

    volume_pct = clamp_pct(volume_pct);
    AudioOut_SetVolumePercent(volume_pct);

    session->open = true;
    session->source = source;
    session->volume_pct = volume_pct;
    session->stop_flag = stop_flag;
    return true;
}

void AudioEngine_Close(AudioEngineSession* session)
{
    if (!session || !session->open) return;

    AudioOut_Deinit();
    session->open = false;
    (void)xSemaphoreGiveRecursive(s_engine_lock);
}

void AudioEngine_SetSessionVolume(AudioEngineSession* session, int volume_pct)
{
    if (!session || !session->open) return;
    volume_pct = clamp_pct(volume_pct);
    session->volume_pct = volume_pct;
    AudioOut_SetVolumePercent(volume_pct);
}

int AudioEngine_GetSessionVolume(const AudioEngineSession* session)
{
    if (!session) return 0;
    return session->volume_pct;
}

bool AudioEngine_SetOutputSampleRateHz(uint32_t sample_rate_hz, uint32_t timeout_ms)
{
    if (!AudioEngine_Init() || !s_engine_lock) return false;

    TickType_t tmo = (timeout_ms == UINT32_MAX)
                         ? portMAX_DELAY
                         : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTakeRecursive(s_engine_lock, tmo) != pdTRUE) return false;

    bool ok = AudioOut_SetSampleRateHz(sample_rate_hz, timeout_ms);
    (void)xSemaphoreGiveRecursive(s_engine_lock);
    return ok;
}

uint32_t AudioEngine_GetOutputSampleRateHz(void)
{
    return AudioOut_GetSampleRateHz();
}

bool AudioEngine_WriteS16(AudioEngineSession* session, const int16_t* samples,
                          size_t sample_count, uint32_t timeout_ms)
{
    if (!session || !session->open || !samples || sample_count == 0) return false;

    // Avoid endless retry loops that can stall progress/UI at a fixed percent.
    for (int tries = 0; tries < 8 && !should_stop(session); tries++) {
        if (AudioOut_Write(samples, sample_count, timeout_ms)) return true;
        vTaskDelay(pdMS_TO_TICKS(1));
    }
    return false;
}

bool AudioEngine_PlaySilenceMs(AudioEngineSession* session, uint32_t sample_rate,
                               uint32_t dur_ms, uint32_t timeout_ms)
{
    if (!session || !session->open) return false;
    if (dur_ms == 0) return true;
    if (sample_rate == 0) return false;

    int32_t total = (int32_t)((sample_rate * dur_ms) / 1000U);
    int16_t buf[64] = {0};

    while (total > 0 && !should_stop(session)) {
        int n = (total > (int32_t)(sizeof(buf) / sizeof(buf[0]))) ? (int)(sizeof(buf) / sizeof(buf[0])) : (int)total;
        if (!AudioEngine_WriteS16(session, buf, (size_t)n, timeout_ms)) return false;
        total -= n;
    }
    return !should_stop(session);
}

bool AudioEngine_PlaySquareMs(AudioEngineSession* session, uint32_t sample_rate,
                              uint32_t freq_hz, uint32_t dur_ms, int16_t amp,
                              uint32_t timeout_ms)
{
    if (!session || !session->open) return false;
    if (dur_ms == 0) return true;
    if (sample_rate == 0) return false;
    if (freq_hz == 0) {
        return AudioEngine_PlaySilenceMs(session, sample_rate, dur_ms, timeout_ms);
    }

    int32_t total = (int32_t)((sample_rate * dur_ms) / 1000U);
    if (total < 1) return true;

    int32_t period = (int32_t)(sample_rate / freq_hz);
    if (period < 2) period = 2;
    int32_t half = period / 2;
    if (half < 1) half = 1;

    int16_t buf[128];
    int32_t pos = 0;
    while (total > 0 && !should_stop(session)) {
        int n = (total > (int32_t)(sizeof(buf) / sizeof(buf[0]))) ? (int)(sizeof(buf) / sizeof(buf[0])) : (int)total;
        for (int i = 0; i < n; i++) {
            int32_t in_period = pos % period;
            buf[i] = (in_period < half) ? amp : (int16_t)-amp;
            pos++;
        }
        if (!AudioEngine_WriteS16(session, buf, (size_t)n, timeout_ms)) return false;
        total -= n;
    }
    return !should_stop(session);
}

bool AudioEngine_PlayPcmS16Le(AudioEngineSession* session, const uint8_t* pcm_bytes,
                              size_t pcm_len_bytes, size_t chunk_bytes,
                              const volatile int32_t* gain_q15,
                              uint32_t timeout_ms)
{
    if (!session || !session->open) return false;
    if (!pcm_bytes || pcm_len_bytes < 2) return false;

    if ((chunk_bytes & 1U) != 0U) chunk_bytes--;
    if (chunk_bytes < 2) chunk_bytes = 2048;

    int16_t temp[1024];
    size_t p = 0;

    while (!should_stop(session) && p < pcm_len_bytes) {
        size_t nbytes = pcm_len_bytes - p;
        if (nbytes > chunk_bytes) nbytes = chunk_bytes;
        if (nbytes > sizeof(temp)) nbytes = sizeof(temp);
        nbytes &= ~((size_t)1);

        int samples = (int)(nbytes / 2);
        const int16_t* in = (const int16_t*)(pcm_bytes + p);
        int32_t gain = gain_q15 ? *gain_q15 : 32767;
        if (gain < 0) gain = 0;
        if (gain > 32767) gain = 32767;

        for (int i = 0; i < samples; i++) {
            int32_t v = (int32_t)in[i] * gain;
            v >>= 15;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            temp[i] = (int16_t)v;
        }

        if (!AudioEngine_WriteS16(session, temp, (size_t)samples, timeout_ms)) return false;
        p += nbytes;
    }

    return !should_stop(session);
}
