// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: core/readbuf — buffered read abstraction.
//
// Wraps sys_read with an 8KB buffer so that byte-by-byte consumers
// (mapfile, read, select, heredoc) don't pay a syscall per char.

#ifndef JASH_CORE_READBUF_H
#define JASH_CORE_READBUF_H

#include <sys/syscalls.h>
#include <stdio.h>

#define JASH_READBUF_SIZE 8192

typedef struct {
    int   fd;
    char  buf[JASH_READBUF_SIZE];
    int   pos;    /* next unread byte index */
    int   len;    /* bytes valid in buf */
} jash_readbuf_t;

/* Initialize a read buffer for the given fd. */
static inline void jash_readbuf_init(jash_readbuf_t* rb, int fd) {
    rb->fd  = fd;
    rb->pos = 0;
    rb->len = 0;
}

/* Refill the internal buffer. Returns bytes read, or <=0 on EOF/error. */
static inline long jash_readbuf_fill(jash_readbuf_t* rb) {
    long n = sys_read(rb->fd, rb->buf, JASH_READBUF_SIZE);
    rb->pos = 0;
    rb->len = (int)n;
    return n;
}

/* Read one character. Returns 1 on success, 0 on EOF, -1 on error. */
static inline int jash_readbuf_getc(jash_readbuf_t* rb, char* out) {
    if (rb->pos >= rb->len) {
        long n = jash_readbuf_fill(rb);
        if (n <= 0) return (n == 0) ? 0 : -1;
    }
    *out = rb->buf[rb->pos++];
    return 1;
}

/* Read up to `max` chars into `out`, stopping at newline or EOF.
 * Returns number of chars read (not counting the newline if stopped at one).
 * The newline is NOT consumed from the buffer (caller can call getc to skip it).
 * If a newline is encountered, `*found_newline` is set to 1. */
static inline int jash_readbuf_readline(jash_readbuf_t* rb, char* out, int max,
                                         int* found_newline) {
    int n = 0;
    *found_newline = 0;
    while (n < max) {
        if (rb->pos >= rb->len) {
            long r = jash_readbuf_fill(rb);
            if (r <= 0) break;
        }
        char c = rb->buf[rb->pos++];
        if (c == '\n') { *found_newline = 1; break; }
        out[n++] = c;
    }
    return n;
}

/* Peek at the next character WITHOUT consuming it.
 * Returns 1 if a character is available, 0 on EOF, -1 on error. */
static inline int jash_readbuf_peek(jash_readbuf_t* rb, char* out) {
    if (rb->pos >= rb->len) {
        long n = jash_readbuf_fill(rb);
        if (n <= 0) return (n == 0) ? 0 : -1;
    }
    *out = rb->buf[rb->pos];
    return 1;
}

#endif /* JASH_CORE_READBUF_H */
