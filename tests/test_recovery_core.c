// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include <unistd.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <core/recovery.h>

static int fault_sync, fault_publish, fault_store, fault_origin;
static int checked_fsync(int fd) {
    struct stat st;
    if (fault_store && !fstat(fd, &st) && S_ISREG(st.st_mode)) { errno = ENOSPC; return -1; }
    if (fault_sync && !fstat(fd, &st) && S_ISDIR(st.st_mode)) {
        char path[64], name[4096];
        snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
        ssize_t n = readlink(path, name, sizeof(name) - 1);
        if (n > 0) { name[n] = 0; if (strstr(name, ".jash-undo-")) { errno = EIO; return -1; } }
    }
    return fsync(fd);
}

int jash_osif_renameat2(int from, const char* src, int to, const char* dst, unsigned flags) {
    if (fault_origin && !strcmp(src, "original") && !strcmp(dst, "origin")) {
        int fd = openat(to, dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, "raced", 5) == 5 && !close(fd));
        fault_origin = 0;
    }
    if (fault_publish && !strcmp(src, "restored")) {
        int fd = openat(to, dst, O_WRONLY | O_CREAT | O_EXCL, 0600);
        assert(fd >= 0 && write(fd, "raced", 5) == 5 && !close(fd));
        fault_publish = 0;
    }
    return (int)syscall(SYS_renameat2, from, src, to, dst, flags);
}

void jash_budget_unwrap(jash_writer_t* w, void** ctx) { (void)w; (void)ctx; }
void jash_redir_stream_writer(jash_writer_t w, void* ctx) { (void)w; (void)ctx; }
void jash_timeline_consume_id(uint64_t id) { (void)id; }

#define fsync checked_fsync
#include "../c/core/recovery.c"
#undef fsync

static char output[16384];
static size_t output_size;
static long sink(void* ctx, const char* text, size_t n) {
    (void)ctx;
    assert(output_size + n < sizeof(output));
    memcpy(output + output_size, text, n); output_size += n; output[output_size] = 0;
    return (long)n;
}
static void content(const char* path, const char* value) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    assert(fd >= 0 && write(fd, value, strlen(value)) == (ssize_t)strlen(value) && !close(fd));
}
static void assert_content(const char* path, const char* value) {
    char buf[32]; int fd = open(path, O_RDONLY);
    assert(fd >= 0); ssize_t n = read(fd, buf, sizeof(buf)); close(fd);
    assert(n == (ssize_t)strlen(value) && !memcmp(buf, value, (size_t)n));
}
static int apply(const jash_recovery_ref_t* ref) {
    char args[128]; snprintf(args, sizeof(args), "--json --ref %s", ref->token);
    output_size = 0;
    return jash_recovery_command(sink, NULL, args);
}

int main(int argc, char** argv) {
    assert(argc == 2 && !chdir(argv[1]) && !setenv("HOME", argv[1], 1));
    content("target", "new");
    unsigned char digest[32]; char hex[65], backup[80];
    jash_sha256("old", 3, digest); jash_sha256_hex(digest, hex);
    snprintf(backup, sizeof(backup), "bk_%s", hex); content(backup, "old");
    jash_recovery_ref_t ref;
    assert(!jash_recovery_capture(UINT64_C(0xfedcba9876543210), JASH_UNDO_FILE_OVERWRITE, "target", backup, &ref));
    char active_token[33]; strcpy(active_token, ref.token);
    fault_publish = 1;
    assert(apply(&ref) == 1 && strstr(output, "\"status\":\"partial\"") && strstr(output, "publish_conflict"));
    assert_content("target", "raced");
    char path[128]; snprintf(path, sizeof(path), ".jash-undo-%s/original", ref.token); assert_content(path, "new");
    snprintf(path, sizeof(path), ".jash-undo-%s/restored", ref.token); assert_content(path, "old");
    assert(apply(&ref) == 1 && strstr(output, "interrupted_recovery") && strstr(output, ".jash-undo-"));

    content("created", "data");
    assert(!jash_recovery_capture(42, JASH_UNDO_FILE_CREATE, "created", NULL, &ref));
    fault_sync = 1;
    assert(apply(&ref) == 1 && strstr(output, "partial") && strstr(output, "restore_sync"));
    assert(access("created", F_OK) && errno == ENOENT);
    fault_sync = 0;
    assert(apply(&ref) == 1 && strstr(output, "interrupted_recovery"));

    /* Missing/corrupted backup must never be exported as usable recovery. */
    content(backup, "BAD");
    assert(jash_recovery_capture(43, JASH_UNDO_FILE_OVERWRITE, "target", backup, &ref) != 0 && !ref.token[0]);
    assert(jash_recovery_capture(44, JASH_UNDO_FILE_OVERWRITE, "target", "absent", &ref) != 0 && !ref.token[0]);
    fault_store = 1;
    assert(jash_recovery_capture(45, JASH_UNDO_FILE_CREATE, "target", NULL, &ref) != 0 && !ref.token[0]);
    fault_store = 0;
    content("moved", "source-data");
    assert(!jash_recovery_capture(47, JASH_UNDO_MOVE, "origin", "moved", &ref));
    fault_origin = 1;
    assert(apply(&ref) == 1 && strstr(output, "publish_conflict"));
    assert_content("origin", "raced"); assert_content("moved", "source-data");
    content("gc-target", "data");
    assert(!jash_recovery_capture(46, JASH_UNDO_FILE_CREATE, "gc-target", NULL, &ref));
    assert(!apply(&ref));
    char args[128];
    snprintf(args, sizeof(args), "--json --pin --ref %s", ref.token);
    output_size = 0;
    assert(!jash_recovery_command(sink, NULL, args) && strstr(output, "\"pinned\":true"));
    output_size = 0;
    assert(!jash_recovery_command(sink, NULL, "--json --gc") && !strstr(output, "\"action\":\"removed\""));
    snprintf(args, sizeof(args), "--json --unpin --ref %s", ref.token);
    output_size = 0; assert(!jash_recovery_command(sink, NULL, args));
    output_size = 0;
    assert(!jash_recovery_command(sink, NULL, "--json --gc --dry-run") && strstr(output, "would_remove"));
    output_size = 0;
    assert(!jash_recovery_command(sink, NULL, "--json --gc") && strstr(output, "\"action\":\"removed\""));
    snprintf(path, sizeof(path), ".jash/recovery-v1/%s", ref.token);
    assert(access(path, F_OK) && errno == ENOENT);
    jash_recovery_pins_t* pins = jash_recovery_pins_load();
    assert(jash_recovery_pins_complete(pins));
    assert(jash_recovery_pins_contains(pins, "target"));
    assert(!jash_recovery_pins_contains(pins, "unrelated"));
    jash_recovery_pins_free(pins);
    snprintf(path, sizeof(path), ".jash/recovery-v1/%s", active_token);
    int corrupted = open(path, O_WRONLY); assert(corrupted >= 0);
    assert(write(corrupted, "X", 1) == 1); close(corrupted);
    pins = jash_recovery_pins_load();
    assert(!jash_recovery_pins_complete(pins));
    assert(jash_recovery_pins_contains(pins, "unrelated"));
    jash_recovery_pins_free(pins);
    return 0;
}
