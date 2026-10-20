#!/usr/bin/env bash
# JINV-135 — argv estruturado read-only nao pode varrer a arvore inteira.
set -euo pipefail

cd "$(dirname "$0")/.." || exit 2
JASH="./build/jash-agentd"
test -x "$JASH" || { echo "SKIP: $JASH ausente"; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP/home" "$TMP/home-trace" "$TMP/home-mutate" "$TMP/tree"

for i in {1..300}; do
    touch "$TMP/tree/f$i"
done

request="{\"id\":\"read-fastpath\",\"argv\":[\"echo\",\"ok\"],\"cwd\":\"$TMP/tree\",\"caps\":[\"effect:read\"]}"
response=$(printf '%s\n' "$request" | HOME="$TMP/home" JASH_SIGIL=0 "$JASH")
printf '%s\n' "$response" > "$TMP/read-response.jsonl"
# Compare the decoded value: JSON permits both \n and \u000a for a newline.
python3 - "$TMP/read-response.jsonl" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream]
matches = [row for row in rows if isinstance(row, dict) and row.get("id") == "read-fastpath"]
if len(matches) != 1:
    raise AssertionError(rows)
row = matches[0]
if (row.get("rc"), row.get("stdout"), row.get("stdout_bytes")) != (0, "ok\n", 3):
    raise AssertionError(row)
PY

# Linux exposes the regression without a timing threshold: before the fix,
# each file causes stat calls because argv[0] becomes the quoted text 'echo'.
if command -v strace >/dev/null 2>&1; then
    printf '%s\n' "$request" \
        | HOME="$TMP/home-trace" JASH_SIGIL=0 strace -f \
            -e trace=newfstatat -o "$TMP/trace" "$JASH" >/dev/null
    calls=$(wc -l < "$TMP/trace")
    if test "$calls" -ge 100; then
        echo "read-only estruturado varreu a arvore: $calls newfstatat" >&2
        exit 1
    fi
fi

# O fast path nao pode desligar a prova de efeito para uma operacao mutante.
victim="$TMP/tree/victim"
mutate="{\"id\":\"mutate-proof\",\"argv\":[\"touch\",\"$victim\"],\"cwd\":\"$TMP/tree\",\"caps\":[\"effect:mutate\"]}"
mutated=$(printf '%s\n' "$mutate" | HOME="$TMP/home-mutate" JASH_SIGIL=0 "$JASH")
printf '%s\n' "$mutated" | grep -E '"id":"mutate-proof".*"rc":0' >/dev/null
printf '%s\n' "$mutated" | grep -F '"files_changed":{"total":1' >/dev/null
printf '%s\n' "$mutated" | grep -F '"created":["victim"]' >/dev/null

# A classificacao e a autoridade do fast path. Builtins que escrevem conforme
# os argumentos devem declarar o maior efeito possivel, nao apenas o default.
digest_victim="$TMP/tree/digest.md"
digest="{\"id\":\"digest-output\",\"argv\":[\"digest\",\"--session\",\"--no-git\",\"--output\",\"$digest_victim\"],\"cwd\":\"$TMP/tree\",\"caps\":[\"effect:read\"]}"
denied=$(printf '%s\n' "$digest" | HOME="$TMP/home-mutate" JASH_SIGIL=0 "$JASH")
printf '%s\n' "$denied" | grep -E \
    '"id":"digest-output".*"rc":125.*"capability denied"' >/dev/null
test ! -e "$digest_victim"

# Regressao concreta: `doctor` criava um probe com fopen("w") e seguia
# symlink sob um TMPDIR controlado pelo request. A cap de leitura nao autoriza
# sequer chegar nessa implementacao.
doctor_victim="$TMP/tree/doctor-victim"
printf 'KEEP\n' > "$doctor_victim"
ln -s "$doctor_victim" "$TMP/tree/.jash_doctor_probe"
doctor="{\"id\":\"doctor-probe\",\"argv\":[\"doctor\"],\"cwd\":\"$TMP/tree\",\"env\":{\"TMPDIR\":\"$TMP/tree\"},\"caps\":[\"effect:read\"]}"
doctor_denied=$(printf '%s\n' "$doctor" | HOME="$TMP/home-mutate" JASH_SIGIL=0 "$JASH")
printf '%s\n' "$doctor_denied" | grep -E \
    '"id":"doctor-probe".*"rc":125.*"capability denied"' >/dev/null
test "$(cat "$doctor_victim")" = "KEEP"
test -L "$TMP/tree/.jash_doctor_probe"

echo "PASS: structured read requests are O(1) and mutations remain measured"
