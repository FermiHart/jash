#!/usr/bin/env bash
# test_world_delta.sh — world-delta: snapshot-diff between -c invocations (ADR-0042 F2).
# Reports created/modified/deleted files across -c calls with attribution.
set -u
set -o pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\[[0-9;]*m//g'; }

have_py=0; command -v python3 >/dev/null 2>&1 && have_py=1
# Does $1 parse as a single JSON object?
is_valid_json(){ [ "$have_py" = 1 ] || return 2; python3 -c "import sys,json; json.loads(sys.argv[1])" "$1" >/dev/null 2>&1; }
# Space-joined list of world_delta.<field> ($2) from envelope $1.
wd_field(){ python3 -c "import sys,json; d=json.loads(sys.argv[1]).get('world_delta',{}); print(' '.join(d.get(sys.argv[2],[])))" "$1" "$2" 2>/dev/null; }
# True if world_delta.attribution is a JSON object (not a bare string).
wd_attr_is_obj(){ python3 -c "import sys,json; a=json.loads(sys.argv[1]).get('world_delta',{}).get('attribution'); sys.exit(0 if isinstance(a,dict) else 1)" "$1" 2>/dev/null; }

fresh_home(){ TH=$(mktemp -d); mkdir -p "$TH/.jash"; bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"; echo "$TH"; }

SID="test_world_$$"

# ---- F2.1: created files appear in world_delta ----
{
  TH=$(fresh_home); WD="$TH/wd"; mkdir -p "$WD"
  # Pre-create file, then first call captures baseline with a.txt.
  # Must launch jash FROM the workspace so getcwd() returns $WD.
  touch "$WD/a.txt"
  (cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID" JASH_AGENT=1 \
    "$J" -c "echo baseline") 2>/dev/null
  # Create a new file between calls
  touch "$WD/b.txt"
  # Second call — b.txt is new vs previous scan
  OUT=$(cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID" JASH_AGENT=1 \
    "$J" -c "echo done" 2>&1 | strip)
  ENV=$(echo "$OUT" | grep jash_agent | tail -1)
  if echo "$ENV" | grep -q 'world_delta'; then
    ok "world_delta appears in envelope"
  else
    no "world_delta in envelope" "no world_delta key"
  fi
  # F2.1b: created array actually names b.txt (not just any world_delta key).
  if [ "$have_py" = 1 ]; then
    if [ "$(wd_field "$ENV" created)" = "b.txt" ]; then
      ok "created array names the new file"
    else
      no "created array" "expected b.txt, got '$(wd_field "$ENV" created)'"
    fi
  fi
}

# ---- F2.2: modified files appear in world_delta.modified ----
# The startup scan runs BEFORE the command body, so the tracked file must
# pre-exist call #1 and be mutated BETWEEN calls to register as modified.
{
  TH=$(fresh_home); WD="$TH/wd2"; mkdir -p "$WD"
  SID2="test_world_mod_$$"
  printf 'v1\n' > "$WD/data.txt"     # exists before call #1's baseline scan
  (cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID2" JASH_AGENT=1 \
    "$J" -c "echo hi") 2>/dev/null
  printf 'v2-longer\n' > "$WD/data.txt"   # mutate between calls (size + mtime change)
  OUT=$(cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID2" JASH_AGENT=1 \
    "$J" -c "echo hi" 2>&1 | strip)
  ENV=$(echo "$OUT" | grep jash_agent | tail -1)
  if [ "$have_py" = 1 ]; then
    if [ "$(wd_field "$ENV" modified)" = "data.txt" ]; then
      ok "modified files tracked in world_delta.modified"
    else
      no "modified tracking" "expected data.txt in world_delta.modified, got '$(wd_field "$ENV" modified)'"
    fi
  else
    # No python: fall back to structural grep on the world_delta object.
    if echo "$ENV" | grep -qE 'world_delta.*"modified":\["data.txt"\]'; then
      ok "modified files tracked in world_delta.modified (grep)"
    else
      no "modified tracking" "no data.txt in world_delta.modified"
    fi
  fi
}

# ---- F2.3: no changes → no world_delta key ----
{
  TH=$(fresh_home); WD="$TH/wd3"; mkdir -p "$WD"
  SID3="test_world_none_$$"
  (cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID3" JASH_AGENT=1 \
    "$J" -c "echo baseline") 2>/dev/null
  OUT=$(cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID3" JASH_AGENT=1 \
    "$J" -c "echo unchanged" 2>&1 | strip)
  if echo "$OUT" | grep -q 'world_delta'; then
    no "no changes" "world_delta present despite no file changes"
  else
    ok "no world_delta when nothing changed"
  fi
}

# ---- F2.4: JASH_WORLD=0 disables ----
{
  TH=$(fresh_home); WD="$TH/wd4"; mkdir -p "$WD"
  SID4="test_world_off_$$"
  (cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID4" JASH_AGENT=1 JASH_WORLD=0 \
    "$J" -c "touch $WD/x.txt") 2>/dev/null
  OUT=$(cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID4" JASH_AGENT=1 JASH_WORLD=0 \
    "$J" -c "echo test" 2>&1 | strip)
  if echo "$OUT" | grep -q 'world_delta'; then
    no "JASH_WORLD=0" "world_delta present despite JASH_WORLD=0"
  else
    ok "JASH_WORLD=0 disables world-delta"
  fi
}

# ---- F2.5: envelope stays VALID JSON when world_delta (+attribution) is present ----
# This is the regression guard: the emitted line must parse. A missing comma or
# a malformed attribution object silently produced unparseable envelopes before.
{
  TH=$(fresh_home); WD="$TH/wd5"; mkdir -p "$WD"
  SID5="test_world_json_$$"
  touch "$WD/seed.txt"
  (cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID5" JASH_AGENT=1 \
    "$J" -c "echo baseline") 2>/dev/null
  touch "$WD/new1.txt"; printf 'x\n' > "$WD/seed.txt"
  OUT=$(cd "$WD" && HOME="$TH" JASH_BRIEF=0 JASH_NUDGE=0 JASH_SIGIL=1 JASH_SESSION="$SID5" JASH_AGENT=1 \
    "$J" -c "echo done" 2>&1 | strip)
  ENV=$(echo "$OUT" | grep jash_agent | tail -1)
  if [ "$have_py" = 1 ]; then
    if [ -n "$ENV" ] && is_valid_json "$ENV"; then
      ok "envelope is valid JSON with world_delta present"
    else
      no "valid JSON" "envelope with world_delta does not parse"
    fi
    if echo "$ENV" | grep -q 'world_delta' && wd_attr_is_obj "$ENV"; then
      ok "world_delta.attribution is a well-formed object"
    else
      no "attribution shape" "attribution missing or not a JSON object"
    fi
  else
    echo "  ~ skipped JSON validation (python3 unavailable)"
  fi
}

echo ""
echo "== world-delta (ADR-0042 F2): $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ] || exit 1
