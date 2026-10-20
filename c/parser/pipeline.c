// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/pipeline — multi-command pipe execution.
//
// Parses "cmd1 | cmd2 | cmd3" into segments, creates N-1 pipes,
// N children with dup2 plumbing, and the parent waits for all.
//
// Redirects (> < >>) are handled here too — they modify the fd
// table of the child before execve.

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "pipeline.h"
#include <core/argvbuf.h>   /* JINV-131: the one growable argv builder */
#include <core/limits.h>
#include "quotes.h"
#include "eval.h"
#include "function_defs.h"
#include <stdio.h>
#include "heredoc.h"
#include <fcntl.h>
#include <stdlib.h>
#include <unistd.h>
#include <signal.h>
#include <spawn.h>
#include <errno.h>
#include "exec.h"
#include <sys/syscalls.h>
#include <core/env.h>
#include <core/envp.h>
#include <core/alias.h>
#include <core/guard.h>
#include <core/preexec.h>
#include <core/timeline.h>
#include <theme/colors.h>
#include "../builtins/builtin.h"
#include "script.h"
#include <sys/stat.h>
#include "cmdsub.h"
#include "arithmetic.h"
#include "param_expand.h"
#include "builtin_redir.h"

/* ═══ JINV-124 — a re-expansão por segmento estava PELA METADE ═══════════════
 *
 * Quando a linha carrega um composto de verdade (`… | ( sub )`, `… | { grp; }`),
 * eval.c ergue `skip_global_expand`: o corpo do composto tem que ser expandido
 * no contexto DELE, não no da linha. A conta é este arquivo re-expandir cada
 * segmento — e ele re-expandia só PARÂMETROS. Aritmética e substituição de
 * comando ficavam sem dono e saíam LITERAIS:
 *
 *     echo $((1+1)) | ( cat )        bash: 2     jash: $((1+1))
 *     echo $(echo cs) | ( cat )      bash: cs    jash: $(echo cs)
 *
 * Na mesma linha `$HOME` funcionava. Essa assimetria é o que fazia o usuário
 * duvidar do próprio comando em vez do shell — o pior tipo de defeito: silencioso
 * e com cara de culpa de quem digitou.
 *
 * A ordem (cmdsub → aritmética → parâmetro) é a MESMA de eval.c:2448/2740/3055/
 * 3366-3397. Ela mora aqui uma vez, e não em cinco cópias — eram cinco os sítios
 * que chamavam `jash_param_expand` sozinho (spawn, builtin, e três caminhos de
 * fork), a mesma multiplicação que produziu JINV-098/116/117.
 *
 * Corner conhecido e registrado: divisão por zero dentro de um segmento reporta
 * o diagnóstico aqui, mas o segmento ainda roda com o texto original (eval.c
 * ABORTA o comando nesse caso). Hoje ele não reportava nada, então isto é
 * estritamente menos ruim; o alinhamento fino está no roadmap, não fingido aqui.
 *
 * Gate: tests/test_jinv124_arith_in_pipeline.sh */
static const char* seg_expand(const char* str, void* s, char* buf, size_t cap) {
    if (!str || !*str) return str;

    if (jash_has_cmdsub(str)) {
        const char* x = jash_expand_cmdsub_known((jash_state_t*)s, str);
        if (x && x != str) {
            size_t n = strlen(x); if (n >= cap) n = cap - 1;
            memcpy(buf, x, n); buf[n] = '\0';
            str = buf;
        }
    }
    if (jash_has_arithmetic(str)) {
        jash_arith_clear_error();
        const char* x = jash_expand_arithmetic(str, s);
        if (jash_arith_error()) {
            jash_writes(jash_stderr_writer, 0, "jash: division by 0\n");
        } else if (x && x != str) {
            size_t n = strlen(x); if (n >= cap) n = cap - 1;
            memcpy(buf, x, n); buf[n] = '\0';
            str = buf;
        }
    }
    {
        const char* x = jash_param_expand(str, s);
        if (x && x != str) {
            size_t n = strlen(x); if (n >= cap) n = cap - 1;
            memcpy(buf, x, n); buf[n] = '\0';
            str = buf;
        }
    }
    return str;
}

/* ═══ JINV-082 — o enunciado composto remontado aqui não pode ser truncado ══
 *
 * Quando um `while`/`for`/`case`/`if`/`select` carrega um redirect ou entra
 * num cano (`while …; do …; done < arq`, `for …; done | wc -l`), o avaliador
 * de eval.c delega a este arquivo, que precisa REMONTAR o enunciado a partir
 * dos pedaços `cmd` + `args` do segmento. Isso era:
 *
 *     char full_line[1024];
 *     while (*cp && flen + 1 < sizeof(full_line)) full_line[flen++] = *cp++;
 *
 * — o quarto gêmeo do mesmo clamp mudo (JINV-080 no `for`, JINV-082 no
 * `while` e no `case`). Penhasco medido com `while read L; do … done < arq`:
 *
 *     1015 B de arquivo  →  igual ao bash
 *     1080 B de arquivo  →  "missing done"     (a cauda do enunciado sumiu)
 *
 * Aqui o corte às vezes grita (quando cai no meio do composto) e às vezes
 * cala (quando cai depois do `done`) — o mesmo defeito com dois sintomas
 * diferentes conforme o tamanho.
 *
 * Conserto idêntico ao dos outros três: pilha até 1023 bytes, heap do
 * tamanho exato acima, falta de memória = recusa ALTA.
 *
 * Devolve NULL só sem memória; `*heap` recebe o que precisa de free().
 * Gate: tests/test_jinv082_loop_body_heap.sh */
static char* jash_compound_join(const char* cmd, const char* args,
                                char* inl, size_t inl_cap, char** heap) {
    size_t lc = cmd  ? strlen(cmd)  : 0;
    size_t la = args ? strlen(args) : 0;
    size_t need = lc + 1 + la + 1;      /* cmd + ' ' + args + NUL */
    char* out = inl;
    *heap = NULL;
    if (need > inl_cap) {
        out = (char*)malloc(need);
        if (!out) return NULL;
        *heap = out;
    }
    if (lc) memcpy(out, cmd, lc);
    out[lc] = ' ';                      /* o espaço existia mesmo sem args */
    if (la) memcpy(out + lc + 1, args, la);
    out[lc + 1 + la] = '\0';
    return out;
}

static int is_compound_keyword(const char* cmd) {
    return (strcmp(cmd, "[[") == 0 ||
            strcmp(cmd, "while") == 0 ||
            strcmp(cmd, "until") == 0 ||
            strcmp(cmd, "for") == 0 ||
            strcmp(cmd, "case") == 0 ||
            strcmp(cmd, "if") == 0 ||
            strcmp(cmd, "select") == 0);
}

/* ---- constants ---- */

#define PIPE_MAX_CMDS    16
/* JINV-021 — teto de um segmento de pipeline. Casado com JASH_CMDSUB_CAP e
 * JASH_PARAM_EXPAND_CAP (128 KB): o segmento precisa caber o que a expansao
 * pode produzir, ou a capacidade daquelas e ficcao. */
#define PIPE_SEG_MAX     (2 * JASH_LINE_CAP)  /* JINV-026/030: >= JASH_LINE_CAP para a linha intacta caber */
#define PIPE_MAX_ARGS    128
#define PIPE_ARG_BUF    32768
#define PIPE_MAX_REDIRS   8

/* ---- redirect operation model ----
 *
 * Bash applies redirects left-to-right; a single fixed in/out/err triple
 * cannot express `2>&1`, `<&3`, `N>file`, `<<<`, `<>` or their ordering.
 * So each segment carries an ordered list of redir_t ops, applied in
 * sequence in the child after pipe plumbing. */

enum {
    RD_FILE_IN = 1,   /* [n]< file      open RDONLY,  dup2 -> target   */
    RD_FILE_OUT,      /* [n]> file      open WRONLY|CREAT|TRUNC        */
    RD_FILE_APPEND,   /* [n]>> file     open WRONLY|CREAT|APPEND       */
    RD_FILE_RW,       /* [n]<> file     open RDWR|CREAT                */
    RD_DUP,           /* [n]>&m / [n]<&m   dup2(src_fd, target_fd)     */
    RD_CLOSE,         /* [n]>&- / [n]<&-   close(target_fd)            */
    RD_PREPARED,      /* heredoc / here-string: prepared_fd -> target */
    RD_FILE_CLOBBER,  /* >| file: force overwrite even when noclobber is set */
};

typedef struct {
    int  kind;           /* RD_* */
    int  target_fd;      /* fd being redirected (default 0 for <, 1 for >) */
    int  src_fd;         /* RD_DUP: fd to duplicate from                    */
    int  prepared_fd;    /* RD_PREPARED: already-open fd (inherited by fork)*/
    char fname[256];     /* file-based kinds                               */
} redir_t;

/* ---- segment: one command in a pipeline ---- */

typedef struct {
    const char* cmd;       /* command name (first token)           */
    const char* args;      /* rest-of-line after first token       */
    char fullpath[256];    /* resolved PATH (filled by lookup)     */
    redir_t redirs[PIPE_MAX_REDIRS];
    int     nredir;
    int     pipe_stderr;   /* `|&` : route stderr into the pipe too */
    int     is_builtin;    /* 1 if cmd resolved to a shell builtin */
    const jash_builtin_t* builtin; /* builtin entry (if is_builtin) */
    int     is_brace_group; /* 1 if cmd is { (brace group) */
    int     is_function;   /* 1 if cmd is a shell function */
    int     is_compound;   /* 1 if cmd is a compound keyword (while/until/for/case/if/select) */
    int     is_absorbed;   /* ADR-0040 W3: bash-c segment re-exec'd as jash */
    const char* assigns;   /* inline env prefix (NAME=VAL\x1F...) or NULL */
} seg_t;

/* ---- small utilities ---- */

static int pipe_cloexec(int fds[2]) {
#ifdef __linux__
    if (pipe2(fds, O_CLOEXEC) == 0) return 0;
    if (errno != ENOSYS) return -1;
#endif
    if (sys_pipe(fds) < 0) return -1;
    /* pipe() starts with descriptor flags clear; fail closed if either set fails. */
    if (fcntl(fds[0], F_SETFD, FD_CLOEXEC) < 0 ||
        fcntl(fds[1], F_SETFD, FD_CLOEXEC) < 0) {
        int err = errno;
        sys_close(fds[0]);
        sys_close(fds[1]);
        errno = err;
        return -1;
    }
    return 0;
}

/* File-static state pointer for variable expansion in heredocs.
 * Set by jash_exec_pipeline before calling split_pipeline.
 * Safe because jash is single-instance per process. */
static const jash_state_t* s_pipeline_state = NULL;

static unsigned my_strlen(const char* s) {
    unsigned n = 0; while (s[n]) n++; return n;
}

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Split first token off line. Returns pointer to args. */
static const char* split_first(const char* line, char* name, unsigned cap) {
    const char* p = skip_ws(line);
    /* Subshell: ( is a keyword, not a command name */
    if (*p == '(') {
        name[0] = '('; name[1] = '\0';
        return skip_ws(p + 1);
    }
    /* Brace group: { is a keyword, not a command name */
    if (*p == '{' && (p[1] == ' ' || p[1] == '\t' || p[1] == '\n')) {
        name[0] = '{'; name[1] = '\0';
        return skip_ws(p + 1);
    }
    /* JINV-019: quote-aware (parser/quotes.c) — este laço cortava no primeiro
     * espaço, truncando um nome de comando citado com espaço. */
    return jash_first_word(p, name, cap, 1, (int*)0);
}

/* ---- redirect parser ---- */

/* Use portable flags from <fcntl.h> instead of hardcoded Linux octal values */
#include <fcntl.h>

/* Read a filename token (stops at whitespace). Returns chars consumed. */
static const char* read_fname(const char* r, char* out, unsigned cap) {
    r = skip_ws(r);
    unsigned fi = 0;
    char quote = 0;
    while (*r) {
        if (!quote && (*r == ' ' || *r == '\t')) break;   /* só fora de aspas */
        /* JINV-098/116: remoção de aspas = parser/quotes.c. */
        char em = 0;
        const char* q = jash_unquote_step(r, &quote, &em);
        if (q) { if (em && fi + 1 < cap) out[fi++] = em; r = q; continue; }
        if (fi + 1 < cap) out[fi++] = *r;
        r++;
    }
    out[fi] = '\0';
    /* The pipeline keeps expansion sentinels until word boundaries are known.
     * A filename is now data for open(), not another expansion input. */
    jash_cmdsub_unprotect(out);
    jash_quote_unshield(out);
    return r;
}

