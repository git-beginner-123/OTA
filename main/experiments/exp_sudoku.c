#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include "esp_log.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char* TAG = "EXP_SUDOKU";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26

#define N 9
#define CELL 22

typedef struct {
    uint8_t a; // coprime with 9
    uint8_t b;
} DigitMapParam;

// 0 = empty
static const uint8_t kBasePuzzle[N * N] = {
    5,3,0, 0,7,0, 0,0,0,
    6,0,0, 1,9,5, 0,0,0,
    0,9,8, 0,0,0, 0,6,0,

    8,0,0, 0,6,0, 0,0,3,
    4,0,0, 8,0,3, 0,0,1,
    7,0,0, 0,2,0, 0,0,6,

    0,6,0, 0,0,0, 2,8,0,
    0,0,0, 4,1,9, 0,0,5,
    0,0,0, 0,8,0, 0,7,9,
};

static const uint8_t kBaseSolution[N * N] = {
    5,3,4, 6,7,8, 9,1,2,
    6,7,2, 1,9,5, 3,4,8,
    1,9,8, 3,4,2, 5,6,7,

    8,5,9, 7,6,1, 4,2,3,
    4,2,6, 8,5,3, 7,9,1,
    7,1,3, 9,2,4, 8,5,6,

    9,6,1, 5,3,7, 2,8,4,
    2,8,7, 4,1,9, 6,3,5,
    3,4,5, 2,8,6, 1,7,9,
};

// 12 valid digit permutations => 12 correct puzzles/solutions
static const DigitMapParam kMaps[12] = {
    {1,0}, {1,1}, {1,2}, {1,3},
    {2,0}, {2,1}, {2,2}, {2,3},
    {4,0}, {4,1}, {5,0}, {8,2},
};

typedef enum {
    kModeMove = 0,
    kModePick,
} SudokuMode;

static bool s_running = false;
static SudokuMode s_mode = kModeMove;
static uint8_t s_board[N * N];
static uint8_t s_solution[N * N];
static bool s_fixed[N * N];
static int s_cursor = 0;
static uint8_t s_pick_value = 1;
static bool s_solved = false;

static uint8_t s_puzzle_idx = 0;
static bool s_hint_armed = false;
static uint32_t s_hint_deadline_ms = 0;
static char s_hint_line[32] = {0};
static uint32_t s_hint_until_ms = 0;

static uint16_t c_bg(void) { return Ui_ColorRGB(10, 16, 26); }
static uint16_t c_cell_a(void) { return Ui_ColorRGB(14, 24, 42); }
static uint16_t c_cell_b(void) { return Ui_ColorRGB(26, 44, 26); }
static uint16_t c_muted(void) { return Ui_ColorRGB(145, 168, 190); }
static uint16_t c_grid(void) { return Ui_ColorRGB(120, 150, 185); }
static uint16_t c_grid_major(void) { return Ui_ColorRGB(210, 225, 245); }
static uint16_t c_text(void) { return Ui_ColorRGB(235, 235, 235); }
static uint16_t c_fixed(void) { return Ui_ColorRGB(120, 210, 255); }
static uint16_t c_focus(void) { return Ui_ColorRGB(68, 92, 128); }
static uint16_t c_pick(void) { return Ui_ColorRGB(30, 48, 74); }
static uint16_t c_pick_focus(void) { return Ui_ColorRGB(246, 205, 86); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 130, 130); }
static uint16_t c_ok(void) { return Ui_ColorRGB(120, 230, 130); }

static int board_x0(void) { return (St7789_Width() - N * CELL) / 2; }
static int board_y0(void) { return UI_HEADER_H + 32; }
static uint32_t now_ms(void) { return (uint32_t)(esp_timer_get_time() / 1000ULL); }

static int idx_row(int i) { return i / N; }
static int idx_col(int i) { return i % N; }

static uint8_t map_digit(uint8_t d, uint8_t map_idx)
{
    if (d == 0) return 0;
    DigitMapParam p = kMaps[map_idx % 12];
    uint8_t x = (uint8_t)(d - 1);
    return (uint8_t)(((p.a * x + p.b) % 9) + 1);
}

static bool has_conflict(int idx, uint8_t v)
{
    if (v == 0) return false;
    int r = idx_row(idx), c = idx_col(idx);

    for (int cc = 0; cc < N; cc++) {
        int j = r * N + cc;
        if (j != idx && s_board[j] == v) return true;
    }
    for (int rr = 0; rr < N; rr++) {
        int j = rr * N + c;
        if (j != idx && s_board[j] == v) return true;
    }

    int br = (r / 3) * 3;
    int bc = (c / 3) * 3;
    for (int rr = br; rr < br + 3; rr++) {
        for (int cc = bc; cc < bc + 3; cc++) {
            int j = rr * N + cc;
            if (j != idx && s_board[j] == v) return true;
        }
    }
    return false;
}

