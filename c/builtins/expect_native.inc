// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "analysis_output.h"
#include "analysis_capture.h"
#include "memory_chain.h"
#include "query_snapshot.h"
#include "semantic_events.h"
#include <core/argv.h>
#include <core/query_cancel.h>

#define EXPECT_NATIVE_FILE_BYTES (16u * 1024u * 1024u)
#define EXPECT_NATIVE_CAPTURE_BYTES (256u * 1024u)
typedef enum {
    EN_FILE, EN_DIR, EN_NO, EN_EXEC, EN_EMPTY, EN_NONEMPTY, EN_CONTAINS,
    EN_NEWER, EN_OLDER, EN_MATCH, EN_RC, EN_CMD, EN_LINK, EN_BROKEN, EN_SNAPSHOT
} expect_native_kind_t;
typedef struct {
    expect_native_kind_t kind;
    const char *verb, *a, *b, *check;
    char* command;
    int negated, update, expected_rc;
    uint64_t until;
    size_t capture_limit, argument_bytes;
} expect_native_options_t;
typedef struct {
    const char* path;
    struct stat metadata;
    int present, observed, error, executable;
    size_t bytes_read;
    int content_complete;
} expect_native_file_t;
typedef struct {
    int complete, holds, status, number;
    const char* error;
    expect_native_file_t files[2]; size_t file_count;
    memory_chain_t chain; int loaded_chain, previous_present, previous_status;
    const char* previous_source;
    const char* previous_command;
    const memory_chain_entry_t* previous_entry;
    analysis_capture_t capture; int captured;
    int snapshot_present, snapshot_read_complete, snapshot_store_attempted;
    size_t snapshot_before_bytes;
    memory_write_t snapshot_write;
    const char* snapshot_action;
} expect_native_observation_t;

static int expect_native_parse(int argc, char* const* argv, expect_native_options_t* options) {
    *options = (expect_native_options_t){.capture_limit = EXPECT_NATIVE_CAPTURE_BYTES};
    int index = 0; unsigned seen = 0;
    for (int i = 0; i < argc; i++) options->argument_bytes += strlen(argv[i]);
    while (index < argc && argv[index][0] == '-') {
        if (!strcmp(argv[index], "--")) { index++; break; }
        unsigned bit = !strcmp(argv[index], "--until") ? 1 : !strcmp(argv[index], "--capture-bytes") ? 2 : 0;
        uint64_t number = 0;
        if (!bit || (seen & bit) || ++index == argc || memory_unsigned(argv[index++],
                bit == 1 ? 60 : EXPECT_NATIVE_CAPTURE_BYTES, &number) || !number) return -1;
        seen |= bit;
        if (bit == 1) options->until = number;
        else options->capture_limit = (size_t)number;
    }
    if (index < argc && (!strcmp(argv[index], "not") || !strcmp(argv[index], "!"))) {
        options->negated = 1; index++;
    }
    if (index == argc) return -1;
    options->verb = argv[index++];
    static const struct { const char* name; expect_native_kind_t kind; int operands; } verbs[] = {
        {"file",EN_FILE,1},{"exists",EN_FILE,1},{"dir",EN_DIR,1},{"no",EN_NO,1},
        {"absent",EN_NO,1},{"exec",EN_EXEC,1},{"empty",EN_EMPTY,1},{"nonempty",EN_NONEMPTY,1},
        {"contains",EN_CONTAINS,2},{"newer",EN_NEWER,2},{"older",EN_OLDER,2},
        {"match",EN_MATCH,2},{"same",EN_MATCH,2},{"rc",EN_RC,1},{"cmd",EN_CMD,3},
        {"link",EN_LINK,1},{"broken",EN_BROKEN,1},{"snapshot",EN_SNAPSHOT,-1}
    };
    size_t selected = 0;
    while (selected < sizeof(verbs)/sizeof(verbs[0]) && strcmp(verbs[selected].name, options->verb)) selected++;
    if (selected == sizeof(verbs)/sizeof(verbs[0])) return -1;
    options->kind = verbs[selected].kind;
    if (options->kind != EN_SNAPSHOT && argc - index != verbs[selected].operands) return -1;
    if (index == argc) return -1;
    options->a = argv[index++];
    if (!options->a[0]) return -1;
    if (options->kind == EN_SNAPSHOT) {
        size_t length = strlen(options->a);
        if (length > 128 || options->a[0] == '.' || strchr(options->a, '/')) return -1;
        if (index < argc && !strcmp(argv[index], "--update")) { options->update = 1; index++; }
        if (index < argc && !strcmp(argv[index], "--")) index++;
        if (index == argc) return -1; /* There is no stored last-output source to bless. */
        char* words[64];
        for (int i = index; i < argc; i++) words[i - index] = argv[i];
        options->command = semantic_legacy_join(argc - index, words);
        if (!options->command || !options->command[0]) return -1;
    } else if (options->kind == EN_CMD) {
        options->command = strdup(options->a); options->check = argv[index++]; options->b = argv[index];
        if (!options->command || (strcmp(options->check, "rc") && strcmp(options->check, "output"))) return -1;
    } else if (index < argc) options->b = argv[index];
    if (options->kind == EN_RC || (options->kind == EN_CMD && !strcmp(options->check, "rc"))) {
        uint64_t status = 0;
        if (memory_unsigned(options->kind == EN_RC ? options->a : options->b, 255, &status)) return -1;
        options->expected_rc = (int)status;
    }
    if ((seen & 2) && options->kind != EN_CMD && options->kind != EN_SNAPSHOT) return -1;
    return 0;
}

