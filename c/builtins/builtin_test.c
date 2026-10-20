// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Builtin: test / [ — minimal POSIX test expression evaluation.
//
// Supported operators (POSIX subset):
//   -f FILE     test if file exists
//   -d DIR      test if dir exists
//   -z STRING   test if string is zero-length
//   -n STRING   test if string is non-zero-length
//   STRING      test if string is non-empty
//
// Both `test X` and `[ X ]` are supported ([ calls test internally).

#include "builtin.h"
#include <sys/syscalls.h>
#include <sys/stat.h>
#include <string.h>
#include <unistd.h>

/* Helper: check if file exists (via sys_stat simplified — just try open) */
static int file_exists(const char* path) {
    long fd = sys_open(path, 0, 0);  /* O_RDONLY */
    if (fd < 0) return 0;
    sys_close(fd);
    return 1;
}

/* Helper: path exists (any type) — `test -e` (BUG-020). Follows symlinks. */
static int path_exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* Helper: simple dir check (ends with / and is accessible) */
static int dir_exists(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    return S_ISDIR(st.st_mode);
}

static int file_readable(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uid_t uid = geteuid();
    uid_t gid = getegid();
    mode_t mode = st.st_mode;
    if (uid == 0) return 1;
    if (uid == st.st_uid) {
        return (mode & S_IRUSR) ? 1 : 0;
    }
    if (gid == st.st_gid) {
        return (mode & S_IRGRP) ? 1 : 0;
    }
    return (mode & S_IROTH) ? 1 : 0;
}

static int file_writable(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uid_t uid = geteuid();
    uid_t gid = getegid();
    mode_t mode = st.st_mode;
    if (uid == 0) return 1;
    if (uid == st.st_uid) {
        return (mode & S_IWUSR) ? 1 : 0;
    }
    if (gid == st.st_gid) {
        return (mode & S_IWGRP) ? 1 : 0;
    }
    return (mode & S_IWOTH) ? 1 : 0;
}

static int file_executable(const char* path) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    uid_t uid = geteuid();
    uid_t gid = getegid();
    mode_t mode = st.st_mode;
    if (uid == 0) return 1;
    if (uid == st.st_uid) {
        return (mode & S_IXUSR) ? 1 : 0;
    }
    if (gid == st.st_gid) {
        return (mode & S_IXGRP) ? 1 : 0;
    }
    return (mode & S_IXOTH) ? 1 : 0;
}

/* Helper: trim trailing whitespace in-place (returns new length). */
static int rtrim(char* s) {
    int len = 0;
    while (s[len]) len++;
    while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n'))
        s[--len] = '\0';
    return len;
}

/* Parse a long integer from a string. Returns 1 on success, 0 on failure. */
static int parse_long(const char* s, long* out) {
    if (!s || !*s) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (!*s) return 0;
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') { s++; }
    if (!*s) return 0;
    long val = 0;
    int digits = 0;
    while (*s >= '0' && *s <= '9') { val = val * 10 + (*s - '0'); s++; digits = 1; }
    if (!digits) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s) return 0;  /* trailing garbage */
    *out = neg ? -val : val;
    return 1;
}

/* Try to evaluate a binary comparison: LEFT OP RIGHT.
 * Supports numeric: -eq, -ne, -lt, -le, -gt, -ge
 * Supports string:  =, !=
 * Returns 0 (true/success) or 1 (false/failure).
 * Returns -1 if the expression is not a recognized binary comparison. */
static int eval_binary_cmp(const char* expr) {
    if (!expr || !*expr) return -1;

    /* Tokenize into: LEFT OP RIGHT (3 whitespace-separated tokens) */
    char buf[512];
    unsigned bl = 0;
    while (expr[bl] && bl + 1 < sizeof(buf)) { buf[bl] = expr[bl]; bl++; }
    buf[bl] = '\0';

    /* Skip leading ws */
    char* p = buf;
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return -1;

    /* Extract LEFT token */
    char* left = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (!*p) return -1;  /* only one token, not a binary comparison */
    *p++ = '\0';

    /* Skip ws */
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return -1;

    /* Extract OP token */
    char* op = p;
    while (*p && *p != ' ' && *p != '\t') p++;
    if (!*p) return -1;  /* only two tokens */
    *p++ = '\0';

    /* Skip ws */
    while (*p == ' ' || *p == '\t') p++;
    if (!*p) return -1;

    /* Extract RIGHT token (rest of string, trimmed) */
    char* right = p;
    /* Trim trailing ws */
    unsigned rlen = 0;
    while (right[rlen]) rlen++;
    while (rlen > 0 && (right[rlen-1] == ' ' || right[rlen-1] == '\t'))
        right[--rlen] = '\0';
    if (!rlen) return -1;

    /* Numeric comparisons */
    if (strcmp(op, "-eq") == 0 || strcmp(op, "-ne") == 0 ||
        strcmp(op, "-lt") == 0 || strcmp(op, "-le") == 0 ||
        strcmp(op, "-gt") == 0 || strcmp(op, "-ge") == 0) {
        long lval, rval;
        if (!parse_long(left, &lval) || !parse_long(right, &rval)) {
            /* Non-numeric operands for numeric operator — bash treats as error (exit 2),
             * but for while-loop safety, return false (1) so loops don't spin forever. */
            return 1;
        }
        int result = 0;
        if (strcmp(op, "-eq") == 0) result = (lval == rval);
        else if (strcmp(op, "-ne") == 0) result = (lval != rval);
        else if (strcmp(op, "-lt") == 0) result = (lval < rval);
        else if (strcmp(op, "-le") == 0) result = (lval <= rval);
        else if (strcmp(op, "-gt") == 0) result = (lval > rval);
        else if (strcmp(op, "-ge") == 0) result = (lval >= rval);
        return result ? 0 : 1;
    }

    /* String comparisons */
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0) {
        return (strcmp(left, right) == 0) ? 0 : 1;
    }
    if (strcmp(op, "!=") == 0) {
        return (strcmp(left, right) != 0) ? 0 : 1;
    }

    return -1;  /* Not a recognized binary operator */
}

