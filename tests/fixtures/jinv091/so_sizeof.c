/* Fixture JINV-091 — SÓ a forma `sizeof(...)`, nenhuma macro em CAIXA ALTA e
 * nenhum literal numérico. É a fixture que importa: se alguém reescrever o
 * padrão com uma classe de caracteres e errar o `]` outra vez, os outros ramos
 * continuam casando e o lint continua parecendo vivo — só ESTE arquivo fica
 * verde, e o gate fica vermelho.
 * As três formas cobrem o que a classe quebrada não conseguia exprimir:
 * nome simples, membro por `->`, e elemento de vetor com `[` e `]`. */
#include <string.h>

struct fixture_slot { char k[128]; };

void fixture_so_sizeof(struct fixture_slot *save, int i, const char *src)
{
    char nome[64];
    size_t n = strlen(src);
    if (n >= sizeof(nome)) n = sizeof(nome) - 1;
    memcpy(nome, src, n);
    nome[n] = 0;

    size_t kl = strlen(src);
    if (kl >= sizeof(save[i].k)) kl = sizeof(save[i].k) - 1;
    memcpy(save[i].k, src, kl);
    save[i].k[kl] = 0;
}
