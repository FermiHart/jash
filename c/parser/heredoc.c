// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/heredoc — here-document parsing (cat << EOF)
// Enhanced with bash parity: <<- indented terminator, pipe integration

#include "heredoc.h"
#include <core/readbuf.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>
#include "sys/syscalls.h"
#include "cmdsub.h"
#include "arithmetic.h"
#include "param_expand.h"
#include "quotes.h"

/* ─── Re-entrant heredoc body registry (BUG-030) ──────────────────────────
 * A heredoc/here-string body is materialized once into a temp file, but its
 * EXPANSION must run against the shell state in effect when the consuming
 * command actually executes — so a body inside a `for`/`while` loop sees the
 * per-iteration value of the loop variable. We keep the RAW body here and
 * re-render it (cmdsub -> arithmetic -> parameter expansion, single-quote
 * aware) every time its temp-file path is about to be read. The expander is
 * driven from the top of jash_eval_line, so each loop iteration — which calls
 * jash_eval_line(body) with the loop var freshly set — gets a correct render.
 *
 * `g_jash_cmdsub_no_split` lives in the command-substitution engine; we toggle
 * it for here-strings (whole-word cmdsub WITHOUT field splitting). */
extern int g_jash_cmdsub_no_split;

#ifndef JASH_HD_REG_MAX
#define JASH_HD_REG_MAX 64
#endif
typedef struct { char path[256]; char* raw; int is_herestring; } jash_hd_reg_t;
static jash_hd_reg_t g_hd_reg[JASH_HD_REG_MAX];
static int g_hd_reg_n = 0;
static int g_hd_track_lifetime = 0;
static int g_hd_may_escape = 0;

void jash_heredoc_track_lifetime(void);
int jash_heredoc_may_escape(void);
void jash_heredoc_track_lifetime(void) { g_hd_track_lifetime = 1; }
int jash_heredoc_may_escape(void) { return g_hd_may_escape; }

/* Shell source must quote the filename, not expand TMPDIR as command text.
 * A buffer of 4 * strlen(path) + 3 always suffices; 0 means insufficient room. */
size_t jash_heredoc_quote_path(const char* path, char* out, size_t cap);
size_t jash_heredoc_quote_path(const char* path, char* out, size_t cap) {
    size_t n = 0;
    if (cap < 3) return 0;
    out[n++] = '\'';
    for (; *path; path++) {
        size_t need = *path == '\'' ? 4 : 1;
        if (need > cap - n - 2) { out[0] = '\0'; return 0; }
        if (*path == '\'') {
            memcpy(out + n, "'\\''", 4);
            n += 4;
        } else {
            out[n++] = *path;
        }
    }
    out[n++] = '\'';
    out[n] = '\0';
    return n;
}

void jash_heredoc_register(const char* path, const char* raw, int is_herestring) {
    if (!path || !raw || g_hd_reg_n >= JASH_HD_REG_MAX) return;
    char* dup = strdup(raw);
    if (!dup) return;
    strlcpy(g_hd_reg[g_hd_reg_n].path, path, sizeof(g_hd_reg[g_hd_reg_n].path));
    g_hd_reg[g_hd_reg_n].raw = dup;
    g_hd_reg[g_hd_reg_n].is_herestring = is_herestring;
    g_hd_reg_n++;
}

int jash_heredoc_registry_mark(void) { return g_hd_reg_n; }

void jash_heredoc_registry_rewind(int mark) {
    if (mark < 0) mark = 0;
    while (g_hd_reg_n > mark) {
        g_hd_reg_n--;
        free(g_hd_reg[g_hd_reg_n].raw);
        g_hd_reg[g_hd_reg_n].raw = NULL;
        g_hd_reg[g_hd_reg_n].path[0] = '\0';
    }
}

/* Render `raw` the way an unquoted heredoc body / a here-string word expands:
 * command substitution, then arithmetic, then parameter expansion — each
 * respecting single quotes — and (for here-strings) a final quote removal.
 * Pure: reads `st`, writes `out`. Mirrors the historical one-shot expander. */
