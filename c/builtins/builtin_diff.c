// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/diff — Myers O(ND) diff engine, structured-JSON gold
// (ADR-0044). Design note: Docs/ADR-0044-diff-engine-design.md.
//
// Why a builtin: an agent that wants "what changed between A and B" has to
// parse `<`/`>`/`@@` markup out of GNU/BSD diff — different on every platform,
// unbounded output, no structure. The GOLD is `diff --json A B`: a JSON array
// of hunks `{op,a:{start,count},b:{start,count},a_lines,b_lines}` — no header
// parsing, auto-governed output (never floods the agent window).
//
// Engine: Myers' O(ND) greedy edit-graph algorithm (forward pass finds the
// edit distance D via furthest-reaching paths per diagonal, snapshotting the
// full V array each step; backtrack through the snapshots recovers the actual
// edit script). Lines compared by a precomputed FNV-1a hash + length + memcmp,
// not per-step strcmp.
//
// THE PARITY TRAP: two correct diffs of the same input can disagree byte-for-
// byte on hunk grouping — GNU/BSD/git diff often disagree while all valid. So
// this builtin is NOT tested for byte-exact parity vs /usr/bin/diff; it is
// tested by round-trip (`diff -u a b | patch` reproduces `b`).
//
// Scope: OWNS two regular-file operands, normal format, unified (-u/-U N),
// --json, the "\ No newline at end of file" marker, and rc 0/1/2. Falls back
// to the real diff (single-quoted argv reconstruction) for every other flag,
// directory/binary/oversize operands, or inputs too complex for the bounded
// in-process backtrack. argv_fn (quote-aware) — never the legacy string path.

#include "builtin.h"
#include <parser/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <errno.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <osif/osif_stat.h>
#include <core/structured_output.h>
#include "query_data.h"
#include "diff_query_api.h"

#define DIFF_MAX_FILE_BYTES (8L*1024*1024)
#define DIFF_MAX_LINES       200000
#define DIFF_MAX_TRACE_BYTES (64L*1024*1024)

typedef struct {
    jash_state_t* state;
    uint64_t offset, limit;
    const char* path_a;
    const char* path_b;
    const char* logical_path;
    const char* checkpoint;
    const char* command;
    const char* error;
    int native, cancelled, emitted;
} diff_query_t;
static diff_query_t* diff_query;

static int diff_query_poll(void) {
    if (diff_query && !diff_query->cancelled)
        diff_query->cancelled = jash_query_cancel_poll(diff_query->state);
    return diff_query && diff_query->cancelled;
}

static int diff_input_failure(const char* code, int result) {
    if (diff_query) diff_query->error = code;
    return result;
}

static int diff_same_file(const struct stat* a, const struct stat* b) {
    return a->st_dev == b->st_dev && a->st_ino == b->st_ino && a->st_size == b->st_size &&
        BEAR_ST_MTIM(*a).tv_sec == BEAR_ST_MTIM(*b).tv_sec &&
        BEAR_ST_MTIM(*a).tv_nsec == BEAR_ST_MTIM(*b).tv_nsec &&
        BEAR_ST_CTIM(*a).tv_sec == BEAR_ST_CTIM(*b).tv_sec &&
        BEAR_ST_CTIM(*a).tv_nsec == BEAR_ST_CTIM(*b).tv_nsec;
}

static uint64_t diff_fnv1a64(const char* p, size_t len) {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; i++) { h ^= (unsigned char)p[i]; h *= 1099511628211ULL; }
    return h;
}

typedef struct { const char* ptr; size_t len; uint64_t hash; } diff_line_t;

typedef struct {
    char* buf;
    diff_line_t* lines;
    int count;
    int has_final_nl;   /* 1 unless the file is non-empty and its last byte isn't '\n' */
} diff_file_t;

static int diff_lines_eq(const diff_file_t* A, int i, const diff_file_t* B, int j) {
    const diff_line_t* a = &A->lines[i];
    const diff_line_t* b = &B->lines[j];
    int a_newline = i + 1 < A->count || A->has_final_nl;
    int b_newline = j + 1 < B->count || B->has_final_nl;
    return a_newline == b_newline && a->hash == b->hash && a->len == b->len &&
           (a->len == 0 || memcmp(a->ptr, b->ptr, a->len) == 0);
}

