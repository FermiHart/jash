// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/wc — count lines/words/chars/bytes, in-process (ADR-0044).
//
// Why a builtin: wc is a workhorse agents call constantly — "how many matches?
// how big is this? how many lines changed?". In-process it skips a fork/exec and,
// at the single-command dispatch (eval.c / main.c), is budget-governed like the
// other builtins. The GOLD angle is `--json`: BSD wc emits fixed-width columns an
// agent has to brittle-parse with awk (and the widths shift with magnitude);
// `wc --json` emits {name,lines,words,bytes,...} objects it can read directly.
//
// The default (no --json) is byte-EXACT BSD wc: fixed column order
// lines / words / (bytes|chars) / max-line-length regardless of flag order, each
// column formatted " %7ju" (min width 7, natural overflow past 7 digits), then
// the filename as " %s". -c and -m are mutually exclusive (last one wins). One
// deliberate improvement over BSD: `-` means stdin (GNU behavior) rather than a
// file literally named "-". argv_fn (quote-aware). Any flag outside wc's
// [-Lclmw] surface falls back to the real wc, so nothing regresses.

#include "builtin.h"
#include <core/structured_output.h>
#include <parser/exec.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>
#include <errno.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <signal.h>
#include <core/json_writer.h>
#include <core/output_budget.h>
#include <parser/builtin_redir.h>
#include "query_data.h"

typedef struct {
    uintmax_t lines, words, bytes, chars, maxline;
} wc_counts_t;

/* Column width, and jash's contract is parity with the wc the HOST ships:
 *
 *   BSD/macOS  — every column is " %7ju", always, even for one operand.
 *   GNU/Linux  — one operand (or stdin): natural width, no padding ("3").
 *                Several operands: every column is right-aligned to the number
 *                of DIGITS IN THE COMBINED BYTE SIZE of the operands — not a
 *                fixed 7. Two tiny files give width 1 ("3 3 6 f"); add a 1.3 MB
 *                file and every column jumps to width 7 (" 200000 big").
 *
 * wc_width() returns 0 for "BSD padding", otherwise the GNU field width. */
#if defined(__APPLE__)
#  define WC_BSD_PAD 1
#else
#  define WC_BSD_PAD 0
#endif

/* Single pass over f. words = maximal runs of non-whitespace; chars = UTF-8 code
 * points (lead byte = (b & 0xC0) != 0x80); maxline = longest line, newline
 * excluded. line_in_chars picks the -L unit: BSD wc -L measures BYTES normally
 * but CHARS when -m is active (so tab = 1 either way; a 2-byte é adds 2 without
 * -m, 1 with it) — replicated exactly here. */
static int wc_count(FILE* f, wc_counts_t* c, int line_in_chars, jash_state_t* state) {
    unsigned char buf[65536];
    size_t n;
    int in_word = 0;
    uintmax_t cur = 0;                 /* current line length, sans '\n' */
    int cancelled = state ? jash_query_cancel_poll(state) : 0;
    if (cancelled) return cancelled;
    while ((n = fread(buf, 1, sizeof buf, f)) > 0) {
        if (state && (cancelled = jash_query_cancel_poll(state))) return cancelled;
        for (size_t i = 0; i < n; i++) {
            unsigned char ch = buf[i];
            int is_lead = (ch & 0xC0) != 0x80;   /* ASCII or UTF-8 lead byte */
            int whitespace = isspace(ch);
            if (c->bytes == UINTMAX_MAX || (is_lead && c->chars == UINTMAX_MAX) ||
                (ch == '\n' && c->lines == UINTMAX_MAX) ||
                (ch != '\n' && (!line_in_chars || is_lead) && cur == UINTMAX_MAX) ||
                (!whitespace && !in_word && c->words == UINTMAX_MAX)) {
                errno = EOVERFLOW;
                return 1;
            }
            c->bytes++;
            if (is_lead) c->chars++;
            if (ch == '\n') {
                c->lines++;
                if (cur > c->maxline) c->maxline = cur;
                cur = 0;
            } else if (!line_in_chars || is_lead) {
                cur++;
            }
            if (whitespace) in_word = 0;
            else if (!in_word) { in_word = 1; c->words++; }
        }
    }
    if (cur > c->maxline) c->maxline = cur;   /* trailing line without a newline */
    return ferror(f) ? 1 : 0;
}

