// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — builtins/context — the context compiler for AI agents (F0).
//
// No other shell does this. jash IS the shell, so it can hand an agent a
// token-budgeted digest of the repo instead of forcing it to read 20 files.
//
//   context              — "where am I" bundle: intent + git + recent cmds
//   context map [dir]     — repo digest: tree + function signatures + size
//   context fold <file>   — signatures-only skeleton (brace-depth body collapse)
//
// 100% sovereign: pure C + dirent. git/session_meta used only if present.
// F E R M I ∞ H A R T

#define _GNU_SOURCE
#include "builtin.h"
#include <core/output_budget.h>
#include <theme/io.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <time.h>

#define CTX_MAP_BUDGET_DEFAULT 32768   /* bytes of map output before truncation */
#define CTX_MAX_DEPTH          8
#define CTX_MAX_FILES          4000

/* ── ask: query-driven code digest (ADR-0027) ───────────────────────────── */
#define ASK_MAX_TERMS      16
#define ASK_MAX_FILES      128
#define ASK_MAX_SIGS       64
#define ASK_MAX_SNIPPETS   8
#define ASK_SNIPPET_LINES  3
#define ASK_MAX_FILE_SIZE  (1024*1024) /* skip files > 1MB */
#define ASK_PHASE1_MAX_LINES 50        /* quick-scan only top of each file */

typedef struct {
    char terms[ASK_MAX_TERMS][48];
    int  n_terms;
} ask_query_t;

typedef struct {
    char  path[1024];
    int   lines;
    long  score;
    char  sigs[ASK_MAX_SIGS][256];
    int   n_sigs;
    int   sig_lines[ASK_MAX_SIGS];
    int   snippet_line[ASK_MAX_SNIPPETS];
    char  snippet_text[ASK_MAX_SNIPPETS][512];
    int   n_snippets;
} ask_file_t;

/* ── source-file detection ──────────────────────────────────────────────── */

static int ctx_is_source(const char* name) {
    const char* dot = strrchr(name, '.');
    if (!dot) return 0;
    static const char* exts[] = {
        ".c",".h",".cc",".cpp",".hpp",".py",".js",".ts",".go",".rs",
        ".sh",".java",".rb",".lua",".zig", NULL
    };
    for (int i = 0; exts[i]; i++) if (strcmp(dot, exts[i]) == 0) return 1;
    return 0;
}

static int ctx_skip_dir(const char* name) {
    static const char* skip[] = {
        ".git","build","build-speed","node_modules",".cache","target",
        ".venv","__pycache__","oci-export",".Trash","dist",
        "Quarentena-codes","osif-macos-backup", NULL
    };
    if (name[0] == '.' && (name[1] == '\0' || (name[1]=='.' && name[2]=='\0')))
        return 1;
    for (int i = 0; skip[i]; i++) if (strcmp(name, skip[i]) == 0) return 1;
    return 0;
}

/* ── C/brace declaration heuristic ──────────────────────────────────────── */
/* Returns 1 if a column-0 line looks like a top-level declaration worth
 * showing in a map (function def/proto, typedef/struct/enum opener). */
static int ctx_is_decl(const char* ln) {
    if (!ln[0] || ln[0]==' ' || ln[0]=='\t') return 0;       /* indented = body */
    if (ln[0]=='}' || ln[0]=='{' || ln[0]=='/' || ln[0]=='*' ||
        ln[0]=='#' || ln[0]==')') return 0;
    /* control keywords that look like calls */
    static const char* kw[] = {"if(","if ","for(","for ","while(","while ",
        "switch(","switch ","return","else","do ","do{","case ","goto ", NULL};
    for (int i = 0; kw[i]; i++)
        if (strncmp(ln, kw[i], strlen(kw[i])) == 0) return 0;

    /* aggregate openers */
    if (!strncmp(ln,"typedef",7) || !strncmp(ln,"struct ",7) ||
        !strncmp(ln,"enum ",5)   || !strncmp(ln,"union ",6))
        return 1;
    /* function-ish: has '(' and last NON-blank char is '{', ')', ';' or ','
     * (line may still carry a trailing newline — ignore trailing whitespace). */
    if (strchr(ln, '(')) {
        size_t len = strlen(ln);
        while (len && (ln[len-1]=='\n'||ln[len-1]=='\r'||ln[len-1]==' '||ln[len-1]=='\t'))
            len--;
        if (len) {
            char last = ln[len-1];
            if (last=='{' || last==')' || last==';' || last==',') return 1;
        }
    }
    return 0;
}

