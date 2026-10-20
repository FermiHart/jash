// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_GAP_SNAPSHOT_H
#define JASH_BUILTINS_GAP_SNAPSHOT_H
#include "semantic_value.h"
/* Bounded checked read of the actual local gap TSV. No creation, attestation
 * or output. Missing is complete/empty; unreadable sources have null counts.
 * Result owns its fields in values->arena; return is the observation status. */
int jash_gap_observe(semantic_value_t* values, jdv_value_t* result);
/* Same checked local observation, exposing actual complete rows to native
 * consumers. On source failure rows is an empty list and source.read_complete
 * is false; do not treat that as an observed empty store. No fleet read or
 * proof execution. Every returned field is owned by values->arena. */
int jash_gap_observe_rows(semantic_value_t* values, jdv_value_t* source,
                         jdv_value_t* rows);
#endif
