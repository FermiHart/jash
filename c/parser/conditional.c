// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Part of Josh OS — author F E R M I ∞ H A R T <contact@fermihart.com>
//
// 🎵 Hear F E R M I ∞ H A R T music. No obligation. Just a wish.
//
// Parser: [[ ... ]] conditional expressions
// Supports: -z, -n, numeric (-eq,-lt,-gt,-le,-ge,-ne), regex (=~), string ops (==, !=, <, >)

#include "conditional.h"
#include <core/state.h>
#include <core/env.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <errno.h>
#include <regex.h>
#include <sys/stat.h>
#include <unistd.h>

typedef struct {
    const char* expr;
    size_t pos;
    size_t len;
    jash_state_t* s;
} cond_parser;

/* Forward declarations */
static int cond_parse_or(cond_parser* p);

/* ===== Utilities ===== */

static void skip_ws(cond_parser* p) {
    while (p->pos < p->len && (p->expr[p->pos] == ' ' || p->expr[p->pos] == '\t')) {
        p->pos++;
    }
}

/* Check if a string is a valid integer (with optional leading sign) */
static int is_integer(const char* str) {
    if (!str || !*str) return 0;
    
    size_t i = 0;
    if (str[i] == '-' || str[i] == '+') i++;
    
    if (!str[i]) return 0;  /* Just a sign, not a number */
    
    while (str[i]) {
        if (!isdigit((unsigned char)str[i])) return 0;
        i++;
    }
    return 1;
}

/* POSIX extended regex match — populates BASH_REMATCH[0..N] on success */
#define JASH_REMATCH_MAX 32
static int regex_match(const char* pattern, const char* str, jash_state_t* s) {
    if (!pattern || !str) return 0;

    regex_t regex;
    int ret = regcomp(&regex, pattern, REG_EXTENDED);
    if (ret != 0) return 0;

    regmatch_t pmatch[JASH_REMATCH_MAX];
    ret = regexec(&regex, str, JASH_REMATCH_MAX, pmatch, 0);
    if (ret != 0) {
        regfree(&regex);
        if (s) jash_env_set(s, "BASH_REMATCH[0]", "");
        return 0;
    }

    if (s) {
        char key[32];
        char val[512];
        for (int g = 0; g < JASH_REMATCH_MAX; g++) {
            if (pmatch[g].rm_so < 0) break;
            int glen = (int)(pmatch[g].rm_eo - pmatch[g].rm_so);
            if (glen < 0) glen = 0;
            if (glen >= (int)sizeof(val)) glen = (int)sizeof(val) - 1;
            for (int ci = 0; ci < glen; ci++) val[ci] = str[pmatch[g].rm_so + ci];
            val[glen] = '\0';
            snprintf(key, sizeof(key), "BASH_REMATCH[%d]", g);
            jash_env_set(s, key, val);
        }
    }

    regfree(&regex);
    return 1;
}

