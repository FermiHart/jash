// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/drift — environment-drift forensics (ADR-0043 W1).
//
// "It worked yesterday." When a command fails, the expensive question is not
// WHAT failed (err_digest already covers that) but WHAT CHANGED since it last
// worked. On every successful agent -c command jash records a cheap
// environment fingerprint keyed by (command, cwd): PATH hash, resolved tool
// binary (path + mtime + size), git HEAD, and 16 build-relevant env vars. On
// a FAILURE of the same command in the same cwd it diffs the live fingerprint
// against the last-success row and names exactly what moved — one stderr
// line, zero happy-path cost beyond the row upsert.
//
// Ledger: ~/.jash/drift.tsv (flocked, 256-row cap, oldest evicted).
// JASH_DRIFT=0 disables. Agent sessions only. Warn-only, never blocks.
// F E R M I ∞ H A R T <contact@fermihart.com>
#ifndef JASH_CORE_DRIFT_H
#define JASH_CORE_DRIFT_H

/* 1 when JASH_DRIFT != 0, HOME is set, and this is an agent session. */
int jash_drift_enabled(void);

/* rc == 0: upsert the fingerprint row for (cmd, cwd).
 * rc != 0: if a last-success row exists for (cmd, cwd) and the live
 * fingerprint differs, emit one "[jash drift]" stderr line naming the
 * changes. cwd == NULL means "use getcwd()". No-op unless enabled. */
void jash_drift_note(const char* cmd, int rc, const char* cwd);

/* git HEAD (12 hex) do repo que contem `cwd`, subindo ate 6 niveis.
 * Escreve "?" quando nao ha repo. Exposto para que `resume` (JINV-057)
 * use a MESMA nocao de HEAD que o drift, e nao uma copia. */
void jash_drift_git_head(const char* cwd, char out[13]);

/* Exact identity used by the existing drift ledger. Pure: no filesystem,
 * environment observation, or state creation. */
void jash_drift_command_key(const char* command, const char* cwd, char out[17]);

#endif /* JASH_CORE_DRIFT_H */
