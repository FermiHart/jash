/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* osif/singuos/builtins.h — SinguOS-target builtin prototypes.
 *
 * These four builtins are SinguOS-target features (they talk to the SinguOS
 * kernel: Genesis pages, Conduit fork/cage, IRQ surface, ring-3 boundaries).
 * The ports's sources live next to this header; J2 wires them into
 * c/builtins/registry.c behind JASH_TARGET_SINGUOS (see AGENT.md here).
 *
 * Signatures mirror the copy's registry shapes: string-args builtins and the
 * argv-form cage must keep these exact prototypes to keep registry tables
 * byte-compatible.
 */
#ifndef SINGUOS_BUILTINS_H
#define SINGUOS_BUILTINS_H

#include "../osif.h"
#include "builtins/builtin.h"

int jash_b_irq(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_genesis(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_ring3_selftest(jash_state_t* s, jash_writer_t w, void* ctx, const char* args);
int jash_b_cage(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv);

#endif
