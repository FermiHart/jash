#!/usr/bin/env bash
# tests/test_brain_goal.sh — ADR-0033: Liquid Reasoning Engine + Intention Memory Caching
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$PROJECT_ROOT"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# Use an isolated brain data dir so tests don't pollute the user's model.
export HOME="$TMP"
# Explicit learning below must be the only training source. The final hook
# cases enable JASH_BRAIN=1 for their own invocations.
export JASH_BRAIN=0
mkdir -p "$HOME/.jash"
chmod 0700 "$HOME/.jash"

# ── brain: status baseline ───────────────────────────────────────────────────
out="$TMP/status.out"
$JASH -c 'brain status' > "$out" 2>&1
if grep -q "hidden:" "$out" && grep -q "examples:.*0" "$out"; then
    ok "brain status shows fresh model"
else
    err "brain status baseline failed"
    cat "$out"
fi

# ── brain: intent classification ─────────────────────────────────────────────
out="$TMP/intent.out"
$JASH -c 'brain intent "fix the cd tab completion bug"' > "$out" 2>&1
if grep -q "fix" "$out"; then
    ok "brain intent classifies fix tasks"
else
    err "brain intent classification failed"
    cat "$out"
fi

$JASH -c 'brain intent "build the project with cmake"' > "$out" 2>&1
if grep -q "build" "$out"; then
    ok "brain intent classifies build tasks"
else
    err "brain intent build classification failed"
    cat "$out"
fi

# ── brain: online learning improves intent confidence ────────────────────────
# The production seed includes clock and PID, so no contract promises an
# absolute 50% confidence after exactly 15 observations. Observe the same
# initialized model before and after training in one process instead. The
# learned build probability must exceed even the strongest untrained class,
# while the persisted model must record exactly the requested observations.
before="$TMP/suggest_before.out"
out="$TMP/suggest.out"
training_status="$TMP/training_status.out"
training_script="$TMP/train.jsh"
{
    printf '%s\n' 'brain reset >/dev/null 2>&1 || exit 1'
    printf '%s\n' 'brain suggest cmake > "$JASH_BRAIN_BEFORE" || exit 1'
    for i in $(seq 1 15); do
        printf '%s\n' 'brain learn cmake rc=0 next=ctest intent=build reward=1 >/dev/null 2>&1 || exit 1'
    done
    printf '%s\n' 'brain suggest cmake > "$JASH_BRAIN_AFTER" || exit 1'
    printf '%s\n' 'brain status > "$JASH_BRAIN_TRAINING_STATUS" || exit 1'
} > "$training_script"
JASH_BRAIN_BEFORE="$before" JASH_BRAIN_AFTER="$out" \
    JASH_BRAIN_TRAINING_STATUS="$training_status" \
    "$JASH" "$training_script" > "$TMP/training.log" 2>&1
training_rc=$?

brain_learning_observed() {
    awk '
        FILENAME == ARGV[1] && $1 == "intent:" {
            before_count++; before = $3; gsub(/[()%]/, "", before)
        }
        FILENAME == ARGV[2] && $1 == "intent:" {
            after_count++; label = $2; after = $3; gsub(/[()%]/, "", after)
        }
        FILENAME == ARGV[3] && $1 == "examples:" {
            example_count++; examples = $2
        }
        END {
            valid = before_count == 1 && after_count == 1 && example_count == 1 &&
                    before ~ /^[0-9]+([.][0-9]+)?$/ && after ~ /^[0-9]+([.][0-9]+)?$/ &&
                    (before + 0) >= 0 && (after + 0) <= 100 && label == "build" &&
                    (after + 0) > (before + 0) && examples ~ /^15$/
            exit !valid
        }
    ' "$1" "$2" "$3"
}

if [ "$training_rc" = 0 ] && brain_learning_observed "$before" "$out" "$training_status"; then
    ok "brain learns build intent with increased confidence in the same model"
else
    err "brain suggest did not learn build intent"
    cat "$before" "$training_status" "$TMP/training.log"
    cat "$out"
fi

