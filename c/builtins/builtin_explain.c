// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/explain — honest blast-radius preview (ADR-0023 E3b).
//
// `explain <command line>` reports what a command WOULD touch, WITHOUT running it.
// It predicts the effects jash actually knows — its mutating builtins (rm/mv/cp/
// mkdir/…), shell redirections (>, >>), and network tools — and is HONEST about
// the rest: an external compiler/script is marked "effects not statically known"
// rather than guessed. No sandbox, no execution, no false confidence.
//
//   explain rm -rf build && cc -o app main.c && echo ok > log
//
// Pairs with `checkpoint`/`rewind`: explain to see the plan, checkpoint, run,
// rewind if wrong. F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>   /* JINV-048: jash_json_esc */
#include <core/guard.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

enum { EFFECT_NONE, EFFECT_DELETE, EFFECT_TRASH, EFFECT_MOVE, EFFECT_COPY,
       EFFECT_CREATE, EFFECT_CHMOD, EFFECT_WRITE, EFFECT_NET, EFFECT_EXTERNAL };

static int in_set(const char* w, const char* const* set) {
    for (int i = 0; set[i]; i++) if (!strcmp(w, set[i])) return 1;
    return 0;
}

/* classify the verb of a segment; returns an EFFECT_* and a human tag. */
static int classify(const char* verb, const char** tag) {
    static const char* del[]  = {"rm","unlink",0};
    static const char* mv_[]  = {"mv",0};
    static const char* cp_[]  = {"cp",0};
    static const char* cr[]   = {"mkdir","touch","mktemp","install",0};
    static const char* perm[] = {"chmod","chown","chgrp","ln",0};
    static const char* net[]  = {"curl","wget","nc","ssh","scp","rsync","ftp","telnet",0};
    static const char* ro[]   = {"echo","printf","ls","cat","pwd","grep","find","head",
                                 "tail","wc","stat","which","type","test","true","false",
                                 "date","env","sort","uniq","cut","sed","awk","diff","file",
                                 "basename","dirname","readlink","realpath","recall","expect",0};
    if (in_set(verb, del))  { *tag = "DELETE";   return EFFECT_DELETE; }
    if (!strcmp(verb,"trash")) { *tag = "trash (recoverable)"; return EFFECT_TRASH; }
    if (in_set(verb, mv_))  { *tag = "MOVE";     return EFFECT_MOVE; }
    if (in_set(verb, cp_))  { *tag = "COPY";     return EFFECT_COPY; }
    if (in_set(verb, cr))   { *tag = "CREATE";   return EFFECT_CREATE; }
    if (in_set(verb, perm)) { *tag = "CHMOD/LINK"; return EFFECT_CHMOD; }
    if (in_set(verb, net))  { *tag = "NETWORK";  return EFFECT_NET; }
    if (in_set(verb, ro))   { *tag = "read-only"; return EFFECT_NONE; }
    *tag = "external (effects not statically known)";
    return EFFECT_EXTERNAL;
}

/* find a `>`/`>>` redirect target in a segment (outside quotes). */
static int find_redirect(const char* seg, char* target, size_t cap, int* append) {
    int sq = 0, dq = 0;
    for (const char* p = seg; *p; p++) {
        if (!dq && *p == '\'') sq = !sq;
        else if (!sq && *p == '"') dq = !dq;
        else if (!sq && !dq && *p == '>') {
            int ap = (p[1] == '>');
            const char* q = p + (ap ? 2 : 1);
            while (*q == ' ' || *q == '\t') q++;
            if (*q == '&') return 0;           /* >&2 etc — not a file */
            size_t i = 0;
            while (*q && *q != ' ' && *q != '\t' && *q != ';' && i < cap - 1) target[i++] = *q++;
            target[i] = '\0';
            if (!i) return 0;
            *append = ap;
            return 1;
        }
    }
    return 0;
}

