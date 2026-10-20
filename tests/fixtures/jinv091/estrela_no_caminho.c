/* Fixture JINV-091 — o SEGUNDO ramo cego do mesmo padrão.
 * A âncora antiga era `^[^*/]*`, escrita para pular continuação de comentário
 * de bloco. Ela pulava junto qualquer linha com `*` ou `/` antes da
 * atribuição — um deref na guarda bastava para sumir com o sítio. A árvore
 * não tinha nenhum caso assim quando isto foi medido (99 sítios com e sem a
 * âncora), o que é justamente por que só uma fixture protege: não há código
 * real segurando essa propriedade. */
#include <string.h>

void fixture_estrela(char *dst, const char *src, const int *p)
{
    char buf[256];
    size_t n = strlen(src);
    if (*p && n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, src, n);
    buf[n] = 0;
    memcpy(dst, buf, n + 1);
}