/* JINV-095 — a ÚNICA ressincronização do `stdin` do C depois de um `dup2` no
 * fd 0. Definição e contrato completo em c/parser/builtin_redir.c. */
extern void jash_stdin_stream_sync(int drop_buffered);

/* Append an op to the segment's redir list (silently drops if full). */
static redir_t* redir_push(seg_t* seg, int kind, int target_fd) {
    if (seg->nredir >= PIPE_MAX_REDIRS) return 0;
    redir_t* rd = &seg->redirs[seg->nredir++];
    rd->kind = kind;
    rd->target_fd = target_fd;
    rd->src_fd = -1;
    rd->prepared_fd = -1;
    rd->fname[0] = '\0';
    return rd;
}

/* ADR-0050 W1: journal write redirects at PARSE time (parent). apply_redirs
 * runs in the forked child — a journal write there would die with the exec.
 * Skips when noclobber guarantees the open fails (existing file + `>`). */
static void pl_journal(const char* fname, int append, int clobber) {
    if (!fname || !*fname) return;
    if (!clobber && s_pipeline_state && s_pipeline_state->shell_opts.noclobber) {
        struct stat st;
        if (stat(fname, &st) == 0) return;
    }
    jash_timeline_journal_redirect(fname, append);
}

/* Prepare a heredoc/here-string body into an inherited read fd. */
static int prepare_temp_fd(const char* content) {
    char tmpname[256]; snprintf(tmpname, sizeof(tmpname), "%s/jash_heredoc_XXXXXX", jash_tmpdir());
    int tmpfd = mkstemp(tmpname);
    if (tmpfd < 0) return -1;
    unsigned long len = 0; while (content[len]) len++;
    sys_write(tmpfd, content, len);
    sys_close(tmpfd);
    /* SECURITY (audit rank 5, fd leak): this heredoc/here-string fd is opened
     * in the PARENT and survives until used, so without O_CLOEXEC every child
     * forked for the pipeline inherits it across execve — leaking the heredoc
     * body (which may be a secret) to unrelated commands in the pipeline. With
     * O_CLOEXEC it auto-closes on exec everywhere EXCEPT where apply_redirs
     * dup2()s it onto the target fd (the dup'd copy intentionally has no
     * CLOEXEC), which is exactly the one process meant to read it.
     * F E R M I ∞ H A R T */
    long fd = sys_open(tmpname, O_RDONLY | O_CLOEXEC, 0);
    unlink(tmpname);
    return (int)fd;
}

/* Scan the args string for redirects and extract them into seg->redirs[].
 * Modifies args in-place to strip the redirect tokens. Files are NOT opened
 * here (except prepared heredoc/here-string fds): file opens happen in the
 * child via apply_redirs so each child gets fresh offsets and ordering. */
static void parse_redirs(seg_t* seg, char* args_mut) {
    seg->nredir = 0;

    char* r = args_mut;
    char* w = args_mut;
    char quote = 0;
    int word_start = 1;

    while (*r) {
        /* Quote-aware: redirect operators inside quotes are literal text and
         * must be copied through untouched (e.g. sh -c 'echo a 1>&2'). */
        const char* quoted_end = jash_quote_step(r, &quote);
        if (quoted_end) {
            if (quote || *r != '\\' || r[1] != '\n') word_start = 0;
            while (r < quoted_end) *w++ = *r++;
            continue;
        }

        /* A numeric suffix of a data word (including quoted/escaped spaces)
         * is not an fd prefix. Only unquoted word-initial digits qualify. */
        int explicit_fd = -1;
        if (word_start && *r >= '0' && *r <= '9') {
            char* d = r;
            int val = 0;
            while (*d >= '0' && *d <= '9') { val = val * 10 + (*d - '0'); d++; }
            if (*d == '<' || *d == '>') { explicit_fd = val; r = d; }
        }

        /* &> file / &>> file — both stdout+stderr to file. */
        if (explicit_fd < 0 && *r == '&' && *(r+1) == '>') {
            word_start = 1;
            r += 2;
            int append = 0;
            if (*r == '>') { append = 1; r++; }
            char fname[256];
            r = (char*)read_fname(r, fname, sizeof(fname));
            if (fname[0]) {
                pl_journal(fname, append, 0);
                redir_t* o = redir_push(seg, append ? RD_FILE_APPEND : RD_FILE_OUT, 1);
                if (o) { unsigned i = 0; while (fname[i] && i+1 < sizeof(o->fname)) { o->fname[i] = fname[i]; i++; } o->fname[i] = '\0'; }
                redir_t* e = redir_push(seg, RD_DUP, 2);
                if (e) e->src_fd = 1;
            }
            continue;
        }

        if (*r == '>') {
            word_start = 1;
            int target = (explicit_fd >= 0) ? explicit_fd : 1;
            r++;
            int append = 0, clobber = 0;
            if (*r == '>') { append = 1; r++; }
            else if (*r == '|') { clobber = 1; r++; }  /* >| noclobber-override */
            if (*r == '&') {
                /* >&m (dup), >&- (close), or >&file (== &>file) */
                r++;
                if (*r == '-') { r++; redir_push(seg, RD_CLOSE, target); continue; }
                if (*r >= '0' && *r <= '9') {
                    int src = 0;
                    while (*r >= '0' && *r <= '9') { src = src * 10 + (*r - '0'); r++; }
                    redir_t* d = redir_push(seg, RD_DUP, target);
                    if (d) d->src_fd = src;
                    continue;
                }
                /* >&file form: both target and stderr to file */
                char fname[256];
                r = (char*)read_fname(r, fname, sizeof(fname));
                if (fname[0]) {
                    pl_journal(fname, append, 0);
                    redir_t* o = redir_push(seg, append ? RD_FILE_APPEND : RD_FILE_OUT, 1);
                    if (o) { unsigned i = 0; while (fname[i] && i+1 < sizeof(o->fname)) { o->fname[i] = fname[i]; i++; } o->fname[i] = '\0'; }
                    redir_t* e = redir_push(seg, RD_DUP, 2);
                    if (e) e->src_fd = 1;
                }
                continue;
            }
            char fname[256];
            r = (char*)read_fname(r, fname, sizeof(fname));
            if (fname[0]) {
                int kind = clobber ? RD_FILE_CLOBBER : (append ? RD_FILE_APPEND : RD_FILE_OUT);
                pl_journal(fname, append, clobber);
                redir_t* o = redir_push(seg, kind, target);
                if (o) { unsigned i = 0; while (fname[i] && i+1 < sizeof(o->fname)) { o->fname[i] = fname[i]; i++; } o->fname[i] = '\0'; }
            }
            continue;
        }

        if (*r == '<') {
            word_start = 1;
            int target = (explicit_fd >= 0) ? explicit_fd : 0;
            r++;
            /* here-string <<< */
            if (*r == '<' && *(r+1) == '<') {
                r += 2;
                char word[1024];
                r = (char*)read_fname(r, word, sizeof(word));
                /* bash appends a trailing newline to here-strings */
                unsigned wl = 0; while (word[wl]) wl++;
                if (wl + 2 < sizeof(word)) { word[wl] = '\n'; word[wl+1] = '\0'; }
                int fd = prepare_temp_fd(word);
                if (fd >= 0) { redir_t* h = redir_push(seg, RD_PREPARED, target); if (h) h->prepared_fd = fd; }
                continue;
            }
            /* heredoc << */
            if (*r == '<') {
                r++;
                int indented = 0;
                if (*r == '-') { indented = 1; r++; }
                /* Handle quoted delimiters: <<'EOF' or <<"EOF" → no expansion */
                int no_expand = 0;
                char delim[256];
                unsigned di = 0;
                if (*r == '\'' || *r == '"') {
                    char q = *r++;
                    no_expand = 1;
                    while (*r && *r != q && di + 1 < sizeof(delim))
                        delim[di++] = *r++;
                    if (*r == q) r++;
                } else {
                    while (*r && *r != ' ' && *r != '\t' && *r != '\n' && di + 1 < sizeof(delim))
                        delim[di++] = *r++;
                }
                delim[di] = '\0';
                if (di > 0) {
                    /* Try to read heredoc body inline from the args string.
                     * The body follows on subsequent lines within the same
                     * command string (e.g. from -c mode or script eval).
                     * Fall back to reading from STDIN if no inline body. F E R M I ∞ H A R T */
                    char* content = NULL;
                    /* Skip to the newline after the delimiter */
                    while (*r && *r != '\n') r++;

                    if (*r == '\n') {
                        r++; /* skip the newline, now r points to body start */
                        /* Collect body lines until delimiter */
                        size_t cap = 4096;
                        content = malloc(cap);
                        size_t pos = 0;
                        if (content) {
                            while (*r) {
                                const char* line_start = r;
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

                                /* Append line to content buffer */
                                size_t needed = line_len + 2;
                                while (pos + needed > cap) {
                                    if (cap > SIZE_MAX / 2) { free(content); content = NULL; break; }
                                    cap *= 2;
                                    char* nb = realloc(content, cap);
                                    if (!nb) { free(content); content = NULL; break; }
                                    content = nb;
                                }
                                if (content) {
                                    memcpy(content + pos, line_start, line_len);
                                    pos += line_len;
                                    if (*r == '\n') { content[pos++] = '\n'; r++; }
                                }
                            }
                            if (content) content[pos] = '\0';
                        }
                    }

                    /* Variable expansion in heredoc body (unless quoted delimiter).
                     * Expand $VAR and ${VAR} using the shell environment. */
                    if (content && !no_expand && s_pipeline_state) {
                        /* Use a simple inline expansion loop */
                        size_t src_len = strlen(content);
                        if (src_len > (SIZE_MAX - 256) / 2) { free(content); content = NULL; }
                        size_t exp_cap = src_len * 2 + 256;
                        char* exp = malloc(exp_cap);
                        if (exp) {
                            size_t ei = 0;
                            const char* cp = content;
                            while (*cp && ei < exp_cap - 1) {
                                if (*cp == '$') {
                                    cp++;
                                    char vname[256];
                                    unsigned vi = 0;
                                    int braced = 0;
                                    if (*cp == '{') { braced = 1; cp++; }
                                    while (*cp && vi < sizeof(vname) - 1 &&
                                           ((*cp >= 'a' && *cp <= 'z') || (*cp >= 'A' && *cp <= 'Z') ||
                                            (*cp >= '0' && *cp <= '9') || *cp == '_'))
                                        vname[vi++] = *cp++;
                                    vname[vi] = '\0';
                                    if (braced && *cp == '}') cp++;
                                    if (vi > 0) {
                                        const char* val = jash_env_get(s_pipeline_state, vname);
                                        if (val) {
                                            while (*val && ei < exp_cap - 1)
                                                exp[ei++] = *val++;
                                        }
                                    } else {
                                        if (ei < exp_cap - 1) exp[ei++] = '$';
                                    }
                                } else {
                                    exp[ei++] = *cp++;
                                }
                            }
                            exp[ei] = '\0';
                            free(content);
                            content = exp;
                        }
                    }

                    if (!content) {
                        /* Fall back to reading from stdin (interactive mode) */
                        content = indented ?
                            jash_parse_heredoc_ex(delim, 1) :
                            jash_parse_heredoc(delim);
                    }
                    if (content) {
                        int fd = prepare_temp_fd(content);
                        if (fd >= 0) { redir_t* h = redir_push(seg, RD_PREPARED, target); if (h) h->prepared_fd = fd; }
                        free(content);
                    }
                }
                continue;
            }
            /* <> read+write */
            if (*r == '>') {
                r++;
                char fname[256];
                r = (char*)read_fname(r, fname, sizeof(fname));
                if (fname[0]) {
                    redir_t* o = redir_push(seg, RD_FILE_RW, target);
                    if (o) { unsigned i = 0; while (fname[i] && i+1 < sizeof(o->fname)) { o->fname[i] = fname[i]; i++; } o->fname[i] = '\0'; }
                }
                continue;
            }
            /* <&m (dup), <&- (close) */
            if (*r == '&') {
                r++;
                if (*r == '-') { r++; redir_push(seg, RD_CLOSE, target); continue; }
                if (*r >= '0' && *r <= '9') {
                    int src = 0;
                    while (*r >= '0' && *r <= '9') { src = src * 10 + (*r - '0'); r++; }
                    redir_t* d = redir_push(seg, RD_DUP, target);
                    if (d) d->src_fd = src;
                    continue;
                }
                /* bare <& — ignore malformed */
                continue;
            }
            /* < file */
            char fname[256];
            r = (char*)read_fname(r, fname, sizeof(fname));
            if (fname[0]) {
                redir_t* o = redir_push(seg, RD_FILE_IN, target);
                if (o) { unsigned i = 0; while (fname[i] && i+1 < sizeof(o->fname)) { o->fname[i] = fname[i]; i++; } o->fname[i] = '\0'; }
            }
            continue;
        }

        word_start = *r == ' ' || *r == '\t' || *r == '\n';
        *w++ = *r++;
    }
    /* Trim trailing whitespace left from a redirect that consumed `>|`/`>>`/etc
     * but kept the space that preceded the operator in the args. Without this,
     * `echo hi >| file` writes "hi \n" (trailing space) — visible in any tool
     * that captures the redirect output.
     * JINV-063: era cego e comia tambem o espaco ESCAPADO, que e DADO
     * (`echo x\  | cat` imprimia `x\`). Termina primeiro, corta depois. */
    *w = '\0';
    args_mut[jash_rtrim_unquoted(args_mut)] = '\0';
}

