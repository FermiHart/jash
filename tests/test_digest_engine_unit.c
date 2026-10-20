// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Direct project API checks: writer errors/short writes, read failure and a
// deterministic mid-read mutation and failed cache fsync. Linux link wrappers.
#define _GNU_SOURCE
#include <core/digest_engine.h>
#include <core/jash_invoice.h>
#include <builtins/builtin.h>
#include <parser/cmdsub.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct stat target;
static int inject, injected;
static int fail_fsync;
static int inject_cache_link_zero, cache_link_zero_injected;
static int invoice_calls;

/* Detect persistence requested by the builtin without writing invoice state.
 * Outer evaluator telemetry is outside the builtin's read-only contract. */
void jash_invoice_account_command(const char* command, int native) {
    (void)command; (void)native;
    invoice_calls++;
}

void jash_invoice_add_saved(jash_invoice_category_t category, uint64_t tokens) {
    (void)category; (void)tokens;
    invoice_calls++;
}

/* The harness exercises the argv builtin only; the string tokenizer's
 * unshield step is stubbed to keep the link self-contained. */
void jash_quote_unshield(char* s) { (void)s; }

ssize_t __real_read(int fd, void* buf, size_t n);
ssize_t __wrap_read(int fd, void* buf, size_t n);
int __real_fsync(int fd);
int __wrap_fsync(int fd);
int __real_fstatat(int dirfd, const char* path, struct stat* st, int flags);
int __wrap_fstatat(int dirfd, const char* path, struct stat* st, int flags);

int __wrap_fstatat(int dirfd, const char* path, struct stat* st, int flags) {
    int rc = __real_fstatat(dirfd, path, st, flags);
    /* A rename replace can expose the old cache inode for one metadata lookup
     * with nlink=0 while another digest publishes the new inode. */
    if (inject_cache_link_zero && !cache_link_zero_injected && rc == 0 &&
        path && strlen(path) == 67 && !strcmp(path + 64, ".md") &&
        S_ISREG(st->st_mode)) {
        st->st_nlink = 0;
        cache_link_zero_injected = 1;
    }
    return rc;
}

int __wrap_fsync(int fd) {
    if (fail_fsync) { injected = 1; errno = EIO; return -1; }
    return __real_fsync(fd);
}

ssize_t __wrap_read(int fd, void* buf, size_t n) {
    struct stat st;
    /* Workers read in parallel, so the one-shot injection must be claimed
     * atomically, and only when the fd matches the chosen target file. */
    if (inject && !__atomic_load_n(&injected, __ATOMIC_RELAXED) &&
        !fstat(fd, &st) && st.st_dev == target.st_dev && st.st_ino == target.st_ino &&
        __sync_bool_compare_and_swap(&injected, 0, 1)) {
        if (inject == 1) { errno = EIO; return -1; }
        ssize_t result = __real_read(fd, buf, n);
        /* Deterministically change size after bytes were obtained but before the
         * engine's post-read fstat; no timing sleeps or concurrent-race lottery. */
        const char* path = getenv("DIGEST_UNIT_TARGET");
        int out = open(path, O_WRONLY | O_APPEND);
        if (out < 0 || write(out, "changed\n", 8) != 8 || close(out)) abort();
        return result;
    }
    return __real_read(fd, buf, n);
}

typedef struct {
    char text[65536];
    size_t len;
    int mode;
} sink_t;

static long sink(void* ctx, const char* data, size_t n) {
    sink_t* s = ctx;
    if (s->mode == 1) return -1;
    if (s->mode == 2) return 0;
    if (s->mode == 3 && n > 3) n = 3;
    if (n >= sizeof(s->text) - s->len) return -1;
    memcpy(s->text + s->len, data, n);
    s->len += n;
    s->text[s->len] = '\0';
    return (long)n;
}

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    digest_cfg_t cfg = {.level = DIGEST_BRIEF, .root_dir = argv[2], .no_cache = 1};
    sink_t actual = {0}, expected = {0};
    if (!strcmp(argv[1], "builtin-readonly")) {
        char* av[] = {"--no-cache", "--brief", argv[2]};
        if (jash_b_digest_argv(NULL, sink, &actual, 3, av) || invoice_calls || !actual.len) return 1;
        av[0] = "--fingerprint";
        if (jash_b_digest_argv(NULL, sink, &expected, 3, av) || invoice_calls || expected.len != 65) return 1;
        expected.text[64] = '\0';
        return strstr(actual.text, expected.text) ? 0 : 1;
    }
    if (!strcmp(argv[1], "negative")) actual.mode = 1;
    else if (!strcmp(argv[1], "zero")) actual.mode = 2;
    else if (!strcmp(argv[1], "short")) actual.mode = 3;
    else if (!strcmp(argv[1], "read-error")) inject = 1;
    else if (!strcmp(argv[1], "mutate")) inject = 2;
    else if (!strcmp(argv[1], "publish-error")) { fail_fsync = 1; cfg.no_cache = 0; cfg.refresh = 1; }
    else if (!strcmp(argv[1], "cache-link-zero")) { inject_cache_link_zero = 1; cfg.no_cache = 0; cfg.refresh = 1; }
    else if (!strcmp(argv[1], "readonly")) { /* capture-only call below */ }
    else return 2;

    if (inject) {
        cfg.no_cache = 0; /* a failed normal scan must not create/publish a cache */
        const char* path = getenv("DIGEST_UNIT_TARGET");
        if (!path || stat(path, &target)) return 2;
    }
    int rc = digest_run(&cfg, sink, &actual);
    if (actual.mode == 1 || actual.mode == 2 || inject || fail_fsync) {
        if (!rc || actual.len || ((inject || fail_fsync) && !injected)) return 1;
    } else {
        if (rc || !strstr(actual.text, "# Project Digest")) return 1;
        if (digest_run(&cfg, sink, &expected) || actual.len != expected.len ||
            memcmp(actual.text, expected.text, actual.len)) return 1;
        cfg.fingerprint = 1;
        expected.len = 0;
        if (digest_run(&cfg, sink, &expected) || expected.len != 65 || expected.text[64] != '\n') return 1;
        expected.text[64] = '\0';
        if (!strstr(actual.text, expected.text)) return 1;
    }
    if (inject_cache_link_zero && !cache_link_zero_injected) return 1;
    return 0;
}
