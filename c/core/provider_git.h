// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/provider_git — git context provider.
//
// Provides branch, status, and diff context items.
// See Docs/ADR-0035-context-engine-stances-cargo.md

#ifndef JASH_PROVIDER_GIT_H
#define JASH_PROVIDER_GIT_H

#include "context_engine.h"

context_provider_t* provider_git_get(void);

#endif