const char* jash_expect_effect_argv(int argc, char* const* argv) {
    if (argc < 1 || argc > 64 || !argv || !argv[0] ||
        (strcmp(argv[0],"--json") && strcmp(argv[0],"--data") && strcmp(argv[0],"--data-v2"))) return "exec";
    size_t bytes = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i] || strlen(argv[i]) > 16384u - bytes) return "unknown";
        bytes += strlen(argv[i]);
    }
    expect_native_options_t options;
    int invalid = expect_native_parse(argc - 1,argv + 1,&options);
    const char* effect = invalid ? "unknown" : options.command ? "exec" : "read";
    free(options.command); return effect;
}

static void expect_native_failure(expect_native_observation_t* result, const char* error, int number, int status) {
    if (result->error) return;
    result->complete = 0; result->error = error; result->number = number; result->status = status;
}
static void expect_native_free(expect_native_observation_t* result) {
    analysis_capture_free(&result->capture);
    if (result->loaded_chain) memory_chain_free(&result->chain);
}
static int expect_native_stat(expect_native_file_t* file, const char* path, int link) {
    *file = (expect_native_file_t){.path = path};
    if (!(link ? lstat(path, &file->metadata) : stat(path, &file->metadata))) {
        file->observed = file->present = 1; return 0;
    }
    file->error = errno;
    if (errno == ENOENT || errno == ENOTDIR) { file->observed = 1; file->error = 0; return 0; }
    return -1;
}
static int expect_native_content(jash_state_t* state, expect_native_file_t* file,
                                  jash_query_file_page_t* page, expect_native_observation_t* result) {
    int status = jash_query_file_page(state, file->path, 0, EXPECT_NATIVE_FILE_BYTES, page);
    if (status) { expect_native_failure(result, page->error, page->error_number, status); return -1; }
    if ((uint64_t)page->metadata.st_size > EXPECT_NATIVE_FILE_BYTES) {
        expect_native_failure(result, "file_byte_limit", E2BIG, 75); return -1;
    }
    file->metadata = page->metadata; file->present = file->observed = 1;
    file->bytes_read = page->used; file->content_complete = 1; return 0;
}
static void expect_native_query(jash_state_t* state, const expect_native_options_t* options,
                                expect_native_observation_t* result) {
    if (options->kind == EN_RC) {
        if (state->last_status.cmd[0]) {
            result->previous_present = 1; result->previous_status = state->last_status.code;
            result->previous_command = state->last_status.cmd; result->previous_source = "current_process";
        } else {
            result->loaded_chain = 1; result->previous_source = "retained_chain_unverified";
            if (memory_chain_load(&result->chain, 0, 0))
                expect_native_failure(result, memory_store_error(result->chain.error), result->chain.error,
                                      result->chain.error == E2BIG ? 75 : 1);
            else if (result->chain.count) {
                result->previous_entry = &result->chain.entries[result->chain.count - 1];
                if (result->previous_entry->status < INT_MIN || result->previous_entry->status > INT_MAX)
                    expect_native_failure(result, "previous_status_out_of_range", EOVERFLOW, 1);
                else {
                    result->previous_present = 1; result->previous_status = (int)result->previous_entry->status;
                    result->previous_command = result->previous_entry->command;
                }
            }
        }
        if (!result->error && !result->previous_present) expect_native_failure(result, "no_previous_status_source", 0, 2);
        result->holds = result->previous_present && result->previous_status == options->expected_rc;
        return;
    }
    int is_link = options->kind == EN_LINK || options->kind == EN_BROKEN;
    result->file_count = options->kind == EN_NEWER || options->kind == EN_OLDER || options->kind == EN_MATCH || is_link ? 2 : 1;
    if (expect_native_stat(&result->files[0], options->a, is_link))
        expect_native_failure(result, "metadata_observation_failed", result->files[0].error, 1);
    if (result->file_count == 2 && expect_native_stat(&result->files[1], is_link ? options->a : options->b, 0))
        expect_native_failure(result, "metadata_observation_failed", result->files[1].error, 1);
    if (result->error) return;
    expect_native_file_t *first = &result->files[0], *second = &result->files[1];
    if (options->kind == EN_NO) { result->holds = !first->present; return; }
    if (!first->present || (result->file_count == 2 && !is_link && !second->present)) return;
    switch (options->kind) {
    case EN_FILE: result->holds = 1; break;
    case EN_DIR: result->holds = S_ISDIR(first->metadata.st_mode); break;
    case EN_EXEC:
        if (!access(options->a, X_OK)) first->executable = 1;
        else if (errno != EACCES && errno != ENOENT && errno != ENOTDIR)
            expect_native_failure(result, "access_observation_failed", errno, 1);
        result->holds = first->executable && S_ISREG(first->metadata.st_mode); break;
    case EN_EMPTY: result->holds = S_ISREG(first->metadata.st_mode) && first->metadata.st_size == 0; break;
    case EN_NONEMPTY: result->holds = S_ISREG(first->metadata.st_mode) && first->metadata.st_size > 0; break;
    case EN_NEWER: result->holds = first->metadata.st_mtime > second->metadata.st_mtime; break;
    case EN_OLDER: result->holds = first->metadata.st_mtime < second->metadata.st_mtime; break;
    case EN_LINK: result->holds = S_ISLNK(first->metadata.st_mode) && second->present; break;
    case EN_BROKEN: result->holds = S_ISLNK(first->metadata.st_mode) && !second->present; break;
    case EN_CONTAINS:
    case EN_MATCH: {
        if (!S_ISREG(first->metadata.st_mode) || (options->kind == EN_MATCH && !S_ISREG(second->metadata.st_mode))) break;
        jash_query_file_page_t pages[2] = {0};
        if (!expect_native_content(state, first, &pages[0], result)) {
            if (options->kind == EN_CONTAINS) result->holds = options->b[0] && memmem(pages[0].bytes, pages[0].used, options->b, strlen(options->b)) != NULL;
            else if (!expect_native_content(state, second, &pages[1], result))
                result->holds = pages[0].used == pages[1].used && !memcmp(pages[0].bytes, pages[1].bytes, pages[0].used);
        }
        free(pages[0].bytes); free(pages[1].bytes); break;
    }
    default: break;
    }
}

