// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Module: parser/builtin_redir — run a builtin with redirects applied
// IN-PROCESS (no fork). Because there is no child to discard, every fd we
// touch is saved to a high fd and restored after the builtin returns.
//
// Supports the same surface as the pipeline redirect engine:
//   [n]<file  [n]>file  [n]>>file  [n]<>file
//   [n]>&m    [n]<&m    [n]>&-     [n]<&-    2>&1    &>file

#include "eval.h"
#include "quotes.h"
#include "cmdsub.h"
#include "builtin_redir.h"
#include <builtins/builtin.h>
#include <core/timeline.h>
#include <sys/syscalls.h>
#include <fcntl.h>
#include <stdio.h>
#include <function_defs.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <limits.h>
#ifndef JASH_AGENT_ONLY
#include <module/loader.h>
#endif
#if defined(__GLIBC__)
#include <stdio_ext.h>          /* __fpurge — JINV-095 */
#endif

/* ═══ JINV-095 — trocar o fd 0 debaixo de um builtin NÃO troca o `stdin` ═══
 *
 * ═══ O DEFEITO, medido ═════════════════════════════════════════════════
 *
 * **Só o PRIMEIRO `cmd < arquivo` de cada processo funcionava, quando `cmd`
 * era builtin.** Do segundo em diante o comando rodava com entrada VAZIA, em
 * silêncio e com rc 0:
 *
 *     cat < f1        bash: aaa bbb      jash: aaa            (bbb sumiu)
 *     cat < f2
 *
 *     wc -c < f1      bash:   4    4     jash:   4    0       (zero, nao erro)
 *     wc -c < f2
 *
 * O sintoma que denunciou foi outro, e muito pior de diagnosticar: **só o
 * primeiro heredoc de cada script era entregue.** Um heredoc vira
 * `cmd < /tmp/jash_heredoc_XXXX` no pré-processamento (c/main.c), então TODO
 * heredoc a partir do segundo caía neste mesmo buraco. O corpo estava no
 * arquivo, correto e inteiro — medido byte a byte —; o que não acontecia era
 * a LEITURA. Um `configure` de autoconf tem centenas de heredocs.
 *
 * ═══ A CAUSA ═══════════════════════════════════════════════════════════
 *
 * `jash_eval_builtin_with_redir` roda o builtin NO PROCESSO DO SHELL: salva o
 * fd alvo num fd alto, faz `dup2(arquivo, 0)`, chama o builtin, restaura. O fd
 * está certo o tempo todo. O que ninguém trocou foi o `FILE*` do C — e é ele,
 * não o fd, que `cat`, `wc`, `grep`, `sed`, `awk`, `xargs` e `mapfile` usam
 * (`fread(..., stdin)`, `fgets(..., stdin)`).
 *
 * Um `FILE*` guarda estado que o `dup2` não alcança:
 *
 *   · `__SEOF` LATCHED — depois que o primeiro builtin lê `f1` até o fim, o
 *     sinalizador de fim-de-arquivo fica ACESO no stream. `fread` passa a
 *     devolver 0 IMEDIATAMENTE, sem nunca mais encostar no fd 0. É por isso
 *     que o segundo `cat` não lê `f2`: ele não lê NADA, para sempre, até o
 *     processo morrer. Daí o escopo do defeito ser por INVOCAÇÃO, não por
 *     script nem por linha.
 *
 *   · BUFFER DE LEITURA ANTECIPADA — o stdio lê em blocos. Bytes lidos a mais
 *     do redirecionamento A ficam no buffer e seriam servidos ao comando B.
 *     Dado de um arquivo aparecendo na entrada de outro comando é a mesma
 *     família de "fronteira dado/estrutura perdida" do JINV-016/020/092.
 *
 * Comando EXTERNO nunca sofreu: ele nasce com stdio virgem. Por isso o
 * defeito era invisível para quem testasse com `/bin/cat`.
 *
 * ═══ POR QUE UMA FUNÇÃO, E NÃO UM CONSERTO NO SÍTIO ════════════════════
 *
 * `c/parser/pipeline.c` já tinha topado com isto e resolvia INLINE, com
 * `stdin = fdopen(0, "r")` — um stream novo a cada `lastpipe`, e o antigo
 * abandonado (vazamento de um `FILE*` + buffer de 4 KiB por pipeline). Era a
 * quarta cópia do mesmo problema esperando para nascer nos outros três sítios
 * de troca de fd 0. Esta casa achou ONZE vezes o padrão "duas implementações
 * da mesma coisa, uma empobrecida"; a onda anterior (JINV-092) removeu duas em
 * vez de criar a terceira. Aqui vale o mesmo: UMA função, e todo sítio que
 * troca o fd 0 debaixo de um builtin passa por ela — inclusive o pipeline,
 * que de quebra deixa de vazar.
 *
 * `drop_buffered`:
 *   0 — só apaga EOF/erro. Use ANTES de aplicar o redirecionamento: o que
 *       estiver no buffer ainda pertence ao stdin REAL do shell e jogá-lo
 *       fora seria perder entrada do usuário.
 *   1 — apaga EOF/erro E descarta o buffer. Use DEPOIS do builtin e ANTES de
 *       restaurar o fd: o que sobrou no buffer foi lido do arquivo do
 *       redirecionamento e não pode vazar para o próximo comando.
 *
 * Não usa `fseek` de propósito: no stdio BSD um `fseek` acende `__SOFF` e
 * passa a CONFIAR num deslocamento em cache — que fica velho no instante em
 * que um `dup2` troca o arquivo debaixo do fd. O conserto viraria um bug de
 * posição errada, que é pior que o que ele conserta.
 *
 * Gate: tests/test_jinv095_multiple_heredocs.sh */