/* trim trailing whitespace and a dangling '{' for clean signatures */
static void ctx_clean_sig(char* ln) {
    size_t len = strlen(ln);
    while (len && (ln[len-1]=='\n'||ln[len-1]=='\r'||ln[len-1]==' '||ln[len-1]=='\t'))
        ln[--len] = '\0';
    if (len && ln[len-1]=='{') { ln[--len]='\0';
        while (len && (ln[len-1]==' '||ln[len-1]=='\t')) ln[--len]='\0'; }
}

/* ── map: walk + emit signatures, budget-aware ──────────────────────────── */

static int g_ctx_files;

static void ctx_map_file(const char* path, const char* rel,
                         jash_writer_t w, void* ctx, long* budget) {
    FILE* f = fopen(path, "r");
    if (!f) return;
    char line[2048];
    int nlines = 0, nsig = 0;
    long bytes = 0;
    /* first pass would cost a second open; instead buffer signatures */
    char sigs[64][200];
    while (fgets(line, sizeof(line), f)) {
        nlines++;
        bytes += (long)strlen(line);
        if (nsig < 64 && ctx_is_decl(line)) {
            ctx_clean_sig(line);
            if (line[0]) { snprintf(sigs[nsig], sizeof(sigs[nsig]), "%s", line); nsig++; }
        }
    }
    fclose(f);
    if (*budget <= 0) return;
    long n = jash_writef(w, ctx, "\n\033[1m%s\033[0m  \033[90m(%d lines, %ldB)\033[0m\n",
                         rel, nlines, bytes);
    *budget -= n;
    for (int i = 0; i < nsig && *budget > 0; i++) {
        n = jash_writef(w, ctx, "  %s\n", sigs[i]);
        *budget -= n;
    }
}

static void ctx_walk(const char* dir, const char* rel, int depth,
                     jash_writer_t w, void* ctx, long* budget) {
    if (depth > CTX_MAX_DEPTH || *budget <= 0 || g_ctx_files >= CTX_MAX_FILES)
        return;
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && *budget > 0 && g_ctx_files < CTX_MAX_FILES) {
        if (e->d_name[0] == '.' && strcmp(e->d_name, ".") != 0) {
            /* allow dotfiles that are sources? skip hidden dirs/files for noise */
            if (e->d_type == DT_DIR) continue;
        }
        char child[2048], crel[1024];
        snprintf(child, sizeof(child), "%s/%s", dir, e->d_name);
        snprintf(crel, sizeof(crel), "%s%s%s", rel, rel[0]?"/":"", e->d_name);
        if (e->d_type == DT_DIR) {
            if (ctx_skip_dir(e->d_name)) continue;
            ctx_walk(child, crel, depth+1, w, ctx, budget);
        } else if (e->d_type == DT_REG && ctx_is_source(e->d_name)) {
            g_ctx_files++;
            ctx_map_file(child, crel, w, ctx, budget);
        }
    }
    closedir(d);
}

static int ctx_cmd_map(const char* arg, jash_writer_t w, void* ctx) {
    char dir[1024];
    if (arg && arg[0]) snprintf(dir, sizeof(dir), "%s", arg);
    else snprintf(dir, sizeof(dir), ".");
    long budget = CTX_MAP_BUDGET_DEFAULT;
    const char* b = getenv("JASH_CONTEXT_BUDGET");
    if (b && b[0]) { long v = atol(b); if (v > 1024) budget = v; }
    g_ctx_files = 0;
    jash_writef(w, ctx, "\033[1;36m# context map: %s\033[0m\n", dir);
    ctx_walk(dir, "", 0, w, ctx, &budget);
    jash_writef(w, ctx, "\n\033[90m# %d files mapped%s\033[0m\n",
                g_ctx_files, budget <= 0 ? " (budget reached — raise JASH_CONTEXT_BUDGET)" : "");
    return 0;
}

/* ── fold: signatures-only skeleton via brace-depth collapse ─────────────── */

