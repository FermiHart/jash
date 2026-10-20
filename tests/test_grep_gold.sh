#!/usr/bin/env bash
# tests/test_grep_gold.sh — ADR-0044 Arc A2: `grep` gold (parity fixes +
# --json + --since + mtime-keyed negative cache).
#
# Covers 6 real bugs found+fixed (long-line truncation, -w/--smart broken on
# stdin, silently-dropping 64KB output cap + cross-file cache leak, stale
# wall-clock cache with no negative caching, a SIGSEGV on `grep -B`/`-C` over
# piped stdin, and permanently-dead after-context on stdin) plus two new gold
# features: --json match records and --since SEQ (chain-scoped recursive
# search).
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash_grep_gold.XXXXXX")" || exit 1
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

bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T"
pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }
run(){ env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
       JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off "$JASH" -c "$1"; }

cd "$T"

# ── 1. long-line: 8KB single line, correct -c count and -n numbering ────────
python3 - <<'EOF' > longline.txt 2>/dev/null || perl -e 'print "x"x8000 . "NEEDLE\n"' > longline.txt
print("x" * 8000 + "NEEDLE")
EOF
printf 'short\n' >> longline.txt
out="$(run 'grep -c NEEDLE longline.txt')"
[[ "$out" == "1" ]] && ok "long-line (8KB): -c finds match split across old 4KB boundary" || no "long-line -c" "$out"
out="$(run 'grep -n short longline.txt')"
[[ "$out" == "2:short" ]] && ok "long-line: -n numbering not inflated by the long first line" || no "long-line -n" "$out"

# ── 2. -w broken on stdin (bug 2) ────────────────────────────────────────────
out="$(printf 'theater only\n' | run 'grep -w the')"
rc=$?
[[ -z "$out" ]] && ok "-w on stdin: no false substring match ('the' inside 'theater')" || no "-w stdin" "out='$out'"

# ── 3. --smart broken on stdin (bonus find, same dispatch fix) ──────────────
out="$(printf 'fixed the bug\n' | run 'grep --smart "fix bug"')"
[[ -n "$out" ]] && ok "--smart on stdin: word-variation match now dispatches" || no "--smart stdin" "out='$out'"

# ── 4. 64KB cap: many matches, none silently dropped ─────────────────────────
python3 -c "
for i in range(2000):
    print(f'line{i} NEEDLE')
" > big.txt 2>/dev/null || { for i in $(seq 1 2000); do echo "line$i NEEDLE"; done > big.txt; }
n="$(run 'grep -c NEEDLE big.txt')"
[[ "$n" == "2000" ]] && ok "64KB-cap fix: 2000 matches all counted, none silently dropped" || no "64KB cap" "n=$n"

# ── 5. stale wall-clock cache fixed (mtime/size-keyed) ───────────────────────
printf 'v1\n' > cache_test.txt
out1="$(run 'grep v1 cache_test.txt')"
sleep 1
printf 'v2\n' > cache_test.txt
out2="$(run 'grep v1 cache_test.txt')"
out3="$(run 'grep v2 cache_test.txt')"
[[ "$out1" == "v1" && -z "$out2" && "$out3" == "v2" ]] && ok "cache: mtime-keyed, edited file never served stale bytes" \
    || no "stale cache" "out1='$out1' out2='$out2' out3='$out3'"

# ── 6. negative-cache correctness (two greps of a no-match file) ────────────
printf 'nothing here\n' > nomatch.txt
r1="$(run 'grep ZZZNOPE nomatch.txt')"; rc1=$?
r2="$(run 'grep ZZZNOPE nomatch.txt')"; rc2=$?
[[ -z "$r1" && -z "$r2" && "$rc1" -eq 1 && "$rc2" -eq 1 ]] && ok "negative cache: consistent empty result + rc 1 across repeats" \
    || no "negative cache" "r1='$r1' r2='$r2' rc1=$rc1 rc2=$rc2"

# ── 7. bug-5 crash regression: -B on piped stdin must not SIGSEGV ──────────
out="$(printf 'foo\nMATCH\n' | run 'grep -B1 MATCH')"; rc=$?
[[ "$rc" -eq 0 && "$out" == $'foo\nMATCH' ]] && ok "bug-5 fix: grep -B1 over piped stdin does not crash" \
    || no "bug-5 -B1 stdin" "rc=$rc out='$out'"

# ── 8. bug-6 dead-branch regression: -A on piped stdin must print context ──
out="$(printf 'MATCH\nafter1\nafter2\n' | run 'grep -A2 MATCH')"
expected=$'MATCH\nafter1\nafter2'
[[ "$out" == "$expected" ]] && ok "bug-6 fix: grep -A2 over piped stdin prints after-context" \
    || no "bug-6 -A2 stdin" "out='$out'"

# ── 9. --json basic shape ────────────────────────────────────────────────────
if command -v python3 >/dev/null 2>&1; then
    printf 'alpha\nbeta NEEDLE\ngamma\n' > json1.txt
    out="$(run 'grep --json NEEDLE json1.txt')"
    if echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert isinstance(d, list) and len(d) == 1
r = d[0]
assert r["file"] == "json1.txt"
assert r["line"] == 2
assert r["col"] == 6
assert r["match"] == "NEEDLE"
' 2>"$T/grep_json_err"; then
        ok "--json: valid JSON, correct {file,line,col,match} shape"
    else no "--json shape" "$(cat "$T/grep_json_err")"; fi
