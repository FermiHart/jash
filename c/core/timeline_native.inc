// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Native observation and action adapters over the same retained timeline.
#include <builtins/recovery_output.h>

static void tl_entry_view(const timeline_entry_t* entry, int undoes, jash_timeline_entry_view_t* view) {
    *view = (jash_timeline_entry_view_t){
        .id = entry->id, .kind = entry->kind, .kind_name = kind_tag(entry->kind),
        .description = entry->desc, .a = entry->a, .b = entry->b,
        .timestamp_seconds = (int64_t)entry->when, .undoes = undoes,
        .a_is_path = tl_kind_has_paths(entry->kind) || entry->kind == JASH_UNDO_CHDIR || entry->kind == JASH_UNDO_PROC_RESTORE,
        .b_is_path = entry->kind == JASH_UNDO_MOVE || entry->kind == JASH_UNDO_TRASH ||
            entry->kind == JASH_UNDO_FILE_OVERWRITE || entry->kind == JASH_UNDO_PROC_SNAPSHOT
    };
}

int jash_timeline_entry_at(unsigned index, jash_timeline_entry_view_t* view) {
    if (!view) return 0;
    unsigned found = 0;
    int count = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;
    for (int i = 0; g_tl && i < count; i++) {
        const timeline_entry_t* entry = tl_slot(i);
        if (!entry || !entry->valid) continue;
        if (found++ == index) { tl_entry_view(entry, index == 0, view); return 1; }
    }
    return 0;
}

static int tl_native_in_scope(const timeline_entry_t* entry, const char* scope) {
    if (!scope) return 1;
    size_t size = strlen(scope);
    const char* paths[] = {tl_kind_has_paths(entry->kind) ? entry->a : NULL,
                          entry->kind == JASH_UNDO_MOVE ? entry->b : NULL};
    for (unsigned i = 0; i < 2; i++) {
        if (!paths[i] || !*paths[i]) continue;
        if ((size == 1 && scope[0] == '/' && paths[i][0] == '/') ||
            (!strncmp(paths[i], scope, size) && (!paths[i][size] || paths[i][size] == '/'))) return 1;
    }
    return 0;
}

int jash_timeline_apply(jash_state_t* state, const jash_timeline_apply_options_t* options,
                        jash_timeline_observer_t observer, void* context, jash_timeline_apply_summary_t* summary) {
    if (!state || !options || !observer || !summary) return 2;
    memset(summary, 0, sizeof *summary);
    summary->depth_before = summary->depth_after = g_tl_count;
    summary->lost = tl_lost_to_ring();
    int redo = options->operation == JASH_TIMELINE_REDO;
    int rewind = options->operation == JASH_TIMELINE_REWIND;
    const char* operation = redo ? "redo" : rewind ? "rewind" : "undo";
    if (options->operation < JASH_TIMELINE_UNDO || options->operation > JASH_TIMELINE_REWIND || (!rewind && (!options->count || options->count > TIMELINE_MAX)) ||
        (rewind && options->target_depth < 0) || (options->entry_id && (redo || rewind)) ||
        (options->scope && (!rewind || options->scope[0] != '/'))) {
        summary->error = "invalid_options"; return 2;
    }
    if (rewind && options->target_depth < summary->lost) {
        summary->error = "history_unavailable"; return 1;
    }
    summary->requested = rewind ? 0u : options->entry_id ? 1u : options->count;
    timeline_entry_t selected[TIMELINE_MAX];
    timeline_entry_t* original[TIMELINE_MAX];
    unsigned selected_count = 0;
    int start = redo ? g_redo_count : g_tl_count;
    int available = start < TIMELINE_MAX ? start : TIMELINE_MAX;
    int selected_index = -1;
    if (options->entry_id) {
        selected_index = tl_find_by_id(options->entry_id);
        if (selected_index < 0) {
            summary->error = selected_index == -2 ? "ambiguous_entry_id" : "entry_not_found"; return 1;
        }
    }
    for (int i = 0; i < available; i++) {
        if (rewind && start - i <= options->target_depth) break;
        if (options->entry_id && i != selected_index) continue;
        timeline_entry_t* entry = redo ? (g_redo ? &g_redo[(start - 1 - i) % TIMELINE_MAX] : NULL) : tl_slot(i);
        if (!entry || !entry->valid) continue;
        selected[selected_count] = *entry;
        original[selected_count++] = entry;
        if (options->entry_id || (!rewind && selected_count == options->count)) break;
    }
    summary->requested = rewind ? selected_count : options->entry_id ? 1u : options->count;
    if (!selected_count && !rewind) { summary->error = redo ? "nothing_to_redo" : "nothing_to_undo"; return 1; }
    int result = 0;
    for (unsigned i = 0; i < selected_count; i++) {
        timeline_entry_t* entry = &selected[i];
        jash_timeline_action_t action = {.operation = operation, .dry_run = options->dry_run,
            .retained = 1, .depth_before = g_tl_count, .depth_after = g_tl_count,
            .status = "planned", .effect = "none"};
        tl_entry_view(entry, !redo && i == 0, &action.entry);
        if (!tl_native_in_scope(entry, options->scope)) {
            action.status = "kept"; summary->kept++;
        } else if (options->dry_run) summary->planned++;
        else {
            errno = 0;
            int previous_redo = g_redo_count;
            if (redo) g_reapplying_redo++;
            else g_applying_inverse++;
            int rc = redo ? redo_one(entry, state, jash_stderr_writer, NULL) :
                            undo_entry_flow(entry, state, jash_stderr_writer, NULL);
            if (redo) g_reapplying_redo--;
            else g_applying_inverse--;
            if (rc) {
                action.status = "failed";
                action.effect = "possible";
                action.error = errno == ENOTSUP ? "inverse_unsupported" : "inverse_failed";
                summary->failed++; result = 1;
            } else {
                if (!redo) tl_note_consumed(entry->id);
                original[i]->valid = 0;
                if (redo) {
                    while (g_redo_count && !g_redo[(g_redo_count - 1) % TIMELINE_MAX].valid) g_redo_count--;
                } else if (!options->entry_id && !options->scope) {
                    while (g_tl_count > summary->lost && tl_slot(0) && !tl_slot(0)->valid) g_tl_count--;
                }
                action.status = "completed"; action.effect = "applied"; action.retained = 0;
                action.redo_available = !redo && g_redo_count > previous_redo;
                summary->completed++;
            }
            action.depth_after = g_tl_count;
        }
        int observed = observer(context, &action);
        if (observed) { summary->stopped = 1; result = observed; break; }
        if (result) break; /* Preserve the failed entry and every older entry. */
    }
    summary->depth_after = g_tl_count;
    if (!result && !rewind && selected_count < summary->requested) {
        summary->error = "history_exhausted"; result = 1;
    }
    if (summary->completed) {
        char detail[160];
        snprintf(detail, sizeof detail, "%s completed=%u failed=%u kept=%u", operation,
                 summary->completed, summary->failed, summary->kept);
        jash_sigil_attest(operation, detail);
    }
    return result;
}

