// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Explicit builtin hash table; this is distinct from the executor PATH cache.
#include "builtin.h"
#include "discovery_output.h"
#include <core/argv.h>
#include <parser/exec.h>
#include <limits.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define HASH_SLOTS 32
#define HASH_NAME_LEN 64
#define HASH_PATH_LEN 512
typedef struct { char name[HASH_NAME_LEN], path[HASH_PATH_LEN]; int used; } hash_entry_t;
static hash_entry_t hash_table[HASH_SLOTS];

static hash_entry_t* hash_find(const char* name) {
    for (int i = 0; i < HASH_SLOTS; i++) if (hash_table[i].used && !strcmp(hash_table[i].name, name)) return &hash_table[i];
    return NULL;
}

static int hash_row(jash_discovery_rows_t* rows, const char* name, const char* operation,
                     const char* before, const char* path, int changed, uint64_t cleared, const char* error) {
    jdv_record_input_t fields[] = {
        {JDV_LITERAL("scope"), jash_discovery_string(rows, "builtin-hash-table")},
        {JDV_LITERAL("name"), jash_discovery_string(rows, name)},
        {JDV_LITERAL("operation"), jash_discovery_string(rows, operation)},
        {JDV_LITERAL("before"), jash_discovery_path(rows, before)},
        {JDV_LITERAL("path"), jash_discovery_path(rows, path)},
        {JDV_LITERAL("changed"), jdv_value_bool(changed)},
        {JDV_LITERAL("cleared"), jdv_value_u64(cleared)},
        {JDV_LITERAL("ok"), jdv_value_bool(!error)},
        {JDV_LITERAL("error"), jash_discovery_string(rows, error)},
    };
    return jash_discovery_add(rows, fields, sizeof fields / sizeof fields[0]);
}

int jash_b_hash_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int mode;
    if (jash_discovery_format(argc, argv, &mode) < 0) return jash_discovery_error(w, ctx, "hash", mode, "conflicting_formats", 2);
    int ended = 0, names = 0, clear = 0;
    for (int i = 0; i < argc; i++) {
        if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
        if (!ended && jash_discovery_format_flag(argv[i])) continue;
        if (!ended && !strcmp(argv[i], "-r")) { clear = 1; continue; }
        if ((!ended && argv[i][0] == '-') || !argv[i][0] || strlen(argv[i]) >= HASH_NAME_LEN)
            return jash_discovery_error(w, ctx, "hash", mode, "invalid_options", 2);
        names++;
    }
    if (clear && names) return jash_discovery_error(w, ctx, "hash", mode, "invalid_options", 2);
    if (names > 1024) return jash_discovery_error(w, ctx, "hash", mode, "operand_limit_exceeded", 75);
    jash_discovery_rows_t rows;
    if (jash_discovery_rows_init(&rows)) return jash_discovery_error(w, ctx, "hash", mode, "allocation_failed", 125);
    if (names || clear) {
        int admitted = jash_discovery_effects_begin(&rows, w, ctx, "hash", mode, "jash.hash.v1");
        if (admitted) { jash_discovery_rows_free(&rows); return admitted; }
    }
    int status = 0;
    if (clear) {
        uint64_t count = 0;
        for (int i = 0; i < HASH_SLOTS; i++) count += hash_table[i].used != 0;
        if (!hash_row(&rows, NULL, "clear", NULL, NULL, count != 0, count, NULL)) memset(hash_table, 0, sizeof hash_table);
        (void)jash_discovery_effects_flush(&rows);
    } else if (!names) {
        for (int i = 0; i < HASH_SLOTS; i++) if (hash_table[i].used &&
                hash_row(&rows, hash_table[i].name, "list", hash_table[i].path, hash_table[i].path, 0, 0, NULL)) break;
    } else {
        ended = 0;
        for (int i = 0; i < argc; i++) {
            if (!ended && !strcmp(argv[i], "--")) { ended = 1; continue; }
            if (!ended && jash_discovery_format_flag(argv[i])) continue;
            hash_entry_t* entry = hash_find(argv[i]);
            char before[HASH_PATH_LEN], resolved[PATH_MAX];
            if (entry) memcpy(before, entry->path, strlen(entry->path) + 1u);
            struct stat st;
            const char* error = NULL;
            if (!jash_path_lookup(s, argv[i], resolved, sizeof resolved) ||
                stat(resolved, &st) || !S_ISREG(st.st_mode) || access(resolved, X_OK)) error = "not_found";
            else if (strlen(resolved) >= HASH_PATH_LEN) error = "path_limit_exceeded";
            if (!error && !entry) {
                for (int j = 0; j < HASH_SLOTS; j++) if (!hash_table[j].used) { entry = &hash_table[j]; break; }
                if (!entry) error = "hash_table_full";
            }
            const char* previous = entry && entry->used ? before : NULL;
            int changed = !error && (!previous || strcmp(previous, resolved));
            if (error) status = 1;
            if (hash_row(&rows, argv[i], "cache", previous, error ? previous : resolved, changed, 0, error)) break;
            if (!error) {
                memcpy(entry->name, argv[i], strlen(argv[i]) + 1u);
                memcpy(entry->path, resolved, strlen(resolved) + 1u); entry->used = 1;
            }
            if (jash_discovery_effects_flush(&rows)) break;
        }
    }
    int rc;
    if (mode != JASH_DISCOVERY_TEXT || rows.error) rc = jash_discovery_emit_schema(w, ctx, "hash", mode, &rows, status, "jash.hash.v1");
    else {
        rc = status;
        for (size_t i = 0; i < rows.count; i++) {
            const jdv_value_t* name = jash_discovery_field(&rows.items[i], "name");
            const jdv_value_t* path = jash_discovery_field(&rows.items[i], "path");
            const jdv_value_t* error = jash_discovery_field(&rows.items[i], "error");
            if (error->kind != JDV_NOTHING) jash_writef(jash_stderr_writer, NULL, "hash: %.*s: %.*s\n",
                    (int)name->as.slice.len, name->as.slice.data, (int)error->as.slice.len, error->as.slice.data);
            else if (!names && !clear && jash_writef(w, ctx, "%.*s=%.*s\n", (int)name->as.slice.len,
                    name->as.slice.data, (int)path->as.slice.len, path->as.slice.data) < 0) rc = 125;
        }
    }
    jash_discovery_rows_free(&rows); return rc;
}

int jash_b_hash(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 2;
    int rc = jash_b_hash_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av); return rc;
}
