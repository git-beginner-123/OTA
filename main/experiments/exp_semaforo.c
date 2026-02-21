#include "experiments/experiment.h"
#include "ui/ui.h"

#include <stdio.h>

#include "display/st7789.h"
#include "esp_timer.h"
#include "driver/gpio.h"

typedef enum {
    kLightRed = 0,
    kLightYellow,
    kLightGreen
} LightState;

typedef struct {
    LightState ns;
    LightState ew;
    int remain_s;
} PhaseState;

static int s_phase = 0;
static int s_remain = 0;
static int s_last_phase = -1;
static int s_last_remain = -1;
static uint32_t s_next_ms = 0;

static uint32_t now_ms(void)
{
    return (uint32_t)(esp_timer_get_time() / 1000ULL);
}

static uint16_t color_off(void)   { return Ui_ColorRGB(40, 40, 40); }
static uint16_t color_red(void)   { return Ui_ColorRGB(220, 40, 40); }
static uint16_t color_yel(void)   { return Ui_ColorRGB(230, 200, 40); }
static uint16_t color_grn(void)   { return Ui_ColorRGB(40, 200, 60); }
static uint16_t color_road(void)  { return Ui_ColorRGB(30, 30, 35); }
static uint16_t color_text(void)  { return Ui_ColorRGB(230, 230, 230); }
static uint16_t color_bg(void)    { return Ui_ColorRGB(8, 14, 20); }

static void apply_display_profile(void)
{
    St7789_ApplyPanelDefaultProfile();
}

// Physical traffic lights
// NS (top/bottom): RED,YEL,GRN = GPIO17,GPIO38,GPIO39
// EW (left/right): RED,YEL,GRN = GPIO2,GPIO1,GPIO37
#define NS_RED_GPIO  GPIO_NUM_17
#define NS_YEL_GPIO  GPIO_NUM_38
#define NS_GRN_GPIO  GPIO_NUM_39
#define EW_RED_GPIO  GPIO_NUM_2
#define EW_YEL_GPIO  GPIO_NUM_1
#define EW_GRN_GPIO  GPIO_NUM_37

typedef struct {
    int body_x;
    int body_y;
    int body_w;
    int body_h;
    int nx, ny;
    int sx, sy;
    int wx, wy;
    int ex, ey;
    int hbox_w, hbox_h;
    int vbox_w, vbox_h;
} SemaLayout;

static SemaLayout s_layout = {0};

static void draw_light_box(int x, int y, LightState state)
{
    int lamp = 14;
    int gap = 4;
    int box_pad = 4;
    int box_w = lamp + box_pad * 2;
    int box_h = (lamp * 3) + (gap * 2) + box_pad * 2;

    St7789_FillRect(x, y, box_w, box_h, Ui_ColorRGB(15, 15, 20));
    // simple border
    St7789_FillRect(x, y, box_w, 1, Ui_ColorRGB(80, 80, 90));
    St7789_FillRect(x, y + box_h - 1, box_w, 1, Ui_ColorRGB(80, 80, 90));
    St7789_FillRect(x, y, 1, box_h, Ui_ColorRGB(80, 80, 90));
    St7789_FillRect(x + box_w - 1, y, 1, box_h, Ui_ColorRGB(80, 80, 90));

    int lx = x + box_pad;
    int ly = y + box_pad;
    St7789_FillRect(lx, ly, lamp, lamp, state == kLightRed ? color_red() : color_off());
    St7789_FillRect(lx, ly + lamp + gap, lamp, lamp, state == kLightYellow ? color_yel() : color_off());
    St7789_FillRect(lx, ly + (lamp + gap) * 2, lamp, lamp, state == kLightGreen ? color_grn() : color_off());
}

static void draw_light_box_h(int x, int y, LightState state)
{
    int lamp = 14;
    int gap = 4;
    int box_pad = 4;
    int box_w = (lamp * 3) + (gap * 2) + box_pad * 2;
    int box_h = lamp + box_pad * 2;

    St7789_FillRect(x, y, box_w, box_h, Ui_ColorRGB(15, 15, 20));
    // simple border
    St7789_FillRect(x, y, box_w, 1, Ui_ColorRGB(80, 80, 90));
    St7789_FillRect(x, y + box_h - 1, box_w, 1, Ui_ColorRGB(80, 80, 90));
    St7789_FillRect(x, y, 1, box_h, Ui_ColorRGB(80, 80, 90));
    St7789_FillRect(x + box_w - 1, y, 1, box_h, Ui_ColorRGB(80, 80, 90));

    int lx = x + box_pad;
    int ly = y + box_pad;
    St7789_FillRect(lx, ly, lamp, lamp, state == kLightRed ? color_red() : color_off());
    St7789_FillRect(lx + lamp + gap, ly, lamp, lamp, state == kLightYellow ? color_yel() : color_off());
    St7789_FillRect(lx + (lamp + gap) * 2, ly, lamp, lamp, state == kLightGreen ? color_grn() : color_off());
}

