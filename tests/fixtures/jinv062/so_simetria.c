/* FIXTURE JINV-062 — SO a simetria, sem guarda nenhuma.
 *
 * As duas condicoes de seguranca sao INDEPENDENTES: a simetria dos conjuntos
 * basta sozinha, e a guarda basta sozinha. Este arquivo prova a primeira — se
 * o lint so aceitasse a guarda, estaria exigindo cerimonia em vez de raciocinar
 * sobre a propriedade, e passaria a acusar codigo correto.
 *
 * Esperado: SEGURO.
 * NAO COMPILA: e entrada de teste do lint.
 */

static int coleta(const char* nota, int cap) {
    int n = 0;
    const char* p = nota;
    while (*p && n < cap) {
        while (*p && (*p == ' ' || *p == '\t' || *p == ',')) p++;
        const char* st = p;
        while (*p && !(*p == ' ' || *p == '\t' || *p == ',')) p++;
        if (p > st) n++;
    }
    return n;
}
