#include "ui/ui.h"
#include "display/st7789.h"
#include "display/font8x16.h"
#include "display/font5x7.h"
#include "esp_log.h"

#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "experiments/experiment.h"
#include "experiments/experiments_registry.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"



// Theme
#define UI_COLOR_BG        ((uint16_t)0x0861)
#define UI_COLOR_TEXT      ((uint16_t)0xEF7D)
#define UI_COLOR_MUTED     ((uint16_t)0x9CF3)
#define UI_COLOR_ACCENT    ((uint16_t)0x2D7F)
#define UI_COLOR_HILITE_BG ((uint16_t)0x114A)
#define UI_COLOR_HILITE_TX ((uint16_t)0xFFFF)

#define UI_PAD_X        10
#define UI_PAD_Y         6

#define UI_FONT_W        8
#define UI_FONT_H       16
#define UI_CHAR_GAP      1
#define UI_LINE_GAP      4
#define UI_LINE_H       (UI_FONT_H + UI_LINE_GAP)

#define UI_HEADER_H     30
#define UI_FOOTER_H     26
#define UI_BAR_W         5

#define UI_LINEBUF_COUNT 1

static int s_cursor_y = 0;
static int s_batch_depth = 0;

static uint16_t* s_linebuf[UI_LINEBUF_COUNT];
static int s_linebuf_idx = 0;
static int s_linebuf_h = 0;
static bool s_mainmenu_cache_valid = false;
static int s_mainmenu_last_index = -1;
static int s_mainmenu_last_start = -1;
static int s_mainmenu_last_count = -1;
static int s_mainmenu_last_rows = -1;
static bool s_mic_cache_valid = false;
static int s_mic_last_count = -1;
static int s_mic_last_levels[16];
static int s_mic_last_vol = -1;
static int s_mic_last_freq = -1;
static bool s_bg_bmp_checked = false;
static bool s_bg_bmp_ok = false;
static const uint8_t* s_bg_bmp_bits = NULL;
static int s_bg_bmp_w = 0;
static int s_bg_bmp_h = 0;
static int s_bg_bmp_bpp = 0;
static int s_bg_bmp_row_stride = 0;
static bool s_bg_bmp_bottom_up = true;

#if UI_BG_BMP_EMBEDDED
extern const uint8_t _binary_bg_main_240x320_bmp_start[] asm("_binary_bg_main_240x320_bmp_start");
extern const uint8_t _binary_bg_main_240x320_bmp_end[] asm("_binary_bg_main_240x320_bmp_end");
#endif

static void Ui_MainMenuInvalidate(void)
{
    s_mainmenu_cache_valid = false;
    s_mainmenu_last_index = -1;
    s_mainmenu_last_start = -1;
    s_mainmenu_last_count = -1;
    s_mainmenu_last_rows = -1;
    s_mic_cache_valid = false;
    s_mic_last_count = -1;
    s_mic_last_vol = -1;
    s_mic_last_freq = -1;
    for (int i = 0; i < 16; i++) s_mic_last_levels[i] = -1;
}

static void Ui_DrawListRow(int y, const char* text, bool selected);
static void Ui_DrawWrappedTextBody(const char* text, int scroll_line);
static SemaphoreHandle_t s_lcd_mutex = 0;

void Ui_LcdLock(void)
{
    if (s_lcd_mutex) xSemaphoreTake(s_lcd_mutex, portMAX_DELAY);
}

void Ui_LcdUnlock(void)
{
    if (s_lcd_mutex) xSemaphoreGive(s_lcd_mutex);
}

static inline void Ui_MaybeFlush(void)
{
    if (s_batch_depth <= 0) St7789_Flush();
}

void Ui_BeginBatch(void)
{
    s_batch_depth++;
}

void Ui_EndBatch(void)
{
    if (s_batch_depth > 0) s_batch_depth--;
    if (s_batch_depth == 0) St7789_Flush();
}


static void Ui_LineBufInit(int line_h)
{
    if (s_linebuf_h == line_h && s_linebuf[0]) return;

    for (int i = 0; i < UI_LINEBUF_COUNT; i++) {
        if (s_linebuf[i]) {
            heap_caps_free(s_linebuf[i]);
            s_linebuf[i] = NULL;
        }
    }

    s_linebuf_h = line_h;
    int w = St7789_Width();
    int bytes = w * line_h * (int)sizeof(uint16_t);

    for (int i = 0; i < UI_LINEBUF_COUNT; i++) {
        // Use normal 8-bit heap to avoid consuming scarce DMA/internal memory
        // needed by WiFi softAP and audio drivers.
        s_linebuf[i] = (uint16_t*)heap_caps_malloc(bytes, MALLOC_CAP_8BIT);
        if (!s_linebuf[i]) {
            ESP_LOGW("UI_LCD", "linebuf alloc failed idx=%d bytes=%d", i, bytes);
        }
    }

    s_linebuf_idx = 0;
}

static uint16_t* Ui_LineBufNext(void)
{
    uint16_t* p = s_linebuf[s_linebuf_idx];
    s_linebuf_idx = (s_linebuf_idx + 1) % UI_LINEBUF_COUNT;
    if (!p) {
        for (int i = 0; i < UI_LINEBUF_COUNT; i++) {
            if (s_linebuf[i]) return s_linebuf[i];
        }
    }
    return p;
}

static inline void LineBufFill(uint16_t* buf, int w, int h, uint16_t c)
{
    if (!buf) return;
    int n = w * h;
    for (int i = 0; i < n; i++) buf[i] = c;
}

static inline void LineBufFillRect(uint16_t* buf, int w, int h,
                                   int x, int y, int rw, int rh, uint16_t c)
{
    if (!buf) return;
    if (rw <= 0 || rh <= 0) return;
    if (x < 0) { rw += x; x = 0; }
    if (y < 0) { rh += y; y = 0; }
    if (x + rw > w) rw = w - x;
    if (y + rh > h) rh = h - y;
    if (rw <= 0 || rh <= 0) return;

    for (int yy = 0; yy < rh; yy++) {
        uint16_t* row = buf + (y + yy) * w + x;
        for (int xx = 0; xx < rw; xx++) row[xx] = c;
    }
}

// RGB565 helpers (RGB order; panel set to BGR via MADCTL)
#define RGB565(r,g,b) (uint16_t)((((r) & 0xF8) << 8) | (((g) & 0xFC) << 3) | (((b) & 0xF8) >> 3))

typedef enum {
    kLampOrderRGB = 0,
    kLampOrderBGR,
    kLampOrderGRB,
    kLampOrderGBR,
    kLampOrderBRG,
    kLampOrderRBG,
} LampColorOrder;

// Keep RGB order here; panel correction is handled separately (SW/HW invert + RB swap)
#define LAMP_COLOR_ORDER kLampOrderRGB

static const char* kUiTag = "UI_LCD";

