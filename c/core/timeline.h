/* core/timeline.h — Command Timeline: universal undo (jash exclusive).
 * F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_CORE_TIMELINE_H
#define JASH_CORE_TIMELINE_H

#include <stddef.h>
#include <core/state.h>
#include <theme/io.h>
#include <core/recovery.h>

#define JASH_TIMELINE_MAX 128

typedef enum {
    JASH_UNDO_FILE_OVERWRITE = 1,  /* a=path,  b=backup copy           */
    JASH_UNDO_FILE_CREATE,         /* a=path created                   */
    JASH_UNDO_MOVE,                /* a=src,   b=dst                   */
    JASH_UNDO_TRASH,               /* a=original path                  */
    JASH_UNDO_ENV_SET,             /* a=name,  b=old value             */
    JASH_UNDO_ENV_UNSET,           /* a=name,  b=value it had          */
    JASH_UNDO_CHDIR,               /* a=previous cwd                   */
    JASH_UNDO_DIR_CREATE,          /* a=dir created (mkdir)            */
    JASH_UNDO_DIR_REMOVE,          /* a=dir removed, b=octal mode      */
    JASH_UNDO_META,                /* a=path,  b="mode:uid:gid" antes  */
    JASH_UNDO_LINK_CREATE,         /* a=linkpath, b="s|h:target"       */
    /* Wave 2: Process snapshots (Caminho A — /proc-based) */
    JASH_UNDO_PROC_SNAPSHOT,       /* a=pid, b=snapshot_path           */
    JASH_UNDO_PROC_RESTORE,        /* a=snapshot_path, b=original_pid  */
    JASH_UNDO_TIMES               /* a=entry, b=atime.sec:nsec:mtime.sec:nsec */
} jash_undo_kind_t;

/* Record one inverse operation. desc is the human line for `timeline`.
 * For ENV_SET on a previously-unset var, pass b="" and desc starting
 * with '+'. Cheap (memcpy into a lazy ring); safe to call always. */
void jash_timeline_record(jash_undo_kind_t kind, const char* desc,
                          const char* a, const char* b);
/* Checked recording for report producers. Exact bounded operational fields,
 * full entry id and durable guarded recovery handle, or an empty reference.
 * A return of -1 means recovery is unavailable, not that the mutation failed. */
int jash_timeline_record_ref(jash_undo_kind_t kind, const char* desc,
                              const char* a, const char* b, jash_recovery_ref_t* out);
void jash_timeline_consume_id(uint64_t id);

/* Snapshot `path` into the session backup area before clobbering it.
 * On success fills out_backup with the copy's path and returns 0.
 * Files >16MB are not backed up (returns -1; caller records without
 * inverse data or skips). */
int jash_timeline_backup_file(const char* path, char* out_backup,
                              size_t out_sz);

/* Session backup dir. SIGIL>=LOG: ~/.jash/tl_backup/<PID>/. */
const char* jash_timeline_backup_dir(void);
int jash_timeline_count(void);

/* ADR-0050 W1: journal a write redirect before it fires (see timeline.c). */
void jash_timeline_journal_redirect(const char* path, int append);

/* Enumerate live entries newest-first for the agent JSON envelope.
 * Fills kind/a/b for the idx-th valid entry (0 = newest). Returns 1 on
 * success, 0 when idx is out of range. Pointers borrow internal storage. */
int jash_timeline_peek(int idx_from_end, int* kind,
                       const char** a, const char** b);

/* ADR-0042 F7: like jash_timeline_peek but also returns the entry's wall-clock
 * timestamp (seconds), used by blame-file to cross-reference the chain. */
int jash_timeline_peek2(int idx_from_end, int* kind,
                        const char** a, const char** b, long* when);

/* Semantic recovery interface. Entry views borrow storage until the next
 * timeline mutation. Apply callbacks receive a stable copy for their call and
 * run after each inverse, before the next one. A nonzero callback return stops
 * the batch. The caller admits its output before calling apply. */