static uint64_t expect_native_clock(int* error) {
    struct timespec now;
    if (clock_gettime(CLOCK_MONOTONIC, &now)) { *error = errno; return 0; }
    return (uint64_t)now.tv_sec * UINT64_C(1000000000) + (uint64_t)now.tv_nsec;
}
typedef struct { jash_state_t* state; uint64_t start; int status, error; } expect_native_lock_t;
static int expect_native_lock_poll(void* context) {
    expect_native_lock_t* lock = context;
    lock->status = jash_query_cancel_poll(lock->state);
    if (lock->status) return 1;
    uint64_t now = expect_native_clock(&lock->error);
    if (lock->error) { lock->status = 125; return 1; }
    if (now - lock->start >= UINT64_C(5000000000)) { lock->status = 75; lock->error = ETIMEDOUT; return 1; }
    return 0;
}
static void expect_native_snapshot(jash_state_t* state, const expect_native_options_t* options,
                                   expect_native_observation_t* result) {
    memory_store_t store = {-1,-1}; char* prior = NULL; size_t length = 0; int present = 0;
    expect_native_lock_t lock = {.state = state}; lock.start = expect_native_clock(&lock.error);
    result->snapshot_store_attempted = 1;
    if (lock.error || memory_store_open_cooperative(&store, "snapshots", 1, expect_native_lock_poll, &lock))
        expect_native_failure(result, lock.status == 75 ? "snapshot_lock_timeout" : "snapshot_store_open_failed",
                              lock.error ? lock.error : errno, lock.status ? lock.status : 1);
    else if (memory_store_read(&store, options->a, EXPECT_NATIVE_CAPTURE_BYTES, &prior, &length, &present))
        expect_native_failure(result, "snapshot_read_failed", errno, errno == E2BIG ? 75 : 1);
    if (!result->error) {
        result->snapshot_read_complete = 1;
        result->snapshot_present = present; result->snapshot_before_bytes = length;
        if (options->update || !present) {
            result->snapshot_action = options->update ? "update" : "record";
            result->snapshot_write = memory_store_publish(&store, options->a, result->capture.bytes[0], result->capture.size[0]);
            if (result->snapshot_write.error) expect_native_failure(result, "snapshot_publish_failed", result->snapshot_write.error, 1);
            else result->holds = 1;
        } else {
            result->snapshot_action = "compare";
            result->holds = length == result->capture.size[0] && !memcmp(prior, result->capture.bytes[0], length);
        }
    }
    if (memory_store_close(&store)) expect_native_failure(result, "snapshot_close_failed", errno, 1);
    free(prior);
}