/* 0 ok, 1 unsuitable for in-process diff (dir/binary/oversize — caller should
 * fall back), -1 hard error (errno set — caller owns the error message). */
static int diff_read_file(const char* path, diff_file_t* out) {
    memset(out, 0, sizeof *out);
    struct stat st;
    if (diff_query_poll()) return diff_input_failure("request_cancelled", 1);
    int descriptor = open(path, O_RDONLY | O_NONBLOCK | O_CLOEXEC);
    if (descriptor < 0) return diff_input_failure("open_failed", -1);
    if (fstat(descriptor, &st) != 0) { close(descriptor); return diff_input_failure("stat_failed", -1); }
    if (!S_ISREG(st.st_mode)) { close(descriptor); return diff_input_failure("non_regular_input", 1); }
    if (st.st_size < 0 || st.st_size > DIFF_MAX_FILE_BYTES) {
        close(descriptor); return diff_input_failure("input_size_limit", 1);
    }
    FILE* f = fdopen(descriptor, "rb");
    if (!f) { int saved = errno; close(descriptor); errno = saved; return diff_input_failure("open_failed", -1); }
    size_t cap = (size_t)st.st_size + 1;
    char* buf = malloc(cap);
    if (!buf) { (void)fclose(f); errno = ENOMEM; return diff_input_failure("allocation_failed", -1); }
    size_t n = 0;
    while (n < (size_t)st.st_size) {
        if (diff_query_poll()) { free(buf); fclose(f); return diff_input_failure("request_cancelled", 1); }
        size_t chunk = (size_t)st.st_size - n;
        if (chunk > 65536u) chunk = 65536u;
        size_t got = fread(buf + n, 1, chunk, f);
        n += got;
        if (got < chunk) break;
    }
    int failed_read = ferror(f);
    int extra = failed_read ? EOF : fgetc(f);
    failed_read |= ferror(f);
    struct stat after, named;
    int changed = n != (size_t)st.st_size || extra != EOF ||
        fstat(fileno(f), &after) != 0 || stat(path, &named) != 0 ||
        !diff_same_file(&st, &after) || !diff_same_file(&st, &named);
    int failed_close = fclose(f) != 0;
    if (diff_query_poll() || failed_read || changed || failed_close) {
        free(buf); errno = EIO;
        return diff_input_failure(diff_query && diff_query->cancelled ? "request_cancelled" :
            failed_read ? "read_failed" : changed ? "changed_during_read" : "close_failed", -1);
    }
    buf[n] = '\0';

    if (memchr(buf, 0, n)) { free(buf); return diff_input_failure("binary_input", 1); }

    out->buf = buf;
    out->has_final_nl = (n == 0) ? 1 : (buf[n-1] == '\n');

    int cap_lines = 64;
    out->lines = malloc(sizeof(diff_line_t) * (size_t)cap_lines);
    if (!out->lines) { free(buf); memset(out, 0, sizeof *out); return diff_input_failure("allocation_failed", 1); }
    size_t i = 0;
    while (i < n) {
        size_t start = i;
        while (i < n && buf[i] != '\n') i++;
        size_t len = i - start;
        if (out->count >= cap_lines) {
            cap_lines *= 2;
            diff_line_t* grown = realloc(out->lines, sizeof(diff_line_t) * (size_t)cap_lines);
            if (!grown) { free(buf); free(out->lines); memset(out, 0, sizeof *out); return diff_input_failure("allocation_failed", 1); }
            out->lines = grown;
        }
        diff_line_t* ln = &out->lines[out->count++];
        ln->ptr = &buf[start]; ln->len = len;
        ln->hash = diff_fnv1a64(ln->ptr, ln->len);
        if (out->count > DIFF_MAX_LINES) { free(buf); free(out->lines); memset(out, 0, sizeof *out); return diff_input_failure("input_line_limit", 1); }
        if (i < n) i++;
    }
    return 0;
}

static void diff_free_file(diff_file_t* f) {
    free(f->buf); free(f->lines);
    f->buf = NULL; f->lines = NULL;
}

