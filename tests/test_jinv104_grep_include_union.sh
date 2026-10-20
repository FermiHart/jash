#!/usr/bin/env bash
# JINV-104 — `--include` is a UNION, and it accepts the separated form.
#
# Two defects, one flag:
#
#   1. LAST-WINS. `--include` was stored in a single scalar (`include_glob`),
#      so each occurrence overwrote the previous one. `grep -r --include=*.h
#      --include=*.c PAT .` searched ONLY the .c files and silently skipped
#      every header. For an agent this is the worst possible failure mode: an
#      empty result set reads as "this symbol is not in the codebase", so the
#      wrong conclusion is drawn confidently and no error is ever printed.
#      GNU semantics are a union — a file is searched if it matches ANY glob.
#
#   2. NO SEPARATED FORM. Only `--include=GLOB` parsed; `--include GLOB` fell
#      through to the operand collector and was read as a PATTERN or a FILE.
#      `--exclude-dir` already accepted both spellings, so the inconsistency
#      was internal to this same builtin.
#
# Fix: same storage shape as `--exclude` (a 16-slot glob array) plus
# grep_name_included() — "no globs given ⇒ everything passes, otherwise match
# at least one".
#
# This gate asserts ABSOLUTE expected sets, not just parity with the host grep,
# so it still means something on a box whose /usr/bin/grep differs (BSD vs GNU).

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
# UX chatter (nudge/brief/loopguard) goes to stderr, which is correct — but it
# must never be compared against program output. This has broken gates before.
export JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off
[ -x "$JASH" ] || { echo "SKIP: $JASH missing"; exit 0; }

pass=0; fail=0
ok()  { pass=$((pass+1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail+1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

TMP=$(mktemp -d) || exit 2
trap 'rm -rf "$TMP"' EXIT INT TERM
cd "$TMP" || exit 2

mkdir -p sub
printf 'needle here\n' > a.c
printf 'needle here\n' > b.h
printf 'needle here\n' > c.txt
printf 'needle here\n' > sub/d.c
printf 'needle here\n' > sub/e.h

# Run jash's grep in $TMP and return the sorted list of MATCHING FILES.
jgrep() { "$JASH" -c "cd '$TMP' && grep -r $1 needle . 2>/dev/null" \
            | sed 's|^\./||; s|:.*||' | sort | tr '\n' ' ' | sed 's/ $//'; }

check() { # name, args, expected
    got=$(jgrep "$2")
    if [ "$got" = "$3" ]; then ok "$1"; else bad "$1" "expected [$3] got [$got]"; fi
}

# ── 1. THE BUG: two --include globs must UNION, not last-wins ──────────────
check "union: --include=*.h --include=*.c hits both" \
      "--include='*.h' --include='*.c'" \
      "a.c b.h sub/d.c sub/e.h"

# ── 2. Order must not change the result (proves it is not last-wins) ───────
check "union is order-independent" \
      "--include='*.c' --include='*.h'" \
      "a.c b.h sub/d.c sub/e.h"

# ── 3. A single --include still narrows (the union did not become a no-op) ─
# If the fix had made every file pass, tests 1-2 would pass for the WRONG
# reason. This is the guard against over-correcting.
check "single --include still filters" \
      "--include='*.c'" \
      "a.c sub/d.c"

# ── 4. No --include at all ⇒ every file is searched ────────────────────────
check "no --include searches everything" \
      "" \
      "a.c b.h c.txt sub/d.c sub/e.h"

# ── 5. Separated form parses as a FLAG, not as pattern/file ────────────────
check "separated form: --include '*.c'" \
      "--include '*.c'" \
      "a.c sub/d.c"

# ── 6. Both spellings mix freely in one command line ───────────────────────
check "mixed --include '*.c' --include='*.h'" \
      "--include '*.c' --include='*.h'" \
      "a.c b.h sub/d.c sub/e.h"

# ── 7. --exclude still overrides the include set ──────────────────────────
# Interaction check: include is a whitelist, exclude is a veto applied after.
check "exclude vetoes an included file" \
      "--include='*.c' --include='*.h' --exclude='e.*'" \
      "a.c b.h sub/d.c"

# ── 8. RISK OF THE FIX: the 16-glob cap must clamp, never overflow ────────
# 17 globs written into a 16-slot array is exactly how a bounds bug becomes a
# stack smash. Extra globs are dropped; the process must still exit cleanly.
many=""
for i in 1 2 3 4 5 6 7 8 9 10 11 12 13 14 15 16; do many="$many --include='*.x$i'"; done
out=$("$JASH" -c "cd '$TMP' && grep -r $many --include='*.c' needle . >/dev/null 2>&1; echo rc=\$?")
if [ "$out" = "rc=0" ] || [ "$out" = "rc=1" ]; then
    ok "17th glob clamps at the cap without crashing ($out)"
else
    bad "glob cap" "expected rc=0 or rc=1, got [$out] — overflow or signal"
fi

# ── 9. State does not leak between invocations in one shell ───────────────
# The globs live in file-scope statics. If they are not reset per invocation,
# a --include from an earlier command silently filters a later one — a bug
# that only ever shows up in a long-lived agent session, never in a one-shot.
leak=$("$JASH" -c "cd '$TMP' && grep -r --include='*.c' needle . >/dev/null 2>&1; \
                   grep -r needle . 2>/dev/null | sed 's|^\./||; s|:.*||' | sort \
                   | tr '\n' ' ' | sed 's/ \$//'")
if [ "$leak" = "a.c b.h c.txt sub/d.c sub/e.h" ]; then
    ok "include globs reset between invocations (no state leak)"
else
    bad "state leak" "second grep saw [$leak] — statics not reset per invocation"
fi

echo ""
echo "=== JINV-104: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
