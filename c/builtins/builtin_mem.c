// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/mem — session memory distilled from the attested chain.
// F E R M I ∞ H A R T
//
// ADR-0054. The attested chain (ADR-0051) records everything a session
// memory needs — what ran, where, how long, whether it failed, in which
// session — but raw log is not memory. mem distills it:
//
//   distill  — split the chain into sessions (sess= field or >30min gap),
//              emit one CARD per session (span, cmds, fails, files that
//              exist) plus typed OBSERVATIONS inferred deterministically:
//                bugfix    same command failed (rc!=0) then passed (rc=0)
//                discovery session of read-only verbs only
//                change    write-shaped verbs or shell redirect
//              (declared heuristics — pattern, not a model). Persisted to
//              ~/.jash/mem-<repokey>.tsv (flock + tmp/rename, gap/graph
//              discipline). Re-distill with no chain novelty is a no-op:
//              output declares it and nothing is re-attested.
//   timeline — every card chronologically with its observations.
//   show <id> — one card, full detail.
//   search "<q>" [--failed] [--type T] — token scoring over cards and
//              observations; outcome is a first-class filter.
//   brief    — session-start injection: last card, how it ended, top
//              failure, open gaps, mesh tasks, graph presence. Pull, never
//              push (ADR-0054 §4.4).
//   verify <id> — replay every seq a card cites against the chain: seq
//              exists, rc matches, session matches → VERIFIED, else DRIFT
//              with the first divergent seq. Memory with a receipt.
//
// Receipt invariant (ADR-0054 §4.1): a card that cites no chain seqs is
// never written — verifiability is not a feature, it's the entry bar.
// Zero LLM, zero network, zero marginal cost.

#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>
#include <core/structured_output.h>
#include <core/output_budget.h>
#include <core/adopt.h>        /* jash_repo_key — per-repo memory (ADR-0051 W6) */
#include <module/sigil.h>      /* jash_sigil_attest */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <unistd.h>
#include <dirent.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <errno.h>

/* ── types ─────────────────────────────────────────────────────────────────── */

typedef struct {
    long seq, ts;                 /* ts in whole seconds (ns part dropped) */
    int  rc;
    char sess[96];
    char cwd[384];
    char cmd[1200];
} mem_ev_t;

typedef struct {
    char id[24];
    char sess[96];
    long first_ts, last_ts, first_seq, last_seq;
    int  ncmds, fails;
    char files[240];              /* comma-separated, as cited, capped */
} mem_card_t;

typedef struct {
    char cardid[24];
    char type[12];                /* bugfix | discovery | change */
    char detail[280];
    long seq_a, seq_b;
} mem_obs_t;

#define MEM_GAP_SECONDS 1800      /* session split: >30min idle = new session */

/* ── paths + lock (gap/graph discipline) ───────────────────────────────────── */

static int mem_store_path(char *b, size_t cap) {
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(b, cap, "%s/.jash/mem-%s.tsv", h, k);
    else
        snprintf(b, cap, "%s/.jash/mem.tsv", h);
    return 1;
}

static int mem_chain_path(char *b, size_t cap) {
    const char *h = getenv("HOME");
    if (!h || !*h) return 0;
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(b, cap, "%s/.jash/chain-%s.log", h, k);
    else
        snprintf(b, cap, "%s/.jash/chain.log", h);
    return 1;
}

static int mem_lock(void) {
    const char *h = getenv("HOME");
    if (!h || !*h) return -1;
    char d[600], lp[700];
    snprintf(d, sizeof d, "%s/.jash", h); mkdir(d, 0700);
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(lp, sizeof lp, "%s/.jash/mem-%s.lock", h, k);
    else
        snprintf(lp, sizeof lp, "%s/.jash/mem.lock", h);
    int fd = open(lp, O_CREAT | O_RDWR, 0600);
    if (fd < 0) return -1;
    if (flock(fd, LOCK_EX) != 0) { close(fd); return -1; }
    return fd;
}
static void mem_unlock(int fd) { if (fd >= 0) { flock(fd, LOCK_UN); close(fd); } }

/* ── buffered output with honest --budget footer (graph W5 pattern) ────────── */

static char  mem_obuf[1 << 16];
static size_t mem_olen;
static size_t mem_required;
static int mem_json_mode, mem_output_status, mem_flushed;

