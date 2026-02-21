#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/sfx.h"

#include "display/st7789.h"
#include "esp_log.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <math.h>
#include <stdio.h>

static const char* TAG = "EXP_BALL";

// Match ui_lcd constants
#define UI_HEADER_H 30
#define UI_FOOTER_H 26

typedef enum {
    kPhaseMenu = 0,
    kPhaseRun,
    kPhasePause,
} BallPhase;

typedef struct {
    float x;
    float y;
    float vx;
    float vy;
} Ball;

static BallPhase s_phase = kPhaseMenu;
static int s_speed_level = 1; // 0..2
static int s_score = 0;
static int s_hits = 0;
static int s_miss = 0;
static int s_rally = 0;

static int s_left = 8;
static int s_top = UI_HEADER_H + 58;
static int s_right = 232;
static int s_bottom = 286;

static int s_pad_x = 92;
static int s_pad_w = 56;
static int s_pad_h = 8;
static int s_pad_y = 272;
static int s_prev_pad_x = 92;

static Ball s_ball = {0};
static Ball s_prev_ball = {0};
static int s_ball_r = 4;

static bool s_full_redraw = true;
static bool s_hud_dirty = true;
static uint32_t s_next_tick_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t c_bg(void) { return Ui_ColorRGB(8, 14, 24); }
static uint16_t c_border(void) { return Ui_ColorRGB(120, 170, 220); }
static uint16_t c_paddle(void) { return Ui_ColorRGB(255, 210, 90); }
static uint16_t c_ball(void) { return Ui_ColorRGB(255, 255, 255); }
static uint16_t c_text(void) { return Ui_ColorRGB(230, 230, 230); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 120, 120); }

static float speed_value(int lv)
{
    if (lv <= 0) return 2.2f;
    if (lv == 1) return 3.0f;
    return 3.8f;
}

static float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void draw_ball(int x, int y, int r, uint16_t col)
{
    for (int yy = -r; yy <= r; yy++) {
        int rem = (int)sqrtf((float)(r * r - yy * yy));
        St7789_FillRect(x - rem, y + yy, rem * 2 + 1, 1, col);
    }
}

static void draw_static_field(void)
{
    Ui_DrawFrame("BALL", "UP/DN:MOVE  OK:START/PAUSE  BACK");
    Ui_DrawBodyClear();

    St7789_FillRect(0, UI_HEADER_H, St7789_Width(), St7789_Height() - UI_HEADER_H - UI_FOOTER_H, c_bg());

    // Closed border
    St7789_FillRect(s_left, s_top, s_right - s_left + 1, 2, c_border());
    St7789_FillRect(s_left, s_bottom - 1, s_right - s_left + 1, 2, c_border());
    St7789_FillRect(s_left, s_top, 2, s_bottom - s_top + 1, c_border());
    St7789_FillRect(s_right - 1, s_top, 2, s_bottom - s_top + 1, c_border());
}

static void draw_playfield_dynamic(void)
{
    // Draw order tuned for stable visuals:
    // erase old ball -> draw new ball -> draw paddle last.
    bool pad_moved = (s_prev_pad_x != s_pad_x);
    if (pad_moved) {
        St7789_FillRect(s_prev_pad_x, s_pad_y, s_pad_w, s_pad_h, c_bg());
    }

    draw_ball((int)s_prev_ball.x, (int)s_prev_ball.y, s_ball_r, c_bg());
    draw_ball((int)s_ball.x, (int)s_ball.y, s_ball_r, c_ball());

    // Always draw paddle so it is visible in menu/initial state too.
    St7789_FillRect(s_pad_x, s_pad_y, s_pad_w, s_pad_h, c_paddle());
}

static void draw_hud(void)
{
    char l1[64];
    char l2[64];
    const char* st = (s_phase == kPhaseMenu) ? "MENU" : (s_phase == kPhaseRun ? "RUN" : "PAUSE");
    snprintf(l1, sizeof(l1), "Score:%d  Hit:%d  Miss:%d", s_score, s_hits, s_miss);
    snprintf(l2, sizeof(l2), "Speed L%d  State:%s", s_speed_level + 1, st);
    Ui_DrawTextAtBg(12, UI_HEADER_H + 4, l1, c_text(), c_bg());
    Ui_DrawTextAtBg(12, UI_HEADER_H + 24, l2, c_text(), c_bg());

    if (s_phase == kPhaseMenu) {
        Ui_DrawTextAtBg(12, UI_HEADER_H + 44, "DN/UP choose speed, OK start", c_text(), c_bg());
    } else if (s_phase == kPhasePause) {
        Ui_DrawTextAtBg(12, UI_HEADER_H + 44, "Paused. OK resume.", c_warn(), c_bg());
    } else {
        Ui_DrawTextAtBg(12, UI_HEADER_H + 44, "DN->RIGHT, UP->LEFT", c_text(), c_bg());
    }
}

static void reset_ball(void)
{
    float v = speed_value(s_speed_level);
    s_ball.x = (float)(s_left + (s_right - s_left) / 2);
    s_ball.y = (float)(s_top + 30);
    s_ball.vx = v * 0.55f;
    s_ball.vy = v;
    s_prev_ball = s_ball;
}

static void reset_game(void)
{
    s_score = 0;
    s_hits = 0;
    s_miss = 0;
    s_rally = 0;
    s_pad_x = s_left + ((s_right - s_left - s_pad_w) / 2);
    s_prev_pad_x = s_pad_x;
    s_pad_y = s_bottom - 14;
    reset_ball();
    s_full_redraw = true;
    s_hud_dirty = true;
}

