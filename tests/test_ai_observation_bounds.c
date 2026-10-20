// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#include "core/ai_observation.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define CHECK(condition) do { if (!(condition)) { \
    fprintf(stderr, "observation bound failed at %d: %s (errno %d)\n", \
            __LINE__, #condition, errno); return 1; \
} } while (0)

static size_t descriptors(void) {
    DIR* directory = opendir("/proc/self/fd");
    if (!directory) abort();
    size_t count = 0;
    struct dirent* entry;
    while ((entry = readdir(directory)))
        if (strcmp(entry->d_name, ".") && strcmp(entry->d_name, "..")) count++;
    if (closedir(directory)) abort();
    return count;
}

int main(void) {
    char root[] = "/tmp/jash-ai-observation-limit-XXXXXX";
    CHECK(mkdtemp(root) != NULL);
    char path[512], out[4096];
    size_t baseline = descriptors();
    CHECK(jash_ai_observe_ls(root, out, sizeof(out)) == 0);
    CHECK(strstr(out, "\"observation_ref\":\"o:") != NULL);
    CHECK(descriptors() == baseline + 1); /* one sealed snapshot FD */
    for (int i = 0; i < 30; i++) {
        CHECK(snprintf(path, sizeof(path), "%s/item-%02d", root, i) > 0);
        int fd = open(path, O_CREAT | O_EXCL | O_WRONLY, 0600);
        CHECK(fd >= 0 && close(fd) == 0);
    }
    for (int attempt = 0; attempt < 3; attempt++) {
        strcpy(out, "dirty");
        CHECK(jash_ai_observe_ls(root, out, sizeof(out)) < 0);
        CHECK(errno == E2BIG && out[0] == '\0');
        CHECK(descriptors() == baseline + 1);
    }
    jash_ai_observation_forget();
    CHECK(descriptors() == baseline);
    for (int i = 0; i < 30; i++) {
        CHECK(snprintf(path, sizeof(path), "%s/item-%02d", root, i) > 0);
        CHECK(unlink(path) == 0);
    }
    CHECK(rmdir(root) == 0);
    puts("ai observation bounds: ok");
    return 0;
}
