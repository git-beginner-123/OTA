#include "audio/audio_playback.h"

#include "audio/audio_engine.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include <limits.h>

static SemaphoreHandle_t s_api_lock = NULL;
static portMUX_TYPE s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static AudioEngineSession s_legacy_session = {0};
static int s_depth = 0;

bool AudioPlayback_Init(void)
{
    if (s_api_lock) return true;

    taskENTER_CRITICAL(&s_init_mux);
    if (!s_api_lock) {
        s_api_lock = xSemaphoreCreateRecursiveMutex();
    }
    taskEXIT_CRITICAL(&s_init_mux);

    if (!s_api_lock) return false;
    return AudioEngine_Init();
}

void AudioPlayback_Deinit(void)
{
    taskENTER_CRITICAL(&s_init_mux);
    SemaphoreHandle_t lock = s_api_lock;
    s_api_lock = NULL;
    taskEXIT_CRITICAL(&s_init_mux);

    if (lock) {
        vSemaphoreDelete(lock);
    }
}

bool AudioPlayback_Begin(uint32_t lock_timeout_ms)
{
    if (!AudioPlayback_Init() || !s_api_lock) return false;

    TickType_t tmo = (lock_timeout_ms == UINT32_MAX)
                         ? portMAX_DELAY
                         : pdMS_TO_TICKS(lock_timeout_ms);
    if (xSemaphoreTakeRecursive(s_api_lock, tmo) != pdTRUE) return false;

    if (s_depth == 0) {
        if (!AudioEngine_Open(&s_legacy_session, kAudioEngineStream, 100, lock_timeout_ms, NULL)) {
            (void)xSemaphoreGiveRecursive(s_api_lock);
            return false;
        }
    }
    s_depth++;
    return true;
}

void AudioPlayback_End(void)
{
    if (!s_api_lock) return;
    if (s_depth > 0) {
        s_depth--;
        if (s_depth == 0) {
            AudioEngine_Close(&s_legacy_session);
        }
    }
    (void)xSemaphoreGiveRecursive(s_api_lock);
}

static inline bool should_stop(volatile bool* stop_flag)
{
    return (stop_flag && *stop_flag);
}

bool AudioPlayback_WriteRetry(const int16_t* samples, size_t sample_count,
                              uint32_t timeout_ms, volatile bool* stop_flag)
{
    if (!samples || sample_count == 0) return false;
    if (!AudioPlayback_Begin(UINT32_MAX)) return false;

    bool ok = false;
    while (!should_stop(stop_flag)) {
        if (AudioEngine_WriteS16(&s_legacy_session, samples, sample_count, timeout_ms)) {
            ok = true;
            break;
        }
    }

    AudioPlayback_End();
    return ok;
}

bool AudioPlayback_PlaySilenceMs(uint32_t sample_rate, uint32_t dur_ms,
                                 uint32_t timeout_ms, volatile bool* stop_flag)
{
    if (!AudioPlayback_Begin(UINT32_MAX)) return false;
    s_legacy_session.stop_flag = stop_flag;
    bool ok = AudioEngine_PlaySilenceMs(&s_legacy_session, sample_rate, dur_ms, timeout_ms);
    s_legacy_session.stop_flag = NULL;
    AudioPlayback_End();
    return ok;
}

bool AudioPlayback_PlaySquareMs(uint32_t sample_rate, uint32_t freq_hz, uint32_t dur_ms,
                                int16_t amp, uint32_t timeout_ms, volatile bool* stop_flag)
{
    if (!AudioPlayback_Begin(UINT32_MAX)) return false;
    s_legacy_session.stop_flag = stop_flag;
    bool ok = AudioEngine_PlaySquareMs(&s_legacy_session, sample_rate, freq_hz, dur_ms, amp, timeout_ms);
    s_legacy_session.stop_flag = NULL;
    AudioPlayback_End();
    return ok;
}

bool AudioPlayback_PlayPcmS16Le(const uint8_t* pcm_bytes, size_t pcm_len_bytes,
                                size_t chunk_bytes, const volatile int32_t* gain_q15,
                                uint32_t timeout_ms, volatile bool* stop_flag)
{
    if (!AudioPlayback_Begin(UINT32_MAX)) return false;
    s_legacy_session.stop_flag = stop_flag;
    bool ok = AudioEngine_PlayPcmS16Le(&s_legacy_session, pcm_bytes, pcm_len_bytes,
                                       chunk_bytes, gain_q15, timeout_ms);
    s_legacy_session.stop_flag = NULL;
    AudioPlayback_End();
    return ok;
}