static PhaseState phase_state(int phase, int remain)
{
    PhaseState s;
    s.remain_s = remain;
    switch (phase) {
        case 0: s.ns = kLightGreen;  s.ew = kLightRed;   break;
        case 1: s.ns = kLightYellow; s.ew = kLightRed;   break;
        case 2: s.ns = kLightRed;    s.ew = kLightGreen; break;
        case 3: s.ns = kLightRed;    s.ew = kLightYellow;break;
        default: s.ns = kLightRed;   s.ew = kLightRed;   break;
    }
    return s;
}

static uint16_t light_color(LightState s)
{
    if (s == kLightRed) return color_red();
    if (s == kLightYellow) return color_yel();
    return color_grn();
}

static void sema_hw_init(void)
{
    gpio_config_t io = {0};
    io.intr_type = GPIO_INTR_DISABLE;
    io.mode = GPIO_MODE_OUTPUT;
    io.pull_down_en = 0;
    io.pull_up_en = 0;
    io.pin_bit_mask =
        (1ULL << NS_RED_GPIO) |
        (1ULL << NS_YEL_GPIO) |
        (1ULL << NS_GRN_GPIO) |
        (1ULL << EW_RED_GPIO) |
        (1ULL << EW_YEL_GPIO) |
        (1ULL << EW_GRN_GPIO);
    gpio_config(&io);
}

static void sema_hw_all_off(void)
{
    gpio_set_level(NS_RED_GPIO, 0);
    gpio_set_level(NS_YEL_GPIO, 0);
    gpio_set_level(NS_GRN_GPIO, 0);
    gpio_set_level(EW_RED_GPIO, 0);
    gpio_set_level(EW_YEL_GPIO, 0);
    gpio_set_level(EW_GRN_GPIO, 0);
}

static void sema_hw_apply(const PhaseState* st)
{
    gpio_set_level(NS_RED_GPIO, st->ns == kLightRed);
    gpio_set_level(NS_YEL_GPIO, st->ns == kLightYellow);
    gpio_set_level(NS_GRN_GPIO, st->ns == kLightGreen);

    gpio_set_level(EW_RED_GPIO, st->ew == kLightRed);
    gpio_set_level(EW_YEL_GPIO, st->ew == kLightYellow);
    gpio_set_level(EW_GRN_GPIO, st->ew == kLightGreen);
}

static void setup_layout(void)
{
    int w = St7789_Width();
    int h = St7789_Height();

    s_layout.body_y = 30;
    s_layout.body_h = h - 30 - 26;
    s_layout.body_x = 0;
    s_layout.body_w = w;

    int cx = w / 2;
    int cy = s_layout.body_y + s_layout.body_h / 2;
    int road_w = 60;
    int road_h = 60;

    // Vertical light size (west/east)
    int lamp = 14;
    int gap = 4;
    int box_pad = 4;
    s_layout.vbox_w = lamp + box_pad * 2;
    s_layout.vbox_h = (lamp * 3) + (gap * 2) + box_pad * 2;
    // Horizontal light size (north/south)
    s_layout.hbox_w = (lamp * 3) + (gap * 2) + box_pad * 2;
    s_layout.hbox_h = lamp + box_pad * 2;

    // North (top)
    s_layout.nx = cx - s_layout.hbox_w / 2;
    s_layout.ny = s_layout.body_y + 6;

    // South (bottom)
    s_layout.sx = cx - s_layout.hbox_w / 2;
    s_layout.sy = s_layout.body_y + s_layout.body_h - s_layout.hbox_h - 6;

    // West (left)
    s_layout.wx = s_layout.body_x + 6;
    s_layout.wy = cy - s_layout.vbox_h / 2;

    // East (right)
    s_layout.ex = s_layout.body_x + s_layout.body_w - s_layout.vbox_w - 6;
    s_layout.ey = cy - s_layout.vbox_h / 2;
}

