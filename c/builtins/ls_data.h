// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Read-only typed ls producer; neither stdout nor external command output is
// implicitly promoted into this stream.
#ifndef JASH_LS_DATA_H
#define JASH_LS_DATA_H

#include "ls_collector.h"
#include <core/data_stream.h>
#include <module/blake3.h>

typedef struct {
    jash_ls_cursor_t filesystem;
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    jdv_arena_t arena;
    jdv_value_t value;
    jdv_stream_end_t terminal;
    struct blake3 digest;
    void* scratch;
    size_t scratch_bytes;
    uint64_t items;
    uint64_t logical_bytes;
    uint64_t item_errors;
    jash_ls_collect_options_t options_template;
    const char* const* roots;
    size_t root_count;
    size_t root_index;
    int active;
} jash_ls_data_t;

/* source must be zero-initialized and address-stable through close. scratch is
 * caller-owned and serves one borrowed value per pull; no row accumulation.
 * The source holds the collector's bounded sorted-name index. A consumer must
 * inspect the terminal: END alone does not imply a complete observation. */
const jdv_schema_t* jash_ls_data_schema(void);
int jash_ls_data_open(jash_ls_data_t* source,
                      const jash_ls_collect_options_t* options,
                      void* scratch, size_t scratch_bytes,
                      const jdv_stream_limits_t* limits);
/* Operand paths are borrowed until close; roots are visited in caller order
 * with one bounded directory index live at a time and one global terminal. */
int jash_ls_data_open_many(jash_ls_data_t* source,
                           const jash_ls_collect_options_t* options,
                           const char* const* roots, size_t root_count,
                           void* scratch, size_t scratch_bytes,
                           const jdv_stream_limits_t* limits);
jdv_stream_pull_result_t jash_ls_data_pull(jash_ls_data_t* source,
                                            jdv_stream_record_t* record);
int jash_ls_data_cancel(jash_ls_data_t* source, jdv_slice_t reason,
                        const jdv_stream_end_t** terminal);
int jash_ls_data_close(jash_ls_data_t* source);

#endif