/* Apply a segment's redirect list in the child, in order. Returns 0 on ok. */
static int apply_redirs(seg_t* seg) {
    const char* subject = "redirection";
    for (int i = 0; i < seg->nredir; i++) {
        redir_t* rd = &seg->redirs[i];
        int flags = 0;
        int fd;
        subject = "redirection";
        switch (rd->kind) {
            case RD_FILE_IN:
                flags = O_RDONLY;
                break;
            case RD_FILE_OUT:
                flags = O_WRONLY | O_CREAT | O_TRUNC;
                if (s_pipeline_state && s_pipeline_state->shell_opts.noclobber)
                    flags = O_WRONLY | O_CREAT | O_EXCL;
                break;
            case RD_FILE_CLOBBER:
                flags = O_WRONLY | O_CREAT | O_TRUNC;
                break;
            case RD_FILE_APPEND:
                flags = O_WRONLY | O_CREAT | O_APPEND;
                break;
            case RD_FILE_RW:
                flags = O_RDWR | O_CREAT;
                break;
            case RD_DUP: {
                long rc;
                do { rc = sys_dup2(rd->src_fd, rd->target_fd); } while (rc < 0 && errno == EINTR);
                if (rc < 0) goto failed;
                continue;
            }
            case RD_CLOSE:
                if (sys_close(rd->target_fd) < 0 && errno != EBADF) goto failed;
                continue;
            case RD_PREPARED:
                break;
            default:
                errno = EINVAL;
                goto failed;
        }
        if (rd->kind == RD_PREPARED) {
            fd = rd->prepared_fd;
            if (fd < 0) { errno = EBADF; goto failed; }
            rd->prepared_fd = -1;
        } else {
            subject = rd->fname;
            fd = (int)sys_open(rd->fname, flags, 0666);
            if (fd < 0) goto failed;
        }
        if (fd != rd->target_fd) {
            long rc;
            do { rc = sys_dup2(fd, rd->target_fd); } while (rc < 0 && errno == EINTR);
            int err = rc < 0 ? errno : 0;
            if (sys_close(fd) < 0 && !err) err = errno;
            if (err) { errno = err; goto failed; }
        } else if (rd->kind == RD_PREPARED) {
            /* dup2(fd, fd) would leave the heredoc's CLOEXEC flag set. */
            int fdflags = fcntl(fd, F_GETFD);
            if (fdflags < 0 || fcntl(fd, F_SETFD, fdflags & ~FD_CLOEXEC) < 0)
                goto failed;
        }
    }
    return 0;
failed:
    jash_writef(jash_stderr_writer, NULL, "jash: %s: %s\n", subject, strerror(errno));
    return -1;
}

static int write_cmd_not_found_with_redirs(seg_t* seg,
                                           const int* operands, size_t count) {
    int targets[PIPE_MAX_REDIRS];
    for (int i = 0; i < seg->nredir; i++) targets[i] = seg->redirs[i].target_fd;
    jash_redir_snapshot_t* saved;
    if (jash_redir_snapshot_begin(&saved, targets, (size_t)seg->nredir, operands, count))
        return 1;
    int rc = apply_redirs(seg) == 0 ? 127 : 1;
    if (rc == 127)
        jash_writef(jash_stderr_writer, NULL, "%s: command not found\n", seg->cmd);
    if (jash_redir_snapshot_end(saved, 1)) rc = 1;
    return rc;
}

/* Close any prepared (already-open) fds held by the parent after fork. */
static void close_prepared_fds(seg_t* segs, int nseg) {
    for (int i = 0; i < nseg; i++)
        for (int j = 0; j < segs[i].nredir; j++)
            if (segs[i].redirs[j].kind == RD_PREPARED &&
                segs[i].redirs[j].prepared_fd >= 0) {
                sys_close(segs[i].redirs[j].prepared_fd);
                segs[i].redirs[j].prepared_fd = -1;
            }
}

/* ---- inline env-assignment prefix (NAME=VALUE cmd) ---- */

/* Detect a leading inline-env assignment prefix (NAME=VALUE [NAME2=VAL2 ...])
 * at the head of a pipeline segment. Copies the raw assignment tokens into
 * `out`, each separated by \x1F, and returns a pointer to the start of the
 * real command. If there is no assignment prefix (or the segment is
 * assignments only), `out` is emptied and the original `line` is returned.
 * Quote/paren/backtick aware so values like FOO="a b" or X=$(cmd) are never
 * split mid-value. */
static const char* scan_assign_prefix(const char* line, char* out, unsigned cap) {
    const char* p = skip_ws(line);
    const char* cmd_start = p;
    unsigned oi = 0;
    out[0] = '\0';
    while (1) {
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_'))
            break;
        const char* q = p;
        while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
               (*q >= '0' && *q <= '9') || *q == '_') q++;
        if (!(*q == '=' || (q[0] == '+' && q[1] == '=')))
            break;  /* not an assignment token — this is the command */
        const char* val = (*q == '=') ? q + 1 : q + 2;
        const char* end = val;
        char qc = 0; int pd = 0, bt = 0;
        while (*end) {
            char c = *end;
            if (qc) { if (c == qc) qc = 0; else if (c == '\\' && qc == '"' && end[1]) end++; end++; continue; }
            if (c == '\\' && end[1]) { end += 2; continue; }
            if (c == '\'' || c == '"') { qc = c; end++; continue; }
            if (c == '`') { bt = !bt; end++; continue; }
            if (c == '(') { pd++; end++; continue; }
            if (c == ')') { if (pd > 0) pd--; end++; continue; }
            if ((c == ' ' || c == '\t') && pd == 0 && !bt) break;
            end++;
        }
        if (oi > 0 && oi + 1 < cap) out[oi++] = '\x1F';
        for (const char* t = p; t < end && oi + 1 < cap; t++) out[oi++] = *t;
        out[oi] = '\0';
        p = skip_ws(end);
        cmd_start = p;
    }
    if (oi == 0 || *cmd_start == '\0') { out[0] = '\0'; return line; }
    return cmd_start;
}

/* Apply a \x1F-separated NAME=VALUE assignment list to env `s`. Each value
 * gets one round of parameter expansion and one level of outer-quote
 * stripping (mirrors the inline-env handling in eval.c). Called only in a
 * forked child, so the assignments are scoped to this pipeline segment and
 * never leak to the parent shell. */
static void apply_assigns(jash_state_t* s, const char* list) {
    extern const char* jash_param_expand(const char*, void*);
    const char* p = list;
    while (*p) {
        const char* tok = p;
        while (*p && *p != '\x1F') p++;
        unsigned tl = (unsigned)(p - tok);
        if (*p == '\x1F') p++;
        if (tl == 0) continue;

        char tbuf[2048];
        if (tl >= sizeof(tbuf)) tl = sizeof(tbuf) - 1;
        memcpy(tbuf, tok, tl); tbuf[tl] = '\0';

        char* eq = tbuf;
        while (*eq && *eq != '=') eq++;
        if (*eq != '=') continue;
        int append = (eq > tbuf && eq[-1] == '+');
        char name[128];
        unsigned nl = (unsigned)((eq - tbuf) - (append ? 1 : 0));
        if (nl >= sizeof(name)) nl = sizeof(name) - 1;
        memcpy(name, tbuf, nl); name[nl] = '\0';
        const char* val = eq + 1;

        const char* xp = jash_param_expand(val, s);
        if (xp) val = xp;

        char vbuf[2048];
        size_t L = strlen(val);
        if (L >= 2 && (val[0] == '"' || val[0] == '\'') && val[L - 1] == val[0]) {
            size_t cl = L - 2;
            if (cl >= sizeof(vbuf)) cl = sizeof(vbuf) - 1;
            memcpy(vbuf, val + 1, cl); vbuf[cl] = '\0';
            val = vbuf;
        }

        if (append) {
            if (jash_env_append(s, name, val) < 0) jash_env_set(s, name, val);
        } else {
            jash_env_set(s, name, val);
        }
    }
}

/* ---- segment splitter ---- */

/* JINV-110: um `|` DENTRO do corpo de um composto (`do`…`done`, `case`…`esac`,
 * `if`…`fi`) pertence ao CORPO, não ao pipeline que envolve o composto:
 *
 *     echo a | while read p; do echo $p | cat; done
 *
 * tem UM cano. O splitter contava DOIS e entregava `cat; done` como se fosse
 * um comando — `cat;: command not found`. O mesmo estrago aparecia quando o
 * laço inteiro era canalizado (`done | cat`) ou redirecionado (`done >f`),
 * porque esses dois casos também descem para o motor de pipeline.
 *
 * A palavra só conta como palavra-reservada em POSIÇÃO DE COMANDO — a mesma
 * regra que o JINV-083 já pagou para aprender. Sem ela, `echo done | cat`
 * (onde `done` é DADO) mudaria a estrutura da linha inteira.
 * Gate: tests/test_jinv110_compound_pipeline.sh          F E R M I ∞ H A R T */
static unsigned pipe_word_len(const char* p) {
    unsigned n = 0;
    while (p[n] && ((p[n] >= 'a' && p[n] <= 'z') || (p[n] >= 'A' && p[n] <= 'Z') ||
                    (p[n] >= '0' && p[n] <= '9') || p[n] == '_')) n++;
    return n;
}
static int pipe_word_is(const char* p, unsigned len, const char* kw) {
    return strlen(kw) == (size_t)len && strncmp(p, kw, len) == 0;
}
/* Palavras que ABREM um corpo (o fechador correspondente devolve o nível). */
static int pipe_kw_opens(const char* p, unsigned len) {
    return pipe_word_is(p, len, "do") || pipe_word_is(p, len, "case") ||
           pipe_word_is(p, len, "if");
}
static int pipe_kw_closes(const char* p, unsigned len) {
    return pipe_word_is(p, len, "done") || pipe_word_is(p, len, "esac") ||
           pipe_word_is(p, len, "fi");
}
/* Palavras que são reservadas mas NÃO consomem a posição de comando: o que
 * vem depois delas continua sendo o começo de um comando. */
static int pipe_kw_keeps_cmd_pos(const char* p, unsigned len) {
    return pipe_word_is(p, len, "then")  || pipe_word_is(p, len, "else")   ||
           pipe_word_is(p, len, "elif")  || pipe_word_is(p, len, "while")  ||
           pipe_word_is(p, len, "until") || pipe_word_is(p, len, "for")    ||
           pipe_word_is(p, len, "select")|| pipe_word_is(p, len, "time");
    /* `in` fica de FORA de propósito: `for f in done; do …` tem um `done` que
     * é DADO, e mantê-lo em posição de comando o transformaria em fechador. */
}

/* Split line on '|' into segments. Handles basic quoting (single/double)
 * so that | inside quotes is not treated as a pipe. Also handles extglob
 * patterns like @(foo|bar) where | is NOT a pipe separator.
 * Returns segment count (0 = empty, 1 = no pipe, 2+ = pipeline). */
