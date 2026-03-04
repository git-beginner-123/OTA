#include "experiments/experiments_registry.h"

extern const Experiment g_exp_gpio;
extern const Experiment g_exp_pwm;
extern const Experiment g_exp_adc;
extern const Experiment g_exp_history;
extern const Experiment g_exp_tof;
extern const Experiment g_exp_mic;
extern const Experiment g_exp_speaker;
extern const Experiment g_exp_ble;
extern const Experiment g_exp_wifi_ap;
extern const Experiment g_exp_economy;
extern const Experiment g_exp_semaforo;
extern const Experiment g_exp_maze;
extern const Experiment g_exp_seesaw;
extern const Experiment g_exp_math24;
extern const Experiment g_exp_ball;
extern const Experiment g_exp_solar;
extern const Experiment g_exp_rocket;
extern const Experiment g_exp_music;
extern const Experiment g_exp_snake;
extern const Experiment g_exp_numline;
extern const Experiment g_exp_fraction;
extern const Experiment g_exp_wifi_ota;
extern const Experiment g_exp_memory;
extern const Experiment g_exp_sudoku;

static const Experiment* kList[] = {
    &g_exp_gpio,
    &g_exp_semaforo,
    &g_exp_adc,
    &g_exp_maze,
    &g_exp_history,
    &g_exp_seesaw,
    &g_exp_tof,
    &g_exp_math24,
    &g_exp_mic,
    &g_exp_ball,
    &g_exp_speaker,
    &g_exp_solar,
    &g_exp_ble,
    &g_exp_rocket,
    &g_exp_music,
    &g_exp_snake,
    &g_exp_numline,
    &g_exp_fraction,
    &g_exp_memory,
    &g_exp_pwm,
    &g_exp_wifi_ap,
    &g_exp_economy,
    &g_exp_wifi_ota,
    &g_exp_sudoku,
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
