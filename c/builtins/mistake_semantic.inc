// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_store.h"

static int mistake_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "list";
    int log = !strcmp(operation, "log"), clear = !strcmp(operation, "clear");
    if ((!log && !clear && strcmp(operation, "list") && strcmp(operation, "avoid")) ||
        (log && argc != 2 && (argc != 4 || strcmp(argv[2], "--context"))) || (!log && argc > 1))
        return semantic_error(call, "invalid_arguments", 2);
    char path[4096];
    if (semantic_home_path(path, sizeof(path), "mistakes.log")) return semantic_error(call, "store_path_limit", 1);
    semantic_value_t* sem = &call->values;
    if (log || clear) {
        const char* error = log ? argv[1] : ""; const char* context = log && argc == 4 ? argv[3] : "";
        /* The legacy store has no escape grammar. Refuse ambiguous record
         * separators explicitly; never flatten or silently lose user bytes. */
        if ((log && !*error) || strlen(error) > 4095u || strlen(context) > 4095u ||
            strpbrk(error, "\r\n") || strpbrk(context, "\r\n") || strstr(error, " | ") || strstr(context, " | "))
            return semantic_error(call, "legacy_record_separator_or_field_limit", 2);
        char timestamp[64], date[64], buffer[8320];
        if (semantic_timestamp(timestamp, date)) return semantic_error(call, "clock_unavailable", 1);
        int length = log ? snprintf(buffer, sizeof(buffer), "%s | %s | %s\n", timestamp, error, context) : 0;
        if (length < 0 || (size_t)length >= sizeof(buffer)) return semantic_error(call, "store_record_limit", 75);
        jdv_record_input_t details[] = {
            semantic_field("error", log ? semantic_text(sem, error) : jdv_value_nothing()),
            semantic_field("context", log ? semantic_text(sem, context) : jdv_value_nothing()),
            semantic_field("timestamp_local", log ? semantic_text(sem, timestamp) : jdv_value_nothing()),
            semantic_field("bytes_written", jdv_value_filesize((uint64_t)length))
        };
        jdv_value_t candidate = semantic_record(sem, details, 4);
        int preflight = semantic_store_preflight(call, operation, path, candidate);
        if (preflight) return semantic_error(call, "receipt_limit_before_store", preflight);
        if (semantic_admit(call)) return 125;
        int failed = semantic_store_directory("") || semantic_write_file(path, buffer, (size_t)length, log);
        if (failed) details[3].value = jdv_value_nothing();
        return semantic_store_receipt(call, operation, path, semantic_record(sem, details, 4), failed ? 1 : 0);
    }
    char* data = NULL; size_t bytes = 0; int exists;
    if (semantic_read_file(path, SEMANTIC_STORE_BYTES, &data, &bytes, &exists)) return semantic_error(call, "store_read_failed", 1);
    jdv_value_t* items = calloc(SEMANTIC_STORE_ROWS, sizeof(*items));
    if (!items) { free(data); return semantic_error(call, "allocation_failed", 125); }
    size_t used = 0, offset = 0; int failed = bytes && memchr(data, 0, bytes);
    while (!failed && offset < bytes) {
        size_t start = offset; while (offset < bytes && data[offset] != '\n') offset++;
        size_t length = offset - start; if (offset < bytes) data[offset++] = 0;
        char* line = data + start;
        if (length && line[length - 1] == '\r') line[--length] = 0;
        if (!length) continue;
        if (used == SEMANTIC_STORE_ROWS || length > 8319u) { failed = 1; break; }
        char* first = strstr(line, " | "); char* second = first ? strstr(first + 3, " | ") : NULL;
        if (!first || !second || strstr(second + 3, " | ")) { failed = 1; break; }
        *first = 0; *second = 0;
        jdv_record_input_t fields[] = {
            semantic_field("timestamp_local", semantic_text(sem, line)),
            semantic_field("error", semantic_text(sem, first + 3)),
            semantic_field("context", semantic_text(sem, second + 3))
        };
        items[used++] = semantic_record(sem, fields, 3);
    }
    jdv_value_t records = semantic_list(sem, items, used); free(items); free(data);
    if (failed) return semantic_error(call, "invalid_or_oversized_mistake_store", 1);
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("source", semantic_path(sem, path)),
        semantic_field("exists", jdv_value_bool(exists)),
        semantic_field("analysis", semantic_symbol(sem, "stored-observations")),
        semantic_field("mistakes", records)
    };
    return semantic_finish(call, semantic_record(sem, fields, 6), 0);
}
