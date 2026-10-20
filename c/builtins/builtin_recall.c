// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/recall (chain facet) — the agent's command memory (ADR-0023 E1).
//
// `recall cmds|did|history [pattern] [--failed] [--last N]` searches the attested
// execution chain (~/.jash/chain.log, written when JASH_SIGIL>=1) for past
// commands and their outcomes, newest-first. The shell remembers what the agent
// ran after the agent itself has forgotten (context-window loss). This is the
// command/outcome facet of `recall`; the output-overflow facet lives in
// builtin_agent.c. Read-only; independent of whether the sigil module is loaded —
// it just parses the chain format (see SPEC.md). Reached via recall_cmd dispatch.
//
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>   /* jash_json_esc (--json) */
#include <core/adopt.h>        /* jash_repo_key (ADR-0051 W2 --repo) */
#include <core/string_compat.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#define RECALL_MAX_SHOW 500

#define RECALL_TAG_LEN 32

typedef struct {
    unsigned long long seq;
    unsigned long long ts_sec;
    int  rc;
    char tag[RECALL_TAG_LEN];
    char cmd[256];
    /* ADR-0051 W1/W2: context fields (empty on v1 chain lines) */
    char cwd[512];
    char repo[24];
    char sess[64];
    long dur_ms;
    size_t cmd_full_len;   /* W8: real length before display truncation */
} recall_rec_t;

int is_cmdword_char(char c);

/* ADR-0051 W2: parse one `key=value` field from a chain line. */
static int recall_field(const char* line, const char* key, char* out, size_t cap) {
    char pat[24];
    snprintf(pat, sizeof pat, " %s=", key);
    const char* p = strstr(line, pat);
    if (!p) { if (out && cap) out[0] = '\0'; return 0; }
    p += strlen(pat);
    size_t i = 0;
    while (p[i] && p[i] != ' ' && p[i] != '\t' && p[i] != '\n' && i + 1 < cap) {
        out[i] = p[i];
        i++;
    }
    if (cap) out[i] = '\0';
    return 1;
}

/* W2: substring match with optional word-boundary (--word). */
static int recall_match(const char* cmd, const char* pattern, int word) {
    if (!pattern) return 1;
    if (!word) return jash_strcasestr_portable(cmd, pattern) != NULL;
    size_t pl = strlen(pattern);
    const char* p = cmd;
    while ((p = jash_strcasestr_portable(p, pattern))) {
        int left = (p == cmd) || !is_cmdword_char(p[-1]);
        int right = !p[pl] || !is_cmdword_char(p[pl]);
        if (left && right) return 1;
        p++;
    }
    return 0;
}

static const char* recall_chain_path(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !*home) return NULL;
    /* ADR-0051 W6: per-repo chain (same key as the writer). A bare
     * `recall --global` reads the global file explicitly. */
    const char* k = jash_repo_key();
    if (k && k[0] != '-' && !getenv("JASH_RECALL_GLOBAL"))
        snprintf(buf, cap, "%s/.jash/chain-%s.log", home, k);
    else
        snprintf(buf, cap, "%s/.jash/chain.log", home);
    return buf;
}

/* The jash DIFFERENTIATORS — the agent-native superpowers bash/zsh don't have.
 * `recall adoption` measures how many recorded commands actually used one of these,
 * computed from the ATTESTED chain (tamper-evident BI, not self-reported). */
static const char* const JASH_DIFFERENTIATORS[] = {
    "recall","checkpoint","rewind","expect","explain","mesh",
    "undo","timeline","trash","sigil","kv","context","verify","doctor",
    "dashboard","speculate","revert","cost","contract","state","digest",
    "json","agent-guard", NULL
};
static int is_differentiator(const char* word) {
    for (int i = 0; JASH_DIFFERENTIATORS[i]; i++)
        if (!strcmp(word, JASH_DIFFERENTIATORS[i])) return i;
    return -1;
}