/* Reconstruct single-quoted argv and defer to the real diff. */
static int diff_fallback(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    char args[8192];
    size_t o = 0;
    for (int i = 0; i < argc && o + 4 < sizeof args; i++) {
        if (i) args[o++] = ' ';
        args[o++] = '\'';
        for (const char* p = argv[i]; *p && o + 5 < sizeof args; p++) {
            if (*p == '\'') { args[o++]='\''; args[o++]='\\'; args[o++]='\''; args[o++]='\''; }
            else args[o++] = *p;
        }
        args[o++] = '\'';
    }
    args[o] = '\0';
    return jash_exec_external(s, w, ctx, "diff", args);
}

typedef enum { DIFF_OP_EQ, DIFF_OP_DEL, DIFF_OP_INS } diff_opkind_t;
typedef struct { diff_opkind_t kind; int a_idx, b_idx; } diff_op_t;
typedef struct { diff_op_t* items; int count, cap, failed; } diff_oplist_t;

static void diff_oplist_push(diff_oplist_t* L, diff_opkind_t k, int a, int b) {
    if (L->failed) return;
    if (L->count >= L->cap) {
        int new_cap = L->cap ? L->cap * 2 : 64;
        diff_op_t* grown = realloc(L->items, sizeof(diff_op_t) * (size_t)new_cap);
        if (!grown) { L->failed = 1; return; }
        L->items = grown;
        L->cap = new_cap;
    }
    L->items[L->count].kind = k;
    L->items[L->count].a_idx = a;
    L->items[L->count].b_idx = b;
    L->count++;
}

/* Myers O(ND): forward pass finds edit distance D (snapshotting the full V
 * array per step), then backtracks through the snapshots to recover the edit
 * script. Returns 0 with `ops` filled (forward order), or 1 if the bounded
 * trace-memory budget was exceeded (caller should fall back to real diff —
 * this only happens for large, heavily-dissimilar inputs; near-identical
 * files of any size we accept stay far under the cap since D is small). */
static int myers_diff(const diff_file_t* A, const diff_file_t* B, diff_oplist_t* ops) {
    int n = A->count, m = B->count;
    int max_d = n + m;
    int offset = max_d;
    int vsize = 2 * max_d + 3;
    int* v = calloc((size_t)vsize, sizeof(int));
    if (!v) return 1;
    v[1 + offset] = 0;

    int** trace = NULL;
    int trace_cap = 0, trace_n = 0;
    size_t trace_bytes = 0;
    int found_d = -1;

    for (int d = 0; d <= max_d; d++) {
        if (diff_query_poll()) break;
        int done = 0;
        for (int k = -d; k <= d; k += 2) {
            int x;
            if (k == -d || (k != d && v[k-1+offset] < v[k+1+offset])) x = v[k+1+offset];
            else x = v[k-1+offset] + 1;
            int y = x - k;
            while (x < n && y < m && diff_lines_eq(A, x, B, y)) {
                x++; y++;
                if ((x & 1023) == 0 && diff_query_poll()) break;
            }
            v[k+offset] = x;
            if (x >= n && y >= m) done = 1;
        }
        int* row = malloc(sizeof(int) * (size_t)vsize);
        if (!row) { for (int t = 0; t < trace_n; t++) free(trace[t]); free(trace); free(v); return 1; }
        memcpy(row, v, sizeof(int) * (size_t)vsize);
        trace_bytes += sizeof(int) * (size_t)vsize;
        if (trace_bytes > DIFF_MAX_TRACE_BYTES) {
            free(row);
            for (int t = 0; t < trace_n; t++) free(trace[t]);
            free(trace); free(v);
            return 1;
        }
        if (trace_n >= trace_cap) {
            int new_cap = trace_cap ? trace_cap * 2 : 64;
            int** grown = realloc(trace, sizeof(int*) * (size_t)new_cap);
            if (!grown) {
                free(row);
                for (int t = 0; t < trace_n; t++) free(trace[t]);
                free(trace); free(v);
                return 1;
            }
            trace = grown;
            trace_cap = new_cap;
        }
        trace[trace_n++] = row;
        if (done) { found_d = d; break; }
    }
    free(v);
    if (found_d < 0) {
        for (int t = 0; t < trace_n; t++) free(trace[t]);
        free(trace);
        return 1;
    }

    int x = n, y = m;
    diff_oplist_t rev = {0};
    for (int d = found_d; d >= 0; d--) {
        int* row = trace[d];
        int k = x - y;
        int prev_k;
        if (k == -d || (k != d && row[k-1+offset] < row[k+1+offset])) prev_k = k + 1;
        else prev_k = k - 1;
        int prev_x = row[prev_k+offset];
        int prev_y = prev_x - prev_k;
        while (x > prev_x && y > prev_y) {
            diff_oplist_push(&rev, DIFF_OP_EQ, x-1, y-1);
            x--; y--;
        }
        if (d > 0) {
            if (x == prev_x) diff_oplist_push(&rev, DIFF_OP_INS, -1, y-1);
            else              diff_oplist_push(&rev, DIFF_OP_DEL, x-1, -1);
        }
        x = prev_x; y = prev_y;
    }
    for (int t = 0; t < trace_n; t++) free(trace[t]);
    free(trace);

    if (rev.failed) { free(rev.items); return diff_input_failure("allocation_failed", 1); }

    ops->count = rev.count;
    ops->cap = rev.count ? rev.count : 1;
    ops->items = malloc(sizeof(diff_op_t) * (size_t)ops->cap);
    if (!ops->items) { free(rev.items); return diff_input_failure("allocation_failed", 1); }
    for (int i = 0; i < rev.count; i++) ops->items[i] = rev.items[rev.count-1-i];
    free(rev.items);
    return 0;
}

