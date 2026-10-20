// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_builtin.h"
#include "json_git_observe.h"

/* The legacy display fields remain bounded previews. Their source is the
 * checked Git plumbing observation, also exposed without lossy rendering. */
static const jdv_value_t* sn_field(const jdv_value_t* value, const char* name) {
    return jash_discovery_field(value, name);
}
static jdv_value_t sn_copy(const jdv_value_t* value) {
    return value ? *value : jdv_value_nothing();
}
static int sn_true(const jdv_value_t* value) {
    return value && value->kind == JDV_BOOL && value->as.boolean;
}
typedef struct { char bytes[4096]; size_t size; int truncated; } sn_preview_t;
static void sn_append(sn_preview_t* preview, const void* bytes, size_t length) {
    size_t room = sizeof(preview->bytes) - preview->size;
    if (length > room) { length = room; preview->truncated = 1; }
    if (length) memcpy(preview->bytes + preview->size, bytes, length);
    preview->size += length;
}
static void sn_text(sn_preview_t* preview, const jdv_value_t* value) {
    if (!value) return;
    if (value->kind == JDV_STRING || value->kind == JDV_PATH || value->kind == JDV_SYMBOL || value->kind == JDV_BYTES)
        sn_append(preview, value->as.slice.data, value->as.slice.len);
}
static jdv_value_t sn_preview_value(semantic_value_t* values, const sn_preview_t* preview) {
    return semantic_text_n(values, preview->bytes, preview->size);
}