/* Glob pattern matching (supports * and ?) */
static int glob_match(const char* pattern, const char* str) {
    if (!pattern || !str) return 0;

    while (*pattern && *str) {
        if (*pattern == '*') {
            while (*pattern == '*') pattern++;
            if (!*pattern) return 1;
            while (*str) {
                if (glob_match(pattern, str)) return 1;
                str++;
            }
            return glob_match(pattern, str);
        } else if (*pattern == '[') {
            /* Bracket expression: [abc], [a-z], [!set], [^set], [[:class:]] */
            int negate = 0;
            const char* p = pattern + 1;
            if (*p == '!' || *p == '^') { negate = 1; p++; }
            int matched = 0;
            unsigned char tc = (unsigned char)*str;
            while (*p && *p != ']') {
                if (p[0] == '[' && p[1] == ':') {
                    const char* cls = p + 2;
                    const char* end = cls;
                    while (*end && !(end[0] == ':' && end[1] == ']')) end++;
                    unsigned clen = (unsigned)(end - cls);
                    if (end[0] == ':' && end[1] == ']') {
                        if      (clen==5 && !strncmp(cls,"alpha",5)) { if(isalpha(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"digit",5)) { if(isdigit(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"alnum",5)) { if(isalnum(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"lower",5)) { if(islower(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"upper",5)) { if(isupper(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"space",5)) { if(isspace(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"blank",5)) { if(tc==' '||tc=='\t') matched=1; }
                        else if (clen==5 && !strncmp(cls,"print",5)) { if(isprint(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"graph",5)) { if(isgraph(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"cntrl",5)) { if(iscntrl(tc)) matched=1; }
                        else if (clen==5 && !strncmp(cls,"punct",5)) { if(ispunct(tc)) matched=1; }
                        else if (clen==6 && !strncmp(cls,"xdigit",6)){ if(isxdigit(tc)) matched=1; }
                        p = end + 2;
                    } else { if(tc==(unsigned char)*p) matched=1; p++; }
                } else if (p[1] == '-' && p[2] && p[2] != ']') {
                    if (tc >= (unsigned char)*p && tc <= (unsigned char)p[2]) matched = 1;
                    p += 3;
                } else {
                    if (tc == (unsigned char)*p) matched = 1;
                    p++;
                }
            }
            if (*p == ']') p++;
            if ((matched && !negate) || (!matched && negate)) { pattern = p; str++; }
            else return 0;
        } else if (*pattern == '?' || *pattern == *str) {
            pattern++;
            str++;
        } else {
            return 0;
        }
    }

    while (*pattern == '*') pattern++;
    return !*pattern && !*str;
}

/* Parse a string token (quoted or unquoted)
 * Returns 1 on success, 0 on failure.
 * Sets *was_quoted to 1 if the string was quoted, 0 otherwise. */
static int parse_string(cond_parser* p, char* out, size_t out_size, int* was_quoted) {
    skip_ws(p);
    
    if (p->pos >= p->len) return 0;
    
    if (was_quoted) *was_quoted = 0;
    
    /* Quoted string (double quotes) */
    if (p->expr[p->pos] == '"') {
        if (was_quoted) *was_quoted = 1;
        p->pos++;
        size_t i = 0;
        while (p->pos < p->len && p->expr[p->pos] != '"' && i + 1 < out_size) {
            out[i++] = p->expr[p->pos++];
        }
        if (p->pos < p->len && p->expr[p->pos] == '"') p->pos++;
        out[i] = '\0';
        return 1;
    }
    
    /* Single-quoted string */
    if (p->expr[p->pos] == '\'') {
        if (was_quoted) *was_quoted = 1;
        p->pos++;
        size_t i = 0;
        while (p->pos < p->len && p->expr[p->pos] != '\'' && i + 1 < out_size) {
            out[i++] = p->expr[p->pos++];
        }
        if (p->pos < p->len && p->expr[p->pos] == '\'') p->pos++;
        out[i] = '\0';
        return 1;
    }
    
    /* Unquoted word (stop at operators, whitespace)
     *
     * JINV-034: DENTRO de uma expressao de colchete os operadores sao
     * LITERAIS. Este laco parava no '!' incondicionalmente, entao
     * `[[ d == [!abc] ]]` nunca entregava o padrao inteiro ao matcher — a
     * palavra terminava no '[' e o '!abc]' virava outra coisa:
     *
     *   [[ d == [!abc] ]]   bash -> casou   jash -> nao
     *   [[ d == [^abc] ]]   bash -> casou   jash -> casou  (a forma '^' escapava)
     *   case d in [!abc])   bash -> casou   jash -> casou  (outro matcher, correto)
     *
     * O matcher de colchete (glob_match, acima) sempre soube tratar '!' e '^'
     * — o padrao e que chegava mutilado. A discrepancia entre as duas formas,
     * e entre `[[ ]]` e `case`, foi o que denunciou o bug numa tabela de
     * comparacao com o bash.
     *
     * Mesma classe do JINV-020 e do JINV-032: uma varredura que decide
     * fronteira sem olhar o contexto em que o caractere aparece. */
    size_t i = 0;
    int in_bracket = 0;
    while (p->pos < p->len && i + 1 < out_size) {
        char c = p->expr[p->pos];
        if (in_bracket) {
            if (c == ']') in_bracket = 0;
            out[i++] = c; p->pos++;
            continue;
        }
        if (isspace((unsigned char)c) ||
            c == '&' || c == '|' || c == '=' || c == '!' ||
            c == '<' || c == '>')
            break;
        if (c == '[') in_bracket = 1;
        out[i++] = c; p->pos++;
    }
    out[i] = '\0';
    return i > 0 ? 1 : 0;
}

/* ===== Unary Operators ===== */

/* Handle unary test operators: -z, -n, -d, -f, -e, -r, -w, -x, -s, -L, etc. */
static int eval_unary_test(const char* op, const char* operand) {
    if (!op || !operand) return 0;
    
    if (strcmp(op, "-z") == 0) {
        return strlen(operand) == 0 ? 1 : 0;
    }
    
    if (strcmp(op, "-n") == 0) {
        return strlen(operand) != 0 ? 1 : 0;
    }
    
    /* File test operators */
    struct stat st;
    int stat_rc = stat(operand, &st);
    int lstat_rc = -1;
    
    if (strcmp(op, "-e") == 0) {
        return stat_rc == 0 ? 1 : 0;
    }
    if (strcmp(op, "-d") == 0) {
        return (stat_rc == 0 && S_ISDIR(st.st_mode)) ? 1 : 0;
    }
    if (strcmp(op, "-f") == 0) {
        return (stat_rc == 0 && S_ISREG(st.st_mode)) ? 1 : 0;
    }
    if (strcmp(op, "-r") == 0) {
        return access(operand, R_OK) == 0 ? 1 : 0;
    }
    if (strcmp(op, "-w") == 0) {
        return access(operand, W_OK) == 0 ? 1 : 0;
    }
    if (strcmp(op, "-x") == 0) {
        return access(operand, X_OK) == 0 ? 1 : 0;
    }
    if (strcmp(op, "-s") == 0) {
        return (stat_rc == 0 && st.st_size > 0) ? 1 : 0;
    }
    if (strcmp(op, "-L") == 0 || strcmp(op, "-h") == 0) {
        lstat_rc = lstat(operand, &st);
        return (lstat_rc == 0 && S_ISLNK(st.st_mode)) ? 1 : 0;
    }
    if (strcmp(op, "-p") == 0) {
        return (stat_rc == 0 && S_ISFIFO(st.st_mode)) ? 1 : 0;
    }
    if (strcmp(op, "-S") == 0) {
        return (stat_rc == 0 && S_ISSOCK(st.st_mode)) ? 1 : 0;
    }
    if (strcmp(op, "-b") == 0) {
        return (stat_rc == 0 && S_ISBLK(st.st_mode)) ? 1 : 0;
    }
    if (strcmp(op, "-c") == 0) {
        return (stat_rc == 0 && S_ISCHR(st.st_mode)) ? 1 : 0;
    }
    if (strcmp(op, "-g") == 0) {
        return (stat_rc == 0 && (st.st_mode & S_ISGID)) ? 1 : 0;
    }
    if (strcmp(op, "-u") == 0) {
        return (stat_rc == 0 && (st.st_mode & S_ISUID)) ? 1 : 0;
    }
    if (strcmp(op, "-k") == 0) {
        return (stat_rc == 0 && (st.st_mode & S_ISVTX)) ? 1 : 0;
    }
    
    return 0;
}

/* ===== Binary Operators ===== */

/* Numeric comparison */
static int numeric_compare(const char* left, const char* op, const char* right) {
    if (!is_integer(left) || !is_integer(right)) return 0;
    
    long lval = strtol(left, NULL, 10);
    long rval = strtol(right, NULL, 10);
    
    if (strcmp(op, "-eq") == 0) return lval == rval ? 1 : 0;
    if (strcmp(op, "-ne") == 0) return lval != rval ? 1 : 0;
    if (strcmp(op, "-lt") == 0) return lval < rval ? 1 : 0;
    if (strcmp(op, "-le") == 0) return lval <= rval ? 1 : 0;
    if (strcmp(op, "-gt") == 0) return lval > rval ? 1 : 0;
    if (strcmp(op, "-ge") == 0) return lval >= rval ? 1 : 0;
    
    return 0;
}

/* String comparison */
static int string_compare(const char* left, const char* op, const char* right, int right_quoted, jash_state_t* s) {
    if (strcmp(op, "==") == 0) {
        /* Check if right side contains glob characters (only if not quoted) */
        if (!right_quoted && (strchr(right, '*') || strchr(right, '?') || strchr(right, '['))) {
            return glob_match(right, left);
        }
        return strcmp(left, right) == 0 ? 1 : 0;
    }

    if (strcmp(op, "!=") == 0) {
        /* Check if right side contains glob characters (only if not quoted) */
        if (!right_quoted && (strchr(right, '*') || strchr(right, '?') || strchr(right, '['))) {
            return !glob_match(right, left);
        }
        return strcmp(left, right) != 0 ? 1 : 0;
    }
    
    if (strcmp(op, "<") == 0) {
        return strcmp(left, right) < 0 ? 1 : 0;
    }
    
    if (strcmp(op, ">") == 0) {
        return strcmp(left, right) > 0 ? 1 : 0;
    }
    
    if (strcmp(op, "=~") == 0) {
        return regex_match(right, left, s);
    }
    
    return 0;
}

/* ===== Parser ===== */

static int cond_parse_comparison(cond_parser* p) {
    char left[256], op[8], right[256];
    int left_quoted = 0, right_quoted = 0;

    /* JINV-035 — negacao unaria `!`.
     *
     * Nao havia tratamento algum: a funcao comecava lendo um operando, e
     * `parse_string` para no '!' (ele e caractere de operador). Resultado:
     * operando vazio, e a comparacao inteira devolvia FALSO. Ou seja
     * `[[ ! ... ]]` nunca era verdadeiro, para nenhum conteudo:
     *
     *   [[ ! -f /nao/existe ]]   bash -> verdadeiro   jash -> falso
     *   [[ ! -d /nao ]]          bash -> verdadeiro   jash -> falso
     *   [[ ! "a" == "b" ]]       bash -> verdadeiro   jash -> falso
     *
     * O sintoma enganava: `[[ ! -f /etc/hosts ]]` devolvia falso, que e a
     * resposta CERTA por acaso (o arquivo existe). So comparando os dois
     * ramos contra o bash a falha aparecia — um `[[ ! ... ]]` que sempre
     * falha passa despercebido em qualquer script cujo caminho feliz seja o
     * ramo negativo.
     *
     * `!` liga mais forte que `&&` e `||`, entao o lugar certo e aqui, antes
     * do operando: nega o resultado da comparacao seguinte, recursivamente
     * (`[[ ! ! x ]]` e valido em bash). O guarda contra `!=` existe porque
     * aquele operador aparece DEPOIS de um operando — mas custa um `if` e
     * evita um caso ambiguo. */
    skip_ws(p);
    if (p->pos < p->len && p->expr[p->pos] == '!' &&
        !(p->pos + 1 < p->len && p->expr[p->pos + 1] == '=')) {
        p->pos++;
        skip_ws(p);
        return !cond_parse_comparison(p);
    }
    
    if (!parse_string(p, left, sizeof(left), &left_quoted)) {
        return 0;
    }
    
    skip_ws(p);
    
    /* Check for unary operators */
    if (left[0] == '-' && left[1] != '\0' && left[2] == '\0') {
        char test = left[1];
        
        /* String tests: -z, -n */
        if (test == 'z' || test == 'n') {
            if (!parse_string(p, right, sizeof(right), NULL)) return 0;
            
            char op_str[4];
            op_str[0] = '-';
            op_str[1] = test;
            op_str[2] = '\0';
            
            return eval_unary_test(op_str, right);
        }
        
        /* File tests: -d, -f, -e, -r, -w, -x, -s, -L, -h, -p, -S, -b, -c, -g, -u, -k */
        if (test == 'd' || test == 'f' || test == 'e' || test == 'r' ||
            test == 'w' || test == 'x' || test == 's' || test == 'L' ||
            test == 'h' || test == 'p' || test == 'S' || test == 'b' ||
            test == 'c' || test == 'g' || test == 'u' || test == 'k') {
            if (!parse_string(p, right, sizeof(right), NULL)) return 0;
            
            char op_str[4];
            op_str[0] = '-';
            op_str[1] = test;
            op_str[2] = '\0';
            
            return eval_unary_test(op_str, right);
        }
    }
    
    /* Check for binary operators */
    if (p->pos >= p->len) {
        /* Single operand: true if non-empty string */
        return strlen(left) > 0 ? 1 : 0;
    }
    
    /* Try to parse a binary operator */
    
    /* Try 3-character operators first (-eq, -ne, -lt, -le, -gt, -ge, =~) */
    if (p->pos + 2 < p->len) {
        if (p->expr[p->pos] == '-' && isalpha((unsigned char)p->expr[p->pos + 1]) &&
            isalpha((unsigned char)p->expr[p->pos + 2])) {
            op[0] = p->expr[p->pos];
            op[1] = p->expr[p->pos + 1];
            op[2] = p->expr[p->pos + 2];
            op[3] = '\0';
            p->pos += 3;
            skip_ws(p);
            
            if (!parse_string(p, right, sizeof(right), NULL)) return 0;
            
            return numeric_compare(left, op, right);
        }
        
        if (p->expr[p->pos] == '=' && p->expr[p->pos + 1] == '~') {
            op[0] = '=';
            op[1] = '~';
            op[2] = '\0';
            p->pos += 2;
            skip_ws(p);
            
            if (!parse_string(p, right, sizeof(right), &right_quoted)) return 0;
            
            return string_compare(left, op, right, right_quoted, p->s);
        }
    }
    
    /* Try 2-character operators (==, !=) */
    if (p->pos + 1 < p->len) {
        if (p->expr[p->pos] == '=' && p->expr[p->pos + 1] == '=') {
            p->pos += 2;
            skip_ws(p);
            if (!parse_string(p, right, sizeof(right), &right_quoted)) return 0;
            return string_compare(left, "==", right, right_quoted, p->s);
        }
        
        if (p->expr[p->pos] == '!' && p->expr[p->pos + 1] == '=') {
            p->pos += 2;
            skip_ws(p);
            if (!parse_string(p, right, sizeof(right), &right_quoted)) return 0;
            return string_compare(left, "!=", right, right_quoted, p->s);
        }
    }
    
    /* Try 1-character operators (<, >) */
    if (p->pos < p->len) {
        if (p->expr[p->pos] == '<') {
            p->pos++;
            skip_ws(p);
            if (!parse_string(p, right, sizeof(right), &right_quoted)) return 0;
            return string_compare(left, "<", right, right_quoted, p->s);
        }
        
        if (p->expr[p->pos] == '>') {
            p->pos++;
            skip_ws(p);
            if (!parse_string(p, right, sizeof(right), &right_quoted)) return 0;
            return string_compare(left, ">", right, right_quoted, p->s);
        }
    }
    
    /* Single operand: true if non-empty string */
    return strlen(left) > 0 ? 1 : 0;
}

static int cond_parse_and(cond_parser* p) {
    int left = cond_parse_comparison(p);
    skip_ws(p);
    
    while (p->pos + 1 < p->len && p->expr[p->pos] == '&' && p->expr[p->pos + 1] == '&') {
        p->pos += 2;
        skip_ws(p);
        int right = cond_parse_comparison(p);
        left = left && right;
        skip_ws(p);
    }
    
    return left;
}

static int cond_parse_or(cond_parser* p) {
    int left = cond_parse_and(p);
    skip_ws(p);
    
    while (p->pos + 1 < p->len && p->expr[p->pos] == '|' && p->expr[p->pos + 1] == '|') {
        p->pos += 2;
        skip_ws(p);
        int right = cond_parse_and(p);
        left = left || right;
        skip_ws(p);
    }
    
    return left;
}

/* ===== Public API ===== */

int jash_parse_conditional(const char* expr, jash_state_t* s) {
    if (!expr) return 0;

    cond_parser p = {
        .expr = expr,
        .pos = 0,
        .len = strlen(expr),
        .s = s,
    };

    return cond_parse_or(&p);
}