static jdv_value_t expect_native_file_value(semantic_value_t* sem, const expect_native_file_t* file) {
    const struct stat* metadata = &file->metadata;
    const char* type = !file->present ? NULL : S_ISREG(metadata->st_mode) ? "regular" : S_ISDIR(metadata->st_mode) ? "directory" :
        S_ISLNK(metadata->st_mode) ? "symlink" : S_ISFIFO(metadata->st_mode) ? "fifo" : "other";
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem,file->path)),
        semantic_field("present", file->observed ? jdv_value_bool(file->present) : jdv_value_nothing()),
        semantic_field("metadata_observed", jdv_value_bool(file->observed)),
        semantic_field("type", semantic_symbol(sem,type)),
        semantic_field("size_bytes", file->present && metadata->st_size >= 0 ? jdv_value_filesize((uint64_t)metadata->st_size) : jdv_value_nothing()),
        semantic_field("mtime_seconds", file->present ? jdv_value_i64(metadata->st_mtime) : jdv_value_nothing()),
        semantic_field("content_read_complete", jdv_value_bool(file->content_complete)),
        semantic_field("bytes_read", jdv_value_filesize(file->bytes_read)),
        semantic_field("access_executable", jdv_value_bool(file->executable)),
        semantic_field("errno", file->error ? jdv_value_i64(file->error) : jdv_value_nothing())
    };
    return semantic_record(sem,fields,10);
}
static jdv_value_t expect_native_attempt(semantic_value_t* sem, const expect_native_observation_t* result,
                                        uint64_t attempt, const char* stage) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem,"jash.expect.v1")),
        semantic_field("kind", semantic_symbol(sem,"attempt")),
        semantic_field("attempt", jdv_value_u64(attempt)),
        semantic_field("stage", semantic_symbol(sem,stage)),
        semantic_field("complete", jdv_value_bool(result->complete)),
        semantic_field("holds_before_negation", result->complete && !strcmp(stage,"predicate_evaluated") ? jdv_value_bool(result->holds) : jdv_value_nothing()),
        semantic_field("command_attempted", jdv_value_bool(result->captured && result->capture.execution.attempted)),
        semantic_field("command_status", result->captured && result->capture.execution.attempted ? jdv_value_i64(result->capture.execution.command_status) : jdv_value_nothing()),
        semantic_field("snapshot_applied", jdv_value_bool(result->snapshot_write.applied)),
        semantic_field("snapshot_durable", jdv_value_bool(result->snapshot_write.durable)),
        semantic_field("error", semantic_symbol(sem,result->error)),
        semantic_field("errno", result->number ? jdv_value_i64(result->number) : jdv_value_nothing())
    };
    return semantic_record(sem,fields,12);
}
static jdv_value_t expect_native_result(semantic_value_t* sem, const expect_native_options_t* options,
        const expect_native_observation_t* result, uint64_t attempts, uint64_t elapsed, int timed_out) {
    jdv_value_t files[2]; for (size_t i = 0; i < result->file_count; i++) files[i] = expect_native_file_value(sem,&result->files[i]);
    jdv_record_input_t previous[] = {
        semantic_field("source", semantic_symbol(sem,result->previous_source)),
        semantic_field("present", jdv_value_bool(result->previous_present)),
        semantic_field("status", result->previous_present ? jdv_value_i64(result->previous_status) : jdv_value_nothing()),
        semantic_field("command_recorded", semantic_text(sem,result->previous_command)),
        semantic_field("sequence", result->previous_entry ? jdv_value_u64(result->previous_entry->sequence) : jdv_value_nothing()),
        semantic_field("chain_source", result->loaded_chain ? memory_chain_source(sem,&result->chain) : jdv_value_nothing())
    };
    jdv_record_input_t snapshot[] = {
        semantic_field("name", semantic_text(sem,options->a)),
        semantic_field("action", semantic_symbol(sem,result->snapshot_action)),
        semantic_field("read_complete", jdv_value_bool(result->snapshot_read_complete)),
        semantic_field("present_before", result->snapshot_read_complete ? jdv_value_bool(result->snapshot_present) : jdv_value_nothing()),
        semantic_field("bytes_before", result->snapshot_read_complete ? jdv_value_filesize(result->snapshot_before_bytes) : jdv_value_nothing()),
        semantic_field("bytes_current", jdv_value_filesize(result->capture.size[0])),
        semantic_field("applied", jdv_value_bool(result->snapshot_write.applied)),
        semantic_field("durable", jdv_value_bool(result->snapshot_write.durable)),
        semantic_field("store_may_have_been_created", jdv_value_bool(result->snapshot_store_attempted)),
        semantic_field("errno", result->snapshot_write.error ? jdv_value_i64(result->snapshot_write.error) : jdv_value_nothing())
    };
    jdv_record_input_t wait[] = {
        semantic_field("seconds", jdv_value_u64(options->until)),
        semantic_field("attempts", jdv_value_u64(attempts)),
        semantic_field("elapsed", jdv_value_duration((int64_t)elapsed)),
        semantic_field("timed_out", jdv_value_bool(timed_out)),
        semantic_field("deadline_scope", semantic_symbol(sem,"between_evaluations_no_command_timeout"))
    };
    int holds = options->negated ? !result->holds : result->holds;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem,"jash.expect.v1")),
        semantic_field("kind", semantic_symbol(sem,"result")),
        semantic_field("predicate", semantic_symbol(sem,options->verb)),
        semantic_field("operand", semantic_text(sem,options->a)),
        semantic_field("second_operand", semantic_text(sem,options->b)),
        semantic_field("command", semantic_text(sem,options->command)),
        semantic_field("command_check", semantic_symbol(sem,options->check)),
        semantic_field("negated", jdv_value_bool(options->negated)),
        semantic_field("complete", jdv_value_bool(result->complete)),
        semantic_field("ok", result->complete ? jdv_value_bool(holds) : jdv_value_nothing()),
        semantic_field("predicate_status", result->complete ? jdv_value_i64(holds ? 0 : 1) : jdv_value_nothing()),
        semantic_field("files", semantic_list(sem,files,result->file_count)),
        semantic_field("previous_status", options->kind == EN_RC ? semantic_record(sem,previous,6) : jdv_value_nothing()),
        semantic_field("execution", result->captured ? analysis_capture_value(sem,&result->capture) : jdv_value_nothing()),
        semantic_field("execution_scope", options->command ? semantic_symbol(sem,"subshell") : jdv_value_nothing()),
        semantic_field("output_scope", options->command ? semantic_symbol(sem,"stdout_stderr_merged") : jdv_value_nothing()),
        semantic_field("snapshot", options->kind == EN_SNAPSHOT ? semantic_record(sem,snapshot,10) : jdv_value_nothing()),
        semantic_field("wait", semantic_record(sem,wait,5)),
        semantic_field("effects_may_have_applied", jdv_value_bool(result->capture.execution.attempted || result->snapshot_store_attempted)),
        semantic_field("error", semantic_symbol(sem,result->error)),
        semantic_field("errno", result->number ? jdv_value_i64(result->number) : jdv_value_nothing())
    };
    return semantic_record(sem,fields,sizeof(fields)/sizeof(fields[0]));
}

