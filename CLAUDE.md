# Claude Code + jash Integration Guide (ADR-0003 A1)

> **Agents: read [`CODEMAP.md`](CODEMAP.md) FIRST** — it indexes implementation
> paths and the generated builtin/module inventory. CI checks freshness, not
> behavioral completeness. Do **not** start with `Docs/ADR-*` (intent/history, may be
> superseded). This file is the Claude Code *integration* guide; CODEMAP is the
> code map.
>
> **Auditing bugs or incomplete features?** Read `Docs/AUDITING.md`. Historical
> findings and registry descriptions are leads, not verdicts. Record the build,
> dispatch path and a reproducible observed result for each claimed defect.
>
> **Executing the agent JSON / IPC roadmap?** Follow
> [`Docs/ADR-0056-agent-contract-and-local-data-plane-20-waves.md`](Docs/ADR-0056-agent-contract-and-local-data-plane-20-waves.md).
> Read the active wave, dependencies and execution record before editing. Update
> its checklist, evidence and next action at every handoff; never mark a wave
> DONE from a conversation summary or without its required gates.
> **Current target: Linux first.** W18 (macOS/BSD ports) is deferred; W20's Linux
> qualification depends on W01–W17 and W19. Keep OSIF boundaries portable and
> qualify new backends separately when W18 resumes (ADR-0056 §1.3).
>
> **If you are an LLM agent using jash as your shell, also read
> [`Docs/EXOCORTEX.md`](Docs/EXOCORTEX.md)** — the superpowers jash gives you that
> bash/zsh don't: `recall` (memory), `checkpoint`/`rewind`/`trash on` (undo any
> step), `expect` (kill hallucination), `explain` (blast radius before running),
> the context governor (your window is protected), and `mesh` (multi-model swarm
> coordination). Runnable walkthrough: [`examples/exocortex/demo.sh`](examples/exocortex/demo.sh).

## Mandatory before starting any change

**Always commit the current working state first.** Before touching any file, run:
```bash
git add -u && git commit -m "checkpoint: pre-<task-description>"
```
This creates an instant rollback point. No backup = no safety net if mistakes compound.

## House rules for agents (use the differentials — that's the point)

> **Setting up jash as Claude Code's shell for the first time?** The drop-in kit
> in [`integrations/claude-code/`](integrations/claude-code/) is the canonical
> standard: a `settings.json` (recording on) + a `CLAUDE.jash.md` rules block
> that makes Claude *use* jash instead of bypassing to `/bin/bash`. The rules
> below are that block, distilled.

Recording auto-enables for your `jash -c` calls, so the floor is free: commands are
remembered, `rm` is recoverable (an `auto` checkpoint is dropped before it), and
huge output is capped. On top of that, make these habits:

1. **Verify, don't assume.** After a build/change, assert it: `make && expect file
   build/app && expect rc 0`. A wrong assumption then stops loudly instead of
   compounding. (This is your #1 failure mode.)
2. **Undo, don't despair.** Broke something? `rewind auto` (undo the last `rm`),
   `rewind --session` (roll back the whole session), or `undo` (one step). Reach for
   these before `git checkout`.
3. **Recall, don't re-derive.** "Did I try this? did it work?" → `recall did "X"` /
   `recall failed`. The shell remembers what your context window dropped.
4. **Look before you leap.** Unsure what a command will touch? `explain "<cmd>"`
   shows the blast radius without running it.
5. **Don't flood your window.** Never `cat` a big file — `recall grep PAT` the
   capped overflow, or `head`/`grep` directly.

Check your own usage: `recall adoption` (% of commands that used a differential).
Full reference: [`Docs/EXOCORTEX.md`](Docs/EXOCORTEX.md).

## Build

```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target jash
```

Binary: `./build/jash` (copied from `./build/jash-dev` on success — safe: failed
links never clobber the shell binary the tool depends on).

## Module build

```bash
bash modules/build_module.sh modules/<name>
# output: modules/<name>/<name>.modjash
```

## Running tests

```bash
bash tests/test_suite.sh
```

## Verifiable execution — Claude Code integration

When jash is used as Claude Code's shell, set `JASH_SIGIL=2` in the environment
to automatically enable cryptographic chain logging for every Bash tool call:

```json
// .claude/settings.json
{
  "env": {
    "JASH_SIGIL": "2"
  }
}
```

With `JASH_SIGIL=2`, every command executed by the agent is:
1. Hashed into a BLAKE3 hash chain with a forward-secure ratchet (`~/.jash/chain.log`)
2. Covered by a sovereign **Ed25519** session proof (`~/.jash/session.jashproof`),
   verifiable by anyone holding the public key — no shared secret (see `SPEC.md`)

After the session, verify and export the artifact:

```
sigil chain verify          # verify the chain is unbroken
sigil history/diff/bisect   # git-like UX over the chain
sigil proof emit            # emit Ed25519 proof (also auto-emitted at exit)
sigil proof verify          # confirm signature with the public key
sigil export                # package as OCI artifact
sigil push ghcr.io/org/jash-audit:latest   # push via oras
```

The OCI artifact at `~/.jash/oci-export/` follows the standard image layout
and can be pushed with `oras` or consumed by any OCI-compliant registry.

## Sigil levels

| Level | Name | Effect |
|-------|------|--------|
| 0 | off  | No recording — zero overhead |
| 1 | log  | Write to `~/.jash/chain.log` |
| 2 | sign | LOG + Ed25519 proof (`~/.jash/session.jashproof`), auto-emitted at exit |
| 3 | seal | SIGN + adversarial 4-attack battery on restore |

Set level: `sigil <off|log|sign|seal>` or `JASH_SIGIL=<0-3>`.

## Key files

| Path | Description |
|------|-------------|
| `c/module/sigil.c` | Sigil state + LOG writer |
| `c/module/jash_crypto.c` | SHA-256 + HMAC-SHA256 |
| `c/module/loader.c` | Module loader + cap enforcement |
| `modules/sigil/sigil_module.c` | Full chain/proof/export module |
| `modules/sigil/sigil.modjash` | Pre-built module (load with `module load`) |
| `~/.jash/chain.log` | Append-only hash chain |
| `~/.jash/session.jashproof` | Ed25519 session proof (self-describing pubkey) |
| `~/.jash/keys/ed25519.seed` | sovereign Ed25519 signing seed (mode 0600) |
| `~/.jash/oci-export/` | OCI layout for `oras push` |

## Commit rules

- Never add `Co-Authored-By` to commits or code, regardless of model.

## Author signature

- When creating or editing ADRs (Architecture Decision Records), the Author field must read: `F E R M I ∞ H A R T` (spaces between each character, infinity symbol U+221E).
- This signature also applies to the Sign-off section.
