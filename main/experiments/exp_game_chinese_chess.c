#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

#define COLS 9
#define ROWS 10
#define CELL 20

static char s_board[COLS * ROWS];
static int s_cur_x = 4;
static int s_cur_y = 9;
static int s_sel_x = -1;
static int s_sel_y = -1;
static bool s_red_turn = true;

static inline int idx(int x, int y) { return y * COLS + x; }
static int board_x0(void) { return (St7789_Width() - (COLS - 1) * CELL) / 2; }
static int board_y0(void) { return 52; }

static uint16_t c_bg(void) { return Ui_ColorRGB(30, 20, 18); }
static uint16_t c_grid(void) { return Ui_ColorRGB(214, 184, 132); }
static uint16_t c_red(void) { return Ui_ColorRGB(255, 120, 110); }
static uint16_t c_blk(void) { return Ui_ColorRGB(230, 230, 230); }
static uint16_t c_cursor(void) { return Ui_ColorRGB(88, 220, 252); }
static uint16_t c_sel(void) { return Ui_ColorRGB(255, 205, 80); }

static void reset_board(void)
{
    memset(s_board, 0, sizeof(s_board));

    const char* top = "rnbakabnr";
    const char* bot = "RNBAKABNR";

    for (int x = 0; x < COLS; x++) {
        s_board[idx(x, 0)] = top[x];
        s_board[idx(x, 9)] = bot[x];
    }

    s_board[idx(1, 2)] = 'c';
    s_board[idx(7, 2)] = 'c';
    s_board[idx(1, 7)] = 'C';
    s_board[idx(7, 7)] = 'C';

    for (int x = 0; x < COLS; x += 2) {
        s_board[idx(x, 3)] = 'p';
        s_board[idx(x, 6)] = 'P';
    }

    s_cur_x = 4;
    s_cur_y = 9;
    s_sel_x = -1;
    s_sel_y = -1;
    s_red_turn = true;
}

static bool belongs_to_turn(char p)
{
    if (!p) return false;
    return s_red_turn ? isupper((unsigned char)p) : islower((unsigned char)p);
}

static void draw_board(void)
{
    int x0 = board_x0();
    int y0 = board_y0();

    St7789_FillRect(0, 30, St7789_Width(), St7789_Height() - 30, c_bg());

    for (int x = 0; x < COLS; x++) {
        int px = x0 + x * CELL;
        St7789_FillRect(px, y0, 1, (ROWS - 1) * CELL + 1, c_grid());
    }
    for (int y = 0; y < ROWS; y++) {
        int py = y0 + y * CELL;
        St7789_FillRect(x0, py, (COLS - 1) * CELL + 1, 1, c_grid());
    }

    Ui_DrawTextAtBg(x0 + 50, y0 + 82, "RIVER", Ui_ColorRGB(150, 210, 255), c_bg());

    for (int y = 0; y < ROWS; y++) {
        for (int x = 0; x < COLS; x++) {
            char p = s_board[idx(x, y)];
            if (!p) continue;
            char t[2] = { (char)toupper((unsigned char)p), 0 };
            uint16_t fg = isupper((unsigned char)p) ? c_red() : c_blk();
            Ui_DrawTextAtBg(x0 + x * CELL - 4, y0 + y * CELL - 7, t, fg, c_bg());
        }
    }

    int cx = x0 + s_cur_x * CELL;
    int cy = y0 + s_cur_y * CELL;
    St7789_FillRect(cx - 7, cy - 7, 15, 1, c_cursor());
    St7789_FillRect(cx - 7, cy + 7, 15, 1, c_cursor());
    St7789_FillRect(cx - 7, cy - 7, 1, 15, c_cursor());
    St7789_FillRect(cx + 7, cy - 7, 1, 15, c_cursor());

    if (s_sel_x >= 0 && s_sel_y >= 0) {
        int sx = x0 + s_sel_x * CELL;
        int sy = y0 + s_sel_y * CELL;
        St7789_FillRect(sx - 5, sy - 5, 11, 1, c_sel());
        St7789_FillRect(sx - 5, sy + 5, 11, 1, c_sel());
        St7789_FillRect(sx - 5, sy - 5, 1, 11, c_sel());
        St7789_FillRect(sx + 5, sy - 5, 1, 11, c_sel());
    }

    Ui_DrawTextAtBg(8, 34, s_red_turn ? "Turn: RED" : "Turn: BLACK", Ui_ColorRGB(230, 235, 240), c_bg());
    St7789_Flush();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("XQ CHINESE", "OK:START BACK:RET");
    Ui_Println("Chinese chess (Xiangqi)");
    Ui_Println("UP/DN/LF/RT: Move cursor");
    Ui_Println("ENTER: Select / Move piece");
    Ui_Println("Rule check: basic sandbox mode");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("XQ CHINESE", "MOVE+OK BACK=RET");
    reset_board();
    draw_board();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;

    if (key == kInputUp && s_cur_y > 0) s_cur_y--;
    if (key == kInputDown && s_cur_y < ROWS - 1) s_cur_y++;
    if (key == kInputLeft && s_cur_x > 0) s_cur_x--;
    if (key == kInputRight && s_cur_x < COLS - 1) s_cur_x++;

    if (key == kInputEnter) {
        if (s_sel_x < 0) {
            char p = s_board[idx(s_cur_x, s_cur_y)];
            if (belongs_to_turn(p)) {
                s_sel_x = s_cur_x;
                s_sel_y = s_cur_y;
            }
        } else {
            int from = idx(s_sel_x, s_sel_y);
            int to = idx(s_cur_x, s_cur_y);
            char dst = s_board[to];
            if (!dst || (isupper((unsigned char)dst) != isupper((unsigned char)s_board[from]))) {
                s_board[to] = s_board[from];
                s_board[from] = 0;
                s_red_turn = !s_red_turn;
            }
            s_sel_x = -1;
            s_sel_y = -1;
        }
    }

    draw_board();
}

const Experiment g_exp_game_chinese_chess = {
    .id = 105,
    .title = "CHINESE CHESS",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = 0,
};
