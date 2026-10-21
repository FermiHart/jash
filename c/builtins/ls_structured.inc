// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
/* Explicit advanced ls queries over the existing bounded filesystem collector.
 * Basic JSON/JTV rows retain their historical schemas. Advanced selectors use
 * a versioned header/entry/summary protocol with exact observation provenance. */
#include "semantic_value.h"
#include <core/query_cancel.h>
#include <fcntl.h>

#define LSX_ITEMS 65536u
#define LSX_SOURCE_BYTES (32u * 1024u * 1024u)
#define LSX_VALUE_BYTES (64u * 1024u * 1024u)
#define LSX_OUTPUT_BYTES (64u * 1024u * 1024u)
#define LSX_FRAME_BYTES (128u * 1024u)
#define LSX_DEPTH 64u
#define LSX_PREVIEW_BYTES 4096u

typedef struct {
    int all, recursive, directory, sort, reverse, recent, large;
    int pretty, pretty_limit, describe, inspect, long_format, preview;
    int version; /* 0 JSONL, 1 JTV1, 2 JTV2 */
    const char* semantic;
    const char* related;
    const char* where_type;
} lsx_options_t;
typedef struct {
    jash_ls_item_t item;
    char* storage;
    const char* root;
    size_t operand, depth, group;
    const char* failure;
    int failure_errno, selected, category;
    unsigned char* preview;
    size_t preview_size, preview_lines;
    int preview_truncated, preview_error, preview_complete;
} lsx_entry_t;
typedef struct {
    jash_state_t* state;
    lsx_options_t options;
    lsx_entry_t* entries;
    size_t count, capacity, allocated, groups;
    uint64_t errors, eligible, selected, omitted;
    int status, cancelled, truncated;
    const char* reason;
    int64_t observed_at;
} lsx_query_t;

