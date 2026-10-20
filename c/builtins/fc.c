// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: fc — fix command (history editor / re-executor)
//
// POSIX / bash compatibility:
//   fc -l [-nr] [first [last]]   list history range
//   fc -s [pat=rep] [cmd]        re-run command (with optional substitution)
//   fc [-e editor] [-nr] [first [last]]  edit then re-execute

#include "builtin.h"
#include <core/history.h>
#include <core/env.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>

extern int jash_eval_line(jash_state_t*, jash_writer_t, void*, const char*);

static int skip_ws_char(const char* p) {
    return *p == ' ' || *p == '\t';
}

static const char* skip_ws(const char* p) {
    while (skip_ws_char(p)) p++;
    return p;
}

/* Resolve a history argument to a 0-based index.
 * User-facing numbers are 1-based (1 = oldest entry).
 * Negative numbers count from the end (-1 = newest).
 * Strings: find the most-recent entry starting with that string.
 * Returns -1 on failure. */
static int resolve_hist(jash_state_t* s, const char* arg) {
    uint32_t cnt = jash_history_count(s);
    if (!cnt) return -1;

    /* Numeric argument */
    if (*arg == '-' || (*arg >= '0' && *arg <= '9')) {
        int n = atoi(arg);
        if (n < 0) {
            /* -1 = newest, -2 = second newest, ... */
            int idx = (int)cnt + n;
            return (idx >= 0) ? idx : 0;
        }
        if (n == 0) return (int)cnt - 1;  /* 0 → newest */
        /* 1-based absolute: clamp to [0, cnt-1] */
        int idx = n - 1;
        return (idx < (int)cnt) ? idx : (int)cnt - 1;
    }

    /* String: find most-recent entry starting with arg */
    size_t alen = strlen(arg);
    for (int i = (int)cnt - 1; i >= 0; i--) {
        const char* line = jash_history_at(s, (uint32_t)i);
        if (line && strncmp(line, arg, alen) == 0) return i;
    }
    return -1;
}

/* Apply a single pat=rep substitution on src into dst (cap bytes).
 * Returns 1 if substitution happened, 0 otherwise. */
static int apply_subst(const char* src, const char* pat, const char* rep,
                       char* dst, size_t cap) {
    const char* hit = strstr(src, pat);
    if (!hit) {
        size_t sl = strlen(src);
        if (sl >= cap) sl = cap - 1;
        memcpy(dst, src, sl); dst[sl] = '\0';
        return 0;
    }
    size_t pre = (size_t)(hit - src);
    size_t pl  = strlen(pat);
    size_t rl  = strlen(rep);
    size_t post_start = pre + pl;
    size_t post_len   = strlen(src + post_start);
    size_t total = pre + rl + post_len;
    if (total >= cap) total = cap - 1;
    memcpy(dst, src, pre);
    size_t rcp = (pre + rl < cap - 1) ? rl : cap - 1 - pre;
    memcpy(dst + pre, rep, rcp);
    size_t pcp = (pre + rl + post_len < cap) ? post_len : cap - pre - rl - 1;
    if (pcp > 0) memcpy(dst + pre + rl, src + post_start, pcp);
    dst[total] = '\0';
    return 1;
}

/* Write a command to a temp file, invoke editor, read it back.
 * Returns malloc'd buffer with the edited command or NULL. */
