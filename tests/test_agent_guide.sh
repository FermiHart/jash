#!/usr/bin/env bash
# tests/test_agent_guide.sh — ADR-0026 Tier 3: agent field guide.
set -e

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH="$PROJECT_ROOT/build/jash"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

cd "$PROJECT_ROOT"

pass=0
fail=0

ok()  { echo "✓ $1"; pass=$((pass+1)); }
err() { echo "✗ $1"; fail=$((fail+1)); }

# 1. --agent-guide flag emits markdown.
out="$TMP/guide.md"
$JASH --agent-guide > "$out"
if grep -q "# jash Agent Field Guide" "$out" && grep -q "agent-guard" "$out"; then
    ok "--agent-guide emits markdown guide"
else
    err "--agent-guide missing content"
fi

# 2. Builtin agent-guide emits human-readable output.
out="$TMP/guide2.out"
$JASH -c 'agent-guide' > "$out" 2>/dev/null
if grep -q "JASH AGENT FIELD GUIDE" "$out" && grep -q "Unique jash primitives" "$out"; then
    ok "agent-guide builtin emits guide"
else
    err "agent-guide builtin missing content"
fi

# 3. Guide lists key primitives.
if $JASH --agent-guide | grep -qE 'speculate|contract|recall|state|json'; then
    ok "guide lists agent primitives"
else
    err "guide missing agent primitives"
fi

# 4. Guide includes translation table.
if $JASH --agent-guide | grep -q 'bash -c'; then
    ok "guide includes bash-to-jash translation"
else
    err "guide missing translation table"
fi

# ── ADR-0037: 100% surface coverage, ENFORCED against the source ─────────────
# The guide is the agent's only window into what this binary ships. A builtin,
# env var, or CLI flag that exists in code but not in the guide is invisible
# to every LLM — these checks turn that into a red suite.
GUIDE_FULL="$($JASH --agent-guide 2>/dev/null)"

# 5. Every builtin in the live registry appears in the guide.
missing_builtins=""
while read -r b; do
    case "$GUIDE_FULL" in
        *"\`$b\`"*|*" $b "*|*" $b\n"*) : ;;
        *) if ! printf '%s\n' "$GUIDE_FULL" | grep -qE "(^|[ \`])$(printf '%s' "$b" | sed 's/[.?]/\\&/g')([ \`]|\$)"; then
               missing_builtins="$missing_builtins $b"
           fi ;;
    esac
done < <("$JASH" --builtins-json | python3 -c 'import json,sys; print("\n".join(sorted({b["name"] for b in json.load(sys.stdin)["builtins"]})))')
if [ -z "$missing_builtins" ]; then
    ok "guide covers 100% of the live builtin registry"
else
    err "builtins registered but ABSENT from guide:$missing_builtins"
fi

# 6. Every JASH_* env var read in the source appears in the guide.
missing_env=""
while read -r v; do
    printf '%s\n' "$GUIDE_FULL" | grep -q "$v" || missing_env="$missing_env $v"
done < <(grep -rhoE '(getenv|jash_env_get)\(\s*(s,\s*)?"JASH_[A-Z0-9_]+"' c/ modules/ 2>/dev/null | grep -oE 'JASH_[A-Z0-9_]+' | sort -u)
if [ -z "$missing_env" ]; then
    ok "guide covers 100% of the JASH_* env surface"
else
    err "env vars read in source but ABSENT from guide (add to ENV_DOCS):$missing_env"
fi

# 7. Every --flag handled on argv[1] in main.c appears in the guide.
missing_flags=""
while read -r f; do
    printf '%s\n' "$GUIDE_FULL" | grep -q -- "$f" || missing_flags="$missing_flags $f"
done < <(grep -oE 'strcmp\(argv\[1\], "--[a-z-]+"' c/main.c | grep -oE '\-\-[a-z-]+' | sort -u)
if [ -z "$missing_flags" ]; then
    ok "guide covers 100% of the argv[1] CLI flags"