static const char* lsx_type(jash_ls_type_t type) {
    static const char* names[] = {"file", "dir", "symlink", "fifo", "socket", "block", "char", "unknown"};
    return type >= JASH_LS_TYPE_REGULAR && type <= JASH_LS_TYPE_UNKNOWN ? names[type] : "unknown";
}
static const char* lsx_consistency(jash_ls_consistency_t consistency) {
    switch (consistency) {
    case JASH_LS_CONSISTENCY_DESCRIPTOR_BOUND: return "descriptor";
    case JASH_LS_CONSISTENCY_PATH_FALLBACK: return "path_fallback";
    case JASH_LS_CONSISTENCY_UNSTABLE: return "unstable";
    default: return "unavailable";
    }
}
static const char* lsx_item_error(jash_ls_item_error_op_t operation) {
    switch (operation) {
    case JASH_LS_ITEM_ERROR_METADATA: return "metadata";
    case JASH_LS_ITEM_ERROR_READLINK: return "readlink";
    case JASH_LS_ITEM_ERROR_UNSTABLE: return "unstable";
    default: return NULL;
    }
}
static void lsx_failed(lsx_query_t* query, const char* reason, int status, int truncated) {
    if (!query->status || status == 125) query->status = status;
    if (!query->reason) query->reason = reason;
    query->truncated |= truncated;
}
static int lsx_poll(void* context) {
    lsx_query_t* query = context;
    int cancelled = jash_query_cancel_poll(query->state);
    if (cancelled) {
        query->cancelled = cancelled; query->status = cancelled;
        query->reason = "cancelled";
    }
    return cancelled;
}
static int lsx_append(lsx_query_t* query, const jash_ls_item_t* source,
                      const char* root, size_t operand, size_t depth, size_t group,
                      const char* failure, int failure_errno) {
    if (query->count == LSX_ITEMS) { lsx_failed(query, "item_limit", 1, 1); return -1; }
    size_t bytes = source->name.len + source->path.len + source->symlink_target.len + 3u;
    if (bytes > LSX_SOURCE_BYTES - query->allocated) { lsx_failed(query, "source_memory_limit", 1, 1); return -1; }
    if (query->count == query->capacity) {
        size_t capacity = query->capacity ? query->capacity * 2u : 64u;
        if (capacity > LSX_ITEMS) capacity = LSX_ITEMS;
        size_t growth = (capacity - query->capacity) * sizeof(*query->entries);
        if (growth > LSX_SOURCE_BYTES - query->allocated - bytes) { lsx_failed(query, "source_memory_limit", 1, 1); return -1; }
        lsx_entry_t* next = realloc(query->entries, capacity * sizeof(*next));
        if (!next) { lsx_failed(query, "allocation_failed", 125, 0); return -1; }
        query->entries = next; query->capacity = capacity; query->allocated += growth;
    }
    char* copy = malloc(bytes);
    if (!copy) { lsx_failed(query, "allocation_failed", 125, 0); return -1; }
    lsx_entry_t* entry = &query->entries[query->count++]; memset(entry, 0, sizeof(*entry));
    entry->storage = copy; entry->item = *source; entry->root = root;
    entry->operand = operand; entry->depth = depth; entry->group = group;
    entry->failure = failure; entry->failure_errno = failure_errno;
    jash_ls_bytes_t* slices[] = {&entry->item.name, &entry->item.path, &entry->item.symlink_target};
    for (size_t i = 0; i < 3; i++) {
        if (slices[i]->len) memcpy(copy, slices[i]->data, slices[i]->len);
        copy[slices[i]->len] = 0; slices[i]->data = (const unsigned char*)copy;
        copy += slices[i]->len + 1u;
    }
    query->allocated += bytes;
    if (failure || source->error.operation != JASH_LS_ITEM_ERROR_NONE) {
        query->errors++; lsx_failed(query, failure ? failure : lsx_item_error(source->error.operation), 1, 0);
    }
    return 0;
}
static int lsx_problem(lsx_query_t* query, const char* path, const char* root,
                       size_t operand, size_t depth, size_t group,
                       const char* operation, int error) {
    const char* name = strrchr(path, '/'); name = name && name[1] ? name + 1 : path;
    jash_ls_item_t item = {.name = {(const unsigned char*)name, strlen(name)},
        .path = {(const unsigned char*)path, strlen(path)}, .type = JASH_LS_TYPE_UNKNOWN};
    return lsx_append(query, &item, root, operand, depth, group, operation, error);
}
static int lsx_compare(const void* left, const void* right) {
    const lsx_entry_t* a = left; const lsx_entry_t* b = right;
    int order = 0;
    if (g_ls_sort == 1) {
        int av = a->item.metadata.modified.available, bv = b->item.metadata.modified.available;
        if (av != bv) return av ? -1 : 1;
        int64_t as = a->item.metadata.modified.seconds, bs = b->item.metadata.modified.seconds;
        order = (as < bs) - (as > bs);
        if (!order) {
            uint32_t an = a->item.metadata.modified.nanoseconds, bn = b->item.metadata.modified.nanoseconds;
            order = (an < bn) - (an > bn);
        }
    } else if (g_ls_sort == 2) {
        int av = !!(a->item.metadata.available & JASH_LS_META_SIZE), bv = !!(b->item.metadata.available & JASH_LS_META_SIZE);
        if (av != bv) return av ? -1 : 1;
        order = (a->item.metadata.size < b->item.metadata.size) - (a->item.metadata.size > b->item.metadata.size);
    }
    if (!order) order = strcmp((const char*)a->item.name.data, (const char*)b->item.name.data);
    return g_ls_reverse && order ? (order < 0 ? 1 : -1) : order;
}
static int lsx_large_compare(const void* left, const void* right) {
    const lsx_entry_t* a = *(const lsx_entry_t* const*)left;
    const lsx_entry_t* b = *(const lsx_entry_t* const*)right;
    int order = (a->item.metadata.size < b->item.metadata.size) - (a->item.metadata.size > b->item.metadata.size);
    return order ? order : strcmp((const char*)a->item.name.data, (const char*)b->item.name.data);
}
static void lsx_collect(lsx_query_t* query, const char* path, const char* root,
                        size_t operand, size_t depth, int directory) {
    if (query->truncated || query->status == 125 || lsx_poll(query)) return;
    size_t group = query->groups++, first = query->count;
    if (depth > LSX_DEPTH) { lsx_failed(query, "depth_limit", 1, 1); return; }
    if (strlen(path) > 4095u) { (void)lsx_problem(query, path, root, operand, depth, group, "path_limit", ENAMETOOLONG); return; }
    jash_ls_collect_options_t options = {.root = {(const unsigned char*)path, strlen(path)},
        .mode = directory ? JASH_LS_COLLECT_DIRECTORY : JASH_LS_COLLECT_OPERAND,
        .symlink_policy = JASH_LS_SYMLINK_NOFOLLOW, .follow_root_symlink = 0,
        .include_hidden = query->options.all, .max_items = LSX_ITEMS,
        .max_logical_bytes = LSX_SOURCE_BYTES, .max_memory_bytes = LSX_SOURCE_BYTES,
        .max_symlink_bytes = 4096};
    jash_ls_cursor_t cursor = {0};
    if (jash_ls_cursor_open(&cursor, &options)) {
        (void)lsx_problem(query, path, root, operand, depth, group, "open", errno); return;
    }
    for (;;) {
        if (lsx_poll(query)) { (void)jash_ls_cursor_cancel(&cursor); break; }
        const jash_ls_item_t* item = NULL;
        jash_ls_next_result_t next = jash_ls_cursor_next(&cursor, &item);
        if (next == JASH_LS_NEXT_END) break;
        if (next == JASH_LS_NEXT_ERROR) {
            (void)lsx_problem(query, path, root, operand, depth, group, "collect", errno); break;
        }
        if (lsx_append(query, item, root, operand, depth, group, NULL, 0)) {
            (void)jash_ls_cursor_cancel(&cursor); break;
        }
    }
    jash_ls_collect_summary_t summary;
    if (jash_ls_cursor_summary(&cursor, &summary)) lsx_failed(query, "collector_summary", 1, 0);
    else if (summary.state == JASH_LS_TERMINAL_TRUNCATED) lsx_failed(query, "collector_limit", 1, 1);
    if (jash_ls_cursor_close(&cursor)) (void)lsx_problem(query, path, root, operand, depth, group, "close", errno);
    size_t end = query->count;
    if (end - first > 1u) qsort(query->entries + first, end - first, sizeof(*query->entries), lsx_compare);
    if (!directory || !query->options.recursive || query->truncated || query->cancelled) return;
    for (size_t i = first; i < end; i++) {
        const lsx_entry_t* entry = &query->entries[i];
        if (entry->item.type != JASH_LS_TYPE_DIRECTORY || entry->failure || entry->item.error.operation != JASH_LS_ITEM_ERROR_NONE) continue;
        /* Paths are individually owned; vector growth cannot invalidate them. */
        const char* child = (const char*)entry->item.path.data;
        lsx_collect(query, child, root, operand, depth + 1u, 1);
        if (query->truncated || query->cancelled || query->status == 125) break;
    }
}
static void lsx_free(lsx_query_t* query) {
    for (size_t i = 0; i < query->count; i++) { free(query->entries[i].storage); free(query->entries[i].preview); }
    free(query->entries); query->entries = NULL;
}

