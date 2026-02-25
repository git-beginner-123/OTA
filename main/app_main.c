#include "esp_log.h"
#include "core/app.h"
#include "core/app_settings.h"
#include "audio/audio_engine.h"
#include "drivers/led_guard.h"
#include "comm_wifi.h"

static const char* kTag = "APP_MAIN";

void app_main(void)
{
    LedGuard_AllOff();
    ESP_LOGI(kTag, "start");

    if (!AudioEngine_Init()) {
        ESP_LOGW(kTag, "audio engine init failed");
    }

    esp_log_level_set("comm_wifi", ESP_LOG_INFO);
    comm_wifi_start();

    AppSettings cfg;
    AppSettings_Default(&cfg);
    (void)AppSettings_Load(&cfg);
    AudioEngine_SetMasterVolumePercent((int)cfg.volume_pct);
    App_Run();
}
