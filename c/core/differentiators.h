// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/differentiators — THE canonical table of what makes jash
// different from bash/zsh/fish (ADR-0040 W1).
//
// History: this list used to live in four places and drifted —
//   c/core/guard.c                    JASH_DIFFERENTIATORS[] + is_jash_native()
//   c/core/guard.h                    the same list, as a comment
//   c/main.c                          FIRST CONTACT brief (superpowers pitch)
//   c/builtins/builtin_agent_guide.c  GUIDE[] + is_jash_unique[]
// Every consumer now reads THIS table. Register a new differentiator here and
// the guard metrics, the first-contact brief, the agent guide, the dashboard
// and the hint engine all see it at once.

#ifndef JASH_CORE_DIFFERENTIATORS_H
#define JASH_CORE_DIFFERENTIATORS_H

#include <stddef.h>

/* Counts toward jash-native adoption in guard metrics (agent_metrics.tsv). */
#define JASH_DIFF_NATIVE 0x1u
/* Has no bash/zsh/fish equivalent (agent-guide "unique primitives" set). */
#define JASH_DIFF_UNIQUE 0x2u

typedef struct {
    const char* name;     /* builtin name as typed by the agent                */
    const char* category; /* guide section (Memory/Safety/...), NULL = none   */
    const char* why;      /* one-line agent-facing pitch, NULL = registry help */
    unsigned    flags;    /* JASH_DIFF_* bits                                  */
} jash_diff_t;

/* Whole table, NULL-name terminated; *count (optional) receives the size. */
const jash_diff_t* jash_diff_table(size_t* count);

/* Lookup by exact builtin name; NULL when not a differentiator. */
const jash_diff_t* jash_diff_find(const char* name);

/* 1 when `name` counts as native adoption for guard metrics. */
int jash_diff_is_native(const char* name);

/* 1 when `name` has no bash/zsh/fish equivalent. */
int jash_diff_is_unique(const char* name);

/* Canonical guide section order, NULL-terminated. */
const char* const* jash_diff_categories(size_t* count);

/* The FIRST CONTACT "superpowers" block shared by the main.c brief. */
const char* jash_diff_brief(void);

#endif
