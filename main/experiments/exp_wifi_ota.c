#include "experiments/experiment.h"
#include "core/app_settings.h"
#include "ui/ui.h"
#include "audio/sfx.h"
#include "audio/audio_engine.h"

#include "comm_wifi.h"
#include "sdkconfig.h"

#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_crt_bundle.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "esp_http_server.h"
#include "display/st7789.h"
#include "qrcode.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

static const char* TAG = "EXP_SYSTEM";
static const char* kSystemTitle = "SETTING";
static const char* kSystemCopyright = "Copyright (C) 2026 SEESAW";
static const char* kProvServiceName = "SOY_GAME_TECK";
static const char* kProvPop = "SEESAW2026";
static const char* kPortalUrl = "http://192.168.4.1";

typedef enum {
    kSettingPageHome = 0,
    kSettingPageOta,
    kSettingPageVolume,
    kSettingPageGoTime,
} SettingPage;

static SettingPage s_page = kSettingPageHome;
static int s_home_sel = 0;
static int s_time_sel = 0;
static AppSettings s_cfg;

typedef enum {
    kStatePortalProvision = 0,
    kStateConnecting,
    kStateDownloading,
    kStateSuccess,
    kStateFail,
} SystemState;

static TaskHandle_t s_ota_task = NULL;
static volatile bool s_abort = false;
static volatile SystemState s_state = kStateConnecting;
static volatile int s_progress = 0;
static bool s_ui_dirty = true;
static char s_status[64] = "Searching WiFi...";
static bool s_need_full_redraw = true;
static uint32_t s_last_anim_ms = 0;
static int s_anim_phase = 0;

static char s_sel_ssid[33];
static bool s_qr_ready = false;
static int s_qr_size = 0;
static uint8_t* s_qr_matrix = NULL;
static size_t s_qr_matrix_cap = 0;
static char s_qr_payload[192];
static char s_ota_url[200];
static httpd_handle_t s_portal_httpd = NULL;
static volatile bool s_portal_submitted = false;
static char s_portal_ssid[33];
static char s_portal_pass[65];
#define OTA_TASK_STACK_BYTES      6144
#define OTA_QR_TASK_STACK_BYTES   7168
#define PORTAL_STACK_BYTES        4096