static int split_pipeline(const char* line, seg_t* segs, int max) {
    int n = 0;
    const char* p = line;

    /* Persistent storage for segment text. Each segment gets its own
     * buffer: first the null-terminated cmd, then a space, then args.
     * Bumped from 512 to 16384 so a single `export FOO="<10KB value>"`
     * segment doesn't get sliced. */
    /* JINV-021: era 8192 (e o comentario acima dizia 16384 — ja nao batia).
     * O teto de um SEGMENTO tem que ser >= o teto do que o alimenta: uma
     * substituicao de comando produz ate JASH_CMDSUB_CAP (128 KB) e a
     * expansao de parametro ate JASH_PARAM_EXPAND_CAP (128 KB). Com 8 KB
     * aqui, essas capacidades eram MENTIRA: `V="$(cat arquivo_de_30KB)"`
     * perdia dados sem um unico aviso. Alocado uma vez, sob demanda. */
    static char (*seg_buf)[PIPE_SEG_MAX] = NULL;
    /* JINV-019: o nome do comando cabia em 64 bytes — um binário em caminho
     * absoluto longo era truncado CALADO, e a mensagem de erro apontava para
     * um arquivo que ninguém escreveu. Um nome de comando é um caminho: use o
     * teto de caminho. */
    static char (*cmd_buf)[JASH_CMDWORD_MAX] = NULL;
    static char (*assign_buf)[1024] = NULL;
    if (!seg_buf) {
        seg_buf    = malloc(PIPE_MAX_CMDS * PIPE_SEG_MAX);
        cmd_buf    = malloc(PIPE_MAX_CMDS * JASH_CMDWORD_MAX);
        assign_buf = malloc(PIPE_MAX_CMDS * 1024);
        if (!seg_buf || !cmd_buf || !assign_buf) return 0;
    }

    while (*p && n < max) {
        p = skip_ws(p);
        if (!*p) break;

        const char* seg_start = p;
        char quote = 0;
        int extglob_depth = 0;  /* tracks @( *( +( ?( !( patterns */
        int paren_depth = 0;    /* tracks ( ) for subshells */
        int brace_depth = 0;    /* tracks { } for brace groups */
        int bracket_depth = 0;  /* tracks [[ ]] for conditional expressions */
        int compound_depth = 0; /* JINV-110: do…done / case…esac / if…fi */
        int at_cmd = 1;         /* JINV-110: estamos em posição de comando? */

        /* Walk until unquoted '|' or end. */
        while (*p) {
            /* JINV-116/117: a regra de aspas vem de parser/quotes.c, não de uma
             * cópia local. A cópia local não tratava \" dentro de aspa dupla,
             * então `echo "a\"|b"` era partido em dois no meio de uma string. */
            { const char* q = jash_quote_step(p, &quote);
              if (q) { at_cmd = 0; p = q; continue; } }

            /* JINV-110: contabilidade de palavra-reservada em posição de
             * comando. Só aqui um `do`/`done` é ESTRUTURA; em qualquer outra
             * posição ele é dado e não pode mexer no nível. */
            if (extglob_depth == 0) {
                if (*p == ' ' || *p == '\t') { p++; continue; }
                if (*p == ';' || *p == '&' || *p == '\n') { at_cmd = 1; p++; continue; }
                unsigned wl = pipe_word_len(p);
                if (wl) {
                    if (at_cmd) {
                        if (pipe_kw_opens(p, wl))            { compound_depth++; }
                        else if (pipe_kw_closes(p, wl))      { if (compound_depth > 0) compound_depth--;
                                                               at_cmd = 0; }
                        else if (!pipe_kw_keeps_cmd_pos(p, wl)) { at_cmd = 0; }
                    }
                    p += wl;
                    continue;
                }
            }

            /* Detect extglob patterns: @( *( +( ?( !( */
            if ((*p == '@' || *p == '*' || *p == '+' || *p == '?' || *p == '!') &&
                p[1] == '(' && extglob_depth == 0) {
                extglob_depth = 1;
                p += 2;
                continue;
            }
            if (extglob_depth > 0) {
                if (*p == '(') { extglob_depth++; p++; continue; }
                if (*p == ')') { extglob_depth--; p++; continue; }
                /* | inside extglob is NOT a pipe */
                if (*p == '|') { p++; continue; }
                p++;
                continue;
            }

            /* Track subshell parentheses: | inside () is NOT a pipe.
             * JINV-110: depois de `(` e de `)` volta-se a posição de comando —
             * `(cmd)` abre um, e `a) cmd` (braço de `case`) começa outro. */
            if (*p == '(') { paren_depth++; at_cmd = 1; p++; continue; }
            if (*p == ')') { if (paren_depth > 0) paren_depth--; at_cmd = 1; p++; continue; }

            /* Track brace groups: | inside {} is NOT a pipe */
            if (*p == '{') { brace_depth++; at_cmd = 1; p++; continue; }
            if (*p == '}') { if (brace_depth > 0) brace_depth--; at_cmd = 0; p++; continue; }

            /* Track [[ ]] conditionals: | inside [[ ... || ... ]] is NOT a pipe */
            if (*p == '[' && p[1] == '[') { bracket_depth++; p += 2; continue; }
            if (*p == ']' && p[1] == ']') { if (bracket_depth > 0) bracket_depth--; at_cmd = 0; p += 2; continue; }

            if (*p == '|' && paren_depth == 0 && brace_depth == 0 &&
                bracket_depth == 0 && compound_depth == 0) {
                /* `>|` is the noclobber-override redirect operator, NOT a pipe.
                 * (Used by Claude Code's cwd handshake: `pwd -P >| /tmp/...`.) */
                if (p > seg_start && p[-1] == '>') { p++; continue; }
                break;
            }
            /* JINV-110: cano de dentro de um corpo — anda, não parte. */
            if (*p == '|') { at_cmd = 1; p++; continue; }
            p++;
        }

        unsigned slen = (unsigned)(p - seg_start);
        /* Trim trailing whitespace from segment (avoids passing " hello " as
         * args to builtins, which then echo the trailing space — bug seen in
         * `echo hello | cat`).
         * JINV-063: o corte agora acontece APOS a copia — ali o segmento ja e
         * uma string terminada em NUL, e o corte pode ser CONSCIENTE DE ASPAS.
         * O rtrim cego que morava aqui tambem comia o espaco ESCAPADO
         * (`echo x\  | cat` imprimia `x\`), que e DADO. */
        /* JINV-021: FALHA ALTA. Cortar calado transforma dado em comando
         * (um `;` que sobrava depois do corte ja virou execucao) e devolve um
         * resultado "quase certo" que ninguem confere.
         * JINV-026: o teto de segmento deve ser maior que o teto de linha, porque
         * expand.c devolve a linha INTACTA quando ela excede o teto de pilha.
         * Uma linha de 256 KB (JASH_LINE_CAP) mais o involucro `echo "..." | wc -c`
         * pode passar de 256 KB; se o segmento recusar, perdemos o proprio pipe. */
        if (slen >= PIPE_SEG_MAX) {
            fprintf(stderr,
                    "jash: segmento de %u bytes excede o teto de %d "
                    "(recusando em vez de truncar em silencio)\n",
                    slen, PIPE_SEG_MAX - 1);
            return 0;
        }
        for (unsigned i = 0; i < slen; i++) seg_buf[n][i] = seg_start[i];
        seg_buf[n][slen] = '\0';
        slen = (unsigned)jash_rtrim_unquoted(seg_buf[n]);   /* JINV-063 */
        seg_buf[n][slen] = '\0';

        /* Inline env-assignment prefix (NAME=VALUE ... cmd): strip leading
         * assignments so cmd resolves to the real command, and remember them
         * so the forked child can apply them to its own env (scoped to this
         * pipeline segment, matching POSIX — never leaks to the parent). */
        const char* cmd_text = scan_assign_prefix(seg_buf[n], assign_buf[n], 1024);
        segs[n].assigns = (cmd_text != seg_buf[n]) ? assign_buf[n] : NULL;

        /* Extract first token (cmd) from the (post-assignment) segment text. */
        /* JINV-019: use o ponteiro DEVOLVIDO para o resto — derivar o offset
         * de strlen(nome) erra sempre que a palavra foi truncada. */
        const char* after_cmd = split_first(cmd_text, cmd_buf[n],
                                            JASH_CMDWORD_MAX);

        /* cmd points to the extracted first token (persistent). */
        segs[n].cmd  = cmd_buf[n];
        segs[n].pipe_stderr = 0;
        /* args points to the rest inside seg_buf[n] (after the cmd). */
        segs[n].args = after_cmd;

        /* BUG-028: POSIX quote removal on the command WORD of a pipeline segment
         * (`echo x | "$GREP" y`). Done AFTER args offset above so args still
         * points into the original (quoted) text correctly. */
        {
            extern int jash_unquote(const char* in, char* out, size_t out_size);
            extern int jash_has_quotes(const char* s);
            if (jash_has_quotes(cmd_buf[n])) {
                char unqc[JASH_CMDWORD_MAX];  /* JINV-019: era 64 */
                int ucl = jash_unquote(cmd_buf[n], unqc, sizeof(unqc));
                if (ucl >= 0) {
                    unsigned k = 0;
                    for (; k < sizeof(cmd_buf[n]) - 1 && unqc[k]; k++) cmd_buf[n][k] = unqc[k];
                    cmd_buf[n][k] = '\0';
                }
            }
        }

        /* Parse redirects (mutates seg_buf[n] in-place after cmd).
          * Skip for [[ ]] — < and > are comparison operators inside conditionals. */
        char* args_mut = (char*)segs[n].args;
        if (strcmp(segs[n].cmd, "[[") != 0)
            parse_redirs(&segs[n], args_mut);

        n++;

        if (*p == '|') {
            p++;
            /* `|&` — pipe stdout AND stderr of the just-finished segment. */
            if (*p == '&') { segs[n - 1].pipe_stderr = 1; p++; }
        }
    }

    return n;
}

/* ---- argv builder (same as exec.c — inlined to avoid coupling) ---- */

/* JINV-131: same storage and growth policy as parser/exec.c (core/argvbuf.h).
 * The tokenizers still differ — this one also un-protects cmdsub sentinels
 * (JINV-090) — but the ceiling that broke both is now one rule in one place.
 *
 * Tokens are recorded as OFFSETS while building: `out->buf` moves under
 * realloc, so a pointer captured here would dangle by the next token. */
static int build_argv_local(jash_argvbuf_t* out, const char* cmd, const char* args) {
    jash_argvbuf_rewind(out);

    unsigned cl = my_strlen(cmd);
    if (jash_argvbuf_reserve(out, cl + 1) != 0) return -1;
    if (jash_argvbuf_slot(out) != 0) return -2;
    for (unsigned i = 0; i < cl; i++) out->buf[out->blen++] = cmd[i];
    out->buf[out->blen++] = '\0';
    out->aoff[out->argc++] = 0;

    if (!args) goto done;
    const char* p = args;
    /* BUG-094: quote state persists across \x1E element boundaries (the
     * elements of "${a[@]}" share one quote pair); a \x1E ends one argv
     * word without closing the quote, and consecutive \x1E inside quotes
     * preserve empty elements. */
    char quote = 0;
    while (*p) {
        if (!quote)
            while (*p == ' ' || *p == '\t' || *p == '\x1E') p++;  /* skip array separators too */
        if (!*p) break;
        if (jash_argvbuf_slot(out) != 0) return -2;

        out->aoff[out->argc++] = out->blen;

        while (*p) {
            char c = *p;
            if (c == '\x1E') break;  /* Array word boundary — splits even inside "..." */
            if (quote) {
                if (c == quote) { p++; quote = 0; continue; }
            } else {
                if (c == ' ' || c == '\t') break;
                if (c == '\'' || c == '"') { quote = c; p++; continue; }
            }
            if (c == '\\' && !quote && p[1]) { p++; c = *p; }
            if (jash_argvbuf_reserve(out, 2) != 0) return -3;
            out->buf[out->blen++] = c;
            p++;
        }
        if (jash_argvbuf_reserve(out, 1) != 0) return -3;
        out->buf[out->blen++] = '\0';
        if (quote && *p == '\x1E') p++;
    }

done:
    /* buf has stopped moving — offsets become pointers exactly once, here. */
    jash_argvbuf_done(out);
    /* BUG-096: restore shielded literal quotes in every token */
    {
        extern void jash_quote_unshield(char* s);
        for (unsigned i = 0; i < out->argc; i++) jash_quote_unshield(out->argv[i]);
    }
    /* JINV-090: aqui — depois da expansão de parâmetros DESTE segmento e da
     * tokenização — é onde o `$`/crase blindado deixa de correr risco de ser
     * reexpandido. eval.c deixou de restaurar antes do cano exatamente para
     * que a marca chegasse viva até este ponto (ver o comentário lá). Os
     * sentinelas não são metacaracteres de citação nem de glob, então
     * atravessam a tokenização intactos. */
    {
        extern void jash_cmdsub_unprotect(char* s);
        for (unsigned i = 0; i < out->argc; i++) jash_cmdsub_unprotect(out->argv[i]);   /* JINV-090 */
    }
    return 0;
}

