#!/usr/bin/env bash
# JINV-102: OCI export binds the caller manifest and the healthy scoped chain.

set -u
cd "$(dirname "$0")/.." || exit 2
JASH="$PWD/build/jash"
JEF="$PWD/build/jef_verify"
MOD="$PWD/modules/sigil/sigil.modjash"
[ -x "$JASH" ] || { echo "SKIP: $JASH missing"; exit 0; }
[ -x "$JEF" ] || { echo "SKIP: $JEF missing"; exit 0; }
[ -f "$MOD" ] || { echo "SKIP: $MOD missing"; exit 0; }
command -v python3 >/dev/null || { echo "SKIP: python3 missing"; exit 0; }

T=$(mktemp -d) || exit 2
trap 'rm -rf "$T"' EXIT
pass=0 fail=0
ok() { pass=$((pass + 1)); printf 'ok    %s\n' "$1"; }
bad() { fail=$((fail + 1)); printf 'FAIL  %s\n        %s\n' "$1" "$2"; }

H="$T/home"; R="$T/repo"; OUT="$T/export"; MAN="$T/manifest.json"
mkdir -p "$H/.jash/modules" "$R"
git -C "$R" init -q || exit 2
bash "$PWD/tests/setup_sigil_modules.sh" "$H"
printf '%s\n' '{"bundle_id":"sha256:test","schema_version":1}' > "$MAN"

run_jash() {
    local level=$1 command=$2
    (cd "$R" && HOME="$H" JASH_SIGIL="$level" JASH_NUDGE=0 \
        JASH_BRIEF=0 JASH_LOOPGUARD=off "$JASH" -c "$command")
}

run_jash 1 true >/dev/null 2>&1
if run_jash 0 "sigil export $OUT --manifest $MAN" >/dev/null 2>&1; then
    ok "healthy chain exports with a caller manifest"
else
    bad "healthy export" "sigil export returned non-zero"
fi

if [ -f "$OUT/oci-layout" ] && [ -f "$OUT/index.json" ]; then
    ok "export writes an OCI image layout"
else
    bad "OCI layout" "oci-layout or index.json missing"
fi

inspection=$(python3 - "$OUT" "$MAN" "$T/export.dsse" "$T/keyid" <<'PY'
import base64, hashlib, json, pathlib, sys

root, caller, dsse_out, keyid_out = map(pathlib.Path, sys.argv[1:])
index = json.loads((root / "index.json").read_bytes())
mdesc = index["manifests"][0]
mblob = root / "blobs" / "sha256" / mdesc["digest"].split(":", 1)[1]
oci = json.loads(mblob.read_bytes())
layers = oci["layers"]
dsse = next(x for x in layers
            if x["mediaType"] == "application/vnd.dsse.envelope.v1+json")
env_path = root / "blobs" / "sha256" / dsse["digest"].split(":", 1)[1]
env = json.loads(env_path.read_bytes())
payload = json.loads(base64.b64decode(env["payload"], validate=True))
dsse_out.write_bytes(env_path.read_bytes())
keyid_out.write_text(env["signatures"][0]["keyid"])
caller_bytes = caller.read_bytes()
subject = oci["subject"]
good = (
    env["payloadType"] == "application/vnd.jash.sigil.export.v1+json"
    and payload["bearcode_manifest_sha256"] == hashlib.sha256(caller_bytes).hexdigest()
    and payload["bearcode_manifest_media_type"]
        == "application/vnd.bearcode.network-proof-manifest.v1+json"
    and payload["manifest_size"] == len(caller_bytes)
    and len(payload["chain_sha256"]) == 64
    and len(payload["chain_tip"]) == 64
    and payload["chain_entries"] == 1
    and subject["digest"] == "sha256:" + payload["bearcode_manifest_sha256"]
    and subject["size"] == len(caller_bytes)
)
print("bound" if good else json.dumps({"payload": payload, "subject": subject}))
PY
)
if [ "$inspection" = bound ]; then
    ok "DSSE binds exact manifest bytes, media type, and chain snapshot"
else
    bad "manifest binding" "$inspection"
fi

identity=$(run_jash 0 'sigil proof identity --json' 2>/dev/null || true)
identity_key=$(printf '%s' "$identity" | python3 -c \
    'import json,sys; print(json.load(sys.stdin).get("key_fingerprint", ""))' 2>/dev/null)
if [ "$identity_key" = "$(cat "$T/keyid")" ]; then
    ok "identity probe matches the DSSE signer key"
else
    bad "identity binding" "identity=$identity_key dsse=$(cat "$T/keyid")"
fi

if "$JEF" proof "$T/export.dsse" >/dev/null 2>&1; then
    ok "libjef verifies the DSSE offline"
else
    bad "offline DSSE verification" "jef_verify rejected the exported envelope"
fi

printf 'tamper\n' >> "$MAN"
if python3 - "$MAN" "$T/export.dsse" <<'PY'
import base64, hashlib, json, pathlib, sys
manifest = pathlib.Path(sys.argv[1]).read_bytes()
envelope = json.loads(pathlib.Path(sys.argv[2]).read_bytes())
payload = json.loads(base64.b64decode(envelope["payload"], validate=True))
raise SystemExit(payload["bearcode_manifest_sha256"] == hashlib.sha256(manifest).hexdigest())
PY
then
    ok "changing one manifest byte breaks the signed digest comparison"
else
    bad "manifest tamper" "changed bytes still matched the signed digest"
fi

HELP_HOME="$T/help-home"; mkdir -p "$HELP_HOME/.jash/modules" "$T/help-repo"
git -C "$T/help-repo" init -q || exit 2
cp "$MOD" "$HELP_HOME/.jash/modules/"
help=$(cd "$T/help-repo" && HOME="$HELP_HOME" JASH_SIGIL=0 JASH_NUDGE=0 \
    JASH_BRIEF=0 "$JASH" -c 'sigil export --help' 2>&1)
case "$help" in
    usage:*) ok "--help prints usage without requiring a chain" ;;
    *) bad "export help" "$help" ;;
esac
[ ! -e "$T/help-repo/--help" ] && [ ! -d "$HELP_HOME/.jash/oci-export" ] \
    && [ ! -d "$HELP_HOME/.jash/keys" ] \
    && ok "--help creates no export or signing key" \
    || bad "help side effects" "help created an output or key"

MISSING="$T/missing-output"
if run_jash 0 "sigil export $MISSING --manifest $T/no-such.json" >/dev/null 2>&1; then
    bad "missing manifest refusal" "export succeeded"
elif [ -e "$MISSING" ]; then
    bad "missing manifest side effect" "destination was created"
else
    ok "missing manifest is refused before creating output"
fi

BAD="$T/refused"
shopt -s nullglob
chains=("$H"/.jash/chain-*.log)
if [ "${#chains[@]}" -ne 1 ]; then
    bad "corrupt export fixture" "expected exactly one repository chain"
else
    printf 'newline fragment\n' >> "${chains[0]}"
    if run_jash 0 "sigil export $BAD --manifest $MAN" >/dev/null 2>&1; then
        bad "corrupt export refusal" "export succeeded"
    elif [ -e "$BAD" ]; then
        bad "corrupt export side effect" "destination was created"
    else
        ok "known-corrupt chain is refused before creating output"
    fi
fi

printf '\n=== JINV-102 sigil export manifest: %d ok / %d fail ===\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
