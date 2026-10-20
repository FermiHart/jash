// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_mirror — Command Execution Mirror (ADR-0099)
// F E R M I ∞ H A R T
//
// jash mirror - Reflexão de comandos para referência posterior
//
// Cria um espelho estruturado da sessão de comandos.
// Uso:
//   mirror             - list last execution
//   mirror last        - show last command execution
//   mirror <id>        - show specific execution by ID
// Execution recording is not wired into the evaluator yet. Command execution
// requests must fail visibly rather than return success after printing usage.
//   mirror --json      - JSON output
//   mirror --history   - report automatic execution-history availability

#include <builtins/builtin.h>
#include "semantic_value.h"
#include "discovery_output.h"
#include <core/state.h>
#include <theme/io.h>
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <core/argv.h>
#include <core/json_writer.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include <module/jash_crypto.h>
#include "../../libjef/jef_verify.h"
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <osif/ipc.h>

#define MIRROR_MAX_HISTORY 64

typedef struct {
    char cmd[512];
    int exit_code;
    long duration_ms;
    long timestamp;
    char cwd[512];
} mirror_entry_t;

static mirror_entry_t mirror_history[MIRROR_MAX_HISTORY];
static int mirror_count = 0;
static int mirror_last_idx = 0; /* next insertion slot */