else
    err "CLI flags handled in main.c but ABSENT from guide:$missing_flags"
fi

if printf '%s\n' "$GUIDE_FULL" | grep -Fq 'automatic command history recording unavailable'; then
    ok "guide distinguishes receipt verification from unavailable mirror history"
else
    err "guide overstates the mirror execution-history contract"
fi
if printf '%s\n' "$GUIDE_FULL" | grep -Fq 'cat' &&
   printf '%s\n' "$GUIDE_FULL" | grep -Fq -- '--data byte-exact paged file content'; then
    ok "guide advertises the bounded byte-exact file-read surface"
else
    err "guide omits cat --data file-read capability"
fi
history_json=$("$JASH" -c 'mirror --json --history' 2>/dev/null)
if printf '%s\n' "$history_json" | python3 -c 'import json,sys; assert json.load(sys.stdin) == {"schema":"jash.mirror.history.v1","available":False,"recording":False,"complete":False,"entries":[],"error":"execution_recording_unavailable"}'; then
    ok "mirror JSON history reports unavailable rather than complete-empty"
else
    err "mirror JSON history did not expose its unavailable status"
fi
mirror_target="$TMP/mirror-must-not-run"
if "$JASH" -c "mirror touch $mirror_target" >/dev/null 2>&1; then
    err "mirror unexpectedly accepted an execution request"
elif [ -e "$mirror_target" ]; then
    err "mirror executed a command despite its read-only contract"
else
    ok "mirror command requests fail without executing"
fi
if printf '%s\n' "$GUIDE_FULL" | grep -Fq 'unfinished C test scaffold; not implemented or validated code'; then
    ok "guide labels forge as an unfinished scaffold"
else
    err "guide overstates forge code-generation support"
fi
forge_json=$("$JASH" -c 'forge --json "W8 scaffold contract"' 2>/dev/null)
if printf '%s\n' "$forge_json" | python3 -c 'import json,sys; d=json.load(sys.stdin); assert d["status"] == "scaffold" and d["description"] == "W8 scaffold contract"'; then
    ok "forge JSON identifies scaffold status"
else
    err "forge JSON implied a completed generator"
fi
forge_source=$("$JASH" -c 'forge "W8 scaffold contract"' 2>/dev/null)
if printf '%s\n' "$forge_source" | grep -Fq '#error "forge scaffold: implement the test before use"'; then
    ok "forge source output refuses compilation until implemented"
else
    err "forge emitted source without the explicit scaffold guard"
fi
if "$JASH" -c 'forge --lang python example' >/dev/null 2>&1; then
    err "forge accepted an unimplemented language option"
else
    ok "forge rejects unsupported generation options"
fi

# 8. --save must perform the documented effect, not only print redirect advice.
SAVE_HOME="$TMP/save-home"; mkdir -m 0700 -p "$SAVE_HOME"
HOME="$SAVE_HOME" JASH_SIGIL=0 "$JASH" -c 'agent-guide --save' > "$TMP/save.out" 2>&1
saved="$SAVE_HOME/.jash/agent_guide.md"
HOME="$SAVE_HOME" JASH_SIGIL=0 "$JASH" -c 'agent-guide --markdown' > "$TMP/expected.md" 2>/dev/null
if stat -c '%a' "$saved" >/dev/null 2>&1; then
    saved_mode="$(stat -c '%a' "$saved")"
else
    saved_mode="$(stat -f '%Lp' "$saved")"
fi
if [ -f "$saved" ] && cmp -s "$saved" "$TMP/expected.md" &&
   [ "$saved_mode" = "600" ]; then
    ok "agent-guide --save writes the exact markdown atomically/private"
else
    err "agent-guide --save did not persist the exact guide"
fi