static void ota_task_exit(void)
{
    UBaseType_t hwm = uxTaskGetStackHighWaterMark(NULL);
    ESP_LOGI(TAG, "task=%s stack_hwm=%u", pcTaskGetName(NULL), (unsigned)hwm);
    s_ota_task = NULL;
    vTaskDelete(NULL);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t c_text(void) { return Ui_ColorRGB(225, 230, 235); }
static uint16_t c_ok(void) { return Ui_ColorRGB(120, 240, 120); }
static uint16_t c_err(void) { return Ui_ColorRGB(255, 130, 130); }
static uint16_t c_info(void) { return Ui_ColorRGB(170, 210, 255); }
static uint16_t c_qr_bg(void) { return Ui_ColorRGB(250, 250, 250); }
static uint16_t c_qr_fg(void) { return Ui_ColorRGB(10, 10, 10); }

static const char* ota_target_slug(void)
{
    if (s_cfg.ota_game_sel == 0) return "go";
    if (s_cfg.ota_game_sel == 1) return "chess";
    if (s_cfg.ota_game_sel == 2) return "dice";
    if (s_cfg.ota_game_sel == 3) return "gomoku";
    return "go";
}

static void format_target_token(int index, char* out, size_t cap)
{
    if (!out || cap < 2) return;
    out[0] = 0;
    const char* name = AppSettings_OtaGameName(index);
    if (index == (int)s_cfg.ota_game_sel) snprintf(out, cap, "[%s]", name);
    else snprintf(out, cap, "%s", name);
}

static void draw_game_selector_rows(void)
{
    const int count = AppSettings_OtaGameCount();
    char t0[20], t1[20], t2[20], t3[20];
    format_target_token(0, t0, sizeof(t0));
    format_target_token(1, t1, sizeof(t1));
    format_target_token(2, t2, sizeof(t2));
    format_target_token(3, t3, sizeof(t3));

    char line0[64];
    char line1[64];
    snprintf(line0, sizeof(line0), "TARGET(LR) %s %s", t0, t1);
    if (count > 2) snprintf(line1, sizeof(line1), "           %s %s", t2, (count > 3) ? t3 : "");
    else snprintf(line1, sizeof(line1), "SEL: %s", AppSettings_OtaGameName(s_cfg.ota_game_sel));

    Ui_DrawBodyTextRowColor(6, line0, c_info());
    Ui_DrawBodyTextRowColor(7, line1, c_info());
}

static bool build_selected_ota_url(char* out, size_t cap)
{
    if (!out || cap < 16) return false;
    out[0] = 0;
    if (!CONFIG_COMM_WIFI_OTA_URL[0]) return false;
    if (strncmp(CONFIG_COMM_WIFI_OTA_URL, "http://", 7) != 0 &&
        strncmp(CONFIG_COMM_WIFI_OTA_URL, "https://", 8) != 0) {
        return false;
    }

    const char* base = CONFIG_COMM_WIFI_OTA_URL;
    const char* marker = strstr(base, "/latest.bin");
    if (!marker || marker[11] != '\0') {
        snprintf(out, cap, "%s", base);
        return true;
    }

    // Only auto-expand when URL is in ".../ota/latest.bin" template form.
    // If user config points to a fixed file such as ".../ota/go/latest.bin",
    // keep it unchanged for direct remote-download testing.
    if ((marker - base) >= 4 && strncmp(marker - 4, "/ota", 4) == 0) {
        int prefix_len = (int)(marker - base) + 1; // include trailing '/'
        if (prefix_len <= 0) return false;
        snprintf(out, cap, "%.*s%s/latest.bin", prefix_len, base, ota_target_slug());
        return true;
    }

    snprintf(out, cap, "%s", base);
    return true;
}

static uint32_t ssid_hash(const char* s)
{
    uint32_t h = 5381U;
    while (s && *s) {
        h = ((h << 5) + h) ^ (uint8_t)(*s++);
    }
    return h;
}

static bool load_saved_pass(const char* ssid, char* out_pass, size_t cap)
{
    if (!ssid || !ssid[0] || !out_pass || cap < 2) return false;
    nvs_handle_t nvs = 0;
    if (nvs_open("syswifi", NVS_READONLY, &nvs) != ESP_OK) return false;

    char key[16];
    snprintf(key, sizeof(key), "p%08x", (unsigned)ssid_hash(ssid));

    size_t req = cap;
    esp_err_t err = nvs_get_str(nvs, key, out_pass, &req);
    nvs_close(nvs);
    return err == ESP_OK && out_pass[0] != 0;
}

static const char* app_version(void)
{
    const esp_app_desc_t* d = esp_app_get_description();
    if (!d || !d->version[0]) return "unknown";
    return d->version;
}

static void set_state(SystemState st, const char* status, int progress)
{
    SystemState prev = s_state;
    bool dynamic_state = (st == kStateConnecting || st == kStateDownloading);
    s_state = st;
    s_progress = progress;
    if (!status) status = "";
    strncpy(s_status, status, sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    if (prev != st || !dynamic_state) {
        s_need_full_redraw = true;
        s_anim_phase = 0;
    }
    s_ui_dirty = true;
}

static void draw_progress_bar(int pct, bool busy)
{
    // Keep geometry aligned with Ui body area constants in ui_lcd.c.
    const int bar_x = 10;
    const int bar_y = 206;
    const int bar_w = 220;
    const int bar_h = 14;
    const uint16_t bg = Ui_ColorRGB(24, 28, 34);
    const uint16_t fg = Ui_ColorRGB(70, 190, 255);
    const uint16_t frame = Ui_ColorRGB(110, 130, 150);

    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;

    St7789_FillRect(bar_x - 1, bar_y - 1, bar_w + 2, bar_h + 2, frame);
    St7789_FillRect(bar_x, bar_y, bar_w, bar_h, bg);

    if (busy && pct == 0) {
        // Animated waiting segment before byte progress starts.
        int seg_w = 36;
        int max_x = bar_w - seg_w;
        if (max_x < 1) max_x = 1;
        int x = (s_anim_phase * 7) % max_x;
        St7789_FillRect(bar_x + x, bar_y + 2, seg_w, bar_h - 4, fg);
    } else {
        int fill = (bar_w * pct) / 100;
        if (fill > 0) St7789_FillRect(bar_x, bar_y + 2, fill, bar_h - 4, fg);
    }
}

static void draw_status_dynamic_rows(void)
{
    char line[64];
    char status_line[64];

    if (s_state == kStateDownloading || s_state == kStateConnecting) {
        const char spinner[] = "|/-\\";
        int ph = s_anim_phase & 3;
        snprintf(status_line, sizeof(status_line), "Status: %c %.52s", spinner[ph], s_status);
    } else {
        snprintf(status_line, sizeof(status_line), "Status: %.55s", s_status);
    }

    uint16_t status_color = c_text();
    if (s_state == kStateFail) status_color = c_err();
    else if (s_state == kStateSuccess) status_color = c_ok();
    Ui_DrawBodyTextRowColor(3, status_line, status_color);

    if (s_state == kStateDownloading) {
        if (s_progress > 0) snprintf(line, sizeof(line), "Progress: %d%%", s_progress);
        else snprintf(line, sizeof(line), "Progress: preparing...");
        Ui_DrawBodyTextRowColor(4, line, c_text());
        draw_progress_bar(s_progress, true);
    } else if (s_state == kStateConnecting && comm_wifi_is_connected()) {
        Ui_DrawBodyTextRowColor(4, "Press OK to start OTA", c_info());
    } else {
        Ui_DrawBodyTextRowColor(4, "WiFi: waiting connection", c_text());
    }
}

static bool perform_ota_http(const char* url, char* errbuf, size_t errcap)
{
    const uint32_t kUiReportMs = 1000U;
    const uint32_t kStallTimeoutMs = 45000U;

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = CONFIG_COMM_WIFI_OTA_HTTP_TIMEOUT_MS,
        .keep_alive_enable = true,
    };

    if (strncmp(url, "https://", 8) == 0) {
        cfg.crt_bundle_attach = esp_crt_bundle_attach;
    }

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (!client) {
        snprintf(errbuf, errcap, "http init failed");
        return false;
    }

    const esp_partition_t* update_part = esp_ota_get_next_update_partition(NULL);
    if (!update_part) {
        snprintf(errbuf, errcap, "no OTA partition");
        esp_http_client_cleanup(client);
        return false;
    }

    esp_ota_handle_t ota_handle = 0;
    bool opened = false;
    bool begun = false;
    bool ok = false;
    int content_len = -1;
    int downloaded = 0;
    int last_report_bucket = -1;
    uint32_t last_ui_ms = now_ms();
    uint32_t last_data_ms = last_ui_ms;

    esp_err_t err = esp_http_client_open(client, 0);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "http open: %s", esp_err_to_name(err));
        goto cleanup;
    }
    opened = true;

    content_len = esp_http_client_fetch_headers(client);
    int status = esp_http_client_get_status_code(client);
    if (status != 200) {
        snprintf(errbuf, errcap, "http status %d", status);
        goto cleanup;
    }

    err = esp_ota_begin(update_part, OTA_SIZE_UNKNOWN, &ota_handle);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "ota begin: %s", esp_err_to_name(err));
        goto cleanup;
    }
    begun = true;

    uint8_t buf[1024];
    while (!s_abort) {
        int n = esp_http_client_read(client, (char*)buf, sizeof(buf));
        if (n < 0) {
            snprintf(errbuf, errcap, "http read fail");
            goto cleanup;
        }
        if (n == 0) break;

        err = esp_ota_write(ota_handle, buf, (size_t)n);
        if (err != ESP_OK) {
            snprintf(errbuf, errcap, "ota write: %s", esp_err_to_name(err));
            goto cleanup;
        }

        downloaded += n;
        last_data_ms = now_ms();
        int progress = 0;
        if (content_len > 0) {
            progress = (downloaded * 100) / content_len;
            if (progress > 100) progress = 100;
        }

        int report_bucket = downloaded / 8192;
        uint32_t now = now_ms();
        if (report_bucket != last_report_bucket || (now - last_ui_ms) >= kUiReportMs) {
            char line[64];
            if (content_len > 0) {
                snprintf(line, sizeof(line), "Downloading... %d%%", progress);
            } else {
                snprintf(line, sizeof(line), "Downloading... %dKB", downloaded / 1024);
            }
            set_state(kStateDownloading, line, progress);
            last_report_bucket = report_bucket;
            last_ui_ms = now;
        }

        if ((now - last_data_ms) > kStallTimeoutMs) {
            snprintf(errbuf, errcap, "download stalled");
            goto cleanup;
        }
    }

    if (s_abort) {
        snprintf(errbuf, errcap, "aborted");
        goto cleanup;
    }

    if (downloaded <= 0) {
        snprintf(errbuf, errcap, "downloaded 0 bytes");
        goto cleanup;
    }

    err = esp_ota_end(ota_handle);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "ota end: %s", esp_err_to_name(err));
        goto cleanup;
    }
    begun = false;

    err = esp_ota_set_boot_partition(update_part);
    if (err != ESP_OK) {
        snprintf(errbuf, errcap, "set boot: %s", esp_err_to_name(err));
        goto cleanup;
    }

    ok = true;

