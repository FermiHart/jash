// SPDX-License-Identifier: LicenseRef-SPL-1.0
// BLAKE3 observation from one pinned regular-file descriptor.
#include "recovery_output.h"
#include <core/query_cancel.h>
#include <osif/osif_stat.h>

typedef struct {
    char digest[65];
    uint64_t bytes;
    const char* error;
    int error_number, complete, stable, cancelled;
} verify_observation_t;

static int verify_same_stat(const struct stat* before, const struct stat* after) {
    return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
        before->st_size == after->st_size && before->st_mode == after->st_mode &&
        BEAR_ST_MTIM(*before).tv_sec == BEAR_ST_MTIM(*after).tv_sec &&
        BEAR_ST_MTIM(*before).tv_nsec == BEAR_ST_MTIM(*after).tv_nsec &&
        BEAR_ST_CTIM(*before).tv_sec == BEAR_ST_CTIM(*after).tv_sec &&
        BEAR_ST_CTIM(*before).tv_nsec == BEAR_ST_CTIM(*after).tv_nsec;
}

static void verify_observe(jash_state_t* state, const char* path, verify_observation_t* result) {
    memset(result, 0, sizeof *result);
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) { result->error = "open_failed"; result->error_number = errno; return; }
    struct stat before, after;
    if (fstat(fd, &before)) { result->error = "stat_failed"; result->error_number = errno; }
    else if (!S_ISREG(before.st_mode)) { result->error = "not_regular_file"; result->error_number = EINVAL; }
    else if (before.st_size < 0) { result->error = "invalid_file_size"; result->error_number = EOVERFLOW; }
    struct blake3 hash;
    blake3_init(&hash);
    unsigned char buffer[65536];
    uint64_t remaining = result->error ? 0 : (uint64_t)before.st_size;
    while (!result->error && remaining) {
        int cancelled = jash_query_cancel_poll(state);
        if (cancelled) { result->cancelled = cancelled; result->error = "request_cancelled"; break; }
        size_t count = remaining < sizeof buffer ? (size_t)remaining : sizeof buffer;
        ssize_t got = read(fd, buffer, count);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { result->error = "read_failed"; result->error_number = errno; break; }
        if (!got) { result->error = "file_changed"; break; }
        blake3_update(&hash, buffer, (uint64_t)got);
        remaining -= (uint64_t)got; result->bytes += (uint64_t)got;
    }
    if (!result->error) {
        ssize_t extra;
        do { extra = read(fd, buffer, 1); } while (extra < 0 && errno == EINTR && !jash_query_cancel_poll(state));
        if (extra < 0) { result->error = "read_failed"; result->error_number = errno; }
        else if (extra) { result->bytes++; result->error = "file_changed"; }
        else if (fstat(fd, &after)) { result->error = "stat_failed"; result->error_number = errno; }
        else if (!verify_same_stat(&before, &after)) result->error = "file_changed";
        else result->stable = 1;
    }
    if (close(fd) && !result->error) { result->error = "close_failed"; result->error_number = errno; }
    int cancelled = jash_query_cancel_poll(state);
    if (cancelled) { result->cancelled = cancelled; result->error = "request_cancelled"; result->error_number = 0; }
    if (result->error) return;
    unsigned char bytes[32]; blake3_final(&hash, bytes, sizeof bytes);
    static const char hex[] = "0123456789abcdef";
    for (unsigned i = 0; i < sizeof bytes; i++) {
        result->digest[2 * i] = hex[bytes[i] >> 4]; result->digest[2 * i + 1] = hex[bytes[i] & 15];
    }
    result->digest[64] = 0; result->complete = 1;
}

int jash_b_verify_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0)
        return jash_discovery_error(writer, context, "verify", mode, "conflicting_formats", 2);
    int ended = 0;
    const char *path = NULL, *expected_argument = NULL;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if ((!ended && argv[i][0] == '-') || !*argv[i] || expected_argument) goto invalid;
        if (!path) path = argv[i]; else expected_argument = argv[i];
    }
    if (!path) goto invalid;
    char expected[65];
    if (expected_argument) {
        if (strlen(expected_argument) != 64) goto invalid;
        for (unsigned i = 0; i < 64; i++) {
            unsigned char c = (unsigned char)expected_argument[i];
            if (c >= 'A' && c <= 'F') c = (unsigned char)(c + ('a' - 'A'));
            if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) goto invalid;
            expected[i] = (char)c;
        }
        expected[64] = 0;
    }
    verify_observation_t observed;
    verify_observe(state, path, &observed);
    int matches = expected_argument && observed.complete && !strcmp(expected, observed.digest);
    const char* error = observed.error ? observed.error : expected_argument && !matches ? "digest_mismatch" : NULL;
    int status = observed.cancelled ? observed.cancelled : error ? 1 : 0;
    if (!mode) {
        if (!observed.complete) { jash_writef(jash_stderr_writer, NULL, "verify: %s: %s\n", path, error); return status; }
        long wrote;
        if (!expected_argument) wrote = jash_writef(writer, context, "%s  %s\n", observed.digest, path);
        else if (matches) wrote = jash_writef(writer, context, "\033[32mok\033[0m %s\n", path);
        else wrote = jash_writef(writer, context, "\033[31mDRIFT\033[0m %s\n  was: %s\n  now: %s\n", path, expected, observed.digest);
        return wrote < 0 ? 125 : status;
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "verify", mode, "allocation_failed", 125);
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("path"), jash_discovery_path(&rows, path)},
        {JDV_LITERAL("algorithm"), jash_discovery_string(&rows, "blake3")},
        {JDV_LITERAL("digest"), jash_discovery_string(&rows, observed.complete ? observed.digest : NULL)},
        {JDV_LITERAL("expected"), jash_discovery_string(&rows, expected_argument ? expected : NULL)},
        {JDV_LITERAL("matches"), expected_argument && observed.complete ? jdv_value_bool(matches) : jdv_value_nothing()},
        {JDV_LITERAL("bytes_read"), jdv_value_filesize(observed.bytes)},
        {JDV_LITERAL("read_complete"), jdv_value_bool(observed.complete)},
        {JDV_LITERAL("stable"), jdv_value_bool(observed.stable)},
        {JDV_LITERAL("error"), jash_discovery_string(&rows, error)},
        {JDV_LITERAL("errno"), jdv_value_u64((uint64_t)observed.error_number)}
    };
    (void)jash_discovery_add(&rows, fields, sizeof fields / sizeof fields[0]);
    if (mode == JASH_DISCOVERY_DATA) status = jash_discovery_emit_data_outcome(writer, context, "verify", &rows, status,
        observed.cancelled ? JDV_STREAM_CANCELLED : status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE, error);
    else status = jash_discovery_emit_schema(writer, context, "verify", mode, &rows, status, "jash.verify.v1");
    jash_discovery_rows_free(&rows); return status;
invalid:
    return jash_discovery_error(writer, context, "verify", mode, "invalid_options", 2);
}
