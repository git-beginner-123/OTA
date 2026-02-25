#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint32_t id;
    uint32_t date_code;   // YYYYMMDD
    uint16_t seq_of_day;  // 1..N
    uint16_t move_count;
    char name[20];
} GoRecordInfo;

bool GoRecordStore_SaveMoves(const uint8_t* moves_xy,
                             uint16_t move_count,
                             bool has_network,
                             char* out_name,
                             size_t out_name_cap);

int GoRecordStore_List(GoRecordInfo* out, int cap);

bool GoRecordStore_LoadMoves(uint32_t record_id,
                             uint8_t* out_moves_xy,
                             int out_cap_bytes,
                             int* out_move_count);
