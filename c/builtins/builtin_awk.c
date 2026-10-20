// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: builtins/awk — narrow in-process awk slice, structured-JSON gold
// (ADR-0044). Design note: Docs/ADR-0044-awk-engine-design.md.
//
// Why a builtin: awk is a full programming language — jash does NOT try to
// own all of it (that would mean shipping a second interpreter). Instead this
// owns the common one-liner shape an agent actually reaches for: a single
// BEGIN/pattern-action/END program over `$0..$N`/NF/NR with print/printf and
// simple +=/-=/++/-- accumulation. Anything outside that narrow grammar falls
// back to the real awk (single-quoted argv reconstruction), same discipline
// as builtin_sed.c/builtin_diff.c/builtin_xargs.c.
//
// THE SAFETY PROPERTY: the parser is an allowlist, not a best-effort parser.
// Any token or construct outside the owned grammar makes parsing fail, which
// always falls back to the real awk — it NEVER attempts a partial/best-guess
// execution. A false fallback costs one fork; a false "understood" would
// silently produce wrong output, which is strictly worse.
//
// GOLD: `--json` emits one `{"fields":[...]}` object per executed print, or
// `{"text":"..."}` per printf, streamed as a JSON array. The high-value case
// is the END-only aggregation idiom (`{s+=$1} END{print s}`), where `--json`
// skips the array wrapper entirely and emits the single aggregate object
// directly: `{"result": 4821}`.
//
// Scope: see Docs/ADR-0044-awk-engine-design.md §2 for the owned grammar and
// §3 for the (extensive) fallback list. argv_fn (quote-aware) — never the
// legacy string-args path (BUG-165).

#include "builtin.h"
#include <parser/exec.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <regex.h>
#include <math.h>
#include <limits.h>

#define AWK_MAX_LINE   65536
#define AWK_MAX_FIELDS 1024
#define AWK_MAX_VARS   32
#define AWK_MAX_STMTS  8
#define AWK_MAX_EXPRS  8
#define AWK_STRBUF     256
#define AWK_MAX_TOK    256

/* ── Tokenizer ────────────────────────────────────────────────────────────
 * Only tokens the owned grammar actually uses get real meaning; everything
 * else becomes TK_UNKNOWN. Since no parse rule ever accepts TK_UNKNOWN, any
 * unrecognized character (parens, brackets, &ing/or, bare '=', '#', etc.)
 * guarantees a parse failure downstream — the conservative-to-fallback
 * property falls out of the grammar shape, not from special-casing. */
typedef enum {
    TK_EOF = 0, TK_LBRACE, TK_RBRACE, TK_DOLLAR, TK_COMMA, TK_SEMI,
    TK_EQEQ, TK_NE, TK_LT, TK_LE, TK_GT, TK_GE, TK_MATCH, TK_NOMATCH,
    TK_PLUSEQ, TK_MINUSEQ, TK_PLUSPLUS, TK_MINUSMINUS,
    TK_IDENT, TK_NUMBER, TK_STRING, TK_REGEX, TK_UNKNOWN
} awk_tt_t;

typedef struct {
    awk_tt_t type;
    char text[AWK_STRBUF];
    double num;
} awk_tok_t;

typedef struct {
    awk_tok_t toks[AWK_MAX_TOK];
    int n;
} awk_toklist_t;

