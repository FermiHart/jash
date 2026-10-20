// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/assert — uma afirmação amarrada ao seu verificador.
//
// JINV-055 (Onda B1 do PLANO-10-ONDAS-AGENTE).
//
// ── Por que isto existe ────────────────────────────────────────────────────
//
// Numa única sessão de trabalho neste repositório, o agente afirmou quatro
// coisas falsas:
//
//   "o hang é deadlock de wait4"   — era spin; amostrou o processo errado
//   "o binário está instalado"     — não estava; construiu 16 invariantes,
//                                     instalou zero
//   "`ls -r` regrediu"             — não regrediu; a memoização substituiu
//                                     a saída
//   "sigil verify --json entregue" — entregue sob invocação que ninguém usa
//
// Nenhuma foi mentira. Em todas o padrão foi o mesmo: **verificou uma vez,
// continuou afirmando, o mundo mudou embaixo.** O binário ESTAVA instalado
// quando a frase foi dita pela primeira vez.
//
// Velocidade não conserta isso. O que conserta é a afirmação carregar consigo
// o modo de refutá-la — e ser reavaliada, não lembrada.
//
// ── O que faz ──────────────────────────────────────────────────────────────
//
//   assert "o gate está verde" --check 'bash tests/test_suite.sh >/dev/null'
//   assert                        lista, REVERIFICANDO cada uma
//   assert --json                 idem, para máquina
//   assert --forget <prefix>      remove matching claims in the current scope
//   assert --status --json        inspect dependencies without executing checks
//   assert "claim" --check 'cmd' --dep path --dep other --timeout-ms 5000
//   Deadlines are per assertion; status is conditional on declared files only.
//   Non-Linux: direct-process-group-only supervision, not detached descendants.
//
// O estado que importa não é "passou" — é a TRANSIÇÃO:
//
//   ✓ vale        verificado agora
//   ✗ CAIU        era verdade quando você afirmou, e não é mais   ← o ponto
//   ? nunca       registrada sem verificador que rode
//
// ── A regra de projeto ─────────────────────────────────────────────────────
//
// Uma afirmação sem verificador é recusada. Isto não é rigor decorativo: é a
// mesma tese que o JINV-017 (campo aceito e ignorado), o JINV-045 (flag que
// devolve zero sem fazer nada) e o JINV-024 (veredito que afirma mais do que
// sustenta) atacaram na entrada DOS OUTROS. Aqui a régua aponta para o próprio
// agente: uma afirmação minha sem verificador é um campo aceito e ignorado.
//
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include <builtins/builtin.h>
#include <core/argv.h>
#include <core/agent_json.h>
#include <core/agent_process.h>
#include <module/blake3.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#if !defined(__linux__) || defined(JASH_ASSERT_TEST_PORTABLE)
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/wait.h>
#endif

#define AS_MAX 128
#define AS_TEXT_MAX 256
#define AS_CHECK_MAX 512
#define AS_PATH_MAX 4096
#define AS_DEPS_MAX 16
#define AS_TIMEOUT_MS 5000
#define AS_TIMEOUT_MAX 60000
#define AS_HASH_MAX (64ull * 1024 * 1024)
#define AS_LINE_MAX (2 * (AS_TEXT_MAX + AS_CHECK_MAX + 2 * AS_PATH_MAX + \
                         AS_DEPS_MAX * (AS_PATH_MAX + 65)) + 512)

enum { AS_UNKNOWN, AS_TRUE, AS_FALLEN, AS_NEVER, AS_INVALIDATED };
static const char* const as_states[] = {"unknown", "vale", "caiu", "nunca", "invalidated"};
typedef struct { char path[AS_PATH_MAX], hash[65]; } as_dep_t;
typedef struct {
    char text[AS_TEXT_MAX], check[AS_CHECK_MAX];
    char cwd[AS_PATH_MAX], scope[AS_PATH_MAX], reason[64];
    long created, last_true;
    int ever, state, timeout_ms, ndeps;
    as_dep_t deps[AS_DEPS_MAX];
} as_t;

typedef struct { int applied, durable, error; } as_save_observation_t;
typedef struct {
    int requested, started, status_valid, exit_code;
    int output_limit, indeterminate, quiescent, descendants_quiescent;
    int quiescence_forced, descendants_forced, cleanup_error_number;
} as_verify_observation_t;

static int as_copy(char* out, size_t cap, const char* in) {
    /* INV-JASH-TIER1-SOURCE-001: copy only the admitted string plus NUL. */
    size_t length = strlen(in);
    if (length >= cap) return -1;
    memcpy(out, in, length + 1);
    return 0;
}

static int as_number(const char* text, long max, long* out) {
    if (!*text) return -1;
    for (const char* p = text; *p; p++) if (*p < '0' || *p > '9') return -1;
    char* end;
    errno = 0;
    long value = strtol(text, &end, 10);
    if (errno || *end || value > max) return -1;
    *out = value;
    return 0;
}

