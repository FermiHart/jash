// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include "agent_cancel.h"
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>

static void cancel_reply(jash_state_t* state, const char* id, int accepted) {
    char reply[192];
    int size = id ? snprintf(reply, sizeof(reply), accepted
        ? "{\"cancel\":\"%s\",\"accepted\":true}\n"
        : "{\"cancel\":\"%s\",\"accepted\":false,\"error\":\"request is not active\"}\n", id)
        : snprintf(reply, sizeof(reply), "{\"cancel\":null,\"accepted\":false,\"error\":\"invalid cancel frame\"}\n");
    if (size <= 0 || (size_t)size >= sizeof(reply)) return;
    int flags = 0;
#ifdef MSG_NOSIGNAL
    flags |= MSG_NOSIGNAL;
#endif
    /* Descriptor is nonblocking. The terminal response remains authoritative
     * if an acknowledgement cannot be delivered to a closed/full peer. */
    (void)send(state->agent_cancel_fd, reply, (size_t)size, flags);
}

int jash_agent_cancel_poll(jash_state_t* s) {
    if (!s || s->agent_cancel_fd < 0 || !s->agent_active_request_id[0]) return 0;
    /* Bound control work per data/operation boundary even under a noisy peer. */
    for (unsigned step = 0; step < 8; step++) {
        char* nl = memchr(s->agent_cancel_buf, '\n', s->agent_cancel_len);
        if (!nl) {
            struct pollfd pfd = {.fd = s->agent_cancel_fd, .events = POLLIN};
            if (poll(&pfd, 1, 0) <= 0) return 0;
            if (!(pfd.revents & POLLIN)) {
                if (pfd.revents & (POLLHUP | POLLERR | POLLNVAL)) s->agent_cancel_fd = -1;
                return 0;
            }
            ssize_t n = read(s->agent_cancel_fd, s->agent_cancel_buf + s->agent_cancel_len,
                             sizeof(s->agent_cancel_buf) - 1 - s->agent_cancel_len);
            if (n <= 0) { if (!n) s->agent_cancel_fd = -1; return 0; }
            s->agent_cancel_len += (unsigned)n;
            s->agent_cancel_buf[s->agent_cancel_len] = 0;
            nl = memchr(s->agent_cancel_buf, '\n', s->agent_cancel_len);
        }
        if (!nl) {
            if (s->agent_cancel_len == sizeof(s->agent_cancel_buf) - 1) {
                s->agent_cancel_len = 0;
                s->agent_cancel_discarding = 1;
            } else return 0;
            continue;
        }
        size_t length = (size_t)(nl - s->agent_cancel_buf);
        char id[48] = "", expected[80];
        *nl = 0;
        int valid = !s->agent_cancel_discarding && !memchr(s->agent_cancel_buf, 0, length) &&
            sscanf(s->agent_cancel_buf, "{\"cancel\":\"%47[A-Za-z0-9._:-]\"}", id) == 1;
        int size = snprintf(expected, sizeof(expected), "{\"cancel\":\"%s\"}", id);
        valid = valid && size >= 0 && (size_t)size == length && !memcmp(expected, s->agent_cancel_buf, length);
        int accepted = valid && !strcmp(id, s->agent_active_request_id);
        cancel_reply(s, valid ? id : NULL, accepted);
        size_t consumed = length + 1;
        memmove(s->agent_cancel_buf, s->agent_cancel_buf + consumed, s->agent_cancel_len - consumed);
        s->agent_cancel_len -= (unsigned)consumed;
        s->agent_cancel_buf[s->agent_cancel_len] = 0;
        s->agent_cancel_discarding = 0;
        if (accepted) return 1;
    }
    return 0;
}