static int ctx_brace_delta(const char* ln) {
    int d = 0, sq = 0, dq = 0;
    for (const char* p = ln; *p; p++) {
        if (sq) { if (*p=='\\' && p[1]) p++; else if (*p=='\'') sq=0; continue; }
        if (dq) { if (*p=='\\' && p[1]) p++; else if (*p=='"') dq=0; continue; }
        if (*p=='\'') sq=1; else if (*p=='"') dq=1;
        else if (*p=='{') d++; else if (*p=='}') d--;
    }
    return d;
}

static int ctx_cmd_fold(const char* file, jash_writer_t w, void* ctx) {
    if (!file || !file[0]) { jash_writes(w, ctx, "context fold: need a file\n"); return 1; }
    FILE* f = fopen(file, "r");
    if (!f) { jash_writef(w, ctx, "context fold: cannot open %s\n", file); return 1; }
    jash_writef(w, ctx, "\033[1;36m# fold: %s\033[0m\n", file);
    char line[4096];
    int depth = 0, collapsed = 0;
    while (fgets(line, sizeof(line), f)) {
        int delta = ctx_brace_delta(line);
        if (depth == 0) {
            jash_writef(w, ctx, "%s", line);
            if (delta > 0) { jash_writes(w, ctx, "    \033[90m{ ... }\033[0m\n"); depth += delta; collapsed++; }
        } else {
            depth += delta;
            if (depth <= 0) depth = 0;   /* closing line suppressed */
        }
    }
    fclose(f);
    jash_writef(w, ctx, "\033[90m# %d bodies collapsed\033[0m\n", collapsed);
    return 0;
}

/* ── ask: query-driven code digest (ADR-0027) ───────────────────────────── */

static void ask_str_tolower(char* s) {
    for (; *s; s++) *s = (char)tolower((unsigned char)*s);
}

static int ask_is_ident_char(unsigned char c) {
    return isalnum(c) || c == '_';
}

static int ask_tokenize(const char* query, ask_query_t* q) {
    q->n_terms = 0;
    const char* p = query;
    while (*p && q->n_terms < ASK_MAX_TERMS) {
        while (*p && !ask_is_ident_char((unsigned char)*p)) p++;
        if (!*p) break;
        const char* start = p;
        while (*p && ask_is_ident_char((unsigned char)*p)) p++;
        size_t len = (size_t)(p - start);
        if (len < 2 || len >= sizeof(q->terms[0])) continue;
        memcpy(q->terms[q->n_terms], start, len);
        q->terms[q->n_terms][len] = '\0';
        ask_str_tolower(q->terms[q->n_terms]);
        q->n_terms++;
    }
    return q->n_terms;
}

static int ask_term_in_text(const char* term, const char* text) {
    if (!term[0] || !text) return 0;
    char tbuf[512], sbuf[4096];
    snprintf(tbuf, sizeof(tbuf), "%s", term);
    ask_str_tolower(tbuf);
    snprintf(sbuf, sizeof(sbuf), "%s", text);
    ask_str_tolower(sbuf);
    return strstr(sbuf, tbuf) != NULL;
}

static int ask_path_score(const ask_query_t* q, const char* rel) {
    int score = 0;
    char path[1024];
    snprintf(path, sizeof(path), "%s", rel);
    ask_str_tolower(path);
    for (int i = 0; i < q->n_terms; i++) {
        if (strstr(path, q->terms[i])) score += 3;
    }
    return score;
}

static int ask_intent_score(const ask_query_t* q) {
    const char* intent = getenv("JASH_INTENT");
    if (!intent || !intent[0]) {
        const char* home = getenv("HOME");
        static char ibuf[512];
        if (home) {
            char ip[600]; snprintf(ip, sizeof(ip), "%s/.jash/intent", home);
            FILE* fi = fopen(ip, "r");
            if (fi) { if (fgets(ibuf, sizeof(ibuf), fi)) intent = ibuf; fclose(fi); }
        }
    }
    if (!intent || !intent[0]) return 0;
    int score = 0;
    for (int i = 0; i < q->n_terms; i++) {
        if (ask_term_in_text(q->terms[i], intent)) score += 5;
    }
    return score;
}

static int ask_git_recent_score(const ask_query_t* q, const char* rel) {
    (void)q;
    (void)rel;
    /* TODO: parse git status --short / timeline for recency boost */
    return 0;
}

