// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/preexec — pre-exec absorption engine (ADR-0040 W3)
//
// At adoption level >= 2 (explicit intent only — core/adopt.h caps inference
// at level 1) a simple command `bash -c 'STR'` (sh/zsh/dash/ksh alike) is
// absorbed: argv[0] is rewritten to the running jash binary and the normal
// external-exec machinery carries on unchanged. The child keeps `bash -c`
// process semantics exactly (own cwd/env/umask, same fork/fds/wait path)
// while regaining recording, guard, undo and attestation.
//
// Termination is structural: each rewrite strips exactly one `<shell> -c`
// wrapper, so nested payloads shrink monotonically — no depth counter.
//
// In JASH_GUARD_BYPASS_STRICT sessions absorption runs BEFORE the bypass
// guard: the escape is neutralized instead of punished, so the command runs
// (under jash) where it would previously have been blocked with rc 126.
//
// JASH_ABSORB=0 disables the engine (escape hatch, mirrors JASH_MEMO).

#include "preexec.h"
#include "adopt.h"
#include "guard.h"
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

static const char* base_name(const char* cmd) {
    const char* b = cmd;
    for (const char* p = cmd; *p; p++) if (*p == '/') b = p + 1;
    return b;
}

static int is_absorbable_shell(const char* cmd) {
    const char* b = base_name(cmd);
    return strcmp(b, "sh") == 0 || strcmp(b, "bash") == 0 ||
           strcmp(b, "zsh") == 0 || strcmp(b, "dash") == 0 ||
           strcmp(b, "ksh") == 0;
}

/* args must be exactly `-c PAYLOAD` — one payload token (quote-aware), no
 * trailing positionals ($0 $1 ...), no other flags. Anything else falls
 * through to the real shell: absorption must never change semantics it
 * does not fully own. Returns 1 when absorbable. */
static int args_absorbable(const char* args) {
    if (!args) return 0;
    const char* p = args;
    while (*p == ' ' || *p == '\t') p++;
    if (p[0] != '-' || p[1] != 'c') return 0;
    p += 2;
    if (*p != ' ' && *p != '\t') return 0;       /* -lc / -ce etc. → no */
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;                           /* empty payload */
    int in_s = 0, in_d = 0;
    while (*p) {
        char c = *p;
        if (in_s) { if (c == '\'') in_s = 0; }
        else if (in_d) {
            if (c == '\\' && p[1]) p++;
            else if (c == '"') in_d = 0;
        } else {
            if (c == '\'') in_s = 1;
            else if (c == '"') in_d = 1;
            else if (c == '\\' && p[1]) p++;
            else if (c == ' ' || c == '\t') break;  /* end of payload token */
        }
        p++;
    }
    if (in_s || in_d) return 0;      /* unterminated — let the real shell err */
    while (*p == ' ' || *p == '\t') p++;
    return *p == '\0';               /* trailing tokens become $0/$1 → no */
}

static int self_path(char* out, unsigned outsz) {
#ifdef __APPLE__
    uint32_t sz = outsz;
    if (_NSGetExecutablePath(out, &sz) != 0) return 0;
    return out[0] == '/';
#else
    ssize_t n = readlink("/proc/self/exe", out, (size_t)outsz - 1);
    if (n <= 0) return 0;
    out[n] = '\0';
    return 1;
#endif
}

int jash_preexec_will_absorb(const char* cmd, const char* args) {
    const char* e = getenv("JASH_ABSORB");
    if (e && e[0] == '0') return 0;
    if (jash_adopt_level() < 2) return 0;
    if (!cmd || !is_absorbable_shell(cmd)) return 0;
    return args_absorbable(args);
}

/* ADR-0040 W8: calibration gap. At level 1 the guard observes but never
 * rewrites (inference-free rule for levels 2/3, adopt.c). Count the
 * one-liners that WOULD have been absorbed at level 2 so the scorecard can
 * turn measurement into a concrete suggestion instead of a vibe. Level 0 is
 * an explicit opt-out — never counted, never nagged. */
static unsigned g_missed_absorb = 0;

void jash_preexec_note_missed(const char* cmd, const char* args) {
    const char* e = getenv("JASH_ABSORB");
    if (e && e[0] == '0') return;            /* absorption disabled: no nag */
    if (jash_adopt_level() != 1) return;
    if (!cmd || !is_absorbable_shell(cmd)) return;
    if (!args_absorbable(args)) return;
    g_missed_absorb++;
}

unsigned jash_preexec_missed_count(void) { return g_missed_absorb; }

/* ADR-0040 W3: level 3's own policy — pipeline segments. Level 2 owns only
 * top-level simple commands; a segment rewrite also touches the caller's
 * pipe/fd plumbing, so it demands the strongest explicit intent (3). The
 * payload predicate is the SAME (`-c` + one token): what changes is where
 * the rewrite may happen, not what it may swallow. */
static int will_absorb_seg(const char* cmd, const char* args) {
    const char* e = getenv("JASH_ABSORB");
    if (e && e[0] == '0') return 0;
    if (jash_adopt_level() < 3) return 0;
    if (!cmd || !is_absorbable_shell(cmd)) return 0;
    return args_absorbable(args);
}

int jash_preexec_absorb_seg(const char* cmd, const char* args,
                            char* self, unsigned selfsz) {
    if (!will_absorb_seg(cmd, args)) return 0;
    if (!self_path(self, selfsz)) return 0;
    jash_guard_metrics_record(1, 0, 0);
    jash_guard_metrics_upgraded();
    static int noticed_seg = 0;                  /* once per process */
    if (!noticed_seg) {
        noticed_seg = 1;
        int pr = fprintf(stderr, "[jash adopt] %s -c pipeline segment "
                         "absorbed \xe2\x86\x92 jash -c (level 3; "
                         "JASH_ABSORB=0 disables)\n", base_name(cmd));
        (void)pr;
    }
    return 1;
}

void jash_preexec_note_missed_seg(const char* cmd, const char* args) {
    const char* e = getenv("JASH_ABSORB");
    if (e && e[0] == '0') return;
    int lv = jash_adopt_level();
    if (lv < 1 || lv > 2) return;            /* 0 = opt-out, 3 = absorbed */
    if (!cmd || !is_absorbable_shell(cmd)) return;
    if (!args_absorbable(args)) return;
    g_missed_absorb++;
}

int jash_preexec_absorb(const char* cmd, const char* args,
                        char* self, unsigned selfsz) {
    if (!jash_preexec_will_absorb(cmd, args)) return 0;
    if (!self_path(self, selfsz)) return 0;
    /* Metrics: the absorbed command is native (it runs under jash) AND
     * shell-assisted — `upgraded` splits the two (core/guard.h). The bypass
     * guard is skipped for absorbed commands, so record the event here. */
    jash_guard_metrics_record(1, 0, 0);
    jash_guard_metrics_upgraded();
    static int noticed = 0;                      /* once per process */
    if (!noticed) {
        noticed = 1;
        /* BUG-148 discipline: the REAL stderr, never the caller's writer —
         * $( ) captures must stay byte-identical. */
        int pr = fprintf(stderr, "[jash adopt] %s -c absorbed \xe2\x86\x92 "
                         "jash -c (level %d; JASH_ABSORB=0 disables)\n",
                         base_name(cmd), jash_adopt_level());
        (void)pr;
    }
    return 1;
}
