// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_output.h"
#include "memory_chain.h"

static int journal_native_cutoff(const char* argument, uint64_t* cutoff) {
    time_t now = time(NULL);
    if (now < 0) return -1;
    if (!strcmp(argument, "today")) {
        struct tm local;
        if (!localtime_r(&now, &local)) return -1;
        local.tm_hour = local.tm_min = local.tm_sec = 0; local.tm_isdst = -1;
        time_t midnight = mktime(&local);
        if (midnight < 0) return -1;
        *cutoff = (uint64_t)midnight; return 0;
    }
    char number[32]; size_t size = strlen(argument); uint64_t multiplier = 1, duration;
    if (!size || size >= sizeof(number)) return -1;
    memcpy(number, argument, size + 1);
    char suffix = number[size - 1];
    if (suffix < '0' || suffix > '9') {
        if (suffix == 's' || suffix == 'S') multiplier = 1;
        else if (suffix == 'm' || suffix == 'M') multiplier = 60;
        else if (suffix == 'h' || suffix == 'H') multiplier = 3600;
        else if (suffix == 'd' || suffix == 'D') multiplier = 86400;
        else return -1;
        number[--size] = 0;
    }
    if (memory_unsigned(number, UINT64_MAX / multiplier, &duration)) return -1;
    duration *= multiplier; *cutoff = duration > (uint64_t)now ? 0 : (uint64_t)now - duration; return 0;
}

static int journal_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* pattern = NULL; const char* tag = NULL;
    uint64_t offset = 0, limit = 20, cutoff = 0;
    int failed_only = 0, transitions = 0, global = 0, literal = 0;
    unsigned seen = 0;
    for (int i = 0; i < argc; i++) {
        const char* argument = argv[i];
        if (!literal && !strcmp(argument, "--")) { literal = 1; continue; }
        if (!literal && !strcmp(argument, "--failed")) { failed_only = 1; continue; }
        if (!literal && !strcmp(argument, "--global")) { global = 1; continue; }
        if (!literal && (!strcmp(argument, "--what-happened") || !strcmp(argument, "--what"))) { transitions = 1; continue; }
        unsigned bit = 0; const char* value = NULL;
        if (!literal && (!strcmp(argument, "--last") || !strcmp(argument, "--limit"))) bit = 1;
        else if (!literal && !strcmp(argument, "--offset")) bit = 2;
        else if (!literal && !strcmp(argument, "--tag")) bit = 4;
        else if (!literal && !strcmp(argument, "--since")) bit = 8;
        else if (!literal && !strncmp(argument, "--last=", 7)) { bit = 1; value = argument + 7; }
        else if (!literal && !strncmp(argument, "--limit=", 8)) { bit = 1; value = argument + 8; }
        else if (!literal && !strncmp(argument, "--offset=", 9)) { bit = 2; value = argument + 9; }
        else if (!literal && !strncmp(argument, "--tag=", 6)) { bit = 4; value = argument + 6; }
        else if (!literal && !strncmp(argument, "--since=", 8)) { bit = 8; value = argument + 8; }
        if (bit) {
            if (seen & bit) return semantic_error(call, "invalid_arguments", 2);
            seen |= bit;
            if (!value) { if (++i == argc) return semantic_error(call, "invalid_arguments", 2); value = argv[i]; }
            if ((bit == 1 && (memory_unsigned(value, JOURNAL_MAX_SHOW, &limit) || !limit)) ||
                (bit == 2 && memory_unsigned(value, MEMORY_CHAIN_ROWS, &offset)) ||
                (bit == 4 && !*value) || (bit == 8 && journal_native_cutoff(value, &cutoff)))
                return semantic_error(call, "invalid_arguments", 2);
            if (bit == 4) tag = value;
        } else if (pattern || (!literal && argument[0] == '-')) return semantic_error(call, "invalid_arguments", 2);
        else pattern = argument;
    }
    if (transitions && (pattern || tag || failed_only || cutoff || global)) return semantic_error(call, "invalid_arguments", 2);
    semantic_value_t* sem = &call->values;
    jdv_value_t entries[JOURNAL_MAX_SHOW], tags[JOURNAL_HIST_MAX];
    size_t matched = 0, emitted = 0, tag_count = 0; int status = 0;
    memory_chain_t chain = {0};
    if (transitions) {
        journal_tag_history_t history;
        journal_tag_history_get(&history, JOURNAL_HIST_MAX);
        if (history.count < 0 || history.count > JOURNAL_HIST_MAX) return semantic_error(call, "invalid_tag_history", 1);
        matched = (size_t)history.count;
        static const char* const kinds[] = {"session_start", "domain", "switched", "error", "retry", "resumed", "sudo"};
        for (size_t i = (size_t)offset; i < matched && tag_count < limit; i++) {
            journal_entry_t* entry = &history.items[matched - 1 - i];
            jdv_record_input_t fields[] = {
                semantic_field("kind", semantic_symbol(sem, (unsigned)entry->kind < sizeof(kinds) / sizeof(kinds[0]) ? kinds[entry->kind] : "unknown")),
                semantic_field("tag", semantic_text(sem, entry->tag)),
                semantic_field("domain", semantic_text(sem, entry->domain)),
                semantic_field("error_count", jdv_value_u64(entry->error_count))
            };
            tags[tag_count++] = semantic_record(sem, fields, 4);
        }
    } else {
        status = memory_chain_load(&chain, global, tag != NULL) ? 1 : 0;
        if (!status) for (size_t i = chain.count; i; i--) {
            memory_chain_entry_t* entry = &chain.entries[i - 1];
            if ((failed_only && !entry->status) || (tag && strcmp(tag, entry->tag)) ||
                (cutoff && entry->seconds < cutoff) || (pattern && !jash_strcasestr_portable(entry->command, pattern))) continue;
            matched++;
            if (matched <= offset || emitted >= limit) continue;
            entries[emitted++] = memory_chain_value(sem, entry);
        }
    }
    size_t shown = transitions ? tag_count : emitted;
    int truncated = !status && offset + shown < matched;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.journal.v1")),
        semantic_field("operation", semantic_symbol(sem, transitions ? "transitions" : "entries")),
        semantic_field("source_scope", semantic_symbol(sem, transitions ? "process_tag_history" : tag ? "global_tag_sidecar" : global ? "global_chain" : "repository_selected_chain")),
        semantic_field("source", transitions ? jdv_value_nothing() : memory_chain_source(sem, &chain)),
        semantic_field("entries", semantic_list(sem, entries, emitted)),
        semantic_field("transitions", semantic_list(sem, tags, tag_count)),
        semantic_field("matched", status ? jdv_value_nothing() : jdv_value_u64(matched)),
        semantic_field("offset", jdv_value_u64(offset)),
        semantic_field("next_offset", truncated ? jdv_value_u64(offset + shown) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(truncated)),
        semantic_field("since_timestamp", (seen & 8) ? jdv_value_u64(cutoff) : jdv_value_nothing()),
        semantic_field("error", semantic_symbol(sem, status ? "source_read_failed" : NULL))
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    memory_chain_free(&chain); return semantic_finish(call, result, status);
}

SEMANTIC_ARGV_WRAPPER(journal, "journal", "jash.journal.v1", journal_native_provider)
