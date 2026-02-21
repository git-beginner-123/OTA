#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/sfx.h"
#include "display/st7789.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <stdio.h>

static const char* TAG = "EXP_SEESAW";

static int s_left_a = 0;
static int s_left_b = 0;
static int s_right = 0;
static int s_correct = 0;
static int s_total = 0;
static char s_last_result[48] = "UP/DN pick, ENTER confirm";
static char s_pick = '=';
static bool s_feedback_pending = false;
static uint32_t s_feedback_until_ms = 0;
static bool s_last_correct = false;

#define FEEDBACK_MS 850U

#define SHAPE_BUF_MAX 64
static uint16_t s_shape_buf[SHAPE_BUF_MAX * SHAPE_BUF_MAX];

static void draw_box(int x, int y, int w, int h, uint16_t bg, uint16_t border)
{
    St7789_FillRect(x, y, w, h, bg);
    St7789_FillRect(x, y, w, 2, border);
    St7789_FillRect(x, y + h - 2, w, 2, border);
    St7789_FillRect(x, y, 2, h, border);
    St7789_FillRect(x + w - 2, y, 2, h, border);
}

static void set_buf_px(uint16_t* buf, int w, int h, int x, int y, uint16_t c)
{
    if (x < 0 || y < 0 || x >= w || y >= h) return;
    buf[y * w + x] = c;
}

static void draw_circle_outline(int cx, int cy, int r, uint16_t fg, uint16_t bg)
{
    int d = r * 2 + 1;
    if (d < 3 || d > SHAPE_BUF_MAX) return;
    for (int i = 0; i < d * d; i++) s_shape_buf[i] = bg;

    int x = r;
    int y = 0;
    int err = 1 - x;

    while (x >= y) {
        set_buf_px(s_shape_buf, d, d, r + x, r + y, fg);
        set_buf_px(s_shape_buf, d, d, r + y, r + x, fg);
        set_buf_px(s_shape_buf, d, d, r - y, r + x, fg);
        set_buf_px(s_shape_buf, d, d, r - x, r + y, fg);
        set_buf_px(s_shape_buf, d, d, r - x, r - y, fg);
        set_buf_px(s_shape_buf, d, d, r - y, r - x, fg);
        set_buf_px(s_shape_buf, d, d, r + y, r - x, fg);
        set_buf_px(s_shape_buf, d, d, r + x, r - y, fg);

        y++;
        if (err < 0) err += 2 * y + 1;
        else {
            x--;
            err += 2 * (y - x + 1);
        }
    }

    St7789_BlitRect(cx - r, cy - r, d, d, s_shape_buf);
}

static void draw_triangle_support(int cx, int top_y, int h, uint16_t c)
{
    for (int i = 0; i < h; i++) {
        int half = i;
        int y = top_y + i;
        int x = cx - half;
        int w = half * 2 + 1;
        St7789_FillRect(x, y, w, 1, c);
    }
}

static void draw_beam(int x, int y, int w, int h, int tilt, uint16_t c)
{
    int y_l = y - tilt;
    int y_r = y + tilt;
    int slices = 24;
    for (int i = 0; i < slices; i++) {
        int sx = x + (w * i) / slices;
        int ex = x + (w * (i + 1)) / slices;
        int sw = ex - sx;
        if (sw < 1) sw = 1;
        int sy = y_l + ((y_r - y_l) * i) / (slices - 1);
        St7789_FillRect(sx, sy, sw, h, c);
    }
}

static int rand_range(int minv, int maxv)
{
    if (maxv <= minv) return minv;
    uint32_t span = (uint32_t)(maxv - minv + 1);
    return minv + (int)(esp_random() % span);
}

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static char relation_char(int left, int right)
{
    if (left > right) return '>';
    if (left < right) return '<';
    return '=';
}

static void next_question(void)
{
    s_left_a = rand_range(0, 9);
    s_left_b = rand_range(0, 9);
    int sum = s_left_a + s_left_b;

    for (int i = 0; i < 8; i++) {
        int target = rand_range(0, 2); // 0:>, 1:<, 2:=
        if (target == 0 && sum > 0) {
            s_right = rand_range(0, sum - 1);
            return;
        }
        if (target == 1 && sum < 18) {
            s_right = rand_range(sum + 1, 18);
            return;
        }
        if (target == 2) {
            s_right = sum;
            return;
        }
    }

    s_right = sum;
}

