// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/expect — contracts the shell enforces (ADR-0023 E3).
//
// `expect <predicate>` asserts a fact about current state and fails LOUDLY when
// the agent's mental model is wrong — turning silent hallucination into a hard,
// readable stop. The value over `test`/`[` is the explicit verdict line an agent
// can read back: "expect: FAILED — <what> (actual: <state>)".
//
//   expect file PATH        PATH exists (any type)
//   expect dir PATH         PATH is a directory
//   expect no PATH          PATH does NOT exist
//   expect exec PATH        PATH is an executable file
//   expect empty PATH       0-byte regular file
//   expect nonempty PATH    regular file, size > 0
//   expect contains PATH S  file PATH contains substring S
//   expect newer A B        A exists and is modified more recently than B
//   expect older A B        A exists and is older than B
//   expect match A B        A and B are byte-identical regular files
//   expect rc N             previous command exited with code N
//   expect not <predicate>  negate any of the above
//
// Compose with the shell: `make && expect newer build/app src/main.c && expect rc 0`.
//
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/agent_json.h>   /* jash_json_esc */
#include <core/adopt.h>        /* jash_repo_key (ADR-0052 W1) */
#include <sys/syscalls.h>      /* jash_tmpdir — J1 fix */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ADR-0052 W1: cross-call rc — the dominant agent pattern (sequential
 * `jash -c` calls) made `expect rc` lie in both directions: a fresh
 * process has last_status.code=0 default, so `expect rc 0` passed
 * VACUOUSLY and `expect rc 1` failed with "actual: rc 0" (reproduced).
 * The attested chain (ADR-0051) knows the last command's real rc.
 * Returns 1 when a chained rc exists, filling rc/seq/cmd. */
static int chain_last_rc(int* out_rc, unsigned long long* out_seq,
                         char* out_cmd, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !*home) return 0;
    char path[600];
    const char* k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(path, sizeof path, "%s/.jash/chain-%s.log", home, k);
    else
        snprintf(path, sizeof path, "%s/.jash/chain.log", home);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[1024], last[1024] = {0};
    while (fgets(line, sizeof line, f)) {
        if (strstr(line, " cmd=") && !strstr(line, " cmd=SESSION_END"))
            memcpy(last, line, sizeof last);
    }
    fclose(f);
    if (!last[0]) return 0;
    unsigned long long seq = 0, ts = 0, ns = 0;
    char hash[80];
    if (sscanf(last, "%llu %llu.%llu %79s", &seq, &ts, &ns, hash) < 4) return 0;
    int rc = 0;
    const char* rp = strstr(last, " rc=");
    if (!rp || sscanf(rp, " rc=%d", &rc) != 1) return 0;
    const char* cp = strstr(last, " cmd=");
    if (out_cmd && cp) {
        snprintf(out_cmd, cap, "%s", cp + 5);
        char* nl = strchr(out_cmd, '\n'); if (nl) *nl = '\0';
    }
    *out_rc = rc;
    *out_seq = seq;
    return 1;
}


/* Chunked substring search: each chunk keeps the trailing (nlen-1) bytes of the
 * previous one as a prefix, so a match straddling a read boundary is found. */
static int file_contains(const char* path, const char* needle) {
    size_t nlen = strlen(needle);
    if (nlen == 0) return 0;
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    enum { CHUNK = 65536 };
    size_t cap = CHUNK + nlen;
    char* win = (char*)malloc(cap);
    if (!win) { fclose(f); return 0; }
    size_t carry = 0;
    int found = 0;
    size_t n;
    while ((n = fread(win + carry, 1, CHUNK, f)) > 0) {
        size_t total = carry + n;
        if (memmem(win, total, needle, nlen)) { found = 1; break; }
        carry = (nlen - 1 < total) ? nlen - 1 : total;
        memmove(win, win + (total - carry), carry);
    }
    free(win);
    fclose(f);
    return found;
}