static void check_solved(void)
{
    for (int i = 0; i < N * N; i++) {
        if (s_board[i] != s_solution[i]) {
            s_solved = false;
            return;
        }
    }
    s_solved = true;
}

static void load_puzzle(uint8_t puzzle_idx)
{
    s_puzzle_idx = (uint8_t)(puzzle_idx % 12);
    for (int i = 0; i < N * N; i++) {
        uint8_t p = map_digit(kBasePuzzle[i], s_puzzle_idx);
        uint8_t s = map_digit(kBaseSolution[i], s_puzzle_idx);
        s_board[i] = p;
        s_solution[i] = s;
        s_fixed[i] = (p != 0);
    }
    s_cursor = 0;
    s_mode = kModeMove;
    s_pick_value = 1;
    s_solved = false;
    s_hint_armed = false;
    s_hint_line[0] = '\0';
    s_hint_until_ms = 0;
}

static void draw_grid_lines(void)
{
    int x0 = board_x0();
    int y0 = board_y0();
    int w = N * CELL;
    int h = N * CELL;

    for (int i = 0; i <= N; i++) {
        int x = x0 + i * CELL;
        int y = y0 + i * CELL;
        uint16_t col = (i % 3 == 0) ? c_grid_major() : c_grid();
        int t = (i % 3 == 0) ? 2 : 1;
        St7789_FillRect(x, y0, t, h + 1, col);
        St7789_FillRect(x0, y, w + 1, t, col);
    }
}

static void draw_cell(int idx)
{
    int r = idx_row(idx), c = idx_col(idx);
    int x = board_x0() + c * CELL + 2;
    int y = board_y0() + r * CELL + 2;
    int w = CELL - 2;
    int h = CELL - 2;

    bool focused = (idx == s_cursor);
    uint16_t base = ((r + c) & 1) ? c_cell_a() : c_cell_b();
    uint16_t bg = focused ? c_focus() : base;
    St7789_FillRect(x, y, w, h, bg);

    uint8_t v = s_board[idx];
    if (v != 0) {
        char t[2] = {(char)('0' + v), '\0'};
        uint16_t fg = s_fixed[idx] ? c_fixed() : c_text();
        if (!s_fixed[idx] && has_conflict(idx, v)) fg = c_warn();
        Ui_DrawTextAtBg(x + 7, y + 3, t, fg, bg);
    }
}

static void draw_board(void)
{
    int x0 = board_x0();
    int y0 = board_y0();
    St7789_FillRect(x0, y0, N * CELL + 3, N * CELL + 3, c_bg());
    for (int i = 0; i < N * N; i++) draw_cell(i);
    draw_grid_lines();
}

static void draw_picker(void)
{
    int y = board_y0() + N * CELL + 6;
    int w = 20;
    int h = 20;
    int gap = 3;
    int total = 9 * w + 8 * gap;
    int x0 = (St7789_Width() - total) / 2;

    St7789_FillRect(0, y - 2, St7789_Width(), h + 4, c_bg());
    for (int i = 1; i <= 9; i++) {
        int x = x0 + (i - 1) * (w + gap);
        bool focused = (s_mode == kModePick && s_pick_value == (uint8_t)i);
        uint16_t bg = focused ? c_pick_focus() : c_pick();
        uint16_t fg = focused ? Ui_ColorRGB(20, 20, 20) : c_text();
        St7789_FillRect(x, y, w, h, bg);
        St7789_FillRect(x, y, w, 1, c_muted());
        St7789_FillRect(x, y + h - 1, w, 1, c_muted());
        St7789_FillRect(x, y, 1, h, c_muted());
        St7789_FillRect(x + w - 1, y, 1, h, c_muted());
        char t[2] = {(char)('0' + i), '\0'};
        Ui_DrawTextAtBg(x + 6, y + 2, t, fg, bg);
    }
}

static void draw_status(void)
{
    int y = UI_HEADER_H + 4;
    St7789_FillRect(0, y, St7789_Width(), 24, c_bg());

    char line[64];
    if (s_hint_line[0] != '\0' && now_ms() < s_hint_until_ms) {
        Ui_DrawTextAtBg(8, y + 4, s_hint_line, c_ok(), c_bg());
    } else if (s_solved) {
        snprintf(line, sizeof(line), "SOLVED! OK:NEXT (%d/12)", (int)(s_puzzle_idx + 1));
        Ui_DrawTextAtBg(8, y + 4, line, c_ok(), c_bg());
    } else if (s_mode == kModePick) {
        snprintf(line, sizeof(line), "Pick %d  UP/DN change  OK set", (int)s_pick_value);
        Ui_DrawTextAtBg(8, y + 4, line, c_text(), c_bg());
    } else if (s_hint_armed && now_ms() < s_hint_deadline_ms) {
        Ui_DrawTextAtBg(8, y + 4, "Hint armed: press OK", c_ok(), c_bg());
    } else {
        int r = idx_row(s_cursor) + 1, c = idx_col(s_cursor) + 1;
        snprintf(line, sizeof(line), "Cell R%dC%d  OK pick", r, c);
        Ui_DrawTextAtBg(8, y + 4, line, c_text(), c_bg());
    }
}

