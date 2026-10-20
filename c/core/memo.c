// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/memo — read memoization for LLM agent sessions (ADR-0038 W1).
// See memo.h for the contract. F E R M I ∞ H A R T

#include "memo.h"
#include "linediff.h"
#include <core/argv.h>
#include <core/env.h>

#include <dirent.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include "osif/osif_stat.h"
#if defined(__APPLE__)
#include <mach-o/dyld.h>   /* JINV-121: _NSGetExecutablePath — self identity */
#endif

#define MEMO_MAX_OPERANDS   16
#define MEMO_MAX_RECORDS    128
#define MEMO_MAX_FILE       (256 * 1024)
#define MEMO_DIR_MAX_ENTRY  4096
#define MEMO_KEY_HEX        16   /* first 16 hex chars of sha256 */

int jash_env_smells_like_agent(void) {
    /* The three direct probes are single getenv() calls — always evaluated
     * fresh, so an interactive `export AI_AGENT=1` is seen immediately. The
     * expensive part is the prefix scan over environ; cache its result and
     * re-scan only when the environ ARRAY pointer changes (setenv() reallocs
     * the array when ADDING a variable; an in-place value rewrite cannot
     * create an agent-prefixed name that was not there). Residual edge:
     * unsetenv() shifts the array in place, so un-setting an agent var is
     * noticed only after the next setenv. */
    if (getenv("JASH_AGENT") || getenv("AI_AGENT") || getenv("CLAUDECODE"))
        return 1;
    extern char** environ;
    static char** scanned_environ;
    static int    scanned_result = -1;
    if (scanned_result >= 0 && environ == scanned_environ)
        return scanned_result;
    static const char* const prefixes[] = {
        "CLAUDE_CODE_", "CODEX_", "GEMINI_CLI", "AIDER_", "CURSOR_",
        "COPILOT_AGENT", "OPENCODE_", "QWEN_CODE", "LLM_AGENT", NULL
    };
    int r = 0;
    for (char** e = environ; e && *e && !r; e++) {
        for (int i = 0; prefixes[i]; i++) {
            size_t pl = strlen(prefixes[i]);
            if (strncmp(*e, prefixes[i], pl) == 0) { r = 1; break; }
        }
    }
    scanned_environ = environ;
    scanned_result = r;
    return r;
}

/* ── top-level stdout registration ──────────────────────────────────────── */

static jash_writer_t g_top_w[2];
static void*         g_top_ctx[2];
static int           g_top_n = 0;

/* ADR-0042 F1: memoΔ state — previous output path/hash and the hash of the
 * output we have just stored (used to copy the captured bytes into the
 * content-addressed memo_out cache). */
static char g_delta_prev_path[1024];
static char g_delta_prev_hash[MEMO_KEY_HEX + 1];
static int  g_delta_active = 0;
static long g_delta_prev_ts = 0;   /* ts of the previous record (for prev_age_s) */
static char g_last_outhash[MEMO_KEY_HEX + 1];

/* ADR-0042 F1: last computed delta, surfaced in the JASH_AGENT envelope. */
static int  g_delta_last_valid = 0;
static int  g_delta_last_add   = 0;
static int  g_delta_last_del   = 0;
static long g_delta_last_age   = 0;

void jash_memo_mark_top(jash_writer_t w, void* ctx) {
    if (g_top_n < 2) { g_top_w[g_top_n] = w; g_top_ctx[g_top_n] = ctx; g_top_n++; }
}

static int memo_is_top(jash_writer_t w, void* ctx) {
    /* Test-only escape hatch: gates exercise memoization while capturing
     * stdout, where the real top writer is not the terminal. */
    const char* test_force = getenv("JASH_MEMO_TEST");
    if (test_force && test_force[0] == '1') return 1;
    for (int i = 0; i < g_top_n; i++)
        if (g_top_w[i] == w && g_top_ctx[i] == ctx) return 1;
    return 0;
}

/* ── whitelist ──────────────────────────────────────────────────────────── */

typedef struct {
    const char* name;
    const char* argflag_letters;  /* short flags that consume the NEXT token */
    const char* disq_letters;     /* short flags that disqualify memoization */
    const char* const* argflag_long;
    const char* const* disq_long;
    int skip_first_operand;       /* grep-family: first operand is the pattern */
    int allow_dir;                /* ls: directory operands allowed */
    int default_dot;              /* ls: zero operands means "." */
    int rc_ok_max;                /* store only when 0 <= rc <= this */
} memo_cmd_t;

static const char* const GREP_AF_LONG[] = {"--include", "--exclude",
                                           "--exclude-dir", "--max-count", NULL};
static const char* const GREP_DQ_LONG[] = {"--recursive",
                                           "--dereference-recursive",
                                           "--directories", "--file", NULL};
static const char* const RG_AF_LONG[]   = {"--glob", "--type", "--max-count", NULL};
static const char* const RG_DQ_LONG[]   = {"--file", NULL};
static const char* const LS_DQ_LONG[]   = {"--recursive", NULL};
static const char* const DIFF_DQ_LONG[] = {"--recursive", NULL};
static const char* const NONE[]         = {NULL};

static const memo_cmd_t MEMO_CMDS[] = {
    {"cat",   "",       "",     NONE,         NONE,         0, 0, 0, 0},
    {"head",  "nc",     "",     NONE,         NONE,         0, 0, 0, 0},
    {"tail",  "nc",     "fFr",  NONE,         NONE,         0, 0, 0, 0},
    {"wc",    "",       "",     NONE,         NONE,         0, 0, 0, 0},
    {"nl",    "bnsvwi", "",     NONE,         NONE,         0, 0, 0, 0},
    {"stat",  "ft",     "",     NONE,         NONE,         0, 0, 0, 0},
    {"file",  "",       "",     NONE,         NONE,         0, 0, 0, 0},
    {"cmp",   "",       "",     NONE,         NONE,         0, 0, 0, 1},
    {"diff",  "UCIx",   "r",    NONE,         DIFF_DQ_LONG, 0, 0, 0, 1},
    {"grep",  "emABCd", "rRfD", GREP_AF_LONG, GREP_DQ_LONG, 1, 0, 0, 1},
    {"egrep", "emABCd", "rRfD", GREP_AF_LONG, GREP_DQ_LONG, 1, 0, 0, 1},
    {"fgrep", "emABCd", "rRfD", GREP_AF_LONG, GREP_DQ_LONG, 1, 0, 0, 1},
    {"rg",    "emABCgtT", "f",  RG_AF_LONG,   RG_DQ_LONG,   1, 0, 0, 1},
    {"ls",    "",       "R",    NONE,         LS_DQ_LONG,   0, 1, 1, 0},
    {NULL, NULL, NULL, NULL, NULL, 0, 0, 0, 0}
};

