// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Allocation-free, linear cursors for bounded semantic streams.
#ifndef JASH_CORE_DATA_STREAM_H
#define JASH_CORE_DATA_STREAM_H

#include <core/data_value.h>

#include <stddef.h>
#include <stdint.h>

#define JDV_STREAM_DESCRIPTOR_VERSION 1u
#define JDV_STREAM_CONFIG_VERSION 1u
#define JDV_STREAM_VTABLE_VERSION 1u
#define JDV_STREAM_CURSOR_VERSION 1u
#define JDV_STREAM_LIMITS_VERSION 1u
#define JDV_STREAM_ACCOUNTING_VERSION 1u
#define JDV_STREAM_RECORD_VERSION 1u
#define JDV_STREAM_SCHEMA_LIFECYCLE_VERSION 1u

typedef struct jdv_stream_descriptor jdv_stream_descriptor_t;
typedef struct jdv_stream_cursor jdv_stream_cursor_t;

typedef enum {
    JDV_STREAM_BOUNDED_UNKNOWN = 0,
    JDV_STREAM_FINITE = 1,
    JDV_STREAM_UNBOUNDED = 2
} jdv_stream_boundedness_t;

typedef enum {
    JDV_STREAM_ORDERED = 0,
    JDV_STREAM_UNORDERED = 1,
    JDV_STREAM_PARTIALLY_ORDERED = 2
} jdv_stream_ordering_t;

typedef enum {
    JDV_PULL_ITEM = 0,
    JDV_PULL_PENDING = 1,
    JDV_PULL_END = 2,
    JDV_PULL_ERROR = 3
} jdv_stream_pull_result_t;

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    uint64_t max_items;
    uint64_t max_logical_bytes;
    uint64_t max_physical_bytes;
    uint64_t max_materialized_bytes;
} jdv_stream_limits_t;

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    uint64_t items;
    uint64_t logical_bytes;
    uint64_t physical_bytes;
    uint64_t materialized_bytes;
} jdv_stream_accounting_t;

/* The schema set remains the canonical schema contract. The additional fields
 * describe stream progress without manufacturing an inferred schema: a true
 * observed_prefix_valid means every observed item was accepted with a YES
 * relation by the schema stored as set.observed_schema. terminal is set for
 * complete, failed, incomplete, and cancelled streams, even when no honest
 * final_schema exists. */
typedef struct {
    uint32_t version;
    uint32_t struct_size;
    jdv_schema_set_t set;
    uint64_t observed_items;
    int observed_prefix_valid;
    int terminal;
    jdv_stream_state_t terminal_state;
} jdv_stream_schema_lifecycle_t;

/* A producer fills item/terminal and the three byte deltas. The runtime fills
 * borrow_owner/borrow_generation for an ITEM returned to the consumer. The
 * record passed to pull is runtime-owned and valid only for that callback.
 * An item and everything reachable from it remain borrowed only until the next
 * pull, cancel, or release on that cursor. A producer terminal structure need
 * only survive its pull callback, but its reason and final_schema pointees have
 * the longer lifetime documented on jdv_stream_pull below. */
typedef struct {
    uint32_t version;
    uint32_t struct_size;
    const jdv_value_t* item;
    const jdv_stream_end_t* terminal;
    uint64_t logical_bytes;
    uint64_t physical_bytes;
    uint64_t materialized_bytes;
    const jdv_stream_cursor_t* borrow_owner;
    uint64_t borrow_generation;
} jdv_stream_record_t;

typedef jdv_stream_pull_result_t (*jdv_stream_pull_fn)(
    void* context, jdv_stream_record_t* record);
/* pull must not block waiting for readiness. PENDING requires a record with no
 * item, terminal, or accounting deltas. The caller must use producer-specific
 * readiness or retry policy before trying again; this API has no event handle
 * and an immediate retry may busy-spin. Cancellation is same-thread and may
 * only occur between completed nonblocking pull calls. cancel returns a
 * producer-domain outcome for the cancellation terminal (zero means accepted);
 * it is semantic status, not errno. */
typedef int (*jdv_stream_cancel_fn)(void* context, jdv_slice_t reason);
typedef void (*jdv_stream_release_fn)(void* context);

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    jdv_stream_pull_fn pull;
    jdv_stream_cancel_fn cancel;
    jdv_stream_release_fn release;
} jdv_stream_producer_vtable_t;

typedef struct {
    uint32_t version;
    uint32_t struct_size;
    jdv_cardinality_t cardinality;
    jdv_stream_boundedness_t boundedness;
    jdv_stream_ordering_t ordering;
    uint64_t provenance_id;
    const jdv_schema_t* declared_schema;
    const jdv_schema_t* guaranteed_schema;
    jdv_limits_t value_limits;
    jdv_stream_limits_t stream_limits;
} jdv_stream_config_t;