static char* edit_command(const char* cmd, const char* editor_bin) {
    char tmppath[64];
    const char* tdir = jash_tmpdir();
    snprintf(tmppath, sizeof(tmppath), "%s/jash_fc_XXXXXX", tdir);
    int fd = mkstemp(tmppath);
    if (fd < 0) return NULL;

    /* Write command to temp file */
    size_t clen = strlen(cmd);
    ssize_t wd1 = write(fd, cmd, clen);
    (void)wd1;
    ssize_t wd2 = write(fd, "\n", 1);
    (void)wd2;
    close(fd);

    /* Launch editor via fork+execl (safer than system()) */
    pid_t pid = fork();
    if (pid == 0) {
        execl(editor_bin, editor_bin, tmppath, NULL);
        _exit(127);
    }
    int status;
    waitpid(pid, &status, 0);

    /* Read back */
    FILE* f = fopen(tmppath, "r");
    if (!f) { unlink(tmppath); return NULL; }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    rewind(f);
    if (sz <= 0) { fclose(f); unlink(tmppath); return NULL; }
    char* buf = malloc((size_t)sz + 1);
    if (!buf) { fclose(f); unlink(tmppath); return NULL; }
    size_t nr = fread(buf, 1, (size_t)sz, f);
    buf[nr] = '\0';
    /* Strip trailing newline */
    while (nr > 0 && (buf[nr-1] == '\n' || buf[nr-1] == '\r')) {
        buf[--nr] = '\0';
    }
    fclose(f);
    unlink(tmppath);
    return buf;
}

