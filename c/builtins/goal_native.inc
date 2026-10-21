// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
#include "memory_output.h"
#include "memory_store.h"
#include "memory_receipt.h"

static int goal_native_parse(char* bytes, size_t size, goal_checkpoint_t* checkpoint) {
    if (memchr(bytes, 0, size)) return -1;
    char* fields[6]; size_t offset = 0;
    for (size_t i = 0; i < 6; i++) {
        if (offset >= size) return -1;
        fields[i] = bytes + offset;
        char* newline = memchr(fields[i], '\n', size - offset);
        if (!newline) return -1;
        *newline = 0; offset = (size_t)(newline - bytes) + 1;
    }
    uint64_t identifier = 0, timestamp = 0;
    if (offset != size || memory_unsigned(fields[0], INT_MAX, &identifier) || !identifier ||
        memory_unsigned(fields[4], LONG_MAX, &timestamp) || !timestamp ||
        strlen(fields[1]) >= sizeof(checkpoint->kind) ||
        (strcmp(fields[1], "begin") && strcmp(fields[1], "step") && strcmp(fields[1], "verify")) ||
        strlen(fields[2]) >= sizeof(checkpoint->text) || strlen(fields[3]) >= sizeof(checkpoint->cwd) ||
        fields[3][0] != '/') return -1;
    char* end = NULL; errno = 0; long rc = strtol(fields[5], &end, 10);
    if (errno || !fields[5][0] || *end || rc < INT_MIN || rc > INT_MAX) return -1;
    memset(checkpoint, 0, sizeof(*checkpoint));
    checkpoint->id = (int)identifier; checkpoint->ts_sec = (long)timestamp; checkpoint->rc_hint = (int)rc;
    memcpy(checkpoint->kind, fields[1], strlen(fields[1]) + 1);
    memcpy(checkpoint->text, fields[2], strlen(fields[2]) + 1);
    memcpy(checkpoint->cwd, fields[3], strlen(fields[3]) + 1);
    return 0;
}

static jdv_value_t goal_native_value(semantic_value_t* sem, const goal_checkpoint_t* checkpoint) {
    if (!checkpoint->id) return jdv_value_nothing();
    jdv_record_input_t fields[] = {
        semantic_field("id", jdv_value_u64((uint64_t)checkpoint->id)),
        semantic_field("kind", semantic_symbol(sem, checkpoint->kind)),
        semantic_field("text", semantic_text(sem, checkpoint->text)),
        semantic_field("cwd", semantic_path(sem, checkpoint->cwd)),
        semantic_field("timestamp", jdv_value_i64(checkpoint->ts_sec)),
        semantic_field("rc_hint", jdv_value_i64(checkpoint->rc_hint)),
        semantic_field("verification", semantic_symbol(sem, "user_recorded_claim"))
    };
    return semantic_record(sem, fields, 7);
}

typedef struct { char* paths[GOAL_RECALL_MAX_FILES]; size_t count; } goal_native_matches_t;
static int goal_native_match(void* context, const char* path) {
    goal_native_matches_t* matches = context;
    if (matches->count == GOAL_RECALL_MAX_FILES) { errno = E2BIG; return -1; }
    char* copy = strdup(path);
    if (!copy) return -1;
    matches->paths[matches->count++] = copy; return 0;
}
static int goal_native_compare(const void* first, const void* second) {
    return strcmp(*(const char* const*)first, *(const char* const*)second);
}