/* Evaluate one test expression. Returns 0 (success) or 1 (failure). */
static int eval_test(const char* expr) {
    if (!expr || !*expr) return 1;  /* Empty = false */

    /* Consume leading whitespace */
    while (*expr == ' ' || *expr == '\t') expr++;
    if (!*expr) return 1;

    /* Work on a mutable copy so we can trim paths. */
    char buf[512];
    unsigned bl = 0;
    while (expr[bl] && bl + 1 < sizeof(buf)) { buf[bl] = expr[bl]; bl++; }
    buf[bl] = '\0';

    /* Trim trailing whitespace */
    rtrim(buf);

    /* FIRST: Try binary comparison (LEFT OP RIGHT) — must check before
     * the catch-all "non-empty string" test, otherwise "0 -lt 3" is
     * always true because "0 -lt 3" is a non-empty string. */
    {
        int cmp = eval_binary_cmp(buf);
        if (cmp >= 0) return cmp;
    }

    /* -e FILE (exists, any type) — BUG-020 */
    if (buf[0] == '-' && buf[1] == 'e' &&
        (buf[2] == ' ' || buf[2] == '\t')) {
        const char* path = buf + 3;
        while (*path == ' ' || *path == '\t') path++;
        char pbuf[256];
        unsigned pi = 0;
        while (path[pi] && pi + 1 < sizeof(pbuf)) { pbuf[pi] = path[pi]; pi++; }
        pbuf[pi] = '\0';
        rtrim(pbuf);
        return path_exists(pbuf) ? 0 : 1;
    }

    /* -f FILE */
    if (buf[0] == '-' && buf[1] == 'f' &&
        (buf[2] == ' ' || buf[2] == '\t')) {
        const char* path = buf + 3;
        while (*path == ' ' || *path == '\t') path++;
        /* Strip trailing whitespace from path (critical for [ -f file ] form). */
        char pbuf[256];
        unsigned pi = 0;
        while (path[pi] && pi + 1 < sizeof(pbuf)) { pbuf[pi] = path[pi]; pi++; }
        pbuf[pi] = '\0';
        rtrim(pbuf);
        return file_exists(pbuf) ? 0 : 1;
    }

    /* -d DIR */
    if (buf[0] == '-' && buf[1] == 'd' &&
        (buf[2] == ' ' || buf[2] == '\t')) {
        const char* path = buf + 3;
        while (*path == ' ' || *path == '\t') path++;
        char pbuf[256];
        unsigned pi = 0;
        while (path[pi] && pi + 1 < sizeof(pbuf)) { pbuf[pi] = path[pi]; pi++; }
        pbuf[pi] = '\0';
        rtrim(pbuf);
        return dir_exists(pbuf) ? 0 : 1;
    }

    /* -r FILE */
    if (buf[0] == '-' && buf[1] == 'r' &&
        (buf[2] == ' ' || buf[2] == '\t')) {
        const char* path = buf + 3;
        while (*path == ' ' || *path == '\t') path++;
        char pbuf[256];
        unsigned pi = 0;
        while (path[pi] && pi + 1 < sizeof(pbuf)) { pbuf[pi] = path[pi]; pi++; }
        pbuf[pi] = '\0';
        rtrim(pbuf);
        return file_readable(pbuf) ? 0 : 1;
    }

    /* -w FILE */
    if (buf[0] == '-' && buf[1] == 'w' &&
        (buf[2] == ' ' || buf[2] == '\t')) {
        const char* path = buf + 3;
        while (*path == ' ' || *path == '\t') path++;
        char pbuf[256];
        unsigned pi = 0;
        while (path[pi] && pi + 1 < sizeof(pbuf)) { pbuf[pi] = path[pi]; pi++; }
        pbuf[pi] = '\0';
        rtrim(pbuf);
        return file_writable(pbuf) ? 0 : 1;
    }

    /* -x FILE */
    if (buf[0] == '-' && buf[1] == 'x' &&
        (buf[2] == ' ' || buf[2] == '\t')) {
        const char* path = buf + 3;
        while (*path == ' ' || *path == '\t') path++;
        char pbuf[256];
        unsigned pi = 0;
        while (path[pi] && pi + 1 < sizeof(pbuf)) { pbuf[pi] = path[pi]; pi++; }
        pbuf[pi] = '\0';
        rtrim(pbuf);
        return file_executable(pbuf) ? 0 : 1;
    }

    /* -z STRING (zero-length) */
    if (buf[0] == '-' && buf[1] == 'z' &&
        (buf[2] == ' ' || buf[2] == '\t' || buf[2] == '\0')) {
        const char* str = buf + 3;
        while (*str == ' ' || *str == '\t') str++;
        /* Trim trailing ws for -z */
        char sbuf[256];
        unsigned si = 0;
        while (str[si] && si + 1 < sizeof(sbuf)) { sbuf[si] = str[si]; si++; }
        sbuf[si] = '\0';
        rtrim(sbuf);
        return (sbuf[0] == 0) ? 0 : 1;
    }

    /* -n STRING (non-zero-length) or just STRING */
    if ((buf[0] == '-' && buf[1] == 'n' &&
         (buf[2] == ' ' || buf[2] == '\t' || buf[2] == '\0')) ||
        (buf[0] != '-')) {
        const char* str = buf;
        if (buf[0] == '-' && buf[1] == 'n') {
            str = buf + 3;
            while (*str == ' ' || *str == '\t') str++;
        }
        /* Trim trailing ws */
        char sbuf[256];
        unsigned si = 0;
        while (str[si] && si + 1 < sizeof(sbuf)) { sbuf[si] = str[si]; si++; }
        sbuf[si] = '\0';
        rtrim(sbuf);
        return (sbuf[0] != 0) ? 0 : 1;
    }

    return 1;  /* Unknown expression = false */
}

