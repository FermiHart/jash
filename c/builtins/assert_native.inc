// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "analysis_output.h"
#include "memory_store.h"
#include "semantic_events.h"

typedef struct { as_t* request; int status, forget, timeout, adding; } as_native_options_t;
typedef struct {
    int directory, lock, present, read_complete, lock_mode;
    struct stat metadata;
    char path[AS_PATH_MAX];
    size_t records;
} as_native_source_t;

static int as_native_parse(int argc, char* const* argv, as_native_options_t* options) {
    memset(options,0,sizeof(*options)); options->request = calloc(1,sizeof(*options->request));
    if (!options->request) return -1;
    as_t* request = options->request; unsigned seen = 0; int literal = 0;
    for (int i = 0; i < argc; i++) {
        const char* argument = argv[i];
        if (!literal && !strcmp(argument,"--")) { literal = 1; continue; }
        unsigned bit = !literal && !strcmp(argument,"--status") ? 1 :
            !literal && !strcmp(argument,"--forget") ? 2 :
            !literal && (!strcmp(argument,"--check") || !strncmp(argument,"--check=",8)) ? 4 :
            !literal && !strcmp(argument,"--timeout-ms") ? 8 : 0;
        if (bit && (seen & bit)) return -1;
        if (bit) seen |= bit;
        if (bit == 1) options->status = 1;
        else if (bit == 2) options->forget = 1;
        else if (bit == 4) {
            const char* value = !strcmp(argument,"--check") ? ++i < argc ? argv[i] : NULL : argument + 8;
            if (!value || !*value || as_copy(request->check,sizeof(request->check),value)) return -1;
        } else if (bit == 8) {
            long value = 0;
            if (++i == argc || as_number(argv[i],AS_TIMEOUT_MAX,&value) || !value) return -1;
            options->timeout = (int)value;
        } else if (!literal && !strcmp(argument,"--dep")) {
            if (++i == argc || !argv[i][0] || request->ndeps == AS_DEPS_MAX ||
                as_copy(request->deps[request->ndeps++].path,AS_PATH_MAX,argv[i])) return -1;
        } else {
            if ((!literal && argument[0] == '-') || !*argument || (!literal && (seen & 4))) return -1;
            size_t used = strlen(request->text), length = strlen(argument);
            if (used + !!used + length >= sizeof(request->text)) return -1;
            if (used) request->text[used++] = ' ';
            memcpy(request->text + used,argument,length + 1u);
        }
    }
    options->adding = request->text[0] && !options->forget;
    if ((options->adding && (!(seen & 4) || options->status)) || (!options->adding && (seen & 4)) ||
        (request->ndeps && !options->adding) || (options->forget && (!request->text[0] || options->status || options->timeout))) return -1;
    return 0;
}

const char* jash_assert_effect_argv(int argc, char* const* argv) {
    if (argc < 1 || argc > 64 || !argv || !argv[0] ||
        (strcmp(argv[0],"--json") && strcmp(argv[0],"--data") && strcmp(argv[0],"--data-v2"))) return "exec";
    size_t bytes = 0;
    for (int i = 0; i < argc; i++) {
        if (!argv[i] || strlen(argv[i]) > 16384u - bytes) return "unknown";
        bytes += strlen(argv[i]);
    }
    as_native_options_t options; int invalid = as_native_parse(argc - 1,argv + 1,&options);
    const char* effect = invalid ? "unknown" : options.status ? "read" : options.forget ? "mutate" : "exec";
    free(options.request); return effect;
}