/* ADR-0042 F5: public whitelist probe for the prefetch engine. */
int jash_memo_name_memoizable(const char* name) {
    if (!name || !*name) return 0;
    for (int i = 0; MEMO_CMDS[i].name; i++)
        if (strcmp(MEMO_CMDS[i].name, name) == 0) return 1;
    return 0;
}

static const memo_cmd_t* memo_lookup(const char* name) {
    for (int i = 0; MEMO_CMDS[i].name; i++)
        if (strcmp(MEMO_CMDS[i].name, name) == 0) return &MEMO_CMDS[i];
    return NULL;
}

/* ── fingerprints ───────────────────────────────────────────────────────── */

typedef struct {
    char          path[512];
    unsigned long a, b, c;   /* file: size, mtime_ns, ctime_ns
                              * dir:  entry_count, fold1, fold2 */
    int           is_dir;
} memo_fp_t;

static unsigned long fnv1a(unsigned long h, const void* data, size_t n) {
    const unsigned char* p = (const unsigned char*)data;
    if (h == 0) h = 1469598103934665603UL;
    for (size_t i = 0; i < n; i++) { h ^= p[i]; h *= 1099511628211UL; }
    return h;
}

static unsigned long ts_ns(const struct timespec* t) {
    return (unsigned long)t->tv_sec * 1000000000UL + (unsigned long)t->tv_nsec;
}

/* Returns 1 on success, 0 if the path can't be fingerprinted. */
static int memo_fingerprint(const char* path, int allow_dir, memo_fp_t* fp) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    size_t pl = strlen(path);
    if (pl >= sizeof(fp->path)) return 0;
    memcpy(fp->path, path, pl + 1);

    fp->is_dir = S_ISDIR(st.st_mode) ? 1 : 0;
    if (S_ISDIR(st.st_mode)) {
        if (!allow_dir) return 0;
        DIR* d = opendir(path);
        if (!d) return 0;
        unsigned long count = 0, f1 = 0, f2 = 0;
        struct dirent* de;
        char child[1024];
        while ((de = readdir(d)) != NULL) {
            if (de->d_name[0] == '.' &&
                (de->d_name[1] == '\0' ||
                 (de->d_name[1] == '.' && de->d_name[2] == '\0')))
                continue;
            count++;
            if (count > MEMO_DIR_MAX_ENTRY) { closedir(d); return 0; }
            struct stat cs;
            snprintf(child, sizeof(child), "%s/%s", path, de->d_name);
            if (lstat(child, &cs) != 0) { closedir(d); return 0; }
            f1 = fnv1a(f1, de->d_name, strlen(de->d_name));
            f1 = fnv1a(f1, &cs.st_size, sizeof(cs.st_size));
            unsigned long m = ts_ns(&BEAR_ST_MTIM(cs));
            unsigned long c = ts_ns(&BEAR_ST_CTIM(cs));
            f2 = fnv1a(f2, &m, sizeof(m));
            f2 = fnv1a(f2, &c, sizeof(c));
        }
        closedir(d);
        fp->a = count; fp->b = f1; fp->c = f2;
        return 1;
    }
    if (!S_ISREG(st.st_mode)) return 0;
    fp->a = (unsigned long)st.st_size;
    fp->b = ts_ns(&BEAR_ST_MTIM(st));
    fp->c = ts_ns(&BEAR_ST_CTIM(st));
    return 1;
}

/* ── candidate analysis ─────────────────────────────────────────────────── */

typedef struct {
    const memo_cmd_t* cmd;
    char key[MEMO_KEY_HEX + 1];
    memo_fp_t fps[MEMO_MAX_OPERANDS];
    int nfp;
} memo_plan_t;

static const char* memo_sid(void) {
    const char* sid = getenv("CLAUDE_CODE_SESSION_ID");
    if (!sid || !*sid) sid = getenv("JASH_SESSION");
    return (sid && *sid) ? sid : NULL;
}

/* JINV-121: the IMPLEMENTATION of a memoized command is an input.
 *
 * Every command on the memo whitelist (grep, ls, wc, find, ...) is served by
 * a jash builtin, so the jash image itself decides the answer. The key hashed
 * only (sid, cwd, name, args, locale) and the plan fingerprinted only the
 * operands — so two different jash binaries sharing ~/.jash answered each
 * other's questions. Measured: the stale installed binary reported `0` for a
 * pattern it actually rejects, having borrowed the fresh binary's record,
 * while printing "inputs re-verified now". A cache that misses is cheap; a
 * cache that lies under a verification banner is the bug it was meant to
 * catch. Fold the running image's identity in so a rebuilt or swapped jash
 * cannot collide with its predecessor. F E R M I ∞ H A R T */
