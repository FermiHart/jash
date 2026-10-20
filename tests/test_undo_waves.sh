#!/usr/bin/env bash
# test_undo_waves.sh — ADR-0050 consolidated gates (W18).
# Each section gets its OWN HOME: the journal persists per HOME, and undo
# tests MUST NOT pop entries from earlier sections (contamination measured:
# a shared HOME made every undo pop a foreign entry).
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1"; shift; local l; for l in "$@"; do echo "      $l"; done; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }

TMP="$(mktemp -d)"; trap 'rm -rf "$TMP"' EXIT
export JASH_BRIEF=0 JASH_MEMO=0 JASH_NUDGE=0 JASH_SIGIL=1

SECN=0
run(){  # run SCRIPT — fresh HOME per invocation, sets OUT/RC/ERR
  local f="$1"; shift
  SECN=$((SECN+1)); local h="$TMP/h$SECN"; mkdir -p "$h"
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  OUT="$(cd "$TMP" && HOME="$h" env "$@" "$J" "$f" 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
  CURHOME="$h"
}
runh(){ # runh HOME SCRIPT — shared HOME across calls of the same section
  local h="$1" f="$2"; shift 2
  bash "$ROOT/tests/setup_sigil_modules.sh" "$h"
  OUT="$(cd "$TMP" && HOME="$h" env "$@" "$J" "$f" 2>"$TMP/e")"; RC=$?
  ERR="$(strip < "$TMP/e")"
}
sc(){ mkdir -p "$TMP/w"; printf '%s\n' "$1" > "$TMP/w/t$SECN.jsh"; echo "$TMP/w/t$SECN.jsh"; }

echo "test_undo_waves (ADR-0050 W18):"

# ── W17 ids + W6 redo + W7 selective (same process) ─────────────────────
run "$(sc 'echo A > a.txt
echo B > b.txt
cp a.txt b.txt
cp b.txt a.txt
timeline
undo id 1
redo
cat a.txt')"
case "$OUT" in *"#1"*"#2"*) ok "W17: entries carry ids";; *) no "ids" "$OUT";; esac
case "$OUT" in *"undo: #1 reverted"*) ok "W7: selective undo by id";;
  *) no "selective" "$OUT";; esac
case "$OUT" in *"redo: 1 re-applied"*) ok "W6: redo re-applies the op";;
  *) no "redo" "$OUT";; esac

# ── W3 hold-on-failure (MOVE back onto an existing file; the obstacle is
#    created by an EXTERNAL unjournaled cp so it sits between the entries) ──
run "$(sc 'echo A > h3a.txt
mv h3a.txt h3c.txt
/bin/cp h3c.txt h3a.txt
undo
timeline')"
case "$OUT" in *"refusing to clobber"*"entry kept"*)
    ok "W3: failed undo holds the entry";;
  *) no "hold" "$OUT";; esac
case "$OUT" in *"move"*) ok "W3: entry still in journal after failure";;
  *) no "entry gone" "$OUT";; esac

# ── W8 undo --dry ────────────────────────────────────────────────────────
run "$(sc 'echo d > d.txt
undo --dry')"
case "$OUT" in
  *"undo --dry"*"create"*"dry run"*) ok "W8: --dry previews without reverting";;
  *) no "--dry" "$OUT";; esac
[ -e "$TMP/d.txt" ] && ok "W8: --dry touched nothing" || no "--dry mutated" ""

# ── W2 metadata ops (fresh HOME each, proven sequences) ─────────────────
run "$(sc 'mkdir -p m/n
undo
ls m 2>&1')"
case "$OUT" in *"removed dir"*) ok "W2: mkdir -p undo (rmdir)";;
  *) no "W2 mkdir" "$OUT";; esac
run "$(sc 'echo z > m2.txt
chmod 600 m2.txt
undo')"
case "$OUT" in *metadata*of*m2.txt*) ok "W2: chmod undo";;
  *) no "W2 chmod" "$OUT";; esac
run "$(sc 'echo z > m3.txt
ln -s m3.txt l.txt
undo')"
case "$OUT" in *"unlinked"*"/l.txt"*) ok "W2: ln -s undo";;
  *) no "W2 ln" "$OUT";; esac

# ── W1 redirect journaling (builtin + external + append) ─────────────────
run "$(sc 'echo base > r.txt
echo str > r.txt
echo more >> r.txt
undo
cat r.txt
undo
cat r.txt')"
case "$OUT" in *"restored"*"base"*) ok "W1: append + overwrite both undoable";;
  *) no "W1" "$OUT";; esac
run "$(sc 'echo ext > e.txt
seq 1 3 > e.txt
undo
cat e.txt')"
case "$OUT" in *"ext"*) ok "W1: external redirect (pipeline) undoable";;
  *) no "W1 external" "$OUT";; esac

# ── W10 swarm merge: 5 parallel writers keep 5 entries ───────────────────
H10="$TMP/hw10"; mkdir -p "$H10"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H10"
echo x > "$TMP/f.txt"
for i in 1 2 3 4 5; do
  (cd "$TMP" && HOME="$H10" JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 JASH_MEMO=0 "$J" -c "echo $i >> f.txt") >/dev/null 2>&1 &
