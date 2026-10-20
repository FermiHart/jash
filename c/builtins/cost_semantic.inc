// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_builtin.h"
#include <signal.h>
#include <time.h>

static int cost_native_run(char* const* argv, uint64_t* bytes, int* command_status, int* started) {
    *started = 0;
    int pipefd[2];
    if (pipe(pipefd)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }
    if (!pid) {
        (void)signal(SIGPIPE, SIG_DFL);
        close(pipefd[0]);
        if (dup2(pipefd[1], STDOUT_FILENO) < 0 || dup2(pipefd[1], STDERR_FILENO) < 0) _exit(126);
        close(pipefd[1]); execvp(argv[0], argv); _exit(errno == ENOENT ? 127 : 126);
    }
    *started = 1; close(pipefd[1]); *bytes = 0; int failed = 0;
    char data[8192];
    for (;;) {
        ssize_t count = read(pipefd[0], data, sizeof(data));
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { failed = 1; break; }
        if (!count) break;
        if ((uint64_t)count > UINT64_MAX - *bytes) failed = 1;
        else *bytes += (uint64_t)count;
    }
    if (close(pipefd[0])) failed = 1;
    int status = 0; pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != pid) { *command_status = 125; return -1; }
    *command_status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
    return failed ? -1 : 0;
}

/* The JDV1 primitive set deliberately has no decimal kind. This factored
 * rational record is exact even when value*coefficient would overflow U64. */
static jdv_value_t cost_scaled(semantic_value_t* sem, const char* units,
        uint64_t value, uint64_t coefficient, uint64_t denominator) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.estimate.scaled-integer.v1")),
        semantic_field("units", semantic_symbol(sem, units)),
        semantic_field("value", jdv_value_u64(value)),
        semantic_field("coefficient", jdv_value_u64(coefficient)),
        semantic_field("denominator", jdv_value_u64(denominator))
    };
    return semantic_record(sem, fields, 5);
}

static jdv_value_t cost_semantic_value(semantic_builtin_t* call, jdv_value_t arguments,
        uint64_t bytes, int64_t duration, int dry, int executed, int command_status, int adapter_status) {
    semantic_value_t* sem = &call->values; uint64_t tokens = bytes / 4u;
    jdv_record_input_t policy[] = {
        semantic_field("source", semantic_symbol(sem, "legacy-fixed-estimate-policy")),
        semantic_field("current_market_price", jdv_value_bool(0)),
        semantic_field("input_usd_per_million_tokens", jdv_value_u64(30)),
        semantic_field("output_usd_per_million_tokens", jdv_value_u64(60)),
        semantic_field("basis", semantic_symbol(sem, "same-estimated-token-count-for-input-and-output"))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("argv", arguments), semantic_field("executed", jdv_value_bool(executed)),
        semantic_field("basis", semantic_symbol(sem, dry ? "argv-bytes-lower-bound" : "combined-child-output-bytes")),
        semantic_field("bytes", jdv_value_filesize(bytes)),
        semantic_field("tokens_estimated", jdv_value_u64(tokens)),
        semantic_field("token_estimator", semantic_symbol(sem, "bytes-divided-by-four")),
        semantic_field("duration_ns", duration >= 0 ? jdv_value_duration(duration) : jdv_value_nothing()),
        semantic_field("seconds", duration >= 0 ? cost_scaled(sem, "seconds", (uint64_t)duration, 1, 1000000000u) : jdv_value_nothing()),
        semantic_field("input_usd", cost_scaled(sem, "USD", tokens, 30, 1000000)),
        semantic_field("output_usd", cost_scaled(sem, "USD", tokens, 60, 1000000)),
        semantic_field("total_usd", cost_scaled(sem, "USD", tokens, 90, 1000000)),
        semantic_field("price_policy", semantic_record(sem, policy, 5)),
        semantic_field("command_status", executed ? jdv_value_i64(command_status) : jdv_value_nothing()),
        semantic_field("adapter_status", jdv_value_i64(adapter_status)),
        semantic_field("ok", jdv_value_bool(!command_status && !adapter_status)),
        semantic_field("error", semantic_symbol(sem, adapter_status ? "measurement_failed" : NULL)),
        semantic_field("effects_may_have_applied", jdv_value_bool(executed))
    };
    return semantic_record(sem, fields, 18);
}

