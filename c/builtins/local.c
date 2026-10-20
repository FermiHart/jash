// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — Josh shell (ring 3)
// Builtin: local — create a local variable (scoped to function)

#include "builtin.h"
#include <core/env.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <parser/arithmetic.h>

extern int  jash_readonly_is_set(const char *name);
extern void jash_readonly_mark(const char *name);

int jash_b_local(jash_state_t *s, jash_writer_t w, void *ctx,
                 const char *args) {
    (void)w; (void)ctx;
    if (!args || !*args) return 0;

    /* Get or create the list of local variables for this function call */
    const char *local_list = jash_env_get(s, "__LOCAL_VARS__");
    char new_list[1024];
    int nli = 0;
    if (local_list) {
        while (*local_list && nli < 1020) new_list[nli++] = *local_list++;
    }
    new_list[nli] = 0;

    /* Parse flags */
    int flag_readonly = 0, flag_integer = 0;
    const char *p = args;
    while (*p == '-') {
        p++;
        while (*p && *p != ' ' && *p != '\t') {
            switch (*p) {
                case 'r': flag_readonly = 1; break;
                case 'i': flag_integer  = 1; break;
                case 'x': break;  /* export flag — ignore (local can't be exported) */
                case 'a': case 'A': break;
            }
            p++;
        }
        while (*p == ' ' || *p == '\t') p++;
        /* If next token doesn't start with '-', stop flag parsing */
        if (*p != '-') break;
    }

    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;

        const char *start = p;
        const char *eq = NULL;
        int in_paren = 0;
        while (*p) {
            if (*p == '(' && eq) { in_paren++; p++; continue; }
            if (*p == ')' && in_paren > 0) { in_paren--; p++; continue; }
            if ((*p == ' ' || *p == '\t') && in_paren == 0) break;
            if (*p == '=' && !eq) eq = p;
            p++;
        }

        /* Extract variable name */
        int namelen = eq ? (int)(eq - start) : (int)(p - start);
        if (namelen <= 0 || namelen >= 63) { p++; continue; }
        char varname[64];
        for (int i = 0; i < namelen; i++) varname[i] = start[i];
        varname[namelen] = 0;

        const char *val = eq ? eq + 1 : "";

        /* Remove quotes from the value, similar to how assignment parsing works.
         * This handles "value", 'value', and unquoted values correctly.
         *
         * JINV-086: `char unquoted_val[4096]` cortava em silêncio todo valor
         * de `local` acima de 4095 bytes — `local V=<8000 a's>; echo ${#V}`
         * dizia 4095 onde o bash diz 8000, com rc 0 e stderr vazio. O
         * resultado da remoção de aspas nunca é MAIOR que a entrada, então
         * `strlen(val) + 1` é um tamanho exato que jamais estoura: os 4096
         * bytes de pilha viram limiar de desempenho (zero malloc no caso
         * comum) e o heap entra acima disso. Sem memória: recusa ALTA — a
         * variável NÃO é criada.
         *
         * Gate: tests/test_jinv086_silent_caps.sh */
        char  unquoted_inline[4096];
        char* unquoted_heap = NULL;
        char* unquoted_val = unquoted_inline;
        size_t unquoted_cap = sizeof(unquoted_inline);
        if (eq && strlen(val) + 1 > unquoted_cap) {
            unquoted_heap = (char*)malloc(strlen(val) + 1);
            if (!unquoted_heap) {
                jash_writes(jash_stderr_writer, 0,
                            "jash: local: sem memoria para o valor — a variavel NAO foi criada\n");
                return 2;
            }
            unquoted_val = unquoted_heap;
            unquoted_cap = strlen(val) + 1;
        }
        if (eq) {
            unsigned vi = 0;
            int sq = 0, dq = 0;
            const char* vp = val;
            while (*vp && vi < unquoted_cap - 1) {
                if (!sq && *vp == '\\' && vp[1]) {
                    unquoted_val[vi++] = vp[1];
                    vp += 2;
                    continue;
                }
                if (!sq && *vp == '"') { dq = !dq; vp++; continue; }
                if (!dq && *vp == '\'') { sq = !sq; vp++; continue; }
                if ((sq || dq) && *vp == '\\') { vp++; continue; }  /* Skip \ inside quotes */
                unquoted_val[vi++] = *vp++;
            }
            unquoted_val[vi] = '\0';
            val = unquoted_val;
        }

        /* Array initializer: local [-a] name=(e0 e1 ...) */
        if (val[0] == '(') {
            /* BUG-102: snapshot the scalar + sweep every existing NAME[...]
             * element into __local_NAME[i] (tracked AFTER the bare name in
             * the list) and remove it — the per-element __UNSET__ save this
             * replaced double-tracked keys and never cleared globals the
             * initializer didn't overwrite. */
            {
                const char *curv = jash_env_get(s, varname);
                char lk[128];
                snprintf(lk, sizeof(lk), "__local_%s", varname);
                jash_env_set(s, lk, curv ? curv : "__UNSET__");
                if (nli > 0 && nli < 1020) new_list[nli++] = ' ';
                for (int vi3 = 0; varname[vi3] && nli < 1020; vi3++) new_list[nli++] = varname[vi3];
                new_list[nli] = 0;
                size_t vl = strlen(varname);
                int removed;
                do {
                    removed = 0;
                    uint32_t n = jash_env_count(s);
                    for (uint32_t i = 0; i < n; i++) {
                        const char *k = 0, *v2 = 0;
                        if (!jash_env_at(s, i, &k, &v2)) continue;
                        if (strncmp(k, varname, vl) == 0 && k[vl] == '[' &&
                            strncmp(k, "__local_", 8) != 0) {
                            char kb[128];
                            snprintf(kb, sizeof(kb), "%s", k);
                            char sk[160];
                            snprintf(sk, sizeof(sk), "__local_%s", kb);
                            jash_env_set(s, sk, v2 ? v2 : "");
                            if (nli > 0 && nli < 1018) new_list[nli++] = ' ';
                            for (int t = 0; kb[t] && nli < 1018; t++) new_list[nli++] = kb[t];
                            new_list[nli] = 0;
                            jash_env_unset(s, kb);
                            removed = 1;
                            break;
                        }
                    }
                } while (removed);
            }
            const char *ep = val + 1;
            int idx = 0;
            while (*ep && *ep != ')') {
                while (*ep == ' ' || *ep == '\t') ep++;
                if (!*ep || *ep == ')') break;
                const char *ee = ep;
                if (*ep == '"') {
                    ee++; ep++;
                    while (*ee && *ee != '"') ee++;
                } else if (*ep == '\'') {
                    ee++; ep++;
                    while (*ee && *ee != '\'') ee++;
                } else {
                    while (*ee && *ee != ' ' && *ee != '\t' && *ee != ')') ee++;
                }
                int el = (int)(ee - ep); if (el < 0) el = 0;
                char elem[256]; if (el >= 256) el = 255;
                memcpy(elem, ep, (unsigned)el); elem[el] = 0;

                char key[128];
                snprintf(key, sizeof(key), "%s[%d]", varname, idx++);
                jash_env_set(s, key, elem);
                if (*ee == '"' || *ee == '\'') ee++;
                ep = ee;
            }
            while (*p == ' ' || *p == '\t') p++;
            if (unquoted_heap) free(unquoted_heap);   /* JINV-086 */
            continue;
        }

        /* Save current value for restoration on function return */
        const char *cur = jash_env_get(s, varname);
        char local_key[128];
        snprintf(local_key, sizeof(local_key), "__local_%s", varname);
        jash_env_set(s, local_key, cur ? cur : "__UNSET__");

        /* Add to local variables list */
        if (nli > 0 && nli < 1020) new_list[nli++] = ' ';
        for (int vi = 0; varname[vi] && nli < 1020; vi++) new_list[nli++] = varname[vi];
        new_list[nli] = 0;

        /* BUG-102: `local NAME` scopes the ARRAY too. Elements live in
         * separate env keys NAME[0], NAME[1], … — snapshot each as
         * __local_NAME[i], track it in the list (AFTER the bare name, so
         * the restore order re-adds them), and REMOVE it so the local
         * array starts empty (bash). Restart the scan after each unset
         * (the env table compacts by swapping). */
        {
            size_t vl = strlen(varname);
            int removed;
            do {
                removed = 0;
                uint32_t n = jash_env_count(s);
                for (uint32_t i = 0; i < n; i++) {
                    const char *k = 0, *v2 = 0;
                    if (!jash_env_at(s, i, &k, &v2)) continue;
                    if (strncmp(k, varname, vl) == 0 && k[vl] == '[' &&
                        strncmp(k, "__local_", 8) != 0) {
                        char kb[128];
                        snprintf(kb, sizeof(kb), "%s", k);
                        char sk[160];
                        snprintf(sk, sizeof(sk), "__local_%s", kb);
                        jash_env_set(s, sk, v2 ? v2 : "");
                        if (nli > 0 && nli < 1018) new_list[nli++] = ' ';
                        for (int t = 0; kb[t] && nli < 1018; t++) new_list[nli++] = kb[t];
                        new_list[nli] = 0;
                        jash_env_unset(s, kb);
                        removed = 1;
                        break;
                    }
                }
            } while (removed);
        }

        /* Compute the new value */
        if (flag_integer && eq) {
            static char int_buf[64];
            snprintf(int_buf, sizeof(int_buf), "%ld", jash_eval_arithmetic(val, s));
            val = int_buf;
        }

        /* Set the local variable */
        jash_env_set(s, varname, val);

        /* Apply attributes */
        if (flag_readonly) jash_readonly_mark(varname);
        if (flag_integer) {
            char int_key[128];
            snprintf(int_key, sizeof(int_key), "__jash_int__%s", varname);
            jash_env_set(s, int_key, "1");
        }
        if (unquoted_heap) free(unquoted_heap);   /* JINV-086 */
    }

    jash_env_set(s, "__LOCAL_VARS__", new_list);
    return 0;
}
