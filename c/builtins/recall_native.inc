// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_output.h"
#include "memory_fsjournal.h"
#include <dirent.h>

typedef struct {
    const char *pattern, *tag, *cwd, *repo, *session;
    uint64_t offset, limit, since, until;
    int failed, ok, word, global;
} recall_native_filter_t;

static int recall_native_matches(const recall_native_filter_t* filter, const memory_chain_entry_t* entry) {
    return (!filter->failed || entry->status != 0) && (!filter->ok || entry->status == 0) &&
        (!filter->pattern || recall_match(entry->command, filter->pattern, filter->word)) &&
        (!filter->tag || (entry->tag && !strcmp(entry->tag, filter->tag))) &&
        (!filter->cwd || (entry->cwd && strstr(entry->cwd, filter->cwd))) &&
        (!filter->repo || (entry->repo && !strcmp(entry->repo, filter->repo))) &&
        (!filter->session || (entry->session && !strcmp(entry->session, filter->session))) &&
        entry->seconds >= filter->since && (!filter->until || entry->seconds <= filter->until);
}

static int recall_native_options(int argc, char** argv, recall_native_filter_t* filter) {
    unsigned seen = 0; int literal = 0;
    for (int i = 0; i < argc; i++) {
        const char* argument = argv[i]; const char* value = NULL; unsigned bit = 0;
        if (!literal && !strcmp(argument, "--")) { literal = 1; continue; }
        if (!literal && !strcmp(argument, "--failed")) { filter->failed = 1; continue; }
        if (!literal && !strcmp(argument, "--ok")) { filter->ok = 1; continue; }
        if (!literal && !strcmp(argument, "--word")) { filter->word = 1; continue; }
        if (!literal && !strcmp(argument, "--global")) { filter->global = 1; continue; }
        static const char* const names[] = {"--last", "--limit", "--offset", "--tag", "--cwd", "--repo", "--session", "--since", "--until"};
        static const unsigned bits[] = {1, 1, 2, 4, 8, 16, 32, 64, 128};
        if (!literal) for (size_t k = 0; k < sizeof(names) / sizeof(names[0]); k++) {
            size_t length = strlen(names[k]);
            if (!strcmp(argument, names[k])) bit = bits[k];
            else if (!strncmp(argument, names[k], length) && argument[length] == '=') { bit = bits[k]; value = argument + length + 1; }
            if (bit) break;
        }
        if (bit) {
            if (seen & bit) return -1;
            seen |= bit;
            if (!value && bit == 16 && (i + 1 == argc || argv[i + 1][0] == '-')) value = jash_repo_key();
            if (!value) { if (++i == argc) return -1; value = argv[i]; }
            if (!*value) return -1;
            if ((bit == 1 && (memory_unsigned(value, 500, &filter->limit) || !filter->limit)) ||
                (bit == 2 && memory_unsigned(value, MEMORY_CHAIN_ROWS, &filter->offset)) ||
                (bit == 64 && memory_unsigned(value, UINT64_MAX, &filter->since)) ||
                (bit == 128 && memory_unsigned(value, UINT64_MAX, &filter->until))) return -1;
            if (bit == 4) filter->tag = value;
            if (bit == 8) filter->cwd = value;
            if (bit == 16) filter->repo = value;
            if (bit == 32) filter->session = value;
        } else {
            if (filter->pattern || (!literal && argument[0] == '-')) return -1;
            filter->pattern = argument;
        }
    }
    return (filter->failed && filter->ok) || (filter->until && filter->since > filter->until) ? -1 : 0;
}

