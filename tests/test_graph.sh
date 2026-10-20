#!/usr/bin/env bash
# test_graph.sh — ADR-0053 living graph: build→persist→incremental→hot→budget.
#
# The graph lives in ~/.jash/graph-<repokey>.tsv (graph.tsv outside a git
# repo), so every verb is exercised in a SEPARATE `jash -c` process — the
# pattern that killed the old in-process skeleton (G1). Fixture is a mini C
# project outside any git repo (repokey "-"): a.c main→foo, util.c foo→bar
# +foo→dup(static), b.c bar→dup(static), structs/enum/typedef in headers.
set -u
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
J="$ROOT/build/jash"; [ -x "$J" ] || J="$ROOT/build-speed/jash-release"
[ -x "$J" ] || { echo "SKIP: no jash binary"; exit 0; }
PASS=0; FAIL=0
ok(){ PASS=$((PASS+1)); echo "  ✓ $1"; }
no(){ FAIL=$((FAIL+1)); echo "  ✗ $1 — $2"; }
strip(){ sed $'s/\x1b\\[[0-9;]*m//g'; }
TH="$(mktemp -d)"; FIX="$(mktemp -d)"; bash "$ROOT/tests/setup_sigil_modules.sh" "$TH"; export HOME="$TH" JASH_SIGIL=1
G(){ (cd "$FIX" && "$J" -c "graph $1" 2>/dev/null | strip); }
# Grc: same, but propagates jash's rc through the strip pipe
Grc(){ (cd "$FIX" && "$J" -c "graph $1" 2>/dev/null | strip; exit "${PIPESTATUS[0]}"); }
RUN(){ (cd "$FIX" && "$J" -c "$1" >/dev/null 2>&1); }

cat > "$FIX/a.c" <<'EOF'
#include "util.h"
int main(void) { return foo(); }
static int dup(void) { return 1; }
EOF
cat > "$FIX/util.c" <<'EOF'
#include "util.h"
#include "b.h"
int foo(void) { return bar() + dup(); }
static int dup(void) { return 2; }
EOF
cat > "$FIX/b.c" <<'EOF'
#include "b.h"
int bar(void) { return dup(); }
static int dup(void) { return 3; }
EOF
cat > "$FIX/util.h" <<'EOF'
struct point { int x; int y; };
typedef unsigned long my_size;
enum color { RED, GREEN };
int foo(void);
EOF
cat > "$FIX/b.h" <<'EOF'
int bar(void);
EOF

echo "== living graph: build, verbs, incremental, hot, budget =="

# 1. build creates the TSV and declares the delta + totals
out=$(G 'build')
printf '%s' "$out" | grep -q 'build: 5 files (0 reused, 5 re-parsed, 0 gone, 0 skipped)' \
  && printf '%s' "$out" | grep -q '14 nodes, 17 edges' \
  && ok 'build declares files/nodes/edges delta' || no 'build output' "$out"
[ -f "$TH/.jash/graph.tsv" ] && ok 'TSV persisted' || no 'TSV' "$(ls "$TH/.jash" 2>/dev/null)"

# 2. cross-call edge resolved EXTRACTED, visible in a separate process
out=$(G 'explain foo')
printf '%s' "$out" | grep -q -- '--calls--> bar (b.c:2) \[EXTRACTED\]' \
  && ok 'explain foo: --calls--> bar EXTRACTED (cross-file)' || no 'explain foo' "$out"
printf '%s' "$out" | grep -q -- '<--calledby-- main (a.c:2)' \
  && ok 'explain foo: <--calledby-- main' || no 'calledby' "$out"

# 3. path main→bar is a 2-hop chain
out=$(G 'path main bar')
printf '%s' "$out" | grep -q 'main --calls--> foo --calls--> bar' \
  && ok 'path main bar: 2 hops' || no 'path' "$out"

