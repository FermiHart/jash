/*═══════════════════════════════════════════════════════════════════════════════
 *
 *   core/timeline.c — THE COMMAND TIMELINE: universal undo for a shell
 *
 *   The feature no other shell has. Every destructive operation that
 *   flows through jash builtins (rm→trash, mv, cp-overwrite, export,
 *   unset, cd) records its INVERSE in a session journal. Then:
 *
 *       jash> undo            # reverts the last destructive action
 *       jash> undo 3          # reverts the last three
 *       jash> timeline        # shows the session's mutation history
 *
 *   Why only jash can do this: bash/zsh/fish EXEC external rm/mv/cp —
 *   they never see the semantics. jash's builtins ARE the file ops, so
 *   the shell witnesses every mutation and can journal the inverse.
 *   The trash builtin already made rm recoverable; the mv builtin
 *   already journals for atomic rollback. The timeline UNIFIES them
 *   into one user-facing safety net.
 *
 *   Design (atomic-quality bar):
 *   - Journal entries are heap-allocated on first use (ADR-0016 .bss
 *     discipline) and bounded (TIMELINE_MAX, ring overwrite).
 *   - Inverse data (file backups) lives under $TMPDIR/jash_timeline_$$/
 *     created 0700; pure syscalls, no shell-outs (osif_rmtree on exit).
 *   - Every undo handler is idempotent and reports honestly when the
 *     world changed underneath it (file modified since backup → refuse
 *     and say why, never silently clobber).
 *
 *   F E R M I ∞ H A R T | SPDX-License-Identifier: LicenseRef-SPL-1.0
 *═══════════════════════════════════════════════════════════════════════════════*/
#if defined(__linux__) && !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include "timeline.h"
#include <builtins/builtin.h>   /* JINV-050: jash_reject_unknown_flag */
#include "osif/osinfo.h"
#include <core/env.h>
#include <core/json_writer.h>
#include <core/argv.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#ifndef JASH_AGENT_ONLY
#include <module/loader.h>
#endif
#include <module/sigil.h>  /* also W14: jash_sigil_attest */
#include <theme/io.h>

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <dirent.h>
#include <sys/stat.h>
#include <sys/file.h>   /* ADR-0050 W10: flock */
#include <limits.h>     /* PATH_MAX — realpath()'s mandated buffer size */
#include <sys/syscalls.h>  /* jash_tmpdir — J1 fix */
#include <osif/osinfo.h>

#ifndef O_NOFOLLOW
#define O_NOFOLLOW 0
#endif

#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1u << 1)
#endif
#ifndef RENAME_NOREPLACE
#define RENAME_NOREPLACE (1u << 0)
#endif

#if defined(__APPLE__)
#ifndef RENAME_SWAP
#define RENAME_SWAP 0x00000002
#endif
extern int renameatx_np(int fromfd, const char* from, int tofd,
                        const char* to, unsigned int flags);
#endif

#define TIMELINE_MAX JASH_TIMELINE_MAX
#define TL_SAVE_MAGIC "JASHTL05"

typedef struct {
    jash_undo_kind_t kind;
    time_t           when;
    unsigned long long id;        /* ADR-0050 W17: (pid<<32)|seq — unique across the swarm, no coordination */
    char             desc[160];     /* human line for `timeline`        */
    char             a[512];        /* primary path / var name          */
    char             b[512];        /* secondary path / old value       */
    int              valid;
} timeline_entry_t;

static timeline_entry_t* g_tl;       /* [TIMELINE_MAX], lazy            */
static int  g_tl_count = 0;          /* total recorded (monotonic)      */
static unsigned g_tl_next_id = 0;    /* W17: next entry id              */
static char g_backup_dir[256];       /* lazily created backup area      */
static int g_backup_dir_created;     /* 0 unprepared, 1 created here, -1 existing */

/* ADR-0050 W20 kill-switch: JASH_TIMELINE=0 turns the whole machinery off
 * (A/B parity invariant: stdout/rc identical, only the safety net is gone). */
static int tl_enabled(void) {
    const char* e = getenv("JASH_TIMELINE");
    return !(e && e[0] == '0' && e[1] == '\0');
}

/* ADR-0050 W9: per-repo journal — two projects no longer share one timeline
 * (a rewind in repo B used to revert repo A's files). Outside a repo the
 * historic global file is kept. */
#include <core/adopt.h>
static void tl_bin_path(char* b, size_t cap, const char* home) {
    const char* k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(b, cap, "%s/.jash/timeline-%s.bin", home, k);
    else
        snprintf(b, cap, "%s/.jash/timeline.bin", home);
}

/* ADR-0050 W6: redo stack — entries undone in THIS process, newest last.
 * Per-process by design (redo after a fresh -c would need a second journal;
 * scoped honestly in the ADR). Cleared by any new mutation, editor-style. */
static timeline_entry_t* g_redo;     /* [TIMELINE_MAX], lazy */
static int  g_redo_count = 0;
static int  g_reapplying_redo = 0;
static int  g_applying_inverse = 0;

/* W4: entries silently overwritten by the ring — counted, never hidden. */
static int tl_lost_to_ring(void) {
    return g_tl_count > TIMELINE_MAX ? g_tl_count - TIMELINE_MAX : 0;
}

/* W7: find a live entry by id; returns idx-from-end or -1. */
static int tl_find_by_id(uint64_t id) {
    /* Full IDs match exactly; short convenience IDs must be unambiguous. */
    if (!g_tl || g_tl_count == 0) return -1;
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;
    int found = -1;
    for (int i = 0; i < live; i++) {
        int pos = (g_tl_count - 1 - i) % TIMELINE_MAX;
        if (pos < 0) pos += TIMELINE_MAX;
        if (g_tl[pos].valid && (id > UINT32_MAX ? g_tl[pos].id == id : (uint32_t)g_tl[pos].id == id)) {
            if (found >= 0) return -2;
            found = i;
        }
    }
    return found;
}

static int tl_ensure(void) {
    if (g_tl) return 0;
    g_tl = (timeline_entry_t*)calloc(TIMELINE_MAX, sizeof(timeline_entry_t));
    if (!g_tl) return -1;
    g_redo = (timeline_entry_t*)calloc(TIMELINE_MAX, sizeof(timeline_entry_t));
    if (!g_redo) { free(g_tl); g_tl = NULL; return -1; }
    return 0;
}

static timeline_entry_t* tl_slot(int idx_from_end) {
    /* idx_from_end: 0 = newest */
    if (!g_tl || g_tl_count == 0) return NULL;
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;
    if (idx_from_end >= live) return NULL;
    int pos = (g_tl_count - 1 - idx_from_end) % TIMELINE_MAX;
    if (pos < 0) pos += TIMELINE_MAX;
    return &g_tl[pos];
}

/* ADR-0050 W11: reap dead-pid backup dirs. Every session created a new
 * tl_backup/<pid> and NOTHING ever removed them (hundreds of dirs on an
 * agent machine). On first ensure, dirs named by a dead pid are removed —
 * BUT ONLY when no live journal references their contents: the journal
 * outlives the pid (SIGIL cross-call persistence), so reaping a referenced
 * dir would strand undo entries with "backup vanished" (measured: the naive
 * reaper deleted backups the very same session still needed). */
static int tl_backup_referenced(const char* home, const char* victim) {
    char pat[600];
    snprintf(pat, sizeof pat, "%s/.jash", home);
    DIR* dp = opendir(pat);
    if (!dp) return 0;
    int found = 0;
    struct dirent* ent;
    size_t vlen = strlen(victim);
    while ((ent = readdir(dp)) && !found) {
        if (strncmp(ent->d_name, "timeline", 8) != 0) continue;
        if (!strstr(ent->d_name, ".bin")) continue;
        char bp[640];
        snprintf(bp, sizeof bp, "%s/%s", pat, ent->d_name);
        int fd = open(bp, O_RDONLY);
        if (fd < 0) continue;
        /* byte-scan for the victim path (paths are embedded in entries) */
        char buf[65536 + 600];
        ssize_t n;
        size_t carry = 0;
        while ((n = read(fd, buf + carry, 65536)) > 0) {
            size_t total = carry + (size_t)n;
            for (size_t i = 0; i + vlen <= total; i++) {
                if (memcmp(buf + i, victim, vlen) == 0) { found = 1; break; }
            }
            if (found) break;
            carry = vlen - 1;
            memmove(buf, buf + total - carry, carry);
        }
        close(fd);
    }
    closedir(dp);
    return found;
}

static void tl_reap_backup_dirs(const char* home, const char* parent) {
    DIR* dp = opendir(parent);
    if (!dp) return;
    struct dirent* ent;
    char self[16];
    snprintf(self, sizeof self, "%d", (int)getpid());
    while ((ent = readdir(dp))) {
        if (ent->d_name[0] == '.') continue;
        /* pid-shaped names only */
        int pid = 0;
        for (const char* q = ent->d_name; *q; q++) {
            if (*q < '0' || *q > '9') { pid = -1; break; }
            pid = pid * 10 + (*q - '0');
        }
        if (pid <= 1 || !strcmp(ent->d_name, self)) continue;
        if (kill((pid_t)pid, 0) != 0 && errno == ESRCH) {
            char victim[600];
            snprintf(victim, sizeof victim, "%s/%s", parent, ent->d_name);
            if (tl_backup_referenced(home, victim)) continue;   /* still needed */
            osif_rmtree(victim);
        }
    }
    closedir(dp);
}

static void tl_backup_dir_init(void) {
    if (!g_backup_dir[0]) {
        g_backup_dir_created = 0;
        if (jash_sigil_get() >= SIGIL_LOG) {
            const char* home = getenv("HOME");
            if (!home || !*home) home = jash_tmpdir();
            char parent[512];
            snprintf(parent, sizeof(parent), "%s/.jash", home);
            mkdir(parent, 0700);
            snprintf(parent, sizeof(parent), "%s/.jash/tl_backup", home);
            mkdir(parent, 0700);
            tl_reap_backup_dirs(home, parent);    /* W11 */
            snprintf(g_backup_dir, sizeof(g_backup_dir), "%s/.jash/tl_backup/%d", home, (int)getpid());
        } else {
            const char* t = getenv("TMPDIR");
            if (!t || !*t) t = jash_tmpdir();
            snprintf(g_backup_dir, sizeof(g_backup_dir), "%s/jash_timeline_%d", t, (int)getpid());
        }
    }
}

const char* jash_timeline_backup_dir(void) {
    tl_backup_dir_init();
    if (!g_backup_dir_created) {
        if (mkdir(g_backup_dir, 0700) == 0) g_backup_dir_created = 1;
        else if (errno == EEXIST) {
            struct stat st;
            if (lstat(g_backup_dir, &st) == 0 && S_ISDIR(st.st_mode)) g_backup_dir_created = -1;
        }
    }
    return g_backup_dir;
}

int jash_timeline_count(void) {
    if (!g_tl || g_tl_count == 0) return 0;
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX, n = 0;
    for (int i = 0; i < live; i++) {
        int pos = (g_tl_count-1-i) % TIMELINE_MAX;
        if (pos < 0) pos += TIMELINE_MAX;
        if (g_tl[pos].valid) n++;
    }
    return n;
}

int jash_timeline_peek(int idx_from_end, int* kind,
                       const char** a, const char** b) {
    if (!g_tl || g_tl_count == 0) return 0;
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;
    int seen = 0;
    for (int i = 0; i < live; i++) {
        int pos = (g_tl_count - 1 - i) % TIMELINE_MAX;
        if (pos < 0) pos += TIMELINE_MAX;
        if (!g_tl[pos].valid) continue;
        if (seen == idx_from_end) {
            if (kind) *kind = (int)g_tl[pos].kind;
            if (a)    *a    = g_tl[pos].a;
            if (b)    *b    = g_tl[pos].b;
            return 1;
        }
        seen++;
    }
    return 0;
}

/* ADR-0042 F7: peek + wall-clock timestamp (see header). */
int jash_timeline_peek2(int idx_from_end, int* kind,
                        const char** a, const char** b, long* when) {
    if (!g_tl || g_tl_count == 0) return 0;
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;
    int seen = 0;
    for (int i = 0; i < live; i++) {
        int pos = (g_tl_count - 1 - i) % TIMELINE_MAX;
        if (pos < 0) pos += TIMELINE_MAX;
        if (!g_tl[pos].valid) continue;
        if (seen == idx_from_end) {
            if (kind) *kind = (int)g_tl[pos].kind;
            if (a)    *a    = g_tl[pos].a;
            if (b)    *b    = g_tl[pos].b;
            if (when) *when = (long)g_tl[pos].when;
            return 1;
        }
        seen++;
    }
    return 0;
}

/* W10: ids consumed by undo/rewind in THIS process — merge-on-save must not
 * resurrect them from a concurrent writer's file. */
static unsigned long long g_consumed[256];
static int g_consumed_n = 0;

static void tl_note_consumed(unsigned long long id) {
    if (!id) return;
    for (int i = 0; i < g_consumed_n; i++) if (g_consumed[i] == id) return;
    if (g_consumed_n < 256) g_consumed[g_consumed_n++] = id;
}

static int tl_is_consumed(unsigned long long id) {
    for (int i = 0; i < g_consumed_n; i++) if (g_consumed[i] == id) return 1;
    return 0;
}

