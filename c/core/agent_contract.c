// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "agent_contract.h"
#include "serve.h"
#include "mutation_report.h"
#include <builtins/builtin.h>
#include <osif/osinfo.h>
#include <string.h>

#include "builtin_contract_strings.inc"
#include "builtin_contract_catalog.inc"
#include "builtin_mode_contracts.inc"

size_t jash_builtin_contract_count(void) {
    return sizeof(BUILTIN_CONTRACTS) / sizeof(BUILTIN_CONTRACTS[0]);
}

const jash_builtin_contract_t* jash_builtin_contract_at(size_t index) {
    return index < jash_builtin_contract_count() ? &BUILTIN_CONTRACTS[index] : NULL;
}

const jash_builtin_contract_t* jash_builtin_contract_find(const char* name) {
    if (!name) return NULL;
    size_t lo = 0, hi = jash_builtin_contract_count();
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        int order = strcmp(name, BUILTIN_CONTRACTS[mid].name);
        if (!order) return &BUILTIN_CONTRACTS[mid];
        if (order < 0) hi = mid;
        else lo = mid + 1;
    }
    return NULL;
}

/* The generator emits contiguous modes in sorted builtin-name order. Looking
 * up that range avoids rescanning every mode for each family and variant in
 * the coverage document, including a single-operation discovery request. */
static size_t builtin_mode_begin(const char* name) {
    size_t lo = 0, hi = sizeof(BUILTIN_MODE_CONTRACTS) / sizeof(BUILTIN_MODE_CONTRACTS[0]);
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        if (strcmp(BUILTIN_MODE_CONTRACTS[mid].name, name) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo;
}

size_t jash_builtin_mode_contract_count(const char* name) {
    if (!name) return 0;
    size_t begin = builtin_mode_begin(name), end = begin;
    size_t total = sizeof(BUILTIN_MODE_CONTRACTS) / sizeof(BUILTIN_MODE_CONTRACTS[0]);
    while (end < total && !strcmp(name, BUILTIN_MODE_CONTRACTS[end].name)) end++;
    return end - begin;
}

const jash_builtin_mode_contract_t* jash_builtin_mode_contract_at(const char* name, size_t index) {
    if (!name) return NULL;
    size_t begin = builtin_mode_begin(name);
    size_t total = sizeof(BUILTIN_MODE_CONTRACTS) / sizeof(BUILTIN_MODE_CONTRACTS[0]);
    if (index >= total - begin) return NULL;
    const jash_builtin_mode_contract_t* entry = &BUILTIN_MODE_CONTRACTS[begin + index];
    return strcmp(entry->name, name) ? NULL : entry;
}

#define BASE (SRV_JOB_SEEN_ID | SRV_JOB_SEEN_VERSION | SRV_JOB_SEEN_OP | SRV_JOB_SEEN_CAPS)
#define JOB (BASE | SRV_JOB_SEEN_JOB_ID)
#define CREATE (BASE | SRV_JOB_SEEN_IR | SRV_JOB_SEEN_ARGV | SRV_JOB_SEEN_CWD | \
                SRV_JOB_SEEN_ENV | SRV_JOB_SEEN_STDIN_B64 | SRV_JOB_SEEN_ARTIFACT_REF | \
                SRV_JOB_SEEN_STREAM_REF | SRV_JOB_SEEN_EXECUTION_SPEC | SRV_JOB_SEEN_RESOURCES)
#define START (CREATE | SRV_JOB_SEEN_BACKGROUND | SRV_JOB_SEEN_PTY | SRV_JOB_SEEN_ROWS | SRV_JOB_SEEN_COLS | SRV_JOB_SEEN_STDIN_MODE)
#define READ (JOB | SRV_JOB_SEEN_STREAM | SRV_JOB_SEEN_OFFSET | SRV_JOB_SEEN_LIMIT)
#define RESIZE (JOB | SRV_JOB_SEEN_ROWS | SRV_JOB_SEEN_COLS)
#define STREAM_OPEN (JOB | SRV_JOB_SEEN_STREAM | SRV_JOB_SEEN_OFFSET)
#define STREAM_CREDIT (BASE | SRV_JOB_SEEN_STREAM_REF | SRV_JOB_SEEN_CREDIT_BYTES | SRV_JOB_SEEN_ACK_OFFSET)

