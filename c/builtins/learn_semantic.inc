// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "semantic_store.h"

static int learn_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "list";
    int list = !strcmp(operation, "list"), recall = !strcmp(operation, "recall"), forget = !strcmp(operation, "forget");
    const char* query = recall && argc == 2 ? argv[1] : NULL;
    if ((list && argc > 1) || ((recall || forget) && argc != 2)) return semantic_error(call, "invalid_arguments", 2);
    char directory[4096];
    if (semantic_home_path(directory, sizeof(directory), "patterns")) return semantic_error(call, "store_path_limit", 1);
    if (forget) {
        if (!semantic_filename(argv[1], 255)) return semantic_error(call, "invalid_repository_name", 2);
        char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s.yaml", directory, argv[1]);
        if (n < 0 || (size_t)n >= sizeof(path)) return semantic_error(call, "store_path_limit", 1);
        jdv_value_t detail = semantic_text(&call->values, argv[1]);
        int preflight = semantic_store_preflight(call, operation, path, detail);
        if (preflight) return semantic_error(call, "receipt_limit_before_store", preflight);
        if (semantic_admit(call)) return 125;
        int failed = unlink(path);
        return semantic_store_receipt(call, operation, path, detail, failed ? 1 : 0);
    }
    if (!list && !recall) {
        int first = !strcmp(operation, "--") ? 1 : 0;
        if (argc - first != 1 && (argc - first != 3 || strcmp(argv[first + 1], "--repo")))
            return semantic_error(call, "invalid_arguments", 2);
        const char* content = argv[first]; const char* repository = argc - first == 3 ? argv[first + 2] : get_repo_name();
        if (!semantic_filename(repository, 255)) return semantic_error(call, "invalid_repository_name", 2);
        char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s.yaml", directory, repository);
        if (n < 0 || (size_t)n >= sizeof(path)) return semantic_error(call, "store_path_limit", 1);
        return semantic_memory_append(call, "learn", path, "patterns", content, NULL);
    }
    DIR* dir = opendir(directory);
    if (!dir && errno != ENOENT) return semantic_error(call, "store_read_failed", 1);
    jdv_value_t* items = calloc(SEMANTIC_STORE_ROWS, sizeof(*items));
    if (!items) { if (dir) closedir(dir); return semantic_error(call, "allocation_failed", 125); }
    semantic_value_t* sem = &call->values; size_t count = 0; int failed = 0;
    while (dir) {
        errno = 0; struct dirent* entry = readdir(dir);
        if (!entry) { if (errno) failed = 1; break; }
        if (!learn_yaml_file(entry->d_name)) continue;
        if (count == SEMANTIC_STORE_ROWS) { failed = 1; break; }
        char path[4096]; int n = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (n < 0 || (size_t)n >= sizeof(path)) { failed = 1; break; }
        jdv_value_t records; int exists;
        if (semantic_memory_collect(sem, path, 0, query, &records, &exists) || !exists) { failed = 1; break; }
        if (recall && !records.as.list.len) continue;
        jdv_record_input_t fields[] = {
            semantic_field("repository", semantic_text_n(sem, entry->d_name, strlen(entry->d_name) - 5)),
            semantic_field("path", semantic_path(sem, path)), semantic_field("patterns", records)
        };
        items[count++] = semantic_record(sem, fields, 3);
    }
    if (dir && closedir(dir)) failed = 1;
    jdv_value_t list_value = semantic_list(sem, items, count); free(items);
    if (failed) return semantic_error(call, "invalid_or_oversized_memory_store", 1);
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("source", semantic_symbol(sem, "home-pattern-store")),
        semantic_field("consistency", semantic_symbol(sem, "independent-file-observations")),
        semantic_field("query", semantic_text(sem, query)),
        semantic_field("repositories", list_value)
    };
    return semantic_finish(call, semantic_record(sem, fields, 6), 0);
}
