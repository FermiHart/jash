// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <osif/ipc.h>
#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <string.h>

int main(int argc, char** argv) {
    assert(argc == 2 && !chdir(argv[1]));
    int file = open("input", O_RDWR | O_CREAT | O_EXCL, 0600);
    assert(file >= 0 && write(file, "old", 3) == 3);
    int snapshot = jash_ipc_snapshot_file("input", 3);
    assert(snapshot >= 0);
    assert(lseek(file, 0, SEEK_SET) == 0 && write(file, "new", 3) == 3);
    char content[3]; assert(read(snapshot, content, 3) == 3 && !memcmp(content, "old", 3));
    int seals = fcntl(snapshot, F_GET_SEALS);
    assert((seals & (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL)) ==
           (F_SEAL_WRITE | F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_SEAL));
    assert(fcntl(snapshot, F_GETFD) & FD_CLOEXEC);
    assert(write(snapshot, "x", 1) < 0 && errno == EPERM);
    assert(ftruncate(snapshot, 0) < 0 && errno == EPERM);
    assert(jash_ipc_snapshot_file("input", 2) < 0);
    close(snapshot); close(file);
    snapshot = jash_ipc_snapshot_bytes("a\0b", 3);
    assert(snapshot >= 0 && read(snapshot, content, 3) == 3 && !memcmp(content, "a\0b", 3));
    close(snapshot); return 0;
}
