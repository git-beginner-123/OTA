#include "experiments/experiment.h"
#include "ui/ui.h"
#include "display/st7789.h"
#include "audio/sfx.h"

#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

static const char* TAG = "EXP_SNAKE";

#define UI_HEADER_H 30
#define UI_FOOTER_H 26

#define CELL_SIZE 14
#define GRID_COLS 14
#define GRID_ROWS 14
#define GRID_MAX_CELLS (GRID_COLS * GRID_ROWS)

#define FOOD_ON_BOARD 5
#define FOODS_PER_LEVEL 30
#define LEVEL_COUNT 9
#define STEP_MS 260
#define FEEDBACK_SHOW_MS 1000

typedef enum {
    kDirUp = 0,
    kDirRight,
    kDirDown,
    kDirLeft
} SnakeDir;

typedef enum {
    kSnakeRun = 0,
    kSnakeInputGuess,
    kSnakeLevelClear,
    kSnakeAllClear,
    kSnakeFailed
} SnakePhase;

typedef struct {
    int x;
    int y;
} Cell;

typedef struct {
    Cell c;
    int v; // food number
    bool active;
} FoodItem;

typedef struct {
    bool valid;
    bool ate;
    int ate_idx;
    bool removed_tail;
    Cell old_head;
    Cell old_tail;
    Cell new_head;
} MoveInfo;

static SnakePhase s_phase = kSnakeRun;
static SnakeDir s_dir = kDirRight;
static Cell s_snake[GRID_MAX_CELLS];
static int s_len = 0;
static int s_grow = 0;

static FoodItem s_foods[FOOD_ON_BOARD];
static int s_level = 1;            // 1..9
static int s_eaten_in_level = 0;   // 0..30
static int s_head_true = 10;       // always authoritative
static int s_last_eq_lhs = 10;
static int s_last_eq_delta = 0;
static bool s_last_eq_valid = false;
static int s_guess_value = 10;     // player input 10..99
static int s_pick_row = 0;         // 0=tens row, 1=ones row
static int s_pick_tens = 0;        // 0..8 => 10..90
static int s_pick_ones = 0;        // 0..9
static bool s_last_ok = false;
static bool s_status_dirty = true;

static uint32_t s_next_tick_ms = 0;
static uint32_t s_feedback_until_ms = 0;

static int s_grid_x = 0;
static int s_grid_y = 0;
static int s_grid_w = GRID_COLS * CELL_SIZE;
static int s_grid_h = GRID_ROWS * CELL_SIZE;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static int clamp_10_99(int v)
{
    if (v < 10) return 10;
    if (v > 99) return 99;
    return v;
}

static uint16_t c_bg(void) { return Ui_ColorRGB(10, 16, 24); }
static uint16_t c_field(void) { return Ui_ColorRGB(14, 24, 36); }
static uint16_t c_grid(void) { return Ui_ColorRGB(28, 44, 62); }
static uint16_t c_snake_head(void) { return Ui_ColorRGB(240, 210, 90); }
static uint16_t c_snake_body(void) { return Ui_ColorRGB(120, 210, 110); }
static uint16_t c_food_pos(void) { return Ui_ColorRGB(90, 180, 255); }
static uint16_t c_food_neg(void) { return Ui_ColorRGB(255, 130, 130); }
static uint16_t c_text(void) { return Ui_ColorRGB(235, 235, 235); }
static uint16_t c_warn(void) { return Ui_ColorRGB(255, 120, 120); }
static uint16_t c_ok(void) { return Ui_ColorRGB(120, 230, 130); }
static uint16_t c_black(void) { return Ui_ColorRGB(0, 0, 0); }

static bool cell_equals(Cell a, Cell b)
{
    return a.x == b.x && a.y == b.y;
}

static bool snake_has_cell(Cell c)
{
    for (int i = 0; i < s_len; i++) {
        if (cell_equals(s_snake[i], c)) return true;
    }
    return false;
}