static int expect_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    expect_native_options_t options;
    if (expect_native_parse(argc,argv,&options)) { free(options.command); return semantic_error(call,"invalid_arguments",2); }
    int needs_admission = options.command != NULL || options.until != 0;
    uint64_t maximum = options.until ? options.until * 10u + 1u : 1;
    /* Compact receipts are <1024 bytes. Final capture is base64 (less than
     * twice the retained bytes), args/recorded chain use at most six-byte JSON
     * escapes. This reserves bounded outcomes, never claims emitted bytes. */
    uint64_t reserve = 16384u + (maximum * (options.kind == EN_SNAPSHOT ? 2u : 1u) + 1u) * 1024u +
        6u * (options.argument_bytes + (options.kind == EN_RC ? 65536u : 0u)) +
        (options.command ? 2u * options.capture_limit : 0u);
    int status = 0;
    if (needs_admission) {
        long budget = jash_budget_limit();
        if (reserve > SEMANTIC_OUTPUT_BYTES) status = 125;
        else if (call->mode == 1 && budget > 0 && (uint64_t)budget < reserve) status = 75;
        if (status) { free(options.command); return semantic_error(call,"receipt_budget_insufficient",status); }
        if (semantic_admit_rows(call, maximum * (options.kind == EN_SNAPSHOT ? 2u : 1u) + 1u)) {
            free(options.command); return 125;
        }
    }
    expect_native_observation_t result = {.complete = 1};
    uint64_t attempts = 0, elapsed = 0; int clock_error = 0, timed_out = 0, sink_failed = 0;
    uint64_t start = expect_native_clock(&clock_error);
    if (clock_error) expect_native_failure(&result,"monotonic_clock_failed",clock_error,125);
    while (!result.error && attempts < maximum) {
        int cancelled = jash_query_cancel_poll(state);
        if (cancelled) { call->cancellation_status = cancelled; expect_native_failure(&result,"cancelled",ECANCELED,cancelled); break; }
        attempts++;
        if (options.command) {
            size_t length = strlen(options.command);
            char* expression = malloc(length + 6u);
            if (!expression) expect_native_failure(&result,"allocation_failed",ENOMEM,125);
            else {
                snprintf(expression,length + 6u,"( %s )",options.command);
                result.captured = 1;
                if (analysis_capture_merged(state,expression,options.capture_limit,&result.capture))
                    expect_native_failure(&result,result.capture.error == EOVERFLOW ? "output_capture_limit" : "command_capture_failed",
                                          result.capture.error,result.capture.error == EOVERFLOW ? 75 : 125);
                free(expression);
                if (!result.error && options.kind == EN_CMD) result.holds = !strcmp(options.check,"rc") ?
                    result.capture.execution.command_status == options.expected_rc :
                    !options.b[0] || memmem(result.capture.bytes[0],result.capture.size[0],options.b,strlen(options.b)) != NULL;
            }
            if (options.kind == EN_SNAPSHOT && !result.error) {
                if (semantic_events_emit(call,expect_native_attempt(&call->values,&result,attempts,"capture_before_snapshot"))) {
                    sink_failed = 1; break;
                }
                call->values.arena.used = 0; call->values.error = 0;
                expect_native_snapshot(state,&options,&result);
            }
        } else expect_native_query(state,&options,&result);
        uint64_t now = expect_native_clock(&clock_error);
        if (clock_error) expect_native_failure(&result,"monotonic_clock_failed",clock_error,125);
        else elapsed = now - start;
        int holds = options.negated ? !result.holds : result.holds;
        if (options.until) {
            if (semantic_events_emit(call,expect_native_attempt(&call->values,&result,attempts,"predicate_evaluated"))) { sink_failed = 1; break; }
            call->values.arena.used = 0; call->values.error = 0;
        }
        if (result.error || holds || !options.until) break;
        if (elapsed >= options.until * UINT64_C(1000000000) || attempts == maximum) { timed_out = 1; break; }
        expect_native_free(&result); result = (expect_native_observation_t){.complete = 1};
        struct timespec pause = {0,100000000};
        while (nanosleep(&pause,&pause)) {
            if (errno != EINTR) { expect_native_failure(&result,"wait_failed",errno,125); break; }
            if ((cancelled = jash_query_cancel_poll(state))) {
                call->cancellation_status = cancelled; expect_native_failure(&result,"cancelled",ECANCELED,cancelled); break;
            }
        }
    }
    if (result.status >= 128 && jash_query_cancel_poll(state)) call->cancellation_status = result.status;
    if (sink_failed) status = 125;
    else {
        jdv_value_t value = expect_native_result(&call->values,&options,&result,attempts,elapsed,timed_out);
        jdv_stream_end_t end; (void)jdv_stream_end_init(&end);
        end.state = call->cancellation_status ? JDV_STREAM_CANCELLED : result.error ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
        end.completeness = result.error ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
        end.producer_status = result.error ? result.status : (options.negated ? !result.holds : result.holds) ? 0 : 1;
        end.errors_seen = result.error ? 1 : 0;
        if (result.error) end.reason = (jdv_slice_t){(const uint8_t*)result.error,strlen(result.error)};
        status = semantic_finish_outcome(call,value,&end);
    }
    expect_native_free(&result); free(options.command); return status;
}

