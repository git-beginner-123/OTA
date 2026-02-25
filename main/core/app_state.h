#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    kPageMainMenu = 0,
    kPageExperimentMenu,
    kPageExperimentRun,
} AppPage;

typedef struct {
    AppPage page;
    int main_index;
    int selected_exp_id;
    int desc_scroll;
} AppState;

void AppState_Init(AppState* s);
