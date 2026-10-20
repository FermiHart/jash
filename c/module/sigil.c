// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — ADR-0003 M3: sigil state + log writer
//
// The core sigil writes ~/.jash/chain.log in a format that `recall` can parse
// directly — no module needed. When the sigil .modjash module is loaded, the
// module takes over chain writing (with BLAKE3 + Ed25519) and this fallback
// is skipped.

#include <module/sigil.h>
#ifndef JASH_AGENT_ONLY
#include <module/loader.h>   /* jmod_find — detect the sigil module */
#endif
#include <module/jash_crypto.h> /* jash_blake3_chain — attest lines chain like the module's */
#include <journal/journal.h> /* auto-tagging */
#include <core/adopt.h>      /* jash_repo_key — ADR-0051 W1/W6 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/file.h>        /* flock — ADR-0051 W3 */
#include <unistd.h>
#include <limits.h>

static int g_sigil_level = SIGIL_OFF;
static struct timespec g_cmd_t0;             /* ADR-0051 W1: duration */

typedef struct {
    int pinned;
    int has_home;
    char home[PATH_MAX];
    char dir[PATH_MAX];
    char chain[PATH_MAX];
    char ratchet[PATH_MAX];
    char legacy_ratchet[PATH_MAX];
    char transaction_lock[PATH_MAX];
    char sequence[PATH_MAX];
} sigil_core_identity_t;

static sigil_core_identity_t g_core_identity;

static int sigil_core_set(char* out, size_t cap, const char* value)
{
    size_t len = value ? strlen(value) : 0;
    if (!value || len >= cap) {
        errno = ENAMETOOLONG;
        return -1;
    }
    memcpy(out, value, len + 1);
    return 0;
}

static void sigil_pin_core_identity(void)
{
    if (g_core_identity.pinned) return;
    memset(&g_core_identity, 0, sizeof g_core_identity);
    const char* home = getenv("HOME");
    if (home && home[0]) {
        char resolved[PATH_MAX];
        if (realpath(home, resolved)) {
            if (sigil_core_set(g_core_identity.home,
                               sizeof g_core_identity.home, resolved) != 0)
                goto invalid_identity;
        } else if (home[0] == '/') {
            if (sigil_core_set(g_core_identity.home,
                               sizeof g_core_identity.home, home) != 0)
                goto invalid_identity;
        } else {
            char cwd[PATH_MAX];
            if (!getcwd(cwd, sizeof cwd) ||
                snprintf(g_core_identity.home, sizeof g_core_identity.home,
                         "%s/%s", cwd, home) >=
                    (int)sizeof g_core_identity.home)
                goto invalid_identity;
        }
        g_core_identity.has_home = g_core_identity.home[0] != '\0';
    }
    if (g_core_identity.has_home) {
        if (snprintf(g_core_identity.dir, sizeof g_core_identity.dir,
                     "%s/.jash", g_core_identity.home) >=
                (int)sizeof g_core_identity.dir)
            goto invalid_identity;
        const char* key = jash_repo_key();
        if (key && key[0] != '-') {
            if (snprintf(g_core_identity.chain, sizeof g_core_identity.chain,
                         "%s/chain-%s.log", g_core_identity.dir, key) >=
                    (int)sizeof g_core_identity.chain ||
                snprintf(g_core_identity.ratchet,
                         sizeof g_core_identity.ratchet,
                         "%s/ratchet-%s.key", g_core_identity.dir, key) >=
                    (int)sizeof g_core_identity.ratchet)
                goto invalid_identity;
        } else {
            if (snprintf(g_core_identity.chain, sizeof g_core_identity.chain,
                         "%s/chain.log", g_core_identity.dir) >=
                    (int)sizeof g_core_identity.chain ||
                snprintf(g_core_identity.ratchet,
                         sizeof g_core_identity.ratchet,
                         "%s/ratchet.key", g_core_identity.dir) >=
                    (int)sizeof g_core_identity.ratchet)
                goto invalid_identity;
        }
        if (snprintf(g_core_identity.legacy_ratchet,
                     sizeof g_core_identity.legacy_ratchet,
                     "%s/ratchet.key", g_core_identity.dir) >=
                (int)sizeof g_core_identity.legacy_ratchet ||
            snprintf(g_core_identity.transaction_lock,
                     sizeof g_core_identity.transaction_lock,
                     "%s.txnlock", g_core_identity.chain) >=
                (int)sizeof g_core_identity.transaction_lock ||
            snprintf(g_core_identity.sequence, sizeof g_core_identity.sequence,
                     "%s/chain.seq", g_core_identity.dir) >=
                (int)sizeof g_core_identity.sequence)
            goto invalid_identity;
    }
    g_core_identity.pinned = 1;
    return;

invalid_identity:
    memset(&g_core_identity, 0, sizeof g_core_identity);
    g_core_identity.pinned = 1;
}

