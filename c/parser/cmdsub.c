// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// Module: parser/cmdsub — command substitution $(cmd) and `cmd`
//
// OPTIMIZED VERSION: Applied audit recommendations for 67-150% overhead reduction.
// Changes:
//   - OPT#1: Depth-indexed capture slots (eliminate 128KB stack copy)
//   - OPT#2: Builtin fast-path (skip pipe for $(pwd), $(echo x), etc.)
//   - OPT#3: Lightweight eval for inner commands (skip redundant expansion passes)

#include "cmdsub.h"
#include <core/limits.h>
#include "eval.h"
#include "expand.h"
#include "param_expand.h"   /* JINV-087: porta única de divisão por IFS */
#include <core/state.h>
#include <core/env.h>
#include <core/cache.h>
#include <builtins/builtin.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <errno.h>
#include <sys/syscalls.h>

/* Capture buffer for inner-command stdout. Module-static so the
 * writer callback can find it. Single-threaded shell, so no lock. */
#define JASH_CMDSUB_CAP JASH_LINE_CAP  /* JINV-030: deriva de core/limits.h — nao um numero solto */

/* ═══ JINV-088 — o TEXTO DO COMANDO dentro de `$( )` não é cortado ════════
 *
 * Este arquivo tinha três `char[512]` em série no caminho de `$( )`, e o que
 * eles cortavam não era saída: era o **enunciado do comando a executar**.
 *
 *   V=$(printf %s <505 a's>)          → devolvia 500
 *   echo $(printf %s <500 a's> | wc -c)  → o `| wc -c` DESAPARECIA
 *
 * O segundo é a doença, não o primeiro. Cortar o fim de um enunciado é
 * apagar exatamente onde moram o `|`, o `> arquivo` e o `&&` — o shell
 * executa um programa que ninguém escreveu, em silêncio e com rc 0. Mesma
 * família de JINV-016/020/022/026/032: dado virando (ou deixando de ser)
 * estrutura. Não é perda de dado; é troca de comando.
 *
 * Os três sítios, medidos em série (a armadilha do JINV-086: consertar dois
 * não move o penhasco):
 *
 *   1. `inner`   — a ANÁLISE de `$( … )`      (clamp `ilen = sizeof - 1`)
 *   2. `cleaned` — a REMOÇÃO DE COMENTÁRIOS   (guarda `ci + 2 < sizeof`)
 *   3. `inner`   — a ANÁLISE de ` … `          (guarda `ilen + 1 < sizeof`)
 *
 * O idioma do conserto é o do JINV-080/081/082/086: o tamanho antigo vira
 * LIMIAR DE DESEMPENHO (pilha, zero `malloc` no caso comum — o mesmo perfil
 * de memória de antes), o heap é do tamanho EXATO acima dele, a saída do
 * ramo passa por uma PORTA ÚNICA (senão o heap vaza a cada `continue`), e
 * falta de memória é RECUSA ALTA: mensagem no stderr e nada roda — jamais um
 * enunciado pela metade com cara de sucesso.
 *
 * `cleaned` nunca é maior que `inner` (o filtro de comentários só remove
 * bytes; os dois ramos que escrevem dois bytes também consomem dois), então
 * `ilen + 1` é um tamanho exato que não pode estourar.
 *
 * Gate: tests/test_jinv088_cmdsub_body.sh */
#define JASH_CMDSUB_INNER_STACK 512
#if JASH_CMDSUB_INNER_STACK > JASH_LINE_CAP_STACK
#error "JASH_CMDSUB_INNER_STACK nao pode exceder JASH_LINE_CAP_STACK (core/limits.h)"
#endif

/* JINV-088: canal de recusa, no mesmo formato que param_expand.c já usa
 * (jash_param_expand_error / _msg) — o avaliador consulta depois de expandir
 * e ABORTA a linha, em vez de rodar um enunciado que a expansão não conseguiu
 * montar inteiro. */
static int  g_cmdsub_err = 0;
static char g_cmdsub_err_msg[160];

int jash_cmdsub_error(void) { return g_cmdsub_err; }
const char* jash_cmdsub_error_msg(void) { return g_cmdsub_err_msg; }
void jash_cmdsub_clear_error(void) { g_cmdsub_err = 0; g_cmdsub_err_msg[0] = '\0'; }

/* JINV-088: recusa ALTA — registra o motivo e marca a expansão como abortada.
 * Nunca trunca: quem chamou vê o erro e não executa nada. */
static void cmdsub_refuse_oom(unsigned need) {
    g_cmdsub_err = 1;
    snprintf(g_cmdsub_err_msg, sizeof g_cmdsub_err_msg,
             "jash: $(): sem memoria para o texto do comando (%u bytes) — "
             "recusando (nada foi executado)", need);
}

static void cmdsub_refuse_output(unsigned cap) {
    g_cmdsub_err = 1;
    snprintf(g_cmdsub_err_msg, sizeof g_cmdsub_err_msg,
             "jash: $(): output excede %u bytes — recusando em vez de truncar",
             cap);
}

static void cmdsub_refuse_capture(void) {
    g_cmdsub_err = 1;
    snprintf(g_cmdsub_err_msg, sizeof g_cmdsub_err_msg,
             "jash: $(): captura segura indisponivel — recusando (nada foi executado)");
}

static void cmdsub_refuse_inner(void) {
    g_cmdsub_err = 1;
    snprintf(g_cmdsub_err_msg, sizeof g_cmdsub_err_msg,
             "jash: $(): inner expansion refused — outer command not executed");
}

/* SECURITY (audit rank 15): command-substitution output must be INERT — once a
 * command runs, the bytes it printed are data and must NOT be re-scanned for
 * further parameter ($VAR), arithmetic ($((..))) or command ($(..)) expansion. */
#define JASH_PROT_DOLLAR   ((char)0x01)
#define JASH_PROT_BACKTICK ((char)0x02)
/* BUG-096: literal quote characters inside EXPANDED VALUES (param expansion
 * and cmdsub output) must not be re-parsed as quote syntax by the downstream
 * tokenizers — q='"'; echo "[$q]" corrupted the word. Shield them with
 * sentinels at emission; every tokenizer unshields its OUTPUT tokens via
 * jash_quote_unshield. Sentinels exist only in the transient expanded text —
 * never in env storage, argv, or output. */
#define JASH_PROT_DQUOTE   ((char)0x03)
#define JASH_PROT_SQUOTE   ((char)0x04)
/* BUG-097: literal backslashes in cmdsub output were emitted RAW into the
 * transient expanded text, where downstream passes re-scanned them as live
 * escapes — `B=$(cat f)` with f containing `a\"b\$c` stored `a"b$c` (the \
 * was consumed escaping the shielded byte that followed). Output bytes are
 * data (see SECURITY #15 above): shield \ like the rest, restore on unshield. */
#define JASH_PROT_BSLASH   ((char)0x05)
/* JINV-009: um espaço vindo de DENTRO de um nome de arquivo (resultado de
 * glob) é DADO, não separador. A expansão do jash é reescrita de string e a
 * separação de campos fica a cargo do tokenizador de espaços a jusante — sem
 * um sentinela, `for f in *` parte "meu arquivo.txt" em duas palavras.
 * Reproduzido em 2026-07-27: diretório com 2 arquivos (um com espaço no nome)
 * era contado como 4 pelo `for`. Mesmo mecanismo dos sentinelas acima.
 * Gate: tests/test_jinv009_glob_spaces.sh */
#define JASH_PROT_SPACE    ((char)0x06)

/* ═══ JINV-096 — a porta ÚNICA que marca "este byte veio de um VALOR" ═════
 *
 * Contrato completo em cmdsub.h. Aqui só a mecânica: um byte que decide a
 * ESTRUTURA do enunciado, quando emitido como RESULTADO de expansão, troca
 * de identidade. As varreduras estruturais a jusante (`line_has_op`, o
 * divisor de segmentos de cano, o analisador de redirecionamento) e a camada
 * de palavra (til) continuam procurando o byte cru e simplesmente não o
 * acham — nenhuma delas precisou aprender regra nova.
 *
 * O conjunto é o mesmo `jash_pe_is_operator_meta` do JINV-093 — a definição
 * que este repositório já tem de "byte estrutural" — mais o til. O `*` fica
 * de FORA de propósito: a expansão de nome de arquivo é uma das DUAS que
 * POSIX manda aplicar ao resultado de uma expansão (a outra é a divisão de
 * campos, que já tem a sua marca em JASH_FIELD_SEP).
 *
 * Idempotente: os sentinelas (0x12..0x19) não são metacaracteres, então
 * blindar duas vezes é blindar uma. Isso importa porque a saída de `$( )` é
 * blindada em cmdsub.c e pode reatravessar a emissão de valor. */
