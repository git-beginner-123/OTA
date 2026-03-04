#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include "esp_log.h"
#include "esp_spiffs.h"
#if __has_include("esp_rom_tjpgd.h")
#include "esp_rom_tjpgd.h"
#elif __has_include("rom/tjpgd.h")
#include "rom/tjpgd.h"
#else
#error "No TJpgDec ROM header found"
#endif

#include <ctype.h>
#include <dirent.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char* TAG = "EXP_UART_HIST";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26
#define MAX_IMAGES 32
#define MAX_PATH_LEN 96

static const char* kPrimaryDir = "/spiffs/spanish_history";
static const char* kFallbackDir = "/spiffs";

typedef struct {
    const uint8_t* p;
    size_t len;
    size_t off;
    int base_x;
    int base_y;
} JpgInput;

static bool s_running = false;
static bool s_spiffs_mounted = false;

static char s_images[MAX_IMAGES][MAX_PATH_LEN];
static int s_image_count = 0;
static int s_idx = 0;

static uint8_t* s_file_buf = NULL;
static size_t s_file_cap = 0;
static uint16_t* s_pixbuf = NULL;
static size_t s_pixbuf_cap_px = 0;
static uint8_t* s_jpeg_work = NULL;
static size_t s_jpeg_work_cap = 0;

static JpgInput s_jpg_in = {0};

static uint16_t c_bg(void) { return Ui_ColorRGB(8, 12, 20); }
static uint16_t c_text(void) { return Ui_ColorRGB(235, 235, 235); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 130, 130); }

static const char* jdr_name(JRESULT rc)
{
    switch (rc) {
    case JDR_OK: return "JDR_OK";
    case JDR_INTR: return "JDR_INTR";
    case JDR_INP: return "JDR_INP";
    case JDR_MEM1: return "JDR_MEM1";
    case JDR_MEM2: return "JDR_MEM2";
    case JDR_PAR: return "JDR_PAR";
    case JDR_FMT1: return "JDR_FMT1";
    case JDR_FMT2: return "JDR_FMT2";
    case JDR_FMT3: return "JDR_FMT3";
    default: return "JDR_UNKNOWN";
    }
}

static bool has_jpeg_ext(const char* name)
{
    if (!name) return false;
    size_t n = strlen(name);
    if (n < 5) return false;

    const char* ext = strrchr(name, '.');
    if (!ext || ext == name) return false;
    ext++;

    char e[6] = {0};
    size_t i = 0;
    while (ext[i] && i < sizeof(e) - 1) {
        e[i] = (char)tolower((unsigned char)ext[i]);
        i++;
    }
    e[i] = '\0';

    return strcmp(e, "jpg") == 0 || strcmp(e, "jpeg") == 0;
}

static void sort_paths(char arr[MAX_IMAGES][MAX_PATH_LEN], int count)
{
    for (int i = 1; i < count; i++) {
        char key[MAX_PATH_LEN];
        memcpy(key, arr[i], sizeof(key));
        int j = i - 1;
        while (j >= 0 && strcmp(arr[j], key) > 0) {
            memcpy(arr[j + 1], arr[j], MAX_PATH_LEN);
            j--;
        }
        memcpy(arr[j + 1], key, MAX_PATH_LEN);
    }
}

static bool mount_spiffs_once(void)
{
    if (s_spiffs_mounted) return true;
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "spiffs",
        .max_files = 5,
        .format_if_mount_failed = false,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spiffs mount failed: %s", esp_err_to_name(err));
        return false;
    }
    s_spiffs_mounted = true;
    return true;
}

static bool collect_jpegs_from_dir(const char* dir)
{
    DIR* d = opendir(dir);
    if (!d) return false;

    s_image_count = 0;
    struct dirent* ent = NULL;
    while ((ent = readdir(d)) != NULL) {
        if (ent->d_name[0] == '\0') continue;
        if (!has_jpeg_ext(ent->d_name)) continue;
        if (s_image_count >= MAX_IMAGES) break;

        int n = snprintf(s_images[s_image_count], MAX_PATH_LEN, "%s/%s", dir, ent->d_name);
        if (n <= 0 || n >= MAX_PATH_LEN) continue;
        s_image_count++;
    }
    closedir(d);

    if (s_image_count > 1) sort_paths(s_images, s_image_count);
    return s_image_count > 0;
}

static bool discover_images(void)
{
    if (collect_jpegs_from_dir(kPrimaryDir)) {
        ESP_LOGI(TAG, "found %d images in %s", s_image_count, kPrimaryDir);
        return true;
    }
    if (collect_jpegs_from_dir(kFallbackDir)) {
        ESP_LOGW(TAG, "fallback images from %s", kFallbackDir);
        return true;
    }
    ESP_LOGW(TAG, "no jpeg found in %s or %s", kPrimaryDir, kFallbackDir);
    return false;
}