static int sigil_fsync_parent(const char* path)
{
    char parent[PATH_MAX];
    if (snprintf(parent, sizeof parent, "%s", path) >= (int)sizeof parent) {
        errno = ENAMETOOLONG;
        return -1;
    }
    char* slash = strrchr(parent, '/');
    if (!slash) snprintf(parent, sizeof parent, ".");
    else if (slash == parent) slash[1] = '\0';
    else *slash = '\0';
    int fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (fd < 0) return -1;
    int rc = fsync(fd);
    if (close(fd) != 0) rc = -1;
    return rc;
}

static int sigil_publish_ratchet(const char* path, const uint8_t key[32],
                                 unsigned long long advances,
                                 const char* nonce)
{
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return -1;
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof tmp, "%s.tmp.%ld.%lld", path, (long)getpid(),
                 (long long)ts.tv_nsec) >= (int)sizeof tmp) {
        errno = ENAMETOOLONG;
        return -1;
    }
    int fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) return -1;
    FILE* out = fdopen(fd, "w");
    if (!out) { close(fd); unlink(tmp); return -1; }
    char hex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(key, hex);
    int ok = nonce && nonce[0]
        ? fprintf(out, "%s %llu %s\n", hex, advances, nonce) > 0
        : fprintf(out, "%s %llu\n", hex, advances) > 0;
    if (fflush(out) != 0 || fsync(fd) != 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); return -1; }
    return sigil_fsync_parent(path);
}

static int sigil_core_hex(const char* text, size_t len)
{
    if (!text) return 0;
    for (size_t i = 0; i < len; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F')))
            return 0;
    }
    return 1;
}

static int sigil_read_ratchet(const char* path, uint8_t key[32],
                              unsigned long long* advances, char nonce[33])
{
    FILE* f = fopen(path, "rb");
    if (!f) return -1;
    char line[192];
    int ok = fgets(line, sizeof line, f) != NULL && strchr(line, '\n') != NULL &&
             fgetc(f) == EOF && !ferror(f);
    if (fclose(f) != 0) ok = 0;
    if (!ok || strlen(line) < 67 || !sigil_core_hex(line, 64) || line[64] != ' ')
        return -1;

    const char* count = line + 65;
    if (*count < '0' || *count > '9') return -1;
    errno = 0;
    char* end = NULL;
    unsigned long long value = strtoull(count, &end, 10);
    if (errno == ERANGE || end == count) return -1;
    if (*end == '\n') {
        if (end[1] != '\0') return -1;
        nonce[0] = '\0';
    } else {
        if (*end++ != ' ' || strlen(end) != 33 || end[32] != '\n' ||
            !sigil_core_hex(end, 32))
            return -1;
        memcpy(nonce, end, 32);
        nonce[32] = '\0';
    }

    for (int i = 0; i < 32; i++) {
        unsigned byte = 0;
        if (sscanf(line + i * 2, "%02x", &byte) != 1) return -1;
        key[i] = (uint8_t)byte;
    }
    *advances = value;
    return 0;
}

int jash_sigil_get(void) { return g_sigil_level; }

void jash_sigil_set(int level)
{
    if (level < 0) level = 0;
    if (level > 3) level = 3;
    g_sigil_level = level;
}

/* Read JASH_SIGIL env var at startup — allows Claude Code / CI to auto-enable. */
void jash_sigil_auto_init(void)
{
    sigil_pin_core_identity();
    const char* env = getenv("JASH_SIGIL");
    if (env) {
        int lvl = atoi(env);
        jash_sigil_set(lvl);
    }
}

#define CHAIN_MAX_LINES 10000
#define CHAIN_TRIM_TO    5000
#define SIGIL_CHAIN_LINE_MAX (2 * 4096 + 1 + 1024)

