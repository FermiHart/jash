// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_DATA_EMIT_H
#define JASH_CORE_DATA_EMIT_H

#include "data_stream.h"

/* Emit an already collected semantic sequence as JTV2. This is an explicit
 * byte adapter, not an implicit typed shell pipe or a zero-copy transport.
 *
 * items is a contiguous, caller-owned array; every reachable value, schema,
 * reason and optional per-item logical credit must remain immutable throughout
 * this synchronous call. NULL logical_credits assigns zero, never an inferred
 * byte count. The caller accounts for collection/materialization before entry.
 * config must be initialized, finite, and describe a JDV_DATA_SEQUENCE.
 *
 * outcome must be initialized and explicitly state the producer's actual
 * state/completeness/errors/status/reason/truncation. These fields are preserved.
 * The helper computes items_seen, bytes_seen and the ordered prefix hash. For a
 * COMPLETE outcome with no final_schema, guaranteed_schema is used. No other
 * outcome is promoted to COMPLETE, even if all supplied items are valid.
 *
 * A complete dry run through the existing cursor/JTV2 writer validates values,
 * schema relations, limits, terminal and workspace capacity before the first
 * real sink byte. Delivery can still fail after a prefix (e.g. EPIPE); a partial
 * frame sequence is never success. The cursor is always released. final is
 * unchanged on failure; on success it is a borrowed snapshot (its schema and
 * reason pointees retain the caller lifetimes above).
 *
 * Return 0 means all frames, including the terminal, were delivered. A FAILED,
 * INCOMPLETE or CANCELLED producer outcome can be successfully delivered: the
 * caller must separately retain command status and inspect final when needed.
 * Workspace is caller-owned writable scratch and must not overlap inputs,
 * final, or sink-owned storage. It must fit the largest schema/item/terminal
 * frame; item payloads include an additional eight-byte logical credit. */
int jdv_emit_sequence(const jdv_value_t* items, size_t count,
                      const uint64_t* logical_credits,
                      const jdv_stream_config_t* config,
                      const jdv_stream_end_t* outcome,
                      jdv_sink_t sink, void* context,
                      void* workspace, size_t workspace_bytes,
                      jdv_stream_end_t* final);

/* Same checked collected-sequence contract with an explicit legacy adapter
 * version (1=JTV1, 2=JTV2). JTV1 retains terminal logical accounting but does
 * not encode per-item credit; its validator cannot independently verify that
 * accounting. No version is inferred from values or a destination. */
int jdv_emit_sequence_version(unsigned version,
                      const jdv_value_t* items, size_t count,
                      const uint64_t* logical_credits,
                      const jdv_stream_config_t* config,
                      const jdv_stream_end_t* outcome,
                      jdv_sink_t sink, void* context,
                      void* workspace, size_t workspace_bytes,
                      jdv_stream_end_t* final);

#endif
