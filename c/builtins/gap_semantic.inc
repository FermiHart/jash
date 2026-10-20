// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "memory_chain.h"
#include "semantic_events.h"
#include "gap_snapshot.h"
#include <core/query_cancel.h>
#include <inttypes.h>
#include <ctype.h>

typedef struct {
    char sig[17]; int fixed;
    uint64_t count;
    int64_t first, last;
    const char *tried, *expected, *prove;
} gap_record_t;
typedef struct {
    gap_record_t rows[GAP_MAX]; size_t count;
    char *raw, *parsed; size_t size;
    int present, directory_present, error, fleet;
    char path[PATH_MAX];
} gap_table_t;
typedef struct { jash_state_t* state; int cancelled; } gap_control_t;

static int gap_poll(void* context) {
    gap_control_t* control = context;
    if (!control->cancelled) control->cancelled = jash_query_cancel_poll(control->state);
    return control->cancelled;
}

static void gap_table_free(gap_table_t* table) {
    free(table->raw); free(table->parsed); table->raw = table->parsed = NULL;
}

static int gap_hex_sig(const char* value) {
    if (strlen(value) != 16u) return 0;
    for (size_t i = 0; i < 16u; i++)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f'))) return 0;
    return 1;
}

static int gap_fields(char* line, char** fields, size_t count) {
    for (size_t i = 0; i < count; i++) {
        fields[i] = line;
        char* tab = strchr(line, '\t');
        if (i + 1u == count) return tab ? -1 : 0;
        if (!tab) return -1;
        *tab = 0; line = tab + 1;
    }
    return -1;
}

static int gap_table_parse(gap_table_t* table) {
    if (!table->size) return 0;
    if (memchr(table->raw, 0, table->size) || memchr(table->raw, '\r', table->size) ||
        table->raw[table->size - 1u] != '\n') return EINVAL;
    table->parsed = malloc(table->size + 1u);
    if (!table->parsed) return ENOMEM;
    memcpy(table->parsed, table->raw, table->size + 1u);
    char* cursor = table->parsed;
    while (*cursor) {
        if (table->count == GAP_MAX) return E2BIG;
        char* line = cursor; char* newline = strchr(line, '\n');
        if (!newline) return EINVAL;
        *newline = 0; cursor = newline + 1;
        char* fields[8]; size_t field_count = table->fleet ? 4u : 8u;
        if (gap_fields(line, fields, field_count) || !gap_hex_sig(fields[0])) return EINVAL;
        for (size_t i = 0; i < table->count; i++) if (!strcmp(table->rows[i].sig, fields[0])) return EINVAL;
        gap_record_t* row = &table->rows[table->count];
        memcpy(row->sig, fields[0], 17u);
        uint64_t first = 0, last = 0;
        if (table->fleet) {
            if (memory_unsigned(fields[1], UINT64_MAX, &row->count) || !row->count ||
                memory_unsigned(fields[2], INT64_MAX, &last) || !fields[3][0] || strlen(fields[3]) > 511u) return EINVAL;
            row->tried = fields[3]; row->expected = row->prove = "";
        } else {
            if (strcmp(fields[1], "open") && strcmp(fields[1], "fixed")) return EINVAL;
            if (memory_unsigned(fields[2], UINT64_MAX, &row->count) || !row->count ||
                memory_unsigned(fields[3], INT64_MAX, &first) || memory_unsigned(fields[4], INT64_MAX, &last) ||
                !fields[5][0] || strlen(fields[5]) > 511u || strlen(fields[6]) > 255u || strlen(fields[7]) > 255u) return EINVAL;
            row->fixed = !strcmp(fields[1], "fixed");
            row->tried = fields[5]; row->expected = fields[6]; row->prove = fields[7];
        }
        row->first = (int64_t)first; row->last = (int64_t)last; table->count++;
    }
    return 0;
}