/* JINV-023: `trunc` (opcional) sai 1 quando a renderizacao NAO coube em
 * `out`. Sem esse sinal, o chamador reescrevia o arquivo do heredoc com
 * O_TRUNC usando um corpo CORTADO — destruindo dados que ja estavam
 * corretamente gravados. Ver o comentario em jash_heredoc_expand_line. */
static void hd_render_into(jash_state_t* st, const char* raw,
                           int is_herestring, char* out, size_t outsz,
                           int* trunc) {
    if (trunc) *trunc = 0;
    if (outsz == 0) { if (trunc) *trunc = 1; return; }
    out[0] = '\0';
    if (is_herestring) {
        const char* v = raw;
        if (jash_has_cmdsub(v)) {
            int ns = g_jash_cmdsub_no_split; g_jash_cmdsub_no_split = 1;
            const char* x = jash_expand_cmdsub_known(st, v); g_jash_cmdsub_no_split = ns;
            if (x) v = x;
        }
        if (jash_has_arithmetic(v))      { const char* x = jash_expand_arithmetic(v, st); if (x) v = x; }
        if (jash_has_param_expansion(v)) { const char* x = jash_param_expand(v, st);      if (x) v = x; }
        char hsout[8192];
        { int ul = jash_unquote(v, hsout, sizeof hsout);
          if (ul < 0) strlcpy(hsout, v, sizeof hsout); }
        jash_cmdsub_unprotect(hsout);
        jash_quote_unshield(hsout);      /* BUG-096 */
        if (trunc && strlen(hsout) >= outsz) *trunc = 1;
        strlcpy(out, hsout, outsz);
        return;
    }
    unsigned eo = 0;
    const char* src = raw;
    while (*src && eo < outsz - 1) {
        /* BUG-121d: a heredoc body is per-line for expansion, but a `$(…)` /
         * `${…}` / `` `…` `` that SPANS lines must be one unit — else
         * `sum=$(⏎ echo $((1+1))⏎)` was split and the substitution never
         * closed. (Quotes are literal in a heredoc body, so not tracked.) */
        const char* eol = src;
        {
            int pd = 0, bt = 0;
            while (*eol) {
                char c = *eol;
                if (bt) { if (c == '`') bt = 0; eol++; continue; }
                if (c == '`') { bt = 1; eol++; continue; }
                if (c == '$' && (eol[1] == '(' || eol[1] == '{')) { pd++; eol += 2; continue; }
                if (pd > 0) { if (c == '(' || c == '{') pd++; else if (c == ')' || c == '}') pd--; eol++; continue; }
                if (c == '\n') break;
                eol++;
            }
        }
        char line_tmp[1024];
        unsigned lt = 0;
        while (src < eol && lt < sizeof(line_tmp) - 1) line_tmp[lt++] = *src++;
        line_tmp[lt] = '\0';
        const char* expanded = line_tmp;
        if (jash_has_cmdsub(expanded))          { const char* x = jash_expand_cmdsub_known(st, expanded); if (x) expanded = x; }
        if (jash_has_arithmetic(expanded))      { const char* x = jash_expand_arithmetic(expanded, st); if (x) expanded = x; }
        if (jash_has_param_expansion(expanded)) { const char* x = jash_param_expand(expanded, st);      if (x) expanded = x; }
        /* Restore shielded $/` and quote sentinels BEFORE copying — the old
         * order mutated `expanded` after `out` already took the raw bytes,
         * so heredoc bodies carried sentinels into the output (BUG-096). */
        jash_cmdsub_unprotect((char*)expanded);  /* SECURITY #15 */
        jash_quote_unshield((char*)expanded);    /* BUG-096 */
        { const char* ep = expanded;
          for (; *ep && eo < outsz - 1; ) out[eo++] = *ep++;
          if (*ep && trunc) *trunc = 1; }        /* JINV-023 */
        if (*src == '\n' && eo < outsz - 1) { out[eo++] = '\n'; src++; }
    }
    if (*src && trunc) *trunc = 1;                /* JINV-023 */
    out[eo] = '\0';
}

