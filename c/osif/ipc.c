// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "ipc.h"
#ifdef JASH_BETHOS
#include <errno.h>

int jash_ipc_listen(const char* path, int backlog)
{ (void)path; (void)backlog; errno = ENOSYS; return -1; }
int jash_ipc_accept(int listener, uid_t* peer)
{ (void)listener; (void)peer; errno = ENOSYS; return -1; }
int jash_ipc_control_pair(int endpoints[2])
{ (void)endpoints; errno = ENOSYS; return -1; }
int jash_ipc_shutdown_pair(int endpoints[2])
{ (void)endpoints; errno = ENOSYS; return -1; }
int jash_ipc_send_control(int connection, int endpoint)
{ (void)connection; (void)endpoint; errno = ENOSYS; return -1; }
int jash_ipc_data_pair(int endpoints[2])
{ (void)endpoints; errno = ENOSYS; return -1; }
int jash_ipc_send_data_endpoint(int connection, int endpoint)
{ (void)connection; (void)endpoint; errno = ENOSYS; return -1; }

int jash_ipc_data_send(int lane, const char* id, uint64_t size, int fd)
{ (void)lane; (void)id; (void)size; (void)fd; errno = ENOSYS; return -1; }
int jash_ipc_data_receive(int lane, const char* id, uint64_t* size, int* fd)
{ (void)lane; (void)id; (void)size; (void)fd; errno = ENOSYS; return -1; }
int jash_ipc_stream_send(int lane, const char* ref, uint64_t sequence,
                         uint64_t offset, unsigned flags,
                         const void* bytes, size_t length)
{ (void)lane; (void)ref; (void)sequence; (void)offset; (void)flags;
  (void)bytes; (void)length; errno = ENOSYS; return -1; }
int jash_ipc_stream_receive(int lane, jash_ipc_stream_frame_t* frame,
                            void* bytes, size_t capacity)
{ (void)lane; (void)frame; (void)bytes; (void)capacity; errno = ENOSYS; return -1; }
int jash_ipc_snapshot_bytes(const void* bytes, size_t size)
{ (void)bytes; (void)size; errno = ENOSYS; return -1; }
int jash_ipc_snapshot_file(const char* path, size_t limit)
{ (void)path; (void)limit; errno = ENOSYS; return -1; }

#else
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>
#include <sys/mman.h>
#include <stdint.h>

static int ipc_pair(int endpoints[2], int type, int both_nonblocking) {
    if (!endpoints) { errno = EINVAL; return -1; }
    endpoints[0] = endpoints[1] = -1;
#if defined(__linux__) || defined(__minix)
    if (socketpair(AF_UNIX, type | SOCK_CLOEXEC, 0, endpoints)) return -1;
    for (int i = 0; i <= both_nonblocking; i++) {
        int flags = fcntl(endpoints[i], F_GETFL);
        if (flags < 0 || fcntl(endpoints[i], F_SETFL, flags | O_NONBLOCK)) {
            int saved = errno; close(endpoints[0]); close(endpoints[1]);
            endpoints[0] = endpoints[1] = -1; errno = saved; return -1;
        }
    }
    return 0;
#else
    (void)type; (void)both_nonblocking;
    errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_control_pair(int endpoints[2]) { return ipc_pair(endpoints, SOCK_STREAM, 0); }
int jash_ipc_shutdown_pair(int endpoints[2]) { return ipc_pair(endpoints, SOCK_SEQPACKET, 1); }
int jash_ipc_data_pair(int endpoints[2]) { return ipc_pair(endpoints, SOCK_SEQPACKET, 1); }

static int ipc_send_endpoint(int connection, int endpoint, char marker) {
#if defined(__linux__) || defined(__minix)
    struct iovec payload = {.iov_base = &marker, .iov_len = 1};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct msghdr message = {0};
    message.msg_iov = &payload; message.msg_iovlen = 1;
    message.msg_control = ancillary.bytes; message.msg_controllen = sizeof(ancillary.bytes);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message);
    cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &endpoint, sizeof(endpoint));
    ssize_t n;
    do { n = sendmsg(connection, &message, MSG_NOSIGNAL); } while (n < 0 && errno == EINTR);
    if (n == 1) return 0;
    if (n >= 0) errno = EIO;
    return -1;
#else
    (void)connection; (void)endpoint; (void)marker; errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_send_control(int connection, int endpoint) { return ipc_send_endpoint(connection, endpoint, 'C'); }
int jash_ipc_send_data_endpoint(int connection, int endpoint) { return ipc_send_endpoint(connection, endpoint, 'D'); }

#if defined(__linux__) || defined(__minix)
static int ipc_data_type(int lane) {
    int type = 0; socklen_t length = sizeof(type);
    if (getsockopt(lane, SOL_SOCKET, SO_TYPE, &type, &length) || type != SOCK_SEQPACKET) { errno = EPROTOTYPE; return -1; }
    return 0;
}