/* JINV/BUG-066: fd de salvamento ALOCADO, nunca base fixa.
 *
 * Era `sys_dup2(tgt, 50 + nsaved)`. Com base fixa, dois niveis de redirect
 * aninhados escolhem os MESMOS fds e o de dentro destroi a copia do de fora.
 * Medido por strace em 2026-08-05, no caso
 *     source arquivo >/dev/null 2>&1      (arquivo contem `cmd 2>/dev/null`)
 *
 *     dup2(1, 50)   externo salva stdout em 50
 *     dup2(3, 1)    stdout -> /dev/null
 *     dup2(2, 50)   INTERNO salva stderr em 50  <- destroi a copia do stdout
 *     dup2(50, 2)   interno restaura
 *     (nunca acontece dup2(50, 1))
 *
 * stdout ficava em /dev/null pelo resto da sessao. E o idioma que TODO harness
 * de agente usa para abrir um comando, entao o efeito era saida VAZIA com rc 0
 * — o comando nunca rodava e o vazio lia como sucesso.
 *
 * F_DUPFD_CLOEXEC allocates backups; redirection operands must also be excluded
 * from those allocations, including when a nested command reuses a backup fd. */
void jash_stdin_stream_sync(int drop_buffered) {
    if (drop_buffered) {
#if defined(__GLIBC__)
        __fpurge(stdin);
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__NetBSD__) || \
      defined(__OpenBSD__) || defined(__DragonFly__) || defined(BSD)
        fpurge(stdin);
#endif
        /* Sem primitiva de purga na libc alvo o buffer sobrevive; o EOF
         * latched — o defeito medido — morre de qualquer jeito no clearerr. */
    }
    clearerr(stdin);
}

/* ── REQ-4: buffered stdout for file-redirected builtins ──────────────────────
 * jash_stdout_writer does one raw sys_write() per call. `export -p` emits ~6
 * jash_writes() per variable (plus one per quoted char), so `export -p > file`
 * fired hundreds of tiny writes into an O_TRUNC'd file — a concurrent reader
 * could catch a half-written line (export→xport, x86_64→86_64). We accumulate
 * the builtin's stdout and flush it as a SINGLE write, matching bash's block
 * flush and closing the torn-read window for ANY consumer. */
typedef struct {
    char*  buf;
    char*  heap;
    size_t len;
    size_t cap;
    int    degraded;   /* direct streaming: OOM, wrapper, or non-file target */
    int    error;
    int    fd;
} br_obuf_t;

static int br_write_all(int fd, const char* data, size_t n) {
    while (n) {
        size_t chunk = n > (size_t)LONG_MAX ? (size_t)LONG_MAX : n;
        long written = sys_write(fd, data, (unsigned long)chunk);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) {
            if (!written) errno = EIO;
            return -1;
        }
        data += written;
        n -= (size_t)written;
    }
    return 0;
}

static void br_report_error(const char* subject, int err) {
    char msg[512];
    int n = snprintf(msg, sizeof msg, "jash: %s: %s\n",
                     subject ? subject : "redirection", strerror(err));
    if (n > 0) {
        size_t len = (size_t)n < sizeof msg ? (size_t)n : sizeof msg - 1;
        if (br_write_all(2, msg, len) < 0) return;
    }
}

static long br_obuf_write(void* ctx, const char* data, size_t n) {
    br_obuf_t* b = (br_obuf_t*)ctx;
    if (b->error) { errno = b->error; return -1; }
    if (n > (size_t)LONG_MAX || n > SIZE_MAX - b->len) {
        b->error = errno = EOVERFLOW;
        return -1;
    }
    if (b->degraded) goto stream;
    if (b->len + n > b->cap) {
        size_t ncap = b->cap ? b->cap : 8192;
        while (ncap < b->len + n) {
            if (ncap > SIZE_MAX / 2) { ncap = b->len + n; break; }
            ncap *= 2;
        }
        char* nb = (char*)realloc(b->heap, ncap);
        if (!nb) {
            /* OOM: flush what we have, then stream directly. Correctness
             * (no data loss) wins over atomicity in this rare path. */
            if (br_write_all(b->fd, b->buf, b->len) < 0) {
                b->error = errno;
                return -1;
            }
            b->len = 0;
            b->degraded = 1;
            goto stream;
        }
        if (!b->heap && b->len) memcpy(nb, b->buf, b->len);
        b->heap = nb;
        b->buf = nb; b->cap = ncap;
    }
    memcpy(b->buf + b->len, data, n);
    b->len += n;
    return (long)n;
stream:
    if (b->len) {
        if (br_write_all(b->fd, b->buf, b->len) < 0) { b->error = errno; return -1; }
        b->len = 0;
    }
    if (br_write_all(b->fd, data, n) < 0) { b->error = errno; return -1; }
    return (long)n;
}

void jash_redir_stream_writer(jash_writer_t writer, void* context) {
    if (writer == br_obuf_write && context) ((br_obuf_t*)context)->degraded = 1;
}

static unsigned my_strlen(const char* s) {
    unsigned n = 0; while (s[n]) n++; return n;
}

static const char* skip_ws(const char* p) {
    while (*p == ' ' || *p == '\t') p++;
    return p;
}

/* Strip first token (cmd name) from line. Returns pointer to args. */
static const char* skip_first_token(const char* line) {
    const char* p = skip_ws(line);
    while (*p && *p != ' ' && *p != '\t') p++;
    return skip_ws(p);
}

/* ---- redirect op model (mirrors pipeline.c) ---- */

#define BR_MAX_REDIRS 8

enum {
    BR_FILE_IN = 1, BR_FILE_OUT, BR_FILE_APPEND, BR_FILE_RW,
    BR_DUP, BR_CLOSE, BR_HERESTRING, BR_FILE_CLOBBER,
};

typedef struct {
    int  kind;
    int  target_fd;
    int  src_fd;
    char fname[256];
} br_redir_t;

static const char* read_fname(const char* r, char* out, unsigned cap) {
    r = skip_ws(r);
    unsigned fi = 0;
    char quote = 0;
    while (*r) {
        if (!quote && (*r == ' ' || *r == '\t' || *r == '\n' ||
                       *r == ';'  || *r == '|'  || *r == '&')) break;
        /* JINV-098/116: remoção de aspas = parser/quotes.c. */
        char em = 0;
        const char* q = jash_unquote_step(r, &quote, &em);
        if (q) { if (em && fi + 1 < cap) out[fi++] = em; r = q; continue; }
        if (fi + 1 < cap) out[fi++] = *r;
        r++;
    }
    out[fi] = '\0';
    /* Filename boundaries are final: protected characters are now path data,
     * not operators or another round of expansion (same as the pipeline). */
    jash_cmdsub_unprotect(out);
    jash_quote_unshield(out);
    return r;
}

