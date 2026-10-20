// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/glob — wildcard pattern matching
//
// Pattern grammar:
//   *       — match any sequence of chars (incl. empty), within one name segment
//   **      — match any sequence of chars including '/' (recursive descent)
//   ?       — match exactly one char
//   [abc]   — character class (Phase 3, not yet)
//   literal — match the same byte
//
// `**` is a manifesto §2 evolution over bash (which requires `shopt -s
// globstar`). In jash it is always on, never opt-in.

#ifndef JASH_CORE_GLOB_H
#define JASH_CORE_GLOB_H

#include "state.h"

/* Match a name against a pattern. Returns 1 on match, 0 otherwise.
 * Pure function — no state, no I/O, no allocation. Safe to call
 * from anywhere. */
int jash_glob_match(const char* pat, const char* name);

/* SECURITY (audit F6): aggregate anti-ReDoS step budget for ONE whole line
 * expansion. The matcher is invoked once per dirent, so a per-call budget
 * alone lets a hostile pattern spend the full cap PER ENTRY of a large
 * directory. Callers expanding a full line thread a single counter through
 * every match call; when it runs out the matcher answers "no match" (the
 * safe default) and the caller is expected to refuse the expansion. */
#define JASH_GLOB_LINE_STEPS 64000000L

/* Context variant: additionally decrements *line_steps (shared across one
 * expansion). Pass NULL to get plain per-call behavior. */
int jash_glob_match_ctx(const char* pat, const char* name, long* line_steps);

/* Pattern contains glob meta-characters? Cheap pre-check used by
 * expanders to skip the work when there's nothing to expand. */
int jash_glob_has_meta(const char* pat);

#endif /* JASH_CORE_GLOB_H */