/* Single-pass file analysis. Reads the file once, detects term matches,
 * extracts matched signatures, and captures snippets around term hits.
 * Returns 1 if any query term was found in the file body. */
static int ask_extract_file(const char* path, const ask_query_t* q,
                            ask_file_t* f, long* budget) {
    (void)budget;
    struct stat st;
    if (stat(path, &st) < 0) return 0;
    if (st.st_size > ASK_MAX_FILE_SIZE) return 0;

    FILE* fp = fopen(path, "r");
    if (!fp) return 0;

    char line[4096];
    char line_lower[4096];
    int line_no = 0;
    int body_match_any = 0;
    f->n_sigs = 0;
    f->n_snippets = 0;

    while (fgets(line, sizeof(line), fp)) {
        line_no++;
        f->lines++;

        snprintf(line_lower, sizeof(line_lower), "%s", line);
        ask_str_tolower(line_lower);
        int line_match = 0;
        for (int i = 0; i < q->n_terms; i++) {
            if (strstr(line_lower, q->terms[i])) { line_match = 1; body_match_any = 1; break; }
        }

        if (f->n_sigs < ASK_MAX_SIGS && ctx_is_decl(line)) {
            char sig[512];
            snprintf(sig, sizeof(sig), "%s", line);
            ctx_clean_sig(sig);
            if (sig[0]) {
                char sig_lower[512];
                snprintf(sig_lower, sizeof(sig_lower), "%s", sig);
                ask_str_tolower(sig_lower);
                int sig_match = 0;
                for (int i = 0; i < q->n_terms; i++) {
                    if (strstr(sig_lower, q->terms[i])) { sig_match = 1; break; }
                }
                /* Only keep signatures if they match the query OR the file is
                 * already known to be relevant (body match) and we want a few
                 * top signatures for context. */
                if (sig_match || (body_match_any && f->n_sigs < 12)) {
                    snprintf(f->sigs[f->n_sigs], sizeof(f->sigs[0]), "%s", sig);
                    f->sig_lines[f->n_sigs] = line_no;
                    f->n_sigs++;
                }
            }
        }

        if (line_match && f->n_snippets < ASK_MAX_SNIPPETS) {
            int dup = 0;
            for (int j = 0; j < f->n_snippets; j++) {
                if (abs(f->snippet_line[j] - line_no) < ASK_SNIPPET_LINES * 2) { dup = 1; break; }
            }
            if (!dup) {
                f->snippet_line[f->n_snippets] = line_no;
                snprintf(f->snippet_text[f->n_snippets], sizeof(f->snippet_text[0]), "%s", line);
                f->n_snippets++;
            }
        }
    }
    fclose(fp);
    return body_match_any;
}

/* Quick one-pass scorer: counts body term hits and signature term hits without
 * allocating snippets or storing signatures. Used for phase-1 filtering. */
typedef struct {
    char  path[1024];
    char  abspath[2048];
    long  quick_score;
} ask_candidate_t;

static long ask_quick_score_file(const char* path, const ask_query_t* q,
                                 int* out_lines) {
    struct stat st;
    if (stat(path, &st) < 0) return -1;
    if (st.st_size > ASK_MAX_FILE_SIZE) return -1;
    FILE* fp = fopen(path, "r");
    if (!fp) return -1;

    char line[4096];
    char line_lower[4096];
    int line_no = 0;
    long score = 0;
    while (fgets(line, sizeof(line), fp) && line_no < ASK_PHASE1_MAX_LINES) {
        line_no++;
        snprintf(line_lower, sizeof(line_lower), "%s", line);
        ask_str_tolower(line_lower);
        int line_match = 0;
        for (int i = 0; i < q->n_terms; i++) {
            if (strstr(line_lower, q->terms[i])) { line_match = 1; score += 1; }
        }
        if (line_match && ctx_is_decl(line)) {
            char sig[512];
            snprintf(sig, sizeof(sig), "%s", line);
            ctx_clean_sig(sig);
            char sig_lower[512];
            snprintf(sig_lower, sizeof(sig_lower), "%s", sig);
            ask_str_tolower(sig_lower);
            for (int i = 0; i < q->n_terms; i++) {
                if (strstr(sig_lower, q->terms[i])) score += 5;
            }
        }
    }
    fclose(fp);
    *out_lines = line_no;
    return score;
}

