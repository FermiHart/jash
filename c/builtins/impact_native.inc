// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "analysis_output.h"
#include "semantic_events.h"
#include "memory_fsjournal.h"
#include "memory_session.h"
#include "eval_output.h"
#include <core/query_cancel.h>

typedef struct {
    char path[PATH_MAX]; size_t files, bytes, records, maybe_truncated;
    int directory_present, error;
} impact_native_source_t;
static int impact_native_name_compare(const void* a, const void* b) {
    return strcmp(*(const char* const*)a, *(const char* const*)b);
}
static int impact_native_load(jash_state_t* state, impact_native_source_t* source,
        impact_test_t* tests, int* ntests, test_run_t* runs, int* nruns) {
    memset(source, 0, sizeof(*source)); *ntests = *nruns = 0;
    const char* home = getenv("HOME");
    if (!home || home[0] != '/') { source->error = EINVAL; return -1; }
    int length = snprintf(source->path, sizeof(source->path), "%s/.jash_sessions", home);
    if (length < 0 || (size_t)length >= sizeof(source->path)) { source->error = ENAMETOOLONG; return -1; }
    memory_store_t store = {-1,-1}; int homefd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (homefd < 0) source->error = errno;
    else {
        if (memory_store_trusted(homefd, 1)) source->error = errno;
        else {
            store.directory = memory_store_child(homefd, ".jash_sessions", 0);
            if (store.directory < 0 && errno != ENOENT) source->error = errno;
        }
        if (close(homefd) && !source->error) source->error = errno;
    }
    char* names[256] = {0}; size_t named = 0; struct stat before = {0}, after = {0};
    if (!source->error && store.directory >= 0) {
        source->directory_present = 1;
        if (fstat(store.directory, &before)) source->error = errno;
        int copied = source->error ? -1 : fcntl(store.directory, F_DUPFD_CLOEXEC, 3);
        DIR* directory = copied < 0 ? NULL : fdopendir(copied);
        if (!directory) { if (copied >= 0) close(copied); if (!source->error) source->error = errno; }
        else {
            for (;;) {
                if (jash_query_cancel_poll(state)) { source->error = ECANCELED; break; }
                errno = 0; struct dirent* entry = readdir(directory);
                if (!entry) { if (errno) source->error = errno; break; }
                if (strncmp(entry->d_name, "session_", 8)) continue;
                if (named == 256u) { source->error = E2BIG; break; }
                names[named] = strdup(entry->d_name);
                if (!names[named]) { source->error = ENOMEM; break; }
                named++;
            }
            if (closedir(directory) && !source->error) source->error = errno;
        }
        qsort(names, named, sizeof(*names), impact_native_name_compare);
    }
    for (size_t i = 0; i < named && !source->error; i++) {
        if (jash_query_cancel_poll(state)) { source->error = ECANCELED; break; }
        char* bytes = NULL; size_t size = 0; int exists = 0;
        if (memory_store_read(&store, names[i], 4u * 1024u * 1024u, &bytes, &size, &exists)) source->error = errno;
        else if (!exists) source->error = ESTALE;
        else if (size > 16u * 1024u * 1024u - source->bytes) source->error = E2BIG;
        else if (memchr(bytes, 0, size)) source->error = EINVAL;
        if (!source->error) { source->files++; source->bytes += size; }
        size_t offset = 0;
        while (offset < size && !source->error) {
            if (jash_query_cancel_poll(state)) { source->error = ECANCELED; break; }
            char* line = bytes + offset; char* newline = memchr(line, '\n', size - offset);
            if (!newline) { source->error = EINVAL; break; }
            size_t width = (size_t)(newline - line);
            if (width > 65536u || source->records == 65536u) { source->error = E2BIG; break; }
            *newline = 0; offset = (size_t)(newline - bytes) + 1u;
            memory_session_record_t record;
            if (memory_session_parse(line, width, &record) || record.status < INT_MIN || record.status > INT_MAX) {
                source->error = EINVAL; break;
            }
            source->records++; source->maybe_truncated += record.maybe_truncated != 0;
            if (!is_test_cmd(record.command)) continue;
            if (strlen(record.command) >= sizeof(tests[0].cmd) || *nruns == IMPACT_MAX_EVENTS) {
                source->error = E2BIG; break;
            }
            int index = test_intern(tests, ntests, record.command);
            if (index < 0) { source->error = E2BIG; break; }
            runs[*nruns] = (test_run_t){.ts = (unsigned long)record.timestamp, .rc = (int)record.status,
                .test_idx = index, .source_order = (unsigned)*nruns};
            (*nruns)++; tests[index].runs++; tests[index].fails += record.status != 0;
        }
        free(bytes);
    }
    if (!source->error && store.directory >= 0) {
        if (fstat(store.directory, &after)) source->error = errno;
        else if (!memory_store_same(&before, &after)) source->error = ESTALE;
    }
    for (size_t i = 0; i < named; i++) free(names[i]);
    if (memory_store_close(&store) && !source->error) source->error = errno;
    if (!source->error) qsort(runs, (size_t)*nruns, sizeof(*runs), impact_run_compare);
    return source->error ? -1 : 0;
}