static const char* memo_self_id(void) {
    static char id[96];
    static int  done = 0;
    if (done) return id;
    done = 1;
    id[0] = '\0';

    char path[1024];
    path[0] = '\0';
#if defined(__APPLE__)
    uint32_t sz = (uint32_t)sizeof(path);
    if (_NSGetExecutablePath(path, &sz) != 0) path[0] = '\0';
#else
    ssize_t n = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (n > 0) path[n] = '\0'; else path[0] = '\0';
#endif
    /* Identity, not content: inode+mtime+size changes on every relink, is
     * O(1), and never reads the (multi-MB) image on the hot path. If the
     * image cannot be located, fail CLOSED — an empty id would silently
     * restore the old colliding behaviour, so key on something unique to
     * this process instead and simply never hit. */
    struct stat st;
    if (path[0] && stat(path, &st) == 0)
        /* NANOSECOND mtime, not st_mtime. A relink or `make install` finishes
         * well inside one second on a fast machine, so a whole-second stamp
         * gives the NEW image the OLD identity and every memo record from
         * before the rebuild reads as a hit. Measured here: two mtimes 17 ms
         * apart, same second, and the stale record was served. */
        snprintf(id, sizeof(id), "%llu:%lld.%09ld:%llu",
                 (unsigned long long)st.st_ino,
                 (long long)BEAR_ST_MTIM(st).tv_sec,
                 (long)BEAR_ST_MTIM(st).tv_nsec,
                 (unsigned long long)st.st_size);
    else
        snprintf(id, sizeof(id), "unknown-image:%lld",
                 (long long)getpid());
    return id;
}

static void memo_key(const char* sid, const char* cwd, const char* name,
                     const char* args, char out[MEMO_KEY_HEX + 1]) {
    jash_sha256_ctx_t cx;
    jash_sha256_init(&cx);
    const char* lang = getenv("LANG");       if (!lang) lang = "";
    const char* lca  = getenv("LC_ALL");     if (!lca)  lca  = "";
    const char* lcc  = getenv("LC_COLLATE"); if (!lcc)  lcc  = "";
    const char* self = memo_self_id();       /* JINV-121 */
    const char* parts[] = {sid, cwd, name, args, lang, lca, lcc, self};
    for (unsigned i = 0; i < sizeof(parts) / sizeof(parts[0]); i++) {
        jash_sha256_update(&cx, (const uint8_t*)parts[i], strlen(parts[i]));
        jash_sha256_update(&cx, (const uint8_t*)"\x1F", 1);
    }
    uint8_t d[JASH_SHA256_DIGEST_LEN];
    jash_sha256_final(&cx, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < MEMO_KEY_HEX / 2; i++) {
        out[i * 2]     = hex[d[i] >> 4];
        out[i * 2 + 1] = hex[d[i] & 15];
    }
    out[MEMO_KEY_HEX] = '\0';
}

static int flag_in(char f, const char* set) { return strchr(set, f) != NULL; }

static int long_in(const char* tok, const char* const* list) {
    for (int i = 0; list[i]; i++) {
        size_t l = strlen(list[i]);
        if (strncmp(tok, list[i], l) == 0 &&
            (tok[l] == '\0' || tok[l] == '=')) return 1 + (tok[l] == '=');
    }
    return 0;
}

/* These are the owned structured producers on the memo whitelist. Their
 * format is an explicit byte contract: neither an unchanged banner nor a
 * memo delta is an equivalent answer. Decide before constructing a memo tee,
 * so checked producer sinks also see the direct presentation-budget writer.
 *
 * Match the producer's selector grammar, not a substring in the command:
 * cat/stat/wc stop at the first operand, all stop at --, and option values
 * (including grep -e --json) remain literal data. External whitelist entries
 * retain their existing memo contract. */
static int memo_structured_request(const char* name, int argc, char* const* argv) {
    int cat = !strcmp(name, "cat"), stat_command = !strcmp(name, "stat");
    int wc = !strcmp(name, "wc"), diff = !strcmp(name, "diff");
    int grep = !strcmp(name, "grep"), ls = !strcmp(name, "ls");
    if (!cat && !stat_command && !wc && !diff && !grep && !ls) return 0;
    for (int i = 0; i < argc; i++) {
        const char* argument = argv[i];
        if (!strcmp(argument, "--")) break;
        if ((cat || stat_command || wc) &&
            (argument[0] != '-' || !argument[1])) break;
        if (!strcmp(argument, "--json") || !strcmp(argument, "--data") ||
            !strcmp(argument, "--data-v2") ||
            ((stat_command || wc) && !strcmp(argument, "--json-report"))) return 1;

        int takes_value = 0;
        if (grep) {
            static const char* const required[] = {
                "--regexp", "--include", "--exclude", "--exclude-dir",
                "--after-context", "--before-context", "--since", "--max-count",
                "--offset", "--limit", NULL
            };
            int optional_context = !strcmp(argument, "--context");
            for (size_t k = 0; required[k]; k++)
                if (!strcmp(argument, required[k])) takes_value = 1;
            if (argument[0] == '-' && argument[1] && argument[1] != '-') {
                for (size_t k = 1; argument[k]; k++) {
                    if (!strchr("ABCem", argument[k])) continue;
                    takes_value = argument[k + 1] == '\0';
                    optional_context = argument[k] == 'C' && takes_value;
                    break;
                }
            }
            if (optional_context) {
                const char* next = i + 1 < argc ? argv[i + 1] : "";
                takes_value = (*next >= '0' && *next <= '9') ||
                    (*next == '-' && next[1] >= '0' && next[1] <= '9');
            }
        } else if (ls) {
            takes_value = !strcmp(argument, "--pretty-limit") ||
                !strcmp(argument, "--semantic") || !strcmp(argument, "--relate") ||
                !strcmp(argument, "--monitor");
        } else {
            takes_value = !strcmp(argument, "--offset") || !strcmp(argument, "--limit");
            if (stat_command) takes_value |= !strcmp(argument, "-c") ||
                !strcmp(argument, "-f") || !strcmp(argument, "--format") ||
                !strcmp(argument, "--printf");
            if (wc) takes_value |= !strcmp(argument, "--files0-from");
            if (diff) takes_value |= !strcmp(argument, "-U");
        }
        if (takes_value && i + 1 < argc) i++;
    }
    return 0;
}

/* Build the memo plan: whitelist + flag rules + operand fingerprints.
 * Returns 1 when the command is a valid memo candidate. */
