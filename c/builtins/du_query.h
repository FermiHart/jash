// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Shared bounded filesystem observations for du's structured projections.
 * Legacy JSON retains name/size_kb rows; native records state both byte units
 * and the physical-symlink/per-path hardlink policy explicitly. */
#ifndef JASH_BUILTINS_DU_QUERY_H
#define JASH_BUILTINS_DU_QUERY_H

enum { DU_QUERY_MAX_DEPTH = 64, DU_QUERY_MAX_VISITS = 100000 };
typedef struct {
    uint64_t apparent, allocated, kib;
    int known, complete, error_number;
    const char* error;
} du_measurement_t;
typedef struct {
    char* path;
    du_measurement_t measured;
    uint64_t operand;
    unsigned depth;
    const char* type;
} du_query_row_t;
typedef struct {
    jash_state_t* state;
    du_query_row_t* rows;
    size_t count, capacity, path_bytes;
    uint64_t visits, errors;
    int capped, cancelled, summarize, all, max_depth, cache;
} du_query_t;

/* Structured caching never trusts a directory TTL. A fresh lstat validates
 * every leaf identity and both quantity inputs before reusing its projection;
 * directory totals are recomputed from the current child observations. */
typedef struct { char* path; struct stat metadata; du_measurement_t measured; } du_query_cache_t;
static du_query_cache_t du_query_cache[128];
static size_t du_query_cache_next;

static int du_query_cache_get(const char* path, const struct stat* current, du_measurement_t* measured) {
    for (size_t i = 0; i < 128; i++) {
        const du_query_cache_t* cached = &du_query_cache[i];
        const struct stat* before = &cached->metadata;
        if (cached->path && !strcmp(cached->path, path) && before->st_dev == current->st_dev &&
            before->st_ino == current->st_ino && before->st_mode == current->st_mode &&
            before->st_size == current->st_size && before->st_blocks == current->st_blocks) {
            *measured = cached->measured; return 1;
        }
    }
    return 0;
}

static void du_query_cache_put(const char* path, const struct stat* metadata, du_measurement_t measured) {
    char* owned = strdup(path);
    if (!owned) return;
    du_query_cache_t* cached = &du_query_cache[du_query_cache_next++ % 128u];
    free(cached->path);
    *cached = (du_query_cache_t){owned, *metadata, measured};
}

static const char* du_query_type(mode_t mode) {
    if (S_ISDIR(mode)) return "dir";
    if (S_ISREG(mode)) return "file";
    if (S_ISLNK(mode)) return "symlink";
    if (S_ISFIFO(mode)) return "fifo";
    if (S_ISSOCK(mode)) return "socket";
    if (S_ISBLK(mode)) return "block";
    if (S_ISCHR(mode)) return "char";
    return "other";
}

static void du_query_add(du_query_t* query, const char* path, unsigned depth,
                          uint64_t operand, const char* type, du_measurement_t measured) {
    size_t bytes = strlen(path) + 1u;
    if (query->count >= QUERY_DATA_MAX_ITEMS || bytes > QUERY_DATA_MAX_BYTES / 2u - query->path_bytes) {
        query->capped = 1; return;
    }
    if (query->count == query->capacity) {
        size_t capacity = query->capacity ? query->capacity * 2u : 64u;
        if (capacity > QUERY_DATA_MAX_ITEMS) capacity = QUERY_DATA_MAX_ITEMS;
        du_query_row_t* rows = realloc(query->rows, capacity * sizeof(*rows));
        if (!rows) { query->capped = 1; return; }
        query->rows = rows; query->capacity = capacity;
    }
    char* copied = strdup(path);
    if (!copied) { query->capped = 1; return; }
    query->rows[query->count++] = (du_query_row_t){copied, measured, operand, depth, type};
    query->path_bytes += bytes;
}

static void du_query_fail(du_query_t* query, du_measurement_t* measured,
                           const char* error, int error_number) {
    query->errors++;
    measured->complete = 0;
    if (!measured->error) { measured->error = error; measured->error_number = error_number; }
}