static void br_set_fname(br_redir_t* o, const char* fname) {
    unsigned i = 0;
    while (fname[i] && i + 1 < sizeof(o->fname)) { o->fname[i] = fname[i]; i++; }
    o->fname[i] = '\0';
}

/* Parse line for redirect tokens. Mutates clean_args with tokens stripped.
 * Fills redirs[]/nredir. Quote-aware. */
static int parse_redirs_inplace(const char* args, char* clean_args,
                                 size_t cap,
                                 br_redir_t* redirs, int* out_nredir) {
    int nr = 0;
    const char* r = args;
    char* w = clean_args;
    char* end = clean_args + cap - 1;
    char quote = 0;
    int word_start = 1;
    int copy_spans = strlen(args) >= 256;

    while (*r && w < end) {
        /* Within quotes, ordinary bytes cannot be redirect operators. Copy
         * whole runs; delimiters and escapes still use the shared quote rule. */
        if (quote && copy_spans) {
            size_t span = strcspn(r, quote == '\'' ? "'" : "\"\\");
            if (span) {
                if (span > (size_t)(end - w)) { errno = E2BIG; return -1; }
                memcpy(w, r, span);
                w += span;
                r += span;
                word_start = 0;
                continue;
            }
        }
        /* JINV-098/116: regra de aspas = parser/quotes.c. Este laço copia
         * verbatim, então copia exatamente os bytes que o passo consumiu.
         * A cópia local não via \" dentro de aspa dupla: `> "a\"b"` fazia o
         * scanner achar que a string tinha fechado e o resto virava redirect. */
        { const char* q = jash_quote_step(r, &quote);
          if (q) {
              if (quote || *r != '\\' || r[1] != '\n') word_start = 0;
              while (r < q && w < end) *w++ = *r++;
              r = q;
              continue;
          } }

        /* Only a wholly unquoted numeric word is an fd prefix, not x2 or
         * digits following quoted/escaped whitespace within the same word. */
        int explicit_fd = -1;
        if (word_start && *r >= '0' && *r <= '9') {
            const char* d = r;
            while (*d >= '0' && *d <= '9') d++;
            if (*d == '<' || *d == '>') {
                int val = 0;
                while (r < d) {
                    if (val > (INT_MAX - (*r - '0')) / 10) {
                        errno = EBADF;
                        return -1;
                    }
                    val = val * 10 + (*r++ - '0');
                }
                explicit_fd = val;
            }
        }

        if ((*r == '<' || *r == '>' || (*r == '&' && r[1] == '>')) &&
            nr == BR_MAX_REDIRS) { errno = E2BIG; return -1; }

        /* &>file / &>>file */
        if (explicit_fd < 0 && *r == '&' && r[1] == '>') {
            word_start = 1;
            if (nr + 2 > BR_MAX_REDIRS) { errno = E2BIG; return -1; }
            r += 2;
            int append = 0;
            if (*r == '>') { append = 1; r++; }
            char fname[256];
            r = read_fname(r, fname, sizeof(fname));
            {
                br_redir_t* o = &redirs[nr++];
                o->kind = append ? BR_FILE_APPEND : BR_FILE_OUT; o->target_fd = 1; o->src_fd = -1;
                br_set_fname(o, fname);
                br_redir_t* e = &redirs[nr++];
                e->kind = BR_DUP; e->target_fd = 2; e->src_fd = 1; e->fname[0] = '\0';
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
                r++;
                if (*r == '-') { r++; if (nr < BR_MAX_REDIRS) { redirs[nr].kind = BR_CLOSE; redirs[nr].target_fd = target; redirs[nr].src_fd = -1; redirs[nr].fname[0] = '\0'; nr++; } continue; }
                if (*r >= '0' && *r <= '9') {
                    int src = 0;
                    while (*r >= '0' && *r <= '9') {
                        if (src > (INT_MAX - (*r - '0')) / 10) { errno = EBADF; return -1; }
                        src = src * 10 + (*r++ - '0');
                    }
                    if (nr < BR_MAX_REDIRS) { redirs[nr].kind = BR_DUP; redirs[nr].target_fd = target; redirs[nr].src_fd = src; redirs[nr].fname[0] = '\0'; nr++; }
                    continue;
                }
                char fname[256];
                r = read_fname(r, fname, sizeof(fname));
                if (nr + 2 > BR_MAX_REDIRS) { errno = E2BIG; return -1; }
                {
                    br_redir_t* o = &redirs[nr++];
                    o->kind = append ? BR_FILE_APPEND : BR_FILE_OUT; o->target_fd = 1; o->src_fd = -1;
                    br_set_fname(o, fname);
                    br_redir_t* e = &redirs[nr++];
                    e->kind = BR_DUP; e->target_fd = 2; e->src_fd = 1; e->fname[0] = '\0';
                }
                continue;
            }
            char fname[256];
            r = read_fname(r, fname, sizeof(fname));
            {
                br_redir_t* o = &redirs[nr++];
                o->kind = clobber ? BR_FILE_CLOBBER : (append ? BR_FILE_APPEND : BR_FILE_OUT);
                o->target_fd = target; o->src_fd = -1;
                br_set_fname(o, fname);
            }
            continue;
        }

        if (*r == '<') {
            word_start = 1;
            int target = (explicit_fd >= 0) ? explicit_fd : 0;
            r++;
            if (*r == '>') {
                r++;
                char fname[256];
                r = read_fname(r, fname, sizeof(fname));
                {
                    br_redir_t* o = &redirs[nr++];
                    o->kind = BR_FILE_RW; o->target_fd = target; o->src_fd = -1;
                    br_set_fname(o, fname);
                }
                continue;
            }
            /* here-string <<<word  OR  heredoc <<DELIM\n...BODY...\nDELIM
             * On entry: outer `if (*r == '<') r++` consumed the first '<',
             * so r points at the SECOND '<' of '<<'. A third '<' (r+1)
             * means here-string; anything else is a heredoc. The previous
             * code conflated both and fed `read x <<EOF\\ntestvalue\\nEOF`
             * the bytes "EOF\\n" instead of "testvalue\\n". */
            if (*r == '<') {
                int indented = 0;
                int is_herestring = (*(r+1) == '<');
                r += is_herestring ? 2 : 1;
                if (!is_herestring && *r == '-') { indented = 1; r++; }
                char content[1024];
                if (is_herestring) {
                    r = read_fname(r, content, sizeof(content));
                    /* bash appends trailing newline to here-strings */
                    unsigned cl = my_strlen(content);
                    if (cl + 2 < sizeof(content)) { content[cl] = '\n'; content[cl+1] = '\0'; }
                } else {
                    /* Heredoc: read delimiter token, then collect lines from
                     * the rest of the same logical input until the delimiter
                     * appears on its own line. The caller has stitched the
                     * whole script into `line`, so `r` is positioned right
                     * before the delimiter; the body follows the next \\n. */
                    char delim[128];
                    r = read_fname(r, delim, sizeof(delim));
                    unsigned dlen = my_strlen(delim);
                    /* Skip to body: first newline after the delimiter token. */
                    while (*r && *r != '\n') r++;
                    if (*r == '\n') r++;
                    unsigned ci = 0;
                    while (*r && ci + 1 < sizeof(content)) {
                        const char* line_start = r;
                        while (*r && *r != '\n') r++;
                        unsigned llen = (unsigned)(r - line_start);
                        /* Tab-strip for <<- */
                        const char* eff = line_start; unsigned elen = llen;
                        if (indented) {
                            while (elen > 0 && (*eff == '\t')) { eff++; elen--; }
                        }
                        /* Closing delimiter line? */
                        if (elen == dlen) {
                            unsigned same = 1;
                            for (unsigned i = 0; i < dlen; i++)
                                if (eff[i] != delim[i]) { same = 0; break; }
                            if (same) { if (*r == '\n') r++; break; }
                        }
                        for (unsigned i = 0; i < llen && ci + 1 < sizeof(content); i++)
                            content[ci++] = line_start[i];
                        if (ci + 1 < sizeof(content)) content[ci++] = '\n';
                        if (*r == '\n') r++;
                    }
                    content[ci] = '\0';
                }
                {
                    br_redir_t* o = &redirs[nr++];
                    o->kind = BR_HERESTRING; o->target_fd = target; o->src_fd = -1;
                    br_set_fname(o, content);  /* fname holds the heredoc/herestring content */
                }
                continue;
            }
            if (*r == '&') {
                r++;
                if (*r == '-') { r++; if (nr < BR_MAX_REDIRS) { redirs[nr].kind = BR_CLOSE; redirs[nr].target_fd = target; redirs[nr].src_fd = -1; redirs[nr].fname[0] = '\0'; nr++; } continue; }
                if (*r >= '0' && *r <= '9') {
                    int src = 0;
                    while (*r >= '0' && *r <= '9') {
                        if (src > (INT_MAX - (*r - '0')) / 10) { errno = EBADF; return -1; }
                        src = src * 10 + (*r++ - '0');
                    }
                    if (nr < BR_MAX_REDIRS) { redirs[nr].kind = BR_DUP; redirs[nr].target_fd = target; redirs[nr].src_fd = src; redirs[nr].fname[0] = '\0'; nr++; }
                    continue;
                }
                errno = EBADF;
                return -1;
            }
            char fname[256];
            r = read_fname(r, fname, sizeof(fname));
            {
                br_redir_t* o = &redirs[nr++];
                o->kind = BR_FILE_IN; o->target_fd = target; o->src_fd = -1;
                br_set_fname(o, fname);
            }
            continue;
        }

        size_t span = copy_spans ? strcspn(r, " \t\n'\"\\<>&") : 0;
        if (span) {
            if (span > (size_t)(end - w)) { errno = E2BIG; return -1; }
            memcpy(w, r, span);
            w += span;
            r += span;
            word_start = 0;
            continue;
        }
        word_start = *r == ' ' || *r == '\t' || *r == '\n';
        *w++ = *r++;
    }
    /* BUG-039 class-kill: the `\`-escape branch writes TWO bytes while the loop
     * guard (w < clean_args+cap-1) reserved one, so w can reach clean_args+cap —
     * clamp so the terminator never writes clean_args[cap]. */
    if (w > end) w = end;   /* end = clean_args + cap - 1 */
    *w = '\0';

    /* Trim trailing whitespace from clean_args (quote-aware, JINV-063). */
    w = clean_args + jash_rtrim_unquoted(clean_args);
    *w = '\0';
    *out_nredir = nr;
    return 0;
}