static int lsx_timestamp_ns(jash_ls_timestamp_t time, int64_t* out) {
    if (!time.available || time.nanoseconds >= 1000000000u ||
        time.seconds < INT64_MIN / INT64_C(1000000000) || time.seconds > INT64_MAX / INT64_C(1000000000)) return -1;
    int64_t base = time.seconds * INT64_C(1000000000);
    if (base > INT64_MAX - time.nanoseconds) return -1;
    *out = base + time.nanoseconds; return 0;
}
static jdv_value_t lsx_time_value(semantic_value_t* values, jash_ls_timestamp_t time) {
    if (!time.available) return jdv_value_nothing();
    int64_t native = 0;
    jdv_record_input_t fields[] = {
        semantic_field("seconds", jdv_value_i64(time.seconds)),
        semantic_field("nanoseconds", jdv_value_u64(time.nanoseconds)),
        semantic_field("native", !lsx_timestamp_ns(time, &native) ? jdv_value_timestamp(native) : jdv_value_nothing())
    };
    return semantic_record(values, fields, 3);
}

static void lsx_select(lsx_query_t* query, const char* related_base) {
    size_t begin = 0;
    while (begin < query->count) {
        size_t end = begin + 1u;
        while (end < query->count && query->entries[end].group == query->entries[begin].group) end++;
        size_t eligible = 0;
        for (size_t i = begin; i < end; i++) {
            lsx_entry_t* entry = &query->entries[i];
            ls_file_info_t presentation = {0};
            strlcpy(presentation.name, (const char*)entry->item.name.data, sizeof(presentation.name));
            presentation.mode = (mode_t)entry->item.metadata.mode;
            entry->category = ls_pretty_category(&presentation);
            int take = 1;
            if (!entry->failure && entry->item.error.operation == JASH_LS_ITEM_ERROR_NONE) {
                if (query->options.where_type && strcmp(query->options.where_type, lsx_type(entry->item.type))) take = 0;
                if (query->options.recent && (!entry->item.metadata.modified.available || entry->item.metadata.modified.seconds < query->observed_at - 86400)) take = 0;
                if (query->options.semantic && *query->options.semantic) {
                    const char* description = ls_description_for_mode((const char*)entry->item.path.data, (mode_t)entry->item.metadata.mode);
                    if (!jash_strcasestr((const char*)entry->item.name.data, query->options.semantic) && !jash_strcasestr(description, query->options.semantic)) take = 0;
                }
                if (related_base && (entry->item.type != JASH_LS_TYPE_REGULAR || !strstr((const char*)entry->item.name.data, related_base))) take = 0;
            }
            entry->selected = take;
            if (take) eligible++;
        }
        query->eligible += eligible;
        if (query->options.large && eligible > 10u) {
            lsx_entry_t** pointers = malloc(eligible * sizeof(*pointers));
            if (!pointers) { lsx_failed(query, "allocation_failed", 125, 0); return; }
            size_t used = 0;
            for (size_t i = begin; i < end; i++) if (query->entries[i].selected && !query->entries[i].failure && !query->entries[i].item.error.operation) pointers[used++] = &query->entries[i];
            qsort(pointers, used, sizeof(*pointers), lsx_large_compare);
            for (size_t i = 10; i < used; i++) pointers[i]->selected = 0;
            free(pointers);
        }
        if (query->options.pretty && query->options.pretty_limit) {
            size_t counts[LS_PRETTY_CATEGORIES] = {0}, quota[LS_PRETTY_CATEGORIES] = {0}, total = 0, assigned = 0;
            for (size_t i = begin; i < end; i++) if (query->entries[i].selected && !query->entries[i].failure && !query->entries[i].item.error.operation) { counts[query->entries[i].category]++; total++; }
            size_t target = total < (size_t)query->options.pretty_limit ? total : (size_t)query->options.pretty_limit;
            while (assigned < target) for (size_t i = 0; i < LS_PRETTY_CATEGORIES && assigned < target; i++) if (quota[i] < counts[i]) { quota[i]++; assigned++; }
            for (size_t i = begin; i < end; i++) {
                lsx_entry_t* entry = &query->entries[i];
                if (!entry->selected || entry->failure || entry->item.error.operation) continue;
                if (quota[entry->category]) quota[entry->category]--; else entry->selected = 0;
            }
        }
        begin = end;
    }
    for (size_t i = 0; i < query->count; i++) query->selected += !!query->entries[i].selected;
    query->omitted = query->eligible - query->selected;
}

