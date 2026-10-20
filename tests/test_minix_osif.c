#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <unistd.h>

#include "osif/osif.h"

int main(void)
{
    long result;
    char byte;
    char directory[] = "/tmp/jash-minix-osif.XXXXXX";
    char absolute[sizeof(directory) + sizeof("/absolute")];
    struct stat st;
    int dirfd, fd;

    result = bear_osif_syscall0(BEAR_OSIF_NR_getpid);
    if (result != (long)getpid()) return 1;

    errno = 0;
    result = bear_osif_syscall3(BEAR_OSIF_NR_read, -1,
        (long)&byte, sizeof(byte));
    if (result != -EBADF) return 2;

    result = bear_osif_syscall0(BEAR_OSIF_NR_MAX + 1);
    if (result != -BEAR_OSIF_ENOSYS) return 3;

    result = bear_osif_syscall3(BEAR_OSIF_NR_mprotect, 1, 1, PROT_NONE);
    if (result != -BEAR_OSIF_ENOSYS) return 13;
    result = bear_osif_syscall3(BEAR_OSIF_NR_sigaltstack, 0, 0, 0);
    if (result != -BEAR_OSIF_ENOSYS) return 14;

    if (mkdtemp(directory) == NULL) return 4;
    dirfd = open(directory, O_RDONLY);
    if (dirfd < 0) return 5;
    result = bear_osif_syscall6(BEAR_OSIF_NR_openat, dirfd,
        (long)"relative", O_WRONLY | O_CREAT | O_EXCL, 0600, 0, 0);
    if (result < 0) return 6;
    fd = (int)result;
    if (bear_osif_syscall1(BEAR_OSIF_NR_close, fd) != 0) return 7;
    if (fstatat(dirfd, "relative", &st, 0) != 0 || !S_ISREG(st.st_mode))
        return 8;
    if (snprintf(absolute, sizeof(absolute), "%s/absolute", directory) < 0)
        return 9;
    result = bear_osif_syscall6(BEAR_OSIF_NR_openat, -1,
        (long)absolute, O_WRONLY | O_CREAT | O_EXCL, 0600, 0, 0);
    if (result < 0) return 10;
    if (bear_osif_syscall1(BEAR_OSIF_NR_close, result) != 0) return 11;
    if (unlinkat(dirfd, "relative", 0) != 0 ||
        unlink(absolute) != 0 || close(dirfd) != 0 || rmdir(directory) != 0)
        return 12;

    return 0;
}