/* Keep a bounded active tail and archive the complete prior LOG ledger.
 * Returns 1 after a durable replacement and archive, 0
 * when no trim was needed, -1 after preserving the active chain, and 2 when
 * the replacement or cleanup is uncertain but a named rollback copy remains. */
static int truncate_chain(const char* path, int keep) {
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    int total = 0;
    char* line = NULL;
    size_t line_cap = 0;
    ssize_t line_len;
    while ((line_len = getline(&line, &line_cap, f)) >= 0) total++;
    int read_ok = !ferror(f);
    free(line);
    if (fclose(f) != 0) read_ok = 0;
    if (!read_ok) return -1;
    /* The size preflight is only a cheap candidate test: long LOG records can
     * exceed it even after trimming. Real hysteresis prevents O(chain) rewrites
     * and prompt warnings on every following command. */
    if (total <= CHAIN_MAX_LINES + 1000) return 0;

    int skip = total - keep;
    f = fopen(path, "r");
    if (!f) return -1;
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) { fclose(f); return -1; }
    char tmp[PATH_MAX];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.%ld", path, (long)getpid(),
                 ts.tv_nsec) >= (int)sizeof(tmp)) {
        fclose(f);
        return -1;
    }
    int out_fd = open(tmp, O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (out_fd < 0) { fclose(f); return -1; }
    FILE* out = fdopen(out_fd, "w");
    if (!out) { close(out_fd); unlink(tmp); fclose(f); return -1; }

    int n = 0;
    int ok = 1;
    line = NULL;
    line_cap = 0;
    while ((line_len = getline(&line, &line_cap, f)) >= 0) {
        if (n >= skip && fwrite(line, 1, (size_t)line_len, out) !=
                             (size_t)line_len) {
            ok = 0;
            break;
        }
        n++;
    }
    if (ferror(f)) ok = 0;
    const char* fault = getenv("JASH_SIGIL_TEST_TRUNCATE_FAIL");
    if (fault && strcmp(fault, "write") == 0) ok = 0;
    free(line);
    if (fclose(f) != 0) ok = 0;
    if (ok && fflush(out) != 0) ok = 0;
    if (ok && fsync(out_fd) != 0) ok = 0;
    if (fault && strcmp(fault, "fsync") == 0) ok = 0;
    if (fclose(out) != 0) ok = 0;
    if (!ok) {
        unlink(tmp);
        return -1;
    }

    char backup[PATH_MAX];
    if (snprintf(backup, sizeof backup, "%s.rollback.%ld.%ld", path,
                 (long)getpid(), ts.tv_nsec) >= (int)sizeof backup) {
        unlink(tmp);
        return -1;
    }
    if (link(path, backup) != 0) { unlink(tmp); return -1; }
    if (sigil_fsync_parent(path) != 0) {
        unlink(backup);
        unlink(tmp);
        return -1;
    }
    if ((fault && strcmp(fault, "rename") == 0) || rename(tmp, path) != 0) {
        unlink(backup);
        unlink(tmp);
        (void)sigil_fsync_parent(path);
        return -1;
    }
    int published_durable = sigil_fsync_parent(path) == 0;
    if (fault && (strcmp(fault, "dir-fsync") == 0 ||
                  strcmp(fault, "rollback") == 0))
        published_durable = 0;
    if (!published_durable) {
        if (fault && strcmp(fault, "rollback") == 0) return 2;
        if (rename(backup, path) == 0 && sigil_fsync_parent(path) == 0)
            return -1;
        return 2; /* Truncated active plus named original recovery remain. */
    }
    if (fault && strcmp(fault, "cleanup") == 0) return 2;
    char archive[PATH_MAX];
    int archive_len = snprintf(archive, sizeof archive, "%s.archive.%ld.%lld.%ld",
                               path, (long)getpid(), (long long)ts.tv_sec, ts.tv_nsec);
    if (archive_len < 0 || (size_t)archive_len >= sizeof archive ||
        rename(backup, archive) != 0 || sigil_fsync_parent(path) != 0)
        return 2;
    return 1;
}

/* ── Log writer (no-module fallback) ──────────────────────────────────── */

/* Transaction lock first, then the current chain inode. The caller releases
 * both through unlock_active_chain(). */