int jash_heredoc_expand_line(jash_state_t* st, const char* line) {
    /* This hook runs before evaluation/forking, including eval, source and
     * later function calls. A sticky hint is intentionally conservative:
     * &&, function definitions, pipelines and indirection can retain files
     * even if no background reader survives. Never infer safety from ECHILD.
     * Check even with no raw bodies registered (quoted heredocs need it too). */
    if (g_hd_track_lifetime && !g_hd_may_escape && line) {
        if (strchr(line, '&') || strstr(line, "coproc")) g_hd_may_escape = 1;
        char quote = 0;
        for (const char* p = line; !g_hd_may_escape && *p;) {
            if (quote != '\'' && (*p == '$' || *p == '`')) {
                g_hd_may_escape = 1;
                break;
            }
            const char* next = jash_quote_step(p, &quote);
            if (next) { p = next; continue; }
            if (*p == '(' || *p == '|') g_hd_may_escape = 1;
            p++;
        }
    }
    if (g_hd_reg_n == 0 || !line) return 0;   /* hot no-op: nothing registered */
    int n = 0;
    for (int i = 0; i < g_hd_reg_n; i++) {
        if (!g_hd_reg[i].raw) continue;
        if (!strstr(line, g_hd_reg[i].path)) {
            char quoted[4 * sizeof(g_hd_reg[i].path) + 3];
            if (!jash_heredoc_quote_path(g_hd_reg[i].path, quoted, sizeof quoted) ||
                !strstr(line, quoted)) continue;
        }
        /* JINV-023: o buffer era `char rendered[8192]` FIXO, e o arquivo era
         * reescrito com O_TRUNC logo abaixo. Um corpo de heredoc com
         * delimitador NAO citado e mais de 8191 bytes era portanto CORTADO na
         * expansao — destruindo, em silencio, o corpo que ja estava gravado
         * inteiro. O JINV-022 elevou o teto da LEITURA; sem este, a escrita
         * de volta continuava sendo o gargalo.
         *
         * Cresce ate caber. Se nao couber (ou faltar memoria), NAO reescreve:
         * o arquivo mantem o corpo cru, que e incompleto quanto a expansao mas
         * INTEIRO quanto ao conteudo — perder a expansao e recuperavel, perder
         * o texto do usuario nao. */
        size_t rcap = strlen(g_hd_reg[i].raw) * 2 + 8192;
        char* rendered = (char*)malloc(rcap);
        int rtrunc = 1;
        while (rendered) {
            hd_render_into(st, g_hd_reg[i].raw, g_hd_reg[i].is_herestring,
                           rendered, rcap, &rtrunc);
            if (!rtrunc) break;
            if (rcap > (64u * 1024u * 1024u)) break;    /* teto de sanidade */
            { char* n2 = (char*)realloc(rendered, rcap * 2);
              if (!n2) break;
              rendered = n2; rcap *= 2; }
        }
        if (!rendered || rtrunc) {
            fprintf(stderr,
                    "jash: heredoc: corpo grande demais para expandir — "
                    "mantendo o texto cru (nada foi truncado)\n");
            free(rendered);
            continue;
        }
        int fd = open(g_hd_reg[i].path, O_WRONLY | O_TRUNC);
        if (fd < 0) { free(rendered); continue; }
        size_t L = strlen(rendered);
        ssize_t wr = write(fd, rendered, L); (void)wr;
        close(fd);
        free(rendered);
        n++;
    }
    return n;
}

/* Internal: Check if a line is only whitespace (tabs/spaces) */
static int is_whitespace_only(const char* line) {
    if (!line) return 0;
    while (*line) {
        if (*line != ' ' && *line != '\t') return 0;
        line++;
    }
    return 1;
}

/* Internal: Strip leading tabs/spaces from a line (for <<- mode).
 * Returns pointer into original string at first non-whitespace char,
 * or points to '\0' if line is all whitespace. */
static const char* strip_leading_whitespace(const char* line) {
    if (!line) return line;
    while (*line && (*line == ' ' || *line == '\t')) {
        line++;
    }
    return line;
}

/* Internal: Count leading whitespace (tabs/spaces) in a line */
static int count_leading_whitespace(const char* line) {
    int count = 0;
    if (!line) return 0;
    while (*line && (*line == ' ' || *line == '\t')) {
        count++;
        line++;
    }
    return count;
}

