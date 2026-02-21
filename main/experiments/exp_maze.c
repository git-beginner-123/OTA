#include <stdint.h>
#include <stdbool.h>

#include "display/st7789.h"
#include "ui/ui.h"
#include "experiments/experiment.h"
#include "audio/sfx.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

// -----------------------------
// Input mapping:
// U/D/L/R : select facing direction
// OK      : move forward one tile
// -----------------------------
#ifndef MAZE_KEY_FORWARD
#define MAZE_KEY_FORWARD kInputEnter
#endif

static inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b)
{
    return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}

#define TILE_W 14
#define TILE_H 14
#define HUD_H 40

#define MAP_W 15
#define MAP_H 20

static const uint8_t kMap[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,1,0,0,0,0,0,0,0,1},
    {1,0,1,1,1,0,1,0,1,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,0,0,0,1,0,1},
    {1,0,1,0,1,1,1,1,1,1,1,0,1,0,1},
    {1,0,0,0,1,0,0,0,0,0,1,0,0,0,1},
    {1,1,1,0,1,0,1,1,1,0,1,1,1,0,1},
    {1,0,0,0,0,0,1,0,0,0,0,0,1,0,1},
    {1,0,1,1,1,0,1,0,1,1,1,0,1,0,1},
    {1,0,0,0,1,0,0,0,1,0,0,0,0,0,1},
    {1,1,1,0,1,1,1,0,1,0,1,1,1,0,1},
    {1,0,0,0,0,0,0,0,1,0,0,0,1,0,1},
    {1,0,1,1,1,1,1,0,1,1,1,0,1,0,1},
    {1,0,0,0,0,0,1,0,0,0,1,0,0,0,1},
    {1,1,1,1,1,0,1,1,1,0,1,1,1,0,1},
    {1,0,0,0,1,0,0,0,1,0,0,0,0,0,1},
    {1,0,1,0,1,1,1,0,1,1,1,1,1,0,1},
    {1,0,1,0,0,0,0,0,0,0,0,0,1,0,1},
    {1,0,0,0,1,0,1,1,1,1,1,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
};

// -----------------------------
// State
// -----------------------------
static bool s_running = false;
static bool s_won = false;

static int s_px = 1;
static int s_py = 1;
static int s_dir = 1; // 0=up,1=right,2=down,3=left

static int s_prev_px = 1;
static int s_prev_py = 1;

static int s_ox = 0;
static int s_oy = 0;
static int s_steps = 0;
static uint32_t s_started_ms = 0;
static uint32_t s_elapsed_s = 0;

static bool s_dirty = false;     // need redraw dirty tiles
static bool s_full_dirty = false; // need redraw full map
static bool s_hud_dirty = false;

static uint16_t s_tilebuf[TILE_W * TILE_H];

typedef struct {
    uint8_t x;
    uint8_t y;
    bool taken;
} Coin;

static const Coin kCoinsInit[] = {
    { 3,  1, false },
    { 13, 3, false },
    { 1, 11, false },
    { 13, 14, false },
    { 7, 17, false },
};

#define COIN_COUNT ((int)(sizeof(kCoinsInit) / sizeof(kCoinsInit[0])))
static Coin s_coins[COIN_COUNT];
static int s_coin_taken = 0;

static const int kExitX = 13;
static const int kExitY = 18;

static uint32_t now_ms(void)
{
    return (uint32_t)(xTaskGetTickCount() * (1000 / configTICK_RATE_HZ));
}

static int find_coin(int x, int y)
{
    for (int i = 0; i < COIN_COUNT; i++) {
        if (!s_coins[i].taken && s_coins[i].x == x && s_coins[i].y == y) return i;
    }
    return -1;
}

// -----------------------------
// Tile drawing
// -----------------------------
static void tile_fill(uint16_t c)
{
    for (int i = 0; i < TILE_W * TILE_H; i++) s_tilebuf[i] = c;
}

static void tile_draw_wall(void)
{
    uint16_t bg = rgb565(10, 10, 20);
    uint16_t fg = rgb565(40, 160, 140);

    tile_fill(bg);

    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            if (y == 0 || y == TILE_H - 1 || x == 0 || x == TILE_W - 1) {
                s_tilebuf[y * TILE_W + x] = fg;
            } else if ((y % 4) == 0) {
                s_tilebuf[y * TILE_W + x] = fg;
            } else if ((x % 6) == 0 && ((y / 4) % 2 == 0)) {
                s_tilebuf[y * TILE_W + x] = fg;
            }
        }
    }
}

