/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#include "surface.h"

#include <minix_surface_api/surface.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int
parse_fd_env(const char *name, int *fd)
{
    const char *value = getenv(name);
    unsigned number = 0;

    if (!value || !*value || !fd) return -EINVAL;
    for (const char *p = value; *p; p++) {
        unsigned digit;
        if (*p < '0' || *p > '9') return -EINVAL;
        digit = (unsigned)(*p - '0');
        if (number > ((unsigned)INT_MAX - digit) / 10) return -ERANGE;
        number = number * 10 + digit;
    }
    if (number <= STDERR_FILENO || fcntl((int)number, F_GETFD) < 0)
        return -EBADF;
    *fd = (int)number;
    return 0;
}

static int
set_cloexec_pair(int first, int second)
{
    int first_flags = fcntl(first, F_GETFD);
    int second_flags = fcntl(second, F_GETFD);
    if (first_flags < 0 || second_flags < 0) return -errno;
    if (fcntl(first, F_SETFD, first_flags | FD_CLOEXEC) < 0) return -errno;
    if (fcntl(second, F_SETFD, second_flags | FD_CLOEXEC) < 0) {
        int saved = errno;
        if (fcntl(first, F_SETFD, first_flags) < 0) return -EIO;
        return -saved;
    }
    return 0;
}

static int
write_byte(int fd, char byte)
{
    for (;;) {
        ssize_t n = write(fd, &byte, 1);
        if (n == 1) return 0;
        if (n < 0 && errno == EINTR) continue;
        return n < 0 ? -errno : -EIO;
    }
}

static int
read_byte(int fd, char *byte)
{
    for (;;) {
        ssize_t n = read(fd, byte, 1);
        if (n == 1) return 0;
        if (n < 0 && errno == EINTR) continue;
        return n < 0 ? -errno : -EPIPE;
    }
}

static int
surface_flush(struct jash_minix_surface *surface)
{
    char ack;
    int rc = write_byte(surface->flush_fd, 'F');
    if (rc < 0) return rc;
    rc = read_byte(surface->ack_fd, &ack);
    if (rc < 0) return rc;
    return ack == 'A' ? 0 : -EPROTO;
}

static int
append_text(char *buffer, size_t capacity, size_t *length, const char *text)
{
    size_t n = strlen(text);
    if (*length > capacity || n >= capacity - *length) return -EOVERFLOW;
    memcpy(buffer + *length, text, n);
    *length += n;
    return 0;
}

static int
append_decimal(char *buffer, size_t capacity, size_t *length, unsigned value)
{
    char digits[10];
    size_t n = 0;
    do {
        digits[n++] = (char)('0' + value % 10);
        value /= 10;
    } while (value && n < sizeof(digits));
    if (*length > capacity || n >= capacity - *length) return -EOVERFLOW;
    while (n) buffer[(*length)++] = digits[--n];
    return 0;
}

static int
parse_session(const char *tag, uint32_t *session)
{
    static const char prefix[] = "console.";
    uint32_t value = 0;
    const char *p;

    if (!tag || !session || strncmp(tag, prefix, sizeof(prefix) - 1) != 0)
        return -EINVAL;
    p = tag + sizeof(prefix) - 1;
    if (!*p) return -EINVAL;
    for (; *p; p++) {
        uint32_t digit;
        if (*p < '0' || *p > '9') return -EINVAL;
        digit = (uint32_t)(*p - '0');
        if (value > (UINT32_MAX - digit) / 10) return -ERANGE;
        value = value * 10 + digit;
    }
    if (!value || value == SURFACE_SESSION_SYSTEM) return -EINVAL;
    *session = value;
    return 0;
}

int
jash_minix_surface_open(const char *tag, struct jash_minix_surface *surface)
{
    struct jash_minix_surface opened;
    size_t tag_len;
    int rc;

    if (!surface || !tag) return -EINVAL;
    tag_len = strlen(tag);
    if (tag_len >= sizeof(opened.tag)) return -ENAMETOOLONG;
    memset(&opened, 0, sizeof(opened));
    rc = parse_fd_env("JASH_SURFACE_FLUSH_FD", &opened.flush_fd);
    if (rc < 0) return rc;
    rc = parse_fd_env("JASH_SURFACE_ACK_FD", &opened.ack_fd);
    if (rc < 0 || opened.flush_fd == opened.ack_fd) return rc < 0 ? rc : -EINVAL;
    rc = parse_session(tag, &opened.session);
    if (rc < 0) return rc;
    memcpy(opened.tag, tag, tag_len + 1);
    opened.input.action = SURFACE_READER_STATUS;
    opened.input.role = SURFACE_READER_INPUT;
    rc = bear_surface_reader_op(opened.session, &opened.input);
    if (rc < 0) return rc;
    if (opened.input.endpoint <= 0 || !opened.input.epoch ||
        !opened.input.next_seq || opened.input.next_seq > (uint64_t)UINT32_MAX + 1)
        return -EPROTO;
    opened.input.action = SURFACE_READER_STATUS;
    rc = set_cloexec_pair(opened.flush_fd, opened.ack_fd);
    if (rc < 0) return rc;
    *surface = opened;
    return 0;
}

