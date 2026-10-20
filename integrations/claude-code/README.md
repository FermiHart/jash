# jash + Claude Code — the standard setup

Make your Claude run **a todo vapor** on jash: with memory, undo, verification,
and attestation — instead of treating jash as plain bash and throwing all of
that away.

This is the missing piece. Claude Code's Bash tool will *run through* jash, but
nothing tells Claude to **use** jash's superpowers — so by default it falls back
to bash habits and adoption reads 0%. These three steps fix that for any user.

## Install (3 steps)

**1. Make jash the shell Claude Code runs.**
```sh
# install jash, then point Claude Code's shell at it:
chsh -s "$(command -v jash)"        # or set your terminal/Claude Code shell to jash
```
Verify: open Claude Code, run any command — you should see the `[jash]` footer.

**2. Copy the settings (env + SessionStart hook).**
```sh
mkdir -p .claude
cp integrations/claude-code/settings.json .claude/settings.json   # per-project
# or merge its "env" + "hooks" blocks into ~/.claude/settings.json for all projects
```
This turns on recording (`JASH_SIGIL=2`), the failure nudge, the context
governor — and, decisively, the **`SessionStart` hook** (ADR-0037): it injects
`jash --agent-guide` into Claude's context at t=0, so the model knows it is in
jash *before its first command*. Without this, models assume bash and bypass
every differential (observed live — see the ADR).

**3. Give Claude the operating rules.**
Append `integrations/claude-code/CLAUDE.jash.md` to your project's `CLAUDE.md`
(or paste it via `/memory`). This is what makes Claude *use* the differentials
and stop bypassing to `/bin/bash`.
```sh
cat integrations/claude-code/CLAUDE.jash.md >> CLAUDE.md
```

**Other harnesses (Codex CLI, Gemini CLI, Aider, Cursor, opencode, Qwen,
custom):** no hook needed — jash itself briefs the agent. On the first `-c`
command of an agent session (auto-detected from the harness's own env markers;
`JASH_AGENT=1` is the universal opt-in for anything unrecognized), it prints a
12-line capability brief on **stderr** (never stdout — captures stay clean),
once per session, pointing to `jash --agent-guide` — which covers **100% of
the surface** (all builtins from the live registry, every `JASH_*` env var,
every CLI flag) and is CI-enforced against the source so it can never go
stale. `JASH_BRIEF=0` disables the brief.

## Verify it's working

After a work session, ask Claude (or run):
```sh
recall adoption     # % of commands that used a jash differential (from the attested chain)
dashboard           # adoption + activity + safety at a glance
```
If adoption is low, Claude reverted to bash habits — the CLAUDE.md rules in step 3
are what pull it back. Watch the number climb as the rules take hold.

## Why this matters (the lesson that created this kit)

Even the agent that *built* jash's features used them ~0% in a long session,
because:
1. it reflexively shelled out to `/bin/bash` (sandbox-disabled) → commands never
   reached jash → no recording, no undo, no proof; and
2. the Claude Code shell tool wraps every command as
   `setopt … && eval '<cmd>' …`, which originally hid the differential from the
   adoption meter (fixed in jash so `recall adoption` now sees through it).

The fix for (1) is **instruction** — that's `CLAUDE.jash.md`. The floor (auto
recording, auto-checkpoint, output capping) only protects an agent that routes
*through* jash; the rules keep it there.