static int ask_cmp_candidate(const void* a, const void* b) {
    long sa = ((const ask_candidate_t*)a)->quick_score;
    long sb = ((const ask_candidate_t*)b)->quick_score;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return strcmp(((const ask_candidate_t*)a)->path, ((const ask_candidate_t*)b)->path);
}

static int ask_qsort_cmp(const void* a, const void* b) {
    long sa = ((const ask_file_t*)a)->score;
    long sb = ((const ask_file_t*)b)->score;
    if (sa > sb) return -1;
    if (sa < sb) return 1;
    return strcmp(((const ask_file_t*)a)->path, ((const ask_file_t*)b)->path);
}

static int ask_qsort_name_cmp(const void* a, const void* b) {
    const char* const* sa = (const char* const*)a;
    const char* const* sb = (const char* const*)b;
    return strcmp(*sa, *sb); /* INV-528 */
}

/* Phase 1: recursively collect quick scores. Returns number of candidates. */
static int ask_walk_phase1(const char* dir, const char* rel, int depth,
                           const ask_query_t* q,
                           ask_candidate_t* cands, int max_cands) {
    if (depth > CTX_MAX_DEPTH) return 0;
    DIR* d = opendir(dir);
    if (!d) return 0;

    struct dirent* e;
    char* names[CTX_MAX_FILES];
    int nn = 0;
    while ((e = readdir(d)) && nn < CTX_MAX_FILES) {
        if (ctx_skip_dir(e->d_name)) continue;
        names[nn] = strdup(e->d_name);
        if (names[nn]) nn++;
    }
    closedir(d);
    qsort(names, (size_t)nn, sizeof(char*), ask_qsort_name_cmp);

    int nc = 0;
    for (int i = 0; i < nn && nc < max_cands; i++) {
        char child[2048], crel[1024];
        if (rel[0]) snprintf(crel, sizeof(crel), "%s/%s", rel, names[i]);
        else        snprintf(crel, sizeof(crel), "%s", names[i]);
        snprintf(child, sizeof(child), "%s/%s", dir, names[i]);

        struct stat st;
        if (stat(child, &st) < 0) { free(names[i]); continue; }

        if (S_ISDIR(st.st_mode)) {
            if (!ctx_skip_dir(names[i])) {
                int sub = ask_walk_phase1(child, crel, depth+1, q, cands + nc, max_cands - nc);
                nc += sub;
            }
        } else if (S_ISREG(st.st_mode) && ctx_is_source(names[i])) {
            long path_score = ask_path_score(q, crel);
            int lines = 0;
            long body_score = ask_quick_score_file(child, q, &lines);
            if (body_score < 0) body_score = 0;
            long total = path_score * 3 + body_score;
            /* keep candidates with path match or any body relevance */
            if (total > 0) {
                snprintf(cands[nc].path, sizeof(cands[nc].path), "%s", crel);
                snprintf(cands[nc].abspath, sizeof(cands[nc].abspath), "%s", child);
                cands[nc].quick_score = total;
                nc++;
            }
        }
        free(names[i]);
    }
    return nc;
}

/* Two-phase ask: quick-score everything, then deeply extract top-K. */
static void ask_walk(const char* root, const ask_query_t* q,
                     ask_file_t* files, int* nfiles) {
    ask_candidate_t cands[ASK_MAX_FILES];
    int nc = ask_walk_phase1(root, "", 0, q, cands, ASK_MAX_FILES);
    qsort(cands, (size_t)nc, sizeof(ask_candidate_t), ask_cmp_candidate);

    int intent_score = ask_intent_score(q);
    int top_k = nc < 32 ? nc : 32;
    *nfiles = 0;
    for (int i = 0; i < top_k && *nfiles < ASK_MAX_FILES; i++) {
        ask_file_t* f = &files[*nfiles];
        memset(f, 0, sizeof(*f));
        snprintf(f->path, sizeof(f->path), "%s", cands[i].path);
        f->score = cands[i].quick_score + intent_score;
        f->score += ask_git_recent_score(q, cands[i].path);
        ask_extract_file(cands[i].abspath, q, f, NULL);
        for (int t = 0; t < q->n_terms; t++) {
            for (int s = 0; s < f->n_sigs; s++) {
                if (ask_term_in_text(q->terms[t], f->sigs[s])) f->score += 5;
            }
            f->score += f->n_snippets;
        }
        if (f->score > 0) (*nfiles)++;
    }
}

