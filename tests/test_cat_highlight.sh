#!/usr/bin/env bash
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$ROOT/build/jash}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

source="$TMP/example.c"
printf '#include <stdio.h>\nint main(void) {\n\treturn 0; // done\n}\n' > "$source"
before="$(sha256sum "$source")"

plain="$(JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c "cat --color=never $source")"
[[ "$plain" == *$'\treturn 0; // done'* ]]
[[ "$plain" != *$'\033['* ]]

colored="$(JASH_SIGIL=0 JASH_NUDGE=0 "$JASH" -c "cat --color=always $source")"
[[ "$colored" == *$'\033[38;5;81m'* ]] # C keyword
[[ "$colored" == *$'\033[38;5;114m'* ]] # string/comment color family
[[ "$colored" == *"#include"* && "$colored" == *"return"* ]]
[[ "$(sha256sum "$source")" == "$before" ]]

echo "PASS: cat syntax colors are opt-in and never modify source"