char jash_meta_shield(char c) {
    switch (c) {
        case '|': return JASH_PROT_PIPE;
        case '<': return JASH_PROT_LT;
        case '>': return JASH_PROT_GT;
        case '&': return JASH_PROT_AMP;
        case ';': return JASH_PROT_SEMI;
        case '(': return JASH_PROT_LPAREN;
        case ')': return JASH_PROT_RPAREN;
        case '~': return JASH_PROT_TILDE;
        default:  return c;
    }
}

int jash_meta_shield_present(const char* s) {
    if (!s) return 0;
    for (; *s; s++) {
        if (*s >= (char)0x12 && *s <= (char)0x19) return 1;   /* JINV-096 */
    }
    return 0;
}

void jash_quote_unshield(char* s) {
    if (!s) return;
    for (; *s; s++) {
        if (*s == JASH_PROT_DQUOTE)      *s = '"';
        else if (*s == JASH_PROT_SQUOTE) *s = '\'';
        else if (*s == JASH_PROT_BSLASH) *s = '\\';  /* BUG-097 */
        else if (*s == JASH_PROT_SPACE)  *s = ' ';   /* JINV-009 */
        /* JINV-087: tabulação e nova-linha que o IFS não elegeu separador são
         * DADO — voltam inteiras ao token. Sem estas duas linhas, `IFS=:` com
         * valor `a<TAB>b:c` entregaria a sentinela crua ao corpo do laço. */
        else if (*s == JASH_PROT_TAB)    *s = '\t';
        else if (*s == JASH_PROT_NL)     *s = '\n';
        /* ═══ JINV-096 — o restauro dos metacaracteres de ESTRUTURA ═══
         *
         * Depois daqui o byte é entregue a um argv / a um corpo de laço / a
         * um padrão de case: já não há mais nenhuma decisão de estrutura pela
         * frente, então ele volta a ser o caractere que o valor sempre teve.
         * Este é o ponto que faz o conserto inteiro caber num só lugar — todo
         * consumidor de token já chamava esta função. */
        else if (*s == JASH_PROT_PIPE)   *s = '|';
        else if (*s == JASH_PROT_LT)     *s = '<';
        else if (*s == JASH_PROT_GT)     *s = '>';
        else if (*s == JASH_PROT_AMP)    *s = '&';
        else if (*s == JASH_PROT_SEMI)   *s = ';';
        else if (*s == JASH_PROT_LPAREN) *s = '(';
        else if (*s == JASH_PROT_RPAREN) *s = ')';
        else if (*s == JASH_PROT_TILDE)  *s = '~';
    }
}

void jash_cmdsub_unprotect(char* s) {
    if (!s) return;
    for (; *s; s++) {
        if (*s == JASH_PROT_DOLLAR)        *s = '$';
        else if (*s == JASH_PROT_BACKTICK) *s = '`';
        /* BUG-097: PROT_BSLASH is NOT restored here — unprotect runs before
         * the assignment path's jash_unquote (POSIX quote removal), which
         * would re-eat a restored backslash. Same hazard as BUG-116a: the
         * backslash is restored late, by jash_quote_unshield. */
    }
}

/* JINV-090 — o inverso de jash_cmdsub_unprotect, para o `$`/crase RESIDUAL.
 *
 * Chamado por eval.c num unico ponto: depois que TODAS as passadas de
 * expansao daquela linha ja rodaram, e so quando a linha vai ser entregue ao
 * motor de canos (que expande outra vez, por segmento). Nesse instante um
 * `$` que sobrou nao pode ser o inicio de uma expansao — o shell acabou de
 * decidir que nao era. Ele e dado: literal da fonte, citado, escapado, ou
 * vindo do VALOR de uma variavel. A marca o leva vivo ate pipeline.c, que
 * restaura depois da expansao de cada segmento.
 *
 * Nao mexe em bytes ja blindados (eles JA sao a marca) nem em nada mais: e
 * uma troca de dois caracteres, e o unico restauro e o de pipeline.c.
 *
 * Gate: tests/test_jinv090_eval_no_reexpand.sh */
void jash_cmdsub_protect_residual(char* s) {
    if (!s) return;
    for (; *s; s++) {
        if (*s == '$')      *s = JASH_PROT_DOLLAR;
        else if (*s == '`') *s = JASH_PROT_BACKTICK;
    }
}

/* ---- OPT#1: Depth-indexed capture slots ----
 * Instead of saving/restoring a 128KB buffer on every cmdsub call,
 * each recursion depth gets its own slot. This eliminates the O(N)
 * memcpy that was costing ~0.3-0.5ms per invocation.
 *
 * ADR-0016 §4 (cmdsub arena): slots are LAZILY allocated slabs, not
 * static arrays. The old fixed arrays reserved 12.6 MB of .bss for a
 * worst case (32-deep nesting) that virtually never occurs. A slab is
 * malloc'd on FIRST use at a given depth and kept for the shell's
 * lifetime, so the hot path pays one branch, not one malloc. Capacity
 * per slot (128 KB) is unchanged. */
#define JASH_CMDSUB_MAX_DEPTH 32

/* One emergency slab so depth-0 cmdsub still works under OOM. */
static char g_capture_fallback[1024];

/* Returns the slab for *slot, allocating on first use. On OOM returns
 * the small static fallback and shrinks *cap_io to its real capacity —
 * callers MUST bound all writes by the returned capacity. */
static char* cmdsub_slab(char** slot, size_t* cap_io) {
    if (!*slot) {
        *slot = (char*)malloc(*cap_io);
        if (!*slot) {
            /* OOM: degrade to the small static slab rather than crash.
             * Output is truncated at 1 KB — honest failure, not UB. */
            cmdsub_refuse_oom((unsigned)*cap_io);
            *cap_io = sizeof(g_capture_fallback);
            g_capture_fallback[0] = '\0';
            return g_capture_fallback;
        }
        (*slot)[0] = '\0';
    }
    return *slot;
}

static char* g_capture_slots[JASH_CMDSUB_MAX_DEPTH];
static unsigned g_capture_lens[JASH_CMDSUB_MAX_DEPTH];
static int g_capture_depth = 0;

/* BUG-074d: exit status of the last command run inside a command substitution
 * (read by the assignment path so `x=$(false)` sets $? to 1). Defined here at
 * file scope so run_and_capture/try_builtin_fastpath above the expander can
 * write it. */
int g_jash_cmdsub_last_status = 0;

