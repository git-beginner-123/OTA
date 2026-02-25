#include "core/app_settings.h"

#include "nvs.h"

#include <string.h>

#define SETTINGS_NS "app_set"
#define SETTINGS_KEY "cfg"
#define SETTINGS_VER 1

typedef struct {
    uint8_t ver;
    uint8_t volume_pct;
    uint8_t ota_game_sel;
    uint8_t reserved0;
    uint16_t go_main_min;
    uint16_t go_byo_sec;
    uint16_t go_byo_count;
    uint16_t reserved1;
} SettingsBlob;

static const char* kOtaGames[] = {
    "GO",
    "CHESS",
    "DICE",
    "GOMOKU",
};

void AppSettings_Default(AppSettings* out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->volume_pct = 85;
    out->ota_game_sel = 0;
    out->go_main_min = 10;
    out->go_byo_sec = 30;
    out->go_byo_count = 3;
}

static void clamp_cfg(AppSettings* c)
{
    if (c->volume_pct > 100) c->volume_pct = 100;
    if (c->ota_game_sel >= (uint8_t)AppSettings_OtaGameCount()) c->ota_game_sel = 0;
    if (c->go_main_min < 1) c->go_main_min = 1;
    if (c->go_main_min > 180) c->go_main_min = 180;
    if (c->go_byo_sec < 5) c->go_byo_sec = 5;
    if (c->go_byo_sec > 120) c->go_byo_sec = 120;
    if (c->go_byo_count < 1) c->go_byo_count = 1;
    if (c->go_byo_count > 20) c->go_byo_count = 20;
}

bool AppSettings_Load(AppSettings* out)
{
    if (!out) return false;
    AppSettings_Default(out);

    nvs_handle_t nvs = 0;
    if (nvs_open(SETTINGS_NS, NVS_READONLY, &nvs) != ESP_OK) return false;

    SettingsBlob b;
    size_t sz = sizeof(b);
    esp_err_t err = nvs_get_blob(nvs, SETTINGS_KEY, &b, &sz);
    nvs_close(nvs);
    if (err != ESP_OK || sz != sizeof(b) || b.ver != SETTINGS_VER) return false;

    out->volume_pct = b.volume_pct;
    out->ota_game_sel = b.ota_game_sel;
    out->go_main_min = b.go_main_min;
    out->go_byo_sec = b.go_byo_sec;
    out->go_byo_count = b.go_byo_count;
    clamp_cfg(out);
    return true;
}

bool AppSettings_Save(const AppSettings* in)
{
    if (!in) return false;
    AppSettings c = *in;
    clamp_cfg(&c);

    SettingsBlob b;
    memset(&b, 0, sizeof(b));
    b.ver = SETTINGS_VER;
    b.volume_pct = c.volume_pct;
    b.ota_game_sel = c.ota_game_sel;
    b.go_main_min = c.go_main_min;
    b.go_byo_sec = c.go_byo_sec;
    b.go_byo_count = c.go_byo_count;

    nvs_handle_t nvs = 0;
    if (nvs_open(SETTINGS_NS, NVS_READWRITE, &nvs) != ESP_OK) return false;
    bool ok = (nvs_set_blob(nvs, SETTINGS_KEY, &b, sizeof(b)) == ESP_OK &&
               nvs_commit(nvs) == ESP_OK);
    nvs_close(nvs);
    return ok;
}

int AppSettings_OtaGameCount(void)
{
    return (int)(sizeof(kOtaGames) / sizeof(kOtaGames[0]));
}

const char* AppSettings_OtaGameName(int index)
{
    if (index < 0 || index >= AppSettings_OtaGameCount()) return "N/A";
    return kOtaGames[index];
}