static int files_equal(const char* a, const char* b) {
    struct stat sa, sb;
    if (stat(a, &sa) != 0 || stat(b, &sb) != 0) return 0;
    if (!S_ISREG(sa.st_mode) || !S_ISREG(sb.st_mode)) return 0;
    if (sa.st_size != sb.st_size) return 0;
    FILE* fa = fopen(a, "rb"); if (!fa) return 0;
    FILE* fb = fopen(b, "rb"); if (!fb) { fclose(fa); return 0; }
    char ba[65536], bb[65536];
    int eq = 1; size_t na, nb;
    do {
        na = fread(ba, 1, sizeof ba, fa);
        nb = fread(bb, 1, sizeof bb, fb);
        if (na != nb || memcmp(ba, bb, na) != 0) { eq = 0; break; }
    } while (na > 0);
    fclose(fa); fclose(fb);
    return eq;
}

static int eval_pred_w(int argc, char** argv, jash_state_t* s,
                       jash_writer_t w, void* ctx,
                       char* desc, size_t dcap, char* actual, size_t acap);

/* ADR-0052 W4: failure with causal evidence. "actual: missing" tells the
 * agent WHAT is wrong; the chain knows WHEN it last held and WHICH command
 * touched it. Scans the chain for the newest entry mentioning the path
 * (reverse-chronological truth), returns 1 and fills seq/cmd. */
static int chain_last_touch(const char* needle, unsigned long long* out_seq,
                            char* out_cmd, size_t ccap) {
    const char* home = getenv("HOME");
    if (!home || !*home || !needle || !*needle) return 0;
    char path[600];
    const char* k = jash_repo_key();
    if (k && k[0] != '-')
        snprintf(path, sizeof path, "%s/.jash/chain-%s.log", home, k);
    else
        snprintf(path, sizeof path, "%s/.jash/chain.log", home);
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    unsigned long long best_seq = 0;
    int found = 0;
    char best_cmd[256] = "";
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        unsigned long long seq = 0, ts = 0, ns = 0;
        char hash[80];
        if (sscanf(line, "%llu %llu.%llu %79s", &seq, &ts, &ns, hash) < 4) continue;
        const char* cp = strstr(line, " cmd=");
        if (!cp || !strstr(cp + 5, needle)) continue;
        if (!found || seq > best_seq) {
            best_seq = seq;
            found = 1;
            snprintf(best_cmd, sizeof best_cmd, "%s", cp + 5);
            char* nl = strchr(best_cmd, '\n'); if (nl) *nl = '\0';
        }
    }
    fclose(f);
    if (!found) return 0;
    *out_seq = best_seq;
    if (out_cmd) snprintf(out_cmd, ccap, "%s", best_cmd);
    return 1;
}

/* ADR-0052 W3: quote-aware parse for `expect cmd "<cmd>" <assert> [arg]`.
 * The generic space-tokenizer would shred a multi-word command; the target
 * is a quoted string or a single bareword. Returns 1 and fills cmd/verb/arg
 * (verb/arg borrow from args), 0 on malformed input. */
static int expect_parse_cmd(const char* args, char* cmd, size_t ccap,
                            const char** verb, const char** arg) {
    const char* p = args;
    while (*p == ' ' || *p == '\t') p++;
    if (strncmp(p, "cmd", 3) != 0 || (p[3] != ' ' && p[3] != '\t')) return 0;
    p += 3;
    while (*p == ' ' || *p == '\t') p++;
    size_t n = 0;
    if (*p == '"' || *p == '\'') {
        char q = *p++;
        while (*p && *p != q && n + 1 < ccap) cmd[n++] = *p++;
        if (*p != q) return 0;
        p++;
    } else {
        while (*p && *p != ' ' && *p != '\t' && n + 1 < ccap) cmd[n++] = *p++;
    }
    cmd[n] = '\0';
    if (!n) return 0;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return 0;
    *verb = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (*p) { *(char*)p = '\0'; p++; }
    while (*p == ' ' || *p == '\t') p++;
    *arg = *p ? p : NULL;
    return 1;
}

/* Evaluate one predicate. Returns 1 (holds), 0 (violated), -1 (usage error).
 * Fills desc (the human assertion) and actual (the observed state on violation).
 * w/ctx pass through to jash_eval_line for `cmd` — a NULL writer breaks the
 * builtin redirect engine silently (same lesson as weave). */
