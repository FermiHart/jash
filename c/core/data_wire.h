// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_DATA_WIRE_H
#define JASH_CORE_DATA_WIRE_H

#include "data_stream.h"

/* Explicit byte adapter for one native linear sequence. Wire format JTV1:
 * magic "JTV1"; then frames tag:u8, size:u64 big-endian, payload:size.
 * JDS1 is reserved for the unrelated agent job-stream seqpacket protocol.
 * D=declared JSC1, G=guaranteed JSC1, I=one canonical JDV1 item,
 * F=final JSC1 (only when present), T=one mandatory terminal. T payload:
 * state, completeness, truncated, hash_present, hash_algorithm, three reserved
 * zero bytes; items, logical bytes, errors, producer, transform, transport
 * status as eight-byte big-endian words; 32 hash bytes; reason length as one
 * eight-byte word followed by exact UTF-8 reason bytes. Signed statuses use
 * two's-complement modulo 2^64. EOF before T is incomplete, never success.
 * PENDING without readiness fails EAGAIN; no implicit busy-polling. The caller
 * owns the workspace and releases/cancels the cursor even on sink failure. */
int jdv_stream_write_frames(jdv_stream_cursor_t* cursor, jdv_sink_t sink,
                            void* sink_context, void* workspace,
                            size_t workspace_bytes, jdv_stream_end_t* final);

/* A read-only observer runs once per borrowed item, before its JTV1 frame.
 * It must neither retain nor mutate the item; failure aborts the adapter. */
typedef int (*jdv_item_observer_t)(void* context, const jdv_value_t* item);
int jdv_stream_write_frames_observed(jdv_stream_cursor_t* cursor,
                                     jdv_sink_t sink, void* sink_context,
                                     void* workspace, size_t workspace_bytes,
                                     jdv_item_observer_t observer,
                                     void* observer_context,
                                     jdv_stream_end_t* final);

/* JTV2: distinct magic, each I payload begins with one big-endian u64 logical
 * credit followed by canonical JDV1. The terminal hash uses JTV2-ITEMS-v1.
 * This is the only wire revision admitted for lossless D05 item accounting. */
int jdv_stream_write_frames_v2(jdv_stream_cursor_t* cursor, jdv_sink_t sink,
                               void* sink_context, void* workspace,
                               size_t workspace_bytes, jdv_stream_end_t* final);

/* Checked incremental JTV2 emission for synchronous producers whose action
 * boundaries must observe delivery (e.g. mutation admission and per-item
 * cancellation). Uses the same canonical codecs, frame functions and linear
 * cursor validation as the pull adapter. open flushes D/G before returning.
 *
 * The opaque handle owns one cursor. Schema/sink/workspace storage is borrowed
 * until close and must not overlap. Each item is fully consumed before item
 * returns, so its caller-owned arena may then be reset. Logical credits are
 * explicit; physical/materialized accounting covers encoded item frame/payload
 * bytes. close always releases/cancels the cursor and frees the handle.
 *
 * end preserves the supplied outcome's state/status/errors/reason/completeness,
 * deriving count, logical total and JTV2 hash. COMPLETE uses the guaranteed
 * schema. A supplied final_schema must be NULL or that same guaranteed_schema:
 * this streaming API cannot prove a new narrower schema after releasing items.
 * final is untouched on failure. Success means the terminal was delivered,
 * including when its producer state is FAILED/INCOMPLETE/CANCELLED. A delivery
 * failure can leave a prefix; callers must not mistake it for completed work.
 * No frames may follow end, and errors are sticky. */
typedef struct jdv_wire_push jdv_wire_push_t;
/* Explicit legacy/versioned admission over the same checked incremental
 * writer. version must be 1 or 2. JTV1 emits canonical item bytes without
 * per-item credit; its terminal total is producer-reported, not independently
 * verifiable. The version controls magic, item framing and ordered hash domain.
 * item/end/close accept either version opened here. The v2 entry points below
 * remain compatible wrappers; their open always selects JTV2. */
int jdv_wire_push_open_version(unsigned version, jdv_wire_push_t** out,
                          const jdv_stream_config_t* config,
                          jdv_sink_t sink, void* context,
                          void* workspace, size_t workspace_bytes);
int jdv_wire_push_item(jdv_wire_push_t*, const jdv_value_t*, uint64_t logical_credit);
int jdv_wire_push_end(jdv_wire_push_t*, const jdv_stream_end_t* outcome,
                         jdv_stream_end_t* final);
void jdv_wire_push_close(jdv_wire_push_t*);

int jdv_wire_push_v2_open(jdv_wire_push_t** out,
                          const jdv_stream_config_t* config,
                          jdv_sink_t sink, void* context,
                          void* workspace, size_t workspace_bytes);
int jdv_wire_push_v2_item(jdv_wire_push_t*, const jdv_value_t*, uint64_t logical_credit);
int jdv_wire_push_v2_end(jdv_wire_push_t*, const jdv_stream_end_t* outcome,
                         jdv_stream_end_t* final);
void jdv_wire_push_v2_close(jdv_wire_push_t*);

/* Strict, bounded JTV1 verifier for an immutable caller-owned byte buffer.
 * It authenticates framing, canonical JDV1/JSC1, item/schema relations,
 * emitted count and the terminal's ordered content hash. Producer-reported
 * logical bytes/errors/status are not independently derived from raw records.
 * arena is scratch only: used and out remain unchanged on failure; on success
 * arena.used returns to its input mark and out contains no borrowed pointers.
 * The input and arena storage must not overlap or be mutated during the call. */
typedef struct {
    uint64_t max_wire_bytes;
    size_t max_frame_bytes;
    uint64_t max_items;
    jdv_limits_t value_limits;
} jdv_wire_limits_t;

typedef struct {
    jdv_stream_state_t state;
    jdv_completeness_t completeness;
    uint64_t items_seen;
    uint64_t bytes_seen;
    uint64_t errors_seen;
    int truncated;
    int final_hash_present;
    int logical_verified; /* JTV2 sum checked; JTV1 terminal claim only. */
    uint8_t final_hash[JDV_HASH_SIZE];
} jdv_wire_info_t;

void jdv_wire_limits_init(jdv_wire_limits_t*);
int jdv_wire_validate(const void* data, size_t size, jdv_arena_t* arena,
                       const jdv_wire_limits_t* limits, jdv_wire_info_t* out);
int jdv_wire_validate_v2(const void* data, size_t size, jdv_arena_t* arena,
                          const jdv_wire_limits_t* limits, jdv_wire_info_t* out);

#endif
