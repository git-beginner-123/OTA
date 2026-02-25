#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint8_t volume_pct;       // 0..100
    uint8_t ota_game_sel;     // 0..3
    uint16_t go_main_min;     // minutes
    uint16_t go_byo_sec;      // seconds
    uint16_t go_byo_count;    // periods
} AppSettings;

void AppSettings_Default(AppSettings* out);
bool AppSettings_Load(AppSettings* out);
bool AppSettings_Save(const AppSettings* in);

int AppSettings_OtaGameCount(void);
const char* AppSettings_OtaGameName(int index);
