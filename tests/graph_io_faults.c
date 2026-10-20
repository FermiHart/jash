// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdio.h>
static int published;
int renameat(int a, const char* from, int b, const char* to) {
    int (*next)(int,const char*,int,const char*) = dlsym(RTLD_NEXT, "renameat");
    const char* mode = getenv("JASH_TEST_GRAPH_FAULT");
    int graph = !strncmp(from, ".graph-", 7) && !strncmp(to, "graph", 5);
    if (graph && mode && !strcmp(mode, "rename")) { errno = EIO; return -1; }
    int result = next(a, from, b, to); if (!result && graph) published = 1;
    return result;
}
int fsync(int fd) {
    int (*next)(int) = dlsym(RTLD_NEXT, "fsync");
    const char* mode = getenv("JASH_TEST_GRAPH_FAULT"); struct stat st;
    if (published && mode && !strcmp(mode, "directory-fsync") && !fstat(fd,&st) && S_ISDIR(st.st_mode)) {
        errno = EIO; return -1;
    }
    return next(fd);
}