static int gap_table_read(gap_table_t* table, int fleet) {
    memset(table, 0, sizeof(*table)); table->fleet = fleet;
    const char* home = getenv("HOME");
    int written = snprintf(table->path, sizeof(table->path), "%s/.jash/%sgaps.tsv", home ? home : "", fleet ? "mesh/" : "");
    if (written < 0 || (size_t)written >= sizeof(table->path)) { table->error = E2BIG; return 75; }
    memory_store_t store = {-1, -1};
    if (memory_store_open(&store, fleet ? "mesh" : NULL, 0)) table->error = errno;
    else {
        table->directory_present = store.directory >= 0;
        if (memory_store_read(&store, "gaps.tsv", MEMORY_STORE_MAX_BYTES, &table->raw,
                              &table->size, &table->present)) table->error = errno;
    }
    if (memory_store_close(&store) && !table->error) table->error = errno;
    if (!table->error) table->error = gap_table_parse(table);
    return table->error ? table->error == E2BIG ? 75 : 1 : 0;
}

static jdv_value_t gap_source(semantic_value_t* sem, const gap_table_t* table) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, table->path)),
        semantic_field("present", jdv_value_bool(table->present)),
        semantic_field("directory_present", jdv_value_bool(table->directory_present)),
        semantic_field("read_complete", jdv_value_bool(!table->error)),
        semantic_field("bytes", jdv_value_filesize(table->size)),
        semantic_field("records", table->error ? jdv_value_nothing() : jdv_value_u64(table->count)),
        semantic_field("format", semantic_symbol(sem, "legacy-tsv-v1")),
        semantic_field("error", semantic_symbol(sem, table->error ? memory_store_error(table->error) : NULL)),
        semantic_field("errno", table->error ? jdv_value_i64(table->error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, 9);
}

int jash_gap_observe(semantic_value_t* values, jdv_value_t* result) {
    if (!values || !result) { errno = EINVAL; return 2; }
    gap_table_t* table = calloc(1, sizeof(*table));
    if (!table) return 125;
    int status = gap_table_read(table, 0);
    uint64_t open = 0, fixed = 0;
    if (!status) for (size_t i = 0; i < table->count; i++) {
        if (table->rows[i].fixed) fixed++; else open++;
    }
    jdv_record_input_t fields[] = {
        semantic_field("source", gap_source(values, table)),
        semantic_field("open", status ? jdv_value_nothing() : jdv_value_u64(open)),
        semantic_field("fixed", status ? jdv_value_nothing() : jdv_value_u64(fixed)),
        semantic_field("status_basis", semantic_symbol(values, "stored-status-not-reverified"))
    };
    *result = semantic_record(values, fields, 4);
    gap_table_free(table); free(table); return values->error ? 125 : status;
}

static int gap_index(const gap_table_t* table, const char* sig) {
    for (size_t i = 0; i < table->count; i++) if (!strcmp(table->rows[i].sig, sig)) return (int)i;
    return -1;
}

static jdv_value_t gap_value(semantic_value_t* sem, const gap_record_t* row, const gap_table_t* fleet) {
    int index = fleet ? gap_index(fleet, row->sig) : -1;
    jdv_record_input_t fields[] = {
        semantic_field("sig", semantic_symbol(sem, row->sig)),
        semantic_field("status", semantic_symbol(sem, row->fixed ? "fixed" : "open")),
        semantic_field("count", jdv_value_u64(row->count)),
        semantic_field("first_ts", jdv_value_i64(row->first)),
        semantic_field("last_ts", jdv_value_i64(row->last)),
        semantic_field("tried", semantic_text(sem, row->tried)),
        semantic_field("expected", semantic_text(sem, row->expected)),
        semantic_field("prove", semantic_text(sem, row->prove)),
        semantic_field("fleet", fleet && !fleet->error ? jdv_value_u64(index < 0 ? 0 : fleet->rows[index].count) : jdv_value_nothing()),
        semantic_field("status_basis", semantic_symbol(sem, "stored-status-not-reverified"))
    };
    return semantic_record(sem, fields, 10);
}

int jash_gap_observe_rows(semantic_value_t* values, jdv_value_t* source,
                         jdv_value_t* rows) {
    if (!values || !source || !rows) { errno = EINVAL; return 2; }
    *source = *rows = jdv_value_nothing();
    gap_table_t* table = calloc(1, sizeof(*table));
    if (!table) return 125;
    int status = gap_table_read(table, 0);
    *source = gap_source(values, table);
    jdv_value_t items[GAP_MAX];
    size_t count = status ? 0 : table->count;
    for (size_t i = 0; i < count; i++) items[i] = gap_value(values, &table->rows[i], NULL);
    *rows = semantic_list(values, items, count);
    gap_table_free(table); free(table);
    return values->error ? 125 : status;
}

