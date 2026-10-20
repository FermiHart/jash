// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_CORE_AGENT_CANCEL_H
#define JASH_CORE_AGENT_CANCEL_H
#include "state.h"

/* Poll a bounded burst on the startup-pinned control socket. True means a
 * canonical frame matched the active request, not that execution has stopped. */
int jash_agent_cancel_poll(jash_state_t* state);
#endif
