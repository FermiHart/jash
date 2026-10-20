#!/usr/bin/env bash
# Reachable Git history must use the sovereign contact identity everywhere.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

repo_root="$(git rev-parse --show-toplevel 2>/dev/null || true)"
if test "$repo_root" != "$ROOT"; then
    echo "SKIP: identity history belongs to the canonical Jash repository"
    exit 0
fi

snapshot="$(mktemp)"
trap 'rm -f "$snapshot"' EXIT INT TERM
git fast-export --all --signed-tags=strip > "$snapshot"

# Split the retired address so the gate itself does not preserve it in a blob.
retired='fernandes@garantia''investimentos.com.br'
current='contact@fermihart.com'

if LC_ALL=C grep -aF "$retired" "$snapshot" >/dev/null; then
    echo "identity history contains the retired email" >&2
    exit 1
fi
if ! LC_ALL=C grep -aF "$current" "$snapshot" >/dev/null; then
    echo "identity history does not contain the current contact email" >&2
    exit 1
fi

echo "PASS: reachable history uses only $current"
