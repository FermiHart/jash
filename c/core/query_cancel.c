// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "query_cancel.h"
#include "agent_cancel.h"
#include <string.h>

static int (*query_control_poll)(void*);
static void* query_control_context;
static int query_cancel_status;

void jash_query_cancel_control(int (*poll)(void*), void* context) {
    query_control_poll = poll;
    query_control_context = context;
    query_cancel_status = 0;
}

int jash_query_cancel_poll(jash_state_t* state) {
    if (query_cancel_status) return query_cancel_status;
    int status = query_control_poll ? query_control_poll(query_control_context) :
        jash_agent_cancel_poll(state) ? 130 : 0;
    if (status <= 0) return 0;
    query_cancel_status = status == 1 ? 130 : status;
    if (state) state->agent_request_cancelled = 1;
    return query_cancel_status;
}

int jash_query_cancelable(const char* operation, int argc, char* const* argv) {
    if (!operation || argc < 1 || !argv || !argv[0]) return 0;
    int native = !strcmp(argv[0], "--data") || !strcmp(argv[0], "--data-v2");
    int json = !strcmp(argv[0], "--json");
    /* mem distill polls before atomic publication and while waiting for its
     * writer lock; its receipt retains any observed publication phases. */
    if (!strcmp(operation, "mem") || !strcmp(operation, "graph")) return native || json;
    if (!strcmp(operation, "context")) return (native || json) && argc > 1 && argv[1] &&
        (!strcmp(argv[1], "map") || !strcmp(argv[1], "fold") || !strcmp(argv[1], "ask"));
    if (!strcmp(operation, "osif")) return (native || json) && argc > 1 && argv[1] &&
        !strcmp(argv[1], "matrix");
    if (!strcmp(operation, "mesh")) return (native || json) && argc > 1 && argv[1] &&
        (!strcmp(argv[1], "workers") || !strcmp(argv[1], "who") ||
         !strcmp(argv[1], "board") || !strcmp(argv[1], "rep") || !strcmp(argv[1], "claims"));
    if (!strcmp(operation, "proc")) return (native || json) && argc > 1 && argv[1] &&
        (!strcmp(argv[1], "tree") || !strcmp(argv[1], "diff"));
    if (!strcmp(operation, "glob") || !strcmp(operation, "find") ||
        !strcmp(operation, "du") || !strcmp(operation, "grep") || !strcmp(operation, "diff") ||
        !strcmp(operation, "tread") || !strcmp(operation, "tdiff") ||
        !strcmp(operation, "verify") || !strcmp(operation, "awk") ||
        !strcmp(operation, "ps") || !strcmp(operation, "lint")) return native || json;
    if (!strcmp(operation, "cat")) return native;
    if (!strcmp(operation, "df") || !strcmp(operation, "stat") || !strcmp(operation, "wc")) return native;
    return 0;
}
