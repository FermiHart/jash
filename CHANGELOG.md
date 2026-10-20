# CHANGELOG — jash

## [2026-09-28] — MINIX typed input cursor and authenticated prompt

- What changed: `c/osif/minix/surface.c` emits MINIX's authenticated
  `jashII:surface$ ` prompt and accepts input whose sequence is at or after
  the input reader cursor, while still checking session, payload, reference,
  and ACK. `tests/test_minix_surface.c` now mocks the real Bear Surface API
  and covers a valid input at sequence 40 following a cursor at 1. Why:
  non-input cells share sequence space; rejecting any gap broke the first
  typed command in BEmu. Verification: the host Surface probe passed and
  BEmu/KVM processed a builtin `echo` with output/status-0 receipt. With
  MINIX-side native user `_exit` and relocated Jash ELF, `surface-check.py`
  passed all 10 typed commands, PID-1 restart/stale-session rejection, and
  corrupt-queue fail-closed. Incomplete: the binary still uses native MINIX
  libc alongside local blibc; a blibc-only build has not been qualified.

---

## [2026-09-27] — Canonical MINIX OSIF backend

- What changed: added MINIX syscall, OS-information, and poll adapters under
  `c/osif/minix/`, and extended the shared `c/osif/ipc.c` POSIX provider to use
  MINIX AF_UNIX, SCM_RIGHTS, and `getpeereid`; MINIX immutable snapshots remain
  explicitly `ENOTSUP`. Added Bear poll ABI declarations, MINIX header overlays,
  CMake source selection, and host behavior probes. Why: keep OSIF code in the
  canonical Jash repository, use native MINIX libc IPC, and never pass Linux
  syscall numbers to the kernel.
- Verification: `tests/test_minix_osif.sh`, `tests/test_minix_poll.sh`, and
  `tests/test_minix_ipc.sh` pass; the IPC harness forces the MINIX code path on
  the host and shims only `getpeereid` with host peer credentials. The target-
  header compile (`tests/test_minix_osif_target.sh` with `MINIX_SRC` and `TMPDIR`
  set) compiles four ELF64 x86_64 objects against
  MINIX headers; readelf checks all objects and nm/objdump inspect the syscall
  adapter for raw syscall use. Generated CMake sources include shared
  `c/osif/ipc.c` and `c/osif/minix/poll.c`; CTest's
  `minix_osif_cmake_target_flags` passes and confirms `__minix` is defined for
  the target sources. These use a host compiler, not a MINIX target link/runtime
  test. Incomplete: no canonical Jash ELF, `/bin/sh`
  image, or QEMU boot is verified; `x86_64-elf-*` tools are absent.

---

## [2026-06-17] — Claude Code + jash integração + ADR-0022 completo

### Claude Code roda dentro do jash

- `~/.zshenv` intercepta `/bin/zsh -c "..."` via `$ZSH_EXECUTION_STRING` quando
  `JASH_SIGIL` está no env (setado por `settings.json`), e faz `exec jash -c "$cmd"`.
  Cada comando do agente agora roda dentro do jash com chain log ativo.
- `CLAUDE_CODE_TMPDIR=~/.jash/tmp` adicionado ao `settings.json` para evitar esgotamento
  do ramdisk /tmp em sessões longas.
- Binário estável em `/Users/admin/.local/bin/jash` (sem sudo) — vem primeiro no PATH.

### ADR-0022 — 5 features LLM substrate

| Feature | Arquivo | Status |
|---------|---------|--------|
| F1 — Output Budget | `c/core/output_budget.c/h` | ✅ |
| F2 — Session Meta Log | `c/core/session_meta.c/h` | ✅ |
| F3 — Pre-Execution Guard | `c/core/guard.c/h` | ✅ |
| F4 — Semantic Error Digest | `c/core/err_digest.c/h` | ✅ |
| F5 — Intent Anchor | `c/builtins/builtin_intent.c` | ✅ |

### Bugs corrigidos

- **`intent` não registrado**: adicionado em `c/builtins/registry.c`.
- **`err_digest` ignorava `export JASH_ERR_DIGEST=1`**: `exec.c` usava `getenv()` em vez de
  `jash_env_get()`. Corrigido para verificar o store interno do jash primeiro.
- **`jash_writer_t` 2-arg**: todas as chamadas em `guard.c` e `output_budget.c`
  corrigidas para `jash_writes(w, ctx, msg)`.

### Bugs conhecidos / melhorias futuras

- **`undo` não persiste entre invocações `-c`**: timeline é em memória por sessão.
  No modo Claude Code (múltiplos `-c` independentes) `undo` após `rm --undo` não funciona.
  Fix: serializar timeline para `~/.jash/timeline.json`. Usar `trash restore` como workaround.
- **`session_meta.jsonl` não grava modo `-c`**: só sessões interativas.
  Chain log (`chain.log`) funciona em qualquer modo e cresce com cada invocação.

### Testes de maturidade (primeira run)

```
sigil:   7/7  100%
guard:   6/6  100%
budget:  3/3  100%
undo:    5/5  100%
intent:  4/4  100%
digest:  2/2  100%
compat:  1/1  100%  (59/61 bash compat tests pass)
session: 2/2  100%
─────────────────
TOTAL:  30/30 100%
```

---

## [2026-06-15] — ADR-0003 completo (S1–S9) + otimizações BSS

### ADR-0003 — Verifiable Execution Layer

- S1: Chain log BLAKE3 (`~/.jash/chain.log`)
- S2: HMAC proof (`~/.jash/session.jashproof`)
- S3: Ratchet anti-replay
- S4: Pledge tracking
- S5: Adversarial 4-attack battery
- S6–S9: OCI export, `sigil export`, `oras push`, seal mode

### Otimizações

- `-1.13MB BSS`, `-4.3MB malloc`
- Startup: 30ms → 19ms (1.08× bash)
- cmdsub: 208ms → 54ms (1.4× bash)

---

## [2026-06-14] — Recuperação do projeto

### Incidente

O opencode deletou o projeto com `rm -r` enquanto testava proteção contra rm em git repos.
Forense via `~/.jash/session_meta.jsonl` linhas 207–208.

### Recuperação

- Base: `/Volumes/dropbox-mac/Jash-release` + overlay Lima VM
- 664 arquivos restaurados, commit `9b0c5ef`
- `modules/slm-mesh/` recuperado de `/tmp/jash_backup/` (opencode backup)
- Guard (F3) agora impede que isso se repita

---

## [Antes de 2026-06-01] — Fundação

- Shell C custom 437KB, macOS + Linux
- Parser completo: lexer, eval, pipeline, redirects, heredoc, arithmetic, coproc
- Builtins POSIX completos + rm/ls/cp/mv/grep/ps/du/df/touch/kill (disruptivos)
- rm --undo + trash (recover 24h), timeline/undo infrastructure
- Job control, history, predict, abbr, alias, statusbar, temas, spinner
- Module system `.modjash` + Plugin system
- Test suite: 59/61 bash_compat pass
