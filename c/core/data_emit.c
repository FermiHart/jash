// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "data_emit.h"
#include "data_wire.h"
#include <module/blake3.h>

#include <errno.h>
#include <limits.h>
#include <string.h>

typedef struct {
    const jdv_value_t* items;
    const uint64_t* credits;
    size_t count, index;
    uint64_t bytes;
    const jdv_limits_t* limits;
    struct blake3 hash;
    jdv_stream_end_t terminal;
} sequence_source_t;

static int fail(int error) { errno = error; return -1; }

static jdv_stream_pull_result_t sequence_pull(void* context,
                                              jdv_stream_record_t* record) {
    sequence_source_t* source = context;
    if (source->index < source->count) {
        const jdv_value_t* value = &source->items[source->index];
        uint64_t credit = source->credits ? source->credits[source->index] : 0;
        uint8_t digest[JDV_HASH_SIZE];
        if (credit > UINT64_MAX - source->bytes) {
            errno = EOVERFLOW; return JDV_PULL_ERROR;
        }
        if (jdv_hash_limited(value, digest, source->limits)) return JDV_PULL_ERROR;
        blake3_update(&source->hash, digest, sizeof(digest));
        source->index++;
        source->bytes += credit;
        record->item = value;
        record->logical_bytes = credit;
        return JDV_PULL_ITEM;
    }
    source->terminal.items_seen = source->index;
    source->terminal.bytes_seen = source->bytes;
    source->terminal.final_hash_present = 1;
    source->terminal.final_hash_algorithm = JDV_HASH_BLAKE3;
    blake3_final(&source->hash, source->terminal.final_hash, JDV_HASH_SIZE);
    record->terminal = &source->terminal;
    return source->terminal.state == JDV_STREAM_COMPLETE ? JDV_PULL_END : JDV_PULL_ERROR;
}

static int sequence_cancel(void* context, jdv_slice_t reason) {
    (void)context; (void)reason; return 0;
}
static void sequence_release(void* context) { (void)context; }
static const jdv_stream_producer_vtable_t SEQUENCE = {
    JDV_STREAM_VTABLE_VERSION, sizeof(jdv_stream_producer_vtable_t),
    sequence_pull, sequence_cancel, sequence_release
};

static long discard(void* context, const void* bytes, size_t size) {
    (void)bytes;
    uint64_t* total = context;
    if (size > LONG_MAX || (size_t)(uint64_t)size != size ||
        (uint64_t)size > UINT64_MAX - *total) return fail(EOVERFLOW);
    *total += (uint64_t)size;
    return (long)size;
}

static int emit_pass(unsigned version, const jdv_value_t* items, size_t count,
                      const uint64_t* credits,
                      const jdv_stream_config_t* config,
                      const jdv_stream_end_t* outcome,
                      jdv_sink_t sink, void* context,
                      void* workspace, size_t workspace_bytes,
                      jdv_stream_end_t* final) {
    sequence_source_t source = {
        .items = items, .credits = credits, .count = count,
        .limits = &config->value_limits, .terminal = *outcome
    };
    if (source.terminal.state == JDV_STREAM_COMPLETE && !source.terminal.final_schema)
        source.terminal.final_schema = config->guaranteed_schema;
    blake3_init(&source.hash);
    blake3_update(&source.hash, "JTV1-ITEMS-v1", sizeof("JTV1-ITEMS-v1") - 1);
    jdv_stream_descriptor_t descriptor = {0};
    jdv_stream_cursor_t cursor = {0};
    if (jdv_stream_descriptor_init(&descriptor, config, &SEQUENCE, &source) ||
        jdv_stream_open(&descriptor, &cursor)) return -1;
    jdv_stream_end_t terminal;
    int rc = version == 2 ? jdv_stream_write_frames_v2(&cursor, sink, context,
                                       workspace, workspace_bytes, &terminal)
                          : jdv_stream_write_frames(&cursor, sink, context,
                                       workspace, workspace_bytes, &terminal);
    int saved = errno;
    int released = jdv_stream_release(&cursor);
    if (rc) { errno = saved; return -1; }
    if (released) return -1;
    /* A generated runtime failure terminal is framing, not admission success.
     * The explicit producer outcome is the sole source of semantic state. */
    if (terminal.state != outcome->state ||
        terminal.completeness != outcome->completeness ||
        terminal.errors_seen != outcome->errors_seen ||
        terminal.producer_status != outcome->producer_status ||
        terminal.transform_status != outcome->transform_status ||
        terminal.transport_status != outcome->transport_status ||
        terminal.truncated != outcome->truncated ||
        terminal.reason.data != outcome->reason.data ||
        terminal.reason.len != outcome->reason.len ||
        terminal.final_schema != (outcome->final_schema ? outcome->final_schema :
            outcome->state == JDV_STREAM_COMPLETE ? config->guaranteed_schema : NULL) ||
        !terminal.final_hash_present || terminal.final_hash_algorithm != JDV_HASH_BLAKE3 ||
        terminal.items_seen != count || source.index != count ||
        terminal.bytes_seen != source.bytes)
        return fail(EPROTO);
    *final = terminal;
    return 0;
}

int jdv_emit_sequence_version(unsigned version,
                      const jdv_value_t* items, size_t count,
                      const uint64_t* logical_credits,
                      const jdv_stream_config_t* config,
                      const jdv_stream_end_t* outcome,
                      jdv_sink_t sink, void* context,
                      void* workspace, size_t workspace_bytes,
                      jdv_stream_end_t* final) {
    if ((version != 1 && version != 2) || (!items && count) || !config || !outcome || !sink || !workspace ||
        workspace_bytes < 96 || !final ||
        config->cardinality != JDV_DATA_SEQUENCE ||
        config->boundedness != JDV_STREAM_FINITE) return fail(EINVAL);
    if ((size_t)(uint64_t)count != count || count > config->stream_limits.max_items)
        return fail(E2BIG);
    /* A final schema can be narrower than an ANY guarantee. The generic
     * cursor checks schema relations; a collected sequence can additionally
     * prove every already observed item satisfies that final claim. */
    const jdv_schema_t* final_schema = outcome->final_schema ? outcome->final_schema :
        outcome->state == JDV_STREAM_COMPLETE ? config->guaranteed_schema : NULL;
    if (final_schema) for (size_t i = 0; i < count; i++) {
        jdv_relation_t relation;
        if (jdv_schema_accepts_checked(final_schema, &items[i], &relation,
                                        &config->value_limits)) return -1;
        if (relation != JDV_RELATION_YES) return fail(EDOM);
    }
    /* Admission is deliberately the same codec/cursor execution as delivery,
     * so a malformed late item cannot leak an apparently valid output prefix. */
    uint64_t dry_bytes = 0;
    jdv_stream_end_t result;
    if (emit_pass(version, items, count, logical_credits, config, outcome, discard,
                   &dry_bytes, workspace, workspace_bytes, &result)) return -1;
    if (emit_pass(version, items, count, logical_credits, config, outcome, sink,
                   context, workspace, workspace_bytes, &result)) return -1;
    *final = result;
    return 0;
}

int jdv_emit_sequence(const jdv_value_t* items, size_t count,
                      const uint64_t* logical_credits,
                      const jdv_stream_config_t* config,
                      const jdv_stream_end_t* outcome,
                      jdv_sink_t sink, void* context,
                      void* workspace, size_t workspace_bytes,
                      jdv_stream_end_t* final) {
    return jdv_emit_sequence_version(2, items, count, logical_credits,
        config, outcome, sink, context, workspace, workspace_bytes, final);
}
