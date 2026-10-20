// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Native projection of an already observed discovery entry. */
#ifndef JASH_BUILTINS_QUERY_FILE_H
#define JASH_BUILTINS_QUERY_FILE_H

#include "query_data.h"
#include <osif/osif_stat.h>
#include <sys/stat.h>

QUERY_DATA_SCALAR(QUERY_FILE_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(QUERY_FILE_PATH, JDV_PATH, 0);
QUERY_DATA_SCALAR(QUERY_FILE_NULL_SYMBOL, JDV_SYMBOL, 1);
QUERY_DATA_SCALAR(QUERY_FILE_NULL_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(QUERY_FILE_NULL_I64, JDV_I64, 1);
QUERY_DATA_SCALAR(QUERY_FILE_NULL_SIZE, JDV_FILESIZE, 1);
QUERY_DATA_SCALAR(QUERY_FILE_NULL_TIME, JDV_TIMESTAMP, 1);
enum {
    QF_COMPLETE, QF_ERRNO, QF_ERROR, QF_INDEX, QF_MTIME, QF_MTIME_NS,
    QF_MTIME_SECONDS, QF_PATH, QF_SIZE, QF_TIME_OVERFLOW, QF_TYPE, QF_FIELDS
};
static const jdv_schema_field_t QUERY_FILE_FIELDS[] = {
    QUERY_DATA_FIELD("complete", &QUERY_FILE_BOOL),
    QUERY_DATA_FIELD("errno", &QUERY_FILE_NULL_U64),
    QUERY_DATA_FIELD("error", &QUERY_FILE_NULL_SYMBOL),
    QUERY_DATA_FIELD("match_index", &QUERY_FILE_NULL_U64),
    QUERY_DATA_FIELD("mtime", &QUERY_FILE_NULL_TIME),
    QUERY_DATA_FIELD("mtime_nsec", &QUERY_FILE_NULL_U64),
    QUERY_DATA_FIELD("mtime_seconds", &QUERY_FILE_NULL_I64),
    QUERY_DATA_FIELD("path", &QUERY_FILE_PATH),
    QUERY_DATA_FIELD("size_bytes", &QUERY_FILE_NULL_SIZE),
    QUERY_DATA_FIELD("time_overflow", &QUERY_FILE_BOOL),
    QUERY_DATA_FIELD("type", &QUERY_FILE_NULL_SYMBOL)
};
static const jdv_schema_t QUERY_FILE_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {QUERY_FILE_FIELDS, QF_FIELDS, 0}
};

static inline jdv_value_t query_file_item(jdv_record_field_t fields[QF_FIELDS],
                                          const char* path, const struct stat* st,
                                          const char* type, int has_index,
                                          uint64_t index, const char* error,
                                          int error_number) {
    jdv_value_t value = query_data_record(fields, &QUERY_FILE_SCHEMA);
    fields[QF_PATH].value = query_data_text(JDV_PATH, path);
    if (has_index) fields[QF_INDEX].value = jdv_value_u64(index);
    if (st && st->st_size < 0 && !error) {
        error = "negative_file_size"; error_number = EOVERFLOW;
    }
    fields[QF_COMPLETE].value = jdv_value_bool(!error);
    if (error) fields[QF_ERROR].value = query_data_text(JDV_SYMBOL, error);
    if (error_number) fields[QF_ERRNO].value = jdv_value_u64((uint64_t)error_number);
    fields[QF_TIME_OVERFLOW].value = jdv_value_bool(0);
    if (!st) return value;
    fields[QF_TYPE].value = query_data_text(JDV_SYMBOL, type);
    if (st->st_size >= 0) fields[QF_SIZE].value = jdv_value_filesize((uint64_t)st->st_size);
    int64_t seconds = BEAR_ST_MTIM(*st).tv_sec;
    long ns = BEAR_ST_MTIM(*st).tv_nsec;
    fields[QF_MTIME].value = query_data_timestamp(seconds, ns);
    fields[QF_MTIME_SECONDS].value = jdv_value_i64(seconds);
    fields[QF_MTIME_NS].value = jdv_value_u64((uint64_t)ns);
    fields[QF_TIME_OVERFLOW].value = jdv_value_bool(fields[QF_MTIME].value.kind == JDV_NOTHING);
    return value;
}

#endif
