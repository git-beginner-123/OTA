#include "experiments/experiment.h"
#include "experiments/go_record_store.h"
#include "ui/ui.h"
#include "display/st7789.h"
#include "display/font8x16.h"
#include "audio/sfx.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>

#define N 13
#define BOARD_CELLS (N * N)
#define CELL 16
#define STONE_SIZE 11
#define BOARD_W ((N - 1) * CELL)
#define BOARD_H ((N - 1) * CELL)
#define MAX_REC 24
#define MAX_MOVES 255

typedef enum {
    kStoneNone = 0,
    kStoneBlack = 1,
    kStoneWhite = 2,
} Stone;

typedef enum {
    kReplayPageList = 0,
    kReplayPageView,
} ReplayPage;

static ReplayPage s_page = kReplayPageList;
static GoRecordInfo s_records[MAX_REC];
static int s_rec_count = 0;
static int s_rec_sel = 0;

static GoRecordInfo s_cur_rec;
static uint8_t s_moves_xy[MAX_MOVES * 2];
static int s_move_count = 0;
static int s_step = 0;
static Stone s_board[BOARD_CELLS];
static int s_ko_point = -1;
static Stone s_trial_turn = kStoneBlack;
static bool s_trial_mode = false;
static int s_cursor_x = 6;
static int s_cursor_y = 6;
static char s_trial_tip[40] = "";
static bool s_list_frame_ready = false;
static int64_t s_enter_first_ms = 0;
static int64_t s_enter_last_ms = 0;
static bool s_enter_long_latched = false;

static inline int idx(int x, int y) { return y * N + x; }
static inline bool inside(int x, int y) { return x >= 0 && x < N && y >= 0 && y < N; }
static inline Stone other(Stone s) { return (s == kStoneBlack) ? kStoneWhite : kStoneBlack; }

static int board_x0(void) { return (St7789_Width() - BOARD_W) / 2; }
static int board_y0(void) { return (St7789_Height() - BOARD_H) / 2; }
static uint16_t c_bg(void) { return Ui_ColorRGB(25, 23, 18); }
static uint16_t c_wood(void) { return Ui_ColorRGB(168, 126, 73); }
static uint16_t c_grid(void) { return Ui_ColorRGB(82, 54, 24); }
static uint16_t c_black(void) { return Ui_ColorRGB(24, 24, 24); }
static uint16_t c_white(void) { return Ui_ColorRGB(236, 236, 236); }
static uint16_t c_cursor(void) { return Ui_ColorRGB(90, 220, 255); }
static uint16_t c_text(void) { return Ui_ColorRGB(232, 236, 240); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 170, 120); }
static uint16_t c_ok(void) { return Ui_ColorRGB(160, 240, 160); }
static int64_t now_ms(void) { return (int64_t)(esp_timer_get_time() / 1000ULL); }

static bool is_star_point(int x, int y)
{
    const int stars[5][2] = {{3,3}, {3,9}, {6,6}, {9,3}, {9,9}};
    for (int i = 0; i < 5; i++) {
        if (x == stars[i][0] && y == stars[i][1]) return true;
    }
    return false;
}

static void draw_char_normal(int x, int y, char c, uint16_t fg)
{
    const uint8_t* rows = Font8x16_Get(c);
    if (!rows) rows = Font8x16_Get('?');
    if (!rows) return;
    for (int ry = 0; ry < 16; ry++) {
        uint8_t bits = rows[ry];
        for (int rx = 0; rx < 8; rx++) {
            if (bits & (0x80U >> rx)) St7789_DrawPixel(x + rx, y + ry, fg);
        }
    }
}

static void draw_text_normal_center(int y, const char* text, uint16_t fg)
{
    int len = 0;
    for (const char* p = text; *p; p++) len++;
    if (len <= 0) return;
    int char_w = 9;
    int total_w = len * char_w - 1;
    int x = (St7789_Width() - total_w) / 2;
    for (int i = 0; i < len; i++) draw_char_normal(x + i * char_w, y, text[i], fg);
}

