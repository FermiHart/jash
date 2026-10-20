// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Named commands execute in isolated child shells. A bounded in-memory prefix
 * and an exact observed-byte count replace the old unbounded tempfile capture. */
static long weave_fd_writer(void* context, const char* bytes, size_t size) {
    (void)context;
    ssize_t written;
    do { written = write(STDOUT_FILENO, bytes, size); } while (written < 0 && errno == EINTR);
    return written < 0 ? -1 : (long)written;
}

static int weave_execute(jash_state_t* state, weave_task_t* task, uint64_t* observed, int* started, int* io_error) {
    int channel[2];
    if (pipe(channel)) { *io_error = errno; return 125; }
    pid_t child = fork();
    if (child < 0) { *io_error = errno; close(channel[0]); close(channel[1]); return 125; }
    if (!child) {
        close(channel[0]);
        if (setpgid(0, 0)) _exit(125);
        if (dup2(channel[1], STDOUT_FILENO) < 0 || dup2(channel[1], STDERR_FILENO) < 0) _exit(125);
        close(channel[1]);
        int rc = jash_eval_line(state, weave_fd_writer, NULL, task->cmd);
        _exit(rc >= 0 && rc <= 255 ? rc : 125);
    }
    *started = 1; close(channel[1]);
    unsigned char block[8192];
    for (;;) {
        ssize_t n = read(channel[0], block, sizeof block);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (n < 0) *io_error = errno; break; }
        if ((uint64_t)n > UINT64_MAX - *observed) { *io_error = EOVERFLOW; break; }
        *observed += (uint64_t)n;
        size_t retained = (size_t)n;
        if (retained > sizeof(task->out) - task->out_len) retained = sizeof(task->out) - task->out_len;
        memcpy(task->out + task->out_len, block, retained); task->out_len += retained;
    }
    if (close(channel[0]) && !*io_error) *io_error = errno;
    /* A broken capture cannot leave a child that ignores TERM blocking this
     * error path forever. The isolated group also contains its descendants. */
    if (*io_error) {
        (void)kill(-child, SIGKILL);
        (void)kill(child, SIGKILL);
    }
    int status = 0; pid_t waited;
    do { waited = waitpid(child, &status, 0); } while (waited < 0 && errno == EINTR);
    if (waited != child) { if (!*io_error) *io_error = errno; return 125; }
    task->out_truncated = *observed > task->out_len;
    return *io_error ? 125 : WIFEXITED(status) ? WEXITSTATUS(status) : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 125;
}

static const char* weave_string_end(const char* start) {
    const char* p = start + 1;
    while (*p) {
        if (*p == '\\') { if (!p[1]) return NULL; p += 2; }
        else if (*p++ == '"') return p;
    }
    return NULL;
}

static int weave_tasks_parse(jash_discovery_rows_t* rows, const char* json, weave_task_t* tasks, size_t* count) {
    size_t length = strlen(json);
    jdv_value_t document;
    if (length > 65536 || jdv_json_parse(&rows->arena, json, length, &JDV_DEFAULT_LIMITS, &document) ||
        document.kind != JDV_RECORD || !document.as.record.len || document.as.record.len > WEAVE_MAX_TASKS) return -1;
    const char* cursor = weave_skip_ws(json) + 1;
    /* The canonical JDV record is sorted. Read key order from the validated
     * source so execution still follows the caller's declaration order. */
    for (size_t i = 0; i < document.as.record.len; i++) {
        cursor = weave_skip_ws(cursor);
        const char* end = weave_string_end(cursor);
        jdv_value_t key;
        if (!end || jdv_json_parse(&rows->arena, cursor, (size_t)(end - cursor), &JDV_DEFAULT_LIMITS, &key) ||
            key.kind != JDV_STRING || !key.as.slice.len || key.as.slice.len >= WEAVE_NAME_CAP ||
            memchr(key.as.slice.data, 0, key.as.slice.len)) return -1;
        memcpy(tasks[i].name, key.as.slice.data, key.as.slice.len); tasks[i].name[key.as.slice.len] = 0;
        const jdv_value_t* command = jash_discovery_field(&document, tasks[i].name);
        if (!command || command->kind != JDV_STRING || !command->as.slice.len || command->as.slice.len >= WEAVE_CMD_CAP ||
            memchr(command->as.slice.data, 0, command->as.slice.len)) return -1;
        memcpy(tasks[i].cmd, command->as.slice.data, command->as.slice.len); tasks[i].cmd[command->as.slice.len] = 0;
        cursor = weave_skip_ws(end); if (*cursor++ != ':') return -1;
        cursor = weave_skip_ws(cursor); end = weave_string_end(cursor);
        if (!end) return -1;
        cursor = weave_skip_ws(end);
        if (*cursor == ',') cursor++;
    }
    *count = document.as.record.len; return 0;
}

