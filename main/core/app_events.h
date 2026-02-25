#pragma once
#include <stdint.h>
#include <stdbool.h>

typedef enum {
    kInputNone = 0,
    kInputUp,
    kInputDown,
    kInputLeft,
    kInputRight,
    kInputEnter,
    kInputBack,
    // White-side keys for versus mode
    kInputWhiteUp,
    kInputWhiteDown,
    kInputWhiteLeft,
    kInputWhiteRight,
    kInputWhiteEnter,
    kInputWhiteBack
} InputKey;

typedef struct {
    InputKey key;
} AppEvent;

bool AppEvents_Poll(AppEvent* out_event, uint32_t timeout_ms);
