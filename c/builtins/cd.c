// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/cd — change working directory with smart features.
//
// Enhanced features:
//   1. Advanced fuzzy matching (subsequence + abbreviation scoring)
//   2. Directory preview (shows file/dir count before changing)
//   3. Smart suggestions on error ("Did you mean: X?")
//   4. Automatic glob expansion (cd /tmp/test* → expands to match)

#include "builtin.h"
#include <core/timeline.h>
#include <stdio.h>
#include <core/env.h>
#include <core/frecency.h>
#include <sys/syscalls.h>
#include <theme/colors.h>
#include <dirent.h>
#include <sys/stat.h>
#include <string.h>
#include <errno.h>
#include <glob.h>

/* ── Helpers ────────────────────────────────────────────────────────────── */

/* Copy path into dst, bounded. */
static void copy_path(char* dst, size_t cap, const char* src) {
    size_t i = 0;
    while (src[i] && i + 1 < cap) { dst[i] = src[i]; i++; }
    dst[i] = '\0';
}

static size_t path_len(const char* p) {
    size_t n = 0;
    while (p[n]) n++;
    return n;
}

/* ── Filesystem helpers ─────────────────────────────────────────────────── */

static char g_oldpwd[256] = "";

/* ── Glob expansion ─────────────────────────────── */

/* Check if path contains glob characters */
static int has_glob_chars(const char* p) {
    while (*p) {
        if (*p == '*' || *p == '?' || *p == '[') return 1;
        p++;
    }
    return 0;
}

/* GLOB_ONLYDIR is a GNU extension (a performance hint, not a filter —
 * even glibc may return non-dirs). Portably degrade to 0 where absent;
 * correctness is unaffected since callers stat the chosen match anyway. */
#ifndef GLOB_ONLYDIR
#define GLOB_ONLYDIR 0
#endif

/* Try to expand a glob pattern. If exactly one directory match, return it
 * in out (bounded by cap). Returns 1 on success, 0 on failure. */
static int cd_glob_expand(const char* pattern, char* out, size_t cap) {
    glob_t gl;
    int rc = glob(pattern, GLOB_NOSORT | GLOB_ONLYDIR, NULL, &gl);
    if (rc != 0 || gl.gl_pathc == 0) {
        globfree(&gl);
        return 0;
    }

    /* If exactly one match, use it */
    if (gl.gl_pathc == 1) {
        copy_path(out, cap, gl.gl_pathv[0]);
        globfree(&gl);
        return 1;
    }

    /* Multiple matches: pick the first one and warn (via return value 2) */
    copy_path(out, cap, gl.gl_pathv[0]);
    globfree(&gl);
    return 2;  /* signal: multiple matches, used first */
}

/* ── Directory preview ──────────────────────────────────────────────────── */

/* Count files and directories in a path. Sets *nfiles and *ndirs. */
static void cd_preview_dir(const char* path, int* nfiles, int* ndirs) {
    *nfiles = 0;
    *ndirs = 0;

    DIR* d = opendir(path);
    if (!d) return;

    struct dirent* ent;
    while ((ent = readdir(d)) != NULL) {
        /* Skip . and .. */
        if (ent->d_name[0] == '.') {
            if (ent->d_name[1] == '\0') continue;
            if (ent->d_name[1] == '.' && ent->d_name[2] == '\0') continue;
        }

#if defined(DT_DIR)
        if (ent->d_type == DT_DIR) {
            (*ndirs)++;
        } else if (ent->d_type != DT_UNKNOWN) {
            (*nfiles)++;
        } else {
            /* Fallback to stat for DT_UNKNOWN */
#endif
            char fullpath[1024];
            size_t plen = path_len(path);
            copy_path(fullpath, sizeof(fullpath), path);
            if (plen > 0 && fullpath[plen - 1] != '/') {
                fullpath[plen] = '/';
                fullpath[plen + 1] = '\0';
            }
            strlcat(fullpath, ent->d_name, sizeof(fullpath));

            struct stat st;
            if (stat(fullpath, &st) == 0) {
                if (S_ISDIR(st.st_mode)) (*ndirs)++;
                else (*nfiles)++;
            }
#if defined(DT_DIR)
        }
#endif
    }
    closedir(d);
}