static void du_query_sum(du_query_t* query, du_measurement_t* total,
                          const du_measurement_t* child) {
    if (!child->complete) { total->complete = 0; if (!total->error) total->error = "partial"; }
    if (!child->known) return;
    if (child->apparent > UINT64_MAX - total->apparent ||
        child->allocated > UINT64_MAX - total->allocated || child->kib > UINT64_MAX - total->kib) {
        total->known = 0;
        du_query_fail(query, total, "quantity_overflow", EOVERFLOW);
        return;
    }
    total->apparent += child->apparent;
    total->allocated += child->allocated;
    total->kib += child->kib;
}

static du_measurement_t du_query_walk(du_query_t* query, const char* path,
                                       unsigned depth, uint64_t operand) {
    du_measurement_t measured = {.known = 1, .complete = 1};
    if (query->cancelled || query->capped) { measured.complete = 0; return measured; }
    if ((query->cancelled = jash_query_cancel_poll(query->state))) { measured.complete = 0; return measured; }
    if (++query->visits > DU_QUERY_MAX_VISITS || depth > DU_QUERY_MAX_DEPTH) {
        query->capped = 1; measured.complete = 0; return measured;
    }
    struct stat metadata;
    if (lstat(path, &metadata)) {
        measured.known = 0;
        du_query_fail(query, &measured, "stat_failed", errno);
        du_query_add(query, path, depth, operand, "unknown", measured);
        return measured;
    }
    const char* type = du_query_type(metadata.st_mode);
    int directory = S_ISDIR(metadata.st_mode);
    if (!directory && query->cache && du_query_cache_get(path, &metadata, &measured)) {
        /* Observation inputs were checked above; reuse their exact projection. */
    } else if (metadata.st_size < 0 || metadata.st_blocks < 0 ||
        (uint64_t)metadata.st_blocks > UINT64_MAX / 512u) {
        measured.known = 0;
        du_query_fail(query, &measured, "quantity_overflow", EOVERFLOW);
    } else {
        measured.apparent = (uint64_t)metadata.st_size;
        measured.allocated = (uint64_t)metadata.st_blocks * 512u;
        if (!directory) measured.kib = measured.apparent / 1024u + (measured.apparent % 1024u != 0);
        if (!directory && query->cache) du_query_cache_put(path, &metadata, measured);
    }
    if (directory) {
        DIR* stream = opendir(path);
        if (!stream) du_query_fail(query, &measured, "directory_open_failed", errno);
        else {
            for (;;) {
                if ((query->cancelled = jash_query_cancel_poll(query->state))) { measured.complete = 0; break; }
                errno = 0;
                struct dirent* entry = readdir(stream);
                if (!entry) { if (errno) du_query_fail(query, &measured, "directory_read_failed", errno); break; }
                if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
                char child[PATH_MAX];
                int size = snprintf(child, sizeof(child), "%s%s%s", path,
                    path[0] && path[strlen(path) - 1] == '/' ? "" : "/", entry->d_name);
                if (size < 0 || (size_t)size >= sizeof(child)) {
                    du_query_fail(query, &measured, "path_limit", ENAMETOOLONG); continue;
                }
                du_measurement_t nested = du_query_walk(query, child, depth + 1u, operand);
                du_query_sum(query, &measured, &nested);
                if (query->capped || query->cancelled) { measured.complete = 0; break; }
            }
            if (closedir(stream)) du_query_fail(query, &measured, "directory_close_failed", errno);
        }
    }
    /* The legacy structured view lists direct children. -s selects roots;
     * -a exposes every observed entry. Depth controls reporting, never an
     * unreported underestimate of a subtree's measured quantities. */
    int wanted = query->summarize ? depth == 0 : query->all ? 1 :
        query->max_depth >= 0 ? directory || depth == 0 : depth == 1 || (!directory && depth == 0);
    if (query->max_depth >= 0 && depth > (unsigned)query->max_depth) wanted = 0;
    if (wanted || (!measured.complete && measured.error && strcmp(measured.error, "partial")))
        du_query_add(query, path, depth, operand, type, measured);
    return measured;
}

