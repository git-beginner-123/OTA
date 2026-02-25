if(NOT DEFINED APP_VARIANT)
    set(APP_VARIANT "ALL")
endif()
string(TOUPPER "${APP_VARIANT}" APP_VARIANT_UPPER)
set(UI_BG_BMP_PATH "${CMAKE_CURRENT_LIST_DIR}/ui/bg_main_240x320.bmp")
set(APP_EMBED_FILES "core/hola_es.pcm")
set(UI_BG_BMP_EMBEDDED 0)
if(EXISTS "${UI_BG_BMP_PATH}")
    list(APPEND APP_EMBED_FILES "ui/bg_main_240x320.bmp")
    set(UI_BG_BMP_EMBEDDED 1)
endif()

set(APP_SRCS
    "app_main.c"
    "core/app.c"
    "core/app_state.c"
    "core/app_events.c"
    "core/app_settings.c"
    "audio/sfx.c"
    "audio/audio_engine.c"
    "audio/audio_playback.c"
    "drivers/audio_out.c"
    "drivers/led_guard.c"
    "ui/ui_lcd.c"
    "ui/ui_console.c"
    "input/input_uart_frame.c"
    "display/st7789.c"
    "display/font5x7.c"
    "display/font8x16.c"
    "experiments/experiments_registry.c"
    "experiments/exp_wifi_ota.c"
    "input/uart1_router.c"
    "input/drv_input_gpio_keys.c"
)

if(APP_VARIANT_UPPER STREQUAL "GO" OR APP_VARIANT_UPPER STREQUAL "ALL")
    list(APPEND APP_SRCS
        "experiments/exp_game_go13.c"
        "experiments/exp_game_go13_replay.c"
        "experiments/exp_game_go13_tsumego.c"
        "experiments/go_record_store.c"
    )
endif()

if(APP_VARIANT_UPPER STREQUAL "CHESS" OR APP_VARIANT_UPPER STREQUAL "ALL")
    list(APPEND APP_SRCS
        "experiments/exp_game_chess.c"
        "experiments/exp_game_chess_replay.c"
        "experiments/exp_game_chess_tsumego.c"
    )
endif()

if(APP_VARIANT_UPPER STREQUAL "DICE" OR APP_VARIANT_UPPER STREQUAL "ALL")
    list(APPEND APP_SRCS "experiments/exp_game_dice_chess.c")
endif()

if(APP_VARIANT_UPPER STREQUAL "GOMOKU" OR APP_VARIANT_UPPER STREQUAL "ALL")
    list(APPEND APP_SRCS "experiments/exp_game_gomoku.c")
endif()

if(APP_VARIANT_UPPER STREQUAL "ALL")
    list(APPEND APP_SRCS "experiments/exp_game_chinese_chess.c")
endif()

idf_component_register(
    SRCS ${APP_SRCS}

    INCLUDE_DIRS
        "."
        "core"
        "ui"
        "input"
        "display"
        "experiments"
        "audio"
        "drivers"

    EMBED_FILES
        ${APP_EMBED_FILES}

    REQUIRES
        esp_driver_uart
        esp_driver_spi
        esp_driver_gpio
        esp_driver_ledc
        esp_driver_i2s
        esp_adc
        driver
        freertos
        esp_system
        esp_common
        app_update
        log
        esp_http_server
        esp_http_client
        esp_netif
        esp_event
        esp_wifi
        wifi_provisioning
        qrcode
        nvs_flash
        mbedtls
        comm_wifi
        json
)

target_compile_definitions(${COMPONENT_LIB} PUBLIC APP_VARIANT_${APP_VARIANT_UPPER}=1 UI_BG_BMP_EMBEDDED=${UI_BG_BMP_EMBEDDED})
