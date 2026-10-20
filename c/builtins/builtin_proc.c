// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/proc — process snapshot/restore/tree/diff (Wave 2).
//
//   proc snapshot [PID] [--output FILE]    capture PID tree + FDs + env to JSON
//   proc restore <snapshot.json>           re-exec process tree from snapshot
//   proc tree [PID] [--json]               print current PID tree as JSON
//   proc diff <snapA> <snapB> [--json]     compare two snapshots
//
// The snapshot captures: cmdline, environ, open FDs (with targets), PID tree.
// Stored as JSON in ~/.jash/proc-snapshots/ or custom path.
// Reuses timeline for undo/redo integration.
//
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/timeline.h>
#include <core/adopt.h>   /* jash_repo_key */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>
#include <limits.h>
#include <dirent.h>

#include "process_snapshot.h"

#define PROC_SNAP_DIR_MAX 512

static int proc_snap_dir(char* out, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    const char* k = jash_repo_key();
    int length;
    if (k && k[0] != '-')
        length = snprintf(out, cap, "%s/.jash/proc-snapshots-%s", h, k);
    else
        length = snprintf(out, cap, "%s/.jash/proc-snapshots", h);
    if (length < 0 || (size_t)length >= cap) { errno = ENAMETOOLONG; return 0; }
    return 1;
}

static int ensure_dir(const char* path) {
    struct stat st;
    if (stat(path, &st) == 0) return S_ISDIR(st.st_mode) ? 0 : -1;
    return mkdir(path, 0700);
}

static void auto_snap_name(char* out, size_t cap) {
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    strftime(out, cap, "proc-snap-%Y%m%d-%H%M%S.json", tm);
}

static int parse_pid_arg(const char* arg) {
    if (!arg || !*arg) return 0;  /* 0 = current shell PID */
    if (arg[0] == '@') return atoi(arg + 1);  /* @checkpoint not supported here */
    return atoi(arg);
}

/* proc snapshot [PID] [--output FILE] [--json] */
int jash_b_proc_snapshot(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int target_pid = 0;
    const char* out_path = NULL;
    int json_mode = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) {
            json_mode = 1;
        } else if (!strcmp(argv[i], "--output") || !strcmp(argv[i], "-o")) {
            if (++i >= argc) {
                jash_writef(jash_stderr_writer, 0, "proc snapshot: --output requires a path\n");
                return 2;
            }
            out_path = argv[i];
        } else if (argv[i][0] != '-') {
            target_pid = parse_pid_arg(argv[i]);
        } else {
            jash_writef(jash_stderr_writer, 0, "proc snapshot: unknown flag '%s'\n", argv[i]);
            return 2;
        }
    }

    char path[PROC_SNAP_DIR_MAX];
    if (out_path) {
        snprintf(path, sizeof path, "%s", out_path);
    } else {
        char dir[PROC_SNAP_DIR_MAX];
        if (!proc_snap_dir(dir, sizeof dir)) {
            jash_writef(jash_stderr_writer, 0, "proc snapshot: HOME not set\n");
            return 1;
        }
        if (ensure_dir(dir) != 0) {
            jash_writef(jash_stderr_writer, 0, "proc snapshot: cannot create snapshot dir\n");
            return 1;
        }
        char name[128];
        auto_snap_name(name, sizeof name);
        snprintf(path, sizeof path, "%s/%s", dir, name);
    }

    if (jash_timeline_proc_snapshot(target_pid, path, sizeof path) != 0) {
        jash_writef(jash_stderr_writer, 0, "proc snapshot: failed to capture pid %d\n", target_pid);
        return 1;
    }

    if (json_mode) {
        jash_writef(w, ctx, "{\"snapshot\":\"%s\",\"pid\":%d}\n", path, target_pid);
    } else {
        jash_writef(w, ctx, "proc snapshot: pid %d -> %s\n", target_pid, path);
    }
    return 0;
}

/* proc restore <snapshot.json> [--json] */
int jash_b_proc_restore(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    const char* snap_path = NULL;
    int json_mode = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) {
            json_mode = 1;
        } else if (argv[i][0] != '-') {
            if (snap_path) {
                jash_writef(jash_stderr_writer, 0, "proc restore: too many arguments\n");
                return 2;
            }
            snap_path = argv[i];
        } else {
            jash_writef(jash_stderr_writer, 0, "proc restore: unknown flag '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!snap_path) {
        jash_writef(jash_stderr_writer, 0, "proc restore: missing snapshot path\n");
        return 2;
    }

    int new_pid;
    if (jash_timeline_proc_restore(snap_path, &new_pid) != 0) {
        jash_writef(jash_stderr_writer, 0, "proc restore: failed to restore from %s\n", snap_path);
        return 1;
    }

    if (json_mode) {
        jash_writef(w, ctx, "{\"restored_pid\":%d,\"snapshot\":\"%s\"}\n", new_pid, snap_path);
    } else {
        jash_writef(w, ctx, "proc restore: started pid %d from %s\n", new_pid, snap_path);
    }
    return 0;
}

