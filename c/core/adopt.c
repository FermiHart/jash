// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/adopt — adoption governor level resolver (ADR-0040 W2)
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "adopt.h"
#include "repo_identity.h"
#include <core/agent_json.h> /* jash_agent_last_absorbed (ADR-0040 W4) */
#include <core/guard.h>   /* jash_guard_metrics_snapshot */
#include <core/memo.h>    /* jash_env_smells_like_agent */
#include <core/preexec.h> /* jash_preexec_missed_count (ADR-0040 W8) */
#include <theme/io.h>     /* jash_stderr_writer, jash_writes */

#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

static int adopt_mode_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    int n = snprintf(b, cap, "%s/.jash/adopt_mode", h);
    if (n < 0 || (size_t)n >= cap) { errno = ENAMETOOLONG; return 0; }
    return 1;
}

static int telemetry_path(char* b, size_t cap) {
    const char* h = getenv("HOME");
    if (!h || !*h) return 0;
    snprintf(b, cap, "%s/.jash/adoption_sessions.tsv", h);
    return 1;
}

/* ADR-0040 W5: repo identity — the governor's evidence is earned per
 * project: promotion in repo A must not unlock rewrites in repo B, and a
 * block in B must not demote A. Key = FNV-1a64 (16 hex) of the nearest
 * ancestor directory containing .git; "-" outside any repo. Computed once
 * per process (a cd mid-session keeps the row attributed to where the
 * session started — one row, one key). Legacy rows without the column
 * count everywhere (conservative: old blocks still demote). */
const char* jash_repo_key(void) {
    static char key[18];
    if (!key[0]) jash_repo_identity(key);
    return key;
}

/* ADR-0040 W4: auto governor — resolve the level from cross-session
 * telemetry. Explicit opt-in (JASH_ADOPT=auto), so reaching 2/3 here does
 * not violate the "only explicit intent reaches rewrites" invariant: the
 * user delegated the decision, the ladder just makes it evidence-based.
 * Conservative ladder over the last 20 session rows:
 *   base 1 (nudges)  — always safe, same ceiling as inference;
 *   → 2 when ≥3 sessions and ≥30 cmds in the window, ZERO blocks, ZERO
 *       absorb failures, bypass ≤10%, and there is rewrite evidence
 *       (missed + upgraded + absorbed > 0 — no evidence, no rewrites);
 *   → 3 when additionally ≥8 sessions, ≥100 cmds, ≥5 upgraded+absorbed
 *       and bypass ≤5%;
 *   any block or absorb failure in the LAST 3 sessions → back to 1
 *   (recent regression outweighs any accumulated record). */
static int adopt_auto_level(void) {
    char p[600];
    if (!telemetry_path(p, sizeof p)) return 1;
    FILE* f = fopen(p, "r");
    if (!f) return 1;                       /* no history → nudges only */
    struct arow { unsigned t, b, bl, up, ab, abf, miss; };
    struct arow ring[20];
    int rn = 0, ri = 0;
    const char* ck = jash_repo_key();     /* ADR-0040 W5 */
    char line[256];
    while (fgets(line, sizeof line, f)) {
        struct arow r = {0, 0, 0, 0, 0, 0, 0};
        unsigned n = 0;
        char rk[17];
        rk[0] = 0;
        if (sscanf(line, "%*s\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%u\t%*u\t%16s",
                   &r.t, &n, &r.b, &r.bl, &r.up, &r.ab, &r.abf,
                   &r.miss, rk) >= 4) {
            /* W5: keyed row from another repo → not our evidence. */
            if (rk[0] && strcmp(rk, ck) != 0) continue;
            ring[ri] = r;
            ri = (ri + 1) % 20;
            if (rn < 20) rn++;
        }
    }
    int rc0 = fclose(f); (void)rc0;
    if (!rn) return 1;
    unsigned t = 0, b = 0, bl = 0, up = 0, ab = 0, abf = 0, miss = 0;
    for (int i = 0; i < rn; i++) {
        t  += ring[i].t;  b   += ring[i].b;   bl   += ring[i].bl;
        up += ring[i].up; ab  += ring[i].ab;  abf  += ring[i].abf;
        miss += ring[i].miss;
    }
    /* recent window: the 3 most recently written rows */
    for (int k = 1; k <= 3 && k <= rn; k++) {
        int i = (ri - k + 20) % 20;
        if (ring[i].bl || ring[i].abf) return 1;
    }
    int lvl = 1;
    if (rn >= 3 && t >= 30 && bl == 0 && abf == 0 &&
        b * 10 <= t && (miss + up + ab) > 0)
        lvl = 2;
    if (lvl == 2 && rn >= 8 && t >= 100 && (up + ab) >= 5 && b * 20 <= t)
        lvl = 3;
    return lvl;
}