static jdv_value_t impact_native_source_value(semantic_value_t* sem, const impact_native_source_t* source) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, source->path)),
        semantic_field("directory_present", jdv_value_bool(source->directory_present)),
        semantic_field("read_complete", jdv_value_bool(!source->error)),
        semantic_field("files_observed", jdv_value_u64(source->files)),
        semantic_field("bytes_observed", jdv_value_filesize(source->bytes)),
        semantic_field("records_observed", jdv_value_u64(source->records)),
        semantic_field("records_maybe_truncated", jdv_value_u64(source->maybe_truncated)),
        semantic_field("record_fidelity", semantic_symbol(sem, "legacy_unescaped_line_writer_may_truncate_at_2040_bytes")),
        semantic_field("consistency", semantic_symbol(sem, "pinned_directory_and_named_file_metadata_before_and_after")),
        semantic_field("error", semantic_symbol(sem, source->error ? memory_store_error(source->error) : NULL)),
        semantic_field("errno", source->error ? jdv_value_i64(source->error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 11);
}
static jdv_value_t impact_native_analysis(semantic_value_t* sem, const char** paths, size_t path_count,
        const impact_native_source_t* source, const memory_fs_t* journal,
        const impact_test_t* tests, int ntests, int nruns, const int* order, size_t hot,
        uint64_t touches, uint64_t offset, uint64_t limit, int error) {
    jdv_value_t inputs[16], rows[64]; size_t shown = 0;
    for (size_t i = 0; i < path_count; i++) inputs[i] = semantic_path(sem, paths[i]);
    if (!error) for (size_t i = (size_t)offset; i < hot && shown < limit; i++) {
        const impact_test_t* item = &tests[order[i]];
        jdv_record_input_t fields[] = {
            semantic_field("rank", jdv_value_u64(i + 1u)),
            semantic_field("command_recorded", semantic_text(sem, item->cmd)),
            semantic_field("score", jdv_value_u64((uint64_t)item->score)),
            semantic_field("runs", jdv_value_u64((uint64_t)item->runs)),
            semantic_field("failures", jdv_value_u64((uint64_t)item->fails))
        };
        rows[shown++] = semantic_record(sem, fields, 5);
    }
    int more = !error && offset + shown < hot;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.impact.v1")),
        semantic_field("kind", semantic_symbol(sem, "analysis")),
        semantic_field("files", semantic_list(sem, inputs, path_count)),
        semantic_field("sessions_source", impact_native_source_value(sem, source)),
        semantic_field("fs_source", memory_fs_source(sem, journal)),
        semantic_field("complete", jdv_value_bool(!error)),
        semantic_field("analysis_scope", semantic_symbol(sem, "retained_temporal_correlation_not_dependency_or_causality_proof")),
        semantic_field("path_matching", semantic_symbol(sem, "exact_or_path_boundary_suffix")),
        semantic_field("ordering", semantic_symbol(sem, "timestamp_then_source_filename_then_line")),
        semantic_field("window_seconds", jdv_value_u64(IMPACT_WINDOW_S)),
        semantic_field("run_weight", jdv_value_u64(2)),
        semantic_field("last_failing_test_success_bonus", jdv_value_u64(5)),
        semantic_field("touches", error ? jdv_value_nothing() : jdv_value_u64(touches)),
        semantic_field("known_test_commands", error ? jdv_value_nothing() : jdv_value_u64((uint64_t)ntests)),
        semantic_field("test_runs", error ? jdv_value_nothing() : jdv_value_u64((uint64_t)nruns)),
        semantic_field("ranked_total", error ? jdv_value_nothing() : jdv_value_u64(hot)),
        semantic_field("cold", error ? jdv_value_nothing() : jdv_value_bool(!hot)),
        semantic_field("recommendations", semantic_list(sem, rows, shown)),
        semantic_field("offset", jdv_value_u64(offset)),
        semantic_field("next_offset", more ? jdv_value_u64(offset + shown) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(more)),
        semantic_field("error", semantic_symbol(sem, error ? memory_store_error(error) : NULL)),
        semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}