/* ---- checked environment admission shared with exec.c ---- */

#define PIPE_MAX_ENV JASH_ENVP_MAX
#define PIPE_ENV_BUF JASH_ENVP_BYTES

static int build_envp_local(char* envp[], int max_envc,
                            char* buf, unsigned buf_cap,
                            const jash_state_t* s) {
    return jash_envp_build(envp, (unsigned)max_envc, buf, buf_cap, s, NULL, NULL);
}

/* ADR-0040 W2/W3: shadow envp for ABSORBED segments — same pointers as the
 * parent-built envp plus JASH_ABSORBED=1, so a re-exec'd `bash -c` segment
 * spools one compact record instead of emitting its own envelope (and never
 * consumes the outermost invocation's spool at its own emit). Pointer copy
 * only; the string storage stays in the caller's buffer. */
static int mk_envp_abs(char* const* src, char** dst, int cap) {
    int ne = 0, have = 0;
    size_t bytes = 0;
    while (src[ne]) {
        size_t length = strlen(src[ne]) + 1;
        if (ne >= cap - 1 || ne >= PIPE_MAX_ENV || length > PIPE_ENV_BUF - bytes)
            goto too_large;
        if (strncmp(src[ne], "JASH_ABSORBED=", 14) == 0) have = 1;
        dst[ne] = (char*)src[ne];
        bytes += length;
        ne++;
    }
    if (!have) {
        if (ne >= cap - 1 || ne >= PIPE_MAX_ENV ||
            sizeof("JASH_ABSORBED=1") > PIPE_ENV_BUF - bytes)
            goto too_large;
        dst[ne++] = (char*)"JASH_ABSORBED=1";
    }
    dst[ne] = NULL;
    return 0;
too_large:
    dst[0] = NULL;
    errno = E2BIG;
    return -1;
}

/* ---- decode wait status ---- */

static int decode_wait_status(int status) {
    if ((status & 0x7F) == 0) return (status >> 8) & 0xFF;
    if ((status & 0x7F) != 0x7F) return 128 + (status & 0x7F);
    return 128;
}

/* ---- background fast path (`cmd &` without a waiter) ----
 *
 * WHY this exists: `cmd &` forks a subshell running the full evaluator;
 * for an external command the evaluator forks AGAIN and the subshell
 * waits, holding the spawner's stdout/stderr pipes for the whole job.
 * An agent tool that reads pipes until EOF then hangs until the job
 * ends (bash execs a lone external in place — one fork — so a
 * redirected job releases the pipes immediately).
 *
 * jash_pipeline_bg_exec runs the single-external case in place: same
 * parse + classify + absorb/guard + PATH + expansion + assigns +
 * redirects + argv/envp as the pipeline fork-path child, then execve
 * (no return). Anything else (pipes, builtins, functions, compounds,
 * aliases, `!`, `time`, `coproc`, absorbable segments, lookup failure)
 * returns untouched and the caller falls back to the full subshell
 * eval — behavior identical to before. MUST be called in an
 * already-forked child whose pgid/signals/stdin were set up by the
 * background spawner.
 *
 * Guard parity: the single-external foreground path (jash_exec_external)
 * runs the absorb check + bypass guard; both are replicated here so
 * `evil &` is policed exactly like `evil`. Memo/budget/err_digest are
 * skipped: there is no parent left to flush/serve them, and a
 * background daemon must really run (bash parity). */
void jash_pipeline_bg_exec(jash_state_t* s, jash_writer_t w, void* ctx,
                           const char* line) {
    if (!s || !line || !*line) return;
    seg_t segs[PIPE_MAX_CMDS];
    /* BUG-157 class: split_pipeline does NOT clear the classification
     * flags (the engine resets them after splitting for the same
     * reason) — stack garbage here caused nondeterministic fallback. */
    memset(segs, 0, sizeof segs);
    int nseg = split_pipeline(line, segs, PIPE_MAX_CMDS);
    if (nseg != 1) return;                    /* pipeline: subshell path */
    if (!segs[0].cmd[0]) return;              /* redirect-only: subshell */
    if (segs[0].cmd[0] == '(') return;        /* subshell segment */
    if (segs[0].pipe_stderr) return;          /* `|&` needs nseg>=2 anyway */
    if (is_compound_keyword(segs[0].cmd)) return;
    if (strcmp(segs[0].cmd, "!") == 0) return;       /* JINV-066 negation */
    if (strcmp(segs[0].cmd, "time") == 0) return;    /* eval.c timed prefix */
    if (strcmp(segs[0].cmd, "coproc") == 0) return;  /* coproc table lives in parent */
    if (strcmp(segs[0].cmd, "{") == 0 || segs[0].is_brace_group) return;
    if (jash_builtin_lookup(segs[0].cmd)) return;
    if (s->functions && jash_function_exists(s->functions, segs[0].cmd)) return;
    if (jash_alias_get(s, segs[0].cmd)) return;      /* body could be anything */
    if (segs[0].is_absorbed) return;           /* ADR-0040 W3: subshell absorbs */

    /* ADR-0040 W3 absorb check first (same order as the engine): an
     * absorbable `bash -c` falls back so the subshell re-execs it. */
    {
        char probe[256];
        if (jash_preexec_absorb_seg(segs[0].cmd, segs[0].args,
                                    probe, sizeof probe))
            return;
        jash_preexec_note_missed_seg(segs[0].cmd, segs[0].args);
    }
    /* Bypass guard (same order as jash_exec_external: before lookup). */
    if (jash_guard_bypass_detect(segs[0].cmd, segs[0].args, w, ctx)) {
        /* Message already printed by the guard; exit code matches the
         * foreground path (126) so `wait %1` reports it identically. */
        sys_exit(126);
        for (;;) { }
    }
    if (!jash_path_lookup(s, segs[0].cmd,
                          segs[0].fullpath, sizeof segs[0].fullpath))
        return;   /* fallback prints today's not-found diagnostic */

    /* Inline env prefix (FOO=1 cmd): child-local, dies with exec. */
    if (segs[0].assigns) apply_assigns(s, segs[0].assigns);

    char exp_args[16384];
    /* JINV-124: cmdsub → aritmética → parâmetro, uma regra só. */
    const char* final_args = seg_expand(segs[0].args, s, exp_args, sizeof exp_args);

    static jash_argvbuf_t bg_ab;
    if (build_argv_local(&bg_ab, segs[0].fullpath, final_args) != 0) {
        const char* msg = "jash: argv build failed\n";
        sys_write(2, msg, my_strlen(msg));
        sys_exit(1);
        for (;;) { }
    }

    char* envbuf = (char*)malloc(PIPE_ENV_BUF);
    if (!envbuf) {
        const char* msg = "jash: out of memory\n";
        sys_write(2, msg, my_strlen(msg));
        sys_exit(1);
        for (;;) { }
    }
    static char* bg_envp[PIPE_MAX_ENV + 1];
    if (build_envp_local(bg_envp, PIPE_MAX_ENV, envbuf, PIPE_ENV_BUF, s) != 0) {
        jash_writes(jash_stderr_writer, NULL, JASH_ENVP_LIMIT_MESSAGE);
        free(envbuf);
        sys_exit(126);
        for (;;) { }
    }

    /* User redirects override whatever fds the spawner set up (incl. the
     * /dev/null stdin for non-job-control background jobs). */
    if (apply_redirs(&segs[0]) != 0) {
        const char* msg = "jash: redirect failed\n";
        sys_write(2, msg, my_strlen(msg));
        sys_exit(1);
        for (;;) { }
    }

    sys_execve(segs[0].fullpath, bg_ab.argv, bg_envp);

    /* execve failed. */
    {
        const char* msg = "jash: execve failed\n";
        sys_write(2, msg, my_strlen(msg));
        sys_exit(127);
        for (;;) { }
    }
}

/* ---- pipeline executor ---- */

