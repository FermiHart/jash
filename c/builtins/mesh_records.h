// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Mesh's native record model is shared by text, JSON and JTV2 rendering. */
#ifndef JASH_BUILTINS_MESH_RECORDS_H
#define JASH_BUILTINS_MESH_RECORDS_H

#define MESH_WORKERS_MAX 512u
#define MESH_TASKS_MAX 1024u
#define MESH_REP_MAX 256u
#define MESH_POSTS_MAX 4096u
#define MESH_BLACKBOARD_BYTES (2u * 1024u * 1024u)
#define MESH_CHAIN_BYTES (16u * 1024u * 1024u)
#define MESH_LEASE_TTL 600

typedef struct {
    char id[128], provider[128], model[128], status[32], headroom[32], task[256];
    long timestamp, pid;
} mesh_worker_t;
typedef struct {
    int id;
    char state[16], owner[160], tag[32], deps[256], desc[512];
    long claim_ts;
} mesh_task_t;
typedef struct { char model[128], tag[32]; uint64_t done, fail; } mesh_rep_t;

static int mesh_id_valid(const char* id) {
    if (!id || !*id || strlen(id) >= sizeof(((mesh_worker_t*)0)->id) || !strcmp(id, ".") || !strcmp(id, "..")) return 0;
    for (const unsigned char* p = (const unsigned char*)id; *p; p++)
        if (!((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') ||
              (*p >= '0' && *p <= '9') || *p == '-' || *p == '_' || *p == '.')) return 0;
    return 1;
}

static int mesh_deps_valid(const char* deps) {
    if (!deps || !*deps || !strcmp(deps, "-")) return 1;
    if (strlen(deps) >= 256) return 0;
    char copy[256]; memcpy(copy, deps, strlen(deps) + 1);
    char* cursor = copy;
    for (;;) {
        char* comma = strchr(cursor, ','); if (comma) *comma = 0;
        uint64_t value;
        if (jash_mesh_number(cursor, INT_MAX, &value) || !value) return 0;
        if (!comma) break;
        cursor = comma + 1;
    }
    return 1;
}

static int mesh_record_field(char* buffer, size_t capacity, size_t* used, const char* text, int first) {
    if (!first) {
        if (*used + 1 >= capacity) { errno = E2BIG; return -1; }
        buffer[(*used)++] = '\t';
    }
    return jash_mesh_escape(buffer, capacity, used, text);
}

static int mesh_record_end(char* buffer, size_t capacity, size_t* used) {
    static const char ending[] = "\t@jash-mesh-v2\n";
    if (sizeof ending > capacity - *used) { errno = E2BIG; return -1; }
    memcpy(buffer + *used, ending, sizeof ending); *used += sizeof ending - 1; return 0;
}

static int mesh_worker_read(jash_mesh_store_t* store, const char* id, mesh_worker_t* worker) {
    char name[160];
    if (!mesh_id_valid(id)) { errno = EINVAL; return -1; }
    snprintf(name, sizeof name, "%s.tsv", id);
    char* bytes = NULL; size_t length = 0;
    int result = jash_mesh_read_at(store->workers, name, 8192, &bytes, &length);
    if (result <= 0) return result;
    if (!length || bytes[length - 1] != '\n' || memchr(bytes, 0, length) || memchr(bytes, '\n', length - 1)) {
        free(bytes); errno = EINVAL; return -1;
    }
    bytes[length - 1] = 0;
    char* fields[9]; int n = jash_mesh_fields(bytes, fields, 9);
    int encoded = n == 9 && !strcmp(fields[8], "@jash-mesh-v2");
    memset(worker, 0, sizeof *worker);
    uint64_t timestamp = 0, pid = 0;
    int error = n != 7 && n != 8 && !encoded;
    if (!error) error = jash_mesh_unescape(fields[0], worker->id, sizeof worker->id, encoded) ||
        jash_mesh_unescape(fields[1], worker->provider, sizeof worker->provider, encoded) ||
        jash_mesh_unescape(fields[2], worker->model, sizeof worker->model, encoded) ||
        jash_mesh_unescape(fields[3], worker->status, sizeof worker->status, encoded) ||
        jash_mesh_unescape(fields[4], worker->headroom, sizeof worker->headroom, encoded) ||
        jash_mesh_unescape(fields[5], worker->task, sizeof worker->task, encoded) ||
        jash_mesh_number(fields[6], LONG_MAX, &timestamp) ||
        (n >= 8 && jash_mesh_number(fields[7], INT_MAX, &pid)) || strcmp(worker->id, id);
    free(bytes);
    if (error) { errno = EINVAL; return -1; }
    worker->timestamp = (long)timestamp; worker->pid = (long)pid; return 1;
}

static int mesh_worker_write(jash_mesh_store_t* store, const mesh_worker_t* worker, jash_mesh_io_t* outcome) {
    char name[160], buffer[8192], timestamp[32], pid[32]; size_t used = 0;
    snprintf(name, sizeof name, "%s.tsv", worker->id);
    snprintf(timestamp, sizeof timestamp, "%ld", worker->timestamp);
    snprintf(pid, sizeof pid, "%ld", worker->pid);
    const char* values[] = {worker->id, worker->provider, worker->model, worker->status,
        worker->headroom, worker->task, timestamp, pid};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        if (mesh_record_field(buffer, sizeof buffer, &used, values[i], i == 0)) {
            *outcome = (jash_mesh_io_t){.error=errno}; return -1;
        }
    if (mesh_record_end(buffer, sizeof buffer, &used)) { *outcome = (jash_mesh_io_t){.error=errno}; return -1; }
    return jash_mesh_write_at(store->workers, name, buffer, used, outcome);
}

static int mesh_task_read(jash_mesh_store_t* store, int id, mesh_task_t* task) {
    char name[32]; snprintf(name, sizeof name, "%d.tsv", id);
    char* bytes = NULL; size_t length = 0;
    int result = jash_mesh_read_at(store->tasks, name, 8192, &bytes, &length);
    if (result <= 0) return result;
    if (!length || bytes[length - 1] != '\n' || memchr(bytes, 0, length) || memchr(bytes, '\n', length - 1)) {
        free(bytes); errno = EINVAL; return -1;
    }
    bytes[length - 1] = 0;
    char* fields[8]; int n = jash_mesh_fields(bytes, fields, 8);
    int encoded = n == 8 && !strcmp(fields[7], "@jash-mesh-v2");
    memset(task, 0, sizeof *task);
    uint64_t parsed_id = 0, timestamp = 0;
    int error = n != 7 && !encoded;
    if (!error) error = jash_mesh_number(fields[0], INT_MAX, &parsed_id) || parsed_id != (uint64_t)id ||
        jash_mesh_unescape(fields[1], task->state, sizeof task->state, encoded) ||
        jash_mesh_unescape(fields[2], task->owner, sizeof task->owner, encoded) ||
        jash_mesh_number(fields[3], LONG_MAX, &timestamp) ||
        jash_mesh_unescape(fields[4], task->tag, sizeof task->tag, encoded) ||
        jash_mesh_unescape(fields[5], task->deps, sizeof task->deps, encoded) ||
        jash_mesh_unescape(fields[6], task->desc, sizeof task->desc, encoded) ||
        !mesh_deps_valid(task->deps) ||
        (strcmp(task->state, "ready") && strcmp(task->state, "claimed") && strcmp(task->state, "done") && strcmp(task->state, "failed"));
    free(bytes);
    if (error) { errno = EINVAL; return -1; }
    task->id = id; task->claim_ts = (long)timestamp; return 1;
}

static int mesh_task_write(jash_mesh_store_t* store, const mesh_task_t* task, jash_mesh_io_t* outcome) {
    char name[32], buffer[8192], id[32], timestamp[32]; size_t used = 0;
    snprintf(name, sizeof name, "%d.tsv", task->id); snprintf(id, sizeof id, "%d", task->id);
    snprintf(timestamp, sizeof timestamp, "%ld", task->claim_ts);
    const char* values[] = {id, task->state, task->owner, timestamp, task->tag, task->deps, task->desc};
    for (size_t i = 0; i < sizeof values / sizeof *values; i++)
        if (mesh_record_field(buffer, sizeof buffer, &used, values[i], i == 0)) {
            *outcome = (jash_mesh_io_t){.error=errno}; return -1;
        }
    if (mesh_record_end(buffer, sizeof buffer, &used)) { *outcome = (jash_mesh_io_t){.error=errno}; return -1; }
    return jash_mesh_write_at(store->tasks, name, buffer, used, outcome);
}

static int mesh_worker_compare(const void* a, const void* b) {
    return strcmp(((const mesh_worker_t*)a)->id, ((const mesh_worker_t*)b)->id);
}
static int mesh_task_compare(const void* a, const void* b) {
    int left = ((const mesh_task_t*)a)->id, right = ((const mesh_task_t*)b)->id;
    return (left > right) - (left < right);
}

static int mesh_collect(jash_mesh_store_t* store, int tasks, void* records, size_t* count, jash_state_t* state) {
    *count = 0;
    int directory = tasks ? store->tasks : store->workers;
    if (directory < 0) return 0;
    int copy = openat(directory, ".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (copy < 0) return -1;
    DIR* scan = fdopendir(copy);
    if (!scan) { int saved = errno; close(copy); errno = saved; return -1; }
    int error = 0;
    for (;;) {
        int cancelled = state ? jash_query_cancel_poll(state) : 0;
        if (cancelled) { error = ECANCELED; break; }
        errno = 0; struct dirent* entry = readdir(scan);
        if (!entry) { if (errno) error = errno; break; }
        if (entry->d_name[0] == '.') continue;
        size_t length = strlen(entry->d_name);
        if (length < 5 || strcmp(entry->d_name + length - 4, ".tsv")) { error = EINVAL; break; }
        if (*count == (tasks ? MESH_TASKS_MAX : MESH_WORKERS_MAX)) { error = E2BIG; break; }
        char name[256]; memcpy(name, entry->d_name, length - 4); name[length - 4] = 0;
        int result;
        if (tasks) {
            uint64_t id;
            if (jash_mesh_number(name, INT_MAX, &id) || !id || (name[0] == '0' && name[1])) { error = EINVAL; break; }
            result = mesh_task_read(store, (int)id, &((mesh_task_t*)records)[*count]);
        } else result = mesh_worker_read(store, name, &((mesh_worker_t*)records)[*count]);
        if (result <= 0) { error = result ? errno : ESTALE; break; }
        (*count)++;
    }
    if (closedir(scan) && !error) error = errno;
    if (error) { errno = error; return -1; }
    qsort(records, *count, tasks ? sizeof(mesh_task_t) : sizeof(mesh_worker_t), tasks ? mesh_task_compare : mesh_worker_compare);
    return 0;
}

static int mesh_board_next(jash_mesh_store_t* store, int* next) {
    char* bytes = NULL; size_t length = 0; *next = 1;
    int result = jash_mesh_read_at(store->root, "board.meta", 32, &bytes, &length);
    if (result <= 0) return result;
    if (length && bytes[length - 1] == '\n') bytes[--length] = 0;
    uint64_t value;
    int invalid = memchr(bytes, 0, length) || jash_mesh_number(bytes, INT_MAX, &value) || !value;
    free(bytes);
    if (invalid) { errno = EINVAL; return -1; }
    *next = (int)value; return 1;
}

static int mesh_pid_alive(long pid) {
    if (pid <= 0 || pid > INT_MAX) return -1;
    if (!kill((pid_t)pid, 0)) return 1;
    return errno == ESRCH ? 0 : -1;
}

static int mesh_task_dependencies(const mesh_task_t* selected, const mesh_task_t* tasks, size_t count) {
    if (!*selected->deps || !strcmp(selected->deps, "-")) return 1;
    char copy[256]; memcpy(copy, selected->deps, strlen(selected->deps) + 1);
    char* cursor = copy;
    for (;;) {
        char* comma = strchr(cursor, ','); if (comma) *comma = 0;
        uint64_t id; if (jash_mesh_number(cursor, INT_MAX, &id) || !id) return 0;
        int done = 0;
        for (size_t i = 0; i < count; i++) if (tasks[i].id == (int)id) { done = !strcmp(tasks[i].state, "done"); break; }
        if (!done) return 0;
        if (!comma) return 1;
        cursor = comma + 1;
    }
}

static int mesh_token_encode(const char* source, char* target, size_t capacity) {
    static const char hex[] = "0123456789ABCDEF"; size_t used = 0;
    for (const unsigned char* p = (const unsigned char*)source; *p; p++) {
        int escaped = *p <= 32 || *p >= 127 || *p == '%' || *p == '\\';
        size_t length = escaped ? 3u : 1u;
        if (used + length >= capacity) return -1;
        if (escaped) { target[used++] = '%'; target[used++] = hex[*p >> 4]; target[used++] = hex[*p & 15]; }
        else target[used++] = (char)*p;
    }
    target[used] = 0; return 0;
}
static int mesh_hex(unsigned char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'A' && c <= 'F' ? c - 'A' + 10 : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
}
static int mesh_token_decode(const char* source, char* target, size_t capacity, int encoded) {
    size_t used = 0;
    for (const char* p = source; *p && *p != ' ' && *p != '\t' && *p != '\r' && *p != '\n'; p++) {
        unsigned char byte = (unsigned char)*p;
        if (encoded && byte == '%') {
            int high = p[1] ? mesh_hex((unsigned char)p[1]) : -1;
            int low = p[1] && p[2] ? mesh_hex((unsigned char)p[2]) : -1;
            if (high < 0 || low < 0 || !(byte = (unsigned char)(high * 16 + low))) return -1;
            p += 2;
        }
        if (used + 1 >= capacity) return -1;
        target[used++] = (char)byte;
    }
    target[used] = 0; return 0;
}

static int mesh_rep_read(jash_mesh_store_t* store, mesh_rep_t* values, size_t* count, jash_state_t* state) {
    *count = 0;
    if (store->base < 0) return 0;
    char name[160]; const char* key = jash_repo_key();
    int n = key && key[0] != '-' ? snprintf(name, sizeof name, "chain-%s.log", key) : snprintf(name, sizeof name, "chain.log");
    if (n < 0 || (size_t)n >= sizeof name || strchr(name, '/')) { errno = EINVAL; return -1; }
    char* bytes = NULL; size_t length = 0;
    int result = jash_mesh_read_at(store->base, name, MESH_CHAIN_BYTES, &bytes, &length);
    if (result <= 0) return result;
    int error = memchr(bytes, 0, length) || (length && bytes[length - 1] != '\n') ? EINVAL : 0;
    char* cursor = bytes;
    while (!error && *cursor) {
        if (state && jash_query_cancel_poll(state)) { error = ECANCELED; break; }
        char* end = strchr(cursor, '\n');
        if (!end || end - cursor > 65535) { error = E2BIG; break; }
        *end = 0;
        /* Only the actual command field is an event. A command that echoes
         * text containing cmd=mesh-done must not fabricate reputation. */
        const char* field = strstr(cursor, " cmd=");
        const char* event = field ? field + 5 : NULL;
        int done = event && !strncmp(event, "mesh-done: ", 11);
        int failed = event && !strncmp(event, "mesh-fail: ", 11);
        if (done || failed) {
            const char* tag = strstr(event, " tag="), *model = strstr(event, " model=");
            const char* description = strstr(event, " desc=");
            const char* format = strstr(event, " mesh_format=2");
            int encoded = format && (!description || format < description);
            mesh_rep_t parsed = {0};
            if (!tag || !model || (description && (tag > description || model > description)) ||
                mesh_token_decode(tag + 5, parsed.tag, sizeof parsed.tag, encoded) ||
                mesh_token_decode(model + 7, parsed.model, sizeof parsed.model, encoded)) { error = EINVAL; break; }
            if (*parsed.tag && *parsed.model && strcmp(parsed.tag, "-") && strcmp(parsed.model, "?")) {
                size_t index = 0;
                while (index < *count && (strcmp(values[index].tag, parsed.tag) || strcmp(values[index].model, parsed.model))) index++;
                if (index == *count) {
                    if (*count == MESH_REP_MAX) { error = E2BIG; break; }
                    values[(*count)++] = parsed;
                }
                if (done) values[index].done++; else values[index].fail++;
            }
        }
        cursor = end + 1;
    }
    free(bytes);
    if (error) { errno = error; return -1; }
    return 0;
}
#endif
