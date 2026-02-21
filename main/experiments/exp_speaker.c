#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/audio_engine.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include <stdint.h>

extern const uint8_t _binary_hola_es_pcm_start[] asm("_binary_hola_es_pcm_start");
extern const uint8_t _binary_hola_es_pcm_end[]   asm("_binary_hola_es_pcm_end");

static const char* TAG = "EXP_SPK";

static TaskHandle_t s_play_task = NULL;
static bool s_running = false;
static bool s_playing = false;
static volatile bool s_stop = false;
static int s_vol_pct = 35;
static const volatile int32_t kVoiceHeadroomQ15 = 11626; // ~0.3548 (-9 dB)
static AudioEngineSession s_spk_session = {0};
static volatile int s_progress_pct = 0;
static int s_last_draw_progress = -1;
static bool s_last_draw_playing = false;
static uint32_t s_sample_rate_hz = 16000;

#define VOL_STEP_PCT 5
#define SPEAKER_AUDIO_ENABLED 1
#define SPK_OPEN_TIMEOUT_MS 200
#define SPK_RECOVER_OPEN_TIMEOUT_MS 40
#define SPK_WRITE_TIMEOUT_MS 800

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void wait_play_task_exit(uint32_t wait_ms)
{
    uint32_t waited = 0;
    while (s_play_task && waited < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

static void spk_play_task(void* arg)
{
    (void)arg;
    if (!AudioEngine_Open(&s_spk_session, kAudioEngineVoice, s_vol_pct, SPK_OPEN_TIMEOUT_MS, &s_stop)) {
        ESP_LOGW(TAG, "speaker open failed in play task");
        s_playing = false;
        s_play_task = NULL;
        vTaskDelete(NULL);
        return;
    }

    const uint8_t* pcm = _binary_hola_es_pcm_start;
    size_t total = (size_t)(_binary_hola_es_pcm_end - _binary_hola_es_pcm_start);
    size_t p = 0;
    int16_t temp[512];
    int write_fail_streak = 0;
    bool finished = false;

    s_progress_pct = 0;
    while (!s_stop && p + 1 < total) {
        size_t nbytes = total - p;
        if (nbytes > sizeof(temp)) nbytes = sizeof(temp);
        nbytes &= ~((size_t)1);
        if (nbytes == 0) break;

        int samples = (int)(nbytes / 2);
        const int16_t* in = (const int16_t*)(pcm + p);
        int32_t gain = kVoiceHeadroomQ15;
        for (int i = 0; i < samples; i++) {
            int32_t v = (int32_t)in[i] * gain;
            v >>= 15;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            temp[i] = (int16_t)v;
        }

        if (!AudioEngine_WriteS16(&s_spk_session, temp, (size_t)samples, SPK_WRITE_TIMEOUT_MS)) {
            write_fail_streak++;
            ESP_LOGW(TAG, "speaker write fail #%d, try recover", write_fail_streak);
            AudioEngine_Close(&s_spk_session);
            vTaskDelay(pdMS_TO_TICKS(5));
            if (!AudioEngine_Open(&s_spk_session, kAudioEngineVoice, s_vol_pct, SPK_RECOVER_OPEN_TIMEOUT_MS, &s_stop)) {
                ESP_LOGW(TAG, "speaker recover open failed");
                if (write_fail_streak >= 8) {
                    ESP_LOGW(TAG, "speaker write failed repeatedly, abort playback");
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            } else {
                write_fail_streak = 0;
            }
            continue;
        }
        write_fail_streak = 0;

        p += nbytes;
        int pct = (int)((p * 100U) / (total ? total : 1U));
        if (pct > 100) pct = 100;
        s_progress_pct = pct;
    }
    if (!s_stop && p + 1 >= total) {
        finished = true;
    }
    // Prevent TX-underflow tail from sounding like a sustained vowel/noise.
    (void)AudioEngine_PlaySilenceMs(&s_spk_session, s_sample_rate_hz, 40, SPK_WRITE_TIMEOUT_MS);
    if (finished) s_progress_pct = 100;

    AudioEngine_Close(&s_spk_session);
    s_playing = false;
    s_play_task = NULL;
    vTaskDelete(NULL);
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("SPK", "OK:START  BACK");
    Ui_Println("Goal: play PCM by I2S.");
    Ui_Println("MAX98357 output.");
    Ui_Println("DIN->7 BCLK->15 WS->16");
    Ui_Println("UP/DN set volume.");
    Ui_Println("OK play/stop.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
    if (s_running) {
        s_stop = true;
        wait_play_task_exit(600);
        if (s_play_task) {
            ESP_LOGW(TAG, "speaker task exit timeout");
            return;
        }
        s_playing = false;
        s_running = false;
    }
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");

    if (s_running) return;

    if (!SPEAKER_AUDIO_ENABLED) {
        ESP_LOGW(TAG, "speaker audio disabled for test");
        Ui_DrawFrame("SPK", "AUDIO OFF (TEST)");
        Ui_DrawBodyClear();
        Ui_Println("Audio disabled for testing.");
        Ui_Println("Enable SPEAKER_AUDIO_ENABLED to test.");
        return;
    }

    s_vol_pct = 35;
    s_stop = false;

    s_running = true;
    s_playing = false;
    s_progress_pct = 0;
    s_last_draw_progress = -1;
    s_last_draw_playing = false;
    s_sample_rate_hz = 16000U;
    (void)AudioEngine_SetOutputSampleRateHz(s_sample_rate_hz, SPK_OPEN_TIMEOUT_MS);

    Ui_DrawFrame("SPK", "DN:-  UP:+  OK:PLAY  BACK");
    Ui_DrawSpeakerBody(s_playing, s_vol_pct, s_progress_pct, s_sample_rate_hz);
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    if (!s_running) return;

    s_stop = true;
    wait_play_task_exit(600);
    if (s_play_task) {
        ESP_LOGW(TAG, "speaker task exit timeout");
        return;
    }
    s_playing = false;
    s_progress_pct = 0;
    s_running = false;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    bool changed = false;

    if (key == kInputUp) {
        s_vol_pct = clamp_int(s_vol_pct + VOL_STEP_PCT, 0, 100);
        if (s_spk_session.open) AudioEngine_SetSessionVolume(&s_spk_session, s_vol_pct);
        changed = true;
    } else if (key == kInputDown) {
        s_vol_pct = clamp_int(s_vol_pct - VOL_STEP_PCT, 0, 100);
        if (s_spk_session.open) AudioEngine_SetSessionVolume(&s_spk_session, s_vol_pct);
        changed = true;
    } else if (key == kInputEnter) {
        if (s_playing) {
            s_stop = true;
            wait_play_task_exit(600);
            if (s_play_task) {
                ESP_LOGW(TAG, "speaker task exit timeout");
                return;
            }
            s_playing = false;
            s_progress_pct = 0;
        } else {
            s_stop = false;
            s_playing = true;
            s_progress_pct = 0;
            xTaskCreate(spk_play_task, "spk_play", 4096, NULL, 5, &s_play_task);
        }
        changed = true;
    } else if (key == kInputBack) {
        return;
    }

    if (changed) {
        Ui_DrawSpeakerBody(s_playing, s_vol_pct, s_progress_pct, s_sample_rate_hz);
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running) return;
    int prog = s_progress_pct;
    bool play = s_playing;
    if (prog != s_last_draw_progress || play != s_last_draw_playing) {
        s_last_draw_progress = prog;
        s_last_draw_playing = play;
        Ui_DrawSpeakerBody(play, s_vol_pct, prog, s_sample_rate_hz);
    }
}

const Experiment g_exp_speaker = {
    .id = 7,
    .title = "SPK",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
