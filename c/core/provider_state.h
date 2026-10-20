// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/provider_state — shell state context provider.
//
// Wraps jash_state_t fields (cwd, last_status, shell_opts, env) into
// context items for the context engine. See Docs/ADR-0035-context-engine-stances-cargo.md

#ifndef JASH_PROVIDER_STATE_H
#define JASH_PROVIDER_STATE_H

#include "context_engine.h"
#include "state.h"

context_provider_t* provider_state_get(jash_state_t* jash_state);

#endif
