#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="${1:-$ROOT/build}"
cmake --build "$BUILD_DIR" --target test_shared_snapshot >/dev/null
"$BUILD_DIR/test_shared_snapshot"