void jash_timeline_save(void) {
    if (!tl_enabled()) return;
    if (jash_sigil_get() < SIGIL_LOG || !g_tl) return;
    const char* home = getenv("HOME"); if (!home || !*home) return;
    char path[600]; tl_bin_path(path, sizeof path, home);

    /* ADR-0050 W10: the swarm runs `jash -c` in parallel. The save is ONE
     * locked transaction on a single fd — LOCK_EX, read, merge, ftruncate,
     * write — because a two-step read-then-write leaves a window where a
     * concurrent writer's commit is lost from the merge base (measured:
     * 5 parallel appends → 1-3 journal entries without the transaction).
     * Merge = my live entries ∪ the file's entries (by W17 id), minus ids
     * this process consumed via undo. No entry is lost; a remote undo may
     * resurrect an entry this process never consumed — documented as
     * process-scoped undo intent, never silent data loss. */
    int fd = open(path, O_RDWR | O_CREAT, 0600);
    if (fd < 0) return;
    (void)flock(fd, LOCK_EX);

    timeline_entry_t file_e[TIMELINE_MAX];
    int file_n = 0;
    int32_t file_nid = 0;
    {
        char magic[8]; int32_t cnt = 0, esz = 0;
        (void)lseek(fd, 0, SEEK_SET);
        if (read(fd, magic, 8) == 8 &&
            memcmp(magic, TL_SAVE_MAGIC, 8) == 0 &&
            read(fd, &cnt, 4) == 4 && read(fd, &esz, 4) == 4 &&
            esz == (int32_t)sizeof(timeline_entry_t) &&
            cnt >= 1 && cnt <= TIMELINE_MAX) {
            for (int i = 0; i < cnt && file_n < TIMELINE_MAX; i++) {
                if (read(fd, &file_e[file_n], sizeof(timeline_entry_t))
                    == (ssize_t)sizeof(timeline_entry_t))
                    file_n++;
                else break;
            }
            (void)!read(fd, &file_nid, 4);
        }
    }

    /* my live entries, newest first */
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;
    timeline_entry_t merged[TIMELINE_MAX];
    int mn = 0;
    for (int i = 0; i < live && mn < TIMELINE_MAX; i++) {
        int pos = (g_tl_count - 1 - i) % TIMELINE_MAX;
        if (pos < 0) pos += TIMELINE_MAX;
        timeline_entry_t* e = &g_tl[pos];
        if (!e->valid) continue;
        merged[mn++] = *e;
    }
    /* file entries I don't hold and didn't consume */
    for (int i = 0; i < file_n && mn < TIMELINE_MAX; i++) {
        unsigned long long id = file_e[i].id;
        if (!id || tl_is_consumed(id) || !file_e[i].valid) continue;
        int dup = 0;
        for (int k = 0; k < mn; k++) if (merged[k].id == id) { dup = 1; break; }
        if (!dup) merged[mn++] = file_e[i];
    }
    /* newest-first by (when, id) — restore appends reversed, slot 0 = newest */
    for (int i = 0; i < mn - 1; i++)
        for (int k = i + 1; k < mn; k++)
            if (merged[k].when > merged[i].when ||
                (merged[k].when == merged[i].when && merged[k].id > merged[i].id)) {
                timeline_entry_t t = merged[i]; merged[i] = merged[k]; merged[k] = t;
            }
    if (mn > TIMELINE_MAX) mn = TIMELINE_MAX;

    int32_t cnt = (int32_t)mn, esz = (int32_t)sizeof(timeline_entry_t);
    int32_t nid = (int32_t)g_tl_next_id;
    if (file_nid > nid) nid = file_nid;
    (void)lseek(fd, 0, SEEK_SET);
    if (ftruncate(fd, 0) != 0) { (void)flock(fd, LOCK_UN); close(fd); return; }
    ssize_t nw __attribute__((unused));
    nw = write(fd, TL_SAVE_MAGIC, 8); nw = write(fd, &cnt, 4); nw = write(fd, &esz, 4);
    for (int i = 0; i < mn; i++)
        nw = write(fd, &merged[i], sizeof(timeline_entry_t));
    nw = write(fd, &nid, 4);
    (void)flock(fd, LOCK_UN);
    close(fd);
}

void jash_timeline_restore(void) {
    if (!tl_enabled()) return;
    if (jash_sigil_get() < SIGIL_LOG) return;
    const char* home = getenv("HOME"); if (!home || !*home) return;
    char path[600]; tl_bin_path(path, sizeof path, home);
    int fd = open(path, O_RDONLY); if (fd < 0) return;
    (void)flock(fd, LOCK_SH);   /* W10: readers don't tear a concurrent write */
    char magic[8]; int32_t cnt = 0, esz = 0; ssize_t nr;
    nr = read(fd, magic, 8);
    if (nr != 8 || memcmp(magic, TL_SAVE_MAGIC, 8) != 0) { (void)flock(fd, LOCK_UN); close(fd); return; }
    nr = read(fd, &cnt, 4); if (nr != 4) { (void)flock(fd, LOCK_UN); close(fd); return; }
    nr = read(fd, &esz, 4); if (nr != 4) { (void)flock(fd, LOCK_UN); close(fd); return; }
    if (esz != (int32_t)sizeof(timeline_entry_t) || cnt < 1 || cnt > TIMELINE_MAX) { (void)flock(fd, LOCK_UN); close(fd); return; }
    if (tl_ensure() != 0) { (void)flock(fd, LOCK_UN); close(fd); return; }
    /* W10: the file stays (saves rewrite it merged); it is ordered
     * newest-first, so append in reverse to keep the ring's
     * positional semantics (slot 0 = newest). */
    timeline_entry_t es[TIMELINE_MAX];
    int en = 0;
    for (int i = 0; i < cnt && en < TIMELINE_MAX; i++) {
        nr = read(fd, &es[en], sizeof(timeline_entry_t));
        if (nr != (ssize_t)sizeof(timeline_entry_t)) break;
        en++;
    }
    {   /* W17: appended id counter (absent in early W17 builds → floor wins) */
        int32_t nid = 0;
        (void)nid; (void)!read(fd, &nid, 4);   /* legacy filler */
    }
    (void)flock(fd, LOCK_UN);
    close(fd);
    for (int i = en - 1; i >= 0; i--) {
        timeline_entry_t e = es[i];
        if (e.kind == JASH_UNDO_FILE_OVERWRITE && e.b[0]) {
            struct stat st; if (stat(e.b, &st) != 0) e.valid = 0;
        }
        if ((unsigned)(e.id >> 32) == (unsigned)getpid()) {
            unsigned lo = (unsigned)(e.id & 0xFFFFFFFFu);
            if (lo > g_tl_next_id) g_tl_next_id = lo;   /* W17 per-pid floor */
        }
        g_tl[g_tl_count % TIMELINE_MAX] = e; g_tl_count++;
    }
}

/* ── Recording ───────────────────────────────────────────────────────────── */

/* ADR-0050 W9-scope: canonicalize a journaled path to absolute, lexically
 * (no realpath — CREATE targets don't exist yet). Entries used to store the
 * path AS TYPED, so `cd` between mutation and undo reverted the WRONG file,
 * and scoped rewind had no stable prefix to match. Recording absolute at
 * record time (when cwd is right) fixes both. */
static void tl_canon(const char* in, char* out, size_t cap) {
    if (!in || !*in) { snprintf(out, cap, "%s", in ? in : ""); return; }
    /* getcwd already names the physical directory. A plain final component
     * stays literal (including symlinks), so there is nothing else to resolve.
     * Query each time: never cache across cd or filesystem changes. */
    if (!strchr(in, '/') && strcmp(in, ".") && strcmp(in, "..")) {
        char cwd[768];
        if (!getcwd(cwd, sizeof cwd)) { snprintf(out, cap, "%s", in); return; }
        if (cwd[0] == '/') {
            snprintf(out, cap, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", in);
            return;
        }
    }
    char tmp[1200];
    if (in[0] == '/') {
        snprintf(tmp, sizeof tmp, "%s", in);
    } else {
        char cwd[768];
        if (!getcwd(cwd, sizeof cwd)) { snprintf(out, cap, "%s", in); return; }
        snprintf(tmp, sizeof tmp, "%s/%s", cwd, in);
    }
    /* lexical normalize: collapse //, /./, resolve /../ */
    char* parts[128]; int np = 0;
    char* save = NULL;
    char work[1200];
    snprintf(work, sizeof work, "%s", tmp);
    for (char* t = strtok_r(work, "/", &save); t && np < 128;
         t = strtok_r(NULL, "/", &save)) {
        if (!strcmp(t, ".")) continue;
        if (!strcmp(t, "..")) { if (np > 0) np--; continue; }
        parts[np++] = t;
    }
    char norm[1200];
    size_t o = 0;
    norm[o++] = '/';
    for (int i = 0; i < np && o + 1 < sizeof norm; i++) {
        int n = snprintf(norm + o, sizeof norm - o, "%s%s", i ? "/" : "", parts[i]);
        if (n < 0) break;
        o += (size_t)n;
    }
    if (o >= sizeof norm) o = sizeof norm - 1;
    norm[o] = '\0';
    /* split the final component (kept LITERAL) from the directory
     * (resolved): realpath follows symlinks, so resolving the whole path
     * turned the journaled link l.txt into its TARGET m3.txt, and the undo
     * then unlinked the target (measured: undo of `ln -s t l` deleted t).
     * A symlink as the final component is DATA, never a path to follow. */
    char dirbuf[1200], basebuf[600];
    const char* last = strrchr(norm, '/');
    if (last && last[1]) {
        snprintf(basebuf, sizeof basebuf, "%s", last + 1);
        snprintf(dirbuf, sizeof dirbuf, "%.*s", (int)(last - norm), norm);
        if (!dirbuf[0]) snprintf(dirbuf, sizeof dirbuf, "/");
    } else {
        snprintf(dirbuf, sizeof dirbuf, "%s", norm);
        basebuf[0] = '\0';
    }
    /* realpath() writes up to PATH_MAX bytes into the caller's buffer — that is
     * the contract, not a function of the input length. Anything smaller is an
     * overflow: glibc's _FORTIFY_SOURCE aborts here outright. This was 1200,
     * which happens to exceed macOS's PATH_MAX (1024) but not Linux's (4096). */
    char r[PATH_MAX];
    if (realpath(dirbuf, r)) {
        if (basebuf[0]) {
            if (!strcmp(r, "/")) snprintf(out, cap, "/%s", basebuf);
            else                 snprintf(out, cap, "%s/%s", r, basebuf);
        } else {
            snprintf(out, cap, "%s", r);
        }
        return;
    }
    /* dir missing: walk up for the longest existing prefix */
    char probe[1200]; snprintf(probe, sizeof probe, "%s", dirbuf);
    char suffix[1200] = "";
    for (;;) {
        if (realpath(probe[0] ? probe : "/", r)) {
            size_t oo = 0;
            if (strcmp(r, "/")) oo += snprintf(out + oo, cap - oo, "%s", r);
            if (suffix[0]) oo += snprintf(out + oo, cap - oo, "%s", suffix);
            if (basebuf[0]) (void)snprintf(out + oo, cap - oo, "/%s", basebuf);
            return;
        }
        char* slash = strrchr(probe, '/');
        if (!slash || !probe[0]) { snprintf(out, cap, "%s", norm); return; }
        char seg[600];
        snprintf(seg, sizeof seg, "/%s%s", slash + 1, suffix);
        snprintf(suffix, sizeof suffix, "%s", seg);
        *slash = '\0';
    }
}

static int tl_kind_has_paths(jash_undo_kind_t k) {
    return k == JASH_UNDO_FILE_OVERWRITE || k == JASH_UNDO_FILE_CREATE ||
           k == JASH_UNDO_MOVE || k == JASH_UNDO_TRASH ||
           k == JASH_UNDO_META || k == JASH_UNDO_LINK_CREATE ||
           k == JASH_UNDO_DIR_CREATE || k == JASH_UNDO_DIR_REMOVE || k == JASH_UNDO_TIMES;
}

static uint64_t tl_record(jash_undo_kind_t kind,
                          const char* desc,
                           const char* a, const char* b, int exact) {
    if (!tl_enabled() || g_applying_inverse) return 0; /* An inverse is not a new mutation. */
    if (tl_ensure() != 0) return 0;            /* OOM: feature off, shell on */
    /* A replay must create a new undo entry without discarding pending redo. */
    if (!g_reapplying_redo) g_redo_count = 0;
    timeline_entry_t* e = &g_tl[g_tl_count % TIMELINE_MAX];
    memset(e, 0, sizeof(*e));
    e->kind  = kind;
    e->when  = time(NULL);
    e->id    = ((unsigned long long)getpid() << 32) | (++g_tl_next_id); /* W17/W10: swarm-unique */
    e->valid = 1;
    if (desc) strlcpy(e->desc, desc, sizeof(e->desc));
    if (exact) {
        if (a) strlcpy(e->a, a, sizeof(e->a));
        if (b) strlcpy(e->b, b, sizeof(e->b));
    } else if (tl_kind_has_paths(kind)) {
        /* canonical absolute for operational fields; desc stays as typed */
        if (a && *a) { char ca[512]; tl_canon(a, ca, sizeof ca); strlcpy(e->a, ca, sizeof(e->a)); }
        else if (a) strlcpy(e->a, a, sizeof(e->a));
        if (b && *b && kind != JASH_UNDO_LINK_CREATE &&
            kind != JASH_UNDO_FILE_OVERWRITE && kind != JASH_UNDO_DIR_REMOVE &&
            kind != JASH_UNDO_META && kind != JASH_UNDO_TIMES) {
            /* MOVE dst: canonical; LINK b (symlink target) stays literal;
             * OVERWRITE b (backup) and DIR_REMOVE/META b (data) already are */
            char cb[512]; tl_canon(b, cb, sizeof cb); strlcpy(e->b, cb, sizeof(e->b));
        } else if (b) {
            strlcpy(e->b, b, sizeof(e->b));
        }
    } else {
        if (a) strlcpy(e->a, a, sizeof(e->a));
        if (b) strlcpy(e->b, b, sizeof(e->b));
    }
    g_tl_count++;
    uint64_t id = e->id;

    /* Notify loaded modules of this mutation (ADR-0018 Wave 1).
     * Zero-cost when no module is loaded: one integer compare. */
#ifndef JASH_AGENT_ONLY
    if (g_modules.count > 0)
        jmod_hook_mutation((int)kind, desc, a, b);
#endif
    return id;
}

void jash_timeline_record(jash_undo_kind_t kind, const char* desc,
                           const char* a, const char* b) {
    (void)tl_record(kind, desc, a, b, 0);
}

int jash_timeline_record_ref(jash_undo_kind_t kind, const char* desc,
                              const char* a, const char* b, jash_recovery_ref_t* out) {
    if (!out) { jash_timeline_record(kind, desc, a, b); return -1; }
    memset(out, 0, sizeof(*out));
    if (!tl_enabled()) return -1;
    char canonical[512];
    if ((b && strlen(b) >= 512) || jash_recovery_canonical(a, canonical, sizeof(canonical))) return -1;
    char secondary[512];
    if (kind == JASH_UNDO_MOVE || kind == JASH_UNDO_TRASH) {
        if (!b || jash_recovery_canonical(b, secondary, sizeof(secondary))) return -1;
        b = secondary;
    }
    uint64_t id = tl_record(kind, desc, canonical, b, 1);
    if (!id) return -1;
    return jash_recovery_capture(id, kind, canonical, b, out);
}

void jash_timeline_consume_id(uint64_t id) {
    for (int i = 0; i < TIMELINE_MAX; i++) {
        timeline_entry_t* e = tl_slot(i);
        if (e && e->valid && e->id == id) e->valid = 0;
    }
    tl_note_consumed(id);
}

/* Copy a file byte-for-byte (pure syscalls). Returns 0 on success. */
static int tl_copy_file(const char* src, const char* dst, mode_t mode) {
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return -1;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, mode & 0777);
    if (dfd < 0) { close(sfd); return -1; }
    char buf[65536];
    ssize_t n;
    int ok = 1;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t wn = write(dfd, buf + off, (size_t)(n - off));
            if (wn <= 0) { ok = 0; break; }
            off += wn;
        }
        if (!ok) break;
    }
    if (n < 0) ok = 0;
    close(sfd);
    close(dfd);
    if (!ok) unlink(dst);
    return ok ? 0 : -1;
}

