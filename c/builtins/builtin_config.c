// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Checked configuration operations. Secret bytes are never included in output.
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <sys/syscalls.h>

#define CONFIG_KEY_LIMIT 65536u

typedef struct { int changed, durable, number; const char* error; } config_outcome_t;

static int config_failure(config_outcome_t* outcome, const char* error, int number) {
    outcome->error = error; outcome->number = number ? number : EIO; return -1;
}

static int config_sync_parent(const char* path) {
    char directory[PATH_MAX];
    size_t length = strlen(path);
    if (length >= sizeof directory) { errno = ENAMETOOLONG; return -1; }
    memcpy(directory, path, length + 1u);
    char* slash = strrchr(directory, '/');
    if (slash) *slash = 0; else memcpy(directory, ".", 2);
    int fd = open(directory[0] ? directory : "/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return -1;
    int rc = fsync(fd), saved = errno;
    if (close(fd) && !rc) { rc = -1; saved = errno; }
    errno = saved; return rc;
}

static int config_directory(const char* path, config_outcome_t* outcome) {
    if (!mkdir(path, 0755)) {
        outcome->changed = 1;
        if (config_sync_parent(path)) return config_failure(outcome, "directory_sync_failed", errno);
        outcome->durable = 1; return 0;
    }
    if (errno != EEXIST) return config_failure(outcome, "mkdir_failed", errno);
    struct stat st;
    if (lstat(path, &st)) return config_failure(outcome, "stat_failed", errno);
    if (!S_ISDIR(st.st_mode)) return config_failure(outcome, "not_a_directory", ENOTDIR);
    return 0;
}

static int config_write_all(int fd, const char* bytes, size_t length) {
    while (length) {
        ssize_t n = write(fd, bytes, length);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        bytes += n; length -= (size_t)n;
    }
    return 0;
}

static int config_write_key(const char* path, const char* value, int replace, config_outcome_t* outcome) {
    struct stat st;
    if (!lstat(path, &st)) {
        if (!S_ISREG(st.st_mode)) return config_failure(outcome, "not_a_regular_file", EINVAL);
        if (!replace) return 0;
    } else if (errno != ENOENT) return config_failure(outcome, "stat_failed", errno);
    char temporary[PATH_MAX];
    int length = snprintf(temporary, sizeof temporary, "%s.tmp.XXXXXX", path);
    if (length < 0 || (size_t)length >= sizeof temporary) return config_failure(outcome, "path_too_long", ENAMETOOLONG);
    int fd = mkstemp(temporary);
    if (fd < 0) return config_failure(outcome, "open_failed", errno);
    const char* error = "write_failed";
    if (fchmod(fd, 0600)) { error = "chmod_failed"; goto failed; }
    if (config_write_all(fd, value, strlen(value)) || config_write_all(fd, "\n", 1u)) goto failed;
    if (fsync(fd)) { error = "fsync_failed"; goto failed; }
    if (close(fd)) {
        int saved = errno; unlink(temporary); return config_failure(outcome, "close_failed", saved);
    }
    /* link publishes a placeholder only if absent, without clobbering a file
     * another process created since inspection. set-key uses atomic replace. */
    int published = replace ? rename(temporary, path) : link(temporary, path);
    if (published) {
        int saved = errno;
        if (unlink(temporary)) return config_failure(outcome, "cleanup_failed", errno);
        if (!replace && saved == EEXIST) return 0;
        return config_failure(outcome, "publish_failed", saved);
    }
    outcome->changed = 1;
    if (!replace && unlink(temporary)) return config_failure(outcome, "cleanup_failed", errno);
    if (config_sync_parent(path)) return config_failure(outcome, "directory_sync_failed", errno);
    outcome->durable = 1; return 0;
failed: {
    int saved = errno;
    if (close(fd) && !saved) saved = errno;
    if (unlink(temporary)) return config_failure(outcome, "cleanup_failed", errno);
    return config_failure(outcome, error, saved);
}
}

static int config_key_configured(const char* path, int* configured) {
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return -1;
    char buffer[4096]; size_t seen = 0; int found = 0, rc = 0, saved = 0;
    for (;;) {
        ssize_t n = read(fd, buffer, sizeof buffer);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { rc = -1; saved = errno; break; }
        if (!n) break;
        seen += (size_t)n;
        if (seen > CONFIG_KEY_LIMIT + 1u) { rc = -1; saved = EFBIG; break; }
        for (ssize_t i = 0; i < n && !found; i++) {
            if (buffer[i] == '\n' || buffer[i] == '\r' || buffer[i] == ' ' || buffer[i] == '\t') continue;
            *configured = buffer[i] != '#'; found = 1;
        }
    }
    if (close(fd) && !rc) { rc = -1; saved = errno; }
    errno = saved; return rc;
}

static int config_row(jash_discovery_rows_t* rows, const char* operation, const char* component,
                       const char* path, int key_file, int mutation, config_outcome_t* outcome) {
    struct stat st; int exists = 0, inspected = 1, configured = 0;
    const char* kind = NULL;
    if (path && !lstat(path, &st)) {
        exists = 1;
        kind = S_ISREG(st.st_mode) ? "file" : S_ISDIR(st.st_mode) ? "directory" : S_ISLNK(st.st_mode) ? "symlink" : "other";
        if (key_file && S_ISREG(st.st_mode)) {
            if (config_key_configured(path, &configured) && !outcome->error) config_failure(outcome, "key_inspection_failed", errno);
        } else if (key_file && !outcome->error) config_failure(outcome, "not_a_regular_file", EINVAL);
    } else if (path && errno != ENOENT && errno != ENOTDIR) {
        inspected = 0;
        if (!outcome->error) config_failure(outcome, "stat_failed", errno);
    }
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("component"), jash_discovery_string(rows, component)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("exists"), path && inspected ? jdv_value_bool(exists) : jdv_value_nothing()},
        {JDV_LITERAL("kind"), jash_discovery_string(rows, kind)},
        {JDV_LITERAL("mode"), exists ? jdv_value_u64(st.st_mode & 07777u) : jdv_value_nothing()},
        {JDV_LITERAL("changed"), jdv_value_bool(outcome->changed)},
        {JDV_LITERAL("durable"), mutation ? jdv_value_bool(outcome->durable) : jdv_value_nothing()},
        {JDV_LITERAL("key_configured"), key_file && inspected && !outcome->error ? jdv_value_bool(configured) : jdv_value_nothing()},
        {JDV_LITERAL("ok"), jdv_value_bool(!outcome->error)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, outcome->error)},
        {JDV_LITERAL("errno"), outcome->error ? jdv_value_u64((unsigned)outcome->number) : jdv_value_nothing()},
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