cleanup:
    if (begun) esp_ota_abort(ota_handle);
    if (opened) esp_http_client_close(client);
    esp_http_client_cleanup(client);
    return ok;
}

static void qr_capture_cb(const uint8_t* qrcode)
{
    int sz = esp_qrcode_get_size(qrcode);
    if (sz <= 0 || sz > 177) {
        s_qr_ready = false;
        return;
    }
    size_t need = (size_t)sz * (size_t)sz;
    if (!s_qr_matrix || s_qr_matrix_cap < need) {
        uint8_t* p = (uint8_t*)realloc(s_qr_matrix, need);
        if (!p) {
            s_qr_ready = false;
            return;
        }
        s_qr_matrix = p;
        s_qr_matrix_cap = need;
    }
    s_qr_size = sz;
    for (int y = 0; y < sz; y++) {
        for (int x = 0; x < sz; x++) {
            s_qr_matrix[y * sz + x] = esp_qrcode_get_module(qrcode, x, y) ? 1 : 0;
        }
    }
    s_qr_ready = true;
}

static bool gen_qr_text_for_lcd(const char* text)
{
    if (!text || !text[0]) return false;
    s_qr_ready = false;
    s_qr_size = 0;
    snprintf(s_qr_payload, sizeof(s_qr_payload), "%s", text);

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_capture_cb;
    cfg.max_qrcode_version = 12;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    esp_err_t err = esp_qrcode_generate(&cfg, s_qr_payload);
    if (err != ESP_OK || !s_qr_ready) {
        ESP_LOGW(TAG, "QR gen failed err=%s ready=%d payload=%s",
                 esp_err_to_name(err), (int)s_qr_ready, s_qr_payload);
        return false;
    }
    ESP_LOGI(TAG, "QR ready size=%d", s_qr_size);
    return true;
}

