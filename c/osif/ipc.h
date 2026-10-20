// SPDX-License-Identifier: LicenseRef-SPL-1.0
#ifndef JASH_OSIF_IPC_H
#define JASH_OSIF_IPC_H
#include <sys/types.h>
#include <stddef.h>
#include <stdint.h>
/* Local JSON control stream. General data/buffer attachments are not implied. */
int jash_ipc_listen(const char* path, int backlog);
int jash_ipc_accept(int listener, uid_t* peer);
/* Private cancellation pair: both CLOEXEC, server endpoint [0] nonblocking.
 * send_control sends exactly one 'C' byte plus one SCM_RIGHTS descriptor;
 * ownership of both sender descriptors remains with the caller. */
int jash_ipc_control_pair(int endpoints[2]);
/* Internal listener/worker lifecycle channel: CLOEXEC and nonblocking at both ends. */
int jash_ipc_shutdown_pair(int endpoints[2]);
int jash_ipc_send_control(int connection, int endpoint);
/* Opt-in data lane: nonblocking CLOEXEC seqpacket. A 64-byte JDF1 frame carries
 * request ID and size, with exactly one borrowed sender FD. Receiver owns the
 * returned FD; closes every received FD on any framing/association error. */
int jash_ipc_data_pair(int endpoints[2]);
int jash_ipc_send_data_endpoint(int connection, int endpoint);
int jash_ipc_data_send(int lane, const char* id, uint64_t size, int fd);
int jash_ipc_data_receive(int lane, const char* id, uint64_t* size, int* fd);
/* Server-to-client binary spool chunks. JDS1 is one atomic seqpacket with a
 * fixed 96-byte header and no ancillary descriptors. Send is nonblocking;
 * EAGAIN means no sequence, offset or credit may be committed by the caller. */
#define JASH_IPC_STREAM_HEADER_SIZE 96u
#define JASH_IPC_STREAM_CHUNK_MAX 16384u
#define JASH_IPC_STREAM_REF_MAX 63u
#define JASH_IPC_STREAM_DATA 0x01u
#define JASH_IPC_STREAM_EOF 0x02u
#define JASH_IPC_STREAM_INCOMPLETE 0x04u
#define JASH_IPC_STREAM_CANCELLED 0x08u
typedef struct {
    char ref[JASH_IPC_STREAM_REF_MAX + 1u];
    uint64_t sequence;
    uint64_t offset;
    size_t length;
    unsigned flags;
} jash_ipc_stream_frame_t;
int jash_ipc_stream_send(int lane, const char* ref, uint64_t sequence,
                         uint64_t offset, unsigned flags,
                         const void* bytes, size_t length);
int jash_ipc_stream_receive(int lane, jash_ipc_stream_frame_t* frame,
                            void* bytes, size_t capacity);
/* Linux sealed-memfd verifier snapshots; unsupported without immutable handles. */
int jash_ipc_snapshot_bytes(const void* bytes, size_t size);
int jash_ipc_snapshot_file(const char* path, size_t limit);
#endif
