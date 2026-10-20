/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#ifndef JASH_MINIX_BEAR_POLL_H
#define JASH_MINIX_BEAR_POLL_H

typedef unsigned long bear_nfds_t;

struct bear_pollfd {
    int fd;
    short events;
    short revents;
};

#define BEAR_POLLIN     0x0001
#define BEAR_POLLPRI    0x0002
#define BEAR_POLLOUT    0x0004
#define BEAR_POLLERR    0x0008
#define BEAR_POLLHUP    0x0010
#define BEAR_POLLNVAL   0x0020
#define BEAR_POLLRDNORM 0x0040
#define BEAR_POLLRDBAND 0x0080
#define BEAR_POLLWRNORM 0x0100
#define BEAR_POLLWRBAND 0x0200
#define BEAR_POLLRDHUP  0x2000

int bear_poll(struct bear_pollfd *fds, bear_nfds_t nfds, int timeout_ms);

#endif
