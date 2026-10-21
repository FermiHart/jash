// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <osif/osif_stat.h>
// Native checkpoint/rewind adapters use the checked store and inverse engine.
static int cp_native_list(jash_writer_t writer, void* context, int mode) {
    if (mode == JASH_DISCOVERY_JSON) return checkpoints_json(writer, context);
    if (mode == JASH_DISCOVERY_TEXT) return list_checkpoints(writer, context);
    cp_t checkpoints[CP_MAX]; char path[600]; int invalid = 0, truncated = 0, failed = 0, count = 0;
    if (!cp_path(path, sizeof path)) failed = 1;
    else {
        count = cp_snapshot(path, checkpoints, CP_MAX, &invalid, &truncated, NULL, 0);
        if (count < 0) { failed = 1; count = 0; }
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "checkpoint", mode, "allocation_failed", 125);
    jdv_value_t values[CP_MAX]; int used = 0, depth = jash_timeline_depth();
    for (int i = count - 1; i >= 0; i--) {
        jdv_record_input_t fields[] = {
            {JDV_LITERAL("name"), jash_discovery_string(&rows, checkpoints[i].name)},
            {JDV_LITERAL("depth"), jdv_value_i64(checkpoints[i].depth)},
            {JDV_LITERAL("mutations_since"), jdv_value_i64(depth > checkpoints[i].depth ? depth - checkpoints[i].depth : 0)},
            {JDV_LITERAL("ts"), jdv_value_i64(checkpoints[i].ts)},
            {JDV_LITERAL("cwd"), jash_discovery_path(&rows, checkpoints[i].cwd)}
        };
        if (jdv_value_record(&rows.arena, fields, sizeof fields / sizeof fields[0], &values[used])) { rows.error = 1; break; }
        used++;
    }
    jdv_value_t list = jdv_value_nothing();
    if (jdv_value_list(&rows.arena, values, (size_t)used, &list)) rows.error = 1;
    int status = failed || invalid || truncated ? 1 : 0;
    jdv_record_input_t report[] = {
        {JDV_LITERAL("schema"), jash_discovery_string(&rows, "jash.checkpoints.v1")},
        {JDV_LITERAL("record_fields"), jash_discovery_string(&rows, "legacy_bounded")},
        {JDV_LITERAL("depth"), jdv_value_i64(depth)},
        {JDV_LITERAL("checkpoints"), list},
        {JDV_LITERAL("invalid_rows"), jdv_value_i64(invalid)},
        {JDV_LITERAL("scan_complete"), jdv_value_bool(!status)},
        {JDV_LITERAL("truncated"), jdv_value_bool(truncated)},
        {JDV_LITERAL("exit_code"), jdv_value_i64(status)}
    };
    (void)jash_discovery_add(&rows, report, sizeof report / sizeof report[0]);
    int rc = jash_discovery_emit_data_outcome(writer, context, "checkpoint", &rows, status,
        status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE,
        failed ? "checkpoint_read_failed" : invalid ? "invalid_checkpoint_rows" : truncated ? "checkpoint_capacity_exceeded" : NULL);
    jash_discovery_rows_free(&rows); return rc;
}

static int cp_native_auxiliary(jash_discovery_rows_t* rows, const char* record, const char* path,
                                int applied, int durable, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, record)},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, "rewind")},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("applied"), jdv_value_bool(applied)},
        {JDV_LITERAL("durable"), durable < 0 ? jdv_value_nothing() : jdv_value_bool(durable)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)}
    };
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    return jash_discovery_effects_flush(rows);
}

struct jash_rewind_plan {
    cp_t checkpoints[CP_MAX];
    struct stat identity;
    char path[600], scope[PATH_MAX];
    int count, selected, session, dry;
};

int jash_rewind_prepare(const char* name, int session, int dry, const char* scope,
                         jash_rewind_plan_t** output, const char** error) {
    *output = NULL; if (error) *error = NULL;
    jash_rewind_plan_t* plan = calloc(1, sizeof(*plan));
    if (!plan) { if (error) *error = "allocation_failed"; return 125; }
    plan->session = session; plan->dry = dry; plan->selected = -1;
    int status = 1, invalid = 0, truncated = 0;
    const char* reason = NULL;
    if (scope) {
        struct stat st;
        if (!realpath(scope, plan->scope) || stat(plan->scope, &st) || !S_ISDIR(st.st_mode)) {
            reason = "invalid_scope"; status = 2; goto failed;
        }
    }
    if (!cp_path(plan->path, sizeof plan->path)) { reason = "checkpoint_path_unavailable"; goto failed; }
    plan->count = cp_snapshot(plan->path, plan->checkpoints, CP_MAX, &invalid, &truncated, &plan->identity, 1);
    if (plan->count < 0 || invalid || truncated) { reason = "invalid_checkpoint_state"; goto failed; }
    if (!session) {
        for (int i = 0; i < plan->count; i++)
            if (!name || !strcmp(plan->checkpoints[i].name, name)) plan->selected = i;
        if (plan->selected < 0) { reason = "checkpoint_not_found"; goto failed; }
    }
    *output = plan; return 0;
failed:
    if (error) *error = reason;
    free(plan); return status;
}

