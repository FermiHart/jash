// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/provider_digest — session digest context provider.
//
// Wraps digest_session_run() output into a context item.
// See Docs/ADR-0035-context-engine-stances-cargo.md

#ifndef JASH_PROVIDER_DIGEST_H
#define JASH_PROVIDER_DIGEST_H

#include "context_engine.h"

context_provider_t* provider_digest_get(void);

#endif