int is_cmdword_char(char c) {
    return (c>='a'&&c<='z')||(c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='_';
}

/* The Claude Code shell tool wraps EVERY command as:
 *   setopt … && eval '<real cmd>' && pwd …
 * so a differentiator sits at a command position INSIDE the line, not at word 0
 * (which is always "setopt"). Scan every command position — start, after
 * ; & | newline ( { `, and the string handed to eval/sh/bash — and return the
 * first differentiator's index. Without this the adoption metric is blind to the
 * harness wrapper and reads 0% even when the agent DOES use the verbs. */
static int scan_differentiator(const char* cmd) {
    const char* p = cmd; int at_cmd = 1;
    while (*p) {
        if (*p == ' ' || *p == '\t') { p++; continue; }
        if (at_cmd) {
            while (*p == '\'' || *p == '"') p++;     /* eval '<cmd>' / -c "<cmd>" */
            while (*p == ' ' || *p == '\t') p++;
            char w[48]; unsigned wi = 0;
            while (wi < sizeof(w)-1 && (is_cmdword_char(p[wi]) || p[wi] == '-')) {
                w[wi] = p[wi]; wi++;
            }
            w[wi] = '\0';
            int idx = is_differentiator(w);
            if (idx >= 0) return idx;
            at_cmd = (!strcmp(w,"eval")||!strcmp(w,"sh")||!strcmp(w,"bash")||!strcmp(w,"command")) ? 1 : 0;
            p += wi ? wi : 1;
            continue;
        }
        if (*p==';'||*p=='\n'||*p=='('||*p=='{'||*p=='`') { at_cmd = 1; p++; continue; }
        if (*p=='&'||*p=='|') { p++; if (*p=='&'||*p=='|') p++; at_cmd = 1; continue; }
        p++;
    }
    return -1;
}

/* ADR-0051 W5: `recall why <pattern>` — the causal chain the market makes
 * you reconstruct by hand. Finds the LAST ok run of the pattern and the
 * FIRST subsequent FAIL in the attested chain, then shows what changed
 * between them: context delta (cwd/repo from the v2 fields) and the files
 * the world touched in between (fsjournal, ADR-0038 W2). */
int jash_recall_why(jash_writer_t w, void* ctx, const char* pattern) {
    if (!pattern || !*pattern) {
        jash_writes(w, ctx, "usage: recall why <pattern>\n");
        return 2;
    }
    char path[600];
    if (!recall_chain_path(path, sizeof path)) return 1;
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writes(w, ctx, "recall why: no chain yet (set JASH_SIGIL>=1).\n");
        return 1;
    }

    recall_rec_t ok = {0}, fail = {0}, first = {0};
    int have_ok = 0, have_fail = 0, occurrences = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long long seq = 0, ts = 0, ns = 0;
        char hash[80];
        if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts, &ns, hash) < 4) continue;
        const char* cp = strstr(line, " cmd="); if (!cp) continue;
        const char* cmd = cp + 5;
        if (!jash_strcasestr_portable(cmd, pattern)) continue;
        int rc = 0;
        const char* rp = strstr(line, " rc=");
        if (rp) sscanf(rp, " rc=%d", &rc);
        recall_rec_t r = {0};
        r.seq = seq; r.ts_sec = ts; r.rc = rc;
        snprintf(r.cmd, sizeof(r.cmd), "%s", cmd);
        char* nl = strchr(r.cmd, '\n'); if (nl) *nl = '\0';
        recall_field(line, "cwd", r.cwd, sizeof(r.cwd));
        recall_field(line, "repo", r.repo, sizeof(r.repo));
        recall_field(line, "sess", r.sess, sizeof(r.sess));
        if (!occurrences) first = r;
        occurrences++;
        if (rc == 0) { ok = r; have_ok = 1; have_fail = 0; }
        else if (have_ok && !have_fail) { fail = r; have_fail = 1; }
    }
    fclose(f);

    if (!occurrences) {
        jash_writef(w, ctx, "recall why: no runs matching '%s'\n", pattern);
        return 1;
    }
    if (!have_ok) {
        /* failures but no baseline — say so honestly instead of "no runs" */
        jash_writef(w, ctx,
            "recall why: '%s' has %d run(s), all failed — no successful baseline to compare against\r\n"
            "  first: seq #%llu rc=%d  %s\r\n",
            pattern, occurrences, first.seq, first.rc, first.cmd);
        return 0;
    }
    if (!have_fail) {
        jash_writef(w, ctx,
            "recall why: '%s' never failed after its last success — nothing to explain\n",
            pattern);
        if (have_ok)
            jash_writef(w, ctx, "  last ok: seq #%llu rc=0  %s\n", ok.seq, ok.cmd);
        return 0;
    }

    jash_writef(w, ctx, "\033[1;36m═══ recall why — causal chain for '%s' ═══\033[0m\r\n", pattern);
    jash_writef(w, ctx, "  \033[1;32mWORKED\033[0m  seq #%-6llu %s\r\n", ok.seq, ok.cmd);
    if (ok.cwd[0]) jash_writef(w, ctx, "           cwd=%s repo=%s\r\n", ok.cwd, ok.repo);
    jash_writef(w, ctx, "  \033[1;31mBROKE\033[0m   seq #%-6llu rc=%d  %s\r\n", fail.seq, fail.rc, fail.cmd);
    if (fail.cwd[0]) jash_writef(w, ctx, "           cwd=%s repo=%s\r\n", fail.cwd, fail.repo);

    /* context delta */
    if (ok.cwd[0] && fail.cwd[0] && strcmp(ok.cwd, fail.cwd))
        jash_writef(w, ctx, "  \033[33mΔ cwd changed:\033[0m %s → %s\r\n", ok.cwd, fail.cwd);
    if (ok.repo[0] && fail.repo[0] && strcmp(ok.repo, fail.repo))
        jash_writef(w, ctx, "  \033[33mΔ repo changed:\033[0m %s → %s\r\n", ok.repo, fail.repo);
    if (ok.sess[0] && fail.sess[0] && strcmp(ok.sess, fail.sess))
        jash_writef(w, ctx, "  \033[33mΔ session changed:\033[0m %s → %s\r\n", ok.sess, fail.sess);

    /* files the world touched between the two timestamps (fsjournal) */
    const char* home = getenv("HOME");
    if (home) {
        char jp[600];
        snprintf(jp, sizeof jp, "%s/.jash/fsjournal.tsv", home);
        FILE* jf = fopen(jp, "r");
        if (jf) {
            int shown = 0;
            char jl[1200];
            jash_writes(w, ctx, "  \033[90mfiles changed between WORKED and BROKE:\033[0m\r\n");
            while (fgets(jl, sizeof jl, jf)) {
                unsigned long long fts = strtoull(jl, NULL, 10);
                if (fts <= ok.ts_sec || fts > fail.ts_sec) continue;
                char kind[8], fpath[1024];
                if (sscanf(jl, "%*s %*s %7s %1023[^\n]", kind, fpath) >= 2) {
                    jash_writef(w, ctx, "    %s %s\r\n", kind, fpath);
                    if (++shown >= 20) break;
                }
            }
            if (!shown)
                jash_writes(w, ctx, "    \033[90m(none recorded — the change is elsewhere: env, args, or outside cwd)\033[0m\r\n");
            fclose(jf);
        }
    }
    return 0;
}