/* Internal: Find the minimum indentation of all non-empty content lines */
static int find_min_indent(const char* content, int content_len) {
    int min_indent = INT_MAX;
    int line_start = 0;
    
    for (int i = 0; i <= content_len; i++) {
        if (i == content_len || content[i] == '\n') {
            /* Found end of line */
            int line_len = i - line_start;
            if (line_len > 0) {
                const char* line = content + line_start;
                /* Skip completely empty lines */
                if (!is_whitespace_only(line)) {
                    int indent = count_leading_whitespace(line);
                    if (indent < min_indent) {
                        min_indent = indent;
                    }
                }
            }
            line_start = i + 1;
        }
    }
    
    return (min_indent == INT_MAX) ? 0 : min_indent;
}

/* Internal: Strip common indentation from heredoc content (for <<- mode).
 * Allocates new string with leading tabs/spaces removed from each line.
 * Caller must free result. */
static char* strip_common_indent(const char* content) {
    if (!content) return NULL;
    
    int content_len = strlen(content);
    int min_indent = find_min_indent(content, content_len);
    
    if (min_indent == 0) {
        /* No common indentation; return copy of original */
        char* result = malloc(content_len + 1);
        if (!result) return NULL;
        strlcpy(result, content, content_len + 1);
        return result;
    }
    
    /* Build new string, skipping min_indent chars from start of each line */
    char* result = malloc(content_len + 1);
    if (!result) return NULL;
    
    int out_pos = 0;
    int line_start = 0;
    
    for (int i = 0; i <= content_len; i++) {
        if (i == content_len || content[i] == '\n') {
            int line_len = i - line_start;
            const char* line = content + line_start;
            
            if (line_len > 0) {
                /* Skip leading whitespace up to min_indent */
                int skip = 0;
                int ws_count = 0;
                while (skip < line_len && ws_count < min_indent &&
                       (line[skip] == ' ' || line[skip] == '\t')) {
                    ws_count++;
                    skip++;
                }
                
                /* Copy remainder of line */
                int copy_len = line_len - skip;
                if (copy_len > 0) {
                    memcpy(result + out_pos, line + skip, copy_len);
                    out_pos += copy_len;
                }
            }
            
            /* Add newline (except for last line if no trailing newline) */
            if (i < content_len) {
                result[out_pos++] = '\n';
            }
            
            line_start = i + 1;
        }
    }
    
    result[out_pos] = '\0';
    return result;
}

/* Parse and collect here-document content until delimiter found.
 * Returns: allocated string with heredoc content, or NULL on error.
 * Caller must free the result.
 * 
 * Supports:
 * - <<EOF: standard heredoc (no processing)
 * - <<-EOF: indented terminator mode (leading tabs/spaces stripped from content)
 * - Proper delimiter matching (line must start with delimiter, optionally with whitespace in <<- mode)
 */
char* jash_parse_heredoc(const char* delimiter) {
    return jash_parse_heredoc_ex(delimiter, 0);  /* default to << mode */
}

/* Extended heredoc parser with options.
 * mode: 0 = << (standard), 1 = <<- (strip indent)
 * Returns: allocated string with heredoc content, or NULL on error.
 * Caller must free result. */