static int du_query_compare(const void* left, const void* right) {
    const du_query_row_t* a = left;
    const du_query_row_t* b = right;
    if (a->measured.kib != b->measured.kib) return a->measured.kib < b->measured.kib ? 1 : -1;
    int path = strcmp(a->path, b->path);
    return path ? path : a->operand < b->operand ? -1 : a->operand > b->operand;
}

QUERY_DATA_SCALAR(DU_Q_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(DU_Q_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(DU_Q_SIZE, JDV_FILESIZE, 1);
QUERY_DATA_SCALAR(DU_Q_PATH, JDV_PATH, 1);
QUERY_DATA_SCALAR(DU_Q_SYMBOL, JDV_SYMBOL, 0);
QUERY_DATA_SCALAR(DU_Q_ERROR, JDV_SYMBOL, 1);
static const jdv_schema_field_t DU_QUERY_FIELDS[] = {
    QUERY_DATA_FIELD("allocated_bytes", &DU_Q_SIZE),
    QUERY_DATA_FIELD("apparent_bytes", &DU_Q_SIZE),
    QUERY_DATA_FIELD("complete", &DU_Q_BOOL),
    QUERY_DATA_FIELD("depth", &DU_Q_U64),
    QUERY_DATA_FIELD("errno", &DU_Q_U64),
    QUERY_DATA_FIELD("error", &DU_Q_ERROR),
    QUERY_DATA_FIELD("freshness", &DU_Q_SYMBOL),
    QUERY_DATA_FIELD("hardlink_policy", &DU_Q_SYMBOL),
    QUERY_DATA_FIELD("is_total", &DU_Q_BOOL),
    QUERY_DATA_FIELD("name", &DU_Q_PATH),
    QUERY_DATA_FIELD("operand_index", &DU_Q_U64),
    QUERY_DATA_FIELD("path", &DU_Q_PATH),
    QUERY_DATA_FIELD("size_kib", &DU_Q_U64),
    QUERY_DATA_FIELD("symlink_policy", &DU_Q_SYMBOL),
    QUERY_DATA_FIELD("type", &DU_Q_SYMBOL)
};
static const jdv_schema_t DU_QUERY_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {DU_QUERY_FIELDS, 15, 0}
};

static const char* du_query_name(const du_query_row_t* row) {
    const char* slash = row->depth == 1 ? strrchr(row->path, '/') : NULL;
    return slash ? slash + 1 : row->path;
}

static jdv_value_t du_query_value(jdv_record_field_t fields[15],
                                   const du_query_row_t* row, int total) {
    jdv_value_t value = query_data_record(fields, &DU_QUERY_SCHEMA);
    const du_measurement_t* measured = &row->measured;
    if (measured->known) {
        fields[0].value = jdv_value_filesize(measured->allocated);
        fields[1].value = jdv_value_filesize(measured->apparent);
        fields[12].value = jdv_value_u64(measured->kib);
    }
    fields[2].value = jdv_value_bool(measured->complete);
    if (!total) fields[3].value = jdv_value_u64(row->depth);
    if (measured->error_number) fields[4].value = jdv_value_u64((uint64_t)measured->error_number);
    if (measured->error) fields[5].value = query_data_text(JDV_SYMBOL, measured->error);
    fields[6].value = query_data_text(JDV_SYMBOL, "live");
    fields[7].value = query_data_text(JDV_SYMBOL, "per-path");
    fields[8].value = jdv_value_bool(total);
    if (!total) {
        fields[9].value = query_data_text(JDV_PATH, du_query_name(row));
        fields[10].value = jdv_value_u64(row->operand);
        fields[11].value = query_data_text(JDV_PATH, row->path);
    }
    fields[13].value = query_data_text(JDV_SYMBOL, "physical");
    fields[14].value = query_data_text(JDV_SYMBOL, total ? "total" : row->type);
    return value;
}

static void du_query_json_row(jash_json_writer_t* json, const du_query_row_t* row, int total) {
    const du_measurement_t* value = &row->measured;
    jash_jsonw_raw(json, "{\"name\":"); jash_jsonw_string_or_bytes(json, total ? "total" : du_query_name(row));
    jash_jsonw_raw(json, ",\"size_kb\":");
    if (value->known) jash_jsonw_safe_u64(json, value->kib); else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"path\":");
    if (total) jash_jsonw_raw(json, "null"); else jash_jsonw_string_or_bytes(json, row->path);
    jash_jsonw_raw(json, ",\"allocated_bytes\":");
    if (value->known) jash_jsonw_safe_u64(json, value->allocated); else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"apparent_bytes\":");
    if (value->known) jash_jsonw_safe_u64(json, value->apparent); else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"complete\":"); jash_jsonw_raw(json, value->complete ? "true" : "false");
    jash_jsonw_raw(json, ",\"error\":");
    if (value->error) jash_jsonw_string(json, value->error); else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"errno\":");
    if (value->error_number) jash_jsonw_i64(json, value->error_number); else jash_jsonw_raw(json, "null");
    jash_jsonw_raw(json, ",\"is_total\":"); jash_jsonw_raw(json, total ? "true" : "false");
    jash_jsonw_raw(json, ",\"freshness\":\"live\",\"symlink_policy\":\"physical\",\"hardlink_policy\":\"per-path\"}\n");
}

