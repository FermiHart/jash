/* FIXTURE JINV-062 — SO a guarda, com os conjuntos assimetricos de proposito.
 *
 * O par de baixo tem exatamente a assimetria do CASO 2 (`)` para a varredura e
 * nao e pulado), mas a guarda de progresso do CASO 1 torna o travamento
 * impossivel por construcao, seja qual for a logica dos conjuntos.
 *
 * Esperado: SEGURO — e por causa da GUARDA, nao por engano na leitura dos
 * conjuntos. O gate confere que o motivo declarado pelo lint cita a guarda.
 * NAO COMPILA: e entrada de teste do lint.
 */

static int coleta(const char* nota, int cap) {
    int n = 0;
    const char* p = nota;
    while (*p && n < cap) {
        const char* iter_start = p;
        while (*p && (*p == ' ' || *p == '\t' || *p == '(')) p++;
        const char* st = p;
        while (*p && !(*p == ' ' || *p == '\t' || *p == ')')) p++;
        if (p == iter_start) { if (!*p) break; p++; continue; }
        if (p > st) n++;
    }
    return n;
}