static int as_scope(char* cwd, char* scope) {
    if (!getcwd(cwd, AS_PATH_MAX)) return -1;
    if (as_copy(scope, AS_PATH_MAX, cwd)) return -1;
    for (;;) {
        char marker[AS_PATH_MAX];
        if (snprintf(marker, sizeof marker, "%s/.git", scope) >= (int)sizeof marker)
            return -1;
        struct stat st;
        if (lstat(marker, &st) == 0) {
            if (S_ISDIR(st.st_mode) || S_ISREG(st.st_mode)) return 0;
        } else if (errno != ENOENT) return -1;
        char* slash = strrchr(scope, '/');
        if (slash == scope && !slash[1]) break;
        if (slash == scope) slash[1] = 0;
        else *slash = 0;
    }
    if (as_copy(scope, AS_PATH_MAX, cwd)) return -1;
    return 0;
}

static int as_unhex(const char* in, char* out, size_t cap) {
    size_t n = strlen(in);
    if (n % 2 || n / 2 >= cap) return -1;
    for (size_t i = 0; i < n; i += 2) {
        unsigned byte = 0;
        for (int j = 0; j < 2; j++) {
            char c = in[i + j];
            int v = c >= '0' && c <= '9' ? c - '0' :
                    c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
            if (v < 0) return -1;
            byte = byte * 16 + (unsigned)v;
        }
        if (!byte) return -1;
        out[i / 2] = (char)byte;
    }
    out[n / 2] = 0;
    return 0;
}

static void as_hex(FILE* f, const char* text) {
    for (const unsigned char* p = (const unsigned char*)text; *p; p++)
        fprintf(f, "%02x", *p);
}

/* Five-field shipped rows remain unscoped, never assigned the caller's cwd.
 * v2 uses hex strings, strict bounds and exactly 13 + 2*ndeps TSV fields. */