int jash_b_weave_argv(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode = argc && (!strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2")) ? JASH_DISCOVERY_DATA :
        argc && !strcmp(argv[0], "--json") ? JASH_DISCOVERY_JSON : 0;
    if (!mode) {
        /* Keep the established human bare-object syntax. */
        size_t size = 1;
        for (int i = 0; i < argc; i++) { if (strlen(argv[i]) > SIZE_MAX - size - 1) return 2; size += strlen(argv[i]) + 1; }
        char* text = malloc(size); if (!text) return 125;
        text[0] = 0;
        for (int i = 0; i < argc; i++) { if (i) strcat(text, " "); strcat(text, argv[i]); }
        int result = jash_b_weave(state, writer, context, text); free(text); return result;
    }
    if (argc != 2) return jash_discovery_error(writer, context, "weave", mode, "one_task_object_required", 2);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(writer, context, "weave", mode, "allocation_failed", 125);
    weave_task_t* tasks = calloc(WEAVE_MAX_TASKS, sizeof(*tasks)); size_t count = 0;
    if (!tasks || weave_tasks_parse(&rows, argv[1], tasks, &count)) {
        int status = jash_discovery_error(writer, context, "weave", mode, tasks ? "invalid_task_object_or_limit" : "allocation_failed", tasks ? 2 : 125);
        free(tasks); jash_discovery_rows_free(&rows); return status;
    }
    int status = 0, failed = 0; size_t completed = 0;
    jash_structured_output_t output = {0};
    if (mode == JASH_DISCOVERY_DATA) status = jash_discovery_effects_begin(&rows, writer, context, "weave", mode, "jash.weave.task.v1");
    else {
        status = jash_structured_begin(&output, writer, context) ? 125 : 0;
        if (!status) { jash_jsonw_raw(&output.json, "{\"results\":["); if (jash_jsonw_finish(&output.json)) status = 125; }
    }
    for (size_t i = 0; !status && i < count; i++) {
        uint64_t observed = 0; int started = 0, io_error = 0;
        tasks[i].rc = weave_execute(state, &tasks[i], &observed, &started, &io_error);
        if (tasks[i].rc) failed++;
        jdv_value_t bytes = jdv_value_nothing();
        if (jash_json_utf8_valid_n(tasks[i].out, tasks[i].out_len)) {
            if (jdv_value_string(&rows.arena, tasks[i].out, tasks[i].out_len, &bytes)) rows.error = 1;
        } else if (jdv_value_bytes(&rows.arena, tasks[i].out, tasks[i].out_len, &bytes)) rows.error = 1;
        jdv_record_input_t fields[] = {
            {JDV_LITERAL("name"), jash_discovery_string(&rows, tasks[i].name)},
            {JDV_LITERAL("cmd"), jash_discovery_string(&rows, tasks[i].cmd)},
            {JDV_LITERAL("rc"), jdv_value_i64(tasks[i].rc)}, {JDV_LITERAL("out"), bytes},
            {JDV_LITERAL("truncated"), jdv_value_bool(tasks[i].out_truncated)},
            {JDV_LITERAL("out_bytes"), jdv_value_filesize(observed)},
            {JDV_LITERAL("retained_bytes"), jdv_value_filesize(tasks[i].out_len)},
            {JDV_LITERAL("started"), jdv_value_bool(started)},
            {JDV_LITERAL("capture_errno"), io_error ? jdv_value_i64(io_error) : jdv_value_nothing()},
            {JDV_LITERAL("output_scope"), jash_discovery_string(&rows, "combined-stdout-stderr-prefix")}
        };
        jash_discovery_add(&rows, fields, sizeof fields / sizeof *fields); completed++;
        if (rows.error) status = 75;
        else if (mode == JASH_DISCOVERY_DATA) status = jash_discovery_effects_flush(&rows);
        else {
            if (i) jash_jsonw_raw(&output.json, ",");
            if (jdv_json_write(&output.json, &rows.items[rows.count - 1], &JDV_DEFAULT_LIMITS) || jash_jsonw_finish(&output.json)) status = 125;
        }
    }
    if (mode == JASH_DISCOVERY_DATA) {
        if (rows.effects) status = jash_discovery_effects_end(&rows, status ? status : failed ? 1 : 0);
    } else if (output.restore_sigpipe) {
        if (!output.json.error) {
            jash_jsonw_raw(&output.json, "],\"status\":"); jash_jsonw_string(&output.json, status || failed ? "failed" : "completed");
            jash_jsonw_raw(&output.json, ",\"tasks\":"); jash_jsonw_safe_u64(&output.json, count);
            jash_jsonw_raw(&output.json, ",\"failed\":"); jash_jsonw_safe_u64(&output.json, (uint64_t)failed);
            jash_jsonw_raw(&output.json, ",\"completed_tasks\":"); jash_jsonw_safe_u64(&output.json, completed);
            jash_jsonw_raw(&output.json, ",\"complete\":"); jash_jsonw_raw(&output.json, completed == count && !status ? "true" : "false");
            jash_jsonw_raw(&output.json, "}\n");
        }
        status = jash_structured_finish(&output, status ? status : failed ? 1 : 0);
    }
    free(tasks); jash_discovery_rows_free(&rows); return status;
}
