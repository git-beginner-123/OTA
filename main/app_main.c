#include "esp_log.h"
#include "core/app.h"
#include "audio/audio_engine.h"
#include "drivers/led_guard.h"
#include "comm_wifi.h"
#include "comm_ble.h"

static const char* kTag = "APP_MAIN";

void app_main(void)
{
    #if 0
    lcd_fill(LCD_RED);
vTaskDelay(pdMS_TO_TICKS(300));
lcd_fill(LCD_GREEN);
vTaskDelay(pdMS_TO_TICKS(300));
lcd_fill(LCD_BLUE);
vTaskDelay(pdMS_TO_TICKS(300));
lcd_fill(LCD_WHITE);
vTaskDelay(pdMS_TO_TICKS(300));
lcd_fill(LCD_BLACK);

#endif


    CommBle_InitOnce();
    LedGuard_AllOff();
    ESP_LOGI(kTag, "start");
    if (!AudioEngine_Init()) {
        ESP_LOGW(kTag, "audio engine init failed");
    }
    esp_log_level_set("comm_wifi", ESP_LOG_INFO);
    esp_log_level_set("comm_ble",  ESP_LOG_INFO);
    comm_wifi_start();
    App_Run();
}
