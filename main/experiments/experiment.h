#pragma once
#include <stdint.h>
#include "core/app_events.h"

typedef struct ExperimentContext ExperimentContext;

typedef struct {
    int id;
    const char* title;

    void (*on_enter)(ExperimentContext* ctx);
    void (*on_exit)(ExperimentContext* ctx);

    void (*show_requirements)(ExperimentContext* ctx);

    void (*start)(ExperimentContext* ctx);
    void (*stop)(ExperimentContext* ctx);

    void (*on_key)(ExperimentContext* ctx, InputKey key);
    void (*tick)(ExperimentContext* ctx);
} Experiment;

struct ExperimentContext {
    int dummy;
};

const Experiment* ExpWifiRemote_Get(void);
bool ExpGo13_ShouldConsumeBack(void);
bool ExpGo13_ShouldConsumeNonBackExit(InputKey key);
bool ExpGo13_TakeExitRequest(void);
bool ExpGoReplay_ShouldConsumeBack(void);
bool ExpSetting_ShouldConsumeBack(void);