/* Reconstruct single-quoted argv and defer to the real wc (flags we don't own). */
static int wc_fallback(jash_state_t* s, jash_writer_t w, void* ctx,
                       int argc, char** argv) {
    size_t capacity = 1;
    for (int i = 0; i < argc; i++) {
        size_t n = strlen(argv[i]);
        if (capacity > SIZE_MAX - 3 || n > (SIZE_MAX - capacity - 3) / 4) return 1;
        capacity += 4 * n + 3;
    }
    char* args = malloc(capacity);
    if (!args) return 1;
    size_t o = 0;
    for (int i = 0; i < argc; i++) {
        if (i) args[o++] = ' ';
        args[o++] = '\'';
        for (const char* p = argv[i]; *p; p++) {
            if (*p == '\'') { args[o++]='\''; args[o++]='\\'; args[o++]='\''; args[o++]='\''; }
            else args[o++] = *p;
        }
        args[o++] = '\'';
    }
    args[o] = '\0';
    int rc = jash_exec_external(s, w, ctx, "wc", args);
    free(args); return rc;
}

/* Column width differs between the two wc lineages, and jash's contract is
 * parity with the wc the HOST ships:
 *
 *   BSD/macOS  — every column is " %7ju", always, even for a single operand.
 *   GNU/Linux  — a single operand (or stdin) prints the natural width with no
 *                left padding ("3", not "      3"); only a multi-operand run
 *                right-aligns to 7 so the columns and the "total" row line up.
 *
 * `pad` carries that decision from the call site, which is the only place that
 * knows how many operands there are. */
/* GNU wc's column width, reverse-engineered against coreutils 9.4 and pinned
 * by the table below (all verified on this host):
 *
 *   cols  operands            size known   width
 *   ----  ------------------  -----------  -----
 *    1    0 (pipe or <file)   either        1
 *    1    1                   either        1
 *    1    2 x 4 B                8 B        1     digits(8)
 *    1    2, one 1.3 MB       1.3 MB        7     digits(1355496)
 *    2    0, stdin is a file     6 B        1     digits(6)
 *    3    0, stdin is a pipe  unknown       7     fallback
 *    3    1 x 6 B                6 B        1     digits(6)
 *
 * So: one column and at most one operand always prints natural width; anything
 * wider takes the digit count of the operands' COMBINED BYTE SIZE, falling back
 * to 7 when any of them cannot be stat'd (a pipe has no size to read ahead). */
static int wc_gnu_width(char** argv, int from, int argc, int ncols, int nfiles) {
    if (ncols <= 1 && nfiles <= 1) return 1;

    uintmax_t tot = 0;
    struct stat st;
    if (nfiles == 0) {
        /* stdin: only a regular file has a size we can read ahead of time. */
        if (fstat(0, &st) != 0 || !S_ISREG(st.st_mode)) return 7;
        tot = (uintmax_t)st.st_size;
    } else {
        for (int k = from; k < argc; k++) {
            if (strcmp(argv[k], "-") == 0) {
                if (fstat(0, &st) != 0 || !S_ISREG(st.st_mode)) return 7;
            } else if (stat(argv[k], &st) != 0 || !S_ISREG(st.st_mode)) {
                return 7;
            }
            tot += (uintmax_t)st.st_size;
        }
    }
    int digits = 1;
    while (tot >= 10) { tot /= 10; digits++; }
    return digits;
}

/* width == 0 selects BSD's unconditional " %7ju". */
static void wc_emit_num(jash_writer_t w, void* ctx, uintmax_t v, int width,
                        int first) {
    char b[48];
    int k;
    if (width <= 0)   k = snprintf(b, sizeof b, " %7ju", v);
    else if (first)   k = snprintf(b, sizeof b, "%*ju", width, v);
    else              k = snprintf(b, sizeof b, " %*ju", width, v);
    w(ctx, b, (size_t)k);
}