typedef struct { int a_start, a_count, b_start, b_count; } diff_hunk_t;

/* Group the op stream into maximal non-equal runs (a "change"/"delete"/
 * "insert" hunk each) separated by equal context — the same grouping normal
 * format, unified format, and --json all build on. */
static diff_hunk_t* build_hunks(diff_op_t* ops, int nops, int* out_n) {
    int cap = 16, n = 0;
    diff_hunk_t* hunks = malloc(sizeof(diff_hunk_t) * (size_t)cap);
    if (!hunks) { *out_n = -1; return NULL; }
    int a_pos = 0, b_pos = 0, i = 0;
    while (i < nops) {
        if (ops[i].kind == DIFF_OP_EQ) { a_pos++; b_pos++; i++; continue; }
        diff_hunk_t h = { a_pos, 0, b_pos, 0 };
        while (i < nops && ops[i].kind != DIFF_OP_EQ) {
            if (ops[i].kind == DIFF_OP_DEL) { h.a_count++; a_pos++; }
            else                            { h.b_count++; b_pos++; }
            i++;
        }
        if (n >= cap) {
            int new_cap = cap * 2;
            diff_hunk_t* grown = realloc(hunks, sizeof(diff_hunk_t) * (size_t)new_cap);
            if (!grown) { free(hunks); *out_n = -1; return NULL; }
            hunks = grown;
            cap = new_cap;
        }
        hunks[n++] = h;
    }
    *out_n = n;
    return hunks;
}

#include "diff_query.h"