/* ADR-0051 W4: ACTIVE memory — the market's memory frameworks all answer
 * when asked; the chain can warn BEFORE the agent repeats the mistake.
 *
 * recall_guard(line) counts how many times THIS EXACT command already
 * failed in the attested chain (tail window of 2000 lines — recent history
 * is the relevant one). Called from the top-level pre-exec hook: count >= 2
 * and the caller prints ONE attested line with the last occurrence's proof.
 * Performance: a per-process cache invalidated on (mtime, size) — one stat
 * per command. Appends (the steady state: the shell itself writes one chain
 * line per command) fold only the new tail into the cache; anything else
 * falls back to a full window rebuild. Never O(chain)/cmd, the JINV-005
 * class. Kill-switch: JASH_RECALL_GUARD=0. */

#define RECALL_GUARD_WINDOW 2000
#define RECALL_GUARD_CACHE 256

typedef struct {
    char cmd[256];
    unsigned fails;
    unsigned long long last_seq;
    unsigned long long last_ts;
    int last_rc;
} recall_guard_ent_t;

static recall_guard_ent_t g_guard[RECALL_GUARD_CACHE];
static unsigned g_guard_n = 0;
static long g_guard_mtime = 0;
static long g_guard_size = 0;   /* chain bytes already folded into the cache */
/* Window parity ring: which guard slot each of the last RECALL_GUARD_WINDOW
 * chain lines fed (-1 = none), so evicted failures stop counting exactly as
 * a full rebuild would. */
static short g_guard_ring[RECALL_GUARD_WINDOW];
static unsigned g_guard_ring_head = 0;   /* oldest line */
static unsigned g_guard_ring_n = 0;

static void guard_ring_push(int k) {
    if (g_guard_ring_n == RECALL_GUARD_WINDOW) {
        int old = g_guard_ring[g_guard_ring_head];
        if (old >= 0 && g_guard[old].fails > 0) g_guard[old].fails--;
        g_guard_ring_head = (g_guard_ring_head + 1) % RECALL_GUARD_WINDOW;
        g_guard_ring_n--;
    }
    g_guard_ring[(g_guard_ring_head + g_guard_ring_n) % RECALL_GUARD_WINDOW] =
        (short)k;
    g_guard_ring_n++;
}

/* Fold one chain line into the guard cache; returns the slot it fed
 * (-1 = line contributes nothing). */
static int guard_fold_line(const char* l) {
    unsigned long long seq = 0, ts = 0, ns = 0;
    char hash[80];
    if (sscanf(l, "%llu %llu.%llu %79s", &seq, &ts, &ns, hash) < 4) return -1;
    const char* cp = strstr(l, " cmd="); if (!cp) return -1;
    const char* cmd = cp + 5;
    if (!*cmd || !strncmp(cmd, "SESSION_END", 11)) return -1;
    int rc = 0;
    const char* rp = strstr(l, " rc=");
    if (rp) sscanf(rp, " rc=%d", &rc);
    if (rc == 0) return -1;                        /* only failures teach */
    char word[256]; unsigned wi = 0;
    while (*cmd == ' ') cmd++;
    while (cmd[wi] && cmd[wi] != '\n' && wi < sizeof(word) - 1) { word[wi] = cmd[wi]; wi++; }
    word[wi] = '\0';
    if (!wi) return -1;
    unsigned k = 0;
    for (; k < g_guard_n; k++) if (!strcmp(g_guard[k].cmd, word)) break;
    if (k == g_guard_n) {
        if (g_guard_n >= RECALL_GUARD_CACHE) return -1;
        g_guard_n++;
        strncpy(g_guard[k].cmd, word, sizeof(g_guard[k].cmd) - 1);
        g_guard[k].fails = 0;
    }
    g_guard[k].fails++;
    g_guard[k].last_seq = seq;
    g_guard[k].last_ts = ts;
    g_guard[k].last_rc = rc;
    return (int)k;
}

