// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Banner records only: the real banner/formatter/version translation units,
 * with deterministic uname and writer seams. No shell, prompt, syscall, ANSI
 * projector, security or scheduling behavior is simulated here.
 *
 * Positive-short retries and failure latching are new behavior. The old void
 * banner ignored errors; this test does not claim parity for those failures.
 * Its public void API cannot expose the latched numeric error to a caller. */
#include <theme/banner.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/utsname.h>
#include "banner_parent_bytes.h"

#define REQUIRE(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: failed: %s\n", __FILE__, __LINE__, #condition); \
    exit(1); \
} } while (0)
#define OUTPUT_MAX 4096u
#define CALL_MAX 2048u
#define RECORD_MAX 192u

/* Use the name declared by this port's header (some libcs macro-map uname).
 * Temporary mixed-compiler controls may rename that same declared name. */
int uname(struct utsname *out);
static char system_name[sizeof(((struct utsname *)0)->sysname)];
static char machine_name[sizeof(((struct utsname *)0)->machine)];
static int uname_failure;
static unsigned uname_calls;

int uname(struct utsname *out) {
    uname_calls++;
    if (uname_failure) return -1;
    memset(out, 0, sizeof(*out));
    memcpy(out->sysname, system_name, sizeof(system_name));
    memcpy(out->machine, machine_name, sizeof(machine_name));
    return 0;
}

static void set_uname(unsigned mode) {
    uname_failure = mode == 1u;
    uname_calls = 0;
    memset(system_name, 0, sizeof(system_name));
    memset(machine_name, 0, sizeof(machine_name));
    if (mode == 2u) {
        memset(system_name, 'S', sizeof(system_name) - 1u);
        memset(machine_name, 'M', sizeof(machine_name) - 1u);
    } else {
        memcpy(system_name, "Beyond", 7u);
        memcpy(machine_name, "x86_64", 7u);
    }
}

struct capture {
    unsigned char bytes[OUTPUT_MAX];
    size_t starts[CALL_MAX], requested[CALL_MAX], accepted[CALL_MAX];
    size_t used, calls, accept_max, fail_at;
    size_t retry_size;
    const char *retry_pointer;
    long failure;
    int stopped, allow_zero, allow_large;
};
static struct capture current, reference;
static unsigned cases_run;

static void reset_capture(struct capture *capture) {
    memset(capture, 0, sizeof(*capture));
}

static long capture_write(void *context, const char *bytes, size_t length) {
    struct capture *capture = context;
    REQUIRE(!capture->stopped); /* no writer call after its first refusal */
    REQUIRE(capture->calls < CALL_MAX);
    REQUIRE(capture->allow_zero || length > 0);
    REQUIRE(capture->allow_large || length <= RECORD_MAX);
    REQUIRE(length <= OUTPUT_MAX - capture->used);
    if (capture->retry_size) {
        REQUIRE(length == capture->retry_size);
        REQUIRE(bytes == capture->retry_pointer);
    }
    size_t index = capture->calls++;
    capture->starts[index] = capture->used;
    capture->requested[index] = length;
    if (capture->fail_at && capture->calls == capture->fail_at) {
        capture->stopped = 1;
        if (capture->failure > 0) return (long)length + 1; /* impossible count */
        return capture->failure;
    }
    size_t accepted = length;
    if (capture->accept_max && accepted > capture->accept_max)
        accepted = capture->accept_max;
    memcpy(capture->bytes + capture->used, bytes, accepted);
    capture->used += accepted;
    capture->accepted[index] = accepted;
    capture->retry_size = length - accepted;
    capture->retry_pointer = bytes + accepted;
    return (long)accepted;
}

static size_t replace_once(unsigned char *bytes, size_t size,
                           const char *needle, const char *replacement) {
    size_t old_size = strlen(needle), new_size = strlen(replacement);
    size_t found = size, matches = 0;
    REQUIRE(old_size > 0 && old_size <= size);
    for (size_t i = 0; i <= size - old_size; i++) {
        if (!memcmp(bytes + i, needle, old_size)) { found = i; matches++; }
    }
    REQUIRE(matches == 1u);
    REQUIRE(new_size <= OUTPUT_MAX - (size - old_size));
    memmove(bytes + found + new_size, bytes + found + old_size,
            size - found - old_size);
    memcpy(bytes + found, replacement, new_size);
    return size - old_size + new_size;
}

static size_t expected_bytes(unsigned char *out, int full, long pid) {
    if (!full) {
        memcpy(out, banner_parent_short, sizeof(banner_parent_short));
        return sizeof(banner_parent_short);
    }
    memcpy(out, banner_parent_full_pid1, sizeof(banner_parent_full_pid1));
    size_t size = sizeof(banner_parent_full_pid1);
    size = replace_once(out, size, "Beyond", uname_failure ? "Unknown" : system_name);
    size = replace_once(out, size, "x86_64", uname_failure ? "" : machine_name);
    char pid_text[80];
    int written = snprintf(pid_text, sizeof(pid_text), "Pid \033[0m%ld\033[0m\n", pid);
    REQUIRE(written > 0 && (size_t)written < sizeof(pid_text));
    return replace_once(out, size, "Pid \033[0m1\033[0m\n", pid_text);
}

static void invoke(int full, struct capture *capture, long pid) {
    if (full) jash_banner_render(capture_write, capture, pid);
    else jash_banner_short(capture_write, capture);
}

/* A deliberately small SGR-only observer checks the record that contains the
 * existing Type/help marker; complete raw-byte equality is checked separately. */
