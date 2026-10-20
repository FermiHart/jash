// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_DIFF_QUERY_H
#define JASH_BUILTINS_DIFF_QUERY_H

QUERY_DATA_SCALAR(DIFF_Q_U64, JDV_U64, 0);
QUERY_DATA_SCALAR(DIFF_Q_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(DIFF_Q_PATH, JDV_PATH, 0);
QUERY_DATA_SCALAR(DIFF_Q_SYMBOL, JDV_SYMBOL, 0);
QUERY_DATA_SCALAR(DIFF_Q_BYTES, JDV_BYTES, 0);
QUERY_DATA_SCALAR(DIFF_Q_CHECKPOINT, JDV_BYTES, 1);
static const jdv_schema_t DIFF_Q_LINES = {.kind = JDV_SCHEMA_LIST, .as.element = &DIFF_Q_BYTES};
static const jdv_schema_field_t DIFF_SIDE_FIELDS[] = {
    QUERY_DATA_FIELD("count", &DIFF_Q_U64),
    QUERY_DATA_FIELD("final_newline", &DIFF_Q_BOOL),
    QUERY_DATA_FIELD("path", &DIFF_Q_PATH),
    QUERY_DATA_FIELD("start", &DIFF_Q_U64)
};
static const jdv_schema_t DIFF_SIDE_SCHEMA = {.kind = JDV_SCHEMA_RECORD, .as.record = {DIFF_SIDE_FIELDS, 4, 0}};
static const jdv_schema_field_t DIFF_QUERY_FIELDS[] = {
    QUERY_DATA_FIELD("a", &DIFF_SIDE_SCHEMA),
    QUERY_DATA_FIELD("a_lines", &DIFF_Q_LINES),
    QUERY_DATA_FIELD("b", &DIFF_SIDE_SCHEMA),
    QUERY_DATA_FIELD("b_lines", &DIFF_Q_LINES),
    QUERY_DATA_FIELD("checkpoint", &DIFF_Q_CHECKPOINT),
    QUERY_DATA_FIELD("complete", &DIFF_Q_BOOL),
    QUERY_DATA_FIELD("hunk_index", &DIFF_Q_U64),
    QUERY_DATA_FIELD("op", &DIFF_Q_SYMBOL)
};
static const jdv_schema_t DIFF_QUERY_SCHEMA = {.kind = JDV_SCHEMA_RECORD, .as.record = {DIFF_QUERY_FIELDS, 8, 0}};

static int diff_native_emit(jash_writer_t writer, void* context, const diff_hunk_t* hunks,
                             int number, const diff_file_t* a, const diff_file_t* b) {
    size_t first = diff_query->offset < (uint64_t)number ? (size_t)diff_query->offset : (size_t)number;
    size_t count = (size_t)number - first;
    if ((uint64_t)count > diff_query->limit) count = (size_t)diff_query->limit;
    int capped = count > QUERY_DATA_MAX_ITEMS;
    if (capped) count = QUERY_DATA_MAX_ITEMS;
    size_t line_count = 0;
    for (size_t i = 0; i < count; i++) line_count += (size_t)hunks[first + i].a_count + (size_t)hunks[first + i].b_count;
    jdv_value_t* lines = line_count ? calloc(line_count, sizeof(*lines)) : NULL;
    jdv_value_t* items = count ? calloc(count, sizeof(*items)) : NULL;
    jdv_record_field_t (*fields)[8] = count ? calloc(count, sizeof(*fields)) : NULL;
    jdv_record_field_t (*sides)[2][4] = count ? calloc(count, sizeof(*sides)) : NULL;
    if ((line_count && !lines) || (count && (!items || !fields || !sides))) {
        free(lines); free(items); free(fields); free(sides);
        return query_data_error(writer, context, &DIFF_QUERY_SCHEMA, "allocation_failed", 2);
    }
    size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        const diff_hunk_t* hunk = &hunks[first + i];
        items[i] = query_data_record(fields[i], &DIFF_QUERY_SCHEMA);
        const diff_file_t* source[2] = {a, b};
        const int starts[2] = {hunk->a_start, hunk->b_start};
        const int lengths[2] = {hunk->a_count, hunk->b_count};
        const char* paths[2] = {diff_query->logical_path ? diff_query->logical_path : diff_query->path_a,
                              diff_query->logical_path ? diff_query->logical_path : diff_query->path_b};
        for (size_t side = 0; side < 2; side++) {
            fields[i][side * 2u].value = query_data_record(sides[i][side], &DIFF_SIDE_SCHEMA);
            sides[i][side][0].value = jdv_value_u64((uint64_t)lengths[side]);
            sides[i][side][1].value = jdv_value_bool(source[side]->has_final_nl);
            sides[i][side][2].value = query_data_text(JDV_PATH, paths[side]);
            sides[i][side][3].value = jdv_value_u64((uint64_t)starts[side]);
            jdv_value_t list = {.kind = JDV_LIST};
            list.as.list.items = lines ? &lines[used] : NULL;
            list.as.list.len = (size_t)lengths[side];
            for (int line = 0; line < lengths[side]; line++) {
                const diff_line_t* observed = &source[side]->lines[starts[side] + line];
                lines[used++] = query_data_slice(JDV_BYTES, observed->ptr, observed->len);
            }
            fields[i][side * 2u + 1u].value = list;
        }
        if (diff_query->checkpoint) fields[i][4].value = query_data_text(JDV_BYTES, diff_query->checkpoint);
        fields[i][5].value = jdv_value_bool(1);
        fields[i][6].value = jdv_value_u64(first + i);
        fields[i][7].value = query_data_text(JDV_SYMBOL,
            hunk->a_count && hunk->b_count ? "change" : hunk->a_count ? "delete" : "insert");
    }
    int partial = count < (size_t)number - first;
    int cancelled = diff_query_poll();
    int result = query_data_emit(writer, context, &DIFF_QUERY_SCHEMA, items, count, 0,
        cancelled ? diff_query->cancelled : capped ? 75 : number ? 1 : 0,
        cancelled ? JDV_STREAM_CANCELLED : partial ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE,
        cancelled ? "request_cancelled" : capped ? "hunk_limit" : partial ? "page_limit" : NULL);
    free(lines); free(items); free(fields); free(sides);
    return result;
}

static int diff_query_error(jash_writer_t writer, void* context, int status) {
    const char* code = diff_query->error ? diff_query->error : "invalid_arguments";
    if (diff_query->native) {
        if (diff_query_poll())
            return query_data_emit(writer, context, &DIFF_QUERY_SCHEMA, NULL, 0, 0,
                diff_query->cancelled, JDV_STREAM_CANCELLED, "request_cancelled");
        return query_data_error(writer, context, &DIFF_QUERY_SCHEMA, code, status);
    }
    return jash_structured_error(writer, context, diff_query->command ? diff_query->command : "diff", code,
        diff_query_poll() ? diff_query->cancelled : status);
}

static int diff_query_number(const char* text, uint64_t* number) {
    if (!text || !*text) return -1;
    *number = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || *number > (UINT64_MAX - (unsigned)(*text - '0')) / 10u) return -1;
        *number = *number * 10u + (unsigned)(*text - '0');
    }
    return 0;
}

#endif
