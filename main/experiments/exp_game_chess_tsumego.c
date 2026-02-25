#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/sfx.h"

static int s_sel = 0;

static void draw_page(void)
{
    Ui_DrawFrame("CHESS PUZZLE", "UP/DN:SEL OK:OPEN BACK:RET");
    Ui_DrawBodyTextRowColor(1, (s_sel == 0) ? "> Puzzle 1" : "  Puzzle 1", Ui_ColorRGB(160, 240, 160));
    Ui_DrawBodyTextRowColor(2, (s_sel == 1) ? "> Puzzle 2" : "  Puzzle 2", Ui_ColorRGB(160, 240, 160));
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("CHESS PUZZLE", "OK:START BACK:RET");
    Ui_Println("Mate puzzle mode");
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
        Ui_DrawFrame("CHESS PUZZLE", "BACK:RET");
        Ui_DrawBodyTextRowColor(1, "Puzzle board placeholder", Ui_ColorRGB(255, 170, 120));
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
}

const Experiment g_exp_game_chess_tsumego = {
    .id = 110,
    .title = "CHESS PUZZLE",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
