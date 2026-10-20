// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE   // strptime(): XSI, so glibc hides it behind _XOPEN_SOURCE
                      // /_GNU_SOURCE. Without a declaration C89-implicit rules
                      // make it return int, TRUNCATING the returned char* to 32
                      // bits — the -d/-t parse below then walks a bad pointer.
                      // Darwin declares it unconditionally, so this was silent.
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/touch — set/create file access & modification times.
//
// ADR-0044 (BUG-161): rewritten from a stub that SKIPPED every flag. Now a full
// coreutils-parity touch: -t STAMP, -d DATE, -r REF, -c/--no-create, -a/-m
// (selective), -h/--no-dereference, --. argv_fn (quote-aware) so a -d date with
// a space (`touch -d "2020-01-01 00:00" f`) arrives as one token. Performance:
// still in-process — no fork/exec per file in create loops.

#include "builtin.h"
#include <core/mutation_report.h>
#include <core/timeline.h>
#include <sys/syscalls.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <fcntl.h>
#include <time.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdio.h>

static int touch_calendar_time(struct tm* tm, time_t* out) {
    struct tm requested = *tm;
    time_t t = mktime(tm);
    if (tm->tm_year != requested.tm_year || tm->tm_mon != requested.tm_mon ||
        tm->tm_mday != requested.tm_mday || tm->tm_hour != requested.tm_hour ||
        tm->tm_min != requested.tm_min || tm->tm_sec != requested.tm_sec) return -1;
    *out = t;
    return 0;
}

static void touch_stat_times(const struct stat* st, struct timespec tv[2]) {
#ifdef __APPLE__
    tv[0] = st->st_atimespec; tv[1] = st->st_mtimespec;
#else
    tv[0] = st->st_atim; tv[1] = st->st_mtim;
#endif
}

/* Parse a -t stamp: [[CC]YY]MMDDhhmm[.ss]. Returns 0 on success. */
static int touch_parse_t(const char* s, time_t* out) {
    /* Split optional .ss */
    char buf[32];
    if (strlen(s) >= sizeof(buf)) return -1;
    strlcpy(buf, s, sizeof buf);
    int sec = 0;
    char* dot = strchr(buf, '.');
    if (dot) {
        if (strlen(dot + 1) != 2 || dot[1] < '0' || dot[1] > '9' ||
            dot[2] < '0' || dot[2] > '9') return -1;
        sec = (dot[1] - '0') * 10 + dot[2] - '0';
        *dot = 0;
    }
    size_t n = strlen(buf);
    /* Valid lengths: MMDDhhmm(8), YYMMDDhhmm(10), CCYYMMDDhhmm(12). */
    if (n != 8 && n != 10 && n != 12) return -1;
    for (size_t i = 0; i < n; i++) if (buf[i] < '0' || buf[i] > '9') return -1;
    struct tm tm; memset(&tm, 0, sizeof tm);
    int idx = 0;
    int year;
    if (n == 12) { year = (buf[0]-'0')*1000 + (buf[1]-'0')*100 + (buf[2]-'0')*10 + (buf[3]-'0'); idx = 4; }
    else if (n == 10) { int yy = (buf[0]-'0')*10 + (buf[1]-'0'); year = (yy < 69 ? 2000 : 1900) + yy; idx = 2; }
    else { /* n==8: use current year */
        time_t now = time(NULL); struct tm* lt = localtime(&now); year = lt->tm_year + 1900;
    }
    tm.tm_year = year - 1900;
    tm.tm_mon  = (buf[idx]-'0')*10 + (buf[idx+1]-'0') - 1; idx += 2;
    tm.tm_mday = (buf[idx]-'0')*10 + (buf[idx+1]-'0');     idx += 2;
    tm.tm_hour = (buf[idx]-'0')*10 + (buf[idx+1]-'0');     idx += 2;
    tm.tm_min  = (buf[idx]-'0')*10 + (buf[idx+1]-'0');
    tm.tm_sec  = sec;
    tm.tm_isdst = -1;
    return touch_calendar_time(&tm, out);
}

/* Parse a -d date. Supports @EPOCH, ISO "YYYY-MM-DD[ HH:MM[:SS]]", and "now". */
static int touch_parse_d(const char* s, time_t* out) {
    if (!s || !*s) return -1;
    if (strcmp(s, "now") == 0) { *out = time(NULL); return 0; }
    if (s[0] == '@') {
        char* end;
        errno = 0;
        long long value = strtoll(s + 1, &end, 10);
        if (errno || end == s + 1 || *end || (long long)(time_t)value != value) return -1;
        *out = (time_t)value;
        return 0;
    }
    struct tm tm; memset(&tm, 0, sizeof tm); tm.tm_isdst = -1;
    const char* rest = strptime(s, "%Y-%m-%d %H:%M:%S", &tm);
    if (!rest) { memset(&tm, 0, sizeof tm); tm.tm_isdst = -1; rest = strptime(s, "%Y-%m-%d %H:%M", &tm); }
    if (!rest) { memset(&tm, 0, sizeof tm); tm.tm_isdst = -1; rest = strptime(s, "%Y-%m-%d", &tm); }
    if (!rest || *rest) return -1;
    return touch_calendar_time(&tm, out);
}