# 4. god-nodes ranks the most-connected node first (util.h: deg 5)
out=$(G 'god-nodes --top 3')
first=$(printf '%s' "$out" | grep -m1 '^\s*[0-9]')
printf '%s' "$first" | grep -q 'util.h' \
  && ok 'god-nodes: most connected first' || no 'god-nodes' "$out"

# 5. incremental: real content change re-parses only that file; clean build reuses all
printf 'int baz(void) { return bar(); }\n' >> "$FIX/util.c"
out=$(G 'build')
printf '%s' "$out" | grep -q '(4 reused, 1 re-parsed, 0 gone' \
  && ok 'incremental: 1 re-parsed, 4 reused declared' || no 'incremental' "$out"
out=$(G 'build')
printf '%s' "$out" | grep -q '(5 reused, 0 re-parsed, 0 gone' \
  && ok 'clean rebuild: 0 re-parsed' || no 'rebuild' "$out"

# 6. deleted file leaves the graph on rebuild
rm "$FIX/b.c"
out=$(G 'build')
printf '%s' "$out" | grep -q '1 gone' \
  && ok 'deleted file declared gone' || no 'gone' "$out"
out=$(Grc 'explain bar'); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q 'not found' \
  && ok 'deleted function leaves the graph' || no 'gone node' "rc=$rc [$out]"

# 7. hot: chain touches/fails rank files by pain (util.h: 1 touch + 1 fail = 4)
RUN "cat $FIX/util.c"
RUN "cat $FIX/util.c"
RUN "ls $FIX/util.h/nope"
out=$(G 'hot')
printf '%s' "$out" | grep -q 'HOT FILES (pain = touches + 3×fails, from attested chain)' \
  && ok 'hot header honest' || no 'hot header' "$out"
hot1=$(printf '%s' "$out" | grep -m1 '^\s*[0-9]')
printf '%s' "$hot1" | grep -q 'util.h' \
  && ok 'hot: failed file ranks first' || no 'hot rank' "$out"
printf '%s' "$out" | grep -q '[0-9] files touched, [0-9] failures, [0-9]* chain lines scanned' \
  && ok 'hot declares totals' || no 'hot totals' "$out"

# 8. --budget caps output with an honest footer
out=$(G 'god-nodes --top 8 --budget 60')
printf '%s' "$out" | grep -q 'output capped at 60 bytes — refine with --top/--json' \
  && ok '--budget cuts with footer' || no 'budget' "$out"

# 9. --json is stable on explain and stats
out=$(G 'explain foo --json')
printf '%s' "$out" | grep -q '"name":"foo","type":"F","file":"util.c"' \
  && printf '%s' "$out" | grep -q '"rel":"calls","target":"main","conf":"EXTRACTED"' \
  && ok 'explain --json fields' || no 'explain json' "$out"
out=$(G 'stats --json')
printf '%s' "$out" | grep -q '"nodes":[0-9]' \
  && printf '%s' "$out" | grep -q '"extracted":[0-9]' \
  && ok 'stats --json fields' || no 'stats json' "$out"

# 10. unknown name: rc=1 with honest lookup failure
out=$(Grc 'explain nosuchfunc'); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q "graph: 'nosuchfunc' not found" \
  && ok 'explain unknown: rc=1 + not found' || no 'unknown' "rc=$rc [$out]"

# 11. every build is attested in the chain
grep -q 'cmd=graph-build' "$TH/.jash/chain.log" \
  && ok 'build attested in chain' || no 'attest' "$(tail -3 "$TH/.jash/chain.log" 2>/dev/null)"

# 12. INFERRED: 3 statics named dup → cross-file resolution is declared INFERRED
#     (same-file preferred: foo→dup lands on util.c's dup, never blended with EXTRACTED)
out=$(G 'explain foo --json')
printf '%s' "$out" | grep -q '"rel":"calls","target":"dup","conf":"INFERRED"' \
  && ok 'ambiguous static call resolved INFERRED (same-file)' || no 'INFERRED' "$out"

