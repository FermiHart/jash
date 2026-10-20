// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/tread + tdiff — time-travel reads without rewind (ADR-0042 F8).
//
//   tread FILE [@checkpoint]     print FILE as it was at checkpoint (default: now)
//   tdiff FILE @checkpoint       unified diff: checkpoint vs working tree
//
// The timeline already snapshots content before overwriting (c/core/timeline.c).
// These builtins expose that snapshot store read-only, so an agent can compare
// without mutating current state.
//
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/timeline.h>
#include <core/adopt.h>
#include <core/structured_output.h>
#include "query_data.h"
#include "query_snapshot.h"
#include "diff_query_api.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/file.h>

#define CP_PATH_SIZE 600
#define CP_NAME_SIZE 64

static int checkpoint_depth(const char* name, int* out_depth) {
    const char* h = getenv("HOME");
    if (!h || !*h || !name || !out_depth) return 0;
    char path[CP_PATH_SIZE];
    /* ADR-0050 W9: checkpoints are per-repo (builtin_checkpoint.c cp_path) —
     * read the same file the writer writes, or every tread @name inside a
     * repo misses with "no checkpoint" (measured). */
    const char* k = jash_repo_key();
    int length = k && k[0] != '-' ? snprintf(path, sizeof(path), "%s/.jash/checkpoints-%s.tsv", h, k) :
        snprintf(path, sizeof(path), "%s/.jash/checkpoints.tsv", h);
    if (length < 0 || (size_t)length >= sizeof(path)) return -1;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat metadata;
    if (fstat(fd, &metadata) || !S_ISREG(metadata.st_mode) || metadata.st_size < 0 ||
        metadata.st_size > 131072 || metadata.st_uid != geteuid() || (metadata.st_mode & 0022) ||
        metadata.st_nlink != 1 || flock(fd, LOCK_SH)) { close(fd); return -1; }
    FILE* file = fdopen(fd, "r");
    if (!file) { close(fd); return -1; }
    char* line = NULL;
    size_t capacity = 0, rows = 0, bytes = 0;
    char names[64][CP_NAME_SIZE];
    ssize_t count;
    int found = 0, invalid = 0;
    while ((count = getline(&line, &capacity, file)) >= 0) {
        if (jash_query_cancel_poll(NULL)) { invalid = 1; break; }
        bytes += (size_t)count;
        if (bytes > 131072u || rows == 64u || count == 0 || line[count - 1] != '\n' ||
            memchr(line, 0, (size_t)count)) { invalid = 1; break; }
        line[count - 1] = '\0';
        char* rest = line;
        char* row_name = strsep(&rest, "\t");
        char* depth = strsep(&rest, "\t");
        char* timestamp = strsep(&rest, "\t");
        if (!row_name || !*row_name || strlen(row_name) >= CP_NAME_SIZE || !depth || !timestamp ||
            !rest || strlen(rest) >= 512u || strchr(rest, '\t')) { invalid = 1; break; }
        for (size_t i = 0; i < rows; i++) if (!strcmp(names[i], row_name)) invalid = 1;
        if (invalid) break;
        memcpy(names[rows++], row_name, strlen(row_name) + 1u);
        char* end;
        errno = 0;
        long value = strtol(depth, &end, 10);
        if (errno || end == depth || *end || value < 0 || value > INT_MAX) { invalid = 1; break; }
        errno = 0;
        (void)strtol(timestamp, &end, 10);
        if (errno || end == timestamp || *end) { invalid = 1; break; }
        if (!strcmp(row_name, name)) { *out_depth = (int)value; found = 1; }
    }
    if (ferror(file)) invalid = 1;
    struct stat after;
    if (fstat(fd, &after) || after.st_size != metadata.st_size || bytes != (size_t)metadata.st_size) invalid = 1;
    if (fclose(file)) invalid = 1;
    free(line);
    return invalid ? -1 : found;
}

static void err(const char* msg) {
    jash_writes(jash_stderr_writer, 0, msg);
}

static int write_file_to_writer(const char* path, jash_writer_t w, void* ctx) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[65536];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0) {
        long wrote = w(ctx, buf, (size_t)n);
        if (wrote < 0) { close(fd); return -1; }
    }
    close(fd);
    return n < 0 ? -1 : 0;
}

static int run_diff(const char* a, const char* a_label,
                    const char* b, const char* b_label) {
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        execlp("diff", "diff", "-u",
               "--label", a_label, "--label", b_label,
               a, b, (char*)NULL);
        _exit(127);
    }
    int status;
    if (waitpid(pid, &status, 0) < 0) return -1;
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    return 1;
}