static void wc_text_line(jash_writer_t w, void* ctx, const wc_counts_t* c,
                         int want_l, int want_w, int cm, int want_L,
                         const char* name, int width) {
    int first = 1;
#define WC_EMIT(V) do { wc_emit_num(w, ctx, (V), width, first); first = 0; } while (0)
    if (want_l)       WC_EMIT(c->lines);
    if (want_w)       WC_EMIT(c->words);
    if (cm == 'c')      WC_EMIT(c->bytes);
    else if (cm == 'm') WC_EMIT(c->chars);
    if (want_L)         WC_EMIT(c->maxline);
#undef WC_EMIT
    if (name) { w(ctx, " ", 1); w(ctx, name, strlen(name)); }
    w(ctx, "\n", 1);
}

/* Shared checked codec: filenames may be non-UTF-8 and counts exceed 2^53. */
static void wc_json_obj(jash_json_writer_t* out, const wc_counts_t* c,
                         int want_l, int want_w, int cm, int want_L,
                         const char* name, const char* error, int report) {
    jash_jsonw_raw(out, "{\"name\":"); jash_jsonw_string_or_bytes(out, name);
    if (want_l) { jash_jsonw_raw(out, ",\"lines\":"); jash_jsonw_safe_u64(out, c->lines); }
    if (want_w) { jash_jsonw_raw(out, ",\"words\":"); jash_jsonw_safe_u64(out, c->words); }
    if (cm == 'c') { jash_jsonw_raw(out, ",\"bytes\":"); jash_jsonw_safe_u64(out, c->bytes); }
    else if (cm == 'm') { jash_jsonw_raw(out, ",\"chars\":"); jash_jsonw_safe_u64(out, c->chars); }
    if (want_L) { jash_jsonw_raw(out, ",\"max_line_length\":"); jash_jsonw_safe_u64(out, c->maxline); }
    if (report) {
        jash_jsonw_raw(out, ",\"complete\":"); jash_jsonw_raw(out, error ? "false" : "true");
        jash_jsonw_raw(out, ",\"error\":"); if (error) jash_jsonw_string(out, error); else jash_jsonw_raw(out, "null");
    }
    jash_jsonw_raw(out, "}");
}

int jash_wc_report_mode(int argc, char* const* argv) {
    int mode = 0;
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        if (!strcmp(a, "--") || a[0] != '-' || !a[1]) break;
        if (!strcmp(a, "--data") || !strcmp(a, "--data-v2")) mode = 2;
        if (!strcmp(a, "--json-report") && !mode) mode = 1;
        if (!strcmp(a, "--json") && !mode) mode = 3;
        if (!strcmp(a, "--files0-from") || !strcmp(a, "--offset") || !strcmp(a, "--limit")) i++;
    }
    return mode;
}

static int wc_page_number(const char* text, uint64_t* out) {
    if (!text || !*text) return -1;
    uint64_t value = 0;
    for (; *text; text++) {
        if (*text < '0' || *text > '9' || value > (UINT64_MAX - (unsigned)(*text - '0')) / 10) return -1;
        value = value * 10 + (unsigned)(*text - '0');
    }
    *out = value; return 0;
}

typedef struct {
    wc_counts_t counts;
    const char* error;
    int error_number;
    int opened;
    int cancelled;
} wc_observation_t;

static void wc_observe(const char* name, int line_in_chars, wc_observation_t* observed, jash_state_t* state) {
    memset(observed, 0, sizeof(*observed));
    FILE* input = !strcmp(name, "-") ? stdin : fopen(name, "rb");
    observed->opened = input != NULL;
    if (!input) { observed->error = "open_failed"; observed->error_number = errno ? errno : EIO; }
    int status = input ? wc_count(input, &observed->counts, line_in_chars, state) : 0;
    if (status) {
        observed->cancelled = status >= 128 ? status : 0;
        observed->error = observed->cancelled ? "request_cancelled" : "read_failed";
        observed->error_number = observed->cancelled ? 0 : errno ? errno : EIO;
    }
    if (input && input != stdin && fclose(input)) {
        observed->error = "close_failed";
        observed->error_number = errno ? errno : EIO;
    }
}

