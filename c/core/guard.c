// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/guard — pre-execution guard (Feature 3, ADR-0022)

#include "guard.h"
#include "differentiators.h"
#include "adopt.h"
#include "nudge.h"
#include "claims.h"
#include <module/sigil.h>
#include <theme/io.h>
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>          /* clock_gettime/CLOCK_MONOTONIC — glibc does not
                            * pull this in via <sys/time.h> the way Darwin does */
#include <unistd.h>

// ── Agent anti-bypass helpers ───────────────────────────────────────────────

static int is_bypass_shell(const char* cmd) {
    if (!cmd || !*cmd) return 0;
    const char* shells[] = {
        "bash", "sh", "zsh", "dash", "ksh", "fish",
        "/bin/bash", "/bin/sh", "/bin/zsh", "/bin/dash", "/usr/bin/bash",
        "/usr/bin/sh", "/usr/bin/zsh", "/usr/bin/env bash", "/usr/bin/env sh",
        "python", "python3", "python2", "/usr/bin/python", "/usr/bin/python3",
        "ruby", "perl", "node", "php", NULL
    };
    for (int i = 0; shells[i]; i++)
        if (strcmp(cmd, shells[i]) == 0) return 1;
    return 0;
}

static int has_bypass_flag(const char* args) {
    if (!args) return 0;
    return strstr(args, " -c") != NULL || strncmp(args, "-c", 2) == 0;
}

/* Suggest a jash primitive for the most common bypass patterns. */
static void bypass_suggest(jash_writer_t w, void* ctx, const char* cmd, const char* args) {
    if (!args) args = "";
    const char* hint = NULL;
    if (strstr(cmd, "bash") || strstr(cmd, "sh") || strstr(cmd, "zsh") || strstr(cmd, "dash")) {
        hint = "Run the command directly with jash -c, or break it into jash primitives.";
    } else if (strstr(cmd, "python") || strstr(cmd, "python3")) {
        hint = "Use jash builtins (json, contract, expect) instead of python one-liners.";
    } else if (strstr(cmd, "ruby") || strstr(cmd, "perl") || strstr(cmd, "node")) {
        hint = "Replace with a jash primitive or a small script executed via jash -c.";
    }
    if (!hint) return;
    /* Also detect embedded obvious operations inside the -c string. */
    const char* inner = args;
    const char* cflag = strstr(args, "-c");
    if (cflag) { inner = cflag + 2; while (*inner == ' ' || *inner == '\t') inner++; if (*inner == '\'' || *inner == '"') inner++; }
    const char* op_hint = NULL;
    if (strstr(inner, "rm ") || strstr(inner, "rm -")) op_hint = "For deletes, use `trash on`, `speculate rm ...`, or `rewind auto`.";
    else if (strstr(inner, "git status")) op_hint = "Use `json git status` for structured output.";
    else if (strstr(inner, "git log"))    op_hint = "Use `json git log` for structured output.";
    else if (strstr(inner, "git commit")) op_hint = "Use `revert git commit` to undo safely.";
    else if (strstr(inner, "make") || strstr(inner, "cmake")) op_hint = "Wrap builds in `contract --expect rc 0 -- ...`.";
    jash_writef(w, ctx, "[agent-guard] tip: %s\n", hint);
    if (op_hint) jash_writef(w, ctx, "[agent-guard] tip: %s\n", op_hint);
}

/* ADR-0040 W8: the generic WARNING above is educational and ignorable — an
 * agent can read it 80 times in a session and keep bypassing (empirical:
 * this feature's author did exactly that). So name the CONCRETE loss at the
 * moment it materializes: WHICH command just became invisible to
 * recall/undo/attestation, and how many times that happened this session.
 * Real stderr only (BUG-148); JASH_NUDGE=0 or JASH_ADOPT=0 silences;
 * cooldown keeps it from becoming wallpaper. */