/* Print preview line: "  → src/ (15 files, 3 dirs)" */
static void cd_print_preview(jash_writer_t w, void* ctx, const char* path) {
    int nfiles = 0, ndirs = 0;
    cd_preview_dir(path, &nfiles, &ndirs);

    jash_writes(w, ctx, J_ROLE_MUTED "  → ");
    jash_writes(w, ctx, J_ROLE_PATH);

    /* Print basename of target */
    const char* base = strrchr(path, '/');
    base = base ? base + 1 : path;
    if (!*base && path[0] == '/') base = "/";
    jash_writes(w, ctx, base);
    jash_writes(w, ctx, "/");
    jash_writes(w, ctx, J_RESET);

    jash_writes(w, ctx, J_ROLE_MUTED " (");

    /* Format counts */
    char buf[64];
    int pos = 0;

    /* files count */
    int tmp = nfiles;
    if (tmp == 0) { buf[pos++] = '0'; }
    else {
        char rev[16]; int ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
        for (int k = ri - 1; k >= 0; k--) buf[pos++] = rev[k];
    }
    buf[pos++] = ' ';
    if (nfiles == 1) {
        buf[pos++] = 'f'; buf[pos++] = 'i'; buf[pos++] = 'l'; buf[pos++] = 'e';
    } else {
        buf[pos++] = 'f'; buf[pos++] = 'i'; buf[pos++] = 'l'; buf[pos++] = 'e'; buf[pos++] = 's';
    }

    buf[pos++] = ','; buf[pos++] = ' ';

    /* dirs count */
    tmp = ndirs;
    if (tmp == 0) { buf[pos++] = '0'; }
    else {
        char rev[16]; int ri = 0;
        while (tmp > 0) { rev[ri++] = '0' + (tmp % 10); tmp /= 10; }
        for (int k = ri - 1; k >= 0; k--) buf[pos++] = rev[k];
    }
    buf[pos++] = ' ';
    if (ndirs == 1) {
        buf[pos++] = 'd'; buf[pos++] = 'i'; buf[pos++] = 'r';
    } else {
        buf[pos++] = 'd'; buf[pos++] = 'i'; buf[pos++] = 'r'; buf[pos++] = 's';
    }

    buf[pos++] = ')';
    buf[pos] = '\0';

    jash_writes(w, ctx, buf);
    jash_writes(w, ctx, J_RESET "\n");
}

/* ── Smart suggestions on error ─────────────────────────────────────────── */

/* Simple tolower for ASCII */
static int cd_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c + 32;
    return c;
}

/* Edit distance (case-insensitive) */
static int cd_edit_dist(const char* a, const char* b) {
    int la = (int)path_len(a), lb = (int)path_len(b);
    if (la > 63 || lb > 63) return 999;
    int d[64][64];
    for (int i = 0; i <= la; i++) d[i][0] = i;
    for (int j = 0; j <= lb; j++) d[0][j] = j;
    for (int i = 1; i <= la; i++) {
        for (int j = 1; j <= lb; j++) {
            int cost = (cd_tolower(a[i-1]) == cd_tolower(b[j-1])) ? 0 : 1;
            d[i][j] = d[i-1][j] + 1;
            if (d[i][j-1] + 1 < d[i][j]) d[i][j] = d[i][j-1] + 1;
            if (d[i-1][j-1] + cost < d[i][j]) d[i][j] = d[i-1][j-1] + cost;
        }
    }
    return d[la][lb];
}

/* Try fuzzy match against frecency (histórico de navegação). */
static const char* cd_fuzzy_frecency(frecency_table_t* ft, const char* query) {
    if (!ft || !query || !*query) return NULL;
    const char* best = NULL;
    int best_score = 0;
    for (int i = 0; i < ft->count; i++) {
        const char* entry = ft->entries[i].path;
        if (!entry || !*entry) continue;
        const char* base = strrchr(entry, '/');
        base = base ? base + 1 : entry;
        int s = 0;
        for (const char* q = query; *q && *base; q++, base++) {
            if (cd_tolower(*q) == cd_tolower(*base)) s++;
        }
        s += (int)(ft->entries[i].visits / 2);
        if (s > best_score) { best_score = s; best = entry; }
    }
    return (best_score >= 3) ? best : NULL;
}

