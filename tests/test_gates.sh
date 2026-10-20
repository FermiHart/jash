#!/usr/bin/env bash
# tests/test_gates.sh — ADR-0036 W6.4: the CI gate matrix.
#
# Two kinds of gate:
#   HARD  (fails the suite): the -Werror build and benchmark command validity.
#         Thresholds vary with host load; a binary failing to execute does not.
#   SOFT  (advisory, never fails): the bench thresholds. Startup time is
#         CPU-load-sensitive and RSS via /usr/bin/time is inflated under memory
#         pressure, so a hard bench gate produces false failures on a busy
#         machine. It reports vs the ADR pins and flags regressions for a
#         human to reconfirm on a quiet machine.
#
# The full matrix order (cheap→expensive) lives in test_suite.sh:
#   build → A-Z battery → bash-compat → conformance → CODEMAP → this gate.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$PROJECT_ROOT"

# ADR pins (baseline 2026-07-01). Startup is p50; RSS is startup peak.
PIN_STARTUP_MS=21
PIN_RSS_KB=800

fail=0
BUILT_JB=
GATE_BENCH_BIN=
GATE_TMP="$(mktemp -d "${TMPDIR:-/tmp}/jash-gates.XXXXXX")" || exit 2
trap 'rm -rf "$GATE_TMP"' EXIT
# A caller may supply a private external time executable. Do not substitute
# the shell keyword: missing/unusable measurement remains a hard failure.
TIME_BIN="${TIME_BIN:-/usr/bin/time}"

# ── HARD gate: -Werror build ────────────────────────────────────────────────
# Full in CI (JASH_CI=1, dedicated machine). Locally, a clean 220-file -Werror
# build is heavy, so verify the flags are wired AND do an incremental -Werror
# build (catches any warning in already-changed objects); CI does the clean run.
echo "── warnings gate (-Werror) ──"
if ! grep -q "JASH_WERROR" CMakeLists.txt; then
    echo "✗ HARD: -Werror option missing from CMakeLists.txt"; fail=1
elif ! grep -q -- "-Wshadow" CMakeLists.txt; then
    echo "✗ HARD: W6 hardening flags missing from CMakeLists.txt"; fail=1
else
    CC="${CC_OVERRIDE:-clang}"
    GB=build-gate
    if [ "${JASH_CI:-0}" = "1" ]; then
        configured=0
        if env CC="$CC" cmake -S . -B "$GB" -DCMAKE_BUILD_TYPE=Release \
             -DJASH_WERROR=ON >"$GATE_TMP/gate_configure.log" 2>&1; then
            configured=1
        fi
        hard_ok=0
        if [ "$configured" = 1 ] &&
           env CC="$CC" cmake --build "$GB" --target jash --clean-first \
               >"$GATE_TMP/gate_werror.log" 2>&1; then
            hard_ok=1
        fi
        # CMake models bundled .modjash files as generated source-tree outputs,
        # so clean-first removes them even though this command targets only jash.
        # Restore every bundled module before returning to the rest of the suite.
        module_home="$(mktemp -d "$GATE_TMP/home.XXXXXX")"
        if [ "$configured" = 1 ] &&
           ! env HOME="$module_home" CC="$CC" cmake --build "$GB" \
             --target modules >"$GATE_TMP/gate_modules.log" 2>&1; then
            echo "✗ HARD: bundled modules were not restored after clean-first"
            fail=1
        fi
        rm -rf "$module_home"
        if [ "$hard_ok" = 1 ]; then
            BUILT_JB="./$GB/jash"
            echo "✓ HARD: clean -Werror build passes ($(find "$GB" -name '*.c.o' | wc -l | tr -d ' ') objects)"
        else
            echo "✗ HARD: -Werror configure/build FAILED:"
            grep -E "error:|warning:|CMake Error" "$GATE_TMP/gate_configure.log" "$GATE_TMP/gate_werror.log" 2>/dev/null | head -10
            fail=1
        fi
    else
        # local: reconfigure build-speed with the mandatory -Werror flag
        if env CC="$CC" cmake -S . -B build-speed -DCMAKE_BUILD_TYPE=Release -DJASH_WERROR=ON >"$GATE_TMP/gate_configure.log" 2>&1 \
           && env CC="$CC" cmake --build build-speed --target jash >"$GATE_TMP/gate_werror.log" 2>&1; then
            GATE_BENCH_BIN="$(mktemp "$GATE_TMP/benchmark.XXXXXX")"
            if cp ./build-speed/jash "$GATE_BENCH_BIN" && chmod 0700 "$GATE_BENCH_BIN"; then
                BUILT_JB="$GATE_BENCH_BIN"
            else
                echo "✗ HARD: could not preserve the -Werror benchmark artifact"
                fail=1
            fi
            echo "✓ HARD: -Werror build passes (incremental; CI runs clean via JASH_CI=1)"
        else
            echo "✗ HARD: -Werror configure/build FAILED:"
            for log in "$GATE_TMP/gate_configure.log" "$GATE_TMP/gate_werror.log"; do
                if [ -f "$log" ]; then head -40 "$log"; fi
            done
            fail=1
        fi
        env CC="$CC" cmake -S . -B build-speed -DCMAKE_BUILD_TYPE=Release -DJASH_WERROR=ON >/dev/null 2>&1
    fi
