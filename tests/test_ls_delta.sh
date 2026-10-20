#!/usr/bin/env bash
# tests/test_ls_delta.sh — ADR-0044 Wave A1: `ls --delta`, session-aware change listing.
#
# Only-in-jash: bash/GNU ls has no session ledger, so it cannot say what changed
# since the agent last looked. `ls --delta` records a per-(session, dir) snapshot
# and re-lists ONLY the added / removed / modified entries; an unchanged dir costs
# a one-line envelope (the token win). Gold persona off (JASH_GOLD=0) degrades it
# to a plain listing; the default and --json paths stay untouched.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build/jash"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d /tmp/jash_lsdelta.XXXXXX)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/wd"

pass=0; fail=0
ok() { pass=$((pass+1)); echo "  ✓ $1"; }
no() { fail=$((fail+1)); echo "  ✗ $1${2:+  ($2)}"; }

# Agent session; isolate every other stateful feature so only --delta is exercised.
BASE=(env -i HOME="$T" CLAUDECODE=1 CLAUDE_CODE_SESSION_ID="lsd-$$" TERM=dumb
      JASH_BRIEF=0 JASH_SIGIL=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_DRIFT=0
      JASH_NUDGE=0 JASH_PREFETCH=0 JASH_CHANGES=0)
lsd() { (cd "$T/wd" && "${BASE[@]}" "$JASH" -c "$1") 2>/dev/null; }

# ── 1. baseline: first look arms tracking and shows the entries ──────────────
: > "$T/wd/alpha"; : > "$T/wd/beta"; : > "$T/wd/gamma"
out="$(lsd 'ls --delta')"
if grep -q "baseline armed" <<<"$out" && grep -q "3 entries" <<<"$out"; then
    ok "baseline armed with entry count"
else no "baseline armed with entry count" "$out"; fi

# ── 2. unchanged dir → tiny envelope (the token win), NOT a re-listing ───────
out="$(lsd 'ls --delta')"
bytes=$(printf '%s' "$out" | wc -c | tr -d ' ')
if grep -q "no changes since" <<<"$out"; then ok "unchanged → no-changes envelope"
else no "unchanged → no-changes envelope" "$out"; fi
# ≤20 tokens ≈ well under 90 bytes for one line.
if [[ "$bytes" -le 90 ]]; then ok "unchanged envelope is tiny ($bytes bytes ≤ 90)"
else no "unchanged envelope is tiny" "$bytes bytes"; fi

# ── 3. added file surfaces with a + marker ───────────────────────────────────
: > "$T/wd/delta_new"
out="$(lsd 'ls --delta')"
if grep -q "^+ delta_new" <<<"$out" && grep -q "1 change since" <<<"$out"; then
    ok "added file → + marker"
else no "added file → + marker" "$out"; fi

# ── 4. modified file surfaces with ~ and a signed size delta ─────────────────
printf 'hello world\n' >> "$T/wd/alpha"
out="$(lsd 'ls --delta')"
if grep -q "^~ alpha" <<<"$out" && grep -q '+12B' <<<"$out"; then
    ok "modified file → ~ with +size"
else no "modified file → ~ with +size" "$out"; fi

# ── 5. removed file surfaces with a - marker ─────────────────────────────────
rm "$T/wd/beta"
out="$(lsd 'ls --delta')"
if grep -q "^- beta" <<<"$out"; then ok "removed file → - marker"
else no "removed file → - marker" "$out"; fi

# ── 6. sub-second same-size change is still detected (nanosecond mtime) ───────
printf 'abc' > "$T/wd/ss"; lsd 'ls --delta' >/dev/null
printf 'xyz' > "$T/wd/ss"    # same size, same wall-clock second
out="$(lsd 'ls --delta')"
if grep -q "^~ ss" <<<"$out"; then ok "sub-second same-size change detected"
else no "sub-second same-size change detected" "$out"; fi

# ── 7. --delta --json emits change records + a summary object ────────────────
: > "$T/wd/zeta"
out="$(lsd 'ls --delta --json')"
if grep -q '"change":"added"' <<<"$out" && grep -q '"summary"' <<<"$out"; then
    ok "--json emits change records + summary"
else no "--json emits change records + summary" "$out"; fi

# ── 8. JASH_GOLD=0 degrades --delta to a plain listing (no [delta] marker) ────
out="$( (cd "$T/wd" && "${BASE[@]}" JASH_GOLD=0 "$JASH" -c 'ls --delta') 2>/dev/null )"
if ! grep -q '\[delta\]' <<<"$out" && grep -q 'alpha' <<<"$out"; then
    ok "JASH_GOLD=0 degrades to plain listing"
else no "JASH_GOLD=0 degrades to plain listing" "$out"; fi

# ── 9. the default listing is untouched (no [delta] marker leaks in) ─────────
out="$(lsd 'ls')"
if ! grep -q '\[delta\]' <<<"$out"; then ok "default ls has no delta marker"
else no "default ls has no delta marker" "$out"; fi

echo "── ls --delta (ADR-0044 A1) ──"
echo "PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ "$fail" -eq 0 ]]
