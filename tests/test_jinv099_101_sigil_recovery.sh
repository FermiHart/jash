#!/usr/bin/env bash
# JINV-099/100/101: one physical line per entry, byte-identical hash/write,
# repo-scoped ratchets, conservative legacy detection, explicit quarantine.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
MOD="$PWD/modules/sigil/sigil.modjash"
[ -x "$JASH" ] || { echo "SKIP: $JASH missing"; exit 0; }
[ -f "$MOD" ] || { echo "SKIP: $MOD missing"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 0; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

setup_case() {
    H="$T/$1/home"
    R="$T/$1/repo"
    mkdir -p "$R"
    git init -q "$R" || return 1
    bash "$PWD/tests/setup_sigil_modules.sh" "$H"
}

run_jash() {
    local home=$1 repo=$2 level=$3 command=$4
    (cd "$repo" && HOME="$home" JASH_SIGIL="$level" JASH_NUDGE=0 \
        JASH_BRIEF=0 JASH_LOOPGUARD=off "$JASH" -c "$command")
}

chainfile() {
    local home=$1 f
    for f in "$home"/.jash/chain-*.log; do
        [ -e "$f" ] && { printf '%s\n' "$f"; return 0; }
    done
    return 1
}

is_valid() {
    printf '%s' "$1" | python3 -c \
        'import json,sys; print(str(json.load(sys.stdin).get("valid", False)).lower())' 2>/dev/null
}

# A: multiline commands stay on one physical ledger line and verify.
setup_case multiline
multiline_cmd=$'printf \'first\nsecond\''
run_jash "$H" "$R" 1 "$multiline_cmd" >/dev/null 2>&1
cf=$(chainfile "$H")
physical=$(wc -l < "$cf" | tr -d ' ')
entries=$(grep -c ' tag=' "$cf" || true)
[ "$physical" = "$entries" ] && ok "multiline: physical_lines == entries ($entries)" \
    || bad "multiline physical lines" "physical=$physical entries=$entries"
grep -Fq '\n' "$cf" && ok "multiline command is wire-escaped" \
    || bad "multiline escaping" "no literal \\n in cmd field"
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
[ "$(is_valid "$out")" = true ] && ok "multiline chain verifies" \
    || bad "multiline verify" "$out"

# B/H: the written command is clipped to exactly the hashed payload boundary,
# and every chain reader consumes that maximum line without splitting it.
setup_case long
long=$(python3 -c 'print("x" * 5000, end="")')
run_jash "$H" "$R" 1 "printf '%s' '$long JINV_SUFFIX' >/dev/null" >/dev/null 2>&1
cf=$(chainfile "$H")
if grep -q 'JINV_SUFFIX' "$cf"; then
    bad "payload boundary" "unattested suffix was written"
else
    ok "payload boundary drops suffix that is outside hash bytes"
fi
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
[ "$(is_valid "$out")" = true ] && ok "maximum written line verifies" \
    || bad "maximum line verify" "$out"
list=$(run_jash "$H" "$R" 0 'sigil list --json' 2>/dev/null || true)
if printf '%s\n' "$list" | python3 -c \
    'import json,sys; [json.loads(x) for x in sys.stdin if x.strip()]' 2>/dev/null; then
    ok "list reader handles maximum escaped line"
else
    bad "list maximum line" "JSONL was split or truncated"
fi
run_jash "$H" "$R" 0 'sigil history --tail 2' >/dev/null 2>&1 \
    && ok "history reader handles maximum escaped line" \
    || bad "history maximum line" "reader failed"
if grep -Eq 'char line\[(1024|2048)\]|char lines\[[^]]+\]\[(1024|2048)\]' \
        modules/sigil/sigil_module.c; then
    bad "reader cap ratchet" "a legacy 1K/2K chain buffer remains"
else
    ok "all module chain readers use SIGIL_LINE_MAX"
fi

# C: a trailing physical fragment is known corruption, never migration or
# forgery, and initialization cannot append over it.
setup_case fragment
run_jash "$H" "$R" 1 'true' >/dev/null 2>&1
cf=$(chainfile "$H")
cp "$cf" "$T/fragment-prefix.log"
prefix_sha=$(shasum -a 256 "$cf" | cut -d' ' -f1)
printf 'legacy continuation\n' >> "$cf"
printf 'legacy continuation\n' > "$T/fragment-suffix.log"
fragment_entries=$(grep -c ' tag=' "$cf" || true)
before=$(shasum -a 256 "$cf" | cut -d' ' -f1)
blocked=$(run_jash "$H" "$R" 1 'true' 2>&1 || true)
after=$(shasum -a 256 "$cf" | cut -d' ' -f1)
case "$blocked" in
    *corrupted_newline_fragment*) ok "fragment blocks append with exact reason" ;;
    *) bad "fragment append block" "$blocked" ;;
