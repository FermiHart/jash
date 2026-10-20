#!/usr/bin/env bash
# tests/asan_test.sh — ADR-0036 W3: full battery + conformance under
# ASan+UBSan, not just a handful of smoke commands. This is the gate that
# would have caught BUG-039 (heap overflow) and BUG-040 (signed overflow UB)
# structurally instead of needing a dedicated offensive-security pass to
# find them by hand.
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
SAN_HOME="$(mktemp -d)"
trap 'rm -rf "$SAN_HOME"' EXIT

# Never let a sanitizer gate mutate or contend on the caller's Sigil chain.
# Compound A-Z cases still traverse jash_state_init()/cleanup() with Sigil off.
export HOME="$SAN_HOME"
export JASH_SIGIL=0
export JASH_MEMO=0

echo "=== Building with ASan+UBSan ==="
mkdir -p "$PROJECT_DIR/build-asan" && cd "$PROJECT_DIR/build-asan"
# -DCMAKE_C_COMPILER=clang: on some machines the first `cc`/`gcc` on PATH
# resolves to a bare-metal cross compiler (no libc) and a fresh `cmake`
# configure picks it by default, failing before it ever gets to jash's own
# source. Pin the compiler the known-good build/ directory actually uses.
cmake -DENABLE_ASAN=ON -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang .. >/dev/null
make -j"$(nproc 2>/dev/null || sysctl -n hw.ncpu)" jash
cd "$PROJECT_DIR"

echo ""
echo "=== Smoke tests ==="
./build-asan/jash -c 'echo hello' 2>&1
./build-asan/jash -c 'echo $((2+3))' 2>&1
./build-asan/jash -c 'exit 0' 2>&1

echo ""
echo "=== A-Z battery under ASan+UBSan ==="
JASH="$PROJECT_DIR/build-asan/jash" bash tests/test_az_battery.sh "$PROJECT_DIR/build-asan/jash"

echo ""
echo "=== Conformance harness under ASan+UBSan ==="
JASH_BIN="$PROJECT_DIR/build-asan/jash" bash tests/conformance/run_conformance.sh

echo ""
echo "=== ASan+UBSan run complete — no sanitizer errors detected ==="
