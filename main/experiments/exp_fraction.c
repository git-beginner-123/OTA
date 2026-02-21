#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include "esp_log.h"

#include <math.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char* TAG = "EXP_FRACTION";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26
#define PI_F 3.14159265f

typedef struct {
    int n;
    int d;
    int n2;
    int d2;
} FractionItem;

static const FractionItem kItems[] = {
    {1, 2, 2, 4},
    {1, 3, 2, 6},
    {2, 3, 4, 6},
    {3, 4, 6, 8},
    {2, 5, 4, 10},
};

static int s_idx = 0;
static bool s_show_equiv = false;

static uint16_t c_bg(void) { return Ui_ColorRGB(12, 16, 30); }
static uint16_t c_ring(void) { return Ui_ColorRGB(200, 220, 240); }
static uint16_t c_fill(void) { return Ui_ColorRGB(255, 180, 90); }
static uint16_t c_empty(void) { return Ui_ColorRGB(36, 56, 84); }
static uint16_t c_text(void) { return Ui_ColorRGB(240, 240, 240); }
static uint16_t c_accent(void) { return Ui_ColorRGB(120, 220, 140); }

static void draw_fraction_pizza(int cx, int cy, int r, int n, int d)
{
    if (d <= 0) return;
    for (int y = -r; y <= r; y++) {
        for (int x = -r; x <= r; x++) {
            if (x * x + y * y > r * r) continue;
            float a = atan2f((float)y, (float)x);
            a += PI_F * 0.5f; // start from top
            if (a < 0.0f) a += 2.0f * PI_F;
            int slice = (int)(a / (2.0f * PI_F / (float)d));
            uint16_t cc = (slice < n) ? c_fill() : c_empty();
            St7789_FillRect(cx + x, cy + y, 1, 1, cc);
        }
    }

    for (int deg = 0; deg < 360; deg++) {
        float a = (float)deg * (PI_F / 180.0f);
        int x = cx + (int)(cosf(a) * r);
        int y = cy + (int)(sinf(a) * r);
        St7789_FillRect(x, y, 1, 1, c_ring());
    }

    for (int i = 0; i < d; i++) {
        float a = -PI_F * 0.5f + ((2.0f * PI_F * (float)i) / (float)d);
        int x = cx + (int)(cosf(a) * r);
        int y = cy + (int)(sinf(a) * r);
        int dx = x - cx;
        int dy = y - cy;
        int steps = r;
        for (int t = 0; t <= steps; t++) {
            int px = cx + (dx * t) / steps;
            int py = cy + (dy * t) / steps;
            St7789_FillRect(px, py, 1, 1, c_ring());
        }
    }
}

static void draw_scene(void)
{
    int w = St7789_Width();
    int h = St7789_Height();
    int body_y = UI_HEADER_H;
    int body_h = h - UI_HEADER_H - UI_FOOTER_H;

    St7789_FillRect(0, body_y, w, body_h, c_bg());

    const FractionItem* it = &kItems[s_idx];
    int n = it->n;
    int d = it->d;
    if (s_show_equiv) {
        n = it->n2;
        d = it->d2;
    }

    int cx = w / 2;
    int cy = body_y + body_h / 2 + 16;
    int r = 56;
    draw_fraction_pizza(cx, cy, r, n, d);

    char line[48];
    snprintf(line, sizeof(line), "Fraction: %d/%d", n, d);
    Ui_DrawTextAtBg(8, UI_HEADER_H + 6, line, c_text(), c_bg());

    snprintf(line, sizeof(line), "Equal: %d/%d", it->n2, it->d2);
    Ui_DrawTextAtBg(8, UI_HEADER_H + 24, line, c_accent(), c_bg());
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("FRACTION", "OK:START  BACK");
    Ui_Println("Primary math demo 3.");
    Ui_Println("Fraction pizza visual.");
    Ui_Println("UP/DN choose fraction.");
    Ui_Println("OK switch equivalent form.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    s_idx = 0;
    s_show_equiv = false;
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    Ui_DrawFrame("FRACTION", "UP/DN:SEL OK:EQUAL BACK");
    draw_scene();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    int cnt = (int)(sizeof(kItems) / sizeof(kItems[0]));
    if (key == kInputUp) {
        s_idx--;
        if (s_idx < 0) s_idx = cnt - 1;
        s_show_equiv = false;
        draw_scene();
    } else if (key == kInputDown) {
        s_idx++;
        if (s_idx >= cnt) s_idx = 0;
        s_show_equiv = false;
        draw_scene();
    } else if (key == kInputEnter) {
        s_show_equiv = !s_show_equiv;
        draw_scene();
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
}

const Experiment g_exp_fraction = {
    .id = 22,
    .title = "FRACTION",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