static void free_qr_matrix(void)
{
    if (s_qr_matrix) {
        free(s_qr_matrix);
        s_qr_matrix = NULL;
    }
    s_qr_matrix_cap = 0;
}

static int hexv(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char* s)
{
    if (!s) return;
    char* r = s;
    char* w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
            continue;
        }
        if (r[0] == '%' && r[1] && r[2]) {
            int hi = hexv(r[1]);
            int lo = hexv(r[2]);
            if (hi >= 0 && lo >= 0) {
                *w++ = (char)((hi << 4) | lo);
                r += 3;
                continue;
            }
        }
        *w++ = *r++;
    }
    *w = 0;
}

static bool parse_form_value(const char* body, const char* key, char* out, size_t cap)
{
    if (!body || !key || !out || cap < 2) return false;
    out[0] = 0;

    size_t klen = strlen(key);
    const char* p = body;
    while (*p) {
        const char* eq = strchr(p, '=');
        if (!eq) break;
        const char* amp = strchr(eq + 1, '&');
        size_t name_len = (size_t)(eq - p);
        if (name_len == klen && strncmp(p, key, klen) == 0) {
            size_t vlen = amp ? (size_t)(amp - (eq + 1)) : strlen(eq + 1);
            if (vlen >= cap) vlen = cap - 1;
            memcpy(out, eq + 1, vlen);
            out[vlen] = 0;
            url_decode_inplace(out);
            return out[0] != 0;
        }
        if (!amp) break;
        p = amp + 1;
    }
    return false;
}

static esp_err_t portal_root_get(httpd_req_t* req)
{
    const char* html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>SOY_GAME_TECK</title></head><body>"
        "<h3>SOY_GAME_TECK Wi-Fi Setup</h3>"
        "<p>Input your router Wi-Fi (SSID/password)</p>"
        "<form method='POST' action='/wifi'>"
        "<label>SSID</label><br><input name='ssid' style='width:260px' maxlength='32' required><br><br>"
        "<label>Password</label><br><input name='pass' type='password' style='width:260px' maxlength='64' required><br><br>"
        "<button type='submit' style='height:38px'>Connect</button>"
        "</form></body></html>";
    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t portal_wifi_post(httpd_req_t* req)
{
    char body[256];
    int total = req->content_len;
    if (total <= 0 || total >= (int)sizeof(body)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
        return ESP_OK;
    }

    int got = 0;
    while (got < total) {
        int r = httpd_req_recv(req, body + got, total - got);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "recv fail");
            return ESP_OK;
        }
        got += r;
    }
    body[got] = 0;

    char ssid[33] = {0};
    char pass[65] = {0};
    bool ok_ssid = parse_form_value(body, "ssid", ssid, sizeof(ssid));
    bool ok_pass = parse_form_value(body, "pass", pass, sizeof(pass));
    if (!ok_ssid || !ok_pass) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid/pass required");
        return ESP_OK;
    }

    strncpy(s_portal_ssid, ssid, sizeof(s_portal_ssid) - 1);
    s_portal_ssid[sizeof(s_portal_ssid) - 1] = 0;
    strncpy(s_portal_pass, pass, sizeof(s_portal_pass) - 1);
    s_portal_pass[sizeof(s_portal_pass) - 1] = 0;
    s_portal_submitted = true;

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req,
                           "<html><body><h3>Saved</h3><p>Device is connecting...</p></body></html>",
                           HTTPD_RESP_USE_STRLEN);
}

static bool portal_start(void)
{
    if (!comm_wifi_switch_to_ap_open(kProvServiceName)) return false;
    if (s_portal_httpd) return true;

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = PORTAL_STACK_BYTES;
    cfg.max_uri_handlers = 4;
    if (httpd_start(&s_portal_httpd, &cfg) != ESP_OK) {
        s_portal_httpd = NULL;
        return false;
    }

    httpd_uri_t root = {.uri="/", .method=HTTP_GET, .handler=portal_root_get, .user_ctx=NULL};
    httpd_uri_t wifi = {.uri="/wifi", .method=HTTP_POST, .handler=portal_wifi_post, .user_ctx=NULL};
    httpd_register_uri_handler(s_portal_httpd, &root);
    httpd_register_uri_handler(s_portal_httpd, &wifi);
    return true;
}

static void portal_stop(void)
{
    if (s_portal_httpd) {
        httpd_stop(s_portal_httpd);
        s_portal_httpd = NULL;
    }
    (void)comm_wifi_switch_to_sta_only();
}

