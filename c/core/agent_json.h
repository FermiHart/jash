// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/agent_json — machine-readable result envelope (F3, ADR-0022).
//
// When JASH_AGENT=1, jash -c emits a final JSON line on stderr aggregating
// everything an agent needs to parse a command result without scraping text:
// rc, ms, tl, cwd, and the file mutations recorded in the timeline.

#ifndef JASH_CORE_AGENT_JSON_H
#define JASH_CORE_AGENT_JSON_H

#include <core/state.h>
#include <core/fschange.h>
#include <core/world.h>
#include <core/json_writer.h>

/* JASH_AGENT set and not "0"/empty. */
int jash_agent_enabled(void);

/* Legacy bounded escaper. Cannot report truncation; new code must use
 * json_writer.h's checked streaming APIs. A zero-capacity output is untouched. */
void jash_json_esc(const char* s, char* out, size_t cap);

/* Emit one JSON object line to stderr. `ms` is wall time, `tl` the timeline
 * count already computed by the caller. Pulls mutations from the timeline;
 * `fsd` (optional, ADR-0038 W2) adds the OBSERVED filesystem delta — what
 * externals did, which the timeline can never see.
 * `world_delta` (optional, ADR-0042 F2) adds the cross-invocation world diff.
 * Return 0 after successful output, -1 on delivery failure. */
int jash_agent_emit(jash_state_t* s, int rc, double ms, int tl,
                     const jash_fsdiff_t* fsd,
                      const jash_world_delta_t* world_delta);
/* Same envelope to a caller sink, useful for adapters and fault-injection gates.
 * Returns 0 on successful write, -1 otherwise; failed output does not consume
 * absorb-spool data or mark background completion notifications delivered. */
int jash_agent_emit_to(jash_state_t* s, int rc, double ms, int tl,
                       const jash_fsdiff_t* fsd, const jash_world_delta_t* world_delta,
                       jash_json_sink_t sink, void* context);

/* ADR-0040 W2/W3: absorb-spool ownership. main.c marks the invocation that
 * CREATED the spool file; only the owner may fold+unlink it at emit time.
 * Inheritors (nested jash with JASH_ABSORB_SPOOL in environ) must never
 * touch it, or they silently steal the parent's absorbed records. */
void jash_agent_spool_mark_owner(void);
int  jash_agent_spool_is_owner(void);

/* ADR-0040 W4: absorbed counts folded by the LAST jash_agent_emit() in this
 * process (0/0 before emit or for non-owners). The spool is unlinked at fold
 * time only after successful delivery; the epilogue telemetry reads the stash. */
void jash_agent_last_absorbed(int* count, int* failed);

#endif /* JASH_CORE_AGENT_JSON_H */
