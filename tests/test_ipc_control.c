// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <osif/ipc.h>
#include <assert.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

static int descriptors(void) {
    DIR* directory = opendir("/proc/self/fd"); assert(directory);
    int count = 0; struct dirent* entry;
    while ((entry = readdir(directory))) if (entry->d_name[0] != '.') count++;
    closedir(directory); return count;
}

int main(void) {
    int before = descriptors();
    for (int iteration = 0; iteration < 32; iteration++) {
        int transport[2], control[2];
        assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, transport));
        assert(!jash_ipc_control_pair(control));
        assert(fcntl(control[0], F_GETFD) & FD_CLOEXEC);
        assert(fcntl(control[1], F_GETFD) & FD_CLOEXEC);
        assert(fcntl(control[0], F_GETFL) & O_NONBLOCK);
        assert(!jash_ipc_send_control(transport[0], control[1]));
        assert(fcntl(control[1], F_GETFD) >= 0); /* send borrows, never consumes */
        close(control[1]);
        char marker;
        struct iovec data = {.iov_base = &marker, .iov_len = 1};
        union { struct cmsghdr align; char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
        struct msghdr message = {0}; message.msg_iov = &data; message.msg_iovlen = 1;
        message.msg_control = ancillary.bytes; message.msg_controllen = sizeof(ancillary.bytes);
        assert(recvmsg(transport[1], &message, MSG_CMSG_CLOEXEC) == 1 && marker == 'C');
        assert(!(message.msg_flags & MSG_CTRUNC));
        struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
        assert(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS);
        assert(cmsg->cmsg_len == CMSG_LEN(sizeof(int)) && !CMSG_NXTHDR(&message, cmsg));
        int received; memcpy(&received, CMSG_DATA(cmsg), sizeof(received));
        assert(fcntl(received, F_GETFD) & FD_CLOEXEC);
        assert(write(received, "cancel", 6) == 6);
        char bytes[6]; assert(read(control[0], bytes, sizeof(bytes)) == 6 && !memcmp(bytes, "cancel", 6));
        close(received); assert(read(control[0], bytes, sizeof(bytes)) == 0);
        close(transport[1]);
        assert(jash_ipc_send_control(transport[0], control[0]) == -1 && errno == EPIPE);
        assert(fcntl(control[0], F_GETFD) >= 0);
        close(control[0]); close(transport[0]);
    }
    assert(descriptors() == before);
    assert(jash_ipc_control_pair(NULL) == -1 && errno == EINVAL);
    return 0;
}
