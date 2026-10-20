// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_output.h"
#include "memory_store.h"
#include "memory_receipt.h"

/* A length after the timestamp preserves exact trailing body bytes. Older
 * readers still parse the leading timestamp and the same next two headers. */
static int resume_native_parse(char* bytes, size_t size, rs_t* note) {
    memset(note, 0, sizeof(*note));
    if (memchr(bytes, 0, size)) return -1;
    char* lines[3]; size_t offset = 0;
    for (int i = 0; i < 3; i++) {
        if (offset >= size) return -1;
        lines[i] = bytes + offset;
        char* end = memchr(lines[i], '\n', size - offset);
        if (!end) return -1;
        *end = 0; offset = (size_t)(end - bytes) + 1;
    }
    char* length_field = strchr(lines[0], '\t');
    if (length_field) *length_field++ = 0;
    uint64_t timestamp = 0, declared = 0;
    if (memory_unsigned(lines[0], LONG_MAX, &timestamp) || !timestamp) return -1;
    size_t body = size - offset;
    if (length_field) {
        if (memory_unsigned(length_field, RS_NOTE_MAX - 1, &declared) || declared != body) return -1;
    } else if (body && bytes[size - 1] == '\n') body--; /* historical rs_save separator */
    size_t head_size = strlen(lines[1]), cwd_size = strlen(lines[2]);
    if (body >= sizeof(note->note) || head_size >= sizeof(note->head) ||
        !cwd_size || cwd_size >= sizeof(note->cwd) || lines[2][0] != '/' || strchr(lines[2], '\r')) return -1;
    memcpy(note->note, bytes + offset, body); note->note[body] = 0;
    memcpy(note->head, lines[1], head_size + 1); memcpy(note->cwd, lines[2], cwd_size + 1);
    note->ts = (long)timestamp; note->ok = 1; return 0;
}