static void ipc_put_u64(unsigned char* out, uint64_t value) {
    for (int i = 7; i >= 0; i--) { out[i] = (unsigned char)value; value >>= 8; }
}

static uint64_t ipc_get_u64(const unsigned char* in) {
    uint64_t value = 0;
    for (int i = 0; i < 8; i++) value = (value << 8) | in[i];
    return value;
}
#endif

int jash_ipc_data_send(int lane, const char* id, uint64_t size, int fd) {
#if defined(__linux__) || defined(__minix)
    if (!id || !*id || strlen(id) > 47 || fd < 0 || ipc_data_type(lane)) { errno = EINVAL; return -1; }
    unsigned char frame[64] = {0}; memcpy(frame, "JDF1", 4); frame[4] = (unsigned char)strlen(id); memcpy(frame + 8, id, frame[4]);
    for (int i = 63; i >= 56; i--) { frame[i] = (unsigned char)size; size >>= 8; }
    struct iovec payload = {frame, sizeof(frame)};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(sizeof(int))]; } ancillary = {0};
    struct msghdr message = {0}; message.msg_iov = &payload; message.msg_iovlen = 1; message.msg_control = ancillary.bytes; message.msg_controllen = sizeof(ancillary.bytes);
    struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg->cmsg_level = SOL_SOCKET; cmsg->cmsg_type = SCM_RIGHTS; cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd)); ssize_t n;
    do { n = sendmsg(lane, &message, MSG_NOSIGNAL | MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
    if (n == (ssize_t)sizeof(frame)) return 0;
    if (n >= 0) errno = EIO;
    return -1;
#else
    (void)lane; (void)id; (void)size; (void)fd; errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_data_receive(int lane, const char* id, uint64_t* size, int* fd) {
    if (!fd || !size) { errno = EINVAL; return -1; } *fd = -1; *size = 0;
#if defined(__linux__) || defined(__minix)
    if (!id || !*id || strlen(id) > 47 || ipc_data_type(lane)) { errno = EINVAL; return -1; }
    unsigned char frame[64]; struct iovec payload = {frame, sizeof(frame)};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(16 * sizeof(int))]; } ancillary = {0};
    struct msghdr message = {0}; message.msg_iov = &payload; message.msg_iovlen = 1; message.msg_control = ancillary.bytes; message.msg_controllen = sizeof(ancillary.bytes);
    ssize_t n;
    do { n = recvmsg(lane, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); } while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (!n) { errno = EPIPE; return -1; }
    int received[16], count = 0, bad = n != 64 || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg; cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS || cmsg->cmsg_len < CMSG_LEN(0)) { bad = 1; continue; }
        size_t bytes = cmsg->cmsg_len - CMSG_LEN(0); if (bytes % sizeof(int)) bad = 1;
        for (size_t i = 0; i + sizeof(int) <= bytes; i += sizeof(int)) {
            int value; memcpy(&value, (char*)CMSG_DATA(cmsg) + i, sizeof(value));
            if (count < 16) received[count++] = value; else { close(value); bad = 1; }
        }
    }
    if (count != 1) bad = 1;
    if (n == 64) {
        size_t length = strlen(id);
        if (memcmp(frame, "JDF1", 4) || frame[4] != length || frame[5] || frame[6] || frame[7] || memcmp(frame + 8, id, length)) bad = 1;
        for (size_t i = 8 + length; i < 56; i++) if (frame[i]) bad = 1;
    }
    if (bad) { for (int i = 0; i < count; i++) close(received[i]); errno = EPROTO; return -1; }
    for (int i = 56; i < 64; i++) *size = (*size << 8) | frame[i];
    *fd = received[0]; return 0;
