#include "market_yahoo.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char* TAG = "markets";
static const int kMaxCaptureBytes = 8192;
static const int kMaxRedirectDepth = 3;
static const uint32_t kFetchIntervalRoundMs = 30000;
static const uint32_t kFetchIntervalStepMs = 120;

static MarketQuote* s_list = NULL;
static int s_count = 0;

static uint32_t s_next_fetch_ms = 0;
static int s_next_index = 0;
static bool s_has_any_valid = false;
static char s_last_status[48] = "WAIT NET";
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_worker_task = NULL;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static void lock_state(void)
{
    if (s_lock) xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock_state(void)
{
    if (s_lock) xSemaphoreGive(s_lock);
}

static int http_get_all_impl(const char* url, char** io_buf, int* io_cap, int* out_status, int depth)
{
    if (depth > kMaxRedirectDepth) return -1;
    bool https = (strncmp(url, "https://", 8) == 0);
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 2200,
        .transport_type = https ? HTTP_TRANSPORT_OVER_SSL : HTTP_TRANSPORT_OVER_TCP,
        .buffer_size = 1024,
        .buffer_size_tx = 512,
        .disable_auto_redirect = false,
        .max_redirection_count = kMaxRedirectDepth,
        .crt_bundle_attach = https ? esp_crt_bundle_attach : NULL,
    };

    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;

    esp_http_client_set_header(c, "Accept-Encoding", "identity");
    esp_http_client_set_header(c, "User-Agent", "Mozilla/5.0");

    esp_err_t err = esp_http_client_open(c, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "http open failed: %s", esp_err_to_name(err));
        esp_http_client_cleanup(c);
        return -1;
    }

    int hdr_len = esp_http_client_fetch_headers(c);
    if (hdr_len < 0) {
        ESP_LOGW(TAG, "fetch headers failed: len=%d", hdr_len);
        esp_http_client_close(c);
        esp_http_client_cleanup(c);
        return -1;
    }

    int status = esp_http_client_get_status_code(c);
    if (out_status) *out_status = status;
    if (status == 301 || status == 302 || status == 307 || status == 308) {
        char* location = NULL;
        if (esp_http_client_get_header(c, "Location", &location) == ESP_OK && location && location[0]) {
            esp_http_client_close(c);
            esp_http_client_cleanup(c);
            return http_get_all_impl(location, io_buf, io_cap, out_status, depth + 1);
        }
    }

    int content_len = esp_http_client_get_content_length(c);
    if (content_len > 0) {
        int need = content_len + 1;
        if (need > (kMaxCaptureBytes + 1)) need = kMaxCaptureBytes + 1;
        if (!*io_buf || !*io_cap || *io_cap < need) {
            char* nb = (char*)realloc(*io_buf, (size_t)need);
            if (!nb) {
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return -1;
            }
            *io_buf = nb;
            *io_cap = need;
        }
    } else {
        if (!*io_buf || !*io_cap) {
            *io_cap = 8 * 1024;
            *io_buf = (char*)malloc((size_t)*io_cap);
            if (!*io_buf) {
                esp_http_client_close(c);
                esp_http_client_cleanup(c);
                return -1;
            }
        }
    }

    int total = 0;
    int empty_reads = 0;
    uint32_t start_ms = now_ms();
    const uint32_t max_wait_ms = 1800;
    while (!esp_http_client_is_complete_data_received(c)) {
        if (total >= kMaxCaptureBytes) break;
        int room = (*io_cap) - 1 - total;
        if (room <= 0) {
            int new_cap = (*io_cap) * 2;
            if (new_cap > (kMaxCaptureBytes + 1)) new_cap = kMaxCaptureBytes + 1;
            if (new_cap <= *io_cap) break;
            char* nb = (char*)realloc(*io_buf, (size_t)new_cap);
            if (!nb) break;
            *io_buf = nb;
            *io_cap = new_cap;
            room = (*io_cap) - 1 - total;
        }

        int n = esp_http_client_read(c, (*io_buf) + total, room);
        if (n > 0) {
            total += n;
            empty_reads = 0;
            continue;
        }

        if (n == 0) {
            empty_reads++;
            if ((now_ms() - start_ms) > max_wait_ms) break;
            if (empty_reads > 20) vTaskDelay(pdMS_TO_TICKS(50));
            else vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // n < 0
        break;
    }
    (*io_buf)[total] = 0;

    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return total;
}

