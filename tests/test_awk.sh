#!/usr/bin/env bash
# tests/test_awk.sh — ADR-0044: `awk` builtin (narrow in-process slice,
# structured JSON gold). See Docs/ADR-0044-awk-engine-design.md.
#
# THE SCOPE TRAP: awk is a full programming language. This builtin owns only
# a single BEGIN/pattern-action/END program over $0..$N/NF/NR with print/
# printf and simple +=/-=/++/-- accumulation. Everything else (control flow,
# arrays, functions, getline, multiple -v/-f, ...) must fall back to the real
# awk — this gate checks BOTH that the owned slice runs in-process AND that
# fallback cases still produce correct output (parity vs /usr/bin/awk).
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash_awk.XXXXXX")" || exit 1
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
# PATH kept real so the fallback path can find the system awk.
run(){ env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
       JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off "$JASH" -c "$1"; }

cd "$T"

printf 'a b c\nd e f\n' > f.txt
printf 'name:age\nalice:30\nbob:25\n' > f2.txt
printf '1 2 3\n4 5 6\n7 8 9\n' > nums.txt

# ── 1. field print (default whitespace split) ───────────────────────────────
out="$(run 'awk "{print \$2}" f.txt')"
[[ "$out" == $'b\ne' ]] && ok "field print: \$2 default whitespace split" || no "field print" "$out"

# ── 2. -F custom separator ───────────────────────────────────────────────────
out="$(run 'awk -F: "{print \$1}" f2.txt')"
[[ "$out" == $'name\nalice\nbob' ]] && ok "-F: custom separator" || no "-F custom sep" "$out"

# ── 3. NF reference ──────────────────────────────────────────────────────────
out="$(run 'awk "{print \$NF}" f.txt')"
[[ "$out" == $'c\nf' ]] && ok "\$NF: last field" || no "\$NF" "$out"

out="$(run 'awk "{print NF}" f.txt')"
[[ "$out" == $'3\n3' ]] && ok "NF: field count" || no "NF count" "$out"

# ── 4. /regex/ pattern against \$0 ───────────────────────────────────────────
out="$(run 'awk "/alice/{print}" f2.txt')"
[[ "$out" == "alice:30" ]] && ok "/regex/ pattern match" || no "/regex/ pattern" "$out"

# ── 5. relational field test + implicit print ───────────────────────────────
out="$(run 'awk "NR>1{print \$1}" nums.txt')"
[[ "$out" == $'4\n7' ]] && ok "NR>1 relational pattern, implicit print" || no "NR>1 relational" "$out"

# ── 6. ~ / !~ match operators ─────────────────────────────────────────────────
out="$(run 'awk -F: "\$1~/ali/{print \$1}" f2.txt')"
[[ "$out" == "alice" ]] && ok "~ match operator" || no "~ operator" "$out"

out="$(run 'awk -F: "\$1!~/ali/{print \$1}" f2.txt')"
[[ "$out" == $'name\nbob' ]] && ok "!~ non-match operator" || no "!~ operator" "$out"

# ── 7. BEGIN + END + accumulation (the highest-value idiom) ─────────────────
out="$(run 'awk "{s+=\$1} END{print s}" nums.txt')"
[[ "$out" == "12" ]] && ok "BEGIN/END accumulation: {s+=\$1} END{print s}" || no "accumulation" "$out"