static void draw_full(void)
{
    const char* footer = (s_mode == kModePick)
        ? "UP/DN:NUM OK:SET BACK:CANCEL"
        : "UP/DN:MOVE OK:PICK BACK+OK:HINT";
    Ui_DrawFrame("SUDOKU", footer);
    St7789_FillRect(0, UI_HEADER_H, St7789_Width(), St7789_Height() - UI_HEADER_H - UI_FOOTER_H, c_bg());
    draw_status();
    draw_board();
    draw_picker();
    St7789_Flush();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("SUDOKU", "OK:START  BACK");
    Ui_Println("9x9 Sudoku with 12 puzzles.");
    Ui_Println("OK enters number picker.");
    Ui_Println("BACK then OK = hint.");
    Ui_Println("Hint uses A-I and 1-9.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    load_puzzle(0);
    s_running = false;
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
    s_running = false;
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    s_running = true;
    draw_full();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    s_running = false;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    if (s_solved) {
        if (key == kInputEnter || key == kInputBack) {
            load_puzzle((uint8_t)(s_puzzle_idx + 1));
            draw_full();
        }
        return;
    }

    if (s_mode == kModeMove) {
        if (s_hint_armed && now_ms() > s_hint_deadline_ms) s_hint_armed = false;

        if (key == kInputUp) {
            s_cursor = (s_cursor + N * N - 1) % (N * N);
            draw_status();
            draw_board();
            St7789_Flush();
            return;
        }
        if (key == kInputDown) {
            s_cursor = (s_cursor + 1) % (N * N);
            draw_status();
            draw_board();
            St7789_Flush();
            return;
        }
        if (key == kInputEnter) {
            if (s_hint_armed && now_ms() <= s_hint_deadline_ms) {
                s_hint_armed = false;
                int found = -1;
                for (int k = 0; k < N * N; k++) {
                    int i = (s_cursor + k) % (N * N);
                    if (!s_fixed[i] && s_board[i] != s_solution[i]) { found = i; break; }
                }
                if (found >= 0) {
                    int rr = idx_row(found);
                    int cc = idx_col(found);
                    char row_letter = (char)('A' + rr);
                    uint8_t v = s_solution[found];
                    snprintf(s_hint_line, sizeof(s_hint_line), "%c%d = %d", row_letter, cc + 1, v);
                    s_hint_until_ms = now_ms() + 3000;
                } else {
                    snprintf(s_hint_line, sizeof(s_hint_line), "HINT no missing cell");
                    s_hint_until_ms = now_ms() + 2000;
                }
                draw_status();
                St7789_Flush();
                return;
            }
            if (!s_fixed[s_cursor]) {
                s_mode = kModePick;
                s_pick_value = s_board[s_cursor];
                if (s_pick_value < 1 || s_pick_value > 9) s_pick_value = 1;
                draw_full();
            }
            return;
        }
        if (key == kInputBack) {
            s_hint_armed = true;
            s_hint_deadline_ms = now_ms() + 1200;
            draw_status();
            St7789_Flush();
            return;
        }
        return;
    }

    if (s_mode == kModePick) {
        if (key == kInputBack) {
            s_mode = kModeMove;
            draw_full();
            return;
        }
        if (key == kInputUp) {
            s_pick_value = (uint8_t)((s_pick_value + 7) % 9 + 1);
            draw_status();
            draw_picker();
            St7789_Flush();
            return;
        }
        if (key == kInputDown) {
            s_pick_value = (uint8_t)(s_pick_value % 9 + 1);
            draw_status();
            draw_picker();
            St7789_Flush();
            return;
        }
        if (key == kInputEnter) {
            s_board[s_cursor] = s_pick_value;
            s_mode = kModeMove;
            check_solved();
            draw_full();
            return;
        }
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running) return;
    if (s_hint_line[0] != '\0' && now_ms() > s_hint_until_ms) {
        s_hint_line[0] = '\0';
        draw_status();
        St7789_Flush();
    }
    if (s_hint_armed && now_ms() > s_hint_deadline_ms && s_mode == kModeMove) {
        s_hint_armed = false;
        draw_status();
        St7789_Flush();
    }
}

const Experiment g_exp_sudoku = {
    .id = 24,
    .title = "SUDOKU",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
