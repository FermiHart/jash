// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/checked_string.h>
#include "memory_chain.h"
#include "gap_snapshot.h"
#include "mesh_snapshot.h"
#include <core/query_cancel.h>

typedef struct {
    char filename[128], path[PATH_MAX]; char* bytes; size_t size;
    int present, error, parsed;
    mem_card_t* cards; mem_obs_t* observations; int card_count, observation_count;
} mem_native_store_t;

static void mem_native_store_free(mem_native_store_t* source) {
    free(source->bytes); free(source->cards); free(source->observations);
}

static int mem_native_fields(char* line, char** fields, size_t count) {
    fields[0] = line;
    for (size_t i = 1; i < count; i++) {
        char* delimiter = strchr(fields[i - 1], '\t');
        if (!delimiter) return -1;
        *delimiter = 0; fields[i] = delimiter + 1;
    }
    return strchr(fields[count - 1], '\t') ? -1 : 0;
}

static int mem_native_long(const char* text, long* value) {
    uint64_t number;
    if (memory_unsigned(text, LONG_MAX, &number)) return -1;
    *value = (long)number; return 0;
}

static int mem_native_copy(char* destination, size_t capacity, const char* source) {
    size_t length = strlen(source);
    if (length >= capacity) return -1;
    memcpy(destination, source, length + 1); return 0;
}

static int mem_native_store_parse(mem_native_store_t* source) {
    source->cards = calloc(4096, sizeof(*source->cards));
    source->observations = calloc(1024, sizeof(*source->observations));
    if (!source->cards || !source->observations) { source->error = ENOMEM; return -1; }
    if (source->size && memchr(source->bytes, 0, source->size)) { source->error = EINVAL; return -1; }
    size_t offset = 0;
    while (offset < source->size) {
        char* line = source->bytes + offset;
        char* newline = memchr(line, '\n', source->size - offset);
        if (!newline) goto malformed;
        *newline = 0; offset = (size_t)(newline - source->bytes) + 1;
        if (line[0] == 'C' && line[1] == '\t') {
            if (source->card_count == 4096) { source->error = E2BIG; return -1; }
            char* fields[9]; mem_card_t* card = &source->cards[source->card_count];
            long commands = 0, failed = 0;
            if (mem_native_fields(line + 2, fields, 9) || !*fields[0] || !*fields[1] ||
                mem_native_copy(card->id, sizeof(card->id), fields[0]) ||
                mem_native_copy(card->sess, sizeof(card->sess), fields[1]) ||
                mem_native_long(fields[2], &card->first_ts) || mem_native_long(fields[3], &card->last_ts) ||
                mem_native_long(fields[4], &card->first_seq) || mem_native_long(fields[5], &card->last_seq) ||
                mem_native_long(fields[6], &commands) || mem_native_long(fields[7], &failed) ||
                mem_native_copy(card->files, sizeof(card->files), fields[8]) ||
                card->last_seq < card->first_seq || !commands || commands > INT_MAX || failed > commands) goto malformed;
            card->ncmds = (int)commands; card->fails = (int)failed;
            for (int i = 0; i < source->card_count; i++) if (!strcmp(source->cards[i].id, card->id)) goto malformed;
            source->card_count++;
        } else if (line[0] == 'O' && line[1] == '\t') {
            if (source->observation_count == 1024) { source->error = E2BIG; return -1; }
            char* fields[5]; mem_obs_t* observation = &source->observations[source->observation_count];
            if (mem_native_fields(line + 2, fields, 5) ||
                mem_native_copy(observation->cardid, sizeof(observation->cardid), fields[0]) ||
                mem_native_copy(observation->type, sizeof(observation->type), fields[1]) ||
                mem_native_copy(observation->detail, sizeof(observation->detail), fields[2]) ||
                mem_native_long(fields[3], &observation->seq_a) || mem_native_long(fields[4], &observation->seq_b) ||
                observation->seq_b < observation->seq_a ||
                (strcmp(observation->type, "bugfix") && strcmp(observation->type, "change") && strcmp(observation->type, "discovery"))) goto malformed;
            source->observation_count++;
        } else goto malformed;
    }
    for (int i = 0; i < source->observation_count; i++) {
        const mem_obs_t* observation = &source->observations[i];
        const mem_card_t* card = mem_find_card(source->cards, source->card_count, observation->cardid);
        if (!card || observation->seq_a < card->first_seq || observation->seq_b > card->last_seq) goto malformed;
    }
    source->parsed = 1; return 0;
malformed:
    source->error = EINVAL; return -1;
}

