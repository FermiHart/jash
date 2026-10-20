// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/speculate — speculative execution sandbox (Wave 3 enhanced).
//
//   speculate CMD [ARGS...]       — run CMD in a throwaway copy of cwd, show diff
//   speculate --json CMD [ARGS...]— same, JSON output
//   speculate --keep CMD ...      — keep sandbox dir for inspection
//   speculate --timeout 5 CMD     — kill child after 5 seconds
//   speculate --quiet CMD         — suppress stdout/stderr capture in output
//
// See Docs/ADR-0026-agent-experience-wave.md (Tier 1: speculative execution)

#include "builtin.h"
#include <module/blake3.h>
#include <theme/io.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/resource.h>
#ifdef JASH_BLIBC
#include <sys/select.h>
#endif
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#include <osif/osinfo.h>
#include <sys/syscalls.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif
#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif
#if defined(__linux__)
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1U << 0)
#endif
#endif

#define SPEC_MAX_PATH 4096
#define SPEC_MAX_FILE_SIZE (16 * 1024 * 1024)
#define SPEC_CAPTURE_SIZE 8192

static int g_spec_timeout_sec = 0;
static int sp_legacy_dispatch(jash_state_t*, jash_writer_t, void*, const char*);

static void spec_json_esc(jash_writer_t w, void* ctx, const char* s) {
    if (!s) { jash_writes(w, ctx, "null"); return; }
    jash_writes(w, ctx, "\"");
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '\\': jash_writes(w, ctx, "\\\\"); break;
            case '"':  jash_writes(w, ctx, "\\\""); break;
            case '\n': jash_writes(w, ctx, "\\n"); break;
            case '\r': jash_writes(w, ctx, "\\r"); break;
            case '\t': jash_writes(w, ctx, "\\t"); break;
            default:
                if ((unsigned char)*p < 0x20) jash_writef(w, ctx, "\\u%04x", (unsigned char)*p);
                else jash_writef(w, ctx, "%c", *p);
                break;
        }
    }
    jash_writes(w, ctx, "\"");
}

static const char* spec_skip_dirs[] = {
    ".git", "build", "build-speed", "node_modules", ".cache", "target",
    ".venv", "__pycache__", "oci-export", ".Trash", "dist",
    "Quarentena-codes", "osif-macos-backup", NULL
};

static int spec_skip_dir(const char* name) {
    if (!name || !name[0]) return 1;
    if (name[0] == '.' && (name[1] == '\0' || (name[1] == '.' && name[2] == '\0'))) return 1;
    for (int i = 0; spec_skip_dirs[i]; i++)
        if (strcmp(name, spec_skip_dirs[i]) == 0) return 1;
    return 0;
}

static int spec_copy_file(const char* src, const char* dst) {
    struct stat st;
    if (stat(src, &st) < 0) return -1;
    if (st.st_size > SPEC_MAX_FILE_SIZE) return 0;
    int in = open(src, O_RDONLY);
    if (in < 0) return -1;
    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC, st.st_mode);
    if (out < 0) { close(in); return -1; }
    char buf[8192];
    ssize_t n;
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        ssize_t nw = write(out, buf, (size_t)n);
        if (nw != n) { close(in); close(out); return -1; }
    }
    close(in); close(out);
    return 0;
}

static int spec_copy_tree(const char* src, const char* dst) {
    struct stat st;
    if (stat(src, &st) < 0) return -1;
    if (S_ISDIR(st.st_mode)) {
        mkdir(dst, st.st_mode);
        DIR* d = opendir(src);
        if (!d) return -1;
        struct dirent* e;
        while ((e = readdir(d))) {
            if (spec_skip_dir(e->d_name)) continue;
            char schild[SPEC_MAX_PATH], dchild[SPEC_MAX_PATH];
            snprintf(schild, sizeof(schild), "%s/%s", src, e->d_name);
            snprintf(dchild, sizeof(dchild), "%s/%s", dst, e->d_name);
            spec_copy_tree(schild, dchild);
        }
        closedir(d);
    } else if (S_ISREG(st.st_mode)) {
        spec_copy_file(src, dst);
    }
    return 0;
}

static int spec_file_hash(const char* path, uint8_t out[32]) {
    struct blake3 hasher;
    blake3_init(&hasher);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[4096];
    ssize_t n;
    while ((n = read(fd, buf, sizeof(buf))) > 0)
        blake3_update(&hasher, buf, (uint64_t)n);
    close(fd);
    blake3_final(&hasher, out, 32);
    return 0;
}

