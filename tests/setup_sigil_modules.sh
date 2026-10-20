#!/usr/bin/env bash
# tests/setup_sigil_modules.sh
# Install jash .modjash modules into a test HOME so that JASH_SIGIL>=1
# can actually load them. Use this in tests that need attestation.
# Usage: source tests/setup_sigil_modules.sh [TARGET_HOME]
set -u
PROJECT_ROOT="${PROJECT_ROOT:-$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)}"
TARGET_HOME="${1:-${HOME:-}}"
if [ -z "$TARGET_HOME" ]; then
    echo "setup_sigil_modules: no HOME provided" >&2
    return 1
fi
mkdir -p "$TARGET_HOME/.jash/modules"
chmod 0700 "$TARGET_HOME/.jash" "$TARGET_HOME/.jash/modules"
for mod in "$PROJECT_ROOT"/modules/*/*.modjash; do
    [ -f "$mod" ] || continue
    dest="$TARGET_HOME/.jash/modules/$(basename "$mod")"
    tmp="$(mktemp "$TARGET_HOME/.jash/modules/.module.XXXXXX")" || return 1
    if cp "$mod" "$tmp" && chmod 0600 "$tmp" && mv -f "$tmp" "$dest"; then
        :
    else
        rm -f "$tmp"
        return 1
    fi
done
