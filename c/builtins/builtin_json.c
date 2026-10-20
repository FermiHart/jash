// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/json — structured adapters for legacy CLIs.
//
//   json git status              → JSON status object
//   json git log [N]             → JSON commit array
//   json git diff --stat         → JSON diff stat array
//   json make [target]           → JSON build result (errors/warnings)
//   json cmake --build <dir>     → JSON build result
//   json npm test                → JSON test result
//
// See Docs/ADR-0026-agent-experience-wave.md (Tier 1: CLI adapters)

#include "builtin.h"
#include "value_output.h"
#include <core/argv.h>
#include <core/data_wire_import.h>
#include <core/json_writer.h>
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/wait.h>

#define JSON_MAX_LINE 8192
#define JSON_MAX_FILES 512
#define JSON_MAX_COMMITS 128
#define JSON_MAX_DIAGS 256
#define JSON_MAX_CAPTURE (8u * 1024u * 1024u)

/* Explicit semantic bridge. Input is fully checked before any output byte;
 * a JSON array is one LIST value unless --jsonl selects record framing. */
static int json_data_bridge(jash_writer_t writer, void* context, int from_data, int jsonl) {
    const size_t input_max = 16u * 1024u * 1024u, arena_max = 64u * 1024u * 1024u;
    unsigned char* input = NULL;
    void* storage = NULL; void* schemas = NULL; void* item_storage = NULL;
    jdv_value_t* values = NULL;
    jdv_wire_import_t imported = {0};
    jash_structured_buffer_t buffer;
    jash_structured_buffer_init(&buffer, arena_max);
    int rc = 2;
    size_t capacity = 65536u, size = 0, count = 0;
    input = malloc(capacity + 1u); storage = malloc(arena_max);
    if (!input || !storage) { rc = 125; goto done; }
    for (;;) {
        if (size == capacity) {
            if (capacity == input_max) {
                int extra = fgetc(stdin);
                if (extra != EOF || ferror(stdin)) { errno = E2BIG; goto done; }
                break;
            }
            capacity *= 2u;
            unsigned char* next = realloc(input, capacity + 1u);
            if (!next) { rc = 125; goto done; }
            input = next;
        }
        size_t got = fread(input + size, 1, capacity - size, stdin);
        size += got;
        if (!got) { if (ferror(stdin)) { errno = EIO; goto done; } break; }
    }
    input[size] = 0;
    jdv_arena_t arena; jdv_arena_init(&arena, storage, arena_max);
    jdv_limits_t limits = JDV_DEFAULT_LIMITS;
    limits.max_depth = 64; limits.max_nodes = 65536;
    limits.max_aggregate_bytes = 2u * 1024u * 1024u;
    limits.max_encoded_bytes = 2u * 1024u * 1024u;
    limits.max_list_items = 65536; limits.max_record_fields = 4096;
    limits.max_schema_fields = 4096;
    if (from_data) {
        schemas = malloc(2u * 1024u * 1024u); item_storage = malloc(4u * 1024u * 1024u);
        if (!schemas || !item_storage) { rc = 125; goto done; }
        jdv_wire_limits_t wire; jdv_wire_limits_init(&wire);
        wire.max_wire_bytes = input_max; wire.max_frame_bytes = 2u * 1024u * 1024u;
        wire.max_items = 65536; wire.value_limits = limits;
        if (jdv_wire_import_v2_open(&imported, input, size, schemas, 2u * 1024u * 1024u,
                item_storage, 4u * 1024u * 1024u, &wire)) goto done;
        jash_json_writer_t output;
        jash_jsonw_init(&output, jash_structured_buffer_sink, &buffer);
        if (from_data == 2) jash_jsonw_raw(&output, "{\"schema\":\"jash.data.projection.v1\",\"items\":[");
        for (;;) {
            jdv_stream_record_t record;
            jdv_stream_pull_result_t result = jdv_stream_pull(&imported.cursor, &record);
            if (result == JDV_PULL_ITEM) {
                arena.used = 0;
                jdj_value_t json;
                if (from_data == 2 && count) jash_jsonw_raw(&output, ",");
                if (jdj_from_value(&arena, record.item, &limits, &json) ||
                    jdj_write(&output, &json, &limits)) goto done;
                if (from_data != 2) jash_jsonw_raw(&output, "\n");
                if (output.error) goto done;
                count++;
            } else {
                const jdv_stream_end_t* end = record.terminal;
                if (!end || (result != JDV_PULL_END && result != JDV_PULL_ERROR) ||
                    (from_data != 2 && (result != JDV_PULL_END || end->state != JDV_STREAM_COMPLETE ||
                    end->completeness != JDV_COMPLETENESS_COMPLETE || end->truncated || end->errors_seen ||
                    !end->final_hash_present || end->producer_status || end->transform_status || end->transport_status))) {
                    errno = EPROTO; goto done;
                }
                if (from_data == 2) {
                    static const char* states[] = {"complete", "cancelled", "failed", "incomplete"};
                    static const char* completeness[] = {"unknown", "complete", "partial"};
                    if ((unsigned)end->state >= sizeof(states) / sizeof(states[0]) ||
                        (unsigned)end->completeness >= sizeof(completeness) / sizeof(completeness[0])) { errno = EPROTO; goto done; }
                    jash_jsonw_raw(&output, "],\"terminal\":{\"state\":"); jash_jsonw_string(&output, states[end->state]);
                    jash_jsonw_raw(&output, ",\"completeness\":"); jash_jsonw_string(&output, completeness[end->completeness]);
                    jash_jsonw_raw(&output, ",\"truncated\":"); jash_jsonw_raw(&output, end->truncated ? "true" : "false");
                    jash_jsonw_raw(&output, ",\"items\":"); jash_jsonw_u64(&output, end->items_seen);
                    jash_jsonw_raw(&output, ",\"logical_bytes\":"); jash_jsonw_u64(&output, end->bytes_seen);
                    jash_jsonw_raw(&output, ",\"errors\":"); jash_jsonw_u64(&output, end->errors_seen);
                    jash_jsonw_raw(&output, ",\"producer_status\":"); jash_jsonw_i64(&output, end->producer_status);
                    jash_jsonw_raw(&output, ",\"transform_status\":"); jash_jsonw_i64(&output, end->transform_status);
                    jash_jsonw_raw(&output, ",\"transport_status\":"); jash_jsonw_i64(&output, end->transport_status);
                    jash_jsonw_raw(&output, ",\"hash_verified\":"); jash_jsonw_raw(&output, end->final_hash_present ? "true" : "false");
                    jash_jsonw_raw(&output, ",\"hash\":");
                    if (end->final_hash_present) {
                        char hex[JDV_HASH_SIZE * 2u + 1u];
                        static const char digits[] = "0123456789abcdef";
                        for (size_t n = 0; n < JDV_HASH_SIZE; n++) {
                            hex[2u*n] = digits[end->final_hash[n] >> 4]; hex[2u*n+1u] = digits[end->final_hash[n] & 15u];
                        }
                        hex[sizeof(hex)-1u] = 0; jash_jsonw_string(&output, hex);
                    } else jash_jsonw_raw(&output, "null");
                    jash_jsonw_raw(&output, ",\"reason\":");
                    jash_jsonw_text_or_bytes(&output, end->reason.data, end->reason.len);
                    jash_jsonw_raw(&output, "}}\n");
                }
                break;
            }
        }
        if (jash_jsonw_finish(&output)) goto done;
        long budget = jash_budget_limit();
        rc = jash_structured_document(writer, context, "json", buffer.data, buffer.used,
            buffer.required, budget > 0 ? budget : (long)arena_max, 0);
    } else {
        values = calloc(jsonl ? 65536u : 1u, sizeof(*values));
        if (!values) { rc = 125; goto done; }
        if (!jsonl) {
            if (jdv_json_parse(&arena, input, size, &limits, &values[0])) goto done;
            count = 1;
        } else {
            size_t start = 0;
            while (start < size) {
                size_t end = start;
                while (end < size && input[end] != '\n') end++;
                size_t first = start;
                while (first < end && (input[first] == ' ' || input[first] == '\t' || input[first] == '\r')) first++;
                if (first < end) {
                    if (end - start > 65536u || count == 65536u) { errno = E2BIG; goto done; }
                    if (jdv_json_parse(&arena, input + start, end - start, &limits, &values[count])) goto done;
                    count++;
                }
                start = end + 1u;
            }
        }
        jdv_schema_t any = {.kind = JDV_SCHEMA_ANY};
        rc = value_output_data(writer, context, values, count, NULL, &any, &limits, arena_max, 0, NULL);
    }
done:
    if (imported.active) (void)jdv_wire_import_close(&imported);
    free(input); free(storage); free(schemas); free(item_storage); free(values);
    jash_structured_buffer_free(&buffer);
    if (rc) jash_writes(jash_stderr_writer, NULL,
        "json: invalid, incomplete, oversized or unrepresentable structured input/output\n");
    return rc;
}

