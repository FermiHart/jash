// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_DATA_WIRE_IMPORT_H
#define JASH_CORE_DATA_WIRE_IMPORT_H

#include "data_wire.h"

/* One linear D05 cursor over prevalidated immutable JTV2 bytes. The source is
 * zero-initialized and address-stable; descriptor, schema and item storage is
 * caller-owned with non-overlapping lifetimes. Input stays immutable and alive
 * through close. Item pointers expire at next pull/cancel/release. Physical
 * credits measure imported frame bytes; materialized credits measure decoder
 * arena use. The JTV2 logical credit is preserved exactly per item. */
typedef struct {
    jdv_stream_descriptor_t descriptor;
    jdv_stream_cursor_t cursor;
    jdv_arena_t schemas;
    jdv_arena_t items;
    jdv_schema_t declared;
    jdv_schema_t guaranteed;
    jdv_schema_t final_schema;
    jdv_value_t item;
    jdv_stream_end_t terminal;
    jdv_wire_limits_t limits;
    const uint8_t* bytes;
    size_t size;
    size_t position;
    int active;
} jdv_wire_import_t;

int jdv_wire_import_v2_open(jdv_wire_import_t* source, const void* bytes,
                            size_t size, void* schema_storage,
                            size_t schema_capacity, void* item_storage,
                            size_t item_capacity,
                            const jdv_wire_limits_t* limits);
int jdv_wire_import_close(jdv_wire_import_t* source);

#endif
