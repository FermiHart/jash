// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/distill — session → executable recipe (ADR-0042 F10).
//
// The agent solves a multi-step problem and re-derives it next session. handoff
// distills prose; learn stores passive patterns. Neither is re-runnable. distill
// mines the attested chain into an EXECUTABLE recipe, keeping only the load-
// bearing steps (mutations, fixes, assertions) and dropping pure reads:
//
//   $ distill crossbuild --param MODULE=sigil
//   [distill] 6 steps kept (2 fixes, 3 mutations, 1 assertion) · 14 reads dropped
//   $ recipe run crossbuild MODULE=undo
//
// Recipe file ~/.jash/recipes/<name>.jashrc: header + typed steps ('C' command,
// 'A' assertion). `recipe run` checkpoints first, evals sequentially, aborts on
// a failed assertion with the rewind handle. Honest v1: range is --from/--to or
// the last N chain entries (goal-window integration deferred); keep-heuristics
// are documented, inspectable via `distill --explain`. F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/chain_read.h>
#include <core/timeline.h>
#include <parser/eval.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include "recipe_store.h"

#define DIST_MAX_CHAIN 256
#define DIST_MAX_STEPS 256
#define DIST_MAX_PARAMS 16
#define DIST_FIX_SIM   0.5
#define DIST_DEFAULT_RANGE 50