/* minimal JSON escaping: \\, ", \n, \r, \t */
static void ask_json_esc(jash_writer_t w, void* ctx, const char* s) {
    for (const char* p = s; *p; p++) {
        switch (*p) {
            case '\\': jash_writes(w, ctx, "\\\\"); break;
            case '"':  jash_writes(w, ctx, "\\\""); break;
            case '\n': jash_writes(w, ctx, "\\n"); break;
            case '\r': jash_writes(w, ctx, "\\r"); break;
            case '\t': jash_writes(w, ctx, "\\t"); break;
            default:   jash_writef(w, ctx, "%c", *p); break;
        }
    }
}

static void ask_emit_json(const char* query, ask_file_t* files, int nfiles,
                          long budget, int truncated,
                          jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "{\n");
    jash_writef(w, ctx, "  \"context_ask\": {\n");
    jash_writef(w, ctx, "    \"query\": \""); ask_json_esc(w, ctx, query); jash_writes(w, ctx, "\",\n");
    jash_writef(w, ctx, "    \"budget_bytes\": %ld,\n", budget);
    jash_writef(w, ctx, "    \"truncated\": %s,\n", truncated ? "true" : "false");
    jash_writef(w, ctx, "    \"overflow_path\": null,\n");
    jash_writef(w, ctx, "    \"files\": [\n");
    for (int i = 0; i < nfiles; i++) {
        const ask_file_t* f = &files[i];
        jash_writes(w, ctx, "      {\n");
        jash_writef(w, ctx, "        \"rank\": %d,\n", i+1);
        jash_writef(w, ctx, "        \"path\": \""); ask_json_esc(w, ctx, f->path); jash_writes(w, ctx, "\",\n");
        jash_writef(w, ctx, "        \"lines\": %d,\n", f->lines);
        jash_writef(w, ctx, "        \"score\": %ld,\n", f->score);
        jash_writef(w, ctx, "        \"signatures\": [\n");
        for (int s = 0; s < f->n_sigs; s++) {
            jash_writes(w, ctx, "          \""); ask_json_esc(w, ctx, f->sigs[s]);
            jash_writef(w, ctx, "\"%s\n", s + 1 < f->n_sigs ? "," : "");
        }
        jash_writes(w, ctx, "        ],\n");
        jash_writef(w, ctx, "        \"snippets\": [\n");
        for (int s = 0; s < f->n_snippets; s++) {
            jash_writes(w, ctx, "          {\"line\": ");
            jash_writef(w, ctx, "%d, \"text\": \"", f->snippet_line[s]);
            ask_json_esc(w, ctx, f->snippet_text[s]);
            jash_writef(w, ctx, "\"}%s\n", s + 1 < f->n_snippets ? "," : "");
        }
        jash_writes(w, ctx, "        ]\n");
        jash_writef(w, ctx, "      }%s\n", i + 1 < nfiles ? "," : "");
    }
    jash_writes(w, ctx, "    ]\n");
    jash_writes(w, ctx, "  }\n");
    jash_writes(w, ctx, "}\n");
}

