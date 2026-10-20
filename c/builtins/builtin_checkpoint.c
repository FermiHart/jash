// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/checkpoint + rewind — git for the agent's live state (ADR-0023 E2).
//
//   checkpoint [name]        mark the current session state (a named timeline depth)
//   checkpoint list          list checkpoints (depth + mutations since)
//   rewind [name]            roll back every jash-mediated mutation since <name>
//   rewind --session         roll the whole session back to depth 0 ("agent went rogue")
//   rewind list              same as `checkpoint list`
//
// A checkpoint is just a remembered timeline depth (+cwd/time) persisted to
// ~/.jash/checkpoints.tsv, so it survives across `jash -c` tool-calls exactly as
// the timeline does (needs JASH_SIGIL>=1 for cross-call persistence). Rewind
// reuses the timeline's undo machinery — coverage is identical to `undo`:
// trash/rm, mv, export/unset, cd. External-tool file writes are NOT journaled
// (same honest limit as `undo`).
//
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include "recovery_output.h"
#include <core/timeline.h>
#include <core/env.h>
#include <module/sigil.h>
#include <core/adopt.h>   /* ADR-0050 W9: jash_repo_key */
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/file.h>   /* ADR-0050 W10: flock */
#include <limits.h>     /* PATH_MAX — realpath()'s mandated buffer size */
#include <core/json_writer.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/stat.h>
#include <signal.h>

#define CP_MAX  64
#define CP_NAME 64

typedef struct { char name[CP_NAME]; int depth; long ts; char cwd[512]; } cp_t;

static int cp_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    /* ADR-0050 W9: per-repo checkpoints (same key as the timeline) —
     * a checkpoint in repo A no longer shares depth with repo B. */
    const char* k = jash_repo_key();
    int n;
    if (k && k[0] != '-')
        n = snprintf(b, cap, "%s/.jash/checkpoints-%s.tsv", h, k);
    else
        n = snprintf(b, cap, "%s/.jash/checkpoints.tsv", h);
    return n >= 0 && (size_t)n < cap;
}

/* Readers pin one immutable published inode. All writers serialize the entire
 * read/modify/publish operation on a separate, never-renamed lock inode. */