static int json_next_nul(const char* data, size_t length, size_t* offset,
                         const char** value, size_t* value_length) {
    if (*offset >= length) return 0;
    const char* start = data + *offset;
    const char* end = memchr(start, '\0', length - *offset);
    if (!end) return -1;
    *value = start;
    *value_length = (size_t)(end - start);
    *offset += *value_length + 1u;
    return 1;
}

static char* json_dup_bytes(const char* bytes, size_t length) {
    char* copy = malloc(length + 1u);
    if (!copy) return NULL;
    memcpy(copy, bytes, length);
    copy[length] = '\0';
    return copy;
}

static const char* git_status_name(char x, char y) {
    if (x == '?' || y == '?') return "untracked";
    if (x == 'U' || y == 'U') return "unmerged";
    if (x == 'R' || y == 'R') return "renamed";
    if (x == 'C' || y == 'C') return "copied";
    if (x == 'A' || y == 'A') return "added";
    if (x == 'D' || y == 'D') return "deleted";
    if (x == 'M' || y == 'M') return "modified";
    return "changed";
}

typedef struct {
    char* path;
    char* original_path;
    char x, y;
} git_status_row_t;

static void git_status_rows_free(git_status_row_t* rows, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(rows[i].path);
        free(rows[i].original_path);
    }
}