void jash_rewind_plan_free(jash_rewind_plan_t* plan) { free(plan); }

int jash_rewind_apply(jash_rewind_plan_t* plan, jash_state_t* state,
                       jash_timeline_observer_t observe, jash_rewind_auxiliary_fn auxiliary,
                       void* context, jash_timeline_apply_summary_t* summary) {
    if (!plan || !state || !summary || !observe) return 125;
    memset(summary, 0, sizeof(*summary));
    summary->depth_before = summary->depth_after = jash_timeline_depth();
    summary->lost = jash_timeline_lost();
    cp_t current[CP_MAX]; struct stat identity = {0}; int invalid = 0, truncated = 0;
    int count = cp_snapshot(plan->path, current, CP_MAX, &invalid, &truncated, &identity, 1);
    if (count < 0 || invalid || truncated || count != plan->count ||
        identity.st_dev != plan->identity.st_dev || identity.st_ino != plan->identity.st_ino ||
        identity.st_size != plan->identity.st_size || BEAR_ST_MTIM(identity).tv_sec != BEAR_ST_MTIM(plan->identity).tv_sec ||
        BEAR_ST_MTIM(identity).tv_nsec != BEAR_ST_MTIM(plan->identity).tv_nsec ||
        BEAR_ST_CTIM(identity).tv_sec != BEAR_ST_CTIM(plan->identity).tv_sec || BEAR_ST_CTIM(identity).tv_nsec != BEAR_ST_CTIM(plan->identity).tv_nsec) {
        summary->error = "checkpoint_state_changed"; errno = ESTALE; return 1;
    }
    const char* canonical = *plan->scope ? plan->scope : NULL;
    jash_timeline_apply_options_t options = {.operation = JASH_TIMELINE_REWIND, .dry_run = plan->dry,
        .target_depth = plan->session ? 0 : plan->checkpoints[plan->selected].depth, .scope = canonical};
    int status = jash_timeline_apply(state, &options, observe, context, summary);
    if (!status && !plan->dry && summary->completed && plan->selected >= 0 && plan->checkpoints[plan->selected].cwd[0]) {
        const char* cwd = plan->checkpoints[plan->selected].cwd;
        size_t length = canonical ? strlen(canonical) : 0;
        int in_scope = !canonical || (length == 1 && canonical[0] == '/' && cwd[0] == '/') ||
                       (!strncmp(cwd, canonical, length) && (!cwd[length] || cwd[length] == '/'));
        if (in_scope) {
            char before[JASH_CWD_MAX]; memcpy(before, state->cwd, sizeof before);
            int changed = jash_timeline_restore_cwd(state, cwd);
            int saved = changed ? errno : 0;
            jash_rewind_auxiliary_t action = {.record = "cwd", .path = cwd,
                .applied = strcmp(before, state->cwd) != 0, .durable = -1,
                .error = changed ? "cwd_restore_failed" : NULL, .error_number = saved};
            int emitted = auxiliary ? auxiliary(context, &action) : 0;
            if (changed) { summary->failed++; summary->error = "cwd_restore_failed"; status = 1; }
            if (emitted) { summary->stopped = 1; status = emitted; }
        }
    }
    int prune = 0;
    for (int i = 0; i < plan->count; i++) if (plan->checkpoints[i].depth > jash_timeline_depth()) prune = 1;
    if (!status && !plan->dry && !canonical && summary->completed && prune) {
        int applied = 0, durable = 0;
        int failed = cp_prune_checked(plan->path, &plan->identity, plan->checkpoints, plan->count,
                                      jash_timeline_depth(), &applied, &durable);
        int saved = failed ? errno : 0;
        jash_rewind_auxiliary_t action = {.record = "checkpoints", .path = plan->path,
            .applied = applied, .durable = durable, .error = failed ? "checkpoint_prune_failed" : NULL,
            .error_number = saved};
        int emitted = auxiliary ? auxiliary(context, &action) : 0;
        if (failed) { summary->failed++; summary->error = "checkpoint_prune_failed"; status = 1; }
        if (emitted) { summary->stopped = 1; status = emitted; }
    }
    summary->depth_after = jash_timeline_depth(); return status;
}

static int cp_native_rewind_auxiliary(void* context, const jash_rewind_auxiliary_t* action) {
    return cp_native_auxiliary(context, action->record, action->path, action->applied,
                                action->durable, action->error);
}

static int cp_native_rewind(jash_state_t* state, jash_writer_t writer, void* context,
                             const char* command, int mode, const char* name, int session, int dry, const char* scope) {
    jash_rewind_plan_t* plan = NULL; const char* error = NULL;
    int status = jash_rewind_prepare(name, session, dry, scope, &plan, &error);
    if (status) return jash_discovery_error(writer, context, command, mode, error, status);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) {
        jash_rewind_plan_free(plan);
        return jash_discovery_error(writer, context, command, mode, "allocation_failed", 125);
    }
    status = dry ? 0 : jash_discovery_effects_begin(&rows, writer, context, command, mode, "jash.timeline.action.v1");
    if (status) { jash_rewind_plan_free(plan); jash_discovery_rows_free(&rows); return status; }
    jash_timeline_apply_summary_t summary;
    status = jash_rewind_apply(plan, state, recovery_action, cp_native_rewind_auxiliary, &rows, &summary);
    jash_rewind_plan_free(plan);
    (void)recovery_summary(&rows, "rewind", &summary);
    status = jash_discovery_emit_schema(writer, context, command, mode, &rows, status, "jash.timeline.action.v1");
    jash_discovery_rows_free(&rows); return status;
}

