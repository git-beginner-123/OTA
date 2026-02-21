#include "experiments/experiment.h"
#include "ui/ui.h"
#include "audio/audio_engine.h"
#include "display/st7789.h"
#include "display/font8x16.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char* TAG = "EXP_MUSIC";
static const char* MUSIC_REV = "R260220C";

#define SAMPLE_RATE 16000
#define WRITE_TIMEOUT_MS 300
#define NOTE_GAP_MS 20
#define AMP 5200
#define MUSIC_AUDIO_ENABLED 1
// Global tempo scale for playback.
#define MUSIC_TEMPO_SCALE_PCT 140
#define UI_HEADER_H 30
#define UI_PAD_X 10
#define UI_PAD_Y 6
#define UI_FONT_W 8
#define UI_CHAR_GAP 1
#define UI_LINE_H 20
#define MUSIC_BG_COLOR ((uint16_t)0x0861)
#define MUSIC_ROWS_PER_PAGE 9
#define MUSIC_UNITS_PER_ROW 8
#define MUSIC_MAX_UNITS (MUSIC_ROWS_PER_PAGE * MUSIC_UNITS_PER_ROW) // 18 bars max

typedef struct {
    uint8_t degree;   // 0 rest, 1..7 note
    int8_t octave;    // -1 low, 0 middle, +1 high
    uint8_t unit_x4;  // duration in quarter-beat units: 4=1 beat, 2=1/2 beat
    uint8_t dotted;   // 1 if dotted rhythm marker exists
} JNote;

static TaskHandle_t s_play_task = NULL;
static bool s_running = false;
static volatile bool s_playing = false;
static volatile bool s_stop_req = false;
static int s_tempo_bpm = 112;
static bool s_last_playing = false;
static int s_song_idx = 0;
static volatile int s_play_note_idx = -1;
static int s_last_play_note_idx = -2;
static AudioEngineSession s_music_session = {0};

typedef struct {
    const char* name;
    const char* key;
    const char* meter;
    int bpm;
    const char* playback_notation;
    const char* display_score;
} Song;

// Each song provides >=14 beats.
static const Song k_songs[] = {
    {
        .name = "SOY UNA TAZA",
        .key = "C",
        .meter = "4/4",
        .bpm = 132,
        .playback_notation =
            "5/4 5/4 5/4 5/4 6/4 6/4 6/4 -/4 5/4 5/4 5/4 5/4 3/4 3/4 3/4 -/4 "
            "5/4 5/4 5/4 5/4 6/4 6/4 6/4 -/4 5/4 3/4 2/4 1/4 1/4 -/4 -/4 -/4 "
            "5/4 5/4 5/4 5/4 6/4 6/4 6/4 -/4 5/4 5/4 5/4 5/4 3/4 3/4 3/4 -/4 "
            "5/4 5/4 5/4 5/4 6/4 6/4 6/4 -/4 5/4 3/4 2/4 1/4 1/4 -/4 -/4 -/4 "
            "3/4 3/4 3/4 3/4 5/4 5/4 5/4 -/4 6/4 6/4 5/4 3/4 2/4 -/4 -/4 -/4 "
            "5/4 3/4 2/4 1/4 1/4 -/4 -/4 -/4 1/4 -/4 -/4 -/4 1/4 -/4 -/4 -/4",
        .display_score = NULL,
    },
    {
        .name = "El patio de mi casa",
        .key = "C",
        .meter = "4/4",
        .bpm = 108,
        .playback_notation =
            "1/4 2/4 3/4 1/4 1/4 2/4 3/4 1/4 "
            "3/4 4/4 5/4 -/4 3/4 4/4 5/4 -/4",
        .display_score = NULL,
    },
    {
        .name = "Debajo un boton",
        .key = "C",
        .meter = "4/4",
        .bpm = 100,
        .playback_notation =
            "5/4 3/4 3/4 4/4 5/4 5/4 5/4 -/4 "
            "6/4 5/4 4/4 3/4 2/4 2/4 1/4 -/4",
        .display_score = NULL,
    },
    {
        .name = "Cinco lobitos",
        .key = "C",
        .meter = "2/4",
        .bpm = 96,
        .playback_notation =
            "3/4 3/4 5/4 5/4 6/4 6/4 5/4 -/4 "
            "4/4 4/4 3/4 3/4 2/4 2/4 1/4 -/4",
        .display_score = NULL,
    },
    {
        .name = "Tengo una muneca",
        .key = "C",
        .meter = "4/4",
        .bpm = 104,
        .playback_notation =
            "5/4 5/4 6/4 5/4 3/4 3/4 2/4 -/4 "
            "3/4 3/4 4/4 3/4 2/4 2/4 1/4 -/4",
        .display_score = NULL,
    },
    {
        .name = "Un elefante se balanceaba",
        .key = "C",
        .meter = "3/4",
        .bpm = 92,
        .playback_notation =
            "1/4 1/4 2/4 3/4 3/4 2/4 1/4 -/4 "
            "1/4 1/4 2/4 3/4 2/4 1/4 2/4 -/4",
        .display_score = NULL,
    },
};