/* `test EXPR` — builtin */
/* BUG-071: POSIX `-a` (AND) / `-o` (OR) connectives. Precedence: `-o` binds
 * looser than `-a` (POSIX), so split on top-level ` -o ` first, then ` -a `,
 * then hand each atom to eval_test. Space-delimited tokens; short-circuit.
 * Returns 0 (true) / 1 (false). F E R M I ∞ H A R T */
static const char* test_find_conn(const char* expr, char c) {
    /* first occurrence of the space-delimited token " -<c> " */
    for (const char* p = expr; p[0] && p[1] && p[2] && p[3]; p++)
        if (p[0] == ' ' && p[1] == '-' && p[2] == c && p[3] == ' ')
            return p;
    return NULL;
}

static int eval_test_and(const char* expr) {
    const char* op = test_find_conn(expr, 'a');
    if (!op) return eval_test(expr);
    char left[512];
    size_t ll = (size_t)(op - expr);
    if (ll >= sizeof(left)) ll = sizeof(left) - 1;
    memcpy(left, expr, ll); left[ll] = '\0';
    if (eval_test(left) != 0) return 1;          /* left false → AND false */
    return eval_test_and(op + 4);                /* skip " -a " */
}

static int eval_test_or(const char* expr) {
    const char* op = test_find_conn(expr, 'o');
    if (!op) return eval_test_and(expr);
    char left[512];
    size_t ll = (size_t)(op - expr);
    if (ll >= sizeof(left)) ll = sizeof(left) - 1;
    memcpy(left, expr, ll); left[ll] = '\0';
    if (eval_test_and(left) == 0) return 0;      /* left true → OR true */
    return eval_test_or(op + 4);                 /* skip " -o " */
}

int jash_builtin_test(jash_state_t* s, jash_writer_t w, void* ctx,
                      const char* args) {
    (void)s; (void)w; (void)ctx;
    return eval_test_or(args ? args : "");
}