static int emit_json(jash_writer_t writer, void* context, const diff_hunk_t* hunks, int number,
                       const diff_file_t* a, const diff_file_t* b) {
    if (diff_query) diff_query->emitted = 1;
    if (diff_query && diff_query->native) return diff_native_emit(writer, context, hunks, number, a, b);
    if (diff_query_poll()) return diff_query_error(writer, context, diff_query->cancelled);
    jash_structured_output_t output;
    if (jash_structured_begin(&output, writer, context) < 0) return 125;
    jash_json_writer_t* json = &output.json;
    jash_jsonw_raw(json, "[");
    for (int index = 0; index < number; index++) {
        const diff_hunk_t* hunk = &hunks[index];
        if (index) jash_jsonw_raw(json, ",");
        jash_jsonw_raw(json, "{\"op\":");
        jash_jsonw_string(json, hunk->a_count && hunk->b_count ? "change" : hunk->a_count ? "delete" : "insert");
        jash_jsonw_raw(json, ",\"a\":{\"start\":"); jash_jsonw_u64(json, (uint64_t)hunk->a_start);
        jash_jsonw_raw(json, ",\"count\":"); jash_jsonw_u64(json, (uint64_t)hunk->a_count);
        jash_jsonw_raw(json, "},\"b\":{\"start\":"); jash_jsonw_u64(json, (uint64_t)hunk->b_start);
        jash_jsonw_raw(json, ",\"count\":"); jash_jsonw_u64(json, (uint64_t)hunk->b_count);
        jash_jsonw_raw(json, "},\"a_lines\":[");
        for (int i = 0; i < hunk->a_count; i++) {
            if (i) jash_jsonw_raw(json, ",");
            const diff_line_t* line = &a->lines[hunk->a_start + i];
            jash_jsonw_text_or_bytes(json, line->ptr, line->len);
        }
        jash_jsonw_raw(json, "],\"b_lines\":[");
        for (int i = 0; i < hunk->b_count; i++) {
            if (i) jash_jsonw_raw(json, ",");
            const diff_line_t* line = &b->lines[hunk->b_start + i];
            jash_jsonw_text_or_bytes(json, line->ptr, line->len);
        }
        jash_jsonw_raw(json, "],\"a_final_newline\":"); jash_jsonw_raw(json, a->has_final_nl ? "true" : "false");
        jash_jsonw_raw(json, ",\"b_final_newline\":"); jash_jsonw_raw(json, b->has_final_nl ? "true" : "false");
        if (diff_query && diff_query->logical_path) {
            jash_jsonw_raw(json, ",\"path\":"); jash_jsonw_string_or_bytes(json, diff_query->logical_path);
            jash_jsonw_raw(json, ",\"checkpoint\":"); jash_jsonw_string_or_bytes(json, diff_query->checkpoint);
        }
        jash_jsonw_raw(json, "}");
    }
    jash_jsonw_raw(json, "]\n");
    return jash_structured_finish(&output, number ? 1 : 0);
}

static const char* DIFF_NO_NL = "\\ No newline at end of file\n";

static void print_line_prefixed(jash_writer_t w, void* ctx, const char* prefix, const diff_line_t* ln) {
    w(ctx, prefix, strlen(prefix));
    w(ctx, ln->ptr, ln->len);
    w(ctx, "\n", 1);
}

static void print_range(jash_writer_t w, void* ctx, int start0, int count) {
    char b[64]; int k;
    if (count == 1) k = snprintf(b, sizeof b, "%d", start0+1);
    else            k = snprintf(b, sizeof b, "%d,%d", start0+1, start0+count);
    w(ctx, b, (size_t)k);
}

static void emit_normal(jash_writer_t w, void* ctx, diff_hunk_t* hunks, int nh,
                        const diff_file_t* A, const diff_file_t* B) {
    for (int h = 0; h < nh; h++) {
        diff_hunk_t* H = &hunks[h];
        char op = (H->a_count > 0 && H->b_count > 0) ? 'c' : (H->a_count > 0 ? 'd' : 'a');
        if (op == 'c') {
            print_range(w, ctx, H->a_start, H->a_count);
            w(ctx, "c", 1);
            print_range(w, ctx, H->b_start, H->b_count);
        } else if (op == 'd') {
            print_range(w, ctx, H->a_start, H->a_count);
            w(ctx, "d", 1);
            char b[32]; int k = snprintf(b, sizeof b, "%d", H->b_start); w(ctx, b, (size_t)k);
        } else {
            char b[32]; int k = snprintf(b, sizeof b, "%d", H->a_start); w(ctx, b, (size_t)k);
            w(ctx, "a", 1);
            print_range(w, ctx, H->b_start, H->b_count);
        }
        w(ctx, "\n", 1);
        for (int i = 0; i < H->a_count; i++) {
            print_line_prefixed(w, ctx, "< ", &A->lines[H->a_start+i]);
            if (i == H->a_count-1 && H->a_start+H->a_count == A->count && !A->has_final_nl)
                w(ctx, DIFF_NO_NL, strlen(DIFF_NO_NL));
        }
        if (op == 'c') w(ctx, "---\n", 4);
        for (int i = 0; i < H->b_count; i++) {
            print_line_prefixed(w, ctx, "> ", &B->lines[H->b_start+i]);
            if (i == H->b_count-1 && H->b_start+H->b_count == B->count && !B->has_final_nl)
                w(ctx, DIFF_NO_NL, strlen(DIFF_NO_NL));
        }
    }
}