static const jash_job_contract_t JOBS[] = {
    {"budget.get", BASE, BASE, "store:read", "base fields required", "observe bounded managed storage and job budget; unavailable rates remain null"},
    {"data.policy.get", BASE, BASE, "store:read", "base fields required", "read typed data-plane policy, revision and authority model"},
    {"data.probe", BASE | SRV_JOB_SEEN_TRANSPORT, BASE, "store:read", "base fields required", "probe compiled, available and permitted local buffer transport"},
    {"data.import", BASE | SRV_JOB_SEEN_TRANSPORT | SRV_JOB_SEEN_DATA_B64, BASE | SRV_JOB_SEEN_TRANSPORT, "data:create", "transport required", "import inline bytes or one sealed FD as a session-owned immutable handle"},
    {"data.read", BASE | SRV_JOB_SEEN_ARTIFACT_REF | SRV_JOB_SEEN_OFFSET | SRV_JOB_SEEN_LIMIT,
     BASE | SRV_JOB_SEEN_ARTIFACT_REF | SRV_JOB_SEEN_OFFSET | SRV_JOB_SEEN_LIMIT, "data:read", "artifact_ref, offset and limit required", "read a bounded immutable byte range without sharing seek offsets"},
    {"data.release", BASE | SRV_JOB_SEEN_ARTIFACT_REF, BASE | SRV_JOB_SEEN_ARTIFACT_REF, "data:manage", "artifact_ref required", "release a volatile session handle"},
    {"data.promote", BASE | SRV_JOB_SEEN_ARTIFACT_REF, BASE | SRV_JOB_SEEN_ARTIFACT_REF, "data:manage", "artifact_ref required", "publish a kept durable artifact with signed evidence and persistent request replay"},
    {"data.attach", BASE | SRV_JOB_SEEN_ARTIFACT_REF, BASE | SRV_JOB_SEEN_ARTIFACT_REF, "data:read", "artifact_ref required", "verify a durable artifact and attach a sealed private snapshot to this session"},
    {"data.stream.open", STREAM_OPEN, JOB | SRV_JOB_SEEN_STREAM, "job:read", "job_id and stream required", "open an independent session-local cursor over bounded job output"},
    {"data.stream.credit", STREAM_CREDIT, STREAM_CREDIT, "job:read", "stream_ref, credit_bytes and ack_offset required", "grant bounded delivery credit and acknowledge an exact output prefix"},
    {"data.stream.cancel", BASE | SRV_JOB_SEEN_STREAM_REF, BASE | SRV_JOB_SEEN_STREAM_REF, "job:read", "stream_ref required", "release one output consumer without signalling its producer"},
    {"data.policy.set", BASE | SRV_JOB_SEEN_REVISION | SRV_JOB_SEEN_FD_ENABLED | SRV_JOB_SEEN_MAX_BYTES | SRV_JOB_SEEN_DRY_RUN,
     BASE | SRV_JOB_SEEN_REVISION | SRV_JOB_SEEN_FD_ENABLED | SRV_JOB_SEEN_MAX_BYTES, "store:manage", "revision, fd_enabled and max_bytes required", "compare revision and atomically publish data policy"},
    {"store.history", BASE | SRV_JOB_SEEN_LIMIT | SRV_JOB_SEEN_CURSOR, BASE,
     "store:read", "base fields required", "page maintenance transactions and unattributed evidence with a guarded snapshot cursor"},
    {"store.inspect", BASE | SRV_JOB_SEEN_TARGET_ID, BASE, "store:read", "base fields required", "inspect artifacts or maintenance evidence for target_id"},
    {"store.resolve", BASE | SRV_JOB_SEEN_TARGET_ID | SRV_JOB_SEEN_DRY_RUN, BASE | SRV_JOB_SEEN_TARGET_ID,
     "store:manage", "store.resolve requires target_id", "publish a verified retained maintenance result without repeating its effects"},
    {"store.gc", BASE | SRV_JOB_SEEN_AGE | SRV_JOB_SEEN_DRY_RUN, BASE, "store:manage", "base fields required", "reclaim verified unreferenced or redundant artifacts while runtimes are quiescent"},
    {"store.quota", BASE | SRV_JOB_SEEN_MAX_BYTES | SRV_JOB_SEEN_DRY_RUN, BASE | SRV_JOB_SEEN_MAX_BYTES,
     "store:manage", "store.quota requires max_bytes", "configure CAS/archive admission quota; zero is unlimited"},
    {"store.journal.quota", BASE | SRV_JOB_SEEN_MAX_BYTES | SRV_JOB_SEEN_DRY_RUN, BASE | SRV_JOB_SEEN_MAX_BYTES,
     "store:manage", "store.journal.quota requires max_bytes", "configure logical maintenance journal admission quota; recovery is exempt"},
    {"store.journal.gc", BASE | SRV_JOB_SEEN_AGE | SRV_JOB_SEEN_DRY_RUN, BASE,
     "store:manage", "base fields required", "reclaim only verified redundant journal result staging; preserve public evidence"},
    {"store.release", BASE | SRV_JOB_SEEN_ROOT_NS | SRV_JOB_SEEN_ROOT_NAME | SRV_JOB_SEEN_ROOT_HASH | SRV_JOB_SEEN_DRY_RUN,
     BASE | SRV_JOB_SEEN_ROOT_NS | SRV_JOB_SEEN_ROOT_NAME | SRV_JOB_SEEN_ROOT_HASH,
     "store:manage", "store.release requires root_namespace, root_name and root_blake3", "release a root's retention role without deleting execution evidence"},
    {"exec", CREATE, BASE, NULL, "id, protocol_version, op, and caps are required",
     "execute external IR and wait for a terminal result"},
    {"job.start", START, BASE | SRV_JOB_SEEN_BACKGROUND, NULL,
     "job.start requires background:true", "start an external job with an opaque runtime-scoped id"},
    {"job.get", JOB, JOB, "job:read", "operation requires job_id", "inspect a job"},
    {"job.result", JOB, JOB, "job:read", "operation requires job_id", "read a retained session-end observation; never reattach a live job"},
    {"job.wait", JOB | SRV_JOB_SEEN_WAIT, JOB | SRV_JOB_SEEN_WAIT, "job:read",
     "job.wait requires wait_ms", "wait within a bounded deadline"},
    {"job.cancel", JOB | SRV_JOB_SEEN_GRACE, JOB, "job:signal",
     "operation requires job_id", "cancel a job with TERM/KILL escalation"},
    {"job.resize", RESIZE, RESIZE, "job:pty:resize",
     "job.resize requires rows and cols", "resize a PTY job"},
    {"job.read", READ, READ, "job:read", "job.read requires stream, offset, and limit",
     "read a bounded byte range of job output"},
    {"job.write", JOB | SRV_JOB_SEEN_DATA_B64 | SRV_JOB_SEEN_WAIT,
      JOB | SRV_JOB_SEEN_DATA_B64, NULL, "job.write requires data_b64",
      "write bounded bytes to a PTY or explicitly writable stdin pipe"},
    {"job.close-stdin", JOB, JOB, "job:stdin:write", "operation requires job_id",
      "idempotently half-close an explicitly writable stdin pipe"},
};

size_t jash_job_contract_count(void) { return sizeof(JOBS) / sizeof(JOBS[0]); }
const jash_job_contract_t* jash_job_contract_at(size_t index) {
    return index < jash_job_contract_count() ? &JOBS[index] : NULL;
}
const jash_job_contract_t* jash_job_contract_find(const char* name) {
    if (!name) return NULL;
    for (size_t i = 0; i < jash_job_contract_count(); i++)
        if (!strcmp(name, JOBS[i].name)) return &JOBS[i];
    return NULL;
}
const char* jash_job_contract_fields(const jash_job_contract_t* op, uint64_t seen) {
    if (!op) return "unknown operation";
    if ((seen & BASE) != BASE) return "id, protocol_version, op, and caps are required";
    if ((op->required & SRV_JOB_SEEN_JOB_ID) && !(seen & SRV_JOB_SEEN_JOB_ID))
        return "operation requires job_id";
    if ((seen & op->required) != op->required) return op->missing_error;
    if (seen & ~op->allowed) return "field is not valid for operation";
    return NULL;
}