/* ════════════════════════════════════════════════════════════════════════
 * JINV-066 — `!` dentro de `[ … ]` tem que INVERTER.
 *
 * O que havia acima (e continua ali, intacto, como porta de string legada)
 * avalia a expressao lendo o RESTO DA LINHA como texto: procura um prefixo
 * `-f `, `-d `, `-z `… e, se nada casa, cai num apanha-tudo "string nao
 * vazia = verdadeiro". O `!` nunca foi um caso desse reconhecedor. Entao
 * `[ ! -f /etc/hosts ]` chegava como a string `! -f /etc/hosts `, nao casava
 * com nenhum prefixo, e caia no apanha-tudo: string nao vazia -> VERDADEIRO.
 *
 * Nao era um `!` "ignorado". Era um `!` virando DADO — a mesma familia de
 * JINV-016/019/022 (dado que vira comando), so que na direcao inversa: o
 * operador virou operando, e o teste inteiro passou a ser "esta linha tem
 * algum caractere?", que e sempre verdade.
 *
 * ═══ POR QUE ISTO E DESTRUICAO DE DADO, NAO COSMETICA ═══
 *
 *     [ ! -f "$backup" ] && rm -rf "$dir"
 *
 * No bash isso so apaga se o backup SUMIU. No jash apagava SEMPRE, com rc 0
 * e sem uma linha de diagnostico. Toda clausula de guarda escrita no idioma
 * mais comum do Unix — `if [ ! -f "$cfg" ]`, `if [ ! -d "$dest" ]` — falhava
 * ABERTA, e sempre no sentido de executar a acao destrutiva. `[[ ! … ]]`
 * funcionava (parser/conditional.c tem um `!` de verdade), o que tornava o
 * defeito ainda mais invisivel: quem testava com colchete duplo via tudo bem.
 *
 * ═══ O CONSERTO: contar ARGUMENTOS, nao casar prefixos ═══
 *
 * A semantica do `test` do POSIX nao e definida por prefixos: e definida pela
 * CONTAGEM de argumentos (0,1,2,3,4,n), e cada contagem tem regra propria.
 * Um reconhecedor de prefixo nao consegue expressar isso — por isso o `!`
 * nao tinha onde morar. A porta nova recebe argv de verdade
 * (`argv_fn`/core/argv.c, ADR-0036 W2), entao `[ ! "a b" = "a b" ]` tambem
 * para de perder a fronteira da palavra citada, que o caminho de string
 * perdia junto.
 *
 * Cada regra abaixo foi COPIADA do oraculo (/bin/bash 3.2), nao adivinhada —
 * inclusive as tres que ninguem teria escrito de memoria:
 *
 *   1. Com 3 argumentos, `-a`/`-o` no meio NAO sao os conectivos do parser:
 *      sao um E/OU entre dois testes de UM argumento. Por isso
 *      `[ -f -a /nope ]` e VERDADEIRO no bash (duas strings nao vazias) e
 *      `[ "" -a "" ]` e falso.
 *   2. Essa regra vem ANTES do `!`: `[ ! -a /etc ]` e verdadeiro
 *      ("!" e "/etc" sao nao vazias), nao `!(-a /etc)`.
 *   3. So o caminho do analisador geral (5+ argumentos, ou 4 sem `!`)
 *      reclama de "too many arguments"; as aridades pequenas consomem tudo.
 *
 * Erro de sintaxe devolve 2 (nao 1) e escreve no stderr, como o bash: um
 * `[ x y z ]` mal escrito nao pode ser indistinguivel de "falso".
 *
 * Gate: tests/test_jinv066_test_negation.sh (bash e o oraculo em cada
 * assercao, nunca um rc escrito a mao).
 * ════════════════════════════════════════════════════════════════════════ */

/* Verdadeiro/falso INTERNOS (1/0), convertidos para rc no fim — e a convencao
 * do bash e trocar aqui e a forma classica de inverter um teste sem querer. */
enum { JT_F = 0, JT_T = 1 };

typedef struct {
    int            argc;
    char* const*   argv;
    int            pos;
    int            err;      /* 1 = erro de sintaxe ja relatado -> rc 2 */
    const char*    prog;     /* "test" ou "[" — so para o diagnostico */
    jash_state_t*  st;       /* para `-o OPCAO` (set -e/-u/…) */
} jtest_t;

static int jt_expr(jtest_t* t);
static int jt_term(jtest_t* t);

static void jt_syntax(jtest_t* t, const char* what, const char* who) {
    if (t->err) return;             /* o primeiro erro e o que vale */
    t->err = 1;
    if (who)
        jash_writef(jash_stderr_writer, 0, "jash: %s: %s: %s\n",
                    t->prog, who, what);
    else
        jash_writef(jash_stderr_writer, 0, "jash: %s: %s\n", t->prog, what);
}

/* Fim da expressao onde ainda faltava argumento (bash: beyond()). */
static int jt_beyond(jtest_t* t) {
    jt_syntax(t, "argument expected", NULL);
    return JT_F;
}

static int jt_is(const char* s, const char* lit) {
    return s && strcmp(s, lit) == 0;
}

/* Teste de UM argumento: verdadeiro sse a string nao e vazia. */
static int jt_one(const char* s) { return (s && s[0] != '\0') ? JT_T : JT_F; }

/* Parece uma CHAVE (`-` mais no maximo um caractere)? Nao diz se e valida —
 * `-q` e `-` passam aqui e viram erro de sintaxe logo depois, que e
 * exatamente o que o bash faz. Uma string mais longa (`-abc`) NAO e chave:
 * e um operando comum. */