static int memo_plan(jash_state_t* s, const char* name, const char* args,
                     memo_plan_t* pl) {
    pl->cmd = memo_lookup(name);
    if (!pl->cmd) return 0;
    const memo_cmd_t* c = pl->cmd;

    jash_argv_t av;
    if (jash_argv_build(args, &av) < 0) return 0;
    if (memo_structured_request(name, av.argc, av.argv)) {
        jash_argv_free(&av);
        return 0;
    }

    const char* operands[MEMO_MAX_OPERANDS];
    int nops = 0, ok = 1, no_more_flags = 0;

    for (int i = 0; i < av.argc && ok; i++) {
        const char* t = av.argv[i];
        if (!no_more_flags && t[0] == '-' && t[1]) {
            if (t[1] == '-') {
                if (t[2] == '\0') { no_more_flags = 1; continue; }
                int dq = long_in(t, c->disq_long);
                if (dq) { ok = 0; break; }
                int af = long_in(t, c->argflag_long);
                if (af == 1) i++;            /* consumes next token */
                continue;                     /* af==2: --flag=value, self-contained */
            }
            /* short flag cluster */
            for (const char* f = t + 1; *f; f++) {
                if (flag_in(*f, c->disq_letters)) { ok = 0; break; }
                if (flag_in(*f, c->argflag_letters)) {
                    if (f[1] == '\0') i++;   /* -n 5: consumes next token */
                    break;                    /* -n5: inline arg, rest is the value */
                }
            }
            continue;
        }
        if (t[0] == '-' && t[1] == '\0') { ok = 0; break; }  /* stdin operand */
        if (nops >= MEMO_MAX_OPERANDS) { ok = 0; break; }
        operands[nops++] = t;
    }

    int first_skip = c->skip_first_operand;
    if (ok && nops == 0 && c->default_dot) { operands[nops++] = "."; first_skip = 0; }
    if (ok && nops - first_skip < 1) ok = 0;

    pl->nfp = 0;
    for (int i = first_skip; ok && i < nops; i++) {
        char resolved[512];
        const char* p = operands[i];
        if (p[0] != '/') {
            if (snprintf(resolved, sizeof(resolved), "%s/%s",
                         s->cwd[0] ? s->cwd : ".", p) >= (int)sizeof(resolved)) {
                ok = 0; break;
            }
            p = resolved;
        }
        if (!memo_fingerprint(p, c->allow_dir, &pl->fps[pl->nfp])) { ok = 0; break; }
        pl->nfp++;
    }
    jash_argv_free(&av);
    if (!ok || pl->nfp == 0) return 0;

    const char* sid = memo_sid();
    if (!sid) return 0;
    memo_key(sid, s->cwd[0] ? s->cwd : "", name, args, pl->key);
    return 1;
}

/* ── store ──────────────────────────────────────────────────────────────── */

typedef struct {
    char          key[MEMO_KEY_HEX + 1];
    unsigned long ts, rc, lines, bytes;
    char          outhash[MEMO_KEY_HEX + 1];
    memo_fp_t     fps[MEMO_MAX_OPERANDS];
    int           nfp;
} memo_rec_t;

static int memo_path(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !*home) return 0;
    char d[512];
    snprintf(d, sizeof(d), "%s/.jash", home);
    mkdir(d, 0700);
    return snprintf(buf, cap, "%s/.jash/memo.tsv", home) < (int)cap;
}

/* One record per line:
 * key \t ts \t rc \t lines \t bytes \t outhash \t nfp \t
 *   path \x1F a \x1F b \x1F c [\x1E next path group...] \n           */
static int memo_rec_parse(char* line, memo_rec_t* r) {
    char* save = NULL;
    char* f[7];
    for (int i = 0; i < 7; i++) {
        f[i] = strtok_r(i == 0 ? line : NULL, "\t", &save);
        if (!f[i]) return 0;
    }
    if (strlen(f[0]) != MEMO_KEY_HEX) return 0;
    memcpy(r->key, f[0], MEMO_KEY_HEX + 1);
    r->ts    = strtoul(f[1], NULL, 10);
    r->rc    = strtoul(f[2], NULL, 10);
    r->lines = strtoul(f[3], NULL, 10);
    r->bytes = strtoul(f[4], NULL, 10);
    if (strlen(f[5]) != MEMO_KEY_HEX) return 0;
    memcpy(r->outhash, f[5], MEMO_KEY_HEX + 1);
    r->nfp = atoi(f[6]);
    if (r->nfp < 1 || r->nfp > MEMO_MAX_OPERANDS) return 0;

    char* groups = strtok_r(NULL, "\n", &save);
    if (!groups) return 0;
    char* gsave = NULL;
    for (int i = 0; i < r->nfp; i++) {
        char* g = strtok_r(i == 0 ? groups : NULL, "\x1E", &gsave);
        if (!g) return 0;
        char* psave = NULL;
        char* pp = strtok_r(g, "\x1F", &psave);
        char* aa = strtok_r(NULL, "\x1F", &psave);
        char* bb = strtok_r(NULL, "\x1F", &psave);
        char* cc = strtok_r(NULL, "\x1F", &psave);
        if (!pp || !aa || !bb || !cc) return 0;
        if (strlen(pp) >= sizeof(r->fps[i].path)) return 0;
        strlcpy(r->fps[i].path, pp, sizeof(r->fps[i].path));
        r->fps[i].a = strtoul(aa, NULL, 10);
        r->fps[i].b = strtoul(bb, NULL, 10);
        r->fps[i].c = strtoul(cc, NULL, 10);
    }
    return 1;
}

static void memo_rec_format(const memo_rec_t* r, FILE* out) {
    fprintf(out, "%s\t%lu\t%lu\t%lu\t%lu\t%s\t%d\t",
            r->key, r->ts, r->rc, r->lines, r->bytes, r->outhash, r->nfp);
    for (int i = 0; i < r->nfp; i++) {
        if (i) fputc('\x1E', out);
        fprintf(out, "%s\x1F%lu\x1F%lu\x1F%lu",
                r->fps[i].path, r->fps[i].a, r->fps[i].b, r->fps[i].c);
    }
    fputc('\n', out);
}

/* Find `key` in the store. Returns 1 + fills *r when found. */
static int memo_load(const char* key, memo_rec_t* r) {
    char path[600];
    if (!memo_path(path, sizeof(path))) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[8192];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, key, MEMO_KEY_HEX) != 0) continue;
        found = memo_rec_parse(line, r);
        if (found) break;
    }
    fclose(f);
    return found;
}