static void lsx_preview(lsx_query_t* query, lsx_entry_t* entry) {
    if (entry->failure || entry->item.error.operation || entry->item.type != JASH_LS_TYPE_REGULAR) return;
    int fd = open((const char*)entry->item.path.data, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    int error = fd < 0 ? errno : 0;
    struct stat before, after;
    if (!error && fstat(fd, &before)) error = errno;
    if (!error && (!S_ISREG(before.st_mode) || (uint64_t)before.st_dev != entry->item.metadata.device ||
        (uint64_t)before.st_ino != entry->item.metadata.inode || before.st_size < 0 || (uint64_t)before.st_size != entry->item.metadata.size ||
        (int64_t)BEAR_ST_MTIM(before).tv_sec != entry->item.metadata.modified.seconds || (uint32_t)BEAR_ST_MTIM(before).tv_nsec != entry->item.metadata.modified.nanoseconds)) error = ESTALE;
    unsigned char buffer[LSX_PREVIEW_BYTES + 1u]; size_t used = 0; int eof = 0;
    while (!error && used < sizeof(buffer)) {
        if (lsx_poll(query)) { error = ECANCELED; break; }
        ssize_t got = read(fd, buffer + used, sizeof(buffer) - used);
        if (got < 0) { if (errno == EINTR) continue; error = errno; }
        else if (!got) { eof = 1; break; }
        else used += (size_t)got;
    }
    if (!error && fstat(fd, &after)) error = errno;
    if (!error && (before.st_size != after.st_size || BEAR_ST_MTIM(before).tv_sec != BEAR_ST_MTIM(after).tv_sec ||
        BEAR_ST_MTIM(before).tv_nsec != BEAR_ST_MTIM(after).tv_nsec || BEAR_ST_CTIM(before).tv_sec != BEAR_ST_CTIM(after).tv_sec ||
        BEAR_ST_CTIM(before).tv_nsec != BEAR_ST_CTIM(after).tv_nsec)) error = ESTALE;
    if (fd >= 0 && close(fd) && !error) error = errno;
    if (!error) {
        size_t keep = 0, lines = 0;
        while (keep < used && keep < LSX_PREVIEW_BYTES && lines < 3u) if (buffer[keep++] == '\n') lines++;
        if (keep && buffer[keep-1u] != '\n') lines++;
        if (keep > LSX_SOURCE_BYTES - query->allocated) error = E2BIG;
        else {
            entry->preview = malloc(keep ? keep : 1u);
            if (!entry->preview) error = ENOMEM;
            else { memcpy(entry->preview, buffer, keep); query->allocated += keep;
                entry->preview_size = keep; entry->preview_lines = lines;
                entry->preview_truncated = keep < used || !eof; entry->preview_complete = 1; }
        }
    }
    if (error) { entry->preview_error = error; query->errors++; if (!query->cancelled) lsx_failed(query, "preview", error == ENOMEM ? 125 : 1, 0); }
}

static jdv_value_t lsx_owner(semantic_value_t* values, uint64_t id, int group, int* lookup_error) {
    char buffer[16384]; const char* name = NULL; int error = 0;
    struct passwd user, *user_result = NULL; struct group grp, *group_result = NULL;
    if (group) {
        if ((uint64_t)(gid_t)id != id) error = EOVERFLOW;
        else { error = getgrgid_r((gid_t)id, &grp, buffer, sizeof(buffer), &group_result); if (!error && group_result) name = grp.gr_name; }
    } else {
        if ((uint64_t)(uid_t)id != id) error = EOVERFLOW;
        else { error = getpwuid_r((uid_t)id, &user, buffer, sizeof(buffer), &user_result); if (!error && user_result) name = user.pw_name; }
    }
    if (error) *lookup_error = error;
    jdv_record_input_t fields[] = {semantic_field("name", semantic_text(values, name)),
        semantic_field("found", jdv_value_bool(name != NULL)), semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing())};
    return semantic_record(values, fields, 3);
}
static jdv_value_t lsx_entry_value(semantic_value_t* values, lsx_query_t* query, const lsx_entry_t* entry) {
    const jash_ls_item_t* item = &entry->item; uint32_t available = item->metadata.available;
    int lookup_error = 0;
    jdv_value_t error = jdv_value_nothing(), preview = jdv_value_nothing();
    const char* operation = entry->failure ? entry->failure : lsx_item_error(item->error.operation);
    if (operation) {
        jdv_record_input_t fields[] = {semantic_field("operation", semantic_symbol(values, operation)),
            semantic_field("errno", jdv_value_i64(entry->failure ? entry->failure_errno : item->error.error_number))};
        error = semantic_record(values, fields, 2);
    }
    if (query->options.preview && item->type == JASH_LS_TYPE_REGULAR) {
        jdv_record_input_t fields[] = {semantic_field("bytes", semantic_bytes(values, entry->preview, entry->preview_size)),
            semantic_field("byte_count", jdv_value_u64(entry->preview_size)), semantic_field("lines", jdv_value_u64(entry->preview_lines)),
            semantic_field("truncated", jdv_value_bool(entry->preview_truncated)), semantic_field("read_complete", jdv_value_bool(entry->preview_complete)),
            semantic_field("errno", entry->preview_error ? jdv_value_i64(entry->preview_error) : jdv_value_nothing())};
        preview = semantic_record(values, fields, 6);
    }
    char permissions[12] = {0}; if (available & JASH_LS_META_MODE) ls_format_permissions((mode_t)item->metadata.mode, permissions);
    const char* description = (available & JASH_LS_META_MODE) ? ls_description_for_mode((const char*)item->path.data, (mode_t)item->metadata.mode) : NULL;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.ls.extended.row.v1")), semantic_field("row_kind", semantic_symbol(values, "entry")),
        semantic_field("operand_index", jdv_value_u64(entry->operand)), semantic_field("root", semantic_path(values, entry->root)),
        semantic_field("depth", jdv_value_u64(entry->depth)), semantic_field("group", jdv_value_u64(entry->group)),
        semantic_field("name", semantic_text_n(values, item->name.data, item->name.len)), semantic_field("path", semantic_path(values, (const char*)item->path.data)),
        semantic_field("type", semantic_symbol(values, lsx_type(item->type))), semantic_field("consistency", semantic_symbol(values, lsx_consistency(item->consistency))),
        semantic_field("size", (available & JASH_LS_META_SIZE) ? jdv_value_filesize(item->metadata.size) : jdv_value_nothing()),
        semantic_field("mode", (available & JASH_LS_META_MODE) ? jdv_value_u64(item->metadata.mode) : jdv_value_nothing()),
        semantic_field("permissions", semantic_text(values, permissions[0] ? permissions : NULL)),
        semantic_field("uid", (available & JASH_LS_META_UID) ? jdv_value_u64(item->metadata.uid) : jdv_value_nothing()),
        semantic_field("gid", (available & JASH_LS_META_GID) ? jdv_value_u64(item->metadata.gid) : jdv_value_nothing()),
        semantic_field("inode", (available & JASH_LS_META_INODE) ? jdv_value_u64(item->metadata.inode) : jdv_value_nothing()),
        semantic_field("device", (available & JASH_LS_META_DEVICE) ? jdv_value_u64(item->metadata.device) : jdv_value_nothing()),
        semantic_field("nlink", (available & JASH_LS_META_NLINK) ? jdv_value_u64(item->metadata.nlink) : jdv_value_nothing()),
        semantic_field("modified", lsx_time_value(values, item->metadata.modified)), semantic_field("changed", lsx_time_value(values, item->metadata.changed)),
        semantic_field("accessed", lsx_time_value(values, item->metadata.accessed)), semantic_field("created", lsx_time_value(values, item->metadata.created)),
        semantic_field("symlink_target", item->symlink_target_available ? semantic_bytes(values, item->symlink_target.data, item->symlink_target.len) : jdv_value_nothing()),
        semantic_field("symlink_target_truncated", jdv_value_bool(item->symlink_target_truncated)),
        semantic_field("description", (query->options.describe || query->options.inspect || query->options.semantic) ? semantic_text(values, description) : jdv_value_nothing()),
        semantic_field("category", query->options.pretty ? semantic_symbol(values, ls_pretty_label(entry->category)) : jdv_value_nothing()),
        semantic_field("owner", (query->options.long_format || query->options.inspect) && (available & JASH_LS_META_UID) ? lsx_owner(values, item->metadata.uid, 0, &lookup_error) : jdv_value_nothing()),
        semantic_field("group_owner", (query->options.long_format || query->options.inspect) && (available & JASH_LS_META_GID) ? lsx_owner(values, item->metadata.gid, 1, &lookup_error) : jdv_value_nothing()),
        semantic_field("preview", preview), semantic_field("error", error)
    };
    if (lookup_error) { query->errors++; lsx_failed(query, "owner_lookup", 1, 0); }
    return semantic_record(values, fields, sizeof(fields)/sizeof(fields[0]));
}

