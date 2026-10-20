// SPDX-License-Identifier: LicenseRef-SPL-1.0
#include <osif/ipc.h>
#include <osif/buffer.h>
#include <assert.h>
#include <errno.h>
int main(void) {
    assert(!jash_buffer_compiled() && !jash_buffer_probe());
    assert(jash_buffer_create("", 0, 0) < 0 && errno == ENOTSUP);
    assert(jash_buffer_reader_fd(-1, 0, 0) < 0 && errno == ENOTSUP);
    assert(jash_buffer_copy_range_fd(-1, 0, 0, 0) < 0 && errno == ENOTSUP);
    int pair[2]; assert(jash_ipc_data_pair(pair) < 0 && pair[0] == -1 && pair[1] == -1 && errno == ENOTSUP);
    int fd; uint64_t size;
    assert(jash_ipc_data_receive(-1, "x", &size, &fd) < 0 && fd == -1 && errno == ENOTSUP);
    jash_ipc_stream_frame_t frame;
    assert(jash_ipc_stream_send(-1, "s-test", 0, 0, JASH_IPC_STREAM_EOF, 0, 0) < 0 && errno == ENOTSUP);
    assert(jash_ipc_stream_receive(-1, &frame, 0, 0) < 0 && errno == ENOTSUP);
    return 0;
}