static void mem_out(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
static void mem_out(const char *fmt, ...) {
    size_t available = sizeof mem_obuf - mem_olen;
    va_list ap; va_start(ap, fmt);
    int r = vsnprintf(mem_obuf + mem_olen, available, fmt, ap);
    va_end(ap);
    if (r < 0) { mem_output_status = 125; return; }
    mem_required += (size_t)r;
    if (r > 0) mem_olen += (size_t)r < available ? (size_t)r : available - 1;
}

static void mem_flush(jash_writer_t w, void *ctx, long budget) {
    mem_flushed = 1;
    if (mem_json_mode) {
        long global_budget = jash_budget_limit();
        if (global_budget > 0 && (budget <= 0 || global_budget < budget))
            budget = global_budget;
        int rc = jash_structured_document(w, ctx, "mem", mem_obuf,
                                           mem_olen, mem_required, budget, 0);
        if (rc) mem_output_status = rc;
        mem_olen = 0; mem_required = 0; mem_obuf[0] = 0;
        return;
    }

    if (budget > 0 && (long)mem_olen > budget) {
        mem_obuf[budget] = 0;
        jash_writes(w, ctx, mem_obuf);
        jash_writef(w, ctx,
            "[mem] output capped at %ld bytes — refine with --json\n", budget);
    } else {
        jash_writes(w, ctx, mem_obuf);
    }
    mem_olen = 0; mem_required = 0; mem_obuf[0] = 0;
}

/* ── small helpers ─────────────────────────────────────────────────────────── */

static unsigned long long mem_fnv(const char *s) {
    unsigned long long h = 1469598103934665603ULL;
    while (*s) { h ^= (unsigned char)*s++; h *= 1099511628211ULL; }
    return h;
}

/* split on '\t' PRESERVING empty fields (strtok collapses them) */
static int mem_split(char *line, char *fld[], int max) {
    int nf = 0;
    char *s = line;
    while (nf < max && s) {
        fld[nf++] = s;
        char *t = strchr(s, '\t');
        s = t ? (*t = 0, t + 1) : NULL;
    }
    return nf;
}

static const char *mem_opt(char *a[], int n, const char *flag) {
    for (int i = 0; i < n - 1; i++) if (!strcmp(a[i], flag)) return a[i + 1];
    return NULL;
}
static int mem_has(char *a[], int n, const char *flag) {
    for (int i = 0; i < n; i++) if (!strcmp(a[i], flag)) return 1;
    return 0;
}
static long mem_budget(char *a[], int n) {
    const char *v = mem_opt(a, n, "--budget");
    return v ? atol(v) : 0;
}
/* which-th positional arg, skipping flags and the values of --budget/--type */
static const char *mem_pos(char *a[], int n, int which) {
    int found = 0;
    for (int i = 0; i < n; i++) {
        if (!strcmp(a[i], "--budget") || !strcmp(a[i], "--type")) { i++; continue; }
        if (a[i][0] == '-') continue;
        if (found++ == which) return a[i];
    }
    return NULL;
}

/* ── chain parsing ─────────────────────────────────────────────────────────── */

/* chain line: <seq> <ts_sec>.<ts_ns> <hash64> tag=<mac> rc=<rc> cwd=<cwd>
 * repo=<key> dur=<ms> sess=<sess> cmd=<free text>. cmd= is always LAST. */
static int mem_parse_line(const char *line, mem_ev_t *ev) {
    const char *cm = strstr(line, " cmd=");
    if (!cm) return 0;
    memset(ev, 0, sizeof *ev);
    ev->seq = atol(line);
    const char *sp = strchr(line, ' ');
    if (sp) ev->ts = (long)strtod(sp + 1, NULL);
    const char *r = strstr(line, " rc=");
    ev->rc = r ? atoi(r + 4) : 0;
    const char *cw = strstr(line, " cwd=");
    if (cw) {
        const char *e = strstr(cw + 5, " repo=");
        size_t l = e ? (size_t)(e - (cw + 5)) : strlen(cw + 5);
        if (l >= sizeof ev->cwd) l = sizeof ev->cwd - 1;
        memcpy(ev->cwd, cw + 5, l); ev->cwd[l] = 0;
    }
    const char *ss = strstr(line, " sess=");
    if (ss) {
        const char *p = ss + 6;
        size_t l = 0;
        while (p[l] && p[l] != ' ' && p[l] != '\n' && l < sizeof ev->sess - 1) l++;
        memcpy(ev->sess, p, l); ev->sess[l] = 0;
    }
    snprintf(ev->cmd, sizeof ev->cmd, "%s", cm + 5);
    char *nl = strchr(ev->cmd, '\n'); if (nl) *nl = 0;
    return 1;
}

static int mem_internal_command(const char* command) {
    return !strncmp(command, "mem", 3) &&
        (command[3] == ' ' || command[3] == '\t' || command[3] == '-' ||
         command[3] == '\n' || command[3] == 0);
}

static mem_ev_t *mem_chain_load(int *out_n) {
    char cp[700];
    if (!mem_chain_path(cp, sizeof cp)) return NULL;
    FILE *f = fopen(cp, "r");
    if (!f) return NULL;
    int cap = 256, n = 0;
    mem_ev_t *evs = malloc((size_t)cap * sizeof *evs);
    if (!evs) { fclose(f); return NULL; }
    char line[4096];
    while (fgets(line, sizeof line, f)) {
        const char *cm = strstr(line, " cmd=");
        if (!cm) continue;
        /* the mem builtin itself is not work memory: running distill/timeline/
         * brief writes chain lines that would otherwise be re-distilled into
         * new "sessions" and break idempotency — declared skip. Covers the
         * bare verb, its subcommands and its own mem-distill attests. */
        const char *c5 = cm + 5;
        if (mem_internal_command(c5)) continue;
        if (n == cap) {
            cap *= 2;
            mem_ev_t *ne = realloc(evs, (size_t)cap * sizeof *evs);
            if (!ne) { free(evs); fclose(f); return NULL; }
            evs = ne;
        }
        mem_parse_line(line, &evs[n]);
        n++;
    }
    fclose(f);
    *out_n = n;
    return evs;
}

/* ── declared heuristics (ADR-0054 W2 — pattern, not a model) ──────────────── */

static const char *MEM_READ_VERBS[] = {
    "cat","ls","grep","rg","head","tail","less","more","pwd","find","wc",
    "file","stat","strings","nm","echo","printf","awk","sort","uniq","diff",
    "readlink","realpath","basename","dirname","jq","cd","which","type",
    "history","env","printenv","df","du","ps","man","objdump","otool",
    "sed","git", NULL   /* sed/git refined below */
};
static const char *MEM_GIT_READ[] = {
    "status","log","diff","show","branch","blame","describe","rev-parse",
    "ls-files","ls-tree","remote","tag","fetch","grep", NULL
};
static const char *MEM_WRITE_VERBS[] = {
    "cp","mv","rm","mkdir","rmdir","touch","chmod","chown","ln","tee",
    "truncate","dd","install","patch","rsync", NULL
};

/* copy one token skipping quotes; ri points at the read cursor */
static void mem_tok(const char **ri, char *out, size_t cap) {
    const char *p = *ri;
    while (*p == ' ' || *p == '\t') p++;
    size_t l = 0;
    while (*p && *p != ' ' && *p != '\t') {
        char c = *p++;
        if (c == '"' || c == '\'') continue;
        if (l < cap - 1) out[l++] = c;
    }
    out[l] = 0;
    *ri = p;
}

/* first whitespace token of a command, without quotes */
static void mem_verb(const char *cmd, char *out, size_t cap) {
    mem_tok(&cmd, out, cap);
    /* basename: /usr/bin/grep counts as grep */
    char *sl = strrchr(out, '/');
    if (sl && sl[1]) memmove(out, sl + 1, strlen(sl));
}

/* second token (first argument), "" when absent */
static void mem_arg1(const char *cmd, char *out, size_t cap) {
    char skip[64];
    mem_tok(&cmd, skip, sizeof skip);
    mem_tok(&cmd, out, cap);
}

static int mem_in_list(const char *v, const char *list[]) {
    for (int i = 0; list[i]; i++) if (!strcmp(v, list[i])) return 1;
    return 0;
}

/* write-shaped: write verb, sed -i, git with a mutating subcommand, or a
 * redirect to a file ('>' NOT followed by '&' — 2>&1 is not a write) */
static int mem_is_write(const char *cmd) {
    char v[64]; mem_verb(cmd, v, sizeof v);
    if (mem_in_list(v, MEM_WRITE_VERBS)) return 1;
    if (!strcmp(v, "sed") && strstr(cmd, "-i")) return 1;
    if (!strcmp(v, "git")) {
        char sub[64]; mem_arg1(cmd, sub, sizeof sub);
        if (!mem_in_list(sub, MEM_GIT_READ)) return 1;
    }
    char q = 0;
    for (const char *p = cmd; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '"' || *p == '\'') { q = *p; continue; }
        if (*p == '>') {
            const char *e = p + 1;
            if (*e == '>') e++;
            while (*e == ' ') e++;
            if (*e != '&') return 1;
        }
    }
    return 0;
}