static uint16_t read_le16(const uint8_t* p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_le32(const uint8_t* p)
{
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static int32_t read_le32s(const uint8_t* p)
{
    return (int32_t)read_le32(p);
}

static bool Ui_TryParseMainMenuBmp(void)
{
    if (s_bg_bmp_checked) return s_bg_bmp_ok;
    s_bg_bmp_checked = true;

#if UI_BG_BMP_EMBEDDED
    const uint8_t* data = _binary_bg_main_240x320_bmp_start;
    size_t size = (size_t)(_binary_bg_main_240x320_bmp_end - _binary_bg_main_240x320_bmp_start);
    if (!data || size < 54) return false;
    if (data[0] != 'B' || data[1] != 'M') return false;

    uint32_t off_bits = read_le32(data + 10);
    uint32_t dib_size = read_le32(data + 14);
    if (dib_size < 40) return false;
    int32_t w = read_le32s(data + 18);
    int32_t h = read_le32s(data + 22);
    uint16_t planes = read_le16(data + 26);
    uint16_t bpp = read_le16(data + 28);
    uint32_t comp = read_le32(data + 30);
    if (planes != 1 || comp != 0) return false;
    if (!(bpp == 24 || bpp == 32)) return false;
    if (w <= 0 || h == 0) return false;

    int src_w = w;
    int src_h = (h < 0) ? -h : h;
    bool bottom_up = (h > 0);
    int bytes_pp = (int)bpp / 8;
    int row_stride = ((src_w * bytes_pp + 3) / 4) * 4;
    size_t need = (size_t)off_bits + (size_t)row_stride * (size_t)src_h;
    if (need > size) return false;

    s_bg_bmp_bits = data + off_bits;
    s_bg_bmp_w = src_w;
    s_bg_bmp_h = src_h;
    s_bg_bmp_bpp = (int)bpp;
    s_bg_bmp_row_stride = row_stride;
    s_bg_bmp_bottom_up = bottom_up;
    s_bg_bmp_ok = true;
    ESP_LOGI(kUiTag, "main menu bmp enabled: %dx%d bpp=%d", s_bg_bmp_w, s_bg_bmp_h, s_bg_bmp_bpp);
    return true;
#else
    return false;
#endif
}

static bool Ui_MainMenuBmpPixelAt(int x, int y, int out_w, int out_h, uint16_t* out_color)
{
    if (!Ui_TryParseMainMenuBmp()) return false;
    if (!out_color) return false;
    if (out_w <= 0 || out_h <= 0) return false;

    int sx = (x * s_bg_bmp_w) / out_w;
    int sy = (y * s_bg_bmp_h) / out_h;
    if (sx < 0) sx = 0;
    if (sx >= s_bg_bmp_w) sx = s_bg_bmp_w - 1;
    if (sy < 0) sy = 0;
    if (sy >= s_bg_bmp_h) sy = s_bg_bmp_h - 1;
    int src_row = s_bg_bmp_bottom_up ? (s_bg_bmp_h - 1 - sy) : sy;
    int bytes_pp = s_bg_bmp_bpp / 8;
    const uint8_t* p = s_bg_bmp_bits + src_row * s_bg_bmp_row_stride + sx * bytes_pp;
    *out_color = Ui_ColorRGB(p[2], p[1], p[0]);
    return true;
}

static uint16_t Ui_LampColor(uint8_t r, uint8_t g, uint8_t b)
{
    switch (LAMP_COLOR_ORDER) {
        case kLampOrderBGR: return RGB565(b, g, r);
        case kLampOrderGRB: return RGB565(g, r, b);
        case kLampOrderGBR: return RGB565(g, b, r);
        case kLampOrderBRG: return RGB565(b, r, g);
        case kLampOrderRBG: return RGB565(r, b, g);
        case kLampOrderRGB:
        default:            return RGB565(r, g, b);
    }
}

// Lamp colors
#define RGB565_RED    Ui_LampColor(255, 0, 0)
#define RGB565_GREEN  Ui_LampColor(0, 255, 0)
#define RGB565_YELLOW Ui_LampColor(255, 255, 0)
#define RGB565_BLUE   Ui_LampColor(0, 0, 255)
#define RGB565_CYAN   Ui_LampColor(0, 255, 255)
#define RGB565_MAGENTA Ui_LampColor(255, 0, 255)
#define RGB565_WHITE  Ui_LampColor(255, 255, 255)
#define RGB565_BLACK  Ui_LampColor(0, 0, 0)

static void draw_char8x16_to_buf(uint16_t* buf, int bw, int bh, int x, int y, char c, uint16_t fg)
{
    if (!buf) return;
    if (x < 0 || y < 0) return;
    if (x + 8 > bw) return;
    if (y + 16 > bh) return;

    const uint8_t* rows = Font8x16_Get(c);
    if (!rows) rows = Font8x16_Get('?');
    if (!rows) return;

    for (int ry = 0; ry < 16; ry++) {
        uint8_t bits = rows[ry];
        uint16_t* dst = buf + (y + ry) * bw + x;
        for (int rx = 0; rx < 8; rx++) {
            if (bits & (0x80U >> rx)) dst[rx] = fg;
        }
    }
}

static void draw_text8x16_to_buf(uint16_t* buf, int bw, int bh, int x, int y, const char* s, uint16_t fg)
{
    int px = x;
    for (const char* p = s; *p; p++) {
        char c = *p;

        if (c == '\n') {
            y += UI_LINE_H;
            px = x;
            continue;
        }

        draw_char8x16_to_buf(buf, bw, bh, px, y, c, fg);
        px += (UI_FONT_W + UI_CHAR_GAP);

        if (px > bw - UI_FONT_W) {
            y += UI_LINE_H;
            px = x;
        }
    }
}

// 5x7 font rendering (used for small label images)
// 5x7 font rendering (optional small label images)
#define LABEL_FONT_W 5
#define LABEL_FONT_H 7
#define LABEL_GAP    1
#define LABEL_PAD_X  1
#define LABEL_PAD_Y  1
#define LABEL_H      (LABEL_FONT_H + (LABEL_PAD_Y * 2))
#define LABEL_MAX_W  40

static void draw_char5x7_to_buf(uint16_t* buf, int bw, int bh, int x, int y, char c, uint16_t fg)
{
    if (!buf) return;
    const uint8_t* cols = Font5x7_Get(c);
    if (!cols) return;

    for (int cx = 0; cx < LABEL_FONT_W; cx++) {
        uint8_t bits = cols[cx];
        for (int cy = 0; cy < LABEL_FONT_H; cy++) {
            if (bits & (1U << cy)) {
                int px = x + cx;
                int py = y + cy;
                if (px >= 0 && px < bw && py >= 0 && py < bh) {
                    buf[py * bw + px] = fg;
                }
            }
        }
    }
}

static void draw_text5x7_to_buf(uint16_t* buf, int bw, int bh, int x, int y, const char* s, uint16_t fg)
{
    int px = x;
    for (const char* p = s; *p; p++) {
        draw_char5x7_to_buf(buf, bw, bh, px, y, *p, fg);
        px += (LABEL_FONT_W + LABEL_GAP);
        if (px >= bw) break;
    }
}

static int Ui_LabelWidth(const char* text)
{
    int len = 0;
    for (const char* p = text; *p; p++) len++;
    if (len <= 0) return 0;
    return (len * LABEL_FONT_W) + ((len - 1) * LABEL_GAP) + (LABEL_PAD_X * 2);
}

static void Ui_DrawLabelImage(int x, int y, const char* text, uint16_t fg, uint16_t bg)
{
    int w = Ui_LabelWidth(text);
    if (w <= 0) return;
    if (w > LABEL_MAX_W) w = LABEL_MAX_W;

    uint16_t buf[LABEL_MAX_W * LABEL_H];
    LineBufFill(buf, w, LABEL_H, bg);
    draw_text5x7_to_buf(buf, w, LABEL_H, LABEL_PAD_X, LABEL_PAD_Y, text, fg);
    St7789_BlitRect(x, y, w, LABEL_H, buf);
}

static void Ui_DrawHeader(const char* title)
{
    St7789_FillRect(0, 0, St7789_Width(), UI_HEADER_H, UI_COLOR_BG);
    St7789_FillRect(0, UI_HEADER_H - 2, St7789_Width(), 2, UI_COLOR_MUTED);

    // Simple title text using a line buffer
    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    int w = St7789_Width();

    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, title, UI_COLOR_ACCENT);
    St7789_BlitRect(0, 6, w, UI_LINE_H, buf);

    s_cursor_y = UI_HEADER_H + UI_PAD_Y;
}

static void Ui_DrawFooter(const char* hint)
{
    int y = St7789_Height() - UI_FOOTER_H;
    St7789_FillRect(0, y, St7789_Width(), UI_FOOTER_H, UI_COLOR_BG);
    St7789_FillRect(0, y, St7789_Width(), 2, UI_COLOR_MUTED);

    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    int w = St7789_Width();

    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, hint, UI_COLOR_MUTED);
    St7789_BlitRect(0, y + 4, w, UI_LINE_H, buf);
}

