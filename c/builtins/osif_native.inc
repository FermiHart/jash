// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Additional OSIF modes consume their actual models and recorded artifacts.
 * The legacy caps/check/score projection remains unchanged. */
#include <stdlib.h>

#define OSIF_MATRIX_REPORTS 256u
#define OSIF_MATRIX_ENTRIES 65536u
#define OSIF_MATRIX_FILE_BYTES (256u * 1024u)
#define OSIF_MATRIX_TOTAL_BYTES (16u * 1024u * 1024u)
#define OSIF_MATRIX_LINE_BYTES 16384u

typedef struct {
    int mode, selected;
    const char* operation;
    const char* operand;
    const char* error;
} osif_arguments_t;

static osif_arguments_t osif_arguments(int argc, char* const* argv) {
    osif_arguments_t result = {0};
    result.selected = jash_discovery_format(argc, argv, &result.mode);
    if (!result.selected) return result;
    if (result.selected < 0) { result.error = "conflicting_formats"; return result; }
    int ended = 0; size_t bytes = 0;
    if (argc > 64) { result.error = "argument_limit"; return result; }
    for (int i = 0; i < argc; i++) {
        size_t length = strlen(argv[i]);
        if (length > 4096u - bytes) { result.error = "argument_limit"; return result; }
        bytes += length;
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && argv[i][0] == '-') { result.error = "invalid_options"; return result; }
        if (!result.operation) result.operation = argv[i];
        else if (!result.operand) result.operand = argv[i];
        else { result.error = "invalid_arguments"; return result; }
    }
    if (!result.operation) result.operation = "caps";
    int operand = !strcmp(result.operation, "explain") || !strcmp(result.operation, "port");
    if ((operand && (!result.operand || !*result.operand)) || (!operand && result.operand)) result.error = "invalid_arguments";
    if (strcmp(result.operation, "caps") && strcmp(result.operation, "check") && strcmp(result.operation, "score") &&
        strcmp(result.operation, "report") && strcmp(result.operation, "matrix") && !operand)
        result.error = "unknown_operation";
    return result;
}

const char* jash_osif_effect_argv(int argc, char* const* argv) {
    osif_arguments_t arguments = osif_arguments(argc, argv);
    if (!arguments.selected || arguments.error) return "exec";
    if (!strcmp(arguments.operation, "caps") || !strcmp(arguments.operation, "matrix") || !strcmp(arguments.operation, "explain")) return "read";
    if (!strcmp(arguments.operation, "port")) return "mutate";
    return "exec";
}

typedef struct {
    jash_discovery_rows_t* rows;
    const char* operation;
    const char* target;
    int failures;
} osif_native_output_t;

static int osif_native_error(osif_native_output_t* output, const char* path,
                              const char* error, int number, int status) {
    jash_discovery_rows_t* rows = output->rows;
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "error")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, output->operation)},
        {JDV_LITERAL("target"), jash_discovery_string(rows, output->target)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
        {JDV_LITERAL("errno"), number ? jdv_value_i64(number) : jdv_value_nothing()},
        {JDV_LITERAL("status"), jdv_value_i64(status)}
    };
    output->failures++;
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    return jash_discovery_effects_flush(rows);
}

static int osif_native_receipt(void* context, const osif_publish_receipt_t* receipt) {
    osif_native_output_t* output = context; jash_discovery_rows_t* rows = output->rows;
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "effect")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, output->operation)},
        {JDV_LITERAL("target"), jash_discovery_string(rows, output->target)},
        {JDV_LITERAL("phase"), jash_discovery_string(rows, receipt->phase)},
        {JDV_LITERAL("action"), jash_discovery_string(rows, receipt->action)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, receipt->path)},
        {JDV_LITERAL("ok"), jdv_value_bool(!receipt->error)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, receipt->error)},
        {JDV_LITERAL("errno"), receipt->error_number ? jdv_value_i64(receipt->error_number) : jdv_value_nothing()},
        {JDV_LITERAL("applied"), jdv_value_bool(receipt->applied)},
        {JDV_LITERAL("durable"), receipt->durable < 0 ? jdv_value_nothing() : jdv_value_bool(receipt->durable)},
        {JDV_LITERAL("expected_bytes"), jdv_value_u64(receipt->expected_bytes)},
        {JDV_LITERAL("written_bytes"), jdv_value_u64(receipt->written_bytes)},
        {JDV_LITERAL("existing_bytes"), receipt->existing_size_known ? jdv_value_u64(receipt->existing_bytes) : jdv_value_nothing()},
        {JDV_LITERAL("temporary_created"), jdv_value_bool(receipt->temporary_created)},
        {JDV_LITERAL("temporary_retained"), jash_discovery_path(rows, receipt->temporary_path)},
        {JDV_LITERAL("cleanup_errno"), receipt->cleanup_error ? jdv_value_i64(receipt->cleanup_error) : jdv_value_nothing()}
    };
    if (receipt->error) output->failures++;
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    return jash_discovery_effects_flush(rows);
}