#else
    (void)lane; (void)id; errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_stream_send(int lane, const char* ref, uint64_t sequence,
                         uint64_t offset, unsigned flags,
                         const void* bytes, size_t length) {
#if defined(__linux__) || defined(__minix)
    size_t ref_length = ref ? strlen(ref) : 0;
    unsigned eof_flags = JASH_IPC_STREAM_EOF | JASH_IPC_STREAM_INCOMPLETE |
                         JASH_IPC_STREAM_CANCELLED;
    int data = flags == JASH_IPC_STREAM_DATA && length > 0;
    int eof = (flags & JASH_IPC_STREAM_EOF) && !(flags & ~eof_flags) && !length;
    if (ipc_data_type(lane) || !ref_length || ref_length > JASH_IPC_STREAM_REF_MAX ||
        length > JASH_IPC_STREAM_CHUNK_MAX || (length && !bytes) ||
        offset > UINT64_MAX - length || (!data && !eof)) {
        errno = EINVAL;
        return -1;
    }
    unsigned char header[JASH_IPC_STREAM_HEADER_SIZE] = {0};
    memcpy(header, "JDS1", 4);
    header[4] = (unsigned char)flags;
    header[5] = (unsigned char)ref_length;
    ipc_put_u64(header + 8, sequence);
    ipc_put_u64(header + 16, offset);
    ipc_put_u64(header + 24, length);
    memcpy(header + 32, ref, ref_length);
    struct iovec payload[2] = {{header, sizeof header}, {(void*)bytes, length}};
    struct msghdr message = {0};
    message.msg_iov = payload;
    message.msg_iovlen = length ? 2 : 1;
    ssize_t n;
    do { n = sendmsg(lane, &message, MSG_NOSIGNAL | MSG_DONTWAIT); }
    while (n < 0 && errno == EINTR);
    size_t expected = sizeof header + length;
    if (n == (ssize_t)expected) return 0;
    if (n >= 0) errno = EIO;
    return -1;
#else
    (void)lane; (void)ref; (void)sequence; (void)offset; (void)flags;
    (void)bytes; (void)length; errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_stream_receive(int lane, jash_ipc_stream_frame_t* frame,
                            void* bytes, size_t capacity) {
    if (!frame) { errno = EINVAL; return -1; }
    memset(frame, 0, sizeof *frame);
#if defined(__linux__) || defined(__minix)
    if (ipc_data_type(lane)) return -1;
    unsigned char packet[JASH_IPC_STREAM_HEADER_SIZE + JASH_IPC_STREAM_CHUNK_MAX];
    struct iovec payload = {packet, sizeof packet};
    union { struct cmsghdr align; unsigned char bytes[CMSG_SPACE(16 * sizeof(int))]; } ancillary = {0};
    struct msghdr message = {0};
    message.msg_iov = &payload; message.msg_iovlen = 1;
    message.msg_control = ancillary.bytes; message.msg_controllen = sizeof ancillary.bytes;
    ssize_t n;
    do { n = recvmsg(lane, &message, MSG_CMSG_CLOEXEC | MSG_DONTWAIT); }
    while (n < 0 && errno == EINTR);
    if (n < 0) return -1;
    if (!n) { errno = EPIPE; return -1; }
    int received[16], count = 0;
    int bad = n < (ssize_t)JASH_IPC_STREAM_HEADER_SIZE ||
              (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC));
    for (struct cmsghdr* cmsg = CMSG_FIRSTHDR(&message); cmsg;
         cmsg = CMSG_NXTHDR(&message, cmsg)) {
        if (cmsg->cmsg_level != SOL_SOCKET || cmsg->cmsg_type != SCM_RIGHTS ||
            cmsg->cmsg_len < CMSG_LEN(0)) { bad = 1; continue; }
        size_t descriptor_bytes = cmsg->cmsg_len - CMSG_LEN(0);
        if (descriptor_bytes % sizeof(int)) bad = 1;
        for (size_t i = 0; i + sizeof(int) <= descriptor_bytes; i += sizeof(int)) {
            int value; memcpy(&value, (char*)CMSG_DATA(cmsg) + i, sizeof value);
            if (count < 16) received[count++] = value;
            else { close(value); bad = 1; }
        }
    }
    if (count) bad = 1;
    size_t ref_length = n >= 6 ? packet[5] : 0;
    uint64_t payload_length = n >= 32 ? ipc_get_u64(packet + 24) : UINT64_MAX;
    unsigned flags = n >= 5 ? packet[4] : 0;
    unsigned eof_flags = JASH_IPC_STREAM_EOF | JASH_IPC_STREAM_INCOMPLETE |
                         JASH_IPC_STREAM_CANCELLED;
    int data = flags == JASH_IPC_STREAM_DATA && payload_length > 0;
    int eof = (flags & JASH_IPC_STREAM_EOF) && !(flags & ~eof_flags) && !payload_length;
    if (n >= (ssize_t)JASH_IPC_STREAM_HEADER_SIZE) {
        if (memcmp(packet, "JDS1", 4) || packet[6] || packet[7] ||
            !ref_length || ref_length > JASH_IPC_STREAM_REF_MAX ||
            payload_length > JASH_IPC_STREAM_CHUNK_MAX ||
            payload_length != (uint64_t)n - JASH_IPC_STREAM_HEADER_SIZE ||
            (!data && !eof)) bad = 1;
        for (size_t i = 32 + ref_length; i < JASH_IPC_STREAM_HEADER_SIZE; i++)
            if (packet[i]) bad = 1;
    }
    if (bad) {
        for (int i = 0; i < count; i++) close(received[i]);
        errno = EPROTO;
        return -1;
    }
    if (payload_length > capacity || (payload_length && !bytes)) {
        errno = EMSGSIZE;
        return -1;
    }
    memcpy(frame->ref, packet + 32, ref_length);
    frame->sequence = ipc_get_u64(packet + 8);
    frame->offset = ipc_get_u64(packet + 16);
    frame->length = (size_t)payload_length;
    frame->flags = flags;
    if (payload_length) memcpy(bytes, packet + JASH_IPC_STREAM_HEADER_SIZE,
                               (size_t)payload_length);
    return 0;
#else
    (void)lane; (void)bytes; (void)capacity; errno = ENOTSUP; return -1;
#endif
}

