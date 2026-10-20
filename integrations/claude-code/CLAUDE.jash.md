<!-- ════════════════════════════════════════════════════════════════════════
     jash + Claude Code — operating rules (drop-in)
     Paste this block into your project's CLAUDE.md (or `/memory`) so Claude
     actually USES jash instead of treating it as plain bash.
     Source: https://github.com/  (jash) · integrations/claude-code/
     ════════════════════════════════════════════════════════════════════════ -->

# You are running on jash — use it, don't bypass it

Your shell is **jash**, an *agent exocortex*: it gives you memory (`recall`),
undo (`rewind`/`checkpoint`), verification (`expect`), and blast-radius preview
(`explain`) that bash/zsh do not. These are WHY your results are faster, safer,
and more honest than on plain bash. Using them is not optional. The shell
measures your usage — run `recall adoption` and keep it high.

## ① The rule that matters most: run THROUGH jash

**Do not shell out to `/bin/bash`, and do not disable the sandbox, unless a
command genuinely cannot run otherwise.** Every command that bypasses jash:
- is **not recorded** → `recall` can't help you later,
- gets **no auto-checkpoint** → `rewind` can't undo it,
- is **not attested** → no proof, no `dashboard` credit.

Bypassing jash is the #1 way an agent silently throws away everything jash
gives it. If a build/test/git command works in jash, run it in jash.

## ② Use the differentials — by situation

1. **Verify, don't assume.** After a build or change, assert it:
   `make && expect file build/app && expect rc 0`. A wrong assumption then
   stops loudly instead of compounding. (This is your #1 failure mode.)
2. **Undo, don't despair.** Broke something? `rewind auto` (undo the last `rm`),
   `rewind --session` (roll back the whole session), or `undo` (one step) —
   reach for these BEFORE `git checkout`. `rm` is auto-checkpointed; deletions
   are recoverable.
3. **Recall, don't re-derive.** "Did I try this? did it work?" →
   `recall did "X"` / `recall failed`. jash remembers what your context dropped.
4. **Look before you leap.** Unsure what a command touches?
   `explain "<cmd>"` shows the blast radius (delete/write/network) WITHOUT running.
5. **Don't flood your window.** Never `cat` a big file — `recall grep PAT` the
   capped overflow, or `head`/`grep` directly.

## ③ Measure yourself

`recall adoption` and `dashboard` show the % of your commands that used a jash
differential, computed from the attested chain. Low % = you've reverted to bash
habits. Check it after a work block and course-correct.

## ④ Known quirks (workarounds until fixed upstream)

- **git commits / heredocs:** a multi-line `-m "..."` or an inline heredoc can
  mangle. Write the message to a file and use `git commit -F msg.txt`.
- Prefer one logical command per call; jash records each call as one attested step.

<!-- end jash block -->