esac
[ "$before" = "$after" ] && ok "blocked append preserves active bytes" \
    || bad "blocked append mutated chain" "$before != $after"
[ ! -e "$cf.v1" ] && ok "S7 plus fragment is not renamed to .v1" \
    || bad "JINV-101 migration" ".v1 was created"
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
summary=$(printf '%s' "$out" | python3 -c \
    'import json,sys; d=json.load(sys.stdin); print(d.get("valid"), d.get("reason"), d.get("fragments"), "entries_verified" in d)' 2>/dev/null)
[ "$summary" = "False corrupted_newline_fragment 1 True" ] \
    && ok "JSON verify distinguishes fragment and reports counts" \
    || bad "fragment JSON verdict" "$summary / $out"
txt=$(run_jash "$H" "$R" 0 'sigil verify' 2>/dev/null || true)
case "$txt" in
    *"KNOWN CORRUPTION"*corrupted_newline_fragment*) ok "text verify names known corruption" ;;
    *) bad "text fragment verdict" "$txt" ;;
esac
FRAG_H=$H FRAG_R=$R FRAG_CF=$cf FRAG_SHA=$before FRAG_PREFIX_SHA=$prefix_sha
FRAG_ENTRIES=$fragment_entries

# C2: a fragment in the middle is ambiguous and must never be auto-salvaged.
setup_case middlefrag
run_jash "$H" "$R" 1 'echo first' >/dev/null 2>&1
run_jash "$H" "$R" 1 'echo second' >/dev/null 2>&1
cf=$(chainfile "$H")
python3 - "$cf" <<'PY'
import sys
p = sys.argv[1]
lines = open(p, "rb").readlines()
open(p, "wb").write(lines[0] + b"middle fragment\n" + b"".join(lines[1:]))
PY
middle_before=$(shasum -a 256 "$cf" | cut -d' ' -f1)
middle_apply=$(run_jash "$H" "$R" 0 'sigil quarantine apply --json' 2>/dev/null || true)
middle_after=$(shasum -a 256 "$cf" | cut -d' ' -f1)
case "$middle_apply" in
    *'"applied":0'*'"failed":1'*'"tail_salvaged":false'*)
        ok "middle fragment refuses automatic salvage" ;;
    *) bad "middle fragment recovery" "$middle_apply" ;;
esac
[ "$middle_before" = "$middle_after" ] && ok "refused middle salvage preserves bytes" \
    || bad "middle salvage mutated chain" "$middle_before != $middle_after"

# D: an S7 file mislabeled .v1 is an invalid migration artifact. A recognized
# tag-free v1 retains migration behavior and is not put in that class.
setup_case badv1
run_jash "$H" "$R" 1 'true' >/dev/null 2>&1
cf=$(chainfile "$H")
cp "$cf" "$cf.v1"
status=$(run_jash "$H" "$R" 0 'sigil quarantine status --json' 2>/dev/null || true)
case "$status" in
    *invalid_migration_v1*) ok "tag-bearing .v1 is invalid_migration_v1" ;;
    *) bad "bad .v1 classification" "$status" ;;
esac
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
case "$out" in
    *'"valid":false'*invalid_migration_v1*) ok "verify rejects invalid .v1 artifact" ;;
    *) bad "bad .v1 verify" "$out" ;;
esac

setup_case genuinev1
run_jash "$H" "$R" 1 'true' >/dev/null 2>&1
cf=$(chainfile "$H")
rm -f "$H"/.jash/ratchet-*.key
zeros=$(printf '%064d' 0)
printf '0 1.000000000 %s rc=0 cmd=legacy\n' "$zeros" > "$cf"
legacy_sha=$(shasum -a 256 "$cf" | cut -d' ' -f1)
status=$(run_jash "$H" "$R" 1 'sigil quarantine status --json' 2>/dev/null || true)
case "$status" in
    *invalid_migration_v1*) bad "genuine v1 classification" "$status" ;;
    *) ok "recognized genuine v1 is not invalid_migration_v1" ;;