static bool foods_has_cell(Cell c, int ignore_idx)
{
    for (int i = 0; i < FOOD_ON_BOARD; i++) {
        if (i == ignore_idx) continue;
        if (!s_foods[i].active) continue;
        if (cell_equals(s_foods[i].c, c)) return true;
    }
    return false;
}

static int rand_food_value_for_level(int level)
{
    if (level <= 3) {
        return 1 + (int)(esp_random() % 2U); // +1..+2
    }
    if (level <= 6) {
        return 1 + (int)(esp_random() % 8U); // +1..+8
    }
    int mag = 1 + (int)(esp_random() % 8U); // 1..8
    bool neg = ((esp_random() & 1U) != 0U);
    return neg ? -mag : mag; // +/-1..8
}

static int cell_px_x(int gx) { return s_grid_x + gx * CELL_SIZE; }
static int cell_px_y(int gy) { return s_grid_y + gy * CELL_SIZE; }

static void fill_cell(Cell c, uint16_t col)
{
    int px = cell_px_x(c.x);
    int py = cell_px_y(c.y);
    St7789_FillRect(px + 1, py + 1, CELL_SIZE - 2, CELL_SIZE - 2, col);
}

static const uint8_t kDigit4x6[10][6] = {
    {0xF, 0x9, 0x9, 0x9, 0x9, 0xF},
    {0x2, 0x6, 0x2, 0x2, 0x2, 0x7},
    {0xF, 0x1, 0xF, 0x8, 0x8, 0xF},
    {0xF, 0x1, 0x7, 0x1, 0x1, 0xF},
    {0x9, 0x9, 0xF, 0x1, 0x1, 0x1},
    {0xF, 0x8, 0xF, 0x1, 0x1, 0xF},
    {0xF, 0x8, 0xF, 0x9, 0x9, 0xF},
    {0xF, 0x1, 0x1, 0x1, 0x1, 0x1},
    {0xF, 0x9, 0xF, 0x9, 0x9, 0xF},
    {0xF, 0x9, 0xF, 0x1, 0x1, 0xF},
};

static void draw_glyph4x6(int x, int y, const uint8_t rows[6], uint16_t fg)
{
    for (int r = 0; r < 6; r++) {
        uint8_t bits = rows[r];
        for (int c = 0; c < 4; c++) {
            if (bits & (1U << (3 - c))) {
                St7789_FillRect(x + c, y + r, 1, 1, fg);
            }
        }
    }
}

static void draw_plus3x6(int x, int y, uint16_t fg)
{
    static const uint8_t g[6] = {0x2, 0x2, 0x7, 0x2, 0x2, 0x0};
    for (int r = 0; r < 6; r++) {
        uint8_t bits = g[r];
        for (int c = 0; c < 3; c++) {
            if (bits & (1U << (2 - c))) St7789_FillRect(x + c, y + r, 1, 1, fg);
        }
    }
}

static void draw_minus3x6(int x, int y, uint16_t fg)
{
    static const uint8_t g[6] = {0x0, 0x0, 0x7, 0x0, 0x0, 0x0};
    for (int r = 0; r < 6; r++) {
        uint8_t bits = g[r];
        for (int c = 0; c < 3; c++) {
            if (bits & (1U << (2 - c))) St7789_FillRect(x + c, y + r, 1, 1, fg);
        }
    }
}

static void draw_small_signed(Cell c, int value, uint16_t fg)
{
    int px = cell_px_x(c.x) + 1;
    int py = cell_px_y(c.y) + 1;
    int base_x = px + 2;
    int base_y = py + 3;

    if (value < 0) draw_minus3x6(base_x, base_y, fg);
    else draw_plus3x6(base_x, base_y, fg);

    int d = value < 0 ? -value : value;
    if (d > 9) d %= 10;
    draw_glyph4x6(base_x + 4, base_y, kDigit4x6[d], fg);
}

