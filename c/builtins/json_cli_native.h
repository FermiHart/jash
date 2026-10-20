// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Included after the length-aware Git collectors in builtin_json.c. The
 * semantic value is shared by JSON and JTV2; legacy output is never reparsed. */
#ifndef JASH_BUILTINS_JSON_CLI_NATIVE_H
#define JASH_BUILTINS_JSON_CLI_NATIVE_H
#include "semantic_value.h"
#include "json_git_observe.h"
#include <core/data_wire.h>
#include <signal.h>

#define CLI_CAPTURE_MAX (256u * 1024u)
#define CLI_VALUE_MAX (2u * 1024u * 1024u)
#define CLI_OUTPUT_MAX (4u * 1024u * 1024u)
typedef struct {
    char* data;
    size_t length, limit;
    uint64_t observed;
    int started, status, truncated, read_error;
} cli_capture_t;

static int cli_run(char* const* argv, cli_capture_t* capture) {
    int descriptors[2];
    capture->status = 125;
    if (pipe(descriptors)) return -1;
    pid_t pid = fork();
    if (pid < 0) { close(descriptors[0]); close(descriptors[1]); return -1; }
    if (!pid) {
        (void)signal(SIGPIPE, SIG_DFL);
        close(descriptors[0]);
        if (dup2(descriptors[1], STDOUT_FILENO) < 0 || dup2(descriptors[1], STDERR_FILENO) < 0) _exit(125);
        close(descriptors[1]);
        execvp(argv[0], argv);
        _exit(127);
    }
    capture->started = 1;
    close(descriptors[1]);
    char bytes[4096];
    for (;;) {
        ssize_t count = read(descriptors[0], bytes, sizeof(bytes));
        if (count < 0 && errno == EINTR) continue;
        if (count <= 0) { if (count < 0) capture->read_error = 1; break; }
        if ((uint64_t)count > UINT64_MAX - capture->observed) capture->read_error = 1;
        else capture->observed += (uint64_t)count;
        size_t copy = (size_t)count;
        if (copy > capture->limit - capture->length) { copy = capture->limit - capture->length; capture->truncated = 1; }
        memcpy(capture->data + capture->length, bytes, copy);
        capture->length += copy;
    }
    capture->data[capture->length] = 0;
    close(descriptors[0]);
    int status = 0; pid_t waited;
    do { waited = waitpid(pid, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited < 0) { capture->read_error = 1; return -1; }
    capture->status = WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
    return 0;
}

static jdv_value_t cli_argv(semantic_value_t* sem, int count, char* const* arguments) {
    jdv_value_t values[64];
    if (count < 1 || count > 64) { sem->error = 1; return jdv_value_nothing(); }
    for (int i = 0; i < count; i++) values[i] = semantic_text(sem, arguments[i]);
    return semantic_list(sem, values, (size_t)count);
}

static jdv_value_t cli_git_status(semantic_value_t* sem, const cli_capture_t* capture, int* error) {
    git_status_row_t* rows = calloc(JSON_MAX_FILES, sizeof(*rows));
    jdv_value_t* items = calloc(JSON_MAX_FILES, sizeof(*items));
    size_t count = 0;
    char* branch = strdup(""); uint64_t ahead = 0, behind = 0;
    jdv_value_t result = jdv_value_nothing();
    if (!rows || !items || !branch || git_status_rows_parse(capture->data, capture->length,
            rows, &count, &branch, &ahead, &behind)) { *error = 75; goto done; }
    for (size_t i = 0; i < count; i++) {
        char xy[3] = {rows[i].x, rows[i].y, 0};
        jdv_record_input_t fields[] = {
            semantic_field("status", semantic_symbol(sem, git_status_name(rows[i].x, rows[i].y))),
            semantic_field("index_status", semantic_text(sem, xy)),
            semantic_field("path", semantic_path(sem, rows[i].path)),
            semantic_field("original_path", semantic_path(sem, rows[i].original_path))
        };
        items[i] = semantic_record(sem, fields, rows[i].original_path ? 4 : 3);
    }
    jdv_record_input_t git[] = {
        semantic_field("branch", semantic_text(sem, branch)),
        semantic_field("dirty", jdv_value_bool(count != 0)),
        semantic_field("ahead", jdv_value_u64((uint64_t)ahead)),
        semantic_field("behind", jdv_value_u64((uint64_t)behind))
    };
    jdv_record_input_t fields[] = {
        semantic_field("files", semantic_list(sem, items, count)),
        semantic_field("git", semantic_record(sem, git, 4))
    };
    result = semantic_record(sem, fields, 2);
done:
    if (rows) git_status_rows_free(rows, count);
    free(rows); free(items); free(branch);
    return result;
}

static jdv_value_t cli_git_diff(semantic_value_t* sem, const cli_capture_t* capture, int* error) {
    git_diff_row_t* rows = calloc(JSON_MAX_FILES, sizeof(*rows));
    jdv_value_t* items = calloc(JSON_MAX_FILES, sizeof(*items));
    size_t count = 0; uint64_t additions = 0, deletions = 0, binary_files = 0;
    jdv_value_t result = jdv_value_nothing();
    if (!rows || !items || git_diff_rows_parse(capture->data, capture->length, rows, &count)) { *error = 75; goto done; }
    for (size_t i = 0; i < count; i++) {
        if (rows[i].insertions > UINT64_MAX - additions || rows[i].deletions > UINT64_MAX - deletions) { *error = 75; goto done; }
        additions += rows[i].insertions; deletions += rows[i].deletions;
        binary_files += rows[i].binary != 0;
        jdv_record_input_t fields[] = {
            semantic_field("path", semantic_path(sem, rows[i].path)),
            semantic_field("binary", jdv_value_bool(rows[i].binary)),
            semantic_field("insertions", rows[i].binary ? jdv_value_nothing() : jdv_value_u64(rows[i].insertions)),
            semantic_field("deletions", rows[i].binary ? jdv_value_nothing() : jdv_value_u64(rows[i].deletions)),
            semantic_field("old_path", semantic_path(sem, rows[i].old_path))
        };
        items[i] = semantic_record(sem, fields, rows[i].old_path ? 5 : 4);
    }
    jdv_record_input_t totals[] = {
        semantic_field("files", jdv_value_u64(count)),
        semantic_field("insertions", jdv_value_u64(additions)),
        semantic_field("deletions", jdv_value_u64(deletions)),
        semantic_field("binary_files", jdv_value_u64(binary_files)),
        semantic_field("text_files_only", jdv_value_bool(1))
    };
    jdv_record_input_t fields[] = {
        semantic_field("files", semantic_list(sem, items, count)),
        semantic_field("total", semantic_record(sem, totals, 5))
    };
    result = semantic_record(sem, fields, 2);
done:
    if (rows) git_diff_rows_free(rows, count);
    free(rows); free(items);
    return result;
}

static jdv_value_t cli_git_log(semantic_value_t* sem, const cli_capture_t* capture, int* error) {
    jdv_value_t commits[JSON_MAX_COMMITS]; size_t count = 0, offset = 0;
    while (offset < capture->length) {
        if (count == JSON_MAX_COMMITS) { *error = 75; break; }
        const char* data[4]; size_t lengths[4];
        for (size_t field = 0; field < 4; field++) {
            if (json_next_nul(capture->data, capture->length, &offset, &data[field], &lengths[field]) != 1) {
                *error = 75; return jdv_value_nothing();
            }
        }
        if (lengths[0] != 40 && lengths[0] != 64) { *error = 75; return jdv_value_nothing(); }
        for (size_t i = 0; i < lengths[0]; i++) if (!isxdigit((unsigned char)data[0][i])) { *error = 75; return jdv_value_nothing(); }
        jdv_record_input_t fields[] = {
            semantic_field("hash", semantic_text_n(sem, data[0], lengths[0])),
            semantic_field("message", semantic_text_n(sem, data[1], lengths[1])),
            semantic_field("author", semantic_text_n(sem, data[2], lengths[2])),
            semantic_field("date", semantic_text_n(sem, data[3], lengths[3]))
        };
        commits[count++] = semantic_record(sem, fields, 4);
    }
    jdv_record_input_t field = semantic_field("commits", semantic_list(sem, commits, count));
    return semantic_record(sem, &field, 1);
}

static jdv_value_t cli_git_observation(semantic_value_t* sem, char** argv, int log,
        jash_git_observation_t* outcome) {
    cli_capture_t capture = {.limit = CLI_CAPTURE_MAX};
    capture.data = malloc(CLI_CAPTURE_MAX + 1u);
    int adapter_status = !capture.data ? 125 : cli_run(argv, &capture) ? 125 : 0;
    if (capture.truncated || capture.read_error) adapter_status = 75;
    jdv_value_t payload = jdv_value_nothing();
    if (!adapter_status && !capture.status)
        payload = log == 2 ? cli_git_diff(sem, &capture, &adapter_status) :
            log == 1 ? cli_git_log(sem, &capture, &adapter_status) : cli_git_status(sem, &capture, &adapter_status);
    if (sem->error) adapter_status = 125;
    if (capture.observed > UINT64_MAX - outcome->output_bytes) adapter_status = 75;
    else outcome->output_bytes += capture.observed;
    if (adapter_status && !outcome->adapter_status) outcome->adapter_status = adapter_status;
    if (capture.status && !outcome->command_status) outcome->command_status = capture.status;
    outcome->truncated |= capture.truncated;
    if (log == 1) { outcome->log_command_status = capture.status; outcome->log_started = capture.started; }
    else { outcome->status_command_status = capture.status; outcome->status_started = capture.started; }
    jdv_record_input_t fields[] = {
        semantic_field("available", jdv_value_bool(!adapter_status && !capture.status)),
        semantic_field("started", jdv_value_bool(capture.started)),
        semantic_field("command_status", capture.started ? jdv_value_i64(capture.status) : jdv_value_nothing()),
        semantic_field("adapter_status", jdv_value_i64(adapter_status)),
        semantic_field("output_bytes", jdv_value_filesize(capture.observed)),
        semantic_field("truncated", jdv_value_bool(capture.truncated)),
        semantic_field("payload", !adapter_status && !capture.status ? payload : jdv_value_nothing()),
        semantic_field("diagnostic_bytes", capture.status && capture.data ? semantic_bytes(sem, capture.data, capture.length) : jdv_value_nothing())
    };
    jdv_value_t value = semantic_record(sem, fields, 8);
    free(capture.data); return value;
}

int jash_git_observe(semantic_value_t* values, int latest,
                    jdv_value_t* result, jash_git_observation_t* outcome) {
    if (!values || !result || !outcome) { errno = EINVAL; return 2; }
    *outcome = (jash_git_observation_t){.log_requested = latest != 0};
    char* status[] = {"git", "--no-pager", "--no-optional-locks", "-c", "core.fsmonitor=false",
        "status", "--porcelain=v1", "-z", "--branch", "--untracked-files=all", NULL};
    jdv_value_t observed = cli_git_observation(values, status, 0, outcome);
    jdv_value_t commit = jdv_value_nothing();
    if (latest && !outcome->adapter_status && !outcome->command_status) {
        char* log[] = {"git", "--no-pager", "--no-optional-locks", "-c", "core.fsmonitor=false",
            "log", "--no-show-signature", "--format=%H%x00%s%x00%an%x00%aI", "--encoding=none", "-z", "-1", NULL};
        commit = cli_git_observation(values, log, 1, outcome);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.git-observation.v1")),
        semantic_field("status", observed), semantic_field("latest_commit", commit),
        semantic_field("latest_commit_requested", jdv_value_bool(latest != 0)),
        semantic_field("cryptographically_verified", jdv_value_bool(0)),
        semantic_field("consistency", semantic_symbol(values, "independent-subprocess-observations"))
    };
    *result = semantic_record(values, fields, 6);
    if (values->error) outcome->adapter_status = 125;
    return outcome->adapter_status ? outcome->adapter_status : outcome->command_status;
}

int jash_git_diff_stat_observe(semantic_value_t* values, jdv_value_t* result,
                              jash_git_observation_t* outcome) {
    if (!values || !result || !outcome) { errno = EINVAL; return 2; }
    *outcome = (jash_git_observation_t){0};
    char* arguments[] = {"git", "--no-pager", "--no-optional-locks", "-c", "core.fsmonitor=false",
        "diff", "--numstat", "-z", "--no-ext-diff", "--no-textconv", NULL};
    *result = cli_git_observation(values, arguments, 2, outcome);
    if (values->error) outcome->adapter_status = 125;
    return outcome->adapter_status ? outcome->adapter_status : outcome->command_status;
}

static int cli_number_before(const char* line, const char* word, uint64_t* output) {
    const char* end = strstr(line, word);
    if (!end) return 0;
    while (end > line && end[-1] == ' ') end--;
    const char* start = end;
    while (start > line && isdigit((unsigned char)start[-1])) start--;
    if (start == end || (start > line && start[-1] == '-')) return 0;
    return git_parse_count(start, (size_t)(end - start), output) ? -1 : 1;
}

static jdv_value_t cli_build_report(semantic_value_t* sem, const cli_capture_t* capture, int npm) {
    jdv_value_t diagnostics[JSON_MAX_DIAGS]; size_t count = 0, offset = 0;
    uint64_t passed = 0, failed = 0, total = 0;
    int have_passed = 0, have_failed = 0, have_total = 0, diagnostics_complete = 1;
    while (offset < capture->length) {
        const char* start = capture->data + offset;
        const char* end = memchr(start, '\n', capture->length - offset);
        size_t length = end ? (size_t)(end - start) : capture->length - offset;
        offset += length + (end != NULL);
        if (length >= JSON_MAX_LINE || memchr(start, 0, length)) { diagnostics_complete = 0; continue; }
        char line[JSON_MAX_LINE]; memcpy(line, start, length); line[length] = 0;
        if (npm) {
            int code; uint64_t value;
            if ((code = cli_number_before(line, strstr(line, "Tests:") ? "passed" : "passing", &value)) == 1) { passed = value; have_passed = 1; }
            else if (code < 0) diagnostics_complete = 0;
            if ((code = cli_number_before(line, strstr(line, "Tests:") ? "failed" : "failing", &value)) == 1) { failed = value; have_failed = 1; }
            else if (code < 0) diagnostics_complete = 0;
            if (strstr(line, "Tests:") && (code = cli_number_before(line, "total", &value)) == 1) { total = value; have_total = 1; }
            else if (strstr(line, "Tests:") && code < 0) diagnostics_complete = 0;
        }
        const char* severity = NULL; char* marker = strstr(line, ": error:");
        if (marker) severity = "error";
        else if ((marker = strstr(line, ": warning:"))) severity = "warning";
        else if ((marker = strstr(line, ": note:"))) severity = "note";
        if (!severity) continue;
        if (count == JSON_MAX_DIAGS) { diagnostics_complete = 0; continue; }
        size_t prefix = (size_t)(marker - line);
        char* location = line;
        char* colon = memchr(line, ':', prefix);
        jdv_value_t line_number = jdv_value_nothing(), column = jdv_value_nothing();
        if (colon) {
            char* last = colon + 1; while (isdigit((unsigned char)*last)) last++;
            uint64_t number;
            if (last > colon + 1 && !git_parse_count(colon + 1, (size_t)(last - colon - 1), &number)) line_number = jdv_value_u64(number);
            if (*last == ':' && last < marker) {
                char* next = last + 1; while (isdigit((unsigned char)*next)) next++;
                if (next > last + 1 && !git_parse_count(last + 1, (size_t)(next - last - 1), &number)) column = jdv_value_u64(number);
            }
            *colon = 0;
        } else line[prefix] = 0;
        const char* message = marker + strlen(severity) + 3u;
        while (*message == ' ') message++;
        jdv_record_input_t fields[] = {
            semantic_field("severity", semantic_symbol(sem, severity)),
            semantic_field("file", semantic_path(sem, location)),
            semantic_field("line", line_number), semantic_field("column", column),
            semantic_field("message", semantic_text(sem, message)),
            semantic_field("raw_line", semantic_text_n(sem, start, length))
        };
        diagnostics[count++] = semantic_record(sem, fields, 6);
    }
    if (!have_total && have_passed && have_failed && passed <= UINT64_MAX - failed) { total = passed + failed; have_total = 1; }
    jdv_record_input_t tests[] = {
        semantic_field("passed", have_passed ? jdv_value_u64(passed) : jdv_value_nothing()),
        semantic_field("failed", have_failed ? jdv_value_u64(failed) : jdv_value_nothing()),
        semantic_field("total", have_total ? jdv_value_u64(total) : jdv_value_nothing())
    };
    jdv_record_input_t fields[] = {
        semantic_field("diagnostics", semantic_list(sem, diagnostics, count)),
        semantic_field("diagnostics_complete", jdv_value_bool(diagnostics_complete && !capture->truncated && !capture->read_error)),
        semantic_field("raw_truncated", semantic_text_n(sem, capture->data, capture->length)),
        semantic_field("raw", semantic_text_n(sem, capture->data, capture->length)),
        semantic_field("truncated", jdv_value_bool(capture->truncated)),
        semantic_field("counter_source", semantic_symbol(sem, "best-effort-text")),
        semantic_field("tests", semantic_record(sem, tests, 3))
    };
    return semantic_record(sem, fields, npm ? 7 : 5);
}

static jdv_value_t cli_failed_report(semantic_value_t* sem, const cli_capture_t* capture,
                                     const char* schema, int adapter_status) {
    sem->arena.used = 0; sem->error = 0;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, schema)),
        semantic_field("rc", jdv_value_i64(capture->status)),
        semantic_field("command_status", jdv_value_i64(capture->status)),
        semantic_field("adapter_status", jdv_value_i64(adapter_status)),
        semantic_field("ok", jdv_value_bool(0)),
        semantic_field("error", semantic_symbol(sem, "report_encoding_or_budget_failed")),
        semantic_field("effects_may_have_applied", jdv_value_bool(capture->started)),
        semantic_field("output_bytes", jdv_value_filesize(capture->observed))
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

