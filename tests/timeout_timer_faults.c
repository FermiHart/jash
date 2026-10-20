// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

static int selected(const char* name) {
    const char* mode = getenv("JASH_TEST_TIMER_FAILURE");
    return mode && !strcmp(mode, name);
}

int getitimer(__itimer_which_t which, struct itimerval* value) {
    if (which == ITIMER_REAL && selected("get")) { errno = EIO; return -1; }
    int (*real_fn)(__itimer_which_t, struct itimerval*) = dlsym(RTLD_NEXT, "getitimer");
    return real_fn(which, value);
}

int setitimer(__itimer_which_t which, const struct itimerval* value, struct itimerval* old) {
    if (which == ITIMER_REAL && value && (value->it_value.tv_sec || value->it_value.tv_usec) && selected("arm")) {
        errno = EIO; return -1;
    }
    int (*real_fn)(__itimer_which_t, const struct itimerval*, struct itimerval*) = dlsym(RTLD_NEXT, "setitimer");
    return real_fn(which, value, old);
}

int sigaction(int signal_number, const struct sigaction* action, struct sigaction* old) {
    if (signal_number == SIGALRM && action && action->sa_handler != SIG_DFL &&
        action->sa_handler != SIG_IGN && selected("handler")) { errno = EIO; return -1; }
    int (*real_fn)(int, const struct sigaction*, struct sigaction*) = dlsym(RTLD_NEXT, "sigaction");
    return real_fn(signal_number, action, old);
}