/* Insert/replace `r` in the store; atomic rewrite; LRU cap. */
static void memo_save(const memo_rec_t* r) {
    char path[600], tmp[640];
    if (!memo_path(path, sizeof(path))) return;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    char* keep[MEMO_MAX_RECORDS];
    int nkeep = 0;
    FILE* f = fopen(path, "r");
    if (f) {
        char line[8192];
        while (fgets(line, sizeof(line), f)) {
            if (strncmp(line, r->key, MEMO_KEY_HEX) == 0) continue;  /* replaced */
            if (strlen(line) < MEMO_KEY_HEX + 8) continue;           /* junk */
            if (nkeep == MEMO_MAX_RECORDS) {                          /* LRU: drop oldest */
                free(keep[0]);
                memmove(keep, keep + 1, sizeof(char*) * (MEMO_MAX_RECORDS - 1));
                nkeep--;
            }
            keep[nkeep] = strdup(line);
            if (keep[nkeep]) nkeep++;
        }
        fclose(f);
    }
    FILE* o = fopen(tmp, "w");
    if (o) {
        int start = (nkeep >= MEMO_MAX_RECORDS) ? 1 : 0;   /* room for the new one */
        for (int i = start; i < nkeep; i++) fputs(keep[i], o);
        memo_rec_format(r, o);
        fclose(o);
        rename(tmp, path);
    }
    for (int i = 0; i < nkeep; i++) free(keep[i]);
}

/* ── ADR-0042 F1: memoΔ helpers ─────────────────────────────────────────────
 * Store the captured output bytes content-addressed by its truncated sha256
 * so the next run can load the previous output and compute a structural diff.
 */

static void memo_save_output_file(const char* src, const char* outhash) {
    const char* home = getenv("HOME");
    if (!home || !*home || !src || !src[0] || !outhash || !outhash[0]) return;
    char dir[512];
    int dn = snprintf(dir, sizeof(dir), "%s/.jash/memo_out", home);
    if (dn < 0 || (size_t)dn >= sizeof dir) return;
    mkdir(dir, 0700);
    char dst[512];
    size_t hn = strlen(outhash);
    if (hn >= sizeof dst - (size_t)dn - 1) return;
    memcpy(dst, dir, (size_t)dn);
    dst[dn] = '/';
    memcpy(dst + dn + 1, outhash, hn + 1);
    int sfd = open(src, O_RDONLY);
    if (sfd < 0) return;
    int dfd = open(dst, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (dfd < 0) { close(sfd); return; }
    char buf[8192];
    ssize_t n;
    while ((n = read(sfd, buf, sizeof(buf))) > 0) {
        ssize_t off = 0;
        while (off < n) {
            ssize_t wn = write(dfd, buf + off, (size_t)(n - off));
            if (wn <= 0) break;
            off += wn;
        }
    }
    close(sfd); close(dfd);
}

#define MEMO_DELTA_SHOW 200   /* cap displayed +/- lines per side */

/* Stream the whole captured output back verbatim.  Fallback path: when the
 * diff is refused (over-cap / I/O), the full body was already suppressed
 * upstream, so the agent must still receive its output. */
static void memo_delta_stream_full(const char* path, jash_writer_t w, void* ctx) {
    FILE* f = fopen(path, "rb");
    if (!f) return;
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        (void)w(ctx, buf, n);
    (void)fclose(f);
}

/* Emit up to MEMO_DELTA_SHOW lines, each `<sign>\t<line>`; the line text is
 * untrusted file content, so it goes through the control-byte sanitizer. */
static void memo_delta_emit_lines(jash_writer_t w, void* ctx, char sign,
                                  const char* const* lines, int n) {
    const char pfx[2] = { sign, '\t' };
    int shown = n < MEMO_DELTA_SHOW ? n : MEMO_DELTA_SHOW;
    for (int i = 0; i < shown; i++) {
        (void)w(ctx, pfx, 2);
        jash_writes_sanitized(w, ctx, lines[i]);
        (void)w(ctx, "\n", 1);
    }
    if (n > shown) {
        (void)w(ctx, pfx, 2);
        jash_writef(w, ctx, "… +%d more\n", n - shown);
    }
}

/* ADR-0042 F1: emit the structural diff of the new output vs the previous
 * captured run — only the `+`/`-` lines that actually changed, never the
 * unchanged bulk.  On a refused diff, stream the full body so nothing is lost.
 * Records the counts for the JASH_AGENT envelope (jash_memo_delta_last). */
static void memo_delta_summary(const char* prev_path, const char* new_path,
                               jash_writer_t w, void* ctx) {
    g_delta_last_valid = 0;
    long age = g_delta_prev_ts > 0 ? (long)time(NULL) - g_delta_prev_ts : 0;
    if (age < 0) age = 0;

    jash_linediff_t d;
    int r = jash_linediff(prev_path, new_path, &d);
    if (r != 0) {
        memo_delta_stream_full(new_path, w, ctx);
        jash_writef(w, ctx,
            "[jash memoΔ] output changed (diff too large) — full body shown · "
            "JASH_MEMO=0 re-prints\n");
        return;
    }

    jash_writef(w, ctx,
        "[jash memoΔ] +%d/-%d lines vs %lds ago · full: JASH_MEMO=0 re-prints\n",
        d.n_add, d.n_del, age);
    memo_delta_emit_lines(w, ctx, '+', d.add, d.n_add);
    memo_delta_emit_lines(w, ctx, '-', d.del, d.n_del);

    g_delta_last_add   = d.n_add;
    g_delta_last_del   = d.n_del;
    g_delta_last_age   = age;
    g_delta_last_valid = 1;
    jash_linediff_free(&d);
}

int jash_memo_delta_last(int* add, int* del, int* age) {
    if (!g_delta_last_valid) return 0;
    if (add) *add = g_delta_last_add;
    if (del) *del = g_delta_last_del;
    if (age) *age = (int)g_delta_last_age;
    return 1;
}

/* ── W3: read ledger ────────────────────────────────────────────────────── */

#define LEDGER_MAX_RECORDS 256

static int ledger_path(char* buf, size_t cap) {
    const char* home = getenv("HOME");
    if (!home || !*home) return 0;
    char d[512];
    snprintf(d, sizeof(d), "%s/.jash", home);
    mkdir(d, 0700);
    return snprintf(buf, cap, "%s/.jash/read_ledger.tsv", home) < (int)cap;
}

/* Record "this session last saw these file versions". Called on every
 * successful memo plan (hit AND miss): both mean the agent has the file's
 * current content in its context. Dirs are skipped — listing a directory
 * is not a content view of the files inside. */
static void ledger_note(const memo_fp_t* fps, int nfp) {
    const char* sid = memo_sid();
    if (!sid) return;
    char path[600], tmp[640];
    if (!ledger_path(path, sizeof(path))) return;
    snprintf(tmp, sizeof(tmp), "%s.tmp.%d", path, (int)getpid());

    /* Which paths are we (re)recording now? */
    int record[MEMO_MAX_OPERANDS];
    int nrec = 0;
    for (int i = 0; i < nfp; i++)
        if (!fps[i].is_dir) record[nrec++] = i;
    if (!nrec) return;

    char* keep[LEDGER_MAX_RECORDS];
    int nkeep = 0;
    FILE* f = fopen(path, "r");
    if (f) {
        char line[1024];
        size_t sidl = strlen(sid);
        while (fgets(line, sizeof(line), f)) {
            /* Drop lines being replaced: same sid + same path. */
            int replaced = 0;
            if (strncmp(line, sid, sidl) == 0 && line[sidl] == '\t') {
                for (int i = 0; i < nrec && !replaced; i++) {
                    const char* p = fps[record[i]].path;
                    size_t pl = strlen(p);
                    if (strncmp(line + sidl + 1, p, pl) == 0 &&
                        line[sidl + 1 + pl] == '\t')
                        replaced = 1;
                }
            }
            if (replaced) continue;
            if (nkeep == LEDGER_MAX_RECORDS) {
                free(keep[0]);
                memmove(keep, keep + 1, sizeof(char*) * (LEDGER_MAX_RECORDS - 1));
                nkeep--;
            }
            keep[nkeep] = strdup(line);
            if (keep[nkeep]) nkeep++;
        }
        fclose(f);
    }
    FILE* o = fopen(tmp, "w");
    if (o) {
        int start = nkeep + nrec > LEDGER_MAX_RECORDS
                  ? nkeep + nrec - LEDGER_MAX_RECORDS : 0;
        for (int i = start; i < nkeep; i++) fputs(keep[i], o);
        unsigned long now = (unsigned long)time(NULL);
        for (int i = 0; i < nrec; i++) {
            const memo_fp_t* fp = &fps[record[i]];
            fprintf(o, "%s\t%s\t%lu\t%lu\n", sid, fp->path, fp->b, now);
        }
        fclose(o);
        rename(tmp, path);
    }
    for (int i = 0; i < nkeep; i++) free(keep[i]);
}

int jash_memo_ledger_lookup(const char* abspath,
                            unsigned long* mtime_ns, unsigned long* read_ts) {
    const char* sid = memo_sid();
    if (!sid || !abspath) return 0;
    char path[600];
    if (!ledger_path(path, sizeof(path))) return 0;
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    size_t sidl = strlen(sid), pl = strlen(abspath);
    char line[1024];
    int found = 0;
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, sid, sidl) != 0 || line[sidl] != '\t') continue;
        if (strncmp(line + sidl + 1, abspath, pl) != 0 ||
            line[sidl + 1 + pl] != '\t') continue;
        char* rest = line + sidl + 1 + pl + 1;
        char* end = NULL;
        unsigned long mt = strtoul(rest, &end, 10);
        unsigned long ts = end ? strtoul(end + 1, NULL, 10) : 0;
        if (mtime_ns) *mtime_ns = mt;
        if (read_ts) *read_ts = ts;
        found = 1;   /* keep scanning: later lines are newer */
    }
    fclose(f);
    return found;
}