/* Native argv preserves quoted commands and operands before the legacy text
 * renderer. Only the legacy compatibility path keeps its old snapshot engine
 * and suffix --json shape; new selectors use checked semantic observations. */
static int expect_legacy_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    char* operands[64]; int count = 0, json = 0, negate = 0, seconds = 0, index = 0;
    if (argc > 64) return exp_usage();
    if (argc >= 2 && !strcmp(argv[0],"--until")) { seconds = atoi(argv[1]); if (seconds <= 0) seconds = 5; index = 2; }
    for (; index < argc; index++) {
        if (!strcmp(argv[index],"--json")) json = 1;
        else operands[count++] = argv[index];
    }
    int offset = count && (!strcmp(operands[0],"not") || !strcmp(operands[0],"!"));
    negate = offset;
    if (count <= offset) return exp_usage();
    const char* verb = operands[offset]; int expected = 2;
    if (!strcmp(verb,"cmd")) expected = 4;
    else if (!strcmp(verb,"contains") || !strcmp(verb,"newer") || !strcmp(verb,"older") ||
             !strcmp(verb,"match") || !strcmp(verb,"same")) expected = 3;
    if (strcmp(verb,"snapshot") && count - offset != expected) return exp_usage();
    char desc[1200], actual[512]; int attempts = 0, result = -1; time_t deadline = time(NULL) + seconds;
    do {
        desc[0] = actual[0] = 0; attempts++;
        result = eval_pred_w(count - offset,operands + offset,state,writer,context,desc,sizeof(desc),actual,sizeof(actual));
        if (result < 0) break;
        int holds = negate ? !result : result;
        if (holds || !seconds || time(NULL) >= deadline) break;
        struct timespec pause = {0,100000000}; (void)nanosleep(&pause,NULL);
    } while (1);
    if (result == -2) { jash_writef(jash_stderr_writer,NULL,"expect: FAILED — %s\n",desc); return 2; }
    if (result < 0) return exp_usage();
    int holds = negate ? !result : result;
    if (json) {
        char ed[4096], ea[2048]; jash_json_esc(desc,ed,sizeof(ed)); jash_json_esc(actual,ea,sizeof(ea));
        jash_writef(writer,context,"{\"ok\":%s,\"negated\":%s,\"predicate\":\"%s\",\"actual\":\"%s\"}\n",
                    holds ? "true" : "false",negate ? "true" : "false",ed,holds ? "" : ea);
    } else if (holds) {
        if (seconds) jash_writef(writer,context,"expect: ok — %s (after %d attempts)\n",desc,attempts);
        else jash_writef(writer,context,"expect: ok — %s%s\n",negate ? "NOT " : "",desc);
    } else if (seconds) jash_writef(jash_stderr_writer,NULL,"expect: FAILED — timed out after %ds (%d attempts): %s\n",seconds,attempts,desc);
    else {
        jash_writef(jash_stderr_writer,NULL,"expect: FAILED — %s%s (actual: %s)\n",negate ? "NOT " : "",desc,actual[0] ? actual : "?");
        if (!negate && count - offset >= 2) {
            unsigned long long sequence = 0; char command[192];
            if (chain_last_touch(operands[offset + 1],&sequence,command,sizeof(command)))
                jash_writef(jash_stderr_writer,NULL,"       · last seen seq #%llu via '%.80s'\n",sequence,command);
        }
    }
    return holds ? 0 : 1;
}

int jash_b_expect_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int status = semantic_dispatch_argv(state,writer,context,"expect","jash.expect.v1",argc,argv,expect_native_provider);
    return status >= 0 ? status : expect_legacy_argv(state,writer,context,argc,argv);
}