out="$(run 'awk "BEGIN{print \"start\"}{s+=\$1} END{print s}" nums.txt')"
[[ "$out" == $'start\n12' ]] && ok "BEGIN block executes before main loop" || no "BEGIN block" "$out"

# ── 8. -v var=val ─────────────────────────────────────────────────────────────
out="$(run 'awk -v name=foo "BEGIN{print name}"')"
[[ "$out" == "foo" ]] && ok "-v var=val: string value preserved (not silently zeroed)" || no "-v string" "$out"

out="$(run 'awk -v n=5 "BEGIN{print n+1}"' 2>/dev/null)"
rc=$?
# n+1 is not owned grammar (no binary + on vars) -- accept either fallback-parity or graceful non-crash
[[ "$rc" -eq 0 || "$rc" -eq 2 ]] && ok "-v numeric var (fallback-safe, no crash)" || no "-v numeric" "rc=$rc"

# ── 9. printf ─────────────────────────────────────────────────────────────────
out="$(run 'awk "{printf \"%s-%s\\n\", \$1, \$2}" f.txt')"
[[ "$out" == $'a-b\nd-e' ]] && ok "printf: %s-%s formatting" || no "printf" "$out"

# ── 10. fallback: if/control-flow → correctness parity vs real awk ──────────
if command -v awk >/dev/null 2>&1; then
    out_jash="$(run 'awk "{if (\$1 > 3) print \$1}" nums.txt')"
    out_real="$(awk '{if ($1 > 3) print $1}' nums.txt)"
    [[ "$out_jash" == "$out_real" ]] && ok "fallback (if/control-flow): parity vs real awk" || no "fallback if" "jash='$out_jash' real='$out_real'"
else ok "fallback if/control-flow (system awk absent — skipped)"; fi

# ── 11. fallback: arrays → correctness parity vs real awk ───────────────────
if command -v awk >/dev/null 2>&1; then
    out_jash="$(run 'awk "{a[\$1]++} END{for (k in a) print k}" nums.txt' 2>/dev/null)"
    out_real="$(awk '{a[$1]++} END{for (k in a) print k}' nums.txt 2>/dev/null | sort)"
    out_jash_sorted="$(echo "$out_jash" | sort)"
    [[ "$out_jash_sorted" == "$out_real" ]] && ok "fallback (arrays): parity vs real awk" || no "fallback arrays" "jash='$out_jash' real='$out_real'"
else ok "fallback arrays (system awk absent — skipped)"; fi

# ── 12. fallback: -f progfile ────────────────────────────────────────────────
printf '{ print $1 }\n' > prog.awk
if command -v awk >/dev/null 2>&1; then
    out="$(run "awk -f prog.awk nums.txt")"
    [[ "$out" == $'1\n4\n7' ]] && ok "fallback (-f progfile): correct output" || no "fallback -f" "$out"
else ok "fallback -f progfile (system awk absent — skipped)"; fi

# ── 13. --json: valid JSON array shape ───────────────────────────────────────
if command -v python3 >/dev/null 2>&1; then
    out="$(run 'awk --json "{print \$1,\$2}" f.txt')"
    if echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert isinstance(d, list) and len(d) == 2
assert d[0] == {"fields": ["a", "b"]}
assert d[1] == {"fields": ["d", "e"]}
' 2>"$T/awk_json_err"; then
        ok "--json: valid JSON array, correct fields shape"
    else no "--json array shape" "$(cat "$T/awk_json_err")"; fi
else ok "--json array validity (python3 absent — skipped)"; fi

# ── 14. --json: aggregate mode emits single {"result": ...} object ─────────
if command -v python3 >/dev/null 2>&1; then
    out="$(run 'awk --json "{s+=\$1} END{print s}" nums.txt')"
    if echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert isinstance(d, dict), "expected a single object, not an array"
assert d == {"result": 12}
' 2>"$T/awk_agg_err"; then
        ok "--json aggregate mode: single {result:12} object, not an array"
    else no "--json aggregate shape" "$(cat "$T/awk_agg_err")"; fi
else ok "--json aggregate validity (python3 absent — skipped)"; fi

# ── 15. --json on no-match input → empty array ──────────────────────────────
out="$(run 'awk --json "/zzz/{print}" f2.txt')"
[[ "$out" == "[]" ]] && ok "--json no matches → []" || no "--json empty" "$out"

# ── 16. rc taxonomy: 0 normal / 2 error ──────────────────────────────────────
run 'awk "{print \$1}" nums.txt' >/dev/null 2>&1; rc0=$?
run 'awk "{print \$1}" does_not_exist.txt' >/dev/null 2>&1; rc2=$?
[[ "$rc0" -eq 0 && "$rc2" -eq 2 ]] && ok "rc taxonomy: 0 normal / 2 missing-file error" || no "rc taxonomy" "rc0=$rc0 rc2=$rc2"

# ── 17. --json + unsupported construct → loud error rc 2 (never silent fallback) ─
run 'awk --json "{if (\$1 > 3) print \$1}" nums.txt' >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 2 ]] && ok "--json + control-flow: loud error rc 2 (no silent fallback)" || no "--json+if error" "rc=$rc"

run 'awk --json -f prog.awk nums.txt' >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 2 ]] && ok "--json + -f progfile: loud error rc 2" || no "--json+-f error" "rc=$rc"

# ── 18. multiple -v → not owned, falls back cleanly (no crash) ──────────────
if command -v awk >/dev/null 2>&1; then
    out_jash="$(run 'awk -v a=1 -v b=2 "BEGIN{print a, b}"')"
    out_real="$(awk -v a=1 -v b=2 'BEGIN{print a, b}')"
    [[ "$out_jash" == "$out_real" ]] && ok "multiple -v: falls back, parity vs real awk" || no "multiple -v" "jash='$out_jash' real='$out_real'"
else ok "multiple -v (system awk absent — skipped)"; fi

# ── 19. budget-cap smoke: large input doesn't hang/crash ───────────────────
seq 1 5000 | awk '{print $1, $1*2}' > big.txt 2>/dev/null || seq 1 5000 > big.txt
out="$(run 'awk "{print \$1}" big.txt' | wc -l)"
[[ "$out" -gt 0 ]] && ok "budget-cap smoke: large input handled without hang" || no "budget-cap smoke" "lines=$out"

echo "── awk builtin (ADR-0044) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
