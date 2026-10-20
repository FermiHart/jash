/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
/* Adapt Jash's stable poll ABI to the native MINIX libc provider. */
#ifdef __minix
#include <sys/poll.h>
#else
#include <poll.h>
#endif
#include <errno.h>
#include <stdlib.h>

#include "bear_poll.h"

extern int *__errno(void);

static short
bear_events_to_minix(short events)
{
    short native_events = 0;

    if (events & BEAR_POLLIN)
        native_events |= POLLIN;
    if (events & BEAR_POLLPRI)
        native_events |= POLLPRI;
    if (events & BEAR_POLLOUT)
        native_events |= POLLOUT;
    if (events & BEAR_POLLRDNORM)
        native_events |= POLLRDNORM;
    if (events & BEAR_POLLRDBAND)
        native_events |= POLLRDBAND;
    if (events & BEAR_POLLWRNORM)
        native_events |= POLLWRNORM;
    if (events & BEAR_POLLWRBAND)
        native_events |= POLLWRBAND;

    return native_events;
}

static short
minix_revents_to_bear(short native_revents, short requested)
{
    short revents = 0;

    if (native_revents & POLLIN) {
        revents |= requested & BEAR_POLLIN;
        revents |= requested & BEAR_POLLRDNORM;
    }
    if (native_revents & POLLRDNORM)
        revents |= requested & BEAR_POLLRDNORM;
    if (native_revents & POLLWRNORM)
        revents |= requested & BEAR_POLLWRNORM;
    if (native_revents & POLLPRI)
        revents |= requested & BEAR_POLLPRI;
    if (native_revents & POLLOUT) {
        revents |= requested & BEAR_POLLOUT;
        revents |= requested & BEAR_POLLWRNORM;
    }
    if (native_revents & POLLRDBAND)
        revents |= requested & BEAR_POLLRDBAND;
    if (native_revents & POLLWRBAND)
        revents |= requested & BEAR_POLLWRBAND;
    if (native_revents & POLLERR)
        revents |= BEAR_POLLERR;
    if (native_revents & POLLHUP)
        revents |= BEAR_POLLHUP;
    if (native_revents & POLLNVAL)
        revents |= BEAR_POLLNVAL;

    return revents;
}

int
bear_poll(struct bear_pollfd *fds, bear_nfds_t nfds, int timeout_ms)
{
    struct pollfd *native_fds;
    bear_nfds_t i;
    int rc, saved_errno;

    if (nfds > (bear_nfds_t)((nfds_t)-1) ||
        nfds > ((bear_nfds_t)-1) / sizeof(*native_fds)) {
        *__errno() = EINVAL;
        return -1;
    }
    if (timeout_ms < 0)
        timeout_ms = -1;
    if (nfds == 0)
        return poll(NULL, 0, timeout_ms);

    native_fds = malloc((size_t)nfds * sizeof(*native_fds));
    if (native_fds == NULL)
        return -1;

    for (i = 0; i < nfds; i++) {
        fds[i].revents = 0;
        if (fds[i].events & BEAR_POLLRDHUP) {
            free(native_fds);
            *__errno() = EOPNOTSUPP;
            return -1;
        }
        native_fds[i].fd = fds[i].fd;
        native_fds[i].events = bear_events_to_minix(fds[i].events);
        native_fds[i].revents = 0;
    }

    rc = poll(native_fds, (nfds_t)nfds, timeout_ms);
    saved_errno = *__errno();
    for (i = 0; i < nfds; i++)
        fds[i].revents = minix_revents_to_bear(native_fds[i].revents,
            fds[i].events);
    free(native_fds);
    *__errno() = saved_errno;
    return rc;
}