static int eval_pred_w(int argc, char** argv, jash_state_t* s,
                       jash_writer_t w, void* ctx,
                       char* desc, size_t dcap, char* actual, size_t acap) {
    const char* verb = argc > 0 ? argv[0] : "";
    const char* a    = argc > 1 ? argv[1] : NULL;
    const char* b    = argc > 2 ? argv[2] : NULL;
    struct stat st, st2;

    if (!strcmp(verb, "file") || !strcmp(verb, "exists")) {
        if (!a) return -1;
        snprintf(desc, dcap, "%s exists", a);
        if (stat(a, &st) == 0) return 1;
        snprintf(actual, acap, "missing"); return 0;
    }
    if (!strcmp(verb, "dir")) {
        if (!a) return -1;
        snprintf(desc, dcap, "%s is a directory", a);
        if (stat(a, &st) == 0 && S_ISDIR(st.st_mode)) return 1;
        snprintf(actual, acap, "%s", stat(a, &st) ? "missing" : "not a directory"); return 0;
    }
    if (!strcmp(verb, "no") || !strcmp(verb, "absent")) {
        if (!a) return -1;
        snprintf(desc, dcap, "%s does not exist", a);
        if (stat(a, &st) != 0) return 1;
        snprintf(actual, acap, "exists"); return 0;
    }
    if (!strcmp(verb, "exec")) {
        if (!a) return -1;
        snprintf(desc, dcap, "%s is executable", a);
        if (access(a, X_OK) == 0 && stat(a, &st) == 0 && S_ISREG(st.st_mode)) return 1;
        snprintf(actual, acap, "%s", stat(a, &st) ? "missing" : "not executable"); return 0;
    }
    if (!strcmp(verb, "empty")) {
        if (!a) return -1;
        snprintf(desc, dcap, "%s is empty", a);
        if (stat(a, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == 0) return 1;
        if (stat(a, &st) != 0) { snprintf(actual, acap, "missing"); return 0; }
        snprintf(actual, acap, "%lld bytes", (long long)st.st_size); return 0;
    }
    if (!strcmp(verb, "nonempty")) {
        if (!a) return -1;
        snprintf(desc, dcap, "%s is non-empty", a);
        if (stat(a, &st) == 0 && S_ISREG(st.st_mode) && st.st_size > 0) return 1;
        snprintf(actual, acap, "%s", stat(a, &st) ? "missing" : "0 bytes"); return 0;
    }
    if (!strcmp(verb, "contains")) {
        if (!a || !b) return -1;
        snprintf(desc, dcap, "%s contains \"%s\"", a, b);
        if (stat(a, &st) != 0) { snprintf(actual, acap, "file missing"); return 0; }
        if (file_contains(a, b)) return 1;
        snprintf(actual, acap, "not found"); return 0;
    }
    if (!strcmp(verb, "newer")) {
        if (!a || !b) return -1;
        snprintf(desc, dcap, "%s is newer than %s", a, b);
        if (stat(a, &st) != 0) { snprintf(actual, acap, "%s missing", a); return 0; }
        if (stat(b, &st2) != 0) { snprintf(actual, acap, "%s missing", b); return 0; }
        if (st.st_mtime > st2.st_mtime) return 1;
        snprintf(actual, acap, "%s is not newer", a); return 0;
    }
    if (!strcmp(verb, "older")) {
        if (!a || !b) return -1;
        snprintf(desc, dcap, "%s is older than %s", a, b);
        if (stat(a, &st) != 0) { snprintf(actual, acap, "%s missing", a); return 0; }
        if (stat(b, &st2) != 0) { snprintf(actual, acap, "%s missing", b); return 0; }
        if (st.st_mtime < st2.st_mtime) return 1;
        snprintf(actual, acap, "%s is not older", a); return 0;
    }
    if (!strcmp(verb, "match") || !strcmp(verb, "same")) {
        if (!a || !b) return -1;
        snprintf(desc, dcap, "%s matches %s", a, b);
        if (files_equal(a, b)) return 1;
        snprintf(actual, acap, "differ"); return 0;
    }
    if (!strcmp(verb, "rc")) {
        if (!a) return -1;
        int want = atoi(a);
        /* ADR-0052 W1: source-declared verdict. In-process previous command
         * wins (most recent truth); the attested chain is the cross-call
         * fallback; no source at all is an honest usage error, never a
         * vacuous `rc 0` pass. */
        int got = -1;
        const char* source = "none";
        unsigned long long cseq = 0;
        char ccmd[96] = "";
        if (s && s->last_status.cmd[0]) {
            got = s->last_status.code;
            source = "this process";
        } else {
            int crc = 0;
            if (chain_last_rc(&crc, &cseq, ccmd, sizeof ccmd)) {
                got = crc;
                source = "chain";
            }
        }
        if (got < 0) {
            snprintf(desc, dcap, "no previous command recorded anywhere "
                                 "(in-process or chain)");
            snprintf(actual, acap, "no source");
            return -2;                       /* honest refusal, distinct rc */
        }
        if (!strcmp(source, "this process"))
            snprintf(desc, dcap, "previous command rc == %d (this process)", want);
        else
            snprintf(desc, dcap, "previous command rc == %d (chain, seq #%llu)",
                     want, cseq);
        if (got == want) return 1;
        snprintf(actual, acap, "rc %d", got); return 0;
    }
    if (!strcmp(verb, "cmd")) {
        /* ADR-0052 W3: gated execution — `expect cmd "<cmd>" rc N` and
         * `expect cmd "<cmd>" output PAT` run the target AND assert in one
         * composite verdict. Before W3 the agent needed two lines and lost
         * the composite: `make test && expect rc 0` gives no captured
         * output on failure. The verdict carries the evidence (capped). */
        if (!a) return -1;
        extern int jash_eval_line(jash_state_t*, jash_writer_t, void*, const char*);
        /* capture output through a temp file (same redirect discipline as
         * weave: writer-capture only sees builtins) */
        char tmp[128];
        const char* tdir = jash_tmpdir();
        snprintf(tmp, sizeof tmp, "%s/jash_expect_%ld_XXXXXX", tdir, (long)getpid());
        int fd = mkstemp(tmp);
        char line[1024];
        if (fd >= 0) {
            (void)close(fd);
            snprintf(line, sizeof line, "( %s ) > %s 2>&1", a, tmp);
        } else {
            snprintf(line, sizeof line, "%s", a);
        }
        int got_rc = jash_eval_line(s, w, ctx, line);
        char out[2048]; size_t olen = 0;
        if (fd >= 0) {
            fd = open(tmp, 0 /* O_RDONLY */);
            if (fd >= 0) {
                ssize_t n = read(fd, out, sizeof(out) - 1);
                if (n > 0) olen = (size_t)n;
                (void)close(fd);
            }
            unlink(tmp);
        }
        out[olen] = '\0';
        /* the assert word after the command: rc N | output PAT */
        const char* assert_verb = b;              /* tok[2] */
        const char* assert_arg = argc > 3 ? argv[3] : NULL;
        if (!assert_verb) return -1;
        if (!strcmp(assert_verb, "rc")) {
            int want = assert_arg ? atoi(assert_arg) : 0;
            snprintf(desc, dcap, "`%.60s` rc == %d", a, want);
            if (got_rc == want) return 1;
            if (olen) snprintf(actual, acap, "rc %d, out: %.80s", got_rc, out);
            else      snprintf(actual, acap, "rc %d", got_rc);
            return 0;
        }
        if (!strcmp(assert_verb, "output")) {
            const char* pat = assert_arg ? assert_arg : "";
            snprintf(desc, dcap, "`%.60s` output contains \"%s\"", a, pat);
            if (strstr(out, pat)) return 1;
            snprintf(actual, acap, "not found (out: %.60s)", out);
            return 0;
        }
        return -1;
    }
    if (!strcmp(verb, "link")) {
        /* ADR-0052 W6: PATH is a symlink whose target resolves (lstat sees
         * the link itself — stat would follow it). */
        if (!a) return -1;
        snprintf(desc, dcap, "%s is a valid symlink", a);
        struct stat lst;
        if (lstat(a, &lst) != 0) { snprintf(actual, acap, "missing"); return 0; }
        if (!S_ISLNK(lst.st_mode)) { snprintf(actual, acap, "not a symlink"); return 0; }
        if (stat(a, &st) == 0) return 1;
        snprintf(actual, acap, "broken (target missing)"); return 0;
    }
    if (!strcmp(verb, "broken")) {
        /* ADR-0052 W6: PATH is a symlink whose target does NOT resolve. */
        if (!a) return -1;
        snprintf(desc, dcap, "%s is a broken symlink", a);
        struct stat lst;
        if (lstat(a, &lst) != 0) { snprintf(actual, acap, "missing"); return 0; }
        if (!S_ISLNK(lst.st_mode)) { snprintf(actual, acap, "not a symlink"); return 0; }
        if (stat(a, &st) != 0) return 1;
        snprintf(actual, acap, "target resolves"); return 0;
    }
    if (!strcmp(verb, "snapshot")) {
        /* ADR-0052 W2: snapshot testing, native to the shell.
         *   expect snapshot <name> [cmd...]   golden file at ~/.jash/snapshots/<name>;
         *                                     first run RECORDS (and says so),
         *                                     later runs DIFF (unified, capped);
         *   expect snapshot <name> --update   bless the new golden (attested).
         * No cmd = snapshot the previous chained command's... no — cmd required. */
        if (!a) return -1;
        const char* home = getenv("HOME");
        if (!home) return -1;
        /* --update is a flag anywhere in argv, never part of the command
         * (measured: `expect snapshot n "echo x" --update` ran
         * `echo x --update` and diffed it against the golden). */
        int update = 0;
        char cmdline[1024] = "";
        for (int i = 2; i < argc; i++) {
            if (!strcmp(argv[i], "--update")) { update = 1; continue; }
            if (cmdline[0]) strlcat(cmdline, " ", sizeof cmdline);
            strlcat(cmdline, argv[i], sizeof cmdline);
        }
        if (!cmdline[0] && !update) return -1;

        char snap[640];
        snprintf(snap, sizeof snap, "%s/.jash/snapshots", home);
        mkdir(snap, 0700);
        strlcat(snap, "/", sizeof snap);
        strlcat(snap, a, sizeof snap);

        /* produce current output (same redirect capture as `cmd`) */
        char current[4096]; size_t clen = 0;
        if (cmdline[0] && !update) {
            extern int jash_eval_line(jash_state_t*, jash_writer_t, void*, const char*);
            char tmp[128];
            const char* tdir = jash_tmpdir();
            snprintf(tmp, sizeof tmp, "%s/jash_snap_%ld_XXXXXX", tdir, (long)getpid());
            int fd = mkstemp(tmp);
            char line[1200];
            if (fd >= 0) {
                (void)close(fd);
                snprintf(line, sizeof line, "( %s ) > %s 2>&1", cmdline, tmp);
            } else {
                snprintf(line, sizeof line, "%s", cmdline);
            }
            (void)jash_eval_line(s, w, ctx, line);
            if (fd >= 0) {
                fd = open(tmp, 0);
                if (fd >= 0) {
                    ssize_t n = read(fd, current, sizeof current - 1);
                    if (n > 0) clen = (size_t)n;
                    (void)close(fd);
                }
                unlink(tmp);
            }
            current[clen] = '\0';
        }

        struct stat sst;
        if (update || stat(snap, &sst) != 0) {
            /* record (first run or explicit bless) — the bless runs the
             * command when one was given, so `--update "echo x"` records x's
             * output; a bare `--update` re-uses the last captured run. */
            if (update && cmdline[0]) {
                extern int jash_eval_line(jash_state_t*, jash_writer_t, void*, const char*);
                char tmp[128];
                const char* tdir = jash_tmpdir();
                snprintf(tmp, sizeof tmp, "%s/jash_snapu_%ld_XXXXXX", tdir, (long)getpid());
                int fd2 = mkstemp(tmp);
                char line[1200];
                if (fd2 >= 0) {
                    (void)close(fd2);
                    snprintf(line, sizeof line, "( %s ) > %s 2>&1", cmdline, tmp);
                    (void)jash_eval_line(s, w, ctx, line);
                    fd2 = open(tmp, 0);
                    if (fd2 >= 0) {
                        ssize_t n = read(fd2, current, sizeof current - 1);
                        if (n > 0) clen = (size_t)n;
                        (void)close(fd2);
                    }
                    unlink(tmp);
                }
                current[clen] = '\0';
            }
            FILE* f = fopen(snap, "w");
            if (!f) { snprintf(actual, acap, "cannot write snapshot"); return 0; }
            if (clen) (void)!fwrite(current, 1, clen, f);
            fclose(f);
            snprintf(desc, dcap, "snapshot '%s' %s", a,
                     update ? "updated (blessed)" : "recorded (first run)");
            return 1;
        }
        /* compare */
        FILE* f = fopen(snap, "r");
        if (!f) { snprintf(actual, acap, "cannot read snapshot"); return 0; }
        char golden[4096]; size_t glen = fread(golden, 1, sizeof golden - 1, f);
        fclose(f);
        golden[glen] = '\0';
        snprintf(desc, dcap, "snapshot '%s' matches golden", a);
        if (glen == clen && !memcmp(golden, current, glen)) return 1;
        /* diff, capped: first differing line */
        char* g = golden, *c = current;
        int ln = 1;
        while (*g && *c) {
            char* ge = strchr(g, '\n'); char* ce = strchr(c, '\n');
            size_t gl2 = ge ? (size_t)(ge - g) : strlen(g);
            size_t cl2 = ce ? (size_t)(ce - c) : strlen(c);
            if (gl2 != cl2 || memcmp(g, c, gl2)) {
                snprintf(actual, acap, "line %d differs: '%.30s' vs golden '%.30s'",
                         ln, c, g);
                return 0;
            }
            ln++;
            g = ge ? ge + 1 : g + strlen(g);
            c = ce ? ce + 1 : c + strlen(c);
        }
        snprintf(actual, acap, "output differs at line %d (%zu vs %zu bytes)",
                 ln, clen, glen);
        return 0;
    }
    return -1;
}

static int exp_usage(void) {
    jash_writes(jash_stderr_writer, 0,
        "expect: usage: expect [not] <file|dir|no|exec|empty|nonempty|contains|"
        "newer|older|match|rc> ARG...\r\n");
    return 2;
}

int jash_b_expect(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    /* ADR-0052 W5: `expect --until SEC <pred>` — re-assert with timeout.
     * The wait-for pattern every agent writes badly (sleep loops in
     * comments): poll every 100ms until the predicate holds or SEC seconds
     * pass. Verdict carries the attempt count; rc is honest (0 held,
     * 1 timed out). */
    { const char* a2 = args;
      while (a2 && (*a2 == ' ' || *a2 == '\t')) a2++;
      if (a2 && !strncmp(a2, "--until", 7) && (a2[7] == ' ' || a2[7] == '\t')) {
          a2 += 7;
          while (*a2 == ' ' || *a2 == '\t') a2++;
          int secs = atoi(a2);
          if (secs <= 0) secs = 5;
          while (*a2 && *a2 != ' ' && *a2 != '\t') a2++;
          char rest[1024];
          snprintf(rest, sizeof rest, "%s", a2);
          int attempts = 0;
          long deadline = (long)time(NULL) + secs;
          for (;;) {
              attempts++;
              char rbuf[2048];
              snprintf(rbuf, sizeof rbuf, "%s", rest);
              char* tt[6] = {0,0,0,0,0,0}; int ntt = 0;
              char* p = rbuf;
              while (*p && ntt < 6) {
                  while (*p == ' ' || *p == '\t') p++;
                  if (!*p) break;
                  if (*p == '"' || *p == '\'') {
                      char q = *p;
                      char* start = ++p;
                      while (*p && *p != q) p++;
                      if (*p) *p++ = '\0';
                      tt[ntt++] = start;
                  } else {
                      tt[ntt++] = p;
                      while (*p && *p != ' ' && *p != '\t') p++;
                      if (*p) *p++ = '\0';
                  }
              }
              if (ntt > 0) {
                  int neg = 0, off2 = 0;
                  if (!strcmp(tt[0], "not") || !strcmp(tt[0], "!")) { neg = 1; off2 = 1; }
                  char d2[1200], a3[256]; a3[0] = '\0';
                  int rr = eval_pred_w(ntt - off2, tt + off2, s, w, ctx,
                                       d2, sizeof d2, a3, sizeof a3);
                  if (rr >= 0 && (neg ? !rr : rr)) {
                      jash_writef(w, ctx,
                          "expect: \033[1;32mok\033[0m — %s (after %d attempt%s)\r\n",
                          d2, attempts, attempts == 1 ? "" : "s");
                      return 0;
                  }
              }
              if ((long)time(NULL) >= deadline) {
                  jash_writef(jash_stderr_writer, 0,
                      "expect: \033[1;31mFAILED\033[0m — timed out after %ds (%d attempts): %s\r\n",
                      secs, attempts, rest);
                  return 1;
              }
              struct timespec ts = { 0, 100000000L };
              nanosleep(&ts, NULL);
          }
      } }
    /* ADR-0052 W3: `expect cmd "<cmd>" rc N|output PAT` — quote-aware path
     * resolved BEFORE the space-tokenizer shreds the command. */
    int cmd_json = 0;
    const char* cargs = args ? args : "";
    while (*cargs == ' ' || *cargs == '\t') cargs++;
    if (!strncmp(cargs, "--json", 6) && (cargs[6] == ' ' || cargs[6] == '\t')) {
        cmd_json = 1; cargs += 6;
        while (*cargs == ' ' || *cargs == '\t') cargs++;
    }
    char cmdstr[512];
    const char* averb = NULL, *aarg = NULL;
    if (expect_parse_cmd(cargs, cmdstr, sizeof cmdstr, &averb, &aarg)) {
        char desc[1200], actual[512];
        actual[0] = '\0';
        char* syn[4] = { (char*)"cmd", cmdstr, (char*)averb, (char*)aarg };
        int r = eval_pred_w(aarg ? 4 : 3, syn, s, w, ctx, desc, sizeof desc,
                            actual, sizeof actual);
        if (r < 0) return exp_usage();
        if (cmd_json) {
            char ed[2048], ea[512];
            jash_json_esc(desc, ed, sizeof ed);
            jash_json_esc(actual, ea, sizeof ea);
            jash_writef(w, ctx,
                "{\"ok\":%s,\"predicate\":\"%s\",\"actual\":\"%s\"}\n",
                r ? "true" : "false", ed, r ? "" : ea);
        } else if (r) {
            jash_writef(w, ctx, "expect: \033[1;32mok\033[0m — %s\r\n", desc);
        } else {
            jash_writef(jash_stderr_writer, 0,
                "expect: \033[1;31mFAILED\033[0m — %s \033[90m(actual: %s)\033[0m\r\n",
                desc, actual[0] ? actual : "?");
        }
        return r ? 0 : 1;
    }

    char buf[2048];
    snprintf(buf, sizeof buf, "%s", args ? args : "");
    char* tok[6] = {0,0,0,0,0,0}; int nt = 0;
    int json = 0;
    /* quote-aware tokenize (ADR-0052 W3: raw args preserve quotes — spaced
     * paths and `cmd "make test"` must stay one word). */
    { char* p = buf;
      while (*p && nt < 6) {
          while (*p == ' ' || *p == '\t') p++;
          if (!*p) break;
          if (!strncmp(p, "--json", 6) && (p[6] == ' ' || p[6] == '\t' || !p[6])) {
              json = 1; p += 6; continue;
          }
          if (*p == '"' || *p == '\'') {
              char q = *p;
              char* start = ++p;
              while (*p && *p != q) p++;
              if (*p) *p++ = '\0';
              tok[nt++] = start;
          } else {
              tok[nt++] = p;
              while (*p && *p != ' ' && *p != '\t') p++;
              if (*p) *p++ = '\0';
          }
      } }
    if (nt == 0) return exp_usage();

    int negate = 0;
    int off = 0;
    if (!strcmp(tok[0], "not") || !strcmp(tok[0], "!")) {
        negate = 1; off = 1;
        if (nt == 1) return exp_usage();
    }

    char desc[1200], actual[256];
    actual[0] = '\0';
    /* ADR-0052: EVERY path passes w/ctx — the w=0 compat wrapper breaks the
     * builtin redirect engine silently (snapshot capture read n=0). */
    int r = eval_pred_w(nt - off, tok + off, s, w, ctx, desc, sizeof desc,
                        actual, sizeof actual);
    if (r >= 0 && nt - off > 2 &&
        strcmp(tok[off], "cmd") != 0 &&
        strcmp(tok[off], "snapshot") != 0 &&
        strcmp(tok[off], "contains") != 0 &&
        strcmp(tok[off], "newer") != 0 &&
        strcmp(tok[off], "older") != 0 &&
        strcmp(tok[off], "match") != 0 &&
        strcmp(tok[off], "same") != 0) {
        return exp_usage();
    }
    if (r == -2) {                       /* W1: honest refusal (no rc source) */
        jash_writef(jash_stderr_writer, 0,
            "expect: \033[1;31mFAILED\033[0m — %s\r\n", desc);
        return 2;
    }
    if (r < 0) return exp_usage();

    int holds = negate ? !r : r;

    if (json) {
        char ed[2048], ea[512];
        jash_json_esc(desc, ed, sizeof ed);
        jash_json_esc(actual[0] ? actual : "", ea, sizeof ea);
        jash_writef(w, ctx,
            "{\"ok\":%s,\"negated\":%s,\"predicate\":\"%s\",\"actual\":\"%s\"}\n",
            holds ? "true" : "false", negate ? "true" : "false", ed,
            holds ? "" : ea);
        return holds ? 0 : 1;
    }

    if (holds) {
        jash_writef(w, ctx, "expect: \033[1;32mok\033[0m — %s%s\r\n",
                    negate ? "NOT " : "", desc);
        return 0;
    }
    /* ADR-0052 W4: attach causal evidence to path-based failures — the chain
     * knows when the path was last touched and by which command. One extra
     * line of proof, only when it exists. */
    char evidence[400] = "";
    if (!negate && nt - off >= 2 && tok[off + 1]) {
        const char* verb = tok[off];
        if (strcmp(verb, "file") && strcmp(verb, "exists") && strcmp(verb, "dir") &&
            strcmp(verb, "no") && strcmp(verb, "absent") && strcmp(verb, "exec") &&
            strcmp(verb, "empty") && strcmp(verb, "nonempty") && strcmp(verb, "contains") &&
            strcmp(verb, "newer") && strcmp(verb, "older") && strcmp(verb, "match") &&
            strcmp(verb, "same") && strcmp(verb, "cmd") && strcmp(verb, "rc")) {
            /* path-bearing verbs */
        }
        if (!strcmp(verb, "file") || !strcmp(verb, "exists") || !strcmp(verb, "dir") ||
            !strcmp(verb, "no") || !strcmp(verb, "absent") || !strcmp(verb, "exec") ||
            !strcmp(verb, "empty") || !strcmp(verb, "nonempty") ||
            !strcmp(verb, "link") || !strcmp(verb, "broken")) {
            unsigned long long eseq = 0;
            char ecmd[192];
            if (chain_last_touch(tok[off + 1], &eseq, ecmd, sizeof ecmd))
                snprintf(evidence, sizeof evidence,
                    "\r\n       \033[90m· last seen seq #%llu via '%.80s'\033[0m",
                    eseq, ecmd);
        }
    }
    if (negate)
        jash_writef(jash_stderr_writer, 0,
            "expect: \033[1;31mFAILED\033[0m — NOT %s \033[90m(but it holds)\033[0m\r\n", desc);
    else
        jash_writef(jash_stderr_writer, 0,
            "expect: \033[1;31mFAILED\033[0m — %s \033[90m(actual: %s)\033[0m%s\r\n",
            desc, actual[0] ? actual : "?", evidence);
    return 1;
}

#include "expect_native.inc"
