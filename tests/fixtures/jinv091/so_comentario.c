/* Fixture JINV-091 — o padrão aparece SÓ dentro de comentário. O lint tem que
 * absolver: documentar a forma proibida é o que este repositório faz o tempo
 * todo (ACHADO-buffers-fixos.md, INVARIANTES, cabeçalho do próprio lint), e um
 * lint que acusa a própria documentação vira ruído e depois vira ignorado.
 *
 * A forma descrita é esta, e ela não pode contar:
 *     if (n >= sizeof(buf)) n = sizeof(buf) - 1;
 * inclusive quando a linha de continuação começa com estrela, como esta:
 * n = sizeof(buf) - 1;
 */
#include <string.h>

int fixture_so_comentario(const char *src)
{
    return src ? (int)strlen(src) : 0;   // n = sizeof(x) - 1; nem aqui
}