static int osif_native_summary(osif_native_output_t* output, int complete, int status,
                                const char* reason, const osif_publish_store_t* store,
                                uint64_t reports, uint64_t source_bytes) {
    jash_discovery_rows_t* rows = output->rows;
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "summary")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, output->operation)},
        {JDV_LITERAL("target"), jash_discovery_string(rows, output->target)},
        {JDV_LITERAL("operation_complete"), jdv_value_bool(complete)},
        {JDV_LITERAL("status"), jdv_value_i64(status)},
        {JDV_LITERAL("reason"), jash_discovery_string(rows, reason)},
        {JDV_LITERAL("failures"), jdv_value_u64((uint64_t)output->failures)},
        {JDV_LITERAL("effects_applied"), jdv_value_u64(store ? store->applied : 0)},
        {JDV_LITERAL("effects_durable"), jdv_value_u64(store ? store->durable : 0)},
        {JDV_LITERAL("files_preserved"), jdv_value_u64(store ? store->preserved : 0)},
        {JDV_LITERAL("reports_observed"), jdv_value_u64(reports)},
        {JDV_LITERAL("source_bytes"), jdv_value_u64(source_bytes)},
        {JDV_LITERAL("atomic_batch"), jdv_value_bool(0)}
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

static int osif_native_probe(jash_discovery_rows_t* rows, const osif_probe_snapshot_t* snapshot,
                             const char* operation, int index, int with_record) {
    const probe_t* probe = &k_probes[index];
    int code = snapshot->codes[index], probing = strcmp(operation, "caps") != 0;
    int advertised = (snapshot->caps & probe->cap_bit) != 0;
    const char* status = probing ? (code == PROBE_PASS ? "pass" : code == PROBE_SKIP ? "skip" : "fail") :
        advertised ? "advertised" : "unavailable";
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("target"), jash_discovery_string(rows, bear_osinfo_target())},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("capability"), jash_discovery_string(rows, probe->cap_name)},
        {JDV_LITERAL("bit"), jdv_value_u64(probe->cap_bit)},
        {JDV_LITERAL("provider"), jash_discovery_string(rows, probe->probe_name)},
        {JDV_LITERAL("advertised"), jdv_value_bool(advertised)},
        {JDV_LITERAL("status"), jash_discovery_string(rows, status)},
        {JDV_LITERAL("detail"), jash_discovery_string(rows, probing ? snapshot->details[index] : NULL)},
        {JDV_LITERAL("record"), jash_discovery_string(rows, "probe")}
    };
    return jash_discovery_add(rows, fields, with_record ? 9 : 8);
}

static int osif_native_report_model(osif_native_output_t* output, const osif_report_t* report) {
    jash_discovery_rows_t* rows = output->rows;
    for (int i = 0; i < k_nprobes; i++)
        if (osif_native_probe(rows, &report->probes, "report", i, 1)) return 75;
    jdv_value_t host = jdv_value_nothing();
    if (report->host_available) {
        jdv_record_input_t identity[] = {
            {JDV_LITERAL("sysname"), jash_discovery_string(rows, report->host.sysname)},
            {JDV_LITERAL("release"), jash_discovery_string(rows, report->host.release)},
            {JDV_LITERAL("machine"), jash_discovery_string(rows, report->host.machine)}
        };
        if (jdv_value_record(&rows->arena, identity, 3, &host)) { rows->error = 1; return 75; }
    }
    char tier[2] = {tier_letter(report->probes.passes, k_nprobes), 0};
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "report")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, "report")},
        {JDV_LITERAL("target"), jash_discovery_string(rows, output->target)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, report->path)},
        {JDV_LITERAL("generated_seconds"), jdv_value_i64((int64_t)report->generated)},
        {JDV_LITERAL("generated_local"), jash_discovery_string(rows, report->generated_local)},
        {JDV_LITERAL("host"), host},
        {JDV_LITERAL("tier"), jash_discovery_string(rows, tier)},
        {JDV_LITERAL("passed"), jdv_value_u64((uint64_t)report->probes.passes)},
        {JDV_LITERAL("failed"), jdv_value_u64((uint64_t)report->probes.failures)},
        {JDV_LITERAL("skipped"), jdv_value_u64((uint64_t)report->probes.skips)},
        {JDV_LITERAL("total"), jdv_value_u64(k_nprobes)},
        {JDV_LITERAL("artifact_bytes"), jdv_value_u64(report->length)},
        {JDV_LITERAL("phase"), jash_discovery_string(rows, "prepared")}
    };
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    return jash_discovery_effects_flush(rows);
}