static int resume_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = "get"; const char* text = NULL;
    if (argc) {
        if (!strcmp(argv[0], "get") || !strcmp(argv[0], "show")) {
            if (argc != 1) return semantic_error(call, "invalid_arguments", 2);
        } else if (!strcmp(argv[0], "--forget") || !strcmp(argv[0], "forget")) {
            if (argc != 1) return semantic_error(call, "invalid_arguments", 2);
            operation = "forget";
        } else if (!strcmp(argv[0], "set") || !strcmp(argv[0], "--")) {
            if (argc != 2) return semantic_error(call, "invalid_arguments", 2);
            operation = "set"; text = argv[1];
        } else if (argc == 1 && argv[0][0] != '-') { operation = "set"; text = argv[0]; }
        else return semantic_error(call, "invalid_arguments", 2);
    }
    if (text && strlen(text) >= RS_NOTE_MAX) return semantic_error(call, "invalid_arguments", 2);
    char cwd[PATH_MAX] = "";
    if (text && (!getcwd(cwd, sizeof(cwd)) || strchr(cwd, '\n') || strchr(cwd, '\r')))
        return semantic_error(call, "cwd_unavailable_or_unrepresentable", 1);
    int mutating = strcmp(operation, "get") != 0;
    /* Includes the old/new note, cwd and all sixteen resolved watched paths
     * under maximum JSON escaping, even on a failed namespace publication. */
    if (mutating) { int admitted = memory_receipt_admit(call, 512u * 1024u); if (admitted) return admitted; }
    memory_store_t store = {-1, -1}; memory_write_t write_result = {0};
    char* bytes = NULL; size_t size = 0; int exists = 0, status = 0, saved = 0;
    const char* error = NULL; rs_t note = {0};
    if (memory_store_open(&store, NULL, mutating) ||
        memory_store_read(&store, "resume.txt", RS_NOTE_MAX + PATH_MAX + 80u, &bytes, &size, &exists)) {
        saved = errno; error = memory_store_error(saved); status = 1;
    } else if (exists && resume_native_parse(bytes, size, &note)) {
        saved = EINVAL; error = "invalid_store"; status = 1;
    }
    if (!status && text) {
        rs_t proposed = {0}; proposed.ts = (long)time(NULL);
        if (proposed.ts <= 0) { saved = EIO; error = "clock_unavailable"; status = 1; }
        else {
            memcpy(proposed.note, text, strlen(text) + 1); memcpy(proposed.cwd, cwd, strlen(cwd) + 1);
            jash_drift_git_head(cwd, proposed.head); proposed.ok = 1;
            char encoded[RS_NOTE_MAX + PATH_MAX + 80];
            int n = snprintf(encoded, sizeof(encoded), "%ld\t%zu\n%s\n%s\n%s",
                             proposed.ts, strlen(text), proposed.head, proposed.cwd, text);
            if (n < 0 || (size_t)n >= sizeof(encoded)) { saved = E2BIG; error = "store_limit"; status = 1; }
            else {
                write_result = memory_store_publish(&store, "resume.txt", encoded, (size_t)n);
                if (write_result.applied) note = proposed;
                if (write_result.error) { saved = write_result.error; error = memory_store_error(saved); status = 1; }
            }
        }
    } else if (!status && mutating) {
        write_result = memory_store_remove(&store, "resume.txt");
        if (write_result.applied || !write_result.error) memset(&note, 0, sizeof(note));
        if (write_result.error) { saved = write_result.error; error = memory_store_error(saved); status = 1; }
    }
    if (memory_store_close(&store) && !status) { saved = errno; error = "store_close_failed"; status = 1; }
    semantic_value_t* sem = &call->values;
    char head_now[13] = "?";
    if (note.ok) jash_drift_git_head(note.cwd, head_now);
    int head_known = note.ok && note.head[0] && strcmp(note.head, "?") && strcmp(head_now, "?");
    int head_moved = head_known && strcmp(note.head, head_now);
    int stale = !!head_moved, checks_failed = 0;
    char paths[RS_PATHS_MAX + 1][RS_PATH_LEN];
    int mentioned = note.ok ? rs_collect_paths(note.note, paths, RS_PATHS_MAX + 1) : 0;
    int checked = mentioned > RS_PATHS_MAX ? RS_PATHS_MAX : mentioned;
    jdv_value_t files[RS_PATHS_MAX];
    for (int i = 0; i < checked; i++) {
        char resolved[PATH_MAX]; int joined;
        if (paths[i][0] == '/') joined = snprintf(resolved, sizeof(resolved), "%s", paths[i]);
        else joined = snprintf(resolved, sizeof(resolved), "%s%s%s", note.cwd,
                               note.cwd[strlen(note.cwd) - 1] == '/' ? "" : "/", paths[i]);
        int file_error = 0, missing = 0, changed = 0; struct stat st;
        if (joined < 0 || (size_t)joined >= sizeof(resolved)) file_error = ENAMETOOLONG;
        else if (stat(resolved, &st)) {
            if (errno == ENOENT || errno == ENOTDIR) missing = 1;
            else file_error = errno;
        } else changed = (long)st.st_mtime > note.ts;
        if (missing || changed) stale = 1;
        if (file_error) { checks_failed = 1; if (!status) { status = 1; saved = file_error; error = "file_check_failed"; } }
        jdv_record_input_t fields[] = {
            semantic_field("path", semantic_path(sem, paths[i])),
            semantic_field("resolved_path", joined >= 0 && (size_t)joined < sizeof(resolved) ? semantic_path(sem, resolved) : jdv_value_nothing()),
            semantic_field("missing", file_error ? jdv_value_nothing() : jdv_value_bool(missing)),
            semantic_field("changed_since_note", file_error || missing ? jdv_value_nothing() : jdv_value_bool(changed)),
            semantic_field("error", semantic_symbol(sem, file_error ? "stat_failed" : NULL)),
            semantic_field("errno", file_error ? jdv_value_i64(file_error) : jdv_value_nothing())
        };
        files[i] = semantic_record(sem, fields, 6);
    }
    if (!status && stale && !mutating) status = 1;
    const char* unverified[] = {"truth_of_note", "uncited_files", "same_second_content_changes", "complete_path_extraction"};
    jdv_value_t not_verified[5]; size_t unchecked = 0;
    for (size_t i = 0; i < sizeof(unverified) / sizeof(unverified[0]); i++) not_verified[unchecked++] = semantic_symbol(sem, unverified[i]);
    if (!head_known) not_verified[unchecked++] = semantic_symbol(sem, "git_head");
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.resume.v1")),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("note", semantic_text(sem, note.ok ? note.note : NULL)),
        semantic_field("cwd", semantic_path(sem, note.ok ? note.cwd : NULL)),
        semantic_field("written", note.ok ? jdv_value_i64(note.ts) : jdv_value_nothing()),
        semantic_field("age_s", note.ok ? jdv_value_i64((int64_t)time(NULL) - note.ts) : jdv_value_nothing()),
        semantic_field("head_then", semantic_text(sem, note.ok ? note.head : NULL)),
        semantic_field("head_now", semantic_text(sem, note.ok ? head_now : NULL)),
        semantic_field("head_moved", head_known ? jdv_value_bool(head_moved) : jdv_value_nothing()),
        semantic_field("stale", (checks_failed && !stale) || (status && error && !note.ok) ? jdv_value_nothing() : jdv_value_bool(stale)),
        semantic_field("files", semantic_list(sem, files, (size_t)checked)),
        semantic_field("watching_truncated", jdv_value_bool(mentioned > RS_PATHS_MAX)),
        semantic_field("path_detection", semantic_symbol(sem, "heuristic_tokens_up_to_255_bytes")),
        semantic_field("not_verified", semantic_list(sem, not_verified, unchecked)),
        semantic_field("applied", jdv_value_bool(write_result.applied)),
        semantic_field("durable", jdv_value_bool(write_result.durable)),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("errno", saved ? jdv_value_i64(saved) : jdv_value_nothing())
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    free(bytes); return semantic_finish(call, result, status);
}

SEMANTIC_ARGV_WRAPPER(resume, "resume", "jash.resume.v1", resume_native_provider)