/* ADR-0050 W1: journal a write redirect before it fires, unless noclobber
 * guarantees the open will fail (existing file + plain `>`). */
static void br_journal(const char* fname, int append, int clobber,
                       const jash_state_t* s) {
    if (!fname || !*fname) return;
    if (!append && !clobber && s && s->shell_opts.noclobber) {
        struct stat st;
        if (stat(fname, &st) == 0) return;   /* open will fail: no mutation */
    }
    jash_timeline_journal_redirect(fname, append);
}

struct jash_redir_snapshot {
    size_t count;
    int sync_stdin;
    int* operands;
    size_t operand_count;
    struct jash_redir_snapshot* previous;
    struct { int fd, target, flags; } entries[];
};

static jash_redir_snapshot_t* br_active;

/* Do not turn a closed user operand into an open fd merely by saving another
 * target. Also exclude enclosing scopes' operands so restoration stays safe. */
static int br_dup_private(int fd, const int* operands, size_t count) {
    int minimum = 10;
    for (;;) {
        int copy = fcntl(fd, F_DUPFD_CLOEXEC, minimum);
        if (copy < 0) {
            if (errno == EINTR) continue;
            return -1;
        }
        int reserved = 0;
        for (size_t i = 0; i < count; i++)
            if (operands[i] == copy) { reserved = 1; break; }
        for (jash_redir_snapshot_t* p = br_active; p && !reserved; p = p->previous)
            for (size_t i = 0; i < p->operand_count; i++)
                if (p->operands[i] == copy) { reserved = 1; break; }
        if (!reserved) return copy;
        if (sys_close(copy) < 0) return -1;
        if (copy == INT_MAX) { errno = EMFILE; return -1; }
        minimum = copy + 1;
    }
}