int jash_b_jash_config_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0) return jash_discovery_error(w, ctx, "config", mode, "conflicting_formats", 2);
    const char* operands[2]; int count = 0, ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if ((!ended && argv[i][0] == '-') || count == 2) return jash_discovery_error(w, ctx, "config", mode, "invalid_options", 2);
        operands[count++] = argv[i];
    }
    const char* operation = count ? operands[0] : (mode == JASH_DISCOVERY_TEXT ? "help" : "show");
    if (!strcmp(operation, "key")) operation = "set-key";
    int show = !strcmp(operation, "show"), init = !strcmp(operation, "init"), set_key = !strcmp(operation, "set-key"), help = !strcmp(operation, "help");
    if ((!show && !init && !set_key && !help) || (set_key && count != 2) || (!set_key && count > 1))
        return jash_discovery_error(w, ctx, "config", mode, "invalid_options", 2);
    if (set_key && (!operands[1][0] || strlen(operands[1]) > CONFIG_KEY_LIMIT || strpbrk(operands[1], "\n\r")))
        return jash_discovery_error(w, ctx, "config", mode, "invalid_key", 2);
    if (help && mode == JASH_DISCOVERY_TEXT)
        return jash_writes(w, ctx, "Usage: config init | set-key <key> | show | help\n") < 0 ? 125 : 0;
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    static const char* suffixes[] = {"/.config", "/.config/jash", "/.jash_obsidian", "/.config/jash/groq.key"};
    static const char* components[] = {"config_parent", "config_dir", "learning_dir", "key_file"};
    char paths[4][PATH_MAX];
    for (size_t i = 0; i < 4; i++) {
        int length = snprintf(paths[i], sizeof paths[i], "%s%s", home, suffixes[i]);
        if (length < 0 || (size_t)length >= sizeof paths[i]) return jash_discovery_error(w, ctx, "config", mode, "path_too_long", 2);
    }
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "config", mode, "allocation_failed", 125);
    if (init || set_key) {
        int admitted = jash_discovery_effects_begin(&rows, w, ctx, "config", mode, "jash.config.v1");
        if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    }
    int status = 0, parents_ok = 1;
    if (help) {
        config_outcome_t outcome = {0};
        config_row(&rows, "help", "init | set-key <key> | show | help", NULL, 0, 0, &outcome);
    } else for (size_t i = 0; i < 4; i++) {
        if (set_key && i == 2) continue;
        config_outcome_t outcome = {0};
        if (init || set_key) {
            if (i < 3) {
                if (config_directory(paths[i], &outcome) && i < 2) parents_ok = 0;
            } else if (!parents_ok) config_failure(&outcome, "parent_unavailable", ENOTDIR);
            else config_write_key(paths[i], set_key ? operands[1] : "# Configure with: config set-key <key>", set_key, &outcome);
        }
        config_row(&rows, operation, components[i], paths[i], i == 3, init || set_key, &outcome);
        if (outcome.error) status = 1;
        if (rows.error) break;
        if (jash_discovery_effects_flush(&rows)) break;
    }
    int rc;
    if (mode != JASH_DISCOVERY_TEXT || rows.error) rc = jash_discovery_emit_schema(w, ctx, "config", mode, &rows, status, "jash.config.v1");
    else {
        rc = status;
        for (size_t i = 0; i < rows.count; i++) {
            const jdv_value_t* component = jash_discovery_field(&rows.items[i], "component");
            const jdv_value_t* path = jash_discovery_field(&rows.items[i], "path");
            const jdv_value_t* error = jash_discovery_field(&rows.items[i], "error");
            const jdv_value_t* exists = jash_discovery_field(&rows.items[i], "exists");
            if (error->kind != JDV_NOTHING) jash_writef(jash_stderr_writer, NULL, "config: %.*s: %.*s\n",
                    (int)component->as.slice.len, component->as.slice.data, (int)error->as.slice.len, error->as.slice.data);
            else if (jash_writef(w, ctx, "%.*s: %s (%.*s)\n", (int)component->as.slice.len, component->as.slice.data,
                    exists->as.boolean ? "present" : "absent", (int)path->as.slice.len, path->as.slice.data) < 0) rc = 125;
        }
    }
    jash_discovery_rows_free(&rows); return rc;
}

int jash_b_jash_config(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_jash_config_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