static int wc_json_run(jash_writer_t w, void* ctx, int count, char** files, int report,
                        int lines, int words, int cm, int maxline, uint64_t offset, uint64_t limit) {
    struct sigaction old, ignore = {0}; ignore.sa_handler = SIG_IGN; sigemptyset(&ignore.sa_mask);
    if (sigaction(SIGPIPE, &ignore, &old)) return 125;
    jash_budget_unwrap(&w, &ctx); jash_redir_stream_writer(w, ctx);
    jash_json_writer_t out; jash_jsonw_init(&out, w, ctx);
    jash_jsonw_raw(&out, report ? "{\"schema\":\"jash.query.wc.v1\",\"consistency\":\"live\",\"word_policy\":\"byte-isspace\",\"total_scope\":\"observed-page\",\"items\":[" : "[");
    uint64_t emitted = 0, failed = 0;
    wc_counts_t total = {0}; int rc = 0;
    if (jash_jsonw_finish(&out)) { rc = 125; goto finish; }
    for (int i = 0; i < (count ? count : 1) && emitted < limit && !out.error; i++) {
        if ((uint64_t)i < offset) continue;
        const char* name = count ? files[i] : "-";
        wc_observation_t observed;
        wc_observe(name, cm == 'm', &observed, NULL);
        wc_counts_t current = observed.counts;
        const char* error = observed.error;
        if (error) { failed++; rc = 1; }
        if (!observed.opened && !report) { jash_writef(jash_stderr_writer, NULL, "wc: cannot open '%s'\n", name); continue; }
        if (emitted++) jash_jsonw_raw(&out, ",");
        wc_json_obj(&out, &current, lines, words, cm, maxline, name, error, report);
        if (UINTMAX_MAX - total.lines < current.lines || UINTMAX_MAX - total.words < current.words ||
            UINTMAX_MAX - total.bytes < current.bytes || UINTMAX_MAX - total.chars < current.chars) { rc = 1; failed++; break; }
        total.lines += current.lines; total.words += current.words; total.bytes += current.bytes; total.chars += current.chars;
        if (current.maxline > total.maxline) total.maxline = current.maxline;
        jash_jsonw_finish(&out);
    }
    if (!report && count > 1) {
        if (emitted) jash_jsonw_raw(&out, ",");
        wc_json_obj(&out, &total, lines, words, cm, maxline, "total", NULL, 0);
    }
    jash_jsonw_raw(&out, "]");
    if (report) {
        jash_jsonw_raw(&out, ",\"total\":"); wc_json_obj(&out, &total, lines, words, cm, maxline, "total", failed ? "partial" : NULL, 1);
        jash_jsonw_raw(&out, ",\"count\":"); jash_jsonw_safe_u64(&out, emitted);
        jash_jsonw_raw(&out, ",\"failed\":"); jash_jsonw_safe_u64(&out, failed);
        jash_jsonw_raw(&out, ",\"next_offset\":");
        uint64_t all = count ? (uint64_t)count : 1;
        if (offset < all && emitted < all - offset) jash_jsonw_safe_u64(&out, offset + emitted); else jash_jsonw_raw(&out, "null");
        jash_jsonw_raw(&out, ",\"exit_code\":"); jash_jsonw_i64(&out, rc); jash_jsonw_raw(&out, "}");
    }
    jash_jsonw_raw(&out, "\n");
    if (jash_jsonw_finish(&out)) rc = 125;
finish:
    if (sigaction(SIGPIPE, &old, NULL)) rc = 125;
    return rc;
}

QUERY_DATA_SCALAR(WC_BOOL, JDV_BOOL, 0);
QUERY_DATA_SCALAR(WC_SYMBOL, JDV_SYMBOL, 0);
QUERY_DATA_SCALAR(WC_NULL_SYMBOL, JDV_SYMBOL, 1);
QUERY_DATA_SCALAR(WC_NULL_PATH, JDV_PATH, 1);
QUERY_DATA_SCALAR(WC_NULL_U64, JDV_U64, 1);
QUERY_DATA_SCALAR(WC_NULL_SIZE, JDV_FILESIZE, 1);