static int br_dup2(int source, int target) {
    long rc;
    do { rc = sys_dup2(source, target); } while (rc < 0 && errno == EINTR);
    return rc < 0 ? -1 : 0;
}

static int br_close(int fd, int allow_closed) {
    if (sys_close(fd) == 0 || (allow_closed && errno == EBADF)) return 0;
    /* Do not retry close: on Linux even EINTR releases the descriptor. */
    br_report_error("redirection close", errno);
    return 1;
}

int jash_redir_reserve_fds(const int* operands, size_t count) {
    /* A nested command must not overwrite an enclosing backup or use one as
     * a source. Pipeline callers need the same protection before saving fds. */
    for (jash_redir_snapshot_t* p = br_active; p; p = p->previous) {
        for (size_t i = 0; i < p->count; i++) {
            int old = p->entries[i].fd;
            if (old < 0) continue;
            size_t j = 0;
            while (j < count && operands[j] != old) j++;
            if (j == count) continue;
            int copy = br_dup_private(old, operands, count);
            if (copy < 0) { br_report_error("redirection save", errno); return 1; }
            p->entries[i].fd = copy;
            if (br_close(old, 0)) return 1;
        }
    }
    return 0;
}

static int br_flush_stdio(void) {
    int err = 0;
    if (fflush(stdout) == EOF) err = errno ? errno : EIO;
    else if (ferror(stdout)) err = EIO;
    if (fflush(stderr) == EOF && !err) err = errno ? errno : EIO;
    else if (ferror(stderr) && !err) err = EIO;
    clearerr(stdout);
    clearerr(stderr);
    if (err) br_report_error("redirection flush", err);
    return err != 0;
}

/* restore=0 commits a permanent exec or discards an unused snapshot. */
int jash_redir_snapshot_end(jash_redir_snapshot_t* saved, int restore) {
    if (!saved || saved != br_active) {
        br_report_error("redirection snapshot order", EINVAL);
        return 1;
    }
    int failed = restore ? br_flush_stdio() : 0;
    if (saved->sync_stdin) jash_stdin_stream_sync(1);
    for (size_t remaining = saved->count; remaining > 0; remaining--) {
        size_t i = remaining - 1;
        int fd = saved->entries[i].fd;
        int target = saved->entries[i].target;
        if (restore) {
            if (fd >= 0) {
                /* Private backups differ from targets; dup2 clears fd flags. */
                if (br_dup2(fd, target) < 0 ||
                    (saved->entries[i].flags &&
                     fcntl(target, F_SETFD, saved->entries[i].flags) < 0)) {
                    br_report_error("redirection restore", errno);
                    failed = 1;
                }
            } else if (br_close(target, 1)) failed = 1;
        }
        if (fd >= 0 && br_close(fd, 0)) failed = 1;
    }
    br_active = saved->previous;
    free(saved);
    return failed;
}

int jash_redir_snapshot_begin(jash_redir_snapshot_t** out,
                              const int* targets, size_t target_count,
                              const int* operands, size_t operand_count) {
    if (!out) return 1;
    *out = NULL;
    if ((target_count && !targets) || (operand_count && !operands)) {
        br_report_error("redirection snapshot", EINVAL);
        return 1;
    }
    if (operand_count > SIZE_MAX - target_count) goto overflow;
    size_t count = target_count + operand_count;
    size_t bytes = sizeof(jash_redir_snapshot_t);
    if (target_count > (SIZE_MAX - bytes) / sizeof(((jash_redir_snapshot_t*)0)->entries[0]))
        goto overflow;
    bytes += target_count * sizeof(((jash_redir_snapshot_t*)0)->entries[0]);
    if (count > (SIZE_MAX - bytes) / sizeof(int)) goto overflow;
    bytes += count * sizeof(int);
    jash_redir_snapshot_t* saved = calloc(1, bytes);
    if (!saved) { br_report_error("redirection snapshot", ENOMEM); return 1; }
    saved->operands = (int*)(saved->entries + target_count);
    saved->operand_count = count;
    if (target_count) memcpy(saved->operands, targets, target_count * sizeof(int));
    if (operand_count) memcpy(saved->operands + target_count, operands, operand_count * sizeof(int));
    if (jash_redir_reserve_fds(saved->operands, count)) { free(saved); return 1; }
    saved->previous = br_active;
    br_active = saved;
    if (target_count && br_flush_stdio()) goto save_failed;

    /* Snapshot all targets before opening anything, so fd==target works and
     * backup allocation failures never leave a partially changed fd table. */
    for (size_t i = 0; i < target_count; i++) {
        int target = targets[i];
        if (target < 0) { errno = EBADF; goto save_error; }
        size_t j = 0;
        while (j < saved->count && saved->entries[j].target != target) j++;
        if (j < saved->count) continue;
        int flags = fcntl(target, F_GETFD);
        int copy = -1;
        if (flags < 0) {
            if (errno != EBADF) goto save_error;
        } else {
            copy = br_dup_private(target, saved->operands, count);
            if (copy < 0) goto save_error;
        }
        saved->entries[j].fd = copy;
        saved->entries[j].target = target;
        saved->entries[j].flags = flags;
        saved->count++;
    }
    for (size_t i = 0; i < target_count; i++)
        if (targets[i] == 0) saved->sync_stdin = 1;
    if (saved->sync_stdin) jash_stdin_stream_sync(0);
    *out = saved;
    return 0;

overflow:
    br_report_error("redirection snapshot", EOVERFLOW);
    return 1;
save_error:
    br_report_error("redirection save", errno);
save_failed:
    if (jash_redir_snapshot_end(saved, 0)) return 1;
    return 1;
}