# Negative controls protect the causal oracle: an unchanged prediction and a
# confident prediction of a different label must both be rejected.
unchanged="$TMP/suggest_unchanged.out"
cp "$before" "$unchanged"
if brain_learning_observed "$before" "$unchanged" "$training_status"; then
    err "learning oracle accepted an unchanged model prediction"
else
    ok "learning oracle rejects an unchanged model prediction"
fi
wrong_label="$TMP/suggest_wrong_label.out"
sed 's/^\([[:space:]]*intent:[[:space:]]*\)build /\1review /' "$out" > "$wrong_label"
if brain_learning_observed "$before" "$wrong_label" "$training_status"; then
    err "learning oracle accepted the wrong learned intent"
else
    ok "learning oracle rejects the wrong learned intent"
fi

# ── brain: next-command prediction with human-readable label ─────────────────
if grep -q "ctest" "$out"; then
    ok "brain suggest predicts next command 'ctest'"
else
    err "brain suggest did not predict 'ctest'"
    cat "$out"
fi

# ── brain: history priming ───────────────────────────────────────────────────
out2="$TMP/suggest_after.out"
$JASH -c 'brain suggest --after "cmake" ""' > "$out2" 2>&1
if grep -q "ctest" "$out2"; then
    ok "brain suggest --after primes hidden state"
else
    err "brain suggest --after did not predict ctest"
    cat "$out2"
fi

# ── brain: impact estimation ─────────────────────────────────────────────────
out="$TMP/impact.out"
$JASH -c "brain impact c/readline/complete.c" > "$out" 2>&1
if grep -q "risk:" "$out"; then
    ok "brain impact produces risk estimate"
else
    err "brain impact produced no risk estimate"
    cat "$out"
fi

# ── brain: guard mode toggle ─────────────────────────────────────────────────
out="$TMP/guard.out"
$JASH -c 'brain guard on' > "$out" 2>&1
$JASH -c 'brain guard status' >> "$out" 2>&1
if grep -q "on" "$out"; then
    ok "brain guard toggles and reports status"
else
    err "brain guard toggle failed"
    cat "$out"
fi

# ── goal: full checkpoint lifecycle ──────────────────────────────────────────
out="$TMP/goal.out"
$JASH -c 'goal begin "fix cd tab completion bug"' > "$out" 2>&1
$JASH -c 'goal step inspect completion flow' >> "$out" 2>&1
$JASH -c 'goal verify pty passes' >> "$out" 2>&1
status_out="$TMP/goal_status.out"
$JASH -c 'goal status' > "$status_out" 2>&1
$JASH -c 'goal close' >> "$out" 2>&1

if grep -q "begin" "$out" && grep -q "step" "$out" && grep -q "verify" "$out" && grep -q "closed" "$out"; then
    ok "goal lifecycle: begin/step/verify/close"
else
    err "goal lifecycle failed"
    cat "$out"
fi

if grep -q "active" "$status_out"; then
    ok "goal status reports active goal"
else
    err "goal status did not report active goal"
    cat "$status_out"
fi

# IDs persist across process restarts and never reuse a closed goal number.
second_goal_out="$TMP/goal_second.out"
$JASH -c 'goal begin "second independent goal"' > "$second_goal_out" 2>&1
if grep -q "begin #2:" "$second_goal_out"; then
    ok "goal ID advances after close and process restart"
else
    err "goal ID was reused after close"
    cat "$second_goal_out"
fi
$JASH -c 'goal close' >> "$second_goal_out" 2>&1
goal_logs=$(find "$TMP/.jash/goals" -maxdepth 1 -type f -name 'goal_*.log' | wc -l | tr -d ' ')
if [ "$goal_logs" = 2 ]; then
    ok "each goal has a distinct persistent log"
else
    err "goal logs collided (found $goal_logs)"
fi

