// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Isolated tests of the current read.c, using real regular-file offsets.
 * Only env/writers are stubs; fault wrappers otherwise call libc. */
#include <sys/syscalls.h> /* Before macros: preserve libc/sys_* declarations. */
#include <builtins/builtin.h>
#include <core/env.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>

static const char* case_name = "setup";
#define CHECK(condition) do { \
    if (!(condition)) { \
        fprintf(stderr, "FAIL: %s:%d: %s: %s\n", \
                __FILE__, __LINE__, case_name, #condition); \
        exit(EXIT_FAILURE); \
    } \
} while (0)

static struct {
    int fd;
    unsigned reads, block_reads, byte_reads, eof_reads, read_interrupts;
    size_t bytes_read, read_limit;
    unsigned eintr_read_first, eintr_read_count;
    unsigned stats, probes, seekbacks, seek_interrupts;
    int stat_error, probe_error, seekback_error;
    unsigned eintr_seek_count;
    off_t last_seekback;
    unsigned reallocs, fail_realloc, allocations, heap_frees;
    void* heap;
} fault;

static long injected_read(int fd, void* buf, unsigned long count) {
    CHECK(fd == fault.fd);
    CHECK(count > 0);
    CHECK(++fault.reads < 100000); /* A regression must not loop forever. */
    if (count == 1) fault.byte_reads++;
    else fault.block_reads++;
    if (fault.eintr_read_first && fault.reads >= fault.eintr_read_first &&
        fault.reads - fault.eintr_read_first < fault.eintr_read_count) {
        fault.read_interrupts++;
        errno = EINTR;
        return -1;
    }
    if (fault.read_limit && count > fault.read_limit) count = fault.read_limit;
    long n = sys_read(fd, buf, count);
    if (n > 0) fault.bytes_read += (size_t)n;
    if (n == 0) fault.eof_reads++;
    return n;
}

static int injected_fstat(int fd, struct stat* st) {
    CHECK(fd == fault.fd);
    fault.stats++;
    if (fault.stat_error) {
        errno = fault.stat_error;
        return -1;
    }
    return fstat(fd, st);
}

static off_t injected_lseek(int fd, off_t offset, int whence) {
    CHECK(fd == fault.fd);
    CHECK(whence == SEEK_CUR);
    if (offset == 0) {
        CHECK(++fault.probes < 10);
        if (fault.probe_error) {
            errno = fault.probe_error;
            return -1;
        }
    } else {
        CHECK(offset < 0);
        CHECK(++fault.seekbacks < 10);
        fault.last_seekback = offset;
        if (fault.seekbacks <= fault.eintr_seek_count) {
            fault.seek_interrupts++;
            errno = EINTR;
            return -1;
        }
        if (fault.seekback_error) {
            errno = fault.seekback_error;
            return -1;
        }
    }
    return lseek(fd, offset, whence);
}

static void* injected_realloc(void* ptr, size_t size) {
    CHECK(ptr == fault.heap);
    CHECK(size > 0);
    if (++fault.reallocs == fault.fail_realloc) {
        errno = ENOMEM;
        return NULL;
    }
    void* next = realloc(ptr, size);
    CHECK(next != NULL); /* Unexpected host OOM is a fixture failure. */
    fault.heap = next;
    fault.allocations++;
    return next;
}

static void observed_free(void* ptr) {
    if (ptr) {
        CHECK(ptr == fault.heap);
        fault.heap = NULL;
        fault.heap_frees++;
    }
    free(ptr);
}

/* readbuf.h must first be included here, with sys_read already redirected:
 * jash_readbuf_getc/fill are inline, and must see the injected refill. */
#define JASH_AGENT_ONLY 1
#define sys_read injected_read
#define fstat injected_fstat
#define lseek injected_lseek
#define realloc injected_realloc
#define free observed_free
#include "../c/builtins/read.c"
#undef free
#undef realloc
#undef lseek
#undef fstat
#undef sys_read

static jash_state_t* state;
static char stored[20000], attempted[20000], diagnostic[512];
static unsigned assignments, stdout_bytes;
static int storage_error;
static off_t assignment_offset;