static int touch_run(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv,
                     jash_mutation_report_t* report) {
    (void)s;

    int no_create = 0;      /* -c */
    int set_atime = 0;      /* neither specified means both */
    int set_mtime = 0;
    int no_deref  = 0;      /* -h */
    int have_time = 0;      /* an explicit -t/-d/-r was given */
    time_t the_time = 0;
    struct timespec reference[2];
    int have_reference = 0;
    int preview = 0;

    int i = 0;
    int end_opts = 0;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (end_opts || a[0] != '-' || a[1] == '\0') break;
        if (strcmp(a, "--") == 0) { i++; break; }
        if (!strcmp(a, "--dry-run")) { preview = 1; if (report) report->preview = 1; continue; }
        if (strcmp(a, "-c") == 0 || strcmp(a, "--no-create") == 0) { no_create = 1; }
        else if (strcmp(a, "-a") == 0) { set_atime = 1; }
        else if (strcmp(a, "-m") == 0) { set_mtime = 1; }
        else if (strcmp(a, "-h") == 0 || strcmp(a, "--no-dereference") == 0) { no_deref = 1; }
        else if (strcmp(a, "-t") == 0 && i + 1 < argc) {
            if (touch_parse_t(argv[++i], &the_time) != 0) {
                jash_writef(jash_stderr_writer, 0, "touch: invalid date format '%s'\n", argv[i]); return 1; }
            have_time = 1; have_reference = 0;
        } else if (strncmp(a, "-t", 2) == 0 && a[2]) {
            if (touch_parse_t(a + 2, &the_time) != 0) {
                jash_writef(jash_stderr_writer, 0, "touch: invalid date format '%s'\n", a + 2); return 1; }
            have_time = 1; have_reference = 0;
        } else if ((strcmp(a, "-d") == 0 || strcmp(a, "--date") == 0) && i + 1 < argc) {
            if (touch_parse_d(argv[++i], &the_time) != 0) {
                jash_writef(jash_stderr_writer, 0, "touch: invalid date '%s'\n", argv[i]); return 1; }
            have_time = 1; have_reference = 0;
        } else if (strncmp(a, "--date=", 7) == 0) {
            if (touch_parse_d(a + 7, &the_time) != 0) {
                jash_writef(jash_stderr_writer, 0, "touch: invalid date '%s'\n", a + 7); return 1; }
            have_time = 1; have_reference = 0;
        } else if ((strcmp(a, "-r") == 0 || strcmp(a, "--reference") == 0) && i + 1 < argc) {
            struct stat rst;
            if (stat(argv[++i], &rst) != 0) {
                jash_writef(jash_stderr_writer, 0, "touch: failed to get attributes of '%s'\n", argv[i]); return 1; }
            touch_stat_times(&rst, reference); have_reference = 1;
        } else if (strncmp(a, "--reference=", 12) == 0) {
            struct stat rst;
            if (stat(a + 12, &rst) != 0) {
                jash_writef(jash_stderr_writer, 0, "touch: failed to get attributes of '%s'\n", a + 12); return 1; }
            touch_stat_times(&rst, reference); have_reference = 1;
        } else {
            jash_writef(jash_stderr_writer, 0, "touch: unknown option '%s'\n", a); return 1;
        }
    }

    if (i >= argc) { jash_writes(jash_stderr_writer, 0, "touch: no files specified\n"); return 1; }
    if (!set_atime && !set_mtime) set_atime = set_mtime = 1;

    /* UTIME_OMIT preserves the unselected field in the kernel, without racing
     * a userspace read/restore. UTIME_NOW is resolved for each syscall. */
    struct timespec tv[2] = {{0, UTIME_NOW}, {0, UTIME_NOW}};
    if (have_reference) { tv[0] = reference[0]; tv[1] = reference[1]; }
    else if (have_time) {
        tv[0].tv_sec = tv[1].tv_sec = the_time;
        tv[0].tv_nsec = tv[1].tv_nsec = 0;
    }
    if (!set_atime) tv[0].tv_nsec = UTIME_OMIT;
    if (!set_mtime) tv[1].tv_nsec = UTIME_OMIT;

    int errors = 0;
    for (; i < argc; i++) {
        if (jash_mutation_report_stop(report)) return jash_mutation_report_stop(report);
        const char* path = argv[i];
        jash_mutation_item_t item = {.destination = path, .action = "touch",
                                    .code = "set_times"};
        int rc = 1;
        struct stat st;
        int exists = ((no_deref ? lstat(path, &st) : stat(path, &st)) == 0);
        int created = 0;

        if (!exists) {
            if (errno != ENOENT) {
                jash_writef(jash_stderr_writer, 0, "touch: cannot inspect '%s'\n", path);
                item.code = "inspect_target";
                goto item_done;
            }
            if (no_create) {
                item.status = JASH_MUT_SKIPPED; item.code = "absent"; rc = 0;
                goto item_done;
            }
            if (no_deref) {
                jash_writef(jash_stderr_writer, 0, "touch: cannot touch '%s': missing entry\n", path);
                item.code = "missing_entry";
                goto item_done;
            }
            if (!preview) {
                /* O_NONBLOCK prevents a raced FIFO from blocking creation.
                 * Keep the fd for timestamp assignment instead of reopening
                 * a name that another process may already have replaced. */
                int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_NONBLOCK | O_CLOEXEC, 0666);
                if (fd >= 0) created = 1;
                else if (errno == EEXIST) {
                    fd = open(path, O_WRONLY | O_NONBLOCK | O_CLOEXEC);
                    if (fd >= 0 && fstat(fd, &st)) { close(fd); fd = -1; }
                }
                if (fd < 0) {
                    jash_writef(jash_stderr_writer, 0, "touch: cannot create '%s'\n", path);
                    item.code = "create_file"; goto item_done;
                }
                item.effect = JASH_MUT_POSSIBLE;
                rc = futimens(fd, tv) == 0 ? 0 : 1;
                if (!rc) item.effect = JASH_MUT_CHANGED;
                if (close(fd) != 0) { rc = 1; item.code = "close_file"; }
                goto item_done;
            }
        }

        if (preview) { rc = 0; goto item_done; }

        rc = utimensat(AT_FDCWD, path, tv, no_deref ? AT_SYMLINK_NOFOLLOW : 0) == 0 ? 0 : 1;
        if (rc != 0) {
            jash_writef(jash_stderr_writer, 0, "touch: setting times of '%s' failed\n", path);
        }