static void tile_draw_floor(void)
{
    uint16_t c0 = rgb565(8, 12, 18);
    uint16_t c1 = rgb565(10, 16, 26);

    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            bool chk = (((x >> 2) + (y >> 2)) & 1) != 0;
            s_tilebuf[y * TILE_W + x] = chk ? c0 : c1;
        }
    }
}

static void tile_draw_player_overlay(void)
{
    uint16_t p = rgb565(255, 255, 255);
    uint16_t a = rgb565(255, 220, 80);
    uint16_t d = rgb565(255, 120, 60);

    int cx = TILE_W / 2;
    int cy = TILE_H / 2;

    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 <= 18) s_tilebuf[y * TILE_W + x] = a;
            if (d2 <= 8)  s_tilebuf[y * TILE_W + x] = p;
        }
    }

    // Direction marker to make controls intuitive.
    int dx = 0, dy = 0;
    if (s_dir == 0) dy = -4;
    else if (s_dir == 1) dx = 4;
    else if (s_dir == 2) dy = 4;
    else dx = -4;

    for (int i = 0; i < 3; i++) {
        int px = cx + dx - (dx != 0 ? (dx > 0 ? i : -i) : 0);
        int py = cy + dy - (dy != 0 ? (dy > 0 ? i : -i) : 0);
        if (px >= 0 && py >= 0 && px < TILE_W && py < TILE_H) {
            s_tilebuf[py * TILE_W + px] = d;
        }
    }
}

static void tile_draw_coin_overlay(void)
{
    uint16_t c0 = rgb565(255, 220, 40);
    uint16_t c1 = rgb565(255, 255, 120);
    int cx = TILE_W / 2;
    int cy = TILE_H / 2;
    for (int y = 0; y < TILE_H; y++) {
        for (int x = 0; x < TILE_W; x++) {
            int dx = x - cx;
            int dy = y - cy;
            int d2 = dx * dx + dy * dy;
            if (d2 <= 10) s_tilebuf[y * TILE_W + x] = c0;
            if (d2 <= 3) s_tilebuf[y * TILE_W + x] = c1;
        }
    }
}

static void tile_draw_exit_overlay(void)
{
    uint16_t c = rgb565(120, 255, 140);
    for (int i = 3; i < TILE_W - 3; i++) {
        s_tilebuf[3 * TILE_W + i] = c;
        s_tilebuf[(TILE_H - 4) * TILE_W + i] = c;
        s_tilebuf[i * TILE_W + 3] = c;
        s_tilebuf[i * TILE_W + (TILE_W - 4)] = c;
    }
}

static int map_origin_x(void)
{
    int sw = St7789_Width();
    int mw = MAP_W * TILE_W;
    if (mw >= sw) return 0;
    return (sw - mw) / 2;
}

static int map_origin_y(void)
{
    int sh = St7789_Height();
    int mh = MAP_H * TILE_H;
    int play_h = sh - HUD_H;
    if (mh >= play_h) return HUD_H;
    return HUD_H + (play_h - mh) / 2;
}

static void draw_tile(int mx, int my, bool player_here)
{
    if (kMap[my][mx] == 1) tile_draw_wall();
    else {
        tile_draw_floor();
        if (mx == kExitX && my == kExitY) tile_draw_exit_overlay();
        if (find_coin(mx, my) >= 0) tile_draw_coin_overlay();
        if (player_here) tile_draw_player_overlay();
    }

    int sx = s_ox + mx * TILE_W;
    int sy = s_oy + my * TILE_H;
    St7789_BlitRect(sx, sy, TILE_W, TILE_H, s_tilebuf);
}

static void redraw_full(void)
{
    for (int y = 0; y < MAP_H; y++) {
        for (int x = 0; x < MAP_W; x++) {
            bool p = (x == s_px && y == s_py);
            draw_tile(x, y, p);
        }
    }
    St7789_Flush();
}

static void redraw_dirty(void)
{
    // redraw old player tile and new player tile
    draw_tile(s_prev_px, s_prev_py, false);
    draw_tile(s_px, s_py, true);
    St7789_Flush();
}