static jdv_value_t recall_native_change(semantic_value_t* sem, const char* field, const char* before, const char* after) {
    jdv_record_input_t fields[] = {
        semantic_field("field", semantic_symbol(sem, field)),
        semantic_field("before", semantic_text(sem, before)),
        semantic_field("after", semantic_text(sem, after)),
        semantic_field("changed", before && after ? jdv_value_bool(strcmp(before, after) != 0) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 4);
}

static jdv_value_t recall_native_why(semantic_value_t* sem, const memory_chain_t* chain,
                                     const recall_native_filter_t* filter, size_t* matched, int* failed) {
    const memory_chain_entry_t *first = NULL, *good = NULL, *bad = NULL;
    for (size_t i = 0; i < chain->count; i++) {
        const memory_chain_entry_t* entry = &chain->entries[i];
        if (!recall_native_matches(filter, entry)) continue;
        if (!first) first = entry;
        (*matched)++;
        if (!entry->status) { good = entry; bad = NULL; }
        else if (good && !bad) bad = entry;
    }
    jdv_value_t changes[3], files[500]; size_t changed = 0, emitted = 0, file_count = 0;
    memory_fs_t journal = {0};
    if (good && bad) {
        changes[changed++] = recall_native_change(sem, "cwd_recorded", good->cwd, bad->cwd);
        changes[changed++] = recall_native_change(sem, "repo", good->repo, bad->repo);
        changes[changed++] = recall_native_change(sem, "session", good->session, bad->session);
        if (memory_fs_load(&journal)) *failed = journal.error;
        else for (size_t i = 0; i < journal.count; i++) {
            const memory_fs_entry_t* entry = &journal.entries[i];
            if (entry->seconds <= good->seconds || entry->seconds > bad->seconds) continue;
            file_count++;
            if (emitted < 500) files[emitted++] = memory_fs_value(sem, entry);
        }
    }
    jdv_record_input_t fields[] = {
        semantic_field("conclusion", semantic_symbol(sem, !first ? "no_recorded_match" : !good ? "no_successful_baseline" : !bad ? "no_failure_after_last_success" : "temporal_comparison")),
        semantic_field("first_recorded_match", memory_chain_value(sem, first)),
        semantic_field("last_success", memory_chain_value(sem, good)),
        semantic_field("first_failure_after_last_success", memory_chain_value(sem, bad)),
        semantic_field("context_changes", semantic_list(sem, changes, changed)),
        semantic_field("file_changes", semantic_list(sem, files, emitted)),
        semantic_field("file_changes_matched", journal.error ? jdv_value_nothing() : jdv_value_u64(file_count)),
        semantic_field("file_changes_truncated", jdv_value_bool(file_count > emitted)),
        semantic_field("file_source", good && bad ? memory_fs_source(sem, &journal) : jdv_value_nothing()),
        semantic_field("causation_established", jdv_value_bool(0))
    };
    jdv_value_t value = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    memory_fs_free(&journal); return value;
}

typedef struct { const char* word; size_t length; uint64_t runs, failed; } recall_native_stat_t;
static int recall_native_stat_compare(const void* left, const void* right) {
    const recall_native_stat_t *a = left, *b = right;
    size_t n = a->length < b->length ? a->length : b->length;
    int compared = memcmp(a->word, b->word, n);
    return compared ? compared : a->length < b->length ? -1 : a->length > b->length;
}

static jdv_value_t recall_native_aggregate(semantic_value_t* sem, const memory_chain_t* chain,
                                           const recall_native_filter_t* filter, int adoption,
                                           jdv_value_t* items, size_t* emitted, size_t* matched, int* error) {
    recall_native_stat_t* words = adoption ? NULL : calloc(4096, sizeof(*words));
    if (!adoption && !words) { *error = ENOMEM; return jdv_value_nothing(); }
    size_t distinct = 0; uint64_t total = 0, failed = 0, differential = 0, features[32] = {0};
    for (size_t i = 0; i < chain->count; i++) {
        const memory_chain_entry_t* entry = &chain->entries[i];
        if (!recall_native_matches(filter, entry)) continue;
        const char* word = entry->command; while (*word == ' ' || *word == '\t') word++;
        size_t size = strcspn(word, " \t\r\n"); if (!size) continue;
        total++; failed += entry->status != 0;
        if (adoption) {
            int feature = scan_differentiator(entry->command);
            if (feature >= 0 && feature < 32) { features[feature]++; differential++; }
        } else {
            size_t k = 0;
            while (k < distinct && (words[k].length != size || memcmp(words[k].word, word, size))) k++;
            if (k == distinct) {
                if (distinct == 4096) { *error = E2BIG; break; }
                words[k].word = word; words[k].length = size; distinct++;
            }
            words[k].runs++; words[k].failed += entry->status != 0;
        }
    }
    if (!*error) {
        if (adoption) {
            for (size_t i = 0; JASH_DIFFERENTIATORS[i]; i++) {
                if (!features[i]) continue;
                size_t index = (*matched)++;
                if (index < filter->offset || *emitted == filter->limit) continue;
                jdv_record_input_t fields[] = {
                    semantic_field("feature", semantic_symbol(sem, JASH_DIFFERENTIATORS[i])),
                    semantic_field("commands", jdv_value_u64(features[i]))
                };
                items[(*emitted)++] = semantic_record(sem, fields, 2);
            }
        } else {
            qsort(words, distinct, sizeof(*words), recall_native_stat_compare); *matched = distinct;
            for (size_t i = (size_t)filter->offset; i < distinct && *emitted < filter->limit; i++) {
                jdv_record_input_t fields[] = {
                    semantic_field("first_word_recorded", semantic_text_n(sem, words[i].word, words[i].length)),
                    semantic_field("runs", jdv_value_u64(words[i].runs)),
                    semantic_field("failed", jdv_value_u64(words[i].failed))
                };
                items[(*emitted)++] = semantic_record(sem, fields, 3);
            }
        }
    }
    jdv_record_input_t fields[] = {
        semantic_field("commands", jdv_value_u64(total)),
        semantic_field("failed", jdv_value_u64(failed)),
        semantic_field("differentiator_uses", adoption ? jdv_value_u64(differential) : jdv_value_nothing()),
        semantic_field("adoption_percent_rounded", adoption && total ? jdv_value_u64((differential * 100 + total / 2) / total) : jdv_value_nothing()),
        semantic_field("classifier", semantic_symbol(sem, adoption ? "first_recognized_differentiator_heuristic" : "first_whitespace_delimited_word")),
        semantic_field("classification_complete", jdv_value_bool(!*error))
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    free(words); return result;
}

#include "recall_overflow_native.inc"

static int recall_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "list";
    int is_why = !strcmp(operation, "why"), stats = !strcmp(operation, "stats"), adoption = !strcmp(operation, "adoption");
    int chain_mode = is_why || stats || adoption || !strcmp(operation, "cmds") ||
        !strcmp(operation, "did") || !strcmp(operation, "history") || !strcmp(operation, "failed");
    if (!chain_mode) return recall_native_overflow(call, argc, argv);
    recall_native_filter_t filter = {.limit = 20, .failed = !strcmp(operation, "failed"), .global = getenv("JASH_RECALL_GLOBAL") != NULL};
    if (recall_native_options(argc - 1, argv + 1, &filter) || (is_why && (!filter.pattern || !*filter.pattern ||
            filter.failed || filter.ok || filter.offset || filter.limit != 20))) return semantic_error(call, "invalid_arguments", 2);
    memory_chain_t chain; int error = 0;
    if (memory_chain_load(&chain, filter.global, 0)) error = chain.error;
    semantic_value_t* sem = &call->values;
    jdv_value_t results[500], analysis = jdv_value_nothing(); size_t matched = 0, emitted = 0;
    if (!error) {
        if (is_why) analysis = recall_native_why(sem, &chain, &filter, &matched, &error);
        else if (stats || adoption) analysis = recall_native_aggregate(sem, &chain, &filter, adoption, results, &emitted, &matched, &error);
        else for (size_t i = chain.count; i > 0; i--) {
            memory_chain_entry_t* entry = &chain.entries[i - 1];
            if (!recall_native_matches(&filter, entry)) continue;
            size_t index = matched++;
            if (index >= filter.offset && emitted < filter.limit) results[emitted++] = memory_chain_value(sem, entry);
        }
    }
    int more = !error && !is_why && filter.offset + emitted < matched;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, is_why ? "why" : stats ? "stats" : adoption ? "adoption" : "commands")),
        semantic_field("source", memory_chain_source(sem, &chain)),
        semantic_field("results", semantic_list(sem, results, emitted)),
        semantic_field("analysis", analysis),
        semantic_field("matched", error ? jdv_value_nothing() : jdv_value_u64(matched)),
        semantic_field("offset", jdv_value_u64(filter.offset)),
        semantic_field("next_offset", more ? jdv_value_u64(filter.offset + emitted) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(more)),
        semantic_field("error", semantic_symbol(sem, error ? memory_store_error(error) : NULL)),
        semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing())
    };
    int result = semantic_finish(call, semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0])), error ? 1 : 0);
    memory_chain_free(&chain); return result;
}

SEMANTIC_ARGV_WRAPPER(recall, "recall", "jash.recall.v1", recall_native_provider)