typedef struct { int a_lo, a_hi, b_lo, b_hi, first_h, last_h; } udiff_group_t;

/* Merge internal hunks whose surrounding context (2*context gap) would
 * overlap into one unified @@ block — the same rule GNU/BSD unified diff use. */
static int build_ugroups(diff_hunk_t* hunks, int nh, int context, int n, int m, udiff_group_t** out) {
    udiff_group_t* groups = malloc(sizeof(udiff_group_t) * (size_t)(nh ? nh : 1));
    int ng = 0, i = 0;
    while (i < nh) {
        int raw_a_end = hunks[i].a_start + hunks[i].a_count;
        int raw_b_end = hunks[i].b_start + hunks[i].b_count;
        int j = i;
        while (j+1 < nh) {
            int gap = hunks[j+1].a_start - raw_a_end;
            if (gap > 2*context) break;
            raw_a_end = hunks[j+1].a_start + hunks[j+1].a_count;
            raw_b_end = hunks[j+1].b_start + hunks[j+1].b_count;
            j++;
        }
        udiff_group_t g;
        g.a_lo = hunks[i].a_start - context; if (g.a_lo < 0) g.a_lo = 0;
        g.b_lo = hunks[i].b_start - context; if (g.b_lo < 0) g.b_lo = 0;
        g.a_hi = raw_a_end + context; if (g.a_hi > n) g.a_hi = n;
        g.b_hi = raw_b_end + context; if (g.b_hi > m) g.b_hi = m;
        g.first_h = i; g.last_h = j;
        groups[ng++] = g;
        i = j+1;
    }
    *out = groups;
    return ng;
}

static void emit_unified(jash_writer_t w, void* ctx, diff_hunk_t* hunks, int nh,
                         const diff_file_t* A, const diff_file_t* B, int context,
                         const char* label_a, const char* label_b) {
    if (nh == 0) return;
    udiff_group_t* groups = NULL;
    int ng = build_ugroups(hunks, nh, context, A->count, B->count, &groups);

    w(ctx, "--- ", 4); w(ctx, label_a, strlen(label_a)); w(ctx, "\n", 1);
    w(ctx, "+++ ", 4); w(ctx, label_b, strlen(label_b)); w(ctx, "\n", 1);

    for (int g = 0; g < ng; g++) {
        udiff_group_t* G = &groups[g];
        int a_len = G->a_hi - G->a_lo, b_len = G->b_hi - G->b_lo;
        char hdr[128]; int k = 0;
        k += snprintf(hdr+k, sizeof(hdr)-(size_t)k, "@@ -");
        if (a_len == 1) k += snprintf(hdr+k, sizeof(hdr)-(size_t)k, "%d", G->a_lo+1);
        else            k += snprintf(hdr+k, sizeof(hdr)-(size_t)k, "%d,%d", a_len==0?G->a_lo:G->a_lo+1, a_len);
        k += snprintf(hdr+k, sizeof(hdr)-(size_t)k, " +");
        if (b_len == 1) k += snprintf(hdr+k, sizeof(hdr)-(size_t)k, "%d", G->b_lo+1);
        else            k += snprintf(hdr+k, sizeof(hdr)-(size_t)k, "%d,%d", b_len==0?G->b_lo:G->b_lo+1, b_len);
        k += snprintf(hdr+k, sizeof(hdr)-(size_t)k, " @@\n");
        w(ctx, hdr, (size_t)k);

        int pa = G->a_lo, pb = G->b_lo;
        for (int h = G->first_h; h <= G->last_h; h++) {
            diff_hunk_t* H = &hunks[h];
            while (pa < H->a_start) { print_line_prefixed(w, ctx, " ", &A->lines[pa]); pa++; pb++; }
            for (int i2 = 0; i2 < H->a_count; i2++) {
                print_line_prefixed(w, ctx, "-", &A->lines[H->a_start+i2]);
                if (i2 == H->a_count-1 && H->a_start+H->a_count == A->count && !A->has_final_nl)
                    w(ctx, DIFF_NO_NL, strlen(DIFF_NO_NL));
            }
            pa = H->a_start + H->a_count;
            for (int i2 = 0; i2 < H->b_count; i2++) {
                print_line_prefixed(w, ctx, "+", &B->lines[H->b_start+i2]);
                if (i2 == H->b_count-1 && H->b_start+H->b_count == B->count && !B->has_final_nl)
                    w(ctx, DIFF_NO_NL, strlen(DIFF_NO_NL));
            }
            pb = H->b_start + H->b_count;
        }
        while (pa < G->a_hi) { print_line_prefixed(w, ctx, " ", &A->lines[pa]); pa++; pb++; }
        (void)pb;
    }
    free(groups);
}

