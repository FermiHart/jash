#!/usr/bin/env bash
# ADR-0040 W5: per-repo adoption profile.
# Evidence earned in repo A must not promote repo B; a block in B must not
# demote A. Legacy rows (10 fields, no key) count everywhere.
set -u

JASH="${JASH:-./build-speed/jash-dev}"
JASH="$(cd "$(dirname "$JASH")" && pwd)/$(basename "$JASH")"
H="$(mktemp -d /tmp/jash_w5.XXXXXX)"
T="$H/.jash/adoption_sessions.tsv"
mkdir -p "$H/.jash"
RA="$(mktemp -d /tmp/jash_w5_repoA.XXXXXX)"; git -C "$RA" init -q
RB="$(mktemp -d /tmp/jash_w5_repoB.XXXXXX)"; git -C "$RB" init -q
NR="$(mktemp -d /tmp/jash_w5_norepo.XXXXXX)"
PASS=0; FAIL=0

chk() {
  if [ "$2" = "$3" ]; then PASS=$((PASS+1)); echo "ok   - $1";
  else FAIL=$((FAIL+1)); echo "FAIL - $1: expected [$2] got [$3]"; fi
}

probe() { # probe <dir> → adoption level under JASH_ADOPT=auto
  (cd "$1" && env HOME="$H" JASH_AGENT=1 JASH_ADOPT=auto JASH_BRIEF=0 \
    "$JASH" -c "true" 2>&1 | grep -o '"level":[0-9]*' | head -1)
}

keyof() { # keyof <dir> → 11th column written by a session in <dir>
  rm -f "$T"
  (cd "$1" && env HOME="$H" JASH_AGENT=1 JASH_ADOPT=3 JASH_BRIEF=0 \
    "$JASH" -c 'bash -c "echo a" | cat' >/dev/null 2>&1)
  awk -F'\t' '{print $11; exit}' "$T"
}

# --- 1. key shape ----------------------------------------------------------
KA="$(keyof "$RA")"; KB="$(keyof "$RB")"; KN="$(keyof "$NR")"
chk "repo A key is 16 hex" "yes" "$(echo "$KA" | grep -qE '^[0-9a-f]{16}$' && echo yes || echo no)"
chk "repo A and repo B keys differ" "yes" "$([ "$KA" != "$KB" ] && echo yes || echo no)"
chk "outside any repo key is '-'" "-" "$KN"

# --- 2. isolation: A's evidence does not promote B -------------------------
seed() { for i in 1 2 3; do printf '1784587000\t12\t10\t0\t0\t2\t2\t0\t1\t2\t%s\n' "$1" >> "$T"; done; }
rm -f "$T"; seed "$KA"
chk "A's clean history promotes A" '"level":2' "$(probe "$RA")"
chk "A's clean history does NOT promote B" '"level":1' "$(probe "$RB")"

# --- 3. isolation: block in B does not demote A ----------------------------
printf '1784587100\t10\t8\t0\t2\t0\t0\t0\t0\t3\t%s\n' "$KB" >> "$T"
chk "block in B demotes B" '"level":1' "$(probe "$RB")"
chk "block in B does NOT demote A" '"level":2' "$(probe "$RA")"

# --- 4. legacy rows (no key) count everywhere ------------------------------
rm -f "$T"
for i in 1 2 3; do printf '1784587000\t12\t10\t0\t0\t2\t2\t0\t1\t2\n' >> "$T"; done
chk "legacy rows promote in a repo" '"level":2' "$(probe "$RA")"
chk "legacy rows promote outside repos" '"level":2' "$(probe "$NR")"

rm -rf "$H" "$RA" "$RB" "$NR"
echo "PASS=$PASS FAIL=$FAIL"
[ "$FAIL" -eq 0 ] || exit 1
exit 0