/* ADR-0050 W13: content-addressed backups — N overwrites of the SAME
 * content cost ONE copy. Named by SHA-256 of the content; an existing
 * backup with the same hash is reused (skip the copy). No refcount: backups
 * are never deleted individually (W11's reaper owns cleanup by age/pid), so
 * a shared file can never dangle. */
#include <module/jash_crypto.h>
#include <sys/syscalls.h>

#define TL_CAPTURE_SIZE 65536u
#define TL_BACKUP_MAX (16u * 1024u * 1024u)

typedef struct {
    int fd;
    struct stat st;
    size_t length;
    int captured;
    char hex[JASH_SHA256_HEX_LEN];
    unsigned char data[TL_CAPTURE_SIZE];
} tl_file_snapshot_t;

static ssize_t tl_read_retry(int fd, void* buf, size_t len) {
    ssize_t n;
    do { n = read(fd, buf, len); } while (n < 0 && errno == EINTR);
    return n;
}

static int tl_write_all(int fd, const void* data, size_t len) {
    const unsigned char* p = data;
    while (len) {
        ssize_t n = write(fd, p, len);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { if (!n) errno = EIO; return -1; }
        p += (size_t)n;
        len -= (size_t)n;
    }
    return 0;
}

/* A scoped directory handle avoids stale cached descriptors between commands.
 * O_PATH/O_SEARCH needs no read permission on a write/search-only store. */
static int tl_open_store(const char* path) {
#if defined(O_PATH) && !defined(JASH_TARGET_BEYOND)
    int flags = O_PATH | O_DIRECTORY | O_CLOEXEC;
#elif defined(O_SEARCH)
    int flags = O_SEARCH | O_DIRECTORY | O_CLOEXEC;
#else
    int flags = O_RDONLY | O_DIRECTORY | O_CLOEXEC;
#endif
    int fd;
    do { fd = open(path, flags); } while (fd < 0 && errno == EINTR);
    if (fd >= 0 || errno != ENOENT) return fd;
    if (mkdir(path, 0700) != 0 && errno != EEXIST) return -1;
    do { fd = open(path, flags); } while (fd < 0 && errno == EINTR);
    return fd;
}

/* Copy from an already-open, fstat-verified source fd. The fd pins the inode,
 * so a pathname swap after the initial open cannot change what is journaled. */
static int tl_copy_fd_to_path(int sfd, int dirfd, const char* dst, mode_t mode,
                              void* buf, size_t cap) {
    int dfd = openat(dirfd, dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, mode & 0777);
    if (dfd < 0) return -1;
    if (lseek(sfd, 0, SEEK_SET) < 0) { close(dfd); unlinkat(dirfd, dst, 0); return -1; }
    ssize_t n;
    int ok = 1;
    while ((n = tl_read_retry(sfd, buf, cap)) > 0) {
        if (tl_write_all(dfd, buf, (size_t)n) != 0) { ok = 0; break; }
    }
    if (n < 0) ok = 0;
    if (close(dfd) != 0) ok = 0;
    if (!ok) unlinkat(dirfd, dst, 0);
    return ok ? 0 : -1;
}

static int tl_copy_capture_to_path(const tl_file_snapshot_t* snapshot,
                                    int dirfd, const char* dst) {
    int fd = openat(dirfd, dst, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, snapshot->st.st_mode & 0777);
    if (fd < 0) return -1;
    int ok = tl_write_all(fd, snapshot->data, snapshot->length) == 0;
    if (close(fd) != 0) ok = 0;
    if (!ok) unlinkat(dirfd, dst, 0);
    return ok ? 0 : -1;
}

/* Keep small preimages in the same buffer used for hashing, so a dedup miss
 * never rereads their source. Larger inputs retain the pinned-fd copy path.
 * This changes neither the content-addressed format nor backup lifetime. */
static int tl_snapshot_to_fd(const char* path, tl_file_snapshot_t* snapshot) {
    snapshot->fd = -1;
    snapshot->length = 0;
    snapshot->captured = 0;
    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &snapshot->st) != 0 || !S_ISREG(snapshot->st.st_mode) ||
        snapshot->st.st_size < 0 || snapshot->st.st_size > TL_BACKUP_MAX) {
        close(fd);
        return -1;
    }

    jash_sha256_ctx_t ctx;
    jash_sha256_init(&ctx);
    ssize_t n;
    if (snapshot->st.st_size <= TL_CAPTURE_SIZE) {
        while (snapshot->length < sizeof snapshot->data) {
            n = tl_read_retry(fd, snapshot->data + snapshot->length,
                             sizeof snapshot->data - snapshot->length);
            if (n < 0) goto fail;
            if (n == 0) { snapshot->captured = 1; goto finish; }
            jash_sha256_update(&ctx, snapshot->data + snapshot->length, (size_t)n);
            snapshot->length += (size_t)n;
        }
        /* Distinguish an exact-fit file from one which grew after fstat. */
        unsigned char extra;
        n = tl_read_retry(fd, &extra, 1);
        if (n < 0) goto fail;
        if (n == 0) { snapshot->captured = 1; goto finish; }
        jash_sha256_update(&ctx, &extra, 1);
        snapshot->length++;
    }
    while ((n = tl_read_retry(fd, snapshot->data, sizeof snapshot->data)) > 0) {
        if ((size_t)n > TL_BACKUP_MAX - snapshot->length) goto fail;
        jash_sha256_update(&ctx, snapshot->data, (size_t)n);
        snapshot->length += (size_t)n;
    }
    if (n < 0) goto fail;
finish:;
    uint8_t d[JASH_SHA256_DIGEST_LEN];
    jash_sha256_final(&ctx, d);
    jash_sha256_hex(d, snapshot->hex);
    snapshot->st.st_size = (off_t)snapshot->length;
    snapshot->fd = fd;
    return 0;
fail:
    close(fd);
    return -1;
}

/* Recovery after an uncertain publication must not accept a same-sized file
 * from a replaced namespace merely because its name resembles the digest. */
static int tl_backup_matches(const char* path, const tl_file_snapshot_t* expected,
                              void* scratch, size_t cap) {
    /* A replacement FIFO must not block before fstat can reject its type. */
    int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
    if (fd < 0) return 0;
    struct stat st;
    int ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) &&
             st.st_size == expected->st.st_size;
    jash_sha256_ctx_t ctx;
    jash_sha256_init(&ctx);
    size_t total = 0;
    while (ok) {
        ssize_t n = tl_read_retry(fd, scratch, cap);
        if (n < 0) { ok = 0; break; }
        if (!n) break;
        if ((size_t)n > expected->length - total) { ok = 0; break; }
        jash_sha256_update(&ctx, scratch, (size_t)n);
        total += (size_t)n;
    }
    if (ok && total == expected->length) {
        unsigned char digest[32];
        char hex[65];
        jash_sha256_final(&ctx, digest);
        jash_sha256_hex(digest, hex);
        ok = strcmp(hex, expected->hex) == 0;
    } else ok = 0;
    close(fd);
    return ok;
}

int jash_timeline_backup_file(const char* path, char* out_backup,
                              size_t out_sz) {
    if (!path || !out_backup || !out_sz) return -1;
    out_backup[0] = '\0';
    tl_file_snapshot_t snapshot;
    if (tl_snapshot_to_fd(path, &snapshot) != 0) return -1;
    int fd = snapshot.fd;

    /* W13: the dedup store is SHARED — hash-named backups in per-pid
     * dirs can never dedup ACROSS calls (measured: two identical
     * overwrites from two -c calls cost two files). The shared dir is
     * never pid-reaped (W11 only reaps pid-shaped dirs, and the
     * referenced check protects live journals). */
    /* Hash backups only need the shared parent, not an unused per-pid dir. */
    tl_backup_dir_init();
    const char* bd = g_backup_dir;
    char shared[600];
    snprintf(shared, sizeof shared, "%s", bd);
    char* slash = strrchr(shared, '/');
    if (slash) *slash = '\0';
    strlcat(shared, "/shared", sizeof shared);
    int bn = snprintf(out_backup, out_sz, "%s/bk_%s", shared, snapshot.hex);
    if (bn < 0 || (size_t)bn >= out_sz) { close(fd); out_backup[0] = '\0'; return -1; }
    struct stat bst;
    if (stat(out_backup, &bst) == 0 && bst.st_size == snapshot.st.st_size) {
        close(fd);
        return 0;                       /* dedup hit: content already stored */
    }
    char tmp_backup[96];
    int tn = snprintf(tmp_backup, sizeof(tmp_backup), "bk_%s.tmp.%ld", snapshot.hex, (long)getpid());
    if (tn < 0 || (size_t)tn >= sizeof(tmp_backup)) { close(fd); return -1; }
    /* Captured bytes no longer need their source fd. Release it before opening
     * the store so the common path keeps the same peak descriptor count. */
    if (snapshot.captured) { close(fd); fd = -1; }
    int dirfd = tl_open_store(shared);
    if (dirfd < 0) {
        if (fd >= 0) close(fd);
        out_backup[0] = '\0';
        return -1;
    }
    int rc = snapshot.captured ? tl_copy_capture_to_path(&snapshot, dirfd, tmp_backup) :
        tl_copy_fd_to_path(fd, dirfd, tmp_backup, snapshot.st.st_mode,
                          snapshot.data, sizeof snapshot.data);
    if (fd >= 0) close(fd);
    if (rc == 0) {
        /* Source remains in the opened directory even if it was renamed. The
         * full destination resolves the current namespace at publication. */
        if (renameat(dirfd, tmp_backup, AT_FDCWD, out_backup) != 0) {
            rc = tl_backup_matches(out_backup, &snapshot, snapshot.data, sizeof snapshot.data) ? 0 : -1;
            unlinkat(dirfd, tmp_backup, 0);
        }
    }
    close(dirfd);
    if (rc != 0) out_backup[0] = '\0';
    return rc;
}

/* ADR-0050 W1: journal a write redirect (`>`/`>>`) BEFORE it fires.
 *
 * This is the wave that makes "undo anything" true: `echo x > file` is the
 * most common destructive op in any shell, and no other shell can journal
 * it — they would have to intercept libc. jash owns both redirect engines
 * (builtin_redir + pipeline), so it witnesses the truncation and keeps the
 * inverse:
 *   existing regular file → backup + FILE_OVERWRITE (restore pre-op content;
 *                           correct for `>>` too: drops the appended bytes)
 *   nonexistent path      → FILE_CREATE upfront (bash creates the file even
 *                           when the command fails, so the inverse unlink is
 *                           always right)
 * Callers must skip journaling when the redirect is guaranteed NOT to fire
 * (noclobber + existing file → the open fails). */
void jash_timeline_journal_redirect(const char* path, int append) {
    if (!tl_enabled()) return;
    if (!path || !*path) return;
    struct stat st;
    char backup[1024];
    if (stat(path, &st) == 0) {
        if (!S_ISREG(st.st_mode)) return;   /* dirs/devices: not our inverse */
        int has = jash_timeline_backup_file(path, backup, sizeof backup) == 0;
        char d[256];
        snprintf(d, sizeof d, "%s %s", append ? ">>" : ">", path);
        jash_timeline_record(JASH_UNDO_FILE_OVERWRITE, d, path,
                             has ? backup : "");
    } else if (errno == ENOENT) {
        char d[256];
        snprintf(d, sizeof d, "%s %s (new)", append ? ">>" : ">", path);
        jash_timeline_record(JASH_UNDO_FILE_CREATE, d, path, NULL);
    }
    /* other stat failures (EACCES…): the redirect itself will fail — journal
     * nothing, the shell will report the real error. */
}

/* ADR-0042 F8: find the backup of `path` as of `target_depth`.
 *
 * Insight: the content at target_depth is the content immediately BEFORE the
 * first timeline entry that affects `path` and has depth > target_depth.  If
 * that entry is an OVERWRITE, its `b` field is exactly that pre-modification
 * backup.  If it is CREATE/TRASH/MOVE, the file did not exist (or is
 * unknowable) at target_depth.  If no later entry exists, the file is
 * unchanged since target_depth — return the current path.
 */
int jash_timeline_backup_at(const char* path, int target_depth,
                            char* out, size_t cap) {
    if (!path || !out || cap == 0) return -1;
    out[0] = '\0';

    /* Entries store canonical absolute paths (tl_canon at record time); the
     * caller hands us the path AS TYPED (`tread h` → "h"). Without the same
     * canonicalization here a relative query never matched any entry and
     * tread silently served the CURRENT content (measured: F8.1). */
    char cpath[1200];
    tl_canon(path, cpath, sizeof cpath);

    struct stat st;
    if (!g_tl || g_tl_count == 0) {
        if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
            snprintf(out, cap, "%s", path);
            return 0;
        }
        return -1;
    }

    if (target_depth < 0) target_depth = 0;
    int cur = g_tl_count;
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;
    int oldest_depth = cur - live + 1;
    if (target_depth < oldest_depth) return -1; /* rotated out of ring */

    /* Index of the oldest entry strictly after target_depth (depth = target+1). */
    int i_start = cur - target_depth - 1;
    if (i_start < 0) i_start = -1; /* no entries after target */
    if (i_start >= live) i_start = live - 1;

    for (int i = i_start; i >= 0; i--) {
        timeline_entry_t* e = tl_slot(i);
        if (!e || !e->valid) continue;
        int a_match = (e->a[0] && !strcmp(e->a, cpath));
        int b_match = (e->b[0] && !strcmp(e->b, cpath));
        if (!a_match && !b_match) continue;

        switch (e->kind) {
        case JASH_UNDO_FILE_OVERWRITE:
            if (a_match) {
                snprintf(out, cap, "%s", e->b);
                return 0;
            }
            break;
        case JASH_UNDO_FILE_CREATE:
            if (a_match) return -1; /* created after target_depth */
            break;
        case JASH_UNDO_TRASH:
            if (a_match) return -1; /* deleted after target_depth */
            break;
        case JASH_UNDO_MOVE:
            if (a_match) return -1; /* moved away after target_depth */
            if (b_match) return -1; /* moved here after target_depth */
            break;
        default:
            break;
        }
    }

    /* No modification since target_depth: file is unchanged. */
    if (stat(path, &st) == 0 && S_ISREG(st.st_mode)) {
        snprintf(out, cap, "%s", path);
        return 0;
    }
    return -1;
}

