// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: builtins/debug — parsing and execution debugging tools
//
// Features:
//   debug --parse 'if [[ -n "$x" ]]; then'  — show parser internals
//   debug --locals            — show all local variables in current scope
//   debug --profile script.sh — line-by-line execution profiling

#include "builtin.h"
#include "debug_tokens.h"
#include <core/state.h>
#include <core/env.h>
#include <theme/io.h>
#include <sys/syscalls.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <sys/time.h>
#include <parser/eval.h>

typedef struct { jash_writer_t writer; void* context; } debug_text_tokens_t;
static int debug_text_token(void* context, const char* text, size_t length, size_t offset) {
    (void)offset;
    debug_text_tokens_t* output = context;
    jash_writef(output->writer, output->context, "'%.*s'%s", (int)length, text, text[length] ? " " : "");
    return 0;
}

/* Debug parsing internals: presentation of the shared legacy scanner. */
static int debug_parse(jash_state_t* s, jash_writer_t w, void* ctx, const char* line) {
    (void)s;
    if (!line) {
        jash_writef(jash_stderr_writer, 0, "debug: --parse requires argument\n");
        return 1;
    }
    jash_writef(w, ctx, "debug: parsing: %s\n", line);
    jash_writef(w, ctx, "  tokens: ");
    debug_text_tokens_t output = {w,ctx};
    (void)debug_scan_tokens(line, debug_text_token, &output, NULL, NULL);
    jash_writes(w, ctx, "\n");
    return 0;
}

/* Debug local variables */
static int debug_locals(jash_state_t* s, jash_writer_t w, void* ctx) {
    const char* local_vars = jash_env_get(s, "__LOCAL_VARS__");
    if (!local_vars || !*local_vars) {
        jash_writes(w, ctx, "locals: no local variables in current scope\n");
        return 0;
    }
    
    jash_writes(w, ctx, "locals:\n");
    
    char local_copy[1024];
    strlcpy(local_copy, local_vars, sizeof(local_copy));
    
    char* token = strtok(local_copy, " \t");
    while (token) {
        const char* val = jash_env_get(s, token);
        if (val) {
            jash_writef(w, ctx, "  %s=%s\n", token, val);
        } else {
            jash_writef(w, ctx, "  %s=<unset>\n", token);
        }
        token = strtok(NULL, " \t");
    }
    return 0;
}

/* Check if p points to keyword kw at a word boundary */
static int at_kw(const char* p, const char* kw) {
    size_t klen = strlen(kw);
    if (strncmp(p, kw, klen) != 0) return 0;
    char after = p[klen];
    return after == '\0' || after == ' ' || after == '\t' || after == ';' || after == '\n';
}

/* Scan trimmed line for a block-opening keyword at word boundaries.
 * Only the FIRST word of the line is checked — openers in the middle
 * of a line (e.g. "echo inside if") are NOT matched. */
static const char* find_open_kw(const char* line, const char** close_out) {
    int in_sq = 0, in_dq = 0;
    const char* p = line;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return NULL;
    if (*p == '\'' && !in_dq) { in_sq = !in_sq; }
    if (*p == '"' && !in_sq)  { in_dq = !in_dq; }
    if (in_sq || in_dq) return NULL;
    
    if (at_kw(p, "if"))    { *close_out = "fi";   return p; }
    if (at_kw(p, "for"))   { *close_out = "done"; return p; }
    if (at_kw(p, "while")) { *close_out = "done"; return p; }
    if (at_kw(p, "until")) { *close_out = "done"; return p; }
    if (at_kw(p, "case"))  { *close_out = "esac"; return p; }
    return NULL;
}

/* Apply a single line's keywords to an accumulated depth counter.
 * Openers (if/for/while/until/case) only match at the FIRST word to
 * avoid false positives like "if" in "echo inside if".
 * Closers (fi/done/esac) match anywhere so single-line
 * "if true; then echo hi; fi" closes correctly. */
