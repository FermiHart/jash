// SPDX-License-Identifier: LicenseRef-SPL-1.0
// Test-only process boundary: export the sealed o: snapshot by SCM_RIGHTS.
#define _GNU_SOURCE
#include "core/ai_observation.h"

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

int main(int argc, char** argv) {
    if (argc != 3) return 2;
    char* end;
    errno = 0;
    long socket_number = strtol(argv[2], &end, 10);
    if (errno || *end || socket_number < 3 || socket_number > INT_MAX) return 2;
    char observation[4096];
    if (jash_ai_observe_ls(argv[1], observation, sizeof(observation))) return 1;
    uint64_t size;
    int snapshot = jash_ai_observation_reader_fd("latest", &size);
    if (snapshot < 0) return 1;
    char marker = 'O';
    struct iovec iov = {.iov_base = &marker, .iov_len = 1};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {0};
    struct msghdr message = {0};
    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    struct cmsghdr* right = CMSG_FIRSTHDR(&message);
    right->cmsg_level = SOL_SOCKET;
    right->cmsg_type = SCM_RIGHTS;
    right->cmsg_len = CMSG_LEN(sizeof(int));
    *((int*)CMSG_DATA(right)) = snapshot;
    ssize_t sent;
    do { sent = sendmsg((int)socket_number, &message, MSG_NOSIGNAL); }
    while (sent < 0 && errno == EINTR);
    int saved = errno;
    close(snapshot);
    jash_ai_observation_forget();
    if (sent != 1) { errno = saved; return 1; }
    if (fputs(observation, stdout) == EOF || putchar('\n') == EOF ||
        fflush(stdout) == EOF) return 1;
    (void)size;
    return 0;
}
