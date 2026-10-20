#!/usr/bin/env bash
# test_expect_waves.sh — ADR-0052 consolidated gates.
# Two-ended per wave: the bug it kills + the behavior it must not break.
# Each section gets its own HOME (isolated chain discipline, ADR-0050).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 JASH_SIGIL=1

run(){  # run CMDLINE — fresh HOME, OUT/RC captured directly (never via pipe)
  SECN=$((SECN+1)); local h="$TMP/h$SECN"; mkdir -p "$h"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  OUT="$(cd "$TMP" && HOME="$h" $J -c "$1" 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"; CURHOME="$h"
}
runh(){ # runh HOME CMDLINE — shared HOME across calls of one section
  local h="$1" c="$2"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  OUT="$(cd "$TMP" && HOME="$h" $J -c "$c" 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}
SECN=0

echo "test_expect_waves (ADR-0052):"

# ── W1+G1b: expect rc cross-call tells the TRUTH, source declared ────────
H1="$TMP/hw1"; mkdir -p "$H1"
runh "$H1" 'false'
runh "$H1" 'expect rc 0'
case "$ERR" in *"FAILED"*"actual: rc 1"*) ok "W1: no vacuous pass (last was rc 1)";;
  *) no "vacuous" "$ERR";; esac
runh "$H1" 'expect rc 1'
case "$OUT" in *"ok"*"rc == 1 (chain, seq #"*) ok "W1: cross-call rc truth from chain";;
  *) no "cross-call rc" "$OUT";; esac
[ "$RC" -eq 0 ] && ok "W1: rc 0 on truth" || no "rc" "RC=$RC"
runh "$H1" 'true'
runh "$H1" 'expect rc 0'
case "$OUT" in *"ok"*) ok "W1: true → rc 0 chained";;
  *) no "true chained" "$OUT";; esac

# ── W1: in-process still wins, source says so ────────────────────────────
run "$(printf 'false\\nexpect rc 1' | head -2 > /dev/null; echo 'false; expect rc 1')"
# use a script for same-process rc
printf 'false\nexpect rc 1\n' > "$TMP/t.jsh"
OUT="$(cd "$TMP" && HOME="$TMP/h$SECN" $J "$TMP/t.jsh" 2>/dev/null)"
case "$OUT" in *"(this process)"*) ok "W1: in-process source declared";;
  *) no "in-process" "$OUT";; esac

# ── W1: no source anywhere → honest refusal, never a vacuous ok ──────────
H1b="$TMP/hw1b"; mkdir -p "$H1b"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H1b"
OUT="$(cd "$TMP" && HOME="$H1b" $J -c 'expect rc 0' 2>&1)"; RC=$?
case "$OUT" in *"no previous command recorded anywhere"*) ok "W1: honest refusal";;
  *) no "refusal" "$OUT";; esac
[ "$RC" -eq 2 ] && ok "W1: refusal rc=2" || no "refusal rc" "RC=$RC"

# ── G1b: ultra-fast records only with explicit opt-out ───────────────────
H1c="$TMP/hw1c"; mkdir -p "$H1c"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H1c"
(cd "$TMP" && HOME="$H1c" $J -c 'false' >/dev/null 2>&1)
[ -f "$H1c/.jash/chain.log" ] && ok "G1b: -c false records the chain" \
    || no "ultra-fast gap" "$(ls "$H1c/.jash" 2>/dev/null)"
H1d="$TMP/hw1d"; mkdir -p "$H1d"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H1d"
out="$(cd "$TMP" && HOME="$H1d" JASH_SIGIL=0 $J -c 'echo quick' 2>/dev/null)"
[ "$out" = "quick" ] && ok "G1b: explicit JASH_SIGIL=0 keeps zero-cost echo" \
    || no "opt-out echo" "$out"

# ── W3: expect cmd — gated execution with captured evidence ─────────────
run 'expect cmd "echo hello" rc 0'
case "$OUT" in *"ok"*'echo hello'*"rc == 0"*) ok "W3: cmd rc pass";;
  *) no "cmd rc" "$OUT";; esac
run 'expect cmd "echo hello" output hell'
case "$OUT" in *"ok"*"hell"*) ok "W3: cmd output pass";;
  *) no "cmd output" "$OUT";; esac
run 'expect cmd "echo hello" output zzz'
case "$ERR" in *"FAILED"*"hello"*) ok "W3: failure carries captured output";;
  *) no "cmd evidence" "$ERR";; esac

# ── W3: quoted multi-word target (the tokenizer bug) ────────────────────
run 'expect cmd "make build" rc 2'
case "$OUT" in *"ok"*'make build'*) ok "W3: quoted multi-word command";;
  *) no "quoted cmd" "$OUT";; esac