#define SONG_COUNT ((int)(sizeof(k_songs) / sizeof(k_songs[0])))

static uint32_t note_hz(uint8_t degree, int8_t octave)
{
    static const uint16_t c_major_mid[7] = {262, 294, 330, 349, 392, 440, 494}; // C4..B4
    if (degree < 1 || degree > 7) return 0;

    uint32_t hz = c_major_mid[degree - 1];
    if (octave > 0) hz <<= 1;
    else if (octave < 0) hz >>= 1;
    if (hz < 40) hz = 40;
    return hz;
}

static int parse_numbered_notation(const char* src, JNote* out, int cap)
{
    if (!src || !out || cap <= 0) return 0;
    int n = 0;
    const char* p = src;

    while (*p != '\0' && n < cap) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == '\0') break;

        int8_t octave = 0;
        if (*p == '.') {
            // ASCII low-octave marker: ".5"
            octave = -1;
            p++;
        } else if (*p == 'L' || *p == 'l') {
            octave = -1;
            p++;
        } else if (*p == 'H' || *p == 'h') {
            octave = 1;
            p++;
        } else if (*p == 'M' || *p == 'm') {
            octave = 0;
            p++;
        }

        uint8_t degree = 0;
        if (*p >= '1' && *p <= '7') degree = (uint8_t)(*p - '0');
        else if (*p == '0' || *p == '-') degree = 0;
        else {
            while (*p != '\0' && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n') p++;
            continue;
        }
        p++;

        // ASCII high-octave marker: "6'"
        if (*p == '\'') {
            octave = 1;
            p++;
        }

        uint8_t unit_x4 = 4; // default 1 beat
        if (*p == '/') {
            p++;
            if (*p == '2') unit_x4 = 2;      // 1/2 beat
            else if (*p == '4') unit_x4 = 1; // 1/4 beat
            else if (*p == '1') unit_x4 = 4;
            if (*p != '\0') p++;
        }

        // Dotted rhythm marker: "5." or "3/2."
        uint8_t dotted = 0;
        if (*p == '.') {
            unit_x4 = (uint8_t)(unit_x4 + (unit_x4 / 2));
            dotted = 1;
            p++;
        }

        out[n].degree = degree;
        out[n].octave = octave;
        out[n].unit_x4 = unit_x4;
        out[n].dotted = dotted;
        n++;
    }
    return n;
}

static void select_song(int delta)
{
    s_song_idx += delta;
    while (s_song_idx < 0) s_song_idx += SONG_COUNT;
    s_song_idx %= SONG_COUNT;
    s_tempo_bpm = k_songs[s_song_idx].bpm;
}

static int limit_note_count_by_units(const JNote* notes, int note_count, int max_units)
{
    if (!notes || note_count <= 0 || max_units <= 0) return 0;
    int used = 0;
    int count = 0;
    for (int i = 0; i < note_count; i++) {
        int dur = (int)notes[i].unit_x4;
        if (dur < 1) dur = 1;
        if (used + dur > max_units) break;
        used += dur;
        count++;
    }
    return count;
}