/* -1 = unset, else 0..3 */
static int adopt_mode_file_get(void) {
    char p[600];
    if (!adopt_mode_path(p, sizeof p)) return -1;
    FILE* f = fopen(p, "r");
    if (!f) return -1;
    char v[16] = {0};
    char* r = fgets(v, sizeof v, f);
    int mc = fclose(f); (void)mc;
    if (!r || v[0] < '0' || v[0] > '3') return -1;
    return v[0] - '0';
}

/* Same 500ms monotonic-TTL cache the guard mode file uses: the resolver is on
 * the W3 per-command path, and an ENOENT miss still costs a path resolution.
 * Cross-process `adopt` toggles land within half a second; jash_adopt_set()
 * invalidates in-process. */
static int g_adopt_level = -1;   /* -1 = never resolved */
static int g_adopt_src = 4;
static unsigned long long g_adopt_ms = 0;

static void adopt_cache_invalidate(void) { g_adopt_level = -1; }

static void adopt_resolve(void) {
    struct timespec ts;
    unsigned long long now = 0;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
        now = (unsigned long long)ts.tv_sec * 1000ull
            + (unsigned long long)ts.tv_nsec / 1000000ull;
    if (g_adopt_level >= 0 && now - g_adopt_ms <= 500) return;
    g_adopt_ms = now;

    /* 1. recording explicitly off → governor off. */
    const char* s = getenv("JASH_SIGIL");
    if (s && s[0] == '0' && !s[1]) { g_adopt_level = 0; g_adopt_src = 0; return; }

    /* 2. explicit env — the ONLY inference-free way to reach levels 2/3.
     * ADR-0040 W4: "auto" delegates the pick to the telemetry ladder —
     * still explicit intent, so 2/3 stay reachable. */
    const char* e = getenv("JASH_ADOPT");
    if (e && strcmp(e, "auto") == 0) {
        g_adopt_level = adopt_auto_level();
        g_adopt_src = 5;
        return;
    }
    if (e && e[0] >= '0' && e[0] <= '9') {
        int v = atoi(e);
        if (v < 0) v = 0;
        if (v > 3) v = 3;
        g_adopt_level = v; g_adopt_src = 1; return;
    }

    /* 3. persisted `adopt` toggle. */
    int f = adopt_mode_file_get();
    if (f >= 0) { g_adopt_level = f; g_adopt_src = 2; return; }

    /* 4. inferred agent → nudges only, NEVER rewrites (protects test suites
     * that inherit agent-ish env from the parent Claude Code session). */
    if (jash_env_smells_like_agent()) { g_adopt_level = 1; g_adopt_src = 3; return; }

    g_adopt_level = 0; g_adopt_src = 4;
}

int jash_adopt_level(void)  { adopt_resolve(); return g_adopt_level; }
int jash_adopt_source(void) { adopt_resolve(); return g_adopt_src; }

const char* jash_adopt_source_name(int src) {
    switch (src) {
        case 0: return "sigil-off";
        case 1: return "env JASH_ADOPT";
        case 2: return "adopt toggle (~/.jash/adopt_mode)";
        case 3: return "agent inference";
        case 5: return "auto governor";
        default: return "default";
    }
}

/* ADR-0040 W4: append this session's summary row (see header). */
void jash_adopt_telemetry_flush(void) {
    int lv = jash_adopt_level();
    if (lv < 1) return;
    /* Absorbed children (spool in environ, not the owner) must NOT write a
     * row: their rc/ms already roll up into the parent's fold, and a row per
     * child would double-count the session (same class as the W2 envelope
     * duplication). */
    const char* sp = getenv("JASH_ABSORB_SPOOL");
    if (sp && *sp && !jash_agent_spool_is_owner()) return;
    unsigned t = 0, n = 0, b = 0, bl = 0, up = 0;
    jash_guard_metrics_session(&t, &n, &b, &bl, &up);
    int ab = 0, abf = 0;
    jash_agent_last_absorbed(&ab, &abf);
    unsigned missed = jash_preexec_missed_count();
    if (!t && !ab && !up && !missed) return;   /* nothing measured */
    char p[600];
    if (!telemetry_path(p, sizeof p)) return;
    FILE* f = fopen(p, "a");
    if (!f) return;
    int pr = fprintf(f, "%lld\t%u\t%u\t%u\t%u\t%u\t%d\t%d\t%u\t%d\t%s\n",
                     (long long)time(NULL), t, n, b, bl, up, ab, abf,
                     missed, lv, jash_repo_key());   /* ADR-0040 W5 */
    long sz = ftell(f);
    int fr = fclose(f); (void)pr; (void)fr;
    /* Self-cap: ~32 KB (≈500+ rows) → rewrite keeping the last 200. The
     * ladder only ever reads the last 20; 200 keeps forensics cheap. */
    if (sz < 32768) return;
    f = fopen(p, "r");
    if (!f) return;
    char (*keep)[256] = calloc(200, 256);
    if (!keep) { int ec = fclose(f); (void)ec; return; }
    int kn = 0, ki = 0;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        int sn = snprintf(keep[ki], 256, "%s", line); (void)sn;
        ki = (ki + 1) % 200;
        if (kn < 200) kn++;
    }
    int kc = fclose(f); (void)kc;
    char tmp[620];
    snprintf(tmp, sizeof tmp, "%s.tmp", p);
    f = fopen(tmp, "w");
    if (f) {
        for (int k = kn; k >= 1; k--) {
            int i = (ki - k + 200) % 200;
            int fp = fputs(keep[i], f); (void)fp;
        }
        int cr = fclose(f); (void)cr;
        int rr = rename(tmp, p); (void)rr;
    }
    free(keep);
}