static int mem_is_read(const char *cmd) {
    if (mem_is_write(cmd)) return 0;
    char v[64]; mem_verb(cmd, v, sizeof v);
    if (!strcmp(v, "sed")) return !strstr(cmd, "-i");
    if (!strcmp(v, "git")) {
        char sub[64]; mem_arg1(cmd, sub, sizeof sub);
        return mem_in_list(sub, MEM_GIT_READ);
    }
    return mem_in_list(v, MEM_READ_VERBS);
}

/* command signature for bugfix matching: verb + first non-flag argument
 * (declared: "same command" = same verb acting on the same target) */
static void mem_sig(const char *cmd, char *out, size_t cap) {
    char v[64], arg[256];
    mem_verb(cmd, v, sizeof v);
    mem_arg1(cmd, arg, sizeof arg);
    if (arg[0] && arg[0] != '-') snprintf(out, cap, "%s %s", v, arg);
    else                          snprintf(out, cap, "%s", v);
}

/* sanitize free text for one TSV field (no tabs/newlines) */
static void mem_clean(char *s) {
    for (; *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') *s = ' ';
}

/* ── distill ───────────────────────────────────────────────────────────────── */

static void mem_card_id(const char *sess, long first_seq, char *out, size_t cap) {
    const char *k = jash_repo_key();
    char seed[256];
    snprintf(seed, sizeof seed, "%s\x01%s\x01%ld",
             sess, (k && k[0] != '-') ? k : "-", first_seq);
    snprintf(out, cap, "%016llx", mem_fnv(seed));
}

/* collect cited files that exist (resolved against the line's cwd=), unique
 * per session, as-written, bounded — receipt, not inventory */
static void mem_note_file(char *files, size_t cap, const char *tok,
                          const char *cwd) {
    if (!strchr(tok, '/') && !strchr(tok, '.')) return;
    if (tok[0] == '-') return;
    char path[768];
    if (tok[0] == '/') snprintf(path, sizeof path, "%s", tok);
    else if (cwd[0])   snprintf(path, sizeof path, "%s/%s", cwd, tok);
    else               snprintf(path, sizeof path, "%s", tok);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return;
    /* unique: substring match on comma boundaries */
    char needle[520];
    snprintf(needle, sizeof needle, "%s", tok);
    const char *p = files;
    size_t nl = strlen(needle);
    while ((p = strstr(p, needle))) {
        int left  = (p == files) || (p[-1] == ',');
        int right = (p[nl] == 0) || (p[nl] == ',');
        if (left && right) return;
        p += nl;
    }
    size_t cur = strlen(files);
    if (cur + nl + 2 >= cap) return;   /* bounded — declared truncation */
    if (cur) files[cur++] = ',';
    strlcpy(files + cur, needle, cap - cur);
}

/* The same deterministic inference engine serves human and native reports.
 * Collection limits fail explicitly; a partial inference set is never saved. */
typedef struct {
    mem_card_t* cards; mem_obs_t* observations;
    int card_count, observation_count, bugfix, discovery, change;
} mem_distillation_t;
static int mem_distill_collect(const mem_ev_t* evs, int klines, mem_distillation_t* result) {
    mem_card_t *cards = malloc(256 * sizeof *cards);
    mem_obs_t  *obs   = malloc(1024 * sizeof *obs);
    if (!cards || !obs) { free(cards); free(obs); errno = ENOMEM; return -1; }
    int ncap = 256, ocap = 1024, nc = 0, no = 0;
    int nbug = 0, ndisc = 0, nchg = 0;

    int i = 0;
    while (i < klines) {
        /* session = same sess= AND gap <= 30min (ADR-0054 W1) */
        int j = i + 1;
        while (j < klines && !strcmp(evs[j].sess, evs[j - 1].sess)
               && evs[j].ts - evs[j - 1].ts <= MEM_GAP_SECONDS) j++;
        int fails = 0;
        char files[240]; files[0] = 0;
        int all_read = 1, any_write = 0;
        for (int e = i; e < j; e++) {
            if (evs[e].rc != 0) fails++;
            if (mem_is_write(evs[e].cmd)) { any_write = 1; all_read = 0; }
            else if (!mem_is_read(evs[e].cmd)) all_read = 0;
            char ctok[520];
            const char *p = evs[e].cmd;
            while (*p) {
                while (*p == ' ' || *p == '\t' || *p == '"' || *p == '\'') p++;
                int tl = 0;
                while (p[tl] && p[tl] != ' ' && p[tl] != '\t'
                       && p[tl] != '"' && p[tl] != '\'' && tl < 500) tl++;
                if (tl > 0) {
                    memcpy(ctok, p, (size_t)tl); ctok[tl] = 0;
                    mem_note_file(files, sizeof files, ctok, evs[e].cwd);
                }
                p += tl;
            }
        }
        if (nc == 4096) { errno = E2BIG; goto fail; }
        if (nc == ncap) {
            ncap *= 2;
            mem_card_t *nc2 = realloc(cards, (size_t)ncap * sizeof *cards);
            if (!nc2) { errno = ENOMEM; goto fail; }
            cards = nc2;
        }
        mem_card_t *c = &cards[nc++];
        memset(c, 0, sizeof *c);
        mem_card_id(evs[i].sess, evs[i].seq, c->id, sizeof c->id);
        snprintf(c->sess, sizeof c->sess, "%s", evs[i].sess);
        c->first_ts  = evs[i].ts;     c->last_ts  = evs[j - 1].ts;
        c->first_seq = evs[i].seq;    c->last_seq = evs[j - 1].seq;
        c->ncmds     = j - i;         c->fails    = fails;
        snprintf(c->files, sizeof c->files, "%s", files);

        /* observations — receipt invariant: every one cites seqs */
        char seen_sig[32][300]; int nsig = 0;   /* one bugfix per signature */
        for (int e = i; e < j; e++) {
            if (evs[e].rc == 0) continue;
            char sig[300]; mem_sig(evs[e].cmd, sig, sizeof sig);
            int dup = 0;
            for (int t = 0; t < nsig; t++) if (!strcmp(seen_sig[t], sig)) { dup = 1; break; }
            if (dup) continue;
            for (int g = e + 1; g < j; g++) {
                if (evs[g].rc != 0) continue;
                char sg[300]; mem_sig(evs[g].cmd, sg, sizeof sg);
                if (strcmp(sg, sig)) continue;
                if (no == ocap) { errno = E2BIG; goto fail; }
                mem_obs_t *o = &obs[no++];
                memset(o, 0, sizeof *o);
                snprintf(o->cardid, sizeof o->cardid, "%s", c->id);
                snprintf(o->type, sizeof o->type, "bugfix");
                char trunc[160];
                snprintf(trunc, sizeof trunc, "%s", evs[e].cmd);
                mem_clean(trunc);
                snprintf(o->detail, sizeof o->detail,
                         "'%s' failed (rc=%d) then succeeded", trunc, evs[e].rc);
                o->seq_a = evs[e].seq; o->seq_b = evs[g].seq;
                nbug++;
                if (nsig < 32) snprintf(seen_sig[nsig++], 300, "%s", sig);
                break;
            }
        }
        if (no == ocap && (any_write || (all_read && c->ncmds > 0))) { errno = E2BIG; goto fail; }
        if (any_write) {
            char ex[160] = "";
            for (int e = i; e < j; e++)
                if (mem_is_write(evs[e].cmd)) {
                    snprintf(ex, sizeof ex, "%s", evs[e].cmd);
                    mem_clean(ex);
                    break;
                }
            mem_obs_t *o = &obs[no++];
            memset(o, 0, sizeof *o);
            snprintf(o->cardid, sizeof o->cardid, "%s", c->id);
            snprintf(o->type, sizeof o->type, "change");
            snprintf(o->detail, sizeof o->detail,
                     "write-shaped session (e.g. '%s')", ex);
            o->seq_a = evs[i].seq; o->seq_b = evs[j - 1].seq;
            nchg++;
        } else if (all_read && c->ncmds > 0) {
            mem_obs_t *o = &obs[no++];
            memset(o, 0, sizeof *o);
            snprintf(o->cardid, sizeof o->cardid, "%s", c->id);
            snprintf(o->type, sizeof o->type, "discovery");
            snprintf(o->detail, sizeof o->detail,
                     "read-only session: %d reads%s%s",
                     c->ncmds, files[0] ? " over " : "", files);
            o->seq_a = evs[i].seq; o->seq_b = evs[j - 1].seq;
            ndisc++;
        }
        i = j;
    }
    *result = (mem_distillation_t){cards, obs, nc, no, nbug, ndisc, nchg};
    return 0;
fail:
    free(cards); free(obs); return -1;
}

static int mem_cmd_distill(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = mem_has(a, n, "--json");
    int klines = 0;
    mem_ev_t *evs = mem_chain_load(&klines);
    if (!evs) {
        jash_writes(jash_stderr_writer, 0, "mem: no chain for this repo\n");
        return 1;
    }

    mem_distillation_t result = {0};
    if (mem_distill_collect(evs, klines, &result)) {
        free(evs); jash_writes(jash_stderr_writer, 0, "mem: inference collection limit or allocation failure\n"); return 1;
    }
    free(evs);
    mem_card_t* cards = result.cards; mem_obs_t* obs = result.observations;
    int nc = result.card_count, no = result.observation_count;
    int nbug = result.bugfix, ndisc = result.discovery, nchg = result.change;

    /* serialize and compare — re-distill with no novelty is a DECLARED no-op */
    size_t bcap = (size_t)(nc + no) * 640 + 1024;
    char *blob = malloc(bcap);
    if (!blob) { free(cards); free(obs); return 1; }
    size_t bl = 0;
    for (int e = 0; e < nc; e++) {
        mem_card_t *c = &cards[e];
        bl += (size_t)snprintf(blob + bl, bcap - bl,
            "C\t%s\t%s\t%ld\t%ld\t%ld\t%ld\t%d\t%d\t%s\n",
            c->id, c->sess, c->first_ts, c->last_ts, c->first_seq,
            c->last_seq, c->ncmds, c->fails, c->files);
    }
    for (int e = 0; e < no; e++) {
        mem_obs_t *o = &obs[e];
        bl += (size_t)snprintf(blob + bl, bcap - bl, "O\t%s\t%s\t%s\t%ld\t%ld\n",
                               o->cardid, o->type, o->detail, o->seq_a, o->seq_b);
    }

    int changed = 1;
    char sp[700];
    if (mem_store_path(sp, sizeof sp)) {
        FILE *old = fopen(sp, "r");
        if (old) {
            char *ob = malloc(bcap); size_t ol = 0;
            if (ob) {
                ol = fread(ob, 1, bcap - 1, old);
                if (ol == bl && !memcmp(ob, blob, bl)) changed = 0;
                free(ob);
            }
            fclose(old);
        }
        if (changed) {
            int lk = mem_lock();
            char tmp[720];
            snprintf(tmp, sizeof tmp, "%s.tmp", sp);
            int fd = open(tmp, O_CREAT | O_WRONLY | O_TRUNC, 0600);
            if (fd >= 0) {
                ssize_t wr = write(fd, blob, bl);
                if (wr == (ssize_t)bl) { close(fd); rename(tmp, sp); }
                else { close(fd); unlink(tmp); }
            }
            mem_unlock(lk);
            char det[160];
            snprintf(det, sizeof det,
                     "sessions=%d obs=%d bugfix=%d discovery=%d change=%d",
                     nc, no, nbug, ndisc, nchg);
            jash_sigil_attest("mem-distill", det);
        }
    }
    free(blob);

    if (json)
        mem_out("{\"sessions\":%d,\"observations\":%d,\"bugfix\":%d,"
                "\"discovery\":%d,\"change\":%d,\"chain_lines\":%d,"
                "\"changed\":%s}\n",
                nc, no, nbug, ndisc, nchg, klines,
                changed ? "true" : "false");
    else
        mem_out("distill: %d sessions, %d observations (%d bugfix, %d discovery,"
                " %d change) from %d chain lines%s\n",
                nc, no, nbug, ndisc, nchg, klines,
                changed ? "" : " — no change");
    mem_flush(w, ctx, mem_budget(a, n));
    free(cards); free(obs);
    return 0;
}

/* ── store loading ─────────────────────────────────────────────────────────── */

static int mem_store_load(mem_card_t **out_c, int *out_nc,
                          mem_obs_t **out_o, int *out_no) {
    char sp[700];
    if (!mem_store_path(sp, sizeof sp)) return 0;
    FILE *f = fopen(sp, "r");
    if (!f) return 0;
    int ccap = 64, ocap = 128, nc = 0, no = 0;
    mem_card_t *cards = malloc((size_t)ccap * sizeof *cards);
    mem_obs_t  *obs   = malloc((size_t)ocap * sizeof *obs);
    if (!cards || !obs) { free(cards); free(obs); fclose(f); return 0; }
    char line[2048];
    while (fgets(line, sizeof line, f)) {
        char *nl = strchr(line, '\n'); if (nl) *nl = 0;
        if (line[0] == 'C' && line[1] == '\t') {
            char *fld[10];
            if (mem_split(line + 2, fld, 10) < 9) continue;
            if (nc == ccap) {
                ccap *= 2;
                mem_card_t *t = realloc(cards, (size_t)ccap * sizeof *cards);
                if (!t) break;
                cards = t;
            }
            mem_card_t *c = &cards[nc++];
            memset(c, 0, sizeof *c);
            snprintf(c->id, sizeof c->id, "%s", fld[0]);
            snprintf(c->sess, sizeof c->sess, "%s", fld[1]);
            c->first_ts = atol(fld[2]); c->last_ts = atol(fld[3]);
            c->first_seq = atol(fld[4]); c->last_seq = atol(fld[5]);
            c->ncmds = atoi(fld[6]); c->fails = atoi(fld[7]);
            snprintf(c->files, sizeof c->files, "%s", fld[8]);
        } else if (line[0] == 'O' && line[1] == '\t') {
            char *fld[6];
            if (mem_split(line + 2, fld, 6) < 5) continue;
            if (no == ocap) {
                ocap *= 2;
                mem_obs_t *t = realloc(obs, (size_t)ocap * sizeof *obs);
                if (!t) break;
                obs = t;
            }
            mem_obs_t *o = &obs[no++];
            memset(o, 0, sizeof *o);
            snprintf(o->cardid, sizeof o->cardid, "%s", fld[0]);
            snprintf(o->type, sizeof o->type, "%s", fld[1]);
            snprintf(o->detail, sizeof o->detail, "%s", fld[2]);
            o->seq_a = atol(fld[3]); o->seq_b = atol(fld[4]);
        }
    }
    fclose(f);
    *out_c = cards; *out_nc = nc;
    *out_o = obs;   *out_no = no;
    return 1;
}

static const mem_card_t *mem_find_card(const mem_card_t *cards, int nc,
                                       const char *id) {
    for (int i = 0; i < nc; i++) if (!strcmp(cards[i].id, id)) return &cards[i];
    return NULL;
}

/* ── timeline / show ───────────────────────────────────────────────────────── */

static void mem_print_card(const mem_card_t *c, const mem_obs_t *obs, int no,
                           int json, int last) {
    if (json) {
        char js[160], jf[520];
        jash_json_esc(c->sess, js, sizeof js);
        jash_json_esc(c->files, jf, sizeof jf);
        mem_out("{\"id\":\"%s\",\"sess\":\"%s\",\"first_ts\":%ld,"
                "\"last_ts\":%ld,\"first_seq\":%ld,\"last_seq\":%ld,"
                "\"cmds\":%d,\"fails\":%d,\"files\":\"%s\",\"observations\":[",
                c->id, js, c->first_ts, c->last_ts, c->first_seq, c->last_seq,
                c->ncmds, c->fails, jf);
        int first = 1;
        for (int i = 0; i < no; i++) {
            if (strcmp(obs[i].cardid, c->id)) continue;
            char jd[600];
            jash_json_esc(obs[i].detail, jd, sizeof jd);
            mem_out("%s{\"type\":\"%s\",\"detail\":\"%s\",\"seq_a\":%ld,"
                    "\"seq_b\":%ld}", first ? "" : ",", obs[i].type, jd,
                    obs[i].seq_a, obs[i].seq_b);
            first = 0;
        }
        mem_out("]}%s", last ? "\n" : ",");
        return;
    }
    mem_out("%s  sess=%s  %ld..%ld  cmds=%d fails=%d%s%s\n",
            c->id, c->sess, c->first_ts, c->last_ts, c->ncmds, c->fails,
            c->files[0] ? "  files=" : "", c->files);
    for (int i = 0; i < no; i++) {
        if (strcmp(obs[i].cardid, c->id)) continue;
        mem_out("  %-9s seq %ld→%ld  %s\n",
                obs[i].type, obs[i].seq_a, obs[i].seq_b, obs[i].detail);
    }
}

static int mem_cmd_timeline(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = mem_has(a, n, "--json");
    mem_card_t *cards; mem_obs_t *obs; int nc, no;
    if (!mem_store_load(&cards, &nc, &obs, &no)) {
        jash_writes(jash_stderr_writer, 0,
            "mem: no memory for this repo — run 'mem distill'\n");
        return 1;
    }
    if (json) mem_out("{\"cards\":[");
    if (!nc && !json) mem_out("mem: no cards yet — run 'mem distill'\n");
    for (int i = 0; i < nc; i++)
        mem_print_card(&cards[i], obs, no, json, i == nc - 1);
    if (json) mem_out("]}\n");
    mem_flush(w, ctx, mem_budget(a, n));
    free(cards); free(obs);
    return 0;
}

static int mem_cmd_show(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = mem_has(a, n, "--json");
    const char *id = mem_pos(a, n, 0);
    if (!id) {
        jash_writes(jash_stderr_writer, 0, "mem show: need a card id\n");
        return 2;
    }
    mem_card_t *cards; mem_obs_t *obs; int nc, no;
    if (!mem_store_load(&cards, &nc, &obs, &no)) {
        jash_writes(jash_stderr_writer, 0,
            "mem: no memory for this repo — run 'mem distill'\n");
        return 1;
    }
    const mem_card_t *c = mem_find_card(cards, nc, id);
    if (!c) {
        jash_writef(jash_stderr_writer, 0, "mem: no card '%s'\n", id);
        free(cards); free(obs);
        return 1;
    }
    if (json) mem_out("{\"card\":");
    mem_print_card(c, obs, no, json, 1);
    if (json) mem_out("}\n");
    mem_flush(w, ctx, mem_budget(a, n));
    free(cards); free(obs);
    return 0;
}

/* ── search (W2: outcome is a first-class filter) ──────────────────────────── */

static int mem_token_hit(const char *hay_lower, const char *needle_lower) {
    return strstr(hay_lower, needle_lower) != NULL;
}

static void mem_lower(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (; s[i] && i < cap - 1; i++) out[i] = (char)tolower((unsigned char)s[i]);
    out[i] = 0;
}

/* score = number of DISTINCT query tokens present (substring, case-insens.) */
static int mem_score(const char *text_lower, char *q_lower) {
    int score = 0;
    char *save = NULL;
    for (char *t = strtok_r(q_lower, " \t", &save); t;
         t = strtok_r(NULL, " \t", &save))
        if (mem_token_hit(text_lower, t)) score++;
    return score;
}

static int mem_cmd_search(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = mem_has(a, n, "--json");
    int failed = mem_has(a, n, "--failed");
    const char *type = mem_opt(a, n, "--type");
    const char *q = mem_pos(a, n, 0);
    if (!q) {
        jash_writes(jash_stderr_writer, 0,
            "mem search: need a query — mem search \"<q>\" [--failed] [--type T]\n");
        return 2;
    }
    mem_card_t *cards; mem_obs_t *obs; int nc, no;
    if (!mem_store_load(&cards, &nc, &obs, &no)) {
        jash_writes(jash_stderr_writer, 0,
            "mem: no memory for this repo — run 'mem distill'\n");
        return 1;
    }
    char q_lower[300]; mem_lower(q, q_lower, sizeof q_lower);

    /* (score, kind, index) triples, insertion-sorted desc — deterministic:
     * ties broken by store order (chain order), never by pointer luck */
    int kmax = nc + no;
    int *s_score = malloc((size_t)(kmax ? kmax : 1) * sizeof(int));
    int *s_kind  = malloc((size_t)(kmax ? kmax : 1) * sizeof(int));
    int *s_idx   = malloc((size_t)(kmax ? kmax : 1) * sizeof(int));
    if (!s_score || !s_kind || !s_idx) {
        free(s_score); free(s_kind); free(s_idx);
        free(cards); free(obs);
        return 1;
    }
    int nk = 0;
    for (int i = 0; i < nc; i++) {
        if (failed && cards[i].fails == 0) continue;
        if (type) continue;   /* --type filters observations, not cards */
        char hay[600];
        snprintf(hay, sizeof hay, "%s %s", cards[i].sess, cards[i].files);
        char hl[600]; mem_lower(hay, hl, sizeof hl);
        char qb[300]; snprintf(qb, sizeof qb, "%s", q_lower);
        int sc = mem_score(hl, qb);
        if (sc > 0) { s_score[nk] = sc; s_kind[nk] = 0; s_idx[nk] = i; nk++; }
    }
    for (int i = 0; i < no; i++) {
        if (type && strcmp(obs[i].type, type)) continue;
        if (failed && strcmp(obs[i].type, "bugfix")) continue;
        char hay[700];
        snprintf(hay, sizeof hay, "%s %s", obs[i].type, obs[i].detail);
        char hl[700]; mem_lower(hay, hl, sizeof hl);
        char qb[300]; snprintf(qb, sizeof qb, "%s", q_lower);
        int sc = mem_score(hl, qb);
        if (sc > 0) { s_score[nk] = sc; s_kind[nk] = 1; s_idx[nk] = i; nk++; }
    }
    for (int i = 1; i < nk; i++) {   /* stable insertion sort by score desc */
        int vs = s_score[i], vk = s_kind[i], vx = s_idx[i], j = i - 1;
        while (j >= 0 && s_score[j] < vs) {
            s_score[j + 1] = s_score[j]; s_kind[j + 1] = s_kind[j];
            s_idx[j + 1] = s_idx[j]; j--;
        }
        s_score[j + 1] = vs; s_kind[j + 1] = vk; s_idx[j + 1] = vx;
    }

    if (json) mem_out("[");
    int shown = 0;
    for (int i = 0; i < nk && shown < 20; i++, shown++) {
        if (s_kind[i] == 0) {
            mem_card_t *c = &cards[s_idx[i]];
            if (json) {
                char js[160], jf[520];
                jash_json_esc(c->sess, js, sizeof js);
                jash_json_esc(c->files, jf, sizeof jf);
                mem_out("%s{\"kind\":\"card\",\"id\":\"%s\",\"sess\":\"%s\","
                        "\"fails\":%d,\"files\":\"%s\",\"score\":%d}",
                        shown ? "," : "", c->id, js, c->fails, jf, s_score[i]);
            } else {
                mem_out("card  %s  sess=%s fails=%d%s%s\n",
                        c->id, c->sess, c->fails,
                        c->files[0] ? "  files=" : "", c->files);
            }
        } else {
            mem_obs_t *o = &obs[s_idx[i]];
            if (json) {
                char jd[600];
                jash_json_esc(o->detail, jd, sizeof jd);
                mem_out("%s{\"kind\":\"observation\",\"card\":\"%s\","
                        "\"type\":\"%s\",\"detail\":\"%s\",\"seq_a\":%ld,"
                        "\"seq_b\":%ld,\"score\":%d}",
                        shown ? "," : "", o->cardid, o->type, jd,
                        o->seq_a, o->seq_b, s_score[i]);
            } else {
                mem_out("%-9s %s  seq %ld→%ld  %s\n",
                        o->type, o->cardid, o->seq_a, o->seq_b, o->detail);
            }
        }
    }
    if (json) mem_out("]\n");
    else if (!shown) mem_out("mem: no match for \"%s\"%s%s%s\n", q,
                             failed ? " (--failed)" : "",
                             type ? " --type " : "", type ? type : "");
    mem_flush(w, ctx, mem_budget(a, n));
    free(s_score); free(s_kind); free(s_idx);
    free(cards); free(obs);
    return 0;
}

/* ── brief (W3: session-start injection — pull, never push) ────────────────── */

static int mem_gaps_open(void) {
    const char *home = getenv("HOME"); if (!home || !home[0]) return -1;
    char p[600]; snprintf(p, sizeof p, "%s/.jash/gaps.tsv", home);
    FILE *f = fopen(p, "r"); if (!f) return -1;
    int cnt = 0; char line[1600];
    while (fgets(line, sizeof line, f)) {
        char *t = strchr(line, '\t');
        if (t && !strncmp(t + 1, "open\t", 5)) cnt++;
    }
    fclose(f);
    return cnt;
}

/* mesh board: tasks/<id>.tsv, field 2 = state; "done" is closed, the rest
 * (ready/claimed) is open demand */
static int mem_mesh_tasks_open(void) {
    const char *home = getenv("HOME"); if (!home || !home[0]) return -1;
    char dir[600]; snprintf(dir, sizeof dir, "%s/.jash/mesh/tasks", home);
    DIR *d = opendir(dir); if (!d) return -1;
    int cnt = 0;
    struct dirent *e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char fp[1024]; snprintf(fp, sizeof fp, "%s/%s", dir, e->d_name);
        FILE *f = fopen(fp, "r"); if (!f) continue;
        char line[1024];
        if (fgets(line, sizeof line, f)) {
            char *fld[7];
            if (mem_split(line, fld, 7) >= 2 && strcmp(fld[1], "done")) cnt++;
        }
        fclose(f);
    }
    closedir(d);
    return cnt;
}