static int gap_semantic_list(semantic_builtin_t* call, int all) {
    gap_table_t* tables = calloc(2, sizeof(*tables)); if (!tables) return 125;
    int status = gap_table_read(&tables[0], 0), fleet_status = gap_table_read(&tables[1], 1);
    if (!status) status = fleet_status;
    size_t order[GAP_MAX], count = 0;
    if (!status) for (size_t i = 0; i < tables[0].count; i++) {
        if (!all && tables[0].rows[i].fixed) continue;
        size_t at = count;
        while (at && (tables[0].rows[order[at - 1u]].count < tables[0].rows[i].count ||
            (tables[0].rows[order[at - 1u]].count == tables[0].rows[i].count &&
             tables[0].rows[order[at - 1u]].last < tables[0].rows[i].last))) {
            order[at] = order[at - 1u]; at--;
        }
        order[at] = i; count++;
    }
    jdv_value_t rows[GAP_MAX];
    for (size_t i = 0; i < count; i++) rows[i] = gap_value(&call->values, &tables[0].rows[order[i]], &tables[1]);
    jdv_record_input_t source[] = {semantic_field("local", gap_source(&call->values, &tables[0])),
        semantic_field("fleet", gap_source(&call->values, &tables[1]))};
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("operation", semantic_symbol(&call->values, "list")),
        semantic_field("row_kind", semantic_symbol(&call->values, "result")),
        semantic_field("complete", jdv_value_bool(!status)),
        semantic_field("include_fixed", jdv_value_bool(all)),
        semantic_field("sources", semantic_record(&call->values, source, 2)),
        semantic_field("consistency", semantic_symbol(&call->values, "independent-file-observations")),
        semantic_field("entries", semantic_list(&call->values, rows, count))
    };
    jdv_value_t value = semantic_record(&call->values, fields, 8);
    gap_table_free(&tables[0]); gap_table_free(&tables[1]); free(tables);
    return semantic_finish(call, value, status);
}

static int gap_semantic_scan(semantic_builtin_t* call) {
    memory_chain_t chain;
    int status = memory_chain_load(&chain, getenv("JASH_RECALL_GLOBAL") != NULL, 0) ? 1 : 0;
    typedef struct { char key[160]; uint64_t count; } cluster_t;
    cluster_t clusters[64] = {0}; size_t count = 0; uint64_t total = 0;
    if (!status) for (size_t i = 0; i < chain.count; i++) {
        if (chain.entries[i].status != 2) continue;
        total++;
        const char* cursor = chain.entries[i].command; char key[160]; size_t length = 0;
        for (unsigned word = 0; word < 2u; word++) {
            while (*cursor && isspace((unsigned char)*cursor)) cursor++;
            if (!*cursor) break;
            if (length) key[length++] = ' ';
            size_t word_size = 0;
            while (*cursor && !isspace((unsigned char)*cursor)) {
                if (word_size == 63u || length + 1u >= sizeof(key)) { status = 75; break; }
                key[length++] = *cursor++; word_size++;
            }
            if (status) break;
        }
        if (status) break;
        key[length] = 0;
        size_t at = 0; while (at < count && strcmp(clusters[at].key, key)) at++;
        if (at == count) {
            if (count == 64u) { status = 75; break; }
            memcpy(clusters[at].key, key, length + 1u); count++;
        }
        clusters[at].count++;
    }
    for (size_t i = 1; i < count; i++) {
        cluster_t value = clusters[i]; size_t at = i;
        while (at && clusters[at - 1u].count < value.count) { clusters[at] = clusters[at - 1u]; at--; }
        clusters[at] = value;
    }
    jdv_value_t rows[64];
    for (size_t i = 0; !status && i < count; i++) {
        jdv_record_input_t fields[] = {semantic_field("misuse", semantic_text(&call->values, clusters[i].key)),
            semantic_field("count", jdv_value_u64(clusters[i].count))};
        rows[i] = semantic_record(&call->values, fields, 2);
    }
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(&call->values, call->schema_id)),
        semantic_field("operation", semantic_symbol(&call->values, "scan")),
        semantic_field("row_kind", semantic_symbol(&call->values, "result")),
        semantic_field("source", memory_chain_source(&call->values, &chain)),
        semantic_field("complete", jdv_value_bool(!status)),
        semantic_field("total_status_two", status ? jdv_value_nothing() : jdv_value_u64(total)),
        semantic_field("clusters", semantic_list(&call->values, rows, status ? 0 : count)),
        semantic_field("classification", semantic_symbol(&call->values, "first-two-whitespace-words-status-two")),
        semantic_field("capability_gap_confirmed", jdv_value_bool(0)),
        semantic_field("error", semantic_symbol(&call->values, status ? status == 75 ? "cluster_limit" : "chain_unavailable" : NULL))
    };
    jdv_value_t value = semantic_record(&call->values, fields, 10);
    memory_chain_free(&chain); return semantic_finish(call, value, status);
}

