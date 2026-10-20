// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/builtin_goal — Intention Memory Caching Protocol
// F E R M I ∞ H A R T
//
// Turns sessions into checkpoints of intent, discovery, action, and
// verification. The agent recalls checkpoints instead of re-reading raw files.
//
//   goal begin "fix cd tab completion bug"
//   goal step  "inspect completion flow"
//   goal step  "patch unique-dir auto-accept"
//   goal verify "pty reproducer passes"
//   goal close --summary
//   goal recall "cd tab"
//   goal status

#include <builtins/builtin.h>
#include <core/state.h>
#include <core/env.h>
#include <theme/io.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>
#include <regex.h>
#include <limits.h>
#include <time.h>
#include <sys/syscalls.h>

#define GOAL_DIR      "goals"
#define GOAL_CURRENT  "goals/current"
#define GOAL_MAX      256
#define GOAL_TEXT_MAX 512
#define GOAL_RECALL_MAX_FILES 512u
#define GOAL_RECALL_MAX_FILE_BYTES (1024u * 1024u)
#define GOAL_RECALL_MAX_TOTAL_BYTES (16u * 1024u * 1024u)
#define GOAL_ID_SCAN_MAX 65536u

// ── Goal checkpoint ──────────────────────────────────────────────────────────

typedef struct {
    int  id;
    char kind[16];            // begin, step, verify, close
    char text[GOAL_TEXT_MAX];
    char cwd[256];
    long ts_sec;
    int  rc_hint;             // 0 success, !=0 failure, -1 unknown
} goal_checkpoint_t;

// ── Storage helpers ──────────────────────────────────────────────────────────

static void goals_dir(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/%s", home, GOAL_DIR);
}

static void current_path(char* out, size_t sz) {
    const char* home = getenv("HOME");
    if (!home || !home[0]) home = jash_tmpdir();
    snprintf(out, sz, "%s/.jash/%s", home, GOAL_CURRENT);
}

static long now_sec(void) {
    struct timespec ts;
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0) return ts.tv_sec;
    return 0;
}

