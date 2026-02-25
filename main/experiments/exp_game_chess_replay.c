#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/sfx.h"

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("CHESS REPLAY", "OK:START BACK:RET");
    Ui_Println("Replay module");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("CHESS REPLAY", "BACK:RET");
    Ui_DrawBodyTextRowColor(1, "Replay coming soon", Ui_ColorRGB(255, 170, 120));
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    (void)key;
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
}

const Experiment g_exp_game_chess_replay = {
    .id = 109,
    .title = "CHESS REPLAY",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