unsigned jash_sync_contract_allowed(void) {
    unsigned fields = SRV_SEEN_ID | SRV_SEEN_ARGV | SRV_SEEN_LINE | SRV_SEEN_STDIN |
                      SRV_SEEN_STDIN_B64 | SRV_SEEN_TIMEOUT | SRV_SEEN_CWD |
                      SRV_SEEN_ENV | SRV_SEEN_VERSION | SRV_SEEN_EXECUTION_SPEC;
#ifdef JASH_AGENT_ONLY
    fields |= SRV_SEEN_CAPS | SRV_SEEN_CANCELABLE;
#endif
    return fields;
}

const char* jash_sync_contract_fields(unsigned seen, int argc, int version) {
    if (seen & ~jash_sync_contract_allowed()) return "field is not valid for invocation profile";
#ifdef JASH_AGENT_ONLY
    if (!(seen & SRV_SEEN_ID)) return "id is required";
#endif
    unsigned command = seen & (SRV_SEEN_ARGV | SRV_SEEN_LINE);
    if (command == (SRV_SEEN_ARGV | SRV_SEEN_LINE)) return "argv and line are exclusive";
    if (!command || (command == SRV_SEEN_ARGV && argc == 0)) return "need argv or line";
    if ((seen & (SRV_SEEN_STDIN | SRV_SEEN_STDIN_B64)) == (SRV_SEEN_STDIN | SRV_SEEN_STDIN_B64))
        return "stdin and stdin_b64 are exclusive";
    if (version != 1 && version != 2) return "protocol version must be 1 or 2";
    if ((seen & SRV_SEEN_STDIN_B64) && version != 2) return "stdin_b64 requires protocol version 2";
    return NULL;
}

typedef struct { const char* name; uint64_t bit; const char* type; } field_t;
static const field_t FIELDS[] = {
    {"revision", SRV_JOB_SEEN_REVISION, "string"},
    {"fd_enabled", SRV_JOB_SEEN_FD_ENABLED, "boolean"},
    {"artifact_ref", SRV_JOB_SEEN_ARTIFACT_REF, "string"},
    {"transport", SRV_JOB_SEEN_TRANSPORT, "string"},
    {"stream_ref", SRV_JOB_SEEN_STREAM_REF, "string"},
    {"credit_bytes", SRV_JOB_SEEN_CREDIT_BYTES, "integer"},
    {"ack_offset", SRV_JOB_SEEN_ACK_OFFSET, "integer"},
    {"cursor", SRV_JOB_SEEN_CURSOR, "string"},
    {"target_id", SRV_JOB_SEEN_TARGET_ID, "string"},
    {"root_namespace", SRV_JOB_SEEN_ROOT_NS, "string"},
    {"root_name", SRV_JOB_SEEN_ROOT_NAME, "string"},
    {"root_blake3", SRV_JOB_SEEN_ROOT_HASH, "string"},
    {"max_bytes", SRV_JOB_SEEN_MAX_BYTES, "integer"},
    {"older_than_seconds", SRV_JOB_SEEN_AGE, "integer"},
    {"dry_run", SRV_JOB_SEEN_DRY_RUN, "boolean"},
    {"id", SRV_JOB_SEEN_ID, "string"},
    {"protocol_version", SRV_JOB_SEEN_VERSION, "integer"},
    {"op", SRV_JOB_SEEN_OP, "string"}, {"caps", SRV_JOB_SEEN_CAPS, "array"},
    {"ir", SRV_JOB_SEEN_IR, "object"}, {"argv", SRV_JOB_SEEN_ARGV, "array"},
    {"background", SRV_JOB_SEEN_BACKGROUND, "boolean"},
    {"pty", SRV_JOB_SEEN_PTY, "boolean"}, {"rows", SRV_JOB_SEEN_ROWS, "integer"},
    {"cols", SRV_JOB_SEEN_COLS, "integer"}, {"job_id", SRV_JOB_SEEN_JOB_ID, "string"},
    {"wait_ms", SRV_JOB_SEEN_WAIT, "integer"}, {"grace_ms", SRV_JOB_SEEN_GRACE, "integer"},
    {"stream", SRV_JOB_SEEN_STREAM, "string"}, {"offset", SRV_JOB_SEEN_OFFSET, "integer"},
    {"limit", SRV_JOB_SEEN_LIMIT, "integer"}, {"cwd", SRV_JOB_SEEN_CWD, "string"},
    {"env", SRV_JOB_SEEN_ENV, "object"}, {"stdin_b64", SRV_JOB_SEEN_STDIN_B64, "string"},
    {"data_b64", SRV_JOB_SEEN_DATA_B64, "string"}, {"stdin_mode", SRV_JOB_SEEN_STDIN_MODE, "string"},
    {"execution_spec_sha256", SRV_JOB_SEEN_EXECUTION_SPEC, "string"},
    {"resources", SRV_JOB_SEEN_RESOURCES, "object"},
};

static const field_t SYNC_FIELDS[] = {
#ifdef JASH_AGENT_ONLY
    {"id", SRV_SEEN_ID, "string"}, {"timeout_ms", SRV_SEEN_TIMEOUT, "integer"},
    {"caps", SRV_SEEN_CAPS, "array"}, {"cancelable", SRV_SEEN_CANCELABLE, "boolean"},
#else
    {"id", SRV_SEEN_ID, "json_value"}, {"timeout_ms", SRV_SEEN_TIMEOUT, "json_value"},
#endif
    {"argv", SRV_SEEN_ARGV, "array"}, {"line", SRV_SEEN_LINE, "string"},
    {"stdin", SRV_SEEN_STDIN, "string"}, {"stdin_b64", SRV_SEEN_STDIN_B64, "string"},
    {"cwd", SRV_SEEN_CWD, "string"}, {"env", SRV_SEEN_ENV, "object"},
    {"protocol_version", SRV_SEEN_VERSION, "integer"},
    {"execution_spec_sha256", SRV_SEEN_EXECUTION_SPEC, "string"},
};

/* A separately versioned document adds invocation discovery without changing
 * the already introduced builtin/job inventory document's shape. */
