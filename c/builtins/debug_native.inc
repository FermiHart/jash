// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "analysis_output.h"
#include "semantic_events.h"
#include "query_snapshot.h"
#include "debug_tokens.h"
#include "eval_output.h"

#define DEBUG_NATIVE_TOKENS 4096u
#define DEBUG_NATIVE_STATEMENTS 128u
#define DEBUG_NATIVE_SCRIPT_BYTES 65535u

typedef struct { semantic_value_t* sem; jdv_value_t* values; size_t count; } debug_native_tokens_t;
static int debug_native_token(void* context, const char* text, size_t length, size_t offset) {
    debug_native_tokens_t* tokens = context;
    if (tokens->count == DEBUG_NATIVE_TOKENS) { errno = E2BIG; return -1; }
    jdv_record_input_t fields[] = {
        semantic_field("text", semantic_text_n(tokens->sem, text, length)),
        semantic_field("byte_offset", jdv_value_u64(offset)),
        semantic_field("byte_length", jdv_value_filesize(length))
    };
    tokens->values[tokens->count++] = semantic_record(tokens->sem, fields, 3);
    return tokens->sem->error ? -1 : 0;
}

static int debug_native_parse(semantic_builtin_t* call, const char* line) {
    jdv_value_t* values = calloc(DEBUG_NATIVE_TOKENS, sizeof(*values));
    if (!values) return semantic_error(call, "allocation_failed", 125);
    semantic_value_t* sem = &call->values;
    sem->limits.max_record_fields = 16384;
    debug_native_tokens_t tokens = {sem,values,0}; int sq = 0, dq = 0;
    if (debug_scan_tokens(line, debug_native_token, &tokens, &sq, &dq)) {
        free(values); return semantic_error(call, errno == E2BIG ? "token_limit" : "semantic_value_limit", errno == E2BIG ? 75 : 125);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.debug.v1")),
        semantic_field("operation", semantic_symbol(sem, "parse")),
        semantic_field("input", semantic_text(sem, line)),
        semantic_field("tokens", semantic_list(sem, values, tokens.count)),
        semantic_field("single_quote_open", jdv_value_bool(sq)),
        semantic_field("double_quote_open", jdv_value_bool(dq)),
        semantic_field("analysis_scope", semantic_symbol(sem, "quote_aware_space_tab_scanner_not_shell_parser")),
        semantic_field("executed", jdv_value_bool(0))
    };
    jdv_value_t value = semantic_record(sem, fields, 8);
    free(values); return semantic_finish(call, value, 0);
}

static int debug_native_locals(semantic_builtin_t* call, jash_state_t* state) {
    const char* recorded = jash_env_get(state, "__LOCAL_VARS__");
    if (!recorded) recorded = "";
    if (strlen(recorded) > 65536u) return semantic_error(call, "local_names_byte_limit", 75);
    char* names = strdup(recorded);
    if (!names) return semantic_error(call, "allocation_failed", 125);
    const char* keys[256], *values[256]; size_t count = 0, bytes = 0;
    char* save = NULL;
    for (char* key = strtok_r(names, " \t", &save); key; key = strtok_r(NULL, " \t", &save)) {
        if (count == 256u) { free(names); return semantic_error(call, "local_count_limit", 75); }
        const char* value = jash_env_get(state, key); size_t size = value ? strlen(value) : 0;
        if (size > 65536u || size + strlen(key) > 512u * 1024u - bytes) {
            free(names); return semantic_error(call, "local_value_limit", 75);
        }
        bytes += size + strlen(key); keys[count] = key; values[count++] = value;
    }
    semantic_value_t* sem = &call->values; jdv_value_t entries[256];
    for (size_t i = 0; i < count; i++) {
        jdv_record_input_t fields[] = {
            semantic_field("name", semantic_text(sem, keys[i])),
            semantic_field("set", jdv_value_bool(values[i] != NULL)),
            semantic_field("value", semantic_text(sem, values[i]))
        };
        entries[i] = semantic_record(sem, fields, 3);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.debug.v1")),
        semantic_field("operation", semantic_symbol(sem, "locals")),
        semantic_field("source", semantic_symbol(sem, "__LOCAL_VARS___and_current_shell_environment")),
        semantic_field("locals", semantic_list(sem, entries, count)),
        semantic_field("complete", jdv_value_bool(1)),
        semantic_field("total", jdv_value_u64(count))
    };
    jdv_value_t value = semantic_record(sem, fields, 6);
    free(names); return semantic_finish(call, value, 0);
}

