// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/fschange — files_changed scanner (ADR-0038 W2).
//
// Snapshot the cwd tree before a command, diff it after: the agent SEES the
// command's real blast radius (created/modified/deleted) instead of guessing
// or burning follow-up commands. Complements the timeline (which journals
// jash's OWN mutations): this catches what externals did — compilers, sed -i,
// git checkout — that the timeline can never see.
//
// Cost control: one snapshot+one rescan per -c invocation (tool-call
// granularity, never per segment), agent sessions only, skipped entirely for
// obviously-pure commands, capped depth/entries, .git//.jash//.cache/
// node_modules excluded. JASH_CHANGES=0 disables. F E R M I ∞ H A R T

#ifndef JASH_CORE_FSCHANGE_H
#define JASH_CORE_FSCHANGE_H

#include <stddef.h>
#include <core/world.h>

typedef struct jash_fssnap jash_fssnap_t;

#define JASH_FSDIFF_MAX_REPORT 24
#define JASH_FSDIFF_PATH_MAX  1024

typedef struct {
    char paths[JASH_FSDIFF_MAX_REPORT][JASH_FSDIFF_PATH_MAX]; /* root-relative */
    char kind [JASH_FSDIFF_MAX_REPORT]; /* '+' created  '~' modified  '-' deleted */
    /* ADR-0038 W3: 1 when the file's PRE-command version differed from the
     * version the agent last READ (read ledger) — the agent mutated a file
     * it had a stale view of. */
    unsigned char stale[JASH_FSDIFF_MAX_REPORT];
    unsigned long stale_read_ts[JASH_FSDIFF_MAX_REPORT];
    int  stale_total;
    int  n;          /* reported entries (≤ MAX_REPORT) */
    int  total;      /* real number of changes (may exceed n) */
    int  valid;      /* 0 = snapshot skipped/blew a cap — say nothing */
} jash_fsdiff_t;

/* NULL when the tree blows the caps (report nothing rather than lie). */
jash_fssnap_t* jash_fssnap_take(const char* root);

/* Take the fschange pre-snapshot and world-delta startup snapshot in one tree
 * walk while preserving each subsystem's independent exclusion policy. */
jash_fssnap_t* jash_fssnap_take_with_world(const char* root, int world_depth,
                                           jash_world_scan_t** world_out);

/* Rescan root, diff against snap, fill out, free snap. */
void jash_fssnap_diff(jash_fssnap_t* snap, const char* root, jash_fsdiff_t* out);

/* Cheap gate: 1 when the command is obviously read-only/pure (single simple
 * command whose name is in the read whitelist or pure-builtin set). */
int jash_fschange_command_is_pure(const char* command_str);

/* ADR-0038 W5: persist the reported delta to ~/.jash/fsjournal.tsv
 * (ts \t sid \t kind \t path). The impact map learns file→test edges by
 * correlating this journal with the session command records. */
void jash_fsdiff_persist(const jash_fsdiff_t* fsd);

#endif /* JASH_CORE_FSCHANGE_H */
