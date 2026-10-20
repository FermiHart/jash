// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_output.h"
#include "memory_fsjournal.h"
#include "json_git_observe.h"

typedef struct {
    uint64_t timestamp, duration, line; int64_t status;
    char* command; size_t shape_length; int maybe_truncated;
} handoff_record_t;
typedef struct {
    char* bytes; size_t size, count; int present, error, identity_lossy;
    handoff_record_t* records; char path[PATH_MAX]; char key[128];
} handoff_source_t;

static int handoff_source_load(handoff_source_t* source, const char* session) {
    memset(source, 0, sizeof(*source));
    if (!session) return 0;
    if (strlen(session) >= sizeof(source->key)) { source->error = E2BIG; return -1; }
    ho_sid_safe(session, source->key, sizeof(source->key));
    source->identity_lossy = strcmp(session, source->key) != 0;
    const char* home = getenv("HOME");
    if (!home || home[0] != '/') { source->error = EINVAL; return -1; }
    char filename[160];
    int n = snprintf(filename, sizeof(filename), "session_%s.jash", source->key);
    if (n < 0 || (size_t)n >= sizeof(filename)) { source->error = ENAMETOOLONG; return -1; }
    n = snprintf(source->path, sizeof(source->path), "%s/.jash_sessions/%s", home, filename);
    if (n < 0 || (size_t)n >= sizeof(source->path)) { source->error = ENAMETOOLONG; return -1; }
    int homefd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    memory_store_t store = {-1, -1};
    if (homefd < 0) source->error = errno;
    else {
        if (memory_store_trusted(homefd, 1)) source->error = errno;
        else {
            store.directory = memory_store_child(homefd, ".jash_sessions", 0);
            if (store.directory < 0 && errno != ENOENT) source->error = errno;
        }
        if (close(homefd) && !source->error) source->error = errno;
    }
    if (!source->error && memory_store_read(&store, filename, 16u * 1024u * 1024u,
            &source->bytes, &source->size, &source->present)) source->error = errno;
    if (memory_store_close(&store) && !source->error) source->error = errno;
    if (source->error) return -1;
    if (!source->size) return 0;
    if (memchr(source->bytes, 0, source->size)) { source->error = EINVAL; return -1; }
    source->records = calloc(4096, sizeof(*source->records));
    if (!source->records) { source->error = ENOMEM; return -1; }
    size_t offset = 0;
    while (offset < source->size) {
        char* line = source->bytes + offset;
        char* newline = memchr(line, '\n', source->size - offset);
        if (!newline) { source->error = EINVAL; return -1; }
        size_t length = (size_t)(newline - line);
        if (length > 65536 || source->count == 4096) { source->error = E2BIG; return -1; }
        *newline = 0; offset = (size_t)(newline - source->bytes) + 1;
        char* fields[4] = {line, NULL, NULL, NULL};
        for (size_t i = 1; i < 4; i++) {
            char* delimiter = strchr(fields[i - 1], '|');
            if (!delimiter) { source->error = EINVAL; return -1; }
            *delimiter = 0; fields[i] = delimiter + 1;
        }
        handoff_record_t record = {.line = source->count + 1, .command = fields[3], .maybe_truncated = length == 2040};
        if (memory_unsigned(fields[0], UINT64_MAX, &record.timestamp) ||
            memory_integer(fields[1], &record.status) || memory_unsigned(fields[2], UINT64_MAX, &record.duration) || !fields[3][0]) {
            source->error = EINVAL; return -1;
        }
        const char* word = record.command;
        while (*word == ' ' || *word == '\t') word++;
        const char* end = word;
        for (int words = 0; words < 2 && *end; words++) {
            end += strcspn(end, " \t");
            if (words == 0) { while (*end == ' ' || *end == '\t') end++; }
        }
        record.shape_length = (size_t)(end - record.command);
        source->records[source->count++] = record;
    }
    return 0;
}