/* ── --risk: verdict derived from the SAME analysis (ADR-0099 W2.3) ──────
 * sense's job — "how dangerous is this command line?" — is the same question
 * explain already answers; the score comes from the SAME counters, never a
 * second opinion (JINV-047 discipline). Two layers:
 *   1. catastrophic constructs the effect classes cannot express (fork bomb,
 *      rm on / or ~, mkfs, dd to a device, download piped to a shell,
 *      chmod 777) — checked segment-aware, never naive substring: the old
 *      sense matched "rm -rf /" inside "rm -rf /home", a false critical.
 *   2. effect-derived score from the blast counters.
 * score = max(1, 2). Bands: <20 safe, 20-49 caution, 50-79 danger,
 * >=80 critical. rc: 0 safe/caution, 1 danger, 124 critical (124 keeps the
 * old sense abort contract). */

/* extract the verb of one segment (skips leading VAR=val assignments). */
static void seg_verb(const char* seg, char* verb, size_t cap) {
    const char* q = seg;
    while (*q == ' ') q++;
    while (1) {
        int looks = 0;
        for (const char* r = q; *r && *r != ' '; r++) { if (*r == '=') { looks = 1; break; } }
        if (!looks) break;
        while (*q && *q != ' ') q++;
        while (*q == ' ') q++;
    }
    size_t vi = 0;
    while (*q && *q != ' ' && vi < cap - 1) verb[vi++] = *q++;
    verb[vi] = '\0';
}

static int seg_has_word(const char* seg, const char* word) {
    size_t wl = strlen(word);
    for (const char* p = seg; (p = strstr(p, word)); p += wl) {
        int left = (p == seg) || p[-1] == ' ' || p[-1] == '\t';
        int right = !p[wl] || p[wl] == ' ' || p[wl] == '\t' || p[wl] == '/';
        if (left && right) return 1;
    }
    return 0;
}

/* rm/unlink with recursive+force flags aiming at / or ~ — the catastrophic
 * cases. Word-boundary checks so "/home" never trips the "/" rule. */
static int seg_rm_catastrophe(const char* seg, int* is_home) {
    int rec = 0, force = 0, root = 0, home = 0;
    const char* p = seg;
    while (*p && *p != ' ') p++;                 /* skip verb */
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char tok[256]; size_t ti = 0;
        while (*p && *p != ' ' && *p != '\t' && ti < sizeof(tok) - 1) tok[ti++] = *p++;
        tok[ti] = '\0';
        if (!ti) continue;
        if (tok[0] == '-') {
            if (!strcmp(tok, "--recursive")) rec = 1;
            else if (!strcmp(tok, "--force")) force = 1;
            else if (tok[1] != '-') {
                for (size_t k = 1; tok[k]; k++) {
                    if (tok[k] == 'r' || tok[k] == 'R') rec = 1;
                    if (tok[k] == 'f') force = 1;
                }
            }
        } else {
            if (!strcmp(tok, "/") || !strcmp(tok, "/*")) root = 1;
            if (!strcmp(tok, "~") || !strncmp(tok, "~/", 2) ||
                !strcmp(tok, "$HOME") || !strncmp(tok, "$HOME/", 6)) home = 1;
        }
    }
    if (rec && force && root) { *is_home = 0; return 1; }
    if (rec && force && home) { *is_home = 1; return 1; }
    return 0;
}

/* catastrophic pattern score for the whole line; reason set to the top hit. */
static int risk_catastrophic(char* const* segs, int nseg, const char* raw,
                             const char** reason) {
    int top = 0;
    *reason = "";
    if (strstr(raw, ":(){") || strstr(raw, ":|:")) {
        *reason = "fork bomb";
        return 100;
    }
    int downloads = strstr(raw, "curl") || strstr(raw, "wget");
    for (int i = 0; i < nseg; i++) {
        char verb[64];
        seg_verb(segs[i], verb, sizeof verb);
        if (!verb[0]) continue;
        if (!strcmp(verb, "rm") || !strcmp(verb, "unlink")) {
            int is_home = 0;
            if (seg_rm_catastrophe(segs[i], &is_home)) {
                int sc = is_home ? 90 : 100;
                if (sc > top) { top = sc; *reason = is_home ?
                    "rm -rf on home directory" : "rm -rf on / (system destroy)"; }
            }
        }
        if (!strncmp(verb, "mkfs", 4) && (verb[4] == '\0' || verb[4] == '.')) {
            if (85 > top) { top = 85; *reason = "mkfs formats a device"; }
        }
        if (!strcmp(verb, "dd") && strstr(segs[i], "of=/dev/")) {
            if (80 > top) { top = 80; *reason = "dd writes directly to a device"; }
        }
        if (downloads && (!strcmp(verb, "sh") || !strcmp(verb, "bash") ||
                          !strcmp(verb, "dash") || !strcmp(verb, "zsh"))) {
            if (75 > top) { top = 75; *reason = "download piped straight to a shell"; }
        }
        if (!strcmp(verb, "chmod") && seg_has_word(segs[i], "777")) {
            if (60 > top) { top = 60; *reason = "chmod 777 (world-writable)"; }
        }
    }
    return top;
}