static int ctx_cmd_ask(const char* args, jash_writer_t w, void* ctx) {
    const char* a = args ? args : "";
    while (*a == ' ' || *a == '\t') a++;

    int json_mode = 0;
    static char query[1024];
    query[0] = '\0';

    /* parse trailing --json (handles both quoted and unquoted queries) */
    size_t len = strlen(a);
    if (len > 7 && strcmp(a + len - 7, " --json") == 0) {
        json_mode = 1;
        len -= 7;
    } else if (len > 8 && strcmp(a + len - 8, "\" --json") == 0) {
        json_mode = 1;
        len -= 8;
    }

    if (len >= sizeof(query)) len = sizeof(query) - 1;
    if (len > 0) {
        memcpy(query, a, len);
        query[len] = '\0';
        /* strip surrounding quotes */
        size_t ql = strlen(query);
        if (ql >= 2 && ((query[0] == '"' && query[ql-1] == '"') ||
                        (query[0] == '\'' && query[ql-1] == '\''))) {
            memmove(query, query + 1, ql - 2);
            query[ql - 2] = '\0';
        }
    }

    if (!query[0]) {
        jash_writes(w, ctx, "context ask: need a query string\n");
        return 1;
    }

    ask_query_t q;
    if (ask_tokenize(query, &q) == 0) {
        jash_writes(w, ctx, "context ask: query has no searchable terms\n");
        return 1;
    }

    long budget = CTX_MAP_BUDGET_DEFAULT;
    const char* b = getenv("JASH_CONTEXT_BUDGET");
    if (b && b[0]) { long v = atol(b); if (v > 1024) budget = v; }

    ask_file_t files[ASK_MAX_FILES];
    int nfiles = 0;

    char root[1024];
    if (!getcwd(root, sizeof(root))) {
        jash_writes(w, ctx, "context ask: cannot determine cwd\n");
        return 1;
    }

    ask_walk(root, &q, files, &nfiles);
    qsort(files, (size_t)nfiles, sizeof(ask_file_t), ask_qsort_cmp);

    int top_n = nfiles < 10 ? nfiles : 10;
    int truncated = 0;

    if (json_mode) {
        ask_emit_json(query, files, top_n, budget, truncated, w, ctx);
    } else {
        long remaining = budget;
        remaining -= jash_writef(w, ctx, "\033[1;36m# context ask: %s\033[0m\n", query);
        remaining -= jash_writef(w, ctx, "\033[90m# budget: %ld bytes | %d ranked files, showing top %d\033[0m\n\n",
                                 budget, nfiles, top_n);
        int emitted = 0;
        for (int i = 0; i < top_n && remaining > 0; i++) {
            const ask_file_t* f = &files[i];
            long n = jash_writef(w, ctx, "\033[1m%s\033[0m  \033[90m(%d lines, score %ld)\033[0m\n",
                                 f->path, f->lines, f->score);
            remaining -= n;
            for (int s = 0; s < f->n_sigs && remaining > 0; s++) {
                n = jash_writef(w, ctx, "  [sig:%d] %s\n", f->sig_lines[s], f->sigs[s]);
                remaining -= n;
            }
            for (int s = 0; s < f->n_snippets && remaining > 0; s++) {
                n = jash_writef(w, ctx, "  [snip:%d] %s", f->snippet_line[s], f->snippet_text[s]);
                remaining -= n;
            }
            if (remaining > 0) {
                remaining -= jash_writes(w, ctx, "\n");
            }
            emitted++;
            if (remaining <= 0 && emitted == 1) truncated = 1;
        }
        jash_writef(w, ctx, "\033[90m# %d files shown%s\033[0m\n",
                    emitted, truncated ? " (truncated — raise JASH_CONTEXT_BUDGET)" : "");
    }
    return 0;
}

/* ── bundle: where-am-I (intent + git + recent commands) ─────────────────── */

static void ctx_cat_cmd(const char* shell_cmd, const char* header,
                        jash_writer_t w, void* ctx) {
    FILE* p = popen(shell_cmd, "r");
    if (!p) return;
    char buf[1024]; int any = 0;
    while (fgets(buf, sizeof(buf), p)) {
        if (!any) { jash_writef(w, ctx, "\033[1m%s\033[0m\n", header); any = 1; }
        jash_writes(w, ctx, buf);
    }
    pclose(p);
    if (any) jash_writes(w, ctx, "\n");
}

/* The path is environment data. Read recent session metadata through an
 * owner-selected path with no shell interpolation, symlink following or
 * unbounded allocation. */
