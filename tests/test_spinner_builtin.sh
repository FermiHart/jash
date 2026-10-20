#!/usr/bin/env bash
# Exercise the builtin, rather than counting an incidental mention of spinner
# in the AI prompt test as semantic coverage.
set -eu
JASH=${JASH_BIN:-build/jash}
out=$(JASH_SIGIL=0 "$JASH" -c 'spinner off; spinner; spinner toggle; spinner; spinner on; spinner')
out=${out//$'\r'/}
expected=$'[spinner] off\n[spinner] on\n[spinner] on'
[[ "$out" == "$expected" ]] || { builtin printf 'spinner state mismatch: %s\n' "$out" >&2; exit 1; }
JASH_SIGIL=0 "$JASH" -c 'spinner help' >/dev/null
if JASH_SIGIL=0 "$JASH" -c 'spinner unsupported' >/dev/null 2>&1; then
    builtin printf 'spinner accepted an invalid option\n' >&2
    exit 1
fi
builtin printf 'spinner builtin: ok\n'
