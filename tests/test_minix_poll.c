/* Host behavior test for Jash's MINIX poll ABI adapter. */
#include "../c/osif/minix/bear_poll.h"

#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <unistd.h>

int *__errno(void)
{
    return &errno;
}

int main(void)
{
    int pipefd[2];
    struct bear_pollfd readfd, writefd;
    int rc;

    if (pipe(pipefd) != 0)
        return 1;
    readfd.fd = pipefd[0];
    readfd.events = BEAR_POLLIN;
    readfd.revents = 0;
    rc = bear_poll(&readfd, 1, 0);
    if (rc != 0 || readfd.revents != 0)
        return 2;
    if (write(pipefd[1], "x", 1) != 1)
        return 3;
    rc = bear_poll(&readfd, 1, -5);
    if (rc != 1 || (readfd.revents & BEAR_POLLIN) == 0)
        return 4;
    readfd.events = BEAR_POLLRDNORM;
    readfd.revents = 0;
    rc = bear_poll(&readfd, 1, 0);
    if (rc != 1 || (readfd.revents & BEAR_POLLRDNORM) == 0)
        return 5;
    writefd.fd = pipefd[1];
    writefd.events = BEAR_POLLWRNORM;
    writefd.revents = 0;
    rc = bear_poll(&writefd, 1, 0);
    if (rc != 1 || (writefd.revents & BEAR_POLLWRNORM) == 0) {
        fprintf(stderr, "write readiness rc=%d revents=%#x\n",
            rc, (unsigned short)writefd.revents);
        return 6;
    }
    if (bear_poll(NULL, 0, 0) != 0)
        return 7;
    errno = 0;
    if (bear_poll(NULL, (bear_nfds_t)-1, 0) != -1 ||
        errno != EINVAL)
        return 8;
    if (close(pipefd[0]) != 0 || close(pipefd[1]) != 0)
        return 9;
    return 0;
}