static int param_trigger_char(char c) {
    return c == '{' || c == '_' || c == '?' || c == '$' || c == '#' ||
           c == '!' || c == '-' || c == '*' || c == '@' ||
           (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
           (c >= '0' && c <= '9');
}

unsigned jash_expansion_mask(const char* line) {
    if (!line) return 0;
    /* Fast reject: no `$` (cmdsub/arith/param), no backtick (cmdsub) and
     * no `<` (quoted heredoc) means no bit can fire — one SIMD strpbrk
     * instead of a quote-tracking pass. This is the common case
     * (`test x = x`, `echo hi`) and the whole point of the fused scan. */
    if (!strpbrk(line, "$`<")) return 0;
    unsigned bits = 0;
    /* Machine A replicates jash_has_cmdsub exactly (BUG-060 quote rules,
     * backslash skip outside single quotes). Machine B replicates
     * jash_has_param_expansion exactly (no backslash skip — `\$V` still
     * counts; the escape is honored later by the expansion itself).
     * ARITH/HEREDOCQ are raw substring checks like strstr, quote-blind. */
    int a_sq = 0, a_dq = 0, a_skip = 0;
    int b_sq = 0, b_dq = 0;
    for (const char* p = line; *p; p++) {
        char c = *p;
        if (c == '$' && p[1] == '(' && p[2] == '(')
            bits |= JASH_EXP_ARITH;
        if (c == '<' && p[1] == '<') {
            if (p[2] == '\'' || p[2] == '"')
                bits |= JASH_EXP_HEREDOCQ;
            else if (p[2] == '-' && (p[3] == '\'' || p[3] == '"'))
                bits |= JASH_EXP_HEREDOCQ;
        }
        if (a_skip) {
            a_skip = 0;
        } else if (c == '\\' && !a_sq && p[1]) {
            a_skip = 1;
        } else if (c == '\'' && !a_dq) {
            a_sq = !a_sq;
        } else if (c == '"' && !a_sq) {
            a_dq = !a_dq;
        } else if (!a_sq) {
            if (c == '$' && p[1] == '(' && p[2] != '(')
                bits |= JASH_EXP_CMDSUB;
            else if (c == '`')
                bits |= JASH_EXP_CMDSUB;
        }
        if (!b_dq && c == '\'') {
            b_sq = !b_sq;
        } else if (!b_sq && c == '"') {
            b_dq = !b_dq;
        } else if (!b_sq) {
            if (c == '$' && p[1] && param_trigger_char(p[1]))
                bits |= JASH_EXP_PARAM;
        }
        if ((bits & JASH_EXP_ALL) == JASH_EXP_ALL) break;
    }
    return bits;
}

int jash_has_cmdsub(const char* line) {
    return (jash_expansion_mask(line) & JASH_EXP_CMDSUB) != 0;
}

/* Writer callback that writes to fd 1 (which is redirected to a pipe
 * during command substitution capture). */
static int g_capture_write_error = 0;
static long pipe_writer(void* ctx, const char* buf, size_t n) {
    (void)ctx;
    size_t off = 0;
    while (off < n) {
        long written = sys_write(1, buf + off, n - off);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) { g_capture_write_error = 1; return -1; }
        off += (size_t)written;
    }
    return (long)off;
}

/* ---- OPT#2: Builtin fast-path context ---- */
typedef struct {
    char* buf;
    unsigned cap;
    unsigned len;
    int overflow;
} mem_capture_ctx_t;

/* Writer callback that captures to a memory buffer (for builtin fast-path).
 * Avoids pipe creation entirely for commands like $(pwd), $(echo x). */
static long mem_writer(void* ctx_ptr, const char* buf, size_t n) {
    mem_capture_ctx_t* c = (mem_capture_ctx_t*)ctx_ptr;
    size_t copied = 0;
    for (; copied < n && c->len + 1 < c->cap; copied++) {
        c->buf[c->len++] = buf[copied];
    }
    if (copied < n) c->overflow = 1;
    c->buf[c->len] = '\0';
    return (long)n;
}

/* Extract command name (first whitespace-delimited token) from cmd string.
 * Returns pointer to args (after first token), or NULL if no args. */
static const char* extract_cmd_name(const char* cmd, char* name, unsigned name_cap) {
    unsigned i = 0;
    const char* p = cmd;
    while (*p == ' ' || *p == '\t') p++;  /* skip leading whitespace */
    while (*p && *p != ' ' && *p != '\t' && i < name_cap - 1) {
        name[i++] = *p++;
    }
    name[i] = '\0';
    while (*p == ' ' || *p == '\t') p++;  /* skip whitespace after name */
    return *p ? p : NULL;
}

/* Fast-path builtins only when arguments are already literal. If args need
 * ANY expansion — parameter/arithmetic/cmdsub ($/`), brace ({), tilde (~),
 * or glob (* ? [) — full eval is required first so the builtin receives
 * already-expanded argv-like text.
 *
 * BUG-049: this used to check ONLY '$'/'`', so `echo "$(echo {a,b})"` took
 * the fast path (echo IS a builtin, no `;`/`|`/`&`) and the builtin got the
 * literal, unexpanded text "{a,b}" — the brace/tilde/glob stages
 * (jash_expand_word_layer) only run on the SLOW path. Same bug existed for
 * `~` and glob metachars, just never reported: `echo "$(echo ~)"` and
 * `echo "$(echo *.md)"` were equally silently un-expanded. Widening this
 * check to bail to the slow path for any of these is strictly more
 * conservative — it can only route MORE cases through full expansion,
 * never skip expansion that used to run. */
static int args_need_expansion(const char* args) {
    if (!args) return 0;
    const char* p = args;
    while (*p) {
        if (*p == '\\' && p[1]) { p += 2; continue; }
        if (*p == '$' || *p == '`' || *p == '{' || *p == '~' ||
            *p == '*' || *p == '?' || *p == '[') return 1;
        p++;
    }
    return 0;
}

/* ---- OPT#2: Execute builtin command directly without pipe/fork ----
 * Returns 1 if cmd was a builtin and was executed successfully.
 * Returns 0 if cmd is NOT a builtin (caller should use slow path).
 *
 * This saves ~8 syscalls (pipe, dup, dup2 x2, close x3, read) and
 * avoids the overhead of the full eval pipeline for simple builtins.
 *
 * Bash does this internally in execute_command_internal(). */
static int cmdsub_may_mutate(const char* cmd);

static int cmdsub_builtin_is_pure(const char* name) {
    static const char* const pure[] = {
        "echo", "pwd", "true", "false", "test", "[",
        "cat", "wc", "head", "tail", "cut", "tr", "sort", "uniq",
        "seq", "grep", "basename", "dirname", NULL
    };
    for (int i = 0; pure[i]; i++) if (strcmp(name, pure[i]) == 0) return 1;
    return 0;
}

static int try_builtin_fastpath(jash_state_t* s, const char* cmd,
                                 char* out, unsigned cap, unsigned* out_len) {
    /* BUG-074 b/c: a mutating builtin (`$(cd /x)`, `$(export …)`) must NOT run
     * on the fast path — that would skip run_and_capture's env/cwd isolation
     * and leak the change to the parent. Bail so the isolated slow path runs. */
    if (cmdsub_may_mutate(cmd)) return 0;
    /* The fast-path runs ONE builtin with literal args. If cmd is actually a
     * command list / pipeline (contains an unquoted ; && || |), it is NOT a
     * single builtin — bail to the slow fork+eval path so e.g.
     * $(echo a; echo b) runs both statements. F E R M I ∞ H A R T */
    {
        int sq = 0, dq = 0;
        for (const char* p = cmd; *p; p++) {
            if (sq) { if (*p == '\'') sq = 0; continue; }
            if (dq) { if (*p == '"') dq = 0; continue; }
            if (*p == '\'') { sq = 1; continue; }
            if (*p == '"') { dq = 1; continue; }
            /* BUG-090: '\n' bails too — a glued multi-line inner text must
             * take the full eval path, not dispatch one builtin with raw
             * newline-joined args.
             * BUG-164: '<' / '>' redirects bail too. The fast path dispatches a
             * builtin with RAW args and applies no redirects, so `$(grep hi <f)`
             * (and heredocs, which the outer reader rewrites to `<tmpfile`)
             * leaked the `<` and the filename into the builtin's argv. The slow
             * path routes through jash_eval_line → builtin_redir, which applies
             * the redirect and feeds the builtin stdin correctly. Quoted
             * `<`/`>` are skipped by the sq/dq tracking above; `<` inside
             * $((…)) is already caught by the expansion bail below. */
            if (*p == ';' || *p == '|' || *p == '&' || *p == '\n' ||
                *p == '<' || *p == '>') return 0;
        }
    }

    char name[64];
    const char* args = extract_cmd_name(cmd, name, sizeof(name));
    
    /* Lookup builtin */
    const jash_builtin_t* b = jash_builtin_lookup(name);
    if (!b) return 0;  /* Not a builtin — fall through to external exec */
    if (!cmdsub_builtin_is_pure(name)) return 0;
    
    /* If args contain expansions, use slow path to expand them first. */
    if (args && args_need_expansion(args)) return 0;
    /* BUG-075: the fast path dispatches args RAW — it skips the quote-removal
     * that the normal command path applies, so `$(echo 'sq')` echoed the
     * literal quotes. Bail to the slow eval path when quotes are present
     * (it does POSIX quote removal, honoring per-builtin skip lists).
     * BUG-097: same for backslashes — `$(echo \her\e)` dispatched the raw
     * `\her\e` without escape processing. */
    if (args && (strchr(args, '\'') || strchr(args, '"') || strchr(args, '\\')))
        return 0;
    
    /* Set up memory capture context */
    mem_capture_ctx_t ctx = { .buf = out, .cap = cap, .len = 0, .overflow = 0 };
    
    /* Execute builtin directly with memory writer */
    int fp_saved_exit = s->exit_requested;
    int rc = jash_builtin_dispatch(b, s, mem_writer, &ctx, args);
    /* BUG-074: `$(exit N)` dispatches the exit builtin here — restore the flag
     * so it ends only the substitution, not the whole shell. */
    s->exit_requested = fp_saved_exit;
    /* BUG-074d: propagate the builtin's status so `x=$(false)` sets $? to 1. */
    g_jash_cmdsub_last_status = rc;
    if (ctx.overflow) cmdsub_refuse_output(cap - 1);
    
    /* Trim trailing newlines (bash semantics) */
    while (ctx.len > 0 && (ctx.buf[ctx.len - 1] == '\n' || ctx.buf[ctx.len - 1] == '\r')) {
        ctx.len--;
    }
    ctx.buf[ctx.len] = '\0';
    *out_len = ctx.len;
    
    return 1;  /* Success — builtin was executed */
}