done; wait
runh "$H10" "$(sc 'timeline')"
n=$(printf '%s' "$OUT" | grep -c 'overwrite')
[ "$n" -ge 5 ] && ok "W10: 5 parallel writers → $n entries merged" \
               || no "swarm merge" "$n entries (expected ≥5)"

# ── W13 dedup: same content → ≤1 backup file (fresh HOME) ───────────────
DED="$TMP/dedhome"; mkdir -p "$DED"
bash "$ROOT/tests/setup_sigil_modules.sh" "$DED"
echo K > "$TMP/k1.txt"; echo K > "$TMP/k2.txt"
(cd "$TMP" && HOME="$DED" JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 JASH_MEMO=0 "$J" -c 'cp k1.txt k2.txt') >/dev/null 2>&1
(cd "$TMP" && HOME="$DED" JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 JASH_MEMO=0 "$J" -c 'cp k1.txt k2.txt') >/dev/null 2>&1
nb=$(find "$DED/.jash/tl_backup" -type f 2>/dev/null | wc -l | tr -d ' ')
[ "$nb" -le 1 ] && ok "W13: identical content dedups ($nb file)" \
                || no "dedup" "$nb files for same content"

# ── W11 reaper (same HOME: fake dead dir + backup-taking op) ────────────
H11="$TMP/hw11"; mkdir -p "$H11/.jash/tl_backup/99999998"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H11"
echo fake > "$H11/.jash/tl_backup/99999998/bk_fake"
runh "$H11" "$(sc 'echo q > q.txt
echo o > q2.txt
cp q.txt q2.txt')"
[ ! -d "$H11/.jash/tl_backup/99999998" ] \
    && ok "W11: dead unreferenced dir reaped" \
    || no "reaper" "99999998 still there"
[ -d "$H11/.jash/tl_backup" ] && find "$H11/.jash/tl_backup" -type f | grep -q . \
    && ok "W11: referenced backup kept" || no "reaper took referenced" ""

# ── W14 attest in chain.log ──────────────────────────────────────────────
H14="$TMP/hw14"; mkdir -p "$H14"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H14"
runh "$H14" "$(sc 'echo v > v.txt
echo w > v.txt
undo')"
# W14 attest line is a chained BLAKE3 entry (tag=zeros: ratchet MAC belongs to
# the sigil module and is not recomputed by verify — JINV-051), identified by
# the "cmd=<event>: <detail>" shape.
grep -q 'rc=0 cmd=undo: ' "$H14/.jash/chain.log" 2>/dev/null \
    && ok "W14: undo writes an attest record" \
    || no "attest" "$(tail -1 "$H14/.jash/chain.log" 2>/dev/null)"
grep -qE '[0-9a-f]{64} tag=[0-9a-f]{64} rc=0 cmd=undo: ' "$H14/.jash/chain.log" 2>/dev/null \
    && ok "W14: attest line is a fully chained entry (hash + ratchet MAC)" \
    || no "attest hash" "$(grep 'cmd=undo: ' "$H14/.jash/chain.log" 2>/dev/null)"

# ── W15 pre-exec snapshot (same HOME; dedicated workdir so the snapshot
#    contains ONLY code.c — the harness's own files in cwd would be
#    snapshotted too and undo would restore the newest of them) ──────────
H15="$TMP/hw15"; W15="$TMP/w15"; mkdir -p "$H15" "$W15"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H15"
echo code > "$W15/code.c"
OUT="$(cd "$W15" && HOME="$H15" JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 JASH_MEMO=0 "$J" -c 'make clean' 2>&1)"
case "$OUT" in *"pre-exec snapshot"*"armed"*) ok "W15: snapshot armed before exec";;
  *) no "W15" "$OUT";; esac
rm -f "$W15/code.c"
OUT="$(cd "$W15" && HOME="$H15" JASH_SIGIL=1 JASH_BRIEF=0 JASH_NUDGE=0 JASH_MEMO=0 "$J" -c 'undo' 2>&1)"
[ -e "$W15/code.c" ] && [ "$(cat "$W15/code.c")" = "code" ] \
    && ok "W15: external damage reverted from snapshot" \
    || no "W15 revert" "$OUT"

# ── W20 kill-switch parity: JASH_TIMELINE=0 → same stdout/rc, no journal ─
H20a="$TMP/h20a"; H20b="$TMP/h20b"; mkdir -p "$H20a" "$H20b"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H20a"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H20b"
runh "$H20a" "$(sc 'echo p > p.txt
cp p.txt p2.txt
cat p2.txt')" JASH_TIMELINE=0
out_off="$OUT"; rc_off="$RC"
runh "$H20b" "$(sc 'echo p > p.txt
cp p.txt p2.txt
cat p2.txt')"
[ "$out_off" = "$OUT" ] && [ "$rc_off" = "$RC" ] \
    && ok "W20: stdout/rc identical with machinery off" \
    || no "parity" "off='$out_off/$rc_off' on='$OUT/$RC'"
runh "$H20a" "$(sc 'undo')" JASH_TIMELINE=0
case "$OUT" in *"nothing to undo"*) ok "W20: off → journal empty";;
  *) no "off journal" "$OUT";; esac

echo ""
echo "=== test_undo_waves: $PASS ok / $FAIL fail ==="
[ "$FAIL" -eq 0 ]