static int br_apply_redirs(jash_state_t* s, const br_redir_t* redirs, int nredir,
                            jash_redir_snapshot_t** saved) {
    int targets[BR_MAX_REDIRS] = {0}, operands[BR_MAX_REDIRS] = {0};
    size_t count = 0;
    if (nredir < 0 || nredir > BR_MAX_REDIRS) {
        br_report_error("redirection count", EINVAL);
        return 1;
    }
    for (int i = 0; i < nredir; i++) {
        targets[i] = redirs[i].target_fd;
        if (redirs[i].kind == BR_DUP) operands[count++] = redirs[i].src_fd;
    }
    if (jash_redir_snapshot_begin(saved, targets, (size_t)nredir, operands, count)) return 1;

    for (int i = 0; i < nredir; i++) {
        const br_redir_t* r = &redirs[i];
        int target = r->target_fd;
        if (r->kind == BR_CLOSE) {
            if (br_close(target, 1)) goto apply_failed;
            continue;
        }
        if (r->kind == BR_DUP) {
            if (br_dup2(r->src_fd, target) < 0) {
                char subject[64];
                int err = errno;
                snprintf(subject, sizeof subject, "redirection %d to %d", r->src_fd, target);
                br_report_error(subject, err);
                goto apply_failed;
            }
            continue;
        }

        int fd;
        if (r->kind == BR_HERESTRING) {
            int pfds[2];
            if (sys_pipe(pfds) < 0) { br_report_error("redirection pipe", errno); goto apply_failed; }
            int err = br_write_all(pfds[1], r->fname, strlen(r->fname)) < 0 ? errno : 0;
            int failed = br_close(pfds[1], 0);
            if (err || failed) {
                if (err) br_report_error("redirection pipe write", err);
                if (br_close(pfds[0], 0)) failed = 1;
                goto apply_failed;
            }
            fd = pfds[0];
        } else {
            int flags;
            switch (r->kind) {
                case BR_FILE_IN: flags = O_RDONLY; break;
                case BR_FILE_OUT:
                    flags = O_WRONLY | O_CREAT | ((s && s->shell_opts.noclobber) ? O_EXCL : O_TRUNC);
                    br_journal(r->fname, 0, 0, s);
                    break;
                case BR_FILE_CLOBBER:
                    flags = O_WRONLY | O_CREAT | O_TRUNC;
                    br_journal(r->fname, 0, 1, s);
                    break;
                case BR_FILE_APPEND:
                    flags = O_WRONLY | O_CREAT | O_APPEND;
                    br_journal(r->fname, 1, 0, s);
                    break;
                case BR_FILE_RW: flags = O_RDWR | O_CREAT; break;
                default: br_report_error("redirection", EINVAL); goto apply_failed;
            }
            fd = (int)sys_open(r->fname, flags, 0666);
            if (fd < 0) { br_report_error(r->fname, errno); goto apply_failed; }
        }
        if (fd != target) {
            int err = br_dup2(fd, target) < 0 ? errno : 0;
            int failed = br_close(fd, 0);
            if (err || failed) {
                if (err) br_report_error("redirection dup2", err);
                goto apply_failed;
            }
        }
    }
    return 0;

apply_failed: {
    int failed = jash_redir_snapshot_end(*saved, 1);
    *saved = NULL;
    if (failed) return 1;
    return 1;
}
}