#define BYPASS_COST_COOLDOWN 120
static void bypass_cost_nudge(const char* args) {
    const char* e = getenv("JASH_NUDGE");
    if (e && e[0] == '0') return;                 /* explicit opt-out */
    if (jash_adopt_level() == 0) return;          /* adoption kill-switch */
    if (!jash_nudge_due("bypass-cost", BYPASS_COST_COOLDOWN)) return;
    const char* inner = args ? args : "";
    const char* cflag = strstr(inner, "-c");
    if (cflag) {
        inner = cflag + 2;
        while (*inner == ' ' || *inner == '\t') inner++;
        if (*inner == '\'' || *inner == '"') inner++;
    }
    char prev[52];
    size_t n = strlen(inner);
    if (n > 48) { memcpy(prev, inner, 45); memcpy(prev + 45, "...", 4); }
    else        { memcpy(prev, inner, n + 1); }
    size_t pl = strlen(prev);
    if (pl && (prev[pl-1] == '\'' || prev[pl-1] == '"')) prev[pl-1] = 0;
    unsigned b = 0;
    jash_guard_metrics_session(NULL, NULL, &b, NULL, NULL);
    jash_writef(jash_stderr_writer, 0,
        "\033[90m[jash] bypass cost #%u this session: '%s' ran outside jash "
        "\xe2\x80\x94 invisible to recall/undo/attestation. The same line "
        "runs natively (jash -c). JASH_NUDGE=0 silences\033[0m\n", b, prev);
}

static int metrics_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    snprintf(b, cap, "%s/.jash/agent_metrics.tsv", h);
    return 1;
}

/* PERF (bench-loss root cause #3): metrics used to fopen+append+fclose the
 * TSV once PER COMMAND — an agent session pays 3 file opens per evaluated
 * line. Records are now tallied in memory and flushed as ONE aggregated row
 * (metrics_read sums per-line counts, so a row with total>1 is format-
 * compatible). Flush happens every 32 records, at exit, and before any
 * in-process read. Worst case on hard crash: 31 advisory counters lost.
 * F E R M I ∞ H A R T */
static unsigned g_pend_total = 0, g_pend_native = 0,
                g_pend_bypass = 0, g_pend_blocked = 0,
                g_pend_upgraded = 0;   /* ADR-0040 W2: shell-assisted rewrites */

/* JINV-005 — o total acumulado tem que custar O(1), não O(arquivo).
 *
 * `metrics_read` somava o TSV INTEIRO a cada chamada, e
 * `jash_adopt_scorecard_maybe` (adopt.c) a chamava no epílogo de TODA
 * invocação `jash -c` — inclusive nos 90% dos casos em que o gate `t % 10`
 * ia descartar o resultado logo em seguida. Medido em 2026-07-27: o arquivo
 * real tinha 684 KB / 36.233 linhas e crescia ~65 KB por hora de uso; a
 * varredura custava 83-147 ms POR COMANDO, e o custo crescia sem limite
 * porque nada rotacionava o arquivo.
 *
 * Correção: um sidecar de UMA linha com os totais acumulados, atualizado no
 * flush. O TSV continua sendo o registro append-only (histórico, auditoria);
 * o sidecar é só o índice. Se o sidecar sumir ou for corrompido, o caminho
 * antigo (varredura completa) o reconstrói — degradação graciosa, nunca
 * resultado errado silencioso.
 * Gate: tests/test_jinv005_metrics_o1.sh
 */
static int metrics_sum_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    snprintf(b, cap, "%s/.jash/agent_metrics.sum", h);
    return 1;
}

/* Devolve 1 se o sidecar existe e parseia; 0 caso contrário. */
static int metrics_sum_read(unsigned* t, unsigned* n, unsigned* b,
                            unsigned* bl, unsigned* up) {
    char p[600];
    if (!metrics_sum_path(p, sizeof p)) return 0;
    FILE* f = fopen(p, "r");
    if (!f) return 0;
    unsigned a=0,c=0,d=0,e=0,g=0;
    int got = fscanf(f, "%u %u %u %u %u", &a, &c, &d, &e, &g);
    (void)fclose(f);
    if (got != 5) return 0;
    *t = a; *n = c; *b = d; *bl = e; *up = g;
    return 1;
}

static void metrics_sum_write(unsigned t, unsigned n, unsigned b,
                              unsigned bl, unsigned up) {
    char p[600];
    if (!metrics_sum_path(p, sizeof p)) return;
    FILE* f = fopen(p, "w");
    if (!f) return;
    (void)fprintf(f, "%u %u %u %u %u\n", t, n, b, bl, up);
    (void)fclose(f);
}