static int gap_serialise(const gap_table_t* table, jash_structured_buffer_t* out) {
    jash_structured_buffer_init(out, MEMORY_STORE_MAX_BYTES);
    for (size_t i = 0; i < table->count; i++) {
        const gap_record_t* row = &table->rows[i]; char prefix[128];
        int size = table->fleet ? snprintf(prefix, sizeof(prefix), "%s\t%" PRIu64 "\t%" PRId64 "\t",
            row->sig, row->count, row->last) :
            snprintf(prefix, sizeof(prefix), "%s\t%s\t%" PRIu64 "\t%" PRId64 "\t%" PRId64 "\t",
                row->sig, row->fixed ? "fixed" : "open", row->count, row->first, row->last);
        if (size < 0 || (size_t)size >= sizeof(prefix) ||
            jash_structured_buffer_sink(out, prefix, (size_t)size) != size ||
            jash_structured_buffer_sink(out, row->tried, strlen(row->tried)) < 0) goto fail;
        if (!table->fleet && (jash_structured_buffer_sink(out, "\t", 1) < 0 ||
            jash_structured_buffer_sink(out, row->expected, strlen(row->expected)) < 0 ||
            jash_structured_buffer_sink(out, "\t", 1) < 0 ||
            jash_structured_buffer_sink(out, row->prove, strlen(row->prove)) < 0)) goto fail;
        if (jash_structured_buffer_sink(out, "\n", 1) < 0) goto fail;
    }
    if (out->error || out->required != out->used) goto fail;
    char* terminated = realloc(out->data, out->used + 1u);
    if (!terminated) { errno = ENOMEM; goto fail; }
    out->data = terminated; out->capacity = out->used + 1u; out->data[out->used] = 0; return 0;
fail:
    if (!errno) errno = E2BIG;
    jash_structured_buffer_free(out); return -1;
}