enum {
    WC_BYTES, WC_CHAR_POLICY, WC_CHARS, WC_COMPLETE, WC_ERRNO, WC_ERROR,
    WC_LINES, WC_MAX_LINE, WC_MAX_UNIT, WC_NAME, WC_INDEX, WC_SOURCE,
    WC_TOTAL_SCOPE, WC_WORD_POLICY, WC_WORDS, WC_FIELDS
};
static const jdv_schema_field_t WC_DATA_FIELDS[] = {
    QUERY_DATA_FIELD("bytes", &WC_NULL_SIZE),
    QUERY_DATA_FIELD("char_policy", &WC_SYMBOL),
    QUERY_DATA_FIELD("chars", &WC_NULL_U64),
    QUERY_DATA_FIELD("complete", &WC_BOOL),
    QUERY_DATA_FIELD("errno", &WC_NULL_U64),
    QUERY_DATA_FIELD("error", &WC_NULL_SYMBOL),
    QUERY_DATA_FIELD("lines", &WC_NULL_U64),
    QUERY_DATA_FIELD("max_line_length", &WC_NULL_U64),
    QUERY_DATA_FIELD("max_line_unit", &WC_SYMBOL),
    QUERY_DATA_FIELD("name", &WC_NULL_PATH),
    QUERY_DATA_FIELD("operand_index", &WC_NULL_U64),
    QUERY_DATA_FIELD("source", &WC_SYMBOL),
    QUERY_DATA_FIELD("total_scope", &WC_SYMBOL),
    QUERY_DATA_FIELD("word_policy", &WC_SYMBOL),
    QUERY_DATA_FIELD("words", &WC_NULL_U64)
};
_Static_assert(sizeof(WC_DATA_FIELDS) / sizeof(WC_DATA_FIELDS[0]) == WC_FIELDS,
               "wc native field mapping");
static const jdv_schema_t WC_DATA_SCHEMA = {
    .kind = JDV_SCHEMA_RECORD, .as.record = {WC_DATA_FIELDS, WC_FIELDS, 0}
};
typedef struct { jdv_record_field_t fields[WC_FIELDS]; } wc_data_row_t;

static jdv_value_t wc_data_item(wc_data_row_t* row, const wc_observation_t* observed,
                                 const char* name, uint64_t index, int total,
                                 int lines, int words, int cm, int maxline) {
    jdv_value_t item = query_data_record(row->fields, &WC_DATA_SCHEMA);
    jdv_record_field_t* f = row->fields;
    if (cm == 'c') f[WC_BYTES].value = jdv_value_filesize(observed->counts.bytes);
    f[WC_CHAR_POLICY].value = query_data_text(JDV_SYMBOL, "utf8-leading-bytes");
    if (cm == 'm') f[WC_CHARS].value = jdv_value_u64(observed->counts.chars);
    f[WC_COMPLETE].value = jdv_value_bool(!observed->error);
    if (observed->error) {
        f[WC_ERROR].value = query_data_text(JDV_SYMBOL, observed->error);
        if (observed->error_number) f[WC_ERRNO].value = jdv_value_u64((uint64_t)observed->error_number);
    }
    if (lines) f[WC_LINES].value = jdv_value_u64(observed->counts.lines);
    if (maxline) f[WC_MAX_LINE].value = jdv_value_u64(observed->counts.maxline);
    f[WC_MAX_UNIT].value = query_data_text(JDV_SYMBOL, cm == 'm' ? "utf8-leading-bytes" : "bytes");
    if (!total && strcmp(name, "-")) f[WC_NAME].value = query_data_text(JDV_PATH, name);
    if (!total) f[WC_INDEX].value = jdv_value_u64(index);
    f[WC_SOURCE].value = query_data_text(JDV_SYMBOL, total ? "total" : !strcmp(name, "-") ? "stdin" : "file");
    f[WC_TOTAL_SCOPE].value = query_data_text(JDV_SYMBOL, "observed-page");
    f[WC_WORD_POLICY].value = query_data_text(JDV_SYMBOL, "byte-isspace");
    if (words) f[WC_WORDS].value = jdv_value_u64(observed->counts.words);
    return item;
}

