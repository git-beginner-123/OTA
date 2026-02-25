#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>
#include <ctype.h>

#define N 8
#define CELL 24

static char s_board[N * N];
static int s_cur_x = 0;
static int s_cur_y = 0;
static int s_sel_x = -1;
static int s_sel_y = -1;
static bool s_white_turn = true;

static inline int idx(int x, int y) { return y * N + x; }
static int board_x0(void) { return (St7789_Width() - N * CELL) / 2; }
static int board_y0(void) { return 56; }

static uint16_t c_dark(void) { return Ui_ColorRGB(114, 84, 66); }
static uint16_t c_light(void) { return Ui_ColorRGB(226, 207, 180); }
static uint16_t c_cursor(void) { return Ui_ColorRGB(70, 220, 250); }
static uint16_t c_sel(void) { return Ui_ColorRGB(255, 196, 70); }
static uint16_t c_text(void) { return Ui_ColorRGB(230, 235, 240); }

static void reset_board(void)
{
    memset(s_board, 0, sizeof(s_board));

    const char* top = "rnbqkbnr";
    const char* pawns = "pppppppp";
    const char* wpawns = "PPPPPPPP";
    const char* bottom = "RNBQKBNR";

    for (int x = 0; x < N; x++) {
        s_board[idx(x, 0)] = top[x];
        s_board[idx(x, 1)] = pawns[x];
        s_board[idx(x, 6)] = wpawns[x];
        s_board[idx(x, 7)] = bottom[x];
    }

    s_cur_x = 0;
    s_cur_y = 0;
    s_sel_x = -1;
    s_sel_y = -1;
    s_white_turn = true;
}

static void draw_board(void)
{
    int x0 = board_x0();
    int y0 = board_y0();

    St7789_FillRect(0, 30, St7789_Width(), St7789_Height() - 30, Ui_ColorRGB(24, 30, 36));

    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int px = x0 + x * CELL;
            int py = y0 + y * CELL;
            uint16_t cc = ((x + y) & 1) ? c_dark() : c_light();
            St7789_FillRect(px, py, CELL, CELL, cc);

            char p = s_board[idx(x, y)];
            if (p) {
                char t[2] = { (char)toupper((unsigned char)p), 0 };
                uint16_t fg = isupper((unsigned char)p) ? Ui_ColorRGB(20, 20, 20) : Ui_ColorRGB(250, 250, 250);
                Ui_DrawTextAtBg(px + 8, py + 4, t, fg, cc);
            }
        }
    }

    int cx = x0 + s_cur_x * CELL;
    int cy = y0 + s_cur_y * CELL;
    St7789_FillRect(cx, cy, CELL, 2, c_cursor());
    St7789_FillRect(cx, cy + CELL - 2, CELL, 2, c_cursor());
    St7789_FillRect(cx, cy, 2, CELL, c_cursor());
    St7789_FillRect(cx + CELL - 2, cy, 2, CELL, c_cursor());

    if (s_sel_x >= 0 && s_sel_y >= 0) {
        int sx = x0 + s_sel_x * CELL;
        int sy = y0 + s_sel_y * CELL;
        St7789_FillRect(sx + 3, sy + 3, CELL - 6, 2, c_sel());
        St7789_FillRect(sx + 3, sy + CELL - 5, CELL - 6, 2, c_sel());
        St7789_FillRect(sx + 3, sy + 3, 2, CELL - 6, c_sel());
        St7789_FillRect(sx + CELL - 5, sy + 3, 2, CELL - 6, c_sel());
    }

    char line[48];
    snprintf(line, sizeof(line), "Turn: %s", s_white_turn ? "WHITE" : "BLACK");
    Ui_DrawTextAtBg(8, 34, line, c_text(), Ui_ColorRGB(24, 30, 36));

    St7789_Flush();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("CHESS", "OK:START BACK:RET");
    Ui_Println("8x8 international chess");
    Ui_Println("UP/DN/LF/RT: Move cursor");
    Ui_Println("ENTER: Select / Move piece");
    Ui_Println("Rule check: basic sandbox mode");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("CHESS", "MOVE+OK BACK=RET");
    reset_board();
    draw_board();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
}

static bool belongs_to_turn(char p)
{
    if (!p) return false;
    return s_white_turn ? isupper((unsigned char)p) : islower((unsigned char)p);
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;

    if (key == kInputUp && s_cur_y > 0) s_cur_y--;
    if (key == kInputDown && s_cur_y < N - 1) s_cur_y++;
    if (key == kInputLeft && s_cur_x > 0) s_cur_x--;
    if (key == kInputRight && s_cur_x < N - 1) s_cur_x++;

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
                s_white_turn = !s_white_turn;
            }
            s_sel_x = -1;
            s_sel_y = -1;
        }
    }

    draw_board();
}

const Experiment g_exp_game_chess = {
    .id = 101,
    .title = "CHESS",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = 0,
};
