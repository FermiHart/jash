// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Binary integrity observation and human-approved baseline publication.
#include "builtin.h"
#include "semantic_builtin.h"
#include <module/jash_crypto.h>
#include <osif/osif_stat.h>
#include <sys/syscalls.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

typedef struct {
    char binary[PATH_MAX], baseline[PATH_MAX];
    char current[JASH_SHA256_HEX_LEN], expected[JASH_SHA256_HEX_LEN];
    const char* error;
    const char* baseline_error;
    int error_number, available, hashed, matched, published, durable, directory_created;
    uint64_t binary_bytes;
} selfcheck_report_t;

static int selfcheck_paths(selfcheck_report_t* report) {
    const char* home = getenv("HOME");
    if (!home || !*home) home = jash_tmpdir();
    int n = snprintf(report->baseline, sizeof report->baseline, "%s/.jash/binary_hash", home);
    if (n < 0 || (size_t)n >= sizeof report->baseline) { errno = ENAMETOOLONG; return -1; }
#ifdef __APPLE__
    uint32_t size = sizeof report->binary;
    if (_NSGetExecutablePath(report->binary, &size)) { errno = ENAMETOOLONG; return -1; }
#else
    ssize_t got = readlink("/proc/self/exe", report->binary, sizeof report->binary - 1);
    if (got <= 0) return -1;
    if ((size_t)got >= sizeof report->binary - 1) { errno = ENAMETOOLONG; return -1; }
    report->binary[got] = 0;
#endif
    return 0;
}

static int selfcheck_same(const struct stat* before, const struct stat* after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
        before->st_size == after->st_size &&
        BEAR_ST_MTIM(*before).tv_sec == BEAR_ST_MTIM(*after).tv_sec &&
        BEAR_ST_MTIM(*before).tv_nsec == BEAR_ST_MTIM(*after).tv_nsec &&
        BEAR_ST_CTIM(*before).tv_sec == BEAR_ST_CTIM(*after).tv_sec &&
        BEAR_ST_CTIM(*before).tv_nsec == BEAR_ST_CTIM(*after).tv_nsec;
}

static int selfcheck_hash(selfcheck_report_t* report) {
#ifdef __linux__
    const char* path = "/proc/self/exe";
#else
    const char* path = report->binary;
#endif
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat before, after; int error = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode) || before.st_size < 0) error = EINVAL;
    else if ((uint64_t)before.st_size > 1024u * 1024u * 1024u) error = E2BIG;
    jash_sha256_ctx_t hash; jash_sha256_init(&hash);
    unsigned char buffer[65536]; uint64_t bytes = 0;
    while (!error) {
        ssize_t got = read(fd, buffer, sizeof buffer);
        if (got > 0) {
            bytes += (uint64_t)got;
            if (bytes > (uint64_t)before.st_size) { error = ESTALE; break; }
            jash_sha256_update(&hash, buffer, (size_t)got);
        } else if (!got) break;
        else if (errno != EINTR) error = errno;
    }
    if (!error && fstat(fd, &after)) error = errno;
    if (!error && (!selfcheck_same(&before, &after) || bytes != (uint64_t)before.st_size)) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (error) { errno = error; return -1; }
    unsigned char digest[JASH_SHA256_DIGEST_LEN];
    jash_sha256_final(&hash, digest); jash_sha256_hex(digest, report->current);
    report->binary_bytes = bytes; report->hashed = 1; return 0;
}