static void draw_qr_on_lcd(void)
{
    const int area_x = 20;
    const int area_y = 110;
    const int area_w = 200;
    const int area_h = 176;

    if (!s_qr_ready || s_qr_size <= 0) {
        St7789_FillRect(area_x, area_y, area_w, area_h, c_qr_bg());
        St7789_FillRect(area_x + 8, area_y + 8, area_w - 16, area_h - 16, c_qr_fg());
        St7789_FillRect(area_x + 12, area_y + 12, area_w - 24, area_h - 24, c_qr_bg());
        Ui_DrawTextAtBg(area_x + 66, area_y + 72, "NO QR", c_err(), c_qr_bg());
        return;
    }

    const int quiet = 2;
    int full = s_qr_size + quiet * 2;
    int scale = area_w / full;
    if (area_h / full < scale) scale = area_h / full;
    if (scale < 1) scale = 1;

    int draw_w = full * scale;
    int draw_h = full * scale;
    int ox = area_x + (area_w - draw_w) / 2;
    int oy = area_y + (area_h - draw_h) / 2;

    St7789_FillRect(area_x, area_y, area_w, area_h, c_qr_bg());

    for (int y = 0; y < s_qr_size; y++) {
        for (int x = 0; x < s_qr_size; x++) {
            if (!s_qr_matrix[y * s_qr_size + x]) continue;
            int px = ox + (x + quiet) * scale;
            int py = oy + (y + quiet) * scale;
            St7789_FillRect(px, py, scale, scale, c_qr_fg());
        }
    }
}

