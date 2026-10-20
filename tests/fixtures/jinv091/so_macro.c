/* Fixture JINV-091 — SÓ a forma com macro em CAIXA ALTA e a forma com literal
 * numérico: os dois ramos que SEMPRE funcionaram. Estão aqui para que um
 * conserto do ramo do `sizeof` não possa quebrá-los sem o gate perceber. */
#include <string.h>

#define FIXTURE_MAX 128

void fixture_so_macro(char *dst, const char *src, size_t n, size_t m)
{
    if (n >= FIXTURE_MAX) n = FIXTURE_MAX - 1;
    memcpy(dst, src, n);
    dst[n] = 0;

    if (m >= 4096) m = 4096 - 1;
    memcpy(dst, src, m);
    dst[m] = 0;
}