/* This advisory reader runs before every command, including commands which
 * will explicitly report a damaged chain. Never wait for a FIFO writer or
 * accept a device merely because a pre-open pathname stat succeeded. */
static FILE* recall_guard_open(const char* path) {
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) return NULL;
    struct stat status;
    int failed = fstat(descriptor, &status) ? errno : !S_ISREG(status.st_mode) ? EINVAL : 0;
    if (failed) { close(descriptor); errno = failed; return NULL; }
    FILE* file = fdopen(descriptor, "r");
    if (!file) { int saved = errno; close(descriptor); errno = saved; }
    return file;
}

static void recall_guard_rebuild(const char* path, long mtime) {
    g_guard_n = 0;
    g_guard_mtime = mtime;
    g_guard_size = 0;
    g_guard_ring_head = 0;
    g_guard_ring_n = 0;
    FILE* f = recall_guard_open(path);
    if (!f) return;
    /* tail window: keep only the last RECALL_GUARD_WINDOW matching lines.
     *
     * JINV-103: the window was always the intent, but the old loop reached it
     * by fgets()-ing the ENTIRE chain into a 2000x1024 ring (2MB calloc) and
     * throwing away all but the tail. This runs on every command via eval.c,
     * and the mtime cache never helps a one-shot `jash -c` (fresh process,
     * cold cache, and the chain's mtime changes every command anyway), so a
     * 105MB chain was fully parsed per command. Seek back from EOF instead
     * and keep only line pointers; selection semantics are unchanged. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return; }
    long fsize = ftell(f);
    if (fsize <= 0) { fclose(f); return; }
    g_guard_size = fsize;

    const char** slot = calloc(RECALL_GUARD_WINDOW, sizeof(char*));
    if (!slot) { fclose(f); return; }
    char* mem = NULL;
    unsigned long long total = 0;
    long win = (long)RECALL_GUARD_WINDOW * 320L;   /* ~typical chain line */
    const long WIN_MAX = 64L * 1024L * 1024L;

    for (;;) {
        if (win > fsize) win = fsize;
        long start = fsize - win;
        free(mem);
        mem = malloc((size_t)win + 1);
        if (!mem) { free(slot); fclose(f); return; }
        if (fseek(f, start, SEEK_SET) != 0) { free(mem); free(slot); fclose(f); return; }
        size_t got = fread(mem, 1, (size_t)win, f);
        mem[got] = '\0';

        char* p = mem;
        if (start > 0) {                    /* drop the partial leading line */
            char* nl = memchr(mem, '\n', got);
            if (!nl) { if (win >= fsize || win >= WIN_MAX) break; win *= 4; continue; }
            p = nl + 1;
        }
        total = 0;
        while (*p) {
            char* nl = strchr(p, '\n');
            if (nl) *nl = '\0';
            slot[total % RECALL_GUARD_WINDOW] = p;
            total++;
            if (!nl) break;
            p = nl + 1;
        }
        /* Fewer lines than the window and more file behind us? Widen once. */
        if (total >= RECALL_GUARD_WINDOW || start == 0 || win >= fsize || win >= WIN_MAX)
            break;
        win *= 4;
    }
    fclose(f);

    unsigned long long shown = total < RECALL_GUARD_WINDOW ? total : RECALL_GUARD_WINDOW;
    for (unsigned long long i = 0; i < shown; i++) {
        unsigned long long idx = total - shown + i;
        const char* l = slot[idx % RECALL_GUARD_WINDOW];
        guard_ring_push(l ? guard_fold_line(l) : -1);
    }
    free(mem);
    free(slot);
}

/* The chain is append-only and the shell itself appends one line per
 * command, so the mtime cache would otherwise trigger a full window
 * re-read (~160KB) on EVERY command in a long-lived shell. When the file
 * only grew since the last fold, parse just the new tail; the eviction
 * ring keeps the last-2000-lines window semantics of a full rebuild. */
static void recall_guard_fold_tail(const char* path, long mtime, long fsize) {
    FILE* f = recall_guard_open(path);
    if (!f) { recall_guard_rebuild(path, mtime); return; }
    long tail = fsize - g_guard_size;
    char* mem = (char*)malloc((size_t)tail + 1);
    if (!mem ||
        fseek(f, g_guard_size, SEEK_SET) != 0 ||
        fread(mem, 1, (size_t)tail, f) != (size_t)tail) {
        free(mem);
        fclose(f);
        recall_guard_rebuild(path, mtime);
        return;
    }
    fclose(f);
    /* Fold complete lines only: a partial trailing line (writer mid-append)
     * is folded next time, once its newline has landed. */
    char* p = mem;
    char* end = mem + tail;
    while (p < end) {
        char* nl = memchr(p, '\n', (size_t)(end - p));
        if (!nl) break;
        *nl = '\0';
        guard_ring_push(guard_fold_line(p));
        g_guard_size += nl + 1 - p;
        p = nl + 1;
    }
    g_guard_mtime = mtime;
    free(mem);
}

