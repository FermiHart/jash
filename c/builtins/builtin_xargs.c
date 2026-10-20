// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/xargs — build & run command batches from stdin (ADR-0044).
//
// Why a builtin: `xargs` is how agents turn a list (from find/grep/ls) into
// actions — `find . -name '*.tmp' | xargs rm`. Two problems make the upstream
// tool hostile to an agent:
//
//   1. It is a FOOTGUN with no preview. The agent cannot see the exact commands
//      xargs is about to run before it runs them; a stray space or a filename
//      with a newline silently mangles the batch and `rm` eats the wrong file.
//   2. Its forks are INVISIBLE to jash. Every command xargs spawns bypasses the
//      shell entirely — no sigil attestation, no undo checkpoint, no output
//      budget, no memoΔ. The single most destructive pattern an agent runs
//      (`| xargs rm`) is exactly the one jash's safety net never sees.
//
// GOLD, two levers:
//   • `--plan` (alias `--dry-run`) emits the exact batches it WOULD run as a
//     JSON array of {"argv":[...]} objects and executes NOTHING — a safe preview
//     of the footgun the agent can inspect (or diff) before approving.
//   • Real execution runs each batch through jash_eval_line(), so every spawned
//     command is a first-class jash command: attested into the sigil chain,
//     checkpointed for undo, output-governed, memoΔ-tracked. `| xargs rm` becomes
//     recoverable.
//
// Scope: OWNS the common surface — default `echo`, -n N, -I REPLSTR, -0/--null,
// -t/--verbose (trace), -r/--no-run-if-empty, --, and --plan. Input splitting in
// default mode honours POSIX xargs quoting (blank/newline separators, backslash
// escapes, single/double-quoted runs) so it is parity-correct, not whitespace-
// naive. Any flag it does not own (-P parallel, -L, -s, -a, -p, -E, -d, -x, -i)
// falls back to the real xargs byte-for-byte. argv_fn (quote-aware).

#include "builtin.h"
#include "value_output.h"
#include <core/argv.h>
#include <parser/exec.h>
#include <parser/eval.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>

#define XARGS_MAXLINE 60000     /* flush a batch before the reconstructed line   */
#define XARGS_LINEBUF 65536     /* grows past this (leaves headroom in LINEBUF)  */
#define XARGS_MAXIN   ((size_t)64 * 1024 * 1024)  /* stdin cap before we bail out  */
#define XARGS_MAXITEMS 65536

/* ── fallback: reconstruct single-quoted argv → the real xargs ───────────────*/
static int xargs_fallback(jash_state_t* s, jash_writer_t w, void* ctx,
                          int argc, char** argv) {
    char* args = jash_argv_join(argc, argv);
    if (!args) return 125;
    int rc = jash_exec_external(s, w, ctx, "xargs", args);
    free(args);
    return rc;
}

/* ── tiny growable helpers ───────────────────────────────────────────────────*/
static int push_char(char** cur, size_t* cl, size_t* cc, char ch) {
    if (*cl + 1 > *cc) {
        size_t nc = *cc ? *cc * 2 : 32;
        char* t = realloc(*cur, nc);
        if (!t) return -1;
        *cur = t; *cc = nc;
    }
    (*cur)[(*cl)++] = ch;
    return 0;
}
static int items_push(char*** arr, int* n, int* cap, char* s) {
    if (*n == XARGS_MAXITEMS) { errno = E2BIG; return -1; }
    if (*n >= *cap) {
        int nc = *cap ? *cap * 2 : 16;
        char** t = (char**)realloc((void*)*arr, (size_t)nc * sizeof(char*));
        if (!t) return -1;
        *arr = t; *cap = nc;
    }
    (*arr)[(*n)++] = s;
    return 0;
}
static void free_items(char** items, int n) {
    for (int i = 0; i < n; i++) free(items[i]);
    free((void*)items);
}

/* ── read all of stdin into a heap buffer ────────────────────────────────────*/
static int read_all_stdin(char** out, size_t* out_len) {
    size_t cap = 65536, len = 0;
    char* b = malloc(cap);
    if (!b) return -1;
    char tmp[65536];
    size_t n;
    while ((n = fread(tmp, 1, sizeof tmp, stdin)) > 0) {
        if (len + n > XARGS_MAXIN) { free(b); return 1; }
        if (len + n > cap) {
            while (cap < len + n) cap *= 2;
            char* nb = realloc(b, cap);
            if (!nb) { free(b); return -1; }
            b = nb;
        }
        memcpy(b + len, tmp, n);
        len += n;
    }
    if (ferror(stdin)) { free(b); errno = EIO; return -1; }
    *out = b; *out_len = len;
    return 0;
}

