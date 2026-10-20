// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/adopt — adoption governor level resolver (ADR-0040 W2)
//
// One question, answered in ONE place: how hard may the shell push jash
// primitives on this session?
//   0 — off        (no nudges, no rewrites)
//   1 — nudge      (hints on stderr only — ceiling for INFERRED agents)
//   2 — rewrite    (W3+: upgrade well-known bash forms to jash primitives)
//   3 — aggressive (W3+: rewrite + destructive-op floor)
//
// Precedence (first match wins):
//   1. JASH_SIGIL=0 in env       → 0   recording explicitly off — no governor
//   2. JASH_ADOPT=0..3 in env    → that level (only explicit env reaches 2/3
//                                  by inference-free intent)
//   3. ~/.jash/adopt_mode file   → persisted level (`adopt` builtin toggle)
//   4. env smells like an agent  → 1   inference NEVER exceeds nudges — test
//                                  suites inherit agent-ish env from the
//                                  parent Claude Code session and must never
//                                  see rewrites inside $( ) captures
//   5. otherwise                 → 0

#ifndef JASH_CORE_ADOPT_H
#define JASH_CORE_ADOPT_H

// Resolved adoption level (0..3). Cached behind a 500ms monotonic TTL like the
// guard mode file — cross-process `adopt` toggles land within half a second.
int jash_adopt_level(void);

/* ADR-0040 W5 / ADR-0050 W9: FNV-1a64 key of the nearest ancestor dir with
 * .git ("-" outside any repo) — shared per-repo identity for adoption
 * telemetry, timeline and checkpoints. */
const char* jash_repo_key(void);

// What decided jash_adopt_level():
//   0=JASH_SIGIL=0, 1=env JASH_ADOPT, 2=mode file, 3=agent smell, 4=default,
//   5=auto governor (JASH_ADOPT=auto — level from cross-session telemetry).
int jash_adopt_source(void);

// Human name for a jash_adopt_source() value ("sigil-off", "env", ...).
const char* jash_adopt_source_name(int src);

// Persist level 0..3 to ~/.jash/adopt_mode; level=-1 removes the file
// (revert to env/inference). Returns 0 on success.
int jash_adopt_set(int level);

// ADR-0040 W4: cross-session telemetry. Appends ONE summary row for this
// session to ~/.jash/adoption_sessions.tsv:
//   ts  cmds  native  bypass  blocked  upgraded  absorbed  ab_failed  missed  level
// No-op below level 1 or when nothing was measured. Called from the -c
// epilogue AFTER jash_agent_emit() (the absorb fold must have happened).
// The file is self-capping: past ~32 KB it is compacted to the last 200 rows.
void jash_adopt_telemetry_flush(void);

// ADR-0040 W2 scorecard: when the governor is active (level>=1) and the
// cumulative recorded-command total hits a multiple of 10, emit ONE summary
// line to the REAL stderr (BUG-148: never the caller's writer). Called from
// the -c epilogue. Returns 1 if a line was emitted.
int jash_adopt_scorecard_maybe(void);

#endif
