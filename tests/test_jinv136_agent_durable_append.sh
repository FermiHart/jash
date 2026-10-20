#!/usr/bin/env bash
# JINV-136 — completion reutiliza o inode running sem perder exactly-once.
set -euo pipefail

cd "$(dirname "$0")/.." || exit 2
JASH="./build/jash-agentd"
test -x "$JASH" || { echo "SKIP: $JASH ausente"; exit 0; }

TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT INT TERM
mkdir -p "$TMP/home" "$TMP/cwd" "$TMP/sync-home"

# Um efeito nao-idempotente permite provar que um registro parcial jamais
# autoriza uma segunda execucao.
counter="$TMP/counter"
request="{\"id\":\"durable-append\",\"argv\":[\"sh\",\"-c\",\"printf x >> '$counter'\"],\"cwd\":\"$TMP/cwd\",\"caps\":[\"effect:exec\"]}"
first=$(printf '%s\n' "$request" | HOME="$TMP/home" JASH_SIGIL=0 "$JASH")
printf '%s\n' "$first" | grep -E '"id":"durable-append".*"rc":0' >/dev/null
test "$(wc -c < "$counter")" -eq 1

record=$(find "$TMP/home/.jash/agent_requests" -type f -name '*.req')
test -n "$record"
grep -F 'completed-v1' "$record" >/dev/null
cp "$record" "$TMP/valid.req"
IFS= read -r digest < "$record"

assert_indeterminate() {
    local observed
    observed=$(printf '%s\n' "$request" | HOME="$TMP/home" JASH_SIGIL=0 "$JASH")
    printf '%s\n' "$observed" | grep -E \
        '"id":"durable-append".*"outcome":"indeterminate"' >/dev/null
    test "$(wc -c < "$counter")" -eq 1
}

# Simula recuperacao apos write parcial do frame de conclusao. O prefixo
# running continua duravel, portanto a unica resposta segura e indeterminate.
printf '%s\nrunning\ncompleted-v1\n1\n' "$digest" > "$record"
assert_indeterminate

PYTHON_BIN=$(command -v python3 || command -v python)
for corruption in length hash trailing; do
    cp "$TMP/valid.req" "$record"
    "$PYTHON_BIN" - "$record" "$corruption" <<'PY'
from pathlib import Path
import sys

path = Path(sys.argv[1])
kind = sys.argv[2]
data = path.read_text(encoding="utf-8")
parts = data.split("\n", 5)
assert parts[1] == "running" and parts[2] == "completed-v1", parts[:3]
if kind == "length":
    parts[3] = str(int(parts[3]) + 1)
    data = "\n".join(parts)
elif kind == "hash":
    parts[4] = ("0" if parts[4][0] != "0" else "1") + parts[4][1:]
    data = "\n".join(parts)
else:
    data += "X"
path.write_text(data, encoding="utf-8")
PY
    assert_indeterminate
done

# Um frame completo continua produzindo replay byte-identico sem novo efeito.
cp "$TMP/valid.req" "$record"
replay=$(printf '%s\n' "$request" | HOME="$TMP/home" JASH_SIGIL=0 "$JASH")
first_response=$(printf '%s\n' "$first" | grep -F '"id":"durable-append"')
replay_response=$(printf '%s\n' "$replay" | grep -F '"id":"durable-append"')
test "$replay_response" = "$first_response"
test "$(wc -c < "$counter")" -eq 1

# O signer faz um sync de startup. Depois disso, cada request inline nova pode
# usar no maximo cinco syncs: 2 reserva + 2 receipt + 1 completion append.
if command -v strace >/dev/null 2>&1; then
    /usr/bin/env -i HOME="$TMP/sync-home" PATH=/usr/bin:/bin JASH_SIGIL=0 \
        "$JASH" </dev/null >/dev/null
    for i in 1 2 3; do
        printf '%s\n' \
            "{\"id\":\"sync-$i\",\"argv\":[\"echo\",\"ok\"],\"cwd\":\"$TMP/cwd\",\"caps\":[\"effect:read\"]}"
    done | /usr/bin/env -i HOME="$TMP/sync-home" PATH=/usr/bin:/bin \
        JASH_SIGIL=0 strace -f -e trace=fsync,fdatasync,syncfs \
        -o "$TMP/sync.trace" "$JASH" >/dev/null
    syncs=$(grep -Ec 'fsync|fdatasync|syncfs' "$TMP/sync.trace")
    test "$syncs" -eq 16 || {
        echo "sync contract changed: $syncs (expected startup + 5/request)" >&2
        exit 1
    }
fi

echo "PASS: completion append is framed, crash-safe, and uses 5 syncs/request"