static int tl_native_uint(const char* text, uint64_t* result) {
    if (!text || !*text) return -1;
    uint64_t value = 0;
    for (const char* p = text; *p; p++) {
        if (*p < '0' || *p > '9' || value > (UINT64_MAX - (unsigned)(*p - '0')) / 10) return -1;
        value = value * 10 + (unsigned)(*p - '0');
    }
    *result = value; return 0;
}

static int tl_native_command(jash_state_t* state, jash_writer_t writer, void* context,
                              int argc, char** argv, int redo) {
    const char* command = redo ? "redo" : "undo";
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(writer, context, command, mode, "conflicting_formats", 2);
    if (!mode) return jash_discovery_text(redo ? jash_b_redo : jash_b_undo, state, writer, context, argc, argv);
    jash_timeline_apply_options_t options = {.operation = redo ? JASH_TIMELINE_REDO : JASH_TIMELINE_UNDO, .count = 1};
    int ended = 0, numeric = 0, id = 0, special = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && (!strcmp(argv[i], "--ref") || !strcmp(argv[i], "--inspect") || !strcmp(argv[i], "--usage") ||
            !strcmp(argv[i], "--quota") || !strcmp(argv[i], "--store") || !strcmp(argv[i], "--gc") ||
            !strcmp(argv[i], "--pin") || !strcmp(argv[i], "--unpin"))) special = 1;
    }
    if (special) {
        if (redo)
            return jash_discovery_error(writer, context, command, mode, "unsupported_recovery_mode", 2);
        char* joined = jash_argv_join(argc, argv);
        if (!joined) return jash_discovery_error(writer, context, command, mode, "allocation_failed", 125);
        int rc = jash_recovery_command(writer, context, joined); free(joined); return rc;
    }
    ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && (!strcmp(argv[i], "--dry") || !strcmp(argv[i], "--dry-run") || !strcmp(argv[i], "-n"))) {
            options.dry_run = 1; continue;
        }
        if (!redo && !strcmp(argv[i], "id") && !numeric && !id) { id = 1; continue; }
        const char* value = argv[i];
        if (!redo && value[0] == '#' && value[1]) { if (id || numeric) goto invalid; id = 1; value++; }
        uint64_t number;
        if (numeric++ || tl_native_uint(value, &number) || !number || (!id && number > TIMELINE_MAX)) goto invalid;
        if (id) options.entry_id = number; else options.count = (unsigned)number;
    }
    if (id && !options.entry_id) goto invalid;
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, command, mode, "allocation_failed", 125);
    int rc = 0;
    if (!options.dry_run) rc = jash_discovery_effects_begin(&rows, writer, context, command, mode, "jash.timeline.action.v1");
    if (!rc) {
        jash_timeline_apply_summary_t summary;
        rc = jash_timeline_apply(state, &options, recovery_action, &rows, &summary);
        (void)recovery_summary(&rows, command, &summary);
        rc = jash_discovery_emit_schema(writer, context, command, mode, &rows, rc, "jash.timeline.action.v1");
    }
    jash_discovery_rows_free(&rows); return rc;