const char* jash_env_get(const jash_state_t* s, const char* key) {
    CHECK(s == state);
    CHECK(strcmp(key, "IFS") == 0);
    return NULL; /* Default IFS, independent of the invoking shell. */
}

int jash_env_set(jash_state_t* s, const char* key, const char* val) {
    CHECK(s == state);
    CHECK(strcmp(key, "value") == 0);
    CHECK(strlen(val) < sizeof(stored));
    assignments++;
    strcpy(attempted, val);
    assignment_offset = lseek(fault.fd, 0, SEEK_CUR); /* Real, uncounted probe. */
    CHECK(assignment_offset >= 0);
    if (storage_error) return -1;
    strcpy(stored, val);
    return 0;
}

long jash_writes(jash_writer_t w, void* ctx, const char* text) {
    return w(ctx, text, strlen(text));
}

long jash_stderr_writer(void* ctx, const char* buf, size_t n) {
    CHECK(ctx == NULL);
    size_t used = strlen(diagnostic);
    CHECK(used + n < sizeof(diagnostic));
    memcpy(diagnostic + used, buf, n);
    diagnostic[used + n] = '\0';
    return (long)n;
}

static long capture_stdout(void* ctx, const char* buf, size_t n) {
    CHECK(ctx == NULL);
    (void)buf;
    stdout_bytes += (unsigned)n;
    return (long)n;
}

static const char* tmpdir;
static unsigned passed;

static void reset_call(int fd) {
    CHECK(fault.heap == NULL);
    memset(&fault, 0, sizeof(fault));
    fault.fd = fd;
    strcpy(stored, "unchanged");
    attempted[0] = diagnostic[0] = '\0';
    assignments = stdout_bytes = 0;
    storage_error = 0;
    assignment_offset = -1;
}

static int fixture(const char* name, const char* data, size_t len, off_t start) {
    case_name = name;
    size_t cap = strlen(tmpdir) + sizeof("/read.XXXXXX");
    char* path = malloc(cap); /* All fixture allocations are outside macros. */
    CHECK(path != NULL);
    CHECK(snprintf(path, cap, "%s/read.XXXXXX", tmpdir) > 0);
    int fd = mkstemp(path);
    CHECK(fd >= 0);
    CHECK(unlink(path) == 0);
    free(path);
    if (fd <= STDERR_FILENO) {
        int explicit_fd = fcntl(fd, F_DUPFD_CLOEXEC, STDERR_FILENO + 1);
        CHECK(explicit_fd > STDERR_FILENO);
        CHECK(close(fd) == 0);
        fd = explicit_fd;
    }
    struct stat st;
    CHECK(fstat(fd, &st) == 0 && S_ISREG(st.st_mode));
    for (size_t done = 0; done < len;) {
        ssize_t n = write(fd, data + done, len - done);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0);
        done += (size_t)n;
    }
    CHECK(lseek(fd, start, SEEK_SET) == start);
    reset_call(fd);
    return fd;
}

static int invoke(int fd) {
    CHECK(state->interactive == 0);
    int rc = jash_b_read_from_fd(state, capture_stdout, NULL, "-r value", fd);
    CHECK(stdout_bytes == 0);
    CHECK(fault.heap == NULL); /* Also checked without LeakSanitizer. */
    return rc;
}

static void check_offset(int fd, off_t expected) {
    off_t actual = lseek(fd, 0, SEEK_CUR); /* Bypass injected lseek. */
    if (actual != expected)
        fprintf(stderr, "%s: offset %lld, expected %lld\n", case_name,
                (long long)actual, (long long)expected);
    CHECK(actual == expected);
}

static void check_assignment(const char* expected, off_t offset) {
    CHECK(assignments == 1);
    CHECK(strcmp(stored, expected) == 0);
    CHECK(strcmp(attempted, expected) == 0);
    CHECK(assignment_offset == offset); /* Restored before storage, too. */
}