static int Ui_Clamp(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int Ui_ListVisibleRows(void)
{
    int h = St7789_Height();
    int top = UI_HEADER_H + UI_PAD_Y;
    int bottom = h - UI_FOOTER_H - UI_PAD_Y;
    int avail = bottom - top;
    if (avail < UI_LINE_H) return 1;
    int rows = avail / UI_LINE_H;
    if (rows < 1) rows = 1;
    return rows;
}

static int Ui_ComputeWindowStart(int index, int count, int rows)
{
    if (count <= rows) return 0;

    int start = index - (rows / 2);
    start = Ui_Clamp(start, 0, count - rows);

    if (index < rows) start = 0;
    if (index > count - rows) start = count - rows;

    start = Ui_Clamp(start, 0, count - rows);
    return start;
}

static int Ui_ListTopY(void) { return UI_HEADER_H + UI_PAD_Y; }

static void Ui_ClearListAreaOnly(void)
{
    int top = Ui_ListTopY();
    int bottom = St7789_Height() - UI_FOOTER_H - UI_PAD_Y;
    int h = bottom - top;
    if (h > 0) St7789_FillRect(0, top, St7789_Width(), h, UI_COLOR_BG);
}

static const char* Ui_MainMenuTitle(const Experiment* exp)
{
    if (!exp) return "N/A";
    if (exp->id == 13) return "SEESAW";
    if (exp->id == 14) return "24 PUNTOS";
    if (exp->id == 15) return "BALL";
    const char* t = exp->title ? exp->title : "N/A";
#if defined(APP_VARIANT_GO)
    if (strncmp(t, "GO ", 3) == 0) return t + 3;
#endif
    return t;
}

static uint16_t Ui_MainMenuBgPixel(int x, int y, int w, int h)
{
    uint16_t bmp_c = 0;
    if (Ui_MainMenuBmpPixelAt(x, y, w, h, &bmp_c)) return bmp_c;

    int base = 244 - ((y * 22) / (h > 1 ? (h - 1) : 1));
    int noise = (int)(((uint32_t)(x * 1877 + y * 3253 + x * y) >> 4) & 0x1F) - 15;
    int fog = 10 - ((x * 14) / (w > 1 ? (w - 1) : 1));
    int r = base + noise / 3 + fog;
    int g = base + noise / 3 + fog;
    int b = base + noise / 2 + fog;
    if (r < 198) r = 198;
    if (r > 252) r = 252;
    if (g < 198) g = 198;
    if (g > 252) g = 252;
    if (b < 202) b = 202;
    if (b > 254) b = 254;
    return Ui_ColorRGB((uint8_t)r, (uint8_t)g, (uint8_t)b);
}

static void Ui_DrawLineInk(int x0, int y0, int x1, int y1, uint16_t color)
{
    int dx = (x1 > x0) ? (x1 - x0) : (x0 - x1);
    int sx = (x0 < x1) ? 1 : -1;
    int dy = -((y1 > y0) ? (y1 - y0) : (y0 - y1));
    int sy = (y0 < y1) ? 1 : -1;
    int err = dx + dy;
    while (1) {
        St7789_DrawPixel(x0, y0, color);
        if (x0 == x1 && y0 == y1) break;
        int e2 = err << 1;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void Ui_DrawMainMenuBackground(void)
{
    int w = St7789_Width();
    int h = St7789_Height();

    Ui_LineBufInit(1);
    for (int y = 0; y < h; y++) {
        uint16_t* buf = Ui_LineBufNext();
        if (!buf) continue;
        for (int x = 0; x < w; x++) {
            buf[x] = Ui_MainMenuBgPixel(x, y, w, h);
        }
        St7789_BlitRect(0, y, w, 1, buf);
    }

    if (Ui_TryParseMainMenuBmp()) return;

    // Red sun.
    uint16_t sun = Ui_ColorRGB(214, 48, 48);
    int sun_x = 44;
    int sun_y = 24;
    int sun_r = 12;
    for (int y = -sun_r; y <= sun_r; y++) {
        for (int x = -sun_r; x <= sun_r; x++) {
            if (x * x + y * y <= sun_r * sun_r) {
                St7789_DrawPixel(sun_x + x, sun_y + y, sun);
            }
        }
    }

    // Mountain ranges.
    uint16_t ink_far = Ui_ColorRGB(182, 182, 182);
    uint16_t ink_mid = Ui_ColorRGB(154, 154, 154);
    uint16_t ink_near = Ui_ColorRGB(120, 120, 120);
    for (int i = 0; i < 6; i++) {
        int y0 = 56 + i * 10;
        for (int x = 0; x < w - 7; x += 7) {
            int y_a = y0 - (x / 14) + (((x + i * 21) % 33) - 16) / 5;
            int y_b = y0 - ((x + 7) / 14) + ((((x + 7) + i * 21) % 33) - 16) / 5;
            Ui_DrawLineInk(x, y_a, x + 7, y_b, ink_far);
        }
    }
    for (int i = 0; i < 5; i++) {
        int y0 = 86 + i * 12;
        for (int x = 0; x < w - 9; x += 9) {
            int y_a = y0 - (x / 11) + (((x + i * 19) % 37) - 18) / 4;
            int y_b = y0 - ((x + 9) / 11) + ((((x + 9) + i * 19) % 37) - 18) / 4;
            Ui_DrawLineInk(x, y_a, x + 9, y_b, ink_mid);
        }
    }
    for (int i = 0; i < 4; i++) {
        int y0 = 118 + i * 12;
        for (int x = 10; x < w - 8; x += 8) {
            int y_a = y0 - (x / 10) + (((x + i * 13) % 29) - 14) / 3;
            int y_b = y0 - ((x + 8) / 10) + ((((x + 8) + i * 13) % 29) - 14) / 3;
            Ui_DrawLineInk(x, y_a, x + 8, y_b, ink_near);
        }
    }

    // Mist overlay.
    uint16_t fog = Ui_ColorRGB(236, 236, 236);
    for (int i = 0; i < 6; i++) {
        int fy = 126 + i * 14;
        St7789_FillRect(0, fy, w, 10, fog);
    }

    // Left-top bamboo.
    uint16_t bamboo = Ui_ColorRGB(26, 26, 26);
    Ui_DrawLineInk(0, 0, 30, 26, bamboo);
    Ui_DrawLineInk(10, 0, 40, 30, bamboo);
    Ui_DrawLineInk(22, 0, 48, 26, bamboo);
    for (int i = 0; i < 6; i++) {
        int sx = 8 + i * 7;
        Ui_DrawLineInk(sx, 8 + (i % 2) * 2, sx - 8, 16 + (i % 2) * 3, bamboo);
        Ui_DrawLineInk(sx, 10 + (i % 2) * 2, sx + 9, 18 + (i % 2) * 3, bamboo);
    }

    // Right-middle bamboo.
    Ui_DrawLineInk(w - 44, h - 88, w - 18, h - 52, bamboo);
    Ui_DrawLineInk(w - 58, h - 84, w - 30, h - 44, bamboo);
    for (int i = 0; i < 5; i++) {
        int sx = w - 54 + i * 8;
        int sy = h - 80 + i * 4;
        Ui_DrawLineInk(sx, sy, sx - 10, sy + 6, bamboo);
        Ui_DrawLineInk(sx, sy, sx + 9, sy + 7, bamboo);
    }

    // Boat.
    uint16_t boat = Ui_ColorRGB(74, 74, 74);
    int boat_x = 24;
    int boat_y = h - 72;
    St7789_FillRect(boat_x, boat_y, 24, 2, boat);
    St7789_FillRect(boat_x + 2, boat_y + 2, 20, 1, boat);
    Ui_DrawLineInk(boat_x + 10, boat_y, boat_x + 10, boat_y - 10, boat);
    Ui_DrawLineInk(boat_x + 10, boat_y - 10, boat_x + 18, boat_y - 6, boat);

    // Go board at bottom.
    int bw = 112;
    int bh = 76;
    int bx = (w - bw) / 2;
    int by = h - bh - 8;
    uint16_t board = Ui_ColorRGB(206, 206, 206);
    uint16_t grid = Ui_ColorRGB(156, 156, 156);
    St7789_FillRect(bx, by, bw, bh, board);
    for (int i = 0; i < 13; i++) {
        int gx = bx + 5 + i * 8;
        int gy = by + 5 + i * 5;
        St7789_FillRect(gx, by + 4, 1, bh - 8, grid);
        if (i < 12) St7789_FillRect(bx + 4, gy, bw - 8, 1, grid);
    }

    // Stones.
    uint16_t black = Ui_ColorRGB(22, 22, 22);
    uint16_t white = Ui_ColorRGB(248, 248, 248);
    St7789_FillRect(bx + 20, by + 28, 4, 4, black);
    St7789_FillRect(bx + 50, by + 42, 4, 4, white);
    St7789_FillRect(bx + 74, by + 22, 4, 4, black);

    // Bottom-right figure silhouette.
    uint16_t fig1 = Ui_ColorRGB(150, 124, 98);
    uint16_t fig2 = Ui_ColorRGB(102, 102, 102);
    St7789_FillRect(w - 58, h - 38, 18, 12, fig1);
    St7789_FillRect(w - 40, h - 34, 12, 9, fig2);
}

static const char* Ui_MainMenuHeaderText(void)
{
#if defined(APP_VARIANT_GO)
    return "GO";
#elif defined(APP_VARIANT_CHESS)
    return "CHESS";
#elif defined(APP_VARIANT_DICE)
    return "DICE";
#elif defined(APP_VARIANT_GOMOKU)
    return "GOMOKU";
#else
    return "GAME";
#endif
}

static void Ui_DrawMainMenuHeader(void)
{
    const char* title = Ui_MainMenuHeaderText();
    int w = St7789_Width();
    int len = (int)strlen(title);
    int text_w = len * (UI_FONT_W + UI_CHAR_GAP);
    if (text_w > 0) text_w -= UI_CHAR_GAP;
    int x = (w - text_w) / 2;
    int y = 14;

    St7789_FillRect(0, y - 4, w, UI_LINE_H + 8, Ui_ColorRGB(222, 222, 222));
    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    if (!buf) return;
    LineBufFill(buf, w, UI_LINE_H, Ui_ColorRGB(222, 222, 222));
    draw_text8x16_to_buf(buf, w, UI_LINE_H, x, 2, title, Ui_ColorRGB(24, 24, 24));
    St7789_BlitRect(0, y, w, UI_LINE_H, buf);
}

static void Ui_DrawMainMenuTextRowAt(int y, const char* text, bool selected)
{
    int w = St7789_Width();
    int h = St7789_Height();
    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    if (!buf) return;

    for (int yy = 0; yy < UI_LINE_H; yy++) {
        int sy = y + yy;
        for (int x = 0; x < w; x++) {
            buf[yy * w + x] = Ui_MainMenuBgPixel(x, sy, w, h);
        }
    }

    char line[40];
    if (!text) text = "";
    if (selected) snprintf(line, sizeof(line), "> %s", text);
    else snprintf(line, sizeof(line), "  %s", text);
    uint16_t fg = selected ? Ui_ColorRGB(156, 28, 28) : Ui_ColorRGB(20, 20, 20);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, 18, 2, line, fg);

    St7789_BlitRect(0, y, w, UI_LINE_H, buf);
}

static void Ui_DrawMainMenuListRow(int y, int item_index, int count, bool selected)
{
    if (item_index < 0 || item_index >= count) {
        Ui_DrawMainMenuTextRowAt(y, "", false);
        return;
    }

    const Experiment* exp = Experiments_GetByIndex(item_index);
    const char* title = Ui_MainMenuTitle(exp);
    Ui_DrawMainMenuTextRowAt(y, title, selected);
}


void Ui_Init(void)
{
    if (!s_lcd_mutex) {
        s_lcd_mutex = xSemaphoreCreateMutex();
    }   
    St7789_Init();
    St7789_ApplyPanelDefaultProfile();
    ESP_LOGI(kUiTag, "Lamp color order = %d", (int)LAMP_COLOR_ORDER);
    Ui_LineBufInit(UI_LINE_H);
    St7789_Fill(UI_COLOR_BG);
    // Avoid an extra full-screen flush before the first menu draw.
}

void Ui_Clear(void)
{
    s_cursor_y = 0;
    St7789_Fill(UI_COLOR_BG);
    St7789_Flush();
}

void Ui_Println(const char* s)
{
    Ui_LineBufInit(UI_LINE_H);

    int w = St7789_Width();
    uint16_t* buf = Ui_LineBufNext();

    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, s, UI_COLOR_TEXT);

    St7789_BlitRect(0, s_cursor_y, w, UI_LINE_H, buf);
    s_cursor_y += UI_LINE_H;
}

void Ui_Printf(const char* fmt, ...)
{
    char buf[128];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Ui_Println(buf);
}

// -----------------------------
// Menus
// -----------------------------
void Ui_DrawMainMenu(int index, int count)
{
    Ui_LcdLock();

    if (count < 0) count = 0;
    if (count > 0) {
        if (index < 0) index = 0;
        if (index >= count) index = count - 1;
    } else {
        index = 0;
    }

    int h = St7789_Height();
    int row_h = UI_LINE_H;
    int row_gap = 10;
    int top_y = 58;
    int rows = (h - top_y - 14) / (row_h + row_gap);
    if (rows < 1) rows = 1;
    int start = Ui_ComputeWindowStart(index, count, rows);

    bool need_full_redraw = (!s_mainmenu_cache_valid ||
                             s_mainmenu_last_count != count ||
                             s_mainmenu_last_start != start ||
                             s_mainmenu_last_rows != rows);

    if (need_full_redraw) {
        Ui_DrawMainMenuBackground();
        Ui_DrawMainMenuHeader();
        for (int r = 0; r < rows; r++) {
            int i = start + r;
            int y = top_y + r * (row_h + row_gap);
            Ui_DrawMainMenuListRow(y, i, count, (i == index));
        }
    } else if (s_mainmenu_last_index != index) {
        int old_row = s_mainmenu_last_index - start;
        int new_row = index - start;
        if (old_row >= 0 && old_row < rows) {
            int y = top_y + old_row * (row_h + row_gap);
            Ui_DrawMainMenuListRow(y, s_mainmenu_last_index, count, false);
        }
        if (new_row >= 0 && new_row < rows) {
            int y = top_y + new_row * (row_h + row_gap);
            Ui_DrawMainMenuListRow(y, index, count, true);
        }
    }

    s_mainmenu_cache_valid = true;
    s_mainmenu_last_index = index;
    s_mainmenu_last_start = start;
    s_mainmenu_last_count = count;
    s_mainmenu_last_rows = rows;

    St7789_Flush();
    Ui_LcdUnlock();
}
static __attribute__((unused)) const char* Ui_ExperimentTitleByIndex(int i)
{
    const Experiment* e = Experiments_GetByIndex(i);
    return (e && e->title) ? e->title : "";
}

typedef const char* (*Ui_GetItemTextFn)(int index);

static __attribute__((unused)) void Ui_DrawBodyList(int selected, int count, Ui_GetItemTextFn get_text)
{
    if (count < 0) count = 0;
    if (selected < 0) selected = 0;
    if (selected >= count && count > 0) selected = count - 1;

    int rows = Ui_ListVisibleRows();
    if (rows <= 0) rows = 1;

    int start = Ui_ComputeWindowStart(selected, count, rows);
    int y = Ui_ListTopY();

    for (int r = 0; r < rows; r++) {
        int i = start + r;

        char line[32];
        if (i < count) {
            const char* s = (get_text != 0) ? get_text(i) : "";
            if (s == 0) s = "";
            snprintf(line, sizeof(line), "%2d  %s", i + 1, s);
            Ui_DrawListRow(y, line, (i == selected));
        } else {
            Ui_DrawListRow(y, "", 0);
        }
        y += UI_LINE_H;
    }
}
void Ui_DrawFrame(const char* header_title, const char* footer_hint)
{
    Ui_MainMenuInvalidate();
    Ui_Clear();
    Ui_DrawHeader(header_title);
    Ui_DrawFooter(footer_hint);
    St7789_Flush();
}

uint16_t Ui_ColorRGB(uint8_t r, uint8_t g, uint8_t b)
{
    return Ui_LampColor(r, g, b);
}

void Ui_DrawBodyClear(void)
{
    int body_y = UI_HEADER_H;
    int body_h = St7789_Height() - UI_HEADER_H - UI_FOOTER_H;
    St7789_FillRect(0, body_y, St7789_Width(), body_h, UI_COLOR_BG);
    Ui_MaybeFlush();
}

void Ui_DrawBodyTextRowColor(int row, const char* text, uint16_t fg)
{
    if (row < 0) return;

    int body_y = UI_HEADER_H + UI_PAD_Y;
    int y = body_y + row * UI_LINE_H;
    int w = St7789_Width();

    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();

    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, text ? text : "", fg);

    St7789_BlitRect(0, y, w, UI_LINE_H, buf);
    Ui_MaybeFlush();
}