static int osif_native_report(jash_writer_t writer, void* context, int mode, jash_discovery_rows_t* rows) {
    osif_native_output_t output = {rows, "report", bear_osinfo_target(), 0};
    int status = jash_discovery_effects_begin(rows, writer, context, "osif", mode, "jash.osif.v1");
    if (status) return status;
    osif_report_t report; osif_publish_store_t store = {0};
    if (osif_report_prepare(&report)) {
        int saved = errno;
        status = osif_native_error(&output, NULL, "report_prepare_failed", saved, 1);
        if (!status) status = 1;
    } else {
        status = osif_native_report_model(&output, &report);
        if (!status) status = osif_report_publish(&report, osif_native_receipt, &output, &store);
        if (!status && report.probes.failures) status = 1;
    }
    int complete = !output.failures && status != 75 && status != 125;
    const char* reason = complete ? NULL : status == 125 ? "delivery_failed" : "publication_failed";
    (void)osif_native_summary(&output, complete, status, reason, &store, 0, 0);
    return jash_discovery_effects_end_outcome(rows, status,
        complete ? JDV_STREAM_COMPLETE : JDV_STREAM_FAILED, rows->error, reason);
}

static int osif_native_port(jash_writer_t writer, void* context, int mode, jash_discovery_rows_t* rows, const char* target) {
    jash_osif_port_plan_t plan;
    if (jash_osif_port_prepare(target, &plan))
        return jash_discovery_error(writer, context, "osif", mode, "invalid_target", 2);
    int status = jash_discovery_effects_begin(rows, writer, context, "osif", mode, "jash.osif.v1");
    if (status) return status;
    osif_native_output_t output = {rows, "port", target, 0}; osif_publish_store_t store;
    status = jash_osif_port_apply(&plan, osif_native_receipt, &output, &store);
    const char* reason = status ? status == 125 ? "delivery_failed" : "publication_failed" : NULL;
    (void)osif_native_summary(&output, !status, status, reason, &store, 0, 0);
    return jash_discovery_effects_end_outcome(rows, status, status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE, rows->error, reason);
}

static int osif_native_explain(jash_writer_t writer, void* context, int mode,
                                jash_discovery_rows_t* rows, const char* provider) {
    size_t index = 0;
    while (osif_lessons[index].cap && strcmp(provider, osif_lessons[index].cap)) index++;
    if (!osif_lessons[index].cap) return jash_discovery_error(writer, context, "osif", mode, "unknown_provider", 2);
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "explanation")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, "explain")},
        {JDV_LITERAL("target"), jash_discovery_string(rows, bear_osinfo_target())},
        {JDV_LITERAL("provider"), jash_discovery_string(rows, osif_lessons[index].cap)},
        {JDV_LITERAL("capability"), jash_discovery_string(rows, k_probes[index].cap_name)},
        {JDV_LITERAL("contract"), jash_discovery_string(rows, osif_lessons[index].doc)},
        {JDV_LITERAL("source"), jash_discovery_string(rows, "builtin-provider-contract")}
    };
    (void)jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
    return jash_discovery_emit_schema(writer, context, "osif", mode, rows, 0, "jash.osif.v1");
}