char* jash_parse_heredoc_ex(const char* delimiter, int mode) {
    if (!delimiter || !*delimiter) return NULL;
    
    /* Dynamic buffer to handle large heredocs */
    size_t capacity = 4096;
    char* buffer = malloc(capacity);
    if (!buffer) return NULL;
    
    size_t pos = 0;
    char line[4096];
    
    /* Use buffered reads instead of byte-by-byte to avoid syscall overhead. */
    jash_readbuf_t rb;
    jash_readbuf_init(&rb, STDIN_FILENO);

    for (;;) {
        size_t line_len = 0;
        int has_newline = 0;
        int rc;
        char c;
        
        /* Read one line using buffered I/O */
        while (line_len < sizeof(line) - 1) {
            rc = jash_readbuf_getc(&rb, &c);
            if (rc <= 0) break;  /* EOF or error */
            if (c == '\n') {
                has_newline = 1;
                break;
            }
            line[line_len++] = c;
        }
        if (rc <= 0 && line_len == 0) break;  /* EOF */
        line[line_len] = '\0';
        
        /* Check for end delimiter */
        int is_delimiter = 0;
        
        if (mode == 0) {
            /* << mode: exact match, no leading whitespace allowed */
            if (strcmp(line, delimiter) == 0) {
                is_delimiter = 1;
            }
        } else if (mode == 1) {
            /* <<- mode: may have leading whitespace before delimiter */
            const char* stripped = strip_leading_whitespace(line);
            if (strcmp(stripped, delimiter) == 0) {
                is_delimiter = 1;
            }
        }
        
        if (is_delimiter) {
            /* Found end delimiter */
            if (pos < capacity) buffer[pos] = '\0';
            
            /* In <<- mode, strip common indentation from content */
            if (mode == 1) {
                char* stripped = strip_common_indent(buffer);
                free(buffer);
                return stripped;
            }
            
            return buffer;
        }
        
        /* Append line to buffer (with newline if original had one) */
        size_t needed = line_len + (has_newline ? 1 : 0) + 1;
        
        /* Resize buffer if needed */
        while (pos + needed > capacity) {
            capacity *= 2;
            char* new_buf = realloc(buffer, capacity);
            if (!new_buf) {
                free(buffer);
                return NULL;  /* Memory error */
            }
            buffer = new_buf;
        }
        
        /* Copy line content */
        strlcpy(buffer + pos, line, capacity - pos);
        pos += line_len;
        
        /* Add newline back if original had it */
        if (has_newline) {
            buffer[pos++] = '\n';
        }
    }
    
    /* EOF without delimiter: error condition */
    free(buffer);
    return NULL;
}

/* JINV-092 — contrato completo em heredoc.h. Uma varredura só, para os dois
 * lados do heredoc (materializar o corpo e blindá-lo da expansão). */
const char* jash_heredoc_delim(const char* p, char* out, size_t cap, int* quoted) {
    unsigned di = 0;
    int q_any = 0;
    char q = 0;
    if (quoted) *quoted = 0;
    if (!p || !out || cap == 0) return p;
    while (*p && di + 1 < cap) {
        char c = *p;
        if (q) {                       /* dentro de '…' ou "…" */
            if (c == q) { q = 0; p++; continue; }
            /* Dentro de aspas DUPLAS a barra ainda escapa `\` `"` `$` `` ` ``;
             * dentro de simples não escapa nada (POSIX 2.2.2). */
            if (c == '\\' && q == '"' && p[1] &&
                (p[1] == '\\' || p[1] == '"' || p[1] == '$' || p[1] == '`')) {
                p++; out[di++] = *p++; continue;
            }
            out[di++] = c; p++; continue;
        }
        if (c == '\'' || c == '"') { q = c; q_any = 1; p++; continue; }
        if (c == '\\' && p[1]) { q_any = 1; p++; out[di++] = *p++; continue; }
        /* Fim da PALAVRA: branco ou metacaractere NU. Citado não termina. */
        if (c == ' ' || c == '\t' || c == '\n' ||
            c == ';' || c == '&' || c == '|' ||
            c == '(' || c == ')' || c == '<' || c == '>')
            break;
        out[di++] = c; p++;
    }
    out[di] = '\0';
    if (quoted) *quoted = q_any;
    return p;
}

/* Check if a token starts a here-document (contains <<) */
/* Protect heredoc bodies with quoted delimiters from expansion.
 * When a heredoc delimiter is quoted (<<'EOF' or <<"EOF"), the body
 * should NOT undergo parameter/arithmetic expansion. This function
 * replaces $ and ` with sentinels (0x01/0x02) in the body so later
 * expansion passes ignore them. jash_cmdsub_unprotect() restores them. */