typedef struct {
    uint64_t id;
    int kind;
    const char *kind_name, *description, *a, *b;
    int64_t timestamp_seconds;
    int a_is_path, b_is_path, undoes;
} jash_timeline_entry_view_t;
typedef enum { JASH_TIMELINE_UNDO, JASH_TIMELINE_REDO, JASH_TIMELINE_REWIND } jash_timeline_operation_t;
typedef struct {
    jash_timeline_operation_t operation;
    unsigned count;
    uint64_t entry_id;
    int dry_run;
    int target_depth;
    const char* scope;
} jash_timeline_apply_options_t;
typedef struct {
    jash_timeline_entry_view_t entry;
    const char *operation, *status, *effect, *error;
    int dry_run, retained, redo_available, depth_before, depth_after;
} jash_timeline_action_t;
typedef struct {
    unsigned requested, completed, failed, planned, kept;
    int depth_before, depth_after, lost, stopped;
    const char* error;
} jash_timeline_apply_summary_t;
typedef int (*jash_timeline_observer_t)(void*, const jash_timeline_action_t*);
int jash_timeline_entry_at(unsigned newest_index, jash_timeline_entry_view_t*);
int jash_timeline_apply(jash_state_t*, const jash_timeline_apply_options_t*,
                        jash_timeline_observer_t, void*, jash_timeline_apply_summary_t*);
/* Restore a literal directory and coherent PWD/OLDPWD without recording a new
 * inverse. On environment failure, attempts to restore the original cwd. */
int jash_timeline_restore_cwd(jash_state_t*, const char*);
void jash_timeline_save(void);
void jash_timeline_restore(void);

/* ADR-0023 E2 (checkpoint/rewind). depth = current undoable stack height
 * (stable across `-c` while ≤ TIMELINE_MAX). rewind_to pops+inverts entries
 * until the depth is reached (coverage == undo); returns #reverted, sets
 * *out_failed. */
int jash_timeline_depth(void);
int jash_timeline_rewind_to(int target_depth, int* out_failed,
                            jash_state_t* s, jash_writer_t w, void* ctx);

/* Builtins. */
int jash_b_undo    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_redo    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_timeline_lost(void);   /* ADR-0050 W4: entries lost to the ring */

/* PATCH-SPEC jash-rewind-scope: scoped rewind (see timeline.c). */
int jash_timeline_rewind_scope(int target_depth, const char* scope,
                               int dry,
                               int* out_failed, int* out_kept,
                               jash_state_t* s, jash_writer_t w, void* ctx);
int jash_b_timeline(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_checkpoint(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_rewind    (jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_checkpoint_auto(void);   /* auto safety floor: drop/refresh the `auto` checkpoint */
/* Unique marker before one AI-auto response; records timeline depth, not a
 * filesystem image. Name stays distinct from rm's rolling `auto` marker. */
int jash_checkpoint_agent_begin(char* name, size_t capacity, int* depth);

/* ADR-0042 F8: read-only time-travel. Return the backup path of `path` as of
 * `target_depth`, or the current path if unchanged. Returns 0 on success,
 * -1 if unknown/too-old/deleted/not-a-regular-file. */
int jash_timeline_backup_at(const char* path, int target_depth,
                            char* out, size_t cap);

/* Wave 2: Process snapshots (Caminho A — /proc-based).
 * jash_timeline_proc_snapshot: capture PID tree + FD table + env for target PID.
 *   Writes snapshot to out_path (JSON). Returns 0 on success, -1 on failure.
 * jash_timeline_proc_restore: restore process tree from snapshot.
 *   Reads snapshot from path, re-executes tree. Returns new root PID in *out_pid.
 * jash_timeline_proc_tree: serialize current PID tree as JSON.
 * jash_timeline_proc_diff: diff two snapshots, output JSON. */
int jash_proc_snapshot_write_atomic(const char* path, const char* data, size_t size);
int jash_timeline_proc_snapshot(int target_pid, const char* out_path, size_t cap);
int jash_timeline_proc_restore(const char* snapshot_path, int* out_pid);
int jash_timeline_proc_tree(int root_pid, char* out_json, size_t cap);
int jash_timeline_proc_diff(const char* snap_a, const char* snap_b, char* out_json, size_t cap);

/* Free the ring + wipe the backup dir (call at shell exit). */
void jash_timeline_shutdown(void);

#endif /* JASH_CORE_TIMELINE_H */