static int http_get_all(const char* url, char** io_buf, int* io_cap, int* out_status)
{
    return http_get_all_impl(url, io_buf, io_cap, out_status, 0);
}

static bool parse_quote(const char* json, float* out_price, float* out_chg_pct)
{
    // Fast parse from the meta section (appears early in the payload).
    const char* kPrice = "\"regularMarketPrice\":";
    const char* kChg = "\"regularMarketChangePercent\":";
    const char* kPrev = "\"regularMarketPreviousClose\":";
    const char* kPrev2 = "\"chartPreviousClose\":";

    const char* p = strstr(json, kPrice);
    const char* c = strstr(json, kChg);
    const char* pv = strstr(json, kPrev);
    const char* pv2 = strstr(json, kPrev2);

    if (p) {
        p += strlen(kPrice);
        char* endp = NULL;
        double price = strtod(p, &endp);
        if (endp != p) {
            if (c) {
                c += strlen(kChg);
                char* endc = NULL;
                double chg = strtod(c, &endc);
                if (endc != c) {
                    *out_price = (float)price;
                    *out_chg_pct = (float)chg;
                    return true;
                }
            }
            if (pv || pv2) {
                const char* pp = pv ? pv : pv2;
                const char* key = pv ? kPrev : kPrev2;
                pp += strlen(key);
                char* endv = NULL;
                double prev = strtod(pp, &endv);
                if (endv != pp && prev > 0.0) {
                    *out_price = (float)price;
                    *out_chg_pct = (float)(((price - prev) / prev) * 100.0);
                    return true;
                }
            }
        }
    }

    cJSON* root = cJSON_Parse(json);
    if (!root) return false;

    bool ok = false;

    cJSON* chart  = cJSON_GetObjectItem(root, "chart");
    cJSON* result = chart ? cJSON_GetObjectItem(chart, "result") : NULL;
    cJSON* r0     = (result && cJSON_IsArray(result)) ? cJSON_GetArrayItem(result, 0) : NULL;
    cJSON* meta   = r0 ? cJSON_GetObjectItem(r0, "meta") : NULL;

    cJSON* price  = meta ? cJSON_GetObjectItem(meta, "regularMarketPrice") : NULL;
    cJSON* chg    = meta ? cJSON_GetObjectItem(meta, "regularMarketChangePercent") : NULL;
    cJSON* prev   = meta ? cJSON_GetObjectItem(meta, "regularMarketPreviousClose") : NULL;
    cJSON* prev2  = meta ? cJSON_GetObjectItem(meta, "chartPreviousClose") : NULL;

    if (cJSON_IsNumber(price)) {
        *out_price = (float)price->valuedouble;
        if (cJSON_IsNumber(chg)) {
            *out_chg_pct = (float)chg->valuedouble;
            ok = true;
        } else {
            cJSON* use_prev = cJSON_IsNumber(prev) ? prev : (cJSON_IsNumber(prev2) ? prev2 : NULL);
            if (use_prev && use_prev->valuedouble > 0.0) {
                double p0 = use_prev->valuedouble;
                *out_chg_pct = (float)(((*out_price - p0) / p0) * 100.0);
                ok = true;
            }
        }
    }

    cJSON_Delete(root);
    return ok;
}

void Markets_Init(const char* const* symbols, int count)
{
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    if (!s_lock) return;

    lock_state();
    if (s_list) {
        free(s_list);
        s_list = NULL;
    }
    s_count = 0;
    s_next_index = 0;
    s_next_fetch_ms = 0;
    s_has_any_valid = false;
    snprintf(s_last_status, sizeof(s_last_status), "WAIT NET");

    if (!symbols || count <= 0) {
        unlock_state();
        return;
    }

    s_list = (MarketQuote*)calloc((size_t)count, sizeof(MarketQuote));
    if (!s_list) {
        unlock_state();
        return;
    }

    s_count = count;
    for (int i = 0; i < count; i++) {
        strncpy(s_list[i].symbol, symbols[i], sizeof(s_list[i].symbol) - 1);
        s_list[i].valid = false;
    }
    unlock_state();
}