/* Run `cmd` (inner text), capture stdout into result slot, return ptr to
 * NUL-terminated captured text (trimmed of trailing newlines).
 *
 * OPTIMIZATIONS APPLIED:
 *   #1: Depth-indexed capture slots (no 128KB stack copy)
 *   #2: Builtin fast-path (no pipe/fork for builtins)
 *   #3: Lightweight eval for inner commands (TODO: requires eval.c changes)
 */
/* BUG-074 b/c: does this inner command look like it mutates shell state
 * (an assignment, or a state-changing builtin) — so the caller should
 * snapshot/restore env+cwd for subshell isolation? Read-only substitutions
 * (`$(echo …)`, `$(git …)`, `$(cat …)`) return 0 and stay on the fast path. */
static int cmdsub_may_mutate(const char* cmd) {
    static const char* kws[] = { "cd", "export", "unset", "declare", "local",
                                 "set", "typeset", "pushd", "popd", "shift",
                                 "eval", "source", 0 };
    const char* p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t' || *p == ';' || *p == '&' ||
               *p == '|' || *p == '\n' || *p == '(') p++;
        if (!*p) break;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_') {
            const char* q = p;
            while ((*q >= 'a' && *q <= 'z') || (*q >= 'A' && *q <= 'Z') ||
                   (*q >= '0' && *q <= '9') || *q == '_' || *q == '[') q++;
            if (*q == '=' || (*q == '+' && q[1] == '=')) return 1; /* NAME=… */
            const char* we = p;
            while (*we && *we != ' ' && *we != '\t' && *we != ';' &&
                   *we != '&' && *we != '|' && *we != '\n') we++;
            size_t fwl = (size_t)(we - p);
            for (int i = 0; kws[i]; i++)
                if (strlen(kws[i]) == fwl && strncmp(p, kws[i], fwl) == 0) return 1;
            p = we;
        } else {
            while (*p && *p != ' ' && *p != '\t' && *p != ';' &&
                   *p != '&' && *p != '|' && *p != '\n') p++;
        }
    }
    return 0;
}