/* These structures are complete so callers can own their storage. Descriptor
 * authority owns the epoch, state, counters, schemas, callbacks, and immutable
 * copies of both limit sets. The public descriptor fields and cursor accounting
 * and terminal fields are inspectable snapshots only; mutating them cannot
 * alter enforcement. Fields with an _authority_ or _runtime_ prefix are private
 * and must not be accessed, copied for later restoration, or relocated. */
struct jdv_stream_descriptor {
    uint32_t version;
    uint32_t struct_size;
    jdv_cardinality_t cardinality;
    jdv_ownership_t ownership;
    jdv_stream_boundedness_t boundedness;
    jdv_stream_ordering_t ordering;
    uint64_t provenance_id;
    jdv_schema_set_t schemas;
    jdv_limits_t value_limits;
    jdv_stream_limits_t stream_limits;
    const jdv_stream_producer_vtable_t* producer;
    void* producer_context;
    const jdv_stream_descriptor_t* _runtime_self;
    jdv_stream_cursor_t* _runtime_cursor;
    uint64_t _runtime_generation;
    int _runtime_state;
    jdv_cardinality_t _authority_cardinality;
    jdv_ownership_t _authority_ownership;
    jdv_stream_boundedness_t _authority_boundedness;
    jdv_stream_ordering_t _authority_ordering;
    uint64_t _authority_provenance_id;
    jdv_stream_schema_lifecycle_t _authority_schema;
    jdv_limits_t _authority_value_limits;
    jdv_stream_limits_t _authority_stream_limits;
    jdv_stream_accounting_t _authority_accounting;
    jdv_stream_end_t _authority_terminal;
    const jdv_stream_producer_vtable_t* _authority_producer;
    void* _authority_context;
    uint64_t _authority_epoch;
    uint64_t _authority_borrow_generation;
    int _authority_state;
    int _authority_busy;
    int _authority_borrow_active;
    int _authority_relation_runtime;
    int _authority_cancel_called;
    int _authority_release_called;
};

struct jdv_stream_cursor {
    uint32_t version;
    uint32_t struct_size;
    jdv_stream_descriptor_t* descriptor;
    jdv_stream_accounting_t accounting;
    jdv_stream_end_t terminal;
    const jdv_stream_cursor_t* _runtime_self;
    uint64_t _runtime_generation;
    uint64_t _runtime_borrow_generation;
    int _runtime_state;
    int _runtime_busy;
    int _runtime_borrow_active;
    int _runtime_terminal_observed;
};

void jdv_stream_limits_init(jdv_stream_limits_t* limits);
void jdv_stream_config_init(jdv_stream_config_t* config);

int jdv_stream_descriptor_init(jdv_stream_descriptor_t* descriptor,
                               const jdv_stream_config_t* config,
                               const jdv_stream_producer_vtable_t* producer,
                               void* producer_context);
int jdv_stream_open(jdv_stream_descriptor_t* descriptor,
                    jdv_stream_cursor_t* cursor);

jdv_stream_pull_result_t jdv_stream_pull(jdv_stream_cursor_t* cursor,
                                         jdv_stream_record_t* record);
/* descriptor and cursor storage are caller-owned and must remain at their
 * original addresses through release. Item pointees and everything reachable
 * from them are borrowed through the next pull/cancel/release. Config schema
 * pointees are borrowed for the descriptor lifetime and must outlive all item
 * validation and schema/terminal queries. The vtable and producer context are
 * borrowed through the one release callback and are severed immediately after
 * it returns. A producer END/ERROR terminal structure is copied during pull;
 * its reason.data and final_schema pointees, and an explicit cancellation
 * reason.data pointee, remain borrowed and must outlive every later terminal
 * query. Returned terminal/schema/lifecycle structure pointers refer to the
 * descriptor's caller-owned storage and remain stable until that storage is
 * reinitialized or discarded. accounting is returned by value. */
int jdv_stream_cancel(jdv_stream_cursor_t* cursor, jdv_slice_t reason,
                      const jdv_stream_end_t** terminal);
/* Idempotent: every call after the first successful release also returns zero;
 * producer cancel/release callbacks still run at most once. */
int jdv_stream_release(jdv_stream_cursor_t* cursor);

int jdv_stream_terminal(const jdv_stream_cursor_t* cursor,
                        const jdv_stream_end_t** terminal);
int jdv_stream_accounting(const jdv_stream_cursor_t* cursor,
                          jdv_stream_accounting_t* accounting);
int jdv_stream_schemas(const jdv_stream_cursor_t* cursor,
                       const jdv_schema_set_t** schemas);
int jdv_stream_schema_lifecycle(
    const jdv_stream_cursor_t* cursor,
    const jdv_stream_schema_lifecycle_t** lifecycle);

/* Returns one only while record's borrowed item is still valid. */
int jdv_stream_item_valid(const jdv_stream_cursor_t* cursor,
                          const jdv_stream_record_t* record);

#endif