/* Returns the number of past failures of this exact command (0 = clean).
 * When >= 1, fills the last occurrence's proof. */
unsigned jash_recall_guard(const char* line,
                           unsigned long long* out_seq,
                           unsigned long long* out_ts,
                           int* out_rc) {
    const char* e = getenv("JASH_RECALL_GUARD");
    if (e && e[0] == '0' && e[1] == '\0') return 0;
    if (!line) return 0;
    char path[600];
    if (!recall_chain_path(path, sizeof path)) return 0;
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    /* Invalidate on (mtime, size): the chain grows by one line per command,
     * and two commands can share the same mtime second. Strict growth means
     * a pure append -> fold only the new tail; equal size with a new mtime
     * (touch/rewrite) or a shrink (rotation) falls back to a full rebuild. */
    if (st.st_mtime != g_guard_mtime || (long)st.st_size != g_guard_size) {
        if (g_guard_size > 0 && (long)st.st_size > g_guard_size)
            recall_guard_fold_tail(path, (long)st.st_mtime, (long)st.st_size);
        else
            recall_guard_rebuild(path, (long)st.st_mtime);
    }

    char word[256]; unsigned wi = 0;
    while (*line == ' ' || *line == '\t') line++;
    while (line[wi] && line[wi] != '\n' && wi < sizeof(word) - 1) { word[wi] = line[wi]; wi++; }
    word[wi] = '\0';
    if (!wi) return 0;

    for (unsigned k = 0; k < g_guard_n; k++) {
        if (!strcmp(g_guard[k].cmd, word)) {
            if (out_seq) *out_seq = g_guard[k].last_seq;
            if (out_ts)  *out_ts  = g_guard[k].last_ts;
            if (out_rc)  *out_rc  = g_guard[k].last_rc;
            return g_guard[k].fails;
        }
    }
    return 0;
}

/* `recall adoption` — BI: of every attested command, how many used a jash
 * differentiator? Overall % + per-feature breakdown. Human + --json. */
int jash_recall_chain_adoption(jash_writer_t w, void* ctx, int json) {
    char path[600];
    if (!recall_chain_path(path, sizeof path)) { jash_writes(jash_stderr_writer, 0, "recall: no HOME\n"); return 1; }
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writes(w, ctx, json
            ? "{\"recorded\":false,\"total\":0,\"differentiator_uses\":0,\"adoption_pct\":0,\"features\":{}}\n"
            : "recall adoption: no chain yet — set JASH_SIGIL>=1 so usage is recorded (attested).\n");
        return 1;
    }
    unsigned total = 0, diff_total = 0;
    unsigned per[32] = {0};
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long long seq, ts, ns; char hash[80];
        if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts, &ns, hash) < 4) continue;
        const char* cp = strstr(line, " cmd="); if (!cp) continue;
        const char* cmd = cp + 5;
        if (strncmp(cmd, "SESSION_END", 11) == 0) continue;
        char word[48]; unsigned wi = 0; const char* q = cmd;
        while (*q == ' ') q++;
        while (*q && *q != ' ' && *q != '\n' && wi < sizeof(word)-1) word[wi++] = *q++;
        word[wi] = '\0';
        if (!wi) continue;
        total++;
        int idx = scan_differentiator(cmd);   /* see through the eval-wrapper */
        if (idx >= 0) { diff_total++; if (idx < 32) per[idx]++; }
    }
    fclose(f);
    int pct = total ? (int)((diff_total * 100 + total/2) / total) : 0;

    if (json) {
        jash_writef(w, ctx, "{\"recorded\":true,\"total\":%u,\"differentiator_uses\":%u,\"adoption_pct\":%d,\"features\":{",
                    total, diff_total, pct);
        int c = 0;
        for (int i = 0; JASH_DIFFERENTIATORS[i] && i < 32; i++)
            if (per[i]) { jash_writef(w, ctx, "%s\"%s\":%u", c?",":"", JASH_DIFFERENTIATORS[i], per[i]); c++; }
        jash_writes(w, ctx, "}}\n");
        return 0;
    }
    jash_writef(w, ctx, "\033[1;36m═══ jash adoption (from the attested chain) ═══\033[0m\r\n");
    jash_writef(w, ctx, "  %u commands recorded · \033[1m%u\033[0m used a jash differential · "
                        "\033[1;%sm%d%%\033[0m adoption\r\n",
                total, diff_total, pct >= 25 ? "32" : (pct > 0 ? "33" : "31"), pct);
    int any = 0;
    for (int i = 0; JASH_DIFFERENTIATORS[i] && i < 32; i++)
        if (per[i]) { jash_writef(w, ctx, "    %-12s %u\r\n", JASH_DIFFERENTIATORS[i], per[i]); any = 1; }
    if (!any) jash_writes(w, ctx, "    \033[31m(none used — agents are treating jash as plain bash)\033[0m\r\n");
    return 0;
}

