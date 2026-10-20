#!/usr/bin/env bash
# tests/test_blamefile.sh — ADR-0042 F7: attested authorship per file.
#
# blame-file cross-references the undo timeline (which files were mutated, how,
# when) with the sigil chain (the exact command + seq) by timestamp window.
# Honest v1 scope: per-file, over jash-mediated mutations (cp/mv/rm) recorded in
# the timeline; second-precision attribution (realistic multi-session mutations
# are seconds/minutes apart).
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"          # sigil.modjash resolves via ./modules/sigil/
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }
HAVE_JQ=0; command -v jq >/dev/null 2>&1 && HAVE_JQ=1

# SIGIL=2 so mutations are chained (for the cross-reference); persistent
# JASH_SESSION so the timeline accumulates across -c calls.
run() { env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
            JASH_SIGIL=2 JASH_BRIEF=0 JASH_NUDGE=0 JASH_SESSION=s1 "$@"; }

printf 'A\n' > "$T/a"; printf 'B\n' > "$T/b"; printf 'orig\n' > "$T/F"

# ── 1. two mutations, spaced, each attributed to its own command ─────────────
run "$JASH" -c "cp $T/a $T/F" >/dev/null 2>&1
sleep 1
run "$JASH" -c "cp $T/b $T/F" >/dev/null 2>&1
j=$(run "$JASH" -c "blame-file $T/F --json" 2>/dev/null)
nmod=$(printf '%s' "$j" | grep -o '"kind":"modified"' | wc -l | tr -d ' ')
[[ "$nmod" -eq 2 ]] && ok "both mutations listed (2 modified)" \
                    || err "expected 2 modified, got $nmod"
if [[ $HAVE_JQ -eq 1 ]]; then
  ndist=$(printf '%s' "$j" | jq '[.[]|select(.kind=="modified")|.chain]|unique|length' 2>/dev/null)
  [[ "$ndist" == "2" ]] && ok "each mutation attributed to a distinct chain entry" \
                        || err "attribution not distinct (chains: $(printf '%s' "$j" | jq -c '[.[].chain]' 2>/dev/null))"
else
  echo "· (jq absent — skipping distinct-attribution sub-check)"
fi

# ── 2. deleted file → kind:deleted with a restorable note ────────────────────
run "$JASH" -c "rm $T/F" >/dev/null 2>&1
d=$(run "$JASH" -c "blame-file $T/F --json" 2>/dev/null)
case "$d" in
  *'"kind":"deleted"'*'"restorable":true'*) ok "delete recorded as kind:deleted + restorable";;
  *) err "deleted entry ('$d')";;
esac

# ── 3. --json parses cleanly ─────────────────────────────────────────────────
if [[ $HAVE_JQ -eq 1 ]]; then
  if printf '%s' "$d" | jq -e 'type=="array" and length>=1' >/dev/null 2>&1; then
    ok "--json output parses as a non-empty array"
  else
    err "--json did not parse"
  fi
else
  case "$d" in [*]*|*']') ok "--json output is bracketed (jq absent)";; *) err "--json shape";; esac
fi

# ── 4. unknown / untracked file → rc 2, clean error ──────────────────────────
run "$JASH" -c "blame-file $T/never-touched.txt" >/dev/null 2>"$T/e"; rc=$?
if [[ $rc -eq 2 ]] && grep -q "no attested history" "$T/e"; then
  ok "unknown file → rc 2 with a clean error"
else
  err "unknown file (rc=$rc, err='$(cat "$T/e")')"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  blame-file (ADR-0042 F7) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ blame-file tests PASSED"; exit 0; } \
                  || { echo "  ✗ blame-file tests FAILED"; exit 1; }