# 13. no graph → honest error, rc=1
TH2="$(mktemp -d)"; FIX2="$(mktemp -d)"; bash "$ROOT/tests/setup_sigil_modules.sh" "$TH2"
out=$( (cd "$FIX2" && HOME="$TH2" "$J" -c 'graph stats' 2>&1 | strip; exit "${PIPESTATUS[0]}") ); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q "no graph for this repo — run 'graph build'" \
  && ok 'missing graph: rc=1 + hint' || no 'missing graph' "rc=$rc [$out]"
rm -rf "$TH2" "$FIX2"

# 14. no-args help lists the verbs
printf '%s' "$(G '')" | grep -q 'living graph' \
  && printf '%s' "$(G '')" | grep -q 'graph hot' \
  && ok 'help on no args' || no 'help' ""

echo "== living graph W7: mesh push/sync =="

# 15. push is an explicit verb: inactive mesh is an honest rc=1, never silent
out=$( (cd "$FIX" && "$J" -c 'graph push' 2>&1 | strip; exit "${PIPESTATUS[0]}") ); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q 'mesh inactive' \
  && ok 'push with inactive mesh: rc=1 honest' || no 'push inactive' "rc=$rc [$out]"
out=$( (cd "$FIX" && "$J" -c 'graph sync' 2>&1 | strip; exit "${PIPESTATUS[0]}") ); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q 'mesh inactive' \
  && ok 'sync with inactive mesh: rc=1 honest' || no 'sync inactive' "rc=$rc [$out]"

# cross-HOME fabric: two agents, two repos, ONE mesh dir (symlinked)
H1="$(mktemp -d)"; H2="$(mktemp -d)"; F1="$(mktemp -d)"; F2="$(mktemp -d)"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H1"
bash "$ROOT/tests/setup_sigil_modules.sh" "$H2"
mkdir -p "$H1/.jash/mesh" "$H2/.jash"
ln -s "$H1/.jash/mesh" "$H2/.jash/mesh"
GX(){ (cd "$2" && HOME="$1" JASH_SIGIL=1 "$J" -c "graph $3" 2>/dev/null | strip); }

# repo A: foo→bar EXTRACTED (single bar); shared lib.c, common at line 1, OLD
cat > "$F1/util.c" <<'EOF'
#include "b.h"
int foo(void) { return bar(); }
EOF
cat > "$F1/b.c" <<'EOF'
int bar(void) { return 1; }
EOF
cat > "$F1/lib.c" <<'EOF'
int common(void) { return 1; }
EOF
cat > "$F1/b.h" <<'EOF'
int bar(void);
EOF
touch -t 202001010000 "$F1/lib.c"
# repo B: same foo/bar plus a second static bar in a.c (→ INFERRED, resolves
# to b.c's bar — same edge key as A's EXTRACTED); lib.c newer, common at line 3
cat > "$F2/util.c" <<'EOF'
#include "b.h"
int foo(void) { return bar(); }
EOF
cat > "$F2/b.c" <<'EOF'
int bar(void) { return 1; }
EOF
cat > "$F2/a.c" <<'EOF'
static int bar(void) { return 2; }
EOF
cat > "$F2/lib.c" <<'EOF'
/* newer version */
/* of the shared file */
int common(void) { return 2; }
EOF
cat > "$F2/b.h" <<'EOF'
int bar(void);
EOF
touch -t 202101010000 "$F2/lib.c"

# 16. push publishes to the mesh file with the same counts + attests
GX "$H1" "$F1" 'build' >/dev/null
out=$(GX "$H1" "$F1" 'push')
printf '%s' "$out" | grep -q 'push: 7 nodes, 5 edges → mesh (+0 merged from fleet)' \
  && ok 'push declares published counts' || no 'push out' "$out"