static void apply_line_depth(const char* trimmed, int* depth) {
    int first = 1;
    int in_sq = 0, in_dq = 0;
    const char* scan = trimmed;
    while (*scan) {
        while (*scan == ' ' || *scan == '\t') { scan++; }
        if (!*scan) break;
        if (*scan == '\'' && !in_dq) { in_sq = !in_sq; scan++; first = 0; continue; }
        if (*scan == '"' && !in_sq)  { in_dq = !in_dq; scan++; first = 0; continue; }
        if (in_sq || in_dq) { scan++; first = 0; continue; }
        
        if (first) {
            if (at_kw(scan, "if"))    { (*depth)++; scan += 2; first = 0; continue; }
            if (at_kw(scan, "for"))   { (*depth)++; scan += 3; first = 0; continue; }
            if (at_kw(scan, "while")) { (*depth)++; scan += 5; first = 0; continue; }
            if (at_kw(scan, "until")) { (*depth)++; scan += 5; first = 0; continue; }
            if (at_kw(scan, "case"))  { (*depth)++; scan += 4; first = 0; continue; }
        }
        if (at_kw(scan, "fi"))   { if (*depth > 0) (*depth)--; scan += 2; first = 0; continue; }
        if (at_kw(scan, "done")) { if (*depth > 0) (*depth)--; scan += 4; first = 0; continue; }
        if (at_kw(scan, "esac")) { if (*depth > 0) (*depth)--; scan += 4; first = 0; continue; }
        first = 0;
        while (*scan && *scan != ' ' && *scan != '\t') scan++;
    }
}

/* Debug profiling with block accumulation */
static int debug_profile(jash_state_t* s, jash_writer_t w, void* ctx, const char* script_path) {
    if (!script_path) {
        jash_writef(jash_stderr_writer, 0, "debug: --profile requires script path\n");
        return 1;
    }
    
    FILE* fp = fopen(script_path, "r");
    if (!fp) {
        jash_writef(jash_stderr_writer, 0, "debug: cannot open: %s\n", script_path);
        return 1;
    }
    
    char script_content[65536];
    size_t sz = fread(script_content, 1, sizeof(script_content) - 1, fp);
    fclose(fp);
    script_content[sz] = '\0';
    
    jash_writef(w, ctx, "Profiling: %s\n", script_path);
    jash_writes(w, ctx, "Lines\tTime(ms)\tStatement\n");
    
    const char* rp = script_content;
    int line_num = 1;
    
    while (*rp) {
        char line_buf[4096];
        unsigned si = 0;
        const char* ln = rp;
        while (*ln && *ln != '\n' && si < sizeof(line_buf) - 1) {
            line_buf[si++] = *ln++;
        }
        line_buf[si] = '\0';
        
        const char* trimmed = line_buf;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
        
        const char* close_kw = NULL;
        find_open_kw(trimmed, &close_kw);
        
        if (close_kw) {
            char block_buf[65536];
            unsigned bpos = 0;
            int depth = 0;
            int start_line = line_num;
            
            do {
                apply_line_depth(trimmed, &depth);
                
                /* Append line to block buffer */
                size_t llen = strlen(line_buf);
                if (bpos + llen + 2 > sizeof(block_buf)) break;
                if (bpos > 0) block_buf[bpos++] = '\n';
                memcpy(block_buf + bpos, line_buf, llen);
                bpos += llen;
                block_buf[bpos] = '\0';
                if (depth <= 0) break;
                
                /* Read next line */
                if (*ln == '\n') { ln++; line_num++; }
                rp = ln;
                si = 0;
                while (*ln && *ln != '\n' && si < sizeof(line_buf) - 1) {
                    line_buf[si++] = *ln++;
                }
                line_buf[si] = '\0';
                trimmed = line_buf;
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                if (!*line_buf && !*ln) break;
            } while (1);
            
            struct timespec st = {0}, en = {0};
            int have_start = clock_gettime(CLOCK_MONOTONIC, &st) == 0;
            (void)jash_eval_line(s, w, ctx, block_buf);
            int have_end = clock_gettime(CLOCK_MONOTONIC, &en) == 0;
            double elapsed_ms = have_start && have_end ?
                (en.tv_sec - st.tv_sec) * 1000.0 +
                (en.tv_nsec - st.tv_nsec) / 1000000.0 : 0.0;
            
            if (line_num > start_line)
                jash_writef(w, ctx, "%d-%d\t%.2f\t\t%s ...\n", start_line, line_num, elapsed_ms, block_buf);
            else
                jash_writef(w, ctx, "%d\t%.2f\t\t%s\n", start_line, elapsed_ms, block_buf);
            
            if (*ln == '\n') { ln++; line_num++; }
            rp = ln;
        } else {
            struct timespec st = {0}, en = {0};
            int have_start = clock_gettime(CLOCK_MONOTONIC, &st) == 0;
            (void)jash_eval_line(s, w, ctx, line_buf);
            int have_end = clock_gettime(CLOCK_MONOTONIC, &en) == 0;
            double elapsed_ms = have_start && have_end ?
                (en.tv_sec - st.tv_sec) * 1000.0 +
                (en.tv_nsec - st.tv_nsec) / 1000000.0 : 0.0;
            
            jash_writef(w, ctx, "%d\t%.2f\t\t%s\n", line_num, elapsed_ms, line_buf);
            
            if (*ln == '\n') { ln++; line_num++; }
            rp = ln;
        }
    }
    
    return 0;
}