/* `recall stats` — aggregate the chain: total runs, failures, and the commands
 * I run/fail most (by first word). The agent's self-knowledge: "what do I lean on,
 * what keeps biting me?" */
#define RECALL_STAT_MAX 512
typedef struct { char word[48]; unsigned runs; unsigned fails; } recall_stat_t;

int jash_recall_chain_stats(jash_writer_t w, void* ctx) {
    char path[600];
    if (!recall_chain_path(path, sizeof path)) { jash_writes(jash_stderr_writer, 0, "recall: no HOME\n"); return 1; }
    FILE* f = fopen(path, "r");
    if (!f) { jash_writes(w, ctx, "recall: no recorded history yet (set JASH_SIGIL>=1).\n"); return 1; }

    recall_stat_t* tab = (recall_stat_t*)calloc(RECALL_STAT_MAX, sizeof(recall_stat_t));
    if (!tab) { fclose(f); return 1; }
    unsigned ndistinct = 0, total = 0, fails = 0;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long long seq, ts, ns; char hash[80];
        if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts, &ns, hash) < 4) continue;
        const char* cp = strstr(line, " cmd=");
        if (!cp) continue;
        const char* cmd = cp + 5;
        if (strncmp(cmd, "SESSION_END", 11) == 0) continue;
        int rc = 0; const char* rp = strstr(line, " rc=");
        if (rp) sscanf(rp, " rc=%d", &rc);
        /* first word of the command */
        char word[48]; unsigned wi = 0;
        const char* q = cmd;
        while (*q == ' ') q++;
        while (*q && *q != ' ' && *q != '\n' && wi < sizeof(word) - 1) word[wi++] = *q++;
        word[wi] = '\0';
        if (!wi) continue;
        total++; if (rc != 0) fails++;
        unsigned i = 0;
        for (; i < ndistinct; i++) if (!strcmp(tab[i].word, word)) break;
        if (i == ndistinct && ndistinct < RECALL_STAT_MAX) { strncpy(tab[i].word, word, sizeof(tab[i].word)-1); ndistinct++; }
        if (i < RECALL_STAT_MAX) { tab[i].runs++; if (rc != 0) tab[i].fails++; }
    }
    fclose(f);

    if (total == 0) { jash_writes(w, ctx, "recall: chain is empty\n"); free(tab); return 1; }

    int pct = (int)((fails * 100 + total/2) / total);
    jash_writef(w, ctx, "\033[1;36m═══ recall stats — %u runs, %u failed (%d%%) ═══\033[0m\r\n",
                total, fails, pct);

    /* top 5 by runs */
    jash_writes(w, ctx, "\033[1mmost run:\033[0m\r\n");
    for (int rank = 0; rank < 5; rank++) {
        int best = -1;
        for (unsigned i = 0; i < ndistinct; i++)
            if (tab[i].runs && (best < 0 || tab[i].runs > tab[best].runs)) best = (int)i;
        if (best < 0) break;
        jash_writef(w, ctx, "  %5u×  %-16s\033[90m%s\033[0m\r\n",
                    tab[best].runs, tab[best].word,
                    tab[best].fails ? "" : "");
        tab[best].runs = 0; /* consume */
    }
    /* top failures (rebuild fails view: re-scan distinct by fails) — tab.runs was
     * consumed, but fails is intact */
    int any_fail = 0;
    for (unsigned i = 0; i < ndistinct; i++) if (tab[i].fails) { any_fail = 1; break; }
    if (any_fail) {
        jash_writes(w, ctx, "\033[1mmost failed:\033[0m\r\n");
        for (int rank = 0; rank < 5; rank++) {
            int best = -1;
            for (unsigned i = 0; i < ndistinct; i++)
                if (tab[i].fails && (best < 0 || tab[i].fails > tab[best].fails)) best = (int)i;
            if (best < 0) break;
            jash_writef(w, ctx, "  \033[1;31m%5u✗\033[0m  %-16s\r\n", tab[best].fails, tab[best].word);
            tab[best].fails = 0;
        }
    }
    free(tab);
    return 0;
}

static void recall_parse_tag(const char* line, char* tag, size_t tagcap) {
    const char* tp = strstr(line, " tag=");
    if (!tp) { snprintf(tag, tagcap, "-"); return; }
    tp += 5;
    size_t i = 0;
    while (*tp && *tp != ' ' && *tp != '\t' && *tp != '\n' && i < tagcap - 1) tag[i++] = *tp++;
    tag[i] = '\0';
}

