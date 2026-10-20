// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: core/argvbuf — the one growable argv builder (JINV-131).
//
// Why this file exists: jash had TWO argv builders — `build_argv` in
// parser/exec.c and `build_argv_local` in parser/pipeline.c — and BOTH were
// capped at 128 tokens / 8-32 KB. Any glob matching more than 128 files
// failed to execute. It survived a long time because our own heavy commands
// are recursive single-argument forms (`grep -rn pat c/`), which never build
// a wide argv; the shape that breaks is `cmd <many files>`.
//
// Two copies of a rule is the defect, not the number in either copy. The
// tokenizers still differ (pipeline.c also un-protects cmdsub sentinels,
// JINV-090), so what is shared here is the STORAGE and its growth policy —
// the part that was silently wrong in both.
//
// Contract, and the one trap in it: while building, tokens are recorded as
// OFFSETS, never pointers. `buf` moves under realloc, so a pointer captured
// mid-build dangles. Call jash_argvbuf_done() once, at the end, to turn the
// offsets into pointers and NULL-terminate.

#ifndef JASH_CORE_ARGVBUF_H
#define JASH_CORE_ARGVBUF_H

#include <stdlib.h>
#include <unistd.h>

#define JASH_ARGV_MIN    128    /* the old ceiling — now just the first step */
#define JASH_ARGBUF_MIN  8192

typedef struct {
    char*     buf;    /* token bytes, NUL-separated */
    unsigned  blen;
    unsigned  bcap;
    unsigned* aoff;   /* token offsets into buf — realloc-safe */
    char**    argv;   /* materialised by jash_argvbuf_done() */
    unsigned  argc;
    unsigned  acap;
} jash_argvbuf_t;

/* The kernel budgets argv and envp together, so we claim only part of the
 * ceiling and leave the rest for the environment. Cached: sysconf can be a
 * syscall and this sits on the hot exec path. */
static inline unsigned jash_argvbuf_limit(void) {
    static unsigned cached = 0;
    if (cached) return cached;
    long v = sysconf(_SC_ARG_MAX);
    if (v <= 0) v = 256 * 1024;                  /* conservative if unknown */
    v -= v / 4;                                  /* headroom for envp */
    if (v < JASH_ARGBUF_MIN) v = JASH_ARGBUF_MIN;/* never regress below the old cap */
    if (v > 64L * 1024 * 1024) v = 64L * 1024 * 1024;
    cached = (unsigned)v;
    return cached;
}

/* Ensure `need` more bytes fit. Fails only when the request genuinely exceeds
 * what execve would accept — never for a capacity we could have grown. */
static inline int jash_argvbuf_reserve(jash_argvbuf_t* o, unsigned need) {
    if (o->buf && o->blen + need <= o->bcap) return 0;
    unsigned limit = jash_argvbuf_limit();
    if (o->blen + need > limit) return -3;
    unsigned cap = o->bcap ? o->bcap : JASH_ARGBUF_MIN;
    while (cap < o->blen + need) {
        if (cap > limit / 2) { cap = limit; break; }
        cap *= 2;
    }
    char* nb = (char*)realloc(o->buf, cap);
    if (!nb) return -3;
    o->buf = nb;
    o->bcap = cap;
    return 0;
}

/* Ensure room for one more token. Keeps THREE spare slots, not one: the
 * terminating NULL, plus the two that maybe_add_ls_color() needs when it
 * inserts a flag at index 1 and writes argv[argc+1]. */
static inline int jash_argvbuf_slot(jash_argvbuf_t* o) {
    if (o->argv && o->argc + 3 <= o->acap) return 0;
    unsigned limit = jash_argvbuf_limit() / 2;  /* one token per 2 bytes is already absurd */
    unsigned cap = o->acap ? o->acap * 2 : JASH_ARGV_MIN;
    if (cap > limit) {
        if (o->acap >= limit) return -2;
        cap = limit;
    }
    unsigned* no = (unsigned*)realloc(o->aoff, (size_t)cap * sizeof *no);
    if (!no) return -2;
    o->aoff = no;
    char** na = (char**)realloc(o->argv, (size_t)cap * sizeof *na);
    if (!na) return -2;
    o->argv = na;
    o->acap = cap;
    return 0;
}

/* Offsets → pointers, exactly once, after buf has stopped moving. */
static inline void jash_argvbuf_done(jash_argvbuf_t* o) {
    for (unsigned i = 0; i < o->argc; i++) o->argv[i] = o->buf + o->aoff[i];
    o->argv[o->argc] = 0;
}

static inline void jash_argvbuf_rewind(jash_argvbuf_t* o) {
    o->blen = 0;
    o->argc = 0;
}

/* Release everything and return the struct to its zero state, so a stale
 * pointer can't be handed to done() after the buffers are gone. Safe on a
 * zeroed struct and safe to call twice — the callers that matter are error
 * paths, which is exactly where a "free only on success" API leaks. */
static inline void jash_argvbuf_free(jash_argvbuf_t* o) {
    free(o->buf);
    free(o->aoff);
    free(o->argv);
    o->buf = NULL; o->aoff = NULL; o->argv = NULL;
    o->blen = o->bcap = o->argc = o->acap = 0;
}

/* Pre-grow to an upper bound so a later build cannot need to allocate.
 *
 * This exists for one reason: pipeline.c builds argv in the CHILD after
 * fork(), and jash starts a background loader thread (boot.c), so the child
 * of a multithreaded parent must not call malloc — the allocator lock may
 * have been held by another thread at fork time. The parent calls this before
 * forking; every reserve in the child then returns on its fast path without
 * touching realloc. Bounds are exact: the tokenizer emits at most one byte
 * per input byte plus a NUL per token, and a token needs at least one byte
 * plus its separator. */
static inline int jash_argvbuf_pregrow(jash_argvbuf_t* o, unsigned max_bytes) {
    unsigned bytes = max_bytes + 2;
    unsigned toks  = max_bytes / 2 + 4;
    if (jash_argvbuf_reserve(o, bytes) != 0) return -3;
    while (o->acap < toks + 3) {
        unsigned before = o->acap;
        if (jash_argvbuf_slot(o) != 0) return -2;
        if (o->acap == before) break;   /* hit the ceiling — stop, do not spin */
    }
    return 0;
}

#endif /* JASH_CORE_ARGVBUF_H */