typedef struct { char* text; uint64_t first, last; } debug_native_statement_t;
static void debug_native_plan_free(debug_native_statement_t* plan, size_t count) {
    for (size_t i = 0; i < count; i++) free(plan[i].text);
}
static int debug_native_plan(char* text, size_t bytes, debug_native_statement_t* plan,
        size_t* count, const char** error) {
    *count = 0; size_t offset = 0; uint64_t line = 1;
    while (offset < bytes) {
        if (*count == DEBUG_NATIVE_STATEMENTS) { *error = "statement_limit"; return 75; }
        size_t start = offset, end = offset; uint64_t first = line; int depth = 0, block = 0;
        do {
            size_t length = 0;
            while (offset + length < bytes && text[offset + length] != '\n') length++;
            if (length > 4095u) { *error = "line_byte_limit"; return 75; }
            char scratch[4096]; memcpy(scratch, text + offset, length); scratch[length] = 0;
            const char* trimmed = scratch;
            while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
            if (offset == start) { const char* close = NULL; block = find_open_kw(trimmed, &close) != NULL; }
            if (block) apply_line_depth(trimmed, &depth);
            end = offset + length; offset = end + (end < bytes);
            if (!block || depth <= 0) break;
            line++;
            if (offset == bytes) { *error = "unclosed_block"; return 2; }
        } while (offset < bytes);
        size_t length = end - start;
        char* copy = malloc(length + 1u);
        if (!copy) { *error = "allocation_failed"; return 125; }
        memcpy(copy, text + start, length); copy[length] = 0;
        plan[(*count)++] = (debug_native_statement_t){copy,first,line}; line++;
    }
    return 0;
}

static jdv_value_t debug_native_profile_header(semantic_value_t* sem, const char* path, size_t bytes, size_t count) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.debug.profile.v1")),
        semantic_field("kind", semantic_symbol(sem, "header")),
        semantic_field("path", semantic_path(sem, path)),
        semantic_field("source_bytes", jdv_value_filesize(bytes)),
        semantic_field("statements_total", jdv_value_u64(count)),
        semantic_field("source_consistency", semantic_symbol(sem, "named_file_metadata_before_and_after")),
        semantic_field("grouping", semantic_symbol(sem, "registered_debug_block_heuristic")),
        semantic_field("execution_scope", semantic_symbol(sem, "current_shell_state")),
        semantic_field("command_output_destination", semantic_symbol(sem, "stderr"))
    };
    return semantic_record(sem, fields, 9);
}
static jdv_value_t debug_native_profile_step(semantic_value_t* sem, const debug_native_statement_t* statement,
        uint64_t index, int64_t duration, const jash_eval_observation_t* observed, int clock_error) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.debug.profile.v1")),
        semantic_field("kind", semantic_symbol(sem, "statement")),
        semantic_field("index", jdv_value_u64(index)),
        semantic_field("first_line", jdv_value_u64(statement->first)),
        semantic_field("last_line", jdv_value_u64(statement->last)),
        semantic_field("command", semantic_text(sem, statement->text)),
        semantic_field("attempted", jdv_value_bool(observed->attempted)),
        semantic_field("command_status", observed->attempted ? jdv_value_i64(observed->command_status) : jdv_value_nothing()),
        semantic_field("duration_ns", clock_error ? jdv_value_nothing() : jdv_value_duration(duration)),
        semantic_field("clock_errno", clock_error ? jdv_value_i64(clock_error) : jdv_value_nothing()),
        semantic_field("adapter_errno", observed->adapter_error ? jdv_value_i64(observed->adapter_error) : jdv_value_nothing()),
        semantic_field("writer_errno", observed->writer_error ? jdv_value_i64(observed->writer_error) : jdv_value_nothing()),
        semantic_field("restore_errno", observed->restore_error ? jdv_value_i64(observed->restore_error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 13);
}
static jdv_value_t debug_native_profile_summary(semantic_value_t* sem, size_t executed,
        size_t failed, int complete, int stopped, const char* error) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.debug.profile.v1")),
        semantic_field("kind", semantic_symbol(sem, "summary")),
        semantic_field("executed", jdv_value_u64(executed)),
        semantic_field("failed_commands", jdv_value_u64(failed)),
        semantic_field("complete", jdv_value_bool(complete)),
        semantic_field("shell_control_requested", jdv_value_bool(stopped)),
        semantic_field("error", semantic_symbol(sem, error))
    };
    return semantic_record(sem, fields, 7);
}