static int diff_run(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    /* Pass 1: is --json requested (before any `--`)? Mirrors stat's own gate:
     * --json has no fallback equivalent, so once it's asked for we must error
     * rather than silently hand the args to real diff and lose it. */
    int json_present = 0;
    for (int j = 0; j < argc; j++) {
        if (strcmp(argv[j], "--") == 0) break;
        if (strcmp(argv[j], "--json") == 0) { json_present = 1; break; }
    }

    int unified = 0, json = 0, context = 3, end_opts = 0;
    int operand_idx[2] = {0, 0}, operand_n = 0, bad = 0;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (!end_opts && strcmp(a, "--") == 0) { end_opts = 1; continue; }
        if (!end_opts && a[0] == '-' && a[1] != '\0') {
            if (strcmp(a, "-u") == 0) { unified = 1; continue; }
            if (strcmp(a, "--json") == 0) { json = 1; continue; }
            if (strncmp(a, "-U", 2) == 0) {
                unified = 1;
                if (a[2] != '\0') context = atoi(a+2);
                else if (i+1 < argc) context = atoi(argv[++i]);
                else { bad = 1; break; }
                continue;
            }
            bad = 1; break;
        }
        if (operand_n < 2) operand_idx[operand_n] = i;
        operand_n++;
    }

    if (bad || operand_n != 2) {
        if (json_present) {
            jash_writef(jash_stderr_writer, 0, "diff: --json: unsupported flags or operand count\n");
            return 2;
        }
        return diff_fallback(s, w, ctx, argc, argv);
    }

    const char* pathA = argv[operand_idx[0]];
    const char* pathB = argv[operand_idx[1]];
    if (diff_query) { diff_query->path_a = pathA; diff_query->path_b = pathB; }

    diff_file_t A, B;
    int ra = diff_read_file(pathA, &A);
    if (ra < 0) { jash_writef(jash_stderr_writer, 0, "diff: %s: %s\n", pathA, strerror(errno)); return 2; }
    int rb = diff_read_file(pathB, &B);
    if (rb < 0) { diff_free_file(&A); jash_writef(jash_stderr_writer, 0, "diff: %s: %s\n", pathB, strerror(errno)); return 2; }
    if (ra > 0 || rb > 0) {
        diff_free_file(&A); diff_free_file(&B);
        if (json) { jash_writef(jash_stderr_writer, 0, "diff: --json: input not suitable for in-process diff (binary/oversize/dir)\n"); return 2; }
        return diff_fallback(s, w, ctx, argc, argv);
    }

    diff_oplist_t ops = {0};
    int rc_myers = myers_diff(&A, &B, &ops);
    if (rc_myers != 0) {
        if (diff_query && !diff_query->error) diff_query->error = "complexity_limit";
        diff_free_file(&A); diff_free_file(&B);
        if (json) { jash_writef(jash_stderr_writer, 0, "diff: --json: input too complex for in-process diff\n"); return 2; }
        return diff_fallback(s, w, ctx, argc, argv);
    }

    int nh = 0;
    diff_hunk_t* hunks = build_hunks(ops.items, ops.count, &nh);
    free(ops.items);
    if (!hunks || nh < 0) {
        diff_free_file(&A); diff_free_file(&B);
        if (diff_query) diff_query->error = "allocation_failed";
        return 2;
    }

    if (nh == 0 && A.count > 0 && B.count > 0 && A.has_final_nl != B.has_final_nl) {
        diff_hunk_t* grown = realloc(hunks, sizeof(diff_hunk_t));
        if (grown) hunks = grown;
        diff_hunk_t only = { A.count-1, 1, B.count-1, 1 };
        hunks[0] = only;
        nh = 1;
    }

    int rc = nh > 0 ? 1 : 0;
    if (json)          rc = emit_json(w, ctx, hunks, nh, &A, &B);
    else if (unified)  emit_unified(w, ctx, hunks, nh, &A, &B, context, pathA, pathB);
    else               emit_normal(w, ctx, hunks, nh, &A, &B);

    free(hunks);
    diff_free_file(&A);
    diff_free_file(&B);
    return rc;
}

