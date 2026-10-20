#!/usr/bin/env bash
# Regression coverage for confirmed findings from security_audit_2026-08-21.md.
set -u

PROJECT_ROOT="$(cd "$(dirname "$0")/.." && pwd)"
JASH_CANDIDATES=("$PROJECT_ROOT/build/jash-release" "$PROJECT_ROOT/build/jash"
                 "$PROJECT_ROOT/build-speed/jash-release")
# An explicit binary must be tested as supplied, never silently substituted.
if [[ ${JASH+x} ]]; then JASH_CANDIDATES=("$JASH"); fi

H="$(mktemp -d /tmp/jash_security_audit.XXXXXX)"
trap 'chmod -R u+w "$H" 2>/dev/null || true; rm -rf "$H"' EXIT
mkdir -p "$H/.jash/patterns"
printf '%s\n' '- pattern: safe' > "$H/.jash/patterns/general.yaml"

pass=0; fail=0
ok(){ pass=$((pass+1)); echo "  PASS $1"; }
no(){ fail=$((fail+1)); echo "  FAIL $1"; }
RUN(){ env HOME="$H" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 "$JASH" "$@"; }

runnable=0
for candidate in "${JASH_CANDIDATES[@]}"; do
    JASH="$candidate"
    if RUN -c 'exit 0' >/dev/null 2>&1; then runnable=1; break; fi
done
if [[ "$runnable" != 1 ]]; then
    echo "FAIL: no runnable jash binary among the selected candidates" >&2
    exit 1
fi

# A schema name is a schema directory name, not a path expression.
change_work="$H/change-proj"
mkdir -p "$change_work/.changes/schemas" "$change_work/outside"
cat > "$change_work/outside/schema.yaml" <<'YAML'
name: outside
version: 1
artifacts:
  - id: findings
    generates: findings.md
    requires: []
YAML
(
    cd "$change_work" &&
    RUN -c 'change propose good --schema ../../outside' >/dev/null 2>&1
) || true
[[ ! -e "$change_work/.changes/good" ]] && ok "change rejects schema path traversal" \
    || no "change schema path traversal"

# sigil push validates the registry reference and invokes oras without a shell.
if command -v cc >/dev/null 2>&1; then
    fakebin="$H/fake-oras-bin"
    mkdir -p "$fakebin"
    cat > "$fakebin/oras" <<'SH'
#!/bin/sh
printf '%s\n' "$@" > "$FAKE_ORAS_ARGS"
SH
    chmod +x "$fakebin/oras"
    # Compile the source under review without replacing the canonical module.
    # build_module.sh resolves this symlink for the source's private includes,
    # and installs the resulting private bundle under this fixture's HOME.
    module_source="$H/module-src/sigil"
    mkdir -p "$module_source" || exit 1
    ln -s "$PROJECT_ROOT/modules/sigil/sigil_module.c" \
        "$module_source/sigil_module.c" || exit 1
    cp "$PROJECT_ROOT/modules/sigil/manifest.toml" "$module_source/manifest.toml" || exit 1
    if ! env HOME="$H" PATH="/usr/bin:/bin" /bin/bash \
        "$PROJECT_ROOT/modules/build_module.sh" "$module_source" \
        >"$H/sigil-module-build.log" 2>&1; then
        no "sigil push private module build"
        tail -20 "$H/sigil-module-build.log" >&2
        exit 1
    fi
    if ! cmp -s "$module_source/sigil.modjash" "$H/.jash/modules/sigil.modjash"; then
        no "sigil push private module installation"
        exit 1
    fi
    marker="$H/sigil-injected"
    oras_args="$H/oras-args"
    out="$(env HOME="$H" PATH="$fakebin:/usr/bin:/bin" FAKE_ORAS_ARGS="$oras_args" \
        JASH_MODULE_DEV_ROOT= \
        JASH_SIGIL=1 JASH_ADOPT=0 JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 \
        JASH_NUDGE=0 "$JASH" -c 'sigil push "example.com/repo;touch $HOME/sigil-injected; #"' 2>&1 || true)"
    if [[ ! -e "$marker" && ! -e "$oras_args" && "$out" == *"invalid registry reference"* ]]; then
        ok "sigil push rejects shell metacharacters before oras"
    else
        no "sigil push command injection"
    fi
else
    echo "  SKIP sigil push regression (no C compiler)"
fi

# cloud_bridge passes prompt JSON and API headers as argv, never to a shell.
if command -v cc >/dev/null 2>&1; then
    fakebin="$H/fake-curl-bin"
    mkdir -p "$fakebin"
    cat > "$fakebin/curl" <<'SH'