void jash_protect_quoted_heredocs(char* line) {
    if (!line) return;
    char* p = line;
    while (*p) {
        /* Find << */
        char* hd = strstr(p, "<<");
        if (!hd) break;
        /* Skip <<< and <<( */
        if (hd[2] == '<' || hd[2] == '(') { p = hd + 2; continue; }
        char* r = hd + 2;
        int indented = 0;
        if (*r == '-') { indented = 1; r++; }
        /* JINV-092: a citação pode ser de QUALQUER parte da palavra e por
         * QUALQUER das três formas — `<<\EOF` conta tanto quanto `<<'EOF'`.
         * A leitura mora em jash_heredoc_delim (contrato em heredoc.h); aqui
         * havia uma segunda, que só enxergava `'…'`/`"…"` inteiriças e por
         * isso deixava o corpo de `<<\EOF` ser expandido. */
        char delim[256];
        int quoted = 0;
        r = (char*)jash_heredoc_delim(r, delim, sizeof(delim), &quoted);
        if (!quoted) { p = hd + 2; continue; }
        if (delim[0] == '\0') { p = hd + 2; continue; }
        /* Skip to newline after delimiter */
        while (*r && *r != '\n') r++;
        if (*r == '\n') r++;
        /* Now r points to heredoc body start */
        /* Scan body lines and protect $ and ` */
        while (*r) {
            char* line_start = r;
            /* Read one line */
            while (*r && *r != '\n') r++;
            size_t line_len = (size_t)(r - line_start);
            /* Check if this line is the delimiter */
            char linebuf[512];
            size_t check_len = line_len < sizeof(linebuf) - 1 ? line_len : sizeof(linebuf) - 1;
            memcpy(linebuf, line_start, check_len);
            linebuf[check_len] = '\0';
            const char* check = linebuf;
            if (indented) {
                while (*check == ' ' || *check == '\t') check++;
            }
            if (strcmp(check, delim) == 0) {
                /* Found closing delimiter */
                if (*r == '\n') r++;
                break;
            }
            /* Protect $ and ` in this line */
            for (size_t i = 0; i < line_len; i++) {
                if (line_start[i] == '$') line_start[i] = 0x01;
                else if (line_start[i] == '`') line_start[i] = 0x02;
            }
            if (*r == '\n') r++;
        }
        p = r;
    }
}

int jash_is_heredoc_redirect(const char* token) {
    if (!token) return 0;
    return strstr(token, "<<") != NULL;
}

/* Check if token uses <<- (indented terminator) mode */
int jash_is_heredoc_indented(const char* token) {
    if (!token) return 0;
    const char* ptr = strstr(token, "<<");
    if (!ptr) return 0;
    return (ptr[2] == '-') ? 1 : 0;
}

/* Extract delimiter from token like "<<EOF" → "EOF" or "<<-EOF" → "EOF" */
char* jash_extract_heredoc_delim(const char* token) {
    if (!token) return NULL;
    
    const char* delim_start = strstr(token, "<<");
    if (!delim_start) return NULL;
    
    delim_start += 2;
    
    /* Skip <<- marker if present */
    if (*delim_start == '-') {
        delim_start++;
    }
    
    if (!*delim_start) return NULL;
    
    /* Allocate and copy */
    int len = strlen(delim_start);
    char* delim = malloc(len + 1);
    if (delim) {
        strlcpy(delim, delim_start, len + 1);
    }
    return delim;
}

/* Validate heredoc delimiter syntax.
 * Returns: 0 if valid, -1 if invalid.
 * Valid delimiters: alphanumeric, underscore, no spaces */
int jash_validate_heredoc_delim(const char* delim) {
    if (!delim || !*delim) return -1;
    
    /* Allow alphanumeric and underscore */
    for (int i = 0; delim[i]; i++) {
        char c = delim[i];
        if (!isalnum(c) && c != '_') {
            return -1;  /* Invalid character in delimiter */
        }
    }
    
    return 0;
}

/* Get total size of heredoc content (for buffer pre-allocation) */
size_t jash_heredoc_content_size(const char* content) {
    if (!content) return 0;
    return strlen(content);
}

/* Helper: Format heredoc error message */
const char* jash_heredoc_strerror(int errcode) {
    switch (errcode) {
        case -1:
            return "Invalid heredoc delimiter";
        case -2:
            return "Heredoc memory allocation failed";
        case -3:
            return "Unexpected EOF in heredoc";
        default:
            return "Unknown heredoc error";
    }
}