# Under agent-strict the effect depends on argv: plain guide is read-only,
# --save writes HOME state and must require effect:mutate.
AGENTD="$PROJECT_ROOT/build/jash-agentd"
STRICT_HOME="$TMP/strict-home"; mkdir -m 0700 -p "$STRICT_HOME"
read_req='{"id":"guide-read","argv":["agent-guide","--save"],"caps":["effect:read"]}'
read_reply=$(printf '%s\n' "$read_req" | HOME="$STRICT_HOME" JASH_SIGIL=0 "$AGENTD" 2>/dev/null)
if [ ! -e "$STRICT_HOME/.jash/agent_guide.md" ] &&
   printf '%s\n' "$read_reply" | grep -q '"id":"guide-read".*"rc":125.*capability denied'; then
    ok "agent-strict refuses --save with effect:read before writing"
else
    err "agent-strict did not enforce the --save mutation effect"
fi
write_req='{"id":"guide-write","argv":["agent-guide","--save"],"caps":["effect:mutate"]}'
write_reply=$(printf '%s\n' "$write_req" | HOME="$STRICT_HOME" JASH_SIGIL=0 "$AGENTD" 2>/dev/null)
if [ -f "$STRICT_HOME/.jash/agent_guide.md" ] &&
   printf '%s\n' "$write_reply" | grep -q '"id":"guide-write".*"rc":0'; then
    ok "agent-strict allows --save with effect:mutate"
else
    err "agent-strict failed authorized --save"
fi
quoted_home="$TMP/quoted-home"; mkdir -m 0700 -p "$quoted_home"
quoted_req='{"id":"guide-quoted-flag","argv":["agent-guide","quoted --save"],"caps":["effect:read"]}'
quoted_reply=$(printf '%s\n' "$quoted_req" | HOME="$quoted_home" JASH_SIGIL=0 "$AGENTD" 2>/dev/null)
if [ ! -e "$quoted_home/.jash/agent_guide.md" ] &&
   printf '%s\n' "$quoted_reply" | grep -q '"id":"guide-quoted-flag".*"rc":0'; then
    ok "agent-strict does not promote --save inside a single quoted argv item"
else
    err "quoted argv text diverged from the agent-guide effect decision"
fi
typo_home="$TMP/typo-home"; mkdir -m 0700 -p "$typo_home"
typo_req='{"id":"guide-save-typo","argv":["agent-guide","--save-extra"],"caps":["effect:read"]}'
typo_reply=$(printf '%s\n' "$typo_req" | HOME="$typo_home" JASH_SIGIL=0 "$AGENTD" 2>/dev/null)
if [ ! -e "$typo_home/.jash/agent_guide.md" ] &&
   printf '%s\n' "$typo_reply" | grep -q '"id":"guide-save-typo".*"rc":0'; then
    ok "agent-strict keeps unknown --save-prefixed options read-only"
else
    err "agent-guide option parsing diverged from its argv effect"
fi

# 9. The documented save path is not allowed to follow a target symlink.
LINK_HOME="$TMP/link-home"; mkdir -m 0700 -p "$LINK_HOME/.jash"
printf 'untouched\n' > "$TMP/victim.md"
ln -s "$TMP/victim.md" "$LINK_HOME/.jash/agent_guide.md"
if HOME="$LINK_HOME" JASH_SIGIL=0 "$JASH" -c 'agent-guide --save' >/dev/null 2>&1; then
    err "agent-guide --save accepted a symlink target"
elif [ "$(cat "$TMP/victim.md")" = "untouched" ]; then
    ok "agent-guide --save refuses symlink targets without touching them"
else
    err "agent-guide --save modified a symlink target"
fi

echo "═══════════════════════════════════════════════════════════════"
echo "  agent-guide RESULTS"
echo "═══════════════════════════════════════════════════════════════"
echo "  PASS: $pass  FAIL: $fail  TOTAL: $((pass+fail))"
if [ "$fail" -eq 0 ]; then
    echo "  ✅ agent-guide tests PASSED"
    exit 0
else
    echo "  ✗ agent-guide tests FAILED"
    exit 1
fi