/* ── Undo handlers (one per kind, each idempotent + honest) ─────────────── */

int jash_timeline_restore_cwd(jash_state_t* state, const char* target) {
    if (!target || !*target || strlen(target) >= sizeof state->cwd) { errno = ENAMETOOLONG; return -1; }
    int previous = open(".", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (previous < 0) return -1;
    if (chdir(target)) { int saved = errno; close(previous); errno = saved; return -1; }
    char actual[JASH_CWD_MAX];
    if (!getcwd(actual, sizeof actual)) {
        int saved = errno; if (fchdir(previous)) saved = EIO; close(previous); errno = saved; return -1;
    }
    const char* keys[] = {"OLDPWD", "PWD"};
    const char* values[] = {state->cwd, target[0] == '/' ? target : actual};
    if (jash_env_apply_batch(state, keys, values, 2)) {
        int saved = ENOMEM; if (fchdir(previous)) saved = EIO; close(previous); errno = saved; return -1;
    }
    memcpy(state->cwd, values[1], strlen(values[1]) + 1u);
    return close(previous);
}

static int undo_one(timeline_entry_t* e, jash_state_t* s,
                    jash_writer_t w, void* ctx) {
    switch (e->kind) {

    case JASH_UNDO_FILE_OVERWRITE: {
        /* a = original path, b = backup copy */
        struct stat st;
        if (stat(e->b, &st) != 0) {
            jash_writef(w, ctx,
                "undo: backup for '%s' vanished — cannot revert\r\n", e->a);
            return -1;
        }
        if (tl_copy_file(e->b, e->a, st.st_mode) != 0) {
            jash_writef(w, ctx, "undo: restore of '%s' failed\r\n", e->a);
            return -1;
        }
        jash_writef(w, ctx, "undo: restored %s\r\n", e->a);
        return 0;
    }

    case JASH_UNDO_FILE_CREATE: {
        /* a = created path: inverse is unlink (only if still a file). */
        struct stat st;
        if (stat(e->a, &st) != 0) {
            if (errno != ENOENT) return -1;
            jash_writef(w, ctx, "undo: '%s' already gone\r\n", e->a);
            return 0;       /* idempotent */
        }
        if (!S_ISREG(st.st_mode)) {
            jash_writef(w, ctx,
                "undo: '%s' is no longer a regular file — refusing\r\n", e->a);
            return -1;
        }
        if (unlink(e->a) != 0) {
            jash_writef(w, ctx, "undo: cannot remove '%s'\r\n", e->a);
            return -1;
        }
        jash_writef(w, ctx, "undo: removed %s\r\n", e->a);
        return 0;
    }

    case JASH_UNDO_MOVE: {
        /* a = src (original), b = dst (where it went): inverse rename. */
        struct stat st;
        if (stat(e->b, &st) != 0) {
            jash_writef(w, ctx,
                "undo: '%s' no longer exists — cannot move back\r\n", e->b);
            return -1;
        }
        if (stat(e->a, &st) == 0) {
            jash_writef(w, ctx,
                "undo: '%s' exists again — refusing to clobber\r\n", e->a);
            return -1;
        }
        if (rename(e->b, e->a) != 0) {
            jash_writef(w, ctx, "undo: move back failed for '%s'\r\n", e->b);
            return -1;
        }
        jash_writef(w, ctx, "undo: moved %s back to %s\r\n", e->b, e->a);
        return 0;
    }

    case JASH_UNDO_TRASH: {
        /* a = original path; ADR-0050 W5: pass the FULL original path —
         * trash restore matches it exactly (kills the basename collision
         * when two trashed files shared a name in different dirs). */
        char args[600];
        snprintf(args, sizeof(args), "restore %s", e->a);
        extern int jash_b_trash(jash_state_t*, jash_writer_t, void*, const char*);
        return jash_b_trash(s, w, ctx, args) == 0 ? 0 : -1;
    }

    case JASH_UNDO_ENV_SET: {
        /* a = name, b = old value ("" sentinel means it was unset). */
        if (e->b[0] == '\0' && e->desc[0] == '+') {
            if (jash_env_get(s, e->a) && jash_env_unset(s, e->a)) return -1;
            jash_writef(w, ctx, "undo: unset %s\r\n", e->a);
        } else {
            if (jash_env_set(s, e->a, e->b)) return -1;
            jash_writef(w, ctx, "undo: %s restored to '%s'\r\n", e->a, e->b);
        }
        return 0;
    }

    case JASH_UNDO_ENV_UNSET: {
        /* a = name, b = value it had. */
        if (jash_env_set(s, e->a, e->b)) return -1;
        jash_writef(w, ctx, "undo: %s=%s restored\r\n", e->a, e->b);
        return 0;
    }

    case JASH_UNDO_CHDIR: {
        /* a = previous cwd. */
        return jash_timeline_restore_cwd(s, e->a);
    }

    case JASH_UNDO_DIR_CREATE: {
        /* a = created dir: inverse is rmdir — refused when the dir gained
         * content after creation (never silently delete user data). */
        if (rmdir(e->a) != 0) {
            jash_writef(w, ctx,
                "undo: cannot rmdir '%s': %s (contents added after creation?)\r\n",
                e->a, strerror(errno));
            return -1;
        }
        jash_writef(w, ctx, "undo: removed dir %s\r\n", e->a);
        return 0;
    }

    case JASH_UNDO_DIR_REMOVE: {
        /* a = removed dir, b = octal mode it had (rmdir only takes empty
         * dirs, so restoring an empty dir is faithful). */
        long mode = e->b[0] ? strtol(e->b, NULL, 8) : 0755;
        if (mkdir(e->a, (mode_t)(mode & 07777)) != 0) {
            jash_writef(w, ctx, "undo: cannot recreate dir '%s': %s\r\n",
                        e->a, strerror(errno));
            return -1;
        }
        jash_writef(w, ctx, "undo: recreated dir %s\r\n", e->a);
        return 0;
    }

    case JASH_UNDO_META: {
        /* a = path, b = "mode(octal):uid:gid" before the change (-1 = untouched). */
        unsigned long mode_o = 0; long uid = -1, gid = -1;
        int got = sscanf(e->b, "%lo:%ld:%ld", &mode_o, &uid, &gid);
        if (got < 1) { errno = EINVAL; return -1; }
        if (got >= 1 && chmod(e->a, (mode_t)(mode_o & 07777)) != 0) {
            jash_writef(w, ctx, "undo: chmod back failed for '%s': %s\r\n",
                        e->a, strerror(errno));
            return -1;
        }
        if ((uid >= 0 || gid >= 0) && chown(e->a, (uid_t)uid, (gid_t)gid) != 0) {
            jash_writef(w, ctx, "undo: chown back failed for '%s': %s\r\n",
                        e->a, strerror(errno));
            return -1;
        }
        jash_writef(w, ctx, "undo: metadata of %s restored\r\n", e->a);
        return 0;
    }

    case JASH_UNDO_TIMES: {
        long long a, m; long an, mn; char extra;
        if (sscanf(e->b, "%lld:%ld:%lld:%ld%c", &a, &an, &m, &mn, &extra) != 4 ||
            an < 0 || an > 999999999 || mn < 0 || mn > 999999999) return -1;
        struct timespec times[2] = {{(time_t)a, an}, {(time_t)m, mn}};
        return utimensat(AT_FDCWD, e->a, times, AT_SYMLINK_NOFOLLOW);
    }

    case JASH_UNDO_LINK_CREATE: {
        /* a = link path: inverse is unlink (lstat — symlinks aren't regular). */
        struct stat st;
        if (lstat(e->a, &st) != 0) {
            if (errno != ENOENT) return -1;
            jash_writef(w, ctx, "undo: link '%s' already gone\r\n", e->a);
            return 0;       /* idempotent */
        }
        if (unlink(e->a) != 0) {
            jash_writef(w, ctx, "undo: cannot unlink '%s': %s\r\n",
                        e->a, strerror(errno));
            return -1;
        }
        jash_writef(w, ctx, "undo: unlinked %s\r\n", e->a);
        return 0;
    }

    case JASH_UNDO_PROC_SNAPSHOT: {
        /* a = pid, b = snapshot_path: inverse is delete snapshot file. */
        if (!e->b[0]) { errno = EINVAL; return -1; }
        if (unlink(e->b) && errno != ENOENT) return -1;
        jash_writef(w, ctx, "undo: deleted proc snapshot %s\r\n", e->b);
        return 0;
    }

    case JASH_UNDO_PROC_RESTORE: {
        /* a = snapshot_path, b = original_pid: inverse is terminate restored tree.
         * We don't auto-terminate; just log. User must manual kill. */
        jash_writef(w, ctx,
            "undo: proc restore from %s (pid %s) — manual cleanup required\r\n",
            e->a, e->b);
        errno = ENOTSUP;
        return -1;
    }

    default:
        jash_writef(w, ctx, "undo: unknown entry kind %d\r\n", (int)e->kind);
        return -1;
    }
}

/* ── Builtins: undo / timeline ───────────────────────────────────────────── */

static const char* kind_tag(jash_undo_kind_t k) {
    switch (k) {
        case JASH_UNDO_FILE_OVERWRITE: return "overwrite";
        case JASH_UNDO_FILE_CREATE:    return "create";
        case JASH_UNDO_MOVE:           return "move";
        case JASH_UNDO_TRASH:          return "trash";
        case JASH_UNDO_ENV_SET:        return "env+";
        case JASH_UNDO_ENV_UNSET:      return "env-";
        case JASH_UNDO_CHDIR:          return "cd";
        case JASH_UNDO_DIR_CREATE:     return "mkdir";
        case JASH_UNDO_DIR_REMOVE:     return "rmdir";
        case JASH_UNDO_META:           return "meta";
        case JASH_UNDO_TIMES:          return "times";
        case JASH_UNDO_LINK_CREATE:    return "link";
        case JASH_UNDO_PROC_SNAPSHOT:  return "procsnap";
        case JASH_UNDO_PROC_RESTORE:   return "procrst";
        default:                       return "?";
    }
}

/* W6: capture the pre-undo state so the op can be re-applied later.
 * Returns 1 when redo data is available (rb filled for content kinds). */
static int redo_capture(const timeline_entry_t* e, char* rb, size_t cap) {
    switch (e->kind) {
        case JASH_UNDO_TIMES: return 0; /* timestamp redo needs a separate preimage */
        case JASH_UNDO_FILE_OVERWRITE:
        case JASH_UNDO_FILE_CREATE:
            /* backup the content as it is RIGHT BEFORE the undo */
            return jash_timeline_backup_file(e->a, rb, cap) == 0;
        case JASH_UNDO_ENV_SET:
        case JASH_UNDO_ENV_UNSET: {
            const char* v = getenv(e->a);
            snprintf(rb, cap, "%s", v ? v : "");
            return 1;
        }
        case JASH_UNDO_CHDIR:
            return getcwd(rb, cap) != NULL;
        case JASH_UNDO_META: {
            struct stat st;
            if (stat(e->a, &st) != 0) return 0;
            snprintf(rb, cap, "%lo:%ld:%ld",
                     (unsigned long)(st.st_mode & 07777),
                     (long)st.st_uid, (long)st.st_gid);
            return 1;
        }
        case JASH_UNDO_PROC_SNAPSHOT:
        case JASH_UNDO_PROC_RESTORE:
            /* No payload needed for proc kinds; snapshot path is in b */
            rb[0] = '\0';
            return 1;
        default:                      /* MOVE, TRASH: no payload needed */
            rb[0] = '\0';
            return 1;
    }
}

static void redo_push(const timeline_entry_t* e, const char* rb) {
    timeline_entry_t* r = &g_redo[g_redo_count % TIMELINE_MAX];
    memset(r, 0, sizeof *r);
    r->kind  = e->kind;
    r->when  = time(NULL);
    r->id    = e->id;
    r->valid = 1;
    strlcpy(r->desc, e->desc, sizeof(r->desc));
    strlcpy(r->a, e->a, sizeof(r->a));
    if (e->kind == JASH_UNDO_MOVE) strlcpy(r->b, e->b, sizeof(r->b));
    else                           strlcpy(r->b, rb, sizeof(r->b));
    g_redo_count++;
}

/* W8: preview the next `count` reversions WITHOUT executing. */
static void undo_dry_run(jash_writer_t w, void* ctx, int count) {
    int shown = 0;
    jash_writef(w, ctx, "\033[1mundo --dry\033[0m — next %d reversion(s):\r\n", count);
    for (int i = 0; i < count; i++) {
        int kind; const char* a = NULL; const char* b = NULL;
        if (!jash_timeline_peek(i, &kind, &a, &b)) break;
        if (b && b[0])
            jash_writef(w, ctx, "  \033[33m%-10s\033[0m %s \033[90m← %s\033[0m\r\n",
                        kind_tag(kind), a, b);
        else
            jash_writef(w, ctx, "  \033[33m%-10s\033[0m %s\r\n", kind_tag(kind), a);
        shown++;
    }
    if (!shown) jash_writes(w, ctx, "  (nothing live to revert)\r\n");
    jash_writes(w, ctx, "\033[90m(dry run — nothing reverted)\033[0m\r\n");
}

/* Shared single-entry undo flow (LIFO top or selective by id):
 * capture redo data, undo, hold on failure, push redo on success. */
static int undo_entry_flow(timeline_entry_t* e, jash_state_t* s,
                           jash_writer_t w, void* ctx) {
    char rb[1024]; rb[0] = '\0';
    int have_redo = redo_capture(e, rb, sizeof rb);
    int rc = undo_one(e, s, w, ctx);
    if (rc != 0) return -1;                  /* W3: hold — entry survives */
    if (have_redo) redo_push(e, rb);
    return 0;
}

int jash_b_undo(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (args && (strstr(args, "--ref") || strstr(args, "--json") || strstr(args, "--inspect") ||
                 strstr(args, "--usage") || strstr(args, "--quota") || strstr(args, "--store") ||
                 strstr(args, "--gc") || strstr(args, "--pin") || strstr(args, "--unpin")))
        return jash_recovery_command(w, ctx, args);
    int count = 1, dry = 0, id_mode = 0;
    uint64_t want_id = 0;
    const char* p = args ? args : "";
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (!strncmp(p, "--dry-run", 9) && (!p[9] || p[9] == ' ' || p[9] == '\t')) { dry = 1; p += 9; }
        else if (!strncmp(p, "--dry", 5)) { dry = 1; p += 5; }
        else if (!strncmp(p, "-n", 2))    { dry = 1; p += 2; }
        else if (!strncmp(p, "id", 2) && (p[2] == ' ' || p[2] == '\t' || !p[2])) {
            id_mode = 1; p += 2;         /* `undo id N` — #N needs quotes (comment) */
        }
        else if (*p >= '0' && *p <= '9') {
            char* end; errno = 0;
            unsigned long long v = strtoull(p, &end, 10);
            if (errno || !v || (*end && *end != ' ' && *end != '\t')) return 2;
            if (id_mode || (p != args && p[-1] == '#')) want_id = v;
            else { if (v > TIMELINE_MAX) return 2; count = (int)v; }
            p = end;
        }
        else if (*p == '#') { id_mode = 1; p++; if (*p < '0' || *p > '9') return 2; }
        else return 2;
    }
    if (id_mode && !want_id) return 2;
    if (count < 1) count = 1;
    if (count > TIMELINE_MAX) count = TIMELINE_MAX;

    if (!g_tl || g_tl_count == 0) {
        jash_writes(w, ctx,
            "undo: nothing to undo in this session\r\n"
            "      (jash journals trash/rm/mv/cp-overwrite/redirect/export/unset/cd)\r\n");
        return 1;
    }

    /* W8: dry run never touches the world. */
    if (dry && want_id) {
        int index = tl_find_by_id(want_id);
        if (index < 0) return 1;
        timeline_entry_t* entry = tl_slot(index);
        if (!entry || !entry->valid) return 1;
        jash_writef(w, ctx, "undo --dry: #%llu %s %s (nothing reverted)\r\n",
                    (unsigned long long)entry->id, kind_tag(entry->kind), entry->a);
        return 0;
    }
    if (dry) { undo_dry_run(w, ctx, count); return 0; }

    /* W7: selective undo by entry id (see `timeline` for ids). */
    if (want_id) {
        int idx = tl_find_by_id(want_id);
        if (idx < 0) {
            jash_writef(w, ctx,
                idx == -2 ? "undo: ambiguous short id #%llu — use the full timeline entry_id\r\n" :
                "undo: no live entry #%llu — `timeline` lists live ids\r\n", (unsigned long long)want_id);
            return 1;
        }
        int pos = (g_tl_count - 1 - idx) % TIMELINE_MAX;
        if (pos < 0) pos += TIMELINE_MAX;
        timeline_entry_t* e = &g_tl[pos];
        if (undo_entry_flow(e, s, w, ctx) != 0) {
            jash_writef(w, ctx,
                "undo: #%llu failed (entry kept — fix the world and retry)\r\n", (unsigned long long)want_id);
            return 1;
        }
        tl_note_consumed(e->id);             /* W10: merge must not resurrect */
        e->valid = 0;                        /* selective: slot freed, others stay */
        jash_writef(w, ctx, "undo: #%llu reverted, 0 failed\r\n", (unsigned long long)want_id);
        return 0;
    }

    int done = 0, failed = 0;
    for (int i = 0; i < count; i++) {
        timeline_entry_t* e = tl_slot(0);
        if (!e) break;
        if (!e->valid) { g_tl_count--; i--; continue; }  /* skip freed slots */
        if (undo_entry_flow(e, s, w, ctx) != 0) {
            failed++;
            break;                           /* W3: hold — entry survives */
        }
        tl_note_consumed(e->id);             /* W10 */
        e->valid = 0;
        g_tl_count--;
        done++;
    }
    if (done == 0 && failed == 0) {
        jash_writes(w, ctx, "undo: nothing to undo\r\n");
        return 1;
    }
    /* BUG-009: always emit a final summary line so `undo` is DETERMINISTIC —
     * env/chdir reverts print nothing themselves, so without this an `undo`
     * that reverted only a silent entry produced empty output (flaky test +
     * confusing UX: "did it do anything?"). */
    jash_writef(w, ctx, "undo: %d reverted, %d failed%s\r\n", done, failed,
                failed ? " (entry kept — fix and retry)" : "");
    /* ADR-0050 W14: attested undo — the chain records the REVERSAL itself */
    if (done > 0) {
        char det[160];
        snprintf(det, sizeof det, "undo reverted=%d failed=%d", done, failed);
        jash_sigil_attest("undo", det);
    }
    return failed ? 1 : 0;
}

