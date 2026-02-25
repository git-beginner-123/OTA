#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/sfx.h"

#include <stdio.h>

static int s_sel = 0;

static void draw_page(void)
{
    Ui_DrawFrame("GO TSUMEGO", "UP/DN:SEL  OK:ENTER  BACK:RET");
    Ui_DrawBodyTextRowColor(0, "Life-and-death training", Ui_ColorRGB(220, 230, 240));
    Ui_DrawBodyTextRowColor(2, (s_sel == 0) ? "> Problem 1" : "  Problem 1", Ui_ColorRGB(160, 240, 160));
    Ui_DrawBodyTextRowColor(3, (s_sel == 1) ? "> Problem 2" : "  Problem 2", Ui_ColorRGB(160, 240, 160));
    Ui_DrawBodyTextRowColor(5, "Press ENTER to open", Ui_ColorRGB(170, 210, 255));
}

static void show_problem(void)
{
    Ui_DrawFrame("TSUMEGO P1", "BACK:RET");
    Ui_DrawBodyTextRowColor(0, "Demo problem (placeholder)", Ui_ColorRGB(255, 170, 120));
    Ui_DrawBodyTextRowColor(2, "Target: Black to live", Ui_ColorRGB(225, 230, 235));
    Ui_DrawBodyTextRowColor(3, "Real problem set can be", Ui_ColorRGB(225, 230, 235));
    Ui_DrawBodyTextRowColor(4, "loaded in next revision.", Ui_ColorRGB(225, 230, 235));
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("GO TSUMEGO", "OK:START BACK:RET");
    Ui_Println("Life and death puzzle mode");
    Ui_Println("Current build: demo page");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    s_sel = 0;
    draw_page();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (key == kInputWhiteUp) key = kInputUp;
    if (key == kInputWhiteDown) key = kInputDown;
    if (key == kInputWhiteEnter) key = kInputEnter;

    if (key == kInputUp || key == kInputDown) {
        s_sel ^= 1;
        draw_page();
        return;
    }
    if (key == kInputEnter) {
        Sfx_PlayNotify();
        show_problem();
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
}

const Experiment g_exp_game_go13_tsumego = {
    .id = 108,
    .title = "GO TSUMEGO",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
