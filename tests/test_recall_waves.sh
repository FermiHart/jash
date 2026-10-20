#!/usr/bin/env bash
# test_recall_waves.sh — ADR-0051 consolidated gates (W9).
# Two-ended per wave where applicable: the bug the wave kills, and the
# behavior it must not break. Each section gets its own HOME (isolated
# journal discipline from ADR-0050's gates).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 JASH_SIGIL=1

run(){  # run CMDLINE [VAR=val ...] with fresh HOME — sets OUT/RC/ERR
  # NOTE: inline assignments, never `env` — jash's env builtin treats flags
  # like --json ANYWHERE in argv as its own and prints the env instead of
  # running the command (measured: `env ... -c 'recall --json'` misfires).
  SECN=$((SECN+1)); local h="$TMP/h$SECN"; mkdir -p "$h"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  OUT="$(cd "$TMP" && { HOME="$h"; "$@"; } >/dev/null 2>&1; HOME="$h" $J -c "$1" 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"; CURHOME="$h"
}
SECN=0

echo "test_recall_waves (ADR-0051 W9):"

# ── W3: seq is contiguous and zero-based across calls ──────────────────
H3="$TMP/hw3"; mkdir -p "$H3"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H3"
(cd "$TMP" && HOME="$H3" $J -c 'false' >/dev/null 2>&1)
(cd "$TMP" && HOME="$H3" $J -c 'echo b' >/dev/null 2>&1)
(cd "$TMP" && HOME="$H3" $J -c 'echo c' >/dev/null 2>&1)
seqs=$(awk '{print $1}' "$H3/.jash/chain.log" 2>/dev/null | tr '\n' ' ')
[ "$seqs" = "0 1 2 " ] && ok "W3: seq 0 1 2 across calls (no restart)" \
    || no "seq restart" "got: $seqs"

# ── W1: chain v2 carries cwd/repo/dur/sess, parse-compatible ─────────────
line=$(tail -1 "$H3/.jash/chain.log")
case "$line" in
  *" cwd="*" repo="*" dur="*" sess="*" cmd="*)
    ok "W1: v2 context fields present";;
  *) no "v2 fields" "$line";; esac
case "$line" in
  *" rc=0 "*) ok "W1: rc field still parseable (v1 compat)";;
  *) no "rc compat" "$line";; esac

# ── W2: contextual search ────────────────────────────────────────────────
H2="$TMP/hw2"; mkdir -p "$H2"
git init -q "$H2/repoA"
git init -q "$H2/repoB"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H2"
(cd "$H2/repoA" && HOME="$H2" $J -c 'make build' >/dev/null 2>&1)
(cd "$H2/repoB" && HOME="$H2" $J -c 'npm test' >/dev/null 2>&1)
(cd "$TMP" && HOME="$H2" $J -c 'echo maker' >/dev/null 2>&1)
out="$(cd "$H2/repoA" && HOME="$H2" $J -c 'recall cmds make --cwd repoA' 2>/dev/null)"
case "$out" in *"make build"*) ok "W2: --cwd finds by directory";;
  *) no "--cwd" "$out";; esac
out="$(cd "$H2/repoB" && HOME="$H2" $J -c 'recall cmds npm --repo' 2>/dev/null)"
case "$out" in *"npm test"*) ok "W2: --repo finds by repo";;
  *) no "--repo" "$out";; esac
out="$(cd "$H2/repoA" && HOME="$H2" $J -c 'recall cmds make --word' 2>/dev/null)"
case "$out" in *maker*) no "--word matched 'maker'" "$out";;
  *) ok "W2: --word boundary (no 'maker')";; esac

# ── W2: JSON envelope is stable and has context (needs an entry first) ──
H2j="$TMP/hw2j"; mkdir -p "$H2j"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H2j"
(cd "$TMP" && HOME="$H2j" $J -c 'false' >/dev/null 2>&1)
OUT="$(cd "$TMP" && HOME="$H2j" $J -c 'recall failed --json' 2>/dev/null)"
case "$OUT" in
  '{"seq":'*'"ts":'*'"rc":'*'"tag":'*'"cmd":'*'"cwd":'*'"repo":'*'"dur":'*'"sess":'*)
    ok "W2: JSON envelope stable + context";;
  *) no "json envelope" "$OUT";; esac