static void check_no_assignment(void) {
    CHECK(assignments == 0);
    CHECK(attempted[0] == '\0');
    CHECK(strcmp(stored, "unchanged") == 0);
}

/* A subsequent ordinary consumer must see every unread byte. Expected offsets
 * and suffixes come from fixtures, never from the builtin's returned string. */
static void check_remaining(int fd, const char* expected, size_t len) {
    char buf[1024];
    for (size_t done = 0; done < len;) {
        size_t want = len - done < sizeof(buf) ? len - done : sizeof(buf);
        ssize_t n = read(fd, buf, want);
        if (n < 0 && errno == EINTR) continue;
        CHECK(n > 0);
        CHECK(memcmp(buf, expected + done, (size_t)n) == 0);
        done += (size_t)n;
    }
    CHECK(read(fd, buf, 1) == 0);
}

static void finish(int fd) {
    CHECK(fault.heap == NULL);
    CHECK(close(fd) == 0);
    printf("PASS: %s\n", case_name);
    passed++;
}

static const char short_input[] = "skip:ab\\cd\nnext\n";

static void test_short_line(const char* name, size_t limit, unsigned eintr_first,
                            unsigned seek_interrupts) {
    int fd = fixture(name, short_input, sizeof(short_input) - 1, 5);
    fault.read_limit = limit;
    fault.eintr_read_first = eintr_first;
    fault.eintr_read_count = eintr_first ? 2 : 0;
    fault.eintr_seek_count = seek_interrupts;
    CHECK(invoke(fd) == 0);
    check_offset(fd, 11); /* Five-byte prefix + five raw bytes + newline. */
    check_assignment("ab\\cd", 11);
    CHECK(diagnostic[0] == '\0');
    CHECK(fault.stats == 1 && fault.probes == 1);
    unsigned expected_reads = limit ? 2 : 1;
    if (eintr_first) expected_reads += 2;
    CHECK(fault.reads == expected_reads);
    CHECK(fault.block_reads == expected_reads && fault.byte_reads == 0);
    CHECK(fault.read_interrupts == (eintr_first ? 2u : 0u));
    CHECK(fault.bytes_read == (limit ? 8u : 11u));
    CHECK(fault.eof_reads == 0 && fault.reallocs == 0);
    CHECK(fault.seekbacks == 1 + seek_interrupts);
    CHECK(fault.seek_interrupts == seek_interrupts);
    CHECK(fault.last_seekback == (limit ? -2 : -5));
    check_remaining(fd, "next\n", 5);
    finish(fd);
}

static void test_eligibility_failure(const char* name, int stat_error,
                                     int probe_error) {
    int fd = fixture(name, short_input, sizeof(short_input) - 1, 5);
    fault.stat_error = stat_error;
    fault.probe_error = probe_error;
    CHECK(invoke(fd) == 0);
    check_offset(fd, 11);
    check_assignment("ab\\cd", 11);
    CHECK(diagnostic[0] == '\0');
    CHECK(fault.stats == 1 && fault.probes == (stat_error ? 0u : 1u));
    CHECK(fault.reads == 6 && fault.byte_reads == 6 && fault.block_reads == 0);
    CHECK(fault.bytes_read == 6 && fault.seekbacks == 0);
    CHECK(fault.reallocs == 0);
    check_remaining(fd, "next\n", 5);
    finish(fd);
}

static void test_seekback_failure(void) {
    int fd = fixture("failed final seek: diagnostic, no assignment", short_input,
                     sizeof(short_input) - 1, 5);
    fault.seekback_error = EIO;
    CHECK(invoke(fd) != 0);
    check_offset(fd, 16); /* Failed rewind leaves the physical block end. */
    check_no_assignment();
    CHECK(strstr(diagnostic, "read: unable to restore input position") != NULL);
    CHECK(fault.reads == 1 && fault.block_reads == 1 && fault.byte_reads == 0);
    CHECK(fault.stats == 1 && fault.probes == 1 && fault.seekbacks == 1);
    CHECK(fault.last_seekback == -5 && fault.bytes_read == 11);
    CHECK(fault.reallocs == 0);
    finish(fd);
}