/* ── input tokenizers ────────────────────────────────────────────────────────*/
/* Default mode: POSIX xargs quoting. Items are separated by blanks/newlines; a
 * backslash escapes the next char; single/double-quoted runs are literal (the
 * other quote and blanks included) until the matching close quote. */
static int split_words(const char* in, size_t n, char*** out, int* cnt, int lines) {
    char** arr = NULL; int na = 0, ca = 0;
    char* cur = NULL; size_t cl = 0, cc = 0; int started = 0;
    for (size_t i = 0; i < n; i++) {
        char c = in[i];
        if (!c) { errno = EINVAL; goto oom; }
        if (c == '\n' || (!lines && (c == ' ' || c == '\t'))) {
            if (started) {
                if (push_char(&cur, &cl, &cc, '\0') < 0) goto oom;
                cl--;                                   /* keep NUL, drop from len */
                if (items_push(&arr, &na, &ca, cur) < 0) goto oom;
                cur = NULL; cl = cc = 0; started = 0;
            }
            continue;
        }
        if (lines && !started && (c == ' ' || c == '\t')) continue;
        started = 1;
        if (c == '\\') {
            if (++i == n || !in[i]) { errno = EINVAL; goto oom; }
            if (push_char(&cur, &cl, &cc, in[i]) < 0) goto oom;
            continue;
        }
        if (c == '\'' || c == '"') {
            char quote = c;
            for (i++; i < n && in[i] != quote; i++) {
                if (!in[i] || in[i] == '\n') { errno = EINVAL; goto oom; }
                if (push_char(&cur, &cl, &cc, in[i]) < 0) goto oom;
            }
            if (i == n) { errno = EINVAL; goto oom; }
            continue;
        }
        if (push_char(&cur, &cl, &cc, c) < 0) goto oom;
    }
    if (started) {
        if (push_char(&cur, &cl, &cc, '\0') < 0) goto oom;
        if (items_push(&arr, &na, &ca, cur) < 0) goto oom;
        cur = NULL;
    }
    free(cur);
    *out = arr; *cnt = na;
    return 0;
 oom:
    free(cur);
    for (int k = 0; k < na; k++) free(arr[k]);
    free((void*)arr);
    return -1;
}

/* -0/--null: items separated by NUL, no quote processing (find -print0). */
static int split_null(const char* in, size_t n, char*** out, int* cnt) {
    char** arr = NULL; int na = 0, ca = 0; size_t start = 0;
    for (size_t i = 0; i < n; i++) {
        if (in[i] == '\0') {
            size_t len = i - start;
            char* s = malloc(len + 1);
            if (!s) goto oom;
            memcpy(s, in + start, len); s[len] = '\0';
            if (items_push(&arr, &na, &ca, s) < 0) { free(s); goto oom; }
            start = i + 1;
        }
    }
    if (start < n) {                                   /* trailing item, no NUL */
        size_t len = n - start;
        char* s = malloc(len + 1);
        if (!s) goto oom;
        memcpy(s, in + start, len); s[len] = '\0';
        if (items_push(&arr, &na, &ca, s) < 0) { free(s); goto oom; }
    }
    *out = arr; *cnt = na;
    return 0;
 oom:
    for (int k = 0; k < na; k++) free(arr[k]);
    free((void*)arr);
    return -1;
}

/* -I mode: one item per input line, leading/trailing blanks stripped, blank
 * lines skipped (GNU xargs -I semantics). */