static int selfcheck_load(selfcheck_report_t* report) {
    int fd = open(report->baseline, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 1 : -1;
    struct stat before, after; int error = 0; unsigned char data[67]; size_t used = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode) || before.st_size < 64 || before.st_size > 66) error = EINVAL;
    while (!error && used < sizeof data) {
        ssize_t got = read(fd, data + used, sizeof data - used);
        if (got > 0) used += (size_t)got;
        else if (!got) break;
        else if (errno != EINTR) error = errno;
    }
    if (!error && fstat(fd, &after)) error = errno;
    if (!error && (!selfcheck_same(&before, &after) || used != (size_t)before.st_size)) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (!error && !(used == 64 || (used == 65 && data[64] == '\n') ||
                   (used == 66 && data[64] == '\r' && data[65] == '\n'))) error = EINVAL;
    for (size_t i = 0; i < 64 && !error; i++)
        if (!((data[i] >= '0' && data[i] <= '9') || (data[i] >= 'a' && data[i] <= 'f'))) error = EINVAL;
    if (error) { errno = error; return -1; }
    memcpy(report->expected, data, 64); report->expected[64] = 0;
    report->available = 1; report->matched = !strcmp(report->current, report->expected); return 0;
}

static int selfcheck_store(selfcheck_report_t* report) {
    char directory[PATH_MAX]; memcpy(directory, report->baseline, strlen(report->baseline) + 1);
    char* slash = strrchr(directory, '/'); if (!slash) { errno = EINVAL; return -1; } *slash = 0;
    if (!mkdir(directory, 0700)) report->directory_created = 1;
    else if (errno != EEXIST) return -1;
    int dir = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) return -1;
    char temporary[96]; int fd = -1, error = 0;
    for (unsigned attempt = 0; attempt < 32; attempt++) {
        snprintf(temporary, sizeof temporary, ".binary_hash.%ld.%u.tmp", (long)getpid(), attempt);
        fd = openat(dir, temporary, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd >= 0 || errno != EEXIST) break;
    }
    if (fd < 0) error = errno;
    else {
        char data[65]; memcpy(data, report->current, 64); data[64] = '\n'; size_t used = 0;
        while (!error && used < sizeof data) {
            ssize_t got = write(fd, data + used, sizeof data - used);
            if (got > 0) used += (size_t)got;
            else if (got < 0 && errno == EINTR) continue;
            else error = got < 0 ? errno : EIO;
        }
        if (!error && fsync(fd)) error = errno;
        if (close(fd) && !error) error = errno;
        if (!error && renameat(dir, temporary, dir, "binary_hash")) error = errno;
        else if (!error) {
            report->published = 1;
            if (fsync(dir)) error = errno;
            else report->durable = 1;
        }
        if (!report->published) (void)unlinkat(dir, temporary, 0);
    }
    if (close(dir) && !error) error = errno;
    if (error) { errno = error; return -1; }
    return 0;
}

static const char* selfcheck_permission(void) {
    if (!isatty(STDIN_FILENO)) return "interactive_terminal_required";
    const char* agent = getenv("JASH_AGENT");
    if (agent && (agent[0] == '1' || agent[0] == 'y' || agent[0] == 'Y')) return "agent_baseline_approval_forbidden";
    return NULL;
}

static int selfcheck_observe(selfcheck_report_t* report, int storing) {
    if (selfcheck_paths(report)) report->error = "path_unavailable";
    else if (selfcheck_hash(report)) report->error = "binary_hash_failed";
    else if (selfcheck_load(report) < 0) {
        report->baseline_error = "baseline_invalid_or_unreadable";
        if (!storing) report->error = report->baseline_error;
    }
    if (report->error) { report->error_number = errno; return 1; }
    return !storing && report->available && !report->matched ? 1 : 0;
}