static int git_parse_count(const char*, size_t, uint64_t*);

static int git_tracking_count(const char* text, const char* key, uint64_t* value) {
    const char* number = strstr(text, key);
    if (!number) return 0;
    number += strlen(key);
    const char* end = number;
    while (isdigit((unsigned char)*end)) end++;
    if ((*end && *end != ',' && *end != ']' && *end != ' ') ||
        git_parse_count(number, (size_t)(end - number), value)) return -1;
    return 0;
}

static int git_status_rows_parse(const char* data, size_t length,
                                 git_status_row_t* rows, size_t* row_count,
                                 char** branch, uint64_t* ahead, uint64_t* behind) {
    size_t offset = 0;
    *row_count = 0;
    while (offset < length) {
        const char *record, *original;
        size_t record_length, original_length;
        int next = json_next_nul(data, length, &offset, &record, &record_length);
        if (next <= 0) return -1;
        if (!record_length) continue;
        if (record_length >= 3 &&
            ((record[0] == '#' && record[1] == ' ') || !memcmp(record, "## ", 3))) {
            if (record_length >= 3 && !memcmp(record, "## ", 3)) {
                char* header = json_dup_bytes(record + 3, record_length - 3u);
                if (!header) return -1;
                char* upstream = strstr(header, "...");
                if (upstream) {
                    *upstream = '\0';
                    char* tracking = strchr(upstream + 3, '[');
                    if (tracking) {
                        if (git_tracking_count(tracking, "ahead ", ahead) ||
                            git_tracking_count(tracking, "behind ", behind)) { free(header); return -1; }
                    }
                } else if (!strncmp(header, "No commits yet on ", 18)) {
                    memmove(header, header + 18, strlen(header + 18) + 1u);
                }
                free(*branch);
                *branch = header;
                continue;
            }
            static const char branch_prefix[] = "# branch.head ";
            static const char ahead_prefix[] = "# branch.ab ";
            if (record_length >= sizeof(branch_prefix) - 1u &&
                !memcmp(record, branch_prefix, sizeof(branch_prefix) - 1u)) {
                free(*branch);
                *branch = json_dup_bytes(record + sizeof(branch_prefix) - 1u,
                    record_length - (sizeof(branch_prefix) - 1u));
                if (!*branch) return -1;
            } else if (record_length >= sizeof(ahead_prefix) - 1u &&
                       !memcmp(record, ahead_prefix, sizeof(ahead_prefix) - 1u)) {
                char numbers[128];
                size_t n = record_length - (sizeof(ahead_prefix) - 1u);
                if (n >= sizeof numbers) return -1;
                memcpy(numbers, record + sizeof(ahead_prefix) - 1u, n);
                numbers[n] = '\0';
                char* separator = strstr(numbers, " -");
                if (numbers[0] != '+' || !separator ||
                    git_parse_count(numbers + 1, (size_t)(separator - numbers - 1), ahead) ||
                    git_parse_count(separator + 2, strlen(separator + 2), behind)) return -1;
            }
            continue;
        }
        if (record_length < 4u || record[2] != ' ' || *row_count >= JSON_MAX_FILES)
            return -1;
        if (!strchr(" MADRCU?!T", record[0]) || !strchr(" MADRCU?!T", record[1])) return -1;
        git_status_row_t* row = &rows[(*row_count)++];
        memset(row, 0, sizeof(*row));
        row->x = record[0]; row->y = record[1];
        row->path = json_dup_bytes(record + 3, record_length - 3u);
        if (!row->path) return -1;
        if ((row->x == 'R' || row->x == 'C' || row->y == 'R' || row->y == 'C')) {
            next = json_next_nul(data, length, &offset, &original, &original_length);
            if (next <= 0) return -1;
            row->original_path = json_dup_bytes(original, original_length);
            if (!row->original_path) return -1;
        }
    }
    return 0;
}