static int recipes_dir(char* b, size_t cap) {
    const char* h = getenv("HOME"); if (!h || !*h) return 0;
    char d[600]; snprintf(d, sizeof d, "%s/.jash", h);          mkdir(d, 0700);
    snprintf(d, sizeof d, "%s/.jash/recipes", h);               mkdir(d, 0700);
    snprintf(b, cap, "%s/.jash/recipes", h);
    return 1;
}
static int recipe_path(const char* name, char* b, size_t cap) {
    char d[600]; if (!recipes_dir(d, sizeof d)) return 0;
    char safe[128]; size_t o = 0;
    for (const char* p = name; *p && o + 1 < sizeof safe; p++) {
        char c = *p;
        if ((c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='-'||c=='_'||c=='.') safe[o++]=c;
    }
    safe[o] = 0; if (!o) return 0;
    snprintf(b, cap, "%s/%s.jashrc", d, safe);
    return 1;
}

static const char* opt_val(char* a[], int n, const char* flag) {
    for (int i = 0; i < n - 1; i++) if (!strcmp(a[i], flag)) return a[i + 1];
    return NULL;
}
static int has_flag(char* a[], int n, const char* flag) {
    for (int i = 0; i < n; i++) if (!strcmp(a[i], flag)) return 1;
    return 0;
}
static const char* first_operand(char* a[], int n) {
    for (int i = 0; i < n; i++) { if (a[i][0]=='-') { i++; continue; } return a[i]; }
    return NULL;
}

/* Proper tokenizer: uses jash_chain_tokenize_nul to handle quotes correctly
 * (BUG-152 fix: tokens must be NUL-terminated for jaccard similarity). */
static int dist_tok(const char* cmd, char* buf, size_t bufcap, char** argv, int max) {
    return jash_chain_tokenize_nul(cmd, buf, bufcap, argv, max);
}

static int is_assertion(const char* cmd) {
    while (*cmd==' '||*cmd=='\t') cmd++;
    return strncmp(cmd,"expect ",7)==0 || strncmp(cmd,"contract ",9)==0 ||
           strncmp(cmd,"goal verify",11)==0 || strncmp(cmd,"assert ",7)==0;
}

/* replace every occurrence of `lit` in src with ${NAME}, into out */
static int subst_lit(const char* src, const char* lit, const char* name, char* out, size_t cap) {
    char replacement[68];
    int n = snprintf(replacement, sizeof(replacement), "${%s}", name);
    if (n < 0 || (size_t)n >= sizeof(replacement)) return -1;
    return recipe_replace(src, lit, replacement, out, cap);
}

#include "distill_semantic.inc"
#include "recipe_semantic.inc"

// ── distill ──────────────────────────────────────────────────────────────────

int jash_b_distill(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "distill", "jash.distill.v1", args, distill_semantic);
    if (structured >= 0) return structured;
    char buf[4096]; snprintf(buf, sizeof buf, "%s", args ? args : "");
    char* a[64]; int n = 0;
    for (char* t = strtok(buf, " \t"); t && n < 64; t = strtok(NULL, " \t")) a[n++] = t;
    const char* name = first_operand(a, n);
    if (!name) { jash_writes(jash_stderr_writer, 0, "distill: need a recipe name\r\n"); return 2; }

    int keep_reads = has_flag(a, n, "--keep-reads");
    int explain    = has_flag(a, n, "--explain");
    long from = -1, to = -1;
    const char* fs = opt_val(a, n, "--from"); if (fs) from = atol(fs);
    const char* ts = opt_val(a, n, "--to");   if (ts) to   = atol(ts);
    /* collect --param NAME=literal (repeatable) */
    char pname[DIST_MAX_PARAMS][64]; char plit[DIST_MAX_PARAMS][256]; int np = 0;
    for (int i = 0; i < n - 1; i++) {
        if (strcmp(a[i], "--param") != 0) continue;
        const char* kv = a[i + 1]; const char* eq = strchr(kv, '=');
        if (!eq || np >= DIST_MAX_PARAMS) continue;
        size_t kl = (size_t)(eq - kv); if (kl >= 64) kl = 63;
        memcpy(pname[np], kv, kl); pname[np][kl] = 0;
        snprintf(plit[np], sizeof plit[np], "%s", eq + 1);
        np++;
    }

    static jash_chain_entry_t chain[DIST_MAX_CHAIN];
    int nc = jash_chain_tail(chain, DIST_MAX_CHAIN, DIST_MAX_CHAIN);
    /* jash_chain_tail returns NEWEST-first; reverse to chronological so exec
     * windows (ts[i]→ts[i+1]) and "prior failure" ordering are correct and the
     * recipe steps are written oldest→newest. */
    for (int x = 0, y = nc - 1; x < y; x++, y--) {
        jash_chain_entry_t tmp = chain[x]; chain[x] = chain[y]; chain[y] = tmp;
    }
    /* select range (chronological) */
    int lo = 0, hi = nc;
    if (from >= 0 || to >= 0) {
        lo = nc; hi = 0;
        for (int i = 0; i < nc; i++) {
            long sq = (long)chain[i].seq;
            if ((from < 0 || sq >= from) && (to < 0 || sq <= to)) { if (i < lo) lo = i; if (i + 1 > hi) hi = i + 1; }
        }
        if (lo > hi) { lo = 0; hi = 0; }
    } else if (nc > DIST_DEFAULT_RANGE) {
        lo = nc - DIST_DEFAULT_RANGE;
    }
    int rn = hi - lo;
    if (rn <= 0) { jash_writes(jash_stderr_writer, 0, "distill: empty chain range — nothing to distill\r\n"); return 2; }

    /* timeline mutation timestamps (for the exec-window correlation) */
    long muts[512]; int nm = 0;
    for (int i = 0; nm < 512; i++) {
        int k; const char *ta=0,*tb=0; long when=0;
        if (!jash_timeline_peek2(i, &k, &ta, &tb, &when)) break;
        if (k==JASH_UNDO_FILE_OVERWRITE||k==JASH_UNDO_FILE_CREATE||k==JASH_UNDO_TRASH||k==JASH_UNDO_MOVE)
            muts[nm++] = when;
    }

    char stepcmd[DIST_MAX_STEPS][1024]; char steptype[DIST_MAX_STEPS]; int ns = 0;
    int n_fix = 0, n_mut = 0, n_assert = 0, n_drop = 0;

    for (int i = lo; i < hi && ns < DIST_MAX_STEPS; i++) {
        const char* cmd = chain[i].cmd;
        if (!cmd[0]) continue;
        /* skip distill/recipe's own bookkeeping commands */
        if (strncmp(cmd,"distill",7)==0 || strncmp(cmd,"recipe",6)==0) continue;

        long ts0 = (long)chain[i].ts_sec;
        long ts1 = (i + 1 < nc) ? (long)chain[i + 1].ts_sec : ts0 + 3600;
        int mut = 0;
        for (int m = 0; m < nm; m++) if (muts[m] >= ts0 && muts[m] < ts1) { mut = 1; break; }
        int assertion = is_assertion(cmd);
        int fix = 0;
        if (chain[i].rc == 0) {
            char ba[1024]; char* ai[64]; int nai = dist_tok(cmd, ba, sizeof ba, ai, 64);
            if (nai < 0) { jash_writes(jash_stderr_writer, NULL, "distill: complete command exceeds tokenizer limits\n"); return 75; }
            for (int j = lo; j < i; j++) {
                if (chain[j].rc == 0) continue;
                char bb[1024]; char* aj[64]; int naj = dist_tok(chain[j].cmd, bb, sizeof bb, aj, 64);
                if (naj < 0) { jash_writes(jash_stderr_writer, NULL, "distill: complete command exceeds tokenizer limits\n"); return 75; }
                if (jash_chain_jaccard(ai, nai, aj, naj) >= DIST_FIX_SIM) { fix = 1; break; }
            }
        }
        const char* reason;
        int keep = 1;
        if (assertion)   { reason = "assertion"; n_assert++; }
        else if (fix)    { reason = "fix";       n_fix++; }
        else if (mut)    { reason = "mutation";  n_mut++; }
        else if (keep_reads) { reason = "read(kept)"; }
        else             { reason = "read(dropped)"; keep = 0; n_drop++; }

        if (explain)
            jash_writef(w, ctx, "  #%llu %-13s %s\r\n",
                        (unsigned long long)chain[i].seq, reason, cmd);
        if (!keep) continue;

        char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", cmd);
        for (int p = 0; p < np; p++) {
            char out[1024];
            if (subst_lit(tmp, plit[p], pname[p], out, sizeof out)) {
                jash_writes(jash_stderr_writer, NULL, "distill: substituted command exceeds limit\n"); return 75;
            }
            snprintf(tmp, sizeof tmp, "%s", out);
        }
        snprintf(stepcmd[ns], sizeof stepcmd[ns], "%s", tmp);
        steptype[ns] = assertion ? 'A' : 'C';
        ns++;
    }

    if (ns == 0) { jash_writes(jash_stderr_writer, 0, "distill: no load-bearing steps in range (try --keep-reads)\r\n"); return 2; }

    char rp[720]; if (!recipe_path(name, rp, sizeof rp)) return 1;
    FILE* f = fopen(rp, "w");
    if (!f) { jash_writef(jash_stderr_writer, 0, "distill: cannot write %s\r\n", rp); return 1; }
    const char* sid = getenv("JASH_SESSION"); if (!sid) sid = getenv("CLAUDE_CODE_SESSION_ID"); if (!sid) sid = "-";
    (void)fprintf(f, "name %s\ncreated %ld\nsession %s\nrange %llu %llu\n",
                  name, (long)time(NULL), sid,
                  (unsigned long long)chain[lo].seq, (unsigned long long)chain[hi-1].seq);
    for (int p = 0; p < np; p++) (void)fprintf(f, "param %s\n", pname[p]);
    for (int i = 0; i < ns; i++) (void)fprintf(f, "%c %s\n", steptype[i], stepcmd[i]);
    (void)fclose(f);

    jash_writef(w, ctx,
        "[distill] %d step%s kept (%d fix%s, %d mutation%s, %d assertion%s) \xc2\xb7 %d read%s dropped\r\n"
        "  recipe run %s\r\n",
        ns, ns==1?"":"s", n_fix, n_fix==1?"":"es", n_mut, n_mut==1?"":"s",
        n_assert, n_assert==1?"":"s", n_drop, n_drop==1?"":"s", name);
    return 0;
}