/* ── reconstruction helpers ──────────────────────────────────────────────────*/
static int append_sq(char* buf, size_t* o, size_t cap, const char* s) {
    if (*o + 1 >= cap) return -1;
    buf[(*o)++] = '\'';
    for (const char* p = s; *p; p++) {
        if (*p == '\'') {
            if (*o + 4 >= cap) return -1;
            buf[(*o)++]='\''; buf[(*o)++]='\\'; buf[(*o)++]='\''; buf[(*o)++]='\'';
        } else {
            if (*o + 1 >= cap) return -1;
            buf[(*o)++] = *p;
        }
    }
    if (*o + 1 >= cap) return -1;
    buf[(*o)++] = '\'';
    return 0;
}
/* Replace every occurrence of needle in hay with item → out. -1 on overflow. */
static int subst(const char* hay, const char* needle, const char* item,
                 char* out, size_t cap) {
    size_t nl = strlen(needle), o = 0;
    const char* p = hay;
    while (*p) {
        if (nl && strncmp(p, needle, nl) == 0) {
            for (const char* q = item; *q; q++) { if (o + 1 >= cap) return -1; out[o++] = *q; }
            p += nl;
        } else {
            if (o + 1 >= cap) return -1;
            out[o++] = *p++;
        }
    }
    out[o] = '\0';
    return 0;
}

/* All structured plans are collected before the first result byte. */
typedef struct {
    jash_state_t* s; jash_writer_t w; void* ctx;
    char** cmd; int ncmd;
    int plan, trace, worst, stop;
    jdv_arena_t arena;
    jdv_value_t* rows;
    size_t count;
    jdv_limits_t limits;
} xctx_t;

static void aggregate_rc(xctx_t* x, int rc) {
    if (rc == 255) { x->worst = 124; x->stop = 1; }
    else if (rc != 0 && x->worst == 0) x->worst = 123;
}

static int plan_add(xctx_t* x, char* const* argv, size_t count) {
    if (x->count == XARGS_MAXITEMS || count > XARGS_MAXITEMS) { errno = E2BIG; return -1; }
    jdv_value_t* values = count ? calloc(count, sizeof(*values)) : NULL;
    if (count && !values) return -1;
    for (size_t i = 0; i < count; i++) {
        size_t length = strlen(argv[i]);
        values[i].kind = jash_json_utf8_valid_n(argv[i], length) ? JDV_STRING : JDV_BYTES;
        values[i].as.slice = (jdv_slice_t){(const uint8_t*)argv[i], length};
    }
    jdv_value_t list = {.kind = JDV_LIST, .as.list = {values, count}};
    jdv_record_input_t field = {JDV_LITERAL("argv"), list};
    int rc = jdv_value_record_limited(&x->arena, &field, 1, &x->rows[x->count], &x->limits);
    free(values);
    if (!rc) x->count++;
    return rc;
}

static void flush_argv(xctx_t* x, char* const* argv, size_t count) {
    if (x->stop) return;
    char line[XARGS_LINEBUF]; size_t used = 0;
    for (size_t i = 0; i < count; i++) {
        if (i) { if (used + 1 >= sizeof(line)) goto fail; line[used++] = ' '; }
        /* The evaluator sees exactly the argv displayed by --plan. Fixed
         * command arguments must be quoted too; they are already expanded. */
        if (append_sq(line, &used, sizeof(line), argv[i])) goto fail;
    }
    line[used] = '\0';
    if (x->plan) {
        if (plan_add(x, argv, count)) goto fail;
        return;
    }
    if (x->trace) jash_writef(jash_stderr_writer, NULL, "+ %s\n", line);
    aggregate_rc(x, jash_eval_line(x->s, x->w, x->ctx, line));
    return;
fail:
    jash_writes(jash_stderr_writer, NULL, "xargs: command or plan exceeds its bounds\n");
    x->worst = 1; x->stop = 1;
}

static void flush_batch(xctx_t* x, char** items, int count) {
    if (x->stop) return;
    size_t total = (size_t)x->ncmd + (size_t)count;
    if (total > XARGS_MAXITEMS) { x->worst = 1; x->stop = 1; return; }
    char** argv = malloc(total * sizeof(*argv));
    if (!argv) { x->worst = 1; x->stop = 1; return; }
    memcpy(argv, x->cmd, (size_t)x->ncmd * sizeof(*argv));
    if (count) memcpy(argv + x->ncmd, items, (size_t)count * sizeof(*argv));
    flush_argv(x, argv, total);
    free(argv);
}