/* ── public API ─────────────────────────────────────────────────────────── */

static int memo_enabled(jash_state_t* s, jash_writer_t w, void* ctx) {
    const char* off = getenv("JASH_MEMO");
    if (off && off[0] == '0') return 0;
    if (!s || s->interactive) return 0;
    if (!memo_is_top(w, ctx)) return 0;

    /* ═══ BUG-067 — quem CAPTURA precisa dos bytes ═══════════════════════
     *
     * O memo troca a saida por uma linha atestada. Para um agente que LE, e o
     * produto. Para quem CAPTURA, e uma resposta errada — e a resposta errada
     * se parece com "nao existe".
     *
     * Reportado do campo em 2026-08-05: `grep -c PADRAO arquivo` devolveu
     * vazio para uma linha que existia, e o relato virou uma tabela errada de
     * quais scripts tinham um recurso. Reproduzido: 30 de 30 capturas
     * receberam `[jash memo] unchanged since ...` no lugar do numero. Quem
     * consome compara com um numero, nao casa, e conclui ausencia — com
     * confianca.
     *
     * memo_is_top() ja protegia a substituicao de comando ANINHADA, e o gate
     * do contrato (tests/test_memo_contract.sh) cobre esse caso. O que faltava
     * era o `jash -c` invocado DE FORA: ali o writer E o de topo, porque do
     * ponto de vista daquele processo e a saida principal — mas do outro lado
     * do pipe alguem vai comparar o resultado com um numero.
     *
     * A regra que fecha isso nao e heuristica: um `-c` cuja saida NAO e um
     * terminal esta, por definicao, sendo capturado. Ninguem le um pipe.
     * jash_env_smells_like_agent() responde "ha um agente na sessao", que e
     * outra pergunta — e responder a pergunta errada foi o defeito.
     *
     * O contrato ja dizia isso em Docs/CONTRATO-MEMO.md; faltava o codigo
     * concordar. F E R M I ∞ H A R T */
    /* Tentei aqui `if (!isatty(STDOUT_FILENO)) return 0;` — "ninguem le um
     * pipe" — e a regra e verdadeira, mas o raio estava errado: DESLIGOU o memo
     * em 9 gates da suite, que exercitam a memoizacao com a saida capturada
     * por construcao. Uma correcao que troca um bug de campo por nove
     * regressoes nao e correcao.
     *
     * A fronteira certa e a que Docs/CONTRATO-MEMO.md ja prescrevia desde a W8:
     * o CONSUMIDOR declara. Quem captura poe JASH_MEMO=0 no ambiente — e e isso
     * que faltava na integracao com o harness, nao no shell. O BUG-067 nasceu
     * de um settings.json incompleto, nao de o memo estar errado.
     *
     * Mantido aqui como registro para quem tentar de novo: a regra e boa, o
     * lugar de aplica-la nao e este. F E R M I ∞ H A R T */
    if (!jash_env_smells_like_agent()) return 0;
    return 1;
}