static int selfcheck_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "verify";
    int storing = !strcmp(operation, "store") || !strcmp(operation, "force");
    if (argc > 1 || (!storing && strcmp(operation, "verify"))) return semantic_error(call, "invalid_arguments", 2);
    const char* denied = storing ? selfcheck_permission() : NULL;
    if (denied) return semantic_error(call, denied, 1);
    selfcheck_report_t report = {0}; int status = selfcheck_observe(&report, storing);
    if (storing && !status) {
        int admission = semantic_admit(call); if (admission) return admission;
        if (selfcheck_store(&report)) { report.error = "baseline_publish_failed"; report.error_number = errno; status = 1; }
    }
    semantic_value_t* values = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.selfcheck.v1")),
        semantic_field("operation", semantic_symbol(values, operation)),
        semantic_field("algorithm", semantic_symbol(values, "sha256")),
        semantic_field("binary", report.binary[0] ? semantic_path(values, report.binary) : jdv_value_nothing()),
        semantic_field("binary_size", report.hashed ? jdv_value_filesize(report.binary_bytes) : jdv_value_nothing()),
        semantic_field("current_hash", report.hashed ? semantic_text(values, report.current) : jdv_value_nothing()),
        semantic_field("baseline", report.baseline[0] ? semantic_path(values, report.baseline) : jdv_value_nothing()),
        semantic_field("baseline_available_before", jdv_value_bool(report.available)),
        semantic_field("expected_hash_before", report.available ? semantic_text(values, report.expected) : jdv_value_nothing()),
        semantic_field("verified", report.available && report.hashed && !report.error ? jdv_value_bool(report.matched) : jdv_value_nothing()),
        semantic_field("baseline_published", jdv_value_bool(report.published)),
        semantic_field("durable", storing ? jdv_value_bool(report.durable) : jdv_value_nothing()),
        semantic_field("directory_created", jdv_value_bool(report.directory_created)),
        semantic_field("baseline_error", semantic_symbol(values, report.baseline_error)),
        semantic_field("complete", jdv_value_bool(!report.error)),
        semantic_field("error", semantic_symbol(values, report.error)),
        semantic_field("errno", report.error_number ? jdv_value_i64(report.error_number) : jdv_value_nothing()),
        semantic_field("exit_code", jdv_value_i64(status))
    };
    return semantic_finish(call, semantic_record(values, fields, sizeof fields / sizeof fields[0]), status);
}

/* Startup checking remains disabled; queries never create or bless a baseline. */
int jash_selfcheck_auto(void) { return 0; }

int jash_b_selfcheck(jash_state_t* state, jash_writer_t writer, void* context, const char* args) {
    int structured = semantic_dispatch(state, writer, context, "selfcheck", "jash.selfcheck.v1", args, selfcheck_provider);
    if (structured >= 0) return structured;
    jash_argv_t parsed = {0};
    if (jash_argv_build(args ? args : "", &parsed) < 0) return 2;
    int storing = parsed.argc == 1 && (!strcmp(parsed.argv[0], "store") || !strcmp(parsed.argv[0], "force"));
    if (parsed.argc > 1 || (parsed.argc && !storing && strcmp(parsed.argv[0], "verify"))) {
        jash_argv_free(&parsed); jash_writes(writer, context, "Usage: selfcheck [--json|--data] [verify|store|force]\n"); return 2;
    }
    jash_argv_free(&parsed);
    const char* denied = storing ? selfcheck_permission() : NULL;
    if (denied) { jash_writef(writer, context, "selfcheck: %s\n", denied); return 1; }
    selfcheck_report_t report = {0}; int status = selfcheck_observe(&report, storing);
    if (storing && !status && selfcheck_store(&report)) { report.error = "baseline_publish_failed"; status = 1; }
    if (report.error) jash_writef(writer, context, "selfcheck: %s\n", report.error);
    else {
        jash_writef(writer, context, "Binary: %s\nCurrent: %s\n", report.binary, report.current);
        if (storing) jash_writes(writer, context, "Hash stored as baseline\n");
        else if (!report.available) jash_writes(writer, context, "No baseline hash. Run `selfcheck store` from an interactive terminal.\n");
        else jash_writes(writer, context, report.matched ? "Binary integrity verified\n" : "Binary differs from stored baseline\n");
    }
    return status;
}
SEMANTIC_ARGV_WRAPPER(selfcheck, "selfcheck", "jash.selfcheck.v1", selfcheck_provider)
