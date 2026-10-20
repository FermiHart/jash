// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/sed — stream editor, in-process (ADR-0044).
//
// The gold angle (census #1 for sed): `sed -i` is a DESTRUCTIVE in-place edit
// with zero undo in a normal shell. In-process, before overwriting a file jash
// snapshots it into the undo timeline — so `undo` / `tread` recover the original.
// Output is budget-governed (single-command dispatch) and there is no fork/exec.
//
// SURFACE SAFETY: sed is a whole language. This builtin handles the ONE command
// that dominates agent usage — a single `s/regex/replacement/[flags]`
// substitution (any delimiter; flags g, p, i/I, and an Nth-occurrence number;
// replacement supports &, \1-\9, \n, \t, \\) with -n, -e, -E/-r, -i. ANYTHING
// else (multiple commands, address ranges, d/p/y/a/i/c, …) FALLS BACK to the
// external `sed` with the original arguments, so no script is ever broken.

#include "builtin.h"
#include <parser/exec.h>
#include <core/timeline.h>
#include <core/mutation_report.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <osif/osinfo.h>
#include <regex.h>
#include <errno.h>

#ifndef O_DIRECTORY
#define O_DIRECTORY 0
#endif

#define SED_MAX_LOGICAL_LINE (16U * 1024U * 1024U)
#define SED_OUTPUT_SLACK (128U * 1024U)

#if defined(__linux__)
#ifndef RENAME_EXCHANGE
#define RENAME_EXCHANGE (1U << 1)
#endif
#endif

static int sed_exchange(int dirfd, const char* left, const char* right) {
#if defined(__linux__)
    return jash_osif_renameat2(dirfd, left, dirfd, right, RENAME_EXCHANGE);
#elif defined(__APPLE__)
    return renameatx_np(dirfd, left, dirfd, right, RENAME_SWAP);
#else
    (void)dirfd; (void)left; (void)right;
    errno = ENOTSUP;
    return -1;
#endif
}

static int sed_same_inode(const struct stat* left, const struct stat* right) {
    return left->st_dev == right->st_dev && left->st_ino == right->st_ino;
}

static unsigned long sed_temp_serial;

