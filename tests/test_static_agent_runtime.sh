#!/usr/bin/env bash
# Static agent artifacts must contain their libc instead of trusting the host.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MUSL_CC="$(command -v musl-gcc || true)"
if test -z "$MUSL_CC"; then
    echo "SKIP: static agent closure requires musl-gcc"
    exit 0
fi
READELF="$(command -v readelf || true)"
if test -z "$READELF"; then
    echo "static agent closure requires readelf" >&2
    exit 1
fi

BUILD="${TMPDIR:-/tmp}/jash-static-agent.$$"
BAD_BUILD="$BUILD.bad"
HOME_DIR="$BUILD/home"
trap 'rm -rf "$BUILD" "$BAD_BUILD"' EXIT INT TERM
EPOCH="${SOURCE_DATE_EPOCH:-$(git -C "$ROOT" show -s --format=%ct HEAD)}"
REVISION="${JASH_SOURCE_REVISION:-$(git -C "$ROOT" rev-parse HEAD)}"

if SOURCE_DATE_EPOCH="$EPOCH" cmake -S "$ROOT" -B "$BAD_BUILD" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$(command -v cc)" \
    -DJASH_SOURCE_REVISION="$REVISION" -DJASH_STATIC_AGENT_RUNTIME=ON \
    >/dev/null 2>&1; then
    echo "static agent closure accepted a non-musl compiler" >&2
    exit 1
fi

SOURCE_DATE_EPOCH="$EPOCH" cmake -S "$ROOT" -B "$BUILD" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$MUSL_CC" \
    -DJASH_SOURCE_REVISION="$REVISION" -DJASH_STATIC_AGENT_RUNTIME=ON \
    >/dev/null
SOURCE_DATE_EPOCH="$EPOCH" cmake --build "$BUILD" \
    --target jash-agentd jef_verify -j2 >/dev/null

for artifact in "$BUILD/jash-agentd" "$BUILD/jef_verify"; do
    test -x "$artifact"
    if "$READELF" -l "$artifact" | grep -q 'INTERP'; then
        echo "static artifact has a program interpreter: $artifact" >&2
        exit 1
    fi
    if "$READELF" -d "$artifact" 2>/dev/null | grep -q 'NEEDED'; then
        echo "static artifact has a shared dependency: $artifact" >&2
        exit 1
    fi
done

mkdir -p "$HOME_DIR"
printf 'Needle\n' > "$BUILD/corpus.txt"
REQUEST='{"id":"static","argv":["grep","-i","needle","'"$BUILD/corpus.txt"'"],"caps":["effect:read"]}'
printf '%s\n' "$REQUEST" > "$BUILD/request.json"
printf '%s\n' "$REQUEST" | HOME="$HOME_DIR" "$BUILD/jash-agentd" \
    > "$BUILD/rows.jsonl"

PYTHON_BIN="$(command -v python3 || command -v python)"
META="$($PYTHON_BIN - "$BUILD/rows.jsonl" "$BUILD/response.json" <<'PY'
import json
from pathlib import Path
import sys

lines = Path(sys.argv[1]).read_text(encoding="utf-8").splitlines()
handshake, response = map(json.loads, lines)
assert handshake["profile"] == "agent-strict", handshake
assert response["rc"] == 0 and response["stdout"] == "Needle\n", response
Path(sys.argv[2]).write_text(lines[1] + "\n", encoding="utf-8")
print(handshake["receipts"]["pubkey"], response["receipt_ref"])
PY
)"
PUBKEY="${META%% *}"
RECEIPT_REF="${META#* }"
"$BUILD/jef_verify" runtime-receipt "$PUBKEY" \
    "$BUILD/request.json" "$BUILD/response.json" \
    "$HOME_DIR/.jash/agent_receipts/$RECEIPT_REF" >/dev/null
printf '%s' "$REQUEST" > "$BUILD/request-body.json"
"$BUILD/jef_verify" runtime-receipt "$PUBKEY" \
    "$BUILD/request-body.json" "$BUILD/response.json" \
    "$HOME_DIR/.jash/agent_receipts/$RECEIPT_REF" >/dev/null
JASH_AGENTD="$BUILD/jash-agentd" JEF_VERIFY="$BUILD/jef_verify" \
    bash "$ROOT/tests/test_jinv137_jef_persistent.sh" >/dev/null
printf '%s\n\n' "$REQUEST" > "$BUILD/request-extra-delimiter.json"
if "$BUILD/jef_verify" runtime-receipt "$PUBKEY" \
    "$BUILD/request-extra-delimiter.json" "$BUILD/response.json" \
    "$HOME_DIR/.jash/agent_receipts/$RECEIPT_REF" >/dev/null 2>&1; then
    echo "runtime receipt verifier ignored more than one JSONL delimiter" >&2
    exit 1
fi

echo "PASS: static musl agent runtime and verifier have zero shared dependencies"