static int has_help_marker(const unsigned char *bytes, size_t size) {
    char visible[RECORD_MAX + 1u];
    size_t used = 0;
    for (size_t i = 0; i < size; i++) {
        if (bytes[i] == 27u && i + 1u < size && bytes[i + 1u] == '[') {
            i += 2u;
            while (i < size && !(bytes[i] >= 0x40u && bytes[i] <= 0x7eu)) i++;
        } else {
            REQUIRE(used < RECORD_MAX);
            visible[used++] = (char)bytes[i];
        }
    }
    visible[used] = 0;
    return strstr(visible, "Type help for commands  Pid ") != NULL;
}

static void check_records(const struct capture *capture, int full) {
    size_t help_records = 0, full_spans = 0;
    for (size_t i = 0; i < capture->calls; i++) {
        size_t start = capture->starts[i], size = capture->requested[i];
        REQUIRE(size > 0 && size <= RECORD_MAX);
        REQUIRE(capture->accepted[i] == size);
        REQUIRE(i + 1u == capture->calls || size == RECORD_MAX ||
                capture->bytes[start + size - 1u] == '\n');
        if (size == RECORD_MAX) full_spans++;
        if (has_help_marker(capture->bytes + start, size)) help_records++;
    }
    REQUIRE(help_records == (full ? 1u : 0u));
    if (full && strlen(system_name) + strlen(machine_name) > 102u && !uname_failure)
        REQUIRE(full_spans > 0); /* max fields cross the bounded record ceiling */
}

#ifdef BANNER_REFERENCE_DIFFERENTIAL
void jash_banner_reference_render(jash_writer_t writer, void *context, long pid);
void jash_banner_reference_short(jash_writer_t writer, void *context);
static void check_parent(int full, long pid, const unsigned char *expected, size_t size) {
    reset_capture(&reference);
    reference.allow_zero = 1;
    reference.allow_large = 1;
    if (full) jash_banner_reference_render(capture_write, &reference, pid);
    else jash_banner_reference_short(capture_write, &reference);
    REQUIRE(reference.used == size);
    REQUIRE(!memcmp(reference.bytes, expected, size));
    REQUIRE(!memcmp(reference.bytes, current.bytes, size));
    REQUIRE(reference.calls > current.calls);
}
#else
/* Keep the reusable standalone test independent of a Git checkout at runtime. */
static void check_parent(int full, long pid, const unsigned char *expected, size_t size) {
    (void)full; (void)pid; (void)expected; (void)size; (void)reference;
}
#endif

static void successful_case(int full, unsigned mode, long pid, size_t short_limit) {
    unsigned char expected[OUTPUT_MAX];
    set_uname(mode);
    size_t size = expected_bytes(expected, full, pid);
    reset_capture(&current);
    current.accept_max = short_limit;
    invoke(full, &current, pid);
    REQUIRE(uname_calls == (full ? 2u : 0u));
    REQUIRE(current.used == size && current.retry_size == 0);
    REQUIRE(!memcmp(current.bytes, expected, size));
    if (!short_limit) {
        check_records(&current, full);
        if (mode == 0u) REQUIRE(current.calls == (full ? 13u : 1u));
        check_parent(full, pid, expected, size);
    }
    cases_run++;
}

static void refusal_case(int full, size_t fail_at, long failure, size_t short_limit) {
    unsigned char expected[OUTPUT_MAX];
    set_uname(0);
    size_t size = expected_bytes(expected, full, 1);
    reset_capture(&current);
    current.accept_max = short_limit;
    current.fail_at = fail_at;
    current.failure = failure;
    invoke(full, &current, 1);
    REQUIRE(current.stopped && current.calls == fail_at);
    REQUIRE(current.used < size);
    REQUIRE(!memcmp(current.bytes, expected, current.used));
    /* A fresh invocation owns a fresh local staging/error state. */
    reset_capture(&current);
    invoke(full, &current, 1);
    REQUIRE(!current.stopped && current.used == size);
    REQUIRE(!memcmp(current.bytes, expected, size));
    cases_run++;
}

int main(void) {
    /* LONG_MIN is intentionally absent: the unchanged formatter negates a
     * signed long long. That pre-existing minimum-value UB is not this delta. */
    const long pids[] = {0, 1, 1041, -1, -42, LONG_MAX, LONG_MIN + 1};
    const size_t shorts[] = {1u, 2u, 3u, 7u, 31u, 191u};
    const long refusals[] = {-1, -5, -85, -122, 0, 1};
    for (unsigned mode = 0; mode < 3u; mode++) {
        for (size_t i = 0; i < sizeof(pids) / sizeof(pids[0]); i++)
            successful_case(1, mode, pids[i], 0);
        successful_case(0, mode, 1, 0);
        for (size_t i = 0; i < sizeof(shorts) / sizeof(shorts[0]); i++) {
            successful_case(1, mode, 1041, shorts[i]);
            successful_case(0, mode, 1, shorts[i]);
        }
    }
    for (size_t i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        for (size_t call = 1; call <= 13u; call++) refusal_case(1, call, refusals[i], 0);
        refusal_case(0, 1, refusals[i], 0);
        refusal_case(1, 5, refusals[i], 3u);
        refusal_case(0, 5, refusals[i], 3u);
    }
    set_uname(0);
    jash_banner_render(NULL, NULL, 1);
    jash_banner_short(NULL, NULL);
    REQUIRE(uname_calls == 0u);
    cases_run++;
    printf("banner records: %u cases PASS; parent bytes, <=192-byte records, "
           "help marker, short suffix retries and fail-stop; no guest/atomicity claim\n",
           cases_run);
    return 0;
}