void Ui_DrawBodyTextRowTwoColor(int row, const char* left, const char* right,
                                uint16_t left_fg, uint16_t right_fg)
{
    if (row < 0) return;

    int body_y = UI_HEADER_H + UI_PAD_Y;
    int y = body_y + row * UI_LINE_H;
    int w = St7789_Width();

    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();

    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);

    // Left label
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, left ? left : "", left_fg);

    // Right value block (fixed column)
    int value_col = 5;
    int value_x = UI_PAD_X + value_col * (UI_FONT_W + UI_CHAR_GAP);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, value_x, 2, right ? right : "", right_fg);

    St7789_BlitRect(0, y, w, UI_LINE_H, buf);
    Ui_MaybeFlush();
}

void Ui_DrawTextAt(int x, int y, const char* text, uint16_t fg)
{
    Ui_DrawTextAtBg(x, y, text, fg, UI_COLOR_BG);
}

void Ui_DrawTextAtBg(int x, int y, const char* text, uint16_t fg, uint16_t bg)
{
    if (!text) text = "";
    int len = 0;
    for (const char* p = text; *p; p++) len++;

    int w = len * (UI_FONT_W + UI_CHAR_GAP);
    if (len > 0) w -= UI_CHAR_GAP;
    if (w < UI_FONT_W) w = UI_FONT_W;

    int max_w = St7789_Width() - x;
    if (max_w < 1) return;
    if (w > max_w) w = max_w;

    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, bg);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, 0, 2, text, fg);
    St7789_BlitRect(x, y, w, UI_LINE_H, buf);
    Ui_MaybeFlush();
}
void Ui_DrawExperimentRun(const char* title)
{
    Ui_LcdLock();
    Ui_MainMenuInvalidate();

    Ui_Clear();
    Ui_DrawHeader(title);

    Ui_Println("RUNNING...");
    Ui_DrawFooter("BACK=RET");
    St7789_Flush();

    Ui_LcdUnlock();
}


