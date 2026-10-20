// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Checked readers of recorded chain/sidecar fields. Parsing a record does not
 * verify its hash/signature, nor restore information lost by its writer. */
#ifndef JASH_BUILTINS_MEMORY_CHAIN_H
#define JASH_BUILTINS_MEMORY_CHAIN_H
#include "memory_store.h"
#include <core/adopt.h>
#include <core/string_compat.h>

#define MEMORY_CHAIN_BYTES (16u * 1024u * 1024u)
#define MEMORY_CHAIN_ROWS 65536u
#define MEMORY_CHAIN_LINE 65536u

typedef struct {
    uint64_t sequence, seconds, nanoseconds, duration_ms, source_line;
    int64_t status;
    char* hash; char* tag; char* command; char* cwd; char* repo; char* session;
    int have_sequence, have_duration;
} memory_chain_entry_t;
typedef struct {
    char* bytes; size_t size; memory_chain_entry_t* entries; size_t count;
    uint64_t markers; int present, error; char path[PATH_MAX];
} memory_chain_t;

static inline int memory_integer(const char* text, int64_t* result) {
    if (!text || !*text || (text[0] != '-' && (text[0] < '0' || text[0] > '9'))) return -1;
    char* end = NULL; errno = 0; long long value = strtoll(text, &end, 10);
    if (errno || *end) return -1;
    *result = (int64_t)value; return 0;
}

static inline int memory_timestamp(char* text, uint64_t* seconds, uint64_t* nanoseconds) {
    char* dot = strchr(text, '.');
    if (!dot) return -1;
    *dot++ = 0;
    return memory_unsigned(text, UINT64_MAX, seconds) || memory_unsigned(dot, 999999999, nanoseconds) ? -1 : 0;
}

