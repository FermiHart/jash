// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "analysis_output.h"
#include "semantic_builtin.h"
#include "query_snapshot.h"
#include <core/query_cancel.h>

#define LINT_NATIVE_BYTES (1024u * 1024u)
#define LINT_NATIVE_LINES 65536u
#define LINT_NATIVE_LINE_BYTES 65536u
#define LINT_NATIVE_RESULTS 1024u

static jdv_value_t lint_native_result(semantic_value_t* sem, const lint_result_t* row) {
    jdv_record_input_t fields[] = {
        semantic_field("line", jdv_value_u64(row->line)),
        semantic_field("col", jdv_value_u64(row->col)),
        semantic_field("severity", semantic_symbol(sem, sev_label(row->sev))),
        semantic_field("rule_id", semantic_symbol(sem, row->rule_id)),
        semantic_field("message", semantic_text(sem, row->message)),
        semantic_field("fix_hint", semantic_text(sem, row->fix_hint))
    };
    return semantic_record(sem, fields, 6);
}

static int lint_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    int inline_code = argc == 2 && !strcmp(argv[0], "-c");
    const char* input = inline_code ? argv[1] : argc == 2 && !strcmp(argv[0], "--") ? argv[1] :
        argc == 1 && argv[0][0] != '-' ? argv[0] : NULL;
    if (!input || (!inline_code && !*input)) return semantic_error(call, "invalid_arguments", 2);
    jash_query_file_page_t page = {0}; int status = 0, number = 0, cancelled = 0, source_complete = inline_code;
    const char* error = NULL; char* content = NULL; size_t bytes = 0;
    if (inline_code) { content = strdup(input); bytes = strlen(input); }
    else {
        status = jash_query_file_page(state, input, 0, LINT_NATIVE_BYTES, &page);
        if (status) { error = page.error; number = page.error_number; }
        else if ((uint64_t)page.metadata.st_size > LINT_NATIVE_BYTES) { status = 75; error = "source_byte_limit"; }
        else { source_complete = 1; content = (char*)page.bytes; bytes = page.used; page.bytes = NULL; }
    }
    if (!status && !content) { error = "allocation_failed"; status = 125; }
    if (!status && memchr(content, 0, bytes)) { error = "embedded_nul_not_supported"; status = 1; }
    if (status >= 128 && jash_query_cancel_poll(state)) cancelled = status;
    lint_result_t* results = calloc(LINT_NATIVE_RESULTS, sizeof(*results)); unsigned count = 0, lines = 0;
    if (!results && !status) { error = "allocation_failed"; status = 125; }
    size_t offset = 0;
    while (!status && offset < bytes) {
        if ((cancelled = jash_query_cancel_poll(state))) { error = "cancelled"; status = cancelled; break; }
        if (lines == LINT_NATIVE_LINES) { error = "source_line_limit"; status = 75; break; }
        char* begin = content + offset; char* end = memchr(begin, '\n', bytes - offset);
        size_t length = end ? (size_t)(end - begin) : bytes - offset;
        offset += length + (end != NULL); lines++;
        if (length > LINT_NATIVE_LINE_BYTES) { error = "line_byte_limit"; status = 75; break; }
        while (length && begin[length - 1] == '\r') length--;
        begin[length] = 0;
        lint_result_t row[sizeof(LINT_RULES) / sizeof(LINT_RULES[0])]; unsigned found = 0;
        (void)lint_line(begin, lines, row, (unsigned)(sizeof(row) / sizeof(row[0])), &found);
        if (found > LINT_NATIVE_RESULTS - count) { error = "diagnostic_limit"; status = 75; break; }
        memcpy(results + count, row, found * sizeof(*row)); count += found;
    }
    if (!status && !inline_code && bytes && !(content[0] == '#' && content[1] == '!')) {
        if (count == LINT_NATIVE_RESULTS) { error = "diagnostic_limit"; status = 75; }
        else results[count++] = (lint_result_t){1,1,LINT_INFO,"SHEBANG_MISS","script has no shebang line",
            "Add: #!/usr/bin/env bash at the top of the script"};
    }
    if (!status && (cancelled = jash_query_cancel_poll(state))) { error = "cancelled"; status = cancelled; }
    semantic_value_t* sem = &call->values;
    /* JDV counts fields across the complete value: 1024 diagnostics alone
     * contain 6144 fields, in addition to the source/outcome fields. */
    sem->limits.max_record_fields = 8192;
    jdv_value_t* items = count ? malloc(count * sizeof(*items)) : NULL;
    if (count && !items) { count = 0; status = 125; error = "allocation_failed"; }
    uint64_t severities[4] = {0};
    for (unsigned i = 0; i < count; i++) { items[i] = lint_native_result(sem, &results[i]); severities[results[i].sev]++; }
    int predicate = severities[LINT_ERROR] || severities[LINT_WARNING];
    jdv_record_input_t source_fields[] = {
        semantic_field("kind", semantic_symbol(sem, inline_code ? "inline" : "file")),
        semantic_field("path", inline_code ? jdv_value_nothing() : semantic_path(sem, input)),
        semantic_field("read_complete", jdv_value_bool(source_complete)),
        semantic_field("bytes", source_complete ? jdv_value_filesize(bytes) : jdv_value_nothing()),
        semantic_field("lines_scanned", jdv_value_u64(lines)),
        semantic_field("consistency", semantic_symbol(sem, inline_code ? "literal_argv" : "named_file_metadata_before_and_after"))
    };
    jdv_record_input_t counts[] = {
        semantic_field("errors", jdv_value_u64(severities[0])),
        semantic_field("warnings", jdv_value_u64(severities[1])),
        semantic_field("style", jdv_value_u64(severities[2])),
        semantic_field("info", jdv_value_u64(severities[3]))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.lint.v1")),
        semantic_field("source", semantic_record(sem, source_fields, 6)),
        semantic_field("complete", jdv_value_bool(!status)),
        semantic_field("lint", semantic_list(sem, items, count)),
        semantic_field("diagnostics_returned", jdv_value_u64(count)),
        semantic_field("total", status ? jdv_value_nothing() : jdv_value_u64(count)),
        semantic_field("counts", semantic_record(sem, counts, 4)),
        semantic_field("predicate_status", status ? jdv_value_nothing() : jdv_value_i64(predicate)),
        semantic_field("analysis_scope", semantic_symbol(sem, "registered_line_pattern_rules_not_full_shell_semantics")),
        semantic_field("column_basis", semantic_symbol(sem, "rule_reports_line_start")),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("errno", number ? jdv_value_i64(number) : jdv_value_nothing())
    };
    jdv_value_t value = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    jdv_stream_end_t outcome; (void)jdv_stream_end_init(&outcome);
    outcome.state = cancelled ? JDV_STREAM_CANCELLED : status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    outcome.completeness = status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
    outcome.producer_status = status ? status : predicate; outcome.errors_seen = status ? 1 : 0;
    if (error) outcome.reason = (jdv_slice_t){(const uint8_t*)error,strlen(error)};
    int rc = semantic_finish_outcome(call, value, &outcome);
    free(items); free(results); free(content); free(page.bytes); return rc;
}

SEMANTIC_ARGV_WRAPPER(lint, "lint", "jash.lint.v1", lint_native_provider)