/* proc tree [PID] [--json] */
int jash_b_proc_tree(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int target_pid = 0;
    int json_mode = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) {
            json_mode = 1;
        } else if (argv[i][0] != '-') {
            target_pid = parse_pid_arg(argv[i]);
        } else {
            jash_writef(jash_stderr_writer, 0, "proc tree: unknown flag '%s'\n", argv[i]);
            return 2;
        }
    }

    char json[65536];
    if (jash_timeline_proc_tree(target_pid, json, sizeof json) != 0) {
        jash_writef(jash_stderr_writer, 0, "proc tree: failed to capture pid %d\n", target_pid);
        return 1;
    }

    if (json_mode || 1) {  /* always JSON for now */
        jash_writef(w, ctx, "%s\n", json);
    } else {
        jash_writef(w, ctx, "%s\n", json);
    }
    return 0;
}

/* proc diff <snapA> <snapB> [--json] */
int jash_b_proc_diff(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    const char* snap_a = NULL;
    const char* snap_b = NULL;
    int json_mode = 0;

    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--json")) {
            json_mode = 1;
        } else if (argv[i][0] != '-') {
            if (!snap_a) snap_a = argv[i];
            else if (!snap_b) snap_b = argv[i];
            else {
                jash_writef(jash_stderr_writer, 0, "proc diff: too many arguments\n");
                return 2;
            }
        } else {
            jash_writef(jash_stderr_writer, 0, "proc diff: unknown flag '%s'\n", argv[i]);
            return 2;
        }
    }

    if (!snap_a || !snap_b) {
        jash_writef(jash_stderr_writer, 0, "proc diff: missing snapshot paths (need two)\n");
        return 2;
    }

    char json[4096];
    if (jash_timeline_proc_diff(snap_a, snap_b, json, sizeof json) != 0) {
        jash_writef(jash_stderr_writer, 0, "proc diff: failed to diff snapshots\n");
        return 1;
    }

    if (json_mode || 1) {
        jash_writef(w, ctx, "%s\n", json);
    } else {
        jash_writef(w, ctx, "%s\n", json);
    }
    return 0;
}

/* Main proc dispatcher */
int jash_b_proc(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode = 0;
    if (jash_discovery_format(argc, argv, &mode)) return proc_native_dispatch(s, w, ctx, argc, argv);
    if (argc < 1) {
        jash_writef(jash_stderr_writer, 0,
            "usage: proc snapshot|restore|tree|diff ...\n");
        return 2;
    }

    const char* sub = argv[0];
    if (!strcmp(sub, "snapshot")) {
        return jash_b_proc_snapshot(s, w, ctx, argc - 1, argv + 1);
    } else if (!strcmp(sub, "restore")) {
        return jash_b_proc_restore(s, w, ctx, argc - 1, argv + 1);
    } else if (!strcmp(sub, "tree")) {
        return jash_b_proc_tree(s, w, ctx, argc - 1, argv + 1);
    } else if (!strcmp(sub, "diff")) {
        return jash_b_proc_diff(s, w, ctx, argc - 1, argv + 1);
    } else {
        jash_writef(jash_stderr_writer, 0, "proc: unknown subcommand '%s'\n", sub);
        return 2;
    }
}

