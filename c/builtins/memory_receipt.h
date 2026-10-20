// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_BUILTINS_MEMORY_RECEIPT_H
#define JASH_BUILTINS_MEMORY_RECEIPT_H
#include "semantic_builtin.h"

/* The old value can change before a native writer obtains its lock. Reserve
 * the documented worst-case receipt, including an error that retains that
 * old value, before even creating the lock/directory. This is an admission
 * reserve, not a claim about the eventual number of emitted bytes. */
static inline int memory_receipt_admit(semantic_builtin_t* call, size_t reserve) {
    if (reserve > SEMANTIC_OUTPUT_BYTES) return semantic_error(call, "receipt_resource_limit", 125);
    long budget = call->mode == 1 ? jash_budget_limit() : 0;
    if (budget > 0 && reserve > (size_t)budget)
        return semantic_error(call, "receipt_budget_insufficient", 75);
    return semantic_admit(call);
}
#endif
