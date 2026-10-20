// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_RECOVERY_OUTPUT_H
#define JASH_BUILTINS_RECOVERY_OUTPUT_H
#include "discovery_output.h"
#include <core/timeline.h>
#include <stdio.h>

/* Direct checkpoint collector. Prepare performs validation only; the caller
 * must admit its complete receipt before create_prepared can publish state.
 * A nonzero result can still have applied=1/durable=0. These are observed
 * engine phases, not a guarantee that arbitrary external writes are undoable. */
typedef struct {
    char name[64], path[600], cwd[512];
    int named, state_observed;
    int64_t depth, timestamp;
    int applied, durable, error_number;
    const char* error;
} jash_checkpoint_result_t;
int jash_checkpoint_prepare(const char* name, jash_checkpoint_result_t* result);
int jash_checkpoint_create_prepared(jash_checkpoint_result_t* result);

/* Prepared rewind owns a checked checkpoint snapshot. Preparation has no
 * effects; apply emits each inverse/auxiliary observation before the next
 * phase. A nonzero observer return stops CWD restoration and pruning too. */
typedef struct jash_rewind_plan jash_rewind_plan_t;
typedef struct {
    const char *record, *path, *error;
    int applied, durable, error_number; /* durable=-1 for process-local CWD */
} jash_rewind_auxiliary_t;
typedef int (*jash_rewind_auxiliary_fn)(void*, const jash_rewind_auxiliary_t*);
int jash_rewind_prepare(const char* name, int session, int dry, const char* scope,
                         jash_rewind_plan_t** plan, const char** error);
int jash_rewind_apply(jash_rewind_plan_t*, jash_state_t*, jash_timeline_observer_t,
                       jash_rewind_auxiliary_fn, void*, jash_timeline_apply_summary_t*);
void jash_rewind_plan_free(jash_rewind_plan_t*);

/* Receipt construction consumes semantic engine results, never human text. */
static inline int recovery_action(void* context, const jash_timeline_action_t* action) {
    jash_discovery_rows_t* rows = context;
    const jash_timeline_entry_view_t* entry = &action->entry;
    char id[32]; snprintf(id, sizeof id, "%llu", (unsigned long long)entry->id);
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "action")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, action->operation)},
        {JDV_LITERAL("entry_id"), jash_discovery_string(rows, id)},
        {JDV_LITERAL("id"), jdv_value_u64(entry->id)},
        {JDV_LITERAL("kind"), jash_discovery_string(rows, entry->kind_name)},
        {JDV_LITERAL("description"), jash_discovery_string(rows, entry->description)},
        {JDV_LITERAL("a"), entry->a_is_path ? jash_discovery_path(rows, entry->a) : jash_discovery_string(rows, entry->a)},
        {JDV_LITERAL("b"), entry->b_is_path ? jash_discovery_path(rows, entry->b) : jash_discovery_string(rows, entry->b)},
        {JDV_LITERAL("status"), jash_discovery_string(rows, action->status)},
        {JDV_LITERAL("effect"), jash_discovery_string(rows, action->effect)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, action->error)},
        {JDV_LITERAL("dry_run"), jdv_value_bool(action->dry_run)},
        {JDV_LITERAL("retained"), jdv_value_bool(action->retained)},
        {JDV_LITERAL("redo_available"), jdv_value_bool(action->redo_available)},
        {JDV_LITERAL("depth_before"), jdv_value_i64(action->depth_before)},
        {JDV_LITERAL("depth_after"), jdv_value_i64(action->depth_after)},
        {JDV_LITERAL("record_fields"), jash_discovery_string(rows, "legacy_bounded")},
        {JDV_LITERAL("guards"), jash_discovery_string(rows, "legacy_inverse")},
    };
    if (jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0])) return 75;
    return jash_discovery_effects_flush(rows);
}

static inline int recovery_summary(jash_discovery_rows_t* rows, const char* operation,
                                    const jash_timeline_apply_summary_t* summary) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("record"), jash_discovery_string(rows, "summary")},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("requested"), jdv_value_u64(summary->requested)},
        {JDV_LITERAL("completed"), jdv_value_u64(summary->completed)},
        {JDV_LITERAL("failed"), jdv_value_u64(summary->failed)},
        {JDV_LITERAL("planned"), jdv_value_u64(summary->planned)},
        {JDV_LITERAL("kept"), jdv_value_u64(summary->kept)},
        {JDV_LITERAL("depth_before"), jdv_value_i64(summary->depth_before)},
        {JDV_LITERAL("depth_after"), jdv_value_i64(summary->depth_after)},
        {JDV_LITERAL("lost"), jdv_value_u64((uint64_t)summary->lost)},
        {JDV_LITERAL("stopped"), jdv_value_bool(summary->stopped)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, summary->error)},
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

int jash_b_timeline_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_undo_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_redo_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_checkpoint_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_rewind_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_verify_argv(jash_state_t*, jash_writer_t, void*, int, char**);
int jash_b_trash_argv(jash_state_t*, jash_writer_t, void*, int, char**);
#endif
