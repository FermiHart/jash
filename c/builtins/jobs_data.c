// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "builtin.h"
#include "discovery_output.h"
#include "query_data.h"
#include <core/data_json.h>
#include <core/jobs_bg.h>
#include <core/structured_output.h>
#include <job_control.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define JF(key, val) {JDV_LITERAL(key), (val)}
static int job_id(const char* text, int suffix) {
    if (!text || *text < '1' || *text > '9') return 0;
    char* end; errno = 0; long value = strtol(text, &end, 10);
    if (errno || value > INT_MAX || (suffix ? strcmp(end, ".json") : *end)) return 0;
    return (int)value;
}
static const jdv_value_t* job_field(const jdv_value_t* value, const char* name) {
    return jash_discovery_field(value, name);
}
static int job_integer(const jdv_value_t* value, int64_t* out) {
    if (!value) return -1;
    if (value->kind == JDV_I64) { *out = value->as.i64; return 0; }
    if (value->kind == JDV_U64 && value->as.u64 <= INT64_MAX) { *out = (int64_t)value->as.u64; return 0; }
    return -1;
}
static int job_string_is(const jdv_value_t* value, const char* text) {
    return value && value->kind == JDV_STRING && value->as.slice.len == strlen(text) &&
        !memcmp(value->as.slice.data, text, value->as.slice.len);
}
static int jobs_detached(jash_discovery_rows_t* rows) {
    const char* home = getenv("HOME");
    if (!home || !*home) return 0;
    char* path = NULL;
    if (asprintf(&path, "%s/.jash/jobs", home) < 0) return ENOMEM;
    DIR* directory = opendir(path); free(path);
    if (!directory) return errno == ENOENT ? 0 : errno;
    int failure = 0;
    for (;;) {
        errno = 0; struct dirent* entry = readdir(directory);
        if (!entry) { if (errno) failure = errno; break; }
        int id = job_id(entry->d_name, 1);
        if (!id) continue;
        int fd = openat(dirfd(directory), entry->d_name, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) { failure = errno; break; }
        struct stat st;
        char input[8192]; size_t used = 0;
        if (fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uint64_t)st.st_size >= sizeof input) failure = EINVAL;
        while (!failure && used < sizeof input) {
            ssize_t count = read(fd, input + used, sizeof(input) - used);
            if (count < 0 && errno == EINTR) continue;
            if (count < 0) { failure = errno; break; }
            if (!count) break;
            used += (size_t)count;
        }
        if (close(fd) && !failure) failure = errno;
        if (failure || used == sizeof input) { if (!failure) failure = EOVERFLOW; break; }
        jdv_value_t parsed;
        if (jdv_json_parse(&rows->arena, input, used, &JDV_DEFAULT_LIMITS, &parsed) || parsed.kind != JDV_RECORD) { failure = EINVAL; break; }
        const jdv_value_t *state = job_field(&parsed, "state"), *command = job_field(&parsed, "cmd");
        int64_t stored_id, pid, start, end = 0, milliseconds = 0, rc = 0;
        int running = job_string_is(state, "running");
        if (job_integer(job_field(&parsed, "id"), &stored_id) || stored_id != id ||
            job_integer(job_field(&parsed, "pid"), &pid) || pid < 0 ||
            job_integer(job_field(&parsed, "start_ts"), &start) ||
            (!running && !job_string_is(state, "done")) || !command || command->kind != JDV_STRING) { failure = EINVAL; break; }
        if (!running && (job_integer(job_field(&parsed, "end_ts"), &end) ||
            job_integer(job_field(&parsed, "ms"), &milliseconds) || milliseconds < 0 ||
            job_integer(job_field(&parsed, "rc"), &rc) || rc < 0 || rc > 255)) { failure = EINVAL; break; }
        const jdv_value_t* notified = job_field(&parsed, "notified");
        if (notified && notified->kind != JDV_BOOL) { failure = EINVAL; break; }
        time_t now = time(NULL);
        jdv_record_input_t fields[] = {
            JF("id", jdv_value_u64((uint64_t)id)), JF("pid", jdv_value_u64((uint64_t)pid)),
            JF("scope", jash_discovery_string(rows, "detached")), JF("state", *state), JF("cmd", *command),
            JF("command_complete", jdv_value_nothing()), /* legacy records do not prove no truncation */
            JF("rc", running ? jdv_value_nothing() : jdv_value_i64(rc)),
            JF("ms", running ? jdv_value_nothing() : jdv_value_u64((uint64_t)milliseconds)),
            JF("elapsed_s", running && now >= start ? jdv_value_u64((uint64_t)(now - start)) : jdv_value_nothing()),
            JF("start_ts", jdv_value_i64(start)), JF("end_ts", running ? jdv_value_nothing() : jdv_value_i64(end)),
            JF("start", query_data_timestamp(start, 0)), JF("end", running ? jdv_value_nothing() : query_data_timestamp(end, 0)),
            JF("notified", notified ? *notified : jdv_value_bool(0))
        };
        jash_discovery_add(rows, fields, sizeof fields / sizeof *fields);
        if (rows->error) { failure = EOVERFLOW; break; }
        if (jash_query_cancel_poll(NULL)) { failure = ECANCELED; break; }
    }
    if (closedir(directory) && !failure) failure = errno;
    return failure;
}
static void jobs_session(jash_state_t* state, jash_discovery_rows_t* rows) {
    jash_job_refresh_all(state->jobs);
    for (size_t i = 0; i < jash_job_count(state->jobs) && !rows->error; i++) {
        jash_job_view_t job;
        if (!jash_job_at(state->jobs, i, &job)) { rows->error = 1; break; }
        jdv_record_input_t fields[] = {
            JF("id", jdv_value_u64((uint64_t)job.id)), JF("pid", jdv_value_u64((uint64_t)job.pgid)),
            JF("scope", jash_discovery_string(rows, "session")),
            JF("state", jash_discovery_string(rows, job.state == JASH_JOB_DONE ? "done" : job.state == JASH_JOB_STOPPED ? "stopped" : "running")),
            JF("cmd", jash_discovery_string(rows, job.command)), JF("command_complete", jdv_value_bool(!job.command_truncated)),
            JF("rc", job.state == JASH_JOB_DONE ? jdv_value_i64(job.exit_status) : jdv_value_nothing()),
            JF("current", jdv_value_bool(job.current)), JF("previous", jdv_value_bool(job.previous)),
            JF("notified", jdv_value_bool(job.notified))
        };
        jash_discovery_add(rows, fields, sizeof fields / sizeof *fields);
    }
}
static int jobs_emit(jash_writer_t writer, void* context, int mode, jash_discovery_rows_t* rows) {
    if (mode == JASH_DISCOVERY_DATA) {
        static const jdv_schema_t any = {.kind=JDV_SCHEMA_ANY};
        return query_data_emit(writer, context, &any, rows->items, rows->count, 0, 0, JDV_STREAM_COMPLETE, NULL);
    }
    jash_structured_buffer_t buffer; jash_structured_buffer_init(&buffer, QUERY_DATA_MAX_BYTES);
    jash_json_writer_t json; jash_jsonw_init(&json, jash_structured_buffer_sink, &buffer);
    jash_jsonw_raw(&json, "[");
    for (size_t i = 0; i < rows->count && !json.error; i++) {
        if (i) jash_jsonw_raw(&json, ",");
        if (jdv_json_write(&json, &rows->items[i], &JDV_DEFAULT_LIMITS)) json.error = 1;
    }
    jash_jsonw_raw(&json, "]\n");
    int rc = jash_jsonw_finish(&json) || buffer.error ? jash_structured_error(writer, context, "jobs", "encoding_failed", 125)
        : jash_structured_document(writer, context, "jobs", buffer.data, buffer.used, buffer.required, jash_budget_limit(), 0);
    jash_structured_buffer_free(&buffer); return rc;
}
int jash_b_jobs_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode, selected = jash_discovery_format(argc, argv, &mode);
    if (!selected) return jash_discovery_text(jash_b_jobs, state, writer, context, argc, argv);
    if (selected < 0) return jash_discovery_error(writer, context, "jobs", mode, "conflicting_formats", 2);
    int session = 0, detached = 1, out = 0, id = 0;
    for (int i = 0; i < argc; i++) {
        if (jash_discovery_format_flag(argv[i])) continue;
        if (!strcmp(argv[i], "--session")) { session = 1; detached = 0; }
        else if (!strcmp(argv[i], "--all")) session = detached = 1;
        else if (!strcmp(argv[i], "--detached")) { session = 0; detached = 1; }
        else if (!strcmp(argv[i], "out") && !out && i + 1 < argc && (id = job_id(argv[i + 1], 0))) { out = 1; i++; }
        else return jash_discovery_error(writer, context, "jobs", mode, "invalid_options", 2);
    }
    if (out) {
        char path[680];
        if (!jash_jobs_bg_out_path(id, path, sizeof path)) return jash_discovery_error(writer, context, "jobs", mode, "home_unset", 1);
        char* args[] = {mode == JASH_DISCOVERY_JSON ? "--json" : "--data-v2", "--", path};
        return jash_b_cat(state, writer, context, 3, args);
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "jobs", mode, "allocation_failed", 125);
    int error = detached ? jobs_detached(&rows) : 0;
    if (!error && session) jobs_session(state, &rows);
    int rc = error || rows.error ? jash_discovery_error(writer, context, "jobs", mode,
        error == ECANCELED ? "request_cancelled" : rows.error || error == EOVERFLOW ? "collection_limit" : "job_store_invalid", error == ECANCELED ? 130 : rows.error || error == EOVERFLOW ? 75 : 1)
        : jobs_emit(writer, context, mode, &rows);
    jash_discovery_rows_free(&rows); return rc;
}