static int mirror_hex(const char* text, size_t length) {
    if (!text || strlen(text) != length) return 0;
    for (size_t i = 0; i < length; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return 0;
    return 1;
}

static int mirror_receipt(jash_writer_t writer, void* context, const char* args) {
    jash_argv_t av = {0};
    if (jash_argv_build(args, &av) < 0) return 2;
    int selected_mode = 0;
    if (jash_discovery_format(av.argc, av.argv, &selected_mode) < 0) {
        jash_argv_free(&av);
        return jash_discovery_error(writer, context, "mirror", selected_mode, "conflicting_formats", 2);
    }
    const char* ref = NULL; const char* expected_hash = NULL; const char* key = NULL;
    const char* request_path = NULL; const char* response_path = NULL;
    const char* stdout_path = NULL; const char* stderr_path = NULL;
    int json = 0;
    for (int i = 0; i < av.argc; i++) {
        if (!strcmp(av.argv[i], "--json") && !json) json = 1;
        else if ((!strcmp(av.argv[i], "--data") || !strcmp(av.argv[i], "--data-v2")) && !json) json = 2;
        else if (!strcmp(av.argv[i], "--receipt") && !ref && i + 1 < av.argc) ref = av.argv[++i];
        else if (!strcmp(av.argv[i], "--blake3") && !expected_hash && i + 1 < av.argc) expected_hash = av.argv[++i];
        else if (!strcmp(av.argv[i], "--pubkey") && !key && i + 1 < av.argc) key = av.argv[++i];
        else if (!strcmp(av.argv[i], "--request") && !request_path && i + 1 < av.argc) request_path = av.argv[++i];
        else if (!strcmp(av.argv[i], "--response") && !response_path && i + 1 < av.argc) response_path = av.argv[++i];
        else if (!strcmp(av.argv[i], "--stdout") && !stdout_path && i + 1 < av.argc) stdout_path = av.argv[++i];
        else if (!strcmp(av.argv[i], "--stderr") && !stderr_path && i + 1 < av.argc) stderr_path = av.argv[++i];
        else {
            jash_argv_free(&av);
            return selected_mode ? jash_discovery_error(writer, context, "mirror", selected_mode, "invalid_receipt_arguments", 2) : 2;
        }
    }
    char identifier[65];
    if (!!request_path != !!response_path || ((stdout_path || stderr_path) && !request_path)) {
        jash_argv_free(&av);
        return selected_mode ? jash_discovery_error(writer, context, "mirror", selected_mode, "unpaired_runtime_inputs", 2) : 2;
    }
    if (!json || !ref || strlen(ref) != 69 || strcmp(ref + 64, ".dsse") ||
        !mirror_hex(expected_hash, 64) || !mirror_hex(key, 64)) {
        jash_argv_free(&av);
        return selected_mode ? jash_discovery_error(writer, context, "mirror", selected_mode, "invalid_receipt_identity", 2) : 2;
    }
    memcpy(identifier, ref, 64); identifier[64] = 0;
    if (!mirror_hex(identifier, 64)) {
        jash_argv_free(&av);
        return jash_discovery_error(writer, context, "mirror", selected_mode, "invalid_receipt_identity", 2);
    }
    int home = -1, state = -1, dir = -1, fd = -1;
    const char* error = "receipt_unavailable";
    const char* home_path = getenv("HOME");
    char bytes[8192]; size_t used = 0; int verified = 0;
    if (!home_path || home_path[0] != '/') goto emit;
    home = open(home_path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (home < 0) goto emit;
    state = openat(home, ".jash", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (state < 0) goto emit;
    dir = openat(state, "agent_receipts", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dir < 0) goto emit;
    fd = openat(dir, ref, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) || !S_ISREG(st.st_mode) || st.st_uid != geteuid() ||
        st.st_size < 1 || (uintmax_t)st.st_size >= sizeof(bytes)) goto emit;
    while (used < (size_t)st.st_size) {
        ssize_t n = read(fd, bytes + used, (size_t)st.st_size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) goto emit;
        used += (size_t)n;
    }
    bytes[used] = 0;
    unsigned char digest[32]; char hash[65];
    jash_blake3(bytes, used, digest); jash_blake3_hex(digest, hash);
    if (strcmp(hash, expected_hash)) { error = "receipt_hash_mismatch"; goto emit; }
    if (jef_verify_proof_bytes_pinned(bytes, (unsigned)used, key)) { error = "signature_or_identity_mismatch"; goto emit; }
    if (request_path) {
        int snapshots[5] = {-1, -1, -1, -1, -1};
        snapshots[0] = jash_ipc_snapshot_bytes(bytes, used);
        snapshots[1] = jash_ipc_snapshot_file(request_path, 32u * 1024u);
        snapshots[2] = jash_ipc_snapshot_file(response_path, 600u * 1024u);
        if (stdout_path) snapshots[3] = jash_ipc_snapshot_file(stdout_path, 16u * 1024u * 1024u);
        if (stderr_path) snapshots[4] = jash_ipc_snapshot_file(stderr_path, 16u * 1024u * 1024u);
        int ready = snapshots[0] >= 0 && snapshots[1] >= 0 && snapshots[2] >= 0 &&
                    (!stdout_path || snapshots[3] >= 0) && (!stderr_path || snapshots[4] >= 0);
        int result = -1;
        if (ready) {
            char receipt_fd[64], request_fd[64], response_fd[64];
            snprintf(receipt_fd, sizeof(receipt_fd), "/proc/self/fd/%d", snapshots[0]);
            snprintf(request_fd, sizeof(request_fd), "/proc/self/fd/%d", snapshots[1]);
            snprintf(response_fd, sizeof(response_fd), "/proc/self/fd/%d", snapshots[2]);
            result = jef_verify_runtime_receipt_with_streams(receipt_fd, response_fd, request_fd, key, ref, snapshots[3], snapshots[4]);
        }
        for (int i = 0; i < 5; i++) if (snapshots[i] >= 0) close(snapshots[i]);
        if (result) { error = ready ? "runtime_binding_failed" : "snapshot_unavailable"; goto emit; }
    }
    verified = 1; error = NULL;
emit:
    if (fd >= 0) close(fd);
    if (dir >= 0) close(dir);
    if (state >= 0) close(state);
    if (home >= 0) close(home);
    semantic_value_t sem;
    if (semantic_value_init(&sem, 65536, 32768)) { jash_argv_free(&av); return 125; }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&sem, "jash.mirror.receipt.v1")),
        semantic_field("receipt_ref", semantic_text(&sem, ref)),
        semantic_field("verified", jdv_value_bool(verified)),
        semantic_field("verification_scope", semantic_symbol(&sem, request_path ?
            "runtime-request-response-and-streams" : "pinned-signature-and-artifact-hash")),
        semantic_field("error", semantic_symbol(&sem, error)),
        semantic_field("receipt", verified ? semantic_text_n(&sem, bytes, used) : jdv_value_nothing())
    };
    jdv_value_t result = semantic_record(&sem, fields, sizeof fields / sizeof *fields);
    jdv_schema_t schema = {.kind=JDV_SCHEMA_RECORD, .as.record={.open=1}};
    int status = verified ? 0 : 1;
    int rc = sem.error ? jash_discovery_error(writer, context, "mirror", json, "collection_limit", 75) :
        json == 2 ? value_output_data(writer, context, &result, 1, NULL, &schema, &sem.limits, 65536, status, error) :
        value_output_json(writer, context, "mirror", &result, &sem.limits, 65536, status);
    semantic_value_free(&sem); jash_argv_free(&av); return rc;
}