static bool run_portal_provision(char* errbuf, size_t errcap)
{
    (void)kProvPop;
    s_portal_submitted = false;
    s_portal_ssid[0] = 0;
    s_portal_pass[0] = 0;

    if (!portal_start()) {
        snprintf(errbuf, errcap, "portal start failed");
        return false;
    }

    (void)gen_qr_text_for_lcd(kPortalUrl);
    set_state(kStatePortalProvision, "Scan QR to provision WiFi", 0);

    uint32_t t0 = now_ms();
    while (!s_abort && !s_portal_submitted) {
        if ((now_ms() - t0) > 180000U) {
            portal_stop();
            snprintf(errbuf, errcap, "portal timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    if (s_abort) {
        portal_stop();
        return false;
    }

    set_state(kStateConnecting, "Connecting WiFi...", 0);

    if (!comm_wifi_switch_to_sta_only()) {
        portal_stop();
        snprintf(errbuf, errcap, "switch sta failed");
        return false;
    }
    if (!comm_wifi_connect_psk(s_portal_ssid, s_portal_pass)) {
        portal_stop();
        snprintf(errbuf, errcap, "wifi connect start failed");
        return false;
    }
    (void)comm_wifi_save_credential(s_portal_ssid, s_portal_pass);

    t0 = now_ms();
    while (!s_abort && !comm_wifi_is_connected()) {
        if ((now_ms() - t0) > (uint32_t)CONFIG_COMM_WIFI_CONNECT_TIMEOUT_MS) {
            portal_stop();
            snprintf(errbuf, errcap, "wifi connect timeout");
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(120));
    }
    portal_stop();
    return !s_abort;
}

static bool auto_connect_with_saved(char* errbuf, size_t errcap)
{
    comm_wifi_start();

    if (comm_wifi_is_connected()) {
        if (!comm_wifi_get_connected_ssid(s_sel_ssid, (int)sizeof(s_sel_ssid))) {
            s_sel_ssid[0] = 0;
        }
        return true;
    }

    uint32_t t0 = now_ms();
    while (!s_abort) {
        CommWifiAp aps[3];
        int ap_count = comm_wifi_scan_top3(aps, 3);
        if (ap_count <= 0) {
            int progress = (int)(((now_ms() - t0) * 100U) / (uint32_t)CONFIG_COMM_WIFI_CONNECT_TIMEOUT_MS);
            if (progress > 99) progress = 99;
            set_state(kStateConnecting, "Searching AP...", progress);
            if ((now_ms() - t0) > (uint32_t)CONFIG_COMM_WIFI_CONNECT_TIMEOUT_MS) break;
            vTaskDelay(pdMS_TO_TICKS(200));
            continue;
        }

        for (int i = 0; i < ap_count && !s_abort; i++) {
            char pass[33] = {0};
            if (!load_saved_pass(aps[i].ssid, pass, sizeof(pass))) continue;

            strncpy(s_sel_ssid, aps[i].ssid, sizeof(s_sel_ssid) - 1);
            s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;

            char st[64];
            snprintf(st, sizeof(st), "Connecting %.30s...", s_sel_ssid);
            set_state(kStateConnecting, st, 0);

            if (!comm_wifi_connect_psk(s_sel_ssid, pass)) continue;

            uint32_t t_connect = now_ms();
            while (!s_abort && !comm_wifi_is_connected()) {
                uint32_t elapsed = now_ms() - t_connect;
                int progress = (int)((elapsed * 100U) / (uint32_t)CONFIG_COMM_WIFI_CONNECT_TIMEOUT_MS);
                if (progress > 99) progress = 99;
                set_state(kStateConnecting, st, progress);
                if (elapsed > (uint32_t)CONFIG_COMM_WIFI_CONNECT_TIMEOUT_MS) break;
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            if (comm_wifi_is_connected()) return true;
        }

        if ((now_ms() - t0) > (uint32_t)CONFIG_COMM_WIFI_CONNECT_TIMEOUT_MS) break;
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    snprintf(errbuf, errcap, "WiFi connect timeout");
    return false;
}

static void ota_prepare_task(void* arg)
{
    (void)arg;
    char err[64] = {0};

    if (!build_selected_ota_url(s_ota_url, sizeof(s_ota_url))) {
        set_state(kStateFail, "Set valid OTA URL", 0);
        ota_task_exit();
        return;
    }

    set_state(kStateConnecting, "Searching WiFi...", 0);

    if (!auto_connect_with_saved(err, sizeof(err))) {
        if (!run_portal_provision(err, sizeof(err))) {
            set_state(kStateFail, err[0] ? err : "QR provision failed", 0);
            ota_task_exit();
            return;
        }
    }

    if (comm_wifi_is_connected()) {
        if (!comm_wifi_get_connected_ssid(s_sel_ssid, (int)sizeof(s_sel_ssid))) {
            s_sel_ssid[0] = 0;
        }
        set_state(kStateConnecting, "WiFi ready. OK to start OTA", 100);
    } else {
        set_state(kStateFail, "WiFi not connected", 0);
    }
    ota_task_exit();
}

static void ota_task_connected(void* arg)
{
    (void)arg;
    char err[64] = {0};

    if (!build_selected_ota_url(s_ota_url, sizeof(s_ota_url))) {
        set_state(kStateFail, "Set valid OTA URL", 0);
        ota_task_exit();
        return;
    }

    if (!comm_wifi_is_connected()) {
        set_state(kStateFail, "WiFi disconnected", 0);
        ota_task_exit();
        return;
    }

    set_state(kStateDownloading, "Downloading... 0%", 0);
    ESP_LOGI(TAG, "OTA URL: %s", s_ota_url);
    if (!perform_ota_http(s_ota_url, err, sizeof(err))) {
        set_state(kStateFail, err[0] ? err : "OTA failed", 0);
        ota_task_exit();
        return;
    }

    Sfx_PlayVictory();
    set_state(kStateSuccess, "Upgrade success. Rebooting...", 100);
    vTaskDelay(pdMS_TO_TICKS(1200));
    esp_restart();
}

static void draw_ui(void)
{
    if (s_page != kSettingPageOta) return;

    char line[64];

    if (s_need_full_redraw) {
        Ui_DrawFrame(kSystemTitle, "LR:TARGET OK:GO BACK");
        Ui_DrawBodyClear();
    }

    if (s_need_full_redraw) {
        // Shorter label leaves more room for git-describe suffix.
        snprintf(line, sizeof(line), "VER: %.58s", app_version());
        Ui_DrawBodyTextRowColor(0, line, c_info());
        Ui_DrawBodyTextRowColor(1, kSystemCopyright, c_info());
        draw_game_selector_rows();
    }

    if (s_state == kStatePortalProvision) {
        Ui_DrawBodyTextRowColor(2, "WiFi Portal Mode", c_text());
        Ui_DrawBodyTextRowColor(3, "AP: SOY_GAME_TECK", c_info());
        Ui_DrawBodyTextRowColor(4, "Scan QR to configure WiFi", c_info());
        Ui_DrawBodyTextRowColor(5, "Connect AP then submit form", c_text());
        draw_qr_on_lcd();
        return;
    }

    if (s_need_full_redraw) {
        snprintf(line, sizeof(line), "SSID: %s", s_sel_ssid[0] ? s_sel_ssid : "(none)");
        Ui_DrawBodyTextRowColor(2, line, c_text());
    }

    draw_status_dynamic_rows();

    if (s_state == kStateConnecting) {
        draw_progress_bar(s_progress, true);
    }

    if (s_state == kStateConnecting && comm_wifi_is_connected()) {
        Ui_DrawBodyTextRowColor(5, "OK: start OTA", c_text());
    }

    if (s_state == kStateFail) {
        Ui_DrawBodyTextRowColor(5, "OK: retry auto flow", c_text());
    }

    s_need_full_redraw = false;
}

static void draw_setting_home(void)
{
    Ui_DrawFrame(kSystemTitle, "UP/DN:SEL OK:ENTER BACK:RET");
    Ui_DrawBodyTextRowColor(0, "1 SYSTEM OTA", (s_home_sel == 0) ? c_ok() : c_text());
    Ui_DrawBodyTextRowColor(1, "2 VOLUME", (s_home_sel == 1) ? c_ok() : c_text());
    Ui_DrawBodyTextRowColor(2, "3 GO TIME", (s_home_sel == 2) ? c_ok() : c_text());

    char line[64];
    snprintf(line, sizeof(line), "OTA GAME: %s", AppSettings_OtaGameName(s_cfg.ota_game_sel));
    Ui_DrawBodyTextRowColor(4, line, c_info());
    snprintf(line, sizeof(line), "VOL: %d%%", (int)s_cfg.volume_pct);
    Ui_DrawBodyTextRowColor(5, line, c_info());
    snprintf(line, sizeof(line), "GO: %dmin + %ds x%d",
             (int)s_cfg.go_main_min, (int)s_cfg.go_byo_sec, (int)s_cfg.go_byo_count);
    Ui_DrawBodyTextRowColor(6, line, c_info());
    snprintf(line, sizeof(line), "VER: %.56s", app_version());
    Ui_DrawBodyTextRowColor(7, line, c_info());
}

static void draw_setting_volume(void)
{
    Ui_DrawFrame(kSystemTitle, "UP/DN:SEL LR:+- OK:SAVE BACK:HOME");
    Ui_DrawBodyTextRowColor(0, "VOLUME SETTING", c_text());
    char line[48];
    snprintf(line, sizeof(line), "%c CURRENT: %d%%", (s_time_sel == 0) ? '>' : ' ', (int)s_cfg.volume_pct);
    Ui_DrawBodyTextRowColor(2, line, (s_time_sel == 0) ? c_ok() : c_text());
    Ui_DrawBodyTextRowColor(4, "UP/DN select  LR +/-5%", c_info());
}

static void draw_setting_gotime(void)
{
    Ui_DrawFrame(kSystemTitle, "UP/DN:SEL LR:+- OK:SAVE BACK:HOME");
    Ui_DrawBodyTextRowColor(0, "GO TIME (JP RULE)", c_text());
    char line[64];
    snprintf(line, sizeof(line), "%c MAIN MIN : %d", (s_time_sel == 0) ? '>' : ' ', (int)s_cfg.go_main_min);
    Ui_DrawBodyTextRowColor(2, line, (s_time_sel == 0) ? c_ok() : c_text());
    snprintf(line, sizeof(line), "%c BYO SEC  : %d", (s_time_sel == 1) ? '>' : ' ', (int)s_cfg.go_byo_sec);
    Ui_DrawBodyTextRowColor(3, line, (s_time_sel == 1) ? c_ok() : c_text());
    snprintf(line, sizeof(line), "%c BYO CNT  : %d", (s_time_sel == 2) ? '>' : ' ', (int)s_cfg.go_byo_count);
    Ui_DrawBodyTextRowColor(4, line, (s_time_sel == 2) ? c_ok() : c_text());
    snprintf(line, sizeof(line), "MATCH: %dmin + %ds x%d",
             (int)s_cfg.go_main_min, (int)s_cfg.go_byo_sec, (int)s_cfg.go_byo_count);
    Ui_DrawBodyTextRowColor(6, line, c_info());
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame(kSystemTitle, "OK:START  BACK");
    Ui_Println("1) SYSTEM OTA");
    Ui_Println("2) VOLUME SETTING");
    Ui_Println("3) GO TIME SETTING");
    Ui_Println("GO: main + byo + count");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    AppSettings_Default(&s_cfg);
    (void)AppSettings_Load(&s_cfg);
    AudioEngine_SetMasterVolumePercent((int)s_cfg.volume_pct);

    s_page = kSettingPageHome;
    s_home_sel = 0;
    s_time_sel = 0;

    s_abort = false;
    s_progress = 0;
    s_sel_ssid[0] = 0;
    s_qr_ready = false;
    s_qr_size = 0;
    s_qr_payload[0] = 0;
    s_ota_url[0] = 0;
    (void)build_selected_ota_url(s_ota_url, sizeof(s_ota_url));
    strncpy(s_status, "Searching WiFi...", sizeof(s_status) - 1);
    s_status[sizeof(s_status) - 1] = 0;
    s_need_full_redraw = true;
    s_last_anim_ms = 0;
    s_anim_phase = 0;

    s_ui_dirty = false;
    draw_setting_home();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    s_abort = true;
    free_qr_matrix();
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (s_page == kSettingPageHome) {
        if (key == kInputUp) {
            s_home_sel--;
            if (s_home_sel < 0) s_home_sel = 2;
            draw_setting_home();
            return;
        }
        if (key == kInputDown) {
            s_home_sel++;
            if (s_home_sel > 2) s_home_sel = 0;
            draw_setting_home();
            return;
        }
        if (key == kInputEnter) {
            if (s_home_sel == 0) {
                s_page = kSettingPageOta;
                (void)build_selected_ota_url(s_ota_url, sizeof(s_ota_url));
                set_state(kStateConnecting, "Searching WiFi...", 0);
                s_abort = false;
                if (!s_ota_task) {
                    if (xTaskCreate(ota_prepare_task, "ota_prepare_task", OTA_QR_TASK_STACK_BYTES,
                                    NULL, 4, &s_ota_task) != pdPASS) {
                        set_state(kStateFail, "Prepare task create failed", 0);
                    }
                }
                s_ui_dirty = true;
                draw_ui();
            } else if (s_home_sel == 1) {
                s_page = kSettingPageVolume;
                s_time_sel = 0;
                draw_setting_volume();
            } else {
                s_page = kSettingPageGoTime;
                if (s_time_sel < 0 || s_time_sel > 2) s_time_sel = 0;
                draw_setting_gotime();
            }
            return;
        }
        return;
    }

    if (s_page == kSettingPageVolume) {
        if (key == kInputUp || key == kInputDown) {
            s_time_sel = 0;
            draw_setting_volume();
            return;
        }
        if (key == kInputLeft) {
            if (s_cfg.volume_pct >= 5) s_cfg.volume_pct -= 5;
            else s_cfg.volume_pct = 0;
            AudioEngine_SetMasterVolumePercent((int)s_cfg.volume_pct);
            draw_setting_volume();
            return;
        }
        if (key == kInputRight) {
            if (s_cfg.volume_pct <= 95) s_cfg.volume_pct += 5;
            else s_cfg.volume_pct = 100;
            AudioEngine_SetMasterVolumePercent((int)s_cfg.volume_pct);
            draw_setting_volume();
            return;
        }
        if (key == kInputEnter) {
            (void)AppSettings_Save(&s_cfg);
            draw_setting_volume();
            return;
        }
        if (key == kInputBack) {
            s_page = kSettingPageHome;
            draw_setting_home();
            return;
        }
        return;
    }

    if (s_page == kSettingPageGoTime) {
        if (key == kInputUp) {
            s_time_sel--;
            if (s_time_sel < 0) s_time_sel = 2;
            draw_setting_gotime();
            return;
        }
        if (key == kInputDown) {
            s_time_sel++;
            if (s_time_sel > 2) s_time_sel = 0;
            draw_setting_gotime();
            return;
        }
        if (key == kInputRight) {
            if (s_time_sel == 0 && s_cfg.go_main_min < 180) s_cfg.go_main_min++;
            if (s_time_sel == 1 && s_cfg.go_byo_sec < 120) s_cfg.go_byo_sec++;
            if (s_time_sel == 2 && s_cfg.go_byo_count < 20) s_cfg.go_byo_count++;
            draw_setting_gotime();
            return;
        }
        if (key == kInputLeft) {
            if (s_time_sel == 0 && s_cfg.go_main_min > 1) s_cfg.go_main_min--;
            if (s_time_sel == 1 && s_cfg.go_byo_sec > 5) s_cfg.go_byo_sec--;
            if (s_time_sel == 2 && s_cfg.go_byo_count > 1) s_cfg.go_byo_count--;
            draw_setting_gotime();
            return;
        }
        if (key == kInputEnter) {
            (void)AppSettings_Save(&s_cfg);
            draw_setting_gotime();
            return;
        }
        if (key == kInputBack) {
            s_page = kSettingPageHome;
            draw_setting_home();
            return;
        }
        return;
    }

    if (s_page != kSettingPageOta) return;
    if (key == kInputBack) {
        if (s_ota_task) {
            s_abort = true;
        } else {
            s_page = kSettingPageHome;
            draw_setting_home();
        }
        return;
    }
    if (key == kInputLeft || key == kInputRight) {
        int n = AppSettings_OtaGameCount();
        int v = (int)s_cfg.ota_game_sel + ((key == kInputRight) ? 1 : -1);
        if (v < 0) v = n - 1;
        if (v >= n) v = 0;
        s_cfg.ota_game_sel = (uint8_t)v;
        (void)AppSettings_Save(&s_cfg);
        (void)build_selected_ota_url(s_ota_url, sizeof(s_ota_url));
        s_need_full_redraw = true;
        s_ui_dirty = true;
        return;
    }
    if (s_ota_task) return;

    if (s_state == kStateConnecting && comm_wifi_is_connected() && key == kInputEnter) {
        s_abort = false;
        if (xTaskCreate(ota_task_connected, "ota_task_conn", OTA_TASK_STACK_BYTES, NULL, 4, &s_ota_task) != pdPASS) {
            set_state(kStateFail, "OTA task create failed", 0);
        }
        return;
    }

    if (s_state == kStateFail && key == kInputEnter) {
        set_state(kStateConnecting, "Searching WiFi...", 0);
        s_abort = false;
        if (!s_ota_task) {
            if (xTaskCreate(ota_prepare_task, "ota_prepare_task", OTA_QR_TASK_STACK_BYTES,
                            NULL, 4, &s_ota_task) != pdPASS) {
                set_state(kStateFail, "Prepare task create failed", 0);
            }
        }
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (s_page != kSettingPageOta) return;
    uint32_t t = now_ms();
    if (s_state == kStateDownloading || s_state == kStateConnecting) {
        if ((t - s_last_anim_ms) >= 120U) {
            s_last_anim_ms = t;
            s_anim_phase++;
            s_ui_dirty = true;
        }
    }
    if (!s_ui_dirty) return;
    draw_ui();
    s_ui_dirty = false;
}

const Experiment g_exp_wifi_ota = {
    .id = 106,
    .title = "SETTING",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};

bool ExpSetting_ShouldConsumeBack(void)
{
    return (s_page != kSettingPageHome);
}