else ok "--json validity (python3 absent — skipped)"; fi

# ── 10. --json -o: multiple spans per line ───────────────────────────────────
if command -v python3 >/dev/null 2>&1; then
    printf 'foo NEEDLE bar NEEDLE baz\n' > json2.txt
    out="$(run 'grep --json -o NEEDLE json2.txt')"
    if echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert isinstance(d, list) and len(d) == 2
assert d[0]["col"] == 5
assert d[1]["col"] == 16
' 2>"$T/grep_json_o_err"; then
        ok "--json -o: one object per span, correct columns"
    else no "--json -o shape" "$(cat "$T/grep_json_o_err")"; fi
else ok "--json -o validity (python3 absent — skipped)"; fi

# ── 11. --json count observes selected lines (ADR-0056 completion) ─────────
out="$(run 'grep --json -c NEEDLE json1.txt' 2>"$T/grep_err1")"; rc=$?
if [[ "$rc" -eq 0 ]] && printf '%s' "$out" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["schema"]=="jash.grep.query.v1"
assert [(r["record"],r["count"],r["complete"]) for r in d["items"]]==[("count",1,True)]
assert d["summary"]["complete"] and d["summary"]["predicate"]
'; then ok "--json + -c: observed line count with complete predicate"; else no "--json+-c" "rc=$rc out='$out'"; fi

# ── 12. --json stdin negative predicate is complete after EOF ──────────────
out="$(printf 'x\n' | run 'grep --json NEEDLE' 2>"$T/grep_err2")"; rc=$?
if [[ "$rc" -eq 1 ]] && printf '%s' "$out" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["items"]==[] and not d["summary"]["predicate"]
assert d["summary"]["complete"] and d["summary"]["scan_complete"]
assert d["summary"]["sources_observed"]==1 and d["summary"]["source_bytes"]==2
'; then ok "--json + stdin: complete negative predicate with measured bytes"; else no "--json+stdin" "rc=$rc out='$out'"; fi

# ── 13. --since without -r/dir → rc 2 ────────────────────────────────────────
run 'grep --since 1 NEEDLE json1.txt' >/dev/null 2>"$T/grep_err3"; rc=$?
[[ "$rc" -eq 2 ]] && ok "--since without -r/dir: loud error rc 2" || no "--since no -r" "rc=$rc"

# ── 14. --since with unknown seq → rc 2 (needs a real chain.log to exercise
#        the "no such seq" path rather than the "no chain log at all" path) ──
mkdir -p sincedir
printf 'x\n' > sincedir/f.txt
env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=1 JASH_MEMO=0 JASH_BRIEF=0 \
    JASH_NUDGE=0 JASH_LOOPGUARD=off JASH_SESSION=seedchain "$JASH" -c "true" >/dev/null 2>&1
run 'grep -r --since 999999999 NEEDLE sincedir' >/dev/null 2>"$T/grep_err4"; rc=$?
grep -q "seq" "$T/grep_err4" 2>/dev/null
seqmsg=$?
[[ "$rc" -eq 2 && "$seqmsg" -eq 0 ]] && ok "--since with unknown seq: loud error rc 2, mentions seq" || no "--since unknown seq" "rc=$rc $(cat "$T/grep_err4" 2>/dev/null)"

# ── 15. --since end-to-end: needs a real chain.log (JASH_SIGIL=1) ──────────
run_sigil() { env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=1 \
              JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off JASH_SESSION=since1 "$JASH" -c "$1"; }
mkdir -p sincetree
printf 'old\n' > sincetree/old.txt
touch -t 202001010000 sincetree/old.txt
run_sigil "true" >/dev/null 2>&1
chainfile="$T/.jash/chain.log"
if [[ -f "$chainfile" ]]; then
    lastline="$(tail -1 "$chainfile")"
    seq="$(awk '{print $1}' <<<"$lastline")"
    printf 'new NEEDLE\n' > sincetree/new.txt
    if [[ -n "$seq" ]]; then
        out="$(run_sigil "grep -r --since $seq NEEDLE sincetree" 2>/dev/null)"
        [[ "$out" == *"new.txt"* && "$out" != *"old.txt"* ]] && ok "--since SEQ end-to-end: only post-seq file's matches appear" \
            || no "--since end-to-end" "seq=$seq out='$out'"
        structured="$(run_sigil "grep --json -rc --since $seq NEEDLE sincetree" 2>/dev/null)"; structured_rc=$?
        if [[ "$structured_rc" -eq 0 ]] && printf '%s' "$structured" | python3 -c '
import json,sys
d=json.load(sys.stdin)
assert d["schema"]=="jash.grep.query.v1"
assert [(r["file"],r["count"]) for r in d["items"]]==[("sincetree/new.txt",1)]
assert d["summary"]["since_sequence"]==int(sys.argv[1]) and d["summary"]["complete"]
' "$seq"; then ok "--json --since: checked native lookup accepts actual zero-based Sigil ledger";
        else no "--json --since real ledger" "rc=$structured_rc out='$structured'"; fi
    else
        no "--since end-to-end" "could not parse seq from chain.log line: $lastline"
    fi
else
    no "--since end-to-end" "chain.log not created under JASH_SIGIL=1"
fi

echo "── grep gold (ADR-0044 Arc A2) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