static int gap_legacy_lock(memory_store_t* store, int fleet, gap_control_t* control) {
    int fd = openat(store->directory, fleet ? "gaps.tsv.lock" : "gaps.lock",
                    O_RDWR | O_CREAT | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    int failed = memory_store_trusted(fd, 0) ? errno : 0;
    while (!failed) {
        if (gap_poll(control)) { failed = ECANCELED; break; }
        if (!flock(fd, LOCK_EX | LOCK_NB)) return fd;
        if (errno == EINTR) continue;
        if (errno != EWOULDBLOCK && errno != EAGAIN) { failed = errno; break; }
        struct timespec pause = {0, 10000000}; (void)nanosleep(&pause, NULL);
    }
    (void)close(fd); errno = failed; return -1;
}

/* Publish only if the source is still the exact snapshot that was validated
 * and budgeted. Hold both the native and legacy writer locks for this one
 * publication, then release them before any proof/attestation callback. */
static memory_write_t gap_publish(gap_table_t* table, gap_control_t* control) {
    memory_write_t result = {0}; jash_structured_buffer_t bytes;
    if (gap_serialise(table, &bytes)) { result.error = errno; return result; }
    memory_store_t store = {-1, -1}; int legacy = -1;
    if (gap_poll(control)) result.error = ECANCELED;
    else if (memory_store_open_cooperative(&store, table->fleet ? "mesh" : NULL, 1, gap_poll, control)) result.error = errno;
    else if ((legacy = gap_legacy_lock(&store, table->fleet, control)) < 0) result.error = errno;
    char* observed = NULL; size_t size = 0; int exists = 0;
    if (!result.error && memory_store_read(&store, "gaps.tsv", MEMORY_STORE_MAX_BYTES, &observed, &size, &exists)) result.error = errno;
    if (!result.error && (exists != table->present || size != table->size ||
        (size && memcmp(observed, table->raw, size)))) result.error = ESTALE;
    free(observed);
    if (!result.error && gap_poll(control)) result.error = ECANCELED;
    if (!result.error) result = memory_store_publish(&store, "gaps.tsv", bytes.data, bytes.used);
    if (legacy >= 0) {
        if (flock(legacy, LOCK_UN) && !result.error) result.error = errno;
        if (close(legacy) && !result.error) result.error = errno;
    }
    if (memory_store_close(&store) && !result.error) result.error = errno;
    if (result.applied) {
        free(table->raw); table->raw = bytes.data; bytes.data = NULL;
        table->size = bytes.used; table->present = table->directory_present = 1;
    }
    jash_structured_buffer_free(&bytes); return result;
}

static jdv_value_t gap_receipt(semantic_builtin_t* call, const char* operation,
        const char* action, const gap_record_t* row, const char* target,
        int callback, int callback_status, const memory_write_t* publication, int worst) {
    semantic_value_t* sem = &call->values;
    int error = publication ? publication->error : 0;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("row_kind", semantic_symbol(sem, "receipt")),
        semantic_field("action", semantic_symbol(sem, action)),
        semantic_field("sig", semantic_symbol(sem, row->sig)),
        semantic_field("target", semantic_path(sem, target)),
        semantic_field("callback_invoked", jdv_value_bool(callback)),
        semantic_field("command_status", callback && callback_status >= 0 ? jdv_value_i64(callback_status) : jdv_value_nothing()),
        semantic_field("applied", publication ? jdv_value_bool(publication->applied) : jdv_value_nothing()),
        semantic_field("durable", publication ? jdv_value_bool(publication->durable) : jdv_value_nothing()),
        semantic_field("persistence_verified", publication ? jdv_value_bool(publication->applied && publication->durable) : jdv_value_nothing()),
        semantic_field("error", semantic_symbol(sem, worst ? "operation_failed_before_or_after_observed_effect_with_partial_state" :
            error ? memory_store_error(error) : callback_status > 0 ? "proof_failed" : NULL)),
        semantic_field("errno", error ? jdv_value_i64(error) : jdv_value_nothing()),
        semantic_field("effects_may_have_applied", jdv_value_bool(callback || (publication && publication->applied)))
    };
    return semantic_record(sem, fields, 14);
}

static uint64_t gap_open_count(const gap_table_t* table) {
    uint64_t count = 0;
    for (size_t i = 0; i < table->count; i++) if (!table->rows[i].fixed) count++;
    return count;
}

static jdv_value_t gap_action_result(semantic_builtin_t* call, const char* operation,
        const gap_record_t* row, const gap_table_t* fleet, int status, const char* error,
        uint64_t with_proof, uint64_t fixed_now, uint64_t still_open, int effects) {
    semantic_value_t* sem = &call->values;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, call->schema_id)),
        semantic_field("operation", semantic_symbol(sem, operation)),
        semantic_field("row_kind", semantic_symbol(sem, "result")),
        semantic_field("ok", jdv_value_bool(!status)),
        semantic_field("error", semantic_symbol(sem, error)),
        semantic_field("entry", row ? gap_value(sem, row, fleet) : jdv_value_nothing()),
        semantic_field("with_proof", jdv_value_u64(with_proof)),
        semantic_field("fixed_now", jdv_value_u64(fixed_now)),
        semantic_field("still_open", jdv_value_u64(still_open)),
        semantic_field("effects_may_have_applied", jdv_value_bool(effects)),
        semantic_field("attestation", semantic_symbol(sem, "callback-request-only"))
    };
    return semantic_record(sem, fields, 11);
}

static int gap_input_field(const char* text, size_t maximum, int empty) {
    return text && (empty || *text) && strlen(text) <= maximum && !strpbrk(text, "\t\n\r");
}