/* ADR-0050 W6: redo — re-apply what undo just reverted. */
static int redo_one(timeline_entry_t* r, jash_state_t* s,
                    jash_writer_t w, void* ctx) {
    switch (r->kind) {
        case JASH_UNDO_FILE_OVERWRITE: {
            struct stat st;
            if (stat(r->b, &st) != 0) {
                jash_writef(w, ctx, "redo: snapshot for '%s' vanished\r\n", r->a);
                return -1;
            }
            char nb[1024];
            int has = jash_timeline_backup_file(r->a, nb, sizeof nb) == 0;
            if (tl_copy_file(r->b, r->a, st.st_mode) != 0) {
                jash_writef(w, ctx, "redo: restore of '%s' failed\r\n", r->a);
                return -1;
            }
            char d[256]; snprintf(d, sizeof d, "redo overwrite %s", r->a);
            jash_timeline_record(JASH_UNDO_FILE_OVERWRITE, d, r->a, has ? nb : "");
            jash_writef(w, ctx, "redo: re-applied overwrite to %s\r\n", r->a);
            return 0;
        }
        case JASH_UNDO_FILE_CREATE: {
            struct stat st;
            if (stat(r->b, &st) != 0) {
                jash_writef(w, ctx, "redo: snapshot for '%s' vanished\r\n", r->a);
                return -1;
            }
            if (tl_copy_file(r->b, r->a, st.st_mode) != 0) {
                jash_writef(w, ctx, "redo: recreate of '%s' failed\r\n", r->a);
                return -1;
            }
            char d[256]; snprintf(d, sizeof d, "redo create %s", r->a);
            jash_timeline_record(JASH_UNDO_FILE_CREATE, d, r->a, NULL);
            jash_writef(w, ctx, "redo: recreated %s\r\n", r->a);
            return 0;
        }
        case JASH_UNDO_MOVE: {
            if (rename(r->a, r->b) != 0) {
                jash_writef(w, ctx, "redo: re-move %s → %s failed\r\n", r->a, r->b);
                return -1;
            }
            char d[256]; snprintf(d, sizeof d, "redo move %s → %s", r->a, r->b);
            jash_timeline_record(JASH_UNDO_MOVE, d, r->a, r->b);
            jash_writef(w, ctx, "redo: moved %s → %s\r\n", r->a, r->b);
            return 0;
        }
        case JASH_UNDO_TRASH: {
            extern int jash_b_rm(jash_state_t*, jash_writer_t, void*, int, char**);
            char* rargv[3];
            rargv[0] = (char*)"rm";
            rargv[1] = r->a;
            rargv[2] = NULL;
            return jash_b_rm(s, w, ctx, 2, rargv) == 0 ? 0 : -1;
        }
        case JASH_UNDO_ENV_SET: {
            char prev[512];
            const char* cur = getenv(r->a);
            snprintf(prev, sizeof prev, "%s", cur ? cur : "");
            if (jash_env_set(s, r->a, r->b)) return -1;
            char d[256]; snprintf(d, sizeof d, "redo export %s", r->a);
            jash_timeline_record(JASH_UNDO_ENV_SET, d, r->a, prev);
            jash_writef(w, ctx, "redo: %s restored to '%s'\r\n", r->a, r->b);
            return 0;
        }
        case JASH_UNDO_ENV_UNSET: {
            if (jash_env_get(s, r->a) && jash_env_unset(s, r->a)) return -1;
            char d[256]; snprintf(d, sizeof d, "redo unset %s", r->a);
            jash_timeline_record(JASH_UNDO_ENV_UNSET, d, r->a, r->b);
            jash_writef(w, ctx, "redo: unset %s\r\n", r->a);
            return 0;
        }
        case JASH_UNDO_CHDIR: {
            char before[JASH_CWD_MAX]; memcpy(before, s->cwd, sizeof before);
            if (jash_timeline_restore_cwd(s, r->b)) return -1;
            jash_timeline_record(JASH_UNDO_CHDIR, "redo cd", before, NULL);
            return 0;
        }
        case JASH_UNDO_DIR_CREATE: {
            if (mkdir(r->a, 0755) != 0) return -1;
            char d[256]; snprintf(d, sizeof d, "redo mkdir %s", r->a);
            jash_timeline_record(JASH_UNDO_DIR_CREATE, d, r->a, NULL);
            jash_writef(w, ctx, "redo: recreated dir %s\r\n", r->a);
            return 0;
        }
        case JASH_UNDO_DIR_REMOVE: {
            if (rmdir(r->a) != 0) return -1;
            char d[256]; snprintf(d, sizeof d, "redo rmdir %s", r->a);
            jash_timeline_record(JASH_UNDO_DIR_REMOVE, d, r->a, r->b);
            jash_writef(w, ctx, "redo: removed dir %s\r\n", r->a);
            return 0;
        }
        case JASH_UNDO_META: {
            /* rb (captured at undo time) = the mode/uid/gid the op had set */
            unsigned long mode_o = 0; long uid = -1, gid = -1;
            int got = sscanf(r->b, "%lo:%ld:%ld", &mode_o, &uid, &gid);
            char prev[64]; struct stat st;
            if (stat(r->a, &st) == 0)
                snprintf(prev, sizeof prev, "%lo:%ld:%ld",
                         (unsigned long)(st.st_mode & 07777),
                         (long)st.st_uid, (long)st.st_gid);
            else prev[0] = '\0';
            if (got >= 1 && chmod(r->a, (mode_t)(mode_o & 07777)) != 0) return -1;
            if ((uid >= 0 || gid >= 0) && chown(r->a, (uid_t)uid, (gid_t)gid) != 0) return -1;
            char d[256]; snprintf(d, sizeof d, "redo meta %s", r->a);
            jash_timeline_record(JASH_UNDO_META, d, r->a, prev);
            jash_writef(w, ctx, "redo: metadata of %s re-applied\r\n", r->a);
            return 0;
        }
        case JASH_UNDO_LINK_CREATE: {
            /* r->b = "s:target" or "h:target" recorded at creation */
            const char* tgt = r->b + 2;
            int rc = (r->b[0] == 's') ? symlink(tgt, r->a) : link(tgt, r->a);
            if (rc != 0) return -1;
            char d[256]; snprintf(d, sizeof d, "redo link %s", r->a);
            jash_timeline_record(JASH_UNDO_LINK_CREATE, d, r->a, r->b);
            jash_writef(w, ctx, "redo: relinked %s\r\n", r->a);
            return 0;
        }
        case JASH_UNDO_PROC_SNAPSHOT: {
            /* a = pid, b = snapshot_path: redo is re-take snapshot. */
            int pid = atoi(r->a);
            if (pid > 0 && jash_timeline_proc_snapshot(pid, r->b, 512) == 0) {
                jash_writef(w, ctx, "redo: re-snapshotted pid %d to %s\r\n", pid, r->b);
                return 0;
            }
            return -1;
        }
        case JASH_UNDO_PROC_RESTORE: {
            /* a = snapshot_path, b = original_pid: redo is re-restore. */
            int new_pid;
            if (jash_timeline_proc_restore(r->a, &new_pid) == 0) {
                jash_writef(w, ctx, "redo: re-restored from %s as pid %d\r\n", r->a, new_pid);
                return 0;
            }
            return -1;
        }
        default:
            return -1;
    }
}

int jash_b_redo(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int count = 1;
    const char* p = args ? args : "";
    while (*p == ' ' || *p == '\t') p++;
    if (*p >= '1' && *p <= '9') count = atoi(p);
    if (count < 1) count = 1;
    if (count > TIMELINE_MAX) count = TIMELINE_MAX;

    if (!g_redo || g_redo_count == 0) {
        jash_writes(w, ctx,
            "redo: nothing to redo\r\n"
            "      (redo lives as long as the session; any new mutation clears it)\r\n");
        return 1;
    }
    int done = 0, failed = 0;
    for (int i = 0; i < count; i++) {
        if (g_redo_count == 0) break;
        timeline_entry_t* r = &g_redo[(g_redo_count - 1) % TIMELINE_MAX];
        if (!r->valid) { g_redo_count--; i--; continue; }
        g_reapplying_redo++;
        int rc = redo_one(r, s, w, ctx);
        g_reapplying_redo--;
        if (rc != 0) { failed++; break; }  /* hold */
        r->valid = 0;
        g_redo_count--;
        done++;
    }
    jash_writef(w, ctx, "redo: %d re-applied, %d failed%s\r\n", done, failed,
                failed ? " (entry kept)" : "");
    if (done > 0) {
        char det[160];
        snprintf(det, sizeof det, "redo reapplied=%d failed=%d", done, failed);
        jash_sigil_attest("redo", det);
    }
    return failed ? 1 : 0;
}

/* ── ADR-0023 E2: rewind — time-travel to a checkpoint ───────────────────────
 * The timeline IS a stack of reversible mutations; a checkpoint is just a
 * remembered stack depth. Rewinding pops+inverts entries until the depth is
 * reached, reusing the exact undo_one machinery (so coverage == `undo`:
 * trash/rm, mv, export/unset, cd). g_tl_count is preserved across `-c` by
 * save/restore as long as live entries ≤ TIMELINE_MAX, so a depth captured in
 * one tool-call is still meaningful in the next. */
int jash_timeline_depth(void) { return g_tl_count; }

int jash_timeline_lost(void) { return tl_lost_to_ring(); }

int jash_timeline_rewind_to(int target_depth, int* out_failed,
                            jash_state_t* s, jash_writer_t w, void* ctx) {
    int done = 0, failed = 0;
    if (target_depth < 0) target_depth = 0;
    while (g_tl && g_tl_count > target_depth) {
        timeline_entry_t* e = tl_slot(0);
        if (!e) { g_tl_count--; continue; }
        if (!e->valid) { g_tl_count--; continue; }   /* freed slot: skip */
        /* W3: hold-on-failure — a failed reversion STOPS the rewind and
         * keeps the entry (and everything older) for inspection/retry,
         * instead of consuming the journal and pretending completeness. */
        char rb[1024]; rb[0] = '\0';
        int have_redo = redo_capture(e, rb, sizeof rb);
        int rc = undo_one(e, s, w, ctx);
        if (rc != 0) { failed++; break; }
        if (have_redo) redo_push(e, rb);
        tl_note_consumed(e->id);             /* W10 */
        e->valid = 0;
        g_tl_count--;
        done++;
    }
    if (out_failed) *out_failed = failed;
    return done;
}