// ── recipe ───────────────────────────────────────────────────────────────────

static int recipe_run(jash_state_t* s, jash_writer_t w, void* ctx, char* a[], int n) {
    const char* name = first_operand(a, n);
    if (!name) { jash_writes(jash_stderr_writer, 0, "recipe run: need a recipe name\r\n"); return 2; }
    char rp[720]; if (!recipe_path(name, rp, sizeof rp)) return 1;
    FILE* f = fopen(rp, "r");
    if (!f) { jash_writef(jash_stderr_writer, 0, "recipe: no recipe '%s'\r\n", name); return 2; }

    /* declared params + K=V bindings (only declared params bind) */
    char pn[DIST_MAX_PARAMS][64]; char pv[DIST_MAX_PARAMS][256]; int np = 0;
    char stc[DIST_MAX_STEPS][1024]; char stt[DIST_MAX_STEPS]; int ns = 0;
    char line[1100];
    while (fgets(line, sizeof line, f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (strncmp(line, "param ", 6) == 0 && np < DIST_MAX_PARAMS) {
            snprintf(pn[np], sizeof pn[np], "%s", line + 6); pv[np][0] = 0; np++;
        } else if ((line[0]=='C'||line[0]=='A') && line[1]==' ' && ns < DIST_MAX_STEPS) {
            stt[ns] = line[0]; snprintf(stc[ns], sizeof stc[ns], "%s", line + 2); ns++;
        }
    }
    (void)fclose(f);
    /* bind K=V args to DECLARED params only */
    for (int i = 0; i < n; i++) {
        const char* eq = strchr(a[i], '='); if (!eq || a[i][0]=='-') continue;
        size_t kl = (size_t)(eq - a[i]);
        for (int p = 0; p < np; p++)
            if (strlen(pn[p]) == kl && strncmp(pn[p], a[i], kl) == 0)
                snprintf(pv[p], sizeof pv[p], "%s", eq + 1);
    }

    /* pre-run checkpoint for a clean abort */
    char ckpt[160]; snprintf(ckpt, sizeof ckpt, "recipe-%s", name);
    int checkpoint_status = jash_b_checkpoint(s, w, ctx, ckpt);
    if (checkpoint_status) return checkpoint_status;

    for (int i = 0; i < ns; i++) {
        /* substitute ${NAME} → value for declared params */
        char cmd[1024]; snprintf(cmd, sizeof cmd, "%s", stc[i]);
        for (int p = 0; p < np; p++) {
            char needle[80]; snprintf(needle, sizeof needle, "${%s}", pn[p]);
            char out[1024];
            if (recipe_replace(cmd, needle, pv[p], out, sizeof out)) {
                jash_writes(jash_stderr_writer, NULL, "recipe: substituted command exceeds limit\n"); return 75;
            }
            snprintf(cmd, sizeof cmd, "%s", out);
        }
        int rc = jash_eval_line(s, w, ctx, cmd);
        if (stt[i] == 'A' && rc != 0) {
            jash_writef(jash_stderr_writer, 0,
                "[recipe] assertion failed at step %d: %s (rc=%d)\r\n"
                "  end state preserved \xe2\x80\x94 undo with: rewind %s\r\n",
                i + 1, cmd, rc, ckpt);
            return rc ? rc : 1;
        }
    }
    jash_writef(w, ctx, "[recipe] %s completed \xc2\xb7 %d step%s\r\n", name, ns, ns==1?"":"s");
    return 0;
}

int jash_b_recipe(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = semantic_dispatch(s, w, ctx, "recipe", "jash.recipe.v1", args, recipe_semantic);
    if (structured >= 0) return structured;
    char buf[4096]; snprintf(buf, sizeof buf, "%s", args ? args : "");
    char* a[64]; int n = 0;
    for (char* t = strtok(buf, " \t"); t && n < 64; t = strtok(NULL, " \t")) a[n++] = t;
    if (n == 0) { jash_writes(w, ctx, "recipe: run <name> [K=V…] | list | show <name> | forget <name>\r\n"); return 0; }
    const char* sub = a[0]; char** rest = a + 1; int rn = n - 1;

    if (!strcmp(sub, "run"))  return recipe_run(s, w, ctx, rest, rn);
    if (!strcmp(sub, "list")) {
        char d[600]; if (!recipes_dir(d, sizeof d)) return 1;
        DIR* dir = opendir(d);
        jash_writes(w, ctx, "\033[1;36m═══ RECIPES ═══\033[0m\r\n");
        int count = 0;
        if (dir) {
            struct dirent* e;
            while ((e = readdir(dir))) {
                const char* dot = strrchr(e->d_name, '.');
                if (!dot || strcmp(dot, ".jashrc") != 0) continue;
                char nm[256]; size_t l = (size_t)(dot - e->d_name);
                if (l >= sizeof nm) l = sizeof nm - 1;
                memcpy(nm, e->d_name, l); nm[l] = 0;
                jash_writef(w, ctx, "  %s\r\n", nm);
                count++;
            }
            closedir(dir);
        }
        if (!count) jash_writes(w, ctx, "  (none — `distill <name>`)\r\n");
        return 0;
    }
    if (!strcmp(sub, "show")) {
        const char* name = first_operand(rest, rn);
        if (!name) { jash_writes(jash_stderr_writer, 0, "recipe show: need a name\r\n"); return 2; }
        char rp[720]; if (!recipe_path(name, rp, sizeof rp)) return 1;
        FILE* f = fopen(rp, "r"); if (!f) { jash_writef(jash_stderr_writer, 0, "recipe: no recipe '%s'\r\n", name); return 2; }
        char line[1100]; while (fgets(line, sizeof line, f)) jash_writes(w, ctx, line);
        (void)fclose(f); return 0;
    }
    if (!strcmp(sub, "forget")) {
        const char* name = first_operand(rest, rn);
        if (!name) { jash_writes(jash_stderr_writer, 0, "recipe forget: need a name\r\n"); return 2; }
        char rp[720]; if (!recipe_path(name, rp, sizeof rp)) return 1;
        if (remove(rp) == 0) jash_writef(w, ctx, "recipe: forgot %s\r\n", name);
        else jash_writef(jash_stderr_writer, 0, "recipe: no recipe '%s'\r\n", name);
        return 0;
    }
    jash_writef(jash_stderr_writer, 0, "recipe: unknown subcommand '%s'\r\n", sub);
    return 2;
}

SEMANTIC_ARGV_WRAPPER(distill, "distill", "jash.distill.v1", distill_semantic)
SEMANTIC_ARGV_WRAPPER(recipe, "recipe", "jash.recipe.v1", recipe_semantic)
