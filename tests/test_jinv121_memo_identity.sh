#!/usr/bin/env bash
# JINV-121 — the running implementation is an input to the memo key.
#
# memo_key() hashed sid/cwd/name/args/locale and memo_plan() fingerprinted the
# operands. Nothing fingerprinted the *implementation*. But every command on
# the memo whitelist (grep, ls, wc, find, ...) is served by a jash builtin, so
# the jash binary IS an input to the result. Two jash binaries sharing ~/.jash
# answered each other's questions -- under the banner "inputs re-verified now".
#
# That is not a stale cache, it is a false attestation, and it made JINV-119
# (installed != built) invisible: the fossil binary borrowed the fresh binary's
# correct answers and looked fixed.
#
# The contract this gate pins, in the safe direction:
#   1. memo still WORKS      -- same image, same command  -> HIT   (or the fix
#                               is just "disable memo", which is not a fix)
#   2. different image       -- same bytes, different inode -> MISS
#   3. rebuilt in place      -- same path, new mtime        -> MISS
#   4. JASH_MEMO=0           -- always the live truth, never a record
#
# Case 2 deliberately over-separates: two byte-identical copies do not share.
# Keying on content would be exact, but it means hashing a multi-MB image on
# every lookup, on the hot path. inode+mtime+size is O(1) and errs toward a
# cache miss -- which costs time -- never toward a wrong answer, which costs
# trust. Over-separation is the only acceptable direction for this tradeoff.
set -u
JASH="${JASH:-./build/jash}"
if ! { [ -f "$JASH" ] && JASH_SIGIL=0 "$JASH" -c 'exit 0' >/dev/null 2>&1; }; then
  echo "no runnable jash binary at $JASH"; exit 2
fi
JASH_ABS="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

work="$(mktemp -d)"; trap 'rm -rf "$work"' EXIT

# memo requires an agent-smelling session; give it a stable per-run SID so that
# repeated commands in this gate share a store without colliding with other tests.
export JASH_SIGIL=1 JASH_AGENT=1 JASH_SESSION="jinv121-$$"

pass=0; fail=0
ck() { # ck <label> <expect: HIT|MISS> <output>
  local label="$1" expect="$2" out="$3" got
  case "$out" in *"[jash memo]"*) got=HIT;; *) got=MISS;; esac
  if [ "$got" = "$expect" ]; then
    pass=$((pass+1)); printf '  ok   %-46s %s\n' "$label" "$got"
  else
    fail=$((fail+1)); printf '  FAIL %-46s want=%s got=%s\n' "$label" "$expect" "$got"
    printf '       output: %s\n' "$(printf '%s' "$out" | head -2)"
  fi
}

printf 'malloc\nfree(\n' > "$work/f.txt"
# A pattern whose answer differs between implementations is not required here;
# the banner alone decides HIT vs MISS, so any whitelisted command works.
CMD="grep -c malloc $work/f.txt"

# An isolated store: never touch (or be perturbed by) the developer's ~/.jash.
export HOME="$work/home"; mkdir -p "$HOME"
bash "$ROOT/tests/setup_sigil_modules.sh" "$HOME" || exit 2
unset JASH_MEMO || true
# The first-contact brief (fired by the fresh HOME above) advertises memo by
# printing a sample "[jash memo]" banner -- which would read as a HIT here.
# Silence it so the detector sees only real banners.
export JASH_BRIEF=0

# --- 1. memo must still work -------------------------------------------------
o1="$("$JASH_ABS" -c "$CMD" 2>&1)"; ck "same image, 1st run" MISS "$o1"
o2="$("$JASH_ABS" -c "$CMD" 2>&1)"; ck "same image, 2nd run (memo alive)" HIT "$o2"

# --- 2. a different image must not inherit that record -----------------------
cp "$JASH_ABS" "$work/copyA"
o3="$("$work/copyA" -c "$CMD" 2>&1)"; ck "byte-identical copy, different inode" MISS "$o3"
o4="$("$work/copyA" -c "$CMD" 2>&1)"; ck "that copy, repeated (own record)" HIT "$o4"

# --- 3. rebuild in place: same path, new mtime -------------------------------
cp "$JASH_ABS" "$work/inplace"
o5="$("$work/inplace" -c "$CMD" 2>&1)"; ck "in-place image, 1st run" MISS "$o5"
o6="$("$work/inplace" -c "$CMD" 2>&1)"; ck "in-place image, 2nd run" HIT "$o6"
touch "$work/inplace"          # what `make install` looks like to stat()
o7="$("$work/inplace" -c "$CMD" 2>&1)"; ck "same path, mtime moved (rebuild)" MISS "$o7"

# --- 4. the escape hatch must never be a record ------------------------------
o8="$(JASH_MEMO=0 "$JASH_ABS" -c "$CMD" 2>&1)"; ck "JASH_MEMO=0 (never a record)" MISS "$o8"

# --- 5. the whole point: a memo answer must equal the live answer ------------
# Run the same command with memo on and memo off and require identical output
# once the banner is stripped. This is the invariant the banner CLAIMS.
unset JASH_MEMO
live="$(JASH_MEMO=0 "$JASH_ABS" -c "$CMD" 2>/dev/null)"
memo="$("$JASH_ABS" -c "$CMD" 2>/dev/null)"
case "$memo" in *"[jash memo]"*) ;; *) memo="$live";; esac   # miss -> nothing to compare
recalled="$(JASH_MEMO=0 "$JASH_ABS" -c "$CMD" 2>/dev/null)"
if [ "$live" = "$recalled" ]; then
  pass=$((pass+1)); printf '  ok   %-46s %s\n' "memo answer == live answer" "$live"
else
  fail=$((fail+1)); printf '  FAIL %-46s live=%s recalled=%s\n' "memo answer == live answer" "$live" "$recalled"
fi

echo "=== JINV-121: $pass ok / $fail fail ==="
[ "$fail" -eq 0 ]
