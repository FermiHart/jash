// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_TREAD_QUERY_H
#define JASH_BUILTINS_TREAD_QUERY_H

QUERY_DATA_SCALAR(TREAD_Q_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(TREAD_Q_OPTION_BOOL, JDV_BOOL, 1);
QUERY_DATA_SCALAR(TREAD_Q_U64, JDV_U64, 0);
QUERY_DATA_SCALAR(TREAD_Q_OPTION_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(TREAD_Q_SIZE, JDV_FILESIZE, 0);
QUERY_DATA_SCALAR(TREAD_Q_OPTION_SIZE, JDV_FILESIZE, 1);
QUERY_DATA_SCALAR(TREAD_Q_BYTES, JDV_BYTES, 1);
QUERY_DATA_SCALAR(TREAD_Q_PATH, JDV_PATH, 0);
QUERY_DATA_SCALAR(TREAD_Q_SYMBOL, JDV_SYMBOL, 0);
QUERY_DATA_SCALAR(TREAD_Q_ERROR, JDV_SYMBOL, 1);
static const jdv_schema_field_t TREAD_QUERY_FIELDS[] = {
    QUERY_DATA_FIELD("bytes_returned", &TREAD_Q_SIZE),
    QUERY_DATA_FIELD("checkpoint", &TREAD_Q_BYTES),
    QUERY_DATA_FIELD("complete", &TREAD_Q_BOOL),
    QUERY_DATA_FIELD("data", &TREAD_Q_BYTES),
    QUERY_DATA_FIELD("device", &TREAD_Q_OPTION_U64),
    QUERY_DATA_FIELD("eof", &TREAD_Q_OPTION_BOOL),
    QUERY_DATA_FIELD("errno", &TREAD_Q_OPTION_U64),
    QUERY_DATA_FIELD("error", &TREAD_Q_ERROR),
    QUERY_DATA_FIELD("freshness", &TREAD_Q_SYMBOL),
    QUERY_DATA_FIELD("inode", &TREAD_Q_OPTION_U64),
    QUERY_DATA_FIELD("offset", &TREAD_Q_U64),
    QUERY_DATA_FIELD("page_blake3", &TREAD_Q_BYTES),
    QUERY_DATA_FIELD("path", &TREAD_Q_PATH),
    QUERY_DATA_FIELD("snapshot", &TREAD_Q_BOOL),
    QUERY_DATA_FIELD("target_depth", &TREAD_Q_U64),
    QUERY_DATA_FIELD("total_bytes", &TREAD_Q_OPTION_SIZE)
};
static const jdv_schema_t TREAD_QUERY_SCHEMA = {.kind = JDV_SCHEMA_RECORD, .as.record = {TREAD_QUERY_FIELDS, 16, 0}};

static int tread_query_mode(int argc, char** argv) {
    int mode = 0;
    for (int i = 0; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--json") && !mode) mode = 1;
        if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) mode = 2;
        if (!strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit")) i++;
    }
    return mode;
}

static int tread_query_error(jash_state_t* state, jash_writer_t writer, void* context,
                              int native, int is_diff, const char* code, int status) {
    if (is_diff) return jash_diff_query_error(state, writer, context, "tdiff", code, native, status);
    int cancelled = jash_query_cancel_poll(state);
    if (native) {
        if (cancelled) return query_data_emit(writer, context, &TREAD_QUERY_SCHEMA, NULL, 0, 0,
            cancelled, JDV_STREAM_CANCELLED, "request_cancelled");
        return query_data_error(writer, context, &TREAD_QUERY_SCHEMA, code, status);
    }
    return jash_structured_error(writer, context, "tread", cancelled ? "request_cancelled" : code,
        cancelled ? cancelled : status);
}

static int tread_query_number(const char* text, uint64_t* number) {
    if (!text || !*text) return -1;
    *number = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || *number > (UINT64_MAX - (unsigned)(*text - '0')) / 10u) return -1;
        *number = *number * 10u + (unsigned)(*text - '0');
    }
    return 0;
}