static void draw_static_board(void)
{
    int x0 = board_x0();
    int y0 = board_y0();

    St7789_Fill(c_bg());
    St7789_FillRect(x0 - 10, y0 - 10, BOARD_W + 21, BOARD_H + 21, c_wood());
    for (int i = 0; i < N; i++) {
        int x = x0 + i * CELL;
        int y = y0 + i * CELL;
        St7789_FillRect(x, y0, 1, BOARD_H + 1, c_grid());
        St7789_FillRect(x0, y, BOARD_W + 1, 1, c_grid());
    }
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            if (!is_star_point(x, y)) continue;
            int px = x0 + x * CELL;
            int py = y0 + y * CELL;
            St7789_FillRect(px - 1, py - 1, 3, 3, c_grid());
        }
    }
}

static void draw_stone_at(int x, int y)
{
    Stone s = s_board[idx(x, y)];
    if (s == kStoneNone) return;
    int x0 = board_x0();
    int y0 = board_y0();
    int px = x0 + x * CELL - (STONE_SIZE / 2);
    int py = y0 + y * CELL - (STONE_SIZE / 2);
    St7789_FillRect(px, py, STONE_SIZE, STONE_SIZE, (s == kStoneBlack) ? c_black() : c_white());
}

static void draw_all_stones(void)
{
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) draw_stone_at(x, y);
    }
}

static void draw_cursor(void)
{
    int x0 = board_x0();
    int y0 = board_y0();
    int cx = x0 + s_cursor_x * CELL;
    int cy = y0 + s_cursor_y * CELL;
    St7789_FillRect(cx - 7, cy - 7, 15, 1, c_cursor());
    St7789_FillRect(cx - 7, cy + 7, 15, 1, c_cursor());
    St7789_FillRect(cx - 7, cy - 7, 1, 15, c_cursor());
    St7789_FillRect(cx + 7, cy - 7, 1, 15, c_cursor());
}

static void redraw_intersection(int x, int y, bool draw_cursor_here)
{
    int x0 = board_x0();
    int y0 = board_y0();
    int px = x0 + x * CELL;
    int py = y0 + y * CELL;

    int half = CELL / 2;
    int left = (x > 0) ? half : 0;
    int right = (x < N - 1) ? half : 0;
    int up = (y > 0) ? half : 0;
    int down = (y < N - 1) ? half : 0;

    int rx = px - left;
    int ry = py - up;
    int rw = left + right + 1;
    int rh = up + down + 1;

    St7789_FillRect(rx - 1, ry - 1, rw + 2, rh + 2, c_wood());
    St7789_FillRect(px - left, py, rw, 1, c_grid());
    St7789_FillRect(px, py - up, 1, rh, c_grid());
    if (is_star_point(x, y)) St7789_FillRect(px - 1, py - 1, 3, 3, c_grid());
    draw_stone_at(x, y);
    if (draw_cursor_here) draw_cursor();
}

static void draw_view_overlay(void)
{
    char top[64];
    char bottom[64];
    snprintf(top, sizeof(top), "%s %d/%d", s_cur_rec.name, s_step, s_move_count);
    if (s_trial_mode) {
        snprintf(bottom, sizeof(bottom), "TRIAL UD/LR BACK:PLAY ENTER:EXIT");
    } else {
        snprintf(bottom, sizeof(bottom), "LEFT PREV RIGHT NEXT ENTER TRIAL");
    }
    St7789_FillRect(0, 0, St7789_Width(), 20, c_bg());
    St7789_FillRect(0, 20, St7789_Width(), 16, c_bg());
    St7789_FillRect(0, St7789_Height() - 20, St7789_Width(), 20, c_bg());
    draw_text_normal_center(2, top, c_text());
    if (s_trial_mode && s_trial_tip[0]) {
        draw_text_normal_center(20, s_trial_tip, c_warn());
    }
    draw_text_normal_center(St7789_Height() - 18, bottom, s_trial_mode ? c_warn() : c_ok());
}

