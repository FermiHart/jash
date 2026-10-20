#!/usr/bin/env bash
# tests/test_serve.sh — ADR-0038 W6: structured exec protocol behavior matrix.
#
# jash --serve: JSONL over stdio, one persistent attested session. The core
# guarantee under test: argv mode is BYTE-EXACT (the BUG-059 class cannot
# exist by construction), state persists across requests, big outputs become
# refs, rc/stdin/env/cwd all honored.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)" || exit 2
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home" "$T/work"
# Admission is an actual launch in the private fixture, not an executable bit.
can_run() { env -i HOME="$T/home" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
    JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_ADOPT=0 JASH_LOOPGUARD=off \
    JASH_RISK_CHECKPOINT=0 "$1" -c 'exit 0' >/dev/null 2>&1; }
if [[ -n "${JASH:-}" ]]; then
    can_run "$JASH" || { echo "FAIL: explicitly selected JASH cannot execute" >&2; exit 1; }
else
    JASH=
    for candidate in "$PROJECT_ROOT/build/jash-release" "$PROJECT_ROOT/build/jash" "$PROJECT_ROOT/build-speed/jash-release"; do
        if can_run "$candidate"; then JASH="$candidate"; break; fi
    done
    if [[ -z "$JASH" ]]; then echo "SKIP: no runnable jash binary"; exit 77; fi
fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# Drive the daemon: feed all requests, capture all responses.
OUT="$T/resp.jsonl"
{
  printf '%s\n' '{"id":"t1","argv":["echo","hello daemon"]}'
  printf '%s\n' '{"id":"t2","argv":["printf","%s","spaces '\''quotes'\'' $novar `backtick` \\backslash"]}'
  printf '%s\n' '{"id":"t3","line":"v=42; echo valor=$v"}'
  printf '%s\n' '{"id":"t4","line":"echo persistiu=$v"}'
  printf '%s\n' '{"id":"t5","argv":["cat"],"stdin":"linha1\nlinha2\n"}'
  printf '%s\n' '{"id":"t6","argv":["false"]}'
  printf '%s\n' "{\"id\":\"t7\",\"line\":\"pwd\",\"cwd\":\"$T/work\"}"
  printf '%s\n' '{"id":"t8","line":"echo E=$SERVE_PROBE","env":{"SERVE_PROBE":"per-req"}}'
  printf '%s\n' '{"id":"t9","line":"echo E2=$SERVE_PROBE"}'
  printf '%s\n' '{"id":"t10","line":"touch novo_arquivo.txt"}'
  printf '%s\n' '{"id":"t11","line":"head -c 100000 /dev/zero | tr \"\\0\" \"x\""}'
  printf '%s\n' 'garbage not json'
} | env -i HOME="$T/home" PATH="$PATH" TERM=dumb JASH_SIGIL=0 JASH_BRIEF=0 \
    JASH_NUDGE=0 "$JASH" --serve 2>/dev/null > "$OUT"
DRC=$?

R() { grep "\"id\":\"$1\"" "$OUT"; }

# JSON permits both \n and \u000a for a newline. Check the captured bytes,
# status and length after decoding instead of requiring one escape spelling.
response_stdout_is() {
  python3 - "$OUT" "$1" "$2" "$3" <<'PY'
import json
import sys

with open(sys.argv[1], encoding="utf-8") as stream:
    rows = [json.loads(line) for line in stream]
matches = [row for row in rows if isinstance(row, dict) and row.get("id") == sys.argv[2]]
if len(matches) != 1:
    raise SystemExit(1)
row = matches[0]
expected = sys.argv[4]
if (row.get("rc") != int(sys.argv[3]) or row.get("stdout") != expected or
        row.get("stdout_bytes") != len(expected.encode("utf-8"))):
    raise SystemExit(1)
PY
}

# 0. daemon exits 0 on clean EOF + hello line present
[[ $DRC -eq 0 ]] && ok "clean EOF exit 0" || err "daemon exit rc=$DRC"
grep -q '"jash_serve":1' "$OUT" && ok "hello line" || err "hello line missing"

# 1. simple argv capture
if response_stdout_is t1 0 $'hello daemon\n'; then ok "argv echo captured";
else err "t1 ($(R t1))"; fi

# 2. THE guarantee: hostile token passes byte-exact, zero expansion
case "$(R t2)" in
  *'spaces '"'"'quotes'"'"' $novar `backtick` \\backslash'*) ok "argv byte-exact (quotes/\$/backtick/backslash)";;
  *) err "t2 ($(R t2))";;
esac

# 3/4. one logical shell: state persists
case "$(R t3)" in *'valor=42'*) ok "line mode + assignment";; *) err "t3";; esac
case "$(R t4)" in *'persistiu=42'*) ok "state persists across requests";; *) err "t4";; esac

# 5. stdin delivery
if response_stdout_is t5 0 $'linha1\nlinha2\n'; then ok "stdin delivered";
else err "t5 ($(R t5))"; fi

# 6. rc propagation
case "$(R t6)" in *'"rc":1'*) ok "rc propagated";; *) err "t6";; esac

# 7. cwd honored (sticky)
case "$(R t7)" in *work*) ok "cwd honored";; *) err "t7 ($(R t7))";; esac

# 8/9. env overlay is per-request (restored after)
case "$(R t8)" in *'E=per-req'*) ok "env overlay applies";; *) err "t8";; esac
case "$(R t9)" in *'E=per-req'*) err "env overlay leaked";; *) ok "env overlay restored";; esac

# 10. files_changed in response
case "$(R t10)" in *'"created":["novo_arquivo.txt"]'*) ok "files_changed in response";;
  *) err "t10 ($(R t10))";; esac

# 11. big output becomes a ref with size + head
case "$(R t11)" in
  *'"stdout_ref":'*'"stdout_bytes":100000'*) ok "big output -> overflow ref";;
  *) err "t11 ($(R t11 | head -c 200))";;
esac

# 12. garbage -> structured error
grep -q '"error":"request must be a JSON object"' "$OUT" \
  && ok "garbage -> structured error" || err "garbage handling"

# 13. protocol purity: every line is a JSON object
bad=$(grep -cv '^{' "$OUT")
[[ "$bad" -eq 0 ]] && ok "protocol stream pure (all lines JSON)" \
                   || err "protocol polluted ($bad non-JSON lines)"

echo "═══════════════════════════════════════════════════════════════"
echo "  serve daemon (ADR-0038 W6) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ serve tests PASSED"; exit 0; } \
                  || { echo "  ✗ serve tests FAILED"; exit 1; }