static int as_load(int dir, const char* name, as_t* arr) {
    int fd = openat(dir, name, O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st, after, named;
    if (fstat(fd, &st)) { int saved = errno; close(fd); errno = saved; return -1; }
    if (!S_ISREG(st.st_mode) || st.st_uid != geteuid() || (st.st_mode & 0022) || st.st_nlink != 1 ||
        st.st_size < 0 || st.st_size > (off_t)AS_MAX * AS_LINE_MAX) {
        int saved = st.st_size > (off_t)AS_MAX * AS_LINE_MAX ? E2BIG : EINVAL;
        close(fd); errno = saved; return -1;
    }
    FILE* f = fdopen(fd, "r");
    if (!f) { close(fd); return -1; }
    char* line = malloc(AS_LINE_MAX);
    int n = 0, bad = !line;
    while (!bad && fgets(line, AS_LINE_MAX, f)) {
        size_t len = strlen(line);
        if (!len || line[len - 1] != '\n' || n == AS_MAX) { bad = 1; break; }
        line[len - 1] = 0;
        char* fields[13 + 2 * AS_DEPS_MAX + 1];
        char* p = line;
        int count = 0;
        while (p && count < (int)(sizeof fields / sizeof *fields)) fields[count++] = strsep(&p, "\t");
        if (p) { bad = 1; break; }
        as_t* a = &arr[n];
        long ever, timeout, state, ndeps;
        if (count == 5) {
            bad = as_copy(a->text, sizeof a->text, fields[0]) ||
                  as_copy(a->check, sizeof a->check, fields[1]) ||
                  as_number(fields[2], LONG_MAX, &a->created) ||
                  as_number(fields[3], LONG_MAX, &a->last_true) ||
                  as_number(fields[4], 1, &ever);
            if (!bad) { a->ever = (int)ever; memcpy(a->reason, "legacy_unscoped", sizeof("legacy_unscoped")); }
        } else if (count >= 13 && !strcmp(fields[0], "v2")) {
            bad = as_unhex(fields[1], a->text, sizeof a->text) ||
                  as_unhex(fields[2], a->check, sizeof a->check) ||
                  as_number(fields[3], LONG_MAX, &a->created) ||
                  as_number(fields[4], LONG_MAX, &a->last_true) ||
                  as_number(fields[5], 1, &ever) ||
                  as_unhex(fields[6], a->cwd, sizeof a->cwd) ||
                  as_unhex(fields[7], a->scope, sizeof a->scope) ||
                  as_number(fields[8], AS_TIMEOUT_MAX, &timeout) || !timeout ||
                  as_number(fields[9], AS_INVALIDATED, &state) ||
                  as_unhex(fields[10], a->reason, sizeof a->reason) ||
                  strcmp(fields[11], "blake3") ||
                  as_number(fields[12], AS_DEPS_MAX, &ndeps);
            if (!bad) {
                a->ever = (int)ever; a->timeout_ms = (int)timeout;
                a->state = (int)state; a->ndeps = (int)ndeps;
                bad = count != 13 + 2 * a->ndeps || a->cwd[0] != '/' || a->scope[0] != '/';
                for (int j = 0; !bad && j < a->ndeps; j++) {
                    const char* hash = fields[14 + 2 * j];
                    bad = as_unhex(fields[13 + 2 * j], a->deps[j].path, AS_PATH_MAX) ||
                          !a->deps[j].path[0] ||
                          (*hash && (strlen(hash) != 64 || strspn(hash, "0123456789abcdef") != 64)) ||
                          as_copy(a->deps[j].hash, 65, hash);
                }
                if (a->state == AS_TRUE && (!a->ever || !a->last_true)) bad = 1;
            }
        } else bad = 1;
        if (!a->text[0] || !a->check[0]) bad = 1;
        n++;
    }
    int saved = bad ? (line ? EINVAL : ENOMEM) : 0;
    if (ferror(f)) { bad = 1; saved = errno ? errno : EIO; }
    if (!bad && (fstat(fd, &after) || fstatat(dir, name, &named, AT_SYMLINK_NOFOLLOW))) {
        bad = 1; saved = errno;
    }
    if (!bad && (st.st_dev != after.st_dev || st.st_ino != after.st_ino ||
        st.st_dev != named.st_dev || st.st_ino != named.st_ino ||
        st.st_size != after.st_size || st.st_size != named.st_size ||
        st.st_mtime != after.st_mtime || st.st_ctime != after.st_ctime ||
        st.st_mtime != named.st_mtime || st.st_ctime != named.st_ctime ||
#if defined(__APPLE__)
        st.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec || st.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec ||
        st.st_mtimespec.tv_nsec != named.st_mtimespec.tv_nsec || st.st_ctimespec.tv_nsec != named.st_ctimespec.tv_nsec
#else
        st.st_mtim.tv_nsec != after.st_mtim.tv_nsec || st.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        st.st_mtim.tv_nsec != named.st_mtim.tv_nsec || st.st_ctim.tv_nsec != named.st_ctim.tv_nsec
#endif
        )) { bad = 1; saved = ESTALE; }
    free(line);
    if (fclose(f) && !bad) { bad = 1; saved = errno; }
    if (bad) errno = saved;
    return bad ? -1 : n;
}

static as_save_observation_t as_save_observed(int dir, const char* name, const as_t* arr, int n) {
    as_save_observation_t result = {0};
    /* O_EXCL in the pinned directory; the stable lock is never renamed. */
    char temp[80];
    int fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        snprintf(temp, sizeof temp, ".assertions.%ld.%d.tmp", (long)getpid(), i);
        fd = openat(dir, temp, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW, 0600);
        if (fd < 0 && errno != EEXIST) { result.error = errno; return result; }
    }
    if (fd < 0) { result.error = errno; return result; }
    FILE* f = fdopen(fd, "w");
    if (!f) { result.error = errno; close(fd); unlinkat(dir, temp, 0); return result; }
    for (int i = 0; i < n; i++) {
        const as_t* a = &arr[i];
        if (!a->scope[0]) {
            fprintf(f, "%s\t%s\t%ld\t%ld\t%d\n", a->text, a->check,
                    a->created, a->last_true, a->ever);
            continue;
        }
        fputs("v2\t", f); as_hex(f, a->text); fputc('\t', f); as_hex(f, a->check);
        fprintf(f, "\t%ld\t%ld\t%d\t", a->created, a->last_true, a->ever);
        as_hex(f, a->cwd); fputc('\t', f); as_hex(f, a->scope);
        fprintf(f, "\t%d\t%d\t", a->timeout_ms, a->state); as_hex(f, a->reason);
        fprintf(f, "\tblake3\t%d", a->ndeps);
        for (int j = 0; j < a->ndeps; j++) {
            fputc('\t', f); as_hex(f, a->deps[j].path);
            fprintf(f, "\t%s", a->deps[j].hash);
        }
        fputc('\n', f);
    }
    if (ferror(f)) result.error = errno ? errno : EIO;
    if ((fflush(f) || fsync(fd)) && !result.error) result.error = errno;
    if (fclose(f) && !result.error) result.error = errno;
    if (!result.error) {
        if (renameat(dir, temp, dir, name)) result.error = errno;
        else {
            result.applied = 1;
            if (fsync(dir)) result.error = errno;
            else result.durable = 1;
        }
    }
    if (!result.applied) (void)unlinkat(dir, temp, 0);
    return result;
}