int
jash_minix_surface_readline(struct jash_minix_surface *surface,
    char *line, size_t cap)
{
    struct surface_take256 takes[SURFACE_IPC_MAX_REFS];
    struct surface_cell_ref refs[SURFACE_IPC_MAX_REFS];
    const size_t tag_len = surface ? strlen(surface->tag) : 0;

    if (!surface || !line || cap < 2 || !surface->session || !tag_len)
        return -EINVAL;
    for (;;) {
        struct surface_reader offered = surface->input;
        int count = bear_surface_consume(surface->session, &surface->input,
            takes, refs, 1);
        if (count < 0) return count;
        if (count == 0) {
            int rc = bear_surface_consumer_wait(surface->session, &surface->input);
            if (rc < 0) return rc;
            continue;
        }
        if (count != 1 || takes[0].hdr.cell_type != SURFACE_CELL_INPUT ||
            takes[0].hdr.session != surface->session ||
            /* Other Surface cell classes may interleave before an INPUT. */
            takes[0].hdr.seq < offered.next_seq ||
            takes[0].hdr.rflags || refs[0].cell_type != SURFACE_CELL_INPUT ||
            refs[0].session != surface->session ||
            refs[0].seq != takes[0].hdr.seq || refs[0].flags ||
            refs[0].len != takes[0].hdr.len ||
            takes[0].hdr.len <= tag_len || takes[0].data[tag_len] != ':' ||
            memcmp(takes[0].data, surface->tag, tag_len) != 0 ||
            memchr(takes[0].data + tag_len + 1, '\0',
                takes[0].hdr.len - tag_len - 1) != NULL)
            return -EPROTO;
        size_t line_len = takes[0].hdr.len - tag_len - 1;
        if (line_len >= cap) return -EOVERFLOW;
        memcpy(line, takes[0].data + tag_len + 1, line_len);
        line[line_len] = '\0';

        /* ACK only after a complete local copy and validation, before evaluation. */
        surface->input.action = SURFACE_READER_ACK;
        int rc = bear_surface_reader_op(surface->session, &surface->input);
        if (rc < 0) return rc;
        surface->input.action = SURFACE_READER_STATUS;
        return (int)line_len;
    }
}

int
jash_minix_surface_prompt(struct jash_minix_surface *surface)
{
    /* The MINIX typed console authenticates this exact owner prompt. */
    static const char prompt[] = "jash:surface$ ";
    if (!surface || !surface->session) return -EINVAL;
    int rc = surface_flush(surface);
    if (rc < 0) return rc;
    rc = bear_surface_emit_wait(SURFACE_CELL_PROMPT, prompt,
        (uint32_t)(sizeof(prompt) - 1));
    return rc < 0 ? rc : (rc == (int)(sizeof(prompt) - 1) ? rc : -EIO);
}

int
jash_minix_surface_receipt(struct jash_minix_surface *surface, int status)
{
    char receipt[128];
    size_t n = 0;
    uint32_t next;
    int rc;

    if (!surface || !surface->session || status < 0 || status > 255)
        return -EINVAL;
    if (surface->command_index == UINT32_MAX) return -EOVERFLOW;
    next = surface->command_index + 1;
    rc = append_text(receipt, sizeof(receipt), &n, "jash.status tag=");
    if (rc < 0) return rc;
    rc = append_text(receipt, sizeof(receipt), &n, surface->tag);
    if (rc < 0) return rc;
    rc = append_text(receipt, sizeof(receipt), &n, " index=");
    if (rc < 0) return rc;
    rc = append_decimal(receipt, sizeof(receipt), &n, next);
    if (rc < 0) return rc;
    rc = append_text(receipt, sizeof(receipt), &n, " status=");
    if (rc < 0) return rc;
    rc = append_decimal(receipt, sizeof(receipt), &n, (unsigned)status);
    if (rc < 0) return rc;
    rc = append_text(receipt, sizeof(receipt), &n, "\n");
    if (rc < 0) return rc;
    rc = surface_flush(surface);
    if (rc < 0) return rc;
    rc = bear_surface_emit_wait(SURFACE_CELL_KV, receipt, (uint32_t)n);
    if (rc < 0) return rc;
    if ((size_t)rc != n) return -EIO;
    surface->command_index = next;
    return 0;
}

long
jash_minix_surface_writer(void *ctx, const char *buf, size_t len)
{
    struct jash_minix_surface *surface = ctx;
    size_t offset = 0;

    if (!surface || !surface->session || (!buf && len)) return -EINVAL;
    while (offset < len) {
        ssize_t n = write(STDOUT_FILENO, buf + offset, len - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return -errno;
        if (n == 0) return -EIO;
        offset += (size_t)n;
    }
    return (long)len;
}