static int jt_switch_form(const char* s) {
    return s && s[0] == '-' && (s[1] == '\0' || s[2] == '\0');
}

/* `-X` de uma letra so, e uma das letras que o bash 3.2 reconhece. */
static int jt_unop(const char* s) {
    if (!s || s[0] != '-' || s[1] == '\0' || s[2] != '\0') return 0;
    switch (s[1]) {
    case 'a': case 'b': case 'c': case 'd': case 'e': case 'f':
    case 'g': case 'h': case 'k': case 'n': case 'o': case 'p':
    case 'r': case 's': case 't': case 'u': case 'w': case 'x':
    case 'z':
    case 'G': case 'L': case 'N': case 'O': case 'S':
        return 1;
    default:
        return 0;
    }
}

static int jt_binop(const char* s) {
    if (!s) return 0;
    if (strcmp(s, "=")  == 0 || strcmp(s, "==") == 0 ||
        strcmp(s, "!=") == 0 || strcmp(s, "<")  == 0 ||
        strcmp(s, ">")  == 0)
        return 1;
    if (strcmp(s, "-eq") == 0 || strcmp(s, "-ne") == 0 ||
        strcmp(s, "-lt") == 0 || strcmp(s, "-le") == 0 ||
        strcmp(s, "-gt") == 0 || strcmp(s, "-ge") == 0 ||
        strcmp(s, "-nt") == 0 || strcmp(s, "-ot") == 0 ||
        strcmp(s, "-ef") == 0)
        return 1;
    return 0;
}

/* `-o NOME` — estado de uma opcao do shell. Nome desconhecido e FALSO, nao
 * erro (bash faz assim: `[ -o naoexiste ]` sai 1, nao 2). */
static int jt_shell_opt(jash_state_t* s, const char* name) {
    if (!s || !name) return JT_F;
    const struct { const char* n; int v; } tab[] = {
        { "errexit",   s->shell_opts.errexit   },
        { "nounset",   s->shell_opts.nounset   },
        { "xtrace",    s->shell_opts.xtrace    },
        { "verbose",   s->shell_opts.verbose   },
        { "pipefail",  s->shell_opts.pipefail  },
        { "posix",     s->shell_opts.posix     },
        { "emacs",     s->shell_opts.emacs     },
        { "vi",        s->shell_opts.vi        },
        { "errtrace",  s->shell_opts.errtrace  },
        { "noglob",    s->shell_opts.noglob    },
        { "noclobber", s->shell_opts.noclobber },
        { "monitor",   s->shell_opts.monitor   },
    };
    for (unsigned i = 0; i < sizeof(tab) / sizeof(tab[0]); i++)
        if (strcmp(tab[i].n, name) == 0) return tab[i].v ? JT_T : JT_F;
    return JT_F;
}

/* Um teste unario aplicado ao operando. Nunca escreve erro: um arquivo
 * ausente e FALSO, nao erro (essa e a diferenca entre `-f` e `-eq`). */
static int jt_unary(jtest_t* t, const char* op, const char* arg) {
    struct stat st;
    if (!arg) arg = "";
    switch (op[1]) {
    case 'a': case 'e': return stat(arg, &st) == 0 ? JT_T : JT_F;
    /* access() em vez do teste manual de bits: ele enxerga grupos
     * suplementares e ACL, que a checagem de modo a mao errava. */
    case 'r': return access(arg, R_OK) == 0 ? JT_T : JT_F;
    case 'w': return access(arg, W_OK) == 0 ? JT_T : JT_F;
    case 'x': return access(arg, X_OK) == 0 ? JT_T : JT_F;
    case 'f': return (stat(arg, &st) == 0 && S_ISREG(st.st_mode))  ? JT_T : JT_F;
    case 'd': return (stat(arg, &st) == 0 && S_ISDIR(st.st_mode))  ? JT_T : JT_F;
    case 'b': return (stat(arg, &st) == 0 && S_ISBLK(st.st_mode))  ? JT_T : JT_F;
    case 'c': return (stat(arg, &st) == 0 && S_ISCHR(st.st_mode))  ? JT_T : JT_F;
    case 'p': return (stat(arg, &st) == 0 && S_ISFIFO(st.st_mode)) ? JT_T : JT_F;
    case 'S': return (stat(arg, &st) == 0 && S_ISSOCK(st.st_mode)) ? JT_T : JT_F;
    case 's': return (stat(arg, &st) == 0 && st.st_size > 0)       ? JT_T : JT_F;
    case 'u': return (stat(arg, &st) == 0 && (st.st_mode & S_ISUID)) ? JT_T : JT_F;
    case 'g': return (stat(arg, &st) == 0 && (st.st_mode & S_ISGID)) ? JT_T : JT_F;
    case 'k': return (stat(arg, &st) == 0 && (st.st_mode & S_ISVTX)) ? JT_T : JT_F;
    case 'O': return (stat(arg, &st) == 0 && st.st_uid == geteuid()) ? JT_T : JT_F;
    case 'G': return (stat(arg, &st) == 0 && st.st_gid == getegid()) ? JT_T : JT_F;
    case 'N': return (stat(arg, &st) == 0 && st.st_mtime > st.st_atime) ? JT_T : JT_F;
    /* -h/-L olham o proprio link (lstat), nao o alvo. */
    case 'h': case 'L':
        return (lstat(arg, &st) == 0 && S_ISLNK(st.st_mode)) ? JT_T : JT_F;
    case 't': {
        long fd;
        if (!parse_long(arg, &fd) || fd < 0 || fd > 1024) return JT_F;
        return isatty((int)fd) ? JT_T : JT_F;
    }
    case 'z': return arg[0] == '\0' ? JT_T : JT_F;
    case 'n': return arg[0] != '\0' ? JT_T : JT_F;
    case 'o': return jt_shell_opt(t->st, arg);
    default:  return JT_F;
    }
}