fi

# ── SOFT gate: bench thresholds (advisory) ──────────────────────────────────
echo "── bench thresholds (advisory — reconfirm on a quiet machine) ──"
# Prefer the freshly-built binary. `[ -x ]` is NOT a portability check:
# a stale macOS Mach-O in build-speed/ has the exec bit set and passes it,
# then dies with rc 126 on Linux (JINV-119, the stale-binary finding).
JB="${JASH_BIN:-$BUILT_JB}"
if [ -x "$JB" ]; then
    bench_env=(env JASH_SIGIL=0 JASH_NUDGE=0 JASH_BRIEF=0 JASH_LOOPGUARD=off)
    if ! "${bench_env[@]}" "$JB" -c exit >/dev/null 2>&1; then
        echo "✗ HARD: benchmark binary is not runnable: $JB"
        fail=1
        bench_commands_ok=0
    else
        bench_commands_ok=1
    fi
    # startup p50 over 11 runs
    ms() { python3 -c 'import time;print(int(time.time()*1000))'; }
    times=""
    if [ "$bench_commands_ok" = 1 ]; then
        for i in $(seq 1 11); do
            t0=$(ms)
            if ! "${bench_env[@]}" "$JB" -c exit >/dev/null 2>&1; then
                bench_commands_ok=0
                break
            fi
            t1=$(ms)
            times="$times $((t1-t0))"
        done
    fi
    if [ "$bench_commands_ok" = 1 ]; then
        p50=$(python3 -c "import sys,statistics;print(int(statistics.median([int(x) for x in sys.argv[1:]])))" $times)
        rss=""
        if LC_ALL=C "$TIME_BIN" -v true >/dev/null 2>&1; then
            if timed=$(LC_ALL=C "$TIME_BIN" -v "${bench_env[@]}" "$JB" -c exit 2>&1); then
                rss=$(printf '%s\n' "$timed" | awk '/Maximum resident/{print int($NF)}')
            else
                bench_commands_ok=0
            fi
        elif LC_ALL=C "$TIME_BIN" -l true >/dev/null 2>&1; then
            if timed=$(LC_ALL=C "$TIME_BIN" -l "${bench_env[@]}" "$JB" -c exit 2>&1); then
                rss=$(printf '%s\n' "$timed" | awk '/maximum resident/{print int($1/1024)}')
            else
                bench_commands_ok=0
            fi
        fi
    fi
    if [ "$bench_commands_ok" = 1 ] && [[ "$rss" =~ ^[0-9]+$ ]]; then
        st="ok"; [ "$p50" -gt "$PIN_STARTUP_MS" ] && st="OVER pin (load-sensitive)"
        echo "  startup p50: ${p50}ms  (pin ≤ ${PIN_STARTUP_MS}ms) — $st"
        rst="ok"; [ -n "$rss" ] && [ "$rss" -gt "$PIN_RSS_KB" ] && rst="OVER pin — see BUG-065"
        echo "  startup RSS: ${rss}KB  (pin ≤ ${PIN_RSS_KB}KB) — $rst"
        echo "  (advisory: thresholds do not fail; benchmark command failures do)"
    elif [ "$bench_commands_ok" = 1 ]; then
        echo "✗ HARD: benchmark RSS measurement unavailable or non-numeric"
        fail=1
    else
        echo "✗ HARD: a benchmark command failed"
        fail=1
    fi
else
    echo "✗ HARD: no benchmark binary at $JB"
    fail=1
fi

echo "───────────────────────────────────────────────"
if [ "$fail" -eq 0 ]; then echo "  ✅ gate matrix PASSED (hard gates green; bench advisory)"; exit 0
else echo "  ✗ gate matrix FAILED"; exit 1; fi