# ── W4: failure carries causal evidence from the chain ──────────────────
H4="$TMP/hw4"; mkdir -p "$H4"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H4"
echo x > "$TMP/alvo.txt"
(cd "$TMP" && HOME="$H4" $J -c 'rm alvo.txt' >/dev/null 2>&1)
runh "$H4" 'expect file alvo.txt'
case "$ERR" in *"FAILED"*"last seen seq #"*"rm alvo.txt"*)
    ok "W4: causal evidence attached";;
  *) no "evidence" "$ERR";; esac
run 'expect file nunca-existiu-zzz.txt'
case "$ERR" in *"last seen"*) no "phantom evidence" "$ERR";;
  *) ok "W4: no evidence, no line";; esac

# ── W2: snapshot — record, match, diff, bless ───────────────────────────
H2="$TMP/hw2"; mkdir -p "$H2"
runh "$H2" 'expect snapshot s1 "echo oi"'
case "$OUT" in *"recorded (first run)"*) ok "W2: first run records";;
  *) no "record" "$OUT";; esac
runh "$H2" 'expect snapshot s1 "echo oi"'
case "$OUT" in *"matches golden"*) ok "W2: second run matches";;
  *) no "match" "$OUT";; esac
runh "$H2" 'expect snapshot s1 "echo mudou"'
case "$ERR" in *"FAILED"*"line 1 differs"*) ok "W2: change diffs loudly";;
  *) no "diff" "$ERR";; esac
runh "$H2" 'expect snapshot s1 "echo mudou" --update'
case "$OUT" in *"updated (blessed)"*) ok "W2: --update blesses";;
  *) no "bless" "$OUT";; esac
runh "$H2" 'expect snapshot s1 "echo mudou"'
case "$OUT" in *"matches golden"*) ok "W2: blessed golden matches";;
  *) no "post-bless" "$OUT";; esac

# ── W5: --until holds when the world arrives, honest timeout ────────────
H5="$TMP/hw5"; mkdir -p "$H5" "$TMP/w5"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H5"
(sleep 1 && echo x > "$TMP/w5/aparece.txt") &
OUT="$(cd "$TMP/w5" && HOME="$H5" $J -c 'expect --until 3 file aparece.txt' 2>/dev/null)"; RC=$?
case "$OUT" in *"ok"*"attempt"*) ok "W5: holds when world arrives";;
  *) no "until" "$OUT";; esac
[ "$RC" -eq 0 ] && ok "W5: rc 0 when held" || no "until rc" "RC=$RC"
OUT="$(cd "$TMP/w5" && HOME="$H5" $J -c 'expect --until 1 file nunca-z.txt' 2>&1)"; RC=$?
case "$OUT" in *"timed out after 1s"*) ok "W5: honest timeout";;
  *) no "timeout" "$OUT";; esac
[ "$RC" -eq 1 ] && ok "W5: rc 1 on timeout" || no "timeout rc" "RC=$RC"

# ── W6: link predicates ──────────────────────────────────────────────────
echo x > "$TMP/alvo.txt"
ln -s "$TMP/alvo.txt" "$TMP/bom.txt" 2>/dev/null
ln -s "$TMP/inexistente-z.txt" "$TMP/ruim.txt" 2>/dev/null
run 'expect link bom.txt'
case "$OUT" in *"ok"*"valid symlink"*) ok "W6: valid link";;
  *) no "link" "$OUT";; esac
run 'expect link ruim.txt'
case "$ERR" in *"FAILED"*"broken"*) ok "W6: broken detected";;
  *) no "broken detect" "$ERR";; esac
run 'expect broken ruim.txt'
case "$OUT" in *"ok"*"broken symlink"*) ok "W6: broken affirmed";;
  *) no "broken affirm" "$OUT";; esac

# ── holding end: the 10 classic predicates keep working ──────────────────
echo x > "$TMP/f.txt" && mkdir -p "$TMP/d"
run 'expect file f.txt'
case "$OUT" in *"ok"*) ok "classic: file";; *) no "file" "$OUT";; esac
run 'expect not file nope-zzz.txt'
case "$OUT" in *"ok"*) ok "classic: not file";; *) no "not file" "$OUT";; esac
run 'expect dir d'
case "$OUT" in *"ok"*) ok "classic: dir";; *) no "dir" "$OUT";; esac
run 'expect contains f.txt x'
case "$OUT" in *"ok"*) ok "classic: contains";; *) no "contains" "$OUT";; esac
run 'expect dir "d"'
case "$OUT" in *"ok"*) ok "classic: quoted path";; *) no "quoted" "$OUT";; esac

echo ""
echo "=== test_expect_waves: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
