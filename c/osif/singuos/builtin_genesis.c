// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
//
// Module: builtins/genesis — inspect and exercise the fixed Genesis v1 PoC.

#include "builtin.h"
#include <singuos/genesis_v1.h>
#include <singuos/nerva_stream_v1.h>

#ifndef JASH_TARGET_BEYOND

int jash_b_genesis(jash_state_t* s, jash_writer_t w, void* ctx,
                   const char* args) {
    (void)s;
    (void)args;
    jash_writes(w, ctx,
                "genesis: unavailable: Genesis ioctls require a Beyond surface\n");
    return 1;
}

#else /* JASH_TARGET_BEYOND */

#include <stdint.h>

extern int bear_ioctl(int fd, unsigned long request, ...);
extern long bear_raw_syscall6(long nr, long a0, long a1, long a2,
                              long a3, long a4, long a5);

#define BEY_NERVA_POLL 0x13

static int genesis_args_equal(const char* args, const char* expected) {
    if (!args) args = "";
    while (*args == ' ' || *args == '\t') args++;
    while (*expected && *args == *expected) {
        args++;
        expected++;
    }
    if (*expected) return 0;
    while (*args == ' ' || *args == '\t' || *args == '\r' || *args == '\n')
        args++;
    return *args == '\0';
}

static const char* genesis_node_kind_name(uint32_t kind) {
    switch (kind) {
    case GENESIS_V1_NODE_ORGANISM: return "ORGANISM";
    case GENESIS_V1_NODE_SURFACE: return "SURFACE";
    case GENESIS_V1_NODE_PROBE: return "PROBE";
    default: return "UNSPECIFIED";
    }
}