static int sed_open_parent(const char* path, char* name, size_t name_cap) {
    char parent[1100];
    if (strlen(path) >= sizeof(parent)) { errno = ENAMETOOLONG; return -1; }
    strlcpy(parent, path, sizeof(parent));
    char* slash = strrchr(parent, '/');
    const char* base = path;
    if (!slash) {
        strlcpy(parent, ".", sizeof(parent));
    } else {
        base = strrchr(path, '/') + 1;
        if (slash == parent) slash[1] = '\0';
        else *slash = '\0';
    }
    if (!*base || strlen(base) >= name_cap) { errno = ENAMETOOLONG; return -1; }
    strlcpy(name, base, name_cap);
    return open(parent, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
}

/* Exchange rollback is safe only while both names still identify the inodes
 * participating in the failed publication. A raced-in inode is retained. */
static int sed_rollback_exchange(int dirfd, const char* tmp, const char* target,
                                  const struct stat* temp_owned,
                                  const struct stat* displaced_owned) {
    struct stat tmp_now, target_now;
    if (fstatat(dirfd, tmp, &tmp_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        fstatat(dirfd, target, &target_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !sed_same_inode(&tmp_now, displaced_owned) ||
        !sed_same_inode(&target_now, temp_owned)) {
        errno = ESTALE;
        return -1;
    }
    if (sed_exchange(dirfd, tmp, target) != 0) return -1;
    if (fstatat(dirfd, tmp, &tmp_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        fstatat(dirfd, target, &target_now, AT_SYMLINK_NOFOLLOW) != 0 ||
        !sed_same_inode(&tmp_now, temp_owned) ||
        !sed_same_inode(&target_now, displaced_owned)) {
        errno = ESTALE;
        return -1;
    }
    return unlinkat(dirfd, tmp, 0);
}

typedef struct {
    regex_t re;
    int     have_re;
    char    repl[2048];
    int     global;   /* g */
    int     nth;      /* start occurrence (1 = first) */
    int     print;    /* p flag */
} sed_sub_t;

enum {
    SED_PARSE_UNSUPPORTED = -1,
    SED_PARSE_OVERFLOW = -2
};

/* Parse `s<d>PAT<d>REPL<d>FLAGS`. Returns 0 on success, -1 if not a simple s///
 * we handle (caller falls back to external sed). `extended` selects ERE. */
static int sed_parse_sub(const char* script, int extended, sed_sub_t* sub) {
    if (script[0] != 's' || !script[1]) return -1;
    char d = script[1];
    if (d == '\\' || d == '\n') return -1;
    const char* p = script + 2;

    /* pattern up to an unescaped delimiter */
    char pat[2048]; size_t pi = 0;
    while (*p && *p != d) {
        if (*p == '\\' && p[1] == d) {
            if (pi + 1 >= sizeof pat) return SED_PARSE_OVERFLOW;
            pat[pi++] = d;
            p += 2;
            continue;
        }
        if (*p == '\\' && p[1]) {
            if (pi + 2 >= sizeof pat) return SED_PARSE_OVERFLOW;
            pat[pi++] = '\\';
            pat[pi++] = p[1];
            p += 2;
            continue;
        }
        if (pi + 1 >= sizeof pat) return SED_PARSE_OVERFLOW;
        pat[pi++] = *p;
        p++;
    }
    if (*p != d) return -1;
    pat[pi] = '\0';
    p++;

    /* replacement up to an unescaped delimiter */
    size_t ri = 0;
    while (*p && *p != d) {
        if (*p == '\\' && p[1] == d) {
            if (ri + 1 >= sizeof sub->repl) return SED_PARSE_OVERFLOW;
            sub->repl[ri++] = d;
            p += 2;
            continue;
        }
        if (*p == '\\' && p[1]) {
            if (ri + 2 >= sizeof sub->repl) return SED_PARSE_OVERFLOW;
            sub->repl[ri++] = '\\';
            sub->repl[ri++] = p[1];
            p += 2;
            continue;
        }
        if (ri + 1 >= sizeof sub->repl) return SED_PARSE_OVERFLOW;
        sub->repl[ri++] = *p;
        p++;
    }
    if (*p != d) return -1;
    sub->repl[ri] = '\0';
    p++;

    /* flags */
    sub->global = 0; sub->nth = 0; sub->print = 0;
    int icase = 0;
    for (; *p; p++) {
        if (*p == 'g') sub->global = 1;
        else if (*p == 'p') sub->print = 1;
        else if (*p == 'i' || *p == 'I') icase = 1;
        else if (*p >= '0' && *p <= '9') { sub->nth = sub->nth * 10 + (*p - '0'); }
        else return -1;   /* unknown flag (m, e, w file, …) → fall back */
    }
    if (sub->nth == 0) sub->nth = 1;

    int cf = (extended ? REG_EXTENDED : 0) | (icase ? REG_ICASE : 0);
    if (regcomp(&sub->re, pat, cf) != 0) return -1;
    /* A pattern that can match the EMPTY string (e.g. `x*`, `a?`) has fiddly
     * trailing-empty-match semantics; defer those to real sed rather than risk
     * a subtly-wrong -i edit. */
    regmatch_t em[1];
    if (regexec(&sub->re, "", 1, em, 0) == 0) { regfree(&sub->re); return -1; }
    sub->have_re = 1;
    return 0;
}

static int sed_append(char* out, size_t cap, size_t* used,
                      const char* src, size_t len) {
    if (cap == 0 || *used > cap - 1 || len > cap - 1 - *used) return -1;
    memcpy(out + *used, src, len);
    *used += len;
    return 0;
}

/* Expand a replacement into `out` for one match: & = whole match, \1-\9 =
 * groups, \n \t \\ \& escapes. Returns -1 instead of shortening expansion. */
static int sed_expand(const sed_sub_t* sub, const char* line,
                      const regmatch_t* m, char* out, size_t cap,
                      size_t* len_out) {
    size_t o = 0;
    for (const char* r = sub->repl; *r; r++) {
        if (*r == '&') {
            size_t len = (size_t)(m[0].rm_eo - m[0].rm_so);
            if (sed_append(out, cap, &o, line + m[0].rm_so, len) != 0) return -1;
        } else if (*r == '\\' && r[1] >= '1' && r[1] <= '9') {
            int g = r[1] - '0';
            if (m[g].rm_so >= 0) {
                size_t len = (size_t)(m[g].rm_eo - m[g].rm_so);
                if (sed_append(out, cap, &o, line + m[g].rm_so, len) != 0) return -1;
            }
            r++;
        } else if (*r == '\\' && r[1]) {
            char c = r[1];
            c = (c == 'n') ? '\n' : (c == 't') ? '\t' : c;  /* \& \\ → the char */
            if (sed_append(out, cap, &o, &c, 1) != 0) return -1;
            r++;
        } else {
            if (sed_append(out, cap, &o, r, 1) != 0) return -1;
        }
    }
    out[o] = '\0';
    *len_out = o;
    return 0;
}

/* Apply one line, returning -1 rather than constructing shortened output. */
static int sed_apply(const sed_sub_t* sub, const char* line, char* out, size_t cap,
                     int* replaced_out) {
    regmatch_t m[10];
    size_t o = 0;
    int count = 0, replaced = 0;
    const char* p = line;
    int at_bol = 1;
    while (regexec(&sub->re, p, 10, m, at_bol ? 0 : REG_NOTBOL) == 0) {
        count++;
        int do_it = sub->global ? (count >= sub->nth) : (count == sub->nth);
        /* copy text before the match */
        if (sed_append(out, cap, &o, p, (size_t)m[0].rm_so) != 0) return -1;
        if (do_it) {
            char exp[4096];
            size_t el;
            if (sed_expand(sub, p, m, exp, sizeof exp, &el) != 0 ||
                sed_append(out, cap, &o, exp, el) != 0)
                return -1;
            replaced = 1;
        } else {
            size_t len = (size_t)(m[0].rm_eo - m[0].rm_so);
            if (sed_append(out, cap, &o, p + m[0].rm_so, len) != 0) return -1;
        }
        /* advance past the match; handle empty match by copying one char */
        regoff_t adv = m[0].rm_eo;
        if (m[0].rm_eo == m[0].rm_so) {
            if (p[m[0].rm_eo] &&
                sed_append(out, cap, &o, p + m[0].rm_eo, 1) != 0)
                return -1;
            adv = m[0].rm_eo + 1;
        }
        p += adv;
        at_bol = 0;
        if (!sub->global && count >= sub->nth) break;  /* single: done after target */
        if (!*p) break;
    }
    /* copy the remainder */
    if (sed_append(out, cap, &o, p, strlen(p)) != 0) return -1;
    out[o] = '\0';
    *replaced_out = replaced;
    return 0;
}

/* Return 1 for a line, 0 for EOF, -1 for I/O/allocation failure, and -2 when
 * one logical line exceeds the explicit in-process limit. */
static int sed_read_line(FILE* f, char** line, size_t* cap,
                         size_t* len_out, int* has_nl_out) {
    size_t used = 0;
    for (;;) {
        if (*cap - used < 2) {
            size_t limit = (size_t)SED_MAX_LOGICAL_LINE + 2;
            if (*cap >= limit) return -2;
            size_t next = *cap ? *cap * 2 : 65536;
            if (next > limit) next = limit;
            char* grown = realloc(*line, next);
            if (!grown) return -1;
            *line = grown;
            *cap = next;
        }
        if (!fgets(*line + used, (int)(*cap - used), f)) {
            if (ferror(f)) return -1;
            if (used == 0) return 0;
            break;
        }
        used += strlen(*line + used);
        if (used > 0 && (*line)[used - 1] == '\n') {
            (*line)[--used] = '\0';
            if (used > SED_MAX_LOGICAL_LINE) return -2;
            *has_nl_out = 1;
            *len_out = used;
            return 1;
        }
        if (used > SED_MAX_LOGICAL_LINE) return -2;
        if (feof(f)) break;
    }
    (*line)[used] = '\0';
    *has_nl_out = 0;
    *len_out = used;
    return 1;
}

/* Rebuild argv into a single-quoted args string and defer to external sed. */
/* JSON editing must never silently shorten a binary line through strlen.
 * Keep bounded allocation and reject NUL in the native regex text subset. */
static int sed_read_checked(FILE* file, char** line, size_t* capacity, size_t* length, int* newline) {
    size_t used = 0;
    for (;;) {
        if (!(used & 4095u) && jash_mutation_cancel_status()) return -1;
        int ch = fgetc(file);
        if (ch == EOF && ferror(file)) return -1;
        if (ch == EOF && !used) return 0;
        if (ch == 0) return -3;
        if (*capacity <= used + 1) {
            size_t next = *capacity ? *capacity * 2 : 65536;
            if (next > SED_MAX_LOGICAL_LINE + 2u) next = SED_MAX_LOGICAL_LINE + 2u;
            char* grown = realloc(*line, next);
            if (!grown) return -1;
            *line = grown; *capacity = next;
        }
        if (ch == '\n' || ch == EOF) {
            (*line)[used] = '\0'; *length = used; *newline = ch == '\n'; return 1;
        }
        if (used == SED_MAX_LOGICAL_LINE) return -2;
        (*line)[used++] = (char)ch;
    }
}

static int sed_fallback(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv,
                        jash_mutation_report_t* report) {
    if (report) {
        report->error_code = "native_edit_unsupported";
        return 2; /* No external child while a native report is active. */
    }
    char args[8192]; size_t o = 0;
    for (int i = 0; i < argc && o + 4 < sizeof args; i++) {
        if (i) args[o++] = ' ';
        args[o++] = '\'';
        for (const char* p = argv[i]; *p && o + 5 < sizeof args; p++) {
            if (*p == '\'') { args[o++]='\''; args[o++]='\\'; args[o++]='\''; args[o++]='\''; }
            else args[o++] = *p;
        }
        args[o++] = '\'';
    }
    args[o] = '\0';
    return jash_exec_external(s, w, ctx, "sed", args);
}

/* Stream one FILE* through the substitution, emitting to w. -n suppresses the
 * automatic print; p flag / -n interplay matches sed. */
static int sed_stream(FILE* f, const sed_sub_t* sub, int quiet,
                      jash_writer_t w, void* ctx) {
    char *line = NULL, *out = NULL;
    size_t line_cap = 0, out_cap = 0, len;
    int has_nl, read_rc;
    while ((read_rc = sed_read_line(f, &line, &line_cap, &len, &has_nl)) == 1) {
        size_t needed = len + SED_OUTPUT_SLACK + 1;
        if (out_cap < needed) {
            char* grown = realloc(out, needed);
            if (!grown) { free(line); free(out); return -1; }
            out = grown;
            out_cap = needed;
        }
        int did;
        if (sed_apply(sub, line, out, out_cap, &did) != 0) {
            free(line); free(out); return -1;
        }
        if (!quiet) {
            w(ctx, out, strlen(out));
            if (has_nl) w(ctx, "\n", 1);
        }
        if (sub->print && did) {   /* p flag: print the pattern space (again) */
            w(ctx, out, strlen(out));
            if (has_nl) w(ctx, "\n", 1);
        }
    }
    free(line);
    free(out);
    return read_rc;
}

static int sed_run(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv,
                    jash_mutation_report_t* report) {
    int quiet = 0, in_place = 0, extended = 0;
    const char* script = NULL;
    char** files;
    int nfiles = 0;
    int preview = 0;

    int i = 0;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;   /* '-' = stdin operand */
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "-n") == 0 || strcmp(a, "--quiet") == 0 || strcmp(a, "--silent") == 0) quiet = 1;
        else if (strcmp(a, "-E") == 0 || strcmp(a, "-r") == 0 || strcmp(a, "--regexp-extended") == 0) extended = 1;
        else if (!strcmp(a, "--dry-run") && report) { preview = 1; report->preview = 1; }
        else if (strcmp(a, "-e") == 0 && i + 1 < argc) { if (script) return sed_fallback(s, w, ctx, argc, argv, report); script = argv[++i]; }
        else if (strncmp(a, "-e", 2) == 0 && a[2]) { if (script) return sed_fallback(s, w, ctx, argc, argv, report); script = a + 2; }
        else if (strcmp(a, "-i") == 0) in_place = 1;
        else if (strncmp(a, "-i", 2) == 0) {
            if (report) return sed_fallback(s, w, ctx, argc, argv, report);
            in_place = 1; /* Legacy suffix handling; JSON refuses until qualified. */
        }
        else return sed_fallback(s, w, ctx, argc, argv, report);   /* -s, -z, -f, … */
    }

    /* If no -e, the first remaining operand is the script. */
    if (!script) {
        if (i >= argc) return sed_fallback(s, w, ctx, argc, argv, report);
        script = argv[i++];
    }
    files = argv + i;
    nfiles = argc - i;
    if (report && !in_place) { report->error_code = "in_place_required"; return 2; }

    sed_sub_t sub; memset(&sub, 0, sizeof sub);
    int parse_rc = sed_parse_sub(script, extended, &sub);
    if (parse_rc == SED_PARSE_OVERFLOW) {
        jash_writes(jash_stderr_writer, 0,
                    "sed: pattern or replacement exceeds internal capacity\n");
        return 1;
    }
    if (parse_rc != 0)
        return sed_fallback(s, w, ctx, argc, argv, report);   /* not a simple s/// */

    int rc = 0;
    if (in_place) {
        if (nfiles == 0) { regfree(&sub.re); return sed_fallback(s, w, ctx, argc, argv, report); }
        for (int fi = 0; fi < nfiles; fi++) {
            if (jash_mutation_report_stop(report)) { rc = jash_mutation_report_stop(report); break; }
            jash_mutation_item_t item = {.source = files[fi], .destination = files[fi],
                                        .action = "edit", .code = "open_parent"};
            char target_name[1024];
            int parent_fd = sed_open_parent(files[fi], target_name,
                                            sizeof(target_name));
            if (parent_fd < 0) {
                jash_writef(jash_stderr_writer, 0,
                            "sed: cannot open parent for %s\n", files[fi]);
                rc = 1;
                goto item_done;
            }
            int in_fd = openat(parent_fd, target_name,
                               O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
            FILE* in = in_fd >= 0 ? fdopen(in_fd, "r") : NULL;
            if (!in) {
                if (in_fd >= 0) close(in_fd);
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: can't read %s: No such file or directory\n",
                            files[fi]);
                rc = 1;
                item.code = "open_source";
                goto item_done;
            }
            /* SECURITY: unpredictable mkstemp name (O_EXCL, no symlink
             * following) in the target's directory + fsync + preserve the
             * original mode. The old deterministic "<file>.jsed.tmp" opened
             * with fopen("w") followed a pre-planted symlink and clobbered
             * the victim, and the rename lost the original's permissions
             * (0600 files became 0644). */
            struct stat ost, path_ost;
            if (fstat(fileno(in), &ost) != 0 ||
                fstatat(parent_fd, target_name, &path_ost,
                        AT_SYMLINK_NOFOLLOW) != 0 ||
                !S_ISREG(ost.st_mode) || path_ost.st_dev != ost.st_dev ||
                path_ost.st_ino != ost.st_ino) {
                fclose(in);
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: cannot safely identify %s\n", files[fi]);
                rc = 1;
                item.code = "identify_source";
                goto item_done;
            }
            if (preview) {
                fclose(in); close(parent_fd);
                item.status = JASH_MUT_PLANNED; item.code = NULL;
                goto item_done;
            }
            /* Snapshot only after validation, and never during preview. */
            char backup[1024]; int have_backup = (jash_timeline_backup_file(files[fi], backup, sizeof backup) == 0);
            char tmp[1100];
            int tfd = -1;
            for (int attempt = 0; attempt < 128; attempt++) {
                if (snprintf(tmp, sizeof(tmp), "%s.jsed.%ld.%lu", target_name,
                             (long)getpid(), ++sed_temp_serial) >=
                    (int)sizeof(tmp)) {
                    errno = ENAMETOOLONG;
                    break;
                }
                tfd = openat(parent_fd, tmp,
                             O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                             0600);
                if (tfd >= 0 || errno != EEXIST) break;
            }
            if (tfd < 0) { close(parent_fd); fclose(in); jash_writef(jash_stderr_writer, 0, "sed: cannot create temp for %s\n", files[fi]); rc = 1; item.code = "create_temp"; goto item_done; }
            FILE* outf = fdopen(tfd, "w");
            if (!outf) { close(tfd); unlinkat(parent_fd, tmp, 0); close(parent_fd); fclose(in); jash_writef(jash_stderr_writer, 0, "sed: cannot create temp for %s\n", files[fi]); rc = 1; item.code = "open_temp_stream"; goto item_done; }
            char *line = NULL, *obuf = NULL;
            size_t line_cap = 0, obuf_cap = 0, len;
            int io_failed = 0, transform_failed = 0, line_too_long = 0;
            int has_nl, read_rc;
            while ((read_rc = report ? sed_read_checked(in, &line, &line_cap, &len, &has_nl)
                                     : sed_read_line(in, &line, &line_cap, &len, &has_nl)) == 1) {
                size_t needed = len + SED_OUTPUT_SLACK + 1;
                if (obuf_cap < needed) {
                    char* grown = realloc(obuf, needed);
                    if (!grown) { io_failed = 1; break; }
                    obuf = grown;
                    obuf_cap = needed;
                }
                int did;
                if (sed_apply(&sub, line, obuf, obuf_cap, &did) != 0) {
                    transform_failed = 1;
                    io_failed = 1;
                    break;
                }
                if (!quiet && (fputs(obuf, outf) == EOF ||
                               (has_nl && fputc('\n', outf) == EOF))) {
                    io_failed = 1;
                    break;
                }
                if (sub.print && did && (fputs(obuf, outf) == EOF ||
                                         (has_nl && fputc('\n', outf) == EOF))) {
                    io_failed = 1;
                    break;
                }
            }
            if (read_rc == -2) { line_too_long = 1; io_failed = 1; }
            else if (read_rc < 0 || ferror(in)) io_failed = 1;
            free(line);
            free(obuf);
            if (fclose(in) != 0) io_failed = 1;
            if (ferror(outf) || fflush(outf) != 0) io_failed = 1;
            if (!io_failed && fchmod(tfd, ost.st_mode & 0777) != 0) io_failed = 1;
            if (!io_failed && fsync(tfd) != 0) io_failed = 1;
            struct stat temp_owned;
            if (!io_failed && fstat(tfd, &temp_owned) != 0) io_failed = 1;
            if (fclose(outf) != 0) io_failed = 1;
            if (jash_mutation_cancel_status()) io_failed = 1;
            if (io_failed) {
                unlinkat(parent_fd, tmp, 0);
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            line_too_long ? "sed: logical line exceeds 16777216-byte limit for %s\n"
                            : transform_failed ? "sed: transformed output too large for %s\n"
                                             : "sed: failed writing %s\n",
                            files[fi]);
                rc = 1;
                item.code = read_rc == -3 ? "binary_input_unsupported" : line_too_long ? "line_limit" :
                            transform_failed ? "transform_limit" : "prepare_edit";
                goto item_done;
            }
            struct stat current;
            if (fstatat(parent_fd, target_name, &current,
                        AT_SYMLINK_NOFOLLOW) != 0 ||
                current.st_dev != ost.st_dev || current.st_ino != ost.st_ino) {
                unlinkat(parent_fd, tmp, 0);
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: refusing to replace changed target %s\n", files[fi]);
                rc = 1;
                item.code = "target_changed";
                goto item_done;
            }
            if (sed_exchange(parent_fd, tmp, target_name) != 0) {
                unlinkat(parent_fd, tmp, 0);
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: atomic exchange unavailable for %s\n", files[fi]);
                rc = 1;
                item.code = "exchange_failed";
                goto item_done;
            }
            item.effect = JASH_MUT_POSSIBLE;
            struct stat published;
            if (fstatat(parent_fd, tmp, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
                !sed_same_inode(&current, &ost) ||
                fstatat(parent_fd, target_name, &published,
                        AT_SYMLINK_NOFOLLOW) != 0 ||
                !sed_same_inode(&published, &temp_owned)) {
                int rollback_rc =
                    (fstatat(parent_fd, tmp, &current, AT_SYMLINK_NOFOLLOW) == 0)
                    ? sed_rollback_exchange(parent_fd, tmp, target_name,
                                            &temp_owned, &current)
                    : -1;
                if (rollback_rc != 0)
                    jash_writef(jash_stderr_writer, 0,
                                "sed: rollback incomplete; recovery retained at %s\n", tmp);
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: target changed during commit: %s\n", files[fi]);
                rc = 1;
                item.code = "commit_target_changed";
                goto item_done;
            }
            if (fsync(parent_fd) != 0) {
                int saved = errno;
                if (sed_rollback_exchange(parent_fd, tmp, target_name,
                                          &temp_owned, &ost) != 0)
                    jash_writef(jash_stderr_writer, 0,
                                "sed: rollback incomplete; recovery retained at %s\n", tmp);
                (void)fsync(parent_fd);
                close(parent_fd);
                errno = saved;
                jash_writef(jash_stderr_writer, 0,
                            "sed: parent directory sync failed for %s\n", files[fi]);
                rc = 1;
                item.code = "publication_sync";
                goto item_done;
            }
            item.effect = JASH_MUT_CHANGED;
            if (fstatat(parent_fd, tmp, &current, AT_SYMLINK_NOFOLLOW) != 0 ||
                !sed_same_inode(&current, &ost)) {
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: old inode changed before cleanup; retained at %s\n", tmp);
                rc = 1;
                item.code = "cleanup_target_changed";
                goto item_done;
            }
            if (unlinkat(parent_fd, tmp, 0) != 0) {
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: committed but cannot remove old inode %s\n", tmp);
                rc = 1;
                item.code = "cleanup_unlink";
                goto item_done;
            }
            if (fsync(parent_fd) != 0) {
                close(parent_fd);
                jash_writef(jash_stderr_writer, 0,
                            "sed: committed but cleanup sync failed for %s\n",
                            files[fi]);
                rc = 1;
                item.code = "cleanup_sync";
                goto item_done;
            }
            close(parent_fd);
            char desc[256]; snprintf(desc, sizeof desc, "sed -i %s", files[fi]);
            jash_timeline_record_ref(JASH_UNDO_FILE_OVERWRITE, desc, files[fi], have_backup ? backup : "",
                                     report ? &item.undo : NULL);
            item.status = JASH_MUT_COMPLETED; item.code = NULL;
item_done:
            if (jash_mutation_report_item(report, &item)) { rc = 125; break; }
        }
    } else if (nfiles == 0) {
        int stream_rc = sed_stream(stdin, &sub, quiet, w, ctx);
        if (stream_rc != 0) {
            jash_writes(jash_stderr_writer, 0,
                        stream_rc == -2 ? "sed: logical line exceeds 16777216-byte limit\n"
                                        : "sed: transformed output exceeds internal capacity\n");
            rc = 1;
        }
    } else {
        for (int fi = 0; fi < nfiles; fi++) {
            FILE* f = (strcmp(files[fi], "-") == 0) ? stdin : fopen(files[fi], "r");
            if (!f) { jash_writef(jash_stderr_writer, 0, "sed: can't read %s: No such file or directory\n", files[fi]); rc = 1; continue; }
            int stream_rc = sed_stream(f, &sub, quiet, w, ctx);
            if (stream_rc != 0) {
                jash_writef(jash_stderr_writer, 0,
                            stream_rc == -2 ? "sed: logical line exceeds 16777216-byte limit for %s\n"
                                            : "sed: transformed output too large for %s\n",
                            files[fi]);
                rc = 1;
            }
            if (f != stdin) fclose(f);
        }
    }

    if (sub.have_re) regfree(&sub.re);
    return rc;
}

int jash_b_sed(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    static const char* const values[] = {"-e", "--expression", "-f", "--file", NULL};
    int count;
    char** args;
    jash_mutation_format_t format;
    int json = jash_mutation_args_mode(argc, argv, values, &count, &args, &format);
    int args_error = errno;
    if (!json) return sed_run(s, w, ctx, argc, argv, NULL);
    jash_mutation_report_t report;
    if (jash_mutation_report_begin_mode(&report, "sed", "filesystem_action", format, w, ctx)) { free(args); return 125; }
    if (json < 0) { report.error_code = args_error == ENOMEM ? "out_of_memory" : "invalid_format_options"; return jash_mutation_report_end(&report, 2); }
    int rc = sed_run(s, jash_stderr_writer, NULL, count, args, &report);
    free(args);
    return jash_mutation_report_end(&report, rc);
}