static int cp_snapshot(const char* path, cp_t* rows, int max, int* invalid,
                       int* truncated, struct stat* identity, int trusted) {
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return errno == ENOENT ? 0 : -1;
    struct stat st;
    if (fstat(fd, &st) || !S_ISREG(st.st_mode) ||
        (trusted && (st.st_uid != geteuid() || (st.st_mode & 0022) || st.st_nlink != 1)) ||
        st.st_size < 0 || st.st_size > 131072 || flock(fd, LOCK_SH)) {
        close(fd); return -1;
    }
    if (identity) *identity = st;
    size_t size = (size_t)st.st_size, used = 0;
    char* bytes = malloc(size + 1);
    if (!bytes) { close(fd); return -1; }
    int failed = 0;
    while (used < size) {
        ssize_t n = read(fd, bytes + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { failed = 1; break; }
        used += (size_t)n;
    }
    bytes[used] = 0;
    char extra; ssize_t n;
    do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
    if (n != 0 || memchr(bytes, 0, used) || (used && bytes[used - 1] != '\n')) failed = 1;
    if (close(fd)) failed = 1;
    int count = 0;
    if (!failed) {
        char* cursor = bytes; char* line;
        while ((line = strsep(&cursor, "\n")) != NULL) {
            if (!*line && !cursor) break;
            if (count == max) { *truncated = 1; break; }
            char* name = strsep(&line, "\t");
            char* depth = strsep(&line, "\t");
            char* timestamp = strsep(&line, "\t");
            if (!name || !depth || !timestamp || !line || !*name || strlen(name) >= CP_NAME || strlen(line) >= sizeof(rows[0].cwd)) { (*invalid)++; continue; }
            char* end; errno = 0; long d = strtol(depth, &end, 10);
            if (errno || end == depth || *end || d < 0 || d > INT_MAX) { (*invalid)++; continue; }
            errno = 0; long t = strtol(timestamp, &end, 10);
            if (errno || end == timestamp || *end) { (*invalid)++; continue; }
            int duplicate = 0;
            for (int i = 0; i < count; i++) if (!strcmp(rows[i].name, name)) duplicate = 1;
            if (duplicate) { (*invalid)++; continue; }
            cp_t* row = &rows[count++]; memset(row, 0, sizeof(*row));
            /* INV-JASH-CHECKPOINT-BBC-001: the validated field capacities are
             * explicit; a pointer to a row does not provide a root BOS. */
            memcpy(row->name, name, strlen(name) + 1);
            memcpy(row->cwd, line, strlen(line) + 1);
            row->depth = (int)d; row->ts = t;
        }
    }
    free(bytes); return failed ? -1 : count;
}

static int cp_load(cp_t* arr, int max) {
    char p[600]; if (!cp_path(p, sizeof p)) return -1;
    int invalid = 0, truncated = 0;
    int n = cp_snapshot(p, arr, max, &invalid, &truncated, NULL, 1);
    return invalid || truncated ? -1 : n;
}

typedef struct { int parent, lock; char name[128]; } cp_transaction_t;

static void cp_unlock(cp_transaction_t* transaction) {
    if (transaction->lock >= 0) close(transaction->lock);
    if (transaction->parent >= 0) close(transaction->parent);
    transaction->lock = transaction->parent = -1;
}

static int cp_lock(const char* path, cp_transaction_t* transaction) {
    transaction->parent = transaction->lock = -1;
    char parent[600], lockname[160];
    const char* slash = strrchr(path, '/');
    if (!slash || (size_t)(slash - path) >= sizeof(parent) || strlen(slash + 1) >= sizeof(transaction->name)) return -1;
    memcpy(parent, path, (size_t)(slash - path)); parent[slash - path] = 0;
    /* INV-JASH-CHECKPOINT-BBC-001: preserve the preceding capacity check. */
    memcpy(transaction->name, slash + 1, strlen(slash + 1) + 1);
    mode_t mask = umask(0);
    int made = mkdir(parent, 0700), saved_errno = errno;
    umask(mask); errno = saved_errno;
    if (made && errno != EEXIST) return -1;
    transaction->parent = open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    struct stat st;
    if (transaction->parent < 0 || fstat(transaction->parent, &st) || st.st_uid != geteuid() || (st.st_mode & 0022)) goto fail;
    snprintf(lockname, sizeof(lockname), "%s.lock", transaction->name);
    transaction->lock = openat(transaction->parent, lockname, O_RDWR | O_CREAT | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC, 0600);
    if (transaction->lock < 0 || fstat(transaction->lock, &st) || !S_ISREG(st.st_mode) ||
        st.st_uid != geteuid() || (st.st_mode & 0022) || st.st_nlink != 1 ||
        fchmod(transaction->lock, 0600) || flock(transaction->lock, LOCK_EX)) goto fail;
    return 0;
fail:
    cp_unlock(transaction); return -1;
}

/* Failure after rename means the new state is visible but its directory sync
 * was not confirmed; callers must report failure rather than claim durability. */
static int cp_store(cp_t* arr, int n, cp_transaction_t* transaction, int* applied, int* durable) {
    if (applied) *applied = 0;
    if (durable) *durable = 0;
    static unsigned serial;
    char temporary[80];
    snprintf(temporary, sizeof(temporary), ".jash-checkpoint-%ld-%u.tmp", (long)getpid(), ++serial);
    int fd = openat(transaction->parent, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) return -1;
    if (fchmod(fd, 0600)) {
        int saved = errno; close(fd); (void)unlinkat(transaction->parent, temporary, 0); errno = saved; return -1;
    }
    FILE* output = fdopen(fd, "w");
    int failed = !output;
    if (output) {
        for (int i = 0; i < n; i++)
            if (fprintf(output, "%s\t%d\t%ld\t%s\n", arr[i].name, arr[i].depth, arr[i].ts, arr[i].cwd) < 0) failed = 1;
        if (fflush(output) || ferror(output) || fsync(fd)) failed = 1;
        if (fclose(output)) failed = 1;
    } else close(fd);
    if (!failed) {
        if (renameat(transaction->parent, temporary, transaction->parent, transaction->name)) failed = 1;
        else if (applied) *applied = 1;
    }
    if (!failed) {
        if (fsync(transaction->parent)) failed = 1;
        else if (durable) *durable = 1;
    }
    if (failed) { int saved = errno; (void)unlinkat(transaction->parent, temporary, 0); errno = saved; }
    return failed ? -1 : 0;
}

static const char* first_token(const char* args, char* buf, size_t cap) {
    if (!args) return NULL;
    while (*args == ' ' || *args == '\t') args++;
    if (!*args) return NULL;
    if (strcspn(args, " \t") >= cap) return NULL;
    size_t i = 0;
    while (args[i] && args[i] != ' ' && args[i] != '\t' && i < cap - 1) { buf[i] = args[i]; i++; }
    buf[i] = '\0';
    return buf;
}

static void auto_name(cp_t* arr, int n, char* out, size_t cap) {
    for (int k = 1; k < 1000; k++) {
        char cand[CP_NAME]; snprintf(cand, sizeof cand, "cp%d", k);
        int used = 0;
        for (int i = 0; i < n; i++) if (!strcmp(arr[i].name, cand)) { used = 1; break; }
        if (!used) { snprintf(out, cap, "%s", cand); return; }
    }
    snprintf(out, cap, "cp");
}

static int cp_create_checked(const char* name, cp_t* created, int* applied, int* durable) {
    if (applied) *applied = 0;
    if (durable) *durable = 0;
    cp_t arr[CP_MAX], c = {0}; char path[600];
    if ((name && (strlen(name) >= CP_NAME || strpbrk(name, "\t\r\n"))) ||
        !getcwd(c.cwd, sizeof(c.cwd)) || strpbrk(c.cwd, "\t\r\n") || !cp_path(path, sizeof(path))) return -1;
    c.depth = jash_timeline_depth(); c.ts = (long)time(NULL);
    cp_transaction_t transaction;
    if (cp_lock(path, &transaction)) return -1;
    /* cp_lock may leave EEXIST from its successful mkdir of ~/.jash. A bad
     * checkpoint snapshot must not report that stale errno as its cause. */
    errno = 0;
    int n = cp_load(arr, CP_MAX), rc = -1;
    if (n < 0) { if (!errno) errno = EBADMSG; goto finish; }
    /* INV-JASH-CHECKPOINT-BBC-001: name was bounded before locking. */
    if (name) memcpy(c.name, name, strlen(name) + 1);
    else auto_name(arr, n, c.name, sizeof(c.name));
    int slot = -1;
    for (int i = 0; i < n; i++) if (!strcmp(arr[i].name, c.name)) { slot = i; break; }
    if (slot >= 0) arr[slot] = c;
    else if (n < CP_MAX) arr[n++] = c;
    else { memmove(arr, arr + 1, (CP_MAX - 1) * sizeof(*arr)); arr[CP_MAX - 1] = c; }
    errno = 0;
    if (created) *created = c;
    rc = cp_store(arr, n, &transaction, applied, durable);
    if (rc && !errno) errno = EIO;
    if (!rc && created) *created = c;
finish:
    cp_unlock(&transaction); return rc;
}

static int cp_create(const char* name, cp_t* created) {
    return cp_create_checked(name, created, NULL, NULL);
}

int jash_checkpoint_prepare(const char* name, jash_checkpoint_result_t* result) {
    if (!result) { errno = EINVAL; return 2; }
    memset(result, 0, sizeof(*result));
    if ((name && (!*name || strlen(name) >= sizeof(result->name) || strpbrk(name, "\t\r\n"))) ||
        !cp_path(result->path, sizeof(result->path)) ||
        !getcwd(result->cwd, sizeof(result->cwd)) || strpbrk(result->cwd, "\t\r\n")) {
        result->error_number = errno ? errno : EINVAL;
        result->error = "checkpoint_state_unrepresentable"; return 1;
    }
    result->named = name != NULL;
    if (name) memcpy(result->name, name, strlen(name) + 1u);
    return 0;
}

int jash_checkpoint_create_prepared(jash_checkpoint_result_t* result) {
    if (!result || result->error || !result->path[0] || !result->cwd[0]) { errno = EINVAL; return 2; }
    char path[600], cwd[512];
    if (!cp_path(path, sizeof(path)) || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(path, result->path) || strcmp(cwd, result->cwd)) {
        result->error_number = ESTALE; result->error = "checkpoint_state_changed"; return 1;
    }
    cp_t current = {0}; errno = 0;
    int status = cp_create_checked(result->named ? result->name : NULL, &current,
                                   &result->applied, &result->durable) ? 1 : 0;
    result->error_number = status ? (errno ? errno : EIO) : 0;
    result->error = !status ? NULL : result->applied ? "checkpoint_directory_sync_failed" : "checkpoint_publication_failed";
    if (current.name[0]) {
        memcpy(result->name, current.name, strlen(current.name) + 1u);
        memcpy(result->cwd, current.cwd, strlen(current.cwd) + 1u);
        result->depth = current.depth; result->timestamp = current.ts; result->state_observed = 1;
    }
    return status;
}

/* Prune only the exact snapshot used to select the rewind target. A concurrent
 * publication is preserved and reported, never overwritten with stale rows. */
static int cp_prune_checked(const char* path, const struct stat* expected, const cp_t* original, int original_count,
                            int depth, int* applied, int* durable) {
    if (applied) *applied = 0;
    if (durable) *durable = 0;
    cp_transaction_t transaction; if (cp_lock(path, &transaction)) return -1;
    cp_t rows[CP_MAX]; int invalid = 0, truncated = 0, rc = -1;
    struct stat current = {0};
    int count = cp_snapshot(path, rows, CP_MAX, &invalid, &truncated, &current, 1);
    int same = count == original_count;
    for (int i = 0; same && i < count; i++)
        if (strcmp(rows[i].name, original[i].name) || strcmp(rows[i].cwd, original[i].cwd) ||
            rows[i].depth != original[i].depth || rows[i].ts != original[i].ts) same = 0;
    if (same && count >= 0 && !invalid && !truncated && current.st_dev == expected->st_dev && current.st_ino == expected->st_ino) {
        int kept = 0;
        for (int i = 0; i < count; i++) if (rows[i].depth <= depth) rows[kept++] = rows[i];
        rc = kept == count ? 0 : cp_store(rows, kept, &transaction, applied, durable);
    }
    cp_unlock(&transaction); return rc;
}

static int cp_prune(const char* path, const struct stat* expected, const cp_t* original, int original_count, int depth) {
    return cp_prune_checked(path, expected, original, original_count, depth, NULL, NULL);
}

/* JINV-049 — `checkpoint list --json`.
 *
 * O texto ja carrega tudo o que um agente precisa (nome, profundidade,
 * mutacoes desde, cwd, hora). Lendo TEXTO, porem, ele volta a parsear com
 * regex — que e a raiz que o JINV-047 atacou. Aqui a mesma informacao sai
 * estruturada, dos MESMOS dados: uma fonte de verdade so, entao texto e JSON
 * nao podem divergir.
 *
 * `json` != 0 emite JSON; caso contrario, o texto de sempre. */
static int checkpoints_json(jash_writer_t w, void* ctx) {
    char path[600];
    cp_t rows[CP_MAX]; int count = 0, invalid = 0, truncated = 0, failed = 0;
    if (!cp_path(path, sizeof(path))) failed = 1;
    else {
        count = cp_snapshot(path, rows, CP_MAX, &invalid, &truncated, NULL, 0);
        if (count < 0) { failed = 1; count = 0; }
    }
    struct sigaction old_pipe, ignore = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old_pipe)) return 125;
    jash_budget_unwrap(&w, &ctx); jash_redir_stream_writer(w, ctx);
    jash_json_writer_t out; jash_jsonw_init(&out, w, ctx);
    int depth = jash_timeline_depth();
    jash_jsonw_raw(&out, "{\"schema\":\"jash.checkpoints.v1\",\"record_fields\":\"legacy_bounded\",\"depth\":");
    jash_jsonw_i64(&out, depth); jash_jsonw_raw(&out, ",\"checkpoints\":[");
    for (int i = count - 1; i >= 0; i--) {
        if (i != count - 1) jash_jsonw_raw(&out, ",");
        jash_jsonw_raw(&out, "{\"name\":"); jash_jsonw_string_or_bytes(&out, rows[i].name);
        jash_jsonw_raw(&out, ",\"depth\":"); jash_jsonw_i64(&out, rows[i].depth);
        jash_jsonw_raw(&out, ",\"mutations_since\":"); jash_jsonw_i64(&out, depth > rows[i].depth ? depth - rows[i].depth : 0);
        jash_jsonw_raw(&out, ",\"ts\":"); jash_jsonw_safe_i64(&out, rows[i].ts);
        jash_jsonw_raw(&out, ",\"cwd\":"); jash_jsonw_string_or_bytes(&out, rows[i].cwd); jash_jsonw_raw(&out, "}");
    }
    jash_jsonw_raw(&out, "],\"invalid_rows\":"); jash_jsonw_i64(&out, invalid);
    jash_jsonw_raw(&out, ",\"scan_complete\":"); jash_jsonw_raw(&out, failed || truncated || invalid ? "false" : "true");
    jash_jsonw_raw(&out, ",\"truncated\":"); jash_jsonw_raw(&out, truncated ? "true" : "false");
    int rc = failed || invalid || truncated ? 1 : 0;
    jash_jsonw_raw(&out, ",\"exit_code\":"); jash_jsonw_i64(&out, rc); jash_jsonw_raw(&out, "}\n");
    if (jash_jsonw_finish(&out)) rc = 125;
    if (sigaction(SIGPIPE, &old_pipe, NULL)) rc = 125;
    return rc;
}

