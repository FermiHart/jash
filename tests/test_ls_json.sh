#!/usr/bin/env bash
# test_ls_json.sh — structured output for ls (ROADMAP Wave 7c).
# `ls --json` emits one JSON object per entry (JSONL). The text path is untouched.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }

TD="$(mktemp -d)"; trap 'rm -rf "$TD"' EXIT
touch "$TD/a.txt" "$TD/.hidden"; mkdir "$TD/sub"
j(){ "$J" -c "ls --json $*" 2>/dev/null; }

echo "== ls --json structured output =="

out="$(j "$TD")"
echo "$out" | grep -q '"name":"a.txt"' && echo "$out" | grep -q '"type":"file"' \
    && ok "lists file with name+type" || no "file record" "$out"
echo "$out" | grep -q '"name":"sub".*"type":"dir"' && ok "dir entry typed as dir" || no "dir type" "$out"

# every line is a well-formed JSON object
bad=0; while IFS= read -r l; do case "$l" in '{'*'}') ;; *) bad=1;; esac; done <<< "$out"
[ "$bad" = 0 ] && ok "every line is a JSON object (JSONL)" || no "jsonl" "$out"

# hidden excluded by default, included with -a
j "$TD" | grep -q '.hidden' && no "hidden default" "leaked" || ok "hidden excluded by default"
j "-a $TD" | grep -q '"name":".hidden"' && ok "-a includes hidden" || no "hidden -a" "$(j -a "$TD")"

# size + mtime are numbers
echo "$out" | grep -qE '"size":[0-9]+,"mode":"[-dlrwxsStT]+","mtime":[0-9]+' \
    && ok "size/mode/mtime well-typed" || no "fields" "$out"

# text path intact (no JSON when --json absent)
"$J" -c "ls $TD" 2>/dev/null | grep -q '^a.txt$' && ok "plain ls text path intact" || no "text path" "$("$J" -c "ls $TD" 2>/dev/null)"

echo "== ls-json: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
