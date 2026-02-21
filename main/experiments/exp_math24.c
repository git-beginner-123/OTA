#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/sfx.h"
#include "display/st7789.h"

#include "esp_log.h"
#include "esp_random.h"

#include <stdbool.h>
#include <stdio.h>
#include <math.h>

static const char* TAG = "EXP_24";

typedef enum {
    kOpAdd = 0,
    kOpSub,
    kOpMul,
    kOpDiv,
} OpType;

typedef enum {
    kSelOp1 = 0,
    kSelOp2,
    kSelOp3,
    kSelPattern,
    kSelCheck,
} SelType;

static int s_nums[4] = { 6, 1, 3, 4 };
static int s_suits[4] = { 0, 1, 2, 3 }; // visual only: 0H 1D 2S 3C
static int s_ops[3] = { 0, 0, 0 };
static int s_pattern = 0;
static int s_sel = 0;
static int s_total = 0;
static int s_correct = 0;
static int s_score = 0;
static char s_msg[40] = "Build expr to make 24.";
static bool s_frame_inited = false;

static const char* suit_text(int suit)
{
    static const char* kSuit[] = { "H", "D", "S", "C" };
    if (suit < 0 || suit > 3) return "?";
    return kSuit[suit];
}

static uint16_t suit_color(int suit)
{
    if (suit == 0 || suit == 1) return Ui_ColorRGB(240, 80, 80);   // red
    return Ui_ColorRGB(220, 220, 220);                               // white
}

static void draw_card(int x, int y, int w, int h, int num, int suit)
{
    uint16_t card_bg = Ui_ColorRGB(250, 250, 250);
    uint16_t border = Ui_ColorRGB(40, 40, 50);
    uint16_t dark = Ui_ColorRGB(30, 30, 30);
    uint16_t sc = suit_color(suit);
    char nbuf[8];

    St7789_FillRect(x, y, w, h, card_bg);
    St7789_FillRect(x, y, w, 2, border);
    St7789_FillRect(x, y + h - 2, w, 2, border);
    St7789_FillRect(x, y, 2, h, border);
    St7789_FillRect(x + w - 2, y, 2, h, border);

    snprintf(nbuf, sizeof(nbuf), "%d", num);
    Ui_DrawTextAtBg(x + 6, y + 6, nbuf, dark, card_bg);
    Ui_DrawTextAtBg(x + w - 16, y + h - 18, nbuf, dark, card_bg);
    Ui_DrawTextAtBg(x + 6, y + h - 18, suit_text(suit), sc, card_bg);
    Ui_DrawTextAtBg(x + w - 16, y + 6, suit_text(suit), sc, card_bg);

    // Center rank
    if (num >= 10) Ui_DrawTextAtBg(x + w / 2 - 8, y + h / 2 - 8, nbuf, dark, card_bg);
    else Ui_DrawTextAtBg(x + w / 2 - 4, y + h / 2 - 8, nbuf, dark, card_bg);
}

static void draw_cards_panel(void)
{
    int cw = 54, ch = 66;
    int gap_x = 16;
    int gap_y = 10;
    int panel_x = 8;
    int panel_y = 34;
    int panel_w = St7789_Width() - 16;
    int x1 = panel_x + (panel_w - (2 * cw + gap_x)) / 2;
    int x2 = x1 + cw + gap_x;
    int y1 = 42, y2 = 42 + ch + gap_y;
    uint16_t bg = Ui_ColorRGB(18, 34, 98);

    St7789_FillRect(panel_x, panel_y, panel_w, 160, bg);
    draw_card(x1, y1, cw, ch, s_nums[0], s_suits[0]);
    draw_card(x2, y1, cw, ch, s_nums[1], s_suits[1]);
    draw_card(x1, y2, cw, ch, s_nums[2], s_suits[2]);
    draw_card(x2, y2, cw, ch, s_nums[3], s_suits[3]);
}

static char op_char(int op)
{
    // Use 'x' instead of '*' for LCD readability.
    static const char kOps[] = { '+', '-', 'x', '/' };
    if (op < 0 || op > 3) return '?';
    return kOps[op];
}

static const char* sel_name(int sel)
{
    switch (sel) {
        case kSelOp1: return "OP1";
        case kSelOp2: return "OP2";
        case kSelOp3: return "OP3";
        case kSelPattern: return "PATT";
        case kSelCheck: return "CHECK";
        default: return "?";
    }
}

static bool apply_op(double a, double b, int op, double* out)
{
    if (!out) return false;
    if (op == kOpAdd) { *out = a + b; return true; }
    if (op == kOpSub) { *out = a - b; return true; }
    if (op == kOpMul) { *out = a * b; return true; }
    if (op == kOpDiv) {
        if (fabs(b) < 1e-9) return false;
        *out = a / b;
        return true;
    }
    return false;
}

