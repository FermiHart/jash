// SPDX-License-Identifier: LicenseRef-SPL-1.0
#define _GNU_SOURCE
#define JASH_AGENT_ONLY
#include <unistd.h>
#include <sys/wait.h>
#include <assert.h>
#include <dirent.h>
static int pause_copy, ready_fd, release_fd;
static ssize_t gated_write(int fd, const void* data, size_t size);
#define write gated_write
#include "../c/core/serve.c"
#undef write

static ssize_t gated_write(int fd, const void* data, size_t size) {
    if (pause_copy) {
        pause_copy = 0;
        assert(write(ready_fd, "R", 1) == 1);
        char byte; if (read(release_fd, &byte, 1) != 1) _exit(42);
    }
    return write(fd, data, size);
}

int main(int argc, char** argv) {
    assert(argc == 2 && !chdir(argv[1]) && !setenv("HOME", argv[1], 1));
    const char data[] = "concurrent output\n";
    int source = open("source", O_WRONLY | O_CREAT | O_EXCL, 0600);
    assert(source >= 0 && write(source, data, sizeof(data) - 1) == sizeof(data) - 1); close(source);
    unsigned char digest[32]; char hash[JASH_BLAKE3_HEX_LEN], ref[160];
    jash_blake3(data, sizeof(data) - 1, digest); jash_blake3_hex(digest, hash);
    int ready[2], release[2]; assert(!pipe(ready) && !pipe(release));
    pid_t child = fork(); assert(child >= 0);
    if (!child) {
        close(ready[0]); close(release[1]); ready_fd = ready[1]; release_fd = release[0]; pause_copy = 1;
        int rc = srv_cas_publish("source", sizeof(data) - 1, hash, ref, sizeof(ref));
        _exit(rc ? 1 : 0);
    }
    close(ready[1]); close(release[0]); char byte;
    assert(read(ready[0], &byte, 1) == 1);
    int second = srv_cas_publish("source", sizeof(data) - 1, hash, ref, sizeof(ref));
    assert(write(release[1], "G", 1) == 1); close(release[1]); close(ready[0]);
    int status; assert(waitpid(child, &status, 0) == child);
    assert(second == 0 && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    char path[256]; snprintf(path, sizeof(path), ".jash/overflow/%s", ref);
    assert(!srv_cas_verify(path, sizeof(data) - 1, hash));
    jash_blake3("missing", 7, digest); jash_blake3_hex(digest, hash);
    assert(srv_cas_publish("missing-source", 7, hash, ref, sizeof(ref)) != 0);
    DIR* directory = opendir(".jash/overflow"); assert(directory);
    struct dirent* entry;
    while ((entry = readdir(directory))) assert(strncmp(entry->d_name, ".cas-", 5));
    closedir(directory);
    return 0;
}
