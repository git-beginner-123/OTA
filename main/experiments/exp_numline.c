#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char* TAG = "EXP_NUMLINE";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26

static int s_a = 3;
static int s_b = 4;
static int s_target = 7;
static int s_pos = 3;
static bool s_add = true;
static bool s_anim = false;
static bool s_answer_revealed = false;
static uint32_t s_next_step_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t c_bg(void) { return Ui_ColorRGB(10, 18, 28); }
static uint16_t c_axis(void) { return Ui_ColorRGB(200, 220, 240); }
static uint16_t c_tick(void) { return Ui_ColorRGB(120, 150, 180); }
static uint16_t c_dot_a(void) { return Ui_ColorRGB(110, 220, 110); }
static uint16_t c_dot_cur(void) { return Ui_ColorRGB(255, 190, 90); }
static uint16_t c_text(void) { return Ui_ColorRGB(240, 240, 240); }
static uint16_t c_hint(void) { return Ui_ColorRGB(255, 130, 130); }

static int value_to_x(int v)
{
    int left = 14;
    int right = St7789_Width() - 14;
    return left + (v * (right - left)) / 20;
}

static void draw_dot(int x, int y, int r, uint16_t col)
{
    for (int yy = -r; yy <= r; yy++) {
        for (int xx = -r; xx <= r; xx++) {
            if (xx * xx + yy * yy <= r * r) {
                St7789_FillRect(x + xx, y + yy, 1, 1, col);
            }
        }
    }
}

static void generate_question(bool force_add)
{
    s_add = force_add;
    s_a = (int)(esp_random() % 11U);
    s_b = 1 + (int)(esp_random() % 9U);

    if (s_add) {
        if (s_a + s_b > 20) s_b = 20 - s_a;
        if (s_b < 1) s_b = 1;
        s_target = s_a + s_b;
    } else {
        if (s_b > s_a) s_b = s_a;
        s_target = s_a - s_b;
    }

    s_pos = s_a;
    s_anim = false;
    s_answer_revealed = false;
}

static void draw_scene(void)
{
    int w = St7789_Width();
    int h = St7789_Height();
    int body_y = UI_HEADER_H;
    int body_h = h - UI_HEADER_H - UI_FOOTER_H;
    int axis_y = body_y + body_h / 2 + 20;

    St7789_FillRect(0, body_y, w, body_h, c_bg());

    int left = 14;
    int right = w - 14;
    St7789_FillRect(left, axis_y, right - left + 1, 2, c_axis());
    for (int v = 0; v <= 20; v++) {
        int x = value_to_x(v);
        int th = (v % 5 == 0) ? 10 : 6;
        St7789_FillRect(x, axis_y - th / 2, 1, th, c_tick());
    }

    int ax = value_to_x(s_a);
    int cx = value_to_x(s_pos);
    draw_dot(ax, axis_y - 14, 4, c_dot_a());
    draw_dot(cx, axis_y - 28, 5, c_dot_cur());

    char line[48];
    snprintf(line, sizeof(line), "%d %c %d = ?", s_a, s_add ? '+' : '-', s_b);
    Ui_DrawTextAtBg(8, UI_HEADER_H + 6, line, c_text(), c_bg());

    if (s_anim) {
        Ui_DrawTextAtBg(8, UI_HEADER_H + 24, "Animating...", c_hint(), c_bg());
    } else if (!s_answer_revealed) {
        Ui_DrawTextAtBg(8, UI_HEADER_H + 24, "Result: ?", c_text(), c_bg());
    } else {
        snprintf(line, sizeof(line), "Result: %d", s_target);
        Ui_DrawTextAtBg(8, UI_HEADER_H + 24, line, c_text(), c_bg());
    }
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("NUM LINE", "OK:PLAY  BACK");
    Ui_Println("Primary math demo 1.");
    Ui_Println("Number line add/sub.");
    Ui_Println("UP new + question.");
    Ui_Println("DN new - question.");
    Ui_Println("OK reveal answer + animation.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    generate_question(true);
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
    Ui_DrawFrame("NUM LINE", "UP:+ DN:- OK:SHOW BACK");
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
    if (key == kInputUp) {
        generate_question(true);
        draw_scene();
    } else if (key == kInputDown) {
        generate_question(false);
        draw_scene();
    } else if (key == kInputEnter) {
        s_answer_revealed = true;
        s_pos = s_a;
        s_anim = true;
        s_next_step_ms = 0;
        draw_scene();
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_anim) return;

    uint32_t t = now_ms();
    if (t < s_next_step_ms) return;
    s_next_step_ms = t + 260;

    if (s_pos < s_target) s_pos++;
    else if (s_pos > s_target) s_pos--;
    else s_anim = false;

    draw_scene();
}

const Experiment g_exp_numline = {
    .id = 21,
    .title = "NUM LINE",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