static int git_parse_count(const char* bytes, size_t length, uint64_t* value) {
    if (!length) return -1;
    uint64_t n = 0;
    for (size_t i = 0; i < length; i++) {
        if (bytes[i] < '0' || bytes[i] > '9') return -1;
        uint64_t digit = (uint64_t)(bytes[i] - '0');
        if (n > (UINT64_MAX - digit) / 10u) return -1;
        n = n * 10u + digit;
    }
    *value = n;
    return 0;
}

typedef struct {
    char* path;
    char* old_path;
    uint64_t insertions, deletions;
    int binary;
} git_diff_row_t;

static void git_diff_rows_free(git_diff_row_t* rows, size_t count) {
    for (size_t i = 0; i < count; i++) {
        free(rows[i].path);
        free(rows[i].old_path);
    }
}

static int git_diff_rows_parse(const char* data, size_t length,
                               git_diff_row_t* rows, size_t* row_count) {
    size_t offset = 0;
    *row_count = 0;
    while (offset < length) {
        const char* record;
        size_t record_length;
        int next = json_next_nul(data, length, &offset, &record, &record_length);
        if (next <= 0) return -1;
        const char* first_tab = memchr(record, '\t', record_length);
        if (!first_tab) return -1;
        size_t first_len = (size_t)(first_tab - record);
        const char* second_tab = memchr(first_tab + 1, '\t',
                                       record_length - first_len - 1u);
        if (!second_tab) return -1;
        size_t second_len = (size_t)(second_tab - (first_tab + 1));
        const char* path = second_tab + 1;
        size_t path_len = record_length - (size_t)(path - record);
        const char* old_path = NULL;
        size_t old_length = 0;
        if (!path_len) {
            next = json_next_nul(data, length, &offset, &old_path, &old_length);
            if (next <= 0) return -1;
            next = json_next_nul(data, length, &offset, &path, &path_len);
            if (next <= 0) return -1;
        }
        if (!path_len || (old_path && !old_length)) return -1;
        if (*row_count >= JSON_MAX_FILES) return -1;
        git_diff_row_t* row = &rows[(*row_count)++];
        memset(row, 0, sizeof(*row));
        row->path = json_dup_bytes(path, path_len);
        if (!row->path) return -1;
        if (old_path) {
            row->old_path = json_dup_bytes(old_path, old_length);
            if (!row->old_path) return -1;
        }
        if ((first_len == 1 && record[0] == '-') ||
            (second_len == 1 && first_tab[1] == '-')) {
            if (first_len != 1 || record[0] != '-' || second_len != 1 || first_tab[1] != '-') return -1;
            row->binary = 1;
        } else if (git_parse_count(record, first_len, &row->insertions) < 0 ||
                   git_parse_count(first_tab + 1, second_len, &row->deletions) < 0) {
            return -1;
        }
    }
    return 0;
}

