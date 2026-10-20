/* Host behavior test for the MINIX AF_UNIX/SCM_RIGHTS OSIF provider. */
#include "../c/osif/ipc.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/un.h>
#include <unistd.h>

/* Linux host shim so this harness can exercise MINIX's getpeereid path. */
int getpeereid(int socket, uid_t *euid, gid_t *egid)
{
    struct ucred credentials;
    socklen_t length = sizeof(credentials);
    (void)egid;
    if (getsockopt(socket, SOL_SOCKET, SO_PEERCRED, &credentials, &length) != 0)
        return -1;
    if (length != sizeof(credentials)) {
        errno = EIO;
        return -1;
    }
    *euid = credentials.uid;
    return 0;
}

int main(void)
{
    int endpoints[2] = {-1, -1};
    int lanes[2] = {-1, -1};
    int pipefd[2] = {-1, -1};
    int payload_pipe[2] = {-1, -1};
    int listener = -1, client = -1, accepted = -1;
    int received = -1;
    int payload_fd = -1;
    int descriptor_flags0, descriptor_flags1, status_flags0, status_flags1;
    uint64_t payload_size = 0;
    const uint64_t expected_size = UINT64_C(0x0102030405060708);
    char socket_dir[512], socket_path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    struct sockaddr_un address = {0};
    uid_t peer = (uid_t)-1;
    jash_ipc_stream_frame_t stream_frame;
    char stream_bytes[4] = {0};
    char marker = 0, byte = 0;
    struct iovec iov = {&marker, sizeof(marker)};
    union {
        struct cmsghdr align;
        unsigned char bytes[CMSG_SPACE(sizeof(int))];
    } control = {{0}};
    struct msghdr message = {0};
    struct cmsghdr *cmsg;

    if (jash_ipc_control_pair(endpoints) != 0) {
        perror("jash_ipc_control_pair");
        return 1;
    }
    descriptor_flags0 = fcntl(endpoints[0], F_GETFD);
    descriptor_flags1 = fcntl(endpoints[1], F_GETFD);
    if (descriptor_flags0 < 0 || descriptor_flags1 < 0 ||
        !(descriptor_flags0 & FD_CLOEXEC) || !(descriptor_flags1 & FD_CLOEXEC))
        return 2;
    status_flags0 = fcntl(endpoints[0], F_GETFL);
    status_flags1 = fcntl(endpoints[1], F_GETFL);
    if (status_flags0 < 0 || status_flags1 < 0 ||
        !(status_flags0 & O_NONBLOCK) || (status_flags1 & O_NONBLOCK))
        return 3;
    if (pipe(pipefd) != 0)
        return 4;
    if (jash_ipc_send_control(endpoints[1], pipefd[1]) != 0) {
        perror("jash_ipc_send_control");
        return 5;
    }

    message.msg_iov = &iov;
    message.msg_iovlen = 1;
    message.msg_control = control.bytes;
    message.msg_controllen = sizeof(control.bytes);
    if (recvmsg(endpoints[0], &message, MSG_CMSG_CLOEXEC) != 1)
        return 6;
    cmsg = CMSG_FIRSTHDR(&message);
    if (marker != 'C' || cmsg == NULL || cmsg->cmsg_level != SOL_SOCKET ||
        cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len != CMSG_LEN(sizeof(int)))
        return 7;
    memcpy(&received, CMSG_DATA(cmsg), sizeof(received));
    if (!(fcntl(received, F_GETFD) & FD_CLOEXEC))
        return 8;
    if (write(received, "x", 1) != 1 || read(pipefd[0], &byte, 1) != 1 || byte != 'x')
        return 9;

    if (jash_ipc_data_pair(lanes) != 0 || pipe(payload_pipe) != 0)
        return 10;
    descriptor_flags0 = fcntl(lanes[0], F_GETFD);
    descriptor_flags1 = fcntl(lanes[1], F_GETFD);
    status_flags0 = fcntl(lanes[0], F_GETFL);
    status_flags1 = fcntl(lanes[1], F_GETFL);
    if (descriptor_flags0 < 0 || descriptor_flags1 < 0 ||
        !(descriptor_flags0 & FD_CLOEXEC) || !(descriptor_flags1 & FD_CLOEXEC) ||
        status_flags0 < 0 || status_flags1 < 0 ||
        !(status_flags0 & O_NONBLOCK) || !(status_flags1 & O_NONBLOCK))
        return 10;
    if (jash_ipc_data_send(lanes[0], "artifact", expected_size, payload_pipe[0]) != 0) {
        perror("jash_ipc_data_send");
        return 11;
    }
    if (jash_ipc_data_receive(lanes[1], "artifact", &payload_size, &payload_fd) != 0) {
        perror("jash_ipc_data_receive");
        return 12;
    }
    descriptor_flags0 = fcntl(payload_fd, F_GETFD);
    if (payload_size != expected_size || payload_fd < 0 ||
        descriptor_flags0 < 0 || !(descriptor_flags0 & FD_CLOEXEC))
        return 13;
    if (jash_ipc_stream_send(lanes[0], "job/alpha", 17, 4,
                             JASH_IPC_STREAM_DATA, "abc", 3) != 0) {
        perror("jash_ipc_stream_send");
        return 14;
    }
    if (jash_ipc_stream_receive(lanes[1], &stream_frame,
                                stream_bytes, sizeof(stream_bytes)) != 0) {
        perror("jash_ipc_stream_receive");
        return 15;
    }
    if (strcmp(stream_frame.ref, "job/alpha") != 0 ||
        stream_frame.sequence != 17 || stream_frame.offset != 4 ||
        stream_frame.length != 3 || stream_frame.flags != JASH_IPC_STREAM_DATA ||
        memcmp(stream_bytes, "abc", 3) != 0)
        return 16;

    if (snprintf(socket_dir, sizeof(socket_dir), "%s/ipc-dir.XXXXXX",
                 getenv("TMPDIR")) >= (int)sizeof(socket_dir) ||
        mkdtemp(socket_dir) == NULL)
        return 17;
    if (snprintf(socket_path, sizeof(socket_path), "%s/control.sock", socket_dir) >=
        (int)sizeof(socket_path))
        return 18;
    listener = jash_ipc_listen(socket_path, 4);
    if (listener < 0) {
        perror("jash_ipc_listen");
        return 19;
    }
    client = socket(AF_UNIX, SOCK_STREAM, 0);
    if (client < 0)
        return 20;
    address.sun_family = AF_UNIX;
    memcpy(address.sun_path, socket_path, strlen(socket_path) + 1);
    if (connect(client, (struct sockaddr *)&address, sizeof(address)) != 0)
        return 21;
    accepted = jash_ipc_accept(listener, &peer);
    if (accepted < 0) {
        perror("jash_ipc_accept");
        return 22;
    }
    descriptor_flags0 = fcntl(accepted, F_GETFD);
    if (peer != geteuid() || descriptor_flags0 < 0 ||
        !(descriptor_flags0 & FD_CLOEXEC))
        return 23;
    errno = 0;
    if (jash_ipc_snapshot_bytes("x", 1) != -1 || errno != ENOTSUP)
        return 24;
    errno = 0;
    if (jash_ipc_snapshot_file(socket_path, 1024) != -1 || errno != ENOTSUP)
        return 25;

    close(accepted);
    close(client);
    close(listener);
    unlink(socket_path);
    rmdir(socket_dir);
    close(received);
    close(payload_fd);
    close(payload_pipe[0]);
    close(payload_pipe[1]);
    close(lanes[0]);
    close(lanes[1]);
    close(pipefd[0]);
    close(pipefd[1]);
    close(endpoints[0]);
    close(endpoints[1]);
    puts("MINIX OSIF local socket, FD passing, data/stream, peer-ID probes PASS; immutable snapshots explicitly ENOTSUP");
    return 0;
}