static int invocation_json(const char* name, jash_json_sink_t sink, void* context) {
    jash_json_writer_t w;
    jash_jsonw_init(&w, sink, context);
    int cli = !strcmp(name, "exec-json");
    int local = !strcmp(name, "listen");
    jash_jsonw_raw(&w, "{\"schema\":\"jash.agent-contract.invocation.v1\",\"version\":1,\"profile\":");
#ifdef JASH_AGENT_ONLY
    jash_jsonw_string(&w, "agent-strict");
#else
    jash_jsonw_string(&w, "general");
#endif
    jash_jsonw_raw(&w, ",\"target\":"); jash_jsonw_string(&w, bear_osinfo_target());
    jash_jsonw_raw(&w, ",\"id\":"); jash_jsonw_string_pair(&w, "invocation:", name);
    jash_jsonw_raw(&w, ",\"availability\":");
#ifdef JASH_AGENT_ONLY
    jash_jsonw_string(&w, cli ? "requires_general_profile" : "compiled");
#else
    jash_jsonw_string(&w, local ? "requires_agent_profile" : "compiled");
#endif
    jash_jsonw_raw(&w, ",\"input_transport\":"); jash_jsonw_string(&w, local ? "unix-jsonl" : cli ? "argv-json" : "stdio-jsonl");
    jash_jsonw_raw(&w, ",\"contract_status\":\"structural\",\"complete\":false,"
                       "\"protocol_versions\":[1,2],\"request_version_default\":1,\"allowed_fields\":[");
    unsigned allowed = jash_sync_contract_allowed();
    int first = 1;
    for (size_t i = 0; i < sizeof(SYNC_FIELDS) / sizeof(SYNC_FIELDS[0]); i++) {
        if (!(SYNC_FIELDS[i].bit & allowed)) continue;
        if (!first) jash_jsonw_raw(&w, ",");
        first = 0;
        jash_jsonw_string(&w, SYNC_FIELDS[i].name);
    }
    jash_jsonw_raw(&w, "],\"field_types\":{");
    first = 1;
    for (size_t i = 0; i < sizeof(SYNC_FIELDS) / sizeof(SYNC_FIELDS[0]); i++) {
        if (!(SYNC_FIELDS[i].bit & allowed)) continue;
        if (!first) jash_jsonw_raw(&w, ",");
        first = 0;
        jash_jsonw_string(&w, SYNC_FIELDS[i].name); jash_jsonw_raw(&w, ":");
        jash_jsonw_string(&w, SYNC_FIELDS[i].type);
    }
    jash_jsonw_raw(&w, "},\"field_aliases\":{\"request_version\":\"protocol_version\"},"
                       "\"exclusive_fields\":[[\"argv\",\"line\"],[\"stdin\",\"stdin_b64\"]],"
                       "\"traits\":{");
#ifdef JASH_AGENT_ONLY
    jash_jsonw_raw(&w, "\"id_required\":true,\"capabilities_required\":true,"
                       "\"text_line_authorized\":false,\"timeout_ms\":\"effect-exec-only\"");
#else
    jash_jsonw_raw(&w, "\"id_required\":false,\"capabilities_required\":false,"
                       "\"text_line_authorized\":true,\"timeout_ms\":\"reserved\"");
#endif
    if (local) jash_jsonw_raw(&w, ",\"authentication\":\"canonical-scope-token-v1\",\"peer\":\"same-effective-uid\","
        "\"control\":{\"negotiation\":\"control=fd-v1\",\"attachment\":\"scm-rights-one-byte-v1\",\"lifetime\":\"connection-worker\",\"shared\":false},"
        "\"shutdown\":{\"signals\":[\"SIGINT\",\"SIGTERM\"],\"policy\":\"drain-workers\",\"deadline_ms\":null,\"parent_loss\":\"cooperative-cleanup\"}");
    jash_jsonw_raw(&w, ",\"stdin_b64_requires_version\":2},\"input_schema\":null,"
                       "\"result_schema\":null,\"semantic_validation_required\":true,"
                       "\"source\":\"c/core/serve.c\"}\n");
    return jash_jsonw_finish(&w) == 0 ? 0 : 1;
}

static void field_names(jash_json_writer_t* w, uint64_t mask) {
    int first = 1;
    jash_jsonw_raw(w, "[");
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        if (!(mask & FIELDS[i].bit)) continue;
        if (!first) jash_jsonw_raw(w, ",");
        first = 0;
        jash_jsonw_string(w, FIELDS[i].name);
    }
    jash_jsonw_raw(w, "]");
}

static void byte_string_schema(jash_json_writer_t* w) {
    jash_jsonw_raw(w, "{\"oneOf\":[{\"type\":\"string\"},{\"type\":\"object\","
                       "\"additionalProperties\":false,\"required\":[\"encoding\",\"data\"],"
                       "\"properties\":{\"encoding\":{\"const\":\"base64\"},"
                       "\"data\":{\"type\":\"string\",\"contentEncoding\":\"base64\"}}}],"
                       "\"x-noDecodedNul\":true}");
}

