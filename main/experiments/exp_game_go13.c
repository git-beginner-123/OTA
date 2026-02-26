#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"
#include "display/font8x16.h"
#include "audio/sfx.h"
#include "comm_wifi.h"
#include "experiments/go_record_store.h"
#include "core/app_settings.h"

#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_timer.h"

#define N 13
#define BOARD_CELLS (N * N)
#define CELL 16
#define STONE_SIZE 11
#define BOARD_W ((N - 1) * CELL)
#define BOARD_H ((N - 1) * CELL)
#define KOMI 6.5f
typedef enum {
    kStoneNone = 0,
    kStoneBlack = 1,
    kStoneWhite = 2,
} Stone;

static Stone s_board[BOARD_CELLS];
static int s_cursor_x = 6;
static int s_cursor_y = 6;
static Stone s_turn = kStoneBlack;
static int s_ko_point = -1;

static int s_cap_black = 0;
static int s_cap_white = 0;
static int s_terr_black = 0;
static int s_terr_white = 0;

static int s_black_ms = 10 * 60 * 1000;
static int s_white_ms = 10 * 60 * 1000;
static int s_black_byo_ms = 0;
static int s_white_byo_ms = 0;
static int s_black_byo_left = 0;
static int s_white_byo_left = 0;
static int s_cfg_main_ms = 10 * 60 * 1000;
static int s_cfg_byo_ms = 30 * 1000;
static int s_cfg_byo_count = 3;
static int64_t s_last_tick_ms = 0;
static int s_last_draw_black_sec = -1;
static int s_last_draw_white_sec = -1;
static int s_last_draw_black_byo_sec = -1;
static int s_last_draw_white_byo_sec = -1;
static int s_last_draw_black_byo_left = -1;
static int s_last_draw_white_byo_left = -1;
static Stone s_hud_turn_cache = kStoneNone;
static char s_top_m_cache[20] = "";
static char s_top_b_cache[20] = "";
static char s_top_c_cache[20] = "";
static char s_bottom_m_cache[20] = "";
static char s_bottom_b_cache[20] = "";
static char s_bottom_c_cache[20] = "";
static char s_status_cache[64] = "";
static Stone s_status_cache_side = kStoneNone;
static int s_status_cache_ui = 0;
static Stone s_status_side = kStoneNone;

static int s_focus_black_x = 9;
static int s_focus_black_y = 3;
static int s_focus_white_x = 3;
static int s_focus_white_y = 9;
static int s_last_move_x = -1;
static int s_last_move_y = -1;

typedef enum {
    kGoUiPlay = 0,
    kGoUiActionMenu,
    kGoUiCountRequest,
} GoUiState;

typedef enum {
    kMenuCancel = 0,
    kMenuCountRequest,
    kMenuExit,
    kMenuResign,
    kMenuSave,
    kMenuPlay,
} GoMenuItem;

static GoUiState s_ui_state = kGoUiPlay;
static int s_menu_sel = kMenuCancel;
static Stone s_menu_owner = kStoneNone;
static Stone s_count_request_from = kStoneNone;
static bool s_game_over = false;
static float s_score_black = 0.0f;
static float s_score_white = 0.0f;
static char s_status[64] = "";
static bool s_exit_requested = false;
static bool s_exit_from_non_back = false;
static InputKey s_exit_trigger_key = kInputNone;
static uint8_t s_moves_xy[BOARD_CELLS * 2];
static int s_move_count = 0;
static bool s_saved_once = false;
static int s_saved_move_count = -1;
static char s_saved_name[20] = "";
static bool s_dirty_since_save = false;

static inline int idx(int x, int y) { return y * N + x; }
static inline bool inside(int x, int y) { return x >= 0 && x < N && y >= 0 && y < N; }
static inline Stone other(Stone s) { return (s == kStoneBlack) ? kStoneWhite : kStoneBlack; }

static int board_x0(void) { return (St7789_Width() - BOARD_W) / 2; }
static int board_y0(void) { return (St7789_Height() - BOARD_H) / 2; }

static uint16_t c_bg(void) { return Ui_ColorRGB(32, 25, 15); }
static uint16_t c_wood(void) { return Ui_ColorRGB(173, 132, 77); }
static uint16_t c_grid(void) { return Ui_ColorRGB(84, 56, 26); }
static uint16_t c_black(void) { return Ui_ColorRGB(24, 24, 24); }
static uint16_t c_white(void) { return Ui_ColorRGB(236, 236, 236); }
static uint16_t c_cursor(void) { return Ui_ColorRGB(90, 220, 255); }
static uint16_t c_text(void) { return Ui_ColorRGB(232, 236, 240); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 170, 120); }
static uint16_t c_ok(void) { return Ui_ColorRGB(160, 240, 160); }
static uint16_t c_hi_white(void) { return Ui_ColorRGB(255, 246, 170); }
static uint16_t c_hi_black(void) { return Ui_ColorRGB(180, 230, 255); }
static uint16_t c_hud_m(void) { return Ui_ColorRGB(255, 106, 86); }
static uint16_t c_hud_b(void) { return Ui_ColorRGB(74, 216, 255); }
static uint16_t c_hud_c(void) { return Ui_ColorRGB(120, 255, 120); }
static uint16_t c_hud_label_bg(void) { return Ui_ColorRGB(18, 28, 42); }

static void update_action_menu_status(void);
static void update_count_request_status(void);
static InputKey normalize_key_for_side(InputKey raw, Stone side);
static bool is_play_menu_mode(void);
static const GoMenuItem* active_menu_items(int* out_count);
static int find_menu_index(GoMenuItem item, const GoMenuItem* items, int count);

static int64_t now_ms(void)
{
    return (int64_t)(esp_timer_get_time() / 1000ULL);
}

static bool is_star_point(int x, int y)
{
    const int stars[5][2] = {{3,3}, {3,9}, {6,6}, {9,3}, {9,9}};
    for (int i = 0; i < 5; i++) {
        if (x == stars[i][0] && y == stars[i][1]) return true;
    }
    return false;
}