static int goal_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    const char* operation = argc ? argv[0] : "status";
    int status_query = !strcmp(operation, "status"), recall = !strcmp(operation, "recall");
    int begin = !strcmp(operation, "begin"), step = !strcmp(operation, "step");
    int verify = !strcmp(operation, "verify"), closing = !strcmp(operation, "close");
    int mutating = begin || step || verify || closing;
    uint64_t offset = 0, limit = 64; const char* text = NULL;
    if ((!status_query && !recall && !mutating) || (status_query && argc > 1) ||
        (begin && argc != 2) || ((step || verify) && argc > 2) ||
        (closing && (argc > 2 || (argc == 2 && strcmp(argv[1], "--summary")))) ||
        (recall && argc < 2)) return semantic_error(call, "invalid_arguments", 2);
    if (begin || step || verify) text = argc == 2 ? argv[1] : operation;
    if (text && (!*text || strlen(text) >= GOAL_TEXT_MAX || strchr(text, '\n') || strchr(text, '\r')))
        return semantic_error(call, "invalid_arguments", 2);
    regex_t pattern; int have_pattern = 0;
    if (recall) {
        if (!*argv[1] || strlen(argv[1]) >= GOAL_TEXT_MAX) return semantic_error(call, "invalid_arguments", 2);
        int have_offset = 0, have_limit = 0;
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--offset") && !have_offset && i + 1 < argc) {
                if (memory_unsigned(argv[++i], GOAL_RECALL_MAX_FILES, &offset)) return semantic_error(call, "invalid_arguments", 2);
                have_offset = 1;
            } else if (!strcmp(argv[i], "--limit") && !have_limit && i + 1 < argc) {
                if (memory_unsigned(argv[++i], 128, &limit) || !limit) return semantic_error(call, "invalid_arguments", 2);
                have_limit = 1;
            } else return semantic_error(call, "invalid_arguments", 2);
        }
        if (regcomp(&pattern, argv[1], REG_EXTENDED | REG_ICASE | REG_NOSUB))
            return semantic_error(call, "invalid_pattern", 2);
        have_pattern = 1;
    }
    char cwd[256] = "";
    if (mutating && (!getcwd(cwd, sizeof(cwd)) || strchr(cwd, '\n') || strchr(cwd, '\r')))
        return semantic_error(call, "cwd_unavailable_or_unrepresentable", 1);
    /* Two bounded checkpoint records plus all receipt/error fields. */
    if (mutating) { int admitted = memory_receipt_admit(call, 16384u); if (admitted) return admitted; }
    memory_store_t store = {-1, -1}; memory_write_t log_write = {0}, current_write = {0};
    int status = 0, saved = 0, exists = 0, reserved = 0, environment_applied = 0;
    const char* error = NULL; char* bytes = NULL; size_t size = 0;
    goal_checkpoint_t current = {0}, checkpoint = {0}; goal_native_matches_t matches = {0};
    if (memory_store_open(&store, "goals", mutating)) {
        saved = errno; error = memory_store_error(saved); status = 1;
    } else if (!recall) {
        if (memory_store_read(&store, "current", 1024, &bytes, &size, &exists)) {
            saved = errno; error = memory_store_error(saved); status = 1;
        } else if (exists && goal_native_parse(bytes, size, &current)) {
            saved = EINVAL; error = "invalid_store"; status = 1;
        }
    }
    if (!status && recall && store.directory >= 0) {
        const char* home = getenv("HOME"); char path[PATH_MAX];
        int n = snprintf(path, sizeof(path), "%s/.jash/goals", home);
        if (n < 0 || (size_t)n >= sizeof(path)) { saved = ENAMETOOLONG; error = "store_path_limit"; status = 1; }
        else {
            goal_search_t search = {.pattern = &pattern, .match_limit = UINT_MAX,
                .on_match = goal_native_match, .match_context = &matches};
            goal_search_dir(&search, store.directory, path, 0);
            if (search.failed) { saved = errno ? errno : EIO; error = "search_failed_or_limit"; status = 1; }
            qsort(matches.paths, matches.count, sizeof(matches.paths[0]), goal_native_compare);
        }
    }
    if (!status && mutating) {
        if (!begin && !current.id) { error = "no_active_goal"; status = 1; }
        else {
            checkpoint = current;
            if (begin) {
                checkpoint.id = next_id();
                if (checkpoint.id < 0) { checkpoint.id = 0; saved = errno ? errno : EIO; error = "id_reservation_failed"; status = 1; }
                else reserved = 1;
            }
            if (!status) {
                if (jash_copy_cstr(checkpoint.kind, sizeof(checkpoint.kind), operation)) { saved = errno; error = "checkpoint_field_limit"; status = 1; } if (jash_copy_cstr(checkpoint.text, sizeof(checkpoint.text), closing ? "goal closed" : text)) { saved = errno; error = "checkpoint_field_limit"; status = 1; }
                if (jash_copy_cstr(checkpoint.cwd, sizeof(checkpoint.cwd), cwd)) { saved = errno; error = "checkpoint_field_limit"; status = 1; } checkpoint.ts_sec = now_sec(); checkpoint.rc_hint = closing ? 0 : -1;
                if (checkpoint.ts_sec <= 0) { saved = EIO; error = "clock_unavailable"; status = 1; }
            }
            char name[64]; char* log = NULL; size_t log_size = 0; int log_exists = 0;
            if (!status) {
                snprintf(name, sizeof(name), "goal_%05d.log", checkpoint.id);
                if (memory_store_read(&store, name, MEMORY_STORE_MAX_BYTES, &log, &log_size, &log_exists)) {
                    saved = errno; error = memory_store_error(saved); status = 1;
                } else if (!log_exists || memchr(log, 0, log_size)) { saved = EINVAL; error = "invalid_store"; status = 1; }
            }
            if (!status) {
                char entry[1024];
                int n = snprintf(entry, sizeof(entry), "[%s] t=%ld cwd=%s rc=%d\n%s\n\n",
                                 checkpoint.kind, checkpoint.ts_sec, checkpoint.cwd, checkpoint.rc_hint, checkpoint.text);
                if (n < 0 || (size_t)n >= sizeof(entry) || (size_t)n > MEMORY_STORE_MAX_BYTES - log_size) {
                    saved = E2BIG; error = "store_limit"; status = 1;
                } else {
                    char* combined = realloc(log, log_size + (size_t)n);
                    if (!combined) { saved = ENOMEM; error = "allocation_failed"; status = 125; }
                    else {
                        log = combined; memcpy(log + log_size, entry, (size_t)n);
                        log_write = memory_store_publish(&store, name, log, log_size + (size_t)n);
                        if (log_write.error) { saved = log_write.error; error = memory_store_error(saved); status = 1; }
                    }
                }
            }
            free(log);
            if (!status) {
                if (closing) current_write = memory_store_remove(&store, "current");
                else {
                    char encoded[1024];
                    int n = snprintf(encoded, sizeof(encoded), "%d\n%s\n%s\n%s\n%ld\n%d\n", checkpoint.id,
                                     checkpoint.kind, checkpoint.text, checkpoint.cwd, checkpoint.ts_sec, checkpoint.rc_hint);
                    if (n < 0 || (size_t)n >= sizeof(encoded)) { current_write.error = E2BIG; }
                    else current_write = memory_store_publish(&store, "current", encoded, (size_t)n);
                }
                if (current_write.applied) {
                    if (closing) memset(&current, 0, sizeof(current));
                    else current = checkpoint;
                    if (begin || closing) {
                        if (jash_env_set(state, "JASH_GOAL", closing ? "" : checkpoint.text)) {
                            saved = errno ? errno : ENOMEM; error = "environment_update_failed"; status = 1;
                        } else environment_applied = 1;
                    }
                }
                if (current_write.error) { saved = current_write.error; error = memory_store_error(saved); status = 1; }
            }
        }
    }
    if (memory_store_close(&store) && !status) { saved = errno; error = "store_close_failed"; status = 1; }
    if (have_pattern) regfree(&pattern);
    semantic_value_t* sem = &call->values;
    jdv_value_t found[128]; size_t emitted = 0;
    for (size_t i = (size_t)offset; i < matches.count && emitted < limit; i++) found[emitted++] = semantic_path(sem, matches.paths[i]);
    int truncated = recall && offset + emitted < matches.count;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.goal.v1")),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("current", goal_native_value(sem, &current)),
        semantic_field("checkpoint", goal_native_value(sem, &checkpoint)),
        semantic_field("matches", semantic_list(sem, found, emitted)),
        semantic_field("total", jdv_value_u64(matches.count)),
        semantic_field("offset", jdv_value_u64(offset)),
        semantic_field("next_offset", truncated ? jdv_value_u64(offset + emitted) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(truncated)),
        semantic_field("scan_complete", recall ? jdv_value_bool(!status) : jdv_value_nothing()),
        semantic_field("id_reserved", jdv_value_bool(reserved)),
        semantic_field("applied", jdv_value_bool(reserved || log_write.applied || current_write.applied)),
        semantic_field("durable", jdv_value_bool(mutating && !status && log_write.durable && current_write.durable)),
        semantic_field("log_applied", jdv_value_bool(log_write.applied)),
        semantic_field("log_durable", jdv_value_bool(log_write.durable)),
        semantic_field("current_applied", jdv_value_bool(current_write.applied)),
        semantic_field("current_durable", jdv_value_bool(current_write.durable)),
        semantic_field("environment_applied", jdv_value_bool(environment_applied)),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("errno", saved ? jdv_value_i64(saved) : jdv_value_nothing())
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    for (size_t i = 0; i < matches.count; i++) free(matches.paths[i]);
    free(bytes); return semantic_finish(call, result, status);
}

SEMANTIC_ARGV_WRAPPER(goal, "goal", "jash.goal.v1", goal_native_provider)
