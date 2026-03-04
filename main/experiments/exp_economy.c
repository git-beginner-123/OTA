#include "experiments/experiment.h"
#include "ui/ui.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_http_server.h"

#include "net/market_yahoo.h"
#include "comm_wifi.h"
#include "audio/sfx.h"
#include "display/st7789.h"
#include "qrcode.h"

typedef enum {
    kStagePortal = 0,
    kStageConnecting,
    kStageConnected,
    kStageConnectFail,
} WifiStaStage;

static const char* TAG = "EXP_WIFI_STA";
static const char* kApSsid = "STEM_SETUP";
static const char* kPortalUrl = "http://192.168.4.1";
#define WIFI_STA_HTTPD_STACK_BYTES 3584

static const char* kSymbols[] = {
    "GC=F", // Gold futures
    "SI=F", // Silver futures
    "BZ=F", // Brent crude oil futures
};

static uint32_t s_next_ui_ms = 0;
static uint32_t s_connect_deadline_ms = 0;
static WifiStaStage s_stage = kStagePortal;
static char s_sel_ssid[33];
static char s_status_line[64] = "Open URL and submit WiFi";
static bool s_connected_screen = false;
static char s_line_cache[3][48];

static bool s_qr_ready = false;
static int s_qr_size = 0;
static uint8_t* s_qr_matrix = NULL;
static size_t s_qr_matrix_cap = 0;
static bool s_portal_drawn = false;
static bool s_fail_drawn = false;

static httpd_handle_t s_httpd = NULL;
static volatile bool s_apply_pending = false;
static char s_pending_ssid[33];
static char s_pending_pass[65];
static bool s_save_pending_cred = false;
static char s_connecting_pass[65];
static bool s_connect_from_saved = false;
static uint32_t s_connected_since_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t color_gold(void)   { return Ui_ColorRGB(212, 175, 55); }
static uint16_t color_silver(void) { return Ui_ColorRGB(192, 192, 192); }
static uint16_t color_oil(void)    { return Ui_ColorRGB(139, 69, 19); }
static uint16_t color_value(void)  { return Ui_ColorRGB(230, 230, 230); }
static uint16_t color_err(void)    { return Ui_ColorRGB(255, 130, 130); }