static int as_save(int dir, const char* name, const as_t* arr, int n) {
    as_save_observation_t result = as_save_observed(dir, name, arr, n);
    if (result.error) { errno = result.error; return -1; }
    return 0;
}

static int64_t as_now(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts)) return -1;
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

/* Regular-file content snapshots, not mtime/size fingerprints. Nonblocking
 * opens reject FIFOs/devices; both byte and time budgets bound streaming work.
 * Filesystem syscalls and supervisor reaping are not hard-real-time on Linux.
 * Pre/post samples are not a filesystem snapshot: a transient change restored
 * between samples cannot be ruled out, nor can undeclared inputs be validated. */
static int as_hash(int cwd, const char* path, char out[65], int64_t deadline) {
    int fd = openat(cwd, path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return -1;
    struct stat before, after, named;
    int error = 0;
    if (fstat(fd, &before)) error = errno;
    else if (!S_ISREG(before.st_mode) || before.st_size < 0) error = EINVAL;
    else if ((uint64_t)before.st_size > AS_HASH_MAX) error = E2BIG;
    struct blake3 hash;
    blake3_init(&hash);
    unsigned char buffer[16384], digest[32];
    uint64_t total = 0;
    while (!error) {
        int64_t now = as_now();
        if (now < 0 || now >= deadline) { error = now < 0 ? errno : ETIMEDOUT; if (!error) error = EIO; break; }
        ssize_t got = read(fd, buffer, sizeof buffer);
        if (got < 0) { if (errno == EINTR) continue; error = errno; break; }
        if (!got) break;
        total += (uint64_t)got;
        if (total > AS_HASH_MAX) { error = E2BIG; break; }
        blake3_update(&hash, buffer, (uint64_t)got);
    }
    if (!error && (fstat(fd, &after) || fstatat(cwd, path, &named, 0))) error = errno;
    if (!error && (before.st_dev != named.st_dev || before.st_ino != named.st_ino ||
        before.st_size != after.st_size || before.st_size != named.st_size ||
        before.st_mtime != named.st_mtime || before.st_ctime != named.st_ctime ||
#if defined(__APPLE__)
        before.st_mtimespec.tv_sec != after.st_mtimespec.tv_sec ||
        before.st_mtimespec.tv_nsec != after.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_sec != after.st_ctimespec.tv_sec ||
        before.st_ctimespec.tv_nsec != after.st_ctimespec.tv_nsec ||
        before.st_mtimespec.tv_nsec != named.st_mtimespec.tv_nsec ||
        before.st_ctimespec.tv_nsec != named.st_ctimespec.tv_nsec
#else
        before.st_mtim.tv_sec != after.st_mtim.tv_sec || before.st_mtim.tv_nsec != after.st_mtim.tv_nsec ||
        before.st_ctim.tv_sec != after.st_ctim.tv_sec || before.st_ctim.tv_nsec != after.st_ctim.tv_nsec ||
        before.st_mtim.tv_nsec != named.st_mtim.tv_nsec || before.st_ctim.tv_nsec != named.st_ctim.tv_nsec
#endif
        )) error = ESTALE;
    if (close(fd) && !error) error = errno;
    if (error) { errno = error; return -1; }
    blake3_final(&hash, digest, sizeof digest);
    for (int i = 0; i < 32; i++) snprintf(out + 2 * i, 3, "%02x", digest[i]);
    return 0;
}

static void as_state(as_t* a, int state, const char* reason) {
    a->state = state;
    if (as_copy(a->reason, sizeof(a->reason), reason)) {
        a->state = AS_UNKNOWN;
        memcpy(a->reason, "invalid_reason", sizeof("invalid_reason"));
    }
}

#if !defined(__linux__) || defined(JASH_ASSERT_TEST_PORTABLE)
/* The structured supervisor is Linux-only. This fallback deliberately claims
 * only direct-process-group supervision: setsid()/setpgid() descendants can
 * escape it. Output is discarded, not captured or limited via RLIMIT_FSIZE.
 * Polling stays deadline-driven even after stdout/stderr close. Cleanup gets
 * 50 ms TERM grace and at most another 1 s; kernel stalls are not real-time.
 * An unreaped child or surviving group can never produce AS_TRUE. */
static void as_verify_portable(as_t* a, int cwd, int64_t deadline) {
    as_state(a, AS_UNKNOWN, "supervisor_error");
    int64_t started = as_now();
    if (started < 0 || started >= deadline) {
        as_state(a, AS_UNKNOWN, started < 0 ? "clock_error" : "timeout"); return;
    }
    sigset_t blocked, oldmask;
    struct sigaction oldaction, action = {0};
    sigemptyset(&blocked);
    sigaddset(&blocked, SIGCHLD);
    action.sa_handler = SIG_DFL;
    sigemptyset(&action.sa_mask);
    if (sigprocmask(SIG_BLOCK, &blocked, &oldmask)) return;
    if (sigaction(SIGCHLD, &action, &oldaction)) {
        sigprocmask(SIG_SETMASK, &oldmask, NULL); return;
    }
    int nullfd = open("/dev/null", O_RDWR);
    pid_t pid = nullfd < 0 ? -1 : fork();
    if (pid == 0) {
        if (setpgid(0, 0) || fchdir(cwd)) _exit(125);
        for (int fd = 0; fd < 3; fd++) if (dup2(nullfd, fd) < 0) _exit(125);
        const int signals[] = {SIGCHLD, SIGINT, SIGQUIT, SIGTERM, SIGHUP,
                               SIGPIPE, SIGALRM, SIGTSTP, SIGTTIN, SIGTTOU};
        for (size_t i = 0; i < sizeof signals / sizeof *signals; i++)
            if (sigaction(signals[i], &action, NULL)) _exit(125);
        sigset_t empty;
        sigemptyset(&empty);
        if (sigprocmask(SIG_SETMASK, &empty, NULL)) _exit(125);
        /* /dev/fd avoids scanning a potentially huge descriptor limit and
         * includes inherited descriptors above a subsequently lowered limit. */
        DIR* fds = opendir("/dev/fd");
        if (fds) {
            struct dirent* entry;
            for (;;) {
                errno = 0;
                entry = readdir(fds);
                if (!entry) { if (errno) _exit(125); break; }
                long fd;
                if (!as_number(entry->d_name, INT_MAX, &fd) && fd >= 3 && fd != dirfd(fds)) {
                    while (close((int)fd) && errno == EINTR) {}
                }
            }
            closedir(fds);
        } else {
            struct rlimit limit;
            if (getrlimit(RLIMIT_NOFILE, &limit) || limit.rlim_max == RLIM_INFINITY ||
                limit.rlim_max > INT_MAX) _exit(125);
            for (int fd = 3; (rlim_t)fd < limit.rlim_max; fd++)
                while (close(fd) && errno == EINTR) {}
        }
        execl("/bin/sh", "sh", "-c", a->check, (char*)NULL);
        _exit(125);
    }
    if (nullfd >= 0) close(nullfd);
    if (pid > 0) {
        int setup_error = setpgid(pid, pid) && errno != EACCES && errno != ESRCH;
        int reaped = 0, valid = 0, child_status = 0, cleanup = 0, killed = 0, ticks = 0;
        int64_t kill_at = 0, cleanup_end = 0;
        for (;;) {
            if (!reaped) {
                pid_t got = waitpid(pid, &child_status, WNOHANG);
                if (got == pid) { reaped = 1; valid = 1; }
                else if (got < 0 && errno != EINTR) { reaped = 1; setup_error = 1; }
            }
            int absent = kill(-pid, 0) < 0 && errno == ESRCH;
            int64_t now = as_now();
            if (!cleanup && !setup_error && now >= 0 && now < deadline && reaped && absent) {
                if (!valid || (WIFEXITED(child_status) && WEXITSTATUS(child_status) == 125))
                    as_state(a, AS_UNKNOWN, "setup_or_verifier_error");
                else if (WIFEXITED(child_status) && !WEXITSTATUS(child_status))
                    as_state(a, AS_TRUE, "verified_direct_process_group_only");
                else as_state(a, a->ever ? AS_FALLEN : AS_NEVER, "verifier_failed");
                break;
            }
            if (!cleanup && (setup_error || reaped || now < 0 || now >= deadline)) {
                as_state(a, AS_UNKNOWN, setup_error ? "supervisor_error" :
                         now < 0 ? "clock_error" : now >= deadline ? "timeout" : "cleanup_unconfirmed");
                kill(-pid, SIGTERM);
                if (!reaped) kill(pid, SIGTERM);
                cleanup = 1; kill_at = now + 50; cleanup_end = now + 1050;
            }
            if (cleanup) {
                if (reaped && absent) break;
                if (!killed && (now < 0 || now >= kill_at)) {
                    kill(-pid, SIGKILL);
                    if (!reaped) kill(pid, SIGKILL);
                    killed = 1;
                }
                if ((now >= 0 && now >= cleanup_end) || ++ticks >= 240) {
                    /* Repeated EINTR can consume the fallback poll budget
                     * before grace expires. Never leave a TERM-resistant job. */
                    if (!killed) {
                        kill(-pid, SIGKILL);
                        if (!reaped) kill(pid, SIGKILL);
                    }
                    break;
                }
            }
            struct timespec pause = {0, 5000000};
            nanosleep(&pause, NULL);
        }
    }
    sigaction(SIGCHLD, &oldaction, NULL);
    sigprocmask(SIG_SETMASK, &oldmask, NULL);
}
#endif

static void as_verify_observed(as_t* a, int status, int timeout, as_verify_observation_t* observation) {
    as_verify_observation_t ignored;
    if (!observation) observation = &ignored;
    *observation = (as_verify_observation_t){.requested = !status};
    int64_t now = as_now(), deadline = now + timeout;
    if (now < 0) { as_state(a, AS_UNKNOWN, "clock_error"); return; }
    int cwd = open(a->cwd, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (cwd < 0) { as_state(a, AS_UNKNOWN, "cwd_unavailable"); return; }
    char hashes[AS_DEPS_MAX][65];
    for (int i = 0; i < a->ndeps; i++) {
        if (as_hash(cwd, a->deps[i].path, hashes[i], deadline)) {
            as_state(a, a->deps[i].hash[0] ? AS_INVALIDATED : AS_UNKNOWN, "dependency_unavailable");
            close(cwd); return;
        }
        if (status && strcmp(hashes[i], a->deps[i].hash)) {
            as_state(a, AS_INVALIDATED, "dependency_changed"); close(cwd); return;
        }
    }
    if (status) {
        if (!a->ndeps && a->state == AS_TRUE) as_state(a, AS_UNKNOWN, "no_dependencies");
        if (close(cwd)) as_state(a, AS_UNKNOWN, "cwd_close_failed");
        return;
    }
    /* Keep the observation even when the verifier fails or times out. The
     * persisted state, not a matching hash alone, determines whether it passed. */
    for (int i = 0; i < a->ndeps; i++)
        memcpy(a->deps[i].hash, hashes[i], sizeof(a->deps[i].hash));
#if !defined(__linux__) || defined(JASH_ASSERT_TEST_PORTABLE)
    as_verify_portable(a, cwd, deadline);
#else
    char temp[] = "/tmp/jash-assert-XXXXXX", out[80], err[80], error[256];
    if (!mkdtemp(temp)) { as_state(a, AS_UNKNOWN, "spool_error"); close(cwd); return; }
    snprintf(out, sizeof out, "%s/stdout", temp);
    snprintf(err, sizeof err, "%s/stderr", temp);
    jash_execution_ir_t ir;
    jash_execution_ir_init(&ir);
    ir.stage_count = 1;
    ir.stages[0].argc = 3;
    ir.stages[0].argv[0] = "/bin/sh";
    ir.stages[0].argv[1] = "-c";
    ir.stages[0].argv[2] = a->check;
    jash_agent_process_options_t options;
    jash_agent_process_options_init(&options);
    options.cwd_fd = cwd;
    options.background = 1;
    options.stdout_path = out; options.stderr_path = err;
    options.output_limit_bytes = 1024 * 1024;
    /* Limit only output spools, not verifier-created build artifacts. The
     * supervisor polls their sizes: overshoot before cancellation is possible. */
    options.spool_limit_only = 1;
    options.cancel_grace_ms = 50;
    jash_agent_process_t* process = NULL;
    as_state(a, AS_UNKNOWN, "supervisor_error");
    now = as_now();
    if (now < 0 || now >= deadline) as_state(a, AS_UNKNOWN, "timeout");
    else if (!jash_agent_process_start(&ir, &options, &process, error, sizeof error)) {
        observation->started = 1;
        now = as_now();
        int waited = now < 0 || now >= deadline ? 1 :
                     jash_agent_process_wait(process, (int)(deadline - now));
        if (waited) {
            as_state(a, AS_UNKNOWN, waited == 1 ? "timeout" : "supervisor_error");
            (void)jash_agent_process_cancel(process, options.cancel_grace_ms);
        } else {
            const jash_agent_process_result_t* r = jash_agent_process_get_result(process);
            if (r->output_limit_hit) as_state(a, AS_UNKNOWN, "output_limit");
            else if (!r->terminal_status_valid || r->indeterminate || !r->quiescent ||
                     !r->descendants_quiescent || r->quiescence_forced || r->descendants_forced)
                as_state(a, AS_UNKNOWN, "cleanup_unconfirmed");
            else as_state(a, r->exit_code == 0 ? AS_TRUE : a->ever ? AS_FALLEN : AS_NEVER,
                          r->exit_code == 0 ? "verified" : "verifier_failed");
        }
        jash_agent_process_result_t final = {0};
        if (jash_agent_process_destroy_checked(process, &final))
            as_state(a, AS_UNKNOWN, "cleanup_unconfirmed");
        const jash_agent_process_result_t* observed = &final;
        if (observed) {
            observation->status_valid = observed->terminal_status_valid;
            observation->exit_code = observed->exit_code;
            observation->output_limit = observed->output_limit_hit;
            observation->indeterminate = observed->indeterminate;
            observation->quiescent = observed->quiescent;
            observation->descendants_quiescent = observed->descendants_quiescent;
            observation->quiescence_forced = observed->quiescence_forced;
            observation->descendants_forced = observed->descendants_forced;
            observation->cleanup_error_number = observed->cleanup_error_number;
        }
    }
    unlink(out); unlink(err); rmdir(temp);
#endif
    now = as_now();
    if (a->state == AS_TRUE && (now < 0 || now >= deadline))
        as_state(a, AS_UNKNOWN, "timeout");
    for (int i = 0; i < a->ndeps; i++) {
        char post[65];
        if (as_hash(cwd, a->deps[i].path, post, deadline)) {
            if (a->state != AS_UNKNOWN) as_state(a, AS_INVALIDATED, "dependency_unavailable");
            break;
        }
        if (strcmp(post, hashes[i])) {
            if (a->state != AS_UNKNOWN) as_state(a, AS_INVALIDATED, "dependency_changed");
            break;
        }
    }
    if (close(cwd)) as_state(a, AS_UNKNOWN, "cwd_close_failed");
    if (a->state == AS_TRUE) {
        long observed = (long)time(NULL);
        if (observed < 0) as_state(a, AS_UNKNOWN, "clock_error");
        else { a->last_true = observed; a->ever = 1; }
    }
}

static void as_verify(as_t* a, int status, int timeout) {
    as_verify_observed(a, status, timeout, NULL);
}

static void as_json_string(jash_writer_t w, void* ctx, const char* text) {
    char escaped[AS_PATH_MAX * 6 + 8];
    jash_json_esc(text, escaped, sizeof escaped);
    jash_writef(w, ctx, "\"%s\"", escaped);
}

int jash_b_assert_argv(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    (void)s;
    int json = 0, status = 0, forget = 0, timeout = 0, check_seen = 0;
    as_t* arr = calloc(AS_MAX, sizeof *arr);
    as_t* request = calloc(1, sizeof *request);
    int dir = -1, lock = -1, rc = 2;
    const char* failure = "invalid arguments (claim --check CMD [--dep PATH] [--timeout-ms 1..60000])";
    if (!arr || !request) { failure = "out of memory"; goto done; }
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!strcmp(arg, "--json")) json = 1;
        else if (!strcmp(arg, "--status")) status = 1;
        else if (!strcmp(arg, "--forget")) forget = 1;
        else if (!strcmp(arg, "--check") || !strncmp(arg, "--check=", 8)) {
            const char* value = arg + 8;
            if (!strcmp(arg, "--check")) {
                if (++i == argc) goto done;
                value = argv[i];
            }
            if (check_seen++ || !*value ||
                as_copy(request->check, sizeof request->check, value)) goto done;
        } else if (!strcmp(arg, "--dep")) {
            if (++i == argc || !argv[i][0] || request->ndeps == AS_DEPS_MAX ||
                as_copy(request->deps[request->ndeps++].path, AS_PATH_MAX, argv[i])) goto done;
        } else if (!strcmp(arg, "--timeout-ms")) {
            long value;
            if (timeout || ++i == argc || as_number(argv[i], AS_TIMEOUT_MAX, &value) || !value) goto done;
            timeout = (int)value;
        } else {
            if (arg[0] == '-' || !*arg || check_seen) goto done;
            size_t used = strlen(request->text), len = strlen(arg);
            if (used + !!used + len >= sizeof request->text) goto done;
            if (used) request->text[used++] = ' ';
            memcpy(request->text + used, arg, len + 1);
        }
    }
    int adding = request->text[0] && !forget;
    if ((adding && (!check_seen || status)) || (!adding && check_seen) ||
        (request->ndeps && !adding) || (forget && (!request->text[0] || status || timeout))) goto done;
    if (as_scope(request->cwd, request->scope)) { failure = "cannot determine cwd/scope"; goto done; }
    char directory[AS_PATH_MAX];
    const char* home = getenv("HOME");
    const char* name = home && *home ? "assertions.tsv" : ".jash_assertions.tsv";
    const char* lockname = home && *home ? "assertions.lock" : ".jash_assertions.lock";
    if (snprintf(directory, sizeof directory, "%s%s", home && *home ? home : ".",
                 home && *home ? "/.jash" : "") >= (int)sizeof directory) {
        failure = "storage path too long"; goto done;
    }
    failure = "cannot open assertion storage";
    if (mkdir(directory, 0700) && errno != EEXIST) goto done;
    dir = open(directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    if (dir < 0) goto done;
    lock = openat(dir, lockname, O_RDWR | O_CREAT | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
    struct stat st;
    if (lock < 0 || fstat(lock, &st) || !S_ISREG(st.st_mode) || st.st_nlink != 1) goto done;
    /* Serialize the transaction, including verification. No caller waits for
     * this lock: concurrent or recursive assert calls fail immediately. */
    failure = "assertion storage busy or lock unavailable";
    if (flock(lock, LOCK_EX | LOCK_NB)) goto done;
    failure = "invalid or unreadable assertion storage";
    int n = as_load(dir, name, arr);
    if (n < 0) goto done;
    int first = 0, removed = 0, legacy = 0;
    if (forget) {
        for (int i = 0; i < n;) {
            if (!strcmp(arr[i].scope, request->scope) &&
                !strncmp(arr[i].text, request->text, strlen(request->text))) {
                memmove(&arr[i], &arr[i + 1], (size_t)(n - i - 1) * sizeof *arr);
                n--; removed++;
            } else i++;
        }
    } else if (adding) {
        if (n == AS_MAX) { failure = "assertion capacity reached"; goto done; }
        request->created = (long)time(NULL);
        request->timeout_ms = timeout ? timeout : AS_TIMEOUT_MS;
        arr[n] = *request;
        first = n++;
    }
    int failed = 0, fallen = 0, visible = 0;
    for (int i = 0; i < n; i++) {
        if (!arr[i].scope[0]) legacy++;
        if (forget || i < first || strcmp(arr[i].scope, request->scope)) continue;
        as_verify(&arr[i], status, timeout ? timeout : arr[i].timeout_ms);
        if (arr[i].state != AS_TRUE) failed++;
        if (arr[i].state == AS_FALLEN) fallen++;
        visible++;
    }
    failure = "cannot persist assertion storage";
    if (!status && as_save(dir, name, arr, n)) goto done;
    rc = failed ? 1 : 0;
    if (forget) {
        if (json) jash_writef(w, ctx, "{\"forgotten\":%d}\n", removed);
        else jash_writef(w, ctx, "assert: %d forgotten\n", removed);
        goto done;
    }
    if (json) jash_writes(w, ctx, "{\"assertions\":[");
    int emitted = 0;
    for (int i = first; i < n; i++) {
        as_t* a = &arr[i];
        if (strcmp(a->scope, request->scope)) continue;
        if (json) {
            jash_writef(w, ctx, "%s{\"claim\":", emitted++ ? "," : "");
            as_json_string(w, ctx, a->text);
            jash_writes(w, ctx, ",\"check\":"); as_json_string(w, ctx, a->check);
            jash_writef(w, ctx, ",\"state\":\"%s\",\"reason\":", as_states[a->state]);
            as_json_string(w, ctx, a->reason);
            jash_writef(w, ctx, ",\"created\":%ld,\"last_true\":%ld,\"timeout_ms\":%d,\"cwd\":",
                        a->created, a->last_true, a->timeout_ms);
            as_json_string(w, ctx, a->cwd);
            jash_writes(w, ctx, ",\"scope\":"); as_json_string(w, ctx, a->scope);
            jash_writes(w, ctx, ",\"dependencies\":[");
            for (int j = 0; j < a->ndeps; j++) {
                jash_writef(w, ctx, "%s{\"path\":", j ? "," : "");
                as_json_string(w, ctx, a->deps[j].path);
                jash_writef(w, ctx, ",\"blake3\":\"%s\"}", a->deps[j].hash);
            }
            jash_writes(w, ctx, "]}");
        } else {
            const char* label = a->state == AS_TRUE ? (status ? "cached" : "✓ verificado agora") :
                                a->state == AS_FALLEN ? "CAIU" :
                                a->state == AS_NEVER ? "nunca passou" : as_states[a->state];
            jash_writef(w, ctx, "  %s: ", label);
            jash_writes_sanitized(w, ctx, a->text);
            jash_writes(w, ctx, " (");
            jash_writes_sanitized(w, ctx, a->reason);
            jash_writes(w, ctx, ")\n");
        }
    }
    if (json) jash_writef(w, ctx, "],\"fallen\":%d,\"failed\":%d,\"legacy_unscoped\":%d,\"status_only\":%s}\n",
                         fallen, failed, legacy, status ? "true" : "false");
    else {
        if (!visible) jash_writes(w, ctx, "assert: nenhuma afirmação registrada in current scope\n");
        if (legacy) jash_writef(w, ctx, "assert: %d legacy_unscoped (unknown, not executed)\n", legacy);
    }
done:
    if (lock >= 0) close(lock);
    if (dir >= 0) close(dir);
    free(arr); free(request);
    if (rc == 2) jash_writef(jash_stderr_writer, NULL, "assert: %s\n", failure);
    return rc;
}

int jash_b_assert(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    jash_argv_t av = {0};
    if (jash_argv_build(args ? args : "", &av) < 0) {
        jash_writes(jash_stderr_writer, NULL, "assert: invalid quoting or allocation failure\n");
        return 2;
    }
    int rc = jash_b_assert_argv(s, w, ctx, av.argc, av.argv);
    jash_argv_free(&av);
    return rc;
}

#include "assert_native.inc"