static int wc_data_run(jash_state_t* state, jash_writer_t writer, void* context, int argc, char** argv) {
    int from = 0, lines = 0, words = 0, cm = 0, maxline = 0;
    uint64_t offset = 0, limit = UINT64_MAX;
    for (; from < argc; from++) {
        const char* arg = argv[from];
        if (!strcmp(arg, "--")) { from++; break; }
        if (arg[0] != '-' || !arg[1]) break;
        if (!strcmp(arg, "--data") || !strcmp(arg, "--data-v2")) continue;
        if (!strcmp(arg, "--offset") || !strcmp(arg, "--limit")) {
            int is_limit = !strcmp(arg, "--limit");
            if (++from >= argc || wc_page_number(argv[from], is_limit ? &limit : &offset) ||
                (is_limit && !limit))
                return query_data_error(writer, context, &WC_DATA_SCHEMA, "invalid_page", 2);
            continue;
        }
        for (size_t j = 1; arg[j]; j++) {
            switch (arg[j]) {
                case 'l': lines = 1; break;
                case 'w': words = 1; break;
                case 'c': cm = 'c'; break;
                case 'm': cm = 'm'; break;
                case 'L': maxline = 1; break;
                default: return query_data_error(writer, context, &WC_DATA_SCHEMA, "unsupported_option", 2);
            }
        }
    }
    if (!lines && !words && !cm && !maxline) { lines = words = 1; cm = 'c'; }
    int operands = argc - from;
    uint64_t available = operands ? (uint64_t)operands : 1;
    available = offset < available ? available - offset : 0;
    uint64_t selected = available < limit ? available : limit;
    size_t count = selected > QUERY_DATA_MAX_ITEMS ? QUERY_DATA_MAX_ITEMS : (size_t)selected;
    size_t capacity = count + (operands > 1);
    wc_data_row_t* rows = capacity ? calloc(capacity, sizeof(*rows)) : NULL;
    jdv_value_t* items = capacity ? calloc(capacity, sizeof(*items)) : NULL;
    if (capacity && (!rows || !items)) {
        free(rows); free(items);
        return query_data_error(writer, context, &WC_DATA_SCHEMA, "allocation_failed", 1);
    }
    wc_observation_t total = {0};
    uint64_t errors = 0;
    int cancelled = 0;
    size_t observed_count = 0;
    for (size_t i = 0; i < count; i++) {
        if ((cancelled = jash_query_cancel_poll(state))) break;
        const char* name = operands ? argv[from + (int)offset + (int)i] : "-";
        wc_observation_t observed;
        wc_observe(name, cm == 'm', &observed, state);
        cancelled = observed.cancelled;
        if (observed.error && !cancelled) errors++;
        items[i] = wc_data_item(&rows[i], &observed, name, offset + i, 0,
                                 lines, words, cm, maxline);
        observed_count++;
        if (UINTMAX_MAX - total.counts.lines < observed.counts.lines ||
            UINTMAX_MAX - total.counts.words < observed.counts.words ||
            UINTMAX_MAX - total.counts.bytes < observed.counts.bytes ||
            UINTMAX_MAX - total.counts.chars < observed.counts.chars) {
            total.error = "total_overflow";
            total.error_number = EOVERFLOW;
            errors++;
        } else {
            total.counts.lines += observed.counts.lines;
            total.counts.words += observed.counts.words;
            total.counts.bytes += observed.counts.bytes;
            total.counts.chars += observed.counts.chars;
        }
        if (observed.counts.maxline > total.counts.maxline) total.counts.maxline = observed.counts.maxline;
        if (cancelled) break;
    }
    if (errors && !total.error) total.error = "partial";
    if (cancelled) total.error = "request_cancelled";
    size_t emitted = observed_count;
    if (operands > 1)
        items[emitted++] = wc_data_item(&rows[count], &total, "total", 0, 1,
                                          lines, words, cm, maxline);
    int capped = selected > count;
    int incomplete = available > count;
    int result = query_data_emit(writer, context, &WC_DATA_SCHEMA, items, emitted,
        errors, cancelled ? cancelled : capped ? 75 : errors ? 1 : 0,
        cancelled ? JDV_STREAM_CANCELLED : incomplete ? JDV_STREAM_INCOMPLETE : JDV_STREAM_COMPLETE,
        cancelled ? "request_cancelled" : capped ? "query_item_limit" : incomplete ? "page_limit" : NULL);
    free(rows); free(items);
    return result;
}