static int mem_graph_present(void) {
    const char *home = getenv("HOME"); if (!home || !home[0]) return 0;
    char gp[700];
    const char *k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(gp, sizeof gp, "%s/.jash/graph-%s.tsv", home, k);
    else
        snprintf(gp, sizeof gp, "%s/.jash/graph.tsv", home);
    struct stat st;
    return stat(gp, &st) == 0;
}

static int mem_cmd_brief(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = mem_has(a, n, "--json");
    mem_card_t *cards = NULL; mem_obs_t *obs = NULL; int nc = 0, no = 0;
    if (!mem_store_load(&cards, &nc, &obs, &no) || nc == 0) {
        if (json) {
            mem_out("{\"cards\":0,\"hint\":\"mem distill\"}\n");
            mem_flush(w, ctx, mem_budget(a, n));
        } else {
            jash_writes(w, ctx,
                "mem: no memory yet for this repo — run 'mem distill'\n");
        }
        free(cards); free(obs);
        return 0;
    }
    const mem_card_t *last = &cards[nc - 1];

    /* top failure of the last session, from the chain (outcome-aware) */
    char top_sig[300] = ""; int top_n = 0;
    {
        int klines = 0;
        mem_ev_t *evs = mem_chain_load(&klines);
        if (evs) {
            char sigs[16][300]; int counts[16] = {0}; int ns = 0;
            for (int i = 0; i < klines; i++) {
                if (evs[i].rc == 0) continue;
                if (evs[i].seq < last->first_seq || evs[i].seq > last->last_seq)
                    continue;
                char sig[300]; mem_sig(evs[i].cmd, sig, sizeof sig);
                int t;
                for (t = 0; t < ns; t++)
                    if (!strcmp(sigs[t], sig)) { counts[t]++; break; }
                if (t == ns && ns < 16) {
                    snprintf(sigs[ns], 300, "%s", sig);
                    counts[ns] = 1; ns++;
                }
            }
            for (int t = 0; t < ns; t++)
                if (counts[t] > top_n) {
                    top_n = counts[t];
                    snprintf(top_sig, sizeof top_sig, "%s", sigs[t]);
                }
            free(evs);
        }
    }

    int gaps = mem_gaps_open();
    int tasks = mem_mesh_tasks_open();
    int graph = mem_graph_present();

    if (json) {
        char js[160], jf[520], jt[600];
        jash_json_esc(last->sess, js, sizeof js);
        jash_json_esc(last->files, jf, sizeof jf);
        jash_json_esc(top_sig, jt, sizeof jt);
        mem_out("{\"card\":{\"id\":\"%s\",\"sess\":\"%s\",\"first_ts\":%ld,"
                "\"last_ts\":%ld,\"cmds\":%d,\"fails\":%d,\"files\":\"%s\"},"
                "\"top_failure\":{\"sig\":\"%s\",\"count\":%d},"
                "\"gaps_open\":%d,\"mesh_tasks_open\":%d,\"graph\":%s}\n",
                last->id, js, last->first_ts, last->last_ts, last->ncmds,
                last->fails, jf, jt, top_n, gaps < 0 ? 0 : gaps,
                tasks < 0 ? 0 : tasks, graph ? "true" : "false");
        mem_flush(w, ctx, mem_budget(a, n));
        free(cards); free(obs);
        return 0;
    }

    const char *k = jash_repo_key();
    mem_out("# mem brief — %s\n", (k && k[0] != '-') ? k : "(no repo)");
    mem_out("last session %s (sess=%s)\n", last->id, last->sess);
    mem_out("  span %ld..%ld — %d cmds, %d failed\n",
            last->first_ts, last->last_ts, last->ncmds, last->fails);
    if (last->files[0]) mem_out("  files: %s\n", last->files);
    for (int i = 0; i < no; i++) {
        if (strcmp(obs[i].cardid, last->id)) continue;
        mem_out("  %-9s seq %ld→%ld  %s\n",
                obs[i].type, obs[i].seq_a, obs[i].seq_b, obs[i].detail);
    }
    if (top_n > 0)
        mem_out("top failure: '%s' (x%d)\n", top_sig, top_n);
    if (gaps >= 0)
        mem_out("gaps: %d open%s\n", gaps,
                gaps ? " — `gap list` ranks the demand" : "");
    if (tasks >= 0)
        mem_out("mesh: %d tasks open\n", tasks);
    if (graph)
        mem_out("graph: present — `graph hot` ranks pain from the chain\n");
    mem_out("hint: mem search \"<q>\" — mem verify %s\n", last->id);
    mem_flush(w, ctx, mem_budget(a, n));
    free(cards); free(obs);
    return 0;
}

