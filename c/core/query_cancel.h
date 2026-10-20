// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_QUERY_CANCEL_H
#define JASH_CORE_QUERY_CANCEL_H
#include "state.h"

/* Conservative admission: only modes with native cooperative boundaries and
 * no external fallback qualify. Plain text commands retain their old policy. */
int jash_query_cancelable(const char* operation, int argc, char* const* argv);
/* Synchronous request scope. Callback returns 0, a signal status (128+signal),
 * or legacy boolean 1 (request cancellation, translated to 130). Clear after
 * execution; registration/clear resets the latched cancellation status. */
void jash_query_cancel_control(int (*poll)(void*), void* context);
int jash_query_cancel_poll(jash_state_t* state);
#endif