int jash_b_wc(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int structured_mode = jash_wc_report_mode(argc, argv);
    if (structured_mode == 2) return wc_data_run(s, w, ctx, argc, argv);
    if (structured_mode == 1) {
        int start = 0; uint64_t offset = 0, limit = UINT64_MAX;
        for (; start < argc; start++) {
            const char* a = argv[start];
            if (!strcmp(a, "--")) { start++; break; }
            if (!strcmp(a, "--json-report")) continue;
            if (!strcmp(a, "--offset") || !strcmp(a, "--limit")) {
                int is_limit = !strcmp(a, "--limit");
                if (++start >= argc || wc_page_number(argv[start], is_limit ? &limit : &offset) || (is_limit && !limit))
                    return jash_structured_error(w, ctx, "wc", "invalid_page", 2);
            } else if (a[0] == '-' && a[1]) return jash_structured_error(w, ctx, "wc", "unsupported_option", 2);
            else break;
        }
        return wc_json_run(w, ctx, argc - start, argv + start, 1, 1, 1, 'c', 0, offset, limit);
    }
    int want_l = 0, want_w = 0, want_L = 0;
    int cm = 0;               /* 0 none, 'c' bytes, 'm' chars — last of -c/-m wins */
    int json = 0;
    int i = 0, end_opts = 0;

    for (; i < argc; i++) {
        const char* a = argv[i];
        if (end_opts || a[0] != '-' || a[1] == '\0') break;   /* '-' alone = stdin */
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "--json") == 0) { json = 1; continue; }
        if (a[1] == '-') return structured_mode == 3 ?
            jash_structured_error(w, ctx, "wc", "unsupported_option", 2) : wc_fallback(s, w, ctx, argc, argv);
        for (int j = 1; a[j]; j++) {
            switch (a[j]) {
                case 'l': want_l = 1; break;
                case 'w': want_w = 1; break;
                case 'c': cm = 'c'; break;
                case 'm': cm = 'm'; break;
                case 'L': want_L = 1; break;
                default:  return structured_mode == 3 ?
                    jash_structured_error(w, ctx, "wc", "unsupported_option", 2) : wc_fallback(s, w, ctx, argc, argv);
            }
        }
    }

    if (!want_l && !want_w && !cm && !want_L) { want_l = want_w = 1; cm = 'c'; }

    int noperands = argc - i;
    if (json) return wc_json_run(w, ctx, noperands, argv + i, 0, want_l, want_w, cm, want_L, 0, UINT64_MAX);
    int rc = 0;
    /* How many count columns this invocation prints — an input to GNU's width. */
    int ncols = (want_l ? 1 : 0) + (want_w ? 1 : 0) + (cm ? 1 : 0) + (want_L ? 1 : 0);

    if (noperands <= 0) {
        /* No file operands → stdin (no name in text mode; "-" in json). */
        wc_counts_t c; memset(&c, 0, sizeof c);
        rc |= wc_count(stdin, &c, cm == 'm', NULL);
        wc_text_line(w, ctx, &c, want_l, want_w, cm, want_L, NULL,
                                WC_BSD_PAD ? 0
                                : wc_gnu_width(argv, i, argc, ncols, 0));
    } else {
        /* 0 selects BSD's unconditional " %7ju". */
        int gnu_width = WC_BSD_PAD ? 0
                      : wc_gnu_width(argv, i, argc, ncols, noperands);
        wc_counts_t total; memset(&total, 0, sizeof total);
        for (int k = i; k < argc; k++) {
            wc_counts_t c; memset(&c, 0, sizeof c);
            int is_stdin = (strcmp(argv[k], "-") == 0);
            FILE* f = stdin;
            if (!is_stdin) {
                f = fopen(argv[k], "rb");
                if (!f) {
                    jash_writef(jash_stderr_writer, 0, "wc: %s: open: %s\n", argv[k], strerror(errno));
                    rc = 1; continue;
                }
            }
            if (wc_count(f, &c, cm == 'm', NULL)) rc = 1;
            if (!is_stdin) fclose(f);

            total.lines += c.lines; total.words += c.words;
            total.bytes += c.bytes; total.chars += c.chars;
            if (c.maxline > total.maxline) total.maxline = c.maxline;

            const char* nm = argv[k];   /* "-" prints as "-" */
            wc_text_line(w, ctx, &c, want_l, want_w, cm, want_L, nm,
                                    gnu_width);
        }
        if (noperands > 1) {
            wc_text_line(w, ctx, &total, want_l, want_w, cm, want_L,
                                    "total", gnu_width);
        }
    }

    return rc;
}
