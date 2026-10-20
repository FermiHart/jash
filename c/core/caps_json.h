// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/caps_json — descritor de capacidades legível por máquina.
//
// JINV-018. `jash --capabilities-json` responde UMA pergunta que hoje um
// agente só consegue responder tentando e falhando: "o que este jash, nesta
// máquina, de fato faz?"
//
// A regra que define este arquivo: ele reporta o que é APLICADO, nunca o que
// é pretendido. A auditoria de 2026-07-27 diagnosticou que os diferenciais do
// jash estavam implementados na camada de UX e não na de imposição — `sigil`
// imprimia "attested" sem verificar o MAC, `timeline` dizia "← undo reverts
// this" engolindo falha de backup. Um descritor de capacidades que herdasse
// esse hábito seria a pior versão do problema: mentira em formato de máquina,
// que nenhum humano relê.
//
// Por isso cada campo aqui sai de uma MEDIÇÃO:
//   - `osif`      vem de bear_osinfo_caps(), a máscara que o backend desta
//                 plataforma realmente implementa (ADR-0017);
//   - `builtins`  vem de jash_builtin_count(), a tabela real;
//   - `exec_json` reflete os campos que a porta única honra (core/serve.c) —
//                 e `timeout_ms` sai como "reserved", que é a verdade: o
//                 protocolo o aceita e NÃO o impõe (serve.h);
//   - `modules`   reporta se a verificação BLAKE3 está exigida ou opcional
//                 (JINV-013), não se ela "existe".
//
// F E R M I ∞ H A R T

#ifndef JASH_CORE_CAPS_JSON_H
#define JASH_CORE_CAPS_JSON_H

/* Emite o descritor em stdout (uma linha JSON) e devolve o código de saída.
 * Não aloca estado de shell: roda antes de qualquer init, como --whoami. */
int jash_capabilities_json(void);

/* JINV-047: inventario de builtins (name/help/argv_native/effect).
 * Flag separada para nao inchar o descritor, que e uma linha so. */
int jash_builtins_json(void);

#endif /* JASH_CORE_CAPS_JSON_H */