static void metrics_flush(void) {
    if (!g_pend_total && !g_pend_upgraded) return;
    unsigned t = g_pend_total, n = g_pend_native,
             b = g_pend_bypass, bl = g_pend_blocked, up = g_pend_upgraded;
    g_pend_total = g_pend_native = g_pend_bypass = g_pend_blocked =
        g_pend_upgraded = 0;
    char p[600];
    if (!metrics_path(p, sizeof p)) return;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    FILE* f = fopen(p, "a");
    if (!f) return;
    (void)fprintf(f, "%llu\t%u\t%u\t%u\t%u\t%u\n",
            (unsigned long long)tv.tv_sec, t, n, b, bl, up);
    (void)fclose(f);
    /* JINV-005: manter o índice em dia. Só incrementa se o sidecar já existe;
     * se não existe, o próximo metrics_read o constrói a partir do TSV — assim
     * um sidecar ausente nunca vira um total ERRADO, só um scan a mais. */
    unsigned st=0,sn=0,sb=0,sbl=0,sup=0;
    if (metrics_sum_read(&st, &sn, &sb, &sbl, &sup))
        metrics_sum_write(st+t, sn+n, sb+b, sbl+bl, sup+up);
}

/* Varredura completa do TSV — o caminho lento, agora só usado para
 * (re)construir o sidecar. JINV-005. */
static void metrics_scan_full(unsigned* total, unsigned* native,
                              unsigned* bypass, unsigned* blocks,
                              unsigned* upgraded) {
    *total = *native = *bypass = *blocks = *upgraded = 0;
    char p[600];
    if (!metrics_path(p, sizeof p)) return;
    FILE* f = fopen(p, "r");
    if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        unsigned t=0,n=0,b=0,bl=0,up=0;
        /* 5-count rows are ADR-0040 W2+; older 4-count rows parse with up=0. */
        if (sscanf(line, "%*s\t%u\t%u\t%u\t%u\t%u", &t, &n, &b, &bl, &up) >= 4) {
            *total += t; *native += n; *bypass += b; *blocks += bl;
            *upgraded += up;
        }
    }
    (void)fclose(f);
}

static void metrics_read(unsigned* total, unsigned* native, unsigned* bypass,
                         unsigned* blocks, unsigned* upgraded) {
    *total = *native = *bypass = *blocks = *upgraded = 0;
    metrics_flush();
    /* JINV-005: caminho rápido — o sidecar. */
    if (metrics_sum_read(total, native, bypass, blocks, upgraded)) return;
    /* Sidecar ausente/corrompido: reconstrói do TSV e persiste o índice. */
    metrics_scan_full(total, native, bypass, blocks, upgraded);
    metrics_sum_write(*total, *native, *bypass, *blocks, *upgraded);
}

/* ADR-0040 W4: session-scoped mirrors — incremented alongside the pending
 * batch but NEVER reset by metrics_flush(). They answer "what happened in
 * THIS process?" for the cross-session telemetry row. */
static unsigned g_sess_total = 0, g_sess_native = 0,
                g_sess_bypass = 0, g_sess_blocked = 0,
                g_sess_upgraded = 0;

void jash_guard_metrics_record(int jash_native, int bypass, int blocked) {
    static int atexit_registered = 0;
    if (!atexit_registered) {
        atexit_registered = 1;
        (void)atexit(metrics_flush);
    }
    g_pend_total++;
    g_sess_total++;
    if (jash_native) { g_pend_native++;  g_sess_native++; }
    if (bypass)      { g_pend_bypass++;  g_sess_bypass++; }
    if (blocked)     { g_pend_blocked++; g_sess_blocked++; }
    if (g_pend_total >= 32) metrics_flush();
}

void jash_guard_metrics_upgraded(void) {
    g_pend_upgraded++;
    g_sess_upgraded++;
}

void jash_guard_metrics_session(unsigned* total, unsigned* native,
                                unsigned* bypass, unsigned* blocks,
                                unsigned* upgraded) {
    if (total)    *total    = g_sess_total;
    if (native)   *native   = g_sess_native;
    if (bypass)   *bypass   = g_sess_bypass;
    if (blocks)   *blocks   = g_sess_blocked;
    if (upgraded) *upgraded = g_sess_upgraded;
}

void jash_guard_metrics_snapshot(unsigned* total, unsigned* native,
                                 unsigned* bypass, unsigned* blocks,
                                 unsigned* upgraded) {
    metrics_read(total, native, bypass, blocks, upgraded);
}

