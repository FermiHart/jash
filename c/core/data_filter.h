// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_DATA_FILTER_H
#define JASH_CORE_DATA_FILTER_H

#include "data_stream.h"
#include <module/blake3.h>

/* Native record-field equality. Input cursor ownership moves to this filter;
 * release of the output also releases the input. Only scalar predicates are
 * admitted. Missing fields never match. No stdout parsing or materialization. */
typedef struct {
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    jdv_stream_cursor_t* input;
    jdv_value_t expected;
    uint8_t field[128];
    size_t field_len;
    uint8_t expected_bytes[256];
    jdv_stream_end_t terminal;
    struct blake3 digest;
    uint64_t items;
    uint64_t logical_bytes;
    int active;
} jdv_filter_t;

int jdv_filter_equal_open(jdv_filter_t* filter, jdv_stream_cursor_t* input,
                          jdv_slice_t field, const jdv_value_t* expected);
int jdv_filter_close(jdv_filter_t* filter);

#endif
