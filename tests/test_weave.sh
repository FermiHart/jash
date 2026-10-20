#!/usr/bin/env bash
# test_weave.sh — ADR-0099 W6.1: weave flow integration.
# Covers: task-object parsing (quoted + bare forms), per-task rc/out capture,
# failure aggregation (rc + status field), --json envelope shape (W4),
# malformed input, usage, and BUG-148 discipline (stderr text never leaks
# into the JSON envelope).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0
export JASH_SIGIL=0 JASH_GUARD_BYPASS=0 JASH_GUARD_BYPASS_STRICT=0

# run BODY [VAR=val ...] — sets OUT/RC/ERR. HOME isolated per call.
run(){
  local body="$1"; shift
  mkdir -p "$TMP/h" "$TMP/wd"
  printf '%s\n' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$TMP/h" env "$@" "$J" t.jsh 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}

echo "test_weave (ADR-0099 W6.1):"

# 1. Quoted form: two tasks run, outputs captured in order, rc=0.
run 'weave '"'"'{"a": "echo hello", "b": "echo world"}'"'"''
case "$OUT" in
  *"── a (rc=0) ──"*"hello"*"── b (rc=0) ──"*"world"*"2 tasks, 0 failed"*)
    ok "quoted form: per-task sections + summary";;
  *) no "quoted form" "$OUT";;
esac
[ "$RC" -eq 0 ] && ok "quoted form rc=0" || no "quoted form rc" "RC=$RC"

# 2. Bare form (no quotes): convenience path for simple commands.
run 'weave {count: echo 42, done: echo ok}'
case "$OUT" in
  *"── count (rc=0) ──"*"42"*"── done (rc=0) ──"*"ok"*)
    ok "bare form: unquoted names/commands parse";;
  *) no "bare form" "$OUT";;
esac

# 3. Failure aggregation: one failing task → rc=1, human summary counts it.
run 'weave '"'"'{"good": "true", "bad": "false"}'"'"''
case "$OUT" in
  *"── bad (rc=1) ──"*"1 failed"*) ok "failure aggregation in summary";;
  *) no "failure aggregation" "$OUT";;
esac
[ "$RC" -eq 1 ] && ok "weave rc=1 when a task fails" || no "weave rc" "RC=$RC"

# 4. --json envelope: stable shape, per-task rc/out, status=failed.
run 'weave --json '"'"'{"a": "echo hi", "b": "false"}'"'"''
if printf '%s' "$OUT" | python3 -c '
import json, sys
d=json.load(sys.stdin)
assert (d["status"],d["tasks"],d["failed"]) == ("failed",2,1)
assert [{k:r[k] for k in ("name","cmd","rc","out")} for r in d["results"]] == [
    {"name":"a","cmd":"echo hi","rc":0,"out":"hi\n"},
    {"name":"b","cmd":"false","rc":1,"out":""}]
'; then ok "--json decoded envelope and per-task values"; else no "--json envelope" "$OUT"; fi
[ "$RC" -eq 1 ] && ok "--json rc=1 on failure" || no "--json rc" "RC=$RC"

# 5. --json success status.
run 'weave --json '"'"'{"only": "echo x"}'"'"''
if printf '%s' "$OUT" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert (d["status"],d["tasks"],d["failed"]) == ("completed",1,0)'; then
  ok "--json status=completed"; else no "--json completed" "$OUT"; fi

# 6. JSON escaping: backslashes and newlines inside output are escaped.
run 'weave --json '"'"'{"q": "echo '"'"'"'"'"'"'"'"'a\\b'"'"'"'"'"'"'"'"'"}'"'"''
if printf '%s' "$OUT" | python3 -c 'import json,sys; assert json.load(sys.stdin)["results"][0]["out"] == "a\\b\n"'; then
  ok "--json preserves backslash/newline"; else no "--json escaping" "$OUT"; fi

# 7. Capture merges stdout+stderr into `out` (agents want everything), and
#    nothing of the task leaks to the real stderr.
run 'weave --json '"'"'{"e": "echo boom >&2"}'"'"''
if printf '%s' "$OUT" | python3 -c 'import json,sys; assert json.load(sys.stdin)["results"][0]["out"] == "boom\n"'; then
  ok "task stderr captured into envelope"; else no "stderr capture" "$OUT"; fi
case "$ERR" in *boom*) no "task stderr leaked to real stderr" "$ERR";; *) ok "task stderr not leaked";; esac

# 8. Malformed input: usage + rc=2, nothing executed.
run 'weave {not-an-object'
case "$OUT" in *"malformed task object"*"usage:"*) ok "malformed input: usage";;
  *) no "malformed input" "$OUT";;
esac
[ "$RC" -eq 2 ] && ok "malformed rc=2" || no "malformed rc" "RC=$RC"

# 9. No args: usage, rc=0.
run 'weave'
case "$OUT" in *"usage: weave"*) ok "no args shows usage";; *) no "usage" "$OUT";; esac

# 10. Truncation cap: a task whose output exceeds WEAVE_OUT_CAP is flagged,
#     never floods (ADR-0044 budget discipline).
run 'weave --json '"'"'{"big": "seq 1 5000"}'"'"''
case "$OUT" in *'"truncated":true'*) ok "output cap flagged";;
  *) no "truncation flag" "$(printf '%s' "$OUT" | tail -c 120)";;
esac

echo ""
echo "=== test_weave: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