/* ── verify (W4: memory with a receipt — replay cited seqs against chain) ── */

static const mem_ev_t *mem_find_seq(const mem_ev_t *evs, int n, long seq) {
    for (int i = 0; i < n; i++) if (evs[i].seq == seq) return &evs[i];
    return NULL;
}

typedef struct { int claims; long drift_sequence; char reason[300]; } mem_verification_t;
static mem_verification_t mem_verify_claims(const mem_card_t* c, const mem_obs_t* obs, int no,
                                            const mem_ev_t* evs, int klines) {
    int claims = 0;
    long drift_seq = 0;
    char drift_msg[300] = "";

#define MEM_DRIFT(sq, ...) do { \
        drift_seq = (sq); \
        snprintf(drift_msg, sizeof drift_msg, __VA_ARGS__); \
    } while (0)

    /* claim: card boundaries exist in the chain and belong to the session */
    const mem_ev_t *fe = mem_find_seq(evs, klines, c->first_seq);
    claims++;
    if (!fe) MEM_DRIFT(c->first_seq, "missing from chain");
    else if (strcmp(fe->sess, c->sess))
        MEM_DRIFT(c->first_seq, "session mismatch (chain has sess=%s)", fe->sess);
    const mem_ev_t *le = mem_find_seq(evs, klines, c->last_seq);
    if (!drift_seq) {
        claims++;
        if (!le) MEM_DRIFT(c->last_seq, "missing from chain");
        else if (strcmp(le->sess, c->sess))
            MEM_DRIFT(c->last_seq, "session mismatch (chain has sess=%s)",
                      le->sess);
    }
    /* claims: every observation's cited seqs replay their typed outcome */
    for (int i = 0; i < no && !drift_seq; i++) {
        if (strcmp(obs[i].cardid, c->id)) continue;
        const mem_ev_t *ea = mem_find_seq(evs, klines, obs[i].seq_a);
        claims++;
        if (!ea) { MEM_DRIFT(obs[i].seq_a, "missing from chain"); break; }
        if (strcmp(ea->sess, c->sess)) {
            MEM_DRIFT(obs[i].seq_a, "session mismatch (chain has sess=%s)",
                      ea->sess);
            break;
        }
        const mem_ev_t *eb = mem_find_seq(evs, klines, obs[i].seq_b);
        claims++;
        if (!eb) { MEM_DRIFT(obs[i].seq_b, "missing from chain"); break; }
        if (strcmp(eb->sess, c->sess)) {
            MEM_DRIFT(obs[i].seq_b, "session mismatch (chain has sess=%s)",
                      eb->sess);
            break;
        }
        if (!strcmp(obs[i].type, "bugfix")) {
            if (ea->rc == 0) {
                MEM_DRIFT(obs[i].seq_a, "rc=0, expected failure");
                break;
            }
            if (eb->rc != 0) {
                MEM_DRIFT(obs[i].seq_b, "rc=%d, expected success", eb->rc);
                break;
            }
            char sa[300], sb[300];
            mem_sig(ea->cmd, sa, sizeof sa);
            mem_sig(eb->cmd, sb, sizeof sb);
            if (strcmp(sa, sb)) {
                MEM_DRIFT(obs[i].seq_b, "cmd diverged ('%s' vs '%s')", sa, sb);
                break;
            }
        }
    }