static bool ensure_file_buffer(size_t cap)
{
    if (s_file_buf && s_file_cap >= cap) return true;
    if (s_file_buf) free(s_file_buf);
    s_file_buf = (uint8_t*)malloc(cap);
    if (!s_file_buf) {
        s_file_cap = 0;
        return false;
    }
    s_file_cap = cap;
    return true;
}

static bool ensure_pixbuf_px(size_t px)
{
    if (s_pixbuf && s_pixbuf_cap_px >= px) return true;
    if (s_pixbuf) free(s_pixbuf);
    s_pixbuf = (uint16_t*)malloc(px * sizeof(uint16_t));
    if (!s_pixbuf) {
        s_pixbuf_cap_px = 0;
        return false;
    }
    s_pixbuf_cap_px = px;
    return true;
}

static bool ensure_jpeg_work_fallback(void)
{
    if (s_jpeg_work && s_jpeg_work_cap >= 8192) return true;
    if (s_jpeg_work) free(s_jpeg_work);

    static const size_t kTryCaps[] = { 16384, 12288, 8192 };
    for (int i = 0; i < (int)(sizeof(kTryCaps) / sizeof(kTryCaps[0])); i++) {
        s_jpeg_work = (uint8_t*)malloc(kTryCaps[i]);
        if (s_jpeg_work) {
            s_jpeg_work_cap = kTryCaps[i];
            ESP_LOGI(TAG, "jpeg work buffer=%u", (unsigned)kTryCaps[i]);
            return true;
        }
    }
    s_jpeg_work_cap = 0;
    return false;
}

static UINT hist_jd_input(JDEC* jd, uint8_t* buff, UINT nbyte)
{
    JpgInput* s = (JpgInput*)jd->device;
    size_t remain = (s->off < s->len) ? (s->len - s->off) : 0;
    if ((size_t)nbyte > remain) nbyte = (UINT)remain;
    if (nbyte == 0) return 0;
    if (buff) memcpy(buff, s->p + s->off, nbyte);
    s->off += nbyte;
    return nbyte;
}

static UINT hist_jd_output(JDEC* jd, void* bitmap, JRECT* rect)
{
    (void)jd;
    int w = (int)(rect->right - rect->left + 1);
    int h = (int)(rect->bottom - rect->top + 1);
    if (w <= 0 || h <= 0) return 1;

    size_t need_px = (size_t)w * (size_t)h;
    if (!ensure_pixbuf_px(need_px)) return 0;

    const uint8_t* px = (const uint8_t*)bitmap;
    uint16_t* dst = s_pixbuf;
#if defined(JD_FORMAT) && (JD_FORMAT == 1)
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
        *dst++ = ((uint16_t)px[0] << 8) | (uint16_t)px[1];
        px += 2;
    }
#else
    size_t npx = (size_t)w * (size_t)h;
    for (size_t i = 0; i < npx; i++) {
        uint8_t r = px[0];
        uint8_t g = px[1];
        uint8_t b = px[2];
        *dst++ = (uint16_t)((r & 0xF8) << 8) | (uint16_t)((g & 0xFC) << 3) | (uint16_t)(b >> 3);
        px += 3;
    }
#endif

    int dx = s_jpg_in.base_x + (int)rect->left;
    int dy = s_jpg_in.base_y + (int)rect->top;
    St7789_BlitRect(dx, dy, w, h, s_pixbuf);
    return 1;
}

static bool load_file_to_buffer(const char* path, const uint8_t** out, size_t* out_len)
{
    *out = NULL;
    *out_len = 0;

    FILE* fp = fopen(path, "rb");
    if (!fp) return false;

    if (fseek(fp, 0, SEEK_END) != 0) {
        fclose(fp);
        return false;
    }
    long flen = ftell(fp);
    if (flen <= 4) {
        fclose(fp);
        return false;
    }
    if (fseek(fp, 0, SEEK_SET) != 0) {
        fclose(fp);
        return false;
    }

    size_t len = (size_t)flen;
    if (!ensure_file_buffer(len)) {
        fclose(fp);
        return false;
    }

    size_t got = fread(s_file_buf, 1, len, fp);
    fclose(fp);
    if (got != len) return false;

    if (!(s_file_buf[0] == 0xFF && s_file_buf[1] == 0xD8)) return false;

    *out = s_file_buf;
    *out_len = len;
    return true;
}

static void draw_caption(const char* path)
{
    const char* base = strrchr(path, '/');
    base = base ? (base + 1) : path;

    int y = St7789_Height() - UI_FOOTER_H + 6;
    char line[64];
    snprintf(line, sizeof(line), "%d/%d %s", s_idx + 1, s_image_count, base);
    St7789_FillRect(0, St7789_Height() - UI_FOOTER_H + 1, St7789_Width(), UI_FOOTER_H - 2, c_bg());
    Ui_DrawTextAtBg(4, y, line, c_text(), c_bg());
}