static void lsx_config(jdv_stream_config_t* config, const semantic_value_t* values, const jdv_schema_t* schema) {
    jdv_stream_config_init(config); config->boundedness = JDV_STREAM_FINITE;
    config->declared_schema = config->guaranteed_schema = schema; config->value_limits = values->limits;
    config->stream_limits.max_items = LSX_ITEMS + 3u; config->stream_limits.max_logical_bytes = LSX_SOURCE_BYTES;
    config->stream_limits.max_physical_bytes = LSX_OUTPUT_BYTES; config->stream_limits.max_materialized_bytes = LSX_VALUE_BYTES;
}
static void lsx_outcome(jdv_stream_end_t* outcome, const lsx_query_t* query) {
    (void)jdv_stream_end_init(outcome);
    outcome->state = query->cancelled ? JDV_STREAM_CANCELLED : query->truncated ? JDV_STREAM_INCOMPLETE : query->status ? JDV_STREAM_FAILED : JDV_STREAM_COMPLETE;
    outcome->completeness = query->status ? JDV_COMPLETENESS_PARTIAL : JDV_COMPLETENESS_COMPLETE;
    outcome->producer_status = query->status; outcome->errors_seen = query->errors ? query->errors : query->status ? 1u : 0u;
    outcome->truncated = query->truncated;
    if (query->reason) outcome->reason = (jdv_slice_t){(const uint8_t*)query->reason, strlen(query->reason)};
}
static int lsx_emit_rows(jash_writer_t writer, void* context, semantic_value_t* values,
                         const jdv_value_t* rows, size_t count, const uint64_t* credits,
                         const lsx_query_t* query) {
    if (values->error) return jash_structured_error(writer, context, "ls", "value_limit", 125);
    if (!query->options.version) {
        jash_structured_buffer_t buffer; jash_structured_buffer_init(&buffer, LSX_OUTPUT_BYTES);
        jash_json_writer_t json; jash_jsonw_init(&json, jash_structured_buffer_sink, &buffer);
        for (size_t i = 0; i < count; i++) { if (jdv_json_write(&json, &rows[i], &values->limits)) break; jash_jsonw_raw(&json, "\n"); }
        int status = jash_jsonw_finish(&json) ? 125 : 0;
        if (!status) status = jash_structured_document(writer, context, "ls", buffer.data, buffer.used, buffer.required, jash_budget_limit(), query->status);
        jash_structured_buffer_free(&buffer); return status;
    }
    jdv_schema_t schema = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
    jdv_stream_config_t config; lsx_config(&config, values, &schema);
    jdv_stream_end_t outcome, final; lsx_outcome(&outcome, query);
    void* workspace = malloc(LSX_FRAME_BYTES); if (!workspace) return 125;
    value_output_measure_t measured = {0, LSX_OUTPUT_BYTES};
    int status = jdv_emit_sequence_version((unsigned)query->options.version, rows, count, credits, &config, &outcome,
        value_output_measure, &measured, workspace, LSX_FRAME_BYTES, &final);
    if (status) { free(workspace); return 125; }
    jash_structured_output_t checked;
    if (jash_structured_begin(&checked, writer, context)) { free(workspace); return 125; }
    value_output_sink_t sink = {checked.json.sink, checked.json.context};
    status = jdv_emit_sequence_version((unsigned)query->options.version, rows, count, credits, &config, &outcome,
        value_output_sink, &sink, workspace, LSX_FRAME_BYTES, &final);
    free(workspace); return jash_structured_finish(&checked, status ? 125 : query->status);
}