static bool eval_expr(const int nums[4], const int ops[3], int pat, double* out)
{
    double a = (double)nums[0], b = (double)nums[1], c = (double)nums[2], d = (double)nums[3];
    double x = 0, y = 0;

    switch (pat) {
        case 0: // ((a o1 b) o2 c) o3 d
            if (!apply_op(a, b, ops[0], &x)) return false;
            if (!apply_op(x, c, ops[1], &y)) return false;
            return apply_op(y, d, ops[2], out);
        case 1: // (a o1 (b o2 c)) o3 d
            if (!apply_op(b, c, ops[1], &x)) return false;
            if (!apply_op(a, x, ops[0], &y)) return false;
            return apply_op(y, d, ops[2], out);
        case 2: // a o1 ((b o2 c) o3 d)
            if (!apply_op(b, c, ops[1], &x)) return false;
            if (!apply_op(x, d, ops[2], &y)) return false;
            return apply_op(a, y, ops[0], out);
        case 3: // a o1 (b o2 (c o3 d))
            if (!apply_op(c, d, ops[2], &x)) return false;
            if (!apply_op(b, x, ops[1], &y)) return false;
            return apply_op(a, y, ops[0], out);
        case 4: // (a o1 b) o2 (c o3 d)
            if (!apply_op(a, b, ops[0], &x)) return false;
            if (!apply_op(c, d, ops[2], &y)) return false;
            return apply_op(x, y, ops[1], out);
        default:
            return false;
    }
}

static bool puzzle_has_solution(const int nums[4])
{
    int ops[3];
    for (ops[0] = 0; ops[0] < 4; ops[0]++) {
        for (ops[1] = 0; ops[1] < 4; ops[1]++) {
            for (ops[2] = 0; ops[2] < 4; ops[2]++) {
                for (int p = 0; p < 5; p++) {
                    double v = 0;
                    if (!eval_expr(nums, ops, p, &v)) continue;
                    if (fabs(v - 24.0) < 1e-6) return true;
                }
            }
        }
    }
    return false;
}

static int rand_1_9(void)
{
    return (int)(esp_random() % 9U) + 1;
}

static void reset_build(void)
{
    s_ops[0] = 0;
    s_ops[1] = 0;
    s_ops[2] = 0;
    s_pattern = 0;
    s_sel = 0;
}

static void next_question(void)
{
    int n[4] = {0};
    bool ok = false;
    for (int i = 0; i < 120; i++) {
        n[0] = rand_1_9();
        n[1] = rand_1_9();
        n[2] = rand_1_9();
        n[3] = rand_1_9();
        if (puzzle_has_solution(n)) {
            ok = true;
            break;
        }
    }

    if (!ok) {
        n[0] = 6; n[1] = 1; n[2] = 3; n[3] = 4; // known solvable
    }

    s_nums[0] = n[0];
    s_nums[1] = n[1];
    s_nums[2] = n[2];
    s_nums[3] = n[3];
    s_suits[0] = (int)(esp_random() % 4U);
    s_suits[1] = (int)(esp_random() % 4U);
    s_suits[2] = (int)(esp_random() % 4U);
    s_suits[3] = (int)(esp_random() % 4U);
    reset_build();
}

static void build_expr_text(char* out, int cap)
{
    char o1 = op_char(s_ops[0]);
    char o2 = op_char(s_ops[1]);
    char o3 = op_char(s_ops[2]);
    int a = s_nums[0], b = s_nums[1], c = s_nums[2], d = s_nums[3];

    if (!out || cap <= 0) return;

    switch (s_pattern) {
        case 0: snprintf(out, cap, "((%d%c%d)%c%d)%c%d", a, o1, b, o2, c, o3, d); break;
        case 1: snprintf(out, cap, "(%d%c(%d%c%d))%c%d", a, o1, b, o2, c, o3, d); break;
        case 2: snprintf(out, cap, "%d%c((%d%c%d)%c%d)", a, o1, b, o2, c, o3, d); break;
        case 3: snprintf(out, cap, "%d%c(%d%c(%d%c%d))", a, o1, b, o2, c, o3, d); break;
        case 4: snprintf(out, cap, "(%d%c%d)%c(%d%c%d)", a, o1, b, o2, c, o3, d); break;
        default: snprintf(out, cap, "?"); break;
    }
}

