// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash-agentd - source-isolated JSONL execution runtime for agents.

#include <core/cache.h>
#include <core/env.h>
#include <core/serve.h>
#include <core/state.h>
#include <core/agent_contract.h>
#include <core/local_server.h>
#include <stdio.h>
#include <string.h>

#include <unistd.h>

static int agent_session(int shutdown_fd, int data_fd);

int main(int argc, char** argv) {
    if (argc >= 2 && !strcmp(argv[1], "--contract-json")) {
        if (argc > 3) {
            fputs("jash-agentd: --contract-json accepts at most one operation id\n", stderr);
            return 2;
        }
        int fd = STDOUT_FILENO;
        return jash_agent_contract_json(argc == 3 ? argv[2] : NULL,
                                        jash_jsonw_fd_sink, &fd);
    }
    if (argc == 5 && !strcmp(argv[1], "--listen") && !strcmp(argv[3], "--scope-file"))
        return jash_local_server(argv[2], argv[4], agent_session);
    if (argc != 1) {
        fputs("usage: jash-agentd [--contract-json [operation-id] | --listen PATH --scope-file FILE]\n", stderr);
        return 2;
    }
    return agent_session(-1, -1);
}

static int agent_session(int shutdown_fd, int data_fd) {
    jash_state_t state;
    jash_state_init(&state);
    state.interactive = 0;
    state.job_control_active = 0;

    cache_init();
    jash_env_seed_defaults(&state);
    if (!state.pid) state.pid = (int)getpid();

    int rc = jash_serve_loop_data(&state, shutdown_fd, data_fd);
    if (data_fd >= 0) close(data_fd);

    jash_state_cleanup(&state);
    return rc;
}
