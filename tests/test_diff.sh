#!/usr/bin/env bash
# tests/test_diff.sh — ADR-0044: `diff` builtin (Myers O(ND) engine, structured
# JSON gold). See Docs/ADR-0044-diff-engine-design.md.
#
# THE PARITY TRAP: two correct diffs of the same input can disagree byte-for-
# byte on hunk grouping (GNU/BSD/git diff often disagree while all valid), so
# this gate does NOT assert byte-exact parity vs /usr/bin/diff. The primary
# correctness gate is round-trip: `diff -u a b | patch` (and normal format)
# must reproduce `b` exactly. Curated fixed-string cases are kept minimal, for
# inputs where every implementation agrees.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash_diff.XXXXXX")" || exit 1
trap 'rm -rf "$T"' EXIT

# Admission is an actual launch in the private fixture, not an executable bit.
can_run() { env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
    JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_ADOPT=0 JASH_LOOPGUARD=off \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1; }
if [[ -n "${JASH:-}" ]]; then
    can_run "$JASH" || { echo "FAIL: explicitly selected JASH cannot execute" >&2; exit 1; }
else
    JASH=
    for candidate in "$PROJECT_ROOT/build/jash-release" "$PROJECT_ROOT/build/jash" "$PROJECT_ROOT/build-speed/jash-release"; do
        if can_run "$candidate"; then JASH="$candidate"; break; fi
    done
    if [[ -z "$JASH" ]]; then echo "SKIP: no runnable jash binary"; exit 77; fi
fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }
# PATH kept real so the fallback path can find the system diff/patch.
run(){ env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
       JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off "$JASH" -c "$1"; }

cd "$T"

# ── 1. identical files → empty output, rc 0 ──────────────────────────────────
printf 'one\ntwo\nthree\n' > id_a.txt
printf 'one\ntwo\nthree\n' > id_b.txt
out="$(run "diff id_a.txt id_b.txt")"; rc=$?
[[ -z "$out" && "$rc" -eq 0 ]] && ok "identical files: empty output, rc 0" || no "identical files" "out='$out' rc=$rc"

# ── 2. pure single-line change → curated fixed string (normal format) ───────
printf 'alpha\nbeta\ngamma\n' > chg_a.txt
printf 'alpha\nBETA\ngamma\n' > chg_b.txt
out="$(run "diff chg_a.txt chg_b.txt")"; rc=$?
expected=$'2c2\n< beta\n---\n> BETA'
[[ "$out" == "$expected" && "$rc" -eq 1 ]] && ok "single change: 2c2 normal format, rc 1" || no "single change" "out='$out' rc=$rc"

# ── 3. pure insert → curated fixed string ────────────────────────────────────
printf 'x\ny\n' > ins_a.txt
printf 'x\ny\nz\n' > ins_b.txt
out="$(run "diff ins_a.txt ins_b.txt")"; rc=$?
expected=$'2a3\n> z'
[[ "$out" == "$expected" && "$rc" -eq 1 ]] && ok "pure insert: 2a3, rc 1" || no "pure insert" "out='$out' rc=$rc"

# ── 4. pure delete → curated fixed string ────────────────────────────────────
printf 'x\ny\nz\n' > del_a.txt
printf 'x\nz\n' > del_b.txt
out="$(run "diff del_a.txt del_b.txt")"; rc=$?
expected=$'2d1\n< y'
[[ "$out" == "$expected" && "$rc" -eq 1 ]] && ok "pure delete: 2d1, rc 1" || no "pure delete" "out='$out' rc=$rc"

# ── 5. no-trailing-newline marker + round-trip ───────────────────────────────
printf 'p\nq\nr' > nonl_a.txt
printf 'p\nq\nR' > nonl_b.txt
out="$(run "diff -u nonl_a.txt nonl_b.txt")"
if [[ "$out" == *'\ No newline at end of file'* ]]; then
    ok "no-trailing-newline marker present"
else no "no-trailing-newline marker" "$out"; fi
run "diff -u nonl_a.txt nonl_b.txt" > nonl.patch
cp nonl_a.txt nonl_rt.txt
if patch -s nonl_rt.txt < nonl.patch >/dev/null 2>&1 && cmp -s nonl_rt.txt nonl_b.txt; then
    ok "no-trailing-newline round-trip via patch"