static void draw_scene_static(void)
{
    int cx = St7789_Width() / 2;
    int cy = s_layout.body_y + s_layout.body_h / 2;
    int road_w = 60;
    int road_h = 60;

    // clear body once
    St7789_FillRect(s_layout.body_x, s_layout.body_y, s_layout.body_w, s_layout.body_h, color_bg());
    // road square
    St7789_FillRect(cx - road_w / 2, cy - road_h / 2, road_w, road_h, color_road());
    // cross roads
    St7789_FillRect(cx - 12, s_layout.body_y + 6, 24, s_layout.body_h - 12, color_road());
    St7789_FillRect(s_layout.body_x + 6, cy - 12, s_layout.body_w - 12, 24, color_road());
}

static void draw_dynamic(const PhaseState* st)
{
    draw_light_box_h(s_layout.nx, s_layout.ny, st->ns);
    draw_light_box_h(s_layout.sx, s_layout.sy, st->ns);
    draw_light_box(s_layout.wx, s_layout.wy, st->ew);
    draw_light_box(s_layout.ex, s_layout.ey, st->ew);

    // Time labels near each side
    char tbuf[12];
    snprintf(tbuf, sizeof(tbuf), "%2ds", st->remain_s);

    // Clear previous time text areas to avoid artifacts, keep static scene untouched.
    St7789_FillRect(s_layout.nx + s_layout.hbox_w + 4, s_layout.ny + s_layout.hbox_h / 2 - 8, 42, 16, color_bg());
    St7789_FillRect(s_layout.sx + s_layout.hbox_w + 4, s_layout.sy + s_layout.hbox_h / 2 - 8, 42, 16, color_bg());
    St7789_FillRect(s_layout.wx + s_layout.vbox_w + 4, s_layout.wy + s_layout.vbox_h / 2 - 8, 42, 16, color_bg());
    St7789_FillRect(s_layout.ex - 30, s_layout.ey + s_layout.vbox_h / 2 - 8, 42, 16, color_bg());

    // North time (right of box)
    Ui_DrawTextAtBg(s_layout.nx + s_layout.hbox_w + 4, s_layout.ny + s_layout.hbox_h / 2 - 8, tbuf, color_text(), color_bg());
    // South time (right of box)
    Ui_DrawTextAtBg(s_layout.sx + s_layout.hbox_w + 4, s_layout.sy + s_layout.hbox_h / 2 - 8, tbuf, color_text(), color_bg());
    // West time (left of box)
    Ui_DrawTextAtBg(s_layout.wx + s_layout.vbox_w + 4, s_layout.wy + s_layout.vbox_h / 2 - 8, tbuf, color_text(), color_bg());
    // East time (right of box)
    Ui_DrawTextAtBg(s_layout.ex - 30, s_layout.ey + s_layout.vbox_h / 2 - 8, tbuf, color_text(), color_bg());

    St7789_Flush();
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    apply_display_profile();
    Ui_DrawFrame("SEMAFORO", "OK:START  BACK");
    Ui_Println("Goal: traffic-light simulation.");
    Ui_Println("NS and EW run opposite.");
    Ui_Println("Green 20s, Yellow 3s.");
    Ui_Println("Auto cycle every second.");
    Ui_Println("Observe timing + sync.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    apply_display_profile();
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    // Restore default display profile explicitly.
    apply_display_profile();
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    apply_display_profile();
    s_phase = 0;
    s_remain = 20;
    s_last_phase = -1;
    s_last_remain = -1;
    s_next_ms = 0;

    Ui_DrawFrame("SEMAFORO", "BACK=RET");
    Ui_DrawBodyClear();

    setup_layout();
    draw_scene_static();
    PhaseState st = phase_state(s_phase, s_remain);
    sema_hw_init();
    sema_hw_apply(&st);
    draw_dynamic(&st);
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    sema_hw_all_off();
    apply_display_profile();
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;

    uint32_t t = now_ms();
    if (t < s_next_ms) return;
    s_next_ms = t + 1000;

    if (s_remain <= 0) {
        s_phase = (s_phase + 1) % 4;
        s_remain = (s_phase == 1 || s_phase == 3) ? 3 : 20;
    } else {
        s_remain--;
    }

    if (s_phase != s_last_phase || s_remain != s_last_remain) {
        PhaseState st = phase_state(s_phase, s_remain);
        sema_hw_apply(&st);
        draw_dynamic(&st);
        s_last_phase = s_phase;
        s_last_remain = s_remain;
    }
}

const Experiment g_exp_semaforo = {
    .id = 11,
    .title = "SEMAFORO",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = 0,
    .tick = tick,
};