esac
after_status=$(shasum -a 256 "$cf" | cut -d' ' -f1)
[ "$legacy_sha" = "$after_status" ] && [ ! -e "$cf.v1" ] \
    && ok "status does not migrate or alter genuine v1 bytes" \
    || bad "genuine v1 status side effect" "$legacy_sha != $after_status"
run_jash "$H" "$R" 1 'true' >/dev/null 2>&1
[ -f "$cf.v1" ] && [ -f "$cf" ] && ok "first append keeps genuine v1 migration behavior" \
    || bad "genuine v1 migration" "expected archived v1 plus new S7 chain"

# E: each repository owns an independent ratchet under one isolated HOME.
H="$T/two-repos/home"; R1="$T/two-repos/repo1"; R2="$T/two-repos/repo2"
mkdir -p "$R1" "$R2"
git init -q "$R1" || exit 2
git init -q "$R2" || exit 2
bash "$PWD/tests/setup_sigil_modules.sh" "$H"
run_jash "$H" "$R1" 1 'true' >/dev/null 2>&1
run_jash "$H" "$R2" 1 'false' >/dev/null 2>&1 || true
set -- "$H"/.jash/ratchet-*.key
[ "$#" -eq 2 ] && ok "two repos create two scoped ratchets" \
    || bad "scoped ratchet count" "count=$#"
v1=$(run_jash "$H" "$R1" 0 'sigil verify --json' 2>/dev/null || true)
v2=$(run_jash "$H" "$R2" 0 'sigil verify --json' 2>/dev/null || true)
if [ "$(is_valid "$v1")" = true ] && [ "$(is_valid "$v2")" = true ] &&
   ! printf '%s%s' "$v1" "$v2" | grep -q '"missing"'; then
    ok "repo switch leaves both chains independently valid"
else
    bad "two-repo verification" "repo1=$v1 repo2=$v2"
fi

# F: a scoped chain with only the legacy global ratchet is blocked and named.
setup_case mismatch
run_jash "$H" "$R" 1 'true' >/dev/null 2>&1
cf=$(chainfile "$H")
rp=$(printf '%s\n' "$H"/.jash/ratchet-*.key)
mv "$rp" "$H/.jash/ratchet.key"
before=$(shasum -a 256 "$cf" | cut -d' ' -f1)
blocked=$(run_jash "$H" "$R" 1 'true' 2>&1 || true)
after=$(shasum -a 256 "$cf" | cut -d' ' -f1)
case "$blocked" in
    *legacy_global_ratchet_scope_mismatch*) ok "legacy ratchet mismatch blocks append" ;;
    *) bad "legacy ratchet mismatch" "$blocked" ;;
esac
[ "$before" = "$after" ] && ok "mismatch block preserves chain" \
    || bad "mismatch mutated chain" "$before != $after"
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
case "$out" in
    *'"valid":false'*legacy_global_ratchet_scope_mismatch*) ok "verify names scope mismatch" ;;
    *) bad "mismatch verify" "$out" ;;
esac

# A scoped chain without ANY ratchet is also unsafe: inventing a fresh key
# beside existing entries would make the witness permanently disagree.
setup_case missingratchet
run_jash "$H" "$R" 1 'true' >/dev/null 2>&1
cf=$(chainfile "$H")
rm -f "$H"/.jash/ratchet*.key
before=$(shasum -a 256 "$cf" | cut -d' ' -f1)
blocked=$(run_jash "$H" "$R" 1 'true' 2>&1 || true)
after=$(shasum -a 256 "$cf" | cut -d' ' -f1)
case "$blocked" in
    *missing_scoped_ratchet*) ok "missing scoped ratchet blocks append" ;;
    *) bad "missing scoped ratchet" "$blocked" ;;
esac
[ "$before" = "$after" ] && ok "missing-ratchet block preserves chain" \
    || bad "missing-ratchet mutated chain" "$before != $after"

# F2: outside a repository the active witness is ratchet.key itself. Full
# quarantine must move that pair together, not strand a witness for no ledger.
H="$T/global-pair/home"; R="$T/global-pair/work"
mkdir -p "$R"
bash "$PWD/tests/setup_sigil_modules.sh" "$H"
run_jash "$H" "$R" 1 'echo global-pair' >/dev/null 2>&1
cf="$H/.jash/chain.log"
awk 'NR == 1 {sub(/cmd=.*/, "cmd=TAMPERED")} {print}' "$cf" > "$cf.bad"
mv "$cf.bad" "$cf"
apply=$(run_jash "$H" "$R" 0 'sigil quarantine apply --json' 2>/dev/null || true)
qratchet=
for p in "$H"/.jash/quarantine/hash_mismatch-*ratchet.key; do
    [ -f "$p" ] && { qratchet=$p; break; }