static const char* risk_band(int score) {
    if (score >= 80) return "critical";
    if (score >= 50) return "danger";
    if (score >= 20) return "caution";
    return "safe";
}

/* ── target-aware write weight ─────────────────────────────────────────────
 * A `>` is not a `>`: clobbering an EXISTING project file is the dangerous
 * case; creating a fresh log in scratch space is nearly free. Weigh by what
 * the redirect actually does (measured: an 11-way experiment fan-out writing
 * only new /tmp logs scored 100 critical — pure alarm noise). */
static int redir_write_pts(const char* target, int append) {
    if (!target || !*target) return 0;
    if (!strcmp(target, "/dev/null")) return 0;
    struct stat st;
    if (stat(target, &st) == 0) return append ? 5 : 15;  /* overwrite hurts, append less */
    const char* tmp = getenv("TMPDIR");
    if (!strncmp(target, "/tmp/", 5) || !strncmp(target, "/var/tmp/", 9) ||
        !strncmp(target, "/private/tmp/", 13) ||
        !strncmp(target, "/var/folders/", 13) ||
        (tmp && *tmp && !strncmp(target, tmp, strlen(tmp))))
        return 2;                                        /* new file in scratch space */
    return 5;                                            /* new file in the project */
}

/* ── scoring counters (single source: headless hook AND the builtin) ──────
 * Occurrence counts stay for display; the SCORE uses weighted points and
 * DISTINCT opaque verbs — running the same binary 11 times is one unknown,
 * not eleven. */
typedef struct {
    int n_del, n_write, n_net, n_ext;   /* occurrences (display/reason) */
    int write_pts;                       /* sum of redir_write_pts */
    char ext_verbs[32][64]; int n_extv;  /* distinct opaque verbs */
} risk_counters_t;

static void risk_count_seg(risk_counters_t* rc, const char* verb, int eff,
                           int has_redir, const char* redir, int append) {
    if (eff == EFFECT_DELETE) rc->n_del++;
    else if (eff == EFFECT_NET) rc->n_net++;
    else if (eff == EFFECT_EXTERNAL) {
        rc->n_ext++;
        int seen = 0;
        for (int k = 0; k < rc->n_extv; k++)
            if (!strcmp(rc->ext_verbs[k], verb)) { seen = 1; break; }
        if (!seen && rc->n_extv < 32) {
            snprintf(rc->ext_verbs[rc->n_extv], 64, "%s", verb);
            rc->n_extv++;
        }
    }
    if (has_redir) { rc->n_write++; rc->write_pts += redir_write_pts(redir, append); }
}

static int risk_derived(const risk_counters_t* rc, int del_pts) {
    int d = del_pts * rc->n_del + rc->write_pts + 20 * rc->n_net + 5 * rc->n_extv;
    return d > 100 ? 100 : d;
}

/* ADR-0050 W12: headless risk scoring for the pre-exec hook — the SAME
 * engine as `explain --risk` (segment + classify + counters + catastrophes),
 * no writer, no output. Single source: this function and jash_b_explain call
 * the same helpers, so the hook can never disagree with the builtin. */
