// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/blame-file — attested authorship per file (ADR-0042 F7).
//
// In a swarm the agent inherits files it did not write and cannot ask "who did
// this, when, with what command". git blame knows commits, not agent
// sessions/commands. `blame-file <path> [--json]` answers from jash's own
// attested substrate: the undo timeline (which files were mutated, how, when)
// cross-referenced to the sigil chain (the exact command + seq) by timestamp
// window. Newest-first. Honest v1 scope: per-file, not per-line. F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/timeline.h>
#include <core/chain_read.h>
#include <core/agent_json.h>   /* jash_json_esc */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <time.h>
#include <unistd.h>

#define BF_WINDOW_S 10   /* attribute a mutation to a chain command within this */

static void bf_norm(const char* p, char* out, size_t cap) {
    char rp[PATH_MAX];
    if (realpath(p, rp)) { snprintf(out, cap, "%s", rp); return; }
    /* The leaf may not exist — exactly the deleted-file blame case. Resolve
     * the parent dir and keep the basename literal, mirroring tl_canon, so
     * the query matches the timeline's canonicalized entries (macOS
     * /var → /private/var; without this a blame on any deleted file under a
     * symlinked dir returned []). */
    char abs[PATH_MAX];
    if (p[0] == '/') { snprintf(abs, sizeof abs, "%s", p); }
    else {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof cwd)) snprintf(abs, sizeof abs, "%s/%s", cwd, p);
        else { snprintf(out, cap, "%s", p); return; }
    }
    char dir[PATH_MAX]; snprintf(dir, sizeof dir, "%s", abs);
    char* slash = strrchr(dir, '/');
    if (slash && slash[1]) {
        const char* base = slash + 1;
        if (slash == dir) slash[1] = '\0'; else *slash = '\0';
        char rd[PATH_MAX];
        if (realpath(dir, rd)) {
            size_t rl = strlen(rd);
            snprintf(out, cap, "%s%s%s", rd, (rl && rd[rl-1] == '/') ? "" : "/", base);
            return;
        }
    }
    snprintf(out, cap, "%s", abs);
}

static const char* bf_kind(int kind, int* is_del) {
    *is_del = 0;
    switch (kind) {
        case JASH_UNDO_FILE_OVERWRITE: return "modified";
        case JASH_UNDO_FILE_CREATE:    return "created";
        case JASH_UNDO_TRASH:          *is_del = 1; return "deleted";
        case JASH_UNDO_MOVE:           return "moved";
        default:                       return NULL;   /* env/chdir: not a file */
    }
}

static void bf_iso(long when, char* out, size_t cap) {
    time_t t = (time_t)when;
    struct tm tm;
    if (gmtime_r(&t, &tm)) strftime(out, cap, "%Y-%m-%dT%H:%M:%SZ", &tm);
    else snprintf(out, cap, "%ld", when);
}

int jash_b_blamefile(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    (void)s;
    /* parse: first bare token = path; --json anywhere */
    int json = 0;
    char path[PATH_MAX] = "";
    if (args) {
        const char* p = args;
        while (*p) {
            while (*p == ' ' || *p == '\t') p++;
            if (!*p) break;
            const char* start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            size_t len = (size_t)(p - start);
            if (len == 6 && strncmp(start, "--json", 6) == 0) { json = 1; continue; }
            if (!path[0]) { size_t n = len < sizeof path - 1 ? len : sizeof path - 1; memcpy(path, start, n); path[n] = 0; }
        }
    }
    if (!path[0]) {
        jash_writes(jash_stderr_writer, 0, "blame-file: need a file path\r\n");
        return 2;
    }
    char norm[PATH_MAX]; bf_norm(path, norm, sizeof norm);

    /* Chain tail for command cross-reference (recent history is plenty). */
    static jash_chain_entry_t chain[1024];
    int nc = jash_chain_tail(chain, 1024, 1024);

    int found = 0;
    if (json) jash_writes(w, ctx, "[");
    for (int i = 0; ; i++) {
        int kind; const char *a = 0, *b = 0; long when = 0;
        if (!jash_timeline_peek2(i, &kind, &a, &b, &when)) break;
        int is_del = 0;
        const char* kstr = bf_kind(kind, &is_del);
        if (!kstr) continue;
        char na[PATH_MAX] = "", nb[PATH_MAX] = "";
        if (a && *a) bf_norm(a, na, sizeof na);
        if (b && *b) bf_norm(b, nb, sizeof nb);
        if (strcmp(na, norm) != 0 && strcmp(nb, norm) != 0) continue;

        /* closest chain entry within the window → (seq, cmd) */
        long best_seq = -1, best_diff = LONG_MAX; const char* best_cmd = "";
        for (int c = 0; c < nc; c++) {
            long d = (long)chain[c].ts_sec - when; if (d < 0) d = -d;
            if (d < best_diff) { best_diff = d; best_seq = (long)chain[c].seq; best_cmd = chain[c].cmd; }
        }
        int has_chain = (best_seq >= 0 && best_diff <= BF_WINDOW_S);

        char iso[40]; bf_iso(when, iso, sizeof iso);
        if (json) {
            char ep[1100], ec[1100];
            jash_json_esc(norm, ep, sizeof ep);
            jash_json_esc(has_chain ? best_cmd : "", ec, sizeof ec);
            jash_writef(w, ctx, "%s{\"ts\":\"%s\",\"kind\":\"%s\",\"path\":\"%s\"",
                        found ? "," : "", iso, kstr, ep);
            if (has_chain) jash_writef(w, ctx, ",\"chain\":%ld,\"cmd\":\"%s\"", best_seq, ec);
            if (is_del)    jash_writes(w, ctx, ",\"restorable\":true,\"restore\":\"rewind auto\"");
            jash_writes(w, ctx, "}");
        } else {
            jash_writef(w, ctx, "  %s  \033[1m%-9s\033[0m", iso, kstr);
            if (has_chain) jash_writef(w, ctx, "  #%ld  %s", best_seq, best_cmd);
            if (is_del)    jash_writes(w, ctx, "  \033[90m(restorable: rewind auto)\033[0m");
            jash_writes(w, ctx, "\r\n");
        }
        found++;
    }
    if (json) jash_writes(w, ctx, "]\n");

    if (!found) {
        if (!json)
            jash_writef(jash_stderr_writer, 0,
                        "blame-file: no attested history for '%s'\r\n", norm);
        return 2;   /* unknown / untracked file */
    }
    return 0;
}

#include "blamefile_native.inc"
