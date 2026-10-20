#!/usr/bin/env bash
# lint-gate-registration — a fixed bug with an unregistered gate is an
# unguarded regression.
#
# Found the hard way (2026-08-04): test_jinv110, test_jinv124 and test_jinv127
# existed, passed, and were never wired into tests/test_suite.sh. Three bugs
# were "fixed" with nothing watching them. Creating a gate and registering a
# gate were two steps; only one of them was habit.
#
# Two rules:
#   HARD  — every tests/test_jinv*.sh MUST appear in tests/test_suite.sh.
#           This is the disciplined family: one JINV = one bug = one gate.
#           Ceiling is zero. No allow-list. No exceptions.
#   DEBT  — the older ad-hoc tests are grandfathered by count only. The number
#           of unregistered non-JINV tests may shrink, never grow.
#
# Usage: bash tools/lint-gate-registration.sh [--check|--list]
set -u

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SUITE="$ROOT/tests/test_suite.sh"
ALLOW="$ROOT/tools/lint-gate-registration.allow"
MODE="${1:---check}"

[ -f "$SUITE" ] || { echo "lint-gate-registration: missing $SUITE" >&2; exit 2; }

# Debt ceiling: legacy unregistered tests, count frozen at discovery time.
DEBT_CEILING=49
[ -f "$ALLOW" ] && DEBT_CEILING="$(sed -n 's/^ceiling=\([0-9]*\).*/\1/p' "$ALLOW" | head -1)"
: "${DEBT_CEILING:=49}"

jinv_missing=()
legacy_missing=()

for f in "$ROOT"/tests/test_*.sh; do
    [ -e "$f" ] || continue
    base="$(basename "$f")"
    # the runner is not one of the runnees; never let it count itself
    [ "$base" = "test_suite.sh" ] && continue
    if grep -qF "$base" "$SUITE"; then continue; fi
    case "$base" in
        test_jinv*) jinv_missing+=("$base") ;;
        *)          legacy_missing+=("$base") ;;
    esac
done

n_jinv=${#jinv_missing[@]}
n_legacy=${#legacy_missing[@]}

if [ "$MODE" = "--list" ]; then
    echo "=== unregistered JINV gates (must be zero) ==="
    [ "$n_jinv" -eq 0 ] && echo "  (none)" || printf '  %s\n' "${jinv_missing[@]}"
    echo "=== unregistered legacy tests (debt, ceiling=$DEBT_CEILING) ==="
    [ "$n_legacy" -eq 0 ] && echo "  (none)" || printf '  %s\n' "${legacy_missing[@]}"
    exit 0
fi

rc=0

if [ "$n_jinv" -gt 0 ]; then
    echo "FAIL: $n_jinv JINV gate(s) exist but are not registered in tests/test_suite.sh:"
    printf '  %s\n' "${jinv_missing[@]}"
    echo
    echo "  A fixed bug with an unregistered gate is an unguarded regression."
    echo "  Add a line to tests/test_suite.sh:"
    echo "      run \"jinvNNN: <what it pins>\" bash tests/${jinv_missing[0]}"
    rc=1
else
    echo "ok    every JINV gate is registered in the master suite"
fi

if [ "$n_legacy" -gt "$DEBT_CEILING" ]; then
    echo "FAIL: unregistered legacy tests grew: $n_legacy > ceiling $DEBT_CEILING"
    echo "  New tests must be registered. Do not raise the ceiling to pass."
    rc=1
elif [ "$n_legacy" -lt "$DEBT_CEILING" ]; then
    echo "ok    legacy debt shrank: $n_legacy < ceiling $DEBT_CEILING"
    echo "      → lower ceiling= in tools/lint-gate-registration.allow to lock the gain"
else
    echo "ok    legacy debt held at ceiling ($n_legacy)"
fi

exit "$rc"
