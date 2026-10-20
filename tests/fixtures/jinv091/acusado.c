/* Fixture JINV-091 — a forma canônica do truncamento silencioso.
 * O lint TEM que acusar este arquivo. É a forma exata que ficou invisível de
 * 2026-07-27 até 2026-07-28: a alternativa `sizeof(...)` do regex tinha o `]`
 * escapado dentro da classe, o que FECHA a classe, e o ramo nunca casou.
 * Nenhuma linha ofensora pode ter comentário: o lint descarta linha com
 * comentário de propósito, e a fixture ficaria verde pelo motivo errado. */
#include <string.h>

void fixture_acusado(char *dst, const char *src)
{
    char buf[256];
    size_t n = strlen(src);
    if (n >= sizeof(buf)) n = sizeof(buf) - 1;
    memcpy(buf, src, n);
    buf[n] = 0;
    memcpy(dst, buf, n + 1);
}