typedef struct { char tier; uint64_t passed, failed, skipped, total; } osif_matrix_score_t;
static int osif_matrix_literal(const char** cursor, const char* literal) {
    size_t length = strlen(literal);
    if (strncmp(*cursor, literal, length)) return -1;
    *cursor += length; return 0;
}
static int osif_matrix_number(const char** cursor, uint64_t* value) {
    const unsigned char* p = (const unsigned char*)*cursor; uint64_t number = 0;
    if (*p < '0' || *p > '9') return -1;
    do {
        unsigned digit = *p++ - '0';
        if (number > (UINT64_MAX - digit) / 10u) return -1;
        number = number * 10u + digit;
    } while (*p >= '0' && *p <= '9');
    *cursor = (const char*)p; *value = number; return 0;
}
static int osif_matrix_score(const char* line, osif_matrix_score_t* score) {
    const char* p = line;
    if (osif_matrix_literal(&p, "**Tier:** ") || *p < 'A' || *p > 'D') return -1;
    score->tier = *p++;
    if (osif_matrix_literal(&p, " — ") || osif_matrix_number(&p, &score->passed) ||
        osif_matrix_literal(&p, "/") || osif_matrix_number(&p, &score->total) ||
        osif_matrix_literal(&p, " providers PASS, ") || osif_matrix_number(&p, &score->failed) ||
        osif_matrix_literal(&p, " FAIL, ") || osif_matrix_number(&p, &score->skipped) ||
        osif_matrix_literal(&p, " SKIP") || *p || !score->total || score->passed > score->total ||
        score->failed > score->total - score->passed ||
        score->skipped != score->total - score->passed - score->failed) return -1;
    char expected = score->passed == score->total ? 'A' :
        score->passed >= score->total - score->total / 4u ? 'B' :
        score->passed >= score->total / 2u + score->total % 2u ? 'C' : 'D';
    return score->tier == expected ? 0 : -1;
}

static int osif_matrix_parse(char* bytes, size_t length, const char* target, osif_matrix_score_t* score) {
    if (memchr(bytes, 0, length)) { errno = EINVAL; return -1; }
    size_t at = 0; int title_seen = 0, tier_seen = 0;
    while (at < length) {
        size_t start = at; while (at < length && bytes[at] != '\n') at++;
        size_t size = at - start;
        if (size > OSIF_MATRIX_LINE_BYTES) { errno = E2BIG; return -1; }
        if (at < length) bytes[at++] = 0;
        if (size && bytes[start + size - 1] == '\r') bytes[start + --size] = 0;
        char* line = bytes + start;
        if (!strncmp(line, "# OSIF Conformance — ", strlen("# OSIF Conformance — "))) {
            if (title_seen++ || strcmp(line + strlen("# OSIF Conformance — "), target)) { errno = EINVAL; return -1; }
        }
        if (!strncmp(line, "**Tier:**", 9)) {
            if (tier_seen++ || osif_matrix_score(line, score)) { errno = EINVAL; return -1; }
        }
    }
    if (title_seen != 1 || tier_seen != 1) { errno = EINVAL; return -1; }
    return 0;
}

static int osif_matrix_stable(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        a->st_mtim.tv_sec == b->st_mtim.tv_sec && a->st_mtim.tv_nsec == b->st_mtim.tv_nsec &&
        a->st_ctim.tv_sec == b->st_ctim.tv_sec && a->st_ctim.tv_nsec == b->st_ctim.tv_nsec;
}

/* Returns shell status, with an error code only for an observed source fault.
 * Cancellation is polled between bounded reads and again after EOF/close. */
