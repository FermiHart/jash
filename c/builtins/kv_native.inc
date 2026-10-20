// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Native KV modes use the documented TSV store itself, not its human output. */
#include "memory_output.h"
#include "memory_store.h"
#include "memory_receipt.h"

#define KV_NATIVE_ENTRIES 4096u
#define KV_NATIVE_VALUE 8192u
typedef struct { char* key; char* value; } kv_native_entry_t;

static int kv_native_key(const char* key) {
    size_t length = strlen(key);
    return length > 0 && length <= 255 && !strchr(key, '\t') && !strchr(key, '\n') && !strchr(key, '\r');
}

static int kv_native_parse(char* bytes, size_t length, kv_native_entry_t* entries, size_t* count) {
    *count = 0;
    if (memchr(bytes, 0, length)) { errno = EINVAL; return -1; }
    size_t offset = 0;
    while (offset < length) {
        char* row = bytes + offset;
        char* newline = memchr(row, '\n', length - offset);
        if (!newline || *count == KV_NATIVE_ENTRIES) { errno = newline ? E2BIG : EINVAL; return -1; }
        *newline = 0; offset = (size_t)(newline - bytes) + 1;
        char* tab = strchr(row, '\t');
        if (!tab) { errno = EINVAL; return -1; }
        *tab++ = 0;
        if (!kv_native_key(row)) { errno = EINVAL; return -1; }
        for (size_t i = 0; i < *count; i++)
            if (!strcmp(row, entries[i].key)) { errno = EINVAL; return -1; }
        char* source = tab; char* target = tab;
        while (*source) {
            char byte = *source++;
            if (byte == '\\') {
                byte = *source++;
                if (byte == 'n') byte = '\n';
                else if (byte == 't') byte = '\t';
                else if (byte != '\\') { errno = EINVAL; return -1; }
            } else if (byte == '\t') { errno = EINVAL; return -1; }
            if ((size_t)(target - tab) == KV_NATIVE_VALUE) { errno = E2BIG; return -1; }
            *target++ = byte;
        }
        *target = 0;
        entries[(*count)++] = (kv_native_entry_t){row, tab};
    }
    return 0;
}

static int kv_native_encode(char* bytes, size_t* used, const char* key, const char* value) {
    size_t key_length = strlen(key);
    if (key_length + 2 > MEMORY_STORE_MAX_BYTES - *used) return -1;
    memcpy(bytes + *used, key, key_length); *used += key_length; bytes[(*used)++] = '\t';
    for (const unsigned char* p = (const unsigned char*)value; *p; p++) {
        int escape = *p == '\n' || *p == '\t' || *p == '\\';
        if ((size_t)(escape ? 3 : 2) > MEMORY_STORE_MAX_BYTES - *used) return -1;
        if (escape) {
            bytes[(*used)++] = '\\';
            bytes[(*used)++] = *p == '\n' ? 'n' : *p == '\t' ? 't' : '\\';
        } else bytes[(*used)++] = (char)*p;
    }
    bytes[(*used)++] = '\n'; return 0;
}