static void gap_native_sig(const char* tried, const char* expected, char out[17]) {
    uint64_t hash = UINT64_C(1469598103934665603);
    for (const unsigned char* p = (const unsigned char*)tried; *p; p++) { hash ^= *p; hash *= UINT64_C(1099511628211); }
    hash ^= 1u; hash *= UINT64_C(1099511628211);
    for (const unsigned char* p = (const unsigned char*)expected; *p; p++) { hash ^= *p; hash *= UINT64_C(1099511628211); }
    (void)snprintf(out, 17u, "%016" PRIx64, hash);
}

static int gap_prepare_report(gap_table_t* local, gap_table_t* fleet,
        const char* tried, const char* expected, const char* proof, int64_t now, int* selected) {
    char sig[17]; gap_native_sig(tried, expected, sig);
    int index = gap_index(local, sig);
    if (index < 0) {
        if (local->count == GAP_MAX) return E2BIG;
        index = (int)local->count++;
        local->rows[index] = (gap_record_t){.first = now, .expected = expected, .prove = ""};
        memcpy(local->rows[index].sig, sig, sizeof(sig));
    } else if (strcmp(local->rows[index].tried, tried) || strcmp(local->rows[index].expected, expected)) return EEXIST;
    gap_record_t* row = &local->rows[index];
    if (row->count == UINT64_MAX) return EOVERFLOW;
    row->count++; row->fixed = 0; row->last = now; row->tried = tried;
    if (proof) row->prove = proof;
    *selected = index;
    if (fleet->directory_present) {
        index = gap_index(fleet, sig);
        if (index < 0) {
            if (fleet->count == GAP_MAX) return E2BIG;
            index = (int)fleet->count++;
            fleet->rows[index] = (gap_record_t){.expected = "", .prove = ""};
            memcpy(fleet->rows[index].sig, sig, sizeof(sig));
        }
        if (fleet->rows[index].count == UINT64_MAX) return EOVERFLOW;
        fleet->rows[index].count++; fleet->rows[index].last = now; fleet->rows[index].tried = tried;
    }
    return 0;
}

