// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "ai_patch.h"

#include <core/timeline.h>
#include <module/blake3.h>
#include <osif/osif_stat.h>

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define AI_PATCH_LIMIT 4096u
#define AI_HUNK_LIMIT (256u * 1024u)

static int admitted(const char* name) {
    if (!name || !*name || *name == '.') return 0;
    size_t n = strnlen(name, 128);
    if (n == 0 || n >= 128) return 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)name[i];
        if ((c < 'a' || c > 'z') && (c < 'A' || c > 'Z') &&
            (c < '0' || c > '9') && c != '.' && c != '_' && c != '-') return 0;
    }
    const char* ext = strrchr(name, '.');
    return ext && (!strcmp(ext, ".c") || !strcmp(ext, ".h") ||
                   !strcmp(ext, ".cc") || !strcmp(ext, ".cpp") ||
                   !strcmp(ext, ".cxx") || !strcmp(ext, ".hpp") ||
                   !strcmp(ext, ".hh"));
}

static void digest_hex(const void* bytes, size_t n, char out[65]) {
    uint8_t digest[32];
    static const char hex[] = "0123456789abcdef";
    blake3_hash(bytes, n, digest);
    for (size_t i = 0; i < sizeof digest; i++) {
        out[2u * i] = hex[digest[i] >> 4];
        out[2u * i + 1u] = hex[digest[i] & 15u];
    }
    out[64] = '\0';
}

static int same_file(const struct stat* a, const struct stat* b) {
    return S_ISREG(b->st_mode) && a->st_dev == b->st_dev &&
           a->st_ino == b->st_ino && a->st_size == b->st_size &&
           BEAR_ST_MTIM(*a).tv_sec == BEAR_ST_MTIM(*b).tv_sec &&
           BEAR_ST_MTIM(*a).tv_nsec == BEAR_ST_MTIM(*b).tv_nsec &&
           BEAR_ST_CTIM(*a).tv_sec == BEAR_ST_CTIM(*b).tv_sec &&
           BEAR_ST_CTIM(*a).tv_nsec == BEAR_ST_CTIM(*b).tv_nsec;
}

static int snapshot(int fd, const char* path, struct stat* before,
                    char hash[65]) {
    if (fstat(fd, before) < 0) return -1;
    if (!S_ISREG(before->st_mode) || before->st_size < 0 ||
        before->st_size > (off_t)AI_PATCH_LIMIT || before->st_nlink != 1 ||
        (before->st_mode & 07000)) { errno = EINVAL; return -1; }
    uint8_t bytes[AI_PATCH_LIMIT];
    size_t amount = (size_t)before->st_size;
    for (size_t offset = 0; offset < amount;) {
        ssize_t n = pread(fd, bytes + offset, amount - offset, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n == 0) errno = ESTALE; return -1; }
        offset += (size_t)n;
    }
    struct stat current, after;
    if (fstat(fd, &after) < 0 || lstat(path, &current) < 0) return -1;
    if (!same_file(before, &after) || !same_file(before, &current)) {
        errno = ESTALE; return -1;
    }
    digest_hex(bytes, amount, hash);
    return 0;
}

static int snapshot_large(int fd, const char* path, struct stat* before,
                          char hash[65], unsigned char** contents) {
    *contents = NULL;
    if (fstat(fd, before) < 0) return -1;
    if (!S_ISREG(before->st_mode) || before->st_size < 0 ||
        before->st_size > (off_t)AI_HUNK_LIMIT || before->st_nlink != 1 ||
        (before->st_mode & 07000)) { errno = EINVAL; return -1; }
    size_t amount = (size_t)before->st_size;
    unsigned char* bytes = malloc(amount ? amount : 1u);
    if (!bytes) return -1;
    for (size_t offset = 0; offset < amount;) {
        ssize_t n = pread(fd, bytes + offset, amount - offset, (off_t)offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) {
            int saved = n < 0 ? errno : ESTALE;
            free(bytes); errno = saved; return -1;
        }
        offset += (size_t)n;
    }
    struct stat current, after;
    if (fstat(fd, &after) < 0 || lstat(path, &current) < 0 ||
        !same_file(before, &after) || !same_file(before, &current)) {
        int saved = errno ? errno : ESTALE;
        free(bytes); errno = saved; return -1;
    }
    digest_hex(bytes, amount, hash);
    *contents = bytes;
    return 0;
}

int jash_ai_source_digest(const char* path, char out[65]) {
    if (!out || !admitted(path)) { errno = EINVAL; return -1; }
    out[0] = '\0';
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat st;
    unsigned char* contents = NULL;
    int rc = snapshot_large(fd, path, &st, out, &contents);
    int saved = errno;
    free(contents); close(fd);
    if (rc < 0) { out[0] = '\0'; errno = saved; }
    return rc;
}

static int valid_digest(const char* digest) {
    if (!digest || strnlen(digest, 65) != 64) return 0;
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f'))) return 0;
    return 1;
}

