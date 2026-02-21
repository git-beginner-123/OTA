#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include "esp_timer.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char* TAG = "EXP_ROCKET";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26

typedef enum {
    kRocketPhaseLaunch = 0,
    kRocketPhaseSeparation,
    kRocketPhaseDeploySat,
    kRocketPhaseBoosterReturn,
    kRocketPhaseDone
} RocketPhase;

static RocketPhase s_phase = kRocketPhaseLaunch;
static uint32_t s_phase_start_ms = 0;
static uint32_t s_next_tick_ms = 0;
static bool s_running = false;
static int s_star_shift = 0;
static bool s_static_drawn = false;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t c_bg(void) { return Ui_ColorRGB(5, 8, 18); }
static uint16_t c_ground(void) { return Ui_ColorRGB(28, 46, 38); }
static uint16_t c_rocket(void) { return Ui_ColorRGB(220, 220, 230); }
static uint16_t c_accent(void) { return Ui_ColorRGB(120, 180, 255); }
static uint16_t c_flame(void) { return Ui_ColorRGB(255, 170, 70); }
static uint16_t c_smoke(void) { return Ui_ColorRGB(120, 120, 130); }
static uint16_t c_text(void) { return Ui_ColorRGB(240, 240, 240); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 120, 120); }

static void draw_stars(void)
{
    static const int star_xy[][2] = {
        {18, 44}, {36, 70}, {62, 56}, {84, 92}, {112, 60}, {140, 78},
        {168, 50}, {196, 84}, {222, 66}, {30, 116}, {72, 130}, {190, 120}
    };

    for (int i = 0; i < (int)(sizeof(star_xy) / sizeof(star_xy[0])); i++) {
        int x = star_xy[i][0];
        int y = star_xy[i][1] + (s_star_shift & 1);
        St7789_FillRect(x, y, 2, 2, c_text());
    }
}

static void draw_rocket_body(int x, int y, int h, uint16_t col)
{
    int w = 14;
    St7789_FillRect(x, y, w, h, col);
    St7789_FillRect(x + 4, y - 6, 6, 6, col);
    St7789_FillRect(x - 4, y + h - 8, 4, 8, col);
    St7789_FillRect(x + w, y + h - 8, 4, 8, col);
}

static void draw_flame(int x, int y, int level)
{
    int h = 10 + (level % 6);
    St7789_FillRect(x + 4, y, 6, h, c_flame());
}

static void draw_smoke(int x, int y, int spread)
{
    for (int i = 0; i < 6; i++) {
        int sx = x + (i * 8) - spread;
        int sy = y + ((i & 1) ? 2 : 0);
        St7789_FillRect(sx, sy, 10, 6, c_smoke());
    }
}

static const char* phase_text(RocketPhase p)
{
    if (p == kRocketPhaseLaunch) return "PHASE: LAUNCH";
    if (p == kRocketPhaseSeparation) return "PHASE: SEPARATE";
    if (p == kRocketPhaseDeploySat) return "PHASE: DEPLOY SAT";
    if (p == kRocketPhaseBoosterReturn) return "PHASE: BOOSTER RTN";
    return "MISSION COMPLETE";
}