static int bypass_mode_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    int n = snprintf(b, cap, "%s/.jash/agent_guard_mode", h);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return 0; }
    return 1;
}

static int bypass_mode_file_read(char* mode) {
    char p[600];
    if (!bypass_mode_path(p, sizeof p)) return 0;
    FILE* f = fopen(p, "r");
    if (!f) return 0;
    char v[16] = {0};
    char* r = fgets(v, sizeof v, f);
    (void)fclose(f);
    if (!r) return 0;
    if (mode) *mode = v[0];
    return 1;
}

/* PERF (bench-loss root cause #2): the mode file was fopen()ed TWICE per
 * evaluated command (bypass_active + bypass_strict) — even its ENOENT miss
 * costs a full path resolution per call. Cache the read behind a 500ms
 * monotonic TTL: cross-process toggles still land within half a second,
 * tight loops stop hitting the filesystem. jash_guard_bypass_set()
 * invalidates in-process. F E R M I ∞ H A R T */
static int  g_mode_cache_present = -1;   /* -1 = never read */
static char g_mode_cache_val = 0;
static unsigned long long g_mode_cache_ms = 0;

static void bypass_mode_cache_invalidate(void) { g_mode_cache_present = -1; }

static int bypass_mode_file(char* mode) {
    struct timespec ts;
    unsigned long long now = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        now = (unsigned long long)ts.tv_sec * 1000ull
            + (unsigned long long)ts.tv_nsec / 1000000ull;
    if (g_mode_cache_present < 0 || now - g_mode_cache_ms > 500) {
        char m = 0;
        g_mode_cache_present = bypass_mode_file_read(&m);
        g_mode_cache_val = m;
        g_mode_cache_ms = now;
    }
    if (g_mode_cache_present && mode) *mode = g_mode_cache_val;
    return g_mode_cache_present;
}

int jash_guard_bypass_active(void) {
    const char* a = getenv("JASH_AGENT");
    if (a && (a[0] == '1' || !strcasecmp(a, "true"))) return 1;
    const char* b = getenv("JASH_GUARD_BYPASS");
    if (b && (!strcmp(b, "1") || !strcasecmp(b, "true") ||
              !strcasecmp(b, "yes") || !strcasecmp(b, "on")))
        return 1;
    if (b && (!strcmp(b, "0") || !strcasecmp(b, "false") ||
              !strcasecmp(b, "no") || !strcasecmp(b, "off")))
        return 0;
    char mode = 0;
    if (bypass_mode_file(&mode)) {
        if (mode == 'w' || mode == 's') return 1;
    }
    return 0;
}

int jash_guard_bypass_strict(void) {
    const char* s = getenv("JASH_GUARD_BYPASS_STRICT");
    if (s && (!strcmp(s, "1") || !strcasecmp(s, "true") ||
              !strcasecmp(s, "yes") || !strcasecmp(s, "on")))
        return 1;
    if (s && (!strcmp(s, "0") || !strcasecmp(s, "false") ||
              !strcasecmp(s, "no") || !strcasecmp(s, "off")))
        return 0;
    char mode = 0;
    if (bypass_mode_file(&mode)) return mode == 's';
    return 0;
}

int jash_guard_bypass_set(int on, int strict) {
    char p[600];
    if (!bypass_mode_path(p, sizeof p)) return -1;
    if (!on && !strict) {
        int result = unlink(p);
        bypass_mode_cache_invalidate();
        return result && errno != ENOENT ? -1 : 0;
    }
    FILE* f = fopen(p, "w");
    if (!f) return -1;
    int failed = fputs(strict ? "strict\n" : (on ? "watch\n" : "off\n"), f) == EOF;
    int saved = failed ? errno : 0;
    if (fclose(f)) { failed = 1; if (!saved) saved = errno; }
    bypass_mode_cache_invalidate();
    if (failed) { errno = saved ? saved : EIO; return -1; }
    return 0;
}