static int list_checkpoints_ex(jash_writer_t w, void* ctx, int json) {
    if (json) return checkpoints_json(w, ctx);
    cp_t arr[CP_MAX];
    int n = cp_load(arr, CP_MAX);
    if (n < 0) { jash_writes(jash_stderr_writer, NULL, "checkpoint: cannot read complete checkpoint state\n"); return 1; }
    int depth = jash_timeline_depth();
    if (n == 0) {
        jash_writes(w, ctx, "checkpoint: none yet — `checkpoint [name]` to mark current state\r\n");
        return 0;
    }
    jash_writef(w, ctx, "\033[1;36m═══ CHECKPOINTS — now at depth %d ═══\033[0m\r\n", depth);
    for (int i = n - 1; i >= 0; i--) {
        int since = depth - arr[i].depth; if (since < 0) since = 0;
        char hhmm[8] = "--:--";
        time_t t = (time_t)arr[i].ts; struct tm* tm = localtime(&t);
        if (tm) strftime(hhmm, sizeof hhmm, "%H:%M", tm);
        jash_writef(w, ctx, "  %s  \033[33m%-16s\033[0m depth=%-3d  \033[90m%d mutation(s) since · %s\033[0m\r\n",
                    hhmm, arr[i].name, arr[i].depth, since, arr[i].cwd);
    }
    return 0;
}