static void input_schema(jash_json_writer_t* w, const jash_job_contract_t* op) {
    jash_jsonw_raw(w, "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
                       "\"type\":\"object\",\"additionalProperties\":false,\"properties\":{");
    int first = 1;
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        const field_t* field = &FIELDS[i];
        if (!(op->allowed & field->bit)) continue;
        if (!first) jash_jsonw_raw(w, ",");
        first = 0;
        jash_jsonw_string(w, field->name);
        if (field->bit == SRV_JOB_SEEN_CWD) {
            jash_jsonw_raw(w, ":"); byte_string_schema(w); continue;
        }
        jash_jsonw_raw(w, ":{\"type\":"); jash_jsonw_string(w, field->type);
        if (field->bit == SRV_JOB_SEEN_VERSION) jash_jsonw_raw(w, ",\"const\":2");
        if (field->bit == SRV_JOB_SEEN_OP) {
            jash_jsonw_raw(w, ",\"const\":"); jash_jsonw_string(w, op->name);
        }
        if (field->bit == SRV_JOB_SEEN_ID || field->bit == SRV_JOB_SEEN_TARGET_ID) {
            jash_jsonw_raw(w, ",\"minLength\":1,\"x-maxDecodedBytes\":");
            jash_jsonw_u64(w, JASH_AGENT_ID_VALUE_CAP - 1u);
        }
        if (!strcmp(op->name, "store.history") && field->bit == SRV_JOB_SEEN_LIMIT)
            jash_jsonw_raw(w, ",\"minimum\":1,\"maximum\":64,\"default\":16");
        if (field->bit == SRV_JOB_SEEN_CURSOR)
            jash_jsonw_raw(w, ",\"pattern\":\"^m1:[0-9a-f]{64}:(0|[1-9][0-9]*)$\",\"maxLength\":95");
        if (field->bit == SRV_JOB_SEEN_REVISION)
            jash_jsonw_raw(w, ",\"pattern\":\"^[0-9a-f]{64}$\"");
        if (field->bit == SRV_JOB_SEEN_ARTIFACT_REF)
            jash_jsonw_raw(w, ",\"minLength\":1,\"x-maxDecodedBytes\":159");
        if (field->bit == SRV_JOB_SEEN_STREAM_REF)
            jash_jsonw_raw(w, ",\"pattern\":\"^s-[0-9a-f]{32}-[0-9a-f]{16}$\"");
        if (field->bit == SRV_JOB_SEEN_CREDIT_BYTES)
            jash_jsonw_raw(w, ",\"minimum\":0,\"maximum\":65536");
        if (field->bit == SRV_JOB_SEEN_ACK_OFFSET)
            jash_jsonw_raw(w, ",\"minimum\":0");
        if (field->bit == SRV_JOB_SEEN_BACKGROUND) jash_jsonw_raw(w, ",\"const\":true");
        if (field->bit == SRV_JOB_SEEN_ARGV) {
            jash_jsonw_raw(w, ",\"items\":"); byte_string_schema(w);
        } else if (!strcmp(field->type, "array"))
            jash_jsonw_raw(w, ",\"items\":{\"type\":\"string\"}");
        jash_jsonw_raw(w, "}");
    }
    jash_jsonw_raw(w, ",\"request_version\":{\"type\":\"integer\",\"const\":2}},\"required\":");
    field_names(w, op->required & ~(uint64_t)SRV_JOB_SEEN_VERSION);
    jash_jsonw_raw(w, ",\"oneOf\":[{\"required\":[\"protocol_version\"],"
                       "\"not\":{\"required\":[\"request_version\"]}},"
                       "{\"required\":[\"request_version\"],"
                       "\"not\":{\"required\":[\"protocol_version\"]}}],"
                       "\"x-validation-scope\":\"structural; server semantic checks also required\"}");
}

static void schema_ref(jash_json_writer_t* w, const char* ref) {
    if (!ref) { jash_jsonw_raw(w, "null"); return; }
    jash_jsonw_raw(w, "{\"$ref\":");
    jash_jsonw_string(w, ref);
    jash_jsonw_raw(w, "}");
}

/* These are operation documents, not a claim that an admission/protocol error
 * has the same shape. store.inspect selects a different document by target_id. */
static void job_result_schema(jash_json_writer_t* w, const char* name) {
    if (!strcmp(name, "store.inspect")) {
        jash_jsonw_raw(w, "{\"oneOf\":[{\"$ref\":\"urn:jash:agent-store:v1\"},"
                           "{\"$ref\":\"urn:jash:agent-maintenance-inspection:v1\"}]}");
        return;
    }
    const char* ref = NULL;
    if (!strcmp(name, "job.result")) ref = "urn:jash:agent-job-result:v1";
    else if (!strcmp(name, "store.history")) ref = "urn:jash:agent-maintenance-history:v1";
    else if (!strcmp(name, "store.resolve")) ref = "urn:jash:agent-maintenance-resolution:v1";
    else if (!strncmp(name, "store.", 6)) ref = "urn:jash:agent-store:v1";
    else if (!strcmp(name, "budget.get")) ref = "urn:jash:agent-budget:v1";
    else if (!strcmp(name, "data.policy.get") || !strcmp(name, "data.policy.set")) ref = "urn:jash:agent-data-policy:v1";
    else if (!strcmp(name, "data.probe")) ref = "urn:jash:agent-data-probe:v1";
    else if (!strcmp(name, "data.promote")) ref = "urn:jash:agent-artifact:v1";
    else if (!strncmp(name, "data.stream.", 12)) ref = "urn:jash:agent-stream:v1";
    else if (!strcmp(name, "data.import") || !strcmp(name, "data.read") ||
             !strcmp(name, "data.release") || !strcmp(name, "data.attach")) ref = "urn:jash:agent-data:v1";
    schema_ref(w, ref);
}

static void job_json(jash_json_writer_t* w, const jash_job_contract_t* op) {
    jash_jsonw_raw(w, "{\"id\":"); jash_jsonw_string_pair(w, "runtime:", op->name);
    jash_jsonw_raw(w, ",\"name\":"); jash_jsonw_string(w, op->name);
    jash_jsonw_raw(w, ",\"kind\":\"runtime_operation\",\"summary\":");
    jash_jsonw_string(w, op->summary);
    jash_jsonw_raw(w, ",\"availability\":");
#ifdef JASH_AGENT_ONLY
    jash_jsonw_string(w, "compiled");
#else
    jash_jsonw_string(w, "requires_agent_profile");
#endif
    jash_jsonw_raw(w, ",\"contract_status\":\"structural\",\"protocol_version\":2,"
                       "\"complete\":false,\"required_fields\":");
    field_names(w, op->required);
    jash_jsonw_raw(w, ",\"allowed_fields\":"); field_names(w, op->allowed);
    jash_jsonw_raw(w, ",\"field_types\":{");
    int first = 1;
    for (size_t i = 0; i < sizeof(FIELDS) / sizeof(FIELDS[0]); i++) {
        if (!(op->allowed & FIELDS[i].bit)) continue;
        if (!first) jash_jsonw_raw(w, ",");
        first = 0;
        jash_jsonw_string(w, FIELDS[i].name); jash_jsonw_raw(w, ":");
        jash_jsonw_string(w, FIELDS[i].type);
    }
    jash_jsonw_raw(w, "},\"field_aliases\":{\"request_version\":\"protocol_version\"},"
                       "\"capability\":");
    if (op->capability) jash_jsonw_string(w, op->capability);
    else jash_jsonw_raw(w, "null");
    jash_jsonw_raw(w, ",\"semantic_validation_required\":true,\"input_schema\":");
    input_schema(w, op);
    jash_jsonw_raw(w, ",\"result_schema\":");
    job_result_schema(w, op->name);
    jash_jsonw_raw(w, ",\"result_schema_scope\":\"operation-result; admission/protocol errors may use an unversioned error object\"");
    if (!strcmp(op->name, "store.inspect"))
        jash_jsonw_raw(w, ",\"result_variants\":["
            "{\"when\":{\"not\":{\"required\":[\"target_id\"]}},\"result_schema\":{\"$ref\":\"urn:jash:agent-store:v1\"}},"
            "{\"when\":{\"required\":[\"target_id\"]},\"result_schema\":{\"$ref\":\"urn:jash:agent-maintenance-inspection:v1\"}}]");
    jash_jsonw_raw(w, ",\"source\":\"c/core/serve.c\"}");
}

