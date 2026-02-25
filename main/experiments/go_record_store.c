#include "experiments/go_record_store.h"

#include "nvs.h"

#include <string.h>
#include <stdio.h>
#include <time.h>

#define GO_REC_NS "go_records"
#define GO_REC_META_KEY "meta"
#define GO_REC_MAX 24
#define GO_REC_VERSION 1

typedef struct {
    uint32_t id;
    uint32_t date_code;
    uint16_t seq_of_day;
    uint16_t move_count;
    char name[20];
} GoRecordMetaEntry;

typedef struct {
    uint16_t version;
    uint16_t count;
    uint32_t next_id;
    GoRecordMetaEntry entries[GO_REC_MAX];
} GoRecordMeta;

static void meta_init(GoRecordMeta* m)
{
    memset(m, 0, sizeof(*m));
    m->version = GO_REC_VERSION;
    m->next_id = 1;
}

static bool meta_load(nvs_handle_t h, GoRecordMeta* out)
{
    size_t sz = sizeof(*out);
    if (nvs_get_blob(h, GO_REC_META_KEY, out, &sz) != ESP_OK || sz != sizeof(*out)) {
        meta_init(out);
        return false;
    }
    if (out->version != GO_REC_VERSION || out->count > GO_REC_MAX) {
        meta_init(out);
        return false;
    }
    if (out->next_id == 0) out->next_id = 1;
    return true;
}

static bool meta_save(nvs_handle_t h, const GoRecordMeta* m)
{
    if (nvs_set_blob(h, GO_REC_META_KEY, m, sizeof(*m)) != ESP_OK) return false;
    return nvs_commit(h) == ESP_OK;
}

static uint32_t fallback_date_code(void)
{
    return 20260101U;
}

static uint32_t today_date_code_if_valid(bool has_network)
{
    if (!has_network) return fallback_date_code();

    time_t now = time(NULL);
    struct tm t = {0};
    if (now <= 0 || localtime_r(&now, &t) == NULL) return fallback_date_code();

    int year = t.tm_year + 1900;
    if (year < 2024 || year > 2100) return fallback_date_code();
    int month = t.tm_mon + 1;
    int day = t.tm_mday;
    return (uint32_t)(year * 10000 + month * 100 + day);
}

static void move_key(char out[16], uint32_t id)
{
    snprintf(out, 16, "m%08x", (unsigned)id);
}

static bool older_than(const GoRecordMetaEntry* a, const GoRecordMetaEntry* b)
{
    if (a->date_code != b->date_code) return a->date_code < b->date_code;
    if (a->seq_of_day != b->seq_of_day) return a->seq_of_day < b->seq_of_day;
    return a->id < b->id;
}

static int oldest_index(const GoRecordMeta* m)
{
    if (m->count <= 0) return -1;
    int oi = 0;
    for (int i = 1; i < (int)m->count; i++) {
        if (older_than(&m->entries[i], &m->entries[oi])) oi = i;
    }
    return oi;
}

static int next_seq_for_date(const GoRecordMeta* m, uint32_t date_code)
{
    int seq = 1;
    for (int i = 0; i < (int)m->count; i++) {
        if (m->entries[i].date_code == date_code && m->entries[i].seq_of_day >= seq) {
            seq = m->entries[i].seq_of_day + 1;
        }
    }
    if (seq > 999) seq = 999;
    return seq;
}