int jash_explain_risk(const char* line, const char** reason_out) {
    if (reason_out) *reason_out = "";
    if (!line || !*line) return 0;
    char buf[4096], raw[4096];
    snprintf(buf, sizeof buf, "%s", line);
    snprintf(raw, sizeof raw, "%s", line);

    char* segs[32]; int nseg = 0;
    int sq = 0, dq = 0;
    char* start = buf;
    for (char* p = buf; ; p++) {
        char c = *p;
        if (c == '\'' && !dq) sq = !sq;
        else if (c == '"' && !sq) dq = !dq;
        int brk = 0, skip = 0;
        if (!sq && !dq) {
            if (c == ';') { brk = 1; skip = 0; }
            else if (c == '|') { brk = 1; skip = (p[1] == '|'); }
            else if (c == '&' && p[1] == '&') { brk = 1; skip = 1; }
        }
        if (brk || c == '\0') {
            *p = '\0';
            char* t = start; while (*t == ' ' || *t == '\t') t++;
            if (*t && nseg < 32) segs[nseg++] = t;
            if (c == '\0') break;
            if (skip) p++;
            start = p + 1;
        }
    }
    if (nseg == 0) return 0;

    risk_counters_t rc; memset(&rc, 0, sizeof rc);
    for (int i = 0; i < nseg; i++) {
        char verb[64];
        seg_verb(segs[i], verb, sizeof verb);
        if (!verb[0]) continue;
        const char* tag = "";
        int eff = classify(verb, &tag);
        char redir[512]; int append = 0;
        int has_redir = find_redirect(segs[i], redir, sizeof redir, &append);
        risk_count_seg(&rc, verb, eff, has_redir, redir, append);
    }

    const char* reason = "";
    int cat = risk_catastrophic(segs, nseg, raw, &reason);
    int del_pts = jash_trash_protected() ? 15 : 30;
    int derived = risk_derived(&rc, del_pts);
    int score = cat > derived ? cat : derived;
    if (!reason[0]) {
        static char rbuf[128];
        if (rc.n_del || rc.n_write || rc.n_net || rc.n_ext)
            snprintf(rbuf, sizeof rbuf, "%d delete, %d write, %d network, %d opaque",
                     rc.n_del, rc.n_write, rc.n_net, rc.n_ext);
        else
            snprintf(rbuf, sizeof rbuf, "no mutating steps");
        reason = rbuf;
    }
    if (reason_out) *reason_out = reason;
    return score;
}

