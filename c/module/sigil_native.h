// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_MODULE_SIGIL_NATIVE_H
#define JASH_MODULE_SIGIL_NATIVE_H
#include <builtins/semantic_builtin.h>

/* Optional symbol of the Sigil module. It extends dispatch without changing
 * jmod_ops_t or the existing module ABI. The host admits a bounded output
 * session before module loading (initialization can create local key state).
 * The provider uses actual engine values and finishes that same session.
 * argv contains only literal operands, with the transport selector removed.
 * The loaded module must have been built with this host's native v1 headers. */
#define JASH_SIGIL_NATIVE_SYMBOL "jash_sigil_native_v1"
typedef int (*jash_sigil_native_fn)(semantic_builtin_t*, jash_state_t*, int, char**);
int jash_sigil_native_v1(semantic_builtin_t*, jash_state_t*, int, char**);
#endif