static void flush_I(xctx_t* x, const char* replacement, const char* item) {
    if (x->stop) return;
    char** argv = calloc((size_t)x->ncmd, sizeof(*argv));
    if (!argv) { x->worst = 1; x->stop = 1; return; }
    char resolved[XARGS_LINEBUF];
    size_t total = 0;
    int i = 0;
    for (; i < x->ncmd; i++) {
        if (subst(x->cmd[i], replacement, item, resolved, sizeof(resolved))) break;
        size_t length = strlen(resolved);
        if (length > XARGS_LINEBUF - total) break;
        total += length;
        argv[i] = strdup(resolved);
        if (!argv[i]) break;
    }
    if (i == x->ncmd) flush_argv(x, argv, (size_t)x->ncmd);
    else { x->worst = 1; x->stop = 1; }
    for (int k = 0; k < x->ncmd; k++) free(argv[k]);
    free(argv);
}

/* Inspect only the option prefix. Values of known upstream options and
 * command operands cannot accidentally select preview or a wire format. */
static int prefix_modes(int argc, char** argv, int* plan, int* mode) {
    *plan = 0; *mode = 0;
    int conflict = 0;
    for (int i = 0; i < argc; i++) {
        const char* arg = argv[i];
        if (!strcmp(arg, "--") || arg[0] != '-' || !arg[1]) break;
        if (!strcmp(arg, "--plan") || !strcmp(arg, "--dry-run")) { *plan = 1; continue; }
        int selected = !strcmp(arg, "--json") ? 1
            : (!strcmp(arg, "--data") || !strcmp(arg, "--data-v2")) ? 2 : 0;
        if (selected) { if (*mode && *mode != selected) conflict = 1; *mode = selected; continue; }
        if ((arg[0] == '-' && arg[1] && !arg[2] && strchr("nILsPaEd", arg[1])) ||
            !strcmp(arg, "--max-args") || !strcmp(arg, "--replace") ||
            !strcmp(arg, "--max-lines") || !strcmp(arg, "--max-chars") ||
            !strcmp(arg, "--max-procs") || !strcmp(arg, "--arg-file") ||
            !strcmp(arg, "--eof") || !strcmp(arg, "--delimiter")) i++;
    }
    return conflict ? -1 : 0;
}

/* Used by the runtime capability classifier. Invalid preview combinations
 * still refuse before execution; a command operand named --plan is not one. */
int jash_xargs_plan_mode(int argc, char** argv) {
    int plan, mode;
    (void)prefix_modes(argc, argv, &plan, &mode);
    return plan;
}