/* Invólucro histórico: nenhum chamador precisou mudar. */
static int list_checkpoints(jash_writer_t w, void* ctx) {
    return list_checkpoints_ex(w, ctx, 0);
}

/* Automatic safety floor (ADR-0023): drop/refresh an `auto` checkpoint at the
 * current timeline depth, called by destructive builtins (rm) BEFORE they act —
 * so `rewind auto` (or bare `rewind`, which targets the most recent marker) undoes
 * the op even if the agent never typed `checkpoint`. The agent gets undo for free;
 * that is what makes a bash-habit agent already better off than on bash. */
int jash_checkpoint_auto(void) {
    return cp_create("auto", NULL) == 0;
}

int jash_checkpoint_agent_begin(char* name, size_t capacity, int* depth) {
    if (!name || capacity < CP_NAME || !depth) {
        errno = EINVAL;
        return -1;
    }
    name[0] = '\0';
    uint8_t nonce[16];
    int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return -1;
    size_t used = 0;
    while (used < sizeof(nonce)) {
        ssize_t got = read(fd, nonce + used, sizeof(nonce) - used);
        if (got < 0 && errno == EINTR) continue;
        if (got <= 0) {
            int saved = got < 0 ? errno : EIO;
            close(fd);
            errno = saved;
            return -1;
        }
        used += (size_t)got;
    }
    if (close(fd)) return -1;
    char marker[CP_NAME];
    int n = snprintf(marker, sizeof(marker), "agent-%lx-",
                     (unsigned long)getpid());
    if (n < 0 || (size_t)n + 2u * sizeof(nonce) >= sizeof(marker)) {
        errno = EOVERFLOW;
        return -1;
    }
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(nonce); i++) {
        marker[n + 2u * i] = hex[nonce[i] >> 4];
        marker[n + 2u * i + 1u] = hex[nonce[i] & 15u];
    }
    marker[n + 2u * sizeof(nonce)] = '\0';
    cp_t created;
    if (cp_create(marker, &created)) {
        if (!errno) errno = EIO;
        return -1;
    }
    memcpy(name, marker, strlen(marker) + 1u);
    *depth = created.depth;
    return 0;
}