static int spec_file_equal(const char* a, const char* b) {
    uint8_t ha[32], hb[32];
    if (spec_file_hash(a, ha) < 0) return 0;
    if (spec_file_hash(b, hb) < 0) return 0;
    return memcmp(ha, hb, 32) == 0;
}

static void spec_emit_path(jash_writer_t w, void* ctx, const char* rel) {
    jash_writef(w, ctx, "%s", rel[0] == '/' ? rel + 1 : rel);
}

static void spec_scan_diff(const char* orig, const char* copy, const char* rel,
                           jash_writer_t w, void* ctx,
                           int* created, int* modified, int* deleted) {
    char opath[SPEC_MAX_PATH], cpath[SPEC_MAX_PATH];
    if (rel[0]) {
        snprintf(opath, sizeof(opath), "%s%s", orig, rel);
        snprintf(cpath, sizeof(cpath), "%s%s", copy, rel);
    } else {
        snprintf(opath, sizeof(opath), "%s", orig);
        snprintf(cpath, sizeof(cpath), "%s", copy);
    }

    struct stat ost, cst;
    int oexists = stat(opath, &ost) == 0;
    int cexists = stat(cpath, &cst) == 0;

    if (!oexists && cexists) {
        if (S_ISREG(cst.st_mode)) {
            (*created)++;
            jash_writes(w, ctx, "  + "); spec_emit_path(w, ctx, rel); jash_writes(w, ctx, "\n");
        } else if (S_ISDIR(cst.st_mode)) {
            DIR* d = opendir(cpath);
            if (d) {
                struct dirent* e;
                while ((e = readdir(d))) {
                    if (spec_skip_dir(e->d_name)) continue;
                    char nrel[SPEC_MAX_PATH];
                    snprintf(nrel, sizeof(nrel), "%s/%s", rel, e->d_name);
                    spec_scan_diff(orig, copy, nrel, w, ctx, created, modified, deleted);
                }
                closedir(d);
            }
        }
        return;
    }
    if (oexists && !cexists) {
        (*deleted)++;
        jash_writes(w, ctx, "  - "); spec_emit_path(w, ctx, rel); jash_writes(w, ctx, "\n");
        return;
    }
    if (oexists && cexists && S_ISREG(ost.st_mode) && S_ISREG(cst.st_mode)) {
        if (!spec_file_equal(opath, cpath)) {
            (*modified)++;
            jash_writes(w, ctx, "  ~ "); spec_emit_path(w, ctx, rel); jash_writes(w, ctx, "\n");
        }
        return;
    }
    if (oexists && cexists && S_ISDIR(ost.st_mode) && S_ISDIR(cst.st_mode)) {
        char* names[4096];
        int nn = 0;
        DIR* d = opendir(opath);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) && nn < 4096) {
                if (spec_skip_dir(e->d_name)) continue;
                names[nn] = strdup(e->d_name);
                if (names[nn]) nn++;
            }
            closedir(d);
        }
        d = opendir(cpath);
        if (d) {
            struct dirent* e;
            while ((e = readdir(d)) && nn < 4096) {
                if (spec_skip_dir(e->d_name)) continue;
                int dup = 0;
                for (int i = 0; i < nn; i++) if (strcmp(names[i], e->d_name) == 0) { dup = 1; break; }
                if (dup) continue;
                names[nn] = strdup(e->d_name);
                if (names[nn]) nn++;
            }
            closedir(d);
        }
        for (int i = 0; i < nn; i++) {
            char nrel[SPEC_MAX_PATH];
            snprintf(nrel, sizeof(nrel), "%s/%s", rel, names[i]);
            spec_scan_diff(orig, copy, nrel, w, ctx, created, modified, deleted);
            free(names[i]);
        }
    }
}

/* Runs command in forked child with optional timeout, captures stdout/stderr,
 * and measures wall-clock time. */