static int awk_tokenize(const char* prog, awk_toklist_t* L) {
    L->n = 0;
    const char* p = prog;
    while (*p) {
        if (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') { p++; continue; }
        if (L->n >= AWK_MAX_TOK - 1) return -1;
        awk_tok_t* t = &L->toks[L->n];
        memset(t, 0, sizeof *t);
        char c = *p;
        if (c == '{') { t->type = TK_LBRACE; p++; }
        else if (c == '}') { t->type = TK_RBRACE; p++; }
        else if (c == '$') { t->type = TK_DOLLAR; p++; }
        else if (c == ',') { t->type = TK_COMMA; p++; }
        else if (c == ';') { t->type = TK_SEMI; p++; }
        else if (c == '=' && p[1] == '=') { t->type = TK_EQEQ; p += 2; }
        else if (c == '!' && p[1] == '=') { t->type = TK_NE; p += 2; }
        else if (c == '!' && p[1] == '~') { t->type = TK_NOMATCH; p += 2; }
        else if (c == '~') { t->type = TK_MATCH; p++; }
        else if (c == '<' && p[1] == '=') { t->type = TK_LE; p += 2; }
        else if (c == '<') { t->type = TK_LT; p++; }
        else if (c == '>' && p[1] == '=') { t->type = TK_GE; p += 2; }
        else if (c == '>') { t->type = TK_GT; p++; }
        else if (c == '+' && p[1] == '=') { t->type = TK_PLUSEQ; p += 2; }
        else if (c == '+' && p[1] == '+') { t->type = TK_PLUSPLUS; p += 2; }
        else if (c == '-' && p[1] == '=') { t->type = TK_MINUSEQ; p += 2; }
        else if (c == '-' && p[1] == '-') { t->type = TK_MINUSMINUS; p += 2; }
        else if (c == '/') {
            /* No division in this grammar, so '/' unconditionally starts a
             * regex literal — no lexer context-sensitivity needed. */
            p++;
            size_t o = 0;
            while (*p && *p != '/') {
                if (*p == '\\' && p[1]) {
                    if (o + 2 >= sizeof t->text) return -1;
                    t->text[o++] = '\\'; t->text[o++] = p[1]; p += 2; continue;
                }
                if (o + 1 >= sizeof t->text) return -1;
                t->text[o++] = *p; p++;
            }
            if (*p != '/') return -1;
            p++;
            t->text[o] = '\0';
            t->type = TK_REGEX;
        } else if (c == '"') {
            p++;
            size_t o = 0;
            while (*p && *p != '"') {
                if (*p == '\\' && p[1]) {
                    char e = p[1];
                    char ch = (e == 'n') ? '\n' : (e == 't') ? '\t' : (e == '\\') ? '\\' : (e == '"') ? '"' : e;
                    if (o + 1 >= sizeof t->text) return -1;
                    t->text[o++] = ch; p += 2; continue;
                }
                if (o + 1 >= sizeof t->text) return -1;
                t->text[o++] = *p; p++;
            }
            if (*p != '"') return -1;
            p++;
            t->text[o] = '\0';
            t->type = TK_STRING;
        } else if ((c >= '0' && c <= '9') || (c == '.' && p[1] >= '0' && p[1] <= '9')) {
            const char* start = p;
            while (*p && ((*p >= '0' && *p <= '9') || *p == '.')) p++;
            size_t len = (size_t)(p - start);
            if (len >= sizeof t->text) return -1;
            memcpy(t->text, start, len); t->text[len] = '\0';
            char* number_end;
            t->num = strtod(t->text, &number_end);
            if (*number_end || !isfinite(t->num)) return -1;
            t->type = TK_NUMBER;
        } else if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '_') {
            const char* start = p;
            while (*p && ((*p >= 'A' && *p <= 'Z') || (*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) p++;
            size_t len = (size_t)(p - start);
            if (len >= sizeof t->text) return -1;
            memcpy(t->text, start, len); t->text[len] = '\0';
            t->type = TK_IDENT;
        } else {
            t->type = TK_UNKNOWN; t->text[0] = c; t->text[1] = '\0'; p++;
        }
        L->n++;
    }
    L->toks[L->n].type = TK_EOF;
    L->n++;
    return 0;
}

/* ── AST ──────────────────────────────────────────────────────────────── */

typedef enum { EXPR_FIELD, EXPR_NR, EXPR_NF, EXPR_NUM, EXPR_STR, EXPR_VAR } awk_expr_kind_t;

typedef struct {
    awk_expr_kind_t kind;
    int field_idx;      /* EXPR_FIELD, when !field_is_nf */
    int field_is_nf;    /* EXPR_FIELD: refers to $NF (dynamic last field) */
    double num;         /* EXPR_NUM */
    char str[AWK_STRBUF]; /* EXPR_STR literal / EXPR_VAR name */
} awk_expr_t;

typedef enum { STMT_PRINT, STMT_PRINTF, STMT_ADD, STMT_SUB } awk_stmt_kind_t;

typedef struct {
    awk_stmt_kind_t kind;
    awk_expr_t args[AWK_MAX_EXPRS];
    int nargs;
    char var[AWK_STRBUF];  /* STMT_ADD/STMT_SUB target */
    awk_expr_t rhs;        /* STMT_ADD/STMT_SUB */
} awk_stmt_t;

typedef struct {
    awk_stmt_t stmts[AWK_MAX_STMTS];
    int n;
} awk_block_t;

typedef enum { PAT_ALWAYS, PAT_REGEX, PAT_RELCMP } awk_pat_kind_t;
typedef enum { REL_EQ, REL_NE, REL_LT, REL_LE, REL_GT, REL_GE, REL_MATCH, REL_NOMATCH } awk_relop_t;
typedef enum { OPND_FIELD, OPND_NR, OPND_NF } awk_operand_kind_t;

typedef struct {
    awk_pat_kind_t kind;
    char regex_src[AWK_STRBUF];        /* PAT_REGEX */
    awk_operand_kind_t loperand_kind;  /* PAT_RELCMP left operand */
    int lfield_idx;
    int lfield_is_nf;
    awk_relop_t relop;
    int rhs_is_num;
    double rhs_num;
    char rhs_str[AWK_STRBUF];  /* string literal, or regex source for ~/!~ */
    regex_t re;
    int re_ok;
} awk_pattern_t;

typedef struct {
    int has_begin; awk_block_t begin;
    int has_main;  awk_pattern_t pattern; awk_block_t main_action; int main_implicit_print;
    int has_end;   awk_block_t end;
} awk_program_t;

/* ── Reserved names — rejected wherever a plain identifier is expected, so
 * the awk keywords/builtins this slice does NOT implement (getline, arrays,
 * string functions, control flow, ...) always fail to parse rather than get
 * silently treated as ordinary variables. ── */
static int awk_reserved_assign_target(const char* n) {
    return strcmp(n, "NR") == 0 || strcmp(n, "NF") == 0 ||
           strcmp(n, "FS") == 0 || strcmp(n, "OFS") == 0;
}

static int awk_is_reserved_kw(const char* n) {
    static const char* kws[] = {
        "if", "for", "while", "do", "function", "func", "getline",
        "split", "gsub", "sub", "substr", "length", "sprintf", "delete", "in",
        "return", "next", "nextfile", "exit", "close", "system",
        "BEGIN", "END", "print", "printf", NULL
    };
    for (int i = 0; kws[i]; i++) if (strcmp(n, kws[i]) == 0) return 1;
    return 0;
}

static int awk_printf_fmt_ok(const char* fmt) {
    for (const char* p = fmt; *p; p++) {
        if (*p != '%') continue;
        p++;
        if (*p == '%' || *p == 's' || *p == 'd' || *p == 'f') continue;
        return 0;
    }
    return 1;
}

/* ── Parser ───────────────────────────────────────────────────────────── */

typedef struct { awk_toklist_t* L; int pos; } awk_parser_t;

static awk_tok_t* pk(awk_parser_t* P) { return &P->L->toks[P->pos]; }
static awk_tok_t* pk2(awk_parser_t* P) { return P->pos + 1 < P->L->n ? &P->L->toks[P->pos + 1] : &P->L->toks[P->L->n - 1]; }
static void adv(awk_parser_t* P) { if (P->pos < P->L->n - 1) P->pos++; }

static int parse_expr(awk_parser_t* P, awk_expr_t* e) {
    memset(e, 0, sizeof *e);
    awk_tok_t* t = pk(P);
    if (t->type == TK_DOLLAR) {
        adv(P);
        awk_tok_t* f = pk(P);
        if (f->type == TK_NUMBER) {
            if (f->num < 0 || f->num > INT_MAX || f->num != floor(f->num)) return -1;
            e->kind = EXPR_FIELD; e->field_idx = (int)f->num; adv(P); return 0;
        }
        if (f->type == TK_IDENT && strcmp(f->text, "NF") == 0) { e->kind = EXPR_FIELD; e->field_is_nf = 1; adv(P); return 0; }
        return -1;
    }
    if (t->type == TK_IDENT && strcmp(t->text, "NR") == 0) { e->kind = EXPR_NR; adv(P); return 0; }
    if (t->type == TK_IDENT && strcmp(t->text, "NF") == 0) { e->kind = EXPR_NF; adv(P); return 0; }
    if (t->type == TK_NUMBER) { e->kind = EXPR_NUM; e->num = t->num; adv(P); return 0; }
    if (t->type == TK_STRING) { e->kind = EXPR_STR; strncpy(e->str, t->text, sizeof e->str - 1); adv(P); return 0; }
    if (t->type == TK_IDENT) {
        if (awk_is_reserved_kw(t->text)) return -1;
        awk_tok_t* nx = pk2(P);
        /* var reference must not be a function call or array index */
        if (nx->type == TK_UNKNOWN && (nx->text[0] == '(' || nx->text[0] == '[')) return -1;
        e->kind = EXPR_VAR; strncpy(e->str, t->text, sizeof e->str - 1); adv(P); return 0;
    }
    return -1;
}

static int parse_expr_list(awk_parser_t* P, awk_expr_t* out, int* n, int max) {
    *n = 0;
    for (;;) {
        if (*n >= max) return -1;
        if (parse_expr(P, &out[*n]) != 0) return -1;
        (*n)++;
        if (pk(P)->type == TK_COMMA) { adv(P); continue; }
        break;
    }
    return 0;
}

static int parse_stmt(awk_parser_t* P, awk_stmt_t* st) {
    memset(st, 0, sizeof *st);
    awk_tok_t* t = pk(P);
    if (t->type == TK_IDENT && strcmp(t->text, "print") == 0) {
        adv(P);
        st->kind = STMT_PRINT;
        awk_tok_t* nx = pk(P);
        if (nx->type == TK_RBRACE || nx->type == TK_SEMI || nx->type == TK_EOF) { st->nargs = 0; return 0; }
        return parse_expr_list(P, st->args, &st->nargs, AWK_MAX_EXPRS);
    }
    if (t->type == TK_IDENT && strcmp(t->text, "printf") == 0) {
        adv(P);
        st->kind = STMT_PRINTF;
        if (pk(P)->type != TK_STRING) return -1;
        if (!awk_printf_fmt_ok(pk(P)->text)) return -1;
        if (parse_expr(P, &st->args[0]) != 0) return -1;
        st->nargs = 1;
        while (pk(P)->type == TK_COMMA) {
            adv(P);
            if (st->nargs >= AWK_MAX_EXPRS) return -1;
            if (parse_expr(P, &st->args[st->nargs]) != 0) return -1;
            st->nargs++;
        }
        return 0;
    }
    if (t->type == TK_IDENT) {
        if (awk_is_reserved_kw(t->text) || awk_reserved_assign_target(t->text)) return -1;
        char varname[AWK_STRBUF];
        strncpy(varname, t->text, sizeof varname - 1); varname[sizeof varname - 1] = '\0';
        awk_tok_t* nx = pk2(P);
        if (nx->type == TK_PLUSEQ) {
            adv(P); adv(P);
            st->kind = STMT_ADD; strncpy(st->var, varname, sizeof st->var - 1);
            return parse_expr(P, &st->rhs);
        }
        if (nx->type == TK_MINUSEQ) {
            adv(P); adv(P);
            st->kind = STMT_SUB; strncpy(st->var, varname, sizeof st->var - 1);
            return parse_expr(P, &st->rhs);
        }
        if (nx->type == TK_PLUSPLUS) {
            adv(P); adv(P);
            st->kind = STMT_ADD; strncpy(st->var, varname, sizeof st->var - 1);
            st->rhs.kind = EXPR_NUM; st->rhs.num = 1;
            return 0;
        }
        if (nx->type == TK_MINUSMINUS) {
            adv(P); adv(P);
            st->kind = STMT_SUB; strncpy(st->var, varname, sizeof st->var - 1);
            st->rhs.kind = EXPR_NUM; st->rhs.num = 1;
            return 0;
        }
        return -1;
    }
    return -1;
}

static int parse_block(awk_parser_t* P, awk_block_t* B) {
    memset(B, 0, sizeof *B);
    if (pk(P)->type != TK_LBRACE) return -1;
    adv(P);
    if (pk(P)->type == TK_RBRACE) { adv(P); return 0; }
    for (;;) {
        if (B->n >= AWK_MAX_STMTS) return -1;
        if (parse_stmt(P, &B->stmts[B->n]) != 0) return -1;
        B->n++;
        if (pk(P)->type == TK_SEMI) {
            adv(P);
            if (pk(P)->type == TK_RBRACE) break;
            continue;
        }
        break;
    }
    if (pk(P)->type != TK_RBRACE) return -1;
    adv(P);
    return 0;
}

static int operand_from_tok(awk_parser_t* P, awk_operand_kind_t* k, int* fidx, int* fnf) {
    awk_tok_t* t = pk(P);
    if (t->type == TK_DOLLAR) {
        adv(P);
        awk_tok_t* f = pk(P);
        if (f->type == TK_NUMBER) {
            if (f->num < 0 || f->num > INT_MAX || f->num != floor(f->num)) return -1;
            *k = OPND_FIELD; *fidx = (int)f->num; *fnf = 0; adv(P); return 0;
        }
        if (f->type == TK_IDENT && strcmp(f->text, "NF") == 0) { *k = OPND_FIELD; *fnf = 1; adv(P); return 0; }
        return -1;
    }
    if (t->type == TK_IDENT && strcmp(t->text, "NR") == 0) { *k = OPND_NR; adv(P); return 0; }
    if (t->type == TK_IDENT && strcmp(t->text, "NF") == 0) { *k = OPND_NF; adv(P); return 0; }
    return -1;
}

static int parse_relop(awk_parser_t* P, awk_relop_t* op) {
    switch (pk(P)->type) {
        case TK_EQEQ: *op = REL_EQ; break;
        case TK_NE: *op = REL_NE; break;
        case TK_LT: *op = REL_LT; break;
        case TK_LE: *op = REL_LE; break;
        case TK_GT: *op = REL_GT; break;
        case TK_GE: *op = REL_GE; break;
        case TK_MATCH: *op = REL_MATCH; break;
        case TK_NOMATCH: *op = REL_NOMATCH; break;
        default: return -1;
    }
    adv(P);
    return 0;
}

/* Returns 0 on success (pattern filled in, possibly PAT_ALWAYS meaning "no
 * pattern text was present"), -1 on grammar failure. */
static int parse_pattern(awk_parser_t* P, awk_pattern_t* pat) {
    memset(pat, 0, sizeof *pat);
    awk_tok_t* t = pk(P);
    if (t->type == TK_LBRACE || t->type == TK_EOF ||
        (t->type == TK_IDENT && strcmp(t->text, "END") == 0)) {
        pat->kind = PAT_ALWAYS;
        return 0;
    }
    if (t->type == TK_REGEX) {
        pat->kind = PAT_REGEX;
        strncpy(pat->regex_src, t->text, sizeof pat->regex_src - 1);
        adv(P);
        return 0;
    }
    awk_operand_kind_t lk; int fidx = 0, fnf = 0;
    if (operand_from_tok(P, &lk, &fidx, &fnf) != 0) return -1;
    awk_relop_t op;
    if (parse_relop(P, &op) != 0) return -1;
    pat->kind = PAT_RELCMP;
    pat->loperand_kind = lk; pat->lfield_idx = fidx; pat->lfield_is_nf = fnf;
    pat->relop = op;
    awk_tok_t* r = pk(P);
    if (op == REL_MATCH || op == REL_NOMATCH) {
        if (r->type != TK_REGEX) return -1;
        strncpy(pat->rhs_str, r->text, sizeof pat->rhs_str - 1);
        adv(P);
        return 0;
    }
    if (r->type == TK_NUMBER) { pat->rhs_is_num = 1; pat->rhs_num = r->num; adv(P); return 0; }
    if (r->type == TK_STRING) { strncpy(pat->rhs_str, r->text, sizeof pat->rhs_str - 1); adv(P); return 0; }
    return -1;
}

static int parse_program(awk_toklist_t* L, awk_program_t* prog) {
    memset(prog, 0, sizeof *prog);
    awk_parser_t P = { L, 0 };

    if (pk(&P)->type == TK_IDENT && strcmp(pk(&P)->text, "BEGIN") == 0) {
        adv(&P);
        if (parse_block(&P, &prog->begin) != 0) return -1;
        prog->has_begin = 1;
    }

    if (!(pk(&P)->type == TK_IDENT && strcmp(pk(&P)->text, "END") == 0) && pk(&P)->type != TK_EOF) {
        if (parse_pattern(&P, &prog->pattern) != 0) return -1;
        if (pk(&P)->type == TK_LBRACE) {
            if (parse_block(&P, &prog->main_action) != 0) return -1;
            prog->has_main = 1;
        } else if (prog->pattern.kind != PAT_ALWAYS) {
            prog->has_main = 1;
            prog->main_implicit_print = 1;
        }
    }

    if (pk(&P)->type == TK_IDENT && strcmp(pk(&P)->text, "END") == 0) {
        adv(&P);
        if (parse_block(&P, &prog->end) != 0) return -1;
        prog->has_end = 1;
    }

    if (pk(&P)->type != TK_EOF) return -1; /* leftover tokens: e.g. a 2nd pattern-action pair */
    if (!prog->has_begin && !prog->has_main && !prog->has_end) return -1;
    return 0;
}

static int awk_pattern_prepare(awk_pattern_t* pat) {
    if (pat->kind == PAT_REGEX) {
        if (regcomp(&pat->re, pat->regex_src, REG_EXTENDED) != 0) return -1;
        pat->re_ok = 1;
    } else if (pat->kind == PAT_RELCMP && (pat->relop == REL_MATCH || pat->relop == REL_NOMATCH)) {
        if (regcomp(&pat->re, pat->rhs_str, REG_EXTENDED) != 0) return -1;
        pat->re_ok = 1;
    }
    return 0;
}

static void awk_pattern_cleanup(awk_pattern_t* pat) { if (pat->re_ok) regfree(&pat->re); }

/* ── Runtime ──────────────────────────────────────────────────────────── */

typedef struct {
    char name[AWK_STRBUF];
    char str[512];
    const char* external;
    int is_num;
    double number;
} awk_var_t;

typedef struct {
    awk_var_t vars[AWK_MAX_VARS];
    int nvars;
    double nr;
    char line_buf[AWK_MAX_LINE];
    char field0[AWK_MAX_LINE];
    char* fields[AWK_MAX_FIELDS];
    int nf;
} awk_runtime_t;

static awk_var_t* awk_var_lookup(awk_runtime_t* R, const char* name) {
    for (int i = 0; i < R->nvars; i++) if (strcmp(R->vars[i].name, name) == 0) return &R->vars[i];
    if (R->nvars >= AWK_MAX_VARS) return NULL;
    awk_var_t* v = &R->vars[R->nvars++];
    strncpy(v->name, name, sizeof v->name - 1);
    v->str[0] = '\0';
    return v;
}

static double awk_var_get_num(awk_var_t* v) {
    return !v ? 0.0 : v->is_num ? v->number : strtod(v->external ? v->external : v->str, NULL);
}

static void awk_fmt_num(double v, char* buf, size_t cap) {
    if (v > -1e15 && v < 1e15 && v == (double)(long long)v) snprintf(buf, cap, "%lld", (long long)v);
    else snprintf(buf, cap, "%.6g", v);
}

/* Keep the actual binary64 accumulator. Formatting is an output operation;
 * rounding through %.6g after every += used to lose small subsequent inputs. */
static void awk_var_set_num(awk_var_t* v, double n) { if (v) { v->is_num = 1; v->number = n; } }

static int awk_split_fields(awk_runtime_t* R, const char* line, int fs_is_ws, char fs_char) {
    strncpy(R->field0, line, sizeof R->field0 - 1); R->field0[sizeof R->field0 - 1] = '\0';
    strncpy(R->line_buf, line, sizeof R->line_buf - 1); R->line_buf[sizeof R->line_buf - 1] = '\0';
    R->nf = 0;
    char* p = R->line_buf;
    if (fs_is_ws) {
        while (*p == ' ' || *p == '\t') p++;
        while (*p && R->nf < AWK_MAX_FIELDS) {
            R->fields[R->nf++] = p;
            while (*p && *p != ' ' && *p != '\t') p++;
            if (*p) { *p = '\0'; p++; while (*p == ' ' || *p == '\t') p++; }
        }
        if (*p) return -1;
    } else {
        if (!*p) return 0;
        char* start = p;
        for (;;) {
            if (*p == fs_char) {
                *p = '\0';
                if (R->nf == AWK_MAX_FIELDS) return -1;
                R->fields[R->nf++] = start;
                p++; start = p;
                continue;
            }
            if (*p == '\0') {
                if (R->nf == AWK_MAX_FIELDS) return -1;
                R->fields[R->nf++] = start; break;
            }
            p++;
        }
    }
    return 0;
}

typedef struct { int is_num; double num; char str[AWK_MAX_LINE]; } awk_val_t;

static void awk_eval_expr(awk_runtime_t* R, const awk_expr_t* e, awk_val_t* out) {
    memset(out, 0, sizeof *out);
    switch (e->kind) {
        case EXPR_FIELD: {
            int idx = e->field_is_nf ? R->nf : e->field_idx;
            out->is_num = 0;
            if (idx == 0) strncpy(out->str, R->field0, sizeof out->str - 1);
            else if (idx >= 1 && idx <= R->nf) strncpy(out->str, R->fields[idx - 1], sizeof out->str - 1);
            else out->str[0] = '\0';
            break;
        }
        case EXPR_NR: out->is_num = 1; out->num = R->nr; break;
        case EXPR_NF: out->is_num = 1; out->num = R->nf; break;
        case EXPR_NUM: out->is_num = 1; out->num = e->num; break;
        case EXPR_STR: out->is_num = 0; strncpy(out->str, e->str, sizeof out->str - 1); break;
        case EXPR_VAR: {
            awk_var_t* v = awk_var_lookup(R, e->str);
            if (v && v->is_num) { out->is_num = 1; out->num = v->number; break; }
            const char* sv = v ? (v->external ? v->external : v->str) : "";
            char* endp;
            double d = strtod(sv, &endp);
            if (*sv != '\0' && *endp == '\0') { out->is_num = 1; out->num = d; }
            else { out->is_num = 0; strncpy(out->str, sv, sizeof out->str - 1); }
            break;
        }
    }
}

static void awk_val_to_text(const awk_val_t* v, char* buf, size_t cap) {
    if (v->is_num) awk_fmt_num(v->num, buf, cap);
    else { strncpy(buf, v->str, cap - 1); buf[cap - 1] = '\0'; }
}

static double awk_val_to_num(const awk_val_t* v) { return v->is_num ? v->num : strtod(v->str, NULL); }

static int awk_pattern_match(awk_runtime_t* R, const awk_pattern_t* pat) {
    if (pat->kind == PAT_ALWAYS) return 1;
    if (pat->kind == PAT_REGEX) {
        regmatch_t m[1];
        return pat->re_ok && regexec(&pat->re, R->field0, 1, m, 0) == 0;
    }
    /* PAT_RELCMP */
    double lnum = 0; char lstr[AWK_MAX_LINE]; int l_is_field = 0;
    lstr[0] = '\0';
    if (pat->loperand_kind == OPND_NR) lnum = R->nr;
    else if (pat->loperand_kind == OPND_NF) lnum = R->nf;
    else {
        int idx = pat->lfield_is_nf ? R->nf : pat->lfield_idx;
        const char* s = (idx == 0) ? R->field0 : (idx >= 1 && idx <= R->nf ? R->fields[idx - 1] : "");
        strncpy(lstr, s, sizeof lstr - 1); lstr[sizeof lstr - 1] = '\0';
        lnum = strtod(lstr, NULL);
        l_is_field = 1;
    }
    if (pat->relop == REL_MATCH || pat->relop == REL_NOMATCH) {
        char numbuf[64];
        regmatch_t m[1];
        int matched = 0;
        if (pat->re_ok) {
            if (l_is_field)
                matched = regexec(&pat->re, lstr, 1, m, 0) == 0;
            else {
                awk_fmt_num(lnum, numbuf, sizeof numbuf);
                matched = regexec(&pat->re, numbuf, 1, m, 0) == 0;
            }
        }
        return pat->relop == REL_MATCH ? matched : !matched;
    }
    if (!l_is_field || pat->rhs_is_num) {
        double rn = pat->rhs_is_num ? pat->rhs_num : strtod(pat->rhs_str, NULL);
        switch (pat->relop) {
            case REL_EQ: return lnum == rn;
            case REL_NE: return lnum != rn;
            case REL_LT: return lnum < rn;
            case REL_LE: return lnum <= rn;
            case REL_GT: return lnum > rn;
            case REL_GE: return lnum >= rn;
            default: return 0;
        }
    }
    /* field (string) vs string literal */
    int eq = strcmp(lstr, pat->rhs_str) == 0;
    if (pat->relop == REL_EQ) return eq;
    if (pat->relop == REL_NE) return !eq;
    double rn = strtod(pat->rhs_str, NULL);
    switch (pat->relop) {
        case REL_LT: return lnum < rn;
        case REL_LE: return lnum <= rn;
        case REL_GT: return lnum > rn;
        case REL_GE: return lnum >= rn;
        default: return 0;
    }
}

/* ── Output: text / json-array / json-aggregate ──────────────────────── */

typedef struct {
    jash_writer_t w; void* ctx;
    int mode;               /* 0=text 1=json array 2=json aggregate (END-only) */
    int json_first;
    int aggregate_emitted;
} awk_out_t;

static void awk_json_str(jash_writer_t w, void* ctx, const char* s) {
    w(ctx, "\"", 1);
    for (const unsigned char* p = (const unsigned char*)s; *p; p++) {
        if (*p == '"' || *p == '\\') { char e[2] = { '\\', (char)*p }; w(ctx, e, 2); }
        else if (*p == '\n') w(ctx, "\\n", 2);
        else if (*p == '\t') w(ctx, "\\t", 2);
        else if (*p < 0x20) { char e[8]; int k = snprintf(e, sizeof e, "\\u%04x", *p); w(ctx, e, (size_t)k); }
        else w(ctx, (const char*)p, 1);
    }
    w(ctx, "\"", 1);
}

static void awk_json_val(jash_writer_t w, void* ctx, const awk_val_t* v) {
    if (v->is_num) { char b[64]; awk_fmt_num(v->num, b, sizeof b); w(ctx, b, strlen(b)); }
    else awk_json_str(w, ctx, v->str);
}

static void awk_render_print_text(awk_runtime_t* R, const awk_stmt_t* st, jash_writer_t w, void* ctx) {
    if (st->nargs == 0) { w(ctx, R->field0, strlen(R->field0)); w(ctx, "\n", 1); return; }
    for (int i = 0; i < st->nargs; i++) {
        if (i) w(ctx, " ", 1);
        awk_val_t v; awk_eval_expr(R, &st->args[i], &v);
        char buf[AWK_MAX_LINE]; awk_val_to_text(&v, buf, sizeof buf);
        w(ctx, buf, strlen(buf));
    }
    w(ctx, "\n", 1);
}

static void awk_render_printf_buf(awk_runtime_t* R, const awk_stmt_t* st, char* buf, size_t cap) {
    size_t o = 0;
    const char* fmt = st->args[0].str;
    int argi = 1;
    for (const char* p = fmt; *p && o + 1 < cap; p++) {
        if (*p == '%') {
            p++;
            if (*p == '\0') break;
            if (*p == '%') { buf[o++] = '%'; continue; }
            awk_val_t v;
            if (argi < st->nargs) { awk_eval_expr(R, &st->args[argi], &v); argi++; }
            else { memset(&v, 0, sizeof v); v.is_num = 1; v.num = 0; }
            char tmp[256]; size_t l;
            if (*p == 's') {
                char t[AWK_MAX_LINE]; awk_val_to_text(&v, t, sizeof t);
                l = strlen(t); if (o + l >= cap) l = cap - o - 1;
                memcpy(buf + o, t, l); o += l;
            } else if (*p == 'd') {
                double n = awk_val_to_num(&v);
                int k = n >= -0x1p63 && n < 0x1p63
                    ? snprintf(tmp, sizeof tmp, "%lld", (long long)n)
                    : snprintf(tmp, sizeof tmp, "%.0f", n);
                if (k < 0 || (size_t)k >= sizeof tmp) k = 0;
                l = (size_t)k; if (o + l >= cap) l = cap - o - 1;
                memcpy(buf + o, tmp, l); o += l;
            } else if (*p == 'f') {
                int k = snprintf(tmp, sizeof tmp, "%f", awk_val_to_num(&v));
                if (k < 0 || (size_t)k >= sizeof tmp) k = 0;
                l = (size_t)k; if (o + l >= cap) l = cap - o - 1;
                memcpy(buf + o, tmp, l); o += l;
            }
            continue;
        }
        buf[o++] = *p;
    }
    buf[o < cap ? o : cap - 1] = '\0';
}

static void awk_render_print_json(awk_runtime_t* R, const awk_stmt_t* st, awk_out_t* O) {
    if (!O->json_first) O->w(O->ctx, ",", 1);
    O->json_first = 0;
    O->w(O->ctx, "{\"fields\":[", 11);
    if (st->nargs == 0) {
        awk_val_t v; memset(&v, 0, sizeof v); strncpy(v.str, R->field0, sizeof v.str - 1);
        awk_json_val(O->w, O->ctx, &v);
    } else {
        for (int i = 0; i < st->nargs; i++) {
            if (i) O->w(O->ctx, ",", 1);
            awk_val_t v; awk_eval_expr(R, &st->args[i], &v);
            awk_json_val(O->w, O->ctx, &v);
        }
    }
    O->w(O->ctx, "]}", 2);
}

static void awk_render_print_aggregate(awk_runtime_t* R, const awk_stmt_t* st, awk_out_t* O) {
    if (O->aggregate_emitted) return;
    O->aggregate_emitted = 1;
    if (st->nargs == 1) {
        O->w(O->ctx, "{\"result\":", 10);
        awk_val_t v; awk_eval_expr(R, &st->args[0], &v);
        awk_json_val(O->w, O->ctx, &v);
        O->w(O->ctx, "}\n", 2);
    } else {
        O->w(O->ctx, "{\"fields\":[", 11);
        for (int i = 0; i < st->nargs; i++) {
            if (i) O->w(O->ctx, ",", 1);
            awk_val_t v; awk_eval_expr(R, &st->args[i], &v);
            awk_json_val(O->w, O->ctx, &v);
        }
        O->w(O->ctx, "]}\n", 3);
    }
}

static void awk_exec_stmt(awk_runtime_t* R, const awk_stmt_t* st, awk_out_t* O, int in_end) {
    switch (st->kind) {
        case STMT_PRINT:
            if (O->mode == 0) awk_render_print_text(R, st, O->w, O->ctx);
            else if (O->mode == 2 && in_end) awk_render_print_aggregate(R, st, O);
            else awk_render_print_json(R, st, O);
            break;
        case STMT_PRINTF: {
            char buf[8192]; awk_render_printf_buf(R, st, buf, sizeof buf);
            if (O->mode == 0) O->w(O->ctx, buf, strlen(buf));
            else if (O->mode == 2 && in_end) { /* aggregate mode: printf in END has no scalar shape, skip */ }
            else {
                if (!O->json_first) O->w(O->ctx, ",", 1);
                O->json_first = 0;
                O->w(O->ctx, "{\"text\":", 8);
                awk_json_str(O->w, O->ctx, buf);
                O->w(O->ctx, "}", 1);
            }
            break;
        }
        case STMT_ADD: {
            awk_var_t* v = awk_var_lookup(R, st->var);
            awk_val_t rv; awk_eval_expr(R, &st->rhs, &rv);
            awk_var_set_num(v, awk_var_get_num(v) + awk_val_to_num(&rv));
            break;
        }
        case STMT_SUB: {
            awk_var_t* v = awk_var_lookup(R, st->var);
            awk_val_t rv; awk_eval_expr(R, &st->rhs, &rv);
            awk_var_set_num(v, awk_var_get_num(v) - awk_val_to_num(&rv));
            break;
        }
    }
}

static void awk_exec_block(awk_runtime_t* R, const awk_block_t* B, awk_out_t* O, int in_end) {
    for (int i = 0; i < B->n; i++) awk_exec_stmt(R, &B->stmts[i], O, in_end);
}

static int awk_block_has_print(const awk_block_t* B) {
    for (int i = 0; i < B->n; i++) if (B->stmts[i].kind == STMT_PRINT || B->stmts[i].kind == STMT_PRINTF) return 1;
    return 0;
}

static int awk_block_has_print_stmt(const awk_block_t* B) {
    for (int i = 0; i < B->n; i++) if (B->stmts[i].kind == STMT_PRINT) return 1;
    return 0;
}

/* Reconstruct single-quoted argv and defer to the real awk. */
static int awk_fallback(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    char args[8192];
    size_t o = 0;
    for (int i = 0; i < argc && o + 4 < sizeof args; i++) {
        if (i) args[o++] = ' ';
        args[o++] = '\'';
        for (const char* p = argv[i]; *p && o + 5 < sizeof args; p++) {
            if (*p == '\'') { args[o++] = '\''; args[o++] = '\\'; args[o++] = '\''; args[o++] = '\''; }
            else args[o++] = *p;
        }
        args[o++] = '\'';
    }
    args[o] = '\0';
    return jash_exec_external(s, w, ctx, "awk", args);
}

#include "awk_query.h"

int jash_b_awk(jash_state_t* s, jash_writer_t w, void* ctx, int argc, char** argv) {
    int structured_mode = awk_query_mode(argc, argv);
    if (structured_mode) return awk_query_run(s, w, ctx, argc, argv, structured_mode == 2);
    int json_present = 0;

    int json = 0;
    int fs_is_ws = 1; char fs_char = ' ';
    int have_v = 0;
    char v_name[AWK_STRBUF] = { 0 }, v_val[1024] = { 0 };
    int i = 0;
    int bad = 0;

    for (; i < argc; i++) {
        const char* a = argv[i];
        if (strcmp(a, "--") == 0) { i++; break; }
        if (strcmp(a, "--json") == 0) { json = 1; continue; }
        if (a[0] == '-' && a[1] != '\0') {
            if (a[1] == 'F') {
                const char* v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
                if (!v || strlen(v) != 1) { bad = 1; break; }
                if (v[0] == ' ') fs_is_ws = 1;
                else { fs_is_ws = 0; fs_char = v[0]; }
                continue;
            }
            if (a[1] == 'v') {
                if (have_v) { bad = 1; break; }
                const char* v = a[2] ? a + 2 : (i + 1 < argc ? argv[++i] : NULL);
                if (!v) { bad = 1; break; }
                const char* eq = strchr(v, '=');
                if (!eq) { bad = 1; break; }
                size_t nlen = (size_t)(eq - v);
                if (nlen == 0 || nlen >= sizeof v_name) { bad = 1; break; }
                memcpy(v_name, v, nlen); v_name[nlen] = '\0';
                if (awk_reserved_assign_target(v_name) || awk_is_reserved_kw(v_name)) { bad = 1; break; }
                strncpy(v_val, eq + 1, sizeof v_val - 1);
                have_v = 1;
                continue;
            }
            bad = 1; break; /* -f progfile, GNU long opts, etc. — not owned */
        }
        break; /* first non-flag token: the program string */
    }

    if (bad || i >= argc) {
        if (json_present) { jash_writef(jash_stderr_writer, 0, "awk: --json: unsupported flags or missing program\n"); return 2; }
        return awk_fallback(s, w, ctx, argc, argv);
    }

    const char* program_str = argv[i++];
    char** files = &argv[i];
    int nfiles = argc - i;

    awk_toklist_t L;
    if (awk_tokenize(program_str, &L) != 0) {
        if (json_present) { jash_writef(jash_stderr_writer, 0, "awk: --json: program has unterminated string/regex literal\n"); return 2; }
        return awk_fallback(s, w, ctx, argc, argv);
    }
    awk_program_t prog;
    if (parse_program(&L, &prog) != 0) {
        if (json_present) { jash_writef(jash_stderr_writer, 0, "awk: --json: program uses constructs outside the in-process subset\n"); return 2; }
        return awk_fallback(s, w, ctx, argc, argv);
    }
    if (awk_pattern_prepare(&prog.pattern) != 0) {
        if (json_present) { jash_writef(jash_stderr_writer, 0, "awk: --json: invalid regular expression in program\n"); return 2; }
        return awk_fallback(s, w, ctx, argc, argv);
    }

    int main_has_print = prog.main_implicit_print || awk_block_has_print(&prog.main_action);
    int begin_has_print = awk_block_has_print(&prog.begin);
    int end_has_print_stmt = awk_block_has_print_stmt(&prog.end);
    int aggregate_mode = json && !main_has_print && !begin_has_print && prog.has_end && end_has_print_stmt;
    int mode = !json ? 0 : (aggregate_mode ? 2 : 1);

    awk_runtime_t* R = malloc(sizeof(awk_runtime_t));
    if (!R) { awk_pattern_cleanup(&prog.pattern); jash_writef(jash_stderr_writer, 0, "awk: out of memory\n"); return 2; }
    memset(R, 0, sizeof *R);
    if (have_v) { awk_var_t* vv = awk_var_lookup(R, v_name); strncpy(vv->str, v_val, sizeof vv->str - 1); }

    awk_out_t O = { w, ctx, mode, 1, 0 };
    if (mode == 1) w(ctx, "[", 1);

    int rc = 0;
    if (prog.has_begin) awk_exec_block(R, &prog.begin, &O, 0);

    awk_stmt_t implicit_print; memset(&implicit_print, 0, sizeof implicit_print);
    implicit_print.kind = STMT_PRINT; implicit_print.nargs = 0;

    int nsources = nfiles > 0 ? nfiles : 1;
    for (int fi = 0; fi < nsources; fi++) {
        FILE* f;
        const char* fname = nfiles > 0 ? files[fi] : "-";
        if (nfiles > 0) {
            f = fopen(fname, "r");
            if (!f) {
                jash_writef(jash_stderr_writer, 0, "awk: %s: %s\n", fname, strerror(errno));
                rc = 2;
                continue;
            }
        } else {
            f = stdin;
        }

        char* line = malloc(AWK_MAX_LINE);
        if (!line) { if (f != stdin) (void)fclose(f); continue; }
        while (fgets(line, AWK_MAX_LINE, f)) {
            size_t ll = strlen(line);
            if (ll && line[ll - 1] == '\n') line[ll - 1] = '\0';
            R->nr += 1;
            awk_split_fields(R, line, fs_is_ws, fs_char);
            if (prog.has_main && awk_pattern_match(R, &prog.pattern)) {
                if (prog.main_implicit_print) awk_exec_stmt(R, &implicit_print, &O, 0);
                else awk_exec_block(R, &prog.main_action, &O, 0);
            }
        }
        free(line);
        if (f != stdin) (void)fclose(f);
    }

    if (prog.has_end) awk_exec_block(R, &prog.end, &O, 1);

    if (mode == 1) w(ctx, "]\n", 2);
    else if (mode == 2 && !O.aggregate_emitted) w(ctx, "{}\n", 3);

    free(R);
    awk_pattern_cleanup(&prog.pattern);
    return rc;
}
