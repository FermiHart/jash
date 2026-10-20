/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#define _GNU_SOURCE
#include <builtins/memory_store.h>
#include <assert.h>
#include <signal.h>
#include <sys/wait.h>

typedef struct { int polls, status; } control_t;
static int cancel(void* context) {
    control_t* control = context;
    if (++control->polls >= 3) control->status = 143;
    return control->status;
}
static int ready(void* context) { (void)context; return 0; }

int main(void) {
    char directory[] = "/tmp/jash-memory-cooperative-XXXXXX";
    assert(mkdtemp(directory)); assert(!setenv("HOME", directory, 1));
    memory_store_t store;
    assert(!memory_store_open(&store, NULL, 1));
    assert(!memory_store_close(&store));
    int start[2], stop[2]; assert(!pipe(start)); assert(!pipe(stop));
    pid_t owner = fork(); assert(owner >= 0);
    if (!owner) {
        close(start[0]); close(stop[1]);
        memory_store_t held;
        if (memory_store_open(&held, NULL, 1)) _exit(1);
        if (write(start[1], "x", 1) != 1) _exit(2);
        char byte; if (read(stop[0], &byte, 1) != 1) _exit(3);
        _exit(memory_store_close(&held) ? 4 : 0);
    }
    close(start[1]); close(stop[0]); char byte;
    assert(read(start[0], &byte, 1) == 1);
    control_t control = {0};
    assert(memory_store_open_cooperative(&store, NULL, 1, cancel, &control) == -1);
    assert(errno == ECANCELED && control.status == 143 && control.polls == 3);
    assert(store.directory == -1 && store.lock == -1);
    assert(!kill(owner, 0)); /* cancellation must not abort the lock owner */
    char path[PATH_MAX]; snprintf(path, sizeof(path), "%s/.jash/.memory-lock", directory);
    int fd = open(path, O_RDWR | O_CLOEXEC); assert(fd >= 0);
    struct flock held = {.l_type = F_WRLCK, .l_whence = SEEK_SET};
    assert(!fcntl(fd, F_GETLK, &held) && held.l_type == F_WRLCK && held.l_pid == owner);
    assert(!close(fd));
    assert(write(stop[1], "x", 1) == 1);
    int status; assert(waitpid(owner, &status, 0) == owner && WIFEXITED(status) && WEXITSTATUS(status) == 0);
    assert(!memory_store_open_cooperative(&store, NULL, 1, ready, NULL));
    assert(!memory_store_close(&store));
    close(start[0]); close(stop[1]);
    assert(!unlink(path)); snprintf(path, sizeof(path), "%s/.jash", directory);
    assert(!rmdir(path)); assert(!rmdir(directory));
    return 0;
}
