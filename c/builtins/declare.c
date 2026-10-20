// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell
// Builtin: declare/typeset — variable declaration

#include "builtin.h"
#include "shell_output.h"
#include <core/env.h>
#include <function_defs.h>
#include <sys/syscalls.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <stdio.h>
#include <parser/arithmetic.h>

static void to_lower(char* dst, const char* src, unsigned cap) {
    unsigned i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = (src[i] >= 'A' && src[i] <= 'Z') ? src[i] + 32 : src[i];
        i++;
    }
    dst[i] = '\0';
}

static void to_upper(char* dst, const char* src, unsigned cap) {
    unsigned i = 0;
    while (src[i] && i < cap - 1) {
        dst[i] = (src[i] >= 'a' && src[i] <= 'z') ? src[i] - 32 : src[i];
        i++;
    }
    dst[i] = '\0';
}

int jash_b_declare(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "declare", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    if (!args || !*args) {
        /* Direct s->env[] reads below bypass jash_env_count()'s sync. */
        jash_env_sync_frame(s);
        for (uint32_t i = 0; i < s->env_count; i++) {
            jash_writef(w, ctx, "declare -- %s=%s\n", s->env[i].key, JASH_ENV_VAL(&s->env[i]));
        }
        return 0;
    }

    const char* p = args;
    while (*p == ' ' || *p == '\t') p++;

    /* `declare -f [NAME...]` — dump function definitions. With no NAME,
     * dump all. POSIX/bash form: `NAME () { BODY }`. Snapshot bootstraps
     * (zsh -> bash compat shims) emit this; without it the snapshot saw
     * empty output and assumed jash had no functions to capture. */
    if (p[0] == '-' && (p[1] == 'f' || p[1] == 'F') &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
        int name_only = (p[1] == 'F');
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        jash_function_table* tab = s->functions;
        if (!tab || tab->count == 0) return 0;
        if (!*p) {
            /* Dump all */
            for (int i = 0; i < tab->count; i++) {
                if (name_only)
                    jash_writef(w, ctx, "declare -f %s\n", tab->funcs[i].name);
                else
                    /* JINV-086: `.body` fica VAZIO quando o corpo migrou para
                     * o heap — ler o campo direto imprimiria uma função oca. */
                    jash_writef(w, ctx, "%s () \n{ \n%s\n}\n",
                                tab->funcs[i].name,
                                jash_function_body_of(&tab->funcs[i]));
            }
            return 0;
        }
        /* Dump named functions */
        int rc = 0;
        while (*p) {
            char name[128];
            unsigned ni = 0;
            while (*p && *p != ' ' && *p != '\t' && ni + 1 < sizeof(name))
                name[ni++] = *p++;
            name[ni] = '\0';
            while (*p == ' ' || *p == '\t') p++;
            const char* body = jash_function_lookup(tab, name);
            if (body) {
                if (name_only)
                    jash_writef(w, ctx, "declare -f %s\n", name);
                else
                    jash_writef(w, ctx, "%s () \n{ \n%s\n}\n", name, body);
            } else {
                rc = 1;
            }
        }
        return rc;
    }

    /* -p NAME: print variable (scalar or array) */
    if (p[0] == '-' && p[1] == 'p') {
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        while (*p) {
            char name[256];
            unsigned ni = 0;
            while (*p && *p != ' ' && *p != '\t' && ni < sizeof(name) - 1)
                name[ni++] = *p++;
            name[ni] = '\0';
            while (*p == ' ' || *p == '\t') p++;

            const char* val = jash_env_get(s, name);
            if (val) {
                jash_writef(w, ctx, "declare -- %s=\"%s\"\n", name, val);
            } else {
                /* Try array: scan for name[N] keys and print bash-style. */
                size_t nl = strlen(name);
                /* Collect matching array entries */
                typedef struct { long idx; const char* v; } arr_entry_t;
                arr_entry_t entries[256];
                int nentries = 0;
                uint32_t ec = s ? jash_env_count(s) : 0;
                for (uint32_t i = 0; i < ec && nentries < 256; i++) {
                    const char *k = 0, *v2 = 0;
                    if (!jash_env_at(s, i, &k, &v2)) continue;
                    size_t kl = strlen(k);
                    if (kl > nl + 2 && strncmp(k, name, nl) == 0 &&
                        k[nl] == '[' && k[kl - 1] == ']') {
                        const char* is2 = k + nl + 1;
                        long num = 0; int dig = 0;
                        while (is2 < k + kl - 1 && *is2 >= '0' && *is2 <= '9') {
                            num = num * 10 + (*is2++ - '0'); dig = 1;
                        }
                        if (dig && is2 == k + kl - 1) {
                            entries[nentries].idx = num;
                            entries[nentries].v = v2;
                            nentries++;
                        }
                    }
                }
                if (nentries > 0) {
                    /* Sort by numeric index (insertion sort — small N) */
                    for (int a = 1; a < nentries; a++) {
                        arr_entry_t tmp = entries[a]; int b = a - 1;
                        while (b >= 0 && entries[b].idx > tmp.idx) {
                            entries[b+1] = entries[b]; b--;
                        }
                        entries[b+1] = tmp;
                    }
                    jash_writef(w, ctx, "declare -a %s=(", name);
                    for (int a = 0; a < nentries; a++) {
                        if (a > 0) jash_writef(w, ctx, " ");
                        jash_writef(w, ctx, "[%ld]=\"%s\"", entries[a].idx, entries[a].v);
                    }
                    jash_writef(w, ctx, ")\n");
                } else {
                    jash_writef(w, ctx, "declare: %s: not found\n", name);
                }
            }
        }
        return 0;
    }

    /* -n: nameref — declare -n ref=target */
    if (p[0] == '-' && p[1] == 'n' &&
        (p[2] == '\0' || p[2] == ' ' || p[2] == '\t')) {
        p += 2;
        while (*p == ' ' || *p == '\t') p++;
        while (*p) {
            char rname[256], tname[256];
            unsigned ri = 0, ti = 0;
            while (*p && *p != '=' && *p != ' ' && *p != '\t' && ri < sizeof(rname) - 1)
                rname[ri++] = *p++;
            rname[ri] = '\0';
            tname[0] = '\0';
            if (*p == '=') {
                p++;
                while (*p && *p != ' ' && *p != '\t' && ti < sizeof(tname) - 1)
                    tname[ti++] = *p++;
                tname[ti] = '\0';
            }
            while (*p == ' ' || *p == '\t') p++;
            if (rname[0] && tname[0]) {
                char nrkey[320];
                snprintf(nrkey, sizeof(nrkey), "__nameref__%s", rname);
                jash_env_set(s, nrkey, tname);
            }
        }
        return 0;
    }

    /* Parse flags */
    int flag_integer = 0, flag_lower = 0, flag_upper = 0;

    while (p[0] == '-') {
        /* BUG-120b: `--` ends option processing — `declare -p` EMITS
         * `declare -- name="val"`, so `eval "$(declare -p v)"` fed the `--`
         * back and it errored "declare: --: invalid option". */
        if (p[1] == '-' && (p[2] == ' ' || p[2] == '\t' || p[2] == '\0')) {
            p += 2;
            while (*p == ' ' || *p == '\t') p++;
            break;
        }
        p++;
        while (*p && *p != ' ' && *p != '\t') {
            switch (*p) {
                case 'i': flag_integer = 1; break;
                case 'r': break;
                case 'x': break;
                case 'l': flag_lower = 1; break;
                case 'u': flag_upper = 1; break;
                case 'a': case 'A': case 'f': case 'F': case 'g': break;
                case 'p': break;
                default:
                    jash_writef(w, ctx, "declare: -%c: invalid option\n", *p);
                    return 1;
            }
            p++;
        }
        while (*p == ' ' || *p == '\t') p++;
    }

    int rc = 0;
    while (*p) {
        char name[256];
        char value[4096];
        unsigned ni = 0, vi = 0;

        while (*p && *p != '=' && *p != ' ' && *p != '\t' && ni < sizeof(name) - 1)
            name[ni++] = *p++;
        name[ni] = '\0';
        value[0] = '\0';

        int is_array_init = 0;
        if (*p == '=') {
            p++;
            if (*p == '(') {
                /* Array initializer: declare [-a] name=(e0 e1 ...) */
                is_array_init = 1;
                p++; /* skip '(' */
                int idx = 0;
                while (*p && *p != ')') {
                    while (*p == ' ' || *p == '\t') p++;
                    if (!*p || *p == ')') break;
                    char elem[1024]; unsigned ei = 0;
                    if (*p == '"') {
                        p++;
                        while (*p && *p != '"' && ei < sizeof(elem)-1) elem[ei++] = *p++;
                        if (*p == '"') p++;
                    } else if (*p == '\'') {
                        p++;
                        while (*p && *p != '\'' && ei < sizeof(elem)-1) elem[ei++] = *p++;
                        if (*p == '\'') p++;
                    } else {
                        while (*p && *p != ' ' && *p != '\t' && *p != ')' && ei < sizeof(elem)-1)
                            elem[ei++] = *p++;
                    }
                    elem[ei] = 0;
                    /* BUG-073: honor `[subscript]=value` elements — declare's
                     * initializer used to store the literal "[0]=x" string at
                     * a sequential index. Detect the form, evaluate the
                     * subscript (numeric or arithmetic), and index by it; a
                     * subscripted element also advances the positional counter
                     * to subscript+1 (bash semantics). Bare elements keep
                     * sequential indexing. */
                    char key[320];
                    int have_key = 0;
                    if (elem[0] == '[') {
                        char* rb = strchr(elem, ']');
                        if (rb && rb[1] == '=') {
                            char subx[64];
                            size_t sl = (size_t)(rb - (elem + 1));
                            if (sl < sizeof(subx)) {
                                memcpy(subx, elem + 1, sl); subx[sl] = '\0';
                                /* Same indexed-vs-associative disambiguation as
                                 * the eval.c subscript path: number → index;
                                 * operators → arith expr; bare id → index iff a
                                 * set var, else an associative STRING key. */
                                int all_digit = subx[0] != '\0', ident = subx[0] != '\0';
                                for (char* q = subx; *q; q++) {
                                    if (*q < '0' || *q > '9') all_digit = 0;
                                    if (!((*q>='a'&&*q<='z')||(*q>='A'&&*q<='Z')||
                                          (*q>='0'&&*q<='9')||*q=='_')) ident = 0;
                                }
                                int do_eval = all_digit ? 0
                                            : !ident      ? 1
                                            : (jash_env_get(s, subx) != NULL);
                                memmove(elem, rb + 2, strlen(rb + 2) + 1);
                                if (do_eval) {
                                    long si = jash_eval_arithmetic(subx, s);
                                    if (si < 0) si = 0;
                                    snprintf(key, sizeof(key), "%s[%ld]", name, si);
                                    idx = (int)si + 1;
                                } else {
                                    snprintf(key, sizeof(key), "%s[%s]", name, subx);
                                }
                                have_key = 1;
                            }
                        }
                    }
                    if (!have_key)   /* bare positional element */
                        snprintf(key, sizeof(key), "%s[%d]", name, idx++);
                    jash_env_set(s, key, elem);
                }
                if (*p == ')') p++;
            } else if (*p == '"') {
                p++;
                while (*p && *p != '"' && vi < sizeof(value) - 1)
                    value[vi++] = *p++;
                if (*p == '"') p++;
            } else if (*p == '\'') {
                p++;
                while (*p && *p != '\'' && vi < sizeof(value) - 1)
                    value[vi++] = *p++;
                if (*p == '\'') p++;
            } else {
                while (*p && *p != ' ' && *p != '\t' && vi < sizeof(value) - 1)
                    value[vi++] = *p++;
            }
            value[vi] = '\0';
        }

        if (!is_array_init) {
            if (flag_lower) { char tmp[4096]; to_lower(tmp, value, sizeof(tmp)); strlcpy(value, tmp, sizeof(value)); }
            if (flag_upper) { char tmp[4096]; to_upper(tmp, value, sizeof(tmp)); strlcpy(value, tmp, sizeof(value)); }

            if (flag_integer) {
                char result[64];
                if (value[0]) {
                    snprintf(result, sizeof(result), "%ld", jash_eval_arithmetic(value, s));
                    jash_env_set(s, name, result);
                }
                char int_key[160];
                snprintf(int_key, sizeof(int_key), "__jash_int__%s", name);
                jash_env_set(s, int_key, "1");
            } else {
                jash_env_set(s, name, value);
            }
        }

        while (*p == ' ' || *p == '\t') p++;
    }

    return rc;
}

int jash_b_typeset(jash_state_t* s, jash_writer_t w, void* ctx, const char* args) {
    int structured = jash_shell_state_try(s, w, ctx, "typeset", args);
    if (structured != JASH_SHELL_TEXT) return structured;
    return jash_b_declare(s, w, ctx, args);
}
