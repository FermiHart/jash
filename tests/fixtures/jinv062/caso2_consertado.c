/* FIXTURE JINV-062 — a versao CONSERTADA do CASO 2, como esta hoje em
 * c/builtins/builtin_resume.c: um unico macro para os dois conjuntos (simetria
 * por construcao) MAIS a guarda de avanco.
 *
 * O gate exige que o lint marque este arquivo SEGURO. Junto com o
 * caso2_bugado.c, isto e a prova de que o lint DISCRIMINA — que ele pode
 * falhar, e nao um script que devolve verde para qualquer entrada.
 *
 * NAO COMPILA e nao deve ser compilado: e entrada de teste do lint.
 */

#define RS_PATH_LEN 256

static int rs_looks_like_path(const char* t, unsigned long len);

static int rs_collect_paths(const char* note, char out[][RS_PATH_LEN], int cap) {
    int n = 0;
    const char* p = note;
    #define RS_SEP(c) ((c)==' '||(c)=='\t'||(c)=='\n'||(c)=='\r'||(c)==','|| \
                       (c)=='('||(c)==')'||(c)=='`'||(c)=='\''||(c)=='"')
    while (*p && n < cap) {
        const char* iter_start = p;
        while (*p && RS_SEP(*p)) p++;
        const char* st = p;
        while (*p && !RS_SEP(*p)) p++;
        unsigned long len = (unsigned long)(p - st);
        if (p == iter_start) { if (!*p) break; p++; continue; }
        while (len > 0 && (st[len-1] == '.' || st[len-1] == ':' ||
                           st[len-1] == ';')) len--;
        if (rs_looks_like_path(st, len)) {
            int dup = 0;
            for (int i = 0; i < n; i++)
                if (out[i][0] == st[0]) { dup = 1; break; }
            if (!dup) n++;
        }
    }
    return n;
}