/* PATCH-SPEC jash-rewind-scope (2026-08-01): scoped rewind — revert ONLY
 * mutations under <scope> (canonical absolute prefix). Out-of-scope entries
 * are NOT reverted and NOT consumed: they stay valid in place (same freed-
 * slot mechanics as `undo id N`), so global depths and existing checkpoints
 * keep their meaning and no sigil invariant is touched. dry=1 lists
 * would-revert/would-keep without touching the world. Returns reversions. */
int jash_timeline_rewind_scope(int target_depth, const char* scope,
                               int dry,
                               int* out_failed, int* out_kept,
                               jash_state_t* s, jash_writer_t w, void* ctx) {
    if (target_depth < 0) target_depth = 0;
    size_t sl = strlen(scope);
    int start = g_tl_count;
    int done = 0, failed = 0, kept = 0;
    for (int idx = 0; g_tl && (start - idx) > target_depth; idx++) {
        timeline_entry_t* e = tl_slot(idx);
        if (!e) break;
        if (!e->valid) continue;
        int in = 0;
        if (tl_kind_has_paths(e->kind) && e->a[0] &&
            !strncmp(e->a, scope, sl) &&
            (e->a[sl] == '\0' || e->a[sl] == '/')) in = 1;
        if (!in && e->kind == JASH_UNDO_MOVE && e->b[0] &&
            !strncmp(e->b, scope, sl) &&
            (e->b[sl] == '\0' || e->b[sl] == '/')) in = 1;  /* move INTO scope */
        if (!in) {
            kept++;
            if (dry)
                jash_writef(w, ctx, "  \033[90mkeep   %-10s %s\033[0m\r\n",
                            kind_tag(e->kind), e->a);
            continue;
        }
        if (dry) {
            jash_writef(w, ctx, "  \033[33mrevert %-10s\033[0m %s\r\n",
                        kind_tag(e->kind), e->a);
            done++;
            continue;
        }
        char rb[1024]; rb[0] = '\0';
        int have_redo = redo_capture(e, rb, sizeof rb);
        int rc = undo_one(e, s, w, ctx);
        if (rc != 0) { failed++; break; }   /* hold-on-failure (spec gate 5) */
        if (have_redo) redo_push(e, rb);
        tl_note_consumed(e->id);
        e->valid = 0;                        /* freed slot; depths stay */
        done++;
    }
    if (out_failed) *out_failed = failed;
    if (out_kept) *out_kept = kept;
    return done;
}

int jash_b_timeline(jash_state_t* s, jash_writer_t w, void* ctx,
                    const char* args) {
    (void)s;
    jash_argv_t av = {0};
    if (jash_argv_build(args ? args : "", &av) < 0) return 2;
    int json_mode = 0;
    uint64_t offset = 0, limit = TIMELINE_MAX;
    int invalid = 0;
    for (int i = 0; i < av.argc; i++) {
        if (!strcmp(av.argv[i], "--json")) json_mode = 1;
        else if (!strcmp(av.argv[i], "--offset") || !strcmp(av.argv[i], "--limit")) {
            int is_limit = !strcmp(av.argv[i], "--limit");
            if (++i >= av.argc || !av.argv[i][0]) { invalid = 1; break; }
            uint64_t value = 0;
            for (const char* p = av.argv[i]; *p; p++) {
                if (*p < '0' || *p > '9' || value > (UINT64_MAX - (unsigned)(*p - '0')) / 10) { invalid = 1; break; }
                value = value * 10 + (unsigned)(*p - '0');
            }
            if (is_limit) { limit = value; if (!limit) invalid = 1; } else offset = value;
        } else invalid = 1;
    }
    jash_argv_free(&av);
    if (invalid || (!json_mode && (offset || limit != TIMELINE_MAX))) return 2;

    /* JINV-050 — a mesma linha do tempo, legivel por maquina.
     *
     * O texto ja carrega tudo: hora, tipo da mutacao, descricao, e qual
     * entrada o `undo` reverteria. Lendo TEXTO, porem, um agente volta a
     * parsear com regex — a raiz que o JINV-047 atacou.
     *
     * Sai dos MESMOS slots que o texto (`tl_slot`), entao nao ha segunda
     * fonte de verdade e as duas saidas nao podem divergir. `undoes` marca a
     * entrada que o `undo` reverteria — o texto ja dizia isso em prosa
     * ("← undo reverts this"); aqui vira campo. */
    if ((!g_tl || g_tl_count == 0) && !json_mode) {
        jash_writes(w, ctx, "timeline: no destructive operations recorded "
                                 "this session\r\n");
        return 0;
    }
    int live = g_tl_count < TIMELINE_MAX ? g_tl_count : TIMELINE_MAX;

    if (json_mode) {
        struct sigaction old_pipe, ignore_pipe = {0};
        ignore_pipe.sa_handler = SIG_IGN; sigemptyset(&ignore_pipe.sa_mask);
        if (sigaction(SIGPIPE, &ignore_pipe, &old_pipe)) return 125;
        jash_budget_unwrap(&w, &ctx); jash_redir_stream_writer(w, ctx);
        jash_json_writer_t out; jash_jsonw_init(&out, w, ctx);
        jash_jsonw_raw(&out, "{\"schema\":\"jash.timeline.v1\",\"record_fields\":\"legacy_bounded\",\"mutations\":");
        jash_jsonw_u64(&out, jash_timeline_count());
        jash_jsonw_raw(&out, ",\"lost\":"); jash_jsonw_u64(&out, tl_lost_to_ring());
        jash_jsonw_raw(&out, ",\"entries\":[");
        uint64_t seen = 0, emitted = 0;
        int newest = -1;
        for (int i = 0; i < live; i++) { timeline_entry_t* e = tl_slot(i); if (e && e->valid) { newest = i; break; } }
        for (int i = live - 1; i >= 0 && !out.error; i--) {
            timeline_entry_t* e = tl_slot(i);
            if (!e || !e->valid) continue;
            if (seen++ < offset || emitted >= limit) continue;
            if (emitted++) jash_jsonw_raw(&out, ",");
            jash_jsonw_raw(&out, "{\"id\":"); jash_jsonw_safe_u64(&out, e->id);
            jash_jsonw_raw(&out, ",\"entry_id\":\""); jash_jsonw_u64(&out, e->id);
            jash_jsonw_raw(&out, "\",\"kind\":"); jash_jsonw_string(&out, kind_tag(e->kind));
            jash_jsonw_raw(&out, ",\"desc\":"); jash_jsonw_string_or_bytes(&out, e->desc);
            jash_jsonw_raw(&out, ",\"a\":"); jash_jsonw_string_or_bytes(&out, e->a);
            jash_jsonw_raw(&out, ",\"b\":"); jash_jsonw_string_or_bytes(&out, e->b);
            jash_jsonw_raw(&out, ",\"ts\":"); jash_jsonw_safe_i64(&out, e->when);
            jash_jsonw_raw(&out, ",\"undoes\":"); jash_jsonw_raw(&out, i == newest ? "true" : "false");
            jash_jsonw_raw(&out, "}");
        }
        jash_jsonw_raw(&out, "],\"count\":"); jash_jsonw_u64(&out, emitted);
        jash_jsonw_raw(&out, ",\"next_offset\":");
        if (offset < seen && emitted < seen - offset) jash_jsonw_u64(&out, offset + emitted);
        else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, "}\n");
        int rc = jash_jsonw_finish(&out) ? 125 : 0;
        if (sigaction(SIGPIPE, &old_pipe, NULL)) rc = 125;
        return rc;
    }

    jash_writef(w, ctx,
        "\033[1;36m═══ SESSION TIMELINE — %d mutation(s)%s ═══\033[0m\r\n",
        jash_timeline_count(),
        tl_lost_to_ring() ? " · ring-lost older entries" : "");
    if (tl_lost_to_ring())
        jash_writef(w, ctx,
            "\033[90m  (%d older entr%s lost to the %d-entry ring)\033[0m\r\n",
            tl_lost_to_ring(), tl_lost_to_ring() == 1 ? "y" : "ies",
            TIMELINE_MAX);
    for (int i = live - 1; i >= 0; i--) {
        timeline_entry_t* e = tl_slot(i);
        if (!e || !e->valid) continue;
        struct tm* tm = localtime(&e->when);
        char hhmm[8];
        strftime(hhmm, sizeof(hhmm), "%H:%M", tm);
        char idbuf[40];
        unsigned hip = (unsigned)(e->id >> 32);
        if (hip == (unsigned)getpid())
            snprintf(idbuf, sizeof idbuf, "#%llu", e->id & 0xFFFFFFFFu);
        else
            snprintf(idbuf, sizeof idbuf, "#%llu@p%u", e->id & 0xFFFFFFFFu, hip);
        jash_writef(w, ctx, "  %s \033[90m%-10s\033[0m \033[33m%-9s\033[0m %s%s\r\n",
                    hhmm, idbuf, kind_tag(e->kind), e->desc,
                    i == 0 ? "   \033[90m← undo reverts this\033[0m" : "");
    }
    return 0;
}

#include "timeline_native.inc"

/* ── Wave 2: Process Snapshots (Caminho A — /proc-based) ───────────────────── */

#include <dirent.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <errno.h>

/* Checked append for snapshot JSON. On failure, used is unchanged and out
 * remains terminated, so callers cannot turn snprintf's would-have-written
 * length into an out-of-bounds offset. */
static int proc_appendf(char* out, size_t cap, size_t* used,
                        const char* fmt, ...)
    __attribute__((format(printf, 4, 5)));

static int proc_appendf(char* out, size_t cap, size_t* used,
                        const char* fmt, ...) {
    if (!out || !used || cap == 0 || *used >= cap) {
        if (out && cap) out[cap - 1] = '\0';
        return -1;
    }
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(out + *used, cap - *used, fmt, ap);
    va_end(ap);
    if (n < 0 || (size_t)n >= cap - *used) {
        out[cap - 1] = '\0';
        return -1;
    }
    *used += (size_t)n;
    return 0;
}

static int proc_append_json_string(char* out, size_t cap, size_t* used,
                                   const char* in) {
    static const char hex[] = "0123456789abcdef";
    if (proc_appendf(out, cap, used, "\"") != 0) return -1;
    for (const unsigned char* p = (const unsigned char*)in; *p; p++) {
        const char* escape = NULL;
        switch (*p) {
            case '"':  escape = "\\\""; break;
            case '\\': escape = "\\\\"; break;
            case '\b': escape = "\\b"; break;
            case '\f': escape = "\\f"; break;
            case '\n': escape = "\\n"; break;
            case '\r': escape = "\\r"; break;
            case '\t': escape = "\\t"; break;
        }
        if (escape) {
            if (proc_appendf(out, cap, used, "%s", escape) != 0) return -1;
        } else if (*p < 0x20) {
            if (proc_appendf(out, cap, used, "\\u00%c%c",
                             hex[*p >> 4], hex[*p & 0xf]) != 0) return -1;
        } else if (proc_appendf(out, cap, used, "%c", *p) != 0) {
            return -1;
        }
    }
    return proc_appendf(out, cap, used, "\"");
}

/* Internal: read /proc/<pid>/cmdline and encode its NUL-separated argv as JSON. */
static int proc_read_argv_json(int pid, char* out, size_t cap) {
    char path[128];
    snprintf(path, sizeof path, "/proc/%d/cmdline", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char raw[8192];
    size_t got = 0;
    while (got < sizeof raw) {
        ssize_t n = read(fd, raw + got, sizeof raw - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); return -1; }
        if (n == 0) break;
        got += (size_t)n;
    }
    if (got == sizeof raw) {
        char extra;
        ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n != 0) { close(fd); return -1; }
    }
    close(fd);
    if (got == 0 || raw[got - 1] != '\0' || cap < 3)
        return -1;

    static const char hex[] = "0123456789abcdef";
    size_t o = 0, pos = 0;
    int first = 1;
    out[o++] = '[';
    while (pos < got) {
        size_t len = strnlen(raw + pos, got - pos);
        if (pos + len >= got || o + 3 >= cap) return -1;
        if (!first) out[o++] = ',';
        first = 0;
        out[o++] = '"';
        for (size_t i = 0; i < len; i++) {
            unsigned char c = (unsigned char)raw[pos + i];
            if (c == '"' || c == '\\') {
                if (o + 2 >= cap) return -1;
                out[o++] = '\\'; out[o++] = (char)c;
            } else if (c == '\b' || c == '\f' || c == '\n' ||
                       c == '\r' || c == '\t') {
                if (o + 2 >= cap) return -1;
                out[o++] = '\\';
                out[o++] = c == '\b' ? 'b' : c == '\f' ? 'f' :
                           c == '\n' ? 'n' : c == '\r' ? 'r' : 't';
            } else if (c < 0x20) {
                if (o + 6 >= cap) return -1;
                out[o++] = '\\'; out[o++] = 'u'; out[o++] = '0'; out[o++] = '0';
                out[o++] = hex[c >> 4]; out[o++] = hex[c & 0xf];
            } else {
                if (o + 1 >= cap) return -1;
                out[o++] = (char)c;
            }
        }
        if (o + 1 >= cap) return -1;
        out[o++] = '"';
        pos += len + 1;
    }
    if (o + 2 > cap) return -1;
    out[o++] = ']';
    out[o] = '\0';
    return 0;
}

/* Internal: read /proc/<pid>/environ → env array (null-separated) */
static int proc_read_environ(int pid, char* out, size_t cap) {
    if (cap < 2) {
        if (cap) out[0] = '\0';
        return -2;
    }
    char path[128];
    snprintf(path, sizeof path, "/proc/%d/environ", pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    size_t got = 0;
    while (got < cap - 1) {
        ssize_t n = read(fd, out + got, cap - 1 - got);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) { close(fd); out[0] = '\0'; return -1; }
        if (n == 0) break;
        got += (size_t)n;
    }
    if (got == cap - 1) {
        char extra;
        ssize_t n;
        do { n = read(fd, &extra, 1); } while (n < 0 && errno == EINTR);
        if (n != 0) { close(fd); out[cap - 1] = '\0'; return n > 0 ? -2 : -1; }
    }
    close(fd);
    if (got >= cap) { out[cap - 1] = '\0'; return -2; }
    out[got] = '\0';
    if (got == 0 || out[got - 1] != '\0') return -1;
    return 0;
}

