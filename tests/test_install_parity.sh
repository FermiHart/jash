#!/usr/bin/env bash
# test_install_parity.sh — ADR-0041 launch gate: the binary agents actually get
# (/usr/local/bin/jash) must be byte-identical to the gated tree build AND
# behaviorally identical (stdout/rc) on a parser/guard/adoption probe, at
# adopt level 0 and 2. SKIPs green when no installed jash exists (dev machine
# without install). Probe runs via SCRIPT FILE per the golden rule, never
# `jash -c` under a jash parent. Override target: JASH_INSTALLED=/path/jash.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# Same probe as tools/install_verify.sh: `[ -x ]` passes on a stale macOS
# Mach-O left in build-speed/, and this gate would then compare the correctly
# installed binary against one the kernel refuses to run (rc 126) and report
# DRIFT (JINV-119).
runs() { [ -x "$1" ] && "$1" -c 'exit 0' >/dev/null 2>&1; }
TREE="$ROOT/build-speed/jash"
runs "$TREE" || TREE="$ROOT/build/jash"
INST="${JASH_INSTALLED:-/usr/local/bin/jash}"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }

echo "test_install_parity (ADR-0041):"
if [ ! -x "$TREE" ]; then echo "  ✗ no tree binary — build first"; exit 1; fi
if [ ! -x "$INST" ]; then
  echo "  - SKIP: no installed jash at $INST (tools/install_verify.sh installs+verifies)"
  exit 0
fi

# 1. Byte identity — the install is exactly the build the gates ran against.
if cmp -s "$INST" "$TREE"; then
  ok "byte-identical: installed == tree"
else
  no "byte DRIFT: installed != tree" \
     "installed: $INST" "tree:      $TREE" \
     "re-run tools/install_verify.sh after gating the tree build"
fi

# 2. Behavioral parity: same probe, isolated HOME per run, stdout+rc must be
#    byte-identical VIA-INSTALLED vs VIA-TREE (stderr carries machine-local
#    paths/hints and is not compared; A/B harness owns stderr discipline).
TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
cat > "$TMP/probe.jsh" <<'EOF'
echo one
x=$(printf 'a%sb' _)
echo "cmdsub:[$x]"
echo 'sq '"dq:$x"
echo $(( 6 * 7 ))
case t in t) echo case-ok;; *) echo case-bad;; esac
printf '1\n2\n' | while read -r l; do echo "r:$l"; done
cat <<HERE
here:$x
HERE
f(){ echo "fn:$1"; }
f arg
echo data > tfile
cat tfile
rm tfile
[ -e tfile ] || echo gone
false; echo "rc1:$?"
(exit 3); echo "rc3:$?"
echo done
EOF

probe(){ # BIN LEVEL — prints probe stdout with the exit code folded in as a
  # trailing "rc:N" line, so a single string compare covers stdout AND rc
  # (probe runs in a $(...) subshell; a global would not survive it).
  local bin="$1" lvl="$2" h wd rc
  h="$(mktemp -d "$TMP/h.XXXXXX")"; wd="$(mktemp -d "$TMP/wd.XXXXXX")"
  cp "$TMP/probe.jsh" "$wd/"
  ( cd "$wd" && HOME="$h" JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 \
      JASH_ADOPT="$lvl" "$bin" probe.jsh 2>/dev/null )
  rc=$?
  echo "rc:$rc"
}

for lvl in 0 2; do
  A="$(probe "$INST" "$lvl")"
  B="$(probe "$TREE" "$lvl")"
  if [ "$A" = "$B" ]; then
    ok "behavioral parity @ JASH_ADOPT=$lvl ($(printf '%s\n' "$A" | tail -1))"
  else
    no "behavioral DRIFT @ JASH_ADOPT=$lvl" \
       "installed: $(printf '%s\n' "$A" | tail -4 | tr '\n' ' ')" \
       "tree:      $(printf '%s\n' "$B" | tail -4 | tr '\n' ' ')"
  fi
done

echo "install parity: $PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