static void draw_char8x16_at(int x, int y, char c, uint16_t fg)
{
    const uint8_t* rows = Font8x16_Get(c);
    if (!rows) return;
    for (int ry = 0; ry < 16; ry++) {
        uint8_t bits = rows[ry];
        for (int rx = 0; rx < 8; rx++) {
            if (bits & (0x80U >> rx)) {
                St7789_FillRect(x + rx, y + ry, 1, 1, fg);
            }
        }
    }
}

static void draw_char8x16_bg(int x, int y, char c, uint16_t fg, uint16_t bg)
{
    St7789_FillRect(x, y, 8, 16, bg);
    draw_char8x16_at(x, y, c, fg);
}

static void draw_note8x16_from_font(int x, int y, uint8_t degree, int8_t octave, uint16_t fg, int filled_bg, uint16_t bg)
{
    if (filled_bg) St7789_FillRect(x, y, 8, 16, bg);
    const uint8_t* rows = Font8x16_GetNumberedNoteGlyph(degree, octave);
    if (!rows) return;
    for (int ry = 0; ry < 16; ry++) {
        uint8_t bits = rows[ry];
        for (int rx = 0; rx < 8; rx++) {
            if (bits & (0x80U >> rx)) {
                St7789_FillRect(x + rx, y + ry, 1, 1, fg);
            }
        }
    }
}

static void draw_text8x16_at(int x, int y, const char* s, uint16_t fg)
{
    if (!s) return;
    int px = x;
    for (const char* p = s; *p; p++) {
        draw_char8x16_at(px, y, *p, fg);
        px += 9;
    }
}

static void draw_bar_marker_at(int x, int y, uint16_t color)
{
    // Thicker bar marker than glyph '|' so it cannot be confused with digit '1'.
    St7789_FillRect(x + 3, y + 1, 2, 14, color);
}

typedef struct {
    char sym;
    int8_t octave;
    uint8_t underlines;
    int note_idx;
    uint8_t valid;
} UnitCell;

// Avoid large stack usage in main task when rendering MUSIC page.
static JNote s_score_notes[256];
static UnitCell s_score_timeline[256];
static int s_score_note_start_unit[256];