static int patch_commit(const char* path, const char* before_blake3,
                        const struct stat* original, const char* text,
                        size_t text_length, jash_ai_patch_result_t* result) {
    char backup[512] = "";
    if (jash_timeline_backup_file(path, backup, sizeof backup) != 0 || !*backup) {
        errno = EIO; return -1;
    }
    int backup_fd = open(backup, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    struct stat backup_stat;
    char backup_hash[65];
    unsigned char* backed = NULL;
    int backup_ok = backup_fd >= 0 &&
        snapshot_large(backup_fd, backup, &backup_stat, backup_hash, &backed) == 0 &&
        !strcmp(backup_hash, before_blake3);
    free(backed);
    if (backup_fd >= 0) close(backup_fd);
    if (!backup_ok) { errno = ESTALE; return -1; }
    /* Backups are deduplicated across the timeline: NEVER unlink on failure. */
    char name[] = ".jash-ai-edit-XXXXXX";
    int temp_fd = mkstemp(name);
    if (temp_fd < 0) return -1;
    int ok = 1;
    for (size_t offset = 0; offset < text_length;) {
        ssize_t n = write(temp_fd, text + offset, text_length - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        offset += (size_t)n;
    }
    if (ok && fchmod(temp_fd, original->st_mode & 0777) != 0) ok = 0;
    if (ok && fsync(temp_fd) != 0) ok = 0;
    struct stat temp_stat, temp_at_path, before_rename;
    if (ok && (fstat(temp_fd, &temp_stat) != 0 ||
               lstat(name, &temp_at_path) != 0 ||
               temp_stat.st_dev != temp_at_path.st_dev ||
               temp_stat.st_ino != temp_at_path.st_ino ||
               !S_ISREG(temp_at_path.st_mode))) ok = 0;
    if (ok && (lstat(path, &before_rename) != 0 ||
               !same_file(original, &before_rename))) {
        errno = ESTALE; ok = 0;
    }
    if (ok && rename(name, path) != 0) ok = 0;
    int saved = errno;
    close(temp_fd);
    if (!ok) {
        unlink(name); errno = saved ? saved : EIO; return -1;
    }
    result->applied = 1;
    digest_hex(text, text_length, result->after_blake3);
    if (jash_timeline_record_ref(JASH_UNDO_FILE_OVERWRITE, "ai structured edit",
                                 path, backup, &result->recovery) != 0) {
        errno = EIO; return -1;
    }
    return 0;
}

int jash_ai_patch_replace(const char* path, const char* before_blake3,
                          const char* text, size_t text_length,
                          jash_ai_patch_result_t* result) {
    if (result) memset(result, 0, sizeof *result);
    if (!result || !admitted(path) || !text || !valid_digest(before_blake3) ||
        text_length > AI_PATCH_LIMIT ||
        memchr(text, 0, text_length) ||
        (getenv("JASH_TIMELINE") && !strcmp(getenv("JASH_TIMELINE"), "0"))) {
        errno = EINVAL; return -1;
    }
    char canonical[512];
    if (jash_recovery_canonical(path, canonical, sizeof canonical) != 0) {
        errno = ENAMETOOLONG; return -1;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat original;
    if (snapshot(fd, path, &original, result->before_blake3) != 0) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    if (strcmp(result->before_blake3, before_blake3)) {
        close(fd); errno = ESTALE; return -1;
    }
    int rc = patch_commit(path, before_blake3, &original,
                          text, text_length, result);
    int saved = errno;
    close(fd); errno = saved;
    return rc;
}

int jash_ai_patch_hunk(const char* path, const char* before_blake3,
                       const char* old_text, size_t old_length,
                       const char* new_text, size_t new_length,
                       jash_ai_patch_result_t* result) {
    if (result) memset(result, 0, sizeof *result);
    if (!result || !admitted(path) || !valid_digest(before_blake3) ||
        !old_text || !new_text || !old_length || old_length > AI_PATCH_LIMIT ||
        new_length > AI_PATCH_LIMIT ||
        memchr(old_text, 0, old_length) || memchr(new_text, 0, new_length) ||
        (getenv("JASH_TIMELINE") && !strcmp(getenv("JASH_TIMELINE"), "0"))) {
        errno = EINVAL; return -1;
    }
    char canonical[512];
    if (jash_recovery_canonical(path, canonical, sizeof canonical) != 0) {
        errno = ENAMETOOLONG; return -1;
    }
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    struct stat original;
    unsigned char* bytes = NULL;
    if (snapshot_large(fd, path, &original, result->before_blake3, &bytes) != 0) {
        int saved = errno; close(fd); errno = saved; return -1;
    }
    if (strcmp(before_blake3, result->before_blake3)) {
        free(bytes); close(fd); errno = ESTALE; return -1;
    }
    size_t original_length = (size_t)original.st_size;
    unsigned char* match = memmem(bytes, original_length, old_text, old_length);
    if (!match || memmem(match + 1,
                         original_length - (size_t)(match - bytes) - 1,
                         old_text, old_length)) {
        free(bytes); close(fd); errno = EINVAL; return -1;
    }
    size_t prefix = (size_t)(match - bytes);
    size_t length = original_length - old_length + new_length;
    if (length > AI_HUNK_LIMIT) {
        free(bytes); close(fd); errno = E2BIG; return -1;
    }
    char* updated = malloc(length ? length : 1u);
    if (!updated) { free(bytes); close(fd); return -1; }
    memcpy(updated, bytes, prefix);
    memcpy(updated + prefix, new_text, new_length);
    memcpy(updated + prefix + new_length, match + old_length,
           original_length - prefix - old_length);
    int rc = patch_commit(path, before_blake3, &original,
                          updated, length, result);
    int saved = errno;
    free(updated); free(bytes); close(fd); errno = saved;
    return rc;
}