static const char* relevance_name(const jash_builtin_contract_t* contract) {
    if (!contract) return "unclassified";
    switch (contract->role) {
        case JASH_CONTRACT_STRUCTURED: return "structured";
        case JASH_CONTRACT_EXECUTION_ENVELOPE: return "execution-envelope";
        case JASH_CONTRACT_INTERACTIVE: return "interactive";
    }
    return "unclassified";
}

static const char* output_name(jash_builtin_contract_output_t output) {
    switch (output) {
        case JASH_CONTRACT_OUTPUT_NATIVE: return "native";
        case JASH_CONTRACT_OUTPUT_ALTERNATE: return "alternate";
        case JASH_CONTRACT_OUTPUT_MODULE: return "optional-module";
        case JASH_CONTRACT_OUTPUT_NONE: return "none";
    }
    return "none";
}

static void mode_input_schema(jash_json_writer_t* w, const jash_builtin_mode_contract_t* mode) {
    jash_jsonw_raw(w, "{\"$schema\":\"https://json-schema.org/draft/2020-12/schema\","
                       "\"type\":\"array\",\"minItems\":");
    jash_jsonw_u64(w, mode->selector ? 2 : 1);
    jash_jsonw_raw(w, ",\"prefixItems\":[{\"const\":"); jash_jsonw_string(w, mode->name);
    jash_jsonw_raw(w, "}");
    if (mode->selector && !strcmp(mode->selector_scope, "first-argument")) {
        jash_jsonw_raw(w, ",{\"const\":"); jash_jsonw_string(w, mode->selector);
        jash_jsonw_raw(w, "}");
    }
    jash_jsonw_raw(w, "],\"items\":"); byte_string_schema(w);
    if (mode->selector) {
        jash_jsonw_raw(w, ",\"contains\":{\"const\":"); jash_jsonw_string(w, mode->selector);
        jash_jsonw_raw(w, "}");
    }
    if (!strcmp(mode->name, "env") && !strcmp(mode->mode, "json"))
        jash_jsonw_raw(w, ",\"maxItems\":2");
    jash_jsonw_raw(w, ",\"x-validation-scope\":\"mode-selection; handler validates order, operands and option combinations\"}");
}

static int mode_qualified(const jash_builtin_mode_contract_t* mode) {
    /* Qualification is evidence for one target, not a property transferred
     * automatically to every port that happens to compile the descriptor. */
#if defined(__linux__) && !defined(JASH_TINALINUX)
    return mode->qualified;
#else
    (void)mode;
    return 0;
#endif
}

static int mode_complete(const jash_builtin_mode_contract_t* mode) {
    return mode_qualified(mode) && mode->options_complete;
}

static int family_mode_complete(const jash_builtin_contract_t* contract, const char* mode) {
    if (!contract || contract->role != JASH_CONTRACT_STRUCTURED || !contract->operations_complete)
        return 0;
    size_t count = 0;
    size_t total = jash_builtin_mode_contract_count(contract->name);
    for (size_t i = 0; i < total; i++) {
        const jash_builtin_mode_contract_t* entry = jash_builtin_mode_contract_at(contract->name, i);
        if (strcmp(entry->mode, mode)) continue;
        count++;
        if (!mode_complete(entry)) return 0;
    }
    return count != 0;
}

static int family_complete(const jash_builtin_contract_t* contract) {
    return family_mode_complete(contract, "json") && family_mode_complete(contract, "data");
}

static void mode_json(jash_json_writer_t* w, const jash_builtin_mode_contract_t* mode) {
    jash_jsonw_raw(w, "{\"id\":"); jash_jsonw_string(w, mode->id);
    jash_jsonw_raw(w, ",\"selector\":");
    if (mode->selector) jash_jsonw_string(w, mode->selector); else jash_jsonw_raw(w, "null");
    jash_jsonw_raw(w, ",\"selector_scope\":"); jash_jsonw_string(w, mode->selector_scope);
    jash_jsonw_raw(w, ",\"framing\":"); jash_jsonw_string(w, mode->framing);
    jash_jsonw_raw(w, ",\"input_framing\":");
    if (mode->input_framing) jash_jsonw_string(w, mode->input_framing);
    else jash_jsonw_raw(w, "null");
    jash_jsonw_raw(w, ",\"resource_limits\":");
    jash_jsonw_raw(w, mode->resource_limits_json ? mode->resource_limits_json : "null");
    jash_jsonw_raw(w, ",\"contract_status\":");
    jash_jsonw_string(w, mode_complete(mode) ? "qualified" : "partial");
    jash_jsonw_raw(w, ",\"complete\":"); jash_jsonw_raw(w, mode_complete(mode) ? "true" : "false");
    jash_jsonw_raw(w, ",\"options_complete\":"); jash_jsonw_raw(w, mode->options_complete ? "true" : "false");
    jash_jsonw_raw(w, ",\"qualified\":"); jash_jsonw_raw(w, mode_qualified(mode) ? "true" : "false");
    jash_jsonw_raw(w, ",\"qualification_scope\":");
    jash_jsonw_raw(w, mode->qualification_scope_json ? mode->qualification_scope_json : "[]");
    jash_jsonw_raw(w, ",\"operations\":");
    jash_jsonw_raw(w, mode->operations_json ? mode->operations_json : "[]");
    jash_jsonw_raw(w, ",\"semantic_tests\":");
    jash_jsonw_raw(w, mode->semantic_tests_json ? mode->semantic_tests_json : "[]");
    jash_jsonw_raw(w, ",\"options\":");
    jash_jsonw_raw(w, mode->options_json ? mode->options_json : "[]");
    jash_jsonw_raw(w, ",\"semantic_validation_required\":true,\"input_schema\":");
    mode_input_schema(w, mode);
    jash_jsonw_raw(w, ",\"result_schema\":"); schema_ref(w, mode->result_schema);
    jash_jsonw_raw(w, ",\"error_schema\":"); schema_ref(w, mode->error_schema);
    jash_jsonw_raw(w, ",\"error_policy\":"); jash_jsonw_string(w, mode->error_policy);
    jash_jsonw_raw(w, ",\"notes\":"); jash_jsonw_string(w, mode->notes);
    jash_jsonw_raw(w, "}");
}

