// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/linediff — order-preserving multiset line diff (ADR-0042 F1).
//
// Given two files (the previous captured output of a whitelisted read-only
// command and the new one), report which concrete lines were ADDED and which
// were REMOVED, in file order. Multiset semantics: a line present N× in prev
// and M× in new contributes max(0, M−N) additions and max(0, N−M) removals —
// so duplicate lines are handled correctly and unchanged lines never appear.
//
// This powers `memoΔ`: instead of re-emitting a whole body the agent already
// has, jash prints only the `+`/`-` lines that actually changed.
//
// Caps (ADR-0042 F1): ≤1000 lines and ≤64 KiB per side. Over either cap the
// diff is refused (return 1) and the caller falls back to the full body — a
// large structural diff is not worth the tokens and risks re-flooding.
// F E R M I ∞ H A R T

#ifndef JASH_CORE_LINEDIFF_H
#define JASH_CORE_LINEDIFF_H

typedef struct {
    char*        prev_buf;   /* owned: prev file bytes, split in place  */
    char*        new_buf;    /* owned: new  file bytes, split in place  */
    const char** add;        /* owned: pointers into new_buf (added)    */
    int          n_add;      /* total additions (true count)            */
    const char** del;        /* owned: pointers into prev_buf (removed) */
    int          n_del;      /* total removals (true count)             */
} jash_linediff_t;

/* Compute the diff. Returns:
 *    0 — success, *out populated (free with jash_linediff_free);
 *    1 — over cap (either side > 1000 lines or > 64 KiB): *out is zeroed;
 *   -1 — I/O error (a path could not be read): *out is zeroed.
 * On any non-zero return there is nothing to free. */
int  jash_linediff(const char* prev_path, const char* new_path,
                   jash_linediff_t* out);

void jash_linediff_free(jash_linediff_t* d);

#endif /* JASH_CORE_LINEDIFF_H */