static void draw_view_full(void)
{
    Ui_BeginBatch();
    draw_static_board();
    draw_all_stones();
    if (s_trial_mode) draw_cursor();
    draw_view_overlay();
    Ui_EndBatch();
}

static void draw_overlay_only(void)
{
    Ui_BeginBatch();
    draw_view_overlay();
    Ui_EndBatch();
}

static void redraw_diff_from_prev(const Stone* prev_board)
{
    Ui_BeginBatch();
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            int p = idx(x, y);
            if (prev_board[p] == s_board[p]) continue;
            bool draw_cur = s_trial_mode && x == s_cursor_x && y == s_cursor_y;
            redraw_intersection(x, y, draw_cur);
        }
    }
    if (s_trial_mode) {
        redraw_intersection(s_cursor_x, s_cursor_y, true);
    }
    draw_view_overlay();
    Ui_EndBatch();
}

static int collect_group_and_liberties(const Stone* board,
                                       int sx,
                                       int sy,
                                       Stone color,
                                       int* out_group,
                                       int* out_group_count)
{
    bool seen[BOARD_CELLS] = {0};
    bool lib_seen[BOARD_CELLS] = {0};
    int queue[BOARD_CELLS];
    int qh = 0;
    int qt = 0;
    int libs = 0;

    int start = idx(sx, sy);
    seen[start] = true;
    queue[qt++] = start;
    int gc = 0;

    while (qh < qt) {
        int p = queue[qh++];
        int x = p % N;
        int y = p / N;
        out_group[gc++] = p;

        const int dxy[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
        for (int i = 0; i < 4; i++) {
            int nx = x + dxy[i][0];
            int ny = y + dxy[i][1];
            if (!inside(nx, ny)) continue;
            int np = idx(nx, ny);
            Stone s = board[np];
            if (s == kStoneNone) {
                if (!lib_seen[np]) {
                    lib_seen[np] = true;
                    libs++;
                }
            } else if (s == color && !seen[np]) {
                seen[np] = true;
                queue[qt++] = np;
            }
        }
    }

    *out_group_count = gc;
    return libs;
}

static int apply_move_on_board(Stone* board, int* io_ko_point, Stone me, int x, int y)
{
    int p = idx(x, y);
    if (board[p] != kStoneNone) return 1;
    if (p == *io_ko_point) return 3;

    Stone opp = other(me);
    Stone next[BOARD_CELLS];
    memcpy(next, board, sizeof(next));
    next[p] = me;

    int total_captured = 0;
    int captured_one_pos = -1;
    const int dxy[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
    for (int i = 0; i < 4; i++) {
        int nx = x + dxy[i][0];
        int ny = y + dxy[i][1];
        if (!inside(nx, ny)) continue;
        int np = idx(nx, ny);
        if (next[np] != opp) continue;
        int grp[BOARD_CELLS];
        int gc = 0;
        int libs = collect_group_and_liberties(next, nx, ny, opp, grp, &gc);
        if (libs == 0) {
            for (int k = 0; k < gc; k++) next[grp[k]] = kStoneNone;
            total_captured += gc;
            if (gc == 1) captured_one_pos = grp[0];
        }
    }

    int my_grp[BOARD_CELLS];
    int my_gc = 0;
    int my_libs = collect_group_and_liberties(next, x, y, me, my_grp, &my_gc);
    if (my_libs == 0) return 2;

    memcpy(board, next, sizeof(next));
    if (total_captured == 1 && my_gc == 1 && my_libs == 1) *io_ko_point = captured_one_pos;
    else *io_ko_point = -1;
    return 0;
}

static void rebuild_to_step(int step)
{
    if (step < 0) step = 0;
    if (step > s_move_count) step = s_move_count;
    memset(s_board, 0, sizeof(s_board));
    s_ko_point = -1;
    Stone turn = kStoneBlack;
    for (int i = 0; i < step; i++) {
        int x = (int)s_moves_xy[i * 2];
        int y = (int)s_moves_xy[i * 2 + 1];
        if (!inside(x, y)) break;
        if (apply_move_on_board(s_board, &s_ko_point, turn, x, y) != 0) break;
        turn = other(turn);
    }
    s_step = step;
    s_trial_turn = turn;
}

static void draw_record_list_rows(void)
{
    if (!s_list_frame_ready) {
        Ui_DrawFrame("GO REPLAY", "UP/DN SEL OK OPEN BACK RET");
        s_list_frame_ready = true;
    }

    Ui_BeginBatch();
    for (int r = 0; r < 9; r++) {
        Ui_DrawBodyTextRowColor(r, "", c_text());
    }

    if (s_rec_count <= 0) {
        Ui_DrawBodyTextRowColor(1, "No saved records", c_warn());
        Ui_DrawBodyTextRowColor(2, "Use GO 13x13 -> BACK -> Save", c_text());
        Ui_EndBatch();
        return;
    }

    int top = s_rec_sel - 3;
    if (top < 0) top = 0;
    if (top > s_rec_count - 7) top = s_rec_count - 7;
    if (top < 0) top = 0;

    for (int r = 0; r < 7; r++) {
        int i = top + r;
        if (i >= s_rec_count) continue;
        char line[64];
        snprintf(line, sizeof(line), "%c %s  m:%d",
                 (i == s_rec_sel) ? '>' : ' ',
                 s_records[i].name,
                 (int)s_records[i].move_count);
        Ui_DrawBodyTextRowColor(r, line, (i == s_rec_sel) ? c_ok() : c_text());
    }
    Ui_EndBatch();
}

static void draw_record_list(void)
{
    draw_record_list_rows();
}

static InputKey map_key(InputKey raw)
{
    switch (raw) {
        case kInputWhiteUp: return kInputUp;
        case kInputWhiteDown: return kInputDown;
        case kInputWhiteLeft: return kInputLeft;
        case kInputWhiteRight: return kInputRight;
        case kInputWhiteBack: return kInputBack;
        default: return raw;
    }
}

static void refresh_record_list(void)
{
    s_rec_count = GoRecordStore_List(s_records, MAX_REC);
    if (s_rec_sel >= s_rec_count) s_rec_sel = s_rec_count - 1;
    if (s_rec_sel < 0) s_rec_sel = 0;
}

static bool open_selected_record(void)
{
    if (s_rec_sel < 0 || s_rec_sel >= s_rec_count) return false;

    int move_count = 0;
    if (!GoRecordStore_LoadMoves(s_records[s_rec_sel].id, s_moves_xy, sizeof(s_moves_xy), &move_count)) {
        return false;
    }

    s_cur_rec = s_records[s_rec_sel];
    s_move_count = move_count;
    s_trial_mode = false;
    s_trial_tip[0] = 0;
    s_enter_first_ms = 0;
    s_enter_last_ms = 0;
    s_enter_long_latched = false;
    s_cursor_x = 6;
    s_cursor_y = 6;
    rebuild_to_step(0);
    return true;
}

static bool detect_enter_long_press(void)
{
    int64_t t = now_ms();
    if ((t - s_enter_last_ms) > 260) {
        s_enter_first_ms = t;
        s_enter_long_latched = false;
    }
    s_enter_last_ms = t;
    if (!s_enter_long_latched && (t - s_enter_first_ms) >= 420) {
        s_enter_long_latched = true;
        return true;
    }
    return false;
}

static void clear_enter_hold_window(void)
{
    s_enter_first_ms = 0;
    s_enter_last_ms = 0;
    s_enter_long_latched = false;
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("GO REPLAY", "OK:START BACK:RET");
    Ui_Println("Record list sorted by date desc");
    Ui_Println("Open: ENTER");
    Ui_Println("Replay: LEFT/RIGHT");
    Ui_Println("Trial: long ENTER in/out");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    s_page = kReplayPageList;
    s_list_frame_ready = false;
    refresh_record_list();
    draw_record_list();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
}

static void on_key(ExperimentContext* ctx, InputKey raw_key)
{
    (void)ctx;
    InputKey key = map_key(raw_key);

    if (s_page == kReplayPageList) {
        if (key == kInputUp) {
            if (s_rec_sel > 0) s_rec_sel--;
            draw_record_list();
            return;
        }
        if (key == kInputDown) {
            if (s_rec_sel + 1 < s_rec_count) s_rec_sel++;
            draw_record_list();
            return;
        }
        if (key == kInputEnter) {
            if (open_selected_record()) {
                Sfx_PlayNotify();
                s_page = kReplayPageView;
                draw_view_full();
            } else {
                Ui_DrawBodyTextRowColor(8, "Open failed", c_warn());
            }
        }
        return;
    }

    if (s_trial_mode) {
        if (key == kInputEnter) {
            if (!detect_enter_long_press()) return;
            Stone prev[BOARD_CELLS];
            memcpy(prev, s_board, sizeof(prev));
            s_trial_mode = false;
            s_trial_tip[0] = 0;
            rebuild_to_step(s_step);
            redraw_diff_from_prev(prev);
            clear_enter_hold_window();
            return;
        }
        clear_enter_hold_window();
        if (key == kInputBack) {
            Stone prev[BOARD_CELLS];
            memcpy(prev, s_board, sizeof(prev));
            int r = apply_move_on_board(s_board, &s_ko_point, s_trial_turn, s_cursor_x, s_cursor_y);
            if (r == 0) {
                s_trial_turn = other(s_trial_turn);
                snprintf(s_trial_tip, sizeof(s_trial_tip), "trial ok");
                Sfx_PlayNotify();
                redraw_diff_from_prev(prev);
            } else if (r == 1) {
                snprintf(s_trial_tip, sizeof(s_trial_tip), "occupied");
                draw_overlay_only();
            } else if (r == 2) {
                snprintf(s_trial_tip, sizeof(s_trial_tip), "suicide");
                draw_overlay_only();
            } else {
                snprintf(s_trial_tip, sizeof(s_trial_tip), "ko");
                draw_overlay_only();
            }
            return;
        }
        int nx = s_cursor_x;
        int ny = s_cursor_y;
        if (key == kInputUp && ny > 0) ny--;
        if (key == kInputDown && ny < N - 1) ny++;
        if (key == kInputLeft && nx > 0) nx--;
        if (key == kInputRight && nx < N - 1) nx++;
        if (nx != s_cursor_x || ny != s_cursor_y) {
            int ox = s_cursor_x;
            int oy = s_cursor_y;
            s_cursor_x = nx;
            s_cursor_y = ny;
            Ui_BeginBatch();
            redraw_intersection(ox, oy, false);
            redraw_intersection(s_cursor_x, s_cursor_y, true);
            draw_view_overlay();
            Ui_EndBatch();
        }
        return;
    }

    clear_enter_hold_window();
    if (key == kInputLeft) {
        if (s_step > 0) {
            Stone prev[BOARD_CELLS];
            memcpy(prev, s_board, sizeof(prev));
            rebuild_to_step(s_step - 1);
            redraw_diff_from_prev(prev);
        }
        return;
    }
    if (key == kInputRight) {
        if (s_step < s_move_count) {
            Stone prev[BOARD_CELLS];
            memcpy(prev, s_board, sizeof(prev));
            rebuild_to_step(s_step + 1);
            redraw_diff_from_prev(prev);
        }
        return;
    }
    if (key == kInputEnter) {
        if (!detect_enter_long_press()) return;
        s_trial_mode = true;
        snprintf(s_trial_tip, sizeof(s_trial_tip), "turn: %s", (s_trial_turn == kStoneBlack) ? "black" : "white");
        Ui_BeginBatch();
        redraw_intersection(s_cursor_x, s_cursor_y, true);
        draw_view_overlay();
        Ui_EndBatch();
        clear_enter_hold_window();
        return;
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
}

const Experiment g_exp_game_go13_replay = {
    .id = 107,
    .title = "GO REPLAY",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};

bool ExpGoReplay_ShouldConsumeBack(void)
{
    return (s_page == kReplayPageView && s_trial_mode);
}