static void mirror_record(const char* cmd, int exit_code, long duration_ms) {
    mirror_entry_t* e = &mirror_history[mirror_last_idx];
    strlcpy(e->cmd, cmd, sizeof(e->cmd));
    e->cmd[sizeof(e->cmd) - 1] = '\0';
    e->exit_code = exit_code;
    e->duration_ms = duration_ms;
    e->timestamp = (long)time(NULL);
    if (!getcwd(e->cwd, sizeof(e->cwd))) e->cwd[0] = '\0';
    
    mirror_last_idx = (mirror_last_idx + 1) % MIRROR_MAX_HISTORY;
    if (mirror_count < MIRROR_MAX_HISTORY) mirror_count++;
}

static void mirror_show(jash_writer_t w, void* ctx, int idx) {
    if (idx < 0 || idx >= mirror_count) {
        jash_writes(w, ctx, "mirror: no such execution\n");
        return;
    }
    
    int actual_idx = (mirror_last_idx - mirror_count + idx + MIRROR_MAX_HISTORY) % MIRROR_MAX_HISTORY;
    mirror_entry_t* e = &mirror_history[actual_idx];
    
    jash_writef(w, ctx,
        "\n┌─ Command ─────────────────────────────────────┐\n"
        "│ %-60s │\n"
        "├─ Result ──────────────────────────────────────┤\n"
        "│ Exit Code: %d                               │\n"
        "│ Duration:  %.3fs                            │\n"
        "│ Timestamp: %s                              │\n"
        "│ CWD:       %-50s │\n"
        "└──────────────────────────────────────────────┘\n",
        e->cmd,
        e->exit_code,
        e->duration_ms / 1000.0,
        ctime(&e->timestamp),
        e->cwd);
}

static void mirror_history_show(jash_writer_t w, void* ctx, int json) {
    if (json) {
        jash_writes(w, ctx, "[\n");
        for (int i = 0; i < mirror_count; i++) {
            int actual_idx = (mirror_last_idx - mirror_count + i + MIRROR_MAX_HISTORY) % MIRROR_MAX_HISTORY;
            mirror_entry_t* e = &mirror_history[actual_idx];
            jash_writef(w, ctx,
                "  {\"id\":%d,\"cmd\":\"%s\",\"exit_code\":%d,\"duration_ms\":%ld,\"timestamp\":%ld,\"cwd\":\"%s\"}%s\n",
                i, e->cmd, e->exit_code, e->duration_ms, e->timestamp, e->cwd,
                i < mirror_count - 1 ? "," : "");
        }
        jash_writes(w, ctx, "]\n");
        return;
    }
    
    jash_writef(w, ctx, "\nExecution History (%d entries)\n", mirror_count);
    jash_writef(w, ctx, "%-4s %-8s %-10s %-10s %s\n",
                "ID", "EXIT", "DUR(ms)", "TIMESTAMP", "COMMAND");
    jash_writef(w, ctx, "%.*s\n", 70, "----------------------------------------------------------------------");
    
    for (int i = 0; i < mirror_count; i++) {
        int actual_idx = (mirror_last_idx - mirror_count + i + MIRROR_MAX_HISTORY) % MIRROR_MAX_HISTORY;
        mirror_entry_t* e = &mirror_history[actual_idx];
        char time_str[32];
        struct tm* tm_info = localtime(&e->timestamp);
        strftime(time_str, sizeof(time_str), "%H:%M:%S", tm_info);
        
        jash_writef(w, ctx, "%-4d %-8d %-10ld %-10s %s\n",
                    i, e->exit_code, e->duration_ms, time_str, e->cmd);
    }
}

static void mirror_history_unavailable(jash_writer_t w, void* ctx, int json) {
    if (json) {
        jash_writes(w, ctx,
            "{\"schema\":\"jash.mirror.history.v1\",\"available\":false,"
            "\"recording\":false,\"complete\":false,\"entries\":[],"
            "\"error\":\"execution_recording_unavailable\"}\n");
    } else {
        jash_writes(w, ctx,
            "mirror: automatic execution recording is not connected; history is unavailable\n");
    }
}

