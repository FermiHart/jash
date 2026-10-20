/* Fixture JINV-091 — a MESMA função depois do conserto: recusa alto em vez de
 * cortar. O lint TEM que absolver este arquivo.
 * Sem esta fixture o gate provaria só que o lint acusa — e um lint que acusa
 * tudo é tão inútil quanto um que não acusa nada. */
#include <string.h>

int fixture_consertado(char *dst, size_t cap, const char *src)
{
    size_t n = strlen(src);
    if (n + 1 > cap) {
        return -1;
    }
    memcpy(dst, src, n + 1);
    return 0;
}
