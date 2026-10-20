#!/usr/bin/env bash
# tests/test_distill.sh — ADR-0042 F10: session → executable recipe.
#
# distill mines the attested chain into a re-runnable recipe, keeping only the
# load-bearing steps (mutations, fixes, assertions) and dropping pure reads.
# `recipe run` checkpoints first, evals sequentially, and aborts on a failed
# assertion with the rewind handle. Second-precision exec-window correlation
# (mutations spaced 1s, as in real multi-step work).
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
run() { env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
            JASH_SIGIL=2 JASH_BRIEF=0 JASH_NUDGE=0 JASH_SESSION=s1 "$@"; }

printf 'hello xyz\n' > "$T/real"; printf 'src\n' > "$T/a"

# Build a fail→fix→mutation→assertion→reads chain, 1s apart.
run "$JASH" -c "grep xyz $T/nonexistent" >/dev/null 2>&1; sleep 1   # FAIL (rc≠0)
run "$JASH" -c "grep xyz $T/real"        >/dev/null 2>&1; sleep 1   # FIX
run "$JASH" -c "cp $T/a $T/F"            >/dev/null 2>&1; sleep 1   # MUTATION
run "$JASH" -c "expect file $T/F"        >/dev/null 2>&1; sleep 1   # ASSERTION
run "$JASH" -c "cat $T/real"             >/dev/null 2>&1; sleep 1   # READ (drop)
run "$JASH" -c "ls $T"                   >/dev/null 2>&1            # READ (drop)

# ── 1. distill keeps fix+mutation+assertion, drops reads ─────────────────────
d=$(run "$JASH" -c "distill build1" 2>&1)
case "$d" in
  *"1 fix"*"1 mutation"*"1 assertion"*"3 reads dropped"*)
      ok "distill kept fix+mutation+assertion, dropped 3 reads";;
  *) err "distill classification ('$d')";;
esac

# ── 2. recipe run reproduces; failed assertion aborts rc≠0 + rewind handle ───
run "$JASH" -c "rm $T/F" >/dev/null 2>&1      # remove the mutation target
run "$JASH" -c "recipe run build1" >/dev/null 2>&1; rc=$?
if [[ $rc -eq 0 && -e "$T/F" ]]; then ok "recipe run reproduces end state (F recreated, rc 0)"
else err "recipe run reproduce (rc=$rc, F exists=$([[ -e $T/F ]] && echo y || echo n))"; fi

run "$JASH" -c "distill onlyassert --from 3 --to 3" >/dev/null 2>&1   # just the assertion
run "$JASH" -c "rm $T/F" >/dev/null 2>&1                               # make it fail
ab=$(run "$JASH" -c "recipe run onlyassert" 2>&1); arc=$?
if [[ $arc -ne 0 ]] && echo "$ab" | grep -q "assertion failed" && echo "$ab" | grep -q "rewind recipe-onlyassert"; then
  ok "failed assertion aborts rc≠0 with the rewind handle"
else
  err "assertion abort (rc=$arc, out='$ab')"
fi

# ── 3. param substitution binds only declared params ─────────────────────────
run "$JASH" -c "cp $T/a $T/F" >/dev/null 2>&1; sleep 1
run "$JASH" -c "expect file $T/F" >/dev/null 2>&1
run "$JASH" -c "distill p1 --param TARGET=$T/F" >/dev/null 2>&1
show=$(run "$JASH" -c "recipe show p1")
echo "$show" | grep -q '${TARGET}' && ok "declared literal rewritten to \${TARGET}" \
                                   || err "param rewrite ('$show')"
# run binding TARGET to a NEW path + an UNDECLARED param → only TARGET binds
run "$JASH" -c "recipe run p1 TARGET=$T/G FOO=bar" >/dev/null 2>&1
if [[ -e "$T/G" ]]; then ok "recipe run bound \${TARGET} to the new path (undeclared FOO ignored)"
else err "param binding did not take effect"; fi

# ── 4. empty range → rc 2 ────────────────────────────────────────────────────
run "$JASH" -c "distill empty --from 99000 --to 99001" >/dev/null 2>"$T/e"; erc=$?
[[ $erc -eq 2 ]] && grep -q "empty chain range" "$T/e" \
    && ok "empty range → rc 2 with honest message" || err "empty range (rc=$erc)"

# ── 5. recipe steps are attested (chain verify passes) ───────────────────────
v=$(run "$JASH" -c "sigil chain verify" 2>&1)
echo "$v" | grep -qiE 'intact|valid|verified' \
    && ok "chain verify passes after recipe runs (steps attested)" \
    || err "chain verify ('$v')"

echo "═══════════════════════════════════════════════════════════════"
echo "  distill/recipe (ADR-0042 F10) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ distill tests PASSED"; exit 0; } \
                  || { echo "  ✗ distill tests FAILED"; exit 1; }