/* mtime em SEGUNDOS, como o bash 3.2 — comparar nanossegundos faria
 * `[ a -nt b ]` divergir do oraculo para arquivos criados no mesmo segundo. */
static int jt_filecomp(const char* a, const char* b, int newer) {
    struct stat sa, sb;
    int ha = (stat(a, &sa) == 0);
    int hb = (stat(b, &sb) == 0);
    if (ha && hb)
        return newer ? (sa.st_mtime > sb.st_mtime ? JT_T : JT_F)
                     : (sa.st_mtime < sb.st_mtime ? JT_T : JT_F);
    if (newer) return (ha && !hb) ? JT_T : JT_F;   /* a existe, b nao */
    return (!ha && hb) ? JT_T : JT_F;              /* b existe, a nao */
}

static int jt_binary(jtest_t* t, const char* l, const char* op, const char* r) {
    if (!l) { l = ""; }
    if (!r) { r = ""; }
    if (strcmp(op, "=") == 0 || strcmp(op, "==") == 0)
        return strcmp(l, r) == 0 ? JT_T : JT_F;
    if (strcmp(op, "!=") == 0) return strcmp(l, r) != 0 ? JT_T : JT_F;
    if (strcmp(op, "<")  == 0) return strcmp(l, r) <  0 ? JT_T : JT_F;
    if (strcmp(op, ">")  == 0) return strcmp(l, r) >  0 ? JT_T : JT_F;
    if (strcmp(op, "-nt") == 0) return jt_filecomp(l, r, 1);
    if (strcmp(op, "-ot") == 0) return jt_filecomp(l, r, 0);
    if (strcmp(op, "-ef") == 0) {
        struct stat sa, sb;
        if (stat(l, &sa) != 0 || stat(r, &sb) != 0) return JT_F;
        return (sa.st_dev == sb.st_dev && sa.st_ino == sb.st_ino) ? JT_T : JT_F;
    }
    /* Numericos: operando nao numerico e ERRO (rc 2), nao "falso". Devolver
     * falso aqui foi o que o caminho legado fazia "para o while nao girar" —
     * mas engolir a diferenca entre `1 -eq 1` falso e `x -eq 1` mal escrito
     * e exatamente a classe de bug que este arquivo esta consertando. */
    long a = 0, b = 0;
    if (!parse_long(l, &a)) { jt_syntax(t, "integer expression expected", l); return JT_F; }
    if (!parse_long(r, &b)) { jt_syntax(t, "integer expression expected", r); return JT_F; }
    if (strcmp(op, "-eq") == 0) return a == b ? JT_T : JT_F;
    if (strcmp(op, "-ne") == 0) return a != b ? JT_T : JT_F;
    if (strcmp(op, "-lt") == 0) return a <  b ? JT_T : JT_F;
    if (strcmp(op, "-le") == 0) return a <= b ? JT_T : JT_F;
    if (strcmp(op, "-gt") == 0) return a >  b ? JT_T : JT_F;
    if (strcmp(op, "-ge") == 0) return a >= b ? JT_T : JT_F;
    return JT_F;
}

/* `-X OPERANDO` no analisador geral: consome os dois e avanca. */
static int jt_unary_at_pos(jtest_t* t) {
    const char* op = t->argv[t->pos];
    /* `-t` sem operando vale `-t 1` (bash). Os outros exigem operando. */
    if (op[1] == 't' && t->pos + 1 >= t->argc) { t->pos++; return jt_unary(t, op, "1"); }
    if (t->pos + 1 >= t->argc) { t->pos++; return jt_beyond(t); }
    int v = jt_unary(t, op, t->argv[t->pos + 1]);
    t->pos += 2;
    return v;
}