static int tread_query_page(jash_state_t* state, jash_writer_t writer, void* context,
                             const char* source, const char* path, const char* checkpoint,
                             int depth, uint64_t offset, size_t limit, int native) {
    jash_query_file_page_t page;
    int status = jash_query_file_page(state, source, offset, limit, &page);
    int eof = !status && offset + page.used == (uint64_t)page.metadata.st_size;
    int result;
    if (native) {
        jdv_record_field_t fields[16];
        jdv_value_t item = query_data_record(fields, &TREAD_QUERY_SCHEMA);
        fields[0].value = jdv_value_filesize(page.used);
        if (checkpoint) fields[1].value = query_data_text(JDV_BYTES, checkpoint);
        fields[2].value = jdv_value_bool(!status);
        fields[8].value = query_data_text(JDV_SYMBOL, "descriptor-stable");
        fields[10].value = jdv_value_u64(offset);
        fields[12].value = query_data_text(JDV_PATH, path);
        fields[13].value = jdv_value_bool(checkpoint != NULL);
        fields[14].value = jdv_value_u64((uint64_t)depth);
        if (!status) {
            fields[3].value = query_data_slice(JDV_BYTES, page.bytes, page.used);
            fields[4].value = jdv_value_u64(page.metadata.st_dev);
            fields[5].value = jdv_value_bool(eof);
            fields[9].value = jdv_value_u64(page.metadata.st_ino);
            fields[11].value = query_data_slice(JDV_BYTES, page.digest, sizeof(page.digest));
            fields[15].value = jdv_value_filesize((uint64_t)page.metadata.st_size);
        } else {
            if (page.error_number) fields[6].value = jdv_value_u64((uint64_t)page.error_number);
            fields[7].value = query_data_text(JDV_SYMBOL, page.error ? page.error : "read_failed");
        }
        result = query_data_emit(writer, context, &TREAD_QUERY_SCHEMA, &item, 1,
            status && status < 128 ? 1 : 0, status,
            status >= 128 ? JDV_STREAM_CANCELLED : status ? JDV_STREAM_FAILED :
            eof ? JDV_STREAM_COMPLETE : JDV_STREAM_INCOMPLETE,
            status ? page.error : eof ? NULL : "page_limit");
    } else if (status) {
        result = tread_query_error(state, writer, context, 0, 0, page.error ? page.error : "read_failed", status);
    } else {
        jash_structured_output_t output;
        if (jash_structured_begin(&output, writer, context) < 0) { free(page.bytes); return 125; }
        jash_json_writer_t* json = &output.json;
        jash_jsonw_raw(json, "{\"schema\":\"jash.tread.v1\",\"path\":"); jash_jsonw_string_or_bytes(json, path);
        jash_jsonw_raw(json, ",\"checkpoint\":");
        if (checkpoint) jash_jsonw_string_or_bytes(json, checkpoint); else jash_jsonw_raw(json, "null");
        jash_jsonw_raw(json, ",\"snapshot\":"); jash_jsonw_raw(json, checkpoint ? "true" : "false");
        jash_jsonw_raw(json, ",\"target_depth\":"); jash_jsonw_safe_u64(json, (uint64_t)depth);
        jash_jsonw_raw(json, ",\"offset\":"); jash_jsonw_safe_u64(json, offset);
        jash_jsonw_raw(json, ",\"bytes_returned\":"); jash_jsonw_safe_u64(json, page.used);
        jash_jsonw_raw(json, ",\"total_bytes\":"); jash_jsonw_safe_u64(json, (uint64_t)page.metadata.st_size);
        jash_jsonw_raw(json, ",\"eof\":"); jash_jsonw_raw(json, eof ? "true" : "false");
        jash_jsonw_raw(json, ",\"complete\":true,\"freshness\":\"descriptor-stable\",\"page_blake3\":");
        jash_jsonw_bytes(json, page.digest, sizeof(page.digest));
        jash_jsonw_raw(json, ",\"data\":"); jash_jsonw_bytes(json, page.bytes, page.used);
        jash_jsonw_raw(json, "}\n");
        result = jash_structured_finish(&output, 0);
    }
    free(page.bytes);
    return result;
}

static int tread_query_run(jash_state_t* state, jash_writer_t writer, void* context,
                            int argc, char** argv, int native, int is_diff) {
    const char* path = NULL;
    const char* checkpoint = NULL;
    const char* error = NULL;
    uint64_t offset = 0, limit = is_diff ? UINT64_MAX : 4096u;
    int end_options = 0, explicit_json = 0, has_page = 0;
    for (int i = 0; i < argc && !error; i++) {
        const char* argument = argv[i];
        if (!end_options && !strcmp(argument, "--")) { end_options = 1; continue; }
        if (!end_options && !strcmp(argument, "--json")) { explicit_json = 1; continue; }
        if (!end_options && (!strcmp(argument, "--data") || !strcmp(argument, "--data-v2"))) continue;
        if (!end_options && (!strcmp(argument, "--offset") || !strcmp(argument, "--limit"))) {
            has_page = 1;
            int is_limit = !strcmp(argument, "--limit");
            uint64_t value;
            if (i + 1 >= argc || tread_query_number(argv[++i], &value) < 0 ||
                (is_limit && (!value || (!is_diff && value > 16384u))) || (is_diff && !native)) {
                error = "invalid_page"; break;
            }
            if (is_limit) limit = value; else offset = value;
            continue;
        }
        if (!end_options && argument[0] == '-' && argument[1]) { error = "unsupported_option"; break; }
        if (!end_options && argument[0] == '@') {
            if (checkpoint || !argument[1] || strlen(argument + 1) >= CP_NAME_SIZE) error = "invalid_checkpoint";
            else checkpoint = argument + 1;
        } else if (!path) path = argument;
        else error = "invalid_operands";
    }
    if (native && explicit_json) error = "conflicting_output_modes";
    int process = checkpoint && !strncmp(checkpoint, "proc:", 5);
    if ((!path && !process) || (is_diff && !checkpoint)) error = "invalid_operands";
    if (path && strlen(path) >= 1024u) error = "path_limit";
    uint64_t process_id = 0;
    if (process) {
        if (tread_query_number(checkpoint + 5, &process_id) < 0 || !process_id || process_id > INT_MAX)
            error = "invalid_process_id";
        else if (is_diff) error = "process_diff_unavailable";
        else if (has_page) error = "process_page_unavailable";
    }
    if (error) return tread_query_error(state, writer, context, native, is_diff, error, 2);
    if (process) return jash_process_tree_report(state, writer, context, "tread", native, (int)process_id);
    int depth = jash_timeline_depth();
    if (checkpoint) {
        int found = checkpoint_depth(checkpoint, &depth);
        if (found != 1) return tread_query_error(state, writer, context, native, is_diff,
            found < 0 ? "checkpoint_read_failed" : "checkpoint_not_found", 1);
    }
    int cancelled = jash_query_cancel_poll(state);
    if (cancelled) return tread_query_error(state, writer, context, native, is_diff, "request_cancelled", cancelled);
    char source[4096];
    if (jash_timeline_backup_at(path, depth, source, sizeof(source)) != 0)
        return tread_query_error(state, writer, context, native, is_diff, "snapshot_unavailable", 1);
    if (is_diff) return jash_diff_query_snapshot(state, writer, context, source, path, path, checkpoint,
        native, offset, limit);
    return tread_query_page(state, writer, context, source, path, checkpoint, depth, offset, (size_t)limit, native);
}

#endif
