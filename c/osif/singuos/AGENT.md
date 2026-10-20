# OSIF Port Runbook — singuos

> SinguOS kernel target (ring 3 jash on the SinguOS native/host path and the
> BEmu guest). Created 2026-09-25 during the de-duplication of the
> `SinguOS/surfaces/jash` copy. **This directory is the single canonical home
> for the SinguOS port; do not re-fork jash into another OS tree.**

## Stack

```text
jash core → c/osif/singuos/syscall.c (L0) → blibc bear_* → SinguOS kernel syscalls
```

L0 is identical in shape to `../beyondbsd/syscall.c` (freestanding, no libc,
Linux-NR → bear_* translation). This port's `syscall.c` includes that file
verbatim — one translation table, one place to fix.

## SinguOS-only builtins

SinguOS-integrated builtins live here (not in `c/builtins/`), because they
talk to SinguOS kernel features and are gated to this target:

- `builtin_cage.c`    — ADR-0089 GAIOLA envelopes (Conduit fork+arm+exec)
- `builtin_genesis.c` — Genesis v1 inspection/freeze
- `builtin_irq.c`     — IRQ surface probe
- `builtin_ring3_selftest.c` — ring-3 self-diagnostics

They are guarded by `JASH_TARGET_BEYOND`/`JASH_TARGET_SINGUOS` and must never
be compiled into host (linux/macos/netbsd) binaries.

## Governance (migration waves J1–J4)

1. **J1 (done):** this port directory exists; additive; canonical tree untouched
   otherwise. The old copy at `SinguOS/surfaces/jash/` is still authoritative
   until J4.
2. **J2:** register the four builtins above into `c/builtins/registry.c` behind
   `JASH_TARGET_SINGUOS` (requires editing the registry — do it deliberately,
   the canonical tree may carry owner WIP).
3. **J3:** `SinguOS/Makefile.jash` points at this tree (`JASH ?= ../Jash`),
   picking source set `c/` minus the other osif targets; gates: `bmake all`,
   DEV-LAB ssh prompt round-trip, `bmake -j1 test-v mx-probe`.
4. **J4:** delete `SinguOS/surfaces/jash/c` (keep `build-sovereign/` out of
   the tree), note the deletion in `Docs/ports/` provenance.

## Known pitfalls (inherited from the copy's notes)

- `c/compiler/*` is excluded from the sovereign find-glob; keep it that way.
- Old stand-alone `highlight_*.c` tools each define `main()`; NEVER glob them.
- `core/job_control.c` is a fossil shadowing `c/job_control.c`; keep excluded.
- `builtins/builtin_printf.c` is stale in the copy; the live one is
  `builtins/printf.c` (argv_fn shape).