static int spec_run_cmd_capture(char* const argv[], const char* cwd,
                                int* rc_out, uint64_t* elapsed_ms,
                                char* stdout_out, size_t stdout_cap,
                                char* stderr_out, size_t stderr_cap) {
    int stdout_pipe[2], stderr_pipe[2];
    if (pipe(stdout_pipe) < 0 || pipe(stderr_pipe) < 0) return -1;

    struct timespec t0 = {0}, t1 = {0};
    int have_start = clock_gettime(CLOCK_MONOTONIC, &t0) == 0;

    pid_t pid = fork();
    if (pid < 0) { close(stdout_pipe[0]); close(stdout_pipe[1]); close(stderr_pipe[0]); close(stderr_pipe[1]); return -1; }
    if (pid == 0) {
        close(stdout_pipe[0]);
        close(stderr_pipe[0]);
        dup2(stdout_pipe[1], 1);
        dup2(stderr_pipe[1], 2);
        close(stdout_pipe[1]);
        close(stderr_pipe[1]);

        if (cwd && chdir(cwd) != 0) _exit(126);

        if (g_spec_timeout_sec > 0) {
            alarm((unsigned int)g_spec_timeout_sec);
            struct rlimit rl = { .rlim_cur = g_spec_timeout_sec + 1, .rlim_max = g_spec_timeout_sec + 5 };
            setrlimit(RLIMIT_CPU, &rl);
        }

        execvp(argv[0], argv);
        _exit(127);
    }

    close(stdout_pipe[1]);
    close(stderr_pipe[1]);

    /* Read stdout/stderr with non-blocking poll.
     * BUG-046: at EOF, POSIX select() reports the fd as permanently readable
     * (that IS how EOF is signaled) while read() returns 0 — the old loop
     * only advanced on n>0 and never marked a drained fd as done, so once the
     * child exited it busy-spun forever (select ready -> read 0 -> repeat),
     * never reaching the "real" timeout below because select never actually
     * blocked. Track per-fd completion explicitly instead. */
    size_t sotal = 0, ertal = 0;
    fd_set rfds;
    struct timeval tv = { .tv_sec = g_spec_timeout_sec > 0 ? g_spec_timeout_sec + 2 : 30, .tv_usec = 0 };
    int stdout_done = (stdout_cap <= 1);
    int stderr_done = (stderr_cap <= 1);

    while (!stdout_done || !stderr_done) {
        FD_ZERO(&rfds);
        int maxfd = -1;
        if (!stdout_done && sotal < stdout_cap - 1) { FD_SET(stdout_pipe[0], &rfds); if (stdout_pipe[0] > maxfd) maxfd = stdout_pipe[0]; }
        if (!stderr_done && ertal < stderr_cap - 1) { FD_SET(stderr_pipe[0], &rfds); if (stderr_pipe[0] > maxfd) maxfd = stderr_pipe[0]; }
        if (maxfd < 0) break;   /* both buffers full — nothing left to wait on */

        struct timeval poll_tv = tv;
        int ret = select(maxfd + 1, &rfds, NULL, NULL, &poll_tv);
        if (ret <= 0) break;    /* real timeout or error */

        if (!stdout_done && FD_ISSET(stdout_pipe[0], &rfds)) {
            ssize_t n = read(stdout_pipe[0], stdout_out + sotal, stdout_cap - 1 - sotal);
            if (n > 0) sotal += (size_t)n;
            else stdout_done = 1;   /* EOF (0) or error (<0) */
        }
        if (!stderr_done && FD_ISSET(stderr_pipe[0], &rfds)) {
            ssize_t n = read(stderr_pipe[0], stderr_out + ertal, stderr_cap - 1 - ertal);
            if (n > 0) ertal += (size_t)n;
            else stderr_done = 1;
        }
    }
    stdout_out[sotal] = '\0';
    stderr_out[ertal] = '\0';

    close(stdout_pipe[0]);
    close(stderr_pipe[0]);

    int status;
    waitpid(pid, &status, 0);

    int have_end = clock_gettime(CLOCK_MONOTONIC, &t1) == 0;
    uint64_t start_ns = (uint64_t)t0.tv_sec * 1000000000ULL + (uint64_t)t0.tv_nsec;
    uint64_t end_ns = (uint64_t)t1.tv_sec * 1000000000ULL + (uint64_t)t1.tv_nsec;
    *elapsed_ms = have_start && have_end && end_ns >= start_ns ?
                  (end_ns - start_ns) / 1000000ULL : 0;

    if (WIFSIGNALED(status)) {
        int sig = WTERMSIG(status);
        *rc_out = 128 + sig;
    } else {
        *rc_out = WIFEXITED(status) ? WEXITSTATUS(status) : 1;
    }
    return 0;
}