/* Parse argv (argv[0] is the first argument, not the command name).
 * Path is the non-@ token; checkpoint is the token starting with '@'. */
static int parse_args(int argc, char** argv, const char** path,
                      const char** checkpoint) {
    if (argc < 1) return -1;
    *path = NULL;
    *checkpoint = NULL;
    for (int i = 0; i < argc; i++) {
        if (argv[i][0] == '@') {
            if (*checkpoint) return -1; /* duplicate checkpoint */
            *checkpoint = argv[i] + 1;
        } else if (!*path) {
            *path = argv[i];
        } else {
            return -1; /* extra positional */
        }
    }
    if (!*path) return -1;
    return 0;
}

static int do_tread(jash_state_t* s, jash_writer_t w, void* ctx,
                    const char* path, const char* checkpoint, int is_diff) {
    (void)s;
    int target_depth = jash_timeline_depth();

    /* Handle @proc:<pid> - capture live process tree */
    if (checkpoint && !strncmp(checkpoint, "proc:", 5)) {
        int pid = atoi(checkpoint + 5);
        if (pid <= 0) pid = getpid();
        char json[65536];
        if (jash_timeline_proc_tree(pid, json, sizeof json) != 0) {
            jash_writef(jash_stderr_writer, 0,
                        "%s: failed to capture proc tree for pid %d\n",
                        is_diff ? "tdiff" : "tread", pid);
            return 1;
        }
        if (is_diff) {
            /* For diff, we need a second snapshot to compare against */
            jash_writef(jash_stderr_writer, 0,
                "tdiff: @proc:<pid> diff not yet implemented (need two snapshots)\n");
            return 1;
        }
        /* Write JSON directly to writer */
        long wrote = w(ctx, json, strlen(json));
        if (wrote < 0) return 1;
        return 0;
    }

    if (checkpoint && checkpoint_depth(checkpoint, &target_depth) != 1) {
        jash_writef(jash_stderr_writer, 0,
                    "%s: no checkpoint '%s' (try: checkpoint list)\n",
                    is_diff ? "tdiff" : "tread", checkpoint);
        return 1;
    }

    char backup[1024];
    if (jash_timeline_backup_at(path, target_depth, backup, sizeof(backup)) != 0) {
        if (checkpoint)
            jash_writef(jash_stderr_writer, 0,
                        "%s: '%s' has no readable backup at '%s'\n",
                        is_diff ? "tdiff" : "tread", path, checkpoint);
        else
            jash_writef(jash_stderr_writer, 0,
                        "%s: '%s' has no readable backup\n",
                        is_diff ? "tdiff" : "tread", path);
        return 1;
    }

    if (is_diff) {
        char a_label[128], b_label[128];
        snprintf(a_label, sizeof(a_label), "%s @ %s", path,
                 checkpoint ? checkpoint : "now");
        snprintf(b_label, sizeof(b_label), "%s @ now", path);
        int rc = run_diff(backup, a_label, path, b_label);
        if (rc < 0) {
            err("tdiff: could not run diff\n");
            return 1;
        }
        return rc; /* 0 = identical, 1 = differences, >1 = diff error */
    }

    if (write_file_to_writer(backup, w, ctx) != 0) {
        jash_writef(jash_stderr_writer, 0, "tread: cannot read '%s'\n", backup);
        return 1;
    }
    return 0;
}

#include "tread_query.h"

int jash_b_tread(jash_state_t* s, jash_writer_t w, void* ctx,
                 int argc, char** argv) {
    int mode = tread_query_mode(argc, argv);
    if (mode) return tread_query_run(s, w, ctx, argc, argv, mode == 2, 0);
    const char* path = NULL;
    const char* checkpoint = NULL;
    if (parse_args(argc, argv, &path, &checkpoint) != 0) {
        jash_writef(jash_stderr_writer, 0,
                    "usage: tread FILE [@checkpoint]\n");
        return 1;
    }
    return do_tread(s, w, ctx, path, checkpoint, 0);
}

int jash_b_tdiff(jash_state_t* s, jash_writer_t w, void* ctx,
                 int argc, char** argv) {
    int mode = tread_query_mode(argc, argv);
    if (mode) return tread_query_run(s, w, ctx, argc, argv, mode == 2, 1);
    const char* path = NULL;
    const char* checkpoint = NULL;
    if (parse_args(argc, argv, &path, &checkpoint) != 0 || !checkpoint) {
        jash_writef(jash_stderr_writer, 0,
                    "usage: tdiff FILE @checkpoint\n");
        return 1;
    }
    return do_tread(s, w, ctx, path, checkpoint, 1);
}