static __attribute__((unused)) int Ui_BodyCols(void)
{
    int cols = St7789_Width() / 8;
    if (cols < 8) cols = 8;
    return cols;
}

static __attribute__((unused)) int Ui_BodyRows(void)
{
    int rows = Ui_ListVisibleRows();
    if (rows <= 0) rows = 1;
    return rows;
}

static __attribute__((unused)) const char* Ui_SkipWrappedLines(const char* text, int cols, int skip_lines)
{
    const char* p = text;
    int line = 0;

    while (*p && line < skip_lines) {
        int n = 0;
        while (*p && *p != '\n' && n < cols) {
            p++;
            n++;
        }
        if (*p == '\n') p++;
        line++;
    }
    return p;
}


static const char* Experiments_GetDescription(const Experiment* exp)
{
    (void)exp;
    return "No description.\n"
           "Please implement\n"
           "exp->show_requirements().";
}

void Ui_DrawExperimentMenu(const char* title, const Experiment* exp, int scroll_line)
{
    Ui_LcdLock();

    if (exp && exp->show_requirements) {
        ExperimentContext dummy = (ExperimentContext){0};
        exp->show_requirements(&dummy);
    } else {
        Ui_DrawFrame(title, "UP/DN: SCROLL  OK: ENTER  BACK: RETURN");
        const char* text = Experiments_GetDescription(exp);
        Ui_DrawWrappedTextBody(text, scroll_line);
    }

    St7789_Flush();

    Ui_LcdUnlock();
}

void Ui_DrawMazeFullScreen(void)
{
    Ui_LcdLock();
    Ui_MainMenuInvalidate();
    Ui_Clear();
    St7789_Flush();
    Ui_LcdUnlock();
}

typedef struct {
    int x;
    int y;
    int w;
    int h;
} UiRect;

static int Ui_RectCols(UiRect r)
{
    int usable = r.w - (UI_PAD_X * 2);
    if (usable < 0) usable = 0;
    int cols = usable / 8; // 8 px per glyph
    if (cols < 1) cols = 1;
    return cols;
}

static void Ui_DrawListRowInRect(UiRect r, int y, const char* text, bool selected)
{
    Ui_LineBufInit(UI_LINE_H);

    int w = r.w;
    int h = UI_LINE_H;

    uint16_t* buf = Ui_LineBufNext();

    uint16_t row_bg = selected ? UI_COLOR_HILITE_BG : UI_COLOR_BG;
    uint16_t row_fg = selected ? UI_COLOR_HILITE_TX : UI_COLOR_TEXT;

    LineBufFill(buf, w, h, row_bg);

    // Selection bar inside the row (keep your current look)
    uint16_t bar = selected ? UI_COLOR_ACCENT : UI_COLOR_MUTED;
    LineBufFillRect(buf, w, h, 0, 0, UI_BAR_W, h, bar);

    draw_text8x16_to_buf(buf, w, h, UI_PAD_X, 2, text, row_fg);

    St7789_BlitRect(r.x, y, w, h, buf);
}

static void Ui_DrawListRow(int y, const char* text, bool selected)
{
    UiRect r = { .x = 0, .y = 0, .w = St7789_Width(), .h = St7789_Height() };
    Ui_DrawListRowInRect(r, y, text, selected);
}