static void draw_message(const char* msg, uint16_t col)
{
    int body_y = UI_HEADER_H;
    int body_h = St7789_Height() - UI_HEADER_H - UI_FOOTER_H;
    St7789_FillRect(0, body_y, St7789_Width(), body_h, c_bg());
    Ui_DrawTextAtBg(8, body_y + 8, msg, col, c_bg());
    St7789_Flush();
}

static void draw_current_image(void)
{
    if (s_image_count <= 0 || s_idx < 0 || s_idx >= s_image_count) {
        draw_message("No JPEG files", c_warn());
        return;
    }

    const char* path = s_images[s_idx];
    const uint8_t* jpg = NULL;
    size_t jpg_len = 0;
    if (!load_file_to_buffer(path, &jpg, &jpg_len)) {
        ESP_LOGW(TAG, "load failed: %s", path);
        draw_message("Load JPEG failed", c_warn());
        return;
    }

    JDEC dec;
    s_jpg_in.p = jpg;
    s_jpg_in.len = jpg_len;
    s_jpg_in.off = 0;

    JRESULT rc = jd_prepare(&dec, hist_jd_input, s_jpeg_work, (UINT)s_jpeg_work_cap, &s_jpg_in);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_prepare err=%d(%s)", (int)rc, jdr_name(rc));
        draw_message("JPEG decode prepare failed", c_warn());
        return;
    }

    int body_x = 0;
    int body_y = UI_HEADER_H;
    int body_w = St7789_Width();
    int body_h = St7789_Height() - UI_HEADER_H - UI_FOOTER_H;

    uint8_t scale = 0;
    int draw_w = dec.width;
    int draw_h = dec.height;
    while (scale < 3 && (draw_w > body_w || draw_h > body_h)) {
        scale++;
        draw_w = (dec.width + (1 << scale) - 1) >> scale;
        draw_h = (dec.height + (1 << scale) - 1) >> scale;
    }

    s_jpg_in.base_x = body_x + (body_w - draw_w) / 2;
    s_jpg_in.base_y = body_y + (body_h - draw_h) / 2;

    St7789_FillRect(body_x, body_y, body_w, body_h, c_bg());
    draw_caption(path);

    rc = jd_decomp(&dec, hist_jd_output, scale);
    if (rc != JDR_OK) {
        ESP_LOGW(TAG, "jd_decomp err=%d(%s)", (int)rc, jdr_name(rc));
        draw_message("JPEG decode failed", c_warn());
        return;
    }

    St7789_Flush();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("HISTORY", "UP/DN:PAGE BACK:EXIT");
    Ui_Println("Show Spanish history JPEGs.");
    Ui_Println("Folder: /spiffs/spanish_history");
    Ui_Println("Fallback: /spiffs/*.jpg");
    Ui_Println("UP/DOWN to flip pages.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    St7789_ApplyPanelDefaultProfile();
    s_idx = 0;
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("HISTORY", "UP/DN:PAGE BACK:EXIT");

    if (!mount_spiffs_once()) {
        draw_message("SPIFFS mount failed", c_warn());
        s_running = false;
        return;
    }
    if (!ensure_jpeg_work_fallback()) {
        draw_message("JPEG work alloc failed", c_warn());
        s_running = false;
        return;
    }
    if (!discover_images()) {
        draw_message("No JPEG in /spiffs/spanish_history", c_warn());
        s_running = true;
        return;
    }

    s_running = true;
    if (s_idx >= s_image_count) s_idx = 0;
    draw_current_image();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;
    if (s_image_count <= 0) return;

    if (key == kInputUp) {
        s_idx--;
        if (s_idx < 0) s_idx = s_image_count - 1;
        draw_current_image();
    } else if (key == kInputDown) {
        s_idx++;
        if (s_idx >= s_image_count) s_idx = 0;
        draw_current_image();
    }
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;

    if (s_file_buf) {
        free(s_file_buf);
        s_file_buf = NULL;
        s_file_cap = 0;
    }
    if (s_pixbuf) {
        free(s_pixbuf);
        s_pixbuf = NULL;
        s_pixbuf_cap_px = 0;
    }
    if (s_jpeg_work) {
        free(s_jpeg_work);
        s_jpeg_work = NULL;
        s_jpeg_work_cap = 0;
    }
    if (s_spiffs_mounted) {
        esp_vfs_spiffs_unregister("spiffs");
        s_spiffs_mounted = false;
    }
}

const Experiment g_exp_history = {
    .id = 4,
    .title = "HISTORY",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = 0,
};