/* Suggest similar directory names when cd fails. */
static void cd_suggest_similar(jash_writer_t w, void* ctx,
                                const char* failed_path,
                                frecency_table_t* ft) {
    char dir[512] = ".";
    char base[256] = "";
    const char* slash = strrchr(failed_path, '/');
    if (slash) {
        int dlen = (int)(slash - failed_path);
        if (dlen > 0 && dlen < 511) {
            for (int i = 0; i < dlen; i++) dir[i] = failed_path[i];
            dir[dlen] = '\0';
        }
        copy_path(base, sizeof(base), slash + 1);
    } else {
        copy_path(base, sizeof(base), failed_path);
    }

    if (!base[0]) return;

    /* Open the directory to find similar names */
    DIR* d = opendir(dir);
    if (d) {
        char best[256] = "";
        int best_dist = 4;
        struct dirent* ent;
        while ((ent = readdir(d)) != NULL) {
            if (ent->d_name[0] == '.') continue;
            int is_dir = 0;
#if defined(DT_DIR)
            if (ent->d_type == DT_DIR) {
                is_dir = 1;
            } else if (ent->d_type == DT_UNKNOWN) {
#endif
                char fullpath[1024];
                size_t plen = path_len(dir);
                copy_path(fullpath, sizeof(fullpath), dir);
                if (plen > 0 && fullpath[plen-1] != '/') {
                    fullpath[plen] = '/';
                    fullpath[plen+1] = '\0';
                }
                strlcat(fullpath, ent->d_name, sizeof(fullpath));
                struct stat st;
                if (stat(fullpath, &st) == 0 && S_ISDIR(st.st_mode))
                    is_dir = 1;
#if defined(DT_DIR)
            }
#endif
            if (!is_dir) continue;
            int dist = cd_edit_dist(base, ent->d_name);
            if (dist < best_dist && dist <= 3) {
                best_dist = dist;
                copy_path(best, sizeof(best), ent->d_name);
            }
        }
        closedir(d);
        if (best[0]) {
            jash_writes(jash_stderr_writer, 0, J_ROLE_WARN "  Did you mean: ");
            jash_writes(jash_stderr_writer, 0, J_ROLE_PATH);
            if (dir[0] != '.' || dir[1] != '\0') {
                jash_writes(jash_stderr_writer, 0, dir);
                jash_writes(jash_stderr_writer, 0, "/");
            }
            jash_writes(jash_stderr_writer, 0, best);
            jash_writes(jash_stderr_writer, 0, J_RESET "\n");
            return;
        }
    }

    /* Try frecency match as fallback */
    if (ft) {
        const char* fz = cd_fuzzy_frecency(ft, base);
        if (fz) {
            jash_writes(jash_stderr_writer, 0, J_ROLE_WARN "  Did you mean: ");
            jash_writes(jash_stderr_writer, 0, J_ROLE_PATH);
            jash_writes(jash_stderr_writer, 0, fz);
            jash_writes(jash_stderr_writer, 0, J_RESET "\n");
        }
    }
}

/* ── Main cd builtin ────────────────────────────────────────────────────── */