# Process-shared lock/atomic reservation: simultaneous begin calls must not
# hand the same identity to two agents.
parallel_home="$TMP/parallel-goals"
mkdir -p "$parallel_home/.jash"
chmod 0700 "$parallel_home"
chmod 0700 "$parallel_home/.jash"
for i in $(seq 1 8); do
    HOME="$parallel_home" "$JASH" -c "goal begin parallel-$i" \
        > "$TMP/parallel-$i.out" 2>&1 &
done
wait
parallel_ids=$(for i in $(seq 1 8); do
    sed -n 's/.*begin #\([0-9][0-9]*\):.*/\1/p' "$TMP/parallel-$i.out"
done)
parallel_unique=$(printf '%s\n' "$parallel_ids" | sort -u | wc -l | tr -d ' ')
parallel_logs=$(find "$parallel_home/.jash/goals" -maxdepth 1 -type f -name 'goal_*.log' | wc -l | tr -d ' ')
if [ "$parallel_unique" = 8 ] && [ "$parallel_logs" = 8 ]; then
    ok "concurrent goal IDs are unique and logs do not collide"
else
    err "concurrent goal IDs collided (unique=$parallel_unique logs=$parallel_logs)"
    cat "$TMP"/parallel-*.out
fi

# ── goal: recall finds closed checkpoints ────────────────────────────────────
out="$TMP/recall.out"
$JASH -c 'goal recall "cd tab"' > "$out" 2>&1
if grep -q "match:" "$out"; then
    ok "goal recall finds past checkpoints"
else
    err "goal recall did not find checkpoints"
    cat "$out"
fi

# ── brain: no catastrophic forgetting across task blocks ─────────────────────
$JASH -c 'brain reset' >/dev/null 2>&1
for i in $(seq 1 25); do
    $JASH -c "brain learn cmake rc=0 next='ctest' intent='build' reward=1" >/dev/null 2>&1
done
for i in $(seq 1 25); do
    $JASH -c "brain learn 'git status' rc=0 next='git diff' intent='explore' reward=1" >/dev/null 2>&1
done
out="$TMP/multi.out"
$JASH -c 'brain suggest cmake' > "$out" 2>&1
# After learning a second task block, cmake should still suggest ctest.
if grep -q "ctest" "$out"; then
    ok "brain retains build sequence after second task block"
else
    err "brain forgot build sequence"
    cat "$out"
fi

# ── brain: automatic hook via JASH_BRAIN ────────────────────────────────────
$JASH -c 'brain reset' >/dev/null 2>&1
for i in $(seq 1 15); do
    JASH_BRAIN=1 $JASH -c 'echo a; echo b' >/dev/null 2>&1
done
out="$TMP/auto.out"
JASH_BRAIN=1 $JASH -c 'brain status' > "$out" 2>&1
if grep -qE "examples:.*[1-9][0-9]" "$out"; then
    ok "JASH_BRAIN=1 hook learns from command sequences"
else
    err "JASH_BRAIN=1 hook did not learn"
    cat "$out"
fi

# ── brain: automatic guard via JASH_BRAIN ────────────────────────────────────
out="$TMP/auto_guard.out"
JASH_BRAIN=1 $JASH -c 'brain guard on; rm -rf /tmp/jash_brain_guard_test_$$' > "$out" 2>&1
if grep -q "unexpected command" "$out"; then
    ok "JASH_BRAIN=1 guard detects anomalous command"
else
    err "JASH_BRAIN=1 guard did not detect anomaly"
    cat "$out"
fi

# ── brain: reset sovereignty ─────────────────────────────────────────────────
$JASH -c 'brain reset' >/dev/null 2>&1
out="$TMP/reset.out"
$JASH -c 'brain status' > "$out" 2>&1
if grep -q "examples:.*0" "$out"; then
    ok "brain reset clears learned state"
else
    err "brain reset did not clear state"
    cat "$out"
fi

# Leave guard off so later suites are not polluted.
$JASH -c 'brain guard off' >/dev/null 2>&1

echo "═══════════════════════════════════════════════════════════════"
echo "  brain/goal RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ brain/goal tests PASSED"
    exit 0
else
    echo "  ✗ brain/goal tests FAILED"
    exit 1
fi
