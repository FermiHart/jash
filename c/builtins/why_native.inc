// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_output.h"
#include "memory_store.h"
#include <core/drift.h>

static int why_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)argv;
    if (argc) return semantic_error(call, "invalid_arguments", 2);
    if (!state) return semantic_error(call, "state_unavailable", 1);
    const char* command = state->last_status.cmd;
    int known = command[0] != 0;
    size_t capacity = sizeof(state->last_status.cmd) - 1;
    int may_be_truncated = strlen(command) == capacity;
    char cwd[PATH_MAX] = "", key[17] = "";
    int status = 0, saved = 0, present = 0; const char* error = NULL;
    uint64_t success_timestamp = 0;
    memory_store_t store = {-1, -1}; char* bytes = NULL; size_t size = 0;
    if (!getcwd(cwd, sizeof(cwd))) { saved = errno; error = "cwd_unavailable"; status = 1; }
    if (!status && known && !may_be_truncated) {
        jash_drift_command_key(command, cwd, key);
        if (memory_store_open(&store, NULL, 0) || memory_store_read(&store, "drift.tsv", 256u * 640u, &bytes, &size, &present)) {
            saved = errno; error = memory_store_error(saved); status = 1;
        } else if (present) {
            size_t offset = 0, rows = 0;
            if (memchr(bytes, 0, size)) { saved = EINVAL; error = "invalid_store"; status = 1; }
            while (!status && offset < size) {
                char* line = bytes + offset; char* newline = memchr(line, '\n', size - offset);
                if (!newline || (size_t)(newline - line) >= 640 || ++rows > 256) { saved = EINVAL; error = "invalid_store"; status = 1; break; }
                *newline = 0; offset = (size_t)(newline - bytes) + 1;
                char* timestamp = strchr(line, '\t');
                if (!timestamp || timestamp - line != 16) { saved = EINVAL; error = "invalid_store"; status = 1; break; }
                *timestamp++ = 0;
                char* end = strchr(timestamp, '\t'); uint64_t observed;
                if (!end) { saved = EINVAL; error = "invalid_store"; status = 1; break; }
                *end = 0;
                if (memory_unsigned(timestamp, INT64_MAX, &observed) || !observed) { saved = EINVAL; error = "invalid_store"; status = 1; break; }
                if (!strcmp(line, key) && observed > success_timestamp) success_timestamp = observed;
            }
        }
    }
    if (memory_store_close(&store) && !status) { saved = errno; error = "store_close_failed"; status = 1; }
    semantic_value_t* sem = &call->values;
    jdv_value_t command_value = semantic_text(sem, known ? command : NULL);
    jdv_value_t status_value = known ? jdv_value_i64(state->last_status.code) : jdv_value_nothing();
    jdv_value_t message_value = semantic_text(sem, known ? state->last_status.msg : NULL);
    jdv_value_t mutations_value = jdv_value_u64((uint64_t)jash_timeline_count());
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.why.v1")),
        semantic_field("known", jdv_value_bool(known)),
        semantic_field("command_recorded", command_value),
        semantic_field("status", status_value),
        semantic_field("message_recorded", message_value),
        semantic_field("command_capacity_bytes", jdv_value_u64(capacity)),
        semantic_field("command_may_be_truncated", jdv_value_bool(may_be_truncated)),
        semantic_field("lookup_cwd", semantic_path(sem, cwd[0] ? cwd : NULL)),
        semantic_field("lookup_scope", semantic_symbol(sem, "recorded_command_at_current_cwd")),
        semantic_field("success_fingerprint_timestamp", success_timestamp && !status ? jdv_value_u64(success_timestamp) : jdv_value_nothing()),
        semantic_field("timestamp_semantics", semantic_symbol(sem, "cached_success_fingerprint_not_latest_execution")),
        semantic_field("mutations_recorded", mutations_value),
        semantic_field("causation_established", jdv_value_bool(0)),
        /* Historical JSON keys remain aliases of this same observation. The
         * zero last_ok_ts sentinel means no observed cached timestamp; its
         * meaning is still bounded by timestamp_semantics and error above. */
        semantic_field("cmd", command_value),
        semantic_field("rc", status_value),
        semantic_field("msg", message_value),
        semantic_field("mutations_this_session", mutations_value),
        semantic_field("last_ok_ts", jdv_value_u64(success_timestamp && !status ? success_timestamp : 0)),
        semantic_field("reason", semantic_text(sem, known ? NULL : "no last command recorded")),
        semantic_field("verdict", semantic_symbol(sem, known && !state->last_status.code ? "ok" : NULL)),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("errno", saved ? jdv_value_i64(saved) : jdv_value_nothing())
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    free(bytes); return semantic_finish(call, result, status);
}

SEMANTIC_ARGV_WRAPPER(why, "why", "jash.why.v1", why_native_provider)