int jash_guard_bypass_detect(const char* cmd, const char* args,
                             jash_writer_t w, void* ctx) {
    (void)w; (void)ctx;  /* BUG-148: never write diagnostics to the caller's
                          * writer — in $(cmdsub) it is the CAPTURE writer and
                          * guard text would contaminate the captured value.
                          * All guard/governor text goes to real stderr. */
    if (!jash_guard_bypass_active()) return 0;
    int native = jash_diff_is_native(cmd);
    int bypass = is_bypass_shell(cmd) && has_bypass_flag(args);

    if (bypass) {
        jash_guard_metrics_record(native, 1, jash_guard_bypass_strict());
        if (jash_guard_bypass_strict()) {
            jash_writes(jash_stderr_writer, 0,
                "[agent-guard] BLOCKED: agent attempted to bypass jash via ");
            jash_writes(jash_stderr_writer, 0, cmd);
            jash_writes(jash_stderr_writer, 0, " -c. Use jash primitives or set JASH_GUARD_BYPASS_STRICT=0.\n");
            bypass_suggest(jash_stderr_writer, 0, cmd, args);
            return 1;
        }
        jash_writes(jash_stderr_writer, 0,
            "[agent-guard] WARNING: agent bypassed jash via ");
        jash_writes(jash_stderr_writer, 0, cmd);
        jash_writes(jash_stderr_writer, 0, " -c. Consider rewriting with jash primitives.\n");
        bypass_suggest(jash_stderr_writer, 0, cmd, args);
        bypass_cost_nudge(args);
        return 0;
    }
    jash_guard_metrics_record(native, 0, 0);
    return 0;
}

void jash_guard_agent_status(jash_writer_t w, void* ctx, unsigned* total,
                             unsigned* native, unsigned* bypass, unsigned* blocks) {
    unsigned upgraded = 0;
    metrics_read(total, native, bypass, blocks, &upgraded);
    int adoption = *total ? (int)(((*native) * 100 + *total/2) / *total) : 0;
    int bypass_pct = *total ? (int)(((*bypass) * 100 + *total/2) / *total) : 0;
    jash_writes(w, ctx, "\033[1;36m═══ agent guard status ═══\033[0m\r\n");
    jash_writef(w, ctx, "  commands recorded   %u\r\n", *total);
    jash_writef(w, ctx, "  jash-native         %u (%d%% adoption · %u shell-upgraded)\r\n",
                *native, adoption, upgraded);
    jash_writef(w, ctx, "  bypass attempts     %u (%d%%)\r\n", *bypass, bypass_pct);
    jash_writef(w, ctx, "  blocked             %u\r\n", *blocks);
    jash_writef(w, ctx, "  mode                %s\r\n",
                jash_guard_bypass_strict() ? "STRICT (blocks bypasses)" :
                (jash_guard_bypass_active() ? "WATCH (warns only)" : "OFF"));
}

int jash_guard_active(void) {
    const char* g = getenv("JASH_GUARD");
    if (g && g[0] == '1') return 1;
    const char* s = getenv("JASH_SIGIL");
    if (s && atoi(s) >= 1) return 1;
    return 0;
}

/* ── ADR-0023 E2 / BUG-027: destructive-op protection toggle ──────────────── */

static int trash_mode_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    snprintf(b, cap, "%s/.jash/trash_mode", h);
    return 1;
}

/* -1 = unset, 0 = off, 1 = on */
static int trash_mode_file_get(void) {
    char p[600];
    if (!trash_mode_path(p, sizeof p)) return -1;
    FILE* f = fopen(p, "r");
    if (!f) return -1;
    char v[16] = {0};
    char* r = fgets(v, sizeof v, f);
    (void)fclose(f);
    if (!r) return -1;
    if (v[0] == 'o' && v[1] == 'n') return 1;
    if (v[0] == 'o' && v[1] == 'f') return 0;
    return -1;
}

static int trash_env_get(void) {
    const char* e = getenv("JASH_TRASH");
    if (!e || !*e) return -1;
    /* BUG-150: "off" used to match the 'o' of "on" and parse as ON — check
     * the off-family FIRST, then the on-family single-letter prefixes. */
    if ((e[0]=='o'||e[0]=='O') && (e[1]=='f'||e[1]=='F')) return 0;
    if (e[0]=='1'||e[0]=='o'||e[0]=='O'||e[0]=='t'||e[0]=='T'||e[0]=='y'||e[0]=='Y') return 1;
    if (e[0]=='0'||e[0]=='f'||e[0]=='F'||e[0]=='n'||e[0]=='N') return 0;
    return -1;
}

int jash_trash_source(void) {
    if (trash_mode_file_get() >= 0) return 0;   /* explicit toggle */
    if (trash_env_get()       >= 0) return 1;   /* env */
    return 2;                                   /* sigil default */
}