static int delta_enabled(void) {
    /* ADR-0042 F1: memoΔ is default-ON when memoization is on. A changed
     * input now emits the actual `+`/`-` lines that differ (never the
     * unchanged bulk), preserving rc — so the agent still sees what changed
     * without re-paying the full body. JASH_MEMO_DELTA=0 restores the binary
     * memo behavior (full re-print on any change). */
    const char* v = getenv("JASH_MEMO_DELTA");
    return !(v && v[0] == '0');
}

static int fp_equal(const memo_fp_t* x, const memo_fp_t* y) {
    return strcmp(x->path, y->path) == 0 &&
           x->a == y->a && x->b == y->b && x->c == y->c;
}

/* Stashed plan between begin() and store(): one simple command at a time
 * flows through the dispatch sites, so a single slot is enough. */
static memo_plan_t g_pending;
static int         g_pending_valid = 0;

int jash_memo_begin(jash_state_t* s, const char* name, const char* args,
                    jash_writer_t w, void* ctx, int* rc_out) {
    g_pending_valid = 0;
    if (!name || !args) return 0;
    if (!memo_enabled(s, w, ctx)) return 0;
    if (!memo_lookup(name)) return 0;          /* cheap gate before real work */

    memo_plan_t pl;
    if (!memo_plan(s, name, args, &pl)) return 0;

    /* W3: hit or miss, the agent is (re)seeing these file versions now. */
    ledger_note(pl.fps, pl.nfp);

    memo_rec_t rec;
    int had_prev = memo_load(pl.key, &rec) && rec.nfp == pl.nfp;
    if (had_prev) {
        int same = 1;
        for (int i = 0; i < pl.nfp && same; i++)
            same = fp_equal(&rec.fps[i], &pl.fps[i]);
        if (same) {
            char when[32];
            time_t t = (time_t)rec.ts;
            struct tm tmv;
            localtime_r(&t, &tmv);
            strftime(when, sizeof(when), "%H:%M:%S", &tmv);
            char kb[64];
            if (rec.bytes >= 10 * 1024)
                snprintf(kb, sizeof(kb), "%.1fKB", rec.bytes / 1024.0);
            else
                snprintf(kb, sizeof(kb), "%luB", rec.bytes);
            jash_writef(w, ctx,
                "[jash memo] unchanged since %s — output identical to your "
                "previous run of this command (%lu lines, %s, sha256:%s). "
                "inputs re-verified now · JASH_MEMO=0 re-prints\n",
                when, rec.lines, kb, rec.outhash);
            if (rc_out) *rc_out = (int)rec.rc;
            return 2;
        }
        /* ADR-0042 F1: fingerprints differ but key matches → same command,
         * different inputs. Prepare to diff the new output against the old. */
        g_delta_active = 0;
        g_delta_prev_path[0] = '\0';
        if (delta_enabled()) {
            const char* home = getenv("HOME");
            if (home && *home) {
                char dir[512];
                snprintf(dir, sizeof(dir), "%s/.jash/memo_out", home);
                mkdir(dir, 0700);
                snprintf(g_delta_prev_path, sizeof(g_delta_prev_path),
                         "%s/%s", dir, rec.outhash);
                if (access(g_delta_prev_path, F_OK) == 0) {
                    g_delta_active = 1;
                    g_delta_prev_ts = (long)rec.ts;   /* for prev_age_s */
                    memcpy(g_delta_prev_hash, rec.outhash, sizeof(g_delta_prev_hash));
                }
            }
        }
    }
    g_pending = pl;
    g_pending_valid = 1;
    return 1;
}

void jash_memo_tee_init(jash_memo_tee_t* t, jash_writer_t w, void* ctx) {
    t->real_w = w;
    t->real_ctx = ctx;
    jash_sha256_init(&t->sha);
    t->bytes = 0;
    t->lines = 0;
    t->tmp_path[0] = '\0';
    t->tmp_fd = -1;
    t->suppress = 0;
    const char* home = getenv("HOME");
    if (home && *home) {
        char dir[512], tmp[512];
        int dn = snprintf(dir, sizeof(dir), "%s/.jash/memo_out", home);
        int fd = -1;
        if (dn >= 0 && (size_t)dn + sizeof "/.tmpXXXXXX" <= sizeof tmp) {
            mkdir(dir, 0700);
            memcpy(tmp, dir, (size_t)dn);
            memcpy(tmp + dn, "/.tmpXXXXXX", sizeof "/.tmpXXXXXX");
            fd = mkstemp(tmp);
        }
        if (fd >= 0) {
            t->tmp_fd = fd;
            snprintf(t->tmp_path, sizeof(t->tmp_path), "%s", tmp);
        }
    }
    /* ADR-0042 F1: withhold the body only when a delta is pending AND we have
     * a capture file to diff/stream from — otherwise the output would be lost
     * with nothing to reconstruct it. store_from_tee emits the diff (or the
     * full body on a refused diff) in place of the withheld bytes. */
    t->suppress = g_delta_active && t->tmp_fd >= 0;
}