static FILE* lock_active_chain(int* transaction_fd)
{
    sigil_pin_core_identity();
    if (!g_core_identity.has_home) return NULL;
    mkdir(g_core_identity.dir, 0700);
    int guard = open(g_core_identity.transaction_lock, O_RDWR | O_CREAT, 0600);
    if (guard < 0 || flock(guard, LOCK_EX) != 0) {
        if (guard >= 0) close(guard);
        return NULL;
    }

    for (int attempt = 0; attempt < 3; attempt++) {
        int fd = open(g_core_identity.chain, O_RDWR | O_CREAT | O_APPEND, 0600);
        if (fd < 0) break;
        if (flock(fd, LOCK_EX) != 0) { close(fd); break; }
        struct stat opened, active;
        if (fstat(fd, &opened) == 0 && stat(g_core_identity.chain, &active) == 0 &&
            opened.st_dev == active.st_dev && opened.st_ino == active.st_ino) {
            FILE* f = fdopen(fd, "a+");
            if (f) {
                *transaction_fd = guard;
                return f;
            }
        }
        (void)flock(fd, LOCK_UN);
        close(fd);
    }
    (void)flock(guard, LOCK_UN);
    close(guard);
    return NULL;
}

static void unlock_active_chain(FILE* chain, int transaction_fd)
{
    if (chain) {
        (void)flock(fileno(chain), LOCK_UN);
        fclose(chain);
    }
    if (transaction_fd >= 0) {
        (void)flock(transaction_fd, LOCK_UN);
        close(transaction_fd);
    }
}

static int sigil_core_line_attested(const char* line)
{
    unsigned long long seq = 0;
    char ts[32] = {0}, hash[65] = {0}, tag[65] = {0};
    int after_tag = 0;
    const char* tag_field = strstr(line, " tag=");
    if (sscanf(line, "%llu %31s %64s", &seq, ts, hash) != 3 ||
        !sigil_core_hex(hash, 64) || strspn(hash, "0") == 64 || !tag_field ||
        sscanf(tag_field, " tag=%64s%n", tag, &after_tag) != 1 ||
        !sigil_core_hex(tag, 64))
        return 0;
    char tail = tag_field[after_tag];
    return tail == ' ' || tail == '\t' || tail == '\r' || tail == '\n' ||
           tail == '\0';
}

/* A core LOG cap may discard only a LOG-only ledger. Once an attested entry or
 * ratchet exists, dropping a prefix would preserve the advanced witness while
 * destroying the history it witnesses. Read errors conservatively preserve.
 *
 * The scan is O(chain) and used to run on every command. The chain changes
 * only via flock-serialized appends (same inode, size/mtime advance) or a
 * trim (rename → new inode), so the verdict is cacheable on
 * (inode, size, mtime): any mutation invalidates. */
static int sigil_core_chain_must_preserve(FILE* chain)
{
    if (access(g_core_identity.ratchet, F_OK) == 0) return 1;
    static int cache_valid, cache_verdict;
    static ino_t cache_ino;
    static off_t cache_size;
    static time_t cache_mtime;
    struct stat st;
    int have_stat = fstat(fileno(chain), &st) == 0;
    if (have_stat && cache_valid && cache_ino == st.st_ino &&
        cache_size == st.st_size && cache_mtime == st.st_mtime)
        return cache_verdict;
    if (fseeko(chain, 0, SEEK_SET) != 0) return 1;
    int preserve = 0;
    char line[SIGIL_CHAIN_LINE_MAX];
    while (fgets(line, sizeof line, chain)) {
        if (!strchr(line, '\n') || sigil_core_line_attested(line)) {
            preserve = 1;
            break;
        }
    }
    if (!preserve && ferror(chain)) preserve = 1;
    (void)fseeko(chain, 0, SEEK_END);
    if (have_stat) {
        cache_valid = 1;
        cache_verdict = preserve;
        cache_ino = st.st_ino;
        cache_size = st.st_size;
        cache_mtime = st.st_mtime;
    }
    return preserve;
}

void jash_sigil_pre_cmd(const char* line)
{
    (void)line;
    /* ADR-0051 W1: duration tracking — the chain learns HOW LONG, not just
     * WHAT. Monotonic start; post_cmd computes elapsed. */
    if (clock_gettime(CLOCK_MONOTONIC, &g_cmd_t0) != 0)
        g_cmd_t0 = (struct timespec){0};
    /* SIGN/SEAL: start-of-command hash accumulation goes here (S2/S5) */
}