// Copies next wrapped line into out (null-terminated).
// Returns pointer to the next position in text after this line.
static const char* Ui_WrapNextLine(const char* p, int cols, char* out, int out_cap)
{
    if (!p) p = "";
    if (cols < 1) cols = 1;
    if (out_cap < 2) { if (out_cap == 1) out[0] = 0; return p; }

    // Skip leading spaces/tabs (but not newlines)
    while (*p == ' ' || *p == '\t') p++;

    // If immediate newline, produce empty line and consume it
    if (*p == '\n') {
        out[0] = 0;
        return p + 1;
    }

    int maxc = cols;
    if (maxc > out_cap - 1) maxc = out_cap - 1;

    int n = 0;
    int last_space_out = -1;   // index in out
    const char* last_space_p = NULL;

    while (*p && *p != '\n' && n < maxc) {
        char ch = *p;
        out[n++] = ch;

        if (ch == ' ' || ch == '\t') {
            last_space_out = n - 1;
            last_space_p = p;
        }
        p++;
    }

    // If we stopped because of newline, just consume it
    if (*p == '\n') {
        out[n] = 0;
        return p + 1;
    }

    // If we filled maxc and there is still text on the same logical line,
    // try to break at last space to avoid cutting a word.
    if (n == maxc && *p && *p != '\n' && last_space_out >= 0) {
        // Trim trailing spaces in out
        int cut = last_space_out;
        while (cut > 0 && (out[cut] == ' ' || out[cut] == '\t')) cut--;
        out[cut + 1] = 0;

        // Continue after the space
        p = last_space_p + 1;
        while (*p == ' ' || *p == '\t') p++;
        return p;
    }

    // Otherwise hard cut
    out[n] = 0;
    return p;
}

static const char* Ui_SkipWrappedLinesEx(const char* text, int cols, int skip_lines)
{
    const char* p = text ? text : "";
    char tmp[64];

    for (int i = 0; i < skip_lines && *p; i++) {
        p = Ui_WrapNextLine(p, cols, tmp, (int)sizeof(tmp));
    }
    return p;
}

static void Ui_DrawWrappedTextInRect(const char* text, int scroll_line, UiRect body)
{
    if (!text) text = "";

    // Clear body
    St7789_FillRect(body.x, body.y, body.w, body.h, UI_COLOR_BG);

    int cols = Ui_RectCols(body);

    int usable_h = body.h - (UI_PAD_Y * 2);
    if (usable_h < UI_LINE_H) usable_h = UI_LINE_H;
    int rows = usable_h / UI_LINE_H;
    if (rows < 1) rows = 1;

    int y = body.y + UI_PAD_Y;

    const char* p = Ui_SkipWrappedLinesEx(text, cols, scroll_line);

    for (int r = 0; r < rows; r++) {
        char line[64];
        p = Ui_WrapNextLine(p, cols, line, (int)sizeof(line));

        Ui_DrawListRowInRect(body, y, line, false);
        y += UI_LINE_H;

        if (!*p) break;
    }
}

static void Ui_DrawWrappedTextBody(const char* text, int scroll_line)
{
    UiRect body = { .x = 0, .y = UI_HEADER_H, .w = St7789_Width(),
                    .h = St7789_Height() - UI_HEADER_H - UI_FOOTER_H };
    Ui_DrawWrappedTextInRect(text, scroll_line, body);
}

static __attribute__((unused)) void Ui_DrawGpioRow(int row, int selected,
                           const char* name, uint16_t color, bool on, int gpio_num,
                           int top, int row_h, int w)
{
    int lamp = 12;
    int lamp_x = 8;
    int text_x = lamp_x + lamp + 8;

    int ry = top + row * row_h;

    if (row == selected) {
        St7789_FillRect(0, ry - 2, w, row_h, UI_COLOR_HILITE_BG);
    }

    uint16_t lamp_color = on ? color : UI_COLOR_MUTED;
    St7789_FillRect(lamp_x, ry + 2, lamp, lamp, lamp_color);

    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();

    LineBufFill(buf, w, UI_LINE_H, (row == selected) ? UI_COLOR_HILITE_BG : UI_COLOR_BG);

    char line[48];
    snprintf(line, sizeof(line), "%s  GPIO%d   [%s]", name, gpio_num, on ? "ON" : "OFF");

    uint16_t fg = (row == selected) ? UI_COLOR_HILITE_TX : UI_COLOR_TEXT;
    draw_text8x16_to_buf(buf, w, UI_LINE_H, text_x, 2, line, fg);

    St7789_BlitRect(0, ry, w, UI_LINE_H, buf);
}







static void Ui_DrawLampIcon(int x, int y, int size, uint16_t fill, uint16_t outline, uint16_t bg)
{
    if (size <= 2) return;
    if (size > 24) size = 24;

    uint16_t buf[24 * 24];
    int w = size;
    int h = size;

    LineBufFill(buf, w, h, bg);

    int r = size / 2 - 1;
    int cx = r;
    int cy = r;

    for (int py = 0; py < h; py++) {
        for (int px = 0; px < w; px++) {
            int dx = px - cx;
            int dy = py - cy;
            int d2 = dx * dx + dy * dy;
            int r2 = r * r;
            int r2_in = (r - 1) * (r - 1);
            if (d2 <= r2_in) {
                buf[py * w + px] = fill;
            } else if (d2 <= r2) {
                buf[py * w + px] = outline;
            }
        }
    }

    // Simple lamp base
    int base_w = size / 2;
    int base_h = size / 5;
    int base_x = (size - base_w) / 2;
    int base_y = size - base_h;
    for (int py = base_y; py < size; py++) {
        for (int px = base_x; px < base_x + base_w; px++) {
            buf[py * w + px] = outline;
        }
    }

    St7789_BlitRect(x, y, w, h, buf);
}