static const char* tl_kind_name(int k) {
    switch (k) {
        case JASH_UNDO_FILE_OVERWRITE: return "overwrite";
        case JASH_UNDO_FILE_CREATE:    return "create";
        case JASH_UNDO_MOVE:           return "move";
        case JASH_UNDO_TRASH:          return "delete";
        case JASH_UNDO_ENV_SET:        return "env-set";
        case JASH_UNDO_ENV_UNSET:      return "env-unset";
        case JASH_UNDO_CHDIR:          return "chdir";
        default: return "op";
    }
}

/* Preview the mutations between the current timeline depth and `target` (newest
 * first) WITHOUT reverting. Used by `rewind --dry` and `checkpoint diff`. */
static int preview_pending(jash_writer_t w, void* ctx, int target, const char* heading) {
    int cur = jash_timeline_depth();
    int npending = cur - target;
    if (npending <= 0) { jash_writef(w, ctx, "%s: nothing (depth %d)\r\n", heading, cur); return 0; }
    jash_writef(w, ctx, "\033[1m%s\033[0m — %d mutation(s), newest first:\r\n", heading, npending);
    for (int i = 0; i < npending; i++) {
        int kind; const char* a = NULL; const char* b = NULL;
        if (!jash_timeline_peek(i, &kind, &a, &b)) break;
        if (b && b[0])
            jash_writef(w, ctx, "  \033[33m%-10s\033[0m %s \033[90m← %s\033[0m\r\n", tl_kind_name(kind), a ? a : "", b);
        else
            jash_writef(w, ctx, "  \033[33m%-10s\033[0m %s\r\n", tl_kind_name(kind), a ? a : "");
    }
    return npending;
}