static int handoff_failure_compare(const void* left, const void* right) {
    const handoff_record_t *a = *(const handoff_record_t* const*)left, *b = *(const handoff_record_t* const*)right;
    size_t size = a->shape_length < b->shape_length ? a->shape_length : b->shape_length;
    int order = memcmp(a->command, b->command, size);
    if (!order && a->shape_length != b->shape_length) order = a->shape_length < b->shape_length ? -1 : 1;
    return order ? order : a->line < b->line ? -1 : a->line > b->line;
}

static int handoff_same_shape(const handoff_record_t* a, const handoff_record_t* b) {
    return a->shape_length == b->shape_length && !memcmp(a->command, b->command, a->shape_length);
}

static int handoff_file_compare(const void* left, const void* right) {
    const memory_fs_entry_t *a = *(const memory_fs_entry_t* const*)left, *b = *(const memory_fs_entry_t* const*)right;
    int order = strcmp(a->path, b->path);
    return order ? order : a->source_line < b->source_line ? -1 : a->source_line > b->source_line;
}

static int handoff_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    uint64_t offset = 0, limit = 64; int git = 1; unsigned seen = 0;
    for (int i = 0; i < argc; i++) {
        const char* value = NULL; unsigned bit = 0;
        if (!strcmp(argv[i], "--no-git")) { git = 0; continue; }
        if (!strcmp(argv[i], "--offset")) bit = 1;
        else if (!strcmp(argv[i], "--limit")) bit = 2;
        else if (!strncmp(argv[i], "--offset=", 9)) { bit = 1; value = argv[i] + 9; }
        else if (!strncmp(argv[i], "--limit=", 8)) { bit = 2; value = argv[i] + 8; }
        if (!bit || (seen & bit)) return semantic_error(call, "invalid_arguments", 2);
        seen |= bit;
        if (!value) { if (++i == argc) return semantic_error(call, "invalid_arguments", 2); value = argv[i]; }
        if (memory_unsigned(value, bit == 1 ? 32768 : 256, bit == 1 ? &offset : &limit) || (bit == 2 && !limit))
            return semantic_error(call, "invalid_arguments", 2);
    }
    const char* session = ho_sid(state);
    handoff_source_t source; (void)handoff_source_load(&source, session);
    memory_fs_t fs; (void)memory_fs_load(&fs);
    int error = source.error ? source.error : fs.error;
    handoff_record_t** failed = calloc(4096, sizeof(*failed));
    memory_fs_entry_t** paths = calloc(32768, sizeof(*paths));
    if (!failed || !paths) { error = ENOMEM; }
    semantic_value_t* sem = &call->values;
    jdv_value_t failures[256], files[256];
    size_t failed_count = 0, file_count = 0, groups = 0, unique_files = 0, shown_failures = 0, shown_files = 0;
    if (!error) {
        for (size_t i = 0; i < source.count; i++) if (source.records[i].status) failed[failed_count++] = &source.records[i];
        qsort(failed, failed_count, sizeof(*failed), handoff_failure_compare);
        for (size_t i = 0; i < failed_count;) {
            size_t end = i + 1;
            while (end < failed_count && handoff_same_shape(failed[i], failed[end])) end++;
            size_t index = groups++; const handoff_record_t* latest = failed[end - 1];
            if (index >= offset && shown_failures < limit) {
                jdv_record_input_t fields[] = {
                    semantic_field("shape_recorded", semantic_text_n(sem, failed[i]->command, failed[i]->shape_length)),
                    semantic_field("count", jdv_value_u64(end - i)),
                    semantic_field("last_status", jdv_value_i64(latest->status)),
                    semantic_field("last_timestamp", jdv_value_u64(latest->timestamp)),
                    semantic_field("last_command_recorded", semantic_text(sem, latest->command)),
                    semantic_field("last_record_maybe_truncated", jdv_value_bool(latest->maybe_truncated))
                };
                failures[shown_failures++] = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
            }
            i = end;
        }
        for (size_t i = 0; i < fs.count; i++) if (!session || !strcmp(session, fs.entries[i].session)) paths[file_count++] = &fs.entries[i];
        qsort(paths, file_count, sizeof(*paths), handoff_file_compare);
        for (size_t i = 0; i < file_count;) {
            size_t end = i + 1; while (end < file_count && !strcmp(paths[i]->path, paths[end]->path)) end++;
            size_t index = unique_files++;
            if (index >= offset && shown_files < limit) files[shown_files++] = memory_fs_value(sem, paths[end - 1]);
            i = end;
        }
    }
    jdv_value_t repository = jdv_value_nothing(); jash_git_observation_t observation = {0};
    int git_error = git ? jash_git_observe(sem, 1, &repository, &observation) : 0;
    int more_failures = !error && offset + shown_failures < groups;
    int more_files = !error && offset + shown_files < unique_files;
    jdv_record_input_t source_fields[] = {
        semantic_field("path", source.path[0] ? semantic_path(sem, source.path) : jdv_value_nothing()),
        semantic_field("present", jdv_value_bool(source.present)),
        semantic_field("selected", jdv_value_bool(session != NULL)),
        semantic_field("read_complete", jdv_value_bool(!source.error)),
        semantic_field("bytes", jdv_value_filesize(source.size)),
        semantic_field("records", jdv_value_u64(source.count)),
        semantic_field("record_fidelity", semantic_symbol(sem, "legacy_unescaped_line_writer_may_truncate_at_2040_bytes")),
        semantic_field("error", semantic_symbol(sem, source.error ? memory_store_error(source.error) : NULL)),
        semantic_field("errno", source.error ? jdv_value_i64(source.error) : jdv_value_nothing())
    };
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("session_id", semantic_text(sem, session)),
        semantic_field("session_key", semantic_text(sem, session ? source.key : NULL)),
        semantic_field("session_identity_lossy", jdv_value_bool(source.identity_lossy)),
        semantic_field("commands_source", semantic_record(sem, source_fields, sizeof(source_fields) / sizeof(source_fields[0]))),
        semantic_field("fs_source", memory_fs_source(sem, &fs)),
        semantic_field("files_scope", semantic_symbol(sem, session ? "exact_session_field" : "all_retained_sessions")),
        semantic_field("git", repository),
        semantic_field("commands_total", source.error ? jdv_value_nothing() : jdv_value_u64(source.count)),
        semantic_field("failed_commands", error ? jdv_value_nothing() : jdv_value_u64(failed_count)),
        semantic_field("first_timestamp", !source.error && source.count ? jdv_value_u64(source.records[0].timestamp) : jdv_value_nothing()),
        semantic_field("last_timestamp", !source.error && source.count ? jdv_value_u64(source.records[source.count - 1].timestamp) : jdv_value_nothing()),
        semantic_field("failures", semantic_list(sem, failures, shown_failures)),
        semantic_field("files", semantic_list(sem, files, shown_files)),
        semantic_field("failure_groups_total", error ? jdv_value_nothing() : jdv_value_u64(groups)),
        semantic_field("files_total", error ? jdv_value_nothing() : jdv_value_u64(unique_files)),
        semantic_field("offset", jdv_value_u64(offset)),
        semantic_field("next_failure_offset", more_failures ? jdv_value_u64(offset + shown_failures) : jdv_value_nothing()),
        semantic_field("next_file_offset", more_files ? jdv_value_u64(offset + shown_files) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(more_failures || more_files)),
        semantic_field("failure_grouping", semantic_symbol(sem, "first_two_whitespace_delimited_words_heuristic")),
        semantic_field("causation_established", jdv_value_bool(0)),
        semantic_field("error", semantic_symbol(sem, error ? memory_store_error(error) : git_error ? "git_observation_failed" : NULL)),
        semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing())
    };
    int result = semantic_finish(call, semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0])), error || git_error ? 1 : 0);
    free(failed); free(paths); free(source.records); free(source.bytes); memory_fs_free(&fs); return result;
}

SEMANTIC_ARGV_WRAPPER(handoff, "handoff", "jash.handoff.v1", handoff_native_provider)
