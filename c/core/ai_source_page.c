// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "ai_source_page.h"

#include "core/json_writer.h"
#include "module/blake3.h"
#include "osif/osif_stat.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static int admitted_name(const char* path) {
    if (!path || !*path || path[0] == '.') return 0;
    size_t n = strnlen(path, 128);
    if (!n || n >= 128) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)path[i];
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') &&
            (c < '0' || c > '9') && c != '.' && c != '_' && c != '-')
            return 0;
    }
    const char* ext = strrchr(path, '.');
    return ext && (!strcmp(ext, ".c") || !strcmp(ext, ".h") ||
                   !strcmp(ext, ".cc") || !strcmp(ext, ".cpp") ||
                   !strcmp(ext, ".cxx") || !strcmp(ext, ".hpp") ||
                   !strcmp(ext, ".hh"));
}

static int unchanged(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino &&
           a->st_size == b->st_size &&
           BEAR_ST_MTIM(*a).tv_sec == BEAR_ST_MTIM(*b).tv_sec &&
           BEAR_ST_MTIM(*a).tv_nsec == BEAR_ST_MTIM(*b).tv_nsec &&
           BEAR_ST_CTIM(*a).tv_sec == BEAR_ST_CTIM(*b).tv_sec &&
           BEAR_ST_CTIM(*a).tv_nsec == BEAR_ST_CTIM(*b).tv_nsec;
}

int jash_ai_source_page(const char* path, size_t offset, size_t limit,
                         char* out, size_t capacity) {
    if (!out || !capacity) { errno = EINVAL; return -1; }
    out[0] = '\0';
    if (!admitted_name(path) || !limit || limit > 1024) {
        errno = EINVAL; return -1;
    }
    /* A named pipe or device must not block the agent before fstat can reject
     * its type. O_NONBLOCK does not change regular-file pread semantics. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat before, after, current;
    if (fstat(fd, &before) < 0) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    if (!S_ISREG(before.st_mode) || before.st_size < 0) {
        close(fd); errno = EINVAL; return -1;
    }
    if (before.st_size > 32 * 1024 * 1024) {
        close(fd); errno = E2BIG; return -1;
    }
    if ((uint64_t)offset > (uint64_t)before.st_size) {
        close(fd); errno = ERANGE; return -1;
    }
    size_t amount = (size_t)before.st_size - offset;
    if (amount > limit) amount = limit;
    uint8_t bytes[1025];
    size_t used = 0;
    while (used < amount) {
        ssize_t got = pread(fd, bytes + used, amount - used,
                            (off_t)(offset + used));
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            int saved = got < 0 ? errno : ESTALE;
            close(fd); errno = saved; return -1;
        }
        used += (size_t)got;
    }
    errno = 0;
    if (fstat(fd, &after) < 0 || lstat(path, &current) < 0 ||
        !unchanged(&before, &after) || !unchanged(&before, &current)) {
        int saved = errno ? errno : ESTALE;
        close(fd); errno = saved; return -1;
    }
    close(fd);

    uint8_t digest[32];
    char hex[65];
    static const char table[] = "0123456789abcdef";
    blake3_hash(bytes, used, digest);
    for (size_t i = 0; i < sizeof(digest); i++) {
        hex[2u * i] = table[digest[i] >> 4];
        hex[2u * i + 1u] = table[digest[i] & 15u];
    }
    hex[64] = '\0';
    bytes[used] = '\0';

    char result[4096];
    jash_json_buffer_t buffer = {result, sizeof(result), 0};
    jash_json_writer_t writer;
    jash_jsonw_init(&writer, jash_jsonw_buffer_sink, &buffer);
    jash_jsonw_raw(&writer, "{\"schema\":\"jash.source-page.v1\",\"path\":");
    jash_jsonw_string(&writer, path);
    jash_jsonw_raw(&writer, ",\"offset\":"); jash_jsonw_u64(&writer, offset);
    jash_jsonw_raw(&writer, ",\"bytes_returned\":"); jash_jsonw_u64(&writer, used);
    jash_jsonw_raw(&writer, ",\"total_bytes\":");
    jash_jsonw_u64(&writer, (uint64_t)before.st_size);
    jash_jsonw_raw(&writer, ",\"eof\":");
    jash_jsonw_raw(&writer, offset + used == (size_t)before.st_size ? "true" : "false");
    jash_jsonw_raw(&writer, ",\"freshness\":\"best_effort\",\"page_blake3\":");
    jash_jsonw_string(&writer, hex);
    jash_jsonw_raw(&writer, ",\"text\":");
    if (!memchr(bytes, 0, used) && jash_json_utf8_valid((const char*)bytes))
        jash_jsonw_string(&writer, (const char*)bytes);
    else jash_jsonw_raw(&writer, "null");
    jash_jsonw_raw(&writer, ",\"data\":");
    jash_jsonw_bytes(&writer, bytes, used);
    jash_jsonw_raw(&writer, "}");
    if (jash_jsonw_finish(&writer) < 0) return -1;
    if (buffer.used >= capacity) { errno = ENOSPC; return -1; }
    memcpy(out, result, buffer.used + 1u);
    return 0;
}
