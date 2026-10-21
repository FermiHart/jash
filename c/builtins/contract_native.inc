// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
#include "analysis_output.h"
#include "semantic_builtin.h"
#include "memory_store.h"

static const char* contract_native_kind(exp_kind_t kind) {
    static const char* names[] = {"rc","stdout-contains","stdout-empty","stdout-matches","stderr-contains","stderr-empty"};
    return names[kind];
}
static long contract_native_discard(void* context, const char* bytes, size_t size) {
    (void)context; (void)bytes; return size > LONG_MAX ? -1 : (long)size;
}
static jdv_value_t contract_native_report(semantic_value_t* sem, int argc, char** argv,
        const assert_t* assertions, size_t count, const analysis_capture_t* capture,
        const int* passed, int matched, const char* error, int status) {
    jdv_value_t words[64], checks[CONTRACT_MAX_ASSERTS];
    for (int i = 0; i < argc; i++) words[i] = semantic_text(sem, argv[i]);
    for (size_t i = 0; i < count; i++) {
        int valued = assertions[i].kind == EXP_STDOUT_CONTAINS || assertions[i].kind == EXP_STDOUT_MATCHES ||
                     assertions[i].kind == EXP_STDERR_CONTAINS;
        jdv_record_input_t fields[] = {
            semantic_field("kind", semantic_symbol(sem, contract_native_kind(assertions[i].kind))),
            semantic_field("expected", assertions[i].kind == EXP_RC ? jdv_value_i64(assertions[i].rc) :
                valued ? semantic_text(sem, assertions[i].str) : jdv_value_nothing()),
            semantic_field("evaluated", jdv_value_bool(passed && passed[i] >= 0)),
            semantic_field("passed", passed && passed[i] >= 0 ? jdv_value_bool(passed[i]) : jdv_value_nothing())
        };
        checks[i] = semantic_record(sem, fields, 4);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.contract.v1")),
        semantic_field("argv", semantic_list(sem, words, (size_t)argc)),
        semantic_field("execution_scope", semantic_symbol(sem, "external_literal_argv")),
        semantic_field("effects_may_have_applied", jdv_value_bool(capture->execution.attempted)),
        semantic_field("execution", analysis_capture_value(sem, capture)),
        semantic_field("expectations", semantic_list(sem, checks, count)),
        semantic_field("complete", jdv_value_bool(error == NULL)),
        semantic_field("matched", error ? jdv_value_nothing() : jdv_value_bool(matched)),
        semantic_field("predicate_status", error ? jdv_value_nothing() : jdv_value_i64(status)),
        semantic_field("error", semantic_symbol(sem, error))
    };
    return semantic_record(sem, fields, 10);
}