#!/bin/sh
printf '%s\n' '{"choices":[{"message":{"content":"ok"}}]}'
SH
    chmod +x "$fakebin/curl"
    cc -std=c99 -D_POSIX_C_SOURCE=200809L \
        -I"$PROJECT_ROOT/modules/slm-mesh" \
        "$PROJECT_ROOT/tests/test_cloud_bridge_security.c" \
        "$PROJECT_ROOT/modules/slm-mesh/cloud_bridge.c" \
        -o "$H/test-cloud-bridge"
    marker="$H/cloud-injected"
    if PATH="$fakebin:/usr/bin:/bin" CLOUD_MARKER="$marker" "$H/test-cloud-bridge" \
        >/dev/null 2>&1 && [[ ! -e "$marker" ]]; then
        ok "cloud bridge does not execute prompt contents"
    else
        no "cloud bridge prompt command injection"
    fi
else
    echo "  SKIP cloud bridge regression (no C compiler)"
fi

# A failed tag allocation must not leave a half-updated brain fact.
if command -v cc >/dev/null 2>&1; then
    cc -std=c99 -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE \
        -I"$PROJECT_ROOT/c" -Wl,--wrap=malloc \
        "$PROJECT_ROOT/tests/test_brain_security.c" \
        "$PROJECT_ROOT/c/core/jash_brain.c" \
        -o "$H/test-brain-security"
    if "$H/test-brain-security" >/dev/null 2>&1; then
        ok "brain store remains atomic on partial OOM"
    else
        no "brain partial-allocation state corruption"
    fi
else
    echo "  SKIP brain regression (no C compiler)"
fi

# Query text is data, not a shell fragment.
marker="$H/learn-injected"
payload="x\"; touch $marker; #"
RUN --exec-json "{\"argv\":[\"learn\",\"recall\",\"$payload\"]}" \
    >/dev/null 2>&1 || true
[[ ! -e "$marker" ]] && ok "learn recall does not execute query text" \
    || no "learn recall command injection"

# Repository names are path components, not arbitrary paths.
RUN --exec-json '{"argv":["learn","payload","--repo","../../escaped"]}' \
    >/dev/null 2>&1 || true
[[ ! -e "$H/escaped.yaml" ]] && ok "learn rejects repository path traversal" \
    || no "learn repository path traversal"

# HOME may contain shell metacharacters and must never become executable syntax.
marker="$H/predict-injected"
bad_home="$H; touch $marker; #"
env HOME="$bad_home" PATH="/usr/bin:/bin" JASH_SIGIL=0 JASH_ADOPT=0 \
    JASH_MEMO=0 JASH_LOOPGUARD=off JASH_BRIEF=0 JASH_NUDGE=0 \
    "$JASH" -c 'predict true' >/dev/null 2>&1 || true
[[ ! -e "$marker" ]] && ok "predict does not execute HOME contents" \
    || no "predict HOME command injection"

# The fuzzy matcher supports at most 64-byte Levenshtein operands. Longer
# patterns must be rejected safely instead of indexing past substr[65].
long_pattern="$(printf '%0128d' 0 | tr 0 a)"
printf '%s\n' "$long_pattern" > "$H/long-line"
RUN --exec-json "{\"argv\":[\"grep\",\"--fuzzy\",\"$long_pattern\",\"$H/long-line\"]}" \
    >/dev/null 2>&1
rc=$?
[[ "$rc" -eq 1 ]] && ok "grep --fuzzy rejects oversized pattern safely" \
    || no "grep --fuzzy oversized pattern returned rc=$rc"

# Equivalent spellings of protected roots must remain protected. No -r keeps
# the pre-fix reproduction non-destructive while distinguishing the guard.
out="$(RUN -c 'rm --dry-run /etc/../etc' 2>&1 || true)"
case "$out" in
    *BLOCKED*) ok "rm canonicalizes critical paths before its safety check" ;;
    *) no "rm critical-path guard bypass via .." ;;
esac

# rename(2) replaces a destination atomically. Removing it first loses data if
# the subsequent rename fails (here source parent is intentionally read-only).
mkdir "$H/src-parent" "$H/dst-parent"
printf old > "$H/dst-parent/file"
printf new > "$H/src-parent/file"
chmod 0555 "$H/src-parent"
RUN --exec-json "{\"argv\":[\"mv\",\"$H/src-parent/file\",\"$H/dst-parent/file\"]}" \
    >/dev/null 2>&1 || true
if [[ -f "$H/dst-parent/file" && "$(cat "$H/dst-parent/file")" == old ]]; then
    ok "mv failure preserves the existing destination"
else
    no "mv removed destination before a failed rename"
fi

echo "security audit regressions: PASS=$pass FAIL=$fail"
[[ "$fail" -eq 0 ]]