static const char* genesis_node_state_name(uint32_t state) {
    switch (state) {
    case GENESIS_V1_NODE_STATE_ACTIVE: return "ACTIVE";
    case GENESIS_V1_NODE_STATE_DORMANT: return "DORMANT";
    case GENESIS_V1_NODE_STATE_FROZEN: return "FROZEN";
    case GENESIS_V1_NODE_STATE_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

static const char* genesis_result_status_name(uint32_t status) {
    switch (status) {
    case GENESIS_V1_RESULT_EMPTY: return "EMPTY";
    case GENESIS_V1_RESULT_COMMITTED: return "COMMITTED";
    case GENESIS_V1_RESULT_AUTHORITY_DENIED: return "AUTHORITY_DENIED";
    case GENESIS_V1_RESULT_ACTION_DENIED: return "ACTION_DENIED";
    case GENESIS_V1_RESULT_REVISION_CONFLICT: return "REVISION_CONFLICT";
    case GENESIS_V1_RESULT_INVALID: return "INVALID";
    case GENESIS_V1_RESULT_FAILED: return "FAILED";
    default: return "UNKNOWN";
    }
}

static const char* genesis_error_name(uint32_t error) {
    switch (error) {
    case GENESIS_V1_ERROR_NONE: return "NONE";
    case GENESIS_V1_ERROR_BAD_SCHEMA: return "BAD_SCHEMA";
    case GENESIS_V1_ERROR_BAD_RESERVED: return "BAD_RESERVED";
    case GENESIS_V1_ERROR_BAD_OPCODE: return "BAD_OPCODE";
    case GENESIS_V1_ERROR_BAD_TARGET: return "BAD_TARGET";
    case GENESIS_V1_ERROR_BAD_CAPABILITY: return "BAD_CAPABILITY";
    case GENESIS_V1_ERROR_BAD_PRINCIPAL: return "BAD_PRINCIPAL";
    case GENESIS_V1_ERROR_REVISION_CONFLICT: return "REVISION_CONFLICT";
    case GENESIS_V1_ERROR_REPLAY: return "REPLAY";
    case GENESIS_V1_ERROR_BAD_STATE: return "BAD_STATE";
    case GENESIS_V1_ERROR_BUDGET_EXHAUSTED: return "BUDGET_EXHAUSTED";
    case GENESIS_V1_ERROR_DEADLINE_EXPIRED: return "DEADLINE_EXPIRED";
    case GENESIS_V1_ERROR_LEDGER_FULL: return "LEDGER_FULL";
    case GENESIS_V1_ERROR_ACTION_ROLLED_BACK: return "ACTION_ROLLED_BACK";
    case GENESIS_V1_ERROR_BAD_AUTHORITY_EPOCH: return "BAD_AUTHORITY_EPOCH";
    case GENESIS_V1_ERROR_CAPABILITY_QUOTA_EXHAUSTED:
        return "CAPABILITY_QUOTA_EXHAUSTED";
    default: return "UNKNOWN";
    }
}

static void genesis_id_hex(const struct genesis_v1_id* id,
                           char out[GENESIS_V1_ID_SIZE * 2u + 1u]) {
    static const char hex[] = "0123456789abcdef";
    uint32_t i;

    for (i = 0u; i < GENESIS_V1_ID_SIZE; ++i) {
        out[i * 2u] = hex[id->bytes[i] >> 4];
        out[i * 2u + 1u] = hex[id->bytes[i] & 0x0fu];
    }
    out[GENESIS_V1_ID_SIZE * 2u] = '\0';
}

static void genesis_label(const struct genesis_v1_node* node,
                          char out[GENESIS_V1_LABEL_SIZE + 1u]) {
    uint32_t i;

    for (i = 0u; i < GENESIS_V1_LABEL_SIZE && node->label[i] != 0u; ++i) {
        uint8_t c = node->label[i];
        out[i] = c >= 0x20u && c < 0x7fu ? (char)c : '?';
    }
    out[i] = '\0';
}

static int genesis_get_page(jash_writer_t w, void* ctx,
                            struct genesis_v1_page* page) {
    genesis_v1_zero(page, GENESIS_V1_ABI_SIZE);
    if (bear_ioctl(1, (unsigned long)SINGUOS_GENESIS_GET_V1, page) != 0) {
        jash_writes(w, ctx, "genesis: GET ioctl failed\n");
        return 1;
    }
    if (!genesis_v1_poc_page_valid(page)) {
        jash_writes(w, ctx,
                    "genesis: GET returned an invalid Genesis v1 PoC page\n");
        return 1;
    }
    return 0;
}

static int genesis_print_state(jash_writer_t w, void* ctx) {
    struct genesis_v1_page page;
    uint32_t node_count;
    uint32_t i;

    if (genesis_get_page(w, ctx, &page) != 0) return 1;
    node_count = genesis_v1_u32_from_le(page.header.node_count_le);
    jash_writef(w, ctx,
                "genesis state generation=%llu body_revision=%llu "
                "graph_revision=%llu logical_clock=%llu events=%u dropped=%llu\n",
                (unsigned long long)genesis_v1_u64_from_le(page.header.generation_le),
                (unsigned long long)genesis_v1_u64_from_le(page.header.body_revision_le),
                (unsigned long long)genesis_v1_u64_from_le(page.header.graph_revision_le),
                (unsigned long long)genesis_v1_u64_from_le(page.header.logical_clock_le),
                genesis_v1_u32_from_le(page.header.event_count_le),
                (unsigned long long)genesis_v1_u64_from_le(page.header.dropped_events_le));

    for (i = 0u; i < node_count; ++i) {
        const struct genesis_v1_node* node = &page.nodes[i];
        char id[GENESIS_V1_ID_SIZE * 2u + 1u];
        char label[GENESIS_V1_LABEL_SIZE + 1u];
        uint32_t kind = genesis_v1_u32_from_le(node->kind_le);
        uint32_t state = genesis_v1_u32_from_le(node->state_le);

        genesis_id_hex(&node->id, id);
        genesis_label(node, label);
        jash_writef(w, ctx,
                    "node index=%u id=%s kind=%s state=%s revision=%llu label=%s\n",
                    i, id, genesis_node_kind_name(kind),
                    genesis_node_state_name(state),
                    (unsigned long long)genesis_v1_u64_from_le(node->object_revision_le),
                    label);
    }
    return 0;
}

static uint32_t genesis_id_hash_byte(uint32_t hash, uint8_t value) {
    return (hash ^ value) * 16777619u;
}

static uint32_t genesis_id_hash_u64(uint32_t hash, uint64_t value) {
    uint32_t i;

    for (i = 0u; i < 8u; ++i)
        hash = genesis_id_hash_byte(hash, (uint8_t)(value >> (i * 8u)));
    return hash;
}

static void genesis_make_id(struct genesis_v1_id* id, uint8_t domain,
                            uint64_t generation, uint64_t body_revision,
                            uint64_t graph_revision, uint64_t target_revision,
                            uint64_t logical_clock) {
    uint32_t hash = genesis_id_hash_byte(2166136261u, domain);
    uint32_t i;

    hash = genesis_id_hash_u64(hash, generation);
    hash = genesis_id_hash_u64(hash, body_revision);
    hash = genesis_id_hash_u64(hash, graph_revision);
    hash = genesis_id_hash_u64(hash, target_revision);
    hash = genesis_id_hash_u64(hash, logical_clock);

    genesis_v1_id_zero(id);
    id->bytes[0] = (uint8_t)'J';
    id->bytes[1] = (uint8_t)'G';
    id->bytes[2] = domain;
    id->bytes[3] = (uint8_t)'1';
    for (i = 0u; i < 8u; ++i)
        id->bytes[4u + i] = (uint8_t)(logical_clock >> (i * 8u));
    for (i = 0u; i < 4u; ++i)
        id->bytes[12u + i] = (uint8_t)(hash >> (i * 8u));
}

static int genesis_find_freeze_path(
    const struct genesis_v1_page* page,
    const struct genesis_v1_node** probe_out,
    const struct genesis_v1_edge** edge_out) {
    const struct genesis_v1_node* probe = 0;
    const struct genesis_v1_edge* edge = 0;
    const struct genesis_v1_node* actor = 0;
    uint32_t node_count = genesis_v1_u32_from_le(page->header.node_count_le);
    uint32_t edge_count = genesis_v1_u32_from_le(page->header.edge_count_le);
    uint32_t probes = 0u;
    uint32_t may_act_edges = 0u;
    uint32_t actors = 0u;
    uint32_t i;

    for (i = 0u; i < node_count; ++i) {
        if (genesis_v1_u32_from_le(page->nodes[i].kind_le) ==
            GENESIS_V1_NODE_PROBE) {
            probe = &page->nodes[i];
            probes++;
        }
    }
    if (probes != 1u || genesis_v1_id_is_zero(&probe->id)) return 1;

    for (i = 0u; i < edge_count; ++i) {
        if (genesis_v1_u32_from_le(page->edges[i].kind_le) ==
            GENESIS_V1_EDGE_MAY_ACT_ON) {
            edge = &page->edges[i];
            may_act_edges++;
        }
    }
    if (may_act_edges != 1u ||
        genesis_v1_u32_from_le(edge->flags_le) != GENESIS_V1_EDGE_F_ACTIVE ||
        !genesis_v1_id_equal(&edge->to_node_id, &probe->id))
        return 1;

    for (i = 0u; i < node_count; ++i) {
        if (genesis_v1_id_equal(&page->nodes[i].id, &edge->from_node_id)) {
            actor = &page->nodes[i];
            actors++;
        }
    }
    if (actors != 1u ||
        !genesis_v1_id_equal(&actor->capability_id, &edge->id))
        return 1;

    *probe_out = probe;
    *edge_out = edge;
    return 0;
}

static int genesis_result_valid(
    const struct genesis_v1_ioctl_envelope* envelope) {
    return genesis_v1_ioctl_result_valid(envelope);
}

static int genesis_render_nerva(
    jash_writer_t w, void* ctx,
    const struct genesis_v1_ioctl_envelope* envelope) {
    struct nerva_v1_record record;
    uint16_t expected_type;
    uint16_t expected_stream;
    uint32_t expected_trust;
    uint32_t status = genesis_v1_u32_from_le(envelope->result.status_le);
    uint32_t i;

    if (status == GENESIS_V1_RESULT_COMMITTED) {
        expected_type = NERVA_V1_RECORD_MUTATION;
        expected_stream = NERVA_V1_STREAM_EXECUTION;
        expected_trust = NERVA_V1_TRUST_KERNEL_OBSERVED;
    } else if (status == GENESIS_V1_RESULT_AUTHORITY_DENIED) {
        expected_type = NERVA_V1_RECORD_AUTHORITY;
        expected_stream = NERVA_V1_STREAM_AUTHORITY;
        expected_trust = NERVA_V1_TRUST_CONSTITUTIONAL;
    } else {
        expected_type = NERVA_V1_RECORD_OBSERVATION;
        expected_stream = NERVA_V1_STREAM_EXECUTION;
        expected_trust = NERVA_V1_TRUST_KERNEL_OBSERVED;
    }
    for (i = 0u; i < NERVA_V1_QUEUE_CAPACITY + 2u; ++i) {
        uint32_t j;
        int matches = 1;
        long rc = bear_raw_syscall6(BEY_NERVA_POLL, (long)&record,
                                    0, 0, 0, 0, 0);
        if (rc != 0) break;
        for (j = 0u; j < NERVA_V1_ID_SIZE; ++j) {
            if (record.request_id[j] != envelope->intent.request_id.bytes[j] ||
                record.intent_id[j] != envelope->intent.intent_id.bytes[j]) {
                matches = 0;
                break;
            }
        }
        if (!matches) continue;
        if (record.record_type != expected_type ||
            record.stream_id != expected_stream ||
            record.trust_class != expected_trust || record.sequence == 0u ||
            record.before_revision != genesis_v1_u64_from_le(
                envelope->result.before_body_revision_le) ||
            record.after_revision != genesis_v1_u64_from_le(
                envelope->result.after_body_revision_le) ||
            record.error != genesis_v1_u32_from_le(
                envelope->result.error_le)) {
            jash_writes(w, ctx, "genesis: invalid NERVA record\n");
            return 1;
        }
        jash_writef(w, ctx,
                    "genesis nerva type=%u stream=%u trust=%u sequence=%llu "
                    "revision=%llu->%llu\n",
                    (unsigned int)record.record_type,
                    (unsigned int)record.stream_id,
                    (unsigned int)record.trust_class,
                    (unsigned long long)record.sequence,
                    (unsigned long long)record.before_revision,
                    (unsigned long long)record.after_revision);
        return 0;
    }
    jash_writes(w, ctx, "genesis: matching NERVA record unavailable\n");
    return 1;
}

static int genesis_act_probe(jash_writer_t w, void* ctx, uint32_t opcode) {
    struct genesis_v1_page page;
    struct genesis_v1_ioctl_envelope envelope;
    const struct genesis_v1_node* probe;
    const struct genesis_v1_edge* edge;
    uint64_t generation;
    uint64_t body_revision;
    uint64_t graph_revision;
    uint64_t target_revision;
    uint64_t logical_clock;
    uint32_t status;
    uint32_t error;
    int ioctl_rc;

    if (genesis_get_page(w, ctx, &page) != 0) return 1;
    if (genesis_find_freeze_path(&page, &probe, &edge) != 0) {
        jash_writes(w, ctx,
                    "genesis: invalid probe/MAY_ACT_ON capability path\n");
        return 1;
    }

    generation = genesis_v1_u64_from_le(page.header.generation_le);
    body_revision = genesis_v1_u64_from_le(page.header.body_revision_le);
    graph_revision = genesis_v1_u64_from_le(page.header.graph_revision_le);
    target_revision = genesis_v1_u64_from_le(probe->object_revision_le);
    logical_clock = genesis_v1_u64_from_le(page.header.logical_clock_le);
    if (logical_clock > UINT64_MAX - 32u) {
        jash_writes(w, ctx, "genesis: logical clock cannot represent deadline +32\n");
        return 1;
    }

    genesis_v1_zero(&envelope, GENESIS_V1_IOCTL_ENVELOPE_SIZE);
    genesis_v1_intent_init(&envelope.intent);
    genesis_v1_result_init(&envelope.result);
    genesis_make_id(&envelope.intent.request_id, (uint8_t)'R', generation,
                    body_revision, graph_revision, target_revision,
                    logical_clock);
    genesis_make_id(&envelope.intent.intent_id, (uint8_t)'I', generation,
                    body_revision, graph_revision, target_revision,
                    logical_clock);
    envelope.intent.target_node_id = probe->id;
    envelope.intent.capability_id = edge->id;
    envelope.intent.expected_body_revision_le =
        genesis_v1_u64_to_le(body_revision);
    envelope.intent.expected_target_revision_le =
        genesis_v1_u64_to_le(target_revision);
    envelope.intent.deadline_le = genesis_v1_u64_to_le(logical_clock + 32u);
    envelope.intent.budget_le = genesis_v1_u64_to_le(
        opcode == GENESIS_V1_INTENT_RESUME ? 0u : 1u);
    envelope.intent.opcode_le = genesis_v1_u32_to_le(opcode);
    envelope.intent.flags_le =
        genesis_v1_u32_to_le(GENESIS_V1_INTENT_F_NONE);

    ioctl_rc = bear_ioctl(1, (unsigned long)SINGUOS_GENESIS_SUBMIT_V1,
                          &envelope);
    if (ioctl_rc != 0) {
        jash_writef(w, ctx, "genesis: SUBMIT ioctl failed (%d)\n", ioctl_rc);
        return 1;
    }
    if (!genesis_result_valid(&envelope)) {
        jash_writef(w, ctx, "genesis: SUBMIT returned an invalid result (ioctl=%d)\n",
                    ioctl_rc);
        return 1;
    }

    status = genesis_v1_u32_from_le(envelope.result.status_le);
    error = genesis_v1_u32_from_le(envelope.result.error_le);
    jash_writef(w, ctx,
                "genesis result status=%s(%u) error=%s(%u) "
                "body_revision=%llu->%llu target_revision=%llu->%llu\n",
                genesis_result_status_name(status), status,
                genesis_error_name(error), error,
                (unsigned long long)genesis_v1_u64_from_le(
                    envelope.result.before_body_revision_le),
                (unsigned long long)genesis_v1_u64_from_le(
                    envelope.result.after_body_revision_le),
                (unsigned long long)genesis_v1_u64_from_le(
                    envelope.result.before_target_revision_le),
                 (unsigned long long)genesis_v1_u64_from_le(
                     envelope.result.after_target_revision_le));
    if (genesis_render_nerva(w, ctx, &envelope) != 0) return 1;
    return status == GENESIS_V1_RESULT_COMMITTED ||
           error == GENESIS_V1_ERROR_ACTION_ROLLED_BACK ? 0 : 1;
}

static int genesis_h5(jash_writer_t w, void *ctx) {
    struct genesis_v1_h5_report report;
    uint32_t samples;
    uint32_t passed;
    uint32_t i;

    genesis_v1_zero(&report, GENESIS_V1_H5_REPORT_SIZE);
    if (bear_ioctl(1, (unsigned long)SINGUOS_GENESIS_H5_V1, &report) != 0) {
        jash_writes(w, ctx, "genesis: H5 ioctl failed\n");
        return 1;
    }
    if (genesis_v1_u16_from_le(report.version_major_le) !=
            GENESIS_V1_VERSION_MAJOR ||
        genesis_v1_u16_from_le(report.version_minor_le) !=
            GENESIS_V1_VERSION_MINOR ||
        genesis_v1_u32_from_le(report.report_size_le) !=
            GENESIS_V1_H5_REPORT_SIZE ||
        genesis_v1_u32_from_le(report.flags_le) != 0u)
        return 1;
    for (i = 0u; i < sizeof(report.reserved); ++i)
        if (report.reserved[i] != 0u) return 1;

    samples = genesis_v1_u32_from_le(report.samples_le);
    passed = genesis_v1_u32_from_le(report.passed_le);
    jash_writef(w, ctx,
                "genesis h5 samples=%u interval=AUTHORITY_ALLOW->FROZEN "
                "clock=kernel_monotonic p50_ns=%llu p95_ns=%llu max_ns=%llu "
                "threshold_ns=%llu gate=%s\n",
                samples,
                (unsigned long long)genesis_v1_u64_from_le(report.p50_ns_le),
                (unsigned long long)genesis_v1_u64_from_le(report.p95_ns_le),
                (unsigned long long)genesis_v1_u64_from_le(report.max_ns_le),
                (unsigned long long)genesis_v1_u64_from_le(report.threshold_ns_le),
                passed == 1u ? "PASS" : "FAIL");
    return passed == 1u && samples >= GENESIS_V1_H5_SAMPLES ? 0 : 1;
}

static int genesis_trace(jash_writer_t w, void *ctx) {
    struct genesis_v1_trace trace;
    char request[33], intent[33], actor[33], subject[33], capability[33];
    char received[33], authority[33], action[33], parent[33];

    genesis_v1_zero(&trace, GENESIS_V1_TRACE_SIZE);
    if (bear_ioctl(1, (unsigned long)SINGUOS_GENESIS_TRACE_V1, &trace) != 0 ||
        genesis_v1_u32_from_le(trace.trace_size_le) != GENESIS_V1_TRACE_SIZE) {
        jash_writes(w, ctx, "genesis: no causal trace available\n");
        return 1;
    }
    genesis_id_hex(&trace.request_id, request);
    genesis_id_hex(&trace.intent_id, intent);
    genesis_id_hex(&trace.actor_node_id, actor);
    genesis_id_hex(&trace.subject_node_id, subject);
    genesis_id_hex(&trace.capability_id, capability);
    genesis_id_hex(&trace.received_event_id, received);
    genesis_id_hex(&trace.authority_event_id, authority);
    genesis_id_hex(&trace.action_event_id, action);
    genesis_id_hex(&trace.causal_parent_event_id, parent);
    jash_writef(w, ctx,
        "genesis trace sequence=%llu request=%s intent=%s actor=%s subject=%s "
        "capability=%s received_event=%s authority_event=%s action_event=%s "
        "causal_parent=%s opcode=%u status=%u error=%u authority=%u action=%u "
        "state=%u->%u body_revision=%llu->%llu target_revision=%llu->%llu "
        "budget=%llu->%llu latency_ns=%llu trace_end=1\n",
        (unsigned long long)genesis_v1_u64_from_le(trace.sequence_le),
        request, intent, actor, subject, capability, received, authority, action,
        parent, genesis_v1_u32_from_le(trace.opcode_le),
        genesis_v1_u32_from_le(trace.status_le),
        genesis_v1_u32_from_le(trace.error_le), trace.authority_decision,
        trace.action_decision,
        (unsigned)genesis_v1_u16_from_le(trace.before_state_le),
        (unsigned)genesis_v1_u16_from_le(trace.after_state_le),
        (unsigned long long)genesis_v1_u64_from_le(trace.before_body_revision_le),
        (unsigned long long)genesis_v1_u64_from_le(trace.after_body_revision_le),
        (unsigned long long)genesis_v1_u64_from_le(trace.before_target_revision_le),
        (unsigned long long)genesis_v1_u64_from_le(trace.after_target_revision_le),
        (unsigned long long)genesis_v1_u64_from_le(trace.before_budget_le),
        (unsigned long long)genesis_v1_u64_from_le(trace.after_budget_le),
        (unsigned long long)genesis_v1_u64_from_le(trace.latency_ns_le));
    return 0;
}

int jash_b_genesis(jash_state_t* s, jash_writer_t w, void* ctx,
                   const char* args) {
    (void)s;
    if (genesis_args_equal(args, "state")) return genesis_print_state(w, ctx);
    if (genesis_args_equal(args, "freeze") ||
        genesis_args_equal(args, "freeze probe"))
        return genesis_act_probe(w, ctx, GENESIS_V1_INTENT_FREEZE);
    if (genesis_args_equal(args, "stop"))
        return genesis_act_probe(w, ctx, GENESIS_V1_INTENT_SAFE_STOP);
    if (genesis_args_equal(args, "isolate"))
        return genesis_act_probe(w, ctx, GENESIS_V1_INTENT_ISOLATE);
    if (genesis_args_equal(args, "resume"))
        return genesis_act_probe(w, ctx, GENESIS_V1_INTENT_RESUME);
    if (genesis_args_equal(args, "rollback"))
        return genesis_act_probe(w, ctx, GENESIS_V1_INTENT_ROLLBACK);
    if (genesis_args_equal(args, "h5")) return genesis_h5(w, ctx);
    if (genesis_args_equal(args, "trace")) return genesis_trace(w, ctx);

    jash_writes(w, ctx,
                "usage: genesis state | freeze [probe] | stop | isolate | "
                "resume | rollback | h5 | trace\n");
    return 2;
}

#endif /* JASH_TARGET_BEYOND */
