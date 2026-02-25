#include "experiments/experiments_registry.h"

extern const Experiment g_exp_game_chess;
extern const Experiment g_exp_game_chess_replay;
extern const Experiment g_exp_game_chess_tsumego;
extern const Experiment g_exp_game_go13;
extern const Experiment g_exp_game_go13_replay;
extern const Experiment g_exp_game_go13_tsumego;
extern const Experiment g_exp_game_gomoku;
extern const Experiment g_exp_game_dice_chess;
extern const Experiment g_exp_game_chinese_chess;
extern const Experiment g_exp_wifi_ota;

static const Experiment* kList[] = {
#if defined(APP_VARIANT_GO)
    &g_exp_game_go13,
    &g_exp_game_go13_replay,
    &g_exp_game_go13_tsumego,
    &g_exp_wifi_ota,
#elif defined(APP_VARIANT_CHESS)
    &g_exp_game_chess,
    &g_exp_game_chess_replay,
    &g_exp_game_chess_tsumego,
    &g_exp_wifi_ota,
#elif defined(APP_VARIANT_DICE)
    &g_exp_game_dice_chess,
    &g_exp_wifi_ota,
#elif defined(APP_VARIANT_GOMOKU)
    &g_exp_game_gomoku,
    &g_exp_wifi_ota,
#else
    &g_exp_game_chess,
    &g_exp_game_go13,
    &g_exp_game_go13_replay,
    &g_exp_game_go13_tsumego,
    &g_exp_game_gomoku,
    &g_exp_game_dice_chess,
    &g_exp_game_chinese_chess,
    &g_exp_wifi_ota,
#endif
};

int Experiments_Count(void)
{
    return (int)(sizeof(kList) / sizeof(kList[0]));
}

const Experiment* Experiments_GetByIndex(int index)
{
    if (index < 0 || index >= Experiments_Count()) return 0;
    return kList[index];
}

const Experiment* Experiments_GetById(int id)
{
    for (int i = 0; i < Experiments_Count(); i++) {
        if (kList[i]->id == id) return kList[i];
    }
    return 0;
}