/* Structured process operations use native observations and explicit receipts. */
#ifndef JASH_AGENT_ONLY
static int proc_b64_digit(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    return c == '+' ? 62 : c == '/' ? 63 : -1;
}
static char* proc_argument(const jdv_value_t* value) {
    if (!value) return NULL;
    if (value->kind == JDV_STRING) {
        if (memchr(value->as.slice.data, 0, value->as.slice.len)) return NULL;
        char* text = malloc(value->as.slice.len + 1);
        if (text) { memcpy(text, value->as.slice.data, value->as.slice.len); text[value->as.slice.len] = 0; }
        return text;
    }
    const jdv_value_t* encoding = jash_discovery_field(value, "encoding");
    const jdv_value_t* data = jash_discovery_field(value, "data");
    if (value->kind != JDV_RECORD || value->as.record.len != 2 || !encoding || encoding->kind != JDV_STRING ||
        encoding->as.slice.len != 6 || memcmp(encoding->as.slice.data, "base64", 6) || !data || data->kind != JDV_STRING || data->as.slice.len % 4) return NULL;
    char* text = malloc(data->as.slice.len / 4 * 3 + 1);
    if (!text) return NULL;
    size_t used = 0;
    for (size_t i = 0; i < data->as.slice.len; i += 4) {
        const unsigned char* p = data->as.slice.data + i;
        int a = proc_b64_digit(p[0]), b = proc_b64_digit(p[1]);
        int c = p[2] == '=' ? 0 : proc_b64_digit(p[2]), d = p[3] == '=' ? 0 : proc_b64_digit(p[3]);
        if (a < 0 || b < 0 || c < 0 || d < 0 || (p[2] == '=' && p[3] != '=') ||
            ((p[2] == '=' || p[3] == '=') && i + 4 != data->as.slice.len) ||
            (p[2] == '=' && (b & 15)) || (p[3] == '=' && (c & 3))) { free(text); return NULL; }
        text[used++] = (char)((a << 2) | (b >> 4));
        if (p[2] != '=') text[used++] = (char)((b << 4) | (c >> 2));
        if (p[3] != '=') text[used++] = (char)((c << 6) | d);
    }
    if (memchr(text, 0, used)) { free(text); return NULL; }
    text[used] = 0; return text;
}
#endif
static int proc_spawn_snapshot(jash_state_t* state, jash_discovery_rows_t* rows,
        const char* path, int* new_pid, int* exec_error) {
#ifdef JASH_AGENT_ONLY
    (void)state; (void)rows; (void)path; (void)new_pid;
    *exec_error = ENOTSUP; return -1; /* strict local sessions advertise background=false */
#else
    (void)state;
    unsigned char* bytes; size_t count;
    if (proc_bytes(path, &bytes, &count, 1)) { *exec_error = errno; return -1; }
    jdv_value_t document;
    int parsed = jdv_json_parse(&rows->arena, bytes, count, &JDV_DEFAULT_LIMITS, &document);
    free(bytes);
    const jdv_value_t* version = parsed ? NULL : jash_discovery_field(&document, "version");
    if (!version || !((version->kind == JDV_I64 && (version->as.i64 == 2 || version->as.i64 == 3)) ||
        (version->kind == JDV_U64 && (version->as.u64 == 2 || version->as.u64 == 3)))) {
        *exec_error = EINVAL; return -1;
    }
    const jdv_value_t* tree = parsed ? NULL : jash_discovery_field(&document, "tree");
    const jdv_value_t* args = tree ? jash_discovery_field(tree, "argv") : NULL;
    if (!args || args->kind != JDV_LIST || !args->as.list.len || args->as.list.len > 256) { *exec_error = EINVAL; return -1; }
    char* arguments[257] = {0};
    for (size_t i = 0; i < args->as.list.len; i++) {
        arguments[i] = proc_argument(&args->as.list.items[i]);
        if (!arguments[i]) { *exec_error = EINVAL; goto finished; }
    }
    if (!arguments[0][0] || (args->as.list.len >= 2 && !strcmp(arguments[0], "proc") &&
        (!strcmp(arguments[1], "snapshot") || !strcmp(arguments[1], "restore")))) { *exec_error = EINVAL; goto finished; }
    int channel[2];
    if (pipe(channel)) { *exec_error = errno; goto finished; }
    if (fcntl(channel[1], F_SETFD, FD_CLOEXEC)) { *exec_error = errno; close(channel[0]); close(channel[1]); goto finished; }
    pid_t pid = fork();
    if (pid < 0) { *exec_error = errno; close(channel[0]); close(channel[1]); goto finished; }
    if (!pid) {
        close(channel[0]); (void)setpgid(0, 0);
        int input = open("/dev/null", O_RDONLY);
        if (input >= 0) { (void)dup2(input, STDIN_FILENO); if (input > 2) close(input); }
        if (dup2(STDERR_FILENO, STDOUT_FILENO) >= 0) execvp(arguments[0], arguments);
        int error = errno; ssize_t n;
        do { n = write(channel[1], &error, sizeof error); } while (n < 0 && errno == EINTR);
        _exit(127);
    }
    close(channel[1]); (void)setpgid(pid, pid);
    int error = 0; ssize_t n;
    do { n = read(channel[0], &error, sizeof error); } while (n < 0 && errno == EINTR);
    close(channel[0]);
    if (n) { *exec_error = n < 0 ? errno : error ? error : EIO; (void)kill(pid, SIGKILL); (void)waitpid(pid, NULL, 0); }
    else {
        *new_pid = pid;
        char description[128]; snprintf(description, sizeof description, "proc restore pid=%d", (int)pid);
        jash_timeline_record(JASH_UNDO_PROC_RESTORE, description, path, "");
    }
finished:
    for (size_t i = 0; i < args->as.list.len; i++) free(arguments[i]);
    return *new_pid > 0 ? 0 : -1;
#endif
}
static int proc_native_dispatch(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode, format = jash_discovery_format(argc, argv, &mode);
    if (format < 0) return jash_discovery_error(writer, context, "proc", mode, "conflicting_formats", 2);
    const char* operation = NULL; const char* operands[2] = {0}; const char* output = NULL;
    int count = 0, ended = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && (!strcmp(argv[i], "--output") || !strcmp(argv[i], "-o"))) {
            if (output || ++i == argc) return jash_discovery_error(writer, context, "proc", mode, "invalid_output", 2);
            output = argv[i];
        } else if (!operation) operation = argv[i];
        else if (count < 2) operands[count++] = argv[i];
        else return jash_discovery_error(writer, context, "proc", mode, "invalid_operands", 2);
    }
    int snapshot = operation && !strcmp(operation, "snapshot"), tree = operation && !strcmp(operation, "tree");
    int restore = operation && !strcmp(operation, "restore"), diff = operation && !strcmp(operation, "diff");
    if ((!snapshot && !tree && !restore && !diff) || (output && !snapshot) ||
        ((snapshot || tree) && count > 1) || (restore && count != 1) || (diff && count != 2))
        return jash_discovery_error(writer, context, "proc", mode, "invalid_arguments", 2);
    int pid = (int)getpid();
    if ((snapshot || tree) && count && proc_exact_pid(operands[0], &pid))
        return jash_discovery_error(writer, context, "proc", mode, "invalid_pid", 2);
    if (tree) return jash_process_tree_report(state, writer, context, "proc", mode == JASH_DISCOVERY_DATA, pid);
