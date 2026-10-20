// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/frecency — frecency-based directory navigation

#ifndef JASH_FRECENCY_H
#define JASH_FRECENCY_H

#include <stdint.h>
#include <time.h>

#define FRECENCY_MAX 64
#define FRECENCY_PATH_MAX 512

typedef struct {
    char path[FRECENCY_PATH_MAX];
    uint32_t visits;        /* total visit count */
    time_t last_visit;      /* last time visited */
    uint32_t rank;          /* computed score = visits * recency_weight */
} frecency_entry_t;

typedef struct {
    frecency_entry_t entries[FRECENCY_MAX];
    int count;
    char persist_path[256]; /* ~/.jash_frecency */
} frecency_table_t;

void frecency_init(frecency_table_t* ft, const char* home);
void frecency_record(frecency_table_t* ft, const char* path);
const char* frecency_best(frecency_table_t* ft);
const char* frecency_match(frecency_table_t* ft, const char* prefix);
void frecency_list(frecency_table_t* ft, char entries[][FRECENCY_PATH_MAX], int* count);
void frecency_save(frecency_table_t* ft);
void frecency_load(frecency_table_t* ft);

#endif /* JASH_FRECENCY_H */