item_done:
        if (!rc && item.status != JASH_MUT_SKIPPED) {
            item.status = preview ? JASH_MUT_PLANNED : JASH_MUT_COMPLETED;
            item.effect = preview ? JASH_MUT_NONE : JASH_MUT_CHANGED;
            item.code = NULL;
            if (!preview && report) {
                struct stat entry;
                if (!lstat(path, &entry) && (no_deref || !S_ISLNK(entry.st_mode))) {
                    char previous[128], description[256];
                    snprintf(description, sizeof(description), "touch %s", path);
                    if (created) jash_timeline_record_ref(JASH_UNDO_FILE_CREATE, description, path, NULL, &item.undo);
                    else {
                        struct timespec old[2]; touch_stat_times(&st, old);
                        snprintf(previous, sizeof(previous), "%lld:%ld:%lld:%ld",
                                 (long long)old[0].tv_sec, old[0].tv_nsec,
                                 (long long)old[1].tv_sec, old[1].tv_nsec);
                        jash_timeline_record_ref(JASH_UNDO_TIMES, description, path, previous, &item.undo);
                    }
                }
            }
            if (preview && !report) jash_writef(w, ctx, "would touch '%s'\n", path);
        }
        if (rc) errors = 1;
        if (jash_mutation_report_item(report, &item) != 0) return 125;
    }

    return errors ? 1 : 0;
}

int jash_b_touch(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    static const char* const values[] = {"-t", "-d", "--date", "-r", "--reference", NULL};
    int count;
    char** args;
    jash_mutation_format_t format;
    int json = jash_mutation_args_mode(argc, argv, values, &count, &args, &format);
    int args_error = errno;
    if (!json) return touch_run(s, w, ctx, argc, argv, NULL);
    jash_mutation_report_t report;
    if (jash_mutation_report_begin_mode(&report, "touch", "filesystem_action", format, w, ctx)) { free(args); return 125; }
    if (json < 0) { report.error_code = args_error == ENOMEM ? "out_of_memory" : "invalid_format_options"; return jash_mutation_report_end(&report, 2); }
    int rc = touch_run(s, jash_stderr_writer, NULL, count, args, &report);
    free(args);
    return jash_mutation_report_end(&report, rc);
}