static int state_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    int no_git = argc == 1 && !strcmp(argv[0], "--no-git");
    if (argc && !no_git) return semantic_error(call, "invalid_arguments", 2);
    semantic_value_t* values = &call->values;
    char* cwd = getcwd(NULL, 0);
    if (!cwd) return semantic_error(call, "cwd_unavailable", 1);
    jdv_value_t directory = semantic_path(values, cwd); free(cwd);
    jash_env_sync_frame(state);
    uint32_t env_count = jash_env_count(state);
    jdv_record_input_t* environment = calloc(env_count ? env_count : 1u, sizeof(*environment));
    if (!environment) return semantic_error(call, "allocation_failed", 125);
    size_t selected = 0;
    for (uint32_t i = 0; i < env_count; i++) {
        const char *key, *value;
        if (!jash_env_at(state, i, &key, &value)) continue;
        if (strncmp(key, "JASH_", 5) && strcmp(key, "PATH") && strcmp(key, "HOME") &&
            strcmp(key, "USER") && strcmp(key, "SHELL") && strcmp(key, "PWD")) continue;
        if (!jash_json_utf8_valid_n(key, strlen(key))) { values->error = 1; break; }
        environment[selected++] = semantic_field(key, semantic_text(values, value));
    }
    jdv_value_t env = semantic_record(values, environment, selected); free(environment);
    int total = jash_timeline_count(), kept = total < STATE_MAX_TIMELINE ? total : STATE_MAX_TIMELINE;
    jdv_value_t entries[STATE_MAX_TIMELINE]; int count = 0;
    for (int i = 0; i < kept; i++) {
        int kind; const char *a, *b;
        if (!jash_timeline_peek(i, &kind, &a, &b)) break;
        jdv_record_input_t fields[] = {
            semantic_field("kind", jdv_value_i64(kind)),
            semantic_field("a", semantic_path(values, a ? a : "")),
            semantic_field("b", semantic_path(values, b ? b : ""))
        };
        entries[count++] = semantic_record(values, fields, 3);
    }
    jdv_record_input_t last_fields[] = {
        semantic_field("rc", jdv_value_i64(state->last_status.code)),
        semantic_field("cmd", semantic_text(values, state->last_status.cmd))
    };
    jdv_record_input_t jobs_fields[] = {
        semantic_field("count", state->jobs ? jdv_value_u64(jash_job_count(state->jobs)) : jdv_value_nothing()),
        semantic_field("available", jdv_value_bool(state->jobs != NULL))
    };
    const char* intent = jash_env_get(state, "JASH_INTENT");
    jdv_value_t intent_value = semantic_text(values, intent ? intent : "");
    jdv_value_t observation = jdv_value_nothing(), diff = jdv_value_nothing();
    jash_git_observation_t git_status = {0}, diff_status = {0};
    if (!no_git) {
        jdv_record_input_t candidate_fields[] = {
            semantic_field("cwd", directory), semantic_field("env", env),
            semantic_field("intent", intent_value), semantic_field("pending", semantic_symbol(values, "git-observations"))
        };
        jdv_value_t candidate = semantic_record(values, candidate_fields, 4);
        int admission = semantic_preflight(call, &candidate);
        if (admission) return semantic_error(call, "output_admission_failed", admission);
        if (semantic_admit(call)) return 125;
        (void)jash_git_observe(values, 1, &observation, &git_status);
        const jdv_value_t* status = sn_field(&observation, "status");
        if (sn_true(sn_field(status, "available")))
            (void)jash_git_diff_stat_observe(values, &diff, &diff_status);
    }
    const jdv_value_t* status = sn_field(&observation, "status");
    const jdv_value_t* status_payload = sn_field(status, "payload");
    const jdv_value_t* git = sn_field(status_payload, "git");
    const jdv_value_t* files = sn_field(status_payload, "files");
    const jdv_value_t* latest = sn_field(&observation, "latest_commit");
    const jdv_value_t* commits = sn_field(sn_field(latest, "payload"), "commits");
    const jdv_value_t* diff_files = sn_field(sn_field(&diff, "payload"), "files");
    int status_available = sn_true(sn_field(status, "available"));
    int complete = no_git || (!git_status.command_status && !git_status.adapter_status &&
                             !diff_status.command_status && !diff_status.adapter_status);
    sn_preview_t status_preview = {0}, diff_preview = {0}, recent_preview = {0};
    if (files && files->kind == JDV_LIST) {
        size_t shown = files->as.list.len < 20 ? files->as.list.len : 20;
        status_preview.truncated = shown != files->as.list.len;
        for (size_t i = 0; i < shown; i++) {
            const jdv_value_t* file = &files->as.list.items[i];
            sn_text(&status_preview, sn_field(file, "index_status")); sn_append(&status_preview, " ", 1);
            sn_text(&status_preview, sn_field(file, "path")); sn_append(&status_preview, "\n", 1);
        }
    }
    if (diff_files && diff_files->kind == JDV_LIST) {
        size_t shown = diff_files->as.list.len < 15 ? diff_files->as.list.len : 15;
        diff_preview.truncated = shown != diff_files->as.list.len;
        for (size_t i = 0; i < shown; i++) {
            const jdv_value_t* file = &diff_files->as.list.items[i];
            sn_text(&diff_preview, sn_field(file, "path")); sn_append(&diff_preview, " | ", 3);
            const jdv_value_t *added = sn_field(file, "insertions"), *removed = sn_field(file, "deletions");
            if (added && removed && added->kind == JDV_U64 && removed->kind == JDV_U64) {
                char text[96]; int size = snprintf(text, sizeof text, "+%llu -%llu\n",
                    (unsigned long long)added->as.u64, (unsigned long long)removed->as.u64);
                if (size > 0 && (size_t)size < sizeof text) sn_append(&diff_preview, text, (size_t)size);
            } else sn_append(&diff_preview, "binary\n", 7);
        }
    }
    if (commits && commits->kind == JDV_LIST && commits->as.list.len) {
        const jdv_value_t* commit = &commits->as.list.items[0];
        sn_text(&recent_preview, sn_field(commit, "hash")); sn_append(&recent_preview, " ", 1);
        sn_text(&recent_preview, sn_field(commit, "message"));
    }
    jdv_record_input_t git_fields[] = {
        semantic_field("present", status_available ? jdv_value_bool(1) : jdv_value_nothing()),
        semantic_field("branch", sn_copy(sn_field(git, "branch"))),
        semantic_field("dirty", sn_copy(sn_field(git, "dirty"))),
        semantic_field("status_short", sn_preview_value(values, &status_preview)),
        semantic_field("diff_stat", sn_preview_value(values, &diff_preview)),
        semantic_field("recent_commits", sn_preview_value(values, &recent_preview)),
        semantic_field("preview", jdv_value_bool(1)), semantic_field("requested", jdv_value_bool(!no_git)),
        semantic_field("complete", jdv_value_bool(complete)),
        semantic_field("status_preview_truncated", jdv_value_bool(status_preview.truncated)),
        semantic_field("diff_preview_truncated", jdv_value_bool(diff_preview.truncated)),
        semantic_field("recent_preview_truncated", jdv_value_bool(recent_preview.truncated)),
        semantic_field("latest_commit_limit", jdv_value_u64(1)),
        semantic_field("observation", observation), semantic_field("diff_observation", diff)
    };
    jdv_record_input_t fields[] = {
        semantic_field("cwd", directory), semantic_field("intent", intent_value),
        semantic_field("git", semantic_record(values, git_fields, 15)),
        semantic_field("last_status", semantic_record(values, last_fields, 2)),
        semantic_field("timeline", semantic_list(values, entries, (size_t)count)),
        semantic_field("timeline_total", jdv_value_u64((uint64_t)total)),
        semantic_field("timeline_truncated", jdv_value_bool(count < total)),
        semantic_field("env", env), semantic_field("jobs", semantic_record(values, jobs_fields, 2)),
        semantic_field("complete", jdv_value_bool(complete && count == total)),
        semantic_field("consistency", semantic_symbol(values, "independent-shell-and-git-observations"))
    };
    jdv_record_input_t document = semantic_field("state", semantic_record(values, fields, 11));
    jdv_value_t result = semantic_record(values, &document, 1);
    int preflight = semantic_preflight(call, &result);
    if (preflight) return semantic_error(call, "output_limit", preflight);
    jdv_stream_end_t end; (void)jdv_stream_end_init(&end);
    end.state = complete && count == total ? JDV_STREAM_COMPLETE : JDV_STREAM_INCOMPLETE;
    end.completeness = end.state == JDV_STREAM_COMPLETE ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL;
    end.errors_seen = complete ? 0 : 1;
    if (!complete) end.reason = JDV_LITERAL("git_observation_unavailable");
    else if (count < total) { end.truncated = 1; end.reason = JDV_LITERAL("timeline_preview"); }
    return semantic_finish_outcome(call, result, &end);
}
SEMANTIC_ARGV_WRAPPER(state, "state", "jash.state.v1", state_native_provider)
