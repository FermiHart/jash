// SPDX-License-Identifier: LicenseRef-SPL-1.0
/* Inject faults only into the original response pipe, never captured commands. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
static dev_t output_device;
static ino_t output_inode;
static int enabled, interrupted, first_line, writes;
__attribute__((constructor)) static void setup(void) {
    struct stat output;
    if (!fstat(STDOUT_FILENO, &output)) { output_device = output.st_dev; output_inode = output.st_ino; enabled = 1; }
}
ssize_t write(int fd, const void* bytes, size_t size) {
    ssize_t (*next)(int, const void*, size_t) = dlsym(RTLD_NEXT, "write");
    struct stat output;
    const char* mode = getenv("JASH_TEST_DELIVERY");
    if (enabled && mode && !fstat(fd, &output) && output.st_dev == output_device && output.st_ino == output_inode) {
        if (!strcmp(mode, "after_first_write") && writes++) { errno = ENOSPC; return -1; }
        if (!strcmp(mode, "full") || (!strcmp(mode, "after_handshake") && first_line)) { errno = ENOSPC; return -1; }
        if (!strcmp(mode, "short")) {
            if (!interrupted++) { errno = EINTR; return -1; }
            if (size > 7) size = 7;
        }
        ssize_t result = next(fd, bytes, size);
        if (result > 0 && memchr(bytes, '\n', (size_t)result)) first_line = 1;
        return result;
    }
    return next(fd, bytes, size);
}