# ── W4: active memory fires with proof on the 3rd identical failure ──────
H4="$TMP/hw4"; mkdir -p "$H4"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H4"
(cd "$TMP" && HOME="$H4" $J -c 'make deploy' >/dev/null 2>&1)
(cd "$TMP" && HOME="$H4" $J -c 'make deploy' >/dev/null 2>&1)
OUT="$(cd "$TMP" && HOME="$H4" $J -c 'make deploy' 2>&1)"; RC=$?
case "$OUT" in
  *"[jash recall] this exact command failed 2× before"*"seq #"*)
    ok "W4: active warning with attested proof";;
  *) no "active memory" "$OUT";; esac
OUT="$(cd "$TMP" && HOME="$H4" JASH_RECALL_GUARD=0 $J -c 'make deploy' 2>&1)"
case "$OUT" in *"this exact command failed"*)
    no "kill-switch failed" "$OUT";;
  *) ok "W4: JASH_RECALL_GUARD=0 silences";; esac

# ── W4: no fire without ≥2 identical failures (holding end) ─────────────
run 'echo nunca-rodou'
case "$ERR" in *"[jash recall]"*) no "false positive" "$ERR";;
  *) ok "W4: silent for new commands";; esac

# ── W5: recall why — causal chain ────────────────────────────────────────
H5="$TMP/hw5"; mkdir -p "$H5"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H5"
(cd "$TMP" && HOME="$H5" $J -c 'true' >/dev/null 2>&1)
(cd "$TMP" && HOME="$H5" $J -c 'false' >/dev/null 2>&1)
OUT="$(cd "$TMP" && HOME="$H5" $J -c 'recall why true' 2>/dev/null)"
case "$OUT" in *"never failed"*) ok "W5: no-break case is honest";;
  *) no "why no-break" "$OUT";; esac
H5b="$TMP/hw5b"; mkdir -p "$H5b"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H5b"
(cd "$TMP" && HOME="$H5b" $J -c 'make x' >/dev/null 2>&1)
OUT="$(cd "$TMP" && HOME="$H5b" $J -c 'recall why make' 2>/dev/null)"
case "$OUT" in *"all failed — no successful baseline"*"seq #"*)
    ok "W5: failures without baseline reported (not 'no runs')";;
  *) no "why no-baseline" "$OUT";; esac

# ── W6: each independently verifiable per-repo chain starts at zero ─────
H6="$TMP/hw6"; mkdir -p "$H6"
git init -q "$H6/ra"
git init -q "$H6/rb"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H6"
(cd "$H6/ra" && HOME="$H6" $J -c 'make build' >/dev/null 2>&1)
(cd "$H6/rb" && HOME="$H6" $J -c 'npm test' >/dev/null 2>&1)
# seq captured BEFORE any recall call — a recall call chains itself (correct)
seqs=$(cat "$H6"/.jash/chain-*.log 2>/dev/null | awk '{print $1}' | sort -n | tr '\n' ' ')
[ "$seqs" = "0 0 " ] && ok "W6: each repo chain has its own zero base" \
    || no "repo seq" "got: $seqs"
out="$(cd "$H6/ra" && HOME="$H6" $J -c 'recall cmds --last 5' 2>/dev/null)"
case "$out" in *"make build"*) ok "W6: repoA sees only its memory";;
  *) no "repoA chain" "$out";; esac
case "$out" in *"npm test"*) no "repoA leaked repoB" "$out";;
  *) ok "W6: no cross-repo leak";; esac

# ── W8: truncation is marked, never silent ───────────────────────────────
LONG=$(printf 'x%.0s' $(seq 1 300))
H8="$TMP/hw8"; mkdir -p "$H8"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H8"
(cd "$TMP" && HOME="$H8" $J -c "echo $LONG" >/dev/null 2>&1)
OUT="$(cd "$TMP" && HOME="$H8" $J -c 'recall cmds --last 1' 2>/dev/null)"
case "$OUT" in *"…(+"*" chars)"*) ok "W8: truncation marker present";;
  *) no "truncation marker" "$OUT";; esac

echo ""
echo "=== test_recall_waves: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