static void structured_mode_json(jash_json_writer_t* w, const char* name,
                                 const jash_builtin_contract_t* contract, const char* mode) {
    jash_builtin_contract_output_t output = contract ?
        (!strcmp(mode, "json") ? contract->json_output : contract->data_output) : JASH_CONTRACT_OUTPUT_NONE;
    int complete = family_mode_complete(contract, mode);
    const char* status = output == JASH_CONTRACT_OUTPUT_NONE ? "missing" : complete ? "qualified" : "partial";
    if (!contract) status = "unclassified";
    else if (contract->role == JASH_CONTRACT_EXECUTION_ENVELOPE) status = "not-applicable";
    else if (contract->role == JASH_CONTRACT_INTERACTIVE) status = "deferred";
    jash_jsonw_raw(w, "{\"status\":"); jash_jsonw_string(w, status);
    jash_jsonw_raw(w, ",\"implementation\":"); jash_jsonw_string(w, output_name(output));
    jash_jsonw_raw(w, ",\"qualification\":"); jash_jsonw_string(w, complete ? "qualified" : "pending");
    jash_jsonw_raw(w, ",\"complete\":"); jash_jsonw_raw(w, complete ? "true" : "false");
    jash_jsonw_raw(w, ",\"variants_complete\":");
    jash_jsonw_raw(w, contract && contract->operations_complete ? "true" : "false");
    jash_jsonw_raw(w, ",\"variants\":[");
    int first = 1;
    size_t total = jash_builtin_mode_contract_count(name);
    for (size_t i = 0; i < total; i++) {
        const jash_builtin_mode_contract_t* entry = jash_builtin_mode_contract_at(name, i);
        if (strcmp(entry->mode, mode)) continue;
        if (!first) jash_jsonw_raw(w, ",");
        first = 0;
        mode_json(w, entry);
    }
    jash_jsonw_raw(w, "]}");
}