static int gap_semantic_action(semantic_builtin_t* call, jash_state_t* state,
        const char* operation, int argc, char** argv) {
    int report = !strcmp(operation, "report"), verify = !strcmp(operation, "verify");
    const char* admission_error = NULL;
    const char *tried = NULL, *expected = "", *proof = NULL, *sig = NULL;
    int expected_seen = 0;
    if (report) {
        int literal = 0;
        for (int i = 0; i < argc; i++) {
            if (!literal && !strcmp(argv[i], "--")) { literal = 1; continue; }
            if (!literal && (!strcmp(argv[i], "--expected") || !strcmp(argv[i], "--prove"))) {
                int is_expected = !strcmp(argv[i], "--expected");
                if (++i == argc || (is_expected ? expected_seen : proof != NULL)) return semantic_error(call, "invalid_arguments", 2);
                if (is_expected) { expected = argv[i]; expected_seen = 1; } else proof = argv[i];
            } else if ((!literal && argv[i][0] == '-') || tried) return semantic_error(call, "invalid_arguments", 2);
            else tried = argv[i];
        }
        if (!gap_input_field(tried, 511u, 0) || !gap_input_field(expected, 255u, 1) ||
            (proof && !gap_input_field(proof, 255u, 1))) return semantic_error(call, "invalid_arguments", 2);
    } else if (verify) {
        if (argc) return semantic_error(call, "invalid_arguments", 2);
    } else {
        if (argc != 1 || !gap_hex_sig(argv[0])) return semantic_error(call, "invalid_arguments", 2);
        sig = argv[0];
    }
    gap_table_t* tables = calloc(2, sizeof(*tables)); if (!tables) return 125;
    gap_table_t *local = &tables[0], *fleet = &tables[1];
    int status = gap_table_read(local, 0), selected = -1;
    if (!status && report) status = gap_table_read(fleet, 1);
    if (status) {
        gap_table_free(local); gap_table_free(fleet); free(tables);
        return semantic_error(call, "store_unavailable", status);
    }
    gap_record_t original_local[GAP_MAX], original_fleet[GAP_MAX];
    size_t original_local_count = local->count, original_fleet_count = fleet->count;
    memcpy(original_local, local->rows, sizeof(original_local));
    memcpy(original_fleet, fleet->rows, sizeof(original_fleet));
    time_t clock = time(NULL); int64_t now = clock < 0 ? -1 : (int64_t)clock;
    if (now < 0) { status = 1; goto prepare_failed; }
    if (report) {
        int error = gap_prepare_report(local, fleet, tried, expected, proof, now, &selected);
        if (error) { status = error == E2BIG ? 75 : 1; goto prepare_failed; }
    } else if (!verify) {
        selected = gap_index(local, sig);
        if (selected < 0 || local->rows[selected].fixed) { status = 1; goto prepare_failed; }
        local->rows[selected].fixed = 1; local->rows[selected].last = now;
    }
    /* Validate every prospective file before sink admission creates lock files. */
    if (verify) {
        for (size_t i = 0; i < local->count; i++) if (!local->rows[i].fixed && local->rows[i].prove[0]) {
            local->rows[i].fixed = 1; local->rows[i].last = INT64_MAX;
        }
        jash_structured_buffer_t serial;
        int invalid = gap_serialise(local, &serial);
        memcpy(local->rows, original_local, sizeof(original_local));
        if (invalid) { status = 75; goto prepare_failed; }
        jash_structured_buffer_free(&serial);
    } else {
        jash_structured_buffer_t serial;
        if (gap_serialise(local, &serial)) { status = 75; goto prepare_failed; }
        jash_structured_buffer_free(&serial);
        if (report && fleet->directory_present) {
            if (gap_serialise(fleet, &serial)) { status = 75; goto prepare_failed; }
            jash_structured_buffer_free(&serial);
        }
    }
    size_t maximum = GAP_MAX * 3u + 1u;
    jdv_value_t* possible = calloc(maximum, sizeof(*possible));
    if (!possible) { status = 125; goto prepare_failed; }
    size_t count = 0;
    memory_write_t worst = {.error = INT_MAX};
    if (verify) {
        for (size_t i = 0; i < local->count; i++) {
            const gap_record_t* row = &local->rows[i];
            if (row->fixed || !row->prove[0]) continue;
            possible[count++] = gap_receipt(call, operation, "expect", row, NULL, 1, 255, NULL, 1);
            possible[count++] = gap_receipt(call, operation, "publish-local", row, local->path, 0, -1, &worst, 1);
            possible[count++] = gap_receipt(call, operation, "attest", row, NULL, 1, -1, NULL, 1);
        }
    } else {
        const gap_record_t* row = &local->rows[selected];
        possible[count++] = gap_receipt(call, operation, "publish-local", row, local->path, 0, -1, &worst, 1);
        if (report && fleet->directory_present) possible[count++] = gap_receipt(call, operation, "publish-fleet", row, fleet->path, 0, -1, &worst, 1);
        if (report) possible[count++] = gap_receipt(call, operation, "attest", row, NULL, 1, -1, NULL, 1);
    }
    possible[count++] = gap_action_result(call, operation, verify ? NULL : &local->rows[selected], report ? fleet : NULL,
        255, "operation_failed_before_or_after_observed_effect_with_partial_state", GAP_MAX, GAP_MAX, GAP_MAX, 0);
    int preflight = semantic_events_preflight(call, possible, count);
    free(possible); call->values.arena.used = 0; call->values.error = 0;
    if (preflight) { status = preflight; admission_error = preflight == 75 ? "output_budget_exceeded" : "output_or_value_limit"; goto prepare_failed; }
    if (semantic_admit_rows(call, count)) { status = 125; goto done; }
    gap_control_t control = {.state = state};
    uint64_t with_proof = 0, fixed_now = 0; int effects = 0, local_applied = 0, fleet_applied = 0;
    const char* failure = NULL;
    for (size_t i = 0; i < local->count; i++) {
        gap_record_t* row = &local->rows[i];
        if (!verify && (int)i != selected) continue;
        if (verify && (row->fixed || !row->prove[0])) continue;
        if (gap_poll(&control)) { status = control.cancelled; failure = "cancelled"; break; }
        gap_record_t before = *row;
        if (verify) {
            int proof_status = jash_b_expect(state, gap_null_writer, NULL, row->prove);
            with_proof++; effects = 1;
            if (semantic_events_emit(call, gap_receipt(call, operation, "expect", row, NULL, 1, proof_status, NULL, 0))) { status = 125; goto done; }
            call->values.arena.used = 0;
            if (proof_status) continue;
            row->fixed = 1; row->last = now;
        }
        effects = 1; /* publication can also create trusted directory/lock metadata */
        memory_write_t published = gap_publish(local, &control);
        if (verify && !published.applied) *row = before;
        if (published.applied) { local_applied = 1; if (verify) fixed_now++; }
        if (semantic_events_emit(call, gap_receipt(call, operation, "publish-local", row, local->path, 0, -1, &published, 0))) { status = 125; goto done; }
        call->values.arena.used = 0;
        if (published.error) { status = control.cancelled ? control.cancelled : published.error == E2BIG ? 75 : 1; failure = memory_store_error(published.error); break; }
        if (report && fleet->directory_present) {
            published = gap_publish(fleet, &control);
            if (published.applied) fleet_applied = 1;
            if (semantic_events_emit(call, gap_receipt(call, operation, "publish-fleet", row, fleet->path, 0, -1, &published, 0))) { status = 125; goto done; }
            call->values.arena.used = 0;
            if (published.error) { status = control.cancelled ? control.cancelled : published.error == E2BIG ? 75 : 1; failure = memory_store_error(published.error); break; }
        }
        if (report || verify) {
            if (gap_poll(&control)) { status = control.cancelled; failure = "cancelled"; break; }
            char detail[1200];
            int size = snprintf(detail, sizeof(detail), "sig=%s count=%" PRIu64 " tried=%s expected=%s prove=%s",
                row->sig, row->count, row->tried, row->expected, row->prove);
            if (size < 0 || (size_t)size >= sizeof(detail)) { status = 75; failure = "attestation_detail_limit"; break; }
            jash_sigil_attest(verify ? "gap-fixed" : "gap", detail); effects = 1;
            if (semantic_events_emit(call, gap_receipt(call, operation, "attest", row, NULL, 1, -1, NULL, 0))) { status = 125; goto done; }
            call->values.arena.used = 0;
        }
    }
    if (control.cancelled) { call->cancellation_status = control.cancelled; status = control.cancelled; failure = "cancelled"; }
    if (!verify && !local_applied) {
        memcpy(local->rows, original_local, sizeof(original_local)); local->count = original_local_count;
    }
    if (report && !fleet_applied) {
        memcpy(fleet->rows, original_fleet, sizeof(original_fleet)); fleet->count = original_fleet_count;
    }
    uint64_t still_open = gap_open_count(local);
    if (!status && verify && still_open) { status = 1; failure = "open_gaps_remaining"; }
    if (semantic_events_emit(call, gap_action_result(call, operation,
            verify || (size_t)selected >= local->count ? NULL : &local->rows[selected], report ? fleet : NULL, status, failure,
            with_proof, fixed_now, still_open, effects))) { status = 125; goto done; }
    status = semantic_events_end(call, status);
    goto done;
prepare_failed:
    status = semantic_error(call, admission_error ? admission_error : status == 125 ? "output_or_value_limit" : !verify && !report ? "open_gap_not_found" :
        status == 75 ? "store_limit" : "store_or_argument_conflict", status);
done:
    gap_table_free(local); gap_table_free(fleet); free(tables); return status;
}

static int gap_semantic(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    if (!argc) return gap_semantic_list(call, 0);
    if (!strcmp(argv[0], "list")) {
        if (argc > 2 || (argc == 2 && strcmp(argv[1], "--all"))) return semantic_error(call, "invalid_arguments", 2);
        return gap_semantic_list(call, argc == 2);
    }
    if (!strcmp(argv[0], "scan")) return argc == 1 ? gap_semantic_scan(call) : semantic_error(call, "invalid_arguments", 2);
    if (!strcmp(argv[0], "verify") || !strcmp(argv[0], "close") || !strcmp(argv[0], "report"))
        return gap_semantic_action(call, state, argv[0], argc - 1, argv + 1);
    return gap_semantic_action(call, state, "report", argc, argv);
}