static int spec_same_inode(const struct stat* left, const struct stat* right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static int spec_rename_noreplace(int dirfd, const char* from, const char* to) {
#if defined(JASH_SPEC_NOREPLACE_UNSUPPORTED)
    (void)dirfd;
    (void)from;
    (void)to;
    errno = ENOTSUP;
    return -1;
#elif defined(__linux__)
    return jash_osif_renameat2(dirfd, from, dirfd, to, RENAME_NOREPLACE);
#elif defined(__APPLE__)
    return renameatx_np(dirfd, from, dirfd, to, RENAME_EXCL);
#else
    (void)dirfd;
    (void)from;
    (void)to;
    /* An existence check followed by renameat() is not no-replace: a raced
     * entry would be overwritten. Unsupported platforms must fail closed. */
    errno = ENOTSUP;
    return -1;
#endif
}

/* Move an entry to a private name, then prove that the captured inode is the
 * one inspected by the caller. Mismatched replacements are restored or kept. */
static int spec_capture_owned(int parent_fd, const char* name,
                              const struct stat* owned,
                              char* hold, size_t hold_size) {
    static unsigned int sequence;
    struct stat current;
    hold[0] = '\0';
    if (fstatat(parent_fd, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        !spec_same_inode(&current, owned)) {
        errno = ESTALE;
        return -1;
    }
    for (int attempt = 0; attempt < 64; attempt++) {
        snprintf(hold, hold_size, ".jspec.%ld.%u", (long)getpid(), ++sequence);
        if (spec_rename_noreplace(parent_fd, name, hold) == 0) break;
        if (errno != EEXIST || attempt == 63) return -1;
    }
    if (fstatat(parent_fd, hold, &current, AT_SYMLINK_NOFOLLOW) == 0 &&
        spec_same_inode(&current, owned))
        return 0;
    if (spec_rename_noreplace(parent_fd, hold, name) == 0) hold[0] = '\0';
    errno = ESTALE;
    return -1;
}

static void spec_hold_path(const char* path, const char* hold,
                           char* out, size_t out_size) {
    const char* slash = strrchr(path, '/');
    if (!slash)
        snprintf(out, out_size, "%s", hold);
    else if (slash == path)
        snprintf(out, out_size, "/%s", hold);
    else
        snprintf(out, out_size, "%.*s/%s", (int)(slash - path), path, hold);
}

typedef struct { size_t remaining; unsigned max_depth; } spec_cleanup_limits_t;
static int spec_rm_rf_at_checked(int parent_fd, const char* name, const char* path,
                         char* retained, size_t retained_size,
                         spec_cleanup_limits_t* limits, unsigned depth) {
    if (limits && (!limits->remaining || depth > limits->max_depth)) {
        snprintf(retained, retained_size, "%s", path);
        errno = E2BIG;
        return -1;
    }
    if (limits) limits->remaining--;
    struct stat st;
    if (fstatat(parent_fd, name, &st, AT_SYMLINK_NOFOLLOW) < 0) {
        snprintf(retained, retained_size, "%s", path);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        char hold[64] = "";
        if (spec_capture_owned(parent_fd, name, &st, hold, sizeof(hold)) != 0) {
            if (hold[0]) spec_hold_path(path, hold, retained, retained_size);
            else snprintf(retained, retained_size, "%s", path);
            return -1;
        }
        if (unlinkat(parent_fd, hold, 0) == 0) return 0;
        spec_hold_path(path, hold, retained, retained_size);
        return -1;
    }

    int fd = openat(parent_fd, name,
                    O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) {
        snprintf(retained, retained_size, "%s", path);
        return -1;
    }
    struct stat opened;
    if (fstat(fd, &opened) != 0 || !spec_same_inode(&opened, &st)) {
        close(fd);
        snprintf(retained, retained_size, "%s", path);
        return -1;
    }
    DIR* d = fdopendir(fd);
    if (!d) { close(fd); return -1; }
    int failed = 0, saved_error = 0;
    for (;;) {
        errno = 0;
        struct dirent* e = readdir(d);
        if (!e) { if (errno) { failed = 1; saved_error = errno; } break; }
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        char child[SPEC_MAX_PATH];
        int length = snprintf(child, sizeof(child), "%s/%s", path, e->d_name);
        if (length < 0 || (size_t)length >= sizeof(child)) {
            failed = 1; saved_error = ENAMETOOLONG; break;
        }
        if (spec_rm_rf_at_checked(fd, e->d_name, child, retained,
                          retained_size, limits, depth + 1u) != 0) {
            failed = 1; saved_error = errno;
            if (limits) break;
        }
    }
    if (closedir(d) != 0) { failed = 1; saved_error = errno; }
    if (failed) {
        snprintf(retained, retained_size, "%s", path);
        errno = saved_error ? saved_error : EIO;
        return -1;
    }
    char hold[64] = "";
    if (spec_capture_owned(parent_fd, name, &opened,
                           hold, sizeof(hold)) != 0) {
        if (hold[0]) spec_hold_path(path, hold, retained, retained_size);
        else snprintf(retained, retained_size, "%s", path);
        return -1;
    }
    if (unlinkat(parent_fd, hold, AT_REMOVEDIR) != 0) {
        spec_hold_path(path, hold, retained, retained_size);
        return -1;
    }
    if (!failed) {
        struct stat replacement;
        if (fstatat(parent_fd, name, &replacement, AT_SYMLINK_NOFOLLOW) == 0 ||
            errno != ENOENT) {
            errno = ESTALE;
            snprintf(retained, retained_size, "%s", path);
            failed = 1;
        }
    }
    return failed ? -1 : 0;
}

static int spec_rm_rf_at(int parent_fd, const char* name, const char* path,
                         char* retained, size_t retained_size) {
    return spec_rm_rf_at_checked(parent_fd, name, path, retained,
                                 retained_size, NULL, 0);
}

static int spec_rm_rf(const char* path, char* retained, size_t retained_size) {
    retained[0] = '\0';
    char parent[SPEC_MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", path);
    char* slash = strrchr(parent, '/');
    if (!slash || !slash[1]) {
        snprintf(retained, retained_size, "%s", path);
        return -1;
    }
    char name[SPEC_MAX_PATH];
    snprintf(name, sizeof(name), "%s", slash + 1);
    if (slash == parent) slash[1] = '\0';
    else *slash = '\0';

    int parent_fd = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (parent_fd < 0) {
        snprintf(retained, retained_size, "%s", path);
        return -1;
    }
    int rc = spec_rm_rf_at(parent_fd, name, path, retained, retained_size);
    close(parent_fd);
    if (rc != 0 && !retained[0])
        snprintf(retained, retained_size, "%s", path);
    return rc;
}

static int spec_parse_args(const char* args, int* json_mode, int* keep,
                           int* quiet, char* cmdbuf, size_t cmdbufs) {
    const char* a = args ? args : "";
    while (*a == ' ' || *a == '\t') a++;

    *json_mode = 0;
    *keep = 0;
    *quiet = 0;
    g_spec_timeout_sec = 0;

    while (1) {
        if (strncmp(a, "--json", 6) == 0 && (a[6] == '\0' || a[6] == ' ' || a[6] == '\t')) {
            *json_mode = 1;
            a += 6;
        } else if (strncmp(a, "--keep", 6) == 0 && (a[6] == '\0' || a[6] == ' ' || a[6] == '\t')) {
            *keep = 1;
            a += 6;
        } else if (strncmp(a, "--quiet", 7) == 0 && (a[7] == '\0' || a[7] == ' ' || a[7] == '\t')) {
            *quiet = 1;
            a += 7;
        } else if (strncmp(a, "--timeout", 9) == 0 && (a[9] == '\0' || a[9] == ' ' || a[9] == '\t')) {
            a += 9;
            while (*a == ' ' || *a == '\t') a++;
            if (*a < '0' || *a > '9') return -1;
            unsigned timeout = 0;
            while (*a >= '0' && *a <= '9') {
                if (timeout > (3600u - (unsigned)(*a - '0')) / 10u) return -1;
                timeout = timeout * 10u + (unsigned)(*a++ - '0');
            }
            if (*a && *a != ' ' && *a != '\t') return -1;
            g_spec_timeout_sec = (int)timeout;
        } else {
            break;
        }
        while (*a == ' ' || *a == '\t') a++;
    }

    if (!*a) return -1;
    snprintf(cmdbuf, cmdbufs, "%s", a);
    return 0;
}

static int spec_tokenize_cmd(char* cmd, char* argv[], int maxargv) {
    int argc = 0;
    char* p = cmd;
    while (*p && argc < maxargv - 1) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        argv[argc++] = p;
        while (*p && *p != ' ' && *p != '\t') p++;
        if (*p) { *p = '\0'; p++; }
    }
    argv[argc] = NULL;
    return argc;
}

int jash_b_speculate(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int native = sp_legacy_dispatch(s, w, ctx, args);
    if (native >= 0) return native;
    char cmdbuf[4096];
    int json_mode, keep, quiet;
    if (spec_parse_args(args, &json_mode, &keep, &quiet, cmdbuf, sizeof(cmdbuf)) < 0) {
        jash_writes(w, ctx, "speculate: need a command to run\n");
        return 1;
    }

    char* argv[64];
    if (spec_tokenize_cmd(cmdbuf, argv, 64) == 0) {
        jash_writes(w, ctx, "speculate: empty command\n");
        return 1;
    }

    /* ── O sandbox é uma CÓPIA da cwd, não um namespace ────────────────────
     * `spec_copy_tree(cwd, tmpdir)` + chdir. Sem chroot e sem sandbox de
     * kernel, um argumento com caminho ABSOLUTO (ou `..`) atravessa a cópia e
     * atinge o disco real — a "simulação" EXECUTA.
     *
     * Medido em 2026-07-28, nos DOIS binários (instalado e build do repo):
     *   speculate touch /abs/novo.txt → novo.txt CRIADO no disco real,
     *                                   e o diff dizia "- novo.txt / 1 deleted"
     *   speculate rm  /abs/alvo.txt   → alvo.txt APAGADO de verdade,
     *                                   e o diff dizia "+ alvo.txt / 1 created"
     * A inversão é consequência, não outro bug: o efeito caiu FORA da cópia,
     * então comparar cópia × real dá o sinal trocado. Com caminho RELATIVO os
     * dois casos se comportam certo e o disco fica intacto.
     *
     * Apagar o arquivo de verdade e reportar "created" é o pior resultado
     * possível para uma primitiva cujo contrato é "mostro sem fazer". Enquanto
     * o isolamento for cópia de diretório, recusar é a única resposta honesta.
     * Gate: tests/test_speculate_scope.sh */
    for (int i = 0; argv[i]; i++) {
        const char* a = argv[i];
        int absoluto = (a[0] == '/');
        int sobe = (strncmp(a, "../", 3) == 0) || (strstr(a, "/../") != NULL) ||
                   (strcmp(a, "..") == 0);
        if (!absoluto && !sobe) continue;
        char msg[SPEC_MAX_PATH + 320];
        snprintf(msg, sizeof(msg),
                 "speculate: recuso — '%s' sai da copia da cwd.\n"
                 "  O sandbox e uma copia de diretorio (sem chroot): este comando\n"
                 "  EXECUTARIA de verdade e o diff sairia invertido.\n"
                 "  Rode de dentro do diretorio com caminho relativo, ou use\n"
                 "  `explain` para ver os passos sem executar.\n", a);
        jash_writes(w, ctx, msg);
        return 2;
    }

    char orig[SPEC_MAX_PATH];
    if (!getcwd(orig, sizeof(orig))) {
        jash_writes(w, ctx, "speculate: cannot determine cwd\n");
        return 1;
    }

    char tmpdir[SPEC_MAX_PATH];
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(tmpdir, sizeof(tmpdir), "%s/.jash/speculate/%d", home, (int)getpid());
    mkdir(tmpdir, 0700);
    char* last = strrchr(tmpdir, '/');
    if (last) {
        *last = '\0';
        mkdir(tmpdir, 0700);
        *last = '/';
    }

    spec_copy_tree(orig, tmpdir);

    int rc = 0;
    uint64_t elapsed_ms = 0;
    char captured_stdout[SPEC_CAPTURE_SIZE] = "";
    char captured_stderr[SPEC_CAPTURE_SIZE] = "";
    if (spec_run_cmd_capture(argv, tmpdir, &rc, &elapsed_ms,
                             captured_stdout, sizeof(captured_stdout),
                             captured_stderr, sizeof(captured_stderr)) < 0) {
        jash_writes(w, ctx, "speculate: failed to run command\n");
        char retained[SPEC_MAX_PATH];
        if (spec_rm_rf(tmpdir, retained, sizeof(retained)) != 0)
            jash_writef(w, ctx,
                        "speculate: cleanup failed; sandbox retained at '%s'\n",
                        retained);
        return 1;
    }

    int created = 0, modified = 0, deleted = 0;
    int cleanup_failed = 0;
    char retained[SPEC_MAX_PATH] = "";

    /* JSON must describe the completed cleanup state, not a promised future
     * state. Text mode needs the tree for its streaming diff and cleans below. */
    if (json_mode && !keep &&
        spec_rm_rf(tmpdir, retained, sizeof(retained)) != 0)
        cleanup_failed = 1;

    if (json_mode) {
        jash_writes(w, ctx, "{\n");
        jash_writef(w, ctx, "  \"rc\": %d,\n", rc);
        jash_writef(w, ctx, "  \"elapsed_ms\": %llu,\n", (unsigned long long)elapsed_ms);
        jash_writef(w, ctx, "  \"sandbox\": ");
        /* INV-867: two local sandbox roots retain distinct call origins. */
        if (cleanup_failed) spec_json_esc(w, ctx, retained);
        else if (keep) spec_json_esc(w, ctx, tmpdir);
        else spec_json_esc(w, ctx, NULL);
        jash_writes(w, ctx, ",\n");
        jash_writef(w, ctx, "  \"cleanup\": \"%s\",\n",
                    keep ? "kept" : cleanup_failed ? "retained" : "removed");
        jash_writes(w, ctx, "  \"created\": [\n");
        jash_writes(w, ctx, "  ],\n");
        jash_writes(w, ctx, "  \"modified\": [],\n");
        jash_writes(w, ctx, "  \"deleted\": [],\n");
        jash_writef(w, ctx, "  \"created_count\": 0,\n");
        jash_writef(w, ctx, "  \"modified_count\": 0,\n");
        jash_writef(w, ctx, "  \"deleted_count\": 0,\n");
        jash_writef(w, ctx, "  \"timeout\": %d,\n", g_spec_timeout_sec);
        /* The child has already exited, so its sockets cannot be observed here.
         * `false` was a plausible lie and lsof on the dead PID could hang. */
        jash_writes(w, ctx, "  \"network\": null,\n");
        jash_writef(w, ctx, "  \"stdout\": "); spec_json_esc(w, ctx, captured_stdout); jash_writes(w, ctx, ",\n");
        jash_writef(w, ctx, "  \"stderr\": "); spec_json_esc(w, ctx, captured_stderr); jash_writes(w, ctx, "\n");
        jash_writes(w, ctx, "}\n");
    } else {
        jash_writef(w, ctx, "\033[1;36m# speculate: %s\033[0m\n", cmdbuf);
        jash_writef(w, ctx, "\033[90m# rc=%d  runtime=%llums  sandbox=%s",
                    rc, (unsigned long long)elapsed_ms, tmpdir);
        if (g_spec_timeout_sec > 0) jash_writef(w, ctx, "  timeout=%ds", g_spec_timeout_sec);
        jash_writes(w, ctx, "\033[0m\n");

        if (!quiet && captured_stdout[0]) {
            jash_writes(w, ctx, "\033[1mstdout:\033[0m\n");
            jash_writes(w, ctx, captured_stdout);
            if (captured_stdout[strlen(captured_stdout)-1] != '\n') jash_writes(w, ctx, "\n");
        }
        if (!quiet && captured_stderr[0]) {
            jash_writes(w, ctx, "\033[1mstderr:\033[0m\n");
            jash_writes(w, ctx, captured_stderr);
            if (captured_stderr[strlen(captured_stderr)-1] != '\n') jash_writes(w, ctx, "\n");
        }

        jash_writes(w, ctx, "\033[1mdiff:\033[0m\n");
        spec_scan_diff(orig, tmpdir, "", w, ctx, &created, &modified, &deleted);
        jash_writef(w, ctx, "\n\033[90m# %d created, %d modified, %d deleted\033[0m\n",
                    created, modified, deleted);
        if (!keep && spec_rm_rf(tmpdir, retained, sizeof(retained)) != 0)
            cleanup_failed = 1;
        if (keep)
            jash_writef(w, ctx, "# sandbox kept at '%s'\n", tmpdir);
        else if (cleanup_failed)
            jash_writef(w, ctx,
                        "speculate: cleanup failed; sandbox retained at '%s'\n",
                        retained);
        else
            jash_writef(w, ctx, "# sandbox removed: '%s'\n", tmpdir);
    }

    return cleanup_failed ? 1 : 0;
}

#include "speculate_native.inc"