done
if printf '%s' "$apply" | grep -q '"applied":1.*"failed":0.*"active_reset":true' &&
   [ -n "$qratchet" ] && [ -f "$H/.jash/chain.log" ] &&
   [ -f "$H/.jash/ratchet.key" ]; then
    ok "global chain.log e ratchet.key sao quarentenados como um par"
else
    bad "global full quarantine deixou witness orfao" \
        "apply=$apply qratchet=$qratchet"
fi

# G: status is read-only; apply preserves bytes, manifests the move, moves the
# scoped ratchet, never moves the shared global ratchet, and starts clean.
H=$FRAG_H R=$FRAG_R cf=$FRAG_CF before=$FRAG_SHA prefix_sha=$FRAG_PREFIX_SHA
printf 'global-sentinel\n' > "$H/.jash/ratchet.key"
cp "$H/.jash/ratchet.key" "$T/global-ratchet"
status=$(run_jash "$H" "$R" 0 'sigil quarantine status --json' 2>/dev/null || true)
after=$(shasum -a 256 "$cf" | cut -d' ' -f1)
[ "$before" = "$after" ] && ok "quarantine status has no chain side effect" \
    || bad "status side effect" "$before != $after"
apply=$(run_jash "$H" "$R" 0 'sigil quarantine apply --json' 2>/dev/null || true)
case "$apply" in
    *'"applied":1'*'"active_reset":false'*'"tail_salvaged":true'*)
        ok "quarantine apply salvages valid prefix" ;;
    *) bad "quarantine apply" "$apply" ;;
esac
qchain=''
for p in "$H"/.jash/quarantine/corrupted_newline_fragment-*"$(basename "$cf")"; do
    case "$p" in *.manifest.json) continue ;; esac
    [ -f "$p" ] && { qchain=$p; break; }
done
[ -n "$qchain" ] && cmp -s "$T/fragment-suffix.log" "$qchain" \
    && ok "quarantine preserves invalid suffix exactly" \
    || bad "quarantined bytes" "quarantined=$qchain"
active_sha=$(shasum -a 256 "$cf" | cut -d' ' -f1)
[ "$active_sha" = "$prefix_sha" ] \
    && ok "active chain preserves valid prefix exactly" \
    || bad "salvaged prefix" "$active_sha != $prefix_sha"