int jash_trash_protected(void) {
    int v = trash_mode_file_get();      /* 1. explicit user toggle wins */
    if (v >= 0) return v;
    /* 1b. ADR-0040 W3: destructive-op floor — aggressive adoption (level 3,
     * explicit JASH_ADOPT=3 / persisted mode only) pins rm & peers to the
     * recoverable path even when JASH_TRASH=off is inherited from the env.
     * The explicit `trash off` toggle above still wins: the floor guards
     * against inherited env, not against the user's own hand. */
    if (jash_adopt_level() >= 3) return 1;
    v = trash_env_get();                /* 2. env override */
    if (v >= 0) return v;
    /* 3. default: protected whenever recording is on. Use the RUNTIME sigil level
     * (jash_sigil_get), not getenv — recording can be auto-enabled at runtime for
     * non-interactive/agent sessions without JASH_SIGIL being set in the env, and
     * the safety floor must follow the recording, not the raw env var. */
    return jash_sigil_get() >= SIGIL_LOG ? 1 : 0;
}

int jash_trash_set(int on) {
    char p[600];
    if (!trash_mode_path(p, sizeof p)) return -1;
    if (on < 0) { unlink(p); return 0; }    /* clear → revert to default */
    FILE* f = fopen(p, "w");
    if (!f) return -1;
    (void)fputs(on ? "on\n" : "off\n", f);
    (void)fclose(f);
    return 0;
}

static int is_git_repo_root(const char* path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s/.git", path);
    struct stat st;
    return stat(buf, &st) == 0;
}

// Walk up looking for a .git directory; returns 1 if found within 8 levels.
static int inside_git_repo(const char* path) {
    char buf[4096];
    snprintf(buf, sizeof(buf), "%s", path);
    for (int i = 0; i < 8; i++) {
        if (is_git_repo_root(buf)) return 1;
        char* slash = strrchr(buf, '/');
        if (!slash || slash == buf) break;
        *slash = '\0';
    }
    return 0;
}

static int is_strict(void) {
    const char* s = getenv("JASH_GUARD_STRICT");
    return s && s[0] == '1';
}

int jash_guard_rm(const char* path, int recursive, jash_writer_t w, void* ctx) {
    if (!jash_guard_active()) return 0;
    (void)recursive;

    // Protect the jash binary itself.
    char real[4096] = {0};
    if (!realpath(path, real)) real[0] = '\0';  /* unresolvable = not the binary */
    const char* jash_bin = "/usr/local/bin/jash";
    if (strcmp(real, jash_bin) == 0 || strcmp(path, jash_bin) == 0) {
        jash_writes(w, ctx, "[guard] BLOCKED: attempt to delete the jash binary\n");
        return 1;
    }
    // Check for resolved symlink target too.
    char link_target[4096] = {0};
    ssize_t n = readlink(jash_bin, link_target, sizeof(link_target) - 1);
    if (n > 0 && strcmp(real, link_target) == 0) {
        jash_writes(w, ctx, "[guard] BLOCKED: attempt to delete the jash binary target\n");
        return 1;
    }

    // ADR-0042 F9: another live agent's file intent-lock (advisory / strict).
    // Placed before the git-repo branches so it always runs (those early-return).
    if (jash_guard_claim(path, w, ctx)) return 1;

    if (is_git_repo_root(path)) {
        if (is_strict()) {
            jash_writes(w, ctx, "[guard] BLOCKED: rm -r on a git repo root — set JASH_GUARD_STRICT=0 to override\n");
            return 1;
        }
        jash_writes(w, ctx, "[guard] WARNING: deleting a git repo root — all history will be lost. Use rm --undo or push first.\n");
        return 0;
    }

    if (inside_git_repo(path)) {
        if (is_strict()) {
            jash_writes(w, ctx, "[guard] BLOCKED: rm inside a git repo — set JASH_GUARD_STRICT=0 to override\n");
            return 1;
        }
        jash_writes(w, ctx, "[guard] WARNING: deleting files inside a git repo. Consider git rm or rm --undo.\n");
        return 0;
    }

    return 0;
}

