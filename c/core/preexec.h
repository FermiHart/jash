// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/preexec — pre-exec absorption engine (ADR-0040 W3)
//
// `bash -c 'STR'` → re-exec of the running jash binary with the same args.
// See preexec.c for the full contract.

#ifndef JASH_CORE_PREEXEC_H
#define JASH_CORE_PREEXEC_H

// Pure gate: 1 when (cmd, args) is an absorbable `<shell> -c STR` under the
// current adoption level (>= 2, explicit intent only). No side effects —
// call sites use it to ORDER absorption before the bypass guard.
int jash_preexec_will_absorb(const char* cmd, const char* args);

// Full absorption: gate + resolve the running binary into self[selfsz] +
// record adoption metrics (native + upgraded) + one-line stderr notice
// (once per process). Returns 1 when the caller must exec `self` with the
// SAME args string; 0 → proceed with the untouched external.
int jash_preexec_absorb(const char* cmd, const char* args,
                        char* self, unsigned selfsz);

// ADR-0040 W8: at adoption level 1 (observe-only), record an absorbable
// one-liner that level 2 would have run natively. No output; feeds the
// scorecard's calibration suggestion. No-op at levels 0/2/3 and when
// JASH_ABSORB=0.
void jash_preexec_note_missed(const char* cmd, const char* args);

// Session count of missed absorptions (see note above).
unsigned jash_preexec_missed_count(void);

// ADR-0040 W3: level 3's OWN policy — absorb `<shell> -c STR` when it is a
// SEGMENT of a simple pipeline (`bash -c A | bash -c B`). Levels <= 2 never
// rewrite segments: pipeline fd plumbing belongs to the caller and level 2
// only owns top-level simple commands. On success self[selfsz] holds the
// running jash binary; the caller execs it with the SAME args string.
int jash_preexec_absorb_seg(const char* cmd, const char* args,
                            char* self, unsigned selfsz);

// Calibration twin of note_missed for segments: at levels 1-2 count a
// pipeline segment that level 3 would have absorbed. Feeds the same
// scorecard counter. No-op at levels 0/3 and when JASH_ABSORB=0.
void jash_preexec_note_missed_seg(const char* cmd, const char* args);

#endif