static uint16_t Ui_ColorScale(uint16_t rgb565, int pct)
{
    if (pct <= 0) return RGB565(0, 0, 0);
    if (pct >= 100) return rgb565;

    int r = (rgb565 >> 11) & 0x1F;
    int g = (rgb565 >> 5) & 0x3F;
    int b = rgb565 & 0x1F;

    r = (r * pct) / 100;
    g = (g * pct) / 100;
    b = (b * pct) / 100;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

void Ui_DrawGpioBody(int selected, bool red_on, bool green_on, bool yellow_on)
{
    // Body area: below header, above footer
    int w = St7789_Width();
    int body_y = UI_HEADER_H;
    int body_h = St7789_Height() - UI_HEADER_H - UI_FOOTER_H;

    // Clear only body (not full screen)
    St7789_FillRect(0, body_y, w, body_h, UI_COLOR_BG);

    // Layout
    int row_h = UI_LINE_H + 8;
    int top = UI_HEADER_H + 10;

    int lamp_size = 18;
    int lamp_x = 6;
    int label_x = lamp_x + lamp_size + 6;

    // Helper: draw one row
    for (int r = 0; r < 3; r++) {
        int ry = top + r * row_h;

        bool on = false;
        uint16_t lamp_color = UI_COLOR_MUTED;
        const char* name = "";
        int gpio_num = 0;

        if (r == 0) { name = "RED";    gpio_num = 13; on = red_on;    lamp_color = on ? RGB565_RED    : UI_COLOR_MUTED; }
        if (r == 1) { name = "GREEN";  gpio_num = 14; on = green_on;  lamp_color = on ? RGB565_GREEN  : UI_COLOR_MUTED; }
        if (r == 2) { name = "YELLOW"; gpio_num = 1;  on = yellow_on; lamp_color = on ? RGB565_YELLOW : UI_COLOR_MUTED; }

        // Selection highlight background bar
        uint16_t bg = (r == selected) ? UI_COLOR_HILITE_BG : UI_COLOR_BG;
        uint16_t fg = (r == selected) ? UI_COLOR_HILITE_TX : UI_COLOR_TEXT;

        // Row background (full width)
        St7789_FillRect(0, ry - 2, w, row_h, bg);

        // Prepare text line buffer first (background + text)
        int label_w = Ui_LabelWidth(name);
        int text_x = label_x + label_w + 6;

        Ui_LineBufInit(UI_LINE_H);
        uint16_t* buf = Ui_LineBufNext();
        LineBufFill(buf, w, UI_LINE_H, bg);

        char line[48];
        snprintf(line, sizeof(line), "GPIO%-2d  [%s]", gpio_num, on ? "ON" : "OFF");
        draw_text8x16_to_buf(buf, w, UI_LINE_H, text_x, 2, line, fg);

        St7789_BlitRect(0, ry, w, UI_LINE_H, buf);

        // Lamp image (colored icon) and label on top
        int ly = ry + (UI_LINE_H - lamp_size) / 2;
        uint16_t lamp_fill = on ? lamp_color : UI_COLOR_MUTED;
        uint16_t lamp_outline = (r == selected) ? UI_COLOR_HILITE_TX : UI_COLOR_TEXT;
        Ui_DrawLampIcon(lamp_x, ly, lamp_size, lamp_fill, lamp_outline, bg);

        int label_y = ry + (UI_LINE_H - LABEL_H) / 2;
        Ui_DrawLabelImage(label_x, label_y, name, fg, bg);
    }
}

void Ui_DrawPwmBody(int selected, int red_pct, int green_pct, int yellow_pct, int freq_hz)
{
    int w = St7789_Width();

    int row_h = UI_LINE_H + 8;
    int top = UI_HEADER_H + 10;

    int lamp_size = 18;
    int lamp_x = 6;
    int label_x = lamp_x + lamp_size + 6;
    int bar_w = 48;
    int bar_h = 6;

    for (int r = 0; r < 3; r++) {
        int ry = top + r * row_h;

        const char* name = "";
        int pct = 0;
        uint16_t base = UI_COLOR_MUTED;

        if (r == 0) { name = "RED";    pct = red_pct;    base = RGB565_RED; }
        if (r == 1) { name = "GREEN";  pct = green_pct;  base = RGB565_GREEN; }
        if (r == 2) { name = "YELLOW"; pct = yellow_pct; base = RGB565_YELLOW; }

        uint16_t bg = (r == selected) ? UI_COLOR_HILITE_BG : UI_COLOR_BG;
        uint16_t fg = (r == selected) ? UI_COLOR_HILITE_TX : UI_COLOR_TEXT;

        St7789_FillRect(0, ry - 2, w, row_h, bg);

        int label_w = Ui_LabelWidth(name);
        int text_x = label_x + label_w + 6;

        Ui_LineBufInit(UI_LINE_H);
        uint16_t* buf = Ui_LineBufNext();
        LineBufFill(buf, w, UI_LINE_H, bg);

        char line[48];
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        snprintf(line, sizeof(line), "PWM %3d%%", pct);
        draw_text8x16_to_buf(buf, w, UI_LINE_H, text_x, 2, line, fg);
        St7789_BlitRect(0, ry, w, UI_LINE_H, buf);

        int ly = ry + (UI_LINE_H - lamp_size) / 2;
        uint16_t lamp_fill = (pct > 0) ? Ui_ColorScale(base, pct) : UI_COLOR_MUTED;
        uint16_t lamp_outline = (r == selected) ? UI_COLOR_HILITE_TX : UI_COLOR_TEXT;
        Ui_DrawLampIcon(lamp_x, ly, lamp_size, lamp_fill, lamp_outline, bg);

        int label_y = ry + (UI_LINE_H - LABEL_H) / 2;
        Ui_DrawLabelImage(label_x, label_y, name, fg, bg);

        int bar_x = w - UI_PAD_X - bar_w;
        int bar_y = ry + (UI_LINE_H - bar_h) / 2;
        St7789_FillRect(bar_x, bar_y, bar_w, bar_h, UI_COLOR_MUTED);
        int fill_w = (bar_w * pct) / 100;
        if (fill_w > 0) {
            St7789_FillRect(bar_x, bar_y, fill_w, bar_h, Ui_ColorScale(base, pct));
        }
    }

    int ry = top + 3 * row_h + 2;
    uint16_t bg = (selected == 3) ? UI_COLOR_HILITE_BG : UI_COLOR_BG;
    uint16_t fg = (selected == 3) ? UI_COLOR_HILITE_TX : UI_COLOR_TEXT;

    St7789_FillRect(0, ry - 2, w, row_h, bg);

    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, bg);

    char line[48];
    snprintf(line, sizeof(line), "FREQ  %4d Hz", freq_hz);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, line, fg);
    St7789_BlitRect(0, ry, w, UI_LINE_H, buf);
}

void Ui_DrawMicBody(const int* bands, int band_count, int freq_hz, int vol_pct)
{
    int w = St7789_Width();
    int body_y = UI_HEADER_H;
    int body_h = St7789_Height() - UI_HEADER_H - UI_FOOTER_H;

    int text_y = body_y + UI_PAD_Y;

    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);

    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    int show_vol = vol_pct;
    int show_freq = freq_hz;
    if (s_mic_cache_valid) {
        int dv = show_vol - s_mic_last_vol;
        if (dv < 0) dv = -dv;
        int df = show_freq - s_mic_last_freq;
        if (df < 0) df = -df;
        // Hysteresis: ignore tiny text changes to reduce flicker.
        if (dv < 2) show_vol = s_mic_last_vol;
        if (df < 250) show_freq = s_mic_last_freq;
    }

    int bar_h = 8;
    int bar_y = body_y + body_h - UI_PAD_Y - bar_h;
    int spec_y0 = text_y + UI_LINE_H + 6;
    int spec_y1 = bar_y - 6;
    if (spec_y1 < spec_y0 + 10) {
        spec_y1 = spec_y0 + 10;
        if (spec_y1 > body_y + body_h - 2) spec_y1 = body_y + body_h - 2;
    }

    int spec_h = spec_y1 - spec_y0;
    int spec_h_draw = (spec_h * 78) / 100;
    if (spec_h_draw < 8) spec_h_draw = spec_h;
    int count = band_count;
    if (count < 1) count = 1;
    if (count > 16) count = 16;

    int gap = 4;
    int total_gap = (count - 1) * gap;
    int slot_w = (w - (UI_PAD_X * 2) - total_gap) / count;
    if (slot_w < 4) slot_w = 4;
    int bar_w = slot_w - 2; // narrower bars
    if (bar_w < 2) bar_w = 2;
    int start_x = UI_PAD_X;
    uint16_t spec_bg = Ui_ColorRGB(18, 24, 34);
    uint16_t spec_bar_bg = Ui_ColorRGB(40, 52, 68);
    uint16_t spec_fill = Ui_ColorRGB(86, 190, 140);

    bool need_full_spec_clear = (!s_mic_cache_valid || s_mic_last_count != count);
    if (need_full_spec_clear) {
        St7789_FillRect(UI_PAD_X, spec_y0, w - (UI_PAD_X * 2), spec_h, spec_bg);
        for (int i = 0; i < 16; i++) s_mic_last_levels[i] = -1;
    }

    if (!s_mic_cache_valid || s_mic_last_freq != show_freq || s_mic_last_vol != show_vol) {
        St7789_FillRect(0, text_y, w, UI_LINE_H, UI_COLOR_BG);
        LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
        char line[64];
        snprintf(line, sizeof(line), "FREQ %4d Hz   VOL %3d%%", show_freq, show_vol);
        draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, line, UI_COLOR_TEXT);
        St7789_BlitRect(0, text_y, w, UI_LINE_H, buf);
    }

    for (int i = 0; i < count; i++) {
        int level = 0;
        if (bands && i < band_count) level = bands[i];
        if (level < 0) level = 0;
        if (level > 100) level = 100;
        if (s_mic_cache_valid && s_mic_last_levels[i] >= 0) {
            int dd = level - s_mic_last_levels[i];
            if (dd < 0) dd = -dd;
            // Ignore tiny per-band jitter.
            if (dd < 2) level = s_mic_last_levels[i];
        }

        if (level != s_mic_last_levels[i] || need_full_spec_clear) {
            int x_slot = start_x + i * (slot_w + gap);
            int x = x_slot + (slot_w - bar_w) / 2;
            int fill_h = (spec_h_draw * level) / 100;
            // Redraw this changed column completely to avoid residual "peak shadow".
            St7789_FillRect(x, spec_y0, bar_w, spec_h, spec_bar_bg);
            if (fill_h > 0) {
                int y = spec_y1 - fill_h;
                St7789_FillRect(x, y, bar_w, fill_h, spec_fill);
            }
            s_mic_last_levels[i] = level;
        }
    }

    int vol_bar_w = w - (UI_PAD_X * 2) - 40;
    if (vol_bar_w < 20) vol_bar_w = 20;
    int vol_bar_x = UI_PAD_X + 40;

    if (!s_mic_cache_valid || s_mic_last_vol != show_vol || s_mic_last_freq != show_freq) {
        Ui_LineBufInit(UI_LINE_H);
        buf = Ui_LineBufNext();
        LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
        draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, "VOL", UI_COLOR_MUTED);
        St7789_BlitRect(0, bar_y - 6, w, UI_LINE_H, buf);

        St7789_FillRect(vol_bar_x, bar_y, vol_bar_w, bar_h, UI_COLOR_MUTED);
        int fill_w = (vol_bar_w * show_vol) / 100;
        if (fill_w > 0) {
            St7789_FillRect(vol_bar_x, bar_y, fill_w, bar_h, UI_COLOR_ACCENT);
        }
    }

    s_mic_cache_valid = true;
    s_mic_last_count = count;
    s_mic_last_vol = show_vol;
    s_mic_last_freq = show_freq;
}