/* Internal: read /proc/<pid>/fd/ → list of fds with targets */
static int proc_read_fds(int pid, char* out, size_t cap) {
    if (cap == 0) return -2;
    out[0] = '\0';
    char path[128];
    int path_n = snprintf(path, sizeof path, "/proc/%d/fd", pid);
    if (path_n < 0 || (size_t)path_n >= sizeof path) return -1;
    DIR* d = opendir(path);
    if (!d) return -1;
    size_t o = 0;
    if (proc_appendf(out, cap, &o, "[") != 0) { closedir(d); return -2; }
    struct dirent* ent;
    int first = 1;
    while ((ent = readdir(d))) {
        if (ent->d_name[0] == '.') continue;
        int fd = atoi(ent->d_name);
        if (fd <= 0) continue;
        char linkpath[256], target[512];
        int link_n = snprintf(linkpath, sizeof linkpath,
                              "/proc/%d/fd/%s", pid, ent->d_name);
        if (link_n < 0 || (size_t)link_n >= sizeof linkpath) {
            closedir(d);
            return -1;
        }
        ssize_t tlen = readlink(linkpath, target, sizeof target - 1);
        if (tlen < 0) continue;
        if ((size_t)tlen >= sizeof target - 1) { closedir(d); return -2; }
        target[tlen] = '\0';
        if (!first && proc_appendf(out, cap, &o, ",") != 0) {
            closedir(d);
            return -2;
        }
        first = 0;
        if (proc_appendf(out, cap, &o, "{\"fd\":%d,\"target\":", fd) != 0 ||
            proc_append_json_string(out, cap, &o, target) != 0 ||
            proc_appendf(out, cap, &o, "}") != 0) {
            closedir(d);
            return -2;
        }
    }
    closedir(d);
    if (proc_appendf(out, cap, &o, "]") != 0) return -2;
    return first ? -1 : 0;  /* -1 if no FDs found */
}

/* Internal: walk /proc to build PID tree rooted at root_pid */
__attribute__((used)) static int proc_walk_tree(int root_pid, char* out, size_t cap, int depth) {
    char path[128];
    snprintf(path, sizeof path, "/proc/%d/task/%d/children", root_pid, root_pid);
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        /* Fallback: scan /proc for PPID == root_pid */
        return 0;  /* Simplified: just return empty children for now */
    }
    char buf[4096];
    ssize_t n = read(fd, buf, sizeof buf - 1);
    close(fd);
    if (n <= 0) return 0;
    buf[n] = '\0';
    return 0;  /* Simplified for now */
}

/* Recursively serialize a process and its children */
static int proc_serialize_tree(int pid, char* out, size_t cap) {
    if (cap == 0) return -1;
    out[0] = '\0';
    char argv_json[16384], environment[16384], fds[4096];
    int has_argv = (proc_read_argv_json(pid, argv_json, sizeof argv_json) == 0);
    int env_rc = proc_read_environ(pid, environment, sizeof environment);
    int fds_rc = proc_read_fds(pid, fds, sizeof fds);
    int has_env = env_rc == 0;
    int has_fds = fds_rc == 0;
    if (env_rc == -2 || fds_rc == -2) return -1;
    if (!has_argv) return -1;

    size_t o = 0;
    if (proc_appendf(out, cap, &o, "{\"pid\":%d,\"argv\":%s,",
                     pid, argv_json) != 0) return -1;
    if (has_fds && proc_appendf(out, cap, &o, "\"fds\":%s,", fds) != 0)
        return -1;
    if (has_env) {
        /* Parse environ into JSON array */
        if (proc_appendf(out, cap, &o, "\"env\":[") != 0) return -1;
        int first = 1;
        for (char* p = environment; *p; p += strlen(p) + 1) {
            if (!first && proc_appendf(out, cap, &o, ",") != 0) return -1;
            first = 0;
            if (proc_append_json_string(out, cap, &o, p) != 0) return -1;
        }
        if (proc_appendf(out, cap, &o, "],") != 0) return -1;
    }
    /* Children placeholder */
    return proc_appendf(out, cap, &o, "\"children\":[]}");
}

/* Open the exact private regular file observed by lstat. */
static int proc_snapshot_open(const char* path, struct stat* out_st) {
    struct stat before, after;
    if (lstat(path, &before) != 0) return -1;
    if (!S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
        (before.st_mode & 077) != 0) return -1;

    int fd = open(path, O_RDONLY | O_NOFOLLOW);
    if (fd < 0) return -1;
    if (fstat(fd, &after) != 0 || !S_ISREG(after.st_mode) ||
        after.st_dev != before.st_dev || after.st_ino != before.st_ino ||
        after.st_uid != geteuid() || (after.st_mode & 077) != 0) {
        close(fd);
        return -1;
    }
    if (out_st) *out_st = after;
    return fd;
}

static int proc_snapshot_exchange(int dirfd, const char* a, const char* b) {
#if defined(__linux__)
    return jash_osif_renameat2(dirfd, a, dirfd, b, RENAME_EXCHANGE);
#elif defined(__APPLE__)
    return renameatx_np(dirfd, a, dirfd, b, RENAME_SWAP);
#else
    (void)dirfd; (void)a; (void)b;
    errno = ENOTSUP;
    return -1;
#endif
}

static int proc_snapshot_rename_noreplace(int dirfd, const char* from,
                                          const char* to) {
#if defined(__linux__)
    return jash_osif_renameat2(dirfd, from, dirfd, to, RENAME_NOREPLACE);
#elif defined(__APPLE__)
    return renameatx_np(dirfd, from, dirfd, to, RENAME_EXCL);
#else
    (void)dirfd; (void)from; (void)to;
    errno = ENOTSUP;
    return -1;
#endif
}