/* Um termo: `!`s a esquerda, `( … )`, binario, unario, ou string. */
static int jt_term(jtest_t* t) {
    if (t->err) return JT_F;
    if (t->pos >= t->argc) return jt_beyond(t);

    /* `!`s empilhados — `[ ! ! -f x ]` e `[ ! ! ! -f x ]` tem que dar
     * respostas DIFERENTES, entao a paridade e contada, nao ignorada. */
    if (jt_is(t->argv[t->pos], "!")) {
        int odd = 0;
        while (t->pos < t->argc && jt_is(t->argv[t->pos], "!")) {
            t->pos++;
            if (t->pos >= t->argc) return jt_beyond(t);
            odd = 1 - odd;
        }
        int v = jt_term(t);
        if (t->err) return JT_F;
        return odd ? (v ? JT_F : JT_T) : v;
    }

    if (jt_is(t->argv[t->pos], "(")) {
        t->pos++;
        if (t->pos >= t->argc) return jt_beyond(t);
        int v = jt_expr(t);
        if (t->err) return JT_F;
        if (t->pos >= t->argc) return jt_beyond(t);
        if (!jt_is(t->argv[t->pos], ")")) {
            jt_syntax(t, "`)' expected", t->argv[t->pos]);
            return JT_F;
        }
        t->pos++;
        return v;
    }

    /* Binario antes de unario: em `[ -f = -f ]` o `=` no meio manda. */
    if (t->pos + 3 <= t->argc && jt_binop(t->argv[t->pos + 1])) {
        int v = jt_binary(t, t->argv[t->pos], t->argv[t->pos + 1],
                          t->argv[t->pos + 2]);
        t->pos += 3;
        return v;
    }

    /* Forma de CHAVE: `-` seguido de no maximo uma letra. Um `-` sozinho
     * entra aqui tambem (e o bash faz o mesmo): nao e uma string comum, e
     * uma chave malformada — `[ "" -a /nope -a - ]` e erro de sintaxe, nao
     * "falso". Achado pelo diferencial aleatorio contra o oraculo; escrever
     * `s[1] != '\0'` aqui parecia mais defensivo e mudava a resposta. */
    if (jt_switch_form(t->argv[t->pos])) {
        if (jt_unop(t->argv[t->pos])) return jt_unary_at_pos(t);
        jt_syntax(t, "unary operator expected", t->argv[t->pos]);
        return JT_F;
    }

    int v = jt_one(t->argv[t->pos]);
    t->pos++;
    return v;
}

static int jt_and(jtest_t* t) {
    int v = jt_term(t);
    while (!t->err && t->pos < t->argc && jt_is(t->argv[t->pos], "-a")) {
        t->pos++;
        /* Os dois lados SEMPRE sao avaliados (o bash tambem faz assim): o
         * lado direito precisa ser consumido para o ponteiro avancar, senao
         * sobra token e vira "too many arguments". */
        int v2 = jt_term(t);
        v = (v && v2) ? JT_T : JT_F;
    }
    return t->err ? JT_F : v;
}

static int jt_expr(jtest_t* t) {
    if (t->err) return JT_F;
    if (t->pos >= t->argc) return jt_beyond(t);
    int v = jt_and(t);
    while (!t->err && t->pos < t->argc && jt_is(t->argv[t->pos], "-o")) {
        t->pos++;
        int v2 = jt_and(t);
        v = (v || v2) ? JT_T : JT_F;
    }
    return t->err ? JT_F : v;
}

/* Dois argumentos a partir de t->pos.
 *
 * O AVANCO DO PONTEIRO aqui nao e detalhe: e o que distingue
 * `[ ! -f x ]` (valido) de `[ ! ! x ]` (que o bash recusa com "too many
 * arguments"). O ramo unario consome os DOIS tokens; o ramo `! STRING` nao
 * consome nada — e e essa sobra que faz o teste de cauda reprovar quando
 * `! STRING` aparece dentro de uma expressao de 3 argumentos. Copiado do
 * oraculo; ninguem escreveria isto de memoria. */
static int jt_two(jtest_t* t) {
    const char* a = t->argv[t->pos];
    const char* b = t->argv[t->pos + 1];
    if (jt_is(a, "!")) return b[0] == '\0' ? JT_T : JT_F;   /* nao avanca */
    if (jt_switch_form(a) && jt_unop(a)) {
        int v = jt_unary(t, a, b);
        t->pos += 2;
        return v;
    }
    jt_syntax(t, "unary operator expected", a);
    return JT_F;
}

/* Tres argumentos a partir de t->pos. A ORDEM destas regras foi medida
 * contra o bash, nao deduzida — ver o cabecalho deste bloco. */
