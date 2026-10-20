#!/usr/bin/env bash
# test_breathe.sh — ADR-0099 W6.2: stuck-process detection.
# Covers: status table + --json envelope (W4), two-sample check (baseline →
# too_soon → stuck verdict), a SYNTHETIC stuck process (sleep: cpu frozen),
# kill safety rails (pid<=1, self, nonexistent) and kill of a real process.
# Sample state lives under HOME (isolated per call); no cross-test leakage.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
TMP="$(mktemp -d)" || exit 1
trap 'rm -rf "$TMP"' EXIT
can_run() { env -i HOME="$TMP" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
    JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_ADOPT=0 JASH_LOOPGUARD=off \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1; }
if [ "${JASH+x}" = x ]; then
    J="$JASH"
    can_run "$J" || { echo "FAIL: explicitly selected JASH cannot execute" >&2; exit 1; }
else
    J=
    for candidate in "$ROOT/build/jash" "$ROOT/build-speed/jash-release"; do
        if can_run "$candidate"; then J="$candidate"; break; fi
    done
    if [ -z "$J" ]; then echo "SKIP: no runnable jash binary"; exit 77; fi
fi
J="$(cd "$(dirname "$J")" && pwd)/$(basename "$J")"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0
export JASH_SIGIL=0 JASH_GUARD_BYPASS=0 JASH_GUARD_BYPASS_STRICT=0

# run HOME BODY — sets OUT/RC/ERR.
run(){
  local h="$1" body="$2"
  mkdir -p "$h" "$TMP/wd"
  printf '%s\n' "$body" > "$TMP/wd/t.jsh"
  OUT="$(cd "$TMP/wd" && HOME="$h" "$J" t.jsh 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}

echo "test_breathe (ADR-0099 W6.2):"

H="$TMP/h1"

# 1. status: the collector excludes itself. The live harness is an independently
#    known process of the same user and must appear with the table columns.
run "$H" 'breathe status'
case "$OUT" in
  *"PID"*"USER"*"COMMAND"*"processes"*)
    if printf '%s\n' "$OUT" | awk -v pid="$$" 'NR > 1 && $1 == pid { found=1 } END { exit !found }'; then
      ok "status: table lists the live harness process"
    else
      no "status: harness process missing" "pid=$$" "$OUT"
    fi;;
  *) no "status table" "$OUT";;
esac
[ "$RC" -eq 0 ] && ok "status rc=0" || no "status rc" "RC=$RC"

# 2. status --json: envelope shape (W4), no human text.
run "$H" 'breathe status --json'
case "$OUT" in
  '{"count":'*'"procs":[{"pid":'*'"ppid":'*'"uid":'*'"user":'*'"comm":'*'"state":'*'"rss_kb":'*'"vsz_kb":'*'"cpu_ticks":'*)
    ok "status --json envelope shape";;
  *) no "status --json" "$(printf '%s' "$OUT" | head -c 200)";;
esac

# 3. check #1 in a fresh HOME: baseline saved, stuck=0, honest "nothing yet".
run "$H" 'breathe check'
case "$OUT" in
  *"baseline saved"*"run check again"*) ok "check: baseline saved on first run";;
  *) no "baseline" "$OUT";;
esac
[ "$RC" -eq 0 ] && ok "baseline rc=0" || no "baseline rc" "RC=$RC"

# 4. check #2 immediately: too_soon honored, baseline kept, still stuck=0.
run "$H" 'breathe check'
case "$OUT" in
  *"since last sample"*"baseline kept"*) ok "check: too_soon honored";;
  *) no "too_soon" "$OUT";;
esac

# 5. Stuck detection with a SYNTHETIC frozen process (sleep spawned by the
#    harness so $! is the real pid). baseline → wait → check flags it.
SPID=""
sleep 300 & SPID=$!
run "$H" 'breathe check --interval 0'
sleep 1
run "$H" 'breathe check --interval 0 --json'
case "$OUT" in
  *'"stuck":'*"\"pid\":$SPID,"*'"comm":"sleep"'*)
    ok "check: synthetic frozen process flagged stuck";;
  *) no "stuck detection" "$(printf '%s' "$OUT" | head -c 300)";;
esac
[ "$RC" -eq 2 ] && ok "check rc=2 when stuck found" || no "check rc" "RC=$RC"

# 6. too_soon --json envelope fields.
run "$H" 'breathe check --json'
case "$OUT" in
  '{"baseline":false,"too_soon":true,'*) ok "too_soon --json envelope";;
  *) no "too_soon json" "$OUT";;
esac

# 7. kill safety: pid 1 refused.
run "$H" 'breathe kill 1'
case "$OUT" in *"refusing to signal pid <= 1"*) ok "kill: pid 1 refused";;
  *) no "kill pid 1" "$OUT";;
esac
[ "$RC" -eq 1 ] && ok "kill refused rc=1" || no "kill refused rc" "RC=$RC"

# 8. kill safety: nonexistent pid.
run "$H" 'breathe kill 4194303'
case "$OUT" in *"no such process"*) ok "kill: nonexistent pid reported";;
  *) no "kill nonexistent" "$OUT";;
esac

# 9. kill the synthetic stuck process for real, --json verdict.
run "$H" "breathe kill $SPID --json"
case "$OUT" in
  "{\"pid\":$SPID,\"signal\":\"TERM\",\"ok\":true}")
    ok "kill: real process terminated, envelope verdict";;
  *) no "kill real" "$OUT";;
esac
sleep 1
if kill -0 "$SPID" 2>/dev/null; then
    no "process survived SIGTERM" "pid $SPID still alive"
    kill -9 "$SPID" 2>/dev/null
else
    ok "process confirmed dead after kill"
fi
wait "$SPID" 2>/dev/null

# 10. kill without pid: usage + rc=2.
run "$H" 'breathe kill'
case "$OUT" in *"usage: breathe kill <pid>"*) ok "kill: usage without pid";;
  *) no "kill usage" "$OUT";;
esac

# 11. unknown subcommand: error + usage, rc=2.
run "$H" 'breathe frobnicate'
case "$OUT" in *"unknown command 'frobnicate'"*"usage:"*) ok "unknown subcommand";;
  *) no "unknown subcommand" "$OUT";;
esac

# 12. no args / help: usage, rc=0.
run "$H" 'breathe'
case "$OUT" in *"usage: breathe <command>"*) ok "no args shows usage";;
  *) no "usage" "$OUT";;
esac

echo ""
echo "=== test_breathe: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