#undef MEM_DRIFT

    mem_verification_t result = {.claims = claims, .drift_sequence = drift_seq};
    snprintf(result.reason, sizeof(result.reason), "%s", drift_msg);
    return result;
}

static int mem_cmd_verify(char *a[], int n, jash_writer_t w, void *ctx) {
    int json = mem_has(a, n, "--json");
    const char *id = mem_pos(a, n, 0);
    if (!id) {
        jash_writes(jash_stderr_writer, 0, "mem verify: need a card id\n");
        return 2;
    }
    mem_card_t *cards; mem_obs_t *obs; int nc, no;
    if (!mem_store_load(&cards, &nc, &obs, &no)) {
        jash_writes(jash_stderr_writer, 0,
            "mem: no memory for this repo — run 'mem distill'\n");
        return 1;
    }
    const mem_card_t *c = mem_find_card(cards, nc, id);
    if (!c) {
        jash_writef(jash_stderr_writer, 0, "mem: no card '%s'\n", id);
        free(cards); free(obs);
        return 1;
    }
    int klines = 0;
    mem_ev_t *evs = mem_chain_load(&klines);
    if (!evs) {
        jash_writes(jash_stderr_writer, 0, "mem: no chain for this repo\n");
        free(cards); free(obs);
        return 1;
    }

    mem_verification_t verified = mem_verify_claims(c, obs, no, evs, klines);
    int claims = verified.claims; long drift_seq = verified.drift_sequence;
    const char* drift_msg = verified.reason;

    if (json) {
        if (drift_seq) {
            char jm[600]; jash_json_esc(drift_msg, jm, sizeof jm);
            mem_out("{\"card\":\"%s\",\"verdict\":\"DRIFT\",\"seq\":%ld,"
                    "\"reason\":\"%s\",\"claims_replayed\":%d}\n",
                    c->id, drift_seq, jm, claims);
        } else {
            mem_out("{\"card\":\"%s\",\"verdict\":\"VERIFIED\","
                    "\"claims_replayed\":%d}\n", c->id, claims);
        }
    } else if (drift_seq) {
        mem_out("DRIFT %s — seq %ld: %s\n", c->id, drift_seq, drift_msg);
    } else {
        mem_out("VERIFIED %s — %d claims replayed\n", c->id, claims);
    }
    mem_flush(w, ctx, mem_budget(a, n));
    free(cards); free(obs); free(evs);
    return drift_seq ? 1 : 0;
}