static int osif_matrix_read(jash_state_t* state, int directory, const char* name,
                             char** bytes, size_t* length, uint64_t* total,
                             const char** error, int* error_number) {
    *bytes = NULL; *length = 0; *error = NULL; *error_number = 0;
    int cancelled = jash_query_cancel_poll(state); if (cancelled) return cancelled;
    int fd = openat(directory, name, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) { *error = "report_open_failed"; *error_number = errno; return 1; }
    struct stat before, after; int status = 0;
    if (fstat(fd, &before)) { *error = "report_stat_failed"; *error_number = errno; status = 1; }
    else if (!S_ISREG(before.st_mode)) { *error = "report_not_regular"; *error_number = EINVAL; status = 1; }
    else if (before.st_size < 0 || (uint64_t)before.st_size > OSIF_MATRIX_FILE_BYTES ||
             (uint64_t)before.st_size > OSIF_MATRIX_TOTAL_BYTES - *total) {
        *error = "source_byte_limit"; *error_number = E2BIG; status = 75;
    }
    char* data = status ? NULL : malloc(OSIF_MATRIX_FILE_BYTES + 1u);
    if (!status && !data) { *error = "allocation_failed"; *error_number = ENOMEM; status = 125; }
    size_t used = 0;
    while (!status) {
        cancelled = jash_query_cancel_poll(state); if (cancelled) { status = cancelled; break; }
        size_t available = OSIF_MATRIX_FILE_BYTES + 1u - used;
        ssize_t count = read(fd, data + used, available < 16384u ? available : 16384u);
        if (count < 0 && errno == EINTR) continue;
        if (count < 0) { *error = "report_read_failed"; *error_number = errno; status = 1; break; }
        if (!count) break;
        used += (size_t)count; *total += (uint64_t)count;
        if (used > OSIF_MATRIX_FILE_BYTES || *total > OSIF_MATRIX_TOTAL_BYTES) {
            *error = "source_byte_limit"; *error_number = E2BIG; status = 75;
        }
    }
    if (!status) {
        if (fstat(fd, &after)) { *error = "report_stat_failed"; *error_number = errno; status = 1; }
        else if (!osif_matrix_stable(&before, &after) || (uint64_t)before.st_size != used) {
            *error = "report_changed"; *error_number = ESTALE; status = 1;
        }
    }
    if (close(fd) && !status) { *error = "report_close_failed"; *error_number = errno; status = 1; }
    cancelled = jash_query_cancel_poll(state); if (cancelled) status = cancelled;
    if (status) { free(data); return status; }
    data[used] = 0; *bytes = data; *length = used; return 0;
}

static int osif_matrix_compare_names(const void* left, const void* right) {
    return strcmp(*(char* const*)left, *(char* const*)right);
}