static int cli_adapter(jash_writer_t writer, void* context, int native, int argc, char** argv) {
    if (!argc || argc > 56) return 2;
    char* command[72]; int count = 0, kind = 0;
    const char* schema = NULL;
    char limit_text[32];
    if (!strcmp(argv[0], "git")) {
        if (argc < 2) return 2;
        command[count++] = "git"; command[count++] = "--no-pager"; command[count++] = "--no-optional-locks";
        command[count++] = "-c"; command[count++] = "core.fsmonitor=false";
        command[count++] = argv[1];
        if (!strcmp(argv[1], "status") && argc == 2) {
            kind = 1; schema = "jash.git-status.v1";
            command[count++] = "--porcelain=v1"; command[count++] = "-z";
            command[count++] = "--branch"; command[count++] = "--untracked-files=all";
        } else if (!strcmp(argv[1], "log") && argc <= 3) {
            uint64_t limit = 10;
            if (argc == 3 && (git_parse_count(argv[2], strlen(argv[2]), &limit) || !limit || limit > JSON_MAX_COMMITS)) return 2;
            snprintf(limit_text, sizeof(limit_text), "-%u", (unsigned)limit);
            kind = 2; schema = "jash.git-log.v1";
            command[count++] = "--no-show-signature";
            command[count++] = "--format=%H%x00%s%x00%an%x00%aI";
            command[count++] = "--encoding=none"; command[count++] = "-z"; command[count++] = limit_text;
        } else if (!strcmp(argv[1], "diff")) {
            kind = 3; schema = "jash.git-diff-stat.v1";
            command[count++] = "--numstat"; command[count++] = "-z";
            command[count++] = "--no-ext-diff"; command[count++] = "--no-textconv";
            int boundary = 0;
            for (int i = 2; i < argc; i++) {
                if (!strcmp(argv[i], "--")) boundary = 1;
                if (boundary || strcmp(argv[i], "--stat")) command[count++] = argv[i];
            }
        } else return 2;
    } else if (!strcmp(argv[0], "make") || !strcmp(argv[0], "cmake") || !strcmp(argv[0], "npm")) {
        kind = !strcmp(argv[0], "npm") ? 5 : 4;
        schema = kind == 5 ? "jash.npm-result.v1" : "jash.build-result.v1";
        for (int i = 0; i < argc; i++) command[count++] = argv[i];
        if (argc == 1 && !strcmp(argv[0], "cmake")) { command[count++] = "--build"; command[count++] = "."; }
        else if (argc == 1 && kind == 5) command[count++] = "test";
    } else return 2;
    command[count] = NULL;
    size_t argv_bytes = 0;
    for (int i = 0; i < count; i++) {
        size_t bytes = strlen(command[i]);
        if (bytes > 8192u - argv_bytes) return 2;
        argv_bytes += bytes;
    }
    long budget = native ? 0 : jash_budget_limit();
    size_t overhead = 4096u + argv_bytes * 6u;
    if (budget > 0 && (uint64_t)budget < overhead) {
        if (!native) return jash_structured_error(writer, context, "json", "budget_too_small_before_execution", 75);
        jash_writes(jash_stderr_writer, NULL, "json: data budget too small before execution\n");
        return 75;
    }
    cli_capture_t capture = {.limit = CLI_CAPTURE_MAX};
    if (budget > 0 && ((size_t)budget - overhead) / 64u < capture.limit) capture.limit = ((size_t)budget - overhead) / 64u;
    capture.data = malloc(capture.limit + 1u);
    semantic_value_t sem;
    if (!capture.data || semantic_value_init(&sem, 16u * 1024u * 1024u, CLI_VALUE_MAX)) { free(capture.data); return 125; }
    void* workspace = native ? malloc(CLI_VALUE_MAX + 512u) : NULL;
    if (native && !workspace) { free(capture.data); semantic_value_free(&sem); return 125; }
    jash_structured_output_t output;
    if (jash_structured_begin(&output, writer, context)) { free(workspace); free(capture.data); semantic_value_free(&sem); return 125; }
    jdv_schema_t any = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
    jdv_wire_push_t* push = NULL;
    value_output_sink_t sink = {output.json.sink, output.json.context};
    jash_structured_buffer_t document; jash_structured_buffer_init(&document, CLI_OUTPUT_MAX);
    int status = 125, adapter_error = 0;
    if (native) {
        jdv_stream_config_t config; jdv_stream_config_init(&config);
        config.boundedness = JDV_STREAM_FINITE; config.declared_schema = config.guaranteed_schema = &any;
        config.value_limits = sem.limits;
        config.stream_limits.max_items = 1; config.stream_limits.max_logical_bytes = UINT64_MAX;
        config.stream_limits.max_materialized_bytes = config.stream_limits.max_physical_bytes = CLI_OUTPUT_MAX;
        if (jdv_wire_push_v2_open(&push, &config, value_output_sink, &sink, workspace, CLI_VALUE_MAX + 512u)) goto done;
    } else {
        /* A checked JSON whitespace write admits the destination before fork. */
        jash_jsonw_raw(&output.json, "\n");
        if (jash_jsonw_finish(&output.json)) goto done;
    }
    if (cli_run(command, &capture)) adapter_error = 125;
    if (capture.truncated || capture.read_error) adapter_error = 75;
    jdv_value_t payload = jdv_value_nothing();
    if (!adapter_error && (!capture.status || kind >= 4)) {
        if (kind == 1) payload = cli_git_status(&sem, &capture, &adapter_error);
        else if (kind == 2) payload = cli_git_log(&sem, &capture, &adapter_error);
        else if (kind == 3) payload = cli_git_diff(&sem, &capture, &adapter_error);
        else payload = cli_build_report(&sem, &capture, kind == 5);
    } else if (kind >= 4) payload = cli_build_report(&sem, &capture, kind == 5);
    status = adapter_error ? adapter_error : capture.status;
    jdv_record_input_t fields[32]; size_t fields_count = 0;
    fields[fields_count++] = semantic_field("schema", semantic_symbol(&sem, schema));
    fields[fields_count++] = semantic_field("rc", jdv_value_i64(capture.status));
    fields[fields_count++] = semantic_field("command_status", jdv_value_i64(capture.status));
    fields[fields_count++] = semantic_field("adapter_status", jdv_value_i64(adapter_error));
    fields[fields_count++] = semantic_field("ok", jdv_value_bool(!status));
    fields[fields_count++] = semantic_field("argv", cli_argv(&sem, count, command));
    fields[fields_count++] = semantic_field("output_bytes", jdv_value_filesize(capture.observed));
    fields[fields_count++] = semantic_field("output_stream", semantic_symbol(&sem, "combined-stdout-stderr"));
    fields[fields_count++] = semantic_field("effects_may_have_applied", jdv_value_bool(capture.started));
    fields[fields_count++] = semantic_field("error", semantic_symbol(&sem, adapter_error ? "capture_or_parse_failed" : capture.status ? "command_failed" : NULL));
    if (payload.kind == JDV_RECORD) for (size_t i = 0; i < payload.as.record.len; i++) fields[fields_count++] = payload.as.record.fields[i];
    jdv_value_t value = semantic_record(&sem, fields, fields_count);
    if (sem.error) { adapter_error = status = 125; value = cli_failed_report(&sem, &capture, schema, adapter_error); }
    if (native) {
        value_output_measure_t measured = {0, CLI_VALUE_MAX};
        if (budget > 0 && (size_t)budget - 1024u < measured.limit) measured.limit = (size_t)budget - 1024u;
        jdv_writer_t measure; jdv_writer_init(&measure, value_output_measure, &measured);
        if (jdv_encode_limited(&value, &measure, &sem.limits) || jdv_writer_finish(&measure)) {
            adapter_error = status = 75; value = cli_failed_report(&sem, &capture, schema, adapter_error);
        }
    } else {
        jash_json_writer_t encoded; jash_jsonw_init(&encoded, jash_structured_buffer_sink, &document);
        int encoding_error = jdv_json_write(&encoded, &value, &sem.limits);
        jash_jsonw_raw(&encoded, "\n");
        if (encoding_error || jash_jsonw_finish(&encoded) || document.required != document.used ||
            (budget > 0 && document.required + 1u > (size_t)budget)) {
            adapter_error = status = 75; value = cli_failed_report(&sem, &capture, schema, adapter_error);
            jash_structured_buffer_free(&document); jash_structured_buffer_init(&document, CLI_OUTPUT_MAX);
            jash_jsonw_init(&encoded, jash_structured_buffer_sink, &document);
            if (jdv_json_write(&encoded, &value, &sem.limits)) { status = 125; goto done; }
            jash_jsonw_raw(&encoded, "\n");
            if (jash_jsonw_finish(&encoded) || document.required != document.used) { status = 125; goto done; }
        }
    }
    if (sem.error) { status = 125; goto done; }
    if (native) {
        if (jdv_wire_push_v2_item(push, &value, capture.observed)) { status = 125; goto done; }
        jdv_stream_end_t outcome, final; (void)jdv_stream_end_init(&outcome);
        outcome.state = status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
        outcome.completeness = status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
        outcome.producer_status = capture.status; outcome.transform_status = adapter_error;
        outcome.errors_seen = status ? 1 : 0; outcome.truncated = capture.truncated;
        if (jdv_wire_push_v2_end(push, &outcome, &final)) status = 125;
    } else {
        jash_jsonw_raw(&output.json, document.data);
    }
done:
    jdv_wire_push_v2_close(push);
    status = jash_structured_finish(&output, status);
    jash_structured_buffer_free(&document);
    free(workspace); free(capture.data); semantic_value_free(&sem);
    return status;
}
#endif
