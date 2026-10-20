#!/usr/bin/env bash
# tests/test_timeout.sh — ADR-0044: `timeout` builtin.
#
# WHY: stock macOS ships NO `timeout` at all (GNU coreutils only) — agents
# reflexively write `timeout 30 cmd` and get "command not found". This gate
# covers exit-code parity with GNU timeout (124/125/126/127/preserve-status),
# process-GROUP signaling (not just the direct child — a wrapped shell's
# grandchildren must die too), -k/--kill-after SIGKILL escalation, and the
# --json gold summary.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash_timeout.XXXXXX")" || exit 2
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
run(){ env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
       JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off "$JASH" -c "$1"; }

cd "$T"

# ── 1. command finishes before timeout: its own exit code passes through ────
run 'timeout 5 true' >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 0 ]] && ok "finishes before timeout: rc 0 passes through" || no "rc 0 passthrough" "rc=$rc"

run 'timeout 5 sh -c "exit 7"' >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 7 ]] && ok "finishes before timeout: rc 7 passes through" || no "rc 7 passthrough" "rc=$rc"

# ── 2. command exceeds timeout → killed, rc 124, well before the full duration ─
t0=$(date +%s)
run 'timeout 1 sleep 5' >/dev/null 2>&1; rc=$?
t1=$(date +%s)
elapsed=$((t1 - t0))
[[ "$rc" -eq 124 && "$elapsed" -lt 4 ]] && ok "exceeds timeout: rc 124, killed promptly (${elapsed}s, not 5s)" \
    || no "timeout kill" "rc=$rc elapsed=${elapsed}s"

# ── 3. --preserve-status: reports the real signaled status, not 124 ─────────
run 'timeout --preserve-status 1 sleep 5' >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 143 ]] && ok "--preserve-status: reports 128+TERM (143), not 124" || no "--preserve-status" "rc=$rc"

# ── 4. -k/--kill-after escalation: TERM-resistant child still dies via KILL ──
t0=$(date +%s)
run 'timeout -k 1 1 sh -c "trap \"\" TERM; sleep 10"' >/dev/null 2>&1; rc=$?
t1=$(date +%s)
elapsed=$((t1 - t0))
[[ "$rc" -eq 124 && "$elapsed" -lt 6 ]] && ok "-k escalation: TERM-ignoring child killed via SIGKILL within grace (${elapsed}s)" \
    || no "-k escalation" "rc=$rc elapsed=${elapsed}s"

# ── 5. process-group signaling: grandchild dies too, not just the direct child ─
run 'timeout 1 sh -c "sleep 100 & wait"' >/dev/null 2>&1 &
bgpid=$!
sleep 3
if pgrep -f "sleep 100" >/dev/null 2>&1; then
    no "process-group kill" "grandchild 'sleep 100' still alive"
else
    ok "process-group kill: grandchild dies too (not just the direct shell child)"
fi
wait "$bgpid" 2>/dev/null

# ── 6. --json: valid JSON, correct shape, both timeout and non-timeout cases ─
if command -v python3 >/dev/null 2>&1; then
    out="$(run 'timeout --json 1 sleep 5' 2>/dev/null)"
    if echo "$out" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert isinstance(d, dict)
assert d["exit_code"] == 124
assert d["timed_out"] is True
assert d["signal_sent"] == "TERM"
assert isinstance(d["duration_ms"], int) and d["duration_ms"] > 0
' 2>"$T/json_err"; then
        ok "--json (timeout case): single object, correct shape"
    else no "--json timeout shape" "$(cat "$T/json_err")"; fi

    out2="$(run 'timeout --json 5 true' 2>/dev/null)"
    if echo "$out2" | python3 -c '
import json, sys
d = json.load(sys.stdin)
assert d["exit_code"] == 0
assert d["timed_out"] is False
assert d["signal_sent"] is None
' 2>"$T/json_err2"; then
        ok "--json (non-timeout case): timed_out false, signal_sent null"
    else no "--json non-timeout shape" "$(cat "$T/json_err2")"; fi
else
    ok "--json validity (python3 absent — skipped)"
fi

# ── 7. --verbose emits a stderr note when a signal is sent, silent otherwise ──
err="$(run 'timeout --verbose 1 sleep 5' 2>&1 >/dev/null)"
[[ "$err" == *"sending signal TERM"* ]] && ok "--verbose: stderr note when a signal is sent" || no "--verbose signal note" "$err"

err2="$(run 'timeout --verbose 5 true' 2>&1 >/dev/null)"
[[ -z "$err2" ]] && ok "--verbose: silent when the command finishes on its own" || no "--verbose silent on success" "$err2"

# ── 8. command not found → rc 127 ────────────────────────────────────────────
run 'timeout 5 this_command_does_not_exist_xyz' >/dev/null 2>&1; rc=$?
[[ "$rc" -eq 127 ]] && ok "command not found: rc 127" || no "command not found" "rc=$rc"

# ── 9. missing duration / missing command / invalid duration → rc 125 ───────
run 'timeout' >/dev/null 2>&1; rc1=$?
run 'timeout 5' >/dev/null 2>&1; rc2=$?
run 'timeout notaduration true' >/dev/null 2>&1; rc3=$?
[[ "$rc1" -eq 125 && "$rc2" -eq 125 && "$rc3" -eq 125 ]] && ok "usage errors (missing duration/command, bad duration): rc 125" \
    || no "usage errors" "rc1=$rc1 rc2=$rc2 rc3=$rc3"

# ── 10. duration suffix + fractional parsing ─────────────────────────────────
t0=$(date +%s)
run 'timeout 1s sleep 5' >/dev/null 2>&1; rc=$?
t1=$(date +%s)
elapsed=$((t1 - t0))
[[ "$rc" -eq 124 && "$elapsed" -lt 4 ]] && ok "duration suffix '1s': times out promptly" || no "duration suffix" "rc=$rc elapsed=${elapsed}s"

t0=$(date +%s)
run 'timeout 0.3 sleep 5' >/dev/null 2>&1; rc=$?
t1=$(date +%s)
elapsed=$((t1 - t0))
[[ "$rc" -eq 124 && "$elapsed" -lt 4 ]] && ok "fractional duration '0.3': times out promptly (not the full 5s)" \
    || no "fractional duration" "rc=$rc elapsed=${elapsed}s"

# ── 11. argv is never permuted: flags after COMMAND belong to the wrapped cmd ─
# (/bin/echo directly, not `sh -c echo`, so this isn't at the mercy of sh's own
# builtin echo -n support varying by implementation.)
out="$(run 'timeout 5 /bin/echo -n hi')"
[[ "$out" == "hi" ]] && ok "argv not permuted: wrapped command's own -n flag passes through untouched" \
    || no "argv permutation" "out='$out'"

echo "── timeout builtin (ADR-0044) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