#if defined(__linux__) && !defined(__minix)
static int snapshot_write(int fd, const void* data, size_t size) {
    const char* p = data;
    while (size) {
        ssize_t n = write(fd, p, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return -1;
        p += n; size -= (size_t)n;
    }
    return 0;
}

static int snapshot_finish(int fd) {
    if (lseek(fd, 0, SEEK_SET) < 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_SEAL)) {
        close(fd); return -1;
    }
    return fd;
}
#endif

int jash_ipc_snapshot_bytes(const void* bytes, size_t size) {
#if defined(__linux__) && !defined(__minix)
    int fd = memfd_create("jash-verifier", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) return -1;
    if (size && (!bytes || snapshot_write(fd, bytes, size))) { close(fd); return -1; }
    return snapshot_finish(fd);
#else
    (void)bytes; (void)size; errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_snapshot_file(const char* path, size_t limit) {
#if defined(__linux__) && !defined(__minix)
    int source = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (source < 0) return -1;
    if (fstat(source, &st) || !S_ISREG(st.st_mode) || st.st_size < 0 || (uintmax_t)st.st_size > limit) { close(source); return -1; }
    int fd = memfd_create("jash-verifier", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    if (fd < 0) { close(source); return -1; }
    char buffer[65536]; size_t total = 0; int failed = 0;
    for (;;) {
        ssize_t n = read(source, buffer, sizeof(buffer));
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { failed = 1; break; }
        if (!n) break;
        if ((size_t)n > limit - total || snapshot_write(fd, buffer, (size_t)n)) { failed = 1; break; }
        total += (size_t)n;
    }
    close(source);
    if (failed) { close(fd); return -1; }
    return snapshot_finish(fd);
#else
    (void)path; (void)limit; errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_listen(const char* path, int backlog) {
#if defined(__linux__) || defined(__minix)
    struct sockaddr_un address = {0}; address.sun_family = AF_UNIX;
    if (!path || path[0] != '/' || strlen(path) >= sizeof(address.sun_path)) { errno = ENAMETOOLONG; return -1; }
    /* INV-JASH-TIER1-SOURCE-001: both path buffers use the checked sun_path bound. */
    char parent[sizeof(address.sun_path)]; memcpy(parent, path, strlen(path) + 1);
    char* slash = strrchr(parent, '/'); if (slash == parent) slash[1] = 0; else *slash = 0;
    int dir = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    int trusted = dir >= 0 && !fstat(dir, &st) && st.st_uid == geteuid() && !(st.st_mode & 0022);
    if (dir >= 0) close(dir);
    if (!trusted) { errno = EPERM; return -1; }
    memcpy(address.sun_path, path, strlen(path) + 1);
    int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    mode_t mask = umask(0077);
    int rc = bind(fd, (struct sockaddr*)&address, sizeof(address));
    int saved = errno; umask(mask);
    if (rc) { close(fd); errno = saved; return -1; }
    if (listen(fd, backlog)) { saved = errno; close(fd); unlink(path); errno = saved; return -1; }
    return fd;
#else
    (void)path; (void)backlog; errno = ENOTSUP; return -1;
#endif
}

int jash_ipc_accept(int listener, uid_t* peer) {
#if defined(__linux__) && !defined(__minix)
    if (peer == NULL) { errno = EINVAL; return -1; }
    int fd = accept4(listener, NULL, NULL, SOCK_CLOEXEC);
    if (fd < 0) return -1;
    struct ucred credentials; socklen_t length = sizeof(credentials);
    if (getsockopt(fd, SOL_SOCKET, SO_PEERCRED, &credentials, &length) || length != sizeof(credentials)) {
        close(fd); return -1;
    }
    *peer = credentials.uid;
    return fd;
#elif defined(__minix)
    if (peer == NULL) { errno = EINVAL; return -1; }
    int fd = accept(listener, NULL, NULL);
    if (fd < 0) return -1;
    int descriptor_flags = fcntl(fd, F_GETFD);
    uid_t euid;
    if (descriptor_flags < 0 ||
        fcntl(fd, F_SETFD, descriptor_flags | FD_CLOEXEC) < 0 ||
        getpeereid(fd, &euid, NULL) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    *peer = euid;
    return fd;
#else
    (void)listener; (void)peer; errno = ENOTSUP; return -1;
#endif
}

#endif