/* ── dispatch ──────────────────────────────────────────────────────────────── */

static int mem_dispatch_impl(jash_state_t *s, jash_writer_t w, void *ctx,
                        char *tok[], int nt) {
    if (nt == 0) {
        jash_writes(w, ctx,
            "mem — session memory distilled from the attested chain (ADR-0054)\n"
            "  mem distill [--json]              split chain into session cards + typed observations\n"
            "  mem timeline [--budget N] [--json]   every card, chronologically, with observations\n"
            "  mem show <id> [--json]            one card, full detail\n"
            "  mem search \"<q>\" [--failed] [--type T] [--json]   token search, outcome-aware\n"
            "  mem brief [--budget N] [--json]   session-start injection: last card, gaps, mesh, graph\n"
            "  mem verify <id> [--json]          replay cited seqs against the chain → VERIFIED/DRIFT\n"
            "  memory lives in ~/.jash/mem-<repokey>.tsv — every card cites chain seqs (receipt)\n");
        return 0;
    }
    const char *sub = tok[0];
    char **a = tok + 1; int n = nt - 1;
    if (!strcmp(sub, "distill"))  return mem_cmd_distill(a, n, w, ctx);
    if (!strcmp(sub, "timeline")) return mem_cmd_timeline(a, n, w, ctx);
    if (!strcmp(sub, "show"))     return mem_cmd_show(a, n, w, ctx);
    if (!strcmp(sub, "search"))   return mem_cmd_search(a, n, w, ctx);
    if (!strcmp(sub, "brief"))    return mem_cmd_brief(a, n, w, ctx);
    if (!strcmp(sub, "verify"))   return mem_cmd_verify(a, n, w, ctx);
    (void)s;
    jash_writef(jash_stderr_writer, 0, "mem: unknown subcommand '%s'\n", sub);
    return 2;
}

