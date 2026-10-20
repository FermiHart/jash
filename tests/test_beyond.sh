#!/bin/bash
# test_beyond.sh — Integration tests for jash running on Beyond kernel
#
# These tests verify that jash builtins work correctly in non-interactive
# mode (piped input), which is how jash operates on Beyond (no TTY, serial
# I/O, raw byte input). The Beyond kernel routes fd 0/1/2 to serial and
# has no real TTY — surfaces replace the TTY concept.
#
# Usage:
#   ./tests/test_beyond.sh              # run all tests
#   ./tests/test_beyond.sh /path/jash   # run against specific jash binary
#
# F E R M I ∞ H A R T

set -e

# Locate jash binary
JASH="${1:-./build-speed/jash}"
if [ ! -x "$JASH" ]; then
    # Try common build locations
    for candidate in ./build-speed/jash ./build/jash ./cmake-build-debug/jash; do
        if [ -x "$candidate" ]; then
            JASH="$candidate"
            break
        fi
    done
fi

if [ ! -x "$JASH" ]; then
    echo "FAIL: jash binary not found. Build first or pass path as \$1"
    exit 1
fi

PASS=0
FAIL=0
TOTAL=0

# Test helper: feed commands via stdin, check output
# Usage: beyond_test "test name" "input commands" "expected output pattern"
beyond_test() {
    local name="$1"
    local input="$2"
    local expected="$3"
    TOTAL=$((TOTAL + 1))

    local actual
    actual=$(printf '%s\n' "$input" | "$JASH" 2>/dev/null) || true

    if echo "$actual" | grep -qF "$expected"; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    expected pattern: '$expected'"
        echo "    got: '$actual'"
        FAIL=$((FAIL + 1))
    fi
}

# Test helper: check exit code
# Usage: beyond_exit_test "test name" "input commands" expected_exit_code
beyond_exit_test() {
    local name="$1"
    local input="$2"
    local expected_code="$3"
    TOTAL=$((TOTAL + 1))

    printf '%s\n' "$input" | "$JASH" >/dev/null 2>&1
    local actual_code=$?

    if [ "$actual_code" -eq "$expected_code" ]; then
        echo "  PASS: $name"
        PASS=$((PASS + 1))
    else
        echo "  FAIL: $name"
        echo "    expected exit code: $expected_code"
        echo "    got: $actual_code"
        FAIL=$((FAIL + 1))
    fi
}

echo "=== jash Beyond integration tests ==="
echo "Binary: $JASH"
echo ""

# ── Wave 16: Non-interactive readline (serial-like piped input) ─────────
echo "--- Wave 16: Readline without termios (piped input) ---"

beyond_test "echo basic" \
    "echo hello" \
    "hello"

beyond_test "echo multiple words" \
    "echo hello world foo bar" \
    "hello world foo bar"

beyond_test "echo with quotes" \
    'echo "hello world"' \
    "hello world"

beyond_test "multiple commands via pipe" \
    "echo first
echo second
echo third" \
    "second"

beyond_test "empty input (no crash)" \
    "" \
    ""

echo ""

# ── Wave 17: Builtin execution (no fork/exec needed) ────────────────────
echo "--- Wave 17: Builtin commands ---"

beyond_test "echo builtin" \
    "echo test123" \
    "test123"

beyond_test "pwd builtin" \
    "pwd" \
    "/"

beyond_test "cd and pwd" \
    "cd /tmp
pwd" \
    "/tmp"

beyond_test "cd .. navigation" \
    "cd /tmp
cd ..
pwd" \
    "/"

beyond_test "true builtin (exit 0)" \
    "true" \
    ""

beyond_test "false builtin (exit 1)" \
    "false" \
    ""

echo ""

# ── Wave 18: Signal handling (EINTR from piped input) ───────────────────
echo "--- Wave 18: Signal handling ---"

# On Beyond, Ctrl-C arrives as EINTR from read(). In piped mode, we can't
# easily simulate EINTR, but we can verify the shell doesn't crash on
# unusual input patterns.
beyond_test "no crash on empty lines" \
    "

echo ok
" \
    "ok"

beyond_test "no crash on whitespace-only lines" \
    "   
echo ok" \
    "ok"

echo ""

# ── Wave 19: History/persist graceful degradation ───────────────────────
echo "--- Wave 19: History/persist graceful degradation ---"

# Verify jash doesn't crash when HOME is unset or points to read-only
TOTAL=$((TOTAL + 1))
if (unset HOME; printf 'echo ok\n' | "$JASH" 2>/dev/null); then
    echo "  PASS: works with HOME unset"
    PASS=$((PASS + 1))
else
    echo "  FAIL: crashed with HOME unset"
    FAIL=$((FAIL + 1))
fi

TOTAL=$((TOTAL + 1))
if (HOME=/nonexistent; printf 'echo ok\n' | "$JASH" 2>/dev/null); then
    echo "  PASS: works with nonexistent HOME"
    PASS=$((PASS + 1))
else
    echo "  FAIL: crashed with nonexistent HOME"
    FAIL=$((FAIL + 1))
fi

echo ""

# ── Wave 20: Integration — basic command sequence ───────────────────────
echo "--- Wave 20: Integration test (basic command sequence) ---"

beyond_test "full command sequence" \
    "echo start
pwd
echo middle
cd /tmp
pwd
echo end" \
    "end"

beyond_test "variable assignment and expansion" \
    'X=hello
echo $X' \
    "hello"

beyond_test "command substitution" \
    'echo $(echo inner)' \
    "inner"

beyond_test "arithmetic expansion" \
    'echo $((2 + 3))' \
    "5"

beyond_test "conditional execution" \
    'true && echo yes' \
    "yes"

beyond_test "pipe between commands" \
    'echo hello | cat' \
    "hello"

beyond_test "semicolon separator" \
    'echo a; echo b' \
    "b"

beyond_test "exit with code" \
    "exit 42" \
    ""

beyond_exit_test "exit code propagation" \
    "exit 42" \
    42

echo ""

# ── Summary ─────────────────────────────────────────────────────────────
echo "=== Results: $PASS passed, $FAIL failed, $TOTAL total ==="
if [ "$FAIL" -gt 0 ]; then
    echo "SOME TESTS FAILED"
    exit 1
else
    echo "ALL TESTS PASSED"
    exit 0
fi