int jash_exec_pipeline(jash_state_t* s, jash_writer_t w, void* ctx,
                       const char* line) {

    s_pipeline_state = s;  /* For heredoc variable expansion */
    seg_t segs[PIPE_MAX_CMDS];
    int nseg = split_pipeline(line, segs, PIPE_MAX_CMDS);

    if (nseg == 0) return 0;

    /* Relocate enclosing in-process backups before any user fd is touched. */
    int operands[2 * PIPE_MAX_CMDS * PIPE_MAX_REDIRS];
    size_t operand_count = 0;
    for (int i = 0; i < nseg; i++) {
        for (int j = 0; j < segs[i].nredir; j++) {
            operands[operand_count++] = segs[i].redirs[j].target_fd;
            if (segs[i].redirs[j].kind == RD_DUP)
                operands[operand_count++] = segs[i].redirs[j].src_fd;
        }
    }
    if (jash_redir_reserve_fds(operands, operand_count)) {
        close_prepared_fds(segs, nseg);
        return 1;
    }

    /* Single command — fast path: delegate to exec_external (which
     * already handles fork+execve+wait). But we must also handle
     * redirects that parse_redirs may have found. */
    if (nseg == 1) {
        /* Check if there are redirects. If not, just call exec_external. */
        if (segs[0].nredir == 0 && !segs[0].pipe_stderr && !segs[0].assigns) {
            return jash_exec_external(s, w, ctx, segs[0].cmd, segs[0].args);
        }
        /* Single command WITH redirects — fall through to the
         * pipeline engine (1 child, no pipes, just dup2 redirs). */
    }

    /* Resolve PATH for each segment. */
    for (int i = 0; i < nseg; i++) {
        segs[i].is_builtin = 0;
        segs[i].builtin = NULL;
        segs[i].is_compound = 0;
        segs[i].is_absorbed = 0;
        /* BUG-157: is_function and is_brace_group were NOT reset here, but
         * `seg_t segs[]` is an uninitialized stack array — they carried stack
         * garbage from a prior call at the same depth. A garbage-nonzero
         * is_function on an EXTERNAL last segment (e.g. `echo x | head -1`)
         * fails the `!is_function` guard at the lastpipe spawn branch, routing
         * the external command into the builtin dispatch where `builtin` is
         * NULL → `builtin->argv_fn` deref → SIGSEGV. Non-deterministic:
         * identical statements crashed or not depending on residual stack.
         * Also clear fullpath so a stale resolved path can't leak. */
        segs[i].is_function = 0;
        segs[i].is_brace_group = 0;
        segs[i].fullpath[0] = '\0';
        /* Subshell segments: (body) — skip PATH lookup */
        if (segs[i].cmd[0] == '(') {
            segs[i].fullpath[0] = '\0';
            continue;
        }
        /* Compound command: while/until/for/case/if/select — evaluate whole line */
        if (is_compound_keyword(segs[i].cmd)) {
            segs[i].is_compound = 1;
            continue;
        }
        /* Check builtin lookup first. */
        const jash_builtin_t* b = jash_builtin_lookup(segs[i].cmd);
        if (b) {
            segs[i].is_builtin = 1;
            segs[i].builtin = b;
            continue;
        }
        /* Brace group: { ... } — treat as compound command */
        if (segs[i].cmd[0] == '{' && segs[i].cmd[1] == '\0') {
            segs[i].is_builtin = 0;
            segs[i].is_brace_group = 1;
            continue;
        }
        /* Shell function: check function table before PATH lookup */
        if (s->functions && jash_function_exists(s->functions, segs[i].cmd)) {
            segs[i].is_builtin = 0;
            segs[i].is_function = 1;
            continue;
        }
        /* ADR-0040 W3 — level 3's OWN policy: a `bash -c STR` segment is
         * absorbed by swapping ONLY the binary (fullpath := running jash).
         * Every pipe/fd/fork/redir detail stays with the caller, so the
         * pipeline keeps bash process semantics exactly while the segment
         * regains recording, guard and undo. Levels 1-2 just count the
         * miss so the scorecard can suggest level 3 with data. */
        if (jash_preexec_absorb_seg(segs[i].cmd, segs[i].args,
                                    segs[i].fullpath,
                                    sizeof(segs[i].fullpath))) {
            /* ADR-0040 W2/W3: the re-exec'd segment must know it is an
             * absorbed CHILD, or it emits its own envelope and — because
             * it inherits JASH_ABSORB_SPOOL — folds+unlinks (steals) the
             * outermost invocation's spool at its own emit. */
            segs[i].is_absorbed = 1;
            continue;        /* fullpath now = the running jash binary */
        }
        jash_preexec_note_missed_seg(segs[i].cmd, segs[i].args);
        if (!jash_path_lookup(s, segs[i].cmd,
                              segs[i].fullpath, sizeof(segs[i].fullpath))) {
            (void)w;
            (void)ctx;
            int rc = write_cmd_not_found_with_redirs(&segs[i], operands, operand_count);
            /* Close any prepared fds opened by parse_redirs. */
            close_prepared_fds(segs, nseg);
            return rc;
        }
    }

    /* Lastpipe runs with shell state: save stdin AND all explicit targets in
     * the shared registry before pipe allocation can occupy a user operand. */
    int lastpipe = (nseg > 1 && !segs[nseg - 1].assigns && (
                    segs[nseg - 1].is_builtin ||
                    segs[nseg - 1].is_compound ||
                    segs[nseg - 1].fullpath[0] != '\0'));
    int fork_count = lastpipe ? nseg - 1 : nseg;
    jash_redir_snapshot_t* lastpipe_saved = NULL;
    if (lastpipe) {
        int targets[PIPE_MAX_REDIRS + 1];
        targets[0] = STDIN_FILENO;
        int last = nseg - 1;
        for (int i = 0; i < segs[last].nredir; i++)
            targets[i + 1] = segs[last].redirs[i].target_fd;
        if (jash_redir_snapshot_begin(&lastpipe_saved, targets,
                                      (size_t)segs[last].nredir + 1,
                                      operands, operand_count)) {
            close_prepared_fds(segs, nseg);
            return 1;
        }
    }

    /* Create N-1 pipes. pipe_fds[i] = {read, write} for the pipe
     * connecting segment i (write) to segment i+1 (read).
     * O_CLOEXEC: pipe ends auto-close on execve in children — no manual
     * close loop needed for external commands, and posix_spawn inherits
     * only the fds explicitly dup2'd via file_actions. */
    int pipe_fds[PIPE_MAX_CMDS][2];
    for (int i = 0; i < nseg - 1; i++) {
        long rc = pipe_cloexec(pipe_fds[i]);
        if (rc < 0) {
            jash_writef(w, ctx, "%sjash: pipe failed (%ld)%s\n",
                        J_ROLE_ERR, rc, J_RESET);
            for (int j = 0; j < i; j++) {
                sys_close(pipe_fds[j][0]);
                sys_close(pipe_fds[j][1]);
            }
            close_prepared_fds(segs, nseg);
            if (lastpipe_saved && jash_redir_snapshot_end(lastpipe_saved, 0)) return 1;
            return 1;
        }
    }

    /* Build envp ONCE in the parent. All children inherit it read-only via
     * COW — no page faults, no per-exec env rebuild (the main reason jash
     * was 3× slower than bash in pipelines). */
    int needs_envp = 0;
    int needs_plain_envp = 0;
    int any_absorbed = 0;
    int needs_plain_absorbed = 0;
    for (int i = 0; i < nseg; i++) {
        /* Only resolved external commands consume this vector. Builtins,
         * functions and compound commands use shell state. Assigned external
         * segments rebuild and check their effective environment in the child. */
        if (segs[i].fullpath[0]) {
            needs_envp = 1;
            if (!segs[i].assigns) needs_plain_envp = 1;
        }
        if (segs[i].is_absorbed) {
            any_absorbed = 1;
            if (!segs[i].assigns) needs_plain_absorbed = 1;
        }
    }
    static char* s_envp[PIPE_MAX_ENV + 1];
    static char* s_envp_abs[PIPE_MAX_ENV + 2];
    static char* s_envp_buf = NULL;
    if (needs_envp && !s_envp_buf) {
        s_envp_buf = malloc(PIPE_ENV_BUF);
        if (!s_envp_buf) {
            for (int i = 0; i < nseg - 1; i++) {
                sys_close(pipe_fds[i][0]);
                sys_close(pipe_fds[i][1]);
            }
            close_prepared_fds(segs, nseg);
            if (lastpipe_saved && jash_redir_snapshot_end(lastpipe_saved, 0)) return 1;
            return 1;
        }
    }
    int env_admission_failed = needs_envp &&
        build_envp_local(s_envp, PIPE_MAX_ENV, s_envp_buf, PIPE_ENV_BUF, s) != 0 &&
        needs_plain_envp;
    /* The injected absorption marker counts against the same entry/byte
     * limits as user entries. Assigned segments check their effective envp
     * again after applying assignments in the child. */
    if (!env_admission_failed && any_absorbed &&
        mk_envp_abs(s_envp, s_envp_abs, PIPE_MAX_ENV + 2) != 0 &&
        needs_plain_absorbed)
        env_admission_failed = 1;
    if (env_admission_failed) {
        jash_writes(jash_stderr_writer, NULL, JASH_ENVP_LIMIT_MESSAGE);
        for (int i = 0; i < nseg - 1; i++) {
            sys_close(pipe_fds[i][0]);
            sys_close(pipe_fds[i][1]);
        }
        close_prepared_fds(segs, nseg);
        if (lastpipe_saved && jash_redir_snapshot_end(lastpipe_saved, 0)) return 1;
        return 126;
    }

    /* Fork children for segments 0..fork_count-1. */
    long pids[PIPE_MAX_CMDS];
    for (int i = 0; i < nseg; i++) pids[i] = -1;

    /* Static argv scratch used by posix_spawn path (built in parent, not child). */
    static jash_argvbuf_t sp_ab;      /* parent: posix_spawn path */
    static jash_argvbuf_t child_ab;   /* child after fork — pre-grown below */

    for (int i = 0; i < fork_count; i++) {

        /* ---- posix_spawn fast path (macOS) ----
         * For pure external commands with no file redirects, skip fork entirely.
         * posix_spawn uses vfork internally: zero address-space copy, no COW
         * faults, and signals are reset atomically via spawnattr. */
        int can_spawn = !segs[i].is_builtin   &&
                        !segs[i].is_function   &&
                        !segs[i].is_brace_group &&
                        !segs[i].is_compound   &&
                        segs[i].cmd[0] != '('  &&
                        segs[i].nredir == 0    &&
                        !segs[i].assigns       &&
                        segs[i].fullpath[0] != '\0';

        if (can_spawn) {
            char exp_sp[16384];
            /* JINV-124: cmdsub → aritmética → parâmetro, uma regra só. */
            const char* sp_args = seg_expand(segs[i].args, s, exp_sp, sizeof(exp_sp));

            if (build_argv_local(&sp_ab, segs[i].fullpath, sp_args) == 0) {

                posix_spawn_file_actions_t fa;
                posix_spawnattr_t          sa;
                posix_spawn_file_actions_init(&fa);
                posix_spawnattr_init(&sa);

                /* Reset SIGINT/SIGQUIT to default in spawned child. */
                sigset_t defsigs;
                sigemptyset(&defsigs);
                sigaddset(&defsigs, SIGINT);
                sigaddset(&defsigs, SIGQUIT);
                posix_spawnattr_setsigdefault(&sa, &defsigs);
                posix_spawnattr_setflags(&sa, POSIX_SPAWN_SETSIGDEF);

                /* Pipe plumbing via file_actions (pipes have O_CLOEXEC —
                 * only the dup2'd fds survive into the child). */
                if (i > 0)
                    posix_spawn_file_actions_adddup2(&fa, pipe_fds[i-1][0], STDIN_FILENO);
                if (i < nseg - 1) {
                    posix_spawn_file_actions_adddup2(&fa, pipe_fds[i][1], STDOUT_FILENO);
                    if (segs[i].pipe_stderr)
                        posix_spawn_file_actions_adddup2(&fa, pipe_fds[i][1], STDERR_FILENO);
                }

                pid_t spid = (pid_t)-1;
                int spawn_rc = posix_spawn(&spid, segs[i].fullpath,
                                           &fa, &sa, sp_ab.argv,
                                           segs[i].is_absorbed ? s_envp_abs
                                                               : s_envp);
                posix_spawn_file_actions_destroy(&fa);
                posix_spawnattr_destroy(&sa);

                if (spawn_rc == 0) {
                    pids[i] = (long)spid;
                    continue;  /* skip fork path entirely */
                }
                /* spawn failed — fall through to fork */
            }
        }

        /* JINV-131: jash starts a background loader thread (boot.c), so the
         * child of fork() must not call malloc — the allocator lock may have
         * been held by another thread at fork time. Grow to the worst case
         * HERE, in the parent; every reserve in the child then hits its fast
         * path. seg_expand caps the expanded args at sizeof(exp_args). */
        (void)jash_argvbuf_pregrow(&child_ab, 16384 + (unsigned)sizeof segs[i].fullpath);

        pids[i] = sys_fork();
        if (pids[i] < 0) {
            jash_writef(w, ctx, "%sjash: fork failed for segment %d (%ld)%s\n",
                        J_ROLE_ERR, i, pids[i], J_RESET);
            pids[i] = -1;
            break;
        }

        if (pids[i] == 0) {
            /* ---- CHILD i (fork path: builtins, functions, subshells) ---- */

            /* SECURITY: restore terminal signals to default. */
            signal(SIGINT,  SIG_DFL);
            signal(SIGQUIT, SIG_DFL);

            /* Plumbing: connect stdin from previous pipe (if any). */
            if (i > 0) {
                sys_dup2(pipe_fds[i - 1][0], 0 /* STDIN */);
            }
            /* Plumbing: connect stdout to next pipe (if any). */
            if (i < nseg - 1) {
                sys_dup2(pipe_fds[i][1], 1 /* STDOUT */);
                if (segs[i].pipe_stderr) {
                    sys_dup2(pipe_fds[i][1], 2 /* STDERR */);
                }
            }

            /* Close all pipe fds in the child (needed for builtins/functions
             * that don't exec — pipes have CLOEXEC but that only fires on exec). */
            for (int j = 0; j < nseg - 1; j++) {
                sys_close(pipe_fds[j][0]);
                sys_close(pipe_fds[j][1]);
            }

            /* Inline env-assignment prefix (FOO=1 cmd): apply to THIS child's
             * env so the builtin/function/external command sees the vars.
             * The change dies with the child, so it stays scoped to this
             * pipeline segment. Rebuild s_envp so the external execve path
             * (which uses the parent-built envp) also picks up the new vars. */
            if (segs[i].assigns) {
                apply_assigns(s, segs[i].assigns);
                if (segs[i].fullpath[0] &&
                    build_envp_local(s_envp, PIPE_MAX_ENV, s_envp_buf, PIPE_ENV_BUF, s) != 0) {
                    jash_writes(jash_stderr_writer, NULL, JASH_ENVP_LIMIT_MESSAGE);
                    sys_exit(126);
                    for (;;) { }
                }
                if (segs[i].is_absorbed &&
                    mk_envp_abs(s_envp, s_envp_abs, PIPE_MAX_ENV + 2) != 0) {
                    jash_writes(jash_stderr_writer, NULL, JASH_ENVP_LIMIT_MESSAGE);
                    sys_exit(126);
                    for (;;) { }
                }
            }

            /* Apply this segment's ordered redirect list (override pipe
             * plumbing where they conflict — bash semantics). */
            if (apply_redirs(&segs[i]) != 0) {
                const char* msg = "jash: redirect failed\n";
                sys_write(2, msg, my_strlen(msg));
                sys_exit(1);
            }

            /* Builtin path: call the builtin function directly. */
            if (segs[i].is_builtin) {
                /* Expand variables in args for this segment.
                 * Global expansion was skipped for pipelines with compound commands. */
                char exp_args[16384];
                /* JINV-124: cmdsub → aritmética → parâmetro, uma regra só. */
                const char* final_args = seg_expand(segs[i].args, s, exp_args, sizeof(exp_args));
                if (final_args) {
                    /* JINV-090: o builtin não passa por build_argv_local, então
                     * o restauro adiado por eval.c tem que acontecer AQUI —
                     * depois da expansão deste segmento e antes do dispatch. */
                    { extern void jash_cmdsub_unprotect(char* s);
                      jash_cmdsub_unprotect((char*)final_args); }   /* JINV-090 */
                }
                
                /* Quote removal for builtins (parity with eval.c builtin path).
                 * Without this, `echo "$(echo deep)" | cat` prints "deep"
                 * instead of deep — outer quotes leak through.
                 * EXCEPTION: printf and set do their own quote-aware parsing. */
                extern int jash_unquote(const char* in, char* out, size_t out_size);
                extern int jash_has_quotes(const char* s);
                char unq_args[16384];
                /* ADR-0036 W2: argv_fn-migrated builtins always get raw args
                 * (see eval.c's identical dispatch note). */
                int skip_unquote = (segs[i].builtin && segs[i].builtin->argv_fn != NULL) ||
                                   (strcmp(segs[i].cmd, "printf") == 0) ||
                                   (strcmp(segs[i].cmd, "set") == 0) ||
                                   (strcmp(segs[i].cmd, "trap") == 0) ||
                                   (strcmp(segs[i].cmd, "builtin") == 0) ||
                                   (strcmp(segs[i].cmd, "command") == 0) ||
                                   (strcmp(segs[i].cmd, "grep") == 0) ||
                                   (strcmp(segs[i].cmd, "mesh") == 0) ||
                                   (strcmp(segs[i].cmd, "contract") == 0) ||
                                   (strcmp(segs[i].cmd, "cost") == 0) ||
                                   (strcmp(segs[i].cmd, "revert") == 0) ||
                                   (strcmp(segs[i].cmd, "export") == 0) ||
                                   (strcmp(segs[i].cmd, "declare") == 0) ||
                                   (strcmp(segs[i].cmd, "typeset") == 0) ||
                                   (strcmp(segs[i].cmd, "readonly") == 0) ||
                                   (strcmp(segs[i].cmd, "local") == 0) ||
                                   /* BUG-151: raw tail → quote-aware exec. */
                                   (strcmp(segs[i].cmd, "env") == 0);
                if (!skip_unquote && final_args && jash_has_quotes(final_args)) {
                    int unq_len = jash_unquote(final_args, unq_args, sizeof(unq_args));
                    if (unq_len >= 0) final_args = unq_args;
                }
                if (jash_guard_bypass_detect(segs[i].cmd, final_args, w, ctx)) {
                    sys_exit(126);
                }
                int brc = jash_builtin_dispatch(segs[i].builtin, s, w, ctx, final_args);
                sys_exit(brc);
                for (;;) { }
            }

            /* Subshell path: segment is (body) — eval body in child.
             * Reconstruct the full segment text from cmd + args, strip
             * the outer parens, and eval. This allows subshells to
             * participate in pipelines: (echo a; echo b) | cat */
            if (segs[i].cmd[0] == '(') {
                char sub_line[512];
                unsigned sl = 0;
                const char* sp = segs[i].cmd;
                while (*sp && sl + 1 < sizeof(sub_line)) sub_line[sl++] = *sp++;
                if (segs[i].args && *segs[i].args) {
                    sub_line[sl++] = ' ';
                    const char* ap = segs[i].args;
                    while (*ap && sl + 1 < sizeof(sub_line)) sub_line[sl++] = *ap++;
                }
                sub_line[sl] = '\0';
                /* Verify it ends with ')' and extract body */
                if (sl > 1 && sub_line[sl - 1] == ')') {
                    char body[512];
                    unsigned bl = sl - 2; /* exclude ( and ) */
                    /* Trim whitespace */
                    const char* bs = sub_line + 1;
                    while (bl > 0 && (*bs == ' ' || *bs == '\t')) { bs++; bl--; }
                    while (bl > 0 && (bs[bl-1] == ' ' || bs[bl-1] == '\t' || bs[bl-1] == ';')) bl--;
                    if (bl >= sizeof(body)) bl = sizeof(body) - 1;
                    for (unsigned k = 0; k < bl; k++) body[k] = bs[k];
                    body[bl] = '\0';
                    int erc = jash_eval_line(s, w, ctx, body);
                    sys_exit(erc);
                    for (;;) { }
                }
                /* Malformed subshell — fall through to execve which will fail */
            }

            /* Brace group path: segment is { body } — eval body in child.
             * Similar to subshell but uses { } instead of ( ). */
            if (segs[i].is_brace_group) {
                char brace_line[512];
                unsigned sl = 0;
                const char* sp = segs[i].cmd;
                while (*sp && sl + 1 < sizeof(brace_line)) brace_line[sl++] = *sp++;
                if (segs[i].args && *segs[i].args) {
                    brace_line[sl++] = ' ';
                    const char* ap = segs[i].args;
                    while (*ap && sl + 1 < sizeof(brace_line)) brace_line[sl++] = *ap++;
                }
                brace_line[sl] = '\0';
                /* Find matching } and extract body */
                if (sl > 1 && brace_line[0] == '{') {
                    char body[512];
                    const char* bs = brace_line + 1;
                    unsigned bl = sl - 1;
                    /* Trim leading whitespace */
                    while (bl > 0 && (*bs == ' ' || *bs == '\t')) { bs++; bl--; }
                    /* Find last } */
                    while (bl > 0 && (bs[bl-1] == ' ' || bs[bl-1] == '\t' || bs[bl-1] == ';')) bl--;
                    if (bl > 0 && bs[bl-1] == '}') bl--;
                    /* Trim trailing whitespace again */
                    while (bl > 0 && (bs[bl-1] == ' ' || bs[bl-1] == '\t')) bl--;
                    if (bl >= sizeof(body)) bl = sizeof(body) - 1;
                    for (unsigned k = 0; k < bl; k++) body[k] = bs[k];
                    body[bl] = '\0';
                    int erc = jash_eval_line(s, w, ctx, body);
                    sys_exit(erc);
                    for (;;) { }
                }
                /* Malformed brace group — fall through to execve which will fail */
            }

            /* Shell function path: segment is a user-defined function */
            if (segs[i].is_function && s->functions) {
                const char* fbody = jash_function_lookup(s->functions, segs[i].cmd);
                if (fbody) {
                    /* Set positional parameters from args.
                     * BUG-095: bound raised from 9 to JASH_POS_MAX. */
                    /* Set $1, $2, … from args. BUG-082: word-split FIRST
                     * (honoring quotes so `f "a b"` is ONE positional), THEN
                     * unquote each field. The old code unquoted the whole
                     * string first, collapsing "a b" to `a b` and splitting it
                     * into two positionals. */
                    int arg_count = 0;
                    char* new_pos[JASH_POS_MAX] = {0};
                    static char func_exp_args[PIPE_SEG_MAX];
                    const char* wp = seg_expand(segs[i].args ? segs[i].args : "",
                                                s, func_exp_args,
                                                sizeof func_exp_args);
                    if (jash_cmdsub_error() || jash_arith_error() ||
                        jash_param_expand_error()) {
                        sys_exit(1);
                    }
                    int parse_rc = jash_parse_function_positionals(wp, new_pos,
                                                                    &arg_count);
                    if (parse_rc != 0) {
                        if (parse_rc == -2)
                            jash_writes(jash_stderr_writer, 0,
                                        "jash: pipeline function: more than 384 positional parameters — refusing\n");
                        else
                            jash_writes(jash_stderr_writer, 0,
                                        "jash: pipeline function: unable to store positional parameters — refusing\n");
                        for (int pi = 0; pi < JASH_POS_MAX; pi++) free(new_pos[pi]);
                        sys_exit(1);
                    }
                    if (jash_env_replace_positionals_owned(s, new_pos,
                                                           arg_count) != 0) {
                        for (int pi = 0; pi < JASH_POS_MAX; pi++) free(new_pos[pi]);
                        sys_exit(1);
                    }

                    /* Evaluate function body in child process */
                    int erc = jash_eval_line(s, w, ctx, fbody);

                    sys_exit(erc);
                    for (;;) { }
                }
                /* Function not found — fall through to execve which will fail */
            }

            /* Compound command path: while/until/for/case/if/select */
            if (segs[i].is_compound) {
                /* Reconstruct full line from cmd + args.
                 * JINV-082: era `char full_line[1024]` com corte mudo. */
                char  fl_inline[1024];
                char* fl_heap = NULL;
                char* full_line = jash_compound_join(segs[i].cmd, segs[i].args,
                                                     fl_inline, sizeof(fl_inline),
                                                     &fl_heap);
                if (!full_line) {
                    jash_writef(w, ctx,
                                "%sjash: sem memoria para o enunciado composto — "
                                "NAO executado%s\n", J_ROLE_ERR, J_RESET);
                    sys_exit(2);
                    for (;;) { }
                }

                int erc;
                const char* cmd = segs[i].cmd;
                if (strcmp(cmd, "while") == 0 || strcmp(cmd, "until") == 0) {
                    erc = jash_eval_while_loop(s, w, ctx, full_line);
                    if (strcmp(cmd, "until") == 0) {
                        erc = erc ? 0 : 1;
                    }
                } else if (strcmp(cmd, "for") == 0) {
                    erc = jash_eval_for_loop(s, w, ctx, full_line);
                } else if (strcmp(cmd, "case") == 0) {
                    erc = jash_eval_case_stmt(s, w, ctx, full_line);
                } else if (strcmp(cmd, "if") == 0) {
                    erc = jash_eval_if_statement(s, w, ctx, full_line);
                } else if (strcmp(cmd, "select") == 0) {
                    erc = jash_eval_select_loop(s, w, ctx, full_line);
                } else if (strcmp(cmd, "[[") == 0) {
                    erc = jash_eval_line(s, w, ctx, full_line);
                } else {
                    erc = 1;
                }
                if (fl_heap) free(fl_heap);   /* JINV-082 */
                sys_exit(erc);
                for (;;) { }
            }

            /* External command path: build argv, use parent's pre-built envp. */

            char exp_args[16384];
            /* JINV-124: cmdsub → aritmética → parâmetro, uma regra só. */
            const char* final_args = seg_expand(segs[i].args, s, exp_args, sizeof(exp_args));

            int rc = build_argv_local(&child_ab, segs[i].fullpath, final_args);
            if (rc != 0) {
                const char* msg = "jash: argv build failed\n";
                sys_write(2, msg, my_strlen(msg));
                sys_exit(1);
            }

            sys_execve(segs[i].fullpath, child_ab.argv,
                       segs[i].is_absorbed ? s_envp_abs : s_envp);

            /* execve failed. */
            const char* msg = "jash: execve failed\n";
            sys_write(2, msg, my_strlen(msg));
            sys_exit(127);
            for (;;) { }
        }

        /* Parent continues to fork next child. The kernel's fork-yield
         * (schedule() in sys_fork) ensures the child gets CPU time. */
    }

    /* ---- PARENT ---- */

    int last_status = 0;
    int statuses[PIPE_MAX_CMDS];
    for (int i = 0; i < nseg; i++) statuses[i] = 0;

    if (lastpipe) {
        /* Run the last builtin segment in the parent process.
         * Set up stdin from the last pipe's read end. */
        int last = nseg - 1;
        int setup_failed = 0;
        if (last > 0) {
            long rc;
            do { rc = sys_dup2(pipe_fds[last - 1][0], STDIN_FILENO); }
            while (rc < 0 && errno == EINTR);
            if (rc < 0) {
                jash_writef(jash_stderr_writer, NULL, "jash: pipeline stdin: %s\n", strerror(errno));
                setup_failed = 1;
            }
            /* Update C library's stdin to reflect the dup2'd fd 0.
             * This is needed for builtins that use C stdio (fgets, stdin, etc.).
             *
             * JINV-095: era `stdin = fdopen(0, "r")` — um stream NOVO por
             * pipeline, com o anterior abandonado (vaza um FILE* + o buffer de
             * 4 KiB, por pipeline). Pior: era a única cópia de "o dup2 não
             * alcança o FILE*" no repositório, e os outros três sítios que
             * trocam o fd 0 debaixo de um builtin não tinham nenhuma — daí o
             * segundo `cmd < arquivo`, e todo heredoc a partir do segundo,
             * rodarem sem entrada. Agora é UMA função, em
             * c/parser/builtin_redir.c, e este sítio deixa de vazar. */
            jash_stdin_stream_sync(1);
        }

        /* Close all pipe fds in the parent (children have them dup'd). */
        for (int j = 0; j < nseg - 1; j++) {
            for (int end = 0; end < 2; end++) {
                int fd = pipe_fds[j][end];
                if (fd == STDIN_FILENO && !setup_failed) continue;
                if (sys_close(fd) < 0) setup_failed = 1;
            }
        }

        /* The last segment's prepared input must survive until apply_redirs. */
        close_prepared_fds(segs, last);

        if (!setup_failed && apply_redirs(&segs[last]) != 0) setup_failed = 1;
        close_prepared_fds(&segs[last], 1);
        if (setup_failed) {
            last_status = 1;
            } else if (segs[last].fullpath[0] != '\0' && !segs[last].is_builtin && !segs[last].is_function) {
                /* External command in lastpipe: spawn directly (no fork). */
                char exp_args[16384];
                /* JINV-124: cmdsub → aritmética → parâmetro, uma regra só. */
                const char* sp_args = seg_expand(segs[last].args, s, exp_args, sizeof(exp_args));
                static jash_argvbuf_t l_ab;
                if (build_argv_local(&l_ab, segs[last].fullpath, sp_args) == 0) {
                    posix_spawn_file_actions_t fa;
                    posix_spawnattr_t          sa;
                    posix_spawn_file_actions_init(&fa);
                    posix_spawnattr_init(&sa);
                    sigset_t defsigs;
                    sigemptyset(&defsigs);
                    sigaddset(&defsigs, SIGINT);
                    sigaddset(&defsigs, SIGQUIT);
                    posix_spawnattr_setsigdefault(&sa, &defsigs);
                    posix_spawnattr_setflags(&sa, POSIX_SPAWN_SETSIGDEF);
                    pid_t spid = (pid_t)-1;
                    int spawn_rc = posix_spawn(&spid, segs[last].fullpath,
                                               &fa, &sa, l_ab.argv,
                                               segs[last].is_absorbed
                                                   ? s_envp_abs : s_envp);
                    posix_spawn_file_actions_destroy(&fa);
                    posix_spawnattr_destroy(&sa);
                    if (spawn_rc == 0) {
                        int wstatus = 0;
                        sys_waitpid((long)spid, &wstatus, 0);
                        last_status = decode_wait_status(wstatus);
                    } else {
                        if (spawn_rc == ENOENT || spawn_rc == ENOTDIR)
                            jash_path_cache_invalidate(s, segs[last].cmd);
                        last_status = 1;
                    }
                } else {
                    last_status = 1;
                }
                statuses[last] = last_status;
            } else {
                /* Compound command path: while/until/for/case/if/select */
                if (segs[last].is_compound) {
                    /* JINV-082: era `char full_line[1024]` com corte mudo. */
                    char  fl_inline[1024];
                    char* fl_heap = NULL;
                    char* full_line = jash_compound_join(segs[last].cmd, segs[last].args,
                                                         fl_inline, sizeof(fl_inline),
                                                         &fl_heap);
                    if (!full_line) {
                        jash_writef(w, ctx,
                                    "%sjash: sem memoria para o enunciado composto — "
                                    "NAO executado%s\n", J_ROLE_ERR, J_RESET);
                        statuses[last] = 2;
                        last_status = 2;
                        goto compound_join_oom;
                    }

                    const char* cmd = segs[last].cmd;
                    if (strcmp(cmd, "while") == 0 || strcmp(cmd, "until") == 0) {
                        last_status = jash_eval_while_loop(s, w, ctx, full_line);
                    if (strcmp(cmd, "until") == 0) {
                        last_status = last_status ? 0 : 1;
                    }
                } else if (strcmp(cmd, "for") == 0) {
                    last_status = jash_eval_for_loop(s, w, ctx, full_line);
                } else if (strcmp(cmd, "case") == 0) {
                    last_status = jash_eval_case_stmt(s, w, ctx, full_line);
                } else if (strcmp(cmd, "if") == 0) {
                    last_status = jash_eval_if_statement(s, w, ctx, full_line);
                } else if (strcmp(cmd, "select") == 0) {
                    last_status = jash_eval_select_loop(s, w, ctx, full_line);
                } else {
                    last_status = 1;
                }
                if (fl_heap) free(fl_heap);   /* JINV-082 */
                statuses[last] = last_status;
compound_join_oom: ;                          /* JINV-082: recusa alta */
            } else {
                /* Expand variables in args for this segment.
                 * Global expansion was skipped for pipelines with compound commands. */
                char exp_args[16384];
                /* JINV-124: cmdsub → aritmética → parâmetro, uma regra só. */
                const char* final_args = seg_expand(segs[last].args, s, exp_args, sizeof(exp_args));
                if (final_args) {
                    /* JINV-090: idem ao builtin forkado acima — este caminho
                     * (builtin do ÚLTIMO segmento, no processo pai) também não
                     * passa por build_argv_local. Restauro aqui, depois da
                     * expansão deste segmento. */
                    { extern void jash_cmdsub_unprotect(char* s);
                      jash_cmdsub_unprotect((char*)final_args); }   /* JINV-090 */
                }
                
                /* Quote removal for builtins (parity with eval.c + forked
                 * builtin path). Without this, `echo a | grep -c "."`
                 * passes `"."` literal to grep instead of `.`.
                 * EXCEPTION: printf, set, and grep do their own quote-aware
                 * tokenization — `grep "No such file" file` must preserve the
                 * pattern as one arg, which generic unquote destroys. */
                extern int jash_unquote(const char* in, char* out, size_t out_size);
                extern int jash_has_quotes(const char* s);
                char unq_args[16384];
                /* ADR-0036 W2: argv_fn-migrated builtins always get raw args. */
                int skip_unquote = (segs[last].builtin && segs[last].builtin->argv_fn != NULL) ||
                                   (strcmp(segs[last].cmd, "printf") == 0) ||
                                   (strcmp(segs[last].cmd, "set") == 0) ||
                                   (strcmp(segs[last].cmd, "trap") == 0) ||
                                   (strcmp(segs[last].cmd, "builtin") == 0) ||
                                   (strcmp(segs[last].cmd, "command") == 0) ||
                                   (strcmp(segs[last].cmd, "grep") == 0) ||
                                   (strcmp(segs[last].cmd, "mesh") == 0) ||
                                   (strcmp(segs[last].cmd, "contract") == 0) ||
                                   (strcmp(segs[last].cmd, "cost") == 0) ||
                                   (strcmp(segs[last].cmd, "revert") == 0) ||
                                   /* BUG-151: raw tail → quote-aware exec. */
                                   (strcmp(segs[last].cmd, "env") == 0);
                if (!skip_unquote && final_args && jash_has_quotes(final_args)) {
                    int unq_len = jash_unquote(final_args, unq_args, sizeof(unq_args));
                    if (unq_len >= 0) final_args = unq_args;
                }
                /* Call the builtin directly in the parent. */
                if (!segs[last].builtin) {
                    /* BUG-157 defense: should be unreachable after the
                     * classification reset, but never dispatch a NULL. */
                    jash_writef(w, ctx, "jash: %s: not a builtin\n", segs[last].cmd);
                    last_status = 127;
                } else if (jash_guard_bypass_detect(segs[last].cmd, final_args, w, ctx)) {
                    last_status = 126;
                } else {
                    last_status = jash_builtin_dispatch(segs[last].builtin, s, w, ctx, final_args);
                }
                statuses[last] = last_status;
            }
        }

        /* Nested exec may have moved the backups; restore through their live
         * registry entries, never stale fd numbers captured before dispatch. */
        if (jash_redir_snapshot_end(lastpipe_saved, 1) && last_status == 0)
            last_status = 1;
        statuses[last] = last_status;

        /* Wait for all forked children. */
        int remaining = fork_count;
        for (int i = 0; i < fork_count; i++) {
            if (pids[i] < 0) remaining--;
        }
        while (remaining > 0) {
            int status = 0;
            long wr = sys_waitpid((uint64_t)-1, &status, 0);
            if (wr < 0) break;
            for (int i = 0; i < fork_count; i++) {
                if (pids[i] == wr) {
                    statuses[i] = decode_wait_status(status);
                    pids[i] = -1;
                    remaining--;
                    break;
                }
            }
        }
    } else {
        /* Close all pipe fds in the parent. */
        for (int j = 0; j < nseg - 1; j++) {
            sys_close(pipe_fds[j][0]);
            sys_close(pipe_fds[j][1]);
        }

        /* Close any prepared (heredoc/here-string) fds (children have them dup'd). */
        close_prepared_fds(segs, nseg);

        /* Wait for all children. */
        int remaining = nseg;
        for (int i = 0; i < nseg; i++) {
            if (pids[i] < 0) remaining--;
        }
        while (remaining > 0) {
            int status = 0;
            long wr = sys_waitpid((uint64_t)-1, &status, 0);
            if (wr < 0) break;
            for (int i = 0; i < nseg; i++) {
                if (pids[i] == wr) {
                    int decoded = decode_wait_status(status);
                    statuses[i] = decoded;
                    if (i == nseg - 1) last_status = decoded;
                    pids[i] = -1;
                    remaining--;
                    break;
                }
            }
        }
    }

    /* No stat on normal PATH-cache hits. Only an exec-style 127 asks whether
     * the resolved file disappeared, then poisons that cache entry so the
     * next invocation can discover a later PATH candidate. */
    for (int i = 0; i < nseg; i++) {
        if (statuses[i] != 127 || segs[i].fullpath[0] == '\0' ||
            segs[i].is_builtin || segs[i].is_function ||
            segs[i].is_compound || strchr(segs[i].cmd, '/'))
            continue;
        struct stat st;
        if (stat(segs[i].fullpath, &st) != 0 || !S_ISREG(st.st_mode))
            jash_path_cache_invalidate(s, segs[i].cmd);
    }

    /* Store exit codes in PIPESTATUS array */
    if (s) {
        /* Clear only the entries from the previous run (tracked by nseg). */
        for (int i = 0; i < nseg; i++) {
            char key[32];
            snprintf(key, sizeof(key), "PIPESTATUS[%d]", i);
            jash_env_unset(s, key);
        }
        /* Set new PIPESTATUS entries */
        for (int i = 0; i < nseg; i++) {
            char key[32];
            char val[16];
            snprintf(key, sizeof(key), "PIPESTATUS[%d]", i);
            snprintf(val, sizeof(val), "%d", statuses[i]);
            jash_env_set(s, key, val);
        }
    }

    if (s && s->shell_opts.pipefail) {
        for (int i = nseg - 1; i >= 0; i--) {
            if (statuses[i] != 0) return statuses[i];
        }
    }
    return last_status;
}