static int osif_native_matrix(jash_state_t* state, jash_writer_t writer, void* context,
                               int mode, jash_discovery_rows_t* rows) {
    osif_native_output_t output = {rows, "matrix", bear_osinfo_target(), 0};
    osif_publish_store_t store;
    int status = osif_publish_open(&store, osif_native_receipt, &output);
    if (!status) status = osif_publish_enter(&store, "Docs", 0);
    if (!status) status = osif_publish_enter(&store, "ports", 0);
    char* names[OSIF_MATRIX_REPORTS] = {0}; size_t count = 0;
    uint64_t source_bytes = 0, observed = 0; const char* reason = status ? "source_failed" : NULL;
    int directory = status ? -1 : store.descriptors[store.depth - 1];
    int duplicate = directory < 0 ? -1 : fcntl(directory, F_DUPFD_CLOEXEC, 3);
    DIR* stream = duplicate < 0 ? NULL : fdopendir(duplicate);
    if (!status && !stream) {
        int saved = errno; if (duplicate >= 0) (void)close(duplicate);
        (void)osif_native_error(&output, "Docs/ports", "directory_open_failed", saved, 1);
        status = 1; reason = "source_failed";
    }
    struct stat before, after; int snapshot_valid = directory >= 0 && !fstat(directory, &before);
    if (stream) {
        size_t visits = 0;
        for (;;) {
            int cancelled = jash_query_cancel_poll(state);
            if (cancelled) { status = cancelled; reason = "request_cancelled"; break; }
            errno = 0; struct dirent* entry = readdir(stream);
            if (!entry) {
                if (errno) { (void)osif_native_error(&output, "Docs/ports", "directory_read_failed", errno, 1); status = 1; reason = "source_failed"; }
                break;
            }
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            if (++visits > OSIF_MATRIX_ENTRIES) { status = 75; reason = "directory_entry_limit"; break; }
            const char* suffix = "-conformance.md"; size_t length = strlen(entry->d_name), tail = strlen(suffix);
            if (length <= tail || strcmp(entry->d_name + length - tail, suffix)) continue;
            if (count == OSIF_MATRIX_REPORTS) { status = 75; reason = "report_count_limit"; break; }
            names[count] = strdup(entry->d_name);
            if (!names[count]) { status = 125; reason = "allocation_failed"; break; }
            count++;
        }
        if (closedir(stream)) { (void)osif_native_error(&output, "Docs/ports", "directory_close_failed", errno, 1); if (!status) status = 1;
            if (!reason) reason = "source_failed"; }
    }
    if (status == 75 || status == 125) (void)osif_native_error(&output, "Docs/ports", reason, status == 75 ? E2BIG : ENOMEM, status);
    qsort(names, count, sizeof names[0], osif_matrix_compare_names);
    if (status < 128) for (size_t index = 0; index < count; index++) {
        char path[OSIF_PUBLISH_PATH]; int n = snprintf(path, sizeof path, "Docs/ports/%s", names[index]);
        if (n < 0 || (size_t)n >= sizeof path) { status = 75; reason = "path_limit"; break; }
        char* bytes = NULL; size_t length = 0; const char* error = NULL; int number = 0;
        int read_status = osif_matrix_read(state, directory, names[index], &bytes, &length, &source_bytes, &error, &number);
        if (read_status) {
            if (read_status >= 128) { status = read_status; reason = "request_cancelled"; break; }
            (void)osif_native_error(&output, path, error, number, read_status);
            if (!status || read_status > status) status = read_status;
            if (!reason) reason = error;
            if (read_status >= 75) break;
            continue;
        }
        char target[256]; size_t target_length = strlen(names[index]) - strlen("-conformance.md");
        if (target_length >= sizeof target) { free(bytes); status = 75; reason = "target_limit"; break; }
        memcpy(target, names[index], target_length); target[target_length] = 0;
        osif_matrix_score_t score = {0};
        if (osif_matrix_parse(bytes, length, target, &score)) {
            int saved = errno; free(bytes);
            int invalid_status = saved == E2BIG ? 75 : 1;
            (void)osif_native_error(&output, path, saved == E2BIG ? "report_line_limit" : "invalid_report", saved, invalid_status);
            if (!status || invalid_status > status) status = invalid_status;
            if (!reason) reason = "invalid_report";
            continue;
        }
        free(bytes); char tier[2] = {score.tier, 0};
        jdv_record_input_t fields[] = {
            {JDV_LITERAL("record"), jash_discovery_string(rows, "matrix")},
            {JDV_LITERAL("operation"), jash_discovery_string(rows, "matrix")},
            {JDV_LITERAL("target"), jash_discovery_string(rows, target)},
            {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
            {JDV_LITERAL("tier"), jash_discovery_string(rows, tier)},
            {JDV_LITERAL("passed"), jdv_value_u64(score.passed)},
            {JDV_LITERAL("failed"), jdv_value_u64(score.failed)},
            {JDV_LITERAL("skipped"), jdv_value_u64(score.skipped)},
            {JDV_LITERAL("total"), jdv_value_u64(score.total)},
            {JDV_LITERAL("artifact_bytes"), jdv_value_u64(length)},
            {JDV_LITERAL("evidence"), jash_discovery_string(rows, "recorded-report")},
            {JDV_LITERAL("verification"), jash_discovery_string(rows, "format-only")}
        };
        if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) { status = 75; reason = "collection_limit"; break; }
        observed++;
    }
    for (size_t index = 0; index < count; index++) free(names[index]);
    if (directory >= 0 && status < 128) {
        if (!snapshot_valid || fstat(directory, &after)) {
            (void)osif_native_error(&output, "Docs/ports", "directory_stat_failed", errno ? errno : EIO, 1);
            if (!status) status = 1;
            if (!reason) reason = "source_failed";
        } else if (!osif_matrix_stable(&before, &after)) {
            (void)osif_native_error(&output, "Docs/ports", "directory_changed", ESTALE, 1);
            if (!status) status = 1;
            if (!reason) reason = "source_changed";
        }
    }
    status = osif_publish_close(&store, status);
    int cancelled = jash_query_cancel_poll(state); if (cancelled) { status = cancelled; reason = "request_cancelled"; }
    (void)osif_native_summary(&output, !status, status, reason, NULL, observed, source_bytes);
    if (mode == JASH_DISCOVERY_DATA && status) return jash_discovery_emit_data_outcome(writer, context, "osif", rows, status,
        status >= 128 ? JDV_STREAM_CANCELLED : status == 75 ? JDV_STREAM_INCOMPLETE : JDV_STREAM_FAILED, reason ? reason : "source_failed");
    return jash_discovery_emit_schema(writer, context, "osif", mode, rows, status, "jash.osif.v1");
}
