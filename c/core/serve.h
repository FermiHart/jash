// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/serve — structured exec protocol daemon (ADR-0038 W6).
//
// `jash --serve`: a persistent jash speaking JSONL over stdio. One request
// object per stdin line, one response object per stdout line:
//
//   → {"id":1,"argv":["grep","-n","a b","file.c"]}
//   → {"id":2,"line":"cmake --build build | tail -3","cwd":"/repo"}
//   → {"id":3,"argv":["cat"],"stdin":"piped bytes"}
//   ← {"id":1,"rc":0,"duration_ms":12,"stdout":"...","stderr":"",
//      "files_changed":{...},"receipt_ref":"...dsse"}
//
// Why it exists (the three costs it kills):
//   1. re-quoting: argv mode takes a pre-tokenized argv — jash generates the
//      quoting itself (single-quote per token), so the BUG-059 class cannot
//      exist by construction; `line` mode keeps full shell semantics.
//   2. startup: one process for the whole conversation instead of ~40ms of
//      fork+exec per tool call.
//   3. window flooding: stdout/stderr beyond 32KB become recall-able
//      overflow refs (stdout_ref) with size + head preview, not inline bytes.
//
// One logical, ATTESTED session: builtins mutate the daemon's own state
// (cd/export persist across requests), and every request flows through
// jash_eval_line. `env` overlays are per-request (saved/restored); `cwd` is
// sticky. In the agent-only profile, timeout_ms is enforced for exec commands.
// Cancelable execs and native mutators with leading --json listen on the
// inherited JASH_AGENT_CANCEL_FD; native cancellation is cooperative (no timeout).
// Every completed idempotency record is
// published only after its pinned-verifiable DSSE receipt. EOF ends the daemon.
// Protocol v2 is request-opt-in (`request_version` or `protocol_version`: 2):
// start/chunk/result JSONL events carry byte-exact base64 stream chunks, and
// `stdin_b64` is strict RFC 4648 input. The terminal result commits each
// stream's BLAKE3, byte count, and immutable content-addressed ref under the
// private overflow directory. Replays return the signed terminal result whose
// refs recover the complete prior output without repeating chunks or effects.
// V1 remains the default when no version is requested.
// Agent-only protocol v2 also accepts shell-free execution IR v1 through
// `exec` and `job.start`, then manages up to 32 runtime-scoped jobs with
// get/wait/cancel/resize/write/read operations. Starts accept bounded cwd,
// environment overlays, binary stdin, and opt-in writable non-PTY stdin pipes
// without mutating daemon state. `stdin_mode:"pipe"` accepts bounded writes
// authorized by `job:stdin:write`; protocol half-close is deferred. Job
// ids are opaque, output is read as paged base64 from separate private spools,
// PTY input is bounded base64 with finite deadlines and accepted-byte counts,
// and EOF cancels/reaps every
// remaining process and archives a runtime-generated terminal observation before
// deleting spools. Failed archival preserves spools and makes session exit 125.
// job.result reads this retained observation across sessions without reattachment.
// Creation shares the durable
// idempotency namespace with synchronous requests, so one id cannot execute
// once through each route. A successful launch publishes a nonterminal DSSE
// start receipt; DONE+quiescent promotes both streams to identity-pinned CAS
// objects, and each terminal response publishes a linked receipt over its
// byte-exact body before receipt metadata is appended. Terminal reads use CAS.
// F E R M I ∞ H A R T

#ifndef JASH_CORE_SERVE_H
#define JASH_CORE_SERVE_H

#include <core/state.h>
#include <core/limits.h>
#include <stddef.h>

/* Run the serve loop until EOF on stdin. Returns the exit code. */
int jash_serve_loop(jash_state_t* s);
/* Borrowed private listener channel; -1 preserves ordinary stdio behavior. */
int jash_serve_loop_controlled(jash_state_t* s, int shutdown_fd);
int jash_serve_loop_data(jash_state_t* s, int shutdown_fd, int data_fd);

/* Teto do comando renderizado. UMA definição só — serve.c deriva SRV_LINE_MAX
 * daqui e main.c dimensiona o buffer de --exec-json por ela. */
#define JASH_EXEC_JSON_LINE_MAX JASH_LINE_CAP_STACK  /* JINV-030: deriva de core/limits.h — nao um numero solto */

/* Machine-negotiated agent runtime contract. Bump the protocol when framing
 * changes and the schema when request/response meaning changes. */
#define JASH_AGENT_PROTOCOL_VERSION 2
#define JASH_AGENT_SCHEMA_VERSION   18

/* JINV-016/017 — a porta ÚNICA do pedido estruturado. `--exec-json` e
 * `--serve` falam o MESMO protocolo; agora com UMA implementação.
 *
 * Interpreta um objeto de pedido e renderiza o comando em `out`:
 *   {"argv":[...]}      argv pré-tokenizado; jash gera a citação de TODO
 *                       token, inclusive argv[0] (JINV-016 — antes ele saía
 *                       bare, e "echo x; rm -rf y" virava DOIS comandos);
 *   {"line":"..."}      texto de shell literal, semântica completa;
 *   cwd / env / stdin   contexto, aplicado a ESTE processo quando
 *                       `apply_context` != 0 (chdir / setenv / dup2 no fd 0).
 *
 * FAIL-CLOSED: devolve NULL em sucesso ou uma string de erro legível — e
 * quando devolve erro, NADA foi executado. Um `cwd` inacessível RECUSA o
 * pedido em vez de rodar o comando no diretório errado.
 *
 * `apply_context == 0` apenas renderiza, sem efeito colateral. */
const char* jash_exec_json(const char* json, char* out, size_t cap,
                           int apply_context);

#endif /* JASH_CORE_SERVE_H */