static void ctx_recent_commands(const char* home, jash_writer_t w, void* ctx) {
    if (!home || !*home) return;
    char path[1024];
    int pn = snprintf(path, sizeof(path), "%s/.jash/session_meta.jsonl", home);
    if (pn < 0 || (size_t)pn >= sizeof(path)) return;
    int fd = open(path, O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) return;
    struct stat before, after;
    if (fstat(fd, &before) < 0 || !S_ISREG(before.st_mode) || before.st_size < 0 ||
        (uint64_t)before.st_size > 1024u * 1024u) {
        close(fd);
        return;
    }
    size_t size = (size_t)before.st_size;
    char* data = malloc(size + 1);
    if (!data) { close(fd); return; }
    size_t used = 0;
    while (used < size) {
        ssize_t n = read(fd, data + used, size - used);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        used += (size_t)n;
    }
    int stable = used == size && fstat(fd, &after) == 0 &&
        before.st_dev == after.st_dev && before.st_ino == after.st_ino &&
        before.st_size == after.st_size;
    if (close(fd) < 0) stable = 0;
    if (!stable || memchr(data, '\0', used)) { free(data); return; }
    data[used] = '\0';

    size_t starts[10], lines = 0;
    if (used) starts[lines++] = 0;
    for (size_t i = 0; i < used; i++) {
        if (data[i] == '\n' && i + 1 < used) {
            if (lines < 10) starts[lines++] = i + 1;
            else {
                memmove(starts, starts + 1, 9 * sizeof(starts[0]));
                starts[9] = i + 1;
            }
        }
    }
    if (lines) {
        size_t start = starts[0];
        jash_writes(w, ctx, "\033[1mrecent commands (session_meta):\033[0m\n");
        jash_writef(w, ctx, "%.*s", (int)(used - start), data + start);
        if (data[used - 1] != '\n') jash_writes(w, ctx, "\n");
        jash_writes(w, ctx, "\n");
    }
    free(data);
}

static int ctx_cmd_bundle(jash_writer_t w, void* ctx) {
    jash_writes(w, ctx, "\033[1;36m# context bundle\033[0m\n");

    /* intent */
    const char* intent = getenv("JASH_INTENT");
    char ibuf[512] = "";
    if (!intent || !intent[0]) {
        const char* home = getenv("HOME");
        if (home) {
            char ip[600]; snprintf(ip, sizeof(ip), "%s/.jash/intent", home);
            FILE* fi = fopen(ip, "r");
            if (fi) { if (fgets(ibuf, sizeof(ibuf), fi)) intent = ibuf; fclose(fi); }
        }
    }
    if (intent && intent[0]) jash_writef(w, ctx, "\033[1mintent:\033[0m %s\n\n", intent);

    /* git */
    ctx_cat_cmd("git status --short 2>/dev/null | head -30", "git status:", w, ctx);
    ctx_cat_cmd("git diff --stat 2>/dev/null | tail -20", "git diff --stat:", w, ctx);

    /* recent commands from session_meta */
    const char* home = getenv("HOME");
    ctx_recent_commands(home, w, ctx);
    return 0;
}

/* ── dispatch ────────────────────────────────────────────────────────────── */

#include "context_native.inc"

int jash_b_context(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int native = semantic_dispatch(s, w, ctx, "context", "jash.context.v1", args, cn_provider);
    if (native >= 0) return native;
    (void)s;
    const char* a = args ? args : "";
    while (*a == ' ') a++;

    jash_budget_wrap_t bw;
    jash_budget_wrap_init(&bw, w, ctx, "context");
    jash_writer_t bw_w = jash_budget_wrap_writer(&bw);
    int rc = 0;

    if (!strncmp(a, "map", 3) && (a[3]==' ' || a[3]=='\0')) {
        const char* arg = a + 3; while (*arg == ' ') arg++;
        rc = ctx_cmd_map(arg, bw_w, &bw);
    } else if (!strncmp(a, "fold", 4) && (a[4]==' ' || a[4]=='\0')) {
        const char* arg = a + 4; while (*arg == ' ') arg++;
        rc = ctx_cmd_fold(arg, bw_w, &bw);
    } else if (!strncmp(a, "ask", 3) && (a[3]==' ' || a[3]=='\0')) {
        const char* arg = a + 3; while (*arg == ' ') arg++;
        rc = ctx_cmd_ask(arg, bw_w, &bw);
    } else if (!strcmp(a, "help") || !strcmp(a, "-h")) {
        jash_writes(bw_w, &bw,
            "context                 — where-am-I bundle (intent + git + recent cmds)\n"
            "context map [dir]       — repo digest: tree + function signatures\n"
            "context fold FILE       — signatures-only skeleton (bodies collapsed)\n"
            "context ask QUERY [--json] — query-driven ranked code digest (ADR-0027)\n");
    } else {
        rc = ctx_cmd_bundle(bw_w, &bw);
    }

    jash_budget_wrap_finish(&bw, w, ctx);
    return rc;
}