static int mirror_native_history(jash_writer_t writer, void* context, int mode, int argc, char** argv) {
    if (argc > 1 || (argc && strcmp(argv[0], "--history") && strcmp(argv[0], "last")))
        return jash_discovery_error(writer, context, "mirror", mode, "execution_recording_unavailable", 2);
    semantic_value_t sem;
    if (semantic_value_init(&sem, 16384, 8192)) return 125;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&sem, "jash.mirror.history.v1")),
        semantic_field("available", jdv_value_bool(0)), semantic_field("recording", jdv_value_bool(0)),
        semantic_field("complete", jdv_value_bool(0)), semantic_field("entries", semantic_list(&sem, NULL, 0)),
        semantic_field("error", semantic_symbol(&sem, "execution_recording_unavailable"))
    };
    jdv_value_t value = semantic_record(&sem, fields, 6);
    jdv_schema_t schema = {.kind=JDV_SCHEMA_RECORD, .as.record={.open=1}};
    int rc = mode == 2 ? value_output_data(writer, context, &value, 1, NULL, &schema, &sem.limits, 16384, 0, NULL) :
        value_output_json(writer, context, "mirror", &value, &sem.limits, 16384, 0);
    semantic_value_free(&sem); return rc;
}

int jash_b_mirror(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;
    if (args && strstr(args, "--receipt")) return mirror_receipt(w, ctx, args);
    jash_argv_t structured;
    if (jash_argv_build(args ? args : "", &structured) < 0) return 2;
    if (structured.argc && (!strcmp(structured.argv[0], "--json") ||
        !strcmp(structured.argv[0], "--data") || !strcmp(structured.argv[0], "--data-v2"))) {
        int mode = !strcmp(structured.argv[0], "--json") ? 1 : 2;
        int rc = mirror_native_history(w, ctx, mode, structured.argc - 1, structured.argv + 1);
        jash_argv_free(&structured); return rc;
    }
    jash_argv_free(&structured);
    
    int json_out = 0;
    int show_history = 0;
    int target_idx = -1;
    const char* cmd_to_run = NULL;
    
    if (args) {
        const char* a = args;
        while (*a == ' ' || *a == '\t') a++;
        
        if (strncmp(a, "--json", 6) == 0) {
            json_out = 1;
            a += 6;
        }
        while (*a == ' ' || *a == '\t') a++;
        if (strncmp(a, "--history", 9) == 0) {
            show_history = 1;
            a += 9;
        }
        
        while (*a == ' ') a++;
        
        if (*a == '\0') {
            if (mirror_count > 0) {
                target_idx = mirror_count - 1;
            }
        } else if (strncmp(a, "last", 4) == 0) {
            if (mirror_count > 0) {
                target_idx = mirror_count - 1;
            }
            a += 4;
            while (*a == ' ') a++;
        } else if (isdigit((unsigned char)*a)) {
            target_idx = atoi(a);
        } else {
            cmd_to_run = args;
            const char* p = args;
            while (*p && *p != ' ') p++;
            if (*p) cmd_to_run = p + 1;
        }
    }
    
    if (show_history) {
        mirror_history_unavailable(w, ctx, json_out);
        return 0;
    }

    if (json_out && !cmd_to_run) {
        mirror_history_show(w, ctx, json_out);
        return 0;
    }
    
    if (target_idx >= 0) {
        mirror_show(w, ctx, target_idx);
        return 0;
    }
    
    if (cmd_to_run) {
        jash_writes(jash_stderr_writer, NULL,
            "mirror: command execution/recording is not implemented; nothing was executed\n");
        return 2;
    }
    
    jash_writes(w, ctx, "usage: mirror <id|last|--history|--json>\n"
                       "Receipt verification is separate; automatic execution history is unavailable.\n");
    return 0;
}

__attribute__((used)) static void mirror_record_execution(const char* cmd, int exit_code, long duration_ms) {
    mirror_record(cmd, exit_code, duration_ms);
}

int jash_b_mirror_argv(jash_state_t* s, jash_writer_t writer, void* context, int argc, char** argv) {
    char* encoded = jash_argv_join(argc, argv);
    if (!encoded) return 125;
    int result = jash_b_mirror(s, writer, context, encoded);
    free(encoded); return result;
}

__attribute__((used)) static int mirror_get_last(char* cmd_buf, size_t buf_sz) {
    if (mirror_count == 0) return 0;
    int actual_idx = (mirror_last_idx - mirror_count + 1 + MIRROR_MAX_HISTORY) % MIRROR_MAX_HISTORY;
    strncpy(cmd_buf, mirror_history[actual_idx].cmd, buf_sz - 1);
    cmd_buf[buf_sz - 1] = '\0';
    return 1;
}

__attribute__((used)) static int mirror_get_count(void) {
    return mirror_count;
}
