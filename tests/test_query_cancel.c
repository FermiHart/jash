// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/query_cancel.h>
#include <core/agent_cancel.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "check failed at %s:%d: %s\n", __FILE__, __LINE__, #condition); \
    exit(EXIT_FAILURE); } } while (0)

static int direct_cancel, callback_calls;
int jash_agent_cancel_poll(jash_state_t* state) { (void)state; return direct_cancel; }
static int poll_status(void* context) { callback_calls++; return *(int*)context; }

int main(void) {
    char* typed[] = {"--data-v2", "."};
    char* legacy[] = {"--json", "."};
    char* literal[] = {"--", "--data-v2"};
    char* text[] = {"."};
    const char* native[] = {"glob", "find", "cat", "df", "stat", "wc", "du", "grep", "diff", "tread", "tdiff", "verify", "awk", "ps"};
    for (size_t i = 0; i < sizeof(native) / sizeof(native[0]); i++) {
        CHECK(jash_query_cancelable(native[i], 2, typed));
        CHECK(!jash_query_cancelable(native[i], 2, literal));
        CHECK(!jash_query_cancelable(native[i], 1, text));
    }
    CHECK(jash_query_cancelable("find", 2, legacy));
    CHECK(jash_query_cancelable("verify", 2, legacy));
    CHECK(jash_query_cancelable("awk", 2, legacy));
    CHECK(jash_query_cancelable("ps", 2, legacy));
    CHECK(jash_query_cancelable("lint", 2, legacy));
    CHECK(jash_query_cancelable("lint", 2, typed));
    CHECK(!jash_query_cancelable("lint", 2, literal));
    char* memory[] = {"--data-v2", "distill"};
    CHECK(jash_query_cancelable("mem", 2, memory));
    memory[0] = "--json"; memory[1] = "brief";
    CHECK(jash_query_cancelable("mem", 2, memory));
    memory[0] = "brief"; memory[1] = "--json";
    CHECK(!jash_query_cancelable("mem", 2, memory));
    CHECK(!jash_query_cancelable("mem", 2, literal));
    char* context[] = {"--data-v2", "map"};
    CHECK(jash_query_cancelable("context", 2, context));
    context[1] = "fold"; CHECK(jash_query_cancelable("context", 2, context));
    context[1] = "ask"; CHECK(jash_query_cancelable("context", 2, context));
    context[1] = "bundle"; CHECK(!jash_query_cancelable("context", 2, context));
    CHECK(!jash_query_cancelable("context", 2, literal));
    char* osif[] = {"--data-v2", "matrix"};
    CHECK(jash_query_cancelable("osif", 2, osif));
    osif[0] = "--json"; CHECK(jash_query_cancelable("osif", 2, osif));
    osif[1] = "report"; CHECK(!jash_query_cancelable("osif", 2, osif));
    osif[1] = "check"; CHECK(!jash_query_cancelable("osif", 2, osif));
    osif[1] = "port"; CHECK(!jash_query_cancelable("osif", 2, osif));
    osif[0] = "matrix"; osif[1] = "--data-v2";
    CHECK(!jash_query_cancelable("osif", 2, osif));
    CHECK(jash_query_cancelable("graph", 2, typed));
    CHECK(!jash_query_cancelable("graph", 2, literal));
    char* mesh[] = {"--data-v2", "workers"};
    CHECK(jash_query_cancelable("mesh", 2, mesh));
    mesh[1] = "claims";
    CHECK(jash_query_cancelable("mesh", 2, mesh));
    mesh[1] = "next";
    CHECK(!jash_query_cancelable("mesh", 2, mesh));
    mesh[1] = "done";
    CHECK(!jash_query_cancelable("mesh", 2, mesh));
    mesh[0] = "--"; mesh[1] = "workers";
    CHECK(!jash_query_cancelable("mesh", 2, mesh));
    char* process[] = {"--data-v2", "tree", "1"};
    CHECK(jash_query_cancelable("proc", 3, process));
    process[1] = "diff";
    CHECK(jash_query_cancelable("proc", 3, process));
    process[1] = "restore";
    CHECK(!jash_query_cancelable("proc", 3, process));
    process[1] = "snapshot";
    CHECK(!jash_query_cancelable("proc", 3, process));
    CHECK(!jash_query_cancelable("cat", 2, legacy));
    CHECK(!jash_query_cancelable("stat", 2, legacy));
    CHECK(!jash_query_cancelable("exec", 2, typed));
    CHECK(!jash_query_cancelable(NULL, 2, typed));
    CHECK(!jash_query_cancelable("find", 0, NULL));
    static jash_state_t state;
    int status = 0;
    jash_query_cancel_control(poll_status, &status);
    CHECK(!jash_query_cancel_poll(&state));
    CHECK(!state.agent_request_cancelled && callback_calls == 1);
    status = 143;
    CHECK(jash_query_cancel_poll(&state) == 143);
    CHECK(state.agent_request_cancelled && callback_calls == 2);
    status = 0;
    CHECK(jash_query_cancel_poll(&state) == 143 && callback_calls == 2);
    state.agent_request_cancelled = 0;
    jash_query_cancel_control(poll_status, &status);
    CHECK(!jash_query_cancel_poll(&state) && !state.agent_request_cancelled);
    status = 1;
    CHECK(jash_query_cancel_poll(&state) == 130);
    jash_query_cancel_control(NULL, NULL);
    state.agent_request_cancelled = 0;
    CHECK(!jash_query_cancel_poll(&state) && !state.agent_request_cancelled);
    direct_cancel = 1;
    CHECK(jash_query_cancel_poll(&state) == 130 && state.agent_request_cancelled);
    jash_query_cancel_control(NULL, NULL);
    puts("query cancellation admission and status: OK");
    return 0;
}