// ── Breakpoint list ──────────────────────────────────────────────────────────
#define MAX_BREAKPOINTS 64

typedef struct {
    int lines[MAX_BREAKPOINTS];
    int count;
} breakpoints_t;

static int bp_has(breakpoints_t* bp, int line) {
    for (int i = 0; i < bp->count; i++)
        if (bp->lines[i] == line) return 1;
    return 0;
}

static void bp_add(breakpoints_t* bp, int line) {
    if (bp->count >= MAX_BREAKPOINTS || bp_has(bp, line)) return;
    bp->lines[bp->count++] = line;
}

static void bp_clear(breakpoints_t* bp) {
    bp->count = 0;
}

/* Debug trace: step-by-step execution with breakpoints */
static int debug_trace(jash_state_t* s, jash_writer_t w, void* ctx, const char* script_path) {
    if (!script_path) {
        jash_writef(jash_stderr_writer, 0, "debug: --trace requires script path\n");
        return 1;
    }

    FILE* fp = fopen(script_path, "r");
    if (!fp) {
        jash_writef(jash_stderr_writer, 0, "debug: cannot open: %s\n", script_path);
        return 1;
    }

    char script_content[65536];
    size_t sz = fread(script_content, 1, sizeof(script_content) - 1, fp);
    fclose(fp);
    script_content[sz] = '\0';

    jash_writef(w, ctx, "Tracing: %s\n", script_path);
    jash_writes(w, ctx, "Commands: n(next) c(continue) q(quit) p<var> b<line> l(list) h(help)\n\n");

    breakpoints_t breakpoints;
    bp_clear(&breakpoints);

    const char* rp = script_content;
    int line_num = 1;
    int stepping = 1;

    while (*rp) {
        /* Read one line */
        char line_buf[4096];
        unsigned si = 0;
        const char* ln = rp;
        while (*ln && *ln != '\n' && si < sizeof(line_buf) - 1) {
            line_buf[si++] = *ln++;
        }
        line_buf[si] = '\0';

        const char* trimmed = line_buf;
        while (*trimmed == ' ' || *trimmed == '\t') trimmed++;

        /* Skip empty/comment lines silently */
        if (!*trimmed || *trimmed == '#') {
            if (*ln == '\n') { ln++; line_num++; }
            rp = ln;
            continue;
        }

        const char* close_kw = NULL;
        find_open_kw(trimmed, &close_kw);

        if (close_kw) {
            /* Accumulate block */
            char block_buf[65536];
            unsigned bpos = 0;
            int depth = 0;
            int start_line = line_num;

            do {
                apply_line_depth(trimmed, &depth);

                size_t llen = strlen(line_buf);
                if (bpos + llen + 2 > sizeof(block_buf)) break;
                if (bpos > 0) block_buf[bpos++] = '\n';
                memcpy(block_buf + bpos, line_buf, llen);
                bpos += llen;
                block_buf[bpos] = '\0';
                if (depth <= 0) break;

                if (*ln == '\n') { ln++; line_num++; }
                rp = ln;
                si = 0;
                while (*ln && *ln != '\n' && si < sizeof(line_buf) - 1) {
                    line_buf[si++] = *ln++;
                }
                line_buf[si] = '\0';
                trimmed = line_buf;
                while (*trimmed == ' ' || *trimmed == '\t') trimmed++;
                if (!*line_buf && !*ln) break;
            } while (1);

            if (!stepping && !bp_has(&breakpoints, line_num)) {
                /* Running without stepping — just execute */
                (void)jash_eval_line(s, w, ctx, block_buf);
                if (*ln == '\n') { ln++; line_num++; }
                rp = ln;
                continue;
            }

            /* Show block */
            if (line_num > start_line)
                jash_writef(w, ctx, "\033[1;36m[%d-%d]\033[0m %s\n", start_line, line_num, block_buf);
            else
                jash_writef(w, ctx, "\033[1;36m[%d]\033[0m %s\n", start_line, block_buf);

            /* Check breakpoint before executing */
            if (bp_has(&breakpoints, start_line))
                stepping = 1;

            if (!stepping) {
                (void)jash_eval_line(s, w, ctx, block_buf);
            } else {
                /* Wait for user input */
                for (;;) {
                    jash_writes(w, ctx, "\033[1;33mtrace>\033[0m ");
                    fflush(stdout);

                    char input[128];
                    if (!fgets(input, sizeof(input), stdin)) {
                        stepping = 0;
                        break;
                    }

                    /* Strip trailing newline */
                    size_t ilen = strlen(input);
                    while (ilen > 0 && (input[ilen-1] == '\n' || input[ilen-1] == '\r'))
                        input[--ilen] = '\0';

                    if (ilen == 0) {
                        /* Enter: execute and stop at next */
                        (void)jash_eval_line(s, w, ctx, block_buf);
                        break;
                    }

                    if (strcmp(input, "n") == 0 || strcmp(input, "next") == 0) {
                        (void)jash_eval_line(s, w, ctx, block_buf);
                        break;
                    }

                    if (strcmp(input, "c") == 0 || strcmp(input, "continue") == 0) {
                        stepping = 0;
                        (void)jash_eval_line(s, w, ctx, block_buf);
                        break;
                    }

                    if (strcmp(input, "q") == 0 || strcmp(input, "quit") == 0) {
                        jash_writes(w, ctx, "Trace stopped.\n");
                        return 0;
                    }

                    if (strcmp(input, "h") == 0 || strcmp(input, "help") == 0) {
                        jash_writes(w, ctx, "Commands:\n");
                        jash_writes(w, ctx, "  Enter    execute this statement, stop at next\n");
                        jash_writes(w, ctx, "  n        next — execute this statement, stop at next\n");
                        jash_writes(w, ctx, "  c        continue — run until next breakpoint\n");
                        jash_writes(w, ctx, "  q        quit — stop tracing\n");
                        jash_writes(w, ctx, "  p VAR    print — show variable value\n");
                        jash_writes(w, ctx, "  b LINE   break — set breakpoint at line\n");
                        jash_writes(w, ctx, "  bl       breakpoints — list breakpoints\n");
                        jash_writes(w, ctx, "  bc       breakpoints clear — remove all breakpoints\n");
                        jash_writes(w, ctx, "  l        list — show lines around current position\n");
                        continue;
                    }

                    if (input[0] == 'p' && (input[1] == ' ' || input[1] == '\t')) {
                        const char* vname = input + 2;
                        while (*vname == ' ' || *vname == '\t') vname++;
                        const char* val = jash_env_get(s, vname);
                        if (val)
                            jash_writef(w, ctx, "  %s=%s\n", vname, val);
                        else
                            jash_writef(w, ctx, "  %s=<unset>\n", vname);
                        continue;
                    }

                    if (input[0] == 'b') {
                        if (input[1] == 'l') {
                            if (breakpoints.count == 0) {
                                jash_writes(w, ctx, "  no breakpoints set\n");
                            } else {
                                jash_writes(w, ctx, "  breakpoints:");
                                for (int bi = 0; bi < breakpoints.count; bi++)
                                    jash_writef(w, ctx, " %d", breakpoints.lines[bi]);
                                jash_writes(w, ctx, "\n");
                            }
                            continue;
                        }
                        if (input[1] == 'c') {
                            bp_clear(&breakpoints);
                            jash_writes(w, ctx, "  all breakpoints cleared\n");
                            continue;
                        }
                        int bline = 0;
                        if (sscanf(input, "b %d", &bline) == 1 && bline > 0) {
                            if (bp_has(&breakpoints, bline)) {
                                jash_writef(w, ctx, "  breakpoint already set at line %d\n", bline);
                            } else {
                                bp_add(&breakpoints, bline);
                                jash_writef(w, ctx, "  breakpoint set at line %d\n", bline);
                            }
                            continue;
                        }
                    }

                    if (strcmp(input, "l") == 0 || input[0] == 'l') {
                        int context = 5;
                        int show_start = (start_line > context) ? start_line - context : 1;
                        for (int li = show_start; li <= line_num; li++) {
                            /* Re-read from script content */
                            const char* sp = script_content;
                            int cl = 1;
                            while (sp && cl < li) {
                                sp = strchr(sp, '\n');
                                if (!sp) break;
                                sp++;
                                cl++;
                            }
                            if (sp) {
                                char tmp[256];
                                int ti = 0;
                                while (*sp && *sp != '\n' && ti < 255) tmp[ti++] = *sp++;
                                tmp[ti] = '\0';
                                if (li == start_line)
                                    jash_writef(w, ctx, "\033[1;32m→ %4d\033[0m %s\n", li, tmp);
                                else
                                    jash_writef(w, ctx, "  %4d  %s\n", li, tmp);
                            }
                        }
                        continue;
                    }

                    jash_writef(w, ctx, "unknown command. type 'h' for help\n");
                }
            }

            if (*ln == '\n') { ln++; line_num++; }
            rp = ln;

        } else {
            /* Single line */
            if (!stepping && !bp_has(&breakpoints, line_num)) {
                (void)jash_eval_line(s, w, ctx, line_buf);
                if (*ln == '\n') { ln++; line_num++; }
                rp = ln;
                continue;
            }

            jash_writef(w, ctx, "\033[1;36m[%d]\033[0m %s\n", line_num, line_buf);

            if (bp_has(&breakpoints, line_num))
                stepping = 1;

            if (!stepping) {
                (void)jash_eval_line(s, w, ctx, line_buf);
            } else {
                for (;;) {
                    jash_writes(w, ctx, "\033[1;33mtrace>\033[0m ");
                    fflush(stdout);

                    char input[128];
                    if (!fgets(input, sizeof(input), stdin)) {
                        stepping = 0;
                        break;
                    }

                    size_t ilen = strlen(input);
                    while (ilen > 0 && (input[ilen-1] == '\n' || input[ilen-1] == '\r'))
                        input[--ilen] = '\0';

                    if (ilen == 0) {
                        (void)jash_eval_line(s, w, ctx, line_buf);
                        break;
                    }

                    if (strcmp(input, "n") == 0 || strcmp(input, "next") == 0) {
                        (void)jash_eval_line(s, w, ctx, line_buf);
                        break;
                    }

                    if (strcmp(input, "c") == 0 || strcmp(input, "continue") == 0) {
                        stepping = 0;
                        (void)jash_eval_line(s, w, ctx, line_buf);
                        break;
                    }

                    if (strcmp(input, "q") == 0 || strcmp(input, "quit") == 0) {
                        jash_writes(w, ctx, "Trace stopped.\n");
                        return 0;
                    }

                    if (strcmp(input, "h") == 0 || strcmp(input, "help") == 0) {
                        jash_writes(w, ctx, "Commands:\n");
                        jash_writes(w, ctx, "  Enter    execute this statement, stop at next\n");
                        jash_writes(w, ctx, "  n        next — execute this statement, stop at next\n");
                        jash_writes(w, ctx, "  c        continue — run until next breakpoint\n");
                        jash_writes(w, ctx, "  q        quit — stop tracing\n");
                        jash_writes(w, ctx, "  p VAR    print — show variable value\n");
                        jash_writes(w, ctx, "  b LINE   break — set breakpoint at line\n");
                        jash_writes(w, ctx, "  bl       breakpoints — list breakpoints\n");
                        jash_writes(w, ctx, "  bc       breakpoints clear — remove all breakpoints\n");
                        jash_writes(w, ctx, "  l        list — show lines around current position\n");
                        continue;
                    }

                    if (input[0] == 'p' && (input[1] == ' ' || input[1] == '\t')) {
                        const char* vname = input + 2;
                        while (*vname == ' ' || *vname == '\t') vname++;
                        const char* val = jash_env_get(s, vname);
                        if (val)
                            jash_writef(w, ctx, "  %s=%s\n", vname, val);
                        else
                            jash_writef(w, ctx, "  %s=<unset>\n", vname);
                        continue;
                    }

                    if (input[0] == 'b') {
                        if (input[1] == 'l') {
                            if (breakpoints.count == 0) {
                                jash_writes(w, ctx, "  no breakpoints set\n");
                            } else {
                                jash_writes(w, ctx, "  breakpoints:");
                                for (int bi = 0; bi < breakpoints.count; bi++)
                                    jash_writef(w, ctx, " %d", breakpoints.lines[bi]);
                                jash_writes(w, ctx, "\n");
                            }
                            continue;
                        }
                        if (input[1] == 'c') {
                            bp_clear(&breakpoints);
                            jash_writes(w, ctx, "  all breakpoints cleared\n");
                            continue;
                        }
                        int bline = 0;
                        if (sscanf(input, "b %d", &bline) == 1 && bline > 0) {
                            if (bp_has(&breakpoints, bline)) {
                                jash_writef(w, ctx, "  breakpoint already set at line %d\n", bline);
                            } else {
                                bp_add(&breakpoints, bline);
                                jash_writef(w, ctx, "  breakpoint set at line %d\n", bline);
                            }
                            continue;
                        }
                    }

                    if (strcmp(input, "l") == 0 || input[0] == 'l') {
                        int context = 5;
                        int show_start = (line_num > context) ? line_num - context : 1;
                        for (int li = show_start; li <= line_num; li++) {
                            const char* sp = script_content;
                            int cl = 1;
                            while (sp && cl < li) {
                                sp = strchr(sp, '\n');
                                if (!sp) break;
                                sp++;
                                cl++;
                            }
                            if (sp) {
                                char tmp[256];
                                int ti = 0;
                                while (*sp && *sp != '\n' && ti < 255) tmp[ti++] = *sp++;
                                tmp[ti] = '\0';
                                if (li == line_num)
                                    jash_writef(w, ctx, "\033[1;32m→ %4d\033[0m %s\n", li, tmp);
                                else
                                    jash_writef(w, ctx, "  %4d  %s\n", li, tmp);
                            }
                        }
                        continue;
                    }

                    jash_writef(w, ctx, "unknown command. type 'h' for help\n");
                }
            }

            if (*ln == '\n') { ln++; line_num++; }
            rp = ln;
        }
    }

    jash_writes(w, ctx, "Trace complete.\n");
    return 0;
}

