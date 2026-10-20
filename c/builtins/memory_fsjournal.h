// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Observations from the retained filesystem journal. It records sampled
 * changes; neither parsing nor temporal overlap proves command causation. */
#ifndef JASH_BUILTINS_MEMORY_FSJOURNAL_H
#define JASH_BUILTINS_MEMORY_FSJOURNAL_H
#include "memory_chain.h"

typedef struct {
    uint64_t seconds, source_line;
    char* session; char* kind; char* path;
} memory_fs_entry_t;
typedef struct {
    char* bytes; size_t size, count; int present, error;
    memory_fs_entry_t* entries; char path[PATH_MAX];
} memory_fs_t;

static inline void memory_fs_free(memory_fs_t* source) {
    free(source->bytes); free(source->entries);
    source->bytes = NULL; source->entries = NULL;
}

static inline int memory_fs_load(memory_fs_t* source) {
    memset(source, 0, sizeof(*source));
    const char* home = getenv("HOME");
    int n = snprintf(source->path, sizeof(source->path), "%s/.jash/fsjournal.tsv", home ? home : "");
    if (n < 0 || (size_t)n >= sizeof(source->path)) { source->error = ENAMETOOLONG; return -1; }
    memory_store_t store = {-1, -1};
    if (memory_store_open(&store, NULL, 0) || memory_store_read(&store, "fsjournal.tsv", 4u * 1024u * 1024u,
            &source->bytes, &source->size, &source->present)) source->error = errno;
    if (memory_store_close(&store) && !source->error) source->error = errno;
    if (source->error) return -1;
    if (!source->size) return 0;
    if (memchr(source->bytes, 0, source->size)) { source->error = EINVAL; return -1; }
    source->entries = calloc(32768, sizeof(*source->entries));
    if (!source->entries) { source->error = ENOMEM; return -1; }
    size_t offset = 0;
    while (offset < source->size) {
        char* line = source->bytes + offset;
        char* newline = memchr(line, '\n', source->size - offset);
        if (!newline) { source->error = EINVAL; return -1; }
        if ((size_t)(newline - line) > 65536 || source->count == 32768) { source->error = E2BIG; return -1; }
        *newline = 0; offset = (size_t)(newline - source->bytes) + 1;
        char* fields[4] = {line, NULL, NULL, NULL};
        for (size_t i = 1; i < 4; i++) {
            char* tab = strchr(fields[i - 1], '\t');
            if (!tab) { source->error = EINVAL; return -1; }
            *tab = 0; fields[i] = tab + 1;
        }
        memory_fs_entry_t entry = {.source_line = source->count + 1};
        if (memory_unsigned(fields[0], UINT64_MAX, &entry.seconds) || !fields[1][0] ||
            !fields[2][0] || fields[2][1] || !strchr("+~-", fields[2][0]) || !fields[3][0]) {
            source->error = EINVAL; return -1;
        }
        entry.session = fields[1]; entry.kind = fields[2]; entry.path = fields[3];
        source->entries[source->count++] = entry;
    }
    return 0;
}

static inline jdv_value_t memory_fs_value(semantic_value_t* sem, const memory_fs_entry_t* entry) {
    jdv_record_input_t fields[] = {
        semantic_field("timestamp_seconds", jdv_value_u64(entry->seconds)),
        semantic_field("source_line", jdv_value_u64(entry->source_line)),
        semantic_field("session", semantic_text(sem, entry->session)),
        semantic_field("kind", semantic_text(sem, entry->kind)),
        semantic_field("path_recorded", semantic_text(sem, entry->path))
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

static inline jdv_value_t memory_fs_source(semantic_value_t* sem, const memory_fs_t* source) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, source->path)),
        semantic_field("present", jdv_value_bool(source->present)),
        semantic_field("read_complete", jdv_value_bool(!source->error)),
        semantic_field("bytes", jdv_value_filesize(source->size)),
        semantic_field("records", jdv_value_u64(source->count)),
        semantic_field("coverage", semantic_symbol(sem, "retained_sampled_changes_only")),
        semantic_field("path_fidelity", semantic_symbol(sem, "recorded_bytes_without_escaping")),
        semantic_field("error", semantic_symbol(sem, source->error ? memory_store_error(source->error) : NULL)),
        semantic_field("errno", source->error ? jdv_value_i64(source->error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}
#endif