int jash_b_checkpoint_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(writer, context, "checkpoint", mode, "conflicting_formats", 2);
    if (!mode) return jash_discovery_text(jash_b_checkpoint, state, writer, context, argc, argv);
    int ended = 0, list = 0, diff = 0, create = 0;
    const char* name = NULL;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && (!strcmp(argv[i], "list") || !strcmp(argv[i], "--list") || !strcmp(argv[i], "-l")) && !name && !diff && !create) { list = 1; continue; }
        if (!ended && !strcmp(argv[i], "diff") && !name && !list && !create && !diff) { diff = 1; continue; }
        if (!ended && !strcmp(argv[i], "create") && !name && !list && !diff && !create) { create = 1; continue; }
        if (name || list || (!ended && argv[i][0] == '-') || !*argv[i] || strlen(argv[i]) >= CP_NAME || strpbrk(argv[i], "\t\r\n"))
            goto invalid;
        name = argv[i];
    }
    if (diff) return cp_native_rewind(state, writer, context, "checkpoint", mode, name, 0, 1, NULL);
    if (list || (!name && !create)) return cp_native_list(writer, context, mode);
    jash_checkpoint_result_t current;
    if (jash_checkpoint_prepare(name, &current))
        return jash_discovery_error(writer, context, "checkpoint", mode, "checkpoint_state_unrepresentable", 1);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "checkpoint", mode, "allocation_failed", 125);
    int status = jash_discovery_effects_begin(&rows, writer, context, "checkpoint", mode, "jash.checkpoint.action.v1");
    if (status) { jash_discovery_rows_free(&rows); return status; }
    status = jash_checkpoint_create_prepared(&current);
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("operation"), jash_discovery_string(&rows, "create")},
        {JDV_LITERAL("name"), jash_discovery_string(&rows, current.name[0] ? current.name : name)},
        {JDV_LITERAL("path"), jash_discovery_path(&rows, current.path)},
        {JDV_LITERAL("depth"), current.state_observed ? jdv_value_i64(current.depth) : jdv_value_nothing()},
        {JDV_LITERAL("ts"), current.state_observed ? jdv_value_i64(current.timestamp) : jdv_value_nothing()},
        {JDV_LITERAL("cwd"), jash_discovery_path(&rows, current.cwd[0] ? current.cwd : NULL)},
        {JDV_LITERAL("applied"), jdv_value_bool(current.applied)},
        {JDV_LITERAL("durable"), jdv_value_bool(current.durable)},
        {JDV_LITERAL("error"), jash_discovery_string(&rows, current.error)},
        {JDV_LITERAL("record_fields"), jash_discovery_string(&rows, "legacy_bounded")}
    };
    (void)jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0]);
    status = jash_discovery_emit_schema(writer, context, "checkpoint", mode, &rows, status, "jash.checkpoint.action.v1");
    jash_discovery_rows_free(&rows); return status;
invalid:
    return jash_discovery_error(writer, context, "checkpoint", mode, "invalid_options", 2);
}

int jash_b_rewind_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(writer, context, "rewind", mode, "conflicting_formats", 2);
    if (!mode) return jash_discovery_text(jash_b_rewind, state, writer, context, argc, argv);
    int ended = 0, dry = 0, session = 0, list = 0;
    const char *name = NULL, *scope = NULL;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && (!strcmp(argv[i], "--dry") || !strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "-n"))) { dry = 1; continue; }
        if (!ended && !strcmp(argv[i], "--scope")) {
            if (scope || ++i == argc || !*argv[i]) goto invalid;
            scope = argv[i]; continue;
        }
        if (!ended && (!strcmp(argv[i], "--session") || !strcmp(argv[i], "session") || !strcmp(argv[i], "--all"))) {
            if (name || session || list) goto invalid;
            session = 1; continue;
        }
        if (!ended && (!strcmp(argv[i], "list") || !strcmp(argv[i], "--list"))) {
            if (name || session || list) goto invalid;
            list = 1; continue;
        }
        if (name || session || list) goto invalid;
        if (!ended && !strcmp(argv[i], "--auto")) name = "auto";
        else {
            if ((!ended && argv[i][0] == '-') || !*argv[i] || strlen(argv[i]) >= CP_NAME || strpbrk(argv[i], "\t\r\n")) goto invalid;
            name = argv[i];
        }
    }
    if (list) {
        if (dry || scope) goto invalid;
        return cp_native_list(writer, context, mode);
    }
    return cp_native_rewind(state, writer, context, "rewind", mode, name, session, dry, scope);
invalid:
    return jash_discovery_error(writer, context, "rewind", mode, "invalid_options", 2);
}