static void draw_numbered_score(const Song* s)
{
    if (!s) return;
    const uint16_t fg = Ui_ColorRGB(168, 172, 176);
    const uint16_t bar_fg = Ui_ColorRGB(255, 208, 96);
    const uint16_t hi_bg = Ui_ColorRGB(255, 208, 96);
    const uint16_t hi_fg = Ui_ColorRGB(12, 12, 12);
    const uint16_t base_bg = MUSIC_BG_COLOR;

    JNote* notes = s_score_notes;
    // Keep UI highlight index aligned with playback index.
    int note_count = parse_numbered_notation(
        s->playback_notation, notes, (int)(sizeof(s_score_notes) / sizeof(s_score_notes[0]))
    );
    if (note_count <= 0) {
        St7789_Flush();
        return;
    }

    // Render on a fixed 1/4-beat unit grid.
    UnitCell* timeline = s_score_timeline;
    for (int i = 0; i < (int)(sizeof(s_score_timeline) / sizeof(s_score_timeline[0])); i++) {
        timeline[i].sym = ' ';
        timeline[i].octave = 0;
        timeline[i].underlines = 0;
        timeline[i].note_idx = -1;
        timeline[i].valid = 0;
    }
    int unit_pos = 0;
    for (int i = 0;
         i < note_count &&
         unit_pos < (int)(sizeof(s_score_timeline) / sizeof(s_score_timeline[0])) &&
         unit_pos < MUSIC_MAX_UNITS;
         i++) {
        char sym = (notes[i].degree == 0) ? '-' : (char)('0' + notes[i].degree);
        int dur = (int)notes[i].unit_x4;
        if (dur < 1) dur = 1;
        uint8_t under = 0;
        if (notes[i].unit_x4 == 2) under = 1; // 8th
        else if (notes[i].unit_x4 == 1) under = 2; // 16th
        for (int u = 0;
             u < dur &&
             unit_pos < (int)(sizeof(s_score_timeline) / sizeof(s_score_timeline[0])) &&
             unit_pos < MUSIC_MAX_UNITS;
             u++) {
            timeline[unit_pos].sym = sym;
            timeline[unit_pos].octave = notes[i].octave;
            timeline[unit_pos].underlines = under;
            timeline[unit_pos].note_idx = i;
            timeline[unit_pos].valid = 1;
            unit_pos++;
        }
    }
    int unit_count = unit_pos;
    int* note_start_unit = s_score_note_start_unit;
    for (int i = 0; i < (int)(sizeof(s_score_note_start_unit) / sizeof(s_score_note_start_unit[0])); i++) {
        note_start_unit[i] = -1;
    }
    for (int u = 0; u < unit_count; u++) {
        int ni = timeline[u].note_idx;
        if (ni >= 0 && ni < (int)(sizeof(s_score_note_start_unit) / sizeof(s_score_note_start_unit[0])) &&
            note_start_unit[ni] < 0) {
            note_start_unit[ni] = u;
        }
    }

    int body_x = 2;
    int body_y = UI_HEADER_H + 1;
    int body_w = St7789_Width() - 4;
    int body_h = St7789_Height() - UI_HEADER_H - 26 - 2;

    // Title/meta line only; no horizontal guide lines in score area.
    char info[64];
    snprintf(info, sizeof(info), "%s  %s  %s", s->name, s->key, s->meter);
    draw_text8x16_at(body_x + 2, body_y + 1, info, fg);

    // Keep original layout style: 2 bars per row, now 9 rows -> 18 bars max.
    const int rows = MUSIC_ROWS_PER_PAGE;
    const int units_per_row = MUSIC_UNITS_PER_ROW;
    const int page_units = rows * units_per_row;
    int active_unit = -1;
    if (s_playing && s_play_note_idx >= 0 &&
        s_play_note_idx < (int)(sizeof(s_score_note_start_unit) / sizeof(s_score_note_start_unit[0]))) {
        active_unit = note_start_unit[s_play_note_idx];
    }
    int start_unit = 0;
    if (active_unit >= 0) {
        start_unit = (active_unit / units_per_row - rows / 2) * units_per_row;
        if (start_unit < 0) start_unit = 0;
        int max_start = unit_count - page_units;
        if (max_start < 0) max_start = 0;
        if (start_unit > max_start) start_unit = max_start;
    }

    int score_top = body_y + 24;
    int score_bottom = body_y + body_h - 3;
    int score_h = score_bottom - score_top;
    if (score_h < 8) score_h = 8;
    int row_gap = score_h / rows;
    if (row_gap < 22) row_gap = 22;
    if (row_gap > 24) row_gap = 24;

    int x_left_bar = body_x + 4;
    int x_mid_bar = body_x + body_w / 2;
    int x_right_bar = body_x + body_w - 5;
    int beat1_w = (x_mid_bar - x_left_bar - 1);
    int beat2_w = (x_right_bar - x_mid_bar - 1);
    int step1 = beat1_w / 5;
    int step2 = beat2_w / 5;
    if (step1 < 9) step1 = 9;
    if (step2 < 9) step2 = 9;

    for (int row = 0; row < rows; row++) {
        int i0 = start_unit + row * units_per_row;
        int y = score_top + row * row_gap;
        int note_y = y;
        draw_bar_marker_at(x_left_bar, note_y, bar_fg);
        draw_bar_marker_at(x_mid_bar, note_y, bar_fg);
        draw_bar_marker_at(x_right_bar, note_y, bar_fg);

        int tx1 = x_left_bar + step1;
        int tx2 = x_mid_bar + step2;
        for (int b = 0; b < 2; b++) {
            int tx = (b == 0) ? tx1 : tx2;
            int step = (b == 0) ? step1 : step2;
            for (int u = 0; u < 4; u++) {
                int unit_idx = i0 + b * 4 + u;
                if (unit_idx < 0 || unit_idx >= unit_count) continue;
                if (!timeline[unit_idx].valid) continue;

                int cx = tx + u * step;
                char c = timeline[unit_idx].sym;
                bool onset = (unit_idx == 0) ||
                             !timeline[unit_idx - 1].valid ||
                             (timeline[unit_idx - 1].note_idx != timeline[unit_idx].note_idx);
                int beat_start = (active_unit >= 0) ? ((active_unit / 4) * 4) : -1;
                bool active = s_playing &&
                              onset &&
                              (unit_idx == beat_start);

                if (timeline[unit_idx].sym >= '1' && timeline[unit_idx].sym <= '7') {
                    draw_note8x16_from_font(
                        cx, note_y,
                        (uint8_t)(timeline[unit_idx].sym - '0'),
                        timeline[unit_idx].octave,
                        active ? hi_fg : fg,
                        1,
                        active ? hi_bg : base_bg
                    );
                } else {
                    draw_char8x16_bg(cx, note_y, c, active ? hi_fg : fg, active ? hi_bg : base_bg);
                }

                // 8th/16th marks: underline(s) below the note symbol.
                if (onset && timeline[unit_idx].underlines >= 1) {
                    St7789_FillRect(cx + 1, note_y + 18, 6, 1, active ? hi_fg : fg);
                } else {
                    St7789_FillRect(cx + 1, note_y + 18, 6, 1, active ? hi_bg : base_bg);
                }
                if (onset && timeline[unit_idx].underlines >= 2) {
                    St7789_FillRect(cx + 1, note_y + 19, 6, 1, active ? hi_fg : fg);
                } else {
                    St7789_FillRect(cx + 1, note_y + 19, 6, 1, active ? hi_bg : base_bg);
                }
            }
        }
    }

    St7789_Flush();
}

