// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Deterministic delivery faults against the actual alias handler and native codec.
#include <builtins/alias_report.h>
#include <core/alias.h>
#include <core/data_wire.h>
#include <core/data_wire_import.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition); exit(1); \
} } while (0)

/* Only presentation wrappers are stubbed. The checked writer, codec, handler,
 * alias state and response admission under test are the production sources. */
void jash_budget_unwrap(jash_writer_t* writer, void** context) { (void)writer; (void)context; }
long jash_budget_limit(void) { return 0; }
void jash_redir_stream_writer(jash_writer_t writer, void* context) { (void)writer; (void)context; }

typedef struct {
    char bytes[65536];
    size_t used, limit;
    const jash_state_t* state;
    int stop_after_first, retry_interrupts, header_interrupted, action_interrupted;
} sink_t;

static long sink(void* context, const char* bytes, size_t length) {
    sink_t* output = context;
    int first_applied = output->state && jash_alias_get(output->state, "first") != NULL;
    if (output->retry_interrupts && !first_applied && !output->header_interrupted) {
        output->header_interrupted = 1; errno = EINTR; return -1;
    }
    if (output->retry_interrupts && first_applied && !output->action_interrupted) {
        output->action_interrupted = 1; errno = EINTR; return -1;
    }
    if ((output->stop_after_first && first_applied) || output->used >= output->limit) {
        errno = EPIPE; return -1;
    }
    if (length > output->limit - output->used) length = output->limit - output->used;
    if (length > 7) length = 7; /* Both codecs must handle partial delivery. */
    CHECK(output->used + length < sizeof output->bytes);
    memcpy(output->bytes + output->used, bytes, length);
    output->used += length;
    output->bytes[output->used] = 0;
    return (long)length;
}

static void pipe_handler(int signal_number) { (void)signal_number; }
static void check_sigpipe(void) {
    struct sigaction current;
    CHECK(sigaction(SIGPIPE, NULL, &current) == 0);
    CHECK(current.sa_handler == pipe_handler);
}

static void validate_native(const sink_t* output, jdv_stream_state_t state, int status, uint64_t items) {
    unsigned char memory[65536];
    jdv_arena_t arena;
    jdv_arena_init(&arena, memory, sizeof memory);
    jdv_wire_limits_t limits;
    jdv_wire_limits_init(&limits);
    jdv_wire_info_t info;
    CHECK(jdv_wire_validate_v2(output->bytes, output->used, &arena, &limits, &info) == 0);
    CHECK(info.state == state && info.items_seen == items);
    CHECK(info.logical_verified && info.final_hash_present);
    CHECK(info.completeness == (state == JDV_STREAM_COMPLETE ? JDV_COMPLETENESS_COMPLETE : JDV_COMPLETENESS_PARTIAL));
    if (status) CHECK(info.errors_seen > 0);
    unsigned char schemas[65536], item_memory[65536];
    jdv_wire_import_t imported = {0};
    CHECK(jdv_wire_import_v2_open(&imported, output->bytes, output->used, schemas,
        sizeof schemas, item_memory, sizeof item_memory, &limits) == 0);
    jdv_stream_record_t record;
    for (uint64_t i = 0; i < items; i++) CHECK(jdv_stream_pull(&imported.cursor, &record) == JDV_PULL_ITEM);
    CHECK(jdv_stream_pull(&imported.cursor, &record) == (status ? JDV_PULL_ERROR : JDV_PULL_END));
    CHECK(record.terminal != NULL && record.terminal->producer_status == status);
    CHECK(jdv_wire_import_close(&imported) == 0);
}

static void handler_contracts(int mode) {
    jash_state_t* state = calloc(1, sizeof *state);
    CHECK(state != NULL);
    char* args[] = {mode == JASH_DISCOVERY_DATA ? "--data" : "--json", "first=value1", "second=value2"};
    sink_t output = {.state = state, .limit = 0};
    CHECK(jash_b_alias_argv(state, sink, &output, 3, args) == 125);
    CHECK(jash_alias_get(state, "first") == NULL && jash_alias_get(state, "second") == NULL);
    check_sigpipe();

    output = (sink_t){.state = state, .limit = sizeof output.bytes - 1, .stop_after_first = 1};
    CHECK(jash_b_alias_argv(state, sink, &output, 3, args) == 125);
    CHECK(output.used > 0); /* A header was admitted before first applied. */
    CHECK(!strcmp(jash_alias_get(state, "first"), "value1"));
    CHECK(jash_alias_get(state, "second") == NULL); /* Failure stopped the batch. */
    check_sigpipe();

    CHECK(jash_alias_unset(state, "first") == 0);
    output = (sink_t){.state = state, .limit = sizeof output.bytes - 1, .retry_interrupts = 1};
    CHECK(jash_b_alias_argv(state, sink, &output, 3, args) == 0);
    CHECK(output.header_interrupted && output.action_interrupted);
    CHECK(!strcmp(jash_alias_get(state, "first"), "value1"));
    CHECK(!strcmp(jash_alias_get(state, "second"), "value2"));
    if (mode == JASH_DISCOVERY_DATA) validate_native(&output, JDV_STREAM_COMPLETE, 0, 2);
    check_sigpipe();
    free(state);
}

static void collection_failure(int mode, sink_t* output) {
    jash_discovery_rows_t rows;
    CHECK(jash_discovery_rows_init(&rows) == 0);
    *output = (sink_t){.limit = sizeof output->bytes - 1};
    CHECK(jash_discovery_effects_begin(&rows, sink, output, "alias", mode, "jash.alias.v1") == 0);
    CHECK(jash_alias_result_row(&rows, "first", "set", NULL, "value1", 1, NULL) == 0);
    CHECK(jash_discovery_effects_flush(&rows) == 0);
    rows.error = 1; /* Exhaustion after a delivered result, before another row. */
    CHECK(jash_discovery_emit_schema(sink, output, "alias", mode, &rows, 0, "jash.alias.v1") == 75);
    CHECK(rows.effects == NULL);
    jash_discovery_rows_free(&rows);
    check_sigpipe();
    if (mode == JASH_DISCOVERY_DATA) validate_native(output, JDV_STREAM_FAILED, 75, 1);
}

int main(void) {
    struct sigaction action = {0};
    action.sa_handler = pipe_handler;
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(SIGPIPE, &action, NULL) == 0);
    handler_contracts(JASH_DISCOVERY_JSON);
    handler_contracts(JASH_DISCOVERY_DATA);
    sink_t output;
    collection_failure(JASH_DISCOVERY_DATA, &output);
    collection_failure(JASH_DISCOVERY_JSON, &output);
    /* The Python gate validates this partial document against the real schema. */
    CHECK(fwrite(output.bytes, 1, output.used, stdout) == output.used);
    return 0;
}