#ifdef JASH_AGENT_ONLY
    if (restore) return jash_discovery_error(writer, context, "proc", mode, "background_not_supported_in_profile", 2);
#endif
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "proc", mode, "allocation_failed", 125);
    jdv_value_t observed = jdv_value_nothing();
    char generated[PROC_SNAP_DIR_MAX];
    jash_structured_buffer_t encoded; jash_structured_buffer_init(&encoded, 32u * 1024u * 1024u);
    int status = 0, error_number = 0, new_pid = 0, applied = 0;
    const char* error = NULL;
    if (snapshot) {
        if (proc_collect_snapshot(&rows, pid, &observed)) error = "process_observation_failed";
        if (!output && !error) {
            char directory[PROC_SNAP_DIR_MAX];
            if (!proc_snap_dir(directory, sizeof directory)) error = "home_unset";
            else {
                int n = snprintf(generated, sizeof generated, "%s/proc-%ld-%d.json", directory, (long)time(NULL), pid);
                if (n < 0 || (size_t)n >= sizeof generated) error = "snapshot_path_limit";
                else output = generated;
            }
        }
        if (!error) {
            jash_json_writer_t json; jash_jsonw_init(&json, jash_structured_buffer_sink, &encoded);
            if (jdv_json_write(&json, &observed, &JDV_DEFAULT_LIMITS) || jash_jsonw_finish(&json) || encoded.error || encoded.used != encoded.required) error = "snapshot_encoding_limit";
        }
    } else if (diff) {
        unsigned char *left = NULL, *right = NULL; size_t left_size = 0, right_size = 0;
        if (proc_bytes(operands[0], &left, &left_size, 2) || proc_bytes(operands[1], &right, &right_size, 2)) error = "snapshot_read_failed";
        else {
            jdv_record_input_t fields[] = {
                {JDV_LITERAL("snap_a"), jash_discovery_path(&rows, operands[0])}, {JDV_LITERAL("snap_b"), jash_discovery_path(&rows, operands[1])},
                {JDV_LITERAL("size_a"), jdv_value_filesize(left_size)}, {JDV_LITERAL("size_b"), jdv_value_filesize(right_size)},
                {JDV_LITERAL("identical"), jdv_value_bool(left_size == right_size && !memcmp(left, right, left_size))},
                {JDV_LITERAL("comparison"), jash_discovery_string(&rows, "byte-exact")}
            };
            observed = proc_record(&rows, fields, sizeof fields / sizeof *fields);
        }
        free(left); free(right);
    }
    if (!error && rows.error) error = "collection_limit";
    if (error) {
        status = jash_discovery_error(writer, context, "proc", mode, error, 1);
        jash_structured_buffer_free(&encoded); jash_discovery_rows_free(&rows); return status;
    }
    jash_structured_output_t checked;
    if (snapshot || restore) {
        if (mode == JASH_DISCOVERY_DATA) {
            int admission = jash_discovery_effects_begin(&rows, writer, context, "proc", mode, "jash.proc.v1");
            if (admission) { jash_structured_buffer_free(&encoded); jash_discovery_rows_free(&rows); return admission; }
        } else {
            if (jash_structured_begin(&checked, writer, context)) { jash_structured_buffer_free(&encoded); jash_discovery_rows_free(&rows); return 125; }
            jash_jsonw_raw(&checked.json, " ");
            if (jash_jsonw_finish(&checked.json)) { jash_structured_buffer_free(&encoded); jash_discovery_rows_free(&rows); return jash_structured_finish(&checked, 125); }
        }
        if (snapshot) {
            if (output == generated) {
                char directory[PROC_SNAP_DIR_MAX], parent[PROC_SNAP_DIR_MAX];
                int n = snprintf(parent, sizeof parent, "%s/.jash", getenv("HOME"));
                if (n < 0 || (size_t)n >= sizeof parent || !proc_snap_dir(directory, sizeof directory) ||
                    ensure_dir(parent) || ensure_dir(directory)) { error_number = errno; error = "snapshot_directory_failed"; }
            }
            if (!error && jash_proc_snapshot_write_atomic(output, encoded.data, encoded.used)) { error_number = errno; error = "snapshot_publish_failed"; }
            else if (!error) {
                applied = 1;
                char description[128]; snprintf(description, sizeof description, "proc snapshot pid=%d", pid);
                jash_timeline_record(JASH_UNDO_PROC_SNAPSHOT, description, "", output);
            }
        } else if (proc_spawn_snapshot(state, &rows, operands[0], &new_pid, &error_number)) error = "restore_failed";
        else applied = 1;
        jdv_record_input_t fields[] = {
            {JDV_LITERAL("schema"), jash_discovery_string(&rows, "jash.proc.v1")},
            {JDV_LITERAL("operation"), jash_discovery_string(&rows, operation)},
            {JDV_LITERAL("snapshot"), jash_discovery_path(&rows, snapshot ? output : operands[0])},
            {JDV_LITERAL("pid"), snapshot ? jdv_value_u64((uint64_t)pid) : jdv_value_nothing()},
            {JDV_LITERAL("restored_pid"), new_pid ? jdv_value_u64((uint64_t)new_pid) : jdv_value_nothing()},
            {JDV_LITERAL("applied"), applied ? jdv_value_bool(1) : error && snapshot ? jdv_value_nothing() : jdv_value_bool(0)},
            {JDV_LITERAL("durable"), jdv_value_bool(snapshot && applied)},
            {JDV_LITERAL("restore_scope"), jash_discovery_string(&rows, "root-argv-only; inherited environment; stdin closed; stdout to stderr")},
            {JDV_LITERAL("error"), jash_discovery_string(&rows, error)},
            {JDV_LITERAL("errno"), error_number ? jdv_value_i64(error_number) : jdv_value_nothing()}
        };
        observed = proc_record(&rows, fields, sizeof fields / sizeof *fields);
        status = error ? 1 : 0;
    }
    if (rows.error) status = 75;
    else rows.items[rows.count++] = observed;
    if (mode == JASH_DISCOVERY_DATA) {
        if (rows.effects) status = jash_discovery_effects_end(&rows, status);
        else { static const jdv_schema_t schema = {.kind=JDV_SCHEMA_ANY}; status = query_data_emit(writer, context, &schema, rows.items, rows.count, 0, status, status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE, NULL); }
    } else if (snapshot || restore) {
        if (rows.error || jdv_json_write(&checked.json, &observed, &JDV_DEFAULT_LIMITS)) {
            if (!checked.json.error) jash_jsonw_raw(&checked.json, "{\"complete\":false,\"error\":{\"code\":\"collection_limit_exceeded\",\"effects_may_have_applied\":true}}");
            status = 75;
        }
        jash_jsonw_raw(&checked.json, "\n"); status = jash_structured_finish(&checked, status);
    } else status = value_output_json(writer, context, "proc", &observed, &JDV_DEFAULT_LIMITS, 32u * 1024u * 1024u, status);
    jash_structured_buffer_free(&encoded); jash_discovery_rows_free(&rows); return status;
}