static int proc_snapshot_unlink_identity(int dirfd, const char* name,
                                         const struct stat* expected) {
    struct stat current;
    if (fstatat(dirfd, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
        current.st_dev != expected->st_dev || current.st_ino != expected->st_ino)
        return -1;
    int rc;
    do { rc = unlinkat(dirfd, name, 0); } while (rc != 0 && errno == EINTR);
    return rc;
}

/* 0 = unlink and directory sync durable, 1 = unlinked but sync failed,
 * -1 = the verified inode remains named (or identity could not be proved). */
static int proc_snapshot_unlink_durable(int dirfd, const char* name,
                                        const struct stat* expected) {
    if (proc_snapshot_unlink_identity(dirfd, name, expected) != 0) return -1;
    return fsync(dirfd) == 0 ? 0 : 1;
}

static void proc_snapshot_report_retained(const char* dirpath,
                                          const char* name) {
    fprintf(stderr, "proc snapshot: recovery retained at '%s%s%s'\n",
            dirpath, strcmp(dirpath, "/") ? "/" : "", name);
}

int jash_proc_snapshot_write_atomic(const char* path, const char* data, size_t len) {
    char dirpath[1024], name[256];
    const char* slash = strrchr(path, '/');
    const char* base = slash ? slash + 1 : path;
    if (!*base || strlen(base) >= sizeof name) return -1;
    snprintf(name, sizeof name, "%s", base);
    if (!slash) {
        snprintf(dirpath, sizeof dirpath, ".");
    } else if (slash == path) {
        snprintf(dirpath, sizeof dirpath, "/");
    } else {
        size_t n = (size_t)(slash - path);
        if (n >= sizeof dirpath) return -1;
        memcpy(dirpath, path, n);
        dirpath[n] = '\0';
    }

    int dirfd = open(dirpath, O_RDONLY | O_DIRECTORY);
    if (dirfd < 0) return -1;
    struct stat before;
    int existed = fstatat(dirfd, name, &before, AT_SYMLINK_NOFOLLOW) == 0;
    if (!existed && errno != ENOENT) { close(dirfd); return -1; }
    if (existed && (!S_ISREG(before.st_mode) || before.st_uid != geteuid() ||
                    (before.st_mode & 077) != 0)) {
        close(dirfd);
        return -1;
    }

    char tmpname[96] = {0};
    int tmpfd = -1;
    for (unsigned attempt = 0; attempt < 128; attempt++) {
        snprintf(tmpname, sizeof tmpname, ".jash-snapshot.recover.%ld.%u",
                 (long)getpid(), attempt);
        tmpfd = openat(dirfd, tmpname,
                       O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
        if (tmpfd >= 0 || errno != EEXIST) break;
    }
    if (tmpfd < 0) { close(dirfd); return -1; }

    struct stat tmp_st = {0};
    int ok = fstat(tmpfd, &tmp_st) == 0 && S_ISREG(tmp_st.st_mode) &&
             tmp_st.st_uid == geteuid() && (tmp_st.st_mode & 077) == 0;
    size_t off = 0;
    while (ok && off < len) {
        ssize_t n = write(tmpfd, data + off, len - off);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { ok = 0; break; }
        off += (size_t)n;
    }
    if (ok && fsync(tmpfd) != 0) ok = 0;
    if (close(tmpfd) != 0) ok = 0;
    tmpfd = -1;

    struct stat current, named_tmp;
    struct stat retained_st = tmp_st;
    int retain_tmp = 0;
    if (ok && (fstatat(dirfd, tmpname, &named_tmp, AT_SYMLINK_NOFOLLOW) != 0 ||
               named_tmp.st_dev != tmp_st.st_dev || named_tmp.st_ino != tmp_st.st_ino))
        ok = 0;
    if (ok && existed) {
        if (fstatat(dirfd, name, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
            current.st_dev != before.st_dev || current.st_ino != before.st_ino ||
            !S_ISREG(current.st_mode) || current.st_uid != geteuid() ||
            (current.st_mode & 077) != 0 ||
            proc_snapshot_exchange(dirfd, tmpname, name) != 0) {
            ok = 0;
        } else {
            struct stat exchanged;
            int exchanged_valid =
                fstatat(dirfd, tmpname, &exchanged, AT_SYMLINK_NOFOLLOW) == 0;
            if (exchanged_valid && exchanged.st_dev == before.st_dev &&
                exchanged.st_ino == before.st_ino) {
                /* The exchange is not committed until both the new target and
                 * the old snapshot's recovery name are durable. Never remove
                 * that recovery inode before this sync succeeds. */
                if (fsync(dirfd) != 0) {
                    int rolled_back =
                        proc_snapshot_exchange(dirfd, tmpname, name) == 0;
                    struct stat restored, returned;
                    int rollback_verified = rolled_back &&
                        fstatat(dirfd, name, &restored, AT_SYMLINK_NOFOLLOW) == 0 &&
                        restored.st_dev == before.st_dev &&
                        restored.st_ino == before.st_ino &&
                        fstatat(dirfd, tmpname, &returned, AT_SYMLINK_NOFOLLOW) == 0 &&
                        returned.st_dev == tmp_st.st_dev &&
                        returned.st_ino == tmp_st.st_ino;
                    if (rollback_verified && fsync(dirfd) == 0) {
                        int cleanup_rc = proc_snapshot_unlink_durable(
                            dirfd, tmpname, &tmp_st);
                        if (cleanup_rc >= 0) tmpname[0] = '\0';
                    } else {
                        retained_st = rolled_back ? tmp_st : before;
                        retain_tmp = 1;
                    }
                    ok = 0;
                } else {
                    int cleanup_rc = proc_snapshot_unlink_durable(
                        dirfd, tmpname, &before);
                    if (cleanup_rc < 0) {
                        retained_st = before;
                        retain_tmp = 1;
                        ok = 0;
                    } else if (cleanup_rc > 0) {
                        tmpname[0] = '\0';
                        ok = 0;
                    } else {
                        tmpname[0] = '\0';
                    }
                }
            } else {
                int rolled_back = proc_snapshot_exchange(dirfd, tmpname, name) == 0;
                struct stat restored, returned;
                int rollback_verified = rolled_back && exchanged_valid &&
                    fstatat(dirfd, name, &restored, AT_SYMLINK_NOFOLLOW) == 0 &&
                    restored.st_dev == exchanged.st_dev &&
                    restored.st_ino == exchanged.st_ino &&
                    fstatat(dirfd, tmpname, &returned, AT_SYMLINK_NOFOLLOW) == 0 &&
                    returned.st_dev == tmp_st.st_dev &&
                    returned.st_ino == tmp_st.st_ino;
                if (rollback_verified && fsync(dirfd) == 0) {
                    int cleanup_rc = proc_snapshot_unlink_durable(
                        dirfd, tmpname, &tmp_st);
                    if (cleanup_rc >= 0) tmpname[0] = '\0';
                } else if (exchanged_valid) {
                    retained_st = rolled_back ? tmp_st : exchanged;
                    retain_tmp = 1;
                }
                ok = 0;
            }
        }
    } else if (ok) {
        if (proc_snapshot_rename_noreplace(dirfd, tmpname, name) != 0) {
            ok = 0;
        } else {
            int validated =
                fstatat(dirfd, name, &named_tmp, AT_SYMLINK_NOFOLLOW) == 0 &&
                named_tmp.st_dev == tmp_st.st_dev && named_tmp.st_ino == tmp_st.st_ino;
            if (!validated || fsync(dirfd) != 0) {
                int rolled_back = proc_snapshot_rename_noreplace(
                    dirfd, name, tmpname) == 0;
                if (rolled_back && fsync(dirfd) == 0) {
                    int cleanup_rc = proc_snapshot_unlink_durable(
                        dirfd, tmpname, &tmp_st);
                    if (cleanup_rc >= 0) tmpname[0] = '\0';
                }
                ok = 0;
            } else {
                tmpname[0] = '\0';
            }
        }
    }

    if (tmpfd >= 0) close(tmpfd);
    if (tmpname[0] &&
        fstatat(dirfd, tmpname, &named_tmp, AT_SYMLINK_NOFOLLOW) == 0 &&
        named_tmp.st_dev == retained_st.st_dev &&
        named_tmp.st_ino == retained_st.st_ino) {
        if (!retain_tmp) {
            int cleanup_rc = proc_snapshot_unlink_durable(
                dirfd, tmpname, &retained_st);
            if (cleanup_rc >= 0) tmpname[0] = '\0';
        }
        if (tmpname[0]) proc_snapshot_report_retained(dirpath, tmpname);
    }
    close(dirfd);
    return ok ? 0 : -1;
}

typedef struct {
    const char* p;
    const char* end;
} proc_json_cursor_t;

static void proc_json_ws(proc_json_cursor_t* c) {
    while (c->p < c->end && (*c->p == ' ' || *c->p == '\t' ||
           *c->p == '\r' || *c->p == '\n')) c->p++;
}

static int proc_json_take(proc_json_cursor_t* c, char wanted) {
    proc_json_ws(c);
    if (c->p >= c->end || *c->p != wanted) return -1;
    c->p++;
    return 0;
}

static int proc_json_string(proc_json_cursor_t* c, char* out, size_t cap) {
    if (proc_json_take(c, '"') != 0) return -1;
    size_t used = 0;
    while (c->p < c->end && *c->p != '"') {
        unsigned value = (unsigned char)*c->p++;
        if (value == '\\') {
            if (c->p >= c->end) return -1;
            unsigned esc = (unsigned char)*c->p++;
            if (esc == '"' || esc == '\\' || esc == '/') value = esc;
            else if (esc == 'b') value = '\b';
            else if (esc == 'f') value = '\f';
            else if (esc == 'n') value = '\n';
            else if (esc == 'r') value = '\r';
            else if (esc == 't') value = '\t';
            else if (esc == 'u') {
                if ((size_t)(c->end - c->p) < 4) return -1;
                value = 0;
                for (int i = 0; i < 4; i++) {
                    unsigned ch = (unsigned char)c->p[i];
                    unsigned h = ch >= '0' && ch <= '9' ? ch - '0' :
                                 ch >= 'a' && ch <= 'f' ? ch - 'a' + 10 :
                                 ch >= 'A' && ch <= 'F' ? ch - 'A' + 10 : 16;
                    if (h > 15) return -1;
                    value = (value << 4) | h;
                }
                c->p += 4;
                if (out && (value == 0 || value > 255)) return -1;
            } else return -1;
        } else if (value < 0x20) return -1;
        if (out) {
            if (used + 1 >= cap) return -1;
            out[used++] = (char)value;
        }
    }
    if (c->p >= c->end || *c->p++ != '"') return -1;
    if (out) out[used] = '\0';
    return 0;
}

static int proc_json_uint(proc_json_cursor_t* c, int* out) {
    proc_json_ws(c);
    if (c->p >= c->end || *c->p < '0' || *c->p > '9') return -1;
    unsigned value = 0;
    do {
        unsigned digit = (unsigned)(*c->p++ - '0');
        if (value > (2147483647u - digit) / 10u) return -1;
        value = value * 10u + digit;
    } while (c->p < c->end && *c->p >= '0' && *c->p <= '9');
    if (out) *out = (int)value;
    return 0;
}

static int proc_json_skip_value(proc_json_cursor_t* c, unsigned depth);

static int proc_json_skip_value(proc_json_cursor_t* c, unsigned depth) {
    if (depth > 32) return -1;
    proc_json_ws(c);
    if (c->p >= c->end) return -1;
    if (*c->p == '"') return proc_json_string(c, NULL, 0);
    if (*c->p == '{') {
        c->p++;
        proc_json_ws(c);
        if (c->p < c->end && *c->p == '}') { c->p++; return 0; }
        for (;;) {
            if (proc_json_string(c, NULL, 0) != 0 || proc_json_take(c, ':') != 0 ||
                proc_json_skip_value(c, depth + 1) != 0) return -1;
            proc_json_ws(c);
            if (c->p < c->end && *c->p == ',') { c->p++; continue; }
            if (c->p < c->end && *c->p == '}') { c->p++; return 0; }
            return -1;
        }
    }
    if (*c->p == '[') {
        c->p++;
        proc_json_ws(c);
        if (c->p < c->end && *c->p == ']') { c->p++; return 0; }
        for (;;) {
            if (proc_json_skip_value(c, depth + 1) != 0) return -1;
            proc_json_ws(c);
            if (c->p < c->end && *c->p == ',') { c->p++; continue; }
            if (c->p < c->end && *c->p == ']') { c->p++; return 0; }
            return -1;
        }
    }
    if (*c->p >= '0' && *c->p <= '9') return proc_json_uint(c, NULL);
    static const char* literals[] = {"true", "false", "null"};
    for (size_t i = 0; i < sizeof literals / sizeof literals[0]; i++) {
        size_t n = strlen(literals[i]);
        if ((size_t)(c->end - c->p) >= n && memcmp(c->p, literals[i], n) == 0) {
            c->p += n;
            return 0;
        }
    }
    return -1;
}

static int proc_json_key(proc_json_cursor_t* c, const char* wanted) {
    char key[32];
    return proc_json_string(c, key, sizeof key) == 0 && !strcmp(key, wanted) &&
           proc_json_take(c, ':') == 0 ? 0 : -1;
}

static int proc_snapshot_version(const char* json, size_t len) {
    proc_json_cursor_t c = {json, json + len};
    int version = -1;
    if (proc_json_take(&c, '{') != 0 || proc_json_key(&c, "version") != 0 ||
        proc_json_uint(&c, &version) != 0) return -1;
    return version;
}

static int proc_snapshot_parse_v2(const char* json, size_t len, int* root_pid,
                                  char* storage, size_t storage_cap,
                                  char** argv, size_t argv_cap) {
    proc_json_cursor_t c = {json, json + len};
    char scratch[64];
    int version, tree_pid;
    if (proc_json_take(&c, '{') != 0 || proc_json_key(&c, "version") != 0 ||
        proc_json_uint(&c, &version) != 0 || version != 2 ||
        proc_json_take(&c, ',') != 0 || proc_json_key(&c, "timestamp") != 0 ||
        proc_json_string(&c, scratch, sizeof scratch) != 0 ||
        proc_json_take(&c, ',') != 0 || proc_json_key(&c, "root_pid") != 0 ||
        proc_json_uint(&c, root_pid) != 0 ||
        proc_json_take(&c, ',') != 0 || proc_json_key(&c, "tree") != 0 ||
        proc_json_take(&c, '{') != 0 || proc_json_key(&c, "pid") != 0 ||
        proc_json_uint(&c, &tree_pid) != 0 ||
        proc_json_take(&c, ',') != 0 || proc_json_key(&c, "argv") != 0 ||
        proc_json_take(&c, '[') != 0) return -1;

    size_t used = 0;
    int argc = 0;
    proc_json_ws(&c);
    if (c.p < c.end && *c.p != ']') {
        for (;;) {
            if (argc >= (int)argv_cap - 1 || used >= storage_cap) return -1;
            argv[argc] = storage + used;
            if (proc_json_string(&c, storage + used, storage_cap - used) != 0)
                return -1;
            used += strlen(storage + used) + 1;
            argc++;
            proc_json_ws(&c);
            if (c.p < c.end && *c.p == ',') { c.p++; continue; }
            break;
        }
    }
    if (proc_json_take(&c, ']') != 0) return -1;
    argv[argc] = NULL;

    int seen_fds = 0, seen_env = 0, seen_children = 0;
    for (;;) {
        proc_json_ws(&c);
        if (c.p < c.end && *c.p == '}') { c.p++; break; }
        if (proc_json_take(&c, ',') != 0 ||
            proc_json_string(&c, scratch, sizeof scratch) != 0 ||
            proc_json_take(&c, ':') != 0) return -1;
        proc_json_ws(&c);
        if (c.p >= c.end || *c.p != '[') return -1;
        if (!strcmp(scratch, "fds") && !seen_fds && !seen_env && !seen_children)
            seen_fds = 1;
        else if (!strcmp(scratch, "env") && !seen_env && !seen_children)
            seen_env = 1;
        else if (!strcmp(scratch, "children") && !seen_children)
            seen_children = 1;
        else return -1;
        if (proc_json_skip_value(&c, 0) != 0) return -1;
    }
    if (!seen_children || *root_pid != tree_pid ||
        proc_json_take(&c, '}') != 0) return -1;
    proc_json_ws(&c);
    return c.p == c.end ? argc : -1;
}

/* Public: capture PID tree + FD table + env for target PID */
int jash_timeline_proc_snapshot(int target_pid, const char* out_path, size_t cap) {
    (void)cap;
    if (target_pid <= 0) target_pid = getpid();
    char json[32768];
    if (proc_serialize_tree(target_pid, json, sizeof json) != 0) return -1;

    /* Add metadata */
    char full[36864];
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", tm);

    size_t full_n = 0;
    if (proc_appendf(full, sizeof full, &full_n,
                     "{\"version\":2,\"timestamp\":\"%s\",\"root_pid\":%d,\"tree\":%s}",
                     ts, target_pid, json) != 0 ||
        jash_proc_snapshot_write_atomic(out_path, full, full_n) != 0)
        return -1;

    /* Record in timeline */
    char desc[256];
    snprintf(desc, sizeof desc, "proc snapshot pid=%d", target_pid);
    jash_timeline_record(JASH_UNDO_PROC_SNAPSHOT, desc,
                         target_pid > 0 ? "" : "", out_path);
    return 0;
}

/* Public: restore process tree from snapshot */
int jash_timeline_proc_restore(const char* snapshot_path, int* out_pid) {
    /* Arbitrary --output paths remain restorable, but only from the private,
     * same-uid inode that was observed before open. */
    struct stat st;
    int fd = proc_snapshot_open(snapshot_path, &st);
    if (fd < 0) return -1;
    if (st.st_size < 0 || st.st_size > 1024*1024) { close(fd); return -1; }
    size_t json_len = (size_t)st.st_size;
    char* json = malloc(json_len + 1);
    if (!json) { close(fd); return -1; }
    size_t got = 0;
    while (got < json_len) {
        ssize_t n = read(fd, json + got, json_len - got);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        got += (size_t)n;
    }
    close(fd);
    if (got != json_len) { free(json); return -1; }
    json[got] = '\0';

    int version = proc_snapshot_version(json, json_len);
    int orig_pid = -1;
    char arg_storage[8192];
    char* argv[256];
    int argc = -1;
    if (version == 2) {
        argc = proc_snapshot_parse_v2(json, json_len, &orig_pid,
                                      arg_storage, sizeof arg_storage,
                                      argv, sizeof argv / sizeof argv[0]);
    } else if (version == 1) {
        /* Version-1 compatibility: preserve the historic single-string
         * behavior without guessing how spaces were originally split. */
        char* rp = strstr(json, "\"root_pid\":");
        char* cl = strstr(json, "\"cmdline\":\"");
        if (!rp || !cl) { free(json); return -1; }
        orig_pid = atoi(rp + 11);
        cl += 11;
        size_t ci = 0;
        int closed = 0;
        for (char* p = cl; *p && ci < sizeof arg_storage - 1; p++) {
            if (*p == '\\' && *(p + 1) == '"') { arg_storage[ci++] = '"'; p++; }
            else if (*p == '\\' && *(p + 1) == '\\') { arg_storage[ci++] = '\\'; p++; }
            else if (*p == '"') { closed = 1; break; }
            else { arg_storage[ci++] = *p; }
        }
        while (ci > 0 && arg_storage[ci - 1] == ' ') ci--;
        arg_storage[ci] = '\0';
        if (!closed || ci == 0) { free(json); return -1; }
        argv[0] = arg_storage;
        argv[1] = NULL;
        argc = 1;
    }
    free(json);

    if (argc <= 0) return -1;

    /* Guard against infinite loop: don't re-exec snapshot/restore commands */
    if (argc >= 2 && !strcmp(argv[0], "proc") &&
        (!strcmp(argv[1], "snapshot") || !strcmp(argv[1], "restore"))) {
        return -1;
    }

    /* Fork and exec */
    pid_t pid = fork();
    if (pid < 0) return -1;
    if (pid == 0) {
        /* Child: exec the command */
        execvp(argv[0], argv);
        _exit(127);
    }

    if (out_pid) *out_pid = pid;
    /* Record in timeline */
    char desc[256];
    snprintf(desc, sizeof desc, "proc restore from %s", snapshot_path);
    jash_timeline_record(JASH_UNDO_PROC_RESTORE, desc, snapshot_path,
                         orig_pid > 0 ? "" : "");
    return 0;
}

/* Public: serialize current PID tree as JSON */
int jash_timeline_proc_tree(int root_pid, char* out_json, size_t cap) {
    if (root_pid <= 0) root_pid = getpid();
    char json[32768];
    if (proc_serialize_tree(root_pid, json, sizeof json) != 0) return -1;
    time_t now = time(NULL);
    struct tm* tm = localtime(&now);
    char ts[32];
    strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", tm);
    size_t used = 0;
    return proc_appendf(out_json, cap, &used,
                        "{\"version\":2,\"timestamp\":\"%s\",\"root_pid\":%d,\"tree\":%s}",
                        ts, root_pid, json);
}

/* Public: diff two snapshots, output JSON (simplified) */
int jash_timeline_proc_diff(const char* snap_a, const char* snap_b, char* out_json, size_t cap) {
    /* Read both files */
    int fd_a = open(snap_a, O_RDONLY);
    int fd_b = open(snap_b, O_RDONLY);
    if (fd_a < 0 || fd_b < 0) { if (fd_a >= 0) close(fd_a); if (fd_b >= 0) close(fd_b); return -1; }
    struct stat st_a, st_b;
    fstat(fd_a, &st_a); fstat(fd_b, &st_b);
    if (st_a.st_size > 1024*1024 || st_b.st_size > 1024*1024) { close(fd_a); close(fd_b); return -1; }
    char* json_a = malloc(st_a.st_size + 1);
    char* json_b = malloc(st_b.st_size + 1);
    if (!json_a || !json_b) { free(json_a); free(json_b); close(fd_a); close(fd_b); return -1; }
    if (read(fd_a, json_a, st_a.st_size) < 0) { /* best-effort */ }
    json_a[st_a.st_size] = '\0';
    if (read(fd_b, json_b, st_b.st_size) < 0) { /* best-effort */ }
    json_b[st_b.st_size] = '\0';
    close(fd_a); close(fd_b);

    /* Simplified diff: just compare sizes and root_pid */
    snprintf(out_json, cap,
        "{\"snap_a\":\"%s\",\"snap_b\":\"%s\",\"size_a\":%ld,\"size_b\":%ld,\"identical\":%s}",
        snap_a, snap_b, (long)st_a.st_size, (long)st_b.st_size,
        (st_a.st_size == st_b.st_size && strcmp(json_a, json_b) == 0) ? "true" : "false");
    free(json_a);
    free(json_b);
    return 0;
}

/* ── Session teardown ────────────────────────────────────────────────────── */

void jash_timeline_shutdown(void) {
    if (g_backup_dir_created == 1 && g_backup_dir[0] && jash_sigil_get() < SIGIL_LOG)
        osif_rmtree(g_backup_dir);
    g_backup_dir_created = 0;
    free(g_tl);
    free(g_redo);
    g_tl = NULL;
    g_redo = NULL;
    g_tl_count = 0;
    g_redo_count = 0;
}
