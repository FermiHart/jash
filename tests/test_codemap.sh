#!/usr/bin/env bash
# test_codemap.sh — anti-rot guard for CODEMAP.md.
# Regenerates the map from the code and fails if it differs from the committed
# copy. So adding a builtin / module / env-knob without regenerating the map
# breaks the build — the map can never silently go stale.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

CODEMAP_TMP="$(mktemp -d "${TMPDIR:-/tmp}/jash-codemap.XXXXXX")" || exit 2
trap 'rm -rf "$CODEMAP_TMP"' EXIT
tmp="$CODEMAP_TMP/CODEMAP.md"

bash tools/gen_codemap.sh > "$tmp" 2>/dev/null || { echo "✗ generator failed"; exit 1; }

if diff -u CODEMAP.md "$tmp" > "$CODEMAP_TMP/codemap.diff" 2>&1; then
    echo "✓ CODEMAP.md is in sync with the code ($(grep -cE '^\| `' CODEMAP.md) mapped rows)"
    exit 0
fi
echo "✗ CODEMAP.md is STALE — the code changed but the map wasn't regenerated."
echo "  Fix: bash tools/gen_codemap.sh > CODEMAP.md && git add CODEMAP.md"
echo "  Drift:"
sed 's/^/    /' "$CODEMAP_TMP/codemap.diff" | head -40
exit 1