int jash_b_diff(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int mode = 0, explicit_json = 0;
    for (int i = 0; i < argc && strcmp(argv[i], "--"); i++) {
        if (!strcmp(argv[i], "--json")) { explicit_json = 1; if (!mode) mode = 1; }
        if (!strcmp(argv[i], "--data") || !strcmp(argv[i], "--data-v2")) mode = 2;
        if (!strcmp(argv[i], "-U") || !strcmp(argv[i], "--offset") || !strcmp(argv[i], "--limit")) i++;
    }
    if (!mode) return diff_run(state, writer, context, argc, argv);
    diff_query_t query = {.state = state, .limit = UINT64_MAX, .native = mode == 2};
    diff_query = &query;
    char** args = argc > 0 && argc <= 64 ? calloc((size_t)argc + 1u, sizeof(*args)) : NULL;
    if (!args) query.error = argc > 64 ? "argument_limit" : "allocation_failed";
    if (query.native && explicit_json) query.error = "conflicting_output_modes";
    int count = 0, end_options = 0;
    for (int i = 0; i < argc && !query.error; i++) {
        char* argument = argv[i];
        if (!end_options && !strcmp(argument, "--")) end_options = 1;
        if (!end_options && (!strcmp(argument, "--data") || !strcmp(argument, "--data-v2"))) {
            args[count++] = "--json"; continue;
        }
        if (!end_options && (!strcmp(argument, "--offset") || !strcmp(argument, "--limit"))) {
            int limit = !strcmp(argument, "--limit");
            uint64_t value;
            if (!query.native || i + 1 >= argc || diff_query_number(argv[++i], &value) < 0 || (limit && !value)) {
                query.error = "invalid_page"; break;
            }
            if (limit) query.limit = value; else query.offset = value;
            continue;
        }
        if (!end_options && !strncmp(argument, "-U", 2)) {
            uint64_t value;
            const char* text = argument[2] ? argument + 2 : i + 1 < argc ? argv[i + 1] : NULL;
            if (diff_query_number(text, &value) < 0 || value > DIFF_MAX_LINES) { query.error = "invalid_context"; break; }
            args[count++] = argument;
            if (!argument[2]) args[count++] = argv[++i];
            continue;
        }
        args[count++] = argument;
    }
    int result = query.error ? 2 : diff_run(state, writer, context, count, args);
    if (!query.emitted) result = diff_query_error(writer, context, 2);
    free(args); diff_query = NULL;
    return result;
}

int jash_diff_query_snapshot(jash_state_t* state, jash_writer_t writer, void* context,
    const char* before, const char* after, const char* logical_path, const char* checkpoint,
    int native, uint64_t offset, uint64_t limit) {
    diff_query_t query = {.state = state, .offset = offset, .limit = limit, .native = native,
        .logical_path = logical_path, .checkpoint = checkpoint, .command = "tdiff"};
    char* arguments[] = {"--json", "--", (char*)before, (char*)after};
    diff_query = &query;
    int result = diff_run(state, writer, context, 4, arguments);
    if (!query.emitted) result = diff_query_error(writer, context, 2);
    diff_query = NULL;
    return result;
}

int jash_diff_query_error(jash_state_t* state, jash_writer_t writer, void* context,
    const char* command, const char* code, int native, int status) {
    diff_query_t query = {.state = state, .native = native, .command = command, .error = code};
    diff_query = &query;
    int result = diff_query_error(writer, context, status);
    diff_query = NULL;
    return result;
}