int jash_b_cd(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    const char* target = args;
    if (target) { while (*target == ' ' || *target == '\t') target++; }

    /* POSIX `--` end-of-options: `cd -- -foo` lets `-foo` be a literal name.
     * After consuming `--`, what follows is the operand (or empty → HOME). */
    int seen_dashdash = 0;
    if (target && target[0] == '-' && target[1] == '-' &&
        (target[2] == '\0' || target[2] == ' ' || target[2] == '\t')) {
        seen_dashdash = 1;
        target += 2;
        while (*target == ' ' || *target == '\t') target++;
    }

    /* cd - : volta para o diretório anterior (suppressed after --) */
    if (!seen_dashdash && target && target[0] == '-' && target[1] == 0) {
        if (g_oldpwd[0] == 0) {
            jash_writes(jash_stderr_writer, 0, J_ROLE_ERR "cd: OLDPWD not set\n" J_RESET);
            return 1;
        }
        target = g_oldpwd;
    } else if (!target || !*target) {
        /* No args: POSIX says `cd` goes to $HOME. (Earlier jash sent the
         * user to `frecency_best()` instead — when /private/tmp had high
         * frecency rank, a bare `cd` teleported there. Confusing and
         * non-POSIX. Frecency-as-default lives behind explicit invocation
         * — `cd <partial>` still uses frecency when the partial matches.) */
        target = jash_env_get(s, "HOME");
        if (!target || !*target) target = "/";
    }

    /* Glob expansion: if target contains *, ?, or [ — expand it */
    char glob_buf[1024];
    if (has_glob_chars(target)) {
        int grc = cd_glob_expand(target, glob_buf, sizeof(glob_buf));
        if (grc == 1) {
            target = glob_buf;
        } else if (grc == 2) {
            /* Multiple matches — use first, but warn */
            jash_writef(jash_stderr_writer, 0, "%scd: glob '%s' matched multiple dirs, using: %s%s\n",
                        J_ROLE_WARN, target, glob_buf, J_RESET);
            target = glob_buf;
        } else {
            /* No matches — fall through to normal error handling */
        }
    }

    /* Save current cwd before changing */
    char old_cwd[256];
    copy_path(old_cwd, sizeof(old_cwd), s->cwd);

    /* Preview: show directory contents before changing.
     * INTERACTIVE ONLY (bash parity): a script doing `cd x && pwd` must
     * not get decoration mixed into its stdout. */
    if (s->interactive)
        cd_print_preview(w, ctx, target);

    long rc = sys_chdir(target);
    if (rc < 0) {
        /* Bash-style diagnostic: name the reason. errno was set by chdir.
         * Avoid the raw "error -1" line — wrappers parse this to classify
         * missing vs. permission errors. */
        const char* reason = strerror(errno ? errno : ENOENT);
        jash_writef(jash_stderr_writer, 0, "%scd: %s: %s%s\n",
                    J_ROLE_ERR, target, reason, J_RESET);

        /* "Did you mean: ..." suggestion is interactive UX. In a captured
         * non-tty stderr (CI, agent transcripts) it just adds a second
         * confusing line, sometimes with bogus content like "//usr".
         * Keep the help for humans, skip it for tooling. */
        if (s->interactive)
            cd_suggest_similar(w, ctx, target, &s->frecency);
        return 1;
    }

    /* Update OLDPWD before changing PWD.
     * ALIASING: for `cd -`, target POINTS INTO g_oldpwd — snapshot it
     * before overwriting, or the logical-path build below reads the
     * freshly clobbered value. */
    char target_snap[256];
    copy_path(target_snap, sizeof(target_snap), target);
    copy_path(g_oldpwd, sizeof(g_oldpwd), old_cwd);
    jash_env_set(s, "OLDPWD", old_cwd);
    {
        char desc[300];
        snprintf(desc, sizeof(desc), "cd %s", target_snap);
        jash_timeline_record(JASH_UNDO_CHDIR, desc, old_cwd, NULL);
    }

    /* Bash parity: PWD is LOGICAL by default (set -o physical off).
     * `cd /tmp && pwd` must print /tmp even when /tmp is a symlink
     * (macOS: getcwd would say /private/tmp). Build the logical path
     * textually from the target; fall back to getcwd only when the
     * textual form cannot be produced. */
    {
        char logical[sizeof(s->cwd)];
        size_t ll = 0;
        if (target_snap[0] == '/') {
            copy_path(logical, sizeof(logical), target_snap);
        } else {
            copy_path(logical, sizeof(logical), old_cwd);
            ll = strlen(logical);
            if (ll > 0 && logical[ll-1] != '/' && ll + 1 < sizeof(logical))
                logical[ll++] = '/';
            size_t ti = 0;
            while (target_snap[ti] && ll + 1 < sizeof(logical))
                logical[ll++] = target_snap[ti++];
            logical[ll] = '\0';
        }
        /* Normalize: collapse "//", resolve "." and ".." components. */
        char norm[sizeof(s->cwd)];
        size_t no = 0;
        const char* lp = logical;
        norm[no++] = '/';
        while (*lp) {
            while (*lp == '/') lp++;
            if (!*lp) break;
            const char* seg = lp;
            while (*lp && *lp != '/') lp++;
            size_t seglen = (size_t)(lp - seg);
            if (seglen == 1 && seg[0] == '.') continue;
            if (seglen == 2 && seg[0] == '.' && seg[1] == '.') {
                /* pop last component */
                while (no > 1 && norm[no-1] == '/') no--;
                while (no > 1 && norm[no-1] != '/') no--;
                continue;
            }
            if (no > 1 && no + 1 < sizeof(norm)) norm[no++] = '/';
            else if (no == 1) { /* root: segment follows the leading '/' */ }
            for (size_t k = 0; k < seglen && no + 1 < sizeof(norm); k++)
                norm[no++] = seg[k];
        }
        norm[no] = '\0';
        /* Strip trailing '/' left by a ".." pop (keep root "/"). */
        while (no > 1 && norm[no-1] == '/') norm[--no] = '\0';
        if (no > 0) {
            copy_path(s->cwd, sizeof(s->cwd), norm);
        } else {
            /* Pull the real cwd back from the kernel as fallback. */
            long n = sys_getcwd(s->cwd, sizeof(s->cwd));
            (void)n;
        }
    }
    jash_env_set(s, "PWD", s->cwd);

    /* Record frecency after successful change */
    frecency_record(&s->frecency, s->cwd);
    frecency_save(&s->frecency);

    /* Print new cwd for cd - */
    if (target == g_oldpwd) {
        /* cd - was called, show the path */
        jash_writes(w, ctx, s->cwd);
        jash_writes(w, ctx, "\n");
    }
    return 0;
}
