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
static int s_dice = 1;

static inline int idx(int x, int y) { return y * N + x; }
static int board_x0(void) { return (St7789_Width() - N * CELL) / 2; }
static int board_y0(void) { return 56; }

static uint16_t c_dark(void) { return Ui_ColorRGB(70, 92, 78); }
static uint16_t c_light(void) { return Ui_ColorRGB(174, 204, 180); }
static uint16_t c_cursor(void) { return Ui_ColorRGB(84, 224, 252); }
static uint16_t c_sel(void) { return Ui_ColorRGB(255, 202, 80); }

static int next_dice(int old)
{
    old++;
    if (old > 6) old = 1;
    return old;
}

static bool piece_matches_dice(char p)
{
    char u = (char)toupper((unsigned char)p);
    switch (s_dice) {
        case 1: return u == 'P';
        case 2: return u == 'N';
        case 3: return u == 'B';
        case 4: return u == 'R';
        case 5: return u == 'Q';
        case 6: return u == 'K';
        default: return false;
    }
}

static bool belongs_to_turn(char p)
{
    if (!p) return false;
    if (s_white_turn && !isupper((unsigned char)p)) return false;
    if (!s_white_turn && !islower((unsigned char)p)) return false;
    return piece_matches_dice(p);
}

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
    s_dice = 1;
}

static void draw_board(void)
{
    int x0 = board_x0();
    int y0 = board_y0();

    St7789_FillRect(0, 30, St7789_Width(), St7789_Height() - 30, Ui_ColorRGB(20, 34, 28));

    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int px = x0 + x * CELL;
            int py = y0 + y * CELL;
            uint16_t cc = ((x + y) & 1) ? c_dark() : c_light();
            St7789_FillRect(px, py, CELL, CELL, cc);

            char p = s_board[idx(x, y)];
            if (p) {
                char t[2] = { (char)toupper((unsigned char)p), 0 };
                uint16_t fg = isupper((unsigned char)p) ? Ui_ColorRGB(25, 25, 25) : Ui_ColorRGB(246, 246, 246);
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

    char line[64];
    snprintf(line, sizeof(line), "Turn:%s Dice:%d", s_white_turn ? "W" : "B", s_dice);
    Ui_DrawTextAtBg(8, 34, line, Ui_ColorRGB(230, 235, 240), Ui_ColorRGB(20, 34, 28));

    St7789_Flush();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("DICE CHESS", "OK:START BACK:RET");
    Ui_Println("Dice chess variant");
    Ui_Println("Dice decides piece type");
    Ui_Println("UP/DN/LF/RT: Move cursor");
    Ui_Println("ENTER: Select / Move");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("DICE CHESS", "MOVE+OK BACK=RET");
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
                s_dice = next_dice(s_dice);
            }
            s_sel_x = -1;
            s_sel_y = -1;
        }
    }

    draw_board();
}

const Experiment g_exp_game_dice_chess = {
    .id = 104,
    .title = "DICE CHESS",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = 0,
};
