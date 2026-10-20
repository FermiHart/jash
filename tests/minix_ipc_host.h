#ifndef JASH_TEST_MINIX_IPC_HOST_H
#define JASH_TEST_MINIX_IPC_HOST_H

#include <sys/types.h>

int getpeereid(int socket, uid_t *euid, gid_t *egid);

#endif