int jash_adopt_set(int level) {
    char p[600];
    if (!adopt_mode_path(p, sizeof p)) return -1;
    if (level < 0) {
        int result = unlink(p);
        adopt_cache_invalidate();
        return result && errno != ENOENT ? -1 : 0;
    }
    if (level > 3) level = 3;
    FILE* f = fopen(p, "w");
    if (!f) return -1;
    int failed = fprintf(f, "%d\n", level) < 0;
    int saved = failed ? errno : 0;
    if (fclose(f)) { failed = 1; if (!saved) saved = errno; }
    adopt_cache_invalidate();
    if (failed) { errno = saved ? saved : EIO; return -1; }
    return 0;
}

int jash_adopt_scorecard_maybe(void) {
    /* JINV-059: JASH_NUDGE e o interruptor UNICO de todas as dicas — o proprio
     * hints.c:23 diz "one kill switch for ALL tips". Este scorecard escapava
     * dele, e por isso disparava dentro de gates que ja pediam silencio.
     *
     * Nao e cosmetico: ele acorda a cada 10o comando (`t % 10`) com contador
     * PERSISTENTE entre invocacoes, e vai para o stderr real. Um gate que faz
     * `2>&1` passava ou falhava conforme o contador global caisse no multiplo
     * de 10 — ou seja, passava por SORTE. O JINV-019 viveu assim desde que
     * foi escrito e so caiu quando a adocao subiu de 2% para 3%.
     *
     * Um gate que passa por sorte e pior que gate nenhum: produz confianca
     * sem sustentacao, que e a mesma falha do JINV-024 e do JINV-051. */
    { const char* e = getenv("JASH_NUDGE");
      if (e && e[0] == '0' && e[1] == '\0') return 0; }
    if (jash_adopt_level() < 1) return 0;

    /* Opportunity adoption (session-scoped). The old line divided the 21
     * differentiator names by EVERY evaluated command (ls/git/make/echo...),
     * a population that can never be native — a structural ~4% ceiling that
     * punished correct usage. Worse, it read the LIFETIME counter: when that
     * counter froze on a multiple of 10 (metrics off in -c), `t % 10 == 0`
     * held forever and the same stale line printed on EVERY invocation.
     *
     * The denominator is now the opportunity space of THIS session:
     *   used  = native (already includes absorbed one-liners)
     *   opp   = used + bypass + missed-absorbable
     *   pct   = used / opp
     * Absorbed commands also bump `upgraded` (preexec.c), so upgraded is a
     * subset of native, never added twice. */
    unsigned t = 0, n = 0, b = 0, bl = 0, up = 0;
    jash_guard_metrics_session(&t, &n, &b, &bl, &up);
    (void)bl;
    unsigned missed = jash_preexec_missed_count();
    unsigned used = n;
    unsigned opp = used + b + missed;
    if (!t || !opp) return 0;
    /* Cadence: interactive sessions (t >= 10) fire every 10th command.
     * One-shot -c invocations speak only when the invocation itself touched
     * the opportunity space — nothing actionable, nothing printed. */
    if (t >= 10 && t % 10 != 0) return 0;
    int pct = (int)((used * 100 + opp / 2) / opp);
    unsigned typed = used >= up ? used - up : 0;
    char line[256];
    int o = snprintf(line, sizeof line,
        "[jash] adoption %d%% this session (%u/%u opportunities: %u native "
        "\xc2\xb7 %u shell-upgraded \xc2\xb7 %u bypass \xc2\xb7 %u cmds) "
        "\xe2\x80\x94 what you're missing: jash --agent-guide\n",
        pct, used, opp, typed, up, b, t);
    if (o <= 0) return 0;
    /* BUG-148: governor text goes to the REAL stderr, never a capture writer. */
    jash_writes(jash_stderr_writer, 0, line);
    /* ADR-0040 W8: calibration suggestion — only when there is a measured
     * gap, and only at level 1 (note_missed never counts at 0/2/3). */
    if (missed > 0) {
        char m[192];
        int mo = snprintf(m, sizeof m,
            "[jash] %u absorbable one-liner%s ran as raw bash \xe2\x80\x94 "
            "`adopt rewrite` (level 2) runs them natively, kill-switch: "
            "JASH_ABSORB=0\n",
            missed, missed == 1 ? "" : "s");
        if (mo > 0) jash_writes(jash_stderr_writer, 0, m);
    }
    return 1;
}