int jash_b_fc(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    const char* p = args ? skip_ws(args) : "";

    int list_mode  = 0;
    int subst_mode = 0;
    int reverse    = 0;
    int no_nums    = 0;
    char editor_bin[128] = "";

    /* Parse flags */
    while (*p == '-' || *p == '+') {
        char sign = *p++;
        if (sign == '+') break;  /* -- or +: end of options */
        while (*p && !skip_ws_char(p)) {
            char opt = *p++;
            if (opt == 'l') { list_mode = 1; }
            else if (opt == 's') { subst_mode = 1; }
            else if (opt == 'r') { reverse = 1; }
            else if (opt == 'n') { no_nums = 1; }
            else if (opt == 'e') {
                p = skip_ws(p);
                unsigned ei = 0;
                while (*p && !skip_ws_char(p) && ei + 1 < sizeof(editor_bin))
                    editor_bin[ei++] = *p++;
                editor_bin[ei] = '\0';
            } else if (opt == '-') {
                /* bare -- ends option parsing */
                goto options_done;
            }
        }
        p = skip_ws(p);
    }
options_done:
    p = skip_ws(p);

    uint32_t cnt = jash_history_count(s);
    if (!cnt) {
        jash_writes(w, ctx, "fc: no history\n");
        return 1;
    }

    /* ── fc -s [pat=rep] [cmd] ── */
    if (subst_mode) {
        /* Optional pat=rep substitution */
        char pat[256] = "", rep[256] = "";
        int  have_subst = 0;

        /* Peek: if arg contains '=' and the left side has no spaces, it's pat=rep */
        const char* eq = strchr(p, '=');
        if (eq) {
            /* Confirm no space before '=' */
            int ok = 1;
            for (const char* q = p; q < eq; q++) if (*q == ' ' || *q == '\t') { ok = 0; break; }
            if (ok) {
                size_t plen = (size_t)(eq - p);
                if (plen < sizeof(pat)) { memcpy(pat, p, plen); pat[plen] = '\0'; }
                p = eq + 1;
                unsigned ri = 0;
                while (*p && !skip_ws_char(p) && ri + 1 < sizeof(rep)) rep[ri++] = *p++;
                rep[ri] = '\0';
                have_subst = (*pat != '\0');
                p = skip_ws(p);
            }
        }

        /* Remaining arg is the command selector (default: last) */
        int idx = (*p) ? resolve_hist(s, p) : (int)cnt - 1;
        if (idx < 0) {
            jash_writef(w, ctx, "fc: %s: not found in history\n", p);
            return 1;
        }

        const char* orig = jash_history_at(s, (uint32_t)idx);
        if (!orig) return 1;

        char cmd[JASH_HIST_LINE_LEN];
        if (have_subst)
            apply_subst(orig, pat, rep, cmd, sizeof(cmd));
        else {
            size_t ol = strlen(orig);
            if (ol >= sizeof(cmd)) ol = sizeof(cmd) - 1;
            memcpy(cmd, orig, ol); cmd[ol] = '\0';
        }

        jash_writef(w, ctx, "%s\n", cmd);
        jash_history_push(s, cmd);
        return jash_eval_line(s, w, ctx, cmd);
    }

    /* ── fc -l [first [last]] ── */
    /* Parse optional first / last args */
    char first_arg[128] = "", last_arg[128] = "";
    if (*p) {
        unsigned fi = 0;
        while (*p && !skip_ws_char(p) && fi + 1 < sizeof(first_arg)) first_arg[fi++] = *p++;
        first_arg[fi] = '\0';
        p = skip_ws(p);
    }
    if (*p) {
        unsigned li = 0;
        while (*p && !skip_ws_char(p) && li + 1 < sizeof(last_arg)) last_arg[li++] = *p++;
        last_arg[li] = '\0';
    }

    int first_idx, last_idx;

    if (list_mode) {
        /* Default: last 16 entries */
        first_idx = (*first_arg) ? resolve_hist(s, first_arg) : (int)cnt - 16;
        last_idx  = (*last_arg)  ? resolve_hist(s, last_arg)  : (int)cnt - 1;
        if (first_idx < 0) first_idx = 0;
        if (last_idx  < 0) last_idx  = (int)cnt - 1;
    } else {
        /* Edit mode: default to last command */
        first_idx = (*first_arg) ? resolve_hist(s, first_arg) : (int)cnt - 1;
        last_idx  = (*last_arg)  ? resolve_hist(s, last_arg)  : first_idx;
        if (first_idx < 0) first_idx = (int)cnt - 1;
        if (last_idx  < 0) last_idx  = first_idx;
    }

    /* Swap for reverse */
    if (!reverse && first_idx > last_idx) { int t = first_idx; first_idx = last_idx; last_idx = t; }

    if (list_mode) {
        /* ── list entries ── */
        int step = reverse ? -1 : 1;
        int start = reverse ? last_idx : first_idx;
        int end   = reverse ? first_idx : last_idx;
        for (int i = start; reverse ? i >= end : i <= end; i += step) {
            const char* line = jash_history_at(s, (uint32_t)i);
            if (!line) continue;
            if (no_nums)
                jash_writef(w, ctx, "\t%s\n", line);
            else
                jash_writef(w, ctx, "%5d\t%s\n", i + 1, line);
        }
        return 0;
    }

    /* ── edit then execute ── */
    /* Concatenate the range into one buffer */
    char cmdbuf[4096]; unsigned cpos = 0;
    int step = 1;
    int start = first_idx, end = last_idx;
    if (reverse) { step = -1; start = last_idx; end = first_idx; }
    for (int i = start; reverse ? i >= end : i <= end; i += step) {
        const char* line = jash_history_at(s, (uint32_t)i);
        if (!line) continue;
        size_t ll = strlen(line);
        if (cpos + ll + 2 < sizeof(cmdbuf)) {
            memcpy(cmdbuf + cpos, line, ll); cpos += (unsigned)ll;
            cmdbuf[cpos++] = '\n';
        }
    }
    cmdbuf[cpos] = '\0';
    /* Trim trailing newline */
    while (cpos > 0 && cmdbuf[cpos-1] == '\n') cmdbuf[--cpos] = '\0';

    /* Pick editor: -e arg > FCEDIT > EDITOR > vi */
    const char* ed = editor_bin[0] ? editor_bin : NULL;
    if (!ed) { const char* fe = jash_env_get(s, "FCEDIT"); if (fe && *fe) ed = fe; }
    if (!ed) { const char* fe = jash_env_get(s, "EDITOR");  if (fe && *fe) ed = fe; }
    if (!ed) ed = "vi";

    char* edited = edit_command(cmdbuf, ed);
    if (!edited) return 1;

    jash_writef(w, ctx, "%s\n", edited);
    jash_history_push(s, edited);
    int rc = jash_eval_line(s, w, ctx, edited);
    free(edited);
    return rc;
}
