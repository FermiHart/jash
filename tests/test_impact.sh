#!/usr/bin/env bash
# tests/test_impact.sh — ADR-0038 W5: test impact map behavior matrix.
#
# `impact FILE` must rank test commands learned from the fs journal (W2
# persisted deltas) correlated with session command records; cold files get
# an honest cold message; --run executes the ranked tests.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home/.jash" "$T/home/.jash_sessions" "$T/work/tests"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

NOW=$(date +%s)

# Fabricated history: parser.c changes are followed by test_parser runs
# (with a fail→pass fix cycle); ui.c changes by test_ui runs.
cat > "$T/home/.jash/fsjournal.tsv" <<EOF
$((NOW-3000))	sidA	~	c/parser.c
$((NOW-2900))	sidA	~	c/parser.c
$((NOW-2000))	sidA	~	c/ui.c
EOF
cat > "$T/home/.jash_sessions/session_1.jash" <<EOF
$((NOW-2990))|1|500|bash tests/test_parser.sh
$((NOW-2980))|0|500|bash tests/test_parser.sh
$((NOW-2895))|0|400|bash tests/test_parser.sh
$((NOW-1995))|0|300|bash tests/test_ui.sh
$((NOW-100))|0|200|echo not-a-test
EOF

# A runnable fake test for --run
printf '#!/bin/bash\necho PARSER-TEST-RAN\nexit 0\n' > "$T/work/tests/test_parser.sh"
chmod +x "$T/work/tests/test_parser.sh"

runj() { ( cd "$T/work" && env -i HOME="$T/home" PATH="$PATH" TERM=dumb \
           JASH_SIGIL=0 JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "$1" 2>&1 ); }

# 1. ranking: parser.c -> test_parser first, with fix-cycle boost
out=$(runj "impact c/parser.c")
case "$out" in
  *"1. "*"tests/test_parser.sh"*) ok "parser.c ranks test_parser first";;
  *) err "ranking (got '$out')";;
esac
case "$out" in
  *"learned from 2 change records"*) ok "honest change-record count";;
  *) err "record count (got '$out')";;
esac

# 2. ui.c -> test_ui ranked, test_parser not above it
out=$(runj "impact c/ui.c")
case "$out" in
  *"1. "*"tests/test_ui.sh"*) ok "ui.c ranks test_ui first";;
  *) err "ui ranking (got '$out')";;
esac

# 3. cold file -> honest cold message with full-suite fallback
out=$(runj "impact c/nunca_mudou.c")
case "$out" in
  *"map is COLD"*"test_suite.sh"*) ok "cold file: honest fallback";;
  *) err "cold (got '$out')";;
esac

# 4. --run executes the top test
out=$(runj "impact --run c/parser.c")
case "$out" in
  *PARSER-TEST-RAN*) ok "--run executes ranked test";;
  *) err "--run (got '$out')";;
esac

# 5. suffix matching: query by basename path fragment
out=$(runj "impact parser.c")
case "$out" in
  *"tests/test_parser.sh"*) ok "suffix path matching works";;
  *) err "suffix match (got '$out')";;
esac

# 6. no args -> usage
out=$(runj "impact")
case "$out" in
  *usage:*) ok "usage on empty args";;
  *) err "usage (got '$out')";;
esac

echo "═══════════════════════════════════════════════════════════════"
echo "  impact map (ADR-0038 W5) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ impact tests PASSED"; exit 0; } \
                  || { echo "  ✗ impact tests FAILED"; exit 1; }