static int as_native_close(as_native_source_t* source) {
    int error = 0;
    if (source->lock >= 0) { if (close(source->lock)) error = errno; source->lock = -1; }
    if (source->directory >= 0) { if (close(source->directory) && !error) error = errno; source->directory = -1; }
    return error;
}
static int as_native_named(as_native_source_t* source, struct stat* metadata, int* present) {
    *present = 0; memset(metadata,0,sizeof(*metadata));
    if (source->directory < 0) return 0;
    if (!fstatat(source->directory,"assertions.tsv",metadata,AT_SYMLINK_NOFOLLOW)) { *present = 1; return 0; }
    return errno == ENOENT ? 0 : -1;
}
static int as_native_lock(as_native_source_t* source, int create, int status) {
    if (source->directory < 0) return 0;
    source->lock = openat(source->directory,"assertions.lock",
        (status ? O_RDONLY : O_RDWR) | (create ? O_CREAT : 0) | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK,0600);
    if (source->lock < 0) return !create && errno == ENOENT ? 0 : -1;
    if (memory_store_trusted(source->lock,0) || flock(source->lock,(status ? LOCK_SH : LOCK_EX) | LOCK_NB)) return -1;
    source->lock_mode = status ? 1 : 2;
    return 0;
}
static int as_native_directory_current(const as_native_source_t* source) {
    if (source->directory < 0) { errno = ENOENT; return -1; }
    const char* home = getenv("HOME");
    int parent = home ? open(home,O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
    if (parent < 0) return -1;
    struct stat pinned, named; int error = 0;
    if (fstat(source->directory,&pinned) || fstatat(parent,".jash",&named,AT_SYMLINK_NOFOLLOW)) error = errno;
    else if (!S_ISDIR(named.st_mode) || pinned.st_dev != named.st_dev || pinned.st_ino != named.st_ino) error = ESTALE;
    if (close(parent) && !error) error = errno;
    if (error) { errno = error; return -1; }
    return 0;
}
static int as_native_load(as_native_source_t* source, as_t* rows, int status) {
    *source = (as_native_source_t){.directory = -1,.lock = -1};
    const char* home = getenv("HOME");
    int length = snprintf(source->path,sizeof(source->path),"%s/.jash/assertions.tsv",home ? home : "");
    if (length < 0 || (size_t)length >= sizeof(source->path)) { errno = ENAMETOOLONG; return -1; }
    memory_store_t store = {-1,-1};
    if (memory_store_open(&store,NULL,0)) return -1;
    source->directory = store.directory;
    if (as_native_lock(source,0,status) || as_native_named(source,&source->metadata,&source->present)) return -1;
    int count = source->directory < 0 ? 0 : as_load(source->directory,"assertions.tsv",rows);
    if (count < 0) return -1;
    source->records = (size_t)count; source->read_complete = 1; return count;
}
static int as_native_prepare_write(as_native_source_t* source) {
    if (source->directory < 0) {
        const char* home = getenv("HOME");
        int directory = home ? open(home,O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC) : -1;
        if (directory < 0) return -1;
        int error = 0;
        if (memory_store_trusted(directory,1)) error = errno;
        else { source->directory = memory_store_child(directory,".jash",1); if (source->directory < 0) error = errno; }
        if (close(directory) && !error) error = errno;
        if (error) { errno = error; return -1; }
    }
    if (source->lock < 0 && as_native_lock(source,1,0)) return -1;
    if (as_native_directory_current(source)) return -1;
    struct stat now; int present = 0;
    if (as_native_named(source,&now,&present)) return -1;
    if (present != source->present || (present && !memory_store_same(&source->metadata,&now))) { errno = ESTALE; return -1; }
    return 0;
}

static jdv_value_t as_native_execution(semantic_value_t* sem, const as_verify_observation_t* observed) {
    jdv_record_input_t fields[] = {
        semantic_field("requested",jdv_value_bool(observed->requested)),
        semantic_field("started",jdv_value_bool(observed->started)),
        semantic_field("status_valid",jdv_value_bool(observed->status_valid)),
        semantic_field("command_status",observed->status_valid ? jdv_value_i64(observed->exit_code) : jdv_value_nothing()),
        semantic_field("output_limit_hit",jdv_value_bool(observed->output_limit)),
        semantic_field("indeterminate",jdv_value_bool(observed->indeterminate)),
        semantic_field("quiescent",jdv_value_bool(observed->quiescent)),
        semantic_field("descendants_quiescent",jdv_value_bool(observed->descendants_quiescent)),
        semantic_field("quiescence_forced",jdv_value_bool(observed->quiescence_forced)),
        semantic_field("descendants_forced",jdv_value_bool(observed->descendants_forced)),
        semantic_field("cleanup_errno",observed->cleanup_error_number ? jdv_value_i64(observed->cleanup_error_number) : jdv_value_nothing()),
        semantic_field("output_scope",semantic_symbol(sem,"bounded_supervisor_spools_discarded"))
    };
    return semantic_record(sem,fields,12);
}
static jdv_value_t as_native_row(semantic_value_t* sem, const as_t* row, int assessed) {
    jdv_value_t dependencies[AS_DEPS_MAX];
    for (int i = 0; i < row->ndeps; i++) {
        jdv_record_input_t fields[] = {
            semantic_field("path",semantic_path(sem,row->deps[i].path)),
            semantic_field("blake3",semantic_text(sem,row->deps[i].hash))
        };
        dependencies[i] = semantic_record(sem,fields,2);
    }
    jdv_record_input_t fields[] = {
        semantic_field("claim",semantic_text(sem,row->text)),
        semantic_field("check",semantic_text(sem,row->check)),
        semantic_field("state",semantic_symbol(sem,as_states[row->state])),
        semantic_field("reason",semantic_symbol(sem,row->reason)),
        semantic_field("created",jdv_value_i64(row->created)),
        semantic_field("last_true",jdv_value_i64(row->last_true)),
        semantic_field("ever_true",jdv_value_bool(row->ever)),
        semantic_field("assessed_in_call",jdv_value_bool(assessed)),
        semantic_field("timeout_ms",jdv_value_u64((uint64_t)row->timeout_ms)),
        semantic_field("cwd",semantic_path(sem,row->cwd)),
        semantic_field("scope",semantic_path(sem,row->scope)),
        semantic_field("dependencies",semantic_list(sem,dependencies,(size_t)row->ndeps)),
        semantic_field("dependency_scope",semantic_symbol(sem,"declared_regular_file_content_pre_post_samples_not_filesystem_snapshot"))
    };
    return semantic_record(sem,fields,13);
}
static jdv_value_t as_native_receipt(semantic_value_t* sem, const as_t* row, size_t index,
        const as_verify_observation_t* observed, int64_t elapsed, int clock_error) {
    jdv_record_input_t fields[] = {
        semantic_field("schema",semantic_symbol(sem,"jash.assert.v1")),
        semantic_field("kind",semantic_symbol(sem,"verification")),
        semantic_field("index",jdv_value_u64(index)),
        semantic_field("claim",semantic_text(sem,row->text)),
        semantic_field("state",semantic_symbol(sem,as_states[row->state])),
        semantic_field("reason",semantic_symbol(sem,row->reason)),
        semantic_field("execution",as_native_execution(sem,observed)),
        semantic_field("elapsed_ms",clock_error ? jdv_value_nothing() : jdv_value_u64((uint64_t)elapsed)),
        semantic_field("clock_error",jdv_value_bool(clock_error)),
        semantic_field("persisted",jdv_value_bool(0))
    };
    return semantic_record(sem,fields,10);
}
static jdv_value_t as_native_summary(semantic_value_t* sem, const as_native_options_t* options,
        const as_native_source_t* source, const as_t* rows, size_t count, size_t first,
        size_t removed, size_t assessed, int complete, int failed, int fallen,
        as_save_observation_t saved, const char* error, int number, int effects) {
    jdv_value_t visible[AS_MAX]; size_t shown = 0, legacy = 0;
    for (size_t i = 0; i < count; i++) {
        legacy += !rows[i].scope[0];
        if (options->forget || i < first || strcmp(rows[i].scope,options->request->scope)) continue;
        visible[shown] = as_native_row(sem,&rows[i],shown < assessed); shown++;
    }
    jdv_record_input_t input[] = {
        semantic_field("path",semantic_path(sem,source->path)),
        semantic_field("present",jdv_value_bool(source->present)),
        semantic_field("read_complete",jdv_value_bool(source->read_complete)),
        semantic_field("records",jdv_value_u64(source->records)),
        semantic_field("bytes",source->present ? jdv_value_filesize((uint64_t)source->metadata.st_size) : jdv_value_filesize(0)),
        semantic_field("consistency",semantic_symbol(sem,"stable_file_metadata_existing_native_lock_when_present")),
        semantic_field("lock_mode",semantic_symbol(sem,source->lock_mode == 1 ? "shared" : source->lock_mode == 2 ? "exclusive" : NULL))
    };
    jdv_record_input_t persistence[] = {
        semantic_field("requested",jdv_value_bool(!options->status)),
        semantic_field("applied",jdv_value_bool(saved.applied)),
        semantic_field("durable",jdv_value_bool(saved.durable)),
        semantic_field("errno",saved.error ? jdv_value_i64(saved.error) : jdv_value_nothing()),
        semantic_field("scope",semantic_symbol(sem,"pinned_store_directory"))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema",semantic_symbol(sem,"jash.assert.v1")),
        semantic_field("kind",semantic_symbol(sem,"summary")),
        semantic_field("operation",semantic_symbol(sem,options->status ? "status" : options->forget ? "forget" : options->adding ? "add" : "verify")),
        semantic_field("scope",semantic_path(sem,options->request->scope)),
        semantic_field("source",semantic_record(sem,input,7)),
        semantic_field("assertions",semantic_list(sem,visible,shown)),
        semantic_field("assessed",jdv_value_u64(assessed)),
        semantic_field("fallen",jdv_value_u64((uint64_t)fallen)),
        semantic_field("failed",jdv_value_u64((uint64_t)failed)),
        semantic_field("forgotten",jdv_value_u64(removed)),
        semantic_field("legacy_unscoped",jdv_value_u64(legacy)),
        semantic_field("status_only",jdv_value_bool(options->status)),
        semantic_field("complete",jdv_value_bool(complete)),
        semantic_field("predicate_status",complete ? jdv_value_i64(failed ? 1 : 0) : jdv_value_nothing()),
        semantic_field("persistence",semantic_record(sem,persistence,5)),
        semantic_field("effects_may_have_applied",jdv_value_bool(effects)),
        semantic_field("error",semantic_symbol(sem,error)),
        semantic_field("errno",number ? jdv_value_i64(number) : jdv_value_nothing())
    };
    return semantic_record(sem,fields,sizeof(fields)/sizeof(fields[0]));
}
static int as_native_observation_error(const as_t* row) {
    if (!strcmp(row->reason,"no_dependencies") || !strcmp(row->reason,"dependency_changed") ||
        !strcmp(row->reason,"verifier_failed") || !strcmp(row->reason,"verified")) return 0;
    return row->state == AS_UNKNOWN || !strcmp(row->reason,"dependency_unavailable");
}

static int as_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    as_native_options_t options;
    if (as_native_parse(argc,argv,&options)) { free(options.request); return semantic_error(call,"invalid_arguments",2); }
    call->values.limits.max_record_fields = 8192;
    as_t* rows = calloc(AS_MAX,sizeof(*rows));
    as_native_source_t source = {.directory = -1,.lock = -1};
    size_t count = 0, first = 0, removed = 0, assessed = 0; int failed = 0, fallen = 0, status = 0, number = 0, effects = 0;
    const char* error = NULL; as_save_observation_t saved = {0};
    if (!rows) { status = 125; error = "allocation_failed"; }
    else if (as_scope(options.request->cwd,options.request->scope)) { status = 1; number = errno; error = "scope_unavailable"; }
    else {
        int loaded = as_native_load(&source,rows,options.status);
        if (loaded < 0) { number = errno; status = number == E2BIG ? 75 : 1; error = "assertion_store_unavailable"; }
        else count = (size_t)loaded;
    }
    if (!status && options.adding) {
        if (count == AS_MAX) { status = 75; error = "assertion_capacity"; }
        else {
            options.request->created = (long)time(NULL);
            if (options.request->created < 0) { status = 1; error = "clock_failed"; }
            else {
                options.request->timeout_ms = options.timeout ? options.timeout : AS_TIMEOUT_MS;
                rows[count] = *options.request; first = count++;
            }
        }
    }
    if (!status && options.forget) {
        for (size_t i = 0; i < count;) {
            if (!strcmp(rows[i].scope,options.request->scope) &&
                !strncmp(rows[i].text,options.request->text,strlen(options.request->text))) {
                memmove(&rows[i],&rows[i + 1],(count - i - 1u) * sizeof(*rows)); count--; removed++;
            } else i++;
        }
    }
    if (!status && !options.status) {
        /* Reserve every possible receipt and complete final row before the
         * first store creation/verifier. Worst-case enum/reason/hash values
         * dominate later engine observations without fabricating any result. */
        jdv_value_t possible[AS_MAX + 1]; size_t possible_count = 0;
        as_verify_observation_t worst = {.requested=1,.started=1,.status_valid=1,.exit_code=INT_MIN,
            .output_limit=1,.indeterminate=1,.quiescent=1,.descendants_quiescent=1,.quiescence_forced=1,.descendants_forced=1,.cleanup_error_number=INT_MAX};
        as_t* reserve_rows = calloc(count ? count : 1,sizeof(*reserve_rows));
        if (!reserve_rows) { status = 125; error = "allocation_failed"; }
        else {
            memcpy(reserve_rows,rows,count * sizeof(*rows));
            for (size_t i = first; i < count && !options.forget; i++) {
                as_t* row = &reserve_rows[i]; if (strcmp(row->scope,options.request->scope)) continue;
                row->state = AS_INVALIDATED; memset(row->reason,'x',sizeof(row->reason)-1); row->reason[sizeof(row->reason)-1] = 0;
                row->last_true = LONG_MAX;
                for (int d = 0; d < row->ndeps; d++) { memset(row->deps[d].hash,'f',64); row->deps[d].hash[64] = 0; }
                possible[possible_count++] = as_native_receipt(&call->values,row,i,&worst,INT64_MAX,0);
            }
            possible[possible_count++] = as_native_summary(&call->values,&options,&source,reserve_rows,count,first,removed,AS_MAX,
                0,AS_MAX,AS_MAX,(as_save_observation_t){1,1,INT_MAX},"assertion_source_observation_or_publication_failed",INT_MAX,1);
            status = semantic_events_preflight(call,possible,possible_count);
            if (status) error = "receipt_admission_failed";
            free(reserve_rows);
        }
        call->values.arena.used = 0; call->values.error = 0;
        if (!status) {
            if (semantic_admit_rows(call,AS_MAX + 1u)) { status = 125; goto cleanup; }
            effects = 1;
            if (as_native_prepare_write(&source)) { status = 1; number = errno; error = "assertion_store_changed_or_locked"; }
        }
    }
    if (!status && !options.forget) for (size_t i = first; i < count; i++) {
        as_t* row = &rows[i]; if (strcmp(row->scope,options.request->scope)) continue;
        as_verify_observation_t observed; int64_t start = as_now();
        as_verify_observed(row,options.status,options.timeout ? options.timeout : row->timeout_ms,&observed);
        int64_t end = as_now(); int clock_error = start < 0 || end < start;
        assessed++; failed += row->state != AS_TRUE; fallen += row->state == AS_FALLEN;
        if (!options.status) {
            if (semantic_events_emit(call,as_native_receipt(&call->values,row,i,&observed,end - start,clock_error))) {
                status = 125; goto cleanup;
            }
            call->values.arena.used = 0; call->values.error = 0;
        }
        if (clock_error || as_native_observation_error(row)) {
            if (!error) error = clock_error ? "clock_failed" : "assertion_verification_incomplete";
            status = clock_error ? 125 : !strcmp(row->reason,"timeout") || !strcmp(row->reason,"output_limit") ? 75 : 1;
            /* The failed observation itself is retained and may be persisted;
             * stop before another arbitrary verifier executes. */
            break;
        }
    }
    if (call->admitted && !options.status && source.directory >= 0 && source.lock >= 0 &&
        (!error || !strcmp(error,"assertion_verification_incomplete") || !strcmp(error,"clock_failed"))) {
        if (as_native_directory_current(&source)) saved.error = errno;
        else {
            saved = as_save_observed(source.directory,"assertions.tsv",rows,(int)count);
            if (as_native_directory_current(&source) && !saved.error) {
                status = 1; number = errno; error = "assertion_store_namespace_changed";
            }
        }
        if (saved.error) { status = 1; number = saved.error; error = "assertion_persistence_failed"; }
    }
    {
        int close_error = as_native_close(&source);
        if (close_error) { status = 1; number = close_error; error = "assertion_store_close_failed"; }
        if (status && !call->admitted && (status == 75 || status == 125) && error && !strcmp(error,"receipt_admission_failed"))
            status = semantic_error(call,error,status);
        else {
            jdv_value_t summary = as_native_summary(&call->values,&options,&source,rows,count,first,removed,assessed,
                !error,failed,fallen,saved,error,number,effects);
            jdv_stream_end_t outcome; (void)jdv_stream_end_init(&outcome);
            outcome.state = error ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
            outcome.completeness = error ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
            outcome.producer_status = error ? status : failed ? 1 : 0; outcome.errors_seen = error ? 1 : 0;
            if (error) outcome.reason = (jdv_slice_t){(const uint8_t*)error,strlen(error)};
            status = semantic_finish_outcome(call,summary,&outcome);
        }
    }
cleanup:
    (void)as_native_close(&source); free(rows); free(options.request); return status;
}

int jash_b_assert_structured_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int result = semantic_dispatch_argv(state,writer,context,"assert","jash.assert.v1",argc,argv,as_native_provider);
    return result >= 0 ? result : jash_b_assert_argv(state,writer,context,argc,argv);
}