static const char* run_and_capture(jash_state_t* s, const char* cmd) {
    /* Get CALLER's function depth for slot indexing (before cmd execution) */
    const char *depth_str = jash_env_get(s, "__FUNC_DEPTH__");
    int caller_depth = depth_str ? atoi(depth_str) : 0;
    if (caller_depth >= JASH_CMDSUB_MAX_DEPTH) caller_depth = JASH_CMDSUB_MAX_DEPTH - 1;
    
    const char *cwd = (s && s->cwd[0]) ? s->cwd : NULL;
    /* ADR-0016 §4: lazily-allocated result slots (was 4 MB of .bss). */
    static char* result_slots[JASH_CMDSUB_MAX_DEPTH];
    size_t result_cap = JASH_CMDSUB_CAP;
    
    /* BUG-086: the cache key is the RAW UNEXPANDED command text, so any `$`
     * or backtick in it makes the entry semantically wrong the moment a
     * referenced variable changes — `$(echo $i)` was served stale forever
     * (whitelisted cmds have TTL 0 = never expire), turning
     * `while [ "$(echo $i)" -lt 3 ]` into an infinite loop. Never cache or
     * serve cached results for command text that references variables. */
    int cs_cacheable = (strchr(cmd, '$') == NULL && strchr(cmd, '`') == NULL);

    /* SMART CACHE: Check cache first for fast path */
    cache_entry_t *cached = cs_cacheable ? cache_lookup(cmd, cwd) : NULL;
    if (cached) {
        char* result = cmdsub_slab(&result_slots[caller_depth], &result_cap);
        if (jash_cmdsub_error()) { result[0] = 0; return result; }
        /* Values are NUL-terminated by construction; strncpy would zero-pad
         * the whole result_cap (~256KB) on every cache hit. */
        size_t vlen = strlen(cached->value);
        if (vlen >= result_cap) vlen = result_cap - 1;
        memcpy(result, cached->value, vlen);
        result[vlen] = '\0';
        return result;
    }
    
    char* result = cmdsub_slab(&result_slots[caller_depth], &result_cap);
    if (jash_cmdsub_error()) { result[0] = 0; return result; }

    /* BUG-076: `$(<file)` — POSIX shorthand that reads the file's contents
     * (no command run). Detect a leading `<` and slurp the named file. */
    {
        const char* q = cmd;
        while (*q == ' ' || *q == '\t') q++;
        if (*q == '<') {
            q++;
            while (*q == ' ' || *q == '\t') q++;
            char path[512]; unsigned pi = 0;
            while (*q && *q != ' ' && *q != '\t' && pi + 1 < sizeof(path)) path[pi++] = *q++;
            path[pi] = '\0';
            /* BUG-101: the filename undergoes parameter expansion and quote
             * removal — `$(<$f)` / `$(<"$f")` opened a file literally named
             * "$f" and silently returned empty. */
            {
                extern int jash_has_param_expansion(const char* str);
                extern const char* jash_param_expand(const char* str, void* state);
                if (jash_has_param_expansion(path)) {
                    const char* px = jash_param_expand(path, s);
                    if (px) { snprintf(path, sizeof(path), "%s", px); }
                }
            }
            {
                extern int jash_unquote(const char* in, char* out, size_t out_size);
                extern int jash_has_quotes(const char* str);
                if (jash_has_quotes(path)) {
                    char unq[512];
                    if (jash_unquote(path, unq, sizeof(unq)) >= 0)
                        snprintf(path, sizeof(path), "%s", unq);
                }
            }
            long fd = sys_open(path, 0 /*O_RDONLY*/, 0);
            unsigned rl = 0;
            if (fd >= 0) {
                long n = 0;
                while (rl + 1 < result_cap) {
                    n = sys_read((int)fd, result + rl, result_cap - rl - 1);
                    if (n < 0 && errno == EINTR) continue;
                    if (n < 0) { cmdsub_refuse_capture(); break; }
                    if (n == 0) break;
                    rl += (unsigned)n;
                }
                if (rl + 1 >= result_cap) {
                    char extra;
                    do { n = sys_read((int)fd, &extra, 1); }
                    while (n < 0 && errno == EINTR);
                    if (n < 0) cmdsub_refuse_capture();
                    else if (n > 0) cmdsub_refuse_output((unsigned)result_cap - 1);
                }
                sys_close((int)fd);
            }
            /* Trim trailing newlines (bash cmdsub semantics). */
            while (rl > 0 && (result[rl-1] == '\n' || result[rl-1] == '\r')) rl--;
            result[rl] = '\0';
            return result;
        }
    }

    /* OPT#2: Try builtin fast-path FIRST (saves pipe + fork overhead).
     * This handles $(pwd), $(echo foo), $(jobs), $(alias), etc. without
     * creating any pipes or forking. */
    unsigned builtin_len = 0;
    if (try_builtin_fastpath(s, cmd, result, (unsigned)result_cap, &builtin_len)) {
        /* Builtin executed — cache and return immediately */
        cache_cmd_config_t cfg;
        if (!jash_cmdsub_error() && strlen(result) < CACHE_VALUE_MAX &&
            cs_cacheable && cache_get_cmd_config(cmd, &cfg)) {
            cache_store(cmd, cwd, result, cfg.ttl, NULL, 0);
        }
        return result;
    }
    
    /* OPT#1: Use depth-indexed capture slot instead of save/restore */
    int depth = g_capture_depth;
    if (depth >= JASH_CMDSUB_MAX_DEPTH) depth = JASH_CMDSUB_MAX_DEPTH - 1;
    size_t capture_cap = JASH_CMDSUB_CAP;
    char* my_capture = cmdsub_slab(&g_capture_slots[depth], &capture_cap);
    unsigned* my_len = &g_capture_lens[depth];
    g_capture_depth = depth + 1;
    *my_len = 0;
    my_capture[0] = '\0';
    if (jash_cmdsub_error()) {
        result[0] = 0;
        g_capture_depth = depth;
        return result;
    }

    /* JINV-010 — `$( )` não pode travar quando o comando produz muito output.
     *
     * A captura usava um PIPE, mas quem escreve e quem lê são o MESMO processo:
     * `dup2(pipe[1], 1)` → roda o comando → só DEPOIS lê `pipe[0]`. O buffer do
     * pipe no kernel é fixo (64 KiB no macOS); passando disso, o `write` do
     * comando bloqueia esperando um leitor que só vai existir quando ele
     * terminar. Deadlock clássico de pipe em processo único.
     * Reproduzido em 2026-07-27: `v=$(cat arquivo_de_200KB)` trava para sempre.
     * Um agente que faça `x=$(cat log)` num log grande pendura a sessão inteira.
     *
     * Correção: capturar em ARQUIVO TEMPORÁRIO. Sem buffer limitado, portanto
     * sem deadlock possível, qualquer que seja o tamanho. O `unlink` imediato
     * faz o arquivo sumir do namespace na hora — nada a limpar depois, nem se
     * o processo morrer no meio.
     * Gate: tests/test_jinv010_cmdsub_size.sh
     */
    int capfd = -1;
    {
        const char* td = getenv("TMPDIR");
        char tmpl[512];
        snprintf(tmpl, sizeof tmpl, "%s/jash_cmdsub_XXXXXX",
                 (td && *td) ? td : jash_tmpdir());
        capfd = mkstemp(tmpl);
        if (capfd >= 0) unlink(tmpl);   /* anônimo a partir daqui */
    }
    int inner_rc = 1;
    if (capfd < 0) {
        cmdsub_refuse_capture();
    } else {
        int errpipe[2];
        if (sys_pipe(errpipe) < 0) {
            sys_close(capfd);
            capfd = -1;
            cmdsub_refuse_capture();
        } else {
            /* Do not let a child flush bytes buffered by its parent. The
             * child flush below is what makes stdio-based builtins capturable. */
            (void)fflush(NULL);
            long pid = sys_fork();
            if (pid == 0) {
                sys_close(errpipe[0]);
                unsigned char marker = 0;
                if (sys_dup2(capfd, 1) < 0) {
                    marker = 2;
                    long mw;
                    do { mw = sys_write(errpipe[1], &marker, 1); }
                    while (mw < 0 && errno == EINTR);
                    sys_exit(125);
                }
                sys_close(capfd);
                g_capture_write_error = 0;
                jash_param_expand_clear_error();
                int rc = jash_eval_line(s, pipe_writer, 0, cmd);
                if (fflush(NULL) != 0) g_capture_write_error = 1;
                if (g_capture_write_error) marker = 2;
                else if (jash_glob_error() || jash_cmdsub_error() ||
                    jash_param_expand_error()) {
                    marker = 1;
                }
                if (marker) {
                    long mw;
                    do { mw = sys_write(errpipe[1], &marker, 1); }
                    while (mw < 0 && errno == EINTR);
                }
                sys_close(errpipe[1]);
                sys_exit(rc);
            }
            sys_close(errpipe[1]);
            if (pid < 0) {
                sys_close(errpipe[0]);
                sys_close(capfd);
                capfd = -1;
                cmdsub_refuse_capture();
            } else {
                int status = 0;
                long wr;
                do { wr = sys_waitpid(pid, &status, 0); }
                while (wr < 0 && errno == EINTR);
                if (wr < 0) {
                    cmdsub_refuse_capture();
                } else if ((status & 0x7f) == 0) {
                    inner_rc = (status >> 8) & 0xff;
                } else {
                    inner_rc = 128 + (status & 0x7f);
                }

                unsigned char marker = 0;
                long mr;
                do { mr = sys_read(errpipe[0], &marker, 1); }
                while (mr < 0 && errno == EINTR);
                sys_close(errpipe[0]);
                if (mr < 0 || marker == 2) cmdsub_refuse_capture();
                else if (marker == 1) cmdsub_refuse_inner();

                if (lseek(capfd, 0, SEEK_SET) < 0) {
                    cmdsub_refuse_capture();
                } else {
                    long n = 0;
                    while (*my_len + 1 < capture_cap) {
                        n = sys_read(capfd, my_capture + *my_len,
                                     capture_cap - *my_len - 1);
                        if (n < 0 && errno == EINTR) continue;
                        if (n < 0) { cmdsub_refuse_capture(); break; }
                        if (n == 0) break;
                        *my_len += (unsigned)n;
                    }
                    if (*my_len + 1 >= capture_cap) {
                        char extra;
                        do { n = sys_read(capfd, &extra, 1); }
                        while (n < 0 && errno == EINTR);
                        if (n < 0) cmdsub_refuse_capture();
                        else if (n > 0)
                            cmdsub_refuse_output((unsigned)capture_cap - 1);
                    }
                }
                sys_close(capfd);
            }
        }
    }

    /* BUG-074d: assignment status comes from the child command. */
    g_jash_cmdsub_last_status = inner_rc;

    /* Trim trailing newlines (bash semantics) */
    while (*my_len > 0 &&
           (my_capture[*my_len - 1] == '\n' ||
            my_capture[*my_len - 1] == '\r')) {
        (*my_len)--;
    }
    my_capture[*my_len] = '\0';
    
    /* Copy result to stable slot (bounded by the result slab's capacity) */
    if (*my_len + 1 > result_cap) *my_len = (unsigned)result_cap - 1;
    memcpy(result, my_capture, *my_len + 1);
    result[*my_len] = '\0';
    
    /* SMART CACHE: Store result (BUG-086: only variable-free text) */
    cache_cmd_config_t cfg;
    if (!jash_cmdsub_error() && strlen(result) < CACHE_VALUE_MAX &&
        cs_cacheable && cache_get_cmd_config(cmd, &cfg)) {
        const char *dep_paths[CACHE_DEPS_MAX];
        char dep_bufs[CACHE_DEPS_MAX][512];
        int dep_count = 0;
        
        for (int i = 0; i < cfg.dep_count; i++) {
            if (cfg.dep_suffixes[i] && cwd) {
                snprintf(dep_bufs[i], sizeof(dep_bufs[i]), "%s/%s", cwd, cfg.dep_suffixes[i]);
                dep_paths[i] = dep_bufs[i];
                dep_count++;
            }
        }
        
        cache_store(cmd, cwd, result, cfg.ttl, dep_paths, dep_count);
    }
    
    /* OPT#1: Restore capture depth (O(1) instead of O(128KB)) */
    g_capture_depth = depth;
    
    return result;
}

/* Reentrancy depth for jash_expand_cmdsub's OWN output buffer — distinct
 * from __FUNC_DEPTH__ (shell function call depth) and from g_capture_depth
 * (run_and_capture's per-inner-command capture slot, above). Nested command
 * substitution like `$(echo $(echo deep))` recurses into THIS function
 * again (via run_and_capture -> jash_eval_line -> ...expand_cmdsub) while
 * the outer call's `out` buffer still has a partially-built result (e.g.
 * a literal prefix word written before the inner $(...) was reached).
 * Indexing out_ring by __FUNC_DEPTH__ alone left both calls aliasing the
 * SAME slot whenever no shell function was involved (the common case) —
 * the inner call would overwrite the outer's in-progress buffer out from
 * under it, silently dropping everything written before the nested
 * substitution. `echo outer $(echo $(echo deep))` produced "deep" instead
 * of "outer deep" until this was split into its own counter. */
