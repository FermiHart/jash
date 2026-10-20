#!/usr/bin/env bash
set -euo pipefail

# This gate asserts both color modes explicitly; an inherited sensory setting
# must not turn its forced-color positive control into a false regression.
unset NO_COLOR JASH_SENSORY

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

mkdir -p "$TMP/tree/assets"
touch "$TMP/tree/.env" "$TMP/tree/Makefile" "$TMP/tree/main.c" \
      "$TMP/tree/README.md" "$TMP/tree/blob.bin"

plain="$(JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c "ls --pretty --pretty-limit 2 --all $TMP/tree")"
[[ "$plain" == *"6 entries"* ]]
[[ "$plain" == *"001"* && "$plain" == *"002"* ]]
[[ "$plain" == *"showing 2 of 6"* ]]
[[ "$plain" == *"4 more"* ]]
[[ "$plain" == *"[DIR]"* && "$plain" == *"[CODE]"* ]]
[[ "$plain" != *$'\033['* ]]

all_entries="$(JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c "ls --pretty --pretty-limit 0 --all $TMP/tree")"
[[ "$all_entries" == *"[CONFIG]"* && "$all_entries" == *"Makefile"* ]]

colored="$(JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c "ls --pretty --pretty-limit 2 --all --color=always $TMP/tree")"
[[ "$colored" == *$'\033[38;5;39m'* ]]
[[ "$colored" == *$'\033[38;5;82m'* ]]

sensory="$(NO_COLOR=1 JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c "ls --pretty --pretty-limit 2 --all --color=always $TMP/tree")"
[[ "$sensory" != *$'\033['* ]]
[[ "$sensory" == *"[DIR]"* && "$sensory" == *"[CODE]"* ]]

echo "PASS: pretty ls is indexed, grouped, bounded, and color-coded"
