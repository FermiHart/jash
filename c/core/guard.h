// SPDX-License-Identifier: LicenseRef-SPL-1.0
// jash — core/guard — pre-execution guard for destructive git/rm operations
//
// Activated when JASH_GUARD=1 or JASH_SIGIL >= 1.
// JASH_GUARD_STRICT=1 blocks (return 1); otherwise warns and returns 0.

#ifndef JASH_CORE_GUARD_H
#define JASH_CORE_GUARD_H

#include <theme/io.h>

// Returns 1 if the guard is active (JASH_GUARD=1 or JASH_SIGIL>=1).
int jash_guard_active(void);

// ADR-0023 E2 / BUG-027: destructive-op protection policy. Returns 1 when rm
// (and peers) should be RECOVERABLE (trash + journaled inverse so undo/rewind
// cover them), 0 for bash-parity permanent delete. Precedence:
//   1. explicit user toggle `trash on|off`  (~/.jash/trash_mode)
//   2. env JASH_TRASH (on/1/true | off/0/false)
//   3. default: protected when recording is on (JASH_SIGIL>=1)
int jash_trash_protected(void);

// Persist the explicit toggle: on=1 (protect), on=0 (off), on=-1 (clear → revert
// to default). Writes/removes ~/.jash/trash_mode. Returns 0 on success.
int jash_trash_set(int on);

// 0=explicit toggle, 1=env, 2=default(sigil) — what decided jash_trash_protected().
int jash_trash_source(void);

// Called from the rm builtin before deleting path.
// recursive=1 means rm -r. Returns 1 to block, 0 to proceed.
int jash_guard_rm(const char* path, int recursive, jash_writer_t w, void* ctx);

// Called from exec.c when argv[0] basename is "git".
// args is the full command string after "git ".
// Returns 1 to block, 0 to proceed.
int jash_guard_git(const char* args, jash_writer_t w, void* ctx);

// ADR-0042 F9: mesh file intent-lock enforcement. Called before a mutating op
// (folded into jash_guard_rm; called from the mv builtin per source). If a live
// claim by a DIFFERENT agent (JASH_AGENT_ID) holds `path`, warns on stderr
// (advisory) and records the conflict for the envelope; blocks (returns 1) only
// when JASH_CLAIM_STRICT=1. JASH_CLAIM=0 disables. Returns 1 to block, 0 to
// proceed. Inert (returns 0) when the guard is off or no claim exists.
int jash_guard_claim(const char* path, jash_writer_t w, void* ctx);

// ── Agent anti-bypass guard (ADR-0026 Tier 3) ───────────────────────────────
// Detects when an agent tries to escape jash primitives by calling bash/sh/python
// directly. Activated by JASH_AGENT=1 or JASH_GUARD_BYPASS=1. In strict mode
// (JASH_GUARD_BYPASS_STRICT=1) the bypass is blocked; otherwise it is logged and
// warned. Returns 1 when the command must be blocked, 0 to proceed.
int jash_guard_bypass_active(void);
int jash_guard_bypass_strict(void);
int jash_guard_bypass_detect(const char* cmd, const char* args,
                             jash_writer_t w, void* ctx);

// Persist guard mode across separate `jash -c` invocations:
//   on=0,strict=0 → off (remove file)
//   on=1,strict=0 → watch (warn only)
//   on=1,strict=1 → strict (block bypasses)
int jash_guard_bypass_set(int on, int strict);

// Record one command event in ~/.jash/agent_metrics.tsv. `jash_native` is 1 when
// the command used a jash differentiator — the canonical list lives in
// core/differentiators.h (JASH_DIFF_NATIVE), NOT here. `bypass` is 1 when the
// command escaped to bash/sh/python/zsh. `blocked` is 1 when the guard refused
// to run it.
void jash_guard_metrics_record(int jash_native, int bypass, int blocked);

// ADR-0040 W2: count one shell-assisted upgrade (the pre-exec engine rewrote
// the agent's command into a jash primitive). Such commands are ALSO recorded
// via jash_guard_metrics_record(native=1,...) — `upgraded` splits the native
// count into agent-initiated (native - upgraded) vs shell-assisted.
void jash_guard_metrics_upgraded(void);

// Cumulative counters from ~/.jash/agent_metrics.tsv (flushes pending first).
// Rows written before ADR-0040 W2 have no `upgraded` column and count as 0.
void jash_guard_metrics_snapshot(unsigned* total, unsigned* native,
                                 unsigned* bypass, unsigned* blocks,
                                 unsigned* upgraded);

// ADR-0040 W4: THIS-process counters only (never reset by the batch flush).
// Feed the per-session adoption telemetry row — the cumulative snapshot above
// cannot answer "what happened in THIS session?".
void jash_guard_metrics_session(unsigned* total, unsigned* native,
                                unsigned* bypass, unsigned* blocks,
                                unsigned* upgraded);

// Print adoption/bypass/block dashboard line to (w,ctx). `total` receives the
// cumulative command count; the caller may use it for envelope reporting.
void jash_guard_agent_status(jash_writer_t w, void* ctx, unsigned* total,
                             unsigned* native, unsigned* bypass, unsigned* blocks);

#endif
