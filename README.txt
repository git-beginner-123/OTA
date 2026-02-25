Game Console (ESP32 + ILI9341 + Keys)

Current framework (variant-based):
1) GO variant: GO 13x13 / GO REPLAY / GO TSUMEGO / SETTING
2) CHESS variant: CHESS / CHESS REPLAY / CHESS PUZZLE / SETTING
3) DICE variant: DICE CHESS / SETTING
4) GOMOKU variant: GOMOKU / SETTING

Note:
- Main menu ENTER now starts app directly (skips intro "OK:START" page).
- In SYSTEM OTA page, LEFT/RIGHT switches target type directly.

Build all variant bins + OTA files:
  ./build_variants_ota.sh ALL
Build one target by numeric type:
  ./build_variants_ota.sh <type>
  type: 1=GO, 2=CHESS, 3=DICE, 4=GOMOKU
  ALL means build all targets
Optional artifact version name override:
  ./build_variants_ota.sh ALL v1.2.8

Flash by target type (same selector style):
  ./flash_variant.sh <type|ALL> [PORT] [--no-monitor]
Outputs:
  ota/go/*
  ota/chess/*
  ota/dice/*
  ota/gomoku/*
  ota/targets.txt

Hardware assumptions:
- LCD: ILI9341-compatible panel (driver file: main/display/st7789.c)
- Input: GPIO keys (main/input/drv_input_gpio_keys.c)
- Audio: speaker only (TX path kept)

Removed from this branch:
- Bluetooth/BLE features and component references
- MIC experiment / MIC input path
- Video/MJPEG assets and scripts
- Non-game app experiments

Build note:
- Run with ESP-IDF environment activated.