else no "no-trailing-newline round-trip" "$(cmp nonl_rt.txt nonl_b.txt 2>&1)"; fi

# ── 6. round-trip: multi-hunk fixture, unified format ────────────────────────
printf 'line1\nline2\nline3\nline4\nline5\nline6\nline7\nline8\n' > rtA.txt
printf 'line1\nLINE2\nline3\nline4\nline5\nNEW\nline6\nline8\n' > rtB.txt
run "diff -u rtA.txt rtB.txt" > rt.patch
cp rtA.txt rt_out.txt
if patch -s rt_out.txt < rt.patch >/dev/null 2>&1 && cmp -s rt_out.txt rtB.txt; then
    ok "round-trip (-u, multi-hunk) reproduces b"
else no "round-trip -u multi-hunk" "$(cmp rt_out.txt rtB.txt 2>&1)"; fi

# ── 7. round-trip: same fixture, normal format ───────────────────────────────
run "diff rtA.txt rtB.txt" > rt_normal.patch
cp rtA.txt rt_out2.txt
if patch -s rt_out2.txt < rt_normal.patch >/dev/null 2>&1 && cmp -s rt_out2.txt rtB.txt; then
    ok "round-trip (normal format) reproduces b"
else no "round-trip normal" "$(cmp rt_out2.txt rtB.txt 2>&1)"; fi

# ── 8. --json: valid JSON + hunk shape ───────────────────────────────────────
if command -v python3 >/dev/null 2>&1; then
    out="$(run "diff --json chg_a.txt chg_b.txt")"
    if echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert isinstance(d, list) and len(d) == 1
h = d[0]
assert h["op"] == "change"
assert h["a"] == {"start": 1, "count": 1}
assert h["b"] == {"start": 1, "count": 1}
assert h["a_lines"] == ["beta"]
assert h["b_lines"] == ["BETA"]
' 2>"$T/diff_json_err"; then
        ok "--json: valid JSON, correct hunk shape"
    else no "--json shape" "$(cat "$T/diff_json_err")"; fi
else ok "--json validity (python3 absent — skipped)"; fi

# ── 9. --json on identical files → empty array ───────────────────────────────
out="$(run "diff --json id_a.txt id_b.txt")"
[[ "$out" == "[]" ]] && ok "--json identical files → []" || no "--json identical" "$out"

# ── 10. rc taxonomy: 0 same / 1 differ / 2 error ─────────────────────────────
run "diff id_a.txt id_b.txt" >/dev/null 2>&1; rc0=$?
run "diff chg_a.txt chg_b.txt" >/dev/null 2>&1; rc1=$?
run "diff id_a.txt does_not_exist.txt" >/dev/null 2>&1; rc2=$?
[[ "$rc0" -eq 0 && "$rc1" -eq 1 && "$rc2" -eq 2 ]] && ok "rc taxonomy: 0/1/2" || no "rc taxonomy" "rc0=$rc0 rc1=$rc1 rc2=$rc2"

# ── 11. fallback: -c (context format) defers to real diff, still works ──────
if command -v diff >/dev/null 2>&1; then
    out="$(run "diff -c chg_a.txt chg_b.txt")"
    [[ "$out" == *'***'* && "$out" == *'beta'* ]] && ok "-c falls back to real diff" || no "-c fallback" "$out"
else ok "-c fallback (system diff absent — skipped)"; fi

# ── 12. fallback: -r on two directories ──────────────────────────────────────
mkdir -p dirA dirB
printf 'same\n' > dirA/f.txt
printf 'same\n' > dirB/f.txt
run "diff -r dirA dirB" >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 0 ]] && ok "-r recursive dir diff falls back (identical dirs, rc 0)" || no "-r fallback" "rc=$rc"

# ── 13. --json + unsupported flag → loud error (rc 2), never silent fallback ─
run "diff --json -c chg_a.txt chg_b.txt" >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 2 ]] && ok "--json + -c: loud error rc 2 (no silent fallback)" || no "--json+-c error" "rc=$rc"

# ── 14. --json + directory operand → loud error (rc 2) ──────────────────────
run "diff --json dirA dirB" >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 2 ]] && ok "--json + directory operand: loud error rc 2" || no "--json+dir error" "rc=$rc"

echo "── diff builtin (ADR-0044) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
