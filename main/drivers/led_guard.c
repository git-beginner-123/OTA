#include "drivers/led_guard.h"

#include "driver/gpio.h"

// Shared LED GPIOs used by experiments:
// GPIO/PWM exp: 36,14,13
// Semaforo exp: 17,38,39,2,1,37
// WS2812 data: 48 (force low)
static const gpio_num_t s_led_pins[] = {
    GPIO_NUM_36, GPIO_NUM_14, GPIO_NUM_13,
    GPIO_NUM_17, GPIO_NUM_38, GPIO_NUM_39,
    GPIO_NUM_2,  GPIO_NUM_1,  GPIO_NUM_37,
    GPIO_NUM_48,
};

void LedGuard_AllOff(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < (int)(sizeof(s_led_pins) / sizeof(s_led_pins[0])); i++) {
        mask |= (1ULL << s_led_pins[i]);
    }

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);

    for (int i = 0; i < (int)(sizeof(s_led_pins) / sizeof(s_led_pins[0])); i++) {
        gpio_set_level(s_led_pins[i], 0);
    }
}

void LedGuard_ReleasePins(void)
{
    uint64_t mask = 0;
    for (int i = 0; i < (int)(sizeof(s_led_pins) / sizeof(s_led_pins[0])); i++) {
        mask |= (1ULL << s_led_pins[i]);
    }

    gpio_config_t io = {
        .pin_bit_mask = mask,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}
