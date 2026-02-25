#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <stdint.h>

#define N 15
#define CELL 14
#define BOARD_W ((N - 1) * CELL)
#define BOARD_H ((N - 1) * CELL)

typedef enum {
    kStoneNone = 0,
    kStoneBlack = 1,
    kStoneWhite = 2,
} Stone;

static Stone s_board[N * N];
static int s_cursor_x = 7;
static int s_cursor_y = 7;
static Stone s_turn = kStoneBlack;
static Stone s_winner = kStoneNone;
static int s_moves = 0;

static int board_x0(void) { return (St7789_Width() - BOARD_W) / 2; }
static int board_y0(void) { return 56; }
static inline int idx(int x, int y) { return y * N + x; }

static uint16_t c_bg(void) { return Ui_ColorRGB(24, 34, 18); }
static uint16_t c_grid(void) { return Ui_ColorRGB(210, 190, 130); }
static uint16_t c_black(void) { return Ui_ColorRGB(30, 30, 30); }
static uint16_t c_white(void) { return Ui_ColorRGB(236, 236, 236); }
static uint16_t c_cursor(void) { return Ui_ColorRGB(90, 220, 255); }
static uint16_t c_text(void) { return Ui_ColorRGB(220, 230, 235); }
static uint16_t c_win(void) { return Ui_ColorRGB(130, 255, 130); }

static bool inside(int x, int y)
{
    return x >= 0 && x < N && y >= 0 && y < N;
}

static bool has_five(int x, int y, Stone who)
{
    const int dirs[4][2] = {{1, 0}, {0, 1}, {1, 1}, {1, -1}};
    for (int d = 0; d < 4; d++) {
        int dx = dirs[d][0];
        int dy = dirs[d][1];
        int count = 1;

        for (int s = 1; s < 5; s++) {
            int nx = x + dx * s;
            int ny = y + dy * s;
            if (!inside(nx, ny) || s_board[idx(nx, ny)] != who) break;
            count++;
        }
        for (int s = 1; s < 5; s++) {
            int nx = x - dx * s;
            int ny = y - dy * s;
            if (!inside(nx, ny) || s_board[idx(nx, ny)] != who) break;
            count++;
        }
        if (count >= 5) return true;
    }
    return false;
}

static void draw_board(void)
{
    int x0 = board_x0();
    int y0 = board_y0();

    St7789_FillRect(0, 30, St7789_Width(), St7789_Height() - 30, c_bg());

    for (int i = 0; i < N; i++) {
        int x = x0 + i * CELL;
        int y = y0 + i * CELL;
        St7789_FillRect(x, y0, 1, BOARD_H + 1, c_grid());
        St7789_FillRect(x0, y, BOARD_W + 1, 1, c_grid());
    }

    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            Stone s = s_board[idx(x, y)];
            if (s == kStoneNone) continue;
            int px = x0 + x * CELL - 4;
            int py = y0 + y * CELL - 4;
            St7789_FillRect(px, py, 9, 9, (s == kStoneBlack) ? c_black() : c_white());
        }
    }

    int cx = x0 + s_cursor_x * CELL;
    int cy = y0 + s_cursor_y * CELL;
    St7789_FillRect(cx - 6, cy - 6, 13, 1, c_cursor());
    St7789_FillRect(cx - 6, cy + 6, 13, 1, c_cursor());
    St7789_FillRect(cx - 6, cy - 6, 1, 13, c_cursor());
    St7789_FillRect(cx + 6, cy - 6, 1, 13, c_cursor());

    char line[48];
    if (s_winner != kStoneNone) {
        snprintf(line, sizeof(line), "Winner: %s (ENTER=restart)", s_winner == kStoneBlack ? "Black" : "White");
        Ui_DrawTextAtBg(8, 34, line, c_win(), c_bg());
    } else {
        snprintf(line, sizeof(line), "Turn: %s", s_turn == kStoneBlack ? "Black" : "White");
        Ui_DrawTextAtBg(8, 34, line, c_text(), c_bg());
    }

    St7789_Flush();
}

static void reset_game(void)
{
    memset(s_board, 0, sizeof(s_board));
    s_cursor_x = 7;
    s_cursor_y = 7;
    s_turn = kStoneBlack;
    s_winner = kStoneNone;
    s_moves = 0;
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("GOMOKU", "OK:START BACK:RET");
    Ui_Println("15x15 board");
    Ui_Println("UP/DN/LF/RT: Move");
    Ui_Println("ENTER: Place stone");
    Ui_Println("BACK: Return menu");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("GOMOKU", "MOVE+OK BACK=RET");
    reset_game();
    draw_board();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;

    if (key == kInputUp && s_cursor_y > 0) s_cursor_y--;
    if (key == kInputDown && s_cursor_y < N - 1) s_cursor_y++;
    if (key == kInputLeft && s_cursor_x > 0) s_cursor_x--;
    if (key == kInputRight && s_cursor_x < N - 1) s_cursor_x++;

    if (key == kInputEnter) {
        if (s_winner != kStoneNone) {
            reset_game();
        } else if (s_board[idx(s_cursor_x, s_cursor_y)] == kStoneNone) {
            s_board[idx(s_cursor_x, s_cursor_y)] = s_turn;
            s_moves++;
            if (has_five(s_cursor_x, s_cursor_y, s_turn)) {
                s_winner = s_turn;
            } else {
                s_turn = (s_turn == kStoneBlack) ? kStoneWhite : kStoneBlack;
            }
        }
    }

    draw_board();
}

const Experiment g_exp_game_gomoku = {
    .id = 103,
    .title = "GOMOKU",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = 0,
};
