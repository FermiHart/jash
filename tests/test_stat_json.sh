#!/usr/bin/env bash
# tests/test_stat_json.sh — ADR-0044: `stat --json` portable file-metadata gold.
#
# System stat has no JSON: BSD wants `-f '%z'`, GNU wants `-c '%s'`, the default
# line differs per platform. jash's in-process stat OWNS `stat --json [-L] FILE...`
# emitting one self-describing object per file; every other form falls back to the
# real stat byte-for-byte. This gate asserts the JSON shape + the fallback parity.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d "${TMPDIR:-/tmp}/jash_statjson.XXXXXX")" || exit 2
trap 'rm -rf "$T"' EXIT
# Admission is an actual launch in the private fixture, not an executable bit.
can_run() { env -i HOME="$T" PATH="$PATH" TERM=dumb JASH_SIGIL=0 \
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

pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  ✓ $1"; }
no(){ fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }
# Both sides of the fallback oracle need the same formatting environment.
# env -i otherwise drops the caller's TZ/locale only for jash, causing the
# external stat it launches to use a different timezone from the oracle.
fixture_env(){ env -i HOME="$T" PATH="$PATH" TERM=dumb TZ=UTC0 LC_ALL=C JASH_SIGIL=0 \
       JASH_MEMO=0 JASH_BRIEF=0 JASH_NUDGE=0 JASH_LOOPGUARD=off "$@"; }
run(){ fixture_env "$JASH" -c "$1"; }

printf 'hello\n' > "$T/reg.txt"       # 6 bytes
chmod 0644 "$T/reg.txt"
mkdir "$T/sub"
ln -s reg.txt "$T/link"               # relative symlink -> reg.txt

# ── 1. regular file: type + exact size ───────────────────────────────────────
out="$(run "stat --json $T/reg.txt" 2>/dev/null)"
[[ "$out" == *'"type":"file"'* && "$out" == *'"size":6'* ]] \
    && ok "regular file: type=file, size=6" || no "regular file shape" "$out"

# ── 2. valid JSON (parses cleanly) ───────────────────────────────────────────
if command -v python3 >/dev/null 2>&1; then
    if printf '%s' "$out" | python3 -c 'import json,sys; json.load(sys.stdin)' 2>/dev/null; then
        ok "emits valid parseable JSON"
    else no "JSON did not parse" "$out"; fi
else ok "emits valid JSON (python3 absent — skipped parse)"; fi

# ── 3. directory → type=dir ──────────────────────────────────────────────────
out="$(run "stat --json $T/sub" 2>/dev/null)"
[[ "$out" == *'"type":"dir"'* ]] && ok "directory: type=dir" || no "dir type" "$out"

# ── 4. symlink (lstat default) → type=symlink + target ───────────────────────
out="$(run "stat --json $T/link" 2>/dev/null)"
[[ "$out" == *'"type":"symlink"'* && "$out" == *'"target":"reg.txt"'* ]] \
    && ok "symlink: type=symlink, target=reg.txt" || no "symlink shape" "$out"

# ── 5. -L follows the link → type of the target (file) ───────────────────────
out="$(run "stat --json -L $T/link" 2>/dev/null)"
[[ "$out" == *'"type":"file"'* ]] && ok "-L follows symlink → type=file" || no "-L follow" "$out"

# ── 6. mode + symbolic perms for a 0644 file ─────────────────────────────────
out="$(run "stat --json $T/reg.txt" 2>/dev/null)"
[[ "$out" == *'"mode":"0644"'* && "$out" == *'"perms":"rw-r--r--"'* ]] \
    && ok "mode=0644, perms=rw-r--r--" || no "mode/perms" "$out"

# ── 7. two operands → two objects ────────────────────────────────────────────
n="$(run "stat --json $T/reg.txt $T/sub" 2>/dev/null | grep -o '"name"' | wc -l | tr -d ' ')"
[[ "$n" == "2" ]] && ok "two operands → 2 objects" || no "multi-operand count" "n=$n"

# ── 8. missing file → rc=1, stderr diagnostic, empty array on stdout ─────────
out="$(run "stat --json $T/nope" 2>/dev/null)";
err="$(run "stat --json $T/nope" 2>&1 1>/dev/null)"; rc=$?
[[ "$out" == "[]" && "$rc" == "1" && "$err" == *"No such file"* ]] \
    && ok "missing file: []+rc1+stderr" || no "missing-file handling" "out=$out rc=$rc err=$err"

# ── 9. --json + a real-stat flag must ERROR, not silently drop --json ────────
err="$(run "stat --json -c %s $T/reg.txt" 2>&1 1>/dev/null)"; rc=$?
[[ "$rc" == "1" && "$err" == *"cannot combine"* ]] \
    && ok "--json + -c errors (no silent-wrong)" || no "combine-guard" "rc=$rc err=$err"

# ── 10. missing operand in --json mode → error + rc1 ─────────────────────────
err="$(run "stat --json" 2>&1 1>/dev/null)"; rc=$?
[[ "$rc" == "1" && "$err" == *"missing file operand"* ]] \
    && ok "--json no operand → error" || no "missing-operand" "rc=$rc err=$err"

# ── 11. non-json invocation falls back to real stat, byte-for-byte ───────────
a="$(run "stat $T/reg.txt" 2>&1)"; a_rc=$?
b="$(fixture_env /usr/bin/stat "$T/reg.txt" 2>&1)"; b_rc=$?
[[ "$a_rc" -eq 0 && "$b_rc" -eq 0 && "$a" == "$b" ]] \
    && ok "non-json falls back byte-exact to /usr/bin/stat" \
    || no "fallback parity" "jash(rc=$a_rc)=[$a] sys(rc=$b_rc)=[$b]"

echo "── stat --json (ADR-0044) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