void Ui_DrawSpeakerBody(bool playing, int vol_pct, int progress_pct, uint32_t sample_rate_hz)
{
    int w = St7789_Width();
    int body_y = UI_HEADER_H;

    int y = body_y + UI_PAD_Y;
    Ui_LineBufInit(UI_LINE_H);
    uint16_t* buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);

    char line[48];
    snprintf(line, sizeof(line), "STATUS : %s", playing ? "PLAY" : "STOP");
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, line, UI_COLOR_TEXT);
    St7789_BlitRect(0, y, w, UI_LINE_H, buf);
    y += UI_LINE_H + 6;

    if (vol_pct < 0) vol_pct = 0;
    if (vol_pct > 100) vol_pct = 100;
    if (progress_pct < 0) progress_pct = 0;
    if (progress_pct > 100) progress_pct = 100;

    Ui_LineBufInit(UI_LINE_H);
    buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    snprintf(line, sizeof(line), "VOL    : %3d%%", vol_pct);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, line, UI_COLOR_TEXT);
    St7789_BlitRect(0, y, w, UI_LINE_H, buf);

    int bar_y = y + UI_LINE_H + 6;
    int bar_h = 10;
    int bar_w = w - (UI_PAD_X * 2);
    int bar_x = UI_PAD_X;
    St7789_FillRect(bar_x, bar_y, bar_w, bar_h, UI_COLOR_MUTED);
    int fill_w = (bar_w * vol_pct) / 100;
    if (fill_w > 0) {
        St7789_FillRect(bar_x, bar_y, fill_w, bar_h, UI_COLOR_ACCENT);
    }

    y = bar_y + bar_h + 10;
    Ui_LineBufInit(UI_LINE_H);
    buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    snprintf(line, sizeof(line), "PROG   : %3d%%", progress_pct);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, line, UI_COLOR_TEXT);
    St7789_BlitRect(0, y, w, UI_LINE_H, buf);

    int pbar_y = y + UI_LINE_H + 6;
    int pbar_h = 10;
    int pbar_w = w - (UI_PAD_X * 2);
    int pbar_x = UI_PAD_X;
    St7789_FillRect(pbar_x, pbar_y, pbar_w, pbar_h, UI_COLOR_MUTED);
    int pfill_w = (pbar_w * progress_pct) / 100;
    if (pfill_w > 0) {
        St7789_FillRect(pbar_x, pbar_y, pfill_w, pbar_h, Ui_ColorRGB(92, 184, 92));
    }

    y = pbar_y + pbar_h + 10;
    Ui_LineBufInit(UI_LINE_H);
    buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    if (sample_rate_hz == 22050U) {
        draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, "SRATE  : 22.05k", UI_COLOR_TEXT);
    } else {
        draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, "SRATE  : 16k", UI_COLOR_TEXT);
    }
    St7789_BlitRect(0, y, w, UI_LINE_H, buf);

    y += UI_LINE_H + 4;
    Ui_LineBufInit(UI_LINE_H);
    buf = Ui_LineBufNext();
    LineBufFill(buf, w, UI_LINE_H, UI_COLOR_BG);
    draw_text8x16_to_buf(buf, w, UI_LINE_H, UI_PAD_X, 2, "Hola soy espanol.", UI_COLOR_TEXT);
    St7789_BlitRect(0, y, w, UI_LINE_H, buf);
}

void Ui_DrawColorTestBody(int selected, bool sw_invert, bool sw_rb_swap, bool hw_invert)
{
    int w = St7789_Width();
    int body_y = UI_HEADER_H;
    int body_h = St7789_Height() - UI_HEADER_H - UI_FOOTER_H;

    St7789_FillRect(0, body_y, w, body_h, UI_COLOR_BG);

    UiRect body = { .x = 0, .y = body_y, .w = w, .h = body_h };

    int y = UI_HEADER_H + UI_PAD_Y;
    char line[48];

    snprintf(line, sizeof(line), "SW_INV   : %s", sw_invert ? "ON" : "OFF");
    Ui_DrawListRowInRect(body, y, line, selected == 0);
    y += UI_LINE_H;

    snprintf(line, sizeof(line), "SW_RB_SW : %s", sw_rb_swap ? "ON" : "OFF");
    Ui_DrawListRowInRect(body, y, line, selected == 1);
    y += UI_LINE_H;

    snprintf(line, sizeof(line), "HW_INV   : %s", hw_invert ? "ON" : "OFF");
    Ui_DrawListRowInRect(body, y, line, selected == 2);
    y += UI_LINE_H + 6;

    int size = 22;
    int gap = 8;
    int x0 = 8;
    int x1 = x0 + size + gap;
    int x2 = x1 + size + gap;

    int y0 = y;
    int y1 = y0 + size + 10;

    St7789_FillRect(x0, y0, size, size, RGB565_RED);
    St7789_FillRect(x1, y0, size, size, RGB565_GREEN);
    St7789_FillRect(x2, y0, size, size, RGB565_BLUE);

    St7789_FillRect(x0, y1, size, size, RGB565_YELLOW);
    St7789_FillRect(x1, y1, size, size, RGB565_CYAN);
    St7789_FillRect(x2, y1, size, size, RGB565_MAGENTA);

    Ui_DrawLabelImage(x0 + 4, y0 + size + 2, "R", UI_COLOR_TEXT, UI_COLOR_BG);
    Ui_DrawLabelImage(x1 + 4, y0 + size + 2, "G", UI_COLOR_TEXT, UI_COLOR_BG);
    Ui_DrawLabelImage(x2 + 4, y0 + size + 2, "B", UI_COLOR_TEXT, UI_COLOR_BG);

    Ui_DrawLabelImage(x0 + 4, y1 + size + 2, "Y", UI_COLOR_TEXT, UI_COLOR_BG);
    Ui_DrawLabelImage(x1 + 4, y1 + size + 2, "C", UI_COLOR_TEXT, UI_COLOR_BG);
    Ui_DrawLabelImage(x2 + 4, y1 + size + 2, "M", UI_COLOR_TEXT, UI_COLOR_BG);
}
