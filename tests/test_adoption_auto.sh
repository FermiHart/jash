#!/usr/bin/env bash
# ADR-0040 W4: adoption telemetry + auto governor ladder.
# Covers: one row per session (absorbed children must NOT double-write),
# row shape, and the JASH_ADOPT=auto escalation/demotion ladder.
set -u

JASH="${JASH:-./build-speed/jash-dev}"
H="$(mktemp -d /tmp/jash_w4.XXXXXX)"
T="$H/.jash/adoption_sessions.tsv"
mkdir -p "$H/.jash"
PASS=0; FAIL=0

chk() { # chk <desc> <expected> <got>
  if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "ok   - $1";
  else FAIL=$((FAIL+1)); echo "FAIL - $1: expected [$2] got [$3]"; fi
}

probe() { # adoption fragment from the envelope under JASH_ADOPT=auto
  env HOME="$H" JASH_AGENT=1 JASH_ADOPT=auto JASH_BRIEF=0 "$JASH" -c "true" 2>&1 \
    | grep -o '"adoption":{[^}]*}'
}

# --- 1. one row per session, even with absorbed children -------------------
rm -f "$T"
env HOME="$H" JASH_AGENT=1 JASH_ADOPT=3 JASH_BRIEF=0 "$JASH" \
  -c 'bash -c "echo a" | cat' >/dev/null 2>&1
chk "single row despite absorbed child" "1" "$(wc -l < "$T" | tr -d ' ')"
chk "row has 11 fields (W5 repo key)" "11" "$(awk -F'\t' '{print NF; exit}' "$T")"
chk "row records absorption" "1" "$(awk -F'\t' '{print $7; exit}' "$T")"

# --- 2. governor ladder ----------------------------------------------------
rm -f "$T"
chk "empty history -> level 1" \
  '"adoption":{"level":1,"source":"auto governor"}' "$(probe)"

rm -f "$T"
for i in 1 2 3; do printf '1784587000\t12\t10\t0\t0\t2\t2\t0\t1\t2\n' >> "$T"; done
chk "3 clean sessions w/ evidence -> level 2" \
  '"adoption":{"level":2,"source":"auto governor"}' "$(probe)"

rm -f "$T"
for i in 1 2 3 4 5 6 7 8; do printf '1784587000\t15\t14\t0\t0\t1\t1\t0\t0\t2\n' >> "$T"; done
chk "8 mature sessions -> level 3" \
  '"adoption":{"level":3,"source":"auto governor"}' "$(probe)"

printf '1784587100\t10\t8\t0\t2\t0\t0\t0\t0\t3\n' >> "$T"
chk "recent blocked session -> demoted to 1" \
  '"adoption":{"level":1,"source":"auto governor"}' "$(probe)"

# --- 3. explicit level still overrides the governor ------------------------
got=$(env HOME="$H" JASH_AGENT=1 JASH_ADOPT=2 JASH_BRIEF=0 "$JASH" -c "true" 2>&1 \
  | grep -o '"adoption":{[^}]*}' | grep -o '"level":[0-9]*')
chk "explicit JASH_ADOPT=2 overrides history" '"level":2' "$got"

rm -rf "$H"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