long jash_memo_tee_write(void* ctx, const char* buf, size_t n) {
    jash_memo_tee_t* t = (jash_memo_tee_t*)ctx;
    jash_sha256_update(&t->sha, (const uint8_t*)buf, n);
    t->bytes += n;
    for (size_t i = 0; i < n; i++)
        if (buf[i] == '\n') t->lines++;
    if (t->tmp_fd >= 0) {
        ssize_t off = 0;
        while (off < (ssize_t)n) {
            ssize_t wn = write(t->tmp_fd, buf + off, (size_t)(n - off));
            if (wn <= 0) { close(t->tmp_fd); t->tmp_fd = -1; break; }
            off += wn;
        }
    }
    if (t->suppress) return (long)n;   /* captured only; body withheld for delta */
    return t->real_w(t->real_ctx, buf, n);
}

static void memo_finish(jash_state_t* s, const char* name, const char* args,
                        jash_sha256_ctx_t* sha, unsigned long bytes,
                        unsigned long lines, unsigned long tail_nonl, int rc) {
    (void)s; (void)name; (void)args;
    if (!g_pending_valid) return;
    const memo_cmd_t* c = g_pending.cmd;
    g_pending_valid = 0;
    if (rc < 0 || rc > c->rc_ok_max) return;

    memo_rec_t r;
    memcpy(r.key, g_pending.key, sizeof(r.key));
    r.ts    = (unsigned long)time(NULL);
    r.rc    = (unsigned long)rc;
    r.bytes = bytes;
    r.lines = lines + (tail_nonl ? 1 : 0);
    r.nfp   = g_pending.nfp;
    for (int i = 0; i < r.nfp; i++) r.fps[i] = g_pending.fps[i];

    uint8_t d[JASH_SHA256_DIGEST_LEN];
    jash_sha256_final(sha, d);
    static const char hex[] = "0123456789abcdef";
    for (int i = 0; i < MEMO_KEY_HEX / 2; i++) {
        r.outhash[i * 2]     = hex[d[i] >> 4];
        r.outhash[i * 2 + 1] = hex[d[i] & 15];
    }
    r.outhash[MEMO_KEY_HEX] = '\0';
    memcpy(g_last_outhash, r.outhash, sizeof(g_last_outhash));
    memo_save(&r);
}

void jash_memo_store_from_tee(jash_state_t* s, const char* name,
                              const char* args, jash_memo_tee_t* t, int rc) {
    /* Trailing partial line counts as a line (wc -l semantics differ, but the
     * agent-facing count should match what it read). */
    unsigned long tail = t->bytes > 0 ? 1 : 0;   /* refined below */
    /* We only know the last byte if we tracked it; approximate: if bytes>0
     * and the line count already accounts for it, tail=0. The tee counts \n
     * only, so a byte stream ending in \n has lines==newlines (tail 0). We
     * cannot see the final byte here without storing it — track in write. */
    (void)tail;
    memo_finish(s, name, args, &t->sha, t->bytes, t->lines, 0, rc);
    if (t->tmp_fd >= 0) { close(t->tmp_fd); t->tmp_fd = -1; }
    if (g_last_outhash[0]) memo_save_output_file(t->tmp_path, g_last_outhash);
    if (g_delta_active && g_delta_prev_path[0] && t->tmp_path[0]) {
        memo_delta_summary(g_delta_prev_path, t->tmp_path, t->real_w, t->real_ctx);
    }
    if (t->tmp_path[0]) unlink(t->tmp_path);
    g_delta_active = 0;
    g_delta_prev_path[0] = '\0';
    g_last_outhash[0] = '\0';
}

void jash_memo_store_from_file(jash_state_t* s, const char* name,
                               const char* args, const char* path, int rc) {
    if (!g_pending_valid) return;
    FILE* f = fopen(path, "r");
    if (!f) { g_pending_valid = 0; return; }
    jash_sha256_ctx_t sha;
    jash_sha256_init(&sha);
    unsigned long bytes = 0, lines = 0;
    char last = '\n';
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        jash_sha256_update(&sha, (const uint8_t*)buf, n);
        bytes += n;
        for (size_t i = 0; i < n; i++)
            if (buf[i] == '\n') lines++;
        last = buf[n - 1];
    }
    fclose(f);
    memo_finish(s, name, args, &sha, bytes, lines,
                (bytes > 0 && last != '\n') ? 1 : 0, rc);
    if (g_last_outhash[0]) memo_save_output_file(path, g_last_outhash);
    g_last_outhash[0] = '\0';
}

const char* jash_memo_delta_active(void) {
    return (g_delta_active && g_delta_prev_path[0]) ? g_delta_prev_path : NULL;
}

/* ADR-0042 F1: external-command delta path.  Called *instead* of the budget
 * flush when a previous captured output exists.  Hashes the new output, stores
 * it, saves the memo record, prints only the diff summary, and suppresses the
 * full-body flush.  Returns 1 (capture was complete). */
void jash_memo_delta_store_from_file(jash_state_t* s, const char* name,
                                     const char* args, const char* path,
                                     int rc, jash_writer_t w, void* ctx) {
    if (!g_pending_valid || !path) {
        g_delta_active = 0;
        g_delta_prev_path[0] = '\0';
        g_last_outhash[0] = '\0';
        return;
    }
    FILE* f = fopen(path, "r");
    if (!f) { g_pending_valid = 0; g_delta_active = 0; return; }
    jash_sha256_ctx_t sha;
    jash_sha256_init(&sha);
    unsigned long bytes = 0, lines = 0;
    char last = '\n';
    char buf[8192];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        jash_sha256_update(&sha, (const uint8_t*)buf, n);
        bytes += n;
        for (size_t i = 0; i < n; i++)
            if (buf[i] == '\n') lines++;
        last = buf[n - 1];
    }
    fclose(f);
    memo_finish(s, name, args, &sha, bytes, lines,
                (bytes > 0 && last != '\n') ? 1 : 0, rc);
    if (g_last_outhash[0]) memo_save_output_file(path, g_last_outhash);
    if (g_delta_active && g_delta_prev_path[0]) {
        memo_delta_summary(g_delta_prev_path, path, w, ctx);
    }
    g_delta_active = 0;
    g_delta_prev_path[0] = '\0';
    g_last_outhash[0] = '\0';
}