static inline int memory_chain_line(char* line, memory_chain_entry_t* entry, int sidecar) {
    char* command = strstr(line, " cmd=");
    if (!command) return -1;
    *command = 0; entry->command = command + 5;
    char* save = NULL; char* token = strtok_r(line, " \t", &save);
    if (!sidecar) {
        /* The actual Sigil ledger is zero-based. Parsing does not establish
         * chain monotonicity or authenticity; retain every valid U64 value. */
        if (!token || memory_unsigned(token, UINT64_MAX, &entry->sequence)) return -1;
        entry->have_sequence = 1; token = strtok_r(NULL, " \t", &save);
    }
    if (!token || memory_timestamp(token, &entry->seconds, &entry->nanoseconds)) return -1;
    token = strtok_r(NULL, " \t", &save);
    if (!token) return -1;
    if (sidecar) entry->tag = token;
    else {
        if (strlen(token) != 64) return -1;
        for (const char* p = token; *p; p++) if (!((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') || (*p >= 'A' && *p <= 'F'))) return -1;
        entry->hash = token;
    }
    unsigned seen = 0;
    while ((token = strtok_r(NULL, " \t", &save))) {
        char* value = strchr(token, '=');
        if (!value) return -1;
        *value++ = 0;
        unsigned bit = 0;
        if (!strcmp(token, "tag") && !sidecar) { bit = 1; entry->tag = value; }
        else if (!strcmp(token, "rc")) { bit = 2; if (memory_integer(value, &entry->status)) return -1; }
        else if (!strcmp(token, "cwd")) { bit = 4; entry->cwd = value; }
        else if (!strcmp(token, "repo")) { bit = 8; entry->repo = value; }
        else if (!strcmp(token, "sess")) { bit = 16; entry->session = value; }
        else if (!strcmp(token, "dur")) { bit = 32; if (memory_unsigned(value, UINT64_MAX, &entry->duration_ms)) return -1; entry->have_duration = 1; }
        else return -1;
        if (seen & bit) return -1;
        seen |= bit;
    }
    if (!(seen & 2u)) return -1;
    if (!entry->tag) entry->tag = "-";
    return 0;
}

static inline void memory_chain_free(memory_chain_t* chain) {
    free(chain->bytes); free(chain->entries); chain->bytes = NULL; chain->entries = NULL;
}

static inline int memory_chain_load(memory_chain_t* chain, int global, int sidecar) {
    memset(chain, 0, sizeof(*chain));
    char filename[128]; const char* key = jash_repo_key();
    if (sidecar) strcpy(filename, "journal.tags");
    else if (!global && key && *key && *key != '-') {
        int n = snprintf(filename, sizeof(filename), "chain-%s.log", key);
        if (n < 0 || (size_t)n >= sizeof(filename)) { chain->error = ENAMETOOLONG; return -1; }
    } else strcpy(filename, "chain.log");
    const char* home = getenv("HOME");
    int n = snprintf(chain->path, sizeof(chain->path), "%s/.jash/%s", home ? home : "", filename);
    if (n < 0 || (size_t)n >= sizeof(chain->path)) { chain->error = ENAMETOOLONG; return -1; }
    memory_store_t store = {-1, -1};
    if (memory_store_open(&store, NULL, 0) ||
        memory_store_read(&store, filename, MEMORY_CHAIN_BYTES, &chain->bytes, &chain->size, &chain->present)) chain->error = errno;
    if (memory_store_close(&store) && !chain->error) chain->error = errno;
    if (chain->error) return -1;
    if (!chain->present || !chain->size) return 0;
    if (memchr(chain->bytes, 0, chain->size)) { chain->error = EINVAL; return -1; }
    chain->entries = calloc(MEMORY_CHAIN_ROWS, sizeof(*chain->entries));
    if (!chain->entries) { chain->error = ENOMEM; return -1; }
    size_t offset = 0; uint64_t source_line = 0;
    while (offset < chain->size) {
        char* line = chain->bytes + offset;
        char* newline = memchr(line, '\n', chain->size - offset);
        if (!newline) { chain->error = EINVAL; return -1; }
        size_t line_size = (size_t)(newline - line);
        if (line_size > MEMORY_CHAIN_LINE || chain->count == MEMORY_CHAIN_ROWS) { chain->error = E2BIG; return -1; }
        *newline = 0; offset = (size_t)(newline - chain->bytes) + 1; source_line++;
        if (!line_size) { chain->error = EINVAL; return -1; }
        memory_chain_entry_t entry = {.source_line = source_line};
        if (memory_chain_line(line, &entry, sidecar)) { chain->error = EINVAL; return -1; }
        if (!strncmp(entry.command, "SESSION_END", 11) && (!entry.command[11] || entry.command[11] == ' ')) { chain->markers++; continue; }
        chain->entries[chain->count++] = entry;
    }
    return 0;
}

static inline jdv_value_t memory_chain_value(semantic_value_t* sem, const memory_chain_entry_t* entry) {
    if (!entry) return jdv_value_nothing();
    jdv_record_input_t fields[] = {
        semantic_field("sequence", entry->have_sequence ? jdv_value_u64(entry->sequence) : jdv_value_nothing()),
        semantic_field("timestamp_seconds", jdv_value_u64(entry->seconds)),
        semantic_field("timestamp_nanoseconds", jdv_value_u64(entry->nanoseconds)),
        semantic_field("source_line", jdv_value_u64(entry->source_line)),
        semantic_field("status", jdv_value_i64(entry->status)),
        semantic_field("hash_recorded", semantic_text(sem, entry->hash)),
        semantic_field("tag", semantic_text(sem, entry->tag)),
        semantic_field("command_recorded", semantic_text(sem, entry->command)),
        semantic_field("cwd_recorded", semantic_text(sem, entry->cwd)),
        semantic_field("repo", semantic_text(sem, entry->repo)),
        semantic_field("session", semantic_text(sem, entry->session)),
        semantic_field("duration_ms", entry->have_duration ? jdv_value_u64(entry->duration_ms) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

static inline jdv_value_t memory_chain_source(semantic_value_t* sem, const memory_chain_t* chain) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, chain->path)),
        semantic_field("present", jdv_value_bool(chain->present)),
        semantic_field("read_complete", jdv_value_bool(!chain->error)),
        semantic_field("bytes", jdv_value_filesize(chain->size)),
        semantic_field("records", jdv_value_u64(chain->count)),
        semantic_field("session_markers", jdv_value_u64(chain->markers)),
        semantic_field("cryptographically_verified", jdv_value_bool(0)),
        semantic_field("command_fidelity", semantic_symbol(sem, "recorded_bytes_only")),
        semantic_field("cwd_fidelity", semantic_symbol(sem, "legacy_may_replace_spaces_with_underscores")),
        semantic_field("error", semantic_symbol(sem, chain->error ? memory_store_error(chain->error) : NULL)),
        semantic_field("errno", chain->error ? jdv_value_i64(chain->error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}
#endif