bool GoRecordStore_SaveMoves(const uint8_t* moves_xy,
                             uint16_t move_count,
                             bool has_network,
                             char* out_name,
                             size_t out_name_cap)
{
    if (!moves_xy || move_count == 0) return false;
    if (move_count > 255) return false;

    nvs_handle_t h = 0;
    if (nvs_open(GO_REC_NS, NVS_READWRITE, &h) != ESP_OK) return false;

    GoRecordMeta meta;
    meta_load(h, &meta);

    if (meta.count >= GO_REC_MAX) {
        int oi = oldest_index(&meta);
        if (oi >= 0) {
            char old_key[16];
            move_key(old_key, meta.entries[oi].id);
            (void)nvs_erase_key(h, old_key);
            for (int i = oi; i < (int)meta.count - 1; i++) {
                meta.entries[i] = meta.entries[i + 1];
            }
            meta.count--;
        }
    }

    uint32_t date_code = today_date_code_if_valid(has_network);
    int seq = next_seq_for_date(&meta, date_code);
    uint32_t id = meta.next_id++;
    if (meta.next_id == 0) meta.next_id = 1;

    char name[20];
    snprintf(name, sizeof(name), "%08u-%03d", (unsigned)date_code, seq);

    char key[16];
    move_key(key, id);
    int bytes = move_count * 2;
    if (nvs_set_blob(h, key, moves_xy, (size_t)bytes) != ESP_OK) {
        nvs_close(h);
        return false;
    }

    GoRecordMetaEntry e;
    memset(&e, 0, sizeof(e));
    e.id = id;
    e.date_code = date_code;
    e.seq_of_day = (uint16_t)seq;
    e.move_count = move_count;
    strncpy(e.name, name, sizeof(e.name) - 1);
    e.name[sizeof(e.name) - 1] = 0;

    meta.entries[meta.count++] = e;

    bool ok = meta_save(h, &meta);
    nvs_close(h);

    if (ok && out_name && out_name_cap > 0) {
        snprintf(out_name, out_name_cap, "%s", name);
    }
    return ok;
}

static int cmp_entry_desc(const GoRecordMetaEntry* a, const GoRecordMetaEntry* b)
{
    if (a->date_code != b->date_code) return (a->date_code > b->date_code) ? -1 : 1;
    if (a->seq_of_day != b->seq_of_day) return (a->seq_of_day > b->seq_of_day) ? -1 : 1;
    if (a->id != b->id) return (a->id > b->id) ? -1 : 1;
    return 0;
}

int GoRecordStore_List(GoRecordInfo* out, int cap)
{
    if (!out || cap <= 0) return 0;

    nvs_handle_t h = 0;
    if (nvs_open(GO_REC_NS, NVS_READONLY, &h) != ESP_OK) return 0;

    GoRecordMeta meta;
    if (!meta_load(h, &meta)) {
        nvs_close(h);
        return 0;
    }
    nvs_close(h);

    for (int i = 0; i < (int)meta.count - 1; i++) {
        int best = i;
        for (int j = i + 1; j < (int)meta.count; j++) {
            if (cmp_entry_desc(&meta.entries[j], &meta.entries[best]) < 0) best = j;
        }
        if (best != i) {
            GoRecordMetaEntry t = meta.entries[i];
            meta.entries[i] = meta.entries[best];
            meta.entries[best] = t;
        }
    }

    int n = meta.count;
    if (n > cap) n = cap;
    for (int i = 0; i < n; i++) {
        out[i].id = meta.entries[i].id;
        out[i].date_code = meta.entries[i].date_code;
        out[i].seq_of_day = meta.entries[i].seq_of_day;
        out[i].move_count = meta.entries[i].move_count;
        strncpy(out[i].name, meta.entries[i].name, sizeof(out[i].name) - 1);
        out[i].name[sizeof(out[i].name) - 1] = 0;
    }
    return n;
}

bool GoRecordStore_LoadMoves(uint32_t record_id,
                             uint8_t* out_moves_xy,
                             int out_cap_bytes,
                             int* out_move_count)
{
    if (!out_moves_xy || out_cap_bytes < 2 || !out_move_count) return false;
    *out_move_count = 0;

    nvs_handle_t h = 0;
    if (nvs_open(GO_REC_NS, NVS_READONLY, &h) != ESP_OK) return false;

    char key[16];
    move_key(key, record_id);
    size_t need = 0;
    if (nvs_get_blob(h, key, NULL, &need) != ESP_OK || need == 0 || (need % 2) != 0) {
        nvs_close(h);
        return false;
    }
    if ((int)need > out_cap_bytes) {
        nvs_close(h);
        return false;
    }
    if (nvs_get_blob(h, key, out_moves_xy, &need) != ESP_OK) {
        nvs_close(h);
        return false;
    }
    nvs_close(h);
    *out_move_count = (int)(need / 2);
    return true;
}