static int contract_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    assert_t assertions[CONTRACT_MAX_ASSERTS] = {0}; size_t count = 0; int first = 0;
    uint64_t capture_limit = 256u * 1024u; int have_capture_limit = 0;
    for (; first < argc;) {
        if (!strcmp(argv[first], "--")) { first++; break; }
        if (!strcmp(argv[first], "--capture-bytes")) {
            if (have_capture_limit || first + 1 == argc || memory_unsigned(argv[first + 1], 256u * 1024u, &capture_limit) || !capture_limit)
                return semantic_error(call, "invalid_arguments", 2);
            have_capture_limit = 1; first += 2; continue;
        }
        if (strcmp(argv[first], "--expect")) {
            if (argv[first][0] == '-') return semantic_error(call, "invalid_arguments", 2);
            break;
        }
        if (count == CONTRACT_MAX_ASSERTS || first + 1 == argc) return semantic_error(call, "invalid_arguments", 2);
        const char* kind = argv[++first]; assert_t* assertion = &assertions[count];
        int value = 0;
        if (!strcmp(kind, "rc")) { assertion->kind = EXP_RC; value = 1; }
        else if (!strcmp(kind, "stdout-contains")) { assertion->kind = EXP_STDOUT_CONTAINS; value = 1; }
        else if (!strcmp(kind, "stdout-empty")) assertion->kind = EXP_STDOUT_EMPTY;
        else if (!strcmp(kind, "stdout-matches")) { assertion->kind = EXP_STDOUT_MATCHES; value = 1; }
        else if (!strcmp(kind, "stderr-contains")) { assertion->kind = EXP_STDERR_CONTAINS; value = 1; }
        else if (!strcmp(kind, "stderr-empty")) assertion->kind = EXP_STDERR_EMPTY;
        else return semantic_error(call, "invalid_expectation", 2);
        first++;
        if (value) {
            if (first == argc) return semantic_error(call, "invalid_arguments", 2);
            if (assertion->kind == EXP_RC) {
                uint64_t rc;
                if (memory_unsigned(argv[first], 255, &rc)) return semantic_error(call, "invalid_expected_status", 2);
                assertion->rc = (int)rc;
            } else {
                if (strlen(argv[first]) >= sizeof(assertion->str)) return semantic_error(call, "expectation_byte_limit", 75);
                if (jash_copy_cstr(assertion->str, sizeof(assertion->str), argv[first])) { return semantic_error(call, "expectation_byte_limit", 75); }
                if (assertion->kind == EXP_STDOUT_MATCHES) {
                    regex_t regex;
                    if (regcomp(&regex, assertion->str, REG_EXTENDED | REG_NOSUB))
                        return semantic_error(call, "invalid_regex", 2);
                    regfree(&regex);
                }
            }
            first++;
        }
        count++;
    }
    if (first == argc || !argv[first][0]) return semantic_error(call, "missing_command", 2);
    char* command[65]; int words = argc - first;
    for (int i = 0; i < words; i++) command[i] = argv[first + i];
    command[words] = NULL;
    analysis_capture_t prototype = {.execution = {.attempted = 1,.command_status = INT_MIN,
        .adapter_error = INT_MAX,.writer_error = INT_MAX,.restore_error = INT_MAX},
        .size = {(size_t)capture_limit,(size_t)capture_limit},.seen = {UINT64_MAX,UINT64_MAX},
        .truncated = {1,1},.error = INT_MAX};
    unsigned char* reserve = calloc((size_t)capture_limit, 1);
    if (!reserve) return semantic_error(call, "allocation_failed", 125);
    prototype.bytes[0] = prototype.bytes[1] = reserve;
    int possible[CONTRACT_MAX_ASSERTS]; for (size_t i = 0; i < count; i++) possible[i] = 0;
    jdv_value_t possible_report = contract_native_report(&call->values, words, command, assertions, count,
        &prototype, possible, 0, "execution_capture_or_predicate_observation_failed", INT_MAX);
    int status = semantic_preflight(call, &possible_report);
    free(reserve);
    if (status) return semantic_error(call, "receipt_admission_failed", status);
    if (semantic_admit(call)) return 125;
    call->values.arena.used = 0; call->values.error = 0;
    analysis_capture_t capture; int captured = analysis_capture_argv(command, (size_t)capture_limit, &capture);
    const char* error = captured ? capture.error == EOVERFLOW ? "output_capture_limit" : "execution_capture_failed" : NULL;
    status = captured ? capture.error == EOVERFLOW ? 75 :
        capture.execution.adapter_error == ENOENT ? 127 : 125 : 0;
    int passed[CONTRACT_MAX_ASSERTS], matched = 1;
    for (size_t i = 0; i < count; i++) passed[i] = -1;
    for (size_t i = 0; !error && i < count; i++) {
        if (assertions[i].kind == EXP_STDOUT_MATCHES && memchr(capture.bytes[0], 0, capture.size[0])) {
            error = "regex_binary_output_not_supported"; status = 1; break;
        }
        passed[i] = !contract_check(&assertions[i], capture.execution.command_status,
            (const char*)capture.bytes[0], (long)capture.size[0],
            (const char*)capture.bytes[1], (long)capture.size[1], contract_native_discard, NULL);
        matched = matched && passed[i];
    }
    if (!error) status = matched ? 0 : 2;
    jdv_value_t result = contract_native_report(&call->values, words, command, assertions, count,
                                               &capture, passed, matched, error, status);
    jdv_stream_end_t outcome; (void)jdv_stream_end_init(&outcome);
    outcome.state = error ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    outcome.completeness = error ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
    outcome.errors_seen = error ? 1 : 0; outcome.producer_status = status;
    if (error) outcome.reason = (jdv_slice_t){(const uint8_t*)error,strlen(error)};
    int result_status = semantic_finish_outcome(call, result, &outcome);
    analysis_capture_free(&capture); return result_status;
}
SEMANTIC_ARGV_WRAPPER(contract, "contract", "jash.contract.v1", contract_native_provider)
