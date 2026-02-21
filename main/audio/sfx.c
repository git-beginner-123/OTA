#include "audio/sfx.h"
#include "audio/audio_engine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdbool.h>

#define SFX_SAMPLE_RATE   16000
#define SFX_WRITE_TIMEOUT_MS 1000
#define SFX_QUEUE_LEN 64
#define SFX_ENABLED 1

typedef enum {
    kSfxEvtSuccess = 1,
    kSfxEvtFailure = 2,
    kSfxEvtNotify = 3,
    kSfxEvtStop = 4,
} SfxEvent;

static const char* TAG = "SFX";

static QueueHandle_t s_evt_q = NULL;
static TaskHandle_t s_task = NULL;
static bool s_inited = false;
static volatile bool s_stopping = false;
static bool s_warned_audio_not_ready = false;
static AudioEngineSession s_sfx_session = {0};

static void sfx_wait_task_exit(uint32_t wait_ms)
{
    uint32_t waited = 0;
    while (s_task && waited < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

static void sfx_play_square(uint32_t freq_hz, uint32_t dur_ms, int16_t amp)
{
    (void)AudioEngine_PlaySquareMs(&s_sfx_session, SFX_SAMPLE_RATE, freq_hz, dur_ms,
                                   amp, SFX_WRITE_TIMEOUT_MS);
}

static void sfx_play_silence(uint32_t dur_ms)
{
    (void)AudioEngine_PlaySilenceMs(&s_sfx_session, SFX_SAMPLE_RATE, dur_ms,
                                    SFX_WRITE_TIMEOUT_MS);
}

static void sfx_play_success(void)
{
    // Fanfare-like simplified cadence: 5-5-6-5-1 (G-G-A-G-C)
    sfx_play_square(392, 65, 5600);  // G4
    sfx_play_silence(10);
    sfx_play_square(392, 65, 5600);  // G4
    sfx_play_silence(10);
    sfx_play_square(440, 75, 5600);  // A4
    sfx_play_silence(10);
    sfx_play_square(392, 80, 5600);  // G4
    sfx_play_silence(10);
    sfx_play_square(523, 130, 5600); // C5
    sfx_play_silence(8);
}

static void sfx_play_failure(void)
{
    // Minor-like descending ending: 6-5-3-2-1 with longer tail.
    sfx_play_square(440, 60, 5200);  // A4
    sfx_play_silence(8);
    sfx_play_square(392, 60, 5200);  // G4
    sfx_play_silence(8);
    sfx_play_square(330, 70, 5200);  // E4
    sfx_play_silence(8);
    sfx_play_square(294, 80, 5200);  // D4
    sfx_play_silence(10);
    sfx_play_square(262, 180, 5200); // C4
    sfx_play_silence(8);
}

static void sfx_play_notify(void)
{
    // Ding-dong: 5 -> 1 (G -> C)
    sfx_play_square(392, 70, 5200);  // G4
    sfx_play_silence(12);
    sfx_play_square(262, 110, 5200); // C4
    sfx_play_silence(6);
}

static void sfx_task(void* arg)
{
    (void)arg;
    SfxEvent ev = 0;
    while (xQueueReceive(s_evt_q, &ev, portMAX_DELAY) == pdTRUE) {
        if (ev == kSfxEvtStop) break;

        bool ready = AudioEngine_Open(&s_sfx_session, kAudioEngineSfx, 80, UINT32_MAX, &s_stopping);
        if (!ready) {
            if (!s_warned_audio_not_ready) {
                ESP_LOGW(TAG, "audio engine busy/not ready, SFX skipped");
                s_warned_audio_not_ready = true;
            }
            continue;
        }

        if (ev == kSfxEvtSuccess) sfx_play_success();
        else if (ev == kSfxEvtFailure) sfx_play_failure();
        else if (ev == kSfxEvtNotify) sfx_play_notify();

        // Write a short silence tail before closing output to reduce pop noise.
        (void)AudioEngine_PlaySilenceMs(&s_sfx_session, SFX_SAMPLE_RATE, 8, SFX_WRITE_TIMEOUT_MS);
        AudioEngine_Close(&s_sfx_session);
    }

    s_task = NULL;
    vTaskDelete(NULL);
}

void Sfx_Init(void)
{
    if (!SFX_ENABLED) return;
    if (s_inited) return;

    s_stopping = false;
    s_warned_audio_not_ready = false;
    s_evt_q = xQueueCreate(SFX_QUEUE_LEN, sizeof(SfxEvent));
    if (!s_evt_q) {
        ESP_LOGW(TAG, "queue create failed");
        return;
    }

    if (xTaskCreate(sfx_task, "sfx_task", 3072, NULL, 4, &s_task) != pdPASS) {
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
        ESP_LOGW(TAG, "task create failed");
        return;
    }

    s_inited = true;
}

void Sfx_Deinit(void)
{
    if (!SFX_ENABLED) return;
    if (!s_inited) return;
    s_stopping = true;

    SfxEvent ev = kSfxEvtStop;
    if (s_evt_q) xQueueSend(s_evt_q, &ev, pdMS_TO_TICKS(20));
    sfx_wait_task_exit(300);
    if (s_task) {
        ESP_LOGW(TAG, "sfx task exit timeout");
        return;
    }

    if (s_evt_q) {
        vQueueDelete(s_evt_q);
        s_evt_q = NULL;
    }
    s_inited = false;
}

void Sfx_PlayKey(void)
{
    // Keep key click mapped to notify for compatibility.
    Sfx_PlayNotify();
}

static void sfx_send_event(SfxEvent ev)
{
    if (!SFX_ENABLED) return;
    if (!s_inited) Sfx_Init();
    if (!s_inited || !s_evt_q) return;
    xQueueSend(s_evt_q, &ev, portMAX_DELAY);
}

void Sfx_PlayNotify(void)
{
    sfx_send_event(kSfxEvtNotify);
}

void Sfx_PlayFailure(void)
{
    sfx_send_event(kSfxEvtFailure);
}

void Sfx_PlaySuccess(void)
{
    sfx_send_event(kSfxEvtSuccess);
}

void Sfx_PlayVictory(void)
{
    // Backward compatible alias.
    Sfx_PlaySuccess();
}