static int mem_dispatch(jash_state_t *s, jash_writer_t w, void *ctx,
                              char *tok[], int nt) {
    mem_olen = 0; mem_required = 0; mem_obuf[0] = 0;
    mem_output_status = 0; mem_flushed = 0;
    mem_json_mode = mem_has(tok, nt, "--json");
    if (mem_json_mode) jash_budget_unwrap(&w, &ctx);
    int rc = mem_dispatch_impl(s, w, ctx, tok, nt);
    if (mem_output_status) return mem_output_status;
    if (mem_json_mode && !mem_flushed)
        return jash_structured_error(w, ctx, "mem",
                                      rc == 2 ? "invalid_arguments" : "operation_failed",
                                      rc ? rc : 1);
    return rc;
}

int jash_b_mem(jash_state_t *s, jash_writer_t w, void *ctx, const char *args) {
    char buf[4096];
    snprintf(buf, sizeof buf, "%s", args ? args : "");
    char *tok[64]; int nt = 0;
    char *p = buf;
    while (*p && nt < 64) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        char q = 0; char *start;
        if (*p == '"' || *p == '\'') { q = *p; p++; start = p; while (*p && *p != q) p++; }
        else { start = p; while (*p && *p != ' ' && *p != '\t') p++; }
        if (*p) { *p = 0; p++; }
        tok[nt++] = start;
    }
    return mem_dispatch(s, w, ctx, tok, nt);
}

#include "mem_native.inc"

/* ADR-0036 W2: argv entry — quoted queries arrive as ONE argv[i] */
int jash_b_mem_argv(jash_state_t *s, jash_writer_t w, void *ctx,
                    int argc, char **argv) {
    int native = semantic_dispatch_argv(s, w, ctx, "mem", "jash.mem.v1",
                                       argc, argv, mem_native_provider);
    if (native >= 0) return native;
    return mem_dispatch(s, w, ctx, argv, argc);
}