int jash_eval_builtin_with_redir(jash_state_t* s, jash_writer_t w, void* ctx,
                                 const jash_builtin_t* b, const char* line) {
    if (!s || !b || !line) return 1;

    const char* args = skip_first_token(line);

    /* JINV-028 — `clean` era `char clean[1024]`, e acima disso o REDIRECT
     * SUMIA em silencio.
     *
     * Medido por bissecao: `echo "$(cat 1019_bytes)" > saida.txt` cria o
     * arquivo; com 1020 bytes o arquivo NAO e criado e o stderr fica VAZIO. O
     * builtin escreve em stdout como se nenhum `>` existisse. Um
     * `config.json` qualquer dispara isso — 1 KB e tamanho cotidiano, nao caso
     * extremo. (Com comando EXTERNO o mesmo redirect funciona ate 16 KB: o
     * defeito era so do caminho de builtin.)
     *
     * Mesma familia do JINV-022 e do JINV-026: quando o corte cai numa linha
     * de comando, o que se perde e o FINAL — e o final e onde moram o `>`, o
     * `|` e o `&&`. O comando executado deixa de ser o comando escrito.
     *
     * A correcao nao precisa de teto nenhum: `clean` e sempre um SUBCONJUNTO
     * de `args` (e `args` menos os tokens de redirect), entao
     * `strlen(args) + 1` e um tamanho EXATO que jamais estoura. Se a alocacao
     * falhar, recusa a chamada em vez de truncar os redirecionamentos.
     * Gate: tests/test_jinv028_builtin_redirect.sh */
    char clean_stack[1024];
    size_t args_len = args ? strlen(args) : 0;
    char* clean_heap = (args_len + 1 > sizeof(clean_stack))
                     ? (char*)malloc(args_len + 1) : NULL;
    if (args_len + 1 > sizeof(clean_stack) && !clean_heap) {
        br_report_error("redirection arguments", ENOMEM);
        return 1;
    }
    char*  clean     = clean_heap ? clean_heap : clean_stack;
    size_t clean_cap = clean_heap ? (args_len + 1) : sizeof(clean_stack);

    br_redir_t redirs[BR_MAX_REDIRS];
    int nredir = 0;
    if (parse_redirs_inplace(args, clean, clean_cap, redirs, &nredir) < 0) {
        br_report_error("redirection syntax", errno);
        free(clean_heap);
        return 1;
    }
    jash_redir_snapshot_t* saved = NULL;
    int direct_fd = -1;
    int direct_output = nredir == 1 && redirs[0].target_fd == 1 &&
        (redirs[0].kind == BR_FILE_OUT || redirs[0].kind == BR_FILE_APPEND ||
         redirs[0].kind == BR_FILE_CLOBBER) &&
        (b->argv_fn == jash_b_printf || b->argv_fn == jash_b_echo_argv);
    int direct_input = !s->interactive && nredir == 1 &&
        redirs[0].target_fd == 0 && redirs[0].kind == BR_FILE_IN &&
        !b->argv_fn && b->fn == jash_b_read;
#ifndef JASH_AGENT_ONLY
    /* Mutation hooks can execute arbitrary module code; retain fd restoration. */
    if (g_modules.count) direct_output = direct_input = 0;
#endif
    if (direct_output || direct_input) {
        /* Native output uses a callback; non-interactive read accepts an fd.
         * None evaluates another command. Keep the standard fds untouched. */
        if (br_flush_stdio()) { free(clean_heap); return 1; }
        int flags = O_RDONLY | O_CLOEXEC;
        if (direct_input) jash_stdin_stream_sync(0);
        else {
            int append = redirs[0].kind == BR_FILE_APPEND;
            int clobber = redirs[0].kind == BR_FILE_CLOBBER;
            flags = O_WRONLY | O_CREAT | O_CLOEXEC |
                (append ? O_APPEND : (!clobber && s->shell_opts.noclobber ? O_EXCL : O_TRUNC));
            br_journal(redirs[0].fname, append, clobber, s);
        }
        direct_fd = (int)sys_open(redirs[0].fname, flags, 0666);
        if (direct_fd < 0) {
            br_report_error(redirs[0].fname, errno);
            if (direct_input) jash_stdin_stream_sync(0);
            free(clean_heap);
            return 1;
        }
        /* If a standard fd was closed, open may reuse it. Do not let printf
         * diagnostics accidentally write into the output file via fd 2. */
        if (direct_fd <= 2) {
            int copy;
            do { copy = fcntl(direct_fd, F_DUPFD_CLOEXEC, 3); } while (copy < 0 && errno == EINTR);
            int err = errno;
            int failed = br_close(direct_fd, 0);
            direct_fd = copy;
            if (copy < 0 || failed) {
                if (copy < 0) br_report_error("redirection descriptor", err);
                else (void)br_close(copy, 0);
                if (direct_input) jash_stdin_stream_sync(0);
                free(clean_heap);
                return 1;
            }
        }
    } else if (br_apply_redirs(s, redirs, nredir, &saved)) {
        free(clean_heap);
        return 1;
    }

    /* Quote removal: strip quotes from clean before passing to builtin.
     * This preserves argument boundaries while removing quote characters.
     * Example: '"%s\n" "hello world"' becomes '%s\n hello world' but
     * builtins like printf will still see the quotes via their own parsing.
     *
     * For simple builtins like echo that just print args, we need to
     * remove quotes here. For builtins like printf that do their own
     * split_args, they will handle quotes correctly.
     *
     * BUG-044: this site had NO exception list at all (unlike the parallel
     * logic in eval.c/pipeline.c), so `printf 'fmt %s\n' 'a b c' > file`
     * silently truncated — jash_unquote flattened the quoted multi-word
     * argument into one blob BEFORE printf's own split_args ever ran, so
     * that split had no quote characters left to tell a real separator
     * space from one that used to be inside quotes. ADR-0036 W2: give
     * argv_fn-migrated builtins (and the same historically-exempted names
     * used in eval.c/pipeline.c) raw args instead, for parity. */
    extern int jash_unquote(const char* in, char* out, size_t out_size);
    extern int jash_has_quotes(const char* s);
    /* JINV-042: era `char unquoted[1024]` FIXO — o mesmo teto que o JINV-028
     * tirou do `clean` logo acima, sobrevivendo no destino do unquote. Com ele,
     * `echo "$(cat arquivo_de_5KB)" > saida` gravava 1024 bytes; `printf` (que
     * esta em skip_unquote e nao passa por aqui) gravava os 5000. A diferenca
     * entre dois builtins no MESMO redirect foi o que denunciou.
     *
     * `unquoted` e sempre <= `clean` (remocao de aspas so encurta), entao
     * strlen(clean)+1 e exato e jamais estoura. */
    char unquoted_stack[1024];
    char* unq_heap = NULL;
    char* unquoted = unquoted_stack;
    const char* bname = b->name ? b->name : "";
    int skip_unquote = (b->argv_fn != NULL) ||
                       (strcmp(bname, "printf") == 0) ||
                       (strcmp(bname, "set") == 0) ||
                       (strcmp(bname, "trap") == 0) ||
                       (strcmp(bname, "builtin") == 0) ||
                       (strcmp(bname, "command") == 0) ||
                       (strcmp(bname, "grep") == 0) ||
                       (strcmp(bname, "mesh") == 0) ||
                       (strcmp(bname, "contract") == 0) ||
                       (strcmp(bname, "cost") == 0) ||
                       (strcmp(bname, "revert") == 0) ||
                       (strcmp(bname, "export") == 0) ||
                       (strcmp(bname, "declare") == 0) ||
                       (strcmp(bname, "typeset") == 0) ||
                       (strcmp(bname, "readonly") == 0) ||
                       (strcmp(bname, "local") == 0) ||
                       /* BUG-151: env with operands delegates its raw tail to
                        * jash_exec_external, whose argv builder is quote-aware.
                        * Pre-unquoting here dissolved a quoted `-c` payload
                        * (`env X=1 bash -c "a b"`) into separate words, so the
                        * wrapped shell ran only `a` with `b` as $0 — silent
                        * rc=0 no-op. Raw args keep the payload one token. */
                       (strcmp(bname, "env") == 0) ||
                       /* BUG-059: eval does its OWN single-layer POSIX quote
                        * removal then re-parses; pre-unquoting here destroyed
                        * the inner quote layer of two-pass-quoted text (the
                        * `eval 'cmd '\''...'\'' < /dev/null` harness shape). */
                       (strcmp(bname, "eval") == 0);
    int use_unquoted = 0;
    if (!skip_unquote && clean[0] && jash_has_quotes(clean)) {
        size_t unq_cap = strlen(clean) + 1;
        if (unq_cap > sizeof(unquoted_stack)) {
            unq_heap = (char*)malloc(unq_cap);
            if (!unq_heap) {
                br_report_error("redirection arguments", ENOMEM);
                int failed;
                if (direct_input) {
                    jash_stdin_stream_sync(0);
                    failed = br_close(direct_fd, 0);
                } else failed = jash_redir_snapshot_end(saved, 1);
                free(clean_heap);
                return failed ? failed : 1;
            }
            unquoted = unq_heap;
        }
        int unquoted_len = jash_unquote(clean, unquoted, unq_cap);
        if (unquoted_len >= 0) use_unquoted = 1;
    }

    /* Track redirected stdout errors even when a builtin ignores the writer's
     * return value. Only direct file targets delay writes; a dup may be a tty. */
    int buffer_stdout = 0;
    int file_stdout = 0;
    for (int i = 0; i < nredir; i++) {
        if (redirs[i].target_fd == 1) {
            int kind = redirs[i].kind;
            buffer_stdout = 1;
            file_stdout = kind == BR_FILE_OUT || kind == BR_FILE_CLOBBER ||
                          kind == BR_FILE_APPEND || kind == BR_FILE_RW;
        }
    }
    char output_inline[1024];
    br_obuf_t obuf = { .buf = output_inline, .cap = sizeof(output_inline),
                       .fd = direct_output ? direct_fd : 1 };
    /* Wrappers may execute nested commands with different stdout targets;
     * stream through the checked writer rather than postponing their output. */
    obuf.degraded = !file_stdout ||
                    strcmp(bname, "eval") == 0 || strcmp(bname, "source") == 0 ||
                    strcmp(bname, ".") == 0 || strcmp(bname, "builtin") == 0 ||
                    strcmp(bname, "command") == 0 || strcmp(bname, "env") == 0 ||
                    strcmp(bname, "exec") == 0 || strcmp(bname, "xargs") == 0;

    /* Call the builtin with the clean args. INV-523: keep local obuf/argv
     * origins on explicit call edges instead of hiding them behind joined
     * pointers before a dispatcher call. */
    int rc;
    if (direct_input) {
        if (use_unquoted)
            rc = jash_b_read_from_fd(s, w, ctx, unquoted, direct_fd);
        else
            rc = jash_b_read_from_fd(s, w, ctx, clean, direct_fd);
    } else if (use_unquoted) {
        rc = buffer_stdout
           ? jash_builtin_dispatch(b, s, br_obuf_write, &obuf, unquoted)
           : jash_builtin_dispatch(b, s, w, ctx, unquoted);
    } else {
        rc = buffer_stdout
           ? jash_builtin_dispatch(b, s, br_obuf_write, &obuf, clean)
           : jash_builtin_dispatch(b, s, w, ctx, clean);
    }
    /* Flush buffered stdout as a SINGLE write to the (still-redirected) fd 1. */
    if (buffer_stdout && !obuf.error && obuf.len > 0 &&
        br_write_all(obuf.fd, obuf.buf, obuf.len) < 0) obuf.error = errno;
    if (obuf.error) {
        br_report_error("redirection write", obuf.error);
        if (rc == 0) rc = 1;
    }
    free(obuf.heap);
    if (direct_output || direct_input) {
        /* fd 0 never changed: unread stdio bytes still belong to its next
         * consumer. Clear status like the full path, but retain that buffer. */
        if (direct_input) jash_stdin_stream_sync(0);
        if (br_close(direct_fd, 0) && rc == 0) rc = 1;
        if (br_flush_stdio() && rc == 0) rc = 1;
    } else if (jash_redir_snapshot_end(saved, 1) && rc == 0) rc = 1;

    free(clean_heap);                                      /* JINV-028 */
    free(unq_heap);                                        /* JINV-042 */
    return rc;
}