static void draw_body(void)
{
    char expr[40], l2[56], l3[40], l4[24], l5[40], l6[40], l7[40];
    double value = 0.0;
    bool valid = eval_expr(s_nums, s_ops, s_pattern, &value);

    build_expr_text(expr, (int)sizeof(expr));
    snprintf(l2, sizeof(l2), "Expr:%s", expr);
    snprintf(l3, sizeof(l3), "Value: %s", valid ? "" : "INVALID");
    if (valid) snprintf(l3, sizeof(l3), "Value: %.2f", value);
    snprintf(l4, sizeof(l4), "Sel: %s", sel_name(s_sel));
    snprintf(l5, sizeof(l5), " +   -   x   / ");
    snprintf(l6, sizeof(l6), " ( )   =   DEL ");
    snprintf(l7, sizeof(l7), " Score: %d   Correct:%d/%d", s_score, s_correct, s_total);

    Ui_BeginBatch();
    if (!s_frame_inited) {
        Ui_DrawFrame("24 GAME", "DN:NEXT  OK:SET  BACK");
        s_frame_inited = true;
    }
    // Avoid full-body clear on every key press (causes visible flicker).
    // Cards area is fully repainted by draw_cards_panel().
    // Text area is cleared in narrow bands before redrawing dynamic text.
    draw_cards_panel();
    uint16_t bg = Ui_ColorRGB(8, 12, 20);
    St7789_FillRect(12, 212, St7789_Width() - 24, 18, bg);
    St7789_FillRect(12, 232, St7789_Width() - 24, 18, bg);
    St7789_FillRect(12, 252, St7789_Width() - 24, 18, bg);
    St7789_FillRect(12, 270, St7789_Width() - 24, 18, bg);
    Ui_DrawTextAtBg(18, 214, l5, Ui_ColorRGB(230, 230, 230), Ui_ColorRGB(8, 12, 20));
    Ui_DrawTextAtBg(18, 234, l6, Ui_ColorRGB(230, 230, 230), Ui_ColorRGB(8, 12, 20));
    Ui_DrawTextAtBg(18, 254, l2, Ui_ColorRGB(230, 230, 230), Ui_ColorRGB(8, 12, 20));
    Ui_DrawTextAtBg(18, 272, l3, Ui_ColorRGB(230, 230, 230), Ui_ColorRGB(8, 12, 20));
    Ui_DrawTextAtBg(138, 214, l4, Ui_ColorRGB(255, 190, 90), Ui_ColorRGB(8, 12, 20));
    Ui_DrawTextAtBg(138, 234, l7, Ui_ColorRGB(180, 220, 180), Ui_ColorRGB(8, 12, 20));
    Ui_DrawTextAtBg(138, 272, s_msg, Ui_ColorRGB(240, 200, 120), Ui_ColorRGB(8, 12, 20));
    Ui_EndBatch();
}

static void cycle_value(int dir)
{
    if (s_sel >= kSelOp1 && s_sel <= kSelOp3) {
        int idx = s_sel;
        s_ops[idx] = (s_ops[idx] + dir + 4) % 4;
    } else if (s_sel == kSelPattern) {
        s_pattern = (s_pattern + dir + 5) % 5;
    }
}

static void check_answer(void)
{
    double v = 0.0;
    bool ok = eval_expr(s_nums, s_ops, s_pattern, &v) && (fabs(v - 24.0) < 1e-6);
    s_total++;
    if (ok) {
        s_correct++;
        s_score += 2;
        snprintf(s_msg, sizeof(s_msg), "Correct! +2 points.");
        Sfx_PlayVictory();
    } else {
        snprintf(s_msg, sizeof(s_msg), "Not 24, try next one.");
    }
    next_question();
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter");
    s_total = 0;
    s_correct = 0;
    s_score = 0;
    s_frame_inited = false;
    snprintf(s_msg, sizeof(s_msg), "Build expr to make 24.");
    next_question();
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("24 GAME", "OK:START  BACK");
    Ui_Println("Style: 2x2 nums + op pad.");
    Ui_Println("Goal: make value = 24.");
    Ui_Println("Single player mode.");
    Ui_Println("DN move sel, UP reverse.");
    Ui_Println("OK set/check.");
    Ui_Println("Correct gives +2 points.");
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    draw_body();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (key == kInputDown) {
        s_sel = (s_sel + 1) % 5;
    } else if (key == kInputUp) {
        if (s_sel == kSelCheck) {
            next_question();
            snprintf(s_msg, sizeof(s_msg), "New puzzle.");
        } else {
            cycle_value(-1);
        }
    } else if (key == kInputEnter) {
        if (s_sel == kSelCheck) check_answer();
        else cycle_value(1);
    } else {
        return;
    }
    draw_body();
}

static void tick(ExperimentContext* ctx) { (void)ctx; }

const Experiment g_exp_math24 = {
    .id = 14,
    .title = "24 GAME",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