static int cost_semantic_finish(semantic_builtin_t* call, jdv_value_t report,
        uint64_t bytes, int64_t duration, int status) {
    if (call->mode == 2 || call->values.error) return semantic_finish(call, report, status);
    /* Preserve the original JSON numeric keys. Construct exact decimal JSON
     * numbers from the same measured integers; no human text or JSON reparse. */
    jdj_value_t json;
    if (jdj_from_value(&call->values.arena, &report, &call->values.limits, &json))
        return semantic_error(call, "json_projection_failed", 125);
    char numbers[4][96]; uint64_t tokens = bytes / 4u;
    snprintf(numbers[0], sizeof(numbers[0]), "%llu.%09llu",
        (unsigned long long)((uint64_t)duration / 1000000000u), (unsigned long long)((uint64_t)duration % 1000000000u));
    static const uint64_t rates[] = {30, 60, 90};
    for (size_t i = 0; i < 3; i++) {
        uint64_t remainder = (tokens % 1000000u) * rates[i];
        uint64_t whole = (tokens / 1000000u) * rates[i] + remainder / 1000000u;
        snprintf(numbers[i + 1], sizeof(numbers[i + 1]), "%llu.%06llu",
            (unsigned long long)whole, (unsigned long long)(remainder % 1000000u));
    }
    static const char* keys[] = {"seconds", "input_usd", "output_usd", "total_usd"};
    for (const jdj_node_t* node = json.children; node; node = node->next)
        for (size_t i = 0; i < 4; i++) if ((i || duration >= 0) && node->key.len == strlen(keys[i]) && !memcmp(node->key.data, keys[i], node->key.len)) {
            /* This AST was just allocated in our caller-owned mutable arena. */
            jdj_node_t* owned = (jdj_node_t*)node;
            owned->value = (jdj_value_t){.kind = JDJ_NUMBER, .scalar = {(const uint8_t*)numbers[i], strlen(numbers[i])}};
        }
    if (call->admitted) {
        if (jdj_write(&call->output.json, &json, &call->values.limits)) return 125;
        jash_jsonw_raw(&call->output.json, "\n");
        return jash_structured_finish(&call->output, status);
    }
    jash_structured_buffer_t buffer; jash_structured_buffer_init(&buffer, SEMANTIC_OUTPUT_BYTES);
    jash_json_writer_t writer; jash_jsonw_init(&writer, jash_structured_buffer_sink, &buffer);
    int failed = jdj_write(&writer, &json, &call->values.limits);
    jash_jsonw_raw(&writer, "\n");
    if (jash_jsonw_finish(&writer)) failed = 1;
    int result = failed ? 125 : jash_structured_document(call->writer, call->context, call->command,
        buffer.data, buffer.used, buffer.required, jash_budget_limit(), status);
    jash_structured_buffer_free(&buffer); return result;
}

static int cost_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state; int dry = 0;
    if (argc && !strcmp(argv[0], "--dry-run")) { dry = 1; argc--; argv++; }
    if (argc && !strcmp(argv[0], "--")) { argc--; argv++; }
    if (!argc || !*argv[0]) return semantic_error(call, "expected_command", 2);
    jdv_value_t values[64]; uint64_t bytes = 0;
    for (int i = 0; i < argc; i++) { values[i] = semantic_text(&call->values, argv[i]); bytes += strlen(argv[i]) + 1u; }
    jdv_value_t arguments = semantic_list(&call->values, values, (size_t)argc);
    size_t mark = call->values.arena.used;
    jdv_value_t candidate = cost_semantic_value(call, arguments, UINT64_MAX, INT64_MAX, dry, 1, 255, 125);
    int preflight = semantic_preflight(call, &candidate);
    if (preflight) return semantic_error(call, "receipt_limit_before_execution", preflight);
    call->values.arena.used = mark;
    int command_status = 0, adapter_status = 0, executed = 0; int64_t duration = 0;
    if (!dry) {
        struct timespec before, after;
        if (clock_gettime(CLOCK_MONOTONIC, &before)) return semantic_error(call, "clock_unavailable", 1);
        if (semantic_admit(call)) return 125;
        bytes = 0;
        if (cost_native_run(argv, &bytes, &command_status, &executed)) adapter_status = 1;
        if (clock_gettime(CLOCK_MONOTONIC, &after) || after.tv_sec < before.tv_sec ||
            (uint64_t)(after.tv_sec - before.tv_sec) > (uint64_t)INT64_MAX / 1000000000u) { adapter_status = 1; duration = -1; }
        else {
            duration = (int64_t)(after.tv_sec - before.tv_sec) * INT64_C(1000000000);
            int64_t nanos = after.tv_nsec - before.tv_nsec;
            if ((nanos > 0 && nanos > INT64_MAX - duration) || (nanos < 0 && duration < -nanos)) { duration = -1; adapter_status = 1; }
            else duration += nanos;
        }
    }
    int status = adapter_status ? 1 : command_status;
    jdv_value_t report = cost_semantic_value(call, arguments, bytes, duration, dry, executed, command_status, adapter_status);
    return cost_semantic_finish(call, report, bytes, duration, status);
}