static int g_expand_cmdsub_depth = 0;

/* BUG-031: when set, suppress unquoted word-splitting of $(...)/`...` output
 * (no IFS-whitespace collapse). Used by the assignment path — POSIX says the RHS
 * of `VAR=$(...)` is NOT field-split, so embedded newlines must survive. The
 * caller save/restores it around the expand call. */
int g_jash_cmdsub_no_split = 0;

/* ═══ JINV-087 — a saída de `$(…)` NÃO CITADA também é campo ══════════════
 *
 * `$(…)` é o outro produtor de expansão que conhece a própria citação (o `dq`
 * abaixo). O laço de emissão já colapsava CORRIDA DE BRANCO quando fora de
 * aspas — metade da regra do POSIX. A outra metade (caractere de IFS
 * NÃO-BRANCO delimita campo; branco FORA do IFS é dado) faltava, e era a
 * divisão cega do `for` que a fingia — dividindo também o que estava entre
 * aspas e o que era literal.
 *
 * Quando ligado, o byte sai pela porta única de param_expand.c. Desligado
 * (padrão), NADA muda: o caminho de argv continua exatamente como estava.
 * O laço do `for` liga em volta da chamada e desliga depois; run_and_capture
 * zera durante o comando interno, senão um `$( … )` aninhado herdaria o modo
 * de quem o encomendou. */
int g_jash_cmdsub_field_split = 0;

