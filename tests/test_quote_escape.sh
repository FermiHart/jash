#!/usr/bin/env bash
# test_quote_escape.sh — backslash escapes inside double quotes passed to an
# EXTERNAL command must match bash (BUG-026). The agent symptom was
# `python3 -c "...\$..."` getting a literal \$ (SyntaxWarning) and quote-splits.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — jash=[$2] bash=[$3]"; }

# Use /usr/bin/printf (external) so we exercise build_argv, not a builtin.
chk(){ # label  cmd
    local j b
    j=$("$J" -c "$2" 2>/dev/null)
    b=$(/bin/bash -c "$2" 2>/dev/null)
    if [ "$j" = "$b" ]; then ok "$1"; else no "$1" "$j" "$b"; fi
}

echo "== escaped quote/dollar in double quotes -> external (BUG-026) =="
chk 'backslash-dollar'            '/usr/bin/printf "[%s]" "a\$b"'
chk 'escaped quote'               '/usr/bin/printf "[%s]" "say \"hi\""'
chk 'quote+dollar (python case)'  '/usr/bin/printf "[%s]" "print(\"v: \$5\")"'
chk 'dollar before escaped quote' '/usr/bin/printf "[%s]" "x\$\""'
chk 'two quoted words w/ spaces'  '/usr/bin/printf "[%s|%s]" "a b" "c d"'
chk 'regex anchor at quote end'   '/usr/bin/printf "[%s]" "re:\"x\$\""'

echo "== quote-escape: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