/* Main debug builtin */
int jash_b_debug(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    if (!args || !*args) {
        jash_writes(jash_stderr_writer, 0, "usage: debug --parse 'line' | --locals | --profile script.sh | --trace script.sh\n");
        return 1;
    }
    
    /* find which flag is first, then use raw remainder */
    const char* flag = args;
    while (*flag == ' ' || *flag == '\t') flag++;
    if (*flag == '-') {
        const char* sp = flag;
        while (*sp && *sp != ' ' && *sp != '\t') sp++;
        size_t flen = (size_t)(sp - flag);
        if (flen == 7 && memcmp(flag, "--parse", 7) == 0) {
            while (*sp == ' ' || *sp == '\t') sp++;
            if (*sp) return debug_parse(s, w, ctx, sp);
            jash_writes(jash_stderr_writer, 0, "debug: --parse requires argument\n");
            return 1;
        } else if (flen == 8 && memcmp(flag, "--locals", 8) == 0) {
            return debug_locals(s, w, ctx);
        } else if (flen == 9 && memcmp(flag, "--profile", 9) == 0) {
            while (*sp == ' ' || *sp == '\t') sp++;
            if (*sp) return debug_profile(s, w, ctx, sp);
            jash_writes(jash_stderr_writer, 0, "debug: --profile requires filename\n");
            return 1;
        } else if (flen == 7 && memcmp(flag, "--trace", 7) == 0) {
            while (*sp == ' ' || *sp == '\t') sp++;
            if (*sp) return debug_trace(s, w, ctx, sp);
            jash_writes(jash_stderr_writer, 0, "debug: --trace requires filename\n");
            return 1;
        }
    }
    
    jash_writes(jash_stderr_writer, 0, "debug: unknown option. use --parse, --locals, --profile, or --trace\n");
    return 1;
}

#include "debug_native.inc"