static void render_run(void)
{
    char t[32];
    snprintf(t, sizeof(t), "MUSIC %s", MUSIC_REV);
    Ui_DrawFrame(t, "UP:PREV DN:NEXT OK:PLAY BACK");
    Ui_DrawBodyClear();
    const Song* s = &k_songs[s_song_idx];
    draw_numbered_score(s);
}

static void wait_play_task_exit(uint32_t wait_ms)
{
    uint32_t waited = 0;
    while (s_play_task && waited < wait_ms) {
        vTaskDelay(pdMS_TO_TICKS(10));
        waited += 10;
    }
}

static void music_play_task(void* arg)
{
    (void)arg;
    const Song* song = &k_songs[s_song_idx];
    JNote notes[128];
    int note_count = parse_numbered_notation(song->playback_notation, notes, (int)(sizeof(notes) / sizeof(notes[0])));
    note_count = limit_note_count_by_units(notes, note_count, MUSIC_MAX_UNITS);
    int tempo = s_tempo_bpm;
    uint32_t beat_ms = (uint32_t)(60000 / (tempo > 0 ? tempo : 1));
    beat_ms = (uint32_t)(((uint64_t)beat_ms * MUSIC_TEMPO_SCALE_PCT) / 100ULL);

    for (int i = 0; i < note_count && !s_stop_req; i++) {
        const JNote* n = &notes[i];
        s_play_note_idx = i;
        uint32_t dur_ms = (beat_ms * n->unit_x4) / 4U;
        if (dur_ms <= NOTE_GAP_MS + 5) dur_ms = NOTE_GAP_MS + 5;
        uint32_t tone_ms = dur_ms - NOTE_GAP_MS;

        // Numbered notation is rendered as monophonic tones:
        // one note -> one frequency -> one square wave segment.
        uint32_t hz = note_hz(n->degree, n->octave);
        if (!AudioEngine_PlaySquareMs(&s_music_session, SAMPLE_RATE, hz, tone_ms, AMP, WRITE_TIMEOUT_MS)) break;
        if (!AudioEngine_PlaySilenceMs(&s_music_session, SAMPLE_RATE, NOTE_GAP_MS, WRITE_TIMEOUT_MS)) break;
    }

    s_play_note_idx = -1;
    s_playing = false;
    s_play_task = NULL;
    vTaskDelete(NULL);
}