int jash_b_checkpoint(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    /* JINV-046: `checkpoint --json` criava um checkpoint CHAMADO "--json".
     * Uma flag desconhecida nao pode virar nome — aqui ela vira EFEITO. */
    { static const char* const ok_flags[] = { "--list", "--json", 0 };
      if (jash_reject_unknown_flag("checkpoint", args, ok_flags, w, ctx))
          return 2; }
    /* JINV-049: `checkpoint --json` e `checkpoint list --json` listam em JSON.
     * A flag NAO pode virar nome de checkpoint — foi o bug do JINV-046. */
    { const char* a2 = args;
      while (a2 && (*a2 == ' ' || *a2 == '\t')) a2++;
      int want_json = 0;
      if (a2 && strncmp(a2, "--json", 6) == 0 &&
          (a2[6] == '\0' || a2[6] == ' ')) want_json = 1;
      else if (a2 && strncmp(a2, "list", 4) == 0) {
          const char* r = a2 + 4;
          while (*r == ' ' || *r == '\t') r++;
          if (strncmp(r, "--json", 6) == 0 && (r[6] == '\0' || r[6] == ' '))
              want_json = 1;
      }
       if (want_json) return list_checkpoints_ex(w, ctx, 1); }
    char tok[CP_NAME];
    const char* a = first_token(args, tok, sizeof tok);
    if (!a && args && args[strspn(args, " \t")]) return 2;
    if (a && (!strcmp(a, "list") || !strcmp(a, "--list") || !strcmp(a, "-l"))) {
        return list_checkpoints(w, ctx);
    }
    /* `checkpoint diff [name]` — what changed since a checkpoint (default: latest). */
    if (a && !strcmp(a, "diff")) {
        char tok2[CP_NAME]; const char* nm = NULL;
        { char d1[CP_NAME]="", d2[CP_NAME]=""; if (args) sscanf(args, "%63s %63s", d1, d2); nm = d2[0] ? tok2 : NULL; if (d2[0]) snprintf(tok2,sizeof tok2,"%s",d2); }
        cp_t carr[CP_MAX]; int cn = cp_load(carr, CP_MAX);
        if (cn < 0) { jash_writes(jash_stderr_writer, NULL, "checkpoint diff: cannot read complete checkpoint state\n"); return 1; }
        if (cn == 0) { jash_writes(w, ctx, "checkpoint diff: no checkpoints yet\r\n"); return 1; }
        int idx = -1;
        if (nm) { for (int i = 0; i < cn; i++) if (!strcmp(carr[i].name, nm)) idx = i; }
        else idx = cn - 1;
        if (idx < 0) { jash_writef(w, ctx, "checkpoint diff: no checkpoint '%s'\r\n", nm); return 1; }
        char head[160]; snprintf(head, sizeof head, "since '%s' (depth %d)", carr[idx].name, carr[idx].depth);
        preview_pending(w, ctx, carr[idx].depth, head);
        return 0;
    }

    if (a) {
        const char* rest = args + strspn(args, " \t"); rest += strcspn(rest, " \t");
        if (rest[strspn(rest, " \t")]) return 2;
    }
    cp_t c;
    if (cp_create(a, &c) != 0) {
        jash_writes(jash_stderr_writer, 0, "checkpoint: cannot write ~/.jash/checkpoints.tsv\r\n");
        return 1;
    }

    jash_writef(w, ctx, "checkpoint '\033[1m%s\033[0m' @ depth %d  \033[90m(%s)\033[0m\r\n",
                c.name, c.depth, c.cwd);
    if (jash_sigil_get() < SIGIL_LOG)
        jash_writes(w, ctx, "  \033[33mnote:\033[0m set JASH_SIGIL>=1 so the timeline (and this checkpoint)\r\n"
                            "        survive across separate `jash -c` calls.\r\n");
    return 0;
}