// -----------------------------
// Movement (only update state, drawing happens in tick)
// -----------------------------
static void try_forward(void)
{
    int dx = 0, dy = 0;
    if (s_dir == 0) dy = -1;
    else if (s_dir == 1) dx = 1;
    else if (s_dir == 2) dy = 1;
    else dx = -1;

    int nx = s_px + dx;
    int ny = s_py + dy;

    if (nx < 0 || nx >= MAP_W || ny < 0 || ny >= MAP_H) return;
    if (kMap[ny][nx] != 0) return;

    s_prev_px = s_px;
    s_prev_py = s_py;

    s_px = nx;
    s_py = ny;
    s_steps++;

    int coin = find_coin(s_px, s_py);
    if (coin >= 0) {
        s_coins[coin].taken = true;
        s_coin_taken++;
        Sfx_PlayNotify();
    }

    if (s_px == kExitX && s_py == kExitY && s_coin_taken == COIN_COUNT) {
        s_won = true;
        Sfx_PlayVictory();
    }

    s_dirty = true;
    s_hud_dirty = true;
}

static void set_dir(int dir)
{
    if (dir < 0 || dir > 3) return;
    if (s_dir == dir) return;
    s_dir = dir;
    s_dirty = true;
    s_hud_dirty = true;
}

static void game_reset(void)
{
    s_px = 1;
    s_py = 1;
    s_prev_px = s_px;
    s_prev_py = s_py;
    s_dir = 1;
    s_steps = 0;
    s_started_ms = now_ms();
    s_elapsed_s = 0;
    s_won = false;
    s_coin_taken = 0;
    for (int i = 0; i < COIN_COUNT; i++) s_coins[i] = kCoinsInit[i];
    s_full_dirty = true;
    s_dirty = false;
    s_hud_dirty = true;
}

static void draw_hud(void)
{
    char l1[48];
    char l2[56];
    uint16_t bg = rgb565(0, 0, 0);
    uint16_t fg = rgb565(255, 230, 120);
    St7789_FillRect(0, 0, St7789_Width(), HUD_H, bg);

    snprintf(l1, sizeof(l1), "Coins %d/%d  Step %d  Time %lus",
             s_coin_taken, COIN_COUNT, s_steps, (unsigned long)s_elapsed_s);
    Ui_DrawTextAtBg(6, 4, l1, fg, bg);

    if (s_won) {
        Ui_DrawTextAtBg(6, 22, "WIN! ENTER=RESTART  BACK=EXIT", rgb565(120, 255, 160), bg);
    } else {
        snprintf(l2, sizeof(l2), "UDLR select-dir  OK move");
        Ui_DrawTextAtBg(6, 22, l2, fg, bg);
    }
}

// -----------------------------
// Experiment callbacks
// -----------------------------
static void Maze_OnEnter(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;
}

static void Maze_OnExit(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;
}

static void Maze_ShowRequirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("MAZE", "OK:START  BACK");
    Ui_Println("MAZE ADVENTURE");
    Ui_Println("Collect all coins first.");
    Ui_Println("Then go to GREEN exit.");
    Ui_Println("UDLR: select direction");
    Ui_Println("OK: move / restart");
}

static void Maze_Start(ExperimentContext* ctx)
{
    (void)ctx;

    s_running = true;

    s_ox = map_origin_x();
    s_oy = map_origin_y();

    game_reset();
}

static void Maze_Stop(ExperimentContext* ctx)
{
    (void)ctx;
    s_running = false;
}

static void Maze_OnKey(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    if (s_won && key == MAZE_KEY_FORWARD) { game_reset(); return; }
    if (s_won) return;

    if (key == kInputUp) { set_dir(0); return; }
    if (key == kInputRight) { set_dir(1); return; }
    if (key == kInputDown) { set_dir(2); return; }
    if (key == kInputLeft) { set_dir(3); return; }
    if (key == MAZE_KEY_FORWARD) { try_forward(); return; }
}

static void Maze_Tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running) return;

    if (s_full_dirty) {
        // Clear and draw the maze fully
        St7789_Fill(rgb565(0, 0, 0));
        St7789_Flush();
        redraw_full();
        draw_hud();
        s_full_dirty = false;
        s_hud_dirty = false;
        return;
    }

    if (s_dirty) {
        redraw_dirty();
        s_dirty = false;
        s_hud_dirty = true;
    }

    uint32_t sec = (now_ms() - s_started_ms) / 1000U;
    if (sec != s_elapsed_s) {
        s_elapsed_s = sec;
        s_hud_dirty = true;
    }

    if (s_hud_dirty) {
        draw_hud();
        s_hud_dirty = false;
    }
}

const Experiment g_exp_maze = {
    .id = 12,
    .title = "MAZE",

    .on_enter = Maze_OnEnter,
    .on_exit = Maze_OnExit,

    .show_requirements = Maze_ShowRequirements,

    .start = Maze_Start,
    .stop = Maze_Stop,

    .on_key = Maze_OnKey,
    .tick = Maze_Tick,
};