static int du_query_number(const char* text, uint64_t* value) {
    if (!text || !*text) return -1;
    *value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || *value > (UINT64_MAX - (unsigned)(*text - '0')) / 10u) return -1;
        *value = *value * 10u + (unsigned)(*text - '0');
    }
    return 0;
}

static int du_query_mode(int argc, char** argv) {
    int mode = 0;
    for (int i = 0; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--json") && !mode) mode = 1;
        if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) mode = 2;
        if (!strcmp(argv[i], "--top") || !strcmp(argv[i], "--depth") || !strcmp(argv[i], "--max-depth") ||
            !strcmp(argv[i], "-d") || !strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit")) i++;
    }
    return mode;
}

static int du_query_run(jash_state_t* state, jash_writer_t writer, void* context,
                         int argc, char** argv, int native) {
    du_query_t query = {.state = state, .max_depth = -1};
    const char* paths[64];
    size_t path_count = 0;
    int end_options = 0, json_seen = 0, data_seen = 0, grand_total = 0;
    uint64_t offset = 0, limit = UINT64_MAX, top = UINT64_MAX;
#define DU_QUERY_ERROR(code, status) (native ? query_data_error(writer, context, &DU_QUERY_SCHEMA, (code), (status)) : \
    jash_structured_error(writer, context, "du", (code), (status)))
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!end_options && !strcmp(arg, "--")) { end_options = 1; continue; }
        if (!end_options && !strcmp(arg, "--json")) { json_seen = 1; continue; }
        if (!end_options && (!strcmp(arg, "--data") || !strcmp(arg, "--data-v2"))) { data_seen = 1; continue; }
        if (!end_options && (!strcmp(arg, "--human") || !strcmp(arg, "--human-readable"))) continue;
        if (!end_options && !strcmp(arg, "--summarize")) { query.summarize = 1; continue; }
        if (!end_options && !strcmp(arg, "--all")) { query.all = 1; continue; }
        if (!end_options && !strcmp(arg, "--total")) { grand_total = 1; continue; }
        if (!end_options && !strcmp(arg, "--cache")) { query.cache = 1; continue; }
        if (!end_options && (!strcmp(arg, "--offset") || !strcmp(arg, "--limit") || !strcmp(arg, "--top") ||
                             !strcmp(arg, "--depth") || !strcmp(arg, "--max-depth") || !strncmp(arg, "--max-depth=", 12))) {
            uint64_t value;
            const char* text = !strncmp(arg, "--max-depth=", 12) ? arg + 12 : ++i < argc ? argv[i] : NULL;
            if (du_query_number(text, &value)) return DU_QUERY_ERROR("invalid_numeric_option", 2);
            if (!strcmp(arg, "--offset")) offset = value;
            else if (!strcmp(arg, "--limit")) { if (!value) return DU_QUERY_ERROR("invalid_page", 2); limit = value; }
            else if (!strcmp(arg, "--top")) top = value;
            else { if (value > DU_QUERY_MAX_DEPTH) return DU_QUERY_ERROR("invalid_depth", 2); query.max_depth = (int)value; }
            continue;
        }
        if (!end_options && arg[0] == '-' && arg[1]) {
            if (arg[1] == '-') return DU_QUERY_ERROR("unsupported_option", 2);
            for (size_t p = 1; arg[p]; p++) {
                if (arg[p] == 'h' || arg[p] == 'k') continue;
                if (arg[p] == 's') query.summarize = 1;
                else if (arg[p] == 'a') query.all = 1;
                else if (arg[p] == 'c') grand_total = 1;
                else if (arg[p] == 'd') {
                    uint64_t value;
                    const char* text = arg[p + 1] ? arg + p + 1 : ++i < argc ? argv[i] : NULL;
                    if (du_query_number(text, &value) || value > DU_QUERY_MAX_DEPTH) return DU_QUERY_ERROR("invalid_depth", 2);
                    query.max_depth = (int)value; break;
                } else return DU_QUERY_ERROR("unsupported_option", 2);
            }
            continue;
        }
        if (path_count == 64 || strlen(arg) >= PATH_MAX) return DU_QUERY_ERROR("operand_limit", 2);
        paths[path_count++] = arg;
    }
    if (json_seen && data_seen) return DU_QUERY_ERROR("incompatible_output_modes", 2);
    if (!path_count) paths[path_count++] = ".";
    du_query_row_t total = {.path = "total", .type = "total", .measured = {.known = 1, .complete = 1}};
    for (size_t i = 0; i < path_count && !query.capped && !query.cancelled; i++) {
        du_measurement_t measured = du_query_walk(&query, paths[i], 0, i);
        du_query_sum(&query, &total.measured, &measured);
    }
    if (!query.cancelled) query.cancelled = jash_query_cancel_poll(state);
    if (query.count > 1) qsort(query.rows, query.count, sizeof(*query.rows), du_query_compare);
    uint64_t selected = query.count < top ? query.count : top;
    size_t count = offset < selected ? (size_t)(selected - offset) : 0;
    if ((uint64_t)count > limit) count = (size_t)limit;
    int incomplete = query.capped || top < query.count || (offset < selected && count < selected - offset);
    const char* reason = query.cancelled ? "request_cancelled" : query.capped ? "traversal_limit" :
        top < query.count ? "top_limit" : incomplete ? "page_limit" : NULL;
    int status = query.cancelled ? query.cancelled : query.capped ? 75 : query.errors ? 1 : 0;
    if (native) {
        size_t capacity = count + (grand_total != 0);
        jdv_record_field_t (*fields)[15] = capacity ? calloc(capacity, sizeof(*fields)) : NULL;
        jdv_value_t* items = capacity ? calloc(capacity, sizeof(*items)) : NULL;
        if (capacity && (!fields || !items)) status = DU_QUERY_ERROR("allocation_failed", 1);
        else {
            for (size_t i = 0; i < count; i++) items[i] = du_query_value(fields[i], &query.rows[(size_t)offset + i], 0);
            if (grand_total) items[count] = du_query_value(fields[count], &total, 1);
            status = query_data_emit(writer, context, &DU_QUERY_SCHEMA, items, capacity, query.errors, status,
                query.cancelled ? JDV_STREAM_CANCELLED : incomplete ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE, reason);
        }
        free(fields); free(items);
    } else {
        jash_structured_output_t output;
        if (jash_structured_begin(&output, writer, context)) status = 125;
        else {
            jash_json_writer_t* json = &output.json;
            for (size_t i = 0; i < count; i++) du_query_json_row(json, &query.rows[(size_t)offset + i], 0);
            if (grand_total) du_query_json_row(json, &total, 1);
            if (reason) {
                jash_jsonw_raw(json, "{\"name\":null,\"size_kb\":null,\"complete\":false,\"error\":");
                jash_jsonw_string(json, reason);
                jash_jsonw_raw(json, ",\"next_offset\":");
                jash_jsonw_safe_u64(json, offset + count);
                jash_jsonw_raw(json, "}\n");
            }
            status = jash_structured_finish(&output, status);
        }
    }
    for (size_t i = 0; i < query.count; i++) free(query.rows[i].path);
    free(query.rows);
#undef DU_QUERY_ERROR
    return status;
}

#endif