static void draw_head_value(Cell c, int value, uint16_t fg)
{
    // Snake head value is always shown as two digits (10..99).
    int v = clamp_10_99(value);
    int tens = v / 10;
    int ones = v % 10;

    int px = cell_px_x(c.x) + 1;
    int py = cell_px_y(c.y) + 1;
    int base_x = px + 1;
    int base_y = py + 3;

    draw_glyph4x6(base_x, base_y, kDigit4x6[tens], fg);
    draw_glyph4x6(base_x + 5, base_y, kDigit4x6[ones], fg);
}

static void draw_food_item(int idx)
{
    if (idx < 0 || idx >= FOOD_ON_BOARD) return;
    if (!s_foods[idx].active) return;
    uint16_t cc = (s_foods[idx].v < 0) ? c_food_neg() : c_food_pos();
    fill_cell(s_foods[idx].c, cc);
    draw_small_signed(s_foods[idx].c, s_foods[idx].v, c_black());
}

static void spawn_one_food(int idx)
{
    if (idx < 0 || idx >= FOOD_ON_BOARD) return;
    for (int i = 0; i < 256; i++) {
        // Avoid outer ring.
        Cell c = { 1 + (int)(esp_random() % (GRID_COLS - 2)),
                   1 + (int)(esp_random() % (GRID_ROWS - 2)) };
        if (snake_has_cell(c)) continue;
        if (foods_has_cell(c, idx)) continue;
        s_foods[idx].c = c;
        s_foods[idx].v = rand_food_value_for_level(s_level);
        s_foods[idx].active = true;
        return;
    }
    s_foods[idx].c = (Cell){1, 1};
    s_foods[idx].v = 1;
    s_foods[idx].active = true;
}

static void setup_level(int level)
{
    s_level = level;
    s_phase = kSnakeRun;
    s_dir = kDirRight;
    s_len = 3;
    s_grow = 0;
    s_eaten_in_level = 0;
    s_last_ok = false;
    s_feedback_until_ms = 0;
    s_guess_value = 30;
    s_pick_row = 0;
    s_pick_tens = 2;
    s_pick_ones = 0;
    s_status_dirty = true;

    s_head_true = (s_level >= 7) ? 30 : 10;
    s_last_eq_lhs = s_head_true;
    s_last_eq_delta = 0;
    s_last_eq_valid = false;

    int cx = GRID_COLS / 2;
    int cy = GRID_ROWS / 2;
    s_snake[0] = (Cell){cx, cy};
    s_snake[1] = (Cell){cx - 1, cy};
    s_snake[2] = (Cell){cx - 2, cy};

    for (int i = 0; i < FOOD_ON_BOARD; i++) s_foods[i].active = false;
    for (int i = 0; i < FOOD_ON_BOARD; i++) spawn_one_food(i);
    s_next_tick_ms = 0;
}

static void rotate_left(void)
{
    int d = (int)s_dir - 1;
    if (d < 0) d = 3;
    s_dir = (SnakeDir)d;
}

static void rotate_right(void)
{
    int d = ((int)s_dir + 1) & 3;
    s_dir = (SnakeDir)d;
}

static Cell next_head(Cell h)
{
    if (s_dir == kDirUp) h.y -= 1;
    else if (s_dir == kDirRight) h.x += 1;
    else if (s_dir == kDirDown) h.y += 1;
    else h.x -= 1;
    return h;
}

static void draw_status(void)
{
    int y1 = UI_HEADER_H + 4;
    int y2 = UI_HEADER_H + 20;
    St7789_FillRect(0, UI_HEADER_H, St7789_Width(), 36, c_bg());

    char line1[64];
    char line2[64];
    snprintf(line1, sizeof(line1), "R:%d/%d S:%d/%d",
             s_level, LEVEL_COUNT, s_eaten_in_level, FOODS_PER_LEVEL);
    Ui_DrawTextAtBg(8, y1, line1, c_text(), c_bg());

    uint16_t fg = c_text();
    if (s_last_eq_valid) {
        int rhs = clamp_10_99(s_last_eq_lhs + s_last_eq_delta);
        snprintf(line2, sizeof(line2), "%d%+d=%d", s_last_eq_lhs, s_last_eq_delta, rhs);
    } else {
        snprintf(line2, sizeof(line2), "%d%+d=%d", s_head_true, 0, s_head_true);
    }
    if (s_phase == kSnakeInputGuess) fg = c_warn();
    else if (s_phase == kSnakeLevelClear || s_phase == kSnakeAllClear) fg = c_ok();
    else if (s_phase == kSnakeFailed) fg = c_warn();
    Ui_DrawTextAtBg(8, y2, line2, fg, c_bg());
    s_status_dirty = false;
}

