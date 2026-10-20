/* SPDX-License-Identifier: LicenseRef-SPL-1.0 */
#pragma once
/* JINV-015 — este header era um DUPLICATA STALE com layout INCOMPATÍVEL.
 *
 * `modules/build_module.sh` compila com `-I$ROOT/c`, então um módulo que
 * escrevesse o nome natural — `#include <modjash.h>` — pegava ESTE arquivo,
 * enquanto o loader do host usa `c/module/modjash.h`. Os dois declaravam
 * `jmod_call_t` com ordem de campos TOTALMENTE diferente:
 *
 *     este (stale)              canônico (c/module/modjash.h)
 *     ─────────────────────     ─────────────────────────────
 *     0  const char* args       0  jash_state_t* state
 *     8  const char* verb       8  jmod_writer_t writer
 *    16  void*       state     16  void*         writer_ctx
 *    24  writer               24  const char*   verb
 *    32  writer_ctx           32  const char*   args
 *
 * E `jmod_writer_t` divergia na própria assinatura: aqui `void (*)(...)`,
 * lá `long (*)(...)`.
 *
 * Consequência: um módulo compilado contra este header e carregado pelo host
 * lia `state` de onde estava `args` — desreferenciando um `const char*` como
 * `jash_state_t*` — e chamava `writer` com convenção errada. Corrupção de
 * memória, não erro de compilação: os dois lados compilam limpos e só quebram
 * em runtime. Auditoria 2026-07-27, achado 1.1.
 *
 * Nada na árvore incluía este arquivo (verificado por grep em c/, modules/ e
 * libjef/), então a armadilha estava armada para o PRIMEIRO módulo de
 * terceiro que usasse o nome óbvio.
 *
 * Correção: encaminhar para o canônico. Os símbolos continuam todos
 * disponíveis por este caminho de include — agora com o layout CERTO. Nenhuma
 * declaração foi removida do projeto; ela passou a ter uma definição só.
 * Gate: tests/test_jinv015_abi_single.sh
 */
#include <module/modjash.h>