static void draw_scene(uint32_t phase_ms)
{
    int w = St7789_Width();
    int h = St7789_Height();
    int body_h = h - UI_HEADER_H - UI_FOOTER_H;

    if (!s_static_drawn) {
        St7789_FillRect(0, UI_HEADER_H, w, body_h, c_bg());
        // Ground strip for launch/landing context.
        St7789_FillRect(0, h - UI_FOOTER_H - 18, w, 18, c_ground());
        s_static_drawn = true;
    } else {
        // Clear only dynamic animation area to reduce per-frame load.
        St7789_FillRect(0, UI_HEADER_H + 34, w, body_h - 34, c_bg());
        St7789_FillRect(0, h - UI_FOOTER_H - 18, w, 18, c_ground());
    }
    draw_stars();

    Ui_DrawTextAtBg(8, UI_HEADER_H + 4, phase_text(s_phase), c_text(), c_bg());

    if (s_phase == kRocketPhaseLaunch) {
        int y = (h - UI_FOOTER_H - 28) - (int)(phase_ms / 45U);
        if (y < 86) y = 86;
        draw_rocket_body(110, y, 30, c_rocket());
        draw_flame(110, y + 30, (int)(phase_ms / 40U));
        draw_smoke(92, h - UI_FOOTER_H - 12, (int)(phase_ms / 90U));
    } else if (s_phase == kRocketPhaseSeparation) {
        int t = (int)(phase_ms / 55U);
        int upper_y = 90 - t;
        int lower_y = 124 + t;
        if (upper_y < 64) upper_y = 64;
        if (lower_y > 170) lower_y = 170;

        draw_rocket_body(112, upper_y, 18, c_rocket());     // second stage
        draw_rocket_body(104, lower_y, 20, Ui_ColorRGB(170, 170, 180)); // booster
        draw_flame(112, upper_y + 18, t);
    } else if (s_phase == kRocketPhaseDeploySat) {
        draw_rocket_body(112, 74, 18, c_rocket());
        int sat_x = 130 + (int)(phase_ms / 35U);
        if (sat_x > 196) sat_x = 196;
        St7789_FillRect(sat_x, 84, 8, 6, c_accent());
        St7789_FillRect(sat_x - 6, 83, 5, 2, c_text());
        St7789_FillRect(sat_x + 9, 83, 5, 2, c_text());
        Ui_DrawTextAtBg(8, UI_HEADER_H + 22, "SATELLITE DEPLOYED", c_accent(), c_bg());
    } else if (s_phase == kRocketPhaseBoosterReturn) {
        int by = 82 + (int)(phase_ms / 35U);
        if (by > 176) by = 176;
        draw_rocket_body(58, by, 20, Ui_ColorRGB(180, 180, 190));
        draw_flame(58, by + 20, (int)(phase_ms / 50U));
        Ui_DrawTextAtBg(8, UI_HEADER_H + 22, "BOOSTER LANDING", c_warn(), c_bg());
    } else {
        Ui_DrawTextAtBg(8, UI_HEADER_H + 22, "SEQUENCE COMPLETE", c_accent(), c_bg());
        St7789_FillRect(96, 108, 48, 22, Ui_ColorRGB(24, 70, 46));
        Ui_DrawTextAtBg(103, 112, "OK", c_text(), Ui_ColorRGB(24, 70, 46));
    }
}

static void switch_phase(RocketPhase next)
{
    s_phase = next;
    s_phase_start_ms = now_ms();
    s_static_drawn = false;
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("ROCKET", "OK:START  BACK");
    Ui_Println("Space launch sequence demo.");
    Ui_Println("1) Launch");
    Ui_Println("2) Stage separation");
    Ui_Println("3) Satellite deploy");
    Ui_Println("4) Booster return");
    Ui_Println("UP/DN: manual phase switch");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    s_running = false;
    s_phase = kRocketPhaseLaunch;
    s_phase_start_ms = now_ms();
    s_next_tick_ms = 0;
    s_star_shift = 0;
    s_static_drawn = false;
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
    Ui_DrawFrame("ROCKET", "UP/DN:PHASE  OK:REPLAY  BACK");
    s_running = true;
    switch_phase(kRocketPhaseLaunch);
    draw_scene(0);
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    s_running = false;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    if (key == kInputEnter) {
        switch_phase(kRocketPhaseLaunch);
        return;
    }
    if (key == kInputUp) {
        int p = (int)s_phase - 1;
        if (p < 0) p = (int)kRocketPhaseDone;
        switch_phase((RocketPhase)p);
        return;
    }
    if (key == kInputDown) {
        int p = (int)s_phase + 1;
        if (p > (int)kRocketPhaseDone) p = 0;
        switch_phase((RocketPhase)p);
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running) return;

    uint32_t t = now_ms();
    if (t < s_next_tick_ms) return;
    s_next_tick_ms = t + 80;
    s_star_shift++;

    uint32_t phase_ms = t - s_phase_start_ms;
    if (s_phase == kRocketPhaseLaunch && phase_ms > 3600U) switch_phase(kRocketPhaseSeparation);
    else if (s_phase == kRocketPhaseSeparation && phase_ms > 2600U) switch_phase(kRocketPhaseDeploySat);
    else if (s_phase == kRocketPhaseDeploySat && phase_ms > 2600U) switch_phase(kRocketPhaseBoosterReturn);
    else if (s_phase == kRocketPhaseBoosterReturn && phase_ms > 3200U) switch_phase(kRocketPhaseDone);

    phase_ms = t - s_phase_start_ms;

    Ui_LcdLock();
    Ui_BeginBatch();
    draw_scene(phase_ms);
    Ui_EndBatch();
    Ui_LcdUnlock();
}

const Experiment g_exp_rocket = {
    .id = 18,
    .title = "ROCKET",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