int jash_recall_chain(jash_writer_t w, void* ctx, const char* args) {
    const char* pattern = NULL;
    const char* tag_filter = NULL;
    int only_failed = 0;
    int only_ok = 0;
    int json = 0;
    int last = 20;
    int word = 0;
    /* ADR-0051 W2: contextual filters */
    const char* f_cwd = NULL;
    const char* f_repo = NULL;
    const char* f_sess = NULL;
    unsigned long long f_since = 0, f_until = 0;

    /* Parse args: [pattern] [--failed] [--ok] [--json] [--last N] [--tag T]
     * [--cwd D] [--repo K] [--session S] [--since TS] [--until TS] [--word] */
    char buf[1024];
    if (args && *args) {
        strlcpy(buf, args, sizeof(buf)); buf[sizeof(buf) - 1] = '\0';
        char* save = NULL;
        for (char* t = strtok_r(buf, " \t", &save); t; t = strtok_r(NULL, " \t", &save)) {
            if (strcmp(t, "--failed") == 0) only_failed = 1;
            else if (strcmp(t, "--ok") == 0) only_ok = 1;
            else if (strcmp(t, "--json") == 0) json = 1;
            else if (strcmp(t, "--word") == 0) word = 1;
            else if (strcmp(t, "--last") == 0) {
                char* n = strtok_r(NULL, " \t", &save);
                if (n) last = atoi(n);
            } else if (strncmp(t, "--last=", 7) == 0) {
                last = atoi(t + 7);
            } else if (strcmp(t, "--tag") == 0) {
                tag_filter = strtok_r(NULL, " \t", &save);
            } else if (strncmp(t, "--tag=", 6) == 0) {
                tag_filter = t + 6;
            } else if (strcmp(t, "--cwd") == 0) {
                f_cwd = strtok_r(NULL, " \t", &save);
            } else if (strncmp(t, "--cwd=", 6) == 0) {
                f_cwd = t + 6;
            } else if (strcmp(t, "--repo") == 0) {
                f_repo = strtok_r(NULL, " \t", &save);
                if (!f_repo) f_repo = jash_repo_key();  /* bare --repo = current */
            } else if (strncmp(t, "--repo=", 7) == 0) {
                f_repo = t + 7;
            } else if (strcmp(t, "--session") == 0) {
                f_sess = strtok_r(NULL, " \t", &save);
            } else if (strncmp(t, "--session=", 10) == 0) {
                f_sess = t + 10;
            } else if (strncmp(t, "--since=", 8) == 0) {
                f_since = strtoull(t + 8, NULL, 10);
            } else if (strcmp(t, "--since") == 0) {
                char* n = strtok_r(NULL, " \t", &save);
                if (n) f_since = strtoull(n, NULL, 10);
            } else if (strncmp(t, "--until=", 8) == 0) {
                f_until = strtoull(t + 8, NULL, 10);
            } else if (strcmp(t, "--until") == 0) {
                char* n = strtok_r(NULL, " \t", &save);
                if (n) f_until = strtoull(n, NULL, 10);
            } else if (!pattern && t[0] != '-') {
                pattern = t;   /* first bare token = search substring */
            }
        }
    }
    if (last < 1) last = 1;
    if (last > RECALL_MAX_SHOW) last = RECALL_MAX_SHOW;

    char path[600];
    if (!recall_chain_path(path, sizeof path)) {
        jash_writes(jash_stderr_writer, 0, "recall: no HOME\n");
        return 1;
    }
    FILE* f = fopen(path, "r");
    if (!f) {
        jash_writes(w, ctx,
            "recall: no recorded history yet.\n"
            "  Set JASH_SIGIL=1 (or 2) so jash records every command into the\n"
            "  verifiable chain (~/.jash/chain.log) — then `recall` can answer\n"
            "  \"did I already try this? did it work?\".\n");
        return 1;
    }

    /* Single forward pass; keep the most recent `last` matches in a ring. */
    recall_rec_t* ring = (recall_rec_t*)calloc((size_t)last, sizeof(recall_rec_t));
    if (!ring) { fclose(f); return 1; }
    unsigned long long matched = 0;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        unsigned long long seq = 0, ts_sec = 0, ts_ns = 0;
        char hash[80];
        if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts_sec, &ts_ns, hash) < 4) continue;
        const char* cp = strstr(line, " cmd=");
        if (!cp) continue;
        const char* cmd = cp + 5;
        if (strncmp(cmd, "SESSION_END", 11) == 0) continue;  /* not a real command */
        int rc = 0;
        const char* rp = strstr(line, " rc=");
        if (rp) sscanf(rp, " rc=%d", &rc);
        if (only_failed && rc == 0) continue;
        if (only_ok && rc != 0) continue;
        if (pattern && !recall_match(cmd, pattern, word)) continue;

        /* ADR-0051 W2: contextual filters (v1 lines have empty fields —
         * a filter set by the user NEVER matches an empty field, honestly:
         * filtering by context can only see context that exists) */
        char fbuf[512];
        if (f_cwd) {
            recall_field(line, "cwd", fbuf, sizeof fbuf);
            if (!fbuf[0] || !jash_strcasestr_portable(fbuf, f_cwd)) continue;
        }
        if (f_repo) {
            recall_field(line, "repo", fbuf, sizeof fbuf);
            if (!fbuf[0] || strcmp(fbuf, f_repo) != 0) continue;
        }
        if (f_sess) {
            recall_field(line, "sess", fbuf, sizeof fbuf);
            if (!fbuf[0] || strcmp(fbuf, f_sess) != 0) continue;
        }
        if (f_since && ts_sec < f_since) continue;
        if (f_until && ts_sec > f_until) continue;

        char tagbuf[RECALL_TAG_LEN];
        recall_parse_tag(line, tagbuf, sizeof(tagbuf));
        if (tag_filter && strcmp(tagbuf, tag_filter) != 0) continue;

        recall_rec_t* r = &ring[matched % (unsigned long long)last];
        r->seq = seq; r->ts_sec = ts_sec; r->rc = rc;
        snprintf(r->tag, sizeof(r->tag), "%s", tagbuf);
        /* ADR-0051 W8: no silent truncation — remember the real length and
         * mark it in the display instead of cutting mid-word invisibly */
        r->cmd_full_len = strlen(cmd);
        strlcpy(r->cmd, cmd, sizeof(r->cmd)); r->cmd[sizeof(r->cmd) - 1] = '\0';
        for (int i = 0; r->cmd[i]; i++) if (r->cmd[i] == '\n') { r->cmd[i] = '\0'; break; }
        recall_field(line, "cwd", r->cwd, sizeof(r->cwd));
        recall_field(line, "repo", r->repo, sizeof(r->repo));
        recall_field(line, "sess", r->sess, sizeof(r->sess));
        r->dur_ms = 0;
        if (recall_field(line, "dur", fbuf, sizeof fbuf)) r->dur_ms = atol(fbuf);
        matched++;
    }
    fclose(f);

    if (matched == 0) {
        if (pattern) jash_writef(w, ctx, "recall: nothing matching '%s'%s\n",
                                 pattern, only_failed ? " (failed only)" : "");
        else         jash_writes(w, ctx, "recall: chain is empty\n");
        free(ring);
        return 1;
    }

    /* Print newest-first: the ring holds the last min(matched,last) records in
     * arrival order; walk backward from the most recent. */
    unsigned long long shown = (matched < (unsigned long long)last) ? matched : (unsigned long long)last;
    for (unsigned long long i = 0; i < shown; i++) {
        unsigned long long idx = (matched - 1 - i) % (unsigned long long)last;
        recall_rec_t* r = &ring[idx];
        if (json) {
            char ecmd[1024];
            jash_json_esc(r->cmd, ecmd, sizeof ecmd);
            char etag[128];
            jash_json_esc(r->tag, etag, sizeof etag);
            char ecwd[600];
            jash_json_esc(r->cwd, ecwd, sizeof ecwd);
            /* ADR-0051 W2: stable machine envelope with context (empty on v1) */
            jash_writef(w, ctx, "{\"seq\":%llu,\"ts\":%llu,\"rc\":%d,\"tag\":\"%s\",\"cmd\":\"%s\",\"cwd\":\"%s\",\"repo\":\"%s\",\"dur\":%ld,\"sess\":\"%s\"}\n",
                        r->seq, r->ts_sec, r->rc, etag, ecmd, ecwd,
                        r->repo, r->dur_ms, r->sess);
            continue;
        }
        char ts[16] = "??:??:??";
        time_t t = (time_t)r->ts_sec;
        struct tm* tm = localtime(&t);
        if (tm) snprintf(ts, sizeof ts, "%02d:%02d:%02d", tm->tm_hour, tm->tm_min, tm->tm_sec);
        const char* rc_tag = (r->rc == 0) ? "\033[1;32mok\033[0m" : "\033[1;31mFAIL\033[0m";
        const char* tag_color = "\033[36m";
        if (strncmp(r->tag, "err", 3) == 0) tag_color = "\033[1;31m";
        else if (strncmp(r->tag, "retry", 5) == 0) tag_color = "\033[1;33m";
        else if (strncmp(r->tag, "sw:", 3) == 0) tag_color = "\033[33m";
        jash_writef(w, ctx, "\033[90m#%llu %s\033[0m %s %s%-8s\033[0m rc=%-3d  %s",
                    r->seq, ts, rc_tag, tag_color, r->tag, r->rc, r->cmd);
        /* ADR-0051 W8: honest truncation marker, never a silent cut */
        if (r->cmd_full_len > sizeof(r->cmd) - 1)
            jash_writef(w, ctx, "\033[90m…(+%zu chars)\033[0m",
                        r->cmd_full_len - (sizeof(r->cmd) - 1));
        if (r->dur_ms > 0)
            jash_writef(w, ctx, "  \033[90m[%ldms]\033[0m", r->dur_ms);
        jash_writes(w, ctx, "\n");
    }
    free(ring);
    return 0;
}

#include "recall_native.inc"