static int mem_native_store_load(mem_native_store_t* source, int parse) {
    memset(source, 0, sizeof(*source));
    const char* key = jash_repo_key();
    int n = key && *key && *key != '-' ? snprintf(source->filename, sizeof(source->filename), "mem-%s.tsv", key) :
                                      snprintf(source->filename, sizeof(source->filename), "mem.tsv");
    if (n < 0 || (size_t)n >= sizeof(source->filename)) { source->error = ENAMETOOLONG; return -1; }
    const char* home = getenv("HOME");
    n = snprintf(source->path, sizeof(source->path), "%s/.jash/%s", home ? home : "", source->filename);
    if (n < 0 || (size_t)n >= sizeof(source->path)) { source->error = ENAMETOOLONG; return -1; }
    memory_store_t store = {-1, -1};
    if (memory_store_open(&store, NULL, 0) || memory_store_read(&store, source->filename, MEMORY_STORE_MAX_BYTES,
            &source->bytes, &source->size, &source->present)) source->error = errno;
    if (memory_store_close(&store) && !source->error) source->error = errno;
    if (source->error) return -1;
    return parse ? mem_native_store_parse(source) : 0;
}

static jdv_value_t mem_native_store_value(semantic_value_t* sem, const mem_native_store_t* store) {
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, store->path)),
        semantic_field("present", jdv_value_bool(store->present)),
        semantic_field("read_complete", jdv_value_bool(!store->error)),
        semantic_field("bytes", jdv_value_filesize(store->size)),
        semantic_field("cards", store->parsed ? jdv_value_u64((uint64_t)store->card_count) : jdv_value_nothing()),
        semantic_field("observations", store->parsed ? jdv_value_u64((uint64_t)store->observation_count) : jdv_value_nothing()),
        semantic_field("observation_time", semantic_symbol(sem, "before_operation")),
        semantic_field("error", semantic_symbol(sem, store->error ? memory_store_error(store->error) : NULL)),
        semantic_field("errno", store->error ? jdv_value_i64(store->error) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

static int mem_native_events(const memory_chain_t* chain, mem_ev_t** events, size_t* count) {
    *count = 0;
    *events = calloc(chain->count ? chain->count : 1, sizeof(**events));
    if (!*events) { errno = ENOMEM; return -1; }
    uint64_t previous = 0;
    for (size_t i = 0; i < chain->count; i++) {
        const memory_chain_entry_t* entry = &chain->entries[i];
        if (!entry->have_sequence || (i && entry->sequence <= previous)) {
            free(*events); *events = NULL; errno = EINVAL; return -1;
        }
        previous = entry->sequence;
        if (mem_internal_command(entry->command)) continue;
        mem_ev_t* event = &(*events)[*count];
        if (entry->sequence > LONG_MAX || entry->seconds > LONG_MAX || entry->status < INT_MIN || entry->status > INT_MAX ||
            mem_native_copy(event->sess, sizeof(event->sess), entry->session ? entry->session : "-") ||
            mem_native_copy(event->cwd, sizeof(event->cwd), entry->cwd ? entry->cwd : "") ||
            mem_native_copy(event->cmd, sizeof(event->cmd), entry->command)) {
            free(*events); *events = NULL; errno = E2BIG; return -1;
        }
        event->seq = (long)entry->sequence; event->ts = (long)entry->seconds; event->rc = (int)entry->status;
        (*count)++;
    }
    return 0;
}

static jdv_value_t mem_native_observation(semantic_value_t* sem, const mem_obs_t* observation) {
    jdv_record_input_t fields[] = {
        semantic_field("card_id", semantic_text(sem, observation->cardid)),
        semantic_field("type", semantic_symbol(sem, observation->type)),
        semantic_field("detail_preview", semantic_text(sem, observation->detail)),
        semantic_field("sequence_a", jdv_value_u64((uint64_t)observation->seq_a)),
        semantic_field("sequence_b", jdv_value_u64((uint64_t)observation->seq_b)),
        semantic_field("inferred", jdv_value_bool(1))
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

static jdv_value_t mem_native_card(semantic_value_t* sem, const mem_card_t* card, const mem_native_store_t* store) {
    if (!card) return jdv_value_nothing();
    jdv_value_t observations[1024]; size_t count = 0;
    for (int i = 0; i < store->observation_count; i++) if (!strcmp(store->observations[i].cardid, card->id))
        observations[count++] = mem_native_observation(sem, &store->observations[i]);
    jdv_record_input_t fields[] = {
        semantic_field("id", semantic_text(sem, card->id)),
        semantic_field("session", semantic_text(sem, card->sess)),
        semantic_field("first_timestamp", jdv_value_u64((uint64_t)card->first_ts)),
        semantic_field("last_timestamp", jdv_value_u64((uint64_t)card->last_ts)),
        semantic_field("first_sequence", jdv_value_u64((uint64_t)card->first_seq)),
        semantic_field("last_sequence", jdv_value_u64((uint64_t)card->last_seq)),
        semantic_field("commands", jdv_value_u64((uint64_t)card->ncmds)),
        semantic_field("failed", jdv_value_u64((uint64_t)card->fails)),
        semantic_field("files_preview", semantic_text(sem, card->files)),
        semantic_field("files_inventory_complete", jdv_value_bool(0)),
        semantic_field("observations", semantic_list(sem, observations, count))
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

typedef struct { int score, kind, index, order; } mem_native_match_t;
static int mem_native_match_compare(const void* left, const void* right) {
    const mem_native_match_t *a = left, *b = right;
    return a->score != b->score ? (a->score > b->score ? -1 : 1) : a->order < b->order ? -1 : a->order > b->order;
}

static int mem_native_search(const mem_native_store_t* store, const char* query, int failed_only,
                              const char* type, mem_native_match_t* matches) {
    char query_lower[300]; mem_lower(query, query_lower, sizeof(query_lower)); int count = 0;
    for (int i = 0; i < store->card_count + store->observation_count; i++) {
        int card = i < store->card_count; int index = card ? i : i - store->card_count;
        char hay[700], lower[700], query_copy[300];
        if (card) {
            const mem_card_t* item = &store->cards[index];
            if (type || (failed_only && !item->fails)) continue;
            snprintf(hay, sizeof(hay), "%s %s", item->sess, item->files);
        } else {
            const mem_obs_t* item = &store->observations[index];
            if ((type && strcmp(type, item->type)) || (failed_only && strcmp(item->type, "bugfix"))) continue;
            snprintf(hay, sizeof(hay), "%s %s", item->type, item->detail);
        }
        mem_lower(hay, lower, sizeof(lower)); if (jash_copy_cstr(query_copy, sizeof(query_copy), query_lower)) { return -1; }
        int score = mem_score(lower, query_copy);
        if (score) matches[count++] = (mem_native_match_t){score, !card, index, i};
    }
    qsort(matches, (size_t)count, sizeof(*matches), mem_native_match_compare); return count;
}

static int mem_native_serialize(const mem_distillation_t* result, char** bytes, size_t* size) {
    *bytes = malloc(MEMORY_STORE_MAX_BYTES + 1u); *size = 0;
    if (!*bytes) { errno = ENOMEM; return -1; }
    for (int i = 0; i < result->card_count + result->observation_count; i++) {
        int n;
        if (i < result->card_count) {
            const mem_card_t* card = &result->cards[i];
            if (strpbrk(card->sess, "\t\r\n") || strpbrk(card->files, "\t\r\n")) { errno = EINVAL; return -1; }
            n = snprintf(*bytes + *size, MEMORY_STORE_MAX_BYTES + 1u - *size,
                "C\t%s\t%s\t%ld\t%ld\t%ld\t%ld\t%d\t%d\t%s\n", card->id, card->sess,
                card->first_ts, card->last_ts, card->first_seq, card->last_seq, card->ncmds, card->fails, card->files);
        } else {
            const mem_obs_t* observation = &result->observations[i - result->card_count];
            n = snprintf(*bytes + *size, MEMORY_STORE_MAX_BYTES + 1u - *size,
                "O\t%s\t%s\t%s\t%ld\t%ld\n", observation->cardid, observation->type, observation->detail,
                observation->seq_a, observation->seq_b);
        }
        if (n < 0 || (size_t)n > MEMORY_STORE_MAX_BYTES - *size) { errno = E2BIG; return -1; }
        *size += (size_t)n;
    }
    return 0;
}

static jdv_value_t mem_native_graph_observe(semantic_value_t* sem, int* error) {
    const char* key = jash_repo_key(); char filename[128], path[PATH_MAX];
    int n = key && *key && *key != '-' ? snprintf(filename, sizeof(filename), "graph-%s.tsv", key) :
                                      snprintf(filename, sizeof(filename), "graph.tsv");
    int number = n < 0 || (size_t)n >= sizeof(filename) ? ENAMETOOLONG : 0;
    const char* home = getenv("HOME");
    n = snprintf(path, sizeof(path), "%s/.jash/%s", home ? home : "", filename);
    if (n < 0 || (size_t)n >= sizeof(path)) number = ENAMETOOLONG;
    memory_store_t store = {-1, -1}; struct stat info; int present = 0;
    if (!number && memory_store_open(&store, NULL, 0)) number = errno;
    if (!number && store.directory >= 0) {
        if (fstatat(store.directory, filename, &info, AT_SYMLINK_NOFOLLOW)) {
            if (errno != ENOENT) number = errno;
        } else {
            present = 1;
            if (!S_ISREG(info.st_mode) || info.st_uid != geteuid() || (info.st_mode & 0022) || info.st_size < 0) number = EPERM;
        }
    }
    if (memory_store_close(&store) && !number) number = errno;
    if (number && !*error) *error = number;
    jdv_record_input_t fields[] = {
        semantic_field("path", semantic_path(sem, path)),
        semantic_field("present", number ? jdv_value_nothing() : jdv_value_bool(present)),
        semantic_field("bytes", !number && present ? jdv_value_filesize((uint64_t)info.st_size) : jdv_value_nothing()),
        semantic_field("metadata_complete", jdv_value_bool(!number)),
        semantic_field("content_verified", jdv_value_bool(0)),
        semantic_field("error", semantic_symbol(sem, number ? memory_store_error(number) : NULL)),
        semantic_field("errno", number ? jdv_value_i64(number) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

static jdv_value_t mem_native_brief(semantic_builtin_t* call, jash_state_t* state,
                                    const mem_native_store_t* store, const memory_chain_t* chain, int* error) {
    semantic_value_t* sem = &call->values;
    const mem_card_t* last = store->card_count ? &store->cards[store->card_count - 1] : NULL;
    typedef struct { char signature[300]; uint64_t count; } signature_t;
    signature_t* signatures = calloc(4096, sizeof(*signatures)); size_t distinct = 0;
    if (!signatures && !*error) *error = ENOMEM;
    uint64_t top_count = 0; const char* top = NULL;
    if (signatures && last && chain && !chain->error) for (size_t i = 0; i < chain->count; i++) {
        const memory_chain_entry_t* entry = &chain->entries[i];
        if (mem_internal_command(entry->command) || !entry->status || entry->sequence < (uint64_t)last->first_seq || entry->sequence > (uint64_t)last->last_seq ||
            strcmp(entry->session ? entry->session : "-", last->sess)) continue;
        char signature[300]; mem_sig(entry->command, signature, sizeof(signature));
        size_t index = 0; while (index < distinct && strcmp(signature, signatures[index].signature)) index++;
        if (index == distinct) {
            if (distinct == 4096) { if (!*error) *error = E2BIG; break; }
            if (jash_copy_cstr(signatures[index].signature, sizeof(signatures[index].signature), signature)) { if (!*error) *error = errno; break; } distinct++;
        }
        signatures[index].count++;
        if (signatures[index].count > top_count) { top_count = signatures[index].count; top = signatures[index].signature; }
    }
    jdv_record_input_t failure_fields[] = {
        semantic_field("signature_preview", semantic_text(sem, top)),
        semantic_field("count", *error ? jdv_value_nothing() : jdv_value_u64(top_count)),
        semantic_field("matching", semantic_symbol(sem, "stored_sequence_range_and_session")),
        semantic_field("classification", semantic_symbol(sem, "bounded_verb_and_first_nonoption_argument_heuristic"))
    };
    jdv_value_t failure = semantic_record(sem, failure_fields, 4);
    free(signatures);
    jdv_value_t gaps = jdv_value_nothing();
    if (jash_gap_observe(sem, &gaps) && !*error) *error = EIO;
    jash_mesh_snapshot_t mesh = {0}; int mesh_status = jash_mesh_snapshot_counts(state, &mesh);
    if (mesh_status && !*error) *error = mesh.error_number ? mesh.error_number : EIO;
    jdv_record_input_t mesh_fields[] = {
        semantic_field("present", jdv_value_bool(mesh.store_present)),
        semantic_field("read_complete", jdv_value_bool(mesh.complete)),
        semantic_field("observed_at", jdv_value_i64(mesh.observed_at)),
        semantic_field("open", mesh.complete ? jdv_value_u64(mesh.tasks_ready + mesh.tasks_claimed) : jdv_value_nothing()),
        semantic_field("ready", mesh.complete ? jdv_value_u64(mesh.tasks_ready) : jdv_value_nothing()),
        semantic_field("claimed", mesh.complete ? jdv_value_u64(mesh.tasks_claimed) : jdv_value_nothing()),
        semantic_field("done", mesh.complete ? jdv_value_u64(mesh.tasks_done) : jdv_value_nothing()),
        semantic_field("failed", mesh.complete ? jdv_value_u64(mesh.tasks_failed) : jdv_value_nothing()),
        semantic_field("error", semantic_symbol(sem, mesh.complete ? NULL : mesh.reason)),
        semantic_field("errno", mesh.error_number ? jdv_value_i64(mesh.error_number) : jdv_value_nothing())
    };
    jdv_record_input_t fields[] = {
        semantic_field("last_card", mem_native_card(sem, last, store)),
        semantic_field("top_failure", failure),
        semantic_field("gaps", gaps),
        semantic_field("mesh_tasks", semantic_record(sem, mesh_fields, sizeof(mesh_fields) / sizeof(mesh_fields[0]))),
        semantic_field("graph", mem_native_graph_observe(sem, error))
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

typedef struct {
    const char* operation; const mem_native_store_t* store; const memory_chain_t* chain;
    jdv_value_t* items; size_t count, matched; uint64_t offset;
    jdv_value_t distillation, verification, brief;
    int applied, durable, changed, attestation_requested, error_number, status;
    const char* error;
} mem_native_report_t;

static jdv_value_t mem_native_report(semantic_value_t* sem, const mem_native_report_t* report) {
    int more = !report->error && report->offset + report->count < report->matched;
    jdv_record_input_t fields[] = {
        semantic_field("schema", semantic_symbol(sem, "jash.mem.v1")),
        semantic_field("operation", semantic_symbol(sem, report->operation)),
        semantic_field("store", mem_native_store_value(sem, report->store)),
        semantic_field("chain_source", report->chain ? memory_chain_source(sem, report->chain) : jdv_value_nothing()),
        semantic_field("results", semantic_list(sem, report->items, report->count)),
        semantic_field("matched", report->error ? jdv_value_nothing() : jdv_value_u64(report->matched)),
        semantic_field("offset", jdv_value_u64(report->offset)),
        semantic_field("next_offset", more ? jdv_value_u64(report->offset + report->count) : jdv_value_nothing()),
        semantic_field("truncated", jdv_value_bool(more)),
        semantic_field("distillation", report->distillation),
        semantic_field("verification", report->verification),
        semantic_field("brief", report->brief),
        semantic_field("applied", jdv_value_bool(report->applied)),
        semantic_field("durable", report->durable < 0 ? jdv_value_nothing() : jdv_value_bool(report->durable)),
        semantic_field("changed", report->changed < 0 ? jdv_value_nothing() : jdv_value_bool(report->changed)),
        semantic_field("attestation_requested", jdv_value_bool(report->attestation_requested)),
        semantic_field("inference_scope", semantic_symbol(sem, "bounded_deterministic_heuristics_not_causal_proof")),
        semantic_field("error", semantic_symbol(sem, report->error)),
        semantic_field("errno", report->error_number ? jdv_value_i64(report->error_number) : jdv_value_nothing())
    };
    return semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
}

static int mem_native_finish(semantic_builtin_t* call, jdv_value_t value, int status, uint64_t budget) {
    if (call->mode != 1 || !budget || call->admitted || call->values.error) return semantic_finish(call, value, status);
    jash_structured_buffer_t buffer; jash_structured_buffer_init(&buffer, SEMANTIC_OUTPUT_BYTES);
    jash_json_writer_t encoded; jash_jsonw_init(&encoded, jash_structured_buffer_sink, &buffer);
    int rc = jdv_json_write(&encoded, &value, &call->values.limits); jash_jsonw_raw(&encoded, "\n");
    if (rc || jash_jsonw_finish(&encoded)) { jash_structured_buffer_free(&buffer); return 125; }
    long global = jash_budget_limit();
    if (global > 0 && (uint64_t)global < budget) budget = (uint64_t)global;
    rc = jash_structured_document(call->writer, call->context, "mem", buffer.data, buffer.used, buffer.required, (long)budget, status);
    jash_structured_buffer_free(&buffer); return rc;
}

typedef struct { jash_state_t* state; int status; } mem_native_control_t;
static int mem_native_poll(void* context) {
    mem_native_control_t* control = context;
    control->status = jash_query_cancel_poll(control->state); return control->status;
}

static int mem_native_provider(semantic_builtin_t* call, jash_state_t* state, int argc, char** argv) {
    const char* operation = argc ? argv[0] : "brief";
    int distill = !strcmp(operation, "distill"), timeline = !strcmp(operation, "timeline"), show = !strcmp(operation, "show"),
        search = !strcmp(operation, "search"), brief = !strcmp(operation, "brief"), verify = !strcmp(operation, "verify");
    if (!distill && !timeline && !show && !search && !brief && !verify) return semantic_error(call, "invalid_arguments", 2);
    const char *operand = NULL, *type = NULL; int failed = 0, literal = 0;
    uint64_t offset = 0, limit = 64, budget = 0; unsigned seen = 0;
    for (int i = argc ? 1 : 0; i < argc; i++) {
        const char* value = NULL; unsigned bit = 0;
        if (!literal && !strcmp(argv[i], "--")) { literal = 1; continue; }
        if (!literal && !strcmp(argv[i], "--failed")) { failed = 1; continue; }
        if (!literal && !strcmp(argv[i], "--offset")) bit = 1;
        else if (!literal && !strcmp(argv[i], "--limit")) bit = 2;
        else if (!literal && !strcmp(argv[i], "--type")) bit = 4;
        else if (!literal && !strcmp(argv[i], "--budget")) bit = 8;
        if (bit) {
            if (seen & bit || ++i == argc) return semantic_error(call, "invalid_arguments", 2);
            seen |= bit; value = argv[i];
            if ((bit == 1 && memory_unsigned(value, 5120, &offset)) ||
                (bit == 2 && (memory_unsigned(value, 128, &limit) || !limit)) ||
                (bit == 8 && memory_unsigned(value, SEMANTIC_OUTPUT_BYTES, &budget))) return semantic_error(call, "invalid_arguments", 2);
            if (bit == 4) {
                if (strcmp(value, "bugfix") && strcmp(value, "discovery") && strcmp(value, "change")) return semantic_error(call, "invalid_arguments", 2);
                type = value;
            }
        } else {
            if (operand || (!literal && argv[i][0] == '-')) return semantic_error(call, "invalid_arguments", 2);
            operand = argv[i];
        }
    }
    if (((show || search || verify) && (!operand || !*operand)) ||
        ((!show && !search && !verify) && operand) || (!search && (failed || type)) ||
        (!search && !timeline && (seen & 3u)) || (search && strlen(operand) >= 300) ||
        ((show || verify) && strlen(operand) >= sizeof(((mem_card_t*)0)->id))) return semantic_error(call, "invalid_arguments", 2);
    int cancelled = jash_query_cancel_poll(state);
    if (cancelled) return semantic_cancelled(call, cancelled);
    mem_native_store_t store; (void)mem_native_store_load(&store, !distill);
    memory_chain_t chain = {0}; int need_chain = distill || verify || (brief && store.card_count);
    if (need_chain) (void)memory_chain_load(&chain, 0, 0);
    semantic_value_t* sem = &call->values; jdv_value_t items[128];
    mem_native_report_t report = {.operation = operation, .store = &store, .chain = need_chain ? &chain : NULL,
        .items = items, .offset = offset, .durable = -1, .changed = -1};
    int error = store.error ? store.error : chain.error; const char* failure = NULL;
    mem_ev_t* events = NULL; size_t event_count = 0;
    mem_distillation_t generated = {0}; char* serialized = NULL; size_t serialized_size = 0;
    if (!error && (distill || verify) && !chain.present) { error = ENOENT; failure = "chain_not_recorded"; }
    if (!error && (distill || verify) && mem_native_events(&chain, &events, &event_count)) {
        error = errno; failure = error == EINVAL ? "invalid_chain_sequence_order" : "engine_input_limit";
    }
    cancelled = jash_query_cancel_poll(state);
    if (cancelled) { report.status = semantic_cancelled(call, cancelled); goto done; }
    if (!error && distill) {
        if (mem_distill_collect(events, (int)event_count, &generated) || mem_native_serialize(&generated, &serialized, &serialized_size)) {
            error = errno; failure = "inference_collection_failed";
        } else {
            jdv_record_input_t fields[] = {
                semantic_field("cards", jdv_value_u64((uint64_t)generated.card_count)),
                semantic_field("observations", jdv_value_u64((uint64_t)generated.observation_count)),
                semantic_field("bugfix", jdv_value_u64((uint64_t)generated.bugfix)),
                semantic_field("discovery", jdv_value_u64((uint64_t)generated.discovery)),
                semantic_field("change", jdv_value_u64((uint64_t)generated.change)),
                semantic_field("chain_records", jdv_value_u64(event_count)),
                semantic_field("skipped_self_records", jdv_value_u64(chain.count - event_count)),
                semantic_field("excluded_commands", semantic_symbol(sem, "mem_verb_subcommands_and_mem_hyphen_prefix")),
                semantic_field("serialized_bytes", jdv_value_filesize(serialized_size)),
                semantic_field("command_signature_capacity", jdv_value_u64(299)),
                semantic_field("files_preview_capacity", jdv_value_u64(239)),
                semantic_field("detail_preview_capacity", jdv_value_u64(279))
            };
            report.distillation = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
            mem_native_report_t candidate = report;
            candidate.error = "publication_durability_failed"; candidate.error_number = INT_MAX;
            candidate.applied = 0; candidate.durable = 0; candidate.changed = 0;
            jdv_value_t receipt = mem_native_report(sem, &candidate);
            int preflight = semantic_preflight(call, &receipt);
            if (!preflight && call->mode == 1 && budget) {
                value_output_measure_t measured = {0, SEMANTIC_OUTPUT_BYTES}; jash_json_writer_t encoded;
                jash_jsonw_init(&encoded, semantic_measure_output, &measured);
                if (jdv_json_write(&encoded, &receipt, &sem->limits) || jash_jsonw_finish(&encoded) || measured.bytes + 1u > budget) preflight = 75;
            }
            if (preflight) { report.status = semantic_error(call, "receipt_admission_failed", preflight); goto done; }
            int admitted = semantic_admit(call);
            if (admitted) { report.status = admitted; goto done; }
            mem_native_control_t control = {.state = state}; memory_store_t locked = {-1, -1};
            if (memory_store_open_cooperative(&locked, NULL, 1, mem_native_poll, &control)) error = errno;
            char* current = NULL; size_t current_size = 0; int current_present = 0;
            if (!error && memory_store_read(&locked, store.filename, MEMORY_STORE_MAX_BYTES, &current, &current_size, &current_present)) error = errno;
            if (!error && mem_native_poll(&control)) error = ECANCELED;
            if (!error) {
                report.changed = !current_present || current_size != serialized_size || (serialized_size && memcmp(current, serialized, serialized_size));
                if (report.changed) {
                    memory_write_t result = memory_store_publish(&locked, store.filename, serialized, serialized_size);
                    report.applied = result.applied; report.durable = result.durable; error = result.error;
                }
            }
            free(current);
            if (memory_store_close(&locked) && !error) error = errno;
            if (!error && report.applied && report.durable) {
                char details[160];
                snprintf(details, sizeof(details), "sessions=%d obs=%d bugfix=%d discovery=%d change=%d",
                    generated.card_count, generated.observation_count, generated.bugfix, generated.discovery, generated.change);
                jash_sigil_attest("mem-distill", details); report.attestation_requested = 1;
            }
            if (control.status) { call->cancellation_status = control.status; report.status = control.status; failure = "cancelled"; }
        }
    } else if (!error && (timeline || show || search)) {
        mem_native_match_t matches[5120]; int matched = 0;
        if (search) matched = mem_native_search(&store, operand, failed, type, matches);
        else for (int i = 0; i < store.card_count; i++) if (!show || !strcmp(store.cards[i].id, operand))
            matches[matched++] = (mem_native_match_t){0, 0, i, i};
        if (matched < 0) { error = errno; failure = "query_copy_limit"; matched = 0; }
        report.matched = (size_t)matched;
        if (show && !matched) { error = ENOENT; failure = "card_not_found"; }
        for (size_t i = (size_t)offset; i < (size_t)matched && report.count < limit; i++) {
            const mem_native_match_t* match = &matches[i];
            jdv_record_input_t fields[] = {
                semantic_field("kind", semantic_symbol(sem, match->kind ? "observation" : "card")),
                semantic_field("score", search ? jdv_value_u64((uint64_t)match->score) : jdv_value_nothing()),
                semantic_field("card", match->kind ? jdv_value_nothing() : mem_native_card(sem, &store.cards[match->index], &store)),
                semantic_field("observation", match->kind ? mem_native_observation(sem, &store.observations[match->index]) : jdv_value_nothing())
            };
            items[report.count++] = semantic_record(sem, fields, 4);
        }
    } else if (!error && verify) {
        const mem_card_t* card = mem_find_card(store.cards, store.card_count, operand);
        if (!card) { error = ENOENT; failure = "card_not_found"; }
        else {
            mem_verification_t result = mem_verify_claims(card, store.observations, store.observation_count, events, (int)event_count);
            uint64_t observed = 0, failures = 0;
            for (size_t i = 0; i < event_count && !result.drift_sequence; i++) {
                const mem_ev_t* event = &events[i];
                if (event->seq < card->first_seq || event->seq > card->last_seq) continue;
                observed++; failures += event->rc != 0; result.claims++;
                if (strcmp(event->sess, card->sess)) {
                    result.drift_sequence = event->seq; if (jash_copy_cstr(result.reason, sizeof(result.reason), "intermediate sequence session mismatch")) { error = errno; failure = "verification_reason_limit"; }
                }
            }
            if (!result.drift_sequence) {
                result.claims += 3;
                const mem_ev_t* first = mem_find_seq(events, (int)event_count, card->first_seq);
                const mem_ev_t* last = mem_find_seq(events, (int)event_count, card->last_seq);
                if (observed != (uint64_t)card->ncmds || failures != (uint64_t)card->fails || !first || !last ||
                    first->ts != card->first_ts || last->ts != card->last_ts) {
                    result.drift_sequence = card->first_seq; if (jash_copy_cstr(result.reason, sizeof(result.reason), "recorded card counts or timestamps differ")) { error = errno; failure = "verification_reason_limit"; }
                }
            }
            jdv_record_input_t fields[] = {
                semantic_field("card_id", semantic_text(sem, card->id)),
                semantic_field("consistent", jdv_value_bool(!result.drift_sequence)),
                semantic_field("claims_replayed", jdv_value_u64((uint64_t)result.claims)),
                semantic_field("drift_sequence", result.drift_sequence ? jdv_value_u64((uint64_t)result.drift_sequence) : jdv_value_nothing()),
                semantic_field("reason", semantic_text(sem, result.drift_sequence ? result.reason : NULL)),
                semantic_field("cryptographically_verified", jdv_value_bool(0)),
                semantic_field("heuristic_truth_verified", jdv_value_bool(0)),
                semantic_field("scope", semantic_symbol(sem, "recorded_references_sessions_outcomes_counts_and_timestamps"))
            };
            report.verification = semantic_record(sem, fields, sizeof(fields) / sizeof(fields[0]));
            if (result.drift_sequence) { error = EINVAL; failure = "record_drift"; }
        }
    } else if (!error && brief) report.brief = mem_native_brief(call, state, &store, need_chain ? &chain : NULL, &error);
    if (!distill && (cancelled = jash_query_cancel_poll(state))) {
        if (semantic_admit(call)) { report.status = 125; goto done; }
        call->cancellation_status = cancelled; report.status = cancelled;
        failure = "cancelled"; error = ECANCELED;
    }
    report.error = failure ? failure : error ? memory_store_error(error) : NULL; report.error_number = error;
    if (!report.status) report.status = error ? 1 : 0;
    report.status = mem_native_finish(call, mem_native_report(sem, &report), report.status, budget);
done:
    free(events); free(generated.cards); free(generated.observations); free(serialized);
    memory_chain_free(&chain); mem_native_store_free(&store); return report.status;
}