#include "json_cli_native.h"

/* ── dispatch ───────────────────────────────────────────────────────────── */

static void json_usage(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx,
        "json — structured adapters for legacy CLIs\n"
        "Usage:\n"
        "  json --to-data [--jsonl] → JSON document/JSONL to native JTV2\n"
        "  json --from-data        → complete native JTV2 to JSONL\n"
        "  json --inspect-data     → validated JTV2 items and explicit terminal\n"
        "  json git status          → JSON status object\n"
        "  json git log [N]         → JSON commit array\n"
        "  json git diff [REF]      → JSON numstat: per-file + total insertions/deletions\n"
        "  json make [target]       → JSON build diagnostics\n"
        "  json cmake [--build DIR] → JSON build diagnostics (default: --build .)\n"
        "  json npm [script...]     → JSON test result (default: npm test)\n");
}

int jash_b_json_argv(jash_state_t* state, jash_writer_t writer, void* context,
                       int argc, char** argv) {
    (void)state;
    if (!argc) { json_usage(writer, context); return 2; }
    if (!strcmp(argv[0], "--to-data") || !strcmp(argv[0], "--from-data") || !strcmp(argv[0], "--inspect-data")) {
        int from = !strcmp(argv[0], "--from-data") ? 1 : !strcmp(argv[0], "--inspect-data") ? 2 : 0;
        if ((from && argc != 1) || (!from && (argc > 2 || (argc == 2 && strcmp(argv[1], "--jsonl"))))) return 2;
        return json_data_bridge(writer, context, from, argc == 2);
    }
    if (argc == 1 && (!strcmp(argv[0], "help") || !strcmp(argv[0], "-h") || !strcmp(argv[0], "--help"))) {
        json_usage(writer, context); return 0;
    }
    int native = 0;
    if (!strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2") || !strcmp(argv[0], "--json")) {
        native = strcmp(argv[0], "--json") != 0;
        argv++; argc--;
    }
    return cli_adapter(writer, context, native, argc, argv);
}

int jash_b_json(jash_state_t* state, jash_writer_t writer, void* context, const char* args) {
    jash_argv_t parsed = {0};
    if (jash_argv_build(args ? args : "", &parsed) < 0) return 2;
    int result = jash_b_json_argv(state, writer, context, parsed.argc, parsed.argv);
    jash_argv_free(&parsed);
    return result;
}