static void full_draw(void)
{
    Ui_BeginBatch();
    draw_static_field();
    draw_hud();
    draw_playfield_dynamic();
    s_prev_pad_x = s_pad_x;
    s_prev_ball = s_ball;
    Ui_EndBatch();
    s_hud_dirty = false;
}

static void step_game(void)
{
    s_ball.x += s_ball.vx;
    s_ball.y += s_ball.vy;

    // Wall bounce
    if (s_ball.x - s_ball_r <= (float)(s_left + 2)) {
        s_ball.x = (float)(s_left + 2 + s_ball_r);
        s_ball.vx = -s_ball.vx;
    } else if (s_ball.x + s_ball_r >= (float)(s_right - 2)) {
        s_ball.x = (float)(s_right - 2 - s_ball_r);
        s_ball.vx = -s_ball.vx;
    }
    if (s_ball.y - s_ball_r <= (float)(s_top + 2)) {
        s_ball.y = (float)(s_top + 2 + s_ball_r);
        s_ball.vy = -s_ball.vy;
    }

    // Paddle / miss
    if (s_ball.vy > 0.0f && s_ball.y + s_ball_r >= (float)s_pad_y) {
        int px1 = s_pad_x;
        int px2 = s_pad_x + s_pad_w;
        if ((int)s_ball.x >= px1 - 1 && (int)s_ball.x <= px2 + 1) {
            // Nonlinear angle by hit position + moving paddle english + rally speed-up.
            float center = (float)(s_pad_x + s_pad_w / 2);
            float rel = (s_ball.x - center) / ((float)s_pad_w * 0.5f); // -1..1
            if (rel < -1.0f) rel = -1.0f;
            if (rel > 1.0f) rel = 1.0f;

            float abs_rel = fabsf(rel);
            float rel_nl = (0.35f * rel) + (0.65f * rel * abs_rel); // stronger edge angle

            float paddle_english = (float)(s_pad_x - s_prev_pad_x) * 0.10f;
            paddle_english = clampf(paddle_english, -1.0f, 1.0f);

            s_rally++;
            float boost = 1.0f + (float)s_rally * 0.03f;
            if (boost > 1.35f) boost = 1.35f;

            float sp = speed_value(s_speed_level) * boost;
            s_ball.vx = (rel_nl * sp * 0.98f) + paddle_english;
            s_ball.vx = clampf(s_ball.vx, -sp * 0.95f, sp * 0.95f);
            float rem = (sp * sp) - (s_ball.vx * s_ball.vx);
            if (rem < 0.0f) rem = 0.0f;
            float vy = sqrtf(rem);
            if (vy < 1.2f) vy = 1.2f;
            s_ball.vy = -vy;
            s_ball.y = (float)(s_pad_y - s_ball_r - 1);
            s_score += 2;
            s_hits++;
            Sfx_PlayKey();
            s_hud_dirty = true;
        } else if (s_ball.y + s_ball_r >= (float)(s_bottom - 2)) {
            s_score -= 1;
            s_miss++;
            s_rally = 0;
            // Ensure the missed ball is erased before respawn.
            draw_ball((int)s_ball.x, (int)s_ball.y, s_ball_r, c_bg());
            reset_ball();
            s_hud_dirty = true;
        }
    }

    Ui_BeginBatch();
    draw_playfield_dynamic();
    if (s_hud_dirty) {
        draw_hud();
        s_hud_dirty = false;
    }
    s_prev_pad_x = s_pad_x;
    s_prev_ball = s_ball;
    Ui_EndBatch();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("BALL", "OK:START  BACK");
    Ui_Println("Closed area + moving paddle.");
    Ui_Println("Catch ball to bounce (+2).");
    Ui_Println("Miss ball: score -1.");
    Ui_Println("Speed level: 1/2/3 in menu.");
    Ui_Println("LEFT/RIGHT move paddle in run.");
    Ui_Println("OK start/pause.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    s_phase = kPhaseMenu;
    s_speed_level = 1;
    reset_game();
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
    s_phase = kPhaseMenu;
    s_full_redraw = true;
    s_next_tick_ms = 0;
    full_draw();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (s_phase == kPhaseMenu) {
        if (key == kInputUp) {
            if (s_speed_level > 0) s_speed_level--;
            s_full_redraw = true;
            s_hud_dirty = true;
        } else if (key == kInputDown) {
            if (s_speed_level < 2) s_speed_level++;
            s_full_redraw = true;
            s_hud_dirty = true;
        } else if (key == kInputEnter) {
            reset_game();
            s_phase = kPhaseRun;
            s_full_redraw = true;
            s_hud_dirty = true;
        }
        return;
    }

    if (key == kInputEnter) {
        s_phase = (s_phase == kPhaseRun) ? kPhasePause : kPhaseRun;
        s_full_redraw = true;
        s_hud_dirty = true;
        return;
    }

    if (s_phase != kPhaseRun) return;

    if (key == kInputLeft) {
        s_pad_x -= 12;
        if (s_pad_x < s_left + 2) s_pad_x = s_left + 2;
    } else if (key == kInputRight) {
        s_pad_x += 12;
        int maxx = s_right - 2 - s_pad_w;
        if (s_pad_x > maxx) s_pad_x = maxx;
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    uint32_t t = now_ms();
    if (t < s_next_tick_ms) return;
    s_next_tick_ms = t + 16;

    if (s_full_redraw) {
        full_draw();
        s_full_redraw = false;
        return;
    }

    if (s_phase == kPhaseRun) {
        step_game();
    }
}

const Experiment g_exp_ball = {
    .id = 15,
    .title = "BALL GAME",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
