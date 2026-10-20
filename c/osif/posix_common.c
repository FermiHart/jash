/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   posix_common.c — OSIF L1 helpers shared by every POSIX-like target
 *
 *   spawn_capture, dirent_kind, rmtree: identical on macOS, Linux,
 *   MINIX 3, BeyondBSD. Targets with non-POSIX process models (Josh)
 *   provide their own versions instead of this file.
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#include "osinfo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <dirent.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/time.h>

#if defined(__linux__)
#include <sys/syscall.h>

extern int renameat2(int olddirfd, const char* oldpath,
                     int newdirfd, const char* newpath,
                     unsigned int flags) __attribute__((weak));
#endif

int jash_osif_renameat2(int olddirfd, const char* oldpath,
                        int newdirfd, const char* newpath, unsigned int flags)
{
#if defined(__linux__)
    if (renameat2) {
        int rc = renameat2(olddirfd, oldpath, newdirfd, newpath, flags);
        if (rc == 0 || errno != ENOSYS) return rc;
    }
#if defined(SYS_renameat2)
    return (int)syscall(SYS_renameat2, olddirfd, oldpath,
                        newdirfd, newpath, flags);
#else
    (void)olddirfd; (void)oldpath; (void)newdirfd; (void)newpath; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
#else
    (void)olddirfd; (void)oldpath; (void)newdirfd; (void)newpath; (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

/* ── spawn + capture with timeout (no shell, no injection) ──────────────── */

int bear_osinfo_spawn_capture(const char* const argv[],
                              const char* workdir,
                              char* out, int outsz,
                              int timeout_ms, int* exit_code)
{
    if (!argv || !argv[0] || !out || outsz < 2) return -1;
    out[0] = '\0';

    int pipefd[2];
    if (pipe(pipefd) < 0) return -1;

    pid_t pid = fork();
    if (pid < 0) { close(pipefd[0]); close(pipefd[1]); return -1; }

    if (pid == 0) {
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        if (workdir && *workdir) {
            if (chdir(workdir) != 0) _exit(126);
        }
        execv(argv[0], (char* const*)argv);
        _exit(127);
    }

    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);

    int total = 0;
    struct timeval start, now;
    gettimeofday(&start, NULL);

    for (;;) {
        ssize_t n = read(pipefd[0], out + total, (size_t)(outsz - total - 1));
        if (n > 0) {
            total += (int)n;
            if (total >= outsz - 1) break;
        } else if (n == 0) {
            break;                            /* EOF: child closed */
        } else {
            gettimeofday(&now, NULL);
            long elapsed = (now.tv_sec - start.tv_sec) * 1000L +
                           (now.tv_usec - start.tv_usec) / 1000L;
            if (elapsed > timeout_ms) {
                kill(pid, SIGKILL);
                close(pipefd[0]);
                out[total] = '\0';
                waitpid(pid, NULL, 0);
                return -2;                    /* timeout */
            }
            usleep(5000);
        }
    }
    out[total] = '\0';
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (exit_code)
        *exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return 0;
}

/* ── d_type with stat() fallback ─────────────────────────────────────────── */

int osif_dirent_kind(const char* dir, const struct dirent* ent)
{
#ifdef DT_DIR
    if (ent->d_type == DT_DIR) return OSIF_KIND_DIR;
    if (ent->d_type == DT_REG) return OSIF_KIND_REG;
    if (ent->d_type != DT_UNKNOWN) return OSIF_KIND_OTHER;
#endif
    /* DT_UNKNOWN or no d_type at all: ask stat. */
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
    struct stat st;
    if (lstat(path, &st) != 0) return OSIF_KIND_OTHER;
    if (S_ISDIR(st.st_mode)) return OSIF_KIND_DIR;
    if (S_ISREG(st.st_mode)) return OSIF_KIND_REG;
    return OSIF_KIND_OTHER;
}

/* ── pure-syscall rm -rf ─────────────────────────────────────────────────── */

int osif_rmtree(const char* path)
{
    struct stat st;
    if (lstat(path, &st) != 0) return 0;     /* already gone — success */

    if (!S_ISDIR(st.st_mode))
        return unlink(path);

    DIR* d = opendir(path);
    if (!d) return -1;

    struct dirent* ent;
    int rc = 0;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
            continue;
        char child[1024];
        snprintf(child, sizeof(child), "%s/%s", path, ent->d_name);
        if (osif_rmtree(child) != 0) rc = -1;
    }
    closedir(d);
    if (rmdir(path) != 0) rc = -1;
    return rc;
}
