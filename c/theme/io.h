// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: theme/io — output writer abstraction.
//
// All theme/* and builtins/* code writes through a single function
// pointer typedef, jash_writer_t. This makes the whole layer testable
// on the host (point it at a stringbuilder) and target-portable
// (Josh: sys_write; standalone POSIX: write(STDOUT_FILENO, ...)).

#ifndef JASH_THEME_IO_H
#define JASH_THEME_IO_H

#include <stddef.h>

/* Write n bytes from buf. Return bytes written, or negative on error. */
typedef long (*jash_writer_t)(void* ctx, const char* buf, size_t n);

/* Helper: write a NUL-terminated string via the writer. */
long jash_writes(jash_writer_t w, void* ctx, const char* s);

/* Helper: write UNTRUSTED text with all control/escape bytes neutralised
 * (see io.c). Use for any attacker-controlled span sent to the terminal:
 * completion candidates, history/reverse-i-search matches, prompt cwd, etc. */
long jash_writes_sanitized(jash_writer_t w, void* ctx, const char* s);

/* Helper: tiny printf, ONLY %s, %d, %u, %ld, %lu, %x supported.
 * The full Bear printf is heavy; for banners/prompts this is plenty
 * and easy to test. */
long jash_writef(jash_writer_t w, void* ctx, const char* fmt, ...);

/* Canonical writer that goes to fd 2 (stderr). All diagnostics —
 * "command not found", syntax errors, builtin errors — must use this,
 * not the per-call writer (which goes to stdout). POSIX + bash parity.
 * Pass NULL as ctx. F E R M I ∞ H A R T */
long jash_stderr_writer(void* ctx, const char* buf, size_t n);

#endif /* JASH_THEME_IO_H */
