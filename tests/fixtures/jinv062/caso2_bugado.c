/* FIXTURE JINV-062 — reconstrucao do CASO 2 (bug real, ja consertado).
 *
 * Esta e a versao ASSIMETRICA de `rs_collect_paths` que travou o shell
 * instalado na maquina do dono, tal como descrita no comentario que ficou em
 * c/builtins/builtin_resume.c: `(` estava no conjunto que PULA e nao no que
 * PARA; `)` no que PARA e nao no que PULA.
 *
 * Num `)`: o laco de pular nao o consome, o de varrer para nele, comprimento
 * zero, ponteiro imovel, iteracao identica. Laco infinito.
 *
 * O gate exige que o lint marque este arquivo SUSPEITO. Se ele parar de
 * marcar, o lint deixou de valer — e um lint que nao pega o bug que motivou
 * sua existencia e so um arquivo a mais no repositorio.
 *
 * NAO COMPILA e nao deve ser compilado: e entrada de teste do lint.
 */

#define RS_PATH_LEN 256

static int rs_looks_like_path(const char* t, unsigned long len);

static int rs_collect_paths(const char* note, char out[][RS_PATH_LEN], int cap) {
    int n = 0;
    const char* p = note;
    while (*p && n < cap) {
        while (*p && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
                      *p == ',' || *p == '(' || *p == '`' || *p == '\'' ||
                      *p == '"')) p++;
        const char* st = p;
        while (*p && !(*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' ||
                       *p == ',' || *p == ')' || *p == '`' || *p == '\'' ||
                       *p == '"')) p++;
        unsigned long len = (unsigned long)(p - st);
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