static void draw_full(void)
{
    int sum = s_left_a + s_left_b;
    char relation = relation_char(sum, s_right);
    int tilt = 0;
    // Show user's current pick on the seesaw itself.
    if (s_pick == '>') tilt = 9;      // left lower
    else if (s_pick == '<') tilt = -9; // right lower

    uint16_t bg = Ui_ColorRGB(8, 15, 26);
    uint16_t panel = Ui_ColorRGB(18, 28, 42);
    uint16_t line = Ui_ColorRGB(239, 125, 74);
    uint16_t beam = Ui_ColorRGB(235, 235, 235);
    uint16_t txt = Ui_ColorRGB(239, 125, 74);

    char nbuf[8];
    char score_line[24];

    Ui_LcdLock();
    Ui_DrawFrame("SEESAW GAME", "UP/DN:PICK ENTER:OK BACK");
    Ui_DrawBodyClear();
    St7789_FillRect(0, 30, St7789_Width(), St7789_Height() - 56, bg);

    // Left side: two boxes and one operator circle.
    draw_box(16, 54, 34, 28, panel, line);
    draw_box(58, 54, 34, 28, panel, line);
    draw_circle_outline(50, 68, 10, line, bg); // '+' circle

    snprintf(nbuf, sizeof(nbuf), "%d", s_left_a);
    Ui_DrawTextAtBg(27, 60, nbuf, txt, panel);
    snprintf(nbuf, sizeof(nbuf), "%d", s_left_b);
    Ui_DrawTextAtBg(69, 60, nbuf, txt, panel);
    Ui_DrawTextAtBg(47, 60, "+", txt, bg);

    // Right side: number circle.
    draw_circle_outline(192, 68, 16, line, bg);
    snprintf(nbuf, sizeof(nbuf), "%d", s_right);
    if (s_right >= 10) Ui_DrawTextAtBg(185, 60, nbuf, txt, bg);
    else Ui_DrawTextAtBg(189, 60, nbuf, txt, bg);

    // Seesaw beam + support.
    draw_beam(36, 122, 168, 10, tilt, beam);
    draw_circle_outline(120, 128, 12, line, bg);        // pivot ring
    draw_triangle_support(120, 140, 16, line);          // support

    // Current player pick symbol at pivot.
    char pick_txt[2] = {s_pick, '\0'};
    Ui_DrawTextAtBg(117, 120, pick_txt, txt, bg);

    snprintf(score_line, sizeof(score_line), "Score: %d/%d", s_correct, s_total);
    Ui_DrawTextAtBg(18, 208, score_line, txt, bg);
    Ui_DrawTextAtBg(18, 228, s_last_result, s_last_correct ? Ui_ColorRGB(120, 230, 130) : txt, bg);
    Ui_LcdUnlock();
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    s_correct = 0;
    s_total = 0;
    s_pick = '=';
    s_feedback_pending = false;
    s_feedback_until_ms = 0;
    s_last_correct = false;
    snprintf(s_last_result, sizeof(s_last_result), "UP/DN pick, ENTER confirm");
    next_question();
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
    draw_full();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    char choice = 0;
    if (s_feedback_pending) return;
    if (key == kInputUp) {
        if (s_pick == '>') s_pick = '=';
        else if (s_pick == '=') s_pick = '<';
        else s_pick = '>';
        draw_full();
        return;
    } else if (key == kInputDown) {
        if (s_pick == '>') s_pick = '<';
        else if (s_pick == '<') s_pick = '=';
        else s_pick = '>';
        draw_full();
        return;
    } else if (key == kInputEnter) {
        choice = s_pick;
    } else return;

    int left_sum = s_left_a + s_left_b;
    char answer = relation_char(left_sum, s_right);

    s_total++;
    if (choice == answer) {
        s_correct++;
        s_last_correct = true;
        snprintf(s_last_result, sizeof(s_last_result), "Your %c, ans %c: CORRECT", choice, answer);
        Sfx_PlaySuccess();
    } else {
        s_last_correct = false;
        snprintf(s_last_result, sizeof(s_last_result), "Your %c, ans %c: WRONG", choice, answer);
        Sfx_PlayFailure();
    }

    s_feedback_pending = true;
    s_feedback_until_ms = now_ms() + FEEDBACK_MS;
    draw_full();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("SEESAW GAME", "OK:START  BACK");
    Ui_Println("Goal: compare left/right.");
    Ui_Println("Left value is a+b.");
    Ui_Println("UP/DN: pick > = <");
    Ui_Println("ENTER: confirm answer.");
    Ui_Println("Show result, then next.");
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (s_feedback_pending && now_ms() >= s_feedback_until_ms) {
        s_feedback_pending = false;
        s_feedback_until_ms = 0;
        s_last_correct = false;
        s_pick = '=';
        next_question();
        draw_full();
    }
}

const Experiment g_exp_seesaw = {
    .id = 13,
    .title = "SEESAW GAME",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