manifest=$(printf '%s\n' "$H"/.jash/quarantine/*.manifest.json | head -1)
mcheck=$(python3 - "$manifest" "$before" <<'PY'
import json, sys
d = json.load(open(sys.argv[1]))
print(d.get("reason"), d.get("original_sha256") == sys.argv[2],
      d.get("entry_count"), d.get("fragment_count"),
      d.get("tail_salvaged"), d.get("ratchet_quarantined") is None)
PY
)
[ "$mcheck" = "corrupted_newline_fragment True $FRAG_ENTRIES 1 True True" ] \
    && ok "manifest records suffix salvage without moving ratchet" \
    || bad "quarantine manifest" "$mcheck"
cmp -s "$H/.jash/ratchet.key" "$T/global-ratchet" \
    && ok "global legacy ratchet remains byte-identical" \
    || bad "global ratchet" "shared file moved or changed"
run_jash "$H" "$R" 1 'true' >/dev/null 2>&1
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
[ "$(is_valid "$out")" = true ] && ok "new scoped chain verifies after apply" \
    || bad "post-quarantine verify" "$out"
status=$(run_jash "$H" "$R" 0 'sigil quarantine status --json' 2>/dev/null || true)
case "$status" in
    *'"active":null'*'.manifest.json'*) ok "quarantine remains listable" ;;
    *) bad "quarantine listing" "$status" ;;
esac

# H: ledger durability precedes ratchet publication. If the atomic ratchet
# rename cannot be prepared, the new entry remains durable while the old
# ratchet remains byte-identical, so verification fails closed instead of
# acknowledging an entry that was never position-witnessed.
setup_case publishfail
run_jash "$H" "$R" 1 'echo first' >/dev/null 2>&1
cf=$(chainfile "$H")
set -- "$H"/.jash/ratchet-*.key
rp=$1
ratchet_before=$(shasum -a 256 "$rp" | cut -d' ' -f1)
entries_before=$(grep -c ' tag=' "$cf" || true)
chmod 0500 "$H/.jash"
run_jash "$H" "$R" 1 'echo second' >/dev/null 2>&1 || true
chmod 0700 "$H/.jash"
ratchet_after=$(shasum -a 256 "$rp" | cut -d' ' -f1)
entries_after=$(grep -c ' tag=' "$cf" || true)
[ "$entries_after" -eq $((entries_before + 1)) ] \
    && ok "ledger entry is durable before ratchet publication" \
    || bad "ledger-first publication" "$entries_before -> $entries_after"
[ "$ratchet_before" = "$ratchet_after" ] \
    && ok "failed atomic ratchet publish preserves prior witness" \
    || bad "ratchet failure mutated witness" "$ratchet_before != $ratchet_after"
publish_verdict=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
case "$publish_verdict" in
    *'"valid":false'*entries_forged*) ok "ratchet publish failure verifies fail-closed" ;;
    *) bad "ratchet publish failure verdict" "$publish_verdict" ;;
esac

# I: concurrent shells serialize the whole ledger + witness transaction.
setup_case concurrent
for i in $(seq 1 128); do
    run_jash "$H" "$R" 1 "echo concurrent-$i >/dev/null" >/dev/null 2>&1 &
done
wait
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
[ "$(is_valid "$out")" = true ] && ok "concurrent appends keep ratchet position exact" \
    || bad "concurrent ratchet publication" "$out"

# J: verification reloads the witness in the same locked snapshot as the
# ledger. A long-lived shell must not accuse entries appended by a peer after
# module initialization.
setup_case staleverify
run_jash "$H" "$R" 1 'echo seed >/dev/null' >/dev/null 2>&1
verify_out="$T/staleverify/output"
ready="$T/staleverify/ready"
run_jash "$H" "$R" 1 "touch '$ready'; sleep 2; sigil verify --json" \
    > "$verify_out" 2>&1 &
verify_pid=$!
for _ in $(seq 1 100); do
    [ -e "$ready" ] && break
    sleep 0.01
done
if [ ! -e "$ready" ]; then
    bad "stale verifier setup" "long-lived shell did not become ready"
else
    run_jash "$H" "$R" 1 'echo peer-append >/dev/null' >/dev/null 2>&1
fi
wait "$verify_pid"
out=$(grep '{"valid":' "$verify_out" | tail -1)
[ "$(is_valid "$out")" = true ] && ok "verify snapshots current ratchet with chain" \
    || bad "stale verifier witness" "$out"

# K: newline-free entries from the old wire format remain byte-compatible.
setup_case intact
run_jash "$H" "$R" 1 'printf legacy-safe >/dev/null' >/dev/null 2>&1
cf=$(chainfile "$H")
clean_before=$(shasum -a 256 "$cf" | cut -d' ' -f1)
run_jash "$H" "$R" 1 'sigil quarantine status --json' >/dev/null 2>&1
clean_after=$(shasum -a 256 "$cf" | cut -d' ' -f1)
[ "$clean_before" = "$clean_after" ] && ok "status is read-only with sigil logging active" \
    || bad "clean status side effect" "$clean_before != $clean_after"
# Read-only is one-command scoped. A later command in the SAME session must
# be recorded; otherwise inspection silently disables attestation.
# Use the interactive input path: script files are one outer eval and receive
# a single post_cmd for the whole script, while the REPL is the session model
# whose per-command attestation this invariant protects.
printf 'sigil quarantine status --json\necho after-status >/dev/null\nexit\n' |
    (cd "$R" && HOME="$H" JASH_SIGIL=1 JASH_NUDGE=0 JASH_BRIEF=0 \
        JASH_LOOPGUARD=off "$JASH") >/dev/null 2>&1
grep -q 'cmd=echo after-status' "$cf" \
    && ok "status suppresses only itself; later command is attested" \
    || bad "status leaked read-only state" "later command missing from chain"
out=$(run_jash "$H" "$R" 0 'sigil verify --json' 2>/dev/null || true)
[ "$(is_valid "$out")" = true ] && ok "intact newline-free entries still verify" \
    || bad "newline-free compatibility" "$out"

printf '\n=== JINV-099/100/101: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
