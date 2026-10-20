// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Module: core/predict — smart command prediction (pattern learning)

#ifndef JASH_PREDICT_H
#define JASH_PREDICT_H

#include <stdint.h>
#include <time.h>

#define PREDICT_MAX 256
#define PREDICT_CMD_LEN 128
#define PREDICT_CONTEXT_LEN 64

typedef struct {
    char cmd[PREDICT_CMD_LEN];
    char context[PREDICT_CONTEXT_LEN];
    uint32_t count;
    time_t last_seen;
} predict_entry_t;

typedef struct {
    predict_entry_t entries[PREDICT_MAX];
    int count;
    char persist_path[256];
    int dirty;  /* Dirty flag: 1 = needs save, 0 = clean */
} predict_table_t;

void predict_init(predict_table_t* pt, const char* home);
void predict_record(predict_table_t* pt, const char* cmd, const char* context);
const char* predict_best(predict_table_t* pt, const char* context);
void predict_save(predict_table_t* pt);
void predict_load(predict_table_t* pt);
void predict_mark_dirty(predict_table_t* pt);
void predict_save_if_dirty(predict_table_t* pt);
void predict_make_context(char* ctx, size_t cap, const char* cwd);

#endif /* JASH_PREDICT_H */