static void qr_capture_cb(const uint8_t* qrcode)
{
    int sz = esp_qrcode_get_size(qrcode);
    if (sz <= 0 || sz > 64) {
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

static bool gen_qr_url(void)
{
    s_qr_ready = false;
    s_qr_size = 0;

    esp_qrcode_config_t cfg = ESP_QRCODE_CONFIG_DEFAULT();
    cfg.display_func = qr_capture_cb;
    cfg.max_qrcode_version = 9;
    cfg.qrcode_ecc_level = ESP_QRCODE_ECC_MED;
    return esp_qrcode_generate(&cfg, kPortalUrl) == ESP_OK && s_qr_ready;
}

static void draw_qr_on_lcd(void)
{
    const int area_x = 20;
    const int area_y = 92;
    const int area_w = 200;
    const int area_h = 194;
    const uint16_t bg = Ui_ColorRGB(250, 250, 250);
    const uint16_t fg = Ui_ColorRGB(10, 10, 10);

    if (!s_qr_ready || s_qr_size <= 0) {
        St7789_FillRect(area_x, area_y, area_w, area_h, bg);
        Ui_DrawTextAtBg(area_x + 52, area_y + 86, "QR UNAVAILABLE", color_err(), bg);
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

    St7789_FillRect(area_x, area_y, area_w, area_h, bg);
    for (int y = 0; y < s_qr_size; y++) {
        for (int x = 0; x < s_qr_size; x++) {
            if (!s_qr_matrix[y * s_qr_size + x]) continue;
            int px = ox + (x + quiet) * scale;
            int py = oy + (y + quiet) * scale;
            St7789_FillRect(px, py, scale, scale, fg);
        }
    }
}

static void render_portal_screen(void)
{
    Ui_DrawFrame("ECONOMY", "BACK=RET");
    Ui_DrawBodyClear();
    char line0[48];
    snprintf(line0, sizeof(line0), "WEB PROVISION (%d SAVED)", comm_wifi_saved_credential_count());
    Ui_DrawBodyTextRowColor(0, line0, color_value());
    Ui_DrawBodyTextRowColor(1, "1) Join AP: STEM_SETUP", color_value());
    Ui_DrawBodyTextRowColor(2, "2) Open: http://192.168.4.1", color_value());
    Ui_DrawBodyTextRowColor(3, s_status_line, color_value());
    draw_qr_on_lcd();
    s_portal_drawn = true;
}

static void render_connecting_screen(void)
{
    Ui_DrawFrame("ECONOMY", "BACK=RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "CONNECTING...", color_value());
    Ui_DrawBodyTextRowColor(1, s_sel_ssid[0] ? s_sel_ssid : "(none)", color_value());
    Ui_DrawBodyTextRowColor(2, s_status_line, color_value());
    s_fail_drawn = false;
}

static void render_connected_screen_once(void)
{
    if (s_connected_screen) return;
    s_connected_screen = true;
    Ui_DrawFrame("ECONOMY", "BACK=RET");
    Ui_DrawBodyClear();
    if (s_sel_ssid[0]) {
        char line[48];
        snprintf(line, sizeof(line), "STATUS: %s", s_sel_ssid);
        Ui_DrawBodyTextRowColor(0, line, color_value());
    } else {
        Ui_DrawBodyTextRowColor(0, "STATUS: CONNECTED", color_value());
    }
    s_fail_drawn = false;
}

static void render_connect_fail_screen_once(void)
{
    if (s_fail_drawn) return;
    s_fail_drawn = true;
    Ui_DrawFrame("ECONOMY", "OK:RETRY  BACK:RET");
    Ui_DrawBodyClear();
    Ui_DrawBodyTextRowColor(0, "PROVISION FAILED", color_err());
    Ui_DrawBodyTextRowColor(1, s_status_line, color_value());
    Ui_DrawBodyTextRowColor(2, "Press OK to start AP again", color_value());
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static void url_decode_inplace(char* s)
{
    char* r = s;
    char* w = s;
    while (*r) {
        if (*r == '+') {
            *w++ = ' ';
            r++;
            continue;
        }
        if (*r == '%' && r[1] && r[2]) {
            int hi = hex_val(r[1]);
            int lo = hex_val(r[2]);
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

static void parse_form_body(char* body, char* out_ssid, size_t ssid_cap, char* out_pwd, size_t pwd_cap)
{
    out_ssid[0] = 0;
    out_pwd[0] = 0;

    char* saveptr = NULL;
    for (char* tok = strtok_r(body, "&", &saveptr); tok; tok = strtok_r(NULL, "&", &saveptr)) {
        char* eq = strchr(tok, '=');
        if (!eq) continue;
        *eq = 0;
        char* k = tok;
        char* v = eq + 1;
        url_decode_inplace(k);
        url_decode_inplace(v);

        if (strcmp(k, "ssid") == 0) {
            strncpy(out_ssid, v, ssid_cap - 1);
            out_ssid[ssid_cap - 1] = 0;
        } else if (strcmp(k, "pwd") == 0) {
            strncpy(out_pwd, v, pwd_cap - 1);
            out_pwd[pwd_cap - 1] = 0;
        }
    }
}

static esp_err_t root_get_handler(httpd_req_t* req)
{
    const char* html =
        "<!doctype html><html><head><meta charset='utf-8'>"
        "<meta name='viewport' content='width=device-width,initial-scale=1'>"
        "<title>STEM WiFi Setup</title></head><body>"
        "<h2>STEM WiFi Setup</h2>"
        "<form method='post' action='/wifi'>"
        "<div><label>SSID</label><br><input name='ssid' style='width:95%'></div>"
        "<div style='margin-top:10px'><label>Password</label><br><input name='pwd' type='password' style='width:95%'></div>"
        "<div style='margin-top:14px'><button type='submit'>Connect</button></div>"
        "</form>"
        "<p style='margin-top:14px'>After submit, return to device screen.</p>"
        "</body></html>";

    httpd_resp_set_type(req, "text/html");
    return httpd_resp_send(req, html, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t wifi_post_handler(httpd_req_t* req)
{
    int len = req->content_len;
    if (len <= 0 || len > 255) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "invalid body\n", HTTPD_RESP_USE_STRLEN);
    }

    char body[256];
    int r = httpd_req_recv(req, body, len);
    if (r <= 0) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "read failed\n", HTTPD_RESP_USE_STRLEN);
    }
    body[r] = 0;

    char ssid[33];
    char pwd[65];
    parse_form_body(body, ssid, sizeof(ssid), pwd, sizeof(pwd));
    if (!ssid[0]) {
        httpd_resp_set_status(req, "400 Bad Request");
        return httpd_resp_send(req, "ssid required\n", HTTPD_RESP_USE_STRLEN);
    }

    strncpy(s_pending_ssid, ssid, sizeof(s_pending_ssid) - 1);
    s_pending_ssid[sizeof(s_pending_ssid) - 1] = 0;
    strncpy(s_pending_pass, pwd, sizeof(s_pending_pass) - 1);
    s_pending_pass[sizeof(s_pending_pass) - 1] = 0;
    s_apply_pending = true;

    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_send(req, "saved. device is connecting...\n", HTTPD_RESP_USE_STRLEN);
}

static bool portal_web_start(void)
{
    if (s_httpd) return true;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = WIFI_STA_HTTPD_STACK_BYTES;
    cfg.max_uri_handlers = 3;
    if (httpd_start(&s_httpd, &cfg) != ESP_OK) {
        s_httpd = NULL;
        return false;
    }

    httpd_uri_t root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = root_get_handler,
        .user_ctx = NULL
    };
    httpd_uri_t wifi = {
        .uri = "/wifi",
        .method = HTTP_POST,
        .handler = wifi_post_handler,
        .user_ctx = NULL
    };
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &wifi);
    return true;
}

static void portal_web_stop(void)
{
    if (!s_httpd) return;
    httpd_stop(s_httpd);
    s_httpd = NULL;
}

static void free_qr_matrix(void)
{
    if (s_qr_matrix) {
        free(s_qr_matrix);
        s_qr_matrix = NULL;
    }
    s_qr_matrix_cap = 0;
}

static void enter_portal_mode(void)
{
    if (!comm_wifi_switch_to_ap_open(kApSsid)) {
        snprintf(s_status_line, sizeof(s_status_line), "AP start failed");
        s_stage = kStageConnectFail;
        s_portal_drawn = false;
        return;
    }
    if (!portal_web_start()) {
        snprintf(s_status_line, sizeof(s_status_line), "Web start failed");
        s_stage = kStageConnectFail;
        s_portal_drawn = false;
        return;
    }
    if (!gen_qr_url()) {
        snprintf(s_status_line, sizeof(s_status_line), "QR generate failed");
    } else {
        snprintf(s_status_line, sizeof(s_status_line), "Scan QR and submit");
    }
    s_stage = kStagePortal;
    s_portal_drawn = false;
    s_fail_drawn = false;
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("ECONOMY", "OK:START  BACK");
    Ui_Println("Fetch market quotes.");
    Ui_Println("If offline, use web provisioning.");
    Ui_Println("Join AP: STEM_SETUP");
    Ui_Println("Open: http://192.168.4.1");
    Ui_Println("Enter SSID and password.");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Sfx_Deinit();
    comm_wifi_start();
    Markets_Init(kSymbols, (int)(sizeof(kSymbols) / sizeof(kSymbols[0])));

    s_next_ui_ms = 0;
    s_connect_deadline_ms = 0;
    s_sel_ssid[0] = 0;
    s_connected_screen = false;
    s_portal_drawn = false;
    s_qr_ready = false;
    s_qr_size = 0;
    s_apply_pending = false;
    s_pending_ssid[0] = 0;
    s_pending_pass[0] = 0;
    s_save_pending_cred = false;
    s_connecting_pass[0] = 0;
    s_connect_from_saved = false;
    s_fail_drawn = false;
    memset(s_line_cache, 0, sizeof(s_line_cache));
    s_connected_since_ms = 0;

    if (comm_wifi_is_connected()) {
        if (!comm_wifi_get_connected_ssid(s_sel_ssid, (int)sizeof(s_sel_ssid))) s_sel_ssid[0] = 0;
        s_stage = kStageConnected;
        s_connected_since_ms = now_ms();
        render_connected_screen_once();
    } else {
        (void)comm_wifi_switch_to_sta_only();
        if (comm_wifi_connect_saved_any(7000, s_sel_ssid, (int)sizeof(s_sel_ssid))) {
            s_stage = kStageConnecting;
            s_connect_deadline_ms = now_ms() + 9000U;
            snprintf(s_status_line, sizeof(s_status_line), "Auto connecting %.24s", s_sel_ssid);
            s_connect_from_saved = true;
            render_connecting_screen();
        } else {
            enter_portal_mode();
            if (s_stage == kStagePortal) render_portal_screen();
        }
    }
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    portal_web_stop();
    free_qr_matrix();
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (key != kInputEnter) return;
    if (s_stage == kStageConnected) return;
    if (s_stage == kStagePortal || s_stage == kStageConnectFail) {
        enter_portal_mode();
        if (s_stage == kStagePortal) render_portal_screen();
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    uint32_t t = now_ms();

    if (s_stage == kStagePortal) {
        if (!s_portal_drawn) render_portal_screen();
        if (s_apply_pending) {
            s_apply_pending = false;
            strncpy(s_sel_ssid, s_pending_ssid, sizeof(s_sel_ssid) - 1);
            s_sel_ssid[sizeof(s_sel_ssid) - 1] = 0;
            strncpy(s_connecting_pass, s_pending_pass, sizeof(s_connecting_pass) - 1);
            s_connecting_pass[sizeof(s_connecting_pass) - 1] = 0;
            s_save_pending_cred = true;
            s_connect_from_saved = false;

            portal_web_stop();
            (void)comm_wifi_switch_to_sta_only();
            // Let WiFi driver settle in STA mode before setting new config.
            vTaskDelay(pdMS_TO_TICKS(120));
            if (!comm_wifi_connect_psk(s_pending_ssid, s_pending_pass)) {
                snprintf(s_status_line, sizeof(s_status_line), "Connect start failed");
                s_stage = kStageConnectFail;
                s_portal_drawn = false;
                return;
            }
            snprintf(s_status_line, sizeof(s_status_line), "Connecting to %.24s", s_pending_ssid);
            s_stage = kStageConnecting;
            s_connect_deadline_ms = t + 25000U;
            render_connecting_screen();
        }
        return;
    }

    if (s_stage == kStageConnecting) {
        if (comm_wifi_is_connected()) {
            if (!comm_wifi_get_connected_ssid(s_sel_ssid, (int)sizeof(s_sel_ssid))) s_sel_ssid[0] = 0;
            if (s_save_pending_cred && s_sel_ssid[0] && s_connecting_pass[0]) {
                (void)comm_wifi_save_credential(s_sel_ssid, s_connecting_pass);
            }
            s_save_pending_cred = false;
            s_connecting_pass[0] = 0;
            s_connect_from_saved = false;
            s_stage = kStageConnected;
            s_connected_since_ms = now_ms();
            s_connected_screen = false;
            s_next_ui_ms = 0;
            render_connected_screen_once();
            return;
        }
        if (s_connect_deadline_ms != 0 && t >= s_connect_deadline_ms) {
            int r = comm_wifi_last_disconnect_reason();
            if (s_connect_from_saved) {
                if (r > 0) snprintf(s_status_line, sizeof(s_status_line), "No saved AP matched (r=%d)", r);
                else snprintf(s_status_line, sizeof(s_status_line), "No saved AP matched");
                enter_portal_mode();
                if (s_stage == kStagePortal) render_portal_screen();
            } else {
                if (r > 0) snprintf(s_status_line, sizeof(s_status_line), "Connect timeout r=%d. OK retry", r);
                else snprintf(s_status_line, sizeof(s_status_line), "Connect timeout. OK retry");
                s_stage = kStageConnectFail;
                s_portal_drawn = false;
            }
        }
        return;
    }

    if (s_stage == kStageConnectFail) {
        render_connect_fail_screen_once();
        return;
    }

    if (s_stage != kStageConnected) return;

    Markets_Tick(t);
    if (t < s_next_ui_ms) return;
    s_next_ui_ms = t + 1000;

    render_connected_screen_once();
    bool any_valid = Markets_HasAnyValid();
    bool show_query_status = (!any_valid && s_connected_since_ms != 0 && (t - s_connected_since_ms) >= 3000U);
    const char* query_status = Markets_LastStatus();
    for (int i = 0; i < Markets_Count() && i < 3; i++) {
        MarketQuote q;
        char value[32];
        const char* label = "";
        uint16_t label_color = color_value();

        if (i == 0) { label = "GOLD"; label_color = color_gold(); }
        if (i == 1) { label = "SILV"; label_color = color_silver(); }
        if (i == 2) { label = "BREN"; label_color = color_oil(); }

        if (Markets_Get(i, &q)) {
            snprintf(value, sizeof(value), "%.1f %+.1fpct", q.price, q.change_pct);
        } else if (show_query_status) {
            snprintf(value, sizeof(value), "%.31s", (query_status && query_status[0]) ? query_status : "QUERYING...");
        } else {
            snprintf(value, sizeof(value), "LOADING...");
        }

        if (strcmp(s_line_cache[i], value) != 0) {
            strncpy(s_line_cache[i], value, sizeof(s_line_cache[i]) - 1);
            s_line_cache[i][sizeof(s_line_cache[i]) - 1] = 0;
            Ui_DrawBodyTextRowTwoColor(i + 1, label, value, label_color, color_value());
        }
    }
}

const Experiment g_exp_economy = {
    .id = 10,
    .title = "ECONOMY",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
