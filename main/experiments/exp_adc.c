#include "experiments/experiment.h"
#include "ui/ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_timer.h"

#define ADC_GPIO 17

static const char* TAG = "EXP_ADC";

static bool s_adc_ok = false;
static adc_oneshot_unit_handle_t s_adc_handle = NULL;
static const adc_unit_t s_adc_unit = ADC_UNIT_2;
static const adc_channel_t s_adc_ch = ADC_CHANNEL_6; // GPIO17 on ESP32-S3 ADC2

static uint32_t s_next_ms = 0;
static char s_line_raw[32];
static char s_line_v[32];

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void adc_init_once(void)
{
    if (s_adc_handle) {
        s_adc_ok = true;
        return;
    }

    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = s_adc_unit,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc_handle) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_new_unit failed");
        s_adc_handle = NULL;
        s_adc_ok = false;
        return;
    }

    adc_oneshot_chan_cfg_t chan_cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_12,
    };
    if (adc_oneshot_config_channel(s_adc_handle, s_adc_ch, &chan_cfg) != ESP_OK) {
        ESP_LOGE(TAG, "adc_oneshot_config_channel failed");
        adc_oneshot_del_unit(s_adc_handle);
        s_adc_handle = NULL;
        s_adc_ok = false;
        return;
    }

    s_adc_ok = true;
}

static void adc_deinit(void)
{
    if (!s_adc_handle) return;
    adc_oneshot_del_unit(s_adc_handle);
    s_adc_handle = NULL;
    s_adc_ok = false;
}

static void render_value(int raw)
{
    // Approximate conversion (no calibration)
    float v = (float)raw * 3.3f / 4095.0f;

    char line_raw[32];
    char line_v[32];
    snprintf(line_raw, sizeof(line_raw), "RAW: %4d", raw);
    snprintf(line_v, sizeof(line_v), "V:   %.2f", v);

    if (strcmp(line_raw, s_line_raw) != 0) {
        strncpy(s_line_raw, line_raw, sizeof(s_line_raw) - 1);
        s_line_raw[sizeof(s_line_raw) - 1] = 0;
        Ui_DrawBodyTextRowColor(1, s_line_raw, Ui_ColorRGB(230, 230, 230));
    }
    if (strcmp(line_v, s_line_v) != 0) {
        strncpy(s_line_v, line_v, sizeof(s_line_v) - 1);
        s_line_v[sizeof(s_line_v) - 1] = 0;
        Ui_DrawBodyTextRowColor(2, s_line_v, Ui_ColorRGB(180, 220, 180));
    }
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("ADC", "OK:START  BACK");
    Ui_Println("Goal: read analog value.");
    Ui_Println("Signal AO -> GPIO17.");
    Ui_Println("Unit: raw + voltage.");
    Ui_Println("Refresh: every 10 sec.");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    s_line_raw[0] = 0;
    s_line_v[0] = 0;
    s_next_ms = 0;
    adc_init_once();

    Ui_DrawFrame("ADC", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "STATUS: RUN", Ui_ColorRGB(200, 200, 200));
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    adc_deinit();
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    uint32_t t = now_ms();
    if (t < s_next_ms) return;
    s_next_ms = t + 10000;

    if (!s_adc_ok) {
        Ui_DrawBodyTextRowColor(1, "ADC ERROR", Ui_ColorRGB(255, 120, 120));
        return;
    }

    int raw = 0;
    if (!s_adc_handle || adc_oneshot_read(s_adc_handle, s_adc_ch, &raw) != ESP_OK) {
        Ui_DrawBodyTextRowColor(1, "READ ERROR", Ui_ColorRGB(255, 120, 120));
        return;
    }

    render_value(raw);
}

const Experiment g_exp_adc = {
    .id = 3,
    .title = "ADC",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = 0,
    .tick = tick,
};