static jdv_value_t impact_native_execution(semantic_value_t* sem, const char* command, size_t index,
        const jash_eval_observation_t* observed) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.impact.v1")),
        semantic_field("kind", semantic_symbol(sem, "execution")),
        semantic_field("index", jdv_value_u64(index)),
        semantic_field("command_recorded", semantic_text(sem, command)),
        semantic_field("attempted", jdv_value_bool(observed->attempted)),
        semantic_field("command_status", observed->attempted ? jdv_value_i64(observed->command_status) : jdv_value_nothing()),
        semantic_field("execution_scope", semantic_symbol(sem, "current_shell_state")),
        semantic_field("command_output_destination", semantic_symbol(sem, "stderr")),
        semantic_field("adapter_errno", observed->adapter_error ? jdv_value_i64(observed->adapter_error) : jdv_value_nothing()),
        semantic_field("writer_errno", observed->writer_error ? jdv_value_i64(observed->writer_error) : jdv_value_nothing()),
        semantic_field("restore_errno", observed->restore_error ? jdv_value_i64(observed->restore_error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 11);
}
static jdv_value_t impact_native_summary(semantic_value_t* sem, size_t planned, size_t executed,
        int status, int complete, const char* stopped) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.impact.v1")),
        semantic_field("kind", semantic_symbol(sem, "summary")),
        semantic_field("planned", jdv_value_u64(planned)),
        semantic_field("executed", jdv_value_u64(executed)),
        semantic_field("complete", jdv_value_bool(complete)),
        semantic_field("command_status", jdv_value_i64(status)),
        semantic_field("stopped_reason", semantic_symbol(sem, stopped))
    };
    return semantic_record(sem, fields, 7);
}