static void draw_static_field(void)
{
    St7789_FillRect(0, UI_HEADER_H, St7789_Width(),
                    St7789_Height() - UI_HEADER_H - UI_FOOTER_H, c_bg());
    St7789_FillRect(s_grid_x - 2, s_grid_y - 2, s_grid_w + 4, s_grid_h + 4, c_grid());
    St7789_FillRect(s_grid_x, s_grid_y, s_grid_w, s_grid_h, c_field());
}

static void draw_full_scene(void)
{
    Ui_LcdLock();
    Ui_BeginBatch();
    draw_static_field();
    for (int i = 0; i < FOOD_ON_BOARD; i++) draw_food_item(i);
    for (int i = s_len - 1; i >= 1; i--) fill_cell(s_snake[i], c_snake_body());
    fill_cell(s_snake[0], c_snake_head());
    draw_head_value(s_snake[0], s_head_true, c_black());
    draw_status();
    Ui_EndBatch();
    Ui_LcdUnlock();
}

static MoveInfo step_snake(void)
{
    MoveInfo m = (MoveInfo){0};
    if (s_phase != kSnakeRun) return m;

    m.valid = true;
    m.ate_idx = -1;
    m.old_head = s_snake[0];
    m.old_tail = s_snake[s_len - 1];

    Cell nh = next_head(s_snake[0]);
    if (nh.x < 0 || nh.x >= GRID_COLS || nh.y < 0 || nh.y >= GRID_ROWS) {
        s_phase = kSnakeFailed;
        Sfx_PlayFailure();
        return m;
    }
    if (snake_has_cell(nh)) {
        s_phase = kSnakeFailed;
        Sfx_PlayFailure();
        return m;
    }
    m.new_head = nh;

    int ate_idx = -1;
    for (int i = 0; i < FOOD_ON_BOARD; i++) {
        if (s_foods[i].active && cell_equals(nh, s_foods[i].c)) {
            ate_idx = i;
            break;
        }
    }
    m.ate = (ate_idx >= 0);
    m.ate_idx = ate_idx;

    for (int i = s_len; i > 0; i--) s_snake[i] = s_snake[i - 1];
    s_snake[0] = nh;
    s_len++;

    bool remove_tail = true;
    if (m.ate) {
        Sfx_PlayNotify();
        int old_head = s_head_true;
        int delta = s_foods[ate_idx].v;
        int new_head = clamp_10_99(old_head + delta);
        s_last_eq_lhs = old_head;
        s_last_eq_delta = delta;
        s_last_eq_valid = true;
        s_head_true = new_head;
        s_eaten_in_level++;
        s_grow++;
        remove_tail = false;

        if (s_eaten_in_level >= FOODS_PER_LEVEL) {
            s_foods[ate_idx].active = false;
            if (s_level >= LEVEL_COUNT) {
                s_phase = kSnakeAllClear;
                Sfx_PlaySuccess();
            } else {
                s_phase = kSnakeLevelClear;
            }
            s_status_dirty = true;
        } else {
            spawn_one_food(ate_idx);
            s_phase = kSnakeInputGuess;
            s_guess_value = s_head_true;
            s_pick_tens = (s_guess_value / 10) - 1;
            if (s_pick_tens < 0) s_pick_tens = 0;
            if (s_pick_tens > 8) s_pick_tens = 8;
            s_pick_ones = s_guess_value % 10;
            s_pick_row = 0;
            s_status_dirty = true;
        }
    }

    if (s_grow > 0) {
        s_grow--;
        remove_tail = false;
    }
    if (remove_tail) {
        s_len--;
        m.removed_tail = true;
    } else {
        m.removed_tail = false;
    }
    if (s_len < 1) s_len = 1;
    return m;
}