int jash_b_explain(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    /* JINV-046: flag desconhecida nao pode virar nome/alvo. */
    /* JINV-046: flag desconhecida nao vira argumento. JINV-048: `--json` e
     * `--risk` sao as unicas que este builtin conhece. */
    { static const char* const ok_flags[] = { "--json", "--risk", 0 };
      if (jash_reject_unknown_flag("explain", args, ok_flags, w, ctx))
          return 2; }

    /* JINV-048 — saida legivel por maquina.
     *
     * O texto do `explain` ja diz tudo o que um guard precisa: o efeito de
     * cada passo e o raio de impacto. Mas um agente que le TEXTO volta a
     * adivinhar — foi o que motivou o JINV-047. Aqui a mesma informacao sai
     * estruturada, dos MESMOS contadores que produzem o texto: nao ha segunda
     * fonte de verdade, entao os dois nao podem divergir. */
    int json_mode = 0, risk_mode = 0;
    { const char* a = args;
      while (a) {
          while (*a == ' ' || *a == '\t') a++;
          if (strncmp(a, "--json", 6) == 0 &&
              (a[6] == '\0' || a[6] == ' ' || a[6] == '\t')) {
              json_mode = 1; a += 6;
          } else if (strncmp(a, "--risk", 6) == 0 &&
                     (a[6] == '\0' || a[6] == ' ' || a[6] == '\t')) {
              risk_mode = 1; a += 6;
          } else break;
      }
      args = a; }
    (void)s;
    if (!args || !*args) {
        /* W6.3: with --json/--risk, empty input is a stable empty envelope
         * (steps:[], risk safe "no steps"), not a usage error. */
        if (!json_mode && !risk_mode) {
            jash_writes(jash_stderr_writer, 0, "explain: usage: explain <command line>\r\n");
            return 2;
        }
        args = "";
    }
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", args);
    /* --risk needs the pristine line: segmentation destroys buf in place. */
    char raw[4096];
    snprintf(raw, sizeof raw, "%s", args);

    /* split into segments on top-level ; && || |  (quote-aware) */
    char* segs[32]; int nseg = 0;
    int sq = 0, dq = 0;
    char* start = buf;
    for (char* p = buf; ; p++) {
        char c = *p;
        if (c == '\'' && !dq) sq = !sq;
        else if (c == '"' && !sq) dq = !dq;
        int brk = 0, skip = 0;
        if (!sq && !dq) {
            if (c == ';') { brk = 1; skip = 0; }
            else if (c == '|') { brk = 1; skip = (p[1] == '|'); }
            else if (c == '&' && p[1] == '&') { brk = 1; skip = 1; }
        }
        if (brk || c == '\0') {
            *p = '\0';
            char* t = start; while (*t == ' ' || *t == '\t') t++;
            if (*t && nseg < 32) segs[nseg++] = t;
            if (c == '\0') break;
            if (skip) p++;          /* consume the 2nd char of && or || */
            start = p + 1;
        }
    }
    if (nseg == 0) {
        if (json_mode) {
            jash_writes(w, ctx, "{\"steps\":[],\"blast\":{\"mutations\":0,"
                                "\"delete\":0,\"write\":0,\"network\":0,"
                                "\"opaque\":0}");
            if (risk_mode)
                jash_writes(w, ctx, ",\"risk\":{\"score\":0,\"band\":\"safe\","
                                    "\"reason\":\"no steps\"}");
            jash_writes(w, ctx, "}\n");
        }
        else jash_writes(w, ctx, "explain: nothing to analyze\r\n");
        return 0;
    }

    int n_del = 0, n_write = 0, n_ext = 0, n_net = 0, n_mut = 0;
    int protected_rm = jash_trash_protected();
    risk_counters_t rc; memset(&rc, 0, sizeof rc);

    if (json_mode) jash_writes(w, ctx, "{\"steps\":[");
    else jash_writef(w, ctx, "\033[1;36m═══ explain — %d step(s), not executed ═══\033[0m\r\n", nseg);
    for (int i = 0; i < nseg; i++) {
        char verb[64]; unsigned vi = 0;
        const char* q = segs[i];
        while (*q == ' ') q++;
        /* skip leading VAR=val assignments to find the real verb */
        while (1) {
            const char* eq = q; int looks = 0;
            for (const char* r = q; *r && *r != ' '; r++) { if (*r == '=') { looks = 1; break; } }
            if (!looks) break;
            (void)eq; while (*q && *q != ' ') q++; while (*q == ' ') q++;
        }
        while (*q && *q != ' ' && vi < sizeof(verb) - 1) verb[vi++] = *q++;
        verb[vi] = '\0';
        if (!vi) continue;

        const char* tag = "";
        int eff = classify(verb, &tag);

        char redir[512]; int append = 0;
        int has_redir = find_redirect(segs[i], redir, sizeof redir, &append);

        if (!json_mode)
            jash_writef(w, ctx, "  \033[1m%d.\033[0m %s\r\n", i + 1, segs[i]);

        const char* j_eff = "read"; int j_recov = 0; const char* j_how = "";
        if (eff == EFFECT_DELETE) {
            n_del++; n_mut++;
            j_eff = "delete"; j_recov = protected_rm;
            j_how = protected_rm ? "trash on -> undo/rewind" : "permanent (trash off)";
            if (!json_mode)
            jash_writef(w, ctx, "       → \033[1;31mDELETE\033[0m %s\r\n",
                        protected_rm ? "(recoverable: trash on → undo/rewind)"
                                     : "\033[1;31mPERMANENT\033[0m (trash off)");
        } else if (eff == EFFECT_TRASH) { n_mut++; j_eff = "trash"; j_recov = 1; j_how = "recoverable 48h";
            if (!json_mode) jash_writef(w, ctx, "       → trash (recoverable 48h)\r\n"); }
        else if (eff == EFFECT_MOVE)  { n_mut++; j_eff = "move"; j_recov = 1; j_how = "undoable";
            if (!json_mode) jash_writes(w, ctx, "       → \033[33mMOVE\033[0m (undoable)\r\n"); }
        else if (eff == EFFECT_COPY)  { n_mut++; j_eff = "copy"; j_how = "may overwrite dest";
            if (!json_mode) jash_writes(w, ctx, "       → \033[33mCOPY\033[0m (may overwrite dest)\r\n"); }
        else if (eff == EFFECT_CREATE){ n_mut++; j_eff = "create";
            if (!json_mode) jash_writes(w, ctx, "       → CREATE\r\n"); }
        else if (eff == EFFECT_CHMOD) { n_mut++; j_eff = "chmod"; j_how = "metadata";
            if (!json_mode) jash_writes(w, ctx, "       → CHMOD/LINK (metadata)\r\n"); }
        else if (eff == EFFECT_NET)   { n_net++; j_eff = "network"; j_how = "may exfiltrate/fetch";
            if (!json_mode) jash_writes(w, ctx, "       → \033[35mNETWORK\033[0m (may exfiltrate/fetch)\r\n"); }
        else if (eff == EFFECT_EXTERNAL){ n_ext++; j_eff = "opaque"; j_how = tag;
            if (!json_mode) jash_writef(w, ctx, "       → \033[90m%s\033[0m\r\n", tag); }
        else if (!json_mode) jash_writes(w, ctx, "       → read-only\r\n");

        if (has_redir) {
            n_write++; n_mut++;
            if (!json_mode)
            jash_writef(w, ctx, "       → \033[33m%s\033[0m %s%s\r\n",
                        append ? "APPEND" : "WRITE", redir,
                        append ? "" : " (overwrites if it exists)");
        }

        risk_count_seg(&rc, verb, eff, has_redir, redir, append);

        if (json_mode) {
            char esc_cmd[1024], esc_how[256], esc_red[600];
            jash_json_esc(segs[i], esc_cmd, sizeof esc_cmd);
            jash_json_esc(j_how,   esc_how, sizeof esc_how);
            jash_json_esc(has_redir ? redir : "", esc_red, sizeof esc_red);
            jash_writef(w, ctx,
                "%s{\"cmd\":\"%s\",\"effect\":\"%s\",\"recoverable\":%s,"
                "\"how\":\"%s\",\"writes\":%s,\"target\":\"%s\"}",
                i ? "," : "", esc_cmd, j_eff, j_recov ? "true" : "false",
                esc_how, has_redir ? "true" : "false", esc_red);
        }
    }

    /* --risk: score from the SAME counters + segment-aware catastrophes. */
    const char* reason = "";
    int score = 0;
    if (risk_mode) {
        int cat = risk_catastrophic(segs, nseg, raw, &reason);
        int del_pts = protected_rm ? 15 : 30;   /* trash on = recoverable */
        int derived = risk_derived(&rc, del_pts);
        score = cat > derived ? cat : derived;
        if (!reason[0]) {
            static char rbuf[128];
            if (n_mut || n_net || n_ext) {
                snprintf(rbuf, sizeof rbuf,
                         "%d delete, %d write, %d network, %d opaque",
                         n_del, n_write, n_net, n_ext);
            } else {
                snprintf(rbuf, sizeof rbuf, "no mutating steps");
            }
            reason = rbuf;
        }
    }

    if (json_mode) {
        jash_writef(w, ctx,
            "],\"blast\":{\"mutations\":%d,\"delete\":%d,\"write\":%d,"
            "\"network\":%d,\"opaque\":%d}",
            n_mut, n_del, n_write, n_net, n_ext);
        if (risk_mode) {
            char esc_r[300];
            jash_json_esc(reason, esc_r, sizeof esc_r);
            jash_writef(w, ctx,
                ",\"risk\":{\"score\":%d,\"band\":\"%s\",\"reason\":\"%s\"}",
                score, risk_band(score), esc_r);
        }
        jash_writes(w, ctx, "}\n");
        if (!risk_mode) return 0;
        return score >= 80 ? 124 : score >= 50 ? 1 : 0;
    }
    jash_writef(w, ctx,
        "\033[90mblast radius: %d mutation(s) — %d delete, %d write, %d network; %d external/opaque\033[0m\r\n",
        n_mut, n_del, n_write, n_net, n_ext);
    if (n_ext)
        jash_writes(w, ctx,
            "\033[90mtip: external steps are opaque — `checkpoint` first so `rewind` can undo them.\033[0m\r\n");
    if (risk_mode) {
        const char* col = score >= 80 ? "\033[1;31m" :
                          score >= 50 ? "\033[31m" :
                          score >= 20 ? "\033[33m" : "\033[32m";
        jash_writef(w, ctx, "%s━━ risk: %d%% %s\033[0m — %s\r\n",
                    col, score, risk_band(score), reason);
        return score >= 80 ? 124 : score >= 50 ? 1 : 0;
    }
    return 0;
}

#include "explain_native.inc"