int jash_b_rewind(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    /* JINV-046: `rewind --json` respondia "already at or before '--json'"
     * — a flag virava ALVO. As flags reais do rewind estao abaixo. */
    { static const char* const ok_flags[] = {
          "--all", "--dry", "--list", "--session", "--auto", "--scope", 0 };
      if (jash_reject_unknown_flag("rewind", args, ok_flags, w, ctx))
          return 2; }
    /* token walk: --scope takes a dir argument; --dry/-n anywhere;
     * first bare token is the checkpoint name */
    char scope_buf[600]; scope_buf[0] = '\0';
    char name_buf[CP_NAME]; name_buf[0] = '\0';
    int dry = 0;
    const char* p = args ? args : "";
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (!strncmp(p, "--scope", 7) && (p[7] == ' ' || p[7] == '\t' || !p[7])) {
            p += 7;
            while (*p == ' ' || *p == '\t') p++;
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && i < (int)sizeof(scope_buf) - 1)
                scope_buf[i++] = *p++;
            scope_buf[i] = '\0';
        }
        else if (!strncmp(p, "--dry", 5) || !strncmp(p, "-n", 2)) {
            dry = 1;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        else if (!name_buf[0]) {
            int i = 0;
            while (*p && *p != ' ' && *p != '\t' && i < CP_NAME - 1)
                name_buf[i++] = *p++;
            name_buf[i] = '\0';
        }
        else { while (*p && *p != ' ' && *p != '\t') p++; }
    }
    const char* a = name_buf[0] ? name_buf : NULL;

    /* PATCH-SPEC: scope must resolve — never a silent realpath of something
     * that does not exist (spec gate 4). */
    /* PATH_MAX, not 600: realpath() writes up to PATH_MAX regardless of input
     * length, so a shorter buffer is an overflow (glibc _FORTIFY_SOURCE aborts). */
    char scope_resolved[PATH_MAX]; scope_resolved[0] = '\0';
    if (scope_buf[0]) {
        if (!realpath(scope_buf, scope_resolved)) {
            jash_writef(w, ctx,
                "rewind --scope: '%s': no such directory\r\n", scope_buf);
            return 2;
        }
    }

    if (a && (!strcmp(a, "list") || !strcmp(a, "--list"))) return list_checkpoints(w, ctx);

    cp_t arr[CP_MAX];
    char store_path[600]; struct stat snapshot = {0}; int invalid = 0, truncated = 0;
    if (!cp_path(store_path, sizeof(store_path))) return 1;
    int n = cp_snapshot(store_path, arr, CP_MAX, &invalid, &truncated, &snapshot, 1);
    if (n < 0 || invalid || truncated) { jash_writes(jash_stderr_writer, NULL, "rewind: cannot read complete checkpoint state\n"); return 1; }
    int cur = jash_timeline_depth();

    int target;
    const char* label;
    char cwd_to[512]; cwd_to[0] = '\0';

    if (a && (!strcmp(a, "--session") || !strcmp(a, "session") || !strcmp(a, "--all"))) {
        target = 0; label = "session start";
    } else {
        if (n == 0) {
            jash_writes(w, ctx,
                "rewind: no checkpoints — `checkpoint` first, or use `undo` for one step,\r\n"
                "        or `rewind --session` to roll the whole session back.\r\n");
            return 1;
        }
        int idx = -1;
        if (a) { for (int i = 0; i < n; i++) if (!strcmp(arr[i].name, a)) idx = i; }
        else   idx = n - 1;                       /* default = most recent */
        if (idx < 0) {
            jash_writef(w, ctx, "rewind: no checkpoint named '%s' (try: rewind list)\r\n", a);
            return 1;
        }
        target = arr[idx].depth;
        label  = arr[idx].name;
        snprintf(cwd_to, sizeof cwd_to, "%s", arr[idx].cwd);
    }

    /* PATCH-SPEC: scoped rewind — revert only mutations under <dir>. */
    if (scope_resolved[0]) {
        if (dry) {
            jash_writef(w, ctx,
                "\033[1mrewind --dry --scope %s → '%s'\033[0m\r\n",
                scope_resolved, label);
            int kept = 0, failed = 0;
            jash_timeline_rewind_scope(target, scope_resolved, 1,
                                       &failed, &kept, s, w, ctx);
            jash_writef(w, ctx,
                "\033[90m(dry run — nothing reverted; %d would keep out of scope)\033[0m\r\n",
                kept);
            return 0;
        }
        int failed = 0, kept = 0;
        int done = jash_timeline_rewind_scope(target, scope_resolved, 0,
                                              &failed, &kept, s, w, ctx);
        /* cwd only restored when it lives under the scope (spec item 4) */
        if (cwd_to[0]) {
            size_t sl = strlen(scope_resolved);
            if (!strncmp(cwd_to, scope_resolved, sl) &&
                (cwd_to[sl] == '\0' || cwd_to[sl] == '/')) {
                if (chdir(cwd_to) == 0) jash_env_set(s, "PWD", cwd_to);
            }
        }
        int newdepth = jash_timeline_depth();
        jash_writef(w, ctx,
            "\033[1mrewind --scope\033[0m → '%s': %d reverted, %d failed, "
            "%d kept out of scope  \033[90m(depth %d→%d)\033[0m\r\n",
            label, done, failed, kept, cur, newdepth);
        if (done > 0) {
            char det[640];
            snprintf(det, sizeof det,
                     "rewind scope=%s target='%s' reverted=%d failed=%d kept=%d",
                     scope_resolved, label, done, failed, kept);
            jash_sigil_attest("rewind", det);   /* ADR-0050 W14 */
        }
        return failed ? 1 : 0;
    }

    if (dry) {
        char head[160]; snprintf(head, sizeof head, "rewind --dry → '%s'", label);
        preview_pending(w, ctx, target, head);
        jash_writes(w, ctx, "\033[90m(dry run — nothing reverted)\033[0m\r\n");
        return 0;
    }

    if (cur <= target) {
        jash_writef(w, ctx, "rewind: already at or before '%s' (depth %d ≤ %d) — nothing to revert\r\n",
                    label, cur, target);
        return 0;
    }

    int failed = 0;
    int done = jash_timeline_rewind_to(target, &failed, s, w, ctx);

    /* land in the checkpoint's cwd deterministically (chdir inverses may not
     * cover an externally-changed cwd). */
    if (cwd_to[0]) { if (chdir(cwd_to) == 0) jash_env_set(s, "PWD", cwd_to); }

    /* prune checkpoints that now point past the new state */
    int newdepth = jash_timeline_depth();
    int prune = 0;
    for (int i = 0; i < n; i++) if (arr[i].depth > newdepth) prune = 1;
    if (prune && cp_prune(store_path, &snapshot, arr, n, newdepth)) {
        jash_writes(jash_stderr_writer, NULL, "rewind: checkpoint pruning failed or state changed concurrently; mutations may already be reverted\n");
        failed++;
    }

    jash_writef(w, ctx, "\033[1mrewind\033[0m → '%s': %d reverted, %d failed  \033[90m(depth %d→%d)\033[0m\r\n",
                label, done, failed, cur, newdepth);
    if (done > 0) {
        char det[160];
        snprintf(det, sizeof det, "rewind target='%s' reverted=%d failed=%d depth=%d->%d",
                 label, done, failed, cur, newdepth);
        jash_sigil_attest("rewind", det);       /* ADR-0050 W14 */
    }
    return failed ? 1 : 0;
}

#include "checkpoint_native.inc"