[ -f "$H1/.jash/mesh/graph.tsv" ] && ok 'mesh graph file exists' || no 'mesh file' ""
na=$(grep -c '^N' "$H1/.jash/mesh/graph.tsv")
nb=$(GX "$H1" "$F1" 'stats --json' | grep -o '"nodes":[0-9]*' | grep -o '[0-9]*')
[ "$na" = "$nb" ] && ok 'mesh file has local counts' || no 'mesh counts' "$na vs $nb"
grep -q 'cmd=graph-push' "$H1/.jash/chain.log" \
  && ok 'push attested in chain' || no 'attest push' ""

# 17. union merge across the fabric: B pushes, both sync, counts converge
GX "$H2" "$F2" 'build' >/dev/null
out=$(GX "$H2" "$F2" 'push')
printf '%s' "$out" | grep -q 'push: ' \
  && ok 'B pushes through the symlinked mesh' || no 'B push' "$out"
out=$(GX "$H1" "$F1" 'sync')
printf '%s' "$out" | grep -q 'sync: +2 nodes, +1 edges from fleet (union, deterministic)' \
  && ok 'sync declares the delta' || no 'sync delta' "$out"
grep -q 'cmd=graph-sync' "$H1/.jash/chain.log" \
  && ok 'sync attested in chain' || no 'attest sync' ""
GX "$H2" "$F2" 'sync' >/dev/null
sa=$(GX "$H1" "$F1" 'stats --json' | grep -o '"nodes":[0-9]*,"funcs"')
sb=$(GX "$H2" "$F2" 'stats --json' | grep -o '"nodes":[0-9]*,"funcs"')
ea=$(GX "$H1" "$F1" 'stats --json' | grep -o '"edges":[0-9]*,"calls"')
eb=$(GX "$H2" "$F2" 'stats --json' | grep -o '"edges":[0-9]*,"calls"')
[ "$sa" = "$sb" ] && [ "$ea" = "$eb" ] \
  && ok 'union deterministic: same counts both sides' || no 'determinism' "$sa/$ea vs $sb/$eb"

# 18. shared node dedups by (name,type,file): common appears ONCE, and the
#     newer-mtime side (B, line 3) wins the conflict
[ "$(grep -c $'^N\tcommon\tF\t' "$H1/.jash/graph.tsv")" = "1" ] \
  && ok 'shared node deduped by key' || no 'node dedup' "$(grep '^N' "$H1/.jash/graph.tsv")"
out=$(GX "$H1" "$F1" 'explain common')
printf '%s' "$out" | head -1 | grep -q 'lib.c:3' \
  && ok 'conflict: newer-mtime side wins (line 3)' || no 'conflict' "$out"

# 19. B's own INFERRED foo→bar resolved to the same node A read EXTRACTED —
#     after the union, B carries ONE foo→bar edge and it is EXTRACTED
out=$(GX "$H2" "$F2" 'explain foo --json')
[ "$(printf '%s' "$out" | grep -o '"target":"bar"' | wc -l | tr -d ' ')" = "1" ] \
  && printf '%s' "$out" | grep -q '"rel":"calls","target":"bar","conf":"EXTRACTED"' \
  && ok 'EXTRACTED beats INFERRED on the same edge key' || no 'conf conflict' "$out"

# 20. sync with no fleet graph is honest
H3="$(mktemp -d)"; F3="$(mktemp -d)"; bash "$ROOT/tests/setup_sigil_modules.sh" "$H3"; mkdir -p "$H3/.jash/mesh"
out=$( (cd "$F3" && HOME="$H3" "$J" -c 'graph sync' 2>&1 | strip; exit "${PIPESTATUS[0]}") ); rc=$?
[ $rc -eq 1 ] && printf '%s' "$out" | grep -q 'no fleet graph yet' \
  && ok 'sync without fleet graph: rc=1 honest' || no 'sync empty' "rc=$rc [$out]"
rm -rf "$H1" "$H2" "$F1" "$F2" "$H3" "$F3"

unset HOME
rm -rf "$TH" "$FIX"
echo "== graph: $PASS passed, $FAIL failed =="
[ "$FAIL" -eq 0 ]