invalid:
    return jash_discovery_error(writer, context, command, mode, "invalid_options", 2);
}

int jash_b_undo_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    return tl_native_command(state, writer, context, argc, argv, 0);
}
int jash_b_redo_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    return tl_native_command(state, writer, context, argc, argv, 1);
}

int jash_b_timeline_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(writer, context, "timeline", mode, "conflicting_formats", 2);
    uint64_t offset = 0, limit = TIMELINE_MAX;
    int ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && (!strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit"))) {
            int is_limit = !strcmp(argv[i], "--limit"); uint64_t value;
            if (++i == argc || tl_native_uint(argv[i], &value) || (is_limit && !value)) goto invalid;
            if (is_limit) limit = value; else offset = value;
        } else goto invalid;
    }
    if (!mode && (offset || limit != TIMELINE_MAX)) goto invalid;
    if (mode != JASH_DISCOVERY_DATA) {
        char text[160];
        if (mode == JASH_DISCOVERY_JSON) snprintf(text, sizeof text, "--json --offset %llu --limit %llu",
            (unsigned long long)offset, (unsigned long long)limit);
        else text[0] = 0;
        return jash_b_timeline(state, writer, context, text);
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "timeline", mode, "allocation_failed", 125);
    unsigned total = (unsigned)jash_timeline_count(), count = 0;
    jdv_value_t entries[TIMELINE_MAX];
    for (uint64_t i = offset; i < total && count < limit && count < TIMELINE_MAX; i++) {
        jash_timeline_entry_view_t entry;
        if (!jash_timeline_entry_at(total - 1u - (unsigned)i, &entry)) { rows.error = 1; break; }
        char id[32]; snprintf(id, sizeof id, "%llu", (unsigned long long)entry.id);
        jdv_record_input_t fields[] = {
            {JDV_LITERAL("id"), jdv_value_u64(entry.id)},
            {JDV_LITERAL("entry_id"), jash_discovery_string(&rows, id)},
            {JDV_LITERAL("kind"), jash_discovery_string(&rows, entry.kind_name)},
            {JDV_LITERAL("desc"), jash_discovery_string(&rows, entry.description)},
            {JDV_LITERAL("a"), entry.a_is_path ? jash_discovery_path(&rows, entry.a) : jash_discovery_string(&rows, entry.a)},
            {JDV_LITERAL("b"), entry.b_is_path ? jash_discovery_path(&rows, entry.b) : jash_discovery_string(&rows, entry.b)},
            {JDV_LITERAL("ts"), jdv_value_i64(entry.timestamp_seconds)},
            {JDV_LITERAL("undoes"), jdv_value_bool(entry.undoes)}
        };
        if (jdv_value_record(&rows.arena, fields, sizeof fields / sizeof fields[0], &entries[count])) { rows.error = 1; break; }
        count++;
    }
    jdv_value_t list = jdv_value_nothing();
    if (jdv_value_list(&rows.arena, entries, count, &list)) rows.error = 1;
    jdv_record_input_t report[] = {
        {JDV_LITERAL("schema"), jash_discovery_string(&rows, "jash.timeline.v1")},
        {JDV_LITERAL("record_fields"), jash_discovery_string(&rows, "legacy_bounded")},
        {JDV_LITERAL("mutations"), jdv_value_u64(total)},
        {JDV_LITERAL("lost"), jdv_value_u64((uint64_t)tl_lost_to_ring())},
        {JDV_LITERAL("entries"), list},
        {JDV_LITERAL("count"), jdv_value_u64(count)},
        {JDV_LITERAL("next_offset"), offset < total && count < total - offset ? jdv_value_u64(offset + count) : jdv_value_nothing()}
    };
    (void)jash_discovery_add(&rows, report, sizeof report / sizeof report[0]);
    int rc = jash_discovery_emit(writer, context, "timeline", mode, &rows, 0);
    jash_discovery_rows_free(&rows); return rc;
invalid:
    return jash_discovery_error(writer, context, "timeline", mode, "invalid_options", 2);
}