static void markets_fetch_one_step(void)
{
    uint32_t t = now_ms();
    lock_state();
    if (!s_list || s_count <= 0 || t < s_next_fetch_ms) {
        unlock_state();
        return;
    }

    static char* buf = NULL;
    static int cap = 0;

    if (s_next_index < 0 || s_next_index >= s_count) s_next_index = 0;
    int idx = s_next_index;
    char symbol[16];
    strncpy(symbol, s_list[idx].symbol, sizeof(symbol) - 1);
    symbol[sizeof(symbol) - 1] = 0;

    snprintf(s_last_status, sizeof(s_last_status), "QUERY %s", symbol);
    unlock_state();

    char url[160];
    snprintf(url, sizeof(url),
             "https://query1.finance.yahoo.com/v8/finance/chart/%s?interval=1d&range=1d",
             symbol);

    int status = 0;
    int n = http_get_all(url, &buf, &cap, &status);
    lock_state();
    if (!s_list || idx < 0 || idx >= s_count || strcmp(s_list[idx].symbol, symbol) != 0) {
        unlock_state();
        return;
    }

    if (n <= 0 || status != 200) {
        if (s_list[idx].last_update_ms == 0) s_list[idx].valid = false;
        if (status > 0) snprintf(s_last_status, sizeof(s_last_status), "HTTP %d %s", status, symbol);
        else snprintf(s_last_status, sizeof(s_last_status), "NET ERR %s", symbol);
        ESP_LOGW(TAG, "fetch failed sym=%s status=%d", symbol, status);
    } else {
        float price = 0.0f, chg = 0.0f;
        if (parse_quote(buf, &price, &chg)) {
            s_list[idx].price = price;
            s_list[idx].change_pct = chg;
            s_list[idx].last_update_ms = t;
            s_list[idx].valid = true;
            s_has_any_valid = true;
            snprintf(s_last_status, sizeof(s_last_status), "OK %s", symbol);
        } else {
            if (s_list[idx].last_update_ms == 0) s_list[idx].valid = false;
            snprintf(s_last_status, sizeof(s_last_status), "PARSE ERR %s", symbol);
            ESP_LOGW(TAG, "parse failed sym=%s status=%d len=%d", symbol, status, n);
        }
    }

    s_next_index++;
    if (s_next_index >= s_count) {
        s_next_index = 0;
        s_next_fetch_ms = t + kFetchIntervalRoundMs;
    } else {
        s_next_fetch_ms = t + kFetchIntervalStepMs;
    }
    unlock_state();
}

static void markets_worker_task(void* arg)
{
    (void)arg;
    while (1) {
        markets_fetch_one_step();
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void Markets_Tick(uint32_t unused_now_ms)
{
    (void)unused_now_ms;
    if (!s_worker_task) {
        if (xTaskCreate(markets_worker_task, "markets_worker", 6144, NULL, 4, &s_worker_task) != pdPASS) {
            s_worker_task = NULL;
            return;
        }
    }
}

int Markets_Count(void)
{
    lock_state();
    int c = s_count;
    unlock_state();
    return c;
}

bool Markets_Get(int index, MarketQuote* out)
{
    if (!out) return false;
    lock_state();
    if (!s_list) {
        unlock_state();
        return false;
    }
    if (index < 0 || index >= s_count) {
        unlock_state();
        return false;
    }
    *out = s_list[index];
    bool ok = out->valid;
    unlock_state();
    return ok;
}

bool Markets_HasAnyValid(void)
{
    lock_state();
    bool ok = s_has_any_valid;
    unlock_state();
    return ok;
}

const char* Markets_LastStatus(void)
{
    lock_state();
    static char copy[48];
    strncpy(copy, s_last_status, sizeof(copy) - 1);
    copy[sizeof(copy) - 1] = 0;
    unlock_state();
    return copy;
}