static int kv_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    (void)state;
    const char* operation = argc ? argv[0] : "list";
    int listing = !strcmp(operation, "list"), getting = !strcmp(operation, "get");
    int setting = !strcmp(operation, "set"), deleting = !strcmp(operation, "del");
    const char* key = listing ? NULL : argc > 1 ? argv[1] : NULL;
    uint64_t offset = 0, limit = 64;
    if ((!listing && !getting && !setting && !deleting) ||
        (!listing && (argc != (setting ? 3 : 2) || !key || !kv_native_key(key))) ||
        (setting && strlen(argv[2]) > KV_NATIVE_VALUE)) return semantic_error(call, "invalid_arguments", 2);
    if (listing) {
        int have_offset = 0, have_limit = 0;
        for (int i = argc ? 1 : 0; i < argc; i++) {
            if (!strcmp(argv[i], "--offset") && !have_offset && i + 1 < argc) {
                if (memory_unsigned(argv[++i], KV_NATIVE_ENTRIES, &offset)) return semantic_error(call, "invalid_arguments", 2);
                have_offset = 1;
            } else if (!strcmp(argv[i], "--limit") && !have_limit && i + 1 < argc) {
                if (memory_unsigned(argv[++i], 256, &limit) || !limit) return semantic_error(call, "invalid_arguments", 2);
                have_limit = 1;
            } else return semantic_error(call, "invalid_arguments", 2);
        }
    }
    int mutating = setting || deleting;
    /* 255 key + 8192 value bytes, at most six JSON bytes per source byte,
     * plus fixed fields/error/counters, fit within the 64KiB reservation. */
    if (mutating) { int admitted = memory_receipt_admit(call, 65536u); if (admitted) return admitted; }
    memory_store_t store = {-1, -1}; memory_write_t write_result = {0};
    char* bytes = NULL; size_t length = 0, count = 0; int exists = 0, saved = 0;
    const char* error = NULL; int status = 0, read_complete = 0;
    kv_native_entry_t* entries = calloc(KV_NATIVE_ENTRIES, sizeof(*entries));
    if (!entries) { saved = ENOMEM; error = "allocation_failed"; status = 125; }
    else if (memory_store_open(&store, NULL, mutating) ||
             memory_store_read(&store, "kv.db", MEMORY_STORE_MAX_BYTES, &bytes, &length, &exists)) {
        saved = errno; error = memory_store_error(saved); status = 1;
    } else if (exists && kv_native_parse(bytes, length, entries, &count)) {
        saved = errno; error = memory_store_error(saved); status = 1;
    } else read_complete = 1;
    size_t found_index = count;
    if (!status && key) for (size_t i = 0; i < count; i++)
        if (!strcmp(key, entries[i].key)) { found_index = i; break; }
    int found = found_index < count;
    const char* value = found ? entries[found_index].value : NULL;
    if (!status && getting && !found) { error = "not_found"; status = 1; }
    if (!status && mutating) {
        if (setting && !found && count == KV_NATIVE_ENTRIES) { saved = E2BIG; error = "store_limit"; status = 1; }
        else if (deleting && !found) write_result.durable = 1;
        else {
            char* encoded = malloc(MEMORY_STORE_MAX_BYTES); size_t used = 0;
            if (!encoded) { saved = ENOMEM; error = "allocation_failed"; status = 125; }
            for (size_t i = 0; encoded && i < count && !status; i++) {
                if (i == found_index) continue;
                if (kv_native_encode(encoded, &used, entries[i].key, entries[i].value)) {
                    saved = E2BIG; error = "store_limit"; status = 1;
                }
            }
            if (!status && setting && kv_native_encode(encoded, &used, key, argv[2])) {
                saved = E2BIG; error = "store_limit"; status = 1;
            }
            if (!status) {
                write_result = memory_store_publish(&store, "kv.db", encoded, used);
                if (write_result.error) { saved = write_result.error; error = memory_store_error(saved); status = 1; }
                if (write_result.applied) {
                    if (setting) { value = argv[2]; if (!found) count++; found = 1; }
                    else { value = NULL; count--; found = 0; }
                }
            }
            free(encoded);
        }
    }
    if (memory_store_close(&store) && !status) { saved = errno; error = "store_close_failed"; status = 1; }
    semantic_value_t* sem = &call->values;
    jdv_value_t page[256]; size_t page_count = 0;
    if (listing && !status) for (size_t i = (size_t)offset; i < count && page_count < limit; i++) {
        jdv_record_input_t fields[] = {semantic_field("key", semantic_text(sem, entries[i].key)),
                                       semantic_field("value", semantic_text(sem, entries[i].value))};
        page[page_count++] = semantic_record(sem, fields, 2);
    }
    int truncated = listing && !status && offset + page_count < count;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.kv.v1")),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("key", semantic_text(sem, key)),
        semantic_field("found", key && read_complete ? jdv_value_bool(found) : jdv_value_nothing()),
        semantic_field("value", semantic_text(sem, value)),
        semantic_field("entries", semantic_list(sem, page, page_count)),
        semantic_field("total", read_complete ? jdv_value_u64(count) : jdv_value_nothing()),
        semantic_field("read_complete", jdv_value_bool(read_complete)),
        semantic_field("offset", jdv_value_u64(offset)),
        semantic_field("next_offset", truncated ? jdv_value_u64(offset + page_count) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(truncated)),
        semantic_field("applied", jdv_value_bool(write_result.applied)),
        semantic_field("durable", jdv_value_bool(write_result.durable)),
        semantic_field("consistency", semantic_symbol(sem, "atomic_file_snapshot")),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("errno", saved ? jdv_value_i64(saved) : jdv_value_nothing())
    };
    jdv_value_t result = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
    free(bytes); free(entries); return semantic_finish(call, result, status);
}

SEMANTIC_ARGV_WRAPPER(kv, "kv", "jash.kv.v1", kv_native_provider)
