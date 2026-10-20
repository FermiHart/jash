/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_MINIX_SURFACE_H
#define JASH_MINIX_SURFACE_H

#include <stddef.h>
#include <stdint.h>
#include <minix/surface.h>

struct jash_minix_surface {
    uint32_t session;
    char tag[32];
    struct surface_reader input;
    uint32_t command_index;
    int flush_fd;
    int ack_fd;
};

/* Open only the pre-registered input reader attached by jinit. */
int jash_minix_surface_open(const char *tag, struct jash_minix_surface *surface);
/* Consume, validate and ACK one tagged input line before returning it. */
int jash_minix_surface_readline(struct jash_minix_surface *surface,
    char *line, size_t cap);
int jash_minix_surface_prompt(struct jash_minix_surface *surface);
int jash_minix_surface_receipt(struct jash_minix_surface *surface, int status);
long jash_minix_surface_writer(void *ctx, const char *buf, size_t len);

#endif