enum long_mode { LONG_OK, LONG_EOF, LONG_OOM_FIRST, LONG_OOM_GROW,
                 LONG_SEEK_ERROR, LONG_STORAGE_ERROR };

static void test_long_line(const char* name, enum long_mode mode) {
    /* 9000 data bytes cross the 8192-byte refill boundary and four growth
     * points. Fixed offsets below deliberately pin the current reader. */
    char* data = malloc(9012);
    char* expected = malloc(9001);
    CHECK(data != NULL && expected != NULL);
    memcpy(data, "skip:", 5);
    for (size_t i = 0; i < 9000; i++) expected[i] = (char)('a' + i % 26);
    expected[9000] = '\0';
    memcpy(data + 5, expected, 9000);
    memcpy(data + 9005, "\nnext\n", 7);
    size_t len = mode == LONG_EOF ? 9005 : 9011;
    int fd = fixture(name, data, len, 5);
    if (mode == LONG_OOM_FIRST) fault.fail_realloc = 1;
    if (mode == LONG_OOM_GROW) fault.fail_realloc = 2;
    if (mode == LONG_SEEK_ERROR) fault.seekback_error = EIO;
    if (mode == LONG_STORAGE_ERROR) storage_error = 1;

    int rc = invoke(fd);
    CHECK(mode == LONG_OK ? rc == 0 : rc != 0);
    CHECK(fault.stats == 1 && fault.probes == 1 && fault.byte_reads == 0);
    CHECK(fault.block_reads == fault.reads);
    if (mode == LONG_OOM_FIRST || mode == LONG_OOM_GROW) {
        /* Reserve fails before consuming byte 1024 or 2048, respectively.
         * Use constants, not assignment length or captured seek arguments. */
        off_t offset = mode == LONG_OOM_FIRST ? 1028 : 2052;
        check_offset(fd, offset);
        check_no_assignment();
        CHECK(strstr(diagnostic, "read: out of memory for input line") != NULL);
        CHECK(fault.reads == 1 && fault.bytes_read == 8192);
        CHECK(fault.seekbacks == 1);
        CHECK(fault.last_seekback == (mode == LONG_OOM_FIRST ? -7169 : -6145));
        CHECK(fault.reallocs == (mode == LONG_OOM_FIRST ? 1u : 2u));
        CHECK(fault.allocations == (mode == LONG_OOM_FIRST ? 0u : 1u));
        CHECK(fault.heap_frees == (mode == LONG_OOM_FIRST ? 0u : 1u));
        check_remaining(fd, data + offset, len - (size_t)offset);
    } else {
        CHECK(fault.reallocs == 4 && fault.allocations == 4);
        CHECK(fault.heap_frees == 1);
        CHECK(fault.reads == (mode == LONG_EOF ? 3u : 2u));
        CHECK(fault.bytes_read == (mode == LONG_EOF ? 9000u : 9006u));
        CHECK(fault.eof_reads == (mode == LONG_EOF ? 1u : 0u));
        CHECK(fault.seekbacks == (mode == LONG_EOF ? 0u : 1u));
        if (mode != LONG_EOF) CHECK(fault.last_seekback == -5);
        if (mode == LONG_SEEK_ERROR) {
            check_offset(fd, 9011);
            check_no_assignment();
            CHECK(strstr(diagnostic, "read: unable to restore input position") != NULL);
        } else {
            off_t offset = mode == LONG_EOF ? 9005 : 9006;
            check_offset(fd, offset);
            CHECK(diagnostic[0] == '\0');
            if (mode == LONG_STORAGE_ERROR) {
                CHECK(assignments == 1 && assignment_offset == 9006);
                CHECK(strcmp(attempted, expected) == 0);
                CHECK(strcmp(stored, "unchanged") == 0);
            } else {
                check_assignment(expected, offset);
            }
            check_remaining(fd, data + offset, len - (size_t)offset);
        }
    }
    free(expected);
    free(data);
    finish(fd);
}