static int goals_open_dir(char* path, size_t path_cap) {
    const char* home = getenv("HOME");
    if (!home || home[0] != '/') return -1;
    int n = snprintf(path, path_cap, "%s/.jash/goals", home);
    if (n < 0 || (size_t)n >= path_cap) { errno = ENAMETOOLONG; return -1; }
    int homefd = open(home, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (homefd < 0) return -1;
    struct stat home_st;
    if (fstat(homefd, &home_st) < 0 || home_st.st_uid != geteuid() ||
        (home_st.st_mode & 0022)) {
        close(homefd);
        errno = EPERM;
        return -1;
    }
    if (mkdirat(homefd, ".jash", 0700) < 0 && errno != EEXIST) {
        close(homefd);
        return -1;
    }
    int statefd = openat(homefd, ".jash", O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(homefd);
    if (statefd < 0) return -1;
    struct stat state_st;
    if (fstat(statefd, &state_st) < 0 || state_st.st_uid != geteuid() ||
        (state_st.st_mode & 0022)) {
        close(statefd);
        errno = EPERM;
        return -1;
    }
    if (mkdirat(statefd, GOAL_DIR, 0700) < 0 && errno != EEXIST) {
        close(statefd);
        return -1;
    }
    int goalsfd = openat(statefd, GOAL_DIR,
                         O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    close(statefd);
    if (goalsfd < 0) return -1;
    struct stat goals_st;
    if (fstat(goalsfd, &goals_st) < 0 || goals_st.st_uid != geteuid() ||
        (goals_st.st_mode & 0022)) {
        close(goalsfd);
        errno = EPERM;
        return -1;
    }
    return goalsfd;
}

static int goal_file_id(const char* name, int* id) {
    static const char prefix[] = "goal_";
    static const char suffix[] = ".log";
    size_t n = strlen(name), pn = sizeof(prefix) - 1, sn = sizeof(suffix) - 1;
    if (n <= pn + sn || memcmp(name, prefix, pn) ||
        memcmp(name + n - sn, suffix, sn)) return 0;
    size_t digits = n - pn - sn;
    if (digits > 10) return 0;
    long value = 0;
    for (size_t i = 0; i < digits; i++) {
        unsigned char c = (unsigned char)name[pn + i];
        if (c < '0' || c > '9') return 0;
        value = value * 10 + (long)(c - '0');
        if (value > INT_MAX) return 0;
    }
    if (value < 1) return 0;
    *id = (int)value;
    return 1;
}

/* Allocate a process-shared ID while holding a POSIX record lock. The empty
 * O_EXCL log is the reservation: another process sees it before this lock is
 * released, and a crash leaves a harmless gap instead of reusing an ID. */
static int next_id(void) {
    char dir[512];
    int dirfd = goals_open_dir(dir, sizeof(dir));
    if (dirfd < 0) return -1;
    int lockfd = openat(dirfd, ".id-lock",
                        O_RDWR | O_CREAT | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (lockfd < 0) { close(dirfd); return -1; }
    struct stat lock_st;
    if (fstat(lockfd, &lock_st) < 0 || !S_ISREG(lock_st.st_mode) ||
        lock_st.st_uid != geteuid() || lock_st.st_nlink != 1 ||
        (lock_st.st_mode & 0022)) {
        close(lockfd); close(dirfd); errno = EPERM; return -1;
    }
    struct flock lock = {.l_type = F_WRLCK, .l_whence = SEEK_SET,
                         .l_start = 0, .l_len = 0};
    while (fcntl(lockfd, F_SETLKW, &lock) < 0) {
        if (errno == EINTR) continue;
        close(lockfd); close(dirfd); return -1;
    }

    int result = -1;
    for (unsigned attempt = 0; attempt < 8; attempt++) {
        int scanfd = dup(dirfd);
        DIR* entries = scanfd >= 0 ? fdopendir(scanfd) : NULL;
        if (!entries) {
            if (scanfd >= 0) close(scanfd);
            break;
        }
        int max = 0, scan_failed = 0;
        size_t visited = 0;
        for (;;) {
            errno = 0;
            struct dirent* entry = readdir(entries);
            if (!entry) { if (errno) scan_failed = 1; break; }
            if (++visited > GOAL_ID_SCAN_MAX) { scan_failed = 1; break; }
            int id = 0;
            if (goal_file_id(entry->d_name, &id) && id > max) max = id;
        }
        if (closedir(entries) < 0) scan_failed = 1;
        if (scan_failed || max == INT_MAX) break;
        int candidate = max + 1;
        char filename[32];
        int fn = snprintf(filename, sizeof(filename), "goal_%05d.log", candidate);
        if (fn < 0 || (size_t)fn >= sizeof(filename)) break;
        int reserve = openat(dirfd, filename,
                             O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                             0600);
        if (reserve >= 0) {
            int durable = fsync(reserve) == 0;
            if (close(reserve) < 0) durable = 0;
            if (durable && fsync(dirfd) == 0) result = candidate;
            else unlinkat(dirfd, filename, 0);
            break;
        }
        if (errno != EEXIST) break;
    }
    lock.l_type = F_UNLCK;
    (void)fcntl(lockfd, F_SETLK, &lock);
    close(lockfd);
    close(dirfd);
    return result;
}

static int write_current(const goal_checkpoint_t* cp) {
    char path[512];
    current_path(path, sizeof(path));
    FILE* f = fopen(path, "w");
    if (!f) return -1;
    fprintf(f, "%d\n%s\n%s\n%s\n%ld\n%d\n",
            cp->id, cp->kind, cp->text, cp->cwd, cp->ts_sec, cp->rc_hint);
    fclose(f);
    return 0;
}

static int read_current(goal_checkpoint_t* cp) {
    char path[512];
    current_path(path, sizeof(path));
    FILE* f = fopen(path, "r");
    if (!f) return -1;
    if (fscanf(f, "%d\n", &cp->id) != 1) { fclose(f); return -1; }
    if (!fgets(cp->kind, sizeof(cp->kind), f)) { fclose(f); return -1; }
    if (!fgets(cp->text, sizeof(cp->text), f)) { fclose(f); return -1; }
    if (!fgets(cp->cwd, sizeof(cp->cwd), f)) { fclose(f); return -1; }
    if (fscanf(f, "%ld\n", &cp->ts_sec) != 1)  cp->ts_sec  = 0;
    if (fscanf(f, "%d\n",  &cp->rc_hint) != 1) cp->rc_hint = 0;
    fclose(f);
    // strip trailing newlines
    size_t n = strlen(cp->kind); if (n && cp->kind[n-1] == '\n') cp->kind[n-1] = '\0';
    n = strlen(cp->text); if (n && cp->text[n-1] == '\n') cp->text[n-1] = '\0';
    n = strlen(cp->cwd);  if (n && cp->cwd[n-1] == '\n')  cp->cwd[n-1]  = '\0';
    return 0;
}

static int append_checkpoint(const goal_checkpoint_t* cp) {
    char dir[512];
    goals_dir(dir, sizeof(dir));
    mkdir(dir, 0755);
    char path[1024];
    snprintf(path, sizeof(path), "%s/goal_%05d.log", dir, cp->id);
    FILE* f = fopen(path, "a");
    if (!f) return -1;
    fprintf(f, "[%s] t=%ld cwd=%s rc=%d\n%s\n\n",
            cp->kind, cp->ts_sec, cp->cwd, cp->rc_hint, cp->text);
    fclose(f);
    return 0;
}

static const char* skip_ws(const char* s) {
    while (s && (*s == ' ' || *s == '\t')) s++;
    return s;
}

static void strip_quotes(char* s) {
    size_t n = strlen(s);
    if (n >= 2 && ((s[0] == '"' && s[n-1] == '"') || (s[0] == '\'' && s[n-1] == '\''))) {
        s[n-1] = '\0';
        memmove(s, s + 1, n - 1);
    }
}

// ── Subcommands ──────────────────────────────────────────────────────────────

static int cmd_begin(jash_state_t* s, jash_writer_t w, void* ctx,
                     const char* args) {
    const char* text = skip_ws(args);
    if (!text || !*text) {
        jash_writes(w, ctx, "usage: goal begin \"<intent>\"\n");
        return 1;
    }
    char tbuf[GOAL_TEXT_MAX];
    snprintf(tbuf, sizeof(tbuf), "%s", text);
    strip_quotes(tbuf);

    goal_checkpoint_t cp = {0};
    cp.id = next_id();
    if (cp.id < 0) {
        jash_writes(w, ctx, "goal: could not reserve a unique persistent ID\n");
        return 1;
    }
    snprintf(cp.kind, sizeof(cp.kind), "begin");
    snprintf(cp.text, sizeof(cp.text), "%s", tbuf);
    if (!getcwd(cp.cwd, sizeof(cp.cwd))) strlcpy(cp.cwd, ".", sizeof(cp.cwd));
    cp.ts_sec = now_sec();
    cp.rc_hint = -1;

    write_current(&cp);
    append_checkpoint(&cp);

    jash_env_set(s, "JASH_GOAL", tbuf);
    jash_writef(w, ctx, "[goal] begin #%d: %s\n", cp.id, tbuf);
    return 0;
}

static goal_checkpoint_t* current_or_fail(jash_state_t* s, jash_writer_t w, void* ctx,
                                          goal_checkpoint_t* cp) {
    if (read_current(cp) < 0) {
        jash_writes(w, ctx, "[goal] no active goal — run 'goal begin \"...\"' first\n");
        return NULL;
    }
    return cp;
}

static int checkpoint_common(jash_state_t* s, jash_writer_t w, void* ctx,
                             const char* args, const char* kind) {
    const char* text = skip_ws(args);
    if (!text || !*text) text = kind;

    goal_checkpoint_t cp;
    if (!current_or_fail(s, w, ctx, &cp)) return 1;

    snprintf(cp.kind, sizeof(cp.kind), "%s", kind);
    snprintf(cp.text, sizeof(cp.text), "%s", text);
    strip_quotes(cp.text);
    if (!getcwd(cp.cwd, sizeof(cp.cwd))) strlcpy(cp.cwd, ".", sizeof(cp.cwd));
    cp.ts_sec = now_sec();
    cp.rc_hint = -1;

    write_current(&cp);
    append_checkpoint(&cp);

    jash_writef(w, ctx, "[goal] %s #%d: %s\n", kind, cp.id, cp.text);
    return 0;
}

static int cmd_step(jash_state_t* s, jash_writer_t w, void* ctx,
                    const char* args) {
    return checkpoint_common(s, w, ctx, args, "step");
}

static int cmd_verify(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    return checkpoint_common(s, w, ctx, args, "verify");
}

static int cmd_close(jash_state_t* s, jash_writer_t w, void* ctx,
                     const char* args) {
    goal_checkpoint_t cp;
    if (!current_or_fail(s, w, ctx, &cp)) return 1;

    snprintf(cp.kind, sizeof(cp.kind), "close");
    snprintf(cp.text, sizeof(cp.text), "goal closed");
    cp.ts_sec = now_sec();
    cp.rc_hint = 0;
    append_checkpoint(&cp);

    // remove current marker
    char path[512];
    current_path(path, sizeof(path));
    unlink(path);

    jash_env_set(s, "JASH_GOAL", "");
    jash_writef(w, ctx, "[goal] closed #%d (log at ~/.jash/goals/goal_%05d.log)\n",
                cp.id, cp.id);
    return 0;
}

static int cmd_status(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    goal_checkpoint_t cp;
    if (read_current(&cp) < 0) {
        jash_writes(w, ctx, "[goal] no active goal\n");
        return 0;
    }
    jash_writef(w, ctx, "[goal] active #%d [%s] %s (cwd=%s)\n",
                cp.id, cp.kind, cp.text, cp.cwd);
    return 0;
}

typedef struct {
    regex_t* pattern;
    jash_writer_t writer;
    void* context;
    size_t visited;
    size_t total_bytes;
    unsigned matches;
    int failed;
    unsigned match_limit; /* zero retains the legacy first-five display */
    int (*on_match)(void*, const char*);
    void* match_context;
} goal_search_t;

/* Bounded, nofollow recursive walk of the goal store. In particular, neither
 * the model query nor HOME-derived paths are interpolated into a shell. */
static void goal_search_dir(goal_search_t* search, int dirfd,
                            const char* shown_path, unsigned depth) {
    unsigned match_limit = search->match_limit ? search->match_limit : 5;
    if (search->failed || search->matches >= match_limit) return;
    if (depth > 32) { search->failed = 1; return; }
    int scanfd = dup(dirfd);
    DIR* entries = scanfd >= 0 ? fdopendir(scanfd) : NULL;
    if (!entries) {
        if (scanfd >= 0) close(scanfd);
        search->failed = 1;
        return;
    }
    for (;;) {
        errno = 0;
        struct dirent* entry = readdir(entries);
        if (!entry) {
            if (errno) search->failed = 1;
            break;
        }
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
        if (++search->visited > GOAL_RECALL_MAX_FILES) {
            search->failed = 1;
            break;
        }
        struct stat observed;
        if (fstatat(dirfd, entry->d_name, &observed, AT_SYMLINK_NOFOLLOW) < 0) {
            if (errno == ENOENT) continue;
            search->failed = 1;
            break;
        }
        char child_path[4096];
        int path_len = snprintf(child_path, sizeof(child_path), "%s/%s",
                                shown_path, entry->d_name);
        if (path_len < 0 || (size_t)path_len >= sizeof(child_path)) {
            search->failed = 1;
            break;
        }
        if (S_ISDIR(observed.st_mode)) {
            int child = openat(dirfd, entry->d_name,
                               O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
            if (child < 0) {
                if (errno == ENOENT) continue;
                search->failed = 1;
                break;
            }
            struct stat opened;
            if (fstat(child, &opened) < 0 || opened.st_dev != observed.st_dev ||
                opened.st_ino != observed.st_ino) search->failed = 1;
            else goal_search_dir(search, child, child_path, depth + 1);
            if (close(child) < 0) search->failed = 1;
            if (search->failed || search->matches >= match_limit) break;
            continue;
        }
        if (!S_ISREG(observed.st_mode)) continue;
        if (observed.st_size < 0 ||
            (uint64_t)observed.st_size > GOAL_RECALL_MAX_FILE_BYTES ||
            (uint64_t)observed.st_size > GOAL_RECALL_MAX_TOTAL_BYTES - search->total_bytes) {
            search->failed = 1;
            break;
        }
        int fd = openat(dirfd, entry->d_name,
                        O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) {
            if (errno == ENOENT) continue;
            search->failed = 1;
            break;
        }
        struct stat before, after;
        if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) ||
            before.st_dev != observed.st_dev || before.st_ino != observed.st_ino ||
            before.st_size != observed.st_size) {
            close(fd);
            search->failed = 1;
            break;
        }
        size_t size = (size_t)before.st_size;
        char* bytes = malloc(size + 1);
        if (!bytes) { close(fd); search->failed = 1; break; }
        size_t used = 0;
        while (used < size) {
            ssize_t n = read(fd, bytes + used, size - used);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) { search->failed = 1; break; }
            used += (size_t)n;
        }
        if (!search->failed && (fstat(fd, &after) < 0 ||
            before.st_dev != after.st_dev || before.st_ino != after.st_ino ||
            before.st_size != after.st_size || used != size)) search->failed = 1;
        if (close(fd) < 0) search->failed = 1;
        if (search->failed) { free(bytes); break; }
        bytes[size] = '\0';
        /* grep -I semantics: binary checkpoint artifacts are not searchable,
         * but they do not invalidate matches from regular text logs. */
        if (memchr(bytes, '\0', size)) { free(bytes); continue; }
        search->total_bytes += size;
        size_t start = 0;
        int matched = 0;
        while (start < size) {
            char* newline = memchr(bytes + start, '\n', size - start);
            size_t end = newline ? (size_t)(newline - bytes) : size;
            char saved = bytes[end];
            bytes[end] = '\0';
            if (regexec(search->pattern, bytes + start, 0, NULL, 0) == 0)
                matched = 1;
            bytes[end] = saved;
            if (matched || !newline) break;
            start = end + 1;
        }
        if (matched) {
            if (search->on_match) {
                if (search->on_match(search->match_context, child_path)) search->failed = 1;
            } else jash_writef(search->writer, search->context, "  match: %s\n", child_path);
            search->matches++;
        }
        free(bytes);
        if (search->failed || search->matches >= match_limit) break;
    }
    if (closedir(entries) < 0) search->failed = 1;
}

static int cmd_recall(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    const char* pat = skip_ws(args);
    if (!pat || !*pat) {
        jash_writes(w, ctx, "usage: goal recall \"<pattern>\"\n");
        return 1;
    }
    char pbuf[GOAL_TEXT_MAX];
    snprintf(pbuf, sizeof(pbuf), "%s", pat);
    strip_quotes(pbuf);

    char dir[512];
    goals_dir(dir, sizeof(dir));

    int dirfd = open(dir, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (dirfd < 0) {
        if (errno == ENOENT) {
            jash_writes(w, ctx, "  (no matching checkpoints)\n");
            return 0;
        }
        jash_writes(w, ctx, "goal: cannot inspect checkpoint directory\n");
        return 1;
    }
    struct stat dir_stat;
    if (fstat(dirfd, &dir_stat) < 0 || !S_ISDIR(dir_stat.st_mode)) {
        close(dirfd);
        jash_writes(w, ctx, "goal: checkpoint path is not a directory\n");
        return 1;
    }

    regex_t pattern;
    int regex_rc = regcomp(&pattern, pbuf, REG_EXTENDED | REG_ICASE | REG_NOSUB);
    if (regex_rc) {
        char message[256];
        regerror(regex_rc, &pattern, message, sizeof(message));
        close(dirfd);
        jash_writef(w, ctx, "goal: invalid recall pattern: %s\n", message);
        return 1;
    }

    jash_writef(w, ctx, "[goal recall] pattern '%s'\n", pbuf);
    goal_search_t search = {.pattern = &pattern, .writer = w, .context = ctx};
    goal_search_dir(&search, dirfd, dir, 0);
    regfree(&pattern);
    if (close(dirfd) < 0) search.failed = 1;
    if (search.failed) {
        jash_writes(w, ctx, "goal: checkpoint search failed or exceeded its bounds\n");
        return 1;
    }
    if (!search.matches) jash_writes(w, ctx, "  (no matching checkpoints)\n");
    return 0;
}

// ── Main dispatch ────────────────────────────────────────────────────────────

int jash_b_goal(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!s || !w) return 1;

    const char* a = skip_ws(args);
    if (!a || !*a) {
        return cmd_status(s, w, ctx, NULL);
    }

    if (strncmp(a, "begin", 5) == 0)  return cmd_begin(s, w, ctx, skip_ws(a + 5));
    if (strncmp(a, "step", 4) == 0)   return cmd_step(s, w, ctx, skip_ws(a + 4));
    if (strncmp(a, "verify", 6) == 0) return cmd_verify(s, w, ctx, skip_ws(a + 6));
    if (strncmp(a, "close", 5) == 0)  return cmd_close(s, w, ctx, skip_ws(a + 5));
    if (strncmp(a, "recall", 6) == 0) return cmd_recall(s, w, ctx, skip_ws(a + 6));
    if (strncmp(a, "status", 6) == 0) return cmd_status(s, w, ctx, skip_ws(a + 6));

    jash_writef(w, ctx, "goal: unknown subcommand '%s'\n", a);
    return 1;
}

#include "goal_native.inc"