static jdv_value_t lsx_header(semantic_value_t* values, const lsx_query_t* query,
                             const char* const* roots, size_t count) {
    jdv_value_t operands[1024];
    for (size_t i = 0; i < count; i++) operands[i] = semantic_path(values, roots[i]);
    const lsx_options_t* o = &query->options;
    jdv_record_input_t options[] = {
        semantic_field("all", jdv_value_bool(o->all)), semantic_field("recursive", jdv_value_bool(o->recursive)),
        semantic_field("directory", jdv_value_bool(o->directory)), semantic_field("sort", semantic_symbol(values, o->sort == 1 ? "mtime" : o->sort == 2 ? "size" : "name")),
        semantic_field("reverse", jdv_value_bool(o->reverse)), semantic_field("recent_since", o->recent ? jdv_value_i64(query->observed_at - 86400) : jdv_value_nothing()),
        semantic_field("large_limit", o->large ? jdv_value_u64(10) : jdv_value_nothing()),
        semantic_field("pretty_limit", o->pretty ? jdv_value_u64((uint64_t)o->pretty_limit) : jdv_value_nothing()),
        semantic_field("semantic", semantic_text(values, o->semantic)), semantic_field("relate", semantic_path(values, o->related)),
        semantic_field("where_type", semantic_symbol(values, o->where_type)), semantic_field("preview", jdv_value_bool(o->preview)),
        semantic_field("describe", jdv_value_bool(o->describe)), semantic_field("inspect", jdv_value_bool(o->inspect)), semantic_field("long", jdv_value_bool(o->long_format))
    };
    jdv_record_input_t provenance[] = {
        semantic_field("collector", semantic_symbol(values, "ls_collector_nofollow")), semantic_field("sort_collation", semantic_symbol(values, "unsigned_bytes")),
        semantic_field("description", semantic_symbol(values, "extension_and_mode")), semantic_field("semantic", semantic_symbol(values, "case_insensitive_substring_name_or_description")),
        semantic_field("relation", semantic_symbol(values, "basename_without_extension_substring_regular_files")), semantic_field("content_analysis", jdv_value_bool(0)),
        semantic_field("external_model", jdv_value_bool(0)), semantic_field("preview", semantic_symbol(values, "first_3_lines_max_4096_bytes")),
        semantic_field("selection_scope", semantic_symbol(values, "per_directory_group"))
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.ls.extended.row.v1")), semantic_field("row_kind", semantic_symbol(values, "header")),
        semantic_field("roots", semantic_list(values, operands, count)), semantic_field("observed_at", jdv_value_i64(query->observed_at)),
        semantic_field("options", semantic_record(values, options, sizeof(options)/sizeof(options[0]))),
        semantic_field("provenance", semantic_record(values, provenance, sizeof(provenance)/sizeof(provenance[0])))
    };
    return semantic_record(values, fields, 6);
}
static jdv_value_t lsx_summary(semantic_value_t* values, const lsx_query_t* query) {
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(values, "jash.ls.extended.row.v1")), semantic_field("row_kind", semantic_symbol(values, "summary")),
        semantic_field("observed", jdv_value_u64(query->count)), semantic_field("eligible", jdv_value_u64(query->eligible)),
        semantic_field("selected", jdv_value_u64(query->selected)), semantic_field("selection_omitted", jdv_value_u64(query->omitted)),
        semantic_field("source_complete", jdv_value_bool(!query->status)), semantic_field("truncated", jdv_value_bool(query->truncated)),
        semantic_field("cancelled", jdv_value_bool(query->cancelled != 0)), semantic_field("errors", jdv_value_u64(query->errors)),
        semantic_field("command_status", jdv_value_i64(query->status)), semantic_field("reason", semantic_symbol(values, query->reason))
    };
    return semantic_record(values, fields, 12);
}
static int ls_structured_query(jash_state_t* state, jash_writer_t writer, void* context,
                               const char* const* roots, size_t root_count, const lsx_options_t* options) {
    lsx_query_t query = {.state = state, .options = *options};
    time_t now = time(NULL); if (now == (time_t)-1) return jash_structured_error(writer, context, "ls", "clock_failed", 1);
    query.observed_at = (int64_t)now;
    char related_directory[4096], related_base[256]; const char* related = NULL;
    if (options->related) {
        size_t length = strlen(options->related);
        const char* slash = strrchr(options->related, '/'); const char* name = slash ? slash + 1 : options->related;
        if (!*name || strlen(name) >= sizeof(related_base) || length >= sizeof(related_directory) || options->directory)
            return jash_structured_error(writer, context, "ls", "invalid_relation_target", 2);
        if (jash_copy_cstr(related_base, sizeof(related_base), name)) { return jash_structured_error(writer, context, "ls", "invalid_relation_target", 2); } char* dot = strrchr(related_base, '.'); if (dot && dot != related_base) *dot = 0;
        size_t dir_length = slash ? (size_t)(slash - options->related) : 0;
        if (slash) { if (!dir_length) dir_length = 1; memcpy(related_directory, options->related, dir_length); related_directory[dir_length] = 0; }
        else if (jash_copy_cstr(related_directory, sizeof(related_directory), ".")) { return jash_structured_error(writer, context, "ls", "invalid_relation_target", 2); }
        related = related_base;
        lsx_collect(&query, related_directory, related_directory, 0, 0, 1);
    } else for (size_t i = 0; i < root_count; i++) {
        struct stat st; int directory = !options->directory && !lstat(roots[i], &st) && S_ISDIR(st.st_mode);
        lsx_collect(&query, roots[i], roots[i], i, 0, directory);
        if (query.truncated || query.cancelled || query.status == 125) break;
    }
    lsx_select(&query, related);
    if (options->preview) for (size_t i = 0; i < query.count; i++) if (query.entries[i].selected) {
        lsx_preview(&query, &query.entries[i]); if (query.cancelled || query.status == 125) break;
    }
    (void)lsx_poll(&query);
    semantic_value_t values;
    if (semantic_value_init(&values, LSX_VALUE_BYTES, LSX_FRAME_BYTES - 512u)) { lsx_free(&query); return 125; }
    size_t count = (size_t)query.selected + 2u;
    jdv_value_t* rows = calloc(count, sizeof(*rows)); uint64_t* credits = calloc(count, sizeof(*credits));
    int status = 125;
    if (rows && credits) {
        const char* related_root = related_directory;
        rows[0] = lsx_header(&values, &query, options->related ? &related_root : roots, options->related ? 1 : root_count); size_t at = 1;
        for (size_t i = 0; i < query.count; i++) if (query.entries[i].selected) {
            rows[at] = lsx_entry_value(&values, &query, &query.entries[i]);
            credits[at++] = query.entries[i].item.name.len + query.entries[i].item.path.len + query.entries[i].item.symlink_target.len + query.entries[i].preview_size;
        }
        rows[at] = lsx_summary(&values, &query);
        status = lsx_emit_rows(writer, context, &values, rows, count, credits, &query);
    }
    free(rows); free(credits); semantic_value_free(&values); lsx_free(&query); return status;
}