static void show_requirements(ExperimentContext* ctx)
{
    (void)ctx;
    Ui_DrawFrame("MUSIC", "OK:START  BACK");
    Ui_Println("6-song numbered notation player.");
    Ui_Println("UP/DN select song.");
    Ui_Println("OK play/stop selected song.");
    Ui_Println("Show title, key, and meter.");
    Ui_Println("Show full score for song 1.");
}

static void on_enter(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_enter rev=%s", MUSIC_REV);
    s_running = false;
    s_playing = false;
    s_stop_req = false;
    s_song_idx = 0;
    s_tempo_bpm = k_songs[s_song_idx].bpm;
    s_last_playing = false;
    s_play_note_idx = -1;
    s_last_play_note_idx = -2;
}

static void exp_on_exit(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "on_exit");
    if (!s_running) return;

    s_stop_req = true;
    wait_play_task_exit(600);
    if (s_play_task) {
        ESP_LOGW(TAG, "music task exit timeout");
        return;
    }
    s_play_note_idx = -1;
    s_playing = false;
    AudioEngine_Close(&s_music_session);
    s_running = false;
}

static void start(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "start");
    if (s_running) return;

    if (!MUSIC_AUDIO_ENABLED) {
        ESP_LOGW(TAG, "music audio disabled for test");
        Ui_DrawFrame("MUSIC", "AUDIO OFF (TEST)");
        Ui_DrawBodyClear();
        Ui_Println("Audio disabled for testing.");
        Ui_Println("Enable MUSIC_AUDIO_ENABLED to test.");
        return;
    }

    if (!AudioEngine_Open(&s_music_session, kAudioEngineMusic, 75, UINT32_MAX, &s_stop_req)) {
        ESP_LOGW(TAG, "audio engine busy/not ready");
        return;
    }

    s_stop_req = false;
    s_playing = false;
    s_play_note_idx = -1;
    s_running = true;
    s_last_playing = false;
    render_run();
}

static void stop(ExperimentContext* ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "stop");
    if (!s_running) return;

    s_stop_req = true;
    wait_play_task_exit(600);
    if (s_play_task) {
        ESP_LOGW(TAG, "music task exit timeout");
        return;
    }
    s_play_note_idx = -1;
    s_playing = false;
    AudioEngine_Close(&s_music_session);
    s_running = false;
}

static void on_key(ExperimentContext* ctx, InputKey key)
{
    (void)ctx;
    if (!s_running) return;

    bool changed = false;

    if (key == kInputUp) {
        if (!s_playing) {
            select_song(-1);
            changed = true;
        }
    } else if (key == kInputDown) {
        if (!s_playing) {
            select_song(+1);
            changed = true;
        }
    } else if (key == kInputEnter) {
        if (s_playing) {
            s_stop_req = true;
            wait_play_task_exit(600);
            if (s_play_task) {
                ESP_LOGW(TAG, "music task exit timeout");
                return;
            }
            s_play_note_idx = -1;
            s_playing = false;
            s_stop_req = false;
        } else {
            s_stop_req = false;
            s_playing = true;
            if (xTaskCreate(music_play_task, "music_play", 4096, NULL, 5, &s_play_task) != pdPASS) {
                s_play_task = NULL;
                s_playing = false;
                ESP_LOGW(TAG, "music task create failed");
            }
        }
        changed = true;
    }

    if (changed) render_run();
}

static void tick(ExperimentContext* ctx)
{
    (void)ctx;
    if (!s_running) return;
    bool playing_changed = (s_last_playing != s_playing);
    bool note_changed = (s_last_play_note_idx != s_play_note_idx);
    if (playing_changed || note_changed) {
        s_last_playing = s_playing;
        s_last_play_note_idx = s_play_note_idx;
        if (playing_changed) {
            render_run();
        } else {
            const Song* s = &k_songs[s_song_idx];
            draw_numbered_score(s);
        }
    }
}

const Experiment g_exp_music = {
    .id = 19,
    .title = "MUSIC",
    .on_enter = on_enter,
    .on_exit = exp_on_exit,
    .show_requirements = show_requirements,
    .start = start,
    .stop = stop,
    .on_key = on_key,
    .tick = tick,
};