static int jt_three(jtest_t* t) {
    const char* a = t->argv[t->pos];
    const char* op = t->argv[t->pos + 1];
    const char* b = t->argv[t->pos + 2];

    /* Os ramos que "consomem tudo" marcam pos = argc, como o bash. So o ramo
     * do `!` deixa o ponteiro onde jt_two() parou — e por isso `[ ! ! x ]`
     * reprova com "too many arguments" enquanto `[ ! -f x ]` passa. */
    if (jt_binop(op)) {
        int v = jt_binary(t, a, op, b);
        t->pos = t->argc;
        return v;
    }
    /* `-a`/`-o` com 3 argumentos NAO sao o conectivo do analisador: sao um
     * E/OU entre dois testes de UM argumento. `[ -f -a /nope ]` e VERDADEIRO
     * no bash — duas strings nao vazias. */
    if (jt_is(op, "-a")) { t->pos = t->argc; return (jt_one(a) && jt_one(b)) ? JT_T : JT_F; }
    if (jt_is(op, "-o")) { t->pos = t->argc; return (jt_one(a) || jt_one(b)) ? JT_T : JT_F; }
    if (jt_is(a, "!")) {
        t->pos++;
        int v = jt_two(t);
        /* At arity 3 the whole expression is consumed by definition, exactly
         * like the -a/-o arms above. jt_two's `! STRING` case deliberately
         * does not advance (see jt_two), so without this the tail check fired
         * "too many arguments" on `[ ! ! x ]` and `[ ! ! ! ]` — both rc 0 in
         * bash. */
        t->pos = t->argc;
        return t->err ? JT_F : (v ? JT_F : JT_T);
    }
    if (jt_is(a, "(") && jt_is(b, ")")) { t->pos = t->argc; return jt_one(op); }
    jt_syntax(t, "binary operator expected", op);
    return JT_F;
}

/* O avaliador POSIX completo. `argc`/`argv` sao SO a expressao (para `[`, o
 * `]` final ja saiu). Devolve rc de shell: 0 verdadeiro, 1 falso, 2 sintaxe. */
static int jt_eval(jash_state_t* s, const char* prog, int argc, char* const* argv) {
    jtest_t t;
    t.argc = argc; t.argv = argv; t.pos = 0; t.err = 0; t.prog = prog; t.st = s;

    int value;
    int checked_tail = 0;   /* so o caminho do analisador geral reclama de sobra */

    switch (argc) {
    /* 0/1/2 consomem a expressao inteira por definicao — o bash forca
     * pos = argc nessas aridades, entao `[ ! x ]` (que nao avanca) nao pode
     * cair no teste de cauda. */
    case 0:  value = JT_F; break;
    case 1:  value = jt_one(argv[0]); break;
    case 2:  value = jt_two(&t); break;
    case 3:  value = jt_three(&t); checked_tail = 1; break;
    case 4:
        if (jt_is(argv[0], "!")) {
            t.pos = 1;
            value = jt_three(&t);
            value = t.err ? JT_F : (value ? JT_F : JT_T);
            checked_tail = 1;
            break;
        }
        value = jt_expr(&t); checked_tail = 1;
        break;
    default:
        value = jt_expr(&t); checked_tail = 1;
        break;
    }

    if (!t.err && checked_tail && t.pos != t.argc)
        jt_syntax(&t, "too many arguments", NULL);

    if (t.err) return 2;
    return value ? 0 : 1;
}

/* `test EXPR` — porta argv (ADR-0036 W2). JINV-066 */
int jash_builtin_test_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                           int argc, char** argv) {
    (void)w; (void)ctx;
    return jt_eval(s, "test", argc, argv);
}

/* `[ EXPR ]` — porta argv. O `]` final e OBRIGATORIO (bash: rc 2,
 * "missing `]'"); aceitar sem ele deixaria `[ -f x` passar calado. JINV-066 */
int jash_builtin_bracket_argv(jash_state_t* s, jash_writer_t w, void* ctx,
                              int argc, char** argv) {
    (void)w; (void)ctx;
    if (argc < 1 || strcmp(argv[argc - 1], "]") != 0) {
        jash_writef(jash_stderr_writer, 0, "jash: [: missing `]'\n");
        return 2;
    }
    return jt_eval(s, "[", argc - 1, argv);
}

/* `[ EXPR ]` — same as test, but expects closing ] */
int jash_builtin_bracket(jash_state_t* s, jash_writer_t w, void* ctx,
                         const char* args) {
    (void)s; (void)w; (void)ctx;

    if (!args || !*args) return 1;

    /* Skip leading ws and opening [ (if present — shouldn't be) */
    const char* p = args;
    while (*p == ' ' || *p == '\t') p++;

    /* Find closing ] */
    const char* end = p;
    while (*end && *end != ']') end++;
    if (!*end) return 1;  /* Missing ] */

    /* Extract content between [ and ] */
    size_t len = end - p;
    if (len == 0) return 1;

    char expr[256];
    if (len >= sizeof(expr)) len = sizeof(expr) - 1;
    memcpy(expr, p, len);
    expr[len] = 0;

    return eval_test_or(expr);   /* BUG-071: honor -a/-o connectives */
}