static uint8_t rev8(uint8_t v)
{
    v = (uint8_t)(((v & 0xF0U) >> 4) | ((v & 0x0FU) << 4));
    v = (uint8_t)(((v & 0xCCU) >> 2) | ((v & 0x33U) << 2));
    v = (uint8_t)(((v & 0xAAU) >> 1) | ((v & 0x55U) << 1));
    return v;
}

static void draw_char_rot180(int x, int y, char c, uint16_t fg)
{
    const uint8_t* rows = Font8x16_Get(c);
    if (!rows) rows = Font8x16_Get('?');
    if (!rows) return;

    for (int ry = 0; ry < 16; ry++) {
        uint8_t bits = rev8(rows[15 - ry]);
        for (int rx = 0; rx < 8; rx++) {
            if (bits & (0x80U >> rx)) {
                St7789_DrawPixel(x + rx, y + ry, fg);
            }
        }
    }
}

static void draw_text_rot180_center(int y, const char* text, uint16_t fg)
{
    int len = 0;
    for (const char* p = text; *p; p++) len++;
    if (len <= 0) return;

    int char_w = 9;
    int total_w = len * char_w - 1;
    int x = (St7789_Width() - total_w) / 2;

    for (int i = 0; i < len; i++) {
        char c = text[len - 1 - i];
        draw_char_rot180(x + i * char_w, y, c, fg);
    }
}

static void draw_text_rot180_at(int x, int y, const char* text, uint16_t fg)
{
    if (!text) return;
    int len = 0;
    for (const char* p = text; *p; ++p) len++;
    for (int i = 0; i < len; i++) {
        draw_char_rot180(x + i * 9, y, text[len - 1 - i], fg);
    }
}

