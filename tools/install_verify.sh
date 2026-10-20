#!/usr/bin/env bash
# install_verify.sh — ADR-0041 install parity protocol.
# Installs the gated tree build atomically, then PROVES the install:
#   1. byte identity      — cmp installed vs tree (IDENTICAL or fail)
#   2. behavioral parity  — tests/test_install_parity.sh probe, VIA-INSTALLED
#                           vs VIA-TREE stdout/rc byte-identical @ adopt 0 and 2
# Usage:
#   tools/install_verify.sh                 # install to /usr/local/bin/jash + verify
#   tools/install_verify.sh --verify-only   # no write, just prove current install
#   tools/install_verify.sh /other/bin/jash # alternate destination
# Exit nonzero if install or any proof fails. Needs write access to the
# destination (sudo if your /usr/local/bin requires it).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# `[ -x ]` is not enough to pick the binary to INSTALL. A stale macOS Mach-O
# left in build-speed/ carries the exec bit and passes it, and this script would
# then copy it over /usr/local/bin/jash — replacing the working shell with one
# the kernel refuses to run. Probe that the candidate actually EXECUTES on this
# host (JINV-119, the stale-binary finding).
runs() { [ -x "$1" ] && "$1" -c 'exit 0' >/dev/null 2>&1; }
TREE="$ROOT/build-speed/jash"
runs "$TREE" || TREE="$ROOT/build/jash"
if ! runs "$TREE"; then
    echo "install_verify: no runnable jash found (tried build-speed/jash, build/jash)" >&2
    echo "  build one first: cmake --build build --target jash" >&2
    exit 1
fi

MODE=install
if [ "${1:-}" = "--verify-only" ]; then MODE=verify; shift; fi
DEST="${1:-/usr/local/bin/jash}"

echo "=== install_verify (ADR-0041) ==="
if [ ! -x "$TREE" ]; then echo "✗ no tree binary — build first (build-speed/)"; exit 1; fi
echo "tree: $TREE"
echo "dest: $DEST ($MODE)"

if [ "$MODE" = install ]; then
  # Atomic: never leave a half-written shell where agents will exec it.
  if cp "$TREE" "$DEST.new" && chmod 755 "$DEST.new" && mv -f "$DEST.new" "$DEST"; then
    echo "✓ installed atomically"
  else
    rm -f "$DEST.new" 2>/dev/null
    echo "✗ install FAILED — permissions? (sudo tools/install_verify.sh)"
    exit 1
  fi
fi

# Proof is delegated to the launch-gate test: one source of truth, so the CI
# gate (JASH_LAUNCH_GATE=1) enforces exactly what this script verifies.
if JASH_INSTALLED="$DEST" bash "$ROOT/tests/test_install_parity.sh"; then
  echo "=== install parity: PROVEN ==="
else
  echo "=== install parity: FAILED ==="
  exit 1
fi