static void test_eof(const char* name, const char* data, size_t len,
                     const char* expected, off_t offset, unsigned reads) {
    int fd = fixture(name, data, len, 5);
    CHECK(invoke(fd) != 0);
    check_offset(fd, offset);
    check_assignment(expected, offset);
    CHECK(diagnostic[0] == '\0');
    CHECK(fault.stats == 1 && fault.probes == 1 && fault.seekbacks == 0);
    CHECK(fault.reads == reads && fault.block_reads == reads && fault.byte_reads == 0);
    CHECK(fault.eof_reads == 1 && fault.bytes_read == len - 5);
    CHECK(fault.reallocs == 0);
    check_remaining(fd, "", 0);
    finish(fd);
}

static void test_storage_failure(void) {
    int fd = fixture("storage failure returns nonzero after restoring offset",
                     short_input, sizeof(short_input) - 1, 5);
    storage_error = 1;
    CHECK(invoke(fd) != 0);
    check_offset(fd, 11);
    CHECK(assignments == 1 && assignment_offset == 11);
    CHECK(strcmp(attempted, "ab\\cd") == 0);
    CHECK(strcmp(stored, "unchanged") == 0);
    CHECK(diagnostic[0] == '\0');
    CHECK(fault.reads == 1 && fault.block_reads == 1 && fault.byte_reads == 0);
    CHECK(fault.seekbacks == 1 && fault.last_seekback == -5);
    CHECK(fault.reallocs == 0);
    check_remaining(fd, "next\n", 5);
    finish(fd);
}

static void test_repeated_calls(void) {
    const char data[] = "skip:one\ntwo\nlast";
    int fd = fixture("repeated explicit-fd calls preserve the shared offset",
                     data, sizeof(data) - 1, 5);
    CHECK(invoke(fd) == 0);
    check_offset(fd, 9);
    check_assignment("one", 9);
    CHECK(fault.block_reads == 1 && fault.byte_reads == 0);
    CHECK(diagnostic[0] == '\0');
    reset_call(fd);
    CHECK(invoke(fd) == 0);
    check_offset(fd, 13);
    check_assignment("two", 13);
    CHECK(fault.block_reads == 1 && fault.byte_reads == 0);
    CHECK(diagnostic[0] == '\0');
    check_remaining(fd, "last", 4);
    finish(fd);
}

int main(int argc, char** argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s PRIVATE_TMPDIR\n", argv[0]);
        return 2;
    }
    tmpdir = argv[1];
    state = calloc(1, sizeof(*state));
    CHECK(state != NULL);
    test_short_line("short regular line uses one block read", 0, 0, 0);
    test_short_line("short refills restore the exact unread suffix", 4, 0, 0);
    test_short_line("EINTR retries the initial buffered refill", 4, 1, 0);
    test_short_line("EINTR retries a later buffered refill", 4, 2, 0);
    test_short_line("EINTR retries the final seek-back", 4, 0, 2);
    test_eligibility_failure("failed eligibility lseek selects bytewise reads", 0, ESPIPE);
    test_eligibility_failure("interrupted eligibility lseek selects bytewise reads", 0, EINTR);
    test_eligibility_failure("failed fstat selects bytewise reads", EIO, 0);
    test_seekback_failure();
    test_long_line("long line crosses refill boundaries without truncation", LONG_OK);
    test_long_line("first allocation failure restores unread bytes", LONG_OOM_FIRST);
    test_long_line("growth allocation failure restores bytes and frees heap", LONG_OOM_GROW);
    test_long_line("failed final seek releases the grown line without assignment", LONG_SEEK_ERROR);
    test_long_line("long EOF partial preserves data, status, offset and frees heap", LONG_EOF);
    test_long_line("long storage failure returns nonzero and frees heap", LONG_STORAGE_ERROR);
    test_eof("EOF partial assigns data with nonzero status", "skip:tail", 9, "tail", 9, 2);
    test_eof("empty EOF assigns empty data with nonzero status", "skip:", 5, "", 5, 1);
    test_storage_failure();
    test_repeated_calls();
    free(state);
    printf("read regular unit: %u tests passed\n", passed);
    return 0;
}