static int impact_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    const char* paths[16]; size_t path_count = 0; int execute = 0, literal = 0;
    uint64_t offset = 0, limit = 5; unsigned seen = 0;
    for (int i = 0; i < argc; i++) {
        if (!literal && !strcmp(argv[i], "--")) { literal = 1; continue; }
        if (!literal && !strcmp(argv[i], "--run")) { execute = 1; continue; }
        unsigned bit = !literal && !strcmp(argv[i], "--offset") ? 1 : !literal && !strcmp(argv[i], "--limit") ? 2 : 0;
        if (bit) {
            if ((seen & bit) || ++i == argc || memory_unsigned(argv[i], 64, bit == 1 ? &offset : &limit) || (bit == 2 && !limit))
                return semantic_error(call, "invalid_arguments", 2);
            seen |= bit; continue;
        }
        if ((!literal && argv[i][0] == '-') || !argv[i][0] || path_count == 16) return semantic_error(call, "invalid_arguments", 2);
        paths[path_count++] = argv[i];
    }
    if (!path_count) return semantic_error(call, "missing_file", 2);
    impact_test_t* tests = calloc(IMPACT_MAX_TESTS, sizeof(*tests));
    test_run_t* runs = calloc(IMPACT_MAX_EVENTS, sizeof(*runs));
    if (!tests || !runs) { free(tests); free(runs); return semantic_error(call, "allocation_failed", 125); }
    impact_native_source_t source; int ntests = 0, nruns = 0;
    (void)impact_native_load(state, &source, tests, &ntests, runs, &nruns);
    memory_fs_t journal; (void)memory_fs_load(&journal);
    int error = source.error ? source.error : journal.error; uint64_t touches = 0;
    for (size_t i = 0; i < journal.count && !error; i++) {
        if (jash_query_cancel_poll(state)) { error = ECANCELED; break; }
        int matches = 0;
        for (size_t p = 0; p < path_count && !matches; p++) matches = path_matches(journal.entries[i].path, paths[p]);
        if (!matches) continue;
        touches++;
        if (impact_score_change(tests, runs, nruns, journal.entries[i].seconds, state)) error = ECANCELED;
    }
    int order[IMPACT_MAX_TESTS]; for (int i = 0; i < ntests; i++) order[i] = i;
    for (int i = 0; i < ntests; i++) for (int j = i + 1; j < ntests; j++) {
        const impact_test_t *a = &tests[order[i]], *b = &tests[order[j]];
        if (b->score > a->score || (b->score == a->score && strcmp(b->cmd, a->cmd) < 0)) {
            int swapped = order[i]; order[i] = order[j]; order[j] = swapped;
        }
    }
    size_t hot = 0; for (int i = 0; i < ntests; i++) hot += tests[i].score > 0;
    int status = error ? error == E2BIG ? 75 : error == ECANCELED ? jash_query_cancel_poll(state) : 1 : 0;
    if (error == ECANCELED && status < 128) status = 130;
    if (error == ECANCELED) call->cancellation_status = status;
    jdv_value_t analysis = impact_native_analysis(&call->values, paths, path_count, &source, &journal,
        tests, ntests, nruns, order, hot, touches, offset, limit, error);
    if (!execute || status) { status = semantic_finish(call, analysis, status); goto cleanup; }
    size_t planned = hot > 3 ? 3 : hot; jdv_value_t possible[5]; possible[0] = analysis;
    jash_eval_observation_t worst = {.attempted = 1,.command_status = INT_MIN,
        .adapter_error = INT_MAX,.writer_error = INT_MAX,.restore_error = INT_MAX};
    for (size_t i = 0; i < planned; i++) possible[i + 1] = impact_native_execution(&call->values, tests[order[i]].cmd, i, &worst);
    possible[planned + 1] = impact_native_summary(&call->values, planned, planned, INT_MIN, 0, "evaluation_or_delivery_observation_failed");
    status = semantic_events_preflight(call, possible, planned + 2);
    if (status) { status = semantic_error(call, "receipt_admission_failed", status); goto cleanup; }
    if (semantic_admit_rows(call, planned + 2)) { status = 125; goto cleanup; }
    if (semantic_events_emit(call, analysis)) { status = 125; goto cleanup; }
    size_t executed = 0; const char* stopped = NULL; int complete = 1, delivery_failed = 0;
    for (size_t i = 0; i < planned; i++) {
        jash_eval_observation_t observed;
        (void)jash_eval_with_output(state, tests[order[i]].cmd, STDERR_FILENO, -1, &observed);
        executed += observed.attempted != 0;
        call->values.arena.used = 0; call->values.error = 0;
        if (semantic_events_emit(call, impact_native_execution(&call->values, tests[order[i]].cmd, i, &observed))) {
            delivery_failed = 1; status = 125; break;
        }
        status = observed.adapter_error ? 125 : observed.command_status;
        if (observed.adapter_error) { complete = 0; stopped = "evaluation_or_delivery_observation_failed"; break; }
        if (status) { stopped = "first_nonzero_test_status"; break; }
        if (state->exit_requested || state->return_requested || jash_loopctl_pending(state)) {
            complete = 0; status = 75; stopped = "shell_control_requested"; break;
        }
    }
    if (!delivery_failed) {
        call->values.arena.used = 0; call->values.error = 0;
        jdv_value_t summary = impact_native_summary(&call->values, planned, executed, status, complete, stopped);
        /* Finishing via explicit outcome emits the final summary item and
         * preserves a fully observed stop-on-failure predicate as COMPLETE. */
        jdv_stream_end_t outcome; (void)jdv_stream_end_init(&outcome);
        outcome.state = complete ? JDV_STREAM_COMPLETE : JDV_STREAM_FAILED;
        outcome.completeness = complete ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
        outcome.errors_seen = complete ? 0 : 1; outcome.producer_status = status;
        if (!complete && stopped) outcome.reason = (jdv_slice_t){(const uint8_t*)stopped,strlen(stopped)};
        status = semantic_finish_outcome(call, summary, &outcome);
    }
cleanup:
    memory_fs_free(&journal); free(tests); free(runs); return status;
}
SEMANTIC_ARGV_WRAPPER(impact, "impact", "jash.impact.v1", impact_native_provider)
