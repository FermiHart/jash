#!/usr/bin/env bash
# tests/test_agent_wave.sh — ADR-0026/27/28 agent-experience wave integration suite.
# Runs all feature-level tests and reports a combined result.
set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

SUITE=(
    tests/test_context_ask.sh
    tests/test_state.sh
    tests/test_budget_builtin.sh
    tests/test_json_adapter.sh
    tests/test_speculate.sh
    tests/test_revert.sh
    tests/test_cost_contract.sh
    tests/test_agent_guard.sh
    tests/test_agent_guide.sh
    tests/test_brain_goal.sh
    tests/test_llm_invoice.sh
)

pass=0
fail=0
failed=()

for t in "${SUITE[@]}"; do
    echo "────────────────────────────────────────────────────────────────"
    echo "  running $t"
    echo "────────────────────────────────────────────────────────────────"
    if bash "$t"; then
        pass=$((pass+1))
    else
        fail=$((fail+1))
        failed+=("$t")
    fi
    echo
done

echo "════════════════════════════════════════════════════════════════"
echo "  AGENT WAVE SUITE RESULTS"
echo "════════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: ${#SUITE[@]}"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ agent-experience wave PASSED"
    exit 0
else
    echo "  ✗ agent-experience wave FAILED"
    for f in "${failed[@]}"; do echo "     - $f"; done
    exit 1
fi