static int debug_native_profile(semantic_builtin_t* call, jash_state_t* state, const char* path) {
    jash_query_file_page_t page = {0};
    int status = jash_query_file_page(state, path, 0, DEBUG_NATIVE_SCRIPT_BYTES, &page);
    const char* error = page.error;
    if (!status && (uint64_t)page.metadata.st_size > DEBUG_NATIVE_SCRIPT_BYTES) { status = 75; error = "script_byte_limit"; }
    if (!status && memchr(page.bytes, 0, page.used)) { status = 2; error = "embedded_nul_not_supported"; }
    debug_native_statement_t plan[DEBUG_NATIVE_STATEMENTS] = {0}; size_t count = 0;
    if (!status) status = debug_native_plan((char*)page.bytes, page.used, plan, &count, &error);
    if (status) { free(page.bytes); debug_native_plan_free(plan, count); return semantic_error(call, error ? error : "source_failed", status); }
    semantic_value_t* sem = &call->values; jdv_value_t possible[DEBUG_NATIVE_STATEMENTS + 2u];
    possible[0] = debug_native_profile_header(sem, path, page.used, count);
    jash_eval_observation_t worst = {.attempted = 1,.command_status = INT_MIN,
        .adapter_error = INT_MAX,.writer_error = INT_MAX,.restore_error = INT_MAX};
    for (size_t i = 0; i < count; i++) possible[i + 1u] = debug_native_profile_step(sem, &plan[i], i,
        INT64_MAX, &worst, 0);
    possible[count + 1u] = debug_native_profile_summary(sem, count, count, 0, 1, "evaluation_or_clock_observation_failed");
    status = semantic_events_preflight(call, possible, count + 2u);
    if (status) {
        free(page.bytes); debug_native_plan_free(plan, count);
        return semantic_error(call, "receipt_admission_failed", status);
    }
    if (semantic_admit_rows(call, count + 2u)) { free(page.bytes); debug_native_plan_free(plan, count); return 125; }
    sem->arena.used = 0; sem->error = 0;
    int delivery_failed = 0;
    if (semantic_events_emit(call, debug_native_profile_header(sem, path, page.used, count))) { status = 125; delivery_failed = 1; }
    size_t executed = 0, failed = 0; int stopped = 0;
    for (size_t i = 0; i < count && !status; i++) {
        jash_eval_observation_t observed = {0}; struct timespec before = {0}, after = {0};
        int clock_error = clock_gettime(CLOCK_MONOTONIC, &before) ? errno : 0;
        if (!clock_error) (void)jash_eval_with_output(state, plan[i].text, STDERR_FILENO, -1, &observed);
        if (!clock_error && clock_gettime(CLOCK_MONOTONIC, &after)) clock_error = errno;
        int64_t duration = 0;
        if (!clock_error) {
            int64_t seconds = (int64_t)after.tv_sec - (int64_t)before.tv_sec;
            if (seconds < 0 || seconds > INT64_MAX / 1000000000) clock_error = EOVERFLOW;
            else {
                duration = seconds * 1000000000;
                int64_t adjustment = after.tv_nsec - before.tv_nsec;
                if (adjustment > 0 && duration > INT64_MAX - adjustment) clock_error = EOVERFLOW;
                else { duration += adjustment; if (duration < 0) clock_error = EOVERFLOW; }
            }
        }
        executed += observed.attempted != 0; failed += observed.attempted && observed.command_status != 0;
        sem->arena.used = 0; sem->error = 0;
        if (semantic_events_emit(call, debug_native_profile_step(sem, &plan[i], i, duration, &observed, clock_error))) {
            status = 125; delivery_failed = 1; break;
        }
        if (observed.adapter_error || clock_error) { error = "evaluation_or_clock_observation_failed"; status = 125; }
        if (state->exit_requested || state->return_requested || jash_loopctl_pending(state)) {
            stopped = 1;
            if (i + 1u < count && !status) { status = 75; error = "shell_control_stopped_profile"; }
            break;
        }
    }
    if (!delivery_failed) {
        sem->arena.used = 0; sem->error = 0;
        if (semantic_events_emit(call, debug_native_profile_summary(sem, executed, failed, !status, stopped, error))) status = 125;
        else status = semantic_events_end(call, status);
    }
    free(page.bytes); debug_native_plan_free(plan, count); return status;
}

static int debug_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    if (argc == 2 && !strcmp(argv[0], "--parse")) return debug_native_parse(call, argv[1]);
    if (argc == 1 && !strcmp(argv[0], "--locals")) return debug_native_locals(call, state);
    if (argc >= 2 && !strcmp(argv[0], "--profile")) {
        const char* path = argc == 3 && !strcmp(argv[1], "--") ? argv[2] : argc == 2 && argv[1][0] != '-' ? argv[1] : NULL;
        if (path && *path) return debug_native_profile(call, state, path);
    }
    if (argc && !strcmp(argv[0], "--trace")) return semantic_error(call, "interactive_trace_requires_terminal_control", 2);
    return semantic_error(call, "invalid_arguments", 2);
}
SEMANTIC_ARGV_WRAPPER(debug, "debug", "jash.debug.v1", debug_native_provider)