/* ADR-0051 W3: global seq — the per-process static counter restarted at 1
 * in every -c call, so three sequential calls wrote three "#1" and the
 * "attested order" recall displays was meaningless across calls (measured).
 * A flock-protected counter file gives the chain ONE real order, swarm-safe. */
static unsigned long long chain_next_seq(FILE* chain) {
    unsigned long long max_seq = 0;
    /* The max seq used to be a full fgets+sscanf scan per command. Writes are
     * flock-serialized and seqs are issued monotonically, so the live max is
     * always near the tip: read a small tail window and take the max seq of
     * the complete lines in it, then reconcile with the counter file below.
     * (A full scan stays as the fallback for a window without a whole line —
     * module lines with huge cmds can exceed the window on their own.) */
    char win[8193];
    size_t got = 0;
    off_t end = -1;
    if (fseeko(chain, 0, SEEK_END) == 0 && (end = ftello(chain)) >= 0) {
        off_t from = end > (off_t)(sizeof win - 1) ? end - (off_t)(sizeof win - 1) : 0;
        if (fseeko(chain, from, SEEK_SET) == 0)
            got = fread(win, 1, sizeof win - 1, chain);
    }
    if (ferror(chain)) {
        clearerr(chain);
        (void)fseeko(chain, 0, SEEK_END);
        return 0;
    }
    win[got] = '\0';
    (void)fseeko(chain, 0, SEEK_END);
    char* p = win;
    char* nl = strchr(p, '\n');
    if (nl) {
        /* The first line in the window may be partial; skip it unless the
         * window starts at offset 0 (the whole file fit). */
        p = end <= (off_t)(sizeof win - 1) ? win : nl + 1;
        for (; p && *p; ) {
            unsigned long long seq = 0;
            if (sscanf(p, "%llu", &seq) == 1 && seq > max_seq) max_seq = seq;
            nl = strchr(p, '\n');
            p = nl ? nl + 1 : NULL;
        }
    }
    if (end > 0 && max_seq == 0) {
        /* Window held no complete line (giant entry): fall back to the scan. */
        if (fseeko(chain, 0, SEEK_SET) != 0) return 0;
        char line[SIGIL_CHAIN_LINE_MAX];
        while (fgets(line, sizeof line, chain)) {
            unsigned long long seq = 0;
            if (sscanf(line, "%llu", &seq) == 1 && seq > max_seq) max_seq = seq;
        }
        if (ferror(chain)) {
            clearerr(chain);
            (void)fseeko(chain, 0, SEEK_END);
            return 0;
        }
        (void)fseeko(chain, 0, SEEK_END);
    }

    int fd = open(g_core_identity.sequence, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return 0;                    /* 0 = counter unavailable */
    (void)flock(fd, LOCK_EX);
    char buf[32] = {0};
    (void)!read(fd, buf, sizeof buf - 1);
    unsigned long long cur = strtoull(buf, NULL, 10);
    if (cur > max_seq) max_seq = cur;
    if (max_seq == ULLONG_MAX) {
        (void)flock(fd, LOCK_UN);
        close(fd);
        return 0;
    }
    unsigned long long v = max_seq + 1;
    (void)lseek(fd, 0, SEEK_SET);
    if (ftruncate(fd, 0) != 0) { /* best-effort: the write below still lands */ }
    char out[32];
    int n = snprintf(out, sizeof out, "%llu\n", v);
    (void)!write(fd, out, (size_t)n);
    (void)flock(fd, LOCK_UN);
    close(fd);
    return v;
}

void jash_sigil_post_cmd(const char* line, int exit_code)
{
    /* Feed the journal engine so auto-tags accumulate even when the sigil
     * module owns crypto chain writing. Must call BEFORE early return. */
    journal_post_cmd(line, exit_code);

    if (g_sigil_level < SIGIL_LOG) return;

    /* If the sigil module is loaded it owns the real cryptographic chain
     * (~/.jash/chain.log). Avoid double-recording. */
#ifndef JASH_AGENT_ONLY
    if (jmod_find("sigil")) return;
#endif

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
    struct timespec t1;
    if (clock_gettime(CLOCK_MONOTONIC, &t1) != 0 || !g_cmd_t0.tv_sec) return;
    long dur_ms = (long)((t1.tv_sec - g_cmd_t0.tv_sec) * 1000 +
                         (t1.tv_nsec - g_cmd_t0.tv_nsec) / 1000000);
    if (dur_ms < 0) dur_ms = 0;

    int transaction_fd = -1;
    FILE* f = lock_active_chain(&transaction_fd);
    if (!f) return;
    int preserve_attested = sigil_core_chain_must_preserve(f);
    unsigned long long seq = chain_next_seq(f);
    if (!seq) { static unsigned long long fallback = 0; seq = ++fallback; }

    const char* tag = journal_current_tag();
    if (!tag || !tag[0]) tag = "-";

    /* Recall-compatible format: <seq> <ts_sec>.<ts_ns> <hash> tag=<tag> rc=<rc> cmd=<cmd>
     * rc= BEFORE cmd= so cmd string doesn't include trailing " rc=N".
     * Matches the sigil module's format (minus the real hash/tag).
     * Newlines in cmd are replaced with spaces to keep single-line records. */
    char cmd_safe[1024];
    if (line) {
        size_t cl = 0;
        for (const char* p = line; *p && cl < sizeof(cmd_safe) - 1; p++) {
            cmd_safe[cl++] = (*p == '\n') ? ' ' : *p;
        }
        cmd_safe[cl] = '\0';
    } else {
        cmd_safe[0] = '\0';
    }

    /* ADR-0051 W1: context fields — cwd, repo key (ADR-0040 W5), duration,
     * session. Inserted BEFORE cmd= so v1 readers (strstr " rc=", " cmd=")
     * parse v2 lines unchanged, and v2 readers treat their absence in v1
     * lines as empty context. Spaces in cwd become '_' (chain is space-
     * delimited; the real path is recoverable via repo key + recall). */
    char cwd_buf[512];
    if (!getcwd(cwd_buf, sizeof cwd_buf)) cwd_buf[0] = '\0';
    for (char* p = cwd_buf; *p; p++) if (*p == ' ') *p = '_';
    const char* repo_key = jash_repo_key();
    char sess_buf[64];
    { const char* se = getenv("JASH_SESSION");
      snprintf(sess_buf, sizeof sess_buf, "%s", (se && *se) ? se : "-");
      for (char* p = sess_buf; *p; p++) if (*p == ' ') *p = '_'; }
    fprintf(f, "%llu %lld.%09ld 0000000000000000000000000000000000000000000000000000000000000000 tag=%s rc=%d cwd=%s repo=%s dur=%ld sess=%s cmd=%s\n",
            seq,
            (long long)ts.tv_sec, (long)ts.tv_nsec,
            tag, exit_code, cwd_buf, repo_key, dur_ms, sess_buf, cmd_safe);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0) {
        unlock_active_chain(f, transaction_fd);
        return;
    }

    /* ADR-0051 W7: truncate with HYSTERESIS — rewriting the whole chain on
     * EVERY command near the cap is O(chain)/cmd (JINV-005 class). Rewrite
     * only past cap+1000, and SAY it happened (silent history loss is the
     * same dishonesty class as the timeline ring was). */
    {
        if (g_core_identity.has_home) {
            struct stat st;
            if (!preserve_attested && stat(g_core_identity.chain, &st) == 0 &&
                st.st_size > 2400 * 1024) {
                (void)flock(fileno(f), LOCK_UN);
                fclose(f);
                f = NULL;
                int trim_rc = truncate_chain(g_core_identity.chain,
                                             CHAIN_TRIM_TO);
                (void)flock(transaction_fd, LOCK_UN);
                close(transaction_fd);
                transaction_fd = -1;
                if (trim_rc == 1)
                    fprintf(stderr,
                        "[jash] active LOG retained last %d lines; full prior ledger preserved in %s.archive.*\n",
                        CHAIN_TRIM_TO, g_core_identity.chain);
                else if (trim_rc < 0)
                    fprintf(stderr,
                        "[jash] chain truncation failed; active history was retained\n");
                else if (trim_rc == 2)
                    fprintf(stderr,
                        "[jash] chain rotation incomplete; inspect the named .rollback/.archive recovery beside the active chain\n");
            }
        }
    }
    if (f) unlock_active_chain(f, transaction_fd);

    /* SIGN: BLAKE3 + Ed25519 per-command signing (S2 placeholder) */
    /* SEAL: anti-bypass enforcement (S5 placeholder) */
}