int jash_b_xargs(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int plan = 0, mode = 0;
    if (prefix_modes(argc, argv, &plan, &mode) || (mode && !plan)) {
        jash_writes(jash_stderr_writer, NULL, "xargs: output formats require --plan and must not conflict\n");
        return 2;
    }

    /* Pass 2: parse the owned flags. An unowned flag → fallback (or, in plan
     * mode, a loud error — the real xargs has no --plan to fall back onto). */
    int null_mode = 0, no_empty = 0, trace = 0, i = 0;
    long nflag = 0;
    const char* replstr = NULL;
    for (; i < argc; i++) {
        const char* a = argv[i];
        if (a[0] != '-' || a[1] == '\0') break;              /* operand / '-' alone */
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "--plan") == 0 || strcmp(a, "--dry-run") == 0) { plan = 1; continue; }
        if (!strcmp(a, "--json") || !strcmp(a, "--data") || !strcmp(a, "--data-v2")) continue;
        if (strcmp(a, "--null") == 0)             { null_mode = 1; continue; }
        if (strcmp(a, "--no-run-if-empty") == 0)  { no_empty = 1;  continue; }
        if (strcmp(a, "--verbose") == 0)          { trace = 1;     continue; }
        if (strcmp(a, "-0") == 0)                 { null_mode = 1; continue; }
        if (strcmp(a, "-r") == 0)                 { no_empty = 1;  continue; }
        if (strcmp(a, "-t") == 0)                 { trace = 1;     continue; }
        if (strncmp(a, "-n", 2) == 0) {
            const char* v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (!v) { jash_writef(jash_stderr_writer, 0, "xargs: option requires an argument -- 'n'\n"); return 1; }
            errno = 0; char* end;
            nflag = strtol(v, &end, 10);
            if (errno || *end || nflag < 1) {
                jash_writes(jash_stderr_writer, NULL, "xargs: -n requires a positive integer\n");
                return 2;
            }
            continue;
        }
        if (strncmp(a, "-I", 2) == 0) {
            replstr = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
            if (!replstr || !*replstr) { jash_writef(jash_stderr_writer, 0, "xargs: option requires an argument -- 'I'\n"); return 1; }
            continue;
        }
        if (plan) { jash_writef(jash_stderr_writer, 0, "xargs: --plan cannot combine with %s\n", a); return 1; }
        return xargs_fallback(s, w, ctx, argc, argv);
    }

    /* Command tokens = argv[i..]; POSIX default is `echo`. */
    char  echo_buf[] = "echo";
    char* fallback_cmd[1] = { echo_buf };
    char** cmd; int ncmd;
    if (i < argc) { cmd = &argv[i]; ncmd = argc - i; }
    else          { cmd = fallback_cmd; ncmd = 1; }

    /* Read + tokenize stdin. */
    char* buf = NULL; size_t blen = 0;
    int rr = read_all_stdin(&buf, &blen);
    if (rr != 0) { jash_writef(jash_stderr_writer, 0, "xargs: %s\n", rr > 0 ? "input too large" : "out of memory"); return 1; }

    char** items = NULL; int nit = 0, trc;
    if (null_mode) trc = split_null(buf, blen, &items, &nit);
    else trc = split_words(buf, blen, &items, &nit, replstr != NULL);
    free(buf);
    if (trc != 0) { jash_writef(jash_stderr_writer, 0, "xargs: malformed or oversized input\n"); return 1; }

    xctx_t x = {.s=s, .w=w, .ctx=ctx, .cmd=cmd, .ncmd=ncmd, .plan=plan, .trace=trace};
    void* storage = NULL;
    if (plan) {
        storage = malloc(XARGS_MAXIN);
        x.rows = calloc(XARGS_MAXITEMS, sizeof(*x.rows));
        if (!storage || !x.rows) { free(storage); free(x.rows); free_items(items, nit); return 125; }
        jdv_arena_init(&x.arena, storage, XARGS_MAXIN);
        x.limits = JDV_DEFAULT_LIMITS;
        x.limits.max_aggregate_bytes = XARGS_MAXIN;
        x.limits.max_encoded_bytes = XARGS_MAXIN;
        x.limits.max_list_items = 262144u; x.limits.max_nodes = 393216u;
        x.limits.max_record_fields = XARGS_MAXITEMS;
    }

    if (replstr) {
        for (int k = 0; k < nit && !x.stop; k++) flush_I(&x, replstr, items[k]);
    } else {
        long cap_n = nflag > 0 ? nflag : LONG_MAX;       /* no -n → as few execs as fit */
        size_t base = 0;
        for (int c = 0; c < ncmd; c++) base += strlen(cmd[c]) * 4u + 4u;
        char** batch = nit > 0 ? (char**)malloc((size_t)nit * sizeof(char*)) : NULL;
        if (nit > 0 && !batch) { free(storage); free(x.rows); free_items(items, nit); return 125; }
        int bn = 0; size_t est = base;
        for (int k = 0; k < nit && !x.stop; k++) {
            size_t ilen = strlen(items[k]) * 4 + 4;      /* worst-case single-quoted */
            if (bn > 0 && ((long)bn >= cap_n || est + ilen > XARGS_MAXLINE)) {
                flush_batch(&x, batch, bn); bn = 0; est = base;
            }
            batch[bn++] = items[k]; est += ilen;
        }
        if (!x.stop) {
            if (bn > 0)                       flush_batch(&x, batch, bn);
            else if (nit == 0 && !no_empty)   flush_batch(&x, NULL, 0);  /* run once, no args */
        }
        free((void*)batch);
    }

    if (plan && !x.worst) {
        jdv_value_t list = {.kind = JDV_LIST, .as.list = {x.rows, x.count}};
        jdv_schema_t schema = {.kind = JDV_SCHEMA_RECORD, .as.record = {.open = 1}};
        jdv_limits_t native_limits = x.limits;
        native_limits.max_encoded_bytes = 128u * 1024u;
        native_limits.max_aggregate_bytes = 128u * 1024u;
        x.worst = mode == 2 ? value_output_data(w, ctx, x.rows, x.count, NULL,
            &schema, &native_limits, XARGS_MAXIN, 0, NULL)
            : value_output_json(w, ctx, "xargs", &list, &x.limits, XARGS_MAXIN, 0);
    }
    free(storage); free(x.rows);
    free_items(items, nit);
    return x.worst;
}