/* Apply redirections permanently to the current shell (for 'exec [redirs]').
 * POSIX: 'exec' with no command but with redirections applies them to the
 * current shell environment, restoring the original fds on failure. Examples:
 *   exec 3>file    — open fd 3 for writing
 *   exec 3>&-      — close fd 3
 *   exec >file 2>&1 — redirect stdout and stderr permanently
 * Returns 0 if redirections were applied (no command), -1 if there was a
 * command (caller should use the normal exec path), positive on error. */
int jash_exec_apply_redirs_permanently(jash_state_t* s, const char* line) {
    if (!line) return 1;

    const char* args = skip_first_token(line);

    size_t clean_cap = strlen(args) + 1;
    char* clean = (char*)malloc(clean_cap);
    if (!clean) { br_report_error("redirection arguments", ENOMEM); return 1; }
    br_redir_t redirs[BR_MAX_REDIRS];
    int nredir = 0;
    if (parse_redirs_inplace(args, clean, clean_cap, redirs, &nredir) < 0) {
        br_report_error("redirection syntax", errno);
        free(clean);
        return 1;
    }

    /* If there's a command after stripping redirections, return -1
     * so the caller uses the normal exec path (replace shell). */
    int has_command = *skip_ws(clean) != '\0';
    free(clean);
    if (has_command) return -1;

    jash_redir_snapshot_t* saved;
    if (br_apply_redirs(s, redirs, nredir, &saved)) return 1;
    return jash_redir_snapshot_end(saved, 0);
}

/* Run a shell function with I/O redirects applied in the current process.
 * Mirrors jash_eval_builtin_with_redir: saves affected fds to high numbers,
 * applies redirects, evaluates the function body, then restores fds.
 * POSIX: a function call with redirects runs in the current shell environment —
 * no fork — so variable assignments and other side-effects are visible to the
 * caller. */
int jash_eval_function_with_redir(jash_state_t* s, jash_writer_t w, void* ctx,
                                   const char* fname, const char* fn_args) {
    if (!s || !fname) return 1;

    const char* args_in = fn_args ? fn_args : "";

    size_t clean_cap = strlen(args_in) + 1;
    char* clean = (char*)malloc(clean_cap);
    if (!clean) { br_report_error("redirection arguments", ENOMEM); return 1; }
    br_redir_t redirs[BR_MAX_REDIRS];
    int nredir = 0;
    if (parse_redirs_inplace(args_in, clean, clean_cap, redirs, &nredir) < 0) {
        br_report_error("redirection syntax", errno);
        free(clean);
        return 1;
    }
    jash_redir_snapshot_t* saved;
    if (br_apply_redirs(s, redirs, nredir, &saved)) {
        free(clean);
        return 1;
    }

    /* Route the cleaned invocation through the ordinary function engine. That
     * keeps local scope, FUNCNAME, recursion limits, return handling, and
     * positional restoration identical with and without redirects. */
    const char* body = jash_function_lookup(s->functions, fname);
    int rc = body ? jash_eval_function(s, w, ctx, fname, body, clean) : 1;
    if (!body) br_report_error(fname, ENOENT);
    if (jash_redir_snapshot_end(saved, 1) && rc == 0) rc = 1;

    free(clean);
    return rc;
}