/* ADR-0050 W14: attested undo — a cryptographic-style record that the agent
 * REVERTED its damage, not just that it ran commands. Also used by mesh
 * (task done/fail → swarm memory). Appends to the same recall-compatible,
 * per-repo chain.log whenever sigil logging is on.
 *
 * The line is CHAINED with the module's exact scheme — h_n = BLAKE3(h_{n-1}
 * || ts_ns(BE8) || rc(LE4) || cmd) over the tip read from the file, plus the
 * forward-secure ratchet MAC (tag_n = BLAKE3_keyed(k_n, h_n), k advances, the
 * persisted position too — JINV-031) — so an attest NEVER breaks
 * `sigil chain verify`, whether or not the sigil module owns command logging.
 * Read-tip + ratchet + append run under one flock (ADR-0051 W3: swarm-safe). */
void jash_sigil_attest(const char* event, const char* detail)
{
    if (g_sigil_level < SIGIL_LOG) return;

    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) != 0) return;
    uint64_t tns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;

    char ev[64], det[900];
    size_t i = 0;
    for (const char* p = event ? event : "-"; *p && i < sizeof(ev) - 1; p++)
        if (*p != '\n') ev[i++] = *p;
    ev[i] = '\0';
    i = 0;
    for (const char* p = detail ? detail : ""; *p && i < sizeof(det) - 1; p++)
        det[i++] = (*p == '\n') ? ' ' : *p;
    det[i] = '\0';
    char cmd[1024];
    snprintf(cmd, sizeof cmd, "%s: %s", ev, det);

    sigil_pin_core_identity();
    if (!g_core_identity.has_home) return;
    const char* k = jash_repo_key();
    int transaction_fd = -1;
    FILE* f = lock_active_chain(&transaction_fd);
    if (!f) return;
    struct stat chain_stat;
    int chain_has_entries = fstat(fileno(f), &chain_stat) == 0 && chain_stat.st_size > 0;
    if (k && k[0] != '-' && chain_has_entries &&
        access(g_core_identity.ratchet, F_OK) != 0) {
        const char* explicit_level = getenv("JASH_SIGIL");
        if (explicit_level && atoi(explicit_level) > 0)
            fprintf(stderr, "sigil append blocked: %s; "
                            "run `sigil quarantine status --json`\n",
                    access(g_core_identity.legacy_ratchet, F_OK) == 0
                        ? "legacy_global_ratchet_scope_mismatch"
                        : "missing_scoped_ratchet");
        unlock_active_chain(f, transaction_fd);
        return;
    }

    /* read the chain tip (last line) → seq+1 and prev hash */
    unsigned long long seq = 1;
    uint8_t prev[32]; memset(prev, 0, sizeof prev);
    char line[SIGIL_CHAIN_LINE_MAX];
    const char* malformed_reason = NULL;
    unsigned long long last_seq = 0;
    int have_line = 0, have_attested = 0;
    (void)fseek(f, 0, SEEK_SET);
    while (fgets(line, sizeof line, f)) {
        unsigned long long s = 0;
        char tsbuf[32] = {0}, hexbuf[65] = {0};
        int fields = sscanf(line, "%llu %31s %64s", &s, tsbuf, hexbuf);
        if (fields < 3 || !strchr(line, '\n')) {
            malformed_reason = "corrupted_newline_fragment";
            break;
        }
        if (!strstr(line, " tag=") || !strstr(line, " rc=") ||
            !strstr(line, " cmd=") || !sigil_core_hex(hexbuf, 64)) {
            malformed_reason = "unknown_malformed_chain";
            break;
        }
        have_line = 1;
        last_seq = s;
        if (strspn(hexbuf, "0") == 64) {
            const char* tag_field = strstr(line, " tag=");
            char tagbuf[65] = {0};
            int after_tag = 0;
            if (tag_field &&
                sscanf(tag_field, " tag=%64s%n", tagbuf, &after_tag) == 1 &&
                sigil_core_hex(tagbuf, 64) &&
                (tag_field[after_tag] == ' ' || tag_field[after_tag] == '\t' ||
                 tag_field[after_tag] == '\r' || tag_field[after_tag] == '\n' ||
                 tag_field[after_tag] == '\0')) {
                malformed_reason = "unknown_malformed_chain";
                break;
            }
        } else {
            if (!sigil_core_line_attested(line)) {
                malformed_reason = "unknown_malformed_chain";
                break;
            }
            for (int j = 0; j < 32; j++) {
                unsigned byte = 0;
                if (sscanf(hexbuf + j * 2, "%02x", &byte) != 1) {
                    malformed_reason = "unknown_malformed_chain";
                    break;
                }
                prev[j] = (uint8_t)byte;
            }
            if (malformed_reason) break;
            have_attested = 1;
        }
    }
    if (ferror(f)) malformed_reason = "chain_read_failed";
    if (malformed_reason) {
        const char* explicit_level = getenv("JASH_SIGIL");
        if (explicit_level && atoi(explicit_level) > 0)
            fprintf(stderr, "sigil append blocked: %s; run `sigil quarantine status --json`\n",
                    malformed_reason);
        unlock_active_chain(f, transaction_fd);
        return;
    }
    if (have_line) seq = last_seq + 1;

    /* payload: ts_ns(8B BE) || rc=0(4B LE) || cmd — module-identical */
    uint8_t payload[1200];
    size_t plen = 0;
    for (int j = 7; j >= 0; j--) payload[plen++] = (uint8_t)(tns >> (j * 8));
    for (int j = 0; j < 4; j++) payload[plen++] = 0;
    size_t clen = strlen(cmd);
    if (clen > sizeof payload - plen - 1) clen = sizeof payload - plen - 1;
    memcpy(payload + plen, cmd, clen); plen += clen;

    uint8_t newh[32];
    jash_blake3_chain(prev, payload, plen, newh);
    char hexh[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(newh, hexh);

    /* ratchet (module-compatible): tag = BLAKE3_keyed(k_n, h_n), then
     * k_{n+1} = BLAKE3(k_n || "r") and the position counter advances — the
     * verify cross-checks entries × ratchet advances (JINV-031), so an attest
     * MUST advance it or it reads as a forged entry. Under the same flock. */
    uint8_t rkey[32]; memset(rkey, 0, sizeof rkey);
    unsigned long long adv = 0;
    char ratchet_nonce[33] = {0};
    {
        if (access(g_core_identity.ratchet, F_OK) == 0) {
            if (sigil_read_ratchet(g_core_identity.ratchet, rkey, &adv,
                                   ratchet_nonce) != 0) {
                fprintf(stderr, "sigil append blocked: malformed_ratchet\n");
                unlock_active_chain(f, transaction_fd);
                return;
            }
        } else {
            if (have_attested) {
                fprintf(stderr, "sigil append blocked: missing_scoped_ratchet\n");
                unlock_active_chain(f, transaction_fd);
                return;
            }
            FILE* rng = fopen("/dev/urandom", "rb");
            if (!rng || fread(rkey, 1, 32, rng) != 32) {
                if (rng) fclose(rng);
                unlock_active_chain(f, transaction_fd);
                return;
            }
            fclose(rng);
        }
    }
    uint8_t tag[32];
    jash_blake3_keyed(rkey, newh, 32, tag);
    jash_blake3_ratchet(rkey);
    adv++;
    char taghex[JASH_BLAKE3_HEX_LEN];
    jash_blake3_hex(tag, taghex);
    memset(tag, 0, sizeof tag);
    memset(newh, 0, sizeof newh);

    (void)fseek(f, 0, SEEK_END);
    fprintf(f, "%llu %llu.%09llu %s tag=%s rc=0 cmd=%s\n",
            seq,
            (unsigned long long)(tns / 1000000000ULL),
            (unsigned long long)(tns % 1000000000ULL),
            hexh, taghex, cmd);
    if (fflush(f) != 0 || fsync(fileno(f)) != 0 ||
        sigil_publish_ratchet(g_core_identity.ratchet, rkey, adv,
                              ratchet_nonce) != 0) {
        fprintf(stderr, "sigil append blocked: durable_publish_failed\n");
    }
    memset(rkey, 0, sizeof rkey);
    unlock_active_chain(f, transaction_fd);
}