static const char* expand_cmdsub_impl(jash_state_t* s, const char* line,
                                      int known_present) {
    if (!line || (!known_present && !jash_has_cmdsub(line))) return line;

    /* ADR-0016 §4: lazily-allocated output ring (was 4.2 MB of .bss). */
    #define JASH_CMDSUB_OUT_SLOTS 32
    #define JASH_CMDSUB_OUT_CAP   (JASH_CMDSUB_CAP + 1)
    static char* out_ring[JASH_CMDSUB_OUT_SLOTS];
    int depth = g_expand_cmdsub_depth;
    if (depth >= JASH_CMDSUB_OUT_SLOTS) depth = JASH_CMDSUB_OUT_SLOTS - 1;
    /* Clear before allocating the slab: allocation failure itself sets the
     * refusal and must not be erased a few lines later. */
    if (depth == 0) jash_cmdsub_clear_error();
    g_expand_cmdsub_depth = depth + 1;
    size_t out_cap = JASH_CMDSUB_OUT_CAP;
    char* out = cmdsub_slab(&out_ring[depth], &out_cap);
    unsigned o = 0;
    const char* p = line;
    int sq = 0, dq = 0;
    /* JINV-088: a crase sem fecho encerrava o laço com um `break` NO MEIO do
     * ramo; com um buffer de heap vivo, esse `break` passaria por cima do
     * `free`. A porta única resolve o vazamento e este sinalizador leva a
     * intenção original (parar de montar a linha) até depois dela. */
    int cs_tick_unterminated = 0;

    /* JINV-088: a recusa é POR EXPANSÃO — o nível 0 limpa, os aninhados não,
     * senão um `$( )` de dentro apagaria o erro que o de fora precisa ver. */
    while (*p && o < out_cap - 1) {
        if (*p == '\\' && !sq && p[1]) {
            out[o++] = *p++;
            if (o < out_cap - 1) out[o++] = *p++;
            continue;
        }
        /* BUG-060: a ' inside "..." is literal — only toggle sq outside dq,
         * or `echo "a: '$(cmd)'"` treats the cmdsub as single-quoted. */
        if (*p == '\'' && !dq) { sq = !sq; out[o++] = *p++; continue; }
        if (*p == '"' && !sq) { dq = !dq; out[o++] = *p++; continue; }

        if (!sq && p[0] == '$' && p[1] == '(' && p[2] != '(') {
            p += 2;
            const char* start = p;
            int pdepth = 1;
            /* BUG-075: the closing-paren match must be QUOTE-AWARE — a `)`
             * inside "…" or '…' is data, not the end of the substitution, or
             * `$(echo "a)b")` closed early at `a)`.
             * BUG-100: it must also be COMMENT-AWARE — an unquoted `#` at a
             * word start comments out the rest of the line (POSIX parses the
             * embedded script), so a `)` inside the comment is not the end.
             * BUG-099: a nested `$(` INSIDE "…" enters a fresh unquoted
             * context (quoting restarts inside a substitution); its matching
             * `)` restores the enclosing quote state. A flat idq boolean
             * inverted quote parity at depth 2+ — track a per-paren-level
             * dq stack instead. */
            char isq = 0, idq = 0; int icmt = 0; char prevc = ' ';
            char dqsave[64]; dqsave[0] = 0;
            while (*p && pdepth > 0) {
                char c = *p;
                if (icmt) { if (c == '\n') icmt = 0; prevc = c; p++; continue; }
                if (isq) { if (c == '\'') isq = 0; prevc = c; p++; continue; }
                if (idq) {
                    if (c == '\\' && p[1]) { prevc = p[1]; p += 2; continue; }
                    if (c == '$' && p[1] == '(' && p[2] != '(') {
                        if (pdepth < (int)sizeof(dqsave)) dqsave[pdepth] = 1;
                        pdepth++; idq = 0; prevc = '('; p += 2; continue;
                    }
                    if (c == '"') idq = 0;
                    prevc = c; p++; continue;
                }
                if (c == '\\' && p[1]) { prevc = p[1]; p += 2; continue; }
                if (c == '\'') { isq = 1; prevc = c; p++; continue; }
                if (c == '"')  { idq = 1; prevc = c; p++; continue; }
                if (c == '#' && (prevc == ' ' || prevc == '\t' || prevc == '\n' ||
                                 prevc == ';' || prevc == '(' || prevc == '|' ||
                                 prevc == '&')) { icmt = 1; p++; continue; }
                if (c == '(') {
                    if (pdepth < (int)sizeof(dqsave)) dqsave[pdepth] = 0;
                    pdepth++;
                } else if (c == ')') {
                    pdepth--;
                    if (pdepth == 0) break;
                    if (pdepth < (int)sizeof(dqsave)) idq = dqsave[pdepth];
                }
                prevc = c; p++;
            }
            if (!*p) break;
            unsigned ilen = (unsigned)(p - start);
            /* JINV-088: pilha até o tamanho antigo (caso comum, zero malloc),
             * heap do tamanho EXATO acima. O clamp `ilen = sizeof(inner) - 1`
             * que morava aqui entregava ao avaliador um enunciado decapitado
             * — sem o `|`, sem o `> arquivo`, sem o `&&` do fim. */
            char inner_stack[JASH_CMDSUB_INNER_STACK];
            char* inner_heap = NULL;
            char* cleaned_heap = NULL;
            char* inner = inner_stack;
            size_t inner_cap = sizeof(inner_stack);
            if ((size_t)ilen + 1 > inner_cap) {
                inner_heap = (char*)malloc((size_t)ilen + 1);
                if (!inner_heap) { cmdsub_refuse_oom(ilen); goto cs_paren_done; }
                inner = inner_heap;
                inner_cap = (size_t)ilen + 1;
            }
            for (unsigned i = 0; i < ilen; i++) inner[i] = start[i];
            inner[ilen] = 0;
            p++;

            /* BUG-100: strip the unquoted #-comments from the inner text —
             * the inner eval does not strip them, so `$(echo hi # x)` would
             * print the comment text. BUG-099: only strip at nesting depth 0
             * of THIS inner text — a nested `$(` (even inside "…", where
             * quoting restarts) is re-expanded recursively and strips its
             * own comments; touching its bytes here would corrupt it. */
            {
                /* JINV-088: o SEGUNDO teto em série. `cleaned` nunca é maior
                 * que `inner` (o filtro só remove bytes; os dois ramos que
                 * escrevem dois consomem dois), então `inner_cap` é um
                 * tamanho exato — e a guarda `ci + 2 < sizeof(cleaned)` deixa
                 * de poder cortar. Consertar só o `inner` acima não movia o
                 * penhasco um byte (armadilha medida no JINV-086). */
                char cleaned_stack[JASH_CMDSUB_INNER_STACK];
                char* cleaned = cleaned_stack;
                size_t cleaned_cap = sizeof(cleaned_stack);
                /* `+3` é a FOLGA da guarda, não um teto: uma volta do laço
                 * pode escrever DOIS bytes (o ramo de `\` e o de `$(`), e a
                 * guarda conservadora `ci + 2 < cap` tem que continuar
                 * verdadeira até o último byte legítimo. Sem a folga, o
                 * conserto trocaria um corte de 512 por um corte de 2 — que
                 * é igualmente uma troca de comando. */
                size_t cleaned_need = (size_t)ilen + 3;
                if (cleaned_need > cleaned_cap) {
                    cleaned_heap = (char*)malloc(cleaned_need);
                    if (!cleaned_heap) { cmdsub_refuse_oom(ilen); goto cs_paren_done; }
                    cleaned = cleaned_heap;
                    cleaned_cap = cleaned_need;
                }
                unsigned ci = 0;
                char q2 = 0; int cm = 0; char pv = ' ';
                int ndep = 0; char ndq[64];
                for (unsigned i = 0; inner[i] && ci + 2 < cleaned_cap; i++) {
                    char cc = inner[i];
                    if (cm) { if (cc == '\n') { cm = 0; cleaned[ci++] = cc; pv = cc; } continue; }
                    if (q2 == '\'') { if (cc == '\'') q2 = 0; cleaned[ci++] = cc; pv = cc; continue; }
                    if (q2 == '"') {
                        if (cc == '\\' && inner[i+1]) { cleaned[ci++] = cc; cleaned[ci++] = inner[++i]; pv = inner[i]; continue; }
                        if (cc == '$' && inner[i+1] == '(' && inner[i+2] != '(') {
                            if (ndep < (int)sizeof(ndq)) ndq[ndep] = 1;
                            ndep++; q2 = 0;
                            cleaned[ci++] = cc; cleaned[ci++] = inner[++i]; pv = '('; continue;
                        }
                        if (cc == '"') q2 = 0;
                        cleaned[ci++] = cc; pv = cc; continue;
                    }
                    if (cc == '\\' && inner[i+1]) { cleaned[ci++] = cc; cleaned[ci++] = inner[++i]; pv = inner[i]; continue; }
                    if (cc == '\'' || cc == '"') { q2 = cc; cleaned[ci++] = cc; pv = cc; continue; }
                    if (cc == '(') {
                        if (ndep < (int)sizeof(ndq)) ndq[ndep] = 0;
                        ndep++; cleaned[ci++] = cc; pv = cc; continue;
                    }
                    if (cc == ')') {
                        if (ndep > 0) { ndep--; if (ndep < (int)sizeof(ndq)) q2 = ndq[ndep] ? '"' : 0; }
                        cleaned[ci++] = cc; pv = cc; continue;
                    }
                    if (ndep == 0 && cc == '#' &&
                        (pv == ' ' || pv == '\t' || pv == '\n' ||
                         pv == ';' || pv == '(' || pv == '|' ||
                         pv == '&')) { cm = 1; continue; }
                    cleaned[ci++] = cc; pv = cc;
                }
                /* trim trailing whitespace left by a stripped comment */
                while (ci > 0 && (cleaned[ci-1] == ' ' || cleaned[ci-1] == '\t' ||
                                  cleaned[ci-1] == '\n')) ci--;
                cleaned[ci] = 0;
                memcpy(inner, cleaned, ci + 1);
            }

            /* JINV-087: o comando INTERNO roda com a divisão de campos
             * DESLIGADA — senão um `for` que pediu divisão contamina tudo o
             * que a substituição executar por baixo (inclusive outro `$( )`
             * aninhado, que tem a própria citação). Modo é do chamador, não
             * do mundo. Mesma disciplina de save/restore que eval.c já usa
             * para g_jash_cmdsub_no_split. */
            int fld_saved = g_jash_cmdsub_field_split;
            int mark_saved = jash_ifs_word_mark_state();
            g_jash_cmdsub_field_split = 0;
            jash_ifs_word_mark_enable(0);
            const char* captured = run_and_capture(s, inner);
            g_jash_cmdsub_field_split = fld_saved;
            jash_ifs_word_mark_enable(mark_saved);
            /* Unquoted $(...) is subject to word splitting: bash collapses
             * any run of IFS whitespace (space/tab/newline) into a single
             * field boundary. We don't have a separate tokenizing pass over
             * the substituted text downstream, so emit that boundary as one
             * literal space right here. Inside "..." the result is a single
             * word — no splitting, captured text passes through verbatim. */
            int run_ws = 0;
            /* JINV-087: fora de aspas e com divisão de campos pedida, a regra
             * inteira do IFS (não só o colapso de branco) vale para a saída. */
            int fld = (!dq && !g_jash_cmdsub_no_split && g_jash_cmdsub_field_split);
            if (fld) jash_ifs_field_begin(s, 1);
            size_t fo = o;   /* JINV-087: a porta de campos conta em size_t */
            if (fld && jash_ifs_word_mark_wanted() && fo < out_cap - 1)
                out[fo++] = JASH_EXP_MARK;   /* JINV-087: a palavra teve expansão */
            while (*captured && fo < out_cap - 1) {
                char oc = *captured++;
                int is_ifs_ws = (!dq && !g_jash_cmdsub_no_split && (oc == ' ' || oc == '\t' || oc == '\n'));
                if (is_ifs_ws && !fld) {
                    if (!run_ws) { out[fo++] = ' '; run_ws = 1; }
                    continue;
                }
                run_ws = 0;
                char sh = oc;
                if (oc == '$')      sh = JASH_PROT_DOLLAR;
                else if (oc == '`') sh = JASH_PROT_BACKTICK;
                else if (oc == '"') sh = JASH_PROT_DQUOTE;   /* BUG-096 */
                else if (oc == '\'') sh = JASH_PROT_SQUOTE;  /* BUG-096 */
                else if (oc == '\\') sh = JASH_PROT_BSLASH;  /* BUG-097 */
                /* JINV-096: a SAIDA de um comando e dado — um `|` nela nunca
                 * foi digitado por ninguem. Depois da cadeia de cima porque
                 * os sentinelas dela nao sao metacaracteres (idempotente). */
                sh = jash_meta_shield(sh);   /* JINV-096 */
                if (fld) { jash_ifs_field_put(oc, sh, out, out_cap, &fo); continue; }
                out[fo++] = sh;
            }
            if (*captured) cmdsub_refuse_output((unsigned)out_cap - 1);
            if (fld) { jash_ifs_field_flush(out, out_cap, &fo); jash_ifs_field_end(); }
            o = (unsigned)fo;
cs_paren_done:                       /* JINV-088: porta de saída ÚNICA */
            free(inner_heap);
            free(cleaned_heap);
            /* Recusa alta: pare de montar a linha AGORA. O avaliador consulta
             * jash_cmdsub_error() e não executa nada — nunca um enunciado
             * pela metade com cara de sucesso. */
            if (g_cmdsub_err) break;
            continue;
        }

        /* ═══ JINV-089 — o `$` que NÃO abre expansão é DADO ═══════════════
         *
         * `$` seguido de crase não introduz expansão nenhuma: POSIX XCU 2.6
         * só reconhece `${`, `$(` e `$nome`, e o oráculo (`/bin/bash`)
         * concorda — `echo "$`echo n`"` imprime `$n`, literal.
         *
         * O jash expande REESCREVENDO TEXTO em passadas, e a substituição de
         * comando roda ANTES da expansão de parâmetros. No instante em que a
         * crase é trocada pela saída do comando, aquele `$` — inerte só
         * porque o byte seguinte era uma crase — passa a encostar num NOME.
         * A passada de parâmetros, que já só enxerga a cadeia pronta,
         * obedece: `$` + `bindir` vira o VALOR de bindir, e esse valor é
         * expandido MAIS UMA VEZ.
         *
         *   exec_prefix=NONE; bindir='${exec_prefix}/bin'
         *   eval ac_val=$`echo bindir`
         *     bash: ${exec_prefix}/bin      jash: NONE/bin   ← duas voltas
         *
         * É a mesma forma dos JINV-016/020/032/083/085/087: a informação
         * "isto é dado" EXISTIA no ponto da decisão e morreu antes do ponto
         * que precisava dela. Aqui ela é GRAVADA onde ainda se sabe — com o
         * mesmo sentinela que já blinda a SAÍDA da substituição (SECURITY
         * #15), aplicado agora ao `$` da FONTE que a antecede. Restaurado
         * por jash_cmdsub_unprotect, depois da passada de parâmetros.
         *
         * Gates: tests/test_jinv089_dollar_as_data.sh (a regra da FONTE)
         *        tests/test_jinv090_eval_no_reexpand.sh (o VALOR não volta a
         *        ser sintaxe — a consequência medida no `configure`).
         * Bloqueava 10 das 38 invocações de `configure` (--bindir NONE/bin). */

        /* JINV-089: `$$` é UM parâmetro (o PID), não dois sigilos em fila.
         *
         * Sem esta porta o escudo abaixo é LARGO DEMAIS e vira o bug oposto:
         * o laço emite o primeiro `$` cru, `p` para no SEGUNDO, e aquele
         * segundo `$` — que não é sigilo nenhum, é a segunda letra do nome do
         * parâmetro — casa com `$` seguido de crase e é blindado.
         *
         *   echo "$$`echo V`"
         *     bash: <pid>V     jash (escudo largo): $$V   ← o PID sumiu
         *
         * O escudo existe para não expandir o que não é expansão; blindar
         * aqui APAGA uma expansão legítima. `$$` é o único caso: é a única
         * forma em que o byte depois do sigilo é ele mesmo um `$`.
         * Secção C do gate (controle positivo) é quem prende esta metade. */
        if (!sq && p[0] == '$' && p[1] == '$') {
            out[o++] = *p++;
            if (o < out_cap - 1) out[o++] = *p++;
            continue;
        }

        if (!sq && p[0] == '$' && p[1] == '`') {   /* JINV-089 */
            out[o++] = JASH_PROT_DOLLAR;
            p++;
            continue;
        }

        if (!sq && *p == '`') {
            p++;
            /* BUG-098: POSIX 2.6.3 — inside `…`, \` escapes a backquote to
             * allow one level of nesting (and \\ / \$ are unescaped). Build
             * the inner text with those escapes RESOLVED so the nested
             * backticks execute when the inner command is evaluated. */
            /* JINV-088: o TERCEIRO teto em série, e o mesmo estrago — o
             * `char inner[512]` cortava o enunciado de ` … ` em silêncio
             * (`V=`printf %s <505 a's>`` devolvia 501). Duas passadas: a
             * primeira MEDE o texto já com os escapes resolvidos, a segunda
             * preenche. Pilha até o tamanho antigo, heap exato acima. */
            unsigned need = 0;
            for (const char* q = p; *q && *q != '`'; ) {
                if (q[0] == '\\' && (q[1] == '`' || q[1] == '\\' || q[1] == '$')) {
                    need++; q += 2; continue;
                }
                need++; q++;
            }
            char inner_stack[JASH_CMDSUB_INNER_STACK];
            char* inner_heap = NULL;
            char* inner = inner_stack;
            size_t inner_cap = sizeof(inner_stack);
            if ((size_t)need + 1 > inner_cap) {
                inner_heap = (char*)malloc((size_t)need + 1);
                if (!inner_heap) { cmdsub_refuse_oom(need); goto cs_tick_done; }
                inner = inner_heap;
                inner_cap = (size_t)need + 1;
            }
            unsigned ilen = 0;
            while (*p && *p != '`') {
                if (p[0] == '\\' && (p[1] == '`' || p[1] == '\\' || p[1] == '$')) {
                    if (ilen + 1 < inner_cap) inner[ilen++] = p[1];
                    p += 2; continue;
                }
                if (ilen + 1 < inner_cap) inner[ilen++] = *p;
                p++;
            }
            /* Crase não fechada: `p` parou no NUL. Sai pela porta única para
             * não vazar o buffer de heap desta volta. */
            if (!*p) { cs_tick_unterminated = 1; goto cs_tick_done; }
            inner[ilen] = 0;
            p++;

            /* JINV-087: o comando INTERNO roda com a divisão de campos
             * DESLIGADA — senão um `for` que pediu divisão contamina tudo o
             * que a substituição executar por baixo (inclusive outro `$( )`
             * aninhado, que tem a própria citação). Modo é do chamador, não
             * do mundo. Mesma disciplina de save/restore que eval.c já usa
             * para g_jash_cmdsub_no_split. */
            int fld_saved = g_jash_cmdsub_field_split;
            int mark_saved = jash_ifs_word_mark_state();
            g_jash_cmdsub_field_split = 0;
            jash_ifs_word_mark_enable(0);
            const char* captured = run_and_capture(s, inner);
            g_jash_cmdsub_field_split = fld_saved;
            jash_ifs_word_mark_enable(mark_saved);
            /* Unquoted $(...) is subject to word splitting: bash collapses
             * any run of IFS whitespace (space/tab/newline) into a single
             * field boundary. We don't have a separate tokenizing pass over
             * the substituted text downstream, so emit that boundary as one
             * literal space right here. Inside "..." the result is a single
             * word — no splitting, captured text passes through verbatim. */
            int run_ws = 0;
            /* JINV-087: fora de aspas e com divisão de campos pedida, a regra
             * inteira do IFS (não só o colapso de branco) vale para a saída. */
            int fld = (!dq && !g_jash_cmdsub_no_split && g_jash_cmdsub_field_split);
            if (fld) jash_ifs_field_begin(s, 1);
            size_t fo = o;   /* JINV-087: a porta de campos conta em size_t */
            if (fld && jash_ifs_word_mark_wanted() && fo < out_cap - 1)
                out[fo++] = JASH_EXP_MARK;   /* JINV-087: a palavra teve expansão */
            while (*captured && fo < out_cap - 1) {
                char oc = *captured++;
                int is_ifs_ws = (!dq && !g_jash_cmdsub_no_split && (oc == ' ' || oc == '\t' || oc == '\n'));
                if (is_ifs_ws && !fld) {
                    if (!run_ws) { out[fo++] = ' '; run_ws = 1; }
                    continue;
                }
                run_ws = 0;
                char sh = oc;
                if (oc == '$')      sh = JASH_PROT_DOLLAR;
                else if (oc == '`') sh = JASH_PROT_BACKTICK;
                else if (oc == '"') sh = JASH_PROT_DQUOTE;   /* BUG-096 */
                else if (oc == '\'') sh = JASH_PROT_SQUOTE;  /* BUG-096 */
                else if (oc == '\\') sh = JASH_PROT_BSLASH;  /* BUG-097 */
                /* JINV-096: a SAIDA de um comando e dado — um `|` nela nunca
                 * foi digitado por ninguem. Depois da cadeia de cima porque
                 * os sentinelas dela nao sao metacaracteres (idempotente). */
                sh = jash_meta_shield(sh);   /* JINV-096 */
                if (fld) { jash_ifs_field_put(oc, sh, out, out_cap, &fo); continue; }
                out[fo++] = sh;
            }
            if (*captured) cmdsub_refuse_output((unsigned)out_cap - 1);
            if (fld) { jash_ifs_field_flush(out, out_cap, &fo); jash_ifs_field_end(); }
            o = (unsigned)fo;
cs_tick_done:                        /* JINV-088: porta de saída ÚNICA */
            free(inner_heap);
            if (cs_tick_unterminated) break;   /* crase sem fecho: como antes */
            if (g_cmdsub_err) break;           /* recusa alta: nada roda */
            continue;
        }

        out[o++] = *p++;
    }
    if (*p && !g_cmdsub_err) cmdsub_refuse_output((unsigned)out_cap - 1);
    out[o] = 0;
    g_expand_cmdsub_depth = depth;
    return out;
}

const char* jash_expand_cmdsub(jash_state_t* s, const char* line) {
    return expand_cmdsub_impl(s, line, 0);
}

const char* jash_expand_cmdsub_known(jash_state_t* s, const char* line) {
    return expand_cmdsub_impl(s, line, 1);
}
