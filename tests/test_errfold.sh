#!/usr/bin/env bash
# tests/test_errfold.sh — ADR-0038 W4: error folding behavior matrix.
#
# A failing noisy tool's stderr cascade must FOLD for agents: first instance
# per root cause + sibling count + machine summary + recall handle; humans
# (and JASH_ERR_FOLD=0) keep the passthrough+digest behavior.
set -u
PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="${JASH:-$PROJECT_ROOT/build/jash-release}"
[[ -x "$JASH" ]] || JASH="$PROJECT_ROOT/build-speed/jash-release"
if [[ ! -x "$JASH" ]]; then echo "SKIP: no jash binary"; exit 0; fi
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"

T="$(mktemp -d)"
trap 'rm -rf "$T"' EXIT
mkdir -p "$T/home" "$T/bin"
bash "$PROJECT_ROOT/tests/setup_sigil_modules.sh" "$T/home"

# A fake `cc` (recognized noisy tool) that emits a classic cascade:
# 1 root cause repeated across 6 TUs + 1 distinct cause + 3 warnings + noise.
cat > "$T/bin/cc" <<'EOF'
#!/bin/bash
for i in 1 2 3 4 5 6; do
  echo "src/file$i.c:$i:7: error: use of undeclared identifier 'g_missing_$i'" >&2
done
echo "src/other.c:9:1: error: expected ';' after top level declarator" >&2
for i in 1 2 3; do
  echo "src/file$i.c:20:3: warning: unused variable 'tmp$i'" >&2
done
echo "compilation progress noise line" >&2
exit 1
EOF
chmod +x "$T/bin/cc"

pass=0 fail=0
ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

agent_run() { env -i HOME="$T/home" PATH="$T/bin:$PATH" TERM=dumb CLAUDECODE=1 \
              CLAUDE_CODE_SESSION_ID=fold1 JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
              JASH_CHANGES=0 "$JASH" -c "cc dummy.c" 2>&1 >/dev/null; }

out=$(agent_run)

# 1. cascade folded: root cause shown once with +5 count
case "$out" in
  *"+5 more with the same cause"*) ok "cascade folded (+5 same cause)";;
  *) err "cascade fold (got: $(printf '%s' "$out" | head -4 | tr '\n' '|'))";;
esac

# 2. distinct cause stays visible
case "$out" in
  *"expected"*"after top level declarator"*) ok "distinct cause stays visible";;
  *) err "distinct cause missing";;
esac

# 3. machine-readable summary with counts
case "$out" in
  *"[jash err-fold] 2 root causes (7 error lines), 3 warnings"*) ok "summary line with honest counts";;
  *) err "summary (got: $(printf '%s' "$out" | grep err-fold))";;
esac

# 4. full stderr preserved as recall-able overflow
case "$out" in
  *"full stderr: recall "*)
    handle=$(printf '%s' "$out" | grep -oE 'recall [^ ]+-cc-stderr\.txt' | head -1 | cut -d" " -f2)
    if [[ -n "$handle" && -s "$T/home/.jash/overflow/$handle" ]]; then
      lines=$(wc -l < "$T/home/.jash/overflow/$handle" | tr -d ' ')
      [[ "$lines" == "11" ]] && ok "full stderr preserved (11 lines in overflow)" \
                             || err "overflow line count ($lines)"
    else err "overflow file missing"; fi;;
  *) err "no recall handle in summary";;
esac

# 5. raw cascade NOT re-emitted (folded output is short)
nlines=$(printf '%s\n' "$out" | wc -l | tr -d ' ')
if [[ "$nlines" -le 8 ]]; then ok "folded output is compact ($nlines lines vs 11 raw)"
else err "output not compact ($nlines lines)"; fi

# 6. JASH_ERR_FOLD=0 -> passthrough (all 6 identical-cause lines visible)
out=$(env -i HOME="$T/home" PATH="$T/bin:$PATH" TERM=dumb CLAUDECODE=1 \
      CLAUDE_CODE_SESSION_ID=fold1 JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 \
      JASH_CHANGES=0 JASH_ERR_FOLD=0 "$JASH" -c "cc dummy.c" 2>&1 >/dev/null)
cnt=$(printf '%s\n' "$out" | grep -c "use of undeclared identifier" )
[[ "$cnt" -ge 6 ]] && ok "JASH_ERR_FOLD=0: passthrough ($cnt raw lines)" \
                   || err "fold-off passthrough (only $cnt lines)"

# 7. non-agent session -> passthrough (digest opt-in default stays off)
out=$(env -i HOME="$T/home" PATH="$T/bin:$PATH" TERM=dumb JASH_SIGIL=0 \
      JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" -c "cc dummy.c" 2>&1 >/dev/null)
cnt=$(printf '%s\n' "$out" | grep -c "use of undeclared identifier")
[[ "$cnt" -ge 6 ]] && ok "non-agent: raw stderr untouched" \
                   || err "non-agent behavior changed ($cnt lines)"

echo "═══════════════════════════════════════════════════════════════"
echo "  error folding (ADR-0038 W4) RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
[[ $fail -eq 0 ]] && { echo "  ✅ err-fold tests PASSED"; exit 0; } \
                  || { echo "  ✗ err-fold tests FAILED"; exit 1; }
