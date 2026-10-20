// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <core/agent_cancel.h>
#include <assert.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static void send_bytes(int fd, const void* bytes, size_t length) {
    assert(send(fd, bytes, length, 0) == (ssize_t)length);
}
static void ack(int fd, const char* expected) {
    char data[1024]; ssize_t n = recv(fd, data, sizeof(data) - 1, 0);
    assert(n > 0); data[n] = 0; assert(strstr(data, expected));
}

int main(void) {
    int sockets[2]; assert(!socketpair(AF_UNIX, SOCK_STREAM, 0, sockets));
    assert(!fcntl(sockets[1], F_SETFL, O_NONBLOCK));
    jash_state_t* state = calloc(1, sizeof(*state)); assert(state);
    state->agent_cancel_fd = sockets[1]; strcpy(state->agent_active_request_id, "active");
    const char valid[] = "{\"cancel\":\"active\"}\n";
    send_bytes(sockets[0], valid, 5);
    assert(!jash_agent_cancel_poll(state));
    send_bytes(sockets[0], valid + 5, sizeof(valid) - 6);
    assert(jash_agent_cancel_poll(state)); ack(sockets[0], "\"accepted\":true");

    const char nul[] = "{\"cancel\":\"active\"}\0junk\n";
    send_bytes(sockets[0], nul, sizeof(nul) - 1);
    assert(!jash_agent_cancel_poll(state)); ack(sockets[0], "invalid cancel frame");

    char oversized[400]; memset(oversized, 'x', 300); memcpy(oversized + 300, valid, sizeof(valid) - 1);
    send_bytes(sockets[0], oversized, 300 + sizeof(valid) - 1);
    assert(!jash_agent_cancel_poll(state)); ack(sockets[0], "invalid cancel frame");
    send_bytes(sockets[0], valid, sizeof(valid) - 1);
    assert(jash_agent_cancel_poll(state)); ack(sockets[0], "\"accepted\":true");

    const char burst[] = "{\"cancel\":\"wrong\"}\n{}\n{\"cancel\":\"active\"}\n";
    send_bytes(sockets[0], burst, sizeof(burst) - 1);
    assert(jash_agent_cancel_poll(state)); ack(sockets[0], "\"accepted\":true");

    send_bytes(sockets[0], valid, sizeof(valid) - 1); close(sockets[0]);
    assert(jash_agent_cancel_poll(state));
    assert(!jash_agent_cancel_poll(state) && state->agent_cancel_fd == -1);
    close(sockets[1]); free(state); return 0;
}