static void draw_char_normal(int x, int y, char c, uint16_t fg)
{
    const uint8_t* rows = Font8x16_Get(c);
    if (!rows) rows = Font8x16_Get('?');
    if (!rows) return;

    for (int ry = 0; ry < 16; ry++) {
        uint8_t bits = rows[ry];
        for (int rx = 0; rx < 8; rx++) {
            if (bits & (0x80U >> rx)) {
                St7789_DrawPixel(x + rx, y + ry, fg);
            }
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

    for (int i = 0; i < len; i++) {
        draw_char_normal(x + i * char_w, y, text[i], fg);
    }
}

static void draw_text_normal_at(int x, int y, const char* text, uint16_t fg)
{
    if (!text) return;
    for (int i = 0; text[i]; i++) draw_char_normal(x + i * 9, y, text[i], fg);
}

static void fmt_time(int ms, char out[8])
{
    if (ms < 0) ms = 0;
    int sec = ms / 1000;
    int mm = sec / 60;
    int ss = sec % 60;
    if (mm > 99) mm = 99;
    snprintf(out, 8, "%02d:%02d", mm, ss);
}

static void compose_hud_values_top(char m[20], char b[20], char c[20])
{
    char wt[8];
    fmt_time(s_white_ms, wt);
    snprintf(m, 20, "%s", wt);
    int by_sec = s_white_byo_ms / 1000;
    if (by_sec < 0) by_sec = 0;
    snprintf(b, 20, "%d:%02d", s_white_byo_left, by_sec);
    snprintf(c, 20, "%d", s_cap_white);
}

static void compose_hud_values_bottom(char m[20], char b[20], char c[20])
{
    char bt[8];
    fmt_time(s_black_ms, bt);
    snprintf(m, 20, "%s", bt);
    int by_sec = s_black_byo_ms / 1000;
    if (by_sec < 0) by_sec = 0;
    snprintf(b, 20, "%d:%02d", s_black_byo_left, by_sec);
    snprintf(c, 20, "%d", s_cap_black);
}

static void draw_hud_labels_top(void)
{
    St7789_FillRect(0, 0, St7789_Width(), 20, c_bg());
    St7789_FillRect(2, 1, 10, 18, c_hud_label_bg());
    // Place top-middle label to the right in screen coordinates so it appears
    // on the left from white-side viewing perspective.
    St7789_FillRect(164, 1, 10, 18, c_hud_label_bg());
    St7789_FillRect(174, 1, 10, 18, c_hud_label_bg());
    draw_text_rot180_at(4, 2, "C", c_hud_c());
    draw_text_rot180_at(166, 2, "B", c_hud_b());
    draw_text_rot180_at(176, 2, "M", c_hud_m());
}

static void draw_hud_labels_bottom(void)
{
    St7789_FillRect(0, St7789_Height() - 20, St7789_Width(), 20, c_bg());
    int y = St7789_Height() - 18;
    St7789_FillRect(2, St7789_Height() - 19, 10, 18, c_hud_label_bg());
    St7789_FillRect(80, St7789_Height() - 19, 10, 18, c_hud_label_bg());
    St7789_FillRect(174, St7789_Height() - 19, 10, 18, c_hud_label_bg());
    draw_text_normal_at(4, y, "M", c_hud_m());
    draw_text_normal_at(82, y, "B", c_hud_b());
    draw_text_normal_at(176, y, "C", c_hud_c());
}

static void draw_hud_top_value(int x, int w, const char* text, uint16_t color)
{
    St7789_FillRect(x, 0, w, 20, c_bg());
    draw_text_rot180_at(x, 2, text, color);
}

static void draw_hud_bottom_value(int x, int w, const char* text, uint16_t color)
{
    int y = St7789_Height() - 20;
    St7789_FillRect(x, y, w, 20, c_bg());
    draw_text_normal_at(x, St7789_Height() - 18, text, color);
}

static void draw_hud_if_changed(bool force)
{
    char tm[20], tb[20], tc[20];
    char bm[20], bb[20], bc[20];
    compose_hud_values_top(tm, tb, tc);
    compose_hud_values_bottom(bm, bb, bc);

    bool turn_changed = (s_hud_turn_cache != s_turn);
    uint16_t top_color = (s_turn == kStoneWhite) ? c_hi_white() : c_text();
    uint16_t bottom_color = (s_turn == kStoneBlack) ? c_hi_black() : c_text();

    if (force) {
        s_top_m_cache[0] = 0; s_top_b_cache[0] = 0; s_top_c_cache[0] = 0;
        s_bottom_m_cache[0] = 0; s_bottom_b_cache[0] = 0; s_bottom_c_cache[0] = 0;
        draw_hud_labels_top();
        draw_hud_labels_bottom();
    }

    // Top band is for white player. Use reversed column order in screen coords
    // so white viewpoint reads M-B-C from left to right.
    if (force || turn_changed || strcmp(tc, s_top_c_cache) != 0) {
        draw_hud_top_value(14, 64, tc, top_color);
        strncpy(s_top_c_cache, tc, sizeof(s_top_c_cache) - 1);
        s_top_c_cache[sizeof(s_top_c_cache) - 1] = 0;
    }
    if (force || turn_changed || strcmp(tb, s_top_b_cache) != 0) {
        draw_hud_top_value(92, 80, tb, top_color);
        strncpy(s_top_b_cache, tb, sizeof(s_top_b_cache) - 1);
        s_top_b_cache[sizeof(s_top_b_cache) - 1] = 0;
    }
    if (force || turn_changed || strcmp(tm, s_top_m_cache) != 0) {
        draw_hud_top_value(186, 52, tm, top_color);
        strncpy(s_top_m_cache, tm, sizeof(s_top_m_cache) - 1);
        s_top_m_cache[sizeof(s_top_m_cache) - 1] = 0;
    }

    if (force || turn_changed || strcmp(bm, s_bottom_m_cache) != 0) {
        draw_hud_bottom_value(14, 64, bm, bottom_color);
        strncpy(s_bottom_m_cache, bm, sizeof(s_bottom_m_cache) - 1);
        s_bottom_m_cache[sizeof(s_bottom_m_cache) - 1] = 0;
    }
    if (force || turn_changed || strcmp(bb, s_bottom_b_cache) != 0) {
        draw_hud_bottom_value(92, 80, bb, bottom_color);
        strncpy(s_bottom_b_cache, bb, sizeof(s_bottom_b_cache) - 1);
        s_bottom_b_cache[sizeof(s_bottom_b_cache) - 1] = 0;
    }
    if (force || turn_changed || strcmp(bc, s_bottom_c_cache) != 0) {
        draw_hud_bottom_value(186, 52, bc, bottom_color);
        strncpy(s_bottom_c_cache, bc, sizeof(s_bottom_c_cache) - 1);
        s_bottom_c_cache[sizeof(s_bottom_c_cache) - 1] = 0;
    }

    s_hud_turn_cache = s_turn;
}

static void draw_status_rot180_center(int y, const char* text, uint16_t color)
{
    draw_text_rot180_center(y, text, color);
}

static void draw_status_normal_center(int y, const char* text, uint16_t color)
{
    draw_text_normal_center(y, text, color);
}

static int text_pixel_width(const char* text)
{
    int len = 0;
    for (const char* p = text; p && *p; ++p) len++;
    return (len > 0) ? (len * 9 - 1) : 0;
}

static void draw_rect_frame(int x, int y, int w, int h, uint16_t c)
{
    if (w < 2 || h < 2) return;
    St7789_FillRect(x, y, w, 1, c);
    St7789_FillRect(x, y + h - 1, w, 1, c);
    St7789_FillRect(x, y, 1, h, c);
    St7789_FillRect(x + w - 1, y, 1, h, c);
}

static const char* menu_item_label(GoMenuItem item)
{
    switch (item) {
        case kMenuCancel: return "CANCEL";
        case kMenuCountRequest: return "COUNT";
        case kMenuExit: return "EXIT";
        case kMenuResign: return "RESIGN";
        case kMenuSave: return "SAVE";
        case kMenuPlay: return "PLAY";
        default: return "MENU";
    }
}

static void draw_action_menu_row(bool top_row)
{
    int count = 0;
    const GoMenuItem* items = active_menu_items(&count);
    if (!items || count <= 0) return;

    const int y = top_row ? 20 : (St7789_Height() - 36);
    const int h = 16;
    const int margin = 6;
    const int gap = 4;
    const int usable = St7789_Width() - margin * 2 - gap * (count - 1);
    const int slot_w = (count > 0) ? (usable / count) : 0;

    for (int i = 0; i < count; i++) {
        int x = margin + i * (slot_w + gap);
        bool selected = ((int)items[i] == s_menu_sel);

        uint16_t bg = selected ? Ui_ColorRGB(240, 206, 126) : Ui_ColorRGB(52, 43, 30);
        uint16_t frame = selected ? c_ok() : Ui_ColorRGB(118, 100, 72);
        uint16_t fg = selected ? Ui_ColorRGB(22, 18, 12) : c_text();
        const char* name = menu_item_label(items[i]);

        St7789_FillRect(x, y, slot_w, h, bg);
        draw_rect_frame(x, y, slot_w, h, frame);

        int tw = text_pixel_width(name);
        int tx = x + (slot_w - tw) / 2;
        int ty = y;
        if (top_row) draw_text_rot180_at(tx, ty, name, fg);
        else draw_text_normal_at(tx, ty, name, fg);
    }
}

static void draw_status_if_changed(bool force)
{
    bool ui_changed = (s_ui_state != s_status_cache_ui);
    if (!force && !ui_changed && strcmp(s_status, s_status_cache) == 0 && s_status_side == s_status_cache_side) return;

    // Action menu redraws full button slots itself; avoid clear+repaint flash on LR switching.
    if (s_ui_state != kGoUiActionMenu || force || ui_changed) {
        St7789_FillRect(0, 20, St7789_Width(), 16, c_bg());
        St7789_FillRect(0, St7789_Height() - 36, St7789_Width(), 16, c_bg());
    }

    if (s_ui_state == kGoUiActionMenu) {
        draw_action_menu_row(true);
        draw_action_menu_row(false);
    } else if (s_status[0]) {
        uint16_t col = s_game_over ? c_ok() : c_warn();
        if (s_game_over) {
            draw_status_rot180_center(20, s_status, col);
            draw_status_normal_center(St7789_Height() - 34, s_status, col);
        } else if (s_status_side == kStoneWhite) {
            draw_status_rot180_center(20, s_status, col);
        } else {
            draw_status_normal_center(St7789_Height() - 34, s_status, col);
        }
    }

    strncpy(s_status_cache, s_status, sizeof(s_status_cache) - 1);
    s_status_cache[sizeof(s_status_cache) - 1] = 0;
    s_status_cache_side = s_status_side;
    s_status_cache_ui = s_ui_state;
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

static void draw_last_move_marker(void)
{
    if (s_last_move_x < 0 || s_last_move_y < 0) return;
    if (!inside(s_last_move_x, s_last_move_y)) return;
    Stone s = s_board[idx(s_last_move_x, s_last_move_y)];
    if (s == kStoneNone) return;

    int x0 = board_x0();
    int y0 = board_y0();
    int cx = x0 + s_last_move_x * CELL;
    int cy = y0 + s_last_move_y * CELL;
    uint16_t mark = (s == kStoneBlack) ? c_white() : c_black();
    St7789_FillRect(cx - 1, cy - 1, 3, 3, mark);
}

static void draw_cursor_at(int x, int y)
{
    int x0 = board_x0();
    int y0 = board_y0();
    int cx = x0 + x * CELL;
    int cy = y0 + y * CELL;

    St7789_FillRect(cx - 7, cy - 7, 15, 1, c_cursor());
    St7789_FillRect(cx - 7, cy + 7, 15, 1, c_cursor());
    St7789_FillRect(cx - 7, cy - 7, 1, 15, c_cursor());
    St7789_FillRect(cx + 7, cy - 7, 1, 15, c_cursor());
}

static void redraw_intersection(int x, int y, bool draw_cursor)
{
    int x0 = board_x0();
    int y0 = board_y0();
    int px = x0 + x * CELL;
    int py = y0 + y * CELL;

    // Edge intersections need a wider clear area; cursor arms extend outside
    // the board line region and can leave trails if we only redraw one cell.
    const int local_radius = 14;
    int ax = px - local_radius;
    int ay = py - local_radius;
    int aw = local_radius * 2 + 1;
    int ah = local_radius * 2 + 1;

    // 1) Restore background (paper + board wood overlap).
    St7789_FillRect(ax, ay, aw, ah, c_bg());
    int wood_x = x0 - 10;
    int wood_y = y0 - 10;
    int wood_w = BOARD_W + 21;
    int wood_h = BOARD_H + 21;
    int ox0 = (ax > wood_x) ? ax : wood_x;
    int oy0 = (ay > wood_y) ? ay : wood_y;
    int ox1 = ((ax + aw) < (wood_x + wood_w)) ? (ax + aw) : (wood_x + wood_w);
    int oy1 = ((ay + ah) < (wood_y + wood_h)) ? (ay + ah) : (wood_y + wood_h);
    if (ox1 > ox0 && oy1 > oy0) {
        St7789_FillRect(ox0, oy0, ox1 - ox0, oy1 - oy0, c_wood());
    }

    // 2) Redraw grid segments crossing this local window.
    int gx0 = x0;
    int gy0 = y0;
    int gx1 = x0 + BOARD_W;
    int gy1 = y0 + BOARD_H;
    for (int i = 0; i < N; i++) {
        int lx = x0 + i * CELL;
        if (lx >= ax && lx < ax + aw) {
            int vy0 = (ay > gy0) ? ay : gy0;
            int vy1 = ((ay + ah) < (gy1 + 1)) ? (ay + ah) : (gy1 + 1);
            if (vy1 > vy0) St7789_FillRect(lx, vy0, 1, vy1 - vy0, c_grid());
        }
        int ly = y0 + i * CELL;
        if (ly >= ay && ly < ay + ah) {
            int hx0 = (ax > gx0) ? ax : gx0;
            int hx1 = ((ax + aw) < (gx1 + 1)) ? (ax + aw) : (gx1 + 1);
            if (hx1 > hx0) St7789_FillRect(hx0, ly, hx1 - hx0, 1, c_grid());
        }
    }

    // 3) Redraw star points in this window.
    for (int sy = 0; sy < N; sy++) {
        for (int sx = 0; sx < N; sx++) {
            if (!is_star_point(sx, sy)) continue;
            int spx = x0 + sx * CELL;
            int spy = y0 + sy * CELL;
            if (spx >= ax - 1 && spx <= ax + aw && spy >= ay - 1 && spy <= ay + ah) {
                St7789_FillRect(spx - 1, spy - 1, 3, 3, c_grid());
            }
        }
    }

    // 4) Redraw stones that overlap this window.
    for (int iy = 0; iy < N; iy++) {
        for (int ix = 0; ix < N; ix++) {
            if (s_board[idx(ix, iy)] == kStoneNone) continue;
            int sx = x0 + ix * CELL - (STONE_SIZE / 2);
            int sy = y0 + iy * CELL - (STONE_SIZE / 2);
            if (sx + STONE_SIZE <= ax || sx >= ax + aw || sy + STONE_SIZE <= ay || sy >= ay + ah) continue;
            draw_stone_at(ix, iy);
        }
    }

    if (s_last_move_x >= 0 && s_last_move_y >= 0) {
        int lx = x0 + s_last_move_x * CELL;
        int ly = y0 + s_last_move_y * CELL;
        if (lx >= ax - 2 && lx <= ax + aw + 1 && ly >= ay - 2 && ly <= ay + ah + 1) {
            draw_last_move_marker();
        }
    }

    // 5) Cursor on top.
    if (draw_cursor && !s_game_over) draw_cursor_at(x, y);
}

static void draw_all_stones(void)
{
    for (int y = 0; y < N; y++) {
        for (int x = 0; x < N; x++) {
            draw_stone_at(x, y);
        }
    }
    draw_last_move_marker();
}

static void redraw_all(void)
{
    Ui_BeginBatch();
    draw_static_board();
    draw_all_stones();
    if (!s_game_over) draw_cursor_at(s_cursor_x, s_cursor_y);
    draw_hud_if_changed(true);
    draw_status_if_changed(true);
    Ui_EndBatch();

    s_last_draw_black_sec = s_black_ms / 1000;
    s_last_draw_white_sec = s_white_ms / 1000;
    s_last_draw_black_byo_sec = s_black_byo_ms / 1000;
    s_last_draw_white_byo_sec = s_white_byo_ms / 1000;
    s_last_draw_black_byo_left = s_black_byo_left;
    s_last_draw_white_byo_left = s_white_byo_left;
}

static void redraw_board_and_overlay_incremental(void)
{
    Ui_BeginBatch();
    draw_static_board();
    draw_all_stones();
    if (!s_game_over) draw_cursor_at(s_cursor_x, s_cursor_y);
    draw_hud_if_changed(false);
    draw_status_if_changed(false);
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

static int try_play_move(int x, int y)
{
    // return: 0=ok, 1=occupied, 2=suicide, 3=ko
    int p = idx(x, y);
    if (s_board[p] != kStoneNone) return 1;
    if (p == s_ko_point) return 3;

    Stone me = s_turn;
    Stone opp = other(me);

    Stone next[BOARD_CELLS];
    memcpy(next, s_board, sizeof(next));
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

    memcpy(s_board, next, sizeof(s_board));

    if (me == kStoneBlack) s_cap_black += total_captured;
    else s_cap_white += total_captured;

    if (total_captured == 1 && my_gc == 1 && my_libs == 1) s_ko_point = captured_one_pos;
    else s_ko_point = -1;

    if (me == kStoneBlack) {
        s_focus_black_x = x;
        s_focus_black_y = y;
    } else {
        s_focus_white_x = x;
        s_focus_white_y = y;
    }

    s_turn = opp;
    if (s_turn == kStoneBlack && s_black_ms <= 0 && s_black_byo_left > 0) {
        s_black_byo_ms = s_cfg_byo_ms;
    } else if (s_turn == kStoneWhite && s_white_ms <= 0 && s_white_byo_left > 0) {
        s_white_byo_ms = s_cfg_byo_ms;
    }
    if (s_turn == kStoneBlack) {
        s_cursor_x = s_focus_black_x;
        s_cursor_y = s_focus_black_y;
    } else {
        s_cursor_x = s_focus_white_x;
        s_cursor_y = s_focus_white_y;
    }
    return 0;
}

static void compute_territory(void)
{
    bool seen[BOARD_CELLS] = {0};
    s_terr_black = 0;
    s_terr_white = 0;

    for (int p = 0; p < BOARD_CELLS; p++) {
        if (seen[p] || s_board[p] != kStoneNone) continue;

        int queue[BOARD_CELLS];
        int qh = 0;
        int qt = 0;
        int rc = 0;
        bool touch_black = false;
        bool touch_white = false;

        seen[p] = true;
        queue[qt++] = p;

        while (qh < qt) {
            int cur = queue[qh++];
            rc++;

            int x = cur % N;
            int y = cur / N;
            const int dxy[4][2] = {{1,0},{-1,0},{0,1},{0,-1}};
            for (int i = 0; i < 4; i++) {
                int nx = x + dxy[i][0];
                int ny = y + dxy[i][1];
                if (!inside(nx, ny)) continue;
                int np = idx(nx, ny);
                Stone s = s_board[np];
                if (s == kStoneNone) {
                    if (!seen[np]) {
                        seen[np] = true;
                        queue[qt++] = np;
                    }
                } else if (s == kStoneBlack) {
                    touch_black = true;
                } else {
                    touch_white = true;
                }
            }
        }

        if (touch_black && !touch_white) s_terr_black += rc;
        else if (touch_white && !touch_black) s_terr_white += rc;
    }
}

static void finish_game(void)
{
    compute_territory();
    s_score_black = (float)s_terr_black + (float)s_cap_black;
    s_score_white = (float)s_terr_white + (float)s_cap_white + KOMI;
    s_game_over = true;

    if (s_score_black > s_score_white) {
        snprintf(s_status, sizeof(s_status), "Black wins +%.1f", s_score_black - s_score_white);
    } else {
        snprintf(s_status, sizeof(s_status), "White wins +%.1f", s_score_white - s_score_black);
    }
    s_status_side = s_turn;
}

static void apply_resign(Stone loser)
{
    if (s_game_over) return;
    Stone winner = other(loser);
    s_game_over = true;
    snprintf(s_status, sizeof(s_status), "%s resigns", (loser == kStoneBlack) ? "Black" : "White");
    s_status_side = loser;
    s_turn = winner;
}

static void open_action_menu(Stone owner)
{
    int count = 0;
    const GoMenuItem* items = active_menu_items(&count);
    s_ui_state = kGoUiActionMenu;
    s_menu_sel = (count > 0) ? (int)items[0] : kMenuCancel;
    s_menu_owner = owner;
    update_action_menu_status();
}

static void close_action_menu(void)
{
    s_ui_state = kGoUiPlay;
    s_menu_owner = kStoneNone;
    s_status[0] = 0;
    s_status_side = kStoneNone;
}

static void update_action_menu_status(void)
{
    static const char* names[6] = {"CANCEL", "COUNT", "EXIT", "RESIGN", "SAVE", "PLAY"};
    int count = 0;
    const GoMenuItem* items = active_menu_items(&count);
    const char* cur_name = "MENU";
    for (int i = 0; i < count; i++) {
        if ((int)items[i] == s_menu_sel) {
            cur_name = names[items[i]];
            break;
        }
    }
    snprintf(s_status, sizeof(s_status), "MENU  << %s >>", cur_name);
    s_status_side = s_menu_owner;
}

static void update_count_request_status(void)
{
    snprintf(s_status, sizeof(s_status), "Count? OK=yes BACK=no");
    s_status_side = s_turn;
}

static void reset_game(void)
{
    AppSettings cfg;
    AppSettings_Default(&cfg);
    (void)AppSettings_Load(&cfg);
    s_cfg_main_ms = (int)cfg.go_main_min * 60 * 1000;
    s_cfg_byo_ms = (int)cfg.go_byo_sec * 1000;
    s_cfg_byo_count = (int)cfg.go_byo_count;

    memset(s_board, 0, sizeof(s_board));
    s_cursor_x = 6;
    s_cursor_y = 6;
    s_turn = kStoneBlack;
    s_ko_point = -1;

    s_cap_black = 0;
    s_cap_white = 0;
    s_terr_black = 0;
    s_terr_white = 0;

    s_black_ms = s_cfg_main_ms;
    s_white_ms = s_cfg_main_ms;
    s_black_byo_ms = s_cfg_byo_ms;
    s_white_byo_ms = s_cfg_byo_ms;
    s_black_byo_left = s_cfg_byo_count;
    s_white_byo_left = s_cfg_byo_count;
    s_last_tick_ms = now_ms();
    s_last_draw_black_sec = -1;
    s_last_draw_white_sec = -1;
    s_last_draw_black_byo_sec = -1;
    s_last_draw_white_byo_sec = -1;
    s_last_draw_black_byo_left = -1;
    s_last_draw_white_byo_left = -1;
    s_top_m_cache[0] = 0;
    s_top_b_cache[0] = 0;
    s_top_c_cache[0] = 0;
    s_bottom_m_cache[0] = 0;
    s_bottom_b_cache[0] = 0;
    s_bottom_c_cache[0] = 0;
    s_hud_turn_cache = kStoneNone;
    s_status_cache[0] = 0;
    s_status_cache_side = kStoneNone;
    s_status_cache_ui = kGoUiPlay;
    s_focus_black_x = 9;
    s_focus_black_y = 3;
    s_focus_white_x = 3;
    s_focus_white_y = 9;
    s_last_move_x = -1;
    s_last_move_y = -1;

    s_ui_state = kGoUiPlay;
    s_menu_sel = kMenuCancel;
    s_menu_owner = kStoneNone;
    s_count_request_from = kStoneNone;
    s_game_over = false;
    s_score_black = 0.0f;
    s_score_white = 0.0f;
    s_status[0] = 0;
    s_status_side = kStoneNone;
    s_exit_requested = false;
    s_exit_from_non_back = false;
    s_exit_trigger_key = kInputNone;
    s_move_count = 0;
    s_saved_once = false;
    s_saved_move_count = -1;
    s_saved_name[0] = 0;
    s_dirty_since_save = false;

    s_cursor_x = s_focus_black_x;
    s_cursor_y = s_focus_black_y;
}

static bool is_play_menu_mode(void)
{
    return !s_game_over;
}

static const GoMenuItem* active_menu_items(int* out_count)
{
    static const GoMenuItem kPlayItems[] = {
        kMenuCancel, kMenuCountRequest, kMenuResign
    };
    static const GoMenuItem kPostItems[] = {
        kMenuExit, kMenuSave, kMenuPlay
    };

    if (is_play_menu_mode()) {
        if (out_count) *out_count = (int)(sizeof(kPlayItems) / sizeof(kPlayItems[0]));
        return kPlayItems;
    }
    if (out_count) *out_count = (int)(sizeof(kPostItems) / sizeof(kPostItems[0]));
    return kPostItems;
}

static int find_menu_index(GoMenuItem item, const GoMenuItem* items, int count)
{
    for (int i = 0; i < count; i++) {
        if ((int)items[i] == (int)item) return i;
    }
    return -1;
}

static InputKey normalize_key_for_side(InputKey raw, Stone side)
{
    if (side == kStoneBlack) {
        switch (raw) {
            case kInputUp:
            case kInputDown:
            case kInputLeft:
            case kInputRight:
            case kInputEnter:
            case kInputBack:
                return raw;
            default:
                return kInputNone;
        }
    }

    switch (raw) {
        case kInputWhiteUp: return kInputDown;
        case kInputWhiteDown: return kInputUp;
        case kInputWhiteLeft: return kInputRight;
        case kInputWhiteRight: return kInputLeft;
        case kInputWhiteEnter: return kInputEnter;
        case kInputWhiteBack: return kInputBack;
        default: return kInputNone;
    }
}

static InputKey normalize_turn_key(InputKey key)
{
    return normalize_key_for_side(key, s_turn);
}

static Stone menu_owner_from_back_key(InputKey raw_key)
{
    if (raw_key == kInputBack) return kStoneBlack;
    if (raw_key == kInputWhiteBack) return kStoneWhite;
    return kStoneNone;
}

static void update_timers(void)
{
    if (s_game_over) return;
    if (s_ui_state != kGoUiPlay) return;

    int64_t t = now_ms();
    if (s_last_tick_ms <= 0) {
        s_last_tick_ms = t;
        return;
    }

    int delta = (int)(t - s_last_tick_ms);
    if (delta <= 0) return;
    s_last_tick_ms = t;

    if (s_turn == kStoneBlack) {
        int remain = delta;
        if (s_black_ms > 0) {
            if (remain >= s_black_ms) {
                remain -= s_black_ms;
                s_black_ms = 0;
            } else {
                s_black_ms -= remain;
                remain = 0;
            }
        }
        if (s_black_ms <= 0 && s_black_byo_left > 0) {
            if (s_black_byo_ms <= 0) s_black_byo_ms = s_cfg_byo_ms;
            s_black_byo_ms -= remain;
            while (s_black_byo_ms <= 0 && s_black_byo_left > 0) {
                s_black_byo_left--;
                if (s_black_byo_left > 0) s_black_byo_ms += s_cfg_byo_ms;
            }
            if (s_black_byo_left <= 0 && s_black_byo_ms <= 0) {
                s_black_byo_ms = 0;
                s_game_over = true;
                snprintf(s_status, sizeof(s_status), "Black timeout, White wins");
                s_status_side = kStoneBlack;
            }
        }
    } else {
        int remain = delta;
        if (s_white_ms > 0) {
            if (remain >= s_white_ms) {
                remain -= s_white_ms;
                s_white_ms = 0;
            } else {
                s_white_ms -= remain;
                remain = 0;
            }
        }
        if (s_white_ms <= 0 && s_white_byo_left > 0) {
            if (s_white_byo_ms <= 0) s_white_byo_ms = s_cfg_byo_ms;
            s_white_byo_ms -= remain;
            while (s_white_byo_ms <= 0 && s_white_byo_left > 0) {
                s_white_byo_left--;
                if (s_white_byo_left > 0) s_white_byo_ms += s_cfg_byo_ms;
            }
            if (s_white_byo_left <= 0 && s_white_byo_ms <= 0) {
                s_white_byo_ms = 0;
                s_game_over = true;
                snprintf(s_status, sizeof(s_status), "White timeout, Black wins");
                s_status_side = kStoneWhite;
            }
        }
    }

    int bs = s_black_ms / 1000;
    int ws = s_white_ms / 1000;
    int bbs = s_black_byo_ms / 1000;
    int wbs = s_white_byo_ms / 1000;
    if (bs != s_last_draw_black_sec ||
        ws != s_last_draw_white_sec ||
        bbs != s_last_draw_black_byo_sec ||
        wbs != s_last_draw_white_byo_sec ||
        s_black_byo_left != s_last_draw_black_byo_left ||
        s_white_byo_left != s_last_draw_white_byo_left) {
        Ui_BeginBatch();
        draw_hud_if_changed(false);
        draw_status_if_changed(false);
        Ui_EndBatch();
        s_last_draw_black_sec = bs;
        s_last_draw_white_sec = ws;
        s_last_draw_black_byo_sec = bbs;
        s_last_draw_white_byo_sec = wbs;
        s_last_draw_black_byo_left = s_black_byo_left;
        s_last_draw_white_byo_left = s_white_byo_left;
    }
}

static void move_cursor_with_minimal_redraw(int nx, int ny)
{
    if (nx == s_cursor_x && ny == s_cursor_y) return;

    int ox = s_cursor_x;
    int oy = s_cursor_y;
    s_cursor_x = nx;
    s_cursor_y = ny;

    Ui_BeginBatch();
    redraw_intersection(ox, oy, false);
    redraw_intersection(s_cursor_x, s_cursor_y, true);
    Ui_EndBatch();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("GO 13x13", "OK:START BACK:RET");
    Ui_Println("Japanese-rule core:");
    Ui_Println("capture/no-suicide/ko");
    Ui_Println("BACK => Menu");
    Ui_Println("LR select + ENTER");
    Ui_Println("Resign/Save in menu");
    Ui_Println("Time: Main + Byo-yomi");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    reset_game();
    redraw_all();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
}

static void on_key(ExperimentContext* ctx, InputKey raw_key)
{
    (void)ctx;

    update_timers();

    if (s_ui_state == kGoUiPlay && (raw_key == kInputBack || raw_key == kInputWhiteBack)) {
        if (s_game_over && s_saved_once && !s_dirty_since_save) {
            s_exit_requested = true;
            s_exit_from_non_back = false;
            s_exit_trigger_key = raw_key;
            return;
        }
        Stone owner = menu_owner_from_back_key(raw_key);
        if (owner == kStoneNone) {
            return;
        }
        if (is_play_menu_mode() && owner != s_turn) {
            return;
        }
        open_action_menu(owner);
        Ui_BeginBatch();
        draw_status_if_changed(false);
        Ui_EndBatch();
        return;
    }

    if (s_game_over && s_ui_state != kGoUiActionMenu) return;

    if (s_ui_state == kGoUiCountRequest) {
        InputKey key = normalize_turn_key(raw_key);
        if (key == kInputNone) return;
        if (key == kInputEnter) {
            Sfx_PlayNotify();
            finish_game();
            s_ui_state = kGoUiPlay;
            s_count_request_from = kStoneNone;
            Ui_BeginBatch();
            draw_hud_if_changed(false);
            draw_status_if_changed(false);
            Ui_EndBatch();
            return;
        }
        if (key == kInputBack) {
            s_ui_state = kGoUiPlay;
            s_turn = s_count_request_from;
            if (s_turn == kStoneBlack) {
                s_cursor_x = s_focus_black_x;
                s_cursor_y = s_focus_black_y;
            } else {
                s_cursor_x = s_focus_white_x;
                s_cursor_y = s_focus_white_y;
            }
            s_count_request_from = kStoneNone;
            snprintf(s_status, sizeof(s_status), "Count denied");
            s_status_side = s_turn;
            redraw_board_and_overlay_incremental();
            return;
        }
        return;
    }

    if (s_ui_state == kGoUiActionMenu) {
        InputKey key = normalize_key_for_side(raw_key, s_menu_owner);
        int item_count = 0;
        const GoMenuItem* items = active_menu_items(&item_count);
        if (key == kInputNone) return;
        if (key == kInputLeft) {
            int cur = find_menu_index((GoMenuItem)s_menu_sel, items, item_count);
            if (cur < 0) cur = 0;
            cur = (cur - 1 + item_count) % item_count;
            s_menu_sel = (int)items[cur];
            update_action_menu_status();
            Ui_BeginBatch();
            draw_status_if_changed(false);
            Ui_EndBatch();
            return;
        }
        if (key == kInputRight) {
            int cur = find_menu_index((GoMenuItem)s_menu_sel, items, item_count);
            if (cur < 0) cur = 0;
            cur = (cur + 1) % item_count;
            s_menu_sel = (int)items[cur];
            update_action_menu_status();
            Ui_BeginBatch();
            draw_status_if_changed(false);
            Ui_EndBatch();
            return;
        }
        if (key == kInputBack) {
            close_action_menu();
            Ui_BeginBatch();
            draw_status_if_changed(false);
            Ui_EndBatch();
            return;
        }
        if (key == kInputEnter) {
            Sfx_PlayNotify();
            if (s_menu_sel == kMenuCancel) {
                close_action_menu();
                Ui_BeginBatch();
                draw_status_if_changed(false);
                Ui_EndBatch();
                return;
            }
            if (s_menu_sel == kMenuExit) {
                s_exit_requested = true;
                s_exit_from_non_back = true;
                s_exit_trigger_key = raw_key;
                close_action_menu();
                return;
            }
            if (s_menu_sel == kMenuResign) {
                apply_resign(s_menu_owner);
                s_ui_state = kGoUiPlay;
                redraw_all();
                return;
            }
            if (s_menu_sel == kMenuSave) {
                if (s_move_count <= 0) {
                    snprintf(s_status, sizeof(s_status), "No moves to save");
                } else {
                    char rec_name[20] = {0};
                    bool ok = GoRecordStore_SaveMoves(s_moves_xy,
                                                      (uint16_t)s_move_count,
                                                      comm_wifi_is_connected(),
                                                      rec_name,
                                                      sizeof(rec_name));
                    if (ok) {
                        snprintf(s_status, sizeof(s_status), "Saved %s", rec_name);
                        s_saved_once = true;
                        s_saved_move_count = s_move_count;
                        s_dirty_since_save = false;
                        strncpy(s_saved_name, rec_name, sizeof(s_saved_name) - 1);
                        s_saved_name[sizeof(s_saved_name) - 1] = 0;
                    } else {
                        snprintf(s_status, sizeof(s_status), "Save failed");
                    }
                }
                s_status_side = s_menu_owner;
                s_ui_state = kGoUiPlay;
                Ui_BeginBatch();
                draw_status_if_changed(false);
                Ui_EndBatch();
                return;
            }
            if (s_menu_sel == kMenuPlay) {
                reset_game();
                redraw_all();
                return;
            }
            // Count request: switch control to opponent for approve/deny.
            s_count_request_from = s_menu_owner;
            s_turn = other(s_menu_owner);
            s_ui_state = kGoUiCountRequest;
            update_count_request_status();
            redraw_all();
            return;
        }
        return;
    }

    InputKey key = normalize_turn_key(raw_key);
    if (key == kInputNone) return;

    int nx = s_cursor_x;
    int ny = s_cursor_y;

    if (key == kInputUp && ny > 0) ny--;
    if (key == kInputDown && ny < N - 1) ny++;
    if (key == kInputLeft && nx > 0) nx--;
    if (key == kInputRight && nx < N - 1) nx++;

    if (key != kInputEnter) {
        move_cursor_with_minimal_redraw(nx, ny);
        return;
    }
    Sfx_PlayNotify();

    if (s_board[idx(s_cursor_x, s_cursor_y)] != kStoneNone) return;

    Stone board_before[BOARD_CELLS];
    memcpy(board_before, s_board, sizeof(board_before));
    int old_last_move_x = s_last_move_x;
    int old_last_move_y = s_last_move_y;

    int old_cursor_x = s_cursor_x;
    int old_cursor_y = s_cursor_y;
    int px = s_cursor_x;
    int py = s_cursor_y;
    int r = try_play_move(s_cursor_x, s_cursor_y);
    if (r == 0) {
        s_last_move_x = px;
        s_last_move_y = py;
        if (s_move_count < (int)(sizeof(s_moves_xy) / 2)) {
            s_moves_xy[s_move_count * 2] = (uint8_t)px;
            s_moves_xy[s_move_count * 2 + 1] = (uint8_t)py;
            s_move_count++;
        }
        s_dirty_since_save = true;
        s_status[0] = 0;
        s_status_side = kStoneNone;
        s_last_tick_ms = now_ms();
    } else if (r == 1) {
        snprintf(s_status, sizeof(s_status), "occupied");
        s_status_side = s_turn;
    } else if (r == 2) {
        snprintf(s_status, sizeof(s_status), "suicide");
        s_status_side = s_turn;
    } else {
        snprintf(s_status, sizeof(s_status), "ko");
        s_status_side = s_turn;
    }
    if (r == 0) {
        Ui_BeginBatch();
        for (int y = 0; y < N; y++) {
            for (int x = 0; x < N; x++) {
                int p = idx(x, y);
                if (board_before[p] != s_board[p]) {
                    redraw_intersection(x, y, false);
                }
            }
        }
        redraw_intersection(old_cursor_x, old_cursor_y, false);
        redraw_intersection(s_cursor_x, s_cursor_y, true);
        if (old_last_move_x >= 0 && old_last_move_y >= 0 &&
            (old_last_move_x != s_last_move_x || old_last_move_y != s_last_move_y)) {
            bool draw_cur_old = (old_last_move_x == s_cursor_x && old_last_move_y == s_cursor_y);
            redraw_intersection(old_last_move_x, old_last_move_y, draw_cur_old);
        }
        if (s_last_move_x >= 0 && s_last_move_y >= 0) {
            bool draw_cur_new = (s_last_move_x == s_cursor_x && s_last_move_y == s_cursor_y);
            redraw_intersection(s_last_move_x, s_last_move_y, draw_cur_new);
        }
        draw_hud_if_changed(false);
        draw_status_if_changed(false);
        Ui_EndBatch();
    } else {
        Ui_BeginBatch();
        draw_status_if_changed(false);
        Ui_EndBatch();
    }
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    update_timers();
}

const Experiment g_exp_game_go13 = {
    .id = 102,
    .title = "GO 13x13",
    .on_enter = 0,
    .on_exit = 0,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};

bool ExpGo13_ShouldConsumeBack(void)
{
    return true;
}

bool ExpGo13_ShouldConsumeNonBackExit(InputKey key)
{
    return s_exit_requested && s_exit_from_non_back && (key == s_exit_trigger_key);
}

bool ExpGo13_TakeExitRequest(void)
{
    bool r = s_exit_requested;
    s_exit_requested = false;
    s_exit_from_non_back = false;
    s_exit_trigger_key = kInputNone;
    return r;
}
