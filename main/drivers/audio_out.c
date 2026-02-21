#include "drivers/audio_out.h"

#include "driver/i2s_std.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_err.h"

// MAX98357 pins
#define AUDIO_I2S_PORT     I2S_NUM_1
#define AUDIO_SAMPLE_RATE_DEFAULT  16000
#define AUDIO_BITS         I2S_DATA_BIT_WIDTH_16BIT
#define AUDIO_DIN_GPIO     7
#define AUDIO_BCLK_GPIO    15
#define AUDIO_LRCLK_GPIO   16

static const char* TAG = "AUDIO_OUT";

static i2s_chan_handle_t s_tx_chan = NULL;
static SemaphoreHandle_t s_io_lock = NULL;
static int s_ref_count = 0;
static volatile int32_t s_volume_q15 = 32767;
static volatile int s_volume_pct = 100;
static volatile uint32_t s_sample_rate_hz = AUDIO_SAMPLE_RATE_DEFAULT;
// Global master attenuation: -6 dB (about 0.501x)
static const int32_t k_master_gain_q15 = 16423;

static bool audio_driver_start(void)
{
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_PORT, I2S_ROLE_MASTER);
    esp_err_t r = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "i2s_new_channel failed: %s", esp_err_to_name(r));
        s_tx_chan = NULL;
        return false;
    }

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(s_sample_rate_hz),
        .slot_cfg = I2S_STD_MSB_SLOT_DEFAULT_CONFIG(AUDIO_BITS, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED,
            .bclk = AUDIO_BCLK_GPIO,
            .ws = AUDIO_LRCLK_GPIO,
            .dout = AUDIO_DIN_GPIO,
            .din = I2S_GPIO_UNUSED,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.slot_cfg.slot_mask = I2S_STD_SLOT_LEFT;

    r = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(r));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return false;
    }

    r = i2s_channel_enable(s_tx_chan);
    if (r != ESP_OK) {
        ESP_LOGW(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(r));
        i2s_del_channel(s_tx_chan);
        s_tx_chan = NULL;
        return false;
    }

    return true;
}

static void audio_driver_stop(void)
{
    if (!s_tx_chan) return;
    i2s_channel_disable(s_tx_chan);
    i2s_del_channel(s_tx_chan);
    s_tx_chan = NULL;
}

static bool audio_driver_restart_locked(void)
{
    audio_driver_stop();
    return audio_driver_start();
}

bool AudioOut_Init(void)
{
    if (s_ref_count > 0) {
        s_ref_count++;
        return true;
    }

    if (!audio_driver_start()) return false;

    s_io_lock = xSemaphoreCreateMutex();
    if (!s_io_lock) {
        audio_driver_stop();
        return false;
    }

    s_ref_count = 1;
    return true;
}

void AudioOut_Deinit(void)
{
    if (s_ref_count <= 0) return;
    s_ref_count--;
    if (s_ref_count > 0) return;

    if (s_io_lock) {
        vSemaphoreDelete(s_io_lock);
        s_io_lock = NULL;
    }
    audio_driver_stop();
}

static uint32_t clamp_sample_rate_hz(uint32_t hz)
{
    if (hz < 8000U) return 8000U;
    if (hz > 48000U) return 48000U;
    return hz;
}

bool AudioOut_SetSampleRateHz(uint32_t sample_rate_hz, uint32_t timeout_ms)
{
    sample_rate_hz = clamp_sample_rate_hz(sample_rate_hz);
    if (s_sample_rate_hz == sample_rate_hz) return true;

    if (!s_io_lock) {
        s_sample_rate_hz = sample_rate_hz;
        return true;
    }

    TickType_t tmo = pdMS_TO_TICKS(timeout_ms);
    if (tmo == 0) tmo = 1;
    if (xSemaphoreTake(s_io_lock, tmo) != pdTRUE) return false;

    s_sample_rate_hz = sample_rate_hz;
    bool ok = true;
    if (s_tx_chan) {
        ok = audio_driver_restart_locked();
    }

    xSemaphoreGive(s_io_lock);
    return ok;
}

uint32_t AudioOut_GetSampleRateHz(void)
{
    return s_sample_rate_hz;
}

void AudioOut_SetVolumePercent(int volume_pct)
{
    if (volume_pct < 0) volume_pct = 0;
    if (volume_pct > 100) volume_pct = 100;
    s_volume_pct = volume_pct;
    s_volume_q15 = (int32_t)((volume_pct * 32767) / 100);
}

int AudioOut_GetVolumePercent(void)
{
    return s_volume_pct;
}

bool AudioOut_Write(const int16_t* samples, size_t sample_count, uint32_t timeout_ms)
{
    if (!samples || sample_count == 0 || !s_tx_chan || !s_io_lock) return false;

    TickType_t tmo = pdMS_TO_TICKS(timeout_ms);
    if (tmo == 0) tmo = 1;
    if (xSemaphoreTake(s_io_lock, tmo) != pdTRUE) return false;

    size_t left = sample_count;
    const int16_t* p = samples;
    bool ok = true;
    bool restarted_once = false;

    int16_t mono_buf[256];

    while (left > 0) {
        size_t mono_chunk = left;
        if (mono_chunk > 256) mono_chunk = 256;
        int32_t vol_q15 = s_volume_q15;
        if (vol_q15 < 0) vol_q15 = 0;
        if (vol_q15 > 32767) vol_q15 = 32767;
        int32_t gain_q15 = (int32_t)((vol_q15 * k_master_gain_q15) >> 15);

        for (size_t i = 0; i < mono_chunk; i++) {
            int32_t v = (int32_t)p[i] * gain_q15;
            v >>= 15;
            if (v > 32767) v = 32767;
            if (v < -32768) v = -32768;
            mono_buf[i] = (int16_t)v;
        }

        // Drain this chunk fully. Treat any positive write as forward progress,
        // even if API returns timeout for the remaining bytes.
        const uint8_t* out = (const uint8_t*)mono_buf;
        size_t remain = mono_chunk * sizeof(int16_t);
        while (remain > 0) {
            size_t written = 0;
            esp_err_t r = i2s_channel_write(s_tx_chan, out, remain, &written, tmo);
            if (written > 0) {
                out += written;
                remain -= written;
                restarted_once = false;
                continue;
            }

            if (r == ESP_OK) {
                // Defensive: ESP_OK with 0-byte write should retry once via restart path.
            }

            if (!restarted_once) {
                ESP_LOGW(TAG, "i2s write failed (r=%s written=%u), restart channel",
                         esp_err_to_name(r), (unsigned)written);
                if (audio_driver_restart_locked()) {
                    restarted_once = true;
                    continue;
                }
                ESP_LOGW(TAG, "i2s channel restart failed");
            }
            ok = false;
            break;
        }
        if (!ok) break;

        p += mono_chunk;
        left -= mono_chunk;
    }

    xSemaphoreGive(s_io_lock);
    return ok;
}