static void draw_incremental(const MoveInfo* m)
{
    Ui_LcdLock();
    Ui_BeginBatch();

    if (m && m->valid) {
        if (m->removed_tail) fill_cell(m->old_tail, c_field());
        fill_cell(m->old_head, c_snake_body());
        fill_cell(m->new_head, c_snake_head());
        draw_head_value(m->new_head, s_head_true, c_black());
        if (m->ate && m->ate_idx >= 0 && s_foods[m->ate_idx].active) draw_food_item(m->ate_idx);
    }

    if (s_status_dirty || s_phase != kSnakeRun || (s_feedback_until_ms > 0 && now_ms() <= s_feedback_until_ms)) {
        draw_status();
    }
    Ui_EndBatch();
    Ui_LcdUnlock();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("SNAKE", "OK:START  BACK");
    Ui_Println("9 levels, 30 foods each.");
    Ui_Println("Lv1-3: + and <3");
    Ui_Println("Lv4-6: + and <9");
    Ui_Println("Lv7-9: +/- and <9, init=30");
    Ui_Println("After eat, guess 1..99.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    int w = St7789_Width();
    int h = St7789_Height();
    s_grid_x = (w - s_grid_w) / 2;
    s_grid_y = UI_HEADER_H + 48;
    int max_y = h - UI_FOOTER_H - s_grid_h - 2;
    if (s_grid_y > max_y) s_grid_y = max_y;
    setup_level(1);
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    Ui_DrawFrame("SNAKE", "RUN:UP/L DN/R  INPUT:UP/DN OK");
    setup_level(1);
    draw_full_scene();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;

    if (key == kInputEnter) {
        if (s_phase == kSnakeFailed) {
            setup_level(s_level);
            draw_full_scene();
        } else if (s_phase == kSnakeLevelClear) {
            setup_level(s_level + 1);
            draw_full_scene();
        } else if (s_phase == kSnakeAllClear) {
            setup_level(1);
            draw_full_scene();
        } else if (s_phase == kSnakeInputGuess) {
            s_last_ok = (s_guess_value == s_head_true);
            s_feedback_until_ms = now_ms() + FEEDBACK_SHOW_MS;
            s_phase = kSnakeRun;
            s_status_dirty = true;
            draw_incremental(NULL);
        }
        return;
    }

    if (s_phase == kSnakeInputGuess) {
        if (key == kInputUp) {
            if (s_pick_row == 0) {
                s_pick_tens = (s_pick_tens + 8) % 9;
            } else {
                s_pick_ones = (s_pick_ones + 9) % 10;
            }
        } else if (key == kInputDown) {
            if (s_pick_row == 0) {
                s_pick_tens = (s_pick_tens + 1) % 9;
            } else {
                s_pick_ones = (s_pick_ones + 1) % 10;
            }
        } else if (key == kInputBack) {
            s_pick_row = 1 - s_pick_row;
        }
        s_guess_value = (s_pick_tens + 1) * 10 + s_pick_ones;
        s_status_dirty = true;
        draw_incremental(NULL);
        return;
    }

    if (s_phase != kSnakeRun) return;
    if (key == kInputUp) rotate_left();
    else if (key == kInputDown) rotate_right();
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (s_feedback_until_ms > 0 && now_ms() > s_feedback_until_ms) {
        s_feedback_until_ms = 0;
        s_status_dirty = true;
    }
    uint32_t t = now_ms();
    if (t < s_next_tick_ms) return;
    s_next_tick_ms = t + STEP_MS;

    if (s_phase == kSnakeRun) {
        MoveInfo m = step_snake();
        draw_incremental(&m);
    } else {
        draw_incremental(NULL);
    }
}

const Experiment g_exp_snake = {
    .id = 20,
    .title = "SNAKE",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