int jash_guard_git(const char* args, jash_writer_t w, void* ctx) {
    if (!jash_guard_active() || !args) return 0;

    // Dangerous subcommands that modify/lose history.
    const char* dangerous[][2] = {
        {"reset --hard",  "[guard] WARNING: git reset --hard discards uncommitted changes.\n"},
        {"reset --mixed", "[guard] WARNING: git reset --mixed unstages and may lose work.\n"},
        {"clean -f",      "[guard] WARNING: git clean -f permanently deletes untracked files.\n"},
        {"checkout --",   "[guard] WARNING: git checkout -- discards working-tree changes.\n"},
        {"push --force",  "[guard] WARNING: git push --force rewrites remote history.\n"},
        {"push -f",       "[guard] WARNING: git push -f rewrites remote history.\n"},
        {"restore",       NULL},  // handled separately
        {NULL, NULL}
    };

    // "restore --staged" is safe; bare "restore <path>" isn't.
    if (strncmp(args, "restore", 7) == 0) {
        const char* rest = args + 7;
        while (*rest == ' ') rest++;
        if (strncmp(rest, "--staged", 8) != 0) {
            if (is_strict()) {
                jash_writes(w, ctx, "[guard] BLOCKED: git restore discards working-tree changes. Use --staged or set JASH_GUARD_STRICT=0.\n");
                return 1;
            }
            jash_writes(w, ctx, "[guard] WARNING: git restore discards working-tree changes.\n");
        }
        return 0;
    }

    for (int i = 0; dangerous[i][0]; i++) {
        if (dangerous[i][1] && strncmp(args, dangerous[i][0], strlen(dangerous[i][0])) == 0) {
            if (is_strict()) {
                jash_writes(w, ctx, "[guard] BLOCKED: dangerous git command. Set JASH_GUARD_STRICT=0 to override.\n");
                return 1;
            }
            jash_writes(w, ctx, dangerous[i][1]);
            return 0;
        }
    }
    return 0;
}

/* ── ADR-0042 F9: file intent-lock enforcement ──────────────────────────────
 * Before a mutating op (rm/mv) on `path`, consult the mesh claim store: if a
 * *live* claim by a DIFFERENT agent holds it, warn (advisory) or block
 * (JASH_CLAIM_STRICT=1). The conflict is noted for the JASH_AGENT envelope
 * ("claim_conflicts"). Diagnostics go to real stderr only (BUG-148: `w`/`ctx`
 * may be a $(cmdsub) capture writer). `self` is $JASH_AGENT_ID (never the
 * generated mesh id — an anonymous session must be warned about foreign
 * claims, not silently matched). Returns 1 to block, 0 to proceed. */
int jash_guard_claim(const char* path, jash_writer_t w, void* ctx) {
    (void)w; (void)ctx;
    if (!path || !*path) return 0;
    if (!jash_guard_active()) return 0;
    const char* off = getenv("JASH_CLAIM");
    if (off && off[0] == '0') return 0;                 /* hard off-switch */
    char norm[JASH_CLAIM_PATH_MAX];
    if (!jash_claims_norm(path, norm, sizeof norm)) return 0;
    const char* self = getenv("JASH_AGENT_ID");
    char owner[JASH_CLAIM_OWNER_MAX] = "";
    long ttl = 0;
    int conflict = jash_claims_conflict(norm, self ? self : "", owner, sizeof owner, &ttl);
    if (!conflict) return 0;
    if (conflict < 0) {
        int error = errno;
        const char* strict = getenv("JASH_CLAIM_STRICT");
        int blocked = strict && strict[0] == '1';
        jash_writef(jash_stderr_writer, 0,
            "[mesh-claim] %s: claim store could not be inspected for '%s': %s\n",
            blocked ? "BLOCKED" : "WARNING", norm, strerror(error));
        return blocked;
    }
    jash_claims_note_conflict(norm);
    const char* s = getenv("JASH_CLAIM_STRICT");
    if (s && s[0] == '1') {
        jash_writef(jash_stderr_writer, 0,
            "[mesh-claim] BLOCKED: '%s' is claimed by %s (live, ttl=%lds) — "
            "coordinate or set JASH_CLAIM_STRICT=0 to override\n", norm, owner, ttl);
        return 1;
    }
    jash_writef(jash_stderr_writer, 0,
        "[mesh-claim] WARNING: '%s' is claimed by %s — another agent intends to "
        "work on it (mesh claims to inspect)\n", norm, owner);
    return 0;
}