static void builtin_json(jash_json_writer_t* w, const jash_builtin_t* b) {
    const jash_builtin_contract_t* contract = jash_builtin_contract_find(b->name);
    jash_jsonw_raw(w, "{\"id\":"); jash_jsonw_string_pair(w, "builtin:", b->name);
    jash_jsonw_raw(w, ",\"name\":"); jash_jsonw_string(w, b->name);
    jash_jsonw_raw(w, ",\"kind\":\"builtin\",\"summary\":"); jash_jsonw_string(w, b->help);
    jash_jsonw_raw(w, ",\"availability\":\"compiled\",\"argv_native\":");
    jash_jsonw_raw(w, b->argv_fn ? "true" : "false");
    jash_jsonw_raw(w, ",\"declared_effect\":"); jash_jsonw_string(w, jash_builtin_effect(b->name));
    jash_jsonw_raw(w, ",\"contract_status\":");
    jash_jsonw_string(w, family_complete(contract) ? "qualified" : "pending");
    jash_jsonw_raw(w, ",\"complete\":"); jash_jsonw_raw(w, family_complete(contract) ? "true" : "false");
    jash_jsonw_raw(w, ",\"operations_complete\":");
    jash_jsonw_raw(w, contract && contract->operations_complete ? "true" : "false");
    jash_jsonw_raw(w, ",\"operation_exclusions\":");
    jash_jsonw_raw(w, contract && contract->operation_exclusions_json ? contract->operation_exclusions_json : "[]");
    jash_jsonw_raw(w, ",\"legacy_output_compatibility\":");
    jash_jsonw_raw(w, contract && contract->legacy_output_compatibility_json ? contract->legacy_output_compatibility_json : "[]");
    jash_jsonw_raw(w, ",\"options\":null,\"input_schema\":null,\"result_schema\":");
    if (!strcmp(b->name, "cp") || !strcmp(b->name, "mv") || !strcmp(b->name, "rm") ||
        !strcmp(b->name, "touch") || !strcmp(b->name, "mkdir") || !strcmp(b->name, "rmdir") ||
        !strcmp(b->name, "ln") || !strcmp(b->name, "chmod") || !strcmp(b->name, "chown") ||
        !strcmp(b->name, "sed"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:mutation:v1\",\"x-required-option\":\"--json\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "undo"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:undo-result:v1\",\"x-required-option\":\"--json\",\"x-required-selector\":\"--ref\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "df"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:query:df:v1\",\"x-required-option\":\"--json-report\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "timeline"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:timeline:v1\",\"x-required-option\":\"--json\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "gossip"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:gossip:v1\",\"x-required-option\":\"--channel\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "mirror"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:mirror:receipt:v1\",\"x-required-option\":\"--json\",\"x-required-selector\":\"--receipt\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "stat"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:query:stat:v1\",\"x-required-option\":\"--json-report\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "wc"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:query:wc:v1\",\"x-required-option\":\"--json-report\",\"x-option-scope\":\"prefix\"}");
    else if (!strcmp(b->name, "checkpoint"))
        jash_jsonw_raw(w, "{\"$ref\":\"urn:jash:checkpoints:v1\",\"x-required-option\":\"--json\",\"x-option-scope\":\"prefix\"}");
    else jash_jsonw_raw(w, "null");
    if (!strcmp(b->name, "undo"))
        jash_jsonw_raw(w, ",\"result_variants\":[{\"$ref\":\"urn:jash:recovery-store:v1\",\"selectors\":[\"--store\",\"--gc\",\"--pin\",\"--unpin\"]},{\"$ref\":\"urn:jash:recovery-quota:v1\",\"selectors\":[\"--quota\",\"--usage\"]}]");
    if (!strcmp(b->name, "df"))
        jash_jsonw_raw(w, ",\"conditional_effects\":[{\"option\":\"--baseline\",\"effect\":\"mutate\"}]");
    if (!strcmp(b->name, "stat"))
        jash_jsonw_raw(w, ",\"conditional_effects\":[{\"option\":\"--json\",\"effect\":\"read\"},{\"option\":\"--json-report\",\"effect\":\"read\"}]");
    if (!strcmp(b->name, "wc"))
        jash_jsonw_raw(w, ",\"conditional_effects\":[{\"option\":\"--json-report\",\"effect\":\"read\"}]");
    if (!strcmp(b->name, "find"))
        jash_jsonw_raw(w, ",\"conditional_effects\":[{\"option\":\"--json\",\"effect\":\"read\"}]");
    if (!strcmp(b->name, "trash"))
        jash_jsonw_raw(w, ",\"result_variants\":[{\"$ref\":\"urn:jash:trash-gc:v1\",\"selectors\":[\"empty --json\",\"gc --json\"]}]");
    char* cancel_args[] = {"--json"};
    if (jash_mutation_cancelable(b->name, 1, cancel_args))
        jash_jsonw_raw(w, ",\"native_cancellation\":{\"profile\":\"agent-strict\",\"mode\":\"cooperative\",\"required_first_argument\":\"--json\",\"timeout_ms\":false}");
    jash_jsonw_raw(w, ",\"structured_relevance\":"); jash_jsonw_string(w, relevance_name(contract));
    jash_jsonw_raw(w, ",\"alias_of\":");
    if (contract && contract->alias_of) jash_jsonw_string(w, contract->alias_of);
    else jash_jsonw_raw(w, "null");
    jash_jsonw_raw(w, ",\"implementation_source\":");
    if (contract) jash_jsonw_string(w, contract->source); else jash_jsonw_raw(w, "null");
    jash_jsonw_raw(w, ",\"structured_modes\":{\"json\":");
    structured_mode_json(w, b->name, contract, "json");
    jash_jsonw_raw(w, ",\"data\":"); structured_mode_json(w, b->name, contract, "data");
    jash_jsonw_raw(w, "}");
    jash_jsonw_raw(w, ",\"source\":\"c/builtins/registry.c\"}");
}

int jash_agent_contract_json(const char* selector, jash_json_sink_t sink, void* context) {
    if (selector && (!strcmp(selector, "invocation:exec-json") || !strcmp(selector, "invocation:serve") || !strcmp(selector, "invocation:listen")))
        return invocation_json(selector + 11, sink, context);
    jash_json_writer_t w;
    jash_jsonw_init(&w, sink, context);
    const jash_builtin_t* builtin = NULL;
    const jash_job_contract_t* job = NULL;
    int all = !selector;
    if (!all) {
        if (!strncmp(selector, "builtin:", 8)) {
            /* Enumerate the compiled table only, not plugin/PATH lookup. */
            for (int i = 0; i < jash_builtin_count(); i++) {
                const jash_builtin_t* b = jash_builtin_at((uint32_t)i);
                if (!strcmp(selector + 8, b->name)) { builtin = b; break; }
            }
        } else if (!strncmp(selector, "runtime:", 8)) job = jash_job_contract_find(selector + 8);
        if (!builtin && !job) {
            jash_jsonw_raw(&w, "{\"error\":{\"code\":\"unknown_operation\",\"operation\":");
            jash_jsonw_string(&w, selector);
            jash_jsonw_raw(&w, "},\"executed\":false}\n");
            return jash_jsonw_finish(&w) == 0 ? 2 : 1;
        }
    }
    jash_jsonw_raw(&w, "{\"schema\":\"jash.agent-contract.inventory.v1\",\"version\":");
    jash_jsonw_u64(&w, JASH_AGENT_CONTRACT_VERSION);
    jash_jsonw_raw(&w, ",\"profile\":");
#ifdef JASH_AGENT_ONLY
    jash_jsonw_string(&w, "agent-strict");
#else
    jash_jsonw_string(&w, "general");
#endif
    jash_jsonw_raw(&w, ",\"target\":"); jash_jsonw_string(&w, bear_osinfo_target());
    jash_jsonw_raw(&w, ",\"inventory_scope\":\"compiled-builtins-and-job-operations\","
                       "\"complete\":false,\"coverage\":{\"compiled_builtins\":");
    jash_jsonw_u64(&w, (uint64_t)jash_builtin_count());
    jash_jsonw_raw(&w, ",\"runtime_operations\":");
    jash_jsonw_u64(&w, jash_job_contract_count());
    size_t classified = 0, structured = 0, envelope = 0, interactive = 0, qualified = 0;
    for (int i = 0; i < jash_builtin_count(); i++) {
        const jash_builtin_contract_t* row = jash_builtin_contract_find(jash_builtin_at((uint32_t)i)->name);
        if (!row) continue;
        classified++;
        if (family_complete(row)) qualified++;
        if (row->role == JASH_CONTRACT_STRUCTURED) structured++;
        else if (row->role == JASH_CONTRACT_EXECUTION_ENVELOPE) envelope++;
        else if (row->role == JASH_CONTRACT_INTERACTIVE) interactive++;
    }
    jash_jsonw_raw(&w, ",\"qualified_complete\":"); jash_jsonw_u64(&w, qualified);
    jash_jsonw_raw(&w, ",\"denominator_frozen\":");
    jash_jsonw_raw(&w, JASH_BUILTIN_DENOMINATOR_FROZEN ? "true" : "false");
    jash_jsonw_raw(&w, ",\"classified_builtin_names\":");
    jash_jsonw_u64(&w, classified);
    jash_jsonw_raw(&w, ",\"builtin_name_catalog_complete\":");
    jash_jsonw_raw(&w, classified == (size_t)jash_builtin_count() ? "true" : "false");
    jash_jsonw_raw(&w, ",\"structured_candidates\":"); jash_jsonw_u64(&w, structured);
    jash_jsonw_raw(&w, ",\"execution_envelope_names\":"); jash_jsonw_u64(&w, envelope);
    jash_jsonw_raw(&w, ",\"interactive_names\":"); jash_jsonw_u64(&w, interactive);
    jash_jsonw_raw(&w, "},\"operations\":[");
    if (builtin) builtin_json(&w, builtin);
    else if (job) job_json(&w, job);
    else {
        for (int i = 0; i < jash_builtin_count(); i++) {
            if (i) jash_jsonw_raw(&w, ",");
            builtin_json(&w, jash_builtin_at((uint32_t)i));
        }
        for (size_t i = 0; i < jash_job_contract_count(); i++) {
            if (i || jash_builtin_count()) jash_jsonw_raw(&w, ",");
            job_json(&w, &JOBS[i]);
        }
    }
    jash_jsonw_raw(&w, "]}\n");
    return jash_jsonw_finish(&w) == 0 ? 0 : 1;
}