/* ---- pipeline detection ---- */

/* BUG-069: `|`/`<`/`>` inside an expansion — `$((a|b))` (arithmetic),
 * `$(cmd|cmd)` (cmdsub), `${v//|/x}` (param), `` `cmd|cmd` `` — are NOT
 * line-level pipeline/redirect operators. Track expansion depth so only the
 * top level counts. Returns 1 at the first genuine `|` (pipe) resp. `<`/`>`
 * (redirect). Same context-blindness class as BUG-060. F E R M I ∞ H A R T */
/* JINV-117: esta função tinha a sua PRÓPRIA máquina de aspas, e ela fechava a
 * aspa dupla num \" escapado. `echo "a\">x"` era lido como redirect e o shell
 * criava o arquivo `x` sem que ninguém tivesse pedido. Agora delega para o
 * único scanner (c/parser/quotes.c) — a regra da aspa dupla mora num lugar só. */
static int line_has_op(const char* line, int want_pipe) {
    if (!want_pipe) return *jash_scan_unquoted(line, "><") != 0;
    const char* cursor = line;
    for (;;) {
        const char* hit = jash_scan_unquoted(cursor, "|>");
        if (!*hit) return 0;
        if (*hit == '|') return 1;
        /* Consume the whole clobber operator. An escaped/quoted '>' is never
         * returned by the scanner, so its following real pipe stays visible. */
        cursor = hit + (hit[1] == '|' ? 2 : 1);
    }
}

int jash_line_has_pipe(const char* line)  { return line_has_op(line, 1); }
int jash_line_has_redir(const char* line) { return line_has_op(line, 0); }
